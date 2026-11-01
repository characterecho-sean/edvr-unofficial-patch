// F205 (native on-foot VR): the full-detail scope takes both eyes, and every ADS press shows its view inside a circle sized as flat's ADS FOV. The pure parts, header only, no game or D3D11
// state, so native_frame_test runs the very functions the frame thread uses. Journal: the arc doc, F205.
//
// 1. THE EFFECTIVE SIGHT EYE. fix.native_on_foot_ads_sight_eye is the player's choice (F162: both | right | left). The full-detail scope (B3) needs both: the left eye is drawn as the zoomed view
//    and the share shows the right eye's picture to both. So while B3 applies, a press behaves as both whatever the key says. It is decided ONCE, at the press's start (the sight lock's aiming eye
//    must not change in the middle of a press: a flip would jump the lock), kept through a re-engage from the release, and ended when the press has fully ended (the state is idle again): then
//    the player's setting is back. One function, effectiveSightEye, is what every reader of the choice asks (adsSightEyeMode in native_on_foot.cpp); nothing else reads the key.
//    B3 applies when its key is on, the scope zoom key is on, and the held item is B3's table entry (the scoped Executioner). With Anti-aliasing off B3's full detail does not run (its left
//    door needs the upscaler's picture: F202 left it so), but the press still takes both eyes and the circle: the override does not look at the temporal mode.
//
// 2. THE VIEW CIRCLE (view_circle.h): the radius is (ADS FOV / hip FOV) x the headset's vertical half-FOV; no circle, with the reason, for a weapon whose ADS FOV is not known, for a frustum
//    that is not an eye, for a sight whose far point is not known, and with the scope zoom off. F205b (the pilot's word): the circle has no key of its own; it is on whenever
//    fix.native_on_foot_ads_scope_zoom is, by the lens's own read.
#pragma once

#include "native_on_foot_f162.h"
#include "../common/view_circle.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace edvr {
namespace nofoot {
namespace f205 {

// ---- 1. the effective sight eye --------------------------------------------------------------------------------------------------------------------------------------------------
inline int effectiveSightEye(int keyMode, bool forceBoth) noexcept { return forceBoth ? f162::kBoth : keyMode; }

// B3 applies: its key on, the scope zoom key on, the held item B3's entry.
inline bool b3Applies(bool fullKeyOn, bool scopeZoomKeyOn, bool renderedEntry) noexcept { return fullKeyOn && scopeZoomKeyOn && renderedEntry; }

// The press latch (frame thread). begin() at an engage from idle decides; reengage() at an engage from the release keeps; end() at idle clears.
struct PressLatch {
    bool forced = false;        // the press takes both whatever the key says
    int keyAtStart = f162::kBoth;
    bool overrode = false;      // forced AND the key was not both: the press's line is due
    void begin(bool applies, int keyMode) noexcept {
        forced = applies;
        keyAtStart = keyMode;
        overrode = applies && keyMode != f162::kBoth;
    }
    void reengage() noexcept {}   // a press re-engaged from its own release keeps its latch: the lock is mid-ramp and must not change eye
    void end() noexcept {
        forced = false;
        overrode = false;
    }
};

// The press's one line when it overrides: "scope zoom (B3): the sight eye is both for this press (fix.native_on_foot_ads_sight_eye is right): full detail needs both eyes".
inline int formatOverride(char* buf, size_t n, int keyMode) noexcept {
    return std::snprintf(buf, n, "scope zoom (B3): the sight eye is both for this press (fix.native_on_foot_ads_sight_eye is %s): full detail needs both eyes", f162::name(keyMode));
}

// ---- 2. the circle ---------------------------------------------------------------------------------------------------------------------------------------------------------------
enum class Why : int { kOk = 0, kScopeZoomOff = 1, kAdsFovUnknown = 2, kNoFrustum = 3, kNoCentre = 4 };
inline const char* whyText(Why w) noexcept {
    switch (w) {
        case Why::kOk: return "ok";
        case Why::kScopeZoomOff: return "fix.native_on_foot_ads_scope_zoom is off";
        case Why::kAdsFovUnknown: return "the ADS FOV of the held weapon is not known";
        case Why::kNoFrustum: return "the eye's frustum is not known";
        default: return "the sight's far point is not known";
    }
}
struct Radius {
    Why why = Why::kOk;
    double radiusRad = std::nan("");
    double halfFovRad = std::nan("");
};
// The flat rule. scopeZoomOn: fix.native_on_foot_ads_scope_zoom, the one setting that switches the circle (F205b: the circle's own key was removed on the pilot's word). fovKnown: the held weapon's ADS FOV
// is in the table (scope_zoom_s1.h adsFovDeg). hipDeg: the scope zoom's reference FOV (the hip FOV the lens measures M against).
inline Radius circleRadius(bool scopeZoomOn, bool fovKnown, double adsDeg, double hipDeg, const float tan4[4]) noexcept {
    Radius r;
    if (!scopeZoomOn) { r.why = Why::kScopeZoomOff; return r; }
    if (!fovKnown || !(std::isfinite(adsDeg) && adsDeg > 0.0)) { r.why = Why::kAdsFovUnknown; return r; }
    r.halfFovRad = viewcircle::halfFovRad(tan4);
    if (!std::isfinite(r.halfFovRad)) { r.why = Why::kNoFrustum; return r; }
    r.radiusRad = viewcircle::radiusRad(adsDeg, hipDeg, r.halfFovRad);
    if (!std::isfinite(r.radiusRad)) { r.why = Why::kAdsFovUnknown; return r; }
    return r;
}

// The press line. mode: "on" or "off" (the Anti-aliasing mode, so a flight in each can be compared). tc: the sight's far point as tangents of the left and the right eye's own view (null: none known).
inline int formatLine(char* buf, size_t n, uint32_t press, const char* mode, const Radius& r, double adsDeg, double hipDeg, bool fromBare, const char* item, const char* scope,
                      const float (*tc)[2] = nullptr) noexcept {
    const double deg = 180.0 / viewcircle::kPi;
    if (r.why == Why::kScopeZoomOff)   // F205b: the circle follows the scope zoom setting; with it off the press says so and nothing else
        return std::snprintf(buf, n, "ADS view circle (F205): press %u: no circle (fix.native_on_foot_ads_scope_zoom is off)", press);
    if (r.why != Why::kOk)
        return std::snprintf(buf, n, "ADS view circle (F205): press %u, Anti-aliasing %s: no circle: %s (item %s, scope %s)", press, mode, whyText(r.why), item ? item : "none", scope ? scope : "unknown");
    int len = std::snprintf(buf, n, "ADS view circle (F205): press %u, Anti-aliasing %s: radius %.2f deg (ADS FOV %.2f / hip FOV %.2f x the headset's half-FOV %.2f deg, vertical)%s; item %s, scope %s",
                            press, mode, r.radiusRad * deg, adsDeg, hipDeg, r.halfFovRad * deg, fromBare ? "; the ADS FOV is the item's own (an unmeasured scoped copy takes it)" : "", item ? item : "none",
                            scope ? scope : "unknown");
    if (tc && len > 0 && static_cast<size_t>(len) < n)
        len += std::snprintf(buf + len, n - static_cast<size_t>(len), "; centre (tangents) left %+.4f/%+.4f right %+.4f/%+.4f", static_cast<double>(tc[0][0]), static_cast<double>(tc[0][1]),
                             static_cast<double>(tc[1][0]), static_cast<double>(tc[1][1]));
    if (len > 0 && static_cast<size_t>(len) < n) len += std::snprintf(buf + len, n - static_cast<size_t>(len), " (the scope zoom is on)");   // F205b: what switched it
    return len;
}

// The runtime's report of one run of the circle, as the graphics log gets it (edvrNoteNativeViewCircle). "ADS view circle on N frames, radius X deg" is the figure --health reads.
inline bool reportReadable(const EdvrNativeViewCircleReport* r) noexcept {
    if (!r || r->size < sizeof(EdvrNativeViewCircleReport) || r->version != EDVR_NATIVE_VIEW_CIRCLE_REPORT_VERSION_1) return false;
    const float values[] = {r->seconds, r->radiusDeg, r->requestedDeg, r->coverageDeg, r->weightPeak, r->centre[0], r->centre[1], r->centre[2], r->centre[3]};
    for (float v : values)
        if (!(v == v) || v > 1e9f || v < -1e9f) return false;
    return true;
}
inline int formatRuntime(char* buf, size_t n, const EdvrNativeViewCircleReport& r) noexcept {
    return std::snprintf(buf, n,
                         "ADS view circle (F205) runtime: ADS view circle on %u frames, radius %.2f deg (the d3d11 half asked %.2f deg%s), over %.2f s; refused %u, with the share %u; weight peak %.2f; "
                         "centre (u, v) left %.3f/%.3f right %.3f/%.3f",
                         r.frames, static_cast<double>(r.radiusDeg), static_cast<double>(r.requestedDeg),
                         r.cappedFrames ? "; cut to the circle that hides the full-detail scope's side bars on some frames" : "", static_cast<double>(r.seconds), r.refusedFrames, r.sharedFrames,
                         static_cast<double>(r.weightPeak), static_cast<double>(r.centre[0]), static_cast<double>(r.centre[1]), static_cast<double>(r.centre[2]), static_cast<double>(r.centre[3]));
}
inline constexpr size_t kRuntimeLineBytes = 400;

}  // namespace f205
}  // namespace nofoot
}  // namespace edvr
