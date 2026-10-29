// F97a checks (native on-foot VR): the armed body under the eye again. Header only, run by native_frame_test (run97). The figures are the F95 flight's
// (edvr_gfx_20261024_152150) and the F93 flight's (edvr_gfx_20261024_124854): the rifle stance's animated neck 1.300 m under a game camera at 1.56 m (F93 raised it
// 8-11 cm); the Energylink's upright neck 1.543 m under 1.59 m; the rifle crouched with the pin's eye level with the animated neck; the pilot's weapon eye up 8.5 cm.
// F161: the crouched stance keeps the lowering through the ADS detach. A standing stance is the earlier formula (kept below as armedNeckTargetBeforeF161) bit for bit; crouched, the
// figures are the crouch-walk of the flight edvr_gfx_20261028_135901 (the animated neck 3.5 cm over the eye, a 19 cm lowering); the blends across the detach and the stance are
// continuous.
#pragma once

#include "../../src/d3d11/native_on_foot_core.h"
#include "../../src/d3d11/native_on_foot_f97.h"

#include <cmath>
#include <cstdint>

namespace f97test {

namespace nf = edvr::nofoot;
namespace f97 = edvr::nofoot::f97;

inline bool within(double a, double b, double tol) { return std::fabs(a - b) <= tol; }

// The target of F97a as it stood before F161, to compare the standing stance with: the detach weight released the eye's lowering, and F93's target came detached.
inline f97::ArmedTarget armedNeckTargetBeforeF161(double n0y, double t93y, double eyeHipLevelY, double eyeHeightM, double detachW, double w) {
    f97::ArmedTarget o;
    o.y = n0y;
    const double wc = !(w > 0.0) ? 0.0 : w > 1.0 ? 1.0 : w;
    if (!std::isfinite(n0y) || wc == 0.0) return o;
    const double dw = !(detachW > 0.0) ? 0.0 : detachW > 1.0 ? 1.0 : detachW;
    o.raiseF93 = std::isfinite(t93y) ? t93y - n0y : 0.0;
    const double low = std::isfinite(eyeHipLevelY) && std::isfinite(eyeHeightM) ? (eyeHipLevelY - eyeHeightM) - n0y : 0.0;
    o.lowerEye = (low < 0.0 ? low : 0.0) * (1.0 - dw);
    o.asked = o.raiseF93 > o.lowerEye ? o.raiseF93 : o.lowerEye;
    o.y = n0y + wc * o.asked;
    return o;
}
// What a caller passes: F93's target at the detach weight (the pin's target moved toward the authored neck by it, the weapon view offset added) and before it.
struct Call97 {
    double n0y, t93Pre, shift, eyeHip, h, dw, w, cw;
    double t93() const { return (t93Pre + (n0y - t93Pre) * (dw < 0.0 ? 0.0 : dw > 1.0 ? 1.0 : dw)) + shift; }
    f97::ArmedTarget run() const { return f97::armedNeckTarget(n0y, t93(), eyeHip, h, dw, w, cw, t93Pre + shift); }
};

template <class Check>
inline void run97(Check&& check) {
    constexpr double kH = 0.155, kUp = 0.085;
    // the standing calls: no crouch weight, F93's target as the call gives it (the hip's target is then not used)
    const auto standing = [](double n0y, double t93y, double eyeHip, double h, double dw, double w) { return f97::armedNeckTarget(n0y, t93y, eyeHip, h, dw, w, 0.0, t93y); };
    // ---- the target's height ----------------------------------------------------------------------------------------------------------------------------------------
    {
        const f97::ArmedTarget un = standing(1.300, 1.410, 1.56 + kUp, kH, 0.0, 0.0);
        check(un.y == 1.300, "F97a: unarmed (w 0) the neck target is the animated neck's height exactly (F94a's)");
        const f97::ArmedTarget r = standing(1.300, 1.410, 1.56 + kUp, kH, 0.0, 1.0);
        check(within(r.y, 1.410, 1e-12) && within(r.raiseF93, 0.110, 1e-12) && r.lowerEye == 0.0,
              "F97a: the rifle standing, armed: F93's 11 cm raise (the leaning stance upright under the eye), nothing lowered");
        const f97::ArmedTarget tool = standing(1.543, 1.590 - kH, 1.590 + kUp, kH, 0.0, 1.0);
        check(within(tool.y, 1.590 + kUp - kH, 1e-12) && within(tool.y - 1.543, -0.023, 1e-12),
              "F97a: the Energylink upright, armed: lowered 2.3 cm, only as far as keeps the eye at the hip 15.5 cm over the neck (F93 lowered 10.8 cm: the knees bent)");
        const f97::ArmedTarget c = standing(1.040, 1.042 - 0.10, 1.042 + kUp, kH, 0.0, 1.0);
        check(within((1.042 + kUp) - c.y, kH, 1e-12) && c.y < 1.040,
              "F97a: the rifle crouched with the eye level with the neck: the body goes down until the eye at the hip is 15.5 cm over the neck (out of the torso)");
        const f97::ArmedTarget ads = standing(1.040, 1.040, 1.042 + kUp, kH, 1.0, 1.0);
        check(ads.y == 1.040 && ads.lowerEye == 0.0, "F97a: in ADS standing (detached, F73) the lowering is released with the detach: the authored neck");
        const f97::ArmedTarget half = standing(1.300, 1.410, 1.56 + kUp, kH, 0.0, 0.5);
        check(within(half.y, 1.355, 1e-12), "F97a: halfway through a draw (w 0.5) the body is half raised");
        const f97::ArmedTarget nan = standing(1.300, std::nan(""), std::nan(""), kH, 0.0, 1.0);
        check(nan.y == 1.300, "F97a: a target and an eye that are not numbers: the animated neck");
    }
    // ---- F161: crouched, the lowering holds through the ADS detach --------------------------------------------------------------------------------------------------------
    {
        // the flight's crouch-walk (edvr_gfx_20261028_135901): the walk lifts the animated neck over the eye (the neck 3.5 cm over the eye at 0.95 m, where the lowering is 19 cm);
        // F93's target for the rifle is the rest target (the eye 15.5 cm over it: prePhys, the lowering itself), for the other items the crouched animation's (the eye 6.6 cm over
        // it: preAnim, a lesser fall)
        const double n0 = 0.985, eyeHip = 0.95, prePhys = eyeHip - kH, preAnim = eyeHip - 0.066, pre = prePhys;
        const double lowering = (eyeHip - kH) - n0;
        // standing ADS stays as flown: the earlier formula's every figure, bit for bit, across the detach and the weights (no crouch weight, any call's shift)
        bool same = true;
        for (const double w : {0.0, 0.25, 1.0})
            for (const double shift : {0.0, 0.012, -0.03})
                for (const double dw : {0.0, 0.05, 0.4, 0.5, 0.97, 1.0})
                    for (const double tPre : {0.80, 0.884, 0.985, 1.150, 1.31, 1.40}) {
                        Call97 call{n0, tPre, shift, eyeHip, kH, dw, w, 0.0};
                        const f97::ArmedTarget a = call.run(), b = armedNeckTargetBeforeF161(n0, call.t93(), eyeHip, kH, dw, w);
                        same = same && a.y == b.y && a.asked == b.asked && a.lowerEye == b.lowerEye && a.raiseF93 == b.raiseF93;
                    }
        check(same, "F161: a standing stance (crouch weight 0) is the earlier formula bit for bit, at every detach weight, weapon weight and shift");
        const f97::ArmedTarget st = Call97{n0, pre, 0.0, eyeHip, kH, 1.0, 1.0, 0.0}.run();
        check(within(st.y, n0, 1e-12) && within(st.asked, 0.0, 1e-12), "F161: standing in ADS the lowering is released with the detach: the authored neck (as flown)");
        // crouched at the hip: the earlier formula, whatever the crouch weight (the detach holds nothing yet)
        bool hip = true;
        for (const double cw : {0.0, 0.3, 0.5, 1.0}) {
            const f97::ArmedTarget a = Call97{n0, pre, 0.01, eyeHip, kH, 0.0, 1.0, cw}.run(), b = armedNeckTargetBeforeF161(n0, pre + 0.01, eyeHip, kH, 0.0, 1.0);
            hip = hip && a.y == b.y && a.asked == b.asked;
        }
        check(hip, "F161: at the hip (detach 0) the demand is the earlier formula's at every crouch weight (nothing changes where it was flown)");
        // crouched in ADS: the eye's whole lowering
        const f97::ArmedTarget cr = Call97{n0, pre, 0.0, eyeHip, kH, 1.0, 1.0, 1.0}.run();
        check(lowering < -0.15 && within(cr.y, eyeHip - kH, 1e-12) && within(cr.y - n0, lowering, 1e-12),
              "F161: crouched in ADS the body keeps the lowering: the neck target stays where the eye at the hip is 15.5 cm over it (the flight's walk: 19 cm down)");
        const f97::ArmedTarget atHip = Call97{n0, pre, 0.0, eyeHip, kH, 0.0, 1.0, 1.0}.run();
        check(atHip.y < n0 && within(cr.y, atHip.y, 1e-12) && cr.y < st.y - 0.15,
              "F161: the rifle's crouched ADS body stands where it stood at the hip, 19 cm lower than the standing stance's in ADS (the one the flight showed)");
        // the rest target's path and the pin's own meet in ADS: the demand does not depend on the target the caller holds
        const f97::ArmedTarget other = Call97{n0, preAnim, 0.0, eyeHip, kH, 1.0, 1.0, 1.0}.run();
        check(within(other.y, cr.y, 1e-12), "F161: at full detach a crouched stance's neck target does not depend on F93's target (the pin's animated one or the rifle's rest one): the paths meet");
        // continuity across the detach (the press and the release): the neck target moves a few mm a step of 1 percent in dw, one way, from the hip's figure to the whole lowering
        // (the animated target's hip figure is a lesser fall: the body goes down 9 cm over the detach)
        double prev = Call97{n0, preAnim, 0.0, eyeHip, kH, 0.0, 1.0, 1.0}.run().y, maxStep = 0.0;
        const double hipAnim = prev;
        bool mono = true;
        for (int i = 1; i <= 100; ++i) {
            const double y = Call97{n0, preAnim, 0.0, eyeHip, kH, i / 100.0, 1.0, 1.0}.run().y;
            maxStep = std::fmax(maxStep, std::fabs(y - prev));
            mono = mono && y <= prev + 1e-12;
            prev = y;
        }
        check(within(hipAnim - cr.y, 0.089, 1e-9) && maxStep < 0.003 && mono && within(prev, cr.y, 1e-12),
              "F161: crouched, the detach 0 to 1 moves the neck target smoothly and one way (under 3 mm a percent), from the hip's figure to the whole lowering");
        prev = Call97{n0, prePhys, 0.0, eyeHip, kH, 0.0, 1.0, 1.0}.run().y;
        maxStep = 0.0;
        for (int i = 1; i <= 100; ++i) {
            const double y = Call97{n0, prePhys, 0.0, eyeHip, kH, i / 100.0, 1.0, 1.0}.run().y;
            maxStep = std::fmax(maxStep, std::fabs(y - prev));
            prev = y;
        }
        check(maxStep < 1e-12 && within(prev, cr.y, 1e-12), "F161: the rifle's rest target at the hip already asks the whole lowering: the detach 0 to 1 moves the neck target not at all");
        // continuity across the stance: the crouch weight 0 to 1 at full detach
        prev = Call97{n0, pre, 0.0, eyeHip, kH, 1.0, 1.0, 0.0}.run().y;
        maxStep = 0.0;
        for (int i = 1; i <= 100; ++i) {
            const double y = Call97{n0, pre, 0.0, eyeHip, kH, 1.0, 1.0, i / 100.0}.run().y;
            maxStep = std::fmax(maxStep, std::fabs(y - prev));
            prev = y;
        }
        check(maxStep < 0.003 && within(prev, cr.y, 1e-12), "F161: in ADS the crouch weight 0 to 1 moves the neck target under 3 mm a percent, from the authored neck to the whole lowering");
        const f97::ArmedTarget mid = Call97{n0, pre, 0.0, eyeHip, kH, 1.0, 1.0, 0.5}.run();
        check(within(mid.y, 0.5 * (n0 + cr.y), 1e-12), "F161: half crouched in ADS is half way between the authored neck and the whole lowering");
        const f97::ArmedTarget weak = Call97{n0, pre, 0.0, eyeHip, kH, 1.0, 0.5, 1.0}.run();
        check(within(weak.y, n0 + 0.5 * lowering, 1e-12), "F161: the weapon weight still blends the body (w 0.5: half the lowering)");
        const f97::ArmedTarget oddCw = Call97{n0, pre, 0.0, eyeHip, kH, 1.0, 1.0, std::nan("")}.run(), over = Call97{n0, pre, 0.0, eyeHip, kH, 1.0, 1.0, 7.0}.run();
        check(oddCw.y == st.y && over.y == cr.y, "F161: a crouch weight that is not a number is standing; one over 1 is crouched");
        const f97::ArmedTarget noHip = f97::armedNeckTarget(n0, n0, eyeHip, kH, 1.0, 1.0, 1.0, std::nan(""));
        check(within(noHip.y, cr.y, 1e-12), "F161: without the hip's target (not a number) the crouched stance still ends at the whole lowering at full detach");
        // an eye that is already high (nothing to lower): the crouched stance releases like the standing one
        const f97::ArmedTarget high = Call97{n0, pre, 0.0, 1.50, kH, 1.0, 1.0, 1.0}.run();
        check(within(high.y, n0, 1e-12), "F161: with the eye already 15.5 cm over the neck there is no lowering to hold: the authored neck");
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
            const double y = standing(n0, cam - kH, cam + kUp * w, kH, 0.0, w).y;
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
