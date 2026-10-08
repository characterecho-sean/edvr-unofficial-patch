#pragma once
// THE SHADOW OF THE SIBLING MODEL (design doc section 104, the 10-07 settlement walk). Computed and logged; it changes nothing.
//
// A draw with no history of its own takes its siblings' motion (flat_foreground_sibling.h): the mean of the donors, when their vertices'
// motions agree to within a pixel per axis. On the 10-07 walk that test refused 1,880 draws in one 5 s window, and by geometry it should: a
// weapon that rotates about the grip moves its parts by about the angle times their distance from the pivot, which is a few pixels across a
// weapon 700 pixels long. The field is not uniform; it is, to first order, affine in the screen position. The question this shadow answers with
// numbers, before anything is applied, is whether an affine model of the donors' motion predicts a piece's motion better than their mean does,
// and where it must refuse.
//
// WHAT IT MEASURES. On a sampled frame, for every draw that matched its own history (its vertices' motions are known exactly), the same-identity
// draws other than itself are the donors it would have had if it had lost its history, and both models predict its motion from them:
//  - the mean model (the production one): the donors' mean motion, everywhere;
//  - the affine model: m(u,v) = m0 + A (p - p0), fitted by least squares on every donor vertex, p the screen position in units of half the
//    render size (u = 2x/W - 1, v = 2y/H - 1), m the motion in pixels.
// Each model's error is the RMS over the draw's vertices of |predicted - own|, in pixels. Both are binned (kFlatShadowEdges). The affine model
// is gated, and the gates are the point:
//  - fit vertices: at least kFlatShadowMinFitVertices donor vertices (an affine fit of fewer is exactly determined, not tested);
//  - conditioning: det(C) / tr(C)^2 of the donors' screen covariance C at least kFlatShadowConditioning (collinear or nearly collinear donors
//    determine the field along a line and nothing across it);
//  - residual: the donors' own RMS residual under the fit at most kFlatShadowResidualPixels (a donor set the model does not explain: parts that
//    move against each other, a skinned piece, a world surface with parallax, is refused);
//  - hull: the receiver's screen extent inside the donors', widened by kFlatShadowHullMargin of it and kFlatShadowHullFloor (no extrapolation);
//  - depth: the receiver's view depth w inside the donors' range widened by kFlatShadowDepthRatio (a piece at another depth moves by another
//    parallax).
// The current policy's accept is the production test: at least kFlatSiblingMinVertices donor vertices and spread (the donors' vertices' largest
// motion less the smallest, per axis, the larger axis) at most kFlatSiblingSpreadPixels.
//
// A receiver (a draw that did not match) is classified the same way from its donors, with no truth to compare: the shadow counts how many the
// current policy refuses (disagree) that the affine gate would accept. A receiver with no donor of its identity is the production's view-attached
// case (mode 2: zero motion, history kept, painted valid); the shadow counts how often it fires in a frame where draws that did match show motion
// above kFlatShadowMovingPixels, which is when zero is wrong.
//
// This header needs no device: the constants, the counters, the CPU form of the model (the rig holds the shaders to it, vertex for vertex), and
// the line. The shaders are in flat_foreground_motion_shader.h and the dispatch in flat_foreground_shadow_gpu.h.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>
#include "flat_foreground_sibling.h"

namespace edvr {

inline constexpr unsigned kFlatShadowMinFitVertices = 24;
inline constexpr float kFlatShadowResidualPixels = 0.5f;
inline constexpr float kFlatShadowConditioning = 0.005f;
inline constexpr float kFlatShadowHullMargin = 0.05f;
inline constexpr float kFlatShadowHullFloor = 0.01f;
inline constexpr float kFlatShadowDepthRatio = 1.25f;
inline constexpr float kFlatShadowMovingPixels = 1.0f;
// The cadence when the shadow is switched on (one frame in this many). It is OFF by default (kFlatShadowDefaultEvery): the 10-07 flight of the
// first build showed the affine sibling model failing on real data (the identity groups pool the weapon with skinned parts of the body), so the
// passes cost nothing in the frame until a rig or a build asks for them.
inline constexpr unsigned kFlatShadowEveryFrames = 8;
inline constexpr unsigned kFlatShadowDefaultEvery = 0;
inline constexpr unsigned kFlatShadowBins = 6;
inline constexpr float kFlatShadowEdges[kFlatShadowBins - 1] = {0.25f, 0.5f, 1.0f, 2.0f, 4.0f};
// The bin an error falls in: <=0.25, <=0.5, <=1, <=2, <=4, more.
inline unsigned flatShadowBin(float error) {
    unsigned b = 0;
    while (b < kFlatShadowBins - 1 && !(error <= kFlatShadowEdges[b])) ++b;
    return b;
}

// What the second shader decided for a draw (the result record's first number).
enum class ShadowKind : unsigned {
    None = 0,          // a matched draw with no vertex to compare, or nothing to say
    MatchedWithDonors, // matched its own history; other same-identity draws matched theirs: leave-one-out
    MatchedAlone,      // matched; no other donor of its identity
    ReceiverWithDonors,// did not match; donors exist
    ReceiverAlone,     // did not match; no donor of its identity: the production's view-attached case
    IdentityUnreadable,// its pool identity cannot be read
    Count
};
inline constexpr unsigned kShadowKinds = static_cast<unsigned>(ShadowKind::Count);

// The gate bits of a result record.
enum ShadowGate : unsigned {
    kGateMinVertices = 1,    // production: at least kFlatSiblingMinVertices donor vertices
    kGateSpread = 2,         // production: spread within kFlatSiblingSpreadPixels
    kGateFitVertices = 4,
    kGateConditioning = 8,
    kGateResidual = 16,
    kGateHull = 32,
    kGateDepth = 64
};
inline constexpr unsigned kGateCurrent = kGateMinVertices | kGateSpread;
inline constexpr unsigned kGateAffine = kGateFitVertices | kGateConditioning | kGateResidual | kGateHull | kGateDepth;

// One draw's result as the second shader writes it: three float4.
struct ShadowRecord {
    float kind = 0, donorDraws = 0, rmsMean = 0, rmsAffine = 0;
    float residual = 0, spread = 0, pooled = 0, gates = 0;
    float ownMean[2] = {0, 0};
    float evaluated = 0, reserved = 0;
    bool currentAccepts() const { return (static_cast<unsigned>(gates) & kGateCurrent) == kGateCurrent; }
    bool affineAccepts() const { return (static_cast<unsigned>(gates) & kGateAffine) == kGateAffine; }
};

struct ShadowStats {
    uint64_t sampledFrames = 0, drawsRead = 0, notReady = 0, failed = 0;
    uint64_t byKind[kShadowKinds] = {};
    // Leave-one-out draws: the error of each model against the draw's own vertices, over all of them and over those the model's gate accepts.
    uint64_t meanAll[kFlatShadowBins] = {}, meanAccepted[kFlatShadowBins] = {}, affineAll[kFlatShadowBins] = {}, affineAccepted[kFlatShadowBins] = {};
    uint64_t residualBins[kFlatShadowBins] = {};
    uint64_t currentAccepts = 0, affineAccepts = 0, bothAccept = 0, neitherAccept = 0;
    uint64_t gateFailed[5] = {};   // fit vertices, conditioning, residual, hull, depth: a draw can fail several
    uint64_t donorDrawsSum = 0, donorVerticesSum = 0;
    // Receivers with donors.
    uint64_t receiverCurrentAccepts = 0, receiverCurrentRefuses = 0, receiverRefusedAffineAccepts = 0, receiverAcceptedAffineAccepts = 0;
    // Receivers with no donor (production mode 2), and the frames where matched draws moved.
    uint64_t modeTwoFires = 0, modeTwoWhileMoving = 0, movingFrames = 0;
    void add(const ShadowStats& o) {
        sampledFrames += o.sampledFrames; drawsRead += o.drawsRead; notReady += o.notReady; failed += o.failed;
        for (unsigned i = 0; i < kShadowKinds; ++i) byKind[i] += o.byKind[i];
        for (unsigned i = 0; i < kFlatShadowBins; ++i) {
            meanAll[i] += o.meanAll[i]; meanAccepted[i] += o.meanAccepted[i];
            affineAll[i] += o.affineAll[i]; affineAccepted[i] += o.affineAccepted[i]; residualBins[i] += o.residualBins[i];
        }
        currentAccepts += o.currentAccepts; affineAccepts += o.affineAccepts; bothAccept += o.bothAccept; neitherAccept += o.neitherAccept;
        for (unsigned i = 0; i < 5; ++i) gateFailed[i] += o.gateFailed[i];
        donorDrawsSum += o.donorDrawsSum; donorVerticesSum += o.donorVerticesSum;
        receiverCurrentAccepts += o.receiverCurrentAccepts; receiverCurrentRefuses += o.receiverCurrentRefuses;
        receiverRefusedAffineAccepts += o.receiverRefusedAffineAccepts; receiverAcceptedAffineAccepts += o.receiverAcceptedAffineAccepts;
        modeTwoFires += o.modeTwoFires; modeTwoWhileMoving += o.modeTwoWhileMoving; movingFrames += o.movingFrames;
    }
};

// The window: the cumulative counters less what the last line had (a counter that went down, a restarted history, is a window of zero).
inline ShadowStats flatShadowDelta(const ShadowStats& now, const ShadowStats& was) {
    const auto d = [](uint64_t a, uint64_t b) { return a > b ? a - b : uint64_t(0); };
    ShadowStats w;
    w.sampledFrames = d(now.sampledFrames, was.sampledFrames); w.drawsRead = d(now.drawsRead, was.drawsRead);
    w.notReady = d(now.notReady, was.notReady); w.failed = d(now.failed, was.failed);
    for (unsigned i = 0; i < kShadowKinds; ++i) w.byKind[i] = d(now.byKind[i], was.byKind[i]);
    for (unsigned i = 0; i < kFlatShadowBins; ++i) {
        w.meanAll[i] = d(now.meanAll[i], was.meanAll[i]); w.meanAccepted[i] = d(now.meanAccepted[i], was.meanAccepted[i]);
        w.affineAll[i] = d(now.affineAll[i], was.affineAll[i]); w.affineAccepted[i] = d(now.affineAccepted[i], was.affineAccepted[i]);
        w.residualBins[i] = d(now.residualBins[i], was.residualBins[i]);
    }
    w.currentAccepts = d(now.currentAccepts, was.currentAccepts); w.affineAccepts = d(now.affineAccepts, was.affineAccepts);
    w.bothAccept = d(now.bothAccept, was.bothAccept); w.neitherAccept = d(now.neitherAccept, was.neitherAccept);
    for (unsigned i = 0; i < 5; ++i) w.gateFailed[i] = d(now.gateFailed[i], was.gateFailed[i]);
    w.donorDrawsSum = d(now.donorDrawsSum, was.donorDrawsSum); w.donorVerticesSum = d(now.donorVerticesSum, was.donorVerticesSum);
    w.receiverCurrentAccepts = d(now.receiverCurrentAccepts, was.receiverCurrentAccepts);
    w.receiverCurrentRefuses = d(now.receiverCurrentRefuses, was.receiverCurrentRefuses);
    w.receiverRefusedAffineAccepts = d(now.receiverRefusedAffineAccepts, was.receiverRefusedAffineAccepts);
    w.receiverAcceptedAffineAccepts = d(now.receiverAcceptedAffineAccepts, was.receiverAcceptedAffineAccepts);
    w.modeTwoFires = d(now.modeTwoFires, was.modeTwoFires); w.modeTwoWhileMoving = d(now.modeTwoWhileMoving, was.modeTwoWhileMoving);
    w.movingFrames = d(now.movingFrames, was.movingFrames);
    return w;
}

// Files one sampled frame's records. `kind` 0 is skipped. The gate failures are counted for draws that have donors (leave-one-out and receivers).
inline void flatShadowAccumulate(ShadowStats& s, const ShadowRecord* records, unsigned count) {
    ++s.sampledFrames;
    bool moving = false;
    for (unsigned i = 0; i < count; ++i) {
        const ShadowRecord& r = records[i];
        const unsigned k = static_cast<unsigned>(r.kind + 0.5f);
        if (k == 0 || k >= kShadowKinds) continue;
        ++s.drawsRead; ++s.byKind[k];
        const ShadowKind kind = static_cast<ShadowKind>(k);
        if (kind == ShadowKind::MatchedWithDonors || kind == ShadowKind::MatchedAlone)
            moving = moving || std::fabs(r.ownMean[0]) > kFlatShadowMovingPixels || std::fabs(r.ownMean[1]) > kFlatShadowMovingPixels;
    }
    if (moving) ++s.movingFrames;
    for (unsigned i = 0; i < count; ++i) {
        const ShadowRecord& r = records[i];
        const unsigned k = static_cast<unsigned>(r.kind + 0.5f);
        if (k == 0 || k >= kShadowKinds) continue;
        const ShadowKind kind = static_cast<ShadowKind>(k);
        const unsigned g = static_cast<unsigned>(r.gates);
        const bool withDonors = kind == ShadowKind::MatchedWithDonors || kind == ShadowKind::ReceiverWithDonors;
        if (withDonors) {
            s.donorDrawsSum += static_cast<uint64_t>(r.donorDraws + 0.5f);
            s.donorVerticesSum += static_cast<uint64_t>(r.pooled + 0.5f);
            if (!(g & kGateFitVertices)) ++s.gateFailed[0];
            else {   // the fit's other gates are meaningful only with enough vertices
                if (!(g & kGateConditioning)) ++s.gateFailed[1];
                if (!(g & kGateResidual)) ++s.gateFailed[2];
            }
            if (!(g & kGateHull)) ++s.gateFailed[3];
            if (!(g & kGateDepth)) ++s.gateFailed[4];
            if ((g & kGateFitVertices) && (g & kGateConditioning)) ++s.residualBins[flatShadowBin(r.residual)];
        }
        if (kind == ShadowKind::MatchedWithDonors) {
            ++s.meanAll[flatShadowBin(r.rmsMean)]; ++s.affineAll[flatShadowBin(r.rmsAffine)];
            if (r.currentAccepts()) ++s.meanAccepted[flatShadowBin(r.rmsMean)];
            if (r.affineAccepts()) ++s.affineAccepted[flatShadowBin(r.rmsAffine)];
            const bool c = r.currentAccepts(), a = r.affineAccepts();
            if (c) ++s.currentAccepts;
            if (a) ++s.affineAccepts;
            if (c && a) ++s.bothAccept;
            if (!c && !a) ++s.neitherAccept;
        } else if (kind == ShadowKind::ReceiverWithDonors) {
            if (r.currentAccepts()) { ++s.receiverCurrentAccepts; if (r.affineAccepts()) ++s.receiverAcceptedAffineAccepts; }
            else { ++s.receiverCurrentRefuses; if (r.affineAccepts()) ++s.receiverRefusedAffineAccepts; }
        } else if (kind == ShadowKind::ReceiverAlone) {
            ++s.modeTwoFires;
            if (moving) ++s.modeTwoWhileMoving;
        }
    }
}

// ---- the CPU form of the model ----
// One vertex of a draw: screen position (u, v), motion in pixels, view depth w. A draw's `hull` lists the positions of every vertex with a valid
// current position (a receiver has no motion, only these); when empty the motion vertices stand for it.
struct ShadowVertex { float u = 0, v = 0, mx = 0, my = 0, w = 0; };
struct ShadowHullVertex { float u = 0, v = 0, w = 0; };
struct ShadowDrawRef {
    uint32_t identityX = 0, identityY = 0;   // identity.x and identity.y with byte 30 left out
    bool identityReadable = true;
    bool matched = false;
    std::vector<ShadowVertex> vertices;
    std::vector<ShadowHullVertex> hull;
};

// Centered moments over (u, v, mx, my): the form both shaders accumulate and merge (Welford per thread, Chan across threads and draws).
struct ShadowMoments {
    double n = 0, mean[4] = {0, 0, 0, 0};
    double suu = 0, suv = 0, svv = 0, sxx = 0, sux = 0, suy = 0, svx = 0, svy = 0, syy = 0;
    double lo[4] = {1e30, 1e30, 1e30, 1e30}, hi[4] = {-1e30, -1e30, -1e30, -1e30};
    double wlo = 1e30, whi = -1e30;
    void add(const ShadowVertex& p) {
        const double x[4] = {p.u, p.v, p.mx, p.my};
        n += 1;
        double d[4], d2[4];
        for (int a = 0; a < 4; ++a) { d[a] = x[a] - mean[a]; mean[a] += d[a] / n; d2[a] = x[a] - mean[a]; lo[a] = std::min(lo[a], x[a]); hi[a] = std::max(hi[a], x[a]); }
        suu += d[0] * d2[0]; suv += d[0] * d2[1]; svv += d[1] * d2[1]; sxx += d[2] * d2[2];
        sux += d[0] * d2[2]; suy += d[0] * d2[3]; svx += d[1] * d2[2]; svy += d[1] * d2[3]; syy += d[3] * d2[3];
        wlo = std::min(wlo, static_cast<double>(p.w)); whi = std::max(whi, static_cast<double>(p.w));
    }
    void merge(const ShadowMoments& b) {
        if (b.n == 0) return;
        if (n == 0) { *this = b; return; }
        const double total = n + b.n, f = n * b.n / total;
        double d[4];
        for (int a = 0; a < 4; ++a) { d[a] = b.mean[a] - mean[a]; }
        suu += b.suu + f * d[0] * d[0]; suv += b.suv + f * d[0] * d[1]; svv += b.svv + f * d[1] * d[1]; sxx += b.sxx + f * d[2] * d[2];
        sux += b.sux + f * d[0] * d[2]; suy += b.suy + f * d[0] * d[3]; svx += b.svx + f * d[1] * d[2]; svy += b.svy + f * d[1] * d[3];
        syy += b.syy + f * d[3] * d[3];
        for (int a = 0; a < 4; ++a) { mean[a] += d[a] * (b.n / total); lo[a] = std::min(lo[a], b.lo[a]); hi[a] = std::max(hi[a], b.hi[a]); }
        wlo = std::min(wlo, b.wlo); whi = std::max(whi, b.whi);
        n = total;
    }
};

// The affine model fitted on pooled donors: m(u, v) = m0 + (ax, bx; ay, by) . (u - u0, v - v0).
struct ShadowFit {
    double n = 0, conditioning = 0, residual = 0, spread = 0;
    double u0 = 0, v0 = 0, m0[2] = {0, 0}, ax = 0, bx = 0, ay = 0, by = 0;
    double ulo = 0, uhi = 0, vlo = 0, vhi = 0, wlo = 0, whi = 0;
    bool solved = false;
};
inline ShadowFit flatShadowFit(const ShadowMoments& m) {
    ShadowFit f;
    f.n = m.n;
    if (m.n <= 0) return f;
    f.u0 = m.mean[0]; f.v0 = m.mean[1]; f.m0[0] = m.mean[2]; f.m0[1] = m.mean[3];
    f.spread = std::max(m.hi[2] - m.lo[2], m.hi[3] - m.lo[3]);
    f.ulo = m.lo[0]; f.uhi = m.hi[0]; f.vlo = m.lo[1]; f.vhi = m.hi[1]; f.wlo = m.wlo; f.whi = m.whi;
    const double det = m.suu * m.svv - m.suv * m.suv, tr = m.suu + m.svv;
    f.conditioning = tr > 0 ? det / (tr * tr) : 0;
    if (!(det > 0) || !(tr > 0)) return f;
    f.ax = (m.svv * m.sux - m.suv * m.svx) / det; f.bx = (-m.suv * m.sux + m.suu * m.svx) / det;
    f.ay = (m.svv * m.suy - m.suv * m.svy) / det; f.by = (-m.suv * m.suy + m.suu * m.svy) / det;
    const double rssx = m.sxx - (f.ax * m.sux + f.bx * m.svx), rssy = m.syy - (f.ay * m.suy + f.by * m.svy);
    f.residual = std::sqrt(std::max(rssx + rssy, 0.0) / m.n);
    f.solved = true;
    return f;
}

// The model's result for every draw of a frame, as the second shader computes it.
inline std::vector<ShadowRecord> flatShadowReference(const std::vector<ShadowDrawRef>& draws) {
    const size_t count = draws.size();
    std::vector<ShadowMoments> own(count);
    for (size_t i = 0; i < count; ++i)
        if (draws[i].matched) for (const ShadowVertex& p : draws[i].vertices) own[i].add(p);
    std::vector<ShadowRecord> out(count);
    for (size_t i = 0; i < count; ++i) {
        const ShadowDrawRef& d = draws[i];
        ShadowRecord& r = out[i];
        if (!d.identityReadable) { r.kind = static_cast<float>(ShadowKind::IdentityUnreadable); continue; }
        ShadowMoments pooled;
        unsigned donors = 0;
        for (size_t j = 0; j < count; ++j) {
            if (j == i || !draws[j].matched || own[j].n == 0 || draws[j].identityX != d.identityX || draws[j].identityY != d.identityY) continue;
            pooled.merge(own[j]); ++donors;
        }
        const bool matched = d.matched;
        if (matched && own[i].n == 0) continue;   // nothing to compare
        r.kind = static_cast<float>(matched ? (donors ? ShadowKind::MatchedWithDonors : ShadowKind::MatchedAlone)
                                            : (donors ? ShadowKind::ReceiverWithDonors : ShadowKind::ReceiverAlone));
        if (matched) { r.ownMean[0] = static_cast<float>(own[i].mean[2]); r.ownMean[1] = static_cast<float>(own[i].mean[3]); }
        if (!donors) continue;
        const ShadowFit fit = flatShadowFit(pooled);
        r.donorDraws = static_cast<float>(donors); r.pooled = static_cast<float>(fit.n);
        r.spread = static_cast<float>(fit.spread); r.residual = static_cast<float>(fit.residual);
        // the receiver's extent
        double ulo = 1e30, uhi = -1e30, vlo = 1e30, vhi = -1e30, wlo = 1e30, whi = -1e30;
        const auto extent = [&](double u, double v, double w) {
            ulo = std::min(ulo, u); uhi = std::max(uhi, u); vlo = std::min(vlo, v); vhi = std::max(vhi, v); wlo = std::min(wlo, w); whi = std::max(whi, w);
        };
        if (!d.hull.empty()) for (const ShadowHullVertex& h : d.hull) extent(h.u, h.v, h.w);
        else for (const ShadowVertex& p : d.vertices) extent(p.u, p.v, p.w);
        unsigned gates = 0;
        if (fit.n >= kFlatSiblingMinVertices) gates |= kGateMinVertices;
        if (fit.spread <= kFlatSiblingSpreadPixels) gates |= kGateSpread;
        if (fit.n >= kFlatShadowMinFitVertices) gates |= kGateFitVertices;
        if (fit.solved && fit.conditioning >= kFlatShadowConditioning) gates |= kGateConditioning;
        if (fit.solved && fit.residual <= kFlatShadowResidualPixels) gates |= kGateResidual;
        const double mu = kFlatShadowHullMargin * (fit.uhi - fit.ulo) + kFlatShadowHullFloor;
        const double mv = kFlatShadowHullMargin * (fit.vhi - fit.vlo) + kFlatShadowHullFloor;
        const bool any = ulo <= uhi;   // a draw with no valid current vertex has no extent to be inside anything
        if (any && ulo >= fit.ulo - mu && uhi <= fit.uhi + mu && vlo >= fit.vlo - mv && vhi <= fit.vhi + mv) gates |= kGateHull;
        if (any && wlo >= fit.wlo / kFlatShadowDepthRatio && whi <= fit.whi * kFlatShadowDepthRatio) gates |= kGateDepth;
        r.gates = static_cast<float>(gates);
        if (matched) {
            double sm = 0, sa = 0;
            for (const ShadowVertex& p : d.vertices) {
                const double ex = p.mx - fit.m0[0], ey = p.my - fit.m0[1];
                sm += ex * ex + ey * ey;
                const double du = p.u - fit.u0, dv = p.v - fit.v0;
                const double px = fit.m0[0] + fit.ax * du + fit.bx * dv, py = fit.m0[1] + fit.ay * du + fit.by * dv;
                const double fx = p.mx - px, fy = p.my - py;
                sa += fx * fx + fy * fy;
            }
            const double nv = static_cast<double>(d.vertices.size());
            r.rmsMean = static_cast<float>(std::sqrt(sm / nv));
            r.rmsAffine = fit.solved ? static_cast<float>(std::sqrt(sa / nv)) : r.rmsMean;
            r.evaluated = static_cast<float>(nv);
        }
    }
    return out;
}

// The window's counters, as the log lines print them: three lines (the logger cuts at about 1167 characters), each part self-describing.
inline constexpr unsigned kFlatShadowLineParts = 3;
inline int flatShadowLine(char* out, size_t n, const ShadowStats& w, unsigned part = 0) {
    const auto u = [](uint64_t v) { return static_cast<unsigned long long>(v); };
    const auto bins = [&](char* text, size_t size, const uint64_t* set) {
        std::snprintf(text, size, "<=0.25=%llu <=0.5=%llu <=1=%llu <=2=%llu <=4=%llu more=%llu", u(set[0]), u(set[1]), u(set[2]), u(set[3]), u(set[4]), u(set[5]));
    };
    char first[200], second[200];
    if (part == 0) {
        bins(first, sizeof(first), w.meanAll);
        bins(second, sizeof(second), w.meanAccepted);
        return std::snprintf(out, n,
            "flat foreground shadow 5s (1/3): sampled-frames=%llu draws-read=%llu (frames unread=%llu failed=%llu) by kind: matched-with-donors=%llu "
            "matched-alone=%llu receiver-with-donors=%llu receiver-alone=%llu identity-unreadable=%llu; leave-one-out error in pixels, draws by RMS over "
            "the draw's vertices: mean-model all {%s} mean-model where the current policy accepts {%s}",
            u(w.sampledFrames), u(w.drawsRead), u(w.notReady), u(w.failed), u(w.byKind[1]), u(w.byKind[2]), u(w.byKind[3]), u(w.byKind[4]),
            u(w.byKind[5]), first, second);
    }
    if (part == 1) {
        char third[200];
        bins(first, sizeof(first), w.affineAll);
        bins(second, sizeof(second), w.affineAccepted);
        bins(third, sizeof(third), w.residualBins);
        return std::snprintf(out, n,
            "flat foreground shadow 5s (2/3): affine-model all {%s} affine-model where its gate accepts {%s} donors' own fit residual {%s}",
            first, second, third);
    }
    if (part == 2) {
        const double leave = static_cast<double>(w.byKind[static_cast<unsigned>(ShadowKind::MatchedWithDonors)]);
        const double withDonors = leave + static_cast<double>(w.byKind[static_cast<unsigned>(ShadowKind::ReceiverWithDonors)]);
        return std::snprintf(out, n,
            "flat foreground shadow 5s (3/3): accepts over leave-one-out draws: current=%llu affine=%llu both=%llu neither=%llu; affine gate failures (a draw "
            "can fail several): fit-vertices=%llu conditioning=%llu residual=%llu hull=%llu depth=%llu; donors per draw with donors: draws=%.1f "
            "vertices=%.0f; receivers with donors: current-accepts=%llu current-refuses(disagree)=%llu affine-would-accept-of-refused=%llu "
            "affine-would-accept-of-accepted=%llu; view-attached (receiver with no donor of its identity) fires=%llu "
            "while-matched-draws-moved-over-1px=%llu (frames with such motion=%llu); computed on one frame in %u, applied to nothing",
            u(w.currentAccepts), u(w.affineAccepts), u(w.bothAccept), u(w.neitherAccept), u(w.gateFailed[0]), u(w.gateFailed[1]),
            u(w.gateFailed[2]), u(w.gateFailed[3]), u(w.gateFailed[4]), withDonors > 0 ? static_cast<double>(w.donorDrawsSum) / withDonors : 0.0,
            withDonors > 0 ? static_cast<double>(w.donorVerticesSum) / withDonors : 0.0, u(w.receiverCurrentAccepts), u(w.receiverCurrentRefuses),
            u(w.receiverRefusedAffineAccepts), u(w.receiverAcceptedAffineAccepts), u(w.modeTwoFires), u(w.modeTwoWhileMoving), u(w.movingFrames),
            kFlatShadowEveryFrames);
    }
    return 0;
}

}  // namespace edvr
