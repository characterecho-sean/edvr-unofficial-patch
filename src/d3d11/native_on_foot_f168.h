// F168 (native on foot VR, TEMPORARY): a dial for the crouched body's distance to the gun, and the figures that judge it. Header only, no game or D3D11 state, so native_frame_test runs the
// very functions the pin and the frame thread use. Journal: the arc doc, F168.
//
// The pilot, after F166: crouched in the sights and walking, "the body is positioned too far back from the butt of the gun, if we move it closer during a crouch that should help the left
// arm". The dumps put the left upper arm near the eye with the forearm reaching forward to the foregrip. The dial moves the third-person body along the aim's horizontal forward, relative to
// the eye, by experimental.native_on_foot_crouch_ads_body_forward_cm (-20..+20, positive toward the gun). It rides the body pin's target (the neck's), so the body moves with its arms while
// the eye and the gun stay where the sight lock puts them. It is scaled by the crouched weight (F163) and the detach weight (the sights), so it is exactly 0 standing and at the hip.
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

// ---- the figures --------------------------------------------------------------------------------------------------------------------------------------------------------------

// One call crouched in the sights (the caller files only those): the dial, the move applied this call, the distances (m; not numbers when unmeasured) and the left shoulder from the eye.
struct Sample {
    bool moving = false;
    double dialCm = 0.0;
    double shiftM = 0.0;
    double rightShoulderToGunM = std::nan(""), leftShoulderToGripM = std::nan("");
    double leftShoulderM[3] = {std::nan(""), std::nan(""), std::nan("")};   // right, up, forward of the eye (the heading's axes)
};
struct Counters {
    uint32_t ads = 0, still = 0, moving = 0;
    double dialSum = 0.0, dialAbsMax = 0.0;
    uint32_t shiftN[2] = {}, gunN[2] = {}, gripN[2] = {}, eyeN[2] = {};   // per kind: still (0), moving (1)
    double shiftSum[2] = {}, gunSum[2] = {}, gripSum[2] = {}, eyeSum[2][3] = {};
    void add(const Sample& s) noexcept {
        ++ads;
        const int i = s.moving ? 1 : 0;
        if (s.moving) ++moving; else ++still;
        if (std::isfinite(s.dialCm)) {
            dialSum += s.dialCm;
            dialAbsMax = std::fabs(s.dialCm) > dialAbsMax ? std::fabs(s.dialCm) : dialAbsMax;
        }
        if (std::isfinite(s.shiftM)) { ++shiftN[i]; shiftSum[i] += s.shiftM; }
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
            gunN[i] += o.gunN[i]; gunSum[i] += o.gunSum[i];
            gripN[i] += o.gripN[i]; gripSum[i] += o.gripSum[i];
            eyeN[i] += o.eyeN[i];
            for (int c = 0; c < 3; ++c) eyeSum[i][c] += o.eyeSum[i][c];
        }
    }
};

// ---- the line -----------------------------------------------------------------------------------------------------------------------------------------------------------------

// The window's line: the item; the dial's mean and largest value over the frames crouched in the sights; those frames, still and moving; the move applied (cm, still and moving); the right
// shoulder to the gun's root, the left shoulder to the left-hand grip (cm, with the frames each is a mean of) and the left shoulder from the eye, still and moving. The log's lines end at
// 1200 bytes; this one runs to about 750.
inline int formatSummary(char* buf, size_t n, uint32_t seconds, const char* item, const Counters& c) noexcept {
    const double an = c.ads ? static_cast<double>(c.ads) : 1.0;
    const auto mean = [](double sum, uint32_t k) noexcept { return k ? sum / static_cast<double>(k) : 0.0; };
    return std::snprintf(buf, n,
                         "native on foot crouched ADS body (F168, TEMPORARY) %u s: item %s; the dial experimental.native_on_foot_crouch_ads_body_forward_cm mean %+.1f cm (largest %.1f); "
                         "crouched in the sights %u frames (still %u, moving %u); the body moved toward the gun (the dial x the crouched weight x the detach weight) %+.1f cm still, %+.1f "
                         "moving; right shoulder to the gun's root (the FP right handAttach 51 through G: the butt is not a joint the code reads) %u frames still %.1f cm, %u moving %.1f; "
                         "left shoulder to the left-hand grip (the FP left handAttach 52 through G) %u frames still %.1f cm, %u moving %.1f; the left shoulder from the eye (right/up/forward, "
                         "cm): still %u frames %+.1f/%+.1f/%+.1f, moving %u frames %+.1f/%+.1f/%+.1f",
                         seconds, item ? item : "none", c.dialSum / an, c.dialAbsMax, c.ads, c.still, c.moving, mean(c.shiftSum[0], c.shiftN[0]) * 100.0, mean(c.shiftSum[1], c.shiftN[1]) * 100.0,
                         c.gunN[0], mean(c.gunSum[0], c.gunN[0]) * 100.0, c.gunN[1], mean(c.gunSum[1], c.gunN[1]) * 100.0, c.gripN[0], mean(c.gripSum[0], c.gripN[0]) * 100.0, c.gripN[1],
                         mean(c.gripSum[1], c.gripN[1]) * 100.0, c.eyeN[0], mean(c.eyeSum[0][0], c.eyeN[0]) * 100.0, mean(c.eyeSum[0][1], c.eyeN[0]) * 100.0,
                         mean(c.eyeSum[0][2], c.eyeN[0]) * 100.0, c.eyeN[1], mean(c.eyeSum[1][0], c.eyeN[1]) * 100.0, mean(c.eyeSum[1][1], c.eyeN[1]) * 100.0,
                         mean(c.eyeSum[1][2], c.eyeN[1]) * 100.0);
}

}  // namespace f168
}  // namespace nofoot
}  // namespace edvr
