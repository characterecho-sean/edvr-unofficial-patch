#pragma once
// THE SHADOW OF THE SIBLING MODEL (design doc section 104, the 10-07 settlement walk): the CPU form of the model, its counters and its line,
// and the pins on the wiring of the shaders and of the capture class that runs them. Nothing here needs a device;
// tools\weapon_motion_test\flat_shadow_gpu_tests.h holds the two compute shaders to the model drawn here, vertex for vertex.
//
// The model (src\d3d11\flat_foreground_shadow.h) fits an affine screen-space motion field to the draws of one identity and asks, for a draw
// that matched its own history, how well the fit and the production's mean predict its own motion. The scenes below are built the way the game
// builds a weapon: a lattice of vertices over a region of the screen, taken through a pinhole camera by a true 3-D rigid motion (rotation
// about a grip plus a translation), cut into pieces of one identity. Positions are in units of half the render size (u, v), motion in pixels.
//
// WHAT THE SCENES SHOW (measured with this file at 2560 x 1440 and a 60 degree horizontal field of view; the numbers are in the expectations):
//  - an in-plane (roll) rotation of a plane facing the camera is exactly affine in the screen position: the fit leaves nothing (under 0.01 px)
//    from 0.3 to 2 degrees, while the mean misses by 0.8 to 5 px and the production policy (spread over one pixel) refuses; with a tenth of
//    depth relief and a drift the fit leaves 0.02 to 0.07 px;
//  - a 3-D rotation of a plane is not affine: the field is a homography, whose perspective term is first order in the angle and second order in the
//    weapon's extent. For a 448 x 216 px weapon the fit leaves 0.03 to 0.04 px at 0.3 degrees, 0.11 px at one and 0.23 px at two (yaw, pitch and
//    a diagonal axis, with relief and drift), 4 to 20 times better than the mean; for a 1152 px weapon (45 percent of the screen) it leaves
//    1.9 px at 2 degrees, over the gate, and the model refuses it. So "under 0.1 px" holds for a roll at any angle and for a 3-D rotation only
//    up to about half a degree at weapon size; the error is the model's, not noise: it doubles with the angle and quadruples with twice the width
//    (both checked below).
//  - the hull gate is the model's refusal of an end piece: a weapon cut in strips has end strips outside what their donors cover, and only those fail.
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <string>
#include <utility>
#include <vector>
#include "../../src/d3d11/flat_foreground_shadow.h"

namespace flatshadow_test {

inline constexpr double kPi = 3.14159265358979323846;
inline constexpr double kW = 2560.0, kH = 1440.0;   // the render size in pixels

struct Camera {
    double fovDegrees = 60.0;   // horizontal
    double f() const { return 0.5 * kW / std::tan(fovDegrees * kPi / 360.0); }
};
struct P3 { double x = 0, y = 0, z = 0; };
inline P3 unproject(const Camera& c, double u, double v, double w) {
    const double f = c.f();
    return {u * (kW / 2) * w / f, v * (kH / 2) * w / f, w};
}

// A rigid motion: a rotation of `degrees` about `axis` through `pivot`, then a translation.
struct Rigid {
    P3 pivot, axis{0, 0, 1};
    double degrees = 0;
    P3 shift;
    P3 apply(const P3& p) const {
        const double n = std::sqrt(axis.x * axis.x + axis.y * axis.y + axis.z * axis.z);
        const P3 k{axis.x / n, axis.y / n, axis.z / n};
        const double a = degrees * kPi / 180, c = std::cos(a), s = std::sin(a);
        const P3 d{p.x - pivot.x, p.y - pivot.y, p.z - pivot.z};
        const P3 kd{k.y * d.z - k.z * d.y, k.z * d.x - k.x * d.z, k.x * d.y - k.y * d.x};
        const double dot = k.x * d.x + k.y * d.y + k.z * d.z;
        return {pivot.x + d.x * c + kd.x * s + k.x * dot * (1 - c) + shift.x, pivot.y + d.y * c + kd.y * s + k.y * dot * (1 - c) + shift.y,
                pivot.z + d.z * c + kd.z * s + k.z * dot * (1 - c) + shift.z};
    }
};

// The vertex at screen (u, v) and view depth w, and where the rigid motion takes it: position, the motion in pixels (new less old), depth.
inline edvr::ShadowVertex moved(const Camera& c, double u, double v, double w, const Rigid& r) {
    const double f = c.f();
    const P3 q = r.apply(unproject(c, u, v, w));
    edvr::ShadowVertex s;
    s.u = static_cast<float>(u); s.v = static_cast<float>(v); s.w = static_cast<float>(w);
    s.mx = static_cast<float>((q.x * f / q.z / (kW / 2) - u) * (kW / 2));
    s.my = static_cast<float>((q.y * f / q.z / (kH / 2) - v) * (kH / 2));
    return s;
}

struct Region { double u0, u1, v0, v1; };
inline constexpr Region kWholeScreenWeapon{-0.3, 0.6, 0.1, 0.9};   // 1152 x 576 px: the extreme of what a weapon could cover
inline constexpr Region kWeapon{-0.2, 0.15, 0.55, 0.85};           // 448 x 216 px

// The plane's depth: 0.5, tilted by +-0.035 across the region's u and +-0.015 across its v when `relief` is 1 (z in [0.45, 0.55]).
inline double depthAt(const Region& r, double relief, double u, double v) {
    return 0.5 + relief * (0.035 * (u - 0.5 * (r.u0 + r.u1)) / (0.5 * (r.u1 - r.u0)) + 0.015 * (v - 0.5 * (r.v0 + r.v1)) / (0.5 * (r.v1 - r.v0)));
}

inline constexpr uint32_t kIdentityX = 0x00A1B2C3u, kIdentityY = 0x0000D4E5u;
inline edvr::ShadowDrawRef emptyDraw(bool matched = true, uint32_t x = kIdentityX, uint32_t y = kIdentityY) {
    edvr::ShadowDrawRef d;
    d.matched = matched; d.identityX = x; d.identityY = y;
    return d;
}

// A weapon: a 36 x 40 lattice over the region (1440 vertices), each taken by `move`, cut into six pieces of 170 to 340 vertices. The pieces
// interleave over the whole region (a draw per material of one mesh), so that the donors of any piece surround it; `strips` cuts six vertical
// strips instead, where the end pieces lie outside what their donors cover. `jitter` moves each vertex by up to 0.005 so that no lattice line
// is exact.
inline std::vector<edvr::ShadowDrawRef> weapon(const Camera& cam, const Region& reg, double relief, const Rigid& move, bool strips = false,
                                               double jitter = 1.0) {
    constexpr int nu = 36, nv = 40;
    std::vector<edvr::ShadowDrawRef> draws(6, emptyDraw());
    for (int i = 0; i < nu; ++i)
        for (int j = 0; j < nv; ++j) {
            double u = reg.u0 + (reg.u1 - reg.u0) * i / (nu - 1), v = reg.v0 + (reg.v1 - reg.v0) * j / (nv - 1);
            u += jitter * 0.005 * std::sin(12.9898 * i + 78.233 * j);
            v += jitter * 0.005 * std::sin(39.346 * i + 11.135 * j);
            const int h = (7 * i + 13 * j) % 17;
            const unsigned piece = strips ? static_cast<unsigned>(i * 6 / nu) : (h < 4 ? 0u : h < 7 ? 1u : h < 9 ? 2u : h < 12 ? 3u : h < 14 ? 4u : 5u);
            draws[piece].vertices.push_back(moved(cam, u, v, depthAt(reg, relief, u, v), move));
        }
    return draws;
}

inline Rigid gripRotation(const Camera& cam, const Region& reg, const P3& axis, double degrees, const P3& shift = P3{}) {
    Rigid r;
    r.pivot = unproject(cam, reg.u1 - 0.05, reg.v1 - 0.05, 0.52);   // the grip: low and to the right of the weapon
    r.axis = axis; r.degrees = degrees; r.shift = shift;
    return r;
}

// What a scene's records say, over every record of the kind asked for.
struct Summary {
    unsigned count = 0;
    double worstAffine = 0, bestMean = 1e30, worstMean = 0, maxResidual = 0, minSpread = 1e30, maxRatio = 0;
    bool allAffineAccept = true, anyCurrentAccept = false, allKind = true;
};
inline Summary summarize(const std::vector<edvr::ShadowRecord>& recs, edvr::ShadowKind kind = edvr::ShadowKind::MatchedWithDonors) {
    Summary s;
    for (const edvr::ShadowRecord& r : recs) {
        if (static_cast<unsigned>(r.kind + 0.5f) != static_cast<unsigned>(kind)) { s.allKind = false; continue; }
        ++s.count;
        s.worstAffine = std::fmax(s.worstAffine, r.rmsAffine);
        s.bestMean = std::fmin(s.bestMean, r.rmsMean);
        s.worstMean = std::fmax(s.worstMean, r.rmsMean);
        s.maxResidual = std::fmax(s.maxResidual, r.residual);
        s.minSpread = std::fmin(s.minSpread, r.spread);
        s.maxRatio = std::fmax(s.maxRatio, r.rmsAffine / std::fmax(r.rmsMean, 1e-9));
        s.allAffineAccept = s.allAffineAccept && r.affineAccepts();
        s.anyCurrentAccept = s.anyCurrentAccept || r.currentAccepts();
    }
    return s;
}
// The affine gate bits missing from every record, or 0xFFFF when the records disagree.
inline unsigned missingAffine(const std::vector<edvr::ShadowRecord>& recs) {
    unsigned want = 0xFFFFu;
    for (const edvr::ShadowRecord& r : recs) {
        const unsigned m = edvr::kGateAffine & ~static_cast<unsigned>(r.gates);
        if (want == 0xFFFFu) want = m;
        else if (want != m) return 0xFFFFu;
    }
    return want == 0xFFFFu ? 0xFFFEu : want;
}

// The model computed the long way, as a check on the reference's arithmetic: every donor vertex of a draw in one list, means and sums of squares in
// two passes in long double (the reference merges per-draw moments by Chan's formula), the normal equations solved by Cramer's rule, and the residual
// summed vertex by vertex. `close` is the smallest distance of a gated quantity to its threshold, relative: a draw that near a threshold is not
// compared on its gate bits (the two forms may round to either side).
struct OracleRecord {
    unsigned kind = 0, donorDraws = 0, gates = 0;
    double pooled = 0, spread = 0, residual = 0, rmsMean = 0, rmsAffine = 0, ownMean[2] = {0, 0}, close = 1e9;
};
inline OracleRecord oracle(const std::vector<edvr::ShadowDrawRef>& draws, size_t index) {
    using namespace edvr;
    OracleRecord r;
    const ShadowDrawRef& d = draws[index];
    if (!d.identityReadable) { r.kind = static_cast<unsigned>(ShadowKind::IdentityUnreadable); return r; }
    std::vector<const ShadowVertex*> pool;
    for (size_t j = 0; j < draws.size(); ++j) {
        if (j == index || !draws[j].matched || draws[j].vertices.empty() || draws[j].identityX != d.identityX || draws[j].identityY != d.identityY) continue;
        ++r.donorDraws;
        for (const ShadowVertex& v : draws[j].vertices) pool.push_back(&v);
    }
    if (d.matched && d.vertices.empty()) { r.donorDraws = 0; return r; }
    r.kind = static_cast<unsigned>(d.matched ? (r.donorDraws ? ShadowKind::MatchedWithDonors : ShadowKind::MatchedAlone)
                                             : (r.donorDraws ? ShadowKind::ReceiverWithDonors : ShadowKind::ReceiverAlone));
    if (d.matched) {
        long double sx = 0, sy = 0;
        for (const ShadowVertex& v : d.vertices) { sx += v.mx; sy += v.my; }
        r.ownMean[0] = static_cast<double>(sx / d.vertices.size()); r.ownMean[1] = static_cast<double>(sy / d.vertices.size());
    }
    if (!r.donorDraws) return r;
    const long double n = static_cast<long double>(pool.size());
    long double mean[4] = {0, 0, 0, 0}, lo[4] = {1e30L, 1e30L, 1e30L, 1e30L}, hi[4] = {-1e30L, -1e30L, -1e30L, -1e30L}, wlo = 1e30L, whi = -1e30L;
    for (const ShadowVertex* v : pool) {
        const long double x[4] = {v->u, v->v, v->mx, v->my};
        for (int a = 0; a < 4; ++a) { mean[a] += x[a] / n; lo[a] = std::fmin(lo[a], x[a]); hi[a] = std::fmax(hi[a], x[a]); }
        wlo = std::fmin(wlo, static_cast<long double>(v->w)); whi = std::fmax(whi, static_cast<long double>(v->w));
    }
    long double suu = 0, suv = 0, svv = 0, sux = 0, suy = 0, svx = 0, svy = 0;
    for (const ShadowVertex* v : pool) {
        const long double du = v->u - mean[0], dv = v->v - mean[1], dx = v->mx - mean[2], dy = v->my - mean[3];
        suu += du * du; suv += du * dv; svv += dv * dv; sux += du * dx; suy += du * dy; svx += dv * dx; svy += dv * dy;
    }
    const long double det = suu * svv - suv * suv, tr = suu + svv;
    const long double conditioning = tr > 0 ? det / (tr * tr) : 0;
    const bool solved = det > 0 && tr > 0;
    long double ax = 0, bx = 0, ay = 0, by = 0, rss = 0;
    if (solved) {
        ax = (svv * sux - suv * svx) / det; bx = (suu * svx - suv * sux) / det;
        ay = (svv * suy - suv * svy) / det; by = (suu * svy - suv * suy) / det;
        for (const ShadowVertex* v : pool) {
            const long double ex = v->mx - mean[2] - ax * (v->u - mean[0]) - bx * (v->v - mean[1]);
            const long double ey = v->my - mean[3] - ay * (v->u - mean[0]) - by * (v->v - mean[1]);
            rss += ex * ex + ey * ey;
        }
    }
    r.pooled = static_cast<double>(n);
    r.spread = static_cast<double>(std::fmax(hi[2] - lo[2], hi[3] - lo[3]));
    r.residual = solved ? static_cast<double>(std::sqrt(rss / n)) : 0.0;
    // the draw's extent
    long double ulo = 1e30L, uhi = -1e30L, vlo = 1e30L, vhi = -1e30L, rwlo = 1e30L, rwhi = -1e30L;
    const auto extent = [&](long double u, long double v, long double w) {
        ulo = std::fmin(ulo, u); uhi = std::fmax(uhi, u); vlo = std::fmin(vlo, v); vhi = std::fmax(vhi, v); rwlo = std::fmin(rwlo, w); rwhi = std::fmax(rwhi, w);
    };
    if (!d.hull.empty()) for (const ShadowHullVertex& h : d.hull) extent(h.u, h.v, h.w);
    else for (const ShadowVertex& v : d.vertices) extent(v.u, v.v, v.w);
    const long double mu = kFlatShadowHullMargin * (hi[0] - lo[0]) + kFlatShadowHullFloor, mv = kFlatShadowHullMargin * (hi[1] - lo[1]) + kFlatShadowHullFloor;
    const auto nearness = [&](long double value, long double threshold) {
        r.close = std::fmin(r.close, static_cast<double>(std::fabs(value - threshold) / std::fmax(1.0L, std::fabs(threshold))));
    };
    nearness(conditioning, kFlatShadowConditioning); nearness(r.residual, kFlatShadowResidualPixels); nearness(r.spread, kFlatSiblingSpreadPixels);
    if (ulo <= uhi) {
        nearness(ulo, lo[0] - mu); nearness(uhi, hi[0] + mu); nearness(vlo, lo[1] - mv); nearness(vhi, hi[1] + mv);
        nearness(rwlo, wlo / kFlatShadowDepthRatio); nearness(rwhi, whi * kFlatShadowDepthRatio);
    }
    unsigned g = 0;
    if (n >= kFlatSiblingMinVertices) g |= kGateMinVertices;
    if (r.spread <= kFlatSiblingSpreadPixels) g |= kGateSpread;
    if (n >= kFlatShadowMinFitVertices) g |= kGateFitVertices;
    if (solved && conditioning >= kFlatShadowConditioning) g |= kGateConditioning;
    if (solved && r.residual <= kFlatShadowResidualPixels) g |= kGateResidual;
    if (ulo <= uhi && ulo >= lo[0] - mu && uhi <= hi[0] + mu && vlo >= lo[1] - mv && vhi <= hi[1] + mv) g |= kGateHull;
    if (ulo <= uhi && rwlo >= wlo / kFlatShadowDepthRatio && rwhi <= whi * kFlatShadowDepthRatio) g |= kGateDepth;
    r.gates = g;
    if (d.matched) {
        long double sm = 0, sa = 0;
        for (const ShadowVertex& v : d.vertices) {
            const long double ex = v.mx - mean[2], ey = v.my - mean[3];
            sm += ex * ex + ey * ey;
            const long double fx = v.mx - mean[2] - ax * (v.u - mean[0]) - bx * (v.v - mean[1]), fy = v.my - mean[3] - ay * (v.u - mean[0]) - by * (v.v - mean[1]);
            sa += fx * fx + fy * fy;
        }
        const long double nv = static_cast<long double>(d.vertices.size());
        r.rmsMean = static_cast<double>(std::sqrt(sm / nv)); r.rmsAffine = static_cast<double>(std::sqrt(sa / nv));
    }
    return r;
}

// A random scene: two to seven draws of one or two identities, matched or not, each a cluster of a few to 80 vertices of its own size, place and
// depth, moving by its identity's affine field plus noise and sometimes an offset of its own.
inline std::vector<edvr::ShadowDrawRef> randomScene(uint32_t seed) {
    using namespace edvr;
    uint32_t state = seed * 2654435761u + 12345u;
    const auto rnd = [&state] { state = state * 1664525u + 1013904223u; return static_cast<double>(state >> 8) / 16777216.0; };
    const auto pick = [&](double lo, double hi) { return lo + (hi - lo) * rnd(); };
    struct Field { double m0[2], a[4], noise; };
    Field fields[2];
    for (Field& f : fields) {
        f.m0[0] = pick(-5, 5); f.m0[1] = pick(-5, 5);
        for (double& a : f.a) a = pick(-6, 6);
        f.noise = rnd() < 0.4 ? 0.0 : rnd() < 0.5 ? 0.3 : 2.0;
    }
    const size_t count = 2 + static_cast<size_t>(rnd() * 6);
    std::vector<ShadowDrawRef> draws(count);
    for (ShadowDrawRef& d : draws) {
        const unsigned id = rnd() < 0.7 ? 0u : 1u;
        d = emptyDraw(rnd() < 0.75, kIdentityX ^ id, kIdentityY);
        d.identityReadable = rnd() > 0.05;
        const double cu = pick(-0.5, 0.5), cv = pick(-0.5, 0.5), radius = pick(0.01, 0.4), w0 = pick(0.3, 4.0);
        const double relief = rnd() < 0.5 ? 0.0 : rnd() < 0.5 ? 0.1 : 0.5;
        const double offset = rnd() < 0.5 ? 0.0 : pick(-3, 3);
        const unsigned n = rnd() < 0.1 ? 0u : rnd() < 0.1 ? 1u + static_cast<unsigned>(rnd() * 2) : 3u + static_cast<unsigned>(rnd() * 78);
        const double radiusV = rnd() < 0.2 ? radius * 0.003 : radius;   // a cluster on a line, now and then
        const Field& f = fields[id];
        for (unsigned i = 0; i < n; ++i) {
            ShadowVertex v;
            const double u = cu + radius * (2 * rnd() - 1), vv = cv + radiusV * (2 * rnd() - 1);
            v.u = static_cast<float>(u); v.v = static_cast<float>(vv); v.w = static_cast<float>(w0 * (1 + relief * (2 * rnd() - 1)));
            v.mx = static_cast<float>(f.m0[0] + f.a[0] * u + f.a[1] * vv + f.noise * (2 * rnd() - 1) + offset);
            v.my = static_cast<float>(f.m0[1] + f.a[2] * u + f.a[3] * vv + f.noise * (2 * rnd() - 1));
            if (d.matched) d.vertices.push_back(v);
            else d.hull.push_back({v.u, v.v, v.w});
        }
    }
    return draws;
}

}  // namespace flatshadow_test

inline int flatShadowModelTests() {
    using namespace edvr;
    using namespace flatshadow_test;
    int failures = 0;
    const auto expect = [&](bool ok, const char* name) {
        if (!ok) { std::printf("FAIL: shadow model %s\n", name); ++failures; }
    };
    // As expect, and says the value that missed.
    const auto expectIn = [&](double v, double lo, double hi, const char* name) {
        if (!(v >= lo && v <= hi)) { std::printf("FAIL: shadow model %s (got %.6g, wanted %.6g to %.6g)\n", name, v, lo, hi); ++failures; }
    };
    const auto allKind = [](const std::vector<ShadowRecord>& recs, ShadowKind kind) {
        for (const ShadowRecord& r : recs)
            if (static_cast<unsigned>(r.kind + 0.5f) != static_cast<unsigned>(kind)) return false;
        return !recs.empty();
    };
    const Camera cam;   // 60 degrees: f = 2217 px
    const P3 roll{0, 0, 1}, yaw{0, 1, 0}, pitch{1, 0, 0}, diagonal{1, 1, 1};
    const P3 drift{0.001, -0.0007, 0.0006};   // a translation of the weapon, 0.001 of its distance: about 2 px

    // -- 1. a rigid, rotating weapon: six pieces of one identity, each predicted from the other five --
    {
        // In-plane rotation of a plane facing the camera: exactly affine.
        const double angles[] = {0.3, 0.5, 1.0, 2.0};
        double lastMean = 0;
        for (double a : angles) {
            const auto draws = weapon(cam, kWeapon, 0.0, gripRotation(cam, kWeapon, roll, a));
            const auto recs = flatShadowReference(draws);
            const Summary s = summarize(recs);
            bool sizes = draws.size() == 6;
            for (const auto& d : draws) sizes = sizes && d.vertices.size() >= 150 && d.vertices.size() <= 400;
            expect(sizes && s.count == 6 && s.allKind, "six pieces of 150 to 400 vertices, each matched with five donors");
            expectIn(s.worstAffine, 0, 0.01, "an in-plane rotation of a plane is affine to float precision: the fit leaves under 0.01 px (0.3 to 2 degrees)");
            expect(s.bestMean > 2 * a && s.bestMean > 3 * s.worstAffine, "while the mean model misses by pixels, many times the fit's error");
            expect(s.bestMean > lastMean, "and the mean's error grows with the angle");
            lastMean = s.bestMean;
            expect(s.allAffineAccept, "every piece passes the affine gate");
            bool refused = true;
            for (const ShadowRecord& r : recs) refused = refused && !r.currentAccepts() && (static_cast<unsigned>(r.gates) & kGateSpread) == 0 && r.spread > 1.0f;
            expect(refused, "and the production policy refuses it: the donors' spread is over a pixel (the disagreement the settlement walk showed)");
            expect(missingAffine(recs) == 0, "no affine gate is missing for any piece");
            bool donors = true;
            for (const ShadowRecord& r : recs) donors = donors && r.donorDraws == 5.0f && r.pooled >= 1000.0f && r.evaluated >= 150.0f;
            expect(donors, "each piece has five donors, the rest of the lattice, and is compared on all its own vertices");
        }
        // The same with relief (the plane tilted: z in [0.45, 0.55]) and a drift: depth now changes the motion by a tenth, which an affine field cannot hold.
        for (double a : angles) {
            const auto recs = flatShadowReference(weapon(cam, kWeapon, 1.0, gripRotation(cam, kWeapon, roll, a, drift)));
            const Summary s = summarize(recs);
            expectIn(s.worstAffine, 0, 0.1, "a rolled weapon with a tenth of depth relief and a drift is affine to under 0.1 px");
            expect(s.maxRatio < 0.05 && s.allAffineAccept, "and the fit removes over 95 percent of the mean model's error and passes the gate");
        }
        // The whole-screen weapon rolled: the same exactness at a larger scale.
        {
            const auto recs = flatShadowReference(weapon(cam, kWholeScreenWeapon, 0.0, gripRotation(cam, kWholeScreenWeapon, roll, 2.0)));
            const Summary s = summarize(recs);
            expectIn(s.worstAffine, 0, 0.02, "a 1152 px weapon rolled by two degrees is still affine to float precision");
            expect(s.bestMean > 10, "(its mean model misses by over ten pixels)");
        }
    }
    {
        // A true 3-D rotation of a plane: a homography. About the three axes, 0.3 to 2 degrees, a 448 x 216 px weapon.
        struct Axis { const P3* axis; const char* name; };
        const Axis axes[] = {{&yaw, "yaw"}, {&pitch, "pitch"}, {&diagonal, "diagonal"}};
        for (const Axis& ax : axes) {
            double previous = 0, previousMean = 0;
            for (double a : {0.3, 0.5, 1.0, 2.0}) {
                const auto recs = flatShadowReference(weapon(cam, kWeapon, 1.0, gripRotation(cam, kWeapon, *ax.axis, a, drift)));
                const Summary s = summarize(recs);
                expect(s.count == 6 && s.allKind, "3-D rotation: six matched pieces with donors");
                expectIn(s.worstAffine, 0.0, 0.35, "3-D rotation of a 448 px weapon: the fit leaves under 0.35 px at up to two degrees (perspective term)");
                expectIn(s.maxResidual, 0.0, 0.35, "and the donors' own residual is the same size");
                expect(s.maxRatio < 0.5, "and the fit is at least twice as good as the mean for every piece");
                expect(s.allAffineAccept, "the affine gate accepts the weapon at every one of these angles (residual under 0.5 px)");
                if (previous > 0) {
                    expect(s.worstAffine > previous, "the perspective residual grows with the angle");
                    expect(s.bestMean > previousMean, "and so does the mean's error");
                }
                previous = s.worstAffine; previousMean = s.bestMean;
                if (a >= 1.0) {
                    bool refused = true;
                    for (const ShadowRecord& r : recs) refused = refused && !r.currentAccepts();
                    expect(refused, "from one degree the production policy refuses it where the affine gate accepts");
                }
            }
        }
        // The residual is first order in the angle: doubling the angle doubles it (within 15 percent), so it is the model's perspective term and
        // not noise.
        const Summary a1 = summarize(flatShadowReference(weapon(cam, kWeapon, 0.0, gripRotation(cam, kWeapon, yaw, 1.0))));
        const Summary a2 = summarize(flatShadowReference(weapon(cam, kWeapon, 0.0, gripRotation(cam, kWeapon, yaw, 2.0))));
        expectIn(a2.worstAffine / a1.worstAffine, 1.85, 2.15, "the perspective residual doubles with the angle");
        // And second order in the extent: a weapon of twice the width leaves about four times the error.
        const Region wide{-0.2, 0.5, 0.55, 0.85};
        const Summary w2 = summarize(flatShadowReference(weapon(cam, wide, 0.0, gripRotation(cam, wide, yaw, 1.0))));
        expectIn(w2.worstAffine / a1.worstAffine, 3.0, 5.0, "and grows with the square of the weapon's width (twice as wide, about four times the residual)");
        // At the whole-screen size the same two degrees break the residual gate: the donors cannot be explained, and the model says so.
        {
            const auto recs = flatShadowReference(weapon(cam, kWholeScreenWeapon, 0.0, gripRotation(cam, kWholeScreenWeapon, yaw, 2.0)));
            const Summary s = summarize(recs);
            expect(s.maxResidual > 1.0 && !s.allAffineAccept && missingAffine(recs) == kGateResidual,
                   "a 1152 px weapon yawed by two degrees leaves over a pixel of residual: kGateResidual fails and nothing else does");
            const auto control = flatShadowReference(weapon(cam, kWholeScreenWeapon, 0.0, gripRotation(cam, kWholeScreenWeapon, yaw, 0.1)));
            expect(missingAffine(control) == 0 && summarize(control).allAffineAccept,
                   "control: the same weapon at a tenth of a degree passes (it is the angle that broke it)");
        }
    }
    {
        // Where the mean worked, the fit does not do worse: a uniform drift of the whole weapon (no rotation).
        const auto recs = flatShadowReference(weapon(cam, kWeapon, 0.0, gripRotation(cam, kWeapon, roll, 0.0, P3{0.002, 0.0015, 0})));
        const Summary s = summarize(recs);
        expect(s.worstMean < 1e-3 && s.worstAffine < 1e-3, "a uniform translation: both models are exact");
        bool both = true;
        for (const ShadowRecord& r : recs) both = both && r.currentAccepts() && r.affineAccepts() && r.spread < 0.01f;
        expect(both, "and both policies accept: the affine model gives up nothing where the mean already worked");
        // Parallax: the drift of a relief weapon (z in [0.45, 0.55]) moves the near side more than the far.
        const auto relief = flatShadowReference(weapon(cam, kWeapon, 1.0, gripRotation(cam, kWeapon, roll, 0.0, P3{0.02, 0, 0})));
        const Summary r = summarize(relief);
        expect(r.minSpread > 1.0 && r.worstAffine < 0.3 && r.allAffineAccept,
               "a drift of a weapon with depth relief is not uniform (spread over a pixel, the policy refuses) but is nearly affine in position");
    }

    // -- 2. refusals, each with the same scene with the defect removed --
    const auto base = [&] { return weapon(cam, kWeapon, 0.0, gripRotation(cam, kWeapon, roll, 1.0)); };
    {
        expect(missingAffine(flatShadowReference(base())) == 0, "the base scene passes every affine gate (the control of the refusals below)");
    }
    {
        // (a) donors the affine model cannot explain: a step field, and a skinned piece.
        const auto step = [&](float left, float right) {
            auto draws = base();
            for (auto& d : draws)
                for (ShadowVertex& v : d.vertices) { v.mx = v.u < -0.025f ? left : right; v.my = 0; }
            return draws;
        };
        const auto stepped = flatShadowReference(step(3.f, -3.f));
        expect(missingAffine(stepped) == kGateResidual && summarize(stepped).allKind, "a step field (left +3 px, right -3 px): kGateResidual fails, no other affine gate does");
        double residual = 1e9;
        for (const ShadowRecord& r : stepped) residual = std::fmin(residual, r.residual);
        expectIn(residual, 1.2, 1.8, "its residual is about half the step: 1.5 px");
        expect(missingAffine(flatShadowReference(step(3.f, 3.f))) == 0, "control: the same field without the step (both halves +3 px) passes");
        expect(missingAffine(flatShadowReference(step(3.f, 0.f))) == kGateResidual, "a step of three pixels (left +3, right 0) fails too");
        expect(missingAffine(flatShadowReference(step(0.4f, -0.4f))) == 0 && missingAffine(flatShadowReference(step(1.6f, -1.6f))) == kGateResidual,
               "and the gate's edge is where it says: steps of +-0.4 px (residual 0.2) pass, +-1.6 px (residual 0.8) fail");
        // Skinned: the left half rotates one way about its own pivot, the right half the other way about another.
        const auto skin = [&](double left, double right) {
            auto draws = weapon(cam, kWeapon, 0.0, Rigid{}, false);
            const Rigid a = gripRotation(cam, kWeapon, roll, left), b = [&] {
                Rigid r = gripRotation(cam, kWeapon, roll, right);
                r.pivot = unproject(cam, kWeapon.u0 + 0.05, kWeapon.v1 - 0.05, 0.52);
                return r;
            }();
            for (auto& d : draws)
                for (ShadowVertex& v : d.vertices) {
                    const ShadowVertex m = moved(cam, v.u, v.v, v.w, v.u < -0.025f ? a : b);
                    v.mx = m.mx; v.my = m.my;
                }
            return draws;
        };
        const auto skinned = flatShadowReference(skin(2.0, -2.0));
        expect(missingAffine(skinned) == kGateResidual, "two halves rotating oppositely about different pivots: kGateResidual fails alone");
        expect(missingAffine(flatShadowReference(skin(1.0, 1.0))) == kGateResidual,
               "(the halves rotating the same way about different pivots are two rigid bodies, not one: refused too)");
        // The control of the skin: the same two halves with one pivot are one rigid body again.
        {
            auto draws = weapon(cam, kWeapon, 0.0, Rigid{}, false);
            const Rigid one = gripRotation(cam, kWeapon, roll, 1.0);
            for (auto& d : draws)
                for (ShadowVertex& v : d.vertices) {
                    const ShadowVertex m = moved(cam, v.u, v.v, v.w, one);
                    v.mx = m.mx; v.my = m.my;
                }
            expect(missingAffine(flatShadowReference(draws)) == 0, "control: the two halves with one pivot and one angle pass");
        }
    }
    {
        // (b) collinear donors: every vertex of every piece on one line v = const.
        const auto line = [&](double vSpread) {
            std::vector<ShadowDrawRef> draws(4, emptyDraw());
            const Rigid r = gripRotation(cam, kWeapon, roll, 1.0);
            for (int i = 0; i < 400; ++i) {
                const double u = -0.2 + 0.35 * i / 399.0;
                const double v = 0.7 + vSpread * std::sin(1.7 * i);
                draws[static_cast<size_t>(i % 4)].vertices.push_back(moved(cam, u, v, 0.5, r));
            }
            return draws;
        };
        const auto exact = flatShadowReference(line(0.0));
        expect(allKind(exact, ShadowKind::MatchedWithDonors), "collinear scene: four pieces, matched, with donors");
        bool unsolved = true;
        for (const ShadowRecord& r : exact) unsolved = unsolved && (static_cast<unsigned>(r.gates) & kGateConditioning) == 0;
        expect(missingAffine(exact) == (kGateConditioning | kGateResidual) && unsolved,
               "donors on one exact line: kGateConditioning fails (and kGateResidual with it: nothing was solved to have a residual)");
        const auto thin = flatShadowReference(line(0.001));
        expect(missingAffine(thin) == kGateConditioning, "donors nearly on a line (v within +-0.001): kGateConditioning fails alone, the fit is solved and its residual small");
        expect(summarize(thin).maxResidual < kFlatShadowResidualPixels, "(that near-line fit's residual is under the gate, the conditioning is what refuses it)");
        const auto spread = flatShadowReference(line(0.1));
        expect(missingAffine(spread) == 0, "control: the same vertices with v spread over +-0.1 pass");
        // the conditioning gate's edge: det/tr^2 of the donors' covariance, kFlatShadowConditioning = 0.005, is about (sigma_v / sigma_u)^2
        expect(missingAffine(flatShadowReference(line(0.0085))) == kGateConditioning && missingAffine(flatShadowReference(line(0.0125))) == 0,
               "and the edge is where the constant says (det/tr^2 is about 49 x the v amplitude squared here, 0.005 at +-0.0101): +-0.0085 fails, +-0.0125 passes");
    }
    {
        // (c) fewer than kFlatShadowMinFitVertices donor vertices: a receiver and one donor of `n` vertices spread over the weapon.
        const auto few = [&](unsigned n) {
            std::vector<ShadowDrawRef> draws(2, emptyDraw());
            const Rigid r = gripRotation(cam, kWeapon, roll, 1.0);
            for (unsigned i = 0; i < n; ++i) {   // the four corners first, then a low-discrepancy spread: any prefix covers the weapon
                const double fu = i < 4 ? double(i & 1) : std::fmod(0.5 + i * 0.6180339887, 1.0), fv = i < 4 ? double(i >> 1) : std::fmod(0.5 + i * 0.7548776662, 1.0);
                draws[1].vertices.push_back(moved(cam, kWeapon.u0 + (kWeapon.u1 - kWeapon.u0) * fu, kWeapon.v0 + (kWeapon.v1 - kWeapon.v0) * fv, 0.5, r));
            }
            for (int i = 0; i < 12; ++i) {   // the receiver, inside
                const double u = -0.1 + 0.15 * (i % 4) / 3.0, v = 0.65 + 0.1 * (i / 4) / 2.0;
                draws[0].vertices.push_back(moved(cam, u, v, 0.5, r));
            }
            return flatShadowReference(draws)[0];
        };
        const ShadowRecord at23 = few(kFlatShadowMinFitVertices - 1), at24 = few(kFlatShadowMinFitVertices);
        expect(at23.pooled == 23.0f && (static_cast<unsigned>(at23.gates) & kGateFitVertices) == 0 &&
                   (kGateAffine & ~static_cast<unsigned>(at23.gates)) == kGateFitVertices,
               "23 donor vertices: kGateFitVertices fails and no other affine gate does (the model fits exactly and is not tested)");
        expect(at24.pooled == 24.0f && (kGateAffine & ~static_cast<unsigned>(at24.gates)) == 0, "control: 24 donor vertices pass every affine gate");
        // The production's own floor is a triangle.
        const ShadowRecord two = few(2), three = few(3);
        expect((static_cast<unsigned>(two.gates) & kGateMinVertices) == 0 && (static_cast<unsigned>(three.gates) & kGateMinVertices) != 0,
               "the production's floor is its own: two donor vertices fail kGateMinVertices, three pass");
        expect(two.kind == static_cast<float>(ShadowKind::MatchedWithDonors) && !two.affineAccepts() && !two.currentAccepts(),
               "and few donors are refused by both");
    }
    {
        // (d) the receiver out of the donors' hull. Strips: the end pieces extend past what their donors cover.
        const auto strips = flatShadowReference(weapon(cam, kWeapon, 0.0, gripRotation(cam, kWeapon, roll, 1.0), true, 0.0));
        bool ends = true, middle = true;
        for (size_t i = 0; i < strips.size(); ++i) {
            const unsigned missing = kGateAffine & ~static_cast<unsigned>(strips[i].gates);
            if (i == 0 || i == strips.size() - 1) ends = ends && missing == kGateHull;
            else middle = middle && missing == 0;
        }
        expect(ends, "a weapon cut in strips: the end strips are outside their donors' extent: kGateHull fails alone though the fit is good");
        expect(middle, "control: the four middle strips (their donors span them) pass everything");
        expectIn(strips[0].residual, 0, 0.01, "(the end strip's fit is exact: the hull is what refuses it)");
        // A receiver that has no motion of its own, only its hull; the margin is kFlatShadowHullMargin of the donors' extent and the floor.
        auto draws = weapon(cam, kWeapon, 0.0, gripRotation(cam, kWeapon, roll, 1.0), false, 0.0);
        const double mu = kFlatShadowHullMargin * (kWeapon.u1 - kWeapon.u0) + kFlatShadowHullFloor;
        const double mv = kFlatShadowHullMargin * (kWeapon.v1 - kWeapon.v0) + kFlatShadowHullFloor;
        const auto receiver = [&](double u0, double u1, double v0, double v1, double w) {
            auto scene = draws;
            ShadowDrawRef r = emptyDraw(false);
            for (double fu : {0.0, 1.0}) for (double fv : {0.0, 1.0}) r.hull.push_back({static_cast<float>(u0 + (u1 - u0) * fu), static_cast<float>(v0 + (v1 - v0) * fv), static_cast<float>(w)});
            scene.push_back(r);
            return flatShadowReference(scene).back();
        };
        const auto missed = [](const ShadowRecord& r) { return kGateAffine & ~static_cast<unsigned>(r.gates); };
        const ShadowRecord inside = receiver(-0.1, 0.1, 0.6, 0.8, 0.5);
        expect(inside.kind == static_cast<float>(ShadowKind::ReceiverWithDonors) && missed(inside) == 0 && inside.rmsMean == 0 && inside.evaluated == 0 &&
                   inside.donorDraws == 6.0f,
               "a receiver inside the weapon: a receiver with six donors, passes, and carries no error (it has no motion of its own)");
        expect(missed(receiver(-0.1, kWeapon.u1 + 0.9 * mu, 0.6, 0.8, 0.5)) == 0, "a receiver 0.9 of the margin past the donors on the right still passes");
        expect(missed(receiver(-0.1, kWeapon.u1 + 1.1 * mu, 0.6, 0.8, 0.5)) == kGateHull, "1.1 of the margin past fails kGateHull alone");
        expect(missed(receiver(kWeapon.u0 - 1.1 * mu, 0.1, 0.6, 0.8, 0.5)) == kGateHull && missed(receiver(kWeapon.u0 - 0.9 * mu, 0.1, 0.6, 0.8, 0.5)) == 0,
               "the same on the left");
        expect(missed(receiver(-0.1, 0.1, 0.6, kWeapon.v1 + 1.1 * mv, 0.5)) == kGateHull && missed(receiver(-0.1, 0.1, 0.6, kWeapon.v1 + 0.9 * mv, 0.5)) == 0,
               "and below");
        expect(missed(receiver(-0.1, 0.1, kWeapon.v0 - 1.1 * mv, 0.8, 0.5)) == kGateHull && missed(receiver(-0.1, 0.1, kWeapon.v0 - 0.9 * mv, 0.8, 0.5)) == 0,
               "and above");
        expect(missed(receiver(0.7, 0.9, 0.6, 0.8, 0.5)) == kGateHull, "a piece placed well outside the weapon's extent fails kGateHull");
        // A receiver with no valid vertex has no extent to be inside anything.
        {
            auto scene = draws;
            scene.push_back(emptyDraw(false));
            const ShadowRecord none = flatShadowReference(scene).back();
            expect(none.kind == static_cast<float>(ShadowKind::ReceiverWithDonors) && (static_cast<unsigned>(none.gates) & (kGateHull | kGateDepth)) == 0,
                   "a receiver with no valid vertex fails the hull and the depth gates (no extent)");
        }
        // (e2) depth: the donors at w = 0.5; a receiver deeper or nearer than the ratio.
        const double hi = 0.5 * kFlatShadowDepthRatio, lo = 0.5 / kFlatShadowDepthRatio;
        expect(missed(receiver(-0.1, 0.1, 0.6, 0.8, hi * 0.99)) == 0 && missed(receiver(-0.1, 0.1, 0.6, 0.8, hi * 1.01)) == kGateDepth,
               "a receiver at a depth past the donors' range by the ratio (1.25): kGateDepth fails alone, just inside it passes");
        expect(missed(receiver(-0.1, 0.1, 0.6, 0.8, lo * 1.01)) == 0 && missed(receiver(-0.1, 0.1, 0.6, 0.8, lo * 0.99)) == kGateDepth,
               "and nearer than the range over the ratio");
        expect(missed(receiver(-0.1, 0.1, 0.6, 0.8, 0.8)) == kGateDepth && missed(receiver(-0.1, 0.1, 0.6, 0.8, 0.5)) == 0,
               "control: a receiver at w = 0.8 against donors at 0.5 fails kGateDepth, at 0.5 passes");
        // a receiver whose depth range straddles an end fails
        {
            auto scene = draws;
            ShadowDrawRef r = emptyDraw(false);
            r.hull.push_back({-0.05f, 0.65f, 0.5f}); r.hull.push_back({0.05f, 0.75f, static_cast<float>(hi * 1.1)});
            scene.push_back(r);
            expect(missed(flatShadowReference(scene).back()) == kGateDepth, "a receiver with one vertex at the donors' depth and another beyond the ratio fails: the whole range must fit");
        }
    }
    {
        // (e) world-like parallax: donors at random depths in [1, 20] with motion proportional to 1/w (a translation of the camera).
        const auto world = [&](bool varyDepth) {
            std::vector<ShadowDrawRef> draws(6, emptyDraw());
            uint32_t seed = 12345;
            const auto rnd = [&seed] { seed = seed * 1664525u + 1013904223u; return static_cast<double>(seed >> 8) / 16777216.0; };
            for (int i = 0; i < 900; ++i) {
                ShadowVertex v;
                v.u = static_cast<float>(-0.4 + 0.8 * rnd()); v.v = static_cast<float>(-0.4 + 0.8 * rnd());
                v.w = varyDepth ? static_cast<float>(1.0 + 19.0 * rnd()) : 5.0f;
                v.mx = 60.0f / v.w; v.my = -20.0f / v.w;
                draws[static_cast<size_t>(i % 6)].vertices.push_back(v);
            }
            return draws;
        };
        const auto recs = flatShadowReference(world(true));
        expect(allKind(recs, ShadowKind::MatchedWithDonors) && missingAffine(recs) == kGateResidual,
               "world-like parallax (random depths 1 to 20, motion proportional to 1/w): kGateResidual fails alone");
        double residual = 1e9;
        for (const ShadowRecord& r : recs) residual = std::fmin(residual, r.residual);
        expect(residual > 5.0, "by a wide margin: the residual is over 5 px against the gate's half pixel");
        expect(missingAffine(flatShadowReference(world(false))) == 0, "control: the same vertices all at one depth move together and pass");
        // The depth gate protects a receiver too: a piece at 40 against donors at 1 to 20 would take a motion it does not have.
        auto scene = world(true);
        ShadowDrawRef r = emptyDraw(false);
        r.hull.push_back({-0.1f, -0.1f, 40.0f}); r.hull.push_back({0.1f, 0.1f, 40.0f});
        scene.push_back(r);
        const ShadowRecord distant = flatShadowReference(scene).back();
        expect((static_cast<unsigned>(distant.gates) & kGateDepth) == 0 && (static_cast<unsigned>(distant.gates) & kGateHull) != 0,
               "a receiver at w = 40 against donors at 1 to 20 fails kGateDepth (and is inside their hull)");
    }
    {
        // (f) all donors lost, and who counts as a donor.
        const Rigid r = gripRotation(cam, kWeapon, roll, 1.0);
        const auto piece = [&](uint32_t x, uint32_t y, bool matched, int n) {
            ShadowDrawRef d = emptyDraw(matched, x, y);
            for (int i = 0; i < n; ++i) d.vertices.push_back(moved(cam, -0.1 + 0.2 * (i % 7) / 6.0, 0.6 + 0.2 * (i / 7 % 7) / 6.0, 0.5, r));
            return d;
        };
        const auto lone = flatShadowReference({piece(kIdentityX, kIdentityY, true, 60)});
        expect(lone.size() == 1 && lone[0].kind == static_cast<float>(ShadowKind::MatchedAlone) && lone[0].donorDraws == 0 && lone[0].gates == 0 &&
                   lone[0].rmsMean == 0 && lone[0].rmsAffine == 0 && lone[0].ownMean[0] != 0.0f,
               "a lone matched piece is MatchedAlone: no donors, no gates, no error, but its own mean motion is recorded");
        const auto receiverAlone = flatShadowReference({emptyDraw(false)});
        expect(receiverAlone[0].kind == static_cast<float>(ShadowKind::ReceiverAlone) && receiverAlone[0].gates == 0, "a receiver with no donor at all is ReceiverAlone (the production's view-attached case)");
        // The first piece, a piece of identity (x, y), and a receiver of the first identity: are both pieces alone, and how many donors has the receiver?
        const auto other = [&](uint32_t x, uint32_t y) {
            const auto recs = flatShadowReference({piece(kIdentityX, kIdentityY, true, 60), piece(x, y, true, 60), emptyDraw(false)});
            struct Result { bool bothAlone; float receiverDonors; };
            return Result{recs[0].kind == static_cast<float>(ShadowKind::MatchedAlone) && recs[1].kind == static_cast<float>(ShadowKind::MatchedAlone), recs[2].donorDraws};
        };
        const auto sameBoth = other(kIdentityX, kIdentityY);
        expect(!sameBoth.bothAlone && sameBoth.receiverDonors == 2.0f, "control: a second piece of the same identity is a donor to the first, and the receiver has both");
        const auto differX = other(kIdentityX ^ 1u, kIdentityY);
        expect(differX.bothAlone && differX.receiverDonors == 1.0f, "a piece whose identity.x differs is not pooled: both matched pieces are alone, the receiver has one donor");
        const auto differY = other(kIdentityX, kIdentityY ^ 0x4u);
        expect(differY.bothAlone && differY.receiverDonors == 1.0f, "nor one whose identity.y (byte 30 left out by the caller) differs");
        // The unmatched draw takes the identity it was given: a receiver of the second identity has only the second as donor.
        {
            const auto recs = flatShadowReference({piece(kIdentityX, kIdentityY, true, 60), piece(kIdentityX ^ 1u, kIdentityY, true, 60), emptyDraw(false, kIdentityX ^ 1u, kIdentityY)});
            expect(recs[2].kind == static_cast<float>(ShadowKind::ReceiverWithDonors) && recs[2].donorDraws == 1.0f && recs[2].pooled == 60.0f,
                   "a receiver pools only the donors of its own identity: one donor of 60 vertices out of the two matched pieces");
        }
        // Leave-one-out: a piece is not its own donor, and three pieces make two donors for each.
        {
            const auto recs = flatShadowReference({piece(kIdentityX, kIdentityY, true, 60), piece(kIdentityX, kIdentityY, true, 80), piece(kIdentityX, kIdentityY, true, 100)});
            expect(recs[0].donorDraws == 2.0f && recs[0].pooled == 180.0f && recs[1].donorDraws == 2.0f && recs[1].pooled == 160.0f && recs[2].pooled == 140.0f,
                   "leave-one-out: each of three pieces has the other two as donors and its own vertices are not in its fit (180, 160, 140 vertices)");
            expect(recs[0].evaluated == 60.0f && recs[1].evaluated == 80.0f && recs[2].evaluated == 100.0f, "and each is compared on its own vertices");
        }
        // A piece whose motion differs from the others': its error is against theirs, not its own.
        {
            auto wild = piece(kIdentityX, kIdentityY, true, 60);
            for (ShadowVertex& v : wild.vertices) { v.mx += 5.f; }
            const auto recs = flatShadowReference({wild, piece(kIdentityX, kIdentityY, true, 80), piece(kIdentityX, kIdentityY, true, 100)});
            expectIn(recs[0].rmsAffine, 4.9, 5.1, "a piece 5 px off its donors has an affine error of 5 px (its own motion is not in the fit)");
            expectIn(recs[0].rmsMean, 4.9, 7.0, "and a mean error of at least that");
            expect(recs[1].donorDraws == 2.0f && recs[1].residual > 0.5f, "while the donors that include it see it as a residual (over half a pixel)");
        }
        // An unreadable identity.
        {
            ShadowDrawRef dead = piece(kIdentityX, kIdentityY, true, 60);
            dead.identityReadable = false;
            const auto recs = flatShadowReference({dead, piece(kIdentityX, kIdentityY, true, 60), emptyDraw(false)});
            expect(recs[0].kind == static_cast<float>(ShadowKind::IdentityUnreadable) && recs[0].gates == 0 && recs[0].donorDraws == 0,
                   "a draw whose pool identity cannot be read is IdentityUnreadable, with nothing else said, whatever its vertices");
            ShadowDrawRef unmatched = emptyDraw(false);
            unmatched.identityReadable = false;
            expect(flatShadowReference({unmatched})[0].kind == static_cast<float>(ShadowKind::IdentityUnreadable), "so is an unmatched one");
        }
        // A matched draw with no vertices.
        {
            const auto recs = flatShadowReference({piece(kIdentityX, kIdentityY, true, 0), piece(kIdentityX, kIdentityY, true, 60)});
            expect(recs[0].kind == 0.0f && recs[0].gates == 0 && recs[0].donorDraws == 0, "a matched draw with no vertex is kind None: nothing to compare");
            expect(recs[1].kind == static_cast<float>(ShadowKind::MatchedAlone), "and it is no donor: the other is alone");
        }
        // An unmatched draw's vertices are ignored: only its hull, or its positions when no hull is given, count.
        {
            ShadowDrawRef unmatched = piece(kIdentityX, kIdentityY, false, 60);
            for (ShadowVertex& v : unmatched.vertices) { v.mx = 99.f; v.my = -99.f; }
            const auto recs = flatShadowReference({piece(kIdentityX, kIdentityY, true, 60), piece(kIdentityX, kIdentityY, true, 60), unmatched});
            expect(recs[2].kind == static_cast<float>(ShadowKind::ReceiverWithDonors) && recs[2].donorDraws == 2.0f && recs[2].rmsMean == 0 &&
                       recs[2].ownMean[0] == 0 && recs[2].ownMean[1] == 0,
                   "an unmatched draw's own motion is never read: no error, no own mean, however its vertices move");
        }
    }


    // -- 3. exact numbers, on scenes whose answer is known without the model --
    {
        // A 6 x 6 grid of vertices, drawn by `pieces` draws at the same positions, each moving by its own constant.
        const auto grid = [&](std::initializer_list<std::pair<float, float>> motions, unsigned perPiece = 36) {
            std::vector<ShadowDrawRef> draws;
            for (const auto& m : motions) {
                ShadowDrawRef d = emptyDraw();
                for (unsigned i = 0; i < perPiece; ++i) {
                    ShadowVertex v;
                    v.u = -0.2f + 0.07f * static_cast<float>(i % 6); v.v = 0.5f + 0.06f * static_cast<float>((i / 6) % 6); v.w = 0.5f;
                    v.mx = m.first; v.my = m.second;
                    d.vertices.push_back(v);
                }
                draws.push_back(d);
            }
            return draws;
        };
        // Donors that move one way and the other at the same places: the best affine field is the mean (zero), and what it leaves is the whole of the
        // motion: the residual is |(0.3, 1.5)|, the spread the larger axis's full range, 3 px in y.
        {
            auto scene = grid({{0.3f, 1.5f}, {-0.3f, -1.5f}});
            ShadowDrawRef receiver = emptyDraw(false);
            for (const ShadowVertex& v : scene[0].vertices) receiver.hull.push_back({v.u, v.v, v.w});
            scene.push_back(receiver);
            const ShadowRecord r = flatShadowReference(scene)[2];
            expectIn(r.residual, 1.52970, 1.52971, "donors moving oppositely at the same places leave a residual of exactly |(0.3, 1.5)| = 1.529706");
            expectIn(r.spread, 2.9999, 3.0001, "and a spread of 3.0: the larger axis's range, not x's 0.6");
            expect(r.pooled == 72.0f && r.donorDraws == 2.0f, "over 72 donor vertices of two draws");
            expect((kGateAffine & ~static_cast<unsigned>(r.gates)) == kGateResidual && !r.currentAccepts(), "kGateResidual fails alone; the production refuses");
        }
        // A piece off its donors by (3, 4) is 5 px away from the mean model and from the affine model alike (the donors are uniform).
        {
            const auto recs = flatShadowReference(grid({{1.f, 2.f}, {1.f, 2.f}, {4.f, 6.f}}));
            expectIn(recs[2].rmsMean, 4.9999, 5.0001, "a piece (3, 4) px off uniform donors has a mean-model error of exactly 5 px");
            expectIn(recs[2].rmsAffine, 4.9999, 5.0001, "and an affine-model error of exactly 5 px");
            expect(recs[2].evaluated == 36.0f && recs[2].ownMean[0] == 4.0f && recs[2].ownMean[1] == 6.0f, "over its 36 vertices, with its own mean motion (4, 6)");
            expectIn(recs[0].rmsMean, 2.4999, 2.5001, "while a piece at (1, 2) among them is off the mean of the other two, (2.5, 4), by exactly |(1.5, 2)| = 2.5");
        }
        // The production's gates are its constants, inclusive: a spread of exactly one pixel is taken, a thousandth over is not.
        {
            const auto spreadOf = [&](float second) {
                auto scene = grid({{0.f, 0.f}, {second, 0.f}});
                ShadowDrawRef receiver = emptyDraw(false);
                for (const ShadowVertex& v : scene[0].vertices) receiver.hull.push_back({v.u, v.v, v.w});
                scene.push_back(receiver);
                return flatShadowReference(scene)[2];
            };
            const ShadowRecord at = spreadOf(1.0f), over = spreadOf(1.001f);
            expect(at.spread == 1.0f && at.currentAccepts() && (static_cast<unsigned>(at.gates) & kGateSpread) != 0, "donors spread by exactly one pixel pass kGateSpread (the production's limit is inclusive)");
            expect(over.spread > 1.0f && !over.currentAccepts() && (static_cast<unsigned>(over.gates) & kGateSpread) == 0, "a thousandth over fails it (control)");
        }
    }

    // -- 4. the reference against the model computed the long way, on 400 random scenes --
    {
        unsigned scenes = 0, records = 0, compared = 0, skipped = 0, bad = 0;
        unsigned kindsSeen[kShadowKinds] = {};
        unsigned bitSet[7] = {}, bitClear[7] = {};
        std::string first;
        for (uint32_t seed = 1; seed <= 400; ++seed) {
            const auto draws = randomScene(seed);
            const auto recs = flatShadowReference(draws);
            ++scenes;
            for (size_t i = 0; i < draws.size(); ++i) {
                ++records;
                const OracleRecord o = oracle(draws, i);
                const ShadowRecord& r = recs[i];
                const unsigned kind = static_cast<unsigned>(r.kind + 0.5f);
                ++kindsSeen[kind < kShadowKinds ? kind : 0];
                bool ok = kind == o.kind && static_cast<unsigned>(r.donorDraws + 0.5f) == o.donorDraws;
                const auto closeTo = [](double a, double b) { return std::fabs(a - b) <= 2e-4 * std::fmax(1.0, std::fabs(b)); };
                ok = ok && closeTo(r.pooled, o.pooled) && closeTo(r.spread, o.spread) && closeTo(r.residual, o.residual) && closeTo(r.rmsMean, o.rmsMean) &&
                     closeTo(r.rmsAffine, o.rmsAffine) && closeTo(r.ownMean[0], o.ownMean[0]) && closeTo(r.ownMean[1], o.ownMean[1]);
                if (o.close < 1e-5) ++skipped;
                else {
                    ok = ok && static_cast<unsigned>(r.gates) == o.gates;
                    ++compared;
                    if (o.donorDraws) for (unsigned b = 0; b < 7; ++b) ++(((o.gates >> b) & 1u) ? bitSet : bitClear)[b];
                }
                if (!ok) {
                    ++bad;
                    if (first.empty()) {
                        char text[400];
                        std::snprintf(text, sizeof(text), "scene %u draw %zu: kind %u/%u donors %u/%u pooled %.1f/%.1f spread %.5f/%.5f residual %.5f/%.5f rms %.5f/%.5f %.5f/%.5f gates %u/%u",
                                      seed, i, kind, o.kind, static_cast<unsigned>(r.donorDraws), o.donorDraws, r.pooled, o.pooled, r.spread, o.spread, r.residual, o.residual,
                                      r.rmsMean, o.rmsMean, r.rmsAffine, o.rmsAffine, static_cast<unsigned>(r.gates), o.gates);
                        first = text;
                    }
                }
            }
        }
        if (bad) std::printf("  first mismatch: %s\n", first.c_str());
        expect(bad == 0, "on 400 random scenes the reference (per-draw moments merged by Chan's formula) equals the long-hand model (all donor vertices, two passes): kinds, donors, fit, error and gate bits");
        bool coverage = scenes == 400 && records > 1000 && skipped * 20 < records;
        for (unsigned k = 1; k < kShadowKinds; ++k) coverage = coverage && kindsSeen[k] >= 30;
        for (unsigned b = 0; b < 7; ++b) coverage = coverage && bitSet[b] >= 25 && bitClear[b] >= 25;
        expect(coverage, "and those scenes reach every kind and set and clear every one of the seven gate bits, for a draw with donors, 25 times or more");
    }

    return failures;
}

// ---- the counters, the window, the line ----
inline int flatShadowAccumulateTests() {
    using namespace edvr;
    int failures = 0;
    const auto expect = [&](bool ok, const char* name) {
        if (!ok) { std::printf("FAIL: shadow counters %s\n", name); ++failures; }
    };
    const auto same = [](const uint64_t* a, std::initializer_list<uint64_t> b) {
        unsigned i = 0;
        for (uint64_t v : b) if (a[i++] != v) return false;
        return true;
    };

    // -- the model's numbers are the ones this arc was designed with --
    expect(kFlatShadowMinFitVertices == 24 && kFlatShadowResidualPixels == 0.5f && kFlatShadowConditioning == 0.005f && kFlatShadowHullMargin == 0.05f &&
               kFlatShadowHullFloor == 0.01f && kFlatShadowDepthRatio == 1.25f && kFlatShadowMovingPixels == 1.0f && kFlatShadowEveryFrames == 8,
           "the shadow's constants: 24 fit vertices, 0.5 px residual, 0.005 conditioning, hull margin 0.05 and floor 0.01, depth ratio 1.25, moving 1 px, one frame in 8");
    expect(kFlatShadowBins == 6 && kFlatShadowEdges[0] == 0.25f && kFlatShadowEdges[1] == 0.5f && kFlatShadowEdges[2] == 1.0f && kFlatShadowEdges[3] == 2.0f &&
               kFlatShadowEdges[4] == 4.0f,
           "the bins are <=0.25, <=0.5, <=1, <=2, <=4 and more");
    expect(kGateMinVertices == 1 && kGateSpread == 2 && kGateFitVertices == 4 && kGateConditioning == 8 && kGateResidual == 16 && kGateHull == 32 &&
               kGateDepth == 64 && kGateCurrent == 3 && kGateAffine == 124,
           "the gate bits are 1, 2 (the production's), 4, 8, 16, 32, 64 (the affine model's)");
    expect(static_cast<unsigned>(ShadowKind::None) == 0 && static_cast<unsigned>(ShadowKind::MatchedWithDonors) == 1 &&
               static_cast<unsigned>(ShadowKind::MatchedAlone) == 2 && static_cast<unsigned>(ShadowKind::ReceiverWithDonors) == 3 &&
               static_cast<unsigned>(ShadowKind::ReceiverAlone) == 4 && static_cast<unsigned>(ShadowKind::IdentityUnreadable) == 5 && kShadowKinds == 6,
           "the kinds are numbered as the shader and the line count them");

    // -- the bins: an error falls in the first bin whose edge it does not exceed --
    {
        struct Case { float error; unsigned bin; };
        const Case cases[] = {{0.f, 0}, {0.25f, 0}, {0.2501f, 1}, {0.5f, 1}, {0.5001f, 2}, {1.f, 2}, {1.0001f, 3}, {2.f, 3}, {2.0001f, 4}, {4.f, 4}, {4.0001f, 5},
                              {1000.f, 5}, {INFINITY, 5}, {NAN, 5}, {-1.f, 0}};
        bool ok = true;
        for (const Case& c : cases) ok = ok && flatShadowBin(c.error) == c.bin;
        expect(ok, "bin edges are inclusive: 0.25 is bin 0, 0.2501 bin 1, ... 4 is bin 4, over 4 bin 5, and a number that is not a number is filed as the worst");
        // The mutation control: a binning with strict edges files every edge value one bin up, and fails the case above.
        const auto strict = [](float error) { unsigned b = 0; while (b < kFlatShadowBins - 1 && !(error < kFlatShadowEdges[b])) ++b; return b; };
        bool strictOk = true;
        for (const Case& c : cases) strictOk = strictOk && strict(c.error) == c.bin;
        expect(!strictOk && strict(0.25f) == 1, "mutation control: strict edges (<) would file 0.25 in bin 1 and fail the cases");
    }

    // -- one sampled frame, every kind, every counter --
    const unsigned allG = kGateCurrent | kGateAffine;
    const auto rec = [](ShadowKind kind, unsigned gates, float rmsMean = 0, float rmsAffine = 0, float residual = 0, float donors = 0, float pooled = 0,
                        float mx = 0, float my = 0) {
        ShadowRecord r;
        r.kind = static_cast<float>(kind); r.gates = static_cast<float>(gates);
        r.rmsMean = rmsMean; r.rmsAffine = rmsAffine; r.residual = residual; r.donorDraws = donors; r.pooled = pooled; r.ownMean[0] = mx; r.ownMean[1] = my;
        return r;
    };
    const auto frame = [&] {
        std::vector<ShadowRecord> f;
        f.push_back(rec(ShadowKind::MatchedWithDonors, allG, 0.3f, 0.1f, 0.2f, 3, 100));                                                        // both accept
        f.push_back(rec(ShadowKind::MatchedWithDonors, kGateAffine | kGateMinVertices, 3.f, 0.2f, 0.3f, 2, 200));                                 // the affine alone
        f.push_back(rec(ShadowKind::MatchedWithDonors, kGateCurrent, 5.f, 5.f, 7.f, 4, 10));                                                     // the production alone, no fit
        f.push_back(rec(ShadowKind::MatchedWithDonors, kGateFitVertices | kGateConditioning | kGateHull | kGateDepth, 0.9f, 0.6f, 0.8f, 5, 300)); // residual fails
        f.push_back(rec(ShadowKind::MatchedWithDonors, kGateFitVertices | kGateHull | kGateDepth, 0.1f, 1.5f, 9.f, 1, 400));                      // conditioning fails
        f.push_back(rec(ShadowKind::MatchedAlone, 0x7F, 9.f, 9.f, 9.f, 9, 9, 2.0f, 0.f));                                                        // moving; the gates are not read
        f.push_back(rec(ShadowKind::ReceiverWithDonors, allG, 0, 0, 0.1f, 2, 50));
        f.push_back(rec(ShadowKind::ReceiverWithDonors, kGateCurrent, 0, 0, 0, 3, 60));
        f.push_back(rec(ShadowKind::ReceiverWithDonors, kGateAffine | kGateMinVertices, 0, 0, 0.6f, 1, 70));
        f.push_back(rec(ShadowKind::ReceiverWithDonors, 0, 0, 0, 0, 2, 5));
        f.push_back(rec(ShadowKind::ReceiverAlone, 0));
        f.push_back(rec(ShadowKind::ReceiverAlone, 0));
        f.push_back(rec(ShadowKind::IdentityUnreadable, 0));
        f.push_back(rec(ShadowKind::None, allG, 9.f, 9.f, 9.f, 9, 9, 9.f, 9.f));                                                                  // skipped
        ShadowRecord beyond = rec(ShadowKind::None, allG, 9.f, 9.f, 9.f, 9, 9, 9.f, 9.f);
        beyond.kind = 6.f;                                                                                                                         // out of range: skipped
        f.push_back(beyond);
        return f;
    };
    {
        ShadowStats s;
        const auto records = frame();
        flatShadowAccumulate(s, records.data(), static_cast<unsigned>(records.size()));
        expect(s.sampledFrames == 1 && s.drawsRead == 13, "one frame sampled; 13 draws read (kind 0 and a kind past the last are skipped)");
        expect(same(s.byKind, {0, 5, 1, 4, 2, 1}), "by kind: five matched with donors, one alone, four receivers with donors, two alone, one unreadable");
        expect(same(s.meanAll, {1, 1, 1, 0, 1, 1}) && same(s.affineAll, {2, 0, 1, 1, 0, 1}),
               "leave-one-out draws are binned by each model's error: mean {0.1,0.3,0.9,3,5}, affine {0.1,0.2,0.6,1.5,5}");
        expect(same(s.meanAccepted, {0, 1, 0, 0, 0, 1}) && same(s.affineAccepted, {2, 0, 0, 0, 0, 0}),
               "and again where the model's own gate accepts: the production's two draws, the affine's two");
        expect(s.currentAccepts == 2 && s.affineAccepts == 2 && s.bothAccept == 1 && s.neitherAccept == 2, "accepts: current 2, affine 2, both 1, neither 2");
        expect(same(s.gateFailed, {3, 1, 2, 3, 3}),
               "gate failures over the draws with donors: a draw that fails the fit-vertices gate is not also charged conditioning and residual (they mean nothing "
               "without a fit), but is charged hull and depth; one that fails conditioning is charged residual too when its bit is absent");
        expect(same(s.residualBins, {2, 1, 2, 0, 0, 0}), "the donors' residual is binned only where the fit was solved (fit-vertices and conditioning set), receivers included");
        expect(s.donorDrawsSum == 23 && s.donorVerticesSum == 1195, "donor sums are over the draws with donors, matched and receivers (the alone draw's 9 are not added)");
        expect(s.receiverCurrentAccepts == 2 && s.receiverAcceptedAffineAccepts == 1 && s.receiverCurrentRefuses == 2 && s.receiverRefusedAffineAccepts == 1,
               "receivers: two the production accepts (one the affine accepts too), two it refuses (one the affine would accept)");
        expect(s.modeTwoFires == 2 && s.modeTwoWhileMoving == 2 && s.movingFrames == 1,
               "two view-attached receivers, both in a frame where a matched draw moved 2 px; one moving frame");
    }
    {
        // Mutation control: the same frame with the matched-alone draw's motion removed has no moving frame, and the check above would fail.
        auto records = frame();
        records[5].ownMean[0] = 0;
        ShadowStats s;
        flatShadowAccumulate(s, records.data(), static_cast<unsigned>(records.size()));
        expect(s.movingFrames == 0 && s.modeTwoWhileMoving == 0 && s.modeTwoFires == 2, "mutation control: with that draw at rest the frame is not a moving one");
    }

    // -- the view-attached counters are per frame and strict --
    {
        ShadowStats s;
        const auto moved = [&](float mx, float my, ShadowKind kind, bool receiverFirst) {
            std::vector<ShadowRecord> f;
            if (receiverFirst) f.push_back(rec(ShadowKind::ReceiverAlone, 0));
            f.push_back(rec(kind, allG, 0, 0, 0, 1, 10, mx, my));
            if (!receiverFirst) f.push_back(rec(ShadowKind::ReceiverAlone, 0));
            flatShadowAccumulate(s, f.data(), static_cast<unsigned>(f.size()));
        };
        moved(1.0f, 0.f, ShadowKind::MatchedWithDonors, true);
        expect(s.modeTwoFires == 1 && s.modeTwoWhileMoving == 0 && s.movingFrames == 0, "a mean motion of exactly one pixel is not moving (strictly over)");
        moved(1.001f, 0.f, ShadowKind::MatchedWithDonors, true);
        expect(s.modeTwoFires == 2 && s.modeTwoWhileMoving == 1 && s.movingFrames == 1, "just over is, and a receiver listed before the moving draw counts (two passes)");
        moved(0.f, -1.5f, ShadowKind::MatchedAlone, false);
        expect(s.modeTwoFires == 3 && s.modeTwoWhileMoving == 2 && s.movingFrames == 2, "motion in y, negative, of a matched draw that is alone, counts too");
        {
            std::vector<ShadowRecord> f = {rec(ShadowKind::ReceiverAlone, 0), rec(ShadowKind::ReceiverWithDonors, allG, 0, 0, 0, 1, 10, 9.f, 9.f)};
            flatShadowAccumulate(s, f.data(), static_cast<unsigned>(f.size()));
            expect(s.modeTwoFires == 4 && s.modeTwoWhileMoving == 2 && s.movingFrames == 2, "a receiver's own motion field is never read: it does not make a frame moving");
        }
        {
            std::vector<ShadowRecord> f = {rec(ShadowKind::MatchedAlone, 0, 0, 0, 0, 0, 0, 5.f, 5.f)};
            flatShadowAccumulate(s, f.data(), 1);
            expect(s.movingFrames == 3 && s.modeTwoFires == 4, "a moving frame without a receiver alone still counts as moving");
        }
        flatShadowAccumulate(s, nullptr, 0);
        expect(s.sampledFrames == 6 && s.drawsRead == 9, "an empty frame is a sampled frame and reads nothing");
    }

    // -- add, and the window --
    {
        static constexpr size_t words = sizeof(edvr::ShadowStats) / sizeof(uint64_t);
        expect(sizeof(ShadowStats) == 58 * sizeof(uint64_t) && words == 58,
               "ShadowStats is 58 counters and nothing else: a counter added to it must be added to add(), to the window and to this test");
        const auto fill = [&](uint64_t base, uint64_t step) {
            std::array<uint64_t, words> w{};
            for (size_t i = 0; i < words; ++i) w[i] = base + step * i;
            ShadowStats s;
            std::memcpy(&s, w.data(), sizeof(s));
            return s;
        };
        const auto read = [&](const ShadowStats& s) {
            std::array<uint64_t, words> w{};
            std::memcpy(w.data(), &s, sizeof(s));
            return w;
        };
        ShadowStats a = fill(1000, 7);
        const ShadowStats b = fill(3, 13);
        a.add(b);
        const auto sum = read(a);
        bool every = true;
        for (size_t i = 0; i < words; ++i) every = every && sum[i] == 1000 + 7 * i + 3 + 13 * i;
        expect(every, "add() sums every one of the 58 counters");
        auto missed = sum;
        missed[31] -= 3 + 13 * 31;
        bool sensitive = false;
        for (size_t i = 0; i < words; ++i) sensitive = sensitive || missed[i] != 1000 + 7 * i + 3 + 13 * i;
        expect(sensitive, "mutation control: an add() that forgot one counter would leave it short, and the check above sees it");

        const ShadowStats now = fill(500, 3);
        std::array<uint64_t, words> was{};
        for (size_t i = 0; i < words; ++i) was[i] = (i % 3 == 0) ? 900 : 100 + i;
        ShadowStats wasStats;
        std::memcpy(&wasStats, was.data(), sizeof(wasStats));
        const auto window = read(flatShadowDelta(now, wasStats));
        bool delta = true;
        for (size_t i = 0; i < words; ++i) {
            const uint64_t n = 500 + 3 * i;
            delta = delta && window[i] == (n > was[i] ? n - was[i] : 0);
        }
        expect(delta, "the window is now less before, counter by counter, and a counter that went down is zero, not a wrapped number");
        bool restarted = true;
        for (uint64_t value : read(flatShadowDelta(fill(1, 1), fill(1000, 1)))) restarted = restarted && value == 0;
        expect(restarted, "a restarted history (every counter below the last line's) is a window of zero");
        const auto equal = read(flatShadowDelta(now, now));
        bool zero = true;
        for (uint64_t value : equal) zero = zero && value == 0;
        expect(zero, "and an unchanged one is zero");
        // The window of a real accumulation: the second frame alone.
        ShadowStats cumulative;
        const auto records = frame();
        flatShadowAccumulate(cumulative, records.data(), static_cast<unsigned>(records.size()));
        const ShadowStats first = cumulative;
        flatShadowAccumulate(cumulative, records.data(), static_cast<unsigned>(records.size()));
        const ShadowStats second = flatShadowDelta(cumulative, first);
        ShadowStats once;
        flatShadowAccumulate(once, records.data(), static_cast<unsigned>(records.size()));
        expect(read(second) == read(once), "the window of a second identical frame is that frame's counters");
    }

    // -- the line --
    {
        ShadowStats w;
        w.sampledFrames = 101; w.drawsRead = 102; w.notReady = 103; w.failed = 104;
        const uint64_t kinds[kShadowKinds] = {0, 100, 111, 100, 113, 115};
        for (unsigned i = 0; i < kShadowKinds; ++i) w.byKind[i] = kinds[i];
        for (unsigned i = 0; i < kFlatShadowBins; ++i) {
            w.meanAll[i] = 200 + i; w.meanAccepted[i] = 210 + i; w.affineAll[i] = 220 + i; w.affineAccepted[i] = 230 + i; w.residualBins[i] = 240 + i;
        }
        w.currentAccepts = 301; w.affineAccepts = 302; w.bothAccept = 303; w.neitherAccept = 304;
        for (unsigned i = 0; i < 5; ++i) w.gateFailed[i] = 311 + i;
        w.donorDrawsSum = 500; w.donorVerticesSum = 60000;
        w.receiverCurrentAccepts = 401; w.receiverCurrentRefuses = 402; w.receiverRefusedAffineAccepts = 403; w.receiverAcceptedAffineAccepts = 404;
        w.modeTwoFires = 501; w.modeTwoWhileMoving = 502; w.movingFrames = 503;
        char line[4096];
        const int n = flatShadowLine(line, sizeof(line), w);
        expect(n > 0 && n < int(sizeof(line)), "the line fits its buffer");
        const std::string text(line);
        const auto has = [&](const char* needle) { return text.find(needle) != std::string::npos; };
        const auto bins = [](const char* label, unsigned base) {
            char out[200];
            std::snprintf(out, sizeof(out), "%s {<=0.25=%u <=0.5=%u <=1=%u <=2=%u <=4=%u more=%u}", label, base, base + 1, base + 2, base + 3, base + 4, base + 5);
            return std::string(out);
        };
        expect(text.rfind("flat foreground shadow 5s: sampled-frames=101 draws-read=102 (frames unread=103 failed=104) by kind: matched-with-donors=100 "
                          "matched-alone=111 receiver-with-donors=100 receiver-alone=113 identity-unreadable=115;", 0) == 0,
               "the line opens with the window's frames, draws and the five kinds, each with its own count");
        expect(has(bins("mean-model all", 200).c_str()) && has(bins("mean-model where the current policy accepts", 210).c_str()) &&
                   has(bins("affine-model all", 220).c_str()) && has(bins("affine-model where its gate accepts", 230).c_str()) &&
                   has(bins("donors' own fit residual", 240).c_str()),
               "then the five bin sets in their order: each of the six bins with its own count");
        expect(has("accepts over leave-one-out draws: current=301 affine=302 both=303 neither=304;"), "the accept counts");
        expect(has("affine gate failures (a draw can fail several): fit-vertices=311 conditioning=312 residual=313 hull=314 depth=315;"), "the five gate failures in their order");
        expect(has("donors per draw with donors: draws=2.5 vertices=300;"), "the donors per draw are the sums over the (matched + receiver) draws with donors: 500 / 200 and 60000 / 200");
        expect(has("receivers with donors: current-accepts=401 current-refuses(disagree)=402 affine-would-accept-of-refused=403 affine-would-accept-of-accepted=404;"),
               "the receivers' four counts, the disagreement named");
        expect(has("view-attached (receiver with no donor of its identity) fires=501 while-matched-draws-moved-over-1px=502 (frames with such motion=503);"),
               "and the view-attached counts");
        expect(has("computed on one frame in 8, applied to nothing"), "and the line says it changes nothing and how often it samples");
        // The mutation control for the mapping: swapping two counters changes the line.
        ShadowStats swapped = w;
        std::swap(swapped.currentAccepts, swapped.affineAccepts);
        char other[4096];
        flatShadowLine(other, sizeof(other), swapped);
        expect(std::string(other) != text, "mutation control: a line whose counters are swapped differs from this one");
    }
    {
        // An empty window prints every counter at zero, without a division by zero.
        char line[4096];
        const int n = flatShadowLine(line, sizeof(line), ShadowStats{});
        const std::string text(line);
        expect(n > 0 && text.find("nan") == std::string::npos && text.find("inf") == std::string::npos && text.find("donors per draw with donors: draws=0.0 vertices=0;") != std::string::npos,
               "an empty window prints donors per draw as zero, not nan");
        unsigned values = 0;
        bool zero = true;
        for (size_t i = 0; i < text.size(); ++i) {
            if (text[i] != '=' || (i && text[i - 1] == '<')) continue;
            size_t j = i + 1;
            bool digits = false, nonzero = false;
            while (j < text.size() && (std::isdigit(static_cast<unsigned char>(text[j])) || text[j] == '.')) {
                digits = true;
                if (text[j] != '0' && text[j] != '.') nonzero = true;
                ++j;
            }
            if (!digits) continue;
            ++values;
            zero = zero && !nonzero;
        }
        expect(zero && values == 57, "every number in the line is zero, and there are 57 of them: the 58 counters less byKind[0] (kind none is never printed), the donor sums as two averages");
        // At the widest.
        ShadowStats widest;
        const uint64_t big = ~0ull;
        widest.sampledFrames = widest.drawsRead = widest.notReady = widest.failed = big;
        for (auto& k : widest.byKind) k = big;
        for (unsigned i = 0; i < kFlatShadowBins; ++i) widest.meanAll[i] = widest.meanAccepted[i] = widest.affineAll[i] = widest.affineAccepted[i] = widest.residualBins[i] = big;
        widest.currentAccepts = widest.affineAccepts = widest.bothAccept = widest.neitherAccept = big;
        for (auto& g : widest.gateFailed) g = big;
        widest.donorDrawsSum = widest.donorVerticesSum = big;
        widest.receiverCurrentAccepts = widest.receiverCurrentRefuses = widest.receiverRefusedAffineAccepts = widest.receiverAcceptedAffineAccepts = big;
        widest.modeTwoFires = widest.modeTwoWhileMoving = widest.movingFrames = big;
        char wide[4096];
        const int wideN = flatShadowLine(wide, sizeof(wide), widest);
        expect(wideN > 0 && wideN < int(sizeof(wide)) && std::string(wide).find("applied to nothing") != std::string::npos,
               "at its widest (every counter ~0) the line fits the runtime's 4096 bytes, untruncated");
        char tight[64];
        expect(flatShadowLine(tight, sizeof(tight), widest) > 0 && std::strlen(tight) == sizeof(tight) - 1, "a buffer too small for the line is truncated, not overrun");
    }
    return failures;
}

// ---- the wiring: the shaders, the capture class and the runtime are sources the model cannot drive end to end, so each decision is held to its
// text here, with a mutation control that fails it (tools\weapon_motion_test runs the shaders on a device against the model above) ----
inline int flatShadowWiringTests() {
    using namespace edvr;
    int failures = 0;
    const auto expect = [&](bool ok, const char* name) {
        if (!ok) { std::printf("FAIL: shadow wiring %s\n", name); ++failures; }
    };
    const auto slurp = [](const char* path) {
        std::ifstream in(path, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    };
    const auto compact = [](const std::string& in) {
        std::string out; out.reserve(in.size());
        for (char c : in) if (c != ' ' && c != '\r' && c != '\n' && c != '\t') out += c;
        return out;
    };
    const auto ordered = [&](const std::string& compacted, std::initializer_list<std::string> needles) {
        size_t pos = 0;
        bool ok = !compacted.empty();
        for (const std::string& needle : needles) {
            const size_t at = compacted.find(compact(needle), pos);
            if (at == std::string::npos) { ok = false; break; }
            pos = at + compact(needle).size();
        }
        return ok;
    };
    // A mutation must change the text, or its control proves nothing: a needle that is not found is a failure of the test itself.
    const auto without = [&](std::string text, const char* needle) {
        const std::string n = compact(needle);
        const size_t at = text.find(n);
        if (at == std::string::npos) { std::printf("FAIL: shadow wiring mutation needle not found: %s\n", needle); ++failures; return text; }
        text.erase(at, n.size());
        return text;
    };
    const auto replaced = [&](std::string text, const char* from, const char* to) {
        const std::string f = compact(from);
        const size_t at = text.find(f);
        if (at == std::string::npos) { std::printf("FAIL: shadow wiring mutation needle not found: %s\n", from); ++failures; return text; }
        text.replace(at, f.size(), compact(to));
        return text;
    };
    const auto count = [](const std::string& text, const std::string& needle) {
        size_t n = 0, at = 0;
        while ((at = text.find(needle, at)) != std::string::npos) { ++n; at += needle.size(); }
        return n;
    };
    const auto bodyOf = [](const std::string& text, const char* from, const char* to) {
        const size_t a = text.find(from);
        if (a == std::string::npos) return std::string();
        const size_t b = text.find(to, a);
        return text.substr(a, b == std::string::npos ? std::string::npos : b - a);
    };
    // A member function of a class whose members are indented four spaces ends at the first line that is four spaces and a brace.
    const auto member = [](const std::string& text, const char* signature) {
        const size_t at = text.find(signature);
        if (at == std::string::npos) return std::string();
        const size_t end = text.find("\n    }\n", at);
        return text.substr(at, end == std::string::npos ? std::string::npos : end + 7 - at);
    };

    const std::string shaderSource = slurp("src/d3d11/flat_foreground_motion_shader.h");
    const std::string motionSource = slurp("src/d3d11/flat_foreground_motion.h");
    const std::string gpuSource = slurp("src/d3d11/flat_foreground_shadow_gpu.h");
    const std::string runtimeSource = slurp("src/d3d11/flat_runtime.cpp");
    expect(!shaderSource.empty() && !motionSource.empty() && !gpuSource.empty() && !runtimeSource.empty(), "the sources are readable from the repo root");

    const std::string common = compact(bodyOf(shaderSource, "inline constexpr char kFlatForegroundShadowCommon", "inline constexpr char kFlatForegroundShadowMomentsBody"));
    const std::string moments = compact(bodyOf(shaderSource, "inline constexpr char kFlatForegroundShadowMomentsBody", "inline constexpr char kFlatForegroundShadowEvalBody"));
    const std::string eval = compact(bodyOf(shaderSource, "inline constexpr char kFlatForegroundShadowEvalBody", "namespace flat_foreground_detail"));
    const std::string donor = compact(bodyOf(shaderSource, "inline constexpr char kFlatForegroundDonorBody", "// The sibling pass, second half"));
    const std::string fit = compact(bodyOf(shaderSource, "inline constexpr char kFlatForegroundFitBody", "// THE SHADOW OF THE SIBLING MODEL"));
    const std::string vs = compact(bodyOf(shaderSource, "inline constexpr char kFlatForegroundMotionVsBody", "inline constexpr char kFlatForegroundMotionPs"));
    const std::string ps = compact(bodyOf(shaderSource, "inline constexpr char kFlatForegroundMotionPs", "// The sibling pass, first half"));
    expect(!common.empty() && !moments.empty() && !eval.empty() && !donor.empty() && !fit.empty() && !vs.empty() && !ps.empty(), "every shader body is found in the source");

    // -- the shaders' constants are the model's --
    const auto number = [](const std::string& text, const char* name) {
        const std::string key = std::string(name) + "=";
        const size_t at = text.find(key);
        return at == std::string::npos ? std::nan("") : std::strtod(text.c_str() + at + key.size(), nullptr);
    };
    const auto constantsMatch = [&](const std::string& text, const std::string& donorText) {
        const auto is = [&](const char* name, double want) { const double v = number(text, name); return std::fabs(v - want) <= 1e-6 * std::fabs(want); };
        return is("kMinVertices", kFlatSiblingMinVertices) && is("kMinFitVertices", kFlatShadowMinFitVertices) && is("kSpreadLimit", kFlatSiblingSpreadPixels) &&
               is("kResidualLimit", kFlatShadowResidualPixels) && is("kConditioning", static_cast<double>(kFlatShadowConditioning)) &&
               is("kHullMargin", static_cast<double>(kFlatShadowHullMargin)) && is("kHullFloor", static_cast<double>(kFlatShadowHullFloor)) &&
               is("kDepthRatio", kFlatShadowDepthRatio) && is("kMotionLimit", 256.0) && number(donorText, "kMotionLimit") == 256.0;
    };
    expect(constantsMatch(common, donor),
           "the shadow shader's constants equal the model's: 3 donor vertices, 24 fit vertices, spread 1, residual 0.5, conditioning 0.005, hull 0.05 + 0.01, depth 1.25, "
           "motion limit 256 (the donor pass's, too)");
    expect(!constantsMatch(replaced(common, "kMinFitVertices=24", "kMinFitVertices=12"), donor), "mutation control: a shader that fits on 12 vertices fails the pin");
    expect(!constantsMatch(replaced(common, "kResidualLimit=0.5", "kResidualLimit=0.6"), donor), "mutation control: a residual limit of 0.6 fails the pin");
    expect(!constantsMatch(replaced(common, "kConditioning=0.005", "kConditioning=0.05"), donor), "mutation control: a conditioning of 0.05 fails the pin");
    expect(!constantsMatch(replaced(common, "kHullMargin=0.05", "kHullMargin=0.5"), donor), "mutation control: a hull margin of 0.5 fails the pin");
    expect(!constantsMatch(replaced(common, "kHullFloor=0.01", "kHullFloor=0.1"), donor), "mutation control: a hull floor of 0.1 fails the pin");
    expect(!constantsMatch(replaced(common, "kDepthRatio=1.25", "kDepthRatio=2.0"), donor), "mutation control: a depth ratio of 2 fails the pin");
    expect(!constantsMatch(replaced(common, "kSpreadLimit=1.0", "kSpreadLimit=2.0"), donor), "mutation control: a spread limit that is not the policy's fails the pin");
    expect(!constantsMatch(replaced(common, "kMinVertices=3", "kMinVertices=1"), donor), "mutation control: a one-vertex floor fails the pin");
    expect(!constantsMatch(replaced(common, "kMotionLimit=256.0", "kMotionLimit=512.0"), donor), "mutation control: a motion limit that is not 256 fails the pin");
    expect(!constantsMatch(common, replaced(donor, "kMotionLimit=256.0", "kMotionLimit=512.0")), "mutation control: the donor pass's limit moved away from the shadow's fails the pin");

    // -- the shadow takes a vertex exactly when the donor pass does: the same test, the same motion --
    const char* const takes[] = {
        "Resolved r=resolveGpuIdentity(id,currentValid);",
        "bool currentValid=validPosition(n0) && validPosition(n1) && validPosition(n2) && asuint(n1.z)==asuint(n0.z) && asuint(n2.z)==asuint(n0.z);",
        "if(r.valid!=1 || !(r.old.w>0) || !(now.w>0) || !all(isfinite(r.old)))",
        "cur=(now.xy/now.w*float2(.5,-.5)+.5)*extentPhase.xy;",
        "prev=(r.old.xy/r.old.w*float2(.5,-.5)+.5)*extentPhase.xy;",
        "m=(prev-r.oldPhase)-(cur-extentPhase.zw);",
        "if(!all(isfinite(m)) || any(abs(m)>kMotionLimit))"};
    const std::string shadowVertex = compact(bodyOf(shaderSource, "bool shadowVertex(", "inline constexpr char kFlatForegroundShadowMomentsBody"));
    const auto bothTake = [&](const std::string& shadowText, const std::string& donorText) {
        for (const char* t : takes) if (shadowText.find(compact(t)) == std::string::npos || donorText.find(compact(t)) == std::string::npos) return false;
        return ordered(shadowText, {"if(r.valid!=1 || !(r.old.w>0) || !(now.w>0) || !all(isfinite(r.old)))return false;",
                                    "if(!all(isfinite(m)) || any(abs(m)>kMotionLimit))return false;", "x=float4(cur/extentPhase.xy*2-1,m);w=now.w;", "return true;"}) &&
               ordered(donorText, {"if(r.valid!=1 || !(r.old.w>0) || !(now.w>0) || !all(isfinite(r.old)))continue;",
                                   "if(!all(isfinite(m)) || any(abs(m)>kMotionLimit))continue;", "sum+=float4(m,1,0);"});
    };
    expect(!shadowVertex.empty() && bothTake(shadowVertex, donor),
           "the shadow's vertex test is the donor pass's, line for line: the map's decision, the valid-and-finite test, the pixel expression for the motion, the 256 px limit");
    expect(!bothTake(replaced(shadowVertex, "r.valid!=1 ||", ""), donor), "mutation control: a shadow that takes vertices the map refused fails the pin");
    expect(!bothTake(replaced(shadowVertex, "!(r.old.w>0) ||", ""), donor), "mutation control: a shadow that takes a vertex with a previous w of zero or less fails the pin");
    expect(!bothTake(replaced(shadowVertex, "any(abs(m)>kMotionLimit)", "any(abs(m)>2*kMotionLimit)"), donor), "mutation control: a shadow with another motion limit fails the pin");
    expect(!bothTake(replaced(shadowVertex, "m=(prev-r.oldPhase)-(cur-extentPhase.zw);", "m=prev-cur;"), donor), "mutation control: a shadow motion that keeps the jitter step fails the pin");
    expect(!bothTake(shadowVertex, replaced(donor, "!(now.w>0) ||", "")), "mutation control: a donor pass that changed its test fails the pin on the shadow's side");
    expect(!bothTake(shadowVertex, replaced(donor, "asuint(n2.z)==asuint(n0.z)", "true")), "mutation control: a donor pass that stopped requiring one z over a triangle fails it too");
    // 'matched' is the donor pass's rule in the shadow's first shader.
    const char* const matchedRule[] = {"const bool authentic=raw==slot && slot<0x007fffff && identity.z!=0;",
                                       "const uint matched=(authentic && matchedPriors(identity,readable)!=0)?1u:0u;"};
    const auto sameMatched = [&](const std::string& momentsText, const std::string& donorText) {
        for (const char* t : matchedRule) if (momentsText.find(compact(t)) == std::string::npos || donorText.find(compact(t)) == std::string::npos) return false;
        return true;
    };
    expect(sameMatched(moments, donor), "a draw is matched in the shadow's first shader exactly as in the donor pass: authentic and a prior of its identity");
    expect(!sameMatched(replaced(moments, "?1u:0u;", "?1u:1u;"), donor), "mutation control: a shadow in which every draw matched fails the pin");
    expect(!sameMatched(moments, replaced(donor, "identity.z!=0;", "true;")), "mutation control: a donor pass that matched unreadable identities fails it on the shadow's side");

    // -- the first shader: the tables the second reads --
    const auto momentsValid = [&](const std::string& text) {
        return ordered(text, {"const uint draw=sibling.y,count=sibling.z;", "for(uint id=tid;id<count;id+=256){", "if(shadowVertex(id,x,w))addSample(acc,x,w);",
                              "if(tid<s)gAcc[tid]=mergeAcc(gAcc[tid],gAcc[tid+s]);", "const uint b=draw*8;",
                              "Moments[b]=uint4(asuint(t.n),identity.x,identity.y&kIdentityParameterMask,matched);", "Moments[b+1]=asuint(t.mean);", "Moments[b+2]=asuint(t.a);",
                              "Moments[b+3]=asuint(t.b);", "Moments[b+4]=uint4(asuint(t.c),0,0,0);", "Moments[b+5]=asuint(t.lo);", "Moments[b+6]=asuint(t.hi);",
                              "Moments[b+7]=uint4(asuint(t.w.x),asuint(t.w.y),0,0);"});
    };
    expect(momentsValid(moments), "the first shader sums every vertex the shadow takes into centered moments and writes eight uint4 a draw at draw * 8, the identity masked of byte 30");
    expect(!momentsValid(replaced(moments, "identity.y&kIdentityParameterMask,matched", "identity.y,matched")), "mutation control: moments keyed by an identity that keeps byte 30 fail the pin");
    expect(!momentsValid(replaced(moments, "const uint b=draw*8;", "const uint b=draw*7;")), "mutation control: a table stride that is not eight fails the pin");
    expect(!momentsValid(replaced(moments, "if(shadowVertex(id,x,w))addSample(acc,x,w);", "addSample(acc,Now[id],1);")), "mutation control: moments taken from every vertex, valid or not, fail the pin");
    expect(!momentsValid(replaced(moments, "id+=256", "id+=128")), "mutation control: a stride over the vertices that is not the group's size fails the pin");

    // -- the second shader: leave-one-out, the gates, the kinds, the record --
    std::string kinds = "if(unreadable)kind=" + std::to_string(static_cast<unsigned>(ShadowKind::IdentityUnreadable)) + ".0;";
    const std::string kindMatched = "else if(matchedFlag)kind=!hasOwn?" + std::to_string(static_cast<unsigned>(ShadowKind::None)) + ".0:(donors>0?" +
                                    std::to_string(static_cast<unsigned>(ShadowKind::MatchedWithDonors)) + ".0:" + std::to_string(static_cast<unsigned>(ShadowKind::MatchedAlone)) + ".0);";
    const std::string kindReceiver = "else kind=donors>0?" + std::to_string(static_cast<unsigned>(ShadowKind::ReceiverWithDonors)) + ".0:" +
                                     std::to_string(static_cast<unsigned>(ShadowKind::ReceiverAlone)) + ".0;";
    const auto gate = [](const char* condition, unsigned bit) { return std::string(condition) + "gates|=" + std::to_string(bit) + ";"; };
    const auto evalValid = [&](const std::string& text) {
        return ordered(text, {"if(j==self)continue;",
                              "if(r0.w==0 || !(asfloat(r0.x)>0) || r0.y!=identity.x || r0.z!=(identity.y&kIdentityParameterMask))continue;", "p=mergeAcc(p,loadAcc(j));++donors;",
                              kinds, kindMatched, kindReceiver,
                              gate("if(p.n>=kMinVertices)", kGateMinVertices), gate("if(gFit.y<=kSpreadLimit)", kGateSpread), gate("if(p.n>=kMinFitVertices)", kGateFitVertices),
                              gate("if(gFit.w!=0 && gFit.z>=kConditioning)", kGateConditioning), gate("if(gFit.w!=0 && gFit.x<=kResidualLimit)", kGateResidual),
                              "if(rl.x<=rh.x && rl.x>=p.lo.x-mu && rh.x<=p.hi.x+mu && rl.y>=p.lo.y-mv && rh.y<=p.hi.y+mv)gates|=" + std::to_string(kGateHull) + ";",
                              "if(rl.x<=rh.x && rl.z>=p.w.x/kDepthRatio && rh.z<=p.w.y*kDepthRatio)gates|=" + std::to_string(kGateDepth) + ";",
                              "Results[self*3]=uint4(asuint(kind),asuint(float(donors)),asuint(rmsMean),asuint(rmsAffine));",
                              "Results[self*3+1]=uint4(asuint(gFit.x),asuint(gFit.y),asuint(p.n),asuint(float(gates)));",
                              "Results[self*3+2]=uint4(asuint(matchedFlag?asfloat(Moments[self*8+1].z):0.0),asuint(matchedFlag?asfloat(Moments[self*8+1].w):0.0),asuint(evaluated),0);"});
    };
    expect(evalValid(eval), "the second shader skips the draw itself, pools the matched draws of its identity, numbers the kinds and the gate bits as the model does, and writes three uint4 a draw");
    expect(!evalValid(without(eval, "if(j==self)continue;")), "mutation control: a second shader whose draw is its own donor (no leave-one-out) fails the pin");
    expect(!evalValid(replaced(eval, "r0.z!=(identity.y&kIdentityParameterMask)", "r0.z!=identity.y")), "mutation control: donors compared on an identity that keeps byte 30 fail the pin");
    expect(!evalValid(replaced(eval, "r0.w==0 || ", "")), "mutation control: draws that did not match counted as donors fail the pin");
    expect(!evalValid(replaced(eval, "if(rl.x<=rh.x && rl.z>=p.w.x/kDepthRatio", "if(rl.z>=p.w.x/kDepthRatio")), "mutation control: a receiver with no extent passing the depth gate fails the pin");
    expect(!evalValid(replaced(eval, "gFit.x<=kResidualLimit)gates|=16;", "gFit.x<=kResidualLimit)gates|=8;")), "mutation control: a gate bit that is not the model's fails the pin");
    expect(!evalValid(replaced(eval, "else kind=donors>0?3.0:4.0;", "else kind=donors>0?4.0:3.0;")), "mutation control: receivers' kinds swapped fail the pin");
    expect(!evalValid(replaced(eval, "gFit.w!=0 && gFit.z>=kConditioning", "gFit.z>=kConditioning")), "mutation control: a conditioning gate that passes an unsolved fit fails the pin");
    // the CPU decode reads the record the way the shader writes it
    const std::string gpuCompact = compact(gpuSource);
    const auto decodeValid = [&](const std::string& text) {
        return ordered(text, {"r.kind = f[0]; r.donorDraws = f[1]; r.rmsMean = f[2]; r.rmsAffine = f[3]; r.residual = f[4]; r.spread = f[5]; r.pooled = f[6]; "
                              "r.gates = f[7]; r.ownMean[0] = f[8]; r.ownMean[1] = f[9]; r.evaluated = f[10];"}) &&
               text.find("decode(f+d*12)") != std::string::npos && text.find("d.ByteWidth=kDraws*3*16;") != std::string::npos &&
               text.find("D3D11_BOXbox{0,0,0,n*3*16,1,1};") != std::string::npos;
    };
    expect(decodeValid(gpuCompact), "the readback decodes the record's eleven numbers where the shader writes them: three float4 a draw, 12 floats apart");
    expect(!decodeValid(replaced(gpuCompact, "r.rmsMean = f[2];", "r.rmsMean = f[3];")) && !decodeValid(replaced(gpuCompact, "decode(f + d * 12)", "decode(f + d * 11)")),
           "mutation control: a decode that swapped two fields, or a stride that is not twelve floats, fails the pin");

    // -- the map does not read the shadow, nor does the sibling pass --
    const auto readsShadow = [](const std::string& text) { return text.find("Moments") != std::string::npos || text.find("Results") != std::string::npos; };
    expect(!readsShadow(vs) && !readsShadow(ps) && !readsShadow(donor) && !readsShadow(fit),
           "the map's vertex and pixel shaders, the donor pass and the fit read nothing of the shadow's tables (Moments, Results)");
    expect(readsShadow(vs + compact("float4 leak=Moments[0];")) && readsShadow(ps + compact("float r=asfloat(Results[0].x);")) && readsShadow(moments) && readsShadow(eval),
           "mutation control: the detector sees a map that read them, and sees the shadow's own shaders");
    expect(count(vs, "StructuredBuffer<") == 1 && vs.find("StructuredBuffer<float4>Fit:register(t11);") != std::string::npos,
           "and the map's vertex shader binds the one structured buffer it did, the fit, at t11 (the shadow's table is t11 of another shader)");

    // -- the capture class: when the shadow runs, what it is given, that nothing depends on it --
    const std::string motion = compact(motionSource);
    const std::string runShadow = compact(member(motionSource, "void runShadow("));
    const auto runShadowValid = [&](const std::string& text) {
        return ordered(text, {"if(!shadowEvery() || n<2 || n>FlatForegroundShadow::kDraws || frame%shadowEvery()!=0)return;", "shadowInputs_.assign(n,FlatForegroundShadow::DrawInput{});",
                              "shadowConstants_[i]=settingsFor(d,width,height,commonNear);",
                              "shadowConstants_[i].sibling[1]=i;shadowConstants_[i].sibling[2]=d.capture.geometry.count;shadowConstants_[i].sibling[3]=n;",
                              "in.views[0]=d.capture.currentPositions.Get();",
                              "in.views[1+k]=d.priors[k].positions.Get();in.views[6+k]=d.priors[k].identity.Get();",
                              "in.views[5]=d.capture.currentIdentity.Get();in.views[10]=d.capture.instanceIndex.Get();",
                              "in.constants=&shadowConstants_[i];in.constantBytes=sizeof(Settings);", "in.dispatchMoments=d.inputs.gpuIdentity && d.priorCount>0;",
                              "shadow_.run(ctx,shadowInputs_.data(),n,frame);"}) &&
               count(text, "shadow_.run(") == 1;
    };
    expect(runShadowValid(runShadow), "runShadow samples one frame in shadowEvery() (none when it is zero), with two draws at least and at most the table's, and hands the shadow the eleven views the donor pass binds");
    expect(!runShadowValid(without(runShadow, "frame%shadowEvery()!=0")) && !runShadowValid(without(runShadow, "!shadowEvery() || ")),
           "mutation control: a shadow that samples every frame, or that cannot be switched off, fails the pin");
    expect(!runShadowValid(replaced(runShadow, "in.dispatchMoments=d.inputs.gpuIdentity && d.priorCount>0;", "in.dispatchMoments=true;")),
           "mutation control: moments dispatched for a draw with no prior fail the pin");
    expect(!runShadowValid(replaced(runShadow, "shadowConstants_[i].sibling[1]=i;", "shadowConstants_[i].sibling[1]=0;")), "mutation control: every draw written to slot 0 fails the pin");
    expect(!runShadowValid(replaced(runShadow, "in.views[10]=d.capture.instanceIndex.Get();", "in.views[10]=nullptr;")), "mutation control: no retained instance index fails the pin");
    expect(!runShadowValid(replaced(runShadow, "in.views[6+k]=d.priors[k].identity.Get();", "in.views[5+k]=d.priors[k].identity.Get();")), "mutation control: the priors' identity at the wrong slot fails the pin");
    expect(!runShadowValid(runShadow + compact("shadow_.run(ctx,shadowInputs_.data(),n,frame);")),
           "mutation control: a second call to the shadow's run in that function fails the pin");
    expect(count(motion, "shadow_.run(") == 1 && count(compact(runtimeSource), "rigShadow") == 0 && count(motion, "voidrunShadow(") == 1 && count(motion, "runShadow(ctx,width,height,commonNear,frame);") == 1,
           "runShadow is the only caller of the shadow's run, called once, from one place, and the runtime never drives the shadow itself");
    const std::string sibling = "constboolsiblings=runSibling(ctx,width,height,commonNear,frame);", call = "runShadow(ctx,width,height,commonNear,frame);";
    expect(ordered(motion, {sibling, call}), "prepareH runs the shadow after the sibling pass (compute only, before the map's state is bound)");
    expect(!ordered(without(motion, call.c_str()), {sibling, call}), "mutation control: a prepareH that never runs the shadow fails the pin");
    expect(!ordered(replaced(without(motion, call.c_str()), sibling.c_str(), (call + sibling).c_str()), {sibling, call}), "mutation control: the shadow run before the sibling pass fails the pin");
    // Nothing reads the shadow's results but its statistics: every use of the member `shadow_` (as a word, in the source as written) is one of five.
    const auto usesAllowed = [&](const std::string& text) {
        size_t at = 0;
        unsigned seen = 0;
        while ((at = text.find("shadow_", at)) != std::string::npos) {
            const char before = at ? text[at - 1] : ' ';
            if (std::isalnum(static_cast<unsigned char>(before)) || before == '_') { at += 7; continue; }   // flat_foreground_shadow_gpu.h, not the member
            const std::string rest = compact(text.substr(at + 7, 80));
            const bool ok = rest.rfind(".run(ctx,shadowInputs_.data(),n,frame);", 0) == 0 || rest.rfind(".poll(ctx,frame);", 0) == 0 || rest.rfind(".stats();", 0) == 0 ||
                            rest.rfind(";", 0) == 0;
            if (!ok) return false;
            ++seen;
            at += 7;
        }
        return seen == 5;   // run, poll, stats, rigShadow's return, the member
    };
    expect(usesAllowed(motionSource), "the capture class uses its shadow five ways only: run, poll, stats, the rig accessor and the member: nothing reads its results to decide");
    expect(!usesAllowed(motionSource + "\n void bad() { auto r = shadow_.lastRecords(); }") && !usesAllowed(motionSource + "\n void bad() { shadow_.run(ctx, x, n, frame); }"),
           "mutation control: a use of the shadow's records, or of a sixth call, fails the pin");
    expect(count(motion, "lastRecords") == 0 && count(compact(runtimeSource), "lastRecords") == 0, "no production source reads lastRecords(): it is for the rigs");
    expect(ordered(motion, {"explicit RigShadowEvery(unsigned every):was(shadowEvery()){shadowEvery()=every;}", "~RigShadowEvery(){shadowEvery()=was;}",
                            "static unsigned& shadowEvery(){static unsigned every=kFlatShadowEveryFrames;return every;}"}),
           "the sampling interval is kFlatShadowEveryFrames, and the rig's override restores it");
    expect(ordered(motion, {"if(polledFrame_!=frame){polledFrame_=frame;pollIdentity(ctx,frame);pollSibling(ctx,frame);history_.pollExtents(ctx,frame);shadow_.poll(ctx,frame);}"}),
           "the shadow's readback is polled once a frame beside the others");
    expect(ordered(motion, {"history.add(o.history);shadow.add(o.shadow);", "stats_.history=history_.writeStats();stats_.shadow=shadow_.stats();"}),
           "the capture statistics carry the shadow's counters and add them across candidates");
    // -- the 5 s report prints it --
    const std::string runtime = compact(runtimeSource);
    const auto reportValid = [&](const std::string& text) {
        return ordered(text, {"const ShadowStats shadowWindow=flatShadowDelta(captures.shadow,was.shadow);",
                              "flatHistoryLine(line,sizeof(line),history);Log::get().note(\"%s\",line);",
                              "flatShadowLine(line,sizeof(line),shadowWindow);Log::get().note(\"%s\",line);"});
    };
    expect(reportValid(runtime), "the 5 s foreground report prints the shadow line from the window of the capture counters, after the history line");
    expect(!reportValid(replaced(runtime, "flatShadowDelta(captures.shadow,was.shadow)", "captures.shadow")), "mutation control: a line of cumulative counters, not the window, fails the pin");
    expect(!reportValid(without(runtime, "flatShadowLine(line,sizeof(line),shadowWindow);Log::get().note(\"%s\",line);")), "mutation control: a report that never prints the line fails the pin");

    // -- the dispatcher --
    const std::string run = compact(member(gpuSource, "bool run("));
    const auto gpuRunValid = [&](const std::string& text) {
        return ordered(text, {"if(!ctx || !draws || !n || n>kDraws || !draws[0].constants || !draws[0].constantBytes)return false;", "for(Slot& s:slots_)if(!s.pending){slot=&s;break;}",
                              "if(!slot)return false;", "CsStageSave saved;saved.save(ctx);", "ctx->ClearUnorderedAccessViewUint(momentsUav_.Get(),zero);",
                              "ctx->ClearUnorderedAccessViewUint(resultsUav_.Get(),zero);", "ctx->CSSetShader(momentsCs_.Get(),nullptr,0);", "if(!draws[i].dispatchMoments)continue;",
                              "ctx->Dispatch(1,1,1);", "ctx->CSSetUnorderedAccessViews(0,1,&noUav,nullptr);", "ctx->CSSetShader(evalCs_.Get(),nullptr,0);",
                              "ctx->CSSetShaderResources(11,1,&moments);", "ctx->Dispatch(1,1,1);", "ctx->CSSetUnorderedAccessViews(0,1,&noUav,nullptr);",
                              "ctx->CSSetShaderResources(0,12,noViews);", "saved.restore(ctx);", "ctx->CopySubresourceRegion(slot->stage.Get()", "slot->pending=true;"}) &&
               count(text, "ctx->Dispatch(") == 2;
    };
    expect(gpuRunValid(run), "the dispatcher refuses too many draws or none, takes a free slot, saves the compute stage, clears both tables, dispatches moments for the draws with priors then the "
                             "evaluation for every draw, unbinds, restores the stage and copies the results to the slot's staging buffer");
    expect(!gpuRunValid(without(run, "CsStageSave saved;saved.save(ctx);")) && !gpuRunValid(without(run, "saved.restore(ctx);")),
           "mutation control: a dispatcher that leaves the game's compute stage as it found it by luck fails the pin");
    expect(!gpuRunValid(without(run, "ctx->ClearUnorderedAccessViewUint(momentsUav_.Get(),zero);")) && !gpuRunValid(without(run, "ctx->ClearUnorderedAccessViewUint(resultsUav_.Get(),zero);")),
           "mutation control: tables left from the frame before fail the pin");
    expect(!gpuRunValid(replaced(run, "n>kDraws", "n>kDraws+1")), "mutation control: a table smaller than the draws it is given fails the pin");
    expect(!gpuRunValid(without(run, "if(!draws[i].dispatchMoments)continue;")), "mutation control: moments dispatched for draws with no priors fail the pin");
    expect(!gpuRunValid(without(run, "ctx->CSSetShaderResources(0,12,noViews);")), "mutation control: the shadow's views left bound after it fail the pin");
    expect(gpuCompact.find("staticconstexprunsignedkDraws=128,kSlots=4;") != std::string::npos && gpuCompact.find("if(!wait&&frame-s.frame<2)continue;") != std::string::npos,
           "the dispatcher holds 128 draws in 4 slots, and reads a slot two frames after it ran unless a rig waits");
    return failures;
}
