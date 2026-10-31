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
// 1.3 to 2.8 m. So the near layer is pushed BACK to the larger panels' depth instead, with the same geometry shader and a NEGATIVE gap: a vertex nearer than kPushSplitM moves
// `back` metres deeper along its ray from the head centre, keeping its size on screen, and a vertex at 1 m or more does not move.
//
// THE FOUR TEMPORARY KEYS (all live, all VR only, 0 = the game's own):
//   fix.srv_turret_camera_back  centimetres, 0..150, default 60: the camera moves back in the turret view, over the turret's axis. Part 1: published as the seated z of the
//                               eye alignment (alignTick), the single publisher of that alignment, so its serials stay coherent.
//   fix.srv_turret_panels_back  centimetres, 0..200, default 110: the weapon panels (Group::kPanels: the two hologram families) go this much deeper where they lie nearer than
//                               kPushSplitM; the larger panels of the same families, at 1.3 m and more, stay. Part 2: a private pass-through geometry shader per output
//                               signature (srv_turret_hud_shader.h) around every issue of the families' draws; the rule is per vertex.
//   fix.srv_turret_radar_back   centimetres, 0..200, default 110: the same for the radar (Group::kRadar), which the game lays on the turret deck right under the player; pushed
//                               back it floats below the deck's surface in stereo while still drawn over it.
//   fix.srv_turret_hud_gap      centimetres, 0..200, default 0: the first build's forward pull, kept for comparison. For a family whose group has a push above 0 the push is
//                               used and this is not; for a group with no push this brings the vertices deeper than gap + kWindowNearM (up to kWindowFarM) forward by it.
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
inline constexpr int kBackDefaultCm = 60, kBackMaxCm = 150;
inline constexpr int kGapDefaultCm = 0, kGapMaxCm = 200;
inline constexpr int kPushDefaultCm = 110, kPushMaxCm = 200;   // fix.srv_turret_panels_back and fix.srv_turret_radar_back

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
// The floats of one vertex as the geometry shader writes it (the instrument's stream-output record), and where the clip position, the last element, starts in it.
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
    uint32_t strideBytes;  // the stream-output record (sigFloats * 4)
    uint32_t posFloat;     // where SV_POSITION starts in it, in floats
    Group group;           // the push-back key that governs the family
};
constexpr Family makeFamily(uint64_t vs, uint64_t ps, const char* name, uint32_t sig, Group group) {
    return Family{vs, ps, name, sig, sigFloats(sig) * 4, sigPosFloat(sig), group};
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
        if (kSignatures[s].count < 2 || kSignatures[s].count > kMaxSigElements || kSignatures[s].count > 32) return false;
        const SigElement& last = kSignatures[s].el[kSignatures[s].count - 1];
        if (last.comps != 4 || last.name[0] != 'S' || last.name[1] != 'V' || 3 * sigFloats(s) > 1024) return false;
    }
    for (uint32_t f = 0; f < kFamilyCount; ++f)
        if (kFamilies[f].sig >= kSignatureCount) return false;
    return true;
}
static_assert(signaturesFit(), "every signature ends in a 4-component SV_Position and fits the geometry stage's limits; every family names a signature");
static_assert(kFamilies[0].strideBytes == 44 && kFamilies[0].posFloat == 7 && kFamilies[1].strideBytes == 40 && kFamilies[1].posFloat == 6 && kFamilies[8].strideBytes == 36 &&
                  kFamilies[8].posFloat == 5,
              "the stream-output records: (4 + 3 + 4), (4 + 2 + 4) and the target hologram sphere's (3 + 2 + 4) floats");
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

// THE PULL (fix.srv_turret_hud_gap): vertices at a view depth between wMin = gap + kWindowNearM and wMax = kWindowFarM move `gap` metres nearer. The near layer and anything far
// (the reticle's sprite, 500 m) never do: both lie outside the band, and the rule is per vertex.
inline constexpr float kWindowNearM = 0.10f;
inline constexpr float kWindowFarM = 4.0f;
// THE PUSH (fix.srv_turret_panels_back, fix.srv_turret_radar_back): vertices at a view depth between kPushNearM and kPushSplitM move `back` metres DEEPER (a negative gap). The
// radar lies at 0.11 m and more, so 2 cm keeps the window open to the nearest thing the families draw while a vertex at or behind the eye plane (w <= 0) is never touched; 1 m splits
// the weapon panels (0.27 to 0.70 m) from the larger panels (1.3 m and deeper), which stay.
inline constexpr float kPushNearM = 0.02f;
inline constexpr float kPushSplitM = 1.0f;

// The geometry shader's constant buffer (GS slot 0, which the game never binds): {s, gap, wMin, wMax}, one register.
struct TurretParams {
    float s = 0.0f, gap = 0.0f, wMin = 0.0f, wMax = 0.0f;
    bool operator==(const TurretParams& o) const { return std::memcmp(this, &o, sizeof(TurretParams)) == 0; }
};
static_assert(sizeof(TurretParams) == 16, "the turret shader's cbuffer is one 16-byte register");

inline TurretParams paramsFor(float s, float gapM) { return TurretParams{s, gapM, gapM + kWindowNearM, kWindowFarM}; }
// The push: a negative gap. A vertex nearer than 1 m moves backM metres deeper, x += -s backM / (w + backM). The shader computes w - gap = w + backM, positive over the whole window.
inline TurretParams pushParamsFor(float s, float backM) { return TurretParams{s, -backM, kPushNearM, kPushSplitM}; }

// Which constants a group's draws run with, from its push key, the gap key and the eye's s: the push when it is above 0, else the pull when the gap is above 0, else none (false:
// the draw goes through as the game made it). The keys are centimetres; each is held to its own range.
inline bool paramsForGroup(float s, int pushCm, int gapCm, TurretParams& out) {
    if (pushCm > 0) {
        out = pushParamsFor(s, static_cast<float>(pushCm > kPushMaxCm ? kPushMaxCm : pushCm) / 100.0f);
        return true;
    }
    if (gapCm > 0) {
        out = paramsFor(s, static_cast<float>(gapCm > kGapMaxCm ? kGapMaxCm : gapCm) / 100.0f);
        return true;
    }
    return false;
}
inline int pushCmOf(Group g, int panelsCm, int radarCm) { return g == Group::kPanels ? panelsCm : radarCm; }

// What one vertex of the quad does, on the CPU (the shader's rule, written again for the rig and for reading): the clip x added, and the clip w it is read at.
//   moving a point `gap` metres nearer along its ray from the HEAD CENTRE keeps the cyclopean direction; an eye hx metres off the centre then sees it at
//   ndc x + P00 hx gap / (w (w - gap)), and with w kept (the rasterizer divides by it) that is clip x + P00 hx gap / (w - gap) = + s gap / (w - gap), s = P00 hx.
//   Exact for a fixed eye separation; y, z, w and the interpolants are untouched. A NEGATIVE gap is the same point moved deeper: for a push of d metres the shift is -s d / (w + d).
inline float clipXShift(const TurretParams& p, float w) {
    return (w > p.wMin && w < p.wMax) ? p.s * p.gap / (w - p.gap) : 0.0f;
}

// P00 of an eye's projection as Elite renders it, from the tangents {left, right, down, up} (temporal pass order: left and down negative): the NDC span 2 over the
// tangent span. 0 when the tangents are not a frustum.
inline float p00OfTangents(const float tan[4]) {
    if (!tan) return 0.0f;
    const float span = tan[1] - tan[0];
    if (!std::isfinite(span) || !(span > 1.0e-4f) || !(span < 1.0e4f)) return 0.0f;
    return 2.0f / span;
}

// hx: the head centre's place in the eye's frame, +ipd/2 for the LEFT eye (index 0) and -ipd/2 for the right: the left eye sees an object that moves nearer along
// the cyclopean ray shift to the right in its own image (towards the nose), the right eye to the left.
inline float eyeHx(int eye, float ipdM) { return eye == 0 ? 0.5f * ipdM : -0.5f * ipdM; }

// The eye separation Elite renders with (the located one times the world scale factor, common/world_scale.h): a human range with room for every world scale, or "not known".
inline bool ipdUsable(float ipdM) { return std::isfinite(ipdM) && ipdM > 0.02f && ipdM < 0.20f; }
inline bool p00Usable(float p00) { return std::isfinite(p00) && p00 > 0.05f && p00 < 20.0f; }

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
// pipeline-statistics query around it. The records the twin streamed out are the vertex as the shader wrote it (Family::strideBytes: every signature element in order, the
// clip position, AFTER the shift, at float offset Family::posFloat). This summarizes them: how many vertices lie in the window (the ones the shader moves), the view depth range
// they have, where they landed in NDC, and the shift the shader's rule gives them. With a push a flight reads the weapon panels' and the radar's draws (w 0.1 to 0.7 m) with
// in_window above 0 and the larger panels' (1.3 m and deeper) with in_window 0; with the pull it is the other way round.
inline constexpr uint32_t kCaptureBufferBytes = 65536;
inline constexpr uint32_t captureMaxVertices(uint32_t strideBytes) { return strideBytes ? kCaptureBufferBytes / strideBytes : 0; }
struct CaptureSummary {
    uint32_t vertices = 0, inWindow = 0;
    float wMin = 0.0f, wMax = 0.0f;           // clip w over all vertices
    float ndcXMin = 0.0f, ndcXMax = 0.0f;     // clip x / w over the vertices with w > 0, after the shift
    float ndcYMin = 0.0f, ndcYMax = 0.0f;
    float shiftMin = 0.0f, shiftMax = 0.0f;   // the shader's rule over the in-window vertices (0 and 0 when none)
};
inline CaptureSummary summarizeCapture(const float* records, uint32_t vertices, uint32_t strideFloats, uint32_t posFloat, const TurretParams& p) {
    CaptureSummary s;
    if (!records || strideFloats < posFloat + 4) return s;
    if (vertices > captureMaxVertices(strideFloats * 4)) vertices = captureMaxVertices(strideFloats * 4);
    bool haveW = false, haveNdc = false, haveShift = false;
    for (uint32_t i = 0; i < vertices; ++i) {
        const float* r = records + static_cast<size_t>(strideFloats) * i + posFloat;   // x, y, z, w
        const float x = r[0], y = r[1], w = r[3];
        if (!std::isfinite(w) || !std::isfinite(x) || !std::isfinite(y)) continue;
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
        if (w > p.wMin && w < p.wMax) {
            ++s.inWindow;
            const float shift = clipXShift(p, w);
            if (!haveShift || shift < s.shiftMin) s.shiftMin = shift;
            if (!haveShift || shift > s.shiftMax) s.shiftMax = shift;
            haveShift = true;
        }
    }
    return s;
}

}  // namespace srv_turret
}  // namespace edvr
