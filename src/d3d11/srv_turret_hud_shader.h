#pragma once
// The SRV turret HUD's private geometry shaders (srv_turret.h says why): one pass-through for each distinct vertex-shader OUTPUT SIGNATURE among the families that draw the turret
// view's HUD (srv_turret_math.h: kFamilies, kSignatures). experimental.srv_turret_systems_near writes a second radial window on the panels group (vertices past 1 m and under 4 m). That window is not
// confirmed as the power and systems panel. Compiled at BUILD time by tools/temporal_shader_build into kSrvTurretHudGs<X>Bytecode (never in the game), created by
// shaderSwapCreateGs.
//
// WHAT IT DOES. In the turret view the weapon panels (the weapon arc brackets, the weapon names, the ammo counts, the INFO strip) are drawn from records about 0.27 to 0.70 m
// from the eye, the radar from 0.11 to 0.44 m, and the larger hologram panels from 1.3 m and more. The systems-near window covers those larger-panel vertices and leaves the weapon panels inside 1 m alone.
// Every shader below is the same triangle-list pass-through
// that moves each vertex in 3D (one shared function, turretMove, so the eight copies cannot drift): it reconstructs the vertex's eye-space position P from its clip position and the
// eye's projection tangents (left, right, down, up as the game projects, the eye jitter included; NDC y is up), translates it by T (the HUD-forward key; every vertex), moves it along
// its ray from the ANCHOR A (a fixed point in the seated frame, in the eye's space here) by `gap` metres nearer (a NEGATIVE gap: deeper, the push-back) when its distance from A lies in
// (dMin, dMax), and projects the result back with the same tangents:
//
//     P1 = P + T;   V = P1 - A;   d = |V|;   Q = (dMin < d < dMax) ? A + V (d - gap) / d : P1;   pos = project(Q), z' = ndcZ * Q.z
//     T = tr.xyz, or tr.xyz * w / z_c when tr.w = 1 / z_c > 0 (THE CONTENT DEPTH, srv_turret_math.h: the radar billboards move on screen as their picture's content at z_c would)
//     ndcZ = z / w when w > 0 and 0 <= z / w <= 1 (the game's own depth), else 0.999        (depth mode game, the default)
//     ndcZ = clamp(z / Q.z, 0, 0.999), with the vertex's own clip z                           (depth mode real: the moved position's own depth)
//     ndcZ = 0.999 for every moved vertex                                                    (depth mode front)
// The mode is the fourth component of the first constant register (srv_turret_math.h DepthMode).
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
// THE CONSTANT BUFFER is private to the shader (GS slot 0, which the game never binds): five registers, srv_turret_math.h's TurretParams -- {gap, dMin, dMax, depth mode}, {left, right, down, up},
// {the anchor in the eye's space: X right, Y up, Z forward; the lock}, {the translation in the same axes, and 1 / the content depth or 0}, {the curved game panel's pull: gap2, dMin2, dMax2, 0}.
//
// THE LOCK (an.w = 1; srv_turret_math.h THE LOCK; only the radar billboards' draw-time correction sets it): tr.xyz is then the billboard's target in the eye's space, and every vertex of a
// triangle moves by tr.xyz - C, C the midpoint of the triangle's longest edge, its eye-space points rebuilt from the game's clip positions (turretLockCentre). A quad's two triangles share
// that edge, so both move by the same vector and the quad's centre lands on the target. The vertex keeps the game's NDC depth (0.999 when it has no valid one), and the move is the
// displacement H's lighting line applies.
//
// THE SCORPION HOLOGRAM (signature H) also reads the draw's own vertex constants at GS slot 1 (the module binds the draw's VS slot 1 there and gives it back): the move's eye-space
// displacement d = Q - P is added to the interpolant __USER_VERTEX_M_LIGHTINGPOSITION (the camera-relative world position, which the pixel shader divides by its view depth) as
// right * d.x + up * d.y + forward * d.z, with the three unit vectors read from rows 277, 278 and 279 of that buffer.
//
// THE SHADERS, by signature (the first family's vertex shader names it):
//   A  vs 1989E6D3B405FDE0 + ps EAB8A1C95A13FFBE    hologram panels        TEXCOORD0 xyzw, TEXCOORD6 xyz, SV_POSITION
//   B  vs 2CECEC3065EF0D4A + ps A4D03619D631B186    hologram panels        TEXCOORD0 xyzw, TEXCOORD7 xy, SV_POSITION
//   C  vs DF3503CD07F9B10C + ps 8C08EB252B0F6095                           M_TEXCOORD xy, SV_POSITION
//   D  vs 44DDCDEBAEA137D8 + ps CB53257EFE8B1696                           RADAR_LOCALKEY_COLOUR xyzw, RADAR_LOCALKEY_POSITIONOFFSET xyzw, M_TEXCOORD x, SV_POSITION
//   E  vs 9B34C331902DC1ED + ps 3B3433E4FEBBC37B                           RADAR_LOCALKEY_COLOUR xyzw, RADAR_LOCALKEY_POSITIONOFFSET xyz, SV_POSITION
//   F  vs 9611A454527F7FEB + ps 1E1C49DC51C0E509                           RADAR_LOCALKEY_COLOUR xyz, RADAR_LOCALKEY_POSITIONOFFSET xy, SV_POSITION
//   G  vs B932058F26B76691 + ps 65861AC394D51526                           RADAR_LOCALKEY_COLOUR xyz, SV_POSITION
//   H  vs 5559BD94B6852E83 + ps EA02FAC2BD6C643C or E95634B0F61D218F        M_LIGHTINGPOSITION xyz, M_TEXCOORD xy, SV_POSITION (the Scorpion's hologram: the near quad of ps EA02FAC2, and the target hologram sphere)
//   I  vs 5DAD41AA66757CE3                                      the gunner camera's curved sheet, 2016 indices only   TEXCOORD0 xyzw, SV_POSITION
namespace edvr {

// The shader text: EDVR_SRV_TURRET_GS_HEAD (the constant buffer and the opening of the `Vertex` struct), the members, EDVR_SRV_TURRET_GS_TAIL (the struct's end and the pass-through).
// Adjacent string literals join, so each shader is one constant text; the macros are single-line concatenations because a raw string cannot span the lines of a directive.
#define EDVR_SRV_TURRET_GS_HEAD \
    "\n" \
    "cbuffer Turret : register(b0) {\n" \
    "    float4 mv;   // x: gap, metres nearer (negative: deeper); y: dMin and z: dMax, the window of distances from the anchor that move; w: the depth mode (0 game, 1 real, 2 front)\n" \
    "    float4 tn;   // the eye's tangents as the game projects: x left, y right, z down, w up\n" \
    "    float4 an;   // the anchor in the eye's space: X right, Y up, Z forward (= clip w)\n" \
    "    float4 tr;   // the translation (HUD forward) in the same axes; w: 1 / the content depth (the radar billboards' correction: the translation scaled by w / z_c), else 0\n" \
    "    float4 fr;   // the curved game panel: x gap2, metres nearer; y dMin2 and z dMax2, the window past 1 m. The near window (mv) wins when both match. w stays 0\n" \
    "    float4 s0;   // row 0 of seat-from-math-eye. Identity except on the gunner sheet, where it keeps the border test still as the head turns\n" \
    "    float4 s1;\n" \
    "    float4 s2;\n" \
    "    float4 s3;   // fourth row of the sheet's mesh-space map. Zero: the sheet is not cropped\n" \
    "};\n" \
    "float4 turretMove(float4 c, out float3 dq) {\n" \
    "    dq = float3(0.0, 0.0, 0.0);\n" \
    "    const float w = c.w;\n" \
    "    if (abs(w) < 1.0e-6) return c;\n" \
    "    const float3 p = float3(c.x * (tn.y - tn.x) * 0.5 + w * (tn.y + tn.x) * 0.5, c.y * (tn.w - tn.z) * 0.5 + w * (tn.w + tn.z) * 0.5, w);\n" \
    "    const float3 tv = tr.w > 0.0 ? tr.xyz * (w * tr.w) : tr.xyz;\n" \
    "    const float3 p1 = p + tv;\n" \
    "    const float3 v = p1 - an.xyz;\n" \
    "    const float d = length(v);\n" \
    "    const bool win = d > mv.y && d < mv.z;\n" \
    "    const bool far = !win && fr.x != 0.0 && d > fr.y && d < fr.z;\n" \
    "    const bool tra = any(tr.xyz != 0.0);\n" \
    "    if (!win && !far && !tra) return c;\n" \
    "    float3 q = p1;\n" \
    "    if (win) q = an.xyz + v * ((d - mv.x) / d);\n" \
    "    else if (far) q = an.xyz + v * ((d - fr.x) / d);\n" \
    "    dq = q - p;\n" \
    "    const float zn = c.z / w;\n" \
    "    float ndcZ = (w > 0.0 && zn >= 0.0 && zn <= 1.0) ? zn : 0.999;\n" \
    "    if (mv.w > 1.5) ndcZ = 0.999;\n" \
    "    else if (mv.w > 0.5) ndcZ = (abs(q.z) > 1.0e-6) ? clamp(c.z / q.z, 0.0, 0.999) : 0.999;\n" \
    "    return float4((2.0 * q.x - q.z * (tn.y + tn.x)) / (tn.y - tn.x), (2.0 * q.y - q.z * (tn.w + tn.z)) / (tn.w - tn.z), ndcZ * q.z, q.z);\n" \
    "}\n" \
    "float3 turretEye(float4 c) {\n" \
    "    return float3(c.x * (tn.y - tn.x) * 0.5 + c.w * (tn.y + tn.x) * 0.5, c.y * (tn.w - tn.z) * 0.5 + c.w * (tn.w + tn.z) * 0.5, c.w);\n" \
    "}\n" \
    "float3 turretLockCentre(float4 a, float4 b, float4 c) {\n" \
    "    const float3 p = turretEye(a), q = turretEye(b), r = turretEye(c);\n" \
    "    const float pq = dot(q - p, q - p), qr = dot(r - q, r - q), rp = dot(p - r, p - r);\n" \
    "    if (pq >= qr && pq >= rp) return (p + q) * 0.5;\n" \
    "    return qr >= rp ? (q + r) * 0.5 : (r + p) * 0.5;\n" \
    "}\n" \
    "float4 turretLock(float4 c, float3 k, out float3 dq) {\n" \
    "    dq = float3(0.0, 0.0, 0.0);\n" \
    "    if (abs(c.w) < 1.0e-6) return c;\n" \
    "    const float3 s = turretEye(c) + k;\n" \
    "    dq = k;\n" \
    "    const float g = c.z / c.w;\n" \
    "    const float gz = (c.w > 0.0 && g >= 0.0 && g <= 1.0) ? g : 0.999;\n" \
    "    return float4((2.0 * s.x - s.z * (tn.y + tn.x)) / (tn.y - tn.x), (2.0 * s.y - s.z * (tn.w + tn.z)) / (tn.w - tn.z), gz * s.z, s.z);\n" \
    "}\n" \
    "struct Vertex {\n"
// The tail in three pieces so signature H can put its constant buffer between the first two and its one line between the last two; every other signature's is the three together.
#define EDVR_SRV_TURRET_GS_TAIL_A \
    "\n" \
    "};\n" \
    "struct Moved {\n" \
    "    Vertex v;\n" \
    "    float4 pre : TURRETPRE;\n" \
    "};\n"
#define EDVR_SRV_TURRET_GS_TAIL_B \
    "[maxvertexcount(3)]\n" \
    "void main(triangle Vertex v[3], inout TriangleStream<Moved> stream) {\n" \
    "    const bool locked = an.w > 0.5;\n" \
    "    const float3 lk = locked ? tr.xyz - turretLockCentre(v[0].pos, v[1].pos, v[2].pos) : float3(0.0, 0.0, 0.0);\n" \
    "    [unroll] for (int i = 0; i < 3; ++i) {\n" \
    "        Moved o;\n" \
    "        o.v = v[i];\n" \
    "        o.pre = v[i].pos;\n" \
    "        float3 d;\n" \
    "        o.v.pos = turretMove(v[i].pos, d);\n" \
    "        if (locked) o.v.pos = turretLock(v[i].pos, lk, d);\n"
#define EDVR_SRV_TURRET_GS_TAIL_C \
    "        stream.Append(o);\n" \
    "    }\n" \
    "    stream.RestartStrip();\n" \
    "}\n"
#define EDVR_SRV_TURRET_GS_TAIL EDVR_SRV_TURRET_GS_TAIL_A EDVR_SRV_TURRET_GS_TAIL_B EDVR_SRV_TURRET_GS_TAIL_C

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

// H also moves the lighting position (the camera-relative world position the pixel shader divides by its view depth) by the world vector of the move's eye-space displacement, from the camera's
// right, up and forward vectors in rows 277 to 279 of the draw's vertex constants (GS slot 1: a 280-register view of the game's own VS slot 1).
constexpr char kSrvTurretHudGsH[] = EDVR_SRV_TURRET_GS_HEAD
    "    float3 l : __USER_VERTEX_M_LIGHTINGPOSITION;\n"
    "    float2 m : __USER_VERTEX_M_TEXCOORD;\n"
    "    float4 pos : SV_Position;"
    EDVR_SRV_TURRET_GS_TAIL_A
    "cbuffer GameVS : register(b1) {\n"
    "    float4 g[280];   // the game's vertex constants of this draw: [277] the camera's right, [278] up, [279] forward, unit vectors in world axes\n"
    "};\n"
    EDVR_SRV_TURRET_GS_TAIL_B
    "        o.v.l += g[277].xyz * d.x + g[278].xyz * d.y + g[279].xyz * d.z;\n"
    EDVR_SRV_TURRET_GS_TAIL_C;

// The gunner camera's curved sheet (vs 5DAD41AA66757CE3, 2016 indices). One mesh holds the faint frame and the
// bright card brackets; nothing in the vertex marks them apart (dump 113734647001: packed colour is constant, the
// UV follows the position). The split is the mesh's own local metres. The game's vertex shader writes
// clip = (dot(cb0[4], w), dot(cb0[5], w), dot(cb0[6], w), dot(cb0[7], w)) with w = local - cb1[275] (identity
// instance, scale 1). Those two buffers are bound here as SheetClip and SheetCam. Inverting them puts the vertex
// back in local metres, which does not move when the headset does. The choice is those metres, never the colour, so a
// HUD recolour keeps the same triangles. Dumps 113734647001 and 122624067001: the bright rows are y -0.478, -0.402,
// -0.325 and -0.246; the faint row under the cards is y -0.553 and the faint row above them is y -0.166. The bright
// top is the quad between y -0.246 and y -0.166, and the bottom hook is the quad between y -0.478 and y -0.553.
// The text, icons and the card body are the same mesh as the border. Dump 131322664001: 28 of the 672 triangles
// sit inside the two fills (y -0.478..-0.325, x about -0.75..-0.50 and 0.50..0.75). Those are drawn. A triangle
// that reaches the border rows (y -0.246 and above, or y -0.553) is sent off screen. No view bound: the sheet is
// drawn whole, so a missed constant buffer does not blank the cards.
// Two bright pieces of the bracket survive that test, because the cells that hold them also hold card text: the
// vertical bar and top hook left of the target card, and the dash under the systems card's lower-left corner. A
// clip plane cuts them, not a cell. The vertices carry SV_ClipDistance0, (x, y) distances to two planes in mesh
// metres, chosen by the triangle's centroid; the pixel shader does not declare it. Dump 131322664001: the dark
// fills (vs C4B4B334B26E81A9) span x -0.7708..-0.4867 on the target card and from 0.4934 on the systems card, y
// -0.481..-0.311 on both; the bar sits just left of -0.7708 and the hook above -0.311; the dash sits in the row
// y -0.553..-0.478 left of x 0.51, and the GUNNER CAMERA label in that row starts near 0.6. A view that could not
// be read leaves both distances at 1, so nothing is clipped.
constexpr char kSrvTurretHudGsI[] = EDVR_SRV_TURRET_GS_HEAD
    "    float4 t0 : TEXCOORD0;\n"
    "    float4 pos : SV_Position;\n"
    "};\n"
    "cbuffer SheetClip : register(b1) { float4 clipRows[8]; };\n"
    "cbuffer SheetCam : register(b2) { float4 camBlock[276]; };\n"
    "struct Moved { Vertex v; float4 pre : TURRETPRE; float2 clip : SV_ClipDistance0; };\n"
    "// Dump 131322664001, mesh metres. The target card's fill starts at x -0.7708 and its top edge is y -0.311; the plane sits 2 mm inside the left edge and 2 mm above the top.\n"
    "static const float kTargetCardLeftX = -0.769;\n"
    "static const float kTargetCardTopY = -0.309;\n"
    "// The systems card's fill ends its row at y -0.481. Below that, on the right, the label row: its dash lies left of x 0.51 and the label starts near 0.6.\n"
    "static const float kCardRowBottomY = -0.481;\n"
    "static const float kLabelRowLeftX = 0.53;\n"
    "float4x4 sheetInv(float4x4 m, out float det) {\n"
    "    const float a00 = m[0][0], a01 = m[0][1], a02 = m[0][2], a03 = m[0][3];\n"
    "    const float a10 = m[1][0], a11 = m[1][1], a12 = m[1][2], a13 = m[1][3];\n"
    "    const float a20 = m[2][0], a21 = m[2][1], a22 = m[2][2], a23 = m[2][3];\n"
    "    const float a30 = m[3][0], a31 = m[3][1], a32 = m[3][2], a33 = m[3][3];\n"
    "    const float b00 = a00 * a11 - a01 * a10;\n"
    "    const float b01 = a00 * a12 - a02 * a10;\n"
    "    const float b02 = a00 * a13 - a03 * a10;\n"
    "    const float b03 = a01 * a12 - a02 * a11;\n"
    "    const float b04 = a01 * a13 - a03 * a11;\n"
    "    const float b05 = a02 * a13 - a03 * a12;\n"
    "    const float b06 = a20 * a31 - a21 * a30;\n"
    "    const float b07 = a20 * a32 - a22 * a30;\n"
    "    const float b08 = a20 * a33 - a23 * a30;\n"
    "    const float b09 = a21 * a32 - a22 * a31;\n"
    "    const float b10 = a21 * a33 - a23 * a31;\n"
    "    const float b11 = a22 * a33 - a23 * a32;\n"
    "    det = b00 * b11 - b01 * b10 + b02 * b09 + b03 * b08 - b04 * b07 + b05 * b06;\n"
    "    const float invDet = 1.0 / det;\n"
    "    float4x4 r;\n"
    "    r[0] = float4( a11 * b11 - a12 * b10 + a13 * b09, -a01 * b11 + a02 * b10 - a03 * b09,  a31 * b05 - a32 * b04 + a33 * b03, -a21 * b05 + a22 * b04 - a23 * b03) * invDet;\n"
    "    r[1] = float4(-a10 * b11 + a12 * b08 - a13 * b07,  a00 * b11 - a02 * b08 + a03 * b07, -a30 * b05 + a32 * b02 - a33 * b01,  a20 * b05 - a22 * b02 + a23 * b01) * invDet;\n"
    "    r[2] = float4( a10 * b10 - a11 * b08 + a13 * b06, -a00 * b10 + a01 * b08 - a03 * b06,  a30 * b04 - a31 * b02 + a33 * b00, -a20 * b04 + a21 * b02 - a23 * b00) * invDet;\n"
    "    r[3] = float4(-a10 * b09 + a11 * b07 - a12 * b06,  a00 * b09 - a01 * b07 + a02 * b06, -a30 * b03 + a31 * b01 - a32 * b00,  a20 * b03 - a21 * b01 + a22 * b00) * invDet;\n"
    "    return r;\n"
    "}\n"
    "bool inFill(float3 p) {\n"
    "    const bool left = p.x > -0.822 && p.x < -0.47 && p.y > -0.51 && p.y < -0.245;\n"
    "    const bool right = p.x > 0.50 && p.x < 0.78 && p.y > -0.51 && p.y < -0.245;\n"
    "    const bool label = p.x > 0.12 && p.x < 0.78 && p.y > -0.58 && p.y < -0.50;\n"
    "    return left || right || label;\n"
    "}\n"
    "bool liveLocal(float4 c, out float3 local) {\n"
    "    float det;\n"
    "    const float4x4 G = sheetInv(float4x4(clipRows[4], clipRows[5], clipRows[6], clipRows[7]), det);\n"
    "    const float4 h = mul(G, c);\n"
    "    if (abs(det) < 1e-8 || abs(h.w) < 1e-6) { local = 0; return false; }\n"
    "    local = h.xyz / h.w + camBlock[275].xyz;\n"
    "    return true;\n"
    "}\n"
    "float2 sheetClip(float3 p, float3 cen) {\n"
    "    if (cen.x < 0.0) return float2(p.x - kTargetCardLeftX, kTargetCardTopY - p.y);\n"
    "    if (cen.x > 0.0 && cen.y < kCardRowBottomY) return float2(p.x - kLabelRowLeftX, 1.0);\n"
    "    return float2(1.0, 1.0);\n"
    "}\n"
    "[maxvertexcount(3)]\n"
    "void main(triangle Vertex v[3], inout TriangleStream<Moved> stream) {\n"
    "    float4 c0 = v[0].pos, c1 = v[1].pos, c2 = v[2].pos;\n"
    "    bool draw = true;\n"
    "    bool live = false;\n"
    "    float3 p0 = float3(0.0, 0.0, 0.0), p1 = float3(0.0, 0.0, 0.0), p2 = float3(0.0, 0.0, 0.0);\n"
    "    if (dot(clipRows[4], clipRows[4]) + dot(clipRows[7], clipRows[7]) > 1e-8) {\n"
    "        if (liveLocal(c0, p0) && liveLocal(c1, p1) && liveLocal(c2, p2)) {\n"
    "            live = true;\n"
    "            draw = inFill(p0) && inFill(p1) && inFill(p2);\n"
    "        }\n"
    "    }\n"
    "    const float3 cen = (p0 + p1 + p2) / 3.0;\n"
    "    const bool locked = an.w > 0.5;\n"
    "    const float3 lk = locked ? tr.xyz - turretLockCentre(c0, c1, c2) : float3(0.0, 0.0, 0.0);\n"
    "    [unroll] for (int i = 0; i < 3; ++i) {\n"
    "        const float4 corner = i == 0 ? c0 : (i == 1 ? c1 : c2);\n"
    "        Moved o;\n"
    "        o.v = v[i];\n"
    "        o.pre = v[i].pos;\n"
    "        o.clip = live ? sheetClip(i == 0 ? p0 : (i == 1 ? p1 : p2), cen) : float2(1.0, 1.0);\n"
    "        float3 d;\n"
    "        const float4 moved = turretMove(corner, d);\n"
    "        o.v.pos = draw ? (locked ? turretLock(corner, lk, d) : moved) : float4(2.0, 2.0, 0.0, 1.0);\n"
    "        stream.Append(o);\n"
    "    }\n"
    "    stream.RestartStrip();\n"
    "}\n";


#undef EDVR_SRV_TURRET_GS_HEAD
#undef EDVR_SRV_TURRET_GS_TAIL
#undef EDVR_SRV_TURRET_GS_TAIL_A
#undef EDVR_SRV_TURRET_GS_TAIL_B
#undef EDVR_SRV_TURRET_GS_TAIL_C
}  // namespace edvr
