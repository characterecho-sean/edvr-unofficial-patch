// F195 (native on-foot VR): at a look-to-aim engage the aim keeps its pitch where the mouse left it; only the yaw is centred. BEHAVIOUR since F195b: the A/B key
// (experimental.native_on_foot_look_to_aim_keeps_aim_pitch) was RETIRED on 2026-10-31 on the pilot's word ("Retire both", after F195 flew OK: "works perfectly now toggling between the two modes"). Header only, no game or D3D11 state, so native_frame_test runs the very
// function the engage's centring uses. Journal: the arc doc, F195.
//
// F109 centred the aim on the head at each engage: the reported head's offset from the aim, yaw and pitch, went into the follower's target once. The pilot flew F194 on his Quest 3 (edvr_gfx_20261031_134656):
// "quite a bit worse", and "the aim dropped". At four engages after a settled mouse aim the forward axis stood +0.77, +4.46, +4.79 and +5.55 deg from the aim in pitch (his gaze sits that far off the
// forward axis, or the mouse aim does; on the Pimax F139 measured 10.5 to 12 deg with a symmetric frustum, so no headset value predicts it); F109 moved the aim up by that, F194's ray (the Quest's
// picture centre, 13 deg down) moved it down 6.6 to 11.4 deg instead. Either way the engage moved the aim off where he was looking.
//
// The rule: the pitch offset the pilot had is kept, up to kKeepPitchDeg either way; only what is beyond it is centred. With op the reported head's pitch offset from the aim (as centreOffset gives it), the
// kept offset is k = clamp(op, -15 deg, +15 deg) and op - k is what goes into the follower's pitch target. A settled 5 deg offset stays (the aim does not move in pitch); a head pitched 26 deg up at the
// first on-foot frame keeps 15 and centres 11. The yaw is centred whole, as before. The follower's head steps, F133's yaw re-centre and the in-flight account are untouched.
//
// F197 (the keep applies only after a settled mouse aim): the pilot (edvr_gfx_20261031_161247, Frontier): "when I first loaded in with LtA enabled, the reticule is well above my eye line. Toggling it fixed it." The
// engage at the first on-foot frame centred "the reported head was yaw -19.92 pitch -18.81 deg from the aim; pitch kept -15.00 deg, centred -3.81 deg": F195 kept 15 deg of an offset from the game's spawn aim, which
// nobody was looking at. The later toggles from mouse look kept -8.4 to -9.3 and centred 0, which is what the keep is for. So the clamp above applies only to an engage that follows a settled mouse aim: the pilot was on
// foot under mouse look (native on foot active, look to aim neither engaged nor unwinding) for kSettledMouseLookMs without a break. Any other engage centres all of the pitch offset, as F109 did: the first engage of an
// on-foot session (the key already on at load or at a disembark), an engage under a second after going on foot, and the end of an F131 hold.
//
// F194 part 1 (the ray through the middle of the picture as the centring's direction, with its key) was REMOVED on 2026-10-31 (F194b), after F195 flew well and the pilot said "Remove the key." It was refuted: the
// pilot's gaze offset is about 5 deg on his Quest against the picture's centre 12.2 deg below the forward axis (and 10.5 to 12 deg on his Pimax, with a symmetric frustum), so the picture's middle moved the aim
// 6.6 to 11.4 deg off where he was looking; the offset he has is his own and an engage should leave it where it is. The offset the engage keeps is the forward axis's (adsHeadAngles) again.
#pragma once

#include "native_on_foot_core.h"   // kAdsDeg

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace edvr {
namespace nofoot {
namespace f195 {

constexpr double kKeepPitchDeg = 15.0;   // the most pitch offset an engage leaves between the head and the aim

struct Kept {
    double centredRad = 0.0;   // what goes into the follower's pitch target (radians, the seat convention)
    double keptRad = 0.0;      // what stays between the reported head and the aim
};

// opRad: the reported head's pitch offset from the aim at the engage. An offset that is not a number centres all of it (it was F109's full centring, and the follower ignores what is not finite).
inline Kept keptPitch(double opRad) noexcept {
    Kept k;
    k.centredRad = opRad;
    if (!std::isfinite(opRad)) return k;
    const double lim = kKeepPitchDeg * kAdsDeg;
    k.keptRad = opRad < -lim ? -lim : opRad > lim ? lim : opRad;
    k.centredRad = opRad - k.keptRad;
    return k;
}

// ---- F197: when the keep applies --------------------------------------------------------------------------------------------------------------------------------------------------------------------

constexpr uint64_t kSettledMouseLookMs = 1000;   // the mouse aim is settled after this long on foot under mouse look, without a break
constexpr uint64_t kRunGapMs = 500;              // a gap between two ticks longer than this (a stall, a load) is a break

// Why an engage kept the pitch offset or did not.
enum Why : int { kAfterMouseLook = 0, kFirstEngage = 1, kUnderASecond = 2, kHoldEnd = 3 };

// The run under mouse look on foot, ticked once a frame with whether the pilot is on foot under mouse look at that tick (native on foot active, on foot, look to aim neither engaged nor unwinding). Any tick that is
// not, or that comes more than kRunGapMs after the one before, breaks the run: the next such tick starts it again.
struct MouseRun {
    bool running = false;
    uint64_t sinceMs = 0, lastMs = 0;
    void step(uint64_t nowMs, bool underMouseLook) noexcept {
        if (!underMouseLook) {
            running = false;
            return;
        }
        if (running && nowMs > lastMs && nowMs - lastMs > kRunGapMs) running = false;
        if (!running) {
            running = true;
            sinceMs = nowMs;
        }
        lastMs = nowMs;
    }
    // The ms of the run as of nowMs; 0 when there is none.
    uint64_t ms(uint64_t nowMs) const noexcept { return running && nowMs >= sinceMs ? nowMs - sinceMs : 0; }
};

// The decision at an engage. runMs: the run under mouse look just before it; firstEngage: no engage yet since the pilot went on foot; holdEnd: the end of an F131 hold (a fresh start, not an engage by the pilot).
inline Why whyOf(uint64_t runMs, bool firstEngage, bool holdEnd) noexcept {
    if (holdEnd) return kHoldEnd;
    if (runMs >= kSettledMouseLookMs) return kAfterMouseLook;
    return firstEngage ? kFirstEngage : kUnderASecond;
}
inline bool keeps(Why why) noexcept { return why == kAfterMouseLook; }

// The centring's pitch part: the clamp above after a settled mouse aim, all of it centred otherwise (F109's full centring).
inline Kept centring(double opRad, Why why) noexcept {
    if (keeps(why)) return keptPitch(opRad);
    Kept k;
    k.centredRad = opRad;
    return k;
}

// The words the F109 centred line gives after "pitch kept ..., centred ...": "after 9.0 s of mouse look", "not kept: the first engage on foot", "not kept: 0.4 s of mouse look, under 1 s", "not kept: the end of a hold".
inline int whyText(char* buf, size_t n, Why why, uint64_t runMs) noexcept {
    const double s = static_cast<double>(runMs) / 1000.0;
    switch (why) {
        case kAfterMouseLook: return std::snprintf(buf, n, "after %.1f s of mouse look", s);
        case kFirstEngage: return std::snprintf(buf, n, "not kept: the first engage on foot");
        case kUnderASecond: return std::snprintf(buf, n, "not kept: %.1f s of mouse look, under 1 s", s);
        default: return std::snprintf(buf, n, "not kept: the end of a hold");
    }
}

}  // namespace f195
}  // namespace nofoot
}  // namespace edvr
