// The VR Galaxy Map's replacement shaders (docs/galaxy-map-vr-2026-11-03.md, Fix 3 and "After the second flight"): the icons', labels' and rings' vertex shaders with every
// quad fixed in the world, the mouse arrow's vertex shader at the stars' depth, and the arrow probe's vertex and pixel shaders.
//
// THE FOUR TRANSCRIPTIONS (C31238331D8AC3D4, 12382D2EA45E9632, 4EF6DDB075A927FA, 94EA991C3A03DBD9) ARE TRANSCRIPTIONS, NOT REWRITES, AND EACH IS GRANTED TO ONE EXACT HASH
// (billboard_facing_vs.h says why that is the rule). Each was written from the disassembly of the game's own shader from `vs_5_0` down, and every float immediate was
// taken from the token hex, never from the text: the disassembler prints 1/32767 (0x38000100), 1/32767.5 (0x38000080), 1/4095.9 (0x39800080) and 1/255 (0x3B808081) all
// as 0.000031, 0.000031, 0.000244 and 0.003922 (visor_axis_vs.h learned that on F121). tools/galaxy_map_vs_test runs the transcriptions through stream output on WARP
// against the formulae below; the same program, started by hand with the game's own bytecode (vs_*.dxbc from the shader dump), compares every output of the game's shader
// with ours.
//
// WHAT EACH IS. The icons' three draw quads and sprites from one record each: t33 holds a 336-byte record (a.x the bone base, a.y the scale s as a float, a.zw the packed
// quaternion q, then the world position W at +16), t38 the bones (3 rows of float4, 48 bytes), and the vertex carries the packed local position v, the per-vertex
// billboard offset b (v2.z) and the uv (v2.x). The game's position is
//     p  = rotate(q, v) * s + (W - cb1[275].xyz)
//     p' = (dot(float4(p, 1), cb0[9]), cb0[10], cb0[11])          (the arrow: clip = dot(cb0[4..7], float4(p, 1)) directly)
//     clip = (p' + cb1[277].xyz * b.x + cb1[278].xyz * b.y) . (cb1[270], cb1[271], cb1[272]) + cb1[273]
// cb1[277] and cb1[278] are the EYE's right and up axes with the quad's size in their lengths, so the b span turns with the head.
//
// EVERY MAP QUAD IS FIXED IN THE WORLD. A quad's axes come from the ray from the eye to the quad and a head-free up, never from the eye's rotation:
//     F = normalize(ray);  c = cross(up, F);  R = length(c) > 0.02 ? c / length(c) : the eye's right made orthogonal to F;  U = cross(F, R)
// (right x up = forward in this game's frame; with the map camera's rows (1,0,0), (0,.7071,.7071), (0,-.7071,.7071) these are the map camera's axes when the ray is its
// forward and `up` is its up). `up` is stage 0's rows[1] of the map camera's pose (flat_stationary_camera_capture.h mapCameraPose), the direction the tracked up lands in; it
// reaches the shaders in a 16-byte constant buffer at vertex slot 10 (MapCam; galaxy_map_ui.cpp binds it; the game's 329 vertex shaders declare only slots 0, 1, 2, 3, 12, 13).
// Turning the head changes nothing in the world, only moving it does, by parallax. Spanning the quads on the eye's axes (build bf17a80a) locked them to the eye, and so does
// the game's own b span: the n=12 triangles, whose quaternion is the identity, are spanned by b alone.
//
// THE ICONS (C31238331D8AC3D4 and 12382D2EA45E9632, which differs only in the uv: `o0 = uv * cb2[1].xy * 16 + cb2[1].zw`). Their records carry a quaternion the CPU
// writes facing the HEAD-FREE map camera (rotate(q, ex) = -baseRight, rotate(q, ey) = +baseUp, rotate(q, ez) = -baseForward). Three kinds of record:
//   * W with |W|^2 <= 1e6: the UI frame's own records (map records sit at y of about 41070), left exactly as the game draws them, b span included;
//   * a world record (|W|^2 > 1e6) that does not face: a quad flat on the galactic plane (rotate(q, ez).y within 0.0005 of 1, the "10 : 88 : -100" labels and discs) or the
//     identity quaternion (rotate(q, ez).z at or above 0.9995, the n=12 triangles). The position is the game's maths;
//   * a world record that faces (the `faces` test): p' = pc' + s * (-v.x * R + v.y * U - v.z * F), pc' the quad's centre in the frame the clip rows read and R, U, F the axes
//     above from ray = pc' and up' = mapUp rotated by cb0[9..11]. The signs -v.x, +v.y, -v.z reproduce rotate(q, v).
// Every world record takes its b span in the same axes: R * b.x * sx * lx + U * b.y * sy * ly, lx and ly the lengths of cb1[277].xyz and cb1[278].xyz and sx and sy the
// signs of their dots with the eye's right and up (the eye's axes from cb1[270..272]: the x and y columns made orthogonal to the w column). The game's scale and sign stay,
// only the direction changes.
//
// THE RINGS (94EA991C3A03DBD9) span a per-instance world position v1 on cb1[277] and cb1[278] and take the clip rows cb0[4..7]; only the span's directions change: ray = v1 -
// cb1[275].xyz, up = mapUp, and the eye's axes from cb0[4].xyz, cb0[5].xyz and cb0[7].xyz. The stems (3028E7C6E0E097F9) build their width from cb1[275] already and stay the game's.
//
// THE ARROW (4EF6DDB075A927FA): the game's own, except that W - cb1[275].xyz becomes W - ((s < 0) ? 0 : cb1[275].xyz). The arrow's record is the only one with a negative
// scale (-0.402 in the dump); the same shader draws other sprites some frames (the loader's), which keep the game's depth. Drawn with the eye origin taken out, the arrow
// sits at the stars' depth, which is where the panels would be were they not at the game's.
//
// THE PROBE (galaxy_map_ui.h): one point at the arrow's tip, in the game's own depth (clip = dot(cb0[4..7], float4(W - cb1[275].xyz, 1)), the arrow's record position W
// being exactly the tip), tested against the panel mask: the pixel shader discards where the mask is clear, an occlusion query counts what stays. The tip is on a panel when the
// mask covers it, or covers a point within reach on both opposite sides of it along any of four axes: the gaps between panels close and a panel's outer edge stays exact.
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

// A map quad's axes (see the header): the forward from the ray, the right from the head-free up, and the eye's right (orthogonal to the forward) where the ray is within
// about a degree of the up and there is no right. Fe is the eye's forward, taken when the ray is zero.
void mapAxes(float3 ray, float3 up, float3 Fe, float3 Re, out float3 F, out float3 R, out float3 U) {
    F = (dot(ray, ray) > 1e-12) ? normalize(ray) : Fe;
    float3 c = cross(up, F);
    float cl = length(c);
    R = (cl > 0.02) ? c / cl : normalize(Re - dot(Re, F) * F);
    U = cross(F, R);
}
)HLSL"

// C31238331D8AC3D4. EDVR_MAP_ICON_UV is the uv statement; 12382D2EA45E9632 adds b2.
#define EDVR_MAP_ICON_MAIN_HLSL R"HLSL(
cbuffer CB0 : register(b0) { float4 cb0[12]; };
cbuffer CB1 : register(b1) { float4 cb1[279]; };
cbuffer MapCam : register(b10) { float4 mapUp; };

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
    bool world = dot(W, W) > 1000000.0;
    bool faces = world && abs(n.y) < 0.9995 && n.z < 0.9995;
    float4 pc = float4(center, 1.0);
    float3 F = 0, R = 0, U = 0;
    float sx = 1.0, sy = 1.0;
    if (world) {
        float3 Fe = normalize(float3(cb1[270].w, cb1[271].w, cb1[272].w));
        float3 Rx = float3(cb1[270].x, cb1[271].x, cb1[272].x);
        float3 Re = normalize(Rx - dot(Rx, Fe) * Fe);
        float3 Ry = float3(cb1[270].y, cb1[271].y, cb1[272].y);
        float3 Ue = normalize(Ry - dot(Ry, Fe) * Fe);
        float4 up4 = float4(mapUp.xyz, 0.0);
        mapAxes(float3(dot(pc, cb0[9]), dot(pc, cb0[10]), dot(pc, cb0[11])), float3(dot(up4, cb0[9]), dot(up4, cb0[10]), dot(up4, cb0[11])), Fe, Re, F, R, U);
        sx = (dot(cb1[277].xyz, Re) < 0.0) ? -1.0 : 1.0;
        sy = (dot(cb1[278].xyz, Ue) < 0.0) ? -1.0 : 1.0;
    }
    if (faces) {
        pp = float3(dot(pc, cb0[9]), dot(pc, cb0[10]), dot(pc, cb0[11])) + s * (-v.x * R + v.y * U - v.z * F);
    } else {
        float4 p = float4(mapRotate(q, v) * s + center, 1.0);
        pp = float3(dot(p, cb0[9]), dot(p, cb0[10]), dot(p, cb0[11]));
    }
    if (world) {
        pp = R * (b.x * sx * length(cb1[277].xyz)) + pp;
        pp = U * (b.y * sy * length(cb1[278].xyz)) + pp;
    } else {
        pp = cb1[277].xyz * b.x + pp;
        pp = cb1[278].xyz * b.y + pp;
    }
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

// 94EA991C3A03DBD9: the rings. The game's shader, with the span's two directions (cb1[277] and cb1[278], the eye's right and up) put on the ray's axes (see the header).
constexpr char kMapRingVS[] =
    EDVR_MAP_COMMON_HLSL
    R"HLSL(
cbuffer CB0 : register(b0) { float4 cb0[8]; };
cbuffer CB1 : register(b1) { float4 cb1[279]; };
cbuffer CB2 : register(b2) { float4 cb2[1]; };
cbuffer MapCam : register(b10) { float4 mapUp; };

void main(float4 v0 : TEXCOORD0, float3 v1 : POSITION0, float4 v2 : POSITION2, float4 v3 : TEXCOORD3, float4 v4 : POSITION4,
          out float4 o0 : TEXCOORD0, out float4 o1 : TEXCOORD1, out float3 o2 : TEXCOORD2, out float2 o3 : TEXCOORD4, out float3 o4 : TEXCOORD5,
          out float4 o5 : SV_POSITION)
{
    o0 = float4(v1, v3.z);
    float3 l = log2(v4.xyz) * cb2[0].xxx;
    o1 = float4(exp2(l), v4.w);
    float2 b = v0.zw * v3.z;
    float3 Fe = normalize(cb0[7].xyz);
    float3 Rx = cb0[4].xyz;
    float3 Re = normalize(Rx - dot(Rx, Fe) * Fe);
    float3 Ry = cb0[5].xyz;
    float3 Ue = normalize(Ry - dot(Ry, Fe) * Fe);
    float3 F, R, U;
    mapAxes(v1 - cb1[275].xyz, mapUp.xyz, Fe, Re, F, R, U);
    float sx = (dot(cb1[277].xyz, Re) < 0.0) ? -1.0 : 1.0;
    float sy = (dot(cb1[278].xyz, Ue) < 0.0) ? -1.0 : 1.0;
    float3 r0 = R * (b.x * sx * length(cb1[277].xyz)) + v1;
    r0 = U * (b.y * sy * length(cb1[278].xyz)) + r0;
    o2 = r0;
    o3 = v2.xy;
    o4 = float3(v0.xy, v2.w);
    float4 p = float4(r0, 1.0);
    o5 = float4(dot(cb0[4], p), dot(cb0[5], p), dot(cb0[6], p), dot(cb0[7], p));
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


// kProbeReach is the gap-bridging reach as a fraction of the mask's width: 0.0235 is about 0.05 tan, 106 pixels at 4508. kProbeSteps is the number of samples a side along
// each axis, so one step is an eighth of the reach (about 13 pixels at 4508).
constexpr char kMapProbePS[] = R"HLSL(
Texture2D<float4> Mask : register(t0);

static const float kProbeReach = 0.0235;
static const int kProbeSteps = 8;
static const float2 kProbeAxis[4] = { float2(1.0, 0.0), float2(0.0, 1.0), float2(0.7071068, 0.7071068), float2(0.7071068, -0.7071068) };

bool covered(int2 p) { return Mask.Load(int3(p, 0)).a >= 0.1; }

float4 main(float4 pos : SV_POSITION) : SV_TARGET
{
    int2 tip = (int2)pos.xy;
    bool on = covered(tip);
    if (!on) {
        uint w, h;
        Mask.GetDimensions(w, h);
        float stride = kProbeReach * (float)w / (float)kProbeSteps;
        [loop] for (int a = 0; a < 4 && !on; ++a) {
            bool plus = false, minus = false;
            [loop] for (int k = 1; k <= kProbeSteps; ++k) {
                int2 o = (int2)round(kProbeAxis[a] * (stride * (float)k));
                plus = plus || covered(tip + o);
                minus = minus || covered(tip - o);
            }
            on = plus && minus;
        }
    }
    if (!on) discard;
    return float4(0.0, 0.0, 0.0, 0.0);
}
)HLSL";

} // namespace edvr
