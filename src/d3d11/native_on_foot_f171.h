// F171 (native on foot VR, TEMPORARY): the crouched ADS body moved to the left of the aim line, and the figures that judge it. Header only, no game or D3D11 state, so native_frame_test runs
// the very functions the pin and the frame thread use. Journal: the arc doc, F171.
//
// The pilot, after F170: crouched in the sights the gun still ends up in the left shoulder; "do some sort of offset to the left of the gun while in ADS and crouched". The flight
// edvr_gfx_20261029_043951 (build d64b85ac), the eye dump at the square dial 0 and the body forward dial +7 cm: the left shoulder from the eye -0.9 right / -14.6 up / -4.0 forward cm (almost
// under the eye), the right shoulder to the gun's root 45.9 cm, the left shoulder to the left grip 57.2 cm. The body has to stand about 15 to 20 cm to the left of the aim line for the stock to
// meet the right shoulder.
//
// The move: the dial (experimental.native_on_foot_crouch_ads_body_left_cm, -20..+20 cm, positive to the LEFT of the aim line) x F163's posture weight x the detach weight, along the horizontal
// left of the aim's forward (perpendicular to it in the horizontal plane, none vertical). It goes into the pin's target for the neck BEFORE the reach rule, as F168's negative dial does, so the
// reach rule keeps the left grip reachable and gives back what the grip needs; a sideways move barely lengthens the line from the left shoulder to the grip, so most of it should survive. The
// move as written is measured against the same call with the dial at 0 (the real reach rule run both ways), and what the reach rule took back is the difference to the move asked.
//
// Exactly nothing when the dial is 0, standing (posture weight 0), at the hip (detach weight 0), unarmed, or for an aim with no horizontal forward.
#pragma once

#include "native_on_foot_core.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace edvr {
namespace nofoot {
namespace f171 {

constexpr double kDialMinCm = -20.0, kDialMaxCm = 20.0;   // the key's range
constexpr double kMinForwardXz = 0.1;                      // an aim whose horizontal part is under this has no forward to be left of
constexpr uint32_t kMinFrames = 30;                        // the health rule: fewer crouched-in-the-sights frames than this are too few to judge

// The key's value held to its range; a value that is not a number reads 0 (the body unmoved).
inline double clampDialCm(double cm) noexcept { return !std::isfinite(cm) ? 0.0 : cm < kDialMinCm ? kDialMinCm : cm > kDialMaxCm ? kDialMaxCm : cm; }

// The move to the left of the aim, m (signed, positive left): the dial x the crouched weight x the detach weight, each held to its range. Exactly 0 when the dial, the posture or the detach is 0.
inline double shiftM(double dialCm, double postureW, double detachW) noexcept { return clampDialCm(dialCm) * 0.01 * unit01(postureW) * unit01(detachW); }

// The unit vector to the left of the aim's horizontal forward, in the model's axes (x right, y up, z forward, as the pin's heading axes read them: headingAxes() takes the eye's right as
// v.x*h.z - v.z*h.x, so the left of a heading (hx, hz) is (-hz, hx)). False and a zero vector for an aim with no horizontal forward (straight up or down, or not a number).
inline bool leftAxis(const double fwd[3], double out[3]) noexcept {
    out[0] = out[1] = out[2] = 0.0;
    if (!fwd) return false;
    const double hn = std::sqrt(fwd[0] * fwd[0] + fwd[2] * fwd[2]);
    if (!std::isfinite(hn) || !(hn >= kMinForwardXz)) return false;
    out[0] = -fwd[2] / hn;
    out[2] = fwd[0] / hn;
    return true;
}

// The move as a vector in the model's axes: along the left of the aim, none vertical. False and a zero vector when the move is 0 or the aim has no horizontal forward: the body is then left
// where the pin put it.
inline bool shiftVector(double dialCm, double postureW, double detachW, const double fwd[3], double out[3]) noexcept {
    out[0] = out[1] = out[2] = 0.0;
    const double m = shiftM(dialCm, postureW, detachW);
    if (!(m != 0.0)) return false;
    double left[3];
    if (!leftAxis(fwd, left)) return false;
    out[0] = left[0] * m;
    out[2] = left[2] * m;
    return true;
}

// The dial's move as written along the left of the aim (m, signed): the move the target carried (shift) plus the share of the reach rule's correction that changed with it (reachDelta: the
// correction with the move less the correction without it, already scaled as the pin scales it). Not a number without a horizontal forward.
inline double writtenLeftM(const double shift[3], const double reachDelta[3], const double fwd[3]) noexcept {
    double left[3];
    if (!leftAxis(fwd, left)) return std::nan("");
    const double r = (shift[0] + reachDelta[0]) * left[0] + (shift[2] + reachDelta[2]) * left[2];
    return std::isfinite(r) ? r : std::nan("");
}

// ---- the figures --------------------------------------------------------------------------------------------------------------------------------------------------------------

// One call crouched in the sights (the caller files only those): the dial, whether the move was made, the move asked (m, signed, left positive) and as written (not a number: unmeasured),
// the shoulders from the eye as drawn (right, up, forward, m; not numbers when unmeasured) and the distances to the gun's root and the left grip (m; not numbers when unmeasured).
struct Sample {
    bool moving = false;
    bool made = false;
    double dialCm = 0.0;
    double askedM = 0.0;
    double writtenM = std::nan("");
    double leftShoulderM[3] = {std::nan(""), std::nan(""), std::nan("")};
    double rightShoulderM[3] = {std::nan(""), std::nan(""), std::nan("")};
    double rightShoulderToGunM = std::nan(""), leftShoulderToGripM = std::nan("");
};
struct Counters {
    uint32_t ads = 0, still = 0, moving = 0, made = 0;
    double dialSum = 0.0, dialAbsMax = 0.0;
    uint32_t askedN[2] = {}, writtenN[2] = {}, backN[2] = {}, leftN[2] = {}, rightN[2] = {}, gunN[2] = {}, gripN[2] = {};   // per kind: still (0), moving (1)
    double askedSum[2] = {}, writtenSum[2] = {}, backSum[2] = {}, leftSum[2][3] = {}, rightSum[2][3] = {}, gunSum[2] = {}, gripSum[2] = {};
    void add(const Sample& s) noexcept {
        ++ads;
        const int i = s.moving ? 1 : 0;
        if (s.moving) ++moving; else ++still;
        if (s.made) ++made;
        if (std::isfinite(s.dialCm)) {
            dialSum += s.dialCm;
            dialAbsMax = std::fabs(s.dialCm) > dialAbsMax ? std::fabs(s.dialCm) : dialAbsMax;
        }
        if (std::isfinite(s.askedM)) { ++askedN[i]; askedSum[i] += s.askedM; }
        if (std::isfinite(s.writtenM)) { ++writtenN[i]; writtenSum[i] += s.writtenM; }
        if (std::isfinite(s.askedM) && std::isfinite(s.writtenM)) { ++backN[i]; backSum[i] += s.askedM - s.writtenM; }
        if (std::isfinite(s.leftShoulderM[0]) && std::isfinite(s.leftShoulderM[1]) && std::isfinite(s.leftShoulderM[2])) {
            ++leftN[i];
            for (int c = 0; c < 3; ++c) leftSum[i][c] += s.leftShoulderM[c];
        }
        if (std::isfinite(s.rightShoulderM[0]) && std::isfinite(s.rightShoulderM[1]) && std::isfinite(s.rightShoulderM[2])) {
            ++rightN[i];
            for (int c = 0; c < 3; ++c) rightSum[i][c] += s.rightShoulderM[c];
        }
        if (std::isfinite(s.rightShoulderToGunM)) { ++gunN[i]; gunSum[i] += s.rightShoulderToGunM; }
        if (std::isfinite(s.leftShoulderToGripM)) { ++gripN[i]; gripSum[i] += s.leftShoulderToGripM; }
    }
    void merge(const Counters& o) noexcept {
        ads += o.ads; still += o.still; moving += o.moving; made += o.made;
        dialSum += o.dialSum;
        dialAbsMax = o.dialAbsMax > dialAbsMax ? o.dialAbsMax : dialAbsMax;
        for (int i = 0; i < 2; ++i) {
            askedN[i] += o.askedN[i]; askedSum[i] += o.askedSum[i];
            writtenN[i] += o.writtenN[i]; writtenSum[i] += o.writtenSum[i];
            backN[i] += o.backN[i]; backSum[i] += o.backSum[i];
            gunN[i] += o.gunN[i]; gunSum[i] += o.gunSum[i];
            gripN[i] += o.gripN[i]; gripSum[i] += o.gripSum[i];
            leftN[i] += o.leftN[i]; rightN[i] += o.rightN[i];
            for (int c = 0; c < 3; ++c) {
                leftSum[i][c] += o.leftSum[i][c];
                rightSum[i][c] += o.rightSum[i][c];
            }
        }
    }
};

// ---- the line -----------------------------------------------------------------------------------------------------------------------------------------------------------------

// The window's line: the item; the dial's mean and largest value over the frames crouched in the sights; those frames, still and moving, and those the move was made in; the move asked, as
// written (the neck's sideways change from the same body with the dial at 0, after the reach rule; with the frames each is a mean of) and what the reach rule took back of it (cm, still and
// moving); the right and the left shoulder from the eye (right/up/forward, cm); the right shoulder to the gun's root and the left shoulder to the left-hand grip. Left is positive. The log's
// lines end at 1200 bytes; this one runs to about 1000.
inline int formatSummary(char* buf, size_t n, uint32_t seconds, const char* item, const Counters& c) noexcept {
    const double an = c.ads ? static_cast<double>(c.ads) : 1.0;
    const auto mean = [](double sum, uint32_t k) noexcept { return k ? sum / static_cast<double>(k) : 0.0; };
    const auto eye = [&](const double sum[2][3], const uint32_t cnt[2], int i, int axis) noexcept { return mean(sum[i][axis], cnt[i]) * 100.0; };
    return std::snprintf(buf, n,
                         "native on foot crouched ADS body sideways (F171, TEMPORARY) %u s: item %s; the dial experimental.native_on_foot_crouch_ads_body_left_cm mean %+.1f cm (largest %.1f; "
                         "positive is left of the aim); crouched in the sights %u frames (still %u, moving %u), the move made in %u; the body moved left (the dial x the crouched weight x the "
                         "detach weight) %+.1f cm still, %+.1f moving; as written (the neck's sideways change from the dial-0 body, after the reach rule; %u/%u frames) %+.1f still, %+.1f moving; "
                         "the reach rule took back %+.1f still, %+.1f moving; right shoulder from the eye (right/up/forward, cm): still %u frames %+.1f/%+.1f/%+.1f, moving %u frames "
                         "%+.1f/%+.1f/%+.1f; left shoulder from the eye: still %u frames %+.1f/%+.1f/%+.1f, moving %u frames %+.1f/%+.1f/%+.1f; right shoulder to the gun's root "
                         "%u frames still %.1f cm, %u moving %.1f; left shoulder to the left-hand grip %u frames still %.1f cm, %u moving %.1f",
                         seconds, item ? item : "none", c.dialSum / an, c.dialAbsMax, c.ads, c.still, c.moving, c.made, mean(c.askedSum[0], c.askedN[0]) * 100.0,
                         mean(c.askedSum[1], c.askedN[1]) * 100.0, c.writtenN[0], c.writtenN[1], mean(c.writtenSum[0], c.writtenN[0]) * 100.0, mean(c.writtenSum[1], c.writtenN[1]) * 100.0,
                         mean(c.backSum[0], c.backN[0]) * 100.0, mean(c.backSum[1], c.backN[1]) * 100.0, c.rightN[0], eye(c.rightSum, c.rightN, 0, 0), eye(c.rightSum, c.rightN, 0, 1),
                         eye(c.rightSum, c.rightN, 0, 2), c.rightN[1], eye(c.rightSum, c.rightN, 1, 0), eye(c.rightSum, c.rightN, 1, 1), eye(c.rightSum, c.rightN, 1, 2), c.leftN[0],
                         eye(c.leftSum, c.leftN, 0, 0), eye(c.leftSum, c.leftN, 0, 1), eye(c.leftSum, c.leftN, 0, 2), c.leftN[1], eye(c.leftSum, c.leftN, 1, 0), eye(c.leftSum, c.leftN, 1, 1),
                         eye(c.leftSum, c.leftN, 1, 2), c.gunN[0], mean(c.gunSum[0], c.gunN[0]) * 100.0, c.gunN[1], mean(c.gunSum[1], c.gunN[1]) * 100.0, c.gripN[0],
                         mean(c.gripSum[0], c.gripN[0]) * 100.0, c.gripN[1], mean(c.gripSum[1], c.gripN[1]) * 100.0);
}

}  // namespace f171
}  // namespace nofoot
}  // namespace edvr
