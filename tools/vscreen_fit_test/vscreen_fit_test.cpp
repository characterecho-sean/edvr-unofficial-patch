// fix.vscreen_res_width = auto, fitted to what each eye shows: the rule, its inputs, its footprint arithmetic, its stored state,
// its log text (src/common/vscreen_fit.h, vscreen_auto_state.cpp, and the wiring in vscreen_res.cpp / vscreen.cpp /
// vscreen_footprint.cpp; docs/design-flat-temporal-aa-2026-09-23.md, section 82, the "vscreen auto-fit" entry).
//
// The pure half is the very code the DLL runs: this file includes src\common\vscreen_fit.h and calls it. Nothing here fakes a
// decision. tools\vscreen_fit_test\mutants.py breaks each rule of the header on purpose (and the wiring pins by editing copies
// of the sources they read) and requires the rule's own checks to fail.
//
// WHAT IS PINNED, by rule (a check's label starts with its rule's id: the mutation tool names the rule each edit must trip):
//   R1  the constants of the rule (m = 0.70, floor, cap multiplier, step, Sean's calibration point: the chosen width 3504, the seed
//       footprint 5006, the eye 4032 at distance 0.7) and today's rule, unchanged: the legacy width and its 16:9 height, rounding half up
//   R2  the route's conditions: each one alone sends the width back to the legacy rule and says why; the rule does not consult the
//       curve (a curved screen is not a condition); a route that will not run gives EXACTLY the old answer for every distance and footprint
//   R3  the fit: the first launch on Sean's rig is the width he chose (3504x1971), a measurement replaces the seed, the footprint
//       rescales as 1/d, the floor and the cap, a small eye, never above the legacy width, always a multiple of 16 and 16:9
//   R4  sizes another target already has: never produced by a fit (nudged up, else down), the legacy rule untouched
//   R5  the footprint geometry: the four corners through the composite vertex shader's own arithmetic against a closed form,
//       and the refusals (behind the eye, not finite, degenerate)
//   R6  the distance law through the geometry: scaling the panel's z translation by d scales the footprint by 1/d
//   R7  the session's sample store: median, range, the percentile the session stores (the head-on floor, p10: known sets,
//       interpolation, an even thinning that keeps the median and the p10 across a long session)
//   R8  the stored record: the codec round trip, the est=p10 tag, and what it refuses (an older build's median record, which has no tag)
//   R9  the log's text: the `vScreen resolution:` line names the rule and why, the menu hint fits its buffer, the 30 s line's
//       tokens (the reader, tools\edvr_log.py --vscreen-fit, parses exactly these), the save-failed token (only after a failed save)
//       and the SAVE FAILED line
//   R10 the setting parse (keyTextIsAuto)
//   R11 the state files on disk (vscreen_auto_state.cpp): the footprint's round trip beside the eye width's, in a temp directory,
//       Sean's own older median file read as no record, and a save that cannot reach the file (held open with no sharing, held by a
//       reader, the temp name or the destination taken by a directory, a record it refuses): false with the Win32 error, the file
//       exactly as it was, no temp left; the same record saved once the obstacle is gone
//   R12 the wiring, as source pins: the resolver's reads match their owners', the instrument's hook sits where the pins say and
//       stays off the flat profile, its D3D calls never wait and always step past the hooks, the resolver reads no curve fact, the
//       instrument claims only what the save reported (and retries a failed one), the writer's temp file / byte count / flush / replace
//   R13 Elite's Supersampling below 1.0 in VR (src/common/vr_supersample_notice.h; design section 83, the VR warning): the pure
//       judgement over the measured render size and the eye's, the published word, the words, and the wiring as source pins
//       (the detection is vscreen's own measurement, never Elite's settings file; the toast and the Status page's hint are the
//       menu's and no page gains a line, because the menu bitmap's 2048-px height guard trips on the Pimax; a flat session never
//       reaches any of it)
//
// Usage: --self-test [<repo root>] [--only R1,R3,...] [--root <dir>] | --dry-run (does nothing) | --write-fixture <path> [--dry-run]
// (regenerates tools\vscreen_fit_fixture.log from the formatters; with --dry-run it says what it would write and writes nothing).
// Run from the repo root (the pins read src\ and edvr.ini). Exit 0 and "PASS" only when every check holds; the first failing label is
// printed as `FAIL: <label>`.
#include <windows.h>

#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#ifndef VSCREEN_FIT_HEADER
#define VSCREEN_FIT_HEADER "../../src/common/vscreen_fit.h"
#endif
#include VSCREEN_FIT_HEADER

#ifndef VSCREEN_FIT_MUTANT
#include "../../src/common/vscreen_auto_state.h"
#endif

// R13: Elite's Supersampling below 1, from the measured render size. The mutation tool builds the rig against an edited copy of this
// header through -DVR_SUPERSAMPLE_HEADER, as it does for the fit's.
#ifndef VR_SUPERSAMPLE_HEADER
#define VR_SUPERSAMPLE_HEADER "../../src/common/vr_supersample_notice.h"
#endif
#include VR_SUPERSAMPLE_HEADER

namespace fit = edvr::vscreenfit;
namespace vrss = edvr::vrss;

namespace {

// ---- the harness ---------------------------------------------------------------------------------------------------------
unsigned g_checks = 0;
std::string g_failure;   // the first failing label of the running case
std::string g_root = ".";

void check(bool ok, const char* label) {
    ++g_checks;
    if (!ok && g_failure.empty()) g_failure = label;
}
void checkf(bool ok, const char* fmt, ...) {
    ++g_checks;
    if (ok || !g_failure.empty()) return;
    char buf[512];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    g_failure = buf;
}
bool closeTo(double a, double b, double tol) { return std::fabs(a - b) <= tol; }
bool has(const std::string& text, const char* needle) { return text.find(needle) != std::string::npos; }

std::string readFile(const std::string& relative) {
    std::string path = g_root;
    if (!path.empty() && path.back() != '\\' && path.back() != '/') path += '\\';
    path += relative;
    std::ifstream in(path, std::ios::binary);
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::string out;
    out.reserve(text.size());
    for (char c : text) if (c != '\r') out += c;
    return out;
}
size_t count(const std::string& s, const std::string& needle) {
    size_t n = 0;
    for (size_t at = s.find(needle); at != std::string::npos; at = s.find(needle, at + needle.size())) ++n;
    return n;
}
// The text of the function whose head is `head` (from the head through its matching brace).
std::string functionBody(const std::string& s, const std::string& head) {
    const size_t at = s.find(head);
    if (at == std::string::npos) return std::string();
    const size_t open = s.find('{', at);
    if (open == std::string::npos) return std::string();
    int depth = 0;
    for (size_t i = open; i < s.size(); ++i) {
        if (s[i] == '{') ++depth;
        else if (s[i] == '}' && --depth == 0) return s.substr(at, i - at + 1);
    }
    return std::string();
}
bool before(const std::string& s, const std::string& a, const std::string& b) {
    const size_t x = s.find(a), y = s.find(b);
    return x != std::string::npos && y != std::string::npos && x < y;
}

// ---- independent references (never the code under test) ------------------------------------------------------------------
uint32_t refRound16(double v) { return static_cast<uint32_t>(std::floor(v / 16.0 + 0.5)) * 16u; }
uint32_t refLegacy(uint32_t eye) {
    uint32_t w = refRound16(eye * 1.25);
    if (w < 640) w = 640;
    if (w > 8192) w = 8192;
    return w;
}
uint32_t refHeight(uint32_t w) { return (w * 9u + 8u) / 16u; }

fit::RouteFacts goodRoute() {
    fit::RouteFacts f;
    f.flatProfile = false;
    f.keyAuto = true;
    f.layerWhy = nullptr;
    f.runtime = fit::RuntimeKind::NotLoadedYet;
    return f;
}
fit::Inputs seedInputs(uint32_t eye, double distance) {
    fit::Inputs in;
    in.eyeWidth = eye;
    in.distance = distance;
    in.route = goodRoute();
    return in;
}
fit::Inputs measuredInputs(uint32_t eye, double distance, double frac1) {
    fit::Inputs in = seedInputs(eye, distance);
    in.haveFootprint = true;
    in.fractionAtUnit = frac1;
    in.footprintSamples = 40;
    return in;
}

// ---- R1: the constants and today's rule ----------------------------------------------------------------------------------
void caseR1() {
    check(fit::kMultiplier == 0.70, "R1a: m is 0.70 (the screen's texels per eye pixel at the middle of the panel: 1.43 eye pixels per texel)");
    check(fit::kFloorWidth == 2880, "R1a: the floor is 2880 (just over twice the stock detail)");
    check(fit::kLegacyMultiplier == 1.25, "R1a: today's rule is 125% of the eye width");
    check(fit::kStep == 16, "R1a: widths are multiples of 16");
    check(fit::kMinWidth == 640 && fit::kMaxWidth == 8192, "R1a: the width bounds are the resolver's own (640..8192)");
    check(fit::kChosenWidthPx == 3504.0 && fit::kSeedFootprintPx == 5006.0 && fit::kSeedDistance == 0.7 && fit::kSeedEyeWidthPx == 4032.0,
          "R1a: the calibration point is Sean's rig: he chose 3504 wide, and the screen spans 5006 px head-on at distance 0.7 on a 4032 px eye");
    check(closeTo(fit::kMultiplier * fit::kSeedFootprintPx, fit::kChosenWidthPx, 1.0),
          "R1a: m x the seed footprint is the chosen width to within a pixel (0.70 x 5006 = 3504.2), so the three constants move together");
    check(closeTo(fit::kSeedFractionAtUnit, 5006.0 * 0.7 / 4032.0, 1e-12) && closeTo(fit::kSeedFractionAtUnit, 0.86910, 5e-6),
          "R1a: the seed fraction at distance 1.0 is the seed FOOTPRINT x 0.7 / 4032 = 0.86910 (not the chosen width's)");
    for (uint32_t eye : {4032u, 3296u, 2016u, 3072u, 3000u, 5000u, 6600u, 1000u, 400u}) {
        const uint32_t want = refLegacy(eye);
        checkf(fit::clampWidth(fit::roundTo16(eye * 1.25)) == want, "R1b: the legacy width of a %u px eye is %u", eye, want);
        fit::Inputs in = seedInputs(eye, 0.7);
        in.route.keyAuto = false;
        const fit::Decision d = fit::decide(in);
        checkf(d.rule == fit::Rule::Legacy && d.width == want && d.legacyWidth == want, "R1b: a route that will not run gives the legacy %u for a %u px eye (got %u)", want, eye, d.width);
        checkf(d.height == refHeight(want), "R1c: the legacy height of %u is %u (got %u)", want, refHeight(want), d.height);
    }
    check(refLegacy(4032) == 5040 && fit::decide([] { auto in = seedInputs(4032, 0.7); in.route.keyAuto = false; return in; }()).width == 5040,
          "R1b: Sean's eye (4032 px) is 5040 wide under the legacy rule, as it always was");
    check(fit::heightFor(5040) == 2835 && fit::heightFor(3504) == 1971 && fit::heightFor(2880) == 1620, "R1c: 16:9 heights (5040x2835, 3504x1971, 2880x1620)");
    for (uint32_t w = 640; w <= 8192; w += 16) {
        if ((w * 9u) % 16u != 0u || fit::heightFor(w) * 16u != w * 9u) { checkf(false, "R1c: a multiple of 16 (%u) is not exactly 16:9", w); break; }
    }
    check(fit::roundTo16(8.0) == 16 && fit::roundTo16(7.99) == 0 && fit::roundTo16(24.0) == 32 && fit::roundTo16(23.99) == 16 && fit::roundTo16(3504.0) == 3504,
          "R1d: rounding to a multiple of 16 is to nearest, half up");
    check(fit::roundTo16(0.0) == 0 && fit::roundTo16(-5.0) == 0 && fit::roundTo16(std::nan("")) == 0, "R1d: a non-positive or NaN value rounds to 0");
    check(fit::roundTo16(1.0e12) == 0 && fit::roundTo16(2.0e6) == 0 && fit::roundTo16(9.0e5) == 900000, "R1d: a value no width could be (past a million) rounds to 0, not to an overflowed number");
    const fit::Decision none = fit::decide(seedInputs(0, 0.7));
    check(none.width == 0 && none.height == 0 && none.rule == fit::Rule::Legacy && none.source == fit::Source::None,
          "R1e: with no eye width on record the decision has no width (the caller leaves the game's own panel alone)");
}

// ---- R2: the route's conditions ------------------------------------------------------------------------------------------
void caseR2() {
    const fit::RouteVerdict good = fit::routeVerdict(goodRoute());
    check(good.runs && good.why[0] == 0, "R2a: every condition holds: the route will run and nothing is named");
    {
        fit::RouteFacts f = goodRoute();
        f.runtime = fit::RuntimeKind::EdvrOpenXr;
        check(fit::routeVerdict(f).runs, "R2c: EDVR's own OpenXR (the module list says so) runs the route");
        f.runtime = fit::RuntimeKind::NotLoadedYet;
        check(fit::routeVerdict(f).runs, "R2c: a runtime not loaded yet at launch is undecided, not a failure (the eye width on record came from EDVR's OpenXR)");
    }
    struct One { const char* label; void (*apply)(fit::RouteFacts&); const char* phrase; };
    const One singles[] = {
        {"R2b: the flat profile never fits", [](fit::RouteFacts& f) { f.flatProfile = true; }, "flat profile"},
        {"R2b: the key not auto: no route", [](fit::RouteFacts& f) { f.keyAuto = false; }, "the on-foot world route is off"},
        {"R2b: the UI layer not live: no route", [](fit::RouteFacts& f) { f.layerWhy = "fix.ui_quality is off"; }, "fix.ui_quality is off"},
        {"R2b: Elite's native Oculus back end: no route", [](fit::RouteFacts& f) { f.runtime = fit::RuntimeKind::OculusNative; }, "native Oculus"},
        {"R2b: a foreign openvr_api.dll: no route", [](fit::RouteFacts& f) { f.runtime = fit::RuntimeKind::ForeignOpenvr; }, "not EDVR's"},
    };
    for (const One& one : singles) {
        fit::RouteFacts f = goodRoute();
        one.apply(f);
        const fit::RouteVerdict v = fit::routeVerdict(f);
        check(!v.runs && has(v.why, one.phrase), one.label);
        fit::Inputs in = seedInputs(4032, 0.7);
        in.route = f;
        const fit::Decision d = fit::decide(in);
        checkf(d.rule == fit::Rule::Legacy && d.width == 5040 && !d.route.runs && has(d.route.why, one.phrase),
               "R2e: with the condition failing (%s) the width is the legacy 5040 and the decision names it", one.phrase);
    }
    {   // every condition at once: the flat profile, the key, the layer and the runtime (four reasons, three separators)
        fit::RouteFacts f = goodRoute();
        f.flatProfile = true;
        f.keyAuto = false;
        f.layerWhy = "fix.ui_quality is off";
        f.runtime = fit::RuntimeKind::OculusNative;
        const fit::RouteVerdict v = fit::routeVerdict(f);
        check(!v.runs && has(v.why, "flat profile") && has(v.why, "route is off") && has(v.why, "ui_quality is off") && has(v.why, "native Oculus") &&
                  count(v.why, "; ") == 3,
              "R2d: every failing condition is named, joined with \"; \"");
    }
    // A route that will not run gives EXACTLY the old answer, for every distance and every stored footprint.
    {
        bool same = true;
        for (uint32_t eye : {2016u, 3296u, 4032u, 5000u})
            for (double d : {0.25, 0.5, 0.7, 1.0, 2.0, 4.0, 9.0})
                for (double frac : {0.0, 0.1, 0.6083, 1.5}) {
                    fit::Inputs in = measuredInputs(eye, d, frac);
                    in.route.keyAuto = false;   // a condition that fails: the route will not run
                    const fit::Decision r = fit::decide(in);
                    same = same && r.rule == fit::Rule::Legacy && r.width == refLegacy(eye) && r.height == refHeight(refLegacy(eye)) && r.footprintPx == 0.0 &&
                           r.source == fit::Source::None;
                }
        check(same, "R2e: a route that will not run is the legacy rule exactly, whatever the distance or the stored footprint");
    }
    // The rule does not consult the curve: the route re-issues a curved screen through the game's own strip, so a curved screen is
    // not a condition. The header has no function that asks, no `curved` fact, and says so in the paragraph that names it.
    const std::string header = readFile("src\\common\\vscreen_fit.h");
    if (!header.empty()) {
        check(!has(header, "routeStandsAsideForCurve") && !has(header, "THE NAMED PLACE"),
              "R2f: the rule does not consult the curve (no routeStandsAsideForCurve, no named place for it)");
        check(!has(header, "bool curved") && !has(header, ".curved") && !has(header, "f.curved"),
              "R2f: and RouteFacts carries no `curved` fact");
        check(count(header, "A CURVED SCREEN (fix.panel_curvature above 0) IS NOT A CONDITION.") == 1,
              "R2f: and the header says in one paragraph that a curved screen is not a condition");
    }
}

// ---- R3: the fit ---------------------------------------------------------------------------------------------------------
void caseR3() {
    {   // Sean's rig, first launch (nothing stored): the width he chose. The numbers below are worked by hand (m 0.70 x A, rounded to 16),
        // never read back from the code under test: A = 0.86910 x 4032 / d on this eye, floor 2880, cap = the legacy 5040.
        const fit::Decision d = fit::decide(seedInputs(4032, 0.7));
        check(d.rule == fit::Rule::Fitted && d.source == fit::Source::Seed, "R3a: the first launch with nothing measured is fitted, from the seed");
        checkf(d.width == 3504 && d.height == 1971, "R3a: the first launch on Sean's rig (Pimax, eye 4032, distance 0.7) is 3504x1971 (got %ux%u)", d.width, d.height);
        check(closeTo(d.footprintPx, 5006.0, 1e-6) && closeTo(d.targetPx, 3504.2, 1e-6) && !d.floored && !d.capped && d.nudgedFrom == 0 && d.legacyWidth == 5040,
              "R3a: its footprint is 5006 px head-on, m x it is 3504.2, unclamped, and the legacy width it replaces is 5040");
        const fit::Decision e = fit::decide(seedInputs(4032, 0.65));
        check(closeTo(e.footprintPx, 5391.08, 0.01) && closeTo(e.targetPx, 3773.8, 0.05) && e.width == 3776,
              "R3a: at distance 0.65 the seed footprint is 5391 px, m x it 3773.8, and the width 3776");
        struct Seed { double d; uint32_t width; bool floored; };
        const Seed seeds[] = {{0.5, 4912, false}, {0.55, 4464, false}, {0.6, 4096, false}, {0.65, 3776, false}, {0.7, 3504, false}, {0.75, 3264, false},
                              {0.8, 3072, false}, {0.85, 2880, false}, {0.9, 2880, true},  {1.0, 2880, true},  {2.0, 2880, true}};
        for (const Seed& s : seeds) {
            const fit::Decision r = fit::decide(seedInputs(4032, s.d));
            checkf(r.source == fit::Source::Seed && r.width == s.width && r.floored == s.floored && !r.capped,
                   "R3a: the seed on a 4032 px eye at distance %.2f gives %u (floored %d), got %u (%d, capped %d)", s.d, s.width, s.floored, r.width, r.floored, r.capped);
        }
        struct Eye { uint32_t eye; double d; uint32_t width; bool floored; };
        const Eye eyes[] = {{2064, 0.7, 2576, true}, {3664, 0.7, 3184, false}, {4072, 0.65, 3808, false}, {3296, 0.7, 2880, true}};
        for (const Eye& s : eyes) {
            const fit::Decision r = fit::decide(seedInputs(s.eye, s.d));
            checkf(r.source == fit::Source::Seed && r.width == s.width && r.floored == s.floored && !r.capped,
                   "R3a: the same arithmetic on a %u px eye at distance %.2f gives %u (floored %d; a 2064 px eye is its legacy 2576), got %u (%d, capped %d)", s.eye, s.d, s.width,
                   s.floored, r.width, r.floored, r.capped);
        }
    }
    {   // A measured launch: the measurement replaces the seed, and 1/d rescales it.
        struct Row { double d, frac1; uint32_t eye; uint32_t width; bool floored, capped; };
        const Row rows[] = {
            {0.7, 5006.0 * 0.7 / 4032.0, 4032, 3504, false, false},   // the seed's own fraction, measured: 5006 px -> 3504.2 -> 3504
            {0.5, 0.8691, 4032, 4912, false, false},      // 7008 px -> 4906 -> 4912
            {1.0, 0.8691, 4032, 2880, true, false},       // 3504 px -> 2453: under the floor
            {2.0, 0.8691, 4032, 2880, true, false},
            {0.4, 0.8691, 4032, 5040, false, true},       // 8761 px -> 6132: over the cap
            {0.7, 0.8691, 3296, 2880, true, false},       // Quest 3: 4092 px -> 2865, under the floor (cap 4128)
            {0.6, 0.7, 4032, 3296, false, false},         // 4704 px exactly -> 3292.8 -> 3296
            {0.7, 0.8, 4032, 3232, false, false},         // 4608 px exactly (0.8 x 4032 / 0.7) -> 3225.6 -> 3232
            {0.7, 0.889105, 4032, 3584, false, false},    // Sean's older MEDIAN file, if it were honoured (R8d and R11 say it is not): 5121 px -> 3585 -> 3584
            {0.65, 0.889105, 4032, 3856, false, false},   // ... and at 0.65: 5515 px -> 3861 -> 3856
        };
        for (const Row& r : rows) {
            const fit::Decision d = fit::decide(measuredInputs(r.eye, r.d, r.frac1));
            checkf(d.source == fit::Source::Measured && d.width == r.width && d.floored == r.floored && d.capped == r.capped,
                   "R3b: measured fraction %.4f at distance %.2f on a %u px eye gives %u (floored %d, capped %d), got %u (%d, %d)", r.frac1, r.d, r.eye, r.width,
                   r.floored, r.capped, d.width, d.floored, d.capped);
        }
    }
    {   // The 1/d rescale, without a new measurement: halving the distance doubles the footprint (inside the band).
        const fit::Decision a = fit::decide(measuredInputs(4032, 0.8, 0.6));
        const fit::Decision b = fit::decide(measuredInputs(4032, 0.4, 0.6));
        check(closeTo(b.footprintPx, a.footprintPx * 2.0, 1e-6), "R3c: the footprint rescales as 1/d (halving panel_distance doubles it)");
        const fit::Decision c = fit::decide(measuredInputs(2016, 0.8, 0.6));
        check(closeTo(a.footprintPx, c.footprintPx * 2.0, 1e-6), "R3c: and it scales with the eye width (the fraction is of the eye, so a doubled eye is a doubled footprint)");
    }
    {   // A small eye: the cap is below the floor, so the answer is the legacy width, never more than it.
        const fit::Decision d = fit::decide(seedInputs(2016, 0.7));
        checkf(d.width == refLegacy(2016) && d.legacyWidth == refLegacy(2016), "R3d: a 2016 px eye (cap %u under the 2880 floor) is fitted at the legacy width, got %u", refLegacy(2016), d.width);
        check(d.floored && !d.capped, "R3d: and it is a floor that was applied, with the floor kept under the cap (min(floor, cap)), not a floor over the cap that the cap then undoes");
    }
    {   // Properties over a grid: never above legacy, never under min(floor, legacy), a multiple of 16, exactly 16:9, within a step of m x A when unclamped.
        bool bounds = true, step = true, shape = true, round = true;
        unsigned unclamped = 0;
        for (uint32_t eye : {1500u, 2016u, 2880u, 3296u, 4032u, 4508u, 5000u, 6400u})
            for (double d : {0.25, 0.5, 0.7, 0.85, 1.0, 1.4, 2.0, 4.0})
                for (double frac : {0.2, 0.45, 0.6083, 0.9, 1.4}) {
                    const fit::Decision r = fit::decide(measuredInputs(eye, d, frac));
                    const uint32_t legacy = refLegacy(eye);
                    const uint32_t lo = std::min<uint32_t>(2880u, legacy);
                    bounds = bounds && r.width <= legacy && r.width >= lo && r.width >= 640 && r.width <= 8192;
                    step = step && r.width % 16u == 0u;
                    shape = shape && r.height == refHeight(r.width) && r.height * 16u == r.width * 9u;
                    const double a = 0.70 * frac * eye / d;   // m x A, with m written out here, not read from the header
                    if (!r.floored && !r.capped && r.nudgedFrom == 0) {
                        ++unclamped;
                        round = round && std::fabs(static_cast<double>(r.width) - a) <= 8.0 + 1e-9;
                    }
                }
        check(bounds, "R3e: the fitted width is never above the legacy width and never below min(floor, legacy)");
        check(step, "R3e: the fitted width is a multiple of 16");
        check(shape, "R3e: the fitted height is the exact 16:9 pair of the width");
        check(round && unclamped >= 20, "R3e: an unclamped fit is m x A to within half a step, m = 0.70 (over a grid with at least 20 unclamped cells, so the check is not vacuous)");
    }
    {   // Measured beats seed; an implausible stored fraction falls back to the seed.
        const fit::Decision measured = fit::decide(measuredInputs(4032, 0.7, 0.7));
        const fit::Decision seed = fit::decide(seedInputs(4032, 0.7));
        check(measured.width != seed.width && measured.source == fit::Source::Measured, "R3f: a stored measurement replaces the seed");
        const fit::Decision junk = fit::decide(measuredInputs(4032, 0.7, 0.01));
        check(junk.source == fit::Source::Seed && junk.width == seed.width, "R3f: an implausible stored fraction (0.01) is the seed, not a width");
        const fit::Decision huge = fit::decide(measuredInputs(4032, 0.7, 9.0));
        check(huge.source == fit::Source::Seed, "R3f: nor is a fraction of several eye widths");
    }
    check(fit::sanitizeDistance(0.0) == 1.0 && fit::sanitizeDistance(-3.0) == 1.0 && fit::sanitizeDistance(std::nan("")) == 1.0 &&
              fit::sanitizeDistance(0.1) == fit::kMinDistance && fit::sanitizeDistance(10.0) == fit::kMaxDistance && fit::sanitizeDistance(0.7) == 0.7,
          "R3g: the panel distance the rule rescales by is bounded (nonsense is the shipped 1.0)");
    check(fit::plausibleFraction(0.6083) && !fit::plausibleFraction(0.0) && !fit::plausibleFraction(0.049) && !fit::plausibleFraction(3.01) &&
              !fit::plausibleFraction(std::nan("")) && fit::plausibleFraction(fit::kMinFraction) && fit::plausibleFraction(fit::kMaxFraction),
          "R3g: a plausible footprint fraction is 0.05..3.0");
}

// ---- R4: sizes another target already has --------------------------------------------------------------------------------
void caseR4() {
    {
        std::vector<uint32_t> hits;
        for (uint32_t w = 640; w <= 8192; w += 16)
            if (fit::collides(w)) hits.push_back(w);
        check(hits.size() == 2 && hits[0] == 1920 && hits[1] == 3840, "R4a: of every multiple of 16 the colliding sizes are exactly 1920x1080 and 3840x2160");
        check(!fit::collides(3856) && !fit::collides(3824) && !fit::collides(5040) && !fit::collides(3504), "R4a: their neighbours and the widths Sean flew do not collide");
    }
    {   // A fit target of exactly 3840 px (a footprint of 3840 / 0.70 = 5485.7 px at distance 0.7): nudged up one step.
        const fit::Decision d = fit::decide(measuredInputs(4032, 0.7, 3840.0 / 0.70 * 0.7 / 4032.0));
        checkf(d.width == 3856 && d.nudgedFrom == 3840 && d.height == refHeight(3856), "R4b: a fit of 3840x2160 is nudged to 3856 (got %u, from %u)", d.width, d.nudgedFrom);
        check(!fit::collides(d.width), "R4b: the result collides with nothing");
    }
    {   // At a cap of 3840 the nudge goes down.
        const fit::Decision d = fit::decide(measuredInputs(3072, 0.5, 1.5));
        checkf(d.legacyWidth == 3840 && d.capped && d.width == 3824 && d.nudgedFrom == 3840, "R4c: capped at 3840 (cap = the legacy width of a 3072 px eye), nudged down to 3824 (got %u, from %u)", d.width, d.nudgedFrom);
    }
    {   // The legacy rule is never nudged: a 3072 px eye has always been 3840.
        fit::Inputs in = seedInputs(3072, 0.7);
        in.route.keyAuto = false;
        const fit::Decision d = fit::decide(in);
        check(d.width == 3840 && d.nudgedFrom == 0, "R4e: the legacy rule is untouched even where it lands on a colliding size");
    }
    check(fit::nudgeOffCollisions(3840, 3840, 3840) == 3840 && fit::nudgeOffCollisions(3840, 3840, 3856) == 3856 && fit::nudgeOffCollisions(3840, 3824, 3840) == 3824 &&
              fit::nudgeOffCollisions(3600, 2880, 5040) == 3600 && fit::nudgeOffCollisions(1920, 1920, 1936) == 1936,
          "R4d: the nudge prefers the larger size, goes down when there is no room above, and leaves a one-size band alone");
    {   // Over a grid no fit ever lands on a colliding size (unless the whole band is one).
        bool clean = true;
        for (uint32_t eye : {2016u, 3000u, 3072u, 3296u, 3600u, 4032u, 4600u})
            for (double d : {0.4, 0.5, 0.6, 0.7, 0.8, 0.9, 1.0})
                for (double frac : {0.5, 0.6, 0.6083, 0.7, 0.8, 0.9}) {
                    const fit::Decision r = fit::decide(measuredInputs(eye, d, frac));
                    if (r.rule == fit::Rule::Fitted && fit::collides(r.width) && r.legacyWidth != r.width) clean = false;
                }
        check(clean, "R4f: no fit over a grid of eyes, distances and footprints lands on another target's size");
    }
}

// ---- R5: the footprint geometry ------------------------------------------------------------------------------------------
struct Scene {
    float model[12];
    float clip[16];
    float positions[4][3];
    float size[2];
};
Scene scenarioA(double z0) {
    Scene s{};
    // X = x, Y = y, Z = z + z0
    const float m[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, static_cast<float>(z0)};
    std::memcpy(s.model, m, sizeof(m));
    // cb1[270..273]: columns of a symmetric-ish projection: clip.x = a X + c Z, clip.y = b Y, clip.w = Z
    const float c[16] = {0.9418f, 0, 0, 0,   0, 0.9714f, 0, 0,   -0.165f, 0, 0, 1.0f,   0, 0, 0.025f, 0};
    std::memcpy(s.clip, c, sizeof(c));
    const float p[4][3] = {{-1, -1, 0}, {1, -1, 0}, {-1, 1, 0}, {1, 1, 0}};
    std::memcpy(s.positions, p, sizeof(p));
    s.size[0] = 20.687f;
    s.size[1] = 20.0f;
    return s;
}
fit::Footprint measure(const Scene& s) { return fit::footprintOfQuad(s.model, s.clip, s.positions, s.size); }

void caseR5() {
    const double z0 = 22.4;
    const Scene a = scenarioA(z0);
    const fit::Footprint f = measure(a);
    check(f.valid, "R5a: a centred quad in front of the eye has a footprint");
    // Closed form: ndc.x = (a X + c Z) / Z with X = +-size.x, Z = z0.
    const double wantW = 0.9418 * 20.687 / z0, wantH = 0.9714 * 20.0 / z0;
    checkf(closeTo(f.widthFraction(), wantW, 1e-6), "R5b: the horizontal footprint is a x SIZE.x / Z = %.6f of the eye (got %.6f)", wantW, f.widthFraction());
    checkf(closeTo(f.heightFraction(), wantH, 1e-6), "R5b: the vertical footprint is b x SIZE.y / Z = %.6f of the eye (got %.6f)", wantH, f.heightFraction());
    checkf(closeTo(f.x0, (-0.9418 * 20.687 - 0.165 * z0) / z0, 1e-6) && closeTo(f.x1, (0.9418 * 20.687 - 0.165 * z0) / z0, 1e-6),
           "R5b: the left and right edges are where the projection's asymmetry (cb1[272].x) puts them (got %.5f..%.5f)", f.x0, f.x1);
    {   // The vertices' order is irrelevant: the extent is of the four corners.
        Scene r = a;
        const float p[4][3] = {{1, 1, 0}, {-1, 1, 0}, {1, -1, 0}, {-1, -1, 0}};
        std::memcpy(r.positions, p, sizeof(p));
        check(closeTo(measure(r).widthFraction(), f.widthFraction(), 1e-12), "R5c: the footprint does not depend on the vertex order");
    }
    {   // SIZE scales the corners; the quad's own corners are read, not assumed: a quad of half the extent is half the footprint.
        Scene r = a;
        for (auto& v : r.positions) { v[0] *= 0.5f; v[1] *= 0.5f; }
        check(closeTo(measure(r).widthFraction(), wantW * 0.5, 1e-6), "R5c: half-size vertex corners are half the footprint (the vertices are read, SIZE multiplies them)");
        Scene t = a;
        t.size[0] *= 2.0f;
        check(closeTo(measure(t).widthFraction(), wantW * 2.0, 1e-6), "R5c: a doubled SIZE.x is a doubled footprint");
    }
    {   // A model rotation and scale: the real composite's world matrix is not the identity. Rotate 10 degrees about Y, scale y by 0.596.
        Scene r = a;
        const double th = 10.0 * 3.14159265358979323846 / 180.0;
        const float m[12] = {static_cast<float>(std::cos(th)), 0, static_cast<float>(std::sin(th)), 0,
                             0, 0.596f, 0, 0,
                             static_cast<float>(-std::sin(th)), 0, static_cast<float>(std::cos(th)), static_cast<float>(z0)};
        std::memcpy(r.model, m, sizeof(m));
        const fit::Footprint g = measure(r);
        // Expected by direct evaluation of the shader's arithmetic, in double, written out here independently.
        double lo = 1e9, hi = -1e9;
        for (int i = 0; i < 4; ++i) {
            const double x = r.positions[i][0] * r.size[0], y = r.positions[i][1] * r.size[1];
            const double X = m[0] * x + m[1] * y + m[2] * 0 + m[3];
            const double Z = m[8] * x + m[9] * y + m[10] * 0 + m[11];
            const double cx = X * r.clip[0] + 0 * r.clip[4] + Z * r.clip[8] + r.clip[12];
            const double cw = X * r.clip[3] + 0 * r.clip[7] + Z * r.clip[11] + r.clip[15];
            lo = std::min(lo, cx / cw);
            hi = std::max(hi, cx / cw);
        }
        check(g.valid && closeTo(g.widthFraction(), (hi - lo) / 2.0, 1e-9), "R5d: a rotated, non-uniformly scaled panel is the shader's own arithmetic (perspective foreshortening included)");
    }
    {   // Refusals.
        Scene behind = scenarioA(-5.0);
        const fit::Footprint b = measure(behind);
        check(!b.valid && std::strcmp(b.why, "behind-eye") == 0, "R5e: a panel behind the eye (w <= 0) has no footprint");
        Scene flat = a;
        flat.size[0] = 0.0f;
        const fit::Footprint d = measure(flat);
        check(!d.valid && std::strcmp(d.why, "degenerate") == 0, "R5e: a zero-width quad is degenerate");
        Scene nan = a;
        nan.clip[0] = std::nanf("");
        const fit::Footprint n = measure(nan);
        check(!n.valid && std::strcmp(n.why, "not-finite") == 0, "R5e: a NaN in the constants is refused, not measured");
    }
    {   // cb1[273] is part of the clip position (the real one is (0, 0, 0.025, 0), so its x and w do nothing on the shipped rows; the
        // instrument still includes them, because it measures what the shader computes): a non-zero x and w must move the footprint.
        Scene r = scenarioA(z0);
        r.clip[12] = 0.1f;
        r.clip[15] = 0.5f;
        const fit::Footprint g = measure(r);
        // Direct evaluation: ndc.x = (a X + c Z + 0.1) / (Z + 0.5) at X = +-SIZE.x, Z = z0.
        const double zz = z0, w = zz + 0.5;
        const double lo = (-0.9418 * 20.687 - 0.165 * zz + 0.1) / w, hi = (0.9418 * 20.687 - 0.165 * zz + 0.1) / w;
        check(g.valid && closeTo(g.widthFraction(), (hi - lo) / 2.0, 1e-6) && !closeTo(g.widthFraction(), wantW, 1e-3),
              "R5g: cb1[273] is added to the clip position (a non-zero x and w move the footprint)");
    }
    {   // The census flight's own eye rows (flight 20260930_202113, eye 1, the right eye), a uniform model scale: the eye's cant (the small
        // cross terms in cb1[270..272].yw) moves the corners' w by up to 1.5% of Z, so a x SIZE.x / Z still holds to a few percent.
        Scene r = scenarioA(z0);
        const float rows[16] = {0.9417881f, -0.01460095f, 0, -0.01424018f, 0.01386155f, 0.9713833f, 0, 0.001570765f,
                                -0.1650185f, -0.001733914f, 0, 0.9998974f, 0, 0, 0.025f, 0};
        std::memcpy(r.clip, rows, sizeof(rows));
        const fit::Footprint g = measure(r);
        check(g.valid && closeTo(g.widthFraction() / (0.9417881 * 20.687 / z0), 1.0, 0.03), "R5f: the real eye rows of a flown census give the closed form within 3% (the eye's cant moves the corners' w by up to 1.5%)");
    }
}

// ---- R6: the distance law through the geometry ----------------------------------------------------------------------------
void caseR6() {
    for (double z0 : {22.4, 32.0, 54.9}) {
        const double base = measure(scenarioA(z0)).widthFraction();
        for (double d : {0.5, 0.7, 1.4, 2.0}) {
            Scene s = scenarioA(z0 * d);   // panel_distance scales the z translation (float 47 of cb0, row 11's w) by d
            const double got = measure(s).widthFraction();
            checkf(closeTo(got * d, base, 1e-6), "R6a: scaling the panel's z translation by %.2f scales the footprint by 1/%.2f (z0 %.1f: %.6f x %.2f vs %.6f)", d, d, z0, got, d, base);
        }
    }
    {   // Through the persisted fraction: normalise a drawn fraction by the applied distance and the resolver's rescale gives the drawn one back.
        const double z0 = 32.0;
        const double drawn = measure(scenarioA(z0 * 0.7)).widthFraction();
        const double frac1 = drawn * 0.7;   // what the instrument stores
        const fit::Decision d = fit::decide(measuredInputs(4032, 0.7, frac1));
        check(closeTo(d.footprintPx, drawn * 4032.0, 1e-6), "R6b: the stored fraction at distance 1 rescaled at the same distance is the footprint that was drawn");
        const fit::Decision half = fit::decide(measuredInputs(4032, 1.4, frac1));
        check(closeTo(half.footprintPx, drawn * 4032.0 * 0.5, 1e-6), "R6b: and at twice the distance it is half");
    }
}

// ---- R7: the sample store ------------------------------------------------------------------------------------------------
void caseR7() {
    fit::FractionStore s;
    double m = -1;
    check(!s.median(&m) && s.kept() == 0 && s.total() == 0, "R7a: an empty store has no median");
    s.add(0.5);
    check(s.median(&m) && m == 0.5 && s.kept() == 1, "R7a: one sample is its own median");
    s.add(0.9);
    check(s.median(&m) && closeTo(m, 0.7, 1e-6), "R7a: two samples: the median is the mean of the two");
    s.add(0.1);
    check(s.median(&m) && closeTo(m, 0.5, 1e-6), "R7a: three samples: the middle one");
    double lo = 0, hi = 0;
    check(s.range(&lo, &hi) && closeTo(lo, 0.1, 1e-6) && closeTo(hi, 0.9, 1e-6), "R7b: the range is the smallest and the largest");
    s.clear();
    check(!s.median(&m) && s.total() == 0, "R7a: clear empties it");
    // A long ramp: the thinning keeps the median.
    for (uint32_t i = 0; i < 10000; ++i) s.add(0.2 + 0.6 * (static_cast<double>(i) / 9999.0));
    check(s.total() == 10000 && s.kept() <= fit::FractionStore::kCap && s.kept() >= fit::FractionStore::kCap / 2, "R7c: a long session is thinned to between half the cap and the cap");
    check(s.median(&m) && closeTo(m, 0.5, 0.01), "R7c: the thinned store's median is still the session's (ramp 0.2..0.8: 0.5)");
    check(s.range(&lo, &hi) && lo < 0.21 && hi > 0.79, "R7c: and its range still spans the session");
    // Noise around a constant: the median is the constant.
    s.clear();
    uint32_t seed = 12345;
    for (uint32_t i = 0; i < 6000; ++i) {
        seed = seed * 1664525u + 1013904223u;
        s.add(0.869 + (static_cast<double>(seed >> 8) / 16777216.0 - 0.5) * 0.01);
    }
    check(s.median(&m) && closeTo(m, 0.869, 0.002), "R7d: samples scattered by +-0.5% of the eye have the constant as their median");
    {   // Monotone in q and inside the store's own range, over those noisy samples.
        bool mono = true, within = true;
        double prev = -1.0, v = 0.0, rlo = 0.0, rhi = 0.0;
        s.range(&rlo, &rhi);
        for (int k = 0; k <= 100; ++k) {
            if (!s.percentile(k / 100.0, &v)) { mono = false; break; }
            mono = mono && v >= prev - 1e-12;
            within = within && v >= rlo - 1e-12 && v <= rhi + 1e-12;
            prev = v;
        }
        check(mono && within, "R7d: the percentile never falls as q rises and never leaves the store's min..max");
    }

    // ---- the percentile the session stores: the head-on floor, p10 ----
    check(fit::kFootprintQuantile == 0.10, "R7e: the session stores the 10th percentile of its on-foot widths (the head-on floor), not the median");
    {
        fit::FractionStore p;
        double q = -1.0;
        check(!p.percentile(0.1, &q) && q == -1.0, "R7f: an empty store has no percentile, and leaves the output alone");
        p.add(0.8);
        check(p.percentile(0.1, &q) && closeTo(q, 0.8, 1e-6) && p.percentile(0.0, &q) && closeTo(q, 0.8, 1e-6) && p.percentile(1.0, &q) && closeTo(q, 0.8, 1e-6),
              "R7f: one sample is every percentile of itself");
        p.add(0.2);
        check(p.percentile(0.1, &q) && closeTo(q, 0.26, 1e-6) && p.percentile(0.5, &q) && closeTo(q, 0.5, 1e-6) && p.percentile(0.9, &q) && closeTo(q, 0.74, 1e-6),
              "R7f: two samples interpolate linearly: 0.2 and 0.8 give 0.26 at q 0.10, 0.50 at 0.50 and 0.74 at 0.90");
        p.clear();
        for (double v : {0.30, 0.10, 0.50, 0.20, 0.40}) p.add(v);   // unsorted: the position is q x 4
        check(p.percentile(0.0, &q) && closeTo(q, 0.10, 1e-6) && p.percentile(1.0, &q) && closeTo(q, 0.50, 1e-6) && p.percentile(0.25, &q) && closeTo(q, 0.20, 1e-6) &&
                  p.percentile(0.5, &q) && closeTo(q, 0.30, 1e-6) && p.percentile(0.75, &q) && closeTo(q, 0.40, 1e-6),
              "R7f: five unsorted samples: the ends and the quartiles are exactly their order statistics");
        check(p.percentile(0.10, &q) && closeTo(q, 0.14, 1e-6) && p.percentile(0.90, &q) && closeTo(q, 0.46, 1e-6),
              "R7f: q 0.10 and 0.90 interpolate between the two nearest: 0.10 + 0.4 x 0.10 = 0.14 and 0.40 + 0.6 x 0.10 = 0.46");
        check(p.percentile(-1.0, &q) && closeTo(q, 0.10, 1e-6) && p.percentile(7.0, &q) && closeTo(q, 0.50, 1e-6) && p.percentile(std::nan(""), &q) && closeTo(q, 0.10, 1e-6),
              "R7f: a q outside [0, 1], or NaN, is the nearest end (never an index outside the store)");
        p.clear();
        for (double v : {0.05, 0.09, 0.01, 0.10, 0.03, 0.00, 0.07, 0.02, 0.08, 0.04, 0.06}) p.add(v);   // eleven: q x 10 lands on a sample
        double med = 0.0;
        check(p.percentile(0.10, &q) && closeTo(q, 0.01, 1e-6) && p.percentile(0.5, &q) && p.median(&med) && closeTo(q, 0.05, 1e-6) && closeTo(q, med, 1e-9),
              "R7f: eleven samples: q 0.10 is exactly the second smallest and q 0.50 is the median");
    }
    {   // The thinned store keeps its p10: the 10000 sample ramp 0.2..0.8 again.
        s.clear();
        for (uint32_t i = 0; i < 10000; ++i) s.add(0.2 + 0.6 * (static_cast<double>(i) / 9999.0));
        check(s.percentile(fit::kFootprintQuantile, &m) && closeTo(m, 0.26, 0.01), "R7g: the thinned store's p10 is still the session's (ramp 0.2..0.8: 0.26)");
    }
    {   // A skewed session: 40% of it head-on at 0.869, the rest with the head turned (0.90..1.20): the floor is the p10, and the median runs 9% above it.
        s.clear();
        for (uint32_t i = 0; i < 100; ++i) s.add(i < 40 ? 0.869 : 0.9 + 0.3 * static_cast<double>(i - 40) / 59.0);
        double p10 = 0.0, med = 0.0;
        check(s.percentile(fit::kFootprintQuantile, &p10) && s.median(&med) && closeTo(p10, 0.869, 1e-6) && closeTo(med, 0.948, 0.002) && med > p10 * 1.05,
              "R7h: on a skewed session the p10 is the head-on floor (0.869) and differs from the median (0.948): storing the median is a different number");
    }
}

// ---- R8: the stored record -----------------------------------------------------------------------------------------------
void caseR8() {
    fit::Record r;
    r.fractionAtUnit = 0.869097;
    r.eyeWidth = 4032;
    r.distance = 0.7;
    r.samples = 41;
    char text[200];
    const int n = fit::formatRecord(text, sizeof(text), r);
    check(n > 0 && static_cast<size_t>(n) < sizeof(text) && text[n - 1] == '\n', "R8a: the record is one line of key=value text");
    check(std::strcmp(text, "fraction=0.869097 est=p10 eye=4032 distance=0.700 samples=41\n") == 0,
          "R8a: and it reads exactly `fraction=0.869097 est=p10 eye=4032 distance=0.700 samples=41` (est=p10 says the fraction is the head-on floor, not a median)");
    fit::Record back;
    check(fit::parseRecord(text, &back) && closeTo(back.fractionAtUnit, 0.869097, 1e-6) && back.eyeWidth == 4032 && closeTo(back.distance, 0.7, 1e-3) && back.samples == 41,
          "R8a: a record round-trips");
    check(fit::parseRecord("fraction=0.6083 est=p10", &back) && closeTo(back.fractionAtUnit, 0.6083, 1e-9) && back.eyeWidth == 0, "R8b: only the fraction and the p10 tag are required");
    check(fit::parseRecord("  eye=4032   est=p10 fraction=0.7  junk samples=3 \n", &back) && closeTo(back.fractionAtUnit, 0.7, 1e-9) && back.samples == 3,
          "R8b: tokens may come in any order and unknown ones are ignored");
    check(!fit::parseRecord("", &back) && !fit::parseRecord(nullptr, &back) && !fit::parseRecord("eye=4032 samples=3 est=p10", &back) && !fit::parseRecord("fraction=abc est=p10", &back),
          "R8c: nothing, no fraction and a non-number are refused");
    check(!fit::parseRecord("fraction=0.01 est=p10", &back) && !fit::parseRecord("fraction=3.5 est=p10", &back) && !fit::parseRecord("fraction=-0.6 est=p10", &back) &&
              !fit::parseRecord("fraction=nan est=p10", &back),
          "R8c: an implausible fraction (0.01, 3.5, negative, NaN) is refused");
    check(fit::parseRecord("fraction=0.05 est=p10", &back) && fit::parseRecord("fraction=3.0 est=p10", &back), "R8c: the bounds themselves are accepted");
    // The tag: a build that stored the session's MEDIAN wrote the same line without it, and a median is about 6% above the floor, so it is no p10.
    check(!fit::parseRecord("fraction=0.889105 eye=4032 distance=0.650 samples=188\n", &back) && !fit::parseRecord("fraction=0.6083", &back) &&
              !fit::parseRecord("fraction=0.869097 eye=4032 distance=0.700 samples=41\n", &back),
          "R8d: an older build's record, which has no est tag (Sean's own file, a median, included), is refused: the seed applies until a session measures a p10");
    check(!fit::parseRecord("fraction=0.889105 est=median eye=4032 distance=0.650 samples=188\n", &back) && !fit::parseRecord("fraction=0.8 est=p50", &back) &&
              !fit::parseRecord("fraction=0.8 est=", &back) && !fit::parseRecord("fraction=0.8 est=p10x", &back) && !fit::parseRecord("fraction=0.8 est=p1", &back) &&
              !fit::parseRecord("fraction=0.8 est p10", &back) && !fit::parseRecord("fraction=0.8 p10", &back),
          "R8d: nor is any other estimator tag one: only exactly est=p10 is");
}

// ---- R9: the log's text --------------------------------------------------------------------------------------------------
// The tokens before " -- " of a line: key=value pairs, the way edvr_log.py reads them.
std::vector<std::pair<std::string, std::string>> tokensOf(const std::string& line, const char* from) {
    std::vector<std::pair<std::string, std::string>> out;
    size_t at = from ? line.find(from) : 0;
    if (at == std::string::npos) return out;
    at += from ? std::strlen(from) : 0;
    const size_t stop = line.find(" -- ", at);
    const std::string body = line.substr(at, stop == std::string::npos ? std::string::npos : stop - at);
    size_t i = 0;
    while (i < body.size()) {
        while (i < body.size() && body[i] == ' ') ++i;
        size_t j = i;
        while (j < body.size() && body[j] != ' ') ++j;
        const std::string tok = body.substr(i, j - i);
        const size_t eq = tok.find('=');
        if (eq != std::string::npos && eq > 0) out.emplace_back(tok.substr(0, eq), tok.substr(eq + 1));
        i = j;
    }
    return out;
}
std::string tok(const std::vector<std::pair<std::string, std::string>>& v, const char* key) {
    for (const auto& kv : v) if (kv.first == key) return kv.second;
    return "";
}

// ---- the fixture: tools\vscreen_fit_fixture.log, what a good flight on Sean's rig writes, held to exactly what the formatters write ----
// edvr_log.py --vscreen-fit's own self-test reads the checked-in file. Regenerate it with `vscreen_fit_test.exe --write-fixture <path>`
// after a deliberate change to a line's text; the check below fails the build until the file says what the formatters say.
std::vector<fit::WindowLine> fixtureWindows() {
    std::vector<fit::WindowLine> out;
    // A good flight on Sean's rig under the calibrated rule. The on-foot window medians are the screen's width in eye pixels at distance
    // 0.7 (his own logs: 5000..6100, median 5267: the head turning only ever widens it); the session's p10 is the head-on floor the next
    // launch fits (about 5006 px, the calibration point); the shape is a little under 16:9 because the same turning stretches the height.
    const double medians[] = {5262.0, 5281.0, 5270.0};   // the on-foot medians of windows 2..4, in eye pixels at distance 0.7
    const double lows[] = {5011.0, 5004.0, 5009.0};      // each window's smallest sample
    const double highs[] = {5890.0, 6044.0, 5961.0};     // and its largest
    const double shapes[] = {1.71, 1.69, 1.70};          // each window's width / height in eye pixels
    const double floors[] = {5008.0, 5004.0, 5006.0};    // the session's p10 after each window, in eye pixels at distance 0.7
    const uint32_t onFoot[] = {60, 58, 59};
    {   // window 1: the main menu (the screen is on show, the commander is not on foot)
        fit::WindowLine w;
        w.window = 1; w.eyeKnown = true; w.eyeW = 4032; w.eyeH = 3898;
        w.samples = 44; w.onFoot = 0; w.other = 44; w.draws = 5280;
        w.distance = 0.7; w.applied = 0.0;
        w.haveOther = true; w.otherFrac = 5640.0 / 4032.0;
        w.fitWidth = 3504; w.legacyWidth = 5040;
        out.push_back(w);
    }
    uint32_t sessionN = 0;
    double storedFrac1 = 0.0;   // what the file holds: written once the session has samples enough, rewritten only when the p10 moves by 0.2%
    for (int i = 0; i < 3; ++i) {
        fit::WindowLine w;
        w.window = static_cast<uint32_t>(i + 2); w.eyeKnown = true; w.eyeW = 4032; w.eyeH = 3898;
        w.onFoot = onFoot[i]; w.samples = onFoot[i]; w.other = 0; w.draws = 5400;
        w.distance = 0.7; w.applied = 0.7;
        w.haveFoot = true;
        w.footFrac = medians[i] / 4032.0; w.footLo = lows[i] / 4032.0; w.footHi = highs[i] / 4032.0;
        w.footH = (medians[i] / shapes[i]) / 3898.0;
        sessionN += onFoot[i];
        w.haveSession = true; w.sessionFrac1 = floors[i] * 0.7 / 4032.0; w.sessionN = sessionN;
        if (storedFrac1 == 0.0 || std::fabs(w.sessionFrac1 - storedFrac1) > 0.002 * storedFrac1) storedFrac1 = w.sessionFrac1;
        w.persisted = true; w.persistedFrac1 = storedFrac1;
        w.fitWidth = fit::decide(measuredInputs(4032, 0.7, w.sessionFrac1)).width;
        w.legacyWidth = 5040;
        out.push_back(w);
    }
    return out;
}
std::string fixtureText() {
    std::string out;
    char line[1200];
    auto push = [&](const char* ts, const char* text) { out += "["; out += ts; out += "] "; out += text; out += "\n"; };
    push("06:07:03.237", "version v0.18.0-rc.5-20-g1a2b3c4 (build 6ABE4701) -- this DLL was linked 2026-10-01 14:02:11 UTC");
    fit::formatRuleLine(line, sizeof(line), fit::decide(seedInputs(4032, 0.7)), 4032);
    push("06:07:03.620", line);
    push("06:07:03.669", "vScreen resolution: 6 site(s) forcing 1920x1080, on game build 332841 -- NOT the build this was developed against.");
    push("06:07:03.669", "vScreen resolution: 1920x1080 -> 3504x1971 at 6 site(s). This writes to game CODE -- to 6 pairs of numbers and nothing else. It reverts when the game closes.");
    fit::formatArmedLine(line, sizeof(line));
    push("06:07:05.412", line);
    static const char* const stamps[] = {"06:07:35.412", "06:08:05.413", "06:08:35.412", "06:09:05.414"};
    const std::vector<fit::WindowLine> windows = fixtureWindows();
    for (size_t i = 0; i < windows.size(); ++i) {
        fit::formatWindowLine(line, sizeof(line), windows[i]);
        push(stamps[i], line);
    }
    return out;
}

void caseR9() {
    char line[1200];
    {   // fitted, from the seed
        const fit::Inputs in = seedInputs(4032, 0.7);
        const fit::Decision d = fit::decide(in);
        const int n = fit::formatRuleLine(line, sizeof(line), d, 4032);
        const std::string s(line);
        check(n > 0 && n < 1100, "R9a: the rule line fits the log's line buffer (under 1100 bytes)");
        check(s.rfind("vScreen resolution: auto = 3504 wide: ", 0) == 0, "R9a: the line is the `vScreen resolution:` line and names the width");
        const auto t = tokensOf(s, "vScreen resolution: auto = 3504 wide: ");
        check(tok(t, "rule") == "fitted" && tok(t, "source") == "seed" && tok(t, "route") == "run" && tok(t, "eye") == "4032" && tok(t, "distance") == "0.700" &&
                  tok(t, "legacy") == "5040",
              "R9b: the fitted line says rule=fitted source=seed route=run and the eye, distance and legacy width");
        check(tok(t, "footprint") == "5006" && tok(t, "m") == "0.700" && tok(t, "floor") == "2880" && tok(t, "cap") == "5040" && tok(t, "clamp") == "none",
              "R9b: and the footprint (5006), m (0.700, three decimals, so a retuned m never prints rounded), floor, cap and clamp the reader recomputes the width from");
        check(has(s, "FITTED") && has(s, "calibration point"), "R9c: the prose says it is fitted and, for the seed, that nothing was measured");
        check(has(s, "head-on") && has(s, "0.700 x that") && has(s, "eye pixels per texel") && has(s, "5006 px head-on at distance 0.7 on a 4032 px eye, which fits 3504 wide") &&
                  !has(s, "so that is the width"),
              "R9c: and says what is true of the width: 0.700 of the head-on footprint, about 1.4 eye pixels per texel (never that the width IS the footprint)");
        char again[1200];
        const fit::Decision measuredAgain = fit::decide(measuredInputs(4032, 0.7, 0.869097));
        fit::formatRuleLine(again, sizeof(again), measuredAgain, 4032);
        check(has(again, "head-on floor (p10)") && has(again, "previous session measured") && !has(again, "median"),
              "R9c: a measured launch says the stored footprint is the head-on floor (p10), not a median");
    }
    {   // fitted, measured, floored and nudged flags
        const fit::Decision d = fit::decide(measuredInputs(4032, 1.0, 0.6083));
        fit::formatRuleLine(line, sizeof(line), d, 4032);
        const auto t = tokensOf(line, "vScreen resolution: auto = 2880 wide: ");
        check(tok(t, "rule") == "fitted" && tok(t, "source") == "measured" && tok(t, "clamp") == "floor" && has(line, "previous session measured"),
              "R9b: a floored measured fit says clamp=floor and source=measured");
        const fit::Decision n = fit::decide(measuredInputs(4032, 0.7, 3840.0 / 0.70 * 0.7 / 4032.0));
        fit::formatRuleLine(line, sizeof(line), n, 4032);
        check(has(line, " nudged=yes") && has(line, "3840x2160") && has(line, "3856 wide"), "R9b: a nudged fit says nudged=yes and names the size it avoided");
        check(std::strlen(line) < 1100, "R9a: a nudged measured line still fits the buffer");
    }
    {   // legacy: names the failed condition
        fit::Inputs in = seedInputs(4032, 0.7);
        in.route.keyAuto = false;
        in.route.layerWhy = "fix.ui_quality is off";
        const fit::Decision d = fit::decide(in);
        const int n = fit::formatRuleLine(line, sizeof(line), d, 4032);
        const std::string s(line);
        check(n > 0 && n < 1100 && s.rfind("vScreen resolution: auto = 5040 wide: ", 0) == 0, "R9d: the legacy line names the legacy width");
        const auto t = tokensOf(s, "vScreen resolution: auto = 5040 wide: ");
        check(tok(t, "rule") == "legacy" && tok(t, "source") == "none" && tok(t, "route") == "no" && tok(t, "m") == "1.25" && tok(t, "legacy") == "5040",
              "R9d: rule=legacy source=none route=no m=1.25");
        check(has(s, "LEGACY") && has(s, "the on-foot world route is off") && has(s, "fix.ui_quality is off") && !has(s, "panel_curvature"),
              "R9d: and the prose names EVERY route condition that failed (the curve is not one of them)");
    }
    {   // the hint
        char hint[200];
        const fit::Decision fitted = fit::decide(seedInputs(4032, 0.7));
        const int a = fit::formatHint(hint, sizeof(hint), fitted);
        check(a > 0 && a < 199 && has(hint, "3504x1971") && has(hint, "fitted") && has(hint, "estimate"), "R9e: the menu hint says fitted, the size, and that a seed is an estimate");
        check(has(hint, "fitted to about 70% of the on-foot screen's width in your view") && !has(hint, "fitted to the width"),
              "R9e: and that the width is about 70% of the screen's width in the view (never that it IS that width)");
        const fit::Decision measured = fit::decide(measuredInputs(4032, 0.7, 0.62));
        fit::formatHint(hint, sizeof(hint), measured);
        check(has(hint, "fitted") && !has(hint, "estimate"), "R9e: a measured fit is not called an estimate");
        fit::Inputs in = seedInputs(4032, 0.7);
        in.route.keyAuto = false;
        const int b = fit::formatHint(hint, sizeof(hint), fit::decide(in));
        check(b > 0 && b < 199 && has(hint, "5040x2835") && has(hint, "125%") && has(hint, "will not run"), "R9e: the legacy hint says 125% and that the route will not run");
    }
    {   // the 30 s line
        fit::WindowLine w;
        w.window = 3;
        w.eyeKnown = true;
        w.eyeW = 4032;
        w.eyeH = 3898;
        w.samples = 57;
        w.onFoot = 41;
        w.other = 16;
        w.skipped = 2;
        w.late = 1;
        std::snprintf(w.skipWhy, sizeof(w.skipWhy), "vb0-stride:2");
        w.draws = 5400;
        w.distance = 0.7;
        w.applied = 0.7;
        w.haveFoot = true;
        w.footFrac = 5267.0 / 4032.0;     // the window's median on-foot width: 5267 px of the eye at distance 0.7
        w.footLo = 5004.0 / 4032.0;
        w.footHi = 5990.0 / 4032.0;
        w.footH = (5267.0 * 9.0 / 16.0) / 3898.0;   // a 16:9 screen in square eye pixels
        w.haveOther = true;
        w.otherFrac = 0.8;
        w.haveSession = true;
        w.sessionFrac1 = 0.869097;        // the session's p10 (the head-on floor), as a fraction of the eye at distance 1: 5006 px at 0.7
        w.sessionN = 41;
        w.persisted = true;
        w.persistedFrac1 = 0.869097;
        w.fitWidth = 3504;
        w.legacyWidth = 5040;
        const int n = fit::formatWindowLine(line, sizeof(line), w);
        const std::string s(line);
        check(n > 0 && n < 700, "R9f: the 30 s line is one short line");
        check(s.rfind("vscreen footprint 30s: window=3 ", 0) == 0, "R9f: it starts `vscreen footprint 30s: window=3`");
        const auto t = tokensOf(s, "vscreen footprint 30s: ");
        check(tok(t, "samples") == "57" && tok(t, "on-foot") == "41" && tok(t, "other") == "16" && tok(t, "skipped") == "2" && tok(t, "late") == "1" &&
                  tok(t, "why") == "vb0-stride:2" && tok(t, "draws") == "5400",
              "R9f: the counters and the skip reasons");
        check(tok(t, "distance") == "0.700" && tok(t, "applied") == "0.700" && tok(t, "eye") == "4032x3898", "R9f: the configured and the applied distance, and the eye the pixels are in");
        check(tok(t, "fp") == "5267" && tok(t, "frac") == "1.3063" && tok(t, "range") == "5004..5990" && tok(t, "other-fp") == "3226",
              "R9f: the on-foot footprint in eye pixels (the window's median, its range) and the other (menu) footprint");
        check(tok(t, "at1") == "3687" && tok(t, "frac1") == "0.9144" && tok(t, "session-n") == "41" && tok(t, "session-frac1") == "0.8691" && tok(t, "persisted") == "0.8691",
              "R9f: the window's footprint at distance 1, and the session's p10 (the head-on floor, 0.8691: a different number from the window's median) and what is stored");
        const double shape = std::atof(tok(t, "shape").c_str());
        check(closeTo(shape, 16.0 / 9.0, 0.002), "R9f: the shape (pixel aspect of the footprint) reads 1.778 for a 16:9 panel in square pixels");
        check(tok(t, "fit") == "3504" && tok(t, "legacy") == "5040" && tok(t, "m") == "0.700" && tok(t, "floor") == "2880", "R9f: and the width the next launch would fit, with m at three decimals");
        fit::WindowLine none;
        none.window = 1;
        none.distance = 1.0;
        fit::formatWindowLine(line, sizeof(line), none);
        const auto u = tokensOf(line, "vscreen footprint 30s: ");
        check(tok(u, "samples") == "0" && tok(u, "draws") == "0" && tok(u, "fp") == "-" && tok(u, "persisted") == "no" && tok(u, "fit") == "-" && tok(u, "eye") == "-",
              "R9g: an armed instrument that saw nothing still prints its line, with draws=0 and a dash for every measurement");
        // A save that failed: save-failed=N (the count so far this session) right after persisted=, and only then, so the line of a window
        // with none is byte for byte what it always was (R9h holds the fixture, every line of a good flight, to the formatters).
        check(!has(s, "save-failed") && !has(line, "save-failed"), "R9j: a window with no failed save carries no save-failed token at all");
        fit::WindowLine failedFirst = w;
        failedFirst.persisted = false;
        failedFirst.saveFailed = 1;
        fit::formatWindowLine(line, sizeof(line), failedFirst);
        const auto ff = tokensOf(line, "vscreen footprint 30s: ");
        check(tok(ff, "persisted") == "no" && tok(ff, "save-failed") == "1" && has(line, "persisted=no save-failed=1 fit=3504 legacy=5040"),
              "R9j: a first save that failed: persisted=no and save-failed=1, right after persisted= and before fit=");
        fit::WindowLine failedLater = w;
        failedLater.saveFailed = 3;
        fit::formatWindowLine(line, sizeof(line), failedLater);
        const auto fl = tokensOf(line, "vscreen footprint 30s: ");
        check(tok(fl, "persisted") == "0.8691" && tok(fl, "save-failed") == "3" && std::strlen(line) < 700,
              "R9j: a later save that failed: persisted= keeps the value that did reach the file, and save-failed=3 is the count so far");
    }
    {   // the line that says a save failed
        char sf[640];
        const int n = fit::formatSaveFailedLine(sf, sizeof(sf), 32, 1);
        check(n > 0 && n < 500 && std::strncmp(sf, "vscreen footprint: SAVE FAILED (Win32 error 32) -- ", 51) == 0,
              "R9k: the save-failed line starts `vscreen footprint: SAVE FAILED (Win32 error N) -- ` and fits the instrument's 520 byte buffer");
        check(has(sf, "did not reach vscreen_auto_footprint.txt") && has(sf, "the file keeps its previous value") && has(sf, "the next 30 s window tries again") &&
                  has(sf, "Failed saves this session: 1;") && has(sf, "save-failed=N") && has(sf, "persisted=no") && !has(sf, "No more of these lines"),
              "R9k: it says what did not happen, that the file keeps its previous value, that the next window retries, and what the 30 s lines will say");
        check(std::strncmp(sf, "vscreen footprint: armed", 24) != 0 && std::strncmp(sf, "vscreen footprint: not armed", 28) != 0 && std::strncmp(sf, "vscreen footprint: STOOD DOWN", 29) != 0 &&
                  !has(sf, "vscreen footprint 30s:"),
              "R9k: and it is none of the lines the reader tells apart by their first words (armed, not armed, STOOD DOWN) nor a 30 s line");
        char last[640];
        fit::formatSaveFailedLine(last, sizeof(last), 5, fit::kSaveFailedLineCap);
        check(fit::kSaveFailedLineCap == 3 && has(last, "Failed saves this session: 3;") && has(last, "No more of these lines this session."),
              "R9k: only three of these lines are printed a session, and the third says there will be no more");
    }
    {   // the once-per-change lines
        char armed[800], notArmed[400], down[400];
        fit::formatArmedLine(armed, sizeof(armed));
        fit::formatNotArmedLine(notArmed, sizeof(notArmed), "3504");
        fit::formatStoodDownLine(down, sizeof(down));
        check(std::strncmp(armed, "vscreen footprint: armed -- ", 28) == 0 && std::strlen(armed) < 700 && has(armed, "vscreen footprint 30s:"),
              "R9i: the arming line starts `vscreen footprint: armed` (the reader's word) and says what the 30 s line is");
        check(has(armed, "the on-foot head-on floor (p10) is stored") && !has(armed, "median"), "R9i: and says the stored number is the head-on floor (p10), not a median");
        check(std::strncmp(notArmed, "vscreen footprint: not armed -- ", 32) == 0 && has(notArmed, "\"3504\""), "R9i: the not-armed line says the explicit width it found");
        check(std::strncmp(down, "vscreen footprint: STOOD DOWN", 29) == 0, "R9i: the stood-down line starts `vscreen footprint: STOOD DOWN`");
    }
    {   // the fixture
        const std::string have = readFile("tools\\vscreen_fit_fixture.log");
        const std::string want = fixtureText();
        check(!have.empty() && have == want,
              "R9h: tools\\vscreen_fit_fixture.log is exactly what the formatters write (regenerate it: vscreen_fit_test.exe --write-fixture tools\\vscreen_fit_fixture.log)");
    }
}

// ---- R10: the setting parse ----------------------------------------------------------------------------------------------
void caseR10() {
    check(!fit::keyTextIsAuto(nullptr), "R10a: no text is not auto");
    check(fit::keyTextIsAuto("auto") && fit::keyTextIsAuto("AUTO") && fit::keyTextIsAuto("Auto") && !fit::keyTextIsAuto("autox") &&
              !fit::keyTextIsAuto("aut") && !fit::keyTextIsAuto("off") && !fit::keyTextIsAuto("") && !fit::keyTextIsAuto(" auto"),
          "R10a: auto in any case and nothing else");
}

// ---- R11: the state files on disk ----------------------------------------------------------------------------------------
#ifndef VSCREEN_FIT_MUTANT
std::wstring tempDir() {
    wchar_t base[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, base);
    std::wstring dir = std::wstring(base) + L"edvr_vscreen_fit_test_" + std::to_wstring(GetCurrentProcessId());
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}
void removeDir(const std::wstring& dir) {
    for (const wchar_t* f : {L"vscreen_auto_eye_width.txt", L"vscreen_auto_footprint.txt", L"vscreen_auto_footprint.txt.tmp"}) DeleteFileW((dir + L"\\" + f).c_str());
    RemoveDirectoryW(dir.c_str());
}
bool pathExists(const std::wstring& path) { return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; }
bool isDirectory(const std::wstring& path) {
    const DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
}
void writeRaw(const std::wstring& path, const char* text) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD w = 0;
    WriteFile(f, text, static_cast<DWORD>(std::strlen(text)), &w, nullptr);
    CloseHandle(f);
}
std::string slurpFile(const std::wstring& path) {
    std::ifstream in(path.c_str(), std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}
void caseR11() {
    const std::wstring dir = tempDir();
    fit::Record r;
    check(!edvr::lastKnownPanelFootprint(dir, &r), "R11a: nothing on record before the first measurement");
    fit::Record w;
    w.fractionAtUnit = 0.6171;
    w.eyeWidth = 4032;
    w.distance = 0.7;
    w.samples = 77;
    edvr::noteMeasuredPanelFootprint(dir, w);
    check(edvr::lastKnownPanelFootprint(dir, &r) && closeTo(r.fractionAtUnit, 0.6171, 1e-5) && r.eyeWidth == 4032 && r.samples == 77, "R11a: a stored measurement reads back");
    check(slurpFile(dir + L"\\vscreen_auto_footprint.txt") == "fraction=0.617100 est=p10 eye=4032 distance=0.700 samples=77\n",
          "R11a: and the file on disk says est=p10 (the writer tags what it stores: the head-on floor)");
    fit::Record w2 = w;
    w2.fractionAtUnit = 0.58;
    edvr::noteMeasuredPanelFootprint(dir, w2);
    check(edvr::lastKnownPanelFootprint(dir, &r) && closeTo(r.fractionAtUnit, 0.58, 1e-5), "R11a: a later one replaces it");
    // The eye width's own file is a different file with its own format: neither reads the other.
    uint32_t eye = 0;
    check(!edvr::lastKnownEyeWidth(dir, &eye), "R11b: storing a footprint does not create an eye width");
    edvr::noteResolvedEyeWidthForVScreenAuto(dir, 4032);
    check(edvr::lastKnownEyeWidth(dir, &eye) && eye == 4032 && edvr::lastKnownPanelFootprint(dir, &r) && closeTo(r.fractionAtUnit, 0.58, 1e-5),
          "R11b: nor does storing the eye width disturb the footprint");
    {   // A save returns whether the record reached the file, and a save that fails leaves the file exactly as it was.
        const std::wstring dest = dir + L"\\vscreen_auto_footprint.txt";
        const std::wstring tmp = dest + L".tmp";
        uint32_t err = 99;
        check(edvr::noteMeasuredPanelFootprint(dir, w, &err) && err == 0 && !pathExists(tmp) && slurpFile(dest) == "fraction=0.617100 est=p10 eye=4032 distance=0.700 samples=77\n",
              "R11f: a save that reaches the file returns true with Win32 error 0, writes the record whole, and leaves no temp file");
        const std::string good = slurpFile(dest);
        // Held open with no sharing: a replace cannot happen. (What the instrument meets when something has the file.)
        HANDLE lock = CreateFileW(dest.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        check(lock != INVALID_HANDLE_VALUE, "R11g: the destination can be held open with no sharing for the test");
        err = 0;
        const bool lockedSave = edvr::noteMeasuredPanelFootprint(dir, w2, &err);
        if (lock != INVALID_HANDLE_VALUE) CloseHandle(lock);
        check(!lockedSave && err != 0, "R11g: a destination held open with no sharing: the save returns false and gives the Win32 error");
        check(slurpFile(dest) == good && !pathExists(tmp), "R11g: and the file holds exactly what it held, with no temp file left behind");
        // The lock gone, the same record again: it saves (a window whose p10 has not moved, retried after a failed save).
        err = 99;
        check(edvr::noteMeasuredPanelFootprint(dir, w2, &err) && err == 0 && edvr::lastKnownPanelFootprint(dir, &r) && closeTo(r.fractionAtUnit, 0.58, 1e-5) && !pathExists(tmp),
              "R11h: the same record again with the lock gone: true, and it reads back");
        const std::string good2 = slurpFile(dest);
        // A reader holding the file open (read and write sharing, but not delete) stops a replace too; a write straight into the file would
        // get through it, so this is what tells a temp file moved over the destination from the destination written in place.
        lock = CreateFileW(dest.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        err = 0;
        const bool readerSave = edvr::noteMeasuredPanelFootprint(dir, w, &err);
        if (lock != INVALID_HANDLE_VALUE) CloseHandle(lock);
        check(lock != INVALID_HANDLE_VALUE && !readerSave && err != 0 && slurpFile(dest) == good2 && !pathExists(tmp),
              "R11i: a reader holding the file open (no delete sharing): false with the error, the file as it was, no temp (the record is moved over the file, never written into it)");
        // The temp file's name taken by a directory: the save cannot start, and the file is untouched.
        CreateDirectoryW(tmp.c_str(), nullptr);
        err = 0;
        const bool tmpSave = edvr::noteMeasuredPanelFootprint(dir, w, &err);
        check(!tmpSave && err != 0 && slurpFile(dest) == good2 && isDirectory(tmp), "R11j: the temp file's name taken by a directory: false with the error, the file as it was");
        RemoveDirectoryW(tmp.c_str());
        // A directory where the file should be (a first save that can never succeed): false, nothing readable, no temp left.
        const std::wstring blocked = dir + L"\\blocked";
        CreateDirectoryW(blocked.c_str(), nullptr);
        CreateDirectoryW((blocked + L"\\vscreen_auto_footprint.txt").c_str(), nullptr);
        err = 0;
        const bool dirSave = edvr::noteMeasuredPanelFootprint(blocked, w, &err);
        check(!dirSave && err != 0 && isDirectory(blocked + L"\\vscreen_auto_footprint.txt") && !pathExists(blocked + L"\\vscreen_auto_footprint.txt.tmp") &&
                  !edvr::lastKnownPanelFootprint(blocked, &r),
              "R11k: a directory where the file should be: the save returns false with the error, the directory stays, no temp is left, and nothing reads as a record");
        DeleteFileW((blocked + L"\\vscreen_auto_footprint.txt.tmp").c_str());   // (a writer that leaves its temp behind must not leave this directory behind too)
        RemoveDirectoryW((blocked + L"\\vscreen_auto_footprint.txt").c_str());
        RemoveDirectoryW(blocked.c_str());
        // Refusals are failures too: a fraction the record cannot hold, and no directory at all. Neither touches the file.
        err = 0;
        const bool junkSave = edvr::noteMeasuredPanelFootprint(dir, fit::Record{0.001, 4032, 1.0, 3}, &err);
        check(!junkSave && err != 0 && slurpFile(dest) == good2, "R11l: an implausible fraction is refused: false with an error, the file as it was");
        err = 0;
        check(!edvr::noteMeasuredPanelFootprint(L"", w, &err) && err != 0, "R11l: and so is no directory at all");
        check(edvr::noteMeasuredPanelFootprint(dir, w2) && slurpFile(dest) == good2, "R11l: the error out parameter is optional (an ordinary save, asking for none, still returns true)");
    }
    // The file an older build wrote, when it stored the session's MEDIAN (Sean's own, 2026-10-01): no est tag, so it is no record.
    writeRaw(dir + L"\\vscreen_auto_footprint.txt", "fraction=0.889105 eye=4032 distance=0.650 samples=188\n");
    check(!edvr::lastKnownPanelFootprint(dir, &r), "R11e: an older build's median file (Sean's: fraction=0.889105 eye=4032 distance=0.650 samples=188) reads as no record");
    {   // The launch then fits the seed (what the resolver does with no record), not the 3584 the median would have given.
        fit::Inputs in = seedInputs(4032, 0.7);
        if (edvr::lastKnownPanelFootprint(dir, &r)) { in.haveFootprint = true; in.fractionAtUnit = r.fractionAtUnit; }
        const fit::Decision d = fit::decide(in);
        check(d.source == fit::Source::Seed && d.width == 3504 && closeTo(d.footprintPx, 5006.0, 1e-6),
              "R11e: so that launch fits the seed (5006 px -> 3504 at distance 0.7), not the 3584 the old median would have given");
    }
    edvr::noteMeasuredPanelFootprint(dir, w);
    check(edvr::lastKnownPanelFootprint(dir, &r) && closeTo(r.fractionAtUnit, 0.6171, 1e-5), "R11e: and the first p10 a session stores replaces it");
    // Junk and implausible content are not a measurement.
    writeRaw(dir + L"\\vscreen_auto_footprint.txt", "this is not a record\n");
    check(!edvr::lastKnownPanelFootprint(dir, &r), "R11c: a garbled file is no measurement");
    writeRaw(dir + L"\\vscreen_auto_footprint.txt", "fraction=0.001 est=p10 eye=4032\n");
    check(!edvr::lastKnownPanelFootprint(dir, &r), "R11c: an implausible fraction in the file is no measurement");
    edvr::noteMeasuredPanelFootprint(dir, fit::Record{0.001, 4032, 1.0, 3});
    check(!edvr::lastKnownPanelFootprint(dir, &r) , "R11c: and the writer refuses to store one (the file keeps its last content, which is junk here)");
    check(!edvr::lastKnownPanelFootprint(L"", &r), "R11d: an empty log directory reads nothing");
    edvr::noteMeasuredPanelFootprint(L"", w);   // must not crash or create anything
    removeDir(dir);
}
#else
void caseR11() {}
#endif

// ---- R12: the wiring, as source pins -------------------------------------------------------------------------------------
void caseR12() {
    const std::string res = readFile("src\\d3d11\\vscreen_res.cpp");
    const std::string route = readFile("src\\d3d11\\vr_world_route.cpp");
    const std::string layer = readFile("src\\d3d11\\ui_layer.cpp");
    const std::string vs = readFile("src\\d3d11\\vscreen.cpp");
    const std::string fp = readFile("src\\d3d11\\vscreen_footprint.cpp");
    const std::string ini = readFile("edvr.ini");
    const std::string flatRuntime = readFile("src\\d3d11\\flat_runtime.cpp");
    const std::string authState = readFile("src\\common\\vscreen_auto_state.cpp");
    check(!res.empty() && !route.empty() && !layer.empty() && !vs.empty() && !fp.empty() && !ini.empty() && !authState.empty(),
          "R12a: the sources the pins read are readable from the repo root");
    if (g_failure.size()) return;

    // The resolver reads each of the route's conditions the way its owner does.
    check(has(res, "f.keyAuto = true;"), "R12b: the resolver states the on-foot world route as on (it always is)");
    for (const char* read : {"getString(\"fix.ui_quality\", \"100\")", "getString(\"fix.temporal_aa\", \"off\")"}) {
        checkf(has(res, read) && has(layer, read), "R12c: the resolver and uiLayerConfigure read the layer's key the same way: %s", read);
    }
    check(has(res, "temporalModeEnabled(") && has(layer, "temporalModeEnabled(") && has(res, "uiQualityParse(") && has(layer, "uiQualityParse(") &&
              has(res, "uiLayerNotLiveReasonFor(target, temporal, jitterAsShipped, /*stoodDown=*/false)"),
          "R12c: through the same parsers, and the layer's own words for why it is not live");
    check(has(res, "f.flatProfile = runtimeFlatProfile();") && has(res, "vscreenfit::decide(in)"),
          "R12e: the resolver asks the pure rule, with the flat profile as a fact");
    {   // A curved screen is not a condition of the route (it re-issues the game's own strip), so the resolver reads no curve fact.
        const std::string facts = functionBody(res, "vscreenfit::RouteFacts routeFactsFromConfig(Config& cfg) {");
        check(!facts.empty() && !has(facts, "panel_curvature") && !has(facts, "panelCurve") && !has(facts, ".curved") &&
                  !has(res, "#include \"panel_curve.h\""),
              "R12e: the resolver reads no curve fact: routeFactsFromConfig does not read the curvature and vscreen_res.cpp does not include panel_curve.h");
    }
    {
        const std::string resolve = functionBody(res, "void resolveVScreenTargetResolution(");
        check(has(resolve, "if (announce) {") && has(resolve, "formatRuleLine(") && before(resolve, "vscreenfit::decide(in)", "formatRuleLine("),
              "R12e: the `vScreen resolution:` line is the rule's own line and only the announcing call prints it");
        check(has(resolve, "lastKnownEyeWidth(") && before(resolve, "lastKnownEyeWidth(", "vscreenfit::decide(in)") && has(resolve, "lastKnownPanelFootprint("),
              "R12e: the rule starts from the eye width on record, with the stored footprint beside it");
    }

    // The instrument's hook in the draw path, and what arms it.
    {
        const std::string thunk = functionBody(vs, "void STDMETHODCALLTYPE hookedDrawIndexedInstanced(");
        check(count(vs, "footprintEyeDraw(") == 3 && count(thunk, "footprintEyeDraw(") == 1 &&
                  has(thunk, "if (eyeGeometry && vscreenFootprintWanted())") && before(thunk, "cameraCensusEyeDraw(self);", "footprintEyeDraw(self,"),
              "R12f: the instrument is called from the DrawIndexedInstanced thunk (for an eye draw, behind its one flag, after the game's own issue) and from the curved screen's swallowed draw (below), and nowhere else");
        check(has(thunk, "v == DrawVerdict::kPanel ? g_state->distanceScale : 1.0f"),
              "R12f: it is told the panel distance the draw's constants carry (the scaled one only when the override replaced the draw's cb0)");
        // The curved screen: the substitution swallows the game's draw (the tail above is never reached), so the instrument is called
        // from the swallowed path, on the owner context, for an eye draw, behind the same flag, and before the route's return.
        const std::string swallowed = functionBody(vs, "void curvedScreenSwallowed(");
        check(count(swallowed, "footprintEyeDraw(") == 1 && has(swallowed, "if (self != g_state->ownerCtx) return;") &&
                  has(swallowed, "if (g_state->rtv0Eye && count && instances && vscreenFootprintWanted())") &&
                  before(swallowed, "if (self != g_state->ownerCtx) return;", "footprintEyeDraw(self,") && before(swallowed, "footprintEyeDraw(self,", "if (!routeOwns) return;"),
              "R12f: the curved screen's swallowed draw calls the instrument once: owner context only, an eye draw, behind the same flag, whether or not the route owns the frame");
        check(has(swallowed, "v == DrawVerdict::kPanel ? g_state->distanceScale : 1.0f, args.base, args.startInstance"),
              "R12f: and tells it the panel distance the draw's constants carry, with the draw's own base vertex and start instance");
        const std::string gate = functionBody(vs, "bool drawGateSubscribed(State* s) {");
        check(has(gate, "vscreenFootprintWanted()"), "R12g: the draw gate lists the instrument (a subscriber that is not listed starves when nothing else is on)");
        check(count(vs, "tkVScreenFootprint.run(") == 1 && before(vs, "tkVrCameraCensus.run(", "tkVScreenFootprint.run(") && before(vs, "tkVScreenFootprint.run(", "tkScreenMotion.run(") &&
                  has(vs, "EDVR_BOUNDARY_TICK(tkVScreenFootprint, \"vscreen_footprint\");"),
              "R12g: its frame-boundary tick is declared and run once, after the route's and the census's and before screen motion's");
        check(has(vs, "vscreenFootprintShutdown();"), "R12g: its staging buffer is released at vScreen's shutdown");
        const std::string noteFlat = flatRuntime;
        check(!has(noteFlat, "vscreenFootprint") && !has(noteFlat, "vscreen_footprint"), "R12h: the flat runtime never touches the instrument");
        const std::string flatThunk = functionBody(vs, "void STDMETHODCALLTYPE hookedDrawIndexedInstanced(");
        check(before(flatThunk, "if (runtimeFlatProfile())", "footprintEyeDraw(self,"), "R12h: the call sits after the flat profile's early return in the thunk");
    }

    // The instrument itself.
    {
        check(has(fp, "D3D11_MAP_FLAG_DO_NOT_WAIT") && count(fp, "->Map(") == 1, "R12i: the instrument maps with DO_NOT_WAIT and in exactly one place (the render thread never waits on the GPU)");
        const std::string draw = functionBody(fp, "void vscreenFootprintCompositeDraw(");
        const std::string boundary = functionBody(fp, "void vscreenFootprintFrameBoundary(");
        check(before(draw, "FlatComputeInternalScope internal;", "guardedBudget(") && before(boundary, "FlatComputeInternalScope internal;", "guardedBudget(g_budget, [&] { collect("),
              "R12j: the hooks' bypass scope is taken OUTSIDE the fault guard (a fault the guard catches does not unwind the frame that holds a scope)");
        check(!has(functionBody(fp, "bool issueCopies("), "FlatComputeInternalScope internal;") && !has(functionBody(fp, "void collect("), "FlatComputeInternalScope internal;"),
              "R12j: and not inside the guarded work");
        check(draw.find("new ") == std::string::npos && draw.find("std::vector") == std::string::npos && draw.find("std::string") == std::string::npos &&
                  draw.find("std::mutex") == std::string::npos && draw.find("Map(") == std::string::npos,
              "R12k: the per-draw path allocates nothing, takes no lock and never maps");
        check(has(functionBody(fp, "void refreshWanted()"), "runtimeVrProfile()") && has(functionBody(fp, "void refreshWanted()"), "keyTextIsAuto("),
              "R12l: the instrument arms only in the VR profile and only with fix.vscreen_res_width auto");
        check(has(fp, "kSkipNames") && has(fp, "skip(kSkipVb0)") && has(fp, "skip(kSkipCbSmall)"), "R12m: a source that is not what the measurement assumes skips the sample, counted by reason");
        check(count(fp, "noteMeasuredPanelFootprint(") == 1 && has(fp, "kMinPersistSamples"), "R12n: the on-foot p10 is stored in one place, behind a minimum sample count");
        check(has(fp, "s.session.percentile(vscreenfit::kFootprintQuantile, &frac1)") && !has(fp, "s.session.median("),
              "R12n: and what is stored is the session's p10 (vscreenfit::kFootprintQuantile), never its median");
        {   // What a window says it saved is what reached the file (the glue rig runs the same instrument against a destination it cannot replace).
            const std::string close = functionBody(fp, "void closeWindow(");
            check(has(close, "if (noteMeasuredPanelFootprint(cfg.logDir(), r, &saveError)) {") && before(close, "if (noteMeasuredPanelFootprint(", "s.wroteOnce = true;") &&
                      before(close, "s.wroteOnce = true;", "} else {") && before(close, "} else {", "++s.saveFailed;"),
                  "R12r: the instrument asks the save whether it succeeded, and what it has written advances only inside the success branch");
            check(count(close, "s.wroteOnce = true;") == 1 && count(close, "s.lastWrittenFrac1 = frac1;") == 1 && !has(close, "w.persisted = true;") &&
                      has(close, "w.persisted = s.wroteOnce;") && has(close, "w.persistedFrac1 = s.lastWrittenFrac1;"),
                  "R12r: the state advances in that one place, and persisted= is only what has reached the file (never true by default)");
            check(has(close, "!s.wroteOnce || std::fabs(frac1 - s.lastWrittenFrac1) > kPersistDelta * s.lastWrittenFrac1"),
                  "R12r: a save is tried while nothing has reached the file and whenever the p10 has moved from what did, so a failed one is tried again at the next window");
            check(has(close, "++s.saveFailed;") && has(close, "if (s.saveFailed <= vscreenfit::kSaveFailedLineCap) {") &&
                      has(close, "vscreenfit::formatSaveFailedLine(failedLine, sizeof(failedLine), saveError, s.saveFailed);") && has(close, "w.saveFailed = s.saveFailed;"),
                  "R12r: a failed save is counted, said in the log (the first kSaveFailedLineCap of a session) and carried on the window line");
        }
        {   // The writer: a temp file beside the file, checked, flushed, then moved over it; a failure leaves nothing behind.
            const std::string writer = functionBody(authState, "bool noteMeasuredPanelFootprint(");
            check(!writer.empty() && has(writer, "CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS") && !has(writer, "CreateFileW(dest.c_str()") &&
                      has(writer, "const std::wstring tmp = dest + kAutoFootprintTempSuffix;") && has(authState, "kAutoFootprintTempSuffix[] = L\".tmp\";"),
                  "R12s: the writer writes a temp file beside the destination (its name plus .tmp), never the destination itself");
            check(before(writer, "WriteFile(f, text, static_cast<DWORD>(n), &written, nullptr)", "FlushFileBuffers(f)") &&
                      before(writer, "FlushFileBuffers(f)", "MoveFileExW(tmp.c_str(), dest.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)"),
                  "R12s: it writes, flushes, then replaces the destination write-through, in that order");
            check(has(writer, "written != static_cast<DWORD>(n)"), "R12s: the byte count is checked against the formatted length (a short write is a failed save)");
            check(has(writer, "DeleteFileW(tmp.c_str());") && before(writer, "MoveFileExW(", "DeleteFileW(tmp.c_str());"), "R12s: a failed save deletes its temp file");
        }
        const std::string state = functionBody(fp, "struct State {");
        check(!state.empty() && state.find("ComPtr<") == std::string::npos && has(state, "ID3D11Buffer* staging = nullptr;") && has(fp, "g.staging->Release();"),
              "R12p: the static state holds the staging buffer as a raw pointer released by hand (a COM smart pointer in a static would Release at DLL detach)");
        const std::string boundary2 = functionBody(fp, "void vscreenFootprintFrameBoundary(");
        check(before(boundary2, "if (!runtimeVrProfile()) {", "++s.frame;") && before(boundary2, "if (!runtimeVrProfile()) {", "refreshWanted();"),
              "R12q: the frame boundary returns first thing in any profile but VR (the flat profile reads and writes nothing, not even a config key)");
    }

    // The ini documents the rule, and still says it needs a restart.
    {
        const size_t at = ini.find("\nvscreen_res_width = auto");
        const size_t from = at == std::string::npos ? at : ini.rfind("\n\n", at);
        const std::string block = (at == std::string::npos || from == std::string::npos) ? std::string() : ini.substr(from, at - from);
        check(!block.empty() && has(block, "world route") && has(block, "fitted") && has(block, "125%") && has(block, "Needs a game restart") &&
                  has(block, "fix.panel_curvature, does not stop it") && !has(block, "fix.panel_curvature at 0"),
              "R12o: edvr.ini's text for the key names both rules, the route's conditions that matter to a user (a curved screen does not stop it), and the restart");
        std::string flat;   // the block as running prose: each line break and its comment marker become one space
        for (size_t i = 0; i < block.size(); ++i) {
            if (block[i] != '\n') { flat += block[i]; continue; }
            flat += ' ';
            while (i + 1 < block.size() && (block[i + 1] == '#' || block[i + 1] == ' ')) ++i;
        }
        check(!block.empty() && has(flat, "about 70% of the width the screen has in your eyes head-on") && has(flat, "remembers its head-on floor") &&
                  has(flat, "3504 wide at panel_distance 0.7 on a 4032 wide eye") && !has(flat, "the width the screen really has in your eyes"),
              "R12o: and says what is true of the fitted width: about 70% of the screen's head-on width in the eyes (the 3504 at 0.7 on a 4032 eye is the calibration), never that it IS that width");
    }
}

// ---- R13: Elite's Supersampling below 1.0 in VR ---------------------------------------------------------------------------
void caseR13() {
    // The judgement: the world rendered under 98% of the eye's width AND height. The eye is 2816x3072 here (a Pimax-like eye).
    check(vrss::kBelowPercent == 98, "R13a: the threshold is 98 percent (Elite's Supersampling steps by 0.05; two percent absorb a rounded size)");
    check(vrss::below(2112, 2304, 2816, 3072) && vrss::below(2675, 2918, 2816, 3072) && vrss::below(1408, 1536, 2816, 3072),
          "R13a: 75%, 95% and 50% of the eye are below 1");
    check(!vrss::below(2816, 3072, 2816, 3072) && !vrss::below(2760, 3011, 2816, 3072) && !vrss::below(3520, 3840, 2816, 3072),
          "R13a: the eye's own size, 98% of it and a larger render (Supersampling above 1) are not");
    check(!vrss::below(2112, 3072, 2816, 3072) && !vrss::below(2816, 2304, 2816, 3072),
          "R13a: one axis under and the other not is no uniform Supersampling: not below 1");
    check(!vrss::below(0, 0, 0, 0) && !vrss::below(2112, 2304, 0, 0) && !vrss::below(0, 0, 2816, 3072) && !vrss::below(2112, 2304, 2816, 0),
          "R13a: an unknown size is never below 1 -- a flat session has no eye texture, so none of this can say yes to it");
    check(vrss::percentOfEye(2112, 2816) == 75 && vrss::percentOfEye(2675, 2816) == 95 && vrss::percentOfEye(2816, 2816) == 100 &&
              vrss::percentOfEye(1409, 2816) == 50 && vrss::percentOfEye(0, 2816) == 0 && vrss::percentOfEye(2112, 0) == 0,
          "R13a: the world's share of the eye's width, rounded to a percent, 0 when unknown");
    // The published word: the four sizes, or nothing.
    {
        uint32_t a = 0, b = 0, c = 0, d = 0;
        const uint64_t word = vrss::pack(2112, 2304, 2816, 3072);
        check(word != 0 && vrss::unpack(word, &a, &b, &c, &d) && a == 2112 && b == 2304 && c == 2816 && d == 3072,
              "R13b: the four sizes pack into one word and read back");
        check(vrss::pack(2816, 3072, 2816, 3072) == 0 && vrss::pack(0, 0, 0, 0) == 0 && vrss::pack(70000, 1, 140000, 2) == 0 &&
                  !vrss::unpack(0, &a, &b, &c, &d),
              "R13b: a size that is not below 1, an unknown one or one past 65535 publishes nothing, and nothing unpacks to no");
    }
    // The words.
    {
        char log[1200], toast[96], hint[200];
        vrss::formatLog(log, sizeof(log), 2112, 2304, 2816, 3072);
        vrss::formatToast(toast, sizeof(toast));
        vrss::formatStatusHint(hint, sizeof(hint));
        const std::string l = log;
        check(std::strncmp(log, "vr supersampling: Elite draws the 3D world at 2112x2304, 75% of the 2816x3072 eye texture, and scales it up before EDVR sees it:", 128) == 0 &&
                  has(l, "Elite's Supersampling is below 1") && has(l, "HMD Image Quality") && has(l, "EDVR's DLSS upscales from that") &&
                  has(l, "Measured from the render sizes, not read from Elite's settings file") && l.size() < 1000,
              "R13c: the log line names the measurement, the cause, what it does to DLSS and the holograms, what to set, and that it is measured");
        check(has(l, "holograms") && has(l, "already upscaled") && has(l, "Supersampling to 1"), "R13c: ...and says why it hurts and what Supersampling should be");
        check(std::string(toast) == "Elite Supersampling is below 1: use HMD Image Quality" && std::strlen(toast) <= 60,
              "R13c: the headset toast is one short line that names both settings");
        // The Status page's hint replaces the 78-character hint the page has (two lines, whatever it says): never longer, so the
        // page never wraps to a third line, and it names both settings.
        check(std::string(hint) == "Elite Supersampling is below 1: set it to 1 and use HMD Image Quality." && std::strlen(hint) <= 78,
              "R13c: the Status page's hint is one sentence, no longer than the hint it replaces (78 characters)");
    }
    // The wiring, as source pins.
    {
        const std::string vs = readFile("src\\d3d11\\vscreen.cpp");
        const std::string menu = readFile("src\\d3d11\\menu.cpp");
        const std::string flat = readFile("src\\d3d11\\flat_runtime.cpp");
        const std::string header = readFile("src\\common\\vr_supersample_notice.h");
        const std::string vsh = readFile("src\\d3d11\\vscreen.h");
        check(!vs.empty() && !menu.empty() && !flat.empty() && !header.empty() && !vsh.empty(), "R13d: the sources the pins read are readable from the repo root");
        if (g_failure.size()) return;
        // Detection: vScreen's own measurement, at the render-size adoption, once; never Elite's settings file.
        const size_t adopt = vs.find("vScreen: the world on this rig is rendered at %ux%u and scaled into");
        const size_t pub = vs.find("g_renderBelowEye.store(vrss::pack(s->renderW, s->renderH, s->eyeW, s->eyeH), std::memory_order_release);");
        const size_t logAt = vs.find("vrss::formatLog(notice, sizeof(notice), s->renderW, s->renderH, s->eyeW, s->eyeH);");
        check(adopt != std::string::npos && pub != std::string::npos && logAt != std::string::npos && adopt < pub && pub < logAt &&
                  count(vs, "g_renderBelowEye.store(") == 1 && count(vs, "if (vrss::below(s->renderW, s->renderH, s->eyeW, s->eyeH)) {") == 1,
              "R13d: vScreen publishes and logs the notice once, from the measured render size against the eye's, right after it adopts the size");
        check(!has(vs, "SSAAMultiplier") && !has(vs, "fxcfg") && !has(header, "fxcfg") && !has(header, "Settings.xml") && !has(header, "SSAAMultiplier"),
              "R13d: nothing here reads Elite's settings file: the sizes are the whole evidence");
        check(has(vsh, "bool vScreenRenderBelowEye(uint32_t* renderW, uint32_t* renderH, uint32_t* eyeW, uint32_t* eyeH);") &&
                  has(vs, "bool vScreenRenderBelowEye(uint32_t* renderW, uint32_t* renderH, uint32_t* eyeW, uint32_t* eyeH) {"),
              "R13d: the accessor the menu reads is declared and defined once");
        // The menu: a toast once, the Status line, the settings pages' note; the VR branch only.
        const std::string tick = functionBody(menu, "void menuTick(ID3D11Device* dev) {");
        const size_t flatReturn = tick.find("if (runtimeFlatProfile()) {");
        const size_t toast = tick.find("if (!s.vrSupersamplingToasted) {");
        const size_t vrBranch = tick.find("guardedBudget(g_budget, [&] {\n        static uint64_t lastNativeRevision = 0;");
        check(flatReturn != std::string::npos && toast != std::string::npos && vrBranch != std::string::npos && flatReturn < vrBranch && vrBranch < toast &&
                  count(menu, "vrss::formatToast(") == 1 && count(menu, "s.toastQueue.push_back(toast);") == 1 &&
                  has(tick, "s.vrSupersamplingToasted = true;\n                if (s.toasts) {\n                    char toast[96];") &&
                  has(tick, "vr supersampling: the headset notice is queued as a toast") && has(tick, "vr supersampling: menu.toasts is off, so no toast"),
              "R13e: the toast is queued once a session, gated on menu.toasts and logged either way, after the flat profile's branch returned: flat never reaches it");
        // The open menu says it in the Status page's hint, and nowhere else: no line is added to any page (the bitmap is refused above
        // 2048 px, which a 48-px cap reaches with a note under a settings page's rows and a 54-px cap with one more Status line; the
        // Pimax has 51-55). The accessor is read exactly twice in menu.cpp, the hint and the toast.
        const std::string build = functionBody(menu, "void buildContent(MenuContent& c) {");
        const size_t statusAt = build.find("buildStatus(c);");
        const size_t compactAt = build.find("c.compact = true;", statusAt == std::string::npos ? 0 : statusAt);
        const size_t hintAt = build.find("if (vScreenRenderBelowEye(&rw, &rh, &ew, &eh)) vrss::formatStatusHint(c.hint, sizeof(c.hint));");
        check(statusAt != std::string::npos && compactAt != std::string::npos && hintAt != std::string::npos && compactAt < hintAt &&
                  count(menu, "vrss::formatStatusHint(c.hint, sizeof(c.hint));") == 1,
              "R13e: the Status page's hint says it, after the page's own hint is set, only when vScreen measured it");
        check(count(menu, "vScreenRenderBelowEye(") == 2 && !has(menu, "formatNote") && !has(menu, "vrss::formatStatus(") &&
                  !has(menu, "statusLine(c, \"Elite supersampling\""),
              "R13e: nothing adds a line to a page for the notice: the accessor is read twice (the hint and the toast), no note, no Status line");
        check(!has(flat, "vrss::") && !has(flat, "vScreenRenderBelowEye") && !has(flat, "vr_supersample_notice") && !has(flat, "Supersampling is below 1"),
              "R13f: the flat runtime never touches the notice");
        // The F8 panel's words: the flat warning no longer carries a supersampling paragraph at all (section 83).
        const std::string settings = readFile("src\\d3d11\\flat_elite_settings.h");
        check(!settings.empty() && !has(settings, "Supersampling is below 1.0") && !has(settings, "kFlatSupersamplingWords"),
              "R13f: the flat warning's own supersampling advice is gone (below 1.0 is supported now), and nothing flat says the VR words");
    }
}

struct Case { const char* id; void (*run)(); };
const Case kCases[] = {{"R1", caseR1}, {"R2", caseR2}, {"R3", caseR3}, {"R4", caseR4}, {"R5", caseR5},  {"R6", caseR6},
                       {"R7", caseR7}, {"R8", caseR8}, {"R9", caseR9}, {"R10", caseR10}, {"R11", caseR11}, {"R12", caseR12},
                       {"R13", caseR13}};

bool selected(const std::string& only, const char* id) {
    if (only.empty()) return true;
    const std::string wanted = "," + only + ",";
    return wanted.find(std::string(",") + id + ",") != std::string::npos;
}

}  // namespace

int main(int argc, char** argv) {
    std::string only;
    bool selfTest = false, dry = false;
    for (int i = 1; i < argc; ++i)
        if (!std::strcmp(argv[i], "--dry-run")) dry = true;   // seen first, so it governs --write-fixture wherever it stands
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--write-fixture") && i + 1 < argc) {
            const std::string text = fixtureText();
            if (dry) {   // anything that writes a file takes --dry-run, and --dry-run writes nothing at all
                std::printf("vscreen_fit_test: --dry-run: would write %zu bytes to %s; wrote nothing\n", text.size(), argv[i + 1]);
                return 0;
            }
            std::ofstream out(argv[i + 1], std::ios::binary);
            out.write(text.data(), static_cast<std::streamsize>(text.size()));
            std::printf("vscreen_fit_test: wrote %zu bytes to %s\n", text.size(), argv[i + 1]);
            return out ? 0 : 1;
        }
        if (!std::strcmp(argv[i], "--self-test")) selfTest = true;
        else if (!std::strcmp(argv[i], "--dry-run")) dry = true;
        else if (!std::strcmp(argv[i], "--only") && i + 1 < argc) only = argv[++i];
        else if (!std::strcmp(argv[i], "--root") && i + 1 < argc) g_root = argv[++i];
        else if (argv[i][0] != '-') g_root = argv[i];
        else { std::fputs("usage: --self-test [<repo root>] [--only R1,R3,...] | --dry-run\n", stderr); return 2; }
    }
    if (dry) {
        std::puts("vscreen_fit_test: --dry-run: nothing run, nothing written");
        return 0;
    }
    if (!selfTest) {
        std::fputs("usage: --self-test [<repo root>] [--only R1,R3,...] | --dry-run\n", stderr);
        return 2;
    }
    if (!only.empty()) {
        for (const char* id = only.c_str(); *id;) {
            const char* comma = std::strchr(id, ',');
            const std::string one = comma ? std::string(id, comma) : std::string(id);
            bool known = false;
            for (const Case& c : kCases) known = known || one == c.id;
            if (!known) { std::fprintf(stderr, "FAIL: --only names a case that does not exist: %s\n", one.c_str()); return 1; }
            id = comma ? comma + 1 : id + one.size();
        }
    }
    std::string firstFailure;
    unsigned ran = 0;
    for (const Case& c : kCases) {
        if (!selected(only, c.id)) continue;
        g_failure.clear();
        c.run();
        ++ran;
        std::printf("case %s: %s\n", c.id, g_failure.empty() ? "ok" : "FAILED");
        if (!g_failure.empty() && firstFailure.empty()) firstFailure = g_failure;
    }
    if (!ran) { std::fputs("FAIL: --only selected no case\n", stderr); return 1; }
    if (!firstFailure.empty()) {
        std::fprintf(stderr, "FAIL: %s\n", firstFailure.c_str());
        return 1;
    }
    std::printf("vscreen_fit_test: PASS (%u checks over %u cases)\n", g_checks, ran);
    return 0;
}
