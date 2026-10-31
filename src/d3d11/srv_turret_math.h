// The SRV turret view (issue 140): the pure parts, shared by the module (srv_turret.cpp), the draw path and tools/srv_turret_hud_test, which runs these very
// functions beside the production geometry shader on WARP.
//
// THE FAULT. In the Scorpion SRV's turret (gunner) view in VR, Elite places the turret HUD badly:
//   1. The camera sits too far forward. The near HUD layer (the two weapon arc brackets, SECONDARY A / PRIMARY A, the ammo counts) is about 0.11 m BEHIND the
//      default eye point, invisible unless the player leans back.
//   2. The target information panel and the SYSTEMS panel sit about a metre deeper than that layer (measured from an eye dump: arc 0.158 m, info panel 1.176 m, in the
//      flight of 2026-10-30 the near layer 0.41 to 0.47 m and the panels 1.29 and 1.41 m, after the camera move).
// WHICH DRAWS. Two rounds of evidence. (1) An offline emulation of the dump (2026-10-30): the arc, weapon-name, ammo and INFO-strip layer and the upper message panels are drawn
// by two instanced hologram families from records 0.41 to 0.47 m out and about 1.39 m out: vs 1989E6D3B405FDE0 (ps EAB8A1C95A13FFBE) and vs 2CECEC3065EF0D4A (ps A4D03619D631B186).
// The targeting sprite (vs E508648660A352B2, ps 63ABD86359B57D01) is NOT a panel: its three draws are the reticle's three layers at clip w of about 500 m. (2) The flight of
// 2026-10-30 16:13 (Steam, dc842603): the upper panels moved with the gap key, the target information panel and the SYSTEMS panel below the centre (w 1.3 to 1.4 m, ndc y about
// -0.25) did not, because none of the eight sampled draws of those two families covers them. They are drawn by other families of the same HDR-target run, between the 2CECEC30
// draws and the INFO strip. Their semantics read `__USER_RADAR_LOCALKEY_*`, so an earlier note filed them as the radar; the names are the game's own and say nothing about what
// the families draw. The list below is every family of that run whose clip w is the view depth in metres (the vertex shader builds its position from the eye's projection
// constants) and that a pass-through geometry shader can carry. Scorpion only: see Part 1.
//
// EXCLUDED, with the reason (from the fxc disassembly and the eye dump 145941154001):
//   vs B12F7A618E1BDE98 (ps 42AC0CACC9CDF72B) and vs 203DF51758AADC4D (ps EEAAC839A9F09448), the 5334-index shells: dp4 against cb0[4..7] of the mesh position, and cb0[7] in
//     both Scorpion dumps (145941154001, 142608727001) read {unit xyz, +2.99e9 / +3.70e8 / +3.1e9}: they are bodies in the sky at gigametres. Their w is a view depth in metres, 3e8 to 3e9 of them,
//     and no vertex of a shell can lie in a 1 to 4 m window, so a pass would only cost twelve 5334-index draws an eye.
//   vs 94D5C556DFD6D705 (ps 912477AEF6958379), the instanced glare sprite: its position is built in NDC (`mov r4.zw, l(0,0,0,1.0)`: z 0 and w a constant 1.0) and masked to
//     all zeros by an occlusion test of its own. Its w is not a depth.
//   vs B7790CBFC6554097 (ps 8DEF46452FA459F5), the flight HUD's vectors: not a turret panel at all. The GPU instrument of the flight of 9fdad3a1 captured its clip w over
//     -658 to +3029 m, so no window of this pass means anything for it. It was family 9 of the first build and is out of the pass (its signature and geometry shader with it).
//
// WHY THE PULL WAS REPLACED BY A PUSH. The first build brought the deep panels forward to the near layer (a window above the gap). The flight of 9fdad3a1 showed no gap makes the
// layers meet: the weapon panels (the arcs, the weapon names, the ammo counts, the INFO strip: families 1989E6D3 and 2CECEC30) lie at clip w 0.27 to 0.70 m, the radar families
// (DF3503CD, 44DDCDEB, 9B34C331, 9611A454, B932058F and the two 5559BD94 sphere pairs) at 0.11 to 0.44 m, and the larger panels (SYSTEMS, the location panel, the upper messages) at
// 1.3 to 2.8 m. So the near layer is pushed BACK to the larger panels' depth instead, with the same geometry shader and a NEGATIVE gap.
// WHY THE MOVE IS IN 3D. The first push changed only the clip x of a vertex, which is exactly right for a head at one place and keeps the panel's motion parallax and looming of the
// OLD depth: the flight of e45c68a9 (Sean: the radar and the panels "move with my head", a panel "got larger" when he moved toward it). The shader now moves each vertex for real
// about a fixed anchor (see THE MOVE below). And at the game's own camera the whole turret HUD lies about 0.7 m too far back for the head (the radar behind it), so the whole HUD is
// first translated forward (fix.srv_turret_hud_forward), which is the old camera move applied to the HUD only: the world keeps the game's camera.
//
// THE SEVEN TEMPORARY KEYS (all live, all VR only, 0 = the game's own; the last is a text switch):
//   fix.srv_turret_camera_back  centimetres, 0..150, default 0: the camera moves back in the turret view, over the turret's axis (the stock camera is the plan; the HUD-forward key
//                               replaces this). Part 1: published as the seated z of the eye alignment (alignTick), the single publisher of that alignment, so its serials stay coherent.
//   fix.srv_turret_hud_forward  centimetres, 0..150, default 70: every vertex of the panels group (Group::kPanels: the two hologram families) moves forward along the turret by this much
//                               (seated -z), so the panels sit in front of the head as they did with the old camera move while the game's own camera stays.
//   fix.srv_turret_radar_forward centimetres, 0..150, default 70: the same for the radar group (Group::kRadar), instead of the HUD-forward value, so it can stay near field and be seen
//                               from above (carried level with the head the disc is seen edge-on).
//   fix.srv_turret_panels_back  centimetres, 0..200, default 120: the weapon panels (Group::kPanels: the two hologram families) go this much deeper where they lie within kPushSplitM of
//                               the anchor; the larger panels of the same families, at 1.3 m and more, stay. Part 2: a private pass-through geometry shader per output signature
//                               (srv_turret_hud_shader.h) around every issue of the families' draws; the rule is per vertex.
//   fix.srv_turret_radar_back   centimetres, 0..200, default 0: the same for the radar (Group::kRadar), which the game lays on the turret deck right under the player.
//   fix.srv_turret_hud_gap      centimetres, 0..200, default 0: the first build's forward pull, kept for comparison. For a family whose group has a push above 0 the push is
//                               used and this is not; for a group with no push this brings the vertices deeper than gap + kWindowNearM (up to kWindowFarM) forward by it.
//   fix.srv_turret_hud_depth    game | real | front, default game: how the depth of a MOVED vertex is written (DepthMode below). A test switch for the radar disc (DF3503CD), which the
//                               moved HUD's depth may hide.
//
// THE TARGET HOLOGRAM (vs 5559BD94, the Scorpion's own hologram, signature H): its vertex shader writes __USER_VERTEX_M_LIGHTINGPOSITION, the vertex's camera-relative WORLD position
// (cb0[9..11] times the position), and its pixel shader uses it twice (a depth test against the scene and a view ray divided by the fragment's view depth). The geometry shader moves
// SV_Position but not that interpolant, so a hologram the game put beside or behind the eye had a view depth near 0 or negative, the ray blew up and the hologram vanished. cb1[277..279] of
// the same draw's vertex constants are the camera's RIGHT, UP and FORWARD unit vectors in world axes (they match the projection rows), so the shader adds the eye-space displacement of the
// move (right * dX + up * dY + forward * dZ) to the interpolant. The module binds the draw's own VS constant buffer at slot 1 to GS slot 1 for that signature (srv_turret_hud_binding.h).
//
// KNOWN GAP. With Elite's "Disable GUI effects" OFF the holo panel family is vs 81216C77F90DEDD6 (ps A2965EC2931A39C8), a different output signature (SV_Position at o5).
// There is no bytecode for it in the dumps, so it is not covered yet.
#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>

namespace edvr {
namespace srv_turret {

// ---- the keys -------------------------------------------------------------------------------------------------------------------------------------------------
inline constexpr int kBackDefaultCm = 0, kBackMaxCm = 150;
inline constexpr int kForwardDefaultCm = 70, kForwardMaxCm = 150;   // fix.srv_turret_hud_forward
inline constexpr int kRadarForwardDefaultCm = 70, kRadarForwardMaxCm = 150;   // fix.srv_turret_radar_forward
inline constexpr int kGapDefaultCm = 0, kGapMaxCm = 200;
inline constexpr int kPanelsDefaultCm = 120, kRadarDefaultCm = 0, kPushMaxCm = 200;   // fix.srv_turret_panels_back and fix.srv_turret_radar_back

// How the depth of a moved vertex is written (fix.srv_turret_hud_depth). game: the vertex's own NDC z where it is a valid reversed-Z depth, else kFallbackNdcZ (the first builds' rule).
// real: the depth the moved position itself has, ndcZ = clamp(c.z / q.z, 0, kFallbackNdcZ) with the vertex's own clip z (Elite writes the constant 0.025, so this is 0.025 / depth).
// front: kFallbackNdcZ for every moved vertex.
enum class DepthMode : uint8_t { kGame = 0, kReal = 1, kFront = 2 };
inline const char* depthModeName(DepthMode m) { return m == DepthMode::kReal ? "real" : m == DepthMode::kFront ? "front" : "game"; }
// The key's text; anything else is the default (the settings line says which it took).
inline DepthMode depthModeOf(const char* text) {
    if (text && !std::strcmp(text, "real")) return DepthMode::kReal;
    if (text && !std::strcmp(text, "front")) return DepthMode::kFront;
    return DepthMode::kGame;
}
// Signature H's geometry shader reads the draw's vertex constants at GS slot 1: the draw's own VS slot 1, rows 277 to 279 the camera's right, up and forward in world axes.
inline constexpr uint32_t kGameCbSlot = 1;
inline constexpr uint32_t kSceneRightRow = 277, kSceneUpRow = 278, kSceneForwardRow = 279, kSceneRows = 280;
inline constexpr uint32_t kSceneSignature = 7;   // kSignatures[7] = 5559BD94, the Scorpion's hologram

// ---- Part 1: when, and which way ------------------------------------------------------------------------------------------------------------------------------
// THE SCORPION ONLY. Status.json reports the turret view bit in the Scarab (SRV type testbuggy) as well, and the Scarab's turret view was fine before this change, so both parts
// act only while the journal says the vehicle is the Scorpion (journal_vehicle_latch.h: LoadGame's "Ship" or LaunchSRV's "SRVType" is combat_multicrew_srv_01, case-insensitively).
// Unknown is "not the Scorpion": a training scenario writes no journal, and the latch starts false and is cleared by the main menu's music, a shutdown, DockSRV, SRVDestroyed and a
// load or launch of anything else.
// Status.json Flags bit 13 (0x2000): "SRV using turret view" (Frontier's documented bit); read only inside an SRV, Flags bit 26 (the same word's seat bits,
// journal_watch.h journalSeatKind bit 0x4), because a stale bit outside one is no turret.
inline constexpr uint32_t kFlagsTurretView = 1u << 13;
inline constexpr uint32_t kFlagsInSrv = 1u << 26;
inline constexpr unsigned kSeatKindSrv = 0x4u;
constexpr bool turretViewFromFlags(uint32_t flags) { return (flags & kFlagsTurretView) != 0 && (flags & kFlagsInSrv) != 0; }

// The seated z the eye alignment carries. The view moves by MINUS the published alignment (native_on_foot_f87.h: "its +y lowers the view"), and seated +z is
// backward (OpenVR), so moving the camera back by D metres publishes z = -D. Exactly 0.0 (not -0.0) whenever the camera is the game's own: off the turret view, key at 0.
inline double backSeatedZ(bool turretOn, int backCm) {
    if (!turretOn || backCm <= 0) return 0.0;
    const int cm = backCm > kBackMaxCm ? kBackMaxCm : backCm;
    return -static_cast<double>(cm) / 100.0;
}

// ---- Part 2: the families, their output signatures and the shift ------------------------------------------------------------------------------------------------------
// A family is a vertex shader and the pixel shader it is drawn with; several families share an output signature (the two pixel shaders of the target hologram sphere), and the
// pass has one geometry shader per SIGNATURE. Each signature below is the vertex shader's output element for element and in register order, read from the fxc disassembly of the
// dumps' bytecode (the pixel shaders' inputs are a subset of it in every case; psReadsPosition marks the ones that also read SV_POSITION.xy, the rasterizer's pixel position).
// Clip w is the view depth along the eye's forward axis, in metres, for every family listed: the hologram panels dp4 their position against cb0[4..7], and the radar-named
// billboards and the sphere add their local offsets, scaled, to a translation column of the same eye's projection constants (b2 or b1 columns, no divide). The capture lines of the
// GPU instrument print the w each family really had.
struct SigElement {
    const char* name;   // the semantic name as the game's shaders spell it (`__USER_...` for the radar and hologram families)
    uint32_t index;
    uint32_t comps;     // the components written (the mask's width)
};
inline constexpr uint32_t kMaxSigElements = 12;
struct Signature {
    const char* name;            // the first family's vertex shader's eight hex digits: how the log names the geometry shader and its twin
    uint32_t count;              // the elements, SV_Position the last
    SigElement el[kMaxSigElements];
    bool psReadsPosition;        // the family's pixel shader also reads SV_POSITION.xy
};
inline constexpr uint32_t kSignatureCount = 8;
inline constexpr Signature kSignatures[kSignatureCount] = {
    {"1989E6D3", 3, {{"TEXCOORD", 0, 4}, {"TEXCOORD", 6, 3}, {"SV_Position", 0, 4}}, false},
    {"2CECEC30", 3, {{"TEXCOORD", 0, 4}, {"TEXCOORD", 7, 2}, {"SV_Position", 0, 4}}, true},
    {"DF3503CD", 2, {{"__USER_VERTEX_M_TEXCOORD", 0, 2}, {"SV_Position", 0, 4}}, false},
    {"44DDCDEB", 4, {{"__USER_RADAR_LOCALKEY_COLOUR", 0, 4}, {"__USER_RADAR_LOCALKEY_POSITIONOFFSET", 0, 4}, {"__USER_VERTEX_M_TEXCOORD", 0, 1}, {"SV_Position", 0, 4}}, true},
    {"9B34C331", 3, {{"__USER_RADAR_LOCALKEY_COLOUR", 0, 4}, {"__USER_RADAR_LOCALKEY_POSITIONOFFSET", 0, 3}, {"SV_Position", 0, 4}}, false},
    {"9611A454", 3, {{"__USER_RADAR_LOCALKEY_COLOUR", 0, 3}, {"__USER_RADAR_LOCALKEY_POSITIONOFFSET", 0, 2}, {"SV_Position", 0, 4}}, false},
    {"B932058F", 2, {{"__USER_RADAR_LOCALKEY_COLOUR", 0, 3}, {"SV_Position", 0, 4}}, false},
    {"5559BD94", 3, {{"__USER_VERTEX_M_LIGHTINGPOSITION", 0, 3}, {"__USER_VERTEX_M_TEXCOORD", 0, 2}, {"SV_Position", 0, 4}}, true},
};
// The floats of one vertex as the vertex shader writes it (the signature), and where the clip position, the last element, starts in it. The geometry shader writes the signature and then one
// more element, `pre` (kPreSemantic, 4 floats): the clip position as the game made it, before the move, so the instrument can tell the vertices the shader moved.
inline constexpr uint32_t kPreFloats = 4;
inline constexpr const char* kPreSemantic = "TURRETPRE";
constexpr uint32_t sigFloats(uint32_t sig) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < kSignatures[sig].count; ++i) n += kSignatures[sig].el[i].comps;
    return n;
}
constexpr uint32_t sigPosFloat(uint32_t sig) { return sigFloats(sig) - kSignatures[sig].el[kSignatures[sig].count - 1].comps; }

// Which key pushes a family back. The weapon panels are what the two hologram families draw at clip w 0.27 to 0.70 m (the arcs, the weapon names, the ammo counts, the INFO strip;
// the same families also draw the larger panels at 1.3 m and more, which a push leaves alone); the radar is what the seven radar-named and sphere families draw, at 0.11 to 0.44 m.
enum class Group : uint8_t { kPanels = 0, kRadar = 1 };
inline constexpr uint32_t kGroupCount = 2;
inline const char* groupName(Group g) { return g == Group::kPanels ? "panels" : "radar"; }

struct Family {
    uint64_t vs, ps;
    const char* name;      // how the log names the family: the vertex shader's first eight hex digits, and for a second pixel shader of the same vertex shader the pixel shader's too
    uint32_t sig;          // the index into kSignatures of the family's vertex shader's output
    uint32_t strideBytes;  // the stream-output record ((sigFloats + kPreFloats) * 4)
    uint32_t posFloat;     // where SV_POSITION starts in it, in floats
    uint32_t preFloat;     // where `pre` (the clip position before the move) starts in it
    Group group;           // the push-back key that governs the family
};
constexpr Family makeFamily(uint64_t vs, uint64_t ps, const char* name, uint32_t sig, Group group) {
    return Family{vs, ps, name, sig, (sigFloats(sig) + kPreFloats) * 4, sigPosFloat(sig), sigFloats(sig), group};
}
inline constexpr uint32_t kFamilyCount = 9;
inline constexpr Family kFamilies[kFamilyCount] = {
    makeFamily(0x1989E6D3B405FDE0ull, 0xEAB8A1C95A13FFBEull, "1989E6D3", 0, Group::kPanels),
    makeFamily(0x2CECEC3065EF0D4Aull, 0xA4D03619D631B186ull, "2CECEC30", 1, Group::kPanels),
    makeFamily(0xDF3503CD07F9B10Cull, 0x8C08EB252B0F6095ull, "DF3503CD", 2, Group::kRadar),
    makeFamily(0x44DDCDEBAEA137D8ull, 0xCB53257EFE8B1696ull, "44DDCDEB", 3, Group::kRadar),
    makeFamily(0x9B34C331902DC1EDull, 0x3B3433E4FEBBC37Bull, "9B34C331", 4, Group::kRadar),
    makeFamily(0x9611A454527F7FEBull, 0x1E1C49DC51C0E509ull, "9611A454", 5, Group::kRadar),
    makeFamily(0xB932058F26B76691ull, 0x65861AC394D51526ull, "B932058F", 6, Group::kRadar),
    makeFamily(0x5559BD94B6852E83ull, 0xEA02FAC2BD6C643Cull, "5559BD94.EA02FAC2", 7, Group::kRadar),
    makeFamily(0x5559BD94B6852E83ull, 0xE95634B0F61D218Full, "5559BD94.E95634B0", 7, Group::kRadar),
};
// The geometry stage's limits the signatures must stay inside: 32 input registers, and 1024 scalars of output a draw (maxvertexcount 3 times a vertex's floats).
constexpr bool signaturesFit() {
    for (uint32_t s = 0; s < kSignatureCount; ++s) {
        if (kSignatures[s].count < 2 || kSignatures[s].count + 1 > kMaxSigElements || kSignatures[s].count + 1 > 32) return false;
        const SigElement& last = kSignatures[s].el[kSignatures[s].count - 1];
        if (last.comps != 4 || last.name[0] != 'S' || last.name[1] != 'V' || 3 * (sigFloats(s) + kPreFloats) > 1024) return false;
    }
    for (uint32_t f = 0; f < kFamilyCount; ++f)
        if (kFamilies[f].sig >= kSignatureCount) return false;
    return true;
}
static_assert(signaturesFit(), "every signature ends in a 4-component SV_Position and fits the geometry stage's limits; every family names a signature");
static_assert(kFamilies[0].strideBytes == 60 && kFamilies[0].posFloat == 7 && kFamilies[0].preFloat == 11 && kFamilies[1].strideBytes == 56 && kFamilies[1].posFloat == 6 &&
                  kFamilies[1].preFloat == 10 && kFamilies[8].strideBytes == 52 && kFamilies[8].posFloat == 5 && kFamilies[8].preFloat == 9,
              "the stream-output records: (4 + 3 + 4 + 4), (4 + 2 + 4 + 4) and the target hologram sphere's (3 + 2 + 4 + 4) floats");
static_assert(kFamilies[0].group == Group::kPanels && kFamilies[1].group == Group::kPanels && kFamilies[2].group == Group::kRadar && kFamilies[8].group == Group::kRadar,
              "the two hologram families are the panels, every other family is the radar");
// The family of a draw's shader pair, or -1: both halves of the pair must match.
inline int familyOf(uint64_t vs, uint64_t ps) {
    for (uint32_t i = 0; i < kFamilyCount; ++i)
        if (kFamilies[i].vs == vs && kFamilies[i].ps == ps) return static_cast<int>(i);
    return -1;
}
// Whether a vertex shader is any family's (the draw path's cheap first test: the pixel shader's hash is read only when this holds).
inline bool isFamilyVs(uint64_t vs) {
    for (uint32_t i = 0; i < kFamilyCount; ++i)
        if (kFamilies[i].vs == vs) return true;
    return false;
}
// The primitive topologies the shaders carry: triangle lists and triangle strips (the input assembler hands the `triangle` input of a geometry shader each strip triangle in the
// winding the rasterizer would have used). D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST is 4 and TRIANGLESTRIP 5; everything else is refused, counted (Refuse::kTopology).
constexpr bool topologyCarried(int d3d11Topology) { return d3d11Topology == 4 || d3d11Topology == 5; }

// THE MOVE. A vertex is moved in 3D, not only in clip x. The geometry shader reconstructs the vertex's eye-space position P from its clip position and the eye's projection tangents (the
// ones the game projects with, the eye jitter included), translates it forward by T (fix.srv_turret_hud_forward, for EVERY vertex of every family), and then moves it along its ray from
// a FIXED ANCHOR A (the head's seated position when the turret view came on, taken again on a recentre, when a camera key changes and when the head stays far from it; the eye's view of it is A_eye), judged on the
// distance of the translated vertex from A:
//     P1 = P + T;   V = P1 - A;   d = |V|;   if (dMin < d < dMax)   Q = A + V (d - gap) / d   else   Q = P1;   reproject Q, for any Q.z (the reprojection is linear in Q)
// gap < 0 is the push-back (deeper), gap > 0 the pull (nearer). The result keeps the NDC z of the vertex (z' = ndcZ w') when that is a valid depth, so the game's own depth values stand;
// otherwise ndcZ = kFallbackNdcZ. Elite is REVERSED Z with an infinite far plane (cb0[6] = (0, 0, 0, 0.025): clip z is the constant 0.025, NDC z = 0.025 / w): a vertex the game had
// behind the eye (w < 0) or inside its 2.5 cm near plane (0 < w < 0.025, z / w above 1) has no valid depth, and the moved vertex would fail the rasterizer's depth clip (0 <= z <= w) and
// be cut; 0.999 is the NEAREST valid reversed-Z depth (1 is near), so such a vertex draws in front, as the HUD does. From the anchor the
// moved vertex is seen in the direction it was (a push keeps the panel's size on screen) and from anywhere else it is a real panel at the new distance: leaning toward it makes it
// larger and moves it by the parallax of its new depth, not the old one's.
// THE PULL (fix.srv_turret_hud_gap): vertices whose distance from the anchor lies between dMin = gap + kWindowNearM and dMax = kWindowFarM move `gap` metres nearer.
inline constexpr float kWindowNearM = 0.10f;
inline constexpr float kWindowFarM = 4.0f;
// THE PUSH (fix.srv_turret_panels_back, fix.srv_turret_radar_back): vertices whose distance from the anchor lies between kPushNearM and kPushSplitM move `back` metres DEEPER (a negative
// gap). 2 cm keeps a vertex on the anchor itself (no ray) out; 1 m splits the weapon panels and the radar (0.1 to 0.7 m from the head) from the larger panels (1.3 m and deeper).
inline constexpr float kPushNearM = 0.02f;
inline constexpr float kPushSplitM = 1.0f;
inline constexpr float kFallbackNdcZ = 0.999f; // the depth of a moved vertex whose own NDC z is not valid (behind the eye, or inside the near plane): the nearest valid reversed-Z depth
inline constexpr float kPlaneEpsM = 1.0e-6f;  // a vertex on the eye plane (clip w about 0) has no ray to follow: left alone
inline constexpr float kAnchorLimitM = 2.0f;  // the constants accept an anchor, a translation and a gap up to this far (the keys' 200 cm, 150 cm)

// The geometry shader's constant buffer (GS slot 0, which the game never binds): four registers.
//   [0] gap, dMin, dMax, depth       the move: metres nearer (negative: deeper), and the window of anchor distances it applies to; the DepthMode (0 game, 1 real, 2 front)
//   [1] l, r, dn, up                 the eye's projection tangents as the game projects (left and down negative; the jitter shift is added)
//   [2] ax, ay, az, (pad)            the anchor in the eye's space: X right, Y up, Z forward (= clip w)
//   [3] tx, ty, tz, (pad)            the translation (HUD forward) in the same axes
struct TurretParams {
    float gap = 0.0f, dMin = 0.0f, dMax = 0.0f, depth = 0.0f;   // depth: the DepthMode as 0, 1 or 2
    float l = 0.0f, r = 0.0f, dn = 0.0f, up = 0.0f;
    float ax = 0.0f, ay = 0.0f, az = 0.0f, pad1 = 0.0f;
    float tx = 0.0f, ty = 0.0f, tz = 0.0f, pad2 = 0.0f;
    bool operator==(const TurretParams& o) const { return std::memcmp(this, &o, sizeof(TurretParams)) == 0; }
};
static_assert(sizeof(TurretParams) == 64, "the turret shader's cbuffer is four 16-byte registers");

// One eye's side of the constants for a frame: its tangents (jitter included), the anchor and the HUD-forward translation, both in the eye's space (X right, Y up, Z forward).
struct EyeFrame {
    float l = 0.0f, r = 0.0f, dn = 0.0f, up = 0.0f;
    float a[3] = {0.0f, 0.0f, 0.0f};
    float t[3] = {0.0f, 0.0f, 0.0f};        // the panels group's translation (fix.srv_turret_hud_forward)
    float tRadar[3] = {0.0f, 0.0f, 0.0f};   // the radar group's (fix.srv_turret_radar_forward)
};

// The constants with the depth mode written (the module applies the key to whatever paramsForGroup made).
inline TurretParams withDepth(TurretParams p, DepthMode m) {
    p.depth = static_cast<float>(static_cast<int>(m));
    return p;
}
inline TurretParams makeParams(const EyeFrame& e, float gapM, float dMin, float dMax, Group group = Group::kPanels) {
    const float* t = group == Group::kRadar ? e.tRadar : e.t;
    TurretParams p;
    p.gap = gapM;
    p.dMin = dMin;
    p.dMax = dMax;
    p.l = e.l;
    p.r = e.r;
    p.dn = e.dn;
    p.up = e.up;
    p.ax = e.a[0];
    p.ay = e.a[1];
    p.az = e.a[2];
    p.tx = t[0];
    p.ty = t[1];
    p.tz = t[2];
    return p;
}
inline TurretParams paramsFor(const EyeFrame& e, float gapM, Group group = Group::kPanels) { return makeParams(e, gapM, gapM + kWindowNearM, kWindowFarM, group); }
// The push: a negative gap. A vertex within 1 m of the anchor moves backM metres deeper along its ray from it.
inline TurretParams pushParamsFor(const EyeFrame& e, float backM, Group group = Group::kPanels) { return makeParams(e, -backM, kPushNearM, kPushSplitM, group); }
// The translation alone: no radial move (gap 0), every vertex moves by T.
inline TurretParams translateParamsFor(const EyeFrame& e, Group group = Group::kPanels) { return makeParams(e, 0.0f, kPushNearM, kPushSplitM, group); }

// Which constants a group's draws run with, from its push key, the gap key and its forward key (centimetres; the panels' is fix.srv_turret_hud_forward, the radar's
// fix.srv_turret_radar_forward): the push when above 0, else the pull when the gap is above 0, else the translation alone when the group's forward key is above 0 (the eye frame's T for the
// group carries it), else none (false: the draw goes through as the game made it). The group's translation is in every case.
inline bool paramsForGroup(const EyeFrame& e, Group group, int pushCm, int gapCm, int forwardCm, TurretParams& out) {
    if (pushCm > 0) {
        out = pushParamsFor(e, static_cast<float>(pushCm > kPushMaxCm ? kPushMaxCm : pushCm) / 100.0f, group);
        return true;
    }
    if (gapCm > 0) {
        out = paramsFor(e, static_cast<float>(gapCm > kGapMaxCm ? kGapMaxCm : gapCm) / 100.0f, group);
        return true;
    }
    if (forwardCm > 0) {
        out = translateParamsFor(e, group);
        return true;
    }
    return false;
}
inline int forwardCmOf(Group g, int hudForwardCm, int radarForwardCm) { return g == Group::kPanels ? hudForwardCm : radarForwardCm; }
inline int pushCmOf(Group g, int panelsCm, int radarCm) { return g == Group::kPanels ? panelsCm : radarCm; }

// The constants a shader may be given (the binding's validity test, written here so the rig can run it): all finite; |gap| <= 2 m; a window above the gap and above 0 (dMax > dMin > max(gap, 0)),
// so the divisor d - gap is positive; r > l and up > dn; the anchor and the translation within 2 m; the depth mode one of 0, 1 and 2; and a move that does something (a gap or a translation).
inline bool paramsUsable(const TurretParams& p) {
    const float all[] = {p.gap, p.dMin, p.dMax, p.depth, p.l, p.r, p.dn, p.up, p.ax, p.ay, p.az, p.tx, p.ty, p.tz};
    for (float f : all)
        if (!std::isfinite(f)) return false;
    if (p.depth != 0.0f && p.depth != 1.0f && p.depth != 2.0f) return false;
    if (!(std::fabs(p.gap) <= kAnchorLimitM)) return false;
    if (!(p.dMin > p.gap) || !(p.dMin > 0.0f) || !(p.dMax > p.dMin)) return false;
    if (!(p.r > p.l) || !(p.up > p.dn)) return false;
    const double a2 = double(p.ax) * p.ax + double(p.ay) * p.ay + double(p.az) * p.az, t2 = double(p.tx) * p.tx + double(p.ty) * p.ty + double(p.tz) * p.tz;
    if (!(a2 < double(kAnchorLimitM) * kAnchorLimitM) || !(t2 <= double(kAnchorLimitM) * kAnchorLimitM)) return false;
    return p.gap != 0.0f || t2 > 0.0;
}

// Whether a vertex's own NDC depth (clip z / w) is a valid one for Elite's reversed Z: w > 0 and 0 <= z / w <= 1. A vertex the game put behind the eye, or inside the 2.5 cm near plane, is not.
template <class T>
inline bool ndcDepthValid(T z, T w) {
    if (!(w > T(0))) return false;
    const T n = z / w;
    return n >= T(0) && n <= T(1);
}

// The move on the CPU: the shader's rule written again for the rig and for the instrument (T is float or double). `in` is the vertex's clip position {x, y, z, w}; `out` receives the clip
// position after the move (the same as `in` when the vertex is left alone); the return is whether it moved. before and after (when given) are the anchor distance of the vertex as the
// game made it and of the moved one; zfixed (when given) says the moved vertex took the depth fallback; displacement (when given) receives Q - P, the eye-space move the vertex made (X right, Y
// up, Z forward; zeros for a vertex left alone), which signature H applies to the lighting position.
template <class T>
inline bool anchoredMove(const TurretParams& p, const T in[4], T out[4], T* before = nullptr, T* after = nullptr, bool* zfixed = nullptr, T* displacement = nullptr) {
    for (int i = 0; i < 4; ++i) out[i] = in[i];
    if (displacement) displacement[0] = displacement[1] = displacement[2] = T(0);
    const T w = in[3];
    if (!(std::fabs(w) >= T(kPlaneEpsM))) return false;
    const T l = p.l, r = p.r, dn = p.dn, up = p.up;
    const T tx = l + (in[0] / w + T(1)) * (r - l) / T(2), ty = dn + (in[1] / w + T(1)) * (up - dn) / T(2);
    const T P[3] = {w * tx, w * ty, w};
    const T P1[3] = {P[0] + T(p.tx), P[1] + T(p.ty), P[2] + T(p.tz)};
    const T V[3] = {P1[0] - T(p.ax), P1[1] - T(p.ay), P1[2] - T(p.az)};
    const T d = std::sqrt(V[0] * V[0] + V[1] * V[1] + V[2] * V[2]);
    const bool inWindow = d > T(p.dMin) && d < T(p.dMax);
    const bool translated = p.tx != 0.0f || p.ty != 0.0f || p.tz != 0.0f;
    if (!translated && !inWindow) return false;
    T Q[3] = {P1[0], P1[1], P1[2]};
    if (inWindow) {
        const T k = (d - T(p.gap)) / d;
        Q[0] = T(p.ax) + V[0] * k;
        Q[1] = T(p.ay) + V[1] * k;
        Q[2] = T(p.az) + V[2] * k;
    }
    const T w2 = Q[2];   // any Q.z: the reprojection is linear in Q, and the rasterizer clips a corner that ends behind the eye
    const bool depthOk = ndcDepthValid<T>(in[2], w);
    T ndcZ = depthOk ? in[2] / w : T(kFallbackNdcZ);
    if (p.depth > 1.5f) {
        ndcZ = T(kFallbackNdcZ);   // front
    } else if (p.depth > 0.5f) {   // real: the moved position's own depth, with the vertex's own clip z
        if (!(std::fabs(w2) > T(kPlaneEpsM))) {
            ndcZ = T(kFallbackNdcZ);
        } else {
            ndcZ = in[2] / w2;
            if (ndcZ < T(0)) ndcZ = T(0);
            if (ndcZ > T(kFallbackNdcZ)) ndcZ = T(kFallbackNdcZ);
        }
    }
    if (displacement) {
        displacement[0] = Q[0] - P[0];
        displacement[1] = Q[1] - P[1];
        displacement[2] = Q[2] - P[2];
    }
    out[0] = (T(2) * Q[0] - w2 * (r + l)) / (r - l);
    out[1] = (T(2) * Q[1] - w2 * (up + dn)) / (up - dn);
    out[2] = ndcZ * w2;
    out[3] = w2;
    if (zfixed) *zfixed = !depthOk;
    if (before) *before = std::sqrt((P[0] - T(p.ax)) * (P[0] - T(p.ax)) + (P[1] - T(p.ay)) * (P[1] - T(p.ay)) + (P[2] - T(p.az)) * (P[2] - T(p.az)));
    if (after) *after = std::sqrt((Q[0] - T(p.ax)) * (Q[0] - T(p.ax)) + (Q[1] - T(p.ay)) * (Q[1] - T(p.ay)) + (Q[2] - T(p.az)) * (Q[2] - T(p.az)));
    return true;
}

// The anchor distance of a vertex from its clip position (the eye-space position is linear in the clip position: X = x (r - l) / 2 + w (r + l) / 2, likewise Y, and Z = w).
inline double anchorDistanceOfClip(const TurretParams& p, const float c[4]) {
    const double w = c[3];
    const double X = c[0] * (double(p.r) - p.l) / 2.0 + w * (double(p.r) + p.l) / 2.0, Y = c[1] * (double(p.up) - p.dn) / 2.0 + w * (double(p.up) + p.dn) / 2.0;
    const double dx = X - p.ax, dy = Y - p.ay, dz = w - p.az;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

// ---- the anchor ---------------------------------------------------------------------------------------------------------------------------------------------------------
// The fixed point in the seated frame the move is made about: the head's position when the turret view came on. It is taken again (and the module says so in the log, once each) when
// the turret view comes on, when fix.srv_turret_camera_back, fix.srv_turret_hud_forward or fix.srv_turret_radar_forward changes, when the head jumps more than kRecentreJumpM between two frames (a recentre), and when
// the head has stayed more than kAwayM from it for kAwayFrames frames in a row (whatever the cause: a safety net, "head away"). Below kAwayM nothing changes, so leaning keeps the
// real-panel behaviour.
// The camera keys take a moment to reach the pose (the runtime applies the alignment at its next poll), so after the view comes on or a key changes the anchor FOLLOWS the head
// (the move then behaves like the old per-eye shift) until kAnchorSettleFrames frames have MOVED it and is latched then. A frame counts only when the head differs from the previous
// frame's by more than kFrozenPoseM: the headset's pose can be frozen for seconds (the flight of b5c49237: the same bits from the launch to the turret view and on, then live), and an
// anchor latched on a frozen pose is a point a metre from the head when the pose goes live. The first frame, with no previous, is not counted. A recentre latches at once.
enum class AnchorReason : uint8_t { kNone = 0, kViewOn, kCameraBack, kHudForward, kRecentre, kHeadAway, kRadarForward };
inline const char* anchorReasonName(AnchorReason r) {
    switch (r) {
        case AnchorReason::kViewOn: return "view on";
        case AnchorReason::kCameraBack: return "camera back changed";
        case AnchorReason::kHudForward: return "HUD forward changed";
        case AnchorReason::kRecentre: return "recentre";
        case AnchorReason::kHeadAway: return "head away";
        case AnchorReason::kRadarForward: return "radar forward changed";
        default: return "none";
    }
}
inline constexpr double kRecentreJumpM = 0.20;     // a head step between two frames above this is a recentre
inline constexpr double kFrozenPoseM = 1.0e-6;     // a head that differs from the previous frame's by no more than this has not moved (a frozen pose)
inline constexpr uint32_t kAnchorSettleFrames = 20;
inline constexpr double kAwayM = 0.35;             // the head farther than this from the latched anchor ...
inline constexpr uint32_t kAwayFrames = 90;        // ... for this many frames in a row (about a second at 90 Hz) takes the anchor again
class AnchorTracker {
    bool active_ = false, valid_ = false, prevValid_ = false;
    int backKey_ = 0, forwardKey_ = 0, radarForwardKey_ = 0;
    AnchorReason pending_ = AnchorReason::kNone;
    uint32_t settle_ = 0, awayFrames_ = 0;
    double a_[3] = {0.0, 0.0, 0.0}, prev_[3] = {0.0, 0.0, 0.0};

    void take(const double head[3]) {
        for (int i = 0; i < 3; ++i) a_[i] = head[i];
        valid_ = true;
        awayFrames_ = 0;
    }

public:
    bool valid() const { return valid_; }
    const double* anchor() const { return a_; }
    bool settling() const { return pending_ != AnchorReason::kNone; }
    void reset() { *this = AnchorTracker(); }
    // One call a frame while the pass is wanted, with the head's seated position and the camera keys. Returns the reason when the anchor was latched THIS frame, else kNone.
    AnchorReason step(const double head[3], bool on, int backCm, int forwardCm, int radarForwardCm = 0) {
        if (!on) {
            reset();
            return AnchorReason::kNone;
        }
        for (int i = 0; i < 3; ++i)
            if (!std::isfinite(head[i])) return AnchorReason::kNone;
        if (!active_) {
            active_ = true;
            pending_ = AnchorReason::kViewOn;
            settle_ = kAnchorSettleFrames;
        } else {
            if (backCm != backKey_) {
                pending_ = AnchorReason::kCameraBack;
                settle_ = kAnchorSettleFrames;
            }
            if (forwardCm != forwardKey_) {
                pending_ = AnchorReason::kHudForward;
                settle_ = kAnchorSettleFrames;
            }
            if (radarForwardCm != radarForwardKey_) {
                pending_ = AnchorReason::kRadarForward;
                settle_ = kAnchorSettleFrames;
            }
        }
        backKey_ = backCm;
        forwardKey_ = forwardCm;
        radarForwardKey_ = radarForwardCm;
        AnchorReason latched = AnchorReason::kNone;
        const double dx = head[0] - prev_[0], dy = head[1] - prev_[1], dz = head[2] - prev_[2];
        const double delta = prevValid_ ? std::sqrt(dx * dx + dy * dy + dz * dz) : 0.0;
        if (prevValid_ && delta > kRecentreJumpM) {
            take(head);
            pending_ = AnchorReason::kNone;
            latched = AnchorReason::kRecentre;
        } else if (pending_ != AnchorReason::kNone) {
            take(head);
            if (prevValid_ && delta > kFrozenPoseM && settle_ > 0 && --settle_ == 0) {   // only a frame in which the head moved counts
                latched = pending_;
                pending_ = AnchorReason::kNone;
            }
        } else if (valid_) {
            const double ex = head[0] - a_[0], ey = head[1] - a_[1], ez = head[2] - a_[2];
            if (std::sqrt(ex * ex + ey * ey + ez * ez) > kAwayM) {
                if (++awayFrames_ >= kAwayFrames) {
                    take(head);
                    latched = AnchorReason::kHeadAway;
                }
            } else {
                awayFrames_ = 0;
            }
        }
        for (int i = 0; i < 3; ++i) prev_[i] = head[i];
        prevValid_ = true;
        return latched;
    }
};

// ---- the eye's frame from the host's pose ------------------------------------------------------------------------------------------------------------------------------
// head and eyeToHead are the host's 3x4 matrices, row-major (m[i * 4 + j], the translation in m[3], m[7], m[11]), in the OpenVR seated space the game is given (x right, y up, z
// BACK; forward is -z), the eye's own frame hanging off the head's (native_temporal.cpp nativeTemporalHeadPose). The eye's position and rotation are E = head * eyeToHead. The anchor in the
// eye's frame is v = R_e^T (A - E), turned into the shader's axes (x, y, -z) so that Z is the forward distance (clip w). The HUD-forward translation is (0, 0, -F) in the seated frame
// (forward), turned the same way, for each group's forward distance (the panels', the radar's). tangents are {left, right, down, up} plus the eye jitter's tangent shift (the game
// projects with those). False when anything is not finite or the tangents are not a frustum.
inline bool eyeFrameFrom(const float head[12], const float eyeToHead[12], const double anchor[3], double forwardM, double radarForwardM, const float tangents[4], EyeFrame& out) {
    for (int i = 0; i < 12; ++i)
        if (!std::isfinite(head[i]) || !std::isfinite(eyeToHead[i])) return false;
    for (int i = 0; i < 4; ++i)
        if (!std::isfinite(tangents[i])) return false;
    if (!(tangents[1] > tangents[0]) || !(tangents[3] > tangents[2]) || !std::isfinite(forwardM) || !std::isfinite(radarForwardM)) return false;
    double R[3][3], E[3];
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            R[i][j] = 0.0;
            for (int k = 0; k < 3; ++k) R[i][j] += double(head[i * 4 + k]) * eyeToHead[k * 4 + j];
        }
        E[i] = head[i * 4 + 3];
        for (int k = 0; k < 3; ++k) E[i] += double(head[i * 4 + k]) * eyeToHead[k * 4 + 3];
    }
    double v[3], tv[3], tr[3];
    for (int j = 0; j < 3; ++j) {
        v[j] = 0.0;
        for (int i = 0; i < 3; ++i) v[j] += R[i][j] * (anchor[i] - E[i]);
        tv[j] = R[2][j] * (-forwardM);        // R^T (0, 0, -F)
        tr[j] = R[2][j] * (-radarForwardM);   // ... for the radar
    }
    out.l = tangents[0];
    out.r = tangents[1];
    out.dn = tangents[2];
    out.up = tangents[3];
    out.a[0] = static_cast<float>(v[0]);
    out.a[1] = static_cast<float>(v[1]);
    out.a[2] = static_cast<float>(-v[2]);
    out.t[0] = static_cast<float>(tv[0]);
    out.t[1] = static_cast<float>(tv[1]);
    out.t[2] = static_cast<float>(-tv[2]);
    out.tRadar[0] = static_cast<float>(tr[0]);
    out.tRadar[1] = static_cast<float>(tr[1]);
    out.tRadar[2] = static_cast<float>(-tr[2]);
    return std::isfinite(out.a[0]) && std::isfinite(out.a[1]) && std::isfinite(out.a[2]) && std::isfinite(out.t[0]) && std::isfinite(out.t[1]) && std::isfinite(out.t[2]) &&
           std::isfinite(out.tRadar[0]) && std::isfinite(out.tRadar[1]) && std::isfinite(out.tRadar[2]);
}

// P00 of an eye's projection as Elite renders it, from the tangents {left, right, down, up}: the NDC span 2 over the tangent span. 0 when the tangents are not a frustum (the log prints it).
inline float p00OfTangents(const float tan[4]) {
    if (!tan) return 0.0f;
    const float span = tan[1] - tan[0];
    if (!std::isfinite(span) || !(span > 1.0e-4f) || !(span < 1.0e4f)) return 0.0f;
    return 2.0f / span;
}
inline bool p00Usable(float p00) { return std::isfinite(p00) && p00 > 0.05f && p00 < 20.0f; }
// The eye separation Elite renders with, from the two eyeToHead origins (the located one times the world scale factor, common/world_scale.h), for the log: a human range, or "not known".
inline bool ipdUsable(float ipdM) { return std::isfinite(ipdM) && ipdM > 0.02f && ipdM < 0.20f; }

// ---- the draw decision's reasons, counted and named --------------------------------------------------------------------------------------------------------------------
enum class Refuse : uint8_t { kNoEye = 0, kGameGs, kTopology, kNotReady, kNoGeometry, kCount };
inline const char* refuseName(Refuse r) {
    switch (r) {
        case Refuse::kNoEye: return "no-eye";
        case Refuse::kGameGs: return "game-gs";
        case Refuse::kTopology: return "topology";
        case Refuse::kNotReady: return "not-ready";
        case Refuse::kNoGeometry: return "no-geometry";
        default: return "?";
    }
}

// ---- the GPU instrument (srv_turret.cpp): what a sampled draw's geometry shader wrote ----------------------------------------------------------------------------------
// Once a second per eye and family one matched draw is issued through a stream-output twin of the shader (the same bytecode, the same constants, still rasterized) with a
// pipeline-statistics query around it. The records the twin streamed out are the vertex as the shader wrote it (Family::strideBytes: every signature element in order, the clip position,
// AFTER the move, at float offset Family::posFloat) and, last, the clip position as the game made it (the shader's own `pre` output, at Family::preFloat). This summarizes them: how many
// vertices the shader moved (post differs from pre), the anchor distance range of those before and after, the view depth range of all of them after, and where they landed in NDC.
// With a push the weapon panels' and the radar's draws read moved vertices (distance before 0.1 to 0.7 m, after that plus the push) and the larger panels' (1.3 m and deeper) read
// none; with only the HUD-forward key every vertex reads as moved.
inline constexpr uint32_t kCaptureBufferBytes = 65536;
inline constexpr uint32_t captureMaxVertices(uint32_t strideBytes) { return strideBytes ? kCaptureBufferBytes / strideBytes : 0; }
struct CaptureSummary {
    uint32_t vertices = 0, inWindow = 0;      // inWindow: the vertices the shader moved
    uint32_t zFixed = 0;                      // ... of them the ones that took the depth fallback (the game's own depth was not a valid one: behind the eye, or inside the near plane)
    float wMin = 0.0f, wMax = 0.0f;           // clip w over all vertices, after the move
    float ndcXMin = 0.0f, ndcXMax = 0.0f;     // clip x / w over the vertices with w > 0, after the move
    float ndcYMin = 0.0f, ndcYMax = 0.0f;
    float beforeMin = 0.0f, beforeMax = 0.0f; // anchor distance of the moved vertices as the game made them (0 and 0 when none moved)
    float afterMin = 0.0f, afterMax = 0.0f;   // ... and after the move
};
inline CaptureSummary summarizeCapture(const float* records, uint32_t vertices, uint32_t strideFloats, uint32_t posFloat, uint32_t preFloat, const TurretParams& p) {
    CaptureSummary s;
    if (!records || strideFloats < posFloat + 4 || strideFloats < preFloat + 4) return s;
    if (vertices > captureMaxVertices(strideFloats * 4)) vertices = captureMaxVertices(strideFloats * 4);
    bool haveW = false, haveNdc = false, haveDist = false;
    for (uint32_t i = 0; i < vertices; ++i) {
        const float* r = records + static_cast<size_t>(strideFloats) * i + posFloat;   // x, y, z, w after the move
        const float* g = records + static_cast<size_t>(strideFloats) * i + preFloat;   // ... as the game made it
        const float x = r[0], y = r[1], w = r[3];
        if (!std::isfinite(w) || !std::isfinite(x) || !std::isfinite(y) || !std::isfinite(r[2]) || !std::isfinite(g[0]) || !std::isfinite(g[1]) || !std::isfinite(g[2]) || !std::isfinite(g[3])) continue;
        ++s.vertices;
        if (!haveW || w < s.wMin) s.wMin = w;
        if (!haveW || w > s.wMax) s.wMax = w;
        haveW = true;
        if (w > 1.0e-6f) {
            const float nx = x / w, ny = y / w;
            if (!haveNdc || nx < s.ndcXMin) s.ndcXMin = nx;
            if (!haveNdc || nx > s.ndcXMax) s.ndcXMax = nx;
            if (!haveNdc || ny < s.ndcYMin) s.ndcYMin = ny;
            if (!haveNdc || ny > s.ndcYMax) s.ndcYMax = ny;
            haveNdc = true;
        }
        if (std::memcmp(r, g, 4 * sizeof(float)) != 0) {
            ++s.inWindow;
            if (!ndcDepthValid<float>(g[2], g[3])) ++s.zFixed;
            const float b = static_cast<float>(anchorDistanceOfClip(p, g)), a = static_cast<float>(anchorDistanceOfClip(p, r));
            if (!haveDist || b < s.beforeMin) s.beforeMin = b;
            if (!haveDist || b > s.beforeMax) s.beforeMax = b;
            if (!haveDist || a < s.afterMin) s.afterMin = a;
            if (!haveDist || a > s.afterMax) s.afterMax = a;
            haveDist = true;
        }
    }
    return s;
}

}  // namespace srv_turret
}  // namespace edvr
