// F209 checks (native on-foot VR): the Pioneer Supplies buy screens found by the game's own open flag, and F107's rule that a buy screen open holds the camera away at any distance.
// Header only, run by native_frame_test (run209). The pure half only: the constants, the once-a-frame re-check's verdicts, the signal, F107's rule (native_on_foot_f107.h Away::step with
// the shop input), the lines and their cap, and the wiring read as text. The real glue (the five hooks over synthetic functions with the game's prologues, the callbacks' reads, the ring, the
// frame thread's tick and its lines) is run by native_on_foot_hooks_test.
//
// The figures are the flights' (edvr_gfx_20261101_114724 and _141044, v0.22.0-rc.5): the suit shop's showroom camera 3.6 to 3.7 m from the neck with a second camera 46 cm over it taking turns at
// 45 calls a second; the weapon showroom's camera the commander's own, 0.9 cm under the neck (no distance test could see it).
#pragma once

#include "../../src/d3d11/native_on_foot_f107.h"
#include "../../src/d3d11/shop_screen_core.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>

namespace f209test {

namespace shop = edvr::shop;
namespace f107 = edvr::nofoot::f107;

inline std::string slurp(const char* path) {
    std::ifstream f(path, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    std::string s = ss.str();
    s.erase(std::remove(s.begin(), s.end(), '\r'), s.end());   // a checkout with CRLF reads as LF
    return s;
}
inline size_t countOf(const std::string& text, const std::string& needle) {
    size_t n = 0;
    for (size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + needle.size())) ++n;
    return n;
}

// What a remembered object looks like to the frame thread's guarded reads: whether its vtable and its byte could be read, and their values.
struct FakeObject {
    bool vtableReadable = true, byteReadable = true;
    uint64_t vtable = 0;
    uint8_t byte = 0;
};
inline shop::Verdict recheck(const FakeObject* o, int cls, uintptr_t base) {
    if (!o) return shop::judgeObject(false, false, 0, 0, false, 0);
    return shop::judgeObject(true, o->vtableReadable, o->vtable, base + shop::vtableRva(cls), o->byteReadable, o->byte);
}

template <class Check>
inline void run209(Check&& check) {
    const uintptr_t base = 0x7FF600000000ull;
    // ---- 1. the constants: build 334925's addresses, offsets and prologues (the RE note's, read from the exe) ----------------------------------------------------------------------------
    {
        check(shop::kShowRva == 0x9C3960 && shop::kCloseRva == 0x9C22A0 && shop::kSetStateRva == 0xA237B0 && shop::kKioskStartRva == 0x1973820 && shop::kKioskStopRva == 0x1916440 &&
                  shop::kSuitVtableRva == 0x5255D20 && shop::kWeaponsVtableRva == 0x5255A20 && shop::kKioskVtableRva == 0x51E9770 && shop::kOffOpen == 0x79 && shop::kOffKiosk == 0x1F8,
              "F209: the addresses are build 334925's (Show 0x9C3960, Close 0x9C22A0, SetState 0xA237B0, kiosk start 0x1973820 and stop 0x1916440; vtables suit 0x5255D20, weapons 0x5255A20, kiosk 0x51E9770; bytes +0x79 and +0x1F8)");
        check(shop::kShowPrologueBytes == 14 && shop::kShowPrologue[0] == 0x40 && shop::kShowPrologue[1] == 0x53 && shop::kShowPrologue[13] == 0xE8 && shop::kClosePrologueBytes == 15 &&
                  shop::kClosePrologue[6] == 0x80 && shop::kClosePrologue[7] == 0x79 && shop::kClosePrologue[8] == 0x79 && shop::kSetStatePrologueBytes == 10 && shop::kSetStatePrologue[9] == 0xE8 &&
                  shop::kKioskStartPrologueBytes == 16 && shop::kKioskStartPrologue[0] == 0x48 && shop::kKioskStartPrologue[2] == 0xC4 && shop::kKioskStopPrologueBytes == 16 &&
                  shop::kKioskStopPrologue[0] == 0x48 && shop::kKioskStopPrologue[4] == 0x10,
              "F209: the prologues are the exe's (Show 14 bytes ending before its call, Close 15 with `cmp byte [rcx+79h],0`, SetState 10, the kiosk's 16 and 16)");
        check(std::strcmp(shop::kStateMachineName, "SuitUIStateMachine") == 0 && shop::kOffHandleName == 0x08 && shop::kOffStringChars == 0x14, "F209: the state machine handle's name is read at [handle+8]+0x14 and is SuitUIStateMachine");
    }
    // ---- 2. the Show/Close filter: the two buy screens by their vtables, every other class of the 40 that share the base is returned at once -----------------------------------------------------------
    {
        const uint64_t suit = base + shop::kSuitVtableRva, weapons = base + shop::kWeaponsVtableRva, kiosk = base + shop::kKioskVtableRva, other = base + 0x5255000;
        check(shop::buyScreenOfVtable(suit, base) == shop::kSuit && shop::buyScreenOfVtable(weapons, base) == shop::kWeapons && shop::buyScreenOfVtable(kiosk, base) == shop::kNotOurs &&
                  shop::buyScreenOfVtable(other, base) == shop::kNotOurs && shop::buyScreenOfVtable(0, base) == shop::kNotOurs && shop::buyScreenOfVtable(suit, base + 0x1000) == shop::kNotOurs,
              "F209: Show and Close are filtered on *(this): the suit and the weapon buy screens' vtables are ours; the kiosk, any other class, a null vtable and another image base are not");
        check(shop::vtableRva(shop::kSuit) == shop::kSuitVtableRva && shop::vtableRva(shop::kWeapons) == shop::kWeaponsVtableRva && shop::vtableRva(shop::kKiosk) == shop::kKioskVtableRva &&
                  std::strcmp(shop::className(shop::kSuit), "SuitVendorInteraction") == 0 && std::strcmp(shop::className(shop::kWeapons), "WeaponsVendorInteraction") == 0 &&
                  std::strcmp(shop::className(shop::kKiosk), "ArmouryVendorActivity") == 0,
              "F209: each class names its vtable and its own name");
    }
    // ---- 3. the signal from Show and Close, and the once-a-frame re-check ----------------------------------------------------------------------------------------------------------------------
    {
        FakeObject suit, weapons, kiosk;
        suit.vtable = base + shop::kSuitVtableRva;
        weapons.vtable = base + shop::kWeaponsVtableRva;
        kiosk.vtable = base + shop::kKioskVtableRva;
        auto sig = [&](const FakeObject* s, const FakeObject* w, const FakeObject* k) {
            return shop::signalOf(recheck(s, shop::kSuit, base), recheck(w, shop::kWeapons, base), recheck(k, shop::kKiosk, base));
        };
        check(!sig(nullptr, nullptr, nullptr).buyScreenOpen(), "F209: nothing remembered: the signal is off");
        suit.byte = 1;   // Show sets +0x79
        check(sig(&suit, nullptr, nullptr).buyScreenOpen() && sig(&suit, nullptr, nullptr).open[shop::kSuit] && !sig(&suit, nullptr, nullptr).open[shop::kWeapons], "F209: the suit buy screen's Show: the signal is on, for the suit");
        weapons.byte = 1;
        check(sig(&suit, &weapons, nullptr).open[shop::kSuit] && sig(&suit, &weapons, nullptr).open[shop::kWeapons], "F209: both buy screens open: both are in the signal");
        suit.byte = 0;   // Close clears it
        check(sig(&suit, &weapons, nullptr).buyScreenOpen() && !sig(&suit, &weapons, nullptr).open[shop::kSuit], "F209: the suit screen's Close: the weapon screen still holds the signal");
        weapons.byte = 0;
        check(!sig(&suit, &weapons, nullptr).buyScreenOpen(), "F209: both Closed: the signal is off");
        kiosk.byte = 1;
        const shop::Signal onlyKiosk = sig(&suit, &weapons, &kiosk);
        check(onlyKiosk.kiosk && !onlyKiosk.buyScreenOpen(), "F209: the kiosk session alone is not a buy screen: it is reported, and it does not hold F107 away");
        // a missed Close: the event never came, the game cleared the byte: the frame thread's read says closed
        suit.byte = 1;
        check(sig(&suit, nullptr, nullptr).buyScreenOpen(), "F209 (control): the byte reads open");
        suit.byte = 0;
        check(!sig(&suit, nullptr, nullptr).buyScreenOpen() && recheck(&suit, shop::kSuit, base) == shop::Verdict::kClosed && !shop::forgets(shop::Verdict::kClosed),
              "F209: a Close the hook missed cannot stick the signal: the byte read each frame says closed, and the object is kept (a screen opens again at the same address)");
        // a freed object whose address was reused: the vtable is no longer the class's
        suit.byte = 1;
        suit.vtable = base + 0x5255000;
        check(recheck(&suit, shop::kSuit, base) == shop::Verdict::kStaleVtable && shop::forgets(recheck(&suit, shop::kSuit, base)) && !sig(&suit, nullptr, nullptr).buyScreenOpen(),
              "F209: a stale `this` whose vtable changed (freed and reused) is forgotten and the signal goes off though its byte reads 1");
        // a vtable of the other buy screen at the same address is just as stale
        suit.vtable = base + shop::kWeaponsVtableRva;
        check(recheck(&suit, shop::kSuit, base) == shop::Verdict::kStaleVtable, "F209: the weapon screen's vtable at the suit's remembered address is stale for the suit");
        // memory that cannot be read
        FakeObject gone;
        gone.vtableReadable = false;
        FakeObject noByte;
        noByte.vtable = base + shop::kSuitVtableRva;
        noByte.byteReadable = false;
        check(recheck(&gone, shop::kSuit, base) == shop::Verdict::kUnreadable && recheck(&noByte, shop::kSuit, base) == shop::Verdict::kUnreadable && shop::forgets(shop::Verdict::kUnreadable) &&
                  !sig(&gone, nullptr, nullptr).buyScreenOpen() && !sig(&noByte, nullptr, nullptr).buyScreenOpen(),
              "F209: an object whose vtable or byte cannot be read is forgotten and never holds the signal");
        check(!shop::forgets(shop::Verdict::kNone) && !shop::forgets(shop::Verdict::kOpen) && !shop::forgets(shop::Verdict::kClosed), "F209: only a stale or unreadable object is forgotten");
        // the game's own tests are `!= 0`
        suit.vtable = base + shop::kSuitVtableRva;
        suit.byte = 2;
        check(recheck(&suit, shop::kSuit, base) == shop::Verdict::kOpen, "F209: the byte is read as the game reads it: any non-zero value is open");
    }
    // ---- 4. F107: a buy screen open holds the camera away at any distance --------------------------------------------------------------------------------------------------------------------
    {   // the weapon showroom: the commander's own camera, 0.9 cm under the neck, 90 calls (the flight's) while the screen is open; the screen closes, the camera stays near
        f107::Away a;
        int left = 0, back = 0;
        double t = 0.0, backAt = -1.0;
        for (int i = 0; i < 20; ++i, t += 0.0222) a.step(0.2, t, false);   // walking up to the shop's counter
        const double openAt = t;
        for (int i = 0; i < 90; ++i, t += 0.0222) {
            const f107::Away::Edge e = a.step(0.009, t, true);
            left += e == f107::Away::kLeft;
            back += e == f107::Away::kBack;
        }
        const bool awayThroughout = a.away && a.shopCalls == 90 && a.distanceCalls == 0 && a.beganByShop && !a.beganByDistance && a.calls == 90 && a.farCalls == 90;
        const double closeAt = t;
        for (int i = 0; i < 200; ++i, t += 0.0222) {   // the screen closed: the camera stays near for 4.4 s
            const f107::Away::Edge e = a.step(0.009, t, false);
            left += e == f107::Away::kLeft;
            if (e == f107::Away::kBack) {
                ++back;
                backAt = t;
            }
        }
        check(left == 1 && awayThroughout && back == 1 && a.episodes == 1 && !a.away && openAt < closeAt && backAt >= closeAt + f107::kReturnHoldS - 1e-9 && backAt < closeAt + f107::kReturnHoldS + 0.03,
              "F209: the weapon showroom (the commander's own camera, 0.9 cm under the neck): the buy screen open is one episode from the first call, every call refused, and it ends through F208's hold (2.0 s) after the screen closes");
    }
    {   // the suit shop: the showroom camera 3.7 m out and the near camera 46 cm over the neck alternating for 18 s with the screen open: one episode, no flap counted as a rejoin, the hold unchanged
        f107::Away a;
        int left = 0, back = 0;
        double t = 0.0;
        for (int i = 0; i < 800; ++i, t += 1.0 / 45.0) {
            const double d = (i / 3) % 2 == 0 ? 3.7 : 0.46;
            const f107::Away::Edge e = a.step(d, t, true);
            left += e == f107::Away::kLeft;
            back += e == f107::Away::kBack;
        }
        check(left == 1 && back == 0 && a.away && a.episodes == 1 && a.rejoins == 0 && std::fabs(a.holdS - f107::kReturnHoldS) < 1e-12 && a.shopCalls == 800 && a.distanceCalls > 0 && a.distanceCalls < 800 &&
                  a.beganByShop && a.beganByDistance,
              "F209: the suit shop's alternating cameras with the screen open are one episode by both reasons, with no rejoin and the base hold (the flap that doubled the hold in F208b needs no hold here)");
        const double closeAt = t;
        double backAt = -1.0;
        for (int i = 0; i < 200; ++i, t += 1.0 / 45.0) {
            const f107::Away::Edge e = a.step(0.46, t, false);
            if (e == f107::Away::kBack) backAt = t;
        }
        check(backAt >= closeAt + f107::kReturnHoldS - 1e-9 && backAt < closeAt + f107::kReturnHoldS + 0.03 && !a.away, "F209: closing the suit screen with the camera on the commander ends the episode after the 2.0 s hold");
    }
    {   // the screen closes but the camera stays at the showroom (3.7 m): the distance test keeps the episode, as before
        f107::Away a;
        double t = 0.0;
        for (int i = 0; i < 100; ++i, t += 0.02) a.step(3.7, t, true);
        for (int i = 0; i < 400; ++i, t += 0.02) a.step(3.7, t, false);
        check(a.away && a.episodes == 1 && a.shopCalls == 100 && a.distanceCalls == 500, "F209: the screen closed with the camera still 3.7 m out: the distance test keeps the episode open");
        double backAt = -1.0, nearAt = t;
        for (int i = 0; i < 200; ++i, t += 0.02)
            if (a.step(0.3, t, false) == f107::Away::kBack) backAt = t;
        check(backAt >= nearAt + f107::kReturnHoldS - 1e-9 && !a.away, "F209: ... and the camera's return ends it after the hold");
    }
    {   // normal play unchanged: the same distances with the shop input absent, false, or true-then-never give what F208/F208b gave; the distance test is the only reason
        const double seq[] = {0.2, 0.2, 3367.4, 3367.6, 139.2, 0.19, 0.19, 3.7, 0.46, 0.46, 3.6, 0.5, 0.2, 710.5, 0.3, 0.2, 0.2, 0.2, 0.2, 0.2, 0.2, 0.2};
        f107::Away dflt, explicitOff;
        bool same = true;
        double t = 0.0;
        for (int round = 0; round < 12; ++round)
            for (const double d : seq) {
                t += 0.4;
                const f107::Away::Edge e1 = dflt.step(d, t);
                const f107::Away::Edge e2 = explicitOff.step(d, t, false);
                same = same && e1 == e2 && dflt.away == explicitOff.away && dflt.calls == explicitOff.calls && dflt.farCalls == explicitOff.farCalls && dflt.episodes == explicitOff.episodes &&
                       dflt.alternations == explicitOff.alternations && dflt.rejoins == explicitOff.rejoins && dflt.holdS == explicitOff.holdS;
            }
        check(same && dflt.shopCalls == 0 && dflt.episodes > 1 && !dflt.beganByShop && dflt.beganByDistance, "F209: with no buy screen open F107 behaves exactly as before: the shop input defaults off, and the distance test is the only reason an episode begins");
        f107::Away fresh;
        for (int i = 0; i < 10; ++i) fresh.step(0.2, i * 0.02, false);
        const f107::Away::Edge e = fresh.step(0.5, 1.0, false);
        check(e == f107::Away::kNone && !fresh.away && fresh.episodes == 0, "F209: the near camera 46 cm over the neck with no buy screen open is the commander's own: no episode");
        f107::Away carrier;
        const f107::Away::Edge left = carrier.step(3367.4, 0.0, false);
        check(left == f107::Away::kLeft && carrier.beganByDistance && !carrier.beganByShop && carrier.distanceCalls == 1 && carrier.shopCalls == 0,
              "F209: the carrier management screen (3367 m) is still away by the distance test alone");
        f107::Away nan;
        const f107::Away::Edge leftNan = nan.step(std::numeric_limits<double>::quiet_NaN(), 0.0, false);
        check(leftNan == f107::Away::kLeft && nan.notNumber == 1 && nan.beganByDistance, "F209: a camera that is not a number is still away by the distance test");
    }
    // ---- 5. the lines ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
    {
        char line[512];
        shop::Event e;
        e.kind = shop::kShow;
        e.cls = shop::kSuit;
        e.before = 0;
        e.after = 1;
        e.tid = 7;
        e.thisLow = 0x1A2B3C4D;
        shop::formatEvent(line, sizeof(line), e);
        check(std::strcmp(line, "shop screens (F209): SuitVendorInteraction Show: +0x79 0 -> 1 (this 0x1A2B3C4D, thread 7)") == 0, "F209: a buy screen's Show line says the class, +0x79 before and after, the object and the thread");
        e.kind = shop::kClose;
        e.cls = shop::kWeapons;
        e.before = 1;
        e.after = 0;
        shop::formatEvent(line, sizeof(line), e);
        check(std::strcmp(line, "shop screens (F209): WeaponsVendorInteraction Close: +0x79 1 -> 0 (this 0x1A2B3C4D, thread 7)") == 0, "F209: a Close line likewise");
        e = shop::Event();
        e.kind = shop::kState;
        e.tid = 9;
        std::snprintf(e.from, sizeof(e.from), "%s", "OnFoot");
        std::snprintf(e.to, sizeof(e.to), "%s", "Suit Vendor");
        shop::formatEvent(line, sizeof(line), e);
        check(std::strcmp(line, "shop screens (F209): SuitUIStateMachine: \"OnFoot\" -> \"Suit Vendor\" (thread 9)") == 0, "F209: a SuitUIStateMachine state change says from, to and the thread");
        e = shop::Event();
        e.kind = shop::kKioskStart;
        e.cls = shop::kKiosk;
        e.before = 0;
        e.after = 1;
        e.tid = 4;
        e.thisLow = 0x3C4D5E6F;
        shop::formatEvent(line, sizeof(line), e);
        check(std::strcmp(line, "shop screens (F209): ArmouryVendorActivity started: +0x1F8 0 -> 1 (this 0x3C4D5E6F, thread 4)") == 0, "F209: the kiosk's start line says +0x1F8 before and after");
        e.kind = shop::kKioskStop;
        e.before = 1;
        e.after = 0;
        shop::formatEvent(line, sizeof(line), e);
        check(std::strcmp(line, "shop screens (F209): ArmouryVendorActivity stopped: +0x1F8 1 -> 0 (this 0x3C4D5E6F, thread 4)") == 0, "F209: ... and its stop line");
        e.kind = shop::kShow;
        e.cls = shop::kSuit;
        e.before = -1;
        e.after = -1;
        shop::formatEvent(line, sizeof(line), e);
        check(std::strstr(line, "+0x79 -1 -> -1") != nullptr, "F209: an unreadable byte reads -1 in the line");
        shop::Signal s;
        s.open[shop::kSuit] = true;
        shop::formatSignal(line, sizeof(line), s, true, 3);
        check(std::strcmp(line, "shop screens (F209): the shop signal is ON: F107 holds the camera away (suit open, weapons closed, kiosk not running; change 3)") == 0, "F209: the signal's ON line says which screen and the change count");
        s = shop::Signal();
        s.kiosk = true;
        shop::formatSignal(line, sizeof(line), s, false, 4);
        check(std::strcmp(line, "shop screens (F209): the shop signal is OFF (suit closed, weapons closed, kiosk running; change 4)") == 0, "F209: the OFF line likewise");
        shop::formatForgotten(line, sizeof(line), shop::kSuit, shop::Verdict::kStaleVtable, 0x7FF600001234ull, 0x7FF605255D20ull, 0x1A2B3C4D);
        check(std::strstr(line, "the remembered SuitVendorInteraction (this 0x1A2B3C4D) is forgotten: its vtable is no longer the class's (vtable 0x7FF600001234, the class's 0x7FF605255D20)") != nullptr,
              "F209: the forgotten line says the class, the object and both vtables");
        shop::formatSecond(line, sizeof(line), 1, -1, 0, "Suit Vendor", true, "away (episode 1, 90 calls refused, 90 with a buy screen open)");
        check(std::strcmp(line, "shop screens (F209): 1 s: suit +0x79 1, weapons +0x79 none, kiosk +0x1F8 0, SuitUIStateMachine \"Suit Vendor\", signal on; F107: away (episode 1, 90 calls refused, 90 with a buy screen open)") == 0,
              "F209: the once-a-second line says the remembered bytes, the state, the kiosk byte, the signal and F107's state");
        shop::formatSecond(line, sizeof(line), -1, -1, -1, "", false, "not away");
        check(std::strstr(line, "SuitUIStateMachine \"unknown\", signal off; F107: not away") != nullptr && std::strstr(line, "suit +0x79 none") != nullptr, "F209: no state yet reads unknown, no object reads none");
    }
    // ---- 6. the cap and the pacer; what is "set" -------------------------------------------------------------------------------------------------------------------------------------------------
    {
        shop::LineCap cap;
        uint32_t summary = 99;
        int written = 0;
        for (int i = 0; i < 100; ++i)
            if (cap.allow(1000 + static_cast<uint64_t>(i), &summary)) ++written;
        check(written == static_cast<int>(shop::kLinesPer10s) && cap.dropped == 100 - shop::kLinesPer10s, "F209: a flapping screen writes at most 40 event lines in ten seconds; the rest are counted");
        const bool next = cap.allow(1000 + 10000, &summary);
        check(next && summary == 100 - shop::kLinesPer10s, "F209: the next window says how many the last one dropped, once");
        const bool again = cap.allow(1000 + 10001, &summary);
        check(again && summary == 0, "F209: ... and not again");
        shop::LineCap quiet;
        bool all = true;
        for (int w = 0; w < 5; ++w)
            for (int i = 0; i < 10; ++i) all = all && quiet.allow(5000 + static_cast<uint64_t>(w) * 10000 + static_cast<uint64_t>(i) * 100, &summary) && summary == 0;
        check(all, "F209: ordinary shop traffic (a handful of lines a window) is never capped");
        shop::Pacer p;
        check(p.due(5000) && !p.due(5500) && !p.due(5999) && p.due(6000) && !p.due(6001) && p.due(7001) && p.due(0) && !p.due(500), "F209: the once-a-second line is due on the first look, then every 1000 ms (a clock that went back is due once)");
        check(shop::stateIsVendor("Suit Vendor") && shop::stateIsVendor("Weapons Upgrade Vendor") && shop::stateIsVendor("Armoury Vendor") && !shop::stateIsVendor("OnFoot") && !shop::stateIsVendor("") &&
                  !shop::stateIsVendor(nullptr) && !shop::stateIsVendor("Cockpit"),
              "F209: a vendor state is one whose name has Vendor in it; OnFoot, an empty state and a state the build does not name are not");
        check(shop::anySet(true, false, "OnFoot") && shop::anySet(false, true, "") && shop::anySet(false, false, "Suit Vendor") && !shop::anySet(false, false, "OnFoot") && !shop::anySet(false, false, ""),
              "F209: the once-a-second line runs while a buy screen is open, the kiosk runs, or the machine is in a vendor state, and is silent otherwise");
        check(shop::isSuitUiStateMachine("SuitUIStateMachine") && !shop::isSuitUiStateMachine("SuitUIStateMachineX") && !shop::isSuitUiStateMachine("SuitUIState") && !shop::isSuitUiStateMachine("") &&
                  !shop::isSuitUiStateMachine(nullptr) && !shop::isSuitUiStateMachine("OnFoot"),
              "F209: only the handle named exactly SuitUIStateMachine is recorded");
    }
    // ---- 7. the wiring, read as text -----------------------------------------------------------------------------------------------------------------------------------------------------------
    {
        const std::string hooks = slurp("src/d3d11/native_on_foot_hooks.cpp"), cpp = slurp("src/d3d11/native_on_foot.cpp");
        check(countOf(hooks, "shop::kShowRva") >= 1 && countOf(hooks, "shop::kCloseRva") >= 1 && countOf(hooks, "shop::kSetStateRva") >= 1 && countOf(hooks, "shop::kKioskStartRva") >= 1 &&
                  countOf(hooks, "shop::kKioskStopRva") >= 1 && countOf(hooks, "case kHkShow: return reinterpret_cast<const void*>(&shopShowHooked)") == 1 &&
                  countOf(hooks, "case kHkKioskStop: return reinterpret_cast<const void*>(&shopKioskStopHooked)") == 1,
              "F209: the five hooks are in the hook table with their addresses and callbacks");
        check(hooks.find("tryShopHooks(sink);") != std::string::npos && hooks.find("shopFrameTick(sink);") != std::string::npos && hooks.find("shopFrameTick(sink);") < hooks.find("    updateGates();\n}\navh::Sink g_frameSink"),
              "F209: the frame boundary tries the shop hooks (while the native key is on) and re-reads the remembered objects every frame, before the gates are set");
        check(cpp.find("g_f107Away.step(eyeNeckM, static_cast<double>(f30QpcUs(in.qpc)) * 1e-6, nativeOnFootShopOpen())") != std::string::npos,
              "F209: the Animate post-hook gives F107 the shop signal with every call");
        // every shop callback forwards to the original: the original's result is returned unchanged (nothing is written to the game)
        const size_t at = hooks.find("// ---- F209: the Pioneer Supplies buy screens");
        const size_t end = hooks.find("// ---- hiding the head parts", at);
        const std::string block = at != std::string::npos && end != std::string::npos ? hooks.substr(at, end - at) : std::string();
        check(!block.empty() && countOf(block, "forward(a, b, c, d)") >= 5 && block.find("VirtualProtect") == std::string::npos && block.find("WriteProcessMemory") == std::string::npos &&
                  block.find("Log::get()") == std::string::npos && block.find("= new ") == std::string::npos && block.find("std::vector") == std::string::npos && block.find("std::string") == std::string::npos &&
                  block.find("malloc(") == std::string::npos && block.find("lock_guard") == std::string::npos && block.find("std::mutex") == std::string::npos,
              "F209: the hook threads' code forwards every call and writes nothing to the game, takes no lock, allocates nothing and logs nothing");
    }
}

}  // namespace f209test
