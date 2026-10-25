// F123 checks (native on-foot VR): head containment, the leash and the wall contact. Header only, run by native_frame_test (run123). The fade ramp, the hard timer, the
// recentre sequence (fade out, ask, wait for the head to come inside soft, fade in), the contact trigger and its latch and release, the recentre once for a latch, the levels
// combined by max, everything cleared when the camera leaves foot, the episodes and the lines.
#pragma once

#include "../../src/d3d11/native_on_foot_f123.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace f123test {

namespace f123 = edvr::nofoot::f123;

struct Sim {
    f123::Machine m;
    uint32_t frame = 100;
    double t = 0.0;
    std::vector<f123::Episode> episodes;
    unsigned asks = 0, dones = 0, fails = 0, afters = 0;
    double askedAt = -1.0, doneAt = -1.0, maxLevel = 0.0;
    uint32_t lastWord = 0;
    f123::Out last;
    static constexpr double dt = 1.0 / 90.0;
    Sim() { m.gate(true); }
    f123::Out step(double h, double up, bool have = true, bool active = true) {
        f123::In in;
        in.dt = dt;
        in.active = active;
        in.haveOffset = have;
        in.hCm = h;
        in.upCm = up;
        last = m.step(in);
        t += dt;
        if (last.asked) {
            ++asks;
            askedAt = t;
        }
        if (last.done) {
            ++dones;
            doneAt = t;
        }
        if (last.failed) ++fails;
        if (last.contactAfter) ++afters;
        for (int i = 0; i < last.ended; ++i) episodes.push_back(last.episode[i]);
        maxLevel = std::max(maxLevel, last.level);
        lastWord = last.requestWord;
        return last;
    }
    void contact(bool read, uint32_t cells, uint32_t farCells = 0) { m.contactFrame(++frame, read, cells, farCells); }
    // n frames at one offset, with the contact frames (cells, or -1: none read) fed before each step
    void run(unsigned n, double h, double up, int cells = 0, uint32_t farCells = 0) {
        for (unsigned i = 0; i < n; ++i) {
            contact(cells >= 0, static_cast<uint32_t>(cells < 0 ? 0 : cells), farCells);
            step(h, up);
        }
    }
    static unsigned frames(double s) { return static_cast<unsigned>(std::ceil(s / dt - 1e-9)); }
};

inline bool has(const char* text, const char* needle) { return std::strstr(text, needle) != nullptr; }

template <class Check>
inline void run123(Check&& check) {
    {   // the fade: linear from soft to hard on the horizontal offset, upward only on the vertical one, the larger of the two
        check(f123::ramp(18.0, 18.0, 23.0) == 0.0 && std::fabs(f123::ramp(20.5, 18.0, 23.0) - 0.5) < 1e-12 && f123::ramp(23.0, 18.0, 23.0) == 1.0 && f123::ramp(40.0, 18.0, 23.0) == 1.0 &&
                  f123::ramp(5.0, 18.0, 23.0) == 0.0,
              "F123: the horizontal fade is 0 up to 18 cm, 0.5 at 20.5, black from 23");
        check(std::fabs(f123::leashLevel(10.0, 5.5) - 0.5) < 1e-12 && f123::leashLevel(10.0, 8.0) == 1.0 && f123::leashLevel(10.0, -20.0) == 0.0 && f123::leashLevel(10.0, 3.0) == 0.0 &&
                  std::fabs(f123::leashLevel(21.0, 5.5) - 0.6) < 1e-12 && f123::leashLevel(23.0, -5.0) == 1.0,
              "F123: the vertical band is up only (+3 cm soft, +8 cm hard; the head lower than the rest point is not limited) and the level is the larger of the two");
        check(f123::ramp(NAN, 18.0, 23.0) == 0.0 && f123::insideSoft(17.9, 2.9) && !f123::insideSoft(18.0, 0.0) && !f123::insideSoft(5.0, 3.0) && f123::pastHard(23.0, 0.0) && f123::pastHard(0.0, 8.0) &&
                  !f123::pastHard(22.9, 7.9),
              "F123: not a number is no fade; inside soft is under 18 cm and under +3 cm; past hard is 23 cm or +8 cm");
        Sim s;
        bool exact = true;
        for (int i = 0; i <= 100; ++i) {
            const double h = 15.0 + 0.1 * i;   // a slow lean through the band
            s.step(h, 0.0);
            exact = exact && std::fabs(s.last.level - f123::ramp(h, 18.0, 23.0)) < 1e-12;
        }
        check(exact && s.last.level == 1.0, "F123: a slow lean from 15 to 25 cm fades the view exactly along the ramp, black from 23 cm");
    }
    {   // the hard timer: 0.3 s past 23 cm, then the recentre (fade out 0.1 s, the ask), the wait, the fade in
        Sim s;
        s.run(Sim::frames(0.25), 24.0, 0.0);
        check(s.asks == 0 && s.last.level == 1.0, "F123: 0.25 s past the hard radius: black, no recentre yet");
        s.run(Sim::frames(0.05) + 1, 22.0, 0.0);   // back inside hard: the timer starts again
        s.run(Sim::frames(0.25), 24.0, 0.0);
        check(s.asks == 0, "F123: coming back under the hard radius restarts the 0.3 s");
        s.run(Sim::frames(0.35), 24.0, 0.0);
        const bool askedWhen = s.asks == 1 && s.askedAt > 0.0;
        check(askedWhen && s.lastWord == 1u && s.last.askedBy == -1, "F123: held past 23 cm for 0.3 s and the fade out's 0.1 s, the position recentre is asked once, as request 1");
        // the runtime moves the origin: the head comes inside soft a few frames later
        s.run(4, 24.0, 0.0);
        check(s.lastWord == 1u && s.dones == 0 && s.m.pending() && s.m.holdFrozen(), "F123: while the ask stands the word stays 1 and the standing-height hold waits");
        s.step(1.0, 0.0);
        s.step(1.0, 0.0);
        check(s.dones == 1 && s.lastWord == 0u && !s.m.pending(), "F123: the head back inside soft after the ask: done, the word back to 0");
        double prev = 2.0;
        bool monotone = true;
        unsigned fadeFrames = 0;
        while (s.last.level > 0.0 && fadeFrames < 100) {
            monotone = monotone && s.last.level <= prev + 1e-12;
            prev = s.last.level;
            s.step(1.0, 0.0);
            ++fadeFrames;
        }
        check(monotone && fadeFrames >= 16 && fadeFrames <= 20 && s.last.level == 0.0, "F123: the fade in takes 0.2 s (about 18 frames) down from black to clear, never up");
        check(s.episodes.size() == 1 && s.episodes[0].kind == f123::kLeashHard && s.episodes[0].recentred && s.episodes[0].peakCm == 24.0 && s.episodes[0].blackS > 0.7 && s.episodes[0].blackS < 1.2 &&
                  s.episodes[0].pastHardS >= 0.3,
              "F123: one leash hard episode, recentred, peak 24 cm, black about a second (0.3 held + 0.1 fade out + the wait + the start of the fade in)");
    }
    {   // a lean past hard that ends in time: an episode, no recentre; soft only: a soft episode
        Sim s;
        s.run(Sim::frames(0.2), 24.0, 0.0);
        s.run(Sim::frames(0.3), 10.0, 0.0);
        check(s.asks == 0 && s.episodes.size() == 1 && s.episodes[0].kind == f123::kLeashHard && !s.episodes[0].recentred && s.episodes[0].pastHardS < 0.3,
              "F123: past the hard radius for 0.2 s and back: a hard episode with no recentre");
        Sim q;
        q.run(30, 20.0, 0.0);
        q.run(30, 5.0, 0.0);
        check(q.asks == 0 && q.episodes.size() == 1 && q.episodes[0].kind == f123::kLeashSoft && q.episodes[0].blackS == 0.0 && std::fabs(q.episodes[0].peakCm - 20.0) < 1e-12 && q.maxLevel < 1.0,
              "F123: 20 cm for a third of a second: a soft episode, never black");
        Sim v;
        v.run(Sim::frames(0.5), 5.0, 9.0);
        check(v.asks == 1 && v.maxLevel == 1.0, "F123: 9 cm above the rest point for half a second (standing up to look over cover) is past the vertical hard radius: black, then the recentre");
    }
    {   // the recentre that does not bring the head inside soft: said failed after 0.6 s, the fade in goes on, a second ask only after the gap
        Sim s;
        s.run(Sim::frames(0.3) + 12, 30.0, 0.0);
        check(s.asks == 1, "(the ask stands)");
        s.run(Sim::frames(0.65), 30.0, 0.0);
        check(s.fails == 1 && s.dones == 0 && s.lastWord == 0u, "F123: a recentre that left the head 30 cm out: failed after 0.6 s, the word withdrawn");
        s.run(Sim::frames(0.6), 30.0, 0.0);
        check(s.asks == 2, "F123: the head still out: it is asked again after the fade in and the gap");
    }
    {   // the contact: 3 consecutive frames of 32 cells or more latch it, black at once
        Sim s;
        s.run(5, 5.0, 0.0, 40);   // frames 1 to 3 latch it on the third
        check(s.m.contactLatched() && s.last.level == 1.0, "F123: 32 cells or more on 3 consecutive frames: latched, black");
        Sim a;
        a.run(2, 5.0, 0.0, 40);
        a.run(1, 5.0, 0.0, 31);
        a.run(2, 5.0, 0.0, 40);
        check(!a.m.contactLatched() && a.last.level == 0.0, "F123: 31 cells breaks the run: two and two frames are no contact");
        Sim b;
        b.run(2, 5.0, 0.0, 40);
        b.run(3, 5.0, 0.0, -1);   // frames no eye was read for: ignored, and their numbers are a gap
        b.run(1, 5.0, 0.0, 40);
        check(!b.m.contactLatched(), "F123: frames with no eye read change nothing themselves, and the gap they leave restarts the count");
        Sim b2;
        b2.run(3, 5.0, 0.0, 40);
        b2.run(5, 5.0, 0.0, -1);
        check(b2.m.contactLatched() && b2.last.level == 1.0, "F123: a latched contact stays latched through frames nothing was read for");
        Sim c;
        c.run(2, 5.0, 0.0, 40);
        c.frame += 3;   // a gap in the readback frames restarts the count
        c.run(2, 5.0, 0.0, 40);
        check(!c.m.contactLatched(), "F123: a gap in the frame numbers restarts the count");
        // the proof of a brush: 31% of the cells (20 of 64) is the 13:59:24 case and never latches
        Sim d;
        d.run(60, 4.6, 0.0, 20);
        check(!d.m.contactLatched() && d.episodes.empty() && d.maxLevel == 0.0, "F123: 20 of 64 cells at the near plane for two thirds of a second (the 13:59:24 brush): nothing happens");
    }
    {   // latched: recentre after 0.3 s, once; released only when it has cleared AND the head is inside soft
        Sim s;
        s.run(3, 20.0, 0.0, 50, 0);       // latched, the head 20 cm out (past soft, short of hard: the leash does not ask)
        s.run(Sim::frames(0.3) + 14, 20.0, 0.0, 60, 2);
        check(s.asks == 1 && s.m.contactLatched() && s.m.pending(), "F123: a contact held 0.3 s asks for the recentre, once");
        s.step(1.0, 0.0);
        s.step(1.0, 0.0);   // the origin moved: the head inside soft, but the wall is still there
        check(s.dones == 1 && s.m.contactLatched(), "F123: the recentre done and the contact still latched: black stays");
        s.run(Sim::frames(1.2), 1.0, 0.0, 50, 0);
        check(s.asks == 1 && s.afters == 1, "F123: no second ask for the same latch; the contact still present 1 s after the recentre is said");
        s.run(2, 1.0, 0.0, 0);
        check(s.m.contactLatched(), "F123: the contact clear for 2 frames is not enough");
        s.run(1, 25.0, 0.0, 0);
        check(s.m.contactLatched(), "F123: the contact clear but the head 25 cm out: still latched (not inside soft)");
        s.run(1, 10.0, 0.0, 0);
        check(!s.m.contactLatched() && s.last.level > 0.8, "F123: cleared 3 frames and the head inside soft: released, and the fade out begins");
        s.run(Sim::frames(0.25), 10.0, 0.0, 0);
        check(s.last.level == 0.0 && s.episodes.size() >= 1, "F123: the release fades out over 0.2 s");
        const f123::Episode& e = s.episodes.back();
        check(e.kind == f123::kContact && e.peakCells == 60 && e.weakestCells == 50 && e.farCells == 2 && e.recentred && e.frames > 60 && e.blackS > 1.0,
              "F123: the contact episode: peak 60 cells, weakest contact frame 50, far cells up to 2, recentred, black over a second");
        // the next latch asks again
        s.run(3, 5.0, 0.0, 40);
        s.run(Sim::frames(0.3) + 14, 5.0, 0.0, 40);
        check(s.asks == 2, "F123: a new latch asks again");
    }
    {   // levels combine by max; the camera leaving foot clears everything and withdraws an ask, the serial goes on
        Sim s;
        s.run(Sim::frames(0.3) + 10, 24.0, 0.0);
        check(s.m.pending() || s.m.holdFrozen(), "(the recentre is under way)");
        const uint32_t serial = s.m.serial();
        s.step(24.0, 0.0, true, false);
        check(s.last.level == 0.0 && s.last.requestWord == 0u && !s.m.pending() && s.m.serial() == serial, "F123: the camera off foot (the boarding edge): clear at once, any ask withdrawn, the serial kept");
        s.run(Sim::frames(0.8), 24.0, 0.0);
        check(s.lastWord == serial + 1u || s.asks == 2, "F123: a later ask carries the next serial (the runtime acts once on each)");
        f123::FadeMix mix;
        mix.snap = 0.4;
        mix.contain = 0.0;
        const double a = mix.next();
        mix.snap = 0.4;
        mix.contain = 0.8;
        const double b = mix.next();
        mix.snap = 0.9;
        mix.contain = 0.3;
        const double c = mix.next();
        mix.snap = 0.0;
        mix.contain = 0.0;
        const double d = mix.next();
        const double e = mix.next();
        check(a == 0.4 && b == 0.8 && c == 0.9 && d == 0.0 && e == -1.0, "F123: the comfort fade is the max of F109's snap and the containment's; a clear frame after a level publishes 0 once, then nothing");
        Sim t;
        t.step(5.0, 0.0, false);
        t.run(1, 5.0, 0.0);
        for (int i = 0; i < 30; ++i) t.step(24.0, 0.0, false);   // no offset for a third of a second: the last one stands 0.2 s, then none
        check(t.last.level == 0.0, "F123: an offset unsampled for more than 0.2 s is no offset: never black by accident");
    }
    {   // the lines
        char b[900];
        f123::formatSession(b, sizeof(b));
        check(has(b, "the leash and the wall contact are on (no key), native on foot only; leash on the physical head offset from the rest point: horizontal fade from 18 cm to black at 23 cm, upward from 3 cm to black at 8 cm;") &&
                  has(b, "a position-only recentre (fade out 0.1 s, the origin under the head with its yaw kept, fade in 0.2 s)") && has(b, "on 32 of 64 or more in either eye for 3 frames") && std::strlen(b) < 900,
              "F123: the session line states the bands, the timers, the recentre and the contact rule");
        f123::Episode e;
        e.kind = f123::kLeashHard;
        e.startS = 12.5;
        e.lengthS = 1.25;
        e.blackS = 0.9;
        e.peakCm = 31.4;
        e.peakUpCm = -3.0;
        e.pastHardS = 0.5;
        e.recentred = true;
        char l[500];
        f123::formatEpisode(l, sizeof(l), e, e.startS, false);
        check(has(l, "(F123): leash hard episode: began 12.50 s into this on-foot spell, lasted 1.25 s, black 0.90 s; peak head offset 31.4 cm horizontal and -3.0 cm up, 0.50 s past a hard radius; recentred yes"),
              "F123: a leash episode line has its kind, start, length, black time, peaks and whether it was recentred");
        e.kind = f123::kContact;
        e.peakCells = 58;
        e.weakestCells = 36;
        e.farCells = 0;
        e.frames = 140;
        e.recentred = false;
        f123::formatEpisode(l, sizeof(l), e, e.startS, true);
        check(has(l, "wall contact episode: began 12.50 s") && has(l, ", ADS at the end;") && has(l, "peak coverage 58 of 64 cells") && has(l, "weakest contact frame 36 cells, far cells (over 1 m) up to 0, contact frames 140; recentred no"),
              "F123: a contact episode line has the coverage peak, the weakest contact frame, the far cells and the contact frames");
        f123::formatAsk(l, sizeof(l), 4, f123::kContact, 12.0, 1.5, 55);
        check(has(l, "position recentre 4 asked by a latched wall contact: the head 12.0 cm horizontal and +1.5 cm up from the rest point, contact 55 of 64 cells"), "F123: the ask line");
        f123::formatResult(l, sizeof(l), 4, true, 0.07, 0.4, 0.0);
        check(has(l, "position recentre 4 done after 0.07 s: the head is 0.4 cm horizontal and +0.0 cm up from the new rest point (inside soft)"), "F123: the done line");
        f123::formatResult(l, sizeof(l), 5, false, 0.6, 28.0, 0.0);
        check(has(l, "position recentre 5 DID NOT bring the head inside soft within 0.60 s: the head is still 28.0 cm horizontal"), "F123: the failed line");
        f123::formatAfter(l, sizeof(l), 4);
        check(has(l, "wall contact still present 1 s after position recentre 4"), "F123: the contact-after line");
    }
}

}  // namespace f123test
