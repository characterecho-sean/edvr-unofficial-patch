#pragma once
// The SRV turret HUD's private geometry shader (srv_turret.h says why). Compiled at BUILD time by tools/temporal_shader_build into kSrvTurretHudGsBytecode (never in
// the game), created by shaderSwapCreateGs.
//
// WHAT IT DOES. The game draws the turret view's target info panel, its SYSTEMS panel and its reticle as three quads of one mesh (6 indices, a triangle list) through
// vs E508648660A352B2. That vertex shader writes TEXCOORD 0 xy at register 0 and SV_POSITION xyzw at register 1, with clip z = |clip w| (the near plane) and the clip
// w the view depth in metres. The panels sit about a metre deeper than the near HUD layer yet are drawn over it. This shader is a triangle-list pass-through that moves
// a vertex whose depth is in the panels' range `gap` metres nearer along its ray from the HEAD CENTRE, which is a horizontal shift of its clip x and nothing else:
//
//     w = pos.w;   if (w > wMin && w < wMax)   pos.x += s * gap / (w - gap);
//
// with s = P00 * hx per eye (P00 = 2 / (tanRight - tanLeft) of that eye's projection as Elite renders it; hx = +ipd/2 for the left eye, -ipd/2 for the right; ipd the
// eye separation Elite renders with). The derivation is in srv_turret_math.h: exact for a fixed eye separation, it keeps each vertex's cyclopean direction (the panel
// does not change size or place for a head at the centre), changes only the disparity, and follows head motion because both layers are fixed in the turret's frame.
// y, z, w and the texture coordinates are not touched, so the pixel shader is the game's own. wMin = gap + 0.10 keeps the divisor positive; wMax = 4.0 leaves the
// reticle, at the target, where it was. With the gap at 0 the shift is exactly 0 and the shader is a copy.
//
// THE INPUT AND OUTPUT STRUCT is the vertex shader's output signature, in its register order, so the linkage the D3D debug layer validates is exact.
//
// THE CONSTANT BUFFER is private to this shader (GS slot 0, which the game never binds): {s, gap, wMin, wMax}.
namespace edvr {
constexpr char kSrvTurretHudGs[] = R"HLSL(
cbuffer Turret : register(b0) {
    float s;      // P00 * hx for the eye drawn
    float gap;    // metres nearer
    float wMin;   // the nearest view depth that moves (gap + 0.10)
    float wMax;   // the farthest (4.0): the reticle stays
};
struct Vertex {
    float2 uv : TEXCOORD0;
    float4 pos : SV_Position;
};
[maxvertexcount(3)]
void main(triangle Vertex v[3], inout TriangleStream<Vertex> stream) {
    [unroll] for (int i = 0; i < 3; ++i) {
        Vertex o = v[i];
        const float w = o.pos.w;
        if (w > wMin && w < wMax) o.pos.x += s * gap / (w - gap);
        stream.Append(o);
    }
    stream.RestartStrip();
}
)HLSL";
}  // namespace edvr
