// fix.vscreen_res_width = auto, fitted to what each eye shows: the rule, its inputs, its footprint arithmetic, its stored state,
// its log text (src/common/vscreen_fit.h, vscreen_auto_state.cpp, and the wiring in vscreen_res.cpp / vscreen.cpp /
// vscreen_footprint.cpp; docs/design-flat-temporal-aa-2026-09-23.md, section 82, the "vscreen auto-fit" entry).
//
// The pure half is the very code the DLL runs: this file includes src\common\vscreen_fit.h and calls it. Nothing here fakes a
// decision. tools\vscreen_fit_test\mutants.py breaks each rule of the header on purpose (and the wiring pins by editing copies
// of the sources they read) and requires the rule's own checks to fail.
//
// WHAT IS PINNED, by rule (a check's label starts with its rule's id: the mutation tool names the rule each edit must trip):
//   R1  the constants of the rule (m, floor, cap multiplier, step, Sean's calibration point) and today's rule, unchanged: the
//       legacy width and its 16:9 height, rounding half up
//   R2  the route's conditions: each one alone sends the width back to the legacy rule and says why; the rule does not consult the
//       curve (a curved screen is not a condition); a route that will not run gives EXACTLY the old answer for every distance and footprint
//   R3  the fit: the first launch on Sean's rig is the width he flew (3504x1971), a measurement replaces the seed, the footprint
//       rescales as 1/d, the floor and the cap, a small eye, never above the legacy width, always a multiple of 16 and 16:9
//   R4  sizes another target already has: never produced by a fit (nudged up, else down), the legacy rule untouched
//   R5  the footprint geometry: the four corners through the composite vertex shader's own arithmetic against a closed form,
//       and the refusals (behind the eye, not finite, degenerate)
//   R6  the distance law through the geometry: scaling the panel's z translation by d scales the footprint by 1/d
//   R7  the session's sample store: median, range, an even thinning that keeps the median across a long session
//   R8  the stored record: the codec round trip and what it refuses
//   R9  the log's text: the `vScreen resolution:` line names the rule and why, the menu hint fits its buffer, the 30 s line's
//       tokens (the reader, tools\edvr_log.py --vscreen-fit, parses exactly these)
//   R10 the route's key parse, pinned against the route's own (the resolver does not include the route's header chain)
//   R11 the state files on disk (vscreen_auto_state.cpp): the footprint's round trip beside the eye width's, in a temp directory
//   R12 the wiring, as source pins: the resolver's reads match their owners', the instrument's hook sits where the pins say and
//       stays off the flat profile, its D3D calls never wait and always step past the hooks, the resolver reads no curve fact
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
    check(fit::kMultiplier == 1.0, "R1a: m is 1.0 (the screen's texels per eye pixel at the middle of the panel)");
    check(fit::kFloorWidth == 2880, "R1a: the floor is 2880 (just over twice the stock detail)");
    check(fit::kLegacyMultiplier == 1.25, "R1a: today's rule is 125% of the eye width");
    check(fit::kStep == 16, "R1a: widths are multiples of 16");
    check(fit::kMinWidth == 640 && fit::kMaxWidth == 8192, "R1a: the width bounds are the resolver's own (640..8192)");
    check(fit::kSeedWidthPx == 3504.0 && fit::kSeedDistance == 0.7 && fit::kSeedEyeWidthPx == 4032.0,
          "R1a: the calibration point is Sean's flight: 3504 px at distance 0.7 on a 4032 px eye");
    check(closeTo(fit::kSeedFractionAtUnit, 3504.0 * 0.7 / 4032.0, 1e-12), "R1a: the seed fraction at distance 1.0 is 3504 x 0.7 / 4032");
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
        {"R2b: the key not auto: no route", [](fit::RouteFacts& f) { f.keyAuto = false; }, "experimental.temporal_aa_on_foot_world is not auto"},
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
        check(!v.runs && has(v.why, "flat profile") && has(v.why, "not auto") && has(v.why, "ui_quality is off") && has(v.why, "native Oculus") &&
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
    {   // Sean's rig, first launch: the width he flew.
        const fit::Decision d = fit::decide(seedInputs(4032, 0.7));
        check(d.rule == fit::Rule::Fitted && d.source == fit::Source::Seed, "R3a: the first launch with nothing measured is fitted, from the seed");
        checkf(d.width == 3504 && d.height == 1971, "R3a: the first launch on Sean's rig (Pimax, eye 4032, distance 0.7) is 3504x1971 (got %ux%u)", d.width, d.height);
        check(closeTo(d.footprintPx, 3504.0, 1e-6) && !d.floored && !d.capped && d.nudgedFrom == 0 && d.legacyWidth == 5040,
              "R3a: its footprint is 3504 px, unclamped, and the legacy width it replaces is 5040");
    }
    {   // A measured launch: the measurement replaces the seed, and 1/d rescales it.
        struct Row { double d, frac1; uint32_t eye; uint32_t width; bool floored, capped; };
        const Row rows[] = {
            {0.7, 3504.0 * 0.7 / 4032.0, 4032, 3504, false, false},
            {0.5, 0.6083, 4032, 4912, false, false},     // 4905.6 px -> 4912
            {1.0, 0.6083, 4032, 2880, true, false},      // 2452.8 px: under the floor
            {2.0, 0.6083, 4032, 2880, true, false},
            {0.4, 0.6083, 4032, 5040, false, true},      // 6132 px: over the cap
            {0.7, 0.6083, 3296, 2880, true, false},      // Quest 3: 2864 px, under the floor (cap 4128)
            {0.6, 0.7, 4032, 4704, false, false},        // 4704 px exactly
            {0.7, 0.8, 4032, 4608, false, false},        // 4608 px exactly (0.8 x 4032 / 0.7 = 4608)
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
        for (uint32_t eye : {1500u, 2016u, 2880u, 3296u, 4032u, 4508u, 5000u, 6400u})
            for (double d : {0.25, 0.5, 0.7, 0.85, 1.0, 1.4, 2.0, 4.0})
                for (double frac : {0.2, 0.45, 0.6083, 0.9, 1.4}) {
                    const fit::Decision r = fit::decide(measuredInputs(eye, d, frac));
                    const uint32_t legacy = refLegacy(eye);
                    const uint32_t lo = std::min<uint32_t>(2880u, legacy);
                    bounds = bounds && r.width <= legacy && r.width >= lo && r.width >= 640 && r.width <= 8192;
                    step = step && r.width % 16u == 0u;
                    shape = shape && r.height == refHeight(r.width) && r.height * 16u == r.width * 9u;
                    const double a = frac * eye / d;
                    if (!r.floored && !r.capped && r.nudgedFrom == 0) round = round && std::fabs(static_cast<double>(r.width) - a) <= 8.0 + 1e-9;
                }
        check(bounds, "R3e: the fitted width is never above the legacy width and never below min(floor, legacy)");
        check(step, "R3e: the fitted width is a multiple of 16");
        check(shape, "R3e: the fitted height is the exact 16:9 pair of the width");
        check(round, "R3e: an unclamped fit is m x A to within half a step: m is 1.0");
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
    {   // A footprint of exactly 3840 px: nudged up one step.
        const fit::Decision d = fit::decide(measuredInputs(4032, 0.7, 3840.0 * 0.7 / 4032.0));
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
}

// ---- R8: the stored record -----------------------------------------------------------------------------------------------
void caseR8() {
    fit::Record r;
    r.fractionAtUnit = 0.608333;
    r.eyeWidth = 4032;
    r.distance = 0.7;
    r.samples = 41;
    char text[200];
    const int n = fit::formatRecord(text, sizeof(text), r);
    check(n > 0 && static_cast<size_t>(n) < sizeof(text) && text[n - 1] == '\n', "R8a: the record is one line of key=value text");
    fit::Record back;
    check(fit::parseRecord(text, &back) && closeTo(back.fractionAtUnit, 0.608333, 1e-6) && back.eyeWidth == 4032 && closeTo(back.distance, 0.7, 1e-3) && back.samples == 41,
          "R8a: a record round-trips");
    check(fit::parseRecord("fraction=0.6083", &back) && closeTo(back.fractionAtUnit, 0.6083, 1e-9) && back.eyeWidth == 0, "R8b: only the fraction is required");
    check(fit::parseRecord("  eye=4032   fraction=0.7  junk samples=3 \n", &back) && closeTo(back.fractionAtUnit, 0.7, 1e-9) && back.samples == 3,
          "R8b: tokens may come in any order and unknown ones are ignored");
    check(!fit::parseRecord("", &back) && !fit::parseRecord(nullptr, &back) && !fit::parseRecord("eye=4032 samples=3", &back) && !fit::parseRecord("fraction=abc", &back),
          "R8c: nothing, no fraction and a non-number are refused");
    check(!fit::parseRecord("fraction=0.01", &back) && !fit::parseRecord("fraction=3.5", &back) && !fit::parseRecord("fraction=-0.6", &back) && !fit::parseRecord("fraction=nan", &back),
          "R8c: an implausible fraction (0.01, 3.5, negative, NaN) is refused");
    check(fit::parseRecord("fraction=0.05", &back) && fit::parseRecord("fraction=3.0", &back), "R8c: the bounds themselves are accepted");
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
    const double fracs[] = {3497.0, 3501.0, 3499.0};   // the on-foot medians of windows 2..4, in eye pixels
    const uint32_t onFoot[] = {60, 58, 59};
    {   // window 1: the main menu (the screen is on show, the commander is not on foot)
        fit::WindowLine w;
        w.window = 1; w.eyeKnown = true; w.eyeW = 4032; w.eyeH = 3898;
        w.samples = 44; w.onFoot = 0; w.other = 44; w.draws = 5280;
        w.distance = 0.7; w.applied = 0.0;
        w.haveOther = true; w.otherFrac = 3503.0 / 4032.0;
        w.fitWidth = 3504; w.legacyWidth = 5040;
        out.push_back(w);
    }
    uint32_t sessionN = 0;
    for (int i = 0; i < 3; ++i) {
        fit::WindowLine w;
        w.window = static_cast<uint32_t>(i + 2); w.eyeKnown = true; w.eyeW = 4032; w.eyeH = 3898;
        w.onFoot = onFoot[i]; w.samples = onFoot[i]; w.other = 0; w.draws = 5400;
        w.distance = 0.7; w.applied = 0.7;
        w.haveFoot = true;
        w.footFrac = fracs[i] / 4032.0; w.footLo = (fracs[i] - 8.0) / 4032.0; w.footHi = (fracs[i] + 8.0) / 4032.0;
        w.footH = (fracs[i] * 9.0 / 16.0) / 3898.0;
        sessionN += onFoot[i];
        w.haveSession = true; w.sessionFrac1 = w.footFrac * 0.7; w.sessionN = sessionN;
        w.persisted = true; w.persistedFrac1 = w.sessionFrac1;
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
        check(tok(t, "footprint") == "3504" && tok(t, "m") == "1.00" && tok(t, "floor") == "2880" && tok(t, "cap") == "5040" && tok(t, "clamp") == "none",
              "R9b: and the footprint, m, floor, cap and clamp the reader recomputes the width from");
        check(has(s, "FITTED") && has(s, "calibration point"), "R9c: the prose says it is fitted and, for the seed, that nothing was measured");
    }
    {   // fitted, measured, floored and nudged flags
        const fit::Decision d = fit::decide(measuredInputs(4032, 1.0, 0.6083));
        fit::formatRuleLine(line, sizeof(line), d, 4032);
        const auto t = tokensOf(line, "vScreen resolution: auto = 2880 wide: ");
        check(tok(t, "rule") == "fitted" && tok(t, "source") == "measured" && tok(t, "clamp") == "floor" && has(line, "previous session measured"),
              "R9b: a floored measured fit says clamp=floor and source=measured");
        const fit::Decision n = fit::decide(measuredInputs(4032, 0.7, 3840.0 * 0.7 / 4032.0));
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
        check(has(s, "LEGACY") && has(s, "experimental.temporal_aa_on_foot_world is not auto") && has(s, "fix.ui_quality is off") && !has(s, "panel_curvature"),
              "R9d: and the prose names EVERY route condition that failed (the curve is not one of them)");
    }
    {   // the hint
        char hint[200];
        const fit::Decision fitted = fit::decide(seedInputs(4032, 0.7));
        const int a = fit::formatHint(hint, sizeof(hint), fitted);
        check(a > 0 && a < 199 && has(hint, "3504x1971") && has(hint, "fitted") && has(hint, "estimate"), "R9e: the menu hint says fitted, the size, and that a seed is an estimate");
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
        w.footFrac = 3504.0 / 4032.0;
        w.footLo = 3498.0 / 4032.0;
        w.footHi = 3511.0 / 4032.0;
        w.footH = 1971.0 / 3898.0;
        w.haveOther = true;
        w.otherFrac = 0.8;
        w.haveSession = true;
        w.sessionFrac1 = 0.608333;
        w.sessionN = 41;
        w.persisted = true;
        w.persistedFrac1 = 0.608333;
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
        check(tok(t, "fp") == "3504" && tok(t, "frac") == "0.8690" && tok(t, "range") == "3498..3511" && tok(t, "other-fp") == "3226",
              "R9f: the on-foot footprint in eye pixels (median, range) and the other (menu) footprint");
        check(tok(t, "at1") == "2453" && tok(t, "frac1") == "0.6083" && tok(t, "session-n") == "41" && tok(t, "session-frac1") == "0.6083" && tok(t, "persisted") == "0.6083",
              "R9f: the footprint at distance 1, the session's median and what is stored");
        const double shape = std::atof(tok(t, "shape").c_str());
        check(closeTo(shape, 3504.0 / 1971.0, 0.002), "R9f: the shape (pixel aspect of the footprint) reads 1.778 for a 16:9 panel in square pixels");
        check(tok(t, "fit") == "3504" && tok(t, "legacy") == "5040" && tok(t, "m") == "1.00" && tok(t, "floor") == "2880", "R9f: and the width the next launch would fit");
        fit::WindowLine none;
        none.window = 1;
        none.distance = 1.0;
        fit::formatWindowLine(line, sizeof(line), none);
        const auto u = tokensOf(line, "vscreen footprint 30s: ");
        check(tok(u, "samples") == "0" && tok(u, "draws") == "0" && tok(u, "fp") == "-" && tok(u, "persisted") == "no" && tok(u, "fit") == "-" && tok(u, "eye") == "-",
              "R9g: an armed instrument that saw nothing still prints its line, with draws=0 and a dash for every measurement");
    }
    {   // the once-per-change lines
        char armed[800], notArmed[400], down[400];
        fit::formatArmedLine(armed, sizeof(armed));
        fit::formatNotArmedLine(notArmed, sizeof(notArmed), "3504");
        fit::formatStoodDownLine(down, sizeof(down));
        check(std::strncmp(armed, "vscreen footprint: armed -- ", 28) == 0 && std::strlen(armed) < 700 && has(armed, "vscreen footprint 30s:"),
              "R9i: the arming line starts `vscreen footprint: armed` (the reader's word) and says what the 30 s line is");
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

// ---- R10: the route's key parse, pinned ----------------------------------------------------------------------------------
// vr_world_route_math.h's vrWorldKeyFromText, verbatim (kept here so the rig needs none of the route's header chain).
bool routeKeyOriginal(const char* text) {
    if (!text) return false;
    const char* a = "auto";
    for (; *a; ++a, ++text) {
        char c = *text;
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        if (c != *a) return false;
    }
    return *text == 0;
}
void caseR10() {
    const char* spellings[] = {"auto", "AUTO", "Auto", "aUtO", "off", "", "on", "autox", "aut", "auto ", " auto", "1", "true", "dlss", "automatic"};
    bool same = true;
    for (const char* s : spellings) same = same && fit::keyTextIsAuto(s) == routeKeyOriginal(s);
    check(same && !fit::keyTextIsAuto(nullptr), "R10a: the resolver reads the route's key exactly as the route does, over a table of spellings");
    check(fit::keyTextIsAuto("auto") && fit::keyTextIsAuto("AUTO") && !fit::keyTextIsAuto("autox") && !fit::keyTextIsAuto("off") && !fit::keyTextIsAuto(""),
          "R10a: auto in any case and nothing else");
    const std::string route = readFile("src\\d3d11\\vr_world_route_math.h");
    if (route.empty()) { check(false, "R10b: vr_world_route_math.h is readable from the repo root"); return; }
    const std::string body = functionBody(route, "inline VrWorldKey vrWorldKeyFromText(const char* text) {");
    check(!body.empty() && has(body, "const char* a = \"auto\";") && has(body, "if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');") &&
              has(body, "if (c != *a) return VrWorldKey::Off;") && has(body, "return *text == 0 ? VrWorldKey::Auto : VrWorldKey::Off;"),
          "R10b: the route's own parse still has the body this rig copies (a change to it must change keyTextIsAuto too)");
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
    for (const wchar_t* f : {L"vscreen_auto_eye_width.txt", L"vscreen_auto_footprint.txt"}) DeleteFileW((dir + L"\\" + f).c_str());
    RemoveDirectoryW(dir.c_str());
}
void writeRaw(const std::wstring& path, const char* text) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD w = 0;
    WriteFile(f, text, static_cast<DWORD>(std::strlen(text)), &w, nullptr);
    CloseHandle(f);
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
    // Junk and implausible content are not a measurement.
    writeRaw(dir + L"\\vscreen_auto_footprint.txt", "this is not a record\n");
    check(!edvr::lastKnownPanelFootprint(dir, &r), "R11c: a garbled file is no measurement");
    writeRaw(dir + L"\\vscreen_auto_footprint.txt", "fraction=0.001 eye=4032\n");
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
    check(!res.empty() && !route.empty() && !layer.empty() && !vs.empty() && !fp.empty() && !ini.empty(), "R12a: the sources the pins read are readable from the repo root");
    if (g_failure.size()) return;

    // The resolver reads each of the route's conditions the way its owner does.
    check(has(res, "getString(\"experimental.temporal_aa_on_foot_world\", \"off\")") && has(route, "getString(\"experimental.temporal_aa_on_foot_world\", \"off\")"),
          "R12b: the resolver and the route read experimental.temporal_aa_on_foot_world with the same default");
    for (const char* read : {"getString(\"fix.ui_quality\", \"100\")", "getString(\"fix.temporal_aa\", \"off\")", "getString(\"advanced.temporal_aa_jitter_sign\", \"as_is\")",
                             "getFloat(\"advanced.temporal_aa_jitter_lag\", 0.0f)"}) {
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
        check(count(fp, "noteMeasuredPanelFootprint(") == 1 && has(fp, "kMinPersistSamples"), "R12n: the on-foot median is stored in one place, behind a minimum sample count");
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
