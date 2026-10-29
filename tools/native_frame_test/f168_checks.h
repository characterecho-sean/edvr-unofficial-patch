// F168 checks (native on-foot VR), run168 below. Header only, run by native_frame_test.
//
// F168 (native_on_foot_f168.h, TEMPORARY): the dial for the crouched body's distance to the gun. experimental.native_on_foot_crouch_ads_body_forward_cm (-20..+20 cm, 0 by default) moves the
// third-person body along the aim's horizontal forward, relative to the eye, while crouched in the sights: the pin's target for the neck takes the move, so the body and the shoulders the arms
// are solved from stand that far toward the gun (positive) or away from it. The move is the dial x the crouched weight (F163) x the detach weight, so it is exactly 0 standing, at the hip and
// with the dial at 0. The figures: the dial and the move applied, the right shoulder to the gun's root, the left shoulder to the left-hand grip and from the eye, crouched in the sights,
// still against moving, on a line within the log's 1200 bytes.
//
// F169 (section 5): a positive dial is added to the written neck after the reach rule, not to the target the rule works from (which gave it all back in the flight edvr_gfx_20261028_201339);
// the rig runs the real reach rule on a left arm out of reach of the foregrip. A negative dial stays in the target; the move as written is measured against the same call with the dial at 0.
#pragma once

#include "../../src/d3d11/native_on_foot_core.h"
#include "../../src/d3d11/native_on_foot_f168.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>

namespace f168test {

namespace nf = edvr::nofoot;
namespace f168 = edvr::nofoot::f168;

template <class Check>
inline void run168(Check&& check) {
    const auto eq = [](double a, double b, double tol) { return std::fabs(a - b) <= tol; };

    // ---- 1. the key's range and the move ---------------------------------------------------------------------------------------------------------------------------------------------
    {
        check(f168::clampDialCm(7.5) == 7.5 && f168::clampDialCm(-25.0) == -20.0 && f168::clampDialCm(25.0) == 20.0 && f168::clampDialCm(-20.0) == -20.0 && f168::clampDialCm(20.0) == 20.0 &&
                  f168::clampDialCm(std::nan("")) == 0.0 && f168::clampDialCm(std::numeric_limits<double>::infinity()) == 0.0 && f168::kDialMinCm == -20.0 && f168::kDialMaxCm == 20.0,
              "F168: the dial is held to -20..+20 cm; a value that is not a number reads 0");
        check(f168::shiftM(0.0, 1.0, 1.0) == 0.0 && f168::shiftM(10.0, 0.0, 1.0) == 0.0 && f168::shiftM(10.0, 1.0, 0.0) == 0.0 && f168::shiftM(10.0, 0.0, 0.0) == 0.0 &&
                  f168::shiftM(-10.0, 0.0, 1.0) == 0.0 && f168::shiftM(10.0, std::nan(""), 1.0) == 0.0 && f168::shiftM(10.0, 1.0, std::nan("")) == 0.0 && f168::shiftM(std::nan(""), 1.0, 1.0) == 0.0,
              "F168: the move is exactly 0 with the dial at 0, standing (the crouched weight 0), at the hip (the detach weight 0) and for a weight or a dial that is not a number");
        check(eq(f168::shiftM(10.0, 1.0, 1.0), 0.10, 1e-15) && eq(f168::shiftM(-20.0, 1.0, 1.0), -0.20, 1e-15) && eq(f168::shiftM(50.0, 1.0, 1.0), 0.20, 1e-15) &&
                  eq(f168::shiftM(10.0, 0.5, 0.5), 0.025, 1e-15) && eq(f168::shiftM(10.0, 0.5, 1.0), 0.05, 1e-15) && eq(f168::shiftM(10.0, 1.0, 0.25), 0.025, 1e-15) &&
                  eq(f168::shiftM(10.0, 2.0, 2.0), 0.10, 1e-15) && eq(f168::shiftM(10.0, -1.0, 1.0), 0.0, 0.0),
              "F168: the move is the dial in metres x the crouched weight x the detach weight, each held to 0..1: 10 cm crouched in the sights is 0.10 m, half and half is 2.5 cm, the dial is clamped first");
    }

    // ---- 2. the vector: along the aim's horizontal forward, never vertical --------------------------------------------------------------------------------------------------------------
    {
        double out[3] = {9.0, 9.0, 9.0};
        const double fwd[3] = {0.6, 0.3, 0.8};   // 0.6, 0.8 horizontal (length 1), looking 17 deg up
        const bool ok = f168::shiftVector(10.0, 1.0, 1.0, fwd, out);
        check(ok && eq(out[0], 0.06, 1e-15) && out[1] == 0.0 && eq(out[2], 0.08, 1e-15), "F168: 10 cm toward the gun crouched in the sights is 6 cm and 8 cm along the aim's horizontal forward (0.6, 0.8), none vertical, whatever the aim's pitch");
        const double fwd2[3] = {1.2, -0.9, 1.6};   // twice as long, looking down
        const bool ok2 = f168::shiftVector(10.0, 1.0, 1.0, fwd2, out);
        check(ok2 && eq(out[0], 0.06, 1e-15) && out[1] == 0.0 && eq(out[2], 0.08, 1e-15), "F168: the aim's length and pitch do not matter, only its horizontal direction");
        const bool back = f168::shiftVector(-10.0, 1.0, 1.0, fwd, out);
        check(back && eq(out[0], -0.06, 1e-15) && eq(out[2], -0.08, 1e-15), "F168: a negative dial moves the body away from the gun, the same line");
        out[0] = out[1] = out[2] = 9.0;
        const bool standing = f168::shiftVector(10.0, 0.0, 1.0, fwd, out);
        const bool standingZero = out[0] == 0.0 && out[1] == 0.0 && out[2] == 0.0;
        out[0] = out[1] = out[2] = 9.0;
        const bool hip = f168::shiftVector(10.0, 1.0, 0.0, fwd, out);
        const bool hipZero = out[0] == 0.0 && out[1] == 0.0 && out[2] == 0.0;
        out[0] = out[1] = out[2] = 9.0;
        const bool dialOff = f168::shiftVector(0.0, 1.0, 1.0, fwd, out);
        const bool dialZero = out[0] == 0.0 && out[1] == 0.0 && out[2] == 0.0;
        check(!standing && standingZero && !hip && hipZero && !dialOff && dialZero,
              "F168: standing, at the hip and with the dial at 0 the vector is not made and is exactly zero");
        const double up[3] = {0.0, 1.0, 0.0}, nearUp[3] = {0.05, 0.99, 0.05}, bad[3] = {std::nan(""), 0.0, 1.0};
        out[0] = out[1] = out[2] = 9.0;
        const bool a = f168::shiftVector(10.0, 1.0, 1.0, up, out);
        const bool aZero = out[0] == 0.0 && out[2] == 0.0;
        const bool b = f168::shiftVector(10.0, 1.0, 1.0, nearUp, out);
        const bool c = f168::shiftVector(10.0, 1.0, 1.0, bad, out);
        const bool d = f168::shiftVector(10.0, 1.0, 1.0, nullptr, out);
        check(!a && aZero && !b && !c && !d, "F168: an aim with no horizontal forward (straight up, under 0.1 horizontal, not a number, none) moves nothing");
        // the ramp: the move rises with the crouch and with the sights, and is 0 until both are
        double last = -1.0;
        bool rising = true;
        for (int i = 0; i <= 10; ++i) {
            const double w = i / 10.0;
            const double m = f168::shiftM(10.0, w, w);
            rising = rising && m >= last;
            last = m;
        }
        check(rising && eq(last, 0.10, 1e-15) && f168::shiftM(10.0, 0.0, 0.0) == 0.0, "F168: through a press (the crouched and detach weights rising together) the move rises without a step from 0 to the full 10 cm");
    }

    // ---- 3. the pin's target: the move is the body's move --------------------------------------------------------------------------------------------------------------------------------
    {
        // the pin moves the body so its neck reaches the target (nf::pinMove): a target shifted by the vector moves the body by that much more, nothing else
        const double neck[3] = {0.10, 0.90, 0.30}, target[3] = {0.12, 0.92, 0.35};
        const nf::PinMove base = nf::pinMove(neck, target);
        const double fwd[3] = {0.0, 0.0, 1.0};
        double shift[3];
        // F169: a negative dial is the only one that rides the target (a positive one is added to the written neck after the reach rule, section 5)
        const bool made = f168::shiftVector(-15.0, 1.0, 1.0, fwd, shift);
        double moved[3] = {target[0], target[1], target[2]};
        nf::weaponViewOffsetTarget(moved, shift);
        const nf::PinMove shifted = nf::pinMove(neck, moved);
        check(made && eq(shifted.th[0] - base.th[0], 0.0, 1e-12) && eq(shifted.th[2] - base.th[2], -0.15, 1e-12) && eq(shifted.delta - base.delta, 0.0, 1e-12) && !shifted.hClamped,
              "F168: the target shifted 15 cm away along the aim (a negative dial) moves the body 15 cm along it and no other way (no vertical move, no clamp)");
        // standing: the same call with the vector not made leaves the target and the move bit for bit
        double none[3];
        const bool madeNone = f168::shiftVector(15.0, 0.0, 1.0, fwd, none);
        double same[3] = {target[0], target[1], target[2]};
        nf::weaponViewOffsetTarget(same, none);
        const nf::PinMove unchanged = nf::pinMove(neck, same);
        check(!madeNone && std::memcmp(same, target, sizeof(same)) == 0 && unchanged.th[0] == base.th[0] && unchanged.th[1] == base.th[1] && unchanged.th[2] == base.th[2] &&
                  unchanged.delta == base.delta && unchanged.hClamped == base.hClamped && unchanged.vClamped == base.vClamped,
              "F168: standing the target and the pin's move are bit for bit what they were");
    }

    // ---- 4. the counters, the line ---------------------------------------------------------------------------------------------------------------------------------------------------------
    {
        f168::Counters c;
        const auto frame = [&](bool moving, double dial, double shift, double gun, double grip, double eyeR, double eyeU, double eyeF) {
            f168::Sample s;
            s.moving = moving;
            s.dialCm = dial;
            s.shiftM = shift;
            s.writtenM = std::isfinite(gun) ? 0.8 * shift : std::nan("");   // F169: written 80% of the ask where the frame was measured, not measured where nothing else was
            s.rightShoulderToGunM = gun;
            s.leftShoulderToGripM = grip;
            s.leftShoulderM[0] = eyeR; s.leftShoulderM[1] = eyeU; s.leftShoulderM[2] = eyeF;
            c.add(s);
        };
        for (int i = 0; i < 40; ++i) frame(false, 10.0, 0.10, 0.30, 0.45, -0.068, -0.150, -0.248);   // still
        for (int i = 0; i < 60; ++i) frame(true, 10.0, 0.09, 0.28, 0.38, -0.014, -0.134, -0.049);    // moving
        for (int i = 0; i < 10; ++i) frame(true, 10.0, 0.05, std::nan(""), std::nan(""), std::nan(""), 0.0, 0.0);   // moving, nothing measured
        check(c.ads == 110 && c.still == 40 && c.moving == 70 && c.shiftN[0] == 40 && c.shiftN[1] == 70 && c.gunN[0] == 40 && c.gunN[1] == 60 && c.gripN[1] == 60 && c.eyeN[0] == 40 && c.eyeN[1] == 60 &&
                  eq(c.dialSum, 1100.0, 1e-9) && c.dialAbsMax == 10.0 && eq(c.shiftSum[0] / c.shiftN[0], 0.10, 1e-12) && eq(c.gunSum[1] / c.gunN[1], 0.28, 1e-12) && eq(c.eyeSum[0][2] / c.eyeN[0], -0.248, 1e-12),
              "F168: the counters keep the frames still against moving, each figure's mean over the frames it was measured on, the dial's sum and largest");
        f168::Counters m;
        m.merge(c);
        m.merge(c);
        check(m.ads == 220 && m.moving == 140 && m.gunN[1] == 120 && eq(m.gunSum[1], 2.0 * c.gunSum[1], 1e-9) && eq(m.dialSum, 2200.0, 1e-9) && m.dialAbsMax == 10.0 && m.eyeN[0] == 80,
              "F168: two windows add up");
        char line[1400];
        const int len = f168::formatSummary(line, sizeof(line), 10, "$wpn_m_shotgun_plasma_doublebarrel_name;", c);
        f168::Counters big;
        big.ads = big.still = big.moving = 4000000000u;
        for (int i = 0; i < 2; ++i) {
            big.shiftN[i] = big.gunN[i] = big.gripN[i] = big.eyeN[i] = 4000000000u;
            big.shiftSum[i] = -1.0e12; big.gunSum[i] = 1.0e12; big.gripSum[i] = 1.0e12;
            for (int k = 0; k < 3; ++k) big.eyeSum[i][k] = -1.0e13;
        }
        big.dialSum = -1.0e12; big.dialAbsMax = 20.0;
        char worst[1400];
        const int worstLen = f168::formatSummary(worst, sizeof(worst), 4000000000u, "$wpn_m_shotgun_plasma_doublebarrel_name_with_a_long_tail;", big);
        check(len > 0 && len < 1100 && worstLen > 0 && worstLen < 1190 && std::strstr(line, "(F168, TEMPORARY) 10 s: item $wpn_m_shotgun_plasma_doublebarrel_name;") != nullptr &&
                  std::strstr(line, "the dial experimental.native_on_foot_crouch_ads_body_forward_cm mean +10.0 cm (largest 10.0)") != nullptr &&
                  std::strstr(line, "crouched in the sights 110 frames (still 40, moving 70)") != nullptr &&
                  std::strstr(line, "the body moved toward the gun (the dial x the crouched weight x the detach weight) +10.0 cm still, +8.4 moving") != nullptr,
              "F168: the window's line names the tag, the key and its value, the frames still against moving and the move applied, within the log's 1200 bytes even with every figure at its worst");
        check(c.writtenN[0] == 40 && c.writtenN[1] == 60 && eq(c.writtenSum[0] / c.writtenN[0], 0.08, 1e-12) && eq(c.writtenSum[1] / c.writtenN[1], 0.072, 1e-12) &&
                  std::strstr(line, "as written (the neck's forward change from the dial-0 body, after the reach rule; 40/60 frames) +8.0 cm still, +7.2 moving") != nullptr && m.writtenN[1] == 120,
              "F169: the move as written is a mean over the frames it was measured on, still against moving, beside the move asked; two windows add up");
        check(std::strstr(line, "right shoulder to the gun's root (the FP right handAttach 51 through G: the butt is not a joint the code reads) 40 frames still 30.0 cm, 60 moving 28.0") != nullptr &&
                  std::strstr(line, "left shoulder to the left-hand grip (the FP left handAttach 52 through G) 40 frames still 45.0 cm, 60 moving 38.0") != nullptr &&
                  std::strstr(line, "the left shoulder from the eye (right/up/forward, cm): still 40 frames -6.8/-15.0/-24.8, moving 60 frames -1.4/-13.4/-4.9") != nullptr,
              "F168: the right shoulder to the gun's root, the left shoulder to the grip and the left shoulder from the eye are printed in cm, still against moving, with the frames each is a mean of");
        // no frame at all: the line has nothing to divide by and still prints
        f168::Counters empty;
        char none[1400];
        const int noneLen = f168::formatSummary(none, sizeof(none), 1, nullptr, empty);
        check(noneLen > 0 && std::strstr(none, "item none") != nullptr && std::strstr(none, "crouched in the sights 0 frames") != nullptr && std::strstr(none, "nan") == nullptr && std::strstr(none, "inf") == nullptr,
              "F168: a window with no frame prints zeros, never a number that is not one");
    }

    // ---- 5. F169: a positive dial moves the written neck after the reach rule ---------------------------------------------------------------------------------------------------------
    // The flight edvr_gfx_20261028_201339: the dial went 0 to +17 cm and the body's move stayed +18.4 to +18.7 cm, because the reach rule works from the target the dial had shifted and gave
    // back the same amount. Here the real reach rule (nf::reachLimitDeltaFeasibleOnly) runs on a left arm that is out of reach of the foregrip, as the log's crouched shotgun was.
    {
        const double shoulder[2][3] = {{-0.20, 1.20, 0.00}, {0.20, 1.20, 0.00}};   // left, right (model x right, y up, z forward)
        const double grip[2][3] = {{0.05, 1.10, 0.80}, {0.12, 1.15, 0.52}};
        const double R[2] = {0.60, 0.58};
        const bool have[2] = {true, true};
        // the reach-limited move for a requested move d0: th = d0 + the rule's correction (the pin's own composition); the correction is returned in full
        const auto reached = [&](const double d0[3], double th[3], double full[3]) {
            double v[2][3];
            for (int s = 0; s < 2; ++s)
                for (int i = 0; i < 3; ++i) v[s][i] = shoulder[s][i] + d0[i] - grip[s][i];
            double cand[3] = {}, f[3] = {};
            bool feasible = true;
            const bool active = nf::reachLimitDeltaFeasibleOnly(v, R, have, cand, f, &feasible);
            for (int i = 0; i < 3; ++i) {
                th[i] = d0[i] + f[i];
                full[i] = f[i];
            }
            return active && feasible;
        };
        const auto leftToGrip = [&](const double th[3]) {
            const double d[3] = {shoulder[0][0] + th[0] - grip[0][0], shoulder[0][1] + th[1] - grip[0][1], shoulder[0][2] + th[2] - grip[0][2]};
            return nf::ikLen(d);
        };
        const double fwd[3] = {0.0, 0.0, 1.0};
        const double d0[3] = {0.0, 0.0, 0.0};
        double th0[3], full0[3];
        const bool active0 = reached(d0, th0, full0);
        check(active0 && th0[2] > 0.15 && leftToGrip(th0) > 0.5, "F169: the rig's left arm is out of reach of the foregrip at the body's own place, so the reach rule is active and pulls the body forward");
        // F168 as flown: the dial rides the target, the rule gives it back
        double shift[3];
        f168::shiftVector(10.0, 1.0, 1.0, fwd, shift);
        const double d0Old[3] = {d0[0] + shift[0], d0[1] + shift[1], d0[2] + shift[2]};
        double thOld[3], fullOld[3];
        reached(d0Old, thOld, fullOld);
        check(std::fabs(thOld[2] - th0[2]) < 0.01 && fullOld[2] < full0[2] - 0.08,
              "F169: with the dial in the target (F168 as flown) a +10 cm dial moved the body under 1 cm along the aim: the reach rule's correction fell by nearly the same 10 cm (the log: +0.3 cm for +17)");
        // F169: the split, then the move added after the rule
        double pre[3], post[3];
        f168::splitShift(10.0, shift, pre, post);
        check(f168::afterReach(10.0) && !f168::afterReach(-10.0) && !f168::afterReach(0.0) && pre[2] == 0.0 && eq(post[2], 0.10, 1e-15) && post[1] == 0.0, "F169: a positive dial is split to the move after the reach rule, none before it");
        double th[3] = {th0[0], th0[1], th0[2]}, reach[3] = {full0[0], full0[1], full0[2]};
        f168::addAfterReach(th, reach, post);
        check(eq(th[2] - th0[2], 0.10, 1e-12) && eq(th[0] - th0[0], 0.0, 1e-12) && th[1] == th0[1] && eq(reach[2] - full0[2], 0.10, 1e-12),
              "F169: with the dial after the reach rule a +10 cm dial moves the written neck 10.0 cm toward the gun, on top of the reach boundary, and the reach correction the bend aims at takes it");
        check(leftToGrip(th) < leftToGrip(th0) - 0.07 && leftToGrip(th) > 0.0, "F169: and the left shoulder to the foregrip is shorter by it (the arm is no longer at its full length)");
        // the move as written
        const double noDelta[3] = {};
        check(eq(f168::writtenAlongM(pre, post, noDelta, fwd), 0.10, 1e-12), "F169: the move as written of a positive dial is the move itself");
        // the dial at 0: nothing is split and nothing added, bit for bit
        double none[3], preZ[3], postZ[3];
        const bool made0 = f168::shiftVector(0.0, 1.0, 1.0, fwd, none);
        f168::splitShift(0.0, none, preZ, postZ);
        double thZ[3] = {th0[0], th0[1], th0[2]}, reachZ[3] = {full0[0], full0[1], full0[2]};
        f168::addAfterReach(thZ, reachZ, postZ);
        check(!made0 && std::memcmp(thZ, th0, sizeof(thZ)) == 0 && std::memcmp(reachZ, full0, sizeof(reachZ)) == 0 && f168::writtenAlongM(preZ, postZ, noDelta, fwd) == 0.0,
              "F169: with the dial at 0 the written move and the reach correction are bit for bit what they were, and the move as written is 0");
        // the same split standing and at the hip (the weights 0): no vector, so none of it
        double stand[3];
        const bool madeStand = f168::shiftVector(10.0, 0.0, 1.0, fwd, stand), madeHip = f168::shiftVector(10.0, 1.0, 0.0, fwd, stand);
        check(!madeStand && !madeHip && stand[0] == 0.0 && stand[2] == 0.0, "F169: standing and at the hip a positive dial makes no move to add");
        // the weights scale the added move
        double half[3], preH[3], postH[3];
        f168::shiftVector(10.0, 0.5, 1.0, fwd, half);
        f168::splitShift(10.0, half, preH, postH);
        check(eq(postH[2], 0.05, 1e-15), "F169: the move added after the rule is scaled by the crouched weight and the detach weight, as the dial always was");
        // a negative dial stays in the target: the rule gives back what the grip needs, and the figure says how much of it stayed
        double shiftN[3];
        f168::shiftVector(-10.0, 1.0, 1.0, fwd, shiftN);
        double preN[3], postN[3];
        f168::splitShift(-10.0, shiftN, preN, postN);
        const double d0N[3] = {d0[0] + preN[0], d0[1] + preN[1], d0[2] + preN[2]};
        double thN[3], fullN[3];
        reached(d0N, thN, fullN);
        const double deltaN[3] = {fullN[0] - full0[0], fullN[1] - full0[1], fullN[2] - full0[2]};
        const double writtenN = f168::writtenAlongM(preN, postN, deltaN, fwd);
        check(preN[2] == -0.10 && postN[2] == 0.0 && std::fabs(thN[2] - th0[2]) < 0.01 && eq(writtenN, thN[2] - th0[2], 1e-9),
              "F169: a negative dial stays in the target; with the left arm at the reach boundary the rule gives nearly all of it back, and the move as written is the body's real change (under 1 cm of -10)");
        // ... and with the left arm in reach with room to spare, the body does go back by the dial, and the figure says so
        const double nearGrip[2][3] = {{0.05, 1.10, 0.30}, {0.12, 1.15, 0.32}};
        const auto reachedNear = [&](const double dd[3], double thOut[3], double fullOut[3]) {
            double v[2][3];
            for (int s = 0; s < 2; ++s)
                for (int i = 0; i < 3; ++i) v[s][i] = shoulder[s][i] + dd[i] - nearGrip[s][i];
            double cand[3] = {}, f[3] = {};
            bool feasible = true;
            nf::reachLimitDeltaFeasibleOnly(v, R, have, cand, f, &feasible);
            for (int i = 0; i < 3; ++i) {
                thOut[i] = dd[i] + f[i];
                fullOut[i] = f[i];
            }
        };
        double thNear0[3], fullNear0[3], thNearN[3], fullNearN[3];
        reachedNear(d0, thNear0, fullNear0);
        reachedNear(d0N, thNearN, fullNearN);
        const double deltaNear[3] = {fullNearN[0] - fullNear0[0], fullNearN[1] - fullNear0[1], fullNearN[2] - fullNear0[2]};
        check(eq(thNearN[2] - thNear0[2], -0.10, 1e-9) && eq(f168::writtenAlongM(preN, postN, deltaNear, fwd), -0.10, 1e-9),
              "F169: with the left arm well inside its reach a -10 cm dial moves the body 10 cm back, and the move as written says -10.0");
        // no horizontal forward (aiming straight up): the move as written is not measured
        const double up[3] = {0.0, 1.0, 0.0};
        check(std::isnan(f168::writtenAlongM(preN, postN, deltaN, up)) && std::isnan(f168::writtenAlongM(preN, postN, deltaN, nullptr)), "F169: aiming straight up has no forward to write the move along");
    }
}

}  // namespace f168test
