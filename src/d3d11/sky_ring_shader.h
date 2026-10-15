#pragma once
// The sky ring's compute shader (I-2 of docs\sky-flicker-2026-10-13.md; TEMPORARY, diag.sky_ring). The layout it writes, the grid and the fixed-point
// scales are src\common\sky_ring.h's, which holds the CPU reference tools\sky_flicker_test runs this against on WARP.
//
// One group of 1024 threads, 16 samples each over a 128x128 grid of the eye's input pixels. A sample is sky when the depth copy says far (Elite's
// reversed-Z value not above the projection's A: the temporal shader's own test). For each sky sample: the raw colour's luma (input, before the
// upscaler), the upscaler output's luma over the block of output pixels covering that input pixel (mean and max), and the motion vector's length
// in pixels. Sums are fixed point and added to group-shared words AFTER the loop (a branch that `continue`s around an atomic is the fxc trap
// skin_join_gpu_test pins), maxima are the float bits of a non-negative value, which order as unsigned words. Thread 0 writes the 32-byte entry.
//
// Bindings: t0 raw colour (R8G8B8A8), t1 upscaler output (R8G8B8A8), t2 motion vectors (R16G16F), t3 depth copy (R32F), u0 the ring (raw), b0 below.
#include "../common/sky_ring.h"

namespace edvr {

inline constexpr char kSkyRingCs[] = R"HLSL(
cbuffer P : register(b0) {
    uint  slotByte;     // the entry's byte offset in the ring
    uint  frame;
    uint  eyeFlags;     // bit 0 the eye, bit 1 the frame has a depth copy
    uint  seq;          // 1-based dispatch number
    uint2 inSize;       // the input textures' size
    uint2 outSize;      // the upscaler output's size
    float projA;        // the scene projection's A: depth = A + B / z
    uint  outSide;      // the output block's side in pixels, 1 or 2
    uint2 pad;
};
Texture2D<float4> Raw   : register(t0);
Texture2D<float4> Out   : register(t1);
Texture2D<float2> Mv    : register(t2);
Texture2D<float>  Depth : register(t3);
RWByteAddressBuffer Ring : register(u0);

groupshared uint gSky;
groupshared uint gBright;
groupshared uint gRawSum;
groupshared uint gOutSum;
groupshared uint gMvSum;
groupshared uint gRawMax;
groupshared uint gOutMax;
groupshared uint gMvMax;

float Luma(float3 c) { return dot(c, float3(0.2126, 0.7152, 0.0722)); }

[numthreads(1024, 1, 1)]
void main(uint tid : SV_GroupIndex) {
    if (tid == 0) {
        gSky = 0; gBright = 0; gRawSum = 0; gOutSum = 0; gMvSum = 0; gRawMax = 0; gOutMax = 0; gMvMax = 0;
    }
    GroupMemoryBarrierWithGroupSync();

    const bool depthValid = (eyeFlags & 2u) != 0u;
    uint sky = 0, bright = 0, rawSum = 0, outSum = 0, mvSum = 0;
    float rawMax = 0.0, outMax = 0.0, mvMax = 0.0;
    [loop] for (uint k = 0; k < 16; ++k) {
        const uint s = tid + 1024u * k;
        const uint gx = s & 127u, gy = s >> 7;
        const uint x = ((2u * gx + 1u) * inSize.x) / 256u;
        const uint y = ((2u * gy + 1u) * inSize.y) / 256u;
        const float d = Depth.Load(int3(x, y, 0));
        const bool isSky = depthValid && (d <= 0.0 || d - projA <= 0.0);
        if (isSky) {
            const float raw = saturate(Luma(Raw.Load(int3(x, y, 0)).rgb));
            const uint ox = (x * outSize.x) / inSize.x;
            const uint oy = (y * outSize.y) / inSize.y;
            float osum = 0.0, omax = 0.0;
            [unroll] for (uint j = 0; j < 4; ++j) {
                const uint px = j & 1u, py = j >> 1;
                if (px < outSide && py < outSide) {
                    const float l = saturate(Luma(Out.Load(int3(min(ox + px, outSize.x - 1u), min(oy + py, outSize.y - 1u), 0)).rgb));
                    osum += l;
                    omax = max(omax, l);
                }
            }
            const float omean = osum / float(outSide * outSide);
            const float2 v = Mv.Load(int3(x, y, 0));
            float mvLen = length(v);
            mvLen = (mvLen < 60000.0) ? mvLen : 60000.0;   // also catches NaN
            sky += 1u;
            rawSum += uint(raw * 4096.0 + 0.5);
            outSum += uint(omean * 4096.0 + 0.5);
            mvSum += uint(mvLen * 16.0 + 0.5);
            rawMax = max(rawMax, raw);
            outMax = max(outMax, omax);
            mvMax = max(mvMax, mvLen);
            bright += (omax > 0.05) ? 1u : 0u;
        }
    }
    uint tmp;
    InterlockedAdd(gSky, sky, tmp);
    InterlockedAdd(gBright, bright, tmp);
    InterlockedAdd(gRawSum, rawSum, tmp);
    InterlockedAdd(gOutSum, outSum, tmp);
    InterlockedAdd(gMvSum, mvSum, tmp);
    InterlockedMax(gRawMax, asuint(rawMax), tmp);
    InterlockedMax(gOutMax, asuint(outMax), tmp);
    InterlockedMax(gMvMax, asuint(mvMax), tmp);
    GroupMemoryBarrierWithGroupSync();

    if (tid == 0) {
        const float n = max(float(gSky), 1.0);
        const float rawMean = float(gRawSum) / (4096.0 * n);
        const float outMean = float(gOutSum) / (4096.0 * n);
        const float mvMean  = float(gMvSum) / (16.0 * n);
        const uint dw1 = (gSky & 0xFFFFu) | ((gBright & 0xFFFFu) << 16);
        const uint dw2 = f32tof16(rawMean) | (f32tof16(asfloat(gRawMax)) << 16);
        const uint dw3 = f32tof16(outMean) | (f32tof16(asfloat(gOutMax)) << 16);
        const uint dw4 = f32tof16(mvMean)  | (f32tof16(asfloat(gMvMax)) << 16);
        Ring.Store4(slotByte, uint4(frame, dw1, dw2, dw3));
        Ring.Store4(slotByte + 16u, uint4(dw4, 0u, eyeFlags & 3u, seq));
    }
}
)HLSL";

// The constant buffer the shader reads, 16-byte rows (64 bytes). Fields in the shader's order.
struct SkyRingParams {
    uint32_t slotByte, frame, eyeFlags, seq;
    uint32_t inW, inH, outW, outH;
    float projA;
    uint32_t outSide;
    uint32_t pad[2];
};
static_assert(sizeof(SkyRingParams) == 48, "the shader's cbuffer is three 16-byte rows");

}  // namespace edvr
