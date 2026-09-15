#pragma once
namespace edvr {
// Colour remains in Elite's original R11 target format. The original tone
// shaders evaluate the clean and UI-bearing images with the same exposure/LUT.
// Only the current UI's residual is added to the reconstructed world. There
// is no history, synthetic depth or motion vector for a deferred UI element.
inline constexpr char kDeferredSeed[] = R"HLSL(
Texture2D<float3> Source : register(t0);
SamplerState LinearClamp : register(s0);
cbuffer Params : register(b0) { float2 outSize; float2 jitterUV; };
float4 vs(uint i:SV_VertexID):SV_Position {
    return float4(i==2?3:-1,i==1?3:-1,0,1);
}
float3 ps(float4 p:SV_Position):SV_Target {
    return Source.SampleLevel(LinearClamp,p.xy/outSize+jitterUV,0);
}
)HLSL";
inline constexpr char kDeferredComposite[] = R"HLSL(
Texture2D<float4> World : register(t0);
Texture2D<float4> BaseTone : register(t1);
Texture2D<float4> UiTone : register(t2);
Texture2D<float> Transmission : register(t3);
RWTexture2D<float4> Out : register(u0);
cbuffer Params : register(b0) { float2 outSize; float2 jitterUV; };
[numthreads(8,8,1)] void main(uint3 p:SV_DispatchThreadID) {
    if(any(p.xy>=uint2(outSize)))return;
    float4 world=World.Load(int3(p.xy,0));
    float3 before=BaseTone.Load(int3(p.xy,0)).rgb;
    float3 after=UiTone.Load(int3(p.xy,0)).rgb;
    float t=Transmission.Load(int3(p.xy,0));
    // Exact passthrough outside coverage (including dither/quantization).
    Out[p.xy]=(t==1 && all(before==after))?world:float4(after+(world.rgb-before)*t,world.a);
}
)HLSL";
}
