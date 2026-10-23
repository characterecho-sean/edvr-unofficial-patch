#pragma once
namespace edvr {
// Passive, event-only depth witness. One 32x18 group scores the same fixed
// current-frame texels under both raw camera-translation hypotheses. Nothing
// written here is read by the renderer.
inline constexpr char kFlatOriginOpticalCsHlsl[] = R"HLSL(
cbuffer Probe : register(b0) {
    float4 now[6], old[6];
    float4 rowPhase;                 // current NDC xy, previous NDC zw
    uint4 size;                      // render xy, sample grid zw (32,18)
    float4 rasterPhase;             // current xy, previous zw, in pixels
    float4 depthScale;              // previous canonical depth / raw world depth in x
    uint4 sources;                  // slots, untrusted, foreground, stencil
};
cbuffer EngineNow : register(b1) { float4 EN[277]; };
cbuffer EngineBefore : register(b2) { float4 EB[277]; };
Texture2D<float> CurrentDepth : register(t0);
Texture2D<float> PreviousDepth : register(t1);
Texture2D<float2> Slots : register(t2);
Texture2D<float> Untrusted : register(t3);
Texture2D<float4> Foreground : register(t4);
Texture2D<uint2> Stencil : register(t5);
RWByteAddressBuffer Result : register(u0);
groupshared uint counters[32];

float4 unphase(float4 row,float2 phase) { row.xy-=phase*row.w; return row; }
// Identical inverse and projection convention to cameraBefore in the shipped
// flat prep, but with an explicit camera-origin delta for each hypothesis.
uint project(float2 uv,float depth,float3 originDelta,out float2 prior,out float expected) {
    prior=0; expected=0;
    float4 n0=unphase(now[0],rowPhase.xy), n1=unphase(now[1],rowPhase.xy);
    float4 n2=unphase(now[2],rowPhase.xy), n3=unphase(now[3],rowPhase.xy);
    float4 p0=unphase(old[0],rowPhase.zw), p1=unphase(old[1],rowPhase.zw);
    float4 p2=unphase(old[2],rowPhase.zw), p3=unphase(old[3],rowPhase.zw);
    if (!(n3.z>0) || !(depth>0)) return 2;
    float3 a=float3(n0.x,n1.x,n2.x), b=float3(n0.y,n1.y,n2.y), c=float3(n0.w,n1.w,n2.w);
    float3 ca=cross(b,c), cb=cross(c,a), cc=cross(a,b);
    float det=dot(a,ca), iz=depth/n3.z;
    if (!(abs(det)>1e-12) || !isfinite(det)) return 2;
    float3 rhs=float3(uv*float2(2,-2)+float2(-1,1),1)-n3.xyw*iz;
    float3 position=(ca*rhs.x+cb*rhs.y+cc*rhs.z)/det+originDelta*iz;
    float4 before=position.x*p0+position.y*p1+position.z*p2+iz*p3;
    if (!(before.w>0) || !all(isfinite(before))) return 2;
    prior=before.xy/before.w*float2(.5,-.5)+.5;
    expected=before.z/before.w*depthScale.x;
    if (!all(isfinite(prior)) || !isfinite(expected) || !(expected>0)) return 2;
    if (any(prior<0) || any(prior>1)) return 1;
    return 0;
}
float residual(float2 prior,float expected) {
    float2 raster=prior*float2(size.xy)+rasterPhase.zw-.5;
    int2 base=int2(floor(raster));
    float best=3.402823e38;
    [unroll] for(int y=0;y<2;++y) [unroll] for(int x=0;x<2;++x) {
        int2 q=clamp(base+int2(x,y),int2(0,0),int2(size.xy)-1);
        float d=PreviousDepth.Load(int3(q,0));
        if (isfinite(d) && d>0) best=min(best,abs(d-expected));
    }
    return best/max(1e-6,expected*.01);
}
void bump(uint i) { InterlockedAdd(counters[i],1); }
[numthreads(32,18,1)]
void main(uint3 thread:SV_GroupThreadID) {
    uint lane=thread.y*32+thread.x;
    if(lane<32)counters[lane]=0;
    GroupMemoryBarrierWithGroupSync();
    // The center of each stratified cell, with no random seed or event bias.
    int2 q=int2((thread.xy*size.xy+size.xy/2)/size.zw);
    q=clamp(q,int2(0,0),int2(size.xy)-1);
    bump(0);
    float depth=CurrentDepth.Load(int3(q,0));
    if (!isfinite(depth) || !(depth>0) || depth>1) bump(1);
    else if(sources.y!=0 && Untrusted.Load(int3(q,0))>0) bump(3);
    else if(sources.z!=0 && Foreground.Load(int3(q,0)).w!=0) bump(2);
    else if(sources.w!=0 && (Stencil.Load(int3(q,0)).y&16)!=0) bump(4);
    else if(sources.x!=0 && (Slots.Load(int3(q,0)).x>=1 || Slots.Load(int3(q,0)).x< -1)) bump(5);
    else {
        float2 uv=(float2(q)+.5-rasterPhase.xy)/float2(size.xy);
        float2 trUv,zeUv; float trZ,zeZ;
        uint tr=project(uv,depth,now[5].xyz-old[5].xyz,trUv,trZ);
        uint ze=project(uv,depth,float3(0,0,0),zeUv,zeZ);
        if(tr==0)bump(8); else if(tr==1)bump(24); else bump(26);
        if(ze==0)bump(9); else if(ze==1)bump(25); else bump(27);
        if(tr==0 && ze==0) {
            bump(7);
            float trErr=residual(trUv,trZ), zeErr=residual(zeUv,zeZ);
            bool trFit=trErr<=1, zeFit=zeErr<=1;
            if(trFit)bump(10);
            if(zeFit)bump(11);
            if(trFit && zeFit)bump(12);
            else if(trFit)bump(13);
            else if(zeFit)bump(14);
            else bump(15);
            uint trBin=trErr<=.25?0:trErr<=1?1:trErr<=4?2:3;
            uint zeBin=zeErr<=.25?0:zeErr<=1?1:zeErr<=4?2:3;
            bump(16+trBin);bump(20+zeBin);
            if(trErr>=3.402823e37 || zeErr>=3.402823e37)bump(29);
        }
    }
    GroupMemoryBarrierWithGroupSync();
    if(lane<32)Result.Store(lane*4,counters[lane]);
    if(lane==0) {
        // Fixed byte offsets keep the compiler from dropping intermediate
        // stores when it optimizes a loop of ByteAddressBuffer.Store calls.
        Result.Store(128,asuint(EN[275].x));
        Result.Store(132,asuint(EN[275].y));
        Result.Store(136,asuint(EN[275].z));
        Result.Store(140,asuint(EB[275].x));
        Result.Store(144,asuint(EB[275].y));
        Result.Store(148,asuint(EB[275].z));
        Result.Store(152,asuint(EN[276].x));
        Result.Store(156,asuint(EB[276].x));
    }
}
)HLSL";
}
