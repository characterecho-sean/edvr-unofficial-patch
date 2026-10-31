#pragma once
// The SRV turret HUD's private geometry shaders (srv_turret.h says why): one pass-through for each distinct vertex-shader OUTPUT SIGNATURE among the families that draw the turret
// view's HUD (srv_turret_math.h: kFamilies, kSignatures). Compiled at BUILD time by tools/temporal_shader_build into kSrvTurretHudGs<X>Bytecode (never in the game), created by
// shaderSwapCreateGs.
//
// WHAT IT DOES. In the turret view the weapon panels (the weapon arc brackets, the weapon names, the ammo counts, the INFO strip) are drawn from records about 0.27 to 0.70 m
// from the eye, the radar from 0.11 to 0.44 m, and the larger panels (SYSTEMS, the location panel) from 1.3 m and more. Every shader below is the same triangle-list pass-through
// that moves a vertex whose depth is in the window `gap` metres nearer along its ray from the HEAD CENTRE (a NEGATIVE gap: that many metres deeper, the push-back), which is a
// horizontal shift of its clip x and nothing else:
//
//     w = pos.w;   if (w > wMin && w < wMax)   pos.x += s * gap / (w - gap);
//
// with s = P00 * hx per eye (P00 = 2 / (tanRight - tanLeft) of that eye's projection as Elite renders it; hx = +ipd/2 for the left eye, -ipd/2 for the right; ipd the
// eye separation Elite renders with). The derivation is in srv_turret_math.h: exact for a fixed eye separation, it keeps each vertex's cyclopean direction (the panel
// does not change size or place for a head at the centre), changes only the disparity, and follows head motion because both layers are fixed in the turret's frame.
// y, z, w and the other interpolants are not touched, so the pixel shader is the game's own. The window is the module's to choose (srv_turret_math.h): a push runs wMin = 0.02 to
// wMax = 1.0 and the pull gap + 0.10 to 4.0, either keeping the divisor w - gap positive. `triangle` input takes triangle lists AND triangle strips (the input
// assembler hands over each strip triangle in the winding the rasterizer would have used), and each output triangle is emitted on its own with RestartStrip.
//
// THE INPUT AND OUTPUT STRUCT is the vertex shader's output signature, element for element and in its register order (the fxc disassembly of the dumps' bytecode, written once in
// srv_turret_math.h's kSignatures), so the linkage the D3D debug layer validates is exact. tools/temporal_shader_build reflects each compiled shader against that table, so a
// member list here that drifts from it fails in the build. Some of the game's semantics are `__USER_...` names (the radar and hologram families); the position is always the last
// member, named `pos`. A pixel shader that also reads SV_POSITION.xy takes the rasterizer's, which is not an interpolant, so it needs nothing here.
//
// THE CONSTANT BUFFER is private to the shader (GS slot 0, which the game never binds): {s, gap, wMin, wMax}.
//
// THE SHADERS, by signature (the first family's vertex shader names it):
//   A  vs 1989E6D3B405FDE0 + ps EAB8A1C95A13FFBE    hologram panels        TEXCOORD0 xyzw, TEXCOORD6 xyz, SV_POSITION
//   B  vs 2CECEC3065EF0D4A + ps A4D03619D631B186    hologram panels        TEXCOORD0 xyzw, TEXCOORD7 xy, SV_POSITION
//   C  vs DF3503CD07F9B10C + ps 8C08EB252B0F6095                           M_TEXCOORD xy, SV_POSITION
//   D  vs 44DDCDEBAEA137D8 + ps CB53257EFE8B1696                           RADAR_LOCALKEY_COLOUR xyzw, RADAR_LOCALKEY_POSITIONOFFSET xyzw, M_TEXCOORD x, SV_POSITION
//   E  vs 9B34C331902DC1ED + ps 3B3433E4FEBBC37B                           RADAR_LOCALKEY_COLOUR xyzw, RADAR_LOCALKEY_POSITIONOFFSET xyz, SV_POSITION
//   F  vs 9611A454527F7FEB + ps 1E1C49DC51C0E509                           RADAR_LOCALKEY_COLOUR xyz, RADAR_LOCALKEY_POSITIONOFFSET xy, SV_POSITION
//   G  vs B932058F26B76691 + ps 65861AC394D51526                           RADAR_LOCALKEY_COLOUR xyz, SV_POSITION
//   H  vs 5559BD94B6852E83 + ps EA02FAC2BD6C643C or E95634B0F61D218F        M_LIGHTINGPOSITION xyz, M_TEXCOORD xy, SV_POSITION (the target hologram sphere)
namespace edvr {

// The shader text: EDVR_SRV_TURRET_GS_HEAD (the constant buffer and the opening of the `Vertex` struct), the members, EDVR_SRV_TURRET_GS_TAIL (the struct's end and the pass-through).
// Adjacent string literals join, so each shader is one constant text; the macros are single-line concatenations because a raw string cannot span the lines of a directive.
#define EDVR_SRV_TURRET_GS_HEAD \
    "\n" \
    "cbuffer Turret : register(b0) {\n" \
    "    float s;      // P00 * hx for the eye drawn\n" \
    "    float gap;    // metres nearer (negative: deeper)\n" \
    "    float wMin;   // the nearest view depth that moves\n" \
    "    float wMax;   // the farthest\n" \
    "};\n" \
    "struct Vertex {\n"
#define EDVR_SRV_TURRET_GS_TAIL \
    "\n" \
    "};\n" \
    "[maxvertexcount(3)]\n" \
    "void main(triangle Vertex v[3], inout TriangleStream<Vertex> stream) {\n" \
    "    [unroll] for (int i = 0; i < 3; ++i) {\n" \
    "        Vertex o = v[i];\n" \
    "        const float w = o.pos.w;\n" \
    "        if (w > wMin && w < wMax) o.pos.x += s * gap / (w - gap);\n" \
    "        stream.Append(o);\n" \
    "    }\n" \
    "    stream.RestartStrip();\n" \
    "}\n"

constexpr char kSrvTurretHudGsA[] = EDVR_SRV_TURRET_GS_HEAD
    "    float4 t0 : TEXCOORD0;\n"
    "    float3 t6 : TEXCOORD6;\n"
    "    float4 pos : SV_Position;"
    EDVR_SRV_TURRET_GS_TAIL;

constexpr char kSrvTurretHudGsB[] = EDVR_SRV_TURRET_GS_HEAD
    "    float4 t0 : TEXCOORD0;\n"
    "    float2 t7 : TEXCOORD7;\n"
    "    float4 pos : SV_Position;"
    EDVR_SRV_TURRET_GS_TAIL;

constexpr char kSrvTurretHudGsC[] = EDVR_SRV_TURRET_GS_HEAD
    "    float2 m : __USER_VERTEX_M_TEXCOORD;\n"
    "    float4 pos : SV_Position;"
    EDVR_SRV_TURRET_GS_TAIL;

constexpr char kSrvTurretHudGsD[] = EDVR_SRV_TURRET_GS_HEAD
    "    float4 c : __USER_RADAR_LOCALKEY_COLOUR;\n"
    "    float4 o : __USER_RADAR_LOCALKEY_POSITIONOFFSET;\n"
    "    float m : __USER_VERTEX_M_TEXCOORD;\n"
    "    float4 pos : SV_Position;"
    EDVR_SRV_TURRET_GS_TAIL;

constexpr char kSrvTurretHudGsE[] = EDVR_SRV_TURRET_GS_HEAD
    "    float4 c : __USER_RADAR_LOCALKEY_COLOUR;\n"
    "    float3 o : __USER_RADAR_LOCALKEY_POSITIONOFFSET;\n"
    "    float4 pos : SV_Position;"
    EDVR_SRV_TURRET_GS_TAIL;

constexpr char kSrvTurretHudGsF[] = EDVR_SRV_TURRET_GS_HEAD
    "    float3 c : __USER_RADAR_LOCALKEY_COLOUR;\n"
    "    float2 o : __USER_RADAR_LOCALKEY_POSITIONOFFSET;\n"
    "    float4 pos : SV_Position;"
    EDVR_SRV_TURRET_GS_TAIL;

constexpr char kSrvTurretHudGsG[] = EDVR_SRV_TURRET_GS_HEAD
    "    float3 c : __USER_RADAR_LOCALKEY_COLOUR;\n"
    "    float4 pos : SV_Position;"
    EDVR_SRV_TURRET_GS_TAIL;

constexpr char kSrvTurretHudGsH[] = EDVR_SRV_TURRET_GS_HEAD
    "    float3 l : __USER_VERTEX_M_LIGHTINGPOSITION;\n"
    "    float2 m : __USER_VERTEX_M_TEXCOORD;\n"
    "    float4 pos : SV_Position;"
    EDVR_SRV_TURRET_GS_TAIL;

#undef EDVR_SRV_TURRET_GS_HEAD
#undef EDVR_SRV_TURRET_GS_TAIL
}  // namespace edvr
