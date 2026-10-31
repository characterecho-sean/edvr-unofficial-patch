// F185 (native on foot VR): the left arm keeps a bend in the sights (TEMPORARY dial), the shoulder first. Header only, no game or D3D11 state, so native_frame_test runs the very functions the
// pin's reach rule and the arm solve use. Journal: the arc doc, F185.
//
// The flight edvr_gfx_20261030_134445 (build d7e918d1, the plasma shotgun, standing in the sights, seconds 13:47:35-42): F178's line put the left shoulder 61.5-62.5 cm from the left-hand grip (the
// left dial asked 23 cm of body move and the reach rule took back 17.9-22.8 of it); F56 read the left arm (30.4 + 28.8 = 59.2 cm) at a reach ratio of 1.04 (1.02 to 1.06), up to 3.7 cm short, the
// swivel clamped in 448 of 448 solves: the arm locked straight, stretched down the side. A real support arm keeps about a 45 deg elbow. In the sights F179a stands the body up (the legs straight),
// the left dial puts the body left of the aim line and the shotgun's foregrip sits far forward: each takes the left shoulder from the grip, and the reach rule gives back only enough to make the
// grip just reachable (the arm's full length less 2 cm).
//
// The dial experimental.native_on_foot_ads_left_reach, 0.80 to 1.00, default 0.92 (live): in the sights with a two-handed gun the left shoulder is kept within this share of the left arm's full
// length (upper arm + forearm) of the left-hand grip, so the elbow keeps a bend. 1.00 is today's, bit for bit (the grip just reachable). Weighted by the detach weight (the share goes from 1 to
// the dial with it) in both stances; the hip, pistols and tools are not touched.
//
// The order, shoulder first:
//  1. The clavicle takes the first of it: the left arm's groups turn about the up axis through the clavicle joint (protraction, the shoulder toward the grip), by up to kMaxTurnDeg (15 deg), by
//     what brings the hand target within the target distance. It is made in the arm solve (the arm IK, native_on_foot.cpp), on the pose the earlier stages wrote: F166's hold and F180b's
//     straighten carry the clavicles unchanged against their spine levels, so the protraction composes with them by starting from the carried shoulder; F59's reach assist (the other clavicle
//     turn in that solve) then works from the protracted shoulder (it saves the pose once, before the first turn, so a side not written gets its pose back whole).
//  2. What the clavicle cannot cover, the pin's reach rule takes from the body's move toward the grip as it does now, to the target distance plus what the clavicle can give (its chord at
//     kMaxTurnDeg) in place of the arm's full length less 2 cm: reachRadiusM. The right arm, the sight lock and the eye are not touched.
#pragma once

#include "native_on_foot_core.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace edvr {
namespace nofoot {
namespace f185 {

constexpr double kDialMin = 0.80, kDialMax = 1.00, kDialDefault = 0.92;   // the key's range and default (1.00 is today's: the grip just reachable)
constexpr double kMaxTurnDeg = 15.0;                                       // the clavicle's protraction, at most
constexpr double kReachTol = 0.03;                                         // the health rule: the left arm's reach ratio within this of the dial
constexpr double kSettledDetach = 0.9;                                     // the solves the line counts: the detach weight from this (the sights settled)
constexpr uint32_t kMinSolves = 30;                                        // the health rule: fewer two-handed ADS solves than this are too few to judge

// The key's value held to its range; a value that is not a number reads the default.
inline double clampDial(double v) noexcept { return !std::isfinite(v) ? kDialDefault : v < kDialMin ? kDialMin : v > kDialMax ? kDialMax : v; }
// The dial is off at 1.00: today's reach rule and no protraction, bit for bit.
inline bool off(double dial) noexcept { return !(clampDial(dial) < 1.0); }

// The share of the arm's full length the shoulder is kept within: 1 at the hip, going to the dial with the detach weight (held to 0..1).
inline double share(double dial, double detachW) noexcept {
    const double d = clampDial(dial), w = !std::isfinite(detachW) ? 0.0 : detachW < 0.0 ? 0.0 : detachW > 1.0 ? 1.0 : detachW;
    return 1.0 - w * (1.0 - d);
}
// The target distance (m): the arm's full length times the share.
inline double targetM(double armLenM, double dial, double detachW) noexcept { return armLenM * share(dial, detachW); }
// What a clavicle of length clavLenM gives the shoulder with the largest protraction: the chord 2 c sin(max turn / 2) (m).
inline double clavicleChordM(double clavLenM) noexcept {
    return std::isfinite(clavLenM) && clavLenM > 0.0 ? 2.0 * clavLenM * std::sin(0.5 * kMaxTurnDeg * kIkPi / 180.0) : 0.0;
}
// The pin's reach rule radius for the left shoulder: today's (the arm's length less its margin) unless the dial is on, then the smaller of today's and the target distance plus the clavicle's
// chord. When the target plus the chord is no smaller than today's, it IS today's (the same double): the clavicle alone makes the difference and the rule is not touched.
inline double reachRadiusM(double todayM, double armLenM, double clavLenM, double dial, double detachW) noexcept {
    if (off(dial) || !(detachW > 0.0) || !std::isfinite(todayM)) return todayM;
    const double r = targetM(armLenM, dial, detachW) + clavicleChordM(clavLenM);
    return r < todayM ? r : todayM;
}

// The clavicle's protraction: the shoulder S turned about the up axis through the clavicle joint C (positive turn: the shoulder's heading atan2(x, z) from C increases) toward the hand target T,
// by the smallest turn that brings |T - S| within targetM, at most kMaxTurnDeg; the full turn when that is not enough (capped). applied false when the shoulder is within the target already,
// the turn would not shorten the distance, or C, S or T is not usable (S on the up axis through C).
struct Turn {
    bool applied = false, capped = false;
    double angleRad = 0.0, moveM = 0.0, distBefore = 0.0, distAfter = 0.0;
};
inline Turn protract(const double C[3], const double S[3], const double T[3], double targetDistM) noexcept {
    Turn t;
    const double cs[3] = {S[0] - C[0], S[1] - C[1], S[2] - C[2]}, ct[3] = {T[0] - C[0], T[1] - C[1], T[2] - C[2]};
    const double rho = std::sqrt(cs[0] * cs[0] + cs[2] * cs[2]), rhoT = std::sqrt(ct[0] * ct[0] + ct[2] * ct[2]);
    const auto dist = [&](double phi) noexcept {
        const double psi = std::atan2(cs[0], cs[2]) + phi;
        const double x = rho * std::sin(psi) - ct[0], y = cs[1] - ct[1], z = rho * std::cos(psi) - ct[2];
        return std::sqrt(x * x + y * y + z * z);
    };
    if (!std::isfinite(rho + rhoT + targetDistM) || !(rho > 1e-3) || !(rhoT > 1e-3)) return t;
    t.distBefore = t.distAfter = dist(0.0);
    if (!(t.distBefore > targetDistM)) return t;
    double delta = std::atan2(ct[0], ct[2]) - std::atan2(cs[0], cs[2]);
    while (delta > kIkPi) delta -= 2.0 * kIkPi;
    while (delta < -kIkPi) delta += 2.0 * kIkPi;
    const double lim = kMaxTurnDeg * kIkPi / 180.0, full = delta > lim ? lim : delta < -lim ? -lim : delta;
    if (!(dist(full) < t.distBefore - 1e-6)) return t;   // the turn does not bring the shoulder closer (the grip straight above or below it)
    double phi = full;
    if (dist(full) > targetDistM) {
        t.capped = true;
    } else {
        double lo = 0.0, hi = 1.0;   // the share of the full turn: the smallest that reaches the target distance
        for (int i = 0; i < 40; ++i) {
            const double mid = 0.5 * (lo + hi);
            if (dist(mid * full) > targetDistM) lo = mid; else hi = mid;
        }
        phi = hi * full;
    }
    t.applied = true;
    t.angleRad = phi;
    t.distAfter = dist(phi);
    const double xs = rho * std::sin(std::atan2(cs[0], cs[2]) + phi) - cs[0], zs = rho * std::cos(std::atan2(cs[0], cs[2]) + phi) - cs[2];
    t.moveM = std::sqrt(xs * xs + zs * zs);
    return t;
}
// The rotation of the turn about the up axis (row-vector, as ikPivot takes it).
inline void turnMatrix(double angleRad, double R[9]) noexcept {
    const double up[3] = {0.0, 1.0, 0.0};
    ikRotAxis(up, angleRad, R);
}

// ---- the figures --------------------------------------------------------------------------------------------------------------------------------------------------------------------

// One two-handed ADS solve of the left arm (the sights settled: the detach weight from kSettledDetach; the dial at 1.00 included: the before picture): the dial, the target distance (m), the
// clavicle's turn (deg; 0 when none was made) and the shoulder's move from it (cm), whether the full turn was not enough, what the reach rule took of the body's move for the dial (cm), and the
// left arm's reach ratio (hand target over the arm's full length) before the clavicle and after it.
struct Sample {
    double dial = 1.0, targetM = 0.0, turnDeg = 0.0, moveCm = 0.0, tookCm = 0.0, reachBefore = std::nan(""), reachAfter = std::nan("");
    bool turned = false, capped = false;
};
struct Counters {
    uint32_t frames = 0, turned = 0, capped = 0, reachN = 0;
    double dialSum = 0.0, targetSum = 0.0, turnSum = 0.0, turnMax = 0.0, moveSum = 0.0, moveMax = 0.0, tookSum = 0.0, tookMax = 0.0;
    double reachBeforeSum = 0.0, reachAfterSum = 0.0, reachAfterMin = 1.0e9, reachAfterMax = -1.0e9;
    void add(const Sample& s) noexcept {
        ++frames;
        if (std::isfinite(s.dial)) dialSum += s.dial;
        if (std::isfinite(s.targetM)) targetSum += s.targetM;
        if (s.turned) {
            ++turned;
            turnSum += s.turnDeg;
            turnMax = std::max(turnMax, s.turnDeg);
            moveSum += s.moveCm;
            moveMax = std::max(moveMax, s.moveCm);
            if (s.capped) ++capped;
        }
        if (std::isfinite(s.tookCm)) {
            tookSum += s.tookCm;
            tookMax = std::max(tookMax, s.tookCm);
        }
        if (std::isfinite(s.reachBefore) && std::isfinite(s.reachAfter)) {
            ++reachN;
            reachBeforeSum += s.reachBefore;
            reachAfterSum += s.reachAfter;
            reachAfterMin = std::min(reachAfterMin, s.reachAfter);
            reachAfterMax = std::max(reachAfterMax, s.reachAfter);
        }
    }
    void merge(const Counters& o) noexcept {
        frames += o.frames; turned += o.turned; capped += o.capped; reachN += o.reachN;
        dialSum += o.dialSum; targetSum += o.targetSum; turnSum += o.turnSum; turnMax = std::max(turnMax, o.turnMax);
        moveSum += o.moveSum; moveMax = std::max(moveMax, o.moveMax); tookSum += o.tookSum; tookMax = std::max(tookMax, o.tookMax);
        reachBeforeSum += o.reachBeforeSum; reachAfterSum += o.reachAfterSum;
        reachAfterMin = std::min(reachAfterMin, o.reachAfterMin);
        reachAfterMax = std::max(reachAfterMax, o.reachAfterMax);
    }
};

// The window's line, printed whenever a two-handed ADS solve was counted (the dial at 1.00 included): the item; the solves; the dial's mean; the target shoulder-to-hand distance (mean, cm);
// the solves the clavicle turned, the turn (mean and largest, deg), the shoulder's move from it (mean and largest, cm) and those where the full 15 deg was not enough; what the reach rule took
// of the body's move for the dial (mean and largest, cm); the left arm's reach ratio before the clavicle and after it (mean; after: min and max). The log's lines end at 1200 bytes.
inline int formatSummary(char* buf, size_t n, uint32_t seconds, const char* item, const Counters& c) noexcept {
    const double fn = c.frames ? static_cast<double>(c.frames) : 1.0, tn = c.turned ? static_cast<double>(c.turned) : 1.0, rn = c.reachN ? static_cast<double>(c.reachN) : 1.0;
    return std::snprintf(buf, n,
                         "native on foot ADS left arm bend (F185, TEMPORARY) %u s: item %s; two-handed ADS solves %u, the dial experimental.native_on_foot_ads_left_reach mean %.2f; the target shoulder to hand "
                         "distance mean %.1f cm; the clavicle turned in %u (the turn mean %.1f deg, largest %.1f; the shoulder moved mean %.1f cm, largest %.1f; the full %.0f deg not enough in %u); the reach "
                         "rule took mean %.1f cm of the body's move for it (largest %.1f); the left arm's reach ratio before the clavicle mean %.3f, after it mean %.3f (min %.3f max %.3f)",
                         seconds, item ? item : "none", c.frames, c.dialSum / fn, 100.0 * c.targetSum / fn, c.turned, c.turnSum / tn, c.turnMax, c.moveSum / tn, c.moveMax, kMaxTurnDeg, c.capped,
                         c.tookSum / fn, c.tookMax, c.reachBeforeSum / rn, c.reachAfterSum / rn, c.reachN ? c.reachAfterMin : 0.0, c.reachN ? c.reachAfterMax : 0.0);
}

}  // namespace f185
}  // namespace nofoot
}  // namespace edvr
