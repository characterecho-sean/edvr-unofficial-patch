// The depth-of-field blur's average (flat_dof_blur.h): its rules, with no graphics API in them, so the rig runs the shipped decisions.
#pragma once

#include <cmath>
#include <cstdint>

namespace edvr {

// The frames the average spans at most. The jitter's cycle is 8 phases, so over a whole cycle a still scene's blur sums to the same image every frame.
constexpr uint32_t kFlatDofBlurMaxFrames = 8;
// The draws that lay the blur over the scene, both with the blur at t1: the separate composite (the 3D menus and the lobby) and the tone that blends it in.
constexpr uint64_t kFlatDofBlurCompositePs = 0xBF2302BCC7B434DBull;
constexpr uint64_t kFlatDofBlurTonePs = 0xDE65BFFF2F12ECC6ull;
constexpr uint32_t kFlatDofBlurSlot = 1;
// Frames without a composite before the images are let go: about 5 s at 90 fps. The composite runs in menus, so a flight holds no memory for it.
constexpr uint64_t kFlatDofBlurIdleFrames = 450;
// A camera step at or under these keeps the average; over either, the frame's own blur. Half a pixel of a 540-row blur at the game's vertical focal of 1.87 is
// 0.001 radians, and 5 mm at 5 m is the same half pixel. The 3D menu's slow sway is about a hundredth of that.
constexpr float kFlatDofBlurStillAngle = 1e-3f;
constexpr float kFlatDofBlurStillStep = 5e-3f;

inline bool flatDofBlurComposite(uint64_t ps) {
    return ps == kFlatDofBlurCompositePs || ps == kFlatDofBlurTonePs;
}

enum class FlatDofBlurStep : uint8_t {
    Average,
    NotJittered,    // nothing jittered this frame, so the blur is already steady
    HdrRoute,       // the HDR route resolved the scene before the blur read it
    CameraUnknown,  // no camera comparison from last frame's resolve
    CameraMoved,    // the camera moved: an average would trail behind it
    Count
};

struct FlatDofBlurAsk {
    bool jittered = false;
    bool hdrRouteTreated = false;
    bool cameraKnown = false;
    bool cameraMoved = false;
};

inline FlatDofBlurStep flatDofBlurDecide(const FlatDofBlurAsk& a) {
    if (!a.jittered) return FlatDofBlurStep::NotJittered;
    if (a.hdrRouteTreated) return FlatDofBlurStep::HdrRoute;
    if (!a.cameraKnown) return FlatDofBlurStep::CameraUnknown;
    if (a.cameraMoved) return FlatDofBlurStep::CameraMoved;
    return FlatDofBlurStep::Average;
}

// Rows 4 and 5 of the camera record, the forward axis and the origin. They carry no jitter, which rows 0 to 3 do. The forward axis does not see roll, and the game's
// cameras here do not roll.
inline bool flatDofBlurCameraMoved(const float* forwardNow, const float* forwardBefore, const float* originNow, const float* originBefore) {
    for (int i = 0; i < 3; ++i) {
        // Written so a NaN reads as moved.
        if (!(std::fabs(forwardNow[i] - forwardBefore[i]) <= kFlatDofBlurStillAngle)) return true;
        if (!(std::fabs(originNow[i] - originBefore[i]) <= kFlatDofBlurStillStep)) return true;
    }
    return false;
}

// A frame's blur goes into the slot push returns; count is how many hold this run's frames. The window is the jitter's cycle, at most kFlatDofBlurMaxFrames.
struct FlatDofBlurRing {
    uint32_t next = 0, count = 0, window = kFlatDofBlurMaxFrames;
    void restart() { next = count = 0; }
    uint32_t push(uint32_t phaseCount) {
        const uint32_t wanted = phaseCount == 0 || phaseCount > kFlatDofBlurMaxFrames ? kFlatDofBlurMaxFrames : phaseCount;
        if (wanted != window) {
            window = wanted;
            restart();
        }
        const uint32_t slot = next;
        next = (next + 1) % window;
        if (count < window) ++count;
        return slot;
    }
};

// The blur as the composite binds it, plain enough to copy and to read back whole.
struct FlatDofBlurShape {
    bool tex2dView = false;
    uint32_t viewFormat = 0, mostDetailedMip = 0, mipLevels = 0, arraySize = 0, sampleCount = 0, width = 0, height = 0;
};

// DXGI's R16G16B16A16_FLOAT and R11G11B10_FLOAT, the two the game's blur has been seen in (flat_dof_blur.cpp asserts the numbers).
inline bool flatDofBlurShapeOk(const FlatDofBlurShape& s) {
    return s.tex2dView && (s.viewFormat == 10 || s.viewFormat == 26) && s.mostDetailedMip == 0 && s.mipLevels == 1 && s.arraySize == 1 && s.sampleCount == 1 &&
           s.width != 0 && s.height != 0;
}

// A failed attempt to make the averaged images (flat_dof_blur.cpp ensureImages) is not repeated every composite: it is remembered by shape and by the frame
// it failed on, and tried again after kFlatDofBlurIdleFrames frames, at once when the shape changes, and at once after a clear (a resize or a device change).
struct FlatDofBlurShapeKey {
    uint32_t width = 0, height = 0, textureFormat = 0, viewFormat = 0;
    bool operator==(const FlatDofBlurShapeKey& o) const { return width == o.width && height == o.height && textureFormat == o.textureFormat && viewFormat == o.viewFormat; }
};
struct FlatDofBlurRetry {
    bool failed = false;
    FlatDofBlurShapeKey key;
    uint64_t failedAt = 0;
    bool noted = false;   // the log line for this shape has been written
};
inline bool flatDofBlurRetryAllowed(const FlatDofBlurRetry& r, const FlatDofBlurShapeKey& key, uint64_t frame) {
    if (!r.failed || !(r.key == key)) return true;
    return frame < r.failedAt || frame - r.failedAt >= kFlatDofBlurIdleFrames;   // a frame counter that went backwards is a new run
}
// Returns true when the failure should be logged: the first for this shape.
inline bool flatDofBlurRetryFailed(FlatDofBlurRetry& r, const FlatDofBlurShapeKey& key, uint64_t frame) {
    const bool say = !(r.noted && r.key == key);
    r.failed = true;
    r.key = key;
    r.failedAt = frame;
    r.noted = true;
    return say;
}
inline void flatDofBlurRetryClear(FlatDofBlurRetry& r) { r = FlatDofBlurRetry{}; }

}  // namespace edvr
