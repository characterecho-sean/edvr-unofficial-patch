// The flat HDR route's luminance census pass (flat_hdr_luma.h; temporary instrument for the 2026-10-09 DLAA dimming
// entry in docs/design-flat-temporal-aa-2026-09-23.md). Compiled at build time by tools\temporal_shader_build into
// kFlatHdrLumaBytecode; the DLL carries no HLSL compiler.
//
// One dispatch of 32x32 groups of 16x16 threads strides over the whole scene image. The raw output buffer, in 32-bit
// words (flat_hdr_luma.h names the layout and decodes it):
//   [0..63]   a log2 histogram of luma, two bins a stop from 2^-16 (bin 0 also takes luma 0)
//   [64]      local maxima above thresholdA            ("stars", the absolute threshold)
//   [65]      local maxima above thresholdB            ("stars scaled": thresholdA times the last out/in mean ratio)
//   [66]      local maxima above twice their 8 neighbours' mean and above peakFloor ("peaks": scale-free)
//   [67]      the largest finite luma, as float bits (non-negative floats order as unsigned)
//   [68]      non-finite or negative pixels, skipped
//   [72..]    one float luma sum per group, 1024 of them, summed on the CPU
// A star is strictly brighter than all 8 neighbours; the image border is never a star.
#pragma once

namespace edvr {

constexpr char kFlatHdrLumaCsHlsl[] = R"HLSL(
Texture2D<float4> Src : register(t0);
RWByteAddressBuffer Out : register(u0);
cbuffer Params : register(b0) { uint2 size; float thresholdA; float thresholdB; float peakFloor; float3 pad; };
groupshared float gsSum[256];
groupshared uint gsHist[64];
float lumaAt(uint2 p) {
    float3 c = Src.Load(int3(p, 0)).rgb;
    float l = dot(c, float3(0.2126, 0.7152, 0.0722));
    return (l >= 0.0 && l < 65000.0) ? l : 0.0;
}
[numthreads(16, 16, 1)]
void main(uint3 gid : SV_GroupID, uint3 gtid : SV_GroupThreadID, uint gi : SV_GroupIndex) {
    if (gi < 64) gsHist[gi] = 0;
    GroupMemoryBarrierWithGroupSync();
    float sum = 0.0, mx = 0.0;
    uint starsA = 0, starsB = 0, peaks = 0, bad = 0;
    for (uint y = gid.y * 16 + gtid.y; y < size.y; y += 512) {
        for (uint x = gid.x * 16 + gtid.x; x < size.x; x += 512) {
            float3 c = Src.Load(int3(x, y, 0)).rgb;
            float l = dot(c, float3(0.2126, 0.7152, 0.0722));
            if (!(l >= 0.0 && l < 65000.0)) { ++bad; continue; }
            sum += l; mx = max(mx, l);
            int bin = l > 0.0 ? clamp(int(floor((log2(l) + 16.0) * 2.0)), 0, 63) : 0;
            InterlockedAdd(gsHist[bin], 1u);
            if (x > 0 && y > 0 && x + 1 < size.x && y + 1 < size.y && l > peakFloor) {
                float n0 = lumaAt(uint2(x - 1, y - 1)), n1 = lumaAt(uint2(x, y - 1)), n2 = lumaAt(uint2(x + 1, y - 1));
                float n3 = lumaAt(uint2(x - 1, y)),                                   n4 = lumaAt(uint2(x + 1, y));
                float n5 = lumaAt(uint2(x - 1, y + 1)), n6 = lumaAt(uint2(x, y + 1)), n7 = lumaAt(uint2(x + 1, y + 1));
                float nmax = max(max(max(n0, n1), max(n2, n3)), max(max(n4, n5), max(n6, n7)));
                if (l > nmax) {
                    if (l > thresholdA) ++starsA;
                    if (l > thresholdB) ++starsB;
                    float nmean = (n0 + n1 + n2 + n3 + n4 + n5 + n6 + n7) * 0.125;
                    if (l > 2.0 * nmean) ++peaks;
                }
            }
        }
    }
    gsSum[gi] = sum;
    if (starsA) Out.InterlockedAdd(64 * 4, starsA);
    if (starsB) Out.InterlockedAdd(65 * 4, starsB);
    if (peaks) Out.InterlockedAdd(66 * 4, peaks);
    if (mx > 0.0) Out.InterlockedMax(67 * 4, asuint(mx));
    if (bad) Out.InterlockedAdd(68 * 4, bad);
    GroupMemoryBarrierWithGroupSync();
    for (uint stride = 128; stride > 0; stride >>= 1) {
        if (gi < stride) gsSum[gi] += gsSum[gi + stride];
        GroupMemoryBarrierWithGroupSync();
    }
    if (gi < 64 && gsHist[gi]) Out.InterlockedAdd(gi * 4, gsHist[gi]);
    if (gi == 0) Out.Store((72 + gid.y * 32 + gid.x) * 4, asuint(gsSum[0]));
}
)HLSL";

}  // namespace edvr
