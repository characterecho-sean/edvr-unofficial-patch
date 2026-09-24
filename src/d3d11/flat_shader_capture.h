#pragma once

#include <cstddef>
#include <cstdint>

namespace edvr {
// Creation-time cache for the manually armed, exact-resource producer probe.
// Count and byte budgets are independent; arithmetic never wraps.
inline constexpr bool flatProbeShaderFits(size_t count, size_t used, size_t incoming) {
    constexpr size_t budget = 16u * 1024u * 1024u;
    return count < 2048 && incoming > 0 && incoming <= 1024u * 1024u &&
        used <= budget && incoming <= budget - used;
}
// Missing bytecodes in the verified Epic 37062878/frame36865 contract.
// This is capture admission only; it grants no motion or treatment support.
struct FlatShaderCaptureKey { char stage; uint64_t hash; };
inline constexpr FlatShaderCaptureKey kFlatShaderCaptureKeys[] = {
    {'v', 0x1F3AD1584D7FA3C8ull}, {'v', 0x4D516EF05C68FFA5ull},
    {'v', 0x6041FD2D3D0164E1ull}, {'v', 0x8BD7C37ABCEE7E45ull},
    {'v', 0x94D5C556DFD6D705ull}, {'v', 0xBBAD1CA808E1E292ull},
    {'p', 0x147E748F4CD3AE9Aull}, {'p', 0x188A933094FB422Aull},
    {'p', 0x4E4FF61E8A08FC7Eull}, {'p', 0x94676B1FD0DF150Full},
    {'p', 0xBA65C50BBA1ECCBBull}, {'p', 0xE54F2A902E5631F6ull},
    {'p', 0xFEE777E92850B390ull},
};
inline constexpr size_t kFlatShaderCaptureCount =
    sizeof(kFlatShaderCaptureKeys) / sizeof(kFlatShaderCaptureKeys[0]);
inline constexpr uint32_t flatShaderCaptureBit(bool flat, char stage, uint64_t hash) {
    if (flat) for (size_t i = 0; i < kFlatShaderCaptureCount; ++i)
        if (kFlatShaderCaptureKeys[i].stage == stage && kFlatShaderCaptureKeys[i].hash == hash)
            return uint32_t(1) << i;
    return 0;
}
static_assert(kFlatShaderCaptureCount == 13 && kFlatShaderCaptureCount <= 32,
              "bounded capture mask must cover the exact missing shader set");
} // namespace edvr
