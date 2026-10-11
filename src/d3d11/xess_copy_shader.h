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
}  // namespace edvr
