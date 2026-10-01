// The intro composite's vertex-shader constants, read as a world-space panel: the pure half of the intro movie and the splash
// following fix.panel_curvature. No D3D: 20 floats in, a reading out. tools\intro_curve_math_test is its rig, and
// tools\intro_curve_math_test\mutants.py holds that rig to every rule below.
//
// THE CONSTANTS. The intro composite (the movie's frame and the splash's still are the same draw: VS EF103A7CB4A8369A, PS
// DED8796049C7BB4A, DrawIndexedInstanced(6, 1) over a unit quad) places itself entirely through VS b2: 80 bytes = 20 floats =
// cb2[0..4] (docs\shaders\intro-composite-vs.asm):
//     (x, y) = (v0.x, v0.y) * cb2[0].xy
//     o1     = x*cb2[1] + y*cb2[2] + v0.z*cb2[3] + cb2[4]            o1 = the clip-space position (x y z w)
// so cb2[1..4] are the columns of a 4x4 acting on (x, y, z, 1). The floats, f[i] being cb2[i / 4] component i % 4:
//     f[0], f[1]     cb2[0].xy   the quad's half-size: for a world-space panel its half-width and half-height in metres (measured
//                                 4.44444 and 2.5); for the movie's stock screen-space placement, in pixels (512 and 288)
//     f[2], f[3]     cb2[0].zw   unused
//     f[4..7]        cb2[1]      the x column: where one unit of x goes in clip space (f[7] is its w)
//     f[8..11]       cb2[2]      the y column (f[11] is its w)
//     f[12..15]      cb2[3]      the z column: where one unit of the quad's z, its offset off the plane, goes. The unit quad has
//                                 z = 0, so it moves nothing today; it is what a strip bent out of the plane scales. Zero in the
//                                 movie's stock constants, a real unit-scale column in the splash's
//     f[16..19]      cb2[4]      the translation: the panel's centre in clip space (f[19] is its w)
// The perspective divide is by clip w. Screen space (the movie's stock constants): w is the constant 1 (cb2[1..3].w == 0 and
// cb2[4].w == 1). World space (the splash): w varies over the panel, and at its centre it is cb2[4].w, the distance in front of the
// eye in the eye's units (negative: the panel is behind the eye).
//
// THE DEPTH DIRECTION. A unit step in z changes w by cb2[3].w, so when cb2[3].w and cb2[4].w have OPPOSITE signs the step reduces
// |w|: it moves the point toward the viewer. With the same sign it moves the point away. introReadWorldCb reads exactly that and
// calls it `toward` (+1 or -1).
//
// TWO REAL CAPTURES of the game's own splash constants (Frontier log edvr_gfx_20260828_182818.log, the `DCW read` lines; cb2[0..4]):
//   capture 2, the panel in front of the viewer (cb2[4].w = +3.76):
//     4.44444 2.5 0 0   -0.780684 -0.0197438 9.48621e-08 -0.000948464   -0.0047429 0.788105 -7.60756e-06 0.076063
//     -0.194148 0.0601386 9.97268e-05 -0.997103   -0.0725725 -0.247872 0.0996338 3.76108
//   capture 1, the head yawed about 180 degrees out, the panel behind the viewer (cb2[4].w = -1.59):
//     4.44444 2.5 0 0   0.795297 0.0141897 -9.32133e-06 0.0931979   -0.0047877 0.788078 -7.64753e-06 0.0764627
//     0.121095 -0.0620333 -9.92872e-05 0.992707   -0.908756 18.4664 0.100169 -1.5911
// and the movie's STOCK constants (docs\frontier-intro-video-report.md, left eye):
//     512 288 0 0   -0.000368732 0 0 0   0 0.000373413 0 0   0 0 0 0   0.193907 0 0 1
// FINDING. In both captures cb2[3] is a real unit-scale column (its length is 1.02 and 1.00) and cb2[3].w and cb2[4].w have
// OPPOSITE signs (-0.997 with +3.761, and +0.993 with -1.591): the game maps +z toward the viewer whether the panel is in front of the
// eye or, after a 180-degree yaw, behind it. The panel's orientation flips between the captures; the depth direction does not, so one
// rule (opposite signs = toward = +1) reads both. The movie's stock constants are screen space and read as such.
#pragma once

#include <cmath>
#include <cstdint>

namespace edvr {

// Today's rule: is this the movie's SCREEN-space placement, and not a world-space one? Verbatim from intro_panel.cpp's
// looksScreenSpace (2026-10-01): w is a constant 1 (cb2[1].w, cb2[2].w and all of cb2[3] zero, cb2[4].w == 1) and the half-size is
// at least 16 pixels (a world-space quad measured 4.4 by 2.5, a screen-space one 512 by 288). The zero test is the open interval
// (-1e-9, 1e-9), so exactly 1e-9 is not zero. The w and half-size tests reject only what lies outside, so a NaN passes them.
inline bool introCbLooksScreenSpace(const float f[20]) {
    auto zero = [](float v) { return v > -1e-9f && v < 1e-9f; };
    if (!zero(f[7]) || !zero(f[11])) return false;       // cb2[1].w, cb2[2].w
    for (uint32_t i = 12; i < 16; ++i) {                 // cb2[3] entirely
        if (!zero(f[i])) return false;
    }
    if (f[19] < 0.999f || f[19] > 1.001f) return false;  // cb2[4].w == 1
    if (f[0] < 16.0f || f[1] < 16.0f) return false;
    return true;
}

// What a world-space panel's constants say. ok only when every condition below holds; otherwise `why` names the first one that
// failed (a static string, for one log line) and halfWidth and toward are 0.
struct IntroWorldCb {
    bool ok;
    float halfWidth;   // cb2[0].x, the panel's half-width in metres
    int toward;        // +1: a step in +z moves toward the viewer; -1: away
    const char* why;   // "" when ok
};

constexpr float kIntroCbMinLength = 0.5f;   // cb2[3]'s length: unit scale, within a factor of two either way
constexpr float kIntroCbMaxLength = 2.0f;
constexpr float kIntroCbMinW = 1e-3f;       // |cb2[3].w| and |cb2[4].w| must exceed this for the depth direction to be readable
constexpr float kIntroCbMinHalf = 0.5f;     // cb2[0].x and cb2[0].y, a half-size in metres (measured 4.44444 and 2.5)
constexpr float kIntroCbMaxHalf = 50.0f;

// Finite in the sense that matters, written out as intro_panel.cpp's isFiniteF is (unambiguous under /fp:precise).
inline bool introCbFinite(float v) { return v == v && v <= 3.4e38f && v >= -3.4e38f; }

// In this order: all 20 floats finite; not the screen-space placement; cb2[3] of unit-scale length; |cb2[3].w| and |cb2[4].w| clear
// of zero; cb2[0].x and cb2[0].y plausible half-sizes. The first to fail is named.
inline IntroWorldCb introReadWorldCb(const float f[20]) {
    IntroWorldCb r = {false, 0.0f, 0, ""};
    for (int i = 0; i < 20; ++i) {
        if (!introCbFinite(f[i])) {
            r.why = "a constant is not a finite number";
            return r;
        }
    }
    if (introCbLooksScreenSpace(f)) {
        r.why = "the constants read as a screen-space placement (cb2[3] is zero and w is a constant 1), not a world-space panel";
        return r;
    }
    const double c0 = f[12], c1 = f[13], c2 = f[14], c3 = f[15];
    const double length = std::sqrt(c0 * c0 + c1 * c1 + c2 * c2 + c3 * c3);
    if (!(length >= kIntroCbMinLength && length <= kIntroCbMaxLength)) {
        r.why = "cb2[3] is not a unit-scale column (its length is outside 0.5 to 2)";
        return r;
    }
    if (!(std::fabs(f[15]) > kIntroCbMinW)) {
        r.why = "cb2[3].w is too close to zero to tell which way the panel's depth runs";
        return r;
    }
    if (!(std::fabs(f[19]) > kIntroCbMinW)) {
        r.why = "cb2[4].w is too close to zero to tell which way the panel's depth runs";
        return r;
    }
    if (!(f[0] >= kIntroCbMinHalf && f[0] <= kIntroCbMaxHalf)) {
        r.why = "cb2[0].x is not a plausible half-width in metres (outside 0.5 to 50)";
        return r;
    }
    if (!(f[1] >= kIntroCbMinHalf && f[1] <= kIntroCbMaxHalf)) {
        r.why = "cb2[0].y is not a plausible half-height in metres (outside 0.5 to 50)";
        return r;
    }
    r.ok = true;
    r.halfWidth = f[0];
    r.toward = (f[15] > 0.0f) != (f[19] > 0.0f) ? 1 : -1;
    return r;
}

}  // namespace edvr
