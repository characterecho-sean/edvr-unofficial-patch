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

}  // namespace edvr
