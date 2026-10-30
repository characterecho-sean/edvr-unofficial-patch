#pragma once
// The SRV turret HUD's private geometry shaders (srv_turret.h says why): one pass-through for each of the two hologram families that draw the turret view's HUD. Compiled at
// BUILD time by tools/temporal_shader_build into kSrvTurretHudGsABytecode and kSrvTurretHudGsBBytecode (never in the game), created by shaderSwapCreateGs.
//
// WHAT IT DOES. In the turret view the near HUD layer (the weapon arc brackets, the weapon names, the ammo counts, the INFO strip) is drawn from records about 0.41 to 0.47 m
// from the eye, and the target information panel and the SYSTEMS panel from records about 1.39 m out, by the same two instanced hologram families:
//   family A  vs 1989E6D3B405FDE0 + ps EAB8A1C95A13FFBE   output TEXCOORD0 xyzw, TEXCOORD6 xyz, SV_POSITION
//   family B  vs 2CECEC3065EF0D4A + ps A4D03619D631B186   output TEXCOORD0 xyzw, TEXCOORD7 xy,  SV_POSITION
// Both vertex shaders compute a camera-relative position and project it with dp4 against cb0[4..7], so the clip w is the view depth in metres. Each shader here is a
// triangle-list pass-through that moves a vertex whose depth is in the deep band `gap` metres nearer along its ray from the HEAD CENTRE, which is a horizontal shift of its
// clip x and nothing else:
//
//     w = pos.w;   if (w > wMin && w < wMax)   pos.x += s * gap / (w - gap);
//
// with s = P00 * hx per eye (P00 = 2 / (tanRight - tanLeft) of that eye's projection as Elite renders it; hx = +ipd/2 for the left eye, -ipd/2 for the right; ipd the
// eye separation Elite renders with). The derivation is in srv_turret_math.h: exact for a fixed eye separation, it keeps each vertex's cyclopean direction (the panel
// does not change size or place for a head at the centre), changes only the disparity, and follows head motion because both layers are fixed in the turret's frame.
// y, z, w and the other interpolants are not touched, so the pixel shader is the game's own. wMin = gap + 0.10 keeps the divisor positive and leaves the near layer alone;
// wMax = 4.0 leaves anything far alone. With the gap at 0 the shift is exactly 0 and the shader is a copy.
//
// THE INPUT AND OUTPUT STRUCT is the vertex shader's output signature, element for element and in its register order (the fxc disassembly of the dumps' bytecode), so the
// linkage the D3D debug layer validates is exact. The pixel shader of family B also reads SV_POSITION.xy: that is the rasterizer's, not an interpolant, so it needs nothing here.
//
// THE CONSTANT BUFFER is private to the shader (GS slot 0, which the game never binds): {s, gap, wMin, wMax}.
namespace edvr {
constexpr char kSrvTurretHudGsA[] = R"HLSL(
cbuffer Turret : register(b0) {
    float s;      // P00 * hx for the eye drawn
    float gap;    // metres nearer
    float wMin;   // the nearest view depth that moves (gap + 0.10)
    float wMax;   // the farthest (4.0)
};
struct Vertex {
    float4 t0 : TEXCOORD0;
    float3 t6 : TEXCOORD6;
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

constexpr char kSrvTurretHudGsB[] = R"HLSL(
cbuffer Turret : register(b0) {
    float s;      // P00 * hx for the eye drawn
    float gap;    // metres nearer
    float wMin;   // the nearest view depth that moves (gap + 0.10)
    float wMax;   // the farthest (4.0)
};
struct Vertex {
    float4 t0 : TEXCOORD0;
    float2 t7 : TEXCOORD7;
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
