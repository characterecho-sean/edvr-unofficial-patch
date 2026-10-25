// F117 (native on-foot VR): the panel cursor's place on a turned panel, pure parts. Header only, no game or D3D11 state, so native_frame_test runs the
// very functions the placement hook uses. Journal: the arc doc, 2026-10-25, F117.
//
// F114b's flight (log edvr_gfx_20261025_085120): on the panels whose pick mesh owner is exe+0x47299F0 the sprite the game places from the window's uv
// moved against the uv on both axes in the pick's frame (r -0.99 right, -0.98 down) while the clicks landed at the uv (38 presses with a uv, the pilot:
// "the mouse click does work if I can find the right spot"); on the one panel whose owner is exe+0x4526E40 it followed the uv (+0.94, +0.98), and there
// the pick camera agreed with the eye (+1.00 on all three axes). So the movie takes the true uv and only the sprite's place is wrong, on turned meshes.
// The game's own uv-to-point (panel vtable +0x188 -> 0x452EC00) is the barycentric point, in uv, of the pick mesh's triangle holding the uv. This file
// finds that triangle the same way, reads from it whether the mesh runs u along -x and v along +y in the pick's frame (turned), and gives the sprite's
// place on a turned panel two ways, behind one temporary setting: the panel's point at (1 - u, 1 - v) (the cursor turned about the panel's middle), or
// the sprite mirrored through the view's centre (x and y negated). An upright mesh is never moved.
#pragma once

#include "native_on_foot_f111.h"   // Corr

#include <cmath>
#include <cstddef>
#include <cstdint>

namespace edvr {
namespace nofoot {
namespace f117 {

// The pick mesh list (panel +0x460): triangles at +0x18, their count at +0x20; a triangle is 0x60 bytes: three vertices (vec4) at +0x00, +0x10, +0x20
// in the pick's camera frame, its plane at +0x30, the three uvs (two floats each) at +0x40, +0x48, +0x50 (0x45412E0 writes them, 0x452EC00 reads them).
constexpr uint32_t kOffList = 0x460, kListTris = 0x18, kListCount = 0x20, kTriStride = 0x60, kTriUv = 0x40;
constexpr size_t kMaxTris = 4096;
constexpr double kAtUvTolerance = 0.01;   // m: the sprite the game placed must be the mesh's point at the uv to within this

enum class Mode : uint8_t { kGame = 0, kPanel, kView };
inline const char* modeName(Mode m) noexcept { return m == Mode::kPanel ? "panel" : m == Mode::kView ? "view" : "game"; }

struct Tri {
    float v[3][4];
    float plane[4];
    float uv[3][2];
    float pad[2];
};
static_assert(sizeof(Tri) == kTriStride, "a pick mesh triangle is 0x60 bytes");

// The barycentric weights of (u, v) in the triangle's uv, the game's way (uv2 as the origin): false outside it or for a degenerate triangle.
inline bool baryUv(const Tri& t, double u, double v, double b[3]) noexcept {
    const double a0 = double(t.uv[0][0]) - t.uv[2][0], a1 = double(t.uv[0][1]) - t.uv[2][1];
    const double c0 = double(t.uv[1][0]) - t.uv[2][0], c1 = double(t.uv[1][1]) - t.uv[2][1];
    const double det = a0 * c1 - a1 * c0;
    if (!(std::fabs(det) > 1e-12)) return false;
    const double du = u - t.uv[2][0], dv = v - t.uv[2][1];
    b[0] = (du * c1 - dv * c0) / det;
    b[1] = (a0 * dv - a1 * du) / det;
    b[2] = 1.0 - b[0] - b[1];
    const double e = -1e-6;
    return b[0] >= e && b[1] >= e && b[2] >= e;
}
inline void pointAt(const Tri& t, const double b[3], double p[3]) noexcept {
    for (int k = 0; k < 3; ++k) p[k] = b[0] * t.v[0][k] + b[1] * t.v[1][k] + b[2] * t.v[2][k];
}
// The triangle's uv-to-point derivatives (the pick's frame): dP/du and dP/dv.
inline bool jacobian(const Tri& t, double du[3], double dv[3]) noexcept {
    const double a0 = double(t.uv[0][0]) - t.uv[2][0], a1 = double(t.uv[0][1]) - t.uv[2][1];
    const double c0 = double(t.uv[1][0]) - t.uv[2][0], c1 = double(t.uv[1][1]) - t.uv[2][1];
    const double det = a0 * c1 - a1 * c0;
    if (!(std::fabs(det) > 1e-12)) return false;
    for (int k = 0; k < 3; ++k) {
        const double e0 = double(t.v[0][k]) - t.v[2][k], e1 = double(t.v[1][k]) - t.v[2][k];
        du[k] = (e0 * c1 - e1 * a1) / det;
        dv[k] = (e1 * a0 - e0 * c0) / det;
    }
    return true;
}
// Upright facing the camera, a panel runs u along +x and v along -y (v grows downward); turned, u along -x and v along +y.
inline bool turned(const double du[3], const double dv[3]) noexcept { return du[0] < 0.0 && dv[1] > 0.0; }

inline int findTri(const Tri* tris, size_t n, double u, double v, double b[3]) noexcept {
    for (size_t i = 0; i < n; ++i)
        if (baryUv(tris[i], u, v, b)) return static_cast<int>(i);
    return -1;
}

enum class Why : uint8_t { kMoved = 0, kUpright, kGame, kNoTriangle, kNotAtUv, kNoTurnedTriangle };
inline const char* whyName(Why w) noexcept {
    switch (w) {
        case Why::kMoved: return "moved";
        case Why::kUpright: return "upright";
        case Why::kGame: return "the game's placement";
        case Why::kNoTriangle: return "no triangle at the uv";
        case Why::kNotAtUv: return "the sprite not at the uv";
        default: return "no triangle at the turned uv";
    }
}

struct Decision {
    Why why = Why::kNoTriangle;
    bool turned = false;
    double target[3] = {};
};

// One 2D placement: the mesh's triangles, the window's uv (clamped to 0..1 as the game clamps it) and the sprite the game placed there.
inline Decision decide(const Tri* tris, size_t n, double u, double v, const float sprite[3], Mode mode) noexcept {
    Decision d;
    u = u < 0.0 ? 0.0 : u > 1.0 ? 1.0 : u;
    v = v < 0.0 ? 0.0 : v > 1.0 ? 1.0 : v;
    double b[3];
    const int i = findTri(tris, n, u, v, b);
    if (i < 0) {
        d.why = Why::kNoTriangle;
        return d;
    }
    double p[3], du[3], dv[3];
    pointAt(tris[i], b, p);
    const double off = std::sqrt((p[0] - sprite[0]) * (p[0] - sprite[0]) + (p[1] - sprite[1]) * (p[1] - sprite[1]) + (p[2] - sprite[2]) * (p[2] - sprite[2]));
    if (!(off <= kAtUvTolerance)) {
        d.why = Why::kNotAtUv;
        return d;
    }
    d.turned = jacobian(tris[i], du, dv) && turned(du, dv);
    if (!d.turned) {
        d.why = Why::kUpright;
        return d;
    }
    if (mode == Mode::kGame) {
        d.why = Why::kGame;
        return d;
    }
    if (mode == Mode::kView) {
        d.target[0] = -double(sprite[0]);
        d.target[1] = -double(sprite[1]);
        d.target[2] = sprite[2];
        d.why = Why::kMoved;
        return d;
    }
    const int j = findTri(tris, n, 1.0 - u, 1.0 - v, b);
    if (j < 0) {
        d.why = Why::kNoTurnedTriangle;
        return d;
    }
    pointAt(tris[j], b, d.target);
    d.why = Why::kMoved;
    return d;
}

// The cursor against the true uv, per second: the uv's change against the sprite's (as drawn, after the setting) on turned and on upright panels.
struct Sec {
    uint32_t placements = 0, turned = 0, upright = 0, moved = 0, refused[6] = {};
    f111::Corr turnedUv[2], uprightUv[2];   // [0] u against x, [1] v against -y
    void merge(const Sec& o) noexcept {
        placements += o.placements;
        turned += o.turned;
        upright += o.upright;
        moved += o.moved;
        for (int i = 0; i < 6; ++i) refused[i] += o.refused[i];
        for (int i = 0; i < 2; ++i) {
            turnedUv[i].merge(o.turnedUv[i]);
            uprightUv[i].merge(o.uprightUv[i]);
        }
    }
};
struct Last {
    bool valid = false, turned = false;
    double u = 0.0, v = 0.0, p[3] = {};
};
inline void add(Sec& s, Last& last, const Decision& d, double u, double v, const double shown[3]) noexcept {
    ++s.placements;
    s.refused[static_cast<int>(d.why)] += d.why == Why::kMoved ? 0u : 1u;
    if (d.why == Why::kMoved) ++s.moved;
    if (d.why == Why::kNoTriangle || d.why == Why::kNotAtUv) {
        last.valid = false;
        return;
    }
    if (d.turned) ++s.turned;
    else ++s.upright;
    if (last.valid && last.turned == d.turned && (std::fabs(u - last.u) > 1e-6 || std::fabs(v - last.v) > 1e-6)) {
        f111::Corr* c = d.turned ? s.turnedUv : s.uprightUv;
        c[0].add(u - last.u, shown[0] - last.p[0]);
        c[1].add(v - last.v, -(shown[1] - last.p[1]));
    }
    last.valid = true;
    last.turned = d.turned;
    last.u = u;
    last.v = v;
    for (int k = 0; k < 3; ++k) last.p[k] = shown[k];
}
inline int pairSign(const f111::Corr c[2]) noexcept {
    const int a = c[0].sign(), b = c[1].sign();
    return a > 0 && b > 0 ? 1 : a < 0 && b < 0 ? -1 : 0;
}
// The cursor's verdict on turned panels: follows the uv, against it, or not judged.
inline const char* cursorText(const Sec& s) noexcept {
    if (!s.turned) return "no turned panel";
    const int t = pairSign(s.turnedUv);
    return t > 0 ? "follows the uv" : t < 0 ? "against the uv" : "not judged";
}

}  // namespace f117
}  // namespace nofoot
}  // namespace edvr
