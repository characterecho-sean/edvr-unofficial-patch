// F203 (native on-foot VR): an ADS press under mouse look with the ADS head aim keeps the sight where the mouse put it; F69i's snap injects only the part of the head-to-sight offset beyond
// kKeepDeg on each axis. Header only, no game or D3D11 state, so native_frame_test runs the very functions adsCentreTick uses. Journal: the arc doc, F203.
//
// F69i brought the sight onto the head's forward axis at every press: the measured offset, yaw and pitch, went into the snap whole (injected over 0.2 s through F68a's path). The pilot
// (edvr_gfx_20261101_041020, Frontier, Pimax, mouse look with fix.native_on_foot_ads_head_aim turned on at 04:12:26): "the ADS spot does not match where the mouse was like it does with full LtA or even
// toggling between LtA and mouse aim." Every press read like press 1 ("the sight is yaw +3.65 pitch -0.28 deg from the head's forward ... injected yaw +3.65 pitch -0.28 deg", then "yaw +0.00 pitch +0.14 deg"
// after it) and press 9 (yaw -0.15, pitch +3.50, injected whole): the press snapped the sight from where the mouse had put it onto the headset's forward axis. The arc has learned that the pilot's gaze sits
// off that axis by a personal amount (about 5 deg on his Quest, 10.5 to 12 deg on his Pimax: F139, F195), and F195 and F197 fixed the same assumption at a look-to-aim engage by keeping the offset the pilot had.
//
// The rule is F195's, on both axes: the offset is kept up to kKeepDeg (F195's kKeepPitchDeg, the one constant) either way, and only what is beyond it is injected. A settled press (offsets of a few
// degrees) injects nothing; a press with the head turned far from the sight still brings the sight to within kKeepDeg of the head. The yaw part still goes through the game's cone bound (the weapon may not
// pass 37 deg off the body: adsCentreYawBounded), and not at all when there is nothing to inject (the bound would otherwise pull a weapon already past the band back in on a zero ask). The head aim in the
// sights works from steps (adsHeadDelta), so it carries on from the kept offset. The unwind of the release undoes what was carried (AdsAimMap: yawCarried and pitchSeat), which a zero injection leaves at zero.
// Not touched: the 0.5 deg floor, the 90 deg refusal (the head turned away from the weapon), and F69i's measure-only role inside look to aim (F109).
#pragma once

#include "native_on_foot_core.h"   // adsCentreYawBounded, kAdsDeg
#include "native_on_foot_f195.h"   // kKeepPitchDeg

#include <cmath>
#include <cstddef>
#include <cstdio>

namespace edvr {
namespace nofoot {
namespace f203 {

constexpr double kKeepDeg = f195::kKeepPitchDeg;   // the most offset an ADS press leaves between the head's forward and the sight, on each axis

struct Axis {
    double injectDeg = 0.0;   // what goes into the snap
    double keptDeg = 0.0;     // what stays between the head and the sight (injectDeg + keptDeg is the offset)
};

// One axis, degrees (the turn that carries the sight onto the head's forward, as adsCentreError gives it). An offset that is not a number injects all of it, as F69i did (the snap ignores what is not finite).
inline Axis keepAxis(double offsetDeg) noexcept {
    Axis a;
    a.injectDeg = offsetDeg;
    if (!std::isfinite(offsetDeg)) return a;
    a.keptDeg = offsetDeg < -kKeepDeg ? -kKeepDeg : offsetDeg > kKeepDeg ? kKeepDeg : offsetDeg;
    a.injectDeg = offsetDeg - a.keptDeg;
    return a;
}

struct Plan {
    double yawDeg = 0.0, pitchDeg = 0.0;           // the snap to inject (the yaw after the cone's bound)
    double keptYawDeg = 0.0, keptPitchDeg = 0.0;   // what is left between the head and the sight after it: the offset less what is injected
    bool bounded = false;                          // the cone cut the yaw
};

// The press's snap: the measured offset (yaw, pitch, degrees) and the weapon's yaw offset from the body (offYawDeg, + left, as adsAimStep measures it, 0 when not measured).
inline Plan plan(double yawOffsetDeg, double pitchOffsetDeg, double offYawDeg) noexcept {
    Plan p;
    const Axis y = keepAxis(yawOffsetDeg), q = keepAxis(pitchOffsetDeg);
    p.yawDeg = y.injectDeg != 0.0 ? adsCentreYawBounded(offYawDeg, y.injectDeg, &p.bounded) : 0.0;
    p.pitchDeg = q.injectDeg;
    p.keptYawDeg = yawOffsetDeg - p.yawDeg;
    p.keptPitchDeg = pitchOffsetDeg - p.pitchDeg;
    return p;
}

// F69i's press line gives the kept offset before the injected one: "kept yaw +3.65 pitch -0.28 deg (the mouse's aim, F203), " (empty inside look to aim, where the press only measures).
inline int keptText(char* buf, size_t n, const Plan& p) noexcept {
    return std::snprintf(buf, n, "kept yaw %+.2f pitch %+.2f deg (the mouse's aim, F203), ", p.keptYawDeg, p.keptPitchDeg);
}

// The proof the report gives after a snap: the sight's offset from the head's forward against the kept offset (not against 0), degrees.
inline double proofErrorDeg(double yawDeg, double pitchDeg, double keptYawDeg, double keptPitchDeg) noexcept {
    return std::sqrt((yawDeg - keptYawDeg) * (yawDeg - keptYawDeg) + (pitchDeg - keptPitchDeg) * (pitchDeg - keptPitchDeg));
}

}  // namespace f203
}  // namespace nofoot
}  // namespace edvr
