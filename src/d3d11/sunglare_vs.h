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
// The swap's own constants at a slot it owns. The field data closed
// the case the other way round from every theory before it: the glare
// CB's ROWS follow the head completely -- no camera clamp -- and what
// freezes past forty-five degrees is i.pos itself, computed CPU-side
// in the game's head-look frame. The glare world's origin sits on the
// sun, so the swap is handed the per-draw camera position (solved from
// the rows) and the true camera-to-sun direction (-normalize(cam)),
// and rebuilds the element on that ray. The row-substitution fields
// stay for compatibility but tValid.x is now always 0: the game's own
// rows are the true ones.
cbuffer CBT : register(b2) {
    float4 tRow4;
    float4 tRow5;
    float4 tRow7;
    float4 tValid;    // x: substitute rows (retired), y: sun solve live
    float4 tSun;      // xyz = true camera-to-sun direction, this draw
    float4 tCam;      // xyz = camera position in the glare frame
};
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

    // The anchor weights: 1,1 means the element sits at the anchor --
    // but the LENS-FLARE GHOSTS carry (1,1) too: their slide lives in
    // t7 (unit screen direction times slide length), applied on top of
    // the anchor. Field data splits the classes by two decades of
    // length -- anchored elements 0.05..0.8, ghosts 7..160 -- so the
    // slide length is the discriminator. A ghost routed down the world
    // path lands stacked ON the sun, hidden under the corona: exactly
    // vivid mode's missing lens flare (field, 2026-08-23).
    bool anchored = i.p3.x > 0.999 && i.p3.y > 0.999;
    bool slider = i.t7.z >= 6.0;
    bool worldPath = anchored && !slider;
#ifdef ALLWORLD
    worldPath = true;
#endif
#ifdef ALLFLAT
    worldPath = false;
#endif

    // Element SELECTION by record, not by instance index. The game's
    // record list is dynamic: elements enter and REORDER with its
    // head-look camera (the forty-five-degree disappearing disc was a
    // prefix clamp faithfully drawing a reordered slot). tValid.z asks
    // for the curated set: anchored, non-sliding, non-axis-locked
    // elements -- the corona and smudge class; beams (axis-locked) and
    // the lens-flare ghosts (sliders) collapse. Selection is
    // identity-proof under any reorder because it reads what the
    // record IS.
    bool selected = tValid.z > 0.5
        ? (anchored && !slider && i.t4.w <= 0.0)
        : true;

    // The element position, REBUILT. i.pos is computed CPU-side in the
    // game's head-look frame and goes stale past the clamp -- the one
    // stale input left. The sun is the glare world's origin, so the
    // true camera-to-sun ray is fully known per draw; keep the game's
    // own element distance (the flare stack's depths) and re-aim it.
    // Inside the clamp this reduces to i.pos exactly.
    bool haveSun = tValid.y > 0.5;
    float3 rel = i.pos.xyz - tCam.xyz;
    float3 epos = (worldPath && haveSun)
        ? tCam.xyz + tSun.xyz * max(length(rel), 1.0)
        : i.pos.xyz;

    // The element centre through the game's own rows: once with the
    // game's position (the flat path, verbatim) and once with the
    // rebuilt position (the world path's centre, taps and depth).
    float4 P = float4(i.pos.xyz, 1.0);
    float cx = dot(cb0[4], P);
    float cy = dot(cb0[5], P);
    float cw = dot(cb0[7], P);
    bool haveTrue = tValid.x > 0.5;
    float4 row4 = haveTrue ? tRow4 : cb0[4];
    float4 row5 = haveTrue ? tRow5 : cb0[5];
    float4 row7 = haveTrue ? tRow7 : cb0[7];
    float4 PT = float4(epos, 1.0);
    float tcx = dot(row4, PT);
    float tcy = dot(row5, PT);
    float tcw = dot(row7, PT);

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

    // ---- position ----
    float4 svpos;
    float2 psPos;
    float2 worldHalf = float2(0.0, 0.0);
    if (worldPath) {
        // The world-anchored billboard: right/up perpendicular to the
        // true camera-to-sun ray, world-up anchored, the element's own
        // rotation kept, each vertex projected with a REAL w.
        float3 d = haveSun ? tSun.xyz : normalize(i.pos.xyz);
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
        worldHalf = halfNdc;
        float2 half2 = halfNdc * i.uvc.zw;
        // NDC half-extent -> world half-extent at the element's depth,
        // PER AXIS through the TRUE rows: d(ndc)/d(world-offset) along
        // each row is |row|/w, and the rows' magnitudes differ by the
        // aspect.
        float toWorldX = tcw / max(length(row4.xyz), 1e-4);
        float toWorldY = tcw / max(length(row5.xyz), 1e-4);
        float3 wpos = epos
                    + rr * (half2.x * toWorldX)
                    + uu * (half2.y * toWorldY);
        float4 WP = float4(wpos, 1.0);
        float px = dot(row4, WP);
        float py = dot(row5, WP);
        float pw = dot(row7, WP);
        svpos = float4(px, py, 0.0, pw);
        psPos = float2(px, py) / pw;
    } else {
        // The original flat path, ported: slide the centre toward the
        // screen anchor by the element's weights, build the flare-axis
        // basis, rotate, expand in NDC, w = 1.
        float2 anchor01 = ndc * 0.5 + 0.5;
        float2 anchorPx = vpSize * anchor01 + vpMin;
        float2 anchorNdc = anchorPx * 2.0 - 1.0;
        // The slide length, capped: past the game's own head-look
        // comfort zone its ghost lengths explode (7..30 in normal
        // viewing, 160+ observed at high head angles) -- the cap keeps
        // a wild ghost bounded instead of smearing across the sky.
        float t7len = min(i.t7.z, 60.0);
        float2 slid = (1.0 - i.p3.xy) * i.t7.xy * t7len + anchorNdc;
        float2 pos2 = baseSize * i.t7.xy * t7len * i.p3.xy * 2.0 + slid;
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
    // World-path taps aim at the TRUE view's sun position -- the depth
    // texture is the true view's, so a clamped-camera tap centre would
    // test the wrong pixels the moment the head passes the clamp.
    float2 tNdc = float2(tcx, tcy) / max(tcw, 1e-4);
    float2 tapNdc = worldPath ? tNdc : ndc;
    float depthRefUsed = worldPath
        ? ((i.pos.w < 0.5) ? tcw : max(tcw - i.pos.w, 0.0))
        : depthRef;
    float2 texel = 1.0 / cb1[332].xy;
    float2 c01 = tapNdc * 0.5 + 0.5;
    float2 cTap = float2(c01.x, 1.0 - c01.y);
    // Clamp the whole tap window inside the depth texture: at high
    // eccentricity the sun sits at the very edge, the taps straddle
    // outside, and the clamped garbage flipped the gate with tiny head
    // motions -- a disc flashing on and off at forty-five degrees.
    // Testing the nearest valid depth is approximate but stable.
    cTap = clamp(cTap, 3.0 * texel, 1.0 - 3.0 * texel);
    float vis = 0.0;
    [unroll]
    for (int ty = -2; ty <= 2; ++ty) {
        [unroll]
        for (int tx = -2; tx <= 2; ++tx) {
            float2 uv = cTap + float2(tx, ty) * texel;
            float dep = t0.SampleLevel(s0, uv, 0.0).x;
            vis += (depthRefUsed < dep) ? 1.0 : 0.0;
        }
    }
    float visFrac = vis * 0.04;

    // The occlusion test is only meaningful while the sun sits well
    // inside this eye's view -- beyond, the tap window has clamped to
    // the texture edge and is testing nothing; visibility blends to
    // shown there so the test cannot flicker the disc.
    float ecc2 = length(tapNdc);
    float edge = smoothstep(0.8, 1.2, ecc2);
    visFrac = lerp(visFrac, max(visFrac, 1.0), edge);
#ifdef NOGATE
    visFrac = 1.0;
#endif

    // Gate, verbatim in spirit: collapse the quad only when the tested
    // visibility is nothing AND the edge blend is not holding it up.
#ifndef NOGATE
    if (visFrac <= 0.01 || (worldPath ? tcw : cw) <= 0.0) {
        svpos = float4(0.0, 0.0, 0.0, 0.0);
    }
#endif
    // Selection is not the occlusion gate: an unselected element stays
    // collapsed even under NOGATE diagnostics.
    if (!selected) svpos = float4(0.0, 0.0, 0.0, 0.0);
    o.pos = svpos;

    // The graceful frame-edge exit the stock eccentricity-shrink used
    // to provide. A world-anchored disc holds constant angular size, so
    // without this it rides at full brightness until the quad falls
    // entirely outside ONE eye's frustum and the clipper removes it
    // whole -- a binary pop in that eye while the other still shows it
    // (field, 2026-08-22; the per-eye eccentricity split runs ~8
    // degrees out there). Fade the glow on the frame-edge approach,
    // per axis because the frustum is a box, finishing before the quad
    // can fully clip; both eyes run the same continuous ramp, so they
    // can only ever differ softly, which is also what a real lens
    // does with a source leaving its field.
    float edgeFade = 1.0;
    if (worldPath) {
        float m = max(worldHalf.x, max(worldHalf.y, 1e-3));
        float eccAxis = max(abs(tapNdc.x), abs(tapNdc.y));
        edgeFade = 1.0 - smoothstep(1.0, 1.0 + 0.8 * m, eccAxis);
    }
    o.t1.rgb = visFrac * i.col * edgeFade;
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
