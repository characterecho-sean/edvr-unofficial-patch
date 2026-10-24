// F85 checks (the pin-step trace, the weapon UI pinned to the weapon, the reticle's hit depth), F86 checks (the pose carry, the weapon UI depth, the floor probe,
// run86 below) and F87 checks (the standing-height hold, the previous copy, the stream windows' history and escalation, the scene rows' fallback, the prompt probes'
// pins and box geometry, the hand residual: run87 below) and F88 checks (the weapon eye offset: run88 below): the pure functions of src\d3d11\native_on_foot_f85.h, _f86.h,
// _f87.h and _f88.h on synthetic cameras. Header only so that native_frame_test, the depth line rig and a desk driver run the same lines.
#pragma once

#include "../../src/d3d11/native_on_foot_f85.h"
#include "../../src/d3d11/native_on_foot_core.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#include <intrin.h>
#endif

namespace f85test {

namespace f85 = edvr::nofoot::f85;

struct Mat {
    double m[3][3];
};
inline Mat rotY(double a) {
    const double c = std::cos(a), s = std::sin(a);
    return Mat{{{c, 0, s}, {0, 1, 0}, {-s, 0, c}}};
}
inline Mat rotX(double a) {
    const double c = std::cos(a), s = std::sin(a);
    return Mat{{{1, 0, 0}, {0, c, -s}, {0, s, c}}};
}
inline Mat mul(const Mat& a, const Mat& b) {
    Mat o;
    f85::mul33(a.m, b.m, o.m);
    return o;
}
inline Mat transpose(const Mat& a) {
    Mat o;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) o.m[i][j] = a.m[j][i];
    return o;
}

// An eye's scene rows (b1 270..273 in row-vector form): looks along the columns of `eye` (right, up, forward), o = the rows' world origin. Reversed-Z
// with an infinite far plane (clip z = near, clip w = view depth); the principal point (px, py) carries the TAA jitter.
inline void sceneRows(const Mat& eye, const double ePosRel[3], double fx, double fy, double px, double py, double nearZ, float out[16]) {
    const Mat R = transpose(eye);   // world -> view
    const double Cv[4][3] = {{fx, 0, px}, {0, fy, py}, {0, 0, 0}, {0, 0, 1}};
    for (int col = 0; col < 4; ++col) {
        double coef[3];
        for (int i = 0; i < 3; ++i) coef[i] = Cv[col][0] * R.m[0][i] + Cv[col][1] * R.m[1][i] + Cv[col][2] * R.m[2][i];
        double re[3];
        f85::mulVec3(R.m, ePosRel, re);
        const double k = -(Cv[col][0] * re[0] + Cv[col][1] * re[1] + Cv[col][2] * re[2]) + (col == 2 ? nearZ : 0.0);
        for (int i = 0; i < 3; ++i) out[4 * i + col] = static_cast<float>(coef[i]);
        out[12 + col] = static_cast<float>(k);
    }
}
// B = A . W . K (the composite's rows from the eye's scene rows, the display affine and the camera K: P - ow = K3 v + k4).
inline f85::Rows compositeFrom(const f85::Rows& W, const f85::AffParams& a, double lambda, const double K3[3][3], const double k4[3]) {
    f85::Rows WK;
    for (int k = 0; k < 3; ++k) {
        for (int j = 0; j < 3; ++j) WK.r[k][j] = W.r[k][0] * K3[0][j] + W.r[k][1] * K3[1][j] + W.r[k][2] * K3[2][j];
        WK.r[k][3] = W.r[k][0] * k4[0] + W.r[k][1] * k4[1] + W.r[k][2] * k4[2] + W.r[k][3];
    }
    const double ox = a.offM[0] - a.sigma[0] * a.offW[0], oy = a.offM[1] - a.sigma[1] * a.offW[1];
    f85::Rows B;
    for (int c = 0; c < 4; ++c) {
        B.r[0][c] = lambda * (a.sigma[0] * WK.r[0][c] + ox * WK.r[2][c]);
        B.r[1][c] = lambda * (a.sigma[1] * WK.r[1][c] + oy * WK.r[2][c]);
        B.r[2][c] = lambda * WK.r[2][c];
    }
    return B;
}

template <class Check>
inline void run(Check&& check) {
    namespace nf = edvr::nofoot;
    // ---- the synthetic rig: the aim camera K, two eyes 6.06 cm apart with a 10.75 degree cant, an affine and a jittered projection ----------
    const double kCant = 10.75 * f85::kPi / 180.0, kIpd = 0.0606;
    const Mat Rk = mul(rotY(0.31), rotX(-0.12));
    const double Ok[3] = {123.4, 1.62, -45.6};
    const double right[3] = {Rk.m[0][0], Rk.m[1][0], Rk.m[2][0]};
    double eyePos[2][3], ow[2][3];
    for (int e = 0; e < 2; ++e) {
        const double s = e == 0 ? -0.5 * kIpd : 0.5 * kIpd;
        for (int i = 0; i < 3; ++i) eyePos[e][i] = Ok[i] + s * right[i];
        for (int i = 0; i < 3; ++i) ow[e][i] = eyePos[e][i] + (e == 0 ? 0.0 : 0.002 * (i + 1));   // the rows' origin: near the eye, not on it
    }
    const double oh[2][3] = {{0.4, -0.1, 2.0}, {0.41, -0.1, 2.02}};
    f85::AffParams aff;
    aff.sigma[0] = 1.0094;
    aff.sigma[1] = 1.0081;
    aff.offM[0] = 0.011;
    aff.offM[1] = -0.007;
    aff.offW[0] = 0.0153;
    aff.offW[1] = 0.0031;
    const double lambda = 1.03;
    float sceneF[2][16];
    f85::Rows W[2], B[2];
    double K3[3][3];
    std::memcpy(K3, Rk.m, sizeof(K3));
    double k4[2][3];
    for (int e = 0; e < 2; ++e) {
        const Mat eye = mul(Rk, rotY(e == 0 ? kCant : -kCant));
        const double ePosRel[3] = {eyePos[e][0] - ow[e][0], eyePos[e][1] - ow[e][1], eyePos[e][2] - ow[e][2]};
        sceneRows(eye, ePosRel, 1.38, 1.38, 0.0153 * (e ? 1.0 : 1.0) + 0.002, 0.0031, 0.025, sceneF[e]);
        W[e] = f85::rowsOfWorld(sceneF[e]);
        double a[3];
        f85::mulVec3(K3, oh[e], a);
        for (int i = 0; i < 3; ++i) k4[e][i] = a[i] + Ok[i] - ow[e][i];
        B[e] = compositeFrom(W[e], aff, lambda, K3, k4[e]);
    }
    // the scene rows against a direct evaluation of the row-vector form
    {
        const double q[3] = {0.3, -0.2, 4.0};
        const double wq[3] = {q[0] + ow[0][0], q[1] + ow[0][1], q[2] + ow[0][2]};
        (void)wq;
        double c[4];
        for (int j = 0; j < 4; ++j) c[j] = q[0] * sceneF[0][j] + q[1] * sceneF[0][4 + j] + q[2] * sceneF[0][8 + j] + sceneF[0][12 + j];
        double ndc[2], w = 0.0;
        check(f85::project(W[0], q, ndc, &w) && std::fabs(ndc[0] - c[0] / c[3]) < 1e-12 && std::fabs(ndc[1] - c[1] / c[3]) < 1e-12 && std::fabs(w - c[3]) < 1e-12,
              "F85b: the scene rows kept in column form project like the row-vector b1 rows");
    }

    // ---- F85b 3: K comes back to 1e-5 with a jittered W, both eyes ---------------------------------------------------------------------
    f85::CamK Ke[2];
    for (int e = 0; e < 2; ++e) Ke[e] = f85::solveK(B[e], W[e], aff);
    {
        bool ok = Ke[0].ok && Ke[1].ok;
        double err = 0.0;
        for (int e = 0; e < 2; ++e)
            for (int i = 0; i < 3; ++i) {
                err = std::max(err, std::fabs(Ke[e].k4[i] - k4[e][i]));
                for (int j = 0; j < 3; ++j) err = std::max(err, std::fabs(Ke[e].K3[i][j] - K3[i][j]));
            }
        check(ok && err < 1e-5, "F85b: K is recovered from the draw's rows and the eye's jittered scene rows to 1e-5 (both eyes)");
        check(ok && Ke[0].rigid < 1e-9 && Ke[1].rigid < 1e-9 && std::fabs(Ke[0].lambda - lambda) < 1e-9,
              "F85b: the solved K is a rotation (rigid to 1e-9) and the w rows' scale is found");
        // a display affine that is wrong shows as a K that is not a rotation (the gate that refuses such a draw)
        f85::AffParams bad = aff;
        bad.sigma[0] = 1.06;
        const f85::CamK Kb = f85::solveK(B[0], W[0], bad);
        check(Kb.ok && Kb.rigid > f85::kRigidMax, "F85b: a wrong display affine makes K non-rigid and the draw would be refused (rigid gate)");
    }
    // ---- 2 and 5: the two eyes' estimates agree to 1 mm and 0.05 degrees; a disagreement drops the latch ----------------------------------
    {
        const f85::KAgree g = f85::kAgree(Ke[0], oh[0], ow[0], Ke[1], oh[1], ow[1]);
        check(g.ok && g.distM < 1e-6 && g.angleDeg < 1e-6, "F85b: the two eyes' K agree (origin 1e-6 m, rotation 1e-6 deg) on a consistent rig");
        check(f85::kAgreeWithin(g), "F85b: agreement within 1 mm / 0.05 degrees is accepted");
        f85::CamK off = Ke[1];
        off.k4[1] += 0.002;
        check(!f85::kAgreeWithin(f85::kAgree(Ke[0], oh[0], ow[0], off, oh[1], ow[1])), "F85b: a 2 mm origin disagreement is refused");
        f85::CamK turned = Ke[1];
        const Mat t = rotY(0.1 * f85::kPi / 180.0);
        f85::mul33(Ke[1].K3, t.m, turned.K3);
        check(!f85::kAgreeWithin(f85::kAgree(Ke[0], oh[0], ow[0], turned, oh[1], ow[1])), "F85b: a 0.1 degree rotation disagreement is refused");
        f85::KLatch latch;
        check(!latch.usable(10), "F85b: no latch before both eyes are in");
        latch.put(0, 10, Ke[0], oh[0], ow[0]);
        check(!latch.usable(10), "F85b: one eye's K is not a latch");
        check(latch.put(1, 10, Ke[1], oh[1], ow[1]) && latch.agreed && latch.usable(10) && latch.usable(11) && !latch.usable(12),
              "F85b: both eyes of a frame agreeing latch for that frame and the next, not the one after");
        latch.put(0, 11, off, oh[0], ow[0]);
        check(latch.put(1, 11, Ke[1], oh[1], ow[1]) && !latch.agreed && !latch.usable(11) && latch.disagreed == 1,
              "F85b: a frame whose eyes disagree drops the latch and is counted");
    }
    // ---- 1 and 2: H = 30 degrees yaw, U = K . H; the unpatched anchor is off by H, rows . M land on W_e . (U v) within 0.01 px -------------
    {
        const double H = 30.0 * f85::kPi / 180.0;
        const Mat Ru = mul(Rk, rotY(H));   // U's axes in world: K's, turned by H about K's up (the head's yaw from the aim)
        float U[16] = {};
        for (int a = 0; a < 3; ++a)
            for (int b = 0; b < 3; ++b) U[4 * b + a] = static_cast<float>(Ru.m[a][b]);
        for (int a = 0; a < 3; ++a) U[12 + a] = static_cast<float>(Ok[a]);
        U[15] = 1.0f;
        // double-precision U for the exact comparison (the float copy above is what the game stores; absolute 100 m floats are good to 8 mm)
        const double axis[2] = {0.0, 0.0};
        const double t0[3] = {0.10, -0.12, 0.36};
        const double corners[5][3] = {{0.10, -0.12, 0.36}, {0.12, -0.10, 0.36}, {0.08, -0.10, 0.36}, {0.12, -0.14, 0.36}, {0.08, -0.14, 0.36}};
        double worstPx = 0.0, unpatchedPx = 1e9;
        bool allOk = true;
        double angleDeg = 0.0, yawDeg = 0.0, pitchDeg = 0.0;
        for (int e = 0; e < 2; ++e) {
            // the float U carries a 100 m origin: rebase it to the rig's double origin for the exactness check
            f85::UFrame uf;
            for (int a = 0; a < 3; ++a)
                for (int b = 0; b < 3; ++b) uf.U3[a][b] = Ru.m[a][b];
            double m[3];
            f85::mulVec3(uf.U3, oh[e], m);
            for (int a = 0; a < 3; ++a) uf.u4[a] = m[a] + Ok[a] - ow[e][a];
            const f85::Corr c = f85::makeCorrection(Ke[e], uf);
            allOk = allOk && c.ok;
            angleDeg = c.angleDeg;
            yawDeg = c.yawDeg;
            pitchDeg = c.pitchDeg;
            const f85::Rows Bp = f85::applyCorrection(B[e], c);
            float r4[4], r5[4], r7[4];
            f85::rowsToFloat(Bp, r4, r5, r7);   // as the private b0 stores them (float32)
            const f85::Rows Bf = f85::rowsOfComposite(r4, r5, r7);
            for (int p = 0; p < 5; ++p) {
                const double v[3] = {corners[p][0] - oh[e][0], corners[p][1] - oh[e][1], corners[p][2] - oh[e][2]};
                double shown[2], target[2], shown0[2];
                // the target: the point t placed by U, through the eye's scene rows, unjittered
                double pRel[3];
                f85::mulVec3(uf.U3, corners[p], pRel);
                for (int i = 0; i < 3; ++i) pRel[i] += Ok[i] - ow[e][i];
                allOk = allOk && f85::shownNdc(aff, axis, Bf, v, shown) && f85::targetNdc(aff, axis, W[e], pRel, target) && f85::shownNdc(aff, axis, B[e], v, shown0);
                const double dx = (shown[0] - target[0]) * 2016.0, dy = (shown[1] - target[1]) * 2016.0;
                worstPx = std::max(worstPx, std::sqrt(dx * dx + dy * dy));
                if (p == 0) {
                    const double ux = (shown0[0] - target[0]) * 2016.0, uy = (shown0[1] - target[1]) * 2016.0;
                    unpatchedPx = std::min(unpatchedPx, std::sqrt(ux * ux + uy * uy));
                }
            }
        }
        (void)t0;
        check(allOk && std::fabs(angleDeg - 30.0) < 1e-6 && std::fabs(yawDeg - 30.0) < 1e-6 && std::fabs(pitchDeg) < 1e-6,
              "F85b: the K -> U rotation is the head's 30 degrees of yaw, no pitch");
        check(unpatchedPx > 100.0, "F85b: unpatched, the readout anchor is hundreds of pixels off the weapon (the head's 30 degrees about the eye)");
        check(allOk && worstPx < 0.01, "F85b: after rows . M the anchor and four corners land within 0.01 px of W_e . (U v) in both eyes");
        // the float rows against the scene: the same through the stored float32 U (the game's own copy, 100 m origin)
        (void)U;
    }
    // ---- 2: H = I gives M = I ----------------------------------------------------------------------------------------------------------
    {
        f85::UFrame uf;
        for (int a = 0; a < 3; ++a)
            for (int b = 0; b < 3; ++b) uf.U3[a][b] = Rk.m[a][b];
        double m[3];
        f85::mulVec3(uf.U3, oh[0], m);
        for (int a = 0; a < 3; ++a) uf.u4[a] = m[a] + Ok[a] - ow[0][a];
        const f85::Corr c = f85::makeCorrection(Ke[0], uf);
        double err = 0.0;
        for (int i = 0; i < 3; ++i) {
            err = std::max(err, std::fabs(c.m4[i]));
            for (int j = 0; j < 3; ++j) err = std::max(err, std::fabs(c.M3[i][j] - (i == j ? 1.0 : 0.0)));
        }
        check(c.ok && err < 1e-6 && c.angleDeg < 1e-4, "F85b: H = I gives M = I to 1e-6");
        // and the float32 getter row of a real (100 m) origin gives the identity within the float's 8 mm: the rows are left within 1e-3
        float Uf[16] = {};
        for (int a = 0; a < 3; ++a)
            for (int b = 0; b < 3; ++b) Uf[4 * b + a] = static_cast<float>(Rk.m[a][b]);
        for (int a = 0; a < 3; ++a) Uf[12 + a] = static_cast<float>(Ok[a]);
        const f85::UFrame fu = f85::uFrame(Uf, oh[0], ow[0]);
        const f85::Corr cf = f85::makeCorrection(Ke[0], fu);
        check(cf.ok && cf.angleDeg < 1e-3, "F85b: the getter's float32 row (a 100 m origin) of the same camera gives an identity rotation");
    }
    // ---- 4/5: tagged keys only; the getter join refuses another frame, part or instance ------------------------------------------------------
    {
        uint32_t slot = 99;
        check(f85::isWeaponKey(0x70, &slot) && slot == 0 && f85::isWeaponKey(0x77, &slot) && slot == 7 && f85::isWeaponKey(0xF3, &slot) && slot == 3,
              "F85b: weapon UI keys are 0x70..0x77 (and their second posts with bit 7)");
        const uint32_t others[] = {1, 10, 43, 45, 46, 47, 48, 49, 50, 0x6F, 0x78, 0x8A, 0xFF};
        bool none = true;
        for (const uint32_t k : others) none = none && !f85::isWeaponKey(k);
        check(none, "F85b: the reticle, the waypoints and the interaction icons are not weapon UI keys (a record without a weapon tag is left untouched)");
        check(nf::weaponUiGetterJoinMatches(10, 3, 0xA, 0xB, 0xC, 100, 200, 10, 3, 0xA, 0xB, 0xC, true, true), "F85b: a getter row of the same frame, item, renderer, part and instance joins");
        check(!nf::weaponUiGetterJoinMatches(9, 3, 0xA, 0xB, 0xC, 100, 200, 10, 3, 0xA, 0xB, 0xC, true, true), "F85b: a getter row of another render frame is refused");
        check(!nf::weaponUiGetterJoinMatches(10, 3, 0xA, 0xD, 0xC, 100, 200, 10, 3, 0xA, 0xB, 0xC, true, true), "F85b: a getter row of another part is refused");
        check(!nf::weaponUiGetterJoinMatches(10, 3, 0xA, 0xB, 0xE, 100, 200, 10, 3, 0xA, 0xB, 0xC, true, true), "F85b: a getter row of another instance is refused");
        check(!nf::weaponUiGetterJoinMatches(10, 3, 0xA, 0xB, 0xC, 100, 200, 10, 3, 0xA, 0xB, 0xC, true, false), "F85b: a stale getter row is refused");
        check(!nf::weaponUiGetterJoinMatches(10, 3, 0xA, 0xB, 0xC, 300, 200, 10, 3, 0xA, 0xB, 0xC, true, true), "F85b: a getter row after the submit is refused");
    }

    // ---- F85c: the march ---------------------------------------------------------------------------------------------------------------------
    {
        const uint32_t N = 256;
        std::vector<float> depth(static_cast<size_t>(N) * N);
        const double nearZ = 0.025;
        double A = 0.0, Bz = 0.0;
        check(f85::zwFit(sceneF[0], &A, &Bz) && std::fabs(A) < 1e-9 && std::fabs(Bz - nearZ) < 1e-7, "F85c: the scene rows' z column fits depth = 0 + near / w (reversed-Z, infinite far)");
        for (double w : {0.3, 1.5, 5.0, 40.0}) check(std::fabs(f85::wOfDepth(nearZ / w, A, Bz) - w) < 1e-6 * w, "F85c: the reversed-Z linearization returns the analytic view depth");
        check(f85::wOfDepth(0.0, A, Bz) > 1e29, "F85c: the far plane linearizes to infinity");
        // a standard-Z projection: depth = f/(f-n) - n f/((f-n) w)
        {
            const double n = 0.1, fz = 1000.0;
            float rows[16] = {};
            rows[10] = static_cast<float>(fz / (fz - n));
            rows[11] = 1.0f;
            rows[14] = static_cast<float>(-n * fz / (fz - n));
            rows[0] = 1.0f;
            rows[5] = 1.0f;
            double a2 = 0.0, b2 = 0.0;
            const bool fit = f85::zwFit(rows, &a2, &b2);
            const double w = 3.0, z = fz / (fz - n) * w - n * fz / (fz - n);
            check(fit && std::fabs(f85::wOfDepth(z / w, a2, b2) - w) < 1e-6, "F85c: a standard-Z projection linearizes through the same fit");
        }
        const f85::Rows W0 = f85::rowsOfWorld(sceneF[0]);
        (void)W0;
        f85::MarchView mv;
        mv.rows = sceneF[0];
        for (int i = 0; i < 3; ++i) mv.ow[i] = ow[0][i];
        mv.A = A;
        mv.B = Bz;
        mv.width = mv.height = N;
        mv.depth = depth.data();
        // the aim line: from the left eye (the rows' camera) along K's forward... the eye looks 10.75 degrees off K, so aim along the eye's own axis
        const Mat eye0 = mul(Rk, rotY(kCant));
        const double u0[3] = {eye0.m[0][2], eye0.m[1][2], eye0.m[2][2]};
        const double O0[3] = {eyePos[0][0], eyePos[0][1], eyePos[0][2]};
        const auto fill = [&](double planeW) {   // a plane perpendicular to the eye's axis at view depth planeW (0: sky)
            for (auto& d : depth) d = planeW > 0.0 ? static_cast<float>(nearZ / planeW) : 0.0f;
        };
        fill(1.5);
        f85::MarchOut o = f85::marchCpu(mv, O0, u0);
        check(o.status == f85::kMarchHit && std::fabs(o.d - 1.5) < 0.01, "F85c: a plane at 1.5 m gives D_hit = 1.50 +- 0.01 m");
        fill(0.0);
        o = f85::marchCpu(mv, O0, u0);
        check(o.status == f85::kMarchSky && std::fabs(o.d - 50.0) < 1e-9, "F85c: sky gives 50 m");
        fill(0.3);
        o = f85::marchCpu(mv, O0, u0);
        check(o.status == f85::kMarchNear, "F85c: a surface 0.3 m out is ignored (status near; the caller holds)");
        fill(80.0);
        o = f85::marchCpu(mv, O0, u0);
        check(o.status == f85::kMarchSky, "F85c: a surface past 50 m is the clamp (sky)");
        fill(0.62);
        o = f85::marchCpu(mv, O0, u0);
        check(o.status == f85::kMarchHit && std::fabs(o.d - 0.62) < 0.01, "F85c: a plane at 0.62 m (just past the minimum) is found");
        // a wall at an angle: the depth image of a plane n . P = c seen from the eye, the hit along the aim line is c / (n . u)
        {
            const double nrm[3] = {std::sin(0.35), 0.0, std::cos(0.35)};   // world-ish normal, in the eye's frame below
            const Mat Re = eye0;
            const double cdist = 3.0;   // plane: n . x = cdist, x in the eye's camera frame (right, up, forward)
            for (uint32_t iy = 0; iy < N; ++iy)
                for (uint32_t ix = 0; ix < N; ++ix) {
                    const double nx = ((ix + 0.5) / N) * 2.0 - 1.0, ny = 1.0 - ((iy + 0.5) / N) * 2.0;
                    const double tx = (nx - (0.0153 + 0.002)) / 1.38, ty = (ny - 0.0031) / 1.38;   // view-space ray (tx, ty, 1)
                    const double den = nrm[0] * tx + nrm[1] * ty + nrm[2];
                    const double s = den > 1e-6 ? cdist / den : 1e30;   // view depth along z
                    depth[static_cast<size_t>(iy) * N + ix] = s < 1e29 ? static_cast<float>(nearZ / s) : 0.0f;
                }
            (void)Re;
            o = f85::marchCpu(mv, O0, u0);
            const double want = cdist / nrm[2];   // the aim ray is the eye's z axis: tx = ty = 0 -> den = nrm.z; view depth s = cdist / nrm.z; the line's d = s
            check(o.status == f85::kMarchHit && std::fabs(o.d - want) < 0.01 * want, "F85c: a slanted wall is hit at its analytic distance along the aim line");
        }
        // the march the shader runs equals this one: two other lines (a ray off the axis) see the same plane at 1.5 m / cos
        fill(2.2);
        {
            const double tilt = 0.05;   // 2.9 degrees off the eye's axis, in its right direction
            const double right0[3] = {eye0.m[0][0], eye0.m[1][0], eye0.m[2][0]};
            double u1[3];
            for (int i = 0; i < 3; ++i) u1[i] = u0[i] + tilt * right0[i];
            const double l = f85::len3(u1);
            for (int i = 0; i < 3; ++i) u1[i] /= l;
            o = f85::marchCpu(mv, O0, u1);
            const double want = 2.2 * l;   // view depth 2.2 along the axis; the line is longer by 1 / cos
            check(o.status == f85::kMarchHit && std::fabs(o.d - want) < 0.012 * want, "F85c: an off-axis aim line meets a plane at its view depth / cos");
        }
    }
    // ---- F85c 3: the dot's NDC in the eye at O does not change with D; the other eye's disparity is IPD . f / D -----------------------------------
    {
        // two zero-cant eyes 6.06 cm apart looking along +z (the cant adds a depth-independent offset; the formula is the zero-cant one)
        const Mat I = Mat{{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}};
        float s0[16], s1[16];
        const double fxv = 1.38, e0[3] = {0, 0, 0}, e1[3] = {kIpd, 0, 0};
        sceneRows(I, e0, fxv, fxv, 0.0, 0.0, 0.025, s0);
        sceneRows(I, e1, fxv, fxv, 0.0, 0.0, 0.025, s1);
        const f85::Rows W0 = f85::rowsOfWorld(s0), W1 = f85::rowsOfWorld(s1);
        const double O[3] = {0, 0, 0}, u[3] = {0.12, -0.05, 0.9916};
        const double ul = f85::len3(u);
        const double un[3] = {u[0] / ul, u[1] / ul, u[2] / ul};
        double n5[2], n15[2], m5[2], m15[2];
        const double P5[3] = {5 * un[0], 5 * un[1], 5 * un[2]}, P15[3] = {1.5 * un[0], 1.5 * un[1], 1.5 * un[2]};
        const bool ok = f85::project(W0, P5, n5, nullptr) && f85::project(W0, P15, n15, nullptr) && f85::project(W1, P5, m5, nullptr) && f85::project(W1, P15, m15, nullptr);
        const double dxPx = std::fabs(n5[0] - n15[0]) * 2016.0, dyPx = std::fabs(n5[1] - n15[1]) * 2016.0;
        check(ok && dxPx < 0.01 && dyPx < 0.01, "F85c: the anchor's NDC in the eye at O is unchanged (within 0.01 px) from 5 m to 1.5 m along the aim ray");
        // the other eye: disparity (eye 1 minus eye 0, NDC x) = -IPD fx / view depth... the view depth of P is D un.z
        const double d5 = (m5[0] - n5[0]) * 2016.0 * 0.5, d15 = (m15[0] - n15[0]) * 2016.0 * 0.5;   // in pixels of a 2016-px target (NDC 2 = 2016 px)
        (void)O;
        const double want5 = -kIpd * fxv / (5 * un[2]) * 2016.0 * 0.5, want15 = -kIpd * fxv / (1.5 * un[2]) * 2016.0 * 0.5;
        check(ok && std::fabs(d5 - want5) < 0.01 && std::fabs(d15 - want15) < 0.01, "F85c: the other eye's disparity is IPD . f / D (5 m and 1.5 m)");
        // the line gate
        const double Oa[3] = {1, 2, 3}, ua[3] = {0, 0, 1};
        const double on[3] = {1, 2, 8}, off[3] = {1 + 5 * std::tan(0.3 * f85::kPi / 180.0), 2, 8};
        check(f85::lineOffAngleDeg(Oa, ua, on) < 1e-9 && f85::lineOffAngleDeg(Oa, ua, off) > 0.2 && f85::lineOffAngleDeg(Oa, ua, off) < 0.31,
              "F85c: P on the aim line passes, 0.3 degrees off is refused (bound 0.2)");
        check(f85::lineOffAngleDeg(Oa, ua, Oa) < 0.0, "F85c: P at O has no direction (refused)");
        check(f85::isReticleKey(10) && !f85::isReticleKey(1) && !f85::isReticleKey(43) && !f85::isReticleKey(45) && !f85::isReticleKey(0x70) && !f85::isReticleKey(0x8A),
              "F85c: only key 10 takes the hit depth (no other key changes)");
    }
    // ---- F85c 4: 5 m to 1.5 m reaches 90% within 0.1 s with no overshoot; frame-time independent -------------------------------------------
    {
        f85::InvDepthSmoother s;
        s.reset(5.0);
        double t = 0.0, t90 = -1.0, maxD = 0.0;
        double prev = 5.0;
        bool monotone = true;
        const double x0 = 1.0 / 5.0, x1 = 1.0 / 1.5;
        for (int i = 0; i < 40; ++i) {
            const double d = s.step(1.5, 0.0055);
            t += 0.0055;
            const double prog = (1.0 / d - x0) / (x1 - x0);
            if (t90 < 0.0 && prog >= 0.9) t90 = t;
            monotone = monotone && d <= prev + 1e-12 && d >= 1.5 - 1e-9;
            prev = d;
            maxD = std::max(maxD, d);
        }
        check(t90 > 0.0 && t90 <= 0.1 && monotone, "F85c: 5 m to 1.5 m reaches 90% (in 1/D) within 0.1 s, monotone, no overshoot");
        f85::InvDepthSmoother a, b;
        a.reset(5.0);
        b.reset(5.0);
        for (int i = 0; i < 10; ++i) a.step(1.5, 0.011);
        for (int i = 0; i < 20; ++i) b.step(1.5, 0.0055);
        check(std::fabs(a.depth() - b.depth()) < 1e-9, "F85c: the smoothing does not depend on the frame time (the closed-form step)");
        f85::InvDepthSmoother c;
        c.reset(1.5);
        for (int i = 0; i < 20; ++i) c.step(40.0, 0.011);
        check(c.depth() > 1.5 && c.depth() <= 50.0, "F85c: a far target moves the dot out and the clamp holds (50 m)");
        f85::InvDepthSmoother e;
        check(std::fabs(e.step(2.0, 0.01) - 2.0) < 1e-12 && e.have, "F85c: the first value takes the target at once");
    }

    // ---- F85b: the value path (the record whose position is bit-equal to the published translation) -----------------------------------------------
    {
        const uint32_t records = 40;
        std::vector<uint8_t> pool(static_cast<size_t>(records) * 336u, 0);
        const auto put = [&](uint32_t idx, const float p[3]) { std::memcpy(pool.data() + static_cast<size_t>(idx) * 336u + 16u, p, 12); };
        const float a[3] = {0.0123f, -0.456f, 0.789f}, b[3] = {1.5f, 2.5f, -3.5f}, other[3] = {0.0123f, -0.456f, 0.790f};
        put(7, a);
        put(21, other);
        put(33, b);
        f85::ValueTarget tg[2];
        std::memcpy(tg[0].pos, a, 12);
        tg[0].sentinel = 0xAAAA;
        std::memcpy(tg[1].pos, b, 12);
        tg[1].sentinel = 0xBBBB;
        f85::ValueHit hits[4];
        const uint32_t n = f85::scanRecordsValue(pool.data(), static_cast<uint32_t>(pool.size()), tg, 2, hits, 4, f85::plainLoadForTest);
        check(n == 2 && hits[0].index == 7 && hits[0].target == 0 && hits[1].index == 33 && hits[1].target == 1,
              "F85b: the value scan finds the records whose position is bit-equal (and not the one a ulp away)");
        f85::ValueHit one;
        check(f85::checkRecordValue(pool.data(), static_cast<uint32_t>(pool.size()), 33, tg, 2, one, f85::plainLoadForTest) && one.target == 1 &&
                  !f85::checkRecordValue(pool.data(), static_cast<uint32_t>(pool.size()), 21, tg, 2, one, f85::plainLoadForTest) &&
                  !f85::checkRecordValue(pool.data(), static_cast<uint32_t>(pool.size()), 99, tg, 2, one, f85::plainLoadForTest),
              "F85b: a hinted record is checked alone; one past the pool is refused");
    }
    // ---- F85a: the tracker ---------------------------------------------------------------------------------------------------------------------
    {
        const uint64_t freq = 10000000ull;
        const auto mkFrame = [&](uint32_t p1, double y, double x = 0.0) {
            f85::StepFrame f;
            f.p1 = p1;
            f.qpc = 1000000000ull + static_cast<uint64_t>(p1) * (freq / 90);
            f.m3pT[0] = static_cast<float>(x);
            f.m3pT[1] = static_cast<float>(y);
            f.up[1] = 1.0f;
            f.haveCam = true;
            f.camBody[1] = 1.6f;
            return f;
        };
        // no triggers: a standing run prints nothing and counts its frames
        {
            f85::StepTracker t;
            bool any = false;
            for (uint32_t i = 0; i < 120; ++i) {
                f85::StepFrame f = mkFrame(i, 0.0);
                const f85::StepResult r = t.step(f, freq);
                any = any || r.printCur || r.trig;
            }
            check(!any && t.totalEvents == 0 && t.totalFrames == 120 && t.sec.trigSeen[0] == 0 && t.sec.trigSeen[1] == 0 && t.sec.trigSeen[2] == 0 && t.sec.trigSeen[3] == 0,
                  "F85a: a still run starts no event and counts its frames and no triggers (the 0 events line)");
        }
        // an M3P excursion: +0.9 m along up for 4 frames, then back; one event with 16 earlier frames and 24 later
        {
            f85::StepTracker t;
            int printed = 0, starts = 0, nPre = 0, lastOffset = -99;
            uint32_t evNo = 0;
            for (uint32_t i = 0; i < 100; ++i) {
                const double y = (i >= 40 && i < 44) ? 0.9 : 0.0;
                f85::StepFrame f = mkFrame(i, y);
                const f85::StepResult r = t.step(f, freq);
                if (r.printCur) {
                    ++printed;
                    lastOffset = r.offset;
                }
                if (r.eventStart) {
                    ++starts;
                    nPre = r.nPre;
                    evNo = r.eventNo;
                    check(t.pre(0).p1 == static_cast<uint32_t>(40 - nPre) && t.pre(nPre - 1).p1 == 39u, "F85a: the earlier frames of an event come oldest first and end at the frame before the trigger");
                }
            }
            check(starts == 1 && nPre == 16 && evNo == 1 && printed == 25 && lastOffset == 24 && t.totalEvents == 1 && t.sec.trigSeen[0] >= 2,
                  "F85a: an M3P step of 0.9 m starts one event: 16 frames before, the trigger, 24 after (the return step does not nest)");
        }
        // the pin cap, the camera in the model and take-off / landing
        {
            f85::StepTracker t;
            f85::StepFrame f0 = mkFrame(0, 0.0), f1 = mkFrame(1, 0.0);
            f1.havePin = true;
            f1.vClamp = true;
            t.step(f0, freq);
            const f85::StepResult r = t.step(f1, freq);
            check(r.eventStart && (r.trig & f85::kTrigPinCap), "F85a: a pin cap trip starts an event");
        }
        {
            // F86b: the camera moving in the model is a figure of the line, no longer a trigger (it fired on every walk start); the hand lag takes its place
            f85::StepTracker t;
            f85::StepFrame f0 = mkFrame(0, 0.0), f1 = mkFrame(1, 0.0);
            f1.camBody[2] = 0.03f;
            t.step(f0, freq);
            const f85::StepResult r = t.step(f1, freq);
            check(!r.eventStart && r.trig == 0u && f1.camMoveCm > 2.9f && f1.camMoveCm < 3.1f, "F86b: the camera moving 3 cm in the model in a frame is measured and starts no event");
            f85::StepTracker u;
            f85::StepFrame g0 = mkFrame(0, 0.0), g1 = mkFrame(1, 0.0);
            g1.haveCarry = true;
            g1.carryBeforeCm[0] = 1.5f;
            g1.carryBeforeCm[1] = 1.9f;
            u.step(g0, freq);
            const f85::StepResult q = u.step(g1, freq);
            check(!q.eventStart && !(q.trig & f85::kTrigHandLag), "F86b: a hand error of 1.9 cm before the carry is under the hand-lag trigger (2 cm)");
            f85::StepTracker w;
            f85::StepFrame h0 = mkFrame(0, 0.0), h1 = mkFrame(1, 0.0);
            h1.haveCarry = true;
            h1.carryBeforeCm[0] = 0.4f;
            h1.carryBeforeCm[1] = 2.6f;
            w.step(h0, freq);
            const f85::StepResult z = w.step(h1, freq);
            check(z.eventStart && (z.trig & f85::kTrigHandLag) && std::fabs(h1.handLagCm - 2.6f) < 1e-6f && w.sec.trigSeen[2] == 1u,
                  "F86b: a hand error of 2.6 cm before the carry starts a hand-lag event and is counted under its trigger");
            f85::StepTracker nw;
            f85::StepFrame n0 = mkFrame(0, 0.0), n1 = mkFrame(1, 0.0);
            n1.haveCarry = true;   // no wrist written: both errors -1
            nw.step(n0, freq);
            check(!nw.step(n1, freq).eventStart, "F86b: a carry with no written wrist (error -1) starts nothing");
        }
        {
            f85::StepTracker t;
            uint32_t vyTrig = 0;
            double y = 0.0;
            for (uint32_t i = 0; i < 60; ++i) {
                // take-off at i = 10 (vy 3 m/s for 15 frames), landing after
                const double vy = (i >= 10 && i < 25) ? 3.0 : 0.0;
                y += vy / 90.0;
                f85::StepFrame f = mkFrame(i, y);
                const f85::StepResult r = t.step(f, freq);
                if (r.trig & f85::kTrigVy) ++vyTrig;
            }
            check(vyTrig == 2, "F85a: |vy| over 0.8 m/s starting (take-off) and ending (landing) trigger once each");
        }
        {
            // F86b: vy is the step along the body's up row over dt, not world Y: a body tilted 90 degrees (up = world +x) walking along world Y has no vy
            f85::StepTracker t;
            double maxAbsVy = 0.0;
            for (uint32_t i = 0; i < 30; ++i) {
                f85::StepFrame f = mkFrame(i, i * 2.0 / 90.0);   // 2 m/s along world Y
                f.up[0] = 1.0f;
                f.up[1] = 0.0f;
                t.step(f, freq);
                maxAbsVy = std::max(maxAbsVy, std::fabs(static_cast<double>(f.vy)));
            }
            check(maxAbsVy < 1e-3 && t.sec.trigSeen[3] == 0, "F86b: vy reads the step along the body's up row (walking along world Y with up = +x gives 0 and no vy trigger)");
        }
        // at most 8 events a minute
        {
            f85::StepTracker t;
            int starts = 0, suppressed = 0;
            for (uint32_t i = 0; i < 90 * 20; ++i) {
                f85::StepFrame f = mkFrame(i, 0.0);
                f.havePin = true;
                f.vClamp = (i % 45) == 0 && i > 0;   // a cap trip every half second, each beyond the 24-frame event
                const f85::StepResult r = t.step(f, freq);
                starts += r.eventStart ? 1 : 0;
                suppressed += r.suppressed ? 1 : 0;
            }
            check(starts == 8 && suppressed >= 1 && t.sec.eventsStarted == 8u, "F85a: at most 8 events start in a minute; the rest are counted as suppressed");
        }
        // F86b: the cap is per trigger: a busy pin-cap does not use up the hand-lag's eight, and the suppressed count names the trigger
        {
            f85::StepTracker t;
            int starts = 0, capStarts = 0, lagStarts = 0;
            for (uint32_t i = 0; i < 90 * 20; ++i) {
                f85::StepFrame f = mkFrame(i, 0.0);
                f.havePin = true;
                f.vClamp = (i % 45) == 0 && i > 0;   // a cap trip every half second
                if (i >= 90 * 5 && (i % 45) == 20) {   // a hand lag every half second from the fifth second, between the cap trips
                    f.haveCarry = true;
                    f.carryBeforeCm[0] = 5.0f;
                }
                const f85::StepResult r = t.step(f, freq);
                if (r.eventStart) {
                    ++starts;
                    if (r.trig & f85::kTrigPinCap) ++capStarts;
                    if (r.trig & f85::kTrigHandLag) ++lagStarts;
                }
            }
            check(capStarts == 8 && lagStarts == 8 && starts == 16 && t.sec.suppressedBy[1] > 0 && t.sec.suppressedBy[2] > 0 && t.sec.suppressedBy[0] == 0 && t.sec.suppressedBy[3] == 0,
                  "F86b: each trigger has its own 8 a minute (8 pin-cap and 8 hand-lag events), and the suppressed count is kept per trigger");
        }
        // the drawn record against M3P, by speed
        {
            f85::StepTracker t;
            for (uint32_t i = 0; i < 20; ++i) {
                f85::StepFrame f = mkFrame(i, 0.0, i * 0.1);   // 9 m/s: sprint
                f.haveRec = true;
                f.rec[0] = f.m3pT[0] + 0.01f;
                f.rec[1] = f.m3pT[1];
                f.rec[2] = f.m3pT[2];
                t.step(f, freq);
            }
            const auto& acc = t.sec.rec[3];
            check(acc.n >= 18 && std::fabs(acc.sum / static_cast<double>(acc.n) - 1.0) < 0.01 && acc.mx < 1.01, "F85a: the record minus M3P (1 cm here) is binned by speed (sprint) with its mean and maximum");
        }
        check(f85::speedBin(0.1) == 0 && f85::speedBin(1.0) == 1 && f85::speedBin(4.0) == 2 && f85::speedBin(7.0) == 3 && f85::speedBin(25.0) == 4, "F85a: the speed bins are still / walk / jog / sprint / step");
    }
}

// ---- F86 (the pose carry, the weapon UI depth, the floor probe; F86c and F86f went in F87) -----------------------------------------
// The pure functions of src\d3d11\native_on_foot_f86.h on synthetic scenes. Design: reviews\native-onfoot-f86-review-2026-10-23.md.
inline void putMat(float m[16], const double R[3][3], const double t[3]) {
    for (int i = 0; i < 16; ++i) m[i] = 0.0f;
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) m[4 * i + j] = static_cast<float>(R[i][j]);
        m[12 + i] = static_cast<float>(t[i]);
    }
    m[15] = 1.0f;
}
// A depth image (reversed-Z, infinite far) of the plane n . x = c seen by the camera at the origin looking down +z (the scene rows of sceneRows with the same fx, px, py).
inline void planeImage(std::vector<float>& img, uint32_t N, double fx, double px, double py, const double nrm[3], double c, double nearZ) {
    img.resize(static_cast<size_t>(N) * N);
    for (uint32_t iy = 0; iy < N; ++iy)
        for (uint32_t ix = 0; ix < N; ++ix) {
            const double nx = ((ix + 0.5) / N) * 2.0 - 1.0, ny = 1.0 - ((iy + 0.5) / N) * 2.0;
            const double tx = (nx - px) / fx, ty = (ny - py) / fx;
            const double den = nrm[0] * tx + nrm[1] * ty + nrm[2];
            const double s = den > 1e-6 ? c / den : 1e30;
            img[static_cast<size_t>(iy) * N + ix] = s < 1e29 && s > 0.0 ? static_cast<float>(nearZ / s) : 0.0f;
        }
}

template <class Check>
inline void run86(Check&& check) {
    namespace f86 = edvr::nofoot::f86;
    const double I3[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    const auto rotYd = [](double a, double R[3][3]) {
        const double c = std::cos(a), s = std::sin(a);
        const double v[3][3] = {{c, 0, s}, {0, 1, 0}, {-s, 0, c}};
        std::memcpy(R, v, sizeof(v));
    };
    // ---- A: the pose carry ------------------------------------------------------------------------------------------------------------------------
    // The wrists are fixed points A_s of the FP root's frame; the pose of Animate p puts them (in the 3P model) at a_s = (A_s . F_p - d) . inv(M_p); the submit of
    // frame i draws them at a_s . R(M_i) + rec_i. M3P trails F_root by one frame. The hand misses by the change of F_root's step: dF_i - dM_i = dF_i - dF_{i-1}.
    const double Aw[2][3] = {{-0.20, -0.25, 0.35}, {0.20, -0.25, 0.35}};
    struct Seq {
        float F[12][16], M[12][16];
        double rec[12][3];
    };
    const auto solvePose = [&](const float Mp[16], const float Fp[16], const double d[3], uint32_t p1) {
        f86::PoseIn pose;
        pose.have = true;
        pose.p1 = p1;
        std::memcpy(pose.m3p, Mp, sizeof(pose.m3p));
        std::memcpy(pose.froot, Fp, sizeof(pose.froot));
        for (int i = 0; i < 3; ++i) pose.d[i] = static_cast<float>(d[i]);
        f86::Aff aF = f86::affOf(Fp), aMi;
        f86::affInv(f86::affOf(Mp), &aMi);
        for (int s = 0; s < 2; ++s) {
            double w[3], m[3];
            f86::affPoint(aF, Aw[s], w);
            for (int j = 0; j < 3; ++j) w[j] -= d[j];
            f86::affPoint(aMi, w, m);
            pose.wrote[s] = true;
            for (int j = 0; j < 3; ++j) pose.wrist[s][j] = static_cast<float>(m[j]);
        }
        return pose;
    };
    {   // the dump-001 shape: uneven vertical steps, M3P a frame behind F_root, no turn
        const double upStepCm[10] = {0.0, 1.5, 11.1, 0.8, -2.0, -4.6, -6.2, -9.1, -13.5, -13.4};
        double Fy[10] = {}, Mt[10][3], Ft[10][3];
        for (int i = 0; i < 10; ++i) {
            Fy[i] = (i ? Fy[i - 1] : 0.0) + upStepCm[i] * 0.01;
            Ft[i][0] = 0.0;
            Ft[i][1] = Fy[i];
            Ft[i][2] = 0.05 * i;
        }
        for (int i = 1; i < 10; ++i) {
            Mt[i][0] = Ft[i - 1][0];
            Mt[i][1] = Ft[i - 1][1] - 1.0;   // the model sits a metre below the root
            Mt[i][2] = Ft[i - 1][2] + 0.02;
        }
        float F[10][16], M[10][16];
        for (int i = 0; i < 10; ++i) putMat(F[i], I3, Ft[i]);
        for (int i = 1; i < 10; ++i) putMat(M[i], I3, Mt[i]);
        const double d0[3] = {0, 0, 0};
        double worstAfter = 0.0, before[10] = {};
        bool allCarried = true;
        for (int i = 2; i < 10; ++i) {
            const f86::PoseIn pose = solvePose(M[i - 1], F[i - 1], d0, static_cast<uint32_t>(i - 1));
            f86::CarryFrame fr;
            fr.p1 = static_cast<uint32_t>(i);
            std::memcpy(fr.m3p, M[i], sizeof(fr.m3p));
            std::memcpy(fr.froot, F[i], sizeof(fr.froot));
            for (int j = 0; j < 3; ++j) fr.rec[j] = Mt[i][j];
            const f86::CarryResult r = f86::poseCarry(pose, fr, true);
            allCarried = allCarried && r.why == f86::kCarried && r.age == 1u && r.haveWrist[0] && r.haveWrist[1] && !r.usedEye;
            before[i] = std::max(r.errBeforeCm[0], r.errBeforeCm[1]);
            worstAfter = std::max(worstAfter, std::max(r.errAfterCm[0], r.errAfterCm[1]));
        }
        check(allCarried, "F86a: every frame of the dump-001 shape is carried at pose age 1 with both wrists written");
        check(std::fabs(before[2] - 9.6) < 0.01 && std::fabs(before[3] - 10.3) < 0.01 && std::fabs(before[4] - 2.8) < 0.01,
              "F86a: without the carry the wrists miss by 9.6, 10.3 and 2.8 cm in the first three frames of uneven steps (the change of F_root's step)");
        check(worstAfter < 0.01, "F86a: with the carry the same wrists are within 0.01 cm in every frame");
    }
    {   // M3P and F turn 20 degrees a frame together
        float F[8][16], M[8][16];
        double Ft[8][3], Mt[8][3];
        for (int i = 0; i < 8; ++i) {
            Ft[i][0] = 0.3 * i * 0.1;
            Ft[i][1] = 0.02 * i * i;
            Ft[i][2] = 0.12 * i;
            if (i) {
                std::memcpy(Mt[i], Ft[i - 1], sizeof(Mt[i]));
                Mt[i][1] -= 1.0;
            }
            double R[3][3];
            rotYd(20.0 * f85::kPi / 180.0 * i, R);
            putMat(F[i], R, Ft[i]);
            if (i) putMat(M[i], R, Mt[i]);
        }
        const double d0[3] = {0, 0, 0};
        double worstAfter = 0.0, worstBefore = 0.0;
        for (int i = 2; i < 8; ++i) {
            const f86::PoseIn pose = solvePose(M[i - 1], F[i - 1], d0, static_cast<uint32_t>(i - 1));
            f86::CarryFrame fr;
            fr.p1 = static_cast<uint32_t>(i);
            std::memcpy(fr.m3p, M[i], sizeof(fr.m3p));
            std::memcpy(fr.froot, F[i], sizeof(fr.froot));
            for (int j = 0; j < 3; ++j) fr.rec[j] = Mt[i][j];
            const f86::CarryResult r = f86::poseCarry(pose, fr, true);
            worstAfter = std::max(worstAfter, std::max(r.errAfterCm[0], r.errAfterCm[1]));
            worstBefore = std::max(worstBefore, std::max(r.errBeforeCm[0], r.errBeforeCm[1]));
        }
        check(worstBefore > 5.0 && worstAfter < 0.1, "F86a: M3P and F turning 20 degrees a frame together: centimetres of error without the carry, under 0.1 cm with it");
    }
    {   // F alone turns 3.7 degrees a frame; M3P does not: the residual is the turn times the wrists' distance from their mean
        float F[8][16], M[8][16];
        double Ft[8][3], Mt[8][3];
        for (int i = 0; i < 8; ++i) {
            Ft[i][0] = 0.0;
            Ft[i][1] = 0.0;
            Ft[i][2] = 0.1 * i;
            if (i) {
                std::memcpy(Mt[i], Ft[i - 1], sizeof(Mt[i]));
                Mt[i][1] -= 1.0;
            }
            double R[3][3];
            rotYd(3.7 * f85::kPi / 180.0 * i, R);
            putMat(F[i], R, Ft[i]);
            if (i) putMat(M[i], I3, Mt[i]);
        }
        const double d0[3] = {0, 0, 0};
        double worst = 0.0;
        for (int i = 2; i < 8; ++i) {
            const f86::PoseIn pose = solvePose(M[i - 1], F[i - 1], d0, static_cast<uint32_t>(i - 1));
            f86::CarryFrame fr;
            fr.p1 = static_cast<uint32_t>(i);
            std::memcpy(fr.m3p, M[i], sizeof(fr.m3p));
            std::memcpy(fr.froot, F[i], sizeof(fr.froot));
            for (int j = 0; j < 3; ++j) fr.rec[j] = Mt[i][j];
            const f86::CarryResult r = f86::poseCarry(pose, fr, true);
            worst = std::max(worst, std::max(r.errAfterCm[0], r.errAfterCm[1]));
        }
        const double bound = 2.0 * std::sin(0.5 * 3.7 * f85::kPi / 180.0) * 0.20 * 100.0;   // |dR_F| x half the wrist spacing (0.20 m), cm
        check(worst <= bound * 1.01 && worst >= bound * 0.9, "F86a: F alone at 3.7 degrees a frame leaves the turn times half the wrist spacing at the wrists (1.29 cm), no more");
    }
    {   // the body shift d rides both: the pose carried d_p, the record is shifted by d_i
        const double d[3] = {0.10, 0.0, -0.20};
        const double Ft[2][3] = {{0.0, 0.0, 0.0}, {0.0, 0.05, 0.12}}, Mt[2][3] = {{0.0, -1.0, 0.0}, {0.0, -0.99, 0.05}};
        float F[2][16], M[2][16];
        putMat(F[0], I3, Ft[0]);
        putMat(F[1], I3, Ft[1]);
        putMat(M[0], I3, Mt[0]);
        putMat(M[1], I3, Mt[1]);
        const double* Mt1 = Mt[1];
        const f86::PoseIn pose = solvePose(M[0], F[0], d, 0);
        f86::CarryFrame fr;
        fr.p1 = 1;
        std::memcpy(fr.m3p, M[1], sizeof(fr.m3p));
        std::memcpy(fr.froot, F[1], sizeof(fr.froot));
        for (int j = 0; j < 3; ++j) fr.rec[j] = Mt1[j] + d[j];   // the record the body shift moved by d
        const f86::CarryResult r = f86::poseCarry(pose, fr, true);
        check(r.why == f86::kCarried && r.errAfterCm[0] < 0.01 && r.errAfterCm[1] < 0.01, "F86a: the body shift d (in the pose and in the record) is carried through: the wrists land within 0.01 cm");
    }
    {   // refusals: a hitch, the age, no pose, the scope
        double Ft[2][3] = {{0, 0, 0}, {0, 2.4, 0}}, Mt[2][3] = {{0, -1, 0}, {0, -1, 0}};
        float F[2][16], M[2][16];
        for (int i = 0; i < 2; ++i) {
            putMat(F[i], I3, Ft[i]);
            putMat(M[i], I3, Mt[i]);
        }
        const double d0[3] = {0, 0, 0};
        f86::PoseIn pose = solvePose(M[0], F[0], d0, 5);
        f86::CarryFrame fr;
        fr.p1 = 6;
        std::memcpy(fr.m3p, M[1], sizeof(fr.m3p));
        std::memcpy(fr.froot, F[1], sizeof(fr.froot));
        for (int j = 0; j < 3; ++j) fr.rec[j] = Mt[1][j];
        f86::CarryResult r = f86::poseCarry(pose, fr, true);
        check(r.why == f86::kCarryHitch && f85::len3(r.delta) == 0.0 && std::fabs(r.errBeforeCm[0] - 240.0) < 0.1 && std::fabs(r.errAfterCm[0] - 240.0) < 0.1,
              "F86a: a 2.4 m step is refused (over 1.5 m), nothing is moved, and the error before is still reported");
        for (uint32_t age = 0; age <= 4; ++age) {
            f86::PoseIn p2 = solvePose(M[0], F[0], d0, 6 - age);
            f86::CarryFrame f2 = fr;
            std::memcpy(f2.froot, F[0], sizeof(f2.froot));
            std::memcpy(f2.m3p, M[0], sizeof(f2.m3p));
            const f86::CarryResult q = f86::poseCarry(p2, f2, true);
            check(q.age == age && (age <= 3 ? q.why == f86::kCarried : q.why == f86::kCarryOld), "F86a: a pose up to 3 frames old is carried, a fourth is refused (over 3 frames)");
        }
        f86::PoseIn later = solvePose(M[0], F[0], d0, 9);
        check(f86::poseCarry(later, fr, true).why == f86::kCarryOld, "F86a: a pose from a frame after the submit's is refused (its age wraps)");
        check(f86::poseCarry(f86::PoseIn{}, fr, true).why == f86::kCarryNoPose, "F86a: no pose record: refused (no pose)");
        check(f86::poseCarry(pose, fr, false).why == f86::kCarryScope, "F86a: outside the pin's scope: refused (out of scope)");
        // one hand written: the eye stands in; neither hand and no eye: no pose
        pose.wrote[1] = false;
        pose.eyeOk = true;
        pose.eye[0] = 0.0f;
        pose.eye[1] = 1.6f;
        pose.eye[2] = 0.1f;
        fr.p1 = 6;
        std::memcpy(fr.froot, F[0], sizeof(fr.froot));
        std::memcpy(fr.m3p, M[0], sizeof(fr.m3p));
        f86::CarryResult e1 = f86::poseCarry(pose, fr, true);
        check(e1.why == f86::kCarried && e1.usedEye && e1.haveWrist[0] && !e1.haveWrist[1] && e1.errBeforeCm[1] < 0.0, "F86a: with one hand written the eye is the carried point, the written wrist is still measured");
        pose.eyeOk = false;
        check(f86::poseCarry(pose, fr, true).why == f86::kCarryNoPose, "F86a: one hand written and no eye: no pose");
        pose.wrote[0] = false;
        pose.eyeOk = true;
        e1 = f86::poseCarry(pose, fr, true);
        check(e1.why == f86::kCarried && e1.usedEye && !e1.haveWrist[0] && !e1.haveWrist[1] && e1.errBeforeCm[0] < 0.0 && e1.errAfterCm[1] < 0.0,
              "F86a: no hand written (a weapon not out): the eye is carried, no wrist error is reported (-1)");
    }
    {   // the steps and turns the line prints
        double R0[3][3], R1[3][3];
        rotYd(0.0, R0);
        rotYd(20.0 * f85::kPi / 180.0, R1);
        const double t0[3] = {1.0, 2.0, 3.0}, t1[3] = {1.0 + 0.05, 2.0 + 0.02, 3.0 + 0.30};
        float m0[16], m1[16];
        putMat(m0, R0, t0);
        putMat(m1, R1, t1);
        const f86::StepGeom g = f86::stepGeom(m0, m1);
        check(g.ok && std::fabs(g.turnDeg - 20.0) < 1e-4 && std::fabs(g.step[0] - 0.05) < 1e-6 && std::fabs(g.step[1] - 0.02) < 1e-6 && std::fabs(g.step[2] - 0.30) < 1e-6,
              "F86a: the step is read in the earlier frame's right / up / forward axes and the turn is the angle between the two frames");
        double R2[3][3];
        rotYd(90.0 * f85::kPi / 180.0, R2);
        float m2[16];
        putMat(m2, R2, t0);
        const f86::StepGeom h = f86::stepGeom(m2, m1);   // the earlier frame's axes are turned: the same world step reads differently
        check(h.ok && std::fabs(h.step[0] - (0.05 * R2[0][0] + 0.30 * R2[0][2])) < 1e-6, "F86a: the step follows the earlier frame's axes (a turned earlier frame reads the world step in its own)");
    }
    {   // the previous copy of a record (F87): equal to the position at the submit's start; the carry moves both, so the drawn motion is the game's own
        std::srand(7);
        bool motionOk = true, stillOk = true;
        float X[3] = {1.0f, 2.0f, 3.0f};
        float lastPos[3] = {X[0], X[1], X[2]};
        double lastDelta[3] = {};
        for (int i = 0; i < 20; ++i) {
            const bool still = i < 6;
            const float pos[3] = {still ? 1.0f : X[0] + 0.011f * (i % 3), still ? 2.0f : X[1] + 0.002f * i, still ? 3.0f : X[2] + 0.05f * (i - 5)};
            const double delta[3] = {i == 9 ? 0.30 : 0.001 * (std::rand() % 50 - 25), i == 9 ? -0.20 : 0.001 * (std::rand() % 50 - 25), i == 9 ? 0.05 : 0.001 * (std::rand() % 50 - 25)};
            // what the game leaves: pos = this frame's; prev = the same value (the flight's dump 003)
            float prev[3] = {pos[0], pos[1], pos[2]};
            const f86::PrevCase c = f86::classifyPrev(prev, pos);
            float prevW[3];
            f86::prevToWrite(c, prev, delta, prevW);
            float posW[3];
            for (int k = 0; k < 3; ++k) posW[k] = static_cast<float>(static_cast<double>(pos[k]) + delta[k]);
            for (int k = 0; k < 3; ++k) {
                const double drawn = static_cast<double>(posW[k]) - static_cast<double>(prevW[k]);   // what the renderer reads as this frame's motion
                if (c != f86::kPrevWithPos || std::fabs(drawn) > 1e-5) motionOk = false;
                if (still && std::fabs(drawn) > 1e-5) stillOk = false;
            }
            std::memcpy(lastPos, pos, sizeof(lastPos));
            std::memcpy(lastDelta, delta, sizeof(lastDelta));
        }
        check(motionOk, "F87a: where prev equals pos at the start, prev takes the carry too: pos - prev stays 0 whatever the carry (5-30 cm in a jump)");
        check(stillOk, "F87a: a body standing still draws no motion with a still carry (pos - prev 0)");
        // a previous copy that is another value is left alone: the drawn motion is then the carry plus the game's own step
        const float posO[3] = {1.0f, 2.0f, 3.5f}, prevO[3] = {1.0f, 2.0f, 3.4f};
        const double dO[3] = {0.1, 0.1, 0.1};
        float wO[3];
        const f86::PrevCase co = f86::classifyPrev(prevO, posO);
        f86::prevToWrite(co, prevO, dO, wO);
        check(co == f86::kPrevOther && wO[0] == prevO[0] && wO[1] == prevO[1] && wO[2] == prevO[2], "F87a: a previous copy that differs from the position is left alone");
        f86::PrevTally t;
        t.add(f86::kPrevWithPos);
        t.add(f86::kPrevWithPos);
        check(t.verdict() == f86::kPrevWithPos, "F87a: a frame whose records all had prev with pos says with pos");
        t.add(f86::kPrevOther);
        check(t.verdict() == f86::kPrevOther && f86::PrevTally{}.verdict() == f86::kPrevNone, "F87a: a mixed frame says other; a frame with no record examined says none");
        (void)lastPos;
        (void)lastDelta;
    }
    // ---- D, F, G: the depth lines ----------------------------------------------------------------------------------------------------------------
    {
        const Mat I = Mat{{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}};
        const double nearZ = 0.025;
        const double e0[3] = {0, 0, 0};
        // ranged march: planes inside, short of and past the range
        {
            const double fx = 1.38, px = 0.0153 + 0.002, py = 0.0031;
            float rows[16];
            sceneRows(I, e0, fx, fx, px, py, nearZ, rows);
            double A = 0.0, B = 0.0;
            f85::zwFit(rows, &A, &B);
            const uint32_t N = 256;
            std::vector<float> img;
            f85::MarchView mv;
            mv.rows = rows;
            mv.A = A;
            mv.B = B;
            mv.width = mv.height = N;
            const double O[3] = {0, 0, 0}, u[3] = {0, 0, 1};
            const double nz[3] = {0, 0, 1};
            planeImage(img, N, fx, px, py, nz, 0.395, nearZ);
            mv.depth = img.data();
            f86::LineOut lo = f86::marchRangeCpu(mv, O, u, 0.12, 0.8);
            check(lo.status == f86::kLineHit && std::fabs(lo.d - 0.395) < 0.002, "F86d: a plane at 0.395 m is found by a ranged march over 0.12..0.8 m");
            lo = f86::marchRangeCpu(mv, O, u, 0.5, 5.0);
            check(lo.status == f86::kLineNear, "F86d: a range that starts behind the surface reports a surface nearer than its minimum (status near)");
            planeImage(img, N, fx, px, py, nz, 1.2, nearZ);
            lo = f86::marchRangeCpu(mv, O, u, 0.12, 0.8);
            check(lo.status == f86::kLineMiss && !lo.pixOk, "F86d: a plane past the range is a miss");
            f86::LineOut rays[5];
            for (auto& r : rays) r = lo;
            double dn = 0.0;
            check(!f86::nearestHit(rays, 5, &dn), "F86d: five misses give no D_s (the part's last value holds)");
            planeImage(img, N, fx, px, py, nz, 0.395, nearZ);
            rays[3] = f86::marchRangeCpu(mv, O, u, 0.12, 0.8);
            rays[1] = rays[3];
            rays[1].d = 0.41;
            check(f86::nearestHit(rays, 5, &dn) && std::fabs(dn - rays[3].d) < 1e-12, "F86d: D_s is the nearest hit of the part's rays");
            f86::LineOut unused = f86::marchRangeCpu(mv, O, u, 0.5, 0.5);
            check(unused.status == f86::kLineUnused, "F86d: a line with no range is unused");
        }
        // the panel, the scale and the two eyes
        {
            const double kIpd = 0.0606, fxv = 1.38;
            float s0[16], s1[16];
            const double e1[3] = {kIpd, 0, 0};
            sceneRows(I, e0, fxv, fxv, 0.0, 0.0, nearZ, s0);
            sceneRows(I, e1, fxv, fxv, 0.0, 0.0, nearZ, s1);
            const f85::Rows W[2] = {f85::rowsOfWorld(s0), f85::rowsOfWorld(s1)};
            f85::AffParams aff;   // sigma 1, no offsets
            const double oh[3] = {0, 0, 0}, ow[3] = {0, 0, 0};
            double K3[3][3];
            std::memcpy(K3, I3, sizeof(K3));
            const double k4[3] = {0, 0, 0};
            f85::Rows B[2];
            f85::CamK K[2];
            for (int e = 0; e < 2; ++e) {
                B[e] = compositeFrom(W[e], aff, 1.0, K3, k4);
                K[e] = f85::solveK(B[e], W[e], aff);
            }
            // the panel 0.30 m ahead: centre and four corners (hud space = K space here), the plane under it at 0.395 m
            const double pv[5][3] = {{0, 0, 0.30}, {-0.02, -0.012, 0.30}, {0.02, -0.012, 0.30}, {-0.02, 0.012, 0.30}, {0.02, 0.012, 0.30}};
            const double Dpanel = f85::len3(pv[0]);
            float rows[16];
            sceneRows(I, e0, fxv, fxv, 0.0, 0.0, nearZ, rows);
            double A = 0.0, Bz = 0.0;
            f85::zwFit(rows, &A, &Bz);
            const uint32_t N = 512;
            std::vector<float> img;
            const double nz[3] = {0, 0, 1};
            planeImage(img, N, fxv, 0.0, 0.0, nz, 0.395, nearZ);
            f85::MarchView mv;
            mv.rows = rows;
            mv.A = A;
            mv.B = Bz;
            mv.width = mv.height = N;
            mv.depth = img.data();
            f86::LineOut rays[5];
            const double O[3] = {0, 0, 0};
            for (int k = 0; k < 5; ++k) {
                const double l = f85::len3(pv[k]);
                const double u[3] = {pv[k][0] / l, pv[k][1] / l, pv[k][2] / l};
                rays[k] = f86::marchRangeCpu(mv, O, u, f86::kPanelMinM, f86::kPanelMaxM);
            }
            double Ds = 0.0;
            check(f86::nearestHit(rays, 5, &Ds) && std::fabs(Ds - 0.395) < 0.001, "F86d: the plane at 0.395 m under the panel is D_s = 0.395 m (the centre's ray is the nearest)");
            const double s = f86::panelScale(Ds, Dpanel);
            check(std::fabs(s - (0.395 - 0.045) / 0.30) < 0.005 && std::fabs(s * Dpanel - 0.35) < 0.001, "F86d: the panel lands at D_s - 4.5 cm = 0.35 m (scale 1.167)");
            check(f86::panelScale(0.2, 0.30) < 1.0 && f86::panelScale(10.0, 0.30) == f86::kPanelScaleMax && f86::panelScale(0.01, 0.30) == f86::kPanelScaleMin && f86::panelScale(0.3, 0.0) == 1.0,
                  "F86d: a surface nearer than the panel pulls it in; the scale is bounded to 0.5..2 and a degenerate panel keeps 1");
            // the rows: eye 0's corners stay (0.01 px), eye 1's disparity is IPD f / 0.35 m
            double worst0 = 0.0, disparity = 0.0, worstPlace = 0.0;
            bool ok = true;
            const double axis[2] = {0, 0};
            for (int k = 0; k < 5; ++k) {
                double shown[2][2], shownScaled[2][2];
                for (int e = 0; e < 2; ++e) {
                    f85::UFrame uf;
                    std::memcpy(uf.U3, I3, sizeof(uf.U3));
                    f85::Corr c = f85::makeCorrection(K[e], uf);
                    ok = ok && c.ok;
                    const f85::Rows Bu = f85::applyCorrection(B[e], c);
                    f86::scaleCorrection(c, s, oh);
                    const f85::Rows Bs = f85::applyCorrection(B[e], c);
                    ok = ok && f85::shownNdc(aff, axis, Bu, pv[k], shown[e]) && f85::shownNdc(aff, axis, Bs, pv[k], shownScaled[e]);
                    const double ps[3] = {s * pv[k][0], s * pv[k][1], s * pv[k][2]};   // the scaled point in world (rows' origin = the K origin = 0)
                    double want[2];
                    ok = ok && f85::targetNdc(aff, axis, W[e], ps, want);
                    worstPlace = std::max(worstPlace, std::max(std::fabs(shownScaled[e][0] - want[0]), std::fabs(shownScaled[e][1] - want[1])) * 2016.0 * 0.5);
                }
                worst0 = std::max(worst0, std::max(std::fabs(shownScaled[0][0] - shown[0][0]), std::fabs(shownScaled[0][1] - shown[0][1])) * 2016.0 * 0.5);
                if (k == 0) disparity = (shownScaled[1][0] - shownScaled[0][0]) * 2016.0 * 0.5;
            }
            const double wantDisp = -kIpd * fxv / 0.35 * 2016.0 * 0.5;
            check(ok && worst0 < 0.01, "F86d: eye 0's image of the panel's centre and four corners is unchanged by the scale (within 0.01 px)");
            check(ok && std::fabs(disparity - wantDisp) < 0.02, "F86d: eye 1's disparity of the scaled panel is IPD . f / 0.35 m");
            check(ok && worstPlace < 0.01, "F86d: both eyes draw every scaled vertex where the scaled world point projects (within 0.01 px)");
            // the scale on the correction equals the scale on U about the pivot (a rotated K, a turned U, a pivot away from the hud origin)
            {
                const Mat Rk2 = mul(rotY(0.31), rotX(-0.12));
                f85::CamK Kr;
                Kr.ok = true;
                std::memcpy(Kr.K3, Rk2.m, sizeof(Kr.K3));
                Kr.k4[0] = 0.7;
                Kr.k4[1] = -1.1;
                Kr.k4[2] = 0.4;
                const double ohr[3] = {0.4, -0.1, 2.0};
                f85::UFrame uf;
                const Mat Ru = mul(Rk2, rotY(30.0 * f85::kPi / 180.0));
                std::memcpy(uf.U3, Ru.m, sizeof(uf.U3));
                uf.u4[0] = 0.5;
                uf.u4[1] = 0.25;
                uf.u4[2] = -0.7;
                double Orel[3];
                f85::mulVec3(Kr.K3, ohr, Orel);
                for (int i = 0; i < 3; ++i) Orel[i] = -Orel[i] + Kr.k4[i];   // K3 (-o_hud) + k4
                f85::Corr c1 = f85::makeCorrection(Kr, uf);
                f86::scaleCorrection(c1, 1.17, ohr);
                f85::UFrame us = uf;
                f86::scaleUFrameAbout(us, Orel, 1.17);
                const f85::Corr c2 = f85::makeCorrection(Kr, us);
                double err = 0.0;
                for (int i = 0; i < 3; ++i) {
                    err = std::max(err, std::fabs(c1.m4[i] - c2.m4[i]));
                    for (int j = 0; j < 3; ++j) err = std::max(err, std::fabs(c1.M3[i][j] - c2.M3[i][j]));
                }
                check(c1.ok && c2.ok && err < 1e-9, "F86d: scaling the correction M = K^-1 U about the aim camera's origin is scaling U about it");
            }
            // a miss holds: the smoother keeps its value when no ray hits, and the first value is taken at once
            f86::RangeSmoother sm{f86::kPanelMinM, f86::kPanelMaxM};
            check(std::fabs(sm.step(0.395, 0.011) - 0.395) < 1e-12 && sm.have, "F86d: the part's first D_s is taken at once");
            for (int i = 0; i < 20; ++i) sm.step(0.395, 0.011);
            const double held = sm.depth();
            check(std::fabs(held - 0.395) < 1e-9, "F86d: a held target keeps the smoothed depth");
            for (int i = 0; i < 20; ++i) sm.step(0.30, 0.011);
            check(sm.depth() < 0.31 && sm.depth() >= f86::kPanelMinM, "F86d: the smoother follows a new target (in 1/D)");
            f86::RangeSmoother clamp{f86::kPanelMinM, f86::kPanelMaxM};
            clamp.step(0.4, 0.01);
            for (int i = 0; i < 60; ++i) clamp.step(5.0, 0.011);
            check(clamp.depth() <= f86::kPanelMaxM + 1e-9, "F86d: the smoother's clamp is the part's 0.12..0.8 m range");
        }
        // the surface normal from the neighbours the depth shader returns (F86f's snap is gone; the normal stays for the multi-line shader's rig)
        {
            // the normal on a wide camera: a plane 34 degrees off the view ray at 0.94 m
            {
                const double fx = 1.38, px = 0.0153 + 0.002, py = 0.0031;
                float rows[16];
                sceneRows(I, e0, fx, fx, px, py, nearZ, rows);
                double A = 0.0, B = 0.0;
                f85::zwFit(rows, &A, &B);
                const uint32_t N = 512;
                std::vector<float> img;
                const double a34 = 34.0 * f85::kPi / 180.0;
                const double nrm[3] = {std::sin(a34), 0.0, std::cos(a34)};
                planeImage(img, N, fx, px, py, nrm, 0.94 * std::cos(a34), nearZ);
                f85::MarchView mv;
                mv.rows = rows;
                mv.A = A;
                mv.B = B;
                mv.width = mv.height = N;
                mv.depth = img.data();
                const double O[3] = {0, 0, 0}, u[3] = {0, 0, 1};
                const f86::LineOut lo = f86::marchRangeCpu(mv, O, u, 0.5, 1.5);
                const f85::Rows Wr = f85::rowsOfWorld(rows);
                const double H[3] = {lo.d * u[0], lo.d * u[1], lo.d * u[2]};
                const double w = Wr.r[2][0] * H[0] + Wr.r[2][1] * H[1] + Wr.r[2][2] * H[2] + Wr.r[2][3];
                const f86::SurfaceNormal sn = f86::surfaceNormal(Wr, N, N, lo, w, u);
                const double dot = -(sn.n[0] * nrm[0] + sn.n[1] * nrm[1] + sn.n[2] * nrm[2]);
                check(lo.status == f86::kLineHit && std::fabs(lo.d - 0.94) < 0.003 && sn.ok && std::acos(std::min(1.0, dot)) * f85::kDeg < 1.0 && std::fabs(sn.cosView - std::cos(a34)) < 0.01,
                      "F86: the surface normal from the four neighbours is the plane's within 1 degree (34 degrees off the view ray at 0.94 m)");
                check(std::fabs(f86::normalOffRayDeg(sn) - 34.0) < 1.0, "F86: the normal's angle off the ray is 34 degrees");
            }
        }
        // G: the floor probe
        {
            const double T[3] = {10.0, 2.0, -5.0}, up[3] = {0, 1, 0};
            double h = 0.0;
            const double H1[3] = {10.5, 1.97, -4.4}, H2[3] = {10.2, 2.0, -5.1}, H3[3] = {12.0, 2.0, -5.0};
            check(f86::floorAccept(H1, T, up, &h) && std::fabs(h + 0.03) < 1e-9, "F86g: a hit 0.78 m from the 3P origin, 3 cm below it, is accepted and reads -3 cm");
            check(!f86::floorAccept(H2, T, up, &h) && !f86::floorAccept(H3, T, up, &h), "F86g: hits under 0.4 m and over 1.2 m horizontally are refused");
            const double tilt[3] = {0.0, std::cos(0.2), std::sin(0.2)};
            const double Ht[3] = {10.0 + 0.7, 2.0 - 0.1, -5.0 + 0.3};   // 0.7 right, 0.1 down, 0.3 ahead in the world axes
            check(f86::floorAccept(Ht, T, tilt, &h) && std::fabs(h - (-0.1 * tilt[1] + 0.3 * tilt[2])) < 1e-9, "F86g: the height is along the body's up, the distance across it");
            float m3p[16];
            double R[3][3];
            rotYd(90.0 * f85::kPi / 180.0, R);
            putMat(m3p, R, T);
            double pts[2][3];
            f86::floorProbePoints(m3p, pts);
            // R = rotY(90): row 0 (right) = (0, 0, 1), row 2 (forward) = (-1, 0, 0): the points are T + 0.7 fwd -+ 0.3 right
            check(std::fabs(pts[0][0] - (T[0] - 0.7)) < 1e-6 && std::fabs(pts[0][2] - (T[2] - 0.3)) < 1e-6 && std::fabs(pts[1][2] - (T[2] + 0.3)) < 1e-6 && std::fabs(pts[0][1] - T[1]) < 1e-6,
                  "F86g: the probe's two rays aim at the floor line 0.7 m ahead of the 3P origin, 0.3 m to each side, in the body's axes");
        }
    }
    // ---- the depth pass in the rig of reticle_depth_test: the CPU side of the unproject / normal ---------------------------------------------------
    {
        float rows[16];
        const double zero3[3] = {0, 0, 0};
        sceneRows(Mat{{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}}, zero3, 1.38, 1.38, 0.01, 0.002, 0.025, rows);
        const f85::Rows W = f85::rowsOfWorld(rows);
        double p[3];
        check(f86::unprojectPixel(W, 512, 512, 100.0, 300.0, 2.0, p), "F86: a pixel at a view depth unprojects");
        double ndc[2], w = 0.0;
        check(f85::project(W, p, ndc, &w) && std::fabs(w - 2.0) < 1e-9 && std::fabs((ndc[0] * 0.5 + 0.5) * 512.0 - 100.5) < 1e-6 && std::fabs((0.5 - ndc[1] * 0.5) * 512.0 - 300.5) < 1e-6,
              "F86: the unprojected point projects back to the pixel's centre at its view depth");
        double o[3];
        check(f86::cameraOriginOfRows(W, o) && f85::len3(o) < 1e-9, "F86d: the eye's origin from its scene rows is where the rows put it");
        check(!f86::unprojectPixel(W, 512, 512, 100.0, 300.0, 0.0, p) && !f86::unprojectPixel(W, 512, 512, 100.0, 300.0, 1e30, p), "F86: a pixel at no depth, or at the far plane, does not unproject");
    }
    check(std::strcmp(f86::carryWhyText(f86::kCarried), "carried") == 0 && std::strcmp(f86::carryWhyText(f86::kCarryNoPose), "no pose") == 0 &&
              std::strcmp(f86::carryWhyText(f86::kCarryOld), "over 3 frames") == 0 && std::strcmp(f86::carryWhyText(f86::kCarryHitch), "over 1.5 m") == 0 &&
              std::strcmp(f86::carryWhyText(f86::kCarryScope), "out of scope") == 0 && std::strcmp(f86::prevCaseText(f86::kPrevWithPos), "with pos") == 0 &&
              std::strcmp(f86::prevCaseText(f86::kPrevOther), "other") == 0,
          "F86a: the carry's status words are the second's line's");
}


#ifdef _WIN32
// F87p: stand-ins for the game's functions and for the probes' callbacks, to run the emitted machine code (nofoot::f87::emitTrampoline, emitGetterStub).
namespace stubrig {
struct State {
    uint64_t origArgs[7] = {};
    double origD[3] = {};
    uint64_t origRet = 0, recSlot = ~0ull, recRet = 0, recRegs[4] = {}, recStk[3] = {}, preArgs[4] = {}, postArgs[4] = {}, realArgs[3] = {};
    int origCalls = 0, recCalls = 0, seq = 0, preAt = 0, realAt = 0, postAt = 0;
    float pAtPre = 0.0f, pAtPost = 0.0f;
    bool aligned = true;
};
inline State& st() {
    static State s;
    return s;
}
inline void clobber() {   // the callbacks use every xmm register they like
    volatile double a = 1.0, b = 2.0, c = 3.0, d = 4.0, e = 5.0, f = 6.0, g = 7.0, h = 8.0;
    for (int i = 0; i < 4; ++i) {
        a = a * b + c;
        b = c * d + e;
        c = d * e + f;
        d = e * f + g;
        e = f * g + h;
        f = g * h + a;
        g = h * a + b;
        h = a * b + c;
    }
}
__forceinline bool enteredAligned() { return (reinterpret_cast<uintptr_t>(_AddressOfReturnAddress()) % 16) == 8; }
inline uint64_t fakeOrig(uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e5, uint64_t e6, uint64_t e7) {
    State& s = st();
    s.aligned = s.aligned && enteredAligned();
    const uint64_t v[7] = {a, b, c, d, e5, e6, e7};
    for (int i = 0; i < 7; ++i) s.origArgs[i] = v[i];
    ++s.origCalls;
    return 0xABCD0001ull;
}
inline uint64_t fakeOrigF(uint64_t a, double d1, double d2, double d3) {
    State& s = st();
    s.aligned = s.aligned && enteredAligned();
    s.origArgs[0] = a;
    s.origD[0] = d1;
    s.origD[1] = d2;
    s.origD[2] = d3;
    ++s.origCalls;
    return 0xABCD0002ull;
}
inline void record(uint64_t slot, uint64_t ret, const uint64_t* regs, const uint8_t* stk) {
    State& s = st();
    s.aligned = s.aligned && enteredAligned();
    s.recSlot = slot;
    s.recRet = ret;
    for (int i = 0; i < 4; ++i) s.recRegs[i] = regs[i];
    std::memcpy(s.recStk, stk + 0x20, sizeof(s.recStk));
    ++s.recCalls;
    clobber();
}
inline void pre(uint64_t a, uint64_t b, uint64_t c, uint64_t site) {
    State& s = st();
    s.aligned = s.aligned && enteredAligned();
    s.preArgs[0] = a;
    s.preArgs[1] = b;
    s.preArgs[2] = c;
    s.preArgs[3] = site;
    s.pAtPre = *reinterpret_cast<float*>(c);
    s.preAt = ++s.seq;
    clobber();
}
inline uint64_t real(uint64_t a, uint64_t b, uint64_t c) {
    State& s = st();
    s.aligned = s.aligned && enteredAligned();
    s.realArgs[0] = a;
    s.realArgs[1] = b;
    s.realArgs[2] = c;
    *reinterpret_cast<float*>(c) = 5.0f;
    s.realAt = ++s.seq;
    return a + b + c;
}
inline void post(uint64_t a, uint64_t b, uint64_t c, uint64_t site) {
    State& s = st();
    s.aligned = s.aligned && enteredAligned();
    s.postArgs[0] = a;
    s.postArgs[1] = b;
    s.postArgs[2] = c;
    s.postArgs[3] = site;
    s.pAtPost = *reinterpret_cast<float*>(c);
    s.postAt = ++s.seq;
    clobber();
}
}  // namespace stubrig
#endif

// ---- F87 ---------------------------------------------------------------------------------------------------------------------------------------------
// The pure functions of src\d3d11\native_on_foot_f87.h. Design: reviews\native-onfoot-f87-review-2026-10-24.md.
template <class Check>
inline void run87(Check&& check) {
    namespace f86 = edvr::nofoot::f86;
    namespace f87 = edvr::nofoot::f87;
    namespace nf = edvr::nofoot;
    const double I3[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    const double dt = 1.0 / 90.0;
    // ---- H: the standing-height hold -----------------------------------------------------------------------------------------------------------
    {   // a 10 cm slump held: the hold reaches +10 cm within 1 cm by 4 s, and the pin's eye does not move
        f87::HeightHold h;
        double e0 = 1.62, maxPinErr = 0.0, raiseAt1 = 0.0, raiseAt2 = 0.0, raiseAt3 = 0.0, raiseAt4 = 0.0, transientAt03 = 0.0, transientAt6 = 1.0, maxRate = 0.0, prevRaise = 0.0;
        f87::HoldIn in;
        in.scope = true;
        in.devValid = true;
        in.dt = dt;
        in.settleS = 3.0;   // F89: the default is 0.1 s; this case is the slow absorb
        for (int i = 0; i < 90; ++i) h.step(in);   // 1 s at the seat: dev 0
        for (int i = 0; i < 90 * 6; ++i) {
            in.devM = -0.10;
            const f87::HoldOut o = h.step(in);
            const double t = (i + 1) * dt;
            const double eCam = e0 + in.devM + o.raiseM;   // the camera over the character's standing height
            maxPinErr = std::max(maxPinErr, std::fabs(f87::pinEyeYAdjust(eCam, o.transientM, o.transientValid) - e0));
            if (i == 89) raiseAt1 = o.raiseM;
            if (i == 179) raiseAt2 = o.raiseM;
            if (i == 269) raiseAt3 = o.raiseM;
            if (i == 359) raiseAt4 = o.raiseM;
            if (std::fabs(t - 0.3) < dt * 0.6) transientAt03 = o.transientM;
            if (i == 90 * 6 - 1) transientAt6 = o.transientM;
            if (i >= 90 && i % 90 == 0) {
                maxRate = std::max(maxRate, std::fabs(o.raiseM - prevRaise));
                prevRaise = o.raiseM;
            }
        }
        check(std::fabs(raiseAt4 - 0.10) < 0.01 && raiseAt4 > 0.0, "F87h: a 10 cm slump held for 4 s is absorbed: the hold reads +10 cm within 1 cm");
        check(maxPinErr < 1e-9, "F87h: while it is absorbed the pin's eye (camera less the transient) does not move: the body keeps the character's height");
        check(transientAt03 < -0.085 && std::fabs(transientAt6) < 0.01, "F87h: the slump is seen at first (the view is 8.5+ cm down at 0.3 s) and undone: the transient is under 1 cm at 6 s");
        check(raiseAt1 > 0.01 && raiseAt1 < raiseAt2 && raiseAt2 < raiseAt3 && raiseAt3 <= raiseAt4 + 1e-9 && maxRate <= 0.0415, "F87h: the hold rises steadily, at most 4 cm a second (the largest second's rise reads under 4.15 cm: 4 cm and a millimetre of rounding)");
        check(f87::kHoldSlewMps == 0.04 && f87::kHoldStepM == 0.001 && f87::kHoldRampS == 0.5 && f87::kSettleDefaultS == 0.1 && f87::HoldIn{}.settleS == 0.1,
              "F87h: the constants are 4 cm/s, 1 mm steps, a 0.5 s ramp and a 0.1 s default settle (F89: was 3)");
    }
    {   // a -60 degree nod with the neck still: the eye drops by the pivot's drop and the hold stays 0
        const double pitch = -60.0 * f85::kPi / 180.0;
        const double drop = nf::pinPitchRestUp(pitch, true) - nf::kPinRestUpM;
        f87::HeightHold h;
        f87::HoldIn in;
        in.scope = true;
        in.devValid = true;
        in.dt = dt;
        double worst = 0.0, worstDev = 0.0;
        for (int i = 0; i < 90 * 5; ++i) {
            in.devM = f87::devOf(0.0 + drop, 0.0, drop);   // the head's seated height is the seat's plus the drop of the pivot
            worstDev = std::max(worstDev, std::fabs(in.devM));
            worst = std::max(worst, std::fabs(h.step(in).raiseM));
        }
        check(drop < -0.15 && worstDev < 1e-12 && worst == 0.0, "F87h: a 60 degree nod with a still neck lowers the eye 15 cm about the pivot and the hold reads 0");
    }
    {   // a 5 cm bob of 0.3 s: at least 80% in the view, none in the pin
        f87::HeightHold h;
        f87::HoldIn in;
        in.scope = true;
        in.devValid = true;
        in.dt = dt;
        in.settleS = 3.0;   // F89: the default is 0.1 s; this case is the slow absorb
        for (int i = 0; i < 90 * 2; ++i) h.step(in);
        double worstView = 0.0, pinErr = 0.0;
        for (int i = 0; i < 27; ++i) {   // 0.3 s
            in.devM = -0.05;
            const f87::HoldOut o = h.step(in);
            worstView = std::max(worstView, std::fabs(in.devM + o.raiseM));
            pinErr = std::max(pinErr, std::fabs(f87::pinEyeYAdjust(1.62 + in.devM + o.raiseM, o.transientM, o.transientValid) - 1.62));
        }
        check(worstView >= 0.04 && pinErr < 1e-9, "F87h: a 5 cm bob of 0.3 s is followed by the view (at least 4 cm of it) and not by the pin");
    }
    {   // the game's crouch moves F_root, not the head: the hold stays, the crouch weight reaches 1
        f87::HeightHold h;
        f87::HoldIn in;
        in.scope = true;
        in.devValid = true;
        in.dt = dt;
        double worst = 0.0;
        for (int i = 0; i < 90 * 3; ++i) worst = std::max(worst, std::fabs(h.step(in).raiseM));
        const double hStand = 1.62, eCrouched = hStand - 0.45;   // the camera follows F_root down 45 cm; the headset did not move
        const f87::HoldOut o = h.step(in);
        check(worst == 0.0 && std::fabs(f87::pinEyeYAdjust(eCrouched, o.transientM, o.transientValid) - eCrouched) < 1e-12 && nf::pinCrouchWeight(true, hStand, f87::pinEyeYAdjust(eCrouched, o.transientM, o.transientValid)) == 1.0,
              "F87h: a game crouch leaves the hold at 0 and the pin sees the full crouch (the crouch weight is 1)");
    }
    {   // ADS: the filter freezes, the eye alignment is the hold plus the lock
        f87::HeightHold h;
        f87::HoldIn in;
        in.scope = true;
        in.devValid = true;
        in.dt = dt;
        in.settleS = 3.0;   // F89: the default is 0.1 s; this case is the slow absorb
        in.devM = -0.08;
        for (int i = 0; i < 90 * 5; ++i) h.step(in);
        const f87::HoldOut before = h.step(in);
        in.frozen = true;
        in.devM = -0.20;
        f87::HoldOut o;
        for (int i = 0; i < 90 * 3; ++i) o = h.step(in);
        check(before.raiseM > 0.07 && o.raiseM == before.raiseM && o.sustainedM == before.sustainedM, "F87h: in ADS the filter is frozen: a head that moves 12 cm more changes neither the sustained part nor the hold");
        const double lockY = 0.013;
        check(std::fabs(f87::eyeAlignY(0.0, lockY, 0.0, 0.0, o.eyeY) - (o.eyeY + lockY)) < 1e-15 && o.eyeY == -o.raiseM,
              "F87h: the eye alignment's y is the hold's (the negative of the raise: the runtime moves the origin up to lower the view) plus the ADS lock");
    }
    {   // out of scope: the hold ramps to 0 in 0.5 s, in whole millimetres
        f87::HeightHold h;
        f87::HoldIn in;
        in.scope = true;
        in.devValid = true;
        in.dt = dt;
        in.devM = -0.09;
        for (int i = 0; i < 90 * 8; ++i) h.step(in);
        const double full = h.step(in).raiseM;
        in.scope = false;
        bool mm = true;
        double at0 = 1.0, at05 = 1.0;
        for (int i = 0; i < 60; ++i) {
            const f87::HoldOut o = h.step(in);
            mm = mm && std::fabs(o.raiseM * 1000.0 - std::round(o.raiseM * 1000.0)) < 1e-6 && std::fabs(o.eyeY * 1000.0 - std::round(o.eyeY * 1000.0)) < 1e-6;
            if (i == 0) at0 = o.raiseM;
            if (i == 45) at05 = o.raiseM;
        }
        const f87::HoldOut last = h.step(in);
        check(full > 0.085 && at0 < full && at05 == 0.0 && last.raiseM == 0.0 && last.eyeY == 0.0 && !last.transientValid && mm,
              "F87h: out of the pin's scope the hold is 0 within 0.5 s, in whole millimetres, and the cockpit publishes zeros");
        in.scope = true;
        double at05in = 0.0;
        for (int i = 0; i < 46; ++i) at05in = h.step(in).raiseM;
        check(at05in > 0.085 && at05in < 0.095, "F87h: entering the scope with a 9 cm slump the view is up 9 cm within 0.5 s (no 3 s drift at the start)");
    }
    {   // settle 0: the neck's height held hard; the clamp of the key
        f87::HeightHold h;
        f87::HoldIn in;
        in.scope = true;
        in.devValid = true;
        in.dt = dt;
        in.settleS = 0.0;
        for (int i = 0; i < 90; ++i) h.step(in);
        in.devM = -0.07;
        f87::HoldOut o = h.step(in);
        check(std::fabs(o.raiseM - 0.07) < 1e-9 && std::fabs(o.transientM) < 1e-9, "F87h: a settle time of 0 holds the neck hard: the raise is the whole deviation at once and the transient is 0");
        check(f87::settleClamp(std::numeric_limits<double>::quiet_NaN()) == f87::kSettleDefaultS && f87::settleClamp(-1.0) == 0.0 && f87::settleClamp(20.0) == f87::kSettleMaxS && f87::settleClamp(3.0) == 3.0,
              "F87h: the settle key reads 0..10 s, NaN reads the default 0.1");
    }
    {   // the measurement and the pin calibration's stillness gate
        check(std::fabs(f87::devOf(0.05, -0.02, -0.10) - 0.13) < 1e-12, "F87h: dev is the seated height plus the eye alignment taken out, less the neck pivot's drop");
        f87::StillGate g;
        for (int i = 0; i < 90 * 2; ++i) g.step(true, (i % 2) ? 0.02 : -0.02, dt);
        const bool movingNo = !g.ok;
        for (int i = 0; i < 80; ++i) g.step(true, 0.003, dt);
        const bool notYet = !g.ok;
        for (int i = 0; i < 20; ++i) g.step(true, 0.003, dt);
        check(movingNo && notYet && g.ok, "F87h: the pin's standing calibration waits until the view has been within 1 cm of the character's height for 1 s");
        f87::HoldSecond sec;
        sec.add(true, -0.08, -0.01, 0.846, true);
        sec.add(true, -0.10, -0.02, 0.744, true);
        sec.add(true, -0.09, -0.01, 0.745, true);
        check(sec.n == 3 && sec.jumps == 1 && std::fabs(sec.maxStep - 0.102) < 1e-9 && std::fabs(sec.devMin + 0.10) < 1e-12 && std::fabs(sec.maxTransient - 0.02) < 1e-12,
              "F87h: the second's figures: the largest step of the LOCAL height (10.2 cm), the steps over 2 cm, dev's range and the transient's largest");
        f87::EyeRootSecond er;
        const double rowsA[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
        const double eye[3] = {0.032, 1.58, 0.21}, root[3] = {0.0, 1.60, 0.20};
        er.add(eye, root, rowsA);
        const double eye2[3] = {0.032, 1.52, 0.21};
        er.add(eye2, root, rowsA);
        check(er.n == 2 && std::fabs(er.last[0] - 0.032) < 1e-12 && std::fabs(er.last[1] + 0.08) < 1e-12 && std::fabs(er.upMin + 0.08) < 1e-12 && std::fabs(er.upMax + 0.02) < 1e-12,
              "F87h: the eye over the weapon's root in the character's axes, with the second's up range");
    }
    // ---- A: the carry, its join, the cap, the floor probe, the hand residual -----------------------------------------------------------------------
    {
        f87::Ring<int, 64> ring;
        ring.post(10, 100);
        ring.post(11, 111);
        int v = 0;
        check(ring.get(10, &v) && v == 100 && ring.get(11, &v) && v == 111 && !ring.get(12, &v) && !ring.get(10 + 64, &v) && !ring.get(9, &v),
              "F87a: the optic record joins the carry by the pose's game frame (the submit of that frame): a frame not yet posted, or one a ring older, is not found");
    }
    {   // 1.2 m is carried, 1.6 m is refused
        float F[2][16], M[2][16];
        const double Fa[3] = {0, 0, 0}, Ma[3] = {0, -1, 0};
        for (double step : {1.2, 1.6}) {
            const double Fb[3] = {0, step, 0};
            putMat(F[0], I3, Fa);
            putMat(F[1], I3, Fb);
            putMat(M[0], I3, Ma);
            putMat(M[1], I3, Ma);
            f86::PoseIn pose;
            pose.have = true;
            pose.p1 = 5;
            std::memcpy(pose.m3p, M[0], sizeof(pose.m3p));
            std::memcpy(pose.froot, F[0], sizeof(pose.froot));
            pose.eyeOk = true;
            pose.eye[1] = 1.6f;
            f86::CarryFrame fr;
            fr.p1 = 6;
            std::memcpy(fr.m3p, M[1], sizeof(fr.m3p));
            std::memcpy(fr.froot, F[1], sizeof(fr.froot));
            for (int j = 0; j < 3; ++j) fr.rec[j] = Ma[j];
            const f86::CarryResult r = f86::poseCarry(pose, fr, true);
            if (step < 1.5) check(r.why == f86::kCarried && std::fabs(f85::len3(r.delta) - 1.2) < 1e-6, "F87a: a 1.2 m delta (a take-off or a landing) is carried");
            else check(r.why == f86::kCarryHitch && f85::len3(r.delta) == 0.0, "F87a: a 1.6 m delta is refused (over 1.5 m) and nothing is moved");
        }
    }
    {
        float m[16];
        const double t[3] = {1, 2, 3};
        putMat(m, I3, t);
        check(f87::affineFinite(m), "F87a: a matrix with finite affine entries is accepted");
        m[3] = m[7] = m[11] = std::numeric_limits<float>::quiet_NaN();
        m[15] = std::numeric_limits<float>::infinity();
        check(f87::affineFinite(m), "F87a: NaN in the padding words (3, 7, 11, 15) is accepted: the floor probe no longer refuses M3P from 02:35:11 on");
        m[13] = std::numeric_limits<float>::quiet_NaN();
        check(!f87::affineFinite(m), "F87a: NaN in an affine entry is refused");
    }
    {   // the hand residual: 0 when the carry is exactly the change, the carry's size without it, the turn applied
        const double target[3] = {0.20, 1.40, 0.50}, wrist[3] = {0.10, 1.30, 0.30};
        double Rm[3][3];
        const double c90 = 0.0, s90 = 1.0;   // rotY(90): row 0 (1,0,0) -> (0,0,-1), row 2 (0,0,1) -> (1,0,0)
        const double Ry[3][3] = {{c90, 0, -s90}, {0, 1, 0}, {s90, 0, c90}};
        std::memcpy(Rm, Ry, sizeof(Rm));
        float m[16];
        const double z3[3] = {};
        putMat(m, Rm, z3);
        // (target - wrist) = (0.10, 0.10, 0.20); times R: x' = 0.10*0 + 0.10*0 + 0.20*1 = 0.20; y' = 0.10; z' = 0.10*(-1) = -0.10
        const double want[3] = {0.20, 0.10, -0.10};
        check(std::fabs(f87::handResidualCm(target, wrist, m, want)) < 1e-9, "F87a: the hand residual is 0 when the carry written is the change of the hand in the world");
        const double none[3] = {};
        check(std::fabs(f87::handResidualCm(target, wrist, m, none) - 100.0 * std::sqrt(0.04 + 0.01 + 0.01)) < 1e-9, "F87a: with no carry the residual is the hand's change, turned by M3P, in cm");
        f87::HandSecond hs;
        hs.add(0, 1.0, 0);
        hs.add(0, 3.0, 0);
        hs.add(1, 2.0, 2);
        hs.add(1, -1.0, 2);
        check(hs.n[0] == 2 && hs.n[1] == 1 && hs.mx[0] == 3.0 && std::fabs(hs.sum[0] - 4.0) < 1e-12 && hs.binN[0][0] == 2 && hs.binN[2][1] == 1 && f87::fpFrameBucket(-1) == 0 &&
                  f87::fpFrameBucket(0) == 1 && f87::fpFrameBucket(1) == 2 && f87::fpFrameBucket(7) == 3,
              "F87a: the second's residual figures by hand and speed bin (a negative residual is not counted), and the FP-frame buckets");
    }
    // ---- C: the stream windows' history and escalation ---------------------------------------------------------------------------------------------
    {
        const uint32_t total = 196608;
        f87::ClusterHist hist;
        f87::WinPlan p;
        f87::planWindows(hist, true, 100000, total, &p);
        check(p.windows == 1 && p.f85 == 1 && p.history == 0 && p.n[0] == f87::kF85Entries && p.holds(100000 - 4090) && p.holds(100000 + 4090) && !p.holds(100000 + 4100),
              "F87c: F85's window (8192 entries about the newest entry) is always in the plan, with no history yet");
        // three layouts cycling one per frame: only the first cycle misses
        int misses = 0, lateMisses = 0;
        uint32_t centre = ~0u;
        const uint32_t base[3] = {20000, 60000, 140000};
        f87::ClusterHist h;
        for (uint32_t f = 0; f < 60; ++f) {
            f87::WinPlan plan;
            h.expire(f);
            f87::planWindows(h, centre != ~0u, centre, total, &plan);
            const uint32_t entry = base[f % 3] + 5 * (f % 7);
            if (!plan.holds(entry)) {
                ++misses;
                if (f >= 3) ++lateMisses;
            }
            h.add(entry, f);
            centre = entry;
        }
        check(misses == 3 && lateMisses == 0, "F87c: three layouts cycling one a frame: only the first cycle misses (3 draws), every later frame is covered by the history");
        // escalation covers all
        f87::Escalation esc;
        esc.outside(100);
        bool on = true;
        for (uint32_t f = 100; f <= 108; ++f) on = on && esc.active(f);
        check(on && !esc.active(109) && !esc.active(300), "F87c: a draw outside the windows holds the whole stream for 8 frames, then not");
        f87::WinPlan full;
        full.full = true;
        check(full.full && f87::kEscalateFrames == 8 && f87::kKeepFrames == 64, "F87c: the escalation lasts 8 frames and the history 64");
    }
    {
        const uint32_t total = 196608;
        f87::ClusterHist h;
        for (uint32_t i = 0; i < 12; ++i) h.add(5000u * (i + 1), 10 + i);   // 12 clusters 5000 apart
        f87::WinPlan p;
        f87::planWindows(h, true, 100, total, &p);
        uint64_t bytes = 0;
        bool even = true, contiguous = true;
        uint32_t offAt = 0;
        for (uint32_t w = 0; w < p.windows; ++w) {
            bytes += static_cast<uint64_t>(p.n[w]) * 8u;
            even = even && (p.lo[w] % 2u) == 0u && (p.n[w] % 2u) == 0u && (w == 0 ? p.n[w] <= f87::kF85Entries : p.n[w] <= f87::kWinEntries);
            contiguous = contiguous && p.off[w] == offAt;
            offAt += p.n[w];
        }
        check(p.windows == f87::kWinMax && p.history == f87::kHistWindows && p.dropped >= 5 && bytes <= static_cast<uint64_t>(f87::kWinCapEntries) * 8u && even && contiguous,
              "F87c: at most 1 + 6 windows an eye (256 KB), even starts and lengths, laid end to end in the eye's buffer; the clusters past them are counted as dropped");
        // the most recently seen clusters are the ones kept
        check(p.holds(5000u * 12u) && p.holds(5000u * 8u) && !p.holds(5000u * 2u), "F87c: when the windows run out the newest clusters are kept and the oldest dropped");
        h.expire(10 + 11 + 65);
        check(h.count() == 0, "F87c: clusters not drawn for 64 frames leave the history");
        f87::ClusterHist m;
        m.add(1000, 1);
        m.add(1010, 2);
        const uint32_t one = m.count();
        m.add(30000, 3);
        const uint32_t two = m.count();
        m.add(1000 + 2048, 4);
        m.add(1000 + 2 * 2048, 5);
        m.add(1000 + 3 * 2048, 5);
        check(one == 1 && two == 2 && m.count() == 2, "F87c: entries within 2048 of a cluster join it (10 apart: one), 29000 apart open another, a chain of near entries extends one cluster");
        f87::ClusterHist wide;
        for (uint32_t x = 0; x <= 10000; x += 1500) wide.add(x, 1);
        f87::planWindows(wide, false, 0, total, &p);
        bool all = true;
        for (uint32_t x = 0; x <= 10000; x += 1500) all = all && p.holds(x);
        check(p.clusters == 1 && p.windows == 3 && all, "F87c: a cluster wider than a window is covered by windows end to end");
        f87::planWindows(wide, true, 5000, 1000, &p);
        check(p.windows >= 1 && p.lo[0] == 0 && p.n[0] <= 1000 && p.distance(500) == 0u, "F87c: a buffer smaller than a window is copied from its start");
        f87::planWindows(f87::ClusterHist{}, false, 0, total, &p);
        check(p.windows == 0 && !p.holds(0) && p.distance(5) == ~0u, "F87c: no entries, no windows");
    }
    // ---- F: the scene rows' fallback ---------------------------------------------------------------------------------------------------------------
    {
        float a[20], b[20];
        for (int i = 0; i < 20; ++i) a[i] = b[i] = 1.0f + static_cast<float>(i) * 0.25f;
        check(f87::rowsMaxDiff(a, b) == 0.0, "F87f: identical rows differ by 0");
        b[5] += 2e-5f;
        check(std::fabs(f87::rowsMaxDiff(a, b) - 2e-5) < 5e-6, "F87f: the largest absolute difference of the 20 values is reported");
        b[5] = std::numeric_limits<float>::quiet_NaN();
        check(f87::rowsMaxDiff(a, b) > 1e29, "F87f: a NaN counts as infinite");
        f87::FbGuard g;
        check(g.compare(0.0) && g.compare(9e-6) && !g.latchedOff && g.both == 2, "F87f: a match is accepted: the fallback stays on");
        check(!g.compare(2e-5) && g.latchedOff && g.mismatches == 1 && std::fabs(g.maxDiff - 2e-5) < 1e-12, "F87f: one difference over 1e-5 latches the fallback off");
        check(!g.compare(0.0) && g.latchedOff, "F87f: and it stays off");
        check(f87::fallbackPair(f87::kFbVs, f87::kFbPs) && !f87::fallbackPair(f87::kFbVs, 0) && !f87::fallbackPair(0, f87::kFbPs) && f87::kFbVs == 0xF516BF0201303B87ull && f87::kFbPs == 0xB40B0462256E31C2ull,
              "F87f: the fallback is the depth prepass pair F516BF02 / B40B0462");
    }

#ifdef _WIN32
    {   // the emitted machine code, run (the very bytes the hooks install)
        using namespace stubrig;
        uint8_t* page = static_cast<uint8_t*>(VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
        check(page != nullptr, "F87p: executable memory for the stub rig");
        if (page) {
            size_t at = 0;
            // a trampoline in front of a 7-argument game function: all seven arguments, the return value and the stack alignment reach it exactly as given
            uint8_t* t1 = page + at;
            at += f87::emitTrampoline(page + at, reinterpret_cast<uint64_t>(&fakeOrig), 1, reinterpret_cast<uint64_t>(&record));
            at = (at + 15) & ~static_cast<size_t>(15);
            uint8_t* t2 = page + at;
            at += f87::emitTrampoline(page + at, reinterpret_cast<uint64_t>(&fakeOrigF), 2, reinterpret_cast<uint64_t>(&record));
            at = (at + 15) & ~static_cast<size_t>(15);
            uint8_t* g1 = page + at;
            at += f87::emitGetterStub(page + at, reinterpret_cast<uint64_t>(&real), 2, reinterpret_cast<uint64_t>(&pre), reinterpret_cast<uint64_t>(&post));
            FlushInstructionCache(GetCurrentProcess(), page, at);
            State& s = st();
            s = State{};
            using Fn7 = uint64_t (*)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
            const uint64_t r = reinterpret_cast<Fn7>(t1)(0x1111, 0x2222, 0x3333, 0x4444, 0x5555, 0x6666, 0x7777);
            check(s.origCalls == 1 && s.origArgs[0] == 0x1111 && s.origArgs[1] == 0x2222 && s.origArgs[2] == 0x3333 && s.origArgs[3] == 0x4444 && s.origArgs[4] == 0x5555 &&
                      s.origArgs[5] == 0x6666 && s.origArgs[6] == 0x7777 && r == 0xABCD0001ull,
                  "F87p: the vtable trampoline hands the game's function its seven arguments (registers and stack) and returns its value unchanged");
            check(s.recCalls == 1 && s.recSlot == 1 && s.recRegs[3] == 0x1111 && s.recRegs[2] == 0x2222 && s.recRegs[1] == 0x3333 && s.recRegs[0] == 0x4444 && s.recStk[0] == 0x5555 &&
                      s.recStk[1] == 0x6666 && s.recStk[2] == 0x7777 && s.recRet != 0,
                  "F87p: the record gets the slot, the return address and the saved r9, r8, rdx, rcx, and the caller's fifth to seventh arguments from its shadow space's end");
            check(s.aligned, "F87p: the game's function and the callbacks are all entered with the stack 16-byte aligned");
            s = State{};
            using FnF = uint64_t (*)(uint64_t, double, double, double);
            const uint64_t rf = reinterpret_cast<FnF>(t2)(0x77, 1.25, 2.5, 3.75);
            check(s.origCalls == 1 && s.origArgs[0] == 0x77 && s.origD[0] == 1.25 && s.origD[1] == 2.5 && s.origD[2] == 3.75 && rf == 0xABCD0002ull && s.recSlot == 2 && s.aligned,
                  "F87p: xmm0..3 survive the record (which uses every xmm register): a float argument reaches the game's function unchanged");
            s = State{};
            float p = 1.0f;
            using FnG = uint64_t (*)(uint64_t, uint64_t, uint64_t);
            const uint64_t rg = reinterpret_cast<FnG>(g1)(0xAA, 0xBB, reinterpret_cast<uint64_t>(&p));
            const uint64_t pp = reinterpret_cast<uint64_t>(&p);
            check(s.preAt == 1 && s.realAt == 2 && s.postAt == 3 && s.preArgs[0] == 0xAA && s.preArgs[1] == 0xBB && s.preArgs[2] == pp && s.preArgs[3] == 2 && s.realArgs[0] == 0xAA &&
                      s.realArgs[1] == 0xBB && s.realArgs[2] == pp && s.postArgs[0] == 0xAA && s.postArgs[1] == 0xBB && s.postArgs[2] == pp && s.postArgs[3] == 2,
                  "F87p: the getter stub calls pre, the game's getter, then post, each with the registers the game's call had (and the site)");
            check(s.pAtPre == 1.0f && s.pAtPost == 5.0f && p == 5.0f && rg == 0xAA + 0xBB + pp && s.aligned,
                  "F87p: pre sees the point before the game's call, post the point after it; the game's result comes back in rax; everything is entered aligned");
            VirtualFree(page, 0, MEM_RELEASE);
        }
    }
#endif
    // ---- P: the prompt probes: the pins (the 334925 address table), roles, geometry --------------------------------------------------------------------
    {
        bool lens = true;
        for (uint32_t i = 0; i < f87::kPinCount; ++i) lens = lens && f87::kPins[i].len >= 3 && f87::kPins[i].len <= 16 && f87::kPins[i].rva > 0x100000u;
        check(lens && f87::kPinCount == 27, "F87p/F92c/F93c: 27 pins, each 3..16 bytes long");
        check(f87::callTarget(f87::kPins[f87::kPinFocusGetter].rva, f87::kPins[f87::kPinFocusGetter].bytes) == f87::kGetterFocus &&
                  f87::callTarget(f87::kPins[f87::kPinNearbyGetter].rva, f87::kPins[f87::kPinNearbyGetter].bytes) == f87::kGetterNearby &&
                  f87::callTarget(f87::kPins[f87::kPinTargetGetter].rva, f87::kPins[f87::kPinTargetGetter].bytes) == f87::kGetterNearby,
              "F87p: the three getter calls decode to 0x191F140 (focus) and 0x1A43230 (nearby, target)");
        check(f87::kSlotUpdateIndicator == 0x520B2E8u + 0x48u && f87::kSlotSetMarker == 0x520B2E8u + 0x50u && f87::kSlotUpdateMarker == 0x520B2E8u + 0x60u &&
                  f87::kPins[3].bytes[2] == 0x50 && f87::kPins[4].bytes[2] == 0x50 && f87::kPins[5].bytes[2] == 0x50,
              "F87p: the swapped slots are +0x48, +0x50 and +0x60 of the interface at 0x520B2E8, and the three SetMarker calls read call [rax+0x50]");
        check(f87::roleOfReturn(0x1AA9DE4) == f87::kRoleFocus && f87::roleOfReturn(0x1AA9F67) == f87::kRoleFocusHidden && f87::roleOfReturn(0x1A9E27D) == f87::kRoleNearby &&
                  f87::roleOfReturn(0x1AA4D66) == f87::kRoleTarget && f87::roleOfReturn(0x1A9E74D) == f87::kRoleList && f87::roleOfReturn(0x1234) == f87::kRoleOther &&
                  f87::kPins[3].rva + 3 == 0x1AA9DE4u && f87::kPins[4].rva + 3 == 0x1A9E27Du && f87::kPins[5].rva + 3 == 0x1AA4D66u,
              "F87p: the roles come from the return addresses (the pinned calls plus 3)");
        // F92c: the indicator owner 0x1AA9830, its SetMarker's role, its callers and the type each passes, and the stack offset of its own return address
        const f87::Pin& entry = f87::kPins[f87::kPinIndicatorEntry];
        check(entry.rva == 0x1AA9830u && entry.len == 7 && entry.bytes[3] == 0x48 && entry.bytes[4] == 0x83 && entry.bytes[5] == 0xEC &&
                  entry.bytes[6] == f87::kIndicatorOuterReturnOffset,
              "F92c: the indicator owner's pinned prologue is mov rax,rsp; sub rsp,0x68 and the outer return offset is that immediate (no push before the call)");
        check(f87::roleOfReturn(f87::kPins[17].rva + 3) == f87::kRoleIndicator && f87::kPins[17].rva == 0x1AA9928u && f87::kPins[16].rva == 0x1AA98EAu &&
                  f87::roleOfReturn(0x1AA992B) == f87::kRoleIndicator && std::string(f87::roleText(f87::kRoleIndicator)) == "indicator" &&
                  std::string(f87::roleText(f87::kRoleOther)) == "other",
              "F92c: the indicator owner's SetMarker (0x1AA9928) returns to 0x1AA992B, the role 'indicator'; UpdateIndicator is called just before it (0x1AA98EA)");
        bool callers = true;
        const uint32_t callPin[3] = {18, 20, 22}, typePin[3] = {19, 21, 23}, wantType[3] = {1000u, 1001u, 1001u};
        for (int i = 0; i < 3; ++i) {
            const f87::Pin& c = f87::kPins[callPin[i]];
            const f87::Pin& t = f87::kPins[typePin[i]];
            uint32_t imm = 0;
            std::memcpy(&imm, t.bytes + 1, 4);
            callers = callers && c.len == 5 && c.bytes[0] == 0xE8 && f87::callTarget(c.rva, c.bytes) == 0x1AA9830u && t.bytes[0] == 0xBA && imm == wantType[i] &&
                      f87::indicatorTypeOfCaller(c.rva + 5) == wantType[i] && t.rva < c.rva && c.rva - t.rva <= 0x20u;
        }
        check(callers && f87::indicatorTypeOfCaller(0x1234) == 0u,
              "F92c: the three callers call 0x1AA9830 with mov edx,1000 (0x1AA9778) and mov edx,1001 (0x1AAE644, 0x1AB67CB); their return addresses give those types");
        // the focus state: the prompt's hidden path clears only its own key; a focus indicator key holds until a quiet second
        uint64_t s = f87::focusPack(-1, f87::kFocusSrcNone);
        s = f87::focusStep(s, f87::kFocusEvIndicator, 45);
        const bool held = f87::focusKeyOf(s) == 45 && f87::focusSrcOf(s) == f87::kFocusSrcIndicator;
        s = f87::focusStep(s, f87::kFocusEvPromptHidden, -1);
        const bool survives = f87::focusKeyOf(s) == 45;
        s = f87::focusStep(s, f87::kFocusEvQuietSecond, -1);
        const bool cleared = f87::focusKeyOf(s) == -1 && f87::focusSrcOf(s) == f87::kFocusSrcNone;
        uint64_t p = f87::focusStep(f87::focusPack(-1, 0), f87::kFocusEvPrompt, 7);
        const bool prompt = f87::focusKeyOf(p) == 7 && f87::focusKeyOf(f87::focusStep(p, f87::kFocusEvQuietSecond, -1)) == 7 &&
                            f87::focusKeyOf(f87::focusStep(p, f87::kFocusEvPromptHidden, -1)) == -1;
        const bool big = f87::focusKeyOf(f87::focusPack(0x7FFFFFFF, 2)) == 0x7FFFFFFF && f87::focusKeyOf(f87::focusPack(-1, 2)) == -1;
        check(held && survives && cleared && prompt && big,
              "F92c: a focus indicator key survives the focus prompt's hidden path and clears after a quiet second; the prompt's own key clears on its hidden path");
        // F93c: SetMarker's offset argument is read where both pinned callers store it ([rsp+0x30]); the focus prompt's comes from [[hud+0x50]+0x10]
        const f87::Pin& fo = f87::kPins[24];
        const f87::Pin& fs = f87::kPins[25];
        const f87::Pin& no = f87::kPins[26];
        check(fo.rva == 0x1AA9DC1u && no.rva == 0x1A9E259u && fo.bytes[4] == f87::kSetMarkerOffsetPtrOffset && no.bytes[4] == f87::kSetMarkerOffsetPtrOffset &&
                  fo.bytes[0] == 0x48 && fo.bytes[1] == 0x89 && fo.bytes[2] == 0x54 && fs.rva == 0x1AA9D7Bu && fs.bytes[7] == 0x10 &&
                  fo.rva < f87::kPins[3].rva && f87::kPins[3].rva - fo.rva < 0x30u && no.rva < f87::kPins[4].rva && f87::kPins[4].rva - no.rva < 0x30u,
              "F93c: the focus and nearby SetMarker calls store the offset's address at [rsp+0x30] just before the call; the focus offset is read from [[hud+0x50]+0x10]");
        check(!f87::hasDesignOffset(0.0f, 0.0f) && !f87::hasDesignOffset(5e-5f, -5e-5f) && f87::hasDesignOffset(0.031f, -0.022f) && f87::hasDesignOffset(0.0f, 0.5f) &&
                  !f87::hasDesignOffset(std::numeric_limits<float>::quiet_NaN(), 1.0f),
              "F93c: a designed offset is a finite offset argument with a component over 1e-4 (zero for the dots and waypoints)");
    }
    {
        // F93c: the designed-offset marker takes the nearest reference's correction, this eye-frame first, then the one before; none older
        f87::RefSet rs;
        double d[2] = {}, dist = 0.0;
        uint32_t age = 9;
        const bool none = !rs.nearest(100, std::array<double, 2>{0.1, 0.1}.data(), 1, d, &dist, &age);
        const double a1[2] = {0.20, 0.05}, d1[2] = {-0.17, 0.13}, a2[2] = {0.60, -0.30}, d2[2] = {-0.02, 0.01}, a3[2] = {0.21, 0.06}, d3[2] = {-0.16, 0.12};
        rs.add(99, a3, d3);    // the frame before, nearest
        rs.add(100, a1, d1);   // this frame, near
        rs.add(100, a2, d2);   // this frame, far
        const double at[2] = {0.22, 0.06};
        const bool ok1 = rs.nearest(100, at, 1, d, &dist, &age) && age == 0 && d[0] == d1[0] && d[1] == d1[1] && std::fabs(dist - std::sqrt(0.02 * 0.02 + 0.01 * 0.01)) < 1e-12;
        f87::RefSet only;
        only.add(99, a3, d3);
        const bool ok2 = only.nearest(100, at, 1, d, &dist, &age) && age == 1 && d[0] == d3[0] && !only.nearest(101, at, 1, d, &dist, &age);
        f87::RefSet wrap;
        for (int i = 0; i < f87::kRefPerEye + 3; ++i) {
            const double ai[2] = {0.01 * i, 0.0}, di[2] = {static_cast<double>(i), 0.0};
            wrap.add(200, ai, di);
        }
        const double at0[2] = {0.0, 0.0};
        const bool ok3 = wrap.nearest(200, at0, 1, d, &dist, &age) && d[0] == 3.0;   // entries 0..2 were overwritten by 16..18: entry 3 is nearest
        check(none && ok1 && ok2 && ok3,
              "F93c: the reference is the nearest in this eye-frame before any older one, one frame old at most, the ring keeps the newest 16");
        // the patch the label gets: its own record through the unpatched rows plus the reference's correction (its layout kept), not P plus the mesh offset
        const double nM0[2] = {0.39, -0.135}, nWu[2] = {0.218, -0.002};
        const double T[2] = {nM0[0] + d1[0], nM0[1] + d1[1]};
        const double dropped[2] = {(nWu[0] - nM0[0]) - d1[0], (nWu[1] - nM0[1]) - d1[1]};
        check(std::fabs(T[0] - 0.22) < 1e-12 && std::fabs(T[1] - (-0.005)) < 1e-12 && std::fabs(dropped[0] - (-0.002)) < 1e-12 && std::fabs(dropped[1] - 0.003) < 1e-12,
              "F93c: the label's target is its record plus the dot's correction; the aim at P would have dropped the game's (+0.17, -0.13) NDC layout offset");
    }
    {
        f87::Box box;
        box.mn[0] = box.mn[1] = box.mn[2] = -0.5;
        box.mx[0] = box.mx[1] = box.mx[2] = 0.5;
        const double t[3] = {10.0, 2.0, -5.0};
        putMat(box.world, I3, t);
        const double cam[3] = {10.0, 2.0, -3.0};   // 2 m in front, on +z
        const f87::BoxFace f = f87::boxFaceFacing(box, cam);
        const double P[3] = {10.0, 2.0, -5.0};   // the box's centre (the bounding-box branch)
        double Pp[3];
        check(f.ok && f.axis == 2 && f.sign > 0.0 && std::fabs(f.h - 0.5) < 1e-12 && f87::boxFrontPoint(box, f, P, Pp) && std::fabs(Pp[2] - (-4.5)) < 1e-12 && std::fabs(Pp[0] - 10.0) < 1e-12,
              "F87p: a box facing the camera on +z gives the +z face; the centre carried along the normal lands on the face (half an extent in front)");
        // a node 5 cm inside the face, off centre: P' keeps the lateral place and lands on the face plane
        const double Pn[3] = {10.2, 2.1, -4.55};
        check(f87::boxFrontPoint(box, f, Pn, Pp) && std::fabs(Pp[0] - 10.2) < 1e-12 && std::fabs(Pp[1] - 2.1) < 1e-12 && std::fabs(Pp[2] + 4.5) < 1e-12,
              "F87p: a node 5 cm behind the face and off centre moves 5 cm along the normal onto the face, nothing laterally");
        // a turned and scaled box: the normal is the model's axis in the world, the extent includes the scale
        double R[3][3];
        const double a = 30.0 * f85::kPi / 180.0;
        const double Rz[3][3] = {{0.5 * std::cos(a), 0.5 * std::sin(a), 0}, {-0.5 * std::sin(a), 0.5 * std::cos(a), 0}, {0, 0, 0.5}};
        std::memcpy(R, Rz, sizeof(R));
        f87::Box b2;
        b2.mn[0] = b2.mn[1] = b2.mn[2] = -1.0;
        b2.mx[0] = b2.mx[1] = b2.mx[2] = 1.0;
        putMat(b2.world, R, t);
        const double cam2[3] = {10.0 + 3.0 * std::cos(a), 2.0 + 3.0 * std::sin(a), -5.0};   // along the box's +x axis
        const f87::BoxFace f2 = f87::boxFaceFacing(b2, cam2);
        double C2[3], P2[3];
        f87::boxCentre(b2, C2);
        const double Pq[3] = {C2[0] - 0.05 * f2.n[0], C2[1] - 0.05 * f2.n[1], C2[2] - 0.05 * f2.n[2]};
        const bool ok2 = f87::boxFrontPoint(b2, f2, Pq, P2);
        const double onPlane = (P2[0] - C2[0]) * f2.n[0] + (P2[1] - C2[1]) * f2.n[1] + (P2[2] - C2[2]) * f2.n[2];
        const double moved = (P2[0] - Pq[0]) * f2.n[0] + (P2[1] - Pq[1]) * f2.n[1] + (P2[2] - Pq[2]) * f2.n[2];
        check(f2.ok && f2.axis == 0 && ok2 && std::fabs(f2.h - 0.5) < 1e-6 && std::fabs(onPlane - 0.5) < 1e-6 && std::fabs(moved - 0.55) < 1e-6,
              "F87p: a turned, half-scale box: the face is the model's x axis in the world, 0.5 m (the scale) from the centre, and the point moves 0.55 m along it");
        const float inside[3] = {0.0f, 0.0f, 0.01f}, near2[3] = {1e-7f, -1e-7f, 0.01f}, off[3] = {0.0f, 0.0f, 0.02f};
        check(f87::cameraLocal(inside) && f87::cameraLocal(near2) && !f87::cameraLocal(off), "F87p: the label's record is camera-local (0, 0, 0.01) within 1e-6");
        f87::LabelCensus lc;
        char txt[96];
        lc.text(txt, sizeof(txt));
        const bool none = std::strcmp(txt, "none") == 0;
        for (int i = 0; i < 2; ++i) lc.index(3600);
        for (int i = 0; i < 2; ++i) lc.index(624);
        lc.text(txt, sizeof(txt));
        check(none && std::strcmp(txt, "3600:2 624:2") == 0, "F87p: the label's index counts are listed as count:draws (3600 and 624 are the two the F85 dump showed), none when there are none");
        check(f87::kLabelVs == 0xB10B032BDFD46700ull && f87::kLabelPs == 0xDB899F4BD577F2E5ull, "F87p: the label's shaders are VS B10B032BDFD46700 and PS DB899F4BD577F2E5");
    }
}

// ---- F88 ---------------------------------------------------------------------------------------------------------------------------------------------
// The pure functions of src\d3d11\native_on_foot_f88.h: the weapon eye offset's weight, its seated image, the pin's share and the hold's wait, on the plant the production
// code uses: the camera moves by adsCamShiftWorld of the seated shift (the character's rows), and the pin takes adsUnlockedCam of the camera less the shift it ignores.
template <class Check>
inline void run88(Check&& check) {
    namespace f87 = edvr::nofoot::f87;
    namespace f88 = edvr::nofoot::f88;
    namespace nf = edvr::nofoot;
    const double dt = 1.0 / 90.0;
    struct Body3 {
        float f[9];
        double d[3][3];
    };
    // a character turned by `a` about the up axis: the rows (right, up, forward) in the world
    const auto rowsOf = [](double a) {
        Body3 b;
        const double c = std::cos(a), s = std::sin(a);
        const double r[3][3] = {{c, 0.0, -s}, {0.0, 1.0, 0.0}, {s, 0.0, c}};
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j) {
                b.d[i][j] = r[i][j];
                b.f[3 * i + j] = float(r[i][j]);
            }
        return b;
    };
    const double yaws[5] = {0.0, 0.6, 1.5707963267948966, 3.14159, -1.3};
    f88::Keys keys;
    keys.rightCm = 4.0;
    keys.upCm = 3.0;
    keys.forwardCm = 10.0;
    const auto isPlusZero = [](const double v[3]) {
        return v[0] == 0.0 && v[1] == 0.0 && v[2] == 0.0 && !std::signbit(v[0]) && !std::signbit(v[1]) && !std::signbit(v[2]);
    };
    // ---- the keys ----------------------------------------------------------------------------------------------------------------------------------
    {
        bool kn = false, ks = false, kx = true, ke = true, kz = true;
        const int follow = f88::bodyFromText("follow", &kn), stay = f88::bodyFromText("stay", &ks), other = f88::bodyFromText("Stay", &kx);
        const int empty = f88::bodyFromText("", &ke), none = f88::bodyFromText(nullptr, &kz);
        check(f88::clampCm(30.0, f88::kForwardMinCm, f88::kForwardMaxCm) == 25.0 && f88::clampCm(-40.0, f88::kForwardMinCm, f88::kForwardMaxCm) == -10.0 &&
                  f88::clampCm(20.0, f88::kUpMinCm, f88::kUpMaxCm) == 15.0 && f88::clampCm(-11.0, f88::kUpMinCm, f88::kUpMaxCm) == -10.0 &&
                  f88::clampCm(12.0, f88::kRightMinCm, f88::kRightMaxCm) == 10.0 && f88::clampCm(-12.0, f88::kRightMinCm, f88::kRightMaxCm) == -10.0 &&
                  f88::clampCm(2.5, f88::kRightMinCm, f88::kRightMaxCm) == 2.5 &&
                  f88::clampCm(std::numeric_limits<double>::quiet_NaN(), -10.0, 25.0) == 0.0 && f88::clampCm(std::numeric_limits<double>::infinity(), -10.0, 25.0) == 0.0,
              "F88: the keys read to forward -10..25, up -10..15 and right -10..10 cm, and a value that is not finite reads 0");
        check(follow == f88::kFollow && kn && stay == f88::kStay && ks && other == f88::kFollow && !kx && empty == f88::kFollow && !ke && none == f88::kFollow && !kz &&
                  std::strcmp(f88::bodyName(f88::kStay), "stay") == 0 && std::strcmp(f88::bodyName(f88::kFollow), "follow") == 0 && f88::Keys{}.body == f88::kFollow && !f88::Keys{}.any(),
              "F88: the body key reads follow or stay, anything else reads follow (and says so), and a Keys with nothing set is 0, 0, 0 and follow (F89's built-in values are checked in run89)");
    }
    // ---- the defaults reproduce F87 exactly ----------------------------------------------------------------------------------------------------------
    {
        const double ws[4] = {0.0, 0.37, 1.0, 1.7};
        bool zeros = true;
        for (const double w : ws) {
            const f88::Out o = f88::make(f88::Keys{}, w);   // every key 0
            zeros = zeros && isPlusZero(o.seated) && isPlusZero(o.ignore) && isPlusZero(o.bodyM);
            f88::Keys stayKeys = keys;
            stayKeys.body = f88::kStay;
            const f88::Out o0 = f88::make(stayKeys, 0.0);   // keys set, nothing drawn
            zeros = zeros && isPlusZero(o0.seated) && isPlusZero(o0.ignore) && isPlusZero(o0.bodyM);
        }
        const double sumF87 = f87::eyeAlignY(0.004, -0.011, 0.0123, -0.0257, 0.031);
        double sumF88 = sumF87;
        sumF88 += f88::make(f88::Keys{}, 1.0).seated[1];
        double sumFollow = sumF87;
        sumFollow += f88::make(f88::Keys{}, 0.5).seated[1];
        check(zeros && std::memcmp(&sumF87, &sumF88, sizeof(double)) == 0 && std::memcmp(&sumF87, &sumFollow, sizeof(double)) == 0,
              "F88: with every key 0, or with nothing drawn, the seated shift, the pin's share and w O are exact positive zeros and the eye alignment's sum is F87's bit for bit");
    }
    // ---- the weight --------------------------------------------------------------------------------------------------------------------------------
    {
        check(f88::weightOn(true, true) && !f88::weightOn(true, false) && !f88::weightOn(false, true) && !f88::weightOn(false, false),
              "F88: w is on only with a weapon or tool drawn in the pin's scope (F89: aiming down sights is not an input; unarmed and the cockpit are off)");
        f88::Weight w;
        double w11 = 0.0, w22 = 0.0, w23 = 0.0;
        for (int i = 1; i <= 23; ++i) {
            w.step(true, dt);
            if (i == 11) w11 = w.w;
            if (i == 22) w22 = w.w;
            if (i == 23) w23 = w.w;
        }
        check(std::fabs(w11 - 11.0 * dt / f88::kRampS) < 1e-12 && w22 > 0.97 && w22 < 1.0 && w23 == 1.0 && f88::kRampS == 0.25,
              "F88: w ramps 0 -> 1 over 0.25 s: 22 ticks at 90 Hz are short of 1, 23 reach it, and the ramp is linear");
        // draw, ADS in, ADS out, holster: w never steps more than a tick of the ramp, is 0 unarmed, 1 with a weapon drawn, and stays 1 through the ADS (F89)
        f88::Weight v;
        double maxStep = 0.0, prev = 0.0;
        const auto phase = [&](bool pin, bool armed, int ticks) {
            for (int i = 0; i < ticks; ++i) {
                v.step(f88::weightOn(pin, armed), dt);
                maxStep = std::max(maxStep, std::fabs(v.w - prev));
                prev = v.w;
            }
            return v.w;
        };
        const double afterUnarmed = phase(true, false, 60), afterDraw = phase(true, true, 30), afterAds = phase(true, true, 30);   // the sights up: nothing changes
        const double afterAdsOut = phase(true, true, 30), midHolster = phase(true, false, 11), afterHolster = phase(true, false, 30);
        check(afterUnarmed == 0.0 && afterDraw == 1.0 && afterAds == 1.0 && afterAdsOut == 1.0 && midHolster > 0.4 && midHolster < 0.6 && afterHolster == 0.0 &&
                  maxStep <= dt / f88::kRampS + 1e-12,
              "F88: w is 0 unarmed, 1 with a weapon drawn at the hip and still 1 through ADS in and out (F89), 0 holstered, ramped on each change and never more than a tick of the ramp apart");
    }
    // ---- the offset rotates with the body's yaw -------------------------------------------------------------------------------------------------------
    {
        bool axesOk = true, headingOk = true;
        double heading0 = 0.0;
        for (int k = 0; k < 5; ++k) {
            const Body3 b = rowsOf(yaws[k]);
            const f88::Out o = f88::make(keys, 1.0);
            double world[3];
            nf::adsCamShiftWorld(o.seated, b.f, world);   // how far the camera moves in the world for the shift the eye alignment gets
            const double want[3] = {0.04, 0.03, 0.10};
            for (int a = 0; a < 3; ++a) axesOk = axesOk && std::fabs(world[0] * b.d[a][0] + world[1] * b.d[a][1] + world[2] * b.d[a][2] - want[a]) < 1e-7;   // right, up, forward (the hook's rows are floats)
            const double heading = std::atan2(world[0], world[2]);
            if (k == 0) heading0 = heading;
            double turn = heading - heading0 - yaws[k];
            turn = std::fabs(std::remainder(turn, 6.283185307179586));
            headingOk = headingOk && turn < 1e-5;
        }
        const Body3 yaw90 = rowsOf(1.5707963267948966);
        double w90[3];
        nf::adsCamShiftWorld(f88::make(keys, 1.0).seated, yaw90.f, w90);
        check(axesOk && headingOk && std::fabs(w90[0] - 0.10) < 1e-6 && std::fabs(w90[2] + 0.04) < 1e-6 && std::fabs(w90[1] - 0.03) < 1e-9,
              "F88: O is in the character's axes: for any body yaw the camera moves by right 4, up 3 and forward 10 cm along the body's own right, up and forward, and its heading turns with the yaw (forward 10 cm at 90 degrees is 10 cm along world x)");
    }
    // ---- the hold: its input is the head's posture, never O ----------------------------------------------------------------------------------------------
    {
        bool same = true;
        const double ups[4] = {0.0, 3.0, -10.0, 15.0};
        for (const double up : ups)
            for (const double w : {0.0, 0.3, 1.0}) {
                f88::Keys k = keys;
                k.upCm = up;
                const f88::Out o = f88::make(k, w);
                const double drop = -0.02, headRaw = -0.07;   // the head's seated height without any eye alignment, and the neck pivot's drop
                const double other = f87::eyeAlignY(0.004, -0.011, 0.0123, -0.0257, 0.031);
                const double total = other + o.seated[1];
                const double renderY = headRaw - total;   // the runtime moves the origin up by the eye alignment: the head reads that much lower
                const double devWith = f87::devOf(renderY, total, drop);
                const double devNone = f87::devOf(headRaw - other, other, drop);
                same = same && std::fabs(devWith - devNone) < 1e-12 && std::fabs(devWith - (headRaw - drop)) < 1e-12;
            }
        check(same, "F88: the hold's input (the render head plus the eye alignment published for it) is the head's posture whatever w O is: it equals the head's height less the pivot's drop with and without O");
        // The runtime applies a published value `lag` ticks late: the render head then misses O's change for that long. The hold waits while O's up changes, and 0.3 s after.
        const int lag = 2;
        bool heldAll = true, covered = true;
        double xFrozenMaxDev = 0.0, xFreeMaxDev = 0.0;
        for (int pass = 0; pass < 2; ++pass) {   // pass 0: the hold waits as the production code does; pass 1: it does not (the control)
            f88::Weight w;
            f88::Settle settle;
            f87::HeightHold h;
            f87::HoldIn in;
            in.scope = true;
            in.devValid = true;
            in.dt = dt;
            std::vector<double> pub;   // the up of w O published at each tick
            double x0 = 0.0;
            for (int i = 0; i < 90 * 8; ++i) {
                const bool armed = i >= 90 * 4;   // a rifle drawn at 4 s
                w.step(f88::weightOn(true, armed), dt);
                f88::Keys k = keys;
                k.upCm = 10.0;
                const f88::Out o = f88::make(k, w.w);
                const bool settling = settle.step(o.seated[1], dt);
                pub.push_back(o.seated[1]);
                const double seen = i - 1 - lag >= 0 ? pub[size_t(i - 1 - lag)] : 0.0, publishedLast = i >= 1 ? pub[size_t(i - 1)] : 0.0;
                const double errO = publishedLast - seen;   // the published value less the one the render head carries
                const double headRaw = -0.07;
                in.devM = headRaw + errO;
                in.frozen = pass == 0 && settling;
                if (pass == 0 && std::fabs(errO) > 1e-12) covered = covered && settling;
                const f87::HoldOut out = h.step(in);
                if (i == 90 * 4 - 1) x0 = out.sustainedM;
                if (i >= 90 * 4) {
                    const double dev = std::fabs(out.sustainedM - x0);
                    if (pass == 0 && i < 90 * 4 + 40) {
                        xFrozenMaxDev = std::max(xFrozenMaxDev, dev);   // during the ramp and the wait that follows (23 ticks + 27)
                        heldAll = heldAll && out.sustainedM == x0;
                    }
                    if (pass == 1 && i < 90 * 4 + 40) xFreeMaxDev = std::max(xFreeMaxDev, dev);
                }
            }
        }
        check(heldAll && covered && xFrozenMaxDev == 0.0 && xFreeMaxDev > 0.001,
              "F88: while O's up changes the render head lags the published eye alignment by two ticks; the hold waits for every tick with that difference (its sustained part does not move) and without the wait it would move by more than a millimetre");
        // O with no up never holds the hold
        f88::Keys flat = keys;
        flat.upCm = 0.0;
        f88::Weight w;
        f88::Settle settle;
        bool ever = false;
        for (int i = 0; i < 90 * 2; ++i) {
            w.step(true, dt);
            ever = ever || settle.step(f88::make(flat, w.w).seated[1], dt);
        }
        check(!ever, "F88: with no up offset the hold never waits, whatever the weight: the hold is F87's");
    }
    // ---- the pin ---------------------------------------------------------------------------------------------------------------------------------
    {
        bool stayOk = true, followOk = true, followUpOnly = true, ignoreShape = true;
        const double head[3] = {3.0, 1.6, -2.0};
        const double sAds[3] = {0.012, -0.007, 0.02};   // an ADS shift beside it: the two add
        const auto pinOf = [&](const Body3& b, const double total[3], const double ignored[3]) {
            double shift[3], cam[3];
            nf::adsCamShiftWorld(total, b.f, shift);
            for (int i = 0; i < 3; ++i) cam[i] = head[i] + shift[i];
            const float camF[3] = {float(cam[0]), float(cam[1]), float(cam[2])};   // the hook holds floats
            float out[3];
            nf::adsUnlockedCam(camF, ignored, b.f, out);
            return std::array<double, 3>{double(out[0]), double(out[1]), double(out[2])};
        };
        for (int k = 0; k < 5; ++k) {
            const Body3 b = rowsOf(yaws[k]);
            const auto pin0 = pinOf(b, sAds, sAds);   // no O
            for (const double w : {0.4, 1.0}) {
                for (int mode = 0; mode < 2; ++mode) {
                    f88::Keys kk = keys;
                    kk.body = mode == 0 ? f88::kStay : f88::kFollow;
                    const f88::Out o = f88::make(kk, w);
                    double total[3], ignored[3];
                    for (int i = 0; i < 3; ++i) {
                        total[i] = sAds[i] + o.seated[i];     // the eye alignment, as the game moves the camera
                        ignored[i] = sAds[i] + o.ignore[i];   // what alignTick publishes as S
                    }
                    const auto pin1 = pinOf(b, total, ignored);
                    double d[3] = {pin1[0] - pin0[0], pin1[1] - pin0[1], pin1[2] - pin0[2]};
                    double dBody[3];
                    for (int a = 0; a < 3; ++a) dBody[a] = d[0] * b.d[a][0] + d[1] * b.d[a][1] + d[2] * b.d[a][2];
                    if (mode == 0) {   // stay: the pin demand does not move
                        for (int a = 0; a < 3; ++a) stayOk = stayOk && std::fabs(d[a]) < 4e-6;
                        for (int a = 0; a < 3; ++a) ignoreShape = ignoreShape && o.ignore[a] == o.seated[a];
                    } else {           // follow: the horizontal part moves it, the vertical part does not
                        followOk = followOk && std::fabs(dBody[0] - w * 0.04) < 4e-6 && std::fabs(dBody[1]) < 4e-6 && std::fabs(dBody[2] - w * 0.10) < 4e-6;
                        ignoreShape = ignoreShape && o.ignore[0] == 0.0 && o.ignore[2] == 0.0 && o.ignore[1] == o.seated[1];
                    }
                }
            }
            // follow with an up offset alone: the pin does not move at all
            f88::Keys upKeys;
            upKeys.upCm = 12.0;
            const f88::Out ou = f88::make(upKeys, 1.0);
            double total[3], ignored[3];
            for (int i = 0; i < 3; ++i) {
                total[i] = sAds[i] + ou.seated[i];
                ignored[i] = sAds[i] + ou.ignore[i];
            }
            const auto pin2 = pinOf(b, total, ignored);
            for (int a = 0; a < 3; ++a) followUpOnly = followUpOnly && std::fabs(pin2[a] - pin0[a]) < 4e-6;
        }
        check(stayOk && ignoreShape, "F88: with body stay the pin ignores all of w O: its demand (the camera less the shift S, with the ADS shift beside it) is the same with and without O, for any yaw and weight");
        check(followOk && followUpOnly, "F88: with body follow the pin demand moves by the horizontal part of w O (w x right 4 cm, w x forward 10 cm in the body's axes) and never by the up part");
        // The eye over the weapon's root, as the F87h line reads it: it moves by w O in the character's axes
        bool lineOk = true;
        for (int k = 0; k < 5; ++k) {
            const Body3 b = rowsOf(yaws[k]);
            const double froot[3] = {5.0, 1.0, -3.0}, e0[3] = {0.033, -0.010, -0.039};   // the F87 flight: right +3.3, up -1.0, forward -3.9 cm
            double eye0[3], eye1[3], shift[3];
            for (int i = 0; i < 3; ++i) eye0[i] = froot[i] + e0[0] * b.d[0][i] + e0[1] * b.d[1][i] + e0[2] * b.d[2][i];
            const f88::Out o = f88::make(keys, 0.6);
            nf::adsCamShiftWorld(o.seated, b.f, shift);
            for (int i = 0; i < 3; ++i) eye1[i] = eye0[i] + shift[i];
            f87::EyeRootSecond er;
            er.add(eye1, froot, b.d);
            lineOk = lineOk && std::fabs(er.last[0] - (e0[0] + 0.6 * 0.04)) < 1e-9 && std::fabs(er.last[1] - (e0[1] + 0.6 * 0.03)) < 1e-9 && std::fabs(er.last[2] - (e0[2] + 0.6 * 0.10)) < 1e-9;
        }
        check(lineOk, "F88: the F87h eye-to-weapon-root readout (the right eye less F_root in the character's axes) moves by w O: 0.6 x (4, 3, 10) cm from (+3.3, -1.0, -3.9) at any yaw");
    }
    // ---- no jump at a draw, a holster, or ADS in and out -------------------------------------------------------------------------------------------------
    {
        f88::Keys big;
        big.rightCm = -10.0;
        big.upCm = 15.0;
        big.forwardCm = 25.0;
        f88::Weight w;
        double prev[3] = {}, maxJump = 0.0, atRest[3] = {};
        const auto phase = [&](bool armed, bool ads, int ticks) {
            (void)ads;   // F89: aiming down sights is not an input to w; the phases keep the sequence the F88 check had
            for (int i = 0; i < ticks; ++i) {
                w.step(f88::weightOn(true, armed), dt);
                const f88::Out o = f88::make(big, w.w);
                for (int a = 0; a < 3; ++a) {
                    maxJump = std::max(maxJump, std::fabs(o.seated[a] - prev[a]));
                    prev[a] = o.seated[a];
                    atRest[a] = o.seated[a];
                }
            }
        };
        phase(false, false, 30);
        phase(true, false, 30);
        const double drawn[3] = {atRest[0], atRest[1], atRest[2]};
        phase(true, true, 30);
        const double inAds[3] = {atRest[0], atRest[1], atRest[2]};
        phase(true, false, 30);
        const double outAds[3] = {atRest[0], atRest[1], atRest[2]};
        phase(false, false, 30);
        const double holstered[3] = {atRest[0], atRest[1], atRest[2]};
        check(maxJump <= 0.25 * dt / f88::kRampS + 1e-12 && std::fabs(drawn[0] - 0.10) < 1e-12 && std::fabs(drawn[1] + 0.15) < 1e-12 && std::fabs(drawn[2] - 0.25) < 1e-12 &&
                  std::fabs(inAds[0] - drawn[0]) < 1e-12 && std::fabs(inAds[1] - drawn[1]) < 1e-12 && std::fabs(inAds[2] - drawn[2]) < 1e-12 &&
                  std::fabs(outAds[0] - drawn[0]) < 1e-12 && isPlusZero(holstered),
              "F88: at the largest offsets (right -10, up 15, forward 25 cm) the seated shift steps by at most 1.2 cm a tick through a draw and a holster, is (+10, -15, +25) cm drawn at the hip, the same in ADS (F89) and exactly 0 holstered");
    }
}

}  // namespace f85test
