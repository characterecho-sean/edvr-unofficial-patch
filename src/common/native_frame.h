#pragma once

#include <windows.h>
#include <d3d11.h>
#include <stddef.h>
#include <stdint.h>

#define EDVR_NATIVE_FRAME_VERSION_1 1u
// Version 2 adds the field-of-view trim to the END of EdvrNativeFrameOutput
// and nothing else. The two DLLs are shipped together but are copied apart
// by hand all the time, so the provider answers either version and a
// version 1 caller simply gets no trim.
#define EDVR_NATIVE_FRAME_VERSION_2 2u
// Version 3 adds deferredPacing to the END and nothing else. The same
// hand-copied-DLLs rule applies: the provider answers whichever of the three
// versions a caller's struct shape asks for, and a version 1 or 2 caller
// simply never turbo-paces, exactly as it never got a trim before version 2.
#define EDVR_NATIVE_FRAME_VERSION_3 3u

// The game producer owns the device and generation passed at acquire. The
// methods in the table are CPU-only and are called by the XR owner after the
// producer has acquired the capability.
struct EdvrNativeFrameRequest {
    uint32_t size, version;
    ID3D11Device* gameDevice;
    uint64_t generation;
};

struct EdvrNativeFrameInput {
    uint32_t size, version;
    uint64_t generation, referenceGeneration, sequence;
    // Runtime pose, row-major 3x4. valid is zero when no pose was provided.
    float physicalHead[12];
    uint32_t valid;
};

struct EdvrNativeFrameOutput {
    uint32_t size, version;
    // Tracking coordinates: +right, +up, -forward.
    float headOffset[3];
    float yawRadians;
    uint32_t offsetEnabled, offsetGamePoses;
    uint32_t cullMode; // 0 off, 1 symmetric, 2 percent
    float cullPercent, cullHorizontalFraction, cullVerticalFraction;
    uint32_t cullSignatureCount;
    uint32_t cullSignatures[8][2];
    uint32_t sceneReady;
    uint32_t transitionEnabled;
    uint32_t resubmitEnabled;
    // Version 2 and later. Degrees off each eye's outer (temple), nasal
    // (nose) and vertical edges, 0..30, as a narrower projection and a
    // proportionally smaller render size.
    float trimOuterDeg, trimNasalDeg, trimVerticalDeg;
    // Version 3 and later. 1: hold the frame wait for the second Submit
    // (fix.weapon_stability while on foot); 0: the wait stays in
    // WaitGetPoses.
    uint32_t deferredPacing;
};

// The size the fields through resubmitEnabled occupy, which is what a
// version 1 caller's struct is. Every member is four bytes, so this is the
// offset of the first version 2 field with no tail padding in play.
#define EDVR_NATIVE_FRAME_OUTPUT_SIZE_1 \
    ((uint32_t)offsetof(EdvrNativeFrameOutput, trimOuterDeg))
// The size the fields through trimVerticalDeg occupy, which is what a
// version 2 caller's struct is: version 1's fields plus the three trims,
// with no tail padding before deferredPacing.
#define EDVR_NATIVE_FRAME_OUTPUT_SIZE_2 \
    ((uint32_t)offsetof(EdvrNativeFrameOutput, deferredPacing))

struct EdvrNativeFrameDecision {
    uint32_t size, version;
    uint32_t withhold;
    uint32_t jumpOnly;
    uint32_t verdict;
};

typedef HRESULT(WINAPI *EdvrNativeFrameBegin)(
    void*, const EdvrNativeFrameInput*, EdvrNativeFrameOutput*);
typedef HRESULT(WINAPI *EdvrNativeFrameSetCullState)(
    void*, uint32_t stage, float factorH, float factorV);
typedef HRESULT(WINAPI *EdvrNativeFrameLatchSubmit)(
    void*, uint64_t sequence, EdvrNativeFrameDecision*);
typedef HRESULT(WINAPI *EdvrNativeFrameInvalidate)(void*);
typedef HRESULT(WINAPI *EdvrNativeFrameClose)(void*);

struct EdvrNativeFrameTable {
    uint32_t size, version;
    void* context;
    EdvrNativeFrameBegin beginFrame;
    EdvrNativeFrameSetCullState setCullState;
    EdvrNativeFrameLatchSubmit latchSubmit;
    EdvrNativeFrameInvalidate invalidate;
    EdvrNativeFrameClose close;
};

extern "C" HRESULT WINAPI edvrAcquireNativeFrame(
    const EdvrNativeFrameRequest*, EdvrNativeFrameTable*);

namespace edvr {

// The worn headset's fix.fov_trim_vertical/_outer/_nasal, resolved and
// cached by this DLL's own beginFrame (the same triple it hands the openvr
// half above): vertical, outer, nasal degrees, in that order. Takes
// native_frame.cpp's own g_mutex; zero in every slot until a beginFrame has
// resolved a headset (or when none is worn, e.g. under SteamVR/
// OpenComposite). For temporal_pass.cpp's fovea, which needs the same
// numbers in-process rather than across the DLL boundary above.
void nativeFrameFovTrimDegrees(uint32_t out[3]);

}  // namespace edvr
