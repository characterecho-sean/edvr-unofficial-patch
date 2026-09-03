#include "temporal_pass.h"

#include <cmath>
#include <cstdio>
#include <cstring>

#include <windows.h>

#include <d3d11.h>

#include "../common/config.h"
#include "../common/guard.h"
#include "../common/log.h"
#include "../common/supersample_math.h"   // supersampleRegionFromBounds: one region rule at the door
#include "../common/temporal_math.h"
#include "depth_probe.h"
#include "dlaa.h"
#include "shader_swap.h"

namespace edvr {
namespace {

// The pass, one compute shader. The reprojection is temporalReproject in
// src/common/temporal_math.h transcribed line for line; the C++ is the
// reference the test pins and this is its GPU twin. Desk-compiled by
// tools/compile_variants.py --target=cs_5_0 before it ships.
//
// Two things happen in gamma space on purpose. The blend: bright hairlines
// in linear light would dominate their neighbours' average, and a
// perceptual space is where every shipped TAA does its accumulation. And
// the history: stored as the output is, so a frame with no history to
// blend is bit-for-bit the game's own.
constexpr char kTemporalCsHlsl[] = R"HLSL(
Texture2D<float4> S : register(t0);      // this frame, the game's own texture (or the region copied out of it)
Texture2D<float4> H : register(t1);      // the history, region-sized, on the unjittered grid
Texture2D<float> Z : register(t2);       // the scene's depth, the game's own, when the pass has it
SamplerState L : register(s0);           // bilinear, clamp
RWTexture2D<float4> O : register(u0);    // the output, region-sized, the game's format
RWTexture2D<float4> N : register(u1);    // the new history
RWStructuredBuffer<uint> Stats : register(u2);   // 0 rejected, 1 clipped, 2 the clips' size (luma/255, summed); then the same three per candidate, four of them
RWTexture2D<float2> MV : register(u3);   // for a trained pass: motion vectors, pixels, current -> previous
RWTexture2D<float>  ZC : register(u4);   // and the depth, copied as it is
cbuffer P : register(b0) {
    int4   region;      // x0 y0 x1 y1: this eye's pixels in S (x1, y1 exclusive)
    int2   size;        // the region's size = the output's
    int2   texSize;     // S's size, for the sampler's uv
    float4 tanNow;      // l r t b this frame, jitter excluded
    float4 tanPrev;     // l r t b for the frame the history holds
    float4 jit;         // xy this frame's jitter in pixels; z 1 = filter the current sample; w the history kernel's C
    float4 dR0;         // rows of the rotation taking this frame's view
    float4 dR1;         // directions to last frame's (xyz; w unused)
    float4 dR2;
    float4 c0R0;        // the registration instrument's candidates, same
    float4 c0R1;        // shape: 0 the head as used, 1 the head one frame
    float4 c0R2;        // earlier, 2 the camera rows as world->view, 3 the
    float4 c1R0;        // game-pose array's head
    float4 c1R1;
    float4 c1R2;
    float4 c2R0;
    float4 c2R1;
    float4 c2R2;
    float4 c3R0;
    float4 c3R1;
    float4 c3R2;
    float  blend;       // history weight
    float  gamma;       // clip half-width, in standard deviations
    int    haveHistory; // 0: nothing to blend, this frame goes out as it is
    int    candMask;    // bit c: candidate c has a delta this frame
    float4 knobs;       // x the rest snap in pixels (0 off); y 1 = depth bound; z near, w far
    float4 tvUsed;      // xyz the translation term for the used delta (depth motion), w unused
    float4 tvCand;      // xyz the same for the instrument's swapped-eyes candidate
};
float3 rgbToYcocg(float3 c) {
    return float3(0.25 * c.r + 0.5 * c.g + 0.25 * c.b,
                  0.5 * c.r - 0.5 * c.b,
                  -0.25 * c.r + 0.5 * c.g - 0.25 * c.b);
}
float3 ycocgToRgb(float3 y) {
    return float3(y.x + y.y - y.z, y.x + y.z, y.x - y.y - y.z);
}
// Pull q into the box along the ray from the box's centre: the standard
// AABB clip, which keeps the history's hue while bounding its distance.
float3 clipToBox(float3 mn, float3 mx, float3 q) {
    float3 c = 0.5 * (mx + mn);
    float3 e = 0.5 * (mx - mn) + 1e-5;
    float3 v = q - c;
    float3 a = abs(v / e);
    float ma = max(a.x, max(a.y, a.z));
    return ma > 1.0 ? c + v / ma : q;
}
// The history's resampling kernel through nine bilinear fetches: a
// bicubic of the B = 0 family with C from the parameters -- C = 0.5 is
// Catmull-Rom, larger C is sharper with more ringing (0.75 is a common
// "sharp bicubic"). Every frame the history is fetched at a sub-pixel
// offset, since a tracked head is never quite still, and each fetch is a
// low-pass whose losses compound through the exponential average: at a
// third of a cycle per pixel Catmull-Rom keeps about half of the
// contrast after that compounding at a 0.9 blend, and a sharper kernel
// keeps more. advanced.temporal_aa_history_sharp sets C, live.
float4 catmullRom(float2 uv, float2 tsize) {
    float C = jit.w;
    float2 sp = uv * tsize;
    float2 t1 = floor(sp - 0.5) + 0.5;
    float2 f = sp - t1;
    float2 g0 = 1.0 + f;
    float2 g3 = 2.0 - f;
    float2 w0 = C * (-g0 * g0 * g0 + 5.0 * g0 * g0 - 8.0 * g0 + 4.0);
    float2 w1 = (2.0 - C) * f * f * f + (C - 3.0) * f * f + 1.0;
    float2 h = 1.0 - f;
    float2 w2 = (2.0 - C) * h * h * h + (C - 3.0) * h * h + 1.0;
    float2 w3 = C * (-g3 * g3 * g3 + 5.0 * g3 * g3 - 8.0 * g3 + 4.0);
    float2 w12 = w1 + w2;
    float2 o12 = w2 / w12;
    float2 t0 = (t1 - 1.0) / tsize;
    float2 t3 = (t1 + 2.0) / tsize;
    float2 t12 = (t1 + o12) / tsize;
    float4 r = 0.0;
    r += H.SampleLevel(L, float2(t0.x, t0.y), 0) * w0.x * w0.y;
    r += H.SampleLevel(L, float2(t12.x, t0.y), 0) * w12.x * w0.y;
    r += H.SampleLevel(L, float2(t3.x, t0.y), 0) * w3.x * w0.y;
    r += H.SampleLevel(L, float2(t0.x, t12.y), 0) * w0.x * w12.y;
    r += H.SampleLevel(L, float2(t12.x, t12.y), 0) * w12.x * w12.y;
    r += H.SampleLevel(L, float2(t3.x, t12.y), 0) * w3.x * w12.y;
    r += H.SampleLevel(L, float2(t0.x, t3.y), 0) * w0.x * w3.y;
    r += H.SampleLevel(L, float2(t12.x, t3.y), 0) * w12.x * w3.y;
    r += H.SampleLevel(L, float2(t3.x, t3.y), 0) * w3.x * w3.y;
    return r;
}
// temporalReproject, transcribed: the pixel's direction now, rotated into
// last frame's view by the rows given, projected through last frame's
// frustum, the history fetched there. False off the image or behind the
// eye. One function, so the instrument can ask it of every candidate.
// With a translation and depth, the pixel is a POINT, not a direction:
// P = z * d (d.z = -1, so z is the view depth in metres), moved to last
// frame's eye space by delta * P + tv, then projected. Without depth (the
// far plane, or none bound) the direction alone is rotated, which is the
// rotation-only path: exact at infinity, and what v1 was everywhere.
bool fetchHistoryT(float2 p, float3 r0, float3 r1, float3 r2, float3 tv,
                   bool useDepth, out float3 hy) {
    float3 d;
    d.x = tanNow.x + (p.x + 0.5) / float(size.x) * (tanNow.y - tanNow.x);
    d.y = tanNow.w - (p.y + 0.5) / float(size.y) * (tanNow.w - tanNow.z);
    d.z = -1.0;
    float3 dp = float3(dot(r0, d), dot(r1, d), dot(r2, d));
    if (useDepth) {
        // The NEAREST depth of the 3x3, not the pixel's own: at the edge of
        // a near thing against a far one the pixel's own depth is either,
        // and a history fetched by the far one at a text stroke's edge is
        // the "underwater" the second depth flight saw at rest. With the
        // nearest, the edge follows the thing in front, which is the
        // standard dilation every velocity-based filter does.
        float zr = 0.0;
        [unroll] for (int oy = -1; oy <= 1; ++oy) {
            [unroll] for (int ox = -1; ox <= 1; ++ox) {
                int2 q = clamp(int2(p) + int2(ox, oy), int2(0, 0), size - 1);
                zr = max(zr, Z.Load(int3(region.xy + q, 0)));
            }
        }
        float den = zr * (knobs.w - knobs.z) + knobs.z;
        if (zr > 0.0 && den > 0.0) {
            float z = knobs.z * knobs.w / den;
            dp = dp * z + tv;
        }
    }
    hy = 0.0;
    if (dp.z >= -1e-6) return false;
    float xt = dp.x / -dp.z;
    float yt = dp.y / -dp.z;
    float2 pp;
    pp.x = (xt - tanPrev.x) / (tanPrev.y - tanPrev.x) * float(size.x) - 0.5;
    pp.y = (tanPrev.w - yt) / (tanPrev.w - tanPrev.z) * float(size.y) - 0.5;
    if (pp.x < 0.0 || pp.y < 0.0 || pp.x > float(size.x) - 1.0 ||
        pp.y > float(size.y) - 1.0) {
        return false;
    }
    // The rest snap: a head within a fraction of a pixel of still is
    // treated as still, and the history is fetched at its own texel
    // instead of resampled a hair off it. Tracking noise alone moves a
    // 3096-wide eye by a few tenths of a pixel a frame, and resampling at
    // such offsets every frame is the blur that compounds; the snap's
    // error is bounded by its threshold and never accumulates past it,
    // since each frame re-registers the history afresh. Off at 0.
    // A SMOOTH snap: the fetch offset is scaled down continuously as it
    // shrinks below the threshold, so neighbouring pixels on either side
    // of it do not resample differently from frame to frame. The hard
    // per-pixel snap of the seventh build did exactly that once depth
    // made the offsets vary pixel by pixel, and the HUD's text shimmered
    // "as if underwater" with the head held still (2026-09-03).
    float2 dpp = pp - p;
    if (knobs.x > 0.0) {
        float m = max(abs(dpp.x), abs(dpp.y));
        pp = p + dpp * smoothstep(0.5 * knobs.x, 1.5 * knobs.x, m);
    }
    hy = rgbToYcocg(catmullRom((pp + 0.5) / float2(size), float2(size)).rgb);
    return true;
}
bool fetchHistory(float2 p, float3 r0, float3 r1, float3 r2, out float3 hy) {
    return fetchHistoryT(p, r0, r1, r2, float3(0.0, 0.0, 0.0), false, hy);
}
// How far a clip moved the history, in luma, as a count of 1/255ths: a
// nudge on a text edge is a few, a history that landed somewhere else
// entirely is tens. Summed per reading, it separates the two where a
// count of clipped pixels cannot.
uint clipSize(float3 hc, float3 hy) {
    return uint(saturate(abs(hc.x - hy.x)) * 255.0 + 0.5);
}
// The motion vectors for a trained pass (DLAA): the same reprojection
// the history fetch does, written out instead of used -- the pixel's
// position last frame minus its position now, in render pixels, which
// is DLSS's convention with a scale of one. Off the image or behind the
// eye: no motion. The depth goes beside it, copied as the game wrote it
// (reversed-Z, told to the runtime as such).
[numthreads(8, 8, 1)]
void mv(uint3 id : SV_DispatchThreadID) {
    if (id.x >= (uint)size.x || id.y >= (uint)size.y) return;
    float2 p = float2(id.xy);
    float3 d;
    d.x = tanNow.x + (p.x + 0.5) / float(size.x) * (tanNow.y - tanNow.x);
    d.y = tanNow.w - (p.y + 0.5) / float(size.y) * (tanNow.w - tanNow.z);
    d.z = -1.0;
    float3 dp = float3(dot(dR0.xyz, d), dot(dR1.xyz, d), dot(dR2.xyz, d));
    float zraw = Z.Load(int3(region.xy + int2(p), 0));
    if (knobs.y != 0.0) {
        float zr = 0.0;
        [unroll] for (int oy = -1; oy <= 1; ++oy) {
            [unroll] for (int ox = -1; ox <= 1; ++ox) {
                int2 q = clamp(int2(p) + int2(ox, oy), int2(0, 0), size - 1);
                zr = max(zr, Z.Load(int3(region.xy + q, 0)));
            }
        }
        float den = zr * (knobs.w - knobs.z) + knobs.z;
        if (zr > 0.0 && den > 0.0) {
            float z = knobs.z * knobs.w / den;
            dp = dp * z + tvUsed.xyz;
        }
    }
    float2 motion = 0.0;
    if (dp.z < -1e-6) {
        float xt = dp.x / -dp.z;
        float yt = dp.y / -dp.z;
        float2 pp;
        pp.x = (xt - tanPrev.x) / (tanPrev.y - tanPrev.x) * float(size.x) - 0.5;
        pp.y = (tanPrev.w - yt) / (tanPrev.w - tanPrev.z) * float(size.y) - 0.5;
        motion = pp - p;
    }
    MV[id.xy] = motion;
    ZC[id.xy] = knobs.y != 0.0 ? zraw : 0.0;
}
groupshared uint gCount[15];
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID, uint gi : SV_GroupIndex) {
    if (gi < 15) gCount[gi] = 0;
    GroupMemoryBarrierWithGroupSync();
    uint count[15] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    if (id.x < (uint)size.x && id.y < (uint)size.y) {
        float2 p = float2(id.xy);
        int2 ci = int2(id.xy);
        // This frame's sample and its neighbourhood, in one pass over the
        // 3x3 around the pixel. The sample the game rendered at q sits at
        // q - jit on the unjittered grid, so each is weighted by its
        // distance from THIS pixel's centre there: exp(-2.29 d^2), a
        // Gaussian of sigma 0.47 px, UE4's filter for the same job. Two
        // The blend uses the filtered value, so a pixel whose history is
        // rejected shows a spatially settled sample rather than the raw
        // one hopping by the jitter. The neighbourhood's moments are NOT
        // weighted the same way: the sixth build tried that, the box
        // narrowed to the filter's width, the clip fired on a third of
        // the pixels at rest by hair-widths, and the picture shimmered
        // faintly everywhere (measured 2026-09-03: 35% clipped by 0.3/255
        // with the head still, against 11% by 0.5 before). The plain 3x3
        // it is. At 3096 wide before a 1.5x resolve the filter's
        // softening is a third of an output pixel; the sharpen recovers
        // the rest. advanced.temporal_aa_current = raw gives the point
        // sample back for an A/B.
        float4 cur = 0.0;
        float wsum = 0.0;
        float3 m1 = 0.0;
        float3 m2 = 0.0;
        float msum = 0.0;
        [unroll] for (int dy = -1; dy <= 1; ++dy) {
            [unroll] for (int dx = -1; dx <= 1; ++dx) {
                int2 q = clamp(ci + int2(dx, dy), int2(0, 0), size - 1);
                float4 sq = S.Load(int3(region.xy + q, 0));
                float2 dpos = float2(dx, dy) - jit.xy;
                float w = jit.z != 0.0 ? exp(-2.29 * dot(dpos, dpos))
                                       : ((dx == 0 && dy == 0) ? 1.0 : 0.0);
                float wm = 1.0;
                cur += sq * w;
                wsum += w;
                float3 s = rgbToYcocg(sq.rgb);
                m1 += s * wm;
                m2 += s * s * wm;
                msum += wm;
            }
        }
        cur /= max(wsum, 1e-6);
        m1 /= msum;
        m2 /= msum;
        float3 sigma = sqrt(max(m2 - m1 * m1, 0.0));
        float3 boxMin = m1 - gamma * sigma;
        float3 boxMax = m1 + gamma * sigma;
        float3 outc = cur.rgb;
        bool used = false;
        if (haveHistory != 0) {
            float3 hy;
            if (fetchHistoryT(p, dR0.xyz, dR1.xyz, dR2.xyz, tvUsed.xyz,
                              knobs.y != 0.0 && tvUsed.w != 0.0, hy)) {
                float3 hc = clipToBox(boxMin, boxMax, hy);
                if (any(abs(hc - hy) > 1e-4)) {
                    count[1] = 1;
                    count[2] = clipSize(hc, hy);
                }
                outc = lerp(cur.rgb, ycocgToRgb(hc), blend);
                used = true;
            }
            // The registration instrument: what each candidate would have
            // fetched, judged by the same clip, counted and not used. The
            // candidates are now: 0 the head's rotation alone, 1 the head
            // with depth and the eyes SWAPPED, 2 the game's camera rows,
            // 3 the head with depth as assigned. Up to four more history
            // reads per pixel while it runs.
            [unroll] for (int c = 0; c < 4; ++c) {
                if ((candMask & (1 << c)) == 0) continue;
                float3 r0 = c == 0 ? c0R0.xyz : (c == 1 ? c1R0.xyz : (c == 2 ? c2R0.xyz : c3R0.xyz));
                float3 r1 = c == 0 ? c0R1.xyz : (c == 1 ? c1R1.xyz : (c == 2 ? c2R1.xyz : c3R1.xyz));
                float3 r2 = c == 0 ? c0R2.xyz : (c == 1 ? c1R2.xyz : (c == 2 ? c2R2.xyz : c3R2.xyz));
                float3 tvc = c == 1 ? tvCand.xyz : tvUsed.xyz;
                bool depthC = knobs.y != 0.0 && (c == 1 || c == 3);
                float3 h;
                if (!fetchHistoryT(p, r0, r1, r2, tvc, depthC, h)) {
                    count[3 + c * 3] = 1;
                } else {
                    float3 hc2 = clipToBox(boxMin, boxMax, h);
                    if (any(abs(hc2 - h) > 1e-4)) {
                        count[4 + c * 3] = 1;
                        count[5 + c * 3] = clipSize(hc2, h);
                    }
                }
            }
        }
        if (!used) count[0] = 1;
        float3 o = saturate(outc);
        O[id.xy] = float4(o, cur.a);
        N[id.xy] = float4(o, 1.0);
    }
    // One atomic per group per counter, not per pixel.
    [unroll] for (int k = 0; k < 15; ++k) {
        if (count[k] != 0) InterlockedAdd(gCount[k], count[k]);
    }
    GroupMemoryBarrierWithGroupSync();
    if (gi < 15) InterlockedAdd(Stats[gi], gCount[gi]);
}
)HLSL";

// The cbuffer above, laid out to match: 384 bytes, twenty-four 16-byte rows.
struct PassParams {
    int32_t region[4];
    int32_t size[2];
    int32_t texSize[2];
    float   tanNow[4];
    float   tanPrev[4];
    float   jit[4];
    float   dR0[4];
    float   dR1[4];
    float   dR2[4];
    float   cand[4][3][4];   // candidate, row, xyz + pad
    float   blend;
    float   gamma;
    int32_t haveHistory;
    int32_t candMask;
    float   knobs[4];
    float   tvUsed[4];
    float   tvCand[4];
};
static_assert(sizeof(PassParams) == 384, "the cbuffer is twenty-four 16-byte rows");

// The format allowlist: the supersample resolve's, for its reasons
// (supersample_pass.cpp) -- typeless and UNORM families read and written
// through the family's plain typed view, the source's own format kept on
// the output, sRGB-typed sources refused.
DXGI_FORMAT viewFormatOf(DXGI_FORMAT f, int* index) {
    switch (f) {
        case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        case DXGI_FORMAT_R8G8B8A8_UNORM:
            *index = 0;
            return DXGI_FORMAT_R8G8B8A8_UNORM;
        case DXGI_FORMAT_B8G8R8A8_TYPELESS:
        case DXGI_FORMAT_B8G8R8A8_UNORM:
            *index = 1;
            return DXGI_FORMAT_B8G8R8A8_UNORM;
        case DXGI_FORMAT_R10G10B10A2_TYPELESS:
        case DXGI_FORMAT_R10G10B10A2_UNORM:
            *index = 2;
            return DXGI_FORMAT_R10G10B10A2_UNORM;
        case DXGI_FORMAT_R16G16B16A16_TYPELESS:
        case DXGI_FORMAT_R16G16B16A16_UNORM:
            *index = 3;
            return DXGI_FORMAT_R16G16B16A16_UNORM;
        case DXGI_FORMAT_R16G16B16A16_FLOAT:
            *index = 4;
            return DXGI_FORMAT_R16G16B16A16_FLOAT;
        default:
            *index = -1;
            return DXGI_FORMAT_UNKNOWN;
    }
}
constexpr int kFormatCount = 5;

const char* formatName(DXGI_FORMAT f) {
    switch (f) {
        case DXGI_FORMAT_R8G8B8A8_TYPELESS:     return "R8G8B8A8_TYPELESS";
        case DXGI_FORMAT_R8G8B8A8_UNORM:        return "R8G8B8A8_UNORM";
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:   return "R8G8B8A8_UNORM_SRGB";
        case DXGI_FORMAT_B8G8R8A8_TYPELESS:     return "B8G8R8A8_TYPELESS";
        case DXGI_FORMAT_B8G8R8A8_UNORM:        return "B8G8R8A8_UNORM";
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:   return "B8G8R8A8_UNORM_SRGB";
        case DXGI_FORMAT_R10G10B10A2_TYPELESS:  return "R10G10B10A2_TYPELESS";
        case DXGI_FORMAT_R10G10B10A2_UNORM:     return "R10G10B10A2_UNORM";
        case DXGI_FORMAT_R16G16B16A16_TYPELESS: return "R16G16B16A16_TYPELESS";
        case DXGI_FORMAT_R16G16B16A16_UNORM:    return "R16G16B16A16_UNORM";
        case DXGI_FORMAT_R16G16B16A16_FLOAT:    return "R16G16B16A16_FLOAT";
        default:                                return "?";
    }
}

const char* motionName(int motion) {
    return motion == 3 ? "head with depth"
         : motion == 2 ? "camera" : motion == 1 ? "head" : "none";
}

// Per-eye owned resources. Release-before-recreate on any size or format
// change; a change of size is also a reset of the history, which cannot
// mean anything across a resize.
struct EyeState {
    void*                      srcRes = nullptr;   // the game's texture the view is over (identity)
    ID3D11ShaderResourceView*  srcSrv = nullptr;
    // The scene's depth for this eye, from the depth probe's held texture:
    // a view typed to the depth channel, keyed on the texture like the
    // source's view. Released when a different texture arrives.
    void*                      depthRes = nullptr;
    ID3D11ShaderResourceView*  depthSrv = nullptr;
    // For the trained pass: the colour copied out typed, the motion
    // vectors and the depth copy it is fed, and its output.
    ID3D11Texture2D*           dlColour = nullptr;
    ID3D11Texture2D*           dlMv = nullptr;
    ID3D11UnorderedAccessView* dlMvUav = nullptr;
    ID3D11Texture2D*           dlDepth = nullptr;
    ID3D11UnorderedAccessView* dlDepthUav = nullptr;
    ID3D11Texture2D*           dlOut = nullptr;
    uint32_t                   dlW = 0, dlH = 0;
    ID3D11Texture2D*           copyTex = nullptr;  // the copy-through, for a source that refuses a view
    ID3D11ShaderResourceView*  copySrv = nullptr;
    uint32_t                   copyW = 0, copyH = 0;
    DXGI_FORMAT                copyFmt = DXGI_FORMAT_UNKNOWN;

    ID3D11Texture2D*           hist[2] = {};       // ping-pong: read one, write the other
    ID3D11ShaderResourceView*  histSrv[2] = {};
    ID3D11UnorderedAccessView* histUav[2] = {};
    int                        histRead = 0;
    bool                       haveHistory = false;

    ID3D11Texture2D*           outTex = nullptr;
    ID3D11UnorderedAccessView* outUav = nullptr;

    uint32_t    w = 0, h = 0;
    DXGI_FORMAT outFmt = DXGI_FORMAT_UNKNOWN;
    DXGI_FORMAT histFmt = DXGI_FORMAT_UNKNOWN;
};
EyeState g_eye[2];

void releaseSrc(EyeState& e) {
    if (e.srcSrv) { e.srcSrv->Release(); e.srcSrv = nullptr; }
    e.srcRes = nullptr;
}
void releaseDepth(EyeState& e) {
    if (e.depthSrv) { e.depthSrv->Release(); e.depthSrv = nullptr; }
    e.depthRes = nullptr;
}
void releaseDl(EyeState& e) {
    if (e.dlMvUav) { e.dlMvUav->Release(); e.dlMvUav = nullptr; }
    if (e.dlDepthUav) { e.dlDepthUav->Release(); e.dlDepthUav = nullptr; }
    if (e.dlColour) { e.dlColour->Release(); e.dlColour = nullptr; }
    if (e.dlMv) { e.dlMv->Release(); e.dlMv = nullptr; }
    if (e.dlDepth) { e.dlDepth->Release(); e.dlDepth = nullptr; }
    if (e.dlOut) { e.dlOut->Release(); e.dlOut = nullptr; }
    e.dlW = e.dlH = 0;
}
void releaseCopy(EyeState& e) {
    if (e.copySrv) { e.copySrv->Release(); e.copySrv = nullptr; }
    if (e.copyTex) { e.copyTex->Release(); e.copyTex = nullptr; }
    e.copyW = e.copyH = 0;
    e.copyFmt = DXGI_FORMAT_UNKNOWN;
}
void releaseOwned(EyeState& e) {
    for (int i = 0; i < 2; ++i) {
        if (e.histUav[i]) { e.histUav[i]->Release(); e.histUav[i] = nullptr; }
        if (e.histSrv[i]) { e.histSrv[i]->Release(); e.histSrv[i] = nullptr; }
        if (e.hist[i]) { e.hist[i]->Release(); e.hist[i] = nullptr; }
    }
    if (e.outUav) { e.outUav->Release(); e.outUav = nullptr; }
    if (e.outTex) { e.outTex->Release(); e.outTex = nullptr; }
    e.w = e.h = 0;
    e.outFmt = e.histFmt = DXGI_FORMAT_UNKNOWN;
    e.haveHistory = false;
    e.histRead = 0;
}
void releaseEye(EyeState& e) {
    releaseSrc(e);
    releaseDepth(e);
    releaseDl(e);
    releaseCopy(e);
    releaseOwned(e);
}

// One slot per treated call: the GPU price by timestamp query, and the
// pass's own count of rejected and clipped pixels copied out to a staging
// buffer. Never awaited (DONOTFLUSH, DO_NOT_WAIT); a slot still in flight
// is read on a later call, and a call that finds every slot busy runs
// unmeasured. Measuring must never be able to stall the pass.
struct Slot {
    ID3D11Query*  disjoint = nullptr;
    ID3D11Query*  begin = nullptr;
    ID3D11Query*  end = nullptr;
    ID3D11Buffer* staging = nullptr;
    bool          inUse = false;
    bool          timeDone = false;
    bool          statsDone = false;
    uint64_t      pixels = 0;
    // The instrument's bookkeeping for this call: which candidates had a
    // delta (their pixel totals), the head's turn, whether history ran.
    uint64_t      candPixels[4] = {};
    float         headDeg = 0.0f;
    bool          hadHistory = false;
};
constexpr int kSlots = 8;
constexpr int kStatCount = 16;   // 15 used; a 64-byte buffer
Slot g_slots[kSlots];

void releaseSlot(Slot& q) {
    if (q.disjoint) { q.disjoint->Release(); q.disjoint = nullptr; }
    if (q.begin) { q.begin->Release(); q.begin = nullptr; }
    if (q.end) { q.end->Release(); q.end = nullptr; }
    if (q.staging) { q.staging->Release(); q.staging = nullptr; }
    q.inUse = false;
}

uint32_t g_timeCount = 0;
double   g_timeSum = 0.0;
double   g_timeMax = 0.0;
uint64_t g_pixelsSeen = 0;
uint64_t g_rejected = 0;
uint64_t g_clipped = 0;
// The registration instrument's sums: per candidate, and the selected
// delta's clip share by head speed (still, slow, fast).
// Since the last registration line, so every candidate is judged over
// the SAME frames: the depth flight of 2026-09-03 compared a session's
// worth of the rotation-only delta against fifteen seconds of the depth
// candidates and could not tell them apart.
uint64_t g_candPix[4] = {};
uint64_t g_candRej[4] = {};
uint64_t g_candClip[4] = {};
uint64_t g_candSize[4] = {};    // the clips' sizes, 1/255ths of luma, summed
uint64_t g_bucketPix[3] = {};
uint64_t g_bucketClip[3] = {};
uint64_t g_bucketSize[3] = {};
uint32_t g_bucketFrames[3] = {};
uint32_t g_intervalFrames = 0;
constexpr float kStillDeg = 0.03f;   // under 2 deg/s at 72 Hz: tracking noise
constexpr float kSlowDeg = 0.30f;    // under 22 deg/s: a glance
bool     g_priceLogged = false;
uint32_t g_lastW = 0, g_lastH = 0;

void maybeLogPrice() {
    if (g_priceLogged || g_timeCount < 120 || g_pixelsSeen == 0) return;
    g_priceLogged = true;
    Log::get().note(
        "temporal aa: measured %.2f ms per eye on average (max %.2f) at "
        "%ux%u -- one dispatch, nine history taps and a 3x3 neighbourhood "
        "per pixel. History rejected for %.1f%% of pixels and clipped for "
        "%.1f%% so far; both low with the head turning and the ship steady "
        "means the reprojection is right (docs\\anti-aliasing.md Phase 0 "
        "item 6's price, measured).",
        g_timeSum / static_cast<double>(g_timeCount), g_timeMax, g_lastW,
        g_lastH,
        100.0 * static_cast<double>(g_rejected) / static_cast<double>(g_pixelsSeen),
        100.0 * static_cast<double>(g_clipped) / static_cast<double>(g_pixelsSeen));
}

void pollSlots(ID3D11DeviceContext* ctx) {
    for (Slot& q : g_slots) {
        if (!q.inUse) continue;
        if (!q.timeDone) {
            D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{};
            if (ctx->GetData(q.disjoint, &dj, sizeof(dj),
                             D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK) {
                UINT64 t0 = 0, t1 = 0;
                const HRESULT hr0 = ctx->GetData(q.begin, &t0, sizeof(t0),
                                                 D3D11_ASYNC_GETDATA_DONOTFLUSH);
                const HRESULT hr1 = ctx->GetData(q.end, &t1, sizeof(t1),
                                                 D3D11_ASYNC_GETDATA_DONOTFLUSH);
                q.timeDone = true;
                if (!dj.Disjoint && hr0 == S_OK && hr1 == S_OK && dj.Frequency) {
                    const double ms = static_cast<double>(t1 - t0) * 1000.0 /
                                      static_cast<double>(dj.Frequency);
                    ++g_timeCount;
                    g_timeSum += ms;
                    if (ms > g_timeMax) g_timeMax = ms;
                }
            }
        }
        if (!q.statsDone) {
            D3D11_MAPPED_SUBRESOURCE m{};
            const HRESULT hr = ctx->Map(q.staging, 0, D3D11_MAP_READ,
                                        D3D11_MAP_FLAG_DO_NOT_WAIT, &m);
            if (SUCCEEDED(hr) && m.pData) {
                const uint32_t* v = static_cast<const uint32_t*>(m.pData);
                g_rejected += v[0];
                g_clipped += v[1];
                g_pixelsSeen += q.pixels;
                if (q.hadHistory) {
                    ++g_intervalFrames;
                    for (int c = 0; c < 4; ++c) {
                        if (!q.candPixels[c]) continue;
                        g_candPix[c] += q.candPixels[c];
                        g_candRej[c] += v[3 + c * 3];
                        g_candClip[c] += v[4 + c * 3];
                        g_candSize[c] += v[5 + c * 3];
                    }
                    const int b = q.headDeg < kStillDeg ? 0 : (q.headDeg < kSlowDeg ? 1 : 2);
                    g_bucketPix[b] += q.pixels;
                    g_bucketClip[b] += v[1];
                    g_bucketSize[b] += v[2];
                    ++g_bucketFrames[b];
                }
                ctx->Unmap(q.staging, 0);
                q.statsDone = true;
            } else if (hr != DXGI_ERROR_WAS_STILL_DRAWING) {
                q.statsDone = true;   // an unreadable sample; drop it
            }
        }
        if (q.timeDone && q.statsDone) q.inUse = false;
    }
    maybeLogPrice();
}

int acquireSlot(ID3D11Device* dev) {
    for (int i = 0; i < kSlots; ++i) {
        Slot& q = g_slots[i];
        if (q.inUse) continue;
        if (!q.disjoint) {
            D3D11_QUERY_DESC qdd{};
            qdd.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
            D3D11_QUERY_DESC qdt{};
            qdt.Query = D3D11_QUERY_TIMESTAMP;
            D3D11_BUFFER_DESC bd{};
            bd.ByteWidth = kStatCount * 4;
            bd.Usage = D3D11_USAGE_STAGING;
            bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            const bool made = SUCCEEDED(dev->CreateQuery(&qdd, &q.disjoint)) &&
                              SUCCEEDED(dev->CreateQuery(&qdt, &q.begin)) &&
                              SUCCEEDED(dev->CreateQuery(&qdt, &q.end)) &&
                              SUCCEEDED(dev->CreateBuffer(&bd, nullptr, &q.staging));
            if (!made) {
                releaseSlot(q);
                continue;
            }
        }
        return i;
    }
    return -1;
}

FaultBudget g_budget("temporalPass", 8);

ID3D11ComputeShader*       g_cs = nullptr;
bool                       g_csTried = false;
ID3D11ComputeShader*       g_csMv = nullptr;     // the motion-vector entry, for DLAA
bool                       g_csMvTried = false;
bool                       g_dlaaNoted = false;
bool                       g_dlaaFailNoted = false;
uint32_t                   g_dlaaTreats = 0;
ID3D11Buffer*              g_cb = nullptr;
ID3D11SamplerState*        g_samp = nullptr;
ID3D11Buffer*              g_stats = nullptr;
ID3D11UnorderedAccessView* g_statsUav = nullptr;

bool     g_failNoted = false;
bool     g_kindNoted = false;
bool     g_regionNoted = false;
bool     g_fmtUnknownNoted = false;
bool     g_fmtChecked[kFormatCount] = {};
bool     g_fmtSupported[kFormatCount] = {};
bool     g_fmtUnsupportedNoted[kFormatCount] = {};
bool     g_histChecked = false;
DXGI_FORMAT g_histFmt = DXGI_FORMAT_UNKNOWN;
bool     g_firstNoted = false;
uint32_t g_treats = 0;

// The configure and warm state.
bool     g_wanted = false;
bool     g_viewTransposed = false;
bool     g_filterCurrent = true;   // advanced.temporal_aa_current = filtered | raw
float    g_historyC = 0.5f;        // advanced.temporal_aa_history_sharp: the cubic's C
float    g_snapPx = 0.15f;         // advanced.temporal_aa_snap: the rest snap, pixels
bool     g_warmNoted = false;

// The camera capture: the pending rows from the latest scene write, the
// rows latched at this frame's first eye draw, and last frame's.
float    g_pendingRows[12] = {};
bool     g_pendingValid = false;
float    g_curRows[12] = {};
bool     g_curValid = false;
bool     g_curLatched = false;
float    g_prevRows[12] = {};
bool     g_prevValid = false;
uint32_t g_camPairs = 0;
bool     g_camNoted = false;

void failOnce(const char* what) {
    if (g_failNoted) return;
    g_failNoted = true;
    Log::get().note("temporal aa: %s; the pass stands down.", what);
}

ID3D11ComputeShader* compileShader(ID3D11DeviceContext* ctx) {
    return shaderSwapCompileCs(ctx, kTemporalCsHlsl, sizeof(kTemporalCsHlsl) - 1,
                               "main", "temporal_aa_cs", nullptr,
                               "temporal aa");
}

bool makeTex(ID3D11Device* dev, uint32_t w, uint32_t h, DXGI_FORMAT texFmt,
             DXGI_FORMAT viewFmt, UINT bindFlags, ID3D11Texture2D** outTex,
             ID3D11ShaderResourceView** outSrv,
             ID3D11UnorderedAccessView** outUav) {
    D3D11_TEXTURE2D_DESC td{};
    td.Width = w;
    td.Height = h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = texFmt;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = bindFlags;
    if (FAILED(dev->CreateTexture2D(&td, nullptr, outTex)) || !*outTex) {
        return false;
    }
    if (outSrv) {
        D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.Format = viewFmt;
        sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        sd.Texture2D.MipLevels = 1;
        if (FAILED(dev->CreateShaderResourceView(*outTex, &sd, outSrv))) {
            return false;
        }
    }
    if (outUav) {
        D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};
        ud.Format = viewFmt;
        ud.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
        if (FAILED(dev->CreateUnorderedAccessView(*outTex, &ud, outUav))) {
            return false;
        }
    }
    return true;
}

bool setParams(ID3D11DeviceContext* ctx, const PassParams& p) {
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(ctx->Map(g_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m)) ||
        !m.pData) {
        return false;
    }
    memcpy(m.pData, &p, sizeof(p));
    ctx->Unmap(g_cb, 0);
    return true;
}

// The history's format: ten bits per channel is enough for an accumulation
// to converge (the 8-bit output stalls within a level of its target; ten
// bits stalls within a quarter of one) at half the memory of float16, and
// this pass holds two of them per eye at render size. Float16 when the
// device cannot store to it.
DXGI_FORMAT pickHistoryFormat(ID3D11Device* dev) {
    UINT support = 0;
    if (SUCCEEDED(dev->CheckFormatSupport(DXGI_FORMAT_R10G10B10A2_UNORM, &support)) &&
        (support & D3D11_FORMAT_SUPPORT_TYPED_UNORDERED_ACCESS_VIEW)) {
        return DXGI_FORMAT_R10G10B10A2_UNORM;
    }
    support = 0;
    if (SUCCEEDED(dev->CheckFormatSupport(DXGI_FORMAT_R16G16B16A16_FLOAT, &support)) &&
        (support & D3D11_FORMAT_SUPPORT_TYPED_UNORDERED_ACCESS_VIEW)) {
        return DXGI_FORMAT_R16G16B16A16_FLOAT;
    }
    return DXGI_FORMAT_UNKNOWN;
}

bool     g_depthNoted = false;
bool     g_depthHeld = false;      // the last treat had the depth in hand
uint32_t g_depthLostCount = 0;

void* temporalInner(void* srcTex, int eye, const float* bounds,
                    const float* tanNow, const float* tanPrev, float jxNow,
                    float jyNow, const float* deltaHead, const float* headTrans,
                    const float* headTransSwapped, float nearZ, float farZ,
                    float headDeg, int motion, float blend, float clampSigma,
                    unsigned flags) {
    ID3D11Texture2D* src = nullptr;
    static_cast<IUnknown*>(srcTex)->QueryInterface(
        __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&src));
    if (!src) return nullptr;

    D3D11_TEXTURE2D_DESC sd{};
    src->GetDesc(&sd);

    bool ok = true;
    if (sd.SampleDesc.Count > 1 || sd.ArraySize != 1 || sd.MipLevels != 1) {
        ok = false;
        if (!g_kindNoted) {
            g_kindNoted = true;
            Log::get().note(
                "temporal aa: the submitted texture is %ux%u samples=%u "
                "array=%u mips=%u, a kind the pass does not handle. The "
                "pass stands down.",
                sd.Width, sd.Height, sd.SampleDesc.Count, sd.ArraySize,
                sd.MipLevels);
        }
    }

    uint32_t region[4] = {};
    if (ok && !supersampleRegionFromBounds(sd.Width, sd.Height, bounds,
                                           region, nullptr, nullptr)) {
        ok = false;
        if (!g_regionNoted) {
            g_regionNoted = true;
            Log::get().note(
                "temporal aa: the Submit bounds name no usable eye region "
                "of a %ux%u texture. The pass stands down.",
                sd.Width, sd.Height);
        }
    }
    const uint32_t w = ok ? region[2] - region[0] : 0;
    const uint32_t h = ok ? region[3] - region[1] : 0;

    int fmtIndex = -1;
    DXGI_FORMAT viewFmt = DXGI_FORMAT_UNKNOWN;
    if (ok) {
        viewFmt = viewFormatOf(sd.Format, &fmtIndex);
        if (fmtIndex < 0) {
            ok = false;
            if (!g_fmtUnknownNoted) {
                g_fmtUnknownNoted = true;
                Log::get().note(
                    "temporal aa: the submitted texture's format is %s "
                    "(DXGI_FORMAT %d), one this pass does not handle -- "
                    "unmeasured formats are refused, not assumed. The pass "
                    "stands down; please report this log.",
                    formatName(sd.Format), static_cast<int>(sd.Format));
            }
        }
    }

    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    if (ok) {
        src->GetDevice(&dev);
        if (dev) dev->GetImmediateContext(&ctx);
        ok = dev != nullptr && ctx != nullptr;
    }
    if (ok) pollSlots(ctx);

    if (ok && !g_fmtChecked[fmtIndex]) {
        g_fmtChecked[fmtIndex] = true;
        UINT support = 0;
        g_fmtSupported[fmtIndex] =
            SUCCEEDED(dev->CheckFormatSupport(viewFmt, &support)) &&
            (support & D3D11_FORMAT_SUPPORT_TYPED_UNORDERED_ACCESS_VIEW) != 0;
    }
    if (ok && !g_fmtSupported[fmtIndex]) {
        ok = false;
        if (!g_fmtUnsupportedNoted[fmtIndex]) {
            g_fmtUnsupportedNoted[fmtIndex] = true;
            Log::get().note(
                "temporal aa: this GPU/driver reports no typed unordered-"
                "access support for %s, so the result cannot be written "
                "here. The pass stands down.",
                formatName(viewFmt));
        }
    }
    if (ok && !g_histChecked) {
        g_histChecked = true;
        g_histFmt = pickHistoryFormat(dev);
        if (g_histFmt == DXGI_FORMAT_UNKNOWN) {
            failOnce("neither R10G10B10A2_UNORM nor R16G16B16A16_FLOAT can be "
                     "stored to on this GPU/driver, and the history needs one");
        }
    }
    ok = ok && g_histFmt != DXGI_FORMAT_UNKNOWN;

    if (ok && !g_cs && !g_csTried) {
        g_csTried = true;
        g_cs = compileShader(ctx);
    }
    ok = ok && g_cs != nullptr;

    if (ok && !g_cb) {
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = sizeof(PassParams);
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        ok = SUCCEEDED(dev->CreateBuffer(&bd, nullptr, &g_cb));
        if (!ok) failOnce("the parameter buffer could not be created");
    }
    if (ok && !g_samp) {
        D3D11_SAMPLER_DESC smd{};
        smd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        smd.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
        smd.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
        smd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        smd.MaxLOD = D3D11_FLOAT32_MAX;
        ok = SUCCEEDED(dev->CreateSamplerState(&smd, &g_samp));
        if (!ok) failOnce("the sampler could not be created");
    }
    if (ok && !g_stats) {
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = kStatCount * 4;
        bd.Usage = D3D11_USAGE_DEFAULT;
        bd.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
        bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        bd.StructureByteStride = 4;
        D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};
        ud.Format = DXGI_FORMAT_UNKNOWN;
        ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        ud.Buffer.NumElements = kStatCount;
        ok = SUCCEEDED(dev->CreateBuffer(&bd, nullptr, &g_stats)) &&
             SUCCEEDED(dev->CreateUnorderedAccessView(g_stats, &ud, &g_statsUav));
        if (!ok) failOnce("the statistics buffer could not be created");
    }

    EyeState* eptr = ok ? &g_eye[eye] : nullptr;

    // The input view: over the source when it allows one, else the region
    // copied out (the theater's copy-through, the resolve's too).
    ID3D11ShaderResourceView* inSrv = nullptr;
    bool viaCopy = false;
    if (ok) {
        EyeState& e = *eptr;
        if (sd.BindFlags & D3D11_BIND_SHADER_RESOURCE) {
            if (e.srcRes != static_cast<void*>(src) || !e.srcSrv) {
                releaseSrc(e);
                D3D11_SHADER_RESOURCE_VIEW_DESC vd{};
                vd.Format = viewFmt;
                vd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
                vd.Texture2D.MipLevels = 1;
                if (SUCCEEDED(dev->CreateShaderResourceView(src, &vd, &e.srcSrv)) &&
                    e.srcSrv) {
                    e.srcRes = src;
                } else {
                    e.srcSrv = nullptr;
                }
            }
            inSrv = e.srcSrv;
        }
        if (!inSrv) {
            viaCopy = true;
            if (!e.copyTex || e.copyW != w || e.copyH != h || e.copyFmt != sd.Format) {
                releaseCopy(e);
                if (makeTex(dev, w, h, sd.Format, viewFmt, D3D11_BIND_SHADER_RESOURCE,
                            &e.copyTex, &e.copySrv, nullptr)) {
                    e.copyW = w;
                    e.copyH = h;
                    e.copyFmt = sd.Format;
                } else {
                    releaseCopy(e);
                }
            }
            inSrv = e.copySrv;
        }
        if (!inSrv) {
            ok = false;
            failOnce("the submitted texture refuses a shader view and could "
                     "not be copied");
        }
    }

    // The scene's depth for this eye, when the probe has settled on it and
    // the planes are known: a view typed to the depth channel over the
    // game's own texture, held by the probe. Wanted by the depth motion
    // and by the instrument's two depth candidates alike.
    ID3D11ShaderResourceView* depthSrv = nullptr;
    if (ok && nearZ > 0.0f && farZ > nearZ) {
        EyeState& e = *eptr;
        ID3D11Texture2D* dtex = nullptr;
        if (depthProbeSceneDepth(sd.Width, sd.Height, eye, &dtex) && dtex) {
            if (e.depthRes != static_cast<void*>(dtex) || !e.depthSrv) {
                releaseDepth(e);
                D3D11_TEXTURE2D_DESC dd{};
                dtex->GetDesc(&dd);
                DXGI_FORMAT rf = DXGI_FORMAT_UNKNOWN;
                switch (dd.Format) {
                    case DXGI_FORMAT_R32G8X24_TYPELESS:
                    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
                        rf = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS; break;
                    case DXGI_FORMAT_R32_TYPELESS:
                    case DXGI_FORMAT_D32_FLOAT:
                        rf = DXGI_FORMAT_R32_FLOAT; break;
                    case DXGI_FORMAT_R24G8_TYPELESS:
                    case DXGI_FORMAT_D24_UNORM_S8_UINT:
                        rf = DXGI_FORMAT_R24_UNORM_X8_TYPELESS; break;
                    default: break;
                }
                if (rf != DXGI_FORMAT_UNKNOWN && (dd.BindFlags & D3D11_BIND_SHADER_RESOURCE)) {
                    D3D11_SHADER_RESOURCE_VIEW_DESC vd{};
                    vd.Format = rf;
                    vd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
                    vd.Texture2D.MipLevels = 1;
                    if (SUCCEEDED(dev->CreateShaderResourceView(dtex, &vd, &e.depthSrv)) &&
                        e.depthSrv) {
                        e.depthRes = dtex;
                    } else {
                        e.depthSrv = nullptr;
                    }
                }
            }
            depthSrv = e.depthSrv;
        }
    }
    if (depthSrv && (!g_depthNoted || !g_depthHeld)) {
        g_depthNoted = true;
        Log::get().note(
            "temporal aa: the scene's depth is in hand -- the depth probe's "
            "%ux%u target for this eye, read through a depth-channel view, "
            "reversed-Z with the game's planes %.3f..%.0f m. The depth motion "
            "reprojects every pixel with the head's translation from here; "
            "the registration line's 'head with depth' and 'depth, eyes "
            "swapped' candidates say whether the eyes are assigned right.",
            sd.Width, sd.Height, static_cast<double>(nearZ), static_cast<double>(farZ));
    } else if (!depthSrv && g_depthHeld && eye == 0) {
        ++g_depthLostCount;
        if (g_depthLostCount <= 3) {
            Log::get().note(
                "temporal aa: the scene's depth went away (the render size "
                "changed, or the probe has not settled on the new targets "
                "yet) -- the pass runs on the head's rotation alone until it "
                "is found again, and says so when it is.");
        }
    }
    if (eye == 0) g_depthHeld = depthSrv != nullptr;

    // The owned pair and the output, rebuilt on any change of size or
    // format -- which is a history reset too.
    if (ok) {
        EyeState& e = *eptr;
        if (!e.outTex || e.w != w || e.h != h || e.outFmt != sd.Format ||
            e.histFmt != g_histFmt) {
            releaseOwned(e);
            bool made = makeTex(dev, w, h, sd.Format, viewFmt,
                                D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS,
                                &e.outTex, nullptr, &e.outUav);
            for (int i = 0; i < 2 && made; ++i) {
                made = makeTex(dev, w, h, g_histFmt, g_histFmt,
                               D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS,
                               &e.hist[i], &e.histSrv[i], &e.histUav[i]);
            }
            if (made) {
                e.w = w;
                e.h = h;
                e.outFmt = sd.Format;
                e.histFmt = g_histFmt;
            } else {
                releaseOwned(e);
                ok = false;
                failOnce("the history or output textures could not be created");
            }
        }
    }

    void* result = nullptr;
    if (ok) {
        EyeState& e = *eptr;
        if (flags & 1u) e.haveHistory = false;

        // The rotation delta for this frame's motion source. The depth
        // motion is the head's rotation with its translation term, and
        // falls back to the rotation alone until the depth is in hand.
        float delta[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
        bool haveDelta = motion == 0;
        const bool depthMotion = motion == 3;
        if ((motion == 1 || depthMotion) && deltaHead) {
            memcpy(delta, deltaHead, sizeof(delta));
            haveDelta = true;
        } else if (motion == 2) {
            if (g_curValid && g_prevValid) {
                temporalViewDelta(g_prevRows, g_curRows, g_viewTransposed, delta);
                haveDelta = true;
            }
        }
        // No delta means no reprojection can be trusted: this frame goes
        // out unblended and the history restarts from it.
        const bool useHistory = e.haveHistory && haveDelta && tanPrev;

        // The registration instrument's four candidates, whichever of
        // them exist this frame (temporalPassRegistration).
        float cand[4][9];
        bool candValid[4] = {};
        const bool haveDepth = depthSrv != nullptr && headTrans != nullptr;
        if (deltaHead) {
            memcpy(cand[0], deltaHead, sizeof(cand[0]));
            candValid[0] = true;
            // 3: the head with depth as assigned; 1: with the eyes swapped.
            if (haveDepth) {
                memcpy(cand[3], deltaHead, sizeof(cand[3]));
                candValid[3] = true;
                if (headTransSwapped) {
                    memcpy(cand[1], deltaHead, sizeof(cand[1]));
                    candValid[1] = true;
                }
            }
        }
        if (g_curValid && g_prevValid) {
            temporalViewDelta(g_prevRows, g_curRows, false, cand[2]);
            candValid[2] = true;
        }

        PassParams p{};
        if (viaCopy) {
            p.region[0] = 0;
            p.region[1] = 0;
            p.region[2] = static_cast<int32_t>(w);
            p.region[3] = static_cast<int32_t>(h);
            p.texSize[0] = static_cast<int32_t>(w);
            p.texSize[1] = static_cast<int32_t>(h);
        } else {
            for (int i = 0; i < 4; ++i) p.region[i] = static_cast<int32_t>(region[i]);
            p.texSize[0] = static_cast<int32_t>(sd.Width);
            p.texSize[1] = static_cast<int32_t>(sd.Height);
        }
        p.size[0] = static_cast<int32_t>(w);
        p.size[1] = static_cast<int32_t>(h);
        memcpy(p.tanNow, tanNow, sizeof(p.tanNow));
        memcpy(p.tanPrev, useHistory ? tanPrev : tanNow, sizeof(p.tanPrev));
        p.jit[0] = jxNow;
        p.jit[1] = jyNow;
        p.jit[2] = g_filterCurrent ? 1.0f : 0.0f;
        p.jit[3] = g_historyC;
        p.knobs[0] = g_snapPx;
        p.knobs[1] = haveDepth ? 1.0f : 0.0f;
        p.knobs[2] = nearZ;
        p.knobs[3] = farZ;
        if (haveDepth) {
            for (int i = 0; i < 3; ++i) {
                p.tvUsed[i] = headTrans[i];
                p.tvCand[i] = headTransSwapped ? headTransSwapped[i] : headTrans[i];
            }
        }
        // The used delta carries its translation only under the depth
        // motion; the head motion stays rotation-only, as v1 was.
        p.tvUsed[3] = (depthMotion && haveDepth) ? 1.0f : 0.0f;
        for (int c = 0; c < 3; ++c) {
            p.dR0[c] = delta[0 * 3 + c];
            p.dR1[c] = delta[1 * 3 + c];
            p.dR2[c] = delta[2 * 3 + c];
        }
        int candMask = 0;
        for (int k = 0; k < 4; ++k) {
            if (!candValid[k]) continue;
            candMask |= 1 << k;
            for (int r = 0; r < 3; ++r) {
                for (int c = 0; c < 3; ++c) p.cand[k][r][c] = cand[k][r * 3 + c];
            }
        }
        p.blend = blend;
        p.gamma = clampSigma;
        p.haveHistory = useHistory ? 1 : 0;
        p.candMask = useHistory ? candMask : 0;

        ID3D11ComputeShader* savedCs = nullptr;
        ID3D11ShaderResourceView* savedSrv[3] = {};
        ID3D11UnorderedAccessView* savedUav[3] = {};
        ID3D11Buffer* savedCb = nullptr;
        ID3D11SamplerState* savedSamp = nullptr;
        ctx->CSGetShader(&savedCs, nullptr, nullptr);
        ctx->CSGetShaderResources(0, 3, savedSrv);
        ctx->CSGetUnorderedAccessViews(0, 3, savedUav);
        ctx->CSGetConstantBuffers(0, 1, &savedCb);
        ctx->CSGetSamplers(0, 1, &savedSamp);
        // The game's depth target may still be bound on the output-merger
        // stage at submit, and D3D nulls a shader view over a bound target
        // without a word (the depth probe learned that the hard way). The
        // stage is cleared for the dispatch and put back exactly after.
        ID3D11RenderTargetView* savedRtv[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
        ID3D11DepthStencilView* savedDsv = nullptr;
        if (depthSrv) {
            ctx->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, savedRtv, &savedDsv);
            ctx->OMSetRenderTargets(0, nullptr, nullptr);
        }

        // THE TRAINED PASS, when asked for (flags bit 1) and available: the
        // motion vectors and the depth copy from the same reprojection
        // the history fetch uses, the colour copied out typed, then
        // NVIDIA's evaluation into an owned output that goes out in the
        // pass's place. Any refusal says so once and the pass's own
        // history runs instead, this frame and after.
        bool usedDlaa = false;
        if ((flags & 2u) != 0) {
            const char* why = "";
            if (!dlaaAvailable(dev, &why)) {
                if (!g_dlaaFailNoted) {
                    g_dlaaFailNoted = true;
                    Log::get().note(
                        "temporal aa: dlaa was asked for, but %s. The pass's own "
                        "history runs instead.",
                        why);
                }
            } else {
                if (!g_csMv && !g_csMvTried) {
                    g_csMvTried = true;
                    g_csMv = shaderSwapCompileCs(ctx, kTemporalCsHlsl, sizeof(kTemporalCsHlsl) - 1,
                                                 "mv", "temporal_mv_cs", nullptr,
                                                 "temporal aa");
                }
                bool made = g_csMv != nullptr;
                if (made && (!e.dlOut || e.dlW != w || e.dlH != h)) {
                    releaseDl(e);
                    made = makeTex(dev, w, h, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM,
                                   D3D11_BIND_SHADER_RESOURCE, &e.dlColour, nullptr, nullptr) &&
                           makeTex(dev, w, h, DXGI_FORMAT_R16G16_FLOAT, DXGI_FORMAT_R16G16_FLOAT,
                                   D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS,
                                   &e.dlMv, nullptr, &e.dlMvUav) &&
                           makeTex(dev, w, h, DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R32_FLOAT,
                                   D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS,
                                   &e.dlDepth, nullptr, &e.dlDepthUav) &&
                           makeTex(dev, w, h, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM,
                                   D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS,
                                   &e.dlOut, nullptr, nullptr);
                    if (made) {
                        e.dlW = w;
                        e.dlH = h;
                    } else {
                        releaseDl(e);
                    }
                }
                if (made && setParams(ctx, p)) {
                    // The colour, typed, whichever way the source came.
                    D3D11_BOX box{};
                    box.left = viaCopy ? 0 : region[0];
                    box.top = viaCopy ? 0 : region[1];
                    box.front = 0;
                    box.right = box.left + w;
                    box.bottom = box.top + h;
                    box.back = 1;
                    if (viaCopy) {
                        D3D11_BOX full{};
                        full.left = region[0];
                        full.top = region[1];
                        full.front = 0;
                        full.right = region[2];
                        full.bottom = region[3];
                        full.back = 1;
                        ctx->CopySubresourceRegion(e.copyTex, 0, 0, 0, 0, src, 0, &full);
                        ctx->CopySubresourceRegion(e.dlColour, 0, 0, 0, 0, e.copyTex, 0, &box);
                    } else {
                        ctx->CopySubresourceRegion(e.dlColour, 0, 0, 0, 0, src, 0, &box);
                    }
                    // The motion vectors and the depth copy.
                    ID3D11ShaderResourceView* nullSrvM[3] = {};
                    ID3D11UnorderedAccessView* nullUavM[5] = {};
                    ctx->CSSetShaderResources(0, 3, nullSrvM);
                    ctx->CSSetUnorderedAccessViews(0, 5, nullUavM, nullptr);
                    ctx->CSSetShader(g_csMv, nullptr, 0);
                    ID3D11ShaderResourceView* srvsM[3] = {inSrv, e.histSrv[e.histRead], depthSrv};
                    ID3D11UnorderedAccessView* uavsM[5] = {nullptr, nullptr, nullptr, e.dlMvUav,
                                                           e.dlDepthUav};
                    ID3D11Buffer* cbM = g_cb;
                    ID3D11SamplerState* smpM = g_samp;
                    ctx->CSSetShaderResources(0, 3, srvsM);
                    ctx->CSSetUnorderedAccessViews(0, 5, uavsM, nullptr);
                    ctx->CSSetConstantBuffers(0, 1, &cbM);
                    ctx->CSSetSamplers(0, 1, &smpM);
                    ctx->Dispatch((w + 7) / 8, (h + 7) / 8, 1);
                    ctx->CSSetShaderResources(0, 3, nullSrvM);
                    ctx->CSSetUnorderedAccessViews(0, 5, nullUavM, nullptr);
                    // NVIDIA's evaluation.
                    const bool resetHist = (flags & 1u) != 0 || !e.haveHistory;
                    if (dlaaEvaluate(ctx, eye, e.dlColour, e.dlDepth, e.dlMv, e.dlOut, w, h,
                                     jxNow, jyNow, resetHist, &why)) {
                        usedDlaa = true;
                        if (!g_dlaaNoted) {
                            g_dlaaNoted = true;
                            Log::get().note(
                                "temporal aa: DLAA engaged -- NVIDIA's history takes the "
                                "%ux%u frame, its depth%s and the pass's own motion "
                                "vectors, jittered as before; the pass's history and "
                                "clip stand aside. Its price prints in the totals.",
                                w, h, depthSrv ? "" : " (none in hand yet: no depth "
                                                       "until the probe finds it)");
                        }
                    } else if (!g_dlaaFailNoted) {
                        g_dlaaFailNoted = true;
                        Log::get().note(
                            "temporal aa: dlaa was asked for, but %s. The pass's own "
                            "history runs instead.",
                            why);
                    }
                }
            }
        }

        const int qs = usedDlaa ? -1 : acquireSlot(dev);
        if (qs >= 0) {
            ctx->Begin(g_slots[qs].disjoint);
            ctx->End(g_slots[qs].begin);
        }
        if (viaCopy && !usedDlaa) {
            D3D11_BOX box{};
            box.left = region[0];
            box.top = region[1];
            box.front = 0;
            box.right = region[2];
            box.bottom = region[3];
            box.back = 1;
            ctx->CopySubresourceRegion(e.copyTex, 0, 0, 0, 0, src, 0, &box);
        }
        const UINT zeros[4] = {0, 0, 0, 0};
        if (!usedDlaa) ctx->ClearUnorderedAccessViewUint(g_statsUav, zeros);

        const int readIdx = e.histRead;
        const int writeIdx = 1 - readIdx;
        bool ran = usedDlaa ? true : setParams(ctx, p);
        if (ran && !usedDlaa) {
            ID3D11ShaderResourceView* nullSrv[3] = {};
            ID3D11UnorderedAccessView* nullUav[3] = {};
            ctx->CSSetShaderResources(0, 3, nullSrv);
            ctx->CSSetUnorderedAccessViews(0, 3, nullUav, nullptr);
            ctx->CSSetShader(g_cs, nullptr, 0);
            ID3D11ShaderResourceView* srvs[3] = {inSrv, e.histSrv[readIdx], depthSrv};
            ID3D11UnorderedAccessView* uavs[3] = {e.outUav, e.histUav[writeIdx],
                                                  g_statsUav};
            ID3D11Buffer* cb = g_cb;
            ID3D11SamplerState* smp = g_samp;
            ctx->CSSetShaderResources(0, 3, srvs);
            ctx->CSSetUnorderedAccessViews(0, 3, uavs, nullptr);
            ctx->CSSetConstantBuffers(0, 1, &cb);
            ctx->CSSetSamplers(0, 1, &smp);
            ctx->Dispatch((w + 7) / 8, (h + 7) / 8, 1);
        }
        if (qs >= 0) {
            ctx->End(g_slots[qs].end);
            ctx->End(g_slots[qs].disjoint);
            ctx->CopyResource(g_slots[qs].staging, g_stats);
            g_slots[qs].inUse = true;
            g_slots[qs].timeDone = false;
            g_slots[qs].statsDone = false;
            g_slots[qs].pixels = static_cast<uint64_t>(w) * h;
            g_slots[qs].hadHistory = useHistory;
            g_slots[qs].headDeg = headDeg;
            for (int k = 0; k < 4; ++k) {
                g_slots[qs].candPixels[k] =
                    (useHistory && candValid[k]) ? static_cast<uint64_t>(w) * h : 0;
            }
        }

        ID3D11ShaderResourceView* nullSrv2[3] = {};
        ID3D11UnorderedAccessView* nullUav2[3] = {};
        ctx->CSSetShaderResources(0, 3, nullSrv2);
        ctx->CSSetUnorderedAccessViews(0, 3, nullUav2, nullptr);
        if (depthSrv) {
            ctx->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, savedRtv, savedDsv);
            for (auto* v : savedRtv) if (v) v->Release();
            if (savedDsv) savedDsv->Release();
        }
        ctx->CSSetShader(savedCs, nullptr, 0);
        ctx->CSSetShaderResources(0, 3, savedSrv);
        ctx->CSSetUnorderedAccessViews(0, 3, savedUav, nullptr);
        ctx->CSSetConstantBuffers(0, 1, &savedCb);
        ctx->CSSetSamplers(0, 1, &savedSamp);
        if (savedCs) savedCs->Release();
        for (auto* v : savedSrv) if (v) v->Release();
        for (auto* v : savedUav) if (v) v->Release();
        if (savedCb) savedCb->Release();
        if (savedSamp) savedSamp->Release();

        if (ran && usedDlaa) {
            // The trained pass's frame goes out; the pass's own history is
            // marked broken so a switch back starts afresh.
            e.haveHistory = false;
            result = e.dlOut;
            ++g_treats;
            ++g_dlaaTreats;
        } else if (ran) {
            e.histRead = writeIdx;
            e.haveHistory = true;
            result = e.outTex;
            ++g_treats;
            g_lastW = w;
            g_lastH = h;
            if (!g_firstNoted) {
                g_firstNoted = true;
                const double mb = static_cast<double>(w) * h *
                                  (2.0 * (g_histFmt == DXGI_FORMAT_R10G10B10A2_UNORM ? 4.0 : 8.0) +
                                   4.0) / 1048576.0;
                Log::get().note(
                    "temporal aa: first treated frame -- the game submits %s "
                    "(DXGI_FORMAT %d), read and written through %s views%s; "
                    "%ux%u per eye, history in %s, about %.0f MB per eye "
                    "resident; motion from the %s, blend %.2f, clip %.2f "
                    "sigma.",
                    formatName(sd.Format), static_cast<int>(sd.Format),
                    formatName(viewFmt),
                    viaCopy ? " (copied out first: the source refuses a "
                              "shader view)"
                            : "",
                    w, h, formatName(g_histFmt), mb, motionName(motion),
                    static_cast<double>(blend), static_cast<double>(clampSigma));
            }
        } else {
            failOnce("the parameter buffer could not be written");
        }
    }

    if (ctx) ctx->Release();
    if (dev) dev->Release();
    src->Release();
    return result;
}

}  // namespace

void temporalPassConfigure(Config& cfg) {
    const std::string mode = cfg.getString("fix.temporal_aa", "off");
    g_wanted = _stricmp(mode.c_str(), "off") != 0 && !mode.empty();
    g_viewTransposed = cfg.getBool("advanced.temporal_aa_view_transpose", false);
    const std::string cur = cfg.getString("advanced.temporal_aa_current", "filtered");
    g_filterCurrent = _stricmp(cur.c_str(), "raw") != 0;
    float c = cfg.getFloat("advanced.temporal_aa_history_sharp", 0.5f);
    if (!std::isfinite(c)) c = 0.5f;
    if (c < 0.5f) c = 0.5f;
    if (c > 1.0f) c = 1.0f;
    g_historyC = c;
    float snap = cfg.getFloat("advanced.temporal_aa_snap", 0.15f);
    if (!std::isfinite(snap) || snap < 0.0f) snap = 0.0f;
    if (snap > 0.5f) snap = 0.5f;
    g_snapPx = snap;
}

void temporalPassTick(ID3D11DeviceContext* ctx) {
    if (!g_wanted || !ctx) return;
    if (!g_cs && !g_csTried) {
        g_csTried = true;
        g_cs = compileShader(ctx);
        if (g_cs && !g_warmNoted) {
            g_warmNoted = true;
            Log::get().note(
                "temporal aa: shader warmed at session start -- the first "
                "treated eye pays no compile.");
        }
    }
}

void temporalPassNoteSceneWrite(const void* data, uint32_t bytes) {
    // The true view matrix lives at float offset 932 of the big scene
    // block (measured by the sun-glare fix's two-shot dump: three 3x4
    // rows, rotation plus translation). Only the rotation is kept, and
    // only when it is one.
    if (!g_wanted || !data || bytes < 944 * 4) return;
    const float* f = static_cast<const float*>(data) + 932;
    if (!temporalRowsAreRotation(f)) return;
    memcpy(g_pendingRows, f, sizeof(g_pendingRows));
    g_pendingValid = true;
}

void temporalPassNoteFirstEyeDraw() {
    if (!g_wanted || g_curLatched) return;
    g_curLatched = true;
    if (g_pendingValid) {
        memcpy(g_curRows, g_pendingRows, sizeof(g_curRows));
        g_curValid = true;
    } else {
        g_curValid = false;
    }
}

void temporalPassFrameBoundary() {
    if (!g_wanted) return;
    if (g_curLatched && g_curValid) {
        if (g_prevValid) ++g_camPairs;
        memcpy(g_prevRows, g_curRows, sizeof(g_prevRows));
        g_prevValid = true;
        if (!g_camNoted && g_camPairs >= 2) {
            g_camNoted = true;
            Log::get().note(
                "temporal aa: the game's camera is being read -- the view "
                "rows at float 932 of the scene block, latched at each "
                "frame's first eye draw; advanced.temporal_aa_motion = "
                "camera reprojects from them%s.",
                g_viewTransposed ? " (transposed)" : "");
        }
    } else {
        g_prevValid = false;
    }
    g_curLatched = false;
    g_curValid = false;
}

bool temporalPassTotals(uint32_t* treated, double* avgMs, double* maxMs,
                        double* rejectPct, double* clipPct) {
    if (g_treats == 0) return false;
    if (treated) *treated = g_treats;
    if (avgMs) *avgMs = g_timeCount ? g_timeSum / static_cast<double>(g_timeCount) : 0.0;
    if (maxMs) *maxMs = g_timeMax;
    const double px = g_pixelsSeen ? static_cast<double>(g_pixelsSeen) : 1.0;
    if (rejectPct) *rejectPct = 100.0 * static_cast<double>(g_rejected) / px;
    if (clipPct) *clipPct = 100.0 * static_cast<double>(g_clipped) / px;
    return true;
}

bool temporalPassRegistration(char* buf, size_t n) {
    if (!buf || n == 0 || g_treats == 0 || g_intervalFrames == 0) return false;
    static const char* const kNames[4] = {"head, rotation only", "head with depth, eyes swapped",
                                          "camera", "head with depth"};
    size_t used = 0;
    {
        const int m = snprintf(buf, n, "over the last %u eye-frames: ", g_intervalFrames);
        if (m > 0) used += static_cast<size_t>(m);
    }
    auto put = [&](const char* fmt, double a, double b, double c) {
        if (used >= n) return;
        const int k = snprintf(buf + used, n - used, fmt, a, b, c);
        if (k > 0) used += static_cast<size_t>(k);
    };
    for (int k = 0; k < 4; ++k) {
        if (used < n) {
            const int m = snprintf(buf + used, n - used, "%s%s ", k ? "; " : "", kNames[k]);
            if (m > 0) used += static_cast<size_t>(m);
        }
        if (!g_candPix[k]) {
            if (used < n) {
                const int m = snprintf(buf + used, n - used, "(no delta yet)");
                if (m > 0) used += static_cast<size_t>(m);
            }
            continue;
        }
        const double px = static_cast<double>(g_candPix[k]);
        // The mean clip size over the CLIPPED pixels: how far a clipped
        // history had strayed, 1/255ths of luma.
        const double meanSize = g_candClip[k]
            ? static_cast<double>(g_candSize[k]) / static_cast<double>(g_candClip[k])
            : 0.0;
        put("clipped %.1f%% by %.1f/255 on average, off %.1f%%",
            100.0 * static_cast<double>(g_candClip[k]) / px, meanSize,
            100.0 * static_cast<double>(g_candRej[k]) / px);
    }
    static const char* const kBuckets[3] = {"still", "slow", "fast"};
    if (used < n) {
        const int m = snprintf(buf + used, n - used,
                               ". The used delta's clip share by head speed (under %.2f, under %.2f, over that, degrees per frame): ",
                               static_cast<double>(kStillDeg), static_cast<double>(kSlowDeg));
        if (m > 0) used += static_cast<size_t>(m);
    }
    for (int b = 0; b < 3; ++b) {
        if (used >= n) break;
        if (!g_bucketPix[b]) {
            const int m = snprintf(buf + used, n - used, "%s%s none", b ? ", " : "", kBuckets[b]);
            if (m > 0) used += static_cast<size_t>(m);
            continue;
        }
        const double meanSize = g_bucketClip[b]
            ? static_cast<double>(g_bucketSize[b]) / static_cast<double>(g_bucketClip[b])
            : 0.0;
        const int m = snprintf(buf + used, n - used, "%s%s %.1f%% by %.1f/255 (%u eye-frames)",
                               b ? ", " : "", kBuckets[b],
                               100.0 * static_cast<double>(g_bucketClip[b]) /
                                   static_cast<double>(g_bucketPix[b]),
                               meanSize, g_bucketFrames[b]);
        if (m > 0) used += static_cast<size_t>(m);
    }
    // The interval starts afresh: the next line judges the next stretch.
    memset(g_candPix, 0, sizeof(g_candPix));
    memset(g_candRej, 0, sizeof(g_candRej));
    memset(g_candClip, 0, sizeof(g_candClip));
    memset(g_candSize, 0, sizeof(g_candSize));
    memset(g_bucketPix, 0, sizeof(g_bucketPix));
    memset(g_bucketClip, 0, sizeof(g_bucketClip));
    memset(g_bucketSize, 0, sizeof(g_bucketSize));
    memset(g_bucketFrames, 0, sizeof(g_bucketFrames));
    g_intervalFrames = 0;
    return true;
}

bool temporalPassDlaaTotals(uint32_t* frames, double* avgMs, double* maxMs) {
    if (g_dlaaTreats == 0) return false;
    uint32_t evals = 0;
    if (!dlaaTotals(&evals, avgMs, maxMs)) return false;
    if (frames) *frames = g_dlaaTreats;
    return true;
}

void temporalPassShutdown() {
    dlaaShutdown();
    if (g_csMv) { g_csMv->Release(); g_csMv = nullptr; }
    if (g_treats > 0) {
        Log::get().note("temporal aa: %u eye-submits treated this session.",
                        g_treats);
    }
    for (EyeState& e : g_eye) releaseEye(e);
    for (Slot& q : g_slots) releaseSlot(q);
    if (g_statsUav) { g_statsUav->Release(); g_statsUav = nullptr; }
    if (g_stats) { g_stats->Release(); g_stats = nullptr; }
    if (g_samp) { g_samp->Release(); g_samp = nullptr; }
    if (g_cb) { g_cb->Release(); g_cb = nullptr; }
    if (g_cs) { g_cs->Release(); g_cs = nullptr; }
}

}  // namespace edvr

extern "C" __declspec(dllexport) void* edvrTemporalAa(
    void* srcTex, int eye, const float* bounds, const float* tanNow,
    const float* tanPrev, float jxNow, float jyNow, const float* deltaHead,
    const float* headTrans, const float* headTransSwapped, float nearZ,
    float farZ, float headDeg, int motion, float blend, float clampSigma,
    unsigned flags) {
    if (!srcTex || eye < 0 || eye > 1 || !tanNow) return nullptr;
    void* out = nullptr;
    edvr::guardedBudget(edvr::g_budget, [&] {
        out = edvr::temporalInner(srcTex, eye, bounds, tanNow, tanPrev, jxNow,
                                  jyNow, deltaHead, headTrans, headTransSwapped,
                                  nearZ, farZ, headDeg, motion, blend,
                                  clampSigma, flags);
    });
    return out;
}
