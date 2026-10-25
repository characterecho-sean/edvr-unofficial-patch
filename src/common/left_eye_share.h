#pragma once
// The left eye's share of the right eye's image, between EDVR's two halves (native on foot, F106, fix.native_on_foot_ads_sight_both_eyes). Same discipline as
// comfort_fade.h, whose level packing and staleness rule it reuses: the d3d11 half publishes one number a frame (0: the left eye's own image; 1: the right eye's image,
// turned into the left eye's view), the provider's beginFrame reads it and hands it to the runtime in a version 10 answer (EdvrNativeFrameOutput::leftEyeShare, the
// float's bits), and the runtime draws the right eye's image over the left eye's own by it, in the scene pass of that eye (src\openxr\d3d11_stereo.cpp, eye_share.h).
// The right eye never reads it.
//
// F89 sent the left eye's darkening in the same slot, with the same timing (the ADS lock's weight). A runtime from F106 on reads such a d3d11.dll's level as the
// share; a runtime from F89 to F105 reads this d3d11.dll's share as a darkening.
//
// NEVER SHARED BY ACCIDENT. A reader that cannot trust the number reads 0: nothing published, a level that is not a number or below 0, a level above 1 (reads 1), a signal
// that went stale (the d3d11 half stopped publishing: past kStaleHoldMs of silence it decays to 0 over kStaleDecayMs), and any answer that is not version 10.
//
// The way back. Once a press, when the share has come back to 0, the runtime tells the d3d11 half what it drew (EdvrNativeEyeShareReport, through the d3d11.dll export
// edvrNoteNativeEyeShare), and the d3d11 half writes it into the graphics log, where --health reads it. A d3d11.dll without the export is not told; the runtime's own log
// has the same figures.
#include "comfort_fade.h"

#include <windows.h>

#define EDVR_NATIVE_EYE_SHARE_REPORT_VERSION_1 1u

// One press of the sights, as the runtime drew it. Every figure is the runtime's: the views are the ones its projection layer submitted.
struct EdvrNativeEyeShareReport {
    uint32_t size, version;
    float peak;                // the highest share the press reached, 0..1
    uint32_t frames;           // frames the runtime drew the share in (a share above 0)
    uint32_t framesFull;       // ... of them at full weight (1)
    uint32_t refused;          // frames with a share above 0 the runtime did not draw it in (views it could not map)
    uint32_t replayed;         // frames among `frames` drawn on a replayed pair
    float seconds;             // from the first shared frame to the last
    float rotationDeg[3];      // the left view's turn against the right view's (inverse(q_right) * q_left): yaw, pitch, roll in degrees, at the press's last frame
    float leftTangents[4];     // the tangents of the left view's fov angles: left, right, up, down
    float rightTangents[4];    // the right view's
    float coverage;            // the mean share of the left eye's image taken from the right at full weight (a 48 x 48 grid over its placement)
    float coverageWhole;       // the part of that grid taken wholly from the right
    uint32_t hiddenTriangles;  // the right eye's hidden-area triangles in the mask; 0: no mask (the runtime had none)
};
typedef HRESULT(WINAPI* EdvrNoteNativeEyeShare)(const EdvrNativeEyeShareReport*);
// The d3d11.dll export (native_on_foot.cpp): checks the report and writes its line. S_OK when written, E_INVALIDARG for a report it cannot read.
extern "C" HRESULT WINAPI edvrNoteNativeEyeShare(const EdvrNativeEyeShareReport* report);

namespace edvr {
namespace eyeshare {

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

// A report the d3d11 half can read: its own size and version, and every figure finite.
inline bool reportReadable(const EdvrNativeEyeShareReport* r) {
    if (!r || r->size < sizeof(EdvrNativeEyeShareReport) || r->version != EDVR_NATIVE_EYE_SHARE_REPORT_VERSION_1) return false;
    const float values[] = {r->peak, r->seconds, r->rotationDeg[0], r->rotationDeg[1], r->rotationDeg[2], r->leftTangents[0], r->leftTangents[1], r->leftTangents[2],
                            r->leftTangents[3], r->rightTangents[0], r->rightTangents[1], r->rightTangents[2], r->rightTangents[3], r->coverage, r->coverageWhole};
    for (float v : values)
        if (!(v == v) || v > 1e9f || v < -1e9f) return false;
    return true;
}

}  // namespace eyeshare
}  // namespace edvr
