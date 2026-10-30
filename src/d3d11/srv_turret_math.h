// The SRV turret view (issue 140): the pure parts, shared by the module (srv_turret.cpp), the draw path and tools/srv_turret_hud_test, which runs these very
// functions beside the production geometry shader on WARP.
//
// THE FAULT. In the Scorpion SRV's turret (gunner) view in VR, Elite places the turret HUD badly:
//   1. The camera sits too far forward. The near HUD layer (the two weapon arc brackets, SECONDARY A / PRIMARY A, the ammo counts) is about 0.11 m BEHIND the
//      default eye point, invisible unless the player leans back.
//   2. The target information panel and the SYSTEMS panel sit about a metre deeper than that layer (measured from an eye dump: arc 0.158 m, info panel 1.176 m, in the
//      flight of 2026-10-30 the near layer 0.41 to 0.47 m and the panels 1.29 and 1.41 m, after the camera move).
// WHICH DRAWS (an offline emulation of the dump, 2026-10-30): the panels are drawn by the SAME two instanced hologram families as the near layer, from records placed about
// 1.39 m out against 0.41 to 0.47 m for the near layer's: vs 1989E6D3B405FDE0 (ps EAB8A1C95A13FFBE) and vs 2CECEC3065EF0D4A (ps A4D03619D631B186). The targeting sprite
// (vs E508648660A352B2, ps 63ABD86359B57D01) is NOT a panel: its three draws are the reticle's three layers, at clip w of about 500 m out at the target. An earlier build of
// this pass targeted that sprite, bound its shader for every draw and moved nothing, because no vertex of it lay in the window.
//
// THE TWO TEMPORARY KEYS (both live, both VR only, 0 = the game's own):
//   fix.srv_turret_camera_back  centimetres, 0..150, default 60: the camera moves back in the turret view, over the turret's axis. Part 1: published as the seated z of the
//                               eye alignment (alignTick), the single publisher of that alignment, so its serials stay coherent.
//   fix.srv_turret_hud_gap      centimetres, 0..200, default 96 (the panels' 1.39 m less the near layer's 0.42 m, less a centimetre): the deeper panels come forward by
//                               this much, to the near layer's depth. Part 2: a private pass-through geometry shader per family (srv_turret_hud_shader.h) around every
//                               issue of the two families' draws; only a vertex whose view depth lies in the deep band moves.
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

// ---- Part 1: when, and which way -------------------------------------------------------------------------------------------------------------------------------
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

// ---- Part 2: the two hologram families and the shift ---------------------------------------------------------------------------------------------------------------
// Each family is a vertex shader and the pixel shader it is drawn with. Both vertex shaders take the same inputs (INSTANCEANDMODELDATAINDEX, PACKEDVERTEXDATAA/B/C) and
// build a camera-relative position that dp4 against cb0[4..7] makes the clip position, so clip w is the view depth along the eye's forward axis, in metres. Their output
// signatures, from the fxc disassembly of the dumps' bytecode (and the pixel shaders' inputs, a subset of them in each case):
//   family 0  vs 1989E6D3B405FDE0 + ps EAB8A1C95A13FFBE:  TEXCOORD 0 xyzw (register 0), TEXCOORD 6 xyz (register 1), SV_POSITION xyzw (register 2); the pixel shader reads
//             TEXCOORD0 xyzw and TEXCOORD6 xyz.
//   family 1  vs 2CECEC3065EF0D4A + ps A4D03619D631B186:  TEXCOORD 0 xyzw (register 0), TEXCOORD 7 xy (register 1), SV_POSITION xyzw (register 2); the pixel shader reads
//             TEXCOORD0 xyzw, TEXCOORD7 xy and SV_POSITION xy (the pixel position).
// The geometry shader of each family declares that signature exactly, in that order (srv_turret_hud_shader.h), so the linkage the D3D debug layer validates is exact.
// strideBytes and posFloat describe the instrument's stream-output record (srv_turret.cpp): the vertex as the geometry shader writes it, its floats in the signature's
// order, the clip position at float offset posFloat.
struct Family {
    uint64_t vs, ps;
    const char* name;      // the vertex shader's first eight hex digits: how the log names the family
    uint32_t strideBytes;  // the stream-output record: (4 + 3 + 4) floats, (4 + 2 + 4) floats
    uint32_t posFloat;     // where SV_POSITION starts in it, in floats
};
inline constexpr uint32_t kFamilyCount = 2;
inline constexpr Family kFamilies[kFamilyCount] = {
    {0x1989E6D3B405FDE0ull, 0xEAB8A1C95A13FFBEull, "1989E6D3", 44, 7},
    {0x2CECEC3065EF0D4Aull, 0xA4D03619D631B186ull, "2CECEC30", 40, 6},
};
// The family of a draw's shader pair, or -1: both halves of the pair must match.
inline int familyOf(uint64_t vs, uint64_t ps) {
    for (uint32_t i = 0; i < kFamilyCount; ++i)
        if (kFamilies[i].vs == vs && kFamilies[i].ps == ps) return static_cast<int>(i);
    return -1;
}

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
inline constexpr uint32_t kCaptureBufferBytes = 32768;
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
