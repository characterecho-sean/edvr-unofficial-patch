// F159 checks (native on-foot VR), run159 below. Header only, run by native_frame_test.
//
// F159 (native_on_foot_f159.h): the left elbow's stable pole is reseeded from the body-down pole when its preferred elbow (the transported reference placed on the circle) is within
// 1 cm of the shoulder's height or above, with the arm bent, for three solves in a row, at most once in 0.5 s; every seed's birth is counted by the reason the last one was gone and
// logged at most once in 0.5 s. The flight edvr_gfx_20261028_113409's raised events are replayed on the real solver (native_on_foot_core.h): the left arm at the hip, the hand about
// 22 cm right, 5 cm up and 43 cm forward of the shoulder, where the natural events chose the reference and the raised ones were parked on the E.y <= S.y ceiling.
#pragma once

#include "../../src/d3d11/native_on_foot_core.h"
#include "../../src/d3d11/native_on_foot_f159.h"

#include <cmath>
#include <cstdint>
#include <cstring>

namespace f159test {

namespace nf = edvr::nofoot;
namespace f159 = edvr::nofoot::f159;

template <class Check>
inline void run159(Check&& check) {
    constexpr double kUp[3] = {0.0, 1.0, 0.0};
    // ---- 1. the decision ----------------------------------------------------------------------------------------------------------------------------------------------------
    {
        check(f159::atCeiling(0.0) && f159::atCeiling(0.05) && f159::atCeiling(-0.0099) && !f159::atCeiling(-0.01) && !f159::atCeiling(-0.0101) && !f159::atCeiling(-0.12),
              "F159: the elbow is at the ceiling when it is above the shoulder's height less 1 cm (+0.05, 0 and -0.99 cm are; -1.0 and -12 cm are not)");
        check(!f159::atCeiling(std::nan("")) && !f159::bentArm(std::nan("")) && !f159::reseedCondition(std::nan(""), 0.15) && !f159::reseedCondition(0.0, std::nan("")),
              "F159: a figure that is not a number is never the condition");
        check(f159::bentArm(0.08) && f159::bentArm(0.158) && !f159::bentArm(0.0799) && !f159::bentArm(0.039),
              "F159: the arm is bent from an elbow 8 cm off the shoulder-to-hand axis (a nearly straight arm, 3.9 cm, is exempt)");
        check(f159::reseedCondition(0.0, 0.15) && f159::reseedCondition(-0.005, 0.08) && !f159::reseedCondition(-0.02, 0.15) && !f159::reseedCondition(0.05, 0.05),
              "F159: the condition needs both: the preferred elbow at the ceiling and the arm bent");
        check(f159::atHip(0.0) && f159::atHip(0.049) && !f159::atHip(0.05) && !f159::atHip(1.0), "F159: the call is at the hip while the body's ADS detach weight is under 0.05");
    }
    // ---- 2. the gate: three solves in a row, at most once in 0.5 s -----------------------------------------------------------------------------------------------------------
    {
        f159::ReseedGate g;
        const bool a = g.step(true, 1000), b = g.step(true, 1011), c = g.step(true, 1022);
        check(!a && !b && c, "F159: the condition must hold for three solves in a row: the third reseeds, the first two do not");
        check(g.held() == 0, "F159: a reseed starts the run again");
        bool again = false;
        for (uint64_t t = 1033; t < 1500; t += 11) again = g.step(true, t) || again;
        check(!again, "F159: the condition held for 40 solves over 470 ms reseeds nothing more (at most one in 0.5 s)");
        check(g.step(true, 1522), "F159: at 0.5 s from the reseed the held condition reseeds again");
        check(!g.step(true, 1533), "F159: and the run starts over after it (one solve is not three)");

        f159::ReseedGate broken;
        broken.step(true, 0);
        broken.step(true, 11);
        broken.step(false, 22);
        const bool afterBreak = broken.step(true, 33) || broken.step(true, 44);
        check(!afterBreak && broken.step(true, 55), "F159: a solve without the condition breaks the run: two, a miss, then three more are needed");

        f159::ReseedGate gap;
        gap.step(true, 0);
        gap.step(true, 11);
        const bool afterGap = gap.step(true, 400);
        check(!afterGap && !gap.step(true, 411) && gap.step(true, 422), "F159: a gap of over 100 ms between two solves breaks the run (the arm was not solved in it)");

        f159::ReseedGate born;
        born.step(true, 0);
        born.step(true, 11);
        born.breakRun();
        check(!born.step(true, 22) && !born.step(true, 33) && born.step(true, 44), "F159: a new seed starts the run again (breakRun)");

        // held at 3 for the whole rate limit, then fires at once: the condition has held for three solves
        f159::ReseedGate limited;
        limited.step(true, 0);
        limited.step(true, 11);
        const bool reseededAt22 = limited.step(true, 22);
        limited.step(true, 33);
        limited.step(true, 44);
        const bool blocked = limited.step(true, 55);   // three in a row again, 33 ms after the last reseed: refused
        check(reseededAt22 && !blocked && limited.held() >= f159::kHoldSolves, "F159: three in a row inside the 0.5 s wait (the run is kept) is not a reseed");
        uint64_t firedAt = 0;
        for (uint64_t t = 66; t < 700 && !firedAt; t += 11)
            if (limited.step(true, t)) firedAt = t;
        check(firedAt == 528, "F159: the held run reseeds at the first solve 0.5 s after the last reseed (at 22 ms: the solve at 528 ms)");
    }
    // ---- 3. the real solver: the raised elbow, parked on the ceiling, and the body-down seed -------------------------------------------------------------------------------------
    {
        // The left arm (out = -1: model x is left negative): shoulder (-0.18, 1.40, 0), the hand 22 cm right, 5 cm up and 43 cm forward, an arm of 0.30 + 0.28 m.
        const double S[3] = {-0.18, 1.40, 0.0}, T[3] = {-0.18 + 0.22, 1.40 + 0.05, 0.43}, ref[3] = {0.0, -1.0, 0.0};
        // The outward requirement is not what is at issue (a hand 22 cm across the body cannot be met by the swivel alone): the stable pole is asked with none, the ceiling alone.
        const double outSign = -1.0, outM = -1.0, downOutM = 0.06;
        const nf::IkTwoBone tb = nf::ikTwoBoneFrame(S, T, 0.30, 0.28, ref);
        check(tb.ok && tb.h > 0.12 && tb.h < 0.20 && f159::bentArm(tb.h), "F159: the flight's reach at the hip is a bent arm (the elbow 12 to 20 cm off the axis)");
        // The preferred elbow's height over the shoulder as a function of the swivel, and the swivel at which it is +1.2 cm (just over the ceiling) on the rising side.
        const auto over = [&](double th) {
            double E[3];
            tb.elbowAt(th, E);
            return E[1] - S[1];
        };
        double upPerp[3];
        {
            const double along = nf::ikDot(kUp, tb.dir);
            for (int i = 0; i < 3; ++i) upPerp[i] = kUp[i] - along * tb.dir[i];
            nf::ikUnit(upPerp, upPerp);
        }
        const double thetaUp = tb.angleOf(upPerp);   // the highest elbow on the circle
        const double thetaDown = nf::ikWrapPi(thetaUp + nf::kIkPi);
        // walk from the highest elbow toward the lowest (the shorter way) until the elbow is 1.2 cm over the shoulder
        double thetaRaised = thetaUp;
        const double stepDir = nf::ikWrapPi(thetaDown - thetaUp) >= 0.0 ? 1.0 : -1.0;
        for (int k = 0; k < 3600 && over(thetaRaised) > 0.012; ++k) thetaRaised = nf::ikWrapPi(thetaRaised + stepDir * 0.0005);
        check(over(thetaUp) > 0.10 && over(thetaDown) < -0.10 && std::fabs(over(thetaRaised) - 0.012) < 0.004,
              "F159: the circle runs from an elbow well over the shoulder to one well under it, and a swivel with the preferred elbow 1.2 cm over the shoulder exists");
        // the seed: a reference at that swivel (the transported pole pointing the elbow up)
        nf::IkPhysicalPole seed;
        seed.valid = nf::ikElbowRadial(tb, thetaRaised, seed.reference);
        for (int i = 0; i < 3; ++i) {
            seed.previous[i] = seed.reference[i];
            seed.axis[i] = tb.dir[i];
        }
        const nf::IkStableSwivel raised = nf::ikStableSwivel(tb, seed, outSign, outM);
        double Ep[3], Ec[3];
        tb.elbowAt(tb.angleOf(raised.reference), Ep);
        tb.elbowAt(raised.theta, Ec);
        const double prefOver = Ep[1] - S[1], chosenOver = Ec[1] - S[1];
        const double dispDeg = std::fabs(nf::ikWrapPi(raised.theta - tb.angleOf(raised.reference))) * 180.0 / nf::kIkPi;
        check(raised.ok && raised.transportedReference && f159::reseedCondition(prefOver, tb.h) && chosenOver <= 1e-6 && chosenOver > -0.012 && dispDeg > 0.5 && dispDeg < 15.0,
              "F159: the transported reference with the elbow 1.2 cm over the shoulder meets the condition, and the stable pole parks the chosen elbow on the ceiling a few degrees off it");
        // the F159 reseed from the body-down pole
        const nf::IkPistolDownSwivel down = nf::ikPistolDownSwivel(tb, seed.previous, kUp, downOutM, 0.0);
        double Ed[3];
        tb.elbowAt(down.theta, Ed);
        const double downOver = Ed[1] - S[1];
        check(down.ok && downOver < -0.08 && downOver > -0.25 && !f159::atCeiling(downOver),
              "F159: the body-down pole at the same reach puts the elbow 8 to 25 cm under the shoulder (the natural events were -8 to -17 cm)");
        nf::IkPhysicalPole reseeded = seed;
        for (int i = 0; i < 3; ++i) {
            reseeded.axis[i] = down.axis[i];
            reseeded.reference[i] = down.reference[i];
        }
        const nf::IkStableSwivel next = nf::ikStableSwivel(tb, reseeded, outSign, outM);
        double En[3];
        tb.elbowAt(next.theta, En);
        check(next.ok && En[1] - S[1] < -0.08 && std::fabs(nf::ikWrapPi(next.theta - tb.angleOf(next.reference))) < 0.02,
              "F159: from the reseeded pole the next solve keeps the elbow under the shoulder with the chosen swivel on the reference (no displacement)");
        // the hand moves 4 cm forward and 3 cm up: the transported reseeded pole still has the elbow clear of the ceiling
        const double T2[3] = {T[0] + 0.01, T[1] + 0.03, T[2] + 0.04};
        const nf::IkTwoBone tb2 = nf::ikTwoBoneFrame(S, T2, 0.30, 0.28, ref);
        const nf::IkStableSwivel moved = nf::ikStableSwivel(tb2, reseeded, outSign, outM);
        double Em[3];
        tb2.elbowAt(moved.theta, Em);
        check(moved.ok && Em[1] - S[1] < -0.05, "F159: with the hand moved 4 cm forward and 3 cm up the reseeded pole still carries the elbow clear of the ceiling");
        // control: without the reseed the same raised seed keeps the chosen elbow on the ceiling for the whole hold
        const nf::IkStableSwivel still = nf::ikStableSwivel(tb2, seed, outSign, outM);
        double Es[3];
        tb2.elbowAt(still.theta, Es);
        check(still.ok && Es[1] - S[1] > -0.012, "F159 (control): the raised seed, left as it was, keeps the elbow on the ceiling when the hand moves (the flight's 8 s runs)");

        // the natural event: the preferred elbow 12 cm under the shoulder is not the condition
        nf::IkPhysicalPole natural;
        natural.valid = nf::ikElbowRadial(tb, thetaDown, natural.reference);
        for (int i = 0; i < 3; ++i) {
            natural.previous[i] = natural.reference[i];
            natural.axis[i] = tb.dir[i];
        }
        const nf::IkStableSwivel nat = nf::ikStableSwivel(tb, natural, outSign, outM);
        double En2[3];
        tb.elbowAt(tb.angleOf(nat.reference), En2);
        check(nat.ok && !f159::reseedCondition(En2[1] - S[1], tb.h), "F159: a preferred elbow well under the shoulder (the natural events) is not the condition");

        // a nearly straight arm (hand 57.5 cm out) with the preferred elbow up is exempt: its elbow is 4 cm off the axis
        const double Tstraight[3] = {S[0] + 0.05, S[1] + 0.05, S[2] + 0.572};
        const nf::IkTwoBone straight = nf::ikTwoBoneFrame(S, Tstraight, 0.30, 0.28, ref);
        double straightUp[3];
        {
            const double along = nf::ikDot(kUp, straight.dir);
            for (int i = 0; i < 3; ++i) straightUp[i] = kUp[i] - along * straight.dir[i];
            nf::ikUnit(straightUp, straightUp);
        }
        double Ef[3];
        straight.elbowAt(straight.angleOf(straightUp), Ef);
        check(straight.ok && straight.h < f159::kBentArmM && f159::atCeiling(Ef[1] - S[1]) && !f159::reseedCondition(Ef[1] - S[1], straight.h),
              "F159: a nearly straight arm with the preferred elbow up (the elbow under 8 cm off the axis) is exempt");
    }
    // ---- 4. the births: why the last seed was gone -------------------------------------------------------------------------------------------------------------------------
    {
        check(f159::beginWhy(false, false, 5, 5, 1, 1) == f159::kWhyArmsOk && f159::beginWhy(false, true, 5, 5, 1, 1) == f159::kWhyStandDown &&
                  f159::beginWhy(true, true, 5, 6, 1, 1) == f159::kWhyGeneration && f159::beginWhy(true, true, 5, 5, 1, 2) == f159::kWhyItem &&
                  f159::beginWhy(true, true, 5, 5, 1, 1) == f159::kWhyFirst && f159::beginWhy(true, true, 0xFFFFFFFFu, 5, 0, 1) == f159::kWhyFirst &&
                  f159::beginWhy(true, true, 5, 6, 1, 2) == f159::kWhyGeneration,
              "F159: the reset at the set's start names its cause: not wanted with armsOk false (armsOk), not wanted otherwise (standDown), a new generation, a new item (the generation first), "
              "and nothing new when only the active flag was down or the set never began");
        f159::PoleTracker t;
        const f159::Birth first = t.born(1000);
        check(first.why == f159::kWhyFirst && first.since == f159::kWhyFirst && first.absentMs == 0 && !t.absent(), "F159: the first seed of the session was born 'first', after no absence");
        t.gone(f159::kWhyItem, 2000);
        t.gone(f159::kWhySupportCup, 2300);
        t.gone(f159::kWhySupportCup, 2310);
        const f159::Birth second = t.born(2500);
        check(second.why == f159::kWhySupportCup && second.since == f159::kWhyItem && second.absentMs == 500,
              "F159: the seed born when the left hand became eligible after an item change says supportCup (the latest), item (the first cause) and the 500 ms without a seed");
        check(t.born(2600).why == f159::kWhyFirst, "F159: a seed born with nothing dropped since the last birth is 'first' again (the tracker starts over)");
        t.gone(f159::kWhyArmsOk, 3000);
        check(t.born(3100).why == f159::kWhyArmsOk, "F159: armsOk false before the draw is the birth's reason");
        t.gone(255, 3200);
        check(!t.absent(), "F159: a reason out of range is ignored");
        check(std::strcmp(f159::whyName(f159::kWhyReseed), "F159") == 0 && std::strcmp(f159::whyName(f159::kWhyStandDown), "standDown") == 0 && std::strcmp(f159::whyName(99), "unknown") == 0,
              "F159: the reasons' names");
        f159::LineLimit lim;
        uint32_t skipped = 99;
        const bool p1 = lim.take(1000, &skipped);
        const bool p2 = lim.take(1100, &skipped), p3 = lim.take(1499, &skipped);
        uint32_t before = 0;
        const bool p4 = lim.take(1500, &before);
        check(p1 && !p2 && !p3 && p4 && before == 2 && lim.take(2000, &before) && before == 0,
              "F159: one birth line in 0.5 s: the first passes, two inside the 0.5 s are counted, the next to pass says 2 were not written");
    }
    // ---- 5. the per-second figures and the lines ---------------------------------------------------------------------------------------------------------------------------
    {
        f159::Counters c;
        const auto add = [&](double solved, double fp, double disp, bool ceiling, bool hip, bool reseed, uint8_t birth) {
            f159::Sample s;
            s.solvedOverM = solved;
            s.fpOverM = fp;
            s.dispDeg = disp;
            s.ceiling = ceiling;
            s.hip = hip;
            s.reseed = reseed;
            s.birth = birth;
            c.add(s);
        };
        add(-0.002, -0.14, 9.0, true, true, false, 0xFF);     // raised, at the hip
        add(0.000, -0.14, 11.0, true, true, false, 0xFF);     // raised, at the hip
        add(-0.122, -0.13, 0.0, false, false, false, 0xFF);   // natural, ADS
        add(-0.05, -0.13, 0.0, false, true, true, f159::kWhyReseed);
        add(-0.04, -0.13, 0.0, false, true, false, f159::kWhyItem);
        add(std::nan(""), 0.0, 0.0, false, true, false, 0xFF);   // not counted
        check(c.n == 5 && c.hipN == 4 && c.high == 2 && c.hipHigh == 2 && c.ceiling == 2 && c.reseeds == 1 && c.births[f159::kWhyReseed] == 1 && c.births[f159::kWhyItem] == 1 &&
                  std::fabs(c.solvedMin + 12.2) < 1e-9 && std::fabs(c.solvedMax - 0.0) < 1e-9 && std::fabs(c.solvedSum + 21.4) < 1e-9 && std::fabs(c.fpMin + 14.0) < 1e-9 &&
                  std::fabs(c.dispMax - 11.0) < 1e-9 && std::fabs(c.dispSum - 20.0) < 1e-9,
              "F159: the counters: solves, at the hip, solved within 1 cm of the shoulder (and at the hip), the preferred elbow at the ceiling, reseeds, births by reason, the solved and FP heights "
              "(mean, min, max) and the swivel's displacement (a figure that is not a number is left out)");
        f159::Counters d;
        d.merge(c);
        d.merge(c);
        check(d.n == 10 && d.high == 4 && d.births[f159::kWhyItem] == 2 && std::fabs(d.solvedMin + 12.2) < 1e-9 && std::fabs(d.dispMax - 11.0) < 1e-9 && std::fabs(d.solvedSum + 42.8) < 1e-9,
              "F159: two seconds merge by sums, minima and maxima");
        check(f159::secondHigh(5, 9) && !f159::secondHigh(4, 8) && !f159::secondHigh(0, 0) && f159::secondHigh(1, 1),
              "F159: a second has the elbow at the shoulder's height when most of its solves do (over half)");

        // the seconds of a window and the run at the hip, carried across a window that closes full
        f159::HighSeconds h;
        const auto second = [&](uint32_t hipHigh, uint32_t hipN, uint32_t high, uint32_t n) {
            f159::Counters s;
            s.hipHigh = hipHigh;
            s.hipN = hipN;
            s.high = high;
            s.n = n;
            h.fold(s);
        };
        second(80, 80, 80, 80);
        second(80, 80, 80, 80);
        check(h.any == 2 && h.hip == 2 && h.run == 2 && h.runMax == 2, "F159: two raised seconds at the hip are a run of two");
        f159::HighSeconds nextWindow = h.next(true);
        h = nextWindow;
        second(80, 80, 80, 80);
        check(h.any == 1 && h.hip == 1 && h.run == 3 && h.runMax == 3,
              "F159: the run is kept across a window that closes full: the third second in a row, in the next window, is a run of three (longer than 2 s)");
        second(0, 0, 70, 70);   // ADS only: not a second at the hip
        check(h.run == 0 && h.runMax == 3 && h.any == 2 && h.hip == 1, "F159: a second raised in ADS only breaks the run at the hip and counts in the seconds");
        check(h.next(false).run == 0 && h.next(false).runMax == 0, "F159: a window closed by a change of item or stance starts the run over");

        // the lines fit under the log's 1200 bytes at their widest
        f159::Counters w;
        for (int i = 0; i < 3; ++i) w.merge(c);
        f159::HighSeconds hs;
        hs.any = hs.hip = hs.run = hs.runMax = 10;
        char buf[1100];
        const int n = f159::formatSummary(buf, sizeof(buf), 10, "$wpn_m_assaultrifle_kinetic_fauto_name;", w, hs);
        check(n > 0 && n < 900 && std::strstr(buf, "(F159) 10 s: item $wpn_m_assaultrifle_kinetic_fauto_name;; 15 solves with the stable pole (12 at the hip)") != nullptr &&
                  std::strstr(buf, "within 1 cm of the shoulder's height or above in 6 (6 at the hip)") != nullptr && std::strstr(buf, "reseeds 3; seed births: first 0, item 3, generation 0, armsOk 0, standDown 0, supportCup 0, refused 0, uncommitted 0, F159 3;") != nullptr &&
                  std::strstr(buf, "seconds with the elbow within 1 cm of the shoulder 10, at the hip 10, longest run at the hip 10 s") != nullptr,
              "F159: the window's line names the item, the solves, the heights, the ceiling count, the displacement, the reseeds, the births by reason and the seconds; under 900 bytes");
        f159::BirthLine bl;
        bl.why = f159::kWhySupportCup;
        bl.since = f159::kWhyItem;
        bl.absentMs = 4294967295ull;
        bl.baseDeg = -179.9;
        bl.clamped = true;
        bl.fpPole = true;
        bl.seededOverCm = -123.4;
        bl.fpOverCm = -123.4;
        bl.handErrCm = 19.0;
        bl.armedRamp = 1.0;
        bl.stance = 1;
        bl.pitchOk = true;
        bl.pitchDeg = -89.9;
        bl.skipped = 4000000000u;
        char line[400];
        const int bn = f159::formatBirth(line, sizeof(line), bl);
        check(bn > 0 && bn < 380 && std::strstr(line, "(F159): born by supportCup (first cause item, no seed for 4294967295 ms); base swivel -179.9 deg, clamped, from the FP elbow's pole;") != nullptr &&
                  std::strstr(line, "the left hand 19.0 cm off its grip; armed ramp 1.00; stance crouched; head pitch -89.9 deg; 4000000000 earlier seed(s) not logged") != nullptr,
              "F159: a birth's line names the reason and its first cause, the swivel and the clamp, the pole's source, the seeded and FP elbow heights, the hand's error, the armed ramp, the stance and the "
              "head pitch, and says how many births the limit kept out; under 380 bytes at its widest");
        f159::BirthLine rl;
        rl.why = rl.since = f159::kWhyReseed;
        rl.reseed = true;
        rl.baseDeg = -75.0;
        rl.seededOverCm = -11.9;
        rl.fpOverCm = -14.0;
        rl.handErrCm = 3.0;
        rl.armedRamp = 1.0;
        rl.stance = 0;
        rl.wasOverCm = 0.4;
        rl.offsetCm = 15.8;
        const int rn = f159::formatBirth(line, sizeof(line), rl);
        check(rn > 0 && std::strstr(line, "born by F159 (reseeded in place); base swivel -75.0 deg, from the body-down pole; seeded elbow -11.9 cm over the shoulder, the FP elbow -14.0 cm;") != nullptr &&
                  std::strstr(line, "head pitch unread; the pole it replaced put the elbow +0.4 cm over the shoulder with the arm 15.8 cm off its axis") != nullptr,
              "F159: a reseed's line says it was reseeded in place from the body-down pole, the height it replaced and the arm's offset; a head pitch not read says so");
        char tiny[40];
        check(f159::formatBirth(tiny, sizeof(tiny), bl) > 0 && std::strlen(tiny) == sizeof(tiny) - 1, "F159: a line cut to a short buffer stays terminated");
    }
}

}  // namespace f159test
