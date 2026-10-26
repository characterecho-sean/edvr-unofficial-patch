// F85c / F86d (native on-foot VR): depth lines. A thread marches a line O + d u through an eye's scene depth and writes where it meets a surface. The same
// procedure as nofoot::f85::marchCpu (native_on_foot_f85.h) and nofoot::f86::marchRangeCpu (native_on_foot_f86.h), which the rigs compare it with.
//
// A line is projected through the eye's scene rows (b1 270..273 in row-vector form, clip = p.x R0 + p.y R1 + p.z R2 + R3 for p = point - the rows'
// origin) and compared with the buffered surface in view depth: the depth value is linearized as w = B / (depth - A), the rows' own z column
// (A = 0, B = near for reversed-Z with an infinite far plane; the sky, depth 0, is infinitely far). 32 geometric steps over d in [dmin, dmax]; the first step
// behind the surface brackets the hit and ten bisection steps refine it. A first step already behind a surface is a surface nearer than dmin: status 2.
//
// Two contracts, selected by Dim.w:
//  * Dim.w = 0 (F85c, the reticle alone; the 128-byte constants): one line from b0 (Orel, Dir, Prm.zw = dmin, dmax). Output (4 uints): the distance as float
//    bits, the status (0 sky, 1 hit, 2 near, 3 off screen), the sequence number the constants carried, 0. Thread 0 only; b1 is not read.
//  * Dim.w = n, 1..32 (F86): n lines, one thread each. Line 0 is b0's (key 10, the reticle); line k >= 1 is b1: Ln[2 (k - 1)] = (Orel.xyz, dmin) and
//    Ln[2 (k - 1) + 1] = (Dir.xyz, dmax). A line with dmax <= dmin is unused (status 4). Output: uint 0 = the sequence number, uint 1 = n, then line k's 8
//    values at 8 + 8 k: status, the distance (float bits), the hit pixel x and y (float bits), the view depths of the four neighbours of the hit pixel
//    (x - 1, x + 1, y - 1, y + 1; float bits, 1e30 past the texture or at the far plane).
#pragma once

namespace edvr {

constexpr unsigned kDepthLinesMax = 32;   // lines a dispatch; the output holds (1 + kDepthLinesMax) * 8 uints

constexpr char kReticleDepthHlsl[] = R"HLSL(
Texture2D<float> Depth : register(t0);
RWStructuredBuffer<uint> Out : register(u0);
cbuffer C : register(b0) {
    float4 R0;
    float4 R1;
    float4 R2;
    float4 R3;
    float4 Orel;   // line 0's origin less the rows' origin (xyz)
    float4 Dir;    // line 0's direction (xyz, unit)
    float4 Prm;    // x = A, y = B (depth = A + B / w), z = line 0's d min, w = line 0's d max
    uint4  Dim;    // x = width, y = height, z = sequence number, w = lines (0: the single-line contract)
};
cbuffer L : register(b1) {
    float4 Ln[64];   // line k >= 1: Ln[2 (k - 1)] = (Orel.xyz, d min), Ln[2 (k - 1) + 1] = (Dir.xyz, d max)
};
float wAt(int2 p) {
    int2 q = int2(clamp(p.x, 0, (int)Dim.x - 1), clamp(p.y, 0, (int)Dim.y - 1));
    float dd = Depth.Load(int3(q.x, q.y, 0)) - Prm.x;
    float w = (abs(dd) > 1e-30) ? Prm.y / dd : 1e30;
    return w > 0.0 ? w : 1e30;
}
bool sampleAt(float3 orel, float3 dir, float d, out float wp, out float ws, out int2 pix) {
    wp = 0.0;
    ws = 1e30;
    pix = int2(0, 0);
    float3 p = orel + d * dir;
    float4 c = p.x * R0 + p.y * R1 + p.z * R2 + R3;
    if (!(c.w > 1e-6)) return false;
    float2 ndc = c.xy / c.w;
    float2 px = float2((ndc.x * 0.5 + 0.5) * (float)Dim.x, (0.5 - ndc.y * 0.5) * (float)Dim.y);
    if (!(px.x >= 0.0 && px.y >= 0.0 && px.x < (float)Dim.x && px.y < (float)Dim.y)) return false;
    pix = int2((int)px.x, (int)px.y);
    float dv = Depth.Load(int3(pix.x, pix.y, 0));
    float dd = dv - Prm.x;
    float w = (abs(dd) > 1e-30) ? Prm.y / dd : 1e30;
    wp = c.w;
    ws = w > 0.0 ? w : 1e30;
    return true;
}
void march(float3 orel, float3 dir, float dmin, float dmax, out float result, out uint status, out int2 hitPix, out bool havePix) {
    float ratio = pow(dmax / dmin, 1.0 / 31.0);
    float prev = dmin;
    bool off = false;
    bool done = false;
    status = 0;
    result = dmax;
    hitPix = int2(0, 0);
    havePix = false;
    [loop]
    for (int i = 0; i < 32 && !done; ++i) {
        float d = dmin * pow(ratio, (float)i);
        float wp;
        float ws;
        int2 pix;
        if (!sampleAt(orel, dir, d, wp, ws, pix)) {
            off = true;
            prev = d;
            continue;
        }
        if (wp >= ws) {
            if (i == 0) {
                status = 2;
                result = dmin;
            } else {
                float lo = prev;
                float hi = d;
                [loop]
                for (int b = 0; b < 10; ++b) {
                    float mid = 0.5 * (lo + hi);
                    float wm;
                    float sm;
                    int2 pm;
                    if (sampleAt(orel, dir, mid, wm, sm, pm) && wm >= sm) hi = mid;
                    else lo = mid;
                }
                result = 0.5 * (lo + hi);
                status = 1;
                float wr;
                float sr;
                int2 pr;
                if (sampleAt(orel, dir, result, wr, sr, pr)) {
                    hitPix = pr;
                    havePix = true;
                }
            }
            done = true;
        } else {
            prev = d;
        }
    }
    if (!done && off) status = 3;
}
[numthreads(32, 1, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    uint n = Dim.w;
    if (n == 0u) {
        if (id.x != 0u) return;
        float result;
        uint status;
        int2 hp;
        bool hv;
        march(Orel.xyz, Dir.xyz, Prm.z, Prm.w, result, status, hp, hv);
        Out[0] = asuint(result);
        Out[1] = status;
        Out[2] = Dim.z;
        Out[3] = 0;
        return;
    }
    if (id.x >= n) return;
    float3 orel = Orel.xyz;
    float3 dir = Dir.xyz;
    float dmin = Prm.z;
    float dmax = Prm.w;
    if (id.x != 0u) {
        uint k = 2u * (id.x - 1u);
        orel = Ln[k].xyz;
        dmin = Ln[k].w;
        dir = Ln[k + 1u].xyz;
        dmax = Ln[k + 1u].w;
    }
    uint b = 8u + id.x * 8u;
    if (id.x == 0u) {
        Out[0] = Dim.z;
        Out[1] = n;
    }
    if (!(dmax > dmin) || !(dmin > 0.0)) {
        Out[b + 0] = 4;
        Out[b + 1] = asuint(0.0);
        Out[b + 2] = asuint(0.0);
        Out[b + 3] = asuint(0.0);
        Out[b + 4] = asuint(1e30);
        Out[b + 5] = asuint(1e30);
        Out[b + 6] = asuint(1e30);
        Out[b + 7] = asuint(1e30);
        return;
    }
    float result;
    uint status;
    int2 hp;
    bool hv;
    march(orel, dir, dmin, dmax, result, status, hp, hv);
    Out[b + 0] = status;
    Out[b + 1] = asuint(result);
    Out[b + 2] = asuint(hv ? (float)hp.x : 0.0);
    Out[b + 3] = asuint(hv ? (float)hp.y : 0.0);
    Out[b + 4] = asuint(hv ? wAt(hp + int2(-1, 0)) : 1e30);
    Out[b + 5] = asuint(hv ? wAt(hp + int2(1, 0)) : 1e30);
    Out[b + 6] = asuint(hv ? wAt(hp + int2(0, -1)) : 1e30);
    Out[b + 7] = asuint(hv ? wAt(hp + int2(0, 1)) : 1e30);
}
)HLSL";

}  // namespace edvr
