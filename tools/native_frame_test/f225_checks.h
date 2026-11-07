// F225 checks (native on-foot VR), run225 below. Header only, run by native_frame_test.
//
// F225 (native_on_foot_f225.h, native_on_foot.cpp): the shoulder's reach margin ramps. The part of the assist's move past 0.97 of the reach stays immediate; the 3 cm margin goes in over 0.2 s and out
// over 0.4 s. The rig sweeps the distance across the threshold and back at the hook's 11 ms (the move changes by the margin's ramp and the change in the excess, never by 3 cm), compares the move with
// ikAssistReachMove at full weight over a grid, checks the reach is never passed at any weight, the margin's timing and bad times, a distance flickering 2 mm either side, the line, and pins the sites.
#pragma once

#include "../../src/d3d11/native_on_foot_core.h"
#include "../../src/d3d11/native_on_foot_f225.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>

namespace f225test {

namespace nf = edvr::nofoot;
namespace f225 = edvr::nofoot::f225;

inline std::string slurp225(const char* path) {
    std::ifstream f(path, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

template <class Check>
inline void run225(Check&& check) {
    char msg[400];
    const double l1 = 0.30, l2 = 0.28, thr = nf::kIkAssistReach * (l1 + l2), dt = 0.011;
    {   // a sweep across the threshold and back
        f225::Margin mg;
        double prevMove = 0.0, prevOver = 0.0, worstStep = 0.0, worstExtra = 0.0;
        bool atOrUnder = true;
        for (int n = 0; n <= 80; ++n) {
            const double dist = thr - 0.015 + 0.001 * (n <= 40 ? n : 80 - n) * 1.0;   // 0.015 under to 0.025 over, and back, 1 mm a call
            const double over = dist - thr;
            mg.step(over > 0.0, dt);
            double ex = 0.0;
            const double move = f225::reachMove(dist, l1, l2, mg.w, &ex);
            const double overPos = over > 0.0 ? over : 0.0;
            if (n > 0) {
                worstStep = std::fmax(worstStep, std::fabs(move - prevMove));
                worstExtra = std::fmax(worstExtra, std::fabs((move - prevMove) - (overPos - prevOver)));
            }
            atOrUnder = atOrUnder && dist - move <= thr + 1e-12;
            prevMove = move;
            prevOver = overPos;
        }
        std::snprintf(msg, sizeof(msg), "F225: across the threshold the move never steps more than %.2f mm a call (the margin's own part %.2f mm), never 3 cm", worstStep * 1000.0, worstExtra * 1000.0);
        check(worstStep < 0.004 && worstExtra < 0.002 && atOrUnder, msg);
    }
    {   // full weight over the reach is ikAssistReachMove, held to 10 cm; the reach is never passed
        bool same = true, held = true, reach = true;
        char bad[200] = "";
        for (int ia = 0; ia <= 4; ++ia)
            for (int ib = 0; ib <= 4; ++ib)
                for (int io = -4; io <= 40; ++io) {
                    const double a = 0.20 + 0.05 * ia, b = 0.20 + 0.05 * ib, over = 0.005 * io;
                    const double dist = nf::kIkAssistReach * (a + b) + over;
                    double e1 = 0.0, e2 = 0.0;
                    const double m1 = nf::ikAssistReachMove(dist, a, b, &e1), m2 = f225::reachMove(dist, a, b, 1.0, &e2);
                    if (dist - nf::kIkAssistReach * (a + b) > 0.0 && (m1 != m2 || e1 != e2)) {
                        same = false;
                        std::snprintf(bad, sizeof(bad), "same: a %.2f b %.2f over %.3f: %.17g vs %.17g, excess %.17g vs %.17g", a, b, over, m1, m2, e1, e2);
                    }
                    held = held && m2 <= nf::kIkAssistMaxM;
                    for (int iw = 0; iw <= 4; ++iw) {
                        double e3 = 0.0;
                        const double w = 0.25 * iw, m3 = f225::reachMove(dist, a, b, w, &e3);
                        if (over <= 0.07 + 1e-9 && dist - m3 > nf::kIkAssistReach * (a + b) + 1e-12) {
                            reach = false;
                            std::snprintf(bad, sizeof(bad), "reach: a %.2f b %.2f over %.3f w %.2f move %.6f", a, b, over, w, m3);
                        }
                    }
                }
        double ex0 = -1.0;
        std::snprintf(msg, sizeof(msg), "F225: at weight 1 over the reach the move is ikAssistReachMove's exactly, held to 10 cm, and the arm is never moved past 0.97 of the reach %s", bad);
        check(same && held && reach, msg);
        check(f225::reachMove(thr - 0.001, l1, l2, 0.0, &ex0) == 0.0 && ex0 == 0.0 && f225::reachMove(thr - 0.001, l1, l2, 1.0, &ex0) == nf::kIkAssistMarginM, "F225: at weight 0 in reach the move is 0; the margin alone is 3 cm at weight 1");
        check(std::fabs(f225::reachMove(thr + 0.001, l1, l2, std::nan(""), &ex0) - 0.001) < 1e-12 && f225::reachMove(thr + 0.001, l1, l2, 2.0, &ex0) == f225::reachMove(thr + 0.001, l1, l2, 1.0, &ex0), "F225: a weight that is not a number reads 0 and one over 1 reads 1");
    }
    {   // the margin's timing and bad times
        f225::Margin mg;
        for (int i = 0; i < 19; ++i) mg.step(true, 0.01);
        const bool notYet = mg.w < 1.0 - 1e-9;
        for (int i = 0; i < 2; ++i) mg.step(true, 0.01);
        check(notYet && mg.w >= 1.0 - 1e-9, "F225: the weight goes in over 0.2 s");
        for (int i = 0; i < 39; ++i) mg.step(false, 0.01);
        const bool notZero = mg.w > 1e-9;
        for (int i = 0; i < 2; ++i) mg.step(false, 0.01);
        check(notZero && mg.w <= 1e-9, "F225: and out over 0.4 s");
        mg.step(true, 0.05);
        const double w = mg.w;
        mg.step(true, 0.0);
        mg.step(true, -1.0);
        mg.step(true, std::nan(""));
        mg.step(false, std::numeric_limits<double>::infinity());
        check(mg.w == w, "F225: a dt that is nothing, negative, not a number or infinite leaves the weight alone");
        mg.reset();
        check(mg.w == 0.0, "F225: reset empties it");
    }
    {   // a distance flickering 2 mm either side of the threshold
        f225::Margin mg;
        double prev = 0.0, worst = 0.0;
        for (int n = 0; n < 200; ++n) {
            const double dist = thr + (n % 2 == 0 ? 0.002 : -0.002);
            mg.step(dist > thr, dt);
            double ex = 0.0;
            const double move = f225::reachMove(dist, l1, l2, mg.w, &ex);
            if (n >= 4) worst = std::fmax(worst, std::fabs(move - prev));
            prev = move;
        }
        std::snprintf(msg, sizeof(msg), "F225: a distance flickering 2 mm either side of the threshold moves the result by %.2f mm a call at most (2 mm of excess plus the margin's 1.65 mm ramp, against a 3 cm step before)", worst * 1000.0);
        check(worst < 0.004, msg);
    }
    {   // the counts and the line
        f225::Stats st;
        check(!st.any(), "F225: an empty window has nothing to say");
        st.add(true, 1.0, 4.0);
        st.add(true, 0.5, 2.5);
        st.add(false, 0.0, 0.0);
        char line[300];
        f225::formatLine(line, sizeof(line), 10, st);
        check(std::string(line) == "native on foot reach margin (F225) 10 s: 3 armed solves, over the reach on 2, margin weight mean 0.50; the shoulder's assist move largest 4.0 cm", line);
    }
    {   // the sites
        const std::string cpp = slurp225("src/d3d11/native_on_foot.cpp");
        const auto has = [&](const char* needle) { return cpp.find(needle) != std::string::npos; };
        check(has("if (armedNow) {\n                if (doSolveWrite) s_f225[side].step(distTS - nofoot::kIkAssistReach * (l1 + l2) > 0.0, supportDt);\n                else s_f225[side].reset();\n                move = nofoot::f225::reachMove(distTS, l1, l2, s_f225[side].w, &excess);") &&
                  has("} else {\n                s_f225[side].reset();\n                move = nofoot::ikAssistReachMove(distTS, l1, l2, &excess);"),
              "F225: armed the assist uses the ramped margin, stepped on a solve that writes; not armed it is ikAssistReachMove as it was and the margin is forgotten");
        check(has("g_f225Box.write(") && has("nofoot::f225::formatLine(marginLine") && has("g_f225Box.clearPending.store(true"), "F225: the window's line is fed by the left arm's armed solves and cleared at an edge");
    }
}

}  // namespace f225test
