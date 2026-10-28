// F165 (native on-foot VR): the left elbow points down while aiming crouched, for every two-handed gun. Header only, no game or D3D11 state, so native_frame_test runs the very
// functions the Animate post-hook and the frame thread use. Journal: the arc doc, F165.
//
// The flight edvr_gfx_20261028_162638 (build b9e27e57, dumps 163023144001 and 163028557002): crouch-walking in the sights, F161 and F163 lowered the body and the left arm no longer
// clipped the eye (the nearest 3.6 cm with the shotgun, against 0.4 cm before), but the arm folded with the elbow low and out to the left near the camera and the forearm steep up
// to the foregrip. F98a measured the elbow off the stance's plane by 43.1 deg (the shotgun) and 39.8 deg (the plasma AR, over 45 deg in 295 calls); the swivel came from the
// transported stable seed on every call (seed 37643, body-down 0). F84's body-down pole (the laser AR only) swings the elbow toward the body's down; the left elbow of every two-handed
// gun takes it while the body is crouched in the sights.
//
// The blend: the swivel angle goes from the stable pole's toward the body-down pole's by w = (F163's crouched weight) x (the ADS detach weight), along the shorter way round the circle.
// At w = 0 (standing, or at the hip) the stable pole's angle is returned as it came, so the solve is the earlier one bit for bit. The stable pole's own seed (its axis and reference) is
// not touched, so leaving the sights or standing up returns along the same angle. The body-down pole is the pistol's (ikPistolDownSwivel, as F81, F84 and F159's reseed take it): the
// elbow under the shoulder, a lateral share from the Elbow out distance. A mix that puts the elbow over the shoulder where the stable pole did not goes through the existing clamp
// (ikSwivelClamp), the ceiling alone: the body-down pole replaces the model-X half-space, as in those paths.
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
namespace f165 {

constexpr double kNoOutwardM = -1.0;      // ikSwivelClamp's outward requirement when only the ceiling is asked: below any elbow's reach of the shoulder
constexpr double kCeilingEpsM = 1e-9;     // an elbow over the shoulder by more than this violates the ceiling (as ikStableSwivel's feasibility)

// ---- the weight -----------------------------------------------------------------------------------------------------------------------------------------------------------

// The crouched weight (F163: the animated neck's drop, smoothstep 0 to 1) times the ADS detach weight (0 at the hip, 1 in the sights), each held to 0..1; a figure that is not a number
// reads 0. 0 whenever either is 0: standing, or at the hip.
inline double blendWeight(double postureW, double detachW) noexcept { return unit01(postureW) * unit01(detachW); }

// The call is crouched in the sights: not at the hip (the detach weight from 0.05) and the crouched weight from 0.5, as F161 files it.
inline bool crouchedAds(double postureW, double detachW) noexcept { return !f159::atHip(detachW) && pinCrouchedWc(postureW); }

// ---- the angle ------------------------------------------------------------------------------------------------------------------------------------------------------------

// The angle from `stable` toward `down` by w along the shorter way round (the difference wrapped to +-pi, then the sum), in (-pi, pi]. w <= 0 (or not a number) returns `stable` as it
// came, unwrapped, so the caller's earlier value is kept bit for bit; w >= 1 returns `down` wrapped. An angle that is not finite returns `stable`.
inline double blendAngle(double stable, double down, double w) noexcept {
    if (!(w > 0.0) || !std::isfinite(stable) || !std::isfinite(down)) return stable;
    if (w >= 1.0) return ikWrapPi(down);
    return ikWrapPi(stable + w * ikWrapPi(down - stable));
}

struct Blend {
    bool applied = false;    // the body-down pole was computed and mixed in (w > 0)
    bool guarded = false;    // the mix put the elbow over the shoulder where the stable pole's did not, and the clamp moved it
    bool fallback = false;   // the body-down pole used the previous radial (the shoulder-to-hand axis is vertical)
    double theta = 0.0;      // the swivel to solve with: the stable pole's, unchanged, when not applied
    double downTheta = 0.0;  // the body-down pole's swivel (applied only)
};

// The left arm's swivel: `stableTheta` (the stable pole's, or the reseeded body-down pole's) blended toward the body-down pole by w. previousRadial: the stable pole's previous radial
// (the body-down pole's fallback when the axis is vertical, as F159's reseed passes it). outSign, elbowOutM, trimRad: the solve's own. The body's up is the model's +y.
inline Blend blendPole(const IkTwoBone& t, double stableTheta, double w, const double previousRadial[3], double outSign, double elbowOutM, double trimRad) noexcept {
    Blend out;
    out.theta = stableTheta;
    if (!(w > 0.0) || !t.ok || !std::isfinite(stableTheta)) return out;
    const double up[3] = {0.0, 1.0, 0.0};
    const IkPistolDownSwivel down = ikPistolDownSwivel(t, previousRadial, up, elbowOutM, trimRad);
    if (!down.ok) return out;
    double theta = blendAngle(stableTheta, ikWrapPi(down.theta), w);
    const auto over = [&](double th) noexcept {
        double E[3];
        t.elbowAt(th, E);
        return E[1] - t.S[1];
    };
    const double overMixed = over(theta);
    if (overMixed > kCeilingEpsM && overMixed > over(stableTheta) + kCeilingEpsM) {
        bool clamped = false;
        theta = ikSwivelClamp(t, theta, outSign, &clamped, kNoOutwardM);
        out.guarded = clamped;
    }
    out.applied = true;
    out.fallback = down.usedFallback;
    out.theta = theta;
    out.downTheta = ikWrapPi(down.theta);
    return out;
}

// ---- the per-second figures -----------------------------------------------------------------------------------------------------------------------------------------------

// One left-arm solve with the stable pole: the weight asked for, whether the blend was applied, the elbow's plane change against the animated arm's (deg: F66d's planeDeg, the figure
// F98a prints), and whether the call is crouched in the sights.
struct Sample {
    double weight = 0.0, planeDeg = 0.0;
    bool applied = false, crouchedAds = false;
};
// One context's solves: how many, in how many the blend was applied, and the plane change's sum and largest.
struct Bin {
    uint32_t n = 0, applied = 0;
    double planeSum = 0.0, planeMax = 0.0;
};
struct Counters {
    uint32_t n = 0;
    double weightSum = 0.0;
    Bin ads, rest;   // crouched in the sights; every other solve
    void add(const Sample& s) noexcept {
        if (!std::isfinite(s.weight) || !std::isfinite(s.planeDeg)) return;
        ++n;
        weightSum += s.weight;
        Bin& b = s.crouchedAds ? ads : rest;
        ++b.n;
        if (s.applied) ++b.applied;
        b.planeSum += s.planeDeg;
        b.planeMax = s.planeDeg > b.planeMax ? s.planeDeg : b.planeMax;
    }
    void merge(const Counters& o) noexcept {
        n += o.n;
        weightSum += o.weightSum;
        for (int i = 0; i < 2; ++i) {
            Bin& d = i == 0 ? ads : rest;
            const Bin& s = i == 0 ? o.ads : o.rest;
            d.n += s.n;
            d.applied += s.applied;
            d.planeSum += s.planeSum;
            d.planeMax = s.planeMax > d.planeMax ? s.planeMax : d.planeMax;
        }
    }
};

// ---- the line -------------------------------------------------------------------------------------------------------------------------------------------------------------

// F165's part of the F159 window line (appended to it by the caller, after F159's formatSummary): the weight's mean over the stable-pole solves, then the solves crouched in the sights
// and the rest, each with how many had the blend applied and the elbow's plane change against the animated arm (mean, largest). Nothing is written when the window had no such solve.
// Writes at most cap - 1 bytes plus the terminator.
inline int appendSummary(char* buf, size_t cap, const Counters& c) noexcept {
    if (!buf || cap == 0) return 0;
    const size_t used = std::strlen(buf);
    if (!c.n || used + 1 >= cap) return 0;
    const double nn = static_cast<double>(c.n), an = c.ads.n ? static_cast<double>(c.ads.n) : 1.0, rn = c.rest.n ? static_cast<double>(c.rest.n) : 1.0;
    return std::snprintf(buf + used, cap - used,
                         "; elbow blend to the body-down pole (F165): weight mean %.2f; in the sights crouched %u solves, %u blended, the elbow %.1f deg off the stance's plane (max %.1f); "
                         "the rest %u solves, %u blended, %.1f deg (max %.1f)",
                         c.weightSum / nn, c.ads.n, c.ads.applied, c.ads.planeSum / an, c.ads.planeMax, c.rest.n, c.rest.applied, c.rest.planeSum / rn, c.rest.planeMax);
}

}  // namespace f165
}  // namespace nofoot
}  // namespace edvr
