// The SRV turret view (issue 140): the pure parts, shared by the module (srv_turret.cpp), the radar hooks (srv_radar_anchor.cpp), the draw path and tools/srv_turret_hud_test, which runs
// these very functions beside the production geometry shader on WARP.
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
// -0.25) did not, because the two families do not draw them. They are drawn by other families of the same HDR-target run, between the 2CECEC30
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
//   vs B7790CBFC6554097 (ps 8DEF46452FA459F5), the flight HUD's vectors: not a turret panel at all. Its clip w in the flight of 9fdad3a1 ran over
//     -658 to +3029 m, so no window of this pass means anything for it. It was family 9 of the first build and is out of the pass (its signature and geometry shader with it).
//
// WHY THE PULL WAS REPLACED BY A PUSH. The first build brought the deep panels forward to the near layer (a window above the gap). The flight of 9fdad3a1 showed no gap makes the
// layers meet: the weapon panels (the arcs, the weapon names, the ammo counts, the INFO strip: families 1989E6D3 and 2CECEC30) lie at clip w 0.27 to 0.70 m, the radar families
// (DF3503CD, 44DDCDEB, 9B34C331, 9611A454, B932058F and the two 5559BD94 sphere pairs) at 0.11 to 0.44 m, and the larger panels (SYSTEMS, the location panel, the upper messages) at
// 1.3 to 2.8 m. So the near layer is pushed BACK to the larger panels' depth instead, with the same geometry shader and a NEGATIVE gap.
// WHY THE MOVE IS IN 3D. The first push changed only the clip x of a vertex, which is exactly right for a head at one place and keeps the panel's motion parallax and looming of the
// OLD depth: the flight of e45c68a9 (Sean: the radar and the panels "move with my head", a panel "got larger" when he moved toward it). The shader now moves each vertex for real
// about a fixed anchor (see THE MOVE below). And at the game's own camera the whole turret HUD lies about 0.7 m too far back for the head (the radar behind it), so the whole HUD is
// first translated forward (experimental.srv_turret_hud_forward), which is the old camera move applied to the HUD only: the world keeps the game's camera.
//
// THE TEN KEYS (all live, all VR only, 0 = the game's own; three of them are text switches). They are [experimental] keys. The four placement defaults are the camera Sean flew on
// 2026-11-02 (Frontier, edvr_gfx_20261102_040458, settled from 04:08:34): camera back 90, HUD forward 0, radar forward 0, panels back 110. The scissor stays open and the radar anchor
// stays camera (with radar forward at 0 those modes match what that flight had set). The shipped ini and the module's literals say the same (the rig pins both):
//   experimental.srv_turret_camera_back  centimetres, 0..150, default 90: the camera moves back in the turret view, over the turret's axis. Part 1: published as the seated z of the eye alignment
//                               (alignTick), the single publisher of that alignment, so its serials stay coherent.
//   experimental.srv_turret_hud_forward  centimetres, 0..150, default 0: every vertex of the panels group (Group::kPanels: the two hologram families) moves forward along the turret by this much
//                               (seated -z), so the panels sit in front of the head as they did with the old camera move, without the camera moving for it.
//   experimental.srv_turret_radar_forward centimetres, 0..150, default 0: the same for the radar group (Group::kRadar), instead of the HUD-forward value, so it can stay near field and be seen
//                               from above (carried level with the head the disc is seen edge-on).
//   experimental.srv_turret_panels_back  centimetres, 0..200, default 110: the weapon panels (Group::kPanels: the two hologram families) go this much deeper where they lie within kPushSplitM of
//                               the anchor; vertices past 1 m are the systems-near window, which a flight did not see move the power panel. Part 2: a private pass-through geometry shader per output signature
//                               (srv_turret_hud_shader.h) around every issue of the families' draws; the rule is per vertex.
//   experimental.srv_turret_radar_back   centimetres, 0..200, default 0: the same for the radar (Group::kRadar), which the game lays on the turret deck right under the player.
//   experimental.srv_turret_hud_gap      centimetres, 0..200, default 0: the first build's forward pull, kept for comparison. For a family whose group has a push above 0 the push is
//                               used and this is not; for a group with no push this brings the vertices deeper than gap + kWindowNearM (up to kWindowFarM) forward by it.
//   experimental.srv_turret_hud_depth    front | game | real, default game: how the depth of a MOVED vertex is written (DepthMode below). front was the first default (Sean found it right in the flight of 45fa7e56).
//   experimental.srv_turret_hud_scissor  open | game, default open: whether the moved HUD may draw outside the scissor rectangle the game set for it at its OLD place. The game sets a scissor rectangle for
//                               the radar's draws from the radar's original screen bounds; the moved geometry was cropped to that stale rectangle (a straight horizontal edge across the disc and
//                               the hologram, which shifted as the head moved). open: while a bound draw is issued the rectangles are the whole render target (Scissor in
//                               srv_turret_hud_binding.h), the game's put back after; game keeps its rectangles.
//   experimental.srv_turret_systems_near centimetres, 0..200, default 80: meant for the vehicle power and systems panel. What it writes now is a second radial window on the two hologram
//                               families, vertices further than 1 m from the anchor and no further than 4 m, this much nearer along the ray. A flight at 40 cm did not move the panel. The Escape menu
//                               is a different mesh and is not bound: translating it locked the headset view. The weapon panels inside 1 m stay on panels_back. The reticle, past 4 m, stays. A pull
//                               reaches a vertex only when it is further than the pull plus 10 cm (and further than 1 m). 0 leaves the game's own depth.
//   experimental.srv_turret_radar_anchor on | reversed | off | camera, default camera (THE RADAR'S OWN CAMERA below). on: the radar's terrain schematic (the volume the disc's picture is drawn from, about
//                               0.44 m behind the eye at the stock camera, so the game drew nothing) moved forward by the radar-forward key IN THE GAME, along the turret's seated forward (on) or against it (reversed), put
//                               in the anchor's axes: a hook on its placement setter adds the offset to the translation the game sets (srv_radar_anchor.cpp, build 334925 only). While the placement holds that offset the
//                               two 5559BD94 billboard families are left to the game (the geometry shader does not move them); the other radar families keep its move. off: no move in
//                               the game, the geometry shader's move as before. camera: no move of the placement; the radar's schematic camera is moved back by the radar-forward key instead.
//
// THE TARGET HOLOGRAM (vs 5559BD94, the Scorpion's own hologram, signature H): its vertex shader writes __USER_VERTEX_M_LIGHTINGPOSITION, the vertex's camera-relative WORLD position
// (cb0[9..11] times the position), and its pixel shader uses it twice (a depth test against the scene and a view ray divided by the fragment's view depth). The geometry shader moves
// SV_Position but not that interpolant, so a hologram the game put beside or behind the eye had a view depth near 0 or negative, the ray blew up and the hologram vanished. cb1[277..279] of
// the same draw's vertex constants are the camera's RIGHT, UP and FORWARD unit vectors in world axes (they match the projection rows), so the shader adds the eye-space displacement of the
// move (right * dX + up * dY + forward * dZ) to the interpolant. The module binds the draw's own VS constant buffer at slot 1 to GS slot 1 for that signature (srv_turret_hud_binding.h).
//
// THE HOLO PANELS WITH "DISABLE GUI EFFECTS" OFF are vs 81216C77F90DEDD6 with ps A2965EC2931A39C8 or B4786E0A0B199285 instead of vs 1989E6D3 (issue 179). The dumped bytecode
// (vs_81216C77F90DEDD6.dxbc) writes TEXCOORD0 xyzw, TEXCOORD4 xyz, TEXCOORD6 xyz, TEXCOORD7 xyz, TEXCOORD8 xy and SV_POSITION at o5 from the same inputs, constant buffers and clip rows as
// vs 1989E6D3, and neither pixel shader reads SV_POSITION. They are signature 9 and the two families 81216C77.A2965EC2 and 81216C77.B4786E0A, in the panels group and moved as 1989E6D3 is
// (TEXCOORD6 is not touched).
//
// IN THIS FILE, in order: the keys and their limits; Part 1 (the Scorpion-only gate and the camera's seated z); Part 2 (the families, their output signatures and groups); the radar's anchor
// moved in the game (the setter's offset, the placement rule, the direction along the turret) and the radar's own camera (the slot rule, the camera move); THE MOVE (the shader's constants
// and its CPU copy); the anchor tracker; the eye's frame from the host's pose; the draw decision's refusal reasons. The scissor is srv_turret_hud_binding.h's.
#pragma once

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace edvr {
namespace srv_turret {

// ---- the keys -------------------------------------------------------------------------------------------------------------------------------------------------
inline constexpr int kBackDefaultCm = 80, kBackMaxCm = 150;   // experimental.srv_turret_camera_back
inline constexpr int kForwardDefaultCm = 0, kForwardMaxCm = 150;   // experimental.srv_turret_hud_forward
inline constexpr int kRadarForwardDefaultCm = 0, kRadarForwardMaxCm = 150;   // experimental.srv_turret_radar_forward
inline constexpr int kGapDefaultCm = 0, kGapMaxCm = 200;
inline constexpr int kPanelsDefaultCm = 0, kRadarDefaultCm = 0, kPushMaxCm = 200;   // experimental.srv_turret_panels_back and experimental.srv_turret_radar_back
inline constexpr int kSystemsNearDefaultCm = 80, kSystemsNearMaxCm = 200;   // experimental.srv_turret_systems_near: centimetres toward the camera for the gunner sheet (2016 indices) and the two cards (288), at game depth. 80 is the distance kept on 2026-11-03.
inline constexpr int kHudUpDefaultCm = 10, kHudUpMaxCm = 100;   // experimental.srv_turret_hud_up: centimetres up, along the seat, for the weapon panel (arcs, names, ammo, INFO). 10 is the raise kept on 2026-11-02.
inline constexpr int kGunnerUpDefaultCm = 35, kGunnerUpMaxCm = 100;   // experimental.srv_turret_gunner_up: centimetres up, along the seat, for the gunner sheet and its two cards. 35 is the raise kept on 2026-11-03.
inline constexpr int kCardsUpDefaultCm = 0, kCardsUpMaxCm = 100;   // experimental.srv_turret_cards_up: centimetres up, along the seat, for the target card and SYSTEMS only

// How the depth of a moved vertex is written (experimental.srv_turret_hud_depth). game: the vertex's own NDC z where it is a valid reversed-Z depth, else kFallbackNdcZ (the first builds' rule).
// real: the depth the moved position itself has, ndcZ = clamp(c.z / q.z, 0, kFallbackNdcZ) with the vertex's own clip z (Elite writes the constant 0.025, so this is 0.025 / depth).
// front: kFallbackNdcZ for every moved vertex.
enum class DepthMode : uint8_t { kGame = 0, kReal = 1, kFront = 2 };
// The key's default, as the module's literal and the shipped ini say it (the rig pins both): game, the value Sean flew with.
inline constexpr DepthMode kDepthDefault = DepthMode::kGame;
inline constexpr const char* kDepthDefaultText = "game";
inline constexpr const char* kScissorDefaultText = "game";   // experimental.srv_turret_hud_scissor: game (the other: open). game is the screen limit kept on 2026-11-02.
inline const char* depthModeName(DepthMode m) { return m == DepthMode::kReal ? "real" : m == DepthMode::kFront ? "front" : "game"; }
// The key's text; anything else is the default (the settings line says which it took).
inline DepthMode depthModeOf(const char* text) {
    if (text && !std::strcmp(text, "real")) return DepthMode::kReal;
    if (text && !std::strcmp(text, "front")) return DepthMode::kFront;
    return DepthMode::kGame;
}
// experimental.srv_turret_radar_anchor (on | reversed | off | camera, default camera): whether the radar's terrain schematic is moved forward IN THE GAME by the radar-forward key (srv_radar_anchor.cpp, build
// 334925 only), along the anchor's +Z (on) or -Z (reversed); off leaves the game's placement. RadarAnchorOffset below makes the offset. camera (the default, flown OK in flight e443b0c2): the placement
// is the game's own, and the radar's schematic CAMERA is moved back by the radar-forward key instead (THE RADAR'S OWN CAMERA below).
enum class RadarAnchorMode : uint8_t { kOn = 0, kReversed = 1, kOff = 2, kCamera = 3 };
inline constexpr RadarAnchorMode kRadarAnchorDefault = RadarAnchorMode::kOff;
inline constexpr const char* kRadarAnchorDefaultText = "off";
inline const char* radarAnchorModeName(RadarAnchorMode m) {
    return m == RadarAnchorMode::kReversed ? "reversed" : m == RadarAnchorMode::kOff ? "off" : m == RadarAnchorMode::kCamera ? "camera" : "on";
}
// The key's text; anything else is the default, camera (the settings line says which it took).
inline RadarAnchorMode radarAnchorModeOf(const char* text) {
    if (text && !std::strcmp(text, "on")) return RadarAnchorMode::kOn;
    if (text && !std::strcmp(text, "reversed")) return RadarAnchorMode::kReversed;
    if (text && !std::strcmp(text, "off")) return RadarAnchorMode::kOff;
    if (text && !std::strcmp(text, "camera")) return RadarAnchorMode::kCamera;
    return kRadarAnchorDefault;   // anything else, and nothing: the default (off)
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
// billboards and the sphere add their local offsets, scaled, to a translation column of the same eye's projection constants (b2 or b1 columns, no divide).
struct SigElement {
    const char* name;   // the semantic name as the game's shaders spell it (`__USER_...` for the radar and hologram families)
    uint32_t index;
    uint32_t comps;     // the components written (the mask's width)
};
inline constexpr uint32_t kMaxSigElements = 12;
struct Signature {
    const char* name;            // the first family's vertex shader's eight hex digits: how the log names the geometry shader
    uint32_t count;              // the elements, SV_Position the last
    SigElement el[kMaxSigElements];
    bool psReadsPosition;        // the family's pixel shader also reads SV_POSITION.xy
};
inline constexpr uint32_t kSignatureCount = 11;
inline constexpr Signature kSignatures[kSignatureCount] = {
    {"1989E6D3", 3, {{"TEXCOORD", 0, 4}, {"TEXCOORD", 6, 3}, {"SV_Position", 0, 4}}, false},
    {"2CECEC30", 3, {{"TEXCOORD", 0, 4}, {"TEXCOORD", 7, 2}, {"SV_Position", 0, 4}}, true},
    {"DF3503CD", 2, {{"__USER_VERTEX_M_TEXCOORD", 0, 2}, {"SV_Position", 0, 4}}, false},
    {"44DDCDEB", 4, {{"__USER_RADAR_LOCALKEY_COLOUR", 0, 4}, {"__USER_RADAR_LOCALKEY_POSITIONOFFSET", 0, 4}, {"__USER_VERTEX_M_TEXCOORD", 0, 1}, {"SV_Position", 0, 4}}, true},
    {"9B34C331", 3, {{"__USER_RADAR_LOCALKEY_COLOUR", 0, 4}, {"__USER_RADAR_LOCALKEY_POSITIONOFFSET", 0, 3}, {"SV_Position", 0, 4}}, false},
    {"9611A454", 3, {{"__USER_RADAR_LOCALKEY_COLOUR", 0, 3}, {"__USER_RADAR_LOCALKEY_POSITIONOFFSET", 0, 2}, {"SV_Position", 0, 4}}, false},
    {"B932058F", 2, {{"__USER_RADAR_LOCALKEY_COLOUR", 0, 3}, {"SV_Position", 0, 4}}, false},
    {"5559BD94", 3, {{"__USER_VERTEX_M_LIGHTINGPOSITION", 0, 3}, {"__USER_VERTEX_M_TEXCOORD", 0, 2}, {"SV_Position", 0, 4}}, true},
    {"5DAD41AA", 2, {{"TEXCOORD", 0, 4}, {"SV_Position", 0, 4}}, false},
    {"81216C77", 6, {{"TEXCOORD", 0, 4}, {"TEXCOORD", 4, 3}, {"TEXCOORD", 6, 3}, {"TEXCOORD", 7, 3}, {"TEXCOORD", 8, 2}, {"SV_Position", 0, 4}}, false},
    {"A888D510", 6, {{"TEXCOORD", 0, 4}, {"TEXCOORD", 2, 3}, {"TEXCOORD", 4, 3}, {"TEXCOORD", 5, 3}, {"TEXCOORD", 6, 2}, {"SV_Position", 0, 4}}, false},
};
// The floats of one vertex as the vertex shader writes it (the signature), and where the clip position, the last element, starts in it. The geometry shader writes the signature and then one
// more element, `pre` (kPreSemantic, 4 floats): the clip position as the game made it, before the move. The pixel shaders do not read it and nothing in the module reads it back.
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
    uint32_t strideBytes;  // the geometry shader's output record ((sigFloats + kPreFloats) * 4)
    uint32_t posFloat;     // where SV_POSITION starts in it, in floats
    uint32_t preFloat;     // where `pre` (the clip position before the move) starts in it
    Group group;           // the push-back key that governs the family
};
constexpr Family makeFamily(uint64_t vs, uint64_t ps, const char* name, uint32_t sig, Group group) {
    return Family{vs, ps, name, sig, (sigFloats(sig) + kPreFloats) * 4, sigPosFloat(sig), sigFloats(sig), group};
}
inline constexpr uint32_t kFamilyCount = 11;
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
    makeFamily(0x81216C77F90DEDD6ull, 0xA2965EC2931A39C8ull, "81216C77.A2965EC2", 9, Group::kPanels),
    makeFamily(0x81216C77F90DEDD6ull, 0xB4786E0A0B199285ull, "81216C77.B4786E0A", 9, Group::kPanels),
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
              "the geometry shader's output records: (4 + 3 + 4 + 4), (4 + 2 + 4 + 4) and the target hologram sphere's (3 + 2 + 4 + 4) floats");
static_assert(kFamilies[9].strideBytes == 92 && kFamilies[9].posFloat == 15 && kFamilies[9].preFloat == 19 && kFamilies[10].strideBytes == 92 && kFamilies[10].posFloat == 15 && kFamilies[10].preFloat == 19,
              "the Disable-GUI-effects-off panels' output record: (4 + 3 + 3 + 3 + 2 + 4 + 4) floats");
static_assert(kFamilies[0].group == Group::kPanels && kFamilies[1].group == Group::kPanels && kFamilies[2].group == Group::kRadar && kFamilies[8].group == Group::kRadar &&
                  kFamilies[9].group == Group::kPanels && kFamilies[10].group == Group::kPanels,
              "the hologram families (1989E6D3, 2CECEC30 and the two 81216C77 pairs) are the panels, every other family is the radar");
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

// ---- the radar anchor moved in the game (experimental.srv_turret_radar_anchor; srv_radar_anchor.cpp) ----------------------------------------------------------------------------------
// The published offset: (0, 0, s * F) metres in the anchor entity's own axes, F the radar-forward key, s +1 for on and -1 for reversed (flown 2026-10-31: on is forward, about 1:1). The
// hook adds s * F along the turret's forward instead when it has u and the anchor's rotation (radarAnchorAlong below), and this is its fallback. All bits zero (never a -0.0) unless
// the Scorpion's turret view is on, the key is on or reversed and F is above 0 (camera mode publishes zero: the existing restore gives the game its own placement back).
struct RadarAnchorOffset {
    float m[3] = {0.0f, 0.0f, 0.0f};
    uint32_t bits[3] = {0u, 0u, 0u};
    bool zero() const { return (bits[0] | bits[1] | bits[2]) == 0u; }
};
inline RadarAnchorOffset radarAnchorOffset(bool on, RadarAnchorMode mode, int radarForwardCm) {
    RadarAnchorOffset o;
    if (!on || mode == RadarAnchorMode::kOff || mode == RadarAnchorMode::kCamera || radarForwardCm <= 0) return o;
    const int cm = radarForwardCm > kRadarForwardMaxCm ? kRadarForwardMaxCm : radarForwardCm;
    o.m[2] = static_cast<float>(cm) / 100.0f * (mode == RadarAnchorMode::kReversed ? -1.0f : 1.0f);
    std::memcpy(&o.bits[2], &o.m[2], sizeof(float));
    return o;
}
// ---- THE RADAR'S OWN CAMERA (experimental.srv_turret_radar_anchor = camera; srv_radar_anchor.cpp, the slot hook; flight fe0b2560) ------------------------------------------------------------
// Camera mode leaves the schematic's placement to the game and hands each of the radar's schematic slots (the terrain disc's and the Scorpion icon's, in both lists: both eyes) a private
// copy of the camera moved BACK by D, the radar-forward key, along the turret's forward, at the slot update's entry (0x36BF3C0 in build 334925;
// analysis\decomp\verify_20261101_srv_radar_slot_camera.txt). The slot's gate, window, quad and the picture's own camera all come from that camera and nothing else, so the schematic, about 0.44 m behind the
// eye at the game's camera, is about D - 0.44 m in front of the picture's camera, which follows the camera every frame. The radar's own picture billboards then move forward by D through the regular
// radar path (srv_turret.cpp), with the rest of the radar.
// The camera object: 0x2F0 bytes of float rows (row-vector convention). World at +0x00 (rows x, y, z axes; row 3, the position, at +0x30), the frustum block's two plane-array
// pointers at +0x70 and +0x78 (into its own inline blocks at +0x90, 6 x 16 bytes, and +0xF0, 8 x 16 bytes), view at +0x170 (row 3 at +0x1A0), projection +0x1B0 (position-free), VP
// +0x1F0, dirty flags +0x230 (bit 3: VP, which the game rebuilds as view x projection before it reads it).
inline constexpr size_t kCamBytes = 0x2F0;
inline constexpr size_t kCamForwardAt = 0x20, kCamPositionAt = 0x30, kCamViewAt = 0x170, kCamFlagsAt = 0x230;
inline constexpr size_t kCamPlanePtrAt[2] = {0x70, 0x78}, kCamInlineAt[2] = {0x90, 0xF0}, kCamInlineBytes[2] = {6 * 16, 8 * 16};
inline constexpr uint32_t kCamVpDirty = 0x8;
inline constexpr double kTurretSampleFreshS = 0.100;   // the turret's forward is taken from a CPU camera sample written at most this long ago

// The back distance the slot hook is given: D = the radar-forward key in metres (capped as the setter's F is) while the Scorpion's turret view is on in camera mode; else 0 (no move).
inline float radarCameraBackM(bool on, RadarAnchorMode mode, int radarForwardCm) {
    if (!on || mode != RadarAnchorMode::kCamera || radarForwardCm <= 0) return 0.0f;
    return static_cast<float>(radarForwardCm > kRadarForwardMaxCm ? kRadarForwardMaxCm : radarForwardCm) / 100.0f;
}

// The private copy's plane-array pointers: one that points into either of the source camera's inline blocks is moved to the same place in the copy's; any other is kept (the game only
// reads through them).
inline void cameraRebase(uint8_t* copy, uintptr_t source) {
    const uintptr_t to = reinterpret_cast<uintptr_t>(copy);
    for (const size_t at : kCamPlanePtrAt) {
        uintptr_t p = 0;
        std::memcpy(&p, copy + at, sizeof(p));
        for (int b = 0; b < 2; ++b) {
            const uintptr_t lo = source + kCamInlineAt[b];
            if (p >= lo && p - lo < kCamInlineBytes[b]) {
                p = to + kCamInlineAt[b] + (p - lo);
                std::memcpy(copy + at, &p, sizeof(p));
                break;
            }
        }
    }
}

// The camera moved by the world offset d: its position += d; its view's row 3 -= d.x view row 0 + d.y view row 1 + d.z view row 2 (the columns of the view's 3x3 are the world's axes,
// so this is the view the game would derive from the moved position, and for d = -D times the camera's forward it is view row 3's z + D); and VP marked dirty, so the game rebuilds it
// from the edited view and the unchanged projection.
inline void cameraMoveBy(uint8_t* cam, const float d[3]) {
    float pos[3], view[16];
    std::memcpy(pos, cam + kCamPositionAt, sizeof(pos));
    std::memcpy(view, cam + kCamViewAt, sizeof(view));
    for (int i = 0; i < 3; ++i) pos[i] += d[i];
    for (int j = 0; j < 3; ++j) view[12 + j] -= d[0] * view[j] + d[1] * view[4 + j] + d[2] * view[8 + j];
    uint32_t flags = 0;
    std::memcpy(&flags, cam + kCamFlagsAt, sizeof(flags));
    flags |= kCamVpDirty;
    std::memcpy(cam + kCamPositionAt, pos, sizeof(pos));
    std::memcpy(cam + kCamViewAt + 12 * sizeof(float), view + 12, 3 * sizeof(float));
    std::memcpy(cam + kCamFlagsAt, &flags, sizeof(flags));
}

// Where the slot camera's move comes from: the turret's forward (the CPU camera sample), the camera's own forward (its world row 2; +z is forward, INFERRED from the projection's
// w = z_view), or no move.
enum class CameraBackFrom : uint8_t { kNone = 0, kTurret = 1, kCamera = 2 };
// The world offset the radar's slot camera is moved by: d = -D * u, u the turret's forward in the world while its sample was written at most kTurretSampleFreshS ago (age and rate in QPC
// ticks), else the camera's own forward, each made unit length. D not above 0 or not finite, or no usable direction: no move (d zero).
inline CameraBackFrom radarCameraBack(float backM, bool haveU, int64_t uAgeTicks, int64_t ticksPerSecond, const float* u, const float* camForward, float d[3]) {
    d[0] = d[1] = d[2] = 0.0f;
    if (!(backM > 0.0f) || !std::isfinite(backM)) return CameraBackFrom::kNone;
    const auto unit = [](const float* v, float out[3]) {
        if (!v) return false;
        const double n = std::sqrt(double(v[0]) * v[0] + double(v[1]) * v[1] + double(v[2]) * v[2]);
        if (!(n > 1.0e-6) || !std::isfinite(n)) return false;
        for (int i = 0; i < 3; ++i) out[i] = static_cast<float>(v[i] / n);
        return true;
    };
    const bool fresh = haveU && ticksPerSecond > 0 && uAgeTicks >= 0 && double(uAgeTicks) <= kTurretSampleFreshS * double(ticksPerSecond);
    float dir[3];
    CameraBackFrom from = CameraBackFrom::kNone;
    if (fresh && unit(u, dir))
        from = CameraBackFrom::kTurret;
    else if (unit(camForward, dir))
        from = CameraBackFrom::kCamera;
    else
        return CameraBackFrom::kNone;
    for (int i = 0; i < 3; ++i) d[i] = -backM * dir[i];
    return from;
}

// THE RADAR'S SLOTS (0x36BF3C0's RCX). A slot is the radar's when one of the components registered at its index has its link (+0x388) on an object of the radar's interface vtable
// (0x5511F18): the slot's vtable; its index (+0x258) and manager (+0x280); the manager's per-index component lists (+0xB0, 0x20 bytes each, +0xA8 of them), each a count (+0x10) and an
// array of component pointers (+0x18); a component's placement mode at *(+0x50)+0x74 (4 the terrain host, 5 the vehicle icon); the slot's list at +0x288 (0 A, 2 B). Mirror slots
// share index and manager, so both lists (both eyes) are caught. Read through `read` (SEH-guarded in the hook; a fake memory in the rig): 1 the radar's (out filled), 0 not, -1 a read
// failed.
using MemRead = bool (*)(void* ctx, uintptr_t address, void* out, size_t bytes);
inline constexpr size_t kSlotIndexAt = 0x258, kSlotManagerAt = 0x280, kSlotListAt = 0x288, kManagerCountAt = 0xA8, kManagerListsAt = 0xB0, kManagerEntryBytes = 0x20, kEntryCountAt = 0x10,
                        kEntryDataAt = 0x18, kComponentLinkAt = 0x388, kComponentDescAt = 0x50, kDescModeAt = 0x74;
inline constexpr uint64_t kSlotComponentsMax = 64;
inline constexpr uint32_t kRadarModeTerrain = 4, kRadarModeIcon = 5;
struct RadarSlot {
    uint32_t index = 0, mode = 0, list = 0;
};
inline int radarSlotTest(uintptr_t slot, uintptr_t slotVtable, uintptr_t linkVtable, MemRead read, void* ctx, RadarSlot& out) {
    uintptr_t vtable = 0, manager = 0, lists = 0, data = 0;
    uint32_t index = 0;
    uint64_t count = 0, n = 0;
    if (!read(ctx, slot, &vtable, sizeof(vtable))) return -1;
    if (vtable != slotVtable) return 0;
    if (!read(ctx, slot + kSlotIndexAt, &index, sizeof(index)) || !read(ctx, slot + kSlotManagerAt, &manager, sizeof(manager))) return -1;
    if (!manager) return 0;
    if (!read(ctx, manager + kManagerCountAt, &count, sizeof(count))) return -1;
    if (index >= count) return 0;
    if (!read(ctx, manager + kManagerListsAt, &lists, sizeof(lists))) return -1;
    const uintptr_t entry = lists + uintptr_t(index) * kManagerEntryBytes;
    if (!read(ctx, entry + kEntryCountAt, &n, sizeof(n)) || !read(ctx, entry + kEntryDataAt, &data, sizeof(data))) return -1;
    if (n > kSlotComponentsMax) return 0;
    for (uint64_t i = 0; i < n; ++i) {
        uintptr_t component = 0, link = 0, linkVt = 0, desc = 0;
        if (!read(ctx, data + uintptr_t(i) * sizeof(uintptr_t), &component, sizeof(component))) return -1;
        if (!component) continue;
        if (!read(ctx, component + kComponentLinkAt, &link, sizeof(link))) return -1;
        if (!link) continue;
        if (!read(ctx, link, &linkVt, sizeof(linkVt))) return -1;
        if (linkVt != linkVtable) continue;
        if (!read(ctx, component + kComponentDescAt, &desc, sizeof(desc)) || !read(ctx, desc + kDescModeAt, &out.mode, sizeof(out.mode)) ||
            !read(ctx, slot + kSlotListAt, &out.list, sizeof(out.list)))
            return -1;
        out.index = index;
        return 1;
    }
    return 0;
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
// the CPU when the game writes them, and u is paired with the anchor's rotation OF THE SAME FRAME (the host's frame sequence; SeqRing below), so the vehicle's own turning adds no error.
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
// The head's pitch in the seat from eye 0's seated rotation R (row-major, OpenVR axes): the eye's forward is minus R's third column. It goes with the direction sample to the hook, which does
// not record it.
inline double headPitchDeg(const double R[9]) {
    double y = -R[5];
    y = y > 1.0 ? 1.0 : y < -1.0 ? -1.0 : y;
    return std::asin(y) * 57.29577951308232;
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
// ones the game projects with, the eye jitter included), translates it forward by T (experimental.srv_turret_hud_forward, for EVERY vertex of every family), and then moves it along its ray from
// a FIXED ANCHOR A (the head's seated position when the turret view came on, taken again on a recentre, when a camera key changes and when the head stays far from it; the eye's view of it is A_eye), judged on the
// distance of the translated vertex from A:
//     P1 = P + T;   V = P1 - A;   d = |V|;   if (dMin < d < dMax)   Q = A + V (d - gap) / d
//     else if (gap2 != 0 and dMin2 < d < dMax2)   Q = A + V (d - gap2) / d   else   Q = P1;   reproject Q, for any Q.z (the reprojection is linear in Q)
// The second window is experimental.srv_turret_systems_near on the panels group. It is not confirmed as the power panel. The first window wins when a vertex is in both.
// gap < 0 is the push-back (deeper), gap > 0 the pull (nearer). The result keeps the NDC z of the vertex (z' = ndcZ w') when that is a valid depth, so the game's own depth values stand;
// otherwise ndcZ = kFallbackNdcZ. Elite is REVERSED Z with an infinite far plane (cb0[6] = (0, 0, 0, 0.025): clip z is the constant 0.025, NDC z = 0.025 / w): a vertex the game had
// behind the eye (w < 0) or inside its 2.5 cm near plane (0 < w < 0.025, z / w above 1) has no valid depth, and the moved vertex would fail the rasterizer's depth clip (0 <= z <= w) and
// be cut; 0.999 is the NEAREST valid reversed-Z depth (1 is near), so such a vertex draws in front, as the HUD does. From the anchor the
// moved vertex is seen in the direction it was (a push keeps the panel's size on screen) and from anywhere else it is a real panel at the new distance: leaning toward it makes it
// larger and moves it by the parallax of its new depth, not the old one's.
// THE PULL (experimental.srv_turret_hud_gap): vertices whose distance from the anchor lies between dMin = gap + kWindowNearM and dMax = kWindowFarM move `gap` metres nearer.
inline constexpr float kWindowNearM = 0.10f;
inline constexpr float kWindowFarM = 4.0f;
// THE PUSH (experimental.srv_turret_panels_back, experimental.srv_turret_radar_back): vertices whose distance from the anchor lies between kPushNearM and kPushSplitM move `back` metres DEEPER (a negative
// gap). 2 cm keeps a vertex on the anchor itself (no ray) out; 1 m splits the weapon panels and the radar (0.1 to 0.7 m from the head) from the larger panels (1.3 m and deeper).
inline constexpr float kPushNearM = 0.02f;
inline constexpr float kPushSplitM = 1.0f;
inline constexpr float kFallbackNdcZ = 0.999f; // the depth of a moved vertex whose own NDC z is not valid (behind the eye, or inside the near plane): the nearest valid reversed-Z depth
inline constexpr float kPlaneEpsM = 1.0e-6f;  // a vertex on the eye plane (clip w about 0) has no ray to follow: left alone
inline constexpr float kAnchorLimitM = 2.0f;  // the constants accept an anchor, a translation and a gap up to this far (the keys' 200 cm, 150 cm)
// THE CONTENT DEPTH and THE LOCK are two branches of the geometry shader (srv_turret_hud_shader.h) that the module does not use: it writes invZc and lock as 0, so the shader moves every vertex by the
// translation as it is. With invZc = 1 / z_c above 0 the shader scales the translation by w / z_c (w the vertex's clip w), and anchoredMove below has that copy; with lock 1 the shader moves a
// triangle's centre onto the target in tx, ty, tz, and anchoredMove has no copy of it.
inline constexpr float kContentInvZMax = 20.0f;   // z_c of 5 cm or more

// The geometry shader's constant buffer (GS slot 0, which the game never binds): eight registers.
//   [0] gap, dMin, dMax, depth       the near move: metres nearer (negative: deeper), and the window of anchor distances it applies to; the DepthMode (0 game, 1 real, 2 front)
//   [1] l, r, dn, up                 the eye's projection tangents as the game projects (left and down negative; the jitter shift is added)
//   [2] ax, ay, az, lock             the anchor in the eye's space: X right, Y up, Z forward (= clip w); lock: THE LOCK above, 0 as the module writes it
//   [3] tx, ty, tz, invZc            the translation (HUD forward) in the same axes; invZc: 1 / the content depth z_c (THE CONTENT DEPTH above), 0 as the module writes it
//   [4] gap2, dMin2, dMax2, 0        a second radial window. w stays 0.
//   [5..7] rows of seat-from-math-eye. Identity except the gunner sheet's border test.
struct TurretParams {
    float gap = 0.0f, dMin = 0.0f, dMax = 0.0f, depth = 0.0f;   // depth: the DepthMode as 0, 1 or 2
    float l = 0.0f, r = 0.0f, dn = 0.0f, up = 0.0f;
    float ax = 0.0f, ay = 0.0f, az = 0.0f, lock = 0.0f;
    float tx = 0.0f, ty = 0.0f, tz = 0.0f, invZc = 0.0f;
    float gap2 = 0.0f, dMin2 = 0.0f, dMax2 = 0.0f, pad2 = 0.0f;   // the old systems pull; pad2 stays 0
    float s0x = 1.0f, s0y = 0.0f, s0z = 0.0f, s0w = 0.0f;   // row 0 of seat-from-math-eye. The gunner sheet uses it to tell the outer border from the card brackets.
    float s1x = 0.0f, s1y = 1.0f, s1z = 0.0f, s1w = 0.0f;
    float s2x = 0.0f, s2y = 0.0f, s2z = 1.0f, s2w = 0.0f;
    bool operator==(const TurretParams& o) const { return std::memcmp(this, &o, sizeof(TurretParams)) == 0; }
};
static_assert(sizeof(TurretParams) == 128, "the turret shader's cbuffer is eight 16-byte registers");

// One eye's side of the constants for a frame: its tangents (jitter included), the anchor and the HUD-forward translation, both in the eye's space (X right, Y up, Z forward).
struct EyeFrame {
    float l = 0.0f, r = 0.0f, dn = 0.0f, up = 0.0f;
    float a[3] = {0.0f, 0.0f, 0.0f};
    float t[3] = {0.0f, 0.0f, 0.0f};        // the panels group's translation: HUD forward, plus the weapon panel's seat-up (experimental.srv_turret_hud_up)
    float tRadar[3] = {0.0f, 0.0f, 0.0f};   // the radar group's (experimental.srv_turret_radar_forward)
    float tNear[3] = {0.0f, 0.0f, 0.0f};    // the gunner panel: toward the camera (experimental.srv_turret_systems_near) plus its seat-up (experimental.srv_turret_gunner_up)
    float tCards[3] = {0.0f, 0.0f, 0.0f};   // extra seat-up for the two cards only (experimental.srv_turret_cards_up). The curved sheet does not get this.
    float seatR[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};   // row-major seat-from-math-eye, so the sheet's border test does not follow the head
    float eyeSeat[3] = {0, 0, 0};                   // the eye's origin in the seat. Added so leaning does not slide the card test.
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

// The curved game panel's pull (experimental.srv_turret_systems_near), on constants that already carry the group's near window. Positive centimetres become metres nearer. The window starts at
// 1 m or at the pull plus 10 cm, whichever is further, and ends at 4 m, so a vertex the weapon push already owns (inside 1 m) is not also pulled, and d - gap2 stays positive.
inline void applySystemsNear(TurretParams& p, int nearCm) {
    if (nearCm <= 0) {
        p.gap2 = p.dMin2 = p.dMax2 = p.pad2 = 0.0f;
        return;
    }
    const int cm = nearCm > kSystemsNearMaxCm ? kSystemsNearMaxCm : nearCm;
    p.gap2 = static_cast<float>(cm) / 100.0f;
    const float margin = p.gap2 + kWindowNearM;
    p.dMin2 = margin > kPushSplitM ? margin : kPushSplitM;
    p.dMax2 = kWindowFarM;
    p.pad2 = 0.0f;
}

// Which constants a group's draws run with, from its push key, the gap key and its forward key (the panels' forward key is experimental.srv_turret_hud_forward, the radar's
// experimental.srv_turret_radar_forward): the push when above 0, else the pull when the gap is above 0, else the translation alone when the group's forward key is above 0, else none
// (false: the draw goes through as the game made it). experimental.srv_turret_systems_near is not one of these windows. It moves the gunner panel on its own draws (gunnerParamsFor).
// hudUpCm opens the panels group when it is the only panels key above 0; the metres are already in e.t (eyeFrameFrom). The radar group ignores it.
inline bool paramsForGroup(const EyeFrame& e, Group group, int pushCm, int gapCm, int forwardCm, TurretParams& out, int systemsNearCm = 0, int hudUpCm = 0) {
    (void)systemsNearCm;
    if (pushCm > 0)
        out = pushParamsFor(e, static_cast<float>(pushCm > kPushMaxCm ? kPushMaxCm : pushCm) / 100.0f, group);
    else if (gapCm > 0)
        out = paramsFor(e, static_cast<float>(gapCm > kGapMaxCm ? kGapMaxCm : gapCm) / 100.0f, group);
    else if (forwardCm > 0 || (group == Group::kPanels && hudUpCm > 0))
        out = translateParamsFor(e, group);
    else
        return false;
    return true;
}

// The gunner camera's curved panel (dump 050105383001). The sheet is vs 5DAD41AA at 2016 indices. The two cards on it, CASPIAN EXPLORER and SYSTEMS, are vs C4B4B334 at 288 indices.
// The Escape menu is the 360-index draws of those shaders and is not this. The centre reticle is vs E5086486 and is not this. Positive centimetres move the whole panel toward the camera
// along the turret, the same axis as the HUD-forward key. The sheet is then unwrapped off its left-right curve onto one plane just in front of the launcher, so a
// horizontal line on that curve stays the game's. The curved sheet is not reshaped. experimental.srv_turret_cards_up raises only the two cards.
// With Elite's "Disable GUI effects" OFF the same 2016-index sheet is drawn by vs A888D51024D9798E (ps 9107E72CB016CC02), the menu panel's vertex shader (issue 179). The dumps (vs_A888D51024D9798E.dxbc
// and vs_5DAD41AA66757CE3.dxbc) show the same unpack of the packed vertex, the same instance record and the same clip rows (cb0[4..7] against the position minus cb1[275]), so the sheet's
// geometry shader inverts them alike; the 2016-index count and the turret gates are what tell it from the menu panel's other draws (6 indices in the dump).
inline constexpr uint64_t kGunnerSheetVs = 0x5DAD41AA66757CE3ull;
inline constexpr uint64_t kGunnerSheetAltVs = 0xA888D51024D9798Eull;
inline constexpr uint64_t kGunnerCardsVs = 0xC4B4B334B26E81A9ull;
inline constexpr uint32_t kGunnerSheetIndices = 2016;
inline constexpr uint32_t kGunnerCardsIndices = 288;
inline constexpr int kGunnerCardsSignature = 1;
inline constexpr int kGunnerSheetSignature = 8;
inline constexpr int kGunnerSheetAltSignature = 10;   // the sheet with Disable GUI effects off (K): the same geometry shader body as signature 8 (I)
static_assert(kGunnerCardsSignature == 1 && kGunnerSheetSignature == 8 && kGunnerSheetAltSignature + 1 == static_cast<int>(kSignatureCount) && kSignatures[kGunnerCardsSignature].el[1].index == 7 &&
                  kSignatures[kGunnerSheetSignature].count == 2 && kSignatures[kGunnerSheetAltSignature].count == 6,
              "the gunner cards use the hologram signature B, and the sheet is signature 8 (I) or, with Disable GUI effects off, the last (K)");
constexpr bool isGunnerSheetSignature(int sig) { return sig == kGunnerSheetSignature || sig == kGunnerSheetAltSignature; }
inline int gunnerSignature(uint64_t vs, char kind, uint32_t indexCount) {
    if (kind != 'I' && kind != 'X') return -1;
    if (indexCount != kGunnerSheetIndices && indexCount != kGunnerCardsIndices) return -1;   // the index count first: nearly every draw ends here
    if (vs == kGunnerCardsVs && indexCount == kGunnerCardsIndices) return kGunnerCardsSignature;
    if (vs == kGunnerSheetVs && indexCount == kGunnerSheetIndices) return kGunnerSheetSignature;
    if (vs == kGunnerSheetAltVs && indexCount == kGunnerSheetIndices) return kGunnerSheetAltSignature;
    return -1;
}
inline int forwardCmOf(Group g, int hudForwardCm, int radarForwardCm) { return g == Group::kPanels ? hudForwardCm : radarForwardCm; }
inline int pushCmOf(Group g, int panelsCm, int radarCm) { return g == Group::kPanels ? panelsCm : radarCm; }

// The constants a shader may be given (the binding's validity test, written here so the rig can run it): all finite; |gap| and |gap2| <= 2 m; a window that moves starts above its gap and above 0
// (dMax > dMin > max(gap, 0)), so the divisor d - gap is positive; an empty near window is allowed only while the systems pull (gap2) is the move; r > l and up > dn; the anchor and the
// translation within 2 m; the depth mode one of 0, 1 and 2; 1 / the content depth in [0, 20] (z_c of 5 cm or more, or none); the lock 0 or 1; the systems register's pad 0; and a move that
// does something (a gap, a translation, or the systems pull).
inline bool paramsUsable(const TurretParams& p) {
    const float all[] = {p.gap, p.dMin, p.dMax, p.depth, p.l, p.r, p.dn, p.up, p.ax, p.ay, p.az, p.lock, p.tx, p.ty, p.tz, p.invZc, p.gap2, p.dMin2, p.dMax2, p.pad2,
                         p.s0x, p.s0y, p.s0z, p.s0w, p.s1x, p.s1y, p.s1z, p.s1w, p.s2x, p.s2y, p.s2z, p.s2w};
    for (float f : all)
        if (!std::isfinite(f)) return false;
    if (!(p.invZc >= 0.0f && p.invZc <= kContentInvZMax)) return false;
    if (p.lock != 0.0f && p.lock != 1.0f) return false;
    if (p.depth != 0.0f && p.depth != 1.0f && p.depth != 2.0f) return false;
    if (p.pad2 != 0.0f) return false;
    if (!(std::fabs(p.gap) <= kAnchorLimitM) || !(std::fabs(p.gap2) <= kAnchorLimitM)) return false;
    const bool farMoves = p.gap2 != 0.0f;
    const bool nearIdle = p.gap == 0.0f && !(p.dMax > p.dMin);   // the systems pull alone: the near window matches nothing
    if (!nearIdle && (!(p.dMin > p.gap) || !(p.dMin > 0.0f) || !(p.dMax > p.dMin))) return false;
    if (farMoves && (!(p.dMin2 > p.gap2) || !(p.dMin2 > 0.0f) || !(p.dMax2 > p.dMin2))) return false;
    if (!(p.r > p.l) || !(p.up > p.dn)) return false;
    const double a2 = double(p.ax) * p.ax + double(p.ay) * p.ay + double(p.az) * p.az, t2 = double(p.tx) * p.tx + double(p.ty) * p.ty + double(p.tz) * p.tz;
    if (!(a2 < double(kAnchorLimitM) * kAnchorLimitM) || !(t2 <= double(kAnchorLimitM) * kAnchorLimitM)) return false;
    return p.gap != 0.0f || t2 > 0.0 || farMoves;
}

// After paramsUsable: a translation toward the camera, at the game's own depth. The curved sheet is not reshaped. cards adds the cards-only seat-up.
inline bool gunnerParamsFor(const EyeFrame& e, TurretParams& out, bool cards = false) {
    const float tx = e.tNear[0] + (cards ? e.tCards[0] : 0.0f);
    const float ty = e.tNear[1] + (cards ? e.tCards[1] : 0.0f);
    const float tz = e.tNear[2] + (cards ? e.tCards[2] : 0.0f);
    const double t2 = double(tx) * tx + double(ty) * ty + double(tz) * tz;
    if (!(t2 > 0.0)) return false;
    out = makeParams(e, 0.0f, 0.0f, 0.0f);
    out.tx = tx;
    out.ty = ty;
    out.tz = tz;
    out.depth = 0.0f;
    out.gap2 = out.dMin2 = out.dMax2 = out.pad2 = 0.0f;
    out.s0x = e.seatR[0]; out.s0y = e.seatR[1]; out.s0z = e.seatR[2]; out.s0w = e.eyeSeat[0];
    out.s1x = e.seatR[3]; out.s1y = e.seatR[4]; out.s1z = e.seatR[5]; out.s1w = e.eyeSeat[1];
    out.s2x = e.seatR[6]; out.s2y = e.seatR[7]; out.s2z = e.seatR[8]; out.s2w = e.eyeSeat[2];
    return paramsUsable(out);
}

// Whether a vertex's own NDC depth (clip z / w) is a valid one for Elite's reversed Z: w > 0 and 0 <= z / w <= 1. A vertex the game put behind the eye, or inside the 2.5 cm near plane, is not.
template <class T>
inline bool ndcDepthValid(T z, T w) {
    if (!(w > T(0))) return false;
    const T n = z / w;
    return n >= T(0) && n <= T(1);
}

// The move on the CPU: the shader's rule written again for the rig (T is float or double). `in` is the vertex's clip position {x, y, z, w}; `out` receives the clip
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
    const bool farWindow = !inWindow && p.gap2 != 0.0f && d > T(p.dMin2) && d < T(p.dMax2);   // the systems-near window; the near window wins when both match
    const bool translated = p.tx != 0.0f || p.ty != 0.0f || p.tz != 0.0f;
    if (!translated && !inWindow && !farWindow) return false;
    T Q[3] = {P1[0], P1[1], P1[2]};
    if (inWindow || farWindow) {
        const T k = (d - (inWindow ? T(p.gap) : T(p.gap2))) / d;
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

// ---- the anchor ---------------------------------------------------------------------------------------------------------------------------------------------------------
// The fixed point in the seated frame the move is made about: the head's position when the turret view came on. It is taken again (and the module says so in the log, once each) when
// the turret view comes on, when experimental.srv_turret_camera_back, experimental.srv_turret_hud_forward or experimental.srv_turret_radar_forward changes, when the head jumps more than kRecentreJumpM between two frames (a recentre), and when
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
// A translation longer than the shader accepts (2 m) is shortened along itself, so a forward move and an up move together still run.
inline void limitTranslation(float t[3]) {
    const double l2 = double(t[0]) * t[0] + double(t[1]) * t[1] + double(t[2]) * t[2];
    const double lim2 = double(kAnchorLimitM) * kAnchorLimitM;
    if (!(l2 > lim2)) return;
    const float s = static_cast<float>(double(kAnchorLimitM) / std::sqrt(l2));
    t[0] *= s;
    t[1] *= s;
    t[2] *= s;
}

inline bool eyeFrameFrom(const float head[12], const float eyeToHead[12], const double anchor[3], double forwardM, double radarForwardM, const float tangents[4], EyeFrame& out, double nearM = 0.0, double hudUpM = 0.0, double gunnerUpM = 0.0, double cardsUpM = 0.0) {
    for (int i = 0; i < 12; ++i)
        if (!std::isfinite(head[i]) || !std::isfinite(eyeToHead[i])) return false;
    for (int i = 0; i < 4; ++i)
        if (!std::isfinite(tangents[i])) return false;
    if (!(tangents[1] > tangents[0]) || !(tangents[3] > tangents[2]) || !std::isfinite(forwardM) || !std::isfinite(radarForwardM) || !std::isfinite(nearM) || !std::isfinite(hudUpM) || !std::isfinite(gunnerUpM) || !std::isfinite(cardsUpM)) return false;
    double R[3][3], E[3];
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            R[i][j] = 0.0;
            for (int k = 0; k < 3; ++k) R[i][j] += double(head[i * 4 + k]) * eyeToHead[k * 4 + j];
            out.seatR[i * 3 + j] = static_cast<float>(R[i][j]);
        }
        E[i] = head[i * 4 + 3];
        for (int k = 0; k < 3; ++k) E[i] += double(head[i * 4 + k]) * eyeToHead[k * 4 + 3];
        out.eyeSeat[i] = static_cast<float>(E[i]);
    }
    double v[3], tv[3], tr[3], tn[3], hu[3], gu[3], cu[3];
    for (int j = 0; j < 3; ++j) {
        v[j] = 0.0;
        for (int i = 0; i < 3; ++i) v[j] += R[i][j] * (anchor[i] - E[i]);
        tv[j] = R[2][j] * (-forwardM);        // R^T (0, 0, -F)
        tr[j] = R[2][j] * (-radarForwardM);   // ... for the radar
        tn[j] = R[2][j] * nearM;              // the opposite way: toward the camera, for the gunner panel
        hu[j] = R[1][j] * hudUpM;             // R^T (0, U, 0): seat up, for the weapon panel
        gu[j] = R[1][j] * gunnerUpM;          // the same axis, for the whole gunner frame
        cu[j] = R[1][j] * cardsUpM;           // the same axis, for the two cards only
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
    out.tNear[0] = static_cast<float>(tn[0]);
    out.tNear[1] = static_cast<float>(tn[1]);
    out.tNear[2] = static_cast<float>(-tn[2]);
    out.t[0] += static_cast<float>(hu[0]);
    out.t[1] += static_cast<float>(hu[1]);
    out.t[2] += static_cast<float>(-hu[2]);
    out.tNear[0] += static_cast<float>(gu[0]);
    out.tNear[1] += static_cast<float>(gu[1]);
    out.tNear[2] += static_cast<float>(-gu[2]);
    out.tCards[0] = static_cast<float>(cu[0]);
    out.tCards[1] = static_cast<float>(cu[1]);
    out.tCards[2] = static_cast<float>(-cu[2]);
    limitTranslation(out.t);
    limitTranslation(out.tNear);
    limitTranslation(out.tCards);
    return std::isfinite(out.a[0]) && std::isfinite(out.a[1]) && std::isfinite(out.a[2]) && std::isfinite(out.t[0]) && std::isfinite(out.t[1]) && std::isfinite(out.t[2]) &&
           std::isfinite(out.tRadar[0]) && std::isfinite(out.tRadar[1]) && std::isfinite(out.tRadar[2]) && std::isfinite(out.tNear[0]) && std::isfinite(out.tNear[1]) &&
           std::isfinite(out.tNear[2]) && std::isfinite(out.tCards[0]) && std::isfinite(out.tCards[1]) && std::isfinite(out.tCards[2]);
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

// ---- the eye's pose in the seated frame -------------------------------------------------------------------------------------------------------------------------------
// eye pose E = head * eyeToHead (R the 3x3, E the position), both the host's 3x4 row-major matrices (the same product eyeFrameFrom makes); R is what turretForwardWorld and headPitchDeg take.
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

}  // namespace srv_turret
}  // namespace edvr
