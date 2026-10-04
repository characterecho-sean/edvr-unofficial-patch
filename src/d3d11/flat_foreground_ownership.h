#pragma once
#include <cstdint>

namespace edvr {

// Readback ABI shared by the runtime and its WARP rig. Every count is over
// pixels inside the render extent, not over padded dispatch threads.
struct FlatForegroundOwnershipCounts {
    uint32_t total = 0;
    uint32_t covered = 0;
    uint32_t survivingExactDepth = 0;
    uint32_t survivingMarked16 = 0;
    uint32_t survivingUnmarked = 0;
    uint32_t overwritten = 0;
    uint32_t markedWithoutSurvivingCoverage = 0;
    uint32_t totalStencil16 = 0;
};
static_assert(sizeof(FlatForegroundOwnershipCounts) == 8 * sizeof(uint32_t),
              "foreground ownership readback ABI changed");
inline constexpr uint32_t kFlatForegroundOwnershipCounterCount = 8;
inline constexpr uint32_t kFlatForegroundOwnershipGroupWidth = 8;

// t0: the persistent R8 union of passing original pixel shader fragments.
// t1: the depth owned by the last foreground draw at each covered pixel,
// merged from a depth copy made immediately after that draw. t2/t3: the depth
// and stencil planes of the same depth resource at the HDR consumer. The
// consumer must supply both planes, or report a refusal without dispatch.
// b0: uint2 extent, uint2 padding. u0: eight zeroed uints in the ABI above.
// Exact depth bits avoid silently admitting a nearby but different surface.
// A marked pixel without surviving coverage includes both uncovered pixels
// and foreground coverage subsequently overwritten in depth.
inline constexpr char kFlatForegroundOwnershipCs[] = R"EDVR(
Texture2D<float> Coverage : register(t0);
Texture2D<float> OwnerDepth : register(t1);
Texture2D<float> ConsumerDepth : register(t2);
Texture2D<uint2> ConsumerStencil : register(t3);
RWStructuredBuffer<uint> Counters : register(u0);
cbuffer Extent : register(b0) { uint2 Size; uint2 Padding; };
groupshared uint Votes[64 * 8];

[numthreads(8,8,1)]
void main(uint3 pixel : SV_DispatchThreadID, uint3 local : SV_GroupThreadID) {
    uint lane = local.y * 8 + local.x;
    uint vote[8] = {0,0,0,0,0,0,0,0};
    if (pixel.x < Size.x && pixel.y < Size.y) {
        int3 p = int3(pixel.xy, 0);
        uint covered = Coverage.Load(p) > 0.0f ? 1u : 0u;
        uint exact = asuint(OwnerDepth.Load(p)) == asuint(ConsumerDepth.Load(p)) ? 1u : 0u;
        uint marked = (ConsumerStencil.Load(p).y & 16u) != 0u ? 1u : 0u;
        uint surviving = covered & exact;
        vote[0] = 1u;
        vote[1] = covered;
        vote[2] = surviving;
        vote[3] = surviving & marked;
        vote[4] = surviving & (1u - marked);
        vote[5] = covered & (1u - exact);
        vote[6] = marked & (1u - surviving);
        vote[7] = marked;
    }
    [unroll] for (uint i = 0; i < 8; ++i) Votes[lane * 8 + i] = vote[i];
    [unroll] for (uint stride = 32; stride > 0; stride >>= 1) {
        GroupMemoryBarrierWithGroupSync();
        if (lane < stride)
            [unroll] for (uint i = 0; i < 8; ++i)
                Votes[lane * 8 + i] += Votes[(lane + stride) * 8 + i];
    }
    GroupMemoryBarrierWithGroupSync();
    if (lane == 0)
        [unroll] for (uint bin = 0; bin < 8; ++bin)
            if (Votes[bin] != 0) InterlockedAdd(Counters[bin], Votes[bin]);
}
)EDVR";

// Merge one completed foreground draw. The current mask has been cleared
// before that original draw, and ImmediateDepth is copied immediately after
// it. A world write between foreground draws cannot change an earlier
// covered pixel's saved depth. Later foreground coverage replaces that
// pixel's owner depth; uncovered pixels retain their previous owner.
// b0 and group geometry match the counter shader. The host must unbind the
// output UAVs before using them as the counter shader's t0/t1 inputs.
inline constexpr char kFlatForegroundMergeCs[] = R"EDVR(
Texture2D<float> CurrentMask : register(t0);
Texture2D<float> ImmediateDepth : register(t1);
RWTexture2D<float> CoverageUnion : register(u0);
RWTexture2D<float> OwnerDepth : register(u1);
cbuffer Extent : register(b0) { uint2 Size; uint2 Padding; };

[numthreads(8,8,1)]
void main(uint3 pixel : SV_DispatchThreadID) {
    if (pixel.x >= Size.x || pixel.y >= Size.y) return;
    int3 p = int3(pixel.xy, 0);
    if (CurrentMask.Load(p) > 0.0f) {
        CoverageUnion[pixel.xy] = 1.0f;
        OwnerDepth[pixel.xy] = ImmediateDepth.Load(p);
    }
}
)EDVR";

} // namespace edvr
