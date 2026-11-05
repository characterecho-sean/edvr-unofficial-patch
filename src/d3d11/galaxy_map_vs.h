// The VR Galaxy Map's replacement shaders (docs/galaxy-map-vr-2026-11-03.md, Fix 3 and "After the second flight"): the icons', labels', rings' and stems' vertex shaders with every
// quad fixed in the world and the overlays' positions held steady against the game's rounded eye, the mouse arrow's vertex shader at the stars' depth, and the arrow probe's vertex and
// pixel shaders.
//
// THE FIVE TRANSCRIPTIONS (C31238331D8AC3D4, 12382D2EA45E9632, 4EF6DDB075A927FA, 94EA991C3A03DBD9, 3028E7C6E0E097F9) ARE TRANSCRIPTIONS, NOT REWRITES, AND EACH IS GRANTED TO ONE EXACT HASH
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
// EVERY MAP QUAD IS FIXED IN THE WORLD, ON ONE PAIR OF AXES FOR THE FRAME (docs/galaxy-map-vr-2026-11-03.md, "After the fifth flight"). The axes are R, U and F = normalize(cross(R, U)), written by
// the CPU into MapCam (galaxy_map_axes.h mapCamAxes) from stage 0's rows of the map camera's pose (right rows[0], up rows[1], forward rows[2]). Every quad is spanned on rows[0] and rows[1]:
// the game's own planes, with the triangles joining them. The grid's vertical in place of rows[1] was flown and declined: it stands vertical while the camera looks down, so with the camera
// 45 degrees down a quad at view tangents (x, y) has 1 + y times its height at the centre and shears by atan(x / (1 + y)). No ray, no eye row and no position enters an axis, so turning the head changes nothing in the world (only moving it does, by parallax) and a camera moved sideways turns no quad. An axis pair from each quad's own ray
// (builds ca707034 to e02c6248) turned every quad about the grid's vertical as the camera moved sideways, 14 degrees per unit at 4 units, and fanned the label baselines from +29 to -17
// degrees across one image; spanning the quads on the eye's axes (build bf17a80a) locked them to the eye, and so does the game's own b span: the n=12 triangles, whose quaternion is the
// identity, are spanned by b alone. MapCam reaches the shaders in a 96-byte constant buffer at vertex slot 10 (R, U, F, then the position and the two eyes' offsets, see THE WOBBLE;
// galaxy_map_ui.cpp binds it; the game's 329 vertex shaders declare only slots 0, 1, 2, 3, 12, 13). Right x up = forward in this game's frame.
//
// THE ICONS (C31238331D8AC3D4 and 12382D2EA45E9632, which differs only in the uv: `o0 = uv * cb2[1].xy * 16 + cb2[1].zw`). Their records carry a quaternion the CPU
// writes facing the HEAD-FREE map camera (rotate(q, ex) = -baseRight, rotate(q, ey) = +baseUp, rotate(q, ez) = -baseForward). Three kinds of record:
//   * W with |W|^2 <= 1e6: the UI frame's own records (map records sit at y of about 41070), left exactly as the game draws them, b span included;
//   * a world record (|W|^2 > 1e6) that does not face: a quad flat on the galactic plane (rotate(q, ez).y within 0.0005 of 1, the "10 : 88 : -100" labels and discs) or the
//     identity quaternion (rotate(q, ez).z at or above 0.9995, the n=12 triangles). The position is the game's maths;
//   * a world record that faces (the `faces` test): p' = pc' + s * (-v.x * R + v.y * U - v.z * F), pc' the quad's centre in the frame the clip rows read and R, U, F the MapCam axes in
//     that frame (each through cb0[9..11] with w = 0). The signs -v.x, +v.y, -v.z reproduce rotate(q, v).
// Every world record takes its b span in the same axes: R * b.x * sx * lx + U * b.y * sy * ly, lx and ly the lengths of cb1[277].xyz and cb1[278].xyz and sx and sy the
// signs of their dots with the eye's right and up (the eye's axes from cb1[270..272]: the x and y columns made orthogonal to the w column). The game's scale and sign stay,
// only the direction changes. Icons and labels share one main.
//
// THE RINGS (94EA991C3A03DBD9) span a per-instance world position v1 on cb1[277] and cb1[278] and take the clip rows cb0[4..7]; only the span's directions change: R and U are MapCam's,
// as they are (the rings are in the world frame already), and the eye's axes, from cb0[4].xyz, cb0[5].xyz and cb0[7].xyz, give the signs.
//
// THE WOBBLE (docs/galaxy-map-vr-2026-11-03.md, "The wobble"). The game's eye position in the map frame, cb1[275].xyz, is a float32 at about 50000 units where one step is
// 0.0039, and it moves in jumps of several steps; every quad's position is W - cb1[275].xyz, so an overlay 4 units from the eye shifts by pixels from frame to frame. The precise parts
// exist: stage 0's position (MapCam.mapPos.xyz, constant at rest) and each eye's offset from it (MapCam.mapEye[i].xyz, from the UI frame's two cameras, held near zero in float32).
// mapEyeRel puts the position relative to the eye as (W - mapPos) - mapEye[i], where W - mapPos is exact for near values. It applies only while the game's eye is within kMapEyeBand of
// one of the two parts' eyes (the map is at rest, or the parts agree with the game); otherwise, and when mapPos.w is 0 (the parts are not valid), the result is the game's own W -
// cb1[275].xyz. The icons and labels take their centre from it for a world record; the rings and the stems take their clip position from it (the game's clip rows carry a translation
// of about -36615, which rounds the same way), with the span added to the exact relative position rather than to the world one, which would round it to the grid again. The stars and
// the sky stay the game's: only the overlays are steadied.
//
// THE ARROW (4EF6DDB075A927FA): the game's own, except that W - cb1[275].xyz becomes W - ((s < 0) ? 0 : cb1[275].xyz). The arrow's record is the only one with a negative
// scale (-0.402 in the dump); the same shader draws other sprites some frames (the loader's), which keep the game's depth. Drawn with the eye origin taken out, the arrow
// sits at the stars' depth, which is where the panels would be were they not at the game's.
//
// THE PROBE (galaxy_map_ui.h): one point at the arrow's tip, in the game's own depth (clip = dot(cb0[4..7], float4(W - cb1[275].xyz, 1)), the arrow's record position W
// being exactly the tip), tested against the panel mask: the pixel shader discards where the mask is clear, an occlusion query counts what stays. The tip is on a panel when the
// mask covers it, or covers a point within reach on both opposite sides of it along any of four axes: the gaps between panels close and the way onto a panel stays exact. While the arrow is on a panel the probe is kMapProbeStickyPS: mask within reach on
// either side holds it, so a narrow strip of buttons does not drop it at its outer edge.
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

// The map camera's constants at vertex slot 10, and the precise position relative to the eye (see THE WOBBLE). mapR, mapU and mapF are the one pair of axes the world quads are spanned on and their forward (galaxy_map_axes.h; w 0); mapPos.xyz is stage 0's position and mapPos.w 1 when the
// two eyes' offsets are valid, 0 when not; mapEye[i].xyz is eye i's offset from mapPos in the map frame. kMapEyeBand is the distance in units, 0.04, within which the game's rounded
// eye has to be of one of the two parts' eyes for the parts to be used: several steps of float32 at 50000 units (0.0039 each), more than the game's rounding ever shows at rest; while the
// map moves its pose can lag a frame behind the eye, and the game's own eye is right then.
#define EDVR_MAP_EYE_BAND 0.04
#define EDVR_MAP_STR2(x) #x
#define EDVR_MAP_STR(x) EDVR_MAP_STR2(x)
#define EDVR_MAP_CAM_HLSL \
    "static const float kMapEyeBand = " EDVR_MAP_STR(EDVR_MAP_EYE_BAND) ";" \
    R"HLSL(
cbuffer MapCam : register(b10) { float4 mapR; float4 mapU; float4 mapF; float4 mapPos; float4 mapEye[2]; };

// eyeGame is the game's rounded eye. False when the precise parts do not apply, and rel is then the game's W - eyeGame.
bool mapEyeRel(float3 W, float3 eyeGame, out float3 rel) {
    rel = W - eyeGame;
    if (mapPos.w == 0.0) return false;
    float3 g = eyeGame - mapPos.xyz;
    float3 a = g - mapEye[0].xyz, b = g - mapEye[1].xyz;
    bool first = dot(a, a) <= dot(b, b);
    float3 d = first ? a : b;
    if (dot(d, d) > kMapEyeBand * kMapEyeBand) return false;
    rel = (W - mapPos.xyz) - (first ? mapEye[0].xyz : mapEye[1].xyz);
    return true;
}
)HLSL"

constexpr float kMapEyeBand = static_cast<float>(EDVR_MAP_EYE_BAND);

// C31238331D8AC3D4. EDVR_MAP_ICON_UV is the uv statement; 12382D2EA45E9632 adds b2. Both are spanned on MapCam's one pair of axes.
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
    bool world = dot(W, W) > 1000000.0;
    float3 center = W - cb1[275].xyz;
    if (world) mapEyeRel(W, cb1[275].xyz, center);
    float4 q = mapQuat(a);
    float3 pp;
    float3 n = mapRotate(q, float3(0.0, 0.0, 1.0));
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
        float4 R4 = float4(mapR.xyz, 0.0), U4 = float4(mapU.xyz, 0.0), F4 = float4(mapF.xyz, 0.0);
        R = float3(dot(R4, cb0[9]), dot(R4, cb0[10]), dot(R4, cb0[11]));
        U = float3(dot(U4, cb0[9]), dot(U4, cb0[10]), dot(U4, cb0[11]));
        F = float3(dot(F4, cb0[9]), dot(F4, cb0[10]), dot(F4, cb0[11]));
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
    EDVR_MAP_CAM_HLSL
    "#define EDVR_MAP_ICON_UV " EDVR_MAP_ICON_UV_PLAIN "\n"
    EDVR_MAP_ICON_MAIN_HLSL;

constexpr char kMapIconUvFacingVS[] =
    EDVR_MAP_COMMON_HLSL
    EDVR_MAP_CAM_HLSL
    "cbuffer CB2 : register(b2) { float4 cb2[2]; };\n"
    "#define EDVR_MAP_ICON_UV " EDVR_MAP_ICON_UV_SCALED "\n"
    EDVR_MAP_ICON_MAIN_HLSL;

// 4EF6DDB075A927FA: the game's, with the eye origin taken out of a negative-scale record.
constexpr char kMapArrowDepthVS[] =
    EDVR_MAP_COMMON_HLSL
    R"HLSL(
cbuffer CB0 : register(b0) { float4 cb0[8]; };
cbuffer CB1 : register(b1) { float4 cb1[276]; };
cbuffer MapBodies : register(b11) { float4 mapBody[32]; float4 mapArrow; };

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
    // The arrow's depth: W - eye is the arrow at |W|; W - eye * |W| / D is the same arrow, at the same size and on the same ray, at distance D. mapArrow.x is 1 / D: 0 on the Galaxy Map, whose stars are at infinity, and on the
    // System Map just over the body it points at (galaxy_map_ui.h rayBodyDistance). Bodies there can be 2 units away, where an arrow at infinity sits a good way behind them in stereo.
    float3 center = rec.p.xyz - ((s < 0.0) ? cb1[275].xyz * (length(rec.p.xyz) * mapArrow.x) : cb1[275].xyz);
    float4 q = mapQuat(a);
    float4 p = float4(mapRotate(q, v) * s + center, 1.0);
    o1 = float4(dot(cb0[4], p), dot(cb0[5], p), dot(cb0[6], p), dot(cb0[7], p));
}
)HLSL";

// 94EA991C3A03DBD9: the rings. The game's shader, with the span's two directions (cb1[277] and cb1[278], the eye's right and up) put on MapCam's axes (see the header), and the clip
// position from the eye-relative position when mapEyeRel holds: the clip rows' xyw take the eye to (0, 0, 0) (their translation is minus the rows . the eye), and the z row is the
// constant cb0[6].w. o2 stays the game's world position.
constexpr char kMapRingVS[] =
    EDVR_MAP_COMMON_HLSL
    EDVR_MAP_CAM_HLSL
    R"HLSL(
cbuffer CB0 : register(b0) { float4 cb0[8]; };
cbuffer CB1 : register(b1) { float4 cb1[279]; };
cbuffer CB2 : register(b2) { float4 cb2[1]; };

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
    float3 ray;
    const bool steady = mapEyeRel(v1, cb1[275].xyz, ray);
    float3 R = mapR.xyz, U = mapU.xyz;
    float sx = (dot(cb1[277].xyz, Re) < 0.0) ? -1.0 : 1.0;
    float sy = (dot(cb1[278].xyz, Ue) < 0.0) ? -1.0 : 1.0;
    float3 spanR = R * (b.x * sx * length(cb1[277].xyz));
    float3 spanU = U * (b.y * sy * length(cb1[278].xyz));
    float3 r0 = spanR + v1;
    r0 = spanU + r0;
    o2 = r0;
    o3 = v2.xy;
    o4 = float3(v0.xy, v2.w);
    if (steady) {
        float3 rel = (ray + spanR) + spanU;
        o5 = float4(dot(cb0[4].xyz, rel), dot(cb0[5].xyz, rel), dot(cb0[6].xyz, rel) + cb0[6].w, dot(cb0[7].xyz, rel));
    } else {
        float4 p = float4(r0, 1.0);
        o5 = float4(dot(cb0[4], p), dot(cb0[5], p), dot(cb0[6], p), dot(cb0[7], p));
    }
}
)HLSL";

// 3028E7C6E0E097F9: the stems (the lines from a star to its foot on the grid), vs_5_0 down from the disassembly. A line from v2 to v3 is widened across its length towards the eye:
// the width axis is cross(eye - v2, v2 -> v3) normalised, and the vertex is v3 or v2 pushed by it (v1.x above 1.5 for the end at v3). The game's shader, with the eye - v2 and the
// clip position from the eye-relative position when mapEyeRel holds; o2 stays the game's world position, and every other output is the game's. Its pixel shader is 6C28D59784CC493D
// (additive, in the map-marks family). The only immediates are 1.5 and 1, and the operations run in the order the bytecode has them.
constexpr char kMapStemVS[] =
    EDVR_MAP_COMMON_HLSL
    EDVR_MAP_CAM_HLSL
    R"HLSL(
cbuffer CB0 : register(b0) { float4 cb0[8]; };
cbuffer CB1 : register(b1) { float4 cb1[276]; };

void main(float4 v0 : TEXCOORD0, float v1 : TEXCOORD1, float3 v2 : POSITION0, float3 v3 : POSITION1, float4 v4 : TEXCOORD3, float4 v5 : POSITION4, float4 v6 : TEXCOORD5,
          out float4 o0 : TEXCOORD2, out float4 o1 : TEXCOORD3, out float3 o2 : TEXCOORD4, out float2 o3 : TEXCOORD6, out float4 o4 : SV_POSITION)
{
    float3 rel2, rel3;
    const bool steady = mapEyeRel(v2, cb1[275].xyz, rel2);
    mapEyeRel(v3, cb1[275].xyz, rel3);
    float3 e = -rel2;                                  // eye - v2: the game's, or the exact one
    float3 r0 = v3.yzx - v2.yzx;
    r0 = rsqrt(dot(r0, r0)) * r0;
    float3 r1 = e.zxy;
    r1 = rsqrt(dot(r1, r1)) * r1;
    // precise keeps the cross product's two products as the bytecode has them: left alone the compiler swaps which of them is fused, and 25 of 1200 random instances then differ by an ULP or three
    precise float3 r2 = r0 * r1;
    precise float3 c = r1.zxy * r0.yzx - r2;
    r0 = c;
    r0 = rsqrt(dot(r0, r0)) * r0;
    o3.y = v0.y / v6.y + v6.x / v6.y;
    float3 rel;
    if (1.5 < v1) {
        float w = v0.w * v4.z;
        r1 = r0 * w + v3;
        rel = rel3 + r0 * w;
        o3.x = sqrt(dot(v2 - v3, v2 - v3)) * v6.z + v6.w;
    } else {
        float w = v0.w * v4.z;
        r1 = r0 * w + v2;
        rel = rel2 + r0 * w;
        o3.x = v6.w;
    }
    o0 = float4(cb1[275].xyz, sqrt(dot(cb1[275].xyz, cb1[275].xyz)));
    if (steady) {
        o4 = float4(dot(cb0[4].xyz, rel), dot(cb0[5].xyz, rel), dot(cb0[6].xyz, rel) + cb0[6].w, dot(cb0[7].xyz, rel));
    } else {
        float4 p = float4(r1, 1.0);
        o4 = float4(dot(cb0[4], p), dot(cb0[5], p), dot(cb0[6], p), dot(cb0[7], p));
    }
    o1 = v5;
    o2 = r1;
}
)HLSL";

// The probe: the arrow's record is the only one with a negative scale; every other record's point is put outside clip space (x = 2 > w = 1) and rasterizes nothing.
// THE SYSTEM MAP'S MARKERS (docs/galaxy-map-vr-2026-11-03.md, "The System Map"). 12382D2EA45E9632 as the game has it, except that a record at a known body's centre is drawn k times larger about its own position, its vertices and its b span alike. MapBodies, vertex slot 11, holds the centres
// and each k (galaxy_map_ui.cpp); a zero k ends the list, and a record at no known body is the game's. The match is 0.05 units: the records' positions and the bodies' constants round the same number apart.
constexpr uint32_t kMapBodies = 32;
// MapBodies, by row. kMapMarkerVS and kMapArrowDepthVS read only as far as the arrow's row.
constexpr uint32_t kMapArrowRow = kMapBodies;        // x: 1 / the arrow's depth
constexpr uint32_t kMapCameraRow = kMapBodies + 1;   // xyz: the map camera's position; w: 1 when it is known
constexpr uint32_t kMapRadiusRow = kMapBodies + 2;   // the bodies' surface radii, four to a row
constexpr uint32_t kMapBodyRows = kMapRadiusRow + kMapBodies / 4;
// A body's shell: the settlements' records stand at 1.0157 of its surface radius (dump 103332590001: 0.425 on 0.4184). kMapArrowFloat is added to 1 / distance, which keeps it the same stereo step at any zoom: 3.8 pixels of disparity at 4032 per eye, measured at the dump's 96 pixels per 1 / unit.
#define EDVR_MAP_BODY_SHELL 1.016
#define EDVR_MAP_ARROW_FLOAT 0.04
#define EDVR_MAP_ARROW_MIN 0.25
constexpr float kMapBodyShell = static_cast<float>(EDVR_MAP_BODY_SHELL);
constexpr float kMapArrowFloat = static_cast<float>(EDVR_MAP_ARROW_FLOAT);
constexpr float kMapArrowMin = static_cast<float>(EDVR_MAP_ARROW_MIN);

constexpr char kMapMarkerVS[] =
    EDVR_MAP_COMMON_HLSL
    R"HLSL(
cbuffer CB0 : register(b0) { float4 cb0[12]; };
cbuffer CB1 : register(b1) { float4 cb1[279]; };
cbuffer CB2 : register(b2) { float4 cb2[2]; };
cbuffer MapBodies : register(b11) { float4 mapBody[32]; float4 mapArrow; };

void main(uint2 v0 : INSTANCEANDMODELDATAINDEX, uint4 v1 : PACKEDVERTEXDATAA, uint4 v2 : PACKEDVERTEXDATAB, uint4 v3 : PACKEDVERTEXDATAC,
          out float2 o0 : TEXCOORD0, out float4 o1 : SV_POSITION)
{
    float3 v = mapVertex(v1.xyz);
    float4 u = float4(v2.z & 0xFFFFu, v2.z >> 16, v2.x & 0xFFFFu, v2.x >> 16) * K32767 - 1.0;
    float2 b = u.xy * 16.0;
    o0 = (u.zw * cb2[1].xy) * 16.0 + cb2[1].zw;
    Rec rec = T33[v0.x];
    uint4 a = rec.a;
    v = mapSkin(a, v3.xy, v);
    float s = asfloat(a.y);
    float3 W = rec.p.xyz;
    float k = 1.0;
    [loop] for (uint i = 0u; i < 32u; ++i) {
        if (mapBody[i].w == 0.0) break;
        float3 d = W - mapBody[i].xyz;
        if (dot(d, d) < 0.0025) { k = mapBody[i].w; break; }
    }
    float4 p = float4(mapRotate(mapQuat(a), v) * (s * k) + (W - cb1[275].xyz), 1.0);
    float3 pp = float3(dot(p, cb0[9]), dot(p, cb0[10]), dot(p, cb0[11]));
    pp = cb1[277].xyz * (b.x * k) + pp;
    pp = cb1[278].xyz * (b.y * k) + pp;
    o1 = pp.y * cb1[271];
    o1 = pp.x * cb1[270] + o1;
    o1 = pp.z * cb1[272] + o1;
    o1 = o1 + cb1[273];
}
)HLSL";

// C31238331D8AC3D4 on the System Map: the reticule round the focused body, whose record stands at the body's centre, so in stereo it sat inside the planet. As the game has it, except that a record at a known body takes the eye term scaled, as the arrow's does:
// rel - (eye - camera) * F is the same quad, at the same size and on the same ray, at distance |rel| / F. Here that is just over the body's shell, the arrow's height. A record at no known body, or no camera yet, is the game's.
constexpr char kMapReticuleVS[] =
    EDVR_MAP_COMMON_HLSL
    "static const float kMapBodyShell = " EDVR_MAP_STR(EDVR_MAP_BODY_SHELL) ";"
    "static const float kMapArrowFloat = " EDVR_MAP_STR(EDVR_MAP_ARROW_FLOAT) ";"
    "static const float kMapArrowMin = " EDVR_MAP_STR(EDVR_MAP_ARROW_MIN) ";"
    R"HLSL(
cbuffer CB0 : register(b0) { float4 cb0[12]; };
cbuffer CB1 : register(b1) { float4 cb1[279]; };
cbuffer MapBodies : register(b11) { float4 mapBody[32]; float4 mapArrow; float4 mapCamera; float4 mapRadius[8]; };

void main(uint2 v0 : INSTANCEANDMODELDATAINDEX, uint4 v1 : PACKEDVERTEXDATAA, uint4 v2 : PACKEDVERTEXDATAB, uint4 v3 : PACKEDVERTEXDATAC,
          out float2 o0 : TEXCOORD0, out float4 o1 : SV_POSITION)
{
    float3 v = mapVertex(v1.xyz);
    float4 u = float4(v2.z & 0xFFFFu, v2.z >> 16, v2.x & 0xFFFFu, v2.x >> 16) * K32767 - 1.0;
    float2 b = u.xy * 16.0;
    o0 = u.zw * 16.0;
    Rec rec = T33[v0.x];
    uint4 a = rec.a;
    v = mapSkin(a, v3.xy, v);
    float s = asfloat(a.y);
    float3 W = rec.p.xyz;
    float3 center = W - cb1[275].xyz;
    if (mapCamera.w != 0.0) {
        [loop] for (uint i = 0u; i < 32u; ++i) {
            if (mapBody[i].w == 0.0) break;
            float3 d = W - mapBody[i].xyz;
            if (dot(d, d) < 0.0025) {
                float3 rel = W - mapCamera.xyz;
                float dist = length(rel);
                float surface = max(dist - mapRadius[i >> 2][i & 3u] * kMapBodyShell, kMapArrowMin);
                center = rel - (cb1[275].xyz - mapCamera.xyz) * (dist * (1.0 / surface + kMapArrowFloat));
                break;
            }
        }
    }
    float4 p = float4(mapRotate(mapQuat(a), v) * s + center, 1.0);
    float3 pp = float3(dot(p, cb0[9]), dot(p, cb0[10]), dot(p, cb0[11]));
    pp = cb1[277].xyz * b.x + pp;
    pp = cb1[278].xyz * b.y + pp;
    o1 = pp.y * cb1[271];
    o1 = pp.x * cb1[270] + o1;
    o1 = pp.z * cb1[272] + o1;
    o1 = o1 + cb1[273];
}
)HLSL";

// THE CUT-OUT BEHIND THE BODIES (ui_layer.cpp mapBodiesPunch). On the System Map the game draws the grid and the stems before most bodies and lets the bodies cover them. In the layer they would lie over the bodies, so at the first draw that comes after the bodies the layer goes back to clear wherever the
// game's depth holds anything. The mapping is the seed's (fixed_shader_source.h kUiSeedPsDepthNoStencil), the jitter included; the clear is kUiLayerClear. The depth is reversed and cleared to 0.
constexpr char kMapPunchVS[] = R"HLSL(
float4 main(uint id : SV_VertexID) : SV_POSITION {
    float2 p = id == 0u ? float2(-1.0, -1.0) : (id == 1u ? float2(-1.0, 3.0) : float2(3.0, -1.0));
    return float4(p, 0.0, 1.0);
}
)HLSL";
constexpr char kMapPunchPS[] = R"HLSL(
cbuffer CB0 : register(b0) { uint2 inSize; uint2 outSize; float2 jitter; float2 pad; };
Texture2D<float> D : register(t0);
float4 main(float4 p : SV_POSITION) : SV_Target {
    int2 q = int2(floor(p.xy * float2(inSize) / float2(outSize) + jitter));
    q = clamp(q, int2(0, 0), int2(inSize) - 1);
    if (D.Load(int3(q, 0)) <= 0.0) discard;
    return float4(0.0, 0.0, 0.0, 1.0);
}
)HLSL";

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
// kMapProbePS decides getting on: both sides of the tip. kMapProbeStickyPS decides staying on: either side. Whatever the first says yes to the second does too, so they cannot alternate at one place. The reach is more than half a side button's width, 95 pixels at 4032 against about 165 (docs/galaxy-map-vr-2026-11-03.md, "The side buttons").
#define EDVR_MAP_PROBE_PS_HEAD R"HLSL(
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
            on = )HLSL"
#define EDVR_MAP_PROBE_PS_TAIL R"HLSL(;
        }
    }
    if (!on) discard;
    return float4(0.0, 0.0, 0.0, 0.0);
}
)HLSL"
constexpr char kMapProbePS[] = EDVR_MAP_PROBE_PS_HEAD "plus && minus" EDVR_MAP_PROBE_PS_TAIL;
constexpr char kMapProbeStickyPS[] = EDVR_MAP_PROBE_PS_HEAD "plus || minus" EDVR_MAP_PROBE_PS_TAIL;

// THE DEPTH ISSUE'S PIXEL SHADERS (docs/galaxy-map-vr-2026-11-03.md, "Sprite order"). The sprites' colour issue tests the map's own D16 depth and writes none; a second issue after the
// colour issue writes it (no stencil write-back comes between them), where the sprite is solid. Each shader below stands in for the game's pixel shader of that issue, bound over the swap scope's vertex shader, and does
// the game's alpha and a clip: 964221955A9E77E7 (the Galaxy Map's icon sprite) has alpha t0.a * cb2[1].w and 855C469156AB997F (the canvas composite) t0.a (disassembled; the
// colour and the game's own discard of an all-zero result are not needed, the depth alone is written). kMapDepthAlpha is the alpha at which a sprite is solid for the depth: the
// see-through parts of a sprite block nothing, its solid parts block what is drawn after them.
#define EDVR_MAP_DEPTH_ALPHA 0.3
constexpr float kMapDepthAlpha = static_cast<float>(EDVR_MAP_DEPTH_ALPHA);

// 964221955A9E77E7
constexpr char kMapDepthSpritePS[] =
    "static const float kMapDepthAlpha = " EDVR_MAP_STR(EDVR_MAP_DEPTH_ALPHA) ";"
    R"HLSL(
Texture2D T0 : register(t0);
SamplerState S0 : register(s0);
cbuffer CB2 : register(b2) { float4 cb2[2]; };

void main(float2 uv : TEXCOORD0)
{
    if (T0.Sample(S0, uv).a * cb2[1].w < kMapDepthAlpha) discard;
}
)HLSL";

// 855C469156AB997F
constexpr char kMapDepthCanvasPS[] =
    "static const float kMapDepthAlpha = " EDVR_MAP_STR(EDVR_MAP_DEPTH_ALPHA) ";"
    R"HLSL(
Texture2D T0 : register(t0);
SamplerState S0 : register(s0);

void main(float2 uv : TEXCOORD0)
{
    if (T0.Sample(S0, uv).a < kMapDepthAlpha) discard;
}
)HLSL";

} // namespace edvr
