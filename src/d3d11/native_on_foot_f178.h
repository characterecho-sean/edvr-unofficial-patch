// F178 (native on foot VR, TEMPORARY): the standing ADS body dials, the crouched ones (F168 forward, F171 left) mirrored for the standing stance, and the figures that judge them. Header only,
// no game or D3D11 state, so native_frame_test runs the very functions the pin and the frame thread use. Journal: the arc doc, F178.
//
// The pilot, after F173: the crouched fix (the body put to the left of the gun so the stock sits in the right shoulder, not the left) is wanted standing too, with rifles. Two dials in
// experimental, so the standing pose can be dialled in the headset and the values baked later: experimental.native_on_foot_stand_ads_body_forward_cm (-20..+20 cm, positive toward the
// gun) and experimental.native_on_foot_stand_ads_body_left_cm (-30..+30 cm, positive to the left of the aim line). Both shipped at 0 (nothing changed until the pilot moved them); F181 made the pilot's flown values the defaults: 4 cm forward and 23 cm left.
//
// They are the crouched pair's moves with the STANDING weight in place of the crouched one: (1 - F163's crouched weight) x the ADS detach weight. So a crouch blends one pair into the
// other: at the crouched weight 0 the standing pair is whole and the crouched pair is 0, at 1 the reverse, at 0.5 each is half; at the hip (detach 0) both are 0. The move itself is F168's
// and F171's own code (f168::shiftM, shiftVector, splitShift, afterReach, addAfterReach, writtenAlongM; f171::shiftM, shiftVector, writtenLeftM): their `postureW` argument is the weight
// of the stance the dial belongs to, and the standing pair hands them standingShare(postureW). The pin applies them in the same places in the same order as the crouched pair (the forward
// dial before the reach rule when negative and after it when positive, F169; the left dial before it), so the crouched pair is not touched by anything here: with the standing dials at 0
// no standing vector is made and every call is bit for bit what it was.
//
// The crouched weight is F163's (the animated neck's drop under the standing unarmed neck, smoothstep 0 to 1; no calibration needed, the commander's upright neck stands in for it), not
// the camera's w_c (F69a, which reads 0 without the standing calibration). A crouch without a standing calibration therefore reads crouched here (the weight 1 once the neck is 43 cm
// down), so it gets the crouched pair and none of the standing pair.
#pragma once

#include "native_on_foot_core.h"
#include "native_on_foot_f159.h"
#include "native_on_foot_f168.h"
#include "native_on_foot_f171.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace edvr {
namespace nofoot {
namespace f178 {

constexpr double kForwardMinCm = f168::kDialMinCm, kForwardMaxCm = f168::kDialMaxCm;   // the forward key's range: the crouched forward dial's
constexpr double kLeftMinCm = f171::kDialMinCm, kLeftMaxCm = f171::kDialMaxCm;         // the left key's range: the crouched left dial's
constexpr uint32_t kMinFrames = 30;                                                     // the health rule: fewer standing-in-the-sights frames than this are too few to judge

// The keys' values held to their ranges; a value that is not a number reads 0 (the body unmoved).
inline double clampForwardCm(double cm) noexcept { return f168::clampDialCm(cm); }
inline double clampLeftCm(double cm) noexcept { return f171::clampDialCm(cm); }

// ---- the weight ----------------------------------------------------------------------------------------------------------------------------------------------------------------

// The standing share of the stance: 1 less the crouched weight (F163), each held to 0..1; a crouched weight that is not a number reads 0 (standing, as F161 reads it). This is the
// `postureW` argument F168's and F171's functions take for the standing pair.
inline double standingShare(double postureW) noexcept { return 1.0 - unit01(postureW); }

// The standing weight: the standing share x the ADS detach weight (0 at the hip, 1 in the sights). 0 whenever either is 0: crouched, or at the hip.
inline double weight(double postureW, double detachW) noexcept { return standingShare(postureW) * unit01(detachW); }

// The call is standing in the sights: not at the hip (the detach weight from 0.05) and the crouched weight under 0.5 (the complement of F165's crouchedAds).
inline bool standingAds(double postureW, double detachW) noexcept { return !f159::atHip(detachW) && !pinCrouchedWc(postureW); }

// ---- the moves -----------------------------------------------------------------------------------------------------------------------------------------------------------------

// The body's move toward the gun, m (signed): F168's, with the standing share for the posture weight. Exactly 0 when the dial, the standing share or the detach is 0.
inline double forwardShiftM(double dialCm, double postureW, double detachW) noexcept { return f168::shiftM(dialCm, standingShare(postureW), detachW); }

// The body's move to the left of the aim, m (signed, positive left): F171's, with the standing share.
inline double leftShiftM(double dialCm, double postureW, double detachW) noexcept { return f171::shiftM(dialCm, standingShare(postureW), detachW); }

// The two moves as vectors in the model's axes (along the horizontal part of the aim's forward, and along its left; none vertical). False and a zero vector when the move is 0 or the aim
// has no horizontal forward.
inline bool forwardVector(double dialCm, double postureW, double detachW, const double fwd[3], double out[3]) noexcept {
    return f168::shiftVector(dialCm, standingShare(postureW), detachW, fwd, out);
}
inline bool leftVector(double dialCm, double postureW, double detachW, const double fwd[3], double out[3]) noexcept {
    return f171::shiftVector(dialCm, standingShare(postureW), detachW, fwd, out);
}

// ---- the figures ---------------------------------------------------------------------------------------------------------------------------------------------------------------

// One call standing in the sights (the caller files only those): the standing weight; for each dial the value, whether its move was made, the move asked (m, signed) and as written (not a
// number: unmeasured); the shoulders from the eye as drawn (right, up, forward, m; not numbers when unmeasured) and the distances to the gun's root and the left grip (m; not numbers when
// unmeasured).
struct Sample {
    bool moving = false;
    double weight = 0.0;
    double forwardDialCm = 0.0, leftDialCm = 0.0;
    bool forwardMade = false, leftMade = false;
    double forwardAskedM = 0.0, leftAskedM = 0.0;
    double forwardWrittenM = std::nan(""), leftWrittenM = std::nan("");
    double leftShoulderM[3] = {std::nan(""), std::nan(""), std::nan("")};
    double rightShoulderM[3] = {std::nan(""), std::nan(""), std::nan("")};
    double rightShoulderToGunM = std::nan(""), leftShoulderToGripM = std::nan("");
};

// One dial's figures over the frames: the dial's sum and largest value, the frames its move was made in, the move asked, as written (with the frames it was measured on) and what the
// reach rule took back (asked less written, on the frames with both).
struct Channel {
    uint32_t made = 0, writtenN = 0, backN = 0;
    double dialSum = 0.0, dialAbsMax = 0.0, askedSum = 0.0, writtenSum = 0.0, backSum = 0.0;
    void add(double dialCm, bool wasMade, double askedM, double writtenM) noexcept {
        if (wasMade) ++made;
        if (std::isfinite(dialCm)) {
            dialSum += dialCm;
            dialAbsMax = std::fabs(dialCm) > dialAbsMax ? std::fabs(dialCm) : dialAbsMax;
        }
        if (std::isfinite(askedM)) askedSum += askedM;
        if (std::isfinite(writtenM)) {
            ++writtenN;
            writtenSum += writtenM;
            if (std::isfinite(askedM)) { ++backN; backSum += askedM - writtenM; }
        }
    }
    void merge(const Channel& o) noexcept {
        made += o.made; writtenN += o.writtenN; backN += o.backN;
        dialSum += o.dialSum;
        dialAbsMax = o.dialAbsMax > dialAbsMax ? o.dialAbsMax : dialAbsMax;
        askedSum += o.askedSum; writtenSum += o.writtenSum; backSum += o.backSum;
    }
};
struct Counters {
    uint32_t ads = 0, still = 0, moving = 0;
    double weightSum = 0.0;
    Channel forward, left;
    uint32_t leftN[2] = {}, rightN[2] = {}, gunN[2] = {}, gripN[2] = {};   // per kind: still (0), moving (1)
    double leftSum[2][3] = {}, rightSum[2][3] = {}, gunSum[2] = {}, gripSum[2] = {};
    void add(const Sample& s) noexcept {
        ++ads;
        const int i = s.moving ? 1 : 0;
        if (s.moving) ++moving; else ++still;
        if (std::isfinite(s.weight)) weightSum += s.weight;
        forward.add(s.forwardDialCm, s.forwardMade, s.forwardAskedM, s.forwardWrittenM);
        left.add(s.leftDialCm, s.leftMade, s.leftAskedM, s.leftWrittenM);
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
        ads += o.ads; still += o.still; moving += o.moving;
        weightSum += o.weightSum;
        forward.merge(o.forward);
        left.merge(o.left);
        for (int i = 0; i < 2; ++i) {
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

// ---- the line ------------------------------------------------------------------------------------------------------------------------------------------------------------------

// The window's line, printed whenever a frame was standing in the sights (a window with the dials at 0 prints it too: that is the before picture): the item; those frames, still and
// moving, and the standing weight's mean; for each dial its mean and largest value, the frames its move was made in, the move asked and as written (the neck's change from the same body
// with the dial at 0, after the reach rule; with the frames it is a mean of) and what the reach rule took back (cm); the right and the left shoulder from the eye (right/up/forward, cm),
// the right shoulder to the gun's root and the left shoulder to the left-hand grip, still against moving. Forward is positive toward the gun, left positive to the left of the aim. The
// log's lines end at 1200 bytes.
inline int formatSummary(char* buf, size_t n, uint32_t seconds, const char* item, const Counters& c) noexcept {
    const double an = c.ads ? static_cast<double>(c.ads) : 1.0;
    const auto mean = [](double sum, uint32_t k) noexcept { return k ? sum / static_cast<double>(k) : 0.0; };
    const auto eye = [&](const double sum[2][3], const uint32_t cnt[2], int i, int axis) noexcept { return mean(sum[i][axis], cnt[i]) * 100.0; };
    return std::snprintf(buf, n,
                         "native on foot standing ADS body (F178, TEMPORARY) %u s: item %s; standing in the sights %u frames (still %u, moving %u), the standing weight "
                         "((1 - the crouched weight) x the detach weight) mean %.2f; "
                         "forward dial experimental.native_on_foot_stand_ads_body_forward_cm mean %+.1f cm (largest %.1f; positive is toward the gun), the move made in %u, asked %+.1f cm, "
                         "as written (after the reach rule; %u frames) %+.1f, the reach rule took back %+.1f; "
                         "left dial experimental.native_on_foot_stand_ads_body_left_cm mean %+.1f cm (largest %.1f; positive is left of the aim), the move made in %u, asked %+.1f cm, "
                         "as written (after the reach rule; %u frames) %+.1f, the reach rule took back %+.1f; "
                         "right shoulder from the eye (right/up/forward, cm): still %u frames %+.1f/%+.1f/%+.1f, moving %u frames %+.1f/%+.1f/%+.1f; "
                         "left shoulder from the eye: still %u frames %+.1f/%+.1f/%+.1f, moving %u frames %+.1f/%+.1f/%+.1f; "
                         "right shoulder to the gun's root %u frames still %.1f cm, %u moving %.1f; left shoulder to the left-hand grip %u frames still %.1f cm, %u moving %.1f",
                         seconds, item ? item : "none", c.ads, c.still, c.moving, c.weightSum / an,
                         c.forward.dialSum / an, c.forward.dialAbsMax, c.forward.made, c.forward.askedSum / an * 100.0, c.forward.writtenN, mean(c.forward.writtenSum, c.forward.writtenN) * 100.0,
                         mean(c.forward.backSum, c.forward.backN) * 100.0,
                         c.left.dialSum / an, c.left.dialAbsMax, c.left.made, c.left.askedSum / an * 100.0, c.left.writtenN, mean(c.left.writtenSum, c.left.writtenN) * 100.0,
                         mean(c.left.backSum, c.left.backN) * 100.0,
                         c.rightN[0], eye(c.rightSum, c.rightN, 0, 0), eye(c.rightSum, c.rightN, 0, 1), eye(c.rightSum, c.rightN, 0, 2), c.rightN[1], eye(c.rightSum, c.rightN, 1, 0),
                         eye(c.rightSum, c.rightN, 1, 1), eye(c.rightSum, c.rightN, 1, 2),
                         c.leftN[0], eye(c.leftSum, c.leftN, 0, 0), eye(c.leftSum, c.leftN, 0, 1), eye(c.leftSum, c.leftN, 0, 2), c.leftN[1], eye(c.leftSum, c.leftN, 1, 0),
                         eye(c.leftSum, c.leftN, 1, 1), eye(c.leftSum, c.leftN, 1, 2),
                         c.gunN[0], mean(c.gunSum[0], c.gunN[0]) * 100.0, c.gunN[1], mean(c.gunSum[1], c.gunN[1]) * 100.0, c.gripN[0], mean(c.gripSum[0], c.gripN[0]) * 100.0, c.gripN[1],
                         mean(c.gripSum[1], c.gripN[1]) * 100.0);
}

}  // namespace f178
}  // namespace nofoot
}  // namespace edvr
