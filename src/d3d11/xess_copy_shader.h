#pragma once

namespace edvr {
// XeSS requires the same colour format for input and output. The HDR flat
// input is R11G11B10_FLOAT; this copy widens its matched XeSS result into the
// flat resolver's R16G16B16A16_FLOAT output without requantising history.
inline constexpr char kXessR11ToFp16Cs[] = R"HLSL(
Texture2D<float4> inputColor : register(t0);
RWTexture2D<float4> outputColor : register(u0);
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    uint width, height;
    outputColor.GetDimensions(width, height);
    if (id.x >= width || id.y >= height) return;
    outputColor[id.xy] = inputColor.Load(int3(id.xy, 0));
}
)HLSL";

// Flat HDR route. XeSS is told the colour is LDR so it does not apply its own tonemap
// (that tonemap at exposure 1 blew the hangar lights, flags 0x2, 2026-10-19). These two
// entries are inverses: a flat panel comes back as the same radiance, and bloom sees it.
inline constexpr char kXessHdrCurveCs[] = R"HLSL(
Texture2D<float4> inputColor : register(t0);
RWTexture2D<float4> outputColor : register(u0);
float3 compress(float3 c) {
    c = max(c, 0);
    float m = max(c.r, max(c.g, c.b));
    return c / (1 + m);
}
float3 expand(float3 c) {
    c = clamp(c, 0, 0.999);
    float m = max(c.r, max(c.g, c.b));
    return c / max(1 - m, 1e-4);
}
[numthreads(8, 8, 1)]
void compressMain(uint3 id : SV_DispatchThreadID) {
    uint width, height;
    outputColor.GetDimensions(width, height);
    if (id.x >= width || id.y >= height) return;
    outputColor[id.xy] = float4(compress(inputColor.Load(int3(id.xy, 0)).rgb), 1);
}
[numthreads(8, 8, 1)]
void expandMain(uint3 id : SV_DispatchThreadID) {
    uint width, height;
    outputColor.GetDimensions(width, height);
    if (id.x >= width || id.y >= height) return;
    outputColor[id.xy] = float4(expand(inputColor.Load(int3(id.xy, 0)).rgb), 1);
}
)HLSL";
}  // namespace edvr
