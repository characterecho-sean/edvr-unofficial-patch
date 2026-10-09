#!/usr/bin/env python3
"""The mutation proof for tools\\dlaa_mode_test: the rig fails when a rule of the DLSS ceiling (the output NGX has no answer for is cut to the largest one it has) is broken.

The rig (dlaa_mode_test.cpp, its last section) drives the pure rule in src\\d3d11\\dlss_floor.h with NGX's measured behaviour: an output with max(width,
height) <= 8192 gets a usable ladder, and anything past that gets ok = true with every size ZERO on all four modes (the 2026-10-09 Pimax flight, 8268x3948).
It pins what makes a mode ANSWERED (C1 zero_ladder, C2 answered_ladder, C3 single_point, C4 partial), the search's answer (C5 ceiling_8268 -> 8192x3910,
C6 ceiling_tall, C7 ceiling_square, C8 ceiling_odd), that nothing answered gives false and zero outputs (C9 ceiling_none), that only a size the search saw
answered is returned (C10 ceiling_verified), that it is a bisection (C11 ceiling_calls), that it follows the query rather than a hard-coded 8192 (C12
ceiling_pixel_model), and that the cut output serves the 2x input (C13 window_mode). Every failure carries a label "C<n>.<case>.<what>"; the number is there
because tools\\rig_mutants_lib.py reads a case id as a prefix plus digits. A rig that passes proves little until it is seen to FAIL on a source that breaks the
rule it pins: for each mutation below the machinery copies the header into a temp directory OUTSIDE the repo, applies the edit, rebuilds the rig against the
copy and requires a FAIL on a check of the case that belongs to the rule.

  python tools\\dlaa_mode_test\\mutants.py --self-test    text only: every anchor is found exactly once, every case named is in the rig, and
                                                          build.bat compiles the rig the way the machinery does and runs this self-test
  python tools\\dlaa_mode_test\\mutants.py --run [--only a,b] [--jobs N] [--keep] [--verbose]
  python tools\\dlaa_mode_test\\mutants.py --list
  python tools\\dlaa_mode_test\\mutants.py --run --dry-run   the plan; writes nothing, starts nothing

--run starts from the repo root: the rig's older section reads src\\d3d11\\dlaa.cpp as text, relative to the working directory.
"""
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))
import rig_mutants_lib as lib  # noqa: E402

ROOT = HERE.parents[1]
FILES = {
    "floor": ROOT / "src" / "d3d11" / "dlss_floor.h",
}
# the rig includes its sources by relative path (../../src/d3d11/...), so the rig itself and the header it includes beside dlss_floor.h sit in the temp tree
TREE_EXTRA = [
    ("tools/dlaa_mode_test/dlaa_mode_test.cpp", HERE / "dlaa_mode_test.cpp"),
    ("src/d3d11/dlaa.h", ROOT / "src" / "d3d11" / "dlaa.h"),
]
CONFIG = lib.Config(
    here=__file__, files=FILES, header_keys=("floor",), pin_keys=(), rig=HERE / "dlaa_mode_test.cpp",
    rig_label=":rig_dlaa_mode_test", rig_source_in_bat="tools\\dlaa_mode_test\\dlaa_mode_test.cpp", rig_exe_in_bat="dlaa_mode_test.exe",
    case_prefix="C", include_dirs=(), min_mutants=20, run_timeout=120.0, tree_extra=TREE_EXTRA, rig_in_tree=True,
    rig_args=lambda root: ["--self-test"])   # this rig takes exactly one argument

# the cases, by name (the labels in the rig read "C<n>.<name>.<what>")
ZERO_LADDER, ANSWERED_LADDER, SINGLE_POINT, PARTIAL = "C1", "C2", "C3", "C4"
CEILING_8268, CEILING_TALL, CEILING_SQUARE, CEILING_ODD, CEILING_NONE = "C5", "C6", "C7", "C8", "C9"
CEILING_VERIFIED, CEILING_CALLS, CEILING_PIXEL_MODEL, WINDOW_MODE = "C10", "C11", "C12", "C13"

ANSWERED = ("return m.ok && m.optW && m.optH && m.minW && m.minH && m.maxW && m.maxH &&\n"
            "           m.minW <= m.maxW && m.minH <= m.maxH;")
ANY_ANSWERED = "if (dlssModeAnswered(modes[k])) return true;"
SERVE_MIN = "w >= m.minW"
SERVE_MAX = " && w <= m.maxW && h <= m.maxH"
HEIGHT = "(uint64_t(doorH) * w / doorW) & ~uint64_t(1);"
CEILING_RANGE = "uint32_t lo = 0, hi = doorW / 2, loH = 0;"
MID = "const uint32_t mid = lo + (hi - lo + 1) / 2;"
ANSWERED_PROBE = "answered = dlssRangesAnswered(modes);"
RAISE_LO = "lo = mid;"
NOTHING = "if (!lo) return false;"
OUT_W = "if (outW) *outW = 2 * lo;"

M = lib.M
MUTANTS = [
    # ---- what makes a mode answered ----
    M("zero-ladder-answers", (ZERO_LADDER, PARTIAL), "floor", [(ANSWERED, "return m.ok;")],
      "a mode counts as answered when the call succeeded, as before: NGX's ok with every size zero (the flight's 8268x3948) is taken for a ladder"),
    M("answered-never", ANSWERED_LADDER, "floor", [(ANSWERED, "return false;")],
      "no mode is ever answered: every output looks past the ceiling"),
    M("point-not-answered", SINGLE_POINT, "floor", [("m.minW <= m.maxW && m.minH <= m.maxH;", "m.minW < m.maxW && m.minH < m.maxH;")],
      "a range of one size (ultra performance on the flights) is not an answer"),
    M("answered-ignores-ok", PARTIAL, "floor", [("return m.ok && m.optW && m.optH", "return m.optW && m.optH")],
      "a mode whose call failed but whose fields hold something is answered"),
    M("answered-ignores-opt", PARTIAL, "floor", [("m.ok && m.optW && m.optH && m.minW", "m.ok && m.minW")],
      "a mode with no optimal size is answered"),
    M("answered-ignores-min", PARTIAL, "floor", [("m.optH && m.minW && m.minH && m.maxW", "m.optH && m.maxW")],
      "a mode with no minimum is answered"),
    M("answered-ignores-inverted", PARTIAL, "floor", [("m.maxH &&\n           m.minW <= m.maxW && m.minH <= m.maxH;", "m.maxH;")],
      "a mode whose minimum is above its maximum is answered"),
    M("ranges-answered-on-ok", (ZERO_LADDER, CEILING_8268, CEILING_NONE), "floor", [(ANY_ANSWERED, "if (modes[k].ok) return true;")],
      "an output is answered when any mode's call succeeded, whatever it returned: the zero ladder passes and the ceiling is never found"),
    M("ranges-first-mode-only", SINGLE_POINT, "floor", [(ANY_ANSWERED, "if (k == 0 && dlssModeAnswered(modes[k])) return true;")],
      "only quality is looked at: an output that only a later mode answers is past the ceiling"),
    # ---- the search ----
    M("ceiling-never-moves", (CEILING_8268, CEILING_TALL, CEILING_ODD), "floor", [(RAISE_LO, "hi = mid - 1;")],
      "an answered probe never raises the answer: the search finds nothing and returns false (the old flight's 8268x3948 stays as it was)"),
    M("ceiling-height-not-even", (CEILING_8268, CEILING_ODD), "floor", [(HEIGHT, "(uint64_t(doorH) * w / doorW);")],
      "the height is left odd: 3911, not 3910"),
    M("ceiling-height-not-scaled", (CEILING_8268, CEILING_TALL), "floor", [(HEIGHT, "uint64_t(doorH) & ~uint64_t(1);")],
      "the height stays the door's whatever the width: the aspect is lost"),
    M("ceiling-width-not-verified", (CEILING_8268, CEILING_VERIFIED), "floor", [(OUT_W, "if (outW) *outW = doorW;")],
      "the width returned is the door's, not the one seen answered"),
    M("ceiling-skips-the-door", CEILING_SQUARE, "floor", [(CEILING_RANGE, "uint32_t lo = 0, hi = doorW / 2 - 1, loH = 0;")],
      "the search never asks for the door's own width: a door NGX answers is cut by a pair of pixels"),
    M("ceiling-query-ignored", (CEILING_NONE, CEILING_VERIFIED, CEILING_PIXEL_MODEL), "floor", [(ANSWERED_PROBE, "answered = w <= 8192 && hh <= 8192;")],
      "NVIDIA's measured limit is hard-coded and the query's answer is not read: it holds on this driver and nowhere else"),
    M("ceiling-linear-scan", CEILING_CALLS, "floor", [(MID, "const uint32_t mid = hi;")],
      "the door is asked for first and the search steps down a pair of pixels at a time: right answers, but dozens of NGX queries on the render thread (thousands for a wider door), not a dozen"),
    M("ceiling-none-returns-true", CEILING_NONE, "floor", [(NOTHING, "if (!lo) return true;")],
      "an output with nothing answered at or under the door is reported as found, with a size of 0x0"),
    # ---- the cut output is served ----
    M("serve-floor-exclusive", WINDOW_MODE, "floor", [(SERVE_MIN, "w > m.minW")],
      "an input exactly on a mode's floor is not served: the 2x input at the cap is cut again"),
    M("serve-max-exclusive", WINDOW_MODE, "floor", [("w <= m.maxW && h <= m.maxH", "w < m.maxW && h < m.maxH")],
      "an input exactly at a mode's maximum is not served"),
    M("serve-ignores-max", (ZERO_LADDER, WINDOW_MODE), "floor", [(SERVE_MAX, "")],
      "a mode serves any input above its minimum: the zero ladder serves every size"),
]

if __name__ == "__main__":
    sys.exit(lib.main(CONFIG, MUTANTS))
