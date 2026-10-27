// F114b checks (native on-foot VR and flat): the panel pointer's second instrument (native_on_foot_f114b.h). Header only, run by native_frame_test (run114b).
// A modelled panel: the eye at the origin looking along +z, the pick's camera, the mesh owner's world rows and the mesh's uv-to-local layout, each of
// which can be turned 180 degrees about the view axis; the mouse moves the window's uv, the sprite lands where the chain puts it, and the verdict must
// name the turned transform from the instrument's own readings (the owner's rows and its transform in the camera's frame, the eye's view rows).
#pragma once

#include "../../src/d3d11/native_on_foot_f114b.h"

#include <cmath>
#include <cstdint>
#include <cstring>

namespace f114btest {

namespace q = edvr::nofoot::f114b;
namespace p = edvr::nofoot::f111;

struct World {
    double E[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};   // the eye's view rows (its axes)
    double e[3] = {0, 0, 0};
    double C[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};   // the pick camera's rows
    double c[3] = {0.0, -0.05, 0.02};
    double O[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};   // the owner's world rows (its axes, scaled by the panel's size below)
    double o[3] = {0.02, 0.05, 0.65};
    double mesh = 1.0;                           // the uv-to-local layout: +1 u along +x and v along -y, -1 turned
    bool eyeRead = true;
};

inline void roll180(double R[9]) {
    for (int k = 0; k < 6; ++k) R[k] = -R[k];   // the x and y axes negated: 180 degrees about z
}
inline void yawDeg(double R[9], double deg) {
    const double a = deg * 3.14159265358979323846 / 180.0, cs = std::cos(a), sn = std::sin(a);
    const double Y[9] = {cs, 0, -sn, 0, 1, 0, sn, 0, cs};
    double out[9];
    for (int r = 0; r < 3; ++r)
        for (int k = 0; k < 3; ++k) out[r * 3 + k] = R[r * 3] * Y[k] + R[r * 3 + 1] * Y[3 + k] + R[r * 3 + 2] * Y[6 + k];
    std::memcpy(R, out, sizeof(out));
}

// One cycle at the mouse's NDC (x right, y down): the window's uv, the sprite on the panel in the pick camera's frame, the owner transform as the game
// returns it (the owner's axes dotted with the camera's), closed as the instrument closes it.
inline void cycleAt(const World& w, float x, float y, p::Cycle& c, q::Extra& ext, uint8_t press = 0) {
    c = p::Cycle{};
    c.ndc[0] = x;
    c.ndc[1] = y;
    c.source = 1;
    c.placements = 13;
    c.windowCalls = 1;
    const double u = 0.5 + 0.4 * x, v = 0.5 + 0.4 * y;
    c.win[0] = static_cast<float>(u);
    c.win[1] = static_cast<float>(v);
    c.winUv = true;
    c.hit = true;
    const double size[2] = {1.6, 0.9};
    const double local[3] = {w.mesh * (u - 0.5) * size[0], w.mesh * (0.5 - v) * size[1], 0.0};
    double world[3], cam[3];
    q::frameToWorld(w.O, w.o, local, world);
    q::worldToFrame(w.C, w.c, world, cam);
    for (int k = 0; k < 3; ++k) c.pos[k] = static_cast<float>(cam[k]);
    ext = q::Extra{};
    ext.owner = true;
    for (int r = 0; r < 3; ++r) {
        double axis[3] = {w.O[r * 3], w.O[r * 3 + 1], w.O[r * 3 + 2]};
        for (int k = 0; k < 3; ++k) {
            ext.owner16[r * 4 + k] = static_cast<float>(axis[k]);
            ext.out16[r * 4 + k] = static_cast<float>(w.C[k * 3] * axis[0] + w.C[k * 3 + 1] * axis[1] + w.C[k * 3 + 2] * axis[2]);
        }
    }
    double oc[3];
    q::worldToFrame(w.C, w.c, w.o, oc);
    for (int k = 0; k < 3; ++k) {
        ext.owner16[12 + k] = static_cast<float>(w.o[k]);
        ext.out16[12 + k] = static_cast<float>(oc[k]);
    }
    ext.pressed = press;
    ext.live = true;
    const double hud[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};   // the drawn canvas, upright for the eye
    q::closeCycle(ext, c, w.eyeRead ? w.E : nullptr, w.e, hud);
}

inline q::Sec run(const World& w, bool still = false) {
    q::Sec s;
    p::Cycle prev, cur;
    q::Extra pe, ce;
    bool have = false;
    float x = 0.0f, y = 0.0f;
    for (int i = 0; i < 28; ++i) {
        if (!still) {
            if (i >= 4 && i < 12) y -= 0.05f;        // up
            else if (i >= 12 && i < 20) x += 0.05f;  // right
            else if (i >= 20) {
                x -= 0.025f;
                y += 0.025f;
            }
        }
        cycleAt(w, x, y, cur, ce, i == 24 ? 1 : 0);
        q::addCycle(s, cur, ce);
        if (have) q::addStep(s, prev, pe, cur, ce);
        prev = cur;
        pe = ce;
        have = true;
    }
    return s;
}

template <class Check>
inline void run114b(Check&& check) {
    {   // the camera from the owner's rows and its transform in the camera's frame, with the owner scaled and the camera turned and yawed
        World w;
        roll180(w.C);
        yawDeg(w.C, 23.0);
        for (int k = 0; k < 9; ++k) w.O[k] *= (k < 3 ? 1.6 : k < 6 ? 0.9 : 1.0);
        yawDeg(w.O, -11.0);
        p::Cycle c;
        q::Extra x;
        cycleAt(w, 0.1f, 0.2f, c, x);
        double err = 0.0;
        for (int k = 0; k < 9; ++k) err = std::fmax(err, std::fabs(x.C[k] - w.C[k]));
        for (int k = 0; k < 3; ++k) err = std::fmax(err, std::fabs(x.c[k] - w.c[k]));
        check(x.cam && err < 1e-5 && x.ortho < 1e-5 && x.camEye && x.camEyeCos[2] > 0.9,
              "F114b: the camera's world rows and position follow from the owner's rows and the transform it returns in the camera's frame (owner scaled "
              "1.6 x 0.9, camera turned and yawed), orthogonal");
    }
    {
        const q::Sec s = run(World{});
        check(q::turn(s) == q::Turn::kNone && q::pairSign(s.uvCam) > 0 && q::pairSign(s.uvLocal) > 0 && q::pairSign(s.uvEye) > 0 &&
                  q::pairSign(s.ndcEye) > 0 && std::strcmp(q::modeText(s), "2D window") == 0 && s.windowPlacements == 28 && s.moves == 24,
              "F114b: everything upright: the sprite follows the uv in the camera, in the owner's axes and for the eye; 'none', the 2D mode");
    }
    {
        World w;
        roll180(w.C);
        const q::Sec s = run(w);
        check(q::turn(s) == q::Turn::kCamera && q::pairSign(s.uvCam) < 0 && q::pairSign(s.uvEye) > 0 && s.camEyeMean(0) < -0.99 &&
                  s.camEyeMean(1) < -0.99 && s.camEyeMean(2) > 0.99,
              "F114b: the pick camera turned 180 degrees against the eye: turned in the pick's frame, upright for the eye: 'the pick camera'");
    }
    {
        World w;
        roll180(w.O);
        const q::Sec s = run(w);
        check(q::turn(s) == q::Turn::kOwner && q::pairSign(s.uvCam) < 0 && q::pairSign(s.uvEye) < 0 && q::pairSign(s.uvLocal) > 0 &&
                  s.ownerHudMean(0) < -0.99 && s.ownerHudMean(1) < -0.99 && q::pairSign(s.ndcEye) < 0,
              "F114b: the owner's world rows turned against the drawn canvas: the mesh upright in its own axes, turned for the eye: 'the owner's world "
              "transform'; the sprite as the eye sees it moves against the mouse");
    }
    {
        World w;
        w.mesh = -1.0;
        const q::Sec s = run(w);
        check(q::turn(s) == q::Turn::kMesh && q::pairSign(s.uvLocal) < 0 && q::pairSign(s.uvEye) < 0 && s.ownerHudMean(0) > 0.99,
              "F114b: the mesh's uv laid out turned in the owner's own axes: 'the mesh data', the owner upright against the canvas");
    }
    {
        World w;
        w.mesh = -1.0;
        w.eyeRead = false;
        const q::Sec s = run(w);
        check(q::turn(s) == q::Turn::kTurnedEyeUnread && std::strcmp(q::drawText(s), "the eye not read") == 0,
              "F114b: no eye's view (the flat profile): 'turned in the pick's frame (the eye not read)'");
    }
    {
        const q::Sec still = run(World{}, true);
        q::Sec none;
        check(q::turn(still) == q::Turn::kFewMoves && q::turn(none) == q::Turn::kNoPanel && std::strcmp(q::modeText(none), "no panel") == 0,
              "F114b: a still cursor reads too few moves; no panel reads no panel");
    }
    {
        const q::Sec s = run(World{});
        check(s.presses[0] == 1 && s.pressesWithUv == 1 && std::fabs(s.lastPressUv[0] - (0.5 + 0.4 * 0.275)) < 1e-5 && s.liveCycles == 28 &&
                  q::buttonEdge(1, 0) == 1 && q::buttonEdge(0, 1) == 2 && q::buttonEdge(1, 1) == 0 && q::buttonEdge(0, 0) == 0,
              "F114b: a button going down (now 1, before 0) is a press, counted with the cycle's uv; up is a release; held or idle is neither");
    }
    {
        const uint8_t zero[3] = {0x33, 0xC0, 0xC3}, imm[6] = {0xB8, 0x02, 0, 0, 0, 0xC3}, dw8[4] = {0x8B, 0x41, 0x18, 0xC3},
                      dw32[7] = {0x8B, 0x81, 0x30, 0x01, 0, 0, 0xC3}, by8[5] = {0x0F, 0xB6, 0x41, 0x0C, 0xC3}, by8b[4] = {0x8A, 0x41, 0x10, 0xC3},
                      call[6] = {0x48, 0x83, 0xEC, 0x28, 0xE8, 0x00};
        int32_t v1 = 9, v2 = 0, v3 = 0, v4 = 0, v5 = 0, v6 = 0, v7 = 0;
        check(q::decodeModeGetter(zero, 3, &v1) == q::Getter::kConst && v1 == 0 && q::decodeModeGetter(imm, 6, &v2) == q::Getter::kConst && v2 == 2 &&
                  q::decodeModeGetter(dw8, 4, &v3) == q::Getter::kDwordField && v3 == 0x18 &&
                  q::decodeModeGetter(dw32, 7, &v4) == q::Getter::kDwordField && v4 == 0x130 &&
                  q::decodeModeGetter(by8, 5, &v5) == q::Getter::kByteField && v5 == 0x0C &&
                  q::decodeModeGetter(by8b, 4, &v6) == q::Getter::kByteField && v6 == 0x10 && q::decodeModeGetter(call, 6, &v7) == q::Getter::kUnknown,
              "F114b: the mode getter is read only as a plain accessor (a constant, a byte or dword field of the object); a getter that calls is not");
    }
}

}  // namespace f114btest
