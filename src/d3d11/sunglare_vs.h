// The replacement vertex shader for the sun-glare element train -- the
// shader-swap arc's payload. Written against the disassembly of the
// game's vs 94D5C556DFD6D705 (dumped 2026-08-22, game build 330683):
// same nine-register input layout, same four-register output signature,
// same constant buffers, same atlas math, same 25-tap depth visibility
// test -- and a different POSITION path. The original emits the quad
// directly in NDC with w = 1: flat in the projection plane, which is
// the whole artifact family (edge-on at high eccentricity, per-eye
// stretch disagreement, apparent rotation under yaw). This one builds
// the quad in WORLD space, perpendicular to the element's own
// direction, and projects each vertex through the game's own rows with
// a real perspective w -- the disc is then simply THERE, and every
// head motion is just a camera move over a world object.
//
// Elements that WANT the screen slide -- the lens-flare ghosts, whose
// charm is sliding along the flare axis -- keep the original flat path,
// selected by the same per-instance anchor weights the original used.
//
// Compiled at runtime through d3dcompiler_47.dll (present on every
// Windows 10/11), once per session; any failure stands the swap down
// and the game draws stock.
#pragma once

namespace edvr {

constexpr const char kSunglareWorldVS[] = R"HLSL(
cbuffer CB0 : register(b0) { float4 cb0[8]; };
cbuffer CB1 : register(b1) { float4 cb1[333]; };
Texture2D t0 : register(t0);
SamplerState s0 : register(s0);

struct VSIn {
    float4 uvc : TEXCOORD0;    // xy = atlas-local uv, zw = corner (+-1)
    float4 pos : POSITION0;    // xyz = element position (camera-relative
                               // world), w = occlusion depth bias
    float4 p1  : POSITION1;    // zw = base size
    float4 p2  : POSITION2;    // xy/zw = eccentricity size shaping
    float4 p3  : POSITION3;    // xy = anchor weights (the slide k),
                               // z = pulse phase, w = element rotation
    float4 t4  : TEXCOORD4;    // x = alpha out, y = opacity drive,
                               // z = through to PS, w = axis-align flag
    float4 t5  : TEXCOORD5;    // atlas tile: xy = tile index, zw = span
    float3 col : COLOUR6;      // element colour
    float4 t7  : TEXCOORD7;    // xy/z = size scale chain, w = mode flag
};

struct VSOut {
    float4 t1 : TEXCOORD1;
    float  t2 : TEXCOORD2;
    float4 t3 : TEXCOORD3;
    float4 pos : SV_POSITION;
};

VSOut main(VSIn i) {
    VSOut o;

    // Atlas UV, verbatim: a 16x8 tile grid over the art sheet.
    o.t3.xy = i.t5.xy * float2(0.0625, 0.125)
            + i.uvc.xy * i.t5.zw * float2(0.0625, 0.125);

    // The element centre through the game's own rows.
    float4 P = float4(i.pos.xyz, 1.0);
    float cx = dot(cb0[4], P);
    float cy = dot(cb0[5], P);
    float cw = dot(cb0[7], P);

    // Occlusion reference depth, verbatim.
    float depthRef = (i.pos.w < 0.5) ? cw : max(cw - i.pos.w, 0.0);

    // Opacity / pulse drive, verbatim.
    float2 baseSize;
    if (i.t4.y > 0.0) {
        baseSize = saturate(i.t4.y) * i.p1.zw;
    } else {
        float sp, cp;
        sincos(i.p3.z * 3.141593, sp, cp);
        baseSize = (1.0 - (sp * 0.5 + 0.5)) * i.p1.zw;
    }

    // Screen-space centre in NDC, for the flat path and the PS feed.
    float2 ndc = float2(cx, cy) / cw;

    // Viewport machinery, verbatim: cb1[281] is this eye's viewport
    // rect within the target, cb1[332].xy the target resolution.
    float2 vpMin = cb1[281].xy;
    float2 vpSize = cb1[281].zw - cb1[281].xy;

    // Size shaping. The original shrinks elements with eccentricity
    // (the flat-screen fade toward the edges); the world path keeps a
    // constant angular size instead, which is what a real object does.
    float ecc = min(length(ndc), 1.0);
    float2 shaped = clamp(1.0 - abs(ndc) * (1.0 - i.p2.zw), 0.2, 1.0);
    float2 eccSize = shaped * ecc + (1.0 - ecc) * i.p2.xy;
    float2 szFlat = eccSize * i.p1.xy;
    szFlat.x = szFlat.x / cb1[91].z;
    if (i.t7.w > 0.0) szFlat = float2(szFlat.x, szFlat.y) * cb1[91].w;
    float2 szWorld = i.p1.xy * baseSize;

    // The anchor weights: 1,1 means "sit on the element" -- those are
    // the world-path elements. Anything else is a slider and keeps the
    // original flat behaviour.
    bool worldPath = i.p3.x > 0.999 && i.p3.y > 0.999;
#ifdef ALLWORLD
    worldPath = true;
#endif
#ifdef ALLFLAT
    worldPath = false;
#endif

    // ---- position ----
    float4 svpos;
    float2 psPos;
    if (worldPath) {
        // The world-anchored billboard: right/up perpendicular to the
        // element's own direction, world-up anchored, the element's own
        // rotation kept, each vertex projected with a REAL w.
        float3 d = normalize(i.pos.xyz);
        float3 wu = float3(0.0, 1.0, 0.0);
        float3 r = cross(wu, d);
        float rl = length(r);
        if (rl < 0.05) { r = float3(1.0, 0.0, 0.0); } else { r /= rl; }
        float3 u = cross(d, r);
        float sr, cr;
        sincos(i.p3.w, sr, cr);
        float3 rr = r * cr + u * sr;
        float3 uu = u * cr - r * sr;
        // The quad-size law, taken from the flat path at zero
        // eccentricity: v3.xy times v2.xy, x aspect-divided, the t7.w
        // mode scale kept. The first world build fed it baseSize -- the
        // CENTRE-offset scale, not the quad size -- and the disc drew
        // sub-pixel small, which the field read as not drawn at all.
        float2 halfNdc = i.p2.xy * i.p1.xy;
        halfNdc.x = halfNdc.x / cb1[91].z;
        if (i.t7.w > 0.0) halfNdc = halfNdc * cb1[91].w;
        float2 half2 = halfNdc * i.uvc.zw;
        // NDC half-extent -> world half-extent at the element's depth,
        // PER AXIS: d(ndc)/d(world-offset) along each row is |row|/w,
        // and the rows' magnitudes differ by the aspect.
        float toWorldX = cw / max(length(cb0[4].xyz), 1e-4);
        float toWorldY = cw / max(length(cb0[5].xyz), 1e-4);
        float3 wpos = i.pos.xyz
                    + rr * (half2.x * toWorldX)
                    + uu * (half2.y * toWorldY);
        float4 WP = float4(wpos, 1.0);
        float px = dot(cb0[4], WP);
        float py = dot(cb0[5], WP);
        float pw = dot(cb0[7], WP);
        svpos = float4(px, py, 0.0, pw);
        psPos = float2(px, py) / pw;
    } else {
        // The original flat path, ported: slide the centre toward the
        // screen anchor by the element's weights, build the flare-axis
        // basis, rotate, expand in NDC, w = 1.
        float2 anchor01 = ndc * 0.5 + 0.5;
        float2 anchorPx = vpSize * anchor01 + vpMin;
        float2 anchorNdc = anchorPx * 2.0 - 1.0;
        float2 slid = (1.0 - i.p3.xy) * i.t7.xy * i.t7.z + anchorNdc;
        float2 pos2 = baseSize * i.t7.xy * i.t7.z * i.p3.xy * 2.0 + slid;
        float2 axis = pos2 - anchorNdc;
        float al = length(axis);
        axis = (al > 1e-5) ? axis / al : float2(1.0, 0.0);
        float2 perp = float2(axis.x, -axis.y);
        if (length(perp) < 0.9 || i.t4.w <= 0.0) perp = float2(1.0, 0.0);
        float sr, cr;
        sincos(i.p3.w, sr, cr);
        float2 bx = float2(dot(float2(perp.x, -perp.y), float2(sr, cr)),
                           dot(float2(perp.x, -perp.y), float2(cr, -sr)));
        float2 by = float2(dot(float2(perp.y, perp.x), float2(sr, cr)),
                           dot(float2(perp.y, perp.x), float2(cr, -sr)));
        float2 h = szFlat * i.uvc.zw;
        float2 off = float2(dot(h, bx), dot(h, by));
        float2 p = pos2 + off;
        svpos = float4(p, 0.0, 1.0);
        psPos = p;
    }

    // ---- the 25-tap visibility test, at the element centre ----
    // The taps sample the PER-EYE depth at eye-normalized coordinates
    // with a flipped Y -- ndc*0.5+0.5, NO viewport remap. The first
    // port remapped into target space, read the wrong depth, and the
    // gate collapsed every quad: the disc vanished entirely.
    float2 texel = 1.0 / cb1[332].xy;
    float2 c01 = ndc * 0.5 + 0.5;
    float2 cTap = float2(c01.x, 1.0 - c01.y);
    float vis = 0.0;
    [unroll]
    for (int ty = -2; ty <= 2; ++ty) {
        [unroll]
        for (int tx = -2; tx <= 2; ++tx) {
            float2 uv = cTap + float2(tx, ty) * texel;
            float dep = t0.SampleLevel(s0, uv, 0.0).x;
            vis += (depthRef < dep) ? 1.0 : 0.0;
        }
    }
    float visFrac = vis * 0.04;
#ifdef NOGATE
    visFrac = 1.0;
#endif

    // Gate and outputs, verbatim: the original collapses the quad to
    // degenerate unless MORE THAN a quarter-tap of the sun is visible
    // (count > 0.25 -- one tap of twenty-five suffices), and behind the
    // camera counts as hidden.
#ifndef NOGATE
    if (vis <= 0.25 || cw <= 0.0) {
        svpos = float4(0.0, 0.0, 0.0, 0.0);
    }
#endif
    o.pos = svpos;
    o.t1.rgb = visFrac * i.col;
    o.t1.a = i.t4.x;
    o.t2 = i.t4.z;

    // The PS position feed, verbatim shape: the element position mapped
    // into target texture space, y flipped.
    float2 fed01 = psPos * 0.5 + 0.5;
    float2 fedTex = fed01 * vpSize + vpMin;
    o.t3.z = fedTex.x;
    o.t3.w = 1.0 - fedTex.y;
    return o;
}
)HLSL";

}  // namespace edvr
