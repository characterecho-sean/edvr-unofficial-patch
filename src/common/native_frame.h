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
};

// The size the fields through resubmitEnabled occupy, which is what a
// version 1 caller's struct is. Every member is four bytes, so this is the
// offset of the first version 2 field with no tail padding in play.
#define EDVR_NATIVE_FRAME_OUTPUT_SIZE_1 \
    ((uint32_t)offsetof(EdvrNativeFrameOutput, trimOuterDeg))

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
