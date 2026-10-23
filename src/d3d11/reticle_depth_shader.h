// F85c (native on-foot VR): the reticle's hit depth. One thread marches the aim line O + d u through an eye's scene depth and writes where it meets a
// surface. The same procedure as nofoot::f85::marchCpu (native_on_foot_f85.h), which the rig compares it with.
//
// The aim line is projected through the eye's scene rows (b1 270..273 in row-vector form, clip = p.x R0 + p.y R1 + p.z R2 + R3 for p = point - the rows'
// origin) and compared with the buffered surface in view depth: the depth value is linearized as w = B / (depth - A), the rows' own z column
// (A = 0, B = near for reversed-Z with an infinite far plane; the sky, depth 0, is infinitely far). 32 geometric steps over d in [0.5, 50] m; the first step
// behind the surface brackets the hit and ten bisection steps refine it. A first step already behind a surface is a surface nearer than 0.5 m: status 2, the
// caller holds. Output (4 uints): the distance as float bits, the status (0 sky, 1 hit, 2 near, 3 off screen), the sequence number the constants carried, 0.
#pragma once

namespace edvr {

constexpr char kReticleDepthHlsl[] = R"HLSL(
Texture2D<float> Depth : register(t0);
RWStructuredBuffer<uint> Out : register(u0);
cbuffer C : register(b0) {
    float4 R0;
    float4 R1;
    float4 R2;
    float4 R3;
    float4 Orel;   // the aim line's origin less the rows' origin (xyz)
    float4 Dir;    // the aim line's direction (xyz, unit)
    float4 Prm;    // x = A, y = B (depth = A + B / w), z = d min, w = d max
    uint4  Dim;    // x = width, y = height, z = sequence number
};
bool sampleAt(float d, out float wp, out float ws) {
    wp = 0.0;
    ws = 1e30;
    float3 p = Orel.xyz + d * Dir.xyz;
    float4 c = p.x * R0 + p.y * R1 + p.z * R2 + R3;
    if (!(c.w > 1e-6)) return false;
    float2 ndc = c.xy / c.w;
    float2 px = float2((ndc.x * 0.5 + 0.5) * (float)Dim.x, (0.5 - ndc.y * 0.5) * (float)Dim.y);
    if (!(px.x >= 0.0 && px.y >= 0.0 && px.x < (float)Dim.x && px.y < (float)Dim.y)) return false;
    float dv = Depth.Load(int3((int)px.x, (int)px.y, 0));
    float dd = dv - Prm.x;
    float w = (abs(dd) > 1e-30) ? Prm.y / dd : 1e30;
    wp = c.w;
    ws = w > 0.0 ? w : 1e30;
    return true;
}
[numthreads(1, 1, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    float dmin = Prm.z;
    float dmax = Prm.w;
    float ratio = pow(dmax / dmin, 1.0 / 31.0);
    float prev = dmin;
    bool off = false;
    bool done = false;
    uint status = 0;
    float result = dmax;
    [loop]
    for (int i = 0; i < 32 && !done; ++i) {
        float d = dmin * pow(ratio, (float)i);
        float wp;
        float ws;
        if (!sampleAt(d, wp, ws)) {
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
                    if (sampleAt(mid, wm, sm) && wm >= sm) hi = mid;
                    else lo = mid;
                }
                result = 0.5 * (lo + hi);
                status = 1;
            }
            done = true;
        } else {
            prev = d;
        }
    }
    if (!done && off) status = 3;
    Out[0] = asuint(result);
    Out[1] = status;
    Out[2] = Dim.z;
    Out[3] = 0;
}
)HLSL";

}  // namespace edvr
