#pragma once
// Pure logic for advanced.eye_origin_readers (docs/design-transition-flash-
// engine-fix-2026-09-23.md, parts A2/B: who reads the head pose the runtime
// hands back, and whether the game's camera positioner swapped at a
// transition). No Windows header, no game memory, no CodeHook, no hardware
// breakpoint -- takes plain data in and hands plain data back, so
// tools\transition_flash_prevent_test can drive it without the game.
// pose_reader_watch.cpp is the only includer that also arms DR0, unwinds a
// real stack, walks Toolhelp32 or touches a file.
#include <cstdint>

namespace edvr {
namespace prw {

// ---------------------------------------------------------------------------
// Dr7 slot-0 composition. Dr7's layout: bit 0 = L0 (local enable, slot 0),
// bits 16-17 = R/W0, bits 18-19 = LEN0. Every other bit -- the other three
// slots' enables and conditions, and the reserved bits -- belongs to
// whichever debugger or future slot set it and must come back exactly as
// given; this is the whole reason the composition is pulled out on its own
// rather than just assigning a literal into Dr7.
constexpr uint32_t kDr7L0Bit = 1u << 0;
constexpr uint32_t kDr7Slot0Mask = 0x000F0001u;   // bit0 | bits16-17 | bits18-19
// RW0 encodings x86 actually defines: 00 = execute, 01 = write, 11 =
// read-or-write (there is no read-ONLY encoding). LEN0=11b is 4 bytes,
// the only length either caller of armSlot0Dr7 below asks for.
constexpr uint32_t kDr7RwWrite = 0x1u;
constexpr uint32_t kDr7RwReadWrite = 0x3u;
constexpr uint32_t kDr7Len4Bytes = 0x3u;
constexpr uint32_t kDr7Slot0ArmedBits = 0x000F0001u;  // L0=1, RW0=11b, LEN0=11b -- kDr7RwReadWrite's shape

// Read-modify-write: clears exactly slot 0's bits (L0, RW0, LEN0) out of
// whatever Dr7 already held, then sets them to "armed" with the given RW
// condition (design doc round 6, part B: the writer watch wants RW0=01b,
// write-only, where the render-pose read watch above wants read-or-write) at
// a fixed 4-byte length. Bits belonging to slots 1-3, and the reserved bits,
// pass through unchanged in either direction. Defaulted to read-or-write/4
// bytes so every existing call site -- the render-pose watch's own --
// compiles and behaves exactly as before without passing anything new.
inline uint32_t armSlot0Dr7(uint32_t existing, uint32_t rw = kDr7RwReadWrite,
                             uint32_t len = kDr7Len4Bytes) noexcept {
    const uint32_t bits = kDr7L0Bit | ((rw & 0x3u) << 16) | ((len & 0x3u) << 18);
    return (existing & ~kDr7Slot0Mask) | bits;
}

// Clears only L0 (the local-enable bit). RW0/LEN0 left behind are inert --
// the CPU ignores a disabled slot's condition/length fields -- and leaving
// them is one fewer thing this function can get wrong.
inline uint32_t disarmSlot0Dr7(uint32_t existing) noexcept {
    return existing & ~kDr7L0Bit;
}

// Dr6 bit 0 (B0): slot 0's condition was detected. Sticky until cleared by
// the handler.
constexpr uint32_t kDr6B0Bit = 1u << 0;
inline bool dr6HasSlot0Hit(uint32_t dr6) noexcept { return (dr6 & kDr6B0Bit) != 0; }

// ---------------------------------------------------------------------------
// Stack-range classification: does `pointer` lie inside [stackLimitLow,
// stackLimitHigh), the range GetCurrentThreadStackLimits reports? A
// stack-resident buffer cannot be hardware-watched across frames -- it is
// gone the moment the function that owns it returns -- so this decides
// whether a candidate render-pose address is even eligible.
inline bool pointerOnStack(uint64_t pointer, uint64_t stackLimitLow, uint64_t stackLimitHigh) noexcept {
    if (stackLimitLow >= stackLimitHigh) return false;  // degenerate range: never "on" it
    return pointer >= stackLimitLow && pointer < stackLimitHigh;
}

// ---------------------------------------------------------------------------
// The 60-consecutive-frame stability gate on the published render-pose
// pointer. observeAddress folds one frame's reading in; isStable answers
// whether it is time to arm. A pointer of 0 (nobody has published yet)
// never counts towards stability and resets the run, the same as any other
// address change.
constexpr uint32_t kStableFrames = 60;

struct StabilityState {
    uint64_t address = 0;
    uint32_t consecutive = 0;
};

inline StabilityState observeAddress(const StabilityState& prev, uint64_t address) noexcept {
    if (address != 0 && address == prev.address) {
        uint32_t next = prev.consecutive;
        if (next < 0xFFFFFFFFu) ++next;
        return StabilityState{address, next};
    }
    return StabilityState{address, address != 0 ? 1u : 0u};
}

inline bool isStable(const StabilityState& s) noexcept {
    return s.address != 0 && s.consecutive >= kStableFrames;
}

// ---------------------------------------------------------------------------
// The unique-reader table's dedupe key and find logic. The VEH handler
// itself never calls this: it works on a preallocated array the caller
// supplies, under whatever spin guard the caller already holds, so the
// arithmetic is testable without any of that machinery.
constexpr uint32_t kMaxReaders = 32;
constexpr uint32_t kMaxUnwindFrames = 16;

struct ReaderKey {
    uint64_t rip = 0;      // game-module RVA of the instruction after the access
    uint64_t caller1 = 0;  // game-module RVA, 0 if the unwind found none
    uint64_t caller2 = 0;
};

inline bool sameReaderKey(const ReaderKey& a, const ReaderKey& b) noexcept {
    return a.rip == b.rip && a.caller1 == b.caller1 && a.caller2 == b.caller2;
}

// Finds `key` among the first `used` keys of `table` (which the caller
// sizes to at least kMaxReaders). Returns its index, or kMaxReaders when
// no entry matches -- the caller's cue to insert at `used` (if `used <
// kMaxReaders`) or count an overflow.
inline uint32_t findReaderSlot(const ReaderKey* table, uint32_t used, const ReaderKey& key) noexcept {
    for (uint32_t i = 0; i < used; ++i) {
        if (sameReaderKey(table[i], key)) return i;
    }
    return kMaxReaders;
}

inline bool readerTableFull(uint32_t used) noexcept { return used >= kMaxReaders; }

// ---------------------------------------------------------------------------
// The dump-trigger filter (design doc, part C): with the instrument off,
// the OLD broad trigger (any of the detector's withheld-class verdicts)
// still applies -- eye_origin_trace's own behaviour, unchanged. With it on,
// the trigger narrows to just the scene-judged eye-camera-reset verdict,
// because the broad one spent 10 of 12 dumps on ordinary render-pass jumps
// in flight 195435 and left only 2 describing the thing being hunted.
inline bool dumpVerdictTrigger(bool readersOn, bool withheldClassVerdict, bool sceneResetVerdict) noexcept {
    return readersOn ? sceneResetVerdict : withheldClassVerdict;
}

}  // namespace prw
}  // namespace edvr
