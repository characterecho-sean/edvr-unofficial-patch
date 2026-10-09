// The HDR route's backend creation flags, as pure functions (docs/design-flat-temporal-aa-2026-09-23.md,
// section 81). Creation-time flags of NVIDIA's DLSS feature and AMD's FSR 3.1 upscaler context cannot change
// after the feature exists, so the flag set is part of each backend's feature key and a route flip remakes the
// feature. The constants mirror the SDK enums; dlaa.cpp and fsr3_engine.cpp static_assert every one of them
// against the real header, so a drift in either SDK fails the build instead of a flight.
//
// With hdr false both functions return exactly the flags the LDR copy route has always created with: the rig
// pins that, because "the copy route is unchanged" is the contract of everything below.
#pragma once
#include <cstdint>

namespace edvr {

// NVSDK_NGX_DLSS_Feature_Flags_* (third_party/ngx/include/nvsdk_ngx_defs.h).
constexpr uint32_t kDlssFlagIsHdr = 1u << 0;
constexpr uint32_t kDlssFlagMvLowRes = 1u << 1;
constexpr uint32_t kDlssFlagMvJittered = 1u << 2;
constexpr uint32_t kDlssFlagDepthInverted = 1u << 3;
constexpr uint32_t kDlssFlagAutoExposure = 1u << 6;

// DLSS: motion vectors at the render size and unjittered (the prep pass computes them on the unjittered grid),
// reversed-Z depth. The HDR route adds IsHDR. MVJittered stays off in both: the vectors exclude the raster phase.
// Exposure on the HDR route (docs/design-flat-temporal-aa-2026-09-23.md, section 106): the flat profile's input H is
// pre-tonemap radiance the game's own tone pass exposes later, so it is evaluated with a fixed exposure of 1.0 (a 1x1
// exposure texture, dlaa.cpp) and no AutoExposure; NVIDIA's automatic exposure dimmed the picture and lost stars (flown
// 2026-10-09). The VR world route, the other caller with hdr true, keeps AutoExposure: flatProfile false.
inline constexpr uint32_t flatDlssCreateFlags(bool hdr, bool flatProfile) {
    return kDlssFlagMvLowRes | kDlssFlagDepthInverted |
           (hdr ? (kDlssFlagIsHdr | (flatProfile ? 0u : kDlssFlagAutoExposure)) : 0u);
}
// Whether the HDR route's evaluation hands NVIDIA the fixed exposure texture: exactly when the feature lacks AutoExposure.
inline constexpr bool flatDlssFixedExposure(bool hdr, bool flatProfile) { return hdr && flatProfile; }

// FfxFsr3UpscalerInitializationFlagBits (FidelityFX-SDK-DX11 fork, ffx_fsr3upscaler.h).
constexpr uint32_t kFsrFlagHighDynamicRange = 1u << 0;
constexpr uint32_t kFsrFlagDepthInverted = 1u << 3;
constexpr uint32_t kFsrFlagDepthInfinite = 1u << 4;
constexpr uint32_t kFsrFlagAutoExposure = 1u << 5;
constexpr uint32_t kFsrFlagDebugChecking = 1u << 8;

// FSR: reversed-Z always; infinite depth and AMD's debug checking as before. The HDR route adds the HDR bit and
// auto exposure, and keeps the exposure resource null and preExposure 1 at dispatch (auto exposure computes it).
inline constexpr uint32_t flatFsrCreateFlags(bool infiniteDepth, bool diagnostics, bool hdr) {
    return kFsrFlagDepthInverted | (infiniteDepth ? kFsrFlagDepthInfinite : 0u) |
           (diagnostics ? kFsrFlagDebugChecking : 0u) |
           (hdr ? (kFsrFlagHighDynamicRange | kFsrFlagAutoExposure) : 0u);
}

}  // namespace edvr
