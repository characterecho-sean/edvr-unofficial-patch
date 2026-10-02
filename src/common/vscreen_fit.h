// fix.vscreen_res_width = auto, fitted to what each eye actually shows (docs/design-flat-temporal-aa-2026-09-23.md,
// section 82, the "vscreen auto-fit" entry of 2026-10-01).
//
// THE PURE HALF. Everything that DECIDES is here: no D3D, no Config, no file I/O, no log. The DLL's resolver
// (src/d3d11/vscreen_res.cpp), the footprint instrument (src/d3d11/vscreen_footprint.cpp) and the state file
// (vscreen_auto_state.cpp) all call the code below, and tools\vscreen_fit_test runs the very same code, so the rig
// pins what flies. tools\vscreen_fit_test\mutants.py breaks each rule of it on purpose and requires the rig to notice.
//
// WHY. On foot Elite draws the world once, flat, into the 2D screen's target and shows it to each eye as a panel. That
// target's size is fix.vscreen_res_width: 5040x2835 on a 4032 px eye (125% of the eye, the legacy rule). The 125%
// oversample exists because nothing anti-aliases the on-foot world before it reaches the panel. With the VR world route
// running (experimental.temporal_aa_on_foot_world = auto) the world is RESOLVED at the screen's size and shown to the eye
// through mips, so a width past what the eye can show only costs: the game renders every on-foot pixel of it (G-buffer,
// depth, HDR, HUD) and the route resolves and mips it. Sean flew 4032 ("looks fine") and 3504 ("looks great still")
// with the route on. So when the route will run, the width is a share of the screen's own footprint in eye pixels, A: the
// screen's texture is allowed to be coarser than the eye by the factor 1/m, because the route anti-aliases it.
//
// THE RULE.
//   route will run  ->  width = roundTo16(clamp(m x A, floor, cap)), 16:9, nudged off any size another target has
//   otherwise       ->  today's rule, unchanged: roundTo16(1.25 x the eye width the runtime last rendered)
// with m = 0.70 (kMultiplier: the width is 70% of the screen's head-on footprint, about 1.4 eye pixels per texel; the
// derivation is at the constants below), floor = 2880 (just over twice the stock detail, the width edvr.ini has always named
// as the useful minimum), cap = the legacy width (a fit never asks for MORE than the old rule did).
//
// A, the screen's width in eye pixels, is MEASURED by the footprint instrument at the composite draw: the four corners of
// the quad through the composite VS's own constants, horizontal extent in NDC / 2 = the fraction of the eye's width the
// screen spans. A is that fraction times the eye width. fix.panel_distance scales one float of the composite's
// placement (its z translation), so the fraction varies as 1/d: the state stores the fraction AT DISTANCE 1.0 and a
// different panel_distance rescales it without a new measurement. What is stored is the HEAD-ON FLOOR, the 10th percentile
// (kFootprintQuantile) of the session's on-foot widths, not their median: a head that is not square on to the screen only
// ever makes it look wider (about 6% at the median), so the low end is the pose-free number. With nothing measured yet the
// footprint is the calibration point (5006 px at distance 0.7 on a 4032 px eye), which makes the first launch on Sean's
// rig the 3504 he chose.
//
// THE ROUTE'S CONDITIONS at launch (all three, plus what the layer itself needs):
//   * experimental.temporal_aa_on_foot_world is auto (its default since 2026-10-01; the resolver's fallback in
//     vscreen_res.cpp, the route's in vr_world_route.cpp and the shipped edvr.ini say the same, and tools\config_test and
//     tools\vscreen_fit_test hold them to one answer)
//   * the UI layer is live: fix.ui_quality is not off, and a temporal mode is on (the layer composites at that pass's door)
//   * the runtime is EDVR's own OpenXR (not Elite's native Oculus back end, not a foreign openvr_api.dll)
// and the flat profile never fits (the world route is a VR route).
//
// A CURVED SCREEN (fix.panel_curvature above 0) IS NOT A CONDITION. The route re-issues a curved screen through the very strip the
// game's draw is substituted with (panel_curve.h panelCurveReissue), so it runs with the curve on and the fit applies to it. The
// footprint the rule uses is the game's own flat quad, which the instrument reads before the strip replaces it (vscreen.cpp): the
// width of the screen as if it were flat. That is exactly what m is defined at -- texels per eye pixel at the MIDDLE of the panel --
// because the bend keeps the middle point and the arc length (panel_curve.cpp bend(): x' = sin(theta)/k has unit slope at x = 0, and
// z' = (1 - cos(theta))/k, toward the viewer, is zero there with zero slope), so the middle density is the flat one at any curvature.
// What the rule does NOT address is the
// edges of a bent panel, which are denser than the middle when the panel is near (computed in the design doc, section 82, the curved
// route entry): they are magnified by the nearness the bend gives them.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace edvr {
namespace vscreenfit {

// ---- the constants of the rule (each pinned by tools\vscreen_fit_test) --------------------------------------------------
constexpr double kMultiplier = 0.70;         // m: the screen's texels per eye pixel at the middle of the panel (1/m = 1.43 eye pixels per texel)
constexpr double kLegacyMultiplier = 1.25;   // today's rule, and the fit's cap
constexpr uint32_t kFloorWidth = 2880;       // never fit below this (edvr.ini: "just over twice the stock detail")
constexpr uint32_t kMinWidth = 640;          // vscreen_res.cpp's own bounds
constexpr uint32_t kMaxWidth = 8192;
constexpr uint32_t kStep = 16;               // widths are multiples of 16, so width * 9 / 16 is an exact integer

// THE CALIBRATION. Sean, 2026-10-01: "for my current openxr resolution 3504 should = auto". His rig is a Pimax Crystal Super
// (4032 px per eye) at fix.panel_distance 0.7 with fix.panel_curvature 0.3, and on the first launch, with nothing stored,
// auto gives 3504x1971 there; the same arithmetic gives every other eye and distance.
//   kChosenWidthPx     the width he chose: 3504 (3504x1971), flown with the route on ("looks great still")
//   kSeedFootprintPx   what the screen spans head-on at that eye and distance, in eye pixels: 5006
//   kMultiplier        m = 3504 / 5000 = 0.7008, kept as 0.70, so that 0.70 x 5006 = 3504.2, which rounds to 3504
// Where 5000 and 5006 come from. Sean's two reader logs from that rig (Frontier 20261001_082459 and _100155), the 15 on-foot
// windows with 12 or more on-foot samples, each normalised to distance 0.7 by the 1/d law: the per-window MINIMA have a median
// of 4951 (range 4651..5175, one outlier below 4880) and the per-window MEDIANS a median of 5267 (range 5000..6135). A window
// of about 50 samples has its minimum near its 2nd percentile and its median at its 50th, and head pose only ever inflates
// the width, so the low end is the number with the head level. A window logs its median and range, not its samples, so the
// 10th percentile (what is stored) cannot be read back from a log; it is bracketed: a skew above the floor fitted to (min,
// median) gives p10 = 4991 (an exponential skew) and 5000 (half-normal), and 4985..5002 for 30 to 60 samples a window. The
// working value is 5000 px at distance 0.7, so m = 3504 / 5000 = 0.7008 -> 0.70, and the seed footprint is the width that
// makes 0.70 give the chosen 3504: 3504 / 0.70 = 5006 px. The unit fraction below is that footprint at distance 1.0 as a
// fraction of the eye: 5006 x 0.7 / 4032 = 0.86910. The first session's own p10 replaces the seed; tools\edvr_log.py
// --vscreen-fit says how far the two are apart (within 5% passes). (A seed left at the WIDTH, 3504, would fit 2453 and floor.)
constexpr double kChosenWidthPx = 3504.0;
constexpr double kSeedFootprintPx = 5006.0;
constexpr double kSeedDistance = 0.7;
constexpr double kSeedEyeWidthPx = 4032.0;
constexpr double kSeedFractionAtUnit = kSeedFootprintPx * kSeedDistance / kSeedEyeWidthPx;

// What is stored for the next launch: the 10th percentile of the session's on-foot widths (the head-on floor), tagged `est=p10` in
// the record so a file written when the median was stored (no tag) is never read as one.
constexpr double kFootprintQuantile = 0.10;

// What a stored or measured fraction may be: a screen narrower than a twentieth of the eye, or several eyes wide, is a
// misread, not a screen. The panel distance the rule will rescale by is bounded the same way (edvr.ini asks for 0.5..2.0).
constexpr double kMinFraction = 0.05;
constexpr double kMaxFraction = 3.0;
constexpr double kMinDistance = 0.25;
constexpr double kMaxDistance = 4.0;

// ---- sizes another target already has (flight logs 2026-09-23..30: the only 16:9 sizes the game's own targets take) -------
// The world-screen gate (ui_layer.cpp, depthProbeDrawsAtSize) and vScreen's panel recognition (srv0IsPanelSized) key on
// the panel's SIZE, so a panel of one of these sizes is mistaken for the other target or the other way round.
struct CollidingSize {
    uint32_t w, h;
    const char* what;
};
constexpr CollidingSize kCollidingSizes[] = {
    {1920, 1080, "the game's own panel size (asking for it turns the patch off)"},
    {3840, 2160, "the menu backdrop still and a per-eye post-pass target"},
};
constexpr size_t kCollidingSizeCount = sizeof(kCollidingSizes) / sizeof(kCollidingSizes[0]);

inline uint32_t roundTo16(double value) {
    if (!(value > 0.0) || value > 1.0e6) return 0;
    return static_cast<uint32_t>((value / static_cast<double>(kStep)) + 0.5) * kStep;
}
inline uint32_t heightFor(uint32_t w) { return (w * 9u + 8u) / 16u; }
inline uint32_t clampWidth(uint32_t w) { return w < kMinWidth ? kMinWidth : (w > kMaxWidth ? kMaxWidth : w); }
inline bool collides(uint32_t w) {
    const uint32_t h = heightFor(w);
    for (size_t i = 0; i < kCollidingSizeCount; ++i)
        if (w == kCollidingSizes[i].w && h == kCollidingSizes[i].h) return true;
    return false;
}
inline const char* collidesWhat(uint32_t w) {
    const uint32_t h = heightFor(w);
    for (size_t i = 0; i < kCollidingSizeCount; ++i)
        if (w == kCollidingSizes[i].w && h == kCollidingSizes[i].h) return kCollidingSizes[i].what;
    return "";
}

// The nearest multiple of 16 that collides with nothing, preferring the larger (more detail), within [lo, hi]; w itself
// when nothing does (a one-size band) and when w collides with nothing.
inline uint32_t nudgeOffCollisions(uint32_t w, uint32_t lo, uint32_t hi) {
    if (!collides(w)) return w;
    for (uint32_t k = 1; k <= 8; ++k) {
        const uint32_t up = w + k * kStep;
        if (up <= hi && !collides(up)) return up;
        if (w >= k * kStep) {
            const uint32_t down = w - k * kStep;
            if (down >= lo && !collides(down)) return down;
        }
    }
    return w;
}

inline double sanitizeDistance(double d) {
    if (!(d > 0.0)) return 1.0;   // NaN, zero or negative: the shipped distance
    return d < kMinDistance ? kMinDistance : (d > kMaxDistance ? kMaxDistance : d);
}
inline bool plausibleFraction(double f) { return f >= kMinFraction && f <= kMaxFraction && std::isfinite(f); }

// ---- the route's conditions ----------------------------------------------------------------------------------------------
// experimental.temporal_aa_on_foot_world reads "auto" exactly the way vr_world_route_math.h's vrWorldKeyFromText reads it:
// the text is "auto" in any case and nothing else (a typo is off, never auto). Spelled out here rather than included so the
// resolver's translation unit does not pull the route's header chain in; tools\vscreen_fit_test compares the two over a
// table of spellings, so a change to either one fails the build.
inline bool keyTextIsAuto(const char* text) {
    if (!text) return false;
    const char* a = "auto";
    for (; *a; ++a, ++text) {
        char c = *text;
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        if (c != *a) return false;
    }
    return *text == 0;
}

enum class RuntimeKind : uint8_t {
    NotLoadedYet,    // the module list has no VR runtime yet (the D3D11 device is made 1.2 s before openvr_api.dll): undecided
    EdvrOpenXr,      // EDVR's own OpenXR facade is loaded
    OculusNative,    // Elite's native Oculus back end (LibOVRRT loaded, no openvr_api.dll)
    ForeignOpenvr,   // an openvr_api.dll that is not EDVR's
};

struct RouteFacts {
    bool flatProfile = false;
    bool keyAuto = false;            // experimental.temporal_aa_on_foot_world reads "auto"
    const char* layerWhy = nullptr;  // nullptr: the UI layer would be live; else uiLayerNotLiveReasonFor's line
    RuntimeKind runtime = RuntimeKind::NotLoadedYet;
};

struct RouteVerdict {
    bool runs = false;
    char why[320] = {};   // every condition that fails, "; "-joined; empty when the route will run
};

inline void addWhy(RouteVerdict& v, const char* text) {
    const size_t used = std::strlen(v.why);
    const size_t n = std::strlen(text);
    const size_t sep = used ? 2 : 0;
    if (used + sep + n + 1 > sizeof(v.why)) return;
    if (sep) { v.why[used] = ';'; v.why[used + 1] = ' '; }
    std::memcpy(v.why + used + sep, text, n + 1);
}

inline RouteVerdict routeVerdict(const RouteFacts& f) {
    RouteVerdict v;
    if (f.flatProfile) addWhy(v, "this is the flat profile (the world route is a VR route)");
    if (!f.keyAuto) addWhy(v, "experimental.temporal_aa_on_foot_world is not auto");
    if (f.layerWhy && f.layerWhy[0]) addWhy(v, f.layerWhy);
    if (f.runtime == RuntimeKind::OculusNative)
        addWhy(v, "this session is on Elite's native Oculus back end (EDVR's OpenXR runtime is not driving it)");
    else if (f.runtime == RuntimeKind::ForeignOpenvr)
        addWhy(v, "the openvr_api.dll loaded is not EDVR's");
    v.runs = v.why[0] == 0;
    return v;
}

// ---- the decision ------------------------------------------------------------------------------------------------------
enum class Rule : uint8_t { Legacy, Fitted };
enum class Source : uint8_t { None, Seed, Measured };

struct Inputs {
    uint32_t eyeWidth = 0;           // the runtime's per-eye width last session (vscreen_auto_eye_width.txt); 0 = none
    double distance = 1.0;           // fix.panel_distance
    bool haveFootprint = false;      // a stored measurement
    double fractionAtUnit = 0.0;     // ... as a fraction of the eye width, at panel distance 1.0
    uint32_t footprintSamples = 0;   // ... and how many on-foot samples the session's p10 it holds was taken over
    RouteFacts route;
};

struct Decision {
    Rule rule = Rule::Legacy;
    Source source = Source::None;
    uint32_t width = 0, height = 0;
    uint32_t legacyWidth = 0;        // what the old rule gives for this eye, always
    double distance = 1.0;           // the (sanitised) distance the fit was made at
    double footprintPx = 0.0;        // A: the screen's head-on width in eye pixels at that distance (0 for legacy)
    double targetPx = 0.0;           // m x A, before the floor and the cap
    bool floored = false, capped = false;
    uint32_t nudgedFrom = 0;         // nonzero: the rounded width was this, another target's size, and moved
    RouteVerdict route;
};

inline Decision decide(const Inputs& in) {
    Decision d;
    d.route = routeVerdict(in.route);
    d.distance = sanitizeDistance(in.distance);
    // No eye width on record: nothing to size from (the caller leaves the game's own panel alone before it gets here).
    if (in.eyeWidth == 0) return d;
    d.legacyWidth = clampWidth(roundTo16(static_cast<double>(in.eyeWidth) * kLegacyMultiplier));
    if (!d.route.runs) {
        d.rule = Rule::Legacy;
        d.width = d.legacyWidth;
        d.height = heightFor(d.width);
        return d;
    }
    d.rule = Rule::Fitted;
    const bool measured = in.haveFootprint && plausibleFraction(in.fractionAtUnit);
    d.source = measured ? Source::Measured : Source::Seed;
    const double fraction = measured ? in.fractionAtUnit : kSeedFractionAtUnit;
    d.footprintPx = fraction * static_cast<double>(in.eyeWidth) / d.distance;
    d.targetPx = kMultiplier * d.footprintPx;
    const double hi = static_cast<double>(d.legacyWidth);
    const double lo = (std::min)(static_cast<double>(kFloorWidth), hi);
    double v = d.targetPx;
    if (v < lo) { v = lo; d.floored = true; }
    if (v > hi) { v = hi; d.capped = true; }
    uint32_t w = roundTo16(v);
    const uint32_t nudged = nudgeOffCollisions(w, static_cast<uint32_t>(lo), d.legacyWidth);
    if (nudged != w) { d.nudgedFrom = w; w = nudged; }
    d.width = clampWidth(w);
    d.height = heightFor(d.width);
    return d;
}

// ---- the footprint instrument's geometry ----------------------------------------------------------------------------------
// The composite's vertex shader (docs/shaders/composite-vs.asm), exactly:
//   r0.xy = POSITION.xy * SIZE.xy ; r0.z = POSITION.z ; r0.w = 1
//   X = dot(cb0[9], r0)  Y = dot(cb0[10], r0)  Z = dot(cb0[11], r0)
//   SV_POSITION = X * cb1[270] + Y * cb1[271] + Z * cb1[272] + cb1[273]
// So the four corners' NDC positions follow from the quad (four vertices, 20 byte stride: float3 position, float2 UV), the
// per-instance SIZE (float2), cb0 rows 9..11 (twelve floats) and cb1 rows 270..273 (sixteen floats: the clip matrix's
// COLUMNS).
struct Footprint {
    bool valid = false;
    const char* why = "";            // when not valid
    double x0 = 0, x1 = 0, y0 = 0, y1 = 0;   // NDC extents of the four corners
    double widthFraction() const { return (x1 - x0) * 0.5; }    // of the eye's width
    double heightFraction() const { return (y1 - y0) * 0.5; }   // of the eye's height
};

inline Footprint footprintOfQuad(const float model[12], const float clip[16], const float positions[4][3], const float size[2]) {
    Footprint f;
    double x0 = 1e300, x1 = -1e300, y0 = 1e300, y1 = -1e300;
    for (int i = 0; i < 4; ++i) {
        const double r[4] = {static_cast<double>(positions[i][0]) * size[0], static_cast<double>(positions[i][1]) * size[1],
                             static_cast<double>(positions[i][2]), 1.0};
        double world[3];
        for (int row = 0; row < 3; ++row) {
            world[row] = 0.0;
            for (int k = 0; k < 4; ++k) world[row] += static_cast<double>(model[row * 4 + k]) * r[k];
        }
        double c[4];
        for (int k = 0; k < 4; ++k)
            c[k] = world[0] * clip[0 + k] + world[1] * clip[4 + k] + world[2] * clip[8 + k] + clip[12 + k];
        if (!std::isfinite(c[0]) || !std::isfinite(c[1]) || !std::isfinite(c[3])) { f.why = "not-finite"; return f; }
        if (c[3] <= 1.0e-9) { f.why = "behind-eye"; return f; }
        const double nx = c[0] / c[3], ny = c[1] / c[3];
        x0 = (std::min)(x0, nx); x1 = (std::max)(x1, nx);
        y0 = (std::min)(y0, ny); y1 = (std::max)(y1, ny);
    }
    if (!(x1 - x0 > 1.0e-6) || !(y1 - y0 > 1.0e-6)) { f.why = "degenerate"; return f; }
    f.valid = true;
    f.x0 = x0; f.x1 = x1; f.y0 = y0; f.y1 = y1;
    return f;
}

// ---- the session's samples -------------------------------------------------------------------------------------------------
// A bounded store of footprint fractions whose median and percentiles survive a long session: when it fills, every second
// sample is dropped and only every second new one is kept after that, so what it holds is an even thinning of the whole session.
class FractionStore {
public:
    static constexpr uint32_t kCap = 2048;
    void clear() { n_ = 0; stride_ = 1; seen_ = 0; total_ = 0; }
    void add(double x) {
        ++total_;
        if ((seen_++ % stride_) != 0) return;
        if (n_ == kCap) {
            for (uint32_t i = 0; i < kCap / 2; ++i) v_[i] = v_[i * 2];
            n_ = kCap / 2;
            stride_ *= 2;
            if ((seen_ - 1) % stride_ != 0) return;
        }
        v_[n_++] = static_cast<float>(x);
    }
    uint32_t kept() const { return n_; }
    uint64_t total() const { return total_; }
    bool median(double* out) const {
        if (!n_) return false;
        float tmp[kCap];
        std::memcpy(tmp, v_, n_ * sizeof(float));
        const uint32_t mid = n_ / 2;
        std::nth_element(tmp, tmp + mid, tmp + n_);
        double m = tmp[mid];
        if ((n_ & 1u) == 0) {
            const float lower = *std::max_element(tmp, tmp + mid);
            m = (m + lower) * 0.5;
        }
        if (out) *out = m;
        return true;
    }
    // The q-quantile, q in [0, 1] (a NaN or an out-of-range q is the nearest end): linear interpolation between the order
    // statistics at position q x (n - 1), numpy's default, so a reader can reproduce it from the samples. False, and *out left
    // alone, for an empty store. percentile(0.5) is the median; percentile(0.10) is what a session stores (kFootprintQuantile).
    bool percentile(double q, double* out) const {
        if (!n_) return false;
        if (!(q > 0.0)) q = 0.0;
        else if (q > 1.0) q = 1.0;
        float tmp[kCap];
        std::memcpy(tmp, v_, n_ * sizeof(float));
        const double pos = q * static_cast<double>(n_ - 1);
        const uint32_t lo = static_cast<uint32_t>(pos);
        const uint32_t hi = lo + 1 < n_ ? lo + 1 : lo;
        std::nth_element(tmp, tmp + lo, tmp + n_);
        const double a = tmp[lo];
        // After nth_element everything past `lo` is at least tmp[lo], so the next order statistic is the smallest of the rest.
        const double b = hi == lo ? a : static_cast<double>(*std::min_element(tmp + hi, tmp + n_));
        if (out) *out = a + (b - a) * (pos - static_cast<double>(lo));
        return true;
    }
    bool range(double* lo, double* hi) const {
        if (!n_) return false;
        const float* a = std::min_element(v_, v_ + n_);
        const float* b = std::max_element(v_, v_ + n_);
        if (lo) *lo = *a;
        if (hi) *hi = *b;
        return true;
    }

private:
    float v_[kCap] = {};
    uint32_t n_ = 0, stride_ = 1;
    uint64_t seen_ = 0, total_ = 0;
};

// ---- the stored measurement (vscreen_auto_footprint.txt, beside vscreen_auto_eye_width.txt) ----------------------------
// One line of key=value text, `fraction=0.869097 est=p10 eye=4032 distance=0.700 samples=188`. The `est=p10` tag says WHICH
// estimate the fraction is: a build that stored the session's median wrote the same line without it, and that number is about
// 6% above the head-on floor this one stores, so parseRecord refuses a record that does not carry the tag (the seed applies
// until a session measures a p10).
struct Record {
    double fractionAtUnit = 0.0;   // the footprint (p10) as a fraction of the eye width, at panel distance 1.0
    uint32_t eyeWidth = 0;         // the eye width it was measured against (informational: the fraction does not depend on it)
    double distance = 1.0;         // the panel distance of the draws it was measured at (informational)
    uint32_t samples = 0;          // the on-foot samples its p10 was taken over
};

inline int formatRecord(char* out, size_t size, const Record& r) {
    return std::snprintf(out, size, "fraction=%.6f est=p10 eye=%u distance=%.3f samples=%u\n", r.fractionAtUnit, r.eyeWidth, r.distance,
                         r.samples);
}

inline bool parseRecord(const char* text, Record* out) {
    if (!text) return false;
    Record r;
    bool haveFraction = false;
    bool haveEstimator = false;    // the last `est=` token says exactly p10
    for (const char* p = text; *p;) {
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') ++p;
        if (!*p) break;
        const char* eq = std::strchr(p, '=');
        const char* end = p;
        while (*end && *end != ' ' && *end != '\t' && *end != '\r' && *end != '\n') ++end;
        if (!eq || eq > end) { p = end; continue; }
        const size_t keyLen = static_cast<size_t>(eq - p);
        char* tail = nullptr;
        if (keyLen == 8 && std::strncmp(p, "fraction", 8) == 0) {
            r.fractionAtUnit = std::strtod(eq + 1, &tail);
            haveFraction = tail != eq + 1;
        } else if (keyLen == 3 && std::strncmp(p, "est", 3) == 0) {
            haveEstimator = (end - (eq + 1)) == 3 && std::strncmp(eq + 1, "p10", 3) == 0;
        } else if (keyLen == 3 && std::strncmp(p, "eye", 3) == 0) {
            r.eyeWidth = static_cast<uint32_t>(std::strtoul(eq + 1, &tail, 10));
        } else if (keyLen == 8 && std::strncmp(p, "distance", 8) == 0) {
            r.distance = std::strtod(eq + 1, &tail);
        } else if (keyLen == 7 && std::strncmp(p, "samples", 7) == 0) {
            r.samples = static_cast<uint32_t>(std::strtoul(eq + 1, &tail, 10));
        }
        p = end;
    }
    if (!haveFraction || !haveEstimator || !plausibleFraction(r.fractionAtUnit)) return false;
    if (out) *out = r;
    return true;
}

// ---- the log's own text (the reader, tools\edvr_log.py --vscreen-fit, parses exactly this; the rig runs both) -------------
inline const char* ruleName(Rule r) { return r == Rule::Fitted ? "fitted" : "legacy"; }
inline const char* sourceName(Source s) { return s == Source::Measured ? "measured" : (s == Source::Seed ? "seed" : "none"); }

// The `vScreen resolution:` line for fix.vscreen_res_width = auto: the size, the rule that chose it and why.
inline int formatRuleLine(char* out, size_t size, const Decision& d, uint32_t eyeWidth) {
    char head[256];
    std::snprintf(head, sizeof(head), "vScreen resolution: auto = %u wide: rule=%s source=%s route=%s eye=%u distance=%.3f legacy=%u", d.width,
                  ruleName(d.rule), sourceName(d.source), d.route.runs ? "run" : "no", eyeWidth, d.distance, d.legacyWidth);
    if (d.rule == Rule::Fitted) {
        char source[320];
        if (d.source == Source::Measured)
            std::snprintf(source, sizeof(source),
                          "The footprint is the head-on floor (p10) of the on-foot widths a previous session measured (vscreen footprint lines), "
                          "rescaled to this distance.");
        else
            std::snprintf(source, sizeof(source),
                          "Nothing has been measured on this install yet, so the footprint is the calibration point (%.0f px head-on at distance "
                          "%.1f on a %.0f px eye, which fits %.0f wide) scaled to this eye and distance; this session's vscreen footprint lines "
                          "measure it.",
                          kSeedFootprintPx, kSeedDistance, kSeedEyeWidthPx, kChosenWidthPx);
        char tail[800];
        std::snprintf(tail, sizeof(tail),
                      " footprint=%.0f m=%.3f floor=%u cap=%u clamp=%s%s -- FITTED: the on-foot screen spans about %.0f px of the %u px each eye "
                      "shows, head-on at fix.panel_distance %.3f, so the width is %.3f x that (rounded to a multiple of 16, never under %u or over "
                      "the legacy %u), about %.2f eye pixels per texel. The world route will run, so the 125%% oversample the legacy rule adds is "
                      "not needed. %s",
                      d.footprintPx, kMultiplier, kFloorWidth, d.legacyWidth,
                      d.floored ? "floor" : (d.capped ? "cap" : "none"), d.nudgedFrom ? " nudged=yes" : "", d.footprintPx, eyeWidth,
                      d.distance, kMultiplier, kFloorWidth, d.legacyWidth, 1.0 / kMultiplier, source);
        const int n = std::snprintf(out, size, "%s%s", head, tail);
        if (d.nudgedFrom && n > 0 && static_cast<size_t>(n) + 200 < size)
            return n + std::snprintf(out + n, size - static_cast<size_t>(n),
                                     " (%u would have been %ux%u, %s.)", d.nudgedFrom, d.nudgedFrom, heightFor(d.nudgedFrom),
                                     collidesWhat(d.nudgedFrom));
        return n;
    }
    return std::snprintf(out, size,
                         "%s m=%.2f -- LEGACY: 125%% of the %u px the runtime last rendered per eye, because the world route will not run: %s. "
                         "Without the route nothing anti-aliases the on-foot world before it reaches the panel, and the extra width does that job.",
                         head, kLegacyMultiplier, eyeWidth, d.route.why[0] ? d.route.why : "no eye width is on record");
}

// The in-headset menu's hint for the row (a 200 byte buffer): what auto resolves to and under which rule.
inline int formatHint(char* out, size_t size, const Decision& d) {
    if (d.rule == Rule::Fitted)
        return std::snprintf(out, size, "Currently resolves to %ux%u: fitted to about %.0f%% of the on-foot screen's width in your view%s.", d.width,
                             d.height, kMultiplier * 100.0,
                             d.source == Source::Measured ? "" : " (an estimate until a session on foot has measured it)");
    return std::snprintf(out, size, "Currently resolves to %ux%u: 125%% of the eye width (the world route will not run).", d.width, d.height);
}

// ---- the footprint instrument's lines ------------------------------------------------------------------------------------
// The once-per-change lines (vscreen_footprint.cpp prints them; the reader tells armed from not armed by their first words).
inline int formatArmedLine(char* out, size_t size) {
    return std::snprintf(out, size,
                         "vscreen footprint: armed -- fix.vscreen_res_width is auto. Sampling the 2D screen's composite (vs 5C36AF05 ps CFE84157) about "
                         "twice a second: the quad's four corners through the composite's own constants (cb0 rows 9..11, cb1 rows 270..273, the quad and "
                         "its SIZE), a width in eye pixels. Every 30 s: a `vscreen footprint 30s:` line; the on-foot head-on floor (p10) is stored "
                         "beside the eye width, and the next launch's auto width fits it when the world route will run (the `vScreen resolution:` "
                         "line says which rule).");
}
inline int formatNotArmedLine(char* out, size_t size, const char* width) {
    return std::snprintf(out, size,
                         "vscreen footprint: not armed -- fix.vscreen_res_width is \"%s\", not auto: an explicit width is used exactly and there is "
                         "nothing to fit.",
                         width ? width : "");
}
inline int formatStoodDownLine(char* out, size_t size) {
    return std::snprintf(out, size,
                         "vscreen footprint: STOOD DOWN after faults in the instrument -- no more samples this session; the stored footprint, if any, "
                         "stands.");
}

// A save of the stored record that did not reach the file (vscreen_footprint.cpp prints this for the first kSaveFailedLineCap failures of a
// session and then stays quiet; every 30 s line from the first failure on carries save-failed=N, the count so far). Its first words are not
// the reader's arming words (armed / not armed / STOOD DOWN), so it never reads as an arming line. win32Error is what the writer reported
// (vscreen_auto_state.cpp: the Win32 error of the step that failed; ERROR_INVALID_DATA for a fraction it refuses to store).
constexpr uint32_t kSaveFailedLineCap = 3;
inline int formatSaveFailedLine(char* out, size_t size, uint32_t win32Error, uint32_t failedSoFar) {
    return std::snprintf(out, size,
                         "vscreen footprint: SAVE FAILED (Win32 error %u) -- the on-foot footprint did not reach vscreen_auto_footprint.txt: the file keeps its "
                         "previous value, and the next 30 s window tries again. Failed saves this session: %u; the 30 s lines carry save-failed=N from now on, "
                         "and persisted=no (or the last value that did reach the file) until a save succeeds.%s",
                         win32Error, failedSoFar, failedSoFar >= kSaveFailedLineCap ? " No more of these lines this session." : "");
}

struct WindowLine {
    uint32_t window = 0;
    bool eyeKnown = false;
    uint32_t eyeW = 0, eyeH = 0;
    uint32_t samples = 0, onFoot = 0, other = 0;
    uint32_t skipped = 0, late = 0, draws = 0;
    char skipWhy[96] = {};
    double distance = 1.0;           // fix.panel_distance as configured now
    double applied = 0.0;            // the distance the window's drawn constants carried (median); 0 = no sample
    // the on-foot samples of the window: median fraction of the eye (drawn), its range, the height fraction and pixel shape
    bool haveFoot = false;
    double footFrac = 0, footLo = 0, footHi = 0, footH = 0;
    bool haveOther = false;
    double otherFrac = 0;
    // the session's on-foot p10 (the head-on floor, kFootprintQuantile) at distance 1.0, how many samples, and what the file holds
    bool haveSession = false;
    double sessionFrac1 = 0;
    uint32_t sessionN = 0;
    bool persisted = false;          // something has reached the file this session (persistedFrac1 is the last value that did)
    double persistedFrac1 = 0;
    uint32_t saveFailed = 0;         // saves that failed so far this session; the token is printed only when this is above 0
    uint32_t fitWidth = 0, legacyWidth = 0;
};

inline int formatWindowLine(char* out, size_t size, const WindowLine& w) {
    char eye[40] = "-", foot[160] = "fp=-", other[48] = "", at1[64] = "at1=- frac1=-", sess[96] = "session-n=0 session-frac1=-";
    if (w.eyeKnown) std::snprintf(eye, sizeof(eye), "%ux%u", w.eyeW, w.eyeH);
    if (w.haveFoot) {
        const double px = w.eyeKnown ? w.footFrac * w.eyeW : 0.0;
        if (w.eyeKnown) {
            const double shape = (w.footH > 0 && w.eyeH) ? (w.footFrac * w.eyeW) / (w.footH * w.eyeH) : 0.0;
            std::snprintf(foot, sizeof(foot), "fp=%.0f frac=%.4f range=%.0f..%.0f h=%.4f shape=%.3f", px, w.footFrac, w.footLo * w.eyeW,
                          w.footHi * w.eyeW, w.footH, shape);
        } else {
            std::snprintf(foot, sizeof(foot), "fp=- frac=%.4f range=%.4f..%.4f h=%.4f shape=-", w.footFrac, w.footLo, w.footHi, w.footH);
        }
        const double d = w.applied > 0 ? w.applied : 1.0;
        const double f1 = w.footFrac * d;
        if (w.eyeKnown) std::snprintf(at1, sizeof(at1), "at1=%.0f frac1=%.4f", f1 * w.eyeW, f1);
        else std::snprintf(at1, sizeof(at1), "at1=- frac1=%.4f", f1);
    }
    if (w.haveOther) {
        if (w.eyeKnown) std::snprintf(other, sizeof(other), " other-fp=%.0f", w.otherFrac * w.eyeW);
        else std::snprintf(other, sizeof(other), " other-frac=%.4f", w.otherFrac);
    }
    if (w.haveSession) std::snprintf(sess, sizeof(sess), "session-n=%u session-frac1=%.4f", w.sessionN, w.sessionFrac1);
    char persisted[72] = "persisted=no";
    if (w.persisted) std::snprintf(persisted, sizeof(persisted), "persisted=%.4f", w.persistedFrac1);
    if (w.saveFailed) {   // only after a failure: a line with none is byte for byte what it was
        const size_t used = std::strlen(persisted);
        std::snprintf(persisted + used, sizeof(persisted) - used, " save-failed=%u", w.saveFailed);
    }
    char fit[96] = "fit=-";
    if (w.fitWidth) std::snprintf(fit, sizeof(fit), "fit=%u legacy=%u m=%.3f floor=%u", w.fitWidth, w.legacyWidth, kMultiplier, kFloorWidth);
    char why[110] = "";
    if (w.skipWhy[0]) std::snprintf(why, sizeof(why), " why=%s", w.skipWhy);
    char applied[24] = "-";
    if (w.applied > 0.0) std::snprintf(applied, sizeof(applied), "%.3f", w.applied);
    return std::snprintf(out, size,
                         "vscreen footprint 30s: window=%u samples=%u on-foot=%u other=%u skipped=%u late=%u%s draws=%u distance=%.3f applied=%s eye=%s %s%s %s %s %s %s",
                         w.window, w.samples, w.onFoot, w.other, w.skipped, w.late, why, w.draws, w.distance, applied, eye, foot, other, at1, sess,
                         persisted, fit);
}

}  // namespace vscreenfit
}  // namespace edvr
