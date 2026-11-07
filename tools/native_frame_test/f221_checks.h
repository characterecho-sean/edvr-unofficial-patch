// F221 checks (native on-foot VR), run221 below. Header only, run by native_frame_test.
//
// F221 (native_on_foot_f221.h, native_on_foot.cpp): with the arc cutter ($humanoid_repairtool_name;) the left elbow takes the body-down pole through F165's blend, ramped over 0.2 s, and F181a's cap
// share falls to 0 with the ramp. The rig runs the name test (the cutter and its neighbours), the ramp (time to 1 and back, a dt that is nothing or not a number), that a weight of 0 leaves
// F192's blend weight and cap share bit for bit, that a weight of 1 takes the cap's share and target to nothing, the real two-bone frame (the blended elbow cannot depend on the stable pole, and
// hangs under the shoulder-to-hand midpoint), the counts and the line, and pins the source sites.
#pragma once

#include "../../src/d3d11/native_on_foot_core.h"
#include "../../src/d3d11/native_on_foot_f165.h"
#include "../../src/d3d11/native_on_foot_f181.h"
#include "../../src/d3d11/native_on_foot_f192.h"
#include "../../src/d3d11/native_on_foot_f221.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>

namespace f221test {

namespace nf = edvr::nofoot;
namespace f221 = edvr::nofoot::f221;

inline std::string slurp221(const char* path) {
    std::ifstream f(path, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

template <class Check>
inline void run221(Check&& check) {
    const auto eq = [](double a, double b, double tol) { return std::fabs(a - b) <= tol; };
    char msg[400];

    // ---- 1. the name ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
    {
        check(f221::arcCutter("$humanoid_repairtool_name;"), "F221: the arc cutter's name is the arc cutter");
        check(!f221::arcCutter(nullptr) && !f221::arcCutter("") && !f221::arcCutter("$humanoid_repairtool_name") && !f221::arcCutter("$humanoid_rechargetool_name;") &&
                  !f221::arcCutter("$humanoid_companalyser_name;") && !f221::arcCutter("$wpn_m_assaultrifle_laser_fauto_name;"),
              "F221: no name, an empty one, the name without its semicolon, the other tools and a rifle are not the arc cutter");
    }

    // ---- 2. the ramp ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
    {
        f221::Ramp r;
        const double dt = 0.01;
        int steps = 0;
        while (r.w < 1.0 && steps < 1000) { r.step(true, dt); ++steps; }
        check(steps == 20 && r.w == 1.0, "F221: from 0 the weight reaches 1 after 0.2 s of steps");
        f221::Ramp early;
        for (int i = 0; i < 19; ++i) early.step(true, dt);
        std::snprintf(msg, sizeof(msg), "F221: after 0.19 s the weight is %.3f, not yet 1", early.w);
        check(early.w < 1.0 && eq(early.w, 0.95, 1e-9), msg);
        steps = 0;
        while (r.w > 0.0 && steps < 1000) { r.step(false, dt); ++steps; }
        check(steps == 20 && r.w == 0.0, "F221: from 1 it falls to 0 the same way");
        f221::Ramp mid;
        mid.step(true, 0.1);
        const double w = mid.w;
        mid.step(true, 0.0);
        mid.step(true, -0.5);
        mid.step(true, std::nan(""));
        mid.step(false, std::numeric_limits<double>::infinity());
        check(mid.w == w && eq(w, 0.5, 1e-12), "F221: a dt of 0, a negative one, one that is not a number or infinite leaves the weight alone");
        f221::Ramp big;
        big.step(true, 5.0);
        const bool top = big.w == 1.0;
        big.step(false, 5.0);
        check(top && big.w == 0.0, "F221: a long step stays inside 0..1");
    }

    // ---- 3. a weight of 0 changes nothing, a weight of 1 takes the cap away ----------------------------------------------------------------------------------------------------------------------
    {
        bool same = true;
        const double xs[] = {0.0, 0.1, 0.25, 0.5, 0.75, 1.0};
        for (double x : xs) {
            same = same && nf::f192::blendWeight(x, 0.0) == x;
            for (double dh : xs) {
                same = same && nf::f192::blendWeight(nf::f192::blendWeight(x, dh), 0.0) == nf::f192::blendWeight(x, dh);
                for (double sh : xs) same = same && nf::f192::capShare(sh, nf::f192::blendWeight(dh, 0.0)) == nf::f192::capShare(sh, dh);
            }
        }
        check(same, "F221: with the weight at 0 the blend weight and the cap's share are what they were, bit for bit");
        check(nf::f192::capShare(1.0, nf::f192::blendWeight(0.0, 1.0)) == 0.0 && nf::f181::targetDeg(70.0, 25.0, 0.0) == 70.0, "F221: at weight 1 the cap's share is 0 and the target is the plane angle: no cap");
        const double half = nf::f192::capShare(1.0, nf::f192::blendWeight(0.0, 0.5));
        check(eq(half, 0.5, 1e-15), "F221: at weight 0.5 the cap keeps half its share");
    }

    // ---- 4. the real two-bone frame ---------------------------------------------------------------------------------------------------------------------------------------------------------------
    {
        const double S[3] = {0.0, 1.3, 0.0}, T[3] = {0.0, 1.0, 0.4}, ref[3] = {1.0, 0.2, 0.0};
        const nf::IkTwoBone tb = nf::ikTwoBoneFrame(S, T, 0.3, 0.28, ref);
        check(tb.ok, "F221: the rig's arm frame is built");
        const double prevRadial[3] = {1.0, 0.0, 0.0};
        const double pi = 3.14159265358979323846;
        const nf::f165::Blend a = nf::f165::blendPole(tb, 0.3, 1.0, prevRadial, 1.0, 0.05, 0.0);
        const nf::f165::Blend b = nf::f165::blendPole(tb, 0.3 + 60.0 * pi / 180.0, 1.0, prevRadial, 1.0, 0.05, 0.0);
        double Ea[3], Eb[3];
        tb.elbowAt(a.theta, Ea);
        tb.elbowAt(b.theta, Eb);
        const double d[3] = {Ea[0] - Eb[0], Ea[1] - Eb[1], Ea[2] - Eb[2]};
        std::snprintf(msg, sizeof(msg), "F221: at weight 1 two stable swivels 60 deg apart give the same elbow (applied %d %d, apart %.3g m)", a.applied, b.applied, nf::ikLen(d));
        check(a.applied && b.applied && nf::ikLen(d) <= 1e-6, msg);
        const double mid = 0.5 * (S[1] + tb.hand[1]);
        std::snprintf(msg, sizeof(msg), "F221: that elbow (y %.3f) hangs under the shoulder-to-hand midpoint (y %.3f)", Ea[1], mid);
        check(Ea[1] < mid, msg);
    }

    // ---- 5. the counts and the line ---------------------------------------------------------------------------------------------------------------------------------------------------------------
    {
        f221::Stats st;
        check(!st.any(), "F221: a window with no solve has nothing to say");
        for (int i = 0; i < 100; ++i) st.add(i < 90, i < 90 ? 1.0 : 0.5, i < 3, i < 2, i < 50 ? -10.0 : -20.0);
        st.add(true, 1.0, false, false, 2.5);
        check(st.any() && st.solves == 101 && st.hung == 91 && st.guarded == 3 && st.fallback == 2 && eq(st.elbowOverHandMaxCm, 2.5, 1e-12) && eq(st.weightSum, 90.0 + 5.0 + 1.0, 1e-9),
              "F221: the window counts solves, those hung, the clamp, the vertical axis, the weight and the elbow over the hand's highest");
        char line[400];
        f221::formatLine(line, sizeof(line), 10.0, st);
        check(std::string(line) == "native on foot tool elbow (F221) 10 s: 101 solves with the arc cutter, hung 91 (weight mean 0.95), guarded 3, axis vertical 2; the elbow over the hand mean -14.8 cm, highest +2.5 cm", line);
        f221::Stats low;
        low.add(true, 1.0, false, false, -8.0);
        check(eq(low.elbowOverHandMaxCm, -8.0, 1e-12), "F221: a window whose elbow is always under the hand reports a highest that is negative, not 0");
        st.clear();
        check(!st.any(), "F221: clear empties the window");
    }

    // ---- 6. the sites -----------------------------------------------------------------------------------------------------------------------------------------------------------------------------
    {
        const std::string cpp = slurp221("src/d3d11/native_on_foot.cpp");
        const auto has = [&](const char* needle) { return cpp.find(needle) != std::string::npos; };
        check(has("g_arcCutterHeld.store(nofoot::f221::arcCutter(name), std::memory_order_relaxed);") && has("g_arcCutterHeld.store(false, std::memory_order_relaxed);"),
              "F221: the held name is tested with arcCutter and cleared when the on-foot state ends");
        check(has("s_f221Ramp.step(arcCutterForCall && armedNow, supportDt)"), "F221: the ramp is stepped once a call, on the cutter held and armed");
        check(has("nofoot::f192::blendWeight(nofoot::f192::blendWeight(w165, dh), w221)"), "F221: the pole blend takes the larger of F165's, F192's and the cutter's weights");
        check(has("nofoot::f192::capShare(nofoot::f181::hipShare(pinCall.detachW), nofoot::f192::blendWeight(o.f192Dh, w221))"), "F221: the cap's target share falls with the cutter's weight");
        check(has("!(so[0].f221W > 0.0)) f.f181.add("), "F221: F181's counters leave out the cutter's solves");
        check(has("(o.f221Applied && o.f221W >= 0.5) ? 2 : 1;"), "F221: the pole source reads body-down once the cutter's weight is half");
        check(has("nofoot::f221::formatLine(toolLine") && has("g_f221Box.write(") && has("g_f221Box.clearPending.store(true"), "F221: the window's line is printed from the solve's window and cleared at an edge");
    }
}

}  // namespace f221test
