// F86 (native on-foot VR): the pure parts of the pose carry (a), the weapon UI depth (d, measure only since F87) and the floor probe (g). The stream windows (c) and
// the charge-port prompt snap (f) of F86 were replaced / backed out in F87 (native_on_foot_f87.h). Design: reviews\native-onfoot-f86-review-2026-10-23.md;
// journal: the arc doc, F86 and F87.
//
// Header only, no game or D3D11 state, so native_frame_test and the depth line rig run the very functions the hooks use. Conventions as native_on_foot_f85.h:
// 4x4 float matrices are row-vector affines (world = p R + t, the rows R[0..2] the axes, the translation at 12..14); scene rows are the column-form Rows.
#pragma once

#include "native_on_foot_f85.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace edvr {
namespace nofoot {
namespace f86 {

// ---- row-vector affines in double ----------------------------------------------------------------------------------------------------------
struct Aff {
    double r[3][3] = {};
    double t[3] = {};
};
inline Aff affOf(const float m[16]) noexcept {
    Aff a;
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) a.r[i][j] = static_cast<double>(m[4 * i + j]);
        a.t[i] = static_cast<double>(m[12 + i]);
    }
    return a;
}
inline void affPoint(const Aff& a, const double p[3], double out[3]) noexcept {
    double o[3];
    for (int j = 0; j < 3; ++j) o[j] = p[0] * a.r[0][j] + p[1] * a.r[1][j] + p[2] * a.r[2][j] + a.t[j];
    std::memcpy(out, o, sizeof(o));
}
inline void affDir(const Aff& a, const double v[3], double out[3]) noexcept {
    double o[3];
    for (int j = 0; j < 3; ++j) o[j] = v[0] * a.r[0][j] + v[1] * a.r[1][j] + v[2] * a.r[2][j];
    std::memcpy(out, o, sizeof(o));
}
inline bool affInv(const Aff& a, Aff* o) noexcept {
    if (!f85::inv3(a.r, o->r)) return false;
    for (int j = 0; j < 3; ++j) o->t[j] = -(a.t[0] * o->r[0][j] + a.t[1] * o->r[1][j] + a.t[2] * o->r[2][j]);
    return true;
}

// ---- A: the pose carry ---------------------------------------------------------------------------------------------------------------------
// The submit packs M3P_i, the Animate hook solved against G_{i-1}: frame i draws M3P_i with the pose of Animate i-1. The body is drawn at
//     a . R(M3P_i) + (the record's translation)      (a: a point of the pose, in the 3P model)
// and the pose put that point where the FP root frame F_{i-1} of the pose's frame had it. The carry moves every record by the difference between where F_i
// has the point now and where it is drawn: want = (a . M3P_p + d_p) . inv(F_p) . F_i  (d_p: the body shift the pose carried; the drawn body is shifted by d).
struct PoseIn {
    bool have = false;
    uint32_t p1 = 0;   // the game frame of G (the pair the Animate hook solved against)
    uint64_t qpc = 0;
    float m3p[16] = {}, froot[16] = {}, d[3] = {};
    bool wrote[2] = {};        // the hand was written by the arm IK
    float wrist[2][3] = {};    // the written hand joint, 3P model coordinates
    bool eyeOk = false;
    float eye[3] = {};         // the pin's eye, 3P model coordinates
};
enum CarryWhy : int { kCarried = 0, kCarryNoPose, kCarryOld, kCarryHitch, kCarryScope, kCarryWhyCount };
inline const char* carryWhyText(int why) noexcept {
    static const char* const n[kCarryWhyCount] = {"carried", "no pose", "over 3 frames", "over 1.5 m", "out of scope"};
    return n[why < 0 || why >= kCarryWhyCount ? 0 : why];
}
constexpr uint32_t kCarryMaxAge = 3;
constexpr double kCarryMaxM = 1.5;   // F87: the take-off and landing deltas of 83-94 cm matched the Animate targets (F86 flight), so they are carried
struct CarryFrame {
    uint32_t p1 = 0;
    float m3p[16] = {}, froot[16] = {};
    double rec[3] = {};   // the record's translation as the renderer will read it (after the body shift)
};
struct CarryResult {
    int why = kCarryNoPose;
    uint32_t age = 0;
    bool usedEye = false;
    double delta[3] = {};
    bool haveWrist[2] = {};
    double errBeforeCm[2] = {-1.0, -1.0}, errAfterCm[2] = {-1.0, -1.0};
    double drawnBefore[2][3] = {}, drawnAfter[2][3] = {};   // the wrists in world as drawn without and with the carry
};
inline CarryResult poseCarry(const PoseIn& pose, const CarryFrame& fr, bool inScope) noexcept {
    CarryResult r;
    if (!inScope) {
        r.why = kCarryScope;
        return r;
    }
    if (!pose.have) {
        r.why = kCarryNoPose;
        return r;
    }
    r.age = fr.p1 - pose.p1;   // unsigned: a pose from a later frame is a huge age
    if (r.age > kCarryMaxAge) {
        r.why = kCarryOld;
        return r;
    }
    const Aff Mp = affOf(pose.m3p), Fp = affOf(pose.froot), Fi = affOf(fr.froot), Mi = affOf(fr.m3p);
    Aff FpInv;
    if (!affInv(Fp, &FpInv)) {
        r.why = kCarryNoPose;
        return r;
    }
    const double dp[3] = {pose.d[0], pose.d[1], pose.d[2]};
    const auto want = [&](const double p[3], double out[3]) {
        double w[3], f[3];
        affPoint(Mp, p, w);
        for (int j = 0; j < 3; ++j) w[j] += dp[j];
        affPoint(FpInv, w, f);
        affPoint(Fi, f, out);
    };
    const auto drawn = [&](const double p[3], double out[3]) {
        double v[3];
        affDir(Mi, p, v);
        for (int j = 0; j < 3; ++j) out[j] = v[j] + fr.rec[j];
    };
    double a[3] = {};
    if (pose.wrote[0] && pose.wrote[1]) {
        for (int j = 0; j < 3; ++j) a[j] = 0.5 * (static_cast<double>(pose.wrist[0][j]) + static_cast<double>(pose.wrist[1][j]));
    } else if (pose.eyeOk) {
        for (int j = 0; j < 3; ++j) a[j] = static_cast<double>(pose.eye[j]);
        r.usedEye = true;
    } else {
        r.why = kCarryNoPose;
        return r;
    }
    double wa[3], da[3];
    want(a, wa);
    drawn(a, da);
    for (int j = 0; j < 3; ++j) r.delta[j] = wa[j] - da[j];
    const bool finite = std::isfinite(r.delta[0]) && std::isfinite(r.delta[1]) && std::isfinite(r.delta[2]);
    const double mag = f85::len3(r.delta);
    const bool hitch = !finite || mag > kCarryMaxM;
    for (int s = 0; s < 2; ++s) {
        if (!pose.wrote[s]) continue;
        const double w[3] = {pose.wrist[s][0], pose.wrist[s][1], pose.wrist[s][2]};
        double ww[3], dd[3];
        want(w, ww);
        drawn(w, dd);
        r.haveWrist[s] = true;
        for (int j = 0; j < 3; ++j) {
            r.drawnBefore[s][j] = dd[j];
            r.drawnAfter[s][j] = dd[j] + (hitch ? 0.0 : r.delta[j]);
        }
        const double eb[3] = {ww[0] - dd[0], ww[1] - dd[1], ww[2] - dd[2]};
        const double ea[3] = {ww[0] - r.drawnAfter[s][0], ww[1] - r.drawnAfter[s][1], ww[2] - r.drawnAfter[s][2]};
        r.errBeforeCm[s] = f85::len3(eb) * 100.0;
        r.errAfterCm[s] = f85::len3(ea) * 100.0;
    }
    r.why = hitch ? kCarryHitch : kCarried;
    if (hitch) std::memset(r.delta, 0, sizeof(r.delta));
    return r;
}
// The frame-to-frame step of a matrix in the previous frame's axes (the right, up and forward rows) and the turn between the two (degrees).
struct StepGeom {
    bool ok = false;
    double step[3] = {};
    double turnDeg = 0.0;
};
inline StepGeom stepGeom(const float prev[16], const float cur[16]) noexcept {
    StepGeom g;
    const double d[3] = {static_cast<double>(cur[12]) - prev[12], static_cast<double>(cur[13]) - prev[13], static_cast<double>(cur[14]) - prev[14]};
    for (int a = 0; a < 3; ++a) g.step[a] = d[0] * prev[4 * a] + d[1] * prev[4 * a + 1] + d[2] * prev[4 * a + 2];
    double tr = 0.0;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) tr += static_cast<double>(prev[4 * i + j]) * static_cast<double>(cur[4 * i + j]);
    double c = (tr - 1.0) * 0.5;
    c = c > 1.0 ? 1.0 : c < -1.0 ? -1.0 : c;
    g.turnDeg = std::acos(c) * f85::kDeg;
    g.ok = std::isfinite(g.step[0]) && std::isfinite(g.step[1]) && std::isfinite(g.step[2]) && std::isfinite(g.turnDeg);
    return g;
}
// The record's previous copy (+0x124), compared with the record's own position at the submit's start, before the body shift and the carry touched either (F87: the
// flight's dump 003 showed the game writes both from the same value, so the copy equals the position "with pos"; anything else is "other" and is left alone).
// The previous copy moves by the carry whenever it was with the position, so position - previous stays the game's own motion.
enum PrevCase : uint8_t { kPrevNone = 0, kPrevWithPos, kPrevOther };
inline const char* prevCaseText(int c) noexcept {
    static const char* const n[3] = {"none", "with pos", "other"};
    return n[c < 0 || c > 2 ? 0 : c];
}
inline PrevCase classifyPrev(const float prevRaw[3], const float posRaw[3]) noexcept {
    return std::memcmp(prevRaw, posRaw, 12) == 0 ? kPrevWithPos : kPrevOther;
}
// The value to write into the previous copy: the carry added when it was with the position, else the copy as it is.
inline void prevToWrite(PrevCase c, const float prevNow[3], const double delta[3], float out[3]) noexcept {
    for (int i = 0; i < 3; ++i) out[i] = c == kPrevWithPos ? static_cast<float>(static_cast<double>(prevNow[i]) + delta[i]) : prevNow[i];
}
// One frame's verdict over the records examined: unanimous "with pos", else "other"; none examined: none.
struct PrevTally {
    uint32_t n[3] = {};
    void add(PrevCase c) noexcept { ++n[c]; }
    PrevCase verdict() const noexcept {
        const uint32_t total = n[kPrevWithPos] + n[kPrevOther];
        if (!total) return kPrevNone;
        return n[kPrevWithPos] == total ? kPrevWithPos : kPrevOther;
    }
};
// The second's figures joined from the carry ring by the frame thread: the wrist error by speed bin, and the carry's size.
struct CarrySecond {
    f85::StepAcc before[f85::kStepBins], after[f85::kStepBins], carry;
};

// ---- D/G: depth lines --------------------------------------------------------------------------------------------------------------------
// A ranged march of the line O + d u through an eye's scene depth: the procedure of the depth shader's `main` with a per-line range (it is
// nofoot::f85::marchCpu with the range a parameter), and the hit pixel and its four neighbours' view depths the shader returns beside it.
enum LineStatus : uint32_t { kLineMiss = 0, kLineHit = 1, kLineNear = 2, kLineOffscreen = 3, kLineUnused = 4 };
struct LineOut {
    uint32_t status = kLineUnused;
    double d = 0.0;
    double px = 0.0, py = 0.0;   // the hit pixel (integer coordinates, as floats)
    double nb[4] = {};           // view depth at (x - 1, y), (x + 1, y), (x, y - 1), (x, y + 1); 1e30 off the texture or at the far plane
    bool pixOk = false;
};
inline bool lineSample(const f85::MarchView& v, const double O[3], const double u[3], double d, double* wPoint, double* wSurface, uint32_t* ix, uint32_t* iy) noexcept {
    const double p[3] = {O[0] + d * u[0] - v.ow[0], O[1] + d * u[1] - v.ow[1], O[2] + d * u[2] - v.ow[2]};
    double c[4];
    for (int j = 0; j < 4; ++j) c[j] = p[0] * v.rows[j] + p[1] * v.rows[4 + j] + p[2] * v.rows[8 + j] + v.rows[12 + j];
    if (!(c[3] > 1e-6)) return false;
    const double nx = c[0] / c[3], ny = c[1] / c[3];
    const double px = (nx * 0.5 + 0.5) * static_cast<double>(v.width), py = (0.5 - ny * 0.5) * static_cast<double>(v.height);
    if (!(px >= 0.0 && py >= 0.0 && px < static_cast<double>(v.width) && py < static_cast<double>(v.height))) return false;
    *ix = static_cast<uint32_t>(px);
    *iy = static_cast<uint32_t>(py);
    *wPoint = c[3];
    *wSurface = f85::wOfDepth(static_cast<double>(v.depth[static_cast<size_t>(*iy) * v.width + *ix]), v.A, v.B);
    return true;
}
inline double lineWAt(const f85::MarchView& v, int64_t x, int64_t y) noexcept {
    x = x < 0 ? 0 : x >= static_cast<int64_t>(v.width) ? static_cast<int64_t>(v.width) - 1 : x;
    y = y < 0 ? 0 : y >= static_cast<int64_t>(v.height) ? static_cast<int64_t>(v.height) - 1 : y;
    return f85::wOfDepth(static_cast<double>(v.depth[static_cast<size_t>(y) * v.width + static_cast<size_t>(x)]), v.A, v.B);
}
inline LineOut marchRangeCpu(const f85::MarchView& v, const double O[3], const double u[3], double dmin, double dmax) noexcept {
    LineOut o;
    if (!(dmax > dmin) || !(dmin > 0.0)) return o;
    const double ratio = std::pow(dmax / dmin, 1.0 / (f85::kMarchSteps - 1));
    double prev = dmin;
    bool off = false;
    o.status = kLineMiss;
    o.d = dmax;
    for (int i = 0; i < f85::kMarchSteps; ++i) {
        const double d = dmin * std::pow(ratio, static_cast<double>(i));
        double wp = 0.0, ws = 0.0;
        uint32_t ix = 0, iy = 0;
        if (!lineSample(v, O, u, d, &wp, &ws, &ix, &iy)) {
            off = true;
            prev = d;
            continue;
        }
        if (wp >= ws) {
            if (i == 0) {
                o.status = kLineNear;
                o.d = dmin;
                return o;
            }
            double lo = prev, hi = d;
            for (int b = 0; b < f85::kMarchBisect; ++b) {
                const double mid = 0.5 * (lo + hi);
                double wm = 0.0, sm = 0.0;
                uint32_t mx = 0, my = 0;
                if (lineSample(v, O, u, mid, &wm, &sm, &mx, &my) && wm >= sm) hi = mid;
                else lo = mid;
            }
            o.d = 0.5 * (lo + hi);
            o.status = kLineHit;
            double wh = 0.0, sh = 0.0;
            uint32_t hx = 0, hy = 0;
            if (lineSample(v, O, u, o.d, &wh, &sh, &hx, &hy)) {
                o.pixOk = true;
                o.px = static_cast<double>(hx);
                o.py = static_cast<double>(hy);
                o.nb[0] = lineWAt(v, static_cast<int64_t>(hx) - 1, hy);
                o.nb[1] = lineWAt(v, static_cast<int64_t>(hx) + 1, hy);
                o.nb[2] = lineWAt(v, hx, static_cast<int64_t>(hy) - 1);
                o.nb[3] = lineWAt(v, hx, static_cast<int64_t>(hy) + 1);
            }
            return o;
        }
        prev = d;
    }
    if (off) o.status = kLineOffscreen;
    return o;
}
// The point of the scene rows' camera space that a pixel at view depth w stands for (relative to the rows' origin).
inline bool unprojectPixel(const f85::Rows& W, uint32_t width, uint32_t height, double px, double py, double w, double out[3]) noexcept {
    if (!(w > 1e-9) || w > 1e29 || !width || !height) return false;
    const double nx = (px + 0.5) / static_cast<double>(width) * 2.0 - 1.0, ny = 1.0 - (py + 0.5) / static_cast<double>(height) * 2.0;
    const double c[3] = {nx * w - W.r[0][3], ny * w - W.r[1][3], w - W.r[2][3]};
    double W3[3][3], Wi[3][3];
    for (int r = 0; r < 3; ++r)
        for (int k = 0; k < 3; ++k) W3[r][k] = W.r[r][k];
    if (!f85::inv3(W3, Wi)) return false;
    f85::mulVec3(Wi, c, out);
    return std::isfinite(out[0]) && std::isfinite(out[1]) && std::isfinite(out[2]);
}
// The surface normal at a line's hit, facing the camera, from the four neighbours' view depths; cosView = n . (-u).
struct SurfaceNormal {
    bool ok = false;
    double n[3] = {}, cosView = 0.0;
};
constexpr double kNeighbourRel = 0.08;  // a neighbour's depth within 8% of the hit's: the same surface
inline SurfaceNormal surfaceNormal(const f85::Rows& W, uint32_t width, uint32_t height, const LineOut& l, double wHit, const double u[3]) noexcept {
    SurfaceNormal s;
    if (l.status != kLineHit || !l.pixOk || !(wHit > 1e-9)) return s;
    for (int k = 0; k < 4; ++k)
        if (!(l.nb[k] > 1e-9) || l.nb[k] > 1e29 || std::fabs(l.nb[k] - wHit) > kNeighbourRel * wHit) return s;
    double pl[3], pr[3], pu[3], pd[3];
    if (!unprojectPixel(W, width, height, l.px - 1.0, l.py, l.nb[0], pl) || !unprojectPixel(W, width, height, l.px + 1.0, l.py, l.nb[1], pr) ||
        !unprojectPixel(W, width, height, l.px, l.py - 1.0, l.nb[2], pu) || !unprojectPixel(W, width, height, l.px, l.py + 1.0, l.nb[3], pd))
        return s;
    const double a[3] = {pr[0] - pl[0], pr[1] - pl[1], pr[2] - pl[2]}, b[3] = {pd[0] - pu[0], pd[1] - pu[1], pd[2] - pu[2]};
    double n[3] = {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
    const double len = f85::len3(n);
    if (!(len > 1e-12)) return s;
    for (double& x : n) x /= len;
    double c = -(n[0] * u[0] + n[1] * u[1] + n[2] * u[2]);
    if (c < 0.0) {
        for (double& x : n) x = -x;
        c = -c;
    }
    std::memcpy(s.n, n, sizeof(n));
    s.cosView = c;
    s.ok = true;
    return s;
}
inline double normalOffRayDeg(const SurfaceNormal& s) noexcept {
    double c = s.cosView > 1.0 ? 1.0 : s.cosView < -1.0 ? -1.0 : s.cosView;
    return std::acos(c) * f85::kDeg;
}

// D: the weapon UI panel. s scales the panel about the aim camera's origin so that it sits kPanelStandoffM in front of the surface under it.
constexpr double kPanelMinM = 0.12, kPanelMaxM = 0.8, kPanelStandoffM = 0.045, kPanelScaleMin = 0.5, kPanelScaleMax = 2.0;
inline double panelScale(double Ds, double Dpanel) noexcept {
    if (!(Dpanel > 1e-6) || !(Ds > 0.0)) return 1.0;
    const double s = (Ds - kPanelStandoffM) / Dpanel;
    return s < kPanelScaleMin ? kPanelScaleMin : s > kPanelScaleMax ? kPanelScaleMax : s;
}
// U (the getter camera folded into the composite frame, P - o_world = U3 v + u4) scaled about O: P' = O + s (P - O). Orel = O - o_world.
inline void scaleUFrameAbout(f85::UFrame& uf, const double Orel[3], double s) noexcept {
    for (int a = 0; a < 3; ++a) {
        for (int b = 0; b < 3; ++b) uf.U3[a][b] *= s;
        uf.u4[a] = s * uf.u4[a] + (1.0 - s) * Orel[a];
    }
}
// The same on the correction M = K^-1 U: the pivot O is the aim camera's origin, which K maps from the hud frame's v = -o_hud (K3 (-o_hud) + k4 = O - o_world), so
//     M3' = s M3,  m4' = s m4 - (1 - s) o_hud.
// The rotation figures of the correction (angle, yaw, pitch) stay those of K -> U.
inline void scaleCorrection(f85::Corr& c, double s, const double oHud[3]) noexcept {
    for (int a = 0; a < 3; ++a) {
        for (int b = 0; b < 3; ++b) c.M3[a][b] *= s;
        c.m4[a] = s * c.m4[a] - (1.0 - s) * oHud[a];
    }
}
// Eye 0's origin from its scene rows (column form): the point where the three clip rows x, y and w vanish, relative to the rows' origin.
inline bool cameraOriginOfRows(const f85::Rows& W, double out[3]) noexcept {
    double W3[3][3], Wi[3][3];
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) W3[r][c] = W.r[r][c];
    if (!f85::inv3(W3, Wi)) return false;
    const double t[3] = {-W.r[0][3], -W.r[1][3], -W.r[2][3]};
    f85::mulVec3(Wi, t, out);
    return std::isfinite(out[0]) && std::isfinite(out[1]) && std::isfinite(out[2]);
}
// The nearest hit of a part's rays (the centre and the four corners) as the target of its smoother; false when none hit.
inline bool nearestHit(const LineOut* lines, int n, double* d) noexcept {
    bool any = false;
    double best = 0.0;
    for (int i = 0; i < n; ++i)
        if (lines[i].status == kLineHit && (!any || lines[i].d < best)) {
            best = lines[i].d;
            any = true;
        }
    if (any) *d = best;
    return any;
}
// F85c's smoother (a critically damped step in 1/D, 40 ms, the closed form of the constant-target step) with its clamp a parameter.
struct RangeSmoother {
    double lo = 0.05, hi = 5.0;
    bool have = false;
    double x = 0.0, v = 0.0;
    static constexpr double kSmoothTimeS = 0.040;
    double clampD(double d) const noexcept { return d < lo ? lo : d > hi ? hi : d; }
    void reset(double d) noexcept {
        x = 1.0 / clampD(d);
        v = 0.0;
        have = true;
    }
    double step(double targetD, double dt) noexcept {
        const double xt = 1.0 / clampD(targetD);
        if (!have) {
            reset(targetD);
            return 1.0 / x;
        }
        if (!(dt > 0.0)) return 1.0 / x;
        if (dt > 0.25) dt = 0.25;
        const double w = 2.0 / kSmoothTimeS;
        const double e = x - xt, c = v + w * e, ex = std::exp(-w * dt);
        x = xt + (e + c * dt) * ex;
        v = (v - w * c * dt) * ex;
        return 1.0 / x;
    }
    double depth() const noexcept { return have ? 1.0 / x : 0.0; }
};

// G: the floor probe. Two rays from eye 0 toward the 3P model's floor line ahead of its origin, (-0.3, 0, 0.7) and (+0.3, 0, 0.7) in the model axes (right, up,
// forward: the rows of M3P), in world. A hit counts only 0.4..1.2 m from the 3P origin T horizontally (perpendicular to up, a unit vector); its height is along up.
inline void floorProbePoints(const float m3p[16], double pts[2][3]) noexcept {
    for (int s = 0; s < 2; ++s)
        for (int i = 0; i < 3; ++i) pts[s][i] = (s == 0 ? -0.3 : 0.3) * static_cast<double>(m3p[i]) + 0.7 * static_cast<double>(m3p[8 + i]) + static_cast<double>(m3p[12 + i]);
}
constexpr double kFloorNearM = 0.4, kFloorFarM = 1.2;
inline bool floorAccept(const double H[3], const double T[3], const double up[3], double* heightM) noexcept {
    const double d[3] = {H[0] - T[0], H[1] - T[1], H[2] - T[2]};
    const double h = d[0] * up[0] + d[1] * up[1] + d[2] * up[2];
    const double q[3] = {d[0] - h * up[0], d[1] - h * up[1], d[2] - h * up[2]};
    const double horiz = f85::len3(q);
    if (!(horiz >= kFloorNearM && horiz <= kFloorFarM) || !std::isfinite(h)) return false;
    *heightM = h;
    return true;
}

}  // namespace f86
}  // namespace nofoot
}  // namespace edvr
