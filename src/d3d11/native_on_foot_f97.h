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
// lowering with it for a standing stance. Unarmed (w 0) the target is the animated neck's height, F94a's, exactly; between the two it is blended by w, the same weight and ramp as the
// eye's, so a draw or a holster moves the body and the view together. A raise is capped where a leg would have to lift its foot (the hip at most the leg's reach
// over the foot, the planting rule's own reach), so the feet stay on the ground.
//
// F161: crouched, the lowering holds through the ADS detach. The flight edvr_gfx_20261028_135901 (dumps 140218592001 and 140223977002, the plasma AR, crouch-walking in ADS): the
// release put the body back at its authored height under the eye, and the walk animation lifts the left shoulder about 23 cm up and 12 cm forward, so the eye sat 2 to 11.5 cm
// under the clavicle line (against 17.6 cm over it at the hip) and the left upper arm and the elbow's sleeve passed 1.3 to 4.7 cm from the eye, cut by the 2.5 cm near plane in
// both eyes. The ADS detach released the lowering twice: F93's target is moved to the authored neck by the detach weight before this function sees it, and the eye's lowering
// was multiplied by (1 - dw); the demand is the larger of the two (the lesser fall), so keeping the second alone changes nothing at full detach (F93's raise is then zero and
// wins). A crouched stance asks, as the detach rises, the eye's whole lowering in place of the hip's demand (F93's target before the detach and the weapon view offset, t93HipY,
// against the eye's lowering): from the hip's at dw 0 to the lowering at dw 1, and the standing demand (released by dw, as before) is blended into it by the camera's crouch
// weight (F161: the pin's w_c, smoothstep over 20 to 45 cm down; F163: the animated neck's drop under the standing neck, smoothstep over 33 to 43 cm, native_on_foot_f163.h, because w_c is 0 on
// every call without the standing calibration). A standing stance (weight 0) is the earlier formula bit for bit; at the hip (dw 0) the demand is the hip's whatever the
// stance, and at dw 1 it does not depend on which target the caller holds (the pin's animated target, or the rifle's rest target), so the caller's two paths meet.
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
// detachW: F73's ADS detach weight (it releases the demand of the standing share of the stance); w: F88's weight; crouchedW: the crouched weight (0
// standing to 1 crouched; F161; F163: the animated neck's drop, native_on_foot_f163.h); t93HipY: t93y as it is with no detach (the same target before the detach, the weapon view offset added), the hip's demand that a crouched
// stance leaves for the whole lowering as the detach rises (not finite: t93y).
inline ArmedTarget armedNeckTarget(double n0y, double t93y, double eyeHipLevelY, double eyeHeightM, double detachW, double w, double crouchedW, double t93HipY) noexcept {
    ArmedTarget o;
    o.y = n0y;
    const double wc = !(w > 0.0) ? 0.0 : w > 1.0 ? 1.0 : w;
    if (!std::isfinite(n0y) || wc == 0.0) return o;
    const double dw = !(detachW > 0.0) ? 0.0 : detachW > 1.0 ? 1.0 : detachW;
    const double cw = !(crouchedW > 0.0) ? 0.0 : crouchedW > 1.0 ? 1.0 : crouchedW;
    o.raiseF93 = std::isfinite(t93y) ? t93y - n0y : 0.0;
    const double low = std::isfinite(eyeHipLevelY) && std::isfinite(eyeHeightM) ? (eyeHipLevelY - eyeHeightM) - n0y : 0.0;
    const double lowering = low < 0.0 ? low : 0.0;
    o.lowerEye = lowering * (1.0 - dw);
    o.asked = o.raiseF93 > o.lowerEye ? o.raiseF93 : o.lowerEye;
    if (cw > 0.0) {   // F161: the crouched share: the hip's demand, becoming the eye's whole lowering with the detach
        const double raiseHip = std::isfinite(t93HipY) ? t93HipY - n0y : o.raiseF93;
        const double askedHip = raiseHip > lowering ? raiseHip : lowering;
        const double askedCrouched = askedHip + (lowering - askedHip) * dw;
        o.asked += (askedCrouched - o.asked) * cw;
        o.lowerEye = lowering * (1.0 - dw * (1.0 - cw));
    }
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
