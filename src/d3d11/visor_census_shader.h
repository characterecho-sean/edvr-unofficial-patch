// F121b (measure only): the census of the HDR scene after the helmet visor's draw. 64 x 64 threads (8 x 8 groups of 8 x 8), each reading 4 x 4 taps of a copy of the eye's HDR
// target: the same procedure as edvr::visorcensus::cpuModel (visor_census_core.h), which the rig compares it with. Out (uints) as visorcensus::Word: the sequence, the
// width, the height, the taps (65536); the non-finite taps (by the float bits: exponent all ones), of them NaN and Inf; the black ones (finite, luma under 1e-4); the
// largest finite channel (float bits; a non-negative float orders as its bits); the sum of the threads' mean luma in 1/1024; the box of the non-finite taps (the minima
// as 0xFFFF - value); the first two texels of the exposure texture t1 where its view can be read (D.w = 1).
#pragma once

namespace edvr {

constexpr char kVisorCensusHlsl[] = R"HLSL(
Texture2D<float4> Hdr : register(t0);
Texture2D<float4> Expo : register(t1);
RWStructuredBuffer<uint> Out : register(u0);
cbuffer C : register(b0) {
    uint4 D;   // x = width, y = height, z = sequence number, w = 1 when Expo is a readable texture
};
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x == 0u && id.y == 0u) {
        Out[0] = D.z;
        Out[1] = D.x;
        Out[2] = D.y;
        Out[3] = 65536u;
        if (D.w != 0u) {
            Out[14] = asuint(Expo.Load(int3(0, 0, 0)).x);
            Out[15] = asuint(Expo.Load(int3(1, 0, 0)).x);
            Out[16] = 1u;
        }
    }
    uint nf = 0u, nn = 0u, ni = 0u, bk = 0u;
    float mx = 0.0;
    float lsum = 0.0;
    uint minXi = 0u, minYi = 0u, maxX = 0u, maxY = 0u;
    for (uint ty = 0u; ty < 4u; ++ty) {
        for (uint tx = 0u; tx < 4u; ++tx) {
            uint sx = id.x * 4u + tx;
            uint sy = id.y * 4u + ty;
            uint px = ((2u * sx + 1u) * D.x) / 512u;
            uint py = ((2u * sy + 1u) * D.y) / 512u;
            px = min(px, D.x - 1u);
            py = min(py, D.y - 1u);
            float3 c = Hdr.Load(int3((int)px, (int)py, 0)).rgb;
            uint3 b = asuint(c);
            bool3 big = (b & 0x7F800000u) == 0x7F800000u;
            if (any(big)) {
                ++nf;
                bool3 mant = (b & 0x007FFFFFu) != 0u;
                bool isNan = (big.x && mant.x) || (big.y && mant.y) || (big.z && mant.z);
                if (isNan) ++nn; else ++ni;
                minXi = max(minXi, 0xFFFFu - px);
                minYi = max(minYi, 0xFFFFu - py);
                maxX = max(maxX, px);
                maxY = max(maxY, py);
            } else {
                float lum = c.r * 0.2126 + c.g * 0.7152 + c.b * 0.0722;
                lsum += lum;
                float m = max(c.r, max(c.g, c.b));
                mx = max(mx, m);
                if (lum < 0.0001 && m < 0.0001) ++bk;
            }
        }
    }
    if (nf != 0u) {
        InterlockedAdd(Out[4], nf);
        InterlockedAdd(Out[5], nn);
        InterlockedAdd(Out[6], ni);
        InterlockedMax(Out[10], minXi);
        InterlockedMax(Out[11], minYi);
        InterlockedMax(Out[12], maxX);
        InterlockedMax(Out[13], maxY);
    }
    if (bk != 0u) InterlockedAdd(Out[7], bk);
    InterlockedMax(Out[8], asuint(mx));
    float tm = min(lsum / 16.0, 100.0);
    InterlockedAdd(Out[9], (uint)(tm * 1024.0));
}
)HLSL";

}  // namespace edvr
