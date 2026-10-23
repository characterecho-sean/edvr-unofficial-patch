// F85 checks (the pin-step trace, the weapon UI pinned to the weapon, the reticle's hit depth): the pure functions of
// src\d3d11\native_on_foot_f85.h on synthetic cameras. Header only so that native_frame_test and a desk driver run the same lines.
#pragma once

#include "../../src/d3d11/native_on_foot_f85.h"
#include "../../src/d3d11/native_on_foot_core.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

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
            f85::StepTracker t;
            f85::StepFrame f0 = mkFrame(0, 0.0), f1 = mkFrame(1, 0.0);
            f1.camBody[2] = 0.03f;
            t.step(f0, freq);
            const f85::StepResult r = t.step(f1, freq);
            check(r.eventStart && (r.trig & f85::kTrigCamMove) && f1.camMoveCm > 2.9f && f1.camMoveCm < 3.1f, "F85a: the camera moving 3 cm in the model in a frame starts an event");
            f85::StepTracker u;
            f85::StepFrame g0 = mkFrame(0, 0.0), g1 = mkFrame(1, 0.0);
            g1.camBody[2] = 0.015f;
            u.step(g0, freq);
            const f85::StepResult q = u.step(g1, freq);
            check(!q.eventStart && !(q.trig & f85::kTrigCamMove), "F85a: 1.5 cm in a frame is under the camera trigger");
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

}  // namespace f85test
