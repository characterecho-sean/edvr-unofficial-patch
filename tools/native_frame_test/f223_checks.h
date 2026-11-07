// F223 checks (native on-foot VR), run223 below. Header only, run by native_frame_test.
//
// F223 (native_on_foot_f223.h, native_on_foot.cpp): a jump of the camera in the model of 0.10 m or more with the camera in the world moving less than half of it is the model root jumping; it is added
// to a move (held to +-1.5 m) that joins F215's shift and eases back at 0.5 m/s otherwise. The rig feeds the recorded series of one jump pack start with a smooth camera, a camera that really moved,
// a step under 0.10 m, a frame that is not new, a gap past 0.1 s, the cap and a number that is not one, the line, and pins the three sum and subtraction sites.
#pragma once

#include "../../src/d3d11/native_on_foot_f223.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

namespace f223test {

namespace f223 = edvr::nofoot::f223;

inline std::string slurp223(const char* path) {
    std::ifstream f(path, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

template <class Check>
inline void run223(Check&& check) {
    char msg[400];
    const double up[3] = {0.0, 1.0, 0.0};
    const double dt = 1.0 / 90.0;
    {   // the recorded series, a smooth camera
        const double series[] = {1.60, 1.65, 1.70, 0.60, 0.92, 1.31, 1.54, 1.61, 1.60};
        f223::RootJump r;
        double lo = 1e9, hi = -1e9, t = 0.0;
        uint32_t taken = 0, refused = 0;
        for (double e : series) {
            const double cam[3] = {0.0, 1.70 + 0.001 * t, 0.0};
            const double pop = r.step(true, t, e, cam, up);
            taken += r.taken ? 1u : 0u;
            refused += r.refused ? 1u : 0u;
            lo = std::fmin(lo, e - pop);
            hi = std::fmax(hi, e - pop);
            t += dt;
        }
        std::snprintf(msg, sizeof(msg), "F223: the recorded jump pack start (E less the move %.2f to %.2f) stays within 1.50 to 1.78 m; taken %u, refused %u", lo, hi, taken, refused);
        // the 1.10 m drop and the returns of 0.32, 0.39 and 0.23 m are the root's; the first changes are under 0.10 m
        check(lo >= 1.50 && hi <= 1.78 && taken == 4 && refused == 0, msg);
        double pop = r.pop;
        int calls = 0;
        double tt = t;
        while (std::fabs(pop) > 0.0 && calls < 200) {
            const double cam[3] = {0.0, 1.70, 0.0};
            pop = r.step(true, tt, 1.60, cam, up);
            tt += dt;
            ++calls;
        }
        std::snprintf(msg, sizeof(msg), "F223: the move eases back to 0 in %d calls (%.2f s)", calls, calls * dt);
        check(pop == 0.0 && calls * dt <= 0.5, msg);
    }
    {   // a camera that really moved: refused, no move
        f223::RootJump r;
        const double cam0[3] = {0.0, 1.70, 0.0}, cam1[3] = {0.0, 1.20, 0.0};
        r.step(true, 0.0, 1.70, cam0, up);
        const double pop = r.step(true, dt, 1.20, cam1, up);
        check(pop == 0.0 && r.refused && !r.taken, "F223: a camera that moved as much as the model's change is refused and leaves no move");
    }
    {   // under 0.10 m, a frame that is not new, a gap past 0.1 s
        f223::RootJump r;
        const double cam[3] = {0.0, 1.70, 0.0};
        r.step(true, 0.0, 1.70, cam, up);
        check(r.step(true, dt, 1.61, cam, up) == 0.0 && !r.taken && !r.refused, "F223: a change under 0.10 m is nothing");
        r.step(true, 2 * dt, 1.61, cam, up);
        const double popJump = r.step(true, 3 * dt, 0.60, cam, up);
        check(popJump < -1.0 && r.taken, "F223: a root jump is taken");
        check(r.step(false, 4 * dt, 5.0, cam, up) == popJump && !r.taken, "F223: a frame that is not new leaves the move as it is");
        f223::RootJump g;
        g.step(true, 0.0, 1.70, cam, up);
        check(g.step(true, 0.25, 0.60, cam, up) == 0.0 && !g.taken && !g.refused, "F223: a gap past 0.1 s is not compared");
    }
    {   // the cap, and numbers that are not
        f223::RootJump r;
        const double cam[3] = {0.0, 1.70, 0.0};
        double e = 1.70, t = 0.0;
        r.step(true, t, e, cam, up);
        for (int i = 0; i < 4; ++i) {
            e -= 0.9;
            t += dt;
            r.step(true, t, e, cam, up);
        }
        check(r.pop == -f223::kCapM, "F223: the move is held to -1.5 m");
        const double before = r.pop;
        const double nanCam[3] = {std::nan(""), 1.70, 0.0};
        check(r.step(true, t + dt, 1.0, nanCam, up) == before && !r.have, "F223: a camera that is not a number holds the move and forgets the last frame");
        check(r.step(true, t + 2 * dt, std::nan(""), cam, up) == before, "F223: a height that is not a number holds the move");
        const double zeroUp[3] = {0.0, 0.0, 0.0};
        check(r.step(true, t + 3 * dt, 1.0, cam, zeroUp) == before, "F223: an up that is nothing holds the move");
        r.reset();
        check(r.pop == 0.0 && !r.have, "F223: reset zeroes it");
    }
    {   // the counts and the line
        f223::RootJump r;
        f223::Stats st;
        check(!st.any(), "F223: an empty window has nothing to say");
        const double cam[3] = {0.0, 1.70, 0.0}, awayCam[3] = {0.0, 1.0, 0.0};
        r.step(true, 0.0, 1.70, cam, up);
        r.step(true, dt, 0.56, cam, up);
        st.add(r);
        r.step(true, 2 * dt, 0.0, awayCam, up);
        st.add(r);
        char line[300];
        f223::formatLine(line, sizeof(line), 10, st);
        check(std::string(line) == "native on foot root jump (F223) 10 s: 1 calls taken, the largest -1.14 m; 1 refused (the camera itself moved); the move now -1.13 m", line);
    }
    {   // the sites
        const std::string cpp = slurp223("src/d3d11/native_on_foot.cpp"), rec = slurp223("src/common/onfoot_recorder_core.h");
        const auto has = [&](const char* needle) { return cpp.find(needle) != std::string::npos; };
        check(has("pc.f215BodyY = g_f215.y + g_f223Pop + g_f224Lead;") && has("yUsed = g_f215.y + g_f223Pop + g_f224Lead;") && has("bi.eyeY = pc.eyeYSelected + pc.f215BodyY - pc.f223PopM;"), "F223: the shift in force is F215's plus the move, and F215's rule reads the camera less the move");
        const size_t step = cpp.find("g_f223Pop = g_f223.step(fresh223,"), shift = cpp.find("yUsed = g_f215.y + g_f223Pop + g_f224Lead;"), branch = cpp.find("if (!nofoot::f126::armsMayUseSnapshot(fpAgeUsable, fpRigCurrent))");
        check(step != std::string::npos && shift != std::string::npos && branch != std::string::npos && step < branch && branch < shift, "F223: the rule runs once a call before G's refusals and before the shift is formed");
        check(has("pinWanted && g_f215Key.load(std::memory_order_relaxed) && !g_f107Away.away") && has("g_f223.reset();\n            g_f223Pop = 0.0;"), "F223: the key, the camera away or a failed read reset the rule and the move");
        check(has("r.f223_pop_m = static_cast<float>(pc.f223PopM);") && rec.find("S(f32, f223_pop_m)") != std::string::npos, "F223: the pin record carries the move");
    }
}

}  // namespace f223test
