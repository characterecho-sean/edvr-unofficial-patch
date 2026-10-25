// F121 (native on-foot VR): the helmet visor's pass, centred on each eye's optical axis. Pure parts: header only, no game or D3D11 state, so the rig
// (tools\visor_axis_test) and remlok_fix.cpp run the very functions and format strings the headset does. Journal: the arc doc, 2026-10-25, F121.
//
// THE DEFECT (measured in the dumps of edvr_gfx_20261025_113157, eye dumps 113657139001 and 113736427002). One game draw does every helmet-glass effect
// on foot: VS B553BB479B7C0B97 / PS 68ABCB9FEF6CA66C, a fullscreen triangle per eye in the HDR scene pass. It draws the RemLok edge lines, the visor's
// condensation and curvature and the breath fog, all in the IMAGE's uv: the fog is a disc at uv (0.5, 0.75), the same pixels in both eyes. On a
// symmetric frustum that is a veil at infinity. On the Pimax Crystal Super the frustum is off axis (projection m02 -/+0.1784, tangents outer 1.255,
// inner 0.875): each eye's straight-ahead sits 0.1784 NDC (360 px of 4032) toward the nose of its image centre, so identical pixels are two directions
// 21.7 degrees apart, and the fog is two 48-degree discs seen double. Far-field content sits at d = xR - xL = -720 px (2 x 0.1784 x 2016) in seven
// dumps; the fog disc reads -0.2 px.
//
// THE FIX: the vertex shader's interpolated uv (o1.x, the only thing the pixel shader's visor frame is built from; its pixel position, which the edge
// lines and the refracted scene sample are keyed to, is a separate input and stays) is moved by s, so the frame centres on each eye's axis:
//     s_left = +a / 2, s_right = -a / 2, a = (outer - inner) / (outer + inner)    (uv units; the axis in NDC from the image centre, toward the nose)
// o1.x' = o1.x - s: the pixel at NDC x = a (the left eye's axis) then has uv 0.5, the centre of the visor frame. A symmetric headset has a = 0, s = 0,
// and nothing is bound.
#pragma once

#include <cmath>
#include <cstdint>
#include <cstdio>

namespace edvr {
namespace visoraxis {

constexpr uint64_t kVisorVsHash = 0xB553BB479B7C0B97ull;   // the game's visor vertex shader (content hash of its bytecode)
constexpr uint64_t kVisorPsHash = 0x68ABCB9FEF6CA66Cull;   // the pixel shader whose visor frame the shift is designed for
constexpr float kMaxAxisNdc = 0.6f;                         // a headset whose axis is further from the centre than this is not one this was written for
constexpr float kZeroShift = 1.0e-4f;                       // below this the frustum is symmetric: no shift to bind

// The eye's optical axis in NDC from the image centre, for the LEFT eye (toward the nose, so positive when the outer tangent is the larger). outer and inner
// are the tangent magnitudes eyeTangents() publishes (the left eye's; the right eye is its mirror). False for a value this was not written for.
inline bool axisNdc(float outer, float inner, float* a) noexcept {
    if (!std::isfinite(outer) || !std::isfinite(inner) || !(outer > 0.0f) || !(inner > 0.0f)) return false;
    const float v = (outer - inner) / (outer + inner);
    if (!std::isfinite(v) || std::fabs(v) >= kMaxAxisNdc) return false;
    if (a) *a = v;
    return true;
}

// The uv shift for an eye (0 left, 1 right): the left eye's frame moves toward the nose (right), the right eye's toward its nose (left).
inline bool shiftUv(float outer, float inner, int eye, float* s) noexcept {
    float a = 0.0f;
    if ((eye != 0 && eye != 1) || !axisNdc(outer, inner, &a)) return false;
    float v = 0.5f * a;
    if (eye == 1) v = -v;
    if (std::fabs(v) < kZeroShift) v = 0.0f;   // no negative zero in a log line, and a symmetric frustum is exactly zero
    if (s) *s = v;
    return true;
}

// The CPU model of the shifted vertex shader's interpolated uv: the fullscreen triangle's u along a clip-space x (u = (x + 1) / 2 before the shift).
inline float uvAtClipX(float clipX, float s) noexcept { return (clipX + 1.0f) * 0.5f - s; }

// Why a visor draw was not shifted, counted per window.
enum class Refusal : unsigned { kNoEye = 0, kNoTangents, kOtherPs, kNoShader, kCount };

struct Counts {
    unsigned long long seen[2] = {0, 0};      // visor draws that reached the swap, per eye (the eye the filing names; a draw with no eye counts in neither)
    unsigned long long shifted[2] = {0, 0};   // drawn through the replacement vertex shader
    unsigned long long symmetric = 0;         // s was zero (a symmetric frustum): the game's own shader, correctly
    unsigned long long keyOff = 0;            // F121b: the test key fix.native_on_foot_visor_axis was off (the default): the game's own shader, by choice
    unsigned long long refused[static_cast<unsigned>(Refusal::kCount)] = {0, 0, 0, 0};
    unsigned long long seenAll = 0;           // every visor draw, with or without an eye
    unsigned long long refusedAll() const {
        unsigned long long n = 0;
        for (unsigned long long r : refused) n += r;
        return n;
    }
};

inline const char* refusalText(Refusal r) noexcept {
    switch (r) {
        case Refusal::kNoEye: return "the draw's eye is not known (the eye filing has no entry for the bound target)";
        case Refusal::kNoTangents: return "the headset's eye tangents are not published (or not ones this was written for)";
        case Refusal::kOtherPs: return "the bound pixel shader is not 68ABCB9FEF6CA66C, the one the visor frame shift was designed for";
        case Refusal::kNoShader: return "the replacement vertex shader or its constant buffer could not be made";
        default: return "?";
    }
}

// The once-per-session lines (the health rule reads their words; the self-test renders them from these literals).
inline int formatArmed(char* out, size_t size) noexcept {
    return std::snprintf(out, size,
        "visor axis (F121): armed (remlok_lines outer): the on-foot visor draw (VS B553BB479B7C0B97) gets a vertex shader that moves its visor frame onto each eye's optical axis; no visor draw yet.");
}
inline int formatFirst(char* out, size_t size, float sLeft, float sRight, float axis, float outer, float inner, unsigned bytes) noexcept {
    return std::snprintf(out, size,
        "visor axis (F121): the visor frame is centred on each eye's optical axis: s left %+.4f right %+.4f uv, from the tangents outer %.3f inner %.3f (the axis %+.4f NDC from the image centre); "
        "replacement vertex shader for VS B553BB479B7C0B97 made (%u bytes of bytecode).",
        sLeft, sRight, outer, inner, axis, bytes);
}
inline int formatRefused(char* out, size_t size, const char* why) noexcept {
    return std::snprintf(out, size, "visor axis (F121): NOT shifted -- %s. The visor draws unchanged until that clears.", why);
}
inline int formatWindow(char* out, size_t size, const Counts& c, float sLeft, float sRight, float outer, float inner, bool tangents) noexcept {
    return std::snprintf(out, size,
        "visor axis (F121) 30 s: visor draws seen %llu (left %llu, right %llu), shifted %llu (left %llu, right %llu), symmetric %llu, key off %llu, refused %llu "
        "(no eye %llu, no tangents %llu, other pixel shader %llu, no replacement %llu); s left %+.4f right %+.4f uv from the tangents outer %.3f inner %.3f%s",
        c.seenAll, c.seen[0], c.seen[1], c.shifted[0] + c.shifted[1], c.shifted[0], c.shifted[1], c.symmetric, c.keyOff, c.refusedAll(),
        c.refused[0], c.refused[1], c.refused[2], c.refused[3], sLeft, sRight, outer, inner, tangents ? "" : " (not published)");
}

}  // namespace visoraxis
}  // namespace edvr
