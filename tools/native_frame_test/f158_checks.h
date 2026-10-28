// F158 checks (native on-foot VR), run158 below. Header only, run by native_frame_test.
//
// F158 (native_on_foot_f158.h, MEASURE ONLY): the local body's draws over the first 30 s on foot, to find the coat pack's lower-detail mesh. The window's open and close rules (a rising
// edge of native on foot, the coat decision, one window at a time, 30 s or the end of native on foot), the list of identities with their times, frames, variants and previous draw, the
// cap of 256 and its overflow, the pack's switch time, the adjacency test, the stop test, and the lines that are written once at the window's end.
#pragma once

#include "../../src/d3d11/native_on_foot_f158.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>

namespace f158test {

namespace f158 = edvr::nofoot::f158;

inline f158::Identity ident158(uint64_t vs, uint64_t ps, uint32_t count, uint32_t instances = 1) {
    f158::Identity id;
    id.vs = vs;
    id.ps = ps;
    id.count = count;
    id.instances = instances;
    return id;
}
inline f158::Variant var158(uint64_t ib, uint32_t start, int32_t base = 0) {
    f158::Variant v;
    v.ib = ib;
    v.start = start;
    v.base = base;
    return v;
}
inline f158::Prev prev158(uint64_t ib, uint32_t start, uint32_t count) {
    f158::Prev p;
    p.valid = true;
    p.ib = ib;
    p.start = start;
    p.count = count;
    return p;
}

template <class Check>
inline void run158(Check&& check) {
    constexpr uint64_t packVs = 0x7B0DC42D383F694Cull, packPs = 0x0DF03E64DF9DBEF1ull;
    using f158::Ended;
    using f158::Trigger;
    // ---- 1. the window: when it opens, and when it closes -----------------------------------------------------------------------------------------------------------------------------
    {
        f158::WindowRules w;
        f158::WindowRules::Step s = w.step(false, false, 0);
        check(!w.open() && s.opened == Trigger::kNone && s.closed == Ended::kNone, "F158: native on foot not active: no window");
        s = w.step(false, true, 100);
        check(!w.open() && s.opened == Trigger::kNone, "F158: ... the coat decided but native on foot not active: no window (the decision is not a trigger on its own)");
        s = w.step(true, false, 1000);
        check(w.open() && s.opened == Trigger::kNativeOnFoot && w.startMs() == 1000, "F158: the rising edge of native on foot opens a window, counted from that step");
        s = w.step(true, false, 30999);
        check(w.open() && s.closed == Ended::kNone, "F158: ... still open at 29999 ms");
        s = w.step(true, false, 31000);
        check(!w.open() && s.closed == Ended::kTime && s.closedMs == 30000 && s.closedTrigger == Trigger::kNativeOnFoot && s.closedIgnored == 0 && s.opened == Trigger::kNone,
              "F158: ... closed by the clock at 30000 ms, opened by native on foot beginning, no trigger ignored");
        s = w.step(true, false, 31500);
        check(!w.open() && s.opened == Trigger::kNone, "F158: native on foot still active after the window closed: no new window (no edge)");
        s = w.step(true, true, 40000);
        check(w.open() && s.opened == Trigger::kCoatOutfit && w.startMs() == 40000, "F158: the decision changing to the outer garment while active opens a window of its own");
        s = w.step(true, true, 50000);
        check(w.open() && s.closed == Ended::kNone && s.opened == Trigger::kNone, "F158: ... the decision staying the outer garment is no further trigger");
        s = w.step(false, true, 52000);
        check(!w.open() && s.closed == Ended::kNativeOnFoot && s.closedMs == 12000 && s.closedTrigger == Trigger::kCoatOutfit, "F158: native on foot ending closes the window at once, after 12000 ms");
        s = w.step(false, false, 53000);
        check(!w.open() && s.closed == Ended::kNone && s.opened == Trigger::kNone, "F158: ... and nothing more while it is not active");
    }
    {   // one window at a time: the triggers that come while one runs are counted and open nothing
        f158::WindowRules w;
        w.step(true, false, 1000);
        f158::WindowRules::Step s = w.step(true, true, 3000);
        check(w.open() && s.opened == Trigger::kNone && w.startMs() == 1000, "F158: one window at a time: the coat decision 2 s into a window opens no second one and the window keeps its start");
        w.step(true, false, 4000);   // the decision flaps off ...
        w.step(true, true, 5000);    // ... and on again
        s = w.step(true, true, 31000);
        check(s.closed == Ended::kTime && s.closedIgnored == 2 && s.closedTrigger == Trigger::kNativeOnFoot, "F158: ... both decisions are counted as ignored triggers (2) and named in the window's end");
        s = w.step(true, true, 32000);
        check(!w.open() && s.opened == Trigger::kNone, "F158: ... the decision still the outer garment after the window closed: no new window (it did not change)");
        w.step(true, false, 33000);
        s = w.step(true, true, 34000);
        check(w.open() && s.opened == Trigger::kCoatOutfit, "F158: ... a later change to the outer garment opens one");
    }
    {   // the rising edge with the decision already made is one trigger, the edge
        f158::WindowRules w;
        f158::WindowRules::Step s = w.step(true, true, 5000);
        check(s.opened == Trigger::kNativeOnFoot && w.open(), "F158: native on foot beginning with the coat already decided: one window, opened by native on foot beginning");
        s = w.step(true, true, 35000);
        check(s.closed == Ended::kTime && s.closedIgnored == 0, "F158: ... the coat decision at the same step is not counted as an ignored trigger");
    }
    {   // leaving and coming back: the decision is read fresh
        f158::WindowRules w;
        w.step(true, true, 0);
        w.step(false, true, 1000);
        const f158::WindowRules::Step s = w.step(true, true, 2000);
        check(s.opened == Trigger::kNativeOnFoot && w.startMs() == 2000, "F158: native on foot ended and began again with the coat decided: the edge opens a new window");
    }
    {   // the clock closes a window in the same step a trigger comes: the trigger opens the next one
        f158::WindowRules w;
        w.step(true, false, 0);
        const f158::WindowRules::Step s = w.step(true, true, 30000);
        check(s.closed == Ended::kTime && s.closedTrigger == Trigger::kNativeOnFoot && s.opened == Trigger::kCoatOutfit && w.open() && w.startMs() == 30000,
              "F158: the window closed by the clock in the step the coat was decided: the next window opens in that step, and the closed one reports the first trigger");
    }
    {   // a clock that stands still or steps back does not wrap the window's length
        f158::WindowRules w;
        w.step(true, false, 10000);
        const f158::WindowRules::Step s = w.step(true, false, 9000);
        check(w.open() && s.closed == Ended::kNone, "F158: a clock behind the window's start counts 0 ms, not a wrapped length");
    }
    check(f158::listable(21) && !f158::listable(20) && !f158::listable(22) && !f158::listable(0), "F158: only stencil ref 21 (the local body) is listed, with any shader");

    // ---- 2. the list ---------------------------------------------------------------------------------------------------------------------------------------------------------------------
    {
        auto logp = std::make_unique<f158::SpawnLog>();
        f158::SpawnLog& log = *logp;
        log.begin(1000);
        check(log.count() == 0 && log.switchMs() == f158::kNever && log.refused() == 0, "F158: a window opens on an empty list, no switch time, nothing turned away");
        const f158::Identity a = ident158(0xAAull, 0xBBull, 3474);
        f158::SpawnLog::Note n = log.note(a, var158(0x1000, 500, 7), prev158(0x1000, 100, 400), 0, 10, 1000);
        check(n.index == 0 && n.added && log.count() == 1, "F158: the first draw is listed as identity 0");
        const f158::Entry& e = log.at(0);
        check(e.id.vs == 0xAAull && e.id.ps == 0xBBull && e.id.count == 3474 && e.id.instances == 1, "F158: ... with its shaders, index count and instance count");
        check(e.first.ib == 0x1000 && e.first.start == 500 && e.first.base == 7 && e.variants == 1, "F158: ... the first variant (index buffer, start index, base vertex) and one variant");
        check(e.prev.valid && e.prev.ib == 0x1000 && e.prev.start == 100 && e.prev.count == 400, "F158: ... the previous indexed draw of its eye at first sight");
        check(e.firstMs == 0 && e.lastMs == 0 && e.frames[0] == 1 && e.frames[1] == 0, "F158: ... first and last at 0 ms from the window's start, one frame in eye 0");
        n = log.note(a, var158(0x1000, 500, 7), prev158(0x9, 9, 9), 0, 10, 1004);
        check(n.index == 0 && !n.added && log.at(0).frames[0] == 1 && log.at(0).variants == 1 && log.at(0).lastMs == 4, "F158: the same draw again in the same frame: the frame counts once, the variant is not new, the last time moves to 4 ms");
        check(log.at(0).prev.start == 100, "F158: ... the previous draw is kept from first sight");
        log.note(a, var158(0x1000, 900, 7), prev158(0, 0, 0), 1, 10, 1006);
        log.note(a, var158(0x2000, 900, 7), prev158(0, 0, 0), 0, 11, 1500);
        log.note(a, var158(0x2000, 900, 8), prev158(0, 0, 0), 1, 11, 1502);
        check(log.at(0).frames[0] == 2 && log.at(0).frames[1] == 2 && log.at(0).lastMs == 502, "F158: frames are counted per eye, once each: 2 in eye 0 and 2 in eye 1, last at 502 ms");
        check(log.at(0).variants == 4 && log.at(0).first.start == 500, "F158: the start index, the index buffer and the base vertex each make a variant (4), and the first one stays the first");
        log.note(a, var158(0x1000, 500, 7), prev158(0, 0, 0), -1, 12, 1600);
        log.note(a, var158(0x1000, 500, 7), prev158(0, 0, 0), 7, 12, 1601);
        check(log.at(0).frames[2] == 1 && log.at(0).variants == 4, "F158: an eye the target's table could not name counts apart (frame 12 once), and a repeated variant is no new one");
        const f158::Identity b = ident158(0xAAull, 0xBBull, 3475);
        const f158::Identity c = ident158(0xAAull, 0xBCull, 3474);
        const f158::Identity d = ident158(0xABull, 0xBBull, 3474);
        const f158::Identity g = ident158(0xAAull, 0xBBull, 3474, 2);
        int idx[4] = {log.note(b, var158(1, 1), prev158(0, 0, 0), 0, 20, 2000).index, log.note(c, var158(1, 1), prev158(0, 0, 0), 0, 20, 2000).index,
                      log.note(d, var158(1, 1), prev158(0, 0, 0), 0, 20, 2000).index, log.note(g, var158(1, 1), prev158(0, 0, 0), 0, 20, 2000).index};
        check(idx[0] == 1 && idx[1] == 2 && idx[2] == 3 && idx[3] == 4 && log.count() == 5,
              "F158: one field off (index count, pixel shader, vertex shader, instance count) is another identity each, numbered in first-seen order");
        log.begin(5000);
        check(log.count() == 0 && log.switchMs() == f158::kNever, "F158: begin() empties the list");
        n = log.note(b, var158(1, 1), f158::Prev{}, 0, 30, 5100);
        check(n.index == 0 && n.added && log.at(0).firstMs == 100 && !log.at(0).prev.valid, "F158: ... the numbers start at 0 again and the times count from the new start (100 ms)");
        log.note(a, var158(1, 1), f158::Prev{}, 1, 31, 4000);
        check(log.at(1).firstMs == 0, "F158: a clock behind the window's start reads 0 ms");
    }
    {   // the previous indexed draw, per eye
        auto logp = std::make_unique<f158::SpawnLog>();
        f158::SpawnLog& log = *logp;
        log.begin(0);
        f158::Prev p = log.swapPrev(0, prev158(0x1, 10, 20));
        check(!p.valid, "F158: the first indexed draw of an eye has no previous draw");
        p = log.swapPrev(1, prev158(0x2, 30, 40));
        check(!p.valid, "F158: ... each eye has its own");
        p = log.swapPrev(0, prev158(0x3, 50, 60));
        check(p.valid && p.ib == 0x1 && p.start == 10 && p.count == 20, "F158: the next draw of eye 0 sees the one before it in eye 0, not eye 1's");
        p = log.swapPrev(-1, prev158(0x4, 1, 2));
        p = log.swapPrev(9, prev158(0x5, 3, 4));
        check(p.valid && p.ib == 0x4, "F158: an eye the table could not name has a slot of its own");
        log.begin(0);
        check(!log.swapPrev(0, prev158(0x6, 1, 1)).valid, "F158: begin() forgets the previous draws");
    }

    // ---- 3. the cap and the overflow ---------------------------------------------------------------------------------------------------------------------------------------------------
    {
        auto logp = std::make_unique<f158::SpawnLog>();
        f158::SpawnLog& log = *logp;
        log.begin(0);
        for (uint32_t i = 0; i < f158::kCap; ++i) log.note(ident158(1000 + i, 7, 100 + i), var158(1, i), f158::Prev{}, 0, 1, i);
        check(log.count() == f158::kCap && f158::kCap == 256 && log.refused() == 0, "F158: 256 identities are listed");
        const f158::Identity over1 = ident158(5000, 7, 100), over2 = ident158(5001, 7, 100);
        f158::SpawnLog::Note n = log.note(over1, var158(1, 1), f158::Prev{}, 0, 2, 300);
        check(n.index == -1 && !n.added && log.count() == 256 && log.refused() == 1 && log.turnedIdentities() == 1, "F158: the 257th identity is not listed: 1 draw turned away, 1 identity");
        log.note(over1, var158(1, 2), f158::Prev{}, 1, 2, 301);
        log.note(over2, var158(1, 3), f158::Prev{}, 0, 3, 302);
        check(log.refused() == 3 && log.turnedIdentities() == 2 && log.turnedLost() == 0, "F158: ... the same one again counts a draw and not an identity; another counts both (3 draws, 2 identities)");
        n = log.note(ident158(1000, 7, 100), var158(1, 0), f158::Prev{}, 1, 3, 400);
        check(n.index == 0 && !n.added && log.at(0).lastMs == 400 && log.at(0).frames[1] == 1, "F158: ... a listed identity is still found and updated with the list full");
        // the table of turned-away identities holds 768; past it they are counted as lost
        for (uint32_t i = 0; i < 900; ++i) log.note(ident158(9000 + i, 9, 50), var158(1, 1), f158::Prev{}, 0, 4, 500);
        check(log.turnedIdentities() == f158::kTurnedCap && log.turnedLost() > 0 && f158::kTurnedCap == 768, "F158: past 768 turned-away identities the count stops at 768 and the draws beyond are counted as lost");
        char line[1100];
        f158::formatHeader(line, sizeof(line), 4, Trigger::kNativeOnFoot, 30002, Ended::kTime, log, 0);
        check(std::strstr(line, "256 identity(ies) listed") && std::strstr(line, "identity(ies) past the cap of 256 turned away") && std::strstr(line, "768+ identity(ies)"),
              "F158: the header says 256 listed and the overflow (a plus once the count is a lower bound)");
    }
    {   // the variants per identity: 64 counted, the rest said to be lost
        auto logp = std::make_unique<f158::SpawnLog>();
        f158::SpawnLog& log = *logp;
        log.begin(0);
        const f158::Identity a = ident158(1, 2, 3);
        for (uint32_t i = 0; i < 70; ++i) log.note(a, var158(1, i), f158::Prev{}, 0, i, i);
        check(log.at(0).variants == f158::kVariantCap && log.at(0).variantsLost == 6 && f158::kVariantCap == 64, "F158: 64 variants are counted; the 6 draws past them are counted as lost");
        char line[1100];
        f158::formatIdentity(line, sizeof(line), 1, 0, log.at(0), 70);
        check(std::strstr(line, "64+ variant(s)") != nullptr, "F158: ... the identity's line says 64+ variant(s)");
    }

    // ---- 4. the pack's switch time -----------------------------------------------------------------------------------------------------------------------------------------------------
    {
        auto logp = std::make_unique<f158::SpawnLog>();
        f158::SpawnLog& log = *logp;
        log.begin(2000);
        log.note(ident158(packVs, packPs, 3474), var158(1, 1), f158::Prev{}, 0, 1, 2100);
        check(log.switchMs() == f158::kNever, "F158: entry 78 (the pack's shaders, 3474 indices) is not the pack: no switch time");
        log.note(ident158(packVs ^ 1, packPs, 30312), var158(1, 1), f158::Prev{}, 0, 1, 2100);
        log.note(ident158(packVs, packPs ^ 1, 30312), var158(1, 1), f158::Prev{}, 0, 1, 2100);
        log.note(ident158(packVs, packPs, 30312, 2), var158(1, 1), f158::Prev{}, 0, 1, 2100);
        check(log.switchMs() == f158::kNever, "F158: ... nor is a draw with another shader or another instance count");
        log.note(ident158(packVs, packPs, 30312), var158(5, 5), f158::Prev{}, 1, 7, 9123);
        check(log.switchMs() == 7123, "F158: the pack's first draw sets the switch time: 7123 ms from the window's start");
        log.note(ident158(packVs, packPs, 30312), var158(5, 6), f158::Prev{}, 1, 8, 12000);
        check(log.switchMs() == 7123, "F158: ... a later draw of it does not move it");
        char line[1100];
        f158::formatHeader(line, sizeof(line), 1, Trigger::kNativeOnFoot, 30001, Ended::kTime, log, 0);
        check(std::strstr(line, "the pack first drawn at 7123 ms") != nullptr, "F158: the header carries the switch time");
    }
    {   // the pack drawn after the list is full: still the switch time
        auto logp = std::make_unique<f158::SpawnLog>();
        f158::SpawnLog& log = *logp;
        log.begin(0);
        for (uint32_t i = 0; i < f158::kCap; ++i) log.note(ident158(1000 + i, 7, 100 + i), var158(1, i), f158::Prev{}, 0, 1, i);
        const f158::SpawnLog::Note n = log.note(ident158(packVs, packPs, 30312), var158(1, 1), f158::Prev{}, 0, 2, 4400);
        check(n.index == -1 && log.switchMs() == 4400, "F158: the pack turned away by a full list still sets the switch time");
    }

    // ---- 5. the stop test, the adjacency test -----------------------------------------------------------------------------------------------------------------------------------------
    check(f158::stopped(9499, 10000) && !f158::stopped(9500, 10000) && !f158::stopped(10000, 10000) && !f158::stopped(10001, 10000) && f158::kStopMs == 500,
          "F158: stopped when the last frame was more than 0.5 s before the window's end: 501 ms yes, 500 ms no, at the end or past it no");
    check(f158::stoppedNearSwitch(7000, 30000, 7000) && f158::stoppedNearSwitch(8000, 30000, 7000) && !f158::stoppedNearSwitch(8001, 30000, 7000) &&
              f158::stoppedNearSwitch(6000, 30000, 7000) && !f158::stoppedNearSwitch(5999, 30000, 7000) && f158::kNearMs == 1000,
          "F158: stopped near the switch when it stopped and its last frame is within 1 s of the switch time, before or after (1000 ms yes, 1001 ms no)");
    check(!f158::stoppedNearSwitch(29800, 30000, 29500) && !f158::stoppedNearSwitch(7000, 30000, f158::kNever),
          "F158: ... an identity still drawn at the end is not near the switch, and without a switch time none is");
    {
        f158::Entry a, b;
        a.id = ident158(1, 1, 3474);
        a.first = var158(0x1DF15DD06A0, 3879112);
        b.id = ident158(1, 1, 30312);
        b.first = var158(0x1DF15DD06A0, 3882586);
        check(f158::adjacent(a, b) && !f158::adjacent(b, a), "F158: the flight's pair is adjacent: start 3879112 + 3474 = 3882586 (a before b, not b before a)");
        b.first.ib = 0x1DF15DD0700;
        check(!f158::adjacent(a, b), "F158: ... not when the index buffer differs");
        b.first = var158(0x1DF15DD06A0, 3882587);
        check(!f158::adjacent(a, b), "F158: ... not with a gap of one index");
        b.first = var158(0x1DF15DD06A0, 3882585);
        check(!f158::adjacent(a, b), "F158: ... not when the ranges overlap by one");
        a.first.ib = b.first.ib = 0;
        a.first.start = 0;
        b.first.start = 3474;
        check(!f158::adjacent(a, b), "F158: ... not when no index buffer was read (0)");
        a.first = var158(7, 0xFFFFFF00u);
        a.id.count = 0x200;
        b.first = var158(7, 0x100u);
        check(!f158::adjacent(a, b), "F158: ... the end of a range is not wrapped around 32 bits");
    }

    // ---- 6. a window as the flight would show it, and the lines written at its end ---------------------------------------------------------------------------------------------------
    {
        auto logp = std::make_unique<f158::SpawnLog>();
        f158::SpawnLog& log = *logp;
        log.begin(10000);
        constexpr uint64_t lodVs = 0x1111111111111111ull, lodPs = 0x2222222222222222ull, otherVs = 0x3333333333333333ull;
        uint32_t frame = 100;
        // one body draw as the draw path sees it: it becomes the previous draw of its eye for the next one, and is listed with the previous one it replaced
        const auto body = [&](int eye, const f158::Identity& id, uint64_t ib, uint32_t start, int32_t base, uint64_t now) {
            const f158::Prev before = log.swapPrev(eye, prev158(ib, start, id.count));
            log.note(id, var158(ib, start, base), before, eye, frame, now);
        };
        // frames from 0 to 7000 ms: two lower-detail meshes adjacent in index buffer 100, an unrelated mesh in 200; from 7100 ms the full-detail pair (3474 then 30312) in index buffer 300
        for (uint32_t ms = 0; ms <= 30000; ms += 100) {
            ++frame;
            const uint64_t now = 10000 + ms;
            for (int eye = 0; eye < 2; ++eye) {
                log.swapPrev(eye, prev158(0xFF, 9, 9));   // a draw of something else first, never listed
                if (ms <= 7000) {
                    body(eye, ident158(lodVs, lodPs, 1200), 0x100, 1000, 0, now);
                    body(eye, ident158(lodVs, lodPs, 6000), 0x100, 2200, 0, now);
                }
                body(eye, ident158(otherVs, lodPs, 900), 0x200, 60, 0, now);
                if (ms >= 7100) {
                    body(eye, ident158(packVs, packPs, 3474), 0x300, 3879112, 11 * static_cast<int32_t>(frame), now);
                    body(eye, ident158(packVs, packPs, 30312), 0x300, 3882586, 5 * static_cast<int32_t>(frame), now);
                }
            }
        }
        check(log.count() == 5 && log.switchMs() == 7100, "F158: the replayed window lists 5 identities and the pack's switch time is 7100 ms");
        check(log.at(0).lastMs == 7000 && log.at(1).lastMs == 7000 && log.at(2).lastMs == 30000 && log.at(3).firstMs == 7100 && log.at(4).firstMs == 7100,
              "F158: ... the lower-detail meshes' last frame is 7000 ms, the unrelated mesh runs to the end, the full-detail pair starts at 7100 ms");
        check(log.at(0).prev.valid && log.at(0).prev.ib == 0xFF && log.at(1).prev.ib == 0x100 && log.at(1).prev.start == 1000 && log.at(1).prev.count == 1200,
              "F158: ... the second lower-detail mesh was first seen right after the first (its previous draw is the first's range)");
        const f158::PairCount pc = f158::countPairs(log, 30000);
        check(pc.total == 2 && pc.marked == 1, "F158: two adjacent pairs, one of them with identities that stopped near the switch (the lower-detail pair; the full-detail pair never stopped)");
        char line[1100];
        f158::formatHeader(line, sizeof(line), 3, Trigger::kNativeOnFoot, 30003, Ended::kTime, log, 2);
        check(std::strstr(line, "body draws at spawn (F158) window 3: opened by native on foot beginning, ran 30003 ms (ended by the clock), the pack first drawn at 7100 ms, 5 identity(ies) listed") &&
                  std::strstr(line, "0 draw(s) of 0 identity(ies) past the cap of 256 turned away, 2 later trigger(s) ignored"),
              "F158: the header: the trigger, the length and what ended it, the switch time, the count listed, the overflow and the ignored triggers");
        f158::formatIdentity(line, sizeof(line), 3, 0, log.at(0), 30003);
        check(std::strstr(line, "window 3 identity 0: vs 1111111111111111 ps 2222222222222222 count 1200 instances 1, first 0 ms, last 7000 ms, frames eye 0 71, eye 1 71, ") &&
                  std::strstr(line, "first variant ib 100 start 1000 base 0, 1 variant(s), previous draw ib FF start 9 count 9, stopped at 7000 ms"),
              "F158: an identity line: the shaders, counts, first and last ms, frames per eye, the first variant, the variant count, the previous draw, and when it stopped");
        f158::formatIdentity(line, sizeof(line), 3, 2, log.at(2), 30003);
        check(std::strstr(line, "stopped at") == nullptr && std::strstr(line, "is the pack") == nullptr, "F158: ... an identity drawn to the end has no stop and is not the pack");
        f158::formatIdentity(line, sizeof(line), 3, 4, log.at(4), 30003);
        check(std::strstr(line, "count 30312 instances 1, first 7100 ms, last 30000 ms") && std::strstr(line, "variant(s), previous draw ib 300 start 3879112 count 3474, is the pack") &&
                  std::strstr(line, "stopped at") == nullptr,
              "F158: the pack's line names it, and its previous draw is the 3474-index range just before it");
        f158::formatIdentity(line, sizeof(line), 3, 3, log.at(3), 30003);
        check(std::strstr(line, "count 3474") && std::strstr(line, ", is the pack") == nullptr, "F158: entry 78 (3474 indices, the pack's shaders) is not named the pack");
        check(std::strstr(line, "variant(s)") && log.at(3).variants > 1, "F158: ... and its base vertex changes from frame to frame are merged as variants");
        f158::formatPairs(line, sizeof(line), 3, log, 30003);
        check(std::strstr(line, "window 3 adjacent pairs: 2 pair(s), 1 with an identity stopped within 1 s of the switch time: 0>1 stopped:both, 3>4") != nullptr,
              "F158: the pairs line: the marked pair (0>1, both stopped) first, then the unmarked one (3>4, the full-detail pair)");
        // the same window read when it ended early (native on foot ended at 7500 ms): the lower-detail meshes stopped 500 ms before the end -> not stopped
        check(!f158::stopped(log.at(0).lastMs, 7500) && f158::stopped(log.at(0).lastMs, 7501), "F158: a window that ended at 7500 ms: the meshes that stopped at 7000 ms are still drawn (500 ms is not more than 500 ms)");
    }
    {   // an empty window is told from a window that never ran by its header
        auto logp = std::make_unique<f158::SpawnLog>();
        f158::SpawnLog& log = *logp;
        log.begin(0);
        char line[1100];
        f158::formatHeader(line, sizeof(line), 1, Trigger::kCoatOutfit, 30001, Ended::kTime, log, 0);
        check(std::strstr(line, "opened by the coat outfit decided") && std::strstr(line, "the pack not drawn in the window, 0 identity(ies) listed"), "F158: a window that listed nothing says 0 identity(ies) listed and the pack not drawn");
        f158::formatPairs(line, sizeof(line), 1, log, 30001);
        check(std::strstr(line, "adjacent pairs: 0 pair(s), 0 with an identity stopped within 1 s of the switch time: none") != nullptr, "F158: ... and no pairs says none");
    }
    {   // the pack and a run of 255 identities adjacent in one index buffer (the list is exactly full): the line stops at its budget, says how many more, and lists the marked pairs first
        auto logp = std::make_unique<f158::SpawnLog>();
        f158::SpawnLog& log = *logp;
        log.begin(0);
        log.note(ident158(packVs, packPs, 30312), var158(1, 1), f158::Prev{}, 0, 1, 7000);   // entry 0, the switch at 7000 ms
        uint32_t start = 0;
        for (uint32_t i = 0; i < 255; ++i) {   // entries 1..255; entries 201 and 202 stopped at 6900 ms, the rest are drawn until 29000 ms
            log.note(ident158(2000 + i, 5, 100), var158(0x77, start), f158::Prev{}, 0, 2 + i, i == 200 || i == 201 ? 6900 : 29000);
            start += 100;
        }
        char line[1100];
        const int n = f158::formatPairs(line, sizeof(line), 2, log, 30000);
        const f158::PairCount pc = f158::countPairs(log, 30000);
        check(log.count() == 256 && pc.total == 254 && pc.marked == 3 && n < static_cast<int>(sizeof(line)) - 1 && std::strstr(line, " more") != nullptr,
              "F158: 255 identities in one run of an index buffer: 254 pairs, 3 of them with a stopped identity; the line fits and says how many more it left out");
        check(std::strstr(line, "254 pair(s), 3 with an identity stopped within 1 s of the switch time: 200>201 stopped:b, 201>202 stopped:both, 202>203 stopped:a, 1>2, 2>3, ") != nullptr,
              "F158: ... the pairs with a stopped identity come first whatever their place in the list, marked b (the second stopped), both and a (the first stopped), then the rest in order");
    }
}

}  // namespace f158test
