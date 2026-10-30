// F171 checks (native on-foot VR), run171 below. Header only, run by native_frame_test.
//
// F171 (native_on_foot_f171.h, TEMPORARY): the crouched ADS body moved to the left of the aim line. experimental.native_on_foot_crouch_ads_body_left_cm (-30..+30 cm since F172, shipped 15 and 25 since F173; the checks set the dial they mean, positive left)
// moves the third-person body across the aim, by the dial x the crouched weight (F163) x the detach weight, along the horizontal left of the aim; it goes into the pin's target for the neck
// before the reach rule, so the rule keeps the left grip reachable. The rig runs the real reach rule on shoulders and grips taken from the flight edvr_gfx_20261029_043951 (the left
// shoulder almost under the eye, the left grip 62.3 cm away, just past the reach boundary), the real heading axes for the sign, the real pin move and the counters and line.
//
// Also here: the raise's carriage (native_on_foot_f171c.h): the slopes of the camera's, G's and the drawn eye's changes against the carried raise's (1, 0, -1 on synthetic pairs), the pairing
// rules, the window and its mailbox, the line within the log's limit; and F170's line reaching the log (logpin::elbow merges every counters member of the F98a window, the F98Left struct in
// native_on_foot.cpp checked against the merge in native_on_foot_logpin.h).
#pragma once

#include "../../src/d3d11/native_on_foot_core.h"
#include "../../src/d3d11/native_on_foot_f159.h"
#include "../../src/d3d11/native_on_foot_f161.h"
#include "../../src/d3d11/native_on_foot_f165.h"
#include "../../src/d3d11/native_on_foot_f166.h"
#include "../../src/d3d11/native_on_foot_f168.h"
#include "../../src/d3d11/native_on_foot_f170.h"
#include "../../src/d3d11/native_on_foot_f171.h"
#include "../../src/d3d11/native_on_foot_f171c.h"
#include "../../src/d3d11/native_on_foot_f178.h"
#include "../../src/d3d11/native_on_foot_f180.h"
#include "../../src/d3d11/native_on_foot_f181.h"
#include "../../src/d3d11/native_on_foot_logpin.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <regex>
#include <set>
#include <sstream>
#include <string>

namespace f171test {

namespace nf = edvr::nofoot;
namespace f171 = edvr::nofoot::f171;
namespace f171c = edvr::nofoot::f171c;

// The members elbow() touches, as the F98a window has them (F98Left in native_on_foot.cpp has the same ones; the source pin below reads that struct).
struct ElbowProbe {
    uint32_t n = 0, aimN = 0, leanN = 0, steps = 0, stepsOver10 = 0, src[3] = {}, spanN = 0, twistLimited = 0;
    double aimSum = 0.0, aimMin = 1e9, aimMax = -1e9, leanSum = 0.0, leanAnimSum = 0.0, yawSum = 0.0, yawMax = 0.0, planeSum = 0.0, planeMax = 0.0, planeOver45 = 0.0;
    double stepSq = 0.0, stepMax = 0.0, spanSum = 0.0, spanMax = 0.0;
    uint32_t downN = 0, downStepTwist = 0;
    double downMin = 1e9, downStepMax = -1.0, downStepAng = 0.0;
    nf::f159::Counters f159;
    nf::f161::Counters f161;
    nf::f165::Counters f165;
    nf::f166::Counters f166;
    nf::f168::Counters f168;
    nf::f170::Counters f170;
    nf::f171::Counters f171;
    nf::f178::Counters f178;   // F178: logpin::elbow merges it too
    nf::f180::Counters f180;   // F180b: and this one
    nf::f181::Counters f181;   // F181a: and this one
};

inline std::string slurp(const char* path) {
    std::ifstream f(path, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

template <class Check>
inline void run171(Check&& check) {
    const auto eq = [](double a, double b, double tol) { return std::fabs(a - b) <= tol; };
    char msg[512];

    // ---- 1. the key's range and the move ---------------------------------------------------------------------------------------------------------------------------------------------
    {
        check(f171::clampDialCm(7.5) == 7.5 && f171::clampDialCm(-35.0) == -30.0 && f171::clampDialCm(35.0) == 30.0 && f171::clampDialCm(-30.0) == -30.0 && f171::clampDialCm(30.0) == 30.0 &&
                  f171::clampDialCm(-25.0) == -25.0 && f171::clampDialCm(25.0) == 25.0 && f171::clampDialCm(std::nan("")) == 0.0 &&
                  f171::clampDialCm(std::numeric_limits<double>::infinity()) == 0.0 && f171::kDialMinCm == -30.0 && f171::kDialMaxCm == 30.0,
              "F171 (F172: the range widened): the dial is held to -30..+30 cm; a value that is not a number reads 0");
        check(f171::shiftM(0.0, 1.0, 1.0) == 0.0 && f171::shiftM(15.0, 0.0, 1.0) == 0.0 && f171::shiftM(15.0, 1.0, 0.0) == 0.0 && f171::shiftM(15.0, 0.0, 0.0) == 0.0 &&
                  f171::shiftM(-15.0, 0.0, 1.0) == 0.0 && f171::shiftM(15.0, std::nan(""), 1.0) == 0.0 && f171::shiftM(15.0, 1.0, std::nan("")) == 0.0 && f171::shiftM(std::nan(""), 1.0, 1.0) == 0.0,
              "F171: the move is exactly 0 with the dial at 0, standing (the crouched weight 0), at the hip (the detach weight 0) and for a weight or a dial that is not a number");
        check(eq(f171::shiftM(15.0, 1.0, 1.0), 0.15, 1e-15) && eq(f171::shiftM(-30.0, 1.0, 1.0), -0.30, 1e-15) && eq(f171::shiftM(25.0, 1.0, 1.0), 0.25, 1e-15) && eq(f171::shiftM(50.0, 1.0, 1.0), 0.30, 1e-15) &&
                  eq(f171::shiftM(15.0, 0.5, 0.5), 0.0375, 1e-15) && eq(f171::shiftM(15.0, 2.0, 2.0), 0.15, 1e-15) && f171::shiftM(15.0, -1.0, 1.0) == 0.0,
              "F171: the move is the dial in metres x the crouched weight x the detach weight, each held to 0..1: 15 cm crouched in the sights is 0.15 m, half and half is 3.75 cm, the dial is clamped first");
        double last = -1.0;
        bool rising = true;
        for (int i = 0; i <= 10; ++i) {
            const double w = i / 10.0;
            const double m = f171::shiftM(15.0, w, w);
            rising = rising && m >= last;
            last = m;
        }
        check(rising && eq(last, 0.15, 1e-15), "F171: through a press (the crouched and detach weights rising together) the move rises without a step from 0 to the full 15 cm");
    }

    // ---- 2. the left of the aim, and its sign against the eye's right -------------------------------------------------------------------------------------------------------------------------
    {
        // the pin reads the eye's right as headingAxes(): v.x*h.z - v.z*h.x (F69d, the shoulders from the eye in the F166, F168, F170 lines). The left of the aim must read -1 there, whatever the heading.
        bool leftEverywhere = true, perpendicular = true, unit = true;
        for (int deg = 0; deg < 360; deg += 15) {
            const double a = deg * 3.14159265358979323846 / 180.0;
            const double fwd[3] = {std::sin(a), 0.2, std::cos(a)};   // looking a little up
            double left[3];
            const bool ok = f171::leftAxis(fwd, left);
            const double h[3] = {std::sin(a), 0.0, std::cos(a)};
            double axes[3];
            nf::headingAxes(h, left, axes);
            leftEverywhere = leftEverywhere && ok && eq(axes[0], -1.0, 1e-12);
            perpendicular = perpendicular && eq(axes[2], 0.0, 1e-12) && left[1] == 0.0;
            unit = unit && eq(std::sqrt(left[0] * left[0] + left[2] * left[2]), 1.0, 1e-12);
        }
        check(leftEverywhere && perpendicular && unit, "F171: at every heading the left axis is a unit vector, perpendicular to the aim's horizontal forward, none vertical, and reads -1 along the eye's right (headingAxes), 0 forward");
        const double fwd[3] = {0.0, 0.0, 1.0};
        double left[3];
        check(f171::leftAxis(fwd, left) && left[0] == -1.0 && left[1] == 0.0 && left[2] == 0.0, "F171: facing the model's forward the left is -x");
        const double east[3] = {1.0, 0.0, 0.0};
        check(f171::leftAxis(east, left) && left[0] == -0.0 && left[2] == 1.0, "F171: facing +x (the model's right) the left is +z, a quarter turn the other way from the eye's right");
        // the shoulder's offset from the eye as the lines print it (nf::f166::offsetFromEye): a body moved 15 cm left of the aim reads 15 cm less right and nothing else
        const double eye[3] = {1.0, 1.6, 2.0}, shoulderBefore[3] = {1.0, 1.45, 1.95};
        const double aim[3] = {0.6, 0.3, 0.8};
        double shift[3];
        double before[3], after[3];
        const bool made = f171::shiftVector(15.0, 1.0, 1.0, aim, shift);
        const double shoulderAfter[3] = {shoulderBefore[0] + shift[0], shoulderBefore[1] + shift[1], shoulderBefore[2] + shift[2]};
        nf::f166::offsetFromEye(shoulderBefore, eye, aim[0], aim[2], before);
        nf::f166::offsetFromEye(shoulderAfter, eye, aim[0], aim[2], after);
        check(made && eq(after[0] - before[0], -0.15, 1e-12) && eq(after[1] - before[1], 0.0, 1e-12) && eq(after[2] - before[2], 0.0, 1e-12),
              "F171: a positive dial moves the shoulder 15 cm to the left of the eye as the F166, F168, F170 and F171 lines read it: its right offset falls by 15 cm, its up and forward offsets do not change");
        double shiftNeg[3];
        f171::shiftVector(-15.0, 1.0, 1.0, aim, shiftNeg);
        check(eq(shiftNeg[0], -shift[0], 1e-15) && eq(shiftNeg[2], -shift[2], 1e-15), "F171: a negative dial moves it to the right, the same line");
    }

    // ---- 3. the vector: along the left of the aim, never vertical ------------------------------------------------------------------------------------------------------------------------------
    {
        double out[3] = {9.0, 9.0, 9.0};
        const double fwd[3] = {0.6, 0.3, 0.8};   // 0.6, 0.8 horizontal (length 1), looking 17 deg up
        const bool ok = f171::shiftVector(15.0, 1.0, 1.0, fwd, out);
        check(ok && eq(out[0], -0.12, 1e-15) && out[1] == 0.0 && eq(out[2], 0.09, 1e-15), "F171: 15 cm left of the aim crouched in the sights is -12 cm and +9 cm along the horizontal (0.6, 0.8) aim's left (-0.8, 0.6), none vertical, whatever the aim's pitch");
        check(eq(out[0] * fwd[0] + out[2] * fwd[2], 0.0, 1e-15), "F171: and it is across the aim, none of it along it");
        const double fwd2[3] = {1.2, -0.9, 1.6};   // twice as long, looking down
        const bool ok2 = f171::shiftVector(15.0, 1.0, 1.0, fwd2, out);
        check(ok2 && eq(out[0], -0.12, 1e-15) && out[1] == 0.0 && eq(out[2], 0.09, 1e-15), "F171: the aim's length and pitch do not matter, only its horizontal direction");
        out[0] = out[1] = out[2] = 9.0;
        const bool standing = f171::shiftVector(15.0, 0.0, 1.0, fwd, out);
        const bool standingZero = out[0] == 0.0 && out[1] == 0.0 && out[2] == 0.0;
        out[0] = out[1] = out[2] = 9.0;
        const bool hip = f171::shiftVector(15.0, 1.0, 0.0, fwd, out);
        const bool hipZero = out[0] == 0.0 && out[1] == 0.0 && out[2] == 0.0;
        out[0] = out[1] = out[2] = 9.0;
        const bool dialOff = f171::shiftVector(0.0, 1.0, 1.0, fwd, out);
        const bool dialZero = out[0] == 0.0 && out[1] == 0.0 && out[2] == 0.0;
        check(!standing && standingZero && !hip && hipZero && !dialOff && dialZero, "F171: standing, at the hip and with the dial at 0 the vector is not made and is exactly zero");
        const double up[3] = {0.0, 1.0, 0.0}, nearUp[3] = {0.05, 0.99, 0.05}, bad[3] = {std::nan(""), 0.0, 1.0};
        out[0] = out[1] = out[2] = 9.0;
        const bool a = f171::shiftVector(15.0, 1.0, 1.0, up, out);
        const bool aZero = out[0] == 0.0 && out[2] == 0.0;
        const bool b = f171::shiftVector(15.0, 1.0, 1.0, nearUp, out);
        const bool c = f171::shiftVector(15.0, 1.0, 1.0, bad, out);
        const bool d = f171::shiftVector(15.0, 1.0, 1.0, nullptr, out);
        check(!a && aZero && !b && !c && !d, "F171: an aim with no horizontal forward (straight up, under 0.1 horizontal, not a number, none) moves nothing");
    }

    // ---- 4. the pin's target: the move is the body's move --------------------------------------------------------------------------------------------------------------------------------
    {
        const double neck[3] = {0.10, 0.90, 0.30}, target[3] = {0.12, 0.92, 0.35};
        const nf::PinMove base = nf::pinMove(neck, target);
        const double fwd[3] = {0.0, 0.0, 1.0};
        double shift[3];
        const bool made = f171::shiftVector(15.0, 1.0, 1.0, fwd, shift);
        double moved[3] = {target[0], target[1], target[2]};
        nf::weaponViewOffsetTarget(moved, shift);
        const nf::PinMove shifted = nf::pinMove(neck, moved);
        check(made && eq(shifted.th[0] - base.th[0], -0.15, 1e-12) && eq(shifted.th[2] - base.th[2], 0.0, 1e-12) && eq(shifted.delta - base.delta, 0.0, 1e-12) && !shifted.hClamped,
              "F171: the target shifted 15 cm left of the aim moves the body 15 cm left and no other way (no vertical move, no clamp)");
        double none[3];
        const bool madeNone = f171::shiftVector(15.0, 0.0, 1.0, fwd, none);
        double same[3] = {target[0], target[1], target[2]};
        nf::weaponViewOffsetTarget(same, none);
        const nf::PinMove unchanged = nf::pinMove(neck, same);
        check(!madeNone && std::memcmp(same, target, sizeof(same)) == 0 && unchanged.th[0] == base.th[0] && unchanged.th[1] == base.th[1] && unchanged.th[2] == base.th[2] &&
                  unchanged.delta == base.delta && unchanged.hClamped == base.hClamped && unchanged.vClamped == base.vClamped,
              "F171: standing the target and the pin's move are bit for bit what they were");
    }

    // ---- 5. the real reach rule: the move survives it, the left grip stays reached ----------------------------------------------------------------------------------------------------------
    // The flight edvr_gfx_20261029_043951: at the dial 0 the left shoulder was -0.9 right / -14.6 up / -4.0 forward cm from the eye, 62.5 cm from the left grip (the reach boundary), and the
    // right shoulder 45.9 cm from the gun's root. Here: the eye at the origin, the aim along +z; the shoulders 36 cm apart, the left one almost under the eye; the left grip on the aim line
    // 58 cm ahead (62.3 cm from the left shoulder, 2.3 cm past the 60 cm boundary), the gun's root 22 cm ahead of the right shoulder's side.
    {
        const double shoulder[2][3] = {{-0.01, -0.146, -0.04}, {0.35, -0.146, -0.04}};   // left, right (model x right, y up, z forward)
        const double grip[2][3] = {{0.0, -0.20, 0.58}, {0.0, -0.10, 0.22}};
        const double R[2] = {0.60, 0.58};
        const bool have[2] = {true, true};
        const auto reached = [&](const double d0[3], double th[3], double full[3], bool* active) {
            double v[2][3];
            for (int s = 0; s < 2; ++s)
                for (int i = 0; i < 3; ++i) v[s][i] = shoulder[s][i] + d0[i] - grip[s][i];
            double cand[3] = {}, f[3] = {};
            bool feasible = true;
            const bool act = nf::reachLimitDeltaFeasibleOnly(v, R, have, cand, f, &feasible);
            if (active) *active = act && feasible;
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
        bool active0 = false;
        reached(d0, th0, full0, &active0);
        check(active0 && dist(0, th0) <= R[0] + 1e-9 && dist(0, d0) > 0.62 && dist(0, d0) < 0.63, "F171: the rig's left shoulder is 62.3 cm from the left grip at the body's own place, past the reach boundary, so the reach rule is active at the dial 0 (the flight's 62.5)");
        double shift[3];
        const bool made = f171::shiftVector(15.0, 1.0, 1.0, fwd, shift);
        double th1[3], full1[3];
        bool active1 = false;
        reached(shift, th1, full1, &active1);
        const double reachDelta[3] = {full1[0] - full0[0], full1[1] - full0[1], full1[2] - full0[2]};
        const double written = f171::writtenLeftM(shift, reachDelta, fwd);
        const double bodyLeft = -(th1[0] - th0[0]);   // the body's real sideways change from the dial-0 body, to the left
        std::snprintf(msg, sizeof(msg),
                      "F171: with the reach rule active a +15 cm dial moves the written neck %.1f cm to the left of the aim (the real change from the dial-0 body is %.1f), the rule took back %.2f cm; the left shoulder is %.1f cm from "
                      "its grip, within the %.0f cm reach",
                      100.0 * written, 100.0 * bodyLeft, 100.0 * (0.15 - written), 100.0 * dist(0, th1), 100.0 * R[0]);
        check(made && active1 && eq(written, bodyLeft, 1e-9) && written > 0.135 && written <= 0.15 + 1e-12 && dist(0, th1) <= R[0] + 1e-9 && dist(1, th1) <= R[1] + 1e-9, msg);
        check(th1[0] < th0[0] - 0.135 && th1[1] == th0[1] && th1[2] >= th0[2] - 1e-12 && th1[2] - th0[2] < 0.03,
              "F171: the body moved left by 13.5 cm or more of the 15 and forward by what the grip needs (under 3 cm more than the dial-0 body), no vertical move");
        // the dial at 0, standing and at the hip: the vector is not made, the same call is bit for bit what it was
        double none[3];
        const bool made0 = f171::shiftVector(0.0, 1.0, 1.0, fwd, none);
        double th00[3], full00[3];
        reached(none, th00, full00, nullptr);
        const bool standing = f171::shiftVector(15.0, 0.0, 1.0, fwd, none), hip = f171::shiftVector(15.0, 1.0, 0.0, fwd, none);
        double thS[3], fullS[3];
        reached(none, thS, fullS, nullptr);
        const double zero3[3] = {0.0, 0.0, 0.0};
        check(!made0 && !standing && !hip && std::memcmp(th00, th0, sizeof(th00)) == 0 && std::memcmp(full00, full0, sizeof(full00)) == 0 && std::memcmp(thS, th0, sizeof(thS)) == 0 &&
                  std::memcmp(fullS, full0, sizeof(fullS)) == 0 && f171::writtenLeftM(none, zero3, fwd) == 0.0,
              "F171: at the dial 0, standing and at the hip nothing is added, the reach rule's correction and the written move are bit for bit what they were, and the move as written is 0");
        // half and half scales the move
        double half[3];
        f171::shiftVector(15.0, 0.5, 0.5, fwd, half);
        check(eq(half[0], -0.0375, 1e-15), "F171: half crouched and half in the sights moves it 3.75 cm");
        // a negative dial (to the right of the aim): the same machinery, the other way
        double shiftN[3];
        f171::shiftVector(-15.0, 1.0, 1.0, fwd, shiftN);
        double thN[3], fullN[3];
        reached(shiftN, thN, fullN, nullptr);
        const double deltaN[3] = {fullN[0] - full0[0], fullN[1] - full0[1], fullN[2] - full0[2]};
        const double writtenN = f171::writtenLeftM(shiftN, deltaN, fwd);
        check(writtenN < -0.135 && eq(writtenN, -(thN[0] - th0[0]), 1e-9) && dist(0, thN) <= R[0] + 1e-9, "F171: a -15 cm dial moves the written neck 13.5+ cm to the right of the aim and the left grip is still reached");
        // F172: the widened ends. +30 to the left writes 25.8 cm with the left grip still reached; -30 to the right is stopped by the right arm's own reach (the right shoulder is then 58.0 cm
        // from the gun's root, its limit): the rule gives 11.2 cm of it back and both arms are within reach.
        {
            double s30[3], th30[3], full30[3], sm30[3], thm30[3], fullm30[3];
            f171::shiftVector(30.0, 1.0, 1.0, fwd, s30);
            reached(s30, th30, full30, nullptr);
            const double d30[3] = {full30[0] - full0[0], full30[1] - full0[1], full30[2] - full0[2]};
            const double w30 = f171::writtenLeftM(s30, d30, fwd);
            f171::shiftVector(-30.0, 1.0, 1.0, fwd, sm30);
            reached(sm30, thm30, fullm30, nullptr);
            const double dm30[3] = {fullm30[0] - full0[0], fullm30[1] - full0[1], fullm30[2] - full0[2]};
            const double wm30 = f171::writtenLeftM(sm30, dm30, fwd);
            std::snprintf(msg, sizeof(msg), "F172: at the new ends the reach rule keeps both arms within reach: +30 cm writes %.2f cm to the left (left shoulder %.1f cm from its grip), -30 cm writes %.2f cm to the right (right shoulder %.1f cm from the gun's root)",
                          100.0 * w30, 100.0 * dist(0, th30), -100.0 * wm30, 100.0 * dist(1, thm30));
            check(eq(s30[0], -0.30, 1e-15) && w30 > 0.25 && w30 < 0.27 && dist(0, th30) <= R[0] + 1e-9 && dist(1, th30) <= R[1] + 1e-9 && wm30 < -0.17 && wm30 > -0.20 &&
                      dist(0, thm30) <= R[0] + 1e-9 && dist(1, thm30) <= R[1] + 1e-9, msg);
        }
        // the move as written with a reach rule that gives half back (the arm is at its reach on the sideways line): the figure is the real change
        const double halfBack[3] = {0.5 * 0.15, 0.0, 0.0};   // the correction's change, model x: back toward the aim line by half
        check(eq(f171::writtenLeftM(shift, halfBack, fwd), 0.075, 1e-12), "F171: a reach rule that gives half of the move back leaves 7.5 cm as written");
        // no horizontal forward: the move as written is not measured
        const double up[3] = {0.0, 1.0, 0.0};
        check(std::isnan(f171::writtenLeftM(shift, reachDelta, up)) && std::isnan(f171::writtenLeftM(shift, reachDelta, nullptr)), "F171: aiming straight up has no left to write the move along");
    }

    // ---- 6. the counters, the line ---------------------------------------------------------------------------------------------------------------------------------------------------------
    {
        f171::Counters c;
        const auto frame = [&](bool moving, bool made, double dial, double asked, double written, double gun, double grip, double rr, double ru, double rf, double lr, double lu, double lf) {
            f171::Sample s;
            s.moving = moving;
            s.made = made;
            s.dialCm = dial;
            s.askedM = asked;
            s.writtenM = written;
            s.rightShoulderToGunM = gun;
            s.leftShoulderToGripM = grip;
            s.rightShoulderM[0] = rr; s.rightShoulderM[1] = ru; s.rightShoulderM[2] = rf;
            s.leftShoulderM[0] = lr; s.leftShoulderM[1] = lu; s.leftShoulderM[2] = lf;
            c.add(s);
        };
        for (int i = 0; i < 40; ++i) frame(false, true, 15.0, 0.15, 0.144, 0.45, 0.60, 0.17, -0.146, -0.04, -0.16, -0.146, -0.04);   // still
        for (int i = 0; i < 60; ++i) frame(true, true, 15.0, 0.15, 0.140, 0.44, 0.61, 0.18, -0.134, -0.05, -0.15, -0.134, -0.05);   // moving
        for (int i = 0; i < 10; ++i) frame(true, false, 15.0, 0.0, std::nan(""), std::nan(""), std::nan(""), std::nan(""), 0.0, 0.0, std::nan(""), 0.0, 0.0);   // moving, the move not made, nothing measured
        check(c.ads == 110 && c.still == 40 && c.moving == 70 && c.made == 100 && c.askedN[0] == 40 && c.askedN[1] == 70 && c.writtenN[0] == 40 && c.writtenN[1] == 60 && c.backN[1] == 60 &&
                  c.gunN[1] == 60 && c.gripN[0] == 40 && c.leftN[0] == 40 && c.rightN[1] == 60 && eq(c.dialSum, 1650.0, 1e-9) && c.dialAbsMax == 15.0 && eq(c.askedSum[0] / c.askedN[0], 0.15, 1e-12) &&
                  eq(c.backSum[0] / c.backN[0], 0.006, 1e-12) && eq(c.writtenSum[1] / c.writtenN[1], 0.14, 1e-12),
              "F171: the counters keep the frames still against moving, the frames the move was made in, each figure's mean over the frames it was measured on, what the reach rule took back, the dial's sum and largest");
        f171::Counters m;
        m.merge(c);
        m.merge(c);
        check(m.ads == 220 && m.moving == 140 && m.made == 200 && m.gunN[1] == 120 && m.backN[1] == 120 && eq(m.gunSum[1], 2.0 * c.gunSum[1], 1e-9) && eq(m.dialSum, 3300.0, 1e-9) && m.dialAbsMax == 15.0 &&
                  m.leftN[0] == 80 && m.rightN[1] == 120 && eq(m.rightSum[1][0], 2.0 * c.rightSum[1][0], 1e-9),
              "F171: two windows add up");
        char line[1400];
        const int len = f171::formatSummary(line, sizeof(line), 10, "$wpn_m_shotgun_plasma_doublebarrel_name;", c);
        // every figure at what a 10 s window can hold: 900 frames, the dial and the moves at their limits (20 cm), the shoulders 1 m from the eye, the distances 1 m
        f171::Counters big;
        big.ads = 900; big.still = big.moving = 450; big.made = 900;
        for (int i = 0; i < 2; ++i) {
            big.askedN[i] = big.writtenN[i] = big.backN[i] = big.leftN[i] = big.rightN[i] = big.gunN[i] = big.gripN[i] = 450;
            big.askedSum[i] = -0.20 * 450.0; big.writtenSum[i] = -0.20 * 450.0; big.backSum[i] = -0.20 * 450.0; big.gunSum[i] = 1.0 * 450.0; big.gripSum[i] = 1.0 * 450.0;
            for (int k = 0; k < 3; ++k) { big.leftSum[i][k] = -1.0 * 450.0; big.rightSum[i][k] = -1.0 * 450.0; }
        }
        big.dialSum = -20.0 * 900.0; big.dialAbsMax = 20.0;
        char worst[1400];
        const int worstLen = f171::formatSummary(worst, sizeof(worst), 10, "$wpn_m_shotgun_plasma_doublebarrel_name_with_a_long_tail;", big);
        // and counts and sums no window could hold: it ends inside the buffer, never past it
        f171::Counters absurd = big;
        absurd.ads = absurd.still = absurd.moving = absurd.made = 4000000000u;
        absurd.dialSum = -1.0e12;
        for (int i = 0; i < 2; ++i) {
            absurd.askedN[i] = absurd.writtenN[i] = absurd.backN[i] = absurd.leftN[i] = absurd.rightN[i] = absurd.gunN[i] = absurd.gripN[i] = 4000000000u;
            absurd.askedSum[i] = -1.0e12; absurd.gunSum[i] = 1.0e12;
            for (int k = 0; k < 3; ++k) { absurd.leftSum[i][k] = -1.0e13; absurd.rightSum[i][k] = -1.0e13; }
        }
        char absurdLine[1200];
        const int absurdLen = f171::formatSummary(absurdLine, sizeof(absurdLine), 4000000000u, "$wpn_m_shotgun_plasma_doublebarrel_name_with_a_long_tail;", absurd);
        std::snprintf(msg, sizeof(msg), "F171: the window's line names the tag, the key and its value, the frames, the move asked, as written and taken back, within the log's 1166 bytes (1200 less its stamp and truncation marker) even with every figure at its worst (%d bytes typical, %d worst)", len, worstLen);
        check(len > 0 && len < 1100 && worstLen > 0 && worstLen < 1166 && absurdLen > 0 && std::strlen(absurdLine) < sizeof(absurdLine) &&
                  std::strstr(line, "(F171, TEMPORARY) 10 s: item $wpn_m_shotgun_plasma_doublebarrel_name;") != nullptr &&
                  std::strstr(line, "the dial experimental.native_on_foot_crouch_ads_body_left_cm mean +15.0 cm (largest 15.0; positive is left of the aim)") != nullptr &&
                  std::strstr(line, "crouched in the sights 110 frames (still 40, moving 70), the move made in 100") != nullptr &&
                  std::strstr(line, "the body moved left (the dial x the crouched weight x the detach weight) +15.0 cm still, +12.9 moving") != nullptr, msg);
        check(std::strstr(line, "as written (the neck's sideways change from the dial-0 body, after the reach rule; 40/60 frames) +14.4 still, +14.0 moving") != nullptr &&
                  std::strstr(line, "the reach rule took back +0.6 still, +1.0 moving") != nullptr,
              "F171: the move as written is a mean over the frames it was measured on beside the move asked, and what the reach rule took back is the difference");
        check(std::strstr(line, "right shoulder from the eye (right/up/forward, cm): still 40 frames +17.0/-14.6/-4.0, moving 60 frames +18.0/-13.4/-5.0") != nullptr &&
                  std::strstr(line, "left shoulder from the eye: still 40 frames -16.0/-14.6/-4.0, moving 60 frames -15.0/-13.4/-5.0") != nullptr &&
                  std::strstr(line, "right shoulder to the gun's root 40 frames still 45.0 cm, 60 moving 44.0; left shoulder to the left-hand grip 40 frames still 60.0 cm, 60 moving 61.0") != nullptr,
              "F171: both shoulders from the eye, the right shoulder to the gun's root and the left shoulder to the left grip are printed in cm, still against moving, with the frames each is a mean of");
        f171::Counters empty;
        char noneLine[1400];
        const int noneLen = f171::formatSummary(noneLine, sizeof(noneLine), 1, nullptr, empty);
        check(noneLen > 0 && std::strstr(noneLine, "item none") != nullptr && std::strstr(noneLine, "crouched in the sights 0 frames") != nullptr && std::strstr(noneLine, "nan") == nullptr &&
                  std::strstr(noneLine, "inf") == nullptr,
              "F171: a window with no frame prints zeros, never a number that is not one");
        // what the log shows if the move never ran: the made count is 0 and the asked figure reads 0 while the dial reads 15
        f171::Counters idle;
        for (int i = 0; i < 50; ++i) {
            f171::Sample s;
            s.dialCm = 15.0;
            s.made = false;
            s.askedM = f171::shiftM(15.0, 1.0, 0.0);   // the sights not up: the move is 0
            s.writtenM = 0.0;
            idle.add(s);
        }
        char idleLine[1400];
        f171::formatSummary(idleLine, sizeof(idleLine), 1, "x", idle);
        check(std::strstr(idleLine, "mean +15.0 cm") != nullptr && std::strstr(idleLine, "the move made in 0") != nullptr && std::strstr(idleLine, "+0.0 cm still, +0.0 moving") != nullptr,
              "F171: a dial of 15 with the move never made reads 'the move made in 0' and 0 cm asked: not the same as a move that was made and written");
    }

    // ---- 7. the raise's carriage: the slopes, the pairs, the window ----------------------------------------------------------------------------------------------------------------------------
    {
        check(f171c::bandOf(0.0) == 0 && f171c::bandOf(9.99) == 0 && f171c::bandOf(-9.99) == 0 && f171c::bandOf(10.0) == 1 && f171c::bandOf(39.99) == 1 && f171c::bandOf(-25.0) == 1 && f171c::bandOf(40.0) == 2 &&
                  f171c::bandOf(-85.0) == 2 && f171c::bandOf(std::nan("")) == -1,
              "F171: the bands of |aim pitch| are under 10 deg, 10 to 40 and 40 and over; a pitch that is not a number has none");
        // a run of 20 calls whose carried raise rises 4 mm a call; make(f) builds each call's other figures from the raise
        const auto run = [](auto&& fig, int mode, int band, double stepM, int calls, f171c::Window* w) {
            f171c::Pairing p;
            for (int i = 0; i < calls; ++i) {
                f171c::Sample s;
                s.have = true;
                s.p1 = 1000u + static_cast<uint32_t>(i);
                s.generation = 7;
                s.mode = mode;
                s.band = band;
                const double raise = -0.30 + stepM * i;
                s.raiseM = raise;
                fig(raise, s);
                p.step(s, *w);
            }
        };
        // the camera carries the raise, G is clear of it: k_up 1, c 0
        f171c::Window carries;
        run([](double raise, f171c::Sample& s) { s.camUpM = 1.50 + raise; s.camFwdM = 0.05; s.gM = 0.16; s.eyeUpM = 0.08 + raise; }, 0, 2, 0.004, 20, &carries);
        const f171c::Cell& cellCarries = carries.cell[0][2];
        check(cellCarries.n == 19 && eq(cellCarries.kUp(), 1.0, 1e-12) && eq(cellCarries.kFwd(), 0.0, 1e-12) && eq(cellCarries.c(), 0.0, 1e-12) && eq(cellCarries.kEye(), 1.0, 1e-12),
              "F171: a camera that carries the raise and a G clear of it read k_up 1, k_fwd 0, c 0, k_eye 1 over 19 pairs of 20 calls");
        check(eq(cellCarries.raiseMin, -0.30, 1e-12) && eq(cellCarries.raiseMax, -0.30 + 0.004 * 19, 1e-12), "F171: the carried raise's range is that of the calls in the pairs");
        // the camera does not carry it but it is removed anyway: k_up 0, c -1
        f171c::Window absent;
        run([](double raise, f171c::Sample& s) { s.camUpM = 1.50; s.camFwdM = 0.05; s.gM = 0.16 - raise; s.eyeUpM = 0.08; }, 1, 2, 0.004, 20, &absent);
        const f171c::Cell& cellAbsent = absent.cell[1][2];
        check(cellAbsent.n == 19 && eq(cellAbsent.kUp(), 0.0, 1e-12) && eq(cellAbsent.c(), -1.0, 1e-12) && eq(cellAbsent.kEye(), 0.0, 1e-12) && absent.cell[0][2].n == 0,
              "F171: a camera that does not carry the raise but has it taken out anyway reads k_up 0, c -1, k_eye 0 (in the look to aim cell)");
        // the camera moves against the raise one to one: -1; forward follows half
        f171c::Window against;
        run([](double raise, f171c::Sample& s) { s.camUpM = 1.50 - raise; s.camFwdM = 0.05 + 0.5 * raise; s.gM = 0.16 - 2.0 * raise; s.eyeUpM = 0.08 - raise; }, 0, 1, -0.004, 20, &against);
        const f171c::Cell& cellAgainst = against.cell[0][1];
        check(cellAgainst.n == 19 && eq(cellAgainst.kUp(), -1.0, 1e-12) && eq(cellAgainst.kFwd(), 0.5, 1e-12) && eq(cellAgainst.c(), -2.0, 1e-12),
              "F171: a camera that moves against the raise reads k_up -1; a forward that follows it by half reads k_fwd 0.5");
        // constant offsets and noise-free steps of any size: the slope is of the changes, through the origin
        f171c::Window offset;
        run([](double raise, f171c::Sample& s) { s.camUpM = 9.0 + 0.8 * raise; s.camFwdM = -3.0; s.gM = 5.0; s.eyeUpM = 0.0 + 0.8 * raise; }, 0, 0, 0.01, 6, &offset);
        check(eq(offset.cell[0][0].kUp(), 0.8, 1e-12) && eq(offset.cell[0][0].kEye(), 0.8, 1e-12), "F171: constant offsets are out of the slope: it is of the changes");

        // the pairing rules
        f171c::Window w;
        f171c::Pairing p;
        const auto sample = [](uint32_t p1, double raise, int mode = 0, int band = 2, uint32_t gen = 7) {
            f171c::Sample s;
            s.have = true;
            s.p1 = p1;
            s.generation = gen;
            s.mode = mode;
            s.band = band;
            s.raiseM = raise;
            s.camUpM = raise;
            s.camFwdM = 0.0;
            s.gM = 0.0;
            s.eyeUpM = raise;
            return s;
        };
        check(!p.step(sample(10, 0.000), w) && w.pairs() == 0, "F171: the first call has nothing to pair with");
        check(p.step(sample(11, 0.003), w) && w.pairs() == 1, "F171: the next game frame with the raise 3 mm different is a pair");
        check(!p.step(sample(12, 0.0041), w) && w.pairs() == 1, "F171: a raise that changed 1.1 mm is not (under 2 mm)");
        check(p.step(sample(14, 0.0075), w) && w.pairs() == 2, "F171: a call two game frames later (one dropped between) still pairs; 3.4 mm");
        check(!p.step(sample(17, 0.0120), w) && w.pairs() == 2, "F171: three game frames later it does not");
        check(!p.step(sample(17, 0.0200), w) && w.pairs() == 2, "F171: the same game frame again does not");
        check(!p.step(sample(16, 0.0300), w) && w.pairs() == 2, "F171: a game frame that went backwards does not");
        check(p.step(sample(17, 0.0400), w) && w.pairs() == 3, "F171: and the run goes on from the call before the refused one");
        const bool modeFirst = p.step(sample(18, 0.0500, 1), w);
        const bool modeSecond = p.step(sample(19, 0.0600, 1), w);
        check(!modeFirst && modeSecond && w.pairs() == 4 && w.cell[1][2].n == 1,
              "F171: a call in another look mode does not pair with the one before it; the next of that mode does, in its own cell");
        check(!p.step(sample(20, 0.0700, 1, 1), w) && p.step(sample(21, 0.0800, 1, 1), w) && w.cell[1][1].n == 1 && w.pairs() == 5, "F171: another band does not pair either; the next call in it does");
        check(!p.step(sample(22, 0.0900, 1, 1, 8), w) && w.pairs() == 5, "F171: another LOCAL generation does not (G contains the headset's own height, measured in another frame)");
        f171c::Sample bad = sample(23, 0.1000, 1, 1, 8);
        bad.gM = std::nan("");
        check(!p.step(bad, w) && !p.step(sample(24, 0.1100, 1, 1, 8), w), "F171: a call with a figure that is not a number is no sample, and the run starts over after it");
        f171c::Sample none;
        check(!none.valid() && !p.step(none, w), "F171: a call that is not a sample ends the run");
        f171c::Sample modeBad = sample(30, 0.0);
        modeBad.mode = 2;
        f171c::Sample bandBad = sample(31, 0.0);
        bandBad.band = -1;
        check(!modeBad.valid() && !bandBad.valid(), "F171: a mode or a band out of range is no sample");
        p.reset();
        check(!p.step(sample(40, 0.0), w) && !p.step(sample(41, 0.0), w), "F171: a reset (the on-foot camera's edge) forgets the call before it");

        // the window, the mailbox
        f171c::Mailbox<f171c::Window> box;
        const auto write = [&](int n) {
            box.write([&](f171c::Window& win) {
                for (int i = 0; i < n; ++i) win.cell[0][2].add(sample(1, 0.0), sample(2, 0.01));
            });
        };
        f171c::Window got;
        check(box.take(&got) && !got.any(), "F171: a take before any write reads an empty window");
        write(29);
        check(box.take(&got) && got.cell[0][2].n == 29 && got.any(), "F171: a take after 29 pairs reads 29");
        int printed = 0;
        for (int s = 0; s < 20; ++s) printed += box.take(&got) && got.any() ? 1 : 0;
        check(printed == 0, "F171: twenty quiet seconds print nothing (the old window is not read again)");
        write(1);
        check(box.take(&got) && got.cell[0][2].n == 1 && box.take(&got) && !got.any(), "F171: the next pair reads alone, and the second after it reads empty");

        // the line
        f171c::Window lw;
        for (int i = 0; i < 37; ++i) {
            f171c::Sample s0 = sample(1, -0.50), s1 = sample(2, -0.1234);
            s0.camUpM = 1.5; s1.camUpM = 1.5 + 0.02 * (s1.raiseM - s0.raiseM); s0.gM = 0.16; s1.gM = 0.16 - 0.98 * (s1.raiseM - s0.raiseM);
            s0.camFwdM = 0.0; s1.camFwdM = -0.05 * (s1.raiseM - s0.raiseM); s0.eyeUpM = 0.0; s1.eyeUpM = 0.01 * (s1.raiseM - s0.raiseM);
            lw.cell[0][2].add(s0, s1);
        }
        for (int i = 0; i < 90; ++i) {
            f171c::Sample s = sample(1, 0.0);
            s.f137UpM = -0.05;
            s.f137FwdM = 0.02;
            lw.addSample(s);
        }
        char text[1400];
        const int tl = f171c::formatSummary(text, sizeof(text), 1, lw);
        check(tl > 0 && std::strstr(text, "native on foot raise carriage (F171) 1 s:") != nullptr &&
                  std::strstr(text, "mouse look 40+ deg: 37 pairs, raise -50.0..-12.3 cm, k_up +0.02, k_fwd -0.05, c -0.98, k_eye +0.01") != nullptr &&
                  std::strstr(text, "samples 90, F137 carried up -5.0 forward +2.0 cm (mean)") != nullptr && lw.samples == 90,
              "F171: the carriage line names the look mode, the band, the pairs, the carried raise's range and the four slopes, and the samples with the virtual neck they carried, as the log reads them");
        // a second with samples and no pair is not a line (the window has samples, not pairs): the code ran, there was nothing to pair
        f171c::Window samplesOnly;
        samplesOnly.addSample(sample(1, 0.0));
        check(samplesOnly.samples == 1 && !samplesOnly.any() && samplesOnly.pairs() == 0, "F171: a window with samples and no pair counts its samples and has no line to print");
        // all six cells at what a second can hold (90 calls a second: under 90 pairs a cell; the raise within the +-50 cm clamp; slopes of any size up to 9.99 either way)
        f171c::Window all;
        for (int m = 0; m < 2; ++m)
            for (int b = 0; b < 3; ++b) {
                f171c::Cell& cell = all.cell[m][b];
                cell.n = 89;
                cell.sxx = 1.0; cell.sxyUp = -9.99; cell.sxyFwd = 9.99; cell.sxyG = -9.99; cell.sxyEye = 9.99;
                cell.raiseMin = -0.50; cell.raiseMax = 0.50;
            }
        all.samples = 90;
        all.f137UpSum = -0.50 * 90.0;
        all.f137FwdSum = -0.50 * 90.0;
        char worstText[1400];
        const int wl = f171c::formatSummary(worstText, sizeof(worstText), 1, all);
        std::snprintf(msg, sizeof(msg), "F171: the carriage line is within the log's 1166 bytes (1200 less its stamp and truncation marker) with all six cells at their worst (%d bytes; one cell %d)", wl, tl);
        check(wl > 0 && wl < 1166 && std::strstr(worstText, "look to aim 40+ deg") != nullptr && std::strstr(worstText, "mouse look under 10 deg") != nullptr && std::strstr(worstText, "10-40 deg") != nullptr, msg);
        // and a window of figures no second could hold still ends inside the buffer, never past it
        f171c::Window absurd;
        for (int m = 0; m < 2; ++m)
            for (int b = 0; b < 3; ++b) {
                f171c::Cell& cell = absurd.cell[m][b];
                cell.n = 4000000000u;
                cell.sxx = 1.0; cell.sxyUp = -12345.678; cell.sxyFwd = 12345.678; cell.sxyG = -12345.678; cell.sxyEye = 12345.678;
                cell.raiseMin = -123.456; cell.raiseMax = 123.456;
            }
        char absurdText[1200];
        const int al = f171c::formatSummary(absurdText, sizeof(absurdText), 4000000000u, absurd);
        check(al > 0 && std::strlen(absurdText) < sizeof(absurdText), "F171: a window of figures no second could hold ends inside the line buffer");
        f171c::Window nothing;
        char emptyText[1400];
        f171c::cellsText(nothing, emptyText, sizeof(emptyText));
        check(!nothing.any() && emptyText[0] == '\0', "F171: a window with no pair has no cell text (the line is not printed)");
    }

    // ---- 8. F170's line reaches the log: the window built through logpin::elbow carries every counters member -------------------------------------------------------------------------------------
    {
        ElbowProbe second;
        second.n = 90;
        for (int i = 0; i < 30; ++i) {
            nf::f159::Sample s159;
            second.f159.add(s159);
            nf::f161::Sample s161;
            second.f161.add(s161);
            nf::f165::Sample s165;
            second.f165.add(s165);
            nf::f166::Sample s166;
            s166.crouchedAds = true;
            second.f166.add(s166);
            nf::f168::Sample s168;
            second.f168.add(s168);
            nf::f170::Sample s170;
            s170.dial = 1.0;
            second.f170.add(s170);
            f171::Sample s171;
            s171.dialCm = 15.0;
            s171.made = true;
            second.f171.add(s171);
        }
        ElbowProbe window;
        nf::logpin::elbow(window, second);
        nf::logpin::elbow(window, second);
        check(window.n == 180 && window.f159.n == 60 && window.f161.frames() == 60 && window.f165.n == 60 && window.f166.ads == 60 && window.f168.ads == 60 && window.f170.ads == 60 && window.f171.ads == 60 &&
                  window.f170.dialMax == 1.0 && window.f171.made == 60 && window.f171.dialAbsMax == 15.0,
              "F171: a window built through logpin::elbow carries the counts of every counters member of the F98a second (F159, F161, F165, F166, F168, F170 and F171): F170's line printed nothing because its merge was missing");
        check(window.f170.ads != 0 && window.f171.ads != 0, "F171: so the print rules (f98.f170.ads, f98.f171.ads) see frames");

        // the source pin: every `nofoot::fNNN::Counters fNNN;` member of F98Left in native_on_foot.cpp is merged by elbow() in native_on_foot_logpin.h
        const std::string cpp = slurp("src/d3d11/native_on_foot.cpp"), logpin = slurp("src/d3d11/native_on_foot_logpin.h");
        check(!cpp.empty() && !logpin.empty(), "F171: the source pin reads src/d3d11/native_on_foot.cpp and native_on_foot_logpin.h (run the rig from the repository root)");
        const size_t at = cpp.find("struct F98Left {");
        const size_t end = at == std::string::npos ? std::string::npos : cpp.find("\n};", at);
        std::set<std::string> members, unmerged;
        if (at != std::string::npos && end != std::string::npos) {
            const std::string body = cpp.substr(at, end - at);
            const std::regex member(R"(nofoot::(f\d+[a-z]?)::Counters\s+(f\d+[a-z]?)\s*;)");
            for (std::sregex_iterator it(body.begin(), body.end(), member), stop; it != stop; ++it) {
                const std::string name = (*it)[2].str();
                members.insert(name);
                if (logpin.find("d." + name + ".merge(s." + name + ")") == std::string::npos) unmerged.insert(name);
            }
        }
        std::string missing;
        for (const std::string& name : unmerged) missing += (missing.empty() ? "" : ", ") + name;
        std::snprintf(msg, sizeof(msg), "F171: every counters member of F98Left (%u found) is merged by logpin::elbow; not merged: %s", static_cast<unsigned>(members.size()), missing.empty() ? "none" : missing.c_str());
        check(members.size() >= 7 && members.count("f159") && members.count("f170") && members.count("f171") && unmerged.empty(), msg);
    }
}

}  // namespace f171test
