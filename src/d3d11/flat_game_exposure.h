#pragma once
#include <cstdint>
#include <d3d11.h>

namespace edvr {

// CS F1FB2EEF, the exposure shader (gpu_pass_profile.cpp). Its u1 is the 6x1 R32 strip.
constexpr uint64_t kFlatExposureShader = 0xF1FB2EEFB662F3AAull;

// The flat HDR route's exposure (docs/design-flat-temporal-aa-2026-09-23.md, section 106).
// The game's tonemap vertex shader samples strip texel 0. That texel is copied on the GPU into a
// 1x1 R32_FLOAT and handed to whichever backend is resolving H, so bloom thresholds the same
// radiance the copy route does. Null until the strip has been seen and one readback of texel 0
// was a finite value: those frames keep the backend's previous exposure (DLSS: none, which NVIDIA
// treats as 1; FSR and XeSS: automatic). No CPU value is ever passed in.
struct FlatGameExposureView {
    ID3D11Texture2D* texture = nullptr;           // the 1x1, owned here; not AddRef'd for the caller
    ID3D11ShaderResourceView* srv = nullptr;
};

// Owner immediate context, from the flat Dispatch hook, once the bound compute shader's hash is known.
// A match on F1FB2EEF remembers a 6x1 R32 strip bound at u1.
void flatGameExposureConsider(uint64_t shaderHash, ID3D11UnorderedAccessView* uav1);

// Copies texel 0 into the 1x1. Empty until the strip is known and a lagged read of texel 0 was sane.
FlatGameExposureView flatGameExposurePrepare(ID3D11DeviceContext* ctx);

void flatGameExposureShutdown();

}  // namespace edvr
