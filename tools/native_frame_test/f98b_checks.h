// F98b checks (native on-foot VR), run98b below: the interaction label placed in the world, upright and facing the neck's pivot (native_on_foot_f98b.h).
// Header only, run by native_frame_test.
//
// The rig: a head turning about the neck's pivot (yaw, then pitch, then roll; the world's up +y, the head looking along -z at rest), the eyes 3.2 cm either side,
// 10 cm over and 9 cm ahead of the pivot, optionally canted 10 deg outward about the head's up. Each eye's scene rows project world points (o_w the world's
// origin); the head's seated pose is the head's rotation (the tracking space's up is the world's). The game's camera (the HUD frame) is the left eye's own,
// origin at it; the label's record is laid out there from its anchor by the designed offset (key 1's, scale 0.3821 z). A label 0.6-1.6 m away, as at a panel.
#pragma once

#include "../../src/d3d11/draw_record_tag.h"
#include "../../src/d3d11/native_on_foot_f98b.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace f98btest {

namespace dt = edvr::drawtag;
namespace f97b = edvr::nofoot::f97b;
namespace f98b = edvr::nofoot::f98b;

constexpr double kPi98 = 3.14159265358979323846;
constexpr double kFx = 0.90326, kFy = 0.83612, kCx = -0.12, kCy = 0.04, kK = 0.3821, kOffX = -0.4791, kOffY = -0.3320;

struct M3 {
    double m[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
};
inline M3 mul(const M3& a, const M3& b) {
    M3 r;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) r.m[i][j] = a.m[i][0] * b.m[0][j] + a.m[i][1] * b.m[1][j] + a.m[i][2] * b.m[2][j];
    return r;
}
inline M3 rotY(double deg) {
    const double s = std::sin(deg * kPi98 / 180.0), c = std::cos(deg * kPi98 / 180.0);
    M3 r;
    r.m[0][0] = c, r.m[0][2] = s, r.m[2][0] = -s, r.m[2][2] = c;
    return r;
}
inline M3 rotX(double deg) {
    const double s = std::sin(deg * kPi98 / 180.0), c = std::cos(deg * kPi98 / 180.0);
    M3 r;
    r.m[1][1] = c, r.m[1][2] = -s, r.m[2][1] = s, r.m[2][2] = c;
    return r;
}
inline M3 rotZ(double deg) {
    const double s = std::sin(deg * kPi98 / 180.0), c = std::cos(deg * kPi98 / 180.0);
    M3 r;
    r.m[0][0] = c, r.m[0][1] = -s, r.m[1][0] = s, r.m[1][1] = c;
    return r;
}
inline void apply(const M3& r, const double v[3], double out[3]) {
    for (int i = 0; i < 3; ++i) out[i] = r.m[i][0] * v[0] + r.m[i][1] * v[1] + r.m[i][2] * v[2];
}

// One eye: its scene rows (world points less o_w = 0), its position and axes.
struct Eye {
    float rows[16] = {};
    double pos[3] = {}, x[3] = {}, y[3] = {}, f[3] = {};
};
inline Eye makeEye(const M3& head, const double pivot[3], double side, double cantDeg) {
    Eye e;
    const double off[3] = {0.032 * side, 0.10, -0.09};
    double o[3];
    apply(head, off, o);
    for (int k = 0; k < 3; ++k) e.pos[k] = pivot[k] + o[k];
    const M3 R = mul(head, rotY(-cantDeg * side));   // canted outward about the head's up
    const double ex[3] = {1, 0, 0}, ey[3] = {0, 1, 0}, eb[3] = {0, 0, 1};
    double b[3];
    apply(R, ex, e.x);
    apply(R, ey, e.y);
    apply(R, eb, b);
    for (int k = 0; k < 3; ++k) e.f[k] = -b[k];
    for (int k = 0; k < 3; ++k) {
        const double cxv = kFx * e.x[k] + kCx * e.f[k], cyv = kFy * e.y[k] + kCy * e.f[k];
        e.rows[k * 4 + 0] = static_cast<float>(cxv);
        e.rows[k * 4 + 1] = static_cast<float>(cyv);
        e.rows[k * 4 + 3] = static_cast<float>(e.f[k]);
    }
    e.rows[10] = 0.5f;
    auto d = [&](int col) { return e.rows[col] * e.pos[0] + e.rows[4 + col] * e.pos[1] + e.rows[8 + col] * e.pos[2]; };
    e.rows[12] = static_cast<float>(-d(0));
    e.rows[13] = static_cast<float>(-d(1));
    e.rows[14] = 0.01f;
    e.rows[15] = static_cast<float>(-d(3));
    return e;
}
inline void seated(const M3& head, float R[12]) {
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) R[4 * i + j] = static_cast<float>(head.m[i][j]);
        R[4 * i + 3] = 0.0f;
    }
}

// The label as the game lays it out in the left eye's frame, and both placements of its record (world): F98b's panel and F97b's eye-facing one.
struct Placed {
    bool ok = false;
    double now[3] = {}, old[3] = {};
    f98b::WorldPanel wp;
    float r4[4] = {}, r5[4] = {}, r7[4] = {};
    dt::MarkerAffine aff;
    double pRec[3] = {}, zGame = 0.0;
};
inline Placed place(const M3& head, const double pivot[3], const double P[3], double cantDeg, const double* upOverride = nullptr) {
    Placed out;
    const Eye L = makeEye(head, pivot, -1.0, cantDeg), Rr = makeEye(head, pivot, 1.0, cantDeg);
    const double o0[3] = {0.0, 0.0, 0.0};
    const f98b::HeadFrame hf = f98b::headFrame(L.rows, o0, Rr.rows, o0);
    float sR[12];
    seated(head, sR);
    double up[3];
    if (!hf.ok || !f98b::worldUp(hf, sR, up)) return out;
    if (upOverride)
        for (int k = 0; k < 3; ++k) up[k] = upOverride[k];
    // the HUD frame: the left eye's camera (x right, y up, z forward), origin at the eye
    const double rel[3] = {P[0] - L.pos[0], P[1] - L.pos[1], P[2] - L.pos[2]};
    const double vH[3] = {f98b::dot3(rel, L.x), f98b::dot3(rel, L.y), f98b::dot3(rel, L.f)};
    const double scale = kK * vH[2];
    const double D[3] = {-scale * kOffX, scale * kOffY, 0.0};
    const double X0[3] = {vH[0] + D[0], vH[1] + D[1], vH[2]};
    out.r4[0] = static_cast<float>(kFx), out.r4[2] = static_cast<float>(kCx);
    out.r5[1] = static_cast<float>(kFy), out.r5[2] = static_cast<float>(kCy);
    out.r7[2] = 1.0f;
    dt::ProjDecomp dM, dW;
    const double axis[2] = {kCx, kCy};
    if (dt::projDecomposeMarker(out.r4, out.r5, out.r7, dM) && dt::projDecomposeWorld(L.rows, dW)) out.aff = dt::markerAffine(dM, dW, axis);
    for (int k = 0; k < 3; ++k) out.pRec[k] = X0[k];
    out.zGame = X0[2];
    // the game's layout place: the anchor through the scene rows plus the record's UI offset from the anchor
    double nP[2], mc0[3], mcV[3];
    const double oz[3] = {0.0, 0.0, 0.0};
    if (!f97b::sceneNdc(L.rows, P, nP)) return out;
    dt::markerClip(out.r4, out.r5, out.r7, X0, oz, mc0);
    dt::markerClip(out.r4, out.r5, out.r7, vH, oz, mcV);
    const double layoutAt[2] = {nP[0] - out.aff.jitter[0] + mc0[0] / mc0[2] - mcV[0] / mcV[2], nP[1] - out.aff.jitter[1] + mc0[1] / mc0[2] - mcV[1] / mcV[2]};
    out.wp = f98b::labelPanelWorld(L.rows, out.r4, out.r5, out.r7, out.aff, out.pRec, out.zGame, D, P, hf.pivot, up, layoutAt);
    if (out.wp.lp.why != 0 || !f98b::eyeFacingRecord(L.rows, P, D, out.zGame, out.old)) return out;
    for (int k = 0; k < 3; ++k) out.now[k] = out.wp.rec[k];
    out.ok = true;
    return out;
}
inline double distMm(const double a[3], const double b[3]) {
    const double d[3] = {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
    return 1000.0 * std::sqrt(f98b::dot3(d, d));
}

template <class Check>
inline void run98b(Check&& check) {
    const double pivot[3] = {0.4, 1.5, 0.2};
    const double P[3] = {0.4 - 0.05, 1.5 + 0.0, 0.2 - 1.0};   // a panel's anchor 1 m ahead, a little left, 10 cm under the eyes
    // ---- the head frame: the eyes' midpoint, the head's axes and the neck's pivot back from both eyes' rows, canted or not ---------------------------------------
    {
        bool ok = true;
        for (double cant : {0.0, 10.0}) {
            const M3 head = mul(rotY(25.0), mul(rotX(-15.0), rotZ(8.0)));
            const Eye L = makeEye(head, pivot, -1.0, cant), R = makeEye(head, pivot, 1.0, cant);
            const double o0[3] = {0.0, 0.0, 0.0};
            const f98b::HeadFrame hf = f98b::headFrame(L.rows, o0, R.rows, o0);
            float sR[12];
            seated(head, sR);
            double up[3];
            const double hy[3] = {head.m[0][1], head.m[1][1], head.m[2][1]};
            ok = ok && hf.ok && distMm(hf.pivot, pivot) < 0.01 && f98b::worldUp(hf, sR, up) && std::fabs(up[1] - 1.0) < 1e-6 &&
                 std::fabs(f98b::dot3(hf.y, hy) - 1.0) < 1e-6;
        }
        check(ok, "F98b: both eyes' rows give back the neck's pivot (within 0.01 mm) and the head's up, and with the seated pose the world's up exactly, canted 10 deg or not, "
                  "the head turned 25, pitched -15 and rolled 8 deg");
    }
    // ---- the world's up filter: the first sample, then slow samples only ----------------------------------------------------------------------------------------
    {
        f98b::UpFilter fu;
        const double a[3] = {0.0, 1.0, 0.0}, b[3] = {1.0, 0.0, 0.0};
        fu.feed(a, 200.0);
        const bool first = fu.have && fu.up[1] == 1.0;
        fu.feed(b, 60.0);   // fast: ignored
        const bool fastIgnored = fu.up[1] == 1.0 && fu.samples == 1;
        fu.feed(b, 5.0);    // slow: averaged in (alpha 0.05)
        const bool slowTaken = fu.samples == 2 && fu.up[0] > 0.04 && fu.up[0] < 0.06;
        check(first && fastIgnored && slowTaken, "F98b: the world's up takes its first sample at once and then only samples taken with the head under 20 deg/s, averaged");
    }
    // ---- the cause and the fix: head rotation about the neck moves F97b's eye-facing label in the world, and moves nothing placed upright facing the pivot ----------
    {
        const M3 rest = rotY(0.0);
        const Placed r0 = place(rest, pivot, P, 0.0);
        double worstNow = 0.0, oldRoll = 0.0, oldYaw = 0.0, oldPitch = 0.0;
        struct Pose {
            double yaw, pitch, roll;
        };
        const Pose poses[] = {{20.0, 0.0, 0.0}, {-30.0, 0.0, 0.0}, {0.0, -20.0, 0.0}, {0.0, 0.0, 10.0}, {15.0, -10.0, -8.0}};
        bool all = r0.ok;
        for (const Pose& p : poses) {
            const Placed r = place(mul(rotY(p.yaw), mul(rotX(p.pitch), rotZ(p.roll))), pivot, P, 10.0);
            all = all && r.ok;
            if (!r.ok) continue;
            worstNow = std::max(worstNow, distMm(r.now, r0.now));
            const double moved = distMm(r.old, r0.old);
            if (p.roll != 0.0 && p.yaw == 0.0) oldRoll = moved;
            if (p.yaw == -30.0) oldYaw = moved;
            if (p.pitch == -20.0 && p.yaw == 0.0) oldPitch = moved;
        }
        check(all && worstNow < 0.5 && oldRoll > 20.0 && oldYaw > 8.0 && oldPitch > 8.0,
              "F98b: turning the head about the neck (20 and -30 deg yaw, -20 pitch, 10 roll, and all three; the eyes canted 10 deg) leaves the label's own point "
              "placed upright facing the pivot within 0.5 mm, while F97b's eye-facing placement moves it over 20 mm in the roll and over 8 mm in the yaw and the pitch "
              "(a label 1 m away)");
    }
    // ---- head translation turns the panel and scales it with the distance; nothing jumps ---------------------------------------------------------------------------
    {
        const M3 rest = rotY(0.0);
        const Placed a = place(rest, pivot, P, 0.0);
        const double pivot2[3] = {pivot[0] + 0.05, pivot[1], pivot[2] - 0.30};   // 5 cm right, 30 cm closer
        const Placed b = place(rest, pivot2, P, 0.0);
        const double offA = distMm(a.now, P), offB = distMm(b.now, P);
        double da[3], db[3];
        for (int k = 0; k < 3; ++k) {
            da[k] = P[k] - (pivot[k] + 0.0);
            db[k] = P[k] - pivot2[k];
        }
        const double sA = std::sqrt(f98b::dot3(da, da)), sB = std::sqrt(f98b::dot3(db, db));
        check(a.ok && b.ok && std::fabs(offB / offA - 0.7) < 0.08 && sB < sA && distMm(a.now, b.now) < 200.0,
              "F98b: stepping 30 cm closer the label's offset from its anchor shrinks with the eyes' distance (the size F97b keeps: the designed angle), no jump");
    }
    // ---- the size: the label's layout through the patched rows is the world panel's, and at rest it is F97b's -----------------------------------------------------
    {
        const Placed r = place(mul(rotY(20.0), rotZ(8.0)), pivot, P, 10.0);
        float q4[4], q5[4];
        for (int i = 0; i < 4; ++i) q4[i] = r.r4[i], q5[i] = r.r5[i];
        double nRec[2];
        {
            double mc[3];
            const double oz[3] = {0.0, 0.0, 0.0};
            dt::markerClip(r.r4, r.r5, r.r7, r.pRec, oz, mc);
            nRec[0] = mc[0] / mc[2];
            nRec[1] = mc[1] / mc[2];
        }
        f97b::affineRowsG(q4, q5, r.r7, r.aff, r.wp.lp.G, r.wp.lp.Trec, nRec);
        double dw = 0.0, dh = 0.0;
        const bool f = r.ok && f97b::sizeFactor(q4, q5, r.r7, r.aff, r.wp.lp.at.J, r.pRec, r.zGame, &dw, &dh);
        check(f && std::fabs(dw - 1.0) < 0.003 && std::fabs(dh - 1.0) < 0.003, "F98b: drawn through the patched rows the label has the world panel's size (within 0.3%)");
    }
    // ---- the guards and the tracker ------------------------------------------------------------------------------------------------------------------------------
    {
        // an anchor straight above the pivot: no upright panel faces it (refused before the rows are read)
        const Eye L = makeEye(rotY(0.0), pivot, -1.0, 0.0);
        const float r4[4] = {static_cast<float>(kFx), 0.0f, static_cast<float>(kCx), 0.0f}, r5[4] = {0.0f, static_cast<float>(kFy), static_cast<float>(kCy), 0.0f},
                    r7[4] = {0.0f, 0.0f, 1.0f, 0.0f};
        dt::MarkerAffine plain;
        const double pRec[3] = {0.1, 0.0, 2.0}, D[3] = {0.3, -0.2, 0.0}, above[3] = {pivot[0], pivot[1] + 2.0, pivot[2]}, upY[3] = {0.0, 1.0, 0.0}, lay[2] = {0.0, 0.0};
        const f98b::WorldPanel wpA = f98b::labelPanelWorld(L.rows, r4, r5, r7, plain, pRec, 2.0, D, above, pivot, upY, lay);
        const bool refusedAbove = wpA.lp.why != 0;
        f98b::Tracker tr;
        f98b::Sec sec;
        const double a0[3] = {0.0, 0.0, -1.0}, a1[3] = {0.0, 0.0005, -1.0}, a2[3] = {0.0, 0.0105, -1.0};
        const double n0[3] = {0.1, 0.0, -1.0}, n1[3] = {0.1, 0.001, -1.0}, o1[3] = {0.12, 0.0, -1.0};
        tr.add(1, 10, a0, n0, n0, true, 50.0, sec);    // first frame of key 1
        tr.add(1, 11, a0, n1, o1, true, 50.0, sec);    // anchor still: a step, 30-90 bin; now 1 mm, F97b's 20 mm
        tr.add(1, 12, a2, n1, o1, true, 50.0, sec);    // the anchor moved 10.5 mm: counted, not a step
        tr.add(1, 14, a2, n1, o1, true, 1.0, sec);     // a gap: no step
        const bool tracked = sec.steps[2] == 1 && std::fabs(sec.nowSum[2] - 1.0) < 1e-6 && std::fabs(sec.meanOld(2) - 20.0) < 1e-6 && sec.anchorMoving == 1 &&
                             sec.steps[0] == 0 && f98b::speedBin(2.9) == 0 && f98b::speedBin(30.0) == 2 && f98b::speedBin(90.0) == 3;
        (void)a1;
        check(refusedAbove && tracked, "F98b: an anchor straight above the head is refused (no upright panel); the step a frame counts only with the anchor still and "
                                       "consecutive frames, by the head's speed, F97b's placement beside it");
    }
}

}  // namespace f98btest
