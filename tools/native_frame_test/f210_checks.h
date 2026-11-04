// F210 checks (native on-foot VR): the full-detail scope's circle on a Quest 3 and any other eye that is not square and a frustum that is not symmetric. Header only, run by native_frame_test (run210).
//
// The d3d11 half's part of the chain (the runtime's part, the pixels the share shader blacks, is measured in openxr_stereo_test's circleAspectSelfTest): the circle's centre is the direction of the
// sight's far point in each eye's own view, two tangents a tangent plane each (view_circle.h). circleTick works it out from the game's own scene rows: sightPointNdc puts the far point in NDC,
// clipZoom/tangentsOf read the tangents that NDC square spans from the same rows, tangentOfNdc turns the NDC into the tangent. This rig builds the rows the way a left-handed D3D projection
// with a principal offset gives them, for the Quest 3's frusta (eye 0: left -53.42, right +39.41, up +41.99, down -53.08 degrees, from the runtime's `view fov:` line of 2026-10-31; eye 1 the
// horizontal mirror; and the earlier session's up 0.9657 / down 1.4281), the Pimax's and a synthetic one, an eye turned and tilted in the world, TAA jitter in the offsets, the left eye
// narrowed to the scope zoom's window, and checks that the tangent that comes out is the tangent of the direction, to rounding: nothing in it depends on the eye's pixel aspect or on how
// asymmetric the frustum is. Also the numbers the pilot's "not centred" is made of: where the forward axis (and so a sight on it) lands in a Quest 3 eye's image.
#pragma once

#include "../../src/common/view_circle.h"
#include "../../src/common/scope_zoom_window.h"
#include "../../src/d3d11/native_on_foot_core.h"
#include "../../src/d3d11/scope_zoom_s0.h"

#include <cmath>
#include <cstdint>
#include <cstring>

namespace f210test {

namespace sz = edvr::scopezoom;

struct Frustum {
    const char* name;
    double l, r, u, d;   // tangents: left and down negative
};

// An eye's scene rows (row-vector form, clip_j = (p - o) . rows[0..11] + rows[12 + j]) for a camera with right r, up u, forward f (unit, orthogonal), a projection that maps the tangents l..r and d..u to
// NDC -1..1 (ndc = p * tangent + off, off = -(r + l) / (r - l)), and TAA jitter (jx, jy) added to the offsets.
inline void buildRows(const Frustum& fr, const double right[3], const double up[3], const double fwd[3], double jx, double jy, float rows[16]) {
    const double p00 = 2.0 / (fr.r - fr.l), p11 = 2.0 / (fr.u - fr.d);
    const double offX = -(fr.r + fr.l) / (fr.r - fr.l) + jx, offY = -(fr.u + fr.d) / (fr.u - fr.d) + jy;
    for (int i = 0; i < 3; ++i) {
        rows[4 * i + 0] = static_cast<float>(p00 * right[i] + offX * fwd[i]);   // clip x
        rows[4 * i + 1] = static_cast<float>(p11 * up[i] + offY * fwd[i]);      // clip y
        rows[4 * i + 2] = static_cast<float>(1.0001 * fwd[i]);                   // clip z (any)
        rows[4 * i + 3] = static_cast<float>(fwd[i]);                            // clip w = the distance along forward
    }
    rows[12] = 0.0f;
    rows[13] = 0.0f;
    rows[14] = -0.1f;
    rows[15] = 0.0f;
}

inline void unit(double v[3]) {
    const double n = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    for (int i = 0; i < 3; ++i) v[i] /= n;
}
inline void cross(const double a[3], const double b[3], double out[3]) {
    out[0] = a[1] * b[2] - a[2] * b[1];
    out[1] = a[2] * b[0] - a[0] * b[2];
    out[2] = a[0] * b[1] - a[1] * b[0];
}
// A camera basis turned by yaw, then pitch, then roll (degrees) from the world axes (x right, y up, z forward).
inline void basis(double yawDeg, double pitchDeg, double rollDeg, double right[3], double up[3], double fwd[3]) {
    const double k = 3.14159265358979323846 / 180.0;
    const double cy = std::cos(yawDeg * k), sy = std::sin(yawDeg * k), cp = std::cos(pitchDeg * k), sp = std::sin(pitchDeg * k), cr = std::cos(rollDeg * k), sr = std::sin(rollDeg * k);
    double f[3] = {sy * cp, sp, cy * cp};   // forward
    double r0[3] = {cy, 0.0, -sy};          // right before the roll
    double u0[3];
    cross(f, r0, u0);
    unit(u0);
    for (int i = 0; i < 3; ++i) {
        fwd[i] = f[i];
        right[i] = cr * r0[i] + sr * u0[i];
        up[i] = -sr * r0[i] + cr * u0[i];
    }
}

// circleTick's chain for one eye: the sight line from the aiming eye's origin E through S, the far point kFarM past S, seen from this eye's origin o through its rows: the tangent that comes out.
inline bool circleTangent(const float rows[16], const double o[3], const double S[3], const double E[3], double farM, double tc[2]) {
    const double dir[3] = {S[0] - E[0], S[1] - E[1], S[2] - E[2]};
    double ndc[2];
    float tj[4];
    if (!edvr::nofoot::sightPointNdc(rows, o, S, dir, farM, ndc) || !sz::tangentsOf(sz::clipZoom(rows), tj)) return false;
    sz::tangentOfNdc(ndc, tj, tc);
    return std::isfinite(tc[0]) && std::isfinite(tc[1]);
}

template <class Check>
inline void run210(Check&& check) {
    const Frustum quest0 = {"Quest 3 eye 0", -std::tan(53.42 * 3.14159265358979323846 / 180.0), std::tan(39.41 * 3.14159265358979323846 / 180.0), std::tan(41.99 * 3.14159265358979323846 / 180.0),
                            -std::tan(53.08 * 3.14159265358979323846 / 180.0)};
    const Frustum quest1 = {"Quest 3 eye 1", -quest0.r, -quest0.l, quest0.u, quest0.d};
    const Frustum earlier0 = {"Quest 3 (the earlier session) eye 0", quest0.l, quest0.r, 0.9657, -1.4281};
    const Frustum pimax0 = {"Pimax eye 0", -1.2543, 0.8746, 1.2648, -1.2648};
    const Frustum synth = {"synthetic", -std::tan(62.0 * 3.14159265358979323846 / 180.0), std::tan(28.0 * 3.14159265358979323846 / 180.0), std::tan(25.0 * 3.14159265358979323846 / 180.0),
                           -std::tan(47.0 * 3.14159265358979323846 / 180.0)};
    const Frustum all[] = {quest0, quest1, earlier0, pimax0, synth};
    // ---- 1. the tangent that comes out is the tangent of the direction ------------------------------------------------------------------------------------------------------------------------------
    {
        double worst = 0.0;
        unsigned n = 0;
        for (const Frustum& fr : all)
            for (int turned = 0; turned < 3; ++turned) {
                double right[3], up[3], fwd[3];
                basis(turned == 0 ? 0.0 : turned == 1 ? 33.0 : -71.0, turned == 0 ? 0.0 : turned == 1 ? -9.0 : 14.0, turned == 2 ? 3.0 : 0.0, right, up, fwd);
                const double jit[2][2] = {{0.0, 0.0}, {0.0007, -0.0011}};
                for (const auto& j : jit) {
                    float rows[16];
                    buildRows(fr, right, up, fwd, j[0], j[1], rows);
                    const double o[3] = {12.5, 1.7, -40.0};
                    // a direction (tx, ty) in the eye's own frame across the frustum, the aiming eye's origin 3.2 cm to the right of this eye's
                    for (int iy = 0; iy < 7; ++iy)
                        for (int ix = 0; ix < 7; ++ix) {
                            const double tx = fr.l + (fr.r - fr.l) * (0.08 + 0.84 * ix / 6.0), ty = fr.d + (fr.u - fr.d) * (0.08 + 0.84 * iy / 6.0);
                            double dirW[3];
                            for (int i = 0; i < 3; ++i) dirW[i] = tx * right[i] + ty * up[i] + fwd[i];
                            unit(dirW);
                            double E[3], S[3];
                            for (int i = 0; i < 3; ++i) {
                                E[i] = o[i] + 0.032 * right[i];
                                S[i] = E[i] + 0.22 * dirW[i];
                            }
                            double tc[2];
                            if (!circleTangent(rows, o, S, E, 100.0, tc)) {
                                worst = 1e9;
                                continue;
                            }
                            // what it should be: the far point seen from this eye
                            double v[3];
                            for (int i = 0; i < 3; ++i) v[i] = (S[i] + 100.0 * dirW[i]) - o[i];
                            const double vf = v[0] * fwd[0] + v[1] * fwd[1] + v[2] * fwd[2];
                            const double wantX = (v[0] * right[0] + v[1] * right[1] + v[2] * right[2]) / vf, wantY = (v[0] * up[0] + v[1] * up[1] + v[2] * up[2]) / vf;
                            worst = std::fmax(worst, std::fmax(std::fabs(tc[0] - wantX), std::fabs(tc[1] - wantY)));
                            ++n;
                        }
                }
            }
        char what[300];
        std::snprintf(what, sizeof(what), "F210: the circle's centre (sightPointNdc, then the tangents the same rows span) is the tangent of the sight's far point in the eye's own frame, for the Quest 3's two eyes, the earlier "
                                           "Quest frustum, the Pimax's and a synthetic one, an eye turned and tilted, with and without TAA jitter: %u directions, worst error %.2e tangents", n, worst);
        check(n == 5u * 3u * 2u * 49u && worst < 2e-5, what);   // float rows: 1e-5 of a tangent is 0.0006 degrees
    }
    // ---- 2. the left eye narrowed to the scope zoom's window: its rows change, the centre does not ----------------------------------------------------------------------------------------------------
    {
        double right[3], up[3], fwd[3];
        basis(20.0, -5.0, 0.0, right, up, fwd);
        const double o[3] = {1.0, 2.0, 3.0};
        const double tx = -0.31, ty = 0.12;
        double dirW[3];
        for (int i = 0; i < 3; ++i) dirW[i] = tx * right[i] + ty * up[i] + fwd[i];
        unit(dirW);
        const double E[3] = {o[0], o[1], o[2]};
        const double S[3] = {E[0] + 0.22 * dirW[0], E[1] + 0.22 * dirW[1], E[2] + 0.22 * dirW[2]};
        const double trueTan[4] = {quest0.l, quest0.r, quest0.d, quest0.u};
        const double tcWindow[2] = {tx, ty};
        double zoomTan[4];
        const bool windowed = edvr::scopewin::window(trueTan, tcWindow, 2.4, zoomTan);
        const Frustum narrowed = {"Quest 3 eye 0 narrowed", zoomTan[0], zoomTan[1], zoomTan[3], zoomTan[2]};
        float rowsTrue[16], rowsZoom[16];
        buildRows(quest0, right, up, fwd, 0.0, 0.0, rowsTrue);
        buildRows(narrowed, right, up, fwd, 0.0, 0.0, rowsZoom);
        double a[2] = {}, b[2] = {};
        const bool okA = circleTangent(rowsTrue, o, S, E, 100.0, a), okB = circleTangent(rowsZoom, o, S, E, 100.0, b);
        check(windowed && okA && okB && std::fabs(a[0] - tx) < 2e-5 && std::fabs(a[1] - ty) < 2e-5 && std::fabs(b[0] - tx) < 2e-5 && std::fabs(b[1] - ty) < 2e-5,
              "F210: the left eye narrowed to the scope zoom's window (2.4x about the sight) gives the same centre as its true frustum: the centre is the sight's tangent whatever frustum the rows have");
    }
    // ---- 3. the wire: the centre rides the version 11 answer as two floats per eye, exactly -------------------------------------------------------------------------------------------------------
    {
        edvr::viewcircle::Request r;
        r.on = true;
        r.tc[0][0] = -0.0123457f;
        r.tc[0][1] = 0.0876543f;
        r.tc[1][0] = 0.1234567f;
        r.tc[1][1] = -0.4567891f;
        uint32_t w[edvr::viewcircle::kWords];
        edvr::viewcircle::toWords(r, w);
        const edvr::viewcircle::Request back = edvr::viewcircle::fromWords(w);
        check(back.on && std::memcmp(back.tc, r.tc, sizeof(r.tc)) == 0, "F210: the circle's centre survives the wire bit for bit, both eyes, both axes");
    }
    // ---- 4. where the forward axis lands in a Quest 3 eye: the numbers the pilot's "not centred" is made of ---------------------------------------------------------------------------------------------
    {
        const auto uv = [](const Frustum& fr, double tx, double ty, double out[2]) {
            out[0] = (tx - fr.l) / (fr.r - fr.l);
            out[1] = (fr.u - ty) / (fr.u - fr.d);
        };
        double q0[2], q1[2], pm[2];
        uv(quest0, 0.0, 0.0, q0);
        uv(quest1, 0.0, 0.0, q1);
        uv(pimax0, 0.0, 0.0, pm);
        check(std::fabs(q0[0] - 0.6210) < 5e-4 && std::fabs(q0[1] - 0.4034) < 5e-4 && std::fabs(q1[0] - 0.3790) < 5e-4 && std::fabs(q1[1] - 0.4034) < 5e-4 && std::fabs(pm[1] - 0.5) < 1e-12,
              "F210: a sight on the forward axis lands at (0.621, 0.403) of a Quest 3 left eye's image and (0.379, 0.403) of its right, that is 372 px right of and 315 px above the middle of a 3072 x 3264 image "
              "(the vertical frustum is up 41.99 / down 53.08 degrees: the picture's middle is 12.2 degrees below the axis); on the Pimax it is 0.5 vertically");
    }
}

}  // namespace f210test
