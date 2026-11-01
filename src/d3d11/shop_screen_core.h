// F209 (native on-foot VR): the Pioneer Supplies buy screens, found by the game's own open flag. The pure half, header only, no Windows and no game: native_frame_test runs these
// functions, native_on_foot_hooks_test runs them against the REAL glue (native_on_foot_hooks.cpp, the five hooks below), and the Animate post-hook's F107 rule takes the signal from
// native_on_foot_hooks.h's nativeOnFootShopOpen().
//
// WHY. At the suit shop the game draws the commander in a showroom from a camera of its own, and at the weapon shop a preview in front of the camera; EDVR took each camera for the
// commander's eye (F208, F208b: the pin, the raise, the hold, the settle) unless its distance from the neck said otherwise. A reverse-engineering pass on build 334925
// (analysis\decomp\verify_20261101_shop_flag.txt) found the game's own flag: SuitVendorInteraction (vtable 0x5255D20) and WeaponsVendorInteraction (0x5255A20), the two buy screens, derive
// from the UI interaction base whose Show (0x9C3960, vtable +0x70) sets the byte at +0x79 to 1 and whose Close (0x9C22A0, +0x80) calls OnClose and clears it; 0x9C3290 (+0x78) is the game's own
// IsOpen for that byte. CONFIRMED by the code: the byte; CANDIDATE until flown: that these screens are the showrooms the pilot saw.
//
// THE HOOKS (build 334925 only; the shared PE identity and the prologue bytes below gate each one; any mismatch stands that hook down with one line and patches nothing):
//   Show     0x9C3960  shared by 40+ classes: the callback tests *(this) against the two vtables and returns on everything else; for ours it reads +0x79 before and after the original.
//   Close    0x9C22A0  likewise.
//   SetState 0xA237B0  (rcx = the state machine's handle, rdx = the new state's C string): records the new state when the handle's name is "SuitUIStateMachine".
//   Kiosk    0x1973820 / 0x1916440  ArmouryVendorActivity OnStart / OnStop (vtable 0x51E9770): the kiosk session, byte +0x1F8.
// The Show/Close pair is hooked once on the shared functions (the per-class OnOpen/OnClose would be four hooks for the same two events and 0x1F344E0's prologue differs between builds).
//
// THE SIGNAL. A buy screen is open from its Show to its Close. The hooks remember `this` for each class; once a frame, on the frame thread, the remembered object's vtable is read (guarded) and
// compared with the class's, and the byte +0x79 is read (guarded): so a missed Close, a freed object or a reused address cannot leave the signal stuck. The answer is handed to the Animate
// post-hook (where F107 decides) through an atomic.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace edvr {
namespace shop {

// ---- build 334925's addresses and prologues ---------------------------------------------------------------------------------------------------------------------------------
constexpr uintptr_t kShowRva = 0x9C3960, kCloseRva = 0x9C22A0, kSetStateRva = 0xA237B0, kKioskStartRva = 0x1973820, kKioskStopRva = 0x1916440;
constexpr uintptr_t kSuitVtableRva = 0x5255D20, kWeaponsVtableRva = 0x5255A20, kKioskVtableRva = 0x51E9770;
constexpr uint32_t kOffOpen = 0x79;        // the interaction's "my screen is open" byte (the base class's Show sets it, Close clears it)
constexpr uint32_t kOffKiosk = 0x1F8;      // ArmouryVendorActivity's session byte (OnStart sets it, OnStop clears it)
constexpr uint32_t kOffHandleName = 0x08;  // the state machine handle's pointer to a refcounted Cobra string
constexpr uint32_t kOffStringChars = 0x14; // ... whose characters start here
constexpr char kStateMachineName[] = "SuitUIStateMachine";

// Each prologue ends before its first rip-relative or relative operand (the call or the displacement is not compared). CodeHook steals the whole instructions that cover five bytes: Show,
// Close and SetState `40 53 | 48 83 EC 20` (six), the kiosk's start `48 8B C4 | 55 | 53` (five) and stop `48 89 5C 24 10` (five).
constexpr size_t kShowPrologueBytes = 14;
inline constexpr uint8_t kShowPrologue[kShowPrologueBytes] = {0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xD9, 0x48, 0x8B, 0x49, 0x70, 0xE8};
constexpr size_t kClosePrologueBytes = 15;
inline constexpr uint8_t kClosePrologue[kClosePrologueBytes] = {0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x80, 0x79, 0x79, 0x00, 0x48, 0x8B, 0xD9, 0x74, 0x1B};
constexpr size_t kSetStatePrologueBytes = 10;
inline constexpr uint8_t kSetStatePrologue[kSetStatePrologueBytes] = {0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xDA, 0xE8};
constexpr size_t kKioskStartPrologueBytes = 16;
inline constexpr uint8_t kKioskStartPrologue[kKioskStartPrologueBytes] = {0x48, 0x8B, 0xC4, 0x55, 0x53, 0x48, 0x8D, 0x68, 0xA1, 0x48, 0x81, 0xEC, 0xA8, 0x00, 0x00, 0x00};
constexpr size_t kKioskStopPrologueBytes = 16;
inline constexpr uint8_t kKioskStopPrologue[kKioskStopPrologueBytes] = {0x48, 0x89, 0x5C, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xD9, 0x48, 0x81, 0xC1};

// ---- the classes -----------------------------------------------------------------------------------------------------------------------------------------------------------
enum Class : int { kSuit = 0, kWeapons = 1, kKiosk = 2, kClassCount = 3, kNotOurs = -1 };
inline const char* className(int c) noexcept {
    return c == kSuit ? "SuitVendorInteraction" : c == kWeapons ? "WeaponsVendorInteraction" : c == kKiosk ? "ArmouryVendorActivity" : "another class";
}
inline uintptr_t vtableRva(int c) noexcept { return c == kSuit ? kSuitVtableRva : c == kWeapons ? kWeaponsVtableRva : c == kKiosk ? kKioskVtableRva : 0; }
// Which of the two buy screens a vtable pointer belongs to (the Show/Close hook's whole filter); kNotOurs for any other class of the 40 that share the base.
inline int buyScreenOfVtable(uint64_t vtable, uintptr_t base) noexcept {
    return vtable == base + kSuitVtableRva ? kSuit : vtable == base + kWeaponsVtableRva ? kWeapons : kNotOurs;
}

// ---- the once-a-frame re-check ---------------------------------------------------------------------------------------------------------------------------------------------
// What one remembered object reads as, from guarded reads the caller made: whether there is an object, whether its vtable and byte could be read, the vtable, the byte.
enum class Verdict : int { kNone = 0, kOpen = 1, kClosed = 2, kStaleVtable = 3, kUnreadable = 4 };
inline Verdict judgeObject(bool haveObject, bool vtableRead, uint64_t vtable, uint64_t expectedVtable, bool byteRead, uint8_t byte) noexcept {
    if (!haveObject) return Verdict::kNone;
    if (!vtableRead) return Verdict::kUnreadable;
    if (vtable != expectedVtable) return Verdict::kStaleVtable;   // freed and reused (or a different class at the address): the object is forgotten
    if (!byteRead) return Verdict::kUnreadable;
    return byte != 0 ? Verdict::kOpen : Verdict::kClosed;        // the game's own tests are `!= 0`
}
// A verdict that makes the frame thread forget the object (so it can never be read again until a Show remembers one).
inline bool forgets(Verdict v) noexcept { return v == Verdict::kStaleVtable || v == Verdict::kUnreadable; }
inline const char* verdictText(Verdict v) noexcept {
    switch (v) {
        case Verdict::kNone: return "none remembered";
        case Verdict::kOpen: return "open";
        case Verdict::kClosed: return "closed";
        case Verdict::kStaleVtable: return "forgotten: its vtable is no longer the class's";
        default: return "forgotten: its memory could not be read";
    }
}

// The signal the rest of EDVR reads: a buy screen (either) is open.
struct Signal {
    bool open[2] = {false, false};   // the suit and the weapon buy screen
    bool kiosk = false;              // ArmouryVendorActivity's session (not a buy screen: the home menu; it does not hold F107 away)
    bool buyScreenOpen() const noexcept { return open[0] || open[1]; }
};
inline Signal signalOf(Verdict suit, Verdict weapons, Verdict kiosk) noexcept {
    Signal s;
    s.open[kSuit] = suit == Verdict::kOpen;
    s.open[kWeapons] = weapons == Verdict::kOpen;
    s.kiosk = kiosk == Verdict::kOpen;
    return s;
}

// ---- the state machine ------------------------------------------------------------------------------------------------------------------------------------------------------
// Is this (bounded) copy of the handle's name string the one machine we record? The hook reads at most kNameCopy bytes and always terminates them.
constexpr size_t kNameCopy = 24, kStateCopy = 40;
inline bool isSuitUiStateMachine(const char* name) noexcept { return name && std::strncmp(name, kStateMachineName, sizeof(kStateMachineName)) == 0; }
// A vendor state ("Armoury Vendor", "Suit Vendor", "Weapons Vendor", "Suit Upgrade Vendor", "Weapons Upgrade Vendor"): the once-a-second line runs while the machine is in one, so a machine left in some
// other state the build does not name (a ship, a menu) never keeps the log writing a line a second for the rest of the session.
inline bool stateIsVendor(const char* state) noexcept { return state && std::strstr(state, "Vendor") != nullptr; }
// Is anything the once-a-second line reports set: a buy screen open, the kiosk session running, or the machine in a vendor state.
inline bool anySet(bool buyScreenOpen, bool kiosk, const char* state) noexcept { return buyScreenOpen || kiosk || stateIsVendor(state); }
// At most one line a second.
struct Pacer {
    bool have = false;
    uint64_t lastMs = 0;
    bool due(uint64_t nowMs) noexcept {
        if (have && nowMs - lastMs < 1000) return false;
        have = true;
        lastMs = nowMs;
        return true;
    }
};

// ---- the lines ---------------------------------------------------------------------------------------------------------------------------------------------------------------
// Every line starts with this, so a flight log is read with one grep (and --health finds the lines by it).
inline const char* tag() noexcept { return "shop screens (F209):"; }
enum Kind : uint8_t { kShow = 1, kClose = 2, kState = 3, kKioskStart = 4, kKioskStop = 5 };
struct Event {
    uint8_t kind = 0;
    int8_t cls = kNotOurs;
    int8_t before = -1, after = -1;     // +0x79 (or +0x1F8) before and after the original; -1 unreadable
    uint32_t tid = 0;
    uint32_t thisLow = 0;
    char from[kStateCopy] = {};
    char to[kStateCopy] = {};
};
inline int formatEvent(char* buf, size_t cap, const Event& e) noexcept {
    switch (e.kind) {
        case kShow:
        case kClose:
            return std::snprintf(buf, cap, "%s %s %s: +0x79 %d -> %d (this 0x%08X, thread %u)", tag(), className(e.cls), e.kind == kShow ? "Show" : "Close", e.before, e.after, e.thisLow, e.tid);
        case kState:
            return std::snprintf(buf, cap, "%s SuitUIStateMachine: \"%s\" -> \"%s\" (thread %u)", tag(), e.from, e.to, e.tid);
        case kKioskStart:
        case kKioskStop:
            return std::snprintf(buf, cap, "%s %s %s: +0x1F8 %d -> %d (this 0x%08X, thread %u)", tag(), className(kKiosk), e.kind == kKioskStart ? "started" : "stopped", e.before, e.after, e.thisLow, e.tid);
        default:
            return std::snprintf(buf, cap, "%s an event of kind %u", tag(), static_cast<unsigned>(e.kind));
    }
}
inline int formatSignal(char* buf, size_t cap, const Signal& s, bool on, uint32_t seq) noexcept {
    return std::snprintf(buf, cap, "%s the shop signal is %s (suit %s, weapons %s, kiosk %s; change %u)", tag(), on ? "ON: F107 holds the camera away" : "OFF", s.open[kSuit] ? "open" : "closed",
                         s.open[kWeapons] ? "open" : "closed", s.kiosk ? "running" : "not running", seq);
}
inline int formatForgotten(char* buf, size_t cap, int cls, Verdict v, uint64_t vtable, uint64_t expected, uint32_t thisLow) noexcept {
    return std::snprintf(buf, cap, "%s the remembered %s (this 0x%08X) is %s (vtable 0x%llX, the class's 0x%llX): the signal does not hold on it", tag(), className(cls), thisLow, verdictText(v),
                         static_cast<unsigned long long>(vtable), static_cast<unsigned long long>(expected));
}
// The once-a-second line while anything is set: the remembered objects' bytes ("none" when none is remembered), the state, the kiosk byte, the signal, and F107's state (text from its owner).
inline const char* byteText(int b) noexcept { return b < 0 ? "none" : b == 0 ? "0" : "1"; }
inline int formatSecond(char* buf, size_t cap, int suitByte, int weaponsByte, int kioskByte, const char* state, bool signal, const char* f107) noexcept {
    return std::snprintf(buf, cap, "%s 1 s: suit +0x79 %s, weapons +0x79 %s, kiosk +0x1F8 %s, SuitUIStateMachine \"%s\", signal %s; F107: %s", tag(), byteText(suitByte), byteText(weaponsByte),
                         byteText(kioskByte), state && state[0] ? state : "unknown", signal ? "on" : "off", f107);
}

// ---- the log's cap ----------------------------------------------------------------------------------------------------------------------------------------------------------
// Events are edges, but a flapping screen must not fill the log: at most kLinesPer10s event lines in any ten seconds; the rest are counted and said once.
constexpr uint32_t kLinesPer10s = 40;
struct LineCap {
    uint64_t windowMs = 0;
    uint32_t written = 0, dropped = 0;
    // True when the line may be written now. A window that ended with drops says so in `*summary` (the number dropped), once.
    bool allow(uint64_t nowMs, uint32_t* summary) noexcept {
        *summary = 0;
        if (nowMs - windowMs >= 10000 || windowMs == 0) {
            *summary = dropped;
            windowMs = nowMs;
            written = 0;
            dropped = 0;
        }
        if (written >= kLinesPer10s) {
            ++dropped;
            return false;
        }
        ++written;
        return true;
    }
};

}  // namespace shop
}  // namespace edvr
