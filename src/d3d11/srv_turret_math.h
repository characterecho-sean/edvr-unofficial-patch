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
//
// THE TWO TEMPORARY KEYS (both live, both VR only, 0 = the game's own):
//   fix.srv_turret_camera_back  centimetres, 0..150, default 60: the camera moves back in the turret view, over the turret's axis. Part 1: published as the seated z of the
//                               eye alignment (alignTick), the single publisher of that alignment, so its serials stay coherent.
//   fix.srv_turret_hud_gap      centimetres, 0..200, default 96 (the panels' 1.39 m less the near layer's 0.42 m, less a centimetre): the deeper panels come forward by
//                               this much, to the near layer's depth. Part 2: a private pass-through geometry shader per output signature (srv_turret_hud_shader.h) around
//                               every issue of the families' draws; only a vertex whose view depth lies in the deep band moves.
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
inline constexpr int kGapDefaultCm = 96, kGapMaxCm = 200;

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
// Clip w is the view depth along the eye's forward axis, in metres, for every family listed: the hologram panels and the flight HUD dp4 their position against cb0[4..7] (the
// flight HUD's cb0[7] in the dump is {0.428, 0.321, -0.845, -118.28}: unit xyz), and the radar-named billboards and the sphere add their local offsets, scaled, to a translation
// column of the same eye's projection constants (b2 or b1 columns, no divide). The capture lines of the GPU instrument print the w each family really had.
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
inline constexpr uint32_t kSignatureCount = 9;
inline constexpr Signature kSignatures[kSignatureCount] = {
    {"1989E6D3", 3, {{"TEXCOORD", 0, 4}, {"TEXCOORD", 6, 3}, {"SV_Position", 0, 4}}, false},
    {"2CECEC30", 3, {{"TEXCOORD", 0, 4}, {"TEXCOORD", 7, 2}, {"SV_Position", 0, 4}}, true},
    {"DF3503CD", 2, {{"__USER_VERTEX_M_TEXCOORD", 0, 2}, {"SV_Position", 0, 4}}, false},
    {"44DDCDEB", 4, {{"__USER_RADAR_LOCALKEY_COLOUR", 0, 4}, {"__USER_RADAR_LOCALKEY_POSITIONOFFSET", 0, 4}, {"__USER_VERTEX_M_TEXCOORD", 0, 1}, {"SV_Position", 0, 4}}, true},
    {"9B34C331", 3, {{"__USER_RADAR_LOCALKEY_COLOUR", 0, 4}, {"__USER_RADAR_LOCALKEY_POSITIONOFFSET", 0, 3}, {"SV_Position", 0, 4}}, false},
    {"9611A454", 3, {{"__USER_RADAR_LOCALKEY_COLOUR", 0, 3}, {"__USER_RADAR_LOCALKEY_POSITIONOFFSET", 0, 2}, {"SV_Position", 0, 4}}, false},
    {"B932058F", 2, {{"__USER_RADAR_LOCALKEY_COLOUR", 0, 3}, {"SV_Position", 0, 4}}, false},
    {"5559BD94", 3, {{"__USER_VERTEX_M_LIGHTINGPOSITION", 0, 3}, {"__USER_VERTEX_M_TEXCOORD", 0, 2}, {"SV_Position", 0, 4}}, true},
    {"B7790CBF", 11, {{"TEXCOORD", 0, 4}, {"TEXCOORD", 1, 4}, {"TEXCOORD", 2, 4}, {"TEXCOORD", 5, 4}, {"TEXCOORD", 9, 4}, {"TEXCOORD", 10, 4}, {"TEXCOORD", 13, 4}, {"TEXCOORD", 16, 4},
                      {"TEXCOORD", 17, 2}, {"TEXCOORD", 18, 3}, {"SV_Position", 0, 4}}, false},
};
// The floats of one vertex as the geometry shader writes it (the instrument's stream-output record), and where the clip position, the last element, starts in it.
constexpr uint32_t sigFloats(uint32_t sig) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < kSignatures[sig].count; ++i) n += kSignatures[sig].el[i].comps;
    return n;
}
constexpr uint32_t sigPosFloat(uint32_t sig) { return sigFloats(sig) - kSignatures[sig].el[kSignatures[sig].count - 1].comps; }

struct Family {
    uint64_t vs, ps;
    const char* name;      // how the log names the family: the vertex shader's first eight hex digits, and for a second pixel shader of the same vertex shader the pixel shader's too
    uint32_t sig;          // the index into kSignatures of the family's vertex shader's output
    uint32_t strideBytes;  // the stream-output record (sigFloats * 4)
    uint32_t posFloat;     // where SV_POSITION starts in it, in floats
};
constexpr Family makeFamily(uint64_t vs, uint64_t ps, const char* name, uint32_t sig) { return Family{vs, ps, name, sig, sigFloats(sig) * 4, sigPosFloat(sig)}; }
inline constexpr uint32_t kFamilyCount = 10;
inline constexpr Family kFamilies[kFamilyCount] = {
    makeFamily(0x1989E6D3B405FDE0ull, 0xEAB8A1C95A13FFBEull, "1989E6D3", 0),
    makeFamily(0x2CECEC3065EF0D4Aull, 0xA4D03619D631B186ull, "2CECEC30", 1),
    makeFamily(0xDF3503CD07F9B10Cull, 0x8C08EB252B0F6095ull, "DF3503CD", 2),
    makeFamily(0x44DDCDEBAEA137D8ull, 0xCB53257EFE8B1696ull, "44DDCDEB", 3),
    makeFamily(0x9B34C331902DC1EDull, 0x3B3433E4FEBBC37Bull, "9B34C331", 4),
    makeFamily(0x9611A454527F7FEBull, 0x1E1C49DC51C0E509ull, "9611A454", 5),
    makeFamily(0xB932058F26B76691ull, 0x65861AC394D51526ull, "B932058F", 6),
    makeFamily(0x5559BD94B6852E83ull, 0xEA02FAC2BD6C643Cull, "5559BD94.EA02FAC2", 7),
    makeFamily(0x5559BD94B6852E83ull, 0xE95634B0F61D218Full, "5559BD94.E95634B0", 7),
    makeFamily(0xB7790CBFC6554097ull, 0x8DEF46452FA459F5ull, "B7790CBF", 8),
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
static_assert(kFamilies[0].strideBytes == 44 && kFamilies[0].posFloat == 7 && kFamilies[1].strideBytes == 40 && kFamilies[1].posFloat == 6 && kFamilies[9].strideBytes == 164 &&
                  kFamilies[9].posFloat == 37,
              "the stream-output records: (4 + 3 + 4), (4 + 2 + 4) and the flight HUD's (8 * 4 + 2 + 3 + 4) floats");
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

// Vertices at a view depth between wMin = gap + kWindowNearM and wMax = kWindowFarM move. The near layer (0.41 to 0.47 m) and anything far (the reticle's sprite, 500 m)
// never do: both lie outside the band, and the rule is per vertex.
inline constexpr float kWindowNearM = 0.10f;
inline constexpr float kWindowFarM = 4.0f;

// The geometry shader's constant buffer (GS slot 0, which the game never binds): {s, gap, wMin, wMax}, one register.
struct TurretParams {
    float s = 0.0f, gap = 0.0f, wMin = 0.0f, wMax = 0.0f;
    bool operator==(const TurretParams& o) const { return std::memcmp(this, &o, sizeof(TurretParams)) == 0; }
};
static_assert(sizeof(TurretParams) == 16, "the turret shader's cbuffer is one 16-byte register");

inline TurretParams paramsFor(float s, float gapM) { return TurretParams{s, gapM, gapM + kWindowNearM, kWindowFarM}; }

// What one vertex of the quad does, on the CPU (the shader's rule, written again for the rig and for reading): the clip x added, and the clip w it is read at.
//   moving a point `gap` metres nearer along its ray from the HEAD CENTRE keeps the cyclopean direction; an eye hx metres off the centre then sees it at
//   ndc x + P00 hx gap / (w (w - gap)), and with w kept (the rasterizer divides by it) that is clip x + P00 hx gap / (w - gap) = + s gap / (w - gap), s = P00 hx.
//   Exact for a fixed eye separation; y, z, w and the interpolants are untouched.
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
// they have, where they landed in NDC, and the shift the shader's rule gives them. A flight reads the near layer's draws with in_window 0 at about 0.4 m and the deep panels'
// with in_window above 0 at about 1.3 to 1.4 m.
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
