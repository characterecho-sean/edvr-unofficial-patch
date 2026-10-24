// F97a checks (native on-foot VR): the armed body under the eye again. Header only, run by native_frame_test (run97). The figures are the F95 flight's
// (edvr_gfx_20261024_152150) and the F93 flight's (edvr_gfx_20261024_124854): the rifle stance's animated neck 1.300 m under a game camera at 1.56 m (F93 raised it
// 8-11 cm); the Energylink's upright neck 1.543 m under 1.59 m; the rifle crouched with the pin's eye level with the animated neck; the pilot's weapon eye up 8.5 cm.
#pragma once

#include "../../src/d3d11/native_on_foot_core.h"
#include "../../src/d3d11/native_on_foot_f97.h"

#include <cmath>
#include <cstdint>

namespace f97test {

namespace nf = edvr::nofoot;
namespace f97 = edvr::nofoot::f97;

inline bool within(double a, double b, double tol) { return std::fabs(a - b) <= tol; }

template <class Check>
inline void run97(Check&& check) {
    constexpr double kH = 0.155, kUp = 0.085;
    // ---- the target's height ----------------------------------------------------------------------------------------------------------------------------------------
    {
        const f97::ArmedTarget un = f97::armedNeckTarget(1.300, 1.410, 1.56 + kUp, kH, 0.0, 0.0);
        check(un.y == 1.300, "F97a: unarmed (w 0) the neck target is the animated neck's height exactly (F94a's)");
        const f97::ArmedTarget r = f97::armedNeckTarget(1.300, 1.410, 1.56 + kUp, kH, 0.0, 1.0);
        check(within(r.y, 1.410, 1e-12) && within(r.raiseF93, 0.110, 1e-12) && r.lowerEye == 0.0,
              "F97a: the rifle standing, armed: F93's 11 cm raise (the leaning stance upright under the eye), nothing lowered");
        const f97::ArmedTarget tool = f97::armedNeckTarget(1.543, 1.590 - kH, 1.590 + kUp, kH, 0.0, 1.0);
        check(within(tool.y, 1.590 + kUp - kH, 1e-12) && within(tool.y - 1.543, -0.023, 1e-12),
              "F97a: the Energylink upright, armed: lowered 2.3 cm, only as far as keeps the eye at the hip 15.5 cm over the neck (F93 lowered 10.8 cm: the knees bent)");
        const f97::ArmedTarget c = f97::armedNeckTarget(1.040, 1.042 - 0.10, 1.042 + kUp, kH, 0.0, 1.0);
        check(within((1.042 + kUp) - c.y, kH, 1e-12) && c.y < 1.040,
              "F97a: the rifle crouched with the eye level with the neck: the body goes down until the eye at the hip is 15.5 cm over the neck (out of the torso)");
        const f97::ArmedTarget ads = f97::armedNeckTarget(1.040, 1.040, 1.042 + kUp, kH, 1.0, 1.0);
        check(ads.y == 1.040 && ads.lowerEye == 0.0, "F97a: in ADS (detached, F73) the lowering is released with the detach: the authored neck");
        const f97::ArmedTarget half = f97::armedNeckTarget(1.300, 1.410, 1.56 + kUp, kH, 0.0, 0.5);
        check(within(half.y, 1.355, 1e-12), "F97a: halfway through a draw (w 0.5) the body is half raised");
        const f97::ArmedTarget nan = f97::armedNeckTarget(1.300, std::nan(""), std::nan(""), kH, 0.0, 1.0);
        check(nan.y == 1.300, "F97a: a target and an eye that are not numbers: the animated neck");
    }
    // ---- the blend through a draw: no jump --------------------------------------------------------------------------------------------------------------------------
    {
        // F88's weight ramps over 0.25 s while the stance leans (the animated neck 1.543 -> 1.300 over 0.4 s, F93's target from a camera 1.59 -> 1.56)
        nf::f88::Weight wt;
        double prev = 1.543, maxStep = 0.0;
        for (int i = 0; i < 90; ++i) {
            const double t = i / 90.0, a = std::fmin(1.0, t / 0.4);
            const double n0 = 1.543 - 0.243 * a, cam = 1.59 - 0.03 * a;
            const double w = wt.step(true, 1.0 / 90.0);
            const double y = f97::armedNeckTarget(n0, cam - kH, cam + kUp * w, kH, 0.0, w).y;
            if (i > 0) maxStep = std::fmax(maxStep, std::fabs(y - prev));
            prev = y;
        }
        check(within(prev, 1.56 - kH, 1e-12) && maxStep < 0.02, "F97a: a rifle draw with the stance leaning under it: the neck target moves under 2 cm a tick to F93's height");
    }
    // ---- the legs keep their feet -----------------------------------------------------------------------------------------------------------------------------------
    {
        const double hip[3] = {0.10, 0.85, 0.0}, foot[3] = {0.10, 0.0, 0.0}, fb[3] = {0.0, 0.0, 1.0};
        const double room = f97::legRaiseRoom(hip[1], foot[1], 0.45, 0.45, 0.85, nf::kStandReach);
        const double up1[3] = {hip[0], hip[1] + room, hip[2]}, up2[3] = {hip[0], hip[1] + room + 0.01, hip[2]};
        const nf::LegPlant a = nf::legPlant(up1, foot, 0.45, 0.45, fb, 0.85), b = nf::legPlant(up2, foot, 0.45, 0.45, fb, 0.85);
        check(within(room, 0.995 * 0.9 - 0.85 - 1e-4, 1e-12) && a.liftM == 0.0 && b.liftM > 0.0,
              "F97a: the raise's room keeps a planted foot on the ground (the planting rule lifts it 1 cm further up)");
        const double crouched = f97::legRaiseRoom(0.55, 0.0, 0.45, 0.45, 0.55, nf::kStandReach);
        check(within(crouched, 0.8955 - 0.55 - 1e-4, 1e-12), "F97a: a crouched leg has the room to stand up to its reach");
    }
    // ---- the stance bins --------------------------------------------------------------------------------------------------------------------------------------------
    check(f97::stanceBin(false, false) == f97::kStanding && f97::stanceBin(true, false) == f97::kCrouched && f97::stanceBin(true, true) == f97::kCrouchMoving &&
              f97::stanceBin(false, true) == f97::kMoving && f97::stanceName(f97::kCrouchMoving)[0] == 'm',
          "F97a: the line's stance bins: standing, crouched, moving crouched, moving");
}

}  // namespace f97test
