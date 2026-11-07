// F224 checks (native on-foot VR), run224 below. Header only, run by native_frame_test.
//
// F224 (native_on_foot_f223.h RootJump::lead, native_on_foot.cpp): the whole-body move y is the last call's target, so it follows the camera's jitter one call late and the camera less y alternates by
// twice the jitter. The lead is this call's change of the camera in the model when it is under 0.08 m and not a root jump; the shift in force is y + pop + lead, and F215's rule still reads the true
// camera. The rig feeds the recorded jitter through F215's Follow (active item) and RootJump together with y one call late as in the hook: the camera less the shift is constant within 1 mm once
// settled where the camera less y alternates. It also runs the limit, a taken jump, a call that is not new, numbers that are not and a bad gap, the line, and pins the sum lines and the active gate.
#pragma once

#include "../../src/d3d11/native_on_foot_f215.h"
#include "../../src/d3d11/native_on_foot_f223.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

namespace f224test {

namespace f215 = edvr::nofoot::f215;
namespace f223 = edvr::nofoot::f223;

inline std::string slurp224(const char* path) {
    std::ifstream f(path, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

template <class Check>
inline void run224(Check&& check) {
    char msg[400];
    const double up[3] = {0.0, 1.0, 0.0}, cam[3] = {0.0, 1.70, 0.0};
    const double dt = 1.0 / 90.0;
    {   // the recorded jitter, y one call late, with and without the lead
        const double series[] = {1.489, 1.479, 1.486, 1.477, 1.486, 1.469, 1.482};
        f215::Follow fol;
        f223::RootJump rj;
        double yPrev = 0.0, loWith = 1e9, hiWith = -1e9, loWithout = 1e9, hiWithout = -1e9, t = 0.0;
        bool leadOk = true;
        for (int n = 0; n < 240; ++n) {
            const double ctrue = series[n % 7];
            const double pop = rj.step(true, t, ctrue, cam, up);
            const bool active = f215::isActive(fol.last);
            const double lead = active ? rj.lead : 0.0;
            const double eInt = ctrue - (yPrev + pop + lead);
            const double eIntNoLead = ctrue - (yPrev + pop);
            f215::In in;
            in.keyOn = true;
            in.hipItem = true;
            in.poseValid = true;
            in.fresh = true;
            in.headPitchDeg = 0.0;
            in.tS = t;
            in.eyeY = eInt + (yPrev + pop + lead) - pop;   // internal E plus the whole shift less the move: the true camera the rule reads
            in.neckY = 1.40;
            const f215::Out o = fol.step(in);
            yPrev = o.y;
            if (n >= 140) {
                loWith = std::fmin(loWith, eInt);
                hiWith = std::fmax(hiWith, eInt);
                loWithout = std::fmin(loWithout, eIntNoLead);
                hiWithout = std::fmax(hiWithout, eIntNoLead);
                leadOk = leadOk && active;
            }
            t += dt;
        }
        std::snprintf(msg, sizeof(msg), "F224: the camera less the shift spans %.2f mm with the lead and %.2f mm without it, once settled", (hiWith - loWith) * 1000.0, (hiWithout - loWithout) * 1000.0);
        check(leadOk && hiWith - loWith < 0.001 && hiWithout - loWithout > 0.005, msg);
    }
    {   // the limit: under it the step leads, 9 cm does not, a root jump is taken and does not lead
        f223::RootJump r;
        r.step(true, 0.0, 1.50, cam, up);
        r.step(true, dt, 1.55, cam, up);
        check(std::fabs(r.lead - 0.05) < 1e-12 && !r.taken && !r.leadOver, "F224: a step of 5 cm leads");
        r.step(true, 2 * dt, 1.46, cam, up);
        check(r.lead == 0.0 && r.leadOver && !r.taken, "F224: a step of 9 cm does not lead and counts as over the limit");
        f223::RootJump g;
        g.step(true, 0.0, 1.50, cam, up);
        const double pop = g.step(true, dt, 1.20, cam, up);
        check(g.taken && g.lead == 0.0 && std::fabs(pop + 0.30) < 1e-12 && !g.leadOver, "F224: a step the root jump rule takes gives no lead and is not counted twice");
        f223::RootJump k;
        k.step(true, 0.0, 1.50, cam, up);
        k.step(true, dt, 1.53, cam, up);
        const double kept = k.lead;
        k.step(false, 2 * dt, 5.0, cam, up);
        check(kept == k.lead && std::fabs(kept - 0.03) < 1e-12, "F224: a call that is not new keeps the lead");
        const double nanCam[3] = {std::nan(""), 1.70, 0.0};
        k.step(true, 3 * dt, 1.54, nanCam, up);
        check(k.lead == 0.0 && !k.have, "F224: a number that is not one gives no lead");
        k.step(true, 4 * dt, 1.54, cam, up);
        check(k.lead == 0.0, "F224: with no last frame there is no lead");
        k.step(true, 5 * dt, 1.56, cam, up);
        const bool leads = std::fabs(k.lead - 0.02) < 1e-12;
        k.step(true, 5 * dt + 0.25, 1.57, cam, up);
        check(leads && k.lead == 0.0, "F224: a gap past 0.1 s gives no lead");
        k.reset();
        check(k.lead == 0.0 && !k.leadOver, "F224: reset clears the lead");
    }
    {   // the counts and the line
        f223::RootJump r;
        f223::LeadStats st;
        check(!st.any(), "F224: an empty window has nothing to say");
        r.step(true, 0.0, 1.50, cam, up);
        r.step(true, dt, 1.53, cam, up);
        st.add(true, r);
        r.step(true, 2 * dt, 1.50, cam, up);
        st.add(true, r);
        r.step(true, 3 * dt, 1.41, cam, up);
        st.add(true, r);
        st.add(false, r);
        char line[300];
        f223::formatLeadLine(line, sizeof(line), 10, st);
        check(std::string(line) == "native on foot body move lead (F224) 10 s: 3 active calls, the lead rms 24.5 mm, largest +30.0 mm; 1 over the limit", line);
    }
    {   // the sites
        const std::string cpp = slurp224("src/d3d11/native_on_foot.cpp"), rec = slurp224("src/common/onfoot_recorder_core.h");
        const auto has = [&](const char* needle) { return cpp.find(needle) != std::string::npos; };
        check(has("pc.f215BodyY = g_f215.y + g_f223Pop + g_f224Lead;") && has("yUsed = g_f215.y + g_f223Pop + g_f224Lead;") && has("bi.eyeY = pc.eyeYSelected + pc.f215BodyY - pc.f223PopM;"),
              "F224: the shift in force is y plus the move plus the lead at both sites, and the rule reads the true camera less the move as before");
        check(has("g_f224Lead = f224Active ? g_f223.lead : 0.0;") && has("const bool f224Active = nofoot::f215::isActive(g_f215.last);") && has("g_f223Pop = 0.0;\n            g_f224Lead = 0.0;"),
              "F224: the lead is taken only while F215's last state is active, and 0 whenever the rule resets");
        check(has("r.f224_lead_m = static_cast<float>(pc.f224LeadM);") && rec.find("S(f32, f224_lead_m)") != std::string::npos && has("r.f215_body_y = pc.f215BodyY;"), "F224: the pin record carries the lead and the whole shift");
    }
}

}  // namespace f224test
