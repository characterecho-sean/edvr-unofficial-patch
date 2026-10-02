#!/usr/bin/env python3
"""The mutation proof for tools\\intro_curve_module_test: the rig fails when a rule of the splash's recogniser is flipped.

The rig (intro_curve_module_test.cpp) runs the REAL src\\d3d11\\intro_curve.cpp on a WARP device, in seventeen cases C1..C17 (its header says
which); every check it makes carries a label "C<case>.<what>". A rig that passes proves little until it is seen to FAIL on a module that
breaks the rule it pins. This tool does that: for each mutation below it copies intro_curve.cpp (and intro_curve.h where the rule lives
there, and intro_curve_math.h for a mutation of the pure reading the module is built on) into a temp directory OUTSIDE the repo, applies one
textual edit (or a few that belong together), compiles the module (and the rig, when intro_curve.h changed) against that copy, links the rig
with the unmutated common sources, runs the rule's case alone, and requires the
rig to fail on a check of the case that belongs to the rule (a FAIL label starting with the case's id and a dot: "C7." and not "C70."). A
mutation whose case raises a fault on purpose may also count a crash as caught (crash_ok). Nothing is written inside the repo; the temp
directory is removed at the end.

  python tools\\intro_curve_module_test\\mutants.py --self-test       text only: every anchor is found exactly once in the file it edits as it
                                                                     is now, every case named is in the rig, and build.bat compiles the rig
                                                                     the way this tool does (add --build-bat PATH to check another copy)
  python tools\\intro_curve_module_test\\mutants.py --run [--only a,b] [--jobs N] [--keep] [--verbose]
  python tools\\intro_curve_module_test\\mutants.py --list
  python tools\\intro_curve_module_test\\mutants.py --run --dry-run   the plan; writes nothing, starts nothing

--run needs the MSVC toolchain (cl.exe on PATH, or Visual Studio found with vswhere as build.bat does); build\\gen is added to the include path
when there is one, as the rig's build.bat line does. It takes a minute or two: the four common sources and the rig are compiled once, then each
mutation compiles one file (two for a header) and links. --self-test runs in build.bat's rig and is what keeps an edit of the module from
silently orphaning a mutation: if an anchor stops matching, the build fails and this file says which.

Not mutated, because the rig cannot see the difference: the Unmap after a successful Map (WARP lets a buffer be released while mapped), the
two flags that only stop work being done twice (the module's own stand-down line, the per-tick release after a fault), and the line a
re-read says when its COPY cannot be made for a reason of another class than the pair's first (startReread's noteStillFlat: a pair's buffer
cannot shrink and WARP will not refuse a staging buffer, so only the same-class silence of a buffer too small to copy is reachable).
"""
import argparse
import concurrent.futures
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
SRC = ROOT / "src" / "d3d11"
FILES = {"cpp": SRC / "intro_curve.cpp", "h": SRC / "intro_curve.h", "math": SRC / "intro_curve_math.h"}
RIG = HERE / "intro_curve_module_test.cpp"
BUILD_BAT = ROOT / "build.bat"
RIG_LABEL = ":rig_intro_curve_module_test"
LIBS = ["user32.lib", "version.lib"]

# The strip the rig draws with: the real src\d3d11\panel_curve.cpp linked in (True: the rig is built with -DINTRO_CURVE_RIG_REAL_STRIP), or its
# own inert doubles of panelCurveSurfaceWanted / Draw / Info and the variables panelCurveInfo() reads (False). build.bat's label says which;
# --self-test checks that it says the same as this. `--strip real|doubles` overrides it for one run of this tool.
REAL_STRIP = True
SOURCES = ["config.cpp", "log.cpp", "guard.cpp", "proxy.cpp"]

# How build.bat compiles the rig; --self-test checks that the label still says the same.
BASE_FLAGS = ["/nologo", "/O2", "/MT", "/std:c++17", "/EHsc", "/W4", "/DWIN32_LEAN_AND_MEAN", "/DNOMINMAX", "/D_CRT_SECURE_NO_WARNINGS", "/DEDVR_INTRO_CURVE_RIG"]
CL_FLAGS = list(BASE_FLAGS)
COMMON = [ROOT / "src" / "common" / n for n in SOURCES]
RUN_TIMEOUT = 120.0


def set_real_strip(on):
    """Flip the rig between the strip's doubles and the real panel_curve.cpp: the flag it is built with, and the source linked."""
    global REAL_STRIP, CL_FLAGS, COMMON
    REAL_STRIP = on
    CL_FLAGS = BASE_FLAGS + (["/DINTRO_CURVE_RIG_REAL_STRIP"] if on else [])
    COMMON = ([SRC / "panel_curve.cpp"] if on else []) + [ROOT / "src" / "common" / n for n in SOURCES]


set_real_strip(REAL_STRIP)


class Mutant:
    def __init__(self, name, caught, edits, why, file="cpp", crash_ok=False):
        self.name = name
        self.caught = tuple([caught] if isinstance(caught, str) else caught)   # case ids whose checks must report it
        self.edits = list(edits)                                                # (old, new) pairs, applied in order, to `file`
        self.why = why
        self.file = file                                                        # "cpp", "h" or "math" (intro_curve_math.h)
        self.crash_ok = crash_ok                                                # a crash of the rig counts as caught
        self.rule = self.caught[0]                                              # the case run for it


def M(name, caught, edits, why, file="cpp", crash_ok=False):
    return Mutant(name, caught, edits, why, file, crash_ok)


def drop(old):
    return [(old, "")]


# ---- the module's lines, verbatim (the self-test finds each exactly once) ------------------------------------------------------
KVS = "constexpr uint32_t kVsSlot = 2;"
KSETTLE = "constexpr uint32_t kSettleFrames = 4;"
KTABLE = "constexpr uint32_t kMaxEntries = 16;"
KEXPIRE = "constexpr uint32_t kExpireFrames = 180;"
KRECHECK = "constexpr uint32_t kFlatRecheckFrames = 60;"
KLINES = "constexpr uint32_t kMaxFlatLines = 12;"
KBUDGET = "constexpr int kFaultBudget = 1;"

STAGE_RELEASE = "        e.stage->Release();\n"
STAGE_NULL = "        e.stage->Release();\n        e.stage = nullptr;"
CLEAR_ENTRY = "void clearEntry(Entry& e) {\n    dropStage(e);\n    e = Entry();\n}"
RELEASE_ALL = "    for (Entry& e : g_entry) clearEntry(e);\n"
STAND_DOWN_LINE = '    Log::get().note("splash curve: a fault stood this down for the session -- every composite is drawn as the game drew it.");'
FAULT_RELEASE = '    guarded("introCurve.release", [] { releaseAll(); });\n'

LEARN_TEST = "    if (g_flatLines < kMaxFlatLines) {"
LEARN_COUNT = "        ++g_flatLines;\n"
CAP_NOTED = "        g_capNoted = true;\n"
CAP_TEST = "    if (!g_capNoted) {"
CAP_TEXT = "lines about composites left as the game drew them are written; the rest are not"

FORMAT = '"%s%.6g"'
GROUP = "i % 4 == 0"
FORMAT_LOOP = "i < kCbFloats && at + 1 < size"
FORMAT_CALL = "        formatCb(f, text, sizeof(text));\n"

FLAT_STATE = "    e.state = State::kFlat;\n"
FLAT_COUNT = "    ++g_learnedFlat;\n"
FLAT_FLOATS = "    if (f) {"
FLAT_LINE = '        Log::get().note("splash curve: %s stays as the game drew it, because %s. Its cb2: %s.", lead, why, text);'
FLAT_TEXT = "stays as the game drew it, because %s. Its cb2: %s."

WORLD_GAIN = "    e.halfWidth = w.halfWidth;\n"
WORLD_TOWARD = "    e.toward = w.toward;\n"
WORLD_COUNT = "    ++g_learnedWorld;\n"
WORLD_STATE = "    e.state = State::kWorld;\n"
FLAT_CAP = "    if (!flatLineAllowed()) return;\n    if (f) {"
WORLD_STRIP = "    const PanelCurveInfo strip = panelCurveInfo();\n"
WORLD_VS = "static_cast<unsigned>(kIntroCompositeVsHash >> 32)"
WORLD_SIGN = "depth toward the viewer %+d "
WORLD_WIDTH = "half-width %.3f m"
WORLD_TEXT = "reads as a world-space panel"

FIND = "e.state != State::kFree && e.buffer == buffer && e.surface == surface"
CLAIM_FREE = "        if (e.state == State::kFree) {\n            clearEntry(e);"
CLAIM_PICK = "if (!oldest || e.lastSeen < oldest->lastSeen || (e.lastSeen == oldest->lastSeen && e.born < oldest->born)) oldest = &e;"
EVICT_COUNT = "    ++g_evicted;\n"
EVICT_NOTED = "        g_evictNoted = true;\n"
EVICT_TEST = "    if (!g_evictNoted) {"
EVICT_CLEAR = "    clearEntry(*oldest);\n    return oldest;"

COPY_SIZE = "    if (bd.ByteWidth < kCbBytes) {"
STAGE_USAGE = "    sd.Usage = D3D11_USAGE_STAGING;\n"
STAGE_CPU = "    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;\n"
DEV_RELEASE = "    dev->Release();\n"
BOX_RIGHT = "    box.right = kCbBytes;\n"
BOX_REST = "    box.bottom = 1;\n    box.back = 1;\n"
COPY_CALL = "    ctx->CopySubresourceRegion(e.stage, 0, 0, 0, 0, static_cast<ID3D11Resource*>(cb), 0, &box);\n"
COPY_STATE = "        e.state = State::kCopying;\n"
COPY_DUE = "    e.dueFrame = g_frame + kSettleFrames;\n"
COPY_READFRAME = "    e.readFrame = g_frame;\n"

MAP_READ = "D3D11_MAP_READ"
READ_COPY = "                std::memcpy(f, m.pData, sizeof(f));\n"
READ_HAVE = "                have = true;\n"
READ_DROP = "    dropStage(e);\n    if (reread) ++g_rereads;"
READ_UNREADABLE = ("    if (!have) {\n        if (reread) noteStillFlat(e, kWhyNoRead, kWhyNoRead, nullptr);\n"
                   "        else settleFlat(e, kWhyNoRead, kWhyNoRead, nullptr);\n        return;\n    }\n")
READ_UNREADABLE_LINE = ("        if (reread) noteStillFlat(e, kWhyNoRead, kWhyNoRead, nullptr);\n"
                        "        else settleFlat(e, kWhyNoRead, kWhyNoRead, nullptr);\n")
READ_JUDGE = "    if (!w.ok) {"
READ_WHY = "        else settleFlat(e, w.why, w.why, f);"
READ_WHY_AGAIN = "        if (reread) noteStillFlat(e, w.why, w.why, f);"
READ_TEXT = '"the copy of its constants could not be read back"'
READ_FLAG = "    const bool reread = e.state == State::kFlat;\n"
READ_COUNT = "    if (reread) ++g_rereads;\n"
READ_CHANGE = "    if (reread) ++g_flatToWorld;\n"

STILL_TEST = "    if (e.flatWhy && std::strcmp(e.flatWhy, cls) == 0) return;\n"
STILL_CLASS = "    e.flatWhy = cls;\n    flatLine(kLeadAgain, why, f);"
STILL_LINE = "    flatLine(kLeadAgain, why, f);\n"
LEAD_AGAIN = 'const char kLeadAgain[] = "this composite, read again,";'
STILL_UNCAPPED = ('    if (f) {\n        char text[512];\n        formatCb(f, text, sizeof(text));\n'
                  '        Log::get().note("splash curve: %s stays as the game drew it, because %s. Its cb2: %s.", kLeadAgain, why, text);\n'
                  '    } else {\n        Log::get().note("splash curve: %s stays as the game drew it, because %s.", kLeadAgain, why);\n    }\n')
REREAD_FAIL = "    if (!issueCopy(ctx, cb, e, fail)) noteStillFlat(e, fail.cls, fail.text, nullptr);\n"
REREAD_START = "        if (e->state == State::kFlat && !e->stage && g_frame - e->readFrame >= kFlatRecheckFrames) startReread(ctx, cb, *e);\n"

REC_NULL = "    if (!cb) return false;\n"
REC_SURFACE = "bindingGet(BindSlot::PsSrv0)"
REC_FOUND = "    if (e) {\n        e->lastSeen = g_frame;\n"
REC_BORN = "        e->born = ++g_born;\n"
REC_NEW_SEEN = "        e->lastSeen = g_frame;\n        startCopy(ctx, cb, *e);"
REC_COPY = "        startCopy(ctx, cb, *e);\n"
REC_RELEASE = "    cb->Release();\n    if (e->state != State::kWorld) return false;"
REC_ARM = "    if (e->state != State::kWorld) return false;"
REC_GAIN = "    g_armedGain = e->halfWidth;\n"
REC_TOWARD = "    g_armedToward = e->toward;\n"
REC_COUNT = "    ++g_armedDraws;\n"

SVC_FREE = "    for (Entry& e : g_entry) {\n        if (e.state == State::kFree) continue;\n        if (g_frame - e.lastSeen > kExpireFrames) {"
SVC_EXPIRE = "g_frame - e.lastSeen > kExpireFrames"
SVC_EXPIRED = "            ++g_expired;\n"
SVC_CLEAR = "            ++g_expired;\n            clearEntry(e);\n            continue;"
SVC_READ = "if (e.stage && g_frame >= e.dueFrame && ctx) readBack(ctx, e);"

RET_FLAG = "    g_retired = true;\n    g_armedGain = 0.0f;"
RET_RELEASE = '    guarded("introCurve.retire", [] { releaseAll(); });\n'
RET_FAULT = 'g_budget.shouldRun() ? "" : "; a fault had already stood it down"'
RET_LEARNED = "    if (g_learnedWorld + g_learnedFlat || g_armedDraws) {"
RET_WANTED = "    } else if (g_wantedSeen) {"
RET_WORLDS = "static_cast<unsigned long long>(g_learnedWorld + g_learnedFlat), static_cast<unsigned long long>(g_learnedWorld),"
RET_CLAUSE_TEST = "        if (g_rereads) {"
RET_CLAUSE_REREADS = "static_cast<unsigned long long>(g_rereads),"
RET_CLAUSE_CHANGES = "static_cast<unsigned long long>(g_flatToWorld));"

WANTS = "    return !g_retired && g_budget.shouldRun() && panelCurveSurfaceWanted();"

ONC_CLEAR = "    g_armedGain = 0.0f;\n    g_armedToward = 0;\n    g_armedReverseU = false;\n    if (!ctx || !introCurveWants()) return false;"
ONC_CLEAR_REVERSE = "    g_armedReverseU = false;\n    if (!ctx || !introCurveWants()) return false;"
ONC_GATE = "    if (!ctx || !introCurveWants()) return false;"
ONC_SHAPE = "    if (kind != 'X' || count != 6 || instances != 1) return false;"
ONC_HASH = "    if (bindingShaderHash(BindSlot::Vs) != kIntroCompositeVsHash) return false;\n"
ONC_GUARD = "    if (!guardedBudget(g_budget, [&] { armed = recognise(ctx); })) {\n        noteStandDown();\n        return false;\n    }"
ONC_RETURN = "    return armed;\n"

END_DRAW = "void introCurveEndDraw() {\n    g_armedGain = 0.0f;\n    g_armedToward = 0;\n    g_armedReverseU = false;\n}"

# The u direction: stored with the pair when it is read, handed over when it is armed.
ENTRY_REVERSE = "    e.reverseU = w.xDir == IntroXDir::kLeft;\n"
ARM_REVERSE = "    g_armedReverseU = e->reverseU;\n"
ARMED_REVERSE_DECL = "bool  g_armedReverseU = false;\n"
WORLD_DIRECTION = "+x runs to the viewer's %s, so u runs %s x;"
WORLD_DIRECTION_ARGS = 'e.reverseU ? "left" : "right", e.reverseU ? "against" : "with"'
GETTER = "bool introCurveReverseU() {\n    return g_armedReverseU;\n}"

TICK_FRAME = "    ++g_frame;\n    if (g_retired) return;"
TICK_RETIRED = "    if (g_retired) return;\n    if (!g_wantedSeen && panelCurveSurfaceWanted()) g_wantedSeen = true;"
TICK_WANTED = "    if (!g_wantedSeen && panelCurveSurfaceWanted()) g_wantedSeen = true;\n"
TICK_SCENE = "    if (sceneFrame) {\n        retire();\n        return;\n    }\n"
TICK_DOWN = "    if (!g_budget.shouldRun()) {\n        releaseAfterFault();\n        return;\n    }\n"
TICK_SERVICE = "    if (!guardedBudget(g_budget, [&] { serviceEntries(ctx); })) noteStandDown();"
SHUTDOWN = '    guarded("introCurve.shutdown", [] { releaseAll(); });\n'

INFO_ENTRIES = "        if (e.state != State::kFree) ++info.entries;\n"
INFO_WORLDS = "        if (e.state == State::kWorld) ++info.worlds;\n        if (e.state == State::kFlat) ++info.flats;\n"
INFO_STAGING = "        if (e.stage) ++info.staging;\n"
INFO_COPYING = "        if (e.state == State::kCopying) ++info.copying;\n"
INFO_LEARNED = "    info.learned = g_learnedWorld + g_learnedFlat;\n"
INFO_REREADS = "    info.rereads = g_rereads;\n"
INFO_CHANGES = "    info.flatToWorld = g_flatToWorld;\n"
INFO_ARMED = "    info.armed = g_armedDraws;\n"
INFO_EVICTED = "    info.evicted = g_evicted;\n"
INFO_EXPIRED = "    info.expired = g_expired;\n"
INFO_DOWN = "    info.standDown = !g_budget.shouldRun();\n"
INFO_RETIRED = "    info.retired = g_retired;\n"
INFO_WANTED = "    info.wanted = introCurveWants();\n"

HASH = "constexpr uint64_t kIntroCompositeVsHash = 0xEF103A7CB4A8369Aull;"


MUTANTS = [
    # ---- C1: the two real captures: the slot, the settle, the numbers, the release ---------------------------------------------
    M("vs-slot-1", "C1", [(KVS, "constexpr uint32_t kVsSlot = 1;")], "the constants are read from slot 1, which holds the movie's stock"),
    M("vs-slot-3", "C1", [(KVS, "constexpr uint32_t kVsSlot = 3;")], "the constants are read from slot 3, which holds the movie's stock"),
    M("settle-3", "C1", [(KSETTLE, "constexpr uint32_t kSettleFrames = 3;")], "the copy is read a frame early"),
    M("settle-5", "C1", [(KSETTLE, "constexpr uint32_t kSettleFrames = 5;")], "the copy is read a frame late"),
    M("copy-due-now", "C1", [(COPY_DUE, "    e.dueFrame = g_frame;\n")], "the copy is due the frame it is made"),
    M("due-gt", "C1", [("g_frame >= e.dueFrame", "g_frame > e.dueFrame")], "the copy is read a tick after it is due"),
    M("copy-state-not-set", "C1", [(COPY_STATE, "")], "a pair with a copy in flight is not an entry"),
    M("copy-not-started", "C1", [(REC_COPY, "")], "a new pair is never copied"),
    M("copy-not-issued", "C1", [(COPY_CALL, "")], "the copy is never issued: the buffer is read as zeros"),
    M("copy-box-narrow", "C1", [(BOX_RIGHT, "    box.right = kCbBytes - 16;\n")], "cb2[4] is not copied"),
    M("copy-box-empty", "C1", [(BOX_REST, "")], "the copy's box has no height or depth"),
    M("stage-usage-default", "C1", [(STAGE_USAGE, "    sd.Usage = D3D11_USAGE_DEFAULT;\n")], "the readback buffer cannot be created"),
    M("stage-no-cpu-read", "C1", [(STAGE_CPU, "")], "the readback buffer cannot be read by the CPU"),
    M("device-ref-leaked", "C1", [(DEV_RELEASE, "")], "the device reference taken for the copy is never given back"),
    M("map-write", "C1", [(MAP_READ, "D3D11_MAP_WRITE")], "the readback is mapped for writing"),
    M("read-not-copied-out", "C1", [(READ_COPY, "")], "the mapped bytes are never copied out"),
    M("read-not-flagged", "C1", [(READ_HAVE, "")], "a read that worked is taken for one that did not"),
    M("read-stage-kept", "C1", [(READ_DROP, "    if (reread) ++g_rereads;")], "the readback buffer outlives its read"),
    M("stage-release-skipped", "C1", [(STAGE_RELEASE, "")], "the readback buffer's reference is never given up"),
    M("stage-pointer-kept", "C1", [(STAGE_NULL, "        e.stage->Release();")], "a released readback buffer is still held as one"),
    M("world-state-not-set", "C1", [(WORLD_STATE, "")], "a world reading leaves the pair copying"),
    M("info-copying-not-counted", "C1", [(INFO_COPYING, "")], "the pairs still being copied are not counted"),
    M("read-flat-always", "C1", [(READ_JUDGE, "    if (true) {")], "every reading is refused"),
    M("buffer-ref-leaked", "C1", [(REC_RELEASE, "    if (e->state != State::kWorld) return false;")], "the reference taken on the game's buffer is never given back"),
    M("arm-inverted", "C1", [(REC_ARM, "    if (e->state == State::kWorld) return false;")], "a world pair is the game's and everything else is armed"),
    M("arm-uncounted", "C1", [(REC_COUNT, "")], "an armed draw is not counted"),
    M("onc-never-armed", "C1", [(ONC_RETURN, "    return false;\n")], "the draw is never handed to the strip"),
    M("onc-kind-lower-x", "C5", [("kind != 'X'", "kind != 'x'")], "the composite's kind is a lower-case x"),
    M("world-gain-from-y", "C1", [(WORLD_GAIN, "    e.halfWidth = f[1];\n")], "the gain is the half-height"),
    M("world-uncounted", "C1", [(WORLD_COUNT, "")], "a world pair is not counted as learned"),
    M("world-line-vs-shifted", "C1", [(WORLD_VS, "static_cast<unsigned>(kIntroCompositeVsHash >> 16)")], "the line names the wrong half of the hash"),
    M("world-line-sign-unsigned", "C1", [(WORLD_SIGN, "depth toward the viewer %d ")], "the direction prints without its sign"),
    M("world-line-width-1-decimal", "C1", [(WORLD_WIDTH, "half-width %.1f m")], "the half-width prints to one decimal"),
    M("world-line-reworded", "C1", [(WORLD_TEXT, "is a world-space panel")], "the world line is reworded"),
    M("world-line-no-strip-settings", "C1", [(WORLD_STRIP, "    const PanelCurveInfo strip = PanelCurveInfo();\n")], "the line does not read the strip's columns and curvature"),
    M("info-staging-not-counted", "C1", [(INFO_STAGING, "")], "the readback buffers held are not counted"),
    M("info-entries-all", "C1", [(INFO_ENTRIES, "        ++info.entries;\n")], "every slot of the table is an entry"),
    M("info-worlds-flats-swapped", "C1", [(INFO_WORLDS, "        if (e.state == State::kFlat) ++info.worlds;\n        if (e.state == State::kWorld) ++info.flats;\n")],
      "the world and flat counts are swapped"),
    M("info-armed-not-reported", "C1", [(INFO_ARMED, "    info.armed = 0;\n")], "the armed count is not reported"),
    M("tick-no-frame", "C1", [(TICK_FRAME, "    if (g_retired) return;")], "the frame is never counted"),
    M("tick-service-skipped", "C1", [(TICK_SERVICE, "")], "the table is never serviced"),
    # ---- C2: flat verdicts -----------------------------------------------------------------------------------------------------
    M("flat-state-world", "C2", [(FLAT_STATE, "    e.state = State::kWorld;\n")], "a flat verdict arms the pair"),
    M("flat-no-floats", "C2", [(FLAT_FLOATS, "    if (false) {")], "the flat line omits the 20 floats"),
    M("flat-line-reworded", "C2", [(FLAT_TEXT, "stays as the game drew it, because %s. cb2: %s.")], "the flat line is reworded"),
    M("flat-line-dropped", "C2", [(FLAT_LINE, "        (void)text;")], "the flat line is never written"),
    M("read-world-always", "C2", [(READ_JUDGE, "    if (false) {")], "every reading is a world-space panel"),
    M("read-why-lost", "C2", [(READ_WHY, '        else settleFlat(e, w.why, "unknown", f);')], "the flat line does not say why"),
    M("read-unreadable-not-flat", "C2", [(READ_UNREADABLE, "")], "a copy that cannot be read back is judged as zeros"),
    M("read-unreadable-reworded", "C2", [(READ_TEXT, '"no read"')], "the unreadable-copy line is reworded"),
    M("copy-size-le", "C2", [(COPY_SIZE, "    if (bd.ByteWidth <= kCbBytes) {")], "an 80-byte buffer is too small"),
    M("copy-size-unchecked", "C2", [(COPY_SIZE, "    if (false) {")], "a 64-byte buffer is copied as if it were 80", crash_ok=True),
    M("info-learned-world-only", "C2", [(INFO_LEARNED, "    info.learned = g_learnedWorld;\n")], "flat pairs are not counted as learned"),
    M("arm-flat-too", "C2", [(REC_ARM, "    if (e->state == State::kCopying) return false;")], "a flat pair is armed too"),
    # ---- C3: a new pair --------------------------------------------------------------------------------------------------------
    M("find-ignores-buffer", "C3", [(FIND, "e.state != State::kFree && e.surface == surface")], "a pair is its surface alone"),
    M("find-ignores-surface", "C3", [(FIND, "e.state != State::kFree && e.buffer == buffer")], "a pair is its buffer alone"),
    M("find-either", "C3", [(FIND, "e.state != State::kFree && (e.buffer == buffer || e.surface == surface)")], "a pair matches on either half"),
    M("surface-from-slot-1", "C3", [(REC_SURFACE, "bindingGet(BindSlot::PsSrv1)")], "the surface is read from the wrong slot"),
    # ---- C4: curvature 0 -------------------------------------------------------------------------------------------------------
    M("wants-ignores-strip", "C4", [(WANTS, "    return !g_retired && g_budget.shouldRun();")], "it is wanted at curvature 0"),
    M("onc-wants-unchecked", "C4", [(ONC_GATE, "    if (!ctx) return false;")], "a draw asked past the gate is learned at curvature 0"),
    M("info-wanted-true", "C4", [(INFO_WANTED, "    info.wanted = true;\n")], "the info says it is wanted at curvature 0"),
    # ---- C5: the shape ---------------------------------------------------------------------------------------------------------
    M("onc-kind-unchecked", "C5", [(ONC_SHAPE, "    if (count != 6 || instances != 1) return false;")], "any kind of draw is the composite"),
    M("onc-count-unchecked", "C5", [(ONC_SHAPE, "    if (kind != 'X' || instances != 1) return false;")], "any index count is the composite"),
    M("onc-instances-unchecked", "C5", [(ONC_SHAPE, "    if (kind != 'X' || count != 6) return false;")], "any instance count is the composite"),
    M("onc-vs-unchecked", "C5", [(ONC_HASH, "")], "any vertex shader is the composite's"),
    M("onc-vs-from-ps", "C5", [("bindingShaderHash(BindSlot::Vs)", "bindingShaderHash(BindSlot::Ps)")], "the pixel shader's hash is tested"),
    M("onc-vs-equal", "C5", [(ONC_HASH, "    if (bindingShaderHash(BindSlot::Vs) == kIntroCompositeVsHash) return false;\n")], "every VS but the composite's is accepted"),
    M("onc-vs-low-half", "C5", [(ONC_HASH, "    if ((bindingShaderHash(BindSlot::Vs) & 0xFFFFFFFFull) != (kIntroCompositeVsHash & 0xFFFFFFFFull)) return false;\n")],
      "only the low half of the hash is tested"),
    M("onc-null-ctx-unchecked", "C5", [(ONC_GATE, "    if (!introCurveWants()) return false;")], "a null context is not refused"),
    M("recognise-null-cb-unchecked", "C5", [(REC_NULL, "")], "an empty slot 2 is taken for a buffer"),
    M("hash-one-digit", "C5", [(HASH, "constexpr uint64_t kIntroCompositeVsHash = 0xEF103A7CB4A8369Bull;")], "the composite's hash has a digit wrong", file="h"),
    M("info-standdown-inverted", "C5", [(INFO_DOWN, "    info.standDown = g_budget.shouldRun();\n")], "the info says it stood down when it did not"),
    # ---- C6: the settle --------------------------------------------------------------------------------------------------------
    M("read-without-context-guard", "C6", [("g_frame >= e.dueFrame && ctx) readBack", "g_frame >= e.dueFrame) readBack")], "a tick with no context reads through it"),
    # ---- C7: the table ---------------------------------------------------------------------------------------------------------
    M("table-15", "C7", [(KTABLE, "constexpr uint32_t kMaxEntries = 15;")], "the table holds 15 pairs"),
    M("table-17", "C7", [(KTABLE, "constexpr uint32_t kMaxEntries = 17;")], "the table holds 17 pairs"),
    M("claim-never-free-first", "C7", [(CLAIM_FREE, "        if (false) {\n            clearEntry(e);")], "a free slot is not taken first: every new pair evicts"),
    M("evict-most-recent", "C7", [(CLAIM_PICK, "if (!oldest || e.lastSeen > oldest->lastSeen || (e.lastSeen == oldest->lastSeen && e.born < oldest->born)) oldest = &e;")],
      "the pair drawn most recently is the one pushed out"),
    M("evict-by-arrival-only", "C7", [(CLAIM_PICK, "if (!oldest || e.born < oldest->born) oldest = &e;")], "first in, first out: a pair drawn again is still the oldest"),
    M("evict-latest-arrival-on-tie", "C7", [(CLAIM_PICK, "if (!oldest || e.lastSeen < oldest->lastSeen || (e.lastSeen == oldest->lastSeen && e.born > oldest->born)) oldest = &e;")],
      "of two last drawn together, the later arrival goes"),
    M("born-not-set", "C7", [(REC_BORN, "        e->born = 0;\n")], "arrival order is not kept: ties go by slot"),
    M("evict-uncounted", "C7", [(EVICT_COUNT, "")], "an eviction is not counted"),
    M("evict-line-repeated", "C7", [(EVICT_NOTED, "")], "the table-full line is written for every eviction"),
    M("evict-line-never", "C7", [(EVICT_TEST, "    if (false) {")], "the table-full line is never written"),
    M("evict-without-release", "C7", [(EVICT_CLEAR, "    *oldest = Entry();\n    return oldest;")], "an evicted pair's readback buffer is not released"),
    M("clear-entry-keeps-stage", "C7", [(CLEAR_ENTRY, "void clearEntry(Entry& e) {\n    e = Entry();\n}")], "clearing a pair does not release its readback buffer"),
    M("info-evicted-not-reported", "C7", [(INFO_EVICTED, "    info.evicted = 0;\n")], "evictions are not reported"),
    # ---- C8: expiry ------------------------------------------------------------------------------------------------------------
    M("expire-179", "C8", [(KEXPIRE, "constexpr uint32_t kExpireFrames = 179;")], "a pair is forgotten after 179 frames"),
    M("expire-181", "C8", [(KEXPIRE, "constexpr uint32_t kExpireFrames = 181;")], "a pair is forgotten after 181 frames"),
    M("expire-600", "C8", [(KEXPIRE, "constexpr uint32_t kExpireFrames = 600;")], "a pair is forgotten after 600 frames, the limit it had before"),
    M("expire-ge", "C8", [(SVC_EXPIRE, "g_frame - e.lastSeen >= kExpireFrames")], "a pair is forgotten at the limit, not past it"),
    M("expire-uncounted", "C8", [(SVC_EXPIRED, "")], "a forgotten pair is not counted"),
    M("expire-keeps-entry", "C8", [(SVC_CLEAR, "            ++g_expired;\n            continue;")], "a pair is counted as forgotten and kept"),
    M("service-expires-free-entries", "C8", [(SVC_FREE, "    for (Entry& e : g_entry) {\n        if (g_frame - e.lastSeen > kExpireFrames) {")], "an empty slot is forgotten as if it were a pair"),
    M("lastseen-not-refreshed", "C8", [(REC_FOUND, "    if (e) {\n")], "drawing a pair does not restart its clock"),
    M("new-lastseen-not-set", "C8", [(REC_NEW_SEEN, "        startCopy(ctx, cb, *e);")], "a new pair's clock starts at frame 0"),
    M("service-reads-every-entry", "C8", [(SVC_READ, "if (g_frame >= e.dueFrame && ctx) readBack(ctx, e);")], "a learned pair is read again when its due frame has passed"),
    M("info-expired-not-reported", "C8", [(INFO_EXPIRED, "    info.expired = 0;\n")], "expiries are not reported"),
    # ---- C9: retirement and shutdown -------------------------------------------------------------------------------------------
    M("retire-flag-dropped", "C9", [(RET_FLAG, "    g_armedGain = 0.0f;")], "retirement does not retire"),
    M("retire-no-release", "C9", [(RET_RELEASE, "")], "retirement does not release the table"),
    M("retire-learned-line-dropped", "C9", [(RET_LEARNED, "    if (false) {")], "the counts line is never written"),
    M("retire-never-seen-silent", "C9", [(RET_WANTED, "    } else if (false) {")], "a retirement with nothing seen says nothing"),
    M("retire-always-says", "C9", [(RET_WANTED, "    } else {")], "a retirement that was never wanted still says so"),
    M("wants-ignores-retired", "C9", [(WANTS, "    return g_budget.shouldRun() && panelCurveSurfaceWanted();")], "it is wanted after it retired"),
    M("tick-retired-keeps-going", "C9", [(TICK_RETIRED, "    if (!g_wantedSeen && panelCurveSurfaceWanted()) g_wantedSeen = true;")], "a retired module retires again at every scene tick"),
    M("tick-wanted-always", "C9", [(TICK_WANTED, "    g_wantedSeen = true;\n")], "it was always wanted"),
    M("tick-wanted-never", "C9", [(TICK_WANTED, "")], "it was never wanted"),
    M("tick-scene-ignored", "C9", [(TICK_SCENE, "")], "the first rendered scene does not retire it"),
    M("shutdown-keeps-table", "C9", [(SHUTDOWN, "")], "shutdown does not release the table"),
    M("release-all-empty", "C9", [(RELEASE_ALL, "")], "releasing everything releases nothing"),
    M("clear-entry-keeps-pair", "C9", [(CLEAR_ENTRY, "void clearEntry(Entry& e) {\n    dropStage(e);\n}")], "clearing a pair releases its buffer and keeps the pair"),
    M("info-retired-dropped", "C9", [(INFO_RETIRED, "    info.retired = false;\n")], "the info never says it retired"),
    # ---- C10: faults -----------------------------------------------------------------------------------------------------------
    M("fault-budget-2", "C10", [(KBUDGET, "constexpr int kFaultBudget = 2;")], "a first fault does not stand it down"),
    M("wants-ignores-budget", "C10", [(WANTS, "    return !g_retired && panelCurveSurfaceWanted();")], "it is wanted after it stood down"),
    M("stand-down-line-dropped", "C10", [(STAND_DOWN_LINE, "")], "a fault stands it down without a word"),
    M("stand-down-line-reworded", "C10", [("a fault stood this down for the session", "something went wrong")], "the stand-down line is reworded"),
    M("fault-release-skipped", "C10", [(FAULT_RELEASE, "")], "a stood-down module keeps what it held"),
    M("onc-stand-down-unsaid", "C10", [(ONC_GUARD, "    if (!guardedBudget(g_budget, [&] { armed = recognise(ctx); })) {\n        return false;\n    }")],
      "a draw-path fault is not said"),
    M("tick-fault-unsaid", "C10", [(TICK_SERVICE, "    guardedBudget(g_budget, [&] { serviceEntries(ctx); });")], "a readback fault is not said"),
    M("tick-down-keeps-reading", "C10", [(TICK_DOWN, "")], "a stood-down module is serviced every tick: it says so again and keeps what it held"),
    M("retire-fault-suffix-dropped", "C10", [(RET_FAULT, '""')], "the retirement line does not say a fault had stood it down"),
    M("onc-unguarded", "C10", [(ONC_GUARD, "    armed = recognise(ctx);\n    if (false) {\n        noteStandDown();\n        return false;\n    }")],
      "the draw path runs outside the fault guard", crash_ok=True),
    # ---- C11: the log ----------------------------------------------------------------------------------------------------------
    M("lines-11", "C11", [(KLINES, "constexpr uint32_t kMaxFlatLines = 11;")], "eleven lines about flat pairs"),
    M("lines-13", "C11", [(KLINES, "constexpr uint32_t kMaxFlatLines = 13;")], "thirteen lines about flat pairs"),
    M("lines-unbounded", "C11", [(LEARN_TEST, "    if (true) {")], "no cap on the lines about flat pairs"),
    M("lines-off-by-one", "C11", [(LEARN_TEST, "    if (g_flatLines <= kMaxFlatLines) {")], "one line over the cap"),
    M("lines-counter-stuck", "C11", [(LEARN_COUNT, "")], "the lines are never counted"),
    M("cap-line-repeated", "C11", [(CAP_NOTED, "")], "the cap is announced for every learn over it"),
    M("cap-line-never", "C11", [(CAP_TEST, "    if (false) {")], "the cap is never announced"),
    M("cap-line-reworded", "C11", [(CAP_TEXT, "lines about composites left as the game drew them are kept; the rest are dropped")], "the cap line is reworded"),
    M("floats-3-digits", "C11", [(FORMAT, '"%s%.3g"')], "the floats print to three digits"),
    M("floats-groups-of-5", "C11", [(GROUP, "i % 5 == 0")], "the floats are grouped five to a line"),
    M("floats-sixteen", "C11", [(FORMAT_LOOP, "i < 16 && at + 1 < size")], "only 16 of the 20 floats are written"),
    M("floats-not-formatted", "C11", [(FORMAT_CALL, "")], "the floats are never formatted"),
    M("flat-uncounted", "C11", [(FLAT_COUNT, "")], "a flat pair is not counted as learned"),
    M("flat-line-cap-ignored", "C11", [(FLAT_CAP, "    if (f) {")], "the cap does not apply to the lines about flat pairs"),
    M("world-line-capped", "C11", [(WORLD_STRIP, "    if (!flatLineAllowed()) return;\n" + WORLD_STRIP)], "a world line is held to the cap on the flat lines"),
    M("retire-counts-swapped", "C11", [(RET_WORLDS, "static_cast<unsigned long long>(g_learnedWorld + g_learnedFlat), static_cast<unsigned long long>(g_learnedFlat),")],
      "the retirement line counts flats as worlds"),
    M("retire-armed-only", "C11", [(RET_LEARNED, "    if (g_armedDraws) {")], "the counts line needs an armed draw"),
    M("world-line-columns-lost", "C11", [(WORLD_STRIP, "    const PanelCurveInfo strip = PanelCurveInfo();\n")], "the line does not read the strip's live columns and curvature"),
    # ---- C12: the wiring -------------------------------------------------------------------------------------------------------
    M("world-gain-constant", "C12", [(WORLD_GAIN, "    e.halfWidth = 4.44444f;\n")], "the gain is the splash's measured width whatever the buffer says"),
    M("world-toward-constant", "C12", [(WORLD_TOWARD, "    e.toward = 1;\n")], "the direction is always toward the viewer"),
    M("arm-gain-constant", "C12", [(REC_GAIN, "    g_armedGain = 4.44444f;\n")], "an armed draw is handed a constant gain"),
    M("arm-toward-constant", "C12", [(REC_TOWARD, "    g_armedToward = 1;\n")], "an armed draw is handed a constant direction"),
    M("onc-numbers-not-cleared", "C12", [(ONC_CLEAR, ONC_GATE)], "a draw that is not armed leaves the last one's numbers"),
    M("enddraw-gain-kept", "C12", [(END_DRAW, "void introCurveEndDraw() {\n    g_armedToward = 0;\n    g_armedReverseU = false;\n}")], "disarming leaves the gain"),
    M("enddraw-toward-kept", "C12", [(END_DRAW, "void introCurveEndDraw() {\n    g_armedGain = 0.0f;\n    g_armedReverseU = false;\n}")], "disarming leaves the direction"),
    M("enddraw-reverse-kept", "C1", [(END_DRAW, "void introCurveEndDraw() {\n    g_armedGain = 0.0f;\n    g_armedToward = 0;\n}")], "disarming leaves the u direction"),
    M("onc-reverse-not-cleared", "C12", [(ONC_CLEAR_REVERSE, "    if (!ctx || !introCurveWants()) return false;")], "a draw that is not armed leaves the last one's u direction"),
    M("reverse-always-false", "C1", [(ARM_REVERSE, "    g_armedReverseU = false;\n")], "every armed draw is handed a u direction of with x"),
    M("reverse-always-true", "C17", [(ARM_REVERSE, "    g_armedReverseU = true;\n")], "every armed draw is handed a u direction of against x, whichever way +x runs"),
    M("reverse-stored-inverted", "C1", [(ENTRY_REVERSE, "    e.reverseU = w.xDir == IntroXDir::kRight;\n")], "a pair keeps the opposite of the direction it was read with"),
    M("reverse-not-stored-per-pair", "C17", [(ENTRY_REVERSE, "    g_lastReverseU = w.xDir == IntroXDir::kLeft;\n"), (ARM_REVERSE, "    g_armedReverseU = g_lastReverseU;\n"),
                                              (ARMED_REVERSE_DECL, ARMED_REVERSE_DECL + "bool  g_lastReverseU = false;\n")],
      "the direction is the last pair learned's, not the pair's own"),
    M("reverse-getter-always-false", "C12", [(GETTER, "bool introCurveReverseU() {\n    return false;\n}")], "the getter never says against x"),
    M("reverse-getter-ignores-arming", "C12", [(GETTER, "bool introCurveReverseU() {\n    return true;\n}")], "the getter says against x whether or not anything is armed"),
    M("world-line-direction-dropped", "C1", [(WORLD_DIRECTION + " drawn", "drawn")], "the learn line does not say which way +x runs"),
    M("world-line-direction-inverted", "C17", [(WORLD_DIRECTION_ARGS, 'e.reverseU ? "right" : "left", e.reverseU ? "with" : "against"')], "the learn line says the opposite of what was read"),
    M("world-line-direction-reworded", "C1", [(WORLD_DIRECTION, "+x runs to the viewer's %s, so u runs %s the x axis;")], "the learn line's direction clause is reworded"),
    M("math-gate-dropped-unknown-armed", "C17", [("    if (dir == IntroXDir::kUnknown) {", "    if (false) {")], "a placement whose +x cannot be told is armed all the same", file="math"),
    M("math-sign-flipped-direction-wrong", "C1", [("    return minor < 0.0 ? IntroXDir::kLeft : IntroXDir::kRight;\n", "    return minor < 0.0 ? IntroXDir::kRight : IntroXDir::kLeft;\n")],
      "the pure reading says right where the placement runs left", file="math"),
    # ---- C13: a flat pair is copied again: the cut -----------------------------------------------------------------------------
    M("reread-never", "C13", [(REREAD_START, "")], "a flat pair is never copied again"),
    M("reread-never-read-back", "C13", [(SVC_READ, "if (e.state == State::kCopying && g_frame >= e.dueFrame && ctx) readBack(ctx, e);")],
      "a flat pair's second copy is issued and never read back"),
    M("recheck-59", "C13", [(KRECHECK, "constexpr uint32_t kFlatRecheckFrames = 59;")], "a flat pair is copied again after 59 frames"),
    M("recheck-61", "C13", [(KRECHECK, "constexpr uint32_t kFlatRecheckFrames = 61;")], "a flat pair is copied again after 61 frames"),
    M("recheck-gt", "C13", [(REREAD_START, REREAD_START.replace(">= kFlatRecheckFrames", "> kFlatRecheckFrames"))],
      "a flat pair is copied again once its last copy is MORE than 60 frames old"),
    M("reread-flips-state-early", "C13", [(REREAD_FAIL, REREAD_FAIL + "    else e.state = State::kCopying;\n")],
      "the pair stops being flat the moment its re-read is issued, before anything has been read"),
    M("reread-period-from-the-readback", "C13", [(COPY_READFRAME, ""), (READ_DROP, "    dropStage(e);\n    e.readFrame = g_frame;\n    if (reread) ++g_rereads;")],
      "the wait to the next copy starts when the last was read, not when it was taken"),
    M("reread-counts-first-reads", "C13", [(READ_FLAG, "    const bool reread = true;\n")], "a first reading is taken for a re-read"),
    M("reread-uncounted", "C13", [(READ_COUNT, "")], "a re-read is not counted"),
    M("reread-change-uncounted", "C13", [(READ_CHANGE, "")], "a flat pair that reads as world is not counted as a change"),
    M("info-rereads-not-reported", "C13", [(INFO_REREADS, "    info.rereads = 0;\n")], "the re-reads are not reported"),
    M("info-changes-not-reported", "C13", [(INFO_CHANGES, "    info.flatToWorld = 0;\n")], "the flat-to-world changes are not reported"),
    M("retire-clause-dropped", "C13", [(RET_CLAUSE_TEST, "        if (false) {")], "the retirement line never says the re-reads"),
    M("retire-clause-always", "C9", [(RET_CLAUSE_TEST, "        if (true) {")], "the retirement line says '0 re-read(s)' when there were none"),
    M("retire-clause-swapped", "C14", [(RET_CLAUSE_REREADS, "static_cast<unsigned long long>(g_flatToWorld),"), (RET_CLAUSE_CHANGES, "static_cast<unsigned long long>(g_rereads));")],
      "the retirement clause says its two counts the wrong way round"),
    # ---- C14: staying flat ---------------------------------------------------------------------------------------------------------
    M("reread-leaks-stage", "C14", [(READ_DROP, "    if (!reread) dropStage(e);\n    if (reread) ++g_rereads;")], "a re-read's readback buffer is never given back"),
    M("reread-ignores-pending-copy", "C14", [(REREAD_START, REREAD_START.replace("!e->stage && ", ""))], "a second copy is issued over the one in flight"),
    M("readframe-never-set", "C14", [(COPY_READFRAME, "")], "a pair's last-copy frame is never set: it is copied again at every draw"),
    M("reread-arms-without-classifying", "C14", [(READ_JUDGE, "    if (!w.ok && !reread) {")], "a re-read that reads flat arms the pair all the same"),
    M("reread-logs-every-time", "C14", [(STILL_TEST, "")], "a re-read says its reason every time it reads it"),
    M("reread-reason-class-kept", "C14", [(STILL_CLASS, "    flatLine(kLeadAgain, why, f);")], "the reason last said for a pair is not kept, so a repeat is said again"),
    M("reread-new-reason-unsaid", "C14", [(STILL_LINE, "")], "a re-read that gives another reason never says so"),
    M("reread-why-lost", "C14", [(READ_WHY_AGAIN, '        if (reread) noteStillFlat(e, w.why, "unknown", f);')], "a re-read's line does not say why"),
    M("reread-lead-reworded", "C14", [(LEAD_AGAIN, 'const char kLeadAgain[] = "this composite, again,";')], "a re-read's line is reworded"),
    M("reread-line-uncapped", "C14", [(STILL_LINE, STILL_UNCAPPED)], "a re-read's line ignores the cap on the lines about flat pairs"),
    M("reread-copy-fail-logs-every-time", "C14", [(REREAD_FAIL, "    if (!issueCopy(ctx, cb, e, fail)) flatLine(kLeadAgain, fail.text, nullptr);\n")],
      "a re-read whose copy cannot be made says so at every period"),
    # ---- C15: failed re-reads ----------------------------------------------------------------------------------------------------
    M("reread-unreadable-settles-as-first", "C15", [(READ_UNREADABLE_LINE, "        settleFlat(e, kWhyNoRead, kWhyNoRead, nullptr);\n")],
      "a re-read that cannot be read back is taken for a first reading: counted as learned again, worded as a first"),
    M("reread-failed-read-keeps-stage", "C15", [(READ_DROP, "    if (have) dropStage(e);\n    if (reread) ++g_rereads;")], "a re-read that cannot be read back keeps its buffer"),
    M("reread-fault-still-wanted", "C15", [(WANTS, "    return !g_retired && panelCurveSurfaceWanted();")], "a module a re-read's fault stood down is still asked for"),
    M("reread-fault-budget-2", "C15", [(KBUDGET, "constexpr int kFaultBudget = 2;")], "a first fault in a re-read does not stand it down"),
    M("reread-fault-release-skipped", "C15", [(FAULT_RELEASE, "")], "a module a fault stood down keeps its re-read's buffer"),
    # ---- C16: a world is final -------------------------------------------------------------------------------------------------
    M("reread-world-too", "C16", [(REREAD_START, REREAD_START.replace("e->state == State::kFlat", "(e->state == State::kFlat || e->state == State::kWorld)"))],
      "a world pair is copied again too"),
]


# ---- applying an edit ----------------------------------------------------------------------------------------------------
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


def fail_labels(output):
    """The labels of every check the rig reported ('C7.the-least-recent...'), in order, and not its closing summary."""
    return [m.group(1) for m in re.finditer(r"^FAIL: (C\d+\.[\w-]+)", output, re.MULTILINE)]


def rig_cases(rig_text):
    return set(re.findall(r'\{"(C\d+)", case\w+\}', rig_text))


def rig_label_exists(rig_text, prefix):
    return ('"%s.' % prefix) in rig_text


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


def find_gen():
    """The generated-headers directory: this checkout's build\\gen, else the main checkout's (a worktree is .claude\\worktrees\\<name>)."""
    for candidate in (ROOT / "build" / "gen", ROOT.parents[2] / "build" / "gen" if len(ROOT.parents) > 2 else None):
        if candidate and candidate.is_dir():
            return candidate
    return None


# ---- the toolchain -----------------------------------------------------------------------------------------------------------
class Toolchain:
    """cl.exe and link.exe by absolute path (Windows looks a bare name up on THIS process's PATH, not on the env passed), with the
    environment they need: this one if cl is on PATH, else the one vcvars64.bat makes, found with vswhere as build.bat does."""

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
        self.link = str(Path(found).with_name("link.exe"))


def compile_obj(tc, source, outdir, includes, obj_name=None):
    """Compile one source to outdir\\<obj_name, else the source's stem>.obj; (exit code, output, obj path)."""
    obj = outdir / ((obj_name or Path(source).stem) + ".obj")
    cmd = [tc.cl] + CL_FLAGS + ["/c"] + ["/I" + str(i) for i in includes] + ["/Fo" + str(obj), str(source)]
    done = subprocess.run(cmd, capture_output=True, text=True, env=tc.env, errors="replace", cwd=str(outdir))
    return done.returncode, (done.stdout + done.stderr), obj


def link_exe(tc, objs, exe, outdir):
    cmd = [tc.link, "/nologo", "/INCREMENTAL:NO", "/OUT:" + str(exe)] + [str(o) for o in objs] + LIBS
    done = subprocess.run(cmd, capture_output=True, text=True, env=tc.env, errors="replace", cwd=str(outdir))
    return done.returncode, (done.stdout + done.stderr)


def run_rig(exe, rule, tc):
    """(outcome, labels, tail) of one run of the rig on one case: 'pass', 'fail', 'crash', 'timeout'."""
    cmd = [str(exe), "--self-test"] + (["--only", rule] if rule else [])
    try:
        done = subprocess.run(cmd, capture_output=True, text=True, env=tc.env, errors="replace", timeout=RUN_TIMEOUT)
    except subprocess.TimeoutExpired:
        return "timeout", [], "no result within %d s" % RUN_TIMEOUT
    out = done.stdout + "\n" + done.stderr
    if done.returncode == 0:
        return "pass", [], ""
    labels = fail_labels(out)
    if not labels:
        return "crash", [], "exit 0x%08X" % (done.returncode & 0xFFFFFFFF)
    return "fail", labels, ""


# ---- the run ---------------------------------------------------------------------------------------------------------------------
def select(only):
    if not only:
        return MUTANTS
    wanted = [n for n in only.split(",") if n]
    unknown = [n for n in wanted if n not in {m.name for m in MUTANTS}]
    if unknown:
        raise ValueError("no such mutation: " + ", ".join(unknown))
    return [m for m in MUTANTS if m.name in wanted]


def plan_text(mutants):
    lines = ["%d mutation(s); each is compiled with the rig in a temp directory outside the repo and run on its rule's case:" % len(mutants)]
    for m in mutants:
        lines.append("  %-30s %-3s caught by %-4s %s" % (m.name, m.file, "/".join(m.caught), m.why))
    lines.append("cl.exe flags: " + " ".join(CL_FLAGS))
    return "\n".join(lines)


def run_all(only=None, jobs=None, keep=False, dry_run=False, verbose=False, out=sys.stdout):
    mutants = select(only)
    if dry_run:
        print(plan_text(mutants), file=out)
        print("dry-run: nothing compiled, started or written", file=out)
        return 0
    gen = find_gen()
    tc = Toolchain()
    sources = {k: read_source(p) for k, p in FILES.items()}
    work = Path(tempfile.mkdtemp(prefix="icm_"))
    workers = jobs or min(8, os.cpu_count() or 2)
    try:
        # The control: the unmutated module and the rig, built the way every mutation is, and every common source compiled once.
        base_inc = ([gen] if gen else []) + [SRC]
        common_dir = work / "c"
        common_dir.mkdir()
        control_dir = work / "k"
        control_dir.mkdir()
        for k in FILES:
            (control_dir / FILES[k].name).write_text(sources[k], encoding="utf-8", newline="\n")
        with concurrent.futures.ThreadPoolExecutor(max_workers=workers) as pool:
            futures = [pool.submit(compile_obj, tc, c, common_dir, base_inc) for c in COMMON]
            futures.append(pool.submit(compile_obj, tc, control_dir / "intro_curve.cpp", control_dir, [control_dir] + base_inc))
            futures.append(pool.submit(compile_obj, tc, RIG, common_dir, [control_dir] + base_inc, "rig"))
            built = [f.result() for f in futures]
        for code, text, obj in built:
            if code != 0:
                print("control: nocompile %s\n%s" % (obj.name, text.strip()[-1500:]), file=out)
                return 1
        common_objs = [b[2] for b in built[:len(COMMON)]]
        module_obj = built[len(COMMON)][2]
        rig_obj = common_dir / "rig.obj"
        code, text = link_exe(tc, [rig_obj, module_obj] + common_objs, control_dir / "rig.exe", control_dir)
        if code != 0:
            print("control: nolink\n" + text.strip()[-1500:], file=out)
            return 1
        outcome, labels, tail = run_rig(control_dir / "rig.exe", None, tc)
        print("control (the unmutated module, every case): %s %s" % (outcome, tail or " ".join(labels[:3])), file=out)
        if outcome != "pass":
            print("the rig does not pass on the unmutated module when built this way; nothing below means anything", file=out)
            return 1

        def one(index, m):
            try:
                mutated = apply_edits(sources[m.file], m.edits, m.name)
            except ValueError as error:
                return m, "badedit", str(error)
            d = work / ("m%03d" % index)
            d.mkdir()
            for k in FILES:
                (d / FILES[k].name).write_text(mutated if k == m.file else sources[k], encoding="utf-8", newline="\n")
            inc = [d] + base_inc
            code, text, mobj = compile_obj(tc, d / "intro_curve.cpp", d, inc)
            if code != 0:
                errors = [l.strip() for l in text.splitlines() if "error" in l]
                return m, "nocompile", (errors[0] if errors else text.strip().splitlines()[-1] if text.strip() else "")[:160]
            robj = rig_obj
            if m.file == "h":   # the rig compiles the header too: rebuild it against the mutated copy
                code, text, robj = compile_obj(tc, RIG, d, inc, "rig")
                if code != 0:
                    errors = [l.strip() for l in text.splitlines() if "error" in l]
                    return m, "nocompile", (errors[0] if errors else "")[:160]
            exe = d / "rig.exe"
            code, text = link_exe(tc, [robj, mobj] + common_objs, exe, d)
            if code != 0:
                return m, "nolink", (text.strip().splitlines() or [""])[-1]
            outcome, labels, tail = run_rig(exe, m.rule, tc)
            if outcome == "fail":
                if any(l.startswith(c + ".") for l in labels for c in m.caught):
                    if verbose:
                        return m, "caught", "%d failing label(s): %s" % (len(labels), ", ".join(labels))
                    return m, "caught", "%d failing label(s), first %s" % (len(labels), labels[0])
                return m, "OTHER", "failed on %s only" % ", ".join(sorted({l.split(".")[0] for l in labels}))
            if outcome == "crash" and m.crash_ok:
                return m, "caught", "crashed, as a fault outside the guard does (%s)" % tail
            return m, {"pass": "SURVIVED", "crash": "CRASH", "timeout": "TIMEOUT"}[outcome], tail

        results = []
        with concurrent.futures.ThreadPoolExecutor(max_workers=workers) as pool:
            for m, verdict, detail in pool.map(lambda im: one(*im), list(enumerate(mutants))):
                results.append((m, verdict, detail))
                print("%-9s %-30s %-4s %s" % (verdict, m.name, "/".join(m.caught), detail if verbose else detail[:150]), file=out)
        bad = [r for r in results if r[1] != "caught"]
        print("%d mutation(s): %d caught by their own case, %d not" % (len(results), len(results) - len(bad), len(bad)), file=out)
        for m, verdict, detail in bad:
            print("  NOT CAUGHT: %s (%s): wanted %s, got %s %s" % (m.name, m.why, "/".join(m.caught), verdict, detail[:120]), file=out)
        return 0 if not bad else 1
    finally:
        if keep:
            print("kept: %s" % work, file=out)
        else:
            shutil.rmtree(work, ignore_errors=True)


# ---- the self-test ------------------------------------------------------------------------------------------------------------------
def self_test(build_bat=BUILD_BAT):
    failures = []

    def check(ok, what):
        if not ok:
            failures.append(what)

    # the tool's own pieces
    check(fail_labels("a\nFAIL: C4.reissue1-identical -- x [y]\nFAIL: C2.sub-bytes: because\nFAIL: intro curve module: 3 of 9\n") == ["C4.reissue1-identical", "C2.sub-bytes"],
          "fail_labels reads the label of every check's FAIL: line, and not the closing summary")
    check(fail_labels("PASS: 12 checks\n") == [], "fail_labels reads nothing from a pass")
    check(apply_edits("a b c", [("b", "B")]) == "a B c", "apply_edits applies one edit")
    for bad, why in ((("a b b", [("b", "B")]), "a repeated anchor"), (("a b", [("x", "y")]), "a missing anchor"), (("a b", [("b", "b")]), "a no-op edit")):
        try:
            apply_edits(*bad)
            check(False, "apply_edits refuses " + why)
        except ValueError:
            pass
    joined = label_block(":rig_a\nx ^\n y\nexit /b 0\n:rig_b\nz\n", ":rig_a")
    check(joined is not None and [" ".join(l.split()) for l in joined] == ["x y", "exit /b 0"], "label_block joins continuations and stops at the next label")
    check(rig_cases('{"C1", case1}, {"C10", case10}') == {"C1", "C10"}, "rig_cases reads the case table")

    # every mutation against the sources as they are now, and against the rig
    sources = {k: read_source(p) for k, p in FILES.items()}
    rig = read_source(RIG)
    cases = rig_cases(rig)
    names = [m.name for m in MUTANTS]
    check(len(names) == len(set(names)), "mutation names are unique")
    check(len(MUTANTS) >= 100, "the mutation list did not shrink below 100 (%d)" % len(MUTANTS))
    for m in MUTANTS:
        check(m.file in FILES, "%s edits intro_curve.%s, which this tool does not know" % (m.name, m.file))
        if m.file in FILES:
            try:
                mutated = apply_edits(sources[m.file], m.edits, m.name)
                check(mutated != sources[m.file], "%s changes the source" % m.name)
            except ValueError as error:
                failures.append(str(error))
        for c in m.caught:
            check(c in cases, "%s: the rig has no case %s" % (m.name, c))
            check(rig_label_exists(rig, c), "%s: the rig has no check labelled %s." % (m.name, c))
    covered = {c for m in MUTANTS for c in m.caught}
    check(covered == cases, "every case of the rig has a mutation, and only cases of the rig: %s vs %s" % (sorted(covered), sorted(cases)))

    # the module still has what the rig stubs and the tool copies
    check(find_gen() is None or find_gen().is_dir(), "the generated-headers directory, when there is one, is a directory")

    # build.bat compiles the rig the way this tool does, and runs this tool's self-test
    bat = Path(build_bat).read_bytes().decode("utf-8", errors="replace")
    block = label_block(bat, RIG_LABEL)
    check(block is not None, "build.bat has %s" % RIG_LABEL)
    if block:
        text = "\n".join(block)
        cl = next((l for l in block if l.strip().lower().startswith("cl.exe")), "")
        for flag in CL_FLAGS:
            check(flag in cl, "build.bat's rig compile has %s" % flag)
        for src in ("tools\\intro_curve_module_test\\intro_curve_module_test.cpp", "src\\d3d11\\intro_curve.cpp", "src\\common\\config.cpp", "src\\common\\log.cpp",
                    "src\\common\\guard.cpp", "src\\common\\proxy.cpp"):
            check(src in cl, "build.bat's rig compile has %s (the sources this tool links)" % src)
        check(("src\\d3d11\\panel_curve.cpp" in cl) == REAL_STRIP, "build.bat's rig links the real panel_curve.cpp if and only if this tool's REAL_STRIP is on (%s)" % REAL_STRIP)
        check(("INTRO_CURVE_RIG_REAL_STRIP" in cl) == REAL_STRIP, "build.bat's rig is built with -DINTRO_CURVE_RIG_REAL_STRIP if and only if this tool's REAL_STRIP is on (%s)" % REAL_STRIP)
        check('/I"src\\d3d11"' in cl, "build.bat's rig compile finds the headers through /I src\\d3d11 (the mutated copy is found the same way)")
        for lib in LIBS:
            check(lib in cl, "build.bat's rig link has %s" % lib)
        check("d3d11.lib" not in cl, "build.bat's rig does not link d3d11.lib (it takes System32's device through src\\common\\system_d3d11.h)")
        check('intro_curve_module_test.exe" --dry-run' in text and 'intro_curve_module_test.exe" --self-test' in text, "build.bat runs the rig's --dry-run and --self-test")
        check('mutants.py" --self-test' in text, "build.bat runs this tool's --self-test")

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
    check(code == 0 and not calls and "dry-run" in sink.getvalue(), "--dry-run starts no process and makes no directory (saw %s)" % calls)
    try:
        select("no-such-mutation")
        check(False, "--only names an unknown mutation")
    except ValueError:
        pass

    if failures:
        for f in failures:
            print("FAIL: " + f, file=sys.stderr)
        return 1
    print("PASS: intro_curve_module_test mutants.py self-test (%d mutations over %d cases, every anchor found once, build.bat wired)" % (len(MUTANTS), len(covered)))
    return 0


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--self-test", action="store_true")
    parser.add_argument("--run", action="store_true")
    parser.add_argument("--list", action="store_true")
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--only", default="")
    parser.add_argument("--jobs", type=int, default=0)
    parser.add_argument("--keep", action="store_true", help="leave the temp directory (printed) for a look at a mutant")
    parser.add_argument("--verbose", action="store_true", help="print every label a mutant made the rig fail on, not just the first")
    parser.add_argument("--build-bat", default=str(BUILD_BAT), help="the build.bat the self-test reads the rig's label from")
    parser.add_argument("--strip", choices=("real", "doubles"), help="build the rig against the real panel_curve.cpp or against its own doubles of the strip, for this run")
    args = parser.parse_args(argv)
    if args.strip:
        set_real_strip(args.strip == "real")
    if args.self_test:
        return self_test(args.build_bat)
    if args.list:
        print(plan_text(MUTANTS))
        return 0
    if args.run:
        return run_all(only=args.only, jobs=args.jobs or None, keep=args.keep, dry_run=args.dry_run, verbose=args.verbose)
    parser.print_help()
    return 2


if __name__ == "__main__":
    sys.exit(main())
