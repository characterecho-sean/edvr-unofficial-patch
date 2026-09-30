#pragma once

// Fixed extra HLSL: verbatim source extracted from runtime callers.

#include <string>

#include "fsr_hlsl_gen.h"

#include "particle_vs.h"

#include "flare_vs.h"

#include "sunglare_vs.h"

namespace edvr { namespace fixed_extra_source {

inline std::string joinChunks(const char* const* chunks) { std::string s; for (; *chunks; ++chunks) s += *chunks; return s; }

namespace backdrop_fix {

constexpr char kBackdropCsHlsl[] = R"HLSL(
Texture2D<float4>   S : register(t0);
RWTexture2D<float4> O : register(u0);
cbuffer P : register(b0) {
    float4 p;   // x = radius in texels, y = flatness threshold, z = dither
};

float mx3(float3 v) { return max(max(v.x, v.y), v.z); }

// Interleaved gradient noise: a pure function of position. The bake is
// therefore deterministic, and since ONE texture feeds both eyes the dither
// cannot differ between them -- the failure mode the FSS arc spent forty
// rounds on, absent here by construction rather than by care.
float ign(float2 q) {
    return frac(52.9829189 * frac(dot(q, float2(0.06711056, 0.00583715))));
}

[numthreads(16, 16, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    uint w, h;
    O.GetDimensions(w, h);
    if (id.x >= w || id.y >= h) return;

    int2 c0 = int2(id.xy);
    int  r  = int(p.x);
    int2 lo = int2(0, 0);
    int2 hi = int2(int(w) - 1, int(h) - 1);

    float4 c  = S[c0];
    float3 n0 = S[clamp(c0 + int2( r,  0), lo, hi)].rgb;
    float3 n1 = S[clamp(c0 + int2(-r,  0), lo, hi)].rgb;
    float3 n2 = S[clamp(c0 + int2( 0,  r), lo, hi)].rgb;
    float3 n3 = S[clamp(c0 + int2( 0, -r), lo, hi)].rgb;

    // The threshold is the whole difference between a deband and a blur. A
    // star, a hull edge, any real structure exceeds it and passes through
    // untouched; only a neighbourhood already flat to within a quantization
    // step or two is averaged -- which is exactly where the step between two
    // block endpoints shows as a contour.
    float d = max(max(mx3(abs(n0 - c.rgb)), mx3(abs(n1 - c.rgb))),
                  max(mx3(abs(n2 - c.rgb)), mx3(abs(n3 - c.rgb))));
    // A SOFT weight, not a hard switch. "d < threshold ? average : centre"
    // makes adjacent pixels land on opposite sides of a cliff, and five
    // chained passes bake each cliff in and re-average it -- which the first
    // field run saw as blotches that are not in the source. Fading the
    // average out as the neighbourhood stops being flat has no boundary to
    // see, and at d = 0 it is still the full average.
    float flat = saturate(1.0 - d / max(p.y, 1e-6));
    float3 o = lerp(c.rgb, (n0 + n1 + n2 + n3) * 0.25, flat);

    // Dither on the final pass only: about one LSB, enough to break the last
    // residual contour and far below what reads as noise.
    if (p.z > 0.0) o += (ign(float2(c0)) - 0.5) * p.z;

    O[id.xy] = float4(saturate(o), c.a);
}
)HLSL";

}

namespace sharpen_pass {

const char kSharpenMain[] =
    "Texture2D<float4> Src : register(t0);\n"
    "RWTexture2D<float4> Dst : register(u0);\n"
    "cbuffer P : register(b0) { uint4 con; int4 region; int2 outSize; int2 pad0; };\n"
    "AF4 FsrRcasLoadF(ASU2 p) {\n"
    "    int2 q = clamp(int2(p), region.xy, region.zw - 1);\n"
    "    return Src.Load(int3(q, 0));\n"
    "}\n"
    "void FsrRcasInputF(inout AF1 r, inout AF1 g, inout AF1 b) {}\n"
    "[numthreads(8,8,1)]\n"
    "void main(uint3 id : SV_DispatchThreadID) {\n"
    "    if (id.x >= (uint)outSize.x || id.y >= (uint)outSize.y) return;\n"
    "    int2 ip = int2(id.xy) + region.xy;\n"
    "    AF3 c;\n"
    "    FsrRcasF(c.r, c.g, c.b, AU2(ip), con);\n"
    "    Dst[id.xy] = float4(c, Src.Load(int3(ip, 0)).a);\n"
    "}\n";

const char kGpuPrologue[] =
    "#define A_GPU 1\n"
    "#define A_HLSL 1\n";

const std::string kRcasSource = std::string(kGpuPrologue) + joinChunks(kFfxAChunks) + "#define FSR_RCAS_F 1\n" + joinChunks(kFfxFsr1Chunks) + kSharpenMain;

}

namespace menu_panel {

constexpr char kCompositeCs[] = R"HLSL(
Texture2D<float4> S : register(t0);
Texture2D<float4> P : register(t1);
SamplerState L : register(s0);
RWTexture2D<float4> O : register(u0);
cbuffer C : register(b0) {
    int4   region;     // the pixels of S this eye owns (x1, y1 exclusive)
    int2   outSize;
    int    flipV;      // the submit's rows run bottom-up
    int    linearOut;  // the frame is linear light: linearise the panel
    int4   box;        // the output pixels this dispatch covers (x1, y1 exclusive)
    float4 tans;       // left, right, top, bottom tangent magnitudes
    float4 m0;         // current-head -> anchor rotation rows; .w = origin
    float4 m1;
    float4 m2;
    float4 geom;       // dist, curve, halfW, halfH
    float4 misc;       // alpha
};
float3 toLinear(float3 c) {
    return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);
}
[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
    uint2 id = uint2(box.x + tid.x, box.y + tid.y);
    if (id.x >= (uint)box.z || id.y >= (uint)box.w) return;
    float4 src = S.Load(int3(region.x + id.x, region.y + id.y, 0));
    float u = (id.x + 0.5) / outSize.x;
    float v = (id.y + 0.5) / outSize.y;
    if (misc.z > 0.5) u = 1.0 - u;
    if (flipV) v = 1.0 - v;
    float tx = lerp(-tans.x, tans.y, u);
    float ty = lerp(tans.z, -tans.w, v);
    float3 dv = float3(tx, ty, -1.0);
    float3 df = float3(dot(m0.xyz, dv), dot(m1.xyz, dv), dot(m2.xyz, dv));
    float3 org = float3(m0.w, m1.w, m2.w);
    float dist = geom.x, curve = geom.y, halfW = geom.z, halfH = geom.w;
    float su = -1.0, sv = -1.0;
    if (curve > 0.005) {
        float R = dist / curve;
        float zc = R - dist;
        float a = df.x * df.x + df.z * df.z;
        float b = 2.0 * (org.x * df.x + (org.z - zc) * df.z);
        float c = org.x * org.x + (org.z - zc) * (org.z - zc) - R * R;
        float disc = b * b - 4.0 * a * c;
        if (disc > 0 && a > 1e-8) {
            float t = (-b + sqrt(disc)) / (2.0 * a);
            if (t > 0) {
                float3 hit = org + t * df;
                float th = atan2(hit.x, zc - hit.z);
                su = (th * R - misc.y + halfW) / (2.0 * halfW);
                sv = (hit.y + halfH) / (2.0 * halfH);
            }
        }
    } else if (df.z < -1e-4) {
        float t = (-dist - org.z) / df.z;
        if (t > 0) {
            float3 hit = org + t * df;
            su = (hit.x - misc.y + halfW) / (2.0 * halfW);
            sv = (hit.y + halfH) / (2.0 * halfH);
        }
    }
    float4 outc = src;
    if (su >= 0 && su <= 1 && sv >= 0 && sv <= 1) {
        float4 p = P.SampleLevel(L, float2(su, 1.0 - sv), 0);
        if (linearOut) {
            float pa = max(p.a, 1e-4);
            p.rgb = toLinear(p.rgb / pa) * pa;
        }
        float a = p.a * misc.x;
        outc = float4(src.rgb * (1.0 - a) + p.rgb * misc.x, src.a);
    }
    O[id.xy] = outc;
}
)HLSL";

}

namespace intro_upscale {

const char kDebandHlsl[] =
    "Texture2D<float4> S : register(t0);\n"
    "RWTexture2D<float4> O : register(u0);\n"
    "cbuffer P : register(b0) { float4 p; };\n"
    "float mx3(float3 v) { return max(max(v.x, v.y), v.z); }\n"
    "float ign(float2 q) {\n"
    "    return frac(52.9829189 * frac(dot(q, float2(0.06711056, 0.00583715))));\n"
    "}\n"
    "[numthreads(8,8,1)]\n"
    "void main(uint3 id : SV_DispatchThreadID) {\n"
    "    uint w, h;\n"
    "    O.GetDimensions(w, h);\n"
    "    if (id.x >= w || id.y >= h) return;\n"
    "    int2 c0 = int2(id.xy);\n"
    "    int r = int(p.x);\n"
    "    int2 lo = int2(0, 0);\n"
    "    int2 hi = int2(int(w) - 1, int(h) - 1);\n"
    "    float4 c = S[c0];\n"
    "    float3 n0 = S[clamp(c0 + int2( r, 0), lo, hi)].rgb;\n"
    "    float3 n1 = S[clamp(c0 + int2(-r, 0), lo, hi)].rgb;\n"
    "    float3 n2 = S[clamp(c0 + int2( 0, r), lo, hi)].rgb;\n"
    "    float3 n3 = S[clamp(c0 + int2( 0,-r), lo, hi)].rgb;\n"
    "    float d = max(max(mx3(abs(n0 - c.rgb)), mx3(abs(n1 - c.rgb))),\n"
    "                  max(mx3(abs(n2 - c.rgb)), mx3(abs(n3 - c.rgb))));\n"
    "    float flatness = saturate(1.0 - d / max(p.y, 1e-6));\n"
    "    float3 o = lerp(c.rgb, (n0 + n1 + n2 + n3) * 0.25, flatness);\n"
    "    if (p.z > 0.0) o += (ign(float2(c0)) - 0.5) * p.z;\n"
    "    O[id.xy] = float4(saturate(o), c.a);\n"
    "}\n";

const char kEasuMain[] =
    "Texture2D<float4> Src : register(t0);\n"
    "SamplerState Smp : register(s0);\n"
    "RWTexture2D<float4> Dst : register(u0);\n"
    "cbuffer P : register(b0) {\n"
    "    uint4 con0; uint4 con1; uint4 con2; uint4 con3; uint2 dstSize;\n"
    "};\n"
    "AF4 FsrEasuRF(AF2 p) { return Src.GatherRed(Smp, p); }\n"
    "AF4 FsrEasuGF(AF2 p) { return Src.GatherGreen(Smp, p); }\n"
    "AF4 FsrEasuBF(AF2 p) { return Src.GatherBlue(Smp, p); }\n"
    "[numthreads(8,8,1)]\n"
    "void main(uint3 id : SV_DispatchThreadID) {\n"
    "    if (id.x >= dstSize.x || id.y >= dstSize.y) return;\n"
    "    AF3 c;\n"
    "    FsrEasuF(c, id.xy, con0, con1, con2, con3);\n"
    "    Dst[id.xy] = float4(c, 1.0);\n"
    "}\n";

const char kRcasMain[] =
    "Texture2D<float4> Src : register(t0);\n"
    "RWTexture2D<float4> Dst : register(u0);\n"
    "cbuffer P : register(b0) { uint4 con; uint2 dstSize; };\n"
    "AF4 FsrRcasLoadF(ASU2 p) { return Src.Load(int3(p, 0)); }\n"
    "void FsrRcasInputF(inout AF1 r, inout AF1 g, inout AF1 b) {}\n"
    "[numthreads(8,8,1)]\n"
    "void main(uint3 id : SV_DispatchThreadID) {\n"
    "    if (id.x >= dstSize.x || id.y >= dstSize.y) return;\n"
    "    AF3 c;\n"
    "    FsrRcasF(c.r, c.g, c.b, id.xy, con);\n"
    "    Dst[id.xy] = float4(c, 1.0);\n"
    "}\n";

const char kGpuPrologue[] =
    "#define A_GPU 1\n"
    "#define A_HLSL 1\n";

const char kCubicHlsl[] =
    "Texture2D<float4> Src : register(t0);\n"
    "RWTexture2D<float4> Dst : register(u0);\n"
    "cbuffer P : register(b0) { uint2 srcSize; uint2 dstSize; };\n"
    "float w(float t) {\n"
    "    t = abs(t);\n"
    "    if (t <= 1.0) return 1.5*t*t*t - 2.5*t*t + 1.0;\n"
    "    if (t <  2.0) return -0.5*t*t*t + 2.5*t*t - 4.0*t + 2.0;\n"
    "    return 0.0;\n"
    "}\n"
    "[numthreads(8,8,1)]\n"
    "void main(uint3 id : SV_DispatchThreadID) {\n"
    "    if (id.x >= dstSize.x || id.y >= dstSize.y) return;\n"
    "    float2 sp = (float2(id.xy) + 0.5) *\n"
    "                (float2(srcSize) / float2(dstSize)) - 0.5;\n"
    "    int2 b = int2(floor(sp));\n"
    "    float2 f = sp - float2(b);\n"
    "    float wx[4], wy[4];\n"
    "    [unroll] for (int k = 0; k < 4; ++k) {\n"
    "        wx[k] = w(float(k - 1) - f.x);\n"
    "        wy[k] = w(float(k - 1) - f.y);\n"
    "    }\n"
    "    float4 acc = 0.0;\n"
    "    float sum = 0.0;\n"
    "    [unroll] for (int j = 0; j < 4; ++j) {\n"
    "        [unroll] for (int i = 0; i < 4; ++i) {\n"
    "            int2 q = clamp(b + int2(i - 1, j - 1), int2(0, 0),\n"
    "                           int2(srcSize) - 1);\n"
    "            float cw = wx[i] * wy[j];\n"
    "            acc += Src.Load(int3(q, 0)) * cw;\n"
    "            sum += cw;\n"
    "        }\n"
    "    }\n"
    "    if (sum > 0.0) acc /= sum;\n"
    "    Dst[id.xy] = float4(saturate(acc.rgb), acc.a);\n"
    "}\n";

const std::string kEasuSource = std::string(kGpuPrologue) + joinChunks(kFfxAChunks) + "#define FSR_EASU_F 1\n" + joinChunks(kFfxFsr1Chunks) + kEasuMain;

const std::string kRcasSource = std::string(kGpuPrologue) + joinChunks(kFfxAChunks) + "#define FSR_RCAS_F 1\n" + joinChunks(kFfxFsr1Chunks) + kRcasMain;

}

namespace splash_dim {

constexpr char kPsHlsl[] =
    "float4 main() : SV_Target { return float4(0.0, 0.0, 0.0, 0.4); }";

}

namespace fss_heal {

constexpr char kHealCsHlsl[] = R"HLSL(
Texture2D<float4> L : register(t0);
Texture2D<float4> R : register(t1);
RWTexture2D<float4> O : register(u0);
cbuffer P : register(b0) {
    float4 p;   // p.x = dx pixels; p.yz = screen AABB min (u,v);
    float4 q;   // p.w,q.x = AABB max -- packed: yz=min, w+q.x=max
}
// Wireframe blue: the blue channel meaningfully ahead of red. Floorless,
// so dim antialiased edges are caught; red-relative, so neutral and warm
// content (ring, body, white bloom) is not.
bool isWire(float4 c) { return c.b > c.r * 1.35 + 0.02; }
[numthreads(16, 16, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    uint w, h;
    O.GetDimensions(w, h);
    if (id.x >= w || id.y >= h) return;
    float4 l = L[id.xy];
    float4 o = l;
    // Round 48e, the SPATIAL scope: the fill exists only inside the
    // scanner screen's rectangle -- the squares live on the body inside
    // it, the near-field neon frame lives around it, and filling beside
    // the neon at the infinity disparity painted offset twins.
    float u = (id.x + 0.5) / w;
    float v = (id.y + 0.5) / h;
    bool inScreen = u >= p.y && u <= p.w && v >= p.z && v <= q.x;
    if (!inScreen) {
        O[id.xy] = o;
        return;
    }
    // Round 48b, the WINDOW-ERA classifier: one test. The heal now exists
    // only inside the zoom-press arrival window -- body at optical
    // infinity, virtually no UI -- so the v5 gate stack (interior,
    // square-scale, right-lit, bright-region) that protected menus in
    // its always-on life is pure fill-suppression here: it left the
    // squares over DIM ring content black and speckled tile boundaries.
    // A hard-black left pixel takes the right's pixel at the infinity
    // shift, whatever it is: filling space-black with space-black is a
    // no-op, and bright chrome is never hard-black.
    if (dot(l.rgb, float3(0.299, 0.587, 0.114)) < 0.004) {
        int rx = int(id.x) - int(round(p.x));
        uint rw, rh;
        R.GetDimensions(rw, rh);
        if (rx >= 0 && rx < int(rw)) {
            float4 rp = R[uint2(uint(rx), id.y)];
            // The neon wireframe lives in PLAYER space, not at the
            // body's optical infinity -- its right-eye pixels are the
            // wrong disparity for this shift, and stamping them paints
            // offset twins of the blue lines (the field's report, three
            // times now). During the zoom TRANSIT the source region is
            // void plus wireframe and nothing else, so every visible
            // fill in those ~3 s is contamination by definition. Round
            // 49's veto (b > 0.10 and b > 1.6r) let two tails through:
            // dim antialiased bar edges under the 0.10 floor, and
            // bloom-brightened cores whose lifted red defeats the
            // ratio. The test is now floorless and red-relative, and a
            // core that blooms to near-white is caught by its GLOW: the
            // four axis neighbours at 4 px are tested too -- a bar is
            // thinner than 8 px, so some neighbour is always still
            // blue. A vetoed source keeps the left's black -- the stock
            // look, never a new artifact. (Known cost: strongly
            // blue-dominant body content can keep its squares;
            // preferred over ever painting the wireframe.)
            bool wire = isWire(rp) ||
                        isWire(R[uint2(min(uint(rx) + 4u, rw - 1u), id.y)]) ||
                        isWire(R[uint2(uint(max(rx - 4, 0)), id.y)]) ||
                        isWire(R[uint2(uint(rx), min(id.y + 4u, rh - 1u))]) ||
                        isWire(R[uint2(uint(rx), uint(max(int(id.y) - 4, 0)))]);
            if (!wire) o = rp;
        }
    }
    O[id.xy] = o;
}
)HLSL";

constexpr char kMirrorCsHlsl[] = R"HLSL(
Texture2D<float4> L : register(t0);
Texture2D<float4> R : register(t1);
RWTexture2D<float4> O : register(u0);
cbuffer P : register(b0) { float4 p; }   // p.x = dx pixels (left minus right)
[numthreads(16, 16, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    uint w, h;
    O.GetDimensions(w, h);
    if (id.x >= w || id.y >= h) return;
    float4 r = R[id.xy];
    float4 o = r;
    float rl = dot(r.rgb, float3(0.299, 0.587, 0.114));
    if (rl > 0.02) {
        uint lw, lh;
        L.GetDimensions(lw, lh);
        int lx = int(id.x) + int(round(p.x));
        if (lx >= 0 && lx < int(lw)) {
            const int dys[3] = {0, -16, 16};
            [unroll] for (int i = 0; i < 3; ++i) {
                int ly = int(id.y) + dys[i];
                if (ly < 0 || ly >= int(lh)) continue;
                if (dot(L[uint2(uint(lx), uint(ly))].rgb,
                        float3(0.299, 0.587, 0.114)) >= 0.004) continue;
                // interior of a black region in the left...
                bool blk = true;
                int2 c = int2(lx, ly);
                int2 offs[4] = {int2(-2, 0), int2(2, 0), int2(0, -2),
                                int2(0, 2)};
                [unroll] for (int k = 0; k < 4; ++k) {
                    int2 q = c + offs[k];
                    if (q.x < 0 || q.y < 0 || q.x >= int(lw) ||
                        q.y >= int(lh)) continue;
                    if (dot(L[uint2(q)].rgb,
                            float3(0.299, 0.587, 0.114)) >= 0.004) {
                        blk = false;
                        break;
                    }
                }
                if (!blk) continue;
                // ...at square scale, not panel background or space
                bool farLit = false;
                int2 far4[4] = {int2(-10, 0), int2(10, 0), int2(0, -10),
                                int2(0, 10)};
                [unroll] for (int k2 = 0; k2 < 4; ++k2) {
                    int2 q2 = c + far4[k2];
                    if (q2.x < 0 || q2.y < 0 || q2.x >= int(lw) ||
                        q2.y >= int(lh)) continue;
                    if (dot(L[uint2(q2)].rgb,
                            float3(0.299, 0.587, 0.114)) >= 0.004) {
                        farLit = true;
                        break;
                    }
                }
                if (farLit) o = float4(0, 0, 0, r.a);
                break;
            }
        }
    }
    O[id.xy] = o;
}
)HLSL";

}

namespace fss_dump {

constexpr char kSeriesCsHlsl[] = R"HLSL(
Texture2D<float4> src : register(t0);
RWTexture2D<float> outt : register(u0);
cbuffer P : register(b0) { uint4 off; }   // x = yOffset, y = tw, z = th
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= off.y || id.y >= off.z) return;
    float s = 0;
    for (uint j = 0; j < 16; ++j)
        for (uint i = 0; i < 16; ++i) {
            float4 c = src.Load(int3(id.x * 16 + i, id.y * 16 + j, 0));
            s += dot(c.rgb, float3(0.299, 0.587, 0.114));
        }
    outt[uint2(id.x, off.x + id.y)] = s / 256.0;
}
)HLSL";

}

namespace depth_probe {

constexpr char kSampleCsHlsl[] = R"HLSL(
Texture2D<float> D : register(t0);
RWStructuredBuffer<float> O : register(u0);
cbuffer P : register(b0) { uint2 size; uint2 pad0; };
[numthreads(16, 16, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= 16 || id.y >= 16) return;
    uint2 p = uint2((id.x * 2 + 1) * size.x / 32, (id.y * 2 + 1) * size.y / 32);
    p = min(p, size - 1);
    O[id.y * 16 + id.x] = D.Load(int3(int2(p), 0));
}
)HLSL";

}

} }
