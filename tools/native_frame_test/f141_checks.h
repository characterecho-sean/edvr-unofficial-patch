// F141 checks (native on-foot VR): the game's ADS sight point (the ADS eye's source since F143) and the weapon readout's anchor. Header only, run by native_frame_test (run141).
// Convention: matrices are row-vector (rows x, y, z, position; a point p goes to p M), as the game's and EDVR's; the bend's E has rows x, y, z = forward and the
// position; the aim dir is in the eye's own axes, (0, 0, 1) straight down E's forward; S's offset from P14's line is in the FP root's unit rows (right, up, forward).
#pragma once

#include "../../src/d3d11/native_on_foot_f141.h"

#include <cmath>
#include <cstdint>
#include <cstring>

namespace f141test {

namespace f141 = edvr::nofoot::f141;

inline bool close141(double a, double b, double tol) { return std::fabs(a - b) <= tol; }

// A rigid row-vector matrix: yaw about y, then pitch about x (degrees), then the position.
inline void rigid(double yawDeg, double pitchDeg, const double pos[3], float out[16]) {
    const double y = yawDeg / f141::kDeg, p = pitchDeg / f141::kDeg;
    const double cy = std::cos(y), sy = std::sin(y), cp = std::cos(p), sp = std::sin(p);
    // rows: the yaw's rows, each pitched (row-vector: R = Rpitch x Ryaw)
    const double ry[9] = {cy, 0, -sy, 0, 1, 0, sy, 0, cy};
    const double rp[9] = {1, 0, 0, 0, cp, sp, 0, -sp, cp};
    double r[9];
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) r[3 * i + j] = rp[3 * i] * ry[j] + rp[3 * i + 1] * ry[3 + j] + rp[3 * i + 2] * ry[6 + j];
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) out[4 * i + j] = static_cast<float>(r[3 * i + j]);
        out[4 * i + 3] = 0.0f;
        out[12 + i] = static_cast<float>(pos[i]);
    }
    out[15] = 1.0f;
}
inline void toFloat(const double in[16], float out[16]) {
    for (int i = 0; i < 16; ++i) out[i] = static_cast<float>(in[i]);
}

template <class Check>
inline void run141(Check&& check) {
    // ---- the prologue constants: the bytes the hooks verify, and the stolen lengths on instruction boundaries (read from the 334925 exe with capstone)
    check(f141::kBendPrologue[0] == 0x4C && f141::kBendPrologue[1] == 0x8B && f141::kBendPrologue[2] == 0xDC && f141::kBendPrologue[3] == 0x56 &&
              f141::kBendPrologue[4] == 0x48 && f141::kBendPrologue[5] == 0x81 && f141::kBendPrologue[6] == 0xEC && f141::kBendPrologue[7] == 0xA0 && f141::kBendStolen == 11,
          "F141: the bend's prologue is mov r11,rsp; push rsi; sub rsp,0xA0 and the hook takes those 11 bytes");
    check(f141::kAimPrologue[0] == 0x4C && f141::kAimPrologue[3] == 0x49 && f141::kAimPrologue[4] == 0x89 && f141::kAimPrologue[5] == 0x5B && f141::kAimPrologue[6] == 0x18 &&
              f141::kAimStolen == 7 && sizeof(f141::kAimPrologue) > f141::kAimStolen && sizeof(f141::kBendPrologue) > f141::kBendStolen,
          "F141: the aim's prologue is mov r11,rsp; mov [r11+0x18],rbx (7 bytes), and both verified runs go past the stolen bytes");
    check(f141::kIfaceMatrix == 0x2C0 - f141::kIfaceToEye && f141::kIfaceAim == 0x300 - f141::kIfaceToEye,
          "F141: the eye matrix and aim dir offsets from the interface are eye+0x2C0 and eye+0x300 less the interface's 0x58");

    // ---- the bend, one call: a level eye at (0, 1.7, 0) looking down +z; the sight 1.1 cm over its line, 35 cm ahead
    f141::BendIn in;
    const double eyePos[3] = {0.0, 1.7, 0.0};
    rigid(0.0, 0.0, eyePos, in.E);
    in.eOk = in.sOk = in.wOk = in.okRead = in.ok = in.aimInOk = in.aimOutOk = true;
    in.w = 1.0f;
    in.S[0] = 0.0f; in.S[1] = 1.711f; in.S[2] = 0.35f;
    const double ray[3] = {0.0, 0.011, 0.35};
    const double rayLen = std::sqrt(ray[1] * ray[1] + ray[2] * ray[2]);
    in.aimIn[2] = 1.0f;
    in.aimOut[0] = 0.0f; in.aimOut[1] = static_cast<float>(ray[1] / rayLen); in.aimOut[2] = static_cast<float>(ray[2] / rayLen);
    f141::BendOut b = f141::bend(in);
    const double rayDeg = std::atan2(0.011, 0.35) * f141::kDeg;
    check(b.geo && b.applied && close141(b.dist, rayLen, 1e-5) && close141(b.eyeUp, 0.011, 1e-5) && close141(b.eyeRight, 0.0, 1e-6) && close141(b.eyeAlong, 0.35, 1e-5) &&
              close141(b.rayDeg, rayDeg, 1e-3) && b.dotZ > 0.99,
          "F141: the sight 1.1 cm over a level eye's line 35 cm ahead: the ray 1.80 deg up, the bend applied at w = 1.0 with the handle valid");
    check(close141(b.aimInDeg, 0.0, 1e-6) && close141(b.aimOutDeg, rayDeg, 1e-3) && close141(b.aimOutVsRayDeg, 0.0, 1e-3),
          "F141: the aim dir straight on entry and along the sight ray on exit (0 deg from it, the ray's angle from E.z)");
    {
        f141::BendIn x = in;
        x.w = 0.999f;
        check(!f141::bend(x).applied && f141::bend(x).geo, "F141: w below 1.0 is not applied (no partial blend), the geometry is still read (candidate a)");
        x = in;
        x.ok = false;
        check(!f141::bend(x).applied, "F141: the sight handle invalid is not applied (candidate b)");
        x = in;
        x.okRead = false;
        check(!f141::bend(x).applied, "F141: the handle's validity not read is not applied");
        x = in;
        x.wOk = false;
        check(!f141::bend(x).applied, "F141: w not read is not applied");
        x = in;
        x.S[2] = -0.35f;
        check(!f141::bend(x).applied && f141::bend(x).dotZ < 0.0, "F141: a sight behind the eye keeps E.z (the game's dot > 0 test)");
        x = in;
        x.S[0] = static_cast<float>(eyePos[0]); x.S[1] = static_cast<float>(eyePos[1]); x.S[2] = static_cast<float>(eyePos[2]);
        check(!f141::bend(x).geo && !f141::bend(x).applied, "F141: a sight on the eye's position has no ray");
        x = in;
        x.sOk = false;
        const f141::BendOut bx = f141::bend(x);
        check(!bx.geo && close141(bx.aimOutDeg, rayDeg, 1e-3), "F141: S not read: no geometry, the aim dir angles still read");
        check(bx.why == f141::kWhySNotRead, "F141: S not read at a fraction of 1.0 is the instrument's: S not read");
        // F147: S is asked for only at w = 1.0, so a sample under it has none and says why (not a rejection)
        x = in;
        x.sOk = false;
        x.w = 0.4f;
        check(f141::bend(x).why == f141::kWhyNotFull && !f141::bend(x).geo, "F147: no S under a fraction of 1.0: kWhyNotFull");
        x.ok = false;
        check(f141::bend(x).why == f141::kWhyHandle, "F147: the game's invalid handle is named before the fraction");
        x.ok = true;
        x.wOk = false;
        check(f141::bend(x).why == f141::kWhySNotRead, "F147: a fraction that was not read is not 'under 1.0': S not read");
        check(std::strstr(f141::whyText(f141::kWhyNotFull), "under 1.0") != nullptr && f141::kWhyCount == 9, "F147: the new reason has its text");
        x = in;
        x.aimOut[0] = 0.0f; x.aimOut[1] = 0.0f; x.aimOut[2] = 1.0f;
        check(close141(f141::bend(x).aimOutVsRayDeg, rayDeg, 1e-3), "F141: an unbent exit aim is the ray's angle off the sight ray");
    }
    {   // a turned eye (yaw 90 deg, pitch 10 deg): the eye-axis offsets and the exit's world dir follow E's rows
        f141::BendIn t;
        const double pos[3] = {3.0, 1.2, -4.0};
        rigid(90.0, 10.0, pos, t.E);
        double S[3];
        const double local[3] = {0.004, -0.02, 0.33};   // right, up, along in the eye's axes
        for (int i = 0; i < 3; ++i) S[i] = pos[i] + local[0] * t.E[i] + local[1] * t.E[4 + i] + local[2] * t.E[8 + i];
        for (int i = 0; i < 3; ++i) t.S[i] = static_cast<float>(S[i]);
        t.eOk = t.sOk = t.wOk = t.okRead = t.ok = t.aimOutOk = true;
        t.w = 1.0f;
        const double n = std::sqrt(local[0] * local[0] + local[1] * local[1] + local[2] * local[2]);
        for (int i = 0; i < 3; ++i) t.aimOut[i] = static_cast<float>(local[i] / n);
        const f141::BendOut bt = f141::bend(t);
        check(bt.applied && close141(bt.eyeRight, 0.004, 1e-5) && close141(bt.eyeUp, -0.02, 1e-5) && close141(bt.eyeAlong, 0.33, 1e-5) && close141(bt.aimOutVsRayDeg, 0.0, 2e-3),
              "F141: a turned eye: S in the eye's axes and the exit aim (local, through E's rows) on the sight ray");
    }

    // ---- S against P14's forward line in the FP root's axes, and E less P14
    {
        float froot[16];
        const double rootPos[3] = {10.0, 0.0, 5.0};
        rigid(30.0, 0.0, rootPos, froot);
        const double p14w[3] = {10.2, 1.65, 5.1};
        const double off[3] = {0.012, -0.02, 0.36}, eOff[3] = {0.0, 0.0, 0.015};   // right, up, forward
        float S[3], E[16];
        rigid(30.0, 0.0, p14w, E);
        for (int i = 0; i < 3; ++i) {
            S[i] = static_cast<float>(p14w[i] + off[0] * froot[i] + off[1] * froot[4 + i] + off[2] * froot[8 + i]);
            E[12 + i] = static_cast<float>(p14w[i] + eOff[0] * froot[i] + eOff[1] * froot[4 + i] + eOff[2] * froot[8 + i]);
        }
        const f141::P14Off p = f141::p14Offset(S, E, p14w, froot);
        check(p.ok && close141(p.right, 0.012, 2e-6) && close141(p.up, -0.02, 2e-6) && close141(p.along, 0.36, 2e-6) && close141(p.eRel[2], 0.015, 2e-6) && close141(p.eDist, 0.015, 2e-6) &&
                  close141(p.eRel[0], 0.0, 2e-6),
              "F141: S 1.2 cm right and 2.0 cm under P14's line (the FP root's axes, turned 30 deg), E 1.5 cm ahead of P14");
        float bad[16] = {};
        check(!f141::p14Offset(S, E, p14w, bad).ok, "F141: a zero root has no axes: no offset");
    }

    // ---- the window's sums
    {
        f141::Acc a;
        const f141::BendOut b1 = f141::bend(in);
        f141::P14Off p;
        p.ok = true; p.right = 0.01; p.up = 0.02; p.along = 0.3; p.eDist = 0.05; p.eRel[2] = 0.05;
        a.add(true, in, b1, true, p, 0);
        f141::BendIn low = in;
        low.w = 0.4f;
        a.add(true, low, f141::bend(low), true, p, 2);
        a.add(false, in, b1, false, p, 0);
        f141::BendIn nos = in;
        nos.sOk = false;
        a.add(true, nos, f141::bend(nos), false, p, 0);
        check(a.samples == 4 && a.adsOff == 1 && a.wN == 3 && close141(a.wMin, 0.4, 1e-6) && close141(a.wMax, 1.0, 1e-9) && a.wFull == 2 && a.okN == 3 && a.applied == 1 && a.geoN == 2,
              "F141: the window: an ADS-off call counts apart, w's min and max, applied only at w = 1");
        f141::Acc under;
        f141::BendIn lowNoS = in;
        lowNoS.w = 0.4f;
        lowNoS.sOk = false;
        under.add(true, lowNoS, f141::bend(lowNoS), false, p, 0);
        under.add(true, in, f141::bend(in), true, p, 0);
        check(under.why[f141::kWhyNotFull] == 1 && under.why[f141::kWhyOk] == 1 && under.rejected() == 0, "F147: a sample under w 1.0 is counted apart and is not a rejection");
        check(a.p14N == 2 && a.p14Exact == 1 && close141(f141::Acc::mean(a.frameGapSum, a.p14N), 1.0, 1e-9) && close141(f141::Acc::mean(a.p14Up, a.p14N), 0.02, 1e-12) &&
                  a.noFp == 0 && close141(a.eDistMax, 0.05, 1e-12) && a.rayVsN == 1,
              "F141: the window: S against P14 on the paired calls (one of the same frame), the frame gap's mean, the exit against the ray on the applied call");
        a.reset();
        check(a.samples == 0 && a.wN == 0 && f141::Acc::mean(1.0, 0) == 0.0, "F141: the window resets");
    }

    // ---- names and activations
    {
        char out[64];
        const char raw[] = {'O', 'p', 't', 'i', 'c', '"', '\x01', 'x', 0, 'y'};
        check(f141::printableName(raw, sizeof(raw), out) == 8 && std::strcmp(out, "Optic??x") == 0, "F141: a name stops at its NUL; a quote and a control byte print as ?");
        char longName[100];
        std::memset(longName, 'a', sizeof(longName));
        check(f141::printableName(longName, sizeof(longName), out) == 63 && out[63] == 0, "F141: a name is cut at 63 bytes");
        f141::ActKey k1, k2;
        k1.act = 0x1000; k1.h0 = 0x2000; k1.h48 = 0x3000;
        k2 = k1;
        check(k1 == k2, "F141: the same act and handle is the same activation");
        k2.h48 = 0x3008;
        check(k1 != k2, "F141: a new locator object behind the sight handle is a new activation");
        k2 = k1;
        k2.act = 0x1100;
        check(k1 != k2, "F141: another act is a new activation");
    }

    // ---- the readout's anchor: U against W and P14
    {
        float W[16], L[16], M[16];
        const double wPos[3] = {4.0, 1.3, -2.0}, lPos[3] = {0.05, 0.03, 0.12}, mPos[3] = {0.3, -0.1, 0.7}, zero[3] = {0.0, 0.0, 0.0};
        rigid(35.0, -12.0, wPos, W);
        rigid(5.0, 2.0, lPos, L);   // the part's locator in the weapon's frame
        rigid(-50.0, 20.0, mPos, M);  // a motion of the whole weapon
        double U[16], Wd[16], W2[16], U2[16];
        f141::mul4(L, W, U);
        for (int i = 0; i < 16; ++i) Wd[i] = W[i];
        float Uf[16];
        toFloat(U, Uf);
        const f141::Rel r1 = f141::relTo(Uf, Wd);
        check(r1.ok && close141(r1.t[0], 0.05, 1e-5) && close141(r1.t[1], 0.03, 1e-5) && close141(r1.t[2], 0.12, 1e-5), "F141: U = L x W: U against W gives L's position, in W's rows");
        float Lq[16];
        rigid(5.0, 2.0, zero, Lq);
        double LR[9], lq[4];
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j) LR[3 * i + j] = Lq[4 * i + j];
        check(f141::quatOf(LR, lq) && f141::quatAngleDeg(r1.q, lq) < 1e-3, "F141: ... and L's rotation");
        f141::mul4(W, M, W2);
        float W2f[16];
        toFloat(W2, W2f);
        f141::mul4(L, W2f, U2);
        toFloat(U2, Uf);
        const f141::Rel r2 = f141::relTo(Uf, W2);
        check(r2.ok && close141(r2.t[0], r1.t[0], 1e-5) && close141(r2.t[1], r1.t[1], 1e-5) && close141(r2.t[2], r1.t[2], 1e-5) && f141::quatAngleDeg(r1.q, r2.q) < 2e-3,
              "F141: the weapon moved: U against the moved W is unchanged (weapon-anchored reads constant)");
        // against a frame that did not move with the weapon (P14 held still), the same motion shows
        const f141::Rel r3 = f141::relTo(Uf, Wd);
        check(r3.ok && (std::fabs(r3.t[0] - r1.t[0]) + std::fabs(r3.t[1] - r1.t[1]) + std::fabs(r3.t[2] - r1.t[2]) > 0.1 || f141::quatAngleDeg(r1.q, r3.q) > 1.0),
              "F141: against a frame that stayed, the moved U is not constant");
        float degenerate[16] = {};
        check(f141::relTo(Uf, Wd).ok && !f141::relTo(degenerate, Wd).ok, "F141: a zero U has no rotation: refused");

        // the spread: 40 samples of one rel with 1 mm of jitter in x and 0.05 deg about y
        f141::RelSample s[40];
        for (int k = 0; k < 40; ++k) {
            const double jx = (k % 2 ? 0.001 : -0.001), a = (k % 2 ? 0.05 : -0.05) / f141::kDeg;
            for (int i = 0; i < 3; ++i) s[k].t[i] = static_cast<float>(r1.t[i]);
            s[k].t[0] += static_cast<float>(jx);
            // q = q1 x (rotation by a about y), via the matrix
            double R[9] = {std::cos(a), 0, -std::sin(a), 0, 1, 0, std::sin(a), 0, std::cos(a)}, q[4];
            f141::quatOf(R, q);
            const double w1 = r1.q[0], x1 = r1.q[1], y1 = r1.q[2], z1 = r1.q[3];
            const double qq[4] = {w1 * q[0] - x1 * q[1] - y1 * q[2] - z1 * q[3], w1 * q[1] + x1 * q[0] + y1 * q[3] - z1 * q[2],
                                  w1 * q[2] - x1 * q[3] + y1 * q[0] + z1 * q[1], w1 * q[3] + x1 * q[2] - y1 * q[1] + z1 * q[0]};
            const double sgn = (k % 3 == 0) ? -1.0 : 1.0;   // a quaternion and its negative are the same rotation
            for (int i = 0; i < 4; ++i) s[k].q[i] = static_cast<float>(sgn * qq[i]);
        }
        const f141::Spread sp = f141::spreadOf(s, 40);
        check(sp.n == 40 && close141(sp.mean[0], r1.t[0], 1e-6) && close141(sp.devMaxM, 0.001, 2e-6) && close141(sp.rotDevMaxDeg, 0.05, 2e-3),
              "F141: the spread: the mean, 1 mm the largest distance from it, 0.05 deg the largest rotation from the mean (signs of q folded)");
        check(f141::spreadOf(s, 0).n == 0 && f141::spreadOf(nullptr, 3).n == 0, "F141: no samples, no spread");

        // a part's window: the same-frame set is the basis from 30 samples, both sets below it; flags and their text
        static f141::PartSets sets;
        static f141::RelSample scratch[2 * f141::kSampleCap];
        f141::Part part;
        f141::Rel good = r1, apart = r1;
        apart.t[0] += 0.01;   // the one-frame-apart samples sit 1 cm off
        for (int k = 0; k < 10; ++k) part.add(sets, 0, good, good);
        for (int k = 0; k < 10; ++k) part.add(sets, 1, apart, apart);
        check(!part.exact() && close141(f141::partSpread(part, sets, true, scratch).devMaxM, 0.005, 1e-5) && part.paired[0] == 10 && part.paired[1] == 10,
              "F141: under 30 same-frame samples the stats are over both sets");
        for (int k = 0; k < 25; ++k) part.add(sets, 0, good, good);
        check(part.exact() && f141::partSpread(part, sets, true, scratch).devMaxM < 1e-6 && f141::partSpread(part, sets, false, scratch).n == 35,
              "F141: from 30 same-frame samples the stats are theirs alone");
        f141::Rel bad;
        part.add(sets, 0, bad, good);
        part.add(sets, 2, good, good);
        check(part.paired[0] == 35 && part.n[0] == 35, "F141: a rel that failed, or no set, is not counted");
        const uint8_t f1[f141::kFlags] = {1, 0, 0, 1, 1, 1}, f2[f141::kFlags] = {1, 0, 1, 1, 1, 1};
        part.noteFlags(f1);
        part.noteFlags(f1);
        check(part.flag[2] == 0 && part.flag[0] == 1, "F141: the flags as first read");
        part.noteFlags(f2);
        char fb[8];
        check(part.flag[2] == f141::kFlagMixed && std::strcmp(f141::flagText(part.flag[2], fb), "mixed") == 0 && std::strcmp(f141::flagText(part.flag[0], fb), "1") == 0 &&
                  std::strcmp(f141::flagText(f141::kFlagNotRead, fb), "not read") == 0 && std::strcmp(f141::flagText(253, fb), "253") == 0 &&
                  std::strcmp(f141::flagText(0, fb), "0") == 0 && std::strcmp(f141::flagText(42, fb), "42") == 0,
              "F141: a flag that changed in the window is mixed; the text of a value, mixed and not read");
    }
    {   // mul4 against the identity, and the angle helpers
        float I[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}, A[16];
        const double pos[3] = {1, 2, 3};
        rigid(20.0, 5.0, pos, A);
        double out[16];
        f141::mul4(A, I, out);
        bool same = true;
        for (int i = 0; i < 16; ++i) same = same && close141(out[i], A[i], 1e-7);
        const double x[3] = {1, 0, 0}, y[3] = {0, 1, 0}, z0[3] = {0, 0, 0};
        check(same && close141(f141::angleDeg(x, y), 90.0, 1e-9) && f141::angleDeg(x, z0) < 0.0, "F141: A x I = A; 90 deg between x and y; no angle to a zero vector");
    }
}

}  // namespace f141test
