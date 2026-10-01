#!/usr/bin/env python3
"""The mutation proof for tools\\flat_camera_vr_test: the rig fails when a rule of the VR camera injection mode is flipped.

The rig (flat_camera_vr_test.cpp) pins the rules of src\\d3d11\\flat_camera_vr.h (the pure half), of the admission table it reuses
(flat_camera_phase.h) and of the detour (flat_camera_inject.cpp, by source pins). A rig that passes proves little until it is seen to
FAIL on a source that breaks the rule it pins. This tool does that: for each mutation below it copies the rig's include closure and
the detour source into a temp directory OUTSIDE the repo, applies one textual edit (or a few that belong together) to one file,
compiles the rig there, runs it with that copy as its root, and requires it to fail on a check that belongs to the rule (a label
starting with one of the prefixes the mutation names, followed by a letter: "R1" matches R1c and not R10a). Nothing is written
inside the repo; the temp directory is removed at the end.

  python tools\\flat_camera_vr_test\\mutants.py --self-test          text only: every anchor is found exactly once in its file as it
                                                                    is now, every label named is in the rig, build.bat compiles the
                                                                    rig the way this tool does and runs this self-test
  python tools\\flat_camera_vr_test\\mutants.py --run [--only a,b] [--jobs N] [--keep]
  python tools\\flat_camera_vr_test\\mutants.py --list
  python tools\\flat_camera_vr_test\\mutants.py --run --dry-run      the plan; writes nothing, starts nothing

--run needs the MSVC toolchain (cl.exe on PATH, or Visual Studio found with vswhere as build.bat does) and takes about a minute.
--self-test runs in build.bat's rig and is what keeps an edit of the sources from silently orphaning a mutation: if an anchor
stops matching, the build fails and this file says which.
"""
import argparse
import concurrent.futures
import contextlib
import io
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
RIG = HERE / "flat_camera_vr_test.cpp"
BUILD_BAT = ROOT / "build.bat"
RIG_LABEL = ":rig_flat_camera_vr_test"
SRC = ROOT / "src" / "d3d11"
# The files a mutation may edit, by the key it names.
FILES = {
    "vr": SRC / "flat_camera_vr.h",
    "phase": SRC / "flat_camera_phase.h",
    "cpp": SRC / "flat_camera_inject.cpp",
    "hdr": SRC / "flat_camera_inject.h",
}
# Read by the rig's source pins without being included.
EXTRA_COPIES = [SRC / "flat_camera_inject.cpp", SRC / "vr_camera_census_core.h"]

# How build.bat compiles the rig; --self-test checks that the label still says the same.
CL_FLAGS = ["/nologo", "/O2", "/MT", "/std:c++17", "/EHsc", "/W4", "/DWIN32_LEAN_AND_MEAN", "/DNOMINMAX", "/D_CRT_SECURE_NO_WARNINGS"]
LINK_LIBS = ["kernel32.lib"]
RUN_TIMEOUT = 120.0


class Mutant:
    def __init__(self, name, target, caught, edits, why):
        self.name = name
        self.target = target                                                     # a key of FILES
        self.caught = tuple([caught] if isinstance(caught, str) else caught)     # label prefixes that count as caught by its rule
        self.edits = list(edits)                                                 # (old, new) pairs, applied in order
        self.why = why


def M(name, target, caught, edits, why):
    return Mutant(name, target, caught, edits, why)


def drop(old):
    return [(old, "")]


# ---- anchors: the sources, verbatim (the self-test finds each exactly once) -------------------------------------------------------
TOL = "kFlatCameraVrScreenTolerance = 0.04;"
RATIO = "kFlatCameraVrFirstPersonNearRatio = 1.5;"
TOL_TEST = "<= kFlatCameraVrScreenTolerance * anchor))"
FABS = "std::fabs(static_cast<double>(aspect) - anchor)"
NEAR_GE = ">= kFlatCameraVrFirstPersonNearRatio * static_cast<double>(sceneNear))"
ANCHOR_MIN = "(!(sceneNear_ > 0.0f) || nearZ < sceneNear_))"
ANCHOR_AUX = "if (role != FlatCameraVrRole::Auxiliary && std::isfinite(nearZ) && nearZ > 0.0f &&"
ROLE_LINE = "            if (in.role == FlatCameraVrRole::Auxiliary) return FlatCameraVrAdmit::RoleExcluded;\n"
WINDOW_LINE = "            if (!in.windowOpen) return FlatCameraVrAdmit::AfterTrigger;\n"
STALE_CASE = "case FlatCameraAdmit::GateClosed: return FlatCameraVrAdmit::Stale;"
UPSTREAM = "base.upstreamOwns = in.mode == FlatCameraVrMode::Inject;"
OBSERVEONLY = "base.observeOnly = in.mode == FlatCameraVrMode::Observe;"
NOTE_HEAD = "        ++c_.calls;\n        if (role != FlatCameraVrRole::Auxiliary && std::isfinite(fov) && fov > 0.0f &&"
KIND_COUNT = "                if (landed) ++c_.injectedKind[flatCameraVrKindIndex(readable, kind)];\n"
KIND_INDEX = "!readable ? 7u : kind <= 5u ? kind : 6u"
CALLS_LINE = "        ++c_.calls;\n"
STALE_COUNT = "case FlatCameraVrAdmit::Stale: ++c_.stale; break;"
SCENE_COUNT = "else { if (landed) ++c_.sceneInjected; else ++c_.sceneRefused; }"
ELIGIBLE_TAIL = ("        case FlatCameraVrAdmit::Stale:\n        case FlatCameraVrAdmit::Unsupported:\n        case FlatCameraVrAdmit::OtherKind:\n"
                 "            return true;\n        case FlatCameraVrAdmit::Inject:\n        case FlatCameraVrAdmit::OffThread:\n        case FlatCameraVrAdmit::Unreadable:\n"
                 "            return false;\n")
ELIGIBLE_OBSERVED = ("        case FlatCameraVrAdmit::NotActive:\n        case FlatCameraVrAdmit::Observed:\n        case FlatCameraVrAdmit::Stale:\n")
BOUND_Y = "*outY = entryY - phaseY / static_cast<float>(renderH);"
BOUND_X = "*outX = entryX + phaseX / static_cast<float>(renderW);"
PHASE_FINITE = "renderW != 0 && renderH != 0 && std::isfinite(phaseX) && std::isfinite(phaseY) && "
LAPSE = "if (stepAtMs == 0 || (nowMs > stepAtMs && nowMs - stepAtMs > FlatCameraGate::kExpiryMs)) return FlatCameraGateVerdict::Expired;"
CENSUS_RULE = "    if (!(bits & kFlatCameraVrBitDriven) || (bits & kFlatCameraVrBitInject)) return bits;"
QUIET_RULE = "    return (bits & (kFlatCameraVrBitInject | kFlatCameraVrBitObserve)) == 0u && injectedSetEmpty;"
NOT_LIVE = "    if (!live) return kFlatCameraVrBitDriven;\n"
PLAN_FLUSH = "        p.flush = flatCameraVrFlushDecision(set, in.camera, p.admit);"
PLAN_INJECT = "        p.inject = p.admit == FlatCameraVrAdmit::Inject;"
PLAN_PHASE = "        a.phaseNonzero = phaseNonzero_;"
PLAN_WINDOW = "        a.windowOpen = windowOpen();"
PLAN_EXCLUDED = ("        if (p.admit == FlatCameraVrAdmit::RoleExcluded)\n"
                 "            excluded_.note(frustum.aspect, frustum.fov, frustum.nearZ, frustum.farZ, in.callerRva);\n")
FINISH_NOTE = "        tally_.note(plan.admit, plan.role, in.readable, in.kind, landed, plan.fov);"
BEGIN_TALLY = "        tally_.reset();\n"
BEGIN_ROLES = "        roles_.beginFrame();   // the field-of-view anchor is the frame's own (the near anchor is kept)\n"
BEGIN_WINDOW = "        windowOpen_.store(true, std::memory_order_release);\n"
BEGIN_FRAME = BEGIN_TALLY + BEGIN_ROLES + BEGIN_WINDOW
CLASSIFY = "p.role = roles_.classify(frustum.aspect, frustum.nearZ, screenAspect_, frustum.fov);"
# the field-of-view test (the stage 2 experiment build)
FOV_RATIO = "kFlatCameraVrFirstPersonFovRatio = 0.92;"
FOV_LE = "static_cast<double>(fov) <= kFlatCameraVrFirstPersonFovRatio * static_cast<double>(sceneFov))"
FOV_GUARD = "if (std::isfinite(fov) && fov > 0.0f && std::isfinite(sceneFov) && sceneFov > 0.0f &&"
FOV_RAISE = "if (role == FlatCameraVrRole::Scene && std::isfinite(fov) && fov > 0.0f && fov > sceneFov_)"
FOV_BEGIN_FN = "void beginFrame() { sceneFov_ = 0.0f; }"
FOV_RESET_FN = "void reset() { sceneNear_ = 0.0f; sceneFov_ = 0.0f; }"
PLAN_FOV = "            p.fov = frustum.fov;\n"
FOV_NOTE_COND = ("        if (role != FlatCameraVrRole::Auxiliary && std::isfinite(fov) && fov > 0.0f &&\n"
                 "            (admit == FlatCameraVrAdmit::Inject || admit == FlatCameraVrAdmit::Warming || admit == FlatCameraVrAdmit::AfterTrigger)) {\n")
FOV_RANGE = ("            if (!(c_.fovNarrowest <= fov)) c_.fovNarrowest = fov;   // a NaN is not <= anything: the first reading sets both\n"
             "            if (!(c_.fovWidest >= fov)) c_.fovWidest = fov;\n")
KIND3 = "    if (in.kind != 3) return FlatCameraAdmit::OtherKind;\n"
EYES = "    if (in.kind == 4 || in.kind == 5) return FlatCameraAdmit::Unsupported;\n"
OBSERVE_SWITCH = "    if (in.observeOnly) return FlatCameraAdmit::Observed;\n"
GATE_LINE = "    if (in.gate != FlatCameraGateVerdict::Admit) return FlatCameraAdmit::GateClosed;\n"
LIMIT = "constexpr uint64_t kFlatCameraWriteFailureLimit = 8;"

COMMIT_GUARD = "    if (!inject && !wantPost) return false;\n    float entryX = 0, entryY = 0;\n"
COMMIT_CALL = "vrCommit(r0, ctx, camera, callNo, plan.inject, wantPost)"
REPORT_GUARD = "    if (report && camera) {\n"
WILL_INJECT = "call.willInject = plan.inject;"
FINISH_CALL = "    g_vr.finish(plan, in, landed);   // the call's ONE outcome\n"
REPORT_BLOCK = ("    if (g_refreshTls.vrReport) {\n        g_refreshTls.vrReport = 0;\n"
                "        const FlatCameraObserver* observer = g_observer.load(std::memory_order_acquire);\n"
                "        if (observer && observer->post) observer->post(camera, g_refreshTls.ctx);\n    }\n")
RESTORE_END = "        if (!(restoredX && restoredY && restoredFlags)) g_vr.tally().noteWriteFailure();\n    }\n"
BRANCH = ("    const uint32_t vrBits = g_vrBits.load(std::memory_order_acquire);\n    if (vrBits != 0) {\n"
          "        refreshPreVr(r0, ctx, p2, camera, callNo, gate, vrBits);\n        return;\n    }\n")
OBSERVE_LINE = "    const bool observe = g_inject.observeOnly.load(std::memory_order_acquire);\n"
TLS_CLEAR = "    g_refreshTls.observe = 0;\n    g_refreshTls.vr = 0;\n"
FLAT_JITY = "    const float jitY = entryY - jy / static_cast<float>(rh);"
FLAT_OBSERVE = ("    if (observe) {\n        observeCall(r0, ctx, p2, camera, callNo, readable, kind, callerRva, gate);\n        return;\n    }\n")
CENSUS_STEP = "    g_vrBits.store(flatCameraVrBitsAfterCensusFrame(g_vrBits.load(std::memory_order_acquire)), std::memory_order_release);\n"
PUBLISH = "    g_vrBits.store(flatCameraVrBitsForFrame(frame.inject, observe, true), std::memory_order_release);\n"
LIVE_COMMENT = "    // A hook that is not live (never installed, failed, stood down) honours neither mode: inject is not honoured.\n"
LIVE_RETURN = "    if (!live) return false;\n"
REPORT_LINE = "    const bool report = flatCameraVrObserves(bits);\n"
NOINJECT_SET = "    if (inject) g_inject.injected.noteInjected(camera);\n    return inject;"
EFFECTIVE = "in.gate = flatCameraVrEffectiveGate(in.mode, gate, g_vr.stepAtMs(), GetTickCount64());"
WANTS = "    if (g_vr.wantsFrustum(in)) frustum = readFrustum(camera);\n"
STAND = "        standDown(why);\n    }\n    // This step is"
QUIET_FN = "bool flatCameraVrQuiet() { return flatCameraVrQuietFor(g_vrBits.load(std::memory_order_acquire), g_inject.injected.empty()); }"
PAUSE_CALL = "    flatCameraInjectPause(flatCameraVrQuiet());\n"
OBSERVE_REG = "    const bool observe = frame.observe || g_observer.load(std::memory_order_acquire) != nullptr;\n"
QUIET_TEXT = "a hook that never installed is quiet."

MUTANTS = [
    # ---- R1: the role ----------------------------------------------------------------------------------------------------------
    M("tolerance-5-percent", "vr", "R1", [(TOL, "kFlatCameraVrScreenTolerance = 0.05;")], "an aspect 4.1% off is a screen view"),
    M("tolerance-3-percent", "vr", "R1", [(TOL, "kFlatCameraVrScreenTolerance = 0.03;")], "an aspect 3.9% off is excluded"),
    M("tolerance-absolute", "vr", "R1", [(TOL_TEST, "<= kFlatCameraVrScreenTolerance))")], "the tolerance is 0.04 in aspect units, not 4%"),
    M("tolerance-one-sided", "vr", "R1", [(FABS, "(static_cast<double>(aspect) - anchor)")], "an aspect far below the screen's is a screen view"),
    M("near-ratio-1.2", "vr", "R1", [(RATIO, "kFlatCameraVrFirstPersonNearRatio = 1.2;")], "a near plane 1.3x the scene's is the weapon"),
    M("near-ratio-1.7", "vr", "R1", [(RATIO, "kFlatCameraVrFirstPersonNearRatio = 1.7;")], "a near plane exactly 1.5x the scene's is not the weapon"),
    M("near-ratio-strict", "vr", "R1", [(NEAR_GE, "> kFlatCameraVrFirstPersonNearRatio * static_cast<double>(sceneNear))")], "exactly 1.5x is a scene call"),
    M("anchor-not-a-minimum", "vr", "R1", [(ANCHOR_MIN, "(!(sceneNear_ > 0.0f) || nearZ > sceneNear_))")], "the anchor follows the largest near"),
    M("anchor-moved-by-auxiliary", "vr", "R1", [(ANCHOR_AUX, "if (std::isfinite(nearZ) && nearZ > 0.0f &&")], "an auxiliary camera's near plane moves the anchor"),
    M("role-from-fov", "vr", ["R1", "R5"], [(CLASSIFY, "p.role = roles_.classify(frustum.aspect, frustum.fov, screenAspect_, frustum.fov);")], "the planner hands the role test the fov as the near plane"),
    # ---- R1, R12: the field-of-view test (the stage 2 experiment build) ----------------------------------------------------------------
    M("fov-ratio-0.85", "vr", "R1", [(FOV_RATIO, "kFlatCameraVrFirstPersonFovRatio = 0.85;")], "a field of view 0.90 of the scene's is not the weapon"),
    M("fov-ratio-0.97", "vr", "R1", [(FOV_RATIO, "kFlatCameraVrFirstPersonFovRatio = 0.97;")], "a field of view 0.95 of the scene's is the weapon"),
    M("fov-edge-strict", "vr", "R1", [(FOV_LE, FOV_LE.replace("<=", "<"))], "exactly 0.92 of the scene's field of view is a scene call"),
    M("fov-wider-is-the-weapon", "vr", ["R1", "R12"], [(FOV_LE, FOV_LE.replace("<=", ">="))], "the wider camera is the weapon, not the tighter one"),
    M("fov-rule-ignores-an-infinite-anchor", "vr", "R1", [(FOV_GUARD, "if (std::isfinite(fov) && fov > 0.0f && sceneFov > 0.0f &&")], "an infinite scene field of view makes every camera the weapon"),
    M("fov-rule-ignores-a-bad-fov", "vr", "R1", [(FOV_GUARD, "if (std::isfinite(sceneFov) && sceneFov > 0.0f &&")], "a negative field of view is the weapon"),
    M("fov-anchor-not-a-maximum", "vr", ["R1", "R12"], [(FOV_RAISE, FOV_RAISE.replace("fov > sceneFov_", "fov < sceneFov_"))], "the anchor follows the narrowest scene field of view"),
    M("fov-anchor-moved-by-auxiliary", "vr", "R1", [(FOV_RAISE, FOV_RAISE.replace("role == FlatCameraVrRole::Scene && ", ""))], "an auxiliary camera's field of view moves the anchor"),
    M("fov-anchor-kept-by-the-tracker-frame-step", "vr", ["R1", "R12"], [(FOV_BEGIN_FN, "void beginFrame() {}")], "the tracker carries last frame's field-of-view anchor"),
    M("fov-anchor-kept-by-the-core-frame-step", "vr", "R12", drop(BEGIN_ROLES), "the core's frame step never clears the field-of-view anchor"),
    M("fov-anchor-kept-by-reset", "vr", "R1", [(FOV_RESET_FN, "void reset() { sceneNear_ = 0.0f; }")], "reset leaves the field-of-view anchor"),
    M("planner-gives-the-role-no-fov", "vr", "R12", [(CLASSIFY, "p.role = roles_.classify(frustum.aspect, frustum.nearZ, screenAspect_);")], "the role test is never given the call's field of view"),
    M("plan-carries-no-fov", "vr", "R12", drop(PLAN_FOV), "the plan does not carry the field of view to the counters"),
    M("outcome-told-no-fov", "vr", "R12", [(FINISH_NOTE, "        tally_.note(plan.admit, plan.role, in.readable, in.kind, landed);")], "the counters are not told the call's field of view"),
    M("fov-range-never-kept", "vr", "R12", drop(FOV_RANGE), "the counters keep no field-of-view range"),
    M("fov-range-swapped", "vr", "R12", [(FOV_RANGE, FOV_RANGE.replace("c_.fovNarrowest <= fov", "c_.fovNarrowest >= fov").replace("c_.fovWidest >= fov", "c_.fovWidest <= fov"))],
      "the narrowest and the widest field of view are swapped"),
    M("fov-range-with-excluded-calls", "vr", "R12", [(FOV_NOTE_COND, "        if (std::isfinite(fov) && fov > 0.0f) {\n")], "an excluded (auxiliary) camera's field of view widens the range"),
    # ---- R2: the admission ----------------------------------------------------------------------------------------------------------
    M("window-rule-gone", "vr", "R2", drop(WINDOW_LINE), "a screen view after the trigger is admitted"),
    M("after-trigger-needs-a-phase", "vr", "R2",
      [("if (!in.windowOpen) return FlatCameraVrAdmit::AfterTrigger;", "if (!in.windowOpen && base == FlatCameraAdmit::Inject) return FlatCameraVrAdmit::AfterTrigger;")],
      "a zero-phase call after the trigger is warming, not after-trigger"),
    M("window-before-role", "vr", "R2", [(ROLE_LINE + WINDOW_LINE, WINDOW_LINE + ROLE_LINE)], "an auxiliary call after the trigger is after-trigger, not excluded"),
    M("auxiliary-not-excluded", "vr", ["R2", "R5"], drop(ROLE_LINE), "an auxiliary kind-3 camera is admitted"),
    M("stale-as-not-active", "vr", "R2", [(STALE_CASE, "case FlatCameraAdmit::GateClosed: return FlatCameraVrAdmit::NotActive;")], "a lapsed window is not-active"),
    M("pass-through-owns", "vr", ["R2", "R3"], [(UPSTREAM, "base.upstreamOwns = true;")], "a pass-through frame injects"),
    M("observe-is-not-observe-only", "vr", "R2", [(OBSERVEONLY, "base.observeOnly = false;")], "an observe frame injects"),
    M("role-defaults-to-scene", "vr", "R2", [("gets the safe answer, Auxiliary.\n    FlatCameraVrRole role = FlatCameraVrRole::Auxiliary;", "gets the safe answer, Auxiliary.\n    FlatCameraVrRole role = FlatCameraVrRole::Scene;")],
      "an input nobody gave a role is a scene camera"),
    # ---- the flat table, the kind test and the eyes ----------------------------------------------------------------------------------
    M("kind-test-greater-than", "phase", ["R2", "R3", "R10"], [(KIND3, "    if (in.kind > 3) return FlatCameraAdmit::OtherKind;\n")], "kinds 0, 1 and 2 are injected"),
    M("kind-5-not-unsupported", "phase", ["R2", "R10"], [(EYES, "    if (in.kind == 4) return FlatCameraAdmit::Unsupported;\n")], "kind 5 (an eye) is not named unsupported"),
    M("eyes-injected", "phase", ["R3", "R2", "R10"], [(EYES, ""), (KIND3, "    if (in.kind != 3 && in.kind != 5) return FlatCameraAdmit::OtherKind;\n")], "kind 5 (an eye) is injected"),
    M("observe-switch-dropped", "phase", ["R2", "R10"], drop(OBSERVE_SWITCH), "the observe-only switch does nothing"),
    M("frame-window-ignored", "phase", ["R2", "R10"], drop(GATE_LINE), "a closed frame window admits a kind-3 call"),
    M("write-failure-limit-9", "phase", "R9", [(LIMIT, "constexpr uint64_t kFlatCameraWriteFailureLimit = 9;")], "nine failed writes stand the hook down, not eight"),
    # ---- R3, R4: the counters --------------------------------------------------------------------------------------------------------
    M("kind-counted-on-every-call", "vr", ["R3", "R4"], [(NOTE_HEAD, NOTE_HEAD.replace("        ++c_.calls;\n", "        ++c_.calls;\n        ++c_.injectedKind[flatCameraVrKindIndex(readable, kind)];\n"))],
      "every call, whatever its kind, is counted as injected by kind"),
    M("kind-counted-when-refused", "vr", ["R3", "R4"], [(KIND_COUNT, "                ++c_.injectedKind[flatCameraVrKindIndex(readable, kind)];\n")],
      "an injection that did not land is counted by kind"),
    M("kind-never-counted", "vr", ["R3", "R4"], drop(KIND_COUNT), "injectedKind is never incremented"),
    M("kind-index-collapsed", "vr", "R3", [(KIND_INDEX, "!readable ? 7u : kind <= 5u ? 3u : 6u")], "every kind from 0 to 5 indexes as 3"),
    M("calls-not-counted", "vr", "R4", drop(CALLS_LINE), "an owner-thread call is not counted in calls"),
    M("calls-counted-twice", "vr", "R4", [(CALLS_LINE, CALLS_LINE + CALLS_LINE)], "an owner-thread call is counted twice"),
    M("stale-counted-as-other-kind", "vr", "R4", [(STALE_COUNT, "case FlatCameraVrAdmit::Stale: ++c_.otherKind; break;")], "a stale call is counted in otherKind"),
    M("refused-counted-as-injected", "vr", "R4", [(SCENE_COUNT, "else { ++c_.sceneInjected; }")], "a scene call whose write failed is counted as injected"),
    M("outcome-told-plan-not-landing", "vr", "R4", [(FINISH_NOTE, "        tally_.note(plan.admit, plan.role, in.readable, in.kind, plan.inject, plan.fov);")],
      "the tally is told the call landed whenever the plan injected it"),
    M("frame-step-keeps-counters", "vr", "R4", [(BEGIN_FRAME, BEGIN_ROLES + BEGIN_WINDOW)], "the frame step does not reset the counters"),
    M("frame-step-keeps-window-closed", "vr", ["R4", "R5"], [(BEGIN_FRAME, BEGIN_TALLY + BEGIN_ROLES)], "the frame step does not reopen the injection window"),
    M("plan-injects-in-warm-up", "vr", ["R3", "R4"], [(PLAN_INJECT, "        p.inject = p.admit == FlatCameraVrAdmit::Inject || p.admit == FlatCameraVrAdmit::Warming;")],
      "the plan writes a phase in a warm-up frame"),
    M("plan-phase-always-set", "vr", ["R4", "R5"], [(PLAN_PHASE, "        a.phaseNonzero = true;")], "a zero phase is never warming"),
    M("plan-window-always-open", "vr", ["R4", "R5"], [(PLAN_WINDOW, "        a.windowOpen = true;")], "a call after the trigger is never after-trigger"),
    # ---- R5, R6: the flush -------------------------------------------------------------------------------------------------------------
    M("flush-never-planned", "vr", "R5", [(PLAN_FLUSH, "        p.flush = false;")], "an injected camera seen un-injected is never flushed"),
    M("flush-on-inject", "vr", ["R6", "R3"], [(ELIGIBLE_TAIL, ELIGIBLE_TAIL.replace("        case FlatCameraVrAdmit::Inject:\n        case FlatCameraVrAdmit::OffThread:", "        case FlatCameraVrAdmit::OffThread:").replace("            return true;\n", "        case FlatCameraVrAdmit::Inject:\n            return true;\n", 1))],
      "a camera is flushed by the call that injects it"),
    M("flush-on-unreadable", "vr", "R6", [(ELIGIBLE_TAIL, ELIGIBLE_TAIL.replace("        case FlatCameraVrAdmit::Unreadable:\n            return false;", "            return false;").replace("            return true;\n", "        case FlatCameraVrAdmit::Unreadable:\n            return true;\n", 1))],
      "an unreadable call flushes (and writes) a camera"),
    M("flush-on-off-thread", "vr", "R6", [(ELIGIBLE_TAIL, ELIGIBLE_TAIL.replace("        case FlatCameraVrAdmit::OffThread:\n", "").replace("            return true;\n", "        case FlatCameraVrAdmit::OffThread:\n            return true;\n", 1))],
      "an off-thread call flushes a camera"),
    M("no-flush-on-kind-change", "vr", ["R5", "R6"], [(ELIGIBLE_TAIL, ELIGIBLE_TAIL.replace("        case FlatCameraVrAdmit::Unsupported:\n", "").replace("        case FlatCameraVrAdmit::Inject:\n", "        case FlatCameraVrAdmit::Unsupported:\n        case FlatCameraVrAdmit::Inject:\n", 1))],
      "a camera injected as kind 3 and seen again as kind 5 is never flushed"),
    M("no-flush-when-observed", "vr", ["R5", "R6", "R8"],
      [(ELIGIBLE_OBSERVED, "        case FlatCameraVrAdmit::NotActive:\n        case FlatCameraVrAdmit::Stale:\n"),
       ("        case FlatCameraVrAdmit::Inject:\n        case FlatCameraVrAdmit::OffThread:\n        case FlatCameraVrAdmit::Unreadable:\n            return false;\n",
        "        case FlatCameraVrAdmit::Observed:\n        case FlatCameraVrAdmit::Inject:\n        case FlatCameraVrAdmit::OffThread:\n        case FlatCameraVrAdmit::Unreadable:\n            return false;\n")],
      "a released route with the census watching leaves the camera with its last phase"),
    # ---- R7: the excluded signatures ----------------------------------------------------------------------------------------------------
    M("excluded-capacity-9", "vr", "R7", [("static constexpr size_t kCapacity = 8;\n    static constexpr double kSameTolerance", "static constexpr size_t kCapacity = 9;\n    static constexpr double kSameTolerance")],
      "the table keeps nine signatures"),
    M("excluded-tolerance-1-percent", "vr", "R7", [("kSameTolerance = 1.0e-3;", "kSameTolerance = 1.0e-2;")], "a fov 0.2% off is the same signature"),
    M("excluded-ignores-the-caller", "vr", "R7", [("if (e.callerRva == callerRva && same(e.aspect, aspect)", "if (same(e.aspect, aspect)")], "two call sites with one camera shape are one row"),
    M("excluded-nan-is-distinct", "vr", "R7", [("return std::isnan(a) && std::isnan(b);", "return false;")], "every unreadable camera is a new row"),
    M("excluded-never-recorded", "vr", ["R5", "R7"], drop(PLAN_EXCLUDED), "an exclusion records no signature"),
    # ---- R8: the mode word ---------------------------------------------------------------------------------------------------------------
    M("census-switches-injection-frame", "vr", "R8", [(CENSUS_RULE, "    if (!(bits & kFlatCameraVrBitDriven)) return bits;")], "the census's frame step makes an injection frame observe-only"),
    M("census-touches-an-undriven-word", "vr", "R8", [(CENSUS_RULE, "    if (bits & kFlatCameraVrBitInject) return bits;")], "the census's step drives a detour the route never stepped"),
    M("quiet-ignores-the-set", "vr", "R8", [(QUIET_RULE, "    return (bits & (kFlatCameraVrBitInject | kFlatCameraVrBitObserve)) == 0u;")], "quiet with a camera still waiting for its flush"),
    M("quiet-ignores-observation", "vr", "R8", [(QUIET_RULE, "    return (bits & kFlatCameraVrBitInject) == 0u && injectedSetEmpty;")], "quiet while the census observes"),
    M("dead-hook-honours-inject", "vr", "R8", drop(NOT_LIVE), "a hook that is not live still injects"),
    # ---- R9: the arithmetic and the window ---------------------------------------------------------------------------------------------
    M("bound-y-sign-flipped", "vr", "R9", [(BOUND_Y, "*outY = entryY + phaseY / static_cast<float>(renderH);")], "the vertical phase has the wrong sign"),
    M("bound-x-by-height", "vr", "R9", [(BOUND_X, "*outX = entryX + phaseX / static_cast<float>(renderH);")], "the horizontal phase is divided by the height"),
    M("phase-not-finite-checked", "vr", "R9", [(PHASE_FINITE, "renderW != 0 && renderH != 0 && ")], "a NaN phase is a phase"),
    M("frame-window-never-lapses", "vr", "R9", [(LAPSE, "if (stepAtMs == 0) return FlatCameraGateVerdict::Expired;")], "the route's last frame injects for ever"),
    M("failure-window-500-ms", "vr", "R9", [("static constexpr uint64_t kWindowMs = 5000;", "static constexpr uint64_t kWindowMs = 500;")], "the stand-down window is half a second"),
    M("frame-window-lapses-in-every-mode", "vr", "R9",
      [("    if (mode != FlatCameraVrMode::Inject || gate != FlatCameraGateVerdict::Admit) return gate;", "    if (gate != FlatCameraGateVerdict::Admit) return gate;")],
      "the route's step ages the census's window field in observe-only frames"),
    # ---- R11: the detour ------------------------------------------------------------------------------------------------------------------
    M("commit-guard-removed", "cpp", "R11", [(COMMIT_GUARD, "    float entryX = 0, entryY = 0;\n")], "a pass-through call writes the return slot"),
    M("commit-always-injects", "cpp", "R11", [(COMMIT_CALL, "vrCommit(r0, ctx, camera, callNo, true, wantPost)")], "every call writes a phase"),
    M("census-hears-without-observing", "cpp", "R11", [(REPORT_GUARD, "    if (camera) {\n")], "the census is told of calls in a frame that does not observe"),
    M("census-told-will-inject", "cpp", "R11", [(WILL_INJECT, "call.willInject = true;")], "the census is told every call will be injected"),
    M("outcome-counted-twice", "cpp", "R11", [(FINISH_CALL, FINISH_CALL + FINISH_CALL)], "a call is counted twice in the detour"),
    M("post-restores-before-the-census", "cpp", "R11", [(REPORT_BLOCK, ""), (RESTORE_END, RESTORE_END + REPORT_BLOCK)], "the census reads the camera after the phase was restored"),
    M("vr-branch-below-the-flat-switch", "cpp", "R11", [(BRANCH, ""), (OBSERVE_LINE, OBSERVE_LINE + BRANCH)], "the VR branch runs after the flat code read its switch"),
    M("vr-flag-not-cleared", "cpp", "R11", [(TLS_CLEAR, "    g_refreshTls.observe = 0;\n")], "a flat call can inherit a VR call's post half"),
    M("flat-inject-edited", "cpp", "R11", [(FLAT_JITY, "    const float jitY = entryY - jy / static_cast<float>(rw);")], "the flat injector's vertical phase uses the width"),
    M("flat-observe-edited", "cpp", "R11", [(FLAT_OBSERVE, "    if (observe && kind != 3) {\n        observeCall(r0, ctx, p2, camera, callNo, readable, kind, callerRva, gate);\n        return;\n    }\n")],
      "the census-only path changed"),
    M("census-step-removed", "cpp", "R11", [(CENSUS_STEP, "")], "the census's frame step never applies the mode word's rule"),
    M("publish-after-install", "cpp", "R11", [(PUBLISH, ""), (LIVE_COMMENT, PUBLISH + LIVE_COMMENT)], "the mode word is published after the install"),
    M("live-not-checked", "cpp", "R11", drop(LIVE_RETURN), "a hook that is not live goes on to arm the window"),
    M("io-on-the-call-path", "cpp", "R11", [(REPORT_LINE, "    Log::get().note(\"vr call\");\n" + REPORT_LINE)], "the call path logs"),
    M("flat-runtime-told-of-a-vr-call", "cpp", "R11", [(RESTORE_END, RESTORE_END.replace("    }\n", "        flatRuntimeNoteCameraApplied();\n    }\n"))], "the VR post half tells the flat runtime"),
    M("every-redirect-joins-the-flush-set", "cpp", "R11", [(NOINJECT_SET, "    g_inject.injected.noteInjected(camera);\n    return inject;")], "a census-only redirect makes the camera a flush candidate"),
    M("frame-window-lapse-not-applied", "cpp", "R11", [(EFFECTIVE, "in.gate = gate;")], "the route's step does not bound the window"),
    M("frame-window-lapse-for-the-wrong-mode", "cpp", "R11",
      [(EFFECTIVE, "in.gate = flatCameraVrEffectiveGate(FlatCameraVrMode::PassThrough, gate, g_vr.stepAtMs(), GetTickCount64());")], "an injecting frame's window never lapses"),
    M("frustum-always-read", "cpp", "R11", [(WANTS, "    frustum = readFrustum(camera);\n")], "every call reads the camera's aspect and near plane"),
    M("stand-down-dropped", "cpp", "R11", [(STAND, "    }\n    // This step is")], "failed writes never stand the hook down"),
    M("quiet-always-true", "cpp", "R11", [(QUIET_FN, "bool flatCameraVrQuiet() { return flatCameraVrQuietFor(g_vrBits.load(std::memory_order_acquire), true); }")], "quiet ignores the injected set"),
    M("relay-closed-whatever-the-frame-asks", "cpp", "R11", [(PAUSE_CALL, "    flatCameraInjectPause(true);\n")], "the relay gate closes while the frame injects"),
    M("census-registration-ignored", "cpp", "R11", [(OBSERVE_REG, "    const bool observe = frame.observe;\n")], "a route that does not say observe closes the relay on a running census"),
    M("quiet-comment-changed", "hdr", "R11", [(QUIET_TEXT, "a hook that never installed is not quiet.")], "flatCameraVrQuiet's declaration no longer has the agreed text"),
    # ---- R11: what the detour tells the planner and the census, what it saves for the post half, how it fails -------------------------------
    M("planner-told-every-kind-is-3", "cpp", "R11", [("    in.kind = kind;\n", "    in.kind = 3;\n")], "the planner is told every camera is kind 3"),
    M("planner-told-readable", "cpp", "R11", [("    in.readable = readable;\n", "    in.readable = true;\n")], "the planner is told an unreadable kind was read"),
    M("planner-told-inject-mode", "cpp", "R11", [("    in.mode = flatCameraVrModeOfBits(bits);\n", "    in.mode = FlatCameraVrMode::Inject;\n")], "every frame is an injection frame to the planner"),
    M("census-always-reported-to", "cpp", "R11", [(REPORT_LINE, "    const bool report = true;\n")], "the census is reported to whatever the frame says"),
    M("census-told-the-wrong-kind", "cpp", "R11", [("            call.kind = kind;\n", "            call.kind = 3;\n")], "the census is told every camera is kind 3"),
    M("failed-flush-counted-as-flushed", "cpp", "R11",
      [("        if (flushCamera(camera)) g_vr.tally().noteFlushed(); else g_vr.tally().noteWriteFailure();\n", "        flushCamera(camera);\n        g_vr.tally().noteFlushed();\n")],
      "a flush that failed is counted as flushed"),
    M("post-flags-swapped", "cpp", "R11",
      [("    g_refreshTls.vrReport = wantPost ? 1u : 0u;\n    g_refreshTls.vrRestore = inject ? 1u : 0u;\n", "    g_refreshTls.vrReport = inject ? 1u : 0u;\n    g_refreshTls.vrRestore = wantPost ? 1u : 0u;\n")],
      "the post half restores when only the census asked and reports when only the phase landed"),
    M("post-half-never-armed", "cpp", "R11", [("    g_refreshTls.armed = 1u;\n    g_refreshTls.vr = 1u;\n", "    g_refreshTls.vr = 1u;\n")], "the post half never runs, so the phase is never restored"),
    M("entry-y-not-saved", "cpp", "R11",
      [("    g_refreshTls.entryY = entryY;\n    g_refreshTls.flags = flags;\n    g_refreshTls.haveFlags = haveFlags ? 1u : 0u;\n    g_refreshTls.armed = 1u;\n    g_refreshTls.vr = 1u;\n",
        "    g_refreshTls.flags = flags;\n    g_refreshTls.haveFlags = haveFlags ? 1u : 0u;\n    g_refreshTls.armed = 1u;\n    g_refreshTls.vr = 1u;\n")],
      "the post half restores the vertical bound from a stale value"),
    M("read-failure-injects-anyway", "cpp", "R11", [("            g_vr.tally().noteWriteFailure();\n            inject = false;\n        } else {", "            g_vr.tally().noteWriteFailure();\n        } else {")],
      "a failed bound read goes on to redirect the return as an injection"),
    M("write-failure-injects-anyway", "cpp", "R11", [("                g_vr.tally().noteWriteFailure();\n                inject = false;\n            }\n", "                g_vr.tally().noteWriteFailure();\n            }\n")],
      "a rolled-back write goes on to redirect the return as an injection"),
    M("redirect-failure-not-counted", "cpp", "R11", [("        g_vr.tally().noteWriteFailure();\n        return false;\n    }\n    g_refreshTls.realRet = realRet;", "        return false;\n    }\n    g_refreshTls.realRet = realRet;")],
      "a failed redirect is not counted toward the stand-down"),
    M("frustum-near-far-swapped", "cpp", "R11", [("out.nearZ = nearZ; out.farZ = farZ;", "out.nearZ = farZ; out.farZ = nearZ;")], "the role test is given the far plane as the near plane"),
    M("stand-down-fed-the-wrong-count", "cpp", "R11", [("g_vrFailures.note(now, g_vr.tally().snapshot().writeFailures)", "g_vrFailures.note(now, g_vr.tally().snapshot().flushed)")],
      "the stand-down counts flushes, not failed writes"),
    M("failure-window-not-accumulated", "vr", "R9", [("        total_ += failures;\n", "        total_ = failures;\n")], "failed writes are not added up across frames"),
    M("role-always-known", "vr", ["R5", "R3", "R4"], [("        p.roleKnown = flatCameraVrWantsRole(a);", "        p.roleKnown = true;")], "the role is decided (and the frustum wanted) for every call"),
    M("excluded-near-far-swapped", "vr", ["R5", "R7"],
      [("excluded_.note(frustum.aspect, frustum.fov, frustum.nearZ, frustum.farZ, in.callerRva);", "excluded_.note(frustum.aspect, frustum.fov, frustum.farZ, frustum.nearZ, in.callerRva);")],
      "the excluded signature records the far plane as the near plane"),
    M("flush-decision-ignores-the-set", "vr", ["R5", "R6"], [("    return flatCameraVrFlushEligible(a) && set.takeForFlush(camera);", "    return flatCameraVrFlushEligible(a);")],
      "a camera this session never injected is flushed"),
]


# ---- applying an edit ------------------------------------------------------------------------------------------------------------------
def apply_edits(text, edits, name="?"):
    """The text with each (old, new) applied in order; each `old` must occur exactly once in the text as it stands then."""
    for old, new in edits:
        count = text.count(old)
        if count != 1:
            raise ValueError("mutation %s: an anchor occurs %d times (want 1): %r" % (name, count, old[:90]))
        if old == new:
            raise ValueError("mutation %s: an edit changes nothing: %r" % (name, old[:90]))
        text = text.replace(old, new)
    return text


def read_source(path):
    return path.read_bytes().decode("utf-8").replace("\r\n", "\n")


def failing_labels(output):
    """The labels of the rig's '  FAIL  <label>: ...' lines."""
    found = []
    for line in output.splitlines():
        m = re.match(r"  FAIL  (R\d+[a-z]+):", line)
        if m:
            found.append(m.group(1))
    return found


def label_matches(label, prefix):
    return re.match(re.escape(prefix) + r"[a-z]", label) is not None


def rig_labels(rig_text):
    return set(re.findall(r'"(R\d+[a-z]+)"', rig_text))


def label_block(bat_text, label):
    """The lines of one build.bat subroutine, continuation lines (a trailing ^) joined; None when the label is absent."""
    lines = bat_text.replace("\r\n", "\n").split("\n")
    start = next((i for i, l in enumerate(lines) if l.strip().lower() == label.lower() or l.lower().startswith(label.lower() + " ")), None)
    if start is None:
        return None
    out, joined = [], ""
    for line in lines[start + 1:]:
        if line.startswith(":") and not line.startswith("::"):
            break
        if line.rstrip().endswith("^"):
            joined += line.rstrip()[:-1] + " "
            continue
        out.append(joined + line)
        joined = ""
    return out


def include_closure(first_files):
    """Repo-relative paths of every file under the repo that `first_files` include with quotes, transitively, plus the files themselves."""
    seen, todo = set(), [Path(p) for p in first_files]
    while todo:
        path = todo.pop()
        path = path.resolve()
        if path in seen or not path.is_file():
            continue
        try:
            path.relative_to(ROOT)
        except ValueError:
            continue
        seen.add(path)
        for inc in re.findall(r'^\s*#\s*include\s+"([^"]+)"', path.read_text(encoding="utf-8", errors="replace"), re.M):
            todo.append(path.parent / inc)
    return sorted(seen)


# ---- the toolchain --------------------------------------------------------------------------------------------------------------------------
class Toolchain:
    """cl.exe and link.exe by absolute path, with the environment they need: this one if cl is on PATH, else the one vcvars64.bat makes,
    found with vswhere as build.bat does."""

    def __init__(self):
        found = shutil.which("cl.exe")
        if found:
            self.env = os.environ.copy()
        else:
            vswhere = Path(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")) / "Microsoft Visual Studio" / "Installer" / "vswhere.exe"
            vs = subprocess.run([str(vswhere), "-latest", "-products", "*", "-requires", "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
                                 "-property", "installationPath"], capture_output=True, text=True).stdout.strip()
            bat = Path(vs) / "VC" / "Auxiliary" / "Build" / "vcvars64.bat"
            if not vs or not bat.is_file():
                raise RuntimeError("cl.exe is not on PATH and no Visual Studio with the x64 C++ tools was found")
            dump = subprocess.run('cmd.exe /s /c ""%s" >nul && set"' % bat, capture_output=True, text=True).stdout
            self.env = {}
            for line in dump.splitlines():
                if "=" in line:
                    key, value = line.split("=", 1)
                    self.env[key] = value
            path = next((v for k, v in self.env.items() if k.upper() == "PATH"), "")
            found = shutil.which("cl.exe", path=path)
            if not found:
                raise RuntimeError("vcvars64.bat did not put cl.exe on PATH")
        self.cl = str(found)


def build_tree(dest, mutated=None):
    """The scratch tree: the rig at tools/flat_camera_vr_test, the rig's include closure and the pinned sources under src/d3d11, with
    `mutated` = (key, new text) replacing one file. Returns the rig's path in the tree."""
    rig_dest = dest / "tools" / "flat_camera_vr_test" / "flat_camera_vr_test.cpp"
    rig_dest.parent.mkdir(parents=True)
    shutil.copyfile(RIG, rig_dest)
    files = include_closure([RIG]) + [p.resolve() for p in EXTRA_COPIES]
    for src in sorted(set(files)):
        if src == RIG.resolve():
            continue
        rel = src.relative_to(ROOT)
        target = dest / rel
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(src, target)
    if mutated:
        key, text = mutated
        (dest / FILES[key].relative_to(ROOT)).write_text(text, encoding="utf-8", newline="\n")
    return rig_dest


def compile_and_run(tc, tree, control):
    """('nocompile'|'pass'|'fail'|'crash'|'timeout', detail): the rig built in `tree` and run with that tree as its root."""
    rig = tree / "tools" / "flat_camera_vr_test" / "flat_camera_vr_test.cpp"
    exe = tree / "rig.exe"
    cmd = [tc.cl] + CL_FLAGS + ["/Fo" + str(tree) + os.sep, "/Fe" + str(exe), str(rig), "/link", "/INCREMENTAL:NO"] + LINK_LIBS
    done = subprocess.run(cmd, capture_output=True, text=True, env=tc.env, errors="replace")
    if done.returncode != 0:
        text = (done.stdout + done.stderr).strip()
        return "nocompile", text.splitlines()[-1] if text else ""
    try:
        run = subprocess.run([str(exe), "--self-test", str(tree)], capture_output=True, text=True, env=tc.env, errors="replace", timeout=RUN_TIMEOUT)
    except subprocess.TimeoutExpired:
        return "timeout", "no result within %d s" % RUN_TIMEOUT
    out = run.stdout + "\n" + run.stderr
    if run.returncode == 0:
        return "pass", ""
    labels = failing_labels(out)
    if not labels:
        return "crash", "exit 0x%08X" % (run.returncode & 0xFFFFFFFF)
    return "fail", " ".join(labels)


# ---- the run -----------------------------------------------------------------------------------------------------------------------------------
def select(only):
    if not only:
        return MUTANTS
    wanted = [n for n in only.split(",") if n]
    unknown = [n for n in wanted if n not in {m.name for m in MUTANTS}]
    if unknown:
        raise ValueError("no such mutation: " + ", ".join(unknown))
    return [m for m in MUTANTS if m.name in wanted]


def plan_text(mutants):
    lines = ["%d mutation(s); each is built in a temp directory outside the repo, the rig compiled there and run with it as its root:" % len(mutants)]
    for m in mutants:
        lines.append("  %-38s %-6s caught by %-14s %s" % (m.name, m.target, "/".join(m.caught), m.why))
    lines.append("cl.exe flags: " + " ".join(CL_FLAGS) + "; libs: " + " ".join(LINK_LIBS))
    return "\n".join(lines)


def run_all(only=None, jobs=None, keep=False, dry_run=False, out=sys.stdout):
    mutants = select(only)
    if dry_run:
        print(plan_text(mutants), file=out)
        print("dry-run: nothing compiled, started or written", file=out)
        return 0
    tc = Toolchain()
    work = Path(tempfile.mkdtemp(prefix="flat_camera_vr_mutants_"))
    sources = {key: read_source(path) for key, path in FILES.items()}
    try:
        control_tree = work / "control"
        build_tree(control_tree)
        control, detail = compile_and_run(tc, control_tree, True)
        print("control (the unmutated sources, every check): %s %s" % (control, detail), file=out)
        if control != "pass":
            print("the rig does not pass on the unmutated sources when built this way; nothing below means anything", file=out)
            return 1

        def one(m):
            try:
                text = apply_edits(sources[m.target], m.edits, m.name)
            except ValueError as error:
                return m, "badedit", str(error)
            tree = work / m.name
            build_tree(tree, (m.target, text))
            outcome, detail = compile_and_run(tc, tree, False)
            return m, outcome, detail

        results = []
        with concurrent.futures.ThreadPoolExecutor(max_workers=jobs or min(8, os.cpu_count() or 2)) as pool:
            for m, outcome, detail in pool.map(one, mutants):
                if outcome == "fail" and any(label_matches(label, p) for label in detail.split() for p in m.caught):
                    verdict = "caught"
                elif outcome == "fail":
                    verdict = "OTHER"
                else:
                    verdict = {"pass": "SURVIVED", "crash": "CRASH", "timeout": "TIMEOUT", "nocompile": "NOCOMPILE", "badedit": "BADEDIT"}[outcome]
                results.append((m, verdict, detail))
                print("%-9s %-40s %s" % (verdict, m.name, detail[:150]), file=out)
        bad = [r for r in results if r[1] != "caught"]
        print("%d mutation(s): %d caught by their own rule, %d not" % (len(results), len(results) - len(bad), len(bad)), file=out)
        for m, verdict, detail in bad:
            print("  NOT CAUGHT: %s (%s): wanted %s, got %s %s" % (m.name, m.why, "/".join(m.caught), verdict, detail[:120]), file=out)
        return 0 if not bad else 1
    finally:
        if keep:
            print("kept: %s" % work, file=out)
        else:
            shutil.rmtree(work, ignore_errors=True)


# ---- the self-test --------------------------------------------------------------------------------------------------------------------------------
def self_test():
    failures = []

    def check(ok, what):
        if not ok:
            failures.append(what)

    # the tool's own pieces
    check(failing_labels("  ok    R1a: x\n  FAIL  R11ab: y\n  FAIL  R2c: z\n") == ["R11ab", "R2c"], "failing_labels reads the rig's FAIL lines")
    check(label_matches("R1c", "R1") and not label_matches("R10a", "R1") and not label_matches("R11a", "R1") and label_matches("R11aa", "R11") and not label_matches("R1", "R1"),
          "a label prefix matches the rule's own labels and not another rule's")
    check(apply_edits("a b c", [("b", "B")]) == "a B c", "apply_edits applies one edit")
    for bad, why in ((("a b b", [("b", "B")]), "a repeated anchor"), (("a b", [("x", "y")]), "a missing anchor"), (("a b", [("b", "b")]), "a no-op edit")):
        try:
            apply_edits(*bad)
            check(False, "apply_edits refuses " + why)
        except ValueError:
            pass
    joined = label_block(":rig_a\nx ^\n y\nexit /b 0\n:rig_b\nz\n", ":rig_a")
    check(joined is not None and [" ".join(l.split()) for l in joined] == ["x y", "exit /b 0"], "label_block joins continuations and stops at the next label")

    # every mutation against the sources as they are now, and against the rig
    sources = {key: read_source(path) for key, path in FILES.items()}
    rig = read_source(RIG)
    labels = rig_labels(rig)
    names = [m.name for m in MUTANTS]
    check(len(names) == len(set(names)), "mutation names are unique")
    check(len(MUTANTS) >= 95, "the mutation list did not shrink (%d)" % len(MUTANTS))
    for m in MUTANTS:
        check(m.target in FILES, "%s names a file this tool knows" % m.name)
        if m.target not in FILES:
            continue
        try:
            mutated = apply_edits(sources[m.target], m.edits, m.name)
            check(mutated != sources[m.target], "%s changes the source" % m.name)
        except ValueError as error:
            failures.append(str(error))
        for p in m.caught:
            check(any(label_matches(label, p) for label in labels), "%s: the rig has no check labelled %s<letter>" % (m.name, p))
    wanted = {re.match(r"R\d+", p).group(0) for m in MUTANTS for p in m.caught}
    have = {re.match(r"R\d+", label).group(0) for label in labels}
    check(have <= wanted, "every group of the rig has a mutation that names it: missing %s" % sorted(have - wanted))
    # the rig's labels are unique to a check (a duplicate would let a mutation be 'caught' by the wrong check)
    all_labels = re.findall(r'"(R\d+[a-z]+)",\s*"', rig)   # every check(...) names its label, then its text
    check(len(all_labels) == len(set(all_labels)) and set(all_labels) == labels,
          "no two checks of the rig share a label: %s" % sorted({l for l in all_labels if all_labels.count(l) > 1}))

    # the include closure finds what the rig includes, and the scratch tree is complete
    closure = [p.name for p in include_closure([RIG])]
    check("flat_camera_vr.h" in closure and "flat_camera_phase.h" in closure and "flat_camera_inject.h" in closure,
          "the rig's include closure holds the pure header, the admission header and the interface header")

    # build.bat compiles the rig the way this tool does, and runs this tool's self-test
    bat = BUILD_BAT.read_bytes().decode("utf-8", errors="replace")
    block = label_block(bat, RIG_LABEL)
    check(block is not None, "build.bat has %s" % RIG_LABEL)
    if block:
        text = "\n".join(block)
        cl = next((l for l in block if l.strip().lower().startswith("cl.exe")), "")
        for flag in CL_FLAGS:
            check(flag in cl, "build.bat's rig compile has %s" % flag)
        check("tools\\flat_camera_vr_test\\flat_camera_vr_test.cpp" in cl, "build.bat compiles the rig")
        for lib in LINK_LIBS:
            check(lib in cl, "build.bat's rig link has %s" % lib)
        check("flat_camera_vr_test.exe\" --self-test \"%ROOT%\"" in text, "build.bat runs the rig's --self-test with the repo root")
        check("mutants.py\" --self-test" in text, "build.bat runs this tool's --self-test")

    # --dry-run starts nothing and writes nothing
    calls = []
    real_mkdtemp, real_run = tempfile.mkdtemp, subprocess.run
    tempfile.mkdtemp = lambda *a, **k: calls.append("mkdtemp") or real_mkdtemp(*a, **k)
    subprocess.run = lambda *a, **k: calls.append("subprocess") or real_run(*a, **k)
    try:
        sink = io.StringIO()
        code = run_all(dry_run=True, out=sink)
    finally:
        tempfile.mkdtemp, subprocess.run = real_mkdtemp, real_run
    check(code == 0 and not calls and "dry-run: nothing" in sink.getvalue() and sink.getvalue().count("\n") > len(MUTANTS), "--dry-run prints the plan and starts nothing")

    if failures:
        for f in failures:
            print("FAIL: " + f)
        print("flat camera vr mutants self-test: %d FAILED" % len(failures))
        return 1
    print("flat camera vr mutants self-test: PASS (%d mutations over %d rig checks)" % (len(MUTANTS), len(labels)))
    return 0


def main(argv):
    ap = argparse.ArgumentParser(description="mutation proof for tools\\flat_camera_vr_test")
    ap.add_argument("--self-test", action="store_true")
    ap.add_argument("--run", action="store_true")
    ap.add_argument("--list", action="store_true")
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--only")
    ap.add_argument("--jobs", type=int)
    ap.add_argument("--keep", action="store_true")
    args = ap.parse_args(argv)
    if args.self_test:
        return self_test()
    if args.list:
        print(plan_text(MUTANTS))
        return 0
    if args.run:
        return run_all(only=args.only, jobs=args.jobs, keep=args.keep, dry_run=args.dry_run)
    ap.print_usage()
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
