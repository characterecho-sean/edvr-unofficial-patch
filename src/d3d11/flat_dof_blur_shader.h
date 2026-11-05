// The depth-of-field blur's average (flat_dof_blur.h): the mean of the frames bound at t0 to t7. An unbound slot reads zero, so weight is 1 over the frames bound.
#pragma once

namespace edvr {

inline constexpr char kFlatDofBlurCsHlsl[] = R"HLSL(
cbuffer Average : register(b0) {
    float4 weight;
};
Texture2D<float4> f0 : register(t0);
Texture2D<float4> f1 : register(t1);
Texture2D<float4> f2 : register(t2);
Texture2D<float4> f3 : register(t3);
Texture2D<float4> f4 : register(t4);
Texture2D<float4> f5 : register(t5);
Texture2D<float4> f6 : register(t6);
Texture2D<float4> f7 : register(t7);
RWTexture2D<float4> averaged : register(u0);

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    const int3 p = int3(id.xy, 0);
    averaged[id.xy] = (f0.Load(p) + f1.Load(p) + f2.Load(p) + f3.Load(p) + f4.Load(p) + f5.Load(p) + f6.Load(p) + f7.Load(p)) * weight.x;
}
)HLSL";

}  // namespace edvr
