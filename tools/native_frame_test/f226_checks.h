// F226 checks (native on-foot VR), run226 below. Header only, run by native_frame_test.
//
// F226 (native_on_foot_f215.h, native_on_foot.cpp): the head's pitch no longer holds F215's whole-body shift. A head past kHeadGateDeg falls through to the same rule as one inside it, and Out::headPitched
// says the call reached the place the old head check stood with the head past the gate, so the pin still refuses the stance's settle. The rig runs the flight's landing (a camera dip with the head at
// -37 deg) armed and unarmed, the flag at the gate and in every state that does not reach it, a non-fresh call and a reset, that crossing the gate is not a change of state, the log line and the sites.
#pragma once

#include "../../src/d3d11/native_on_foot_f215.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

namespace f226test {

namespace f215 = edvr::nofoot::f215;

inline std::string slurp226(const char* path) {
    std::ifstream f(path, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

constexpr double kDt226 = 1.0 / 90.0;

// A call with an item at the hip (armed) or unarmed, the head at headDeg, a fresh frame.
inline f215::In call226(bool armed, double tS, double eyeY, double headDeg) {
    f215::In in;
    in.keyOn = true;
    in.hipItem = armed;
    in.poseValid = true;
    in.fresh = true;
    in.headPitchDeg = headDeg;
    in.tS = tS;
    in.eyeY = eyeY;
    in.neckY = 1.40;
    in.eyeHeightM = 0.15;
    return in;
}

// The flight's landing: E 1.62 to 1.15 over 0.3 s, back to 1.47 over 0.4 s, then steady. Returns E at call n counted from the start of the dip.
inline double landingE(int n) {
    const int down = static_cast<int>(0.3 / kDt226 + 0.5), up = static_cast<int>(0.4 / kDt226 + 0.5);
    if (n < 0) return 1.62;
    if (n < down) return 1.62 + (1.15 - 1.62) * (n + 1) / down;
    if (n < down + up) return 1.15 + (1.47 - 1.15) * (n - down + 1) / up;
    return 1.47;
}

template <class Check>
inline void run226(Check&& check) {
    char msg[400];
    const double level = f215::kDefaultLevelCamM;
    // ---- 1. the landing, armed -------------------------------------------------------------------------------------------------------------------------------------------------------------------
    {
        f215::Follow f;
        f215::Out o;
        double t = 0.0;
        for (int i = 0; i < 270; ++i, t += kDt226) o = f.step(call226(true, t, 1.62, -37.0));
        bool heldAny = false, pitchedAll = true;
        double worstAfter = 0.0, minY = 1e9;
        const int bottom = static_cast<int>(0.3 / kDt226 + 0.5), after = bottom + static_cast<int>(0.75 / kDt226 + 0.5);
        for (int n = 0; n < 300; ++n, t += kDt226) {
            const double e = landingE(n);
            o = f.step(call226(true, t, e, -37.0));
            heldAny = heldAny || o.state != f215::kActiveItem;
            pitchedAll = pitchedAll && o.headPitched;
            minY = std::fmin(minY, o.y);
            if (n >= after) worstAfter = std::fmax(worstAfter, std::fabs(o.y - (e - level)));
        }
        std::snprintf(msg, sizeof(msg), "F226: armed, the landing with the head at -37 deg: no call held (%d), y reached %.2f m and is within %.1f mm of E less the level 0.75 s after the bottom", heldAny ? 1 : 0, minY, worstAfter * 1000.0);
        check(!heldAny && pitchedAll && minY < -0.40 && worstAfter < 0.01, msg);
    }
    // ---- 2. the same, unarmed, with the neck window filling while the head is pitched ----------------------------------------------------------------------------------------------------------
    {
        f215::Follow f;
        f215::Out o;
        double t = 0.0;
        bool heldAny = false, filledThenActive = true;
        int firstActive = -1;
        for (int i = 0; i < 270; ++i, t += kDt226) {
            o = f.step(call226(false, t, 1.40 + f215::kCamOverNeckM, -37.0));
            heldAny = heldAny || (o.state != f215::kFilling && o.state != f215::kActive);
            if (firstActive < 0 && o.state == f215::kActive) firstActive = i;
            if (firstActive >= 0) filledThenActive = filledThenActive && o.state == f215::kActive;
        }
        double worstAfter = 0.0, minY = 1e9;
        const int bottom = static_cast<int>(0.3 / kDt226 + 0.5), after = bottom + static_cast<int>(0.75 / kDt226 + 0.5);
        for (int n = 0; n < 300; ++n, t += kDt226) {
            const double e = landingE(n) - 1.62 + 1.40 + f215::kCamOverNeckM;
            o = f.step(call226(false, t, e, -37.0));
            heldAny = heldAny || o.state != f215::kActive;
            minY = std::fmin(minY, o.y);
            if (n >= after) worstAfter = std::fmax(worstAfter, std::fabs(o.y - (e - 1.40 - f215::kCamOverNeckM)));
        }
        std::snprintf(msg, sizeof(msg), "F226: unarmed, the window fills with the head pitched (first active call %d), no call held, y reached %.2f m and is within %.1f mm of its target 0.75 s after the bottom", firstActive, minY, worstAfter * 1000.0);
        check(!heldAny && filledThenActive && firstActive >= 17 && firstActive <= 20 && minY < -0.40 && worstAfter < 0.01, msg);
    }
    // ---- 3. the flag ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------
    for (int armed = 0; armed <= 1; ++armed) {
        const auto out = [&](double head, void (*mod)(f215::In&)) {
            f215::Follow f;
            double t = 0.0;
            f215::Out o;
            for (int i = 0; i < 3; ++i, t += kDt226) o = f.step(call226(armed != 0, t, 1.50, 0.0));
            f215::In in = call226(armed != 0, t, 1.50, head);
            if (mod) mod(in);
            return f.step(in);
        };
        const char* who = armed ? "armed" : "unarmed";
        std::snprintf(msg, sizeof(msg), "F226: %s, the flag is true at +20 and -20 deg, false at 19.9 and -19.9", who);
        check(out(20.0, nullptr).headPitched && out(-20.0, nullptr).headPitched && !out(19.9, nullptr).headPitched && !out(-19.9, nullptr).headPitched && out(60.0, nullptr).headPitched, msg);
        std::snprintf(msg, sizeof(msg), "F226: %s, the flag is false where the old head check was not reached: low posture, key off, away, sights, an aim hold and a head that is not a number", who);
        const f215::Out low = out(40.0, [](f215::In& i) { i.lowPosture = true; i.lowPostureItem = true; });
        const f215::Out off = out(40.0, [](f215::In& i) { i.keyOn = false; });
        const f215::Out away = out(40.0, [](f215::In& i) { i.away = true; });
        const f215::Out sights = out(40.0, [](f215::In& i) { i.inSights = true; i.hipItem = false; });
        const f215::Out aim = out(40.0, [](f215::In& i) { i.aimPitchDeg = 30.0; i.aimRefused = true; });
        const f215::Out nan = out(std::nan(""), nullptr);
        check(!low.headPitched && !off.headPitched && !away.headPitched && !sights.headPitched && !aim.headPitched && !nan.headPitched && nan.state == f215::kHeldPose && sights.state == f215::kHeldSights &&
                  aim.state == f215::kHeldAim && low.state == f215::kReleasedPosture && off.state == f215::kOff && away.state == f215::kReleasedAway,
              msg);
        std::snprintf(msg, sizeof(msg), "F226: %s, no pose is held_pose with the flag false", who);
        const f215::Out nopose = out(40.0, [](f215::In& i) { i.poseValid = false; });
        check(nopose.state == f215::kHeldPose && !nopose.headPitched, msg);
    }
    // ---- 4. a call that is not new keeps it; reset clears it ---------------------------------------------------------------------------------------------------------------------------------------
    {
        f215::Follow f;
        double t = 0.0;
        f215::Out o = f.step(call226(true, t, 1.50, 30.0));
        f215::In again = call226(true, t, 1.50, 0.0);
        again.fresh = false;
        o = f.step(again);
        const bool kept = o.headPitched;
        o = f.step(call226(true, t + kDt226, 1.50, 0.0));
        f215::In again2 = call226(true, t + 2 * kDt226, 1.50, 30.0);
        again2.fresh = false;
        const bool keptFalse = !f.step(again2).headPitched;
        f.step(call226(true, t + 3 * kDt226, 1.50, 30.0));
        f.reset();
        f215::In none = call226(true, t + 4 * kDt226, 1.50, 30.0);
        none.fresh = false;
        check(kept && !o.headPitched && keptFalse && !f.step(none).headPitched, "F226: a call that is not new returns the last flag, and reset clears it");
    }
    // ---- 5. crossing the gate is not a change of state ----------------------------------------------------------------------------------------------------------------------------------------------
    for (int armed = 0; armed <= 1; ++armed) {
        double stepAt[2] = {};
        for (int head = 0; head <= 1; ++head) {
            f215::Follow f;
            double t = 0.0;
            f215::Out o;
            const double neckOver = 1.40 + f215::kCamOverNeckM;
            const double e0 = armed ? level : neckOver;
            for (int i = 0; i < 400; ++i, t += kDt226) o = f.step(call226(armed != 0, t, e0, 2.0));
            const double y0 = o.y;
            o = f.step(call226(armed != 0, t, e0 - 0.30, head ? 30.0 : 2.0));
            stepAt[head] = y0 - o.y;
        }
        std::snprintf(msg, sizeof(msg), "F226: %s, the head crossing the gate starts no catching: y steps %.2f mm with the head at 30 deg, %.2f mm inside the gate (the track rate is 3 m/s)", armed ? "armed" : "unarmed",
                      stepAt[1] * 1000.0, stepAt[0] * 1000.0);
        check(std::fabs(stepAt[1] - stepAt[0]) < 1e-9 && std::fabs(stepAt[1] - f215::kTrackMps * kDt226) < 1e-6, msg);
    }
    // ---- 6. the log line ----------------------------------------------------------------------------------------------------------------------------------------------------------------------
    {
        f215::Stats s;
        for (int i = 0; i < 5; ++i) {
            f215::Call c;
            c.state = f215::kActive;
            c.keyOn = true;
            c.unarmed = true;
            c.yM = 0.1;
            c.headPitched = i < 3;
            s.add(c);
        }
        char line[900];
        f215::formatLine(line, sizeof(line), 10, s, 4);
        check(s.headPitched == 3 && std::strstr(line, "pin calls 5, unarmed 5, active 5, head past the gate with the move on 3, held aim 0 pose 0, filling 0, released item 0 posture 0 away 0;") != nullptr,
              "F226: the F215 line counts the calls with the head past the gate and the move on, where it counted held head");
    }
    // ---- 7. the sites ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------
    {
        const std::string cpp = slurp226("src/d3d11/native_on_foot.cpp"), rec = slurp226("src/common/onfoot_recorder_core.h");
        const auto has = [&](const char* needle) { return cpp.find(needle) != std::string::npos; };
        check(has("if (bi.keyOn && (nofoot::f215::settleHeld(bo.state) || bo.headPitched)) si.valid = false;") && has("pc.f226HeadPitched = bo.headPitched;") && has("bc.headPitched = bo.headPitched;"),
              "F226: the pin refuses the stance's settle on bo.headPitched, and records and counts it");
        check(has("r.f226_head_pitched = pc.f226HeadPitched ? 1 : 0;") && rec.find("S(u8, f226_head_pitched)") != std::string::npos && rec.find("\"held_head\", \"filling\", \"active\"") == std::string::npos,
              "F226: the pin record carries the flag and the F215 states no longer name held_head");
    }
}

}  // namespace f226test
