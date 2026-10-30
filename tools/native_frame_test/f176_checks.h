// F176 checks (native on-foot VR), run176 below. Header only, run by native_frame_test.
//
// F176 (native_on_foot_f176.h): with the head aiming in the sights in mouse look (fix.native_on_foot_ads_head_aim on, look to aim off), the mouse's and the stick's pitch are held; the yaws
// are not touched. The rig pins the decision (an active press, the head aim on, look to aim neither engaged nor unwinding, the hook on, the guard not tripped), the values a look update runs
// with (both pitches zero while held, the player's |value| counted, the yaws and look to aim's own drop untouched), the press's account and its line (a press where the hold never ran
// reads differently from one where it ran and the mouse was still), and, by source pins, where the game's look update applies it and that every path that clears the hook clears the flag.
#pragma once

#include "../../src/d3d11/native_on_foot_f109.h"
#include "../../src/d3d11/native_on_foot_f176.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>

namespace f176test {

namespace f109 = edvr::nofoot::f109;
namespace f176 = edvr::nofoot::f176;

template <class Check>
inline void run176(Check&& check) {
    char msg[512];

    // ---- 1. the decision ------------------------------------------------------------------------------------------------------------------------------------------------------------------------
    check(f176::holds(true, true, false, false, true, false), "F176: held in an active press with the head aim on, look to aim off, the hook on and the guard quiet");
    check(!f176::holds(false, true, false, false, true, false), "F176: not held outside an active press (idle, or releasing: the head aim's unwind rides the same hook)");
    check(!f176::holds(true, false, false, false, true, false), "F176: not held with the head aim off (the mouse looks up and down as the game does)");
    check(!f176::holds(true, true, true, false, true, false) && !f176::holds(true, true, false, true, true, false), "F176: not held while look to aim is engaged (it drops the pitch itself) or unwinding");
    check(!f176::holds(true, true, false, false, false, false), "F176: not held with the update hook off");
    check(!f176::holds(true, true, false, false, true, true), "F176: a guard trip gives the mouse's pitch back for the rest of the press");

    // ---- 2. the values a look update runs with ---------------------------------------------------------------------------------------------------------------------------------------------------
    {
        const f176::Pitches p = f176::apply(true, 0.40, -0.25);
        check(p.mouse == 0.0 && p.stick == 0.0 && p.droppedMouse == 0.40 && p.droppedStick == 0.25, "F176: held, both pitches run as 0 and the player's |value| of each is what was dropped");
        const f176::Pitches q = f176::apply(false, 0.40, -0.25);
        check(q.mouse == 0.40 && q.stick == -0.25 && q.droppedMouse == 0.0 && q.droppedStick == 0.0, "F176: not held, the values are the game's own and nothing is dropped");
        const f176::Pitches still = f176::apply(true, 0.0, 0.0);
        check(still.mouse == 0.0 && still.stick == 0.0 && still.droppedMouse == 0.0 && still.droppedStick == 0.0, "F176: held with the mouse and the stick still, nothing is dropped (the press ran and there was nothing to hold)");
        const f176::Pitches bad = f176::apply(true, std::nan(""), std::numeric_limits<double>::infinity());
        check(bad.mouse == 0.0 && bad.stick == 0.0 && bad.droppedMouse == 0.0 && bad.droppedStick == 0.0, "F176: a value that is not a number is dropped as 0");
        // through the real feed: the yaws are not touched, the head aim's own ask rides the mouse's value as before, look to aim's drop is the same drop
        f109::Feed f = f109::feed(false, f109::kSnap, 0.7, 0.3, 0.5, 0.4, 0.0);
        const f176::Pitches h = f176::apply(true, f.mousePitch, f.stickPitch);
        f.mousePitch = h.mouse;
        f.stickPitch = h.stick;
        const double ip = 0.05;   // the head aim''s ask for pitch
        check(f.stickYaw == 0.7 && f.mouseYaw == 0.5 && f.mousePitch + ip == 0.05 && f.stickPitch == 0.0,
              "F176: through f109::feed (look to aim off) the yaws of the mouse and the stick stay as they were, the pitches are 0, and the head aim's ask is the only pitch the call runs with");
        const f109::Feed drop = f109::feed(true, f109::kSmooth, 0.7, 0.3, 0.5, 0.4, 0.0);
        const f176::Pitches dropHeld = f176::apply(false, drop.mousePitch, drop.stickPitch);   // pitchHeld is false under look to aim (hookOn && !drop)
        check(drop.mousePitch == 0.0 && drop.stickPitch == 0.0 && drop.mouseYaw == 0.5 && dropHeld.droppedMouse == 0.0, "F176: look to aim's own drop is unchanged and F176 adds nothing to it");
    }

    // ---- 3. the press's account and its line --------------------------------------------------------------------------------------------------------------------------------------------------------
    {
        f176::Press ran;
        ran.begin(3, 1000, 100, 40, 1.5, 0.5);
        for (int i = 0; i < 90; ++i) ran.tick(1.0 / 90.0, true, false, true);
        char line[700];
        const int len = f176::formatPress(line, sizeof(line), ran, 3000, 190, 70, 4.25, 1.0, false, "released");
        std::snprintf(msg, sizeof(msg), "F176: a press that ran: the line names the press, the mode, the head aim, the seconds held, the updates, those with a pitch to drop, the sums (%d bytes)", len);
        check(len > 0 && len < 600 &&
                  std::strstr(line, "native on foot ADS pitch held (F176) press 3 (mouse look, head aim on): lasted 2.00 s; the mouse's and the stick's pitch held for 1.00 s (90 ticks) of it;") != nullptr &&
                  std::strstr(line, "the game's look updates it was held in 90, of them with a pitch to drop 30; mouse |pitch| dropped 2.750, stick |pitch| dropped 0.500 (game input units); guard tripped no; ended released") != nullptr,
              msg);
        // a press where the mouse was still: held, updates counted, nothing to drop
        f176::Press quiet;
        quiet.begin(4, 0, 100, 40, 1.5, 0.5);
        for (int i = 0; i < 90; ++i) quiet.tick(1.0 / 90.0, true, false, true);
        char stillLine[700];
        f176::formatPress(stillLine, sizeof(stillLine), quiet, 1000, 190, 40, 1.5, 0.5, false, "released");
        check(std::strstr(stillLine, "held in 90, of them with a pitch to drop 0; mouse |pitch| dropped 0.000, stick |pitch| dropped 0.000") != nullptr && std::strstr(stillLine, "held for 1.00 s (90 ticks)") != nullptr,
              "F176: a press where it ran and the mouse was still reads 90 updates held in, 0 with a pitch to drop, zero sums");
        // a press where it never ran: the head aim off
        f176::Press off;
        off.begin(5, 0, 100, 40, 1.5, 0.5);
        for (int i = 0; i < 90; ++i) off.tick(1.0 / 90.0, false, false, false);
        char offLine[700];
        f176::formatPress(offLine, sizeof(offLine), off, 1000, 100, 40, 1.5, 0.5, false, "released");
        check(std::strstr(offLine, "press 5 (mouse look, head aim off)") != nullptr && std::strstr(offLine, "held for 0.00 s (0 ticks)") != nullptr && std::strstr(offLine, "held in 0, of them with a pitch to drop 0") != nullptr,
              "F176: a press with the head aim off reads 'head aim off', 0 s held and 0 updates: not the same as a press where it ran and the mouse was still");
        // held for a second and the update never ran (the hook is not taking the look update)
        f176::Press dead;
        dead.begin(6, 0, 100, 40, 1.5, 0.5);
        for (int i = 0; i < 90; ++i) dead.tick(1.0 / 90.0, true, false, true);
        char deadLine[700];
        f176::formatPress(deadLine, sizeof(deadLine), dead, 1000, 100, 40, 1.5, 0.5, false, "released");
        check(std::strstr(deadLine, "held for 1.00 s (90 ticks) of it") != nullptr && std::strstr(deadLine, "held in 0, of them with a pitch to drop 0") != nullptr,
              "F176: held for a second and 0 updates reads as exactly that: the look update never ran with the hold (the health rule's NEVER APPLIED)");
        // look to aim: the press is look to aim's, whatever the head aim says
        f176::Press lta;
        lta.begin(7, 0, 0, 0, 0.0, 0.0);
        for (int i = 0; i < 60; ++i) lta.tick(1.0 / 90.0, false, true, true);
        char ltaLine[700];
        f176::formatPress(ltaLine, sizeof(ltaLine), lta, 700, 0, 0, 0.0, 0.0, false, "released");
        check(lta.lookToAimPress() && std::strstr(ltaLine, "press 7 (look to aim, head aim on)") != nullptr, "F176: a press inside look to aim is named look to aim, with the pitch held 0 s (look to aim drops it itself)");
        // a guard trip ends the press stopped, tripped yes
        f176::Press trip;
        trip.begin(8, 0, 0, 0, 0.0, 0.0);
        for (int i = 0; i < 10; ++i) trip.tick(1.0 / 90.0, true, false, true);
        char tripLine[700];
        f176::formatPress(tripLine, sizeof(tripLine), trip, 400, 5, 2, 0.1, 0.0, true, "stopped (the guard tripped)");
        check(std::strstr(tripLine, "guard tripped yes; ended stopped (the guard tripped)") != nullptr, "F176: a trip prints 'guard tripped yes' and how the press ended");
        f176::Press never;
        char noLine[700];
        never.tick(0.01, true, false, true);
        check(never.ticks == 0 && !never.open && f176::formatPress(noLine, sizeof(noLine), never, 0, 0, 0, 0.0, 0.0, false, nullptr) > 0 && std::strstr(noLine, "ended ?") != nullptr,
              "F176: a press never begun counts nothing");
        char widest[700];
        f176::Press big;
        big.begin(4000000000u, 1, 0, 0, 0.0, 0.0);
        for (int i = 0; i < 5; ++i) big.tick(1.0e6, true, false, true);
        const int wl = f176::formatPress(widest, sizeof(widest), big, 4000000000000ull, 4000000000u, 4000000000u, 1.0e12, 1.0e12, true, "stopped (a look value could not be restored after the game's update)");
        check(wl > 0 && wl < 600, "F176: the line is well inside the log's 1166 bytes at its widest");
    }

    // ---- 4. the source: where it is applied, and every path that clears the hook clears the flag -----------------------------------------------------------------------------------------------------
    {
        std::ifstream f("src/d3d11/native_on_foot.cpp", std::ios::binary);
        std::stringstream ss;
        ss << f.rdbuf();
        const std::string cpp = ss.str();
        const auto count = [&](const std::string& needle) {
            size_t n = 0, at = 0;
            while ((at = cpp.find(needle, at)) != std::string::npos) {
                ++n;
                at += needle.size();
            }
            return n;
        };
        check(!cpp.empty() && count("const bool pitchHeld = hookOn && !drop && g_adsPitchHeld.load(std::memory_order_relaxed);") == 1 &&
                  count("const nofoot::f176::Pitches held = nofoot::f176::apply(pitchHeld, f.mousePitch, f.stickPitch);") == 1 && count("f.mousePitch = held.mouse;") == 1 &&
                  count("f.stickPitch = held.stick;") == 1,
              "F176: lookUpdateRun holds the pitches after feed and before the wanted values are formed, only with the hook on and look to aim not dropping them (run from the repository root)");
        // every g_adsHookOn.store(false ...) is followed by the flag's clear
        const std::string clear = "g_adsHookOn.store(false, std::memory_order_relaxed);";
        const std::string pair = " g_adsPitchHeld.store(false, std::memory_order_relaxed);";
        size_t at = 0, clears = 0, paired = 0;
        while ((at = cpp.find(clear, at)) != std::string::npos) {
            ++clears;
            if (cpp.compare(at + clear.size(), pair.size(), pair) == 0) ++paired;
            at += clear.size();
        }
        std::snprintf(msg, sizeof(msg), "F176: every path that clears g_adsHookOn clears the pitch hold beside it (%u of %u)", static_cast<unsigned>(paired), static_cast<unsigned>(clears));
        check(clears >= 8 && paired == clears, msg);
        check(count("g_adsPitchHeld.store(held, std::memory_order_relaxed);") == 1 && count("nofoot::f176::holds(g_adsState == kAdsActive, g_adsAimMode == nofoot::f92::kAimOn, g_lta.engaged, g_lta.unwinding, true, false)") == 1 &&
                  count("f176PressEnd(now, \"released\", false);") == 1 && count("f176PressEnd(GetTickCount64(), how, holdOff);") == 1 && count("f176PressBegin(now);") == 2,
              "F176: the frame thread publishes the hold once a tick from the state it settled on; the press begins at both engages and ends at the release and at any stop (a hold-off is a trip)");
    }
}

}  // namespace f176test
