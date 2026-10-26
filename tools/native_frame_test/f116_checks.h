// F116 checks (native on-foot VR): the HUD hidden while a terminal fills the view. Header only, run by native_frame_test (run116).
// The footprints are the two terminal flights' (logs edvr_gfx_20261025_055612 and _071049, Pimax Crystal Super, 2016x2017 render per eye): in use the F105
// screens covered 19-35% of each eye in every census window, walking past 0.4-0.5%.
#pragma once

#include "../../src/d3d11/native_on_foot_f116.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace f116test {

namespace f116 = edvr::nofoot::f116;

template <class Check>
inline void run116(Check&& check) {
    {   // the footprint: the larger eye's share of its own render pixels
        const uint64_t eye = 2016ull * 2017ull;
        check(std::fabs(f116::frameFootprint(1411200, eye, 700000, eye) - 0.3470f) < 1e-3f && f116::frameFootprint(10, 0, 0, eye) == 0.0f &&
                  f116::frameFootprint(0, eye, 40000, eye) > f116::frameFootprint(40000, eye * 2, 0, eye),
              "F116: the frame's footprint is the larger eye's share of its own pixels (an eye with no pixels is 0)");
    }
    {   // the hysteresis: two frames over 15% hide, ten under 10% show; one frame over does not; the 10-15% band holds either state
        f116::Hysteresis h;
        check(!h.step(0.30f) && h.step(0.30f), "F116: the HUD hides on the second frame in a row over 15%");
        f116::Hysteresis one;
        bool any = false;
        for (int i = 0; i < 20; ++i) any = one.step(i % 2 ? 0.30f : 0.05f) || any;
        check(!any, "F116: single frames over 15% between frames under it never hide the HUD");
        bool shown = false;
        int frames = 0;
        for (; frames < 20 && !shown; ++frames) shown = !h.step(0.05f);
        check(shown && frames == 10, "F116: the HUD shows again on the tenth frame in a row under 10%");
        f116::Hysteresis band;
        band.step(0.30f);
        band.step(0.30f);
        bool held = true;
        for (int i = 0; i < 40; ++i) held = band.step(0.12f) && held;
        for (int i = 0; i < 9; ++i) band.step(0.05f);
        const bool stillHidden = band.step(0.12f);   // a band frame restarts the count
        int more = 0;
        while (band.step(0.05f) && more < 20) ++more;
        check(held && stillHidden && more == 9, "F116: 10-15% keeps the HUD hidden and restarts the count to show it");
        f116::Hysteresis low;
        bool hid = false;
        for (int i = 0; i < 40; ++i) hid = low.step(0.12f) || hid;
        check(!hid && !low.step(0.15f) && !low.step(0.15f), "F116: 10-15% (and exactly 15%) never hides a shown HUD");
    }
    {   // the frame order: frames handed on in order, a frame with a screen waiting for its row, none at once as 0, a skipped census unknown
        f116::FrameOrder q;
        std::vector<std::string> got;
        const auto take = [&](uint64_t frame, bool known, float fp) {
            char b[48];
            std::snprintf(b, sizeof(b), "%llu:%s:%.2f", static_cast<unsigned long long>(frame), known ? "k" : "u", static_cast<double>(fp));
            got.emplace_back(b);
        };
        q.none(10);
        q.waiting(11, 100, 100);
        q.none(12);
        q.unknown(13);
        q.waiting(14, 100, 200);
        q.pop(take);
        check(got.size() == 1 && got[0] == "10:k:0.00" && q.size() == 4, "F116: a frame waiting for its census row holds the frames after it");
        q.row(11, true, 30, 10);
        q.pop(take);
        check(got.size() == 4 && got[1] == "11:k:0.30" && got[2] == "12:k:0.00" && got[3] == "13:u:0.00" && q.size() == 1,
              "F116: its row hands it and the frames behind it on, a skipped census as unknown");
        q.row(99, true, 50, 50);   // a row for no waiting frame changes nothing
        q.row(14, false, 0, 0);    // lost on read-back
        q.pop(take);
        check(got.size() == 5 && got[4] == "14:u:0.00" && q.size() == 0, "F116: a row lost on read-back hands its frame on as unknown");
        f116::FrameOrder full;
        for (uint64_t f = 0; f < f116::FrameOrder::kCap + 3; ++f) full.waiting(f, 1, 1);
        check(full.size() == f116::FrameOrder::kCap && full.dropped == 3, "F116: a full queue drops its oldest waiting frames, counted");
    }
    {   // the hook's decision: the hide, stepped, and fresh
        check(f116::decideSkip(true, true, 0) && f116::decideSkip(true, true, f116::kFreshMs) && !f116::decideSkip(true, true, f116::kFreshMs + 1) &&
                  !f116::decideSkip(true, false, 0) && !f116::decideSkip(false, true, 0),
              "F116: the submit is skipped only while the hide holds and the render thread stepped it within 250 ms");
    }
    {   // the counters' read (logged only): every refusal before the value
        using R = f116::Read;
        check(f116::readCounters(false, true, 1, true, true, true) == R::kNotVerified && f116::readCounters(true, false, 0, false, false, false) == R::kUnreadable &&
                  f116::readCounters(true, true, 0, false, false, false) == R::kNoController &&
                  f116::readCounters(true, true, 0x1000, false, false, false) == R::kUnreadable &&
                  f116::readCounters(true, true, 0x1000, true, false, false) == R::kWrongClass &&
                  f116::readCounters(true, true, 0x1000, true, true, false) == R::kUnreadable && f116::readCounters(true, true, 0x1000, true, true, true) == R::kRead,
              "F116: the counters are read only from a verified controller of the verified class");
        check(f116::pinDword(f116::kTestPin, f116::kTestPinCounterAt) == 0x380u && f116::kSlotTargets[5] == f116::kTestRva && f116::kSlots[5] == 0x80u &&
                  f116::kSlots[4] == 0x78u,
              "F116: the test reads +0x380, at slot +0x80 beside slot +0x78 (the one EDVR's creation patch asks)");
    }
    {   // the episodes: the hide's (frames, calls and skips while it held, the footprint peak) and the counters' (log only)
        f116::TerminalEpisode t;
        check(t.step(false, 0.01f, 1000, 10, 0) == f116::TerminalEpisode::kNone, "F116: no episode while shown");
        check(t.step(true, 0.20f, 1011, 11, 0) == f116::TerminalEpisode::kBegan && t.number == 1, "F116: the hide begins an episode");
        t.step(true, 0.34f, 1022, 12, 1);
        t.step(true, 0.25f, 1033, 13, 2);
        check(t.step(false, 0.05f, 2511, 140, 129) == f116::TerminalEpisode::kEnded && t.frames == 3 && t.calls == 129 && t.skipped == 129 &&
                  std::fabs(t.peak - 0.34f) < 1e-6f && std::fabs(t.lengthS() - 1.5) < 1e-9,
              "F116: the episode ends with its frames, its length, its calls and skips, and its footprint peak");
        f116::CounterEpisode c;
        c.step(0, 0, 0.0f, 100);
        check(c.step(1, 0, 0.30f, 111) == f116::CounterEpisode::kBegan && c.startHide == 1, "F116: a counter rising begins its episode");
        c.step(2, 1, 0.31f, 122);
        check(c.step(0, 0, 0.02f, 611) == f116::CounterEpisode::kEnded && c.maxHide == 2 && c.maxSibling == 1 && c.frames == 2 &&
                  std::fabs(c.lengthS() - 0.5) < 1e-9,
              "F116: both falling ends it, with the counters' largest values and its length");
    }
    {   // the HUD submitted with a terminal over 15%: seconds, the longest run, and a skip or a frame under 15% ending the run
        f116::SubmittedRun r;
        for (int i = 0; i < 2; ++i) r.step(true, 0.30f, true, false, 0.011);   // the hysteresis's two frames
        r.step(true, 0.30f, true, true, 0.011);
        check(r.longestS < 0.03 && r.runS == 0.0, "F116: the hysteresis's two frames are a run of 22 ms, ended by the skip");
        for (int i = 0; i < 250; ++i) r.step(true, 0.30f, true, false, 0.011);
        check(r.longestS > 2.7 && r.longestS < 2.8, "F116: a terminal over 15% with the HUD submitted for 2.75 s is a 2.75 s run");
        r.resetWindow();
        r.step(true, 0.30f, false, false, 0.011);   // no hook call: no HUD to hide
        check(r.totalS == 0.0 && r.runS == 0.0, "F116: a frame with no HRC call adds nothing and ends the run");
    }
    {   // the lines, in the order the reader parses them
        char line[700];
        f116::TerminalEpisode t;
        t.step(true, 0.312f, 1000, 5, 0);
        t.step(true, 0.345f, 1500, 50, 45);
        t.step(false, 0.01f, 2200, 60, 55);
        f116::formatTerminalEnded(line, sizeof(line), t);
        check(std::strcmp(line, "HUD hide (F116): terminal episode 1 ended after 1.20 s, 2 frames: footprint peak 34.5%; HRC calls 55, skipped 55 (frames the HUD "
                                "was hidden).") == 0,
              "F116: the terminal episode's line");
        f116::Window w;
        w.calls = 2700;
        w.skipped = 1200;
        w.stepped = 2690;
        w.censusRows = 1500;
        w.upscaler = true;
        w.over = 1210;
        w.hiddenFrames = 1205;
        w.terminalBegan = w.terminalEnded = 1;
        w.readCalls = 2700;
        w.hideSet = 3;
        w.counterBegan = w.counterEnded = 1;
        f116::SubmittedRun s;
        s.step(true, 0.3f, true, false, 0.022);
        const int n = f116::formatWindow(line, sizeof(line), w, s, 0.004f);
        check(n > 0 && n < static_cast<int>(sizeof(line)) &&
                  std::strcmp(line, "HUD hide (F116) 30 s: HRC calls 2700, skipped 1200 (the HUD hidden); terminal frames 2690 (census rows 1500, an external upscaler "
                                    "ran): over 15% 1210, hidden 1205; terminal episodes began 1, ended 1; submitted with a terminal over 15% 0.0 s, longest 0.0 s; "
                                    "footprint now 0.4%; the game's counters (logged only): calls read 2700, refused 0 (not verified 0, no controller 0, wrong class "
                                    "0, unreadable 0), with +0x380 set 3, with +0x384 set 0, rises 1, falls 1. Limits: F105-pair terminals only, and only with an "
                                    "external upscaler.") == 0,
              "F116: the 30 s line");
    }
}

}  // namespace f116test
