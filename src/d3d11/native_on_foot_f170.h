// F170 (native on foot VR, TEMPORARY): the crouched ADS torso squared to the aim, and the figures that judge it. Header only, no game or D3D11 state, so native_frame_test runs the very
// functions the pin and the frame thread use. Journal: the arc doc, F170.
//
// The pilot, after F169: crouched in the sights "the crouch pose puts the gun in the left shoulder, can we make the whole pose more square?". The flight edvr_gfx_20261028_215408
// (build b181c4c7): the shotgun's stock crossed over the left shoulder pad in the eye dump of 21:57:05, and the square-up (F75) read "authored stance selected", torso phi +0.00, while the
// measured shoulder phi was -28.7 deg (largest 28.8) in 90 of 90 frames: the crouched ADS animation's torso is bladed about 29 deg with the left shoulder forward, and an armed torso keeps
// its authored stance (F75), so nothing squared it. The F168 dial then put the body toward the gun and the left shoulder under the sight line.
//
// The turn: while crouched (F163's posture weight) and in the sights (the detach weight), scaled by both and by the dial, the torso turns about the vertical through the neck toward
// square: the upper-arm line perpendicular to the aim's horizontal forward, removing that share of the measured shoulder phi (held to the square-up's old limit, 50 deg). It is the
// square-up's own machinery: the same angle feeds the reach rule's shoulders (so the left hand stays on the grip) and the written turn of the spine levels 16, 21, 24 (1/3, 2/3, 1).
//
// While the upper body is held (F166, walking crouched in the sights) the square-up yields to the hold by 1 less its weight, so the held torso cannot be given a lean. This turn does not
// yield: its angle is measured on the pose as it is after the hold wrote it (the held torso's own blade), and it is turned onto that pose after the yield, so it is the same share of the
// held pose's blade still and moving, and nothing accumulates (the hold rewrites the torso from the captured pose every call).
//
// The dial is experimental.native_on_foot_crouch_ads_square, 0..1, shipped 1. At 0, standing, at the hip and unarmed the turn is exactly 0 and every path is what it was.
#pragma once

#include "native_on_foot_core.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace edvr {
namespace nofoot {
namespace f170 {

constexpr double kDialMin = 0.0, kDialMax = 1.0;   // the key's range
constexpr uint32_t kMinFrames = 30;                // the health rule: fewer crouched-in-the-sights frames than this are too few to judge
constexpr double kBladedDeg = 1.0;                 // a shoulder line this far off square (or more) has a turn to make

// The key's value held to its range; a value that is not a number reads 0 (no turn).
inline double clampDial(double dial) noexcept { return !std::isfinite(dial) ? 0.0 : dial < kDialMin ? kDialMin : dial > kDialMax ? kDialMax : dial; }

// The share of the measured shoulder phi that is removed: the dial x the crouched weight x the detach weight, each held to 0..1. Exactly 0 when any is 0.
inline double share(double dial, double postureW, double detachW) noexcept { return clampDial(dial) * unit01(postureW) * unit01(detachW); }

// The turn (radians, ikYawFromTo's sense: the same sign as the square-up's phi): the share of the measured shoulder phi, the phi held to the square-up's old limit. Exactly 0 for a share of
// 0, a phi that was not measured (haveSh false) or is not a number.
inline double turnRad(double shareNow, bool haveSh, double phiSh) noexcept {
    if (!(shareNow > 0.0) || !haveSh || !std::isfinite(phiSh)) return 0.0;
    bool clamped = false;
    const double t = shareNow * ikSquareClampRad(phiSh, &clamped);
    return std::isfinite(t) ? t : 0.0;
}

// The square-up as it is written: the armed base angle (squareApplied) taken through the upper-body hold's yield, plus this turn, which the hold does not take off. With no turn this is
// exactly base x yield, as it was before F170.
inline double writtenYaw(double armedBase, double holdYield, double turn) noexcept {
    const double held = armedBase * holdYield;
    return turn != 0.0 && std::isfinite(turn) ? held + turn : held;
}

// What the shoulder line's phi is after a turn (radians): the turn is a rotation about the vertical of the whole upper-arm line, so the yaw left to square it is the phi less the turn.
inline double phiAfter(double phi, double turn) noexcept { return phi - turn; }

// ---- the figures --------------------------------------------------------------------------------------------------------------------------------------------------------------

// One call crouched in the sights (the caller files only those): the dial, the measured shoulder phi before the turn and as finally written (degrees; not numbers when unmeasured), the
// turn applied (degrees, 0 when none), the shoulders from the eye (right, up, forward, m; not numbers when unmeasured) and the right shoulder to the gun's root.
struct Sample {
    bool moving = false;
    double dial = 0.0;
    double phiBeforeDeg = std::nan(""), phiAfterDeg = std::nan("");
    double turnDeg = 0.0;
    double leftShoulderM[3] = {std::nan(""), std::nan(""), std::nan("")};
    double rightShoulderM[3] = {std::nan(""), std::nan(""), std::nan("")};
    double rightShoulderToGunM = std::nan("");
};
struct Counters {
    uint32_t ads = 0, still = 0, moving = 0;
    double dialSum = 0.0, dialMax = 0.0;
    uint32_t bladed = 0, turned = 0;   // frames with the shoulder line kBladedDeg or more off square before the turn; frames with a turn applied
    uint32_t beforeN[2] = {}, afterN[2] = {}, turnN[2] = {}, leftN[2] = {}, rightN[2] = {}, gunN[2] = {};   // per kind: still (0), moving (1)
    double beforeSum[2] = {}, afterSum[2] = {}, turnSum[2] = {}, leftSum[2][3] = {}, rightSum[2][3] = {}, gunSum[2] = {};
    void add(const Sample& s) noexcept {
        ++ads;
        const int i = s.moving ? 1 : 0;
        if (s.moving) ++moving; else ++still;
        if (std::isfinite(s.dial)) {
            dialSum += s.dial;
            dialMax = s.dial > dialMax ? s.dial : dialMax;
        }
        if (std::isfinite(s.phiBeforeDeg)) {
            ++beforeN[i];
            beforeSum[i] += s.phiBeforeDeg;
            if (std::fabs(s.phiBeforeDeg) >= kBladedDeg) ++bladed;
        }
        if (std::isfinite(s.phiAfterDeg)) { ++afterN[i]; afterSum[i] += s.phiAfterDeg; }
        if (std::isfinite(s.turnDeg)) {
            ++turnN[i];
            turnSum[i] += s.turnDeg;
            if (s.turnDeg != 0.0) ++turned;
        }
        if (std::isfinite(s.leftShoulderM[0]) && std::isfinite(s.leftShoulderM[1]) && std::isfinite(s.leftShoulderM[2])) {
            ++leftN[i];
            for (int c = 0; c < 3; ++c) leftSum[i][c] += s.leftShoulderM[c];
        }
        if (std::isfinite(s.rightShoulderM[0]) && std::isfinite(s.rightShoulderM[1]) && std::isfinite(s.rightShoulderM[2])) {
            ++rightN[i];
            for (int c = 0; c < 3; ++c) rightSum[i][c] += s.rightShoulderM[c];
        }
        if (std::isfinite(s.rightShoulderToGunM)) { ++gunN[i]; gunSum[i] += s.rightShoulderToGunM; }
    }
    void merge(const Counters& o) noexcept {
        ads += o.ads; still += o.still; moving += o.moving;
        dialSum += o.dialSum;
        dialMax = o.dialMax > dialMax ? o.dialMax : dialMax;
        bladed += o.bladed; turned += o.turned;
        for (int i = 0; i < 2; ++i) {
            beforeN[i] += o.beforeN[i]; beforeSum[i] += o.beforeSum[i];
            afterN[i] += o.afterN[i]; afterSum[i] += o.afterSum[i];
            turnN[i] += o.turnN[i]; turnSum[i] += o.turnSum[i];
            gunN[i] += o.gunN[i]; gunSum[i] += o.gunSum[i];
            leftN[i] += o.leftN[i]; rightN[i] += o.rightN[i];
            for (int c = 0; c < 3; ++c) {
                leftSum[i][c] += o.leftSum[i][c];
                rightSum[i][c] += o.rightSum[i][c];
            }
        }
    }
};

// ---- the line -----------------------------------------------------------------------------------------------------------------------------------------------------------------

// The window's line: the item; the dial's mean and largest value over the frames crouched in the sights; those frames, still and moving; the frames the turn was written in and those with the
// shoulder line 1 deg or more off square; the turn, the measured shoulder phi before it and as finally written (degrees, still and moving, with the frames the phis are means of); the left and the
// right shoulder from the eye, still and moving; the right shoulder to the gun's root (the FP right handAttach 51 through G). The log's lines end at 1200 bytes; this one runs to about 850.
inline int formatSummary(char* buf, size_t n, uint32_t seconds, const char* item, const Counters& c) noexcept {
    const double an = c.ads ? static_cast<double>(c.ads) : 1.0;
    const auto mean = [](double sum, uint32_t k) noexcept { return k ? sum / static_cast<double>(k) : 0.0; };
    const auto eye = [&](const double sum[2][3], const uint32_t cnt[2], int i, int axis) noexcept { return mean(sum[i][axis], cnt[i]) * 100.0; };
    return std::snprintf(buf, n,
                         "native on foot crouched ADS torso square (F170, TEMPORARY) %u s: item %s; the dial experimental.native_on_foot_crouch_ads_square mean %.2f (largest %.2f); "
                         "crouched in the sights %u frames (still %u, moving %u); turned toward square in %u frames, the shoulder line 1 deg or more off square in %u; the turn %+.1f deg still, %+.1f moving; "
                         "the measured shoulder phi (the yaw that squares the upper-arm line to the aim) before the turn %+.1f deg still, %+.1f moving (%u/%u frames), as written %+.1f deg still, "
                         "%+.1f moving (%u/%u frames); left shoulder from the eye (right/up/forward, cm): still %u frames %+.1f/%+.1f/%+.1f, moving %u frames %+.1f/%+.1f/%+.1f; "
                         "right shoulder from the eye: still %u frames %+.1f/%+.1f/%+.1f, moving %u frames %+.1f/%+.1f/%+.1f; "
                         "right shoulder to the gun's root %u frames still %.1f cm, %u moving %.1f",
                         seconds, item ? item : "none", c.dialSum / an, c.dialMax, c.ads, c.still, c.moving, c.turned, c.bladed, mean(c.turnSum[0], c.turnN[0]), mean(c.turnSum[1], c.turnN[1]),
                         mean(c.beforeSum[0], c.beforeN[0]), mean(c.beforeSum[1], c.beforeN[1]), c.beforeN[0], c.beforeN[1], mean(c.afterSum[0], c.afterN[0]),
                         mean(c.afterSum[1], c.afterN[1]), c.afterN[0], c.afterN[1], c.leftN[0], eye(c.leftSum, c.leftN, 0, 0), eye(c.leftSum, c.leftN, 0, 1), eye(c.leftSum, c.leftN, 0, 2),
                         c.leftN[1], eye(c.leftSum, c.leftN, 1, 0), eye(c.leftSum, c.leftN, 1, 1), eye(c.leftSum, c.leftN, 1, 2), c.rightN[0], eye(c.rightSum, c.rightN, 0, 0),
                         eye(c.rightSum, c.rightN, 0, 1), eye(c.rightSum, c.rightN, 0, 2), c.rightN[1], eye(c.rightSum, c.rightN, 1, 0), eye(c.rightSum, c.rightN, 1, 1),
                         eye(c.rightSum, c.rightN, 1, 2), c.gunN[0], mean(c.gunSum[0], c.gunN[0]) * 100.0, c.gunN[1], mean(c.gunSum[1], c.gunN[1]) * 100.0);
}

}  // namespace f170
}  // namespace nofoot
}  // namespace edvr
