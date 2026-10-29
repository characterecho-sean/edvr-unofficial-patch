// F170 checks (native on-foot VR), run170 below. Header only, run by native_frame_test.
//
// F170 (native_on_foot_f170.h, TEMPORARY): the crouched ADS torso squared to the aim. experimental.native_on_foot_crouch_ads_square (0..1, shipped 1 in F170 and 0 since F173; the checks set the dial they mean) is the share of the shoulder line's
// measured blade (the square-up's phi: the yaw that squares the upper-arm line to the aim) the torso is turned out of about the vertical through the neck, while crouched (F163's weight) in the
// sights (the detach weight): the dial x both weights. It is the square-up's machinery: the reach rule turns the shoulders it keeps within reach of the grips by it, and the written square-up
// turns the spine levels by it, after the upper-body hold's yield (F166, F168) so it holds while walking crouched in the sights. At 0, standing and at the hip the turn is exactly 0.
//
// The flight edvr_gfx_20261028_215408: the crouched shotgun's shoulder line was 28.7 deg off square (largest 28.8) in 90 of 90 frames with the left shoulder forward, and the stock lay in the
// left shoulder.
#pragma once

#include "../../src/d3d11/native_on_foot_core.h"
#include "../../src/d3d11/native_on_foot_f166.h"
#include "../../src/d3d11/native_on_foot_f170.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>

namespace f170test {

namespace nf = edvr::nofoot;
namespace f170 = edvr::nofoot::f170;
namespace f166 = edvr::nofoot::f166;

template <class Check>
inline void run170(Check&& check) {
    const auto eq = [](double a, double b, double tol) { return std::fabs(a - b) <= tol; };
    const double kDeg = nf::kIkPi / 180.0;

    // ---- 1. the key's range, the share, the turn -------------------------------------------------------------------------------------------------------------------------------------------
    {
        check(f170::clampDial(0.5) == 0.5 && f170::clampDial(-0.3) == 0.0 && f170::clampDial(1.7) == 1.0 && f170::clampDial(0.0) == 0.0 && f170::clampDial(1.0) == 1.0 &&
                  f170::clampDial(std::nan("")) == 0.0 && f170::clampDial(std::numeric_limits<double>::infinity()) == 0.0 && f170::kDialMin == 0.0 && f170::kDialMax == 1.0,
              "F170: the dial is held to 0..1; a value that is not a number reads 0 (no turn)");
        check(f170::share(0.0, 1.0, 1.0) == 0.0 && f170::share(1.0, 0.0, 1.0) == 0.0 && f170::share(1.0, 1.0, 0.0) == 0.0 && f170::share(1.0, 0.0, 0.0) == 0.0 && f170::share(1.0, std::nan(""), 1.0) == 0.0 &&
                  f170::share(1.0, 1.0, std::nan("")) == 0.0 && f170::share(std::nan(""), 1.0, 1.0) == 0.0 && f170::share(1.0, -0.4, 1.0) == 0.0,
              "F170: the share is exactly 0 with the dial at 0, standing (the crouched weight 0), at the hip (the detach weight 0) and for a weight or a dial that is not a number");
        check(eq(f170::share(1.0, 1.0, 1.0), 1.0, 0.0) && eq(f170::share(0.5, 1.0, 1.0), 0.5, 1e-15) && eq(f170::share(1.0, 0.5, 0.5), 0.25, 1e-15) && eq(f170::share(0.5, 0.5, 1.0), 0.25, 1e-15) &&
                  eq(f170::share(3.0, 2.0, 2.0), 1.0, 0.0),
              "F170: the share is the dial x the crouched weight x the detach weight, each held to 0..1: half and half at a full dial is a quarter, the dial is clamped first");
        const double phi = -28.7 * kDeg;   // the flight's mean measured shoulder phi
        check(f170::turnRad(0.0, true, phi) == 0.0 && f170::turnRad(1.0, false, phi) == 0.0 && f170::turnRad(1.0, true, std::nan("")) == 0.0 && f170::turnRad(1.0, true, 0.0) == 0.0 &&
                  f170::turnRad(-0.5, true, phi) == 0.0,
              "F170: the turn is exactly 0 for a share of 0, a phi that was not measured and one that is not a number");
        check(eq(f170::turnRad(1.0, true, phi), phi, 1e-15) && eq(f170::turnRad(0.5, true, phi), 0.5 * phi, 1e-15) && eq(f170::turnRad(0.25, true, phi), 0.25 * phi, 1e-15) &&
                  f170::turnRad(1.0, true, phi) < 0.0 && f170::turnRad(1.0, true, -phi) > 0.0,
              "F170: the turn is the share of the measured phi, with its sign: the dial at 1 takes out all of 28.7 deg, at 0.5 half");
        check(eq(f170::turnRad(1.0, true, 80.0 * kDeg), 50.0 * kDeg, 1e-12) && eq(f170::turnRad(1.0, true, -80.0 * kDeg), -50.0 * kDeg, 1e-12) && eq(f170::turnRad(0.5, true, 80.0 * kDeg), 25.0 * kDeg, 1e-12),
              "F170: the phi is held to the square-up's old limit (50 deg) before the share is taken");
    }

    // ---- 2. the geometry on the real functions: a shoulder line 29 deg off square ---------------------------------------------------------------------------------------------------------
    // The model's axes: x right, y up, z forward. The aim looks along +z. The shoulders stand 0.20 m either side of the line through the neck; the pivot is the neck.
    const double fwd[3] = {0.0, 0.0, 1.0};
    const double pivot[3] = {0.0, 1.40, 0.05};
    const double squareShoulder[2][3] = {{-0.20, 1.25, 0.05}, {0.20, 1.25, 0.05}};   // left, right, square to the aim
    // blade the line by `deg` about the vertical through the pivot, the sign chosen so the LEFT shoulder is the forward one (the flight's)
    double bladed[2][3];
    double phi0 = 0.0;
    {
        double trial[2][2][3];
        double phiOf[2];
        const double angles[2] = {29.0 * kDeg, -29.0 * kDeg};
        for (int k = 0; k < 2; ++k) {
            for (int s = 0; s < 2; ++s) nf::turnAboutVertical(squareShoulder[s], pivot, angles[k], trial[k][s]);
            nf::ikSquareToForward(trial[k][0], trial[k][1], fwd, &phiOf[k]);
        }
        const int pick = trial[0][0][2] > trial[0][1][2] ? 0 : 1;
        for (int s = 0; s < 2; ++s)
            for (int i = 0; i < 3; ++i) bladed[s][i] = trial[pick][s][i];
        phi0 = phiOf[pick];
        check(bladed[0][2] > bladed[1][2] + 0.15 && eq(std::fabs(phi0), 29.0 * kDeg, 1e-9), "F170: the rig's shoulder line is 29 deg off square with the left shoulder forward of the right by ~19 cm (the flight's left shoulder was the forward one)");
    }
    // the turn the pin writes at a share, applied to the shoulders as the reach rule applies it (turnAboutVertical at the full share: the arm joints sit under the chest), and the phi left
    const auto turnedAt = [&](double dial, double posture, double detach, double u[2][3]) {
        const double turn = f170::turnRad(f170::share(dial, posture, detach), true, phi0);
        for (int s = 0; s < 2; ++s) nf::turnAboutVertical(bladed[s], pivot, turn, u[s]);
        double phi = 0.0;
        nf::ikSquareToForward(u[0], u[1], fwd, &phi);
        return phi;
    };
    {
        double u[2][3];
        const double full = turnedAt(1.0, 1.0, 1.0, u), half = turnedAt(0.5, 1.0, 1.0, u), none = turnedAt(0.0, 1.0, 1.0, u);
        check(std::fabs(full) < 1e-9, "F170: a shoulder line 29 deg off square goes to 0 at the dial's 1 (crouched, in the sights)");
        check(eq(std::fabs(half), 14.5 * kDeg, 1e-9), "F170: and to 14.5 deg at the dial's 0.5");
        check(eq(none, phi0, 1e-12), "F170: at the dial's 0 the line stays where it was (the turn is exactly 0, section 1)");
        const double q = turnedAt(1.0, 0.5, 0.5, u);
        check(eq(std::fabs(q), 0.75 * 29.0 * kDeg, 1e-9), "F170: half crouched and half in the sights at a full dial takes out a quarter of it");
        {
            const double stand = turnedAt(1.0, 0.0, 1.0, u);
            const double hip = turnedAt(1.0, 1.0, 0.0, u);
            check(eq(stand, phi0, 1e-12) && eq(hip, phi0, 1e-12), "F170: standing and at the hip a full dial leaves the line where it was (the turn is exactly 0, section 1)");
        }
        // the neck stays put: the pivot is the neck, so a turn about the vertical through it moves the shoulders and not the neck
        const double turn = f170::turnRad(1.0, true, phi0);
        double neckAfter[3];
        nf::turnAboutVertical(pivot, pivot, turn, neckAfter);
        check(neckAfter[0] == pivot[0] && neckAfter[1] == pivot[1] && neckAfter[2] == pivot[2], "F170: the pivot is the neck: it does not move, and the eye and the gun are not touched");
        // the vertical: no shoulder changes height
        double ux[2][3];
        turnedAt(1.0, 1.0, 1.0, ux);
        check(eq(ux[0][1], bladed[0][1], 1e-12) && eq(ux[1][1], bladed[1][1], 1e-12), "F170: the turn is about the vertical: no shoulder changes height");
        // the shoulders' fore-aft after the square: the left one moved back, the right one forward, toward the gun
        check(ux[0][2] < bladed[0][2] - 0.04 && ux[1][2] > bladed[1][2] + 0.04, "F170: squaring the line takes the left shoulder back out of the sight line and the right shoulder forward, toward the gun");
        // the reach rule's turn and the written one are the same: turnAboutVertical, and ikRotAxis through ikPivot at the chest's full weight, land the shoulder in the same place
        for (int s = 0; s < 2; ++s) {
            float m[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, static_cast<float>(bladed[s][0]), static_cast<float>(bladed[s][1]), static_cast<float>(bladed[s][2]), 1};
            float out[16];
            const double up[3] = {0.0, 1.0, 0.0};
            double R[9];
            nf::ikRotAxis(up, turn * nf::kIkSpineWeight[2], R);
            nf::ikPivot(m, R, pivot, out);
            const double d[3] = {out[12] - ux[s][0], out[13] - ux[s][1], out[14] - ux[s][2]};
            check(nf::ikLen(d) < 1e-5, "F170: the reach rule's shoulders and the spine levels' written turn put the shoulder in the same place (to float precision)");
        }
        // the spine levels take it in thirds: the lowest level a third, the chest all of it
        check(eq(f170::turnRad(1.0, true, phi0) * nf::kIkSpineWeight[0], phi0 / 3.0, 1e-12) && nf::kIkSpineWeight[2] == 1.0, "F170: the spine root takes a third of the turn, the middle two thirds and the chest the whole");
    }

    // ---- 3. the hold: the turn is not taken off by the upper body's yield -----------------------------------------------------------------------------------------------------------------
    {
        const double turn = f170::turnRad(1.0, true, -28.7 * kDeg);
        // armed: the square-up's own angle is 0 (the authored stance), so what the yield takes off is nothing, and the turn is the whole angle at every hold weight
        for (double hold : {0.0, 0.3, 0.5, 1.0}) {
            const double yield = f166::yieldFactor(hold);
            const double written = f170::writtenYaw(0.0, yield, turn);
            check(written == turn, "F170: the turn is written whole at hold weight 0, 0.3, 0.5 and 1: the hold does not take it off");
        }
        // what F168's yield would have done to it, had the turn been part of the angle: nothing left at hold weight 1
        check((0.0 + turn) * f166::yieldFactor(1.0) == 0.0 && f166::yieldFactor(1.0) == 0.0, "F170: (a turn inside the yield would have vanished at hold weight 1: that is why it is added after it)");
        // an unarmed base angle (no turn then) still goes through the yield
        check(eq(f170::writtenYaw(0.4, f166::yieldFactor(0.25), 0.0), 0.4 * 0.75, 1e-15) && eq(f170::writtenYaw(0.4, f166::yieldFactor(0.25), 0.1), 0.4 * 0.75 + 0.1, 1e-15),
              "F170: a base angle still takes the yield (1 less the hold's weight); the turn is added to what is left");
        // no turn: bit for bit what it was (the base through the yield), including the signed zero and a base that is not a number
        const double bases[] = {0.0, -0.0, 0.37, -1.2, std::numeric_limits<double>::infinity()};
        bool same = true;
        for (double b : bases)
            for (double y : {1.0, 0.75, 0.0}) {
                const double a = f170::writtenYaw(b, y, 0.0), e = b * y;
                same = same && std::memcmp(&a, &e, sizeof(a)) == 0;
            }
        const double nanBase = f170::writtenYaw(std::nan(""), 1.0, 0.0);
        check(same && std::isnan(nanBase), "F170: with no turn the written angle is the base x the yield bit for bit (the signed zero and a base that is not a number included)");
        check(f170::writtenYaw(0.0, 0.0, std::nan("")) == 0.0 && f170::writtenYaw(0.0, 1.0, std::numeric_limits<double>::infinity()) == 0.0, "F170: a turn that is not a number is not added");
        // the angle measured on the held pose: the phi is a property of the shoulders as they stand, so the same share of the same held blade is turned still and moving
        // (the hold rewrites the torso from the captured pose every call, so nothing accumulates: the turn this call is the share of this call's blade)
        const double held = -28.7 * kDeg;
        check(f170::turnRad(1.0, true, held) == f170::turnRad(1.0, true, held), "F170: the turn is a function of this call's measured phi and share alone: no state to accumulate");
    }

    // ---- 4. the left hand still reaches the grip with the shoulders turned -----------------------------------------------------------------------------------------------------------------
    // The real reach rule (nf::reachLimitDeltaFeasibleOnly) on the bladed and the squared shoulders: the left grip is out in front of the sight line, as the foregrip is.
    {
        const double R[2] = {0.60, 0.58};
        const bool have[2] = {true, true};
        // the grips: the left one 0.57 m ahead of the bladed left shoulder (in reach), the right one at the stock
        const double grip[2][3] = {{bladed[0][0] + 0.25, 1.12, bladed[0][2] + 0.52}, {bladed[1][0] - 0.08, 1.15, bladed[1][2] + 0.30}};
        const auto reach = [&](double dial, double th[3], double u[2][3]) {
            turnedAt(dial, 1.0, 1.0, u);
            const double d0[3] = {};
            double v[2][3], cand[3] = {}, full[3] = {};
            for (int s = 0; s < 2; ++s)
                for (int i = 0; i < 3; ++i) v[s][i] = u[s][i] + d0[i] - grip[s][i];
            bool feasible = true;
            nf::reachLimitDeltaFeasibleOnly(v, R, have, cand, full, &feasible);
            for (int i = 0; i < 3; ++i) th[i] = d0[i] + full[i];
            return feasible;
        };
        const auto dist = [&](const double u[3], const double th[3], const double g[3]) {
            const double d[3] = {u[0] + th[0] - g[0], u[1] + th[1] - g[1], u[2] + th[2] - g[2]};
            return nf::ikLen(d);
        };
        double th0[3], th1[3], thHalf[3], u0[2][3], u1[2][3], uHalf[2][3];
        const bool f0 = reach(0.0, th0, u0), f1 = reach(1.0, th1, u1), fh = reach(0.5, thHalf, uHalf);
        const double left0 = dist(u0[0], th0, grip[0]), right0 = dist(u0[1], th0, grip[1]);
        const double left1 = dist(u1[0], th1, grip[0]), right1 = dist(u1[1], th1, grip[1]);
        const double leftH = dist(uHalf[0], thHalf, grip[0]), rightH = dist(uHalf[1], thHalf, grip[1]);
        check(f0 && f1 && fh, "F170: the rig's grips are reachable at every dial (the reach rule's problem is feasible)");
        check(left0 <= R[0] + 1e-9 && right0 <= R[1] + 1e-9 && left1 <= R[0] + 1e-9 && right1 <= R[1] + 1e-9 && leftH <= R[0] + 1e-9 && rightH <= R[1] + 1e-9,
              "F170: with the shoulders turned (dial 0, 0.5, 1) the reach rule keeps the left shoulder within arm's length of the foregrip and the right of the stock");
        // the turn took the left shoulder back from the grip: the rule had to bring the body forward to keep the grip, more at a full dial than at none
        check(th1[2] > th0[2] + 0.02 && thHalf[2] > th0[2] && thHalf[2] < th1[2], "F170: the squared left shoulder is further from the foregrip, so the reach rule brings the body toward it by more as the dial rises (the left hand stays on the grip)");
        // the right shoulder is nearer the gun's root when squared
        const double rootD0 = dist(u0[1], th0, grip[1]), rootD1 = dist(u1[1], th1, grip[1]);
        check(rootD1 < rootD0, "F170: and the right shoulder is nearer the gun's root squared than bladed");
    }

    // ---- 5. the counters, the line -------------------------------------------------------------------------------------------------------------------------------------------------------
    {
        f170::Counters c;
        const auto frame = [&](bool moving, double dial, double before, double after, double turn, double gun, double lr, double lu, double lf, double rr, double ru, double rf) {
            f170::Sample s;
            s.moving = moving;
            s.dial = dial;
            s.phiBeforeDeg = before;
            s.phiAfterDeg = after;
            s.turnDeg = turn;
            s.rightShoulderToGunM = gun;
            s.leftShoulderM[0] = lr; s.leftShoulderM[1] = lu; s.leftShoulderM[2] = lf;
            s.rightShoulderM[0] = rr; s.rightShoulderM[1] = ru; s.rightShoulderM[2] = rf;
            c.add(s);
        };
        for (int i = 0; i < 40; ++i) frame(false, 1.0, -28.7, 0.0, -28.7, 0.40, -0.04, -0.146, -0.004, 0.17, -0.14, 0.05);   // still
        for (int i = 0; i < 60; ++i) frame(true, 1.0, -26.0, 0.0, -26.0, 0.42, 0.03, -0.14, -0.006, 0.19, -0.13, 0.07);       // moving
        for (int i = 0; i < 10; ++i) frame(true, 1.0, std::nan(""), std::nan(""), 0.0, std::nan(""), std::nan(""), 0.0, 0.0, std::nan(""), 0.0, 0.0);   // moving, nothing measured
        check(c.ads == 110 && c.still == 40 && c.moving == 70 && c.beforeN[0] == 40 && c.beforeN[1] == 60 && c.afterN[1] == 60 && c.turnN[0] == 40 && c.turnN[1] == 70 && c.bladed == 100 && c.turned == 100 &&
                  c.leftN[0] == 40 && c.leftN[1] == 60 && c.rightN[1] == 60 && c.gunN[0] == 40 && c.gunN[1] == 60 && eq(c.dialSum, 110.0, 1e-9) && c.dialMax == 1.0 &&
                  eq(c.beforeSum[0] / c.beforeN[0], -28.7, 1e-9) && eq(c.turnSum[1] / c.turnN[1], -26.0 * 60.0 / 70.0, 1e-9),
              "F170: the counters keep the frames still against moving, each figure's mean over the frames it was measured on, the bladed frames and those a turn was applied in");
        f170::Counters m;
        m.merge(c);
        m.merge(c);
        check(m.ads == 220 && m.moving == 140 && m.bladed == 200 && m.turned == 200 && m.beforeN[0] == 80 && eq(m.dialSum, 220.0, 1e-9) && m.leftN[1] == 120 && eq(m.gunSum[1], 2.0 * c.gunSum[1], 1e-9),
              "F170: two windows add up");
        // a frame whose shoulder line is square already counts as no blade and no turn
        f170::Counters sq;
        f170::Sample s;
        s.dial = 1.0;
        s.phiBeforeDeg = 0.4;
        s.phiAfterDeg = 0.4;
        sq.add(s);
        check(sq.ads == 1 && sq.bladed == 0 && sq.turned == 0, "F170: a shoulder line under 1 deg off square is no blade and needs no turn");
        char line[1400];
        const int len = f170::formatSummary(line, sizeof(line), 10, "$wpn_m_shotgun_plasma_doublebarrel_name;", c);
        f170::Counters big;
        big.ads = big.still = big.moving = 4000000000u;
        big.bladed = big.turned = 4000000000u;
        for (int i = 0; i < 2; ++i) {
            big.beforeN[i] = big.afterN[i] = big.turnN[i] = big.leftN[i] = big.rightN[i] = big.gunN[i] = 4000000000u;
            big.beforeSum[i] = -1.0e12; big.afterSum[i] = 1.0e12; big.turnSum[i] = -1.0e12; big.gunSum[i] = 1.0e12;
            for (int k = 0; k < 3; ++k) { big.leftSum[i][k] = -1.0e13; big.rightSum[i][k] = 1.0e13; }
        }
        big.dialSum = 4.0e9; big.dialMax = 1.0;
        char worst[1400];
        const int worstLen = f170::formatSummary(worst, sizeof(worst), 4000000000u, "$wpn_m_shotgun_plasma_doublebarrel_name_with_a_long_tail;", big);
        check(len > 0 && len < 1100 && worstLen > 0 && worstLen < 1190 && std::strstr(line, "(F170, TEMPORARY) 10 s: item $wpn_m_shotgun_plasma_doublebarrel_name;") != nullptr &&
                  std::strstr(line, "the dial experimental.native_on_foot_crouch_ads_square mean 1.00 (largest 1.00)") != nullptr &&
                  std::strstr(line, "crouched in the sights 110 frames (still 40, moving 70)") != nullptr && std::strstr(line, "turned toward square in 100 frames") != nullptr,
              "F170: the window's line names the tag, the key and its value, the frames still against moving and the frames turned, within the log's 1200 bytes even with every figure at its worst");
        check(std::strstr(line, "turned toward square in 100 frames, the shoulder line 1 deg or more off square in 100; the turn -28.7 deg still, -22.3 moving; ") != nullptr &&
                  std::strstr(line, "before the turn -28.7 deg still, -26.0 moving (40/60 frames), as written +0.0 deg still, +0.0 moving (40/60 frames)") != nullptr,
              "F170: the turn, the measured phi before it and as written are printed in degrees, still and moving, with the frames the phis are means of");
        check(std::strstr(line, "left shoulder from the eye (right/up/forward, cm): still 40 frames -4.0/-14.6/-0.4, moving 60 frames +3.0/-14.0/-0.6") != nullptr &&
                  std::strstr(line, "right shoulder from the eye: still 40 frames +17.0/-14.0/+5.0, moving 60 frames +19.0/-13.0/+7.0") != nullptr &&
                  std::strstr(line, "right shoulder to the gun's root 40 frames still 40.0 cm, 60 moving 42.0") != nullptr,
              "F170: the left and the right shoulder from the eye and the right shoulder to the gun's root are printed in cm, still against moving");
        f170::Counters empty;
        char none[1400];
        const int noneLen = f170::formatSummary(none, sizeof(none), 1, nullptr, empty);
        check(noneLen > 0 && std::strstr(none, "item none") != nullptr && std::strstr(none, "crouched in the sights 0 frames") != nullptr && std::strstr(none, "nan") == nullptr && std::strstr(none, "inf") == nullptr,
              "F170: a window with no frame prints zeros, never a number that is not one");
    }
}

}  // namespace f170test
