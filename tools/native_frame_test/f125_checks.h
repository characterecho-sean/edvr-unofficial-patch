// F125 checks (native on-foot VR): the on-foot log's windows (native_on_foot_logwin.h). Header only, run by native_frame_test (run125).
// The lines of nine later steps move their print to these helpers, so the helpers are held here, with the clock driven by hand:
// 1. the gate: detail() is the cached key, off until it is read, and a detail line's reset runs whether or not the line is written;
// 2. the window of a summary of sums: 10 s by default, 1 s in the detail log, opened by the first call (never at the clock's zero), its seconds capped at 10;
// 3. the per-second fold: the 1 s gate unchanged by the key, SecCount's N of M s, due() at 10 folds (1 in the detail log);
// 4. the flush: a window closed early prints its own length (1 to 10 s), and the next one opens fresh;
// 5. the key turned on and off part way through a window;
// 6. a snapshot (step 3): its content's coarse signature, written on a change at most once per 30 s and otherwise once a minute, and a count said when it
//    changes.
#pragma once

#include "../../src/d3d11/native_on_foot_logwin.h"
#include "../../src/d3d11/native_on_foot_f125ui.h"
#include "../../src/d3d11/native_on_foot_f91.h"

#include <cstdint>
#include <vector>

namespace f125test {

namespace lw = edvr::nofoot::logwin;

// A summary of sums driven at a frame period for a span: the seconds of each window it printed, and the frames those windows summed.
struct SumRun {
    std::vector<uint32_t> windows;
    uint64_t framesPrinted = 0, framesSeen = 0;
};
inline SumRun runSums(uint64_t startMs, uint64_t spanMs, uint64_t frameMs, bool detailOn, bool flushAtEnd) {
    SumRun r;
    uint64_t win = 0, frames = 0, now = startMs;
    for (; now < startMs + spanMs; now += frameMs) {
        ++frames;   // the hook's accumulator: it counts whether or not a line is due
        ++r.framesSeen;
        const uint32_t secs = lw::window(win, now, detailOn);
        if (!secs) continue;
        r.windows.push_back(secs);
        r.framesPrinted += frames;
        frames = 0;   // the line's reset, with the print
    }
    if (flushAtEnd) {
        const uint32_t secs = lw::flush(win, now);
        if (secs) {
            r.windows.push_back(secs);
            r.framesPrinted += frames;
        }
    }
    return r;
}

template <class Check>
inline void run125(Check&& check) {
    const bool was = lw::detail();
    // ---- 1. the gate ----
    {
        lw::setDetail(false);
        check(!lw::detail() && lw::windowSeconds() == 10 && lw::windowSeconds(false) == 10 && lw::windowSeconds(true) == 1,
              "F125: the detail log is off unless the key is read on; a summary's window is 10 s, 1 s in the detail log");
        lw::setDetail(true);
        const bool on = lw::detail() && lw::windowSeconds() == 1;
        lw::setDetail(false);
        check(on && !lw::detail(), "F125: detail() follows the value readKey stores");
        // A detail line: its 1 s gate and its reset run as before; only the note is skipped. Over 30 s at 90 Hz with the key off the accumulator never holds
        // more than a second's frames, and no line is written.
        uint64_t gate = 0, acc = 0, accMax = 0;
        uint32_t lines = 0;
        for (uint64_t now = 50000; now < 80000; now += 11) {
            ++acc;
            accMax = acc > accMax ? acc : accMax;
            if (!lw::second(gate, now)) continue;
            if (lw::detail()) ++lines;
            acc = 0;
        }
        check(lines == 0 && accMax <= 92, "F125: a detail line with the key off writes nothing and still resets its sums each second (no accumulator grows)");
    }
    // ---- 2. the window of a summary of sums ----
    {
        uint64_t w = 0;
        const uint32_t opened = lw::window(w, 123456, false);
        check(opened == 0 && w == 123456, "F125: the first call opens the window at its own time and prints nothing (a window never starts at the clock's zero)");
        check(lw::window(w, 123456 + 9999, false) == 0 && w == 123456, "F125: a 10 s window is open 9.999 s in");
        check(lw::window(w, 123456 + 10000, false) == 10 && w == 133456, "F125: ...and closes at 10 s with 10, the next window starting at that call");
        uint64_t d = 0;
        lw::window(d, 5000, true);
        check(lw::window(d, 5999, true) == 0 && lw::window(d, 6000, true) == 1 && d == 6000, "F125: in the detail log the window is 1 s and prints 1 (the 1 s line)");
        check(lw::window(d, 7600, true) == 2, "F125: a detail window that closes 1.6 s in (a stall) prints its own length, 2 s");
        uint64_t late = 1000;
        lw::window(late, 1000, false);
        check(lw::window(late, 1000 + 300000, false) == 10, "F125: a window that ran on past its length (a gap with no flush) counts at most 10 s");
        uint64_t back = 9000;
        check(lw::window(back, 8000, false) == 0 && back == 8000, "F125: a clock behind the window's start reopens it rather than printing");
        // 35 s of 90 Hz frames: three 10 s windows with the key off (the last 5 s flushed), 35 one-second windows with it on; every frame summed is printed once.
        const SumRun off = runSums(200000, 35000, 11, false, true);
        const SumRun on = runSums(200000, 35000, 11, true, true);
        bool offShape = off.windows.size() == 4 && off.windows[0] == 10 && off.windows[1] == 10 && off.windows[2] == 10 && off.windows[3] == 5;
        bool onShape = on.windows.size() == 35;
        for (uint32_t s : on.windows) onShape = onShape && s == 1;
        check(offShape && off.framesPrinted == off.framesSeen, "F125: 35 s with the key off: windows of 10, 10, 10 and a flushed 5 s, every frame summed once");
        check(onShape && on.framesPrinted == on.framesSeen, "F125: the same 35 s in the detail log: 35 lines of 1 s, every frame summed once");
    }
    // ---- 3. the per-second fold ----
    {
        uint64_t g = 0;
        const bool firstTick = lw::second(g, 70000);
        check(firstTick && !lw::second(g, 70999) && lw::second(g, 71000) && g == 71000,
              "F125: the fold tick is the old 1 s gate: true at the first call, then once a second");
        lw::setDetail(true);
        uint64_t g2 = 0;
        uint32_t ticks = 0;
        for (uint64_t now = 90000; now < 100000; now += 11) ticks += lw::second(g2, now) ? 1u : 0u;
        lw::setDetail(false);
        uint64_t g3 = 0;
        uint32_t ticksOff = 0;
        for (uint64_t now = 90000; now < 100000; now += 11) ticksOff += lw::second(g3, now) ? 1u : 0u;
        check(ticks == ticksOff && ticks == 10, "F125: the fold tick's cadence is the same with the key on or off (what a line does each second keeps its cadence)");
        // 25 s, armed in seconds 3..14 and ADS in 5..8: key off, lines at folds 10 and 20 and a flush at 25; key on, a line each fold.
        const auto fold = [](bool detailOn, std::vector<uint32_t>& m, std::vector<uint32_t>& armed, std::vector<uint32_t>& ads) {
            lw::SecCount a, s;
            for (uint32_t sec = 0; sec < 25; ++sec) {
                a.fold(sec >= 3 && sec <= 14);
                s.fold(sec >= 5 && sec <= 8);
                if (!lw::due(a.m, detailOn)) continue;
                m.push_back(a.m);
                armed.push_back(a.n);
                ads.push_back(s.n);
                a.clear();
                s.clear();
            }
            if (a.m) {   // the flush at the on-foot exit
                m.push_back(a.m);
                armed.push_back(a.n);
                ads.push_back(s.n);
            }
        };
        std::vector<uint32_t> m, armed, ads, m1, armed1, ads1;
        fold(false, m, armed, ads);
        fold(true, m1, armed1, ads1);
        const bool offShape = m == std::vector<uint32_t>{10, 10, 5} && armed == std::vector<uint32_t>{7, 5, 0} && ads == std::vector<uint32_t>{4, 0, 0};
        uint32_t armedOn = 0, adsOn = 0;
        bool onShape = m1.size() == 25;
        for (size_t i = 0; i < m1.size(); ++i) {
            onShape = onShape && m1[i] == 1 && armed1[i] <= 1 && ads1[i] <= 1;
            armedOn += armed1[i];
            adsOn += ads1[i];
        }
        check(offShape, "F125: a fold line with the key off: armed 7 of 10 s, 5 of 10 s, then 0 of 5 s at the flush; ADS 4 of 10 s, then none");
        check(onShape && armedOn == 12 && adsOn == 4, "F125: ...and in the detail log a line a second, 1 or 0 of 1 s, the same 12 armed and 4 ADS seconds in all");
        check(!lw::due(9, false) && lw::due(10, false) && lw::due(1, true) && !lw::due(0, true), "F125: due() at 10 folds, at 1 in the detail log");
    }
    // ---- 4. the flush ----
    {
        uint64_t w = 0;
        check(lw::flush(w, 5000) == 0 && w == 0, "F125: a flush with no window open prints nothing");
        lw::window(w, 40000, false);
        check(lw::flush(w, 43400) == 3 && w == 0, "F125: a window flushed 3.4 s in (a held-item change) prints 3 s, and none is open after it");
        check(lw::window(w, 43411, false) == 0 && w == 43411, "F125: ...the next call opens a new window at its own time");
        check(lw::flush(w, 43611) == 1, "F125: a window flushed 0.2 s after it opened still prints a length, 1 s");
        lw::window(w, 60000, false);
        check(lw::flush(w, 60000 + 600000) == 10, "F125: a flush long after the window opened counts at most 10 s");
    }
    // ---- 5. the key turned on and off part way through a window ----
    {
        uint64_t w = 0;
        lw::window(w, 10000, false);
        check(lw::window(w, 16000, false) == 0 && lw::window(w, 16011, true) == 6 && w == 16011,
              "F125: the key turned on 6 s into a 10 s window closes it at the next call with its own 6 s, then 1 s windows");
        check(lw::window(w, 17011, true) == 1, "F125: ...and the next detail window is 1 s");
        uint64_t v = 0;
        lw::window(v, 20000, true);
        check(lw::window(v, 20500, false) == 0 && lw::window(v, 29999, false) == 0 && lw::window(v, 30000, false) == 10,
              "F125: the key turned off inside a 1 s window makes it a 10 s window from its own start");
        lw::SecCount c;
        for (int i = 0; i < 4; ++i) c.fold(true);
        check(!lw::due(c.m, false) && lw::due(c.m, true) && c.m == 4 && c.n == 4,
              "F125: four folds into a 10 s window are due once the key is on, and print 4 of 4 s");
    }
    // ---- 6. a snapshot: written on a change of content at most once per 30 s, otherwise once a minute (the avatar's parts, the AMCs near the player) ----
    {
        // A part signs its name, its flags and which of its four mask words are set, not their bits (the flight's masks gained view bits every few windows).
        const auto part = [](const char* name, uint64_t m0, uint64_t m1) {
            const uint64_t masks[4] = {m0, m1, 0, 0};
            return lw::sigNonZero(lw::sigMix(lw::sigText(lw::kSigSeed, name), 1u), masks, 4);
        };
        check(part("Beard", 0x180000000ull, 0x180000000ull) == part("Beard", 0x105980000000ull, 0x6180000000ull),
              "F125: a part's mask words gaining view bits (Beard 0x180000000 -> 0x105980000000, flight 10-25) sign the same");
        check(part("Beard", 0x180000000ull, 0) != part("Beard", 0x180000000ull, 0x180000000ull) && part("Head", 1, 0) != part("Hair", 1, 0),
              "F125: a mask word set or cleared, or another part name, signs differently");
        check(lw::sigText(lw::sigText(lw::kSigSeed, "ab"), "c") != lw::sigText(lw::sigText(lw::kSigSeed, "a"), "bc"),
              "F125: names are signed with an end mark (\"ab\", \"c\" is not \"a\", \"bc\")");
        // The AMC rows sign their count and the multiset of their modes: no address, no distance.
        const auto modes = [](std::vector<uint32_t> m) {
            uint64_t s = lw::sigMix(lw::kSigSeed, m.size());
            for (uint32_t v : m) s = lw::sigSetAdd(s, v);
            return s;
        };
        check(modes({3, 3, 2}) == modes({2, 3, 3}) && modes({3, 3, 2}) != modes({3, 3, 3}) && modes({3, 3}) != modes({3, 3, 3}) &&
                  modes({3, 2}) != modes({3, 3, 2}),
              "F125: the AMC rows' modes sign the same in any order; another mode or another count of rows signs differently");

        // The gate, every 5 s for 3 minutes: the same content throughout writes at 0, 60, 120 and 180 s with the key off, and every 5 s with it on.
        const auto run = [](bool detailOn, uint64_t changeAtMs, std::vector<uint64_t>& at) {
            lw::Snapshot snap;
            for (uint64_t t = 100000; t <= 100000 + 180000; t += 5000) {
                const uint64_t sig = t >= changeAtMs ? 2 : 1;
                if (snap.due(sig, t, detailOn)) at.push_back(t - 100000);
            }
        };
        std::vector<uint64_t> off, on, changed, early;
        run(false, ~0ull, off);
        run(true, ~0ull, on);
        run(false, 100000 + 35000, changed);
        run(false, 100000 + 15000, early);
        check(off == std::vector<uint64_t>{0, 60000, 120000, 180000}, "F125: an unchanged snapshot is written at once, then once a minute (0, 60, 120, 180 s)");
        check(on.size() == 37, "F125: ...and every 5 s in the detail log (37 lines in 180 s), as before");
        check(changed == std::vector<uint64_t>{0, 35000, 95000, 155000},
              "F125: a change of content at 35 s is written at once, and the minute counts again from it (95, 155 s)");
        check(early == std::vector<uint64_t>{0, 30000, 90000, 150000},
              "F125: a change of content 15 s after the last written snapshot waits for the 30 s limit, then the minute counts from it");
        // Content that alternates (A, B, A, ...) every 5 s for 2 minutes: written at 0, 35, 70 and 105 s (a B or A differing from the last written, 30 s or
        // more after it); content new every 5 s: written every 30 s.
        lw::Snapshot alt, churn;
        std::vector<uint64_t> altAt;
        uint32_t churnLines = 0;
        for (uint64_t t = 0; t < 120000; t += 5000) {
            if (alt.due((t / 5000) % 2 ? 7u : 9u, 200000 + t, false)) altAt.push_back(t);
            churnLines += churn.due(1000 + t, 200000 + t, false) ? 1u : 0u;
        }
        check(altAt == std::vector<uint64_t>{0, 35000, 70000, 105000}, "F125: alternating content is written at most once per 30 s (0, 35, 70, 105 s)");
        check(churnLines == 4, "F125: content new every 5 s is written every 30 s (4 lines in 2 minutes, not 24)");
        lw::Snapshot cleared;
        const bool first = cleared.due(5, 300000, false);
        const bool same = cleared.due(5, 305000, false);
        cleared.clear();
        check(first && !same && cleared.due(5, 310000, false), "F125: clear() (off foot) makes the next snapshot due, so each time on foot starts written");
        lw::Snapshot back;
        back.due(5, 400000, false);
        check(back.due(5, 399000, false), "F125: a clock behind the last written snapshot writes it again rather than holding it for a minute");

        // The count of AMCs not collected: said when it changes, never as 0, every time in the detail log.
        lw::CountChange cc;
        const uint32_t counts[] = {0, 3, 3, 3, 5, 5, 0, 5, 2};
        std::vector<uint32_t> said;
        for (uint32_t n : counts)
            if (cc.due(n, false)) said.push_back(n);
        lw::CountChange cd;
        uint32_t saidDetail = 0;
        for (uint32_t n : counts) saidDetail += cd.due(n, true) ? 1u : 0u;
        check(said == std::vector<uint32_t>{3, 5, 5, 2}, "F125: the not-collected count is said at 3, 5, 5 after a 0, and 2; not repeated, never 0");
        check(saidDetail == 7, "F125: ...and in the detail log at every window it is not 0, as before");
    }
    // ---- 7. the marker report block: the functional second stays independent of print windows and immediate edge flushes ----
    {
        uint64_t start = 1000, lastSecond = 1000;
        uint32_t ticks = 0, sums = 0, reported = 0;
        std::vector<uint32_t> windows;
        for (uint64_t now = 1020; now <= 27000; now += 20) {
            ++sums;
            const bool second = lw::second(lastSecond, now);
            if (second) ++ticks;   // v2Report, f85Report, f90Report and the burst/census re-arm stay here
            const bool edge = now == 14300 || now == 27000;   // held item, ADS end, setting or on-foot edge, even between second folds
            if (!second && !edge) continue;
            const uint32_t n = lw::gateWindow(start, now, edge, false);
            if (!n) continue;
            windows.push_back(n);
            reported += sums;
            sums = 0;
        }
        check(windows == std::vector<uint32_t>{10, 3, 10, 2} && reported == 1300 && sums == 0,
              "F125 marker: 10 s block, immediate 3 s edge flush, fresh 10 s block and 2 s exit flush, every sample once");
        check(ticks == 26, "F125 marker: a partial-window edge does not advance or delay any of the 26 functional second folds");
        uint64_t detailStart = 1000;
        check(lw::gateWindow(detailStart, 2000, false, true) == 1 && lw::gateWindow(detailStart, 2400, true, true) == 1 && detailStart == 2400 &&
                  lw::gateWindow(detailStart, 3400, false, true) == 1,
              "F125 marker: detail windows remain 1 s, and an edge immediately closes even a partial detail second");
        uint64_t flipStart = 1000;
        check(lw::gateWindow(flipStart, 4300, true, false) == 3 && lw::gateWindow(flipStart, 5300, false, true) == 1 &&
                  lw::gateWindow(flipStart, 5700, true, true) == 1 && lw::gateWindow(flipStart, 15700, false, false) == 10,
              "F125 marker: changing detail in either direction flushes the previous mode's window before the next mode's window");
        lw::SecondShare tagged;
        const uint64_t a = tagged.take(8), b = tagged.take(13);
        tagged.reset(16);   // an edge after the fold carries the 3 not yet consumed
        tagged.reset(2);    // another edge before the next fold carries another 2
        const uint64_t c = tagged.take(4);
        tagged.reset(4);    // a reset immediately after a fold carries nothing
        const uint64_t d = tagged.take(7);
        check(a == 8 && b == 5 && c == 9 && d == 7 && a + b + c + d == 29,
              "F125 marker: F85's functional unmap deltas survive multiple partial flushes with no lost or double-counted unmaps");
    }
    // ---- 8. F50's global cap survives key-slot churn; detail retains the old event stream ----
    {
        lw::EventCap cap;
        uint32_t lines = 0, held = 0;
        for (uint64_t now = 1000; now < 6000; now += 20)
            if (cap.due(now, false, &held)) ++lines;
        check(lines == 8 && cap.due(11000, false, &held) && held == 242,
              "F125 marker: 250 resets across reclaimed slots write at most 8 per 10 s; the next event reports the 242 held");
        lw::EventCap detailed;
        lines = 0;
        for (uint64_t now = 1000; now < 6000; now += 20)
            if (detailed.due(now, true, &held)) ++lines;
        check(lines == 250, "F125 marker: detail keeps every reset that passes the existing per-slot cap");
        cap.due(11001, false, &held);
        check(cap.due(500, false, &held), "F125 marker: a clock behind the global cap reopens it rather than suppressing events indefinitely");
    }
    // ---- 9. F91's live detail toggle cannot emit or backfill a window collected before off ----
    {
        namespace fit = edvr::nofoot::f91;
        fit::FitWindow w;
        fit::FitFrame frame;
        frame.dt = 0.01;
        frame.applied[0] = 2.0;
        w.beginPress();
        for (uint32_t i = 0; i < fit::kFitMinFrames; ++i)
            if (fit::fitDetailFrame(w, true, true)) w.add(frame);
        w.prevValid = w.prevMoving = true;
        w.prevNet[0] = 5.0;
        check(w.n >= fit::kFitMinFrames && w.hist[0][0] == 2.0,
              "F125 F91: detail on collects a fit window with enough frames to emit if the trace ends");
        const bool offFrame = fit::fitDetailFrame(w, false, true);
        check(!offFrame && w.n == 0 && w.dur == 0.0 && w.hist[0][0] == 0.0 && !w.prevValid && !w.prevMoving && w.prevNet[0] == 0.0,
              "F125 F91: turning detail off during a press discards the pending fit and its applied/motion history before any partial emit");
        frame.applied[0] = 99.0;
        for (uint32_t i = 0; i < 100; ++i)
            if (fit::fitDetailFrame(w, false, true)) w.add(frame);
        check(w.n == 0 && !fit::fitDetailFrame(w, false, false),
              "F125 F91: detail off neither collects a full window nor emits a partial window at the trace's end");
        frame.applied[0] = 3.0;
        const bool resumed = fit::fitDetailFrame(w, true, true);
        if (resumed) w.add(frame);
        check(resumed && w.n == 1 && w.hist[0][0] == 3.0 && w.hist[1][0] == 0.0 && w.f[0].applied[0] == 3.0,
              "F125 F91: turning detail on during the same press resumes with only fresh frame measurements, never the off interval");
        check(!fit::fitDetailFrame(w, true, false) && w.n == 1,
              "F125 F91: a trace ending with detail on keeps the original partial-window close path");
    }
    // ---- 10. reticle summaries classify each functional second, retaining state across both second and print boundaries ----
    {
        lw::ReticleWindow window;
        edvr::nofoot::f118::Steps steps;
        edvr::nofoot::f118::Trail trail;
        edvr::nofoot::f118::FootSecond foot;
        steps.have = steps.open = true;
        steps.prevD = 8.0;
        steps.openFrom = 4.0;
        steps.openAge = 2;
        trail.runMs = 35.0;
        for (uint32_t second = 0; second < 10; ++second) {
            steps.reads = 80;
            steps.steps = 1;
            steps.flips = second < 3 ? 1 : 0;
            trail.frames = 90;
            trail.longestMs = second < 3 ? 120.0 : 22.0;
            foot.read = 40;
            foot.edge = 30;
            window.take(steps, trail, foot, true);
            steps.second();
            trail.second();
            foot.second();
        }
        check(window.steps.reads == 800 && window.steps.steps == 10 && window.foot.read == 400 && window.trail.frames == 900 && window.trail.longestMs == 120.0,
              "F125 UI: ten functional reticle seconds sum counts once and retain the window's largest trailing run");
        check(window.edgeSeconds.n == 10 && window.speedSeconds.n == 3 && window.edgeSeconds.m == 10,
              "F125 UI: classify edge/speed per second (10 edge, 3 speed), never from a window's ten combined steps or largest run");
        check(steps.have && steps.open && steps.prevD == 8.0 && steps.openFrom == 4.0 && steps.openAge == 2 && trail.runMs == 35.0,
              "F125 UI: reporting does not change the reticle's open step, previous target or current trailing run");
        window.clearWindow();
        steps.reads = 3;
        foot.read = 3;
        foot.edge = 1;
        window.take(steps, trail, foot, false);
        const uint64_t first = window.steps.reads;
        window.clearWindow();
        steps.reads = 5;
        steps.steps = 1;
        foot.read = 5;
        foot.edge = 2;
        window.take(steps, trail, foot, true);
        check(first == 3 && window.steps.reads == 2 && window.foot.read == 2 && window.foot.edge == 1 && window.edgeSeconds.n == 1,
              "F125 UI: an immediate partial flush takes three reads; the next functional fold takes only two new reads and still classifies the whole second");
        steps.second();
        foot.second();
        window.clearWindow();
        steps.reads = 7;
        window.take(steps, trail, foot, true);
        check(window.steps.reads == 7 && window.edgeSeconds.m == 1 && window.edgeSeconds.n == 0,
              "F125 UI: after the flushed second closes, fresh reads and fresh flag counts begin at zero");
        steps.second();
        trail.second();
        foot.second();
        window.clearWindow();
        steps.reads = 1;
        steps.largestDeg = 2.0;
        trail.longestMs = 120.0;
        trail.worstDeg = 1.5;
        foot.read = 1;
        foot.largestDeg = 3.0;
        window.take(steps, trail, foot, false);
        window.clearWindow();
        window.take(steps, trail, foot, false);
        check(window.steps.reads == 0 && window.steps.largestDeg == 0.0 && window.trail.longestMs == 0.0 && window.trail.worstDeg == 0.0 && window.foot.largestDeg == 0.0,
              "F125 UI: flushing with no new samples never repeats the previous reporting window's maxima");
        auto noSamples = window;
        auto noSteps = steps;
        auto noTrail = trail;
        auto noFoot = foot;
        noSamples.take(noSteps, noTrail, noFoot, true);
        check(noSamples.steps.reads == 0 && noSamples.steps.largestDeg == 0.0 && noSamples.trail.longestMs == 0.0 && noSamples.trail.worstDeg == 0.0 &&
                  noSamples.foot.largestDeg == 0.0 && noSamples.speedSeconds.n == 1,
              "F125 UI: after repeated between-fold flushes a no-new-sample fold prints no old extrema while classifying the complete second");
        steps.reads = 2;
        steps.largestDeg = 0.2;
        trail.longestMs = 20.0;
        trail.worstDeg = 0.1;
        foot.read = 2;
        foot.largestDeg = 0.3;
        window.take(steps, trail, foot, true);
        check(window.steps.reads == 1 && window.steps.largestDeg == 0.2 && window.trail.longestMs == 20.0 && window.trail.worstDeg == 0.1 &&
                  window.foot.largestDeg == 0.3 && window.speedSeconds.n == 1,
              "F125 UI: the next print window records its smaller peaks while classification retains the whole second's earlier 120 ms run");
    }
    {
        struct Meta { uint32_t item = 0, setting = 0; };
        lw::WindowMetadata<Meta> metadata;
        metadata.prime(Meta{1, 10});   // report's initial call, before any 1 s fold
        const Meta before = metadata.prime(Meta{2, 20});   // an item/setting change just before the first fold
        metadata.next(Meta{2, 20});
        const Meta after = metadata.prime(Meta{2, 20});
        check(before.item == 1 && before.setting == 10 && after.item == 2 && after.setting == 20,
              "F125 UI: metadata primed before the first fold labels an immediate flush with the old item/settings, then starts the new metadata");
    }
    lw::setDetail(was);
}

}  // namespace f125test
