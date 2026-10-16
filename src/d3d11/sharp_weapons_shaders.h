// fix.sharp_weapons' source pass (docs/dlss-blur-elements-2026-10-14.md, "Built, 2026-10-16: weapons fire in the HDR layer"): the HLSL the
// build compiles to fixed DXBC (tools/temporal_shader_build/fixed_sharp_weapons_variants.h) and the rig runs. Header-only text, no device.
//
// ONE fullscreen pass, two targets. It reads the game's HDR tonemap input F (the lit R11G11B10 eye target, at the render's size) and writes
// F_UP -- F resampled to the LAYER's size by a bilinear filter built from Loads (no sampler, so nothing of the game's sampler state matters
// and the function is exactly the one the rig evaluates on the CPU) -- to two targets:
//   SV_Target0, the scratch S, replaced: S = F_up. The tonemap re-issue's "base" term is the game's own tonemap of S.
//   SV_Target1, the weapons layer W, ADDED (the pass's blend state: ONE, ONE on the colour channels): W = L + F_up. The re-issue's "lit"
//   term is the game's own tonemap of W. Their difference, T(F_up + L) - T(F_up), is the weapons' light as the tonemap would have shown it
//   over the scene it lands on.
// The vertex shader is a fullscreen triangle that also hands the pixel shader its UV (so the pass needs no constant buffer: the layer's
// size is implicit in the interpolated UV, the source's in GetDimensions).
#pragma once

namespace edvr {

constexpr char kSharpWeaponsSourceVsHlsl[] = R"HLSL(
struct V { float4 p : SV_POSITION; float2 uv : TEXCOORD0; };
V main(uint id : SV_VertexID) {
    float2 p = id == 0 ? float2(-1, -1) : (id == 1 ? float2(-1, 3) : float2(3, -1));
    V v;
    v.p = float4(p, 0, 1);
    v.uv = float2(p.x * 0.5 + 0.5, 0.5 - p.y * 0.5);
    return v;
}
)HLSL";

constexpr char kSharpWeaponsSourcePsHlsl[] = R"HLSL(
Texture2D<float4> Src : register(t0);
struct O { float4 scratch : SV_Target0; float4 weapons : SV_Target1; };
O main(float4 p : SV_POSITION, float2 uv : TEXCOORD0) {
    uint w, h;
    Src.GetDimensions(w, h);
    float2 q = uv * float2(w, h) - 0.5;
    float2 f = frac(q);
    int2 i0 = (int2)floor(q);
    int2 hi = int2(w - 1, h - 1);
    float3 a = Src.Load(int3(clamp(i0, int2(0, 0), hi), 0)).rgb;
    float3 b = Src.Load(int3(clamp(i0 + int2(1, 0), int2(0, 0), hi), 0)).rgb;
    float3 c = Src.Load(int3(clamp(i0 + int2(0, 1), int2(0, 0), hi), 0)).rgb;
    float3 d = Src.Load(int3(clamp(i0 + int2(1, 1), int2(0, 0), hi), 0)).rgb;
    float3 v = lerp(lerp(a, b, f.x), lerp(c, d, f.x), f.y);
    O o;
    o.scratch = float4(v, 1);
    o.weapons = float4(v, 0);
    return o;
}
)HLSL";

}  // namespace edvr
