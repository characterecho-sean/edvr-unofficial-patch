// F205/F206 (native on-foot VR): the full-detail scope takes both eyes, and its view is shown inside a circle that hides its black side bars. The pure parts, header only, no game or D3D11
// state, so native_frame_test runs the very functions the frame thread uses. Journal: the arc doc, F205 and F206.
//
// 1. THE EFFECTIVE SIGHT EYE. fix.native_on_foot_ads_sight_eye is the player's choice (F162: both | right | left). The full-detail scope (B3) needs both: the left eye is drawn as the zoomed view
//    and the share shows the right eye's picture to both. So while B3 applies, a press behaves as both whatever the key says. It is decided ONCE, at the press's start (the sight lock's aiming eye
//    must not change in the middle of a press: a flip would jump the lock), kept through a re-engage from the release, and ended when the press has fully ended (the state is idle again): then
//    the player's setting is back. One function, effectiveSightEye, is what every reader of the choice asks (adsSightEyeMode in native_on_foot.cpp); nothing else reads the key.
//    B3 applies when its key is on, the scope zoom key is on, the held item is B3's table entry (the scoped Executioner) AND a temporal mode runs. With Anti-aliasing off B3's full detail does not
//    run (its left door needs the upscaler's picture: F202 left it so), so nothing needs the left eye to show the right's picture and the press keeps the player's sight eye (the RC5 review:
//    forcing both there took the player's choice for no benefit, and the log's "full detail needs both eyes" was false). The circle follows the latch (section 2). The latch is
//    decided once per press, so a mode switched in the middle of one cannot flip the aiming eye.
//
// 2. THE VIEW CIRCLE (view_circle.h; F206, the pilot's word: "back off on the stencil entirely for all weapons except the sniper with scope mod so that it hides the black vertical bars and then
//    have the widest radius possible that does"). The circle is the full-detail scope's alone: it is asked for only in a press B3 applies to (the latch above forced both: the scoped Executioner,
//    B3 on, the scope zoom on, a temporal mode), so no other weapon, no B3-less scoped press and no Anti-aliasing-off press draws one, and the circle needs no sight-eye condition of its own.
//    The d3d11 half asks for its centre alone (the sight's far point in each eye's own view); the runtime draws it exactly while B3's right-eye mirror strip is, by the mirror's weight, as wide
//    as it can be and still hide the bars (scope_circle.h coverageRadius). F205's flat-ADS-FOV rule, its table and its weight ramp are gone.
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

// B3 applies: its key on, the scope zoom key on, the held item B3's entry, and a temporal mode running (temporalPassWantsFssChrome: what b3Tick gates the full detail on).
inline bool b3Applies(bool fullKeyOn, bool scopeZoomKeyOn, bool renderedEntry, bool temporalMode) noexcept { return fullKeyOn && scopeZoomKeyOn && renderedEntry && temporalMode; }

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

// ---- 2. the circle (F206) --------------------------------------------------------------------------------------------------------------------------------------------------------
// Whether a press has a circle: exactly when the latch forced both, that is when B3's full-detail view runs in it (b3Applies at the engage from idle). Decided once, with the rest of the press's
// state, so a setting changed in the middle of a press changes nothing until the next one.
inline bool pressHasCircle(const PressLatch& latch) noexcept { return latch.forced; }
// The request: the circle's centre is asked for in a press that has a circle once the sight's far point is known in both eyes. Whether the circle is drawn, and by what weight and radius, is the
// runtime's (the mirror strip's weight; the widest radius that keeps the bars in the black): a request while the strip is down draws nothing.
inline bool requestOn(bool pressHasCircleNow, bool haveCentre) noexcept { return pressHasCircleNow && haveCentre; }

// A B3 press whose window stood (the strip up or the left eye narrowed) and that got no circle says why, once, in a line after the press.
enum class NoCircle : int { kNoCentre = 1, kNoFrame = 2 };
inline const char* noCircleText(NoCircle w) noexcept {
    return w == NoCircle::kNoCentre ? "the sight's far point was not known in both eyes while the full-detail view stood"
                                    : "the runtime drew no circle frame while the full-detail view stood (no report: the request did not reach it, or no radius hid the bars)";
}
// What the frame thread knows of a press at its end. The line is due when the press had a circle and its window stood, no run of the circle was reported for it, and the window has been gone
// (and the press over) for kNoCircleWaitMs: the runtime reports a run when the mirror has gone, so a report that is coming has come.
constexpr uint64_t kNoCircleWaitMs = 1000;
struct PressEnd {
    bool hasCircle = false, windowSeen = false, runReported = false, haveCentre = false;
};
inline bool noCircleDue(const PressEnd& p, uint64_t msSinceOverAndGone) noexcept { return p.hasCircle && p.windowSeen && !p.runReported && msSinceOverAndGone >= kNoCircleWaitMs; }
inline NoCircle noCircleWhy(const PressEnd& p) noexcept { return p.haveCentre ? NoCircle::kNoFrame : NoCircle::kNoCentre; }
inline int formatNoCircle(char* buf, size_t n, uint32_t press, NoCircle why, const char* item, const char* scope) noexcept {
    return std::snprintf(buf, n, "ADS view circle (F206): press %u: no circle: %s (item %s, scope %s)", press, noCircleText(why), item ? item : "none", scope ? scope : "unknown");
}

// The runtime's report of one run of the circle (view_circle.h), as the graphics log gets it (edvrNoteNativeViewCircle): the press line. "ADS view circle (F206): press N: radius X deg" is what
// --health reads. Y, the nearest edge of what the other eye covers, less the band (the margin and half the 3 deg soft edge: Y - X) is the radius; the run's smallest and largest radius say how it
// followed the window.
// F210b: a report of the full size also carries the pitch the centre was turned by and the field's up and down angles; one of the size before it (a runtime of the build before) is read without them.
inline bool reportHasPitch(const EdvrNativeViewCircleReport* r) noexcept { return r && r->size >= sizeof(EdvrNativeViewCircleReport); }
inline bool reportReadable(const EdvrNativeViewCircleReport* r) noexcept {
    if (!r || r->size < EDVR_NATIVE_VIEW_CIRCLE_REPORT_BASE_BYTES || r->version != EDVR_NATIVE_VIEW_CIRCLE_REPORT_VERSION_2) return false;
    const float values[] = {r->seconds, r->radiusDeg, r->radiusMinDeg, r->radiusMaxDeg, r->edgeDeg, r->weightPeak, r->centre[0], r->centre[1], r->centre[2], r->centre[3]};
    for (float v : values)
        if (!(v == v) || v > 1e9f || v < -1e9f) return false;
    if (reportHasPitch(r)) {
        const float pitch[] = {r->pitchDeg, r->fieldUpDeg, r->fieldDownDeg};
        for (float v : pitch)
            if (!(v == v) || v > 1e9f || v < -1e9f) return false;
    }
    return true;
}
inline int formatRun(char* buf, size_t n, uint32_t press, const EdvrNativeViewCircleReport& r) noexcept {
    const int head = std::snprintf(buf, n,
                         "ADS view circle (F206): press %u: radius %.2f deg (the widest that keeps the full-detail view's side bars in the black: nearest edge %.2f deg less the band %.2f deg); "
                         "over the run %.2f to %.2f deg; drawn on %u frames over %.2f s; refused %u, with the share %u; weight peak %.2f; centre (u, v) left %.3f/%.3f right %.3f/%.3f",
                         press, static_cast<double>(r.radiusDeg), static_cast<double>(r.edgeDeg), static_cast<double>(r.edgeDeg - r.radiusDeg), static_cast<double>(r.radiusMinDeg),
                         static_cast<double>(r.radiusMaxDeg), r.frames, static_cast<double>(r.seconds), r.refusedFrames, r.sharedFrames, static_cast<double>(r.weightPeak),
                         static_cast<double>(r.centre[0]), static_cast<double>(r.centre[1]), static_cast<double>(r.centre[2]), static_cast<double>(r.centre[3]));
    if (!reportHasPitch(&r) || head < 0 || static_cast<size_t>(head) >= n) return head;
    // F210b: the centre is the sight pitched by the middle of the field vertically (the mean over both eyes of up and down, signed); a headset whose up and down are opposite reads +0.0000
    const int tail = std::snprintf(buf + head, n - static_cast<size_t>(head), "; pitch %+.4f deg (the middle of the field: up %+.2f, down %+.2f deg)", static_cast<double>(r.pitchDeg),
                                   static_cast<double>(r.fieldUpDeg), static_cast<double>(r.fieldDownDeg));
    return tail < 0 ? head : head + tail;
}
inline constexpr size_t kRunLineBytes = 480;

}  // namespace f205
}  // namespace nofoot
}  // namespace edvr
