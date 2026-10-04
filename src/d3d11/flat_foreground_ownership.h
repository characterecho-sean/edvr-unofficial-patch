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

// t0: the private MRT7 R8 coverage from the original pixel shader's passing
// fragments. t1: the depth copied after the early cohort. t2/t3: the depth
// and stencil planes of the same depth resource at the HDR consumer. The
// consumer must supply both planes, or report a refusal without dispatch.
// b0: uint2 extent, uint2 padding. u0: eight zeroed uints in the ABI above.
// Exact depth bits avoid silently admitting a nearby but different surface.
// A marked pixel without surviving coverage includes both uncovered pixels
// and early coverage subsequently overwritten in depth.
inline constexpr char kFlatForegroundOwnershipCs[] = R"EDVR(
Texture2D<float> Coverage : register(t0);
Texture2D<float> CohortDepth : register(t1);
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
        uint exact = asuint(CohortDepth.Load(p)) == asuint(ConsumerDepth.Load(p)) ? 1u : 0u;
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

} // namespace edvr
