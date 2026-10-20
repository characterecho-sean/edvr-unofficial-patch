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
// Exposure on the HDR route (docs/design-flat-temporal-aa-2026-09-23.md, sections 81 and 106). The flat profile's
// input H is pre-tonemap radiance. Bloom reads that radiance; the tonemap (ps E8948389) divides by strip texel 0
// afterwards. Scaling H by that luminance before bloom clips the lights (hangar, 2026-10-19, texel 0 at 150–271).
// NVIDIA's automatic exposure dimmed H and lost stars (flown 2026-10-09). The flat route stays off AutoExposure and
// passes no exposure texture, which NVIDIA treats as 1.0. The VR world route keeps AutoExposure: flatProfile false.
inline constexpr uint32_t flatDlssCreateFlags(bool hdr, bool flatProfile) {
    return kDlssFlagMvLowRes | kDlssFlagDepthInverted |
           (hdr ? (kDlssFlagIsHdr | (flatProfile ? 0u : kDlssFlagAutoExposure)) : 0u);
}
// Whether the HDR route's evaluation may hand NVIDIA the game's exposure texture: exactly when the feature lacks AutoExposure.
inline constexpr bool flatDlssFixedExposure(bool hdr, bool flatProfile) { return hdr && flatProfile; }

// FfxFsr3UpscalerInitializationFlagBits (FidelityFX-SDK-DX11 fork, ffx_fsr3upscaler.h).
constexpr uint32_t kFsrFlagHighDynamicRange = 1u << 0;
constexpr uint32_t kFsrFlagDepthInverted = 1u << 3;
constexpr uint32_t kFsrFlagDepthInfinite = 1u << 4;
constexpr uint32_t kFsrFlagAutoExposure = 1u << 5;
constexpr uint32_t kFsrFlagDebugChecking = 1u << 8;

// FSR: reversed-Z always; infinite depth and AMD's debug checking as before. The HDR route adds the HDR bit.
// Auto exposure is the VR world route. gameExposure drops it and the dispatch binds a 1x1 (no current caller).
// flatFixedExposure is the flat HDR route: HDR, no auto exposure, no texture, preExposure 1.
inline constexpr uint32_t flatFsrCreateFlags(bool infiniteDepth, bool diagnostics, bool hdr, bool gameExposure = false,
                                            bool flatFixedExposure = false) {
    const bool autoExposure = hdr && !gameExposure && !flatFixedExposure;
    return kFsrFlagDepthInverted | (infiniteDepth ? kFsrFlagDepthInfinite : 0u) |
           (diagnostics ? kFsrFlagDebugChecking : 0u) |
           (hdr ? kFsrFlagHighDynamicRange : 0u) | (autoExposure ? kFsrFlagAutoExposure : 0u);
}

// XeSS init flags (xess.h). Creation-time, so a change of gameExposure remakes the context. The constants are
// static_asserted against the SDK in xess_engine.cpp. LDR is the copy route. HDR with gameExposure uses the
// exposure-scale texture. flatFixedExposure is the flat HDR route: inverted depth only, so XeSS does not scale
// the scene (its exposure-scale texture flag is exactly that scale). HDR otherwise keeps automatic exposure (VR).
constexpr uint32_t kXessFlagInvertedDepth = 1u << 1;
constexpr uint32_t kXessFlagExposureTexture = 1u << 2;
constexpr uint32_t kXessFlagLdr = 1u << 6;
constexpr uint32_t kXessFlagAutoExposure = 1u << 8;
inline constexpr uint32_t flatXessInitFlags(bool hdr, bool gameExposure, bool flatFixedExposure = false) {
    if (!hdr) return kXessFlagInvertedDepth | kXessFlagLdr;
    if (gameExposure) return kXessFlagInvertedDepth | kXessFlagExposureTexture;
    // Tonemap off (LDR flag). The flat route round-trips radiance around XeSS itself, because
    // XeSS's own HDR tonemap at exposure 1 still blew the hangar lights (edvr_gfx_20261019_073958, flags 0x2).
    if (flatFixedExposure) return kXessFlagInvertedDepth | kXessFlagLdr;
    return kXessFlagInvertedDepth | kXessFlagAutoExposure;
}

}  // namespace edvr
