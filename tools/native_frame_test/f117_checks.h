// F117 checks (native on-foot VR): the panel cursor's place on a turned panel (native_on_foot_f117.h). Header only, run by native_frame_test (run117).
// A curved panel 0.65 m ahead in the pick's frame, built as the game builds a pick mesh (triangles with their uvs), upright or turned 180 degrees about
// its middle (the F114b flight's turned meshes: u along -x, v along +y in the same place), and the sprite the game places at the window's uv on it.
#pragma once

#include "../../src/d3d11/native_on_foot_f117.h"

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace f117test {

namespace z = edvr::nofoot::f117;

// The panel's point at (u, v): 1.6 m wide, 0.9 m high, centred a little up and left of the view axis, curved about a vertical axis.
inline void panelPoint(bool turnedMesh, double u, double v, double p[3]) {
    const double su = turnedMesh ? -1.0 : 1.0;
    const double x = su * (u - 0.5) * 1.6, y = su * (0.5 - v) * 0.9;
    p[0] = -0.02 + x;
    p[1] = 0.05 + y;
    p[2] = 0.65 - 0.15 * x * x;
}

inline std::vector<z::Tri> mesh(bool turnedMesh, int nu = 8, int nv = 4) {
    std::vector<z::Tri> out;
    auto vert = [&](z::Tri& t, int k, double u, double v) {
        double p[3];
        panelPoint(turnedMesh, u, v, p);
        for (int i = 0; i < 3; ++i) t.v[k][i] = static_cast<float>(p[i]);
        t.v[k][3] = 1.0f;
        t.uv[k][0] = static_cast<float>(u);
        t.uv[k][1] = static_cast<float>(v);
    };
    for (int i = 0; i < nu; ++i)
        for (int j = 0; j < nv; ++j) {
            const double u0 = double(i) / nu, u1 = double(i + 1) / nu, v0 = double(j) / nv, v1 = double(j + 1) / nv;
            z::Tri a{}, b{};
            vert(a, 0, u0, v0);
            vert(a, 1, u1, v0);
            vert(a, 2, u0, v1);
            vert(b, 0, u1, v0);
            vert(b, 1, u1, v1);
            vert(b, 2, u0, v1);
            out.push_back(a);
            out.push_back(b);
        }
    return out;
}

// The sprite the game places at (u, v): the mesh's barycentric point (0x452EC00).
inline bool spriteAt(const std::vector<z::Tri>& m, double u, double v, float s[3]) {
    double b[3], p[3];
    const int i = z::findTri(m.data(), m.size(), u, v, b);
    if (i < 0) return false;
    z::pointAt(m[static_cast<size_t>(i)], b, p);
    for (int k = 0; k < 3; ++k) s[k] = static_cast<float>(p[k]);
    return true;
}

inline double dist(const double a[3], const double b[3]) {
    return std::sqrt((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2]));
}

template <class Check>
inline void run117(Check&& check) {
    const std::vector<z::Tri> up = mesh(false), turned = mesh(true);
    {   // the game's own point and the turn read from a triangle
        float s[3];
        double b[3], du[3], dv[3];
        const bool ok = spriteAt(up, 0.31, 0.62, s);
        double want[3];
        panelPoint(false, 0.31, 0.62, want);
        const double got[3] = {s[0], s[1], s[2]};
        const int iu = z::findTri(up.data(), up.size(), 0.31, 0.62, b), it = z::findTri(turned.data(), turned.size(), 0.31, 0.62, b);
        const bool jt = z::jacobian(turned[static_cast<size_t>(it)], du, dv) && z::turned(du, dv);
        const bool ju = z::jacobian(up[static_cast<size_t>(iu)], du, dv) && !z::turned(du, dv) && du[0] > 0.0 && dv[1] < 0.0;
        check(ok && dist(got, want) < 0.01 && jt && ju,
              "F117: the mesh's point at the uv is the triangle's barycentric point (within 1 cm of the curved panel); the turned mesh runs u along -x and "
              "v along +y, the upright one along +x and -y");
    }
    {   // an upright panel is left alone, whatever the setting
        float s[3];
        spriteAt(up, 0.7, 0.2, s);
        const z::Decision d = z::decide(up.data(), up.size(), 0.7, 0.2, s, z::Mode::kPanel);
        check(d.why == z::Why::kUpright && !d.turned, "F117: an upright panel's cursor is left at the game's place");
    }
    {   // panel: the turned mesh's point at (1 - u, 1 - v) is where the uv is on the panel as drawn (the upright one, in the same place)
        float s[3];
        spriteAt(turned, 0.7, 0.2, s);
        const z::Decision d = z::decide(turned.data(), turned.size(), 0.7, 0.2, s, z::Mode::kPanel);
        double drawn[3];
        panelPoint(false, 0.7, 0.2, drawn);
        check(d.why == z::Why::kMoved && d.turned && dist(d.target, drawn) < 0.01,
              "F117: panel: on a turned mesh the cursor goes to the mesh's point at (1 - u, 1 - v), the uv's place on the panel as drawn");
    }
    {   // view: mirrored through the view's centre; game: left, but counted as turned
        float s[3];
        spriteAt(turned, 0.7, 0.2, s);
        const z::Decision v = z::decide(turned.data(), turned.size(), 0.7, 0.2, s, z::Mode::kView);
        const z::Decision g = z::decide(turned.data(), turned.size(), 0.7, 0.2, s, z::Mode::kGame);
        check(v.why == z::Why::kMoved && std::fabs(v.target[0] + s[0]) < 1e-6 && std::fabs(v.target[1] + s[1]) < 1e-6 && std::fabs(v.target[2] - s[2]) < 1e-6 &&
                  g.why == z::Why::kGame && g.turned,
              "F117: view: the cursor's x and y negated (mirrored through the view's centre); game: the game's place, counted as turned");
    }
    {   // the guards: the sprite not at the uv, no triangle, the uv clamped as the game clamps it
        float s[3];
        spriteAt(turned, 0.4, 0.4, s);
        float off[3] = {s[0] + 0.05f, s[1], s[2]};
        const z::Decision offUv = z::decide(turned.data(), turned.size(), 0.4, 0.4, off, z::Mode::kPanel);
        const std::vector<z::Tri> none;
        const z::Decision empty = z::decide(none.data(), 0, 0.4, 0.4, s, z::Mode::kPanel);
        float edge[3];
        spriteAt(turned, 1.0, 0.0, edge);
        const z::Decision clamp = z::decide(turned.data(), turned.size(), 1.3, -0.2, edge, z::Mode::kPanel);
        double corner[3];
        panelPoint(false, 1.0, 0.0, corner);
        check(offUv.why == z::Why::kNotAtUv && empty.why == z::Why::kNoTriangle && clamp.why == z::Why::kMoved && dist(clamp.target, corner) < 0.01,
              "F117: a sprite 5 cm off the mesh's point at the uv is refused, no triangle is refused, a uv past the edge is clamped as the game clamps it");
    }
    {   // the instrument: a mouse path over the turned panel; panel follows the uv, game moves against it; an upright panel follows it either way
        auto path = [&](const std::vector<z::Tri>& m, z::Mode mode) {
            z::Sec s;
            z::Last last;
            for (int i = 0; i < 20; ++i) {
                const double u = 0.2 + 0.03 * i, v = 0.3 + (i < 10 ? 0.02 * i : 0.2 - 0.02 * (i - 10));
                float sp[3];
                spriteAt(m, u, v, sp);
                const z::Decision d = z::decide(m.data(), m.size(), u, v, sp, mode);
                const double shown[3] = {d.why == z::Why::kMoved ? d.target[0] : sp[0], d.why == z::Why::kMoved ? d.target[1] : sp[1],
                                         d.why == z::Why::kMoved ? d.target[2] : sp[2]};
                z::add(s, last, d, u, v, shown);
            }
            return s;
        };
        const z::Sec fixed = path(turned, z::Mode::kPanel), game = path(turned, z::Mode::kGame), upright = path(up, z::Mode::kPanel);
        check(fixed.moved == 20 && z::pairSign(fixed.turnedUv) > 0 && std::string(z::cursorText(fixed)) == "follows the uv" && game.moved == 0 &&
                  game.turned == 20 && std::string(z::cursorText(game)) == "against the uv" && upright.upright == 20 && z::pairSign(upright.uprightUv) > 0 &&
                  std::string(z::cursorText(upright)) == "no turned panel",
              "F117: over a mouse path, panel's cursor follows the uv on the turned panel, game's moves against it (the flight's report), an upright panel "
              "follows it untouched");
    }
}

}  // namespace f117test
