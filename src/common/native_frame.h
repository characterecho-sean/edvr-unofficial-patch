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
// Version 4 added one word to the END and nothing else, under the same
// hand-copied-DLLs rule (the projection channel of the terrain guard, retired
// 2026-10-09: the slot stays, see reservedProjectionChannel below).
#define EDVR_NATIVE_FRAME_VERSION_4 4u
// Version 5 adds fadeAlpha to the END and nothing else (Explorer Cam's comfort
// fade, comfort_fade.h), under the same hand-copied-DLLs rule: a version 1 to
// 4 caller never learns of it, and a runtime that asks version 5 of a
// d3d11.dll that refuses the shape steps down and reads "no fade" (0), never
// black.
#define EDVR_NATIVE_FRAME_VERSION_5 5u

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
    // RETIRED 2026-10-07 with the old Explorer Cam route (a headset-pose offset
    // and yaw, applied while a key-counting gate said the external camera was
    // up). Always zero now: the provider writes nothing here and the runtime
    // reads nothing. The slots stay so the struct, and the four versions that
    // name its prefixes, keep their layout -- the two DLLs are copied apart by
    // hand, and a runtime from before the change then reads "offset off".
    float reservedOffset[3];
    float reservedYaw;
    uint32_t reservedOffsetEnabled, reservedOffsetGamePoses;
    // RETIRED 2026-10-09 with the terrain guard (a mode, three margins, a count
    // and up to eight headset signatures: 21 words). Always zero now: the provider
    // writes nothing here and the runtime reads nothing. The slots stay so the
    // struct, and the versions that name its prefixes, keep their layout --
    // the two DLLs are copied apart by hand, and a runtime from before the
    // removal then reads a guard that is off.
    uint32_t reservedTerrainGuard[21];
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
    // RETIRED 2026-10-09 (version 4's one word: which projection query channel
    // the terrain guard's widened frustum was told through). Always zero, never
    // read; the slot stays for the same reason as the 21 above.
    uint32_t reservedProjectionChannel;
    // Version 5 and later. How black the projection layer is, 0 (clear, the
    // default and the only value an older provider can mean) to 1: the runtime
    // blends black over each eye image by this much when it composes. Already
    // freshness-checked by the provider (comfort_fade.h): a signal that went
    // stale reads 0.
    float fadeAlpha;
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
// The size the fields through deferredPacing occupy, which is what a
// version 3 caller's struct is, with no tail padding before
// reservedProjectionChannel.
#define EDVR_NATIVE_FRAME_OUTPUT_SIZE_3 \
    ((uint32_t)offsetof(EdvrNativeFrameOutput, reservedProjectionChannel))
// The size the fields through reservedProjectionChannel occupy, which is what a version 4
// caller's struct is, with no tail padding before fadeAlpha.
#define EDVR_NATIVE_FRAME_OUTPUT_SIZE_4 \
    ((uint32_t)offsetof(EdvrNativeFrameOutput, fadeAlpha))

struct EdvrNativeFrameDecision {
    uint32_t size, version;
    uint32_t withhold;
    uint32_t jumpOnly;
    uint32_t verdict;
};

typedef HRESULT(WINAPI *EdvrNativeFrameBegin)(
    void*, const EdvrNativeFrameInput*, EdvrNativeFrameOutput*);
// RETIRED 2026-10-09 with the terrain guard: this slot carried the guard's stage
// to the d3d11 half. Nothing calls it any more, and the provider's entry does
// nothing and returns S_OK, so a runtime built before the removal, which still
// checks that the slot is the provider's own, acquires as it always did.
typedef HRESULT(WINAPI *EdvrNativeFrameRetiredCall)(
    void*, uint32_t, float, float);
typedef HRESULT(WINAPI *EdvrNativeFrameLatchSubmit)(
    void*, uint64_t sequence, EdvrNativeFrameDecision*);
typedef HRESULT(WINAPI *EdvrNativeFrameInvalidate)(void*);
typedef HRESULT(WINAPI *EdvrNativeFrameClose)(void*);

struct EdvrNativeFrameTable {
    uint32_t size, version;
    void* context;
    EdvrNativeFrameBegin beginFrame;
    EdvrNativeFrameRetiredCall reservedCall;
    EdvrNativeFrameLatchSubmit latchSubmit;
    EdvrNativeFrameInvalidate invalidate;
    EdvrNativeFrameClose close;
};

extern "C" HRESULT WINAPI edvrAcquireNativeFrame(
    const EdvrNativeFrameRequest*, EdvrNativeFrameTable*);

namespace edvr {

// The worn headset's experimental.fov_trim_vertical/_outer/_nasal, resolved and
// cached by this DLL's own beginFrame (the same triple it hands the openvr
// half above): vertical, outer, nasal degrees, in that order. Takes
// native_frame.cpp's own g_mutex; zero in every slot until a beginFrame has
// resolved a headset (or when none is worn, e.g. under SteamVR/
// OpenComposite). For temporal_pass.cpp's fovea, which needs the same
// numbers in-process rather than across the DLL boundary above.
void nativeFrameFovTrimDegrees(uint32_t out[3]);

}  // namespace edvr
