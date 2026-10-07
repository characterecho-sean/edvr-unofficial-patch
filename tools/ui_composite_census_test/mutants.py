#!/usr/bin/env python3
"""The mutation proof for tools\\ui_composite_census_test: the rig fails when a rule of the pure headers is flipped.

The rig (ui_composite_census_test.cpp) pins nine rules, R1..R9 (its header says which): R1 the family rule that names the cockpit
holo panels by both their vertex shaders (src\\d3d11\\ui_layer_math.h, with the hashes in holo_material.h), R2..R7 the census of the
interface composites the layer leaves in the scene (src\\d3d11\\ui_scene_composites.h): the window's table, the zero line, the line
with pairs, its length, the detector-off wording, a window with no frames, R8 the setting off changing nothing (the production rule held to
a frozen copy of the rule as it was, over every combination of facts), R9 the pair the scene keeps on purpose (the engine's null-output
quad: kept, not left; only the exact pair with no family; its clause; the rule's exclusion). A rig that passes proves little until it is seen to FAIL on
a header that breaks the rule it pins. This tool does that: for each mutation below it copies the production headers into a temp
directory OUTSIDE the repo (all of them: an include resolves in the including file's own directory first, so a copy of one header
beside the production others would be included twice), applies one textual edit (or a few that belong together) to one of them,
compiles the rig against that directory alone, runs it on the rule's cases, and requires the rig to fail on the check that belongs to
the rule (the first line it prints after FAIL: starts with the label prefix the mutation names). Nothing is written inside the repo;
the temp directory is removed at the end. The source pins (P1..P7) read the runtime's sources and are not mutated here (each carries
controls of its own: the same predicate over copies with one edit must fail).

  python tools\\ui_composite_census_test\\mutants.py --self-test    text only: every anchor is found exactly once in its header as it is
                                                                  now, every label named is in the rig, and build.bat compiles the rig
                                                                  the way this tool does
  python tools\\ui_composite_census_test\\mutants.py --run [--only a,b] [--jobs N] [--keep]
  python tools\\ui_composite_census_test\\mutants.py --list
  python tools\\ui_composite_census_test\\mutants.py --run --dry-run    the plan; writes nothing, starts nothing

--run needs the MSVC toolchain (cl.exe on PATH, or Visual Studio found with vswhere as build.bat does) and takes about a minute.
--self-test runs in build.bat's rig and is what keeps an edit of a header from silently orphaning a mutation: if an anchor stops
matching, the build fails and this file says which.
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
# Every header the rig's include closure holds (the rig includes ui_layer_math.h and ui_scene_composites.h; the first includes the two holo
# headers and, since 2026-10-07, supercruise_lines.h, the supercruise draws' shader hashes). A mutation edits one of them; the others are
# copied as they are.
HEADERS = ("ui_layer_math.h", "ui_scene_composites.h", "holo_material.h", "holo_families.h", "supercruise_lines.h")
RIG = HERE / "ui_composite_census_test.cpp"
BUILD_BAT = ROOT / "build.bat"
RIG_LABEL = ":rig_ui_composite_census_test"

# How build.bat compiles the rig; --self-test checks that the label still says the same.
CL_FLAGS = ["/nologo", "/O2", "/MT", "/std:c++17", "/EHsc", "/W4", "/DWIN32_LEAN_AND_MEAN", "/DNOMINMAX", "/D_CRT_SECURE_NO_WARNINGS"]
RUN_TIMEOUT = 90.0


class Mutant:
    def __init__(self, name, caught, edits, why, header):
        self.name = name
        self.caught = tuple([caught] if isinstance(caught, str) else caught)   # label prefixes that count as caught by its rule
        self.edits = list(edits)                                                # (old, new) pairs, applied in order
        self.why = why
        self.header = header                                                    # the header the edits apply to
        self.rule = re.match(r"R\d+", self.caught[0]).group(0)                  # the rig's case to run


def M(name, caught, edits, why, header="ui_scene_composites.h"):
    return Mutant(name, caught, edits, why, header)


def drop(old):
    return [(old, "")]


# ---- the anchors: text of the production headers, verbatim (the self-test finds each exactly once) -------------------------
# ui_layer_math.h
PRED = "constexpr bool uiVsIsHoloPanel(uint64_t vs) { return vs == kUiVsHolo || vs == kUiVsHoloGuiFxOff; }\n"
HDR_USE = "        out = uiVsIsHoloPanel(f.vs)    ? UiLayerFamily::kHolo\n"
LDR_USE = "              : uiVsIsHoloPanel(f.vs)  ? UiLayerFamily::kHolo\n"
KUI = "constexpr uint64_t kUiVsHoloGuiFxOff = kHoloGuiFxOffVs;\n"
FAMILY_NAME = '        case UiLayerFamily::kHolo: return "cockpit holo panels";\n'
HDR_SPRITE = "              : f.vs == kUiVsFlightHud ? UiLayerFamily::kFlightHud\n              : f.vs == kUiVsSprite    ? UiLayerFamily::kSprite\n"
LDR_LOADER = "              : f.vs == kUiVsLoader    ? UiLayerFamily::kLoader\n"
NO_SURFACE = "    } else if (f.vs == kUiVsPanel || f.vs == kUiVsLoader) {\n        w = UiFamilyWhy::kNoSurface;\n    }\n"
SPHERE_PS = "                  (f.ps == 0xEA02FAC2BD6C643Cull || f.ps == 0xE95634B0F61D218Full)))\n"
# holo_material.h
MAT_VS = "constexpr uint64_t kHoloGuiFxOffVs = 0x1989E6D3B405FDE0ull;\n"
MAT_PS = "constexpr uint64_t kHoloGuiFxOffPs = 0xEAB8A1C95A13FFBEull;\n"
# ui_scene_composites.h
PAIRS = "constexpr size_t kUiSceneCompositePairs = 8;\n"
USED = "    uint32_t used = 0;\n"
NOTE_TAKEN = "    void noteTaken() { ++seen; }\n"
NOTE_SEEN = "    void noteLeft(uint64_t vs, uint64_t ps, int family) {\n        ++seen;\n"
NOTE_LEFT = "        ++left;\n        for (uint32_t i = 0; i < used; ++i) {\n"
PAIR_MATCH = "            if (p.vs == vs && p.ps == ps && p.family == family) {\n"
PAIR_ROOM = "        if (used < kUiSceneCompositePairs) {\n"
PAST = "        ++pastTable;\n"
RESET = "    void reset() { *this = UiSceneCompositeWindow{}; }\n"
PER_FRAME = "    const double perFrame = frames ? 1.0 / static_cast<double>(frames) : 0.0;\n"
PREFIX = '    std::string out = "ui quality: composites left in the scene: ";\n'
DETECTOR = "    if (!detectorOn) {\n"
HEAD_RATE = "static_cast<double>(w.left) * perFrame,"
HEAD_LIVE = "static_cast<unsigned long long>(w.framesLive));"
NO_SEEN = "    if (!w.seen) {\n"
NO_LEFT = "    if (!w.left) {\n"
FAMILY_NONE = '        std::snprintf(label, sizeof(label), "no family");\n'
FAMILY_NAMED = '"%s, not taken"'
PAIR_FMT = '"vs %016llX ps %016llX (%s) %.2f a frame",\n'
JOIN_SEP = '        if (!first) out += "; ";\n'
PAIR_RATE = "                      static_cast<double>(p.draws) * perFrame);\n"
# the kept pair (R9)
KEPT_ENTRY = '    {0xB018D143700AB803ull, 0x258B95AC99520C1Full, "null-output quad"},\n'
KEPT_MATCH = "        if (kUiSceneKeptPairs[i].vs == vs && kUiSceneKeptPairs[i].ps == ps) return static_cast<int>(i);\n"
KEPT_GATE = "        ++seen;\n        if (family == 0) {\n"
KEPT_COUNT = "                ++kept;\n                ++keptDraws[k];\n                return;\n"
KEPT_PER_PAIR = "                ++keptDraws[k];\n"
KEPT_LOOP = "        if (!w.keptDraws[i]) continue;\n"
KEPT_FMT = '"vs %016llX ps %016llX (%s, kept in the scene by design) %llu draws, %.2f a frame"'
KEPT_RATE = "static_cast<double>(w.keptDraws[i]) * perFrame);\n"
KEPT_COUNT_ARG = "                      kUiSceneKeptPairs[i].name, static_cast<unsigned long long>(w.keptDraws[i]),\n"
NONE_TEXT = '        out += "none: every interface composite drawn into an eye went into the layer";\n'
# ui_layer_math.h: the rule's exclusion branch
EXCLUDED_BRANCH = "    } else if (f.excluded) {\n        w = UiFamilyWhy::kExcluded;\n"
PAST_IF = "    if (w.pastTable) {\n"
PAST_TEXT = "%llu draws of pairs past the table's %u (%.2f a frame)"
FULL_STOP = '    out += ".";\n    return out;\n'
FMT_CLAMP = "    const size_t n = text.size() < cap - 1 ? text.size() : cap - 1;\n"
FMT_TERMINATE = "    out[n] = 0;\n"
FMT_REFUSE = "    if (!out || !cap) return 0;\n"

MATH = "ui_layer_math.h"
MAT = "holo_material.h"

MUTANTS = [
    # ---- R1: the family rule ---------------------------------------------------------------------------------------------------
    M("holo-vs-stock-only", "R1b", [(PRED, "constexpr bool uiVsIsHoloPanel(uint64_t vs) { return vs == kUiVsHolo; }\n")],
      "the predicate names only the stock holo-panel vertex shader: Disable GUI effects stays unnamed (the field's defect)", MATH),
    M("holo-vs-effects-off-only", "R1a", [(PRED, "constexpr bool uiVsIsHoloPanel(uint64_t vs) { return vs == kUiVsHoloGuiFxOff; }\n")],
      "the predicate names only the new vertex shader: the stock panels are dropped", MATH),
    M("holo-vs-anything", "R1d", [(PRED, "constexpr bool uiVsIsHoloPanel(uint64_t vs) { return vs == kUiVsHolo || vs == kUiVsHoloGuiFxOff || vs != 0; }\n")],
      "every vertex shader is a holo panel", MATH),
    M("hdr-branch-stock-only", "R1b", [(HDR_USE, "        out = f.vs == kUiVsHolo    ? UiLayerFamily::kHolo\n")],
      "the lit HDR target's branch compares the stock hash by itself: the panels with the setting on are not named there", MATH),
    M("ldr-branch-stock-only", "R1c", [(LDR_USE, "              : f.vs == kUiVsHolo  ? UiLayerFamily::kHolo\n")],
      "the post-tonemap branch compares the stock hash by itself", MATH),
    M("hdr-branch-wrong-family", "R1a", [(HDR_USE, "        out = uiVsIsHoloPanel(f.vs)    ? UiLayerFamily::kFlightHud\n")],
      "the HDR branch names the holo panels as the flight HUD", MATH),
    M("hdr-sprite-lost", "R1i", [(HDR_SPRITE, "              : f.vs == kUiVsFlightHud ? UiLayerFamily::kFlightHud\n              : f.vs == kUiVsSprite    ? UiLayerFamily::kHolo\n")],
      "the target sprite is named as a holo panel on the HDR target", MATH),
    M("layer-constant-off-by-one", "R1b", [(KUI, "constexpr uint64_t kUiVsHoloGuiFxOff = 0x1989E6D3B405FDE1ull;\n")],
      "the layer's constant is not the disassembled hash", MATH),
    M("family-name-changed", "R1h", [(FAMILY_NAME, '        case UiLayerFamily::kHolo: return "holo panels";\n')],
      "the family is census-named something the reader and the lines do not say", MATH),
    M("material-vs-typo", "R1b", [(MAT_VS, "constexpr uint64_t kHoloGuiFxOffVs = 0x1989E6D3B405FDE1ull;\n")],
      "the vertex shader hash in holo_material.h is one bit off: the disassembled shader is not named", MAT),
    M("material-ps-typo", "R1f", [(MAT_PS, "constexpr uint64_t kHoloGuiFxOffPs = 0xEAB8A1C95A13FFBFull;\n")],
      "the pixel shader hash is one bit off: the pair is not the one the dump named", MAT),
    # ---- R8: the setting off changes nothing (the frozen rule) --------------------------------------------------------------------
    M("ldr-loader-becomes-surface", "R8a", [(LDR_LOADER, "              : f.vs == kUiVsLoader    ? UiLayerFamily::kSurface\n")],
      "the loading screen's composite over a learned surface is named a generic surface: a draw that has nothing to do with the new shader changes", MATH),
    M("no-surface-reason-lost", "R8a", [(NO_SURFACE, "    } else if (f.vs == kUiVsPanel || f.vs == kUiVsLoader) {\n        w = UiFamilyWhy::kOther;\n    }\n")],
      "a composite shader with no learned surface and an unknown pair loses its reason", MATH),
    M("sphere-second-ps-dropped", "R8a", [(SPHERE_PS, "                  (f.ps == 0xEA02FAC2BD6C643Cull)))\n")],
      "the target sphere's second pixel shader is no longer a hologram of the take", MATH),
    M("effects-off-answers-as-sprite", "R8a", [(HDR_USE, "        out = uiVsIsHoloPanel(f.vs)    ? (f.vs == kUiVsHoloGuiFxOff ? UiLayerFamily::kSprite : UiLayerFamily::kHolo)\n")],
      "the new shader is named, but not as the stock one is", MATH),
    # ---- R2: the table ---------------------------------------------------------------------------------------------------------
    M("fresh-window-not-empty", "R2a", [(USED, "    uint32_t used = 1;\n")], "a fresh window already names a pair"),
    M("taken-not-seen", "R2b", [(NOTE_TAKEN, "    void noteTaken() {}\n")], "a taken composite is not counted as seen"),
    M("left-not-seen", "R2c", [(NOTE_SEEN, "    void noteLeft(uint64_t vs, uint64_t ps, int family) {\n")], "a left composite is not counted as seen"),
    M("left-not-left", "R2c", [(NOTE_LEFT, "        for (uint32_t i = 0; i < used; ++i) {\n")], "a left composite is not counted as left"),
    M("pair-key-ignores-ps", "R2e", [(PAIR_MATCH, "            if (p.vs == vs && p.family == family) {\n")], "two pixel shaders of one vertex shader are one pair"),
    M("pair-key-ignores-vs", "R2e", [(PAIR_MATCH, "            if (p.ps == ps && p.family == family) {\n")], "two vertex shaders of one pixel shader are one pair"),
    M("pair-key-ignores-family", "R2e", [(PAIR_MATCH, "            if (p.vs == vs && p.ps == ps) {\n")], "one pair named by two families is one entry"),
    M("pair-never-aggregates", "R2d", [(PAIR_MATCH, "            if (false) {\n")], "every draw of a pair is a new entry"),
    M("table-one-short", "R2f", [(PAIR_ROOM, "        if (used + 1 < kUiSceneCompositePairs) {\n")], "the table holds one pair fewer than it says"),
    M("overflow-not-counted", "R2g", drop(PAST), "draws of pairs past the table are lost"),
    M("reset-incomplete", "R2h", [(RESET, "    void reset() { seen = left = 0; }\n")], "a window's reset keeps the table and the live frames"),
    # ---- R3: the zero line ------------------------------------------------------------------------------------------------------
    M("none-line-silent", "R3a", [(NO_LEFT, "    if (!w.left && false) {\n")], "nothing left prints no sentence at all: a zero that says nothing"),
    M("idle-line-says-none", "R3b", [(NO_SEEN, "    if (!w.seen && false) {\n")], "no composite drawn is told as \"none left\" -- the two are not told apart"),
    M("prefix-changed", "R3a", [(PREFIX, '    std::string out = "ui quality: composites in the scene: ";\n')], "the line's prefix, which the reader keys on, changes"),
    M("detector-always-off", "R3a", [(DETECTOR, "    if (true) {\n")], "every window says NOT COUNTED, so a running pass's zero is never seen"),
    # ---- R4: the line with pairs --------------------------------------------------------------------------------------------------
    M("total-rate-per-composite", "R4a", [(HEAD_RATE, "static_cast<double>(w.left) / (w.seen ? static_cast<double>(w.seen) : 1.0),")],
      "the total rate is draws a composite, not draws a frame"),
    M("live-frames-shown-as-frames", "R4a", [(HEAD_LIVE, "static_cast<unsigned long long>(frames));")], "the live frames are the window's frames"),
    M("pair-rate-raw-count", "R4a", [(PAIR_RATE, "                      static_cast<double>(p.draws));\n")], "a pair's rate is its draw count"),
    M("hash-lowercase", "R4a", [(PAIR_FMT, '"vs %016llx ps %016llx (%s) %.2f a frame",\n')], "hashes in lower case"),
    M("hash-unpadded", "R4d", [(PAIR_FMT, '"vs %llX ps %llX (%s) %.2f a frame",\n')], "hashes without their leading zeros"),
    M("no-family-unlabelled", "R4a", [(FAMILY_NONE, '        std::snprintf(label, sizeof(label), "unknown");\n')], "a composite no family names is called something else"),
    M("named-family-not-taken-missing", "R4b", [(FAMILY_NAMED, '"%s"')], "a named family left in the scene does not say it was not taken"),
    M("pair-separator", "R4b", [(JOIN_SEP, '        if (!first) out += ", ";\n')], "pairs are separated by commas"),
    M("past-table-silent", "R4e", [(PAST_IF, "    if (w.pastTable && false) {\n")], "draws of pairs past the table are not said"),
    M("past-table-text", "R4e", [(PAST_TEXT, "%llu draws of unnamed pairs (%u, %.2f a frame)")], "the overflow sentence changes"),
    M("pairs-sixteen-text", "R4e", [(PAIRS, "constexpr size_t kUiSceneCompositePairs = 16;\n")], "the table holds sixteen: the sentence names a table of sixteen"),
    M("full-stop-dropped", "R4a", [(FULL_STOP, "    return out;\n")], "the line with pairs does not end with a full stop"),
    # ---- R5: the length ----------------------------------------------------------------------------------------------------------
    M("pairs-sixteen-length", "R5a", [(PAIRS, "constexpr size_t kUiSceneCompositePairs = 16;\n")], "a full table of sixteen runs past the Log's 1200 characters"),
    M("format-unterminated", "R5b", drop(FMT_TERMINATE), "a truncated line is not terminated"),
    M("format-overruns", ("R5b", "R5c"), [(FMT_CLAMP, "    const size_t n = text.size() < cap - 1 ? text.size() : cap;\n")], "the line is cut one byte past the buffer"),
    M("format-refusal-answers-one", "R5d", [(FMT_REFUSE, "    if (!out || !cap) return 1;\n")], "a refused buffer reports a byte written"),
    M("format-drops-the-last-character", "R5e", [(FMT_CLAMP, "    const size_t n = text.size() < cap - 1 ? text.size() - 1 : cap - 1;\n")],
      "a buffer that holds the whole line gets all but its last character"),
    # ---- R6: the detector off ---------------------------------------------------------------------------------------------------
    M("detector-never-on", "R6a", [(DETECTOR, "    if (false) {\n")], "the interface depth pass off still prints a count: a zero that means nothing"),
    M("detector-off-only-when-empty", "R6b", [(DETECTOR, "    if (!detectorOn && !w.seen) {\n")], "NOT COUNTED only when nothing was counted"),
    # ---- R7: a window with no frames ----------------------------------------------------------------------------------------------
    M("no-frames-divides", "R7a", [(PER_FRAME, "    const double perFrame = 1.0 / static_cast<double>(frames);\n")], "a window with no frames divides by zero"),
    # ---- R9: the pair the scene keeps on purpose ------------------------------------------------------------------------------------
    M("kept-vs-off-by-one", "R9a", [(KEPT_ENTRY, '    {0xB018D143700AB804ull, 0x258B95AC99520C1Full, "null-output quad"},\n')],
      "the kept table's vertex shader is one bit off: the dumped null-output quad is left, and the reader STOPs on every loading screen again"),
    M("kept-ps-off-by-one", "R9a", [(KEPT_ENTRY, '    {0xB018D143700AB803ull, 0x258B95AC99520C20ull, "null-output quad"},\n')],
      "the kept table's pixel shader is not the dumped one"),
    M("kept-name-changed", "R9a", [(KEPT_ENTRY, '    {0xB018D143700AB803ull, 0x258B95AC99520C1Full, "quad"},\n')],
      "the kept pair is named something the reader and the doc do not say"),
    M("kept-key-ignores-ps", "R9d", [(KEPT_MATCH, "        if (kUiSceneKeptPairs[i].vs == vs) return static_cast<int>(i);\n")],
      "the vertex shader alone is kept: a real composite drawn with that vertex shader would be hidden"),
    M("kept-key-ignores-vs", "R9d", [(KEPT_MATCH, "        if (kUiSceneKeptPairs[i].ps == ps) return static_cast<int>(i);\n")],
      "the pixel shader alone is kept: every draw of the zero-output pixel shader would be hidden"),
    M("kept-ignores-family", "R9e", [(KEPT_GATE, "        ++seen;\n        if (true) {\n")],
      "a pair a family names is kept as well: the layer's own business hides in the kept count"),
    M("kept-also-left", "R9b", [(KEPT_COUNT, "                ++kept;\n                ++left;\n                ++keptDraws[k];\n                return;\n")],
      "a kept draw is counted as left too: the defect count the reader STOPs on includes it"),
    M("kept-not-seen", "R9b", [(KEPT_GATE, "        if (family != 0) ++seen;\n        if (family == 0) {\n")],
      "a kept draw is not counted as a composite draw at all"),
    M("kept-not-per-pair", "R9b", drop(KEPT_PER_PAIR), "the kept pair's own draw count is never kept"),
    M("kept-clause-silent", "R9f", [(KEPT_LOOP, "        continue;\n")], "the kept clause is never said: the pair is hidden, not named"),
    M("kept-clause-wording", "R9f", [(KEPT_FMT, '"vs %016llX ps %016llX (%s, kept) %llu draws, %.2f a frame"')], "the kept clause stops saying why it is not a defect: \"kept in the scene by design\""),
    M("kept-rate-raw", "R9f", [(KEPT_RATE, "static_cast<double>(w.keptDraws[i]));\n")], "the kept pair's rate is its draw count"),
    M("kept-count-wrong", "R9f", [(KEPT_COUNT_ARG, "                      kUiSceneKeptPairs[i].name, static_cast<unsigned long long>(w.keptDraws[i] + 1),\n")],
      "the kept clause's draw count is not the count"),
    M("kept-hides-none", "R9f", [(NO_LEFT, "    if (!w.left && !w.kept) {\n")], "a window with only the kept pair no longer says none: the zero reads as missing"),
    M("kept-none-wording", "R9f", [(NONE_TEXT, '        out += "none left";\n')], "the sentence that says every composite went into the layer changes"),
    M("exclusion-branch-dropped", "R9j", [(EXCLUDED_BRANCH, "    } else if (false) {\n        w = UiFamilyWhy::kExcluded;\n")],
      "the family rule ignores ui depth's exclusion: the null-output quad over a learned surface is named a generic surface and TAKEN", MATH),
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


def parse_fail(output):
    """The text after the last 'FAIL: ' line of the rig's output, or None when it printed none."""
    found = None
    for line in output.splitlines():
        if line.startswith("FAIL: "):
            found = line[len("FAIL: "):]
    return found


def rig_cases(rig_text):
    return set(re.findall(r'\{"(R\d+)", case\w+\}', rig_text))


def rig_label_exists(rig_text, prefix):
    return ('"%s:' % prefix) in rig_text


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


# ---- the toolchain -----------------------------------------------------------------------------------------------------------
class Toolchain:
    """cl.exe by absolute path (Windows looks a bare name up on THIS process's PATH, not on the env passed), with the environment
    it needs: this one if cl is on PATH, else the one vcvars64.bat makes, found with vswhere as build.bat does."""

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


def build_rig(tc, header_dir, outdir):
    """Compile the rig against the headers in header_dir (and only there); (exit code, output)."""
    exe = outdir / "rig.exe"
    cmd = [tc.cl] + CL_FLAGS + ["/I" + str(header_dir), "/Fo" + str(outdir) + os.sep, "/Fe" + str(exe), str(RIG)]
    done = subprocess.run(cmd, capture_output=True, text=True, env=tc.env, errors="replace", cwd=str(outdir))
    return done.returncode, (done.stdout + done.stderr), exe


def run_rig(exe, rule, tc):
    """(outcome, detail) of one run of the rig: 'pass', 'fail' (detail = the label), 'crash', 'timeout'."""
    cmd = [str(exe), "--self-test"] + (["--only", rule] if rule else [])
    try:
        done = subprocess.run(cmd, capture_output=True, text=True, env=tc.env, errors="replace", timeout=RUN_TIMEOUT)
    except subprocess.TimeoutExpired:
        return "timeout", "no result within %d s" % RUN_TIMEOUT
    if done.returncode == 0:
        return "pass", ""
    label = parse_fail(done.stdout + "\n" + done.stderr)
    if label is None:
        return "crash", "exit 0x%08X" % (done.returncode & 0xFFFFFFFF)
    return "fail", label


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
    lines = ["%d mutation(s); each header set is compiled with the rig in a temp directory outside the repo and run on its rule's cases:" % len(mutants)]
    for m in mutants:
        lines.append("  %-34s %-22s rule %-3s caught by %-12s %s" % (m.name, m.header, m.rule, "/".join(m.caught), m.why))
    lines.append("cl.exe flags: " + " ".join(CL_FLAGS))
    return "\n".join(lines)


def run_all(only=None, jobs=None, keep=False, dry_run=False, out=sys.stdout):
    mutants = select(only)
    if dry_run:
        print(plan_text(mutants), file=out)
        print("dry-run: nothing compiled, started or written", file=out)
        return 0
    prod = {h: read_source(SRC / h) for h in HEADERS}
    tc = Toolchain()
    work = Path(tempfile.mkdtemp(prefix="ui_composite_census_mutants_"))
    try:
        def build_and_run(name, texts, rule):
            d = work / name
            d.mkdir()
            for h, text in texts.items():
                (d / h).write_text(text, encoding="utf-8", newline="\n")
            code, text, exe = build_rig(tc, d, d)
            if code != 0:
                return "nocompile", text.strip().splitlines()[-1] if text.strip() else ""
            return run_rig(exe, rule, tc)

        control, detail = build_and_run("control", prod, None)
        print("control (the unmutated headers, every case): %s %s" % (control, detail), file=out)
        if control != "pass":
            print("the rig does not pass on the unmutated headers when built this way; nothing below means anything", file=out)
            return 1

        def one(m):
            try:
                texts = dict(prod)
                texts[m.header] = apply_edits(prod[m.header], m.edits, m.name)
            except ValueError as error:
                return m, "badedit", str(error)
            outcome, detail = build_and_run(m.name, texts, m.rule)
            return m, outcome, detail

        results = []
        with concurrent.futures.ThreadPoolExecutor(max_workers=jobs or min(8, os.cpu_count() or 2)) as pool:
            for m, outcome, detail in pool.map(one, mutants):
                if outcome == "fail" and any(detail.startswith(p + ":") for p in m.caught):
                    verdict = "caught"
                elif outcome == "fail":
                    verdict = "OTHER"
                else:
                    verdict = {"pass": "SURVIVED", "crash": "CRASH", "timeout": "TIMEOUT", "nocompile": "NOCOMPILE", "badedit": "BADEDIT"}[outcome]
                results.append((m, verdict, detail))
                print("%-9s %-34s %-4s %s" % (verdict, m.name, m.rule, detail[:150]), file=out)
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


# ---- the self-test ------------------------------------------------------------------------------------------------------------------
def self_test():
    failures = []

    def check(ok, what):
        if not ok:
            failures.append(what)

    # the tool's own pieces
    check(parse_fail("a\nFAIL: R1c: x\nb\n") == "R1c: x" and parse_fail("PASS: 3\n") is None, "parse_fail reads the last FAIL: line")
    check(apply_edits("a b c", [("b", "B")]) == "a B c", "apply_edits applies one edit")
    for bad, why in ((("a b b", [("b", "B")]), "a repeated anchor"), (("a b", [("x", "y")]), "a missing anchor"), (("a b", [("b", "b")]), "a no-op edit")):
        try:
            apply_edits(*bad)
            check(False, "apply_edits refuses " + why)
        except ValueError:
            pass
    joined = label_block(":rig_a\nx ^\n y\nexit /b 0\n:rig_b\nz\n", ":rig_a")
    check(joined is not None and [" ".join(l.split()) for l in joined] == ["x y", "exit /b 0"], "label_block joins continuations and stops at the next label")
    check(rig_cases('{"R1", caseR1}, {"R10", caseR10}') == {"R1", "R10"}, "rig_cases reads the case table")

    # every mutation against the header as it is now, and against the rig
    prod = {h: read_source(SRC / h) for h in HEADERS}
    rig = read_source(RIG)
    cases = rig_cases(rig)
    names = [m.name for m in MUTANTS]
    check(len(names) == len(set(names)), "mutation names are unique")
    check(len(MUTANTS) >= 40, "the mutation list did not shrink (%d)" % len(MUTANTS))
    for m in MUTANTS:
        check(m.header in prod and m.header in ("ui_layer_math.h", "ui_scene_composites.h", "holo_material.h"), "%s: names a header the tool holds (%s)" % (m.name, m.header))
        if m.header not in prod:
            continue
        try:
            mutated = apply_edits(prod[m.header], m.edits, m.name)
            check(mutated != prod[m.header], "%s changes the header" % m.name)
        except ValueError as error:
            failures.append(str(error))
        for p in m.caught:
            check(rig_label_exists(rig, p), "%s: the rig has no check labelled %s" % (m.name, p))
        check(m.rule in cases, "%s: the rig has no case %s" % (m.name, m.rule))
    rules = {m.rule for m in MUTANTS}
    check(rules == cases, "every rule of the rig has a mutation, and only rules of the rig: %s vs %s" % (sorted(rules), sorted(cases)))

    # the include closure: every header the rig's two headers include is one this tool copies
    closure = set()
    todo = ["ui_layer_math.h", "ui_scene_composites.h"]
    while todo:
        h = todo.pop()
        if h in closure:
            continue
        closure.add(h)
        for inc in re.findall(r'^#include "([^"]+)"', prod[h] if h in prod else read_source(SRC / h), re.M):
            check(inc in HEADERS, "%s includes %s, which this tool does not copy: a mutated copy would resolve it from src\\d3d11 beside the production copies" % (h, inc))
            if inc in HEADERS:
                todo.append(inc)
    check(closure == set(HEADERS), "the headers copied are exactly the include closure of the rig's headers: %s vs %s" % (sorted(closure), sorted(HEADERS)))

    # build.bat compiles the rig the way this tool does, and runs this tool's self-test
    bat = BUILD_BAT.read_bytes().decode("utf-8", errors="replace")
    block = label_block(bat, RIG_LABEL)
    check(block is not None, "build.bat has %s" % RIG_LABEL)
    if block:
        text = "\n".join(block)
        cl = next((l for l in block if l.strip().lower().startswith("cl.exe")), "")
        for flag in CL_FLAGS:
            check(flag in cl, "build.bat's rig compile has %s" % flag)
        check("tools\\ui_composite_census_test\\ui_composite_census_test.cpp" in cl, "build.bat compiles the rig")
        check('/I"src\\d3d11"' in cl, "build.bat's rig compile finds the headers through /I src\\d3d11 (the mutated copies are found the same way, from their own directory)")
        check("ui_composite_census_test.exe\" --dry-run" in text and "ui_composite_census_test.exe\" --self-test \"%ROOT%\"" in text,
              "build.bat runs the rig's --dry-run and --self-test with the repo root")
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
    check(code == 0 and not calls and "dry-run" in sink.getvalue(), "--dry-run starts no process and makes no directory (saw %s)" % calls)
    try:
        select("no-such-mutation")
        check(False, "--only names an unknown mutation")
    except ValueError:
        pass

    if failures:
        print("ui_composite_census_test mutants self-test: FAILED")
        for f in failures:
            print("  " + f)
        return 1
    print("ui_composite_census_test mutants self-test: ok (%d mutations over %d rules)" % (len(MUTANTS), len(rules)))
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(description="Mutation proof for tools\\ui_composite_census_test.")
    ap.add_argument("--self-test", action="store_true", help="text only: anchors, labels and build.bat; compiles nothing")
    ap.add_argument("--run", action="store_true", help="compile the rig against each mutated header set and run it (needs the MSVC toolchain)")
    ap.add_argument("--list", action="store_true", help="list the mutations")
    ap.add_argument("--only", default="", help="comma-separated mutation names")
    ap.add_argument("--jobs", type=int, default=0)
    ap.add_argument("--keep", action="store_true", help="keep the temp directory")
    ap.add_argument("--dry-run", action="store_true", help="with --run: print the plan, start and write nothing")
    args = ap.parse_args(argv)
    if args.self_test:
        return self_test()
    if args.list:
        print(plan_text(MUTANTS))
        return 0
    if args.run:
        try:
            return run_all(only=args.only, jobs=args.jobs or None, keep=args.keep, dry_run=args.dry_run)
        except (ValueError, RuntimeError) as error:
            print("error: %s" % error, file=sys.stderr)
            return 2
    ap.print_help()
    return 2


if __name__ == "__main__":
    sys.exit(main())
