#pragma once
// The SRV turret HUD's private geometry shaders (srv_turret.h says why): one pass-through for each distinct vertex-shader OUTPUT SIGNATURE among the families that draw the turret
// view's HUD (srv_turret_math.h: kFamilies, kSignatures). Compiled at BUILD time by tools/temporal_shader_build into kSrvTurretHudGs<X>Bytecode (never in the game), created by
// shaderSwapCreateGs.
//
// WHAT IT DOES. In the turret view the weapon panels (the weapon arc brackets, the weapon names, the ammo counts, the INFO strip) are drawn from records about 0.27 to 0.70 m
// from the eye, the radar from 0.11 to 0.44 m, and the larger panels (SYSTEMS, the location panel) from 1.3 m and more. Every shader below is the same triangle-list pass-through
// that moves each vertex in 3D (one shared function, turretMove, so the eight copies cannot drift): it reconstructs the vertex's eye-space position P from its clip position and the
// eye's projection tangents (left, right, down, up as the game projects, the eye jitter included; NDC y is up), translates it by T (the HUD-forward key; every vertex), moves it along
// its ray from the ANCHOR A (a fixed point in the seated frame, in the eye's space here) by `gap` metres nearer (a NEGATIVE gap: deeper, the push-back) when its distance from A lies in
// (dMin, dMax), and projects the result back with the same tangents:
//
//     P1 = P + T;   V = P1 - A;   d = |V|;   Q = (dMin < d < dMax) ? A + V (d - gap) / d : P1;   pos = project(Q), z' = ndcZ * Q.z
//     ndcZ = z / w when w > 0 and 0 <= z / w <= 1 (the game's own depth), else 0.999
//
// The derivation and the CPU copy are in srv_turret_math.h (anchoredMove). The vertex's NDC z is kept when it is a valid depth, and its other interpolants are not touched, so the pixel
// shader is the game's own. ELITE IS REVERSED Z with an infinite far plane (cb0[6] = (0, 0, 0, 0.025): clip z is the constant 0.025 and NDC z = 0.025 / w), so a vertex the game put
// behind the eye (w < 0: z / w is negative) or inside its 2.5 cm near plane (0 < w < 0.025: z / w above 1) has no valid depth, and the moved vertex would fail the rasterizer's depth
// clip (0 <= z <= w) and be cut. Such a vertex is drawn at 0.999, the NEAREST valid reversed-Z depth (1 is near), like the rest of the HUD. The reprojection is linear in Q (x' = (2 Q.x -
// Q.z (r + l)) / (r - l), likewise y, w' = Q.z), so it is valid for any Q.z: every vertex the move applies to is output MOVED (never the unmoved one beside moved corners, which would
// stretch the triangle), and the rasterizer's homogeneous clipping handles a corner that ends behind the eye. A vertex on the eye plane (clip w about 0) is left as the game made it, and so
// is every vertex when neither the translation nor the window applies (the output is then the input bit for bit). `triangle` input takes triangle lists AND triangle strips (the input assembler hands over each
// strip triangle in the winding the rasterizer would have used), and each output triangle is emitted on its own with RestartStrip.
//
// THE INPUT AND OUTPUT STRUCT is the vertex shader's output signature, element for element and in its register order (the fxc disassembly of the dumps' bytecode, written once in
// srv_turret_math.h's kSignatures), so the linkage the D3D debug layer validates is exact; the output is that signature followed by one more element, `pre` (TURRETPRE, 4 floats): the
// clip position as the game made it. The pixel shader does not read it; the GPU instrument's stream-output twin streams it out beside the moved position (srv_turret_math.h). 
// tools/temporal_shader_build reflects each compiled shader against that table, so a member list here that drifts from it fails in the build. Some of the game's semantics are `__USER_...`
// names (the radar and hologram families); the position is always the last member, named `pos`. A pixel shader that also reads SV_POSITION.xy takes the rasterizer's, which is not an
// interpolant, so it needs nothing here.
//
// THE CONSTANT BUFFER is private to the shader (GS slot 0, which the game never binds): four registers, srv_turret_math.h's TurretParams -- {gap, dMin, dMax, -}, {left, right, down, up},
// {the anchor in the eye's space: X right, Y up, Z forward}, {the translation in the same axes}.
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
    "    float4 mv;   // x: gap, metres nearer (negative: deeper); y: dMin and z: dMax, the window of distances from the anchor that move\n" \
    "    float4 tn;   // the eye's tangents as the game projects: x left, y right, z down, w up\n" \
    "    float4 an;   // the anchor in the eye's space: X right, Y up, Z forward (= clip w)\n" \
    "    float4 tr;   // the translation (HUD forward) in the same axes\n" \
    "};\n" \
    "float4 turretMove(float4 c) {\n" \
    "    const float w = c.w;\n" \
    "    if (abs(w) < 1.0e-6) return c;\n" \
    "    const float3 p = float3(c.x * (tn.y - tn.x) * 0.5 + w * (tn.y + tn.x) * 0.5, c.y * (tn.w - tn.z) * 0.5 + w * (tn.w + tn.z) * 0.5, w);\n" \
    "    const float3 p1 = p + tr.xyz;\n" \
    "    const float3 v = p1 - an.xyz;\n" \
    "    const float d = length(v);\n" \
    "    const bool win = d > mv.y && d < mv.z;\n" \
    "    const bool tra = any(tr.xyz != 0.0);\n" \
    "    if (!win && !tra) return c;\n" \
    "    float3 q = p1;\n" \
    "    if (win) q = an.xyz + v * ((d - mv.x) / d);\n" \
    "    const float zn = c.z / w;\n" \
    "    const float ndcZ = (w > 0.0 && zn >= 0.0 && zn <= 1.0) ? zn : 0.999;\n" \
    "    return float4((2.0 * q.x - q.z * (tn.y + tn.x)) / (tn.y - tn.x), (2.0 * q.y - q.z * (tn.w + tn.z)) / (tn.w - tn.z), ndcZ * q.z, q.z);\n" \
    "}\n" \
    "struct Vertex {\n"
#define EDVR_SRV_TURRET_GS_TAIL \
    "\n" \
    "};\n" \
    "struct Moved {\n" \
    "    Vertex v;\n" \
    "    float4 pre : TURRETPRE;\n" \
    "};\n" \
    "[maxvertexcount(3)]\n" \
    "void main(triangle Vertex v[3], inout TriangleStream<Moved> stream) {\n" \
    "    [unroll] for (int i = 0; i < 3; ++i) {\n" \
    "        Moved o;\n" \
    "        o.v = v[i];\n" \
    "        o.pre = v[i].pos;\n" \
    "        o.v.pos = turretMove(v[i].pos);\n" \
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
