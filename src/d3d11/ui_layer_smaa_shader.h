#pragma once

namespace edvr {

// SMAA 1x over the HUD layer's finished 8-bit content, in the flat profile, before the composite downsamples it (fix.hud_antialiasing;
// docs/design-flat-ui-quality-2026-10-05.md, "2026-10-24: edge smoothing for the HUD layer"). The layer is composited after the game's anti-aliasing, so
// the game's own SMAA or DLSS never sees the HUD; this runs the same SMAA on the layer image instead.
//
// The three passes are the map's (map_smaa_shader.h): the same prologue, the same SMAA chunks (SMAA.hlsl, unmodified, SMAA_PRESET_HIGH), and for
// passes 2 and 3 the map's own entries (smaaWeights, smaaBlend), which are plain SMAA with no colour transform. Only the edge test differs, and it is
// the one entry here. The layer holds premultiplied light in rgb and TRANSMITTANCE in alpha, cleared to (0, 0, 0, 1) and composited as
// out = L.rgb + F.rgb * L.a: a dark opaque backing is rgb 0 and a 0 against the clear's rgb 0 and a 1, so only alpha shows that edge. SMAA's own colour
// edge test looks at rgb alone; this one takes the colour distance over all four channels. Everything else in it -- the threshold, the steps, the local
// contrast adaptation -- is SMAAColorEdgeDetectionPS line for line (SMAA.hlsl), so SMAA.hlsl itself stays as upstream wrote it.
//
// The blend writes all four channels with no transform: a linear mix of premultiplied rgb and transmittance is what the composite's own box filter
// does to them, so it is exact for this composite. The layer is read with Load (the point sampler's clamp becomes an explicit clamp), and a texel with no
// edge writes zero edges, as the map's compute edge entry does, so the pass covers every pixel and nothing is left over from the last frame.
inline constexpr char kUiLayerSmaaEdgesHlsl[] = R"HLSL(
Texture2D UiSmaaLayer : register(t0);
RWTexture2D<float2> UiSmaaEdges : register(u0);

float4 uiSmaaTexel(int2 p) {
    return UiSmaaLayer.Load(int3(clamp(p, int2(0, 0), int2(smaaMetrics.zw) - 1), 0));
}

float uiSmaaDelta(float4 a, float4 b) {
    const float4 t = abs(a - b);
    return max(max(t.r, t.g), max(t.b, t.a));
}

float2 uiSmaaEdgeTest(int2 p) {
    const float2 threshold = float2(SMAA_THRESHOLD, SMAA_THRESHOLD);
    const float4 C = uiSmaaTexel(p);
    float4 delta;
    delta.x = uiSmaaDelta(C, uiSmaaTexel(p + int2(-1, 0)));
    delta.y = uiSmaaDelta(C, uiSmaaTexel(p + int2(0, -1)));
    float2 edges = step(threshold, delta.xy);
    if (dot(edges, float2(1.0, 1.0)) == 0.0)
        return float2(0.0, 0.0);
    delta.z = uiSmaaDelta(C, uiSmaaTexel(p + int2(1, 0)));
    delta.w = uiSmaaDelta(C, uiSmaaTexel(p + int2(0, 1)));
    float2 maxDelta = max(delta.xy, delta.zw);
    delta.z = uiSmaaDelta(C, uiSmaaTexel(p + int2(-2, 0)));
    delta.w = uiSmaaDelta(C, uiSmaaTexel(p + int2(0, -2)));
    maxDelta = max(maxDelta.xy, delta.zw);
    const float finalDelta = max(maxDelta.x, maxDelta.y);
    edges.xy *= step(finalDelta, SMAA_LOCAL_CONTRAST_ADAPTATION_FACTOR * delta.xy);
    return edges;
}

[numthreads(8, 8, 1)]
void uiSmaaEdges(uint3 id : SV_DispatchThreadID) {
    if (id.x >= uint(smaaMetrics.z) || id.y >= uint(smaaMetrics.w)) return;
    UiSmaaEdges[id.xy] = uiSmaaEdgeTest(int2(id.xy));
}
)HLSL";

}  // namespace edvr
