#!/usr/bin/env python3
"""The mutation proof for the VR world route's three behaviour rigs: tools\\vr_world_route_test (the pure half),
tools\\vr_world_route_gpu_test (the route's runtime on WARP) and tools\\ui_layer_world_test (the layer's half, ui_layer.cpp on WARP).
The CURVED ROUTE (design doc section 82: a curved screen no longer holds the route off, the route's lines name the curve, the layer
plans a substituted draw like a flat one, and a re-issue that did not land takes no eye) is pinned by all three; a rig that passes
proves little until it is seen to FAIL on a source that breaks what it pins.

For each mutation below this tool applies one textual edit to ONE production file, compiles the rig against the edited copy in a temp
directory OUTSIDE the repo, runs it, and requires it to fail on a check whose label contains the text the mutation names:

  pure   src\\d3d11\\vr_world_route_math.h: the rig's include closure is copied into the temp directory with the edited header, and
         the rig is built and run there (its source pins still read the real repo, which they are not about);
  gpu    src\\d3d11\\vr_world_route.cpp: the edited copy replaces the production file in the rig's compile line (everything else is
         the real tree, found through /I); the rig takes about half a minute to run (it waits out real 5 s windows);
  layer  src\\d3d11\\ui_layer.cpp: the same, for the layer's rig.
  wiring src\\d3d11\\vscreen.cpp, for the SURFACE STRIP's wiring pins (intro movie and splash; docs\\intro-video.md, job 3), which are source
         scans: tools\\ui_quality_test is built ONCE from the real tree (build.bat's :rig_ui_quality_test compile line) and run with
         --wiring in a temp tree whose src\\d3d11 holds the edited vscreen.cpp (the rig reads its sources from the working directory, and the
         files beside vscreen.cpp it scans are copied unedited). Nothing is compiled per mutation, so the whole rig takes seconds. It proves
         the pins end to end on the real file, where the controls inside the rig prove each pin function on a copy.

Nothing is written inside the repo; the temp directory is removed at the end. (The route's SOURCE pins hold their own controls inside
the rigs: tools\\vr_world_route_test curveBoundaryControls, tools\\ui_quality_test testControls, tools\\on_foot_maps_test P4a.)

  python tools\\vr_world_route_test\\mutants.py --self-test          text only: every anchor is found exactly once in its file as it is
                                                                     now, every label named is in its rig, and build.bat compiles the
                                                                     rigs the way this tool does and runs this self-test
  python tools\\vr_world_route_test\\mutants.py --run [--rig pure|gpu|layer|wiring] [--only a,b] [--jobs N] [--keep]
  python tools\\vr_world_route_test\\mutants.py --list
  python tools\\vr_world_route_test\\mutants.py --run --dry-run      the plan; writes nothing, starts nothing

--run needs the MSVC toolchain (cl.exe on PATH, or Visual Studio found with vswhere as build.bat does) and, for the gpu rig, the
generated headers of one build (build\\gen). --self-test runs in build.bat's rig and is what keeps an edit of the sources from silently
orphaning a mutation: if an anchor stops matching, the build fails and this file says which.
"""
import argparse
import concurrent.futures
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
SRC = ROOT / "src" / "d3d11"
BUILD_BAT = ROOT / "build.bat"
GEN = ROOT / "build" / "gen"

# One entry a rig: where it is, its build.bat label, the file its mutations edit, and what its compile line holds (the self-test checks
# that build.bat still compiles it that way).
RIGS = {
    "pure": {
        "rig": HERE / "vr_world_route_test.cpp",
        "label": ":rig_vr_world_route_test",
        "target": SRC / "vr_world_route_math.h",
        "sources": ["tools\\vr_world_route_test\\vr_world_route_test.cpp"],
        "libs": ["kernel32.lib"],
        "include_gen": False,
    },
    "gpu": {
        "rig": ROOT / "tools" / "vr_world_route_gpu_test" / "vr_world_route_gpu_test.cpp",
        "label": ":rig_vr_world_route_gpu_test",
        "target": SRC / "vr_world_route.cpp",
        # the production source the mutations edit is the second of these; it is replaced by the edited copy
        "sources": ["tools\\vr_world_route_gpu_test\\vr_world_route_gpu_test.cpp", "src\\d3d11\\vr_world_route.cpp", "src\\d3d11\\binding_shadow.cpp",
                    "src\\d3d11\\flat_mono_resolve.cpp", "src\\d3d11\\flat_projection_scope.cpp", "src\\d3d11\\flat_projection_runtime.cpp",
                    "src\\common\\config.cpp", "src\\common\\proxy.cpp", "src\\common\\guard.cpp"],
        "libs": ["dxgi.lib", "d3dcompiler.lib", "user32.lib", "version.lib"],
        "include_gen": True,
    },
    "layer": {
        "rig": ROOT / "tools" / "ui_layer_world_test" / "ui_layer_world_test.cpp",
        "label": ":rig_ui_layer_world_test",
        "target": SRC / "ui_layer.cpp",
        # the production source the mutations edit is the second of these; it is replaced by the edited copy
        "sources": ["tools\\ui_layer_world_test\\ui_layer_world_test.cpp", "src\\d3d11\\ui_layer.cpp", "src\\common\\config.cpp", "src\\common\\log.cpp",
                    "src\\common\\guard.cpp", "third_party\\dxbc_hash\\DxilHash.cpp"],
        "libs": ["d3dcompiler.lib", "user32.lib"],
        "include_gen": True,
        "flags": ["/DUNICODE", "/D_UNICODE"],
    },
    "wiring": {
        "rig": ROOT / "tools" / "ui_quality_test" / "ui_quality_test.cpp",
        # the text the failing labels live in (the rig includes it): the self-test looks for each label's text here
        "labels": [ROOT / "tools" / "ui_quality_test" / "ui_intro_curve_wiring_test.h"],
        "label": ":rig_ui_quality_test",
        "target": SRC / "vscreen.cpp",
        "sources": ["tools\\ui_quality_test\\ui_quality_test.cpp"],
        "libs": ["d3dcompiler.lib"],
        "include_gen": True,
        # the other files `ui_quality_test --wiring` scans, copied unedited beside the edited vscreen.cpp
        "reads": ["src/d3d11/ui_layer.cpp", "src/d3d11/native_temporal.cpp", "src/d3d11/native_sharpen.cpp"],
    },
}
WIRING_EXE = {}   # the wiring rig, built once by its control and run in every mutation's tree
CL_FLAGS = ["/nologo", "/O2", "/MT", "/std:c++17", "/EHsc", "/W4", "/DWIN32_LEAN_AND_MEAN", "/DNOMINMAX", "/D_CRT_SECURE_NO_WARNINGS"]
RUN_TIMEOUT = 300.0


COMPILE = "compile: "


class Mutant:
    def __init__(self, name, rig, expect, edits, why):
        self.name = name
        self.rig = rig                                  # a key of RIGS
        self.expect = expect                            # text a failing check's label must contain; "compile: <text>": the build must fail with <text>
        self.edits = list(edits)                        # (old, new) pairs, applied in order
        self.why = why
        self.compile_error = expect.startswith(COMPILE)  # caught by a static check: the edited source must not compile
        self.text = expect[len(COMPILE):] if self.compile_error else expect


def M(name, rig, expect, edits, why):
    return Mutant(name, rig, expect, edits, why)


# ---- wiring: the raw text of vscreen.cpp's blocks the mutations move or delete (the self-test finds each exactly once) ------------------
W_CLEAR = "    s->curveThisDraw = false;\n    s->introCurveThisDraw = false;\n"
W_SET = "    if (kind == 'X' && count == 6 && introCurveWants()) {\n        s->introCurveThisDraw = introCurveOnComposite(self, kind, count, instances);\n    }\n"
W_END = "    if (g_state->introCurveThisDraw) {\n        g_state->introCurveThisDraw = false;\n        introCurveEndDraw();\n    }\n"
W_AFTER_BACKDROP = "        return DrawVerdict::kBackdrop;\n    }\n\n    // The FSS panel composite pair"
W_BEGIN = "    if (v != DrawVerdict::kNone) forwardVerdictBegin(self, v);\n"
W_OBSERVED = ("    const bool originalIssued=observedDraw(alteredClass == AlteredDrawClass::Verdict\n"
              "                                               ? AlteredDraw(alteredClass, alteredFixOf(v)) : AlteredDraw(alteredClass));\n")
W_STRIP_START = "    float stripGain = 0.0f;\n"
W_GUARD = ("    if (panelCurveWants() && srv0IsPanelSized(s, kind, count) &&\n"
           "        !(kind == 'X' && count == 6 && bindingShaderHash(BindSlot::Vs) == kIntroCompositeVsHash)) {\n")
W_GATE = ("        vscreenFootprintWanted() ||   // the footprint instrument (vscreen_footprint.h): it reads the 2D screen's composite\n"
          "        introCurveWants();")
W_DIM = ("            if (!(stripIssued && panelCurveSurfaceDraw(self, stripGain, stripToward, stripReverseU, g_state->realDrawIndexedInstanced))) {\n"
         "                draw(AlteredDrawClass::None);\n            }\n")
W_DIM_CALL = "panelCurveSurfaceDraw(self, stripGain, stripToward, stripReverseU, g_state->realDrawIndexedInstanced))) {"
W_REVERSE_DECL = "    bool stripReverseU = false;\n"
W_MOVIE_ARMED = "            if (introPanelStripArmed()) {\n"
W_MOVIE_REVERSE = "                stripReverseU = introPanelStripReverseU();\n"
W_REFUSAL = ("        if (owner && uiLayerIssueBlocked()) return false;\n"
             "        if (stripIssued) return true;   // the strip was issued in its place: nothing more, and something was\n")
W_CANDIDATE = "if ((v == DrawVerdict::kIntroPanel || g_state->introCurveThisDraw) && owner && panelCurveSurfaceWanted()) {"
W_STRIP_CALL = "stripIssued = panelCurveSurfaceDraw(self, stripGain, stripToward, stripReverseU, g_state->realDrawIndexedInstanced);"
W_TICK = ("        tkIntroCurve.run([&] {\n            const bool sceneFrame = g_state->eyeDrawsLastFrame >= kSceneEyeDraws;\n"
          "            introCurveTick(g_state->ownerCtx, sceneFrame);\n            if (sceneFrame) introCurveNoteRetired();\n        });\n")
W_RETIRE_TEXT = "in all (the movie's, the splash's and the splash dim's re-issues of either)"
W_MOVIE_COMMENT = "    // The intro movie's panel (intro_panel.h). First thing in the eye\n"
W_CENSUS_LINE = "    // The census line for this draw, recorded while its bindings are certainly\n"


def wiring(name, pin, edits, why):
    """One mutation of the wiring rig: the rig must fail on the check the named pin makes."""
    return M(name, "wiring", "intro curve wiring [%s]" % pin, edits, why)


# ---- anchors: the sources, verbatim (the self-test finds each exactly once) -----------------------------------------------------------
MUTANTS = [
    # ---- pure: vrWorldFormatCurve, the window's tokens and the OWNS line (vr_world_route_math.h) ----------------------------------------
    M("curve-off-lost", "pure", "curve: a screen nothing curves",
      [('    if (!configured) return std::snprintf(out, size, "off");\n', '    if (!configured && false) return std::snprintf(out, size, "off");\n')],
      "a screen nothing curves no longer reads off"),
    M("curve-standdown-lost", "pure", "curve: a substitution that stood itself down",
      [('    if (standDown) return std::snprintf(out, size, "stood-down");\n', "")], "a stood-down substitution no longer reads stood-down"),
    M("curve-pending-lost", "pure", "curve: asked for and no strip drawn yet",
      [('    if (!ready) return std::snprintf(out, size, "pending");\n', "")], "a strip not drawn yet no longer reads pending"),
    M("curve-two-decimals", "pure", "curve: a strip in hand reads", [('"%.3f/%d/%.3f"', '"%.2f/%d/%.2f"')], "the strip's numbers print two decimals"),
    M("curve-order-swapped", "pure", "curve: a substitution that stood itself down",
      [('    if (standDown) return std::snprintf(out, size, "stood-down");\n    if (!ready) return std::snprintf(out, size, "pending");\n',
        '    if (!ready) return std::snprintf(out, size, "pending");\n    if (standDown) return std::snprintf(out, size, "stood-down");\n')],
      "pending is tested before stood-down"),
    M("window-token-renamed", "pure", "curve: the 5 s line carries curve=",
      [("steady-detail=%s curve=%s curve-reissues=%llu ", "steady-detail=%s curve=%s reissues=%llu ")], "the strip count's token is renamed"),
    M("window-curve-token-renamed", "pure", "curve: the 5 s line carries curve=",
      [("steady-detail=%s curve=%s curve-reissues=%llu ", "steady-detail=%s crv=%s curve-reissues=%llu ")], "the curve's token is renamed"),
    M("entered-appends-for-off", "pure", "curve: ... and with curve=off",
      [('    if (curve && std::strcmp(curve, "off") != 0) {\n', "    if (curve) {\n")], "a flat screen's OWNS line gains a sentence"),
    M("entered-default-curve", "pure", "curve: the OWNS line without a curve argument",
      [("const char* curve = nullptr) {", 'const char* curve = "0.1/64/1.0") {')], "a caller that passes no curve gets one"),
    # The OWNS line says what is true per state: one sentence for a strip in hand, one for pending, one for stood-down.
    M("entered-strip-sentence-reworded", "pure", "curve: a strip in hand (0.300/64/35.556): the OWNS line is the flat line plus the sentence",
      [('constexpr char kVrWorldEnteredStripHead[] = "; the screen is curved (curve=";', 'constexpr char kVrWorldEnteredStripHead[] = "; the screen is bent (curve=";')],
      "the strip sentence is reworded"),
    M("entered-strip-gets-pending-sentence", "pure", "curve: a strip in hand (0.300/64/35.556): the OWNS line is the flat line plus the sentence",
      [('        else std::snprintf(tail, sizeof(tail), "%s%s%s", kVrWorldEnteredStripHead, curve, kVrWorldEnteredStripTail);\n',
        '        else std::snprintf(tail, sizeof(tail), "%s", kVrWorldEnteredPending);\n')],
      "a strip in hand is given the pending sentence"),
    M("entered-pending-gets-strip-sentence", "pure", "curve: pending: the OWNS line is the flat line plus the sentence",
      [('        if (std::strcmp(curve, "pending") == 0) std::snprintf', '        if (false) std::snprintf')],
      "pending is given the strip sentence: the game and the layer are said to draw a strip"),
    M("entered-stood-down-gets-strip-sentence", "pure", "curve: stood-down: the OWNS line is the flat line plus the sentence",
      [('        else if (std::strcmp(curve, "stood-down") == 0) std::snprintf', '        else if (false) std::snprintf')],
      "stood-down is given the strip sentence"),
    M("entered-pending-gets-stood-down-sentence", "pure", "curve: pending: the OWNS line is the flat line plus the sentence",
      [('std::snprintf(tail, sizeof(tail), "%s", kVrWorldEnteredPending);', 'std::snprintf(tail, sizeof(tail), "%s", kVrWorldEnteredStoodDown);')],
      "pending is given the stood-down sentence"),
    M("entered-stood-down-gets-pending-sentence", "pure", "curve: stood-down: the OWNS line is the flat line plus the sentence",
      [('std::snprintf(tail, sizeof(tail), "%s", kVrWorldEnteredStoodDown);', 'std::snprintf(tail, sizeof(tail), "%s", kVrWorldEnteredPending);')],
      "stood-down is given the pending sentence"),
    M("entered-pending-reworded", "pure", "curve: pending: the OWNS line is the flat line plus the sentence",
      [("the strip is not built yet, so the game and the layer both draw the flat quad until it is", "the strip is built, so the game and the layer both draw it")],
      "the pending sentence says a strip is drawn"),
    M("entered-stood-down-reworded", "pure", "curve: stood-down: the OWNS line is the flat line plus the sentence",
      [("the game draws its own flat quad and the layer re-issues it flat\";", "the game and the layer both draw the strip\";")],
      "the stood-down sentence says a strip is drawn"),
    M("entered-tail-small", "pure", "curve: a strip in hand (0.300/64/35.556): the OWNS line is the flat line plus the sentence",
      [('    char tail[kVrWorldEnteredTailBytes] = "";', '    char tail[64] = "";')], "the suffix buffer is smaller than its constant and cuts a sentence"),
    # The compile-time check: a sentence that outgrows the suffix buffer fails the build (the message must name it).
    M("entered-pending-too-long", "pure", "compile: the OWNS line's pending sentence must fit the suffix buffer",
      [('both draw the flat quad until it is";', 'both draw the flat quad until it is' + ", and then some" * 20 + '";')], "the pending sentence outgrows the suffix buffer"),
    M("entered-stood-down-too-long", "pure", "compile: the OWNS line's stood-down sentence must fit the suffix buffer",
      [("the layer re-issues it flat\";", "the layer re-issues it flat" + ", and then some" * 20 + "\";")], "the stood-down sentence outgrows the suffix buffer"),
    M("entered-strip-too-long", "pure", "compile: the OWNS line's strip sentence (with the widest curve text) must fit the suffix buffer",
      [("so the bend and the placement are the game's\";", "so the bend and the placement are the game's" + ", and then some" * 20 + "\";")], "the strip sentence outgrows the suffix buffer"),
    M("window-default-curve-empty", "pure", "curve: a default window says curve off", [('    char curve[48] = "off";', '    char curve[48] = "";')],
      "a default window's curve is empty"),
    M("window-default-reissues", "pure", "curve: a default window says curve off", [("    uint64_t curveReissues = 0;", "    uint64_t curveReissues = 1;")],
      "a default window has already re-issued a strip"),
    # The world's jitter and the depth-checked steady detail are ALWAYS ON (their keys retired 2026-10-01): the decision has no key to
    # hold it off, the global jitter key is the one thing that does, and the lines the route writes say steady-detail=on.
    M("jitter-never-on", "pure", "jitter: route auto, global on, route wants, last frame named, hook live, no fault: On",
      [("    if (!hookLive) return VrWorldJitter::NoHook;\n    return VrWorldJitter::On;\n", "    if (!hookLive) return VrWorldJitter::NoHook;\n    return VrWorldJitter::Idle;\n")],
      "the decision never says On: a route that is Warming or Owned, named and hooked, is left unjittered"),
    M("jitter-global-ignored", "pure", "jitter: experimental.temporal_aa_jitter off is GlobalOff",
      [("    if (!globalJitter) return VrWorldJitter::GlobalOff;\n", "")], "the global jitter key no longer stops the world's jitter"),
    M("jitter-global-token-on", "pure", "jitter: the tokens of the 5 s line",
      [('        case VrWorldJitter::GlobalOff: return "off";\n', '        case VrWorldJitter::GlobalOff: return "on";\n')],
      "the 5 s line says jitter=on for a world the global key left unjittered"),
    M("window-steady-default-off", "pure", "route line: steady-detail=on sits after fp-mode in a window nothing has touched",
      [('    const char* steady = "on";\n', '    const char* steady = "off";\n')], "the route line says steady-detail=off unless something sets it on"),
    M("refusal-steady-default-off", "pure", "refusal line: every counter is printed, zero included, an empty window reads census=off pixels=0, and the steady detail reads on",
      [('    const char* steady = "on";            // always on in this build; only a rig that models an older log sets "off"\n',
        '    const char* steady = "off";           // always on in this build; only a rig that models an older log sets "off"\n')],
      "the refusal line says steady-detail=off unless something sets it on"),
    # ---- gpu: the route's runtime (vr_world_route.cpp) ----------------------------------------------------------------------------------
    M("old-stand-aside", "gpu", "curved screen: the first boundary with the key auto leaves the route Observing",
      [("    const bool layerLive = uiLayerLiveForWorldRoute();\n", "    const bool layerLive = uiLayerLiveForWorldRoute() && !panelCurveWants();\n")],
      "a curved screen holds the route off again"),
    M("owns-line-no-curve", "gpu", "curved screen: the OWNS line is printed once and says what is true while no strip is built",
      [("vrWorldFormatEntered(line, sizeof(line), g_frameNo, kVrWorldWarmFrames, curveText);", "vrWorldFormatEntered(line, sizeof(line), g_frameNo, kVrWorldWarmFrames);")],
      "the OWNS line is not told the curve"),
    M("ready-ignored", "gpu", "curved screen: the OWNS line is printed once and says what is true while no strip is built",
      [("configured, pc.standDown, pc.ready,", "configured, pc.standDown, true,")], "a strip that is not built is named as if it were (the OWNS line claims the layer draws it)"),
    M("reissues-cumulative", "gpu", "curved screen: curve-reissues is each window's own count",
      [("g_win.curveReissues = curveReissues - g_curveReissuesSeen;", "g_win.curveReissues = curveReissues;")], "the 5 s line prints the cumulative strip count, not the window's"),
    M("curve-always-configured", "gpu", "owned: a flat screen: the 5 s line says curve=off",
      [("const bool configured = pc.curvature > 0.0f || pc.segments != detail::kDefaultSegments;", "const bool configured = true;")],
      "a flat screen's lines name a curve"),
    M("standdown-ignored", "gpu", "curved screen: a substitution that stood itself down does not hold the route off",
      [("configured, pc.standDown, pc.ready,", "configured, false, pc.ready,")], "a stood-down substitution is not named"),
    M("stays-off-names-curve", "gpu", "curved screen, layer not live",
      [('so keep fix.ui_quality on)", why);', 'so keep fix.ui_quality on and fix.panel_curvature at 0)", why);')], "the stays-off line names the curve again"),
    M("window-curve-never-filled", "gpu", "curved screen: the 5 s line names the curve before its strip is drawn",
      [("curveTextNow(g_win.curve, sizeof(g_win.curve), &curveReissues);", "curveReissues = 0;")], "the 5 s line's curve is never read from panel_curve"),
    M("key-off-reads-curve", "gpu", "key off with a curved screen",
      [("    g_census = vrCameraCensusWanted();\n", '    g_census = vrCameraCensusWanted();\n    if (panelCurveInfo().curvature > 0.0f) Log::get().note("curve seen with the key off");\n')],
      "a route that is off reads (and logs about) the curve"),
    M("curve-gain-dropped", "gpu", "curved screen: once the strip is ready the 5 s line names curvature/columns/gain",
      [("pc.curvature, pc.segments, pc.gain);\n    if (reissues)", "pc.curvature, pc.segments, 0.0f);\n    if (reissues)")], "the strip's gain is not named"),
    # The world's jitter and the depth-checked steady detail are ALWAYS ON (their keys retired 2026-10-01).
    M("steady-detail-off", "gpu", "owned: the steady detail is on with no key",
      [("    f.steadyDetail = true;\n", "    f.steadyDetail = false;\n")],
      "the route hands the resolver steadyDetail = false: its depth check counts no frame and the window prints no refusal line"),
    M("jitter-global-key-ignored", "gpu", "global key: with experimental.temporal_aa_jitter off the route owns the world",
      [('    const bool globalJitter = Config::get().getBool("experimental.temporal_aa_jitter", true);\n', "    const bool globalJitter = true;\n")],
      "the global jitter key no longer stops the world's jitter"),
    M("jitter-never-asked", "gpu", "jitter: the first boundary after a treated frame (Warming, it named its source) opens the window",
      [("vrWorldJitterDecide(true, globalJitter, wantsInjection && w && h, g_namedLast, true, g_injectFault);",
        "vrWorldJitterDecide(true, false, wantsInjection && w && h, g_namedLast, true, g_injectFault);")],
      "the route decides as if the global key were off: the world it owns is never jittered"),
    # ---- layer: the plan and End (ui_layer.cpp) -------------------------------------------------------------------------------------------
    M("plan-refuses-substituted", "layer", "a substituted (curved) screen draw: the decision is the route's",
      [("    if (f.ds.tests() || f.ds.writes()) {\n        why = UiWorldRefuse::kDepthState;",
        "    if (f.substituted) {\n        why = UiWorldRefuse::kFault;\n    } else if (f.ds.tests() || f.ds.writes()) {\n        why = UiWorldRefuse::kDepthState;")],
      "the plan refuses a substituted (curved) draw again"),
    M("end-ignores-landed", "layer", "the eye is NOT taken (the route is not told, nothing is counted as a re-issue)",
      [("    if (!landed) {\n", "    if (false) {\n")], "End takes the eye for a draw that did not land"),
    M("end-counts-landed-when-not", "layer", "the eye is NOT taken (the route is not told, nothing is counted as a re-issue)",
      [("        worldRefuse(plan.eye, uiWorldReasonId(UiWorldRefuse::kFault));\n        return;\n    }\n    ++g_win.worldReissued;",
        "        ++g_win.worldReissued;\n        worldRefuse(plan.eye, uiWorldReasonId(UiWorldRefuse::kFault));\n        return;\n    }\n    ++g_win.worldReissued;")],
      "a draw that did not land is counted as a re-issue as well as a fault"),
    M("end-not-landed-uncounted", "layer", "and the refusal is counted as a fault",
      [("        worldRefuse(plan.eye, uiWorldReasonId(UiWorldRefuse::kFault));\n        return;\n    }\n    ++g_win.worldReissued;",
        "        return;\n    }\n    ++g_win.worldReissued;")], "a draw that did not land is not counted as a fault"),
    M("end-eye-not-marked", "layer", "the eye is the route's, as a flat screen's is",
      [("    g_eye[plan.eye].worldSeq = plan.seq;\n", "")], "a landed re-issue does not mark the eye as the route's"),
    # ---- wiring: the surface strip in vscreen.cpp (the movie and the splash), end to end through ui_quality_test --wiring ------------------
    wiring("iw-flag-never-cleared", "flag-lifetime", [(W_CLEAR, "    s->curveThisDraw = false;\n")], "the splash's flag is not cleared at the top of beginPanelOverride"),
    wiring("iw-flag-thunk-leaves-it", "flag-lifetime", [(W_END, "")], "the thunk never puts the numbers and the flag away"),
    wiring("iw-flag-put-away-before-the-draw", "flag-lifetime",
           [(W_END, ""), ("    const DrawVerdict v = beginPanelOverride(self, 'X', perInstance, instances, args);\n",
                          "    const DrawVerdict v = beginPanelOverride(self, 'X', perInstance, instances, args);\n" + W_END)],
           "the numbers are put away before the draw and the dim have used them"),
    wiring("iw-recognition-without-wants", "recognition", [("if (kind == 'X' && count == 6 && introCurveWants()) {", "if (kind == 'X' && count == 6) {")],
           "the recogniser is asked at curvature 0 too"),
    wiring("iw-recognition-below-the-backdrop", "recognition",
           [(W_SET, ""), (W_AFTER_BACKDROP, "        return DrawVerdict::kBackdrop;\n    }\n" + W_SET + "\n    // The FSS panel composite pair")],
           "the recognition sits below the eye-side backdrop claim, which returns first"),
    wiring("iw-recognition-always-armed", "recognition",
           [("s->introCurveThisDraw = introCurveOnComposite(self, kind, count, instances);\n",
             "introCurveOnComposite(self, kind, count, instances);\n        s->introCurveThisDraw = true;\n")],
           "the flag is set whatever the recogniser said"),
    wiring("iw-recognition-above-the-movie", "after-movie", [(W_SET, ""), (W_MOVIE_COMMENT, W_SET + "\n" + W_MOVIE_COMMENT)],
           "the recognition sits above the movie's claim: the movie's own draws reach the recogniser and their stock pairs are learned (and logged) as flat"),
    wiring("iw-recognition-away-from-the-movie", "after-movie", [(W_SET, ""), (W_CENSUS_LINE, W_SET + "\n" + W_CENSUS_LINE)],
           "the recognition sits a block below the movie's claim, no longer right after it"),
    wiring("iw-guard-gone", "guard", [(W_GUARD, "    if (panelCurveWants() && srv0IsPanelSized(s, kind, count)) {\n")], "the on-foot recognition claims the intro composite again"),
    wiring("iw-guard-before-the-gate", "guard",
           [(W_GUARD, "    if (!(kind == 'X' && count == 6 && bindingShaderHash(BindSlot::Vs) == kIntroCompositeVsHash) && panelCurveWants() && srv0IsPanelSized(s, kind, count)) {\n")],
           "the guard is read before panelCurveWants(): curvature 0 reads something new"),
    wiring("iw-gate-unlisted", "gate", [(W_GATE, "        vscreenFootprintWanted();   // the footprint instrument (vscreen_footprint.h): it reads the 2D screen's composite\n        (")],
           "the draw gate does not list the recogniser"),
    wiring("iw-site-before-the-verdict-begin", "strip-site", [(W_BEGIN, ""), (W_OBSERVED, W_BEGIN + W_OBSERVED)],
           "the verdict's Begin runs after the strip: kBackdrop's slot swap is not in place for it"),
    wiring("iw-site-after-the-game-draw", "strip-site", [(W_OBSERVED, ""), (W_STRIP_START, W_OBSERVED + W_STRIP_START)],
           "the strip is drawn after the game's own issue"),
    wiring("iw-site-dim-is-flat", "strip-site", [(W_DIM, "            draw(AlteredDrawClass::None);\n")], "the splash dim does not follow the strip"),
    wiring("iw-site-third-call", "strip-site", [(W_END, W_END + "    panelCurveSurfaceDraw(self, 1.0f, 1, false, g_state->realDrawIndexedInstanced);\n")],
           "a third place draws the strip"),
    wiring("iw-site-dim-drops-reverse", "strip-site", [(W_DIM_CALL, "panelCurveSurfaceDraw(self, stripGain, stripToward, g_state->realDrawIndexedInstanced))) {")],
           "the splash dim's re-issue of the strip drops the u direction"),
    wiring("iw-site-dim-hardcodes-reverse", "strip-site", [(W_DIM_CALL, "panelCurveSurfaceDraw(self, stripGain, stripToward, false, g_state->realDrawIndexedInstanced))) {")],
           "the splash dim's re-issue of the strip passes a constant u direction"),
    wiring("iw-site-reverse-starts-true", "strip-site", [(W_REVERSE_DECL, "    bool stripReverseU = true;\n")], "an unarmed draw's u direction starts as against x"),
    wiring("iw-unarmed-early-return-first", "unarmed-text",
           [(W_REFUSAL, "        if (stripIssued) return true;   // the strip was issued in its place: nothing more, and something was\n"
                        "        if (owner && uiLayerIssueBlocked()) return false;\n")],
           "observedDraw answers for the strip before its own refusal"),
    wiring("iw-unarmed-no-early-return", "unarmed-text", [("        if (stripIssued) return true;   // the strip was issued in its place: nothing more, and something was\n", "")],
           "the game's own draw is issued a second time over the strip"),
    wiring("iw-unarmed-original-issued", "unarmed-text",
           [("const bool originalIssued=observedDraw(alteredClass", "const bool originalIssued = stripIssued || observedDraw(alteredClass")],
           "originalIssued is no longer observedDraw's answer"),
    wiring("iw-unarmed-dim-ungated", "unarmed-text",
           [("if (!(stripIssued && panelCurveSurfaceDraw(self, stripGain, stripToward, stripReverseU, g_state->realDrawIndexedInstanced))) {",
             "if (!(panelCurveSurfaceDraw(self, stripGain, stripToward, stripReverseU, g_state->realDrawIndexedInstanced))) {")],
           "the dim draws the strip even when the main draw's strip did not draw"),
    wiring("iw-candidate-asks-first", "candidate",
           [(W_CANDIDATE, "if (panelCurveSurfaceWanted() && (v == DrawVerdict::kIntroPanel || g_state->introCurveThisDraw) && owner) {")],
           "every draw pays the cross-TU question"),
    wiring("iw-candidate-every-draw", "candidate", [(W_CANDIDATE, "if (owner && panelCurveSurfaceWanted()) {")], "every draw is a candidate for the strip"),
    wiring("iw-args-movie-gain", "strip-args", [("stripGain = introPanelStripGain();", "stripGain = introCurveGain();")], "the movie is drawn at the splash's gain"),
    wiring("iw-args-movie-direction", "strip-args", [("                stripToward = 1;\n", "                stripToward = -1;\n")], "the movie bends the other way"),
    wiring("iw-args-splash-direction", "strip-args", [("stripToward = introCurveToward();", "stripToward = 1;")], "the splash's direction is a constant"),
    wiring("iw-args-movie-reverse-constant", "strip-args", [("stripReverseU = introPanelStripReverseU();", "stripReverseU = true;")],
           "the movie passes a constant u direction, whatever its placement says"),
    wiring("iw-args-splash-reverse-constant", "strip-args", [("stripReverseU = introCurveReverseU();", "stripReverseU = true;")],
           "the splash passes a constant u direction, whatever its constants say"),
    wiring("iw-args-splash-reverse-is-the-movies", "strip-args", [("stripReverseU = introCurveReverseU();", "stripReverseU = introPanelStripReverseU();")],
           "the splash passes the movie's u direction"),
    wiring("iw-args-movie-reverse-is-the-splashs", "strip-args", [(W_MOVIE_REVERSE, "                stripReverseU = introCurveReverseU();\n")],
           "the movie passes the splash's u direction"),
    wiring("iw-args-movie-reverse-before-the-arm-test", "strip-args", [(W_MOVIE_REVERSE, ""), (W_MOVIE_ARMED, "            stripReverseU = introPanelStripReverseU();\n" + W_MOVIE_ARMED)],
           "the movie's u direction is read before its strip is armed (it is valid only while armed)"),
    wiring("iw-args-call-drops-reverse", "strip-args", [(W_STRIP_CALL, "stripIssued = panelCurveSurfaceDraw(self, stripGain, stripToward, g_state->realDrawIndexedInstanced);")],
           "the strip's main call passes no u direction"),
    wiring("iw-args-call-hardcodes-reverse", "strip-args",
           [(W_STRIP_CALL, "stripIssued = panelCurveSurfaceDraw(self, stripGain, stripToward, false, g_state->realDrawIndexedInstanced);")],
           "the strip's main call passes a constant u direction"),
    wiring("iw-args-no-draw-function", "strip-args", [(W_STRIP_CALL, "stripIssued = panelCurveSurfaceDraw(self, stripGain, stripToward, stripReverseU, nullptr);")],
           "the strip is drawn through no draw function"),
    wiring("iw-args-past-the-refusal", "strip-args", [("if (stripToward != 0 && !uiLayerIssueBlocked()) {", "if (stripToward != 0) {")],
           "the strip is asked past observedDraw's own refusal"),
    wiring("iw-fallback-always-issued", "fallback",
           [(W_STRIP_CALL, "panelCurveSurfaceDraw(self, stripGain, stripToward, stripReverseU, g_state->realDrawIndexedInstanced);\n            stripIssued = true;")],
           "the strip counts as issued whatever it returned: the game's quad is never drawn"),
    wiring("iw-fallback-starts-true", "fallback", [("bool stripIssued = false;", "bool stripIssued = true;")], "every draw starts as the strip's"),
    wiring("iw-onfoot-flag-rides", "not-on-foot-flag",
           [("        s->introCurveThisDraw = introCurveOnComposite(self, kind, count, instances);\n",
             "        s->introCurveThisDraw = introCurveOnComposite(self, kind, count, instances);\n        s->curveThisDraw = s->introCurveThisDraw;\n")],
           "the splash's strip rides the on-foot flag, whose branch returns before the verdict's Begin and the dim"),
    wiring("iw-onfoot-flag-read", "not-on-foot-flag",
           [("(v == DrawVerdict::kIntroPanel || g_state->introCurveThisDraw) && owner",
             "(v == DrawVerdict::kIntroPanel || g_state->introCurveThisDraw || g_state->curveThisDraw) && owner")],
           "the strip's site also reads the on-foot flag"),
    wiring("iw-tick-missing", "tick", [(W_TICK, "")], "the recogniser is never ticked: it never retires"),
    wiring("iw-tick-no-scene", "tick", [("introCurveTick(g_state->ownerCtx, sceneFrame);", "introCurveTick(g_state->ownerCtx, false);")], "the tick is told no scene"),
    wiring("iw-tick-before-the-movie", "tick", [(W_TICK, ""), ("        tkIntroPanel.run([&] {\n", W_TICK + "        tkIntroPanel.run([&] {\n")],
           "the tick runs before the movie's"),
    wiring("iw-tick-line-every-frame", "tick", [("if (sceneFrame) introCurveNoteRetired();", "introCurveNoteRetired();")], "the retirement line is called every frame"),
    wiring("iw-shutdown-missing", "shutdown", [("    introPanelShutdown();\n    introCurveShutdown();\n", "    introPanelShutdown();\n")],
           "the shutdown never releases the recogniser's staging buffers"),
    wiring("iw-retire-reworded", "retire-line", [(W_RETIRE_TEXT, "in all (the movie's and the splash's together)")],
           "the retirement line is reworded: it no longer says the dim's re-issues are counted"),
    wiring("iw-retire-old-text", "retire-line",
           [("\"intro curve: %llu strip draw(s) in all (the movie's, the splash's and the splash dim's re-issues of either); the splash recogniser handed over %llu composite draw(s)\",",
             "\"intro curve: strip draws %llu (the movie's and the splash's together), %llu of them the splash's\",")],
           "the retirement line says the old text again: the splash's count is called a share of the strip's total"),
    wiring("iw-retire-every-time", "retire-line", [("    if (said) return;\n    said = true;\n", "")], "the retirement line is said every time it is asked"),
    wiring("iw-retire-at-curvature-0", "retire-line", [("    if (drawn == 0 && !panelCurveSurfaceWanted()) return;\n", "")],
           "the retirement line is said at curvature 0 with nothing drawn"),
    wiring("iw-retire-swapped-counts", "retire-line",
           [("static_cast<unsigned long long>(drawn), static_cast<unsigned long long>(introCurveInfo().armed));",
             "static_cast<unsigned long long>(introCurveInfo().armed), static_cast<unsigned long long>(drawn));")],
           "the retirement line swaps the strip's total and the splash's count"),
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
    return Path(path).read_bytes().decode("utf-8").replace("\r\n", "\n")


def failing_labels(output):
    """The labels of the rigs' failing checks: '  FAIL  <label>' (the route's rigs) or 'FAIL: <label>' (the layer's)."""
    found = []
    for line in output.splitlines():
        if line.startswith("  FAIL  "):
            found.append(line[len("  FAIL  "):].strip())
        elif line.startswith("FAIL: "):
            found.append(line[len("FAIL: "):].strip())
    return found


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
    """Every file under the repo that `first_files` include with quotes, transitively, plus the files themselves."""
    seen, todo = set(), [Path(p) for p in first_files]
    while todo:
        path = todo.pop().resolve()
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


# ---- the toolchain -----------------------------------------------------------------------------------------------------------------------
class Toolchain:
    """cl.exe by absolute path, with the environment it needs: this one if cl is on PATH, else the one vcvars64.bat makes, found with
    vswhere as build.bat does."""

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


def run_rig_exe(tc, run_cmd, cwd):
    """('pass'|'fail'|'crash'|'timeout', detail) of one run of a built rig; a 'fail' detail is the failing labels, joined with ' | '."""
    try:
        run = subprocess.run(run_cmd, capture_output=True, text=True, env=tc.env, errors="replace", timeout=RUN_TIMEOUT, cwd=str(cwd))
    except subprocess.TimeoutExpired:
        return "timeout", "no result within %d s" % RUN_TIMEOUT
    out = run.stdout + "\n" + run.stderr
    if run.returncode == 0:
        return "pass", ""
    labels = failing_labels(out)
    if not labels:
        return "crash", "exit 0x%08X" % (run.returncode & 0xFFFFFFFF)
    return "fail", " | ".join(labels)


def build_and_run_wiring(tc, tree, edited_text):
    """The wiring rig: ui_quality_test built ONCE (by the control, in its own tree, from the real sources, the way build.bat builds it) and run
    with --wiring in `tree`, whose src/d3d11 holds `edited_text` as vscreen.cpp beside unedited copies of the other files the scans read."""
    spec = RIGS["wiring"]
    tree.mkdir(parents=True, exist_ok=True)
    exe = WIRING_EXE.get("exe")
    if exe is None:
        if not GEN.is_dir():
            return "nocompile", "build\\gen is missing: run one build first"
        exe = tree / "rig.exe"
        cmd = [tc.cl] + CL_FLAGS + ["/I" + str(GEN), "/Fo" + str(tree) + os.sep, "/Fe" + str(exe)] + [str(ROOT / s) for s in spec["sources"]] + \
              ["/link", "/INCREMENTAL:NO"] + spec["libs"]
        done = subprocess.run(cmd, capture_output=True, text=True, env=tc.env, errors="replace", cwd=str(ROOT))
        if done.returncode != 0:
            errors = [l.strip() for l in (done.stdout + done.stderr).splitlines() if "error" in l]
            return "nocompile", " | ".join(errors[:8])[:1500]
        WIRING_EXE["exe"] = exe
    for rel in spec["reads"]:
        dest = tree / rel
        dest.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(ROOT / rel, dest)
    target = tree / spec["target"].relative_to(ROOT)
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text(edited_text, encoding="utf-8", newline="\n")
    return run_rig_exe(tc, [str(exe), "--wiring"], tree)


def build_and_run(tc, rig_key, tree, edited_text):
    """('nocompile'|'pass'|'fail'|'crash'|'timeout', detail): the rig built in `tree` with `edited_text` in place of its mutable file (the
    unedited text for the control) and run. A 'fail' detail is the failing labels, joined with ' | '."""
    if rig_key == "wiring":
        return build_and_run_wiring(tc, tree, edited_text)
    spec = RIGS[rig_key]
    tree.mkdir(parents=True, exist_ok=True)
    exe = tree / "rig.exe"
    if rig_key == "pure":
        # The rig and its include closure, mirrored under the temp directory, so its relative includes find the edited header.
        rig_dest = tree / "tools" / "vr_world_route_test" / "vr_world_route_test.cpp"
        rig_dest.parent.mkdir(parents=True, exist_ok=True)
        for src in include_closure([spec["rig"]]):
            dest = tree / src.relative_to(ROOT)
            dest.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(src, dest)
        (tree / spec["target"].relative_to(ROOT)).write_text(edited_text, encoding="utf-8", newline="\n")
        sources = [str(rig_dest)]
        extra = []
        run_cmd = [str(exe), "--self-test", str(ROOT)]
    else:
        if not GEN.is_dir():
            return "nocompile", "build\\gen is missing: run one build first"
        edited = tree / spec["target"].name
        edited.write_text(edited_text, encoding="utf-8", newline="\n")
        sources = [str(edited) if Path(s).name == spec["target"].name else str(ROOT / s) for s in spec["sources"]]
        extra = ["/I" + str(GEN), "/I" + str(SRC)]
        run_cmd = [str(exe), "--self-test"]
    cmd = [tc.cl] + CL_FLAGS + spec.get("flags", []) + extra + ["/Fo" + str(tree) + os.sep, "/Fe" + str(exe)] + sources + \
          ["/link", "/INCREMENTAL:NO"] + spec["libs"]
    done = subprocess.run(cmd, capture_output=True, text=True, env=tc.env, errors="replace", cwd=str(ROOT))
    if done.returncode != 0:
        errors = [l.strip() for l in (done.stdout + done.stderr).splitlines() if "error" in l]
        return "nocompile", " | ".join(errors[:8])[:1500]
    try:
        run = subprocess.run(run_cmd, capture_output=True, text=True, env=tc.env, errors="replace", timeout=RUN_TIMEOUT, cwd=str(ROOT))
    except subprocess.TimeoutExpired:
        return "timeout", "no result within %d s" % RUN_TIMEOUT
    out = run.stdout + "\n" + run.stderr
    if run.returncode == 0:
        return "pass", ""
    labels = failing_labels(out)
    if not labels:
        return "crash", "exit 0x%08X" % (run.returncode & 0xFFFFFFFF)
    return "fail", " | ".join(labels)


# ---- the run -------------------------------------------------------------------------------------------------------------------------------
def select(only, rig):
    chosen = MUTANTS
    if rig:
        chosen = [m for m in chosen if m.rig == rig]
    if only:
        wanted = [n for n in only.split(",") if n]
        unknown = [n for n in wanted if n not in {m.name for m in MUTANTS}]
        if unknown:
            raise ValueError("no such mutation: " + ", ".join(unknown))
        chosen = [m for m in chosen if m.name in wanted]
    return chosen


def plan_text(mutants):
    lines = ["%d mutation(s); each is built in a temp directory outside the repo against one edited production file and run:" % len(mutants)]
    for m in mutants:
        lines.append("  %-28s %-5s %s -- %s %r" % (m.name, m.rig, m.why, "does not compile: the static check says" if m.compile_error else "fails on a check labelled", m.text))
    lines.append("cl.exe flags: " + " ".join(CL_FLAGS))
    return "\n".join(lines)


def run_all(only=None, rig=None, jobs=None, keep=False, dry_run=False, out=sys.stdout):
    mutants = select(only, rig)
    if dry_run:
        print(plan_text(mutants), file=out)
        print("dry-run: nothing compiled, started or written", file=out)
        return 0
    tc = Toolchain()
    work = Path(tempfile.mkdtemp(prefix="vr_world_route_mutants_"))
    sources = {key: read_source(spec["target"]) for key, spec in RIGS.items()}
    try:
        used = sorted({m.rig for m in mutants})
        with concurrent.futures.ThreadPoolExecutor(max_workers=max(1, len(used))) as pool:
            controls = list(pool.map(lambda k: (k, build_and_run(tc, k, work / ("control_" + k), sources[k])), used))
        for key, (outcome, detail) in controls:
            print("control, rig %s (the unedited source): %s %s" % (key, outcome, detail[:150]), file=out)
        if any(outcome != "pass" for _, (outcome, _) in controls):
            print("a rig does not pass on the unedited source when built this way; nothing below means anything", file=out)
            return 1

        def one(m):
            try:
                text = apply_edits(sources[m.rig], m.edits, m.name)
            except ValueError as error:
                return m, "badedit", str(error)
            outcome, detail = build_and_run(tc, m.rig, work / m.name, text)
            return m, outcome, detail

        results = []
        with concurrent.futures.ThreadPoolExecutor(max_workers=jobs or min(4, os.cpu_count() or 2)) as pool:
            for m, outcome, detail in pool.map(one, mutants):
                if m.compile_error and outcome == "nocompile" and m.text in detail:
                    verdict = "caught"   # a static check in the source refused the edit, in its own words
                elif m.compile_error and outcome == "nocompile":
                    verdict = "OTHER"    # it did not compile, but not because of the check
                elif outcome == "fail" and any(m.text in label for label in detail.split(" | ")):
                    verdict = "caught"
                elif outcome == "fail":
                    verdict = "OTHER"
                else:
                    verdict = {"pass": "SURVIVED", "crash": "CRASH", "timeout": "TIMEOUT", "nocompile": "NOCOMPILE", "badedit": "BADEDIT"}[outcome]
                results.append((m, verdict, detail))
                print("%-9s %-28s %-5s %s" % (verdict, m.name, m.rig, detail[:140]), file=out)
        bad = [r for r in results if r[1] != "caught"]
        print("%d mutation(s): %d caught by a check or a static check that names them, %d not" % (len(results), len(results) - len(bad), len(bad)), file=out)
        for m, verdict, detail in bad:
            print("  NOT CAUGHT: %s (%s): wanted %s %r, got %s %s" % (m.name, m.why, "a compile error saying" if m.compile_error else "a failing check labelled", m.text,
                                                                     verdict, detail[:160]), file=out)
        return 0 if not bad else 1
    finally:
        if keep:
            print("kept: %s" % work, file=out)
        else:
            shutil.rmtree(work, ignore_errors=True)


# ---- the self-test -----------------------------------------------------------------------------------------------------------------------------
def self_test():
    failures = []

    def check(ok, what):
        if not ok:
            failures.append(what)

    # the tool's own pieces
    check(failing_labels("  ok    a\n  FAIL  b: c\n  FAIL  d\nFAIL: e\nx FAIL: not a line start\n3 checks FAILED\n") == ["b: c", "d", "e"],
          "failing_labels reads the rigs' FAIL lines, both spellings, and only at the start of a line")
    check(apply_edits("a b c", [("b", "B")]) == "a B c", "apply_edits applies one edit")
    for bad, why in ((("a b b", [("b", "B")]), "a repeated anchor"), (("a b", [("x", "y")]), "a missing anchor"), (("a b", [("b", "b")]), "a no-op edit")):
        try:
            apply_edits(*bad)
            check(False, "apply_edits refuses " + why)
        except ValueError:
            pass
    joined = label_block(":rig_a\nx ^\n y\nexit /b 0\n:rig_b\nz\n", ":rig_a")
    check(joined is not None and [" ".join(l.split()) for l in joined] == ["x y", "exit /b 0"], "label_block joins continuations and stops at the next label")

    # every mutation against the file it edits as it is now, and against its rig
    sources = {key: read_source(spec["target"]) for key, spec in RIGS.items()}
    rigs = {key: read_source(spec["rig"]) + "".join(read_source(p) for p in spec.get("labels", [])) for key, spec in RIGS.items()}
    names = [m.name for m in MUTANTS]
    check(len(names) == len(set(names)), "mutation names are unique")
    check(len(MUTANTS) >= 75, "the mutation list did not shrink (%d)" % len(MUTANTS))
    # the wiring rig: the rig has the --wiring mode this tool runs, includes the header its labels live in, and the files it scans exist
    wiring_spec = RIGS["wiring"]
    check('"--wiring"' in read_source(wiring_spec["rig"]) and '#include "ui_intro_curve_wiring_test.h"' in read_source(wiring_spec["rig"]),
          "ui_quality_test has the --wiring mode and includes ui_intro_curve_wiring_test.h")
    for rel in wiring_spec["reads"]:
        check((ROOT / rel).is_file(), "the wiring rig scans %s, which exists" % rel)
    for m in MUTANTS:
        check(m.rig in RIGS, "%s names a rig this tool knows" % m.name)
        if m.rig not in RIGS:
            continue
        try:
            mutated = apply_edits(sources[m.rig], m.edits, m.name)
            check(mutated != sources[m.rig], "%s changes the source" % m.name)
        except ValueError as error:
            failures.append(str(error))
        if m.compile_error:   # the source's own static_assert message (the production file says it, not the rig)
            check(m.text in sources[m.rig], "%s: %s has no static check that says %r" % (m.name, RIGS[m.rig]["target"].name, m.text))
        else:
            check(m.text in rigs[m.rig], "%s: rig %s has no check labelled %r" % (m.name, m.rig, m.text))
    for key in RIGS:
        check(any(m.rig == key for m in MUTANTS), "rig %s has a mutation" % key)

    # build.bat compiles each rig the way this tool does, and runs this tool's self-test
    bat = BUILD_BAT.read_bytes().decode("utf-8", errors="replace")
    for key, spec in RIGS.items():
        block = label_block(bat, spec["label"])
        check(block is not None, "build.bat has %s" % spec["label"])
        if not block:
            continue
        cl = next((l for l in block if l.strip().lower().startswith("cl.exe")), "")
        for flag in CL_FLAGS + spec.get("flags", []):
            check(flag in cl, "build.bat's %s compile line lacks %s" % (spec["label"], flag))
        for src in spec["sources"]:
            check('"%s"' % src in cl, "build.bat's %s compile line lacks %s" % (spec["label"], src))
        for lib in spec["libs"]:
            check(lib in cl, "build.bat's %s compile line lacks %s" % (spec["label"], lib))
        check(('/I"%GEN%"' in cl) == spec["include_gen"], "build.bat's %s compile line and this tool disagree about /I\"%%GEN%%\"" % spec["label"])
    block = label_block(bat, RIGS["pure"]["label"])
    check(block is not None and any("tools\\vr_world_route_test\\mutants.py" in l and "--self-test" in l for l in block),
          "build.bat's %s runs this tool's --self-test" % RIGS["pure"]["label"])

    if failures:
        for f in failures:
            print("FAIL: " + f)
        return 1
    print("PASS: vr_world_route_test mutants.py self-test (%d mutations over %d rigs, every anchor found once, build.bat wired)" % (len(MUTANTS), len(RIGS)))
    return 0


def main(argv):
    ap = argparse.ArgumentParser(description="mutation proof for the VR world route's rigs")
    ap.add_argument("--self-test", action="store_true")
    ap.add_argument("--run", action="store_true")
    ap.add_argument("--list", action="store_true")
    ap.add_argument("--rig", choices=sorted(RIGS))
    ap.add_argument("--only", default="")
    ap.add_argument("--jobs", type=int, default=0)
    ap.add_argument("--keep", action="store_true")
    ap.add_argument("--dry-run", action="store_true")
    a = ap.parse_args(argv)
    if a.self_test:
        return self_test()
    if a.list:
        print(plan_text(select(a.only, a.rig)))
        return 0
    if a.run:
        return run_all(a.only, a.rig, a.jobs or None, a.keep, a.dry_run)
    ap.print_help()
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
