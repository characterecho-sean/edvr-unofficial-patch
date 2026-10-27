// F118 checks (native on-foot VR, measure only): why the aim reticle is seen double. Header only, run by native_frame_test (run118). The readback steps and
// flips, the drawn depth's trailing frames, the footprint's four directions, and the footprint decoded from lines marched through a depth image (the CPU
// procedure the depth shader runs) over an edge and a wall.
#pragma once

#include "../../src/d3d11/native_on_foot_f118.h"

#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <vector>

namespace f118test {

namespace f118 = edvr::nofoot::f118;
namespace f86 = edvr::nofoot::f86;
namespace f85 = edvr::nofoot::f85;

inline double angleDeg(const double a[3], const double b[3]) {
    double c = f85::dot3(a, b) / (f85::len3(a) * f85::len3(b));
    c = c > 1.0 ? 1.0 : c < -1.0 ? -1.0 : c;
    return std::acos(c) * f85::kDeg;
}

template <class Check>
inline void run118(Check&& check) {
    {   // the disparity at 63 mm: 10 m against the sky's 50 m is just past the fusion limit, 3 m against it well past
        const double a = f118::disparityDeg(10.0, 50.0), b = f118::disparityDeg(3.0, 50.0), c = f118::disparityDeg(50.0, 10.0);
        check(std::fabs(a - 0.28877) < 1e-4 && std::fabs(b - 1.13110) < 1e-4 && a == c && f118::disparityDeg(0.0, 3.0) == 0.0 && a > f118::kStepDeg,
              "F118: disparity 10 m vs 50 m 0.289 deg, 3 m vs 50 m 1.131 deg, symmetric, 0 for a non-depth");
    }
    {   // steps and flips: a sky step and its undoing (one flip), a hit-hit step left standing past the window, then a hit-hit step back that is no flip
        f118::Steps s;
        s.read(false, 10.0);
        s.read(false, 10.0);
        s.read(true, 50.0);    // step, sky-hit
        s.read(false, 10.0);   // step, sky-hit, and the flip of the one before
        s.read(false, 10.1);
        s.read(false, 3.0);    // step, hit-hit
        for (int i = 0; i < 7; ++i) s.read(false, 3.0);
        s.read(false, 10.1);   // step, hit-hit; the 3 m step's window closed after six readbacks
        check(s.reads == 14 && s.steps == 4 && s.skyHit == 2 && s.hitHit == 2 && s.flips == 1 && std::fabs(s.largestDeg - f118::disparityDeg(3.0, 10.1)) < 1e-12,
              "F118: 14 readbacks give 4 steps over 0.25 deg (sky-hit 2, hit-hit 2) and 1 flip; a step back after the window is no flip");
        s.second();
        s.read(false, 10.1);
        check(s.reads == 1 && s.steps == 0 && s.flips == 0 && s.largestDeg < 1e-12, "F118: the last target carries over the second's end (no step from nothing)");
        f118::Steps f;
        for (double d : {10.0, 50.0, 10.0, 50.0, 10.0}) f.read(d == 50.0, d);
        check(f.steps == 4 && f.skyHit == 4 && f.flips == 3, "F118: a target flip-flopping between a hit and the sky: 4 steps, 3 of them flips");
        f118::Steps q;
        for (double d : {10.0, 11.0, 12.0, 11.0}) q.read(false, d);
        check(q.steps == 0 && q.flips == 0, "F118: a target drifting within the fusion limit makes no step");
    }
    {   // trailing frames: the drawn depth against the newest target, the longest run in ms, a run carried over the second's end
        f118::Trail t;
        t.frame(50.0, 10.0, 11.0);
        t.frame(50.0, 10.0, 11.0);
        t.frame(40.0, 10.0, 12.0);   // 0.271 deg: still over
        t.frame(20.0, 10.0, 11.0);   // 0.180 deg: the run ends
        t.frame(50.0, 10.0, 11.0);
        check(t.frames == 5 && t.over == 4 && std::fabs(t.longestMs - 34.0) < 1e-9 && std::fabs(t.worstDeg - f118::disparityDeg(50.0, 10.0)) < 1e-12,
              "F118: 4 of 5 frames trail the target past 0.25 deg; the longest run 34 ms");
        t.second();
        t.frame(50.0, 10.0, 11.0);
        check(t.frames == 1 && t.over == 1 && std::fabs(t.longestMs - 22.0) < 1e-9, "F118: a trailing run carries over the second's end (11 + 11 ms)");
    }
    {   // the footprint's directions: 0.6 deg off u toward right, left, up and down, from K's columns even when they are scaled and skewed
        const double u[3] = {0.0, 0.0, 1.0}, right[3] = {2.0, 0.0, 0.6}, up[3] = {0.2, 1.5, 0.3};
        double ring[f118::kRing][3];
        const bool ok = f118::ringDirs(u, right, up, ring);
        bool angles = ok;
        for (int q = 0; q < f118::kRing && ok; ++q) angles = angles && std::fabs(angleDeg(ring[q], u) - 0.6) < 1e-9 && std::fabs(f85::len3(ring[q]) - 1.0) < 1e-12;
        const bool sides = ok && ring[0][0] > 0.0 && ring[1][0] < 0.0 && ring[2][1] > 0.0 && ring[3][1] < 0.0 && std::fabs(angleDeg(ring[0], ring[1]) - 1.2) < 1e-9 &&
                           std::fabs(angleDeg(ring[0], ring[2]) - angleDeg(ring[1], ring[3])) < 1e-9;
        double bad[f118::kRing][3];
        const double along[3] = {0.0, 0.0, 3.0};
        check(ok && angles && sides && !f118::ringDirs(u, along, up, bad),
              "F118: the four ring lines are unit and 0.6 deg off the aim line (right, left, up, down; opposite pairs 1.2 deg apart); a right axis along u is refused");
    }
    {   // the footprint decoded from marched lines: a near wall's edge 1 px left of the aim (the centre on the sky), then a plain wall, then a line off the screen
        constexpr uint32_t W = 200, H = 200;
        constexpr float kNear = 0.1f;
        const float rows[16] = {1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, kNear, 0.0f};   // x/z, y/z, depth near/z
        std::vector<float> edge(W * H), wall(W * H, kNear / 10.0f);
        for (uint32_t y = 0; y < H; ++y)
            for (uint32_t x = 0; x < W; ++x) edge[y * W + x] = x < 99 ? kNear / 3.0f : 0.0f;   // a surface 3 m away left of column 99, the sky right of it
        f85::MarchView v;
        v.rows = rows;
        v.width = W;
        v.height = H;
        double A = 0.0, B = 0.0;
        const bool fit = f85::zwFit(rows, &A, &B);
        v.A = A;
        v.B = B;
        const double O[3] = {0.0, 0.0, 0.0}, u[3] = {0.0, 0.0, 1.0}, right[3] = {1.0, 0.0, 0.0}, up[3] = {0.0, 1.0, 0.0};
        double dirs[f118::kRing][3];
        f118::ringDirs(u, right, up, dirs);
        const auto marchAll = [&](const std::vector<float>& depth, f86::LineOut* centre, f86::LineOut ring[f118::kRing]) {
            v.depth = depth.data();
            *centre = f86::marchRangeCpu(v, O, u, f85::kReticleMinM, f85::kReticleMaxM);
            for (int q = 0; q < f118::kRing; ++q) ring[q] = f86::marchRangeCpu(v, O, dirs[q], f85::kReticleMinM, f85::kReticleMaxM);
        };
        f86::LineOut c, r[f118::kRing];
        marchAll(edge, &c, r);
        const f118::Footprint fe = f118::footprint(c, r);
        const bool edgeLines = c.status == f86::kLineMiss && r[1].status == f86::kLineHit && std::fabs(r[1].d - 3.0) < 0.01 && r[0].status == f86::kLineMiss &&
                               r[2].status == f86::kLineMiss && r[3].status == f86::kLineMiss;
        check(fit && edgeLines && fe.ok && std::fabs(fe.spanDeg - f118::disparityDeg(r[1].d, 50.0)) < 1e-9 && fe.spanDeg > 1.1 && fe.centreFar,
              "F118: over an edge the centre line finds the sky and the left ring line the 3 m surface: the footprint spans 1.13 deg, the centre on the far side");
        marchAll(wall, &c, r);
        const f118::Footprint fw = f118::footprint(c, r);
        check(fw.ok && fw.spanDeg < 0.01 && !fw.centreFar && std::fabs(c.d - 10.0) < 0.01,
              "F118: on a wall 10 m away all five lines hit within 0.01 deg of each other (not an edge)");
        f86::LineOut off[f118::kRing] = {r[0], r[1], r[2], r[3]};
        off[2].status = f86::kLineOffscreen;
        const f118::Footprint fo = f118::footprint(c, off);
        f118::FootSecond s;
        s.add(fe);
        s.add(fw);
        s.add(fo);
        check(!fo.ok && s.read == 2 && s.edge == 1 && s.centreFar == 1 && s.incomplete == 1 && std::fabs(s.largestDeg - fe.spanDeg) < 1e-12,
              "F118: a second's footprints: 2 read (1 over an edge, its centre on the far side), 1 with a line off the screen counted incomplete");
    }
}

}  // namespace f118test
