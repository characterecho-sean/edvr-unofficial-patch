// F136 checks (native on-foot VR): an upright body under a level head while the aim is pitched down (mouse look). Header only, run by native_frame_test (run136).
// The flight (edvr_gfx_20261027_100213, 10:05:30-44, look to aim off): the chest's lean +58 to +61 deg at an aim of -67 to -74 deg against +33 to +37 deg at a level aim; the
// view held at the standing eye over a bent neck.
#pragma once

#include "../../src/d3d11/native_on_foot_f136.h"

#include <cmath>
#include <cstdint>

namespace f136test {

namespace f136 = edvr::nofoot::f136;
namespace nf = edvr::nofoot;

template <class Check>
inline void run136(Check&& check) {
    {   // the weight: the aim pitched down by more than 10 deg, the head's pitch above it through smoothstep(10, 30)
        check(f136::targetWeight(true, -68.0, 0.0) == 1.0, "F136 weight: the aim down at -68 deg under a level head (gap 68) is the full weight");
        check(f136::targetWeight(true, -5.0, 0.0) == 0.0 && f136::targetWeight(true, -10.0, 0.0) == 0.0 && f136::targetWeight(true, 30.0, 0.0) == 0.0,
              "F136 weight: an aim level, up or down by no more than 10 deg writes nothing");
        check(f136::targetWeight(true, -12.0, 0.0) == f136::smoothstep(10.0, 30.0, 12.0) && f136::targetWeight(true, -12.0, 0.0) > 0.0 && f136::targetWeight(true, -12.0, 0.0) < 0.1,
              "F136 weight: -12 deg under a level head is a gap of 12: a small weight");
        check(f136::targetWeight(true, -40.0, -35.0) == 0.0 && f136::targetWeight(true, -40.0, -30.0) == 0.0 && std::fabs(f136::targetWeight(true, -40.0, -20.0) - 0.5) < 1e-12 &&
                  f136::targetWeight(true, -40.0, -10.0) == 1.0,
              "F136 weight: the head 5 or 10 deg above the aim is 0, 20 deg is a half, 30 deg is the whole");
        check(f136::targetWeight(true, -60.0, -60.0) == 0.0 && f136::targetWeight(true, -60.0, -70.0) == 0.0 && f136::targetWeight(false, -60.0, 0.0) == 0.0 &&
                  f136::targetWeight(true, std::nan(""), 0.0) == 0.0,
              "F136 weight: the head on the aim, or below it, or an unknown angle, is 0");
        check(f136::weightFor(false, true, -68.0, 0.0, 0.0) == 1.0 && f136::weightFor(true, true, -68.0, 0.0, 0.0) == 0.0 && f136::weightFor(false, true, -68.0, 0.0, 1.0) == 0.0 &&
                  std::fabs(f136::weightFor(false, true, -68.0, 0.0, 0.5) - 0.5) < 1e-12,
              "F136 weight: 0 while look to aim drops the player's look, 0 in ADS (scaled by its weight)");
    }
    {   // under look to aim the head follows the aim: a nod down and back, the aim lagging the head by the follower's 150 ms smoothing and 180 deg/s cap, never writes
        double head = 0.0, aim = 0.0, maxDown = 0.0, maxUp = 0.0;
        const double dt = 1.0 / 90.0, tau = 0.15;
        const auto simulate = [&](double headRateDps, double seconds, double* maxW) {
            for (double t = 0.0; t < seconds; t += dt) {
                head += headRateDps * dt;
                const double ask = (head - aim) * (1.0 - std::exp(-dt / tau));
                const double cap = 180.0 * dt;
                aim += ask > cap ? cap : ask < -cap ? -cap : ask;
                const double w = f136::targetWeight(true, aim, head);
                if (w > *maxW) *maxW = w;
            }
        };
        simulate(-120.0, 0.5, &maxDown);   // the nod: the head leads the aim downward
        simulate(0.0, 1.0, &maxDown);      // held: the aim catches up
        const double settledGap = head - aim;
        simulate(+120.0, 0.5, &maxUp);     // the head comes back up with the aim still pitched down: the gap grows for a moment
        check(maxDown == 0.0 && std::fabs(settledGap) < 1.0,
              "F136 look to aim: the head nodding down leads the aim, so the head is below it (gap negative) and nothing is written; held, the gap closes (under 1 deg)");
        check(maxUp > 0.0 && f136::weightFor(true, true, -60.0, 0.0, 0.0) == 0.0,
              "F136 look to aim: a fast return of the head lags the aim for a moment (the gap passes 10 deg), which is why the hook also writes nothing while look to aim drops the look");
    }
    {   // the ramp: 0.15 s, no pop on a stall
        f136::Ramp r;
        r.step(1.0, 0.075);
        const double half = r.w;
        r.step(1.0, 0.075);
        const double full = r.w;
        r.step(0.0, 0.075);
        const double down = r.w;
        f136::Ramp s;
        s.step(1.0, 2.0);   // a 2 s stall is no time at all
        check(std::fabs(half - 0.5) < 1e-12 && std::fabs(full - 1.0) < 1e-12 && std::fabs(down - 0.5) < 1e-12 && s.w == 0.0, "F136 ramp: 0.15 s from 0 to 1 and back, and a long gap moves nothing");
    }
    {   // the lean and the turn back: the chest turned back by the angle lowers the lean by it, the levels share it, and the neck rises
        const double chest[3] = {0.0, 1.0, 0.05}, neck[3] = {0.0, 1.3, 0.25};
        const double lean = f136::leanDeg(chest, neck);   // atan2(0.2, 0.3) = 33.7
        double R[9], v[3] = {neck[0] - chest[0], neck[1] - chest[1], neck[2] - chest[2]}, back[3];
        f136::backRot(20.0, 1.0, R);
        nf::bendApply(v, R, back);
        const double c2[3] = {0.0, 0.0, 0.0}, n2[3] = {back[0], back[1], back[2]};
        const double leanBack = f136::leanDeg(c2, n2);
        check(std::fabs(lean - 33.690067525979785) < 1e-9 && std::fabs(lean - leanBack - 20.0) < 1e-9 && back[1] > v[1] && back[2] < v[2] && std::fabs(back[0]) < 1e-12,
              "F136 turn: backRot(20 deg) lowers the chest-to-neck lean by exactly 20 deg about the body's right axis, nose up (the neck rises and moves back), no sideways part");
        double R3[9], R2[9], R1[9], a[3], b[3], c[3];
        f136::backRot(30.0, nf::kIkSpineWeight[0], R1);
        f136::backRot(30.0, nf::kIkSpineWeight[1], R2);
        f136::backRot(30.0, nf::kIkSpineWeight[2], R3);
        const double up[3] = {0.0, 1.0, 0.0};
        nf::bendApply(up, R1, a);
        nf::bendApply(up, R2, b);
        nf::bendApply(up, R3, c);
        const double zero[3] = {0.0, 0.0, 0.0};
        check(std::fabs(f136::leanDeg(zero, a) + 10.0) < 1e-9 && std::fabs(f136::leanDeg(zero, b) + 20.0) < 1e-9 && std::fabs(f136::leanDeg(zero, c) + 30.0) < 1e-9,
              "F136 turn: the spine levels (16, 21, 24) take 1/3, 2/3 and the whole of the angle, as the F58 bend and the square-up share theirs");
        // the flight's figures: animated 59.5 at an aim of -68 under a level head, level 34.5: the whole 25 deg is taken back
        check(std::fabs(f136::backDeg(1.0, 59.5, 34.5) - 25.0) < 1e-12 && std::fabs(f136::backDeg(0.4, 59.5, 34.5) - 10.0) < 1e-12 && f136::backDeg(1.0, 30.0, 34.5) == 0.0 &&
                  f136::backDeg(0.0, 59.5, 34.5) == 0.0 && f136::backDeg(1.0, 99.0, 34.5) == f136::kMaxBackDeg && f136::backDeg(1.0, std::nan(""), 34.5) == 0.0,
              "F136 target: w x (animated - level) degrees back (25 at the flight's 59.5 against 34.5), never turned forward, never past 45 deg");
    }
    {   // the level lean: learned per item from the animated pose while the aim is level for 0.5 s
        f136::LevelLean ll;
        double lean = 0.0;
        const uint32_t rifle = 7, pistol = 9;
        double t = 10.0;
        for (int i = 0; i < 40; ++i, t += 0.011) ll.observe(true, 2.0, 34.0, rifle, t, static_cast<uint64_t>(t * 1000.0));   // 0.44 s level: not settled yet
        const bool tooSoon = !ll.level(rifle, &lean);
        for (int i = 0; i < 100; ++i, t += 0.011) ll.observe(true, 2.0, 34.0 + (i % 2 ? 0.5 : -0.5), rifle, t, static_cast<uint64_t>(t * 1000.0));
        const bool learned = ll.level(rifle, &lean);
        const double leanRifle = lean;
        for (int i = 0; i < 100; ++i, t += 0.011) ll.observe(true, -68.0, 59.5, rifle, t, static_cast<uint64_t>(t * 1000.0));   // aimed down: no sample
        double after = 0.0;
        ll.level(rifle, &after);
        const bool pistolUnknown = !ll.level(pistol, &lean);
        // a level aim broken by an aim down restarts the 0.5 s
        ll.observe(true, 3.0, 20.0, pistol, t, static_cast<uint64_t>(t * 1000.0));
        const bool pistolStill = !ll.level(pistol, &lean);
        check(tooSoon && learned && std::fabs(leanRifle - 34.0) < 0.6 && after == leanRifle && pistolUnknown && pistolStill,
              "F136 level lean: learned per item only after 0.5 s of level aim (|pitch| < 10 deg), untouched by the aim down, unknown for an item not seen level");
        f136::LevelLean many;
        double tt = 0.0;
        for (uint32_t item = 1; item <= 6; ++item) {
            for (int i = 0; i < 80; ++i, tt += 0.011) many.observe(true, 0.0, 30.0 + item, item, tt, static_cast<uint64_t>(tt * 1000.0) + 1);
        }
        double l1 = 0.0, l6 = 0.0;
        check(!many.level(1, &l1) && many.level(6, &l6) && std::fabs(l6 - 36.0) < 1e-9, "F136 level lean: four items are kept, the least recently used goes");
    }
}

}  // namespace f136test
