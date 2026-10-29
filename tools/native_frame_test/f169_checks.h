// F169 checks (native on-foot VR), run169 below. Header only, run by native_frame_test.
//
// F169 (native_on_foot_f169.h): the stance while the aim is pitched (w_c capped under the animated neck's posture weight while |the aim's pitch| is at F138's gate or more), and the pistol
// sprint's arm swing: the left arm's animated pose read a fraction of the measured stride period back (experimental.native_on_foot_pistol_sprint_swing_phase, TEMPORARY), the stride's
// period from the animated left hand's forward swing, the measure of how the two arms swing and its line. The part of F169 that moves a positive crouched-body dial past the reach rule
// is in f168_checks.h (section 5).
#pragma once

#include "../../src/d3d11/native_on_foot_core.h"
#include "../../src/d3d11/native_on_foot_f94.h"
#include "../../src/d3d11/native_on_foot_f163.h"
#include "../../src/d3d11/native_on_foot_f166.h"
#include "../../src/d3d11/native_on_foot_f169.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <vector>

namespace f169test {

namespace nf = edvr::nofoot;
namespace f169 = edvr::nofoot::f169;
namespace f166 = edvr::nofoot::f166;
namespace f94 = edvr::nofoot::f94;
namespace f163 = edvr::nofoot::f163;

template <class Check>
inline void run169(Check&& check) {
    const auto eq = [](double a, double b, double tol) { return std::fabs(a - b) <= tol; };
    constexpr double kPi = 3.14159265358979323846;

    // ======================================================== B: the stance while the aim is pitched ========================================================
    // ---- 1. the gate and the cap -------------------------------------------------------------------------------------------------------------------------------------------------------
    {
        check(!f169::pitchedPast(19.9) && f169::pitchedPast(20.0) && f169::pitchedPast(85.0) && f169::pitchedPast(-85.0) && f169::pitchedPast(-20.0) && !f169::pitchedPast(-19.9) &&
                  !f169::pitchedPast(0.0) && !f169::pitchedPast(std::nan("")) && !f169::pitchedPast(std::numeric_limits<double>::infinity()) && f169::kPitchedGateDeg == nf::f138::kSettleGateDeg,
              "F169: the aim is pitched from F138's 20 deg either way; level, not a number and infinite are not");
        check(f169::wcCap(false, 0.0) == 1.0 && f169::wcCap(false, 1.0) == 1.0 && f169::wcCap(true, 0.0) == 0.0 && f169::wcCap(true, 0.7) == 0.7 && f169::wcCap(true, 1.0) == 1.0 &&
                  f169::wcCap(true, std::nan("")) == 0.0 && f169::wcCap(true, 3.0) == 1.0 && f169::wcCap(true, -1.0) == 0.0,
              "F169: the cap on w_c is 1 while the aim is level and the posture weight while it is pitched (0 for a posture that is not a number)");
        check(f169::wcUsed(0.64, 1.0) == 0.64 && f169::wcUsed(0.0, 1.0) == 0.0 && f169::wcUsed(1.0, 1.0) == 1.0 && f169::wcUsed(0.64, 0.0) == 0.0 && f169::wcUsed(0.64, 0.3) == 0.3 && f169::wcUsed(0.2, 0.3) == 0.2,
              "F169: w_c held under the cap is w_c itself, bit for bit, at a cap of 1, and never above the cap");
    }

    // ---- 2. the neck target: level it is what it was, pitched the sunk camera is no crouch ----------------------------------------------------------------------------------------------
    {
        const double n0s[3] = {0.0, 1.45, 0.0}, p44s[3] = {0.0, 1.58, 0.03};
        double rowsId[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
        const double fwd0[3] = {0.0, 0.0, 1.0};
        nf::PinEyeCal cal;
        for (int i = 0; i < 45; ++i) cal.add(true, p44s, rowsId, n0s, 1.62);
        const double pStance[3] = {p44s[0] + 0.053, p44s[1] + (0.066 - 0.155), p44s[2] + (0.154 - 0.09)};
        const double eyeSunk[3] = {0.0, 1.62 - 0.35, 0.0};   // the camera 35 cm under its standing mean: w_c 0.65, the log's 0.54 to 0.65
        double tFree[3], tDefault[3], tOne[3], tCap0[3], tCap03[3];
        nf::PinAnimTarget pFree, pDefault, pOne, pCap0, pCap03;
        nf::pinNeckTargetAnim(eyeSunk, fwd0, pStance, rowsId, n0s, cal, tFree, &pFree);
        nf::pinNeckTargetAnim(eyeSunk, fwd0, pStance, rowsId, n0s, cal, tDefault, &pDefault, 0.0, false, nullptr);
        nf::pinNeckTargetAnim(eyeSunk, fwd0, pStance, rowsId, n0s, cal, tOne, &pOne, 0.0, false, nullptr, 1.0);
        nf::pinNeckTargetAnim(eyeSunk, fwd0, pStance, rowsId, n0s, cal, tCap0, &pCap0, 0.0, false, nullptr, 0.0);
        nf::pinNeckTargetAnim(eyeSunk, fwd0, pStance, rowsId, n0s, cal, tCap03, &pCap03, 0.0, false, nullptr, 0.3);
        double rest[3];
        nf::pinNeckTarget(eyeSunk, fwd0, rest);
        check(pFree.wc > 0.5 && pFree.wc < 0.7 && std::memcmp(tFree, tOne, sizeof(tFree)) == 0 && std::memcmp(tFree, tDefault, sizeof(tFree)) == 0 && pFree.wc == pOne.wc && pFree.wc == pDefault.wc &&
                  pOne.wcUncapped == pOne.wc,
              "F169: with the camera 35 cm down and the aim level (a cap of 1) w_c and the neck target are what they were, bit for bit");
        const double dCap0 = std::sqrt((tCap0[0] - rest[0]) * (tCap0[0] - rest[0]) + (tCap0[1] - rest[1]) * (tCap0[1] - rest[1]) + (tCap0[2] - rest[2]) * (tCap0[2] - rest[2]));
        const double dFree = std::sqrt((tFree[0] - rest[0]) * (tFree[0] - rest[0]) + (tFree[1] - rest[1]) * (tFree[1] - rest[1]) + (tFree[2] - rest[2]) * (tFree[2] - rest[2]));
        check(pCap0.wc == 0.0 && pCap0.wcUncapped == pFree.wc && dCap0 < 1e-9 && dFree > 0.04 && pCap03.wc == 0.3 && pCap03.wcUncapped == pFree.wc,
              "F169: pitched with the animated neck standing (a cap of 0) the sunk camera gives w_c 0 and the REST neck-to-eye vector, where it gave a target 4 cm or more away; a cap of 0.3 holds w_c at 0.3");
        // the caller's cap of NaN leaves w_c alone
        double tNan[3];
        nf::PinAnimTarget pNan;
        nf::pinNeckTargetAnim(eyeSunk, fwd0, pStance, rowsId, n0s, cal, tNan, &pNan, 0.0, false, nullptr, std::nan(""));
        check(pNan.wc == pFree.wc, "F169: a cap that is not a number leaves w_c alone");
    }

    // ---- 3. the replays: a look-up standing, a look-up crouched, an unpitched crouch --------------------------------------------------------------------------------------------------------
    {
        // The call series of the flight: 90 calls a second. h_stand 1.60; the standing neck 1.543 stored (F98c); the item drawn (the armed target takes the stance's class). Each replay runs
        // the pin's own chain: the camera's weight (pinCrouchWeight), the posture weight (the animated neck's drop), the cap, w_c used, the class, F98c's raise target (f94::EyeLevel).
        struct Step { double t, camY, neckY, aimPitch; };
        struct Result {
            int calls = 0, flipsUsed = 0, flipsRaw = 0, notStance = 0, notStanceRaw = 0, crouchedUsed = 0, crouchedRaw = 0;
            double firstCrouchedUsedAt = -1.0, firstStandingUsedAt = -1.0;
            double targetMin = 1e9, targetMax = -1e9;
            double wcRawMax = 0.0;
            bool usedEqualsRaw = true;
        };
        const double hStand = 1.60, standNeck = 1.543, eyeHeight = 0.15;
        const auto replay = [&](const std::vector<Step>& steps) {
            Result r;
            f94::EyeLevel level, levelRaw;
            level.have[f94::kClassStanding] = levelRaw.have[f94::kClassStanding] = true;
            level.neckY[f94::kClassStanding] = levelRaw.neckY[f94::kClassStanding] = standNeck;
            level.camY[f94::kClassStanding] = levelRaw.camY[f94::kClassStanding] = hStand;
            bool havePrev = false, prevUsed = false, prevRaw = false;
            for (const Step& s : steps) {
                const double wcRaw = nf::pinCrouchWeight(true, hStand, s.camY);
                const double posture = f163::postureWeight(f163::neckDrop(true, standNeck, s.neckY));
                const double cap = f169::wcCap(f169::pitchedPast(s.aimPitch), posture);
                const double wc = f169::wcUsed(wcRaw, cap);
                const bool used = nf::pinCrouchedWc(wc), raw = nf::pinCrouchedWc(wcRaw);
                r.usedEqualsRaw = r.usedEqualsRaw && wc == wcRaw;
                uint8_t src = 0, srcRaw = 0;
                const double target = level.target(true, used ? f94::kClassCrouched : f94::kClassStanding, s.neckY, s.camY, eyeHeight, &src);
                levelRaw.target(true, raw ? f94::kClassCrouched : f94::kClassStanding, s.neckY, s.camY, eyeHeight, &srcRaw);
                ++r.calls;
                if (used) ++r.crouchedUsed;
                if (raw) ++r.crouchedRaw;
                if (used && r.firstCrouchedUsedAt < 0.0) r.firstCrouchedUsedAt = s.t;
                if (!used && r.firstStandingUsedAt < 0.0) r.firstStandingUsedAt = s.t;
                if (src != f94::kEyeStance) ++r.notStance;
                if (srcRaw != f94::kEyeStance) ++r.notStanceRaw;
                if (havePrev && used != prevUsed) ++r.flipsUsed;
                if (havePrev && raw != prevRaw) ++r.flipsRaw;
                havePrev = true;
                prevUsed = used;
                prevRaw = raw;
                r.targetMin = std::min(r.targetMin, target);
                r.targetMax = std::max(r.targetMax, target);
                r.wcRawMax = std::max(r.wcRawMax, wcRaw);
            }
            return r;
        };
        // (a) looking straight up standing: the aim at +85, the animated neck at 1.362 (the log's), the camera sinking 12 to 35 cm under its standing mean in runs (the log's bimodal
        // heights), 3 s
        std::vector<Step> up;
        for (int i = 0; i < 270; ++i) {
            const double t = i / 90.0;
            const int run = (i / 11) % 4;   // 11 calls at 35 cm down, then three runs of calls at 5 to 25 cm down
            const double sink = run == 0 ? 0.35 : run == 1 ? 0.12 : run == 2 ? 0.22 : 0.05;
            up.push_back({t, hStand - sink, 1.362, 85.0});
        }
        const Result a = replay(up);
        check(a.wcRawMax > 0.5 && a.flipsRaw > 10 && a.notStanceRaw > 0 && a.crouchedUsed == 0 && a.flipsUsed == 0 && a.notStance == 0,
              "F169: looking straight up standing (aim +85, the animated neck 1.362 m, the camera sinking 35 cm in runs) w_c alone flips the class and F98c's source; with the cap the class never leaves "
              "standing, the settled raise's source never leaves the stance's");
        // (b) looking straight up crouched: the neck 1.0 m (a 54 cm drop), the camera 70 cm down: crouched on every call, no flip
        std::vector<Step> crouchUp;
        for (int i = 0; i < 270; ++i) crouchUp.push_back({i / 90.0, hStand - 0.70 + 0.05 * ((i / 11) % 3), 1.0 + 0.003 * (i % 4), 85.0});
        const Result b = replay(crouchUp);
        check(b.crouchedUsed == b.calls && b.flipsUsed == 0 && b.crouchedRaw == b.calls,
              "F169: looking straight up crouched (the neck 54 cm under the standing one) the class is crouched on every call, as w_c alone has it");
        // (c) a pilot crouched and looking up stands up: the neck rises from 1.0 to 1.543 over 0.6 s while the camera sinks and the aim stays at +85. The class switches within 0.3 s.
        std::vector<Step> standUp;
        for (int i = 0; i < 90; ++i) {
            const double t = i / 90.0, k = std::min(1.0, t / 0.6);
            standUp.push_back({t, hStand - (0.70 * (1.0 - k) + 0.35 * k), 1.0 + 0.543 * k, 85.0});
        }
        const Result c = replay(standUp);
        check(c.firstStandingUsedAt >= 0.0 && c.firstStandingUsedAt < 0.30 && c.flipsUsed == 1 && c.crouchedUsed > 0,
              "F169: standing up from a crouch while looking up the class switches to standing once, within 0.3 s of the neck's rise (the neck's drop under 0.38 m), though the camera still reads a crouch");
        // (d) a pilot standing crouches while the aim is pitched: the neck falls 1.543 to 1.0 over 0.4 s: the class switches once the neck is 38 cm down (70% of the way), within 0.3 s
        std::vector<Step> crouchDown;
        for (int i = 0; i < 90; ++i) {
            const double t = i / 90.0, k = std::min(1.0, t / 0.4);
            crouchDown.push_back({t, hStand - (0.35 * (1.0 - k) + 0.70 * k), 1.543 - 0.543 * k, 85.0});
        }
        const Result d = replay(crouchDown);
        check(d.firstCrouchedUsedAt >= 0.0 && d.firstCrouchedUsedAt < 0.30 && d.flipsUsed == 1,
              "F169: crouching while the aim is pitched (the neck falls 54 cm in 0.4 s) the class switches to crouched once, within 0.3 s, when the neck is 38 cm down");
        // (e) an unpitched crouch and the stand-up from it: w_c is exactly what it was on every call, and the class switches with the camera within 0.3 s of the camera's rise
        std::vector<Step> level;
        for (int i = 0; i < 90; ++i) {
            const double t = i / 90.0, k = std::min(1.0, t / 0.5);
            level.push_back({t, hStand - 0.70 * k, 1.543 - 0.543 * k, 0.0});
        }
        for (int i = 0; i < 90; ++i) {
            const double t = 1.0 + i / 90.0, k = std::min(1.0, i / 90.0 / 0.5);
            level.push_back({t, hStand - 0.70 * (1.0 - k), 1.0 + 0.543 * k, 0.0});
        }
        const Result e = replay(level);
        check(e.usedEqualsRaw && e.flipsUsed == 2 && e.flipsRaw == 2 && e.crouchedUsed == e.crouchedRaw && e.firstCrouchedUsedAt > 0.0 && e.firstCrouchedUsedAt < 0.30,
              "F169: level, a crouch and the stand-up from it switch the class exactly as w_c does (w_c used equals w_c on every call, bit for bit): crouched within 0.3 s, standing again once the camera is up");
        // (f) the gate: 19 deg is level, 21 deg is pitched (the sunk camera at each)
        std::vector<Step> gate19, gate21;
        for (int i = 0; i < 20; ++i) {
            gate19.push_back({i / 90.0, hStand - 0.35, 1.362, 19.0});
            gate21.push_back({i / 90.0, hStand - 0.35, 1.362, 21.0});
        }
        check(replay(gate19).crouchedUsed == 20 && replay(gate21).crouchedUsed == 0, "F169: the sunk camera at 19 deg of aim pitch still reads as w_c does, at 21 deg it reads standing with the neck standing");
    }

    // ---- 4. the window and the line ---------------------------------------------------------------------------------------------------------------------------------------------------
    {
        f169::Window w;
        const auto call = [&](double raw, double used, double posture, double step, double target) {
            f169::Call c;
            c.wcRaw = raw;
            c.wcUsed = used;
            c.postureW = posture;
            c.viewStepM = step;
            c.settledTargetM = target;
            w.add(c);
        };
        // the old behaviour: the class flips with the sink while the neck stands
        for (int i = 0; i < 40; ++i) call(i % 4 == 0 ? 0.62 : 0.05, i % 4 == 0 ? 0.62 : 0.05, 0.0, 0.002, std::nan(""));
        check(w.calls == 40 && w.sunk == 10 && w.sunkStanding == 10 && w.vetoed == 0 && w.crouched == 10 && w.flips == 19 && w.rawFlips == 19,
              "F169: the window counts the camera read as a crouch with the neck standing, and the class flipping with it, when the cap is not there");
        f169::Window v;
        for (int i = 0; i < 40; ++i) {
            f169::Call c;
            c.wcRaw = i % 4 == 0 ? 0.62 : 0.05;
            c.wcUsed = f169::wcUsed(c.wcRaw, f169::wcCap(true, 0.0));
            c.postureW = 0.0;
            c.viewStepM = (i % 2 ? 0.001 : -0.003);
            c.settledTargetM = i % 10 == 0 ? 0.27 : std::nan("");
            v.add(c);
        }
        check(v.sunk == 10 && v.sunkStanding == 10 && v.vetoed == 10 && v.crouched == 0 && v.flips == 0 && v.rawFlips == 19 && v.stepN == 40 && eq(v.stepMax, 0.003, 1e-15) &&
                  v.settled == 4 && eq(v.settledMin, 0.27, 1e-15) && eq(v.settledMax, 0.27, 1e-15),
              "F169: with the cap the window counts the vetoes, no flip with the neck standing (the health rule's test) and the raw flips w_c alone would have made");
        // a real crouch is not a flip with the neck standing
        f169::Window real;
        for (int i = 0; i < 20; ++i) {
            f169::Call c;
            c.wcRaw = i < 10 ? 0.05 : 0.9;
            c.postureW = i < 10 ? 0.0 : 1.0;
            c.wcUsed = f169::wcUsed(c.wcRaw, f169::wcCap(true, c.postureW));
            real.add(c);
        }
        check(real.flips == 0 && real.crouched == 10 && real.vetoed == 0 && real.sunkStanding == 0, "F169: a real crouch while pitched (the neck dropped too) switches the class and is not a flip with the neck standing");
        f169::Window both;
        both.merge(w);
        both.merge(v);
        f169::Seconds secs;
        secs.fold(w);
        secs.fold(v);
        f169::Window tiny;
        tiny.add(f169::Call{});
        secs.fold(tiny);
        check(both.calls == 80 && both.flips == 19 && both.vetoed == 10 && secs.pitched == 2 && secs.flipped == 1 && secs.sunk == 2, "F169: windows add up; seconds with under 10 pitched calls are not counted");
        char line[1300], worst[1300];
        const int len = f169::formatStance(line, sizeof(line), 10, both, secs);
        f169::Window big;
        big.calls = big.sunk = big.sunkStanding = big.vetoed = big.crouched = big.flips = big.rawFlips = big.stepN = big.settled = 4000000000u;
        big.stepSq = 1.0e12;
        big.stepMax = 1.0e6;
        big.settledMin = -1.0e6;
        big.settledMax = 1.0e6;
        f169::Seconds bigSecs;
        bigSecs.pitched = bigSecs.flipped = bigSecs.sunk = 4000000000u;
        const int worstLen = f169::formatStance(worst, sizeof(worst), 4000000000u, big, bigSecs);
        check(len > 0 && len < 800 && worstLen > 0 && worstLen < 1190 && std::strstr(line, "(F169) 10 s: pitched calls (|aim pitch| 20 deg or more) 80; the camera read as a crouch (w_c 0.5 or more) in 20, with the animated neck standing in 20; the cap took the crouch away in 10; used as crouched in 10; the stance flipped 19 times with the neck standing (w_c alone would have flipped it 38)") != nullptr &&
                  std::strstr(line, "pitched seconds 2, flipped 1, with a sink 2") != nullptr,
              "F169: the pitched stance line names the gate, the sink, the vetoes, the flips with the neck standing and the flips w_c alone would have made, within the log's 1200 bytes even at its worst");
        f169::Window empty;
        char none[1300];
        const int noneLen = f169::formatStance(none, sizeof(none), 1, empty, f169::Seconds{});
        check(noneLen > 0 && std::strstr(none, "nan") == nullptr && std::strstr(none, "inf") == nullptr && std::strstr(none, "pitched calls (|aim pitch| 20 deg or more) 0") != nullptr,
              "F169: an empty window prints zeros, never a number that is not one");
    }

    // ======================================================== C: the pistol sprint's arm swing ========================================================
    // ---- 5. the plan, the capture and the apply ----------------------------------------------------------------------------------------------------------------------------------------
    //  0 root; 1 chest; 2 left clavicle (1); 3 left upper (2); 4 left forearm (3); 5 left hand (4); 6, 7 fingers (5, 6); 8 twist (3); 9 right clavicle (1); 10 right upper (9); 11 head (1);
    //  12 a twist under the forearm (4); 13 a joint under the clavicle that is not under the upper arm (2)
    const uint16_t parents[14] = {0xFFFFu, 0, 1, 2, 3, 4, 5, 6, 3, 1, 9, 1, 4, 2};
    {
        const f169::Plan pl = f169::buildPlan(parents, 14, 2, 3, 4, 5);
        bool owners = pl.ok() && pl.carriedN == 5;
        const uint16_t want[5] = {6, 7, 8, 12, 13};
        const uint8_t wantOwner[5] = {3, 3, 1, 2, 0};   // fingers: the hand; twist under the upper arm; twist under the forearm; the clavicle's own child
        for (int i = 0; owners && i < 5; ++i) owners = pl.carried[i] == want[i] && pl.owner[i] == wantOwner[i];
        check(owners && pl.joint[0] == 2 && pl.joint[3] == 5 && pl.parent[0] == 1 && pl.parentSlot[0] == -1 && pl.parentSlot[1] == 0 && pl.parentSlot[3] == 2 && pl.joints == 14,
              "F169: the plan is the clavicle, upper arm, forearm and hand, parent first, with every other joint under the clavicle owned by its nearest chain ancestor (the right arm and the head are not)");
        const f169::Plan notChain = f169::buildPlan(parents, 14, 2, 3, 5, 5);
        const f169::Plan badIdx = f169::buildPlan(parents, 14, 2, 3, 4, 40);
        const f169::Plan none = f169::buildPlan(nullptr, 14, 2, 3, 4, 5);
        const uint16_t cyc[4] = {1, 2, 3, 0};
        const f169::Plan cycle = f169::buildPlan(cyc, 4, 1, 2, 3, 0);
        check(!notChain.ok() && notChain.why == f169::kNotChain && !badIdx.ok() && badIdx.why == f169::kBadInput && !none.ok() && !cycle.ok(),
              "F169: a hand whose parent is not the forearm, a joint out of range, no parent array and a cycle refuse the plan");
    }
    {
        // a pose: each joint a rotation about y by a different angle and a position, model matrices local x parent
        const f169::Plan pl = f169::buildPlan(parents, 14, 2, 3, 4, 5);
        float anim[14][16];
        const auto place = [&](float (*m)[16], const double* angles, const double offs[14][3]) {
            for (uint32_t j = 0; j < 14; ++j) {
                f166::Rec r;
                r.p[0] = offs[j][0]; r.p[1] = offs[j][1]; r.p[2] = offs[j][2];
                r.q[0] = 0.0; r.q[1] = std::sin(0.5 * angles[j]); r.q[2] = 0.0; r.q[3] = std::cos(0.5 * angles[j]);
                float local[16];
                f166::matOf(r, local);
                if (parents[j] == 0xFFFFu) std::memcpy(m[j], local, sizeof(local));
                else nf::pfzMul(local, m[parents[j]], m[j]);
            }
        };
        const double ang[14] = {0.0, 0.1, 0.2, 0.3, 0.5, -0.2, 0.1, 0.1, 0.4, 0.2, 0.1, 0.0, 0.3, 0.2};
        double offs[14][3];
        for (int j = 0; j < 14; ++j) { offs[j][0] = 0.05 * j; offs[j][1] = 0.30 - 0.01 * j; offs[j][2] = 0.02 * (j % 5); }
        place(anim, ang, offs);
        f169::Chain chain;
        check(f169::capture(pl, anim, 14, &chain), "F169: the animated chain is captured as local records against the parents");
        float out[14][16];
        uint16_t touched[14];
        uint32_t touchedN = 0;
        check(f169::apply(pl, chain, anim, 14, out, touched, &touchedN) && touchedN == 9, "F169: setting the chain to its own records touches the four chain joints and the five that come with them");
        double worstDiff = 0.0;
        for (uint32_t i = 0; i < touchedN; ++i)
            for (int c = 0; c < 16; ++c) worstDiff = std::max(worstDiff, std::fabs(static_cast<double>(out[touched[i]][c]) - anim[touched[i]][c]));
        check(worstDiff < 2e-5, "F169: ... and leaves every one of them where it was (the chain's own records, no change)");
        // a forearm bent 40 deg further: the hand and the fingers and the twist under the forearm come with it rigidly; the upper arm and its twist do not move
        f169::Chain bent = chain;
        const double extra = 40.0 * kPi / 180.0;
        const double a2 = std::atan2(chain.rec[2].q[1], chain.rec[2].q[3]) * 2.0 + extra;
        bent.rec[2].q[1] = std::sin(0.5 * a2);
        bent.rec[2].q[3] = std::cos(0.5 * a2);
        float out2[14][16];
        touchedN = 0;
        check(f169::apply(pl, bent, anim, 14, out2, touched, &touchedN), "F169: a bent forearm applies");
        const auto distTo = [&](const float (*m)[16], uint32_t a, uint32_t b) {
            const double d[3] = {static_cast<double>(m[a][12]) - m[b][12], static_cast<double>(m[a][13]) - m[b][13], static_cast<double>(m[a][14]) - m[b][14]};
            return nf::ikLen(d);
        };
        bool moved[14] = {};
        for (uint32_t i = 0; i < touchedN; ++i) {
            double dmax = 0.0;
            for (int c = 0; c < 16; ++c) dmax = std::max(dmax, std::fabs(static_cast<double>(out2[touched[i]][c]) - anim[touched[i]][c]));
            moved[touched[i]] = dmax > 1e-4;
        }
        check(!moved[2] && !moved[3] && !moved[8] && !moved[13] && moved[4] && moved[5] && moved[6] && moved[7] && moved[12] &&
                  eq(distTo(out2, 5, 6), distTo(anim, 5, 6), 1e-5) && eq(distTo(out2, 6, 7), distTo(anim, 6, 7), 1e-5) && eq(distTo(out2, 5, 4), distTo(anim, 5, 4), 1e-5) &&
                  eq(distTo(out2, 12, 4), distTo(anim, 12, 4), 1e-5),
              "F169: a forearm bent 40 deg moves the forearm, the hand, the fingers and the twist under the forearm rigidly with it, and nothing above it or beside it");
        // a singular parent refuses the capture and the apply
        float sing[14][16];
        std::memcpy(sing, anim, sizeof(sing));
        for (int c = 0; c < 16; ++c) sing[2][c] = 0.0f;
        f169::Chain cc;
        check(!f169::capture(pl, sing, 14, &cc), "F169: a singular parent refuses the capture");
    }

    // ---- 6. the ring ------------------------------------------------------------------------------------------------------------------------------------------------------------------
    const auto chainAt = [&](double z) {
        f169::Chain c;
        for (uint32_t k = 0; k < f169::kChain; ++k) c.rec[k].p[2] = z + 10.0 * k;
        return c;
    };
    {
        auto ring = std::make_unique<f169::Ring>();
        f169::Chain out;
        check(!ring->lookup(1000000, &out) && ring->n == 0 && ring->spanSec() == 0.0, "F169: an empty ring finds nothing");
        for (int i = 0; i < 100; ++i) ring->push(1000000ull + i * 11111ull, chainAt(i * 0.01));
        check(ring->n == 100 && eq(ring->spanSec(), 99 * 0.011111, 1e-9) && !ring->push(1000000ull + 99 * 11111ull, chainAt(9.0)) && !ring->push(5, chainAt(9.0)) && ring->n == 100,
              "F169: the ring keeps the entries in time order and drops one that is not later than the newest");
        const uint64_t t = 1000000ull + 10 * 11111ull + 5555ull;   // half-way between entries 10 and 11
        check(ring->lookup(t, &out) && eq(out.rec[0].p[2], 0.105, 1e-6) && eq(out.rec[3].p[2], 30.105, 1e-6), "F169: a lookup between two entries is interpolated by the fraction of the way");
        check(ring->lookup(1000000ull + 11111ull * 20, &out) && eq(out.rec[0].p[2], 0.20, 1e-12), "F169: a lookup at an entry's time is that entry");
        check(!ring->lookup(999999, &out) && !ring->lookup(1000000ull + 99 * 11111ull + 1, &out) && ring->lookup(1000000ull + 99 * 11111ull, &out),
              "F169: a time before the oldest entry or after the newest finds nothing (too little history: no delay)");
        auto wrap = std::make_unique<f169::Ring>();
        for (int i = 0; i < 400; ++i) wrap->push(2000000ull + i * 11111ull, chainAt(i * 0.01));
        check(wrap->n == f169::kRingCap && eq(wrap->spanSec(), (f169::kRingCap - 1) * 0.011111, 1e-9) && !wrap->lookup(2000000ull + 100 * 11111ull, &out) &&
                  wrap->lookup(2000000ull + 300 * 11111ull, &out) && eq(out.rec[0].p[2], 3.00, 1e-9),
              "F169: the ring wraps at its capacity; what it has dropped is gone and what it keeps is found in order");
        auto gap = std::make_unique<f169::Ring>();
        gap->push(1000000, chainAt(0.0));
        gap->push(1000000 + 500000, chainAt(1.0));
        check(!gap->lookup(1000000 + 250000, &out) && gap->lookup(1000000, &out) && gap->lookup(1000000 + 500000, &out), "F169: two entries 0.5 s apart are not interpolated across");
    }

    // ---- 7. the stride's period, from a swinging signal ----------------------------------------------------------------------------------------------------------------------------------
    {
        const auto run = [&](double periodSec, double amp, double offset, double seconds, uint32_t* stridesOut, bool* validOut, double* estOut) {
            f169::Stride st;
            uint64_t us = 5000000;
            uint32_t seed = 12345;
            double tt = 0.0;
            while (tt < seconds) {
                seed = seed * 1664525u + 1013904223u;
                const double jitter = ((seed >> 8) % 1000) / 1000.0 * 0.004 - 0.002;   // +-2 ms of frame time
                const double dt = 1.0 / 90.0 + jitter;
                tt += dt;
                us += static_cast<uint64_t>(dt * 1e6);
                st.feed(us, offset + amp * std::sin(2.0 * kPi * tt / periodSec));
            }
            *stridesOut = st.strides;
            *validOut = st.valid(us);
            *estOut = st.periodSec;
        };
        for (const double T : {0.45, 0.64, 0.80, 1.20}) {
            uint32_t strides = 0;
            bool valid = false;
            double est = 0.0;
            run(T, 0.10, 0.05, 8.0, &strides, &valid, &est);
            check(valid && strides >= 3 && std::fabs(est - T) < 0.01 * T,
                  "F169: the stride's period is read from a synthetic sine of the animated hand's swing within 1% (periods 0.45, 0.64, 0.80, 1.20 s, 10 cm amplitude, a 5 cm offset, frame times jittered 2 ms)");
        }
        uint32_t strides = 0;
        bool valid = true;
        double est = 0.0;
        run(0.64, 0.01, 0.0, 8.0, &strides, &valid, &est);   // a swing of 1 cm is inside the hysteresis: no stride
        check(!valid && strides == 0, "F169: a swing inside the hysteresis band (1 cm) is no stride");
        run(0.20, 0.10, 0.0, 6.0, &strides, &valid, &est);   // faster than 0.30 s
        const bool fastRefused = !valid;
        run(4.0, 0.10, 0.0, 20.0, &strides, &valid, &est);   // slower than 1.6 s
        check(fastRefused && !valid, "F169: a swing faster than 0.30 s or slower than 1.6 s a stride is not a stride");
        // the signal stops: the period goes stale
        f169::Stride st;
        uint64_t us = 1000000;
        double tt = 0.0;
        for (int i = 0; i < 540; ++i) {
            tt += 1.0 / 90.0;
            us += 11111;
            st.feed(us, 0.1 * std::sin(2.0 * kPi * tt / 0.64));
        }
        const bool liveValid = st.valid(us);
        for (int i = 0; i < 270; ++i) {
            us += 11111;
            st.feed(us, 0.0);
        }
        check(liveValid && !st.valid(us), "F169: when the hand stops swinging the period goes stale within about a second");
        // a gap in the calls restarts the estimator
        st.feed(us + 3000000, 0.1);
        check(st.strides == 0 && !st.valid(us + 3000000), "F169: a gap of seconds between calls restarts the stride estimator");
    }

    // ---- 8. the controller: the delay, the blend, the continuity -----------------------------------------------------------------------------------------------------------------------
    {
        const double A = 0.10, T = 0.64;
        // F167's ramps, for the release weight: in over 0.2 s while sprinting, out over 0.25 s after
        struct Sim {
            f169::Controller ctl;
            double tt = 0.0, ramp = 0.0;
            uint64_t us = 3000000;
        };
        const auto stepSim = [&](Sim& s, bool sprinting, double phase, double* signalNow, f169::Out* outp, double dtOverride = 0.0) {
            const double dt = dtOverride > 0.0 ? dtOverride : 1.0 / 90.0;
            s.tt += dt;
            s.us += static_cast<uint64_t>(dt * 1e6);
            s.ramp += sprinting ? dt / 0.20 : -(dt / 0.25);
            s.ramp = s.ramp < 0.0 ? 0.0 : s.ramp > 1.0 ? 1.0 : s.ramp;
            const double z = sprinting || s.ramp > 0.0 ? A * std::sin(2.0 * kPi * s.tt / T) : 0.0;
            f169::In in;
            in.us = s.us;
            in.pistolArmed = true;
            in.sprinting = sprinting;
            in.release = s.ramp;
            in.phase = phase;
            in.cur = chainAt(z);
            in.curValid = true;
            in.strideSignal = z;
            *outp = s.ctl.step(in);
            *signalNow = z;
        };
        // (a) the dial at 0: never applies, whatever else is there
        {
            Sim s;
            f169::Out o;
            double z = 0.0;
            bool any = false;
            for (int i = 0; i < 540; ++i) {
                stepSim(s, true, 0.0, &z, &o);
                any = any || o.apply;
            }
            check(!any && o.periodValid && eq(o.periodSec, T, 0.01 * T) && o.wantedSec == 0.0,
                  "F169: with the dial at 0 the stage never applies a chain (bit for bit F167) though the ring is full and the stride is known");
        }
        // (b) no period yet: no delay; a hand that does not swing: none; a stride out of range: none
        {
            Sim s;
            f169::Out o;
            double z = 0.0;
            bool applyEarly = false;
            for (int i = 0; i < 45; ++i) {   // 0.5 s of sprint: under two strides
                stepSim(s, true, 0.5, &z, &o);
                applyEarly = applyEarly || o.apply;
            }
            check(!applyEarly && !o.periodValid, "F169: at the start of a sprint, before the stride is measured, there is no delay");
            Sim flat;
            bool applyFlat = false;
            for (int i = 0; i < 540; ++i) {
                f169::In in;
                flat.us += 11111;
                in.us = flat.us;
                in.pistolArmed = true;
                in.sprinting = true;
                in.release = 1.0;
                in.phase = 0.5;
                in.cur = chainAt(0.0);
                in.curValid = true;
                in.strideSignal = 0.0;
                applyFlat = applyFlat || flat.ctl.step(in).apply;
            }
            check(!applyFlat, "F169: a left hand that does not swing has no stride and so no delay");
        }
        // (c) a half-stride delay inverts the sine, once the stride is known and the delay has slewed in
        {
            Sim s;
            f169::Out o;
            double z = 0.0;
            double worstInvert = 0.0;
            int inverted = 0;
            for (int i = 0; i < 1800; ++i) {   // 20 s
                stepSim(s, true, 0.5, &z, &o);
                if (i > 900 && o.apply) {
                    ++inverted;
                    worstInvert = std::max(worstInvert, std::fabs(o.target.rec[3].p[2] - (30.0 - z)));   // rec[3].p[2] is 30 + z for the hand: the delayed sine is the negative of the present one
                }
            }
            check(inverted > 800 && worstInvert < 0.03 * A && eq(o.delaySec, 0.5 * T, 0.01 * T) && o.ringReady && o.weight == 1.0,
                  "F169: a dial of 0.5 reads the arm half a stride back, which inverts the sine (within 3% of its amplitude): the left arm swings against the right (the delay is 0.5 of the measured stride)");
        }
        // (d) a quarter stride is the quadrature, a whole stride the same sine
        {
            Sim s;
            f169::Out o;
            double z = 0.0;
            double worst1 = 0.0;
            for (int i = 0; i < 1800; ++i) {
                stepSim(s, true, 1.0, &z, &o);
                if (i > 900 && o.apply) worst1 = std::max(worst1, std::fabs(o.target.rec[3].p[2] - (30.0 + z)));
            }
            check(worst1 < 0.03 * A, "F169: a dial of 1 reads a whole stride back: the same sine again (within 3% of its amplitude)");
        }
        // (e) continuity at the sprint's start and end
        {
            Sim s;
            f169::Out o;
            double z = 0.0, prevOut = 0.0;
            bool havePrev = false;
            double worstStep = 0.0, plainWorst = 0.0, prevPlain = 0.0;
            bool applyStarted = false, applyEnded = false, everApplied = false;
            double firstApplyGap = -1.0, lastApplyGap = -1.0;
            for (int i = 0; i < 1800; ++i) {
                const bool sprinting = i >= 90 && i < 1350;   // 1 s of walking, 14 s of sprint, 5 s of walking
                stepSim(s, sprinting, 0.5, &z, &o);
                const double cur = 30.0 + z;
                const double drawn = o.apply ? o.target.rec[3].p[2] : cur;   // what the arm is set to: the delayed blend where the stage applies, else the animated arm
                if (o.apply && !everApplied) {
                    everApplied = true;
                    applyStarted = true;
                    firstApplyGap = std::fabs(drawn - cur);
                }
                if (!o.apply && everApplied) {
                    applyEnded = true;
                    lastApplyGap = std::fabs(drawn - cur);
                }
                if (havePrev) {
                    worstStep = std::max(worstStep, std::fabs(drawn - prevOut));
                    plainWorst = std::max(plainWorst, std::fabs(cur - prevPlain));
                }
                havePrev = true;
                prevOut = drawn;
                prevPlain = cur;
            }
            check(applyStarted && applyEnded && worstStep <= 1.6 * plainWorst + 1e-9 && firstApplyGap < 0.25 * A && lastApplyGap < 1e-9,
                  "F169: the arm set by the stage never steps further per call than the sine itself does by more than 60%, through the sprint's start (the delay slews in at 0.5 s per s) and its end (the release falls in 0.25 s); "
                  "where the stage stops it is the animated arm exactly");
        }
        // (f) not armed (a put-away pistol, another item): everything starts again
        {
            Sim s;
            f169::Out o;
            double z = 0.0;
            for (int i = 0; i < 540; ++i) stepSim(s, true, 0.5, &z, &o);
            f169::In in;
            in.us = s.us + 11111;
            in.pistolArmed = false;
            in.curValid = true;
            in.cur = chainAt(0.0);
            const f169::Out gone = s.ctl.step(in);
            check(!gone.apply && s.ctl.ring.n == 0 && s.ctl.delaySec == 0.0 && s.ctl.stride.strides == 0, "F169: with the pistol put away the ring, the stride and the delay start again");
        }
        // (g) the release weight scales the blend
        {
            Sim s;
            f169::Out o;
            double z = 0.0;
            for (int i = 0; i < 900; ++i) stepSim(s, true, 0.5, &z, &o);
            f169::In in;
            in.us = s.us + 11111;
            in.pistolArmed = true;
            in.sprinting = true;
            in.phase = 0.5;
            in.strideSignal = 0.0;
            in.curValid = true;
            in.cur = chainAt(0.1);
            in.release = 0.5;
            const f169::Out half = s.ctl.step(in);
            in.us += 11111;
            in.release = 0.0;
            const f169::Out zero = s.ctl.step(in);
            check(half.apply && half.weight == 0.5 && !zero.apply && zero.weight == 0.0, "F169: the release weight is the blend's weight, and with no release nothing is applied");
        }
    }

    // ---- 9. the measure ---------------------------------------------------------------------------------------------------------------------------------------------------------------
    {
        const auto drive = [&](double rightSign, double rightPhase, double leftDrawnPhase, double seconds, f169::Counters* c, double legSign = 1.0) {
            f169::Meter m;
            const double A = 0.10, T = 0.64;
            double tt = 0.0;
            for (int i = 0; i < static_cast<int>(seconds * 90.0); ++i) {
                f169::Sample s;
                s.dtSec = 1.0 / 90.0;
                tt += s.dtSec;
                const double animF = A * std::sin(2.0 * kPi * tt / T), animU = 0.5 * A * std::sin(2.0 * kPi * tt / T + 0.7);
                s.animL[0] = 0.30 + animF;
                s.animL[1] = -0.05 + animU;
                s.drawnL[0] = 0.30 + A * std::sin(2.0 * kPi * tt / T + leftDrawnPhase);
                s.drawnL[1] = -0.05 + 0.5 * A * std::sin(2.0 * kPi * tt / T + 0.7 + leftDrawnPhase);
                s.right[0] = 0.45 + rightSign * 0.5 * A * std::sin(2.0 * kPi * tt / T + rightPhase);
                s.right[1] = -0.10 + rightSign * 0.25 * A * std::sin(2.0 * kPi * tt / T + 0.7 + rightPhase);
                s.legs = legSign * 0.2 * std::sin(2.0 * kPi * tt / T);
                s.periodSec = T;
                s.delaySec = 0.0;
                s.phase = 0.0;
                m.add(s, *c);
            }
        };
        f169::Counters unison, opposed, quad;
        drive(1.0, 0.0, 0.0, 6.0, &unison);
        drive(-1.0, 0.0, 0.0, 6.0, &opposed);
        drive(1.0, kPi / 2.0, 0.0, 6.0, &quad);
        check(unison.measured > 400 && eq(unison.lrF.corr(), 1.0, 1e-3) && eq(unison.lrU.corr(), 1.0, 1e-3) && eq(unison.arF.corr(), 1.0, 1e-3) && eq(unison.legA.corr(), 1.0, 1e-3) &&
                  eq(opposed.lrF.corr(), -1.0, 1e-3) && eq(opposed.lrU.corr(), -1.0, 1e-3) && std::fabs(quad.lrF.corr()) < 0.05,
              "F169: the measure reads +1 for arms in unison, -1 for arms opposing and about 0 in quadrature, forward and up, and the left hand's forward against the legs");
        check(eq(unison.lrF.ampA(), 0.10, 0.004) && eq(unison.lrF.ampB(), 0.05, 0.003) && eq(unison.lrU.ampA(), 0.05, 0.003) && eq(unison.lrU.ampB(), 0.025, 0.002) && eq(unison.legA.ampB(), 0.2, 0.008),
              "F169: the amplitudes are those of the sines the figures were made of, from the swing about the running mean (the offsets are out of them)");
        // the dial's effect on the measure: the drawn left against the animated one half a stride apart
        f169::Counters half;
        drive(1.0, 0.0, kPi, 6.0, &half);
        check(eq(half.arF.corr(), 1.0, 1e-3) && eq(half.lrF.corr(), -1.0, 1e-3) && eq(half.legD.corr(), -1.0, 1e-3),
              "F169: with the drawn left arm half a stride from the animated one the drawn arm opposes the right while the animated one still swings with it");
        // the warm-up, the gaps, the means
        f169::Meter m;
        f169::Counters c;
        f169::Sample s;
        s.dtSec = 1.0 / 90.0;
        for (int i = 0; i < 20; ++i) {
            s.drawnL[0] = s.drawnL[1] = s.right[0] = s.right[1] = s.animL[0] = s.animL[1] = 0.1;
            s.legs = 0.0;
            m.add(s, c);
        }
        check(c.released == 20 && c.measured == 0, "F169: calls inside the warm-up are released but not measured");
        s.dtSec = 1.0;
        m.add(s, c);
        check(c.released == 21 && c.measured == 0 && m.warmSec == 0.0, "F169: a gap between calls restarts the measure");
        f169::Sample bad;
        bad.dtSec = 0.5;
        f169::Counters c2;
        f169::Meter m2;
        m2.add(bad, c2);
        bad.dtSec = 0.02;
        for (int i = 0; i < 100; ++i) m2.add(bad, c2);
        check(c2.measured == 0, "F169: a call whose figures are not numbers is released but not measured");
        f169::SprintSeconds secs;
        f169::Counters ten, nine;
        ten.released = 10;
        nine.released = 9;
        secs.fold(ten);
        secs.fold(nine);
        check(secs.sprint == 1, "F169: a second with ten released calls is a second of pistol sprint");
        f169::Counters sum;
        sum.merge(unison);
        sum.merge(opposed);
        check(sum.released == unison.released + opposed.released && sum.lrF.n == unison.lrF.n + opposed.lrF.n && std::fabs(sum.lrF.corr()) < 0.01, "F169: two windows add up; unison and opposing together cancel");
    }

    // ---- 10. the swing line ---------------------------------------------------------------------------------------------------------------------------------------------------------
    {
        f169::Counters c;
        c.released = 400;
        c.measured = 380;
        c.withPeriod = 300;
        c.delayed = 200;
        c.periodSum = 300 * 0.64;
        c.phaseSum = 380 * 0.5;
        c.delaySum = 200 * 0.32;
        for (int i = 0; i < 380; ++i) {
            const double a = 0.1 * std::sin(i * 0.2), b = 0.05 * std::sin(i * 0.2 + 0.5);
            c.lrF.add(a, b);
            c.lrU.add(0.5 * a, 0.5 * b);
            c.arF.add(a, b);
            c.arU.add(0.5 * a, 0.5 * b);
            c.legD.add(a, 0.2 * b);
            c.legA.add(a, 0.2 * b);
        }
        f169::SprintSeconds secs;
        secs.sprint = 4;
        char line[1300];
        const int len = f169::formatSwing(line, sizeof(line), 10, "$wpn_s_pistol_kinetic_sauto_name;", c, secs);
        f169::Counters big;
        big.released = big.measured = big.withPeriod = big.delayed = 4000000000u;
        big.periodSum = big.phaseSum = big.delaySum = 1.0e12;
        for (f169::Pair* p : {&big.lrF, &big.lrU, &big.arF, &big.arU, &big.legD, &big.legA}) {
            p->n = 4000000000u;
            p->saa = p->sbb = 1.0e12;
            p->sab = -1.0e12;
        }
        f169::SprintSeconds bigSecs;
        bigSecs.sprint = 4000000000u;
        char worst[1300];
        const int worstLen = f169::formatSwing(worst, sizeof(worst), 4000000000u, "$wpn_s_pistol_kinetic_sauto_name_with_a_long_tail;", big, bigSecs);
        check(len > 0 && len < 1000 && worstLen > 0 && worstLen < 1190 &&
                  std::strstr(line, "(F169, TEMPORARY) 10 s: item $wpn_s_pistol_kinetic_sauto_name;: calls") == nullptr &&
                  std::strstr(line, "(F169, TEMPORARY) 10 s: item $wpn_s_pistol_kinetic_sauto_name;; calls released (0.5 or more) 400, measured 380, with a stride 300, delayed 200; seconds 4; the stride 0.64 s "
                                    "(from the animated left hand's forward swing against the chest)") != nullptr &&
                  std::strstr(line, "the dial experimental.native_on_foot_pistol_sprint_swing_phase mean 0.50, the delay applied 0.32 s") != nullptr,
              "F169: the swing line names the item, the calls released, measured, with a stride and delayed, the stride and where it is read from, and the dial with the delay applied, within the log's 1200 bytes at its worst");
        f169::Counters empty;
        char none[1300];
        const int noneLen = f169::formatSwing(none, sizeof(none), 1, nullptr, empty, f169::SprintSeconds{});
        check(noneLen > 0 && std::strstr(none, "item none") != nullptr && std::strstr(none, "nan") == nullptr && std::strstr(none, "inf") == nullptr, "F169: an empty window prints zeros, never a number that is not one");
        check(eq(f169::clampPhase(0.45), 0.45, 1e-15) && f169::clampPhase(-1.0) == 0.0 && f169::clampPhase(1.5) == 1.0 && f169::clampPhase(std::nan("")) == 0.0 && f169::clampPhase(std::numeric_limits<double>::infinity()) == 0.0,
              "F169: the dial is held to 0..1; a value that is not a number reads 0 (no delay)");
    }
}

}  // namespace f169test
