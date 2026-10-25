// F125 checks (native on-foot VR): the on-foot log's windows (native_on_foot_logwin.h). Header only, run by native_frame_test (run125).
// The lines of nine later steps move their print to these helpers, so the helpers are held here, with the clock driven by hand:
// 1. the gate: detail() is the cached key, off until it is read, and a detail line's reset runs whether or not the line is written;
// 2. the window of a summary of sums: 10 s by default, 1 s in the detail log, opened by the first call (never at the clock's zero), its seconds capped at 10;
// 3. the per-second fold: the 1 s gate unchanged by the key, SecCount's N of M s, due() at 10 folds (1 in the detail log);
// 4. the flush: a window closed early prints its own length (1 to 10 s), and the next one opens fresh;
// 5. the key turned on and off part way through a window.
#pragma once

#include "../../src/d3d11/native_on_foot_logwin.h"

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
    lw::setDetail(was);
}

}  // namespace f125test
