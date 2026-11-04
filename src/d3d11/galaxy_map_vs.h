// The VR Galaxy Map's replacement shaders (docs/galaxy-map-vr-2026-11-03.md, Fix 3): the icons' and labels' vertex shaders facing the eye, the mouse arrow's vertex
// shader at the stars' depth, and the arrow probe's vertex and pixel shaders.
//
// THE THREE TRANSCRIPTIONS (C31238331D8AC3D4, 12382D2EA45E9632, 4EF6DDB075A927FA) ARE TRANSCRIPTIONS, NOT REWRITES, AND EACH IS GRANTED TO ONE EXACT HASH
// (billboard_facing_vs.h says why that is the rule). Each was written from the disassembly of the game's own shader from `vs_5_0` down, and every float immediate was
// taken from the token hex, never from the text: the disassembler prints 1/32767 (0x38000100), 1/32767.5 (0x38000080), 1/4095.9 (0x39800080) and 1/255 (0x3B808081) all
// as 0.000031, 0.000031, 0.000244 and 0.003922 (visor_axis_vs.h learned that on F121). tools/galaxy_map_vs_test runs the transcriptions through stream output on WARP
// against the formulae below; the same program, started by hand with the game's own bytecode (vs_*.dxbc from the shader dump), compares every output of the game's shader
// with ours.
//
// WHAT EACH IS. The three draw quads and sprites from one record each: t33 holds a 336-byte record (a.x the bone base, a.y the scale s as a float, a.zw the packed
// quaternion q, then the world position W at +16), t38 the bones (3 rows of float4, 48 bytes), and the vertex carries the packed local position v, the per-vertex
// billboard offset b (v2.z) and the uv (v2.x). The game's position is
//     p  = rotate(q, v) * s + (W - cb1[275].xyz)
//     p' = (dot(float4(p, 1), cb0[9]), cb0[10], cb0[11])          (the arrow: clip = dot(cb0[4..7], float4(p, 1)) directly)
//     clip = (p' + cb1[277].xyz * b.x + cb1[278].xyz * b.y) . (cb1[270], cb1[271], cb1[272]) + cb1[273]
//
// THE ICONS (C31238331D8AC3D4 and 12382D2EA45E9632, which differs only in the uv: `o0 = uv * cb2[1].xy * 16 + cb2[1].zw`). Their records carry a quaternion the CPU
// writes facing the HEAD-FREE map camera (rotate(q, ex) = -baseRight, rotate(q, ey) = +baseUp, rotate(q, ez) = -baseForward), so each quad is a plane fixed in the
// world: in flat the map camera is the eye and they face it, in VR the eye turns with the head and the icons and labels twist (measured 0.5 to 0.9 degrees rms against
// that prediction in the 2026-11-04 dumps). Three records are left exactly as the game draws them:
//   * W with |W|^2 <= 1e6: the UI frame's own records (map records sit at y of about 41070);
//   * a quad flat on the galactic plane (rotate(q, ez).y within 0.0005 of 1, the "10 : 88 : -100" labels and discs): they belong on the plane;
//   * the identity quaternion (rotate(q, ez).z at or above 0.9995, the n=12 triangles, spanned by b).
// Every other record faces the eye: p' = (dot(float4(center, 1), cb0[9]), cb0[10], cb0[11]) + s * (-v.x * R + v.y * U - v.z * F), R, U and F the eye's axes taken from
// the view-projection rows (F from the w column, R and U the x and y columns made orthogonal to F), so no row's sign is assumed. The signs -v.x, +v.y, -v.z reproduce
// rotate(q, v) when the eye is the map camera.
//
// THE ARROW (4EF6DDB075A927FA): the game's own, except that W - cb1[275].xyz becomes W - ((s < 0) ? 0 : cb1[275].xyz). The arrow's record is the only one with a negative
// scale (-0.402 in the dump); the same shader draws other sprites some frames (the loader's), which keep the game's depth. Drawn with the eye origin taken out, the arrow
// sits at the stars' depth, which is where the panels would be were they not at the game's.
//
// THE PROBE (galaxy_map_ui.h): one point at the arrow's tip, in the game's own depth (clip = dot(cb0[4..7], float4(W - cb1[275].xyz, 1)), the arrow's record position W
// being exactly the tip), tested against the panel mask: the pixel shader discards where the mask is clear, an occlusion query counts what stays.
#pragma once

namespace edvr {

#define EDVR_MAP_COMMON_HLSL R"HLSL(
struct Rec { uint4 a; float4 p; float4 pad[19]; };
struct Bone { float4 r0; float4 r1; float4 r2; };
StructuredBuffer<Rec> T33 : register(t33);
StructuredBuffer<Bone> T38 : register(t38);

#define K32767 asfloat(0x38000100u)
#define K32767_5 asfloat(0x38000080u)
#define K4096 asfloat(0x39800080u)
#define K255 asfloat(0x3B808081u)

// The packed local position (PACKEDVERTEXDATAA): an exponent form when the top seven bits of z are 64, otherwise a fixed-point form.
float3 mapVertex(uint3 v1) {
    float3 v;
    uint kind = (v1.z >> 24) & 0x7Fu;
    if (kind == 64u) {
        float4 u = float4(v1.x & 0xFFFFu, v1.y & 0xFFFFu, v1.x >> 16, v1.y >> 16);
        float e = exp2(u.w * K4096) - 1.0;
        v = (float3(u.x, u.z, u.y) * K32767_5 - 1.0) * e;
    } else {
        int sh = 20 - (int)kind;
        float scale = rcp((float)((1 << sh) - 1));
        float off = (float)(1 << (int)(v1.z >> 24));
        float3 t = float3((float)(v1.x & 0x1FFFFFu), (float)((v1.x >> 21) + ((v1.y & 0x3FFu) << 11)), (float)((v1.y >> 10) & 0x1FFFFFu));
        v = t * scale - off;
    }
    return v;
}

// Up to four bones, weights in the bytes of PACKEDVERTEXDATAC.y, indices in the bytes of .x, from the record's base a.x.
float3 mapSkin(uint4 a, uint2 v3, float3 v) {
    if (a.x != 0u) {
        float4 m0 = 0, m1 = 0, m2 = 0;
        uint2 sk = v3;
        uint n = 4u;
        [loop] while (sk.y != 0u && n > 0u) {
            uint bi = a.x + (sk.x & 255u);
            float w = (float)(sk.y & 255u) * K255;
            Bone b = T38[bi];
            m0 += b.r0 * w;
            m1 += b.r1 * w;
            m2 += b.r2 * w;
            sk >>= 8;
            n -= 1u;
        }
        float4 vv = float4(v, 1.0);
        v = float3(dot(m0, vv), dot(m1, vv), dot(m2, vv));
    }
    return v;
}

float4 mapQuat(uint4 a) {
    return float4(a.z & 0xFFFFu, a.z >> 16, a.w & 0xFFFFu, a.w >> 16) * K32767 - 1.0;
}

float3 mapRotate(float4 q, float3 v) {
    float3 c = cross(q.xyz, v);
    float3 r = (q.w * (q.w + q.w)) * v - v;
    r = dot(q.xyz, v) * (q.xyz + q.xyz) + r;
    return c * (q.w + q.w) + r;
}
)HLSL"

// C31238331D8AC3D4. EDVR_MAP_ICON_UV is the uv statement; 12382D2EA45E9632 adds b2.
#define EDVR_MAP_ICON_MAIN_HLSL R"HLSL(
cbuffer CB0 : register(b0) { float4 cb0[12]; };
cbuffer CB1 : register(b1) { float4 cb1[279]; };

void main(uint2 v0 : INSTANCEANDMODELDATAINDEX, uint4 v1 : PACKEDVERTEXDATAA, uint4 v2 : PACKEDVERTEXDATAB, uint4 v3 : PACKEDVERTEXDATAC,
          out float2 o0 : TEXCOORD0, out float4 o1 : SV_POSITION)
{
    float3 v = mapVertex(v1.xyz);
    float4 u = float4(v2.z & 0xFFFFu, v2.z >> 16, v2.x & 0xFFFFu, v2.x >> 16) * K32767 - 1.0;
    float2 b = u.xy * 16.0;
    float2 uv = u.zw;
    EDVR_MAP_ICON_UV
    Rec rec = T33[v0.x];
    uint4 a = rec.a;
    v = mapSkin(a, v3.xy, v);
    float s = asfloat(a.y);
    float3 W = rec.p.xyz;
    float3 center = W - cb1[275].xyz;
    float4 q = mapQuat(a);
    float3 pp;
    float3 n = mapRotate(q, float3(0.0, 0.0, 1.0));
    bool faces = dot(W, W) > 1000000.0 && abs(n.y) < 0.9995 && n.z < 0.9995;
    float4 pc = float4(center, 1.0);
    if (faces) {
        float3 F = normalize(float3(cb1[270].w, cb1[271].w, cb1[272].w));
        float3 Rx = float3(cb1[270].x, cb1[271].x, cb1[272].x);
        float3 R = normalize(Rx - dot(Rx, F) * F);
        float3 Ry = float3(cb1[270].y, cb1[271].y, cb1[272].y);
        float3 U = normalize(Ry - dot(Ry, F) * F);
        pp = float3(dot(pc, cb0[9]), dot(pc, cb0[10]), dot(pc, cb0[11])) + s * (-v.x * R + v.y * U - v.z * F);
    } else {
        float4 p = float4(mapRotate(q, v) * s + center, 1.0);
        pp = float3(dot(p, cb0[9]), dot(p, cb0[10]), dot(p, cb0[11]));
    }
    pp = cb1[277].xyz * b.x + pp;
    pp = cb1[278].xyz * b.y + pp;
    o1 = pp.y * cb1[271];
    o1 = pp.x * cb1[270] + o1;
    o1 = pp.z * cb1[272] + o1;
    o1 = o1 + cb1[273];
}
)HLSL"

#define EDVR_MAP_ICON_UV_PLAIN "o0 = uv * 16.0;"
#define EDVR_MAP_ICON_UV_SCALED "o0 = (uv * cb2[1].xy) * 16.0 + cb2[1].zw;"

constexpr char kMapIconFacingVS[] =
    EDVR_MAP_COMMON_HLSL
    "#define EDVR_MAP_ICON_UV " EDVR_MAP_ICON_UV_PLAIN "\n"
    EDVR_MAP_ICON_MAIN_HLSL;

constexpr char kMapIconUvFacingVS[] =
    EDVR_MAP_COMMON_HLSL
    "cbuffer CB2 : register(b2) { float4 cb2[2]; };\n"
    "#define EDVR_MAP_ICON_UV " EDVR_MAP_ICON_UV_SCALED "\n"
    EDVR_MAP_ICON_MAIN_HLSL;

// 4EF6DDB075A927FA: the game's, with the eye origin taken out of a negative-scale record.
constexpr char kMapArrowDepthVS[] =
    EDVR_MAP_COMMON_HLSL
    R"HLSL(
cbuffer CB0 : register(b0) { float4 cb0[8]; };
cbuffer CB1 : register(b1) { float4 cb1[276]; };

void main(uint2 v0 : INSTANCEANDMODELDATAINDEX, uint4 v1 : PACKEDVERTEXDATAA, uint4 v2 : PACKEDVERTEXDATAB, uint4 v3 : PACKEDVERTEXDATAC,
          out float2 o0 : TEXCOORD0, out float4 o1 : SV_POSITION)
{
    float3 v = mapVertex(v1.xyz);
    float2 uv = float2(v2.x & 0xFFFFu, v2.x >> 16) * K32767 - 1.0;
    o0 = uv * 16.0;
    Rec rec = T33[v0.x];
    uint4 a = rec.a;
    v = mapSkin(a, v3.xy, v);
    float s = asfloat(a.y);
    float3 center = rec.p.xyz - ((s < 0.0) ? float3(0.0, 0.0, 0.0) : cb1[275].xyz);
    float4 q = mapQuat(a);
    float4 p = float4(mapRotate(q, v) * s + center, 1.0);
    o1 = float4(dot(cb0[4], p), dot(cb0[5], p), dot(cb0[6], p), dot(cb0[7], p));
}
)HLSL";

// The probe: the arrow's record is the only one with a negative scale; every other record's point is put outside clip space (x = 2 > w = 1) and rasterizes nothing.
constexpr char kMapProbeVS[] = R"HLSL(
struct Rec { uint4 a; float4 p; float4 pad[19]; };
StructuredBuffer<Rec> T33 : register(t33);
cbuffer CB0 : register(b0) { float4 cb0[8]; };
cbuffer CB1 : register(b1) { float4 cb1[276]; };

void main(uint2 v0 : INSTANCEANDMODELDATAINDEX, out float4 o0 : SV_POSITION)
{
    Rec rec = T33[v0.x];
    float s = asfloat(rec.a.y);
    float4 p = float4(rec.p.xyz - cb1[275].xyz, 1.0);
    o0 = (s >= 0.0) ? float4(2.0, 2.0, 2.0, 1.0) : float4(dot(cb0[4], p), dot(cb0[5], p), dot(cb0[6], p), dot(cb0[7], p));
}
)HLSL";

constexpr char kMapProbePS[] = R"HLSL(
Texture2D<float4> Mask : register(t0);

float4 main(float4 pos : SV_POSITION) : SV_TARGET
{
    if (Mask.Load(int3((int2)pos.xy, 0)).a < 0.1) discard;
    return float4(0.0, 0.0, 0.0, 0.0);
}
)HLSL";

} // namespace edvr
