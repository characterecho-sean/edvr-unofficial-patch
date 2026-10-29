// F168 (native on foot VR, TEMPORARY): a dial for the crouched body's distance to the gun, and the figures that judge it. Header only, no game or D3D11 state, so native_frame_test runs the
// very functions the pin and the frame thread use. Journal: the arc doc, F168.
//
// The pilot, after F166: crouched in the sights and walking, "the body is positioned too far back from the butt of the gun, if we move it closer during a crouch that should help the left
// arm". The dumps put the left upper arm near the eye with the forearm reaching forward to the foregrip. The dial moves the third-person body along the aim's horizontal forward, relative to
// the eye, by experimental.native_on_foot_crouch_ads_body_forward_cm (-20..+20, positive toward the gun). The body moves with its arms while the eye and the gun stay where the sight lock
// puts them. It is scaled by the crouched weight (F163) and the detach weight (the sights), so it is exactly 0 standing and at the hip. F168 put it in the body pin's target (the neck's);
// F169 moved a positive dial past the reach rule (below), which had absorbed it.
//
// The figures (per second, crouched in the sights, still and moving): the move applied, the right shoulder to the gun's root, the left shoulder to the left-hand grip and the left shoulder
// from the eye. The gun's butt is not a joint the code can read: the FP rig's right handAttach (51), the point the weapon hangs from, stands for it, taken through G into the model.
#pragma once

#include "native_on_foot_core.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace edvr {
namespace nofoot {
namespace f168 {

constexpr double kDialMinCm = -20.0, kDialMaxCm = 20.0;   // the key's range
constexpr double kMinForwardXz = 0.1;                      // an aim whose horizontal part is under this has no forward to move along
constexpr uint32_t kMinFrames = 30;                        // the health rule: fewer crouched-in-the-sights frames than this are too few to judge

// The key's value held to its range; a value that is not a number reads 0 (the body unmoved).
inline double clampDialCm(double cm) noexcept { return !std::isfinite(cm) ? 0.0 : cm < kDialMinCm ? kDialMinCm : cm > kDialMaxCm ? kDialMaxCm : cm; }

// The body's move toward the gun, m (signed): the dial x the crouched weight x the detach weight, each held to its range. Exactly 0 when the dial, the posture or the detach is 0.
inline double shiftM(double dialCm, double postureW, double detachW) noexcept { return clampDialCm(dialCm) * 0.01 * unit01(postureW) * unit01(detachW); }

// The move as a vector in the model's axes: along the horizontal part of the aim's forward (fwd, any length), none vertical. False and a zero vector when the move is 0 or the aim has no
// horizontal forward (straight up or down, or not a number): the body is then left where the pin put it.
inline bool shiftVector(double dialCm, double postureW, double detachW, const double fwd[3], double out[3]) noexcept {
    out[0] = out[1] = out[2] = 0.0;
    const double m = shiftM(dialCm, postureW, detachW);
    if (!(m != 0.0) || !fwd) return false;
    const double hn = std::sqrt(fwd[0] * fwd[0] + fwd[2] * fwd[2]);
    if (!std::isfinite(hn) || !(hn >= kMinForwardXz)) return false;
    out[0] = fwd[0] / hn * m;
    out[2] = fwd[2] / hn * m;
    return true;
}

// ---- F169: where the move is made --------------------------------------------------------------------------------------------------------------------------------------------
//
// The flight edvr_gfx_20261028_201339 (build df917db6): the pilot dialled the move 0 to +17 cm crouched in the sights, standing still, and saw no change. The body pin's horizontal move
// stayed at +18.4 to +18.7 cm at every value, while the target less the neck as written went from -18.4 to -2.0 cm. The reach rule (the least move that brings the left shoulder within
// arm's length less 2 cm of the foregrip) is made on the move the target asks for, and the dial was part of the target: a forward target only shortened the reach rule's own move by the same
// amount, and the body stayed at the reach boundary, the left arm nearly straight, until the dial passed about 18.5 cm. A positive dial now moves the written neck after the reach rule, on
// top of the boundary. A negative dial stays in the target, before it (the grip must still be reachable: the reach rule gives back what it needs), and the move as written is measured
// against the same call with the dial at 0.

// True when the dial's move is made after the reach rule (a positive dial); a negative dial or none is part of the target the reach rule works from.
inline bool afterReach(double dialCm) noexcept { return clampDialCm(dialCm) > 0.0; }

// The vector split around the reach rule: before it (the negative dial's), after it (the positive dial's); the other is exactly zero. A vector that is not made (all zero) splits to zeros.
inline void splitShift(double dialCm, const double shift[3], double pre[3], double post[3]) noexcept {
    const bool after = afterReach(dialCm);
    for (int i = 0; i < 3; ++i) {
        pre[i] = after ? 0.0 : shift[i];
        post[i] = after ? shift[i] : 0.0;
    }
}

// The move after the reach rule: added to the written horizontal move th (model x, y, z) and to the reach correction the bend and the torso yaw aim at (reach), so they see the neck where
// it is written. The vertical part is left alone. Exactly nothing for a zero vector.
inline void addAfterReach(double th[3], double reach[3], const double post[3]) noexcept {
    if (post[0] == 0.0 && post[2] == 0.0) return;
    th[0] += post[0];
    th[2] += post[2];
    reach[0] += post[0];
    reach[2] += post[2];
}

// The dial's move as written along the aim's horizontal forward (m, signed): the part made before the reach rule (pre), the part after (post) and the share of the reach rule's correction
// that changed with it (reachDelta: the correction with the dial less the correction at dial 0, already scaled as the pin scales it). Not a number without a horizontal forward.
inline double writtenAlongM(const double pre[3], const double post[3], const double reachDelta[3], const double fwd[3]) noexcept {
    if (!fwd) return std::nan("");
    const double hn = std::sqrt(fwd[0] * fwd[0] + fwd[2] * fwd[2]);
    if (!std::isfinite(hn) || !(hn >= kMinForwardXz)) return std::nan("");
    const double dx = pre[0] + post[0] + reachDelta[0], dz = pre[2] + post[2] + reachDelta[2];
    const double r = (dx * fwd[0] + dz * fwd[2]) / hn;
    return std::isfinite(r) ? r : std::nan("");
}

// ---- the figures --------------------------------------------------------------------------------------------------------------------------------------------------------------

// One call crouched in the sights (the caller files only those): the dial, the move asked this call, the move as written (F169), the distances (m; not numbers when unmeasured) and the
// left shoulder from the eye.
struct Sample {
    bool moving = false;
    double dialCm = 0.0;
    double shiftM = 0.0;
    double writtenM = std::nan("");   // F169: the neck's forward change from the dial-0 body, after the reach rule (not a number: unmeasured)
    double rightShoulderToGunM = std::nan(""), leftShoulderToGripM = std::nan("");
    double leftShoulderM[3] = {std::nan(""), std::nan(""), std::nan("")};   // right, up, forward of the eye (the heading's axes)
};
struct Counters {
    uint32_t ads = 0, still = 0, moving = 0;
    double dialSum = 0.0, dialAbsMax = 0.0;
    uint32_t shiftN[2] = {}, gunN[2] = {}, gripN[2] = {}, eyeN[2] = {}, writtenN[2] = {};   // per kind: still (0), moving (1)
    double shiftSum[2] = {}, gunSum[2] = {}, gripSum[2] = {}, eyeSum[2][3] = {}, writtenSum[2] = {};
    void add(const Sample& s) noexcept {
        ++ads;
        const int i = s.moving ? 1 : 0;
        if (s.moving) ++moving; else ++still;
        if (std::isfinite(s.dialCm)) {
            dialSum += s.dialCm;
            dialAbsMax = std::fabs(s.dialCm) > dialAbsMax ? std::fabs(s.dialCm) : dialAbsMax;
        }
        if (std::isfinite(s.shiftM)) { ++shiftN[i]; shiftSum[i] += s.shiftM; }
        if (std::isfinite(s.writtenM)) { ++writtenN[i]; writtenSum[i] += s.writtenM; }
        if (std::isfinite(s.rightShoulderToGunM)) { ++gunN[i]; gunSum[i] += s.rightShoulderToGunM; }
        if (std::isfinite(s.leftShoulderToGripM)) { ++gripN[i]; gripSum[i] += s.leftShoulderToGripM; }
        if (std::isfinite(s.leftShoulderM[0]) && std::isfinite(s.leftShoulderM[1]) && std::isfinite(s.leftShoulderM[2])) {
            ++eyeN[i];
            for (int c = 0; c < 3; ++c) eyeSum[i][c] += s.leftShoulderM[c];
        }
    }
    void merge(const Counters& o) noexcept {
        ads += o.ads; still += o.still; moving += o.moving;
        dialSum += o.dialSum;
        dialAbsMax = o.dialAbsMax > dialAbsMax ? o.dialAbsMax : dialAbsMax;
        for (int i = 0; i < 2; ++i) {
            shiftN[i] += o.shiftN[i]; shiftSum[i] += o.shiftSum[i];
            writtenN[i] += o.writtenN[i]; writtenSum[i] += o.writtenSum[i];
            gunN[i] += o.gunN[i]; gunSum[i] += o.gunSum[i];
            gripN[i] += o.gripN[i]; gripSum[i] += o.gripSum[i];
            eyeN[i] += o.eyeN[i];
            for (int c = 0; c < 3; ++c) eyeSum[i][c] += o.eyeSum[i][c];
        }
    }
};

// ---- the line -----------------------------------------------------------------------------------------------------------------------------------------------------------------

// The window's line: the item; the dial's mean and largest value over the frames crouched in the sights; those frames, still and moving; the move asked (cm, still and moving); F169: the
// move as written (the neck's forward change from the same body with the dial at 0, after the reach rule; the part of a negative dial the reach rule gave back is not in it); the right
// shoulder to the gun's root, the left shoulder to the left-hand grip (cm, with the frames each is a mean of) and the left shoulder from the eye, still and moving. The log's lines end at
// 1200 bytes; this one runs to about 830.
inline int formatSummary(char* buf, size_t n, uint32_t seconds, const char* item, const Counters& c) noexcept {
    const double an = c.ads ? static_cast<double>(c.ads) : 1.0;
    const auto mean = [](double sum, uint32_t k) noexcept { return k ? sum / static_cast<double>(k) : 0.0; };
    return std::snprintf(buf, n,
                         "native on foot crouched ADS body (F168, TEMPORARY) %u s: item %s; the dial experimental.native_on_foot_crouch_ads_body_forward_cm mean %+.1f cm (largest %.1f); "
                         "crouched in the sights %u frames (still %u, moving %u); the body moved toward the gun (the dial x the crouched weight x the detach weight) %+.1f cm still, %+.1f "
                         "moving; as written (the neck's forward change from the dial-0 body, after the reach rule; %u/%u frames) %+.1f cm still, %+.1f moving; "
                         "right shoulder to the gun's root (the FP right handAttach 51 through G: the butt is not a joint the code reads) %u frames still %.1f cm, %u moving %.1f; "
                         "left shoulder to the left-hand grip (the FP left handAttach 52 through G) %u frames still %.1f cm, %u moving %.1f; the left shoulder from the eye (right/up/forward, "
                         "cm): still %u frames %+.1f/%+.1f/%+.1f, moving %u frames %+.1f/%+.1f/%+.1f",
                         seconds, item ? item : "none", c.dialSum / an, c.dialAbsMax, c.ads, c.still, c.moving, mean(c.shiftSum[0], c.shiftN[0]) * 100.0, mean(c.shiftSum[1], c.shiftN[1]) * 100.0,
                         c.writtenN[0], c.writtenN[1], mean(c.writtenSum[0], c.writtenN[0]) * 100.0, mean(c.writtenSum[1], c.writtenN[1]) * 100.0,
                         c.gunN[0], mean(c.gunSum[0], c.gunN[0]) * 100.0, c.gunN[1], mean(c.gunSum[1], c.gunN[1]) * 100.0, c.gripN[0], mean(c.gripSum[0], c.gripN[0]) * 100.0, c.gripN[1],
                         mean(c.gripSum[1], c.gripN[1]) * 100.0, c.eyeN[0], mean(c.eyeSum[0][0], c.eyeN[0]) * 100.0, mean(c.eyeSum[0][1], c.eyeN[0]) * 100.0,
                         mean(c.eyeSum[0][2], c.eyeN[0]) * 100.0, c.eyeN[1], mean(c.eyeSum[1][0], c.eyeN[1]) * 100.0, mean(c.eyeSum[1][1], c.eyeN[1]) * 100.0,
                         mean(c.eyeSum[1][2], c.eyeN[1]) * 100.0);
}

}  // namespace f168
}  // namespace nofoot
}  // namespace edvr
