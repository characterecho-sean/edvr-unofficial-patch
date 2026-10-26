#pragma once

namespace edvr {
// Capture-only point sampler for the native-on-foot jump trace. The packed
// outputs preserve the source half-float words and the slot target's float
// words without a lossy CPU conversion.
constexpr char kEyeJumpLatticeHlsl[] = R"HLSL(
Texture2D<float4> SkinE : register(t0);
Texture2D<float2> Motion : register(t1);
Texture2D<float2> Slots : register(t2);
RWTexture2D<uint2> PackedSkinE : register(u0);
RWTexture2D<uint> PackedMotion : register(u1);
RWTexture2D<uint2> PackedSlots : register(u2);
[numthreads(8,8,1)]
void main(uint3 id : SV_DispatchThreadID) {
    uint w,h; SkinE.GetDimensions(w,h);
    uint2 outSize=uint2((w+1)/2,(h+1)/2);
    if(id.x>=outSize.x || id.y>=outSize.y)return;
    uint2 p=min(id.xy*2,uint2(w-1,h-1));
    float4 e=SkinE.Load(int3(p,0));
    float2 mv=Motion.Load(int3(p,0));
    float2 slot=Slots.Load(int3(p,0));
    uint ex=f32tof16(e.x),ey=f32tof16(e.y),ez=f32tof16(e.z),ew=f32tof16(e.w);
    uint mx=f32tof16(mv.x),my=f32tof16(mv.y);
    PackedSkinE[id.xy]=uint2(ex|(ey<<16),ez|(ew<<16));
    PackedMotion[id.xy]=mx|(my<<16);
    PackedSlots[id.xy]=asuint(slot);
}
)HLSL";
}
