// The scope lens's two compute passes (src/d3d11/scope_zoom.cpp; the pure parts and the buffer layout are scope_zoom_s1.h). Header-only HLSL text, no device: the build compiles it to
// fixed DXBC (tools/temporal_shader_build/fixed_scope_zoom_variants.h) and the rig runs those bytes.
#pragma once

#include <cstdint>

namespace edvr {

// The constants of the rays pass: five 16-byte rows.
struct ScopeZoomRaysParams {
    float cRen[4];     // c in render px (the draw's, jittered), then the render's size
    float cOut[4];     // c in frame px (unjittered), then the eye image's size in frame px
    float tanE[4];     // the tangents L, R, D, U the render's NDC square spans
    float win[4];      // the window of the picture the passes touch, frame px: x0, y0, x1, y1
    float knobs[4];    // the depth of kLensNearM, cos(kLensMaxDeg), the most rays that may escape, the step cap
};
static_assert(sizeof(ScopeZoomRaysParams) == 80, "the rays pass's constant buffer is five rows");
// The constants of the magnify pass: three 16-byte rows.
struct ScopeZoomMagnifyParams {
    float cM[4];       // c in frame px, M, the feather in output px
    float win[4];      // the window's origin in frame px, then its size
    float scale[4];    // output px per render px, x and y
};
static_assert(sizeof(ScopeZoomMagnifyParams) == 48, "the magnify pass's constant buffer is three rows");

// Pass 1: one group of 128 threads, one ray each. Z: the eye's depth at the render's size, the depth channel of the game's R32G8X24 texture (reverse-Z: larger is nearer). Lens: the
// raw buffer scope_zoom_s1.h lays out (the indirect arguments of pass 2 first). A ray marches from c in 1 px steps along its direction in render px; the frame is the first run of
// three px nearer than the depth of 0.6 m; the ray escapes at 12 degrees at the eye, off the image or at the step cap, and an escaped ray stores 12 degrees whichever way it
// ended (the sentinel the instrument's hole statistics skip). An escaped ray takes the smaller radius of its nearest rays
// that did not escape; the largest radius gives the box of pass 2, and more than two escaped rays (an open sight) make the groups of pass 2 zero.
constexpr char kScopeZoomRaysCsHlsl[] = R"HLSL(
cbuffer P : register(b0) {
    float4 cRen;
    float4 cOut;
    float4 tanE;
    float4 winB;
    float4 knobs;
};
Texture2D<float> Z : register(t0);
RWByteAddressBuffer Lens : register(u0);
groupshared float gR[128];
groupshared uint gEsc[128];
groupshared uint gEscCount;
groupshared uint gMaxR;
float2 tanAt(float2 p) {
    return float2(tanE.x + (tanE.y - tanE.x) * (p.x / cRen.z), tanE.w - (tanE.w - tanE.z) * (p.y / cRen.w));
}
float cosBetween(float2 a, float2 b) {
    return (1.0 + a.x * b.x + a.y * b.y) * rsqrt((1.0 + dot(a, a)) * (1.0 + dot(b, b)));
}
float degAt(float2 tc, float2 p) {
    return degrees(acos(clamp(cosBetween(tc, tanAt(p)), -1.0, 1.0)));
}
[numthreads(128, 1, 1)]
void main(uint3 tid : SV_GroupThreadID) {
    uint k = tid.x;
    if (k == 0) {
        gEscCount = 0;
        gMaxR = 0;
    }
    GroupMemoryBarrierWithGroupSync();
    float phi = 6.28318530718 * (float)k / 128.0;
    float sn, cs;
    sincos(phi, sn, cs);
    float2 dir = float2(cs, sn);
    float2 tc = tanAt(cRen.xy);
    int2 size = int2(cRen.zw);
    uint steps = (uint)knobs.w;
    uint run = 0;
    bool esc = true;
    float rPx = (float)steps;
    float rDeg = degAt(tc, cRen.xy + dir * (float)steps);
    [loop] for (uint i = 1; i <= steps; ++i) {
        float2 p = cRen.xy + dir * (float)i;
        int2 q = int2(floor(p));
        if (q.x < 0 || q.y < 0 || q.x >= size.x || q.y >= size.y) {
            rPx = (float)i;
            rDeg = degAt(tc, p);
            break;
        }
        if (cosBetween(tc, tanAt(p)) <= knobs.y) {
            rPx = (float)i;
            rDeg = 12.0;
            break;
        }
        run = Z.Load(int3(q, 0)) > knobs.x ? run + 1 : 0;
        if (run >= 3) {
            uint first = i - 2;
            esc = false;
            rPx = (float)first;
            rDeg = degAt(tc, cRen.xy + dir * (float)first);
            break;
        }
    }
    if (esc) rDeg = 12.0;   // a ray that left the image or took every step reads as the escape sentinel, whatever angle it reached
    gR[k] = rPx;
    gEsc[k] = esc ? 1u : 0u;
    if (esc) InterlockedAdd(gEscCount, 1u);
    GroupMemoryBarrierWithGroupSync();
    float use = rPx;
    if (esc) {
        float a = 1.0e30, b = 1.0e30;
        [loop] for (uint j = 1; j < 128; ++j) {
            uint kk = (k + j) & 127;
            if (gEsc[kk] == 0u) { a = gR[kk]; break; }
        }
        [loop] for (uint j2 = 1; j2 < 128; ++j2) {
            uint kk2 = (k + 128 - j2) & 127;
            if (gEsc[kk2] == 0u) { b = gR[kk2]; break; }
        }
        float m = min(a, b);
        if (m < 1.0e29) use = m;
    }
    Lens.Store(64 + 4 * k, asuint(use));
    Lens.Store(576 + 4 * k, asuint(rDeg));
    InterlockedMax(gMaxR, asuint(use));
    GroupMemoryBarrierWithGroupSync();
    if (k == 0) {
        uint escCount = gEscCount;
        float maxR = asfloat(gMaxR);
        int2 qc = clamp(int2(floor(cRen.xy)), int2(0, 0), size - 1);
        Lens.Store(28, escCount);
        Lens.Store(32, asuint(Z.Load(int3(qc, 0))));
        float2 scl = cOut.zw / cRen.zw;
        float hx = maxR * scl.x + 4.0, hy = maxR * scl.y + 4.0;
        int x0 = max((int)winB.x, (int)floor(cOut.x - hx));
        int y0 = max((int)winB.y, (int)floor(cOut.y - hy));
        int x1 = min((int)winB.z, (int)ceil(cOut.x + hx));
        int y1 = min((int)winB.w, (int)ceil(cOut.y + hy));
        bool open = (float)escCount > knobs.z || x1 <= x0 || y1 <= y0 || maxR <= 0.0;
        uint gx = open ? 0u : ((uint)(x1 - x0) + 31u) / 32u;
        uint gy = open ? 0u : ((uint)(y1 - y0) + 7u) / 8u;
        Lens.Store3(0, uint3(gx, gy, 1u));
        Lens.Store4(12, open ? uint4(0u, 0u, 0u, 0u) : uint4((uint)x0, (uint)y0, (uint)(x1 - x0), (uint)(y1 - y0)));
    }
}
)HLSL";

// Pass 2: dispatched indirectly over the box of the largest ray. Copy: a copy of the window of the picture (the one image the pass reads, so a pixel never reads one it has written).
// Lens: the buffer pass 1 filled. Frame: the picture as a typed UAV, written only inside the hole. A pixel at p, inside r(theta) of c, takes the Catmull-Rom sample of the copy at
// c + (p - c) / M, met with the copy's own pixel over the last two output px of the hole.
constexpr char kScopeZoomMagnifyCsHlsl[] = R"HLSL(
cbuffer P : register(b0) {
    float4 cM;
    float4 win;
    float4 scale;
};
Texture2D<float4> Copy : register(t0);
ByteAddressBuffer Lens : register(t1);
RWTexture2D<unorm float4> Frame : register(u0);
SamplerState Lin : register(s0);
float4 sampleCR(float2 pos, float2 texSize) {
    float2 tp1 = floor(pos - 0.5) + 0.5;
    float2 f = pos - tp1;
    float2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
    float2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    float2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
    float2 w3 = f * f * (-0.5 + 0.5 * f);
    float2 w12 = w1 + w2;
    float2 off12 = w2 / w12;
    float2 t0 = (tp1 - 1.0) / texSize;
    float2 t3 = (tp1 + 2.0) / texSize;
    float2 t12 = (tp1 + off12) / texSize;
    float4 r = Copy.SampleLevel(Lin, float2(t0.x, t0.y), 0) * (w0.x * w0.y);
    r += Copy.SampleLevel(Lin, float2(t12.x, t0.y), 0) * (w12.x * w0.y);
    r += Copy.SampleLevel(Lin, float2(t3.x, t0.y), 0) * (w3.x * w0.y);
    r += Copy.SampleLevel(Lin, float2(t0.x, t12.y), 0) * (w0.x * w12.y);
    r += Copy.SampleLevel(Lin, float2(t12.x, t12.y), 0) * (w12.x * w12.y);
    r += Copy.SampleLevel(Lin, float2(t3.x, t12.y), 0) * (w3.x * w12.y);
    r += Copy.SampleLevel(Lin, float2(t0.x, t3.y), 0) * (w0.x * w3.y);
    r += Copy.SampleLevel(Lin, float2(t12.x, t3.y), 0) * (w12.x * w3.y);
    r += Copy.SampleLevel(Lin, float2(t3.x, t3.y), 0) * (w3.x * w3.y);
    return r;
}
[numthreads(32, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    uint4 box = Lens.Load4(12);
    if (id.x >= box.z || id.y >= box.w) return;
    int2 px = int2(box.xy) + int2(id.xy);
    float2 d = float2(px) + 0.5 - cM.xy;
    float2 dRen = d / scale.xy;
    float phi = atan2(dRen.y, dRen.x);
    if (phi < 0.0) phi += 6.28318530718;
    float fk = phi * (128.0 / 6.28318530718);
    uint k0 = ((uint)floor(fk)) & 127u;
    uint k1 = (k0 + 1u) & 127u;
    float r = lerp(asfloat(Lens.Load(64 + 4 * k0)), asfloat(Lens.Load(64 + 4 * k1)), frac(fk));
    float inside = (r - length(dRen)) * 0.5 * (scale.x + scale.y);
    float wgt = saturate(inside / cM.w);
    if (wgt <= 0.0) return;
    float2 texSize;
    Copy.GetDimensions(texSize.x, texSize.y);
    float2 src = (cM.xy + d / cM.z) - win.xy;
    float4 mag = saturate(sampleCR(src, texSize));
    float4 orig = Copy.Load(int3(px - int2(win.xy), 0));
    Frame[uint2(px)] = lerp(orig, mag, wgt);
}
)HLSL";

}  // namespace edvr
