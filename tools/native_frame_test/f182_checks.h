// F182 checks (native on-foot VR), run182 below. Header only, run by native_frame_test.
//
// F182a (native_on_foot_f149.h): the plasma shotgun's ADS eye 9.5 cm forward, baked: its depth is 9.51 (0.01 + 9.5) at the trim 0, the other rows as they were. F182b (native_on_foot_f180.h): the
// spine straighten is one-sided per bone about the body's right axis: written lean = animated - share x max(0, animated - reference), the level's other components as animated, a bone at or
// behind the reference (looking up) as animated; the rig runs oneSided and applyOneSided on a toy skeleton with a roll and a forward offset in every level (so the bones have x and z parts), on the
// flight's numbers (all written = animated), on a forward-hunched case, on a mixed case (a turned bone above a bone left as animated), at share 0, then the counters, the line and the stage's
// source sites. F182c (native_on_foot_f182.h): the left hand stays on a two-handed gun aimed steeply up: the gate (sights, two-handed, pitch >= 45), the held point in the item's frame under an item
// move, the 3 s limit, the release, nothing outside the gate, the counters and the F67f line's ending, and the source sites.
#pragma once

#include "../../src/d3d11/journal_loadout.h"
#include "../../src/d3d11/native_on_foot_core.h"
#include "../../src/d3d11/native_on_foot_f149.h"
#include "../../src/d3d11/native_on_foot_f166.h"
#include "../../src/d3d11/native_on_foot_f180.h"
#include "../../src/d3d11/native_on_foot_f182.h"
#include "f180_checks.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>

namespace f182test {

namespace nf = edvr::nofoot;
namespace f149 = edvr::nofoot::f149;
namespace f166 = edvr::nofoot::f166;
namespace f180 = edvr::nofoot::f180;
namespace f182 = edvr::nofoot::f182;
namespace ls = edvr::loadout;

constexpr double kPi = 3.14159265358979323846;
constexpr uint32_t kJ = f180test::kJoints;   // the toy skeleton's joints (at namespace scope: the lambdas below size arrays with it)

inline std::string slurp(const char* path) {
    std::ifstream f(path, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// ---- 3x3 row-vector rotations, for a toy pose with a roll and an offset in every level --------------------------------------------------------------------------------------------------
using R3 = double[3][3];
inline void rotX(double deg, R3 r) {   // [[1,0,0],[0,c,s],[0,-s,c]]: adds deg to atan2(z, y) of a row vector in the y-z plane
    const double c = std::cos(deg * kPi / 180.0), s = std::sin(deg * kPi / 180.0);
    const double v[3][3] = {{1, 0, 0}, {0, c, s}, {0, -s, c}};
    std::memcpy(r, v, sizeof(v));
}
inline void rotZ(double deg, R3 r) {
    const double c = std::cos(deg * kPi / 180.0), s = std::sin(deg * kPi / 180.0);
    const double v[3][3] = {{c, s, 0}, {-s, c, 0}, {0, 0, 1}};
    std::memcpy(r, v, sizeof(v));
}
inline void rotY(double deg, R3 r) {
    const double c = std::cos(deg * kPi / 180.0), s = std::sin(deg * kPi / 180.0);
    const double v[3][3] = {{c, 0, -s}, {0, 1, 0}, {s, 0, c}};
    std::memcpy(r, v, sizeof(v));
}
inline void mul3(const R3 a, const R3 b, R3 out) {   // row vectors: apply a, then b
    double t[3][3];
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) t[i][j] = a[i][0] * b[0][j] + a[i][1] * b[1][j] + a[i][2] * b[2][j];
    std::memcpy(out, t, sizeof(t));
}
inline void matOf3(const R3 r, double x, double y, double z, float out[16]) {
    std::memset(out, 0, 16 * sizeof(float));
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) out[i * 4 + j] = static_cast<float>(r[i][j]);
    out[12] = static_cast<float>(x); out[13] = static_cast<float>(y); out[14] = static_cast<float>(z); out[15] = 1.0f;
}

// A pose of the toy skeleton: every joint's local is a rotation about x by pitch[j] then about z by roll[j], at the offset (x[j], 0.10, 0.03) against its parent (the clavicles at +-0.05);
// the model matrices are local x parent's model.
inline void poseOf3(const f180test::Skel& k, const double pitch[f180test::kJoints], const double roll[f180test::kJoints], float (*out)[16]) {
    for (uint32_t j = 0; j < f180test::kJoints; ++j) {
        R3 rx, rz, r;
        rotX(pitch[j], rx);
        rotZ(roll[j], rz);
        mul3(rx, rz, r);
        float local[16];
        const double ox = j == 35 ? -0.05 : j == 36 ? 0.05 : (j == 21 ? 0.012 : j == 24 ? -0.008 : j == 34 ? 0.01 : 0.0);
        matOf3(r, ox, 0.10, 0.03, local);
        if (k.parents[j] == 0xFFFF) std::memcpy(out[j], local, sizeof(local));
        else nf::pfzMul(local, out[k.parents[j]], out[j]);
    }
}
// The model x part of each of the four bones.
inline void boneX(const float (*m)[16], double x[4]) {
    for (int i = 0; i < 4; ++i) x[i] = static_cast<double>(m[f180::kLevelNext[i]][12]) - static_cast<double>(m[f180::kLevelJoint[i]][12]);
}

// ---- F182c: rigid matrices and the scene's calls ------------------------------------------------------------------------------------------------------------------------------------------
inline void rigid(double yawDeg, double pitchDeg, double x, double y, double z, float out[16]) {
    R3 a, b, r;
    rotY(yawDeg, a);
    rotX(pitchDeg, b);
    mul3(a, b, r);
    matOf3(r, x, y, z, out);
}
inline double maxAbsDiff(const float a[16], const float b[16]) {
    double m = 0.0;
    for (int i = 0; i < 16; ++i) m = std::fmax(m, std::fabs(static_cast<double>(a[i]) - static_cast<double>(b[i])));
    return m;
}

template <class Check>
inline void run182(Check&& check) {
    const auto eq = [](double a, double b, double tol) { return std::fabs(a - b) <= tol; };
    char msg[700];

    // ---- F182a. the shotgun's eye forward, baked ----------------------------------------------------------------------------------------------------------------------------------------------
    {
        const char* shotgun = "$wpn_m_shotgun_plasma_doublebarrel_name;";
        const f149::EyeDepth s0 = f149::eyeDepth(0.0, true, shotgun, ls::kScopeNo, nullptr);
        const f149::EyeDepth s2 = f149::eyeDepth(2.0, true, shotgun, ls::kScopeNo, nullptr);
        const f149::EyeDepth ar = f149::eyeDepth(0.0, true, "$wpn_m_assaultrifle_plasma_fauto_name;", ls::kScopeNo, nullptr);
        const f149::EyeDepth kar = f149::eyeDepth(0.0, true, "$wpn_m_assaultrifle_kinetic_fauto_name;", ls::kScopeNo, nullptr);
        const f149::EyeDepth smg = f149::eyeDepth(0.0, true, "$wpn_m_submachinegun_laser_fauto_name;", ls::kScopeNo, nullptr);
        check(eq(s0.depthCm, 9.51, 1e-9) && s0.offsetCm == 9.5 && s0.baseCm == 0.01 && eq(s2.askedCm, 11.51, 1e-9) && f149::kAdsBackOffCount == 3 && eq(ar.depthCm, 12.99, 1e-9) && ar.offsetCm == 0.0 &&
                  kar.offsetCm == -19.0 && eq(kar.depthCm, 6.0, 1e-9) && smg.offsetCm == -3.0 && eq(smg.depthCm, 12.0, 1e-9),
              "F182a: the plasma shotgun's depth is 9.51 cm (0.01 + 9.5 forward) at the trim 0 and 11.51 at +2; the plasma assault rifle stays 12.99, the kinetic one 6.0 and the laser SMG 12.0, as they were");
    }

    // ---- F182b 1. the one-sided rule ---------------------------------------------------------------------------------------------------------------------------------------------------------
    {
        // the flight edvr_gfx_20261030_115059 at aim pitch +74.6: animated -4.3/-14.7/-26.3/-6.9, reference +29.7/+28.7/+38.4/+55.6, the share at the dial 0.9
        const double animUp[4] = {-4.3, -14.7, -26.3, -6.9}, refUp[4] = {29.7, 28.7, 38.4, 55.6};
        for (double share : {0.9, 0.76, 1.0}) {
            const f180::OneSided o = f180::oneSided(animUp, refUp, share);
            check(!o.any && o.cum[0] == 0.0 && o.cum[1] == 0.0 && o.cum[2] == 0.0 && o.cum[3] == 0.0 && o.left[0] && o.left[1] && o.left[2] && o.left[3],
                  "F182b: on the flight's numbers (looking up, every bone behind the reference) nothing is taken back and every bone is counted as left as animated: written = animated");
        }
        // a forward-hunched case: the animation leans 25/28/20/14 deg beyond the reference; the share 0.8 takes 80% of the excess back
        const double animFwd[4] = {45.0, 50.0, 60.0, 70.0}, refFwd[4] = {20.0, 22.0, 40.0, 56.0};
        const f180::OneSided h = f180::oneSided(animFwd, refFwd, 0.8);
        check(h.any && eq(h.cum[0], 20.0, 1e-12) && eq(h.cum[1], 22.4, 1e-12) && eq(h.cum[2], 16.0, 1e-12) && eq(h.cum[3], 11.2, 1e-12) && !h.left[0] && !h.left[1] && !h.left[2] && !h.left[3],
              "F182b: a forward-hunched case is taken back by the share of the excess beyond the reference: 25/28/20/14 deg over at 0.8 is 20/22.4/16/11.2 deg");
        // a mixed case: bone 0 and 3 over the reference, bones 1 and 2 behind it
        const double animMix[4] = {28.0, 10.0, 14.0, 40.0}, refMix[4] = {20.0, 25.0, 30.0, 34.0};
        const f180::OneSided m = f180::oneSided(animMix, refMix, 0.5);
        check(m.any && eq(m.cum[0], 4.0, 1e-12) && m.cum[1] == 0.0 && m.cum[2] == 0.0 && eq(m.cum[3], 3.0, 1e-12) && !m.left[0] && m.left[1] && m.left[2] && !m.left[3],
              "F182b: a bone behind the reference is left as animated and counted, the bones over it are taken back, in the same call");
        // a bone exactly at the reference is left as animated; the share 0 takes nothing back
        const double refEq[4] = {45.0, 50.0, 60.0, 70.0};
        const f180::OneSided e = f180::oneSided(animFwd, refEq, 1.0), z = f180::oneSided(animFwd, refFwd, 0.0);
        check(!e.any && e.left[0] && e.left[3] && !z.any && z.cum[0] == 0.0 && z.cum[3] == 0.0 && !z.left[0],
              "F182b: a bone level with the reference is left as animated; at the share 0 (the dial 0, crouched, the hip) nothing is taken back and nothing is counted as left");
        // a lean that is not a number is left alone and uncounted; the share is held to 0..1
        const double animNan[4] = {std::nan(""), 45.0, 45.0, 45.0}, refNan[4] = {20.0, std::nan(""), 20.0, 20.0};
        const f180::OneSided n = f180::oneSided(animNan, refNan, 2.0);
        check(n.cum[0] == 0.0 && !n.left[0] && n.cum[1] == 0.0 && !n.left[1] && eq(n.cum[2], 25.0, 1e-12) && eq(n.cum[3], 25.0, 1e-12) && f180::oneSided(animFwd, refFwd, std::nan("")).any == false,
              "F182b: a bone whose lean (animated or reference) is not a number is left alone and not counted; a share past 1 reads 1, one that is not a number reads 0");
    }

    // ---- F182b 2. applyOneSided on a toy skeleton ----------------------------------------------------------------------------------------------------------------------------------------------
    {
        const f180test::Skel skel;
        const f166::Plan plan = f180::buildPlan(skel.parents, kJ);
        double zeroP[kJ] = {}, probeP[kJ] = {}, roll[kJ] = {};
        probeP[16] = 10.0;
        roll[16] = 3.0; roll[21] = -2.0; roll[24] = 4.0; roll[34] = 1.0;
        float pz[kJ][16], pp[kJ][16];
        double noRoll[kJ] = {};
        poseOf3(skel, zeroP, noRoll, pz);   // no roll: a pitch alone turns every bone's (y, z) part, so its lean changes by exactly the pitch
        poseOf3(skel, probeP, noRoll, pp);
        double lz[4], lp[4];
        f180test::leansOf(pz, lz);
        f180test::leansOf(pp, lp);
        const double sg = lp[0] - lz[0] >= 0.0 ? 1.0 : -1.0;   // the toy's sense (+1: a positive pitch leans forward)
        check(eq(lp[0] - lz[0], sg * 10.0, 1e-3) && eq(lp[3] - lz[3], sg * 10.0, 1e-3), "F182b: the toy's right-axis turn of 10 deg on the spine root changes every bone's lean by 10 deg (the sense the rule's turns use)");

        struct Case {
            double anim[4], ref[4], cum[4], written[4];
            bool any, applied;
            double xMove, carriedMove;
            uint32_t touchedN;
            bool heldTouched, otherTouched;
        };
        const auto run = [&](const double animP[kJ], const double refP[kJ], double shareV) {
            Case c{};
            float animM[kJ][16], refM[kJ][16], out[kJ][16], full[kJ][16];
            poseOf3(skel, animP, roll, animM);
            poseOf3(skel, refP, roll, refM);
            f180test::leansOf(animM, c.anim);
            f180test::leansOf(refM, c.ref);
            const f180::OneSided o = f180::oneSided(c.anim, c.ref, shareV);
            for (int k = 0; k < 4; ++k) c.cum[k] = o.cum[k];
            c.any = o.any;
            uint16_t touched[kJ];
            uint32_t n = 0;
            c.applied = f180::applyOneSided(plan, o.cum, animM, kJ, out, touched, &n);
            std::memcpy(full, animM, sizeof(animM));
            for (uint32_t i = 0; i < n; ++i) std::memcpy(full[touched[i]], out[touched[i]], 64);
            f180test::leansOf(full, c.written);
            double xa[4], xw[4];
            boneX(animM, xa);
            boneX(full, xw);
            for (int k = 0; k < 4; ++k) c.xMove = std::fmax(c.xMove, std::fabs(xa[k] - xw[k]));
            for (uint32_t j : {25u, 35u, 36u, 40u, 44u, 46u}) {
                float la[16], lw[16];
                f180test::localOf(animM, j, skel.parents[j], la);
                f180test::localOf(full, j, skel.parents[j], lw);
                c.carriedMove = std::fmax(c.carriedMove, maxAbsDiff(la, lw));
            }
            c.touchedN = n;
            bool h16 = false, h21 = false, h24 = false, h34 = false, other = false;
            for (uint32_t i = 0; i < n; ++i) {
                h16 = h16 || touched[i] == 16; h21 = h21 || touched[i] == 21; h24 = h24 || touched[i] == 24; h34 = h34 || touched[i] == 34;
                other = other || touched[i] == 0 || touched[i] == 1 || touched[i] == 15;
            }
            c.heldTouched = h16 && h21 && h24 && h34;
            c.otherTouched = other;
            return c;
        };
        const auto pitches = [&](double a, double b, double c3, double d, double (&p)[kJ]) {
            for (uint32_t j = 0; j < kJ; ++j) p[j] = 0.0;
            p[16] = sg * a; p[21] = sg * b; p[24] = sg * c3; p[34] = sg * d;
        };
        double refP[kJ], hunchP[kJ], upP[kJ], mixP[kJ];
        pitches(2.0, 3.0, 2.0, 4.0, refP);
        pitches(10.0, 12.0, 8.0, 15.0, hunchP);
        pitches(-6.0, -5.0, -8.0, -10.0, upP);
        pitches(10.0, -14.0, 6.0, 15.0, mixP);

        // forward-hunched at the share 0.8: written = animated - 0.8 x (animated - reference) in every bone, and at the share 1 the reference's lean
        const Case h = run(hunchP, refP, 0.8), h1 = run(hunchP, refP, 1.0);
        std::snprintf(msg, sizeof(msg), "F182b: forward-hunched at the share 0.8 each bone is written at animated less 0.8 x its excess over the reference (animated %.2f/%.2f/%.2f/%.2f, reference %.2f/%.2f/%.2f/%.2f, written %.2f/%.2f/%.2f/%.2f); at the share 1 the reference's lean",
                      h.anim[0], h.anim[1], h.anim[2], h.anim[3], h.ref[0], h.ref[1], h.ref[2], h.ref[3], h.written[0], h.written[1], h.written[2], h.written[3]);
        bool hunchOk = h.applied && h.any && h1.applied;
        for (int k = 0; k < 4; ++k)
            hunchOk = hunchOk && h.cum[k] > 1.0 && eq(h.written[k], h.anim[k] - h.cum[k], 1e-3) && eq(h.cum[k], 0.8 * (h.anim[k] - h.ref[k]), 1e-9) && eq(h1.written[k], h1.ref[k], 1e-3);
        check(hunchOk, msg);

        // the flight's case: looking up, every bone behind the reference: nothing taken back, the pose written is the animated one (the turns are all 0)
        const Case u = run(upP, refP, 0.9);
        bool upOk = u.applied && !u.any;
        for (int k = 0; k < 4; ++k) upOk = upOk && u.cum[k] == 0.0 && u.anim[k] < u.ref[k] && eq(u.written[k], u.anim[k], 1e-3);
        check(upOk, "F182b: looking up (every bone's animated lean behind the reference's) the written bones are the animated ones, in every level, at the share 0.9");

        // mixed: bone 0 and 3 over the reference, bones 1 and 2 behind it: bone 1 is written as animated although the level above was turned (the turn is undone there)
        const Case m = run(mixP, refP, 0.8);
        bool mixOk = m.applied && m.any && m.cum[0] > 1.0 && m.cum[1] == 0.0 && m.cum[2] == 0.0 && m.cum[3] > 1.0;
        for (int k = 0; k < 4; ++k) mixOk = mixOk && eq(m.written[k], m.anim[k] - m.cum[k], 1e-3);
        std::snprintf(msg, sizeof(msg), "F182b: a turned bone above a bone left as animated: bone 0 and 3 are taken back (%.2f, %.2f deg), bones 1 and 2 are written exactly as animated (the turn above them is undone at their level): written %.2f/%.2f/%.2f/%.2f against animated %.2f/%.2f/%.2f/%.2f",
                      m.cum[0], m.cum[3], m.written[0], m.written[1], m.written[2], m.written[3], m.anim[0], m.anim[1], m.anim[2], m.anim[3]);
        check(mixOk, msg);

        // the level's other components stay as animated; everything under a level keeps its local against it; the root and the legs' side are not written
        std::snprintf(msg, sizeof(msg), "F182b: the turns are about the body's right axis only: every bone's model x part is unchanged (%.2e), every joint under a level keeps its local against it (%.2e), the four levels are written and the root and the joints below the spine root are not",
                      std::fmax(h.xMove, m.xMove), std::fmax(h.carriedMove, m.carriedMove));
        check(h.xMove < 1e-5 && m.xMove < 1e-5 && h.carriedMove < 1e-5 && m.carriedMove < 1e-5 && h.heldTouched && m.heldTouched && !h.otherTouched && !m.otherTouched && h.touchedN > 4,
              msg);

        // the share 0: nothing is taken back and nothing is counted; applyOneSided with all-zero turns leaves every joint where it was
        const Case z = run(hunchP, refP, 0.0);
        bool zeroOk = z.applied && !z.any;
        for (int k = 0; k < 4; ++k) zeroOk = zeroOk && z.cum[k] == 0.0 && eq(z.written[k], z.anim[k], 1e-3);
        check(zeroOk, "F182b: at the share 0 (the dial 0) no turn is made: the written pose is the animated one");

        // refusals: a singular matrix, a turn that is not a number, too few joints, a plan without the neck
        float animM[kJ][16], out[kJ][16], bad[kJ][16];
        poseOf3(skel, hunchP, roll, animM);
        std::memcpy(bad, animM, sizeof(animM));
        for (int c = 0; c < 16; ++c) bad[16][c] = 0.0f;
        const double cumOk[4] = {5.0, 5.0, 5.0, 5.0}, cumNan[4] = {5.0, std::nan(""), 5.0, 5.0};
        uint16_t touched[kJ];
        uint32_t touchedN = 7;
        const bool singular = !f180::applyOneSided(plan, cumOk, bad, kJ, out, touched, &touchedN) && touchedN == 0;
        const bool nanTurn = !f180::applyOneSided(plan, cumNan, animM, kJ, out, touched, &touchedN);
        const bool fewJoints = !f180::applyOneSided(plan, cumOk, animM, 30, out, touched, &touchedN);
        f180test::Skel odd;
        odd.parents[34] = 23;
        const bool noNeck = !f180::applyOneSided(f180::buildPlan(odd.parents, kJ), cumOk, animM, kJ, out, touched, &touchedN);
        check(singular && nanTurn && fewJoints && noNeck, "F182b: a singular matrix, a turn that is not a number, too few joints and a plan without the neck write nothing and say so");
    }

    // ---- F182b 3. the counters and the line --------------------------------------------------------------------------------------------------------------------------------------------------
    {
        f180::Counters c;
        for (int i = 0; i < 100; ++i) {
            f180::Sample s;
            s.dial = 0.9;
            s.share = 0.76;
            s.wrote = true;
            s.source = f166::kStill;
            s.raiseAskedM = 0.09;
            s.leftAsIs[0] = i < 70; s.leftAsIs[1] = i < 70; s.leftAsIs[2] = i < 50; s.leftAsIs[3] = i < 20;
            const double anim[4] = {-4.3, -14.7, -26.3, -6.9}, ref[4] = {29.7, 28.7, 38.4, 55.6};
            for (int k = 0; k < 4; ++k) { s.animLean[k] = anim[k]; s.refLean[k] = ref[k]; s.writtenLean[k] = anim[k]; }
            c.add(s);
        }
        check(c.frames == 100 && c.wrote == 100 && c.leftN[0] == 70 && c.leftN[1] == 70 && c.leftN[2] == 50 && c.leftN[3] == 20,
              "F182b: the counters keep, per bone, the frames left as animated; 'written' still counts every frame the rule was made in");
        f180::Counters m;
        m.merge(c);
        m.merge(c);
        check(m.leftN[0] == 140 && m.leftN[2] == 100 && m.leftN[3] == 40 && m.wrote == 200, "F182b: two windows add up, the left-as-animated frames too");
        char line[1400];
        const int len = f180::formatSummary(line, sizeof(line), 10, "$wpn_m_shotgun_plasma_doublebarrel_name;", c);
        check(len > 0 && std::strstr(line, "written in 100 frames;") != nullptr && std::strstr(line, "spine 16 -4.3/+29.7/-4.3") != nullptr &&
                  std::strstr(line, "F179a's leg raise asked mean +9.0 cm; left as animated, leaning back of the reference (frames): spine 16 70, spine 21 70, chest 24 50, neck 34 20") != nullptr,
              "F182b: the line ends with the frames left as animated per bone (after F179a's raise), and keeps its leading figures");
        f180::Counters big;
        big.frames = big.wrote = 900; big.src[1] = 900; big.dialSum = 900.0; big.dialMax = 1.0; big.shareSum = 900.0; big.raiseSum = 900.0 * 0.2;
        for (int t = 0; t < 3; ++t)
            for (int k = 0; k < 4; ++k) { big.leanN[t][k] = 900; big.leanSum[t][k] = -180.0 * 900.0; }
        for (int i = 0; i < 2; ++i) {
            big.neckN[i] = big.shoulderN[i] = 900;
            for (int c3 = 0; c3 < 3; ++c3) { big.neckSum[i][c3] = -900.0; big.shoulderSum[i][c3] = -900.0; }
        }
        for (int k = 0; k < 4; ++k) big.leftN[k] = 4000000000u;
        char worst[1400];
        const int worstLen = f180::formatSummary(worst, sizeof(worst), 10, "$wpn_m_shotgun_plasma_doublebarrel_name_with_a_long_tail;", big);
        std::snprintf(msg, sizeof(msg), "F182b: the line stays within the log's 1166 bytes (1200 less its stamp and truncation marker) with the new ending at its worst (%d bytes typical, %d worst)", len, worstLen);
        check(worstLen > 0 && worstLen < 1166 && len < 1166, msg);
    }

    // ---- F182c 1. the gate and the held point ---------------------------------------------------------------------------------------------------------------------------------------------
    {
        check(f182::steepSights(0.5, 45.0) && f182::steepSights(1.0, 74.6) && !f182::steepSights(0.49, 74.6) && !f182::steepSights(1.0, 44.9) && !f182::steepSights(0.0, 90.0) &&
                  !f182::steepSights(std::nan(""), 60.0) && !f182::steepSights(1.0, std::nan("")) && f182::kSteepDeg == 45.0 && f182::kDetachMin == 0.5 && f182::kHoldMaxSec == 3.0 && f182::kMinSteepCalls == 30,
              "F182c: the gate is the sights (detach weight 0.5 or more) and the aim pitch 45 deg or more up; a value that is not a number is neither; 3 s of hook time, 30 calls to judge");
        float g[16] = {};
        g[9] = static_cast<float>(std::sin(74.6 * kPi / 180.0));
        g[10] = static_cast<float>(std::cos(74.6 * kPi / 180.0));
        float gDown[16] = {};
        gDown[9] = static_cast<float>(-std::sin(23.9 * kPi / 180.0));
        gDown[10] = static_cast<float>(std::cos(23.9 * kPi / 180.0));
        float gZero[16] = {};
        check(eq(f182::aimPitchDeg(g), 74.6, 1e-3) && eq(f182::aimPitchDeg(gDown), -23.9, 1e-3) && std::isnan(f182::aimPitchDeg(gZero)),
              "F182c: the aim's pitch is read from G's forward row (up positive), as the pin's: +74.6 and -23.9 deg, not a number for a zero row");

        float L0[16], R0[16], rel[16], back[16];
        rigid(-15.0, 5.0, 0.12, 1.28, 0.45, L0);
        rigid(20.0, -10.0, 0.30, 1.30, 0.20, R0);
        check(f182::relativeOf(L0, R0, rel) && (f182::leftFrom(rel, R0, back), maxAbsDiff(back, L0) < 1e-5), "F182c: the left target kept in the right target's frame comes back as it was (left = relative x right)");
        float zeroM[16] = {};
        check(!f182::relativeOf(L0, zeroM, rel), "F182c: a singular right target makes no relative");

        const auto mk = [&](const float* left, const float* right) {
            f182::In in;
            in.armed = in.gun = in.liveOk = true;
            in.two = true;
            in.offW = 1.0;
            in.detachW = 1.0;
            in.pitchDeg = 74.6;
            in.dtSec = 0.011;
            in.nowMs = 1000;
            in.item = 7; in.planGen = 3; in.fpGen = 5; in.epoch = 0;
            in.modeLeft = 1; in.modeRight = 1;
            in.left = left; in.right = right;
            return in;
        };
        const auto freeIn = [&](f182::In in) {
            in.two = false;
            in.offW = 0.0;
            return in;
        };

        // two-handed calls store the point; steep, not freed, not held
        f182::State st;
        f182::Out o;
        bool storedAll = true, steepAll = true, notHeld = true;
        for (int i = 0; i < 20; ++i) {
            f182::In in = mk(L0, R0);
            in.nowMs = 1000 + 11 * i;
            o = st.step(in);
            storedAll = storedAll && o.stored;
            steepAll = steepAll && o.steep && !o.freed;
            notHeld = notHeld && !o.held && o.w == 0.0 && !o.limit;
        }
        check(storedAll && steepAll && notHeld && st.have, "F182c: while F69h holds the pair at weight 1 the point is stored on every call, the call is steep and not freed, and nothing is held");

        // F69h lets go (an unreadable input): held on the first call, whole weight, at the stored point, 40 ms old
        f182::In fin = freeIn(mk(L0, R0));
        fin.offW = 0.85;   // its ramp falling
        fin.two = false;
        fin.nowMs = 1000 + 11 * 19 + 40;
        o = st.step(fin);
        check(o.steep && o.freed && o.held && o.w == 1.0 && !o.limit && !o.stored && maxAbsDiff(o.left, L0) < 1e-5 && eq(o.ageMs, 40.0, 1e-9),
              "F182c: the call F69h lets go of the hand in is held at once: the left target is the stored point at weight 1, the point 40 ms old; the call is counted steep and freed");

        // the item moves (the right target and the live left both unreliable): the held left follows the right in the item's frame
        float T[16], R1[16], expectL[16];
        rigid(30.0, 8.0, 0.20, -0.10, 0.40, T);
        nf::pfzMul(R0, T, R1);
        nf::pfzMul(L0, T, expectL);
        f182::In moved = freeIn(mk(L0, R1));
        moved.nowMs = fin.nowMs + 11;
        o = st.step(moved);
        std::snprintf(msg, sizeof(msg), "F182c: the item moves (the right target by a rigid move): the held left target moves with it, in the item's frame (max element difference %.2e from the left target moved the same way)", maxAbsDiff(o.left, expectL));
        check(o.held && maxAbsDiff(o.left, expectL) < 1e-4 && maxAbsDiff(o.left, L0) > 0.05, msg);

        // the 3 s limit: a freeze does not spend it (a step is held to 0.1 s), 0.1 s steps do; once, then released over 0.15 s and not taken again while the condition holds
        f182::State s2;
        for (int i = 0; i < 5; ++i) s2.step(mk(L0, R0));
        f182::In big = freeIn(mk(L0, R0));
        big.dtSec = 5.0;
        o = s2.step(big);
        f182::In slow = freeIn(mk(L0, R0));
        slow.dtSec = 0.1;
        int limitAt = 0, limits = 0, heldAfterLimit = 0;
        bool heldBefore = o.held;
        for (int i = 1; i <= 60; ++i) {
            o = s2.step(slow);
            if (o.limit) { ++limits; if (!limitAt) limitAt = i; }
            if (limitAt && i > limitAt && o.held) ++heldAfterLimit;
        }
        std::snprintf(msg, sizeof(msg), "F182c: the 3 s limit ends the hold once (at call %d of 0.1 s after a 5 s freeze that counted 0.1 s), the hold falls over the next call (%d held after it) and is not taken again while F69h stays off", limitAt, heldAfterLimit);
        check(heldBefore && limits == 1 && limitAt >= 29 && limitAt <= 31 && heldAfterLimit <= 2 && !o.held && o.w == 0.0, msg);
        // F69h taking the pair back ends the stretch: a later release is held again
        f182::In two = mk(L0, R0);
        o = s2.step(two);
        const f182::Out again = s2.step(freeIn(mk(L0, R0)));
        check(!o.held && o.w == 0.0 && again.held && again.w == 1.0 && !again.limit, "F182c: F69h holding the pair again ends the hold at once (weight 0), and the next release is held afresh");

        // the release: the pitch falls under 45 deg while F69h is still off: the weight falls over 0.15 s, the left target still the point's meanwhile
        f182::State s3;
        for (int i = 0; i < 5; ++i) s3.step(mk(L0, R0));
        for (int i = 0; i < 3; ++i) s3.step(freeIn(mk(L0, R0)));
        f182::In level = freeIn(mk(L0, R0));
        level.pitchDeg = 30.0;
        level.dtSec = 0.05;
        f182::In level3 = level;
        level3.dtSec = 0.06;
        const f182::Out r1 = s3.step(level), r2 = s3.step(level), r3 = s3.step(level3);
        check(r1.held && eq(r1.w, 1.0 - 0.05 / 0.15, 1e-9) && !r1.steep && r2.held && eq(r2.w, 1.0 - 2.0 * 0.05 / 0.15, 1e-9) && !r3.held && r3.w == 0.0,
              "F182c: when the pitch falls under 45 deg the hold lets go over 0.15 s (weight 1 -> 0.67 -> 0.33 -> 0), the point still the left target while it falls");

        // nothing outside the gate: a level aim, a detach under 0.5, no sights at all
        bool outside = true;
        for (int variant = 0; variant < 3; ++variant) {
            f182::State s4;
            for (int i = 0; i < 5; ++i) s4.step(mk(L0, R0));
            for (int i = 0; i < 20; ++i) {
                f182::In in = freeIn(mk(L0, R0));
                if (variant == 0) in.pitchDeg = 20.0;
                else if (variant == 1) in.detachW = 0.3;
                else in.detachW = 0.0;
                const f182::Out q = s4.step(in);
                outside = outside && !q.held && !q.steep && !q.freed && q.w == 0.0 && !q.limit;
            }
        }
        check(outside, "F182c: a level aim (20 deg), a detach weight under 0.5 and the hip never hold: F69h's decision is the only one, as it was");

        // a pistol or tool (gun false), unarmed, a hand that was never two-handed, another item or rig, a right target made another way, no right target
        bool never = true;
        {
            f182::State s5;   // never two-handed: the first acquisition is F69h's alone
            f182::In in = freeIn(mk(L0, R0));
            for (int i = 0; i < 10; ++i) {
                const f182::Out q = s5.step(in);
                never = never && !q.held && !q.steep && !q.freed;
            }
        }
        f182::State s6;
        for (int i = 0; i < 5; ++i) s6.step(mk(L0, R0));
        f182::In pistol = freeIn(mk(L0, R0));
        pistol.gun = false;
        const f182::Out p1 = s6.step(pistol);
        const f182::Out p2 = s6.step(freeIn(mk(L0, R0)));   // the point was dropped with the gun: nothing to hold
        f182::State s7;
        for (int i = 0; i < 5; ++i) s7.step(mk(L0, R0));
        f182::In unarmed = freeIn(mk(L0, R0));
        unarmed.armed = false;
        const f182::Out u1 = s7.step(unarmed);
        const f182::Out u2 = s7.step(freeIn(mk(L0, R0)));
        f182::State s8;
        for (int i = 0; i < 5; ++i) s8.step(mk(L0, R0));
        f182::In otherItem = freeIn(mk(L0, R0));
        otherItem.item = 8;
        const f182::Out i1 = s8.step(otherItem);
        f182::State s9;
        for (int i = 0; i < 5; ++i) s9.step(mk(L0, R0));
        f182::In otherRig = freeIn(mk(L0, R0));
        otherRig.fpGen = 6;
        const f182::Out g1 = s9.step(otherRig);
        f182::State s10;
        for (int i = 0; i < 5; ++i) s10.step(mk(L0, R0));
        f182::In otherMode = freeIn(mk(L0, R0));
        otherMode.modeRight = 3;
        const f182::Out m1 = s10.step(otherMode);
        f182::In noRight = freeIn(mk(L0, R0));
        noRight.liveOk = false;
        const f182::Out n1 = s10.step(noRight);
        check(never && !p1.held && !p1.steep && !p2.held && !p2.steep && !u1.held && !u2.held && !i1.held && !i1.steep && !g1.held && !m1.held && !n1.held && m1.w == 0.0 && n1.w == 0.0,
              "F182c: a pistol or tool, unarmed, a hand never two-handed on this item, another item or rig (the point is dropped), a right target made another way and no right target are never held");
    }

    // ---- F182c 2. the counters and the F67f line's ending -------------------------------------------------------------------------------------------------------------------------------------
    {
        f182::Counters c;
        char clause[600];
        check(f182::formatClause(clause, sizeof(clause), c) == 0 && clause[0] == '\0', "F182c: a window with no steep call has no ending on the F67f line");
        f182::Out steepOnly, freed, held;
        steepOnly.steep = true;
        freed.steep = freed.freed = true;
        held.steep = held.freed = held.held = true;
        held.ageMs = 40.0;
        for (int i = 0; i < 60; ++i) c.add(steepOnly);
        for (int i = 0; i < 20; ++i) c.add(freed);
        for (int i = 0; i < 20; ++i) { held.ageMs = 20.0 + 2.0 * i; c.add(held); c.addError(0.30 + 0.02 * i); }
        f182::Out lim;
        lim.limit = true;
        c.add(lim);
        const int n = f182::formatClause(clause, sizeof(clause), c);
        std::snprintf(msg, sizeof(msg), "F182c: the F67f line's ending names the steep calls (100), those F69h freed the hand in (40), those held (20), the point's age mean/max, the held calls' hand-to-grip error and the limit (%d bytes)", n);
        check(n > 0 && n < 420 && std::strstr(clause, "; steep two-handed ADS (F182c): 100 calls (the sights, the aim pitch 45 deg or more up), F69h freed the off hand in 40, held on the item by F182c 20 (the pitch over 45 deg), "
                                                    "its grip point age mean 39 max 58 ms, hand-to-grip error of the held calls mean 0.49 max 0.68 cm, ended by the 3 s limit 1") != nullptr,
              msg);
        check(c.steep == 100 && c.freed == 40 && c.held == 20 && c.limit == 1 && c.errN == 20, "F182c: the counters keep the steep, freed and held calls, the limit's endings and the error samples");
        f182::Counters bad;
        bad.addError(std::nan(""));
        check(bad.errN == 0, "F182c: an error that is not a number is not counted");
    }

    // ---- sources: the stage and the hook -------------------------------------------------------------------------------------------------------------------------------------------------------
    {
        const std::string cpp = slurp("src/d3d11/native_on_foot.cpp");
        const auto has = [&](const std::string& needle) { return cpp.find(needle) != std::string::npos; };
        const auto at = [&](const std::string& needle) { return cpp.find(needle); };
        check(has("const nofoot::f180::OneSided os = nofoot::f180::oneSided(ci.animLean, g_f180.refLean, st.share);") && has("for (int k = 0; k < nofoot::f180::kLevels; ++k) ci.leftAsIs[k] = os.left[k];") &&
                  has("    if (os.any) {") && has("nofoot::f180::applyOneSided(pl.f180, os.cum, g_pfzAnim, count, g_f180Out, g_f180Touched, &touchedN)") && has("    ci.ran = true;\n    ci.weight = os.any ? st.share : 0.0;") &&
                  has("s180.leftAsIs[k] = c180.ran && c180.leftAsIs[k];") && has("if (!(st.share > 0.0) || !g_f180.held.have()) return true;"),
              "F182b: the stage returns at the share 0 or with no reference before the rule; the rule is one-sided (oneSided, applyOneSided), writes nothing when every bone is left as animated, counts the frame as made, and the bend yields only to a straighten that moved something");
        const size_t bodyDetach = at("const double bodyDetachForCall = pinWanted ?"), block = at("nofoot::f182::Out f182Out;"), sprint = at("const double offWBeforeSprint = offW;"),
                     stable = at("const bool leftStablePolicy = (offOut.ok && offOut.stablePolicy) || f182Out.held;");
        check(bodyDetach != std::string::npos && bodyDetach < block && block < stable && stable < sprint && has("static nofoot::f182::State s_f182;") && has("f182Out = s_f182.step(in);") &&
                  has("in.pitchDeg = in.armed ? nofoot::f182::aimPitchDeg(G.g) : std::nan(\"\");") && has("in.gun = in.armed && offOut.allowed && supportKindForCall == nofoot::kSupportNone && !authoredPistolSupport && !supportCup && !primaryDrawWrite") &&
                  has("in.two = offOut.ok && offOut.two;") && has("in.offW = offOut.w;") && has("in.detachW = bodyDetachForCall;") && has("std::memcpy(effectiveHand[0], f182Out.left, sizeof(effectiveHand[0]));") &&
                  has("offW = std::max(offW, f182Out.w);"),
              "F182c: the hold sits after the sights' detach weight is known and before F167's release: F69h's weight and the left target are replaced only by a held point (gun, two-handed, detach and pitch gates)");
        check(has("physicalPoleActive, leftStablePolicy, rifleStableAuthority, bodyDetachForCall,") && has("leftStablePolicy);") && !has("offOut.ok && offOut.stablePolicy);") &&
                  !has("physicalPoleActive, offOut.ok && offOut.stablePolicy,"),
              "F182c: a hand held on the item keeps the stable pole it had: both users of F69h's stable policy read the held one too");
        check(has("const bool offInputValid = armsOk && fpHandsGenerationOk && fpHands.valid[0] && fpHands.valid[1] && fpAge <= nofoot::kIkPoseDeviationMaxGapMs;") &&
                  has("offOut = s_off.step(dtOff, armedNow, offInputValid, fpm[0], fpm[1], !(relaxLeftArmForItem && !f83JournalPolicyHold), poseFrame,") && has("double offW = armedNow ? offOut.w : 1.0;"),
              "F182c: F69h's own decision (its input, the step, the weight) is unchanged: the hold only replaces what it frees");
        check(has("fw.f182.add(f182Out);") && has("if (side == 0 && f182Out.held) fw.f182.addError(o.gripErr);") && has("nofoot::f182::formatClause(f182Clause, sizeof(f182Clause), w.f182);") &&
                  has("G %llu (oldest %.0f ms)%s\",") && has("w.staleG), w.gAgeMax, f182Clause);") && has("nofoot::f182::Counters f182;") && has("#include \"native_on_foot_f182.h\""),
              "F182c: the F67f window counts the steep, freed and held calls and the held calls' error, and the line ends with the clause");
    }
}

}  // namespace f182test
