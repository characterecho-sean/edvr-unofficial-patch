// F215 checks (native on-foot VR), run215 below. Header only, run by native_frame_test.
//
// F215 (native_on_foot_f215.h, native_on_foot.cpp; TEMPORARY A/B key experimental.native_on_foot_body_follows_view): unarmed and upright, the whole skeleton moves vertically by y so the neck
// stays the eye height under a view that is the game's camera plus a constant raise R0 = the eye height key less 8 cm. The rig runs the rule on level ground (y about 0), on a slope (the slope
// term passes straight through), in a simulated in-place turn (the view holds still and the neck is the eye height under it), the releases (an item, a posture: caught back to 0 at the catch
// rate), the holds (head, aim, no pose), the key off, the rate limit, the cap, a call with no new frame, the shifted matrix write, the line, and the source sites.
//
// F217 adds the item at the hip: E against one level camera height (levelCamY), the held sights, the aim pitched up, the settle held while y holds, and the world-to-model shift.
#pragma once

#include "../../src/d3d11/native_on_foot_f215.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

namespace f215test {

namespace f215 = edvr::nofoot::f215;

inline std::string slurp(const char* path) {
    std::ifstream f(path, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

constexpr double kDt = 1.0 / 90.0;
constexpr double kHeight = 0.15;   // the eye height key

// A call that runs the rule if the window allows it: unarmed, upright, the head level, a fresh frame.
inline f215::In calm(double tS, double eyeY, double neckY) {
    f215::In in;
    in.keyOn = true;
    in.poseValid = true;
    in.fresh = true;
    in.headPitchDeg = 2.0;
    in.tS = tS;
    in.eyeY = eyeY;
    in.neckY = neckY;
    in.eyeHeightM = kHeight;
    return in;
}

// Runs the rule for `seconds` from t0 with the camera at neck + over (m) and a constant neck; returns the time after the last call.
inline double run(f215::Follow& f, double t0, double seconds, double neck, double over, f215::Out* last = nullptr) {
    double t = t0;
    for (int i = 0, n = static_cast<int>(seconds / kDt + 0.5); i < n; ++i, t += kDt) {
        const f215::Out o = f.step(calm(t, neck + over, neck));
        if (last) *last = o;
    }
    return t;
}

template <class Check>
void run215(Check&& check) {
    const double c = f215::kCamOverNeckM;
    // ---- 0. the constant ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
    check(c >= 0.08 - 1e-12 && c <= 0.10 + 1e-12, "F215: the level-ground constant (nofoot::f94::kCamOverUprightNeckM) is within 8 to 10 cm");
    check(std::fabs(f215::levelRaiseM(0.15) - (0.15 - c)) < 1e-12, "F215: R0 is the eye height key less that constant");

    // ---- 1. level ground: y about 0 -----------------------------------------------------------------------------------------------------------------------------------------------------------------
    {
        f215::Follow f;
        f215::Out o;
        run(f, 0.0, 2.0, 1.54, c, &o);
        check(o.state == f215::kActive && std::fabs(o.y) < 1e-9, "F215: on level ground (the camera the constant over the neck) the rule is active with y 0");
        check(std::fabs(o.targetY) < 1e-9, "F215: ... and its target is 0");
    }

    // ---- 2. a slope term passes straight through ---------------------------------------------------------------------------------------------------------------------------------------------
    {
        f215::Follow f;
        f215::Out o;
        const double slope = 0.30;   // the camera 30 cm over the level value (a hill's 60 cm x tan)
        double t = run(f, 0.0, 0.3, 1.54, c + slope, &o);
        check(o.state == f215::kFilling || o.state == f215::kActive, "F215: the window fills first");
        t = run(f, t, 2.0, 1.54, c + slope, &o);
        check(o.state == f215::kActive && std::fabs(o.y - slope) < 1e-6, "F215: a 30 cm slope term is taken whole by the body's shift");
        const double eye = 1.54 + c + slope, view = eye + f215::levelRaiseM(kHeight), neck = 1.54 + o.y;
        check(std::fabs(neck + kHeight - view) < 1e-6, "F215: the neck plus the eye height is the view (the neck stays the eye height under the view)");
    }

    // ---- 3. an in-place turn: the camera barely moves, the view holds, the neck stays under it ---------------------------------------------------------------------------------------
    {
        f215::Follow f;
        f215::Out o;
        double t = run(f, 0.0, 2.0, 1.54, c + 0.2, &o);   // on a hill, turned to a slope
        double worst = 0.0, viewLo = 1e9, viewHi = -1e9;
        for (int i = 0; i < 360; ++i, t += kDt) {   // 4 s: the game's camera moves a median 0.9 cm in a turn; the neck swings with the body and is animated at the same height
            const double eye = 1.54 + c + 0.2 + 0.009 * std::sin(6.283185307179586 * 0.5 * t);
            o = f.step(calm(t, eye, 1.54));
            const double view = eye + f215::levelRaiseM(kHeight);
            worst = std::fmax(worst, std::fabs(1.54 + o.y + kHeight - view));
            viewLo = std::fmin(viewLo, view - eye);
            viewHi = std::fmax(viewHi, view - eye);
        }
        check(viewHi - viewLo < 1e-12, "F215: the view is the game's camera plus a constant (E + R0) through a turn");
        check(worst < 0.002, "F215: through a turn the neck plus the shift plus the eye height stays the view to 2 mm");
    }

    // ---- 4. releases: an item and a posture catch back to 0 at the catch rate ----------------------------------------------------------------------------------------------------------
    {
        for (int which = 0; which < 2; ++which) {
            f215::Follow f;
            f215::Out o;
            double t = run(f, 0.0, 2.5, 1.54, c + 0.3, &o);
            const double y0 = o.y;
            check(std::fabs(y0 - 0.3) < 1e-6, "F215: the release test starts at +30 cm");
            double elapsed = 0.0, yAt025 = 0.0;
            bool monotone = true, rateOk = true;
            double prev = y0;
            for (int i = 0; i < 90 * 2; ++i, t += kDt, elapsed += kDt) {
                f215::In in = calm(t, 1.54 + c + 0.3, 1.54);
                if (which == 0) {   // an item at the hip in a low posture (postureW 0.5 or over)
                    in.hipItem = true;
                    in.lowPostureItem = true;
                } else {
                    in.lowPosture = true;
                }
                o = f.step(in);
                if (o.y > prev + 1e-12) monotone = false;
                if (prev - o.y > f215::kCatchMps * kDt + 1e-9) rateOk = false;
                prev = o.y;
                if (std::fabs(elapsed - 0.25) <= kDt * 0.5 + 1e-9) yAt025 = o.y;
            }
            check(o.state == f215::kReleasedPosture && o.y == 0.0, which == 0 ? "F217: an item at the hip in a low posture runs y to exactly 0" : "F215: a low posture runs y to exactly 0");
            check(monotone && rateOk, which == 0 ? "F215: ... never faster than the catch rate (0.6 m/s)" : "F215: ... never faster than the catch rate, posture");
            check(std::fabs(yAt025 - (0.3 - 0.6 * 0.25)) < 0.015, "F215: after 0.25 s of release y has fallen 15 cm");
        }
        {   // the camera away releases too
            f215::Follow f;
            f215::Out o;
            double t = run(f, 0.0, 2.5, 1.54, c + 0.3, &o);
            f215::In in = calm(t, 1.54 + c + 0.3, 1.54);
            in.away = true;
            o = f.step(in);
            check(o.state == f215::kReleasedAway && o.y < 0.3 + 1e-12, "F215: the camera away is released_away");
            in = calm(t + kDt, std::nan(""), 1.54);
            o = f.step(in);
            check(o.state == f215::kReleasedAway, "F215: an eye that is not a number is released_away too");
        }
    }

    // ---- 5. holds: head, aim, pose hold y --------------------------------------------------------------------------------------------------------------------------------------------------------------
    {
        struct Case {
            const char* name;
            f215::State want;
            void (*set)(f215::In&);
        };
        const Case cases[] = {
            {"F138's aim gate", f215::kHeldAim, [](f215::In& i) { i.aimRefused = true; }},
            {"no valid pose", f215::kHeldPose, [](f215::In& i) { i.poseValid = false; }},
            {"a head pitch that is not a number", f215::kHeldPose, [](f215::In& i) { i.headPitchDeg = std::nan(""); }},
        };
        for (const Case& cs : cases) {
            f215::Follow f;
            f215::Out o;
            double t = run(f, 0.0, 2.5, 1.54, c + 0.3, &o);
            const double y0 = o.y;
            bool held = true, state = true;
            for (int i = 0; i < 90; ++i, t += kDt) {
                f215::In in = calm(t, 1.54 + c + 0.1, 1.54);   // the camera moves; the hold does not
                cs.set(in);
                o = f.step(in);
                held = held && o.y == y0;
                state = state && o.state == cs.want;
            }
            char msg[120];
            std::snprintf(msg, sizeof(msg), "F215: %s is %s and holds y", cs.name, f215::stateName(cs.want));
            check(held && state, msg);
        }
        {   // a hold longer than the window's gap empties the neck window: the fill comes again before the rule acts
            f215::Follow f;
            f215::Out o;
            double t = run(f, 0.0, 2.5, 1.54, c + 0.3, &o);
            f215::In pitched = calm(t, 1.54 + c, 1.54);
            pitched.headPitchDeg = 45.0;
            f.step(pitched);
            o = f.step(calm(t + 0.5, 1.54 + c, 1.54));
            check(o.state == f215::kFilling, "F215: after a hold over 0.25 s the neck window fills again before the rule acts");
        }
    }

    // ---- 6. the key off runs to 0 ----------------------------------------------------------------------------------------------------------------------------------------------------------------------
    {
        f215::Follow f;
        f215::Out o;
        double t = run(f, 0.0, 2.5, 1.54, c + 0.3, &o);
        bool rateOk = true;
        double prev = o.y;
        for (int i = 0; i < 90; ++i, t += kDt) {
            f215::In in = calm(t, 1.54 + c + 0.3, 1.54);
            in.keyOn = false;
            o = f.step(in);
            rateOk = rateOk && prev - o.y <= f215::kCatchMps * kDt + 1e-9 && o.state == f215::kOff;
            prev = o.y;
        }
        check(o.y == 0.0 && rateOk, "F215: the key off runs y to exactly 0 at the catch rate");
        f215::Follow g;
        bool zero = true;
        for (int i = 0; i < 180; ++i) zero = zero && g.step([&] { f215::In in = calm(i * kDt, 1.54 + c + 0.3, 1.54); in.keyOn = false; return in; }()).y == 0.0;
        check(zero, "F215: the key off from the start never moves y");
    }

    // ---- 7. the rate limit and the cap ------------------------------------------------------------------------------------------------------------------------------------------------------------
    {
        f215::Follow f;
        f215::Out o;
        double t = run(f, 0.0, 3.0, 1.54, c, &o);
        check(!f.catching && std::fabs(o.y) < 1e-9, "F215: on level ground the catch has ended");
        double prev = o.y, worst = 0.0;
        for (int i = 0; i < 90; ++i, t += kDt) {
            o = f.step(calm(t, 1.54 + c + 0.45, 1.54));   // a 45 cm step in the camera (a 3.4 s-long slope in one frame)
            worst = std::fmax(worst, std::fabs(o.y - prev));
            prev = o.y;
        }
        check(worst <= f215::kTrackMps * kDt + 1e-9 && worst > 0.5 * f215::kTrackMps * kDt, "F215: y follows a step at the tracking rate, 3 m/s");
        check(std::fabs(o.y - 0.45) < 0.011, "F215: ... and arrives");
        run(f, t, 4.0, 1.54, c + 2.0, &o);
        check(std::fabs(o.targetY - f215::kCapM) < 1e-12 && std::fabs(o.y - f215::kCapM) < 1e-9, "F215: a camera 2 m over the neck is held to the +60 cm cap");
        run(f, t + 4.0, 6.0, 1.54, c - 2.0, &o);
        check(std::fabs(o.y + f215::kCapM) < 1e-9, "F215: and the other way to -60 cm");
    }
    {   // a fresh start meets a slope with the catch rate
        f215::Follow f;
        f215::Out o;
        double t = 0.0;
        double prev = 0.0, worst = 0.0;
        for (int i = 0; i < 45; ++i, t += kDt) {
            o = f.step(calm(t, 1.54 + c + 0.3, 1.54));
            worst = std::fmax(worst, o.y - prev);
            prev = o.y;
        }
        check(worst <= f215::kCatchMps * kDt + 1e-9, "F215: entering the rule on a slope catches up at 0.6 m/s, not 3");
    }

    // ---- 8. a call with no new frame -------------------------------------------------------------------------------------------------------------------------------------------------------------
    {
        f215::Follow f;
        f215::Out o;
        const double t = run(f, 0.0, 2.5, 1.54, c + 0.3, &o);
        const double y0 = o.y;
        const int n0 = f.win.n;
        f215::In stale = calm(t, 1.54 + c + 0.9, 1.54);
        stale.fresh = false;
        stale.keyOn = false;
        const f215::Out s = f.step(stale);
        check(s.y == y0 && s.state == f215::kActive && f.win.n == n0, "F215: a call with no new game frame outputs the last y, changes no state and takes no sample");
    }

    // ---- 9. the shifted write --------------------------------------------------------------------------------------------------------------------------------------------------------------------------
    {
        float m[16];
        for (int i = 0; i < 16; ++i) m[i] = 0.25f * static_cast<float>(i + 1);
        float out[16];
        f215::shiftedMatrix(m, 0.0, out);
        check(std::memcmp(m, out, sizeof(m)) == 0, "F215: a shift of 0 writes the matrix bit for bit");
        f215::shiftedMatrix(m, 0.3, out);
        bool others = true;
        for (int i = 0; i < 16; ++i)
            if (i != 13) others = others && out[i] == m[i];
        check(others && std::fabs(static_cast<double>(out[13]) - (static_cast<double>(m[13]) + 0.3)) < 1e-6, "F215: a shift changes element 13 (the height) alone");
    }

    // ---- 10. the log line ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
    {
        f215::Stats s;
        const auto call = [&](f215::State st, bool unarmed, double y, bool headPitched = false) {
            f215::Call cl;
            cl.state = st;
            cl.headPitched = headPitched;
            cl.keyOn = true;
            cl.unarmed = unarmed;
            cl.yM = y;
            cl.r0M = 0.07;
            s.add(cl);
        };
        call(f215::kFilling, true, 0.0);
        call(f215::kActive, true, 0.10);
        call(f215::kActive, true, 0.30);
        call(f215::kFilling, true, 0.30, true);
        call(f215::kHeldAim, true, 0.30);
        call(f215::kHeldPose, true, 0.30);
        call(f215::kReleasedItem, false, 0.2);
        call(f215::kReleasedPosture, true, 0.1);
        call(f215::kReleasedAway, true, 0.0);
        call(f215::kActiveItem, false, 0.2);
        call(f215::kActiveItem, false, 0.2);
        call(f215::kHeldSights, false, 0.2);
        char line[900];
        f215::formatLine(line, sizeof(line), 10, s, 7);
        check(std::strstr(line, "native on foot body follows view (F215) 10 s:") == line &&
                  std::strstr(line, "experimental.native_on_foot_body_follows_view is on; pin calls 12, unarmed 8, active 2, head past the gate with the move on 1, held aim 1 pose 1, filling 2, released item 1 posture 1 away 1;") != nullptr &&
                  std::strstr(line, "body move mean +20.0 cm (min +10.0, max +30.0) over 2 active calls; raise R0 7.0 cm; poses written shifted 7; item at the hip active 2, held in the sights 1") != nullptr,
              "F215: the line names the key, the calls, the active, every held and released reason, the body move, R0 and the shifted writes");
        f215::Stats off;
        f215::Call cl;
        cl.state = f215::kOff;
        cl.keyOn = false;
        cl.unarmed = true;
        off.add(cl);
        f215::formatLine(line, sizeof(line), 10, off, 0);
        check(std::strstr(line, "is off; pin calls 1, unarmed 1, active 0,") != nullptr && std::strstr(line, "body move none; raise R0 0.0 cm; poses written shifted 0") != nullptr,
              "F215: with the key off the line says so and that nothing moved");
        check(std::strcmp(f215::stateName(f215::kActiveItem), "active_item") == 0 && std::strcmp(f215::stateName(f215::kHeldSights), "held_sights") == 0 && std::strcmp(f215::stateName(f215::kActive), "active") == 0 && std::strcmp(f215::stateName(f215::kNotRun), "not_run") == 0 && std::strcmp(f215::stateName(99), "?") == 0, "F215: the state names");
    }

    // ---- 12. F217: an item at the hip --------------------------------------------------------------------------------------------------------------------------------------------------------------
    {
        const double level = f215::levelCamY(true, 1.54);
        check(std::fabs(level - (1.54 + f215::kCamOverNeckItemM)) < 1e-12 && std::fabs(f215::levelCamY(false, 1.54) - f215::kDefaultLevelCamM) < 1e-12 &&
                  std::fabs(f215::levelCamY(true, std::nan("")) - f215::kDefaultLevelCamM) < 1e-12,
              "F217: the level camera is the stored standing neck plus 6 cm, 1.60 m with no store");
        const auto itemCall = [&](double tS, double eyeY) {
            f215::In in = calm(tS, eyeY, 1.54);
            in.hipItem = true;
            in.levelCamY = level;
            in.aimPitchDeg = -10.0;
            return in;
        };
        const auto itemRun = [&](f215::Follow& f, double t0, double seconds, double eyeY, f215::Out* last) {
            double t = t0;
            for (int i = 0, n = static_cast<int>(seconds / kDt + 0.5); i < n; ++i, t += kDt) *last = f.step(itemCall(t, eyeY));
            return t;
        };
        {   // follows E against the level height, on a slope and on level ground
            f215::Follow f;
            f215::Out o;
            double t = itemRun(f, 0.0, 1.0, level, &o);
            check(o.state == f215::kActiveItem && std::fabs(o.y) < 1e-9 && o.active(), "F217: an item at the hip with E at the level height is active_item with y 0");
            t = itemRun(f, t, 3.0, level + 0.55, &o);
            check(o.state == f215::kActiveItem && std::fabs(o.y - 0.55) < 1e-6 && std::fabs(o.targetY - 0.55) < 1e-9, "F217: a 55 cm slope (the arc cutter on a tilted hull) is taken whole by the body");
            itemRun(f, t, 4.0, level + 2.0, &o);
            check(std::fabs(o.targetY - f215::kCapM) < 1e-12 && std::fabs(o.y - f215::kCapM) < 1e-9, "F217: ... and held to the +60 cm cap");
        }
        {   // the sights hold y; the release catches at the slower rate
            f215::Follow f;
            f215::Out o;
            double t = itemRun(f, 0.0, 3.0, level + 0.3, &o);
            const double y0 = o.y;
            bool held = true;
            for (int i = 0; i < 90; ++i, t += kDt) {
                f215::In in = itemCall(t, level + 0.1);
                in.inSights = true;
                o = f.step(in);
                held = held && o.y == y0 && o.state == f215::kHeldSights;
            }
            check(held, "F217: the sights hold y while the camera moves on");
            double prev = o.y, worst = 0.0;
            for (int i = 0; i < 90; ++i, t += kDt) {
                o = f.step(itemCall(t, level + 0.1));
                worst = std::fmax(worst, std::fabs(o.y - prev));
                prev = o.y;
            }
            check(worst <= f215::kCatchMps * kDt + 1e-9 && std::fabs(o.y - 0.1) < 0.011, "F217: leaving the sights catches at 0.6 m/s, not 3, and arrives");
        }
        {   // the aim pitched up holds; the aim pitched down still follows
            f215::Follow f;
            f215::Out o;
            double t = itemRun(f, 0.0, 3.0, level + 0.3, &o);
            const double y0 = o.y;
            f215::In up = itemCall(t, level + 0.1);
            up.aimPitchDeg = 25.0;
            o = f.step(up);
            check(o.state == f215::kHeldAim && o.y == y0, "F217: an aim pitched up 25 deg holds y");
            f215::In edge = itemCall(t + kDt, level + 0.3);
            edge.aimPitchDeg = 19.9;
            check(f.step(edge).state == f215::kActiveItem, "F217: ... and 19.9 deg does not");
            f215::In down = itemCall(t + 2 * kDt, level + 0.1);
            down.aimPitchDeg = -40.0;
            check(f.step(down).state == f215::kActiveItem, "F217: an aim pitched down 40 deg still follows");
            f215::In none = itemCall(t + 3 * kDt, level + 0.1);
            none.aimPitchDeg = std::nan("");
            check(f.step(none).state == f215::kActiveItem, "F217: an aim not known does not hold");
        }
        {   // head and pose
            f215::Follow f;
            f215::Out o;
            double t = itemRun(f, 0.0, 3.0, level + 0.3, &o);
            const double y0 = o.y;
            f215::In head = itemCall(t, level + 0.1);
            head.headPitchDeg = 30.0;
            o = f.step(head);
            check(o.state == f215::kActiveItem && o.headPitched && o.y != y0, "F226: a head pitched 30 deg no longer holds y with an item at the hip: the move follows, and the call says the head was past the gate");
            const double y1 = o.y;   // F226: the head call moved y
            f215::In pose = itemCall(t + kDt, level + 0.1);
            pose.poseValid = false;
            o = f.step(pose);
            check(o.state == f215::kHeldPose && o.y == y1, "F217: no valid pose holds y with an item at the hip");
        }
        {   // postureW releases toward 0 at the catch rate
            f215::Follow f;
            f215::Out o;
            double t = itemRun(f, 0.0, 3.0, level + 0.3, &o);
            double prev = o.y;
            bool rateOk = true;
            for (int i = 0; i < 90 * 2; ++i, t += kDt) {
                f215::In in = itemCall(t, level + 0.3);
                in.lowPostureItem = true;
                o = f.step(in);
                rateOk = rateOk && prev - o.y <= f215::kCatchMps * kDt + 1e-9 && o.state == f215::kReleasedPosture;
                prev = o.y;
            }
            check(rateOk && o.y == 0.0, "F217: postureW 0.6 with an item at the hip releases y to 0 at the catch rate");
        }
        {   // drawing on a slope: unarmed active at 30 cm, then the item: no jump, y stays near its value
            f215::Follow f;
            f215::Out o;
            double t = run(f, 0.0, 2.5, 1.54, c + 0.3, &o);
            const double y0 = o.y;
            const double eye = 1.54 + c + 0.3;   // the true camera on the slope, the same in the next calls
            double prev = y0, worst = 0.0;
            bool changed = false;
            for (int i = 0; i < 90; ++i, t += kDt) {
                o = f.step(itemCall(t, eye));
                changed = changed || o.state == f215::kActiveItem;
                worst = std::fmax(worst, std::fabs(o.y - prev));
                prev = o.y;
            }
            check(changed && worst <= f215::kCatchMps * kDt + 1e-9 && std::fabs(o.y - y0) < 0.03, "F217: drawing an item on a slope keeps y within 3 cm of its unarmed value and moves it at the catch rate only");
        }
        {   // the key off releases
            f215::Follow f;
            f215::Out o;
            double t = itemRun(f, 0.0, 3.0, level + 0.3, &o);
            bool rateOk = true;
            double prev = o.y;
            for (int i = 0; i < 90; ++i, t += kDt) {
                f215::In in = itemCall(t, level + 0.3);
                in.keyOn = false;
                o = f.step(in);
                rateOk = rateOk && prev - o.y <= f215::kCatchMps * kDt + 1e-9 && o.state == f215::kOff;
                prev = o.y;
            }
            check(rateOk && o.y == 0.0, "F217: the key off releases an item's y to exactly 0");
        }
        check(f215::settleHeld(f215::kHeldAim) && f215::settleHeld(f215::kHeldPose) && f215::settleHeld(f215::kFilling) && f215::settleHeld(f215::kHeldSights) &&
                  !f215::settleHeld(f215::kActive) && !f215::settleHeld(f215::kActiveItem) && !f215::settleHeld(f215::kOff) && !f215::settleHeld(f215::kReleasedAway) && !f215::settleHeld(f215::kReleasedPosture) &&
                  !f215::settleHeld(f215::kNotRun),
              "F217: the settle is not taken in a state that holds y");
        {
            float m[16] = {};
            m[13] = 0.25f;
            float same[16];
            std::memcpy(same, m, sizeof(m));
            f215::shiftWorldToModel(same, 0.0);
            check(std::memcmp(same, m, sizeof(m)) == 0, "F217: a shift of 0 leaves the world-to-model matrix bit for bit");
            f215::shiftWorldToModel(m, 0.3);
            check(std::fabs(static_cast<double>(m[13]) - (0.25 - 0.3)) < 1e-6 && m[12] == 0.0f && m[14] == 0.0f, "F217: the shift is taken off the y translation alone");
        }
    }

    // ---- 11. the sites ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
    {
        const std::string cpp = slurp("src/d3d11/native_on_foot.cpp"), ini = slurp("edvr.ini"), rec = slurp("src/common/onfoot_recorder_core.h");
        const auto has = [&](const std::string& hay, const char* needle) { return hay.find(needle) != std::string::npos; };
        const auto count = [&](const std::string& hay, const char* needle) {
            int n = 0;
            for (size_t at = hay.find(needle); at != std::string::npos; at = hay.find(needle, at + 1)) ++n;
            return n;
        };
        check(has(cpp, "cfg.getString(\"experimental.native_on_foot_body_follows_view\", \"on\")") && has(cpp, "g_f215Key.store(on, std::memory_order_relaxed)") &&
                  has(cpp, "bi.keyOn = g_f215Key.load(std::memory_order_relaxed)"),
              "F215: the key is read on the frame thread, stored in an atomic and loaded by the pin hook");
        check(has(ini, "# ui: Body follows view | choices on, off | recommended on | live\nnative_on_foot_body_follows_view = on"), "F215: the shipped key is on, with its ui line, and says TEMPORARY");
        check(has(ini, "# TEMPORARY (F215, F217): Body follows view:"), "F215: the key's text says TEMPORARY");
        check(count(cpp, "guardedWritePoseJoint(sp.buf") == 6, "F215: the five pose writes of the hook and F217's whole-pose fallback use the shifted write");
        check(count(cpp, "guardedWriteFloats16(sp.buf") == 0, "F215: no pose write into the span bypasses the shift");
        check(has(cpp, "guardedWriteFloats16(buf + static_cast<uint64_t>(pl.member[k]) * 64u, g_pfzChain[k])"), "F215: the pose freeze's own write stays unshifted");
        const size_t top = cpp.find("g_f215WriteY = 0.0;   // F215: no shift on any pose write of this call"), arm = cpp.find("void armIkAfterAnimate("), set = cpp.find("g_f215WriteY = pc.f215BodyY;");
        check(arm != std::string::npos && top != std::string::npos && top > arm && top - arm < 200 && set != std::string::npos && set < arm, "F215: the shift is 0 at the top of armIkAfterAnimate and set only where the whole pose is written");
        check(has(cpp, "si.camY = camLevelY;") && has(cpp, "const double camLevelY = pc.eyeYSelected;") && has(cpp, "si.neckY = N0[1] + f97W * std::max(pc.f97RaiseF93, 0.0);"),
              "F215: the settle sees the level camera and the animated neck");
        check(has(cpp, "pc.rawEyeY, N0[1], static_cast<double>(in.localHeadY), in.localPoseGeneration, localPitchValid, si.tS)") && has(cpp, "fi.rawEyeY = pc.rawEyeY;") &&
                  has(cpp, "g_f138.noteRunning(si.tS, g_f138HeldM, pc.rawEyeY, N0[1]") && !has(cpp, "pc.rawEyeY - pc.f215BodyY"),
              "F217: F138 sees the internal (level) camera in its stance, its running reference and its step, with no second subtraction");
        check(has(cpp, "nofoot::f215::shiftWorldToModel(invM, pc.f215BodyY);") && has(cpp, "nofoot::f215::shiftWorldToModel(G.g, yUsed);") && has(cpp, "bi.eyeY = pc.eyeYSelected + pc.f215BodyY - pc.f223PopM;") &&
                  !has(cpp, "pc.f215BodyY = bo.y;") && has(cpp, "nofoot::f215::settleHeld(bo.state) || bo.headPitched)) si.valid = false;"),
              "F217: the world-to-model matrix and G enter less the shift in force, the step reads the true camera, its result takes effect next call, and a held state takes no settle");
        check(has(cpp, "r.eye_y_selected = pc.eyeYSelected + pc.f215BodyY;") && has(cpp, "r.raw_eye_y = pc.rawEyeY + pc.f215BodyY;") && has(cpp, "r.e[1] += pc.f215BodyY;"), "F217: the recording keeps the true camera");
        check(has(cpp, "xi.keyOn = g_f214Key.load(std::memory_order_relaxed) && !g_f215Key.load(std::memory_order_relaxed);"), "F215: F214 is inert while the key is on");
        const size_t f214Step = cpp.find("g_f214.step(xi)"), f215Pub = cpp.find("f94TargetPublish(nofoot::f215::levelRaiseM(f97H), now)");
        check(f214Step != std::string::npos && f215Pub != std::string::npos && f215Pub > f214Step && has(cpp, "if (pc.f215State == nofoot::f215::kActive && freshIn && !(f138Own || fo.publish))"),
              "F215: the constant raise is published only when the rule is active on a fresh frame and no other publisher took the call");
        check(has(cpp, "g_f215.reset();"), "F215: an edge of the on-foot camera resets the window and the shift");
        check(has(cpp, "r.f215_state = pc.f215State;") && has(cpp, "r.f215_body_y = pc.f215BodyY;") && has(cpp, "r.f215_target = pc.f215Target;") && has(rec, "E(F215State, f215_state)") &&
                  has(rec, "S(f64, f215_body_y)") && has(rec, "S(f64, f215_target)"),
              "F215: the pin record carries the shift, the target and the state");
        check(has(cpp, "uint8_t f215State = nofoot::f215::kNotRun;"), "F215: a call that never reached the step records not_run, not off");
        check(has(cpp, "nofoot::f215::formatLine(bodyFollowLine") && has(cpp, "g_f215Shifted.fetch_add(1") && has(cpp, "g_f215Shifted.exchange(0"), "F215: the window line is printed with the count of shifted pose writes");
        check(has(cpp, "str(\"experimental.native_on_foot_body_follows_view\"") && has(rec, "kF215StateEnum"), "F215: the key is in the recording's key list");
        // the heading of the row and what follows it
        const size_t heading = ini.find("# Native on foot: body on slopes\n"), f214Key = ini.find("native_on_foot_view_follows_body_moving = on"), f215Key = ini.find("native_on_foot_body_follows_view = on"),
                     ads = ini.find("# Native on foot: aiming down sights\n", f215Key == std::string::npos ? 0 : f215Key);
        check(heading != std::string::npos && f214Key != std::string::npos && f215Key != std::string::npos && heading < f214Key && f214Key < f215Key && ads != std::string::npos && ads > f215Key,
              "F215: F214's row and F215's sit under their own heading and the aiming heading comes back after them");
    }
}

}  // namespace f215test
