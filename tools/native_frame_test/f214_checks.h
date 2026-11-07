// F214 checks (native on-foot VR), run214 below. Header only, run by native_frame_test.
//
// F214 (native_on_foot_f214.h, native_on_foot.cpp; TEMPORARY A/B key experimental.native_on_foot_view_follows_body_moving): unarmed, the view raise follows the neck while the body moves,
// through a 0.6 s mean published once the window holds 0.3 s. The slope flight (edvr_gfx_20261105_195735) measured the camera over the neck at +9 cm + 60 cm x tan(slope) at any speed, and
// nothing corrected it in motion. The rig runs the window (the mean, the fill, the gap), a walk cycle's stride through it (the ripple it leaves), a slope step (how fast it tracks), the rule's
// outcomes one at a time (every held reason, the once-only re-publish for an item or a posture and none for a pitch, the key off, the owned call), the line, and the sites.
#pragma once

#include "../../src/d3d11/native_on_foot_f214.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

namespace f214test {

namespace f214 = edvr::nofoot::f214;

inline std::string slurp(const char* path) {
    std::ifstream f(path, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// A call that follows if the window allows it: unarmed, upright, moving, the head level, a fresh frame with a sample.
inline f214::In walking(double tS, double sampleM) {
    f214::In in;
    in.keyOn = true;
    in.moving = true;
    in.valid = true;
    in.poseValid = true;
    in.fresh = true;
    in.sampleOk = true;
    in.headPitchDeg = 2.0;
    in.tS = tS;
    in.sampleM = sampleM;
    return in;
}

// A stride's ripple left in the published mean: the half range of the mean over the last 2 s of a 4 s run at 90 Hz, amplitude ampM, period periodS.
inline double ripple(double ampM, double periodS) {
    f214::Follow f;
    double lo = 1e9, hi = -1e9;
    const double dt = 1.0 / 90.0;
    for (int i = 0; i < 360; ++i) {
        const double t = i * dt;
        const f214::Out o = f.step(walking(t, 0.15 + ampM * std::sin(6.283185307179586 * t / periodS)));
        if (t >= 2.0 && o.publish) {
            lo = std::fmin(lo, o.publishM);
            hi = std::fmax(hi, o.publishM);
        }
    }
    return (hi - lo) * 0.5;
}

template <class Check>
void run214(Check&& check) {
    // ---- 1. the window: the mean, the fill, the gap -----------------------------------------------------------------------------------------------------------------------------------------------------------
    {
        f214::Mean w;
        w.add(0.0, 0.10);
        w.add(0.1, 0.20);
        w.add(0.2, 0.30);
        check(std::fabs(w.mean() - 0.20) < 1e-12 && w.n == 3, "F214: the mean of the samples held");
        check(!w.ready(), "F214: 0.2 s of samples is not a full fill");
        w.add(0.3, 0.40);
        check(w.ready() && std::fabs(w.mean() - 0.25) < 1e-12, "F214: 0.3 s of samples fills the window (the first mean is published there)");
        w.add(0.7, 0.60);   // 0.4 s since the last sample: over the 0.25 s gap
        check(w.n == 1 && std::fabs(w.mean() - 0.60) < 1e-12 && !w.ready(), "F214: a gap over 0.25 s empties the window");
        f214::Mean e;
        for (int i = 0; i <= 12; ++i) e.add(i * 0.1, i < 7 ? 0.0 : 1.0);   // 0 for 0.0..0.6 s, 1 from 0.7 s
        check(std::fabs(e.spanS() - 0.5) < 1e-9 && std::fabs(e.mean() - 1.0) < 1e-12, "F214: a sample 0.6 s old has left the window (span under 0.6 s)");
        f214::Mean g;
        g.add(1.0, 0.1);
        g.add(1.1, 0.1);
        g.add(1.05, 0.9);   // time went back: a new run
        check(g.n == 1, "F214: a time that went back starts the window again");
        f214::Mean full;
        for (int i = 0; i < 3 * f214::kCap; ++i) full.add(i * 0.0001, 1.0);   // far more samples than the ring holds inside the window
        check(full.n <= f214::kCap && std::fabs(full.mean() - 1.0) < 1e-12, "F214: a full ring drops the oldest and stays a mean");
        f214::Mean bad;
        bad.add(0.0, std::nan(""));
        check(bad.n == 0 && std::isnan(bad.mean()), "F214: a sample that is not a number is not taken");
    }

    // ---- 2. a stride through the window ---------------------------------------------------------------------------------------------------------------------------------------------------------------------
    {
        const double r3 = ripple(0.10, 0.3), r5 = ripple(0.10, 0.5);
        std::printf("F214 rig: a +-10 cm stride at 90 Hz leaves %.2f cm ripple at a 0.3 s period and %.2f cm at 0.5 s\n", r3 * 100.0, r5 * 100.0);
        check(r3 < 0.015, "F214: a +-10 cm stride at a 0.3 s period leaves under 1.5 cm in the published mean");
        // A 0.6 s mean passes a sine of period P at |sin(pi 0.6 / P) / (pi 0.6 / P)| of its amplitude: 0 at 0.3 s, 0.156 at 0.5 s (1.56 cm of +-10 cm), the worst a walk's stride gives. Under 1.5 cm at 0.5 s
        // would take a window other than 0.6 s; the bound is the analytic value with a little room.
        check(r5 < 0.0160 && std::fabs(r5 - 0.1 * std::fabs(std::sin(3.141592653589793 * 0.6 / 0.5) / (3.141592653589793 * 0.6 / 0.5))) < 0.0005,
              "F214: a +-10 cm stride at a 0.5 s period leaves what a 0.6 s mean must (1.56 cm), under 1.6 cm");
        check(r5 > 0.001, "F214: the ripple measure is not vacuous (a 0.5 s stride leaves some)");
    }

    // ---- 3. a slope step is tracked ----------------------------------------------------------------------------------------------------------------------------------------------------------------------
    {
        f214::Follow f;
        const double dt = 1.0 / 90.0;
        double atStep = std::nan(""), after1s = std::nan(""), first = std::nan("");
        for (int i = 0; i < 400; ++i) {
            const double t = i * dt;
            const double v = t < 2.0 ? 0.0 : 0.25;
            const f214::Out o = f.step(walking(t, v));
            if (o.publish && std::isnan(first)) first = t;
            if (o.publish && t < 2.0 - dt * 0.5) atStep = o.publishM;
            if (o.publish && t >= 3.0 - dt * 0.5 && std::isnan(after1s)) after1s = o.publishM;
        }
        check(std::fabs(first - 0.3) < 2 * dt, "F214: the first mean is published when the window holds 0.3 s");
        check(std::fabs(atStep) < 1e-9 && std::fabs(after1s - 0.25) < 0.005, "F214: a 25 cm slope step is tracked within 1 s");
    }

    // ---- 4. the rule, one outcome at a time -----------------------------------------------------------------------------------------------------------------------------------------------------------
    {
        using f214::In;
        const auto fill = [](f214::Follow& f, double t0, double v) {   // 0.4 s of followed calls from t0; the last call's time
            double t = t0;
            for (int i = 0; i < 37; ++i, t += 1.0 / 90.0) f.step(walking(t, v));
            return t;
        };
        {   // off: nothing runs, nothing is kept
            f214::Follow f;
            In in = walking(0.0, 0.1);
            in.keyOn = false;
            bool any = false;
            for (int i = 0; i < 90; ++i) {
                in.tS = i / 90.0;
                const f214::Out o = f.step(in);
                any = any || o.publish || o.state != f214::kOff;
            }
            check(!any && f.win.n == 0 && !f.dirty, "F214: the key off publishes nothing and keeps no window");
        }
        {   // not moving: the settled branch's business
            f214::Follow f;
            In in = walking(0.0, 0.1);
            in.moving = false;
            const f214::Out o = f.step(in);
            check(o.state == f214::kNotMoving && !o.publish && f.win.n == 0, "F214: a body not moving is not_moving and takes no sample");
        }
        {   // every held reason, with a full window before it
            struct Case {
                const char* name;
                f214::State want;
                void (*set)(In&);
            };
            const Case cases[] = {
                {"the camera away", f214::kHeldAway, [](In& i) { i.away = true; }},
                {"an item drawn", f214::kHeldItem, [](In& i) { i.itemDrawn = true; }},
                {"a crouched or hunched posture", f214::kHeldPosture, [](In& i) { i.lowPosture = true; }},
                {"another publisher's target", f214::kHeldF138, [](In& i) { i.owned = true; }},
                {"F138's aim gate", f214::kHeldAim, [](In& i) { i.valid = false; i.aimRefused = true; }},
                {"a target not computable", f214::kHeldPose, [](In& i) { i.valid = false; }},
                {"no valid pose", f214::kHeldPose, [](In& i) { i.poseValid = false; }},
                {"a head pitch that is not a number", f214::kHeldPose, [](In& i) { i.headPitchDeg = std::nan(""); }},
                {"the head pitched down 60 deg", f214::kHeldHead, [](In& i) { i.headPitchDeg = -60.0; }},
                {"the head pitched up 20 deg", f214::kHeldHead, [](In& i) { i.headPitchDeg = 20.0; }},
                {"the head pitched -20 deg", f214::kHeldHead, [](In& i) { i.headPitchDeg = -20.0; }},
            };
            for (const Case& c : cases) {
                f214::Follow f;
                f.noteSettled(0.12);
                const double t = fill(f, 0.0, 0.3);
                In in = walking(t, 0.3);
                c.set(in);
                const f214::Out o = f.step(in);
                const bool republished = o.republish;
                const bool wantRepublish = c.want == f214::kHeldItem || c.want == f214::kHeldPosture;
                char msg[160];
                std::snprintf(msg, sizeof(msg), "F214: %s is %s%s", c.name, f214::stateName(c.want), wantRepublish ? " and re-publishes the last settled target once" : " and publishes nothing");
                const bool ok = o.state == c.want && republished == wantRepublish && o.publish == wantRepublish && (!wantRepublish || std::fabs(o.publishM - 0.12) < 1e-12);
                check(ok, msg);
                if (wantRepublish) {   // once: the next call, still the item, publishes nothing
                    In again = walking(t + 1.0 / 90.0, 0.3);
                    c.set(again);
                    const f214::Out o2 = f.step(again);
                    check(!o2.publish && !o2.republish, "F214: the re-publish is once");
                }
            }
        }
        {   // a pitch holds and later an item does not re-publish a stale slope value... it does: the moving target is still in force, so the item takes the settled one once
            f214::Follow f;
            f.noteSettled(0.12);
            double t = fill(f, 0.0, 0.3);
            In pitched = walking(t, 0.3);
            pitched.headPitchDeg = -60.0;
            const f214::Out p = f.step(pitched);
            check(p.state == f214::kHeldHead && !p.publish && f.dirty, "F214: a head pitch holds the moving target in force and publishes nothing");
            In item = walking(t + 0.1, 0.3);
            item.itemDrawn = true;
            const f214::Out o = f.step(item);
            check(o.republish && std::fabs(o.publishM - 0.12) < 1e-12, "F214: an item drawn after a hold takes the last settled target once");
        }
        {   // no settled target yet: nothing to publish on leaving
            f214::Follow f;
            const double t = fill(f, 0.0, 0.3);
            In item = walking(t, 0.3);
            item.itemDrawn = true;
            const f214::Out o = f.step(item);
            check(o.state == f214::kHeldItem && !o.publish, "F214: no settled target yet, leaving for an item publishes nothing");
        }
        {   // an owned call (ADS's zero, a settled stance, F138) replaces the moving target: no re-publish over it
            f214::Follow f;
            f.noteSettled(0.12);
            const double t = fill(f, 0.0, 0.3);
            In item = walking(t, 0.3);
            item.itemDrawn = true;
            item.owned = true;
            const f214::Out o = f.step(item);
            check(o.state == f214::kHeldItem && !o.publish && !f.dirty, "F214: a call another publisher owns gets no re-publish");
        }
        {   // the settled branch replaces the moving target
            f214::Follow f;
            f.noteSettled(0.12);
            const double t = fill(f, 0.0, 0.3);
            check(f.dirty, "F214: a followed call leaves a moving target in force");
            f.noteSettled(0.14);
            In item = walking(t, 0.3);
            item.itemDrawn = true;
            const f214::Out o = f.step(item);
            check(!o.publish && std::fabs(f.settledM - 0.14) < 1e-12, "F214: a settled target replaces the moving one: leaving for an item publishes nothing");
        }
        {   // the first calls fill and publish nothing; not a fresh frame follows without a sample or a publish
            f214::Follow f;
            bool publishedEarly = false;
            for (int i = 0; i < 20; ++i) {
                const f214::Out o = f.step(walking(i / 90.0, 0.2));
                publishedEarly = publishedEarly || o.publish || o.state != f214::kFilling;
            }
            check(!publishedEarly, "F214: until the window holds 0.3 s the calls are filling and publish nothing (the raise holds)");
            const double t = fill(f, 0.3, 0.2);
            In stale = walking(t, 0.9);
            stale.fresh = false;
            const f214::Out o = f.step(stale);
            check(o.state == f214::kFollowed && !o.publish && std::fabs(o.meanM - 0.2) < 1e-9, "F214: a call with no new game frame follows without taking a sample or publishing");
        }
        {   // a hold longer than the gap: the window starts again, with the fill
            f214::Follow f;
            const double t = fill(f, 0.0, 0.3);
            In pitched = walking(t, 0.3);
            pitched.headPitchDeg = 45.0;
            f.step(pitched);
            const f214::Out o = f.step(walking(t + 0.5, 0.1));
            check(o.state == f214::kFilling && !o.publish, "F214: after a hold over 0.25 s the window starts again and fills before it publishes");
        }
    }

    // ---- 5. the log line --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
    {
        f214::Stats s;
        const auto call = [&](f214::State st, bool movingUnarmed, double pub, bool re) {
            f214::Call c;
            c.state = st;
            c.keyOn = true;
            c.movingUnarmed = movingUnarmed;
            c.publishedM = pub;
            c.republished = re;
            s.add(c);
        };
        call(f214::kFilling, true, std::nan(""), false);
        call(f214::kFollowed, true, 0.10, false);
        call(f214::kFollowed, true, 0.20, false);
        call(f214::kHeldHead, true, std::nan(""), false);
        call(f214::kHeldItem, false, std::nan(""), true);
        call(f214::kNotMoving, false, std::nan(""), false);
        char line[760];
        f214::formatLine(line, sizeof(line), 10, s);
        check(std::strstr(line, "native on foot view follows body (F214) 10 s:") == line &&
                  std::strstr(line, "experimental.native_on_foot_view_follows_body_moving is on; pin calls 6, moving and unarmed 4, followed 2;") != nullptr &&
                  std::strstr(line, "held: filling 1, aim pitch 0, head pitch 1, no pose 0, posture 0, item 1, F138 0, away 0;") != nullptr &&
                  std::strstr(line, "published target mean +15.0 cm (min +10.0, max +20.0) over 2 calls; re-published 1") != nullptr,
              "F214: the line names the key, the calls, the followed, every held reason, the published mean, min and max and the re-publishes");
        f214::Stats off;
        f214::Call c;
        c.state = f214::kOff;
        c.keyOn = false;
        c.movingUnarmed = true;
        off.add(c);
        f214::formatLine(line, sizeof(line), 10, off);
        check(std::strstr(line, "is off; pin calls 1, moving and unarmed 1, followed 0;") != nullptr && std::strstr(line, "published target none; re-published 0") != nullptr,
              "F214: with the key off the line says so and that nothing was followed or published");
        check(f214::stateName(f214::kNotRun) != nullptr && std::strcmp(f214::stateName(f214::kHeldF138), "held_f138") == 0 && std::strcmp(f214::stateName(99), "?") == 0, "F214: the state names");
    }

    // ---- 6. the sites ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
    {
        const std::string cpp = slurp("src/d3d11/native_on_foot.cpp"), ini = slurp("edvr.ini"), rec = slurp("src/common/onfoot_recorder_core.h");
        const auto has = [&](const std::string& hay, const char* needle) { return hay.find(needle) != std::string::npos; };
        const auto at = [&](const char* needle) { return cpp.find(needle); };
        check(has(cpp, "cfg.getString(\"experimental.native_on_foot_view_follows_body_moving\", \"on\")") && has(cpp, "g_f214Key.store(on, std::memory_order_relaxed)") &&
                  has(cpp, "xi.keyOn = g_f214Key.load(std::memory_order_relaxed)"),
              "F214: the key is read on the frame thread, stored in an atomic and loaded by the pin hook");
        check(has(ini, "# ui: View follows body moving | choices on, off | recommended on | live\nnative_on_foot_view_follows_body_moving = on"), "F214: the shipped key is on, with its ui line");
        const size_t f138Step = at("const f138::Out fo = g_f138.step(fi);"), f214Step = at("g_f214.step(xi)"), settled = at("g_f214.noteSettled(pc.f95Target)"), settledPub = at("f94TargetPublish(pc.f95Target, now);");
        check(f138Step != std::string::npos && f214Step != std::string::npos && f214Step > f138Step, "F214: the step comes after F138's, which keeps its say");
        check(settled != std::string::npos && settledPub != std::string::npos && settled > settledPub && settled - settledPub < 400, "F214: the settled branch notes the target it published");
        check(!has(cpp, "g_f160.store(xo") && !has(cpp, "g_f160.store(pc.f214"), "F214: a moving target is never stored for F160");
        check(has(cpp, "g_f214.reset();"), "F214: an edge of the on-foot camera resets the window and the settled target");
        check(has(cpp, "r.f214_state = pc.f214State;") && has(cpp, "r.f214_target = pc.f214Target;") && has(rec, "E(F214State, f214_state)") && has(rec, "S(f64, f214_target)"),
              "F214: the pin record carries the state and the target");
        check(has(cpp, "uint8_t f214State = nofoot::f214::kNotRun;"), "F214: a call that never reached the step records not_run, not off");
        check(has(cpp, "nofoot::f214::formatLine(followLine"), "F214: the window line is printed");
    }
}

}  // namespace f214test
