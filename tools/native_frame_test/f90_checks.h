// F90 and F91a checks (native on-foot VR; F91a: the parts known by their record's mesh and scale, across sessions, and the role line). F90: the weapon readout at
// the hip's size in ADS (A) and the pistol's readout placed in the weapon's frame (B), run90 below: the pure
// functions of src\d3d11\native_on_foot_f90.h on the F85 two-eye rig (two eyes 6.06 cm apart with a 10.75 degree cant, a display affine and jittered scene rows).
// Header only, run by native_frame_test.
#pragma once

#include <d3d11.h>   // draw_record_tag.h needs it ahead of it (its staging ring)

#include "../../src/d3d11/draw_record_tag.h"   // rotateByPackedQuat: the record's quaternion, as the vertex shader turns a mesh point
#include "../../src/d3d11/native_on_foot_f90.h"
#include "f85_checks.h"   // the F85 rig's helpers: sceneRows, compositeFrom, rotY, rotX

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <vector>

namespace f90test {

namespace f85 = edvr::nofoot::f85;
namespace f86 = edvr::nofoot::f86;
namespace f88 = edvr::nofoot::f88;
namespace f90 = edvr::nofoot::f90;
using f85test::Mat;

// The F85 rig: the aim camera K, two eyes 6.06 cm apart with a 10.75 degree cant, a display affine and a jittered projection; U the head turned 30 degrees.
struct Rig {
    f85::AffParams aff;
    double oh[2][3] = {{0.4, -0.1, 2.0}, {0.41, -0.1, 2.02}}, ow[2][3] = {}, eyePos[2][3] = {}, Ok[3] = {123.4, 1.62, -45.6};
    f85::Rows W[2], B[2];
    f85::CamK K[2];
    Mat Rk{}, Ru{};
};
inline Rig makeRig() {
    Rig g;
    const double kCant = 10.75 * f85::kPi / 180.0, kIpd = 0.0606;
    g.Rk = f85test::mul(f85test::rotY(0.31), f85test::rotX(-0.12));
    g.Ru = f85test::mul(g.Rk, f85test::rotY(30.0 * f85::kPi / 180.0));
    const double right[3] = {g.Rk.m[0][0], g.Rk.m[1][0], g.Rk.m[2][0]};
    for (int e = 0; e < 2; ++e) {
        const double s = e == 0 ? -0.5 * kIpd : 0.5 * kIpd;
        for (int i = 0; i < 3; ++i) g.eyePos[e][i] = g.Ok[i] + s * right[i];
        for (int i = 0; i < 3; ++i) g.ow[e][i] = g.eyePos[e][i] + (e == 0 ? 0.0 : 0.002 * (i + 1));
    }
    g.aff.sigma[0] = 1.0094;
    g.aff.sigma[1] = 1.0081;
    g.aff.offM[0] = 0.011;
    g.aff.offM[1] = -0.007;
    g.aff.offW[0] = 0.0153;
    g.aff.offW[1] = 0.0031;
    const double lambda = 1.03;
    for (int e = 0; e < 2; ++e) {
        const Mat eye = f85test::mul(g.Rk, f85test::rotY(e == 0 ? kCant : -kCant));
        const double ePosRel[3] = {g.eyePos[e][0] - g.ow[e][0], g.eyePos[e][1] - g.ow[e][1], g.eyePos[e][2] - g.ow[e][2]};
        float sceneF[16];
        f85test::sceneRows(eye, ePosRel, 1.38, 1.38, 0.0173, 0.0031, 0.025, sceneF);
        g.W[e] = f85::rowsOfWorld(sceneF);
        double a[3], k4[3];
        f85::mulVec3(g.Rk.m, g.oh[e], a);
        for (int i = 0; i < 3; ++i) k4[i] = a[i] + g.Ok[i] - g.ow[e][i];
        g.B[e] = f85test::compositeFrom(g.W[e], g.aff, lambda, g.Rk.m, k4);
        g.K[e] = f85::solveK(g.B[e], g.W[e], g.aff);
    }
    return g;
}
// The eye's getter frame for U (P - o_world = U3 v + u4, v = X0 - o_hud).
inline f85::UFrame uOf(const Rig& g, int e) {
    f85::UFrame uf;
    std::memcpy(uf.U3, g.Ru.m, sizeof(uf.U3));
    double m[3];
    f85::mulVec3(uf.U3, g.oh[e], m);
    for (int a = 0; a < 3; ++a) uf.u4[a] = m[a] + g.Ok[a] - g.ow[e][a];
    return uf;
}
// The draw's rewritten rows (B . K^-1 U'), as the private b0 stores them (float32): F85b's rows when uf is the game's.
inline bool rowsOf(const Rig& g, int e, const f85::UFrame& uf, float r[12]) {
    const f85::Corr c = f85::makeCorrection(g.K[e], uf);
    if (!c.ok) return false;
    f85::rowsToFloat(f85::applyCorrection(g.B[e], c), r, r + 4, r + 8);
    return true;
}
inline double dist3(const double a[3], const double b[3]) { return std::sqrt((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2])); }
// A world point (relative to the eye's rows' origin) of a hud-frame point through a getter frame.
inline void worldOf(const f85::UFrame& uf, const double v[3], double out[3]) {
    f85::mulVec3(uf.U3, v, out);
    for (int i = 0; i < 3; ++i) out[i] += uf.u4[i];
}
// The parts' name ids in the two flights' sessions (F91: labels only; the game interns them per session).
constexpr uint32_t kN89Hip = 29501, kN89RifleAds = 29500, kN89PistolAds = 29498, kN89Tool = 29495;
constexpr uint32_t kN90Hip = 29510, kN90RifleAds = 29526, kN90PistolAds = 29524, kN90Tool = 29553;
// A part's record on the rig: the name id (a label), the translation X0 in the hud frame, the quaternion words (-90 degrees about x, as every record of the F89
// flight's dumps), the scale and the mesh's x and z extents.
struct Record {
    uint32_t name;
    double x0[3];
    double scale, mesh[2];
};
constexpr uint32_t kQ2 = 0x81892657u, kQ3 = 0xDB557FE1u;
inline f90::In inOf(const Rig& g, int e, uint32_t frame, const Record& rec, int item, uint32_t serial, const f90::Keys& keys, double adsW = 0.0) {
    f90::In in;
    in.frame = frame;
    in.name = rec.name;
    in.item = item;
    in.itemSerial = serial;
    in.meshRead = true;
    in.meshExt[0] = rec.mesh[0];
    in.meshExt[1] = rec.mesh[1];
    in.recScale = rec.scale;
    in.q2 = kQ2;
    in.q3 = kQ3;
    for (int i = 0; i < 3; ++i) {
        in.x0rel[i] = rec.x0[i] - g.oh[e][i];
        in.ow[i] = g.ow[e][i];
    }
    double eyeRel[3];
    in.eyeOk = f86::cameraOriginOfRows(g.W[e], eyeRel);
    for (int i = 0; i < 3; ++i) in.eye[i] = eyeRel[i] + g.ow[e][i];
    in.keys = keys;
    in.adsW = adsW;
    return in;
}
// The content centre of a record relative to o_hud (v = X - o_hud: the table's centre turned and scaled as the vertex shader does).
inline void contentHud(const Rig& g, int e, const Record& rec, double v[3]) {
    const f90::Part* p = f90::partOf(true, rec.mesh, rec.scale);
    double x0rel[3];
    for (int i = 0; i < 3; ++i) x0rel[i] = rec.x0[i] - g.oh[e][i];
    f90::contentPoint(x0rel, kQ2, kQ3, rec.scale, p->centre, edvr::drawtag::rotateByPackedQuat, v);
}
// A synthetic weapon: F_root's rows are K's axes (right, up, forward) at a hip-height origin; the FP right attach is turned by yawDeg about up from F_root, with
// its barrel on row 0 pointing backwards (so the pick must take the sign) and its up on row 2.
struct Weapon {
    float froot[16] = {}, attach[16] = {};
};
inline Weapon weaponOf(const Rig& g, double yawDeg) {
    Weapon w;
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) w.froot[4 * r + c] = static_cast<float>(g.Rk.m[c][r]);
        w.froot[12 + r] = static_cast<float>(g.Ok[r] + 0.3 * g.Rk.m[r][2] - 0.25 * g.Rk.m[r][1]);
    }
    w.froot[15] = 1.0f;
    const double a = yawDeg * f85::kPi / 180.0, s = std::sin(a), c = std::cos(a);
    const double rows[3][3] = {{-s, 0.0, -c}, {c, 0.0, -s}, {0.0, 1.0, 0.0}};
    for (int r = 0; r < 3; ++r)
        for (int k = 0; k < 3; ++k) w.attach[4 * r + k] = static_cast<float>(rows[r][k]);
    w.attach[12] = 0.05f;
    w.attach[13] = 1.1f;
    w.attach[14] = 0.3f;
    w.attach[15] = 1.0f;
    return w;
}
// A row (x, y, z) through F_root's 3x3 (row-vector).
inline void throughRoot(const float froot[16], const double v[3], double out[3]) {
    for (int c = 0; c < 3; ++c) out[c] = v[0] * froot[c] + v[1] * froot[4 + c] + v[2] * froot[8 + c];
}

template <class Check>
inline void run90(Check&& check) {
    const Rig g = makeRig();
    const double axis[2] = {0.0, 0.0};
    const auto rot = edvr::drawtag::rotateByPackedQuat;
    check(g.K[0].ok && g.K[1].ok && g.K[0].rigid < 1e-9 && g.K[1].rigid < 1e-9, "F90: the F85 two-eye rig solves K in both eyes");

    // ---- 1: the texel ratios, from the table ----------------------------------------------------------------------------------------------------------------
    {
        const double r0u = f90::texelRatio(f90::kRoleRifleAds, 0), r0v = f90::texelRatio(f90::kRoleRifleAds, 1);
        const double r8u = f90::texelRatio(f90::kRolePistolAds, 0), r8v = f90::texelRatio(f90::kRolePistolAds, 1);
        check(std::fabs(r0u - 16.0 / 9.0) < 1e-3 && std::fabs(r0v - 16.0 / 9.0) < 1e-3, "F90 A: the rifle's ADS readout maps 16/9 the hip readout's texels per metre, in u and in v (1e-3)");
        check(std::fabs(r8u - 4.0 / 7.0) < 1e-3 && std::fabs(r8v - 4.0 / 7.0) < 1e-3, "F90 A: the pistol's ADS readout maps 4/7 the hip readout's texels per metre, in u and in v (1e-3)");
        check(f90::texelRatio(f90::kRoleHip, 0) == 1.0 && f90::texelRatio(f90::kRoleHip, 1) == 1.0 && std::isnan(f90::texelRatio(f90::kRoleNone, 0)) &&
                  std::isnan(f90::texelRatio(f90::kRoleTool, 0)),
              "F90 A: the hip readout's ratio is exactly 1; no role and the tool (never scaled) have none");
        const double t1 = f90::texelsPerM(*f90::partOfRole(f90::kRoleHip), 0), t0 = f90::texelsPerM(*f90::partOfRole(f90::kRoleRifleAds), 0);
        check(std::fabs(t1 - 10022.0) < 5.0 && std::fabs(t0 - 17816.0) < 5.0, "F90 A: the table reproduces the review's texels per metre (10022 and 17816)");
        // F91 check 2: the factor is the layouts' base-scale ratio (texels/m = 4008 / base)
        const double bh = f90::partOfRole(f90::kRoleHip)->base;
        check(std::fabs(f90::sizeFactor(f90::kRoleRifleAds) - bh / f90::partOfRole(f90::kRoleRifleAds)->base) < 1e-3 &&
                  std::fabs(f90::sizeFactor(f90::kRolePistolAds) - bh / f90::partOfRole(f90::kRolePistolAds)->base) < 1e-3 && f90::sizeFactor(f90::kRoleHip) == 1.0,
              "F91 A: the factor equals the game's base-scale ratio, 0.400000006 / 0.224999994 and / 0.699999988 (1e-3); the hip's is 1");
    }

    // ---- F91 1: the parts are known across sessions by the record's mesh and scale --------------------------------------------------------------------------
    {
        // the measured parts, each session's names, and the name 0 or garbage: the same roles
        const double meshes[4][2] = {{2.0, 1.0}, {3.0, 1.0}, {2.0, 1.0}, {2.0, 1.0}};
        const double scales[4] = {0.072070308, 0.0500976555, 0.166796863, 0.0939941406};
        const int roles[4] = {f90::kRoleHip, f90::kRoleRifleAds, f90::kRolePistolAds, f90::kRoleTool};
        const uint32_t names[4][4] = {{kN89Hip, kN89RifleAds, kN89PistolAds, kN89Tool}, {kN90Hip, kN90RifleAds, kN90PistolAds, kN90Tool}, {0, 0, 0, 0},
                                      {0xDEADBEEFu, 7u, 0xFFFFFFFFu, 29501u}};
        bool ok = true;
        for (int s = 0; s < 4; ++s)
            for (int q = 0; q < 4; ++q) {
                const f90::Part* p = f90::partOf(true, meshes[q], scales[q]);
                const Record rec{names[s][q], {-0.34, -0.05, 0.52}, scales[q], {meshes[q][0], meshes[q][1]}};
                f90::Latch latch;
                const f90::Out o = f90::decide(inOf(g, 0, 5, rec, f90::kItemOther, 0, f90::Keys{}), uOf(g, 0), latch, rot, [] { return f90::WeaponFrame{}; });
                ok = ok && p && p->role == roles[q] && o.role == roles[q];
            }
        check(ok, "F91 A: the hip, rifle ADS, pistol ADS and tool readouts are named by mesh and scale, whatever the name id (F89's, F90's, 0, garbage)");
        bool unique = true;
        for (int q = 0; q < 4; ++q)
            for (int r = 0; r < 4; ++r)
                if (q != r && f90::tableMatches(*f90::partOfRole(roles[r]), true, meshes[q], scales[q])) unique = false;
        check(unique, "F91 A: each measured part matches exactly one row (the four scales are at least 1.3x apart)");
        // the same draws named by each session give the same rows, bit for bit: the rifle's ADS readout x1.7778; a pistol held: the hip readout moved to T,
        // the pistol's ADS readout x0.5714 and moved
        const Weapon wp = weaponOf(g, 7.0);
        const auto frameFn = [&] { return f90::weaponFrame(true, wp.attach, true, wp.froot); };
        f90::Keys keys;
        keys.forwardCm = 13.0;
        keys.leftCm = 6.0;
        keys.upCm = -3.0;
        keys.towardCm = 2.0;
        const auto session = [&](const uint32_t nm[4], bool pistolFirst, float rows[3][2][12], f90::Out outs[3][2]) {
            f90::Latch latch;
            bool s = true;
            const Record recs[3] = {{nm[0], {0.10, -0.12, 0.36}, 0.072070308, {2.0, 1.0}},
                                    {nm[1], {-0.34, -0.05, 0.52}, 0.0500976555, {3.0, 1.0}},
                                    {nm[2], {0.07, -0.05, 0.42}, 0.166796863, {2.0, 1.0}}};
            for (uint32_t frame = 40; frame < 42; ++frame)   // two frames: with the pistol's ADS readout first, its second frame finds the first frame's T
                for (int e = 0; e < 2; ++e)
                    for (int k = 0; k < 3; ++k) {
                        const int q = pistolFirst ? (k == 0 ? 2 : k == 2 ? 0 : 1) : k;
                        const f85::UFrame game = uOf(g, e);
                        f85::UFrame uf = game;
                        const int item = q == 1 ? f90::kItemOther : f90::kItemPistol;
                        const f90::Out o = f90::decide(inOf(g, e, frame, recs[q], item, 9, keys), game, latch, rot, frameFn);
                        f90::applyMap(uf, o.map);
                        if (frame == 41) {
                            outs[q][e] = o;
                            s = s && rowsOf(g, e, uf, rows[q][e]);
                        }
                    }
            return s;
        };
        float r89[3][2][12], r90[3][2][12], rPerm[3][2][12];
        f90::Out o89[3][2], o90[3][2], oPerm[3][2];
        const bool ran = session(names[0], false, r89, o89) && session(names[1], false, r90, o90) && session(names[3], true, rPerm, oPerm);
        check(ran && std::memcmp(r89, r90, sizeof(r89)) == 0, "F91 A: the F89-named and F90-named draws give bit-identical rows");
        check(ran && o90[1][0].a == f90::kAScaled && std::fabs(o90[1][0].k - 16.0 / 9.0) < 1e-4 && o90[0][0].b == f90::kBMoved && o90[0][0].map.move &&
                  o90[2][0].a == f90::kAScaled && std::fabs(o90[2][0].k - 4.0 / 7.0) < 1e-4 && o90[2][0].b == f90::kBMoved && o90[2][0].map.move,
              "F91 A: by mesh and scale: 29526 x1.7778, and with a pistol held 29510 moved to T, 29524 x0.5714 and moved");
        check(ran && std::memcmp(r89, rPerm, sizeof(r89)) == 0 && oPerm[2][0].b == f90::kBMovedPrevious,
              "F91 A: the pistol's parts drawn first and with garbage names give the same rows (its ADS readout takes the previous frame's T, counted)");
        // the role line: once per (held item, slot) whatever the slot order, again on an item change or a role change, at most kRoleLinesPerItem a held item
        f90::RoleLog logs[4];
        const int order[2][4] = {{0, 1, 2, 3}, {2, 0, 3, 1}};   // which role sits in which slot: F89's order, then pistol first
        bool once = true;
        for (const auto& ord : order) {
            for (f90::RoleLog& l : logs) l = f90::RoleLog{};
            for (int rep = 0; rep < 3; ++rep)
                for (int sl = 0; sl < 4; ++sl) once = once && logs[sl].step(11, roles[ord[sl]]) == (rep == 0);
            for (int sl = 0; sl < 4; ++sl) once = once && logs[sl].step(12, roles[ord[sl]]);   // another item
        }
        f90::RoleLog flip;
        int lines = 0;
        for (int i = 0; i < 20; ++i) lines += flip.step(13, i % 2 ? f90::kRoleNone : f90::kRoleRifleAds) ? 1 : 0;
        check(once && lines == static_cast<int>(f90::kRoleLinesPerItem) && flip.suppressed == 20 - f90::kRoleLinesPerItem && flip.step(14, f90::kRoleNone),
              "F91 A: the role line comes once per held item and slot in any slot order, again on an item change; a flipping role is capped and counted");
    }

    // ---- 2: part 29500 at the hip's size, about its content centre, on the two-eye rig ------------------------------------------------------------------------
    // The record sits 0.52 m out along U, 30 degrees left of U's axis, so the scaled panel (27 cm wide, its content 4.8 cm left of its centre) lies within 23
    // degrees of both eyes' axes: on screen, where a pixel figure means something.
    const Record ads{kN89RifleAds, {-0.34, -0.05, 0.52}, 0.0501, {3.0, 1.0}};
    {
        double moved = 0.0, ratioErr = 0.0, cornerPx = 0.0, proofPx = 0.0, k = 0.0;
        bool ok = true;
        for (int e = 0; e < 2; ++e) {
            f90::Latch latch;
            const f85::UFrame uf = uOf(g, e);
            const f90::Out o = f90::decide(inOf(g, e, 10, ads, f90::kItemOther, 0, f90::Keys{}), uf, latch, rot, [] { return f90::WeaponFrame{}; });
            ok = ok && o.a == f90::kAScaled && o.map.scale && !o.map.move && o.b == f90::kBNone;
            k = o.k;
            f85::UFrame uf2 = uf;
            f90::applyMap(uf2, o.map);
            double cv[3], C[3], C2[3];
            contentHud(g, e, ads, cv);
            worldOf(uf, cv, C);
            worldOf(uf2, cv, C2);
            moved = std::max(moved, dist3(C, C2));
            // the panel's corners: (+-1.5, 0, +-0.5) in mesh units
            float r[12];
            ok = ok && rowsOf(g, e, uf2, r);
            const f85::Rows Bf = f85::rowsOfComposite(r, r + 4, r + 8);
            for (int q = 0; q < 4; ++q) {
                const double m[3] = {q & 1 ? 1.5 : -1.5, 0.0, q & 2 ? 0.5 : -0.5};
                double rv[3], v[3], P[3], P2[3];
                rot(kQ2, kQ3, m, rv);
                for (int i = 0; i < 3; ++i) v[i] = ads.x0[i] - g.oh[e][i] + ads.scale * rv[i];
                worldOf(uf, v, P);
                worldOf(uf2, v, P2);
                ratioErr = std::max(ratioErr, std::fabs(dist3(P2, C2) / dist3(P, C) - o.k));
                double shown[2], target[2];
                ok = ok && f85::shownNdc(g.aff, axis, Bf, v, shown) && f85::targetNdc(g.aff, axis, g.W[e], P2, target);
                cornerPx = std::max(cornerPx, std::hypot((shown[0] - target[0]) * 2016.0, (shown[1] - target[1]) * 2016.0));
            }
            // the proof: the anchor (the record's translation) drawn by the rewritten rows against the CPU's P_ui under the same map
            const double v0[3] = {ads.x0[0] - g.oh[e][0], ads.x0[1] - g.oh[e][1], ads.x0[2] - g.oh[e][2]};
            double pRel[3];
            f85::mulVec3(g.Ru.m, ads.x0, pRel);
            for (int i = 0; i < 3; ++i) pRel[i] += g.Ok[i] - g.ow[e][i];
            f90::mapPoint(o.map, pRel, pRel);
            double shown[2], target[2];
            ok = ok && f85::shownNdc(g.aff, axis, Bf, v0, shown) && f85::targetNdc(g.aff, axis, g.W[e], pRel, target);
            proofPx = std::max(proofPx, std::hypot((shown[0] - target[0]) * 2016.0, (shown[1] - target[1]) * 2016.0));
        }
        std::printf("native_frame_test: F90 A rig: factor %.6f, content centre moved %.2e m, corner ratio error %.2e, corners %.5f px, proof %.5f px\n", k, moved, ratioErr,
                    cornerPx, proofPx);
        check(ok && moved < 1e-6, "F90 A: scaling part 29500 leaves its content centre's world point where the game put it (both eyes, 1e-6 m)");
        check(ok && ratioErr < 1e-9 && std::fabs(k - 16.0 / 9.0) < 1e-3, "F90 A: the panel's corner-to-centre distances grow by 16/9 (the factor, within 1e-3 of 16/9)");
        check(ok && cornerPx < 0.01, "F90 A: both eyes' scaled corners land within 0.01 px of W_e (U' v) through the stored float rows");
        check(ok && proofPx < 0.01, "F90 A: the proof's corrected residual, its target under the same map, stays within 0.01 px");
        // the correction's figures: from the game's U (a scaled M3 is not a rotation)
        f85::UFrame uf = uOf(g, 0), uf2 = uf;
        f90::Latch latch;
        f90::applyMap(uf2, f90::decide(inOf(g, 0, 10, ads, f90::kItemOther, 0, f90::Keys{}), uf, latch, rot, [] { return f90::WeaponFrame{}; }).map);
        const f85::Corr cg = f85::makeCorrection(g.K[0], uf), cs = f85::makeCorrection(g.K[0], uf2);
        check(cg.ok && std::fabs(cg.angleDeg - 30.0) < 1e-6 && f85::rigidError(cs.M3) > 0.5, "F90 A: the scaled correction is no rotation, the game's U still gives the head's 30 degrees");
    }

    // ---- 3: no ADS input; the rifle's hip part, a tool's part and a pistol with all four keys 0 keep F85b's rows bit for bit -----------------------------------
    {
        float r0[12], r1[12];
        f90::Latch l0, l1;
        bool ok = true;
        for (int e = 0; e < 2; ++e) {
            f85::UFrame a = uOf(g, e), b = uOf(g, e);
            f90::applyMap(a, f90::decide(inOf(g, e, 20, ads, f90::kItemOther, 0, f90::Keys{}, 0.0), a, l0, rot, [] { return f90::WeaponFrame{}; }).map);
            f90::applyMap(b, f90::decide(inOf(g, e, 20, ads, f90::kItemOther, 0, f90::Keys{}, 1.0), b, l1, rot, [] { return f90::WeaponFrame{}; }).map);
            ok = ok && rowsOf(g, e, a, r0) && rowsOf(g, e, b, r1) && std::memcmp(r0, r1, sizeof(r0)) == 0;
        }
        check(ok, "F90 A: ADS weight 0 and 1 give the rifle's ADS readout the same rows bit for bit (no ADS gate, no ramp)");
        const Record hip{kN89Hip, {0.10, -0.12, 0.36}, 0.07207, {2.0, 1.0}};
        const Record tool{kN90Tool, {0.10, -0.12, 0.36}, 0.0939941406, {2.0, 1.0}};
        const Record outside{31000, {0.10, -0.12, 0.36}, 0.11, {2.0, 1.0}};
        const Weapon wp = weaponOf(g, 7.0);
        const auto frameFn = [&] { return f90::weaponFrame(true, wp.attach, true, wp.froot); };
        const auto same = [&](const Record& rec, int item, int firstEye, uint32_t frame, f90::Latch& latch) {
            bool s = true;
            for (int i = 0; i < 2; ++i) {
                const int e = i == 0 ? firstEye : 1 - firstEye;
                const f85::UFrame game = uOf(g, e);
                f85::UFrame uf = game;
                const f90::Out o = f90::decide(inOf(g, e, frame, rec, item, 7, f90::Keys{}), uf, latch, rot, frameFn);
                f90::applyMap(uf, o.map);
                float ra[12], rb[12];
                s = s && o.map.identity() && rowsOf(g, e, game, ra) && rowsOf(g, e, uf, rb) && std::memcmp(ra, rb, sizeof(ra)) == 0;
            }
            return s;
        };
        f90::Latch latch;
        check(same(hip, f90::kItemOther, 0, 30, latch), "F90: a rifle's hip readout keeps F85b's rows bit for bit in both eyes");
        check(same(outside, f90::kItemOther, 0, 30, latch) && same(outside, f90::kItemPistol, 0, 30, latch), "F90: a part outside the table keeps F85b's rows bit for bit");
        f90::Latch tl;
        const f90::Out to = f90::decide(inOf(g, 0, 30, tool, f90::kItemPistol, 7, f90::Keys{}), uOf(g, 0), tl, rot, frameFn);
        check(to.role == f90::kRoleTool && to.a == f90::kATool && to.b == f90::kBNone && same(tool, f90::kItemOther, 0, 30, tl) && same(tool, f90::kItemPistol, 1, 31, tl) &&
                  !tl.have,
              "F91 A: the tool's readout is named and keeps F85b's rows bit for bit (never scaled or moved)");
        f90::Latch pl;
        check(same(hip, f90::kItemPistol, 0, 31, pl) && same(hip, f90::kItemPistol, 1, 32, pl) && pl.have && pl.frame == 32,
              "F90 B: a pistol with all four keys 0 keeps F85b's rows bit for bit in both eyes, either eye first (the latch still runs)");
    }

    // ---- 4: the pistol's keys (13, 6, -3, 2): the hip readout on T, the facing kept, the ADS readout on T at the hip's size, one T per frame ------------------------
    {
        const Record hip{kN89Hip, {0.10, -0.12, 0.36}, 0.07207, {2.0, 1.0}};
        const Record pads{kN89PistolAds, {0.07, -0.05, 0.42}, 0.1668, {2.0, 1.0}};
        const Weapon wp = weaponOf(g, 7.0);
        int frames = 0;
        const auto frameFn = [&] {
            ++frames;
            return f90::weaponFrame(true, wp.attach, true, wp.froot);
        };
        f90::Keys keys;
        keys.forwardCm = f90::kForwardDefaultCm;
        keys.leftCm = f90::kLeftDefaultCm;
        keys.upCm = f90::kUpDefaultCm;
        keys.towardCm = f90::kTowardDefaultCm;
        check(keys.forwardCm == 13.0 && keys.leftCm == 6.0 && keys.upCm == -3.0 && keys.towardCm == 2.0, "F90 B: the built-in values are the review's: forward 13, left 6, up -3, toward eye 2 cm");
        const auto lands = [&](const f90::Out& o, const f85::UFrame& game, int e, const Record& rec, double* toT, bool* facing, double* scale) {
            f85::UFrame uf = game;
            f90::applyMap(uf, o.map);
            double cv[3], P[3];
            contentHud(g, e, rec, cv);
            worldOf(uf, cv, P);
            for (int i = 0; i < 3; ++i) P[i] += g.ow[e][i];
            *toT = dist3(P, o.T);
            if (facing) *facing = std::memcmp(uf.U3, game.U3, sizeof(uf.U3)) == 0;
            if (scale) *scale = std::sqrt(uf.U3[0][0] * uf.U3[0][0] + uf.U3[1][0] * uf.U3[1][0] + uf.U3[2][0] * uf.U3[2][0]);
        };
        f90::Latch latch;
        // frame 100: eye 0 first latches T; eye 1 reuses it; 29498 lands on it
        const f85::UFrame g0 = uOf(g, 0), g1 = uOf(g, 1);
        const f90::Out a0 = f90::decide(inOf(g, 0, 100, hip, f90::kItemPistol, 7, keys), g0, latch, rot, frameFn);
        const f90::Out a1 = f90::decide(inOf(g, 1, 100, hip, f90::kItemPistol, 7, keys), g1, latch, rot, frameFn);
        const f90::Out a8 = f90::decide(inOf(g, 0, 100, pads, f90::kItemPistol, 7, keys), g0, latch, rot, frameFn);
        double d0 = 1.0, d1 = 1.0, d8 = 1.0, s8 = 0.0;
        bool facing0 = false, facing1 = false;
        lands(a0, g0, 0, hip, &d0, &facing0, nullptr);
        lands(a1, g1, 1, hip, &d1, &facing1, nullptr);
        lands(a8, g0, 0, pads, &d8, nullptr, &s8);
        check(a0.b == f90::kBMoved && a0.latched && a0.frameComputed && a0.wf.src == f90::kFrameAttach && d0 < 1e-6,
              "F90 B: the first 29501 draw of a frame latches T and its content centre lands on T (1e-6 m)");
        check(facing0 && facing1, "F90 B: the move leaves U3 as the game's (no turn, no scale: the panel keeps its facing)");
        check(a1.b == f90::kBMoved && !a1.latched && !a1.frameComputed && dist3(a1.T, a0.T) == 0.0 && d1 < 1e-6 && frames == 1,
              "F90 B: the other eye reuses the latched T and its content lands on it; the weapon frame is read once a frame");
        double tpmErr = 0.0;
        for (int ax = 0; ax < 2; ++ax) {
            const double world = f90::texelsPerM(*f90::partOfRole(f90::kRolePistolAds), ax) / s8, hipW = f90::texelsPerM(*f90::partOfRole(f90::kRoleHip), ax);
            tpmErr = std::max(tpmErr, std::fabs(world / hipW - 1.0));
        }
        check(a8.a == f90::kAScaled && a8.b == f90::kBMoved && d8 < 1e-6 && tpmErr < 1e-3,
              "F90 B: 29498 is scaled to the hip's texels per metre (1e-3) and its content lands on the same T (1e-6 m)");
        // T itself: forward along the barrel, left across it, up along the pistol's up, then 2 cm toward the deciding eye
        {
            const f90::WeaponFrame wf = a0.wf;
            double T0[3], toE[3], dE[3];
            for (int i = 0; i < 3; ++i) T0[i] = a0.C[i] + 0.13 * wf.b[i] + 0.06 * wf.l[i] - 0.03 * wf.u[i];
            const f90::In in0 = inOf(g, 0, 100, hip, f90::kItemPistol, 7, keys);
            for (int i = 0; i < 3; ++i) {
                toE[i] = a0.T[i] - T0[i];
                dE[i] = in0.eye[i] - T0[i];
            }
            double dEu[3];
            edvr::nofoot::ikUnit(dE, dEu);
            const double along = edvr::nofoot::ikDot(toE, dEu);
            double across = 0.0;
            for (int i = 0; i < 3; ++i) across = std::max(across, std::fabs(toE[i] - along * dEu[i]));
            const double eyeErr = std::max({std::fabs(in0.eye[0] - g.eyePos[0][0]), std::fabs(in0.eye[1] - g.eyePos[0][1]), std::fabs(in0.eye[2] - g.eyePos[0][2])});
            check(std::fabs(along - 0.02) < 1e-9 && across < 1e-9 && eyeErr < 1e-6 && std::fabs(a0.shiftM - dist3(a0.T, a0.C)) < 1e-12,
                  "F90 B: T = C + 13 cm along the barrel + 6 cm left - 3 cm up, then 2 cm toward the deciding eye (its origin from its scene rows)");
        }
        // frame 101: eye 1 first
        const f90::Out b1 = f90::decide(inOf(g, 1, 101, hip, f90::kItemPistol, 7, keys), g1, latch, rot, frameFn);
        const f90::Out b0 = f90::decide(inOf(g, 0, 101, hip, f90::kItemPistol, 7, keys), g0, latch, rot, frameFn);
        double e0 = 1.0, e1 = 1.0;
        lands(b0, g0, 0, hip, &e0, nullptr, nullptr);
        lands(b1, g1, 1, hip, &e1, nullptr, nullptr);
        check(b1.latched && !b0.latched && dist3(b0.T, b1.T) == 0.0 && e0 < 1e-6 && e1 < 1e-6 && latch.frame == 101,
              "F90 B: eye 1 first: it latches T and eye 0 lands on the same T");
        // frame 102: 29498 before 29501 takes frame 101's T (counted); then 29501 latches 102's
        const f90::Out c8 = f90::decide(inOf(g, 0, 102, pads, f90::kItemPistol, 7, keys), g0, latch, rot, frameFn);
        double c8d = 1.0;
        lands(c8, g0, 0, pads, &c8d, nullptr, nullptr);
        const f90::Out c0 = f90::decide(inOf(g, 0, 102, hip, f90::kItemPistol, 7, keys), g0, latch, rot, frameFn);
        check(c8.b == f90::kBMovedPrevious && dist3(c8.T, b1.T) == 0.0 && c8d < 1e-6 && c0.latched && latch.frame == 102,
              "F90 B: a 29498 draw ahead of the frame's latch uses the previous frame's T and is counted as such");
        // no T: two frames old, another item, or none yet
        const f90::Out n2 = f90::decide(inOf(g, 0, 104, pads, f90::kItemPistol, 7, keys), g0, latch, rot, frameFn);
        const f90::Out ni = f90::decide(inOf(g, 0, 103, pads, f90::kItemPistol, 8, keys), g0, latch, rot, frameFn);
        f90::Latch fresh;
        const f90::Out nn = f90::decide(inOf(g, 0, 103, pads, f90::kItemPistol, 7, keys), g0, fresh, rot, frameFn);
        check(n2.b == f90::kBRefNoTarget && ni.b == f90::kBRefNoTarget && nn.b == f90::kBRefNoTarget && !n2.map.move && !ni.map.move && !nn.map.move && nn.map.scale,
              "F90 B: 29498 with no T of this frame or the last (two frames old, another item, none yet) is refused and only scaled");
        // a pistol name whose serial is not the held item's: refused, rows untouched
        f90::Latch lm;
        f85::UFrame um = g0;
        const f90::Out m = f90::decide(inOf(g, 0, 110, hip, f90::kItemNotHeld, 7, keys), um, lm, rot, frameFn);
        check(m.b == f90::kBRefItem && m.map.identity() && !lm.have, "F90 B: a pistol readout whose item is not the held one is refused and left as F85b's");
    }

    // ---- 5: the weapon frame -----------------------------------------------------------------------------------------------------------------------------------
    {
        const Weapon wp = weaponOf(g, 7.0);
        const f90::WeaponFrame w = f90::weaponFrame(true, wp.attach, true, wp.froot);
        const double a = 7.0 * f85::kPi / 180.0;
        const double barrelModel[3] = {std::sin(a), 0.0, std::cos(a)};
        double bw[3], R0[3], R1[3], R2[3];
        throughRoot(wp.froot, barrelModel, bw);
        const double e0[3] = {1, 0, 0}, e1[3] = {0, 1, 0}, e2[3] = {0, 0, 1};
        throughRoot(wp.froot, e0, R0);
        throughRoot(wp.froot, e1, R1);
        throughRoot(wp.froot, e2, R2);
        double bu[3];
        edvr::nofoot::ikUnit(bw, bu);
        const double ortho = std::max({std::fabs(edvr::nofoot::ikDot(w.b, w.l)), std::fabs(edvr::nofoot::ikDot(w.b, w.u)), std::fabs(edvr::nofoot::ikDot(w.l, w.u)),
                                       std::fabs(edvr::nofoot::ikLen(w.b) - 1.0), std::fabs(edvr::nofoot::ikLen(w.l) - 1.0), std::fabs(edvr::nofoot::ikLen(w.u) - 1.0)});
        check(w.src == f90::kFrameAttach && dist3(w.b, bu) < 1e-6 && std::fabs(w.barrelDeg - 7.0) < 1e-4 && edvr::nofoot::ikDot(w.l, R0) < 0.0 && edvr::nofoot::ikDot(w.u, R1) > 0.9 &&
                  ortho < 1e-9,
              "F90 B: an attach 7 degrees off F_root gives b on its barrel (the signed row), l against F_root's right, u up; orthonormal to 1e-9");
        const f90::WeaponFrame st = f90::weaponFrame(false, wp.attach, true, wp.froot);
        double nR2[3], nR1[3], nR0[3];
        edvr::nofoot::ikUnit(R2, nR2);
        edvr::nofoot::ikUnit(R1, nR1);
        edvr::nofoot::ikUnit(R0, nR0);
        const double negR0[3] = {-nR0[0], -nR0[1], -nR0[2]};
        check(st.src == f90::kFrameRoot && dist3(st.b, nR2) < 1e-7 && dist3(st.u, nR1) < 1e-6 && dist3(st.l, negR0) < 1e-6,
              "F90 B: a stale snapshot takes F_root's rows (forward, up, minus right)");
        // an attach whose three rows each lie 54.7 degrees off the aim (forward = (1, 1, 1) / sqrt 3 in its axes)
        Weapon off = wp;
        {
            const double s2 = 1.0 / std::sqrt(2.0), s3 = 1.0 / std::sqrt(3.0), s6 = 1.0 / std::sqrt(6.0);
            const double rows[3][3] = {{s2, s6, s3}, {-s2, s6, s3}, {0.0, -2.0 * s6, s3}};
            for (int r = 0; r < 3; ++r)
                for (int k = 0; k < 3; ++k) off.attach[4 * r + k] = static_cast<float>(rows[r][k]);
        }
        check(f90::weaponFrame(true, off.attach, true, off.froot).src == f90::kFrameRoot, "F90 B: no attach row within 45 degrees of the aim takes F_root's rows");
        check(f90::weaponFrame(true, wp.attach, false, wp.froot).src == f90::kFrameNone && f90::weaponFrame(false, wp.attach, false, wp.froot).src == f90::kFrameNone,
              "F90 B: no fresh F_root: no weapon frame");
        // in the decision: a stale snapshot still moves (counted as F_root only); no frame leaves the rows unmoved (counted as none)
        const Record hip{kN89Hip, {0.10, -0.12, 0.36}, 0.07207, {2.0, 1.0}};
        f90::Keys keys;
        keys.forwardCm = 13.0;
        keys.leftCm = 6.0;
        keys.upCm = -3.0;
        keys.towardCm = 2.0;
        f90::Latch l1, l2;
        const f85::UFrame g0 = uOf(g, 0);
        f85::UFrame u1 = g0, u2 = g0;
        const f90::Out o1 = f90::decide(inOf(g, 0, 200, hip, f90::kItemPistol, 7, keys), u1, l1, rot, [&] { return f90::weaponFrame(false, wp.attach, true, wp.froot); });
        const f90::Out o2 = f90::decide(inOf(g, 0, 200, hip, f90::kItemPistol, 7, keys), u2, l2, rot, [&] { return f90::weaponFrame(false, wp.attach, false, wp.froot); });
        f90::applyMap(u2, o2.map);
        float ra[12], rb[12];
        check(o1.frameComputed && o1.wf.src == f90::kFrameRoot && o1.b == f90::kBMoved && o1.map.move,
              "F90 B: with a stale snapshot the pistol readout still moves, in F_root's frame (counted as F_root only)");
        check(o2.frameComputed && o2.wf.src == f90::kFrameNone && o2.b == f90::kBRefFrame && o2.map.identity() && !l2.have && rowsOf(g, 0, g0, ra) && rowsOf(g, 0, u2, rb) &&
                  std::memcmp(ra, rb, sizeof(ra)) == 0,
              "F90 B: with no weapon frame the rows are left as F85b's and the refusal is counted");
    }

    // ---- 6 (F91 3): a record that matches no row is not scaled, not moved, and counted ---------------------------------------------------------------------------
    {
        bool ok = true;
        std::vector<Record> misses = {{kN89RifleAds, {0.06, -0.10, 0.30}, 0.06, {3.0, 1.0}},                // F90's scale 0.06 for the rifle's ADS mesh
                                      {kN89RifleAds, {0.06, -0.10, 0.30}, 0.0501, {2.0, 1.0}},              // the rifle's ADS scale on a 2x1 mesh
                                      {kN90RifleAds, {0.06, -0.10, 0.30}, 0.0721, {3.0, 1.0}}};             // a 3x1 mesh at the hip's scale
        for (int r = 0; r < f90::kPartCount; ++r)   // each row's scale 2% off, either way
            for (const double f : {1.02, 0.98}) misses.push_back(Record{0, {0.06, -0.10, 0.30}, f90::kParts[r].scale * f, {f90::kParts[r].mesh[0], f90::kParts[r].mesh[1]}});
        for (const Record& rec : misses)
            for (const int item : {f90::kItemOther, f90::kItemPistol}) {
                f90::Latch latch;
                const f85::UFrame game = uOf(g, 0);
                f85::UFrame uf = game;
                const f90::Out o = f90::decide(inOf(g, 0, 300, rec, item, 7, f90::Keys{}), uf, latch, rot, [] { return f90::WeaponFrame{}; });
                f90::applyMap(uf, o.map);
                float ra[12], rb[12];
                ok = ok && o.role == f90::kRoleNone && o.a == f90::kANotTable && o.b == f90::kBNone && o.map.identity() && !latch.have && rowsOf(g, 0, game, ra) &&
                     rowsOf(g, 0, uf, rb) && std::memcmp(ra, rb, sizeof(ra)) == 0;
            }
        f90::In unread = inOf(g, 0, 300, ads, f90::kItemOther, 0, f90::Keys{});
        unread.meshRead = false;
        f90::Latch latch;
        ok = ok && f90::decide(unread, uOf(g, 0), latch, rot, [] { return f90::WeaponFrame{}; }).a == f90::kANotTable;
        check(ok && misses.size() == 11, "F91 A: a scale 2% off any row, a 3x1 mesh at 0.0721, F90's two mismatches and an unread mesh match no row: not scaled, not moved, the rows left as F85b's");
    }

    // ---- the keys -----------------------------------------------------------------------------------------------------------------------------------------------
    {
        const double nan = std::numeric_limits<double>::quiet_NaN();
        check(f88::clampCm(40.0, f90::kForwardMinCm, f90::kForwardMaxCm) == 30.0 && f88::clampCm(-20.0, f90::kLeftMinCm, f90::kLeftMaxCm) == -15.0 &&
                  f88::clampCm(12.0, f90::kUpMinCm, f90::kUpMaxCm) == 10.0 && f88::clampCm(-6.0, f90::kTowardMinCm, f90::kTowardMaxCm) == -5.0 &&
                  f88::clampCm(nan, f90::kTowardMinCm, f90::kTowardMaxCm) == 0.0 && !f90::Keys{}.any() &&
                  f88::clampCm(f90::kForwardDefaultCm, f90::kForwardMinCm, f90::kForwardMaxCm) == f90::kForwardDefaultCm &&
                  f88::clampCm(f90::kUpDefaultCm, f90::kUpMinCm, f90::kUpMaxCm) == f90::kUpDefaultCm,
              "F90 B: the keys clamp to forward -10..30, left -15..15, up -10..10, toward eye -5..10 cm; not a number reads 0; the defaults are inside");
    }
}

}  // namespace f90test
