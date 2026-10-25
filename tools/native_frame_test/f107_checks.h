// F107 checks (native on-foot VR): the game's camera away from the commander. Header only, run by native_frame_test (run107).
// The figures are the carrier flight's (log edvr_gfx_20261025_044101): the carrier management screen's camera 3367 m from the neck (+739, +174, +3280 m in the
// model), the standing eye 1.58 m, the upright neck 1.543 m, the eye height key 15 cm.
#pragma once

#include "../../src/d3d11/native_on_foot_core.h"
#include "../../src/d3d11/native_on_foot_f94.h"
#include "../../src/d3d11/native_on_foot_f95.h"
#include "../../src/d3d11/native_on_foot_f107.h"

#include <cmath>
#include <limits>

namespace f107test {

namespace f94 = edvr::nofoot::f94;
namespace f95 = edvr::nofoot::f95;
namespace f107 = edvr::nofoot::f107;

template <class Check>
inline void run107(Check&& check) {
    const double neck[3] = {0.0, 1.543, 0.0};
    {   // the gate: the management screen's camera away, the commander's eye walking and in the largest second three flights had on
        const double screen[3] = {739.40, 1.543 + 174.25, 3280.48}, walking[3] = {0.023, 1.543 - 0.044, 0.173}, load[3] = {0.0, 1.543 - 0.94, 0.85};
        const double nan = std::numeric_limits<double>::quiet_NaN();
        const double notANumber[3] = {nan, nan, nan};
        check(!f107::onCommander(f107::eyeNeckDistance(screen, neck)) && f107::onCommander(f107::eyeNeckDistance(walking, neck)) &&
                  f107::onCommander(f107::eyeNeckDistance(load, neck)) && !f107::onCommander(f107::eyeNeckDistance(notANumber, neck)),
              "F107: the carrier management screen's camera (3367 m from the neck) is away; the eye walking (0.18 m) and a load's first second (1.27 m) are on; not a number is away");
    }
    {   // the episode: on, then away for five calls (one not a number), then on: one edge each way, its calls and distances
        f107::Away a;
        const double d[9] = {0.18, 0.20, 3367.4, 3367.6, std::numeric_limits<double>::quiet_NaN(), 139.2, 3367.5, 0.19, 0.18};
        int left = 0, back = 0;
        for (int i = 0; i < 9; ++i) {
            const f107::Away::Edge e = a.step(d[i], i * 0.011);
            left += e == f107::Away::kLeft;
            back += e == f107::Away::kBack;
        }
        check(left == 1 && back == 1 && a.episodes == 1 && a.calls == 5 && a.notNumber == 1 && std::fabs(a.nearestM - 139.2) < 1e-9 &&
                  std::fabs(a.farthestM - 3367.6) < 1e-9 && !a.away,
              "F107: one episode: left on the first call away, back on the first call on, 5 calls (1 not a number), the camera 139.2 to 3367.6 m from the neck");
    }
    {   // the neck reference through the screen: the settle takes no sample from a camera away and the raise ramps out (scope off), against the 1c26ea5f build
        const auto run = [&](bool gate, double* duringRaise, double* worstRaise, double* afterRaise) {
            f95::StanceSettle settle;
            f94::NeckRaise nr;
            f94::In in;
            in.dt = 1.0 / 90.0;
            in.settleS = 0.1;
            *worstRaise = 0.0;
            for (int i = 0; i < 720; ++i) {   // 8 s: on 0-2 s, the screen 2-5 s, on again 5-8 s
                const double t = i / 90.0;
                const bool screen = t >= 2.0 && t < 5.0;
                const double eye[3] = {screen ? 739.40 : 0.02, screen ? 1.543 + 174.25 : 1.58, screen ? 3280.48 : 0.17};
                const bool away = !f107::onCommander(f107::eyeNeckDistance(eye, neck));
                f95::SettleIn si;
                si.valid = !(gate && away);
                si.still = true;
                si.tS = t;
                si.neckY = neck[1];
                si.camY = eye[1];
                const f95::SettleOut so = settle.step(si);
                in.scope = !(gate && away);
                in.targetFresh = so.settled;
                if (so.settled) in.targetM = f94::raiseTarget(so.neckY, 0.15, so.camY);
                const f94::Out o = nr.step(in);
                *worstRaise = std::fmin(*worstRaise, o.raiseM);
                if (i == 440) *duringRaise = o.raiseM;   // 4.9 s, the screen
                if (i == 719) *afterRaise = o.raiseM;
            }
        };
        double during = 0.0, worst = 0.0, after = 0.0, duringOld = 0.0, worstOld = 0.0, afterOld = 0.0;
        run(true, &during, &worst, &after);
        run(false, &duringOld, &worstOld, &afterOld);
        check(std::fabs(during) < 0.0005 && worst > -0.0005 && std::fabs(after - (1.543 + 0.15 - 1.58)) < 0.0015,
              "F107: through the screen the raise ramps out to 0 (the view is the game's camera), never below it, and is back at the commander's +11.3 cm after");
        check(duringOld < -0.49 && worstOld < -0.49, "F107 (control): the 1c26ea5f build settles on the screen's camera and holds the raise at its -50 cm bound");
    }
    {   // the standing calibration: taken on the screen's camera, every later standing call reads 174 m under it and is binned crouched (the flight from 05:00:24)
        const double hScreen = 1.543 + 174.25, hStand = 1.58, eyeNow = 1.58;
        check(edvr::nofoot::pinCrouchWeight(true, hScreen, eyeNow) >= 1.0 && edvr::nofoot::pinCrouchWeight(true, hStand, eyeNow) == 0.0,
              "F107 (control): a standing calibration taken on the screen's camera makes every standing call crouched (w_c 1); the commander's own leaves it 0");
    }
}

}  // namespace f107test
