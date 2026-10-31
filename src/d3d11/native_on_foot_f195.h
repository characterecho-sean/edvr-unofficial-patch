// F195 (native on-foot VR): at a look-to-aim engage the aim keeps its pitch where the mouse left it; only the yaw is centred. Header only, no game or D3D11 state, so native_frame_test runs the very
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
#pragma once

#include "native_on_foot_core.h"   // kAdsDeg

#include <cmath>

namespace edvr {
namespace nofoot {
namespace f195 {

constexpr double kKeepPitchDeg = 15.0;   // the most pitch offset an engage leaves between the head and the aim

struct Kept {
    double centredRad = 0.0;   // what goes into the follower's pitch target (radians, the seat convention)
    double keptRad = 0.0;      // what stays between the reported head and the aim
};

// opRad: the reported head's pitch offset from the aim at the engage. The key off, or an offset that is not a number, centres all of it (F109's full centring, as before).
inline Kept keptPitch(double opRad, bool keyOn) noexcept {
    Kept k;
    k.centredRad = opRad;
    if (!keyOn || !std::isfinite(opRad)) return k;
    const double lim = kKeepPitchDeg * kAdsDeg;
    k.keptRad = opRad < -lim ? -lim : opRad > lim ? lim : opRad;
    k.centredRad = opRad - k.keptRad;
    return k;
}

}  // namespace f195
}  // namespace nofoot
}  // namespace edvr
