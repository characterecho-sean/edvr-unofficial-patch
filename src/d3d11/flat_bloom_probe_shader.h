#pragma once
// TEMPORARY hangar bloom measurement. Two reductions of one 2D texture: the whole frame and, in a
// second dispatch, the central half. Remove with the arc.
namespace edvr {
inline constexpr char kFlatBloomProbeReduceCs[] = R"HLSL(
Texture2D<float4> Src : register(t0);
cbuffer Limits : register(b0) {
    uint2 size;
    uint2 origin;
    uint2 extent;
    uint groupsX;
    uint groupCount;
    uint pad0;
    uint pad1;
    uint pad2;
    uint pad3;
};
struct Bin {
    uint n;
    float sum;
    float maxv;
    uint c0, c1, c2, c3, c4, c5, c6, c7;
    uint pad;
};
RWStructuredBuffer<Bin> GroupBins : register(u0);
groupshared uint gn, gc0, gc1, gc2, gc3, gc4, gc5, gc6, gc7;
groupshared float gsum[256];
groupshared float gmax[256];
float luma(float3 c) { return dot(c, float3(0.2126, 0.7152, 0.0722)); }
[numthreads(16, 16, 1)]
void reduceMain(uint3 id : SV_DispatchThreadID, uint3 gid : SV_GroupID, uint gi : SV_GroupIndex) {
    if (gi == 0) { gn = gc0 = gc1 = gc2 = gc3 = gc4 = gc5 = gc6 = gc7 = 0; }
    gsum[gi] = 0;
    gmax[gi] = 0;
    GroupMemoryBarrierWithGroupSync();
    if (id.x < extent.x && id.y < extent.y) {
        uint x = origin.x + id.x;
        uint y = origin.y + id.y;
        float3 c = max(Src.Load(int3(x, y, 0)).rgb, 0);
        float m = max(c.r, max(c.g, c.b));
        InterlockedAdd(gn, 1);
        gsum[gi] = luma(c);
        gmax[gi] = m;
        if (m >= 0.85) InterlockedAdd(gc0, 1);
        if (m >= 0.95) InterlockedAdd(gc1, 1);
        if (m >= 0.99) InterlockedAdd(gc2, 1);
        if (m >= 0.999) InterlockedAdd(gc3, 1);
        if (m >= 1) InterlockedAdd(gc4, 1);
        if (m >= 4) InterlockedAdd(gc5, 1);
        if (m >= 16) InterlockedAdd(gc6, 1);
        if (m >= 64) InterlockedAdd(gc7, 1);
    }
    GroupMemoryBarrierWithGroupSync();
    if (gi == 0) {
        float sum = 0;
        float mx = 0;
        [unroll] for (uint i = 0; i < 256; ++i) { sum += gsum[i]; mx = max(mx, gmax[i]); }
        Bin o;
        o.n = gn; o.sum = sum; o.maxv = mx;
        o.c0 = gc0; o.c1 = gc1; o.c2 = gc2; o.c3 = gc3;
        o.c4 = gc4; o.c5 = gc5; o.c6 = gc6; o.c7 = gc7;
        o.pad = 0;
        GroupBins[gid.y * groupsX + gid.x] = o;
    }
}
)HLSL";

inline constexpr char kFlatBloomProbeFinishCs[] = R"HLSL(
cbuffer Limits : register(b0) {
    uint2 size;
    uint2 origin;
    uint2 extent;
    uint groupsX;
    uint groupCount;
    uint pad0;
    uint pad1;
    uint pad2;
    uint pad3;
};
struct Bin {
    uint n;
    float sum;
    float maxv;
    uint c0, c1, c2, c3, c4, c5, c6, c7;
    uint pad;
};
StructuredBuffer<Bin> InBins : register(t0);
RWStructuredBuffer<Bin> OutBin : register(u0);
groupshared uint pn[256];
groupshared float psum[256];
groupshared float pmax[256];
groupshared uint p0[256], p1[256], p2[256], p3[256], p4[256], p5[256], p6[256], p7[256];
[numthreads(256, 1, 1)]
void finishMain(uint3 id : SV_DispatchThreadID) {
    uint n = 0, c0 = 0, c1 = 0, c2 = 0, c3 = 0, c4 = 0, c5 = 0, c6 = 0, c7 = 0;
    float sum = 0, mx = 0;
    for (uint g = id.x; g < groupCount; g += 256) {
        Bin b = InBins[g];
        n += b.n; sum += b.sum; mx = max(mx, b.maxv);
        c0 += b.c0; c1 += b.c1; c2 += b.c2; c3 += b.c3;
        c4 += b.c4; c5 += b.c5; c6 += b.c6; c7 += b.c7;
    }
    pn[id.x] = n; psum[id.x] = sum; pmax[id.x] = mx;
    p0[id.x] = c0; p1[id.x] = c1; p2[id.x] = c2; p3[id.x] = c3;
    p4[id.x] = c4; p5[id.x] = c5; p6[id.x] = c6; p7[id.x] = c7;
    GroupMemoryBarrierWithGroupSync();
    if (id.x == 0) {
        Bin t;
        t.n = 0; t.sum = 0; t.maxv = 0; t.pad = 0;
        t.c0 = t.c1 = t.c2 = t.c3 = t.c4 = t.c5 = t.c6 = t.c7 = 0;
        if (groupCount != 0) {
            [unroll] for (uint k = 0; k < 256; ++k) {
                t.n += pn[k]; t.sum += psum[k]; t.maxv = max(t.maxv, pmax[k]);
                t.c0 += p0[k]; t.c1 += p1[k]; t.c2 += p2[k]; t.c3 += p3[k];
                t.c4 += p4[k]; t.c5 += p5[k]; t.c6 += p6[k]; t.c7 += p7[k];
            }
        }
        OutBin[0] = t;
    }
}
)HLSL";
}
