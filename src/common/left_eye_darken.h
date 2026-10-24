#pragma once
// The left eye's darkening signal between EDVR's two halves (native on foot, F89, fix.native_on_foot_ads_close_left_eye). Same discipline as comfort_fade.h, whose level
// packing and staleness rule it reuses: the d3d11 half publishes one number a frame (0 the image as it is, 1 black), the provider's beginFrame reads it and hands it to the
// runtime in a version 10 answer (EdvrNativeFrameOutput::reservedProjectionChannel, the float's bits), and the runtime blends black over the LEFT eye's finished image by it
// as the last pass of that eye (src\openxr\d3d11_stereo.cpp). The right eye never reads it.
//
// NEVER DARK BY ACCIDENT. A reader that cannot trust the number reads 0: nothing published, a level that is not a number or below 0, a level above 1 (reads 1), a signal that
// went stale (the d3d11 half stopped publishing: past kStaleHoldMs of silence it decays to 0 over kStaleDecayMs), and any answer that is not version 10.
#include "comfort_fade.h"

namespace edvr {
namespace eyedarken {

inline std::atomic<uint64_t>& signalWord() {
    static std::atomic<uint64_t> word{0};
    return word;
}
// The frame thread, once a frame.
inline void publish(float level, uint64_t nowMs) { signalWord().store(comfort::pack(level, nowMs), std::memory_order_release); }
// The provider's beginFrame, once a runtime frame.
inline float read(uint64_t nowMs) { return comfort::effective(signalWord().load(std::memory_order_acquire), nowMs); }
// Forgotten: reads 0 until the next publish.
inline void clear() { signalWord().store(0, std::memory_order_release); }

// The wire form: the float's bits (the slot is a uint32).
inline uint32_t toBits(float level) {
    const float v = comfort::sanitize(level);
    uint32_t bits = 0;
    std::memcpy(&bits, &v, 4);
    return bits;
}
inline float fromBits(uint32_t bits) {
    float v = 0.0f;
    std::memcpy(&v, &bits, 4);
    return comfort::sanitize(v);
}

}  // namespace eyedarken
}  // namespace edvr
