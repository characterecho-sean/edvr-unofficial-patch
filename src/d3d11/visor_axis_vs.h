// F121: the replacement vertex shader for the on-foot helmet visor (VS B553BB479B7C0B97), identical to the game's except that the interpolated uv it
// hands the visor pixel shader (o1.x) is moved by s, per eye (visor_axis_math.h says why and by how much).
//
// A TRANSCRIPTION, NOT A REWRITE, AND GRANTED TO ONE EXACT HASH (billboard_facing_vs.h says why that is the rule). It was written from the disassembly of
// the game's own shader (tools\visor_axis_test\fixtures\vs_B553BB479B7C0B97.dxbc, held to its hash name), from `vs_5_0` down: the signatures, the
// declarations, every instruction. What holds it to the original:
//   * the rig compares the input and output signatures column for column and every dcl line (ours the one added b3, one float4: the shift);
//   * the rig runs the ORIGINAL bytecode and this one on the same inputs through stream output and compares every output register: identical with the
//     shift at zero, and with the shift at s only o1.x differs, by exactly s.
// The game's draw is a fullscreen triangle from SV_VertexID alone (no vertex buffer): vertex 0 at clip (-1, -1 + dy), vertex 1 at (-1, 3 + dy), vertex
// 2 at (3, -1 + dy), uv (0, 1), (0, -1), (2, 1); dy = 2 * (1 - cb2[1].x), the visor's slide. The rest is the tint (cb2[0], the hue wheel; cb2[1].yz, the
// HUD colour's saturation and gain) and the HUD colour read from the first element of t0 (the colour buffer), rotated about the grey axis. All of it is
// copied untouched. Inputs: b1 (the frame block; only row 90 is read), b2 (the visor's own block), t0 (a structured buffer of float4), and ours at b3.
#pragma once

namespace edvr {

constexpr char kVisorAxisVS[] = R"HLSL(
cbuffer CB1 : register(b1) { float4 cb1[91]; };
cbuffer CB2 : register(b2) { float4 cb2[2]; };
cbuffer CBA : register(b3) { float4 axis; };   // x: the shift of the interpolated uv, in uv units (positive moves the frame toward +x)
StructuredBuffer<float4> T0 : register(t0);

void main(uint vid : SV_VERTEXID,
          out float4 o0 : __USER_TINTDATA,
          out float2 o1 : __USER_VERTEX_M_TEXCOORD,
          out float3 o2 : __USER_VERTEX_M_VERTEXCOLOUR,
          out float4 o3 : SV_POSITION)
{
    float4 r0;
    float4 r1;

    // The tint: the hue wheel's three phases and the visor's gain.
    r0 = cb2[0].xxxz + float4(-0.0, -0.333, -0.667, -1.0);
    r0.xyz = frac(r0.xyz);
    r0.w = cb2[0].w * r0.w + 1.0;
    r0.xyz = r0.xyz + float3(-0.5, -0.5, -0.5);
    r0.xyz = saturate(abs(r0.xyz) * 6.0 - 1.0);
    r0.xyz = r0.xyz - 1.0;
    r1.x = cb2[0].w * cb2[0].y;
    r0.xyz = r1.x * r0.xyz + 1.0;
    o0.xyz = r0.w * r0.xyz;

    // The triangle: the alpha, the uv, the position (the slide dy in y).
    precise float slide = 1.0 - cb2[1].x;
    o0.w = slide;
    precise float hi = (float)(vid >> 1);
    o1.x = hi * 2.0 - axis.x;                  // F121: the only line that differs from the game's (the game's is hi * 2.0)
    precise float slide2 = slide * 2.0;
    precise float hi4 = hi * 4.0;
    o3.x = hi4 - 1.0;
    precise float lo = (float)(vid & 1);
    o1.y = -lo * 2.0 + 1.0;
    precise float lo4 = lo * 4.0;
    lo4 = lo4 - 1.0;
    o3.y = slide2 + lo4;

    // The HUD colour from the colour buffer, rotated about the grey axis, its saturation and gain.
    r0.xyz = max(T0[0].xyz, 0.0);
    r0.w = max(cb1[90].z, 0.0);
    r0.xyz = r0.xyz / r0.w;
    r0.w = dot(r0.xyz, r0.xyz);
    r0.w = rsqrt(r0.w);
    r1.xyz = r0.w * r0.xyz;
    r1.x = dot(float3(0.557350, 0.557350, 0.557350), r1.xyz);
    r1.y = r1.x * 0.557350;
    r0.xyz = r0.xyz * r0.w - r1.y;
    float3 t0z = r0.zxy * 0.557350;
    float3 t0w = float3(t0z.z, t0z.x, t0z.y);   // r1.wyz of the game's first product: (r0.y, r0.z, r0.x) * 0.55735
    float3 rot = r0.zxy * 0.557350 - t0w;
    rot = rot * 0.707103;
    r0.xyz = r0.xyz * -0.707111 + rot;
    r0.xyz = r1.x * 0.557350 + r0.xyz;
    r0.w = dot(r0.xyz, float3(0.299, 0.587, 0.114));
    r0.xyz = r0.xyz - r0.w;
    r0.xyz = cb2[1].y * r0.xyz + r0.w;
    r0.xyz = r0.xyz - 1.0;
    o2.xyz = cb2[1].z * r0.xyz + 1.0;
    o3.zw = float2(0.99, 1.0);
}
)HLSL";

}  // namespace edvr
