// F178 checks (native on-foot VR), run178 below. Header only, run by native_frame_test.
//
// F178 (native_on_foot_f178.h, TEMPORARY): the standing ADS body dials, the crouched ones (F168 forward, F171 left) mirrored for the standing stance.
// experimental.native_on_foot_stand_ads_body_forward_cm (-20..+20 cm, positive toward the gun) and experimental.native_on_foot_stand_ads_body_left_cm (-30..+30 cm, positive left of the aim
// line), both shipped at 0, are F168's and F171's own moves with the standing weight, (1 - F163's crouched weight) x the ADS detach weight, in place of the crouched weight. The rig pins the
// weights (the crouched pair and the standing pair add up to the dial whole at every crouch), the clamps, the axes, that the crouched pair is untouched, the real reach rule on the mirrored
// move, the counters and the line, and the source sites (both keys read and logged, the dials applied beside the crouched ones in the same order, the line printed).
#pragma once

#include "../../src/d3d11/native_on_foot_core.h"
#include "../../src/d3d11/native_on_foot_f159.h"
#include "../../src/d3d11/native_on_foot_f165.h"
#include "../../src/d3d11/native_on_foot_f166.h"
#include "../../src/d3d11/native_on_foot_f168.h"
#include "../../src/d3d11/native_on_foot_f171.h"
#include "../../src/d3d11/native_on_foot_f178.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>

namespace f178test {

namespace nf = edvr::nofoot;
namespace f178 = edvr::nofoot::f178;

inline std::string slurp(const char* path) {
    std::ifstream f(path, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

template <class Check>
inline void run178(Check&& check) {
    const auto eq = [](double a, double b, double tol) { return std::fabs(a - b) <= tol; };
    const auto zero3 = [](const double v[3]) { return v[0] == 0.0 && v[1] == 0.0 && v[2] == 0.0; };
    char msg[640];

    // ---- 1. the keys' ranges ---------------------------------------------------------------------------------------------------------------------------------------------------------------
    {
        check(f178::clampForwardCm(7.5) == 7.5 && f178::clampForwardCm(-25.0) == -20.0 && f178::clampForwardCm(25.0) == 20.0 && f178::clampForwardCm(-20.0) == -20.0 &&
                  f178::clampForwardCm(20.0) == 20.0 && f178::clampForwardCm(std::nan("")) == 0.0 && f178::clampForwardCm(std::numeric_limits<double>::infinity()) == 0.0 &&
                  f178::kForwardMinCm == -20.0 && f178::kForwardMaxCm == 20.0,
              "F178: the forward dial is held to -20..+20 cm (the crouched forward dial's range); a value that is not a number reads 0");
        check(f178::clampLeftCm(7.5) == 7.5 && f178::clampLeftCm(-35.0) == -30.0 && f178::clampLeftCm(35.0) == 30.0 && f178::clampLeftCm(-30.0) == -30.0 && f178::clampLeftCm(30.0) == 30.0 &&
                  f178::clampLeftCm(std::nan("")) == 0.0 && f178::clampLeftCm(-std::numeric_limits<double>::infinity()) == 0.0 && f178::kLeftMinCm == -30.0 && f178::kLeftMaxCm == 30.0,
              "F178: the left dial is held to -30..+30 cm (the crouched left dial's range); a value that is not a number reads 0");
    }

    // ---- 2. the weights --------------------------------------------------------------------------------------------------------------------------------------------------------------------
    {
        // w_c 0: the standing pair whole, the crouched pair 0; w_c 1: the reverse; 0.5 halves both; detach 0: nothing
        check(f178::weight(0.0, 1.0) == 1.0 && f178::weight(1.0, 1.0) == 0.0 && f178::weight(0.5, 1.0) == 0.5 && f178::weight(0.25, 1.0) == 0.75 && f178::weight(0.0, 0.5) == 0.5 &&
                  f178::weight(0.5, 0.5) == 0.25 && f178::weight(0.0, 0.0) == 0.0 && f178::weight(1.0, 0.0) == 0.0 && f178::weight(0.5, 0.0) == 0.0,
              "F178: the standing weight is (1 - the crouched weight) x the detach weight: 1 standing in the sights, 0 crouched in them, half at half a crouch, 0 at the hip whatever the stance");
        check(f178::weight(-0.5, 1.0) == 1.0 && f178::weight(1.5, 1.0) == 0.0 && f178::weight(0.0, 2.0) == 1.0 && f178::weight(0.0, -1.0) == 0.0 && f178::weight(std::nan(""), 1.0) == 1.0 &&
                  f178::weight(0.0, std::nan("")) == 0.0 && f178::standingShare(std::nan("")) == 1.0,
              "F178: each weight is held to 0..1; a crouched weight that is not a number reads standing (as F161 reads it), a detach weight that is not a number reads the hip");
        const double w[5] = {0.0, 0.25, 0.5, 0.75, 1.0};
        bool wholeAtEveryCrouch = true;
        for (int i = 0; i < 5; ++i) {
            // the crouched pair and the standing pair with the same dial in both: a crouch blends one into the other, the two moves always add up to the dial whole (in the sights)
            const double fwd = nf::f168::shiftM(8.0, w[i], 1.0) + f178::forwardShiftM(8.0, w[i], 1.0);
            const double left = nf::f171::shiftM(-12.0, w[i], 1.0) + f178::leftShiftM(-12.0, w[i], 1.0);
            wholeAtEveryCrouch = wholeAtEveryCrouch && eq(fwd, 0.08, 1e-15) && eq(left, -0.12, 1e-15);
        }
        check(wholeAtEveryCrouch, "F178: with the same dial in both pairs, the crouched move and the standing move add up to the dial whole at every crouch weight: a crouch blends one pair into the other");
        check(eq(f178::forwardShiftM(10.0, 0.0, 1.0), 0.10, 1e-15) && f178::forwardShiftM(10.0, 1.0, 1.0) == 0.0 && eq(f178::forwardShiftM(10.0, 0.5, 1.0), 0.05, 1e-15) &&
                  f178::forwardShiftM(10.0, 0.0, 0.0) == 0.0 && f178::forwardShiftM(0.0, 0.0, 1.0) == 0.0 && f178::forwardShiftM(std::nan(""), 0.0, 1.0) == 0.0 &&
                  eq(f178::forwardShiftM(35.0, 0.0, 1.0), 0.20, 1e-15) && eq(f178::forwardShiftM(-35.0, 0.0, 1.0), -0.20, 1e-15) && eq(f178::forwardShiftM(10.0, 0.5, 0.5), 0.025, 1e-15),
              "F178: the forward move is the dial in metres x the standing weight: 10 cm standing in the sights is 0.10 m, 0 crouched, 5 cm at half a crouch, 0 at the hip and at the dial 0, the dial clamped first");
        check(eq(f178::leftShiftM(15.0, 0.0, 1.0), 0.15, 1e-15) && f178::leftShiftM(15.0, 1.0, 1.0) == 0.0 && eq(f178::leftShiftM(15.0, 0.5, 1.0), 0.075, 1e-15) &&
                  f178::leftShiftM(15.0, 0.0, 0.0) == 0.0 && f178::leftShiftM(0.0, 0.0, 1.0) == 0.0 && f178::leftShiftM(std::nan(""), 0.0, 1.0) == 0.0 &&
                  eq(f178::leftShiftM(50.0, 0.0, 1.0), 0.30, 1e-15) && eq(f178::leftShiftM(-50.0, 0.0, 1.0), -0.30, 1e-15) && eq(f178::leftShiftM(-15.0, 0.0, 1.0), -0.15, 1e-15),
              "F178: the left move is the dial in metres x the standing weight, positive left: 15 cm standing in the sights is 0.15 m, 0 crouched, 7.5 cm at half a crouch, 0 at the hip and at the dial 0");
        double last = -1.0;
        bool rising = true;
        for (int i = 0; i <= 10; ++i) {
            const double detach = i / 10.0;
            const double m = f178::leftShiftM(15.0, 0.0, detach);
            rising = rising && m >= last;
            last = m;
        }
        check(rising && eq(last, 0.15, 1e-15), "F178: through a press (the detach weight rising, standing) the move rises without a step from 0 to the full 15 cm");
    }

    // ---- 3. the gate: standing in the sights, the complement of the crouched ones -----------------------------------------------------------------------------------------------------------------
    {
        bool complement = true, bothFalseAtHip = true;
        const double ws[7] = {0.0, 0.2, 0.49, 0.5, 0.8, 1.0, std::nan("")};
        const double ds[6] = {0.0, 0.04, 0.05, 0.5, 1.0, std::nan("")};
        for (double wc : ws)
            for (double d : ds) {
                const bool stand = f178::standingAds(wc, d), crouched = nf::f165::crouchedAds(wc, d);
                if (nf::f159::atHip(d)) bothFalseAtHip = bothFalseAtHip && !stand && !crouched;
                else complement = complement && stand != crouched;
            }
        check(complement && bothFalseAtHip && f178::standingAds(0.0, 1.0) && f178::standingAds(0.49, 1.0) && !f178::standingAds(0.5, 1.0) && !f178::standingAds(1.0, 1.0) && !f178::standingAds(0.0, 0.0) &&
                  !f178::standingAds(0.0, 0.04) && f178::standingAds(0.0, 0.05) && f178::standingAds(std::nan(""), 1.0),
              "F178: a frame is standing in the sights when it is not at the hip (the detach weight from 0.05) and the crouched weight is under 0.5: in the sights, exactly one of crouched and standing; at the hip neither");
    }

    // ---- 4. the vectors: the crouched ones' functions with the standing share, along the same axes ------------------------------------------------------------------------------------------------
    {
        const double aim[3] = {0.6, 0.3, 0.8};   // 0.6, 0.8 horizontal (length 1), looking 17 deg up
        double stand[3], crouched[3];
        // the mirror: standing the standing vector is the crouched vector at the crouched weight 1, bit for bit; crouched it is not made
        bool mirrored = true;
        const double dials[5] = {-20.0, -7.5, 0.5, 10.0, 20.0};
        for (double dial : dials) {
            const bool a = f178::forwardVector(dial, 0.0, 1.0, aim, stand), b = nf::f168::shiftVector(dial, 1.0, 1.0, aim, crouched);
            mirrored = mirrored && a == b && std::memcmp(stand, crouched, sizeof(stand)) == 0;
        }
        for (double dial : {-30.0, -15.0, 0.5, 15.0, 30.0}) {
            const bool a = f178::leftVector(dial, 0.0, 1.0, aim, stand), b = nf::f171::shiftVector(dial, 1.0, 1.0, aim, crouched);
            mirrored = mirrored && a == b && std::memcmp(stand, crouched, sizeof(stand)) == 0;
        }
        check(mirrored, "F178: standing in the sights the forward and the left vectors are F168's and F171's, bit for bit, at the crouched weight 1: same code, same axes, same length");
        double out[3] = {9.0, 9.0, 9.0};
        const bool fwdCrouched = f178::forwardVector(10.0, 1.0, 1.0, aim, out);
        const bool fwdZero = zero3(out);
        out[0] = out[1] = out[2] = 9.0;
        const bool leftCrouched = f178::leftVector(15.0, 1.0, 1.0, aim, out);
        const bool leftZero = zero3(out);
        out[0] = out[1] = out[2] = 9.0;
        const bool fwdHip = f178::forwardVector(10.0, 0.0, 0.0, aim, out);
        const bool fwdHipZero = zero3(out);
        out[0] = out[1] = out[2] = 9.0;
        const bool leftHip = f178::leftVector(15.0, 0.0, 0.0, aim, out);
        const bool leftHipZero = zero3(out);
        out[0] = out[1] = out[2] = 9.0;
        const bool fwdOff = f178::forwardVector(0.0, 0.0, 1.0, aim, out);
        const bool fwdOffZero = zero3(out);
        out[0] = out[1] = out[2] = 9.0;
        const bool leftOff = f178::leftVector(0.0, 0.0, 1.0, aim, out);
        const bool leftOffZero = zero3(out);
        check(!fwdCrouched && fwdZero && !leftCrouched && leftZero && !fwdHip && fwdHipZero && !leftHip && leftHipZero && !fwdOff && fwdOffZero && !leftOff && leftOffZero,
              "F178: crouched, at the hip and with the dial at 0 neither vector is made and each is exactly zero");
        const double up[3] = {0.0, 1.0, 0.0}, nearUp[3] = {0.05, 0.99, 0.05}, bad[3] = {std::nan(""), 0.0, 1.0};
        check(!f178::forwardVector(10.0, 0.0, 1.0, up, out) && !f178::forwardVector(10.0, 0.0, 1.0, nearUp, out) && !f178::forwardVector(10.0, 0.0, 1.0, bad, out) &&
                  !f178::forwardVector(10.0, 0.0, 1.0, nullptr, out) && !f178::leftVector(15.0, 0.0, 1.0, up, out) && !f178::leftVector(15.0, 0.0, 1.0, nearUp, out) &&
                  !f178::leftVector(15.0, 0.0, 1.0, bad, out) && !f178::leftVector(15.0, 0.0, 1.0, nullptr, out),
              "F178: an aim with no horizontal forward (straight up, under 0.1 horizontal, not a number, none) moves nothing");

        // the axes: forward along the aim's horizontal forward, left across it to the left of the eye as headingAxes reads the eye's right (-1), none vertical, at every yaw
        bool forwardAlong = true, leftOfEye = true, vertical = true, lengths = true;
        for (int deg = 0; deg < 360; deg += 15) {
            const double a = deg * 3.14159265358979323846 / 180.0;
            const double fwd[3] = {std::sin(a), 0.2, std::cos(a)};
            const double h[3] = {std::sin(a), 0.0, std::cos(a)};
            double f[3], l[3], axesF[3], axesL[3];
            const bool okF = f178::forwardVector(10.0, 0.0, 1.0, fwd, f), okL = f178::leftVector(10.0, 0.0, 1.0, fwd, l);
            nf::headingAxes(h, f, axesF);
            nf::headingAxes(h, l, axesL);
            forwardAlong = forwardAlong && okF && eq(axesF[2], 0.10, 1e-12) && eq(axesF[0], 0.0, 1e-12);   // along the aim, none across
            leftOfEye = leftOfEye && okL && eq(axesL[0], -0.10, 1e-12) && eq(axesL[2], 0.0, 1e-12);          // across it, to the left (the eye's right reads -)
            vertical = vertical && f[1] == 0.0 && l[1] == 0.0;
            lengths = lengths && eq(std::sqrt(f[0] * f[0] + f[2] * f[2]), 0.10, 1e-12) && eq(std::sqrt(l[0] * l[0] + l[2] * l[2]), 0.10, 1e-12);
        }
        check(forwardAlong && leftOfEye && vertical && lengths,
              "F178: at every yaw (every 15 deg) a 10 cm forward dial moves the body 10 cm along the aim's horizontal forward (the eye's forward reads +, its right 0) and a 10 cm left dial 10 cm to the left of "
              "the aim (the eye's right reads -10 cm, its forward 0), none vertical");
        // the shoulder as the lines read it: standing a +15 cm left dial moves the shoulder 15 cm to the left of the eye (its right offset falls by 15 cm), a +10 cm forward dial 10 cm toward the gun
        const double eye[3] = {1.0, 1.6, 2.0}, shoulderBefore[3] = {1.0, 1.45, 1.95};
        double shiftL[3], shiftF[3], before[3], afterL[3], afterF[3];
        const bool madeL = f178::leftVector(15.0, 0.0, 1.0, aim, shiftL), madeF = f178::forwardVector(10.0, 0.0, 1.0, aim, shiftF);
        const double shoulderL[3] = {shoulderBefore[0] + shiftL[0], shoulderBefore[1] + shiftL[1], shoulderBefore[2] + shiftL[2]};
        const double shoulderF[3] = {shoulderBefore[0] + shiftF[0], shoulderBefore[1] + shiftF[1], shoulderBefore[2] + shiftF[2]};
        nf::f166::offsetFromEye(shoulderBefore, eye, aim[0], aim[2], before);
        nf::f166::offsetFromEye(shoulderL, eye, aim[0], aim[2], afterL);
        nf::f166::offsetFromEye(shoulderF, eye, aim[0], aim[2], afterF);
        check(madeL && madeF && eq(afterL[0] - before[0], -0.15, 1e-12) && eq(afterL[1] - before[1], 0.0, 1e-12) && eq(afterL[2] - before[2], 0.0, 1e-12) && eq(afterF[0] - before[0], 0.0, 1e-12) &&
                  eq(afterF[2] - before[2], 0.10, 1e-12),
              "F178: as the lines read the shoulder from the eye, a +15 cm left dial standing takes 15 cm off its right offset and nothing off its up and forward offsets, a +10 cm forward dial adds 10 cm to its forward offset");
        double neg[3];
        f178::leftVector(-15.0, 0.0, 1.0, aim, neg);
        check(eq(neg[0], -shiftL[0], 1e-15) && eq(neg[2], -shiftL[2], 1e-15), "F178: a negative left dial moves it to the right, the same line");
    }

    // ---- 5. the split around the reach rule, and the crouched pair unchanged ---------------------------------------------------------------------------------------------------------------------
    {
        const double aim[3] = {0.0, 0.0, 1.0};
        double shift[3], pre[3], post[3];
        // a positive forward dial rides after the reach rule (F169), a negative one before it, as the crouched dial does: the standing vector goes through F168's own split
        f178::forwardVector(10.0, 0.0, 1.0, aim, shift);
        nf::f168::splitShift(10.0, shift, pre, post);
        check(nf::f168::afterReach(10.0) && zero3(pre) && eq(post[2], 0.10, 1e-15), "F178: a positive forward dial's move is made after the reach rule (all of it in post), as the crouched dial's is");
        f178::forwardVector(-10.0, 0.0, 1.0, aim, shift);
        nf::f168::splitShift(-10.0, shift, pre, post);
        check(!nf::f168::afterReach(-10.0) && zero3(post) && eq(pre[2], -0.10, 1e-15), "F178: a negative forward dial's move rides the target before the reach rule (all of it in pre)");
        // the pin's two additions with the standing dials at 0: the crouched pair's post is copied unsummed, the target takes nothing: bit for bit what it was
        double crouchedPre[3], crouchedPost[3], crouchedShift[3];
        nf::f168::shiftVector(-5.0, 1.0, 1.0, aim, crouchedShift);
        nf::f168::splitShift(-5.0, crouchedShift, crouchedPre, crouchedPost);
        const double target[3] = {0.12, 0.92, 0.35};
        double t1[3] = {target[0], target[1], target[2]}, t2[3] = {target[0], target[1], target[2]};
        nf::weaponViewOffsetTarget(t1, crouchedPre);
        double standShiftF[3], standShiftL[3];
        const bool madeF = f178::forwardVector(0.0, 0.0, 1.0, aim, standShiftF), madeL = f178::leftVector(0.0, 0.0, 1.0, aim, standShiftL);
        nf::weaponViewOffsetTarget(t2, crouchedPre);
        if (madeF) nf::weaponViewOffsetTarget(t2, standShiftF);   // the pin's `if (forwardVector(...))`: not made, not applied
        if (madeL) nf::weaponViewOffsetTarget(t2, standShiftL);
        check(!madeF && !madeL && std::memcmp(t1, t2, sizeof(t1)) == 0, "F178: with both standing dials at 0 no standing move is made and the crouched pair's target is bit for bit what it was");
        // the crouched pair at its flown values (5 cm forward, 25 cm left) crouched in the sights: the standing share is 0, the crouched vectors are F168's and F171's as ever
        double cF[3], cL[3];
        const bool inSights = nf::f168::shiftVector(5.0, 1.0, 1.0, aim, cF) && nf::f171::shiftVector(25.0, 1.0, 1.0, aim, cL);
        double sF[3] = {9.0, 9.0, 9.0}, sL[3] = {9.0, 9.0, 9.0};
        const bool standingMade = f178::forwardVector(5.0, 1.0, 1.0, aim, sF) || f178::leftVector(25.0, 1.0, 1.0, aim, sL);
        check(inSights && eq(cF[2], 0.05, 1e-15) && eq(-cL[0], 0.25, 1e-15) && !standingMade && zero3(sF) && zero3(sL),
              "F178: crouched in the sights at the crouched pair's flown values (5 cm forward, 25 cm left) the crouched vectors are what they were and the standing pair with the same numbers is zero");
    }

    // ---- 6. the real reach rule: the standing left move survives it like the crouched one ---------------------------------------------------------------------------------------------------------
    // The same geometry as F171's: the eye at the origin, the aim along +z; the shoulders 36 cm apart, the left one almost under the eye; the left grip on the aim line 58 cm ahead (62.3 cm from the left
    // shoulder, past the 60 cm boundary), the gun's root 22 cm ahead of the right shoulder's side.
    {
        const double shoulder[2][3] = {{-0.01, -0.146, -0.04}, {0.35, -0.146, -0.04}};
        const double grip[2][3] = {{0.0, -0.20, 0.58}, {0.0, -0.10, 0.22}};
        const double R[2] = {0.60, 0.58};
        const bool have[2] = {true, true};
        const auto reached = [&](const double d0[3], double th[3], double full[3]) {
            double v[2][3];
            for (int s = 0; s < 2; ++s)
                for (int i = 0; i < 3; ++i) v[s][i] = shoulder[s][i] + d0[i] - grip[s][i];
            double cand[3] = {}, f[3] = {};
            bool feasible = true;
            nf::reachLimitDeltaFeasibleOnly(v, R, have, cand, f, &feasible);
            for (int i = 0; i < 3; ++i) {
                th[i] = d0[i] + f[i];
                full[i] = f[i];
            }
        };
        const auto dist = [&](int s, const double th[3]) {
            const double d[3] = {shoulder[s][0] + th[0] - grip[s][0], shoulder[s][1] + th[1] - grip[s][1], shoulder[s][2] + th[2] - grip[s][2]};
            return nf::ikLen(d);
        };
        const double fwd[3] = {0.0, 0.0, 1.0};
        const double d0[3] = {0.0, 0.0, 0.0};
        double th0[3], full0[3];
        reached(d0, th0, full0);
        double shift[3], th1[3], full1[3];
        const bool made = f178::leftVector(15.0, 0.0, 1.0, fwd, shift);
        reached(shift, th1, full1);
        const double reachDelta[3] = {full1[0] - full0[0], full1[1] - full0[1], full1[2] - full0[2]};
        const double written = nf::f171::writtenLeftM(shift, reachDelta, fwd);
        std::snprintf(msg, sizeof(msg),
                      "F178: standing, a +15 cm left dial through the real reach rule (the left shoulder 62.3 cm from its grip at the dial 0) writes %.1f cm to the left of the aim, the rule took back %.2f cm, "
                      "the left shoulder is %.1f cm from its grip, within the %.0f cm reach",
                      100.0 * written, 100.0 * (0.15 - written), 100.0 * dist(0, th1), 100.0 * R[0]);
        check(made && written > 0.135 && written <= 0.15 + 1e-12 && dist(0, th1) <= R[0] + 1e-9 && dist(1, th1) <= R[1] + 1e-9, msg);
        // the same call at half a crouch moves half as far, and crouched not at all
        double half[3], none[3];
        f178::leftVector(15.0, 0.5, 1.0, fwd, half);
        const bool crouchedMade = f178::leftVector(15.0, 1.0, 1.0, fwd, none);
        check(eq(half[0], -0.075, 1e-15) && !crouchedMade && zero3(none), "F178: at half a crouch the standing left move is 7.5 cm, fully crouched it is not made");
        // the forward dial after the reach rule: the written neck takes its whole move on top of the boundary
        double fShift[3], fPre[3], fPost[3];
        f178::forwardVector(10.0, 0.0, 1.0, fwd, fShift);
        nf::f168::splitShift(10.0, fShift, fPre, fPost);
        double th[3] = {th0[0], th0[1], th0[2]}, reach[3] = {full0[0], full0[1], full0[2]};
        nf::f168::addAfterReach(th, reach, fPost);
        const double zeroDelta[3] = {0.0, 0.0, 0.0};
        const double fwdWritten = nf::f168::writtenAlongM(fPre, fPost, zeroDelta, fwd);
        check(eq(th[2] - th0[2], 0.10, 1e-15) && eq(reach[2] - full0[2], 0.10, 1e-15) && th[0] == th0[0] && th[1] == th0[1] && eq(fwdWritten, 0.10, 1e-15),
              "F178: a +10 cm forward dial standing is added whole after the reach rule, to the written neck and to the reach correction the bend aims at, and is written whole along the aim");
        // nothing added for a zero post: the pin's sum guard leaves the crouched pair's post unsummed
        double thSame[3] = {th0[0], th0[1], th0[2]}, reachSame[3] = {full0[0], full0[1], full0[2]};
        nf::f168::addAfterReach(thSame, reachSame, zeroDelta);
        check(std::memcmp(thSame, th0, sizeof(thSame)) == 0 && std::memcmp(reachSame, full0, sizeof(reachSame)) == 0, "F178: a zero post adds nothing to the written neck or the reach correction");
    }

    // ---- 7. the counters and the line ------------------------------------------------------------------------------------------------------------------------------------------------------------
    {
        f178::Counters c;
        const auto frame = [&](bool moving, bool made, double fdial, double ldial, double fasked, double lasked, double fwritten, double lwritten, double weight, double gun, double grip, double rr,
                               double ru, double rf, double lr, double lu, double lf) {
            f178::Sample s;
            s.moving = moving;
            s.weight = weight;
            s.forwardDialCm = fdial;
            s.leftDialCm = ldial;
            s.forwardMade = made && fdial != 0.0;
            s.leftMade = made && ldial != 0.0;
            s.forwardAskedM = fasked;
            s.leftAskedM = lasked;
            s.forwardWrittenM = fwritten;
            s.leftWrittenM = lwritten;
            s.rightShoulderToGunM = gun;
            s.leftShoulderToGripM = grip;
            s.rightShoulderM[0] = rr; s.rightShoulderM[1] = ru; s.rightShoulderM[2] = rf;
            s.leftShoulderM[0] = lr; s.leftShoulderM[1] = lu; s.leftShoulderM[2] = lf;
            c.add(s);
        };
        for (int i = 0; i < 40; ++i) frame(false, true, 6.0, 15.0, 0.06, 0.15, 0.06, 0.144, 1.0, 0.45, 0.60, 0.17, -0.146, -0.04, -0.16, -0.146, -0.04);   // still
        for (int i = 0; i < 60; ++i) frame(true, true, 6.0, 15.0, 0.06, 0.15, 0.06, 0.140, 0.9, 0.44, 0.61, 0.18, -0.134, -0.05, -0.15, -0.134, -0.05);    // moving
        for (int i = 0; i < 10; ++i) frame(true, false, 6.0, 15.0, 0.0, 0.0, std::nan(""), std::nan(""), 1.0, std::nan(""), std::nan(""), std::nan(""), 0.0, 0.0, std::nan(""), 0.0, 0.0);   // moving, no move made
        check(c.ads == 110 && c.still == 40 && c.moving == 70 && c.forward.made == 100 && c.left.made == 100 && c.forward.writtenN == 100 && c.left.writtenN == 100 && c.left.backN == 100 &&
                  c.gunN[1] == 60 && c.gripN[0] == 40 && c.leftN[0] == 40 && c.rightN[1] == 60 && eq(c.forward.dialSum, 660.0, 1e-9) && eq(c.left.dialSum, 1650.0, 1e-9) &&
                  c.forward.dialAbsMax == 6.0 && c.left.dialAbsMax == 15.0 && eq(c.weightSum, 40.0 * 1.0 + 60.0 * 0.9 + 10.0 * 1.0, 1e-9) && eq(c.left.backSum / c.left.backN, 0.15 - (40 * 0.144 + 60 * 0.140) / 100.0, 1e-12),
              "F178: the counters keep the frames still against moving, the frames each move was made in, each figure's mean over the frames it was measured on, what the reach rule took back, each dial's sum and "
              "largest, the standing weight's sum");
        f178::Counters m;
        m.merge(c);
        m.merge(c);
        check(m.ads == 220 && m.moving == 140 && m.forward.made == 200 && m.left.made == 200 && m.gunN[1] == 120 && m.left.backN == 200 && eq(m.gunSum[1], 2.0 * c.gunSum[1], 1e-9) &&
                  eq(m.left.dialSum, 3300.0, 1e-9) && m.left.dialAbsMax == 15.0 && eq(m.weightSum, 2.0 * c.weightSum, 1e-9) && m.leftN[0] == 80 && m.rightN[1] == 120 &&
                  eq(m.rightSum[1][0], 2.0 * c.rightSum[1][0], 1e-9),
              "F178: two windows add up");
        char line[1400];
        const int len = f178::formatSummary(line, sizeof(line), 10, "$wpn_m_assaultrifle_plasma_name;", c);
        // every figure at what a 10 s window can hold: 900 frames, the dials and the moves at their limits, the shoulders 1 m from the eye, the distances 1 m
        f178::Counters big;
        big.ads = 900; big.still = big.moving = 450; big.weightSum = 900.0;
        big.forward.made = big.left.made = 900;
        big.forward.writtenN = big.left.writtenN = big.forward.backN = big.left.backN = 900;
        big.forward.dialSum = -20.0 * 900.0; big.forward.dialAbsMax = 20.0; big.left.dialSum = -30.0 * 900.0; big.left.dialAbsMax = 30.0;
        big.forward.askedSum = big.forward.writtenSum = big.forward.backSum = -0.20 * 900.0;
        big.left.askedSum = big.left.writtenSum = big.left.backSum = -0.30 * 900.0;
        for (int i = 0; i < 2; ++i) {
            big.leftN[i] = big.rightN[i] = big.gunN[i] = big.gripN[i] = 450;
            big.gunSum[i] = 1.0 * 450.0; big.gripSum[i] = 1.0 * 450.0;
            for (int k = 0; k < 3; ++k) { big.leftSum[i][k] = -1.0 * 450.0; big.rightSum[i][k] = -1.0 * 450.0; }
        }
        char worst[1400];
        const int worstLen = f178::formatSummary(worst, sizeof(worst), 10, "$wpn_m_shotgun_plasma_doublebarrel_name_with_a_long_tail;", big);
        // and counts and sums no window could hold: it ends inside the buffer, never past it
        f178::Counters absurd = big;
        absurd.ads = absurd.still = absurd.moving = 4000000000u;
        absurd.forward.made = absurd.left.made = absurd.forward.writtenN = absurd.left.writtenN = absurd.forward.backN = absurd.left.backN = 4000000000u;
        absurd.forward.dialSum = absurd.left.dialSum = -1.0e12;
        absurd.forward.askedSum = absurd.left.askedSum = -1.0e12;
        for (int i = 0; i < 2; ++i) {
            absurd.leftN[i] = absurd.rightN[i] = absurd.gunN[i] = absurd.gripN[i] = 4000000000u;
            absurd.gunSum[i] = 1.0e12;
            for (int k = 0; k < 3; ++k) { absurd.leftSum[i][k] = -1.0e13; absurd.rightSum[i][k] = -1.0e13; }
        }
        char absurdLine[1200];
        const int absurdLen = f178::formatSummary(absurdLine, sizeof(absurdLine), 4000000000u, "$wpn_m_shotgun_plasma_doublebarrel_name_with_a_long_tail;", absurd);
        std::snprintf(msg, sizeof(msg),
                      "F178: the window's line names the tag, the item, both keys and their values, the frames, the standing weight, the moves asked, as written and taken back, within the log's 1166 bytes (1200 less "
                      "its stamp and truncation marker) even with every figure at its worst (%d bytes typical, %d worst)",
                      len, worstLen);
        check(len > 0 && worstLen > 0 && worstLen < 1166 && absurdLen > 0 && std::strlen(absurdLine) < sizeof(absurdLine) &&
                  std::strstr(line, "(F178, TEMPORARY) 10 s: item $wpn_m_assaultrifle_plasma_name;") != nullptr &&
                  std::strstr(line, "standing in the sights 110 frames (still 40, moving 70), the standing weight ((1 - the crouched weight) x the detach weight) mean 0.95") != nullptr, msg);
        check(std::strstr(line, "forward dial experimental.native_on_foot_stand_ads_body_forward_cm mean +6.0 cm (largest 6.0; positive is toward the gun), the move made in 100, asked +5.5 cm, "
                                "as written (after the reach rule; 100 frames) +6.0, the reach rule took back +0.0") != nullptr &&
                  std::strstr(line, "left dial experimental.native_on_foot_stand_ads_body_left_cm mean +15.0 cm (largest 15.0; positive is left of the aim), the move made in 100, asked +13.6 cm, "
                                    "as written (after the reach rule; 100 frames) +14.2, the reach rule took back +0.8") != nullptr,
              "F178: each dial's line names its key, the mean and largest value, the frames its move was made in, the move asked (a mean over all the frames), as written (a mean over the frames it was "
              "measured on) and what the reach rule took back");
        check(std::strstr(line, "right shoulder from the eye (right/up/forward, cm): still 40 frames +17.0/-14.6/-4.0, moving 60 frames +18.0/-13.4/-5.0") != nullptr &&
                  std::strstr(line, "left shoulder from the eye: still 40 frames -16.0/-14.6/-4.0, moving 60 frames -15.0/-13.4/-5.0") != nullptr &&
                  std::strstr(line, "right shoulder to the gun's root 40 frames still 45.0 cm, 60 moving 44.0; left shoulder to the left-hand grip 40 frames still 60.0 cm, 60 moving 61.0") != nullptr,
              "F178: both shoulders from the eye, the right shoulder to the gun's root and the left shoulder to the left grip are printed in cm, still against moving, with the frames each is a mean of");
        // the before picture: the dials at 0 still print every measure
        f178::Counters before;
        for (int i = 0; i < 50; ++i) {
            f178::Sample s;
            s.weight = 1.0;
            s.forwardWrittenM = 0.0;
            s.leftWrittenM = 0.0;
            s.rightShoulderToGunM = 0.46;
            s.leftShoulderToGripM = 0.62;
            s.rightShoulderM[0] = 0.18; s.rightShoulderM[1] = -0.146; s.rightShoulderM[2] = -0.04;
            s.leftShoulderM[0] = -0.01; s.leftShoulderM[1] = -0.146; s.leftShoulderM[2] = -0.04;
            before.add(s);
        }
        char beforeLine[1400];
        f178::formatSummary(beforeLine, sizeof(beforeLine), 1, "x", before);
        check(before.ads == 50 && std::strstr(beforeLine, "mean +0.0 cm (largest 0.0; positive is toward the gun), the move made in 0, asked +0.0 cm") != nullptr &&
                  std::strstr(beforeLine, "mean +0.0 cm (largest 0.0; positive is left of the aim), the move made in 0, asked +0.0 cm") != nullptr &&
                  std::strstr(beforeLine, "right shoulder to the gun's root 50 frames still 46.0 cm, 0 moving 0.0; left shoulder to the left-hand grip 50 frames still 62.0 cm, 0 moving 0.0") != nullptr &&
                  std::strstr(beforeLine, "right shoulder from the eye (right/up/forward, cm): still 50 frames +18.0/-14.6/-4.0") != nullptr,
              "F178: a window with both dials at 0 still prints the shoulders and the distances (the before picture), with the moves reading 0 and made in 0");
        f178::Counters empty;
        char noneLine[1400];
        const int noneLen = f178::formatSummary(noneLine, sizeof(noneLine), 1, nullptr, empty);
        check(noneLen > 0 && std::strstr(noneLine, "item none") != nullptr && std::strstr(noneLine, "standing in the sights 0 frames") != nullptr && std::strstr(noneLine, "nan") == nullptr &&
                  std::strstr(noneLine, "inf") == nullptr,
              "F178: a window with no frame prints zeros, never a number that is not one");
        // what the log shows if the move never ran: the made count is 0 and the asked figure reads 0 while the dial reads 15
        f178::Counters idle;
        for (int i = 0; i < 50; ++i) {
            f178::Sample s;
            s.leftDialCm = 15.0;
            s.leftMade = false;
            s.leftAskedM = f178::leftShiftM(15.0, 1.0, 1.0);   // crouched: the standing share is 0, the move is 0
            s.leftWrittenM = 0.0;
            idle.add(s);
        }
        char idleLine[1400];
        f178::formatSummary(idleLine, sizeof(idleLine), 1, "x", idle);
        check(std::strstr(idleLine, "left dial experimental.native_on_foot_stand_ads_body_left_cm mean +15.0 cm") != nullptr && std::strstr(idleLine, "the move made in 0, asked +0.0 cm") != nullptr,
              "F178: a dial of 15 with the move never made reads 'the move made in 0' and 0 cm asked: not the same as a move that was made and written");
    }

    // ---- 8. the source sites: both keys read and logged, the dials applied beside the crouched ones, the line printed -------------------------------------------------------------------------------
    {
        const std::string cpp = slurp("src/d3d11/native_on_foot.cpp"), ini = slurp("edvr.ini"), logpin = slurp("src/d3d11/native_on_foot_logpin.h");
        check(!cpp.empty() && !ini.empty() && !logpin.empty(), "F178: the source pins read src/d3d11/native_on_foot.cpp, edvr.ini and native_on_foot_logpin.h (run the rig from the repository root)");
        const auto has = [&](const std::string& needle) { return cpp.find(needle) != std::string::npos; };
        const auto at = [&](const std::string& needle) { return cpp.find(needle); };
        // both keys: the atomic, the read with its fallback, the clamp, the first-read-and-change log line, the store
        check(has("std::atomic<float> g_standAdsBodyCm{4.0f};") && has("std::atomic<float> g_standAdsBodyLeftCm{23.0f};") &&
                  has("nofoot::f178::clampForwardCm(static_cast<double>(cfg.getFloat(\"experimental.native_on_foot_stand_ads_body_forward_cm\", 4.0f)))") &&
                  has("nofoot::f178::clampLeftCm(static_cast<double>(cfg.getFloat(\"experimental.native_on_foot_stand_ads_body_left_cm\", 23.0f)))"),
              "F178 (F181: the pilot's values): both keys are read from [experimental] with the defaults 4 and 23 (was 0 and 0: the atomic, the fallback) and held to their ranges");
        check(has("if (first || cm != g_standAdsBodyCmLast)") && has("if (first || cm != g_standAdsBodyLeftCmLast)") &&
                  has("native on foot standing ADS body dial (F178, TEMPORARY): experimental.native_on_foot_stand_ads_body_forward_cm = %+.1f cm") &&
                  has("native on foot standing ADS body dial (F178, TEMPORARY): experimental.native_on_foot_stand_ads_body_left_cm = %+.1f cm") &&
                  has("g_standAdsBodyCm.store(static_cast<float>(cm), std::memory_order_relaxed);") && has("g_standAdsBodyLeftCm.store(static_cast<float>(cm), std::memory_order_relaxed);"),
              "F178: both keys are logged at the first read and at each change, and stored for the pin");
        // the pin: the crouched pair as it was (the crouched posture weight), the standing pair beside it with the same call shape
        check(has("nofoot::f168::shiftVector(pc.f168DialCm, pc.postureW, pc.detachW, fwdPin, pc.f168Shift)") && has("nofoot::f171::shiftVector(pc.f171DialCm, pc.postureW, pc.detachW, fwdPin, pc.f171Shift)") &&
                  has("pc.f178FwdDialCm = static_cast<double>(g_standAdsBodyCm.load(std::memory_order_relaxed));") &&
                  has("pc.f178LeftDialCm = static_cast<double>(g_standAdsBodyLeftCm.load(std::memory_order_relaxed));") &&
                  has("nofoot::f178::forwardVector(pc.f178FwdDialCm, pc.postureW, pc.detachW, fwdPin, pc.f178FwdShift)") &&
                  has("nofoot::f178::leftVector(pc.f178LeftDialCm, pc.postureW, pc.detachW, fwdPin, pc.f178LeftShift)") &&
                  has("nofoot::f168::splitShift(pc.f178FwdDialCm, pc.f178FwdShift, pc.f178FwdPre, pc.f178FwdPost);") && has("if (!nofoot::f168::afterReach(pc.f178FwdDialCm)) {"),
              "F178: the pin reads both dials and makes the standing vectors with the same arguments as the crouched pair (the crouched calls are unchanged), splitting the forward move around the reach rule as F169 does");
        const size_t crouchedF = at("nofoot::f168::shiftVector(pc.f168DialCm"), crouchedL = at("nofoot::f171::shiftVector(pc.f171DialCm"), standF = at("nofoot::f178::forwardVector(pc.f178FwdDialCm"),
                     standL = at("nofoot::f178::leftVector(pc.f178LeftDialCm"), f170 = at("pc.f170Dial = static_cast<double>(g_crouchAdsSquare");
        check(crouchedF != std::string::npos && crouchedF < crouchedL && crouchedL < standF && standF < standL && standL < f170,
              "F178: the standing vectors are made right after the crouched ones and before the torso square, in the crouched pair's order (forward, then left)");
        // after the reach rule: the positive forward move is summed into the post only when made; the crouched written move keeps its own post; the standing one has its own
        check(has("if (pc.f178FwdPost[0] != 0.0 || pc.f178FwdPost[2] != 0.0)") && has("for (int i = 0; i < 3; ++i) f168Post[i] += pc.f178FwdPost[i];") &&
                  has("double f168Post[3] = {pc.f168Post[0], pc.f168Post[1], pc.f168Post[2]};") &&
                  has("pc.f168WrittenM = nofoot::f168::writtenAlongM(pc.f168Pre, pc.f168Post, pc.f168ReachDelta, pc.f168Fwd);") &&
                  has("pc.f178FwdWrittenM = nofoot::f168::writtenAlongM(pc.f178FwdPre, pc.f178FwdPost, pc.f178FwdReachDelta, pc.f168Fwd);") &&
                  has("pc.f178LeftWrittenM = nofoot::f171::writtenLeftM(pc.f178LeftShift, pc.f178LeftReachDelta, pc.f171Fwd);"),
              "F178: the positive forward move is summed into the post-reach vector only when it is made; the crouched written move uses the crouched post alone, the standing ones their own");
        check(has("reachDeltaWithout(pc.f178FwdPre, pc.f178FwdReachDelta)") && has("reachDeltaWithout(pc.f178LeftShift, pc.f178LeftReachDelta)") &&
                  has("nofoot::weaponViewOffsetTarget(physicalTarget, pc.f178FwdPre);") && has("nofoot::weaponViewOffsetTarget(physicalTarget, pc.f178LeftShift);") &&
                  has("pc.f178FwdReachDelta[i] *= pc.detachW;") && has("pc.f178LeftReachDelta[i] *= pc.detachW;"),
              "F178: the reach rule's correction with each standing move taken out is measured, and the neck-rest path takes the moves and scales the corrections as it does for the crouched pair");
        // the sample and the line: the standing frames filed after the crouched ones in the same block, the line printed on the frames and no other condition
        const size_t file171 = at("f.f171.add(sample171);"), file178 = at("f.f178.add(sample178);");
        check(has("if (nofoot::f178::standingAds(pinCall.postureW, pinCall.detachW)) {") && file171 != std::string::npos && file171 < file178 && has("nofoot::f178::Counters f178;") &&
                  has("if (f98.f178.ads) {") && has("nofoot::f178::formatSummary(line178, sizeof(line178), w.seconds, w.item, f98.f178);") && has("#include \"native_on_foot_f178.h\""),
              "F178: the frames standing in the sights are filed in the F166 block after the crouched ones, F98Left carries the counters, and the line is printed whenever a frame was filed");
        // the crouched sample is gated by crouchedAds as ever
        check(has("if (sample166.crouchedAds) {") && has("sample171.askedM = nofoot::f171::shiftM(pinCall.f171DialCm, pinCall.postureW, pinCall.detachW);"),
              "F178: the crouched samples are filed as before (the crouched weight, not the standing share)");
        // the ini: both keys beside the crouched pair, TEMPORARY, with their ui rows and the default 0
        const size_t crouchedLeftKey = ini.find("\nnative_on_foot_crouch_ads_body_left_cm = "), standFwdKey = ini.find("\nnative_on_foot_stand_ads_body_forward_cm = 4.0\n"),
                     standLeftKey = ini.find("\nnative_on_foot_stand_ads_body_left_cm = 23.0\n"), squareKey = ini.find("\nnative_on_foot_crouch_ads_square = ");
        check(crouchedLeftKey != std::string::npos && standFwdKey != std::string::npos && standLeftKey != std::string::npos && squareKey != std::string::npos && crouchedLeftKey < standFwdKey &&
                  standFwdKey < standLeftKey && standLeftKey < squareKey &&
                  ini.find("# TEMPORARY (F178): Standing ADS body forward / back") != std::string::npos && ini.find("# TEMPORARY (F178): Standing ADS body left / right") != std::string::npos &&
                  ini.find("# ui: Standing ADS body forward / back | range -20..20 | step 0.5 | unit cm | signed | live\n") != std::string::npos &&
                  ini.find("# ui: Standing ADS body left / right | range -30..30 | step 0.5 | unit cm | signed | live\n") != std::string::npos,
              "F178 (F181: the pilot's values): edvr.ini has both keys at 4.0 and 23.0 (were 0.0), TEMPORARY, each with its ui: row (-20..20 and -30..30 cm, step 0.5, signed, live), between the crouched left dial and the torso square");
    }
}

}  // namespace f178test
