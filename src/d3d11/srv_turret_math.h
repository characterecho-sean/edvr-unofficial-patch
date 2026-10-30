// The SRV turret view (issue 140): the pure parts, shared by the module (srv_turret.cpp), the draw path and tools/srv_turret_hud_test, which runs these very
// functions beside the production geometry shader on WARP.
//
// THE FAULT. In the Scorpion SRV's turret (gunner) view in VR, Elite places the turret HUD badly:
//   1. The camera sits too far forward. The near HUD layer (the two weapon arc brackets, SECONDARY A / PRIMARY A, the ammo counts) is about 0.11 m BEHIND the
//      default eye point, invisible unless the player leans back.
//   2. The target info panel and the SYSTEMS panel sit about 1.02 m deeper than that layer, yet are drawn OVER it (their vertex shader writes the clip z = |w|, the
//      near plane), so in stereo the far panel occludes the nearer arcs. Measured from an eye dump: arc 0.158 m, info panel 1.176 m, reticle at infinity.
//
// THE TWO TEMPORARY KEYS (both live, both VR only, 0 = the game's own):
//   fix.srv_turret_camera_back  centimetres, 0..150, default 60: the camera moves back in the turret view, over the turret's axis. Part 1: published as the seated z of the
//                               eye alignment (alignTick), the single publisher of that alignment, so its serials stay coherent.
//   fix.srv_turret_hud_gap      centimetres, 0..200, default 102: the deeper panels come forward by this much, to the near layer's depth. Part 2: a private pass-through
//                               geometry shader (srv_turret_hud_shader.h) around every issue of the targeting sprite's draws.
#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>

namespace edvr {
namespace srv_turret {

// ---- the keys -------------------------------------------------------------------------------------------------------------------------------------------------
inline constexpr int kBackDefaultCm = 60, kBackMaxCm = 150;
inline constexpr int kGapDefaultCm = 102, kGapMaxCm = 200;

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

// ---- Part 2: the targeting sprite and the shift --------------------------------------------------------------------------------------------------------------------
// vs E508648660A352B2 with ps 63ABD86359B57D01 (ui_layer_math.h kUiVsSprite, ui_depth.cpp, target_sharp.cpp). The vertex shader's output signature, from its
// disassembly: TEXCOORD 0 xy at register 0, SV_POSITION xyzw at register 1. It computes a camera-relative position r0 and writes o1.x = dp4(cb0[4], r0), o1.y =
// dp4(cb0[5], r0), o1.w = dp4(cb0[7], r0), o1.z = |o1.w|: the clip w is the view depth along the eye's forward axis, in metres, and Elite's reversed Z puts z = w on the
// near plane.
inline constexpr uint64_t kSpriteVs = 0xE508648660A352B2ull;
inline constexpr uint64_t kSpritePs = 0x63ABD86359B57D01ull;

// Vertices at a view depth between wMin = gap + kWindowNearM and wMax = kWindowFarM move; the reticle (at the target, far away) never does.
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

}  // namespace srv_turret
}  // namespace edvr
