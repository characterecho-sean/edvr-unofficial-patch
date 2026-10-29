// F167 checks (native on-foot VR), run167 below. Header only, run by native_frame_test.
//
// F167 (native_on_foot_f167.h): the left hand lets go of the pistol while sprinting. The release weight is a factor on the off hand's weight (1 less the release), so at 1 the left arm is
// not solved and the game's own animated arm draws, and in between the solve blends from the animation to the grip. The sprint is the pin's low-passed AMC speed; its figures are the
// flight edvr_gfx_20261028_113409 (a submachine gun sprint, the F85a events 6 to 16 and 27 to 36, the run in events 25 and 37), every third sample of the 0.15 s low-pass of the events'
// own speeds: the sprints hold 5.4 to 8.5 m/s, the runs stay under 3.0 m/s.
#pragma once

#include "../../src/d3d11/native_on_foot_core.h"
#include "../../src/d3d11/native_on_foot_f167.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace f167test {

namespace nf = edvr::nofoot;
namespace f167 = edvr::nofoot::f167;

struct Frame {
    bool pistol = true, armed = true, fresh = true;
    double speed = 0.0, detach = 0.0;
    uint32_t item = 1;
};
// A scripted run: the state and its clock, stepped at dt.
struct Run {
    f167::State state;
    double dt = 0.01;
    f167::Step last;
    f167::Step step(const Frame& f) {
        f167::In in;
        in.pistol = f.pistol;
        in.armed = f.armed;
        in.speedFresh = f.fresh;
        in.speed = f.speed;
        in.detachW = f.detach;
        in.dtSec = dt;
        in.item = f.item;
        last = state.step(in);
        return last;
    }
    // n steps at one frame; the release after the last
    double hold(const Frame& f, int n) {
        for (int i = 0; i < n; ++i) step(f);
        return last.release;
    }
};

template <class Check>
inline void run167(Check&& check) {
    const auto eq = [](double a, double b, double tol) { return std::fabs(a - b) <= tol; };
    Frame sprint;
    sprint.speed = 6.5;
    Frame walk;
    walk.speed = 1.5;

    // ---- 1. the sprint signal: the flights' numbers, the dead band ----------------------------------------------------------------------------------------------------------------
    {
        // every third sample of the low-passed speeds, per event of the flight (m/s)
        const double sprintRunUp[] = {4.4, 5.1, 5.5, 5.6, 5.9, 6.2, 6.9, 7.0, 7.3, 7.3, 7.3};                    // event 6: from the run into a sprint
        const double sprintLow[] = {5.5, 5.6, 5.6, 5.8, 5.9, 5.7, 5.7, 5.8, 5.8, 5.8, 5.9, 5.9};                 // event 35: the slowest sprint plateau
        const double sprintLow2[] = {5.5, 5.7, 5.8, 5.7, 5.7, 5.9, 6.0, 6.0, 6.0, 6.0, 6.2, 6.2};                 // event 33
        const double sprintFast[] = {8.6, 8.2, 7.8, 7.5, 7.4, 7.2, 7.0, 7.0, 7.1, 6.7, 7.0, 6.8};                // event 9: the fastest
        const double runA[] = {3.0, 2.9, 2.9, 2.9, 2.8, 1.1, 0.5, 1.0, 2.1, 0.4, 0.4, 0.3};                      // event 25: a run, then stopping
        const double runB[] = {1.5, 1.6, 1.9, 2.2, 2.4, 2.7, 2.5, 2.5, 2.4, 2.3, 2.3, 2.2};                      // event 37
        const double walkUp[] = {0.0, 0.0, 0.1, 0.2, 0.4, 0.7, 1.1, 1.8, 2.4, 3.1, 3.7};                         // event 26's first half: from standing to a run
        const auto replay = [](const double* v, size_t n, bool* ever, bool* always, bool from) {
            bool s = from;
            *ever = false;
            *always = true;
            for (size_t i = 0; i < n; ++i) {
                s = f167::sprintingNow(s, v[i]);
                *ever = *ever || s;
                *always = *always && s;
            }
        };
        bool ever = false, always = false;
        replay(sprintLow, sizeof(sprintLow) / sizeof(sprintLow[0]), &ever, &always, false);
        bool allSprint = ever && always;
        replay(sprintLow2, sizeof(sprintLow2) / sizeof(sprintLow2[0]), &ever, &always, false);
        allSprint = allSprint && ever && always;
        replay(sprintFast, sizeof(sprintFast) / sizeof(sprintFast[0]), &ever, &always, false);
        allSprint = allSprint && ever && always;
        check(allSprint, "F167: the flight's sprint plateaus (5.4 to 8.6 m/s low-passed) read as a sprint from the first sample to the last");
        replay(sprintRunUp, sizeof(sprintRunUp) / sizeof(sprintRunUp[0]), &ever, &always, false);
        const bool startsAt = ever && !f167::sprintingNow(false, 4.4) && f167::sprintingNow(false, 5.1);
        bool stays = true, s = false;
        for (const double v : sprintRunUp) {
            const bool was = s;
            s = f167::sprintingNow(s, v);
            stays = stays && !(was && !s);   // once begun it does not stop on the way up
        }
        check(startsAt && stays, "F167: the run into a sprint (4.4 m/s, then 5.1) begins the sprint at the 5.1 sample and does not drop out");
        bool runsFree = true;
        replay(runA, sizeof(runA) / sizeof(runA[0]), &ever, &always, false);
        runsFree = runsFree && !ever;
        replay(runB, sizeof(runB) / sizeof(runB[0]), &ever, &always, false);
        runsFree = runsFree && !ever;
        replay(walkUp, sizeof(walkUp) / sizeof(walkUp[0]), &ever, &always, false);
        runsFree = runsFree && !ever;
        check(runsFree, "F167: the flight's runs (up to 3.0 m/s low-passed) and the walk up to them are never a sprint");
        check(!f167::sprintingNow(false, f167::kEnterMps - 1e-9) && f167::sprintingNow(false, f167::kEnterMps) && f167::sprintingNow(true, f167::kExitMps) &&
                  !f167::sprintingNow(true, f167::kExitMps - 1e-9) && f167::kExitMps < f167::kEnterMps,
              "F167: the sprint begins at 4.6 m/s and ends under 3.9 m/s; between the two the state holds");
        bool wobble = true, a = false, b = true;
        for (int i = 0; i < 200; ++i) {
            const double v = 3.95 + 0.6 * (i % 2);   // 3.95 and 4.55: both inside the dead band
            a = f167::sprintingNow(a, v);
            b = f167::sprintingNow(b, v);
            wobble = wobble && !a && b;
        }
        check(wobble, "F167: a speed wobbling inside the dead band (3.95 to 4.55 m/s) keeps whatever state it was in: no flapping in either direction");
        check(!f167::sprintingNow(true, std::nan("")) && !f167::sprintingNow(false, std::nan("")) && !f167::sprintingNow(true, INFINITY) && !f167::sprintingNow(false, INFINITY),
              "F167: a speed that is not a number, or not finite, is not a sprint, whatever the state before it");
    }

    // ---- 2. the weight: a rifle at any speed, a pistol walking, a pistol sprinting ----------------------------------------------------------------------------------------------
    {
        bool rifleFree = true;
        for (double v = 0.0; v <= 14.0; v += 0.5) {
            Run r;
            Frame f = sprint;
            f.pistol = false;
            f.speed = v;
            rifleFree = rifleFree && r.hold(f, 300) == 0.0 && !r.last.sprinting;
        }
        check(rifleFree, "F167: any other item (a rifle, a shotgun, a launcher, a tool) has release 0 at every speed from standing to 14 m/s, for 3 s each");
        Run walking;
        check(walking.hold(walk, 300) == 0.0 && !walking.last.sprinting, "F167: a pistol walking (1.5 m/s) for 3 s has release 0");
        Frame run;
        run.speed = 3.0;
        Run running;
        check(running.hold(run, 300) == 0.0, "F167: a pistol run (3.0 m/s) for 3 s has release 0");
        Run sprinting;
        const double after = sprinting.hold(sprint, 100);
        check(after == 1.0 && sprinting.last.sprinting && f167::gripFactor(after) == 0.0, "F167: a pistol sprinting (6.5 m/s) for 1 s has release 1 and the grip factor 0");
        Run unarmed;
        Frame f = sprint;
        f.armed = false;
        check(unarmed.hold(f, 100) == 0.0, "F167: a pistol that is not drawn has release 0 whatever the speed");
        Run stale;
        f = sprint;
        f.fresh = false;
        check(stale.hold(f, 100) == 0.0 && !stale.last.sprinting, "F167: a speed older than the pin's last half second is not a sprint: the hand keeps the grip");
        // the factor at release 0 is exactly 1: the earlier solve bit for bit
        const double offW = 0.7310585786;
        check(offW * f167::gripFactor(0.0) == offW && f167::gripFactor(1.0) == 0.0 && f167::gripFactor(std::nan("")) == 1.0 && f167::gripFactor(-3.0) == 1.0 && f167::gripFactor(7.0) == 0.0 &&
                  eq(f167::gripFactor(0.25), 0.75, 1e-15),
              "F167: the factor on the off hand's weight is 1 less the release: exactly 1 at 0 (the earlier solve bit for bit), 0 at 1, held to 0..1, and 1 for a figure that is not a number");
        // at release 1 the left arm's solve weight is exactly 0 (the side is neither solved nor written: the game's arm draws), at release 0 what it was
        const double envelope = 1.0;
        const double wFull = envelope * (offW * f167::gripFactor(1.0)), wNone = envelope * (offW * f167::gripFactor(0.0));
        check(wFull == 0.0 && !(wFull > 0.0) && wNone == offW && eq(nf::ikOffHandReachRadius(0.60, offW * f167::gripFactor(1.0)), 0.60 - nf::kReachMarginM + nf::kOffReachRelaxM, 1e-12),
              "F167: at release 1 the left arm's weight (the armed envelope x the off hand's weight x the factor) is exactly 0, so the side is not written, and the reach rule relaxes as for a free off hand");
    }

    // ---- 3. the ramps' times ---------------------------------------------------------------------------------------------------------------------------------------------------
    {
        Run r;
        r.dt = 0.0125;
        const double at4 = r.hold(sprint, 4), at8 = r.hold(sprint, 4), at12 = r.hold(sprint, 4), at16 = r.hold(sprint, 4), at20 = r.hold(sprint, 4);
        check(eq(at4, 0.25, 1e-12) && eq(at8, 0.5, 1e-12) && eq(at12, 0.75, 1e-12) && at16 == 1.0 && at20 == 1.0,
              "F167: the release rises 0 to 1 in 0.2 s while the sprint holds (0.25 at 50 ms, 0.5 at 100 ms, 1 at 200 ms), then stays");
        // the hand returns over 0.25 s
        Frame slow = walk;
        const double b4 = r.hold(slow, 4), b8 = r.hold(slow, 4), b12 = r.hold(slow, 4), b16 = r.hold(slow, 4), b20 = r.hold(slow, 4);
        check(eq(b4, 0.8, 1e-12) && eq(b8, 0.6, 1e-12) && eq(b12, 0.4, 1e-12) && eq(b16, 0.2, 1e-12) && eq(b20, 0.0, 1e-12) && f167::kRegripSec == 0.25 && f167::kReleaseSec == 0.20,
              "F167: once the sprint ends the release falls 1 to 0 in 0.25 s (0.8 at 50 ms, 0.6 at 100 ms, 0.4 at 150 ms, 0.2 at 200 ms, 0 at 250 ms)");
        // a sprint that ends half way up turns round from where it is
        Run half;
        half.dt = 0.0125;
        half.hold(sprint, 8);   // 0.5
        const double down = half.hold(slow, 5);   // 0.5 - 5 x 0.05 = 0.25
        const double up = half.hold(sprint, 4);   // 0.25 + 4 x 0.0625 = 0.5
        check(eq(down, 0.25, 1e-12) && eq(up, 0.5, 1e-12), "F167: a sprint that ends half way up returns from where it is, not from 1, and a new sprint rises from there");
        // a long gap steps by 0.1 s at most
        Run gap;
        gap.dt = 5.0;
        gap.step(sprint);
        check(eq(gap.last.release, 0.5, 1e-12), "F167: a call after a long gap steps by 0.1 s: a stall does not fling the hand to the sprint at once");
        Run zero;
        zero.dt = 0.0;
        check(zero.hold(sprint, 50) == 0.0 && zero.last.sprinting, "F167: no time passed, no ramp");
    }

    // ---- 4. continuity ---------------------------------------------------------------------------------------------------------------------------------------------------------
    {
        // a scripted pilot: stand, run, sprint, wobble in the band, stop, sprint again, press the sights while sprinting, leave them, stop; at 90 Hz
        Run r;
        r.dt = 1.0 / 90.0;
        double prev = 0.0, worst = 0.0;
        int flips = 0;
        bool prevSprint = false;
        const double rampStep = r.dt / f167::kReleaseSec;   // the fastest ramp's step
        double prevDetach = 0.0;
        bool smooth = true;
        const auto drive = [&](double speed, double detach, double seconds) {
            Frame f = sprint;
            f.speed = speed;
            f.detach = detach;
            const int n = static_cast<int>(seconds * 90.0 + 0.5);
            for (int i = 0; i < n; ++i) {
                r.step(f);
                const double d = std::fabs(r.last.release - prev);
                worst = d > worst ? d : worst;
                smooth = smooth && d <= rampStep + std::fabs(detach - prevDetach) + 1e-12;   // the ramp's step, and the detach weight's own where the sights move the cap
                if (r.last.sprinting != prevSprint) ++flips;
                prevSprint = r.last.sprinting;
                prev = r.last.release;
                prevDetach = detach;
            }
        };
        drive(0.0, 0.0, 1.0);
        drive(3.0, 0.0, 1.0);
        for (int i = 0; i < 6; ++i) drive(4.0 + 0.2 * (i % 2), 0.0, 0.2);    // 4.0 and 4.2: inside the band
        drive(6.5, 0.0, 1.5);
        for (int i = 0; i < 10; ++i) drive(4.1 + 0.4 * (i % 2), 0.0, 0.1);   // 4.1 and 4.5 in the band while sprinting
        drive(0.5, 0.0, 1.0);
        drive(6.5, 0.0, 0.3);   // sprint again, the hand half way up
        // the sights come up over 0.15 s while sprinting and the release gives way to the grip
        for (int i = 0; i < 14; ++i) drive(6.5, (i + 1) / 14.0, 0.15 / 14.0);
        drive(6.5, 1.0, 1.0);
        for (int i = 0; i < 14; ++i) drive(6.5, 1.0 - (i + 1) / 14.0, 0.15 / 14.0);   // and leave them
        drive(6.5, 0.0, 1.0);
        drive(0.0, 0.0, 1.0);
        check(smooth && worst > 0.0, "F167: through a run, a sprint, a wobble in the dead band, a stop, a second sprint, the sights up and down and a stop, the release never moves by more than one 0.2 s ramp step "
                                     "(at 90 Hz) plus the detach weight's own step");
        check(flips == 4, "F167: the same script flips the sprint state four times (on and off for each of the two sprints) and not once for the band's wobble or the sights");
    }

    // ---- 5. the sights hold the grip -------------------------------------------------------------------------------------------------------------------------------------------------
    {
        Run r;
        r.dt = 0.01;
        r.hold(sprint, 30);   // release 1 at the hip
        Frame ads = sprint;
        ads.detach = 1.0;
        const double inSights = r.hold(ads, 1);
        check(inSights == 0.0 && r.last.sprinting && eq(r.last.ramp, 1.0, 1e-12) && f167::gripFactor(inSights) == 1.0,
              "F167: in the sights (detach 1) the grip is held (release 0, the factor 1) while the sprint state and its ramp carry on");
        Frame half = sprint;
        half.detach = 0.4;
        const double part = r.hold(half, 1);
        check(eq(part, 0.6, 1e-12), "F167: with the sights 40% up the release is capped at 60%");
        half.detach = 0.5;
        check(eq(r.hold(half, 1), 0.5, 1e-12), "F167: with the sights half up the release is capped at 50%");
        Frame hip = sprint;
        check(r.hold(hip, 1) == 1.0, "F167: leaving the sights returns the release to the ramp, which was waiting at 1 (a sprint that never stopped)");
        // sights up before the sprint starts: the sprint's ramp climbs under the cap, the grip stays until the sights come down
        Run early;
        early.dt = 0.01;
        const double held = early.hold(ads, 100);
        check(held == 0.0 && early.last.sprinting && early.last.ramp == 1.0, "F167: a sprint begun in the sights holds the grip for as long as the sights are up (release 0 after 1 s)");
        double leaving = 0.0;
        for (int i = 1; i <= 5; ++i) {   // the sights come down over 10 steps; half way the release is the cap's 0.5, the ramp having waited at 1
            Frame f = sprint;
            f.detach = 1.0 - 0.1 * i;
            leaving = early.hold(f, 1);
        }
        check(eq(leaving, 0.5, 1e-9), "F167: ... and as the sights come down the release is the cap's own rise (half way down, 0.5), the ramp having waited at 1");
        // the cap and the ramp together never raise the release above either
        bool bound = true;
        for (double d = 0.0; d <= 1.0; d += 0.05) {
            Run q;
            q.dt = 0.01;
            Frame f = sprint;
            f.detach = d;
            q.hold(f, 40);
            bound = bound && q.last.release <= 1.0 - d + 1e-12 && q.last.release <= q.last.ramp + 1e-12;
        }
        check(bound, "F167: the release never exceeds 1 less the detach weight, nor the ramp");
    }

    // ---- 6. a new pistol, a put-away pistol, another item --------------------------------------------------------------------------------------------------------------------------
    {
        Run r;
        r.dt = 0.01;
        r.hold(sprint, 30);
        Frame other = sprint;
        other.item = 2;
        check(eq(r.hold(other, 1), r.dt / f167::kReleaseSec, 1e-12) && r.last.sprinting,
              "F167: another pistol in hand starts over: the grip, then its own ramp from 0 (one step up at a 6.5 m/s sprint), not the last pistol's release");
        Run swap;
        swap.dt = 0.01;
        swap.hold(sprint, 30);
        Frame rifle = sprint;
        rifle.pistol = false;
        check(swap.hold(rifle, 1) == 0.0 && !swap.last.sprinting, "F167: a pistol swapped for a rifle mid-sprint has release 0 at once, whatever the speed");
        Frame back = sprint;
        check(eq(swap.hold(back, 1), swap.dt / f167::kReleaseSec, 1e-12), "F167: ... and drawing the pistol again starts the ramp from 0");
        Run holster;
        holster.dt = 0.01;
        holster.hold(sprint, 30);
        Frame away = sprint;
        away.armed = false;
        check(holster.hold(away, 1) == 0.0 && !holster.last.sprinting, "F167: a pistol put away has release 0 at once");
    }

    // ---- 7. the counters, the seconds and the line ------------------------------------------------------------------------------------------------------------------------------------
    {
        f167::Counters c;
        const auto add = [&c](double speed, double release, double detach, double leftW) {
            f167::Sample s;
            s.speed = speed;
            s.release = release;
            s.detachW = detach;
            s.leftW = leftW;
            c.add(s);
        };
        for (int i = 0; i < 20; ++i) add(1.5, 0.0, 0.0, 1.0);       // walking
        for (int i = 0; i < 10; ++i) add(std::nan(""), 0.0, 0.0, 1.0);   // a stale speed
        for (int i = 0; i < 16; ++i) add(6.0, i / 16.0, 0.0, 1.0 - i / 16.0);   // the ramp up: half of them released
        for (int i = 0; i < 40; ++i) add(6.5, 1.0, 0.0, 0.0);       // fully released, the left arm not written
        for (int i = 0; i < 3; ++i) add(6.5, 1.0, 0.0, 0.9);        // fully released and the left arm still solved: a defect
        for (int i = 0; i < 12; ++i) add(6.0, 0.0, 1.0, 1.0);       // sprinting in the sights, the grip held
        for (int i = 0; i < 2; ++i) add(6.0, 0.8, 0.7, 0.2);        // in the sights, released past the cap: a defect
        check(c.pistol == 103 && c.fast == 16 + 40 + 3 + 12 + 2 && c.hip == 59 && c.ads == 14 && c.adsGripped == 12 && c.adsReleased == 2 && c.full == 43 && c.stillGripping == 3 &&
                  c.released == 8 + 43,
              "F167: the counters: armed pistol calls, those at sprint speed (a walk and a stale speed are not), at the hip against in the sights, released (0.5 or more) and fully, the left arm "
              "still solved at full release, the grip held in the sights and the release past what the sights allow");
        check(eq(c.speedMax, 6.5, 1e-12) && eq(c.speedSum, 16 * 6.0 + 43 * 6.5 + 14 * 6.0, 1e-9) && eq(c.releaseSum, (0 + 15) * 16 / 2 / 16.0 + 43.0, 1e-9),
              "F167: the speed's mean and largest over the sprint calls, the release's mean over the calls at the hip");
        f167::Counters other;
        for (int i = 0; i < 5; ++i) {
            f167::Sample s;
            s.speed = 9.0;
            s.release = 1.0;
            other.add(s);
        }
        f167::Counters sum = c;
        sum.merge(other);
        check(sum.pistol == 108 && sum.fast == c.fast + 5 && sum.speedMax == 9.0 && sum.hip == 64 && sum.full == 48 && eq(sum.speedSum, c.speedSum + 45.0, 1e-9),
              "F167: two windows add up, the speed's largest the larger");
        // a release that never ran: the stage off, sprint speeds and no release
        f167::Counters dead;
        for (int i = 0; i < 100; ++i) {
            f167::Sample s;
            s.speed = 6.5;
            s.release = 0.0;
            s.leftW = 1.0;
            dead.add(s);
        }
        check(dead.fast == 100 && dead.hip == 100 && dead.released == 0 && dead.full == 0,
              "F167: a sprint with the release never applied counts as sprint calls at the hip with none released: the signature the health rule calls NEVER APPLIED");
        // the seconds
        f167::Seconds sec;
        f167::Counters few;
        for (int i = 0; i < 9; ++i) {
            f167::Sample s;
            s.speed = 6.5;
            s.release = 1.0;
            few.add(s);
        }
        f167::Counters mostly;
        for (int i = 0; i < 20; ++i) {
            f167::Sample s;
            s.speed = 6.5;
            s.release = i < 12 ? 1.0 : 0.0;
            mostly.add(s);
        }
        f167::Counters thin;
        for (int i = 0; i < 20; ++i) {
            f167::Sample s;
            s.speed = 6.5;
            s.release = i < 3 ? 1.0 : 0.0;
            thin.add(s);
        }
        sec.fold(few);
        sec.fold(mostly);
        sec.fold(thin);
        sec.fold(dead);
        sec.fold(c);
        check(sec.sprint == 4 && sec.released == 2, "F167: a second counts from 10 sprint calls at the hip, and is released when half or more of them are");
        // the line
        char line[1400];
        const int len = f167::formatSummary(line, sizeof(line), 10, "$wpn_s_pistol_kinetic_sauto_name;", c, sec);
        f167::Counters big;
        big.pistol = big.fast = big.ads = big.adsGripped = big.adsReleased = big.hip = big.released = big.full = big.stillGripping = 4000000000u;
        big.speedSum = 4.0e19;
        big.speedMax = 1.0e9;
        big.releaseSum = 4.0e9;
        big.leftWSum = 4.0e9;
        f167::Seconds bigSec;
        bigSec.sprint = bigSec.released = 4000000000u;
        char worst[1400];
        const int worstLen = f167::formatSummary(worst, sizeof(worst), 4000000000u, "$wpn_s_pistol_kinetic_sauto_name_with_a_long_tail_for_the_worst_case;", big, bigSec);
        check(len > 0 && len < 800 && worstLen > 0 && worstLen < 1190 &&
                  std::strstr(line, "native on foot pistol sprint release (F167) 10 s: item $wpn_s_pistol_kinetic_sauto_name;; armed pistol calls 103, ") != nullptr,
              "F167: the window's line names the tag, the pistol, the calls at sprint speed, the release at the hip and in the sights and the seconds, within the log's 1200 bytes at its worst");
        check(std::strstr(line, "at sprint speed (4.6 m/s or more) 73") != nullptr && std::strstr(line, "at the hip 59: release weight mean") != nullptr &&
                  std::strstr(line, "(the left arm still solved above weight 0.05 in 3)") != nullptr && std::strstr(line, "in the sights 14: grip held in 12, released past the sights' hold in 2") != nullptr &&
                  std::strstr(line, "seconds sprinting at the hip 4, released in half or more of them 2") != nullptr,
              "F167: the line carries the sprint threshold, the counts at the hip and in the sights, the arm still solved at full release and the seconds");
    }
}

}  // namespace f167test
