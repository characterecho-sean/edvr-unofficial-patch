// F133 (native on-foot VR): a stall cannot leave the body off the head. Header only, no game or D3D11 state, so native_frame_test runs the very functions the frame
// thread uses. Journal: the arc doc, 2026-10-27, F133.
//
// The pilot took two eye dumps in the Frontier flight edvr_gfx_20261027_080326: render stalls of 0.5 to 1 s (08:06:06-16). Between 08:06:12.4 and 08:06:15.5 his head
// turned about 41 deg left. The runtime saw it (look_to_aim_runtime head_seated_yaw +0.7 -> -41.1 deg, no recentre, space-change events 0), but look to aim's 1 s line over
// the gap counted +2.96 deg: adsHeadDelta drops a head step over kAdsHeadJumpRad (30 deg) as a LOCAL recentre, and across a 3 s stall a normal turn is one step. The turn
// back (+39.5 deg) was counted, so the follower turned the aim 40 deg right of the head and kept it: the reported head stood -39.3 to -41.4 deg off the aim for 20 s and
// more, because the follower integrates head steps and never reads the offset.
//
// Two changes:
//  - a head step is a discontinuity (a recentre) by its RATE, not its size: a rotation faster than kJumpRateDps over the time between the two samples (no head turns that
//    fast, a recentre is an instant step), or a translation over kAdsHeadJumpM. A 41 deg step over 3 s is motion. With no usable time the old size rule (30 deg) stands;
//  - the follower re-centres on a standing yaw offset: the reported head more than kRecentreMinDeg off the aim in yaw, the head still, for kRecentreHoldSec, adds that
//    offset to the follower's target once (as the engage's centring does), and waits kRecentreGapSec before another. Yaw only: the game clamps the aim's pitch, so a pitch
//    offset can be legitimate (the F109c case: the aim's pitch parted from the account at its clamp), and a re-centre would fight the clamp.
#pragma once

#include "native_on_foot_core.h"   // adsHeadAngles, adsWrap, kAdsDeg, kAdsHeadJumpRad, kAdsHeadJumpM

#include <cmath>
#include <cstdint>

namespace edvr {
namespace nofoot {
namespace f133 {

constexpr double kJumpRateDps = 1500.0;   // a head rotation faster than this (deg/s) between two samples is a recentre, not a turn
// The time between two samples is the frame thread's read times, which jitter by a few ms: a step over a shorter time than this is judged as if it had taken this long
// (so 12 deg at 1500 deg/s is the smallest step the rate rule can ever call a discontinuity).
constexpr double kMinDtSec = 0.008;

struct Step {
    bool ok = false;            // motion: the head's turn since the last sample (seat convention)
    double dYaw = 0.0, dPitch = 0.0;
    bool bigAsMotion = false;   // a step over the old 30 deg size rule that the rate rule took as motion
    double rateDps = 0.0;       // the larger of the yaw and pitch rates (deg/s); 0 with no usable time
};
// The head's turn from prev to now, dtSec apart (the frame thread's read times; <= 0 or not finite: unknown, the old size rule). False for a discontinuity or an
// unusable head. Rotation by rate, translation by distance (kAdsHeadJumpM: no head moves half a metre between two samples).
inline Step headStep(const float prev[12], const float now[12], double dtSec) noexcept {
    Step s;
    double y0 = 0.0, p0 = 0.0, y1 = 0.0, p1 = 0.0;
    if (!adsHeadAngles(prev, &y0, &p0) || !adsHeadAngles(now, &y1, &p1)) return s;
    const double dy = adsWrap(y1 - y0), dp = p1 - p0;
    const double dx = static_cast<double>(now[3]) - prev[3], dyy = static_cast<double>(now[7]) - prev[7], dz = static_cast<double>(now[11]) - prev[11];
    if (std::sqrt(dx * dx + dyy * dyy + dz * dz) > kAdsHeadJumpM) return s;
    const double big = std::fabs(dy) > std::fabs(dp) ? std::fabs(dy) : std::fabs(dp);
    if (dtSec > 0.0 && std::isfinite(dtSec)) {
        const double t = dtSec < kMinDtSec ? kMinDtSec : dtSec;
        s.rateDps = big / kAdsDeg / t;
        if (s.rateDps > kJumpRateDps) return s;
        s.bigAsMotion = big > kAdsHeadJumpRad;
    } else if (big > kAdsHeadJumpRad) {
        return s;
    }
    s.ok = true;
    s.dYaw = dy;
    s.dPitch = dp;
    return s;
}

// The count of large steps taken as motion, for the line: the cumulative number and the largest one (deg, and the time it took, ms).
struct BigSteps {
    uint32_t n = 0;
    double largestDeg = 0.0, largestMs = 0.0;
    void note(double stepDeg, double dtMs) noexcept {
        ++n;
        if (stepDeg > largestDeg) {
            largestDeg = stepDeg;
            largestMs = dtMs;
        }
    }
};

// ---- the re-centre ----------------------------------------------------------------------------------------------------------------------------------
constexpr double kRecentreMinDeg = 3.0;      // the reported head more than this off the aim in yaw
constexpr double kRecentreStillDps = 5.0;    // ... with the head turning slower than this
constexpr double kRecentreHoldSec = 2.0;     // ... continuously for this long
constexpr double kRecentreGapSec = 2.0;      // ... and not again for this long after a re-centre
constexpr double kStillWindowSec = 0.5;      // the head's rate is judged over windows of this length (the head's tremor is no rate)
constexpr double kStillOpenMinSec = 0.1;      // the window in progress is read once it is this old

struct Recentre {
    double standSince = -1.0;     // when the offset and the head's stillness began, continuously; < 0 while not
    double lastFireSec = -1e9;
    double cpT = -1.0, cpYaw = 0.0;   // the stillness window's start and the head's cumulative yaw then (deg)
    uint32_t fired = 0;
    struct Out {
        bool fire = false;
        double offDeg = 0.0, standSec = 0.0;
    };
    void reset() noexcept {
        standSince = -1.0;
        cpT = -1.0;
    }
    // One tick. allowed: look to aim is engaged and following (not blocked, not held, no snap fade, not in an ADS press). offValid, offDeg: the reported head's yaw offset
    // from the aim (seat convention, degrees). headYawDeg: the head's cumulative yaw (degrees, the accepted steps summed). nowSec: a clock in seconds.
    Out step(double nowSec, bool allowed, bool offValid, double offDeg, double headYawDeg) noexcept {
        Out o;
        if (!allowed || !offValid || !(std::fabs(offDeg) > kRecentreMinDeg)) {
            reset();
            return o;
        }
        if (cpT < 0.0) {
            cpT = nowSec;
            cpYaw = headYawDeg;
        } else if (nowSec - cpT >= kStillWindowSec) {
            const double rate = std::fabs(headYawDeg - cpYaw) / (nowSec - cpT);
            cpT = nowSec;
            cpYaw = headYawDeg;
            if (!(rate < kRecentreStillDps)) standSince = -1.0;
        }
        if (standSince < 0.0) standSince = nowSec;
        // The window in progress is not closed yet: a head that has been turning since it opened is not still, whatever the closed windows said.
        const double open = nowSec - cpT;
        const bool movingNow = open >= kStillOpenMinSec && std::fabs(headYawDeg - cpYaw) / open >= kRecentreStillDps;
        if (!movingNow && nowSec - standSince >= kRecentreHoldSec && nowSec - lastFireSec >= kRecentreGapSec) {
            o.fire = true;
            o.offDeg = offDeg;
            o.standSec = nowSec - standSince;
            lastFireSec = nowSec;
            standSince = nowSec;   // the next one needs another 2 s of standing offset
            ++fired;
        }
        return o;
    }
};

}  // namespace f133
}  // namespace nofoot
}  // namespace edvr
