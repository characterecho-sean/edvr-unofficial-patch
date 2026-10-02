#!/usr/bin/env python3
"""The mutation proof for tools\\vscreen_fit_test: the rig fails when a rule of the fit, or a wiring pin, is flipped.

The rig (vscreen_fit_test.cpp) pins the rules of src\\common\\vscreen_fit.h (R1..R10), the state files on disk (R11) and the wiring of
the resolver, the footprint instrument and its hooks (R12). A rig that passes proves little until it is seen to FAIL on a source
that breaks the rule it pins. This tool does that, for three kinds of mutation:

  code   one textual edit of the header, compiled into the rig in a temp directory OUTSIDE the repo (the rig takes the header
         through -DVSCREEN_FIT_HEADER and is built with -DVSCREEN_FIT_MUTANT, which leaves out the one case that needs another
         source linked), and the rig run on the rule's case;
  state  one textual edit of a COPY of src\\common\\vscreen_auto_state.cpp (the state-file writer and reader), the rig built WITH its
         state-file case against the copy (the real headers are found through /I), and the rig run on the rule's case: R11 runs
         the writer against a destination held open, a reader's handle, a directory in the way, and so on, on disk;
  pin    one textual edit of a COPY of a source file the rig's source pins read, in a temp root, and the rig (built once, from
         the real header) run with --root on the rule's case.

Each must make the rig fail on a check whose label starts with the id the mutation names. Nothing is written inside the repo; the
temp directory is removed at the end.

  python tools\\vscreen_fit_test\\mutants.py --self-test          text only: every anchor is found exactly once in its source as it
                                                                is now, every label named is in the rig, and build.bat compiles
                                                                the rig the way this tool does
  python tools\\vscreen_fit_test\\mutants.py --run [--only a,b] [--jobs N] [--keep]
  python tools\\vscreen_fit_test\\mutants.py --list
  python tools\\vscreen_fit_test\\mutants.py --run --dry-run      the plan; writes nothing, starts nothing

--run needs the MSVC toolchain (cl.exe on PATH, or Visual Studio found with vswhere as build.bat does) and takes about a minute.
--self-test runs in build.bat's rig and is what keeps an edit of a source from silently orphaning a mutation: if an anchor stops
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
HEADER = "src/common/vscreen_fit.h"
HEADER2 = "src/common/vr_supersample_notice.h"   # R13: Elite's Supersampling below 1 (the rig takes it through -DVR_SUPERSAMPLE_HEADER)
HEADERS = {HEADER: "VSCREEN_FIT_HEADER", HEADER2: "VR_SUPERSAMPLE_HEADER"}
STATE_SRC = "src/common/vscreen_auto_state.cpp"   # R11: the state files on disk (the rig links the real one; a state mutation links an edited copy)
RIG = HERE / "vscreen_fit_test.cpp"
BUILD_BAT = ROOT / "build.bat"
RIG_LABEL = ":rig_vscreen_fit_test"

# How build.bat compiles the rig; --self-test checks that the label still says the same.
CL_FLAGS = ["/nologo", "/O2", "/MT", "/std:c++17", "/EHsc", "/W4", "/DWIN32_LEAN_AND_MEAN", "/DNOMINMAX", "/D_CRT_SECURE_NO_WARNINGS"]
LINK_LIBS = ["kernel32.lib"]
RUN_TIMEOUT = 60.0

# Every file the rig's source pins read, relative to the repo root (a pin mutation edits a copy of one of these in a temp root).
PIN_FILES = [
    "src/common/vscreen_fit.h",
    "src/common/vr_supersample_notice.h",
    "src/d3d11/vscreen.h",
    "src/d3d11/menu.cpp",
    "src/d3d11/flat_elite_settings.h",
    "src/d3d11/vscreen_res.cpp",
    "src/d3d11/vr_world_route.cpp",
    "src/d3d11/vr_world_route_math.h",
    "src/d3d11/ui_layer.cpp",
    "src/d3d11/vscreen.cpp",
    "src/d3d11/vscreen_footprint.cpp",
    "src/d3d11/flat_runtime.cpp",
    "src/common/vscreen_auto_state.cpp",
    "edvr.ini",
]


class Mutant:
    def __init__(self, name, caught, edits, why, target=HEADER):
        self.name = name
        self.caught = tuple([caught] if isinstance(caught, str) else caught)   # label prefixes that count as caught by its rule
        self.edits = list(edits)                                                # (old, new) pairs, applied in order
        self.why = why
        self.target = target                                                    # the file edited; the header means a code mutant
        self.rule = re.match(r"R\d+", self.caught[0]).group(0)                  # the rig's case to run
        if target in HEADERS and not name.startswith("pin-"):
            self.kind = "code"
        elif target == STATE_SRC and name.startswith("state-"):
            self.kind = "state"    # the state-file source, edited and LINKED into the rig (a pin- name edits it as text the pins read instead)
        else:
            self.kind = "pin"


def M(name, caught, edits, why, target=HEADER):
    return Mutant(name, caught, edits, why, target)


def drop(old):
    return [(old, "")]


H = HEADER
MUTANTS = [
    # ---- R1: the constants and today's rule -------------------------------------------------------------------------------
    M("m-is-125", "R1a", [("constexpr double kMultiplier = 0.70;", "constexpr double kMultiplier = 1.25;")], "the screen's texels per eye pixel is 1.25"),
    M("m-back-to-1", ("R1a", "R3a"), [("constexpr double kMultiplier = 0.70;", "constexpr double kMultiplier = 1.0;")], "m is back at 1.0: the fitted width is the whole footprint, 5006 on Sean's rig, not his 3504"),
    M("m-moved-a-little", "R1a", [("constexpr double kMultiplier = 0.70;", "constexpr double kMultiplier = 0.665;")], "m is the plan's 0.665: the three calibration constants no longer agree"),
    M("legacy-is-150", "R1a", [("constexpr double kLegacyMultiplier = 1.25;", "constexpr double kLegacyMultiplier = 1.5;")], "today's rule is 150% of the eye"),
    M("floor-is-1920", "R1a", [("constexpr uint32_t kFloorWidth = 2880;", "constexpr uint32_t kFloorWidth = 1920;")], "the floor is the stock width"),
    M("step-is-8", "R1a", [("constexpr uint32_t kStep = 16;", "constexpr uint32_t kStep = 8;")], "widths are multiples of 8"),
    M("chosen-width-moved", "R1a", [("constexpr double kChosenWidthPx = 3504.0;", "constexpr double kChosenWidthPx = 3520.0;")], "the calibration point is not the width Sean chose"),
    M("seed-footprint-moved", "R1a", [("constexpr double kSeedFootprintPx = 5006.0;", "constexpr double kSeedFootprintPx = 5200.0;")], "the seed footprint is not the one that gives his width at m 0.70"),
    M("seed-footprint-is-the-width", "R3a", [("constexpr double kSeedFootprintPx = 5006.0;", "constexpr double kSeedFootprintPx = 3504.0;")],
      "the seed is left at the chosen WIDTH: the first launch fits 0.70 x 3504 = 2453 and floors to 2880, not 3504"),
    M("seed-distance-moved", "R1a", [("constexpr double kSeedDistance = 0.7;", "constexpr double kSeedDistance = 0.75;")], "the calibration point is at another distance"),
    M("round-down", ("R1b", "R1d"), [("(value / static_cast<double>(kStep)) + 0.5)", "(value / static_cast<double>(kStep)))")], "rounding to 16 goes down instead of to nearest"),
    M("round-accepts-nan", "R1d", [("    if (!(value > 0.0) || value > 1.0e6) return 0;\n", "")], "rounding takes a NaN or a negative"),
    M("height-off", "R1c", [("{ return (w * 9u + 8u) / 16u; }", "{ return (w * 9u + 8u) / 17u; }")], "the height is not 9/16 of the width"),
    M("no-max-width", "R1b", [("(w > kMaxWidth ? kMaxWidth : w)", "w")], "no ceiling on a width"),
    # ---- R2: the route's conditions ---------------------------------------------------------------------------------------
    M("flat-profile-fits", "R2b", [('    if (f.flatProfile) addWhy(v, "this is the flat profile (the world route is a VR route)");\n', "")], "the flat profile is not a reason against the route"),
    M("key-inverted", "R2a", [("    if (!f.keyAuto) addWhy(", "    if (f.keyAuto) addWhy(")], "the route's key is read the wrong way round"),
    M("layer-ignored", "R2b", [("    if (f.layerWhy && f.layerWhy[0]) addWhy(v, f.layerWhy);\n", "")], "a UI layer that is not live is not a reason against the route"),
    M("oculus-ignored", "R2b", [("    if (f.runtime == RuntimeKind::OculusNative)\n", "    if (false)\n")], "Elite's native Oculus back end is not a reason against the route"),
    M("foreign-ignored", "R2b", [("    else if (f.runtime == RuntimeKind::ForeignOpenvr)\n", "    else if (false)\n")], "a foreign openvr_api.dll is not a reason against the route"),
    M("route-always-runs", "R2b", [("    v.runs = v.why[0] == 0;", "    v.runs = true;")], "the verdict says the route runs whatever failed"),
    M("separator-lost", "R2d", [("    if (sep) { v.why[used] = ';'; v.why[used + 1] = ' '; }\n", "")], "the reasons run together without a separator"),
    M("legacy-not-used", "R2e", [("    if (!d.route.runs) {", "    if (false) {")], "a route that will not run still fits"),
    # A curved screen is not a condition (the route re-issues the game's own strip): each edit puts the curve back into the rule's header.
    M("pin-curve-function-back", "R2f", [("struct RouteVerdict {\n    bool runs = false;", "inline bool routeStandsAsideForCurve(const RouteFacts&) { return false; }\n\nstruct RouteVerdict {\n    bool runs = false;")],
      "the header asks a function whether the curve holds the route off", HEADER),
    M("pin-curve-named-place-back", "R2f", [("// A CURVED SCREEN (fix.panel_curvature above 0) IS NOT A CONDITION.", "// THE NAMED PLACE.\n// A CURVED SCREEN (fix.panel_curvature above 0) IS NOT A CONDITION.")],
      "the header names a place the curve is consulted in", HEADER),
    M("pin-curve-fact-back", "R2f", [('    bool keyAuto = false;            // experimental.temporal_aa_on_foot_world reads "auto"\n',
                                      '    bool keyAuto = false;            // experimental.temporal_aa_on_foot_world reads "auto"\n    bool curved = false;\n')],
      "RouteFacts carries a curved fact again", HEADER),
    M("pin-curve-paragraph-lost", "R2f", [("// A CURVED SCREEN (fix.panel_curvature above 0) IS NOT A CONDITION.", "// A CURVED SCREEN (fix.panel_curvature above 0) IS A CONDITION.")],
      "the header no longer says a curved screen is not a condition", HEADER),
    M("why-buffer-small", "R2d", [("    char why[320] = {};", "    char why[160] = {};")], "four failing conditions at once do not fit the reasons' buffer (the last is dropped)"),
    M("legacy-names-curve", "R9d", [('    if (!f.keyAuto) addWhy(v, "experimental.temporal_aa_on_foot_world is not auto");\n',
                                    '    if (!f.keyAuto) addWhy(v, "experimental.temporal_aa_on_foot_world is not auto");\n    if (!f.keyAuto) addWhy(v, "fix.panel_curvature is above 0 (the route stands aside for a curved screen)");\n')],
      "the legacy prose names the curve as a reason the route will not run"),
    # ---- R3: the fit --------------------------------------------------------------------------------------------------------
    M("measurement-ignored", "R3b", [("const double fraction = measured ? in.fractionAtUnit : kSeedFractionAtUnit;", "const double fraction = kSeedFractionAtUnit;")], "the stored measurement never replaces the seed"),
    M("distance-not-divided", ("R3a", "R3b"), [("static_cast<double>(in.eyeWidth) / d.distance;", "static_cast<double>(in.eyeWidth);")], "the footprint does not rescale with the panel distance"),
    M("distance-multiplied", ("R3a", "R3b"), [("static_cast<double>(in.eyeWidth) / d.distance;", "static_cast<double>(in.eyeWidth) * d.distance;")], "the footprint rescales the wrong way with the panel distance"),
    M("m-from-legacy", ("R3a", "R3e"), [("d.targetPx = kMultiplier * d.footprintPx;", "d.targetPx = kLegacyMultiplier * d.footprintPx;")], "the fit takes 125% of the footprint"),
    # (R3a's seed table runs first and has a floored row and a small-eye row, so it is what trips first; R3b / R3d pin the same rules from the measured side.)
    M("floor-ignores-small-eyes", ("R3a", "R3d"), [("const double lo = (std::min)(static_cast<double>(kFloorWidth), hi);", "const double lo = static_cast<double>(kFloorWidth);")], "the floor is above the cap on a small eye"),
    M("no-floor", ("R3a", "R3b"), [("    if (v < lo) { v = lo; d.floored = true; }\n", "")], "no floor"),
    M("no-cap", "R3b", [("    if (v > hi) { v = hi; d.capped = true; }\n", "")], "no cap: a fit may ask for more than the legacy rule did"),
    M("junk-footprint-trusted", "R3f", [("const bool measured = in.haveFootprint && plausibleFraction(in.fractionAtUnit);", "const bool measured = in.haveFootprint;")], "a stored fraction of 0.01 is believed"),
    M("distance-unbounded", "R3g", [("    if (!(d > 0.0)) return 1.0;   // NaN, zero or negative: the shipped distance\n", "")], "a zero panel distance is divided by"),
    M("min-fraction-zero", ("R3f", "R3g"), [("constexpr double kMinFraction = 0.05;", "constexpr double kMinFraction = 0.0;")], "a zero fraction is a footprint"),
    # ---- R4: sizes another target already has -----------------------------------------------------------------------------
    M("3840-allowed", "R4a", [('    {3840, 2160, "the menu backdrop still and a per-eye post-pass target"},\n', "")], "3840x2160 is a width like any other"),
    M("nudge-ignores-cap", ("R4c", "R4d"), [("        if (up <= hi && !collides(up)) return up;", "        if (!collides(up)) return up;")], "the nudge goes above the cap"),
    M("nudge-two-steps", "R4b", [("const uint32_t up = w + k * kStep;", "const uint32_t up = w + k * kStep * 2u;")], "the nudge moves two steps at a time"),
    M("nudge-not-applied", "R4b", [("const uint32_t nudged = nudgeOffCollisions(w, static_cast<uint32_t>(lo), d.legacyWidth);", "const uint32_t nudged = w;")], "the fit never moves off a colliding size"),
    # ---- R5: the footprint geometry ---------------------------------------------------------------------------------------
    M("model-transposed", ("R5b", "R5d"), [("static_cast<double>(model[row * 4 + k]) * r[k];", "static_cast<double>(model[k * 3 + row]) * r[k];")], "the model rows are read as columns"),
    M("no-clip-translation", "R5g", [(" + world[2] * clip[8 + k] + clip[12 + k];", " + world[2] * clip[8 + k];")], "cb1[273] is left out of the clip position"),
    M("no-perspective-divide", "R5b", [("const double nx = c[0] / c[3], ny = c[1] / c[3];", "const double nx = c[0], ny = c[1];")], "no divide by w"),
    M("width-not-halved", "R5b", [("double widthFraction() const { return (x1 - x0) * 0.5; }", "double widthFraction() const { return (x1 - x0); }")], "NDC span is not turned into a fraction of the eye"),
    M("size-ignored", ("R5b", "R5c"), [("static_cast<double>(positions[i][0]) * size[0], static_cast<double>(positions[i][1]) * size[1],", "static_cast<double>(positions[i][0]), static_cast<double>(positions[i][1]),")], "SIZE does not scale the quad"),
    M("behind-eye-measured", "R5e", [('        if (c[3] <= 1.0e-9) { f.why = "behind-eye"; return f; }\n', "")], "a panel behind the eye has a footprint"),
    M("degenerate-measured", "R5e", [('    if (!(x1 - x0 > 1.0e-6) || !(y1 - y0 > 1.0e-6)) { f.why = "degenerate"; return f; }\n', "")], "a zero-width quad has a footprint"),
    M("nan-measured", "R5e", [('        if (!std::isfinite(c[0]) || !std::isfinite(c[1]) || !std::isfinite(c[3])) { f.why = "not-finite"; return f; }\n', "")], "a NaN is measured"),
    # ---- R6: the distance law ---------------------------------------------------------------------------------------------
    M("law-by-d-squared", "R6b", [("static_cast<double>(in.eyeWidth) / d.distance;", "static_cast<double>(in.eyeWidth) / (d.distance * d.distance);")], "the footprint rescales as 1/d squared"),
    # ---- R7: the sample store ---------------------------------------------------------------------------------------------
    M("median-index", "R7a", [("const uint32_t mid = n_ / 2;", "const uint32_t mid = n_ / 3;")], "the median is the third, not the middle"),
    M("even-median-not-averaged", "R7a", [("            m = (m + lower) * 0.5;\n", "")], "an even count's median is not averaged"),
    M("thinning-stride-stuck", ("R7c", "R7g"), [("            stride_ *= 2;\n", "")], "after thinning the store keeps every sample (a recent-biased median and p10)"),
    M("thinning-keeps-first-half", ("R7c", "R7g"), [("v_[i] = v_[i * 2];", "v_[i] = v_[i];")], "thinning keeps the first half of the session (an early-biased median and p10)"),
    # The percentile a session stores (the head-on floor, p10).
    M("quantile-is-median", "R7e", [("constexpr double kFootprintQuantile = 0.10;", "constexpr double kFootprintQuantile = 0.50;")], "the session stores the median again, not the 10th percentile"),
    M("quantile-is-minimum", "R7e", [("constexpr double kFootprintQuantile = 0.10;", "constexpr double kFootprintQuantile = 0.0;")], "the session stores its single lowest reading"),
    M("percentile-no-interpolation", "R7f", [("        if (out) *out = a + (b - a) * (pos - static_cast<double>(lo));\n", "        if (out) *out = a;\n")],
      "a percentile between two samples is the lower one, not the interpolation"),
    M("percentile-position-halved", "R7f", [("        const double pos = q * static_cast<double>(n_ - 1);\n", "        const double pos = q * static_cast<double>(n_ - 1) * 0.5;\n")],
      "the percentile is read at half the position it should be"),
    M("percentile-reads-the-wrong-neighbour", "R7f", [("const uint32_t hi = lo + 1 < n_ ? lo + 1 : lo;", "const uint32_t hi = lo;")],
      "a percentile never looks at the next order statistic"),
    # ---- R8: the stored record --------------------------------------------------------------------------------------------
    M("record-precision", "R8a", [('"fraction=%.6f est=p10 eye=%u distance=%.3f samples=%u\\n"', '"fraction=%.1f est=p10 eye=%u distance=%.3f samples=%u\\n"')], "the record keeps one decimal"),
    M("record-tag-not-written", "R8a", [('"fraction=%.6f est=p10 eye=%u distance=%.3f samples=%u\\n"', '"fraction=%.6f eye=%u distance=%.3f samples=%u\\n"')], "the record is written without its est=p10 tag"),
    M("record-implausible-accepted", "R8c", [("    if (!haveFraction || !haveEstimator || !plausibleFraction(r.fractionAtUnit)) return false;", "    if (!haveFraction || !haveEstimator) return false;")], "an implausible stored fraction is accepted"),
    M("record-without-est-accepted", "R8d", [("    if (!haveFraction || !haveEstimator || !plausibleFraction(r.fractionAtUnit)) return false;", "    if (!haveFraction || !plausibleFraction(r.fractionAtUnit)) return false;")],
      "a record with no est tag (an older build's median, Sean's own file) is read as a p10"),
    M("record-any-est-accepted", "R8d", [('        haveEstimator = (end - (eq + 1)) == 3 && std::strncmp(eq + 1, "p10", 3) == 0;', "        haveEstimator = true;")],
      "a record that says est=median (or anything) is read as a p10"),
    M("record-est-prefix-accepted", "R8d", [('(end - (eq + 1)) == 3 && std::strncmp(eq + 1, "p10", 3) == 0;', 'std::strncmp(eq + 1, "p10", 3) == 0;')],
      "est=p10x, or any value that starts with p10, is read as a p10"),
    # ---- R9: the log's text -----------------------------------------------------------------------------------------------
    M("rule-names-swapped", "R9b", [('return r == Rule::Fitted ? "fitted" : "legacy";', 'return r == Rule::Fitted ? "legacy" : "fitted";')], "the line says legacy for a fitted width"),
    M("legacy-reason-lost", "R9d", [('d.route.why[0] ? d.route.why : "no eye width is on record");', '"no eye width is on record");')], "the legacy line does not name the failed condition"),
    M("footprint-token-renamed", "R9b", [('" footprint=%.0f m=%.3f floor=%u cap=%u clamp=%s%s -- FITTED:', '" fp=%.0f m=%.3f floor=%u cap=%u clamp=%s%s -- FITTED:')], "the footprint token the reader parses is renamed"),
    M("m-rounded-in-rule-line", "R9b", [('" footprint=%.0f m=%.3f floor=%u cap=%u clamp=%s%s -- FITTED:', '" footprint=%.0f m=%.2f floor=%u cap=%u clamp=%s%s -- FITTED:')],
      "the rule line prints m at two decimals (a retuned 0.665 would print 0.67 and the reader's recomputed width would not match)"),
    M("m-rounded-in-window-line", "R9f", [('"fit=%u legacy=%u m=%.3f floor=%u"', '"fit=%u legacy=%u m=%.2f floor=%u"')], "the 30 s line prints m at two decimals"),
    M("prose-says-the-width-is-the-footprint", "R9c", [('so the width is %.3f x that (rounded to a multiple of 16', 'so that is the width (%.3f x it, rounded to a multiple of 16')],
      "the rule line says the fitted width IS the footprint (it is 0.70 of it)"),
    M("measured-source-says-median", "R9c", [("The footprint is the head-on floor (p10) of the on-foot widths a previous session measured", "The footprint is the on-foot median a previous session measured")],
      "the rule line says the stored footprint is a median"),
    M("hint-says-the-whole-width", "R9e", [("fitted to about %.0f%% of the on-foot screen's width in your view%s.", "fitted to %.0f%% of the on-foot screen's width in your view%s.")],
      "the menu hint drops its 'about'"),
    M("hint-no-estimate", "R9e", [('" (an estimate until a session on foot has measured it)"', '""')], "the hint does not say a seed is an estimate"),
    M("arming-line-says-median", "R9i", [("the on-foot head-on floor (p10) is stored ", "the on-foot median is stored ")], "the arming line says a median is stored"),
    M("window-token-renamed", "R9f", [('"vscreen footprint 30s: window=%u samples=%u on-foot=%u other=%u', '"vscreen footprint 30s: window=%u samples=%u foot=%u other=%u')], "the on-foot token of the 30 s line is renamed"),
    M("at1-not-normalised", "R9f", [('const double f1 = w.footFrac * d;', 'const double f1 = w.footFrac / d;')], "the footprint at distance 1 is divided, not multiplied"),
    # ---- R10: the route's key parse ----------------------------------------------------------------------------------------
    M("key-case-sensitive", "R10a", [("        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');\n", "")], "AUTO is not auto"),
    M("key-prefix-matches", "R10a", [("    return *text == 0;\n}\n\nenum class RuntimeKind", "    return true;\n}\n\nenum class RuntimeKind")], "autox is auto"),
    # ---- R12: the wiring pins (edits of copies of the sources the pins read) --------------------------------------------------
    M("pin-gate-term-gone", "R12g", [("        witchspaceStarsHidden() || depthProbeWanted() ||\n        vscreenFootprintWanted() ||   // the footprint instrument (vscreen_footprint.h): it reads the 2D screen's composite\n",
                                      "        witchspaceStarsHidden() || depthProbeWanted() ||\n")], "the draw gate does not list the instrument", "src/d3d11/vscreen.cpp"),
    M("pin-applied-distance-lost", "R12f", [("footprintEyeDraw(self, v == DrawVerdict::kPanel ? g_state->distanceScale : 1.0f, baseVertex, startInstance);", "footprintEyeDraw(self, 1.0f, baseVertex, startInstance);")],
      "the instrument is not told the distance its constants carry", "src/d3d11/vscreen.cpp"),
    M("pin-hook-for-every-draw", "R12f", [("            if (eyeGeometry && vscreenFootprintWanted())\n", "            if (vscreenFootprintWanted())\n")], "the hook runs for draws that are not eye draws", "src/d3d11/vscreen.cpp"),
    # The curved screen's swallowed draw (vscreen.cpp curvedScreenSwallowed) is the instrument's second call site.
    M("pin-curved-footprint-for-every-draw", "R12f", [("    if (g_state->rtv0Eye && count && instances && vscreenFootprintWanted())\n        footprintEyeDraw(", "    if (g_state->rtv0Eye && count && instances)\n        footprintEyeDraw(")],
      "the curved screen's swallowed draw calls the instrument whether or not it is armed", "src/d3d11/vscreen.cpp"),
    M("pin-curved-applied-distance-lost", "R12f", [("footprintEyeDraw(self, v == DrawVerdict::kPanel ? g_state->distanceScale : 1.0f, args.base, args.startInstance);", "footprintEyeDraw(self, 1.0f, args.base, args.startInstance);")],
      "the curved path does not tell the instrument the distance its constants carry", "src/d3d11/vscreen.cpp"),
    M("pin-curved-footprint-only-when-routed", "R12f", [("    if (g_state->rtv0Eye && count && instances && vscreenFootprintWanted())\n        footprintEyeDraw(self, v == DrawVerdict::kPanel ? g_state->distanceScale : 1.0f, args.base, args.startInstance);\n    if (!routeOwns) return;\n",
                                                        "    if (!routeOwns) return;\n    if (g_state->rtv0Eye && count && instances && vscreenFootprintWanted())\n        footprintEyeDraw(self, v == DrawVerdict::kPanel ? g_state->distanceScale : 1.0f, args.base, args.startInstance);\n")],
      "the curved path measures the footprint only on a frame the route owns", "src/d3d11/vscreen.cpp"),
    M("pin-curved-footprint-dropped", "R12f", [("    if (g_state->rtv0Eye && count && instances && vscreenFootprintWanted())\n        footprintEyeDraw(self, v == DrawVerdict::kPanel ? g_state->distanceScale : 1.0f, args.base, args.startInstance);\n", "")],
      "the curved path never calls the instrument", "src/d3d11/vscreen.cpp"),
    M("pin-curved-footprint-any-context", "R12f", [("    if (self != g_state->ownerCtx) return;\n    if (g_state->rtv0Eye && count && instances", "    if (g_state->rtv0Eye && count && instances")],
      "the curved path calls the instrument for a context that is not the owner's", "src/d3d11/vscreen.cpp"),
    M("pin-shutdown-lost", "R12g", [("    vscreenFootprintShutdown();\n", "")], "the staging buffer is never released", "src/d3d11/vscreen.cpp"),
    M("pin-tick-renamed", "R12g", [('EDVR_BOUNDARY_TICK(tkVScreenFootprint, "vscreen_footprint");', 'EDVR_BOUNDARY_TICK(tkVScreenFootprintX, "vscreen_footprint");')], "the boundary tick is not the one the pin knows", "src/d3d11/vscreen.cpp"),
    M("pin-map-waits", "R12i", [("D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m", "D3D11_MAP_READ, 0, &m")], "the instrument blocks the render thread on the GPU", "src/d3d11/vscreen_footprint.cpp"),
    M("pin-scope-inside-guard", "R12j", [("    FlatComputeInternalScope internal;\n    guardedBudget(g_budget, [&] { issueCopies(", "    guardedBudget(g_budget, [&] { FlatComputeInternalScope internal; issueCopies(")],
      "the hooks' bypass scope is taken inside the fault guard", "src/d3d11/vscreen_footprint.cpp"),
    M("pin-draw-allocates", "R12k", [("    const uint64_t now = GetTickCount64();\n    if (now - s.lastSampleMs < detail::g_footprintSampleMs) return;", "    std::string scratch = \"x\";\n    const uint64_t now = GetTickCount64();\n    if (now - s.lastSampleMs < detail::g_footprintSampleMs) return;")],
      "the per-draw path allocates", "src/d3d11/vscreen_footprint.cpp"),
    M("pin-arms-in-flat", "R12l", [("    const bool vr = runtimeVrProfile();\n    const std::string width", "    const bool vr = true;\n    const std::string width")], "the instrument arms in the flat profile too", "src/d3d11/vscreen_footprint.cpp"),
    M("pin-skip-uncounted", "R12m", [("{ skip(kSkipVb0); return false; }", "{ return false; }")], "a source that is not what the measurement assumes is skipped without being counted", "src/d3d11/vscreen_footprint.cpp"),
    M("pin-static-com-pointer", "R12p", [("    ID3D11Buffer* staging = nullptr;\n", "    ComPtr<ID3D11Buffer> staging;\n")], "a COM smart pointer in the static state releases at DLL detach", "src/d3d11/vscreen_footprint.cpp"),
    M("pin-flat-reads-config", "R12q", [("    if (!runtimeVrProfile()) {\n        detail::g_footprintWanted = false;\n        return;\n    }\n", "")], "the flat profile reads its arming condition at the boundary", "src/d3d11/vscreen_footprint.cpp"),
    M("pin-footprint-stores-median", "R12n", [("    if (s.session.percentile(vscreenfit::kFootprintQuantile, &frac1)) {", "    if (s.session.median(&frac1)) {")],
      "the instrument stores the session's median again, not its p10", "src/d3d11/vscreen_footprint.cpp"),
    M("pin-footprint-stores-another-quantile", "R12n", [("s.session.percentile(vscreenfit::kFootprintQuantile, &frac1)", "s.session.percentile(0.25, &frac1)")],
      "the instrument stores a quantile of its own instead of the header's", "src/d3d11/vscreen_footprint.cpp"),
    M("pin-resolver-reads-curve", "R12e", [("    f.flatProfile = runtimeFlatProfile();\n", "    f.flatProfile = runtimeFlatProfile();\n    (void)cfg.getFloat(\"fix.panel_curvature\", 0.0f);\n")],
      "the resolver reads the curvature again (a curved screen is not a condition of the route)", "src/d3d11/vscreen_res.cpp"),
    M("pin-resolver-includes-curve", "R12e", [('#include "ui_layer_math.h"\n', '#include "panel_curve.h"\n#include "ui_layer_math.h"\n')],
      "the resolver includes panel_curve.h again", "src/d3d11/vscreen_res.cpp"),
    M("pin-quality-default-drifts", "R12c", [('cfg.getString("fix.ui_quality", "100")', 'cfg.getString("fix.ui_quality", "125")')], "the resolver's fix.ui_quality default is not the layer's", "src/d3d11/vscreen_res.cpp"),
    M("pin-route-default-drifts", "R12b", [('cfg.getString("experimental.temporal_aa_on_foot_world", "auto")', 'cfg.getString("experimental.temporal_aa_on_foot_world", "off")')], "the resolver's route key default is not the route's (auto since 2026-10-01)", "src/d3d11/vscreen_res.cpp"),
    M("pin-layer-reason-dropped", "R12c", [("uiLayerNotLiveReasonFor(target, temporal, jitterAsShipped, /*stoodDown=*/false)", "nullptr")], "the resolver does not ask why the layer is not live", "src/d3d11/vscreen_res.cpp"),
    M("pin-ini-forgets-the-rule", "R12o", [("#   fitted   when the VR on-foot world route will run (it needs", "#   fitted   when the VR on-foot route will run (it needs")], "the ini's text no longer names the world route", "edvr.ini"),
    M("pin-ini-forgets-the-restart", "R12o", [("# while it runs (never any file on disk). Needs a game restart; typing the", "# while it runs (never any file on disk). Takes effect later; typing the")], "the ini's text no longer says a restart is needed", "edvr.ini"),
    M("pin-ini-curve-stops-it", "R12o", [("fix.panel_curvature, does not stop it)", "fix.panel_curvature at 0)")], "the ini's text says a curved screen holds the route off again", "edvr.ini"),
    M("pin-ini-curve-at-zero-added", "R12o", [("#            layer on, and EDVR's own OpenXR runtime; a curved screen,", "#            layer on, fix.panel_curvature at 0, and EDVR's own OpenXR runtime; a curved screen,")],
      "the ini's text lists the old condition beside the new sentence", "edvr.ini"),
    M("pin-ini-says-it-is-the-width", "R12o", [("does not stop it): about 70% of the\n#            width the screen has in your eyes head-on", "does not stop it): the width the screen has in your eyes head-on")],
      "the ini's text says the fitted width is the screen's width, not about 70% of it", "edvr.ini"),
    M("pin-ini-forgets-the-floor", "R12o", [("remembers its head-on floor", "remembers its median")], "the ini's text says a median is remembered, not the head-on floor", "edvr.ini"),
    M("pin-ini-calibration-forgotten", "R12o", [("# starts from a calibration (3504 wide at panel_distance 0.7 on a 4032 wide\n# eye, where", "# starts from a calibration (3200 wide at panel_distance 0.7 on a 4032 wide\n# eye, where")],
      "the ini's text quotes another calibration width", "edvr.ini"),
    # ---- a save that fails (R11 runs the writer on disk against obstacles: a state mutation links an edited copy of vscreen_auto_state.cpp) ----------
    M("state-failure-reported-as-success", "R11g", [("        return refused(error);\n    }\n    return true;\n}", "        refused(error);\n        return true;\n    }\n    return true;\n}")],
      "the writer says true although the record did not reach the file", STATE_SRC),
    M("state-error-not-reported", "R11g", [("        if (win32Error) *win32Error = static_cast<uint32_t>(error);\n", "")],
      "the writer fails but gives no Win32 error for the log", STATE_SRC),
    M("state-write-straight-to-destination", ("R11i", "R11j"), [("CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS", "CreateFileW(dest.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS"),
                                                                ("!MoveFileExW(tmp.c_str(), dest.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))", "false)")],
      "the record is written into the destination itself (as before): a reader holding the file, or a temp name in the way, no longer stops the save", STATE_SRC),
    M("state-no-temp-cleanup", "R11g", [("        DeleteFileW(tmp.c_str());   // nothing of a failed save is left behind\n", "")],
      "a failed save leaves its temp file behind", STATE_SRC),
    M("state-implausible-accepted", "R11l", [("    if (!vscreenfit::plausibleFraction(record.fractionAtUnit)) return refused(ERROR_INVALID_DATA);\n", "")],
      "the writer stores a fraction the record cannot hold", STATE_SRC),
    M("pin-save-bytecount-unchecked", "R12s", [("    else if (written != static_cast<DWORD>(n)) error = ERROR_WRITE_FAULT;   // a short write is a failed save, not a shorter record\n", "")],
      "the writer does not compare the bytes written with the record's length", STATE_SRC),
    M("pin-save-no-flush", "R12s", [("    else if (!FlushFileBuffers(f)) error = lastError();\n", "")], "the writer does not flush the temp file before it replaces the destination", STATE_SRC),
    M("pin-save-no-write-through", "R12s", [("MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))", "MOVEFILE_REPLACE_EXISTING))")],
      "the replace does not wait for the disk", STATE_SRC),
    M("pin-save-result-ignored", "R12r", [("                if (noteMeasuredPanelFootprint(cfg.logDir(), r, &saveError)) {\n                    s.wroteOnce = true;\n                    s.lastWrittenFrac1 = frac1;\n                } else {\n",
                                           "                noteMeasuredPanelFootprint(cfg.logDir(), r, &saveError);\n                s.wroteOnce = true;\n                s.lastWrittenFrac1 = frac1;\n                if (false) {\n")],
      "the instrument does not look at what the save returned: what it has written advances either way (the old behaviour)", "src/d3d11/vscreen_footprint.cpp"),
    M("pin-state-advanced-before-save", "R12r", [("                if (noteMeasuredPanelFootprint(cfg.logDir(), r, &saveError)) {\n                    s.wroteOnce = true;\n                    s.lastWrittenFrac1 = frac1;\n                } else {\n",
                                                  "                s.wroteOnce = true;\n                s.lastWrittenFrac1 = frac1;\n                if (noteMeasuredPanelFootprint(cfg.logDir(), r, &saveError)) {\n                } else {\n")],
      "what has been written advances before the save says whether it happened", "src/d3d11/vscreen_footprint.cpp"),
    M("pin-persisted-always-true", "R12r", [("            w.persisted = s.wroteOnce;\n", "            w.persisted = true;\n")], "the 30 s line says persisted whether or not anything reached the file", "src/d3d11/vscreen_footprint.cpp"),
    M("pin-failed-save-not-retried", "R12r", [("!s.wroteOnce || std::fabs(frac1 - s.lastWrittenFrac1) > kPersistDelta * s.lastWrittenFrac1;", "std::fabs(frac1 - s.lastWrittenFrac1) > kPersistDelta * s.lastWrittenFrac1;")],
      "a first save that failed is not tried again until the p10 moves", "src/d3d11/vscreen_footprint.cpp"),
    M("pin-failed-save-not-counted", "R12r", [("                    ++s.saveFailed;\n", "")], "a failed save is not counted", "src/d3d11/vscreen_footprint.cpp"),
    M("pin-failed-save-line-unbounded", "R12r", [("if (s.saveFailed <= vscreenfit::kSaveFailedLineCap) {", "if (true) {")], "a save that fails every window logs a line every window", "src/d3d11/vscreen_footprint.cpp"),
    M("pin-failed-save-not-on-the-line", "R12r", [("    w.saveFailed = s.saveFailed;\n", "")], "the 30 s line never carries save-failed", "src/d3d11/vscreen_footprint.cpp"),
    M("save-failed-token-always-printed", "R9j", [("    if (w.saveFailed) {   // only after a failure: a line with none is byte for byte what it was", "    if (true) {   // only after a failure: a line with none is byte for byte what it was")],
      "every 30 s line carries save-failed (a line with no failure is no longer what it was)"),
    M("save-failed-token-renamed", "R9j", [('" save-failed=%u"', '" save_failed=%u"')], "the token the reader parses by key is renamed"),
    M("save-failed-line-forgets-the-retry", "R9k", [("and the next 30 s window tries again.", "and that is the end of it.")], "the failure line does not say the next window tries again"),
    M("save-failed-cap-raised", "R9k", [("constexpr uint32_t kSaveFailedLineCap = 3;", "constexpr uint32_t kSaveFailedLineCap = 30;")], "a failing save may log thirty lines a session, not three"),
    M("save-failed-line-reads-as-arming", "R9k", [('"vscreen footprint: SAVE FAILED (Win32 error %u) -- the on-foot footprint did not reach', '"vscreen footprint: armed (Win32 error %u) -- the on-foot footprint did not reach')],
      "the failure line starts with the reader's arming word"),
    # ---- R13: Elite's Supersampling below 1.0 in VR (the second header, and the wiring the pins read) -----------------------------
    M("ss-threshold-90", "R13a", [("constexpr uint32_t kBelowPercent = 98;", "constexpr uint32_t kBelowPercent = 90;")], "the world must render under 90% of the eye, not 98%", HEADER2),
    M("ss-one-axis-is-enough", "R13a", [("kBelowPercent &&\n           static_cast<uint64_t>(renderH)", "kBelowPercent ||\n           static_cast<uint64_t>(renderH)")], "one axis under the eye's is a Supersampling below 1", HEADER2),
    M("ss-percent-truncates", "R13a", [("(static_cast<uint64_t>(renderW) * 100u + eyeW / 2u) / eyeW", "(static_cast<uint64_t>(renderW) * 100u) / eyeW")], "the percentage is cut, not rounded", HEADER2),
    M("ss-pack-ignores-the-judgement", "R13b", [("    if (!below(renderW, renderH, eyeW, eyeH) || renderW > 0xFFFFu", "    if (renderW > 0xFFFFu")], "a size that is not below 1 is published as a notice", HEADER2),
    M("ss-unpack-shift", "R13b", [("*eyeW = static_cast<uint32_t>(packed >> 32 & 0xFFFFu);", "*eyeW = static_cast<uint32_t>(packed >> 16 & 0xFFFFu);")], "the eye's width is read from the render height's bits", HEADER2),
    M("ss-log-forgets-hmd-quality", "R13c", [("\"raise HMD Image Quality instead: EDVR's DLSS upscales from that. Measured from the render sizes, not read from \"", "\"raise the quality elsewhere: EDVR's DLSS upscales from that. Measured from the render sizes, not read from \"")],
      "the log line does not name HMD Image Quality", HEADER2),
    M("ss-toast-reworded", "R13c", [('"Elite Supersampling is below 1: use HMD Image Quality"', '"Elite Supersampling is low"')], "the toast no longer says what to use instead", HEADER2),
    M("ss-hint-reworded", "R13c", [('"Elite Supersampling is below 1: set it to 1 and use HMD Image Quality."', '"Elite Supersampling is low."')], "the Status hint no longer says what to set and use", HEADER2),
    M("ss-hint-too-long", "R13c", [('set it to 1 and use HMD Image Quality."', 'set it to 1 and use HMD Image Quality instead of Elite\'s own Supersampling setting."')],
      "the Status hint outgrows the 78 characters of the hint it replaces", HEADER2),
    M("pin-ss-toast-not-gated", "R13e", [("                if (s.toasts) {\n                    char toast[96];", "                {\n                    char toast[96];")], "the toast ignores menu.toasts", "src/d3d11/menu.cpp"),
    M("pin-ss-hint-dropped", "R13e", [("if (vScreenRenderBelowEye(&rw, &rh, &ew, &eh)) vrss::formatStatusHint(c.hint, sizeof(c.hint));", "(void)rw;")],
      "the Status page's hint no longer says it", "src/d3d11/menu.cpp"),
    M("pin-ss-note-returns", "R13e", [("        if (c.lineCount == 0) {\n            MenuLine& l = c.lines[c.lineCount++];\n            strncpy(l.left, \"Nothing on this page yet.\"",
                                       "        { uint32_t a = 0, b = 0, c2 = 0, d = 0; (void)vScreenRenderBelowEye(&a, &b, &c2, &d); }\n        if (c.lineCount == 0) {\n            MenuLine& l = c.lines[c.lineCount++];\n            strncpy(l.left, \"Nothing on this page yet.\"")],
      "a settings page reads the notice again (a note under its rows takes the bitmap past 2048 px on the Pimax)", "src/d3d11/menu.cpp"),
    M("pin-ss-status-line-returns", "R13e", [("    statusLine(c, \"Eye texture\", buf);\n", "    statusLine(c, \"Eye texture\", buf);\n    statusLine(c, \"Elite supersampling\", \"below 1\");\n")],
      "the Status page gains a line for the notice (sixteen lines at 1.7 caps trip the guard from a 54-px cap)", "src/d3d11/menu.cpp"),
    M("pin-ss-reads-elite-settings", "R13d", [("g_renderBelowEye.store(vrss::pack(s->renderW, s->renderH, s->eyeW, s->eyeH), std::memory_order_release);", "g_renderBelowEye.store(vrss::pack(s->renderW, s->renderH, s->eyeW, s->eyeH), std::memory_order_release); (void)\"SSAAMultiplier\";")],
      "the detection reads Elite's settings file instead of only the sizes", "src/d3d11/vscreen.cpp"),
    M("pin-ss-flat-runtime-touches-it", "R13f", [('#include "flat_copy_structure.h"', '#include "flat_copy_structure.h"\n#include "vscreen.h"   // vScreenRenderBelowEye')], "the flat runtime includes the VR notice's accessor", "src/d3d11/flat_runtime.cpp"),
    M("pin-ss-flat-warning-says-it", "R13f", [("inline constexpr char kFlatTaaAboveWords[] =", 'inline constexpr char kFlatSupersamplingWords[] = "Supersampling is below 1.0";\ninline constexpr char kFlatTaaAboveWords[] =')],
      "the flat warning has a supersampling paragraph again", "src/d3d11/flat_elite_settings.h"),
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


def read_source(rel):
    return (ROOT / rel).read_bytes().decode("utf-8").replace("\r\n", "\n")


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


def build_rig(tc, outdir, extra=(), sources=None):
    """Compile and link the rig into `outdir`; (exit code, output). `sources` defaults to the rig alone."""
    exe = outdir / "rig.exe"
    srcs = sources if sources is not None else [RIG]
    cmd = [tc.cl] + CL_FLAGS + list(extra) + ["/Fo" + str(outdir) + os.sep, "/Fe" + str(exe)] + [str(s) for s in srcs] + \
          ["/link", "/INCREMENTAL:NO"] + LINK_LIBS
    done = subprocess.run(cmd, capture_output=True, text=True, env=tc.env, errors="replace")
    return done.returncode, (done.stdout + done.stderr), exe


def run_rig(exe, rule, tc, root=None):
    """(outcome, detail) of one run of the rig: 'pass', 'fail' (detail = the label), 'crash', 'timeout'."""
    cmd = [str(exe), "--self-test"] + (["--only", rule] if rule else []) + (["--root", str(root)] if root else [])
    try:
        done = subprocess.run(cmd, capture_output=True, text=True, env=tc.env, errors="replace", timeout=RUN_TIMEOUT, cwd=str(ROOT))
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
    lines = ["%d mutation(s); each is compiled or staged in a temp directory outside the repo and run on its rule's case:" % len(mutants)]
    for m in mutants:
        lines.append("  %-30s %-4s rule %-4s caught by %-8s %-30s %s" % (m.name, m.kind, m.rule, "/".join(m.caught), m.target, m.why))
    lines.append("cl.exe flags: " + " ".join(CL_FLAGS) + "; libs: " + " ".join(LINK_LIBS))
    return "\n".join(lines)


def stage_root(work, name, target, text):
    """A temp repo root holding every file the pins read, with `target` replaced by `text`."""
    root = work / ("root_" + name)
    for rel in PIN_FILES:
        dst = root / rel
        dst.parent.mkdir(parents=True, exist_ok=True)
        if rel == target:
            dst.write_text(text, encoding="utf-8", newline="\n")
        else:
            shutil.copyfile(ROOT / rel, dst)
    return root


def run_all(only=None, jobs=None, keep=False, dry_run=False, out=sys.stdout):
    mutants = select(only)
    if dry_run:
        print(plan_text(mutants), file=out)
        print("dry-run: nothing compiled, started or written", file=out)
        return 0
    tc = Toolchain()
    work = Path(tempfile.mkdtemp(prefix="vscreen_fit_mutants_"))
    try:
        # The control: the rig built from the real header, run on every case against the real tree. Pin mutations reuse this exe.
        cdir = work / "control"
        cdir.mkdir()
        code, text, control_exe = build_rig(tc, cdir, sources=[RIG, ROOT / "src" / "common" / "vscreen_auto_state.cpp"])
        if code != 0:
            print(text, file=out)
            print("the rig does not compile", file=out)
            return 1
        outcome, detail = run_rig(control_exe, None, tc)
        print("control (the unmutated sources, every case): %s %s" % (outcome, detail), file=out)
        if outcome != "pass":
            print("the rig does not pass on the unmutated tree; nothing below means anything", file=out)
            return 1

        def code_mutant(m):
            try:
                text = apply_edits(read_source(m.target), m.edits, m.name)
            except ValueError as error:
                return m, "badedit", str(error)
            d = work / ("code_" + m.name)
            d.mkdir()
            mutated = d / Path(m.target).name
            mutated.write_text(text, encoding="utf-8", newline="\n")
            define = '/D%s="%s"' % (HEADERS[m.target], str(mutated).replace("\\", "/"))
            code, output, exe = build_rig(tc, d, extra=["/DVSCREEN_FIT_MUTANT", define])
            if code != 0:
                return m, "nocompile", (output.strip().splitlines()[-1] if output.strip() else "")
            outcome, detail = run_rig(exe, m.rule, tc)
            return m, outcome, detail

        def pin_mutant(m):
            try:
                text = apply_edits(read_source(m.target), m.edits, m.name)
            except ValueError as error:
                return m, "badedit", str(error)
            root = stage_root(work, m.name, m.target, text)
            outcome, detail = run_rig(control_exe, m.rule, tc, root=root)
            return m, outcome, detail

        def state_mutant(m):
            try:
                text = apply_edits(read_source(m.target), m.edits, m.name)
            except ValueError as error:
                return m, "badedit", str(error)
            d = work / ("state_" + m.name)
            d.mkdir()
            mutated = d / Path(m.target).name
            mutated.write_text(text, encoding="utf-8", newline="\n")
            # The rig is built WITH its state-file case (no -DVSCREEN_FIT_MUTANT) and the real header, linked against the edited copy of the
            # state-file source, which finds the real vscreen_auto_state.h (and through it vscreen_fit.h) along /I.
            code, output, exe = build_rig(tc, d, extra=["/I" + str(ROOT / "src" / "common")], sources=[RIG, mutated])
            if code != 0:
                return m, "nocompile", (output.strip().splitlines()[-1] if output.strip() else "")
            outcome, detail = run_rig(exe, m.rule, tc)
            return m, outcome, detail

        def one(m):
            if m.kind == "code":
                return code_mutant(m)
            return state_mutant(m) if m.kind == "state" else pin_mutant(m)

        results = []
        with concurrent.futures.ThreadPoolExecutor(max_workers=jobs or min(8, os.cpu_count() or 2)) as pool:
            for m, outcome, detail in pool.map(one, mutants):
                if outcome == "fail" and any(detail.startswith(p) for p in m.caught):
                    verdict = "caught"
                elif outcome == "fail":
                    verdict = "OTHER"
                else:
                    verdict = {"pass": "SURVIVED", "crash": "CRASH", "timeout": "TIMEOUT", "nocompile": "NOCOMPILE", "badedit": "BADEDIT"}[outcome]
                results.append((m, verdict, detail))
                print("%-9s %-30s %-4s %s" % (verdict, m.name, m.rule, detail[:150]), file=out)
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

    # every mutation against the sources as they are now, and against the rig
    rig = RIG.read_bytes().decode("utf-8").replace("\r\n", "\n")
    cases = rig_cases(rig)
    names = [m.name for m in MUTANTS]
    check(len(names) == len(set(names)), "mutation names are unique")
    check(len(MUTANTS) >= 60, "the mutation list did not shrink (%d)" % len(MUTANTS))
    sources = {}
    for m in MUTANTS:
        if m.target not in sources:
            try:
                sources[m.target] = read_source(m.target)
            except OSError as error:
                failures.append("%s: cannot read %s: %s" % (m.name, m.target, error))
                continue
        try:
            mutated = apply_edits(sources[m.target], m.edits, m.name)
            check(mutated != sources[m.target], "%s changes the source" % m.name)
        except ValueError as error:
            failures.append(str(error))
        for p in m.caught:
            check(rig_label_exists(rig, p), "%s: the rig has no check labelled %s" % (m.name, p))
        check(m.rule in cases, "%s: the rig has no case %s" % (m.name, m.rule))
        if m.kind == "pin":
            check(m.target in PIN_FILES, "%s: %s is not a file the pins' temp root copies" % (m.name, m.target))
        if m.kind == "state":
            check(m.target == STATE_SRC and m.rule == "R11", "%s: a state mutation edits %s and is caught by the rig's R11" % (m.name, STATE_SRC))
    rules = {m.rule for m in MUTANTS}
    check(rules == cases, "every rule of the rig has a mutation (R11, the state files on disk, through the linked copy of the state-file source), and only "
          "rules of the rig: %s vs %s" % (sorted(rules), sorted(cases)))

    # build.bat compiles the rig the way this tool does, and runs this tool's self-test
    bat = BUILD_BAT.read_bytes().decode("utf-8", errors="replace")
    block = label_block(bat, RIG_LABEL)
    check(block is not None, "build.bat has %s" % RIG_LABEL)
    if block:
        text = "\n".join(block)
        cl = next((l for l in block if l.strip().lower().startswith("cl.exe")), "")
        for flag in CL_FLAGS:
            check(flag in cl, "build.bat's rig compile has %s" % flag)
        check("tools\\vscreen_fit_test\\vscreen_fit_test.cpp" in cl and "src\\common\\vscreen_auto_state.cpp" in cl, "build.bat compiles the rig and the state-file source")
        for lib in LINK_LIBS:
            check(lib in cl, "build.bat's rig link has %s" % lib)
        check("vscreen_fit_test.exe\" --dry-run" in text and "vscreen_fit_test.exe\" --self-test \"%ROOT%\"" in text, "build.bat runs the rig's --dry-run and --self-test with the repo root")
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
    print("PASS: vscreen_fit_test mutants.py self-test (%d mutations over %d rules, every anchor found once, build.bat wired)" % (len(MUTANTS), len(rules)))
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
    args = parser.parse_args(argv)
    if args.self_test:
        return self_test()
    if args.list:
        print(plan_text(select(args.only)))
        return 0
    if args.run:
        return run_all(args.only, args.jobs, args.keep, args.dry_run)
    parser.print_help()
    return 2


if __name__ == "__main__":
    sys.exit(main())
