// F118 (native on-foot VR, measure only): why the aim reticle is seen double. The reticle stands at the depth of one point under the aim line (F85c's
// march through the left eye's scene depth), read back two frames later and smoothed in 1/D over 40 ms. Two causes are told apart here:
//  * the edge: the point lies on a silhouette, so the reticle takes one side's depth while the eyes look at the other. Measured by the footprint: four
//    more lines of the same dispatch at +-0.6 deg about the aim line (the camera's right and up), and how far apart in disparity the five depths are;
//  * the speed: the target jumps between depths (the steps, and the flips that undo one), or the drawn depth trails the newest target.
// A disparity is the vergence angle between two depths at a nominal 63 mm IPD: 0.063 m x |1/D_a - 1/D_b|, in degrees. Past 0.25 deg the eyes must converge
// again or see the reticle double. Nothing here changes what is drawn.
#pragma once

#include "native_on_foot_f85.h"
#include "native_on_foot_f86.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace edvr {
namespace nofoot {
namespace f118 {

constexpr double kIpdM = 0.063;          // the nominal IPD the disparities are stated at
constexpr double kStepDeg = 0.25;        // a vergence change past the eyes' fusion limit
constexpr double kFlipBackDeg = 0.10;    // a step is undone when the target comes back within this of where it stood before the step
constexpr uint32_t kFlipWindow = 6;      // readbacks after a step within which coming back counts as a flip
constexpr double kRingDeg = 0.6;         // the footprint's half-size about the aim line
constexpr int kRing = 4;                 // right, left, up, down

// The disparity between two depths in degrees (0 when either is not a depth).
inline double disparityDeg(double dA, double dB) noexcept {
    if (!(dA > 0.0) || !(dB > 0.0)) return 0.0;
    return std::fabs(kIpdM * (1.0 / dA - 1.0 / dB)) * f85::kDeg;
}

// A readback line's depth for the footprint: a hit's distance, the march's far end for the sky (as F85c places the reticle); false for a surface nearer than
// the march's start, a line off the screen or an unused line.
inline bool lineDepth(const f86::LineOut& l, double* d) noexcept {
    if (l.status == f86::kLineHit) {
        *d = std::max(f85::kReticleMinM, std::min(f85::kReticleMaxM, l.d));
        return true;
    }
    if (l.status == f86::kLineMiss) {
        *d = f85::kReticleMaxM;
        return true;
    }
    return false;
}

// The footprint's four directions: u turned by kRingDeg toward the camera's right, left, up and down. right and up need not be unit or exactly
// perpendicular to u (K's columns); false when they do not span the plane across u.
inline bool ringDirs(const double u[3], const double right[3], const double up[3], double out[kRing][3]) noexcept {
    const double lu = f85::len3(u);
    if (!(lu > 1e-12)) return false;
    const double f[3] = {u[0] / lu, u[1] / lu, u[2] / lu};
    double r[3], v[3];
    const double rf = f85::dot3(right, f);
    for (int i = 0; i < 3; ++i) r[i] = right[i] - rf * f[i];
    const double lr = f85::len3(r);
    if (!(lr > 1e-9)) return false;
    for (double& x : r) x /= lr;
    const double vf = f85::dot3(up, f), vr = f85::dot3(up, r);
    for (int i = 0; i < 3; ++i) v[i] = up[i] - vf * f[i] - vr * r[i];
    const double lv = f85::len3(v);
    if (!(lv > 1e-9)) return false;
    for (double& x : v) x /= lv;
    const double c = std::cos(kRingDeg / f85::kDeg), s = std::sin(kRingDeg / f85::kDeg);
    const double* axis[kRing] = {r, r, v, v};
    const double sign[kRing] = {1.0, -1.0, 1.0, -1.0};
    for (int q = 0; q < kRing; ++q)
        for (int i = 0; i < 3; ++i) out[q][i] = c * f[i] + sign[q] * s * axis[q][i];
    return true;
}

// One readback's footprint: the centre line (line 0) and the four ring lines. ok when all five gave a depth; then the span between the nearest and the
// farthest of the five, and whether the centre stands on the far side (a ring line found a surface nearer than the centre's by more than kStepDeg).
struct Footprint {
    bool ok = false;
    double spanDeg = 0.0;
    bool centreFar = false;
};
inline Footprint footprint(const f86::LineOut& centre, const f86::LineOut ring[kRing]) noexcept {
    Footprint fp;
    double dc = 0.0;
    if (!lineDepth(centre, &dc)) return fp;
    double dNear = dc, dFar = dc, ringNear = 1e30;
    for (int q = 0; q < kRing; ++q) {
        double d = 0.0;
        if (!lineDepth(ring[q], &d)) return fp;
        dNear = std::min(dNear, d);
        dFar = std::max(dFar, d);
        ringNear = std::min(ringNear, d);
    }
    fp.ok = true;
    fp.spanDeg = disparityDeg(dNear, dFar);
    fp.centreFar = ringNear < dc && disparityDeg(ringNear, dc) > kStepDeg;
    return fp;
}

// The readback targets in order (the hits and the sky; a near surface or a line off the screen leaves F85c's target as it was and is not fed here): the
// steps over kStepDeg, sky-hit (one side the sky) or hit-hit, and the flips (a step whose target comes back within kFlipBackDeg of the depth before it,
// within kFlipWindow readbacks). The second's tallies go with second(); the last target and an open step carry over.
struct Steps {
    uint64_t reads = 0, steps = 0, skyHit = 0, hitHit = 0, flips = 0;
    double largestDeg = 0.0;
    bool have = false, prevSky = false;
    double prevD = 0.0;
    bool open = false;
    double openFrom = 0.0;
    uint32_t openAge = 0;
    void read(bool sky, double d) noexcept {
        ++reads;
        if (!have) {
            have = true;
            prevSky = sky;
            prevD = d;
            return;
        }
        if (open) {
            if (disparityDeg(d, openFrom) <= kFlipBackDeg) {
                ++flips;
                open = false;
            } else if (++openAge >= kFlipWindow) {
                open = false;
            }
        }
        const double s = disparityDeg(d, prevD);
        largestDeg = std::max(largestDeg, s);
        if (s > kStepDeg) {
            ++steps;
            if (sky != prevSky) ++skyHit;
            else ++hitHit;
            open = true;
            openFrom = prevD;
            openAge = 0;
        }
        prevSky = sky;
        prevD = d;
    }
    void second() noexcept {
        reads = steps = skyHit = hitHit = flips = 0;
        largestDeg = 0.0;
    }
};

// Each frame's drawn depth D_f against the newest target: the frames more than kStepDeg apart, the longest such run in ms (a run carries over the second's
// end), the worst.
struct Trail {
    uint64_t frames = 0, over = 0;
    double runMs = 0.0, longestMs = 0.0, worstDeg = 0.0;
    void frame(double drawnD, double targetD, double dtMs) noexcept {
        ++frames;
        const double e = disparityDeg(drawnD, targetD);
        worstDeg = std::max(worstDeg, e);
        if (e > kStepDeg) {
            ++over;
            runMs += dtMs > 0.0 ? dtMs : 0.0;
            longestMs = std::max(longestMs, runMs);
        } else {
            runMs = 0.0;
        }
    }
    void second() noexcept {
        frames = over = 0;
        longestMs = worstDeg = 0.0;
    }
};

// The footprints of a second.
struct FootSecond {
    uint64_t read = 0, edge = 0, centreFar = 0, incomplete = 0, notDispatched = 0;
    double largestDeg = 0.0;
    void add(const Footprint& fp) noexcept {
        if (!fp.ok) {
            ++incomplete;
            return;
        }
        ++read;
        largestDeg = std::max(largestDeg, fp.spanDeg);
        if (fp.spanDeg > kStepDeg) {
            ++edge;
            if (fp.centreFar) ++centreFar;
        }
    }
    void second() noexcept { *this = FootSecond{}; }
};

}  // namespace f118
}  // namespace nofoot
}  // namespace edvr
