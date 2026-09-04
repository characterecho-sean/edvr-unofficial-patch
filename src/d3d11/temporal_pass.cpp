#include "temporal_pass.h"

#include <cmath>
#include <cstdio>
#include <cstring>

#include <windows.h>

#include <d3d11.h>
#include <cstdarg>

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
Texture2D<float> Z2 : register(t3);      // a cockpit or HUD layer's depth, the same way (unbound reads 0)
Texture2D<float> Z3 : register(t4);      // ...and a second layer's
SamplerState L : register(s0);           // bilinear, clamp
RWTexture2D<float4> O : register(u0);    // the output, region-sized, the game's format
RWTexture2D<float4> N : register(u1);    // the new history
RWStructuredBuffer<uint> Stats : register(u2);   // 0 rejected, 1 clipped, 2 the clips' size (luma/255, summed); then the same three per candidate, four of them; 15 pixels on the world path, 16 bright pixels, 17 bright pixels with no depth; 18-20 the registration probes on the world path (sum dx*100, sum dy*100, count) and 21-23 on the ship; 24-27 world pixels, world clipped, ship pixels, ship clipped; 28 pixels with a layer's depth, 29 bright pixels with a layer's depth; 30-32 the registration probes on the sky (the far plane: sum dx*100, sum dy*100, count); 33-34 the probes' sum resid.mv*100 and sum mv.mv*100 on the sky, 35-36 on the world with a depth, 37-38 on the ship
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
    float4 tvCam;       // xyz the translation term for the camera rows (the world path); w 1 = the world path is on
    float4 split;       // x the ship's radius in metres (nearer: the head's delta; farther and the far plane: the camera's); y the debug view (1 motion, 2 error, 3 depth); z a depth in metres for depthless pixels in a menu-like scene (0 off); w 1 = menu-like scene
    float4 hud;         // x an assumed depth in metres for bright text-like pixels that read far or beyond y metres (0 off); zw unused
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
)HLSL"
// (adjacent literals: MSVC caps one at 16 KB)
R"HLSL(
// The depth at a texel: the NEAREST of the scene's and the layers' (reversed
// Z, so the largest). HUD text has no depth of its own in the scene's
// target and borrowed the sky's where it sat over the canopy; the HUD pass
// writes its own target, and this is where it joins (2026-09-04, the
// 'Point Defence' observation: the letters over the frame registered, the
// letters over the glass warped).
// The HUD is drawn ON TOP of the cockpit without a depth test, so where
// its layer has a depth, that is the visible surface's -- even where the
// dashboard behind it is nearer. The first build took the nearest of
// all, and the text over the dashboard was reprojected at the dashboard's
// half metre instead of the HUD's metre and a half: a shimmer bounded by
// the dashboard's rounded silhouette (2026-09-04). So the layer wins
// wherever it has a value, and the scene's depth fills the rest.
// How many of the 3x3 around a texel are bright: a text stroke has three or
// more, a lone star one or two. The HUD's text has no depth anywhere in
// the game's targets (the depth view of 2026-09-04), so this is the only
// handle on it.
int brightAround(int2 c) {
    int n = 0;
    [unroll] for (int ty = -1; ty <= 1; ++ty) {
        [unroll] for (int tx = -1; tx <= 1; ++tx) {
            int2 hq = clamp(c + int2(tx, ty), int2(0, 0), size - 1);
            if (rgbToYcocg(S.Load(int3(region.xy + hq, 0)).rgb).x > 0.6) ++n;
        }
    }
    return n;
}
float zSceneAt(int2 q) { return Z.Load(int3(q, 0)); }
float zLayerAt(int2 q) { return max(Z2.Load(int3(q, 0)), Z3.Load(int3(q, 0))); }
float zAt(int2 q) {
    float l = zLayerAt(q);
    return l > 0.0 ? l : zSceneAt(q);
}
bool fetchHistoryT(float2 p, float3 r0, float3 r1, float3 r2, float3 tv,
                   bool useDepth, bool allowWorld, out uint world, out float2 mvOut,
                   out float3 hy) {
    float3 d;
    d.x = tanNow.x + (p.x + 0.5) / float(size.x) * (tanNow.y - tanNow.x);
    d.y = tanNow.w - (p.y + 0.5) / float(size.y) * (tanNow.w - tanNow.z);
    d.z = -1.0;
    float3 dp = float3(dot(r0, d), dot(r1, d), dot(r2, d));
    world = 0;
    mvOut = 0.0;
    // The world/ship split: the ship's own things (the cockpit, the hull)
    // move with the head's delta; everything farther than split.x metres,
    // and the far plane, moves with the game's CAMERA -- the head and the
    // ship together, the rows the instrument's candidate 2 reads -- which
    // is what a turning ship needs for its skybox and a station (the Pimax
    // flight of 2026-09-04 saw both smear; the review's F6). The split
    // needs a depth to classify by, so it runs only with one bound.
    bool worldOn = allowWorld && tvCam.w != 0.0 && split.x > 0.0 && knobs.y != 0.0;
    if (useDepth || worldOn) {
        // The NEAREST depth of the 3x3, not the pixel's own: at the edge of
        // a near thing against a far one the pixel's own depth is either,
        // and a history fetched by the far one at a text stroke's edge is
        // the "underwater" the second depth flight saw at rest. With the
        // nearest, the edge follows the thing in front, which is the
        // standard dilation every velocity-based filter does.
        float zs = 0.0, zl = 0.0;
        [unroll] for (int oy = -1; oy <= 1; ++oy) {
            [unroll] for (int ox = -1; ox <= 1; ++ox) {
                int2 q = clamp(int2(p) + int2(ox, oy), int2(0, 0), size - 1);
                zs = max(zs, zSceneAt(region.xy + q));
                zl = max(zl, zLayerAt(region.xy + q));
            }
        }
        float zr = zl > 0.0 ? zl : zs;
        float den = zr * (knobs.w - knobs.z) + knobs.z;
        bool far = zr <= 0.0 || den <= 0.0;
        float z = far ? 0.0 : knobs.z * knobs.w / den;
        // The HUD's text has no depth in any of the game's targets: drawn on
        // top, it borrows whatever is behind it -- the far plane in space, a
        // hangar wall docked -- and cannot follow the head's translation. An
        // assumed HUD distance for pixels that look like its text (bright,
        // three or more bright neighbours) and read far or beyond hud.y
        // metres; advanced.temporal_aa_hud_metres, off by default, since a
        // bright lamp or a lit panel far away pays the same price in reverse.
        if (hud.x > 0.0 && split.w == 0.0 && (far || z > hud.y) && brightAround(int2(p)) >= 3) {
            far = false;
            z = hud.x;
        }
        if (worldOn && (far || z > split.x)) {
            world = 1;
            dp = float3(dot(c2R0.xyz, d), dot(c2R1.xyz, d), dot(c2R2.xyz, d));
            if (!far) dp = dp * z + tvCam.xyz;
        } else if (useDepth && !far) {
            dp = dp * z + tv;
        } else if (useDepth && far && split.w != 0.0 && split.z > 0.0) {
            // A menu-like scene's depthless pixels (the main menu's hangar
            // wall reads no depth and reprojected as the far plane, so it
            // detached under head translation): an assumed depth, the
            // player's choice, better than infinity for a wall a few metres
            // off. advanced.temporal_aa_menu_metres.
            dp = dp * split.z + tv;
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
    // such offsets every frame is the blur that compounds. OFF by default
    // since 2026-09-04: the snap's error is NOT bounded by its threshold.
    // Each frame re-registers by the frame's delta, not by the accumulated
    // error, so motion the snap suppresses accumulates in the history to a
    // steady lag of blend / (1 - blend) times the suppressed motion, about
    // 0.7 px at the 0.9 blend for content drifting under the threshold --
    // where distant content sits under a slow ship turn -- and the lag
    // differs across the image (the review of 2026-09-04, F4). The rest
    // lock (experimental.shimmer_rest) holds the pose at rest, which is
    // what the snap was for. Off at 0.
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
    mvOut = pp - p;
    hy = rgbToYcocg(catmullRom((pp + 0.5) / float2(size), float2(size)).rgb);
    return true;
}
bool fetchHistory(float2 p, float3 r0, float3 r1, float3 r2, out float3 hy) {
    uint wd = 0;
    float2 mvd = 0.0;
    return fetchHistoryT(p, r0, r1, r2, float3(0.0, 0.0, 0.0), false, false, wd, mvd, hy);
}
// How far a clip moved the history, in luma, as a count of 1/255ths: a
// nudge on a text edge is a few, a history that landed somewhere else
// entirely is tens. Summed per reading, it separates the two where a
// count of clipped pixels cannot.
uint clipSize(float3 hc, float3 hy) {
    return uint(saturate(abs(hc.x - hy.x)) * 255.0 + 0.5);
}
)HLSL"
// (adjacent literals: MSVC caps one at 16 KB)
R"HLSL(
groupshared uint gCount[40];
// The motion vectors for a trained pass (DLAA): the same reprojection
// the history fetch does, written out instead of used -- the pixel's
// position last frame minus its position now, in render pixels, which
// is DLSS's convention with a scale of one (pinned on the desk by the
// conventions rig in tools/smoke, 2026-09-04). Off the image or behind
// the eye: no motion. The depth goes beside it, copied as the game wrote
// it (reversed-Z, told to the runtime as such). The world/ship split is
// the history fetch's, transcribed, and the counts feed the registration
// line the way main's do.
[numthreads(8, 8, 1)]
void mv(uint3 id : SV_DispatchThreadID, uint gi : SV_GroupIndex) {
    if (gi < 40) gCount[gi] = 0;
    GroupMemoryBarrierWithGroupSync();
    uint count[40] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    if (id.x < (uint)size.x && id.y < (uint)size.y) {
        float2 p = float2(id.xy);
        float3 d;
        d.x = tanNow.x + (p.x + 0.5) / float(size.x) * (tanNow.y - tanNow.x);
        d.y = tanNow.w - (p.y + 0.5) / float(size.y) * (tanNow.w - tanNow.z);
        d.z = -1.0;
        float3 dp = float3(dot(dR0.xyz, d), dot(dR1.xyz, d), dot(dR2.xyz, d));
        float zraw = zAt(region.xy + int2(p));
        if (knobs.y != 0.0) {
            float zs = 0.0, zl = 0.0;
            [unroll] for (int oy = -1; oy <= 1; ++oy) {
                [unroll] for (int ox = -1; ox <= 1; ++ox) {
                    int2 q = clamp(int2(p) + int2(ox, oy), int2(0, 0), size - 1);
                    zs = max(zs, zSceneAt(region.xy + q));
                zl = max(zl, zLayerAt(region.xy + q));
                }
            }
            float zr = zl > 0.0 ? zl : zs;
            float den = zr * (knobs.w - knobs.z) + knobs.z;
            bool far = zr <= 0.0 || den <= 0.0;
            float z = far ? 0.0 : knobs.z * knobs.w / den;
            if (hud.x > 0.0 && split.w == 0.0 && (far || z > hud.y) && brightAround(int2(p)) >= 3) {
                far = false;
                z = hud.x;
                // ...and the depth copy NVIDIA gets says the same
                zraw = knobs.z * (knobs.w - hud.x) / (hud.x * (knobs.w - knobs.z));
            }
            bool worldOn = tvCam.w != 0.0 && split.x > 0.0;
            if (worldOn && (far || z > split.x)) {
                count[15] = 1;
                dp = float3(dot(c2R0.xyz, d), dot(c2R1.xyz, d), dot(c2R2.xyz, d));
                if (!far) dp = dp * z + tvCam.xyz;
            } else if (!far) {
                dp = dp * z + tvUsed.xyz;
            } else if (split.w != 0.0 && split.z > 0.0) {
                dp = dp * split.z + tvUsed.xyz;   // the menu's assumed depth (fetchHistoryT says)
            }
            float luma = rgbToYcocg(S.Load(int3(region.xy + int2(p), 0)).rgb).x;
            if (luma > 0.6) {
                count[16] = 1;
                if (zraw <= 0.0) count[17] = 1;
            }
            if (zLayerAt(region.xy + int2(p)) > 0.0) {
                count[28] = 1;
                if (luma > 0.6) count[29] = 1;
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
        // The motion view on the trained path: painted into the output in
        // NVIDIA's place (the pass skips its evaluation that frame).
        if (split.y == 1.0) {
            O[id.xy] = float4(saturate(0.5 + motion.x / 16.0), saturate(0.5 + motion.y / 16.0),
                              count[15] != 0 ? 1.0 : 0.0, 1.0);
        } else if (split.y == 3.0) {
            float zl3 = zLayerAt(region.xy + int2(p));
            float zs3 = zSceneAt(region.xy + int2(p));
            float3 o3;
            if (knobs.y == 0.0) {
                o3 = float3(0.25, 0.0, 0.25);
            } else if (zl3 > 0.0) {
                o3 = float3(0.0, 1.0, 0.0);
            } else if (zs3 > 0.0) {
                float m3 = knobs.z * knobs.w / (zs3 * (knobs.w - knobs.z) + knobs.z);
                float g3 = saturate(1.0 - log2(max(m3, 0.5)) / 8.0);
                o3 = g3.xxx;
            } else {
                o3 = float3(1.0, 0.0, 1.0);
            }
            O[id.xy] = float4(o3, 1.0);
        }
        ZC[id.xy] = knobs.y != 0.0 ? zraw : 0.0;
    }
    [unroll] for (int k = 0; k < 40; ++k) {
        if (count[k] != 0) InterlockedAdd(gCount[k], count[k]);
    }
    GroupMemoryBarrierWithGroupSync();
    if (gi < 40) InterlockedAdd(Stats[gi], gCount[gi]);
}
)HLSL"
// (adjacent literals: MSVC caps one at 16 KB)
R"HLSL(
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID, uint gi : SV_GroupIndex) {
    if (gi < 40) gCount[gi] = 0;
    GroupMemoryBarrierWithGroupSync();
    uint count[40] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
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
        // The bright pixels, and the bright pixels with no depth behind
        // them: HUD text the game draws without a depth write takes the
        // rotation-only path and cannot register under head translation
        // (the review of 2026-09-04, H5, believed; this counts it).
        {
            float lumaC = rgbToYcocg(S.Load(int3(region.xy + ci, 0)).rgb).x;
            if (lumaC > 0.6) {
                count[16] = 1;
                if (knobs.y != 0.0 && zAt(region.xy + ci) <= 0.0) count[17] = 1;
            }
            // ...and how much of the image, and of its bright pixels, a
            // layer's depth covers: whether the HUD's text is in the layer.
            if (knobs.y != 0.0 && zLayerAt(region.xy + ci) > 0.0) {
                count[28] = 1;
                if (lumaC > 0.6) count[29] = 1;
            }
        }
        m1 /= msum;
        m2 /= msum;
        float3 sigma = sqrt(max(m2 - m1 * m1, 0.0));
        float3 boxMin = m1 - gamma * sigma;
        float3 boxMax = m1 + gamma * sigma;
        float3 outc = cur.rgb;
        bool used = false;
        uint worldTaken = 0;
        float2 mvUsed = 0.0;
        float errUsed = 0.0;
        if (haveHistory != 0) {
            float3 hy;
            if (fetchHistoryT(p, dR0.xyz, dR1.xyz, dR2.xyz, tvUsed.xyz,
                              knobs.y != 0.0 && tvUsed.w != 0.0, true, worldTaken, mvUsed, hy)) {
                if (worldTaken != 0) count[15] = 1;
                errUsed = saturate(abs(hy.x - rgbToYcocg(cur.rgb).x) * 4.0);
                float3 hc = clipToBox(boxMin, boxMax, hy);
                if (any(abs(hc - hy) > 1e-4)) {
                    count[1] = 1;
                    count[2] = clipSize(hc, hy);
                }
                // The used delta's clip share per class: the world path's
                // pixels and the ship's, since the two are registered by
                // different deltas and one number hid the other.
                if (worldTaken != 0) {
                    count[24] = 1;
                    if (count[1] != 0) count[25] = 1;
                } else {
                    count[26] = 1;
                    if (count[1] != 0) count[27] = 1;
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
                // Candidates 1 and 2 are the two readings of the game's view
                // rows composed with the head (2 the reading in use, 1 the
                // other), judged rotation-only so they are compared alike;
                // 3 is the head with depth as used for the ship's own pixels.
                float3 tvc = c == 1 ? tvCand.xyz : tvUsed.xyz;
                bool depthC = knobs.y != 0.0 && c == 3;
                float3 h;
                uint wc = 0;
                float2 mvc = 0.0;
                if (!fetchHistoryT(p, r0, r1, r2, tvc, depthC, false, wc, mvc, h)) {
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
        // The registration probes: on a 64-pixel grid, where the frame has
        // texture, the history's best match within 4 px of the predicted
        // position by a 5x5 luma SAD. The mean offset per class -- the
        // world path, the ship -- is the used reprojection's systematic
        // error in pixels, which no clip share can give: a steady offset
        // that follows the motion is a lag or a scale, noise averages to
        // zero, and the jitter's sub-pixel offset is in every probe and
        // averages out too. Signed: (+2, 0) means the content sat two
        // pixels further right in the history than the prediction said.
        if (used && (id.x & 63) == 16 && (id.y & 63) == 16 && id.x >= 8 && id.y >= 8 &&
            id.x + 8 < (uint)size.x && id.y + 8 < (uint)size.y) {
            float curL[25];
            float meanL = 0.0;
            int kk = 0;
            [unroll] for (int wy = -2; wy <= 2; ++wy) {
                [unroll] for (int wx = -2; wx <= 2; ++wx) {
                    curL[kk] = rgbToYcocg(S.Load(int3(region.xy + ci + int2(wx, wy), 0)).rgb).x;
                    meanL += curL[kk];
                    ++kk;
                }
            }
            meanL /= 25.0;
            float varL = 0.0;
            [unroll] for (int jj = 0; jj < 25; ++jj) varL += (curL[jj] - meanL) * (curL[jj] - meanL);
            // Three classes: the sky (the far plane, whose Milky Way is soft,
            // so its texture bar is lower), the world with a depth (a station,
            // whose own rotation is in no vector and shows here), the ship.
            const bool sky = worldTaken != 0 && knobs.y != 0.0 && zAt(region.xy + ci) <= 0.0;
            if (varL > (sky ? 0.0025 : 0.0225)) {
                float2 pp = p + mvUsed;
                int2 pq = int2(round(pp));
                float bestSad = 1e9;
                int2 best = int2(0, 0);
                for (int sy = -4; sy <= 4; ++sy) {
                    for (int sx = -4; sx <= 4; ++sx) {
                        float sad = 0.0;
                        int mm = 0;
                        [unroll] for (int wy2 = -2; wy2 <= 2; ++wy2) {
                            [unroll] for (int wx2 = -2; wx2 <= 2; ++wx2) {
                                int2 q = clamp(pq + int2(sx + wx2, sy + wy2), int2(0, 0), size - 1);
                                sad += abs(curL[mm] - rgbToYcocg(H.Load(int3(q, 0)).rgb).x);
                                ++mm;
                            }
                        }
                        if (sad < bestSad) {
                            bestSad = sad;
                            best = int2(sx, sy);
                        }
                    }
                }
                float2 resid = float2(best) + (float2(pq) - pp);
                uint base = sky ? 30u : (worldTaken != 0 ? 18u : 21u);
                InterlockedAdd(Stats[base], asuint(int(round(resid.x * 100.0))));
                InterlockedAdd(Stats[base + 1], asuint(int(round(resid.y * 100.0))));
                InterlockedAdd(Stats[base + 2], 1u);
                // The match against the prediction's own motion, for a
                // least-squares scale: with resid = k * mv the true motion
                // was (1 + k) times the vector, k = sum(resid.mv) /
                // sum(mv.mv) over the class. A signed mean cancels over a
                // head that turns both ways; this does not.
                uint base2 = sky ? 33u : (worldTaken != 0 ? 35u : 37u);
                InterlockedAdd(Stats[base2], asuint(int(round(dot(resid, mvUsed) * 100.0))));
                InterlockedAdd(Stats[base2 + 1], uint(round(dot(mvUsed, mvUsed) * 100.0)));
            }
        }
        if (!used) count[0] = 1;
        float3 o = saturate(outc);
        N[id.xy] = float4(o, 1.0);
        // The debug views (advanced.temporal_aa_debug): the history keeps
        // accumulating as normal, only what leaves changes. motion paints
        // the used reprojection -- +x red, +y green, around mid-grey with
        // 16 px to the rail -- and the world path in blue; error paints
        // how far the fetched history sat from this frame's sample, in
        // luma, four times over. A skybox that leads or trails its true
        // motion shows as a colour that disagrees with the ship's turn.
        if (split.y == 1.0) {
            o = float3(saturate(0.5 + mvUsed.x / 16.0), saturate(0.5 + mvUsed.y / 16.0),
                       worldTaken != 0 ? 1.0 : 0.0);
        } else if (split.y == 2.0) {
            o = errUsed.xxx;
        } else if (split.y == 3.0) {
            // The depth view: where each pixel's depth comes from -- a
            // layer's in green, the scene's in grey by distance (near
            // bright, log scale to 256 m), none in magenta, no depth
            // bound at all in dark purple. HUD text that is not green
            // has no depth of its own and reprojects by what is behind it.
            float zl3 = zLayerAt(region.xy + ci);
            float zs3 = zSceneAt(region.xy + ci);
            if (knobs.y == 0.0) {
                o = float3(0.25, 0.0, 0.25);
            } else if (zl3 > 0.0) {
                o = float3(0.0, 1.0, 0.0);
            } else if (zs3 > 0.0) {
                float m3 = knobs.z * knobs.w / (zs3 * (knobs.w - knobs.z) + knobs.z);
                float g3 = saturate(1.0 - log2(max(m3, 0.5)) / 8.0);
                o = g3.xxx;
            } else {
                o = float3(1.0, 0.0, 1.0);
            }
            // ...and the assumed HUD depth, where it applies, in yellow.
            if (hud.x > 0.0 && split.w == 0.0 && knobs.y != 0.0 && zl3 <= 0.0) {
                float mz = zs3 > 0.0 ? knobs.z * knobs.w / (zs3 * (knobs.w - knobs.z) + knobs.z) : 0.0;
                if ((zs3 <= 0.0 || mz > hud.y) && brightAround(ci) >= 3) o = float3(1.0, 1.0, 0.0);
            }
        }
        O[id.xy] = float4(o, cur.a);
    }
    // One atomic per group per counter, not per pixel.
    [unroll] for (int k = 0; k < 40; ++k) {
        if (count[k] != 0) InterlockedAdd(gCount[k], count[k]);
    }
    GroupMemoryBarrierWithGroupSync();
    if (gi < 40) InterlockedAdd(Stats[gi], gCount[gi]);
}
)HLSL";

// The cbuffer above, laid out to match: 432 bytes, twenty-seven 16-byte rows.
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
    float   tvCam[4];    // the camera rows' translation term, w 1 = world path on
    float   split[4];    // x the ship's radius in metres
    float   hud[4];      // x an assumed HUD depth in metres (0 off), y its far threshold
};
static_assert(sizeof(PassParams) == 432, "the cbuffer is twenty-seven 16-byte rows");

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
    // The cockpit and HUD layers' depth, up to two, the same way.
    void*                      layerRes[2] = {};
    ID3D11ShaderResourceView*  layerSrv[2] = {};
    // For the trained pass: the colour copied out typed, the motion
    // vectors and the depth copy it is fed, and its output.
    ID3D11Texture2D*           dlColour = nullptr;
    ID3D11Texture2D*           dlMv = nullptr;
    ID3D11UnorderedAccessView* dlMvUav = nullptr;
    ID3D11Texture2D*           dlDepth = nullptr;
    ID3D11UnorderedAccessView* dlDepthUav = nullptr;
    ID3D11Texture2D*           dlOut = nullptr;
    ID3D11UnorderedAccessView* dlOutUav = nullptr;   // the debug motion view paints here
    ID3D11Texture2D*           dlSubmit = nullptr;  // NVIDIA's frame copied into the game's own format: what goes out
    uint32_t                   dlW = 0, dlH = 0;
    uint32_t                   dlOutW = 0, dlOutH = 0;
    ID3D11Texture2D*           copyTex = nullptr;  // the copy-through, for a source that refuses a view
    ID3D11ShaderResourceView*  copySrv = nullptr;
    uint32_t                   copyW = 0, copyH = 0;
    DXGI_FORMAT                copyFmt = DXGI_FORMAT_UNKNOWN;

    ID3D11Texture2D*           hist[2] = {};       // ping-pong: read one, write the other
    ID3D11ShaderResourceView*  histSrv[2] = {};
    ID3D11UnorderedAccessView* histUav[2] = {};
    int                        histRead = 0;
    bool                       haveHistory = false;
    // The trained pass's continuity (NVIDIA's history), kept apart from
    // the pass's own. The review of 2026-09-04 (docs/review-motion-vectors-2026-09-04.md,
    // F1) found the reset flag keyed on haveHistory, which the trained
    // path never sets: NVIDIA was told "the scene changed completely" on
    // every frame and never accumulated a thing.
    bool                       dlHaveHistory = false;
    int64_t                    dlLastQpc = 0;   // the previous evaluation, for NVIDIA's frame delta
    // The headset's poses and this eye's offset, noted by the openvr half
    // before each treat (edvrTemporalAaNoteHead): the world path's
    // composition with the ship's camera rows needs them.
    float                      headPrev[12] = {};
    float                      headNow[12] = {};
    float                      eyeOff[3] = {};
    bool                       headNoted = false;

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
    for (int k = 0; k < 2; ++k) {
        if (e.layerSrv[k]) { e.layerSrv[k]->Release(); e.layerSrv[k] = nullptr; }
        e.layerRes[k] = nullptr;
    }
}
void releaseDl(EyeState& e) {
    if (e.dlMvUav) { e.dlMvUav->Release(); e.dlMvUav = nullptr; }
    if (e.dlDepthUav) { e.dlDepthUav->Release(); e.dlDepthUav = nullptr; }
    if (e.dlColour) { e.dlColour->Release(); e.dlColour = nullptr; }
    if (e.dlMv) { e.dlMv->Release(); e.dlMv = nullptr; }
    if (e.dlDepth) { e.dlDepth->Release(); e.dlDepth = nullptr; }
    if (e.dlOutUav) { e.dlOutUav->Release(); e.dlOutUav = nullptr; }
    if (e.dlOut) { e.dlOut->Release(); e.dlOut = nullptr; }
    if (e.dlSubmit) { e.dlSubmit->Release(); e.dlSubmit = nullptr; }
    e.dlW = e.dlH = 0;
    e.dlOutW = e.dlOutH = 0;
    e.dlHaveHistory = false;
    e.dlLastQpc = 0;
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
constexpr int kStatCount = 40;   // 39 used; a 160-byte buffer
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
uint64_t g_intervalPix = 0;         // pixels this interval, both paths
uint64_t g_worldPix = 0;            // ...of which the world path took
uint64_t g_brightPix = 0;           // bright pixels (luma over 0.6)
uint64_t g_brightNoDepthPix = 0;    // ...of which had no depth
double   g_camHeadDiffSum = 0.0;    // degrees: the camera's delta against the head's
double   g_camMoveSum = 0.0;        // metres: the camera's displacement a frame
uint32_t g_camFrames = 0;
uint32_t g_camDropRot = 0;          // frames whose camera delta was another camera's
uint32_t g_camDropMove = 0;         // frames whose camera translation was a jump
// The registration probes and the per-class clip shares, per interval
// (the shader says what they are).
int64_t     g_probeWorldDx = 0, g_probeWorldDy = 0;
uint64_t    g_probeWorldN = 0;
int64_t     g_probeShipDx = 0, g_probeShipDy = 0;
uint64_t    g_probeShipN = 0;
uint64_t    g_classWorldPix = 0, g_classWorldClip = 0;
uint64_t    g_classShipPix = 0, g_classShipClip = 0;
uint64_t    g_layerPix = 0, g_brightLayerPix = 0;   // pixels, and bright pixels, with a layer's depth
int64_t     g_probeSkyDx = 0, g_probeSkyDy = 0;
uint64_t    g_probeSkyN = 0;
int64_t     g_probeDot[3] = {};    // sum resid.mv * 100: sky, world with a depth, ship
uint64_t    g_probeMm[3] = {};     // sum mv.mv * 100, the same classes
// The rows' delta against the head's, per frame the world path had both:
// the residual rotation's size by head-speed bucket, its regression on
// the head's turn (a scale k: the rows turned (1 + k) times the head),
// per axis, and on the turn's change (a lead in frames).
double      g_rhN[3] = {}, g_rhSum[3] = {};
double      g_rhDot = 0.0, g_rhMm = 0.0;
double      g_rhDotAx[3] = {}, g_rhMmAx[3] = {};
double      g_rhDotLag = 0.0, g_rhMmLag = 0.0;
float       g_omegaPrev[3] = {};
bool        g_omegaPrevValid = false;
// The chooser's ambiguity: frames on which a second continuous reading
// differed from the chosen one, and how far apart they sat.
uint32_t    g_chooseMulti = 0;
// The rows' translation against the head's, per axis, on frames the ship
// stood still (under 2 cm in the rows): the sign says whether the frame
// change is the z flip (+1, +1, +1 after it) or a half turn (-1, -1, -1).
double      g_tvDot[3] = {}, g_tvMm[3] = {};
uint32_t    g_tvFrames = 0;
// The previous frame's rows written again this frame (the game's own
// last-view block): skipped by the chooser, counted here.
uint32_t    g_twinFrames = 0;
double      g_chooseSpreadSum = 0.0, g_chooseSpreadMax = 0.0;

// A rotation as a small vector (degrees about x, y, z): the skew part,
// scaled from sin to the angle. Exact enough under a few degrees.
void temporalSmallRotVecDeg(const float R[9], float v[3]) {
    const float deg = temporalRotationAngleDeg(R);
    const float rad = deg * 3.14159265f / 180.0f;
    const float s = sinf(rad);
    const float scale = (s > 1e-6f ? rad / s : 1.0f) * 0.5f * (180.0f / 3.14159265f);
    v[0] = (R[7] - R[5]) * scale;
    v[1] = (R[2] - R[6]) * scale;
    v[2] = (R[3] - R[1]) * scale;
}
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
                g_intervalPix += q.pixels;
                g_worldPix += v[15];
                g_brightPix += v[16];
                g_brightNoDepthPix += v[17];
                g_probeWorldDx += static_cast<int32_t>(v[18]);
                g_probeWorldDy += static_cast<int32_t>(v[19]);
                g_probeWorldN += v[20];
                g_probeShipDx += static_cast<int32_t>(v[21]);
                g_probeShipDy += static_cast<int32_t>(v[22]);
                g_probeShipN += v[23];
                g_classWorldPix += v[24];
                g_classWorldClip += v[25];
                g_classShipPix += v[26];
                g_classShipClip += v[27];
                g_layerPix += v[28];
                g_brightLayerPix += v[29];
                g_probeSkyDx += static_cast<int32_t>(v[30]);
                g_probeSkyDy += static_cast<int32_t>(v[31]);
                g_probeSkyN += v[32];
                for (int c = 0; c < 3; ++c) {
                    g_probeDot[c] += static_cast<int32_t>(v[33 + c * 2]);
                    g_probeMm[c] += v[34 + c * 2];
                }
                ++g_intervalFrames;
                if (q.hadHistory) {
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
bool                       g_dlssNoted = false;
bool                       g_dlaaFailNoted = false;
bool                       g_trainedNoted = false;   // the first trained frame's line
int                        g_layersNoted = -1;       // how many depth layers the log last named
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
float    g_snapPx = 0.0f;          // advanced.temporal_aa_snap: the rest snap, pixels (0 = off, the default since 2026-09-04)
float    g_shipMetres = 100.0f;    // advanced.temporal_aa_ship_metres: the world/ship split (0 off)
int      g_debugMode = 0;          // advanced.temporal_aa_debug: 0 off, 1 motion, 2 error
float    g_menuMetres = 0.0f;      // advanced.temporal_aa_menu_metres: a depth for depthless pixels in a menu-like scene
float    g_hudMetres = 0.0f;       // advanced.temporal_aa_hud_metres: an assumed depth for bright text-like pixels with none (0 off)
bool     g_depthLayers = false;    // advanced.temporal_aa_depth_layers: the extra depth targets (off: the only one found was the ship model's)
int      g_rowsFollow = 0;         // +1 a frame the rows turn with the head, -4 a frame they do not; the world path needs >= 0
bool     g_rowsFollowNoted = false;
bool     g_warmNoted = false;

// The camera capture: a ring of the last writes of every scene-block-sized
// buffer the game maps (the object, the rows, the frame, the order), the
// object bound at this frame's first scene draw, the rows CHOSEN for the
// frame (chooseCameraRows), and last frame's.
struct RowsWrite {
    const void* buf = nullptr;
    float       rows[12] = {};
    uint32_t    frame = 0;
    uint32_t    seq = 0;
    bool        valid = false;
};
// 256: in space the game writes the block over a hundred times a frame
// (114 measured 2026-09-04), and a ring of 48 had lost the frame's early
// writes -- the eyes' among them, drawn before the reflections -- by
// the time the frame was chosen.
constexpr int kRowsRing = 256;
RowsWrite   g_rowsRing[kRowsRing];
uint32_t    g_rowsSeq = 0;           // writes ever, the ring's clock
uint32_t    g_rowsFrame = 0;         // bumped each boundary
const void* g_boundBuf = nullptr;    // the object bound at this frame's first scene draw
bool        g_boundSeen = false;
uint32_t    g_rowsWrites = 0;        // writes this frame
uint64_t    g_rowsWritesSum = 0;     // ...summed over the interval
uint32_t    g_rowsFramesSum = 0;
uint64_t    g_candSumCount = 0;      // this frame's candidate writes, summed
uint32_t    g_chooseBound = 0;       // frames whose chosen rows were the bound object's
uint32_t    g_chooseOther = 0;       // ...another object's, by continuity
uint32_t    g_chooseResync = 0;      // ...nothing followed last frame's: the latest taken
uint32_t    g_chooseNone = 0;        // ...no write this frame at all
bool        g_chosenThisFrame = false;
int         g_latchSlotVs = -1;      // where the bound block was found, for the log
int         g_latchSlotPs = -1;
float       g_lastGoodC[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};   // the last accepted ship delta
float       g_lastGoodTv[3] = {};                            // ...and its translation term
bool        g_lastGoodValid = false;
uint32_t    g_camCarried = 0;        // frames the ship's delta was carried over a drop
uint32_t    g_camCarriedJump = 0;    // ...of which carried a translation over 50 m: zero by construction
float    g_curRows[12] = {};
bool     g_curValid = false;
bool     g_curLatched = false;
float    g_prevRows[12] = {};
bool     g_prevValid = false;
uint32_t g_camPairs = 0;
bool     g_camNoted = false;

// The frame's camera rows, chosen once per frame at its first treat from
// the frame's writes: the one that FOLLOWS last frame's chosen rows within
// 3 degrees in absolute orientation (a ship turns under 2 a frame; a
// reflection face's or a shadow cascade's camera sits tens away), the
// bound object's preferred, else the latest such write. With nothing
// continuous (the first frame, a cut) the bound object's latest write,
// else the frame's latest. The bound block at the first scene draw held
// a reflection face's camera on half the frames in space (2026-09-04):
// the game draws into the scene's depth before it rewrites the block.
void chooseCameraRows() {
    if (g_chosenThisFrame) return;
    g_chosenThisFrame = true;
    g_curValid = false;
    int bestIdx = -1, fallIdx = -1;
    uint32_t bestSeq = 0, fallSeq = 0;
    bool bestBound = false, fallBound = false;
    uint32_t count = 0;
    int contIdx[kRowsRing];
    int contN = 0;
    int twinIdx = -1;
    uint32_t twinSeq = 0;
    bool twinBound = false;
    float rpT[9] = {};
    if (g_prevValid) {
        float rp[9];
        temporalRot3Of34(g_prevRows, rp);
        temporalTranspose3(rp, rpT);
    }
    for (int i = 0; i < kRowsRing; ++i) {
        const RowsWrite& w = g_rowsRing[i];
        if (!w.valid || w.frame != g_rowsFrame) continue;
        ++count;
        const bool bound = g_boundSeen && w.buf == g_boundBuf;
        bool continuous = false;
        if (g_prevValid) {
            float rn[9], d[9];
            temporalRot3Of34(w.rows, rn);
            temporalMul3(rpT, rn, d);
            continuous = temporalRotationAngleDeg(d) < 3.0f;
        }
        if (continuous) {
            // A write identical to last frame's chosen rows is the game's
            // own last-view block (nearly every frame in space carried one,
            // a head-turn's angle from the current; 2026-09-04): kept only
            // as the fallback, for a camera that truly stood still.
            if (g_prevValid && memcmp(w.rows, g_prevRows, sizeof(w.rows)) == 0) {
                if (twinIdx < 0 || w.seq > twinSeq) { twinIdx = i; twinSeq = w.seq; twinBound = bound; }
                continue;
            }
            // The BOUND object's latest continuous write, else the latest
            // continuous write of any object. The bound object is the scene
            // camera's by construction (the latch fires at the frame's first
            // draw into the scene pair's depth, depth_probe.cpp), and within
            // one object the frame's last view matrix is the one the eyes
            // were drawn with (an earlier write of the same frame is a staler
            // prediction of the same head). Latest-of-any-object (6677fca)
            // took another block's write on half the frames of every
            // supercruise and arrival interval of 2026-09-04, and the rows
            // then turned a quarter to a half of the head, lagging: a stale
            // camera within three degrees, written after the scene's own.
            // Steady space flight never showed it, since the bound block's
            // write was the latest there (docs/review-temporal-far-warp-
            // darkness-2026-09-04.md, F2).
            const bool better = bestIdx < 0 || (bound && !bestBound) ||
                                (bound == bestBound && w.seq > bestSeq);
            if (better) { bestIdx = i; bestSeq = w.seq; bestBound = bound; }
            contIdx[contN++] = i;
        }
        const bool fbetter = fallIdx < 0 || (bound && !fallBound) ||
                             (bound == fallBound && w.seq > fallSeq);
        if (fbetter) { fallIdx = i; fallSeq = w.seq; fallBound = bound; }
    }
    g_candSumCount += count;
    if (twinIdx >= 0) {
        if (bestIdx >= 0) ++g_twinFrames;
        else { bestIdx = twinIdx; bestSeq = twinSeq; bestBound = twinBound; }
    }
    // The ambiguity: another continuous write whose rows differ from the
    // chosen (the same matrix written again is no ambiguity). Two cameras
    // within three degrees of each other -- the other eye on canted
    // panels, a pass with a stale view -- would alternate the choice and
    // put their difference into the delta.
    if (bestIdx >= 0) {
        float cT[9], cr[9];
        temporalRot3Of34(g_rowsRing[bestIdx].rows, cr);
        temporalTranspose3(cr, cT);
        float spread = 0.0f;
        bool multi = false;
        for (int k = 0; k < contN; ++k) {
            const int i = contIdx[k];
            if (i == bestIdx) continue;
            if (memcmp(g_rowsRing[i].rows, g_rowsRing[bestIdx].rows, sizeof(float) * 12) == 0) continue;
            float on[9], d[9];
            temporalRot3Of34(g_rowsRing[i].rows, on);
            temporalMul3(cT, on, d);
            const float a = temporalRotationAngleDeg(d);
            multi = true;
            if (a > spread) spread = a;
        }
        if (multi) {
            ++g_chooseMulti;
            g_chooseSpreadSum += spread;
            if (spread > g_chooseSpreadMax) g_chooseSpreadMax = spread;
        }
    }
    int pick = bestIdx;
    if (pick >= 0) {
        if (bestBound) ++g_chooseBound; else ++g_chooseOther;
    } else if (fallIdx >= 0) {
        pick = fallIdx;
        ++g_chooseResync;
    } else {
        ++g_chooseNone;
    }
    if (pick >= 0) {
        memcpy(g_curRows, g_rowsRing[pick].rows, sizeof(g_curRows));
        g_curValid = true;
    }
}

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
                    unsigned outW, unsigned outH, unsigned flags) {
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
    // A view typed to the depth channel over one of the game's depth
    // textures, or null when its format has no such view.
    auto depthViewOf = [&](ID3D11Texture2D* tex) -> ID3D11ShaderResourceView* {
        D3D11_TEXTURE2D_DESC dd{};
        tex->GetDesc(&dd);
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
        if (rf == DXGI_FORMAT_UNKNOWN || !(dd.BindFlags & D3D11_BIND_SHADER_RESOURCE)) return nullptr;
        D3D11_SHADER_RESOURCE_VIEW_DESC vd{};
        vd.Format = rf;
        vd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        vd.Texture2D.MipLevels = 1;
        ID3D11ShaderResourceView* v = nullptr;
        if (FAILED(dev->CreateShaderResourceView(tex, &vd, &v))) return nullptr;
        return v;
    };
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
            // The cockpit and HUD layers' depth beside it, up to two pairs,
            // keyed on the texture like the scene's; the shader takes the
            // nearest of all three at every read.
            ID3D11Texture2D* ltex[2] = {};
            // Off by default since 2026-09-04: the only layer the census ever
            // qualified was a head-locked render of the ship's own model (the
            // HUD's schematic camera), whose silhouette then registered the
            // text inside it and nothing outside. The HUD's text writes no
            // depth anywhere; advanced.temporal_aa_hud_metres is the lever.
            const int nLayers = g_depthLayers ? depthProbeLayerDepths(sd.Width, sd.Height, eye, ltex, 2) : 0;
            for (int k = 0; k < 2; ++k) {
                if (k < nLayers && ltex[k]) {
                    if (e.layerRes[k] != static_cast<void*>(ltex[k]) || !e.layerSrv[k]) {
                        if (e.layerSrv[k]) { e.layerSrv[k]->Release(); e.layerSrv[k] = nullptr; }
                        e.layerRes[k] = nullptr;
                        e.layerSrv[k] = depthViewOf(ltex[k]);
                        if (e.layerSrv[k]) e.layerRes[k] = ltex[k];
                    }
                } else if (e.layerSrv[k]) {
                    e.layerSrv[k]->Release();
                    e.layerSrv[k] = nullptr;
                    e.layerRes[k] = nullptr;
                }
            }
            if (eye == 0) {
                const int have = (e.layerSrv[0] ? 1 : 0) + (e.layerSrv[1] ? 1 : 0);
                if (have != g_layersNoted) {
                    g_layersNoted = have;
                    Log::get().note(
                        "temporal aa: %d cockpit/HUD depth layer(s) join the scene's depth -- "
                        "the nearest of all is what every pixel reprojects by, so HUD text over "
                        "the canopy takes its own depth instead of the sky's (the registration "
                        "line's 'bright pixels with no depth' says what is left).",
                        have);
                }
            }
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
        if (flags & 1u) {
            e.haveHistory = false;
            e.dlHaveHistory = false;
        }

        // This frame's camera rows, chosen from the frame's writes (once).
        chooseCameraRows();
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
        // The world path's delta and translation term, from the game's view
        // rows alone. The rows are the FULL view -- the headset's pose is in
        // them -- stored view->world. The motion view of 2026-09-04 showed
        // the world standing still under a head turn while the rows were
        // read world->view and composed with the head (the head cancelled),
        // and moving twice the head's turn under the other reading with the
        // same composition (the head doubled); a still ship's delta is the
        // identity, so the docked figure could not tell, and the earlier
        // 'ship camera without the head' reading of a 0.1 to 0.26 deg/frame
        // docked residual was twice a slow head's rate, not the rate. So:
        // no composition. For rows [R | c] (view->world, c the eye's place
        // in the world) a point P now was, last frame, at
        //   W P + tv,   W = R_p^T R_n,   tv = R_p^T (c_n - c_p)
        // (advanced.temporal_aa_view_transpose = 1, the default); read as
        // world->view [S | t] instead (= 0), W = S_p S_n^T and tv = t_p -
        // W t_n. The reading not in use is the instrument's candidate 1.
        // Docked, W must equal the head's delta whichever way the head
        // turns -- a real check now, since the rows carry the head.
        float tvCam[3] = {0.0f, 0.0f, 0.0f};
        float worldDelta[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
        bool worldValid = false;
        auto worldFromRows = [](const float prev[12], const float now[12], bool transposed,
                                float W[9], float tv[3], float camMove[3]) {
            float Rp[9], Rn[9], RpT[9], RnT[9];
            temporalRot3Of34(prev, Rp);
            temporalRot3Of34(now, Rn);
            temporalTranspose3(Rp, RpT);
            temporalTranspose3(Rn, RnT);
            const float colP[3] = {prev[3], prev[7], prev[11]};
            const float colN[3] = {now[3], now[7], now[11]};
            if (transposed) {
                temporalMul3(RpT, Rn, W);
                const float dc[3] = {colN[0] - colP[0], colN[1] - colP[1], colN[2] - colP[2]};
                temporalApply3(RpT, dc, tv);
                for (int i = 0; i < 3; ++i) camMove[i] = dc[i];
            } else {
                temporalMul3(Rp, RnT, W);
                float wt[3], cp[3], cn[3];
                temporalApply3(W, colN, wt);
                for (int i = 0; i < 3; ++i) tv[i] = colP[i] - wt[i];
                temporalApply3(RpT, colP, cp);
                temporalApply3(RnT, colN, cn);
                for (int i = 0; i < 3; ++i) camMove[i] = cp[i] - cn[i];
            }
            // The game's view space runs z forward (DirectX), the runtime's
            // eye space z back. A rotation read in the one and applied in
            // the other has its pitch and yaw reversed and its roll kept,
            // which is exactly what the regression measured: over a dozen
            // intervals in space the rows turned -1 times the head about x
            // and y and +1 about z (k = -2, -2, 0; 2026-09-04), and the far
            // plane, on this delta alone, moved the wrong way by the head's
            // whole turn -- the sky's smear, and the station's under a head
            // turn, both gone with the world path off. Conjugating by the
            // z flip carries the delta into the eye's frame; the translation
            // term takes the same flip (a reflection, not a half turn: the
            // still-ship regression on the third line says which).
            W[2] = -W[2];
            W[5] = -W[5];
            W[6] = -W[6];
            W[7] = -W[7];
            tv[2] = -tv[2];
        };
        if (candValid[2]) {
            float camMove[3], otherW[9], otherTv[3], otherMove[3];
            worldFromRows(g_prevRows, g_curRows, g_viewTransposed, worldDelta, tvCam, camMove);
            worldFromRows(g_prevRows, g_curRows, !g_viewTransposed, otherW, otherTv, otherMove);
            memcpy(cand[2], worldDelta, sizeof(worldDelta));
            memcpy(cand[1], otherW, sizeof(otherW));
            candValid[1] = true;
            worldValid = true;
            const double move = sqrt(static_cast<double>(camMove[0]) * camMove[0] +
                                     static_cast<double>(camMove[1]) * camMove[1] +
                                     static_cast<double>(camMove[2]) * camMove[2]);
            g_camMoveSum += move;
            float diffDeg = 0.0f;
            if (deltaHead) {
                float ht[9], diff[9];
                temporalTranspose3(deltaHead, ht);
                temporalMul3(worldDelta, ht, diff);
                diffDeg = temporalRotationAngleDeg(diff);
                g_camHeadDiffSum += diffDeg;
                // The residual against the head's turn, on the frames the
                // delta is accepted: its size by head speed (a still head
                // with a residual is noise between the two pose streams; one
                // that grows with the speed is a scale or a lag), and the
                // regressions that name the scale and the lead. The far
                // plane is on this delta alone, and the sky probes read a
                // steady fifth of a pixel off it in space (2026-09-04).
                if (diffDeg <= 3.0f) {
                    float rv[3], hv[3];
                    temporalSmallRotVecDeg(diff, rv);
                    temporalSmallRotVecDeg(deltaHead, hv);
                    const int b = headDeg < kStillDeg ? 0 : (headDeg < kSlowDeg ? 1 : 2);
                    g_rhN[b] += 1.0;
                    g_rhSum[b] += diffDeg;
                    for (int i = 0; i < 3; ++i) {
                        g_rhDot += static_cast<double>(rv[i]) * hv[i];
                        g_rhMm += static_cast<double>(hv[i]) * hv[i];
                        g_rhDotAx[i] += static_cast<double>(rv[i]) * hv[i];
                        g_rhMmAx[i] += static_cast<double>(hv[i]) * hv[i];
                    }
                    if (g_omegaPrevValid) {
                        for (int i = 0; i < 3; ++i) {
                            const double dw = static_cast<double>(hv[i]) - g_omegaPrev[i];
                            g_rhDotLag += rv[i] * dw;
                            g_rhMmLag += dw * dw;
                        }
                    }
                    memcpy(g_omegaPrev, hv, sizeof(g_omegaPrev));
                    g_omegaPrevValid = true;
                }
            }
            if (headTrans && diffDeg <= 3.0f && move < 0.02) {
                for (int i = 0; i < 3; ++i) {
                    g_tvDot[i] += static_cast<double>(tvCam[i]) * headTrans[i];
                    g_tvMm[i] += static_cast<double>(headTrans[i]) * headTrans[i];
                }
                ++g_tvFrames;
            }
            ++g_camFrames;
            // Do the rows follow the head at all? In the cockpit they carry it;
            // at the main menu the camera is the menu's and the head is applied
            // elsewhere, so the rows stand still while the head turns, and a
            // world path built on them held the hangar's far wall still under
            // a head turn (2026-09-04, at 40 m; not at 100 m, which put the
            // wall on the head's path). A turning head with rows that turn
            // less than a third as much is that case; the score keeps the
            // path down until the rows follow again for a while.
            if (headDeg > 0.1f) {
                const float rowsDeg = temporalRotationAngleDeg(worldDelta);
                if (rowsDeg < 0.3f * headDeg) {
                    g_rowsFollow = g_rowsFollow > -30 ? g_rowsFollow - 4 : -30;
                    if (g_rowsFollow < 0 && !g_rowsFollowNoted) {
                        g_rowsFollowNoted = true;
                        Log::get().note(
                            "temporal aa: the game's view rows do not follow the head here (the "
                            "menu's camera?) -- the world path stands down until they do.");
                    }
                } else {
                    g_rowsFollow = g_rowsFollow < 30 ? g_rowsFollow + 1 : 30;
                    if (g_rowsFollow >= 0 && g_rowsFollowNoted) {
                        g_rowsFollowNoted = false;
                        Log::get().note("temporal aa: the game's view rows follow the head again -- "
                                        "the world path is back.");
                    }
                }
            }
            // Plausibility, per frame: the rows' delta is the head's plus the
            // ship's turn, and no ship turns 270 degrees a second; a delta
            // beyond 3 degrees from the head's is another camera's rows or
            // a stale latch, and last frame's accepted delta is carried in
            // its place (a far better guess than the head alone, which
            // smeared the world on every dropped frame). A jump over 50 m
            // is the floating origin moving: only the translation is dropped.
            // The jump is dropped BEFORE the last-good store, so a jump never
            // becomes the translation a later dropped frame carries: stored
            // first, a jump of hundreds of metres to tens of kilometres was
            // carried into the next dropped frame and moved every pixel with
            // a depth on the world path by it for one frame (the review of
            // 2026-09-04, F3). A jump frame keeps the last plausible
            // translation as its last-good, and a carried figure over 50 m is
            // counted so the invariant has a witness on the line.
            const bool jump = move >= 50.0;
            if (jump) {
                for (int i = 0; i < 3; ++i) tvCam[i] = 0.0f;
                ++g_camDropMove;
            }
            if (diffDeg > 3.0f) {
                ++g_camDropRot;
                if (g_lastGoodValid) {
                    memcpy(worldDelta, g_lastGoodC, sizeof(worldDelta));
                    memcpy(tvCam, g_lastGoodTv, sizeof(tvCam));
                    memcpy(cand[2], worldDelta, sizeof(worldDelta));
                    ++g_camCarried;
                    const double carried = sqrt(static_cast<double>(tvCam[0]) * tvCam[0] +
                                                static_cast<double>(tvCam[1]) * tvCam[1] +
                                                static_cast<double>(tvCam[2]) * tvCam[2]);
                    if (carried >= 50.0) ++g_camCarriedJump;
                } else {
                    candValid[2] = false;
                    worldValid = false;
                }
            } else {
                memcpy(g_lastGoodC, worldDelta, sizeof(g_lastGoodC));
                if (!jump) memcpy(g_lastGoodTv, tvCam, sizeof(g_lastGoodTv));
                g_lastGoodValid = true;
            }
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
        // The trained path's continuity is NVIDIA's, not the pass's: its
        // motion vectors need last frame's frustum whenever that history
        // continues. Keyed on haveHistory, which the trained path never
        // set, the vectors described the wrong previous frustum across a
        // guard re-stage or a resolution change (the review's F7).
        const bool trainedWanted = (flags & 2u) != 0;
        const bool useTanPrev =
            useHistory || (trainedWanted && e.dlHaveHistory && haveDelta && tanPrev);
        memcpy(p.tanPrev, useTanPrev ? tanPrev : tanNow, sizeof(p.tanPrev));
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
        // The world/ship split (the shader says what it is): under the
        // depth motion, with a depth bound and both frames' camera rows
        // read, in the rows' measured convention (world->view; the
        // transposed reading has no translation column to trust).
        // ...and only in a REAL scene: fifty draws into the scene pair a
        // frame. The main menu's backdrop is a pre-rendered image at the far
        // plane drawn with one or two, and its camera does not follow the
        // head, so the world path detached its hangar wall (2026-09-04).
        const uint32_t sceneDraws = depthProbeSceneDraws();
        const bool worldOn = depthMotion && haveDepth && g_shipMetres > 0.0f &&
                             candValid[2] && worldValid && sceneDraws >= 50u &&
                             g_rowsFollow >= 0;
        for (int i = 0; i < 3; ++i) p.tvCam[i] = worldOn ? tvCam[i] : 0.0f;
        p.tvCam[3] = worldOn ? 1.0f : 0.0f;
        p.split[0] = g_shipMetres;
        p.split[1] = static_cast<float>(g_debugMode);
        p.split[2] = g_menuMetres;
        p.split[3] = (haveDepth && sceneDraws < 50u) ? 1.0f : 0.0f;
        p.hud[0] = g_hudMetres;
        p.hud[1] = 8.0f;   // beyond this a bright text-like pixel is taken for HUD text over the scene
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
        ID3D11ShaderResourceView* savedSrv[5] = {};
        ID3D11UnorderedAccessView* savedUav[3] = {};
        ID3D11Buffer* savedCb = nullptr;
        ID3D11SamplerState* savedSamp = nullptr;
        ctx->CSGetShader(&savedCs, nullptr, nullptr);
        ctx->CSGetShaderResources(0, 5, savedSrv);
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
        // The price and the stats slot, both paths: the trained path's own
        // work (the colour copy and the motion-vector dispatch) is timed
        // too, and its counts (the world path, the bright pixels without
        // depth) come back through the same staging buffer.
        const int qs = acquireSlot(dev);
        if (qs >= 0) {
            ctx->Begin(g_slots[qs].disjoint);
            ctx->End(g_slots[qs].begin);
        }
        const UINT zeros[4] = {0, 0, 0, 0};
        ctx->ClearUnorderedAccessViewUint(g_statsUav, zeros);

        bool usedDlaa = false;
        // The trained path copies the colour into R8G8B8A8_UNORM, which is
        // only legal within that family (the review of 2026-09-04, F9): any
        // other family runs the pass's own history and says so once.
        if ((flags & 2u) != 0 && fmtIndex != 0 && !g_dlaaFailNoted) {
            g_dlaaFailNoted = true;
            Log::get().note(
                "temporal aa: dlaa was asked for, but the game submits %s and NVIDIA is "
                "handed R8G8B8A8, a different family. The pass's own history runs instead.",
                formatName(sd.Format));
        }
        if ((flags & 2u) != 0 && fmtIndex == 0) {
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
                // The size to come back at: the frame's own, or the larger
                // one asked for (DLSS proper).
                const uint32_t oW = (outW && outH && (outW != w || outH != h)) ? outW : w;
                const uint32_t oH = (outW && outH && (outW != w || outH != h)) ? outH : h;
                bool made = g_csMv != nullptr;
                if (made && (!e.dlOut || e.dlW != w || e.dlH != h || e.dlOutW != oW ||
                             e.dlOutH != oH)) {
                    releaseDl(e);
                    made = makeTex(dev, w, h, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM,
                                   D3D11_BIND_SHADER_RESOURCE, &e.dlColour, nullptr, nullptr) &&
                           makeTex(dev, w, h, DXGI_FORMAT_R16G16_FLOAT, DXGI_FORMAT_R16G16_FLOAT,
                                   D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS,
                                   &e.dlMv, nullptr, &e.dlMvUav) &&
                           makeTex(dev, w, h, DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R32_FLOAT,
                                   D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS,
                                   &e.dlDepth, nullptr, &e.dlDepthUav) &&
                           makeTex(dev, oW, oH, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM,
                                   D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS,
                                   &e.dlOut, nullptr, &e.dlOutUav) &&
                           // ...and the texture that goes OUT, in the game's own format
                           // (typeless when the game's is), so the compositor is told the
                           // same kind of texture on every path. NVIDIA writes a typed
                           // UNORM, which the own pass never hands out: a typed texture
                           // admits only a typed view at the compositor where a typeless
                           // one admits an sRGB view, and that was the one uniform
                           // brightness change the trained path could have made (the
                           // review of 2026-09-04, D1).
                           makeTex(dev, oW, oH, sd.Format, viewFmt, D3D11_BIND_SHADER_RESOURCE,
                                   &e.dlSubmit, nullptr, nullptr);
                    if (made) {
                        e.dlW = w;
                        e.dlH = h;
                        e.dlOutW = oW;
                        e.dlOutH = oH;
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
                    ID3D11ShaderResourceView* nullSrvM[5] = {};
                    ID3D11UnorderedAccessView* nullUavM[5] = {};
                    ctx->CSSetShaderResources(0, 5, nullSrvM);
                    ctx->CSSetUnorderedAccessViews(0, 5, nullUavM, nullptr);
                    ctx->CSSetShader(g_csMv, nullptr, 0);
                    ID3D11ShaderResourceView* srvsM[5] = {inSrv, e.histSrv[e.histRead], depthSrv,
                                                          e.layerSrv[0], e.layerSrv[1]};
                    ID3D11UnorderedAccessView* uavsM[5] = {(g_debugMode == 1 || g_debugMode == 3) ? e.dlOutUav : nullptr,
                                                           nullptr, g_statsUav, e.dlMvUav,
                                                           e.dlDepthUav};
                    ID3D11Buffer* cbM = g_cb;
                    ID3D11SamplerState* smpM = g_samp;
                    ctx->CSSetShaderResources(0, 5, srvsM);
                    ctx->CSSetUnorderedAccessViews(0, 5, uavsM, nullptr);
                    ctx->CSSetConstantBuffers(0, 1, &cbM);
                    ctx->CSSetSamplers(0, 1, &smpM);
                    ctx->Dispatch((w + 7) / 8, (h + 7) / 8, 1);
                    ctx->CSSetShaderResources(0, 5, nullSrvM);
                    ctx->CSSetUnorderedAccessViews(0, 5, nullUavM, nullptr);
                    // NVIDIA's evaluation. Its history restarts only when it is
                    // broken: this eye's first frame, a withhold (flags bit 0),
                    // rebuilt textures, or a frame the pass's own history ran in
                    // between -- never every frame (the review's F1, 2026-09-04).
                    const bool resetHist = (flags & 1u) != 0 || !e.dlHaveHistory;
                    // The time since this eye's previous evaluation, which the
                    // runtime uses to weigh motion against frame rate; zero on
                    // a restart, when there is no previous frame to measure to.
                    LARGE_INTEGER qNow{}, qFreq{};
                    QueryPerformanceCounter(&qNow);
                    QueryPerformanceFrequency(&qFreq);
                    float frameMs = 0.0f;
                    if (!resetHist && e.dlLastQpc && qFreq.QuadPart > 0) {
                        frameMs = static_cast<float>(
                            static_cast<double>(qNow.QuadPart - e.dlLastQpc) * 1000.0 /
                            static_cast<double>(qFreq.QuadPart));
                        if (frameMs < 1.0f || frameMs > 100.0f) frameMs = 0.0f;
                    }
                    e.dlLastQpc = qNow.QuadPart;
                    if (g_debugMode == 1 || g_debugMode == 3) {
                        // The motion view: the mv entry painted the vectors into
                        // the output; NVIDIA is skipped and starts afresh after.
                        usedDlaa = true;
                        e.dlHaveHistory = false;
                    } else if (dlaaEvaluate(ctx, eye, e.dlColour, e.dlDepth, e.dlMv, e.dlOut, w, h,
                                            oW, oH, jxNow, jyNow, resetHist, frameMs, &why)) {
                        usedDlaa = true;
                        e.dlHaveHistory = true;
                        if (!g_dlaaNoted || (oW != w && !g_dlssNoted)) {
                            g_dlaaNoted = true;
                            if (oW != w) g_dlssNoted = true;
                            Log::get().note(
                                "temporal aa: %s engaged -- NVIDIA's history takes the "
                                "%ux%u frame, its depth%s and the pass's own motion "
                                "vectors, jittered as before%s; the pass's history and "
                                "clip stand aside. Its price prints in the totals.",
                                oW != w ? "DLSS" : "DLAA", w, h,
                                depthSrv ? "" : " (none in hand yet: no depth until the "
                                                "probe finds it)",
                                oW != w ? " and brings it back to the unit-quality size" : "");
                        }
                    } else {
                        e.dlHaveHistory = false;
                        if (!g_dlaaFailNoted) {
                            g_dlaaFailNoted = true;
                            Log::get().note(
                                "temporal aa: dlaa was asked for, but %s. The pass's own "
                                "history runs instead.",
                                why);
                        }
                    }
                    // The frame that goes out, in the game's own format (dlSubmit
                    // says why). Inside the timed region, so the price is honest.
                    if (usedDlaa) ctx->CopyResource(e.dlSubmit, e.dlOut);
                }
            }
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
        const int readIdx = e.histRead;
        const int writeIdx = 1 - readIdx;
        bool ran = usedDlaa ? true : setParams(ctx, p);
        if (ran && !usedDlaa) {
            ID3D11ShaderResourceView* nullSrv[5] = {};
            ID3D11UnorderedAccessView* nullUav[3] = {};
            ctx->CSSetShaderResources(0, 5, nullSrv);
            ctx->CSSetUnorderedAccessViews(0, 3, nullUav, nullptr);
            ctx->CSSetShader(g_cs, nullptr, 0);
            ID3D11ShaderResourceView* srvs[5] = {inSrv, e.histSrv[readIdx], depthSrv,
                                                 e.layerSrv[0], e.layerSrv[1]};
            ID3D11UnorderedAccessView* uavs[3] = {e.outUav, e.histUav[writeIdx],
                                                  g_statsUav};
            ID3D11Buffer* cb = g_cb;
            ID3D11SamplerState* smp = g_samp;
            ctx->CSSetShaderResources(0, 5, srvs);
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

        ID3D11ShaderResourceView* nullSrv2[5] = {};
        ID3D11UnorderedAccessView* nullUav2[3] = {};
        ctx->CSSetShaderResources(0, 5, nullSrv2);
        ctx->CSSetUnorderedAccessViews(0, 3, nullUav2, nullptr);
        if (depthSrv) {
            ctx->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, savedRtv, savedDsv);
            for (auto* v : savedRtv) if (v) v->Release();
            if (savedDsv) savedDsv->Release();
        }
        ctx->CSSetShader(savedCs, nullptr, 0);
        ctx->CSSetShaderResources(0, 5, savedSrv);
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
            result = e.dlSubmit;
            ++g_treats;
            ++g_dlaaTreats;
            if (!g_trainedNoted) {
                // What NVIDIA is handed, once: the review of 2026-09-04
                // found the trained path invisible in the log (F12).
                g_trainedNoted = true;
                Log::get().note(
                    "temporal aa: first trained frame -- the game submits %s (DXGI_FORMAT "
                    "%d)%s, %ux%u per eye; NVIDIA is handed the colour as R8G8B8A8_UNORM, "
                    "the depth as R32_FLOAT (%s), the motion as R16G16_FLOAT in render "
                    "pixels and this frame's jitter (%+.3f, %+.3f) px, and answers at "
                    "%ux%u, handed on in the game's own format.",
                    formatName(sd.Format), static_cast<int>(sd.Format),
                    viaCopy ? " (copied out first: the source refuses a shader view)" : "",
                    w, h,
                    depthSrv ? "the scene's, reversed-Z"
                             : "none yet: zeros until the probe finds it",
                    static_cast<double>(jxNow), static_cast<double>(jyNow), e.dlOutW,
                    e.dlOutH);
            }
        } else if (ran) {
            e.histRead = writeIdx;
            e.haveHistory = true;
            e.dlHaveHistory = false;   // NVIDIA's history did not see this frame
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
    const std::string mode = cfg.getString("experimental.temporal_aa", "off");
    g_wanted = _stricmp(mode.c_str(), "off") != 0 && !mode.empty();
    g_viewTransposed = cfg.getBool("advanced.temporal_aa_view_transpose", true);
    const std::string cur = cfg.getString("advanced.temporal_aa_current", "filtered");
    g_filterCurrent = _stricmp(cur.c_str(), "raw") != 0;
    float c = cfg.getFloat("advanced.temporal_aa_history_sharp", 0.5f);
    if (!std::isfinite(c)) c = 0.5f;
    if (c < 0.5f) c = 0.5f;
    if (c > 1.0f) c = 1.0f;
    g_historyC = c;
    float snap = cfg.getFloat("advanced.temporal_aa_snap", 0.0f);
    if (!std::isfinite(snap) || snap < 0.0f) snap = 0.0f;
    if (snap > 0.5f) snap = 0.5f;
    g_snapPx = snap;
    float ship = cfg.getFloat("advanced.temporal_aa_ship_metres", 100.0f);
    if (!std::isfinite(ship) || ship < 0.0f) ship = 0.0f;
    if (ship > 100000.0f) ship = 100000.0f;
    g_shipMetres = ship;
    const std::string dbg = cfg.getString("advanced.temporal_aa_debug", "off");
    g_debugMode = _stricmp(dbg.c_str(), "motion") == 0 ? 1 : _stricmp(dbg.c_str(), "error") == 0 ? 2
                : _stricmp(dbg.c_str(), "depth") == 0 ? 3 : 0;
    float menu = cfg.getFloat("advanced.temporal_aa_menu_metres", 0.0f);
    if (!std::isfinite(menu) || menu < 0.0f) menu = 0.0f;
    if (menu > 50.0f) menu = 50.0f;
    g_menuMetres = menu;
    float hudm = cfg.getFloat("advanced.temporal_aa_hud_metres", 0.0f);
    if (!std::isfinite(hudm) || hudm < 0.0f) hudm = 0.0f;
    if (hudm > 10.0f) hudm = 10.0f;
    g_hudMetres = hudm;
    g_depthLayers = cfg.getBool("advanced.temporal_aa_depth_layers", false);
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

void temporalPassNoteSceneWrite(const void* res, const void* data, uint32_t bytes) {
    // The true view matrix lives at float offset 932 of the big scene
    // block (measured by the sun-glare fix's two-shot dump: three 3x4
    // rows, rotation plus translation). Every write of every block of
    // that size goes into the ring, stamped with the frame and the order;
    // chooseCameraRows picks the frame's at its first treat. Kept only
    // when the rows are a rotation.
    if (!g_wanted || !data || bytes < 944 * 4) return;
    const float* f = static_cast<const float*>(data) + 932;
    if (!temporalRowsAreRotation(f)) return;
    ++g_rowsWrites;
    RowsWrite& w = g_rowsRing[g_rowsSeq % kRowsRing];
    w.buf = res;
    memcpy(w.rows, f, sizeof(w.rows));
    w.frame = g_rowsFrame;
    w.seq = g_rowsSeq;
    w.valid = true;
    ++g_rowsSeq;
}

void temporalPassNoteFirstEyeDraw(ID3D11DeviceContext* ctx) {
    if (!g_wanted || g_curLatched) return;
    g_curLatched = true;
    g_boundSeen = false;
    g_boundBuf = nullptr;
    if (!ctx) return;
    // The block bound at the scene's first draw: the vertex stage's
    // constant buffers first, then the pixel stage's, the lowest slot
    // holding an object the ring has a write for. Two queries a frame.
    // Only the OBJECT is kept: which of its writes is the frame's camera
    // is chooseCameraRows's question, answered by continuity.
    ID3D11Buffer* vs[8] = {};
    ID3D11Buffer* ps[8] = {};
    ctx->VSGetConstantBuffers(0, 8, vs);
    ctx->PSGetConstantBuffers(0, 8, ps);
    auto inRing = [&](const void* b) {
        if (!b) return false;
        for (int i = 0; i < kRowsRing; ++i) {
            if (g_rowsRing[i].valid && g_rowsRing[i].buf == b) return true;
        }
        return false;
    };
    for (int i = 0; i < 8 && !g_boundSeen; ++i) {
        if (inRing(vs[i])) { g_boundBuf = vs[i]; g_boundSeen = true; g_latchSlotVs = i; }
    }
    for (int i = 0; i < 8 && !g_boundSeen; ++i) {
        if (inRing(ps[i])) { g_boundBuf = ps[i]; g_boundSeen = true; g_latchSlotPs = i; }
    }
    for (int i = 0; i < 8; ++i) {
        if (vs[i]) vs[i]->Release();
        if (ps[i]) ps[i]->Release();
    }
}

void temporalPassNoteHead(int eye, const float* prevPose, const float* nowPose,
                          const float* eyeOffset) {
    if (eye < 0 || eye > 1) return;
    EyeState& e = g_eye[eye];
    e.headNoted = false;
    if (!prevPose || !nowPose || !eyeOffset) return;
    memcpy(e.headPrev, prevPose, sizeof(e.headPrev));
    memcpy(e.headNow, nowPose, sizeof(e.headNow));
    memcpy(e.eyeOff, eyeOffset, sizeof(e.eyeOff));
    e.headNoted = true;
}

void temporalPassFrameBoundary() {
    if (!g_wanted) return;
    ++g_rowsFrame;
    g_rowsWritesSum += g_rowsWrites;
    ++g_rowsFramesSum;
    g_rowsWrites = 0;
    g_chosenThisFrame = false;
    if (g_curValid) {
        if (g_prevValid) ++g_camPairs;
        memcpy(g_prevRows, g_curRows, sizeof(g_prevRows));
        g_prevValid = true;
        if (!g_camNoted && g_camPairs >= 2) {
            g_camNoted = true;
            Log::get().note(
                "temporal aa: the game's camera is being read -- the view "
                "rows at float 932 of the scene block, the write of the frame that "
                "follows last frame's (the block bound at the scene's first draw is "
                "at VS b%d / PS b%d, -1 = not seen); they are the full view with the "
                "headset in it, read %s for the world path.",
                g_latchSlotVs, g_latchSlotPs, g_viewTransposed ? "view->world" : "world->view");
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

void regAppend(char* buf, size_t n, size_t& used, const char* fmt, ...) {
    if (used >= n) return;
    va_list ap;
    va_start(ap, fmt);
    const int m = vsnprintf(buf + used, n - used, fmt, ap);
    va_end(ap);
    if (m > 0) used += static_cast<size_t>(m);
    if (used > n) used = n;
}

bool temporalPassRegistration(char* buf, size_t n, char* buf2, size_t n2, char* buf3, size_t n3) {
    if (buf2 && n2) buf2[0] = 0;
    if (buf3 && n3) buf3[0] = 0;
    if (!buf || n == 0 || g_treats == 0 || g_intervalFrames == 0) return false;
    static const char* const kNames[4] = {"head, rotation only", "world, the other reading of the rows",
                                          "world, the reading in use", "head with depth"};
    size_t used = 0;
    regAppend(buf, n, used, "over the last %u eye-frames: ", g_intervalFrames);
    bool anyCand = false;
    for (int k = 0; k < 4; ++k) if (g_candPix[k]) anyCand = true;
    if (!anyCand) {
        regAppend(buf, n, used, "the candidates were not judged (NVIDIA's history ran, or the "
                                "pass's own had no history yet)");
    } else {
        for (int k = 0; k < 4; ++k) {
            regAppend(buf, n, used, "%s%s ", k ? "; " : "", kNames[k]);
            if (!g_candPix[k]) {
                regAppend(buf, n, used, "(no delta yet)");
                continue;
            }
            const double px = static_cast<double>(g_candPix[k]);
            // The mean clip size over the CLIPPED pixels: how far a clipped
            // history had strayed, 1/255ths of luma.
            const double meanSize = g_candClip[k]
                ? static_cast<double>(g_candSize[k]) / static_cast<double>(g_candClip[k])
                : 0.0;
            regAppend(buf, n, used, "clipped %.1f%% by %.1f/255 on average, off %.1f%%",
                      100.0 * static_cast<double>(g_candClip[k]) / px, meanSize,
                      100.0 * static_cast<double>(g_candRej[k]) / px);
        }
        static const char* const kBuckets[3] = {"still", "slow", "fast"};
        bool anyBucket = false;
        for (int b = 0; b < 3; ++b) if (g_bucketPix[b]) anyBucket = true;
        if (anyBucket) {
            regAppend(buf, n, used,
                      ". The used delta's clip share by head speed (under %.2f, under %.2f, over "
                      "that, degrees per frame): ",
                      static_cast<double>(kStillDeg), static_cast<double>(kSlowDeg));
            for (int b = 0; b < 3; ++b) {
                if (!g_bucketPix[b]) {
                    regAppend(buf, n, used, "%s%s none", b ? ", " : "", kBuckets[b]);
                    continue;
                }
                const double meanSize = g_bucketClip[b]
                    ? static_cast<double>(g_bucketSize[b]) / static_cast<double>(g_bucketClip[b])
                    : 0.0;
                regAppend(buf, n, used, "%s%s %.1f%% by %.1f/255 (%u eye-frames)",
                          b ? ", " : "", kBuckets[b],
                          100.0 * static_cast<double>(g_bucketClip[b]) /
                              static_cast<double>(g_bucketPix[b]),
                          meanSize, g_bucketFrames[b]);
            }
        }
    }
    // The world/ship split's share, the bright pixels without depth, and
    // the camera rows against the head (docked: zero and zero).
    if (g_intervalPix) {
        regAppend(buf, n, used,
                  ". The world path (the camera's delta beyond %.0f m and at the far plane) took "
                  "%.1f%% of pixels; %.1f%% of the bright pixels (luma over 0.6) had no depth",
                  static_cast<double>(g_shipMetres),
                  100.0 * static_cast<double>(g_worldPix) / static_cast<double>(g_intervalPix),
                  g_brightPix ? 100.0 * static_cast<double>(g_brightNoDepthPix) /
                                    static_cast<double>(g_brightPix)
                              : 0.0);
    }
    if (g_camFrames) {
        regAppend(buf, n, used,
                  "; the world delta (the game's view rows, the head in them) differed "
                  "from the head's by %.3f deg/frame on average and the eye moved %.4f "
                  "m/frame in the rows (docked: the first reads 0 whichever way the head "
                  "turns, the second a head's sway)",
                  g_camHeadDiffSum / g_camFrames, g_camMoveSum / g_camFrames);
    }
    if (g_rowsFramesSum) {
        const uint32_t chosen = g_chooseBound + g_chooseOther + g_chooseResync + g_chooseNone;
        regAppend(buf, n, used,
                  "; the scene block was written %.1f times a frame (%.1f candidates); the rows "
                  "chosen by continuity were the bound block's on %u frames and another's on "
                  "%u, nothing followed last frame's on %u, no write on %u; the ship's delta "
                  "was carried over a drop on %u frames",
                  static_cast<double>(g_rowsWritesSum) / static_cast<double>(g_rowsFramesSum),
                  chosen ? static_cast<double>(g_candSumCount) / static_cast<double>(chosen) : 0.0,
                  g_chooseBound, g_chooseOther, g_chooseResync, g_chooseNone, g_camCarried);
    }
    // The second line: the logger caps a line at 1200 characters, and the
    // probes' figures fell off the end of the first (2026-09-04).
    buf = buf2;
    n = buf2 ? n2 : 0;
    used = 0;
    if (g_camDropRot || g_camDropMove) {
        regAppend(buf, n, used,
                  "; the camera's delta was dropped on %u frames as another camera's (over 3 "
                  "deg from the head's) and its translation on %u as a jump (over 50 m); a "
                  "jump was carried on %u (zero by construction)",
                  g_camDropRot, g_camDropMove, g_camCarriedJump);
    }
    if (g_classWorldPix || g_classShipPix) {
        regAppend(buf, n, used,
                  "; the used delta clipped %.1f%% of the world path's pixels and %.1f%% of the ship's",
                  g_classWorldPix ? 100.0 * static_cast<double>(g_classWorldClip) /
                                        static_cast<double>(g_classWorldPix)
                                  : 0.0,
                  g_classShipPix ? 100.0 * static_cast<double>(g_classShipClip) /
                                       static_cast<double>(g_classShipPix)
                                 : 0.0);
    }
    if (g_intervalPix) {
        regAppend(buf, n, used,
                  "; a layer's depth covered %.2f%% of pixels and %.1f%% of the bright ones",
                  100.0 * static_cast<double>(g_layerPix) / static_cast<double>(g_intervalPix),
                  g_brightPix ? 100.0 * static_cast<double>(g_brightLayerPix) /
                                    static_cast<double>(g_brightPix)
                              : 0.0);
    }
    // The third line: the probes and the rows against the head.
    buf = buf3;
    n = buf3 ? n3 : 0;
    used = 0;
    if (g_probeSkyN) {
        regAppend(buf, n, used,
                  "; the history's best match sat (%+.2f, %+.2f) px from the prediction on the sky "
                  "(%llu probes)",
                  static_cast<double>(g_probeSkyDx) / 100.0 / static_cast<double>(g_probeSkyN),
                  static_cast<double>(g_probeSkyDy) / 100.0 / static_cast<double>(g_probeSkyN),
                  static_cast<unsigned long long>(g_probeSkyN));
    }
    if (g_probeWorldN || g_probeShipN) {
        regAppend(buf, n, used,
                  "; the history's best match sat (%+.2f, %+.2f) px from the prediction on the world "
                  "with a depth (%llu probes) and (%+.2f, %+.2f) px on the ship (%llu probes) -- a "
                  "steady offset that follows the motion is a lag or a scale, noise averages to zero",
                  g_probeWorldN ? static_cast<double>(g_probeWorldDx) / 100.0 / static_cast<double>(g_probeWorldN) : 0.0,
                  g_probeWorldN ? static_cast<double>(g_probeWorldDy) / 100.0 / static_cast<double>(g_probeWorldN) : 0.0,
                  static_cast<unsigned long long>(g_probeWorldN),
                  g_probeShipN ? static_cast<double>(g_probeShipDx) / 100.0 / static_cast<double>(g_probeShipN) : 0.0,
                  g_probeShipN ? static_cast<double>(g_probeShipDy) / 100.0 / static_cast<double>(g_probeShipN) : 0.0,
                  static_cast<unsigned long long>(g_probeShipN));
    }
    if (g_probeMm[0] || g_probeMm[1] || g_probeMm[2]) {
        auto kOf = [](int c) {
            return g_probeMm[c] ? static_cast<double>(g_probeDot[c]) / static_cast<double>(g_probeMm[c]) : 0.0;
        };
        auto pxOf = [](int c, uint64_t cnt) {
            return cnt ? sqrt(static_cast<double>(g_probeMm[c]) / 100.0 / static_cast<double>(cnt)) : 0.0;
        };
        regAppend(buf, n, used,
                  "; against its own vector the match scaled the motion by 1+k with k = %+.3f on the "
                  "sky (%.1f px rms), %+.3f on the world (%.1f px), %+.3f on the ship (%.1f px) -- k "
                  "under zero: the vector overshot the scene's turn",
                  kOf(0), pxOf(0, g_probeSkyN), kOf(1), pxOf(1, g_probeWorldN), kOf(2), pxOf(2, g_probeShipN));
    }
    if (g_rhN[0] + g_rhN[1] + g_rhN[2] > 0.0) {
        regAppend(buf, n, used,
                  "; the rows' delta sat %.4f deg/frame from the head's still (%.0f frames), %.4f "
                  "slow (%.0f), %.4f fast (%.0f); the rows turned (1+k) times the head, k = %+.3f "
                  "(x %+.3f, y %+.3f, z %+.3f), leading by %+.2f frames",
                  g_rhN[0] ? g_rhSum[0] / g_rhN[0] : 0.0, g_rhN[0],
                  g_rhN[1] ? g_rhSum[1] / g_rhN[1] : 0.0, g_rhN[1],
                  g_rhN[2] ? g_rhSum[2] / g_rhN[2] : 0.0, g_rhN[2],
                  g_rhMm > 0.0 ? g_rhDot / g_rhMm : 0.0,
                  g_rhMmAx[0] > 0.0 ? g_rhDotAx[0] / g_rhMmAx[0] : 0.0,
                  g_rhMmAx[1] > 0.0 ? g_rhDotAx[1] / g_rhMmAx[1] : 0.0,
                  g_rhMmAx[2] > 0.0 ? g_rhDotAx[2] / g_rhMmAx[2] : 0.0,
                  g_rhMmLag > 0.0 ? g_rhDotLag / g_rhMmLag : 0.0);
    }
    if (g_tvFrames) {
        regAppend(buf, n, used,
                  "; with the ship still (%u frames) the rows' translation followed the head's by "
                  "(%+.2f, %+.2f, %+.2f) per axis",
                  g_tvFrames,
                  g_tvMm[0] > 0.0 ? g_tvDot[0] / g_tvMm[0] : 0.0,
                  g_tvMm[1] > 0.0 ? g_tvDot[1] / g_tvMm[1] : 0.0,
                  g_tvMm[2] > 0.0 ? g_tvDot[2] / g_tvMm[2] : 0.0);
    }
    if (g_chooseMulti || g_twinFrames) {
        regAppend(buf, n, used,
                  "; on %u frames a second continuous reading differed from the chosen one, by %.2f "
                  "deg on average and %.2f at most; last frame's rows came again on %u",
                  g_chooseMulti, g_chooseMulti ? g_chooseSpreadSum / g_chooseMulti : 0.0,
                  g_chooseSpreadMax, g_twinFrames);
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
    g_intervalPix = 0;
    g_worldPix = 0;
    g_brightPix = 0;
    g_brightNoDepthPix = 0;
    g_camHeadDiffSum = 0.0;
    g_camMoveSum = 0.0;
    g_camFrames = 0;
    g_camDropRot = 0;
    g_camDropMove = 0;
    g_rowsWritesSum = 0;
    g_rowsFramesSum = 0;
    g_candSumCount = 0;
    g_chooseBound = 0;
    g_chooseOther = 0;
    g_chooseResync = 0;
    g_chooseNone = 0;
    g_camCarried = 0;
    g_camCarriedJump = 0;
    g_probeWorldDx = g_probeWorldDy = 0;
    g_probeWorldN = 0;
    g_probeShipDx = g_probeShipDy = 0;
    g_probeShipN = 0;
    g_classWorldPix = g_classWorldClip = 0;
    g_classShipPix = g_classShipClip = 0;
    g_layerPix = g_brightLayerPix = 0;
    g_probeSkyDx = g_probeSkyDy = 0;
    g_probeSkyN = 0;
    memset(g_probeDot, 0, sizeof(g_probeDot));
    memset(g_probeMm, 0, sizeof(g_probeMm));
    memset(g_rhN, 0, sizeof(g_rhN));
    memset(g_rhSum, 0, sizeof(g_rhSum));
    g_rhDot = g_rhMm = 0.0;
    memset(g_rhDotAx, 0, sizeof(g_rhDotAx));
    memset(g_rhMmAx, 0, sizeof(g_rhMmAx));
    g_rhDotLag = g_rhMmLag = 0.0;
    g_chooseMulti = 0;
    g_chooseSpreadSum = g_chooseSpreadMax = 0.0;
    memset(g_tvDot, 0, sizeof(g_tvDot));
    memset(g_tvMm, 0, sizeof(g_tvMm));
    g_tvFrames = 0;
    g_twinFrames = 0;
    return true;
}

bool temporalPassDlaaTotals(uint32_t* frames, double* avgMs, double* maxMs,
                            uint32_t* resets) {
    if (g_dlaaTreats == 0) return false;
    uint32_t evals = 0, rs = 0;
    if (!dlaaTotals(&evals, avgMs, maxMs, &rs)) return false;
    if (frames) *frames = g_dlaaTreats;
    if (resets) *resets = rs;
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
    unsigned outW, unsigned outH, unsigned flags) {
    if (!srcTex || eye < 0 || eye > 1 || !tanNow) return nullptr;
    void* out = nullptr;
    edvr::guardedBudget(edvr::g_budget, [&] {
        out = edvr::temporalInner(srcTex, eye, bounds, tanNow, tanPrev, jxNow,
                                  jyNow, deltaHead, headTrans, headTransSwapped,
                                  nearZ, farZ, headDeg, motion, blend,
                                  clampSigma, outW, outH, flags);
    });
    return out;
}

extern "C" __declspec(dllexport) void edvrTemporalAaNoteHead(int eye, const float* prevPose,
                                                             const float* nowPose,
                                                             const float* eyeOffset) {
    edvr::temporalPassNoteHead(eye, prevPose, nowPose, eyeOffset);
}
