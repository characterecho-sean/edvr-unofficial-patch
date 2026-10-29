// F163 (native on-foot VR): the crouched stance read from the animated neck, which holds in the sights. Header only, no game or D3D11 state, so native_frame_test runs the very
// functions the Animate post-hook uses. Journal: the arc doc, F163.
//
// F161 held F97a's lowering through the ADS detach for a crouched stance, blended by the pin's crouch weight (w_c, F69a: smoothstep over 20 to 45 cm of the cyclopean eye's drop
// below its standing mean). That weight reads 0 without the standing calibration, and the calibration (45 frames: unarmed, still, upright, not in the sights, F69a) is not there for
// a pilot who drew a weapon before he stood still. The flight edvr_gfx_20261028_153030 (and the flight edvr_gfx_20261028_135901 before it) had no calibration in any of its ADS
// seconds: the pin's w_c read 0.00 on every call while the camera stood at 0.95 to 0.99 m (a standing ADS second reads 1.55 to 1.60 m) and the animated neck at 0.85 to 1.08 m, so
// F161 filed the crouch-walk as "ADS standing", held nothing, and the left upper arm passed 0.4 to 4 cm from the eye (17 frames under 4 cm with the shotgun, 12 with the plasma AR).
// The camera did not collapse in the sights; the calibration was missing.
//
// The posture here is the game's own stance and nothing else: the animated neck's height (the skeleton's animation, before EDVR writes the body), against the standing unarmed neck.
// It needs no calibration, no camera and no detach state. The reference is the neck F98c stored from a settled unarmed upright stance when there is one, else the commander's
// upright neck (1.542 to 1.543 m in every flight); neither flight had a stored one in its crouched ADS seconds ("the unarmed neck stored: standing none").
//
// The figures (the animated neck, per second, ADS with an item, the flights of 10-27 and 10-28, every item: rifles, shotgun, SMGs, sniper, launcher, pistols):
//   standing in the sights   neck 1.247 to 1.550 m  (down 0.0 to 0.296 m from 1.543)   per-second minimum 1.247 (the laser SMG), the plasma AR 1.292, the pistols 1.328
//   crouched in the sights   neck 0.847 to 1.080 m  (down 0.463 to 0.696 m)             crouch-walking 1.012 to 1.080, still 0.847 to 0.871
// so the weight is a smoothstep over the neck's drop from 0.33 to 0.43 m (neck 1.213 to 1.113 m): 0 on every standing frame, 1 on every crouched one, 3 cm of margin each side.
// The legs ratio (F97a's "legs") does not separate them (crouch-walking 0.795, a standing laser SMG 0.700); the camera's height does (crouched 0.88 to 0.99 m, standing 1.55 to
// 1.60 m) but needs the same standing reference.
#pragma once

#include "native_on_foot_core.h"   // smoothstep3

#include <cmath>

namespace edvr {
namespace nofoot {
namespace f163 {

constexpr double kStandingNeckM = 1.543;                         // the commander's upright unarmed neck (F98c's stored one: 1.542 to 1.543 m in every flight)
constexpr double kStoredNeckMinM = 1.40, kStoredNeckMaxM = 1.70;  // a stored standing neck outside this is not an upright commander's: the constant stands
constexpr double kDropLoM = 0.33, kDropHiM = 0.43;                // the weight: 0 up to a 33 cm drop of the neck, 1 from 43 cm

// The upright neck the drop is measured from: the stored unarmed standing neck (F98c) when there is one and it is a plausible upright neck, else the commander's.
inline double standingNeck(bool haveStored, double storedNeckM) noexcept {
    return haveStored && std::isfinite(storedNeckM) && storedNeckM >= kStoredNeckMinM && storedNeckM <= kStoredNeckMaxM ? storedNeckM : kStandingNeckM;
}
// How far the animated neck stands under the upright one (m, negative over it); not a number when the neck is not.
inline double neckDrop(bool haveStored, double storedNeckM, double animatedNeckM) noexcept {
    return std::isfinite(animatedNeckM) ? standingNeck(haveStored, storedNeckM) - animatedNeckM : std::nan("");
}
// The crouched weight of that drop: 0 standing, 1 crouched, a smoothstep between (not a number: 0, standing, as F161 reads a weight that is not a number).
inline double postureWeight(double neckDropM) noexcept { return smoothstep3(kDropLoM, kDropHiM, neckDropM); }

}  // namespace f163
}  // namespace nofoot
}  // namespace edvr
