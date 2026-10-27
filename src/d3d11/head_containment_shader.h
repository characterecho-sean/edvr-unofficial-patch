// F119 (native on-foot VR, measure only): how close world geometry gets to an eye. One thread per cell of an 8 x 8 grid over the eye's whole render image reads
// 8 x 8 taps of the eye's scene depth (and its stencil plane) and writes the nearest view depth of the cell, in metres: once over the taps the world drew (stencil
// bit 0x10 clear) and once over all of them. The same procedure as nofoot::f119::cellCpu (native_on_foot_f119.h), which the rigs compare it with.
//
// The depth is linearized as in reticle_depth_shader.h: w = B / (depth - A), the rows' own z column (A = 0, B = near for reversed-Z with an infinite far plane; the
// sky, depth 0, is infinitely far: 1e30). The stencil bit 0x10 is what the game's first-person draws write; the player's weapon and arms carry it, and so do the
// game's characters, the commander's own body among them (flat_mono_shader_source.h, measured 2026-10-08 in the eye depth stencil). Without a stencil view (F.x = 0)
// no tap is marked and the two minima are equal.
//
// One dispatch (8 x 8 threads) a call, one eye. Out (uints), from the base the constants name: base + 0 the sequence number, + 1 the depth texture's width, + 2 its
// height, + 3 the stencil mask in use (0: none); then cell k = 8 y + x at base + 4 + 3 k: the nearest world depth (float bits), the nearest depth over all taps
// (float bits), the number of taps carrying the mask (of 64).
#pragma once

namespace edvr {

constexpr unsigned kHeadGrid = 8;                               // cells along each axis
constexpr unsigned kHeadTaps = 8;                               // taps along each axis of a cell
constexpr unsigned kHeadCells = kHeadGrid * kHeadGrid;          // 64
constexpr unsigned kHeadEyeWords = 4u + 3u * kHeadCells;        // one eye's output
constexpr unsigned kHeadStencilMask = 0x10u;                    // the first-person bit
constexpr unsigned kHeadOutWords = 2u * kHeadEyeWords;          // both eyes in one buffer

constexpr char kHeadContainmentHlsl[] = R"HLSL(
Texture2D<float> Depth : register(t0);
Texture2D<uint2> Stencil : register(t1);
RWStructuredBuffer<uint> Out : register(u0);
cbuffer C : register(b0) {
    float4 P;   // x = A, y = B (depth = A + B / w)
    uint4  D;   // x = width, y = height, z = sequence number, w = the output base (uints)
    uint4  F;   // x = the stencil mask (0: no stencil view), y = cells along an axis, z = taps along a cell axis
};
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    uint g = F.y;
    uint t = F.z;
    if (id.x >= g || id.y >= g) return;
    uint cell = id.y * g + id.x;
    uint b = D.w + 4u + 3u * cell;
    if (cell == 0u) {
        Out[D.w + 0u] = D.z;
        Out[D.w + 1u] = D.x;
        Out[D.w + 2u] = D.y;
        Out[D.w + 3u] = F.x;
    }
    float minWorld = 1e30;
    float minAll = 1e30;
    uint marked = 0u;
    uint span = g * t;
    for (uint ty = 0u; ty < t; ++ty) {
        for (uint tx = 0u; tx < t; ++tx) {
            uint sx = id.x * t + tx;
            uint sy = id.y * t + ty;
            int px = (int)(((2u * sx + 1u) * D.x) / (2u * span));
            int py = (int)(((2u * sy + 1u) * D.y) / (2u * span));
            px = clamp(px, 0, (int)D.x - 1);
            py = clamp(py, 0, (int)D.y - 1);
            float dd = Depth.Load(int3(px, py, 0)) - P.x;
            float w = (abs(dd) > 1e-30) ? P.y / dd : 1e30;
            if (!(w > 0.0)) w = 1e30;
            bool fp = false;
            if (F.x != 0u) fp = (Stencil.Load(int3(px, py, 0)).y & F.x) != 0u;
            minAll = min(minAll, w);
            if (fp) ++marked;
            else minWorld = min(minWorld, w);
        }
    }
    Out[b + 0u] = asuint(minWorld);
    Out[b + 1u] = asuint(minAll);
    Out[b + 2u] = marked;
}
)HLSL";

}  // namespace edvr
