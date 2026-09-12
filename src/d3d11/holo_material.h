#pragma once
#include <cstdint>

namespace edvr {
// Verified from the original material bytecode. Both sample TEXCOORD8
// through s1, but the unlit variant has one fewer preceding texture.
constexpr uint64_t kHoloLitPs = 0xA2965EC2931A39C8ull;
constexpr uint64_t kHoloUnlitPs = 0xB4786E0A0B199285ull;
constexpr unsigned holoSurfaceSlot(uint64_t ps) {
    return ps == kHoloUnlitPs ? 1u : 2u;
}
}
