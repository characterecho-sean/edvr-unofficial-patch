// F98b (native on-foot VR): the interaction label placed in the world, upright and facing the head's position. Journal: the arc doc, F98b.
//
// Header only, no game or D3D11 state, so native_frame_test runs the very functions the hooks use.
//
// F97b drew the label as a panel facing the EYE: its record at the game's designed angles from the anchor along the eye's own right and up axes (made
// perpendicular to the line of sight), at the eye's distance. Pure yaw and pitch leave those axes where they were, but head roll turns them, so the label orbits
// its dot and tilts with the head; and the eye is not where the head turns about (it swings on the neck), so every head turn moves the eye and the label with
// it: a label 0.2 m from its dot at 1 m moves about 2 cm in a 60 deg turn. The dots (a point each) show none of it.
//
// A world-locked label is placed by the head's POSITION only. Its panel stands upright (world up, from the head's seated pose: the tracking space's up is the
// world's for the character) and faces the neck's pivot (a point fixed in the head, 10 cm under and 9 cm behind the eyes' midpoint, which a head turn about
// the neck leaves where it is); its record sits on that panel at the game's designed angles from the anchor, scaled by the pivot's distance less the eyes'
// 9 cm ahead of it (F97b's size: the angle the game designs at the centre of its view, seen from the eyes), and its layout is mapped about the record onto the panel's axes (F97b's G with this panel's
// Jacobian). Head rotation moves nothing; head translation turns the panel and scales it with the distance.
#pragma once

#include "native_on_foot_f97b.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace edvr {
namespace nofoot {
namespace f98b {

using f97b::dot3;
inline void norm3(double v[3]) noexcept {
    const double l = std::sqrt(dot3(v, v));
    if (l > 1e-18) {
        v[0] /= l;
        v[1] /= l;
        v[2] /= l;
    }
}
inline void cross3(const double a[3], const double b[3], double out[3]) noexcept {
    const double x = a[1] * b[2] - a[2] * b[1], y = a[2] * b[0] - a[0] * b[2], z = a[0] * b[1] - a[1] * b[0];
    out[0] = x;
    out[1] = y;
    out[2] = z;
}

// An eye's axes in the world from its scene rows (the projection's decomposition): right, up and forward, unit.
inline bool eyeAxes(const float rows[16], double x[3], double y[3], double f[3]) noexcept {
    const double cx[3] = {rows[0], rows[4], rows[8]}, cy[3] = {rows[1], rows[5], rows[9]}, cw[3] = {rows[3], rows[7], rows[11]};
    const double len = std::sqrt(dot3(cw, cw));
    if (!(len > 1e-12)) return false;
    for (int k = 0; k < 3; ++k) f[k] = cw[k] / len;
    const double ox = dot3(cx, f) / len, oy = dot3(cy, f) / len;
    for (int k = 0; k < 3; ++k) {
        x[k] = cx[k] / len - ox * f[k];
        y[k] = cy[k] / len - oy * f[k];
    }
    norm3(x);
    norm3(y);
    return std::isfinite(x[0]) && std::isfinite(y[0]) && std::isfinite(f[0]);
}

// The head in the world from both eyes' rows (each with its own origin o): the eyes' midpoint, the head's axes (the eyes' averaged: a canted pair turns about
// the head's up, symmetrically), and the neck's pivot (10 cm under and 9 cm behind the midpoint, in the head's axes). Absolute world coordinates.
constexpr double kPivotDownM = 0.10, kPivotBackM = 0.09;
struct HeadFrame {
    bool ok = false;
    double mid[3] = {}, pivot[3] = {}, x[3] = {}, y[3] = {}, back[3] = {};
};
inline HeadFrame headFrame(const float rows0[16], const double o0[3], const float rows1[16], const double o1[3]) noexcept {
    HeadFrame h;
    double e0[3], e1[3], x0[3], y0[3], f0[3], x1[3], y1[3], f1[3];
    if (!f97b::eyeOfRows(rows0, e0) || !f97b::eyeOfRows(rows1, e1) || !eyeAxes(rows0, x0, y0, f0) || !eyeAxes(rows1, x1, y1, f1)) return h;
    for (int k = 0; k < 3; ++k) {
        h.mid[k] = 0.5 * (e0[k] + o0[k] + e1[k] + o1[k]);
        h.back[k] = -(f0[k] + f1[k]);
        h.y[k] = y0[k] + y1[k];
    }
    norm3(h.back);
    // y perpendicular to back, x = y x back (right-handed in the eyes' own sense: its sign is set from the eyes' right below)
    const double yb = dot3(h.y, h.back);
    for (int k = 0; k < 3; ++k) h.y[k] -= yb * h.back[k];
    norm3(h.y);
    cross3(h.y, h.back, h.x);
    norm3(h.x);
    if (dot3(h.x, x0) + dot3(h.x, x1) < 0.0)
        for (int k = 0; k < 3; ++k) h.x[k] = -h.x[k];
    for (int k = 0; k < 3; ++k) h.pivot[k] = h.mid[k] - kPivotDownM * h.y[k] + kPivotBackM * h.back[k];
    h.ok = std::isfinite(h.mid[0]) && std::isfinite(h.x[0]) && std::isfinite(h.y[0]) && std::isfinite(h.back[0]) && std::fabs(dot3(h.x, h.y)) < 0.2;
    return h;
}
// The world's up from the head's seated pose R (row-major 3x4, y up, -z forward): the seated up in the head's coordinates is R's row 1, laid on the head's axes.
inline bool worldUp(const HeadFrame& h, const float R[12], double up[3]) noexcept {
    if (!h.ok) return false;
    const double a = R[4], b = R[5], c = R[6];
    for (int k = 0; k < 3; ++k) up[k] = a * h.x[k] + b * h.y[k] + c * h.back[k];
    const double l = std::sqrt(dot3(up, up));
    if (!(l > 0.5) || !(l < 1.5)) return false;
    norm3(up);
    return true;
}
// The world's up, steady: the first sample, then an average of the samples taken with the head turning under 20 deg/s (a pose a frame from the rows' is then a
// fraction of a degree off). The world's up does not move, so the average loses nothing.
constexpr double kUpSlowDps = 20.0, kUpAlpha = 0.05;
struct UpFilter {
    bool have = false;
    double up[3] = {0.0, 1.0, 0.0};
    uint64_t samples = 0;
    void feed(const double u[3], double headDps) noexcept {
        if (!have) {
            for (int k = 0; k < 3; ++k) up[k] = u[k];
            have = true;
            ++samples;
            return;
        }
        if (!(headDps >= 0.0 && headDps < kUpSlowDps)) return;
        for (int k = 0; k < 3; ++k) up[k] += kUpAlpha * (u[k] - up[k]);
        norm3(up);
        ++samples;
    }
};

// The label's panel in the world: upright, facing the pivot from the anchor, the record on it at the game's designed angles from the anchor scaled by s, the
// pivot's distance less the eyes' 9 cm ahead of it (the eyes' distance when they look at it; the eyes' own would move with every head turn), the layout's map G = Je Jm^-1 / z_game with Je the scene rows' Jacobian along the panel's axes at the record times s (per radian of the layout).
// Positions relative to the eye's origin o_w: anchor and pivot as given. recWorld is the record's world point (relative to o_w).
struct WorldPanel {
    f97b::LabelPanel lp;   // why, G, at (Je at the record), atGame (left as the caller's), Trec
    double rec[3] = {};
    double right[3] = {}, upP[3] = {};
};
inline WorldPanel labelPanelWorld(const float sceneRows[16], const float r4[4], const float r5[4], const float r7[4], const drawtag::MarkerAffine& aff,
                                  const double pRec[3], double zGame, const double D[3], const double anchor[3], const double pivot[3],
                                  const double up[3], const double layoutAt[2]) noexcept {
    WorldPanel w;
    f97b::M2 Jm;
    if (!(zGame > 0.05) || !f97b::markerJacobianHud(r4, r5, r7, pRec, Jm)) return w;
    double v[3] = {anchor[0] - pivot[0], anchor[1] - pivot[1], anchor[2] - pivot[2]};
    const double dp = std::sqrt(dot3(v, v));
    const double s = dp - kPivotBackM;   // the eyes' distance when they look at it: the pivot's less the eyes' depth ahead of it (no head turn changes it)
    if (!(dp > 1e-6) || !(s > 0.05)) return w;
    norm3(v);
    double U[3] = {up[0], up[1], up[2]};
    const double uv = dot3(U, v);
    for (int k = 0; k < 3; ++k) U[k] -= uv * v[k];
    if (!(dot3(U, U) > 1e-6)) return w;   // the anchor straight above or below the head: no upright panel
    norm3(U);
    double R[3];
    cross3(v, U, R);
    norm3(R);
    double ex[3], ey[3], ef[3];
    if (!eyeAxes(sceneRows, ex, ey, ef)) return w;
    if (dot3(R, ex) < 0.0)
        for (int k = 0; k < 3; ++k) R[k] = -R[k];
    const double ta = std::tan(D[0] / zGame), tb = std::tan(D[1] / zGame);
    for (int k = 0; k < 3; ++k) {
        w.rec[k] = anchor[k] + s * (ta * R[k] + tb * U[k]);
        w.right[k] = R[k];
        w.upP[k] = U[k];
    }
    double nr[2];
    if (!f97b::sceneNdc(sceneRows, w.rec, nr)) return w;
    w.lp.Trec[0] = nr[0] - aff.jitter[0];
    w.lp.Trec[1] = nr[1] - aff.jitter[1];
    // Je: the projection's derivative along the panel's axes at the record, per metre, times s
    const double cx[3] = {sceneRows[0], sceneRows[4], sceneRows[8]}, cy[3] = {sceneRows[1], sceneRows[5], sceneRows[9]}, cw[3] = {sceneRows[3], sceneRows[7], sceneRows[11]};
    const double X = dot3(cx, w.rec) + sceneRows[12], Y = dot3(cy, w.rec) + sceneRows[13], Wc = dot3(cw, w.rec) + sceneRows[15];
    if (!(Wc > 1e-9)) return w;
    const double w2 = Wc * Wc;
    f97b::SceneAt at;
    at.J.a = s * (dot3(cx, R) * Wc - X * dot3(cw, R)) / w2;
    at.J.b = s * (dot3(cx, U) * Wc - X * dot3(cw, U)) / w2;
    at.J.c = s * (dot3(cy, R) * Wc - Y * dot3(cw, R)) / w2;
    at.J.d = s * (dot3(cy, U) * Wc - Y * dot3(cw, U)) / w2;
    double eye[3];
    if (f97b::eyeOfRows(sceneRows, eye)) {
        double ur[3] = {w.rec[0] - eye[0], w.rec[1] - eye[1], w.rec[2] - eye[2]};
        at.dist = std::sqrt(dot3(ur, ur));
        norm3(ur);
        at.angleDeg = std::acos(std::min(1.0, std::max(-1.0, dot3(ur, ef)))) * 180.0 / 3.14159265358979323846;
    }
    at.ok = std::isfinite(at.J.a) && std::isfinite(at.J.b) && std::isfinite(at.J.c) && std::isfinite(at.J.d);
    w.lp.at = at;
    if (!at.ok) return w;
    f97b::M2 G;
    if (!f97b::panelMap(at.J, Jm, zGame, G) ||
        !(std::fabs(w.lp.Trec[0] - layoutAt[0]) < drawtag::kPatchGateNdc && std::fabs(w.lp.Trec[1] - layoutAt[1]) < drawtag::kPatchGateNdc)) {
        w.lp.why = 2;
        return w;
    }
    w.lp.G = G;
    w.lp.why = 0;
    return w;
}

// F97b's placement of the record, for the measure (relative to o_w): from the eye, at the designed angles along the eye's axes made perpendicular to the line of sight.
inline bool eyeFacingRecord(const float sceneRows[16], const double anchor[3], const double D[3], double zGame, double out[3]) noexcept {
    double eye[3];
    if (!(zGame > 0.05) || !f97b::eyeOfRows(sceneRows, eye)) return false;
    double u[3] = {anchor[0] - eye[0], anchor[1] - eye[1], anchor[2] - eye[2]};
    const double d = std::sqrt(dot3(u, u));
    if (!(d > 1e-6)) return false;
    norm3(u);
    double R[3], U[3];
    if (!f97b::panelAxes(sceneRows, u, R, U)) return false;
    const double ta = std::tan(D[0] / zGame), tb = std::tan(D[1] / zGame);
    double dir[3] = {u[0] + ta * R[0] + tb * U[0], u[1] + ta * R[1] + tb * U[1], u[2] + ta * R[2] + tb * U[2]};
    norm3(dir);
    for (int k = 0; k < 3; ++k) out[k] = eye[k] + d * dir[k];
    return true;
}

// The label's own world point's step a frame against its anchor, per eye and key (the anchor still: under 0.5 mm), by the head's turning speed: under 3, 3-30,
// 30-90, 90 deg/s and over. Placed as now (the world panel) and as F97b placed it, side by side.
constexpr int kSpeedBins = 4;
inline int speedBin(double dps) noexcept { return dps < 3.0 ? 0 : dps < 30.0 ? 1 : dps < 90.0 ? 2 : 3; }
constexpr double kAnchorStillMm = 0.5;
struct Sec {
    uint64_t placed = 0, refusedNoHead = 0, refusedPanel = 0, steps[kSpeedBins] = {}, oldN[kSpeedBins] = {}, anchorMoving = 0;
    double nowSum[kSpeedBins] = {}, nowMax[kSpeedBins] = {}, oldSum[kSpeedBins] = {}, oldMax[kSpeedBins] = {};
    double upTiltSum = 0.0;   // the world's up against the eyes' up, deg (the head's tilt off level)
    uint64_t upN = 0;
    double mean(const double* v, int b) const noexcept { return steps[b] ? v[b] / static_cast<double>(steps[b]) : 0.0; }
    double meanOld(int b) const noexcept { return oldN[b] ? oldSum[b] / static_cast<double>(oldN[b]) : 0.0; }
};
struct Tracker {
    struct Slot {
        bool used = false;
        uint32_t key = 0, frame = 0;
        double offNow[3] = {}, offOld[3] = {}, anchor[3] = {};
        bool haveOld = false;
    };
    static constexpr int kSlots = 8;
    Slot s[kSlots];
    // recNow, recOld: the record's world point (absolute) placed now and by F97b's rule (haveOld false when it could not be placed); anchor absolute.
    void add(uint32_t key, uint32_t frame, const double anchor[3], const double recNow[3], const double recOld[3], bool haveOld, double headDps, Sec& sec) noexcept {
        double offNow[3], offOld[3] = {};
        for (int k = 0; k < 3; ++k) {
            offNow[k] = recNow[k] - anchor[k];
            if (haveOld) offOld[k] = recOld[k] - anchor[k];
        }
        Slot* t = nullptr;
        for (Slot& q : s)
            if (q.used && q.key == key) t = &q;
        if (!t) {
            t = &s[0];
            for (Slot& q : s) {
                if (!q.used) {
                    t = &q;
                    break;
                }
                if (q.frame < t->frame) t = &q;
            }
            *t = Slot{};
        } else if (t->frame == frame) {
            return;
        } else if (t->frame + 1u == frame && headDps >= 0.0) {
            const double da[3] = {anchor[0] - t->anchor[0], anchor[1] - t->anchor[1], anchor[2] - t->anchor[2]};
            if (1000.0 * std::sqrt(dot3(da, da)) > kAnchorStillMm) {
                ++sec.anchorMoving;
            } else {
                const int b = speedBin(headDps);
                const double dn[3] = {offNow[0] - t->offNow[0], offNow[1] - t->offNow[1], offNow[2] - t->offNow[2]};
                const double mn = 1000.0 * std::sqrt(dot3(dn, dn));
                ++sec.steps[b];
                sec.nowSum[b] += mn;
                sec.nowMax[b] = std::max(sec.nowMax[b], mn);
                if (haveOld && t->haveOld) {
                    const double dd[3] = {offOld[0] - t->offOld[0], offOld[1] - t->offOld[1], offOld[2] - t->offOld[2]};
                    const double mo = 1000.0 * std::sqrt(dot3(dd, dd));
                    ++sec.oldN[b];
                    sec.oldSum[b] += mo;
                    sec.oldMax[b] = std::max(sec.oldMax[b], mo);
                }
            }
        }
        t->used = true;
        t->key = key;
        t->frame = frame;
        for (int k = 0; k < 3; ++k) {
            t->offNow[k] = offNow[k];
            t->offOld[k] = offOld[k];
            t->anchor[k] = anchor[k];
        }
        t->haveOld = haveOld;
    }
};

}  // namespace f98b
}  // namespace nofoot
}  // namespace edvr
