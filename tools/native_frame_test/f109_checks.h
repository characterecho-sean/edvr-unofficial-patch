// F109 checks (native on-foot VR): look to aim on foot, the player's own turn in ADS, snap or smooth turning. Header only, run by native_frame_test (run109).
// The game's figures are build 334925's as the RE reads them (analysis\decomp\verify_20261025_onfoot_look_input.txt): the processor's delta is
// c * (dt * rate * stick + g * (mouse + in) + dt * spring), linear in the mouse path, and the aim takes it whole in the call.
#pragma once

#include "../../src/d3d11/native_on_foot_core.h"
#include "../../src/d3d11/native_on_foot_f109.h"

#include <algorithm>
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
    // F109b: the guard's three gates (nofoot::AdsResidual). Axes: yaw about the world's up (+ = left of +z), then pitch; forward (-sy cp, sp, cy cp), right
    // (cy, 0, sy), up (sy sp, cp, -cy sp).
    const auto axesAt = [](double yaw, double pitch, double r[3], double u[3], double f[3]) {
        const double cy = std::cos(yaw), sy = std::sin(yaw), cp = std::cos(pitch), sp = std::sin(pitch);
        f[0] = -sy * cp; f[1] = sp; f[2] = cy * cp;
        r[0] = cy; r[1] = 0.0; r[2] = sy;
        u[0] = sy * sp; u[1] = cp; u[2] = -cy * sp;
    };
    {   // the first trip: a smooth stick turn of 191 deg from a base pitched -8 deg, the head still. Read in the base's axes that turn is about -16 deg of pitch
        // (the F109 flight's -12.2 at 07:19:25); the player's own turn past 15 deg takes a new base, so it never trips
        nofoot::AdsResidual res;
        res.causal = true;
        double vr[3], vu[3], vf[3], hr[3], hu[3], hf[3];
        const double p = -8.0 * kD;
        axesAt(0.0, p, hr, hu, hf);
        nofoot::RotFrameSum base;
        axesAt(0.0, p, vr, vu, vf);
        base.add(vr, vu, vf);
        bool trip = false;
        uint64_t ms = 10000;
        const int n = 270;   // 3 s at 90 Hz
        for (int i = 0; i <= n; ++i, ms += 11) {
            if (i > 0) res.addUser(191.0 * kD / n, 0.0);
            axesAt(191.0 * kD * i / n, p, vr, vu, vf);
            trip = trip || res.step(ms, true, vr, vu, vf, true, hr, hu, hf, true, true) == nofoot::AdsResidual::kTrip;
        }
        nofoot::RotFrameSum now;
        now.add(vr, vu, vf);
        double ay = 0.0, ap = 0.0;
        nofoot::rotFrameDelta(base, now, &ay, &ap);
        check(!trip && res.userRebases >= 10 && ap < -15.0 && ap > -17.0 && res.maxAbs < 0.5,
              "F109b guard: a 191 deg turn of the player's own from a base pitched -8 deg reads about -16 deg of pitch in the base's axes (the first F109 trip); "
              "a new base every 15 deg of the player's own turn keeps the residual under 0.5 deg, and it never trips");
    }
    {   // the second trip: the game pitches the view 18 deg down in 0.3 s (a jump) with the head still and next to nothing asked: causal holds it, a new base
        nofoot::AdsResidual res, old;
        res.causal = true;
        double vr[3], vu[3], vf[3], hr[3], hu[3], hf[3];
        axesAt(0.0, 0.0, hr, hu, hf);
        bool tripRes = false, tripOld = false, held = false;
        uint64_t ms = 10000;
        for (int i = 0; i <= 120; ++i, ms += 11) {
            const double pitch = -18.0 * kD * std::min(1.0, i / 27.0);
            axesAt(0.0, pitch, vr, vu, vf);
            res.addAsk(0.01 * kD, -0.01 * kD);
            const nofoot::AdsResidual::Result r = res.step(ms, true, vr, vu, vf, true, hr, hu, hf, true, true);
            tripRes = tripRes || r == nofoot::AdsResidual::kTrip;
            held = held || r == nofoot::AdsResidual::kHeld;
            tripOld = tripOld || old.step(ms, true, vr, vu, vf, true, hr, hu, hf, true, true) == nofoot::AdsResidual::kTrip;
        }
        check(tripOld && !tripRes && held && res.holds == 1 && res.heldRes[1] < -17.0 && std::fabs(res.heldAsk[1]) < 1.0 && res.maxAbs > 17.0,
              "F109b guard: a view pitched 18 deg by the game with next to nothing asked trips the guard without the causal gate (F109's second trip) and is held "
              "with it (one hold, residual pitch under -17 deg against asks under 1 deg), then measured from a new base");
    }
    {   // a real runaway still trips with causal on: the camera does not take the aim (the view stays, the residual is minus the asks), or the sign is inverted
        // (the view turns against the head, the residual about twice the asks)
        for (int mode = 0; mode < 2; ++mode) {
            nofoot::AdsResidual res;
            res.causal = true;
            double vr[3], vu[3], vf[3], hr[3], hu[3], hf[3];
            bool trip = false;
            uint64_t ms = 10000, tripMs = 0;
            double head = 0.0, asked = 0.0;
            for (int i = 0; i <= 720; ++i, ms += 11) {
                const double step = 0.35 * kD;   // 31.5 deg/s to the left
                if (i > 0) {
                    head += step;
                    asked += step;
                    res.addAsk(step, 0.0);
                }
                axesAt(head, 0.0, hr, hu, hf);
                axesAt(mode == 0 ? head - asked : head - 2.0 * asked, 0.0, vr, vu, vf);
                const nofoot::AdsResidual::Result r = res.step(ms, true, vr, vu, vf, true, hr, hu, hf, true, true);
                if (!trip && r == nofoot::AdsResidual::kTrip) {
                    trip = true;
                    tripMs = ms;
                }
            }
            check(trip && tripMs < 10000 + 2000 && res.holds == 0,
                  mode == 0 ? "F109b guard: with the causal gate on, a camera that does not take the aim (the residual minus the asks) still trips within 2 s, never held"
                            : "F109b guard: with the causal gate on, an inverted sign (the residual twice the asks) still trips within 2 s, never held");
        }
    }
    {   // a rows gap restarts the growth run: a run 300 ms in, then 300 ms without fresh rows, does not trip on the first fresh row (the run's 0.5 s was not seen)
        nofoot::AdsResidual res;
        double vr[3], vu[3], vf[3], hr[3], hu[3], hf[3];
        axesAt(0.0, 0.0, vr, vu, vf);   // the camera does not take the aim: the view stays, the residual is minus the head
        uint64_t ms = 10000, runAt = 0, gapFrom = 0, gapEnd = 0, tripMs = 0;
        double head = 0.0;
        for (int i = 0; i <= 400; ++i, ms += 11) {
            head += 0.35 * kD;
            axesAt(head, 0.0, hr, hu, hf);
            bool rowsOk = true;
            if (runAt != 0 && ms >= runAt + 300 && gapEnd == 0) {
                if (gapFrom == 0) gapFrom = ms;
                if (ms < gapFrom + 300) rowsOk = false;
                else gapEnd = ms;
            }
            const nofoot::AdsResidual::Result r = res.step(ms, rowsOk, vr, vu, vf, true, hr, hu, hf, true, true);
            if (runAt == 0 && res.growth.growing) runAt = ms;
            if (tripMs == 0 && r == nofoot::AdsResidual::kTrip) tripMs = ms;
        }
        check(res.gapRestarts == 1 && gapEnd != 0 && tripMs >= gapEnd + 500 && tripMs <= gapEnd + 520,
              "F109b guard: a growth run with 300 ms of no fresh rows in it restarts at the first fresh row (gap restarts 1) and trips only 0.5 s after it, not on "
              "the time it did not see");
    }
    // F109c (the F109b flight, edvr_gfx_20261025_085120, the Arc Cutter at 08:57:09-12): the gain only between the game's two ends, and one pitch account.
    {   // the split with the ends read (hip 1.0, ADS 0.85335): the flight's foreign ratios are not taken, the gain stays, the call's delta is still the head aim's
        const double lo = 1.0, hi = 0.85335;
        auto clean = [&](f109::Split& s, double in, double d) {
            f109::SplitIn c;
            c.in = in;
            c.d = d;
            c.gainLo = lo;
            c.gainHi = hi;
            return s.step(c);
        };
        f109::Split s;
        const f109::SplitOut hip = clean(s, 0.01, 0.01);
        const f109::SplitOut ads = clean(s, 0.01, 0.01 * hi * 0.99);
        const f109::SplitOut a = clean(s, 0.001, 0.001 * 741.12622), b = clean(s, 0.001, -0.001 * 405.42361), c = clean(s, 0.001, 0.001 * 22.21323);
        const f109::SplitOut tiny = clean(s, 0.01, 0.01 * 0.002), flip = clean(s, 0.01, -0.01 * 0.9);
        f109::Split old;
        f109::SplitIn u;
        u.in = 0.001;
        u.d = 0.001 * 741.12622;
        const f109::SplitOut unread = old.step(u);
        const double askRad = 2.0 * kD;
        check(hip.gainTaken && ads.gainTaken && std::fabs(s.gain - hi * 0.99) < 1e-12 && !a.gainTaken && a.foreign && !b.gainTaken && b.foreign && !c.gainTaken &&
                  c.foreign && !tiny.gainTaken && tiny.foreign && !flip.gainTaken && flip.foreign && std::fabs(a.ours - 0.001 * 741.12622) < 1e-12 &&
                  unread.gainTaken && std::fabs(old.gain - 741.12622) < 1e-9 && std::fabs(nofoot::adsInput(askRad, 0.002) - 500.0 * askRad) < 1e-9 &&
                  std::fabs(nofoot::adsInput(askRad, s.gain) - askRad / (hi * 0.99)) < 1e-12,
              "F109c split: with the ends read (1.0, 0.85335) a clean call takes the hip and the ADS gain; the flight's 741.1, -405.4 and 22.2, a 0.002 and a "
              "sign flip are foreign and not taken (the gain stays 0.845, the call's delta still the head aim's); with the ends unread F109's window took 741; a "
              "0.002 gain would have sent the next 2 deg ask as 1000 deg of input");
    }
    {   // one pitch account: the game turns the aim +10 deg a tick on its own for 10 ticks (counted as applied, as a clean call counts it) against the follower's
        // asks, so the aim goes past 60; the follower brings it back, then the head pitches down 30 deg. The account is the aim's turn throughout, its publication
        // held to +-60; F68a's clamped account (computed alongside, the control) is left as far from the aim as the turn went past 60, which is what parted the
        // follower from the map's limit in the flight.
        nofoot::AdsAimMap map;
        f109::Follower fo;
        double pending = 0.0, aim = 0.0, head = 0.0, oldSeat = 0.0, oldErr = 0.0, seatMax = 0.0, pubMax = 0.0, accErr = 0.0;
        const double dt = 1.0 / 90.0;
        for (int k = 0; k < 900; ++k) {
            const double dHead = (k >= 400 && k < 490) ? -30.0 * kD / 90.0 : 0.0;
            head += dHead;
            const double game = (k >= 90 && k < 100) ? 10.0 * kD : 0.0;
            const double applied = pending + game;
            fo.addHead(0.0, dHead);
            fo.addApplied(0.0, applied);
            const double ifp = f109::inFlightAfter(map.flightPitch, applied), ify = f109::inFlightAfter(map.flightYaw, 0.0);
            double askY = 0.0, askP = 0.0;
            fo.ask(dt, 0.0, ify, ifp, &askY, &askP);
            const nofoot::AdsAimMap::Out mo = map.step(true, false, dt, 1.0, askP != 0.0, 0.0, askP, 0.0, applied, true, true);
            aim += applied;
            oldSeat = nofoot::adsClampPitch(oldSeat + applied);
            if (k == 399) oldErr = oldSeat - aim;
            pending = mo.injPitch;
            seatMax = std::max(seatMax, map.pitchSeat);
            pubMax = std::max(pubMax, map.pitchPublished());
            accErr = std::max(accErr, std::fabs(map.pitchSeat - aim));
        }
        check(seatMax > 75.0 * kD && pubMax <= nofoot::kAdsPitchClampRad + 1e-12 && accErr < 1e-12 && std::fabs(fo.applied[1] - map.pitchSeat) < 1e-12 &&
                  std::fabs(head - aim) < 0.05 * kD && std::fabs(oldErr + (seatMax - nofoot::kAdsPitchClampRad)) < 1e-9,
              "F109c account: a turn of the game's own to past 75 deg is carried whole (the publication held to 60), the follower's sum and the map's account "
              "agree, the aim comes back and follows the head 30 deg down; F68a's clamped account is left as far from the aim as the turn went past 60");
    }
}

}  // namespace f109test
