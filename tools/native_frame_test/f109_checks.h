// F109 checks (native on-foot VR): look to aim on foot, the player's own turn in ADS, snap or smooth turning. Header only, run by native_frame_test (run109).
// The game's figures are build 334925's as the RE reads them (analysis\decomp\verify_20261025_onfoot_look_input.txt): the processor's delta is
// c * (dt * rate * stick + g * (mouse + in) + dt * spring), linear in the mouse path, and the aim takes it whole in the call.
#pragma once

#include "../../src/d3d11/native_on_foot_core.h"
#include "../../src/d3d11/native_on_foot_f109.h"

#include <cmath>
#include <limits>

namespace f109test {

namespace nofoot = edvr::nofoot;
namespace f109 = edvr::nofoot::f109;

template <class Check>
inline void run109(Check&& check) {
    const double kD = f109::kDeg;
    {   // the split: a clean call gives the gain; a stick's turn with the head aim writing is the player's (the tester's ADS report), F62b's ratio counted it as ours
        f109::Split s;
        f109::SplitIn c;
        c.in = 0.5;
        c.d = 0.425;
        const f109::SplitOut a = s.step(c);
        f109::SplitIn st;
        st.in = 0.01;          // the head's tremor, written this call
        st.stick = 0.8;        // the right stick held
        st.d = 0.85 * 0.01 + 0.05;
        const f109::SplitOut b = s.step(st);
        const double f62b = st.d * st.in / (st.mouse + st.in);   // F62b's ours: the whole delta
        check(a.clean && a.gainTaken && std::fabs(s.gain - 0.85) < 1e-12 && std::fabs(a.ours - 0.425) < 1e-12 && std::fabs(a.player) < 1e-12 && !b.clean &&
                  !b.gainTaken && std::fabs(b.ours - 0.0085) < 1e-12 && std::fabs(b.player - 0.05) < 1e-12 && std::fabs(f62b - st.d) < 1e-12,
              "F109 split: a clean call takes the gain 0.85; a call with the stick held and the head aim writing counts the stick's 0.05 rad as the player's and "
              "only 0.85 x the head aim's input as ours (F62b's mouse ratio took the whole delta as ours, so the cancel held the view while the avatar turned)");
    }
    {   // the gain only from clean calls: a stick, a spring or a blocked call leaves it; a clean call in ADS takes the ADS gain
        f109::Split s;
        s.gain = 0.85;
        f109::SplitIn st, sp, bl, ads;
        st.in = 0.2;
        st.stick = -0.5;
        st.d = 0.3;
        sp.in = 0.1;
        sp.springTerm = 0.02;
        sp.d = 0.85 * 0.1 + 0.02;
        bl.in = 0.2;
        bl.d = 0.0;
        ads.in = 0.4;
        ads.d = 0.2;
        const f109::SplitOut o1 = s.step(st);
        const double g1 = s.gain;
        const f109::SplitOut o2 = s.step(sp);
        const double g2 = s.gain;
        const f109::SplitOut o3 = s.step(bl);
        const double g3 = s.gain;
        const f109::SplitOut o4 = s.step(ads);
        check(!o1.clean && g1 == 0.85 && !o2.clean && g2 == 0.85 && std::fabs(o2.ours - 0.085) < 1e-12 && std::fabs(o2.player - 0.02) < 1e-12 && o3.blocked &&
                  o3.ours == 0.0 && o3.player == 0.0 && g3 == 0.85 && o4.clean && std::fabs(s.gain - 0.5) < 1e-12,
              "F109 split: the gain is taken only from clean calls (a stick, a spring and a blocked call leave 0.85; the spring's 0.02 rad is the player's), and a "
              "clean call in ADS takes the ADS end of the gain (0.5)");
    }
    {   // before any clean call a stick's call is split by the mouse ratio and counted; a non-number is the player's whole
        f109::Split s;
        f109::SplitIn st;
        st.in = 0.1;
        st.mouse = 0.1;
        st.stick = 0.3;
        st.d = 0.4;
        const f109::SplitOut o = s.step(st);
        f109::SplitIn nan = st;
        nan.stick = std::numeric_limits<double>::quiet_NaN();
        const f109::SplitOut n = s.step(nan);
        check(o.unsplit && std::fabs(o.ours - 0.2) < 1e-12 && s.gain == 0.0 && n.ours == 0.0 && n.player == 0.4,
              "F109 split: with no gain measured yet a stick's call is split by the mouse ratio and flagged unsplit; a call with a non-number is the player's whole");
    }
    {   // the drop: off leaves the game's look; snap takes the yaw and the pitch (the snap rides the mouse's yaw); smooth keeps the yaw, takes the pitch
        const f109::Feed off = f109::feed(false, f109::kSnap, 0.7, 0.3, 2.0, -1.0, 0.0);
        const f109::Feed snap = f109::feed(true, f109::kSnap, 0.7, 0.3, 2.0, -1.0, 5.0);
        const f109::Feed smooth = f109::feed(true, f109::kSmooth, 0.7, 0.3, 2.0, -1.0, 0.0);
        check(off.stickYaw == 0.7 && off.stickPitch == 0.3 && off.mouseYaw == 2.0 && off.mousePitch == -1.0 && snap.stickYaw == 0.0 && snap.stickPitch == 0.0 &&
                  snap.mouseYaw == 5.0 && snap.mousePitch == 0.0 && smooth.stickYaw == 0.7 && smooth.stickPitch == 0.0 && smooth.mouseYaw == 2.0 &&
                  smooth.mousePitch == 0.0,
              "F109 drop: look to aim off leaves the game's look; snap drops the stick's and the mouse's yaw and pitch (the snap's input rides the mouse's yaw); "
              "smooth keeps the yaw and drops the pitch");
    }
    {   // the stick snap: disarmed until near the centre, one step a push, re-armed under 0.25
        f109::StickSnap s;
        const double xs[9] = {0.9, 0.1, 0.75, 0.95, 0.3, 0.71, 0.2, -0.8, -0.9};
        int got[9];
        for (int i = 0; i < 9; ++i) got[i] = s.step(xs[i]);
        check(got[0] == 0 && got[1] == 0 && got[2] == 1 && got[3] == 0 && got[4] == 0 && got[5] == 0 && got[6] == 0 && got[7] == -1 && got[8] == 0,
              "F109 stick snap: a stick held at the start does not fire; past 0.70 one step a push; held or back to 0.3 nothing more; under 0.25 it re-arms and "
              "the next push the other way steps left");
    }
    {   // the mouse flick: 20 deg of would-be turn within 150 ms fires once; a long swipe fires once; slow mouse never; after a pause the other way fires
        f109::MouseFlick m;
        int fires = 0, dirSum = 0;
        double t = 0.0;
        for (int i = 0; i < 90; ++i, t += 0.011) {   // a 1 s swipe at 3 deg a tick (270 deg/s)
            const int d = m.step(t, 3.0);
            fires += d != 0;
            dirSum += d;
        }
        const int swipeFires = fires;
        for (int i = 0; i < 30; ++i, t += 0.011) m.step(t, 0.0);   // a pause
        fires = 0;
        for (int i = 0; i < 10; ++i, t += 0.011) fires += m.step(t, -3.0) != 0 ? 1 : 0;
        f109::MouseFlick slow;
        int slowFires = 0;
        for (int i = 0; i < 300; ++i) slowFires += slow.step(i * 0.011, 1.0) != 0;   // 90 deg/s: 13.6 deg in a window
        check(swipeFires == 1 && dirSum == 1 && fires == 1 && slowFires == 0,
              "F109 mouse flick: a 1 s swipe at 270 deg/s turns one snap; after a pause a flick the other way turns one; a 90 deg/s mouse (14 deg a window) never");
    }
    {   // the snap's run: dark over 20 ms, injected once black, held until applied (25 ms at least, 100 ms at most), back over 30 ms
        f109::SnapRun r;
        bool inj = false;
        int injections = 0;
        const bool began = r.begin(0.0, 1) && !r.begin(0.001, -1);
        const double l1 = r.step(0.011, false, &inj);
        injections += inj;
        const double l2 = r.step(0.022, false, &inj);
        injections += inj;
        const double l3 = r.step(0.033, true, &inj);   // applied 11 ms after the injection: still held
        injections += inj;
        const double l4 = r.step(0.050, true, &inj);   // 28 ms: the fade out begins
        const double l5 = r.step(0.065, true, &inj);
        const double l6 = r.step(0.085, true, &inj);
        f109::SnapRun never;
        never.begin(0.0, -1);
        never.step(0.021, false, &inj);
        never.step(0.100, false, &inj);
        const double n1 = never.step(0.122, false, &inj);   // 101 ms after the injection: out, unapplied
        check(began && l1 > 0.5 && l1 < 0.6 && l2 == 1.0 && injections == 1 && l3 == 1.0 && l4 == 1.0 && std::fabs(l5 - 0.5) < 1e-9 && l6 == 0.0 &&
                  r.phase == f109::SnapRun::kIdle && n1 == 1.0 && never.phase == f109::SnapRun::kFadeOut,
              "F109 snap run: half dark at 11 ms, black and injected once at 22 ms, held while applied under 25 ms, out from 28 ms (half at 15 ms of it, clear at 30), "
              "and an unapplied snap leaves the dark after 100 ms");
    }
    {   // the turn style and the slider
        check(f109::turnStyleFromKey("smooth") == f109::kSmooth && f109::turnStyleFromKey("snap") == f109::kSnap && f109::turnStyleFromKey("blink") == f109::kSnap &&
                  f109::turnStyleFromKey(nullptr) == f109::kSnap && f109::snapDegClamp(5) == 15 && f109::snapDegClamp(200) == 90 && f109::snapDegClamp(45) == 45,
              "F109 turn style: smooth or snap (anything else snap); the snap step held to 15..90 deg");
    }
    {   // the smoothing: the key's with nothing drawn, ramped out by F88's armed weight and by ADS
        check(std::fabs(f109::followerTau(150.0, 0.0, 0.0) - 0.15) < 1e-12 && f109::followerTau(150.0, 1.0, 0.0) == 0.0 &&
                  std::fabs(f109::followerTau(150.0, 0.5, 0.0) - 0.075) < 1e-12 && f109::followerTau(150.0, 0.0, 1.0) == 0.0 && f109::followerTau(0.0, 0.0, 0.0) == 0.0 &&
                  f109::followerAlpha(0.011, 0.0) == 1.0 && std::fabs(f109::followerAlpha(0.15, 0.15) - (1.0 - std::exp(-1.0))) < 1e-12,
              "F109 smoothing: 150 ms with nothing drawn, 75 ms halfway through a draw, 0 with an item drawn or in ADS; alpha 1 at 0 ms");
    }
    // The follower against the real map and a game that applies each ask whole one tick later (the RE's aim step): the head turns 30 deg/s for 2 s and stops;
    // the aim (the game's applied turns) follows; the cancel is exactly the aim's turn, so the view (head + aim - cancel) is the head's; a block of 12 ticks drops
    // the asks and the aim still arrives.
    auto run = [&](double tau, int blockFrom, int blockTo, double centre, double* maxLag, double* endErr, double* viewErr) {
        nofoot::AdsAimMap map;
        f109::Follower fo;
        fo.addHead(centre, 0.0);
        double pending = 0.0, aim = 0.0, head = centre, cancel = 0.0;
        *maxLag = *viewErr = 0.0;
        const double dt = 1.0 / 90.0;
        for (int k = 0; k < 540; ++k) {
            const double dHead = k < 180 ? 30.0 * kD * dt : 0.0;
            head += dHead;
            const double applied = (k >= blockFrom && k < blockTo) ? 0.0 : pending;
            fo.addHead(dHead, 0.0);
            fo.addApplied(applied, 0.0);
            const double ify = f109::inFlightAfter(map.flightYaw, applied), ifp = f109::inFlightAfter(map.flightPitch, 0.0);
            double askY = 0.0, askP = 0.0;
            fo.ask(dt, tau, ify, ifp, &askY, &askP);
            const nofoot::AdsAimMap::Out mo = map.step(true, false, dt, 1.0, askY != 0.0, askY, askP, applied, 0.0, true, true);
            aim += applied;
            cancel += mo.cancelYaw;
            pending = mo.injYaw;
            *maxLag = std::max(*maxLag, std::fabs(head - aim));
            *viewErr = std::max(*viewErr, std::fabs((head + aim - cancel) - head));
        }
        *endErr = std::fabs(head - aim);
    };
    {
        double lag0 = 0, end0 = 0, view0 = 0, lag1 = 0, end1 = 0, view1 = 0, lagB = 0, endB = 0, viewB = 0, lagC = 0, endC = 0, viewC = 0;
        run(0.0, -1, -1, 0.0, &lag0, &end0, &view0);
        run(0.15, -1, -1, 0.0, &lag1, &end1, &view1);
        run(0.0, 60, 72, 0.0, &lagB, &endB, &viewB);
        run(0.0, -1, -1, 30.0 * kD, &lagC, &endC, &viewC);
        check(lag0 < 0.75 * kD && end0 < 0.01 * kD && view0 < 1e-12 && lag1 < 5.5 * kD && end1 < 0.01 * kD && view1 < 1e-12 && endB < 0.01 * kD && viewB < 1e-12 &&
                  endC < 0.01 * kD && viewC < 1e-12,
              "F109 follower: with no smoothing the aim trails a 30 deg/s head by under 0.75 deg and lands within 0.01 deg; at 150 ms it trails under 5.5 deg and "
              "lands; asks the game dropped for 12 ticks are asked again and the aim lands; a 30 deg centring lands (at most 180 deg/s); the view is the head's "
              "throughout (the cancel is the aim's turn)");
    }
    {   // the per-second check and the aim's wrapped step
        check(f109::turnAgrees(10.0, 9.0) && !f109::turnAgrees(10.0, 0.0) && f109::turnAgrees(40.0, 32.0) && !f109::turnAgrees(40.0, 25.0) &&
                  std::fabs(f109::aimStep(3.1, -3.1) - (2.0 * f109::kPi - 6.2)) < 1e-12 && f109::aimStep(0.5, std::numeric_limits<double>::quiet_NaN()) == 0.0,
              "F109 check: a 10 deg player's turn agrees with a 9 deg view turn and not with none (the stick cancelled); 25% tolerance for large turns; the aim's step "
              "wraps across pi");
    }
}

}  // namespace f109test
