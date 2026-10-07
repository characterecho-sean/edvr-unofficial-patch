#pragma once

// Celestial body motion: the arithmetic, with no D3D in it, so tools/celestial_motion_test runs it on the real
// dumped constants (docs/terrain-motion-dispatch-cost-2026-09-17.md, 2026-10-06 "the design").
//
// WHY. In supercruise the game's camera rows keep the head's rotation and lose the ship's translation, so the
// world path (temporal_shader_source.h, decision path 2) gives a planet the camera's motion alone while the ship
// closes on it at several km a frame. Elite draws a body near range as cube-sphere surface patches (VS
// 72BDD292154158AD, colour pass; depth pass ACE405F428C17EF6) whose constants say exactly where the body is each
// frame. This file turns two frames of those constants into one rigid motion per body, and a coverage volume.
//
// THE PATCH VS (disassembly of vs_72BDD292154158AD, the CB2 rows used):
//   X  = rotate(q = cb2[10], lerp(cb2[4], cb2[5], h)) + cb2[2]            camera-relative, head axes (+Z forward)
//   X2 = rotate(q, rotate(cb2[23], lerp(cb2[6], cb2[7], h2)) + cb2[8]) + cb2[2]   the LOD-morph target
//   X' = X + (X2 - X) * saturate(|X - cb2[3]|^2 * cb2[0].z + cb2[0].w)
//   world-aligned = cb0[9..11] . (X', 1)   (the camera rows' 3x3; w = 0)        clip = cb1[270..273] . that
//   cb2[12] = (the body's centre, world-aligned and camera-relative; its radius): bit-equal on every patch of a
//   body, distinct between bodies (checked on eye dump 180540: |A c - centre| is the radius within 5 km).
// Everything a patch needs for placement is therefore c, q, the two boxes (cb2[4..7], also the patch's identity
// across frames), o, q2 and the rows A.
//
// ONE RIGID DELTA PER BODY. For a patch, T = [A R(q) | A c] maps its local points to world-aligned, camera-relative
// space. D = T_prev T_cur^-1 maps a current point on the body to where it was last frame: X_prev = R X_cur + t.
// Every patch of a rigid body gives the same D to float32 rounding (|c| ulp: 0.25 m at 4e6 m, 64 m at 8e8 m).
//
// THE SHADER'S [R|t]. The pass works in the eye's view space with -Z forward (the game's view is +Z forward), and
// a view point V relates to the world-aligned one by X = A V. So V_prev = A_p^T D A_cur V, conjugated by the z flip
// F = diag(1,1,-1): M = F (A_p^T R A_c) F, tv = F (A_p^T t) -- the same form temporalWorldFromRows gives the
// camera (src/common/temporal_math.h), which is the D of a static world seen from a moving camera (R = I, t = c_n - c_p).

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace edvr {
namespace celestial {

constexpr uint32_t kMaxPatches = 512;   // per eye per frame; the rest are counted and left to the camera term
constexpr uint32_t kMaxBodies = 16;     // records per eye; nearest first
constexpr uint32_t kMaxGroups = 64;     // bodies tracked while grouping, before the nearest kMaxBodies are kept
constexpr uint32_t kRecordFloats = 20;  // five float4: m0 m1 m2 (rows of [R|t]), box (x0 y0 x1 y1 px), span (zmin zmax valid 0)

// What one patch draw contributes. The first three rows of cb0 and the first 24 of cb2 are all a patch needs.
struct Patch {
    float c[3];        // cb2[2].xyz
    float q[4];        // cb2[10]   (x y z w)
    float rows[16];    // cb2[4..7]: box one (min, max) and box two (min, max); the patch's identity across frames
    float o[3];        // cb2[8].xyz
    float q2[4];       // cb2[23]
    float body[4];     // cb2[12]: the body's centre (world-aligned, camera-relative) and radius
    float A[9];        // cb0[9..11].xyz, row-major
    uint32_t hash;     // hashRows(rows), made once at capture
};

// The part of the game's constants a patch is read from, as the CPU shadow holds it.
constexpr uint32_t kB2Rows = 24;                 // VS b2 rows 0..23
constexpr uint32_t kB2Bytes = kB2Rows * 16;
constexpr uint32_t kB0Offset = 9 * 16;           // VS b0 rows 9..11
constexpr uint32_t kB0Bytes = 3 * 16;
constexpr uint32_t kB2MinBytes = kB2Bytes;       // a bound buffer smaller than this cannot be a patch's
constexpr uint32_t kB0MinBytes = kB0Offset + kB0Bytes;

enum Fallback : uint32_t {
    kFbNoPrevFrame = 0,   // the eye has no patches from the frame before (first frame, a gap, stale)
    kFbNoPrevBody,        // no body of the previous frame with this radius and a near centre
    kFbNoMatch,           // not one patch of the body has a unique counterpart by its static rows
    kFbImplausible,       // non-finite, translation beyond the body's distance, a spin over 10 degrees, no volume
    kFbDisagree,          // the matched patches disagree beyond float tolerance: no consensus
    kFbCount
};
inline const char* fallbackName(uint32_t f) {
    static const char* const names[kFbCount] = {"no-previous-frame", "no-previous-body", "no-match", "implausible", "disagree"};
    return f < kFbCount ? names[f] : "unknown";
}

// Bodies the build leaves alone without calling it a failure: a body wholly behind the eye (the eye dump's five far
// bodies are 4e8 to 8e8 m behind the camera, drawn and clipped), or whose volume misses the eye's pixels.
enum Skip : uint32_t { kSkipNone = 0, kSkipBehind, kSkipOffscreen };

// Why a draw's constants could not become a Patch.
enum Capture : uint32_t { kCaptureOk = 0, kCaptureNonFinite, kCaptureRowsMove, kCaptureBadQuaternion };

inline uint32_t hashRows(const float rows[16]) {
    uint32_t h = 2166136261u;
    for (int i = 0; i < 16; ++i) {
        uint32_t w;
        std::memcpy(&w, &rows[i], 4);
        h = (h ^ w) * 16777619u;
    }
    return h;
}

inline bool finite3(const float* v) { return std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]); }

// A patch from the shadowed bytes: b2 = VS b2 rows 0..23 (384 bytes), b0rows = VS b0 rows 9..11 (48 bytes).
inline Capture patchFromBlocks(const uint8_t* b2, const uint8_t* b0rows, Patch& out) {
    float f2[kB2Rows * 4], f0[12];
    std::memcpy(f2, b2, sizeof(f2));
    std::memcpy(f0, b0rows, sizeof(f0));
    std::memcpy(out.c, &f2[2 * 4], 12);
    std::memcpy(out.q, &f2[10 * 4], 16);
    std::memcpy(out.rows, &f2[4 * 4], 64);
    std::memcpy(out.o, &f2[8 * 4], 12);
    std::memcpy(out.q2, &f2[23 * 4], 16);
    std::memcpy(out.body, &f2[12 * 4], 16);
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) out.A[i * 3 + j] = f0[i * 4 + j];
    bool ok = finite3(out.c) && finite3(out.o) && finite3(out.body) && std::isfinite(out.body[3]);
    for (int i = 0; i < 16 && ok; ++i) ok = std::isfinite(out.rows[i]);
    for (int i = 0; i < 4 && ok; ++i) ok = std::isfinite(out.q[i]) && std::isfinite(out.q2[i]);
    for (int i = 0; i < 9 && ok; ++i) ok = std::isfinite(out.A[i]);
    if (!ok) return kCaptureNonFinite;
    // The rows' w are the translation of a 3x4 matrix; the patch chain has none (it is camera-relative). A nonzero
    // one means these are not the rows this math reads.
    if (std::fabs(f0[3]) > 1e-3f || std::fabs(f0[7]) > 1e-3f || std::fabs(f0[11]) > 1e-3f) return kCaptureRowsMove;
    const double qn = double(out.q[0]) * out.q[0] + double(out.q[1]) * out.q[1] + double(out.q[2]) * out.q[2] + double(out.q[3]) * out.q[3];
    if (!(qn > 0.9 && qn < 1.1)) return kCaptureBadQuaternion;
    out.hash = hashRows(out.rows);
    return kCaptureOk;
}

// --- small double-precision 3x3 helpers (row-major) ---------------------------------------------------------------
using M3 = double[9];
inline void mulM(const double* a, const double* b, double* out) {   // out = a b
    double r[9];
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) r[i * 3 + j] = a[i * 3] * b[j] + a[i * 3 + 1] * b[3 + j] + a[i * 3 + 2] * b[6 + j];
    std::memcpy(out, r, sizeof(r));
}
inline void mulMT(const double* a, const double* b, double* out) {  // out = a b^T
    double r[9];
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) r[i * 3 + j] = a[i * 3] * b[j * 3] + a[i * 3 + 1] * b[j * 3 + 1] + a[i * 3 + 2] * b[j * 3 + 2];
    std::memcpy(out, r, sizeof(r));
}
inline void mulTM(const double* a, const double* b, double* out) {  // out = a^T b
    double r[9];
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) r[i * 3 + j] = a[i] * b[j] + a[3 + i] * b[3 + j] + a[6 + i] * b[6 + j];
    std::memcpy(out, r, sizeof(r));
}
inline void mulV(const double* a, const double* v, double* out) {   // out = a v
    const double x = a[0] * v[0] + a[1] * v[1] + a[2] * v[2];
    const double y = a[3] * v[0] + a[4] * v[1] + a[5] * v[2];
    const double z = a[6] * v[0] + a[7] * v[1] + a[8] * v[2];
    out[0] = x; out[1] = y; out[2] = z;
}
inline void mulTV(const double* a, const double* v, double* out) {  // out = a^T v
    const double x = a[0] * v[0] + a[3] * v[1] + a[6] * v[2];
    const double y = a[1] * v[0] + a[4] * v[1] + a[7] * v[2];
    const double z = a[2] * v[0] + a[5] * v[1] + a[8] * v[2];
    out[0] = x; out[1] = y; out[2] = z;
}
inline double len3(const double* v) { return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); }
inline double dist3(const double* a, const double* b) {
    const double d[3] = {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
    return len3(d);
}
// The rotation of a quaternion (x y z w), normalised first: the game's float quaternions are unit only to ~1e-7, and
// the VS formula then scales by |q|^2 -- a rotation read from the unnormalised matrix carries a 1e-7 scale whose
// trace looks like a 0.03 degree turn (eye dump 180540). False for a zero or non-finite quaternion.
inline bool quatToMat(const float q[4], double R[9]) {
    double x = q[0], y = q[1], z = q[2], w = q[3];
    const double n = std::sqrt(x * x + y * y + z * z + w * w);
    if (!(n > 1e-6) || !std::isfinite(n)) return false;
    x /= n; y /= n; z /= n; w /= n;
    R[0] = 1 - 2 * (y * y + z * z); R[1] = 2 * (x * y - z * w);     R[2] = 2 * (x * z + y * w);
    R[3] = 2 * (x * y + z * w);     R[4] = 1 - 2 * (x * x + z * z); R[5] = 2 * (y * z - x * w);
    R[6] = 2 * (x * z - y * w);     R[7] = 2 * (y * z + x * w);     R[8] = 1 - 2 * (x * x + y * y);
    return true;
}
inline void rotateQuat(const float q[4], const double v[3], double out[3]) {
    double R[9];
    quatToMat(q, R);
    mulV(R, v, out);
}
inline double rotationDeg(const double R[9]) {
    const double c = std::max(-1.0, std::min(1.0, (R[0] + R[4] + R[8] - 1.0) * 0.5));
    return std::acos(c) * (180.0 / 3.14159265358979323846);
}

// The patch's place in world-aligned camera-relative space: T = [A R(q) | A c].
struct Placement {
    double R[9], t[3];   // R = A Rq, t = A c
    bool ok;
};
inline Placement placement(const Patch& p) {
    Placement pl{};
    double A[9], Rq[9];
    for (int i = 0; i < 9; ++i) A[i] = p.A[i];
    pl.ok = quatToMat(p.q, Rq);
    mulM(A, Rq, pl.R);
    const double c[3] = {p.c[0], p.c[1], p.c[2]};
    mulV(A, c, pl.t);
    return pl;
}
inline double distanceOf(const Patch& p) {
    return std::sqrt(double(p.c[0]) * p.c[0] + double(p.c[1]) * p.c[1] + double(p.c[2]) * p.c[2]);
}

// float32's unit in the last place at a magnitude, bounded above (x * 2^-23). A tolerance is whole units of it.
inline double ulp32(double magnitude) { return magnitude * 1.1920929e-7; }
// How far two frames' readings of one patch may disagree about a rigid delta, metres: 12 ulp of the larger distance
// plus a metre. The moon's six faces agreed to 3 m at 3.3e6 m (eye dump 180540); 12 ulp is 4.7 m there.
inline double agreementTolerance(double dCur, double dPrev) { return 1.0 + 12.0 * ulp32(std::max(dCur, dPrev)); }

// --- the eye -------------------------------------------------------------------------------------------------------
struct EyeInput {
    float tan[4];   // this frame's tangents, the pass's order: l r t b with b the TOP (d.y = tan.w - v (tan.w - tan.z)), jitter excluded
    int w, h;       // the render size the pass dispatches over
};

// The pixel and depth a head-axes point (+Z forward) has in the pass: false when it is not in front of the eye.
inline bool projectHead(const float tan[4], int w, int h, const double X[3], double* px, double* py, double* z) {
    if (!(X[2] > 1e-3)) return false;
    const double xt = X[0] / X[2], yt = X[1] / X[2];
    *px = (xt - tan[0]) / (double(tan[1]) - tan[0]) * w - 0.5;
    *py = (double(tan[3]) - yt) / (double(tan[3]) - tan[2]) * h - 0.5;
    *z = X[2];
    return std::isfinite(*px) && std::isfinite(*py);
}

// One body's result for one eye and frame.
struct BodyResult {
    bool ok = false;
    uint32_t fallback = kFbNoPrevFrame;   // the reason when !ok and not skipped
    uint32_t skip = kSkipNone;            // set for a body left alone (behind the eye, off the eye's pixels)
    uint32_t patches = 0, matched = 0, agreeing = 0;
    float radius = 0.0f;
    double centre[3] = {};                // cb2[12].xyz, this frame
    double nearest = 0.0;                 // the smallest patch distance, metres
    double R[9] = {}, t[3] = {};          // D, world-aligned: X_prev = R X_cur + t
    double rotDeg = 0.0, displacement = 0.0;   // D's turn, and how far D moves the nearest agreeing patch's centre
    double M[9] = {}, tv[3] = {};         // the shader's view-space [R|t]
    double box[4] = {};                   // pixel rectangle x0 y0 x1 y1, margins in
    double zmin = 0.0, zmax = 0.0;        // view-depth interval, margins in
    bool straddle = false;                // a patch box reaches the eye plane: the volume is the whole eye
};

struct BuildResult {
    uint32_t bodies = 0;                  // bodies the frame's patches form (before the cap)
    uint32_t records = 0;                 // records written: the bodies with a delta, nearest first, at most kMaxBodies
    uint32_t patches = 0, matched = 0, unmatched = 0;   // unmatched: patches of the bodies the build worked on, without a counterpart
    uint32_t overflowBodies = 0;          // bodies past kMaxGroups, and records past kMaxBodies
    uint32_t behind = 0, offscreen = 0;   // bodies skipped, by kSkip*
    uint32_t fallbacks[kFbCount] = {};
    double maxDisplacement = 0.0;         // the largest accepted displacement this build, metres
    BodyResult body[kMaxGroups];          // every body, in group order
    uint32_t order[kMaxBodies] = {};      // body indices of the records, nearest first
    float gpu[kMaxBodies][kRecordFloats] = {};
};

namespace detail {
struct Group { float key[4]; uint32_t count; };
struct Pair { uint32_t ci, pi; double dist; };
struct Seen { Placement c, p; double dc, dp; };

inline bool sameKey(const float a[4], const float b[4]) { return std::memcmp(a, b, 16) == 0; }

// Patches by body: the group index of each, and the groups in first-seen order.
inline uint32_t groupPatches(const Patch* p, uint32_t n, Group* g, uint32_t* groupOf, uint32_t* overflow) {
    uint32_t groups = 0;
    for (uint32_t i = 0; i < n; ++i) {
        uint32_t k = 0;
        while (k < groups && !sameKey(g[k].key, p[i].body)) ++k;
        if (k == groups) {
            if (groups == kMaxGroups) { groupOf[i] = UINT32_MAX; ++*overflow; continue; }
            std::memcpy(g[groups].key, p[i].body, 16);
            g[groups].count = 0;
            ++groups;
        }
        groupOf[i] = k;
        ++g[k].count;
    }
    return groups;
}

// Box corners and their head-axes points, both boxes, every corner of one patch. 16 points.
inline void patchCorners(const Patch& p, double out[16][3]) {
    double R2[9];
    const bool haveQ2 = quatToMat(p.q2, R2);
    int n = 0;
    for (int box = 0; box < 2; ++box) {
        const float* lo = &p.rows[box * 8];
        const float* hi = &p.rows[box * 8 + 4];
        for (int ix = 0; ix < 2; ++ix)
            for (int iy = 0; iy < 2; ++iy)
                for (int iz = 0; iz < 2; ++iz) {
                    double v[3] = {ix ? hi[0] : lo[0], iy ? hi[1] : lo[1], iz ? hi[2] : lo[2]};
                    if (box == 1) {
                        // the morph target: rotate(q, rotate(q2, v) + o)
                        double w[3];
                        if (haveQ2) mulV(R2, v, w); else { w[0] = v[0]; w[1] = v[1]; w[2] = v[2]; }
                        v[0] = w[0] + p.o[0]; v[1] = w[1] + p.o[1]; v[2] = w[2] + p.o[2];
                    }
                    double X[3];
                    rotateQuat(p.q, v, X);
                    out[n][0] = X[0] + p.c[0]; out[n][1] = X[1] + p.c[1]; out[n][2] = X[2] + p.c[2];
                    ++n;
                }
    }
}
}  // namespace detail

// The working memory build() uses, held by the caller so no frame pays for 130 KB of stack. Single-threaded use.
struct Scratch {
    detail::Group cg[kMaxGroups], pg[kMaxGroups];
    uint32_t cgOf[kMaxPatches], pgOf[kMaxPatches], used[kMaxPatches];
    detail::Pair pairs[kMaxPatches];
    detail::Seen seen[kMaxPatches];
};

namespace detail {
// The consensus of np matched pairs (s.pairs sorted nearest first, s.seen filled): hypotheses from the nearest few
// patches' own deltas, the one the most patches agree with wins (ties keep the nearer), the translation the
// inverse-square-distance weighted mean over its agreeing patches with the winner's rotation held. Fills r.R, r.t,
// r.agreeing, r.displacement, r.rotDeg and returns the winning hypothesis; kFbCount when all is well, else the reason.
inline uint32_t consensus(Scratch& s, uint32_t np, BodyResult& r, uint32_t* winner) {
    const uint32_t hyps = std::min<uint32_t>(np, 4);
    uint32_t bestHyp = 0, bestCount = 0;
    double hypR[4][9], hypT[4][3];
    for (uint32_t h = 0; h < hyps; ++h) {
        mulMT(s.seen[h].p.R, s.seen[h].c.R, hypR[h]);   // R = Rp Rc^T
        double Rt[3];
        mulV(hypR[h], s.seen[h].c.t, Rt);
        for (int i = 0; i < 3; ++i) hypT[h][i] = s.seen[h].p.t[i] - Rt[i];
        uint32_t count = 0;
        for (uint32_t k = 0; k < np; ++k) {
            double mapped[3];
            mulV(hypR[h], s.seen[k].c.t, mapped);
            for (int i = 0; i < 3; ++i) mapped[i] += hypT[h][i];
            if (dist3(mapped, s.seen[k].p.t) <= agreementTolerance(s.seen[k].dc, s.seen[k].dp)) ++count;
        }
        if (count > bestCount) { bestCount = count; bestHyp = h; }
    }
    *winner = bestHyp;
    std::memcpy(r.R, hypR[bestHyp], sizeof(r.R));
    double tSum[3] = {}, wSum = 0.0;
    uint32_t agree = 0;
    for (uint32_t k = 0; k < np; ++k) {
        double mapped[3];
        mulV(r.R, s.seen[k].c.t, mapped);
        double at[3];
        for (int i = 0; i < 3; ++i) at[i] = mapped[i] + hypT[bestHyp][i];
        if (dist3(at, s.seen[k].p.t) > agreementTolerance(s.seen[k].dc, s.seen[k].dp)) continue;
        const double w = 1.0 / ((1.0 + s.seen[k].dc) * (1.0 + s.seen[k].dc));
        for (int i = 0; i < 3; ++i) tSum[i] += w * (s.seen[k].p.t[i] - mapped[i]);
        wSum += w;
        ++agree;
    }
    r.agreeing = agree;
    if (agree == 0 || !(wSum > 0.0)) return kFbDisagree;
    for (int i = 0; i < 3; ++i) r.t[i] = tSum[i] / wSum;
    if ((np >= 3 && agree * 2 < np) || (np == 2 && agree == 1)) return kFbDisagree;
    // Plausible: finite, a displacement no larger than the body is far, a spin under ten degrees.
    double mapped[3];
    mulV(r.R, s.seen[0].c.t, mapped);
    for (int i = 0; i < 3; ++i) mapped[i] += r.t[i];
    r.displacement = dist3(mapped, s.seen[0].c.t);
    r.rotDeg = rotationDeg(r.R);
    bool finite = std::isfinite(r.displacement) && std::isfinite(r.rotDeg);
    for (int i = 0; i < 9; ++i) finite = finite && std::isfinite(r.R[i]);
    for (int i = 0; i < 3; ++i) finite = finite && std::isfinite(r.t[i]);
    if (!finite || r.displacement > std::max(s.seen[0].dc, s.seen[0].dp) || r.rotDeg > 10.0) return kFbImplausible;
    return kFbCount;
}

// The shader's [R|t] from D and the two frames' rows: M = F (A_p^T R A_c) F, tv = F (A_p^T t), F = diag(1,1,-1) -- the
// conjugation changes the sign of entries (0,2) (1,2) (2,0) (2,1) and of tv.z (temporalWorldFromRows does the same).
inline void viewFrame(const float Ac[9], const float Ap[9], BodyResult& r) {
    double c[9], p[9], tmp[9], M[9], tt[3];
    for (int i = 0; i < 9; ++i) { c[i] = Ac[i]; p[i] = Ap[i]; }
    mulTM(p, r.R, tmp);
    mulM(tmp, c, M);
    mulTV(p, r.t, tt);
    M[2] = -M[2]; M[5] = -M[5]; M[6] = -M[6]; M[7] = -M[7];
    tt[2] = -tt[2];
    std::memcpy(r.M, M, sizeof(r.M));
    std::memcpy(r.tv, tt, sizeof(r.tv));
}

// The coverage volume of a body: every corner of both boxes of every one of its patches, projected as the pass does.
// A corner at or behind the eye plane makes the volume the whole eye from the plane out (a landed ship's patches
// reach under it). 0 ok, 1 wholly behind the eye, 2 not finite.
inline int volume(const Patch* cur, uint32_t nCur, const uint32_t* groupOf, uint32_t body, const EyeInput& eye, BodyResult& r) {
    double x0 = 1e300, y0 = 1e300, x1 = -1e300, y1 = -1e300, z0 = 1e300, z1 = -1e300;
    bool straddle = false, finite = true, any = false;
    for (uint32_t i = 0; i < nCur; ++i) {
        if (groupOf[i] != body) continue;
        double pts[16][3];
        patchCorners(cur[i], pts);
        for (int k = 0; k < 16; ++k) {
            if (!std::isfinite(pts[k][0]) || !std::isfinite(pts[k][1]) || !std::isfinite(pts[k][2])) { finite = false; continue; }
            z0 = std::min(z0, pts[k][2]);
            z1 = std::max(z1, pts[k][2]);
            double px, py, z;
            if (!projectHead(eye.tan, eye.w, eye.h, pts[k], &px, &py, &z)) { straddle = true; continue; }
            any = true;
            x0 = std::min(x0, px); x1 = std::max(x1, px);
            y0 = std::min(y0, py); y1 = std::max(y1, py);
        }
    }
    if (!finite) return 2;
    if (!any) return 1;   // no corner in front of the eye: the body is behind it
    r.straddle = straddle;
    if (straddle) {
        r.box[0] = -4.0; r.box[1] = -4.0; r.box[2] = eye.w + 4.0; r.box[3] = eye.h + 4.0;
        r.zmin = 0.0;
    } else {
        // Margins: three pixels, a percent of depth. The pass reads the NEAREST depth of a pixel's 3x3, so a pixel
        // beside the silhouette carries the body's depth and must still be inside.
        r.box[0] = x0 - 3.0; r.box[1] = y0 - 3.0; r.box[2] = x1 + 3.0; r.box[3] = y1 + 3.0;
        r.zmin = std::max(0.0, z0 * 0.99 - 1.0);
    }
    r.zmax = z1 * 1.01 + 1.0;
    return 0;
}
}  // namespace detail

// The whole step, for one eye: the frame's patches `cur`, last frame's `prev` (nPrev 0 = none), the eye's tangents.
// Bodies by cb2[12]; each current body finds its previous one (same radius, nearest centre, a clear winner); patches
// match by static rows inside the pair (unique, else the one whose predicted position is clearly nearest); the
// delta is the consensus of the matched patches and must be plausible; the volume is the union of the body's boxes.
// The records are the bodies that got a delta, nearest first.
inline void build(const Patch* cur, uint32_t nCur, const Patch* prev, uint32_t nPrev, const EyeInput& eye,
                  BuildResult& out, Scratch& s) {
    out = BuildResult{};
    if (nCur > kMaxPatches) nCur = kMaxPatches;
    if (nPrev > kMaxPatches) nPrev = kMaxPatches;
    out.patches = nCur;
    uint32_t cOver = 0, pOver = 0;
    const uint32_t nb = detail::groupPatches(cur, nCur, s.cg, s.cgOf, &cOver);
    const uint32_t npb = detail::groupPatches(prev, nPrev, s.pg, s.pgOf, &pOver);
    out.bodies = nb;
    out.overflowBodies = cOver ? 1u : 0u;
    for (uint32_t b = 0; b < nb; ++b) {
        BodyResult& r = out.body[b];
        r.radius = s.cg[b].key[3];
        for (int i = 0; i < 3; ++i) r.centre[i] = s.cg[b].key[i];
        r.patches = s.cg[b].count;
        r.nearest = 1e300;
    }
    for (uint32_t i = 0; i < nCur; ++i)
        if (s.cgOf[i] != UINT32_MAX) out.body[s.cgOf[i]].nearest = std::min(out.body[s.cgOf[i]].nearest, distanceOf(cur[i]));
    uint32_t attempted = 0;   // patches of the bodies not skipped
    for (uint32_t b = 0; b < nb; ++b) {
        BodyResult& r = out.body[b];
        auto refuse = [&](uint32_t why) { r.ok = false; r.fallback = why; ++out.fallbacks[why]; };
        // Where the body is on the eye, first: a body behind it or off its pixels needs no delta.
        const int vol = detail::volume(cur, nCur, s.cgOf, b, eye, r);
        if (vol == 2) { refuse(kFbImplausible); continue; }
        if (vol == 1) { r.skip = kSkipBehind; ++out.behind; continue; }
        if (r.box[2] < 0.0 || r.box[3] < 0.0 || r.box[0] > eye.w - 1.0 || r.box[1] > eye.h - 1.0) {
            r.skip = kSkipOffscreen;
            ++out.offscreen;
            continue;
        }
        attempted += r.patches;
        if (nPrev == 0) { refuse(kFbNoPrevFrame); continue; }
        // The previous body: same radius bits, the nearest centre, and clearly the nearest.
        uint32_t best = UINT32_MAX;
        double bestD = 1e300, second = 1e300;
        for (uint32_t k = 0; k < npb; ++k) {
            if (std::memcmp(&s.pg[k].key[3], &s.cg[b].key[3], 4) != 0) continue;
            double a[3], c[3];
            for (int i = 0; i < 3; ++i) { a[i] = s.cg[b].key[i]; c[i] = s.pg[k].key[i]; }
            const double d = dist3(a, c);
            if (d < bestD) { second = bestD; bestD = d; best = k; } else if (d < second) second = d;
        }
        if (best == UINT32_MAX || !(bestD <= len3(r.centre) + double(r.radius)) || (second < 1e299 && second < 2.0 * bestD)) {
            refuse(kFbNoPrevBody);
            continue;
        }
        // Used only to break a tie between prev patches that share their rows: where the centre's own move puts the patch.
        double shift[3];
        for (int i = 0; i < 3; ++i) shift[i] = double(s.pg[best].key[i]) - s.cg[b].key[i];
        uint32_t np = 0;
        std::memset(s.used, 0, sizeof(uint32_t) * nPrev);
        for (uint32_t i = 0; i < nCur; ++i) {
            if (s.cgOf[i] != b) continue;
            uint32_t found = 0, which = UINT32_MAX;
            double d1 = 1e300, d2 = 1e300;
            Placement pc{};
            for (uint32_t j = 0; j < nPrev; ++j) {
                if (s.pgOf[j] != best || prev[j].hash != cur[i].hash || std::memcmp(prev[j].rows, cur[i].rows, 64) != 0) continue;
                if (++found == 1) pc = placement(cur[i]);
                const Placement pp = placement(prev[j]);
                const double guess[3] = {pc.t[0] + shift[0], pc.t[1] + shift[1], pc.t[2] + shift[2]};
                const double d = dist3(guess, pp.t);
                if (d < d1) { d2 = d1; d1 = d; which = j; } else if (d < d2) d2 = d;
            }
            if (found == 0) continue;
            if (found > 1 && !(d1 < 0.25 * double(r.radius) && d2 > 4.0 * d1)) continue;   // ambiguous: left to the others
            s.pairs[np++] = detail::Pair{i, which, distanceOf(cur[i])};
            ++s.used[which];
        }
        // A prev patch claimed twice has two current patches with its rows: neither is trusted.
        uint32_t kept = 0;
        for (uint32_t k = 0; k < np; ++k)
            if (s.used[s.pairs[k].pi] == 1) s.pairs[kept++] = s.pairs[k];
        np = kept;
        out.matched += np;
        r.matched = np;
        if (np == 0) { refuse(kFbNoMatch); continue; }
        std::sort(s.pairs, s.pairs + np, [](const detail::Pair& a, const detail::Pair& c) { return a.dist < c.dist; });
        bool bad = false;
        for (uint32_t k = 0; k < np; ++k) {
            s.seen[k].c = placement(cur[s.pairs[k].ci]);
            s.seen[k].p = placement(prev[s.pairs[k].pi]);
            s.seen[k].dc = distanceOf(cur[s.pairs[k].ci]);
            s.seen[k].dp = distanceOf(prev[s.pairs[k].pi]);
            if (!s.seen[k].c.ok || !s.seen[k].p.ok) bad = true;
        }
        if (bad) { refuse(kFbImplausible); continue; }
        uint32_t winner = 0;
        const uint32_t why = detail::consensus(s, np, r, &winner);
        if (why != kFbCount) { refuse(why); continue; }
        detail::viewFrame(cur[s.pairs[winner].ci].A, prev[s.pairs[winner].pi].A, r);
        r.ok = true;
    }
    out.unmatched = attempted > out.matched ? attempted - out.matched : 0;
    // Records: the bodies with a delta, nearest first.
    uint32_t idx[kMaxGroups], n = 0;
    for (uint32_t b = 0; b < nb; ++b)
        if (out.body[b].ok) idx[n++] = b;
    std::sort(idx, idx + n, [&](uint32_t a, uint32_t c) { return out.body[a].nearest < out.body[c].nearest; });
    if (n > kMaxBodies) { out.overflowBodies += n - kMaxBodies; n = kMaxBodies; }
    out.records = n;
    for (uint32_t k = 0; k < n; ++k) {
        const BodyResult& r = out.body[idx[k]];
        out.order[k] = idx[k];
        float* g = out.gpu[k];
        for (int row = 0; row < 3; ++row) {
            for (int col = 0; col < 3; ++col) g[row * 4 + col] = float(r.M[row * 3 + col]);
            g[row * 4 + 3] = float(r.tv[row]);
        }
        g[12] = float(r.box[0]); g[13] = float(r.box[1]); g[14] = float(r.box[2]); g[15] = float(r.box[3]);
        g[16] = float(r.zmin);   g[17] = float(r.zmax);   g[18] = 1.0f;             g[19] = 0.0f;
        out.maxDisplacement = std::max(out.maxDisplacement, r.displacement);
    }
}

// The shader's arithmetic on one pixel, for the rig and for reading a log against it: a pixel p at depth z (metres)
// inside a record's volume moves to M (d z) + tv in last frame's view, projected by last frame's tangents. false
// when the pixel is outside the volume. mx, my are previous minus current, render pixels, jitter excluded.
inline bool shaderMotion(const float rec[kRecordFloats], const float tanNow[4], const float tanPrev[4], int w, int h,
                         double px, double py, double z, double* mx, double* my) {
    if (!(px >= rec[12] && px <= rec[14] && py >= rec[13] && py <= rec[15] && z >= rec[16] && z <= rec[17])) return false;
    const double dx = tanNow[0] + (px + 0.5) / w * (double(tanNow[1]) - tanNow[0]);
    const double dy = tanNow[3] - (py + 0.5) / h * (double(tanNow[3]) - tanNow[2]);
    const double P[3] = {dx * z, dy * z, -z};
    const double X = rec[0] * P[0] + rec[1] * P[1] + rec[2] * P[2] + rec[3];
    const double Y = rec[4] * P[0] + rec[5] * P[1] + rec[6] * P[2] + rec[7];
    const double Z = rec[8] * P[0] + rec[9] * P[1] + rec[10] * P[2] + rec[11];
    if (!(Z < -1e-6)) return false;
    const double xt = X / -Z, yt = Y / -Z;
    const double ppx = (xt - tanPrev[0]) / (double(tanPrev[1]) - tanPrev[0]) * w - 0.5;
    const double ppy = (double(tanPrev[3]) - yt) / (double(tanPrev[3]) - tanPrev[2]) * h - 0.5;
    *mx = ppx - px;
    *my = ppy - py;
    return true;
}

}  // namespace celestial
}  // namespace edvr
