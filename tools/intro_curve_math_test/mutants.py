#!/usr/bin/env python3
"""The mutation proof for tools\\intro_curve_math_test: the rig fails when a rule of the intro composite's constants is flipped.

The rig (intro_curve_math_test.cpp) pins the rules of src\\d3d11\\intro_curve_math.h, cases C1..C15 (its header says which). A rig that
passes proves little until it is seen to FAIL on a header that breaks the rule it pins. This tool does that: for each mutation below
it copies the production header into a temp directory OUTSIDE the repo, applies one textual edit (or a few that belong together),
compiles the rig against that copy, runs it on the rule's case alone, and requires the rig to fail on a check that belongs to the rule
(the label of the first FAIL starts with the case's id and a dot: "C4." and not "C40." or "C1" for "C10."). Nothing is written inside
the repo; the temp directory is removed at the end.

  python tools\\intro_curve_math_test\\mutants.py --self-test        text only: every anchor is found exactly once in the header as it
                                                                    is now, every label named is in the rig, and build.bat compiles
                                                                    the rig the way this tool does
  python tools\\intro_curve_math_test\\mutants.py --run [--only a,b] [--jobs N] [--keep]
  python tools\\intro_curve_math_test\\mutants.py --list
  python tools\\intro_curve_math_test\\mutants.py --run --dry-run    the plan; writes nothing, starts nothing
  --build-bat PATH   read another build.bat in the self-test (one with the :rig_intro_curve_math_test block, before it is in the repo's)

--run needs the MSVC toolchain (cl.exe on PATH, or Visual Studio found with vswhere as build.bat does) and takes about a minute.
--self-test runs in build.bat's rig and is what keeps an edit of the header from silently orphaning a mutation: if an anchor stops
matching, the build fails and this file says which.
"""
import argparse
import concurrent.futures
import io
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
PROD = ROOT / "src" / "d3d11" / "intro_curve_math.h"
RIG = HERE / "intro_curve_math_test.cpp"
BUILD_BAT = ROOT / "build.bat"
RIG_LABEL = ":rig_intro_curve_math_test"

# How build.bat compiles the rig; --self-test checks that the label still says the same.
CL_FLAGS = ["/nologo", "/O2", "/MT", "/std:c++17", "/EHsc", "/W4", "/DWIN32_LEAN_AND_MEAN", "/DNOMINMAX", "/D_CRT_SECURE_NO_WARNINGS"]
RUN_TIMEOUT = 90.0


class Mutant:
    def __init__(self, name, caught, edits, why):
        self.name = name
        self.caught = tuple([caught] if isinstance(caught, str) else caught)   # label prefixes that count as caught by its rule
        self.edits = list(edits)                                                # (old, new) pairs, applied in order
        self.why = why
        self.rule = re.match(r"C\d+", self.caught[0]).group(0)                  # the rig's case to run


def M(name, caught, edits, why):
    return Mutant(name, caught, edits, why)


# ---- float32 neighbours, for the mutations that move a threshold by one ulp ---------------------------------------------------
def f32(x):
    return struct.unpack("<f", struct.pack("<f", x))[0]


def _bits(x):
    return struct.unpack("<I", struct.pack("<f", x))[0]


def _from_bits(b):
    return struct.unpack("<f", struct.pack("<I", b))[0]


def next_down(x):
    """The float32 just below x, for a positive x."""
    return _from_bits(_bits(f32(x)) - 1)


def next_up(x):
    """The float32 just above x, for a positive x."""
    return _from_bits(_bits(f32(x)) + 1)


def lit(x):
    """A float32 as a C++ literal that parses back to exactly the same float."""
    return "%.9gf" % x


# ---- the header's lines, verbatim (the self-test finds each exactly once) -----------------------------------------------------
ZERO = "    auto zero = [](float v) { return v > -1e-9f && v < 1e-9f; };\n"
CB2_W = "    if (!zero(f[7]) || !zero(f[11])) return false;"
CB3_LOOP = "    for (uint32_t i = 12; i < 16; ++i) {"
W_TEST = "    if (f[19] < 0.999f || f[19] > 1.001f) return false;"
HALF_TEST = "    if (f[0] < 16.0f || f[1] < 16.0f) return false;"
FINITE_FN = "{ return v == v && v <= 3.4e38f && v >= -3.4e38f; }"
RESULT_INIT = '    IntroWorldCb r = {false, 0.0f, 0, ""};\n'
LENGTH_SUM = "std::sqrt(c0 * c0 + c1 * c1 + c2 * c2 + c3 * c3)"
LENGTH_TEST = "if (!(length >= kIntroCbMinLength && length <= kIntroCbMaxLength)) {"
W3_TEST = "if (!(std::fabs(f[15]) > kIntroCbMinW)) {"
W4_TEST = "if (!(std::fabs(f[19]) > kIntroCbMinW)) {"
X_TEST = "if (!(f[0] >= kIntroCbMinHalf && f[0] <= kIntroCbMaxHalf)) {"
Y_TEST = "if (!(f[1] >= kIntroCbMinHalf && f[1] <= kIntroCbMaxHalf)) {"
TOWARD = "    r.toward = (f[15] > 0.0f) != (f[19] > 0.0f) ? 1 : -1;\n"

# The x direction (introPlacementXDir) and the gate that asks it.
XA = "    const double a = static_cast<double>(f[4]) * static_cast<double>(f[19]);    // cb2[1].x * cb2[4].w\n"
XB = "    const double b = static_cast<double>(f[16]) * static_cast<double>(f[7]);    // cb2[4].x * cb2[1].w\n"
XMINOR = "    const double minor = a - b;\n"
XSCALE = "    const double scale = std::fabs(a) + std::fabs(b);\n"
XFINITE = "    if (!std::isfinite(minor) || !std::isfinite(scale)) return IntroXDir::kUnknown;"
XTEST = "    if (!(scale > 0.0) || !(std::fabs(minor) >= kIntroXDirMargin * scale)) return IntroXDir::kUnknown;"
XRETURN = "    return minor < 0.0 ? IntroXDir::kLeft : IntroXDir::kRight;\n"
XMARGIN = "constexpr double kIntroXDirMargin = 0.05;"
XGATE = "    if (dir == IntroXDir::kUnknown) {"
XCARRY = "    r.xDir = dir;\n"
XWHY = '"the placement\'s +x cannot be told to run left or right (the panel is seen edge-on, or its x column and its centre cancel)"'
XSCREEN = "    if (introCbLooksScreenSpace(f)) {\n"

# The reader's blocks, as written, for the mutations that put two of them in the other order.
BLOCK_FINITE = ('    for (int i = 0; i < 20; ++i) {\n        if (!introCbFinite(f[i])) {\n            r.why = "a constant is not a finite number";\n'
                '            return r;\n        }\n    }\n')
BLOCK_SCREEN = ('    if (introCbLooksScreenSpace(f)) {\n        r.why = "the constants read as a screen-space placement (cb2[3] is zero and w is a constant 1), '
                'not a world-space panel";\n        return r;\n    }\n')
BLOCK_LENGTH = ('    const double c0 = f[12], c1 = f[13], c2 = f[14], c3 = f[15];\n    const double length = ' + LENGTH_SUM + ';\n    ' + LENGTH_TEST +
                '\n        r.why = "cb2[3] is not a unit-scale column (its length is outside 0.5 to 2)";\n        return r;\n    }\n')
BLOCK_W3 = ('    ' + W3_TEST + '\n        r.why = "cb2[3].w is too close to zero to tell which way the panel\'s depth runs";\n        return r;\n    }\n')
BLOCK_W4 = ('    ' + W4_TEST + '\n        r.why = "cb2[4].w is too close to zero to tell which way the panel\'s depth runs";\n        return r;\n    }\n')
BLOCK_X = ('    ' + X_TEST + '\n        r.why = "cb2[0].x is not a plausible half-width in metres (outside 0.5 to 50)";\n        return r;\n    }\n')
BLOCK_Y = ('    ' + Y_TEST + '\n        r.why = "cb2[0].y is not a plausible half-height in metres (outside 0.5 to 50)";\n        return r;\n    }\n')
BLOCK_DIR = ('    const IntroXDir dir = introPlacementXDir(f);\n' + XGATE + '\n        r.why = ' + XWHY + ';\n        return r;\n    }\n')
MARK = "    /*@@swap@@*/\n"


def swap(a, b):
    """Edits that put two of the reader's blocks in the other order."""
    return [(a, MARK), (b, a), (MARK, b)]


MUTANTS = [
    # ---- C1: the two real captures ---------------------------------------------------------------------------------------------
    M("ok-never-set", "C1", [("    r.ok = true;\n", "")], "a reading is never ok"),
    M("screen-space-check-inverted", "C1", [("    if (introCbLooksScreenSpace(f)) {\n", "    if (!introCbLooksScreenSpace(f)) {\n")],
      "a world-space reading is refused as screen space"),
    M("reason-default-null", "C1", [(RESULT_INIT, '    IntroWorldCb r = {false, 0.0f, 0, nullptr};\n')], "an ok reading carries a null reason"),
    # ---- C2: the stock constants ----------------------------------------------------------------------------------------------
    M("screen-space-check-dropped", "C2", [("    if (introCbLooksScreenSpace(f)) {\n", "    if (false) {\n")], "the stock constants are refused for another reason"),
    # ---- C3: every refusal in its own words, and nothing left behind ---------------------------------------------------------------
    M("why-finite-reworded", "C3", [('"a constant is not a finite number"', '"a constant is NaN"')], "the finite reason is reworded"),
    M("why-screen-reworded", "C3", [("not a world-space panel\";", "not world space\";")], "the screen-space reason is reworded"),
    M("why-length-reworded", "C3", [('"cb2[3] is not a unit-scale column (its length is outside 0.5 to 2)"', '"cb2[3] is not unit length"')], "the length reason is reworded"),
    M("why-w3-reworded", "C3", [('"cb2[3].w is too close to zero to tell which way the panel\'s depth runs"', '"cb2[3].w is zero"')], "the cb2[3].w reason is reworded"),
    M("why-w4-reworded", "C3", [('"cb2[4].w is too close to zero to tell which way the panel\'s depth runs"', '"cb2[4].w is zero"')], "the cb2[4].w reason is reworded"),
    M("why-w4-is-w3", "C3", [('"cb2[4].w is too close to zero to tell which way the panel\'s depth runs"', '"cb2[3].w is too close to zero to tell which way the panel\'s depth runs"')],
      "the two w reasons are one text"),
    M("why-x-reworded", "C3", [('"cb2[0].x is not a plausible half-width in metres (outside 0.5 to 50)"', '"cb2[0].x is out of range"')], "the half-width reason is reworded"),
    M("why-y-reworded", "C3", [('"cb2[0].y is not a plausible half-height in metres (outside 0.5 to 50)"', '"cb2[0].y is out of range"')], "the half-height reason is reworded"),
    M("refusal-keeps-half-width", "C3", [(RESULT_INIT, '    IntroWorldCb r = {false, 1.0f, 0, ""};\n')], "a refusal reports a half-width"),
    M("refusal-keeps-direction", "C3", [(RESULT_INIT, '    IntroWorldCb r = {false, 0.0f, 1, ""};\n')], "a refusal reports a direction"),
    # ---- C4: NaN and infinity in every slot ----------------------------------------------------------------------------------------
    M("finite-check-dropped", "C4", [("if (!introCbFinite(f[i])) {", "if (false) {")], "nothing is refused as not finite"),
    M("finite-nan-only", "C4", [(FINITE_FN, "{ return v == v; }")], "an infinity counts as finite"),
    M("finite-upper-bound-dropped", "C4", [(FINITE_FN, "{ return v == v && v >= -3.4e38f; }")], "+infinity counts as finite"),
    M("finite-lower-bound-dropped", "C4", [(FINITE_FN, "{ return v == v && v <= 3.4e38f; }")], "-infinity counts as finite"),
    M("finite-bound-tightened", "C4", [(FINITE_FN, "{ return v == v && v <= 1e20f && v >= -1e20f; }")], "a huge but finite value is refused"),
    M("finite-first-slot-skipped", "C4", [("for (int i = 0; i < 20; ++i) {", "for (int i = 1; i < 20; ++i) {")], "slot 0 is never checked"),
    M("finite-last-slot-skipped", "C4", [("for (int i = 0; i < 20; ++i) {", "for (int i = 0; i < 19; ++i) {")], "slot 19 is never checked"),
    M("finite-fourth-column-skipped", "C4", [("for (int i = 0; i < 20; ++i) {", "for (int i = 0; i < 16; ++i) {")], "cb2[4] is never checked"),
    # ---- C5: cb2[3]'s length -------------------------------------------------------------------------------------------------------
    M("length-min-raised", "C5", [("constexpr float kIntroCbMinLength = 0.5f;", "constexpr float kIntroCbMinLength = 0.6f;")], "the shortest length is 0.6"),
    M("length-min-lowered", "C5", [("constexpr float kIntroCbMinLength = 0.5f;", "constexpr float kIntroCbMinLength = 0.4f;")], "the shortest length is 0.4"),
    M("length-max-lowered", "C5", [("constexpr float kIntroCbMaxLength = 2.0f;", "constexpr float kIntroCbMaxLength = 1.9f;")], "the longest length is 1.9"),
    M("length-max-raised", "C5", [("constexpr float kIntroCbMaxLength = 2.0f;", "constexpr float kIntroCbMaxLength = 2.1f;")], "the longest length is 2.1"),
    M("length-min-edge-exclusive", "C5", [("length >= kIntroCbMinLength", "length > kIntroCbMinLength")], "a length of exactly 0.5 is refused"),
    M("length-max-edge-exclusive", "C5", [("length <= kIntroCbMaxLength", "length < kIntroCbMaxLength")], "a length of exactly 2 is refused"),
    M("length-range-or", "C5", [("length >= kIntroCbMinLength && length <= kIntroCbMaxLength", "length >= kIntroCbMinLength || length <= kIntroCbMaxLength")],
      "every length passes"),
    M("length-check-dropped", "C5", [(LENGTH_TEST, "if (false) {")], "the length is never checked"),
    M("length-x-dropped", "C5", [("c0 * c0 + ", "")], "cb2[3].x is left out of the length"),
    M("length-y-dropped", "C5", [(" + c1 * c1", "")], "cb2[3].y is left out of the length"),
    M("length-z-dropped", "C5", [(" + c2 * c2", "")], "cb2[3].z is left out of the length"),
    M("length-w-dropped", "C5", [(" + c3 * c3", "")], "cb2[3].w is left out of the length"),
    M("length-sqrt-dropped", "C5", [(LENGTH_SUM, "(c0 * c0 + c1 * c1 + c2 * c2 + c3 * c3)")], "the sum of squares is the length"),
    # ---- C6: cb2[3].w and cb2[4].w clear of zero ----------------------------------------------------------------------------------------
    M("w3-edge-inclusive", "C6", [("std::fabs(f[15]) > kIntroCbMinW", "std::fabs(f[15]) >= kIntroCbMinW")], "|cb2[3].w| of exactly 1e-3 reads clear"),
    M("w4-edge-inclusive", "C6", [("std::fabs(f[19]) > kIntroCbMinW", "std::fabs(f[19]) >= kIntroCbMinW")], "|cb2[4].w| of exactly 1e-3 reads clear"),
    M("w3-sign-sensitive", "C6", [("std::fabs(f[15]) > kIntroCbMinW", "f[15] > kIntroCbMinW")], "a negative cb2[3].w is refused"),
    M("w4-sign-sensitive", "C6", [("std::fabs(f[19]) > kIntroCbMinW", "f[19] > kIntroCbMinW")], "a negative cb2[4].w is refused"),
    M("w3-check-dropped", "C6", [(W3_TEST, "if (false) {")], "cb2[3].w is never checked"),
    M("w4-check-dropped", "C6", [(W4_TEST, "if (false) {")], "cb2[4].w is never checked"),
    M("w-threshold-lowered", "C6", [("constexpr float kIntroCbMinW = 1e-3f;", "constexpr float kIntroCbMinW = 1e-5f;")], "a w of 1e-4 reads clear of zero"),
    M("w-threshold-raised", "C6", [("constexpr float kIntroCbMinW = 1e-3f;", "constexpr float kIntroCbMinW = 1e-2f;")], "a w of 0.0011 reads as zero"),
    M("w3-reads-w4", "C6", [("std::fabs(f[15]) > kIntroCbMinW", "std::fabs(f[19]) > kIntroCbMinW")], "the cb2[3].w test reads cb2[4].w"),
    M("w4-reads-w3", "C6", [("std::fabs(f[19]) > kIntroCbMinW", "std::fabs(f[15]) > kIntroCbMinW")], "the cb2[4].w test reads cb2[3].w"),
    # ---- C7: cb2[0].x and cb2[0].y ---------------------------------------------------------------------------------------------------
    M("half-min-lowered", "C7", [("constexpr float kIntroCbMinHalf = 0.5f;", "constexpr float kIntroCbMinHalf = 0.4f;")], "the smallest half-size is 0.4"),
    M("half-min-raised", "C7", [("constexpr float kIntroCbMinHalf = 0.5f;", "constexpr float kIntroCbMinHalf = 0.6f;")], "the smallest half-size is 0.6"),
    M("half-max-lowered", "C7", [("constexpr float kIntroCbMaxHalf = 50.0f;", "constexpr float kIntroCbMaxHalf = 40.0f;")], "the largest half-size is 40"),
    M("half-max-raised", "C7", [("constexpr float kIntroCbMaxHalf = 50.0f;", "constexpr float kIntroCbMaxHalf = 60.0f;")], "the largest half-size is 60"),
    M("half-x-min-edge-exclusive", "C7", [("f[0] >= kIntroCbMinHalf", "f[0] > kIntroCbMinHalf")], "a half-width of exactly 0.5 is refused"),
    M("half-x-max-edge-exclusive", "C7", [("f[0] <= kIntroCbMaxHalf", "f[0] < kIntroCbMaxHalf")], "a half-width of exactly 50 is refused"),
    M("half-y-min-edge-exclusive", "C7", [("f[1] >= kIntroCbMinHalf", "f[1] > kIntroCbMinHalf")], "a half-height of exactly 0.5 is refused"),
    M("half-y-max-edge-exclusive", "C7", [("f[1] <= kIntroCbMaxHalf", "f[1] < kIntroCbMaxHalf")], "a half-height of exactly 50 is refused"),
    M("half-x-range-or", "C7", [("f[0] >= kIntroCbMinHalf && f[0] <= kIntroCbMaxHalf", "f[0] >= kIntroCbMinHalf || f[0] <= kIntroCbMaxHalf")], "every half-width passes"),
    M("half-y-range-or", "C7", [("f[1] >= kIntroCbMinHalf && f[1] <= kIntroCbMaxHalf", "f[1] >= kIntroCbMinHalf || f[1] <= kIntroCbMaxHalf")], "every half-height passes"),
    M("half-x-check-dropped", "C7", [(X_TEST, "if (false) {")], "the half-width is never checked"),
    M("half-y-check-dropped", "C7", [(Y_TEST, "if (false) {")], "the half-height is never checked"),
    M("half-width-from-y", "C7", [("    r.halfWidth = f[0];\n", "    r.halfWidth = f[1];\n")], "the half-width is the half-height"),
    M("half-x-test-reads-y", "C7", [(X_TEST, "if (!(f[1] >= kIntroCbMinHalf && f[1] <= kIntroCbMaxHalf)) {")], "the half-width test reads cb2[0].y"),
    # ---- C8: the depth direction ---------------------------------------------------------------------------------------------------------
    M("toward-inverted", "C8", [("? 1 : -1;", "? -1 : 1;")], "the direction is the wrong way round"),
    M("toward-always-away", "C8", [("? 1 : -1;", "? -1 : -1;")], "the direction is always away"),
    M("toward-always-toward", "C8", [("? 1 : -1;", "? 1 : 1;")], "the direction is always toward"),
    M("toward-ignores-w4-sign", "C8", [("(f[19] > 0.0f) ? 1 : -1;", "true ? 1 : -1;")], "the direction ignores the sign of cb2[4].w"),
    M("toward-ignores-w3-sign", "C8", [("(f[15] > 0.0f) != (f[19] > 0.0f)", "false != (f[19] > 0.0f)")], "the direction ignores the sign of cb2[3].w"),
    M("toward-reads-wrong-w4", "C8", [("(f[19] > 0.0f) ? 1 : -1;", "(f[18] > 0.0f) ? 1 : -1;")], "the direction reads cb2[4].z for the w"),
    M("toward-reads-wrong-w3", "C8", [("(f[15] > 0.0f) != (f[19] > 0.0f)", "(f[14] > 0.0f) != (f[19] > 0.0f)")], "the direction reads cb2[3].z for the w"),
    # ---- C9: the first failed condition is the one named ----------------------------------------------------------------------------------
    M("order-finite-after-screen", "C9", swap(BLOCK_FINITE, BLOCK_SCREEN), "the screen-space reason is asked before the finite one"),
    M("order-screen-after-length", "C9", swap(BLOCK_SCREEN, BLOCK_LENGTH), "the length reason is asked before the screen-space one"),
    M("order-length-after-w3", "C9", swap(BLOCK_LENGTH, BLOCK_W3), "the cb2[3].w reason is asked before the length one"),
    M("order-w3-after-w4", "C9", swap(BLOCK_W3, BLOCK_W4), "the cb2[4].w reason is asked before the cb2[3].w one"),
    M("order-w4-after-x", "C9", swap(BLOCK_W4, BLOCK_X), "the half-width reason is asked before the cb2[4].w one"),
    M("order-x-after-y", "C9", swap(BLOCK_X, BLOCK_Y), "the half-height reason is asked before the half-width one"),
    # ---- C10: today's screen-space rule, weakened or tightened ----------------------------------------------------------------------------
    M("zero-threshold-1e-6", "C10", [(ZERO, "    auto zero = [](float v) { return v > -1e-6f && v < 1e-6f; };\n")], "the zero test is the interval (-1e-6, 1e-6)"),
    M("zero-lower-bound-1e-6", "C10", [("v > -1e-9f", "v > -1e-6f")], "the zero test's lower bound is -1e-6"),
    M("zero-upper-bound-1e-6", "C10", [("v < 1e-9f", "v < 1e-6f")], "the zero test's upper bound is 1e-6"),
    M("zero-threshold-1e-12", "C10", [(ZERO, "    auto zero = [](float v) { return v > -1e-12f && v < 1e-12f; };\n")], "the zero test is the interval (-1e-12, 1e-12)"),
    M("zero-interval-closed", "C10", [("v > -1e-9f && v < 1e-9f", "v >= -1e-9f && v <= 1e-9f")], "the zero test's interval is closed"),
    M("zero-lower-edge-closed", "C10", [("v > -1e-9f", "v >= -1e-9f")], "exactly -1e-9 reads as zero"),
    M("zero-upper-edge-closed", "C10", [("v < 1e-9f", "v <= 1e-9f")], "exactly 1e-9 reads as zero"),
    M("zero-interval-or", "C10", [("v > -1e-9f && v < 1e-9f", "v > -1e-9f || v < 1e-9f")], "everything reads as zero"),
    M("cb21-w-unchecked", "C10", [(CB2_W, "    if (!zero(f[11])) return false;")], "cb2[1].w is not tested"),
    M("cb22-w-unchecked", "C10", [(CB2_W, "    if (!zero(f[7])) return false;")], "cb2[2].w is not tested"),
    M("cb21-cb22-and", "C10", [("!zero(f[7]) || !zero(f[11])", "!zero(f[7]) && !zero(f[11])")], "cb2[1].w and cb2[2].w must both be non-zero to refuse"),
    M("cb21-w-wrong-slot", "C10", [("!zero(f[7])", "!zero(f[6])")], "cb2[1].z is tested for cb2[1].w"),
    M("cb22-w-wrong-slot", "C10", [("!zero(f[11])", "!zero(f[10])")], "cb2[2].z is tested for cb2[2].w"),
    M("cb23-x-unchecked", "C10", [(CB3_LOOP, "    for (uint32_t i = 13; i < 16; ++i) {")], "cb2[3].x is not tested"),
    M("cb23-w-unchecked", "C10", [(CB3_LOOP, "    for (uint32_t i = 12; i < 15; ++i) {")], "cb2[3].w is not tested"),
    M("cb23-reaches-cb24", "C10", [(CB3_LOOP, "    for (uint32_t i = 12; i < 17; ++i) {")], "cb2[4].x is tested as part of cb2[3]"),
    M("w-lower-edge-exclusive", "C10", [("f[19] < 0.999f", "f[19] <= 0.999f")], "a w of exactly 0.999 is refused"),
    M("w-upper-edge-exclusive", "C10", [("f[19] > 1.001f", "f[19] >= 1.001f")], "a w of exactly 1.001 is refused"),
    M("w-lower-loosened", "C10", [("f[19] < 0.999f", "f[19] < 0.99f")], "the w's lower bound is 0.99"),
    M("w-upper-loosened", "C10", [("f[19] > 1.001f", "f[19] > 1.01f")], "the w's upper bound is 1.01"),
    M("w-lower-tightened", "C10", [("f[19] < 0.999f", "f[19] < 0.9995f")], "the w's lower bound is 0.9995"),
    M("w-upper-tightened", "C10", [("f[19] > 1.001f", "f[19] > 1.0005f")], "the w's upper bound is 1.0005"),
    M("w-range-and", "C10", [("f[19] < 0.999f || f[19] > 1.001f", "f[19] < 0.999f && f[19] > 1.001f")], "nothing is refused for its w"),
    M("w-lower-unchecked", "C10", [(W_TEST, "    if (f[19] > 1.001f) return false;")], "a w below 0.999 is accepted"),
    M("w-upper-unchecked", "C10", [(W_TEST, "    if (f[19] < 0.999f) return false;")], "a w above 1.001 is accepted"),
    M("w-wrong-slot", "C10", [(W_TEST, "    if (f[18] < 0.999f || f[18] > 1.001f) return false;")], "cb2[4].z is tested for cb2[4].w"),
    M("half-x-min-8", "C10", [("f[0] < 16.0f", "f[0] < 8.0f")], "the half-width must reach 8, not 16"),
    M("half-y-min-8", "C10", [("f[1] < 16.0f", "f[1] < 8.0f")], "the half-height must reach 8, not 16"),
    M("half-x-min-32", "C10", [("f[0] < 16.0f", "f[0] < 32.0f")], "the half-width must reach 32, not 16"),
    M("half-y-min-32", "C10", [("f[1] < 16.0f", "f[1] < 32.0f")], "the half-height must reach 32, not 16"),
    M("half-x-edge-exclusive", "C10", [("f[0] < 16.0f", "f[0] <= 16.0f")], "a half-width of exactly 16 is refused"),
    M("half-y-edge-exclusive", "C10", [("f[1] < 16.0f", "f[1] <= 16.0f")], "a half-height of exactly 16 is refused"),
    M("half-x-unchecked", "C10", [(HALF_TEST, "    if (f[1] < 16.0f) return false;")], "the half-width is not tested"),
    M("half-y-unchecked", "C10", [(HALF_TEST, "    if (f[0] < 16.0f) return false;")], "the half-height is not tested"),
    M("half-and", "C10", [("f[0] < 16.0f || f[1] < 16.0f", "f[0] < 16.0f && f[1] < 16.0f")], "both half-sizes must be small to refuse"),
    M("half-upper-bound-added", "C10", [(HALF_TEST, "    if (f[0] < 16.0f || f[0] > 4096.0f || f[1] < 16.0f) return false;")], "a half-width over 4096 is refused"),
    M("never-screen-space", "C10", [("    return true;\n}\n", "    return false;\n}\n")], "nothing reads as screen space"),
    # ---- C11: one float either side of each threshold (what only the sweep sees) -------------------------------------------------------------
    M("sweep-zero-upper-down", "C11", [("v < 1e-9f", "v < " + lit(next_down(1e-9)))], "the zero test's upper bound is one float lower"),
    M("sweep-zero-lower-toward-zero", "C11", [("v > -1e-9f", "v > -" + lit(next_down(1e-9)))], "the zero test's lower bound is one float closer to zero"),
    M("sweep-w-lower-down", "C11", [("f[19] < 0.999f", "f[19] < " + lit(next_down(0.999)))], "the w's lower bound is one float lower"),
    M("sweep-w-upper-up", "C11", [("f[19] > 1.001f", "f[19] > " + lit(next_up(1.001)))], "the w's upper bound is one float higher"),
    M("sweep-half-x-down", "C11", [("f[0] < 16.0f", "f[0] < " + lit(next_down(16.0)))], "the half-width's bound is one float lower than 16"),
    M("sweep-half-y-down", "C11", [("f[1] < 16.0f", "f[1] < " + lit(next_down(16.0)))], "the half-height's bound is one float lower than 16"),
    # ---- C12: which way +x runs, on the real readings ---------------------------------------------------------------------------------
    M("xdir-sign-flipped", "C12", [(XRETURN, "    return minor < 0.0 ? IntroXDir::kRight : IntroXDir::kLeft;\n")], "+x is said to run right where it runs left"),
    M("xdir-always-left", "C12", [(XRETURN, "    return IntroXDir::kLeft;\n")], "+x is always said to run left"),
    M("xdir-always-right", "C12", [(XRETURN, "    return IntroXDir::kRight;\n")], "+x is always said to run right"),
    M("xdir-margin-0.9", "C12", [(XMARGIN, "constexpr double kIntroXDirMargin = 0.9;")], "capture 1's reading (a ratio of 0.87) is unknown"),
    M("xdir-not-carried", "C12", [(XCARRY, "")], "an ok reading does not say which way +x runs"),
    M("xdir-carried-inverted", "C12", [(XCARRY, "    r.xDir = dir == IntroXDir::kLeft ? IntroXDir::kRight : IntroXDir::kLeft;\n")], "an ok reading says the opposite of what was read"),
    # ---- C13: the margin, float rounding, the degenerate inputs ------------------------------------------------------------------------
    M("xdir-margin-0", "C13", [(XMARGIN, "constexpr double kIntroXDirMargin = 0.0;")], "no cancellation is too much"),
    M("xdir-margin-half", "C13", [(XMARGIN, "constexpr double kIntroXDirMargin = 0.5;")], "anything under half is unknown"),
    M("xdir-margin-edge-exclusive", "C13", [(XTEST, XTEST.replace(">=", ">"))], "a ratio exactly on the margin is unknown"),
    M("xdir-margin-absolute", "C13", [(XTEST, XTEST.replace("kIntroXDirMargin * scale", "kIntroXDirMargin"))], "the margin is on the size of the minor, not on its share of the terms"),
    M("xdir-scale-max", "C13", [(XSCALE, "    const double scale = std::fmax(std::fabs(a), std::fabs(b));\n")], "the margin is held against the larger term, not the two together"),
    M("xdir-minor-sum", "C13", [(XMINOR, "    const double minor = a + b;\n")], "the terms are added, not subtracted"),
    M("xdir-minor-a-only", "C13", [(XMINOR, "    const double minor = a;\n")], "the second term is left out of the minor"),
    M("xdir-sign-from-a-alone", "C13", [(XRETURN, "    return a < 0.0 ? IntroXDir::kLeft : IntroXDir::kRight;\n")], "the sign comes from the first term alone"),
    M("xdir-zero-scale-known", "C13", [(XTEST, XTEST.replace("!(scale > 0.0) || ", ""))], "no term at all reads as known"),
    M("xdir-finite-guard-dropped", "C13", [(XFINITE, "    if (false) return IntroXDir::kUnknown;")], "an infinity outruns the margin and reads as known"),
    M("xdir-a-reads-f5", "C13", [("static_cast<double>(f[4])", "static_cast<double>(f[5])")], "cb2[1].y is read for cb2[1].x"),
    M("xdir-b-reads-f17", "C13", [("static_cast<double>(f[16])", "static_cast<double>(f[17])")], "cb2[4].y is read for cb2[4].x"),
    M("xdir-float-arithmetic", "C13", [(XA, "    const float a = f[4] * f[19];\n"), (XB, "    const float b = f[16] * f[7];\n"),
                                       (XMINOR, "    const float minor = a - b;\n"), (XSCALE, "    const float scale = std::fabs(a) + std::fabs(b);\n")],
      "the arithmetic is done in float"),
    # ---- C14: a direction that cannot be told -----------------------------------------------------------------------------------------
    M("gate-dropped", "C14", [(XGATE, "    if (false) {")], "a placement whose +x cannot be told is read ok"),
    # ---- C15: the answer against independent geometry ---------------------------------------------------------------------------------
    M("xdir-a-reads-f18", "C15", [("static_cast<double>(f[19])", "static_cast<double>(f[18])")], "cb2[4].z is read for cb2[4].w"),
    M("xdir-b-reads-f6", "C15", [("static_cast<double>(f[7])", "static_cast<double>(f[6])")], "cb2[1].z is read for cb2[1].w"),
    M("xdir-margin-0.06", "C15", [(XMARGIN, "constexpr double kIntroXDirMargin = 0.06;")], "a ratio under 0.06 is unknown"),
    M("xdir-margin-0.04", "C15", [(XMARGIN, "constexpr double kIntroXDirMargin = 0.04;")], "a ratio under 0.04 is unknown"),
]
# Mutations of the reader's gate, in the groups above: their rules are C1, C3 and C9.
MUTANTS += [
    M("gate-inverted", "C1", [(XGATE, "    if (dir != IntroXDir::kUnknown) {")], "every placement whose +x can be told is refused"),
    M("gate-reason-reworded", "C3", [(XWHY, '"the placement\'s x is unclear"')], "the direction reason is reworded"),
    M("gate-reason-is-length", "C3", [(XWHY, '"cb2[3] is not a unit-scale column (its length is outside 0.5 to 2)"')], "the direction reason is the length one"),
    M("refusal-default-xdir-left", "C3", [(RESULT_INIT, '    IntroWorldCb r = {false, 0.0f, 0, "", IntroXDir::kLeft};\n')], "a refusal reports a direction"),
    M("refusal-sets-xdir-early", "C3", [(XSCREEN, "    r.xDir = introPlacementXDir(f);\n" + XSCREEN)], "a refusal after the finite check carries the direction"),
    M("order-y-after-direction", "C9", swap(BLOCK_Y, BLOCK_DIR), "the direction reason is asked before the half-height one"),
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
    """Compile the rig against the header in header_dir (and only there); (exit code, output, exe)."""
    exe = outdir / "rig.exe"
    cmd = [tc.cl] + CL_FLAGS + ["/I" + str(header_dir), "/Fo" + str(outdir) + os.sep, "/Fe" + str(exe), str(RIG), "/link", "/INCREMENTAL:NO", "kernel32.lib"]
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


# ---- the run -------------------------------------------------------------------------------------------------------------------
def select(only):
    if not only:
        return MUTANTS
    wanted = [n for n in only.split(",") if n]
    unknown = [n for n in wanted if n not in {m.name for m in MUTANTS}]
    if unknown:
        raise ValueError("no such mutation: " + ", ".join(unknown))
    return [m for m in MUTANTS if m.name in wanted]


def plan_text(mutants):
    lines = ["%d mutation(s); each header copy is compiled with the rig in a temp directory outside the repo and run on its rule's case:" % len(mutants)]
    for m in mutants:
        lines.append("  %-34s rule %-4s caught by %-6s %s" % (m.name, m.rule, "/".join(m.caught), m.why))
    lines.append("cl.exe flags: " + " ".join(CL_FLAGS))
    return "\n".join(lines)


def run_all(only=None, jobs=None, keep=False, dry_run=False, out=sys.stdout):
    mutants = select(only)
    if dry_run:
        print(plan_text(mutants), file=out)
        print("dry-run: nothing compiled, started or written", file=out)
        return 0
    prod = read_source(PROD)
    tc = Toolchain()
    work = Path(tempfile.mkdtemp(prefix="intro_curve_math_mutants_"))
    try:
        def build_and_run(name, header_text, rule):
            d = work / name
            d.mkdir()
            (d / "intro_curve_math.h").write_text(header_text, encoding="utf-8", newline="\n")
            code, text, exe = build_rig(tc, d, d)
            if code != 0:
                errors = [l.strip() for l in text.splitlines() if "error" in l]
                return "nocompile", (errors[0] if errors else "")[:200]
            return run_rig(exe, rule, tc)

        control, detail = build_and_run("control", prod, None)
        print("control (the unmutated header, every case): %s %s" % (control, detail), file=out)
        if control != "pass":
            print("the rig does not pass on the unmutated header when built this way; nothing below means anything", file=out)
            return 1

        def one(m):
            try:
                text = apply_edits(prod, m.edits, m.name)
            except ValueError as error:
                return m, "badedit", str(error)
            outcome, detail = build_and_run(m.name, text, m.rule)
            return m, outcome, detail

        results = []
        with concurrent.futures.ThreadPoolExecutor(max_workers=jobs or min(8, os.cpu_count() or 2)) as pool:
            for m, outcome, detail in pool.map(one, mutants):
                if outcome == "fail" and any(detail.startswith(p + ".") for p in m.caught):
                    verdict = "caught"
                elif outcome == "fail":
                    verdict = "OTHER"
                else:
                    verdict = {"pass": "SURVIVED", "crash": "CRASH", "timeout": "TIMEOUT", "nocompile": "NOCOMPILE", "badedit": "BADEDIT"}[outcome]
                results.append((m, verdict, detail))
                print("%-9s %-34s %-4s %s" % (verdict, m.name, m.rule, detail[:130]), file=out)
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
    check(parse_fail("a\nFAIL: C1.x\nb\n") == "C1.x" and parse_fail("PASS: 3\n") is None, "parse_fail reads the last FAIL: line")
    check(apply_edits("a b c", [("b", "B")]) == "a B c", "apply_edits applies one edit")
    for bad, why in ((("a b b", [("b", "B")]), "a repeated anchor"), (("a b", [("x", "y")]), "a missing anchor"), (("a b", [("b", "b")]), "a no-op edit")):
        try:
            apply_edits(*bad)
            check(False, "apply_edits refuses " + why)
        except ValueError:
            pass
    joined = label_block(":rig_a\nx ^\n y\nexit /b 0\n:rig_b\nz\n", ":rig_a")
    check(joined is not None and [" ".join(l.split()) for l in joined] == ["x y", "exit /b 0"], "label_block joins continuations and stops at the next label")
    check(rig_cases('{"C1", caseC1}, {"C10", caseC10}') == {"C1", "C10"}, "rig_cases reads the case table")
    check(swap("a", "b") == [("a", MARK), ("b", "a"), (MARK, "b")] and apply_edits("x a y b z", swap("a", "b")) == "x b y a z", "swap puts two blocks in the other order")
    check(lit(f32(1e-9)) == "9.99999972e-10f" and next_down(1e-9) < f32(1e-9) < next_up(1e-9), "lit and the float neighbours read as float32s")

    # every mutation against the header as it is now, and against the rig
    prod = read_source(PROD)
    rig = read_source(RIG)
    cases = rig_cases(rig)
    names = [m.name for m in MUTANTS]
    check(len(names) == len(set(names)), "mutation names are unique")
    check(len(MUTANTS) >= 90, "the mutation list did not shrink (%d)" % len(MUTANTS))
    for m in MUTANTS:
        try:
            mutated = apply_edits(prod, m.edits, m.name)
            check(mutated != prod, "%s changes the header" % m.name)
        except ValueError as error:
            failures.append(str(error))
        for p in m.caught:
            check(rig_label_exists(rig, p), "%s: the rig has no check labelled %s." % (m.name, p))
        check(m.rule in cases, "%s: the rig has no case %s" % (m.name, m.rule))
    rules = {m.rule for m in MUTANTS}
    check(rules == cases, "every rule of the rig has a mutation, and only rules of the rig: %s vs %s" % (sorted(rules), sorted(cases)))

    # build.bat compiles the rig the way this tool does, and runs this tool's self-test
    bat = BUILD_BAT.read_bytes().decode("utf-8", errors="replace")
    block = label_block(bat, RIG_LABEL)
    check(block is not None, "build.bat has %s" % RIG_LABEL)
    if block:
        text = "\n".join(block)
        cl = next((l for l in block if l.strip().lower().startswith("cl.exe")), "")
        for flag in CL_FLAGS:
            check(flag in cl, "build.bat's rig compile has %s" % flag)
        check("tools\\intro_curve_math_test\\intro_curve_math_test.cpp" in cl, "build.bat compiles the rig")
        check('/I"src\\d3d11"' in cl, "build.bat's rig compile finds the header through /I src\\d3d11 (the mutated copy is found the same way)")
        check("intro_curve_math_test.exe\" --dry-run" in text and "intro_curve_math_test.exe\" --self-test" in text, "build.bat runs the rig's --dry-run and --self-test")
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
        for f in failures:
            print("FAIL: " + f, file=sys.stderr)
        return 1
    print("PASS: intro_curve_math_test mutants.py self-test (%d mutations over %d rules, every anchor found once, build.bat wired)" % (len(MUTANTS), len(rules)))
    return 0


def main(argv=None):
    global BUILD_BAT
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--self-test", action="store_true")
    parser.add_argument("--run", action="store_true")
    parser.add_argument("--list", action="store_true")
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--only", default="")
    parser.add_argument("--jobs", type=int, default=0)
    parser.add_argument("--keep", action="store_true", help="leave the temp directory (printed) for a look at a mutant")
    parser.add_argument("--build-bat", default="", help="the build.bat the self-test reads (default: the repo's)")
    args = parser.parse_args(argv)
    if args.build_bat:
        BUILD_BAT = Path(args.build_bat)
    if args.self_test:
        return self_test()
    if args.list:
        print(plan_text(MUTANTS))
        return 0
    if args.run:
        return run_all(only=args.only, jobs=args.jobs or None, keep=args.keep, dry_run=args.dry_run)
    parser.print_help()
    return 2


if __name__ == "__main__":
    sys.exit(main())
