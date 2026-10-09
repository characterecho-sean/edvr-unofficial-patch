#pragma once
// Pure logic for composing the x86 debug register Dr7 (slot 0), shared by the
// hardware write watches that arm a data breakpoint on a game address
// (flat_camera_producer_probe.cpp). No Windows header, no game memory --
// tools\transition_flash_prevent_test drives it without the game.
#include <cstdint>

namespace edvr {
namespace dr7 {

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

}  // namespace dr7
}  // namespace edvr
