#pragma once
// The comfort fade's signal between EDVR's two halves (Explorer Cam, docs\design-explorer-cam-free-camera-2026-10-07.md, "Comfort fade").
//
// The d3d11 half owns the timeline (src\d3d11\explorer_cam_fade_core.h) and publishes one number a frame: how black the view is, 0 (clear) to 1 (black). The
// OpenXR runtime half blends black over each eye image by that amount when it composes (src\openxr\d3d11_stereo.cpp), and reads the number through
// EdvrNativeFrameOutput::fadeAlpha (native_frame.h, version 5), which the d3d11 half fills in at beginFrame from here.
//
// NEVER BLACK BY ACCIDENT. A reader that cannot trust the number reads 0:
//   * nothing published yet, or a mismatched DLL pair (the runtime asks version 5 and an older d3d11.dll refuses it, or the other way round: the output
//     struct then has no fadeAlpha at all, and native_frame_client.h zeroes it);
//   * a published level that is not a number, or below 0, reads 0; above 1 reads 1;
//   * a STALE level: the d3d11 half stopped publishing (the game is hung, or Explorer Cam stood down without saying so). Past kStaleHoldMs of silence the
//     level decays to 0 over kStaleDecayMs, so the user is never left in black. The price: a render-thread stall longer than 0.2 s while black lets the
//     scene show through, because from here a stall and a dead game cannot be told apart.
// One 64-bit atomic carries the level and the time it was published, so a reader never sees one with the other's age.
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace edvr {
namespace comfort {

constexpr uint32_t kStaleHoldMs = 200;    // a level older than this starts to decay
constexpr uint32_t kStaleDecayMs = 100;   // ...and is gone this much later: 0.3 s after the last publish

// A level a consumer may use: NaN and anything not above 0 read 0, anything above 1 reads 1.
inline float sanitize(float v) { return (v == v && v > 0.0f) ? (v > 1.0f ? 1.0f : v) : 0.0f; }

// bit 63: published; bits 62..32: the publish time in ms (31 bits, wrapping); bits 31..0: the level's float bits.
inline uint64_t pack(float alpha, uint64_t nowMs) {
    uint32_t bits = 0;
    std::memcpy(&bits, &alpha, 4);
    return (1ull << 63) | ((nowMs & 0x7FFFFFFFull) << 32) | bits;
}
// The level a reader should use at `nowMs`: the published level while it is fresh, decaying once stale, 0 when nothing was published.
inline float effective(uint64_t packed, uint64_t nowMs) {
    if ((packed >> 63) == 0) return 0.0f;
    uint32_t bits = static_cast<uint32_t>(packed & 0xFFFFFFFFull);
    float alpha = 0.0f;
    std::memcpy(&alpha, &bits, 4);
    alpha = sanitize(alpha);
    if (alpha <= 0.0f) return 0.0f;
    const uint32_t at = static_cast<uint32_t>((packed >> 32) & 0x7FFFFFFFull);
    uint32_t age = (static_cast<uint32_t>(nowMs & 0x7FFFFFFFull) - at) & 0x7FFFFFFFu;
    if (age > 0x40000000u) age = 0;   // published a moment after `nowMs` was read: fresh
    if (age <= kStaleHoldMs) return alpha;
    if (age >= kStaleHoldMs + kStaleDecayMs) return 0.0f;
    return alpha * (1.0f - static_cast<float>(age - kStaleHoldMs) / static_cast<float>(kStaleDecayMs));
}

inline std::atomic<uint64_t>& signalWord() {
    static std::atomic<uint64_t> word{0};
    return word;
}
// The frame thread, once a frame.
inline void publish(float alpha, uint64_t nowMs) { signalWord().store(pack(alpha, nowMs), std::memory_order_release); }
// The provider's beginFrame, once a runtime frame.
inline float read(uint64_t nowMs) { return effective(signalWord().load(std::memory_order_acquire), nowMs); }
// Forgotten: reads 0 until the next publish.
inline void clear() { signalWord().store(0, std::memory_order_release); }

}  // namespace comfort
}  // namespace edvr
