// visual.desktop_window (docs\desktop-window.md): the HLSL the build compiles to fixed DXBC (tools/temporal_shader_build/fixed_desktop_window_variants.h) and
// desktop_window.cpp draws with. Header-only text, no device.
//
// ONE fullscreen triangle per eye, drawn into a viewport that is the eye's rectangle in the swap chain's buffer. The vertex shader hands the pixel shader a UV that
// runs 0..1 across the viewport; the pixel shader maps it onto the eye's bounds sub-rect and averages four bilinear taps, a quarter of a destination pixel either
// side of the pixel's centre in each direction (adequate up to about a 4:1 downscale). Every tap is held inside the sub-rect, half a source texel in from its edge,
// so a neighbouring image in the same texture never bleeds in. The shader view and the target are byte-pass-through formats (desktop_window_math.h), so the
// averaging is the only arithmetic done to the image.
//   b0  Bounds  {uMin, vMin, uMax, vMax} as submitted (a min above its max is a flip, which falls out of the lerp), then Step: the bounds' span over one destination
//       pixel on each axis, signed the same way.
//   t0  the eye texture, s0 a linear clamp sampler.
#pragma once

namespace edvr {

constexpr char kDesktopWindowVsHlsl[] = R"HLSL(
struct V { float4 p : SV_POSITION; float2 t : TEXCOORD0; };
V main(uint id : SV_VertexID) {
    float2 p = id == 0 ? float2(-1, -1) : (id == 1 ? float2(-1, 3) : float2(3, -1));
    V v;
    v.p = float4(p, 0, 1);
    v.t = float2(p.x * 0.5 + 0.5, 0.5 - p.y * 0.5);
    return v;
}
)HLSL";

constexpr char kDesktopWindowPsHlsl[] = R"HLSL(
Texture2D<float4> Eye : register(t0);
SamplerState Linear : register(s0);
cbuffer Shown : register(b0) { float4 Bounds; float2 Step; float2 Pad; };
float4 main(float4 p : SV_POSITION, float2 t : TEXCOORD0) : SV_Target {
    uint w, h;
    Eye.GetDimensions(w, h);
    float2 texel = 0.5 / float2(w, h);
    float2 lo = min(Bounds.xy, Bounds.zw) + texel;
    float2 hi = max(Bounds.xy, Bounds.zw) - texel;
    float2 uv = lerp(Bounds.xy, Bounds.zw, t);
    float2 o = Step * 0.25;
    float4 s = Eye.SampleLevel(Linear, clamp(uv + float2(-o.x, -o.y), lo, hi), 0)
             + Eye.SampleLevel(Linear, clamp(uv + float2( o.x, -o.y), lo, hi), 0)
             + Eye.SampleLevel(Linear, clamp(uv + float2(-o.x,  o.y), lo, hi), 0)
             + Eye.SampleLevel(Linear, clamp(uv + float2( o.x,  o.y), lo, hi), 0);
    return s * 0.25;
}
)HLSL";

}  // namespace edvr
