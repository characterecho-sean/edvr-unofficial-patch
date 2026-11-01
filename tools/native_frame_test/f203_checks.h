// F203 checks (native on-foot VR), run203 below. Header only, run by native_frame_test.
//
// F203 (native_on_foot_f203.h, native_on_foot.cpp adsCentreTick): an ADS press under mouse look with the ADS head aim keeps the sight where the mouse put it; F69i's snap injects only the part of the
// head-to-sight offset beyond 15 deg on each axis (F195's constant). The pilot's flight (edvr_gfx_20261101_041020, Frontier, Pimax): press 1 read "the sight is yaw +3.65 pitch -0.28 deg from the head's
// forward ... injected yaw +3.65 pitch -0.28 deg" and press 9 injected yaw -0.15 pitch +3.50: the snap took the sight from the mouse's aim onto the headset's forward axis. The rig runs the rule on those two
// flown presses (inject nothing, keep all), on 20 deg (inject 5), on exactly 15 (inject 0), on offsets that are not numbers (inject all, the old behaviour), the cone bound (a zero ask is not pulled in), the
// unwind of a zero injection, the line and the proof, and the sites.
#pragma once

#include "../../src/d3d11/native_on_foot_core.h"
#include "../../src/d3d11/native_on_foot_f195.h"
#include "../../src/d3d11/native_on_foot_f203.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>

namespace f203test {

namespace nf = edvr::nofoot;
namespace f195 = edvr::nofoot::f195;
namespace f203 = edvr::nofoot::f203;

inline bool eq(double a, double b, double tol) { return std::fabs(a - b) <= tol; }
inline std::string slurp(const char* path) {
    std::ifstream f(path, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

template <class Check>
void run203(Check&& check) {
    constexpr double kR = nf::kAdsDeg;
    // ---- 1. the rule on the flown presses and the edges ---------------------------------------------------------------------------------------------------------------------------------------------
    {
        check(f203::kKeepDeg == f195::kKeepPitchDeg && f203::kKeepDeg == 15.0, "F203: the keep is F195's constant, 15 deg");
        const f203::Plan p1 = f203::plan(3.65, -0.28, 0.0), p9 = f203::plan(-0.15, 3.50, 0.0);
        check(p1.yawDeg == 0.0 && p1.pitchDeg == 0.0 && eq(p1.keptYawDeg, 3.65, 1e-12) && eq(p1.keptPitchDeg, -0.28, 1e-12) && !p1.bounded,
              "F203: the flown press 1 (the sight yaw +3.65 pitch -0.28 deg from the head's forward) injects 0 and keeps all of it");
        check(p9.yawDeg == 0.0 && p9.pitchDeg == 0.0 && eq(p9.keptYawDeg, -0.15, 1e-12) && eq(p9.keptPitchDeg, 3.50, 1e-12),
              "F203: the flown press 9 (yaw -0.15 pitch +3.50) injects 0 and keeps all of it");
        const f203::Plan yaw20 = f203::plan(20.0, 0.0, 0.0), pitch20 = f203::plan(0.0, -20.0, 0.0), both = f203::plan(-25.0, 17.5, 0.0);
        check(eq(yaw20.yawDeg, 5.0, 1e-12) && yaw20.pitchDeg == 0.0 && eq(yaw20.keptYawDeg, 15.0, 1e-12), "F203: 20 deg of yaw injects 5 and keeps 15");
        check(yaw20.keptPitchDeg == 0.0 && eq(pitch20.pitchDeg, -5.0, 1e-12) && eq(pitch20.keptPitchDeg, -15.0, 1e-12) && pitch20.yawDeg == 0.0, "F203: 20 deg of pitch down injects -5 and keeps -15");
        check(eq(both.yawDeg, -10.0, 1e-12) && eq(both.pitchDeg, 2.5, 1e-12) && eq(both.keptYawDeg, -15.0, 1e-12) && eq(both.keptPitchDeg, 15.0, 1e-12), "F203: each axis is judged alone (yaw -25 injects -10, pitch +17.5 injects 2.5)");
        const f203::Plan at = f203::plan(15.0, -15.0, 0.0), over = f203::plan(15.01, 0.0, 0.0);
        check(at.yawDeg == 0.0 && at.pitchDeg == 0.0 && at.keptYawDeg == 15.0 && at.keptPitchDeg == -15.0, "F203: exactly 15 deg (and -15) injects exactly 0 and keeps it whole");
        check(eq(over.yawDeg, 0.01, 1e-9) && eq(over.keptYawDeg, 15.0, 1e-9), "F203: 15.01 deg injects the 0.01 beyond");
        // inject plus kept is the offset, on its side, and the kept part never passes 15 deg, from -80 to +80 deg
        bool sum = true;
        for (int i = -400; i <= 400; ++i) {
            const double off = i * 0.2;
            const f203::Axis a = f203::keepAxis(off);
            sum = sum && eq(a.injectDeg + a.keptDeg, off, 1e-12) && std::fabs(a.keptDeg) <= 15.0 + 1e-12 && a.injectDeg * a.keptDeg >= 0.0;
        }
        check(sum, "F203: injected plus kept is the offset, never past 15 deg kept, both on the offset's side, for every offset from -80 to +80 deg");
    }
    // ---- 2. an offset that is not a number: all of it injected, the old behaviour ------------------------------------------------------------------------------------------------------------------------
    {
        const double nan = std::numeric_limits<double>::quiet_NaN(), inf = std::numeric_limits<double>::infinity();
        const f203::Axis n = f203::keepAxis(nan), pi = f203::keepAxis(inf), ni = f203::keepAxis(-inf);
        check(!std::isfinite(n.injectDeg) && n.keptDeg == 0.0 && pi.injectDeg == inf && pi.keptDeg == 0.0 && ni.injectDeg == -inf && ni.keptDeg == 0.0,
              "F203: an offset that is not a number keeps nothing and injects it as it came (the snap ignores what is not finite: F69i's behaviour)");
        nf::AdsCentre c;
        c.begin(n.injectDeg * kR, 0.0);
        check(!c.active, "F203: ... and a snap given it does nothing");
    }
    // ---- 3. the cone bound: it judges what is injected, and a zero ask is not pulled in -----------------------------------------------------------------------------------------------------------------
    {
        // F69i's bound (the weapon keeps 3 deg off 40): 30 deg off the body and asked +9 gets +7. With 19 deg of offset only 4 is asked, which is inside the band and goes whole.
        const f203::Plan cut = f203::plan(24.0, 0.0, 30.0), whole = f203::plan(19.0, 0.0, 30.0);
        check(cut.bounded && eq(cut.yawDeg, 7.0, 1e-12) && eq(cut.keptYawDeg, 17.0, 1e-12), "F203: 9 deg to inject (24 less the 15 kept) from a weapon 30 deg off the body is bounded by the cone to +7, the rest stays");
        check(!whole.bounded && eq(whole.yawDeg, 4.0, 1e-12) && eq(whole.keptYawDeg, 15.0, 1e-12), "F203: 4 deg to inject from the same weapon is inside the band and goes whole");
        // a weapon already 38 deg off the body (past the band) and a settled press: nothing to inject, and the bound does not pull it back (nf::adsCentreYawBounded on a zero ask would return -1)
        bool b = false;
        const double pulled = nf::adsCentreYawBounded(38.0, 0.0, &b);
        const f203::Plan settled = f203::plan(3.65, -0.28, 38.0);
        check(pulled != 0.0 && b, "F203: control: the bound alone, asked for 0 by a weapon past the band, would inject a pull-in");
        check(settled.yawDeg == 0.0 && settled.pitchDeg == 0.0 && !settled.bounded, "F203: a settled press from a weapon 38 deg off the body injects nothing (the zero ask skips the bound)");
    }
    // ---- 4. the unwind of a zero injection is zero --------------------------------------------------------------------------------------------------------------------------------------------------------
    {
        nf::AdsAimMap map;
        map.beginPress();
        const f203::Plan p = f203::plan(3.65, -0.28, 0.0);
        map.beginCentre(p.yawDeg * kR, p.pitchDeg * kR);
        check(!map.centre.active, "F203: a snap of 0 and 0 is not begun");
        double injected = 0.0;
        for (int i = 0; i < 20; ++i) {   // the press's first frames: engaged, no head step
            const nf::AdsAimMap::Out o = map.step(true, false, 1.0 / 90.0, 1.0, false, 0.0, 0.0, 0.0, 0.0, true, true);
            injected += std::fabs(o.injYaw) + std::fabs(o.injPitch);
        }
        check(injected == 0.0 && map.yawCarried == 0.0 && map.pitchSeat == 0.0 && map.yawInFlight() == 0.0 && map.pitchInFlight() == 0.0, "F203: a press that injects nothing carries nothing");
        map.beginRelease();
        double unwound = 0.0;
        bool done = false;
        for (int i = 0; i < 90 * 2 && !done; ++i) {
            const nf::AdsAimMap::Out o = map.step(false, true, 1.0 / 90.0, 0.0, false, 0.0, 0.0, 0.0, 0.0, true, true);
            unwound += std::fabs(o.injYaw) + std::fabs(o.injPitch);
            done = map.releaseDone();
        }
        check(done && unwound == 0.0 && map.relY0 == 0.0 && map.relP0 == 0.0, "F203: the release of a zero injection unwinds nothing and is done as soon as its schedule has run");
        // a partial snap (20 deg of yaw: 5 injected) unwinds exactly the 5, not the 20
        nf::AdsAimMap m2;
        m2.beginPress();
        const f203::Plan p20 = f203::plan(20.0, 0.0, 0.0);
        m2.beginCentre(p20.yawDeg * kR, p20.pitchDeg * kR);
        double ask = 0.0;
        for (int i = 0; i < 40; ++i) {
            const nf::AdsAimMap::Out o = m2.step(true, false, 1.0 / 90.0, 1.0, false, 0.0, 0.0, 0.0, 0.0, true, true);
            ask += o.injYaw;
        }
        check(eq(ask, 5.0 * kR, 1e-9), "F203: a 20 deg offset asks the game for 5 deg of yaw in all");
        m2.yawCarried = ask;   // the game applied it: the cancel's account (the plant is the rig's elsewhere)
        m2.yawSeat = ask;
        m2.beginRelease();
        check(eq(m2.relY0, 5.0 * kR, 1e-9) && m2.relP0 == 0.0, "F203: and the release unwinds that 5 deg, not the 20");
    }
    // ---- 5. the line and the proof ------------------------------------------------------------------------------------------------------------------------------------------------------------------
    {
        char buf[160] = {};
        f203::Plan p = f203::plan(3.65, -0.28, 0.0);
        f203::keptText(buf, sizeof(buf), p);
        check(std::strcmp(buf, "kept yaw +3.65 pitch -0.28 deg (the mouse's aim, F203), ") == 0, "F203: the press line says what was kept: \"kept yaw +3.65 pitch -0.28 deg (the mouse's aim, F203), \" then the injected 0");
        check(eq(f203::proofErrorDeg(3.65, -0.28, p.keptYawDeg, p.keptPitchDeg), 0.0, 1e-12) && eq(f203::proofErrorDeg(3.90, -0.28, p.keptYawDeg, p.keptPitchDeg), 0.25, 1e-9),
              "F203: the proof is the sight's offset against the kept one: a sight still at +3.65/-0.28 is 0 off, one 0.25 deg further is 0.25 off (the proof: under 1 deg)");
        const f203::Plan big = f203::plan(20.0, 0.0, 0.0);
        check(f203::proofErrorDeg(15.2, 0.1, big.keptYawDeg, big.keptPitchDeg) < 1.0 && f203::proofErrorDeg(0.0, 0.0, big.keptYawDeg, big.keptPitchDeg) > 14.0,
              "F203: after a 20 deg snap the sight at 15.2 deg is near the kept 15 (the old test against 0 would have called it a failure)");
    }
    // ---- 6. the sites --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
    {
        const std::string cpp = slurp("src/d3d11/native_on_foot.cpp");
        const auto has = [&](const std::string& needle) { return cpp.find(needle) != std::string::npos; };
        const size_t tick = cpp.find("void adsCentreTick(uint64_t now) noexcept {");
        const std::string body = tick == std::string::npos ? std::string() : cpp.substr(tick, 16000);
        const auto in = [&](const std::string& needle) { return body.find(needle) != std::string::npos; };
        check(!cpp.empty() && !body.empty(), "F203: the source the pins read is readable from the repo root");
        check(has("#include \"native_on_foot_f203.h\"") && in("keep = nofoot::f203::plan(m.yawDeg, m.pitchDeg, offYaw);") && in("wantY = keep.yawDeg;") && in("wantP = keep.pitchDeg;") &&
                  !in("wantY = nofoot::adsCentreYawBounded(offYaw, m.yawDeg, &bounded);"),
              "F203: the press's snap is the plan (the offset less the kept 15 deg), not the whole offset through the cone's bound");
        check(in("if (c.measureOnly) {") && in("note = \"; look to aim (F109): measured only, no snap (the aim is the head's)\";") && in("if (!c.measureOnly) {   // the kept offset") &&
                  in("nofoot::f203::keptText(keptText, sizeof(keptText), keep);"),
              "F203: look to aim's measure-only press is as it was, and says nothing of a keep");
        check(in("%sinjected yaw %+.2f \"") && in("nofoot::kAdsConeDeg, keptText, wantY, wantP,"), "F203: the press line gives the kept offset ahead of the injected one");
        check(in("nofoot::f203::proofErrorDeg(m.yawDeg, m.pitchDeg, c.keptYaw, c.keptPitch)") && in("kept %+.2f %+.2f; \"") && in("the proof: under 1 deg)%s"),
              "F203: the proof line checks the sight against the kept offset");
        check(in("g_adsMap.beginCentre(wantY * kD, wantP * kD);"), "F203: the snap is begun on the map with what is injected");
    }
}

}  // namespace f203test
