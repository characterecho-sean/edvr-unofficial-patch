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
// THE NINE TEMPORARY KEYS (all live, all VR only, 0 = the game's own; the last three are text switches):
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
//   fix.srv_turret_hud_depth    front | game | real, default front (Sean found it right in the flight of 45fa7e56): how the depth of a MOVED vertex is written (DepthMode below).
//   fix.srv_turret_hud_scissor  open | game, default open: whether the moved HUD may draw outside the scissor rectangle the game set for it at its OLD place. The game sets a scissor rectangle for
//                               the radar's draws from the radar's original screen bounds; the moved geometry was cropped to that stale rectangle (a straight horizontal edge across the disc and
//                               the hologram, which shifted as the head moved). open: while a bound draw is issued the rectangles are the whole render target (Scissor in
//                               srv_turret_hud_binding.h), the game's put back after; game keeps its rectangles.
//   fix.srv_turret_radar_anchor on | reversed | off, default on: the radar's terrain schematic (the volume the disc's picture is drawn from, about 0.44 m behind the eye at the stock
//                               camera, so the game drew nothing) moved forward by the radar-forward key IN THE GAME, along the turret's seated forward (on) or against it (reversed), put
//                               in the anchor's axes: a hook on its placement setter adds the offset to the translation the game sets (srv_radar_anchor.cpp, build 334925 only). While the placement holds that offset the
//                               two 5559BD94 billboard families are left to the game (the geometry shader does not move them); the other radar families keep its move. off: no move in
//                               the game, the geometry shader's move as before.
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
#include <cstdio>
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
// The key's default, as the module's literal and the shipped ini say it (the rig pins both): front, which Sean found right.
inline constexpr DepthMode kDepthDefault = DepthMode::kFront;
inline constexpr const char* kDepthDefaultText = "front";
inline constexpr const char* kScissorDefaultText = "open";   // fix.srv_turret_hud_scissor: open (the other: game)
inline const char* depthModeName(DepthMode m) { return m == DepthMode::kReal ? "real" : m == DepthMode::kFront ? "front" : "game"; }
// The key's text; anything else is the default (the settings line says which it took).
inline DepthMode depthModeOf(const char* text) {
    if (text && !std::strcmp(text, "real")) return DepthMode::kReal;
    if (text && !std::strcmp(text, "front")) return DepthMode::kFront;
    return DepthMode::kGame;
}
// fix.srv_turret_radar_anchor (on | reversed | off, default on): whether the radar's terrain schematic is moved forward IN THE GAME by the radar-forward key (srv_radar_anchor.cpp, build
// 334925 only), along the anchor's +Z (on) or -Z (reversed); off leaves the game's placement. RadarAnchorOffset below makes the offset.
enum class RadarAnchorMode : uint8_t { kOn = 0, kReversed = 1, kOff = 2 };
inline constexpr RadarAnchorMode kRadarAnchorDefault = RadarAnchorMode::kOn;
inline constexpr const char* kRadarAnchorDefaultText = "on";
inline const char* radarAnchorModeName(RadarAnchorMode m) { return m == RadarAnchorMode::kReversed ? "reversed" : m == RadarAnchorMode::kOff ? "off" : "on"; }
// The key's text; anything else is the default, on (the settings line says which it took).
inline RadarAnchorMode radarAnchorModeOf(const char* text) {
    if (text && !std::strcmp(text, "reversed")) return RadarAnchorMode::kReversed;
    if (text && !std::strcmp(text, "off")) return RadarAnchorMode::kOff;
    return RadarAnchorMode::kOn;
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

// ---- the radar anchor moved in the game (fix.srv_turret_radar_anchor; srv_radar_anchor.cpp) ----------------------------------------------------------------------------------
// The published offset: (0, 0, s * F) metres in the anchor entity's own axes, F the radar-forward key, s +1 for on and -1 for reversed (flown 2026-10-31: on is forward, about 1:1). The
// hook adds s * F along the turret's forward instead when it has u and the anchor's rotation (radarAnchorAlong below), and this is its fallback. All bits zero (never a -0.0) unless
// the Scorpion's turret view is on, the key is not off and F is above 0.
struct RadarAnchorOffset {
    float m[3] = {0.0f, 0.0f, 0.0f};
    uint32_t bits[3] = {0u, 0u, 0u};
    bool zero() const { return (bits[0] | bits[1] | bits[2]) == 0u; }
};
inline RadarAnchorOffset radarAnchorOffset(bool on, RadarAnchorMode mode, int radarForwardCm) {
    RadarAnchorOffset o;
    if (!on || mode == RadarAnchorMode::kOff || radarForwardCm <= 0) return o;
    const int cm = radarForwardCm > kRadarForwardMaxCm ? kRadarForwardMaxCm : radarForwardCm;
    o.m[2] = static_cast<float>(cm) / 100.0f * (mode == RadarAnchorMode::kReversed ? -1.0f : 1.0f);
    std::memcpy(&o.bits[2], &o.m[2], sizeof(float));
    return o;
}
// The placement setter's rule (0x38563A0 in build 334925; srv_radar_anchor.cpp). The game hands it P, a 4x4 of rows x, y, z and translation; P[12..14] is the hologram's place
// relative to the anchor in the anchor's axes (about (0, +1.96, 0) on the Scorpion: the first build REPLACED it and dropped the radar 1.96 m). The hook ADDS d to that translation
// and never replaces it, and it never adds twice: a translation that is bit for bit the one this object last left the setter with is our own value fed back, which passes through
// unchanged while d is unchanged and is re-based on the game's value it came from when d changes (a new key value, or d = 0: the game's own place back). Per object:
struct PlacementMemory {
    bool have = false;       // the object's placement holds our offset (the last call moved it)
    uint32_t in[3] = {};     // the game's translation that was moved (bits)
    uint32_t out[3] = {};    // what the setter was given (bits)
    uint32_t d[3] = {};      // the offset added (bits)
};
enum class PlacementMove : uint8_t { kNone = 0, kAdded, kOurs, kRestored };
inline const char* placementMoveName(PlacementMove m) {
    return m == PlacementMove::kAdded ? "added" : m == PlacementMove::kOurs ? "ours" : m == PlacementMove::kRestored ? "restored" : "none";
}
// Q receives what the setter is to be given; m is updated. kNone: Q = P (d zero and P the game's own). kAdded: Q = P with d added to the game's translation (P's, or the one a
// fed-back P came from). kOurs: Q = P (our own value fed back, d unchanged). kRestored: d is zero and P is our value fed back: Q = P with the game's translation it came from.
inline PlacementMove radarPlacementMove(const float P[16], const uint32_t d[3], PlacementMemory& m, float Q[16]) {
    std::memcpy(Q, P, 16 * sizeof(float));
    uint32_t t[3];
    std::memcpy(t, P + 12, sizeof(t));
    const bool fedBack = m.have && t[0] == m.out[0] && t[1] == m.out[1] && t[2] == m.out[2];
    const bool sameD = d[0] == m.d[0] && d[1] == m.d[1] && d[2] == m.d[2];
    if (fedBack && sameD) return PlacementMove::kOurs;
    uint32_t base[3] = {t[0], t[1], t[2]};
    if (fedBack) std::memcpy(base, m.in, sizeof(base));
    if ((d[0] | d[1] | d[2]) == 0u) {
        m = PlacementMemory{};
        if (!fedBack) return PlacementMove::kNone;
        std::memcpy(Q + 12, base, sizeof(base));
        return PlacementMove::kRestored;
    }
    float b[3], df[3];
    std::memcpy(b, base, sizeof(b));
    std::memcpy(df, d, sizeof(df));
    Q[12] = b[0] + df[0];
    Q[13] = b[1] + df[1];
    Q[14] = b[2] + df[2];
    m.have = true;
    std::memcpy(m.in, base, sizeof(m.in));
    std::memcpy(m.out, Q + 12, sizeof(m.out));
    std::memcpy(m.d, d, sizeof(m.d));
    return PlacementMove::kAdded;
}
// THE DIRECTION (flight of 07bde936). The game's placement keeps the radar on the turret's yaw axis, and d = (0, 0, s * F) is along the VEHICLE's forward (the anchor's +Z): the disc swung
// off as the turret turned, and sat about 5 cm high, because the geometry shader moves the icons along the seated forward, about 7.5 deg below the vehicle's level. So d is F along the
// TURRET's seated forward u, expressed in the anchor's axes: d = s * F * (u . A_row0, u . A_row1, u . A_row2).
//   u comes from the live camera's world axes (VS cb1 rows 277..279 of an eye-0 draw: right, up, forward; the rows signature H's geometry shader reads) and eye 0's seated rotation R of
//   the same frame (head * eyeToHead[0], the host's row-major 3x3; OpenVR axes, x right, y up, z back). The seated forward (0, 0, -1) in the eye's axes is e = R^T (0, 0, -1), and an
//   eye-axes vector (x, y, z) is x * right + y * up - z * forward in the world.
// Axes are usable when each is a unit vector and they are orthogonal, within kAxisTolerance.
inline constexpr double kAxisTolerance = 0.05;
inline bool axesUsable(const float a[3], const float b[3], const float c[3]) {
    const float* v[3] = {a, b, c};
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j)
            if (!std::isfinite(v[i][j])) return false;
        const double n = std::sqrt(double(v[i][0]) * v[i][0] + double(v[i][1]) * v[i][1] + double(v[i][2]) * v[i][2]);
        if (std::fabs(n - 1.0) > kAxisTolerance) return false;
    }
    for (int i = 0; i < 3; ++i) {
        const float* p = v[i];
        const float* q = v[(i + 1) % 3];
        if (std::fabs(double(p[0]) * q[0] + double(p[1]) * q[1] + double(p[2]) * q[2]) > kAxisTolerance) return false;
    }
    return true;
}
// u, unit, in the game's world. False when the camera's axes are not usable or R is not finite.
inline bool turretForwardWorld(const float right[3], const float up[3], const float forward[3], const double R[9], float u[3]) {
    if (!axesUsable(right, up, forward)) return false;
    for (int i = 0; i < 9; ++i)
        if (!std::isfinite(R[i])) return false;
    const double e[3] = {-R[6], -R[7], -R[8]};   // R^T (0, 0, -1): minus R's third row
    double w[3];
    for (int i = 0; i < 3; ++i) w[i] = e[0] * right[i] + e[1] * up[i] - e[2] * forward[i];
    const double n = std::sqrt(w[0] * w[0] + w[1] * w[1] + w[2] * w[2]);
    if (!(n > 0.5) || !std::isfinite(n)) return false;
    for (int i = 0; i < 3; ++i) u[i] = static_cast<float>(w[i] / n);
    return true;
}
// d = sF * u in the anchor's axes. A is the anchor's world 4x4 (rows x, y, z, translation; the row-vector convention the game composes M with). uAnchor receives u in the anchor's axes. False
// when u is not a unit vector or A's rotation rows are not usable: the caller falls back to (0, 0, sF).
inline bool radarAnchorAlong(const float u[3], const float A[16], float sF, float d[3], float uAnchor[3]) {
    if (!std::isfinite(sF)) return false;
    for (int i = 0; i < 3; ++i)
        if (!std::isfinite(u[i])) return false;
    const double un = std::sqrt(double(u[0]) * u[0] + double(u[1]) * u[1] + double(u[2]) * u[2]);
    if (std::fabs(un - 1.0) > kAxisTolerance || !axesUsable(A, A + 4, A + 8)) return false;
    for (int i = 0; i < 3; ++i) {
        const float* row = A + 4 * i;
        uAnchor[i] = static_cast<float>((double(u[0]) * row[0] + double(u[1]) * row[1] + double(u[2]) * row[2]) / un);
        d[i] = sF * uAnchor[i];
    }
    return true;
}
// THE LAG (flight of c1275025: the disc slightly lagged the turret while it turned). u reached the setter two or three frames late (the GPU readback). Now the camera rows are read on
// the CPU when the game writes them, u is paired with the anchor's rotation OF THE SAME FRAME (the host's frame sequence; SeqRing below), so the vehicle's own turning adds no error,
// and what the turret does after the sample is taken up when the frame is drawn (THE SHAKE below).
// A small ring of values keyed by the host's frame sequence: put overwrites the value of the same sequence, else the oldest; find gives the value of exactly that sequence (0 is none).
template <typename T, uint32_t N>
struct SeqRing {
    uint64_t seq[N] = {};
    T val[N] = {};
    uint32_t next = 0;
    void put(uint64_t s, const T& v) {
        if (!s) return;
        for (uint32_t i = 0; i < N; ++i)
            if (seq[i] == s) {
                val[i] = v;
                return;
            }
        seq[next] = s;
        val[next] = v;
        next = (next + 1) % N;
    }
    const T* find(uint64_t s) const {
        if (!s) return nullptr;
        for (uint32_t i = 0; i < N; ++i)
            if (seq[i] == s) return &val[i];
        return nullptr;
    }
};
struct Rot9 {
    float r[9] = {};   // the anchor's rotation rows x, y, z
};
// The turret's aim relative to the vehicle, in degrees: yaw = atan2(x, z) about the anchor's y, pitch = asin(y), from u in the anchor's axes.
struct Aim {
    double yaw = 0.0, pitch = 0.0;
};
inline double wrapDeg(double a) {
    while (a > 180.0) a -= 360.0;
    while (a < -180.0) a += 360.0;
    return a;
}
inline Aim aimOf(const float v[3]) {
    Aim a;
    const double n = std::sqrt(double(v[0]) * v[0] + double(v[1]) * v[1] + double(v[2]) * v[2]);
    if (!(n > 0.0) || !std::isfinite(n)) return a;
    double y = v[1] / n;
    y = y > 1.0 ? 1.0 : y < -1.0 ? -1.0 : y;
    a.yaw = std::atan2(double(v[0]), double(v[2])) * 57.29577951308232;
    a.pitch = std::asin(y) * 57.29577951308232;
    return a;
}
inline void unitOfAim(const Aim& a, float out[3]) {
    const double y = a.yaw / 57.29577951308232, p = a.pitch / 57.29577951308232;
    out[0] = static_cast<float>(std::cos(p) * std::sin(y));
    out[1] = static_cast<float>(std::sin(p));
    out[2] = static_cast<float>(std::cos(p) * std::cos(y));
}
// THE SHAKE (flights of a8465cb3 and c8d79c6a). Leading the sample by its age in host frames flipped between 0 and 1 frame and shook the disc; a lead in time (the measured display
// horizon) took the mean error to 0 (residual -0.003..+0.003 deg, 0.00 frames of latency) but left +-1 to 2 deg of the turret's own changes, which no prediction can foresee. So the
// setter uses the latest sample as it is, with NO lead, and the radar's billboards are corrected when the frame is drawn, from that frame's own camera: each moves by
// delta = desired - used, in the world, where used is the offset the shown setter call left in force (put through the anchor's rotation it read) and desired is s * F along this
// frame's true u in the same anchor's axes (radarCorrectionWorld). Unled, an error in which call a write shows would be a smooth one-frame lag, not a jitter.
//
// delta in the world: dStar is exactly the offset the setter would have set for u_true with the shown call's anchor (radarAnchorAlong, the same function and the same rows), then
// (dStar - dUsed) is put back through A's rows. Equal aims therefore give a delta of exactly 0 whatever small scale the rows carry. A's rows and u must pass radarAnchorAlong (unit
// and orthogonal within kAxisTolerance, as the setter needs to direct at all); false otherwise (the draw is the game's own). dStar receives the desired offset in A's axes.
inline bool radarCorrectionWorld(const float uTrue[3], const float A[16], float sF, const float dUsed[3], float deltaW[3], float dStar[3]) {
    if (!std::isfinite(sF) || sF == 0.0f) return false;
    for (int i = 0; i < 3; ++i)
        if (!std::isfinite(dUsed[i])) return false;
    float uAnchor[3];
    if (!radarAnchorAlong(uTrue, A, sF, dStar, uAnchor)) return false;
    for (int j = 0; j < 3; ++j) {
        double w = 0.0;
        for (int i = 0; i < 3; ++i) w += (double(dStar[i]) - dUsed[i]) * A[i * 4 + j];
        deltaW[j] = static_cast<float>(w);
    }
    return std::isfinite(deltaW[0]) && std::isfinite(deltaW[1]) && std::isfinite(deltaW[2]);
}
// A world displacement in one eye's space, the shader's axes (X right, Y up, Z forward), from that draw's camera rows (VS b1 rows 277..279: right, up, forward; 4 floats a row): the
// inverse of signature H's lighting line (l += g277 * d.x + g278 * d.y + g279 * d.z), so the lighting position moves by the same world displacement.
inline void radarDeltaInEye(const float rows[12], const float deltaW[3], float out[3]) {
    for (int k = 0; k < 3; ++k) out[k] = rows[4 * k] * deltaW[0] + rows[4 * k + 1] * deltaW[1] + rows[4 * k + 2] * deltaW[2];
}
// A correction larger than kCorrectionMaxM leaves the draw to the game: a sanity bound only, since every draw left to the game pops (flight da716942: a 10 cm cap left 4 to 12 draws a
// window). One larger than kCorrectionLargeM is applied and counted as large.
inline constexpr double kCorrectionMaxM = 0.50;
inline constexpr double kCorrectionLargeM = 0.10;
// THE RESIDUAL (the instrument): at each camera write, the true aim (that camera with its own pose, in the anchor's axes of its frame) minus the aim the setter call it shows used. Lagging
// when it has the sign of the turret's rate, leading when the opposite; with the rate above kResidualMinRateDegPerFrame its quotient by the rate a frame is the latency left, in frames.
inline constexpr double kResidualMinRateDegPerFrame = 0.5;
inline int residualSide(double residualDeg, double rateDegPerFrame) {
    if (std::fabs(rateDegPerFrame) < kResidualMinRateDegPerFrame || residualDeg == 0.0) return 0;
    return (residualDeg > 0.0) == (rateDegPerFrame > 0.0) ? 1 : -1;
}
struct ResidualStats {
    uint64_t n = 0, lagging = 0, leading = 0, latN = 0;
    double sum = 0.0, maxAbs = 0.0, latSum = 0.0;
    void add(double residualDeg, double rateDegPerFrame) {
        if (!std::isfinite(residualDeg)) return;
        ++n;
        sum += residualDeg;
        maxAbs = std::fabs(residualDeg) > maxAbs ? std::fabs(residualDeg) : maxAbs;
        const int side = residualSide(residualDeg, rateDegPerFrame);
        if (side > 0) ++lagging;
        if (side < 0) ++leading;
        if (side != 0) {
            latSum += residualDeg / rateDegPerFrame;
            ++latN;
        }
    }
    double mean() const { return n ? sum / double(n) : 0.0; }
    double latency() const { return latN ? latSum / double(latN) : 0.0; }
};
// The head's pitch in the seat from eye 0's seated rotation R (row-major, OpenVR axes): the eye's forward is minus R's third column. For the log beside u's pitch: a pairing error would
// make u's pitch follow the head's; gun elevation does not.
inline double headPitchDeg(const double R[9]) {
    double y = -R[5];
    y = y > 1.0 ? 1.0 : y < -1.0 ? -1.0 : y;
    return std::asin(y) * 57.29577951308232;
}
// The angle between a vector and +Z (the anchor's forward), in degrees; its yaw about Y (atan2(x, z)) for the log.
inline double angleToZDeg(const float v[3]) {
    const double n = std::sqrt(double(v[0]) * v[0] + double(v[1]) * v[1] + double(v[2]) * v[2]);
    if (!(n > 0.0)) return 0.0;
    double c = v[2] / n;
    c = c > 1.0 ? 1.0 : c < -1.0 ? -1.0 : c;
    return std::acos(c) * 57.29577951308232;
}
inline double yawDeg(const float v[3]) { return std::atan2(double(v[0]), double(v[2])) * 57.29577951308232; }
// A sign of a caller that feeds back a value of ours in some other form (not bit for bit): the game's translation moved between two calls by about d (within a quarter of |d|).
inline bool placementStepLooksLikeOffset(const float before[3], const float after[3], const float d[3]) {
    const double dd = std::sqrt(double(d[0]) * d[0] + double(d[1]) * d[1] + double(d[2]) * d[2]);
    if (!(dd > 1.0e-4)) return false;
    const double e[3] = {double(after[0]) - before[0] - d[0], double(after[1]) - before[1] - d[1], double(after[2]) - before[2] - d[2]};
    return std::sqrt(e[0] * e[0] + e[1] * e[1] + e[2] * e[2]) <= 0.25 * dd;
}
// The families the native move leaves to the game while it is applied: the hologram billboard 5559BD94 with either pixel shader (the radar disc's picture, the Scorpion's hologram).
// The game frames and places those from the moved schematic itself; the other radar families keep the geometry shader's move.
inline bool radarAnchorLeavesFamily(int family) { return family == 7 || family == 8; }
static_assert(kFamilies[7].vs == 0x5559BD94B6852E83ull && kFamilies[7].ps == 0xEA02FAC2BD6C643Cull && kFamilies[8].vs == 0x5559BD94B6852E83ull && kFamilies[8].ps == 0xE95634B0F61D218Full,
              "the two families the native radar anchor leaves to the game are the 5559BD94 billboard's");
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
// THE CONTENT DEPTH (the radar billboards' draw-time correction, flight e64866f9). A billboard quad shows a picture of content at depth z_c (the radar schematic's origin, about 0.34 m),
// whatever the quad's own depth. Moving the quad's vertices by one 3D delta shifts it on screen by delta over the QUAD's depth; moving each vertex by delta * w / z_c instead (w its clip w,
// the eye-space depth) puts it where the content point on the same ray at depth z_c, moved by delta, projects: q = p + delta p.z / z_c projects to (p.x / p.z + delta.x / z_c) / (1 +
// delta.z / z_c), the content's own. The constants carry 1 / z_c (TurretParams::invZc; 0 for every other draw, which moves by the translation as it is).
inline constexpr float kContentInvZMax = 20.0f;                              // z_c of 5 cm or more
inline constexpr double kContentDepthMinM = 0.10, kContentDepthMaxM = 2.0;   // a V0 schematic origin outside this is not taken as the content's depth
inline constexpr uint32_t kContentDepthFreshFrames = 90;                     // ... nor one whose last good read is older than this

// The geometry shader's constant buffer (GS slot 0, which the game never binds): four registers.
//   [0] gap, dMin, dMax, depth       the move: metres nearer (negative: deeper), and the window of anchor distances it applies to; the DepthMode (0 game, 1 real, 2 front)
//   [1] l, r, dn, up                 the eye's projection tangents as the game projects (left and down negative; the jitter shift is added)
//   [2] ax, ay, az, lock             the anchor in the eye's space: X right, Y up, Z forward (= clip w); lock 1: THE LOCK (below), 0 for every other draw
//   [3] tx, ty, tz, invZc            the translation (HUD forward) in the same axes, or with lock 1 the radar billboard's target in them; invZc 1 / the content depth z_c (THE
//                                    CONTENT DEPTH below), 0 for every draw but the radar billboards' draw-time correction
struct TurretParams {
    float gap = 0.0f, dMin = 0.0f, dMax = 0.0f, depth = 0.0f;   // depth: the DepthMode as 0, 1 or 2
    float l = 0.0f, r = 0.0f, dn = 0.0f, up = 0.0f;
    float ax = 0.0f, ay = 0.0f, az = 0.0f, lock = 0.0f;
    float tx = 0.0f, ty = 0.0f, tz = 0.0f, invZc = 0.0f;
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
// so the divisor d - gap is positive; r > l and up > dn; the anchor and the translation within 2 m; the depth mode one of 0, 1 and 2; 1 / the content depth in [0, 20] (z_c of 5 cm or more,
// or none); the lock 0 or 1; and a move that does something (a gap or a translation).
inline bool paramsUsable(const TurretParams& p) {
    const float all[] = {p.gap, p.dMin, p.dMax, p.depth, p.l, p.r, p.dn, p.up, p.ax, p.ay, p.az, p.lock, p.tx, p.ty, p.tz, p.invZc};
    for (float f : all)
        if (!std::isfinite(f)) return false;
    if (!(p.invZc >= 0.0f && p.invZc <= kContentInvZMax)) return false;
    if (p.lock != 0.0f && p.lock != 1.0f) return false;
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
    // the translation, scaled by w / z_c when the content depth is given (THE CONTENT DEPTH), else as it is
    const T s = w * T(p.invZc);
    const T tv[3] = {p.invZc > 0.0f ? T(p.tx) * s : T(p.tx), p.invZc > 0.0f ? T(p.ty) * s : T(p.ty), p.invZc > 0.0f ? T(p.tz) * s : T(p.tz)};
    const T P1[3] = {P[0] + tv[0], P[1] + tv[1], P[2] + tv[2]};
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

// The content depth the radar correction runs with, as 1 / z_c: the V0 instrument's schematic origin depth `z` when it has one (`have`), read `age` frames ago at most
// kContentDepthFreshFrames, and z in [kContentDepthMinM, kContentDepthMaxM]; else 0 (the plain delta).
inline float contentInvDepth(bool have, uint32_t age, double z) {
    if (!have || age > kContentDepthFreshFrames || !(z >= kContentDepthMinM && z <= kContentDepthMaxM)) return 0.0f;
    return static_cast<float>(1.0 / z);
}

// Where content at depth zc on the ray through NDC (x, y) goes on screen when moved by delta (the eye's axes: X right, Y up, Z forward), as an NDC shift, with the constants' tangents: the
// quad probe line's expectation (the move with invZc = 1 / zc puts every vertex on the ray there). False when zc, the tangents or the moved depth are unusable.
inline bool contentShiftNdc(const TurretParams& p, double x, double y, const float delta[3], double zc, double out[2]) {
    if (!(zc > 0.0) || !(p.r > p.l) || !(p.up > p.dn)) return false;
    const double z2 = zc + double(delta[2]);
    if (!(z2 > 1.0e-6)) return false;
    const double tx = p.l + (x + 1.0) * (double(p.r) - p.l) * 0.5, ty = p.dn + (y + 1.0) * (double(p.up) - p.dn) * 0.5;
    const double tx2 = (tx * zc + delta[0]) / z2, ty2 = (ty * zc + delta[1]) / z2;
    out[0] = 2.0 * (tx2 - p.l) / (double(p.r) - p.l) - 1.0 - x;
    out[1] = 2.0 * (ty2 - p.dn) / (double(p.up) - p.dn) - 1.0 - y;
    return true;
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

// ---- the radar disc's plane in the seated frame (is it a billboard?) -------------------------------------------------------------------------------------------------
// The game builds the disc's clip position from its own constants (clip = cb2[9] + h * cb2[7] + u * cb2[6] + v * cb2[8]) and may ORIENT it toward its camera or the head, in which case a rigid
// translation keeps an orientation meant for a viewer elsewhere. The instrument reads a sampled draw's PRE-move clip positions (TURRETPRE) back out of the stream-output twin, rebuilds each
// vertex's eye-space point with that eye's tangents, and moves it to the SEATED frame with the eye's pose: the inverse of eyeFrameFrom's step for the anchor. A turret-fixed disc keeps the
// same centre and normal as the head moves; a billboard's normal turns to follow the head.
//   eye pose E = head * eyeToHead (R the 3x3, E the position), both the host's 3x4 row-major matrices; an eye-space point (X right, Y up, Z forward) is the pose-frame vector (X, Y, -Z), so
//   the seated point is E + R * (X, Y, -Z).
inline bool eyePoseOf(const float head[12], const float eyeToHead[12], double R[9], double E[3]) {
    for (int i = 0; i < 12; ++i)
        if (!std::isfinite(head[i]) || !std::isfinite(eyeToHead[i])) return false;
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            R[i * 3 + j] = 0.0;
            for (int k = 0; k < 3; ++k) R[i * 3 + j] += double(head[i * 4 + k]) * eyeToHead[k * 4 + j];
        }
        E[i] = head[i * 4 + 3];
        for (int k = 0; k < 3; ++k) E[i] += double(head[i * 4 + k]) * eyeToHead[k * 4 + 3];
    }
    return true;
}
inline void eyeToSeated(const double R[9], const double E[3], const double p[3], double out[3]) {
    const double v[3] = {p[0], p[1], -p[2]};
    for (int i = 0; i < 3; ++i) out[i] = E[i] + R[i * 3 + 0] * v[0] + R[i * 3 + 1] * v[1] + R[i * 3 + 2] * v[2];
}
struct SeatedQuad {
    double centre[3] = {0.0, 0.0, 0.0};
    double normal[3] = {0.0, 0.0, 0.0};   // unit, from the first triangle's winding
};
// The quad of six consecutive records (two triangles) starting at `firstVertex`, from the PRE clip position at float offset preFloat of each record: its centre (the mean of the six, which is
// the quad's centre for a parallelogram) and its unit normal, in the seated frame. tangents are {left, right, down, up} as the shader was given them. False when fewer than six records are
// there, anything is not finite, or the first triangle has no area.
inline bool quadInSeated(const float* records, uint32_t strideFloats, uint32_t preFloat, uint32_t firstVertex, uint32_t vertexCount, const float tangents[4], const double R[9], const double E[3],
                         SeatedQuad& out) {
    if (!records || !tangents || strideFloats < preFloat + 4 || firstVertex > vertexCount || vertexCount - firstVertex < 6) return false;
    double pts[6][3];
    for (uint32_t i = 0; i < 6; ++i) {
        const float* c = records + static_cast<size_t>(strideFloats) * (firstVertex + i) + preFloat;
        if (!std::isfinite(c[0]) || !std::isfinite(c[1]) || !std::isfinite(c[2]) || !std::isfinite(c[3])) return false;
        const double w = c[3];
        const double eye[3] = {c[0] * (double(tangents[1]) - tangents[0]) / 2.0 + w * (double(tangents[1]) + tangents[0]) / 2.0,
                               c[1] * (double(tangents[3]) - tangents[2]) / 2.0 + w * (double(tangents[3]) + tangents[2]) / 2.0, w};
        eyeToSeated(R, E, eye, pts[i]);
        for (int k = 0; k < 3; ++k)
            if (!std::isfinite(pts[i][k])) return false;
    }
    for (int k = 0; k < 3; ++k) {
        double sum = 0.0;
        for (int i = 0; i < 6; ++i) sum += pts[i][k];
        out.centre[k] = sum / 6.0;
    }
    const double a[3] = {pts[1][0] - pts[0][0], pts[1][1] - pts[0][1], pts[1][2] - pts[0][2]}, b[3] = {pts[2][0] - pts[0][0], pts[2][1] - pts[0][1], pts[2][2] - pts[0][2]};
    const double n[3] = {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
    const double len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
    if (!(len > 1.0e-12)) return false;
    for (int k = 0; k < 3; ++k) out.normal[k] = n[k] / len;
    return true;
}
// The angle between two vectors, in degrees (0 when either has no length).
inline double angleBetweenDeg(const double a[3], const double b[3]) {
    const double la = std::sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]), lb = std::sqrt(b[0] * b[0] + b[1] * b[1] + b[2] * b[2]);
    if (!(la > 0.0) || !(lb > 0.0)) return 0.0;
    double c = (a[0] * b[0] + a[1] * b[1] + a[2] * b[2]) / (la * lb);
    if (c > 1.0) c = 1.0;
    if (c < -1.0) c = -1.0;
    return std::acos(c) * 180.0 / 3.14159265358979323846;
}

// ---- THE LOCK (flight 8e3bcd7b, dump 072742058001: the game damps the radar's world placement toward P + d over about 65 ms, and differently for the disc and the icon) --------------------
// Correcting by what the setter's last d left undone takes off one frame of it and no more: at a steady -130 deg/s the disc's quad stayed about 5 cm and the icon's about 3 cm off their
// rest places, and the 2 cm between them is the Scorpion's ghost. So each radar billboard (disc, icon) is locked to its own rest place in the SEATED frame, learned while the turret and the
// vehicle are still: at each draw that place is put in the eye's space with that eye's pose, and the geometry shader moves the quad so its centre lands on it (srv_turret_hud_shader.h
// turretLockCentre and turretLock, with TurretParams::lock 1 and the target in tx, ty, tz). Before a target is learned the correction keeps its delta.
//
// The centre the shader takes for a triangle: the midpoint of its LONGEST edge. A quad's two triangles share a diagonal; for a rectangle (a billboard in the eye's space) and for a
// parallelogram cut along its long diagonal that diagonal is each triangle's longest edge, so both triangles take the same point, bit for bit (the sum of the same two points), and move by
// the same vector: no crack opens. The same comparisons in the same order as the shader. Eye-space points.
template <class T>
inline void lockCentre(const T a[3], const T b[3], const T c[3], T out[3]) {
    const auto d2 = [](const T* p, const T* q) { return (q[0] - p[0]) * (q[0] - p[0]) + (q[1] - p[1]) * (q[1] - p[1]) + (q[2] - p[2]) * (q[2] - p[2]); };
    const T ab = d2(a, b), bc = d2(b, c), ca = d2(c, a);
    const T* u = a;
    const T* v = b;
    if (!(ab >= bc && ab >= ca)) {
        u = bc >= ca ? b : c;
        v = bc >= ca ? c : a;
    }
    for (int k = 0; k < 3; ++k) out[k] = (u[k] + v[k]) * T(0.5);
}
// A seated point in one eye's space: the inverse of eyeToSeated (R the eye's seated rotation, E its position).
inline void seatedToEye(const double R[9], const double E[3], const double s[3], double out[3]) {
    const double d[3] = {s[0] - E[0], s[1] - E[1], s[2] - E[2]};
    const double v[3] = {R[0] * d[0] + R[3] * d[1] + R[6] * d[2], R[1] * d[0] + R[4] * d[1] + R[7] * d[2], R[2] * d[0] + R[5] * d[1] + R[8] * d[2]};
    out[0] = v[0];
    out[1] = v[1];
    out[2] = -v[2];
}
// The lock takes a single quad: two triangles (a list of 6 vertices or a strip of 4), one instance. Any other draw keeps the delta, counted.
inline bool lockShape(int d3d11Topology, uint32_t count, uint32_t instances) {
    if (instances > 1) return false;
    if (d3d11Topology == 4) return count == 6;
    if (d3d11Topology == 5) return count == 4;
    return false;
}
// The angle between two rotations given as three rows each (rows at [0..2], [4..6], [8..10], each normalized first), in degrees: the vehicle's turn between two frames.
inline double rotationAngleDeg(const float A[16], const float B[16]) {
    double tr = 0.0;
    for (int i = 0; i < 3; ++i) {
        const float* a = A + 4 * i;
        const float* b = B + 4 * i;
        const double la = std::sqrt(double(a[0]) * a[0] + double(a[1]) * a[1] + double(a[2]) * a[2]), lb = std::sqrt(double(b[0]) * b[0] + double(b[1]) * b[1] + double(b[2]) * b[2]);
        if (!(la > 0.0) || !(lb > 0.0)) return 1.0e9;
        tr += (double(a[0]) * b[0] + double(a[1]) * b[1] + double(a[2]) * b[2]) / (la * lb);
    }
    double c = (tr - 1.0) * 0.5;
    c = c > 1.0 ? 1.0 : c < -1.0 ? -1.0 : c;
    return std::acos(c) * 57.29577951308232;
}
// REST: the turret's yaw and pitch rates on the vehicle (the desired aim in the anchor's axes, frame to frame) and the vehicle's own turn rate (the anchor's world rotation, frame to
// frame) all under kRestDegS, for kRestFrames frames in a row. A frame without the rates (no correction computed, or the previous frame's missing) breaks it.
inline constexpr double kRestDegS = 3.0;
inline constexpr uint32_t kRestFrames = 20;
struct RestWatch {
    uint32_t frames = 0;
    bool step(bool haveRates, double turretYawDegS, double turretPitchDegS, double vehicleDegS) {
        const bool still = haveRates && std::fabs(turretYawDegS) < kRestDegS && std::fabs(turretPitchDegS) < kRestDegS && std::fabs(vehicleDegS) < kRestDegS;
        frames = still ? frames + 1 : 0;
        return atRest();
    }
    bool atRest() const { return frames >= kRestFrames; }
};
// THE TARGET of one kind, in the seated frame: the first kLockLearnSamples rest samples (the pre-move centres the pair capture measures, the eyes of a frame averaged) are averaged;
// after that each rest sample moves it by 1 - exp(-dt / kLockTauS), dt the time since the last sample taken (at most kLockMaxStepS), so a lean or the head's drift is followed.
inline constexpr uint32_t kLockLearnSamples = 6;
inline constexpr uint32_t kLockLearnEveryFrames = 5;   // the pair capture's rate while a kind has no target
inline constexpr double kLockTauS = 1.0, kLockMaxStepS = 2.0;
struct LockTarget {
    bool learned = false;
    double seated[3] = {0.0, 0.0, 0.0};
    double sum[3] = {0.0, 0.0, 0.0};
    uint32_t n = 0;           // samples of the first learning
    uint64_t relearned = 0;   // samples after it
    double lastS = -1.0;      // when the last sample was taken (seconds; below 0: none)
    // True the moment the target is first learned.
    bool add(const double s[3], double nowS) {
        for (int k = 0; k < 3; ++k)
            if (!std::isfinite(s[k])) return false;
        if (!learned) {
            for (int k = 0; k < 3; ++k) sum[k] += s[k];
            ++n;
            lastS = nowS;
            if (n < kLockLearnSamples) return false;
            for (int k = 0; k < 3; ++k) seated[k] = sum[k] / double(n);
            learned = true;
            return true;
        }
        double dt = lastS >= 0.0 ? nowS - lastS : kLockMaxStepS;
        dt = dt < 0.0 ? 0.0 : dt > kLockMaxStepS ? kLockMaxStepS : dt;
        const double a = 1.0 - std::exp(-dt / kLockTauS);
        for (int k = 0; k < 3; ++k) seated[k] += a * (s[k] - seated[k]);
        ++relearned;
        lastS = nowS;
        return false;
    }
};
// The targets start again when the turret view comes on and when fix.srv_turret_camera_back, fix.srv_turret_radar_forward or fix.srv_turret_radar_anchor changes (each moves where the
// game's radar rests); no other key does.
enum class LockReset : uint8_t { kNone = 0, kViewOn, kKey };
inline LockReset lockResetWhy(bool wasOn, bool on, int backBefore, int back, int radarForwardBefore, int radarForward, int anchorBefore, int anchor) {
    if (on && !wasOn) return LockReset::kViewOn;
    if (back != backBefore || radarForward != radarForwardBefore || anchor != anchorBefore) return LockReset::kKey;
    return LockReset::kNone;
}
inline const char* lockResetName(LockReset r) {
    return r == LockReset::kViewOn ? "the turret view came on" : r == LockReset::kKey ? "camera back, radar forward or the radar anchor key changed" : "none";
}

// ---- the radar picture's camera (TEMPORARY instrument) -----------------------------------------------------------------------------------------------------------------------------
// The radar's disc picture is drawn by vs 4EDE4BC9003BE352 (the terrain schematic) from a camera of its own, V0, which did not follow the head in the flight of 707c0ab5 (the doc's journal,
// "the radar's camera is fixed"). Its vertex constants carry V0 whole: cb0 rows 9..11 take a local vertex to camera-relative world (orthonormal columns times a scale), and cb0 rows 4..7 are the
// view-projection times that (local to clip). So VP = M A^-1; its w row is V0's forward in world axes, its x and y rows give the window (clip.x = P00 right.X + P02 fwd.X, likewise y), and the
// schematic's local origin, A's translation t, sits at (right, up, fwd) . t in V0's eye space. m: rows 4..7 (x, y, z, w; 16 floats); a: rows 9..11 (12 floats). False when a matrix is singular or
// anything is not finite. The rig checks it against the constants logged in that flight.
struct RadarCam {
    double centre[3] = {0.0, 0.0, 0.0};   // the schematic's local origin in V0's eye space: x right, y up, z forward (metres; negative z is behind V0)
    double fwd[3] = {0.0, 0.0, 0.0};      // V0's forward, in the draw's world axes
    double tanX[2] = {0.0, 0.0}, tanY[2] = {0.0, 0.0};   // the window: left, right; down, up
    double scale = 0.0;                   // the local-to-world scale (the cube root of A's determinant)
};
inline bool radarCamFrom(const float m[16], const float a[12], RadarCam& out) {
    out = RadarCam{};
    if (!m || !a) return false;
    for (int i = 0; i < 16; ++i)
        if (!std::isfinite(m[i])) return false;
    for (int i = 0; i < 12; ++i)
        if (!std::isfinite(a[i])) return false;
    const double L[3][3] = {{a[0], a[1], a[2]}, {a[4], a[5], a[6]}, {a[8], a[9], a[10]}};
    const double t[3] = {a[3], a[7], a[11]};
    const double det = L[0][0] * (L[1][1] * L[2][2] - L[1][2] * L[2][1]) - L[0][1] * (L[1][0] * L[2][2] - L[1][2] * L[2][0]) + L[0][2] * (L[1][0] * L[2][1] - L[1][1] * L[2][0]);
    if (!(std::fabs(det) > 1e-30)) return false;
    double inv[3][3];
    inv[0][0] = (L[1][1] * L[2][2] - L[1][2] * L[2][1]) / det;
    inv[0][1] = (L[0][2] * L[2][1] - L[0][1] * L[2][2]) / det;
    inv[0][2] = (L[0][1] * L[1][2] - L[0][2] * L[1][1]) / det;
    inv[1][0] = (L[1][2] * L[2][0] - L[1][0] * L[2][2]) / det;
    inv[1][1] = (L[0][0] * L[2][2] - L[0][2] * L[2][0]) / det;
    inv[1][2] = (L[0][2] * L[1][0] - L[0][0] * L[1][2]) / det;
    inv[2][0] = (L[1][0] * L[2][1] - L[1][1] * L[2][0]) / det;
    inv[2][1] = (L[0][1] * L[2][0] - L[0][0] * L[2][1]) / det;
    inv[2][2] = (L[0][0] * L[1][1] - L[0][1] * L[1][0]) / det;
    double vp[3][3];   // rows x, y, w of VP over a camera-relative world direction: vp[r][j] = sum_i m[r][i] inv[i][j]
    const int rowOf[3] = {0, 1, 3};
    for (int r = 0; r < 3; ++r)
        for (int j = 0; j < 3; ++j) vp[r][j] = m[rowOf[r] * 4 + 0] * inv[0][j] + m[rowOf[r] * 4 + 1] * inv[1][j] + m[rowOf[r] * 4 + 2] * inv[2][j];
    const double s = std::sqrt(vp[2][0] * vp[2][0] + vp[2][1] * vp[2][1] + vp[2][2] * vp[2][2]);
    if (!(s > 1e-9)) return false;
    double f[3], rgt[3], up[3];
    for (int j = 0; j < 3; ++j) f[j] = vp[2][j] / s;
    auto split = [&](const double row[3], double axis[3], double& p0, double& p2) {   // row / s = p0 axis + p2 f, with axis a unit vector orthogonal to f
        p2 = (row[0] * f[0] + row[1] * f[1] + row[2] * f[2]) / s;
        for (int j = 0; j < 3; ++j) axis[j] = row[j] / s - p2 * f[j];
        p0 = std::sqrt(axis[0] * axis[0] + axis[1] * axis[1] + axis[2] * axis[2]);
        if (!(p0 > 1e-9)) return false;
        for (int j = 0; j < 3; ++j) axis[j] /= p0;
        return true;
    };
    double p00 = 0.0, p02 = 0.0, p11 = 0.0, p12 = 0.0;
    if (!split(vp[0], rgt, p00, p02) || !split(vp[1], up, p11, p12)) return false;
    for (int j = 0; j < 3; ++j) out.fwd[j] = f[j];
    out.centre[0] = rgt[0] * t[0] + rgt[1] * t[1] + rgt[2] * t[2];
    out.centre[1] = up[0] * t[0] + up[1] * t[1] + up[2] * t[2];
    out.centre[2] = f[0] * t[0] + f[1] * t[1] + f[2] * t[2];
    out.tanX[0] = (-1.0 - p02) / p00;
    out.tanX[1] = (1.0 - p02) / p00;
    out.tanY[0] = (-1.0 - p12) / p11;
    out.tanY[1] = (1.0 - p12) / p11;
    out.scale = std::cbrt(std::fabs(det));
    return std::isfinite(out.centre[0]) && std::isfinite(out.centre[1]) && std::isfinite(out.centre[2]) && std::isfinite(out.tanX[0]) && std::isfinite(out.tanY[1]);
}

// ---- draws of a turret family that the pass did not move ---------------------------------------------------------------------------------------------------------------------
// Sean: the radar "seems to be stretched toward its origin". Besides a billboard, a second UNMOVED copy of the radar landing at its original place would smear toward it. Every route by which a
// draw of one of the families can reach the GPU without the pass's geometry shader bound is counted per family (the 10 s line's `unmoved family draws:` list) and named the first time it is seen.
//   refused           the owner's draw thunk offered the draw to the pass's scope and the scope declined it (the refused[...] counts say why)
//   group-off         ... and the family's group has its push, gap and forward keys all at 0
//   fast-path         the owner's draw thunk took the fast path (no verdict scope), so the pass never saw the draw
//   indirect          DrawInstancedIndirect or DrawIndexedInstancedIndirect: those thunks have no scope
//   auto              DrawAuto: no scope either
//   foreign-immediate a draw on a second IMMEDIATE context (not the owner's)
//   foreign-deferred  a draw RECORDED on a deferred context (replayed later by ExecuteCommandList, which no scope covers)
//   reissue           pureDrawReissue (an EDVR replay of a draw: the UI layer's, the interface depth's, the blend probe's, the crisp tone's) called while the pass's shader is not bound
//   command-list      the game executed a command list: what is inside cannot be seen, so the family is unknown
enum class UnmovedRoute : uint8_t { kRefused = 0, kGroupOff, kFastPath, kIndirect, kAuto, kForeignImmediate, kForeignDeferred, kReissue, kCommandList, kCount };
inline constexpr uint32_t kUnmovedRoutes = static_cast<uint32_t>(UnmovedRoute::kCount);
inline const char* unmovedRouteName(UnmovedRoute r) {
    switch (r) {
        case UnmovedRoute::kRefused: return "refused";
        case UnmovedRoute::kGroupOff: return "group-off";
        case UnmovedRoute::kFastPath: return "fast-path";
        case UnmovedRoute::kIndirect: return "indirect";
        case UnmovedRoute::kAuto: return "auto";
        case UnmovedRoute::kForeignImmediate: return "foreign-immediate";
        case UnmovedRoute::kForeignDeferred: return "foreign-deferred";
        case UnmovedRoute::kReissue: return "reissue";
        case UnmovedRoute::kCommandList: return "command-list";
        default: return "?";
    }
}
inline const char* unmovedRouteWhere(UnmovedRoute r) {
    switch (r) {
        case UnmovedRoute::kRefused: return "the owner context's draw, offered to the pass's scope, which declined it (the 10 s line's refused[...] says why)";
        case UnmovedRoute::kGroupOff: return "the owner context's draw, offered to the scope; the family's group has its push, gap and forward keys at 0";
        case UnmovedRoute::kFastPath: return "the owner context's draw thunk took the fast path (no verdict scope): the pass never saw the draw";
        case UnmovedRoute::kIndirect: return "DrawInstancedIndirect or DrawIndexedInstancedIndirect: vscreen's thunks for them have no scope";
        case UnmovedRoute::kAuto: return "DrawAuto: vscreen's thunk has no scope";
        case UnmovedRoute::kForeignImmediate: return "a draw on a second immediate context, outside the owner's scope";
        case UnmovedRoute::kForeignDeferred: return "a draw recorded on a deferred context, replayed later by ExecuteCommandList outside any scope";
        case UnmovedRoute::kReissue: return "pureDrawReissue (an EDVR replay of a draw) called while the pass's shader is not bound";
        case UnmovedRoute::kCommandList: return "the game executed a command list while the pass was wanted: the draws inside cannot be seen, so the family is unknown";
        default: return "?";
    }
}
// counts[family][route], with the row kFamilyCount for a draw whose family is not known (a command list). The list as the 10 s line spells it: `<family> <route> N, ...` in family then route order
// (the unknown family is `?`), or `none`. Always NUL-terminated within cap; returns the length. A list that does not fit ends at the last entry that did.
inline size_t unmovedList(char* out, size_t cap, const uint32_t (*counts)[kUnmovedRoutes]) {
    if (!out || cap == 0) return 0;
    size_t n = 0;
    out[0] = '\0';
    for (uint32_t f = 0; f <= kFamilyCount; ++f) {
        for (uint32_t r = 0; r < kUnmovedRoutes; ++r) {
            if (!counts[f][r]) continue;
            char entry[96];
            const int len = std::snprintf(entry, sizeof(entry), "%s%s %s %u", n ? ", " : "", f < kFamilyCount ? kFamilies[f].name : "?", unmovedRouteName(static_cast<UnmovedRoute>(r)), counts[f][r]);
            if (len <= 0 || n + static_cast<size_t>(len) + 1 > cap) return n;
            std::memcpy(out + n, entry, static_cast<size_t>(len) + 1);
            n += static_cast<size_t>(len);
        }
    }
    if (!n) {
        const char* none = "none";
        const size_t len = std::strlen(none);
        if (len + 1 <= cap) {
            std::memcpy(out, none, len + 1);
            n = len;
        }
    }
    return n;
}

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
