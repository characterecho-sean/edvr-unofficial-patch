// F133 checks (native on-foot VR): a stall cannot leave the body off the head. Header only, run by native_frame_test (run133).
// The flight (edvr_gfx_20261027_080326, 08:06:12-16): the head turned 41 deg in 3 s across a render stall; adsHeadDelta dropped the step as a LOCAL recentre (over 30 deg),
// the turn back was counted, and the reported head stood 40 deg off the aim for 20 s.
#pragma once

#include "../../src/d3d11/native_on_foot_f133.h"

#include <cmath>
#include <cstdint>

namespace f133test {

namespace nf = edvr::nofoot;
namespace f133 = edvr::nofoot::f133;

// A head transform in OpenXR axes at the given heading (seat convention, left +) and elevation, as the F62b checks build them.
inline void headAt(double yawDeg, double pitchDeg, float tx, float m[12]) {
    const double kR = nf::kAdsDeg;
    const double cy = std::cos(yawDeg * kR), sy = std::sin(yawDeg * kR), cp = std::cos(pitchDeg * kR), sp = std::sin(pitchDeg * kR);
    const double R[3][3] = {{cy, sy * sp, sy * cp}, {0.0, cp, -sp}, {-sy, cy * sp, cy * cp}};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) m[4 * i + j] = static_cast<float>(R[i][j]);
    m[3] = tx;
    m[7] = 1.6f;
    m[11] = 0.0f;
}

template <class Check>
inline void run133(Check&& check) {
    const double kR = nf::kAdsDeg;
    {   // a step is judged by its rate
        float a[12], b41[12], b25[12], b10[12], moved[12], vertical[12];
        headAt(0.0, 0.0, 0.0f, a);
        headAt(-41.1, 0.0, 0.0f, b41);
        headAt(25.0, 0.0, 0.0f, b25);
        headAt(10.0, 0.0, 0.0f, b10);
        headAt(0.0, 0.0, 0.8f, moved);
        headAt(0.0, 85.0, 0.0f, vertical);
        const f133::Step stall = f133::headStep(a, b41, 3.0);
        check(stall.ok && std::fabs(stall.dYaw + 41.1 * kR) < 1e-5 && stall.bigAsMotion && stall.rateDps > 13.0 && stall.rateDps < 14.5,
              "F133: a 41 deg head step over 3000 ms is motion (13.7 deg/s), not a recentre (the 08:06:12-16 stall); it is counted as a large step taken as motion");
        const f133::Step instant = f133::headStep(a, b41, 0.011);
        check(!instant.ok && instant.rateDps > 3000.0, "F133: a 41 deg step over 11 ms is a discontinuity (3700 deg/s): an instant recentre");
        const f133::Step quick = f133::headStep(a, b25, 0.011);
        const f133::Step oldRule = f133::headStep(a, b25, 0.0);
        check(!quick.ok && quick.rateDps > 2200.0 && quick.rateDps < 2300.0 && oldRule.ok && !oldRule.bigAsMotion,
              "F133: a 25 deg step over 11 ms (2270 deg/s) is a discontinuity under the rate rule, though the old size rule (30 deg) took it as motion; with no usable time "
              "the old rule stands");
        const f133::Step none = f133::headStep(a, b41, 0.0), nan = f133::headStep(a, b41, std::nan(""));
        check(!none.ok && !nan.ok, "F133: with no usable time a 41 deg step is a discontinuity (the old 30 deg rule)");
        const f133::Step walk = f133::headStep(a, b10, 0.011);
        check(walk.ok && std::fabs(walk.dYaw - 10.0 * kR) < 1e-5 && !walk.bigAsMotion && walk.rateDps < 1000.0, "F133: an ordinary turn (10 deg in a frame, 900 deg/s) is motion as before");
        // the read-time jitter: a short dt is floored at 8 ms, so a 5 deg step read 2 ms after the last sample (2500 deg/s unfloored) is not a recentre
        float b5[12];
        headAt(5.0, 0.0, 0.0f, b5);
        const f133::Step jitter = f133::headStep(a, b5, 0.002);
        check(jitter.ok && jitter.rateDps < 700.0, "F133: a step read 2 ms after the last is judged over 8 ms at the least (read-time jitter is no recentre): 5 deg is 625 deg/s");
        const f133::Step moveFar = f133::headStep(a, moved, 3.0), vert = f133::headStep(a, vertical, 3.0);
        check(!moveFar.ok && !vert.ok, "F133: a 0.8 m step is a discontinuity whatever the time, and a head near vertical has no heading");
        f133::BigSteps big;
        big.note(41.1, 3000.0);
        big.note(33.0, 800.0);
        check(big.n == 2 && big.largestDeg == 41.1 && big.largestMs == 3000.0, "F133: the large steps are counted with the largest and the time it took");
    }
    {   // the re-centre: once after 2 s at 4 deg with the head still; not at 2 deg; not while the head moves; not twice within 2 s
        const auto drive = [&](f133::Recentre& r, double offDeg, double seconds, double headRateDps, double& t, double& head, bool allowed = true) {
            int fires = 0;
            for (double e = 0.0; e < seconds; e += 0.011) {
                t += 0.011;
                head += headRateDps * 0.011;
                const f133::Recentre::Out o = r.step(t, allowed, true, offDeg, head);
                fires += o.fire ? 1 : 0;
            }
            return fires;
        };
        {
            f133::Recentre r;
            double t = 100.0, head = 0.0;
            const int early = drive(r, 4.0, 1.9, 0.0, t, head);
            const int onTime = drive(r, 4.0, 0.2, 0.0, t, head);
            check(early == 0 && onTime == 1 && r.fired == 1, "F133 re-centre: 4 deg of yaw offset with the head still fires once, after 2 s (not before)");
            const int again = drive(r, 4.0, 1.7, 0.0, t, head);   // 1.8 s after the first (the 9 ticks left of the 0.2 s drive count)
            const int later = drive(r, 4.0, 0.5, 0.0, t, head);
            check(again == 0 && later == 1 && r.fired == 2, "F133 re-centre: and not again within 2 s; a standing offset that stays fires again after 2 more");
        }
        {
            f133::Recentre r;
            double t = 100.0, head = 0.0;
            check(drive(r, 2.0, 10.0, 0.0, t, head) == 0, "F133 re-centre: 2 deg off the aim is not a standing offset");
            f133::Recentre q;
            double tq = 100.0, hq = 0.0;
            check(drive(q, -3.0, 10.0, 0.0, tq, hq) == 0 && drive(q, -3.2, 2.1, 0.0, tq, hq) == 1, "F133 re-centre: more than 3 deg either way counts (-3.2), exactly 3 does not");
        }
        {
            f133::Recentre r;
            double t = 100.0, head = 0.0;
            check(drive(r, 6.0, 10.0, 20.0, t, head) == 0, "F133 re-centre: not while the head turns (20 deg/s)");
            const int still = drive(r, 6.0, 2.6, 0.0, t, head);
            check(still == 1, "F133 re-centre: 2 s after the head came to rest (within the stillness window) it fires");
            f133::Recentre w;
            double tw = 100.0, hw = 0.0;
            const int a = drive(w, 6.0, 1.5, 0.0, tw, hw);
            const int b = drive(w, 6.0, 0.5, 8.0, tw, hw);   // a turn of 8 deg/s for half a second: over the 5 deg/s
            const int c = drive(w, 6.0, 1.5, 0.0, tw, hw);   // 1.5 s more: 2 s since the start, but the clock restarted with the turn
            const int d = drive(w, 6.0, 1.2, 0.0, tw, hw);
            check(a == 0 && b == 0 && c == 0 && d == 1, "F133 re-centre: a turn restarts the 2 s");
            f133::Recentre tremor;
            double tt = 100.0, ht = 0.0;
            int f = 0;
            for (double e = 0.0; e < 3.0; e += 0.011) {   // the head's tremor: 0.05 deg a frame back and forth (net zero)
                tt += 0.011;
                ht += (static_cast<int>(e / 0.011) % 2 ? -0.05 : 0.05);
                f += tremor.step(tt, true, true, 6.0, ht).fire ? 1 : 0;
            }
            check(f == 1, "F133 re-centre: the head's tremor (0.05 deg a frame, no net turn) is still");
        }
        {
            f133::Recentre r;
            double t = 100.0, head = 0.0;
            check(drive(r, 6.0, 10.0, 0.0, t, head, false) == 0, "F133 re-centre: not while blocked, held, in a snap fade or an ADS press (allowed false)");
            check(drive(r, 6.0, 1.5, 0.0, t, head) == 0 && drive(r, 1.0, 0.5, 0.0, t, head) == 0 && drive(r, 6.0, 1.5, 0.0, t, head) == 0,
                  "F133 re-centre: an offset that falls under 3 deg restarts the 2 s");
            f133::Recentre unread;
            double tu = 100.0;
            int f = 0;
            for (double e = 0.0; e < 5.0; e += 0.011) {
                tu += 0.011;
                f += unread.step(tu, true, false, 40.0, 0.0).fire ? 1 : 0;
            }
            check(f == 0, "F133 re-centre: an unreadable reported head (within 11 deg of vertical) re-centres nothing");
        }
        {   // the flight: after the stall the reported head stood -39.3 deg off for 20 s: one re-centre (a head step now taken as motion would not have left it)
            f133::Recentre r;
            double t = 100.0, head = 0.0;
            const int fires = drive(r, -39.3, 20.0, 0.0, t, head);
            check(fires >= 1 && fires <= 10, "F133 re-centre: a 39 deg offset standing for 20 s is re-centred (the 08:06:16 case), once per 2 s at the most");
        }
    }
}

}  // namespace f133test
