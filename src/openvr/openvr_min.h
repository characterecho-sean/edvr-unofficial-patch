// Minimal OpenVR declarations.
//
// Deliberately not the vendored SDK: we only need the ABI shapes of the few
// calls we intercept, and keeping the surface small makes it obvious exactly
// what we touch. Types follow the public openvr.h (Valve, BSD-3-Clause) so the
// binary layout matches what the runtime expects.
//
// NOTE ON CONFIDENCE: the struct layouts below are stable across the OpenVR 1.x
// interface generations. The *vtable indices* used by compositor_hook.cpp are
// not part of these declarations, are not verifiable from here, and are treated
// as unverified throughout -- which is why that file validates the arguments of
// the first calls it receives and goes inert if they do not look right.
#pragma once

#include <cstdint>

namespace vr {

enum EVREye : int32_t {
    Eye_Left  = 0,
    Eye_Right = 1,
};

enum ETextureType : int32_t {
    TextureType_Invalid           = -1,
    TextureType_DirectX           = 0,
    TextureType_OpenGL            = 1,
    TextureType_Vulkan            = 2,
    TextureType_IOSurface         = 3,
    TextureType_DirectX12         = 4,
    TextureType_DXGISharedHandle  = 5,
    TextureType_Metal             = 6,
};

enum EColorSpace : int32_t {
    ColorSpace_Auto   = 0,
    ColorSpace_Gamma  = 1,
    ColorSpace_Linear = 2,
};

enum EVRSubmitFlags : int32_t {
    Submit_Default                       = 0x00,
    Submit_LensDistortionAlreadyApplied  = 0x01,
    Submit_GlRenderBuffer                = 0x02,
    Submit_Reserved                      = 0x04,
    Submit_TextureWithPose               = 0x08,
    Submit_TextureWithDepth              = 0x10,
    Submit_FrameDiscontinuty             = 0x20,
    Submit_VulkanTextureWithArrayData    = 0x40,
};

enum ETrackingUniverseOrigin : int32_t {
    TrackingUniverseSeated          = 0,
    TrackingUniverseStanding        = 1,
    TrackingUniverseRawAndUncalibrated = 2,
};

enum ETrackingResult : int32_t {
    TrackingResult_Uninitialized = 1,
    TrackingResult_Running_OK    = 200,
    TrackingResult_Running_OutOfRange = 201,
};

struct HmdMatrix34_t { float m[3][4]; };
struct HmdMatrix44_t { float m[4][4]; };
struct HmdVector3_t  { float v[3]; };

struct Texture_t {
    void*        handle;
    ETextureType eType;
    EColorSpace  eColorSpace;
};

struct VRTextureBounds_t {
    float uMin, vMin;
    float uMax, vMax;
};

struct TrackedDevicePose_t {
    HmdMatrix34_t   mDeviceToAbsoluteTracking;
    HmdVector3_t    vVelocity;
    HmdVector3_t    vAngularVelocity;
    ETrackingResult eTrackingResult;
    bool            bPoseIsValid;
    bool            bDeviceIsConnected;
};

constexpr uint32_t k_unMaxTrackedDeviceCount = 64;
constexpr uint32_t k_unTrackedDeviceIndex_Hmd = 0;

typedef int32_t EVRCompositorError;
typedef int32_t EVRInitError;

}  // namespace vr
