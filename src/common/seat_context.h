#pragma once
// The seat memory's cross-DLL word (docs/seat-memory-2026-10-14.md). d3d11.dll knows what the game says about the commander's seat (Status.json
// Flags and Flags2, the journal's boundary events, Explorer Cam) and the one setting; openvr_api.dll owns the seated origin and has to act on
// them. One 32-bit word crosses, so the runtime can never read half a change: a count that moved together with the flag it belongs to.
//
//   bit  0       published    d3d11.dll has written the word (0 is a fresh mapping or a graphics half that does not know the seat memory)
//   bit  1       keep         the seat memory is active. Always set by this build (advanced.cockpit_centre = keep | game was retired 2026-10-14 with
//                             keep shipped); the bit stays so the layout and the pure logic's inactive path (published = 0) are unchanged
//   bit  2       gameplay     LoadGame seen in this process
//   bit  3, 4    seatedKnown, seated    Status.json says the commander is in a seat (ship, fighter, SRV, taxi, multicrew)
//   bit  5, 6    onFootKnown, onFoot    Status.json Flags2 bit 0
//   bit  7       cameraOpen   Explorer Cam has a session open
//   bits 8-12    seatKind     Flags bits 24-26 and Flags2 bits 1-2 (journal_watch.h journalSeatKind)
//   bits 13-31   transitions  embarks + disembarks + LoadGames + Undocks, modulo 2^19; a change is a journal boundary
//
// Pure: no windows.h, so the rig and both DLLs include it.
#include <cstdint>

namespace edvr {

struct SeatContext {
    bool published = false;
    bool keep = false;
    bool gameplay = false;
    bool seatedKnown = false, seated = false;
    bool onFootKnown = false, onFoot = false;
    bool cameraOpen = false;
    uint32_t seatKind = 0;       // 5 bits
    uint32_t transitions = 0;    // 19 bits
};

constexpr uint32_t kSeatTransitionMask = 0x7FFFFu;

constexpr uint32_t encodeSeatContext(const SeatContext& c) {
    return 1u | (c.keep ? 2u : 0u) | (c.gameplay ? 4u : 0u) | (c.seatedKnown ? 8u : 0u) | (c.seated ? 16u : 0u) |
           (c.onFootKnown ? 32u : 0u) | (c.onFoot ? 64u : 0u) | (c.cameraOpen ? 128u : 0u) | ((c.seatKind & 0x1Fu) << 8) |
           ((c.transitions & kSeatTransitionMask) << 13);
}

constexpr SeatContext decodeSeatContext(uint32_t w) {
    SeatContext c{};
    c.published = (w & 1u) != 0;
    if (!c.published) return c;     // nobody wrote: every other bit is meaningless
    c.keep = (w & 2u) != 0;
    c.gameplay = (w & 4u) != 0;
    c.seatedKnown = (w & 8u) != 0;
    c.seated = c.seatedKnown && (w & 16u) != 0;
    c.onFootKnown = (w & 32u) != 0;
    c.onFoot = c.onFootKnown && (w & 64u) != 0;
    c.cameraOpen = (w & 128u) != 0;
    c.seatKind = (w >> 8) & 0x1Fu;
    c.transitions = (w >> 13) & kSeatTransitionMask;
    return c;
}

// ---- The HMD recentre key, as the graphics half sees it (docs/seat-memory-2026-10-14.md, "the press"). -------------------------------------------
// The game's automatic recentres and the player's key press arrive through the SAME caller, so only the press itself can tell them apart.
// d3d11.dll reads the binding of Elite's HMDReset action from the active .binds file, watches those inputs (the keyboard with its modifiers, the
// XInput pad, a joystick button the game is seen reading) without consuming anything, and publishes two words: this state, and the tick of the
// last press edge (GetTickCount64, one clock for both halves).
//
//   bits 0-3     status       HmdPressStatus
//   bit  4       heldNow      a watched slot is held at the last poll
//   bits 8-31    seq          press edges seen (24 bits, wraps)
enum class HmdPressStatus : uint32_t {
    Unpublished = 0,        // the graphics half has not written the word (or an older one): nothing is known
    Observable = 1,         // a bound slot is watched and has been proven to see its own presses (the keyboard always; a pad or joystick after its first press)
    NoBind = 2,             // HMDReset has no usable slot in the active .binds (unbound, or reading the game's bindings is off)
    UnobservableDevice = 3, // every bound slot is on a device nothing here can watch (the mouse, an axis, a key with no name)
    Unreadable = 4,         // no preset or no readable .binds file
    Unproven = 5            // a pad or joystick slot is watched but has not yet seen one press: its absence proves nothing yet
};
struct HmdResetPress {
    HmdPressStatus status = HmdPressStatus::Unpublished;
    bool heldNow = false;
    uint32_t seq = 0;
};
constexpr uint32_t kHmdPressSeqMask = 0xFFFFFFu;
constexpr uint32_t encodeHmdPress(const HmdResetPress& p) {
    return (static_cast<uint32_t>(p.status) & 0xFu) | (p.heldNow ? 16u : 0u) | ((p.seq & kHmdPressSeqMask) << 8);
}
constexpr HmdResetPress decodeHmdPress(uint32_t w) {
    HmdResetPress p{};
    const uint32_t s = w & 0xFu;
    p.status = s <= 5u ? static_cast<HmdPressStatus>(s) : HmdPressStatus::Unpublished;
    p.heldNow = (w & 16u) != 0;
    p.seq = (w >> 8) & kHmdPressSeqMask;
    return p;
}
constexpr const char* hmdPressStatusName(HmdPressStatus s) {
    return s == HmdPressStatus::Observable ? "observable" : s == HmdPressStatus::NoBind ? "no_bind"
         : s == HmdPressStatus::UnobservableDevice ? "unobservable_device" : s == HmdPressStatus::Unreadable ? "unreadable"
         : s == HmdPressStatus::Unproven ? "unproven_device" : "unpublished";
}

}  // namespace edvr
