// F181 (native on foot VR): the left elbow's swivel capped at the hip with a two-handed gun (TEMPORARY dial). Header only, no game or D3D11 state, so native_frame_test runs the very
// functions the arm solve uses. Journal: the arc doc, F181.
//
// The flight edvr_gfx_20261030_111627 (build 9126c7f8, the plasma assault rifle at the hip; the eye dump 112141005002): the forearm climbs steeply to the foregrip with the elbow up and out.
// F98a's 1 s lines: the elbow's swivel from the stance's plane 52 to 55 deg at the hip (over 45 deg in every solve; body-down 0, the stable pole's seed in every solve), 1.5 to 35 deg in the
// sights since F180a (body-down 90 of 90), and 18.5 to 18.8 deg for the plasma shotgun at the hip. The pilot: the ADS settings are good now; the rifle's left elbow at the hip has a problem
// like the ADS elbow F180a fixed.
//
// The cap: experimental.native_on_foot_hip_elbow_swivel_max_deg, 0..90, default 25, live. The "swivel from the stance's plane" is F98a's measure (HingeSwings::planeDeg: the angle, about the
// upper arm, between the animated arm's hinge normal turned by the shortest arc and the solved arm's, 0..180). With a two-handed gun (the left hand's stable-pole branch, the gate F165 and F180a
// use) at the hip, a solve whose plane angle p is over the cap c is brought to p - hipShare x (p - c), the hip share being 1 less the detach weight: the cap whole at the hip, none in the
// sights (F180a's body-down blend rules there), continuous between. It turns the pole about the shoulder-to-hand axis (the swivel) toward the plane, from where the pole is, along the way the
// plane angle falls, to the first swivel where the plane angle is the target: it cannot pass through the plane, so the elbow keeps its side. A plane angle already at or under the cap, a cap of
// 90 (off), the sights, a pistol, a tool, unarmed: the swivel is not touched, bit for bit. The pole's seed (axis, reference) is not touched: the cap is made again on every solve from the
// pole, so leaving the cap's reach (the key at 90, the sights) returns along the pole's own angle.
#pragma once

#include "native_on_foot_core.h"
#include "native_on_foot_f159.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace edvr {
namespace nofoot {
namespace f181 {

constexpr double kCapMinDeg = 0.0, kCapMaxDeg = 90.0, kCapDefaultDeg = 25.0;   // the key's range and default (90 leaves the game's pose)
constexpr double kHoldTolDeg = 2.0;                                              // the health rule: a plane angle this far over the cap after it is a cap that did not hold
constexpr uint32_t kMinSolves = 30;                                              // the health rule: fewer two-handed hip solves than this are too few to judge

// The key's value held to its range; a value that is not a number reads the default.
inline double clampCapDeg(double deg) noexcept { return !std::isfinite(deg) ? kCapDefaultDeg : deg < kCapMinDeg ? kCapMinDeg : deg > kCapMaxDeg ? kCapMaxDeg : deg; }

// The cap is off at 90: the game's pose is left as it is.
inline bool capOff(double capDeg) noexcept { return !(clampCapDeg(capDeg) < kCapMaxDeg); }

// The hip share of the cap: 1 less the detach weight, held to 0..1 (a detach weight that is not a number reads the hip).
inline double hipShare(double detachW) noexcept { return 1.0 - unit01(detachW); }

// The plane angle a solve is brought to: p less the hip share of what is over the cap; p itself (untouched) when the cap is off, the plane angle is not over it, or the share is 0.
inline double targetDeg(double planeDeg, double capDeg, double share) noexcept {
    if (!std::isfinite(planeDeg) || capOff(capDeg)) return planeDeg;
    const double cap = clampCapDeg(capDeg);
    const double s = unit01(share);
    if (!(planeDeg > cap) || !(s > 0.0)) return planeDeg;
    return planeDeg - s * (planeDeg - cap);
}

struct Cap {
    bool applied = false;
    double theta = 0.0;          // the swivel to solve with: the pole's own, unchanged, when not applied
    double planeAfter = std::nan("");
    double turnedDeg = 0.0;      // how far the pole was turned
};

// The swivel brought to the target plane angle. theta0 / planeDeg0: the pole's swivel (rad) and its plane angle (deg); targetDeg: from targetDeg() above; planeAt(theta): the plane angle (deg)
// of the solve at a swivel (the same measure, 180 where it cannot be measured). Along the way the plane angle falls (tried 0.5 deg either side of the pole), in steps of 3 deg up to a half
// turn, to the first swivel where it is at or under the target; then 24 bisections between the last swivel over and that one. Where it never gets to the target on that side (a cap of 0 asks
// for the plane itself) the closest approach it does reach is taken, when that is 1 deg or more nearer the plane than the pole is. Not applied (the swivel unchanged) when the plane angle is not
// over the target, or does not fall.
template <class PlaneAt>
inline Cap capSwivel(double theta0, double planeDeg0, double target, PlaneAt&& planeAt) noexcept {
    Cap out;
    out.theta = theta0;
    if (!std::isfinite(theta0) || !std::isfinite(planeDeg0) || !std::isfinite(target) || !(planeDeg0 > target + 1e-9)) return out;
    const double deg = kIkPi / 180.0;
    const double gPlus = planeAt(theta0 + 0.5 * deg), gMinus = planeAt(theta0 - 0.5 * deg);
    const double dir = gPlus < gMinus ? 1.0 : -1.0;
    double lo = 0.0, hi = -1.0;   // lo: the last turn with the plane angle over the target, hi: the first at or under it
    double bestT = 0.0, bestG = planeDeg0;
    for (double t = 3.0 * deg; t <= kIkPi + 1e-9; t += 3.0 * deg) {
        const double g = planeAt(theta0 + dir * t);
        if (g <= target) {
            hi = t;
            break;
        }
        if (g < bestG) {
            bestG = g;
            bestT = t;
        }
        lo = t;
        if (g > bestG + 1e-9 && bestT > 0.0) break;   // past the closest approach: the plane angle is rising again
    }
    if (hi < 0.0) {
        // The plane angle does not reach the target on this side (a cap of 0 asks for the plane itself, which a step of 3 deg steps over; a pose that cannot be brought as near as the cap): the closest
        // approach it does reach, found between the steps either side of it, when that is a real gain (1 deg or more) over where the pole is.
        if (!(bestT > 0.0) || !(planeDeg0 - bestG >= 1.0)) return out;
        double a = bestT - 3.0 * deg, b = bestT + 3.0 * deg;
        a = a < 0.0 ? 0.0 : a;
        for (int i = 0; i < 60; ++i) {
            const double m1 = a + (b - a) / 3.0, m2 = b - (b - a) / 3.0;
            if (planeAt(theta0 + dir * m1) < planeAt(theta0 + dir * m2)) b = m2;
            else a = m1;
        }
        const double t = 0.5 * (a + b), g = planeAt(theta0 + dir * t);
        if (!(g < planeDeg0 - 1.0)) return out;
        out.applied = true;
        out.theta = ikWrapPi(theta0 + dir * t);
        out.planeAfter = g;
        out.turnedDeg = t / deg;
        return out;
    }
    for (int i = 0; i < 24; ++i) {
        const double mid = 0.5 * (lo + hi);
        if (planeAt(theta0 + dir * mid) <= target) hi = mid;
        else lo = mid;
    }
    out.applied = true;
    out.theta = ikWrapPi(theta0 + dir * hi);
    out.planeAfter = planeAt(theta0 + dir * hi);
    out.turnedDeg = hi / deg;
    return out;
}

// ---- the figures ----------------------------------------------------------------------------------------------------------------------------------------------------------------------

// The window's counters (F98Left's): the two-handed solves at the hip, those the cap turned, the cut (the plane angle before less after, deg; sum and largest) and the solves whose plane angle
// was still over the cap by 2 deg or more after it. The F98a line ends with these.
struct Counters {
    uint32_t hip = 0, capped = 0, over = 0;
    double cutSum = 0.0, cutMax = 0.0;
    // One two-handed hip solve: whether the cap turned the swivel, the cut (deg) and the plane angle after the cap (deg) against the cap (deg).
    void add(bool wasCapped, double cutDeg, double planeAfterDeg, double capDeg) noexcept {
        ++hip;
        if (wasCapped && std::isfinite(cutDeg)) {
            ++capped;
            cutSum += cutDeg;
            cutMax = cutDeg > cutMax ? cutDeg : cutMax;
        }
        if (!capOff(capDeg) && std::isfinite(planeAfterDeg) && planeAfterDeg > clampCapDeg(capDeg) + kHoldTolDeg) ++over;
    }
    void merge(const Counters& o) noexcept {
        hip += o.hip; capped += o.capped; over += o.over;
        cutSum += o.cutSum;
        cutMax = o.cutMax > cutMax ? o.cutMax : cutMax;
    }
};

// The F98a line's ending: "; capped N of M solves (the cut mean X deg, largest Y; the cap Z deg; still 2 deg or more over it after: K)". Appended to the line the caller built (cap - 1 bytes
// at most, nothing when the buffer has no room).
inline int appendSummary(char* buf, size_t cap, const Counters& c, double capDeg) noexcept {
    if (!buf || cap == 0) return 0;
    const size_t used = std::strlen(buf);
    if (used + 1 >= cap) return 0;
    return std::snprintf(buf + used, cap - used, "; capped %u of %u solves (the cut mean %.1f deg, largest %.1f; the cap %.0f deg; still %.0f deg or more over it after: %u)", c.capped, c.hip,
                         c.capped ? c.cutSum / static_cast<double>(c.capped) : 0.0, c.cutMax, clampCapDeg(capDeg), kHoldTolDeg, c.over);
}

}  // namespace f181
}  // namespace nofoot
}  // namespace edvr
