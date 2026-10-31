// F182 checks (native on-foot VR), run182 below. Header only, run by native_frame_test.
//
// F182a (native_on_foot_f149.h): the plasma shotgun's ADS eye 9.5 cm forward, baked: its depth is 9.51 (0.01 + 9.5) at the trim 0, the other rows as they were. F182b (native_on_foot_f180.h): the
// spine straighten is one-sided per bone about the body's right axis: written lean = animated - share x max(0, animated - reference), the level's other components as animated, a bone at or
// behind the reference (looking up) as animated; the rig runs oneSided and applyOneSided on a toy skeleton with a roll and a forward offset in every level (so the bones have x and z parts), on the
// flight's numbers (all written = animated), on a forward-hunched case, on a mixed case (a turned bone above a bone left as animated), at share 0, then the counters, the line and the stage's
// source sites. F182c (native_on_foot_core.h OffHand::step, native_on_foot_f182.h): F69h keeps the left hand on the item through a stale first-person snapshot: a stale-only call keeps the decision and
// the history (a fresh unchanged pair stays on with no 11 frames to wait for), 3 s of hook time releases (counted), a fresh changed pair frees as before, nothing is kept for what was not on the item;
// while kept, the left grip target is the last grip point in the item's frame (it moves with the item); the counters and the F67f line's ending, and the source sites.
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

    // ---- F182c 1. F69h keeps the decision through a stale snapshot ----------------------------------------------------------------------------------------------------------------------------------
    {
        float a51[16], a52[16], a52far[16], a52turned[16];
        rigid(0.0, 0.0, 0.30, 1.30, 0.20, a51);
        rigid(0.0, 0.0, 0.30 + 0.311, 1.30, 0.20, a52);       // the rifle's pair: 31.1 cm apart, a steady relative orientation
        rigid(0.0, 0.0, 0.30 + 0.60, 1.30, 0.20, a52far);     // the left hand 60 cm away (a reload)
        rigid(0.0, 45.0, 0.30 + 0.311, 1.30, 0.20, a52turned);   // 311 mm apart, the orientation turned 45 deg against the right's
        struct Rig {
            nf::OffHand st;
            uint32_t frame = 1;
            const float* a51p = nullptr;
            nf::OffHand::Out step(bool ok, const float* l, bool stale, double dt = 0.011, uint32_t item = 7, bool allow = true) {
                return st.step(dt, true, ok, a51p, l, allow, frame++, item, 3, 5ull, 5, 0, false, stale);
            }
        };
        Rig r;
        r.a51p = a51;
        // acquired after 11 fresh frames, as always
        int acquiredAt = 0;
        nf::OffHand::Out o;
        for (int i = 1; i <= 60; ++i) {
            o = r.step(true, a52, false);
            if (o.two && !acquiredAt) acquiredAt = i;
        }
        check(acquiredAt == 11 && o.two && o.w == 1.0 && !o.kept && o.ok && o.historySamples == 11, "F182c: fresh frames acquire the pair after 11 steady frames and the weight rises to 1, as before (nothing kept, nothing stale)");

        // a stale call: invalid ONLY for the snapshot's age: the decision, the weight and the history stand
        const nf::OffHand::Out s1 = r.step(false, a52, true);
        check(s1.two && s1.kept && !s1.ok && s1.w == 1.0 && s1.stablePolicy && s1.historySamples == 11 && !s1.staleLimit,
              "F182c: a call invalid only because the FP snapshot is stale keeps the decision (two-handed, weight 1, the stable policy) and the history (11 samples); it is not read (ok false) and counted as kept");
        // a fresh unchanged pair right after: stays on at once, with no 11 frames to wait for
        const nf::OffHand::Out f1 = r.step(true, a52, false);
        check(f1.two && f1.ok && !f1.kept && f1.w == 1.0 && f1.historySamples == 11 && f1.steady, "F182c: a fresh unchanged pair after the stale call stays two-handed at weight 1 at once, the history intact (no 11 frames to wait for)");

        // the old behaviour, for the record: the same call as unreadable for another reason (staleOnly false) releases the hand and needs 11 fresh frames to take it back
        Rig old;
        old.a51p = a51;
        for (int i = 0; i < 60; ++i) old.step(true, a52, false);
        const nf::OffHand::Out u1 = old.step(false, a52, false);
        int backAt = 0;
        for (int i = 1; i <= 30 && !backAt; ++i)
            if (old.step(true, a52, false).two) backAt = i;
        check(!u1.two && !u1.kept && u1.historySamples == 0 && backAt == 11,
              "F182c: an input unreadable for any other reason (not the snapshot's age alone) releases the hand and clears the history as before; taking it back needs 11 fresh frames");

        // the 3 s limit: stale calls of 0.1 s keep it for 3 s of hook time, counted once; then the hand is released; a 5 s freeze counts 0.1 s only
        Rig lim;
        lim.a51p = a51;
        for (int i = 0; i < 60; ++i) lim.step(true, a52, false);
        const nf::OffHand::Out frz = lim.step(false, a52, true, 5.0);
        int limitAt = 0, limits = 0, keptCalls = frz.kept ? 1 : 0;
        nf::OffHand::Out l;
        for (int i = 1; i <= 50; ++i) {
            l = lim.step(false, a52, true, 0.1);
            if (l.staleLimit) { ++limits; if (!limitAt) limitAt = i; }
            if (l.kept) ++keptCalls;
        }
        std::snprintf(msg, sizeof(msg), "F182c: stale calls keep the decision for 3 s of hook time (a call counts at most 0.1 s: a 5 s freeze counted 0.1), %d kept calls, the release counted once at call %d, then the hand is free (two %d)",
                      keptCalls, limitAt, l.two ? 1 : 0);
        check(frz.kept && limits == 1 && limitAt >= 29 && limitAt <= 31 && keptCalls >= 29 && keptCalls <= 31 && !l.two && !l.kept, msg);
        // after the release a fresh call with the pair still there takes it back only through the 11 steady frames
        int relBack = 0;
        for (int i = 1; i <= 30 && !relBack; ++i)
            if (lim.step(true, a52, false).two) relBack = i;
        check(relBack == 11, "F182c: after the 3 s release the normal rule takes the hand back: 11 fresh steady frames");

        // a fresh changed pair frees as it always did: the left hand leaving the item (60 cm), once fresh frames show it; and an orientation change alone keeps the authority, as before
        Rig chg;
        chg.a51p = a51;
        for (int i = 0; i < 60; ++i) chg.step(true, a52, false);
        chg.step(false, a52, true);
        const nf::OffHand::Out gone = chg.step(true, a52far, false);
        Rig turn;
        turn.a51p = a51;
        for (int i = 0; i < 60; ++i) turn.step(true, a52, false);
        turn.step(false, a52, true);
        const nf::OffHand::Out turned = turn.step(true, a52turned, false);
        check(!gone.two && !gone.kept && gone.ok && turned.two && turned.ok,
              "F182c: after a stale call a fresh pair 60 cm apart frees the hand (a reload), and a fresh pair turned 45 deg stays two-handed (the rule's authority through orientation motion, unchanged)");

        // nothing is kept for a hand that was not on the item: never acquired (a pistol's 55 cm pair), another item or rig, the one-hand policy, disarmed
        float a52pistol[16];
        rigid(0.0, 0.0, 0.30 + 0.55, 1.30, 0.20, a52pistol);
        Rig pis;
        pis.a51p = a51;
        for (int i = 0; i < 40; ++i) pis.step(true, a52pistol, false);
        const nf::OffHand::Out p1 = pis.step(false, a52pistol, true);
        Rig oth;
        oth.a51p = a51;
        for (int i = 0; i < 60; ++i) oth.step(true, a52, false);
        const nf::OffHand::Out i1 = oth.step(false, a52, true, 0.011, 8);   // another item: the identity change resets
        Rig pol;
        pol.a51p = a51;
        for (int i = 0; i < 60; ++i) pol.step(true, a52, false);
        const nf::OffHand::Out w1 = pol.step(false, a52, true, 0.011, 7, false);   // the explicit one-hand policy
        check(!p1.two && !p1.kept && !i1.two && !i1.kept && !w1.two && !w1.kept, "F182c: nothing is kept for a pistol's pair, another item, or the one-hand policy: those release or never acquire as before");
        // the constant
        check(nf::kOffHandStaleKeepSec == 3.0, "F182c: the stale keep is 3 s of hook time");
    }

    // ---- F182c 2. the held grip point in the item's frame --------------------------------------------------------------------------------------------------------------------------------------------
    {
        float L0[16], R0[16], rel[16], back[16], zeroM[16] = {};
        rigid(-15.0, 5.0, 0.12, 1.28, 0.45, L0);
        rigid(20.0, -10.0, 0.30, 1.30, 0.20, R0);
        check(f182::relativeOf(L0, R0, rel) && (f182::leftFrom(rel, R0, back), maxAbsDiff(back, L0) < 1e-5) && !f182::relativeOf(L0, zeroM, rel),
              "F182c: the left target kept in the right target's frame comes back as it was (left = relative x right); a singular right target makes no relative");

        float Lstale[16];
        rigid(80.0, 30.0, 0.9, 1.0, -0.5, Lstale);   // where a stalled frame's left target ends up: nowhere near the gun
        const auto mk = [&](const float* left, const float* right) {
            f182::In in;
            in.armed = in.gun = in.liveOk = true;
            in.fresh = true;
            in.two = true;
            in.offW = 1.0;
            in.nowMs = 1000;
            in.item = 7; in.planGen = 3; in.fpGen = 5; in.epoch = 0;
            in.modeLeft = 1; in.modeRight = 1;
            in.left = left; in.right = right;
            return in;
        };
        const auto kept = [&](f182::In in) {   // F69h kept the decision through a stale input
            in.fresh = false;
            in.kept = true;
            in.staleAgeMs = 2200.0;
            return in;
        };

        // fresh two-handed calls store the point; nothing is held on a fresh call
        f182::State st;
        f182::Out o;
        bool storedAll = true, notHeld = true;
        for (int i = 0; i < 20; ++i) {
            f182::In in = mk(L0, R0);
            in.nowMs = 1000 + 11 * i;
            o = st.step(in);
            storedAll = storedAll && o.stored;
            notHeld = notHeld && !o.held && !o.kept && !o.limit;
        }
        check(storedAll && notHeld && st.have, "F182c: while F69h reads the pair and holds it at weight 1 the point is stored on every call and nothing is held");

        // a kept call: the left target is the stored point, not the stale snapshot's left target; the point is 40 ms old; it is not overwritten by the stale call
        f182::In k = kept(mk(Lstale, R0));
        k.nowMs = 1000 + 11 * 19 + 40;
        o = st.step(k);
        check(o.kept && o.held && !o.stored && maxAbsDiff(o.left, L0) < 1e-5 && maxAbsDiff(o.left, Lstale) > 0.5 && eq(o.pointAgeMs, 40.0, 1e-9) && eq(o.staleAgeMs, 2200.0, 1e-9),
              "F182c: on a call F69h kept through a stale snapshot the left target is the stored grip point (the stale snapshot's left target, far from the gun, does not drag the hand), 40 ms old, the snapshot 2200 ms old");

        // the item moves while the snapshot is stale (the right target by a rigid move): the held left follows it, in the item's frame
        float T[16], R1[16], expectL[16];
        rigid(30.0, 8.0, 0.20, -0.10, 0.40, T);
        nf::pfzMul(R0, T, R1);
        nf::pfzMul(L0, T, expectL);
        f182::In moved = kept(mk(Lstale, R1));
        o = st.step(moved);
        std::snprintf(msg, sizeof(msg), "F182c: the item moves (the right target by a rigid move): the held left target moves with it, in the item's frame (max element difference %.2e from the left target moved the same way)", maxAbsDiff(o.left, expectL));
        check(o.held && maxAbsDiff(o.left, expectL) < 1e-4 && maxAbsDiff(o.left, L0) > 0.05, msg);

        // the point is the last fresh full call's: a later fresh pair with another relative (the hand re-gripped) replaces it
        float L2[16];
        rigid(-15.0, 5.0, 0.12, 1.28, 0.55, L2);
        f182::In regrip = mk(L2, R0);
        regrip.nowMs = 5000;
        st.step(regrip);
        o = st.step(kept(mk(Lstale, R0)));
        check(o.held && maxAbsDiff(o.left, L2) < 1e-5 && maxAbsDiff(o.left, L0) > 0.05, "F182c: the stored point is the last fresh two-handed call's");

        // no gate on the pitch or the detach weight: the type has neither input; hip or sights, the hold is the same (a kept call is all it needs)
        check(!o.limit && o.staleAgeMs == 2200.0, "F182c: no pitch or detach gate (the hold needs only the kept decision): the same call at the hip and in the sights holds");

        // the limit the decision's release reports is echoed for the count
        f182::In lim = kept(mk(Lstale, R0));
        lim.kept = false;
        lim.staleLimit = true;
        o = st.step(lim);
        check(o.limit && !o.held && !o.kept, "F182c: the call F69h released for staleness after 3 s is counted and not held");

        // not kept: no point; never stored; another item or rig; a right target made another way; no right target; a pistol, tool or unarmed (the point is dropped)
        f182::State fresh1;
        o = fresh1.step(kept(mk(Lstale, R0)));
        check(o.kept && !o.held, "F182c: a kept decision with no stored point (F69h acquired and was stale at once) is kept but not held: the snapshot's own target stands");
        bool dropped = true;
        {
            f182::State s;
            for (int i = 0; i < 5; ++i) s.step(mk(L0, R0));
            f182::In other = kept(mk(Lstale, R0));
            other.item = 8;
            dropped = dropped && !s.step(other).held;
        }
        {
            f182::State s;
            for (int i = 0; i < 5; ++i) s.step(mk(L0, R0));
            f182::In other = kept(mk(Lstale, R0));
            other.fpGen = 6;
            dropped = dropped && !s.step(other).held;
        }
        {
            f182::State s;
            for (int i = 0; i < 5; ++i) s.step(mk(L0, R0));
            f182::In other = kept(mk(Lstale, R0));
            other.modeRight = 3;
            const bool a = !s.step(other).held;
            f182::In none = kept(mk(Lstale, R0));
            none.liveOk = false;
            dropped = dropped && a && !s.step(none).held;
        }
        {
            f182::State s;
            for (int i = 0; i < 5; ++i) s.step(mk(L0, R0));
            f182::In pistol = kept(mk(Lstale, R0));
            pistol.gun = false;
            const bool a = !s.step(pistol).held;
            dropped = dropped && a && !s.step(kept(mk(Lstale, R0))).held;   // the point went with the gun
            f182::State u;
            for (int i = 0; i < 5; ++i) u.step(mk(L0, R0));
            f182::In unarmed = kept(mk(Lstale, R0));
            unarmed.armed = false;
            dropped = dropped && !u.step(unarmed).held && !u.step(kept(mk(Lstale, R0))).held;
        }
        check(dropped, "F182c: another item or rig, a right target made another way, no right target, a pistol or tool and unarmed are never held (and drop the point)");
    }

    // ---- F182c 3. the counters and the F67f line's ending -------------------------------------------------------------------------------------------------------------------------------------
    {
        f182::Counters c;
        char clause[600];
        check(f182::formatClause(clause, sizeof(clause), c, 0.0, 0) == 0 && clause[0] == '\0', "F182c: a window with no kept call and no release has no ending on the F67f line");
        f182::Out keptOnly, held;
        keptOnly.kept = true;
        keptOnly.staleAgeMs = 100.0;
        held.kept = held.held = true;
        for (int i = 0; i < 10; ++i) { keptOnly.staleAgeMs = 100.0 + 10.0 * i; c.add(keptOnly); }
        for (int i = 0; i < 30; ++i) { held.staleAgeMs = 2000.0 + 20.0 * i; c.add(held); c.addError(0.30 + 0.02 * i); }
        f182::Out lim;
        lim.limit = true;
        c.add(lim);
        const int n = f182::formatClause(clause, sizeof(clause), c, 3.9, 500);
        std::snprintf(msg, sizeof(msg), "F182c: the F67f line's ending names the calls kept through a stale input (40) and the snapshot's age, the releases after 3 s, the calls on the held point, their error against the unheld calls' (%d bytes)", n);
        check(n > 0 && n < 420 && std::strstr(clause, "; stale grip hold (F182c): kept through a stale FP input 40 calls (stale age mean 1754 max 2580 ms), released for staleness after 3 s 1; the left hand on the held grip point in 30 calls, "
                                                    "their hand-to-grip error mean 0.59 max 0.88 cm against 3.90 cm on the 500 unheld calls") != nullptr,
              msg);
        check(c.kept == 40 && c.limit == 1 && c.held == 30 && c.errN == 30, "F182c: the counters keep the kept calls, the releases, the held calls and the error samples");
        f182::Counters limitOnly;
        limitOnly.add(lim);
        check(f182::formatClause(clause, sizeof(clause), limitOnly, 0.0, 0) > 0 && std::strstr(clause, "kept through a stale FP input 0 calls") != nullptr, "F182c: a window with only a release still prints the ending");
        f182::Counters bad;
        bad.addError(std::nan(""));
        check(bad.errN == 0, "F182c: an error that is not a number is not counted");
    }

    // ---- sources: the stage and the hook -------------------------------------------------------------------------------------------------------------------------------------------------------
    {
        const std::string cpp = slurp("src/d3d11/native_on_foot.cpp"), core = slurp("src/d3d11/native_on_foot_core.h");
        const auto has = [&](const std::string& needle) { return cpp.find(needle) != std::string::npos; };
        const auto at = [&](const std::string& needle) { return cpp.find(needle); };
        check(has("const nofoot::f180::OneSided os = nofoot::f180::oneSided(ci.animLean, g_f180.refLean, st.share);") && has("for (int k = 0; k < nofoot::f180::kLevels; ++k) ci.leftAsIs[k] = os.left[k];") &&
                  has("    if (os.any) {") && has("nofoot::f180::applyOneSided(pl.f180, os.cum, g_pfzAnim, count, g_f180Out, g_f180Touched, &touchedN)") && has("    ci.ran = true;\n    ci.weight = os.any ? st.share : 0.0;") &&
                  has("s180.leftAsIs[k] = c180.ran && c180.leftAsIs[k];") && has("if (!(st.share > 0.0) || !g_f180.held.have()) return true;"),
              "F182b: the stage returns at the share 0 or with no reference before the rule; the rule is one-sided (oneSided, applyOneSided), writes nothing when every bone is left as animated, counts the frame as made, and the bend yields only to a straighten that moved something");
        check(has("const bool offStaleOnly = armsOk && fpHandsGenerationOk && fpHands.valid[0] && fpHands.valid[1] && fpAge > nofoot::kIkPoseDeviationMaxGapMs;") &&
                  has("const bool offInputValid = armsOk && fpHandsGenerationOk && fpHands.valid[0] && fpHands.valid[1] && fpAge <= nofoot::kIkPoseDeviationMaxGapMs;") &&
                  has("f83NativeEpochIdentity ? f83SelectionForCall.epoch : 0, f83SelectionSuspended, offStaleOnly);") && has("double offW = armedNow ? offOut.w : 1.0;"),
              "F182c: F69h is given the snapshot's staleness apart from its other reasons for an unreadable input (offStaleOnly: the pair, the generation and the rig readable, the age over the gap)");
        check(has("const bool offReadable = offOut.ok || offOut.kept;") && has("exactRifleForCall && offReadable && offOut.stablePolicy && !primaryDrawWrite ?") && has("const bool leftStablePolicy = offReadable && offOut.stablePolicy;") &&
                  has("sample.leftStablePolicy = offReadable && offOut.stablePolicy;") && has("leftStablePolicy);") && !has("offOut.ok && offOut.stablePolicy);") && !has("physicalPoleActive, offOut.ok && offOut.stablePolicy,"),
              "F182c: a decision kept through a stale input keeps the stable pole and the rifle's quality it had (the pole's eligibility, the pin's stable policy, the rifle quality and the sample read offReadable)");
        const size_t offStep = at("offOut = s_off.step(dtOff"), block = at("nofoot::f182::Out f182Out;"), sprint = at("const double offWBeforeSprint = offW;"), stable = at("const bool leftStablePolicy = offReadable");
        check(offStep != std::string::npos && offStep < block && block < stable && stable < sprint && has("static nofoot::f182::State s_f182;") && has("f182Out = s_f182.step(in);") &&
                  has("in.gun = in.armed && offOut.allowed && supportKindForCall == nofoot::kSupportNone && !authoredPistolSupport && !supportCup && !primaryDrawWrite") && has("in.fresh = offOut.ok;") &&
                  has("in.two = offOut.two;") && has("in.kept = offOut.kept;") && has("in.staleLimit = offOut.staleLimit;") && has("in.staleAgeMs = static_cast<double>(fpAge);") &&
                  has("std::memcpy(effectiveHand[0], f182Out.left, sizeof(effectiveHand[0]));") && !has("nofoot::f182::aimPitchDeg") && !has("steepSights") && !has("offW = std::max(offW, f182Out.w);"),
              "F182c: the hold sits after F69h's step and before F167's release, takes F69h's decision (kept, fresh, two-handed), replaces only the left target on a kept call, and has no pitch or detach gate");
        check(has("fw.f182.add(f182Out);") && has("if (side == 0 && f182Out.held) fw.f182.addError(o.gripErr);") && has("const uint64_t f182UnheldN = w.errN[0] > w.f182.errN ? w.errN[0] - w.f182.errN : 0;") &&
                  has("nofoot::f182::formatClause(f182Clause, sizeof(f182Clause), w.f182, f182UnheldN ?") && has("G %llu (oldest %.0f ms)%s\",") && has("w.staleG), w.gAgeMax, f182Clause);") &&
                  has("nofoot::f182::Counters f182;") && has("#include \"native_on_foot_f182.h\""),
              "F182c: the F67f window counts the kept calls, the releases, the held calls and their error against the unheld calls', and the line ends with the clause");
        check(core.find("constexpr double kOffHandStaleKeepSec = 3.0;") != std::string::npos && core.find("bool sourceSuspended = false, bool staleOnly = false) noexcept {") != std::string::npos &&
                  core.find("bool keepNow = staleOnly && !ok && attached && allowTwoHand;") != std::string::npos && core.find("if (!allowTwoHand || (!ok && !keepNow)) clearHistory();") != std::string::npos &&
                  core.find("        if (keepNow) {\n            o.kept = true;") != std::string::npos && core.find("} else if (!o.ok || !allowTwoHand || !closeEnough) {") != std::string::npos,
              "F182c: OffHand::step keeps an attached decision and its history through a stale-only input (up to 3 s), and releases for every other unreadable input, a pair past 40 cm or the one-hand policy as before");
    }
}

}  // namespace f182test
