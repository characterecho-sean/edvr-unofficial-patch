// The shield ripple's door pass (src/d3d11/shield_ripple.h). Header-only HLSL text, no device: the build
// compiles it to fixed DXBC (tools/temporal_shader_build/fixed_shield_ripple_variants.h) and the rig runs those bytes.
#pragma once

#include <cstdint>

namespace edvr {

// The parameters of the door pass, laid out for its constant buffer: four 16-byte rows.
struct ShieldRippleParams {
    int32_t region[4];     // the frame's region (x0, y0, x1, y1), in the frame's pixels
    float layerUv[4];      // the eye uv of the region's corners (u0, v0, u1, v1)
    float jitterUv[2];     // the draw's jitter in the uv of the render's size
    uint32_t outSize[2];   // the region's size
};
static_assert(sizeof(ShieldRippleParams) == 48, "the door pass's constant buffer is three rows");

// The door pass. Copy: a region-sized copy of the frame (the RCAS output) in the frame's UNORM view; D: the layer's R32G32 uv_jit + d (cleared far outside
// [0, 1] where the bubble did not draw); Frame: the same frame as a typed UAV, written ONLY where D covers. The displaced read is the copy's, filtered
// linearly and clamped, at D - jitter: the copy is the one image that is read, so a pixel never reads a pixel this pass has written.
constexpr char kShieldRippleDoorCsHlsl[] = R"HLSL(
cbuffer P : register(b0) {
    int4 region;
    float4 layerUv;
    float2 jitterUv;
    uint2 outSize;
};
Texture2D<float4> Copy : register(t0);
Texture2D<float2> D : register(t1);
RWTexture2D<unorm float4> Frame : register(u0);
SamplerState Lin : register(s0);
[numthreads(32, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= outSize.x || id.y >= outSize.y) return;
    float2 local = (float2(id.xy) + 0.5) / float2(outSize);
    float2 uvEye = layerUv.xy + local * (layerUv.zw - layerUv.xy);
    uint dw, dh;
    D.GetDimensions(dw, dh);
    int2 dp = int2(min(uvEye * float2(dw, dh), float2(dw - 1, dh - 1)));
    float2 d = D.Load(int3(dp, 0));
    if (d.x < -1000.0) return;
    float2 uvS = d - jitterUv;
    float2 localS = (uvS - layerUv.xy) / (layerUv.zw - layerUv.xy);
    Frame[uint2(region.xy) + id.xy] = Copy.SampleLevel(Lin, localS, 0);
}
)HLSL";

}  // namespace edvr
