// F97a (native on-foot VR): the armed body under the eye again. Header only, no game or D3D11 state, so native_frame_test runs the very functions the Animate
// post-hook uses. Journal: the arc doc, 2026-10-24, F97a.
//
// F94a held the commander's neck at its animated height in every stance and placed the eye from the neck unarmed and from the weapon armed. Armed, that left the
// body where the animation has it under an eye the weapon eye offset places: with the rifle the leaning stance's neck stands at 1.30 m, so the shoulders sat about 9
// cm lower against the eye than in F93 (the eye over the clavicle line +27.4 cm against +18.2 in the F93 flight, the pin's eye), and crouched and crouch-moving the
// eye reached the torso (INSIDE 164 crouched and 1458 in the rifle's airborne-classed seconds, against 0 in F93), because the body no longer followed the camera
// when the character's model moves under it. F93 pinned the armed body under the camera every frame (the rest offset below the pin's eye: raised for the leaning
// stances, lowered for the rest).
//
// F97a, armed (F88's weight w above 0), the neck target's height is F93's again where F93 raised the body (the leaning stances stand upright under the eye), and
// it is lowered only as far as keeps the eye as drawn at the hip (the pin's eye plus, since F98c, the neck reference's raise the pose carried; F97a had the weapon
// eye offset's up) the eye height key above the neck: an upright stance whose eye is already that high (a tool) keeps its legs, and when the model moves under
// the camera the body goes down with the eye. The ADS detach (F73) releases the
// lowering with it. Unarmed (w 0) the target is the animated neck's height, F94a's, exactly; between the two it is blended by w, the same weight and ramp as the
// eye's, so a draw or a holster moves the body and the view together. A raise is capped where a leg would have to lift its foot (the hip at most the leg's reach
// over the foot, the planting rule's own reach), so the feet stay on the ground.
#pragma once

#include <cmath>
#include <cstdint>

namespace edvr {
namespace nofoot {
namespace f97 {

struct ArmedTarget {
    double y = 0.0;         // the neck target's height
    double raiseF93 = 0.0;  // F93's vertical demand (its target less the animated neck; > 0 raises)
    double lowerEye = 0.0;  // the lowering the eye's clearance asks (<= 0)
    double asked = 0.0;     // the armed demand before the weight (max of F93's raise and the eye's lowering)
};
// n0y: the animated neck's height; t93y: F93's target height (the pin's eye less the neck-to-eye vector, after the ADS detach and the weapon view offset);
// eyeHipLevelY: the eye as drawn at the hip with the head level (the pin's eye, head pitch out, plus w times the weapon eye offset's up); eyeHeightM: the key;
// detachW: F73's ADS detach weight (the lowering is released with it); w: F88's weight.
inline ArmedTarget armedNeckTarget(double n0y, double t93y, double eyeHipLevelY, double eyeHeightM, double detachW, double w) noexcept {
    ArmedTarget o;
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

// The largest rise of the hips that keeps a planted foot on the ground: legPlant lifts a foot when the hip stands more than rMax over it (rMax: kStandReach of the
// leg, or the animated leg's own distance when that is longer, below 0.9995 of the leg), so the rise is rMax less the hip's height over the foot now.
inline double legRaiseRoom(double hipY, double footY, double l1, double l2, double animDist, double standReach) noexcept {
    const double L = l1 + l2;
    double rMax = standReach * L;
    if (animDist == animDist && animDist > 0.0) rMax = std::fmax(rMax, std::fmin(animDist, 0.9995 * L));
    const double room = rMax - (hipY - footY) - 1e-4;
    return std::isfinite(room) ? room : 0.0;
}

// The line's stance bins: standing, crouched still, crouched and moving, moving (or airborne) upright.
enum Stance : int { kStanding = 0, kCrouched = 1, kCrouchMoving = 2, kMoving = 3, kStanceN = 4 };
inline int stanceBin(bool crouched, bool moving) noexcept { return crouched ? (moving ? kCrouchMoving : kCrouched) : (moving ? kMoving : kStanding); }
inline const char* stanceName(int s) noexcept {
    return s == kStanding ? "standing" : s == kCrouched ? "crouched" : s == kCrouchMoving ? "moving crouched" : "moving";
}

}  // namespace f97
}  // namespace nofoot
}  // namespace edvr
