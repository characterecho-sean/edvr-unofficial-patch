// F184 checks (native on-foot VR), run184 below. Header only, run by native_frame_test.
//
// F184 (native_on_foot_f179.h, native_on_foot.cpp): in the sights nothing that moves the body reads the eye. The flight edvr_gfx_20261030_134445 (build d7e918d1): engaged at the aim clamp
// from the hip the guard pushed at its bound in every second and the standing raise sat at the legs' cap, the body feeding the camera the eye is read from. (a) F179b's guard acts at the hip
// armed alone (guardActs) and measures only unarmed and in the sights: the push it would make is counted, nothing moves. (b) F179a's standing raise in the sights is asked of the legs' room
// directly (legsRaiseAskedM: F97a's cap, by the standing share), with no eye in it. The rig runs the contexts through the guard (the sights move nothing and count, the hip armed pushes), the
// raise on the F179/F180 flights' scenes (the legs' room, the same with the eye moved 20 cm), two toy loops (the camera riding on the chest and on the neck: settled in one step with the change,
// running away without it), the lines, and the source sites.
#pragma once

#include "../../src/d3d11/native_on_foot_core.h"
#include "../../src/d3d11/native_on_foot_f97.h"
#include "../../src/d3d11/native_on_foot_f178.h"
#include "../../src/d3d11/native_on_foot_f179.h"
#include "f179_checks.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>

namespace f184test {

namespace nf = edvr::nofoot;
namespace f97 = edvr::nofoot::f97;
namespace f179 = edvr::nofoot::f179;

inline std::string slurp(const char* path) {
    std::ifstream f(path, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// One guard call as the pin makes it for a context: at the hip armed the guard acts (the target is pushed back), elsewhere it only measures (the push it would make, the target untouched).
struct Call {
    bool ok = false, bounded = false;
    double push = 0.0, would = 0.0, target[3] = {};
};
inline Call guardCall(int ctx, const f179test::Scene& s, double distM = 0.10) {
    Call c;
    const f179::Geometry g = f179::makeGeometry(true, s.eye, s.hx, s.hz, true, s.chest);
    c.ok = g.ok;
    for (int i = 0; i < 3; ++i) c.target[i] = s.target[i];
    if (!g.ok) return c;
    if (f179::guardActs(ctx)) {
        c.push = f179::guardTarget(g, s.n0, s.bias, c.target, distM, &c.bounded);
    } else {
        const nf::PinMove mv = nf::pinMove(s.n0, c.target);
        c.would = f179::deficitBoundedM(g.aheadWith(mv.th) + s.bias, distM, &c.bounded);
    }
    return c;
}

template <class Check>
inline void run184(Check&& check) {
    const auto eq = [](double a, double b, double tol) { return std::fabs(a - b) <= tol; };
    char msg[700];

    // ---- 1. the guard: the hip armed acts, the sights and the unarmed hip measure -------------------------------------------------------------------------------------------------------------
    {
        const int hipUnarmed = f179::contextOf(false, 0.0, 0.0), hipArmed = f179::contextOf(true, 0.0, 0.0), adsStanding = f179::contextOf(true, 1.0, 0.0), adsCrouched = f179::contextOf(true, 1.0, 1.0),
                  adsPress = f179::contextOf(true, 0.05, 0.0);
        check(!f179::guardActs(hipUnarmed) && f179::guardActs(hipArmed) && !f179::guardActs(adsStanding) && !f179::guardActs(adsCrouched) && !f179::guardActs(adsPress) &&
                  adsStanding == f179::kAdsStanding && adsCrouched == f179::kAdsCrouched && hipArmed == f179::kHipArmed,
              "F184: the guard acts at the hip armed alone (detach under 0.05); unarmed, standing or crouched in the sights, from the first 0.05 of the detach, it measures only");
        f179test::Scene s;   // the chest 6 cm behind the eye along the heading: 4 cm short of the 10
        s.chest[2] = -0.06; s.n0[2] = -0.06; s.target[2] = -0.06;
        const Call hip = guardCall(f179::kHipArmed, s), sights = guardCall(f179::kAdsStanding, s), crouched = guardCall(f179::kAdsCrouched, s), unarmed = guardCall(f179::kHipUnarmed, s);
        std::snprintf(msg, sizeof(msg), "F184: the same scene (the chest 6 cm behind the eye): at the hip armed the target is pushed back %.1f cm; in the sights, crouched or standing, and unarmed it is left bit for bit and the push it would make (%.1f cm) is counted",
                      100.0 * hip.push, 100.0 * sights.would);
        check(hip.ok && eq(hip.push, 0.04, 1e-12) && eq(hip.target[2], -0.06 - 0.04, 1e-12) && sights.push == 0.0 && eq(sights.would, 0.04, 1e-12) && std::memcmp(sights.target, s.target, sizeof(s.target)) == 0 &&
                  crouched.push == 0.0 && eq(crouched.would, 0.04, 1e-12) && std::memcmp(crouched.target, s.target, sizeof(s.target)) == 0 && unarmed.push == 0.0 && eq(unarmed.would, 0.04, 1e-12),
              msg);

        // a toy loop: the camera (and the drawn eye) ride on the chest, so the eye stays 6 cm ahead of it whatever the guard does. Acting it takes the same 4 cm back every call and the chest
        // runs away (the flight: the guard at its bound in every second); measuring only the chest stays and the figure is the same 4 cm every call.
        double chestAct = -0.06, chestMeasure = -0.06, wouldSum = 0.0;
        int moved = 0;
        for (int step = 0; step < 40; ++step) {
            f179test::Scene a;   // the eye rides: it is 6 cm ahead of the chest as it stood the call before
            a.eye[2] = chestAct + 0.06; a.chest[2] = chestAct; a.n0[2] = chestAct; a.target[2] = chestAct;
            const Call ca = guardCall(f179::kHipArmed, a);
            if (ca.push > 0.0) ++moved;
            chestAct = ca.target[2];
            f179test::Scene m;
            m.eye[2] = chestMeasure + 0.06; m.chest[2] = chestMeasure; m.n0[2] = chestMeasure; m.target[2] = chestMeasure;
            const Call cm = guardCall(f179::kAdsStanding, m);
            chestMeasure = cm.target[2];
            wouldSum += cm.would;
        }
        std::snprintf(msg, sizeof(msg), "F184: a toy loop (the camera rides on the chest): acting, the guard pushes every call and the chest has run %.2f m back after 40 calls; measuring only the chest has moved %.2f m and counted %.2f m of pushes it did not make",
                      chestAct, chestMeasure + 0.06, wouldSum);
        check(moved == 40 && chestAct < -1.5 && chestMeasure == -0.06 && eq(wouldSum, 40 * 0.04, 1e-9), msg);

        // the bound still applies to what it counts: a far deficit is the distance plus 10 cm and counted bounded
        f179test::Scene distant;
        distant.eye[2] = 0.0; distant.chest[2] = 0.5; distant.n0[2] = 0.5; distant.target[2] = 0.5;
        const Call fc = guardCall(f179::kAdsStanding, distant, 0.04);
        check(eq(fc.would, 0.14, 1e-12) && fc.bounded && fc.push == 0.0, "F184: what the sights count is bounded as a push was (the distance plus 10 cm) and flagged");
    }

    // ---- 2. the standing raise in the sights is asked of the legs --------------------------------------------------------------------------------------------------------------------------
    {
        // the F179 flight's scene (the legs hip to foot over the leg 0.895 on a 0.90 m leg), and the F180 flight's (edvr_gfx_20261030_103635: the neck written +13.2 cm, capped by the legs in 50 of 69
        // calls, asked +13.6 by the eye)
        const double room179 = f97::legRaiseRoom(0.805, 0.0, 0.45, 0.45, 0.805, nf::kStandReach);
        const double room180 = 0.132;   // the F180 flight's written raise at the cap: the legs' room there
        check(eq(f179::legsRaiseAskedM(room179, 0.0, 1.0), room179, 1e-15) && eq(room179, 0.0904, 1e-4) && eq(f179::legsRaiseAskedM(room180, 0.0, 1.0), 0.132, 1e-15) &&
                  f179::legsRaiseAskedM(room179, 0.0, 0.5) == 0.5 * room179 && f179::legsRaiseAskedM(room179, 1.0, 1.0) == 0.0 && f179::legsRaiseAskedM(room179, 0.0, 0.0) == 0.0 &&
                  f179::legsRaiseAskedM(std::nan(""), 0.0, 1.0) == 0.0 && f179::legsRaiseAskedM(-0.02, 0.0, 1.0) == 0.0 && f179::legsRaiseAskedM(std::numeric_limits<double>::infinity(), 0.0, 1.0) == 0.0,
              "F184: the raise asked in the sights is the legs' room by the standing share (F178's weight): 9.0 cm on the F179 flight's 0.90 m leg at 0.895, 13.2 on the F180 flight's; half at half the detach; 0 crouched, at the hip, for no legs, no room and an unmeasurable one");
        // the eye is not in it: F97a's target at the full detach with the eye moved 20 cm gives the same neck, and it is the animated neck plus the room
        const double n0y = 1.291, key = 0.15;
        f97::ArmedTarget a = f97::armedNeckTarget(n0y, n0y, 1.577, key, 1.0, 1.0, 0.0, n0y), b = f97::armedNeckTarget(n0y, n0y, 1.777, key, 1.0, 1.0, 0.0, n0y);
        const f97::ArmedTarget a2 = f179::withStandingRaise(a, n0y, 1.0, f179::legsRaiseAskedM(room180, 0.0, 1.0)), b2 = f179::withStandingRaise(b, n0y, 1.0, f179::legsRaiseAskedM(room180, 0.0, 1.0));
        const double oldAsked = f179::standingRaiseM(n0y, 1.577, key, 0.0, 1.0), oldAsked2 = f179::standingRaiseM(n0y, 1.777, key, 0.0, 1.0);
        std::snprintf(msg, sizeof(msg), "F184: with the eye moved 20 cm the neck target is the same (%.3f m = animated %.3f + the room %.1f cm) where the eye-based ask moved with it (%.1f cm, then %.1f); on the F180 scene the ask (13.2) is the written raise and within 0.5 cm of the eye's (13.6)",
                      a2.y, n0y, 100.0 * room180, 100.0 * oldAsked, 100.0 * oldAsked2);
        check(a2.y == b2.y && eq(a2.y, n0y + room180, 1e-12) && eq(oldAsked, 0.136, 1e-12) && eq(oldAsked2 - oldAsked, 0.20, 1e-12) && std::fabs(room180 - oldAsked) < 0.005, msg);
        // at the hip and crouched the earlier value bit for bit: F97a's own target, nothing added
        const f97::ArmedTarget hipT = f179::withStandingRaise(f97::armedNeckTarget(n0y, n0y + 0.04, 1.577, key, 0.0, 1.0, 0.0, n0y + 0.04), n0y, 1.0, f179::legsRaiseAskedM(room180, 0.0, 0.0));
        const f97::ArmedTarget hipBase = f97::armedNeckTarget(n0y, n0y + 0.04, 1.577, key, 0.0, 1.0, 0.0, n0y + 0.04);
        check(hipT.y == hipBase.y && hipT.asked == hipBase.asked, "F184: at the hip the target is F97a's own, bit for bit (the standing share is 0)");

        // a toy loop: the camera rides on the neck, 28 cm above it (F94a's +28 to +30 cm, key 15 cm). The eye-based ask lifts the neck by the difference every call: capped by the legs it saturates
        // after a few calls (the flight: at the cap in 4347 of 4395 calls), uncapped it runs away. The legs-based ask is the room in the first call and the same after.
        const double cam = 0.28, room = 0.60;
        double nOldCap = n0y, nOld = n0y, nNew = n0y;
        int oldSettledAt = 0, newChanged = 0;
        double prevOld = n0y;
        for (int step = 1; step <= 50; ++step) {
            nOld = n0y + f179::standingRaiseM(n0y, nOld + cam, key, 0.0, 1.0);
            const double nCapNow = n0y + std::fmin(room, f179::standingRaiseM(n0y, nOldCap + cam, key, 0.0, 1.0));
            if (!oldSettledAt && nCapNow == prevOld && step > 1) oldSettledAt = step - 1;
            prevOld = nCapNow;
            nOldCap = nCapNow;
            const double nNewNow = n0y + f179::legsRaiseAskedM(room, 0.0, 1.0);
            if (step > 1 && nNewNow != nNew) ++newChanged;
            nNew = nNewNow;
        }
        std::snprintf(msg, sizeof(msg), "F184: a toy loop (the camera rides on the neck, 28 cm above it): the eye-based ask runs away uncapped (the neck %.1f m after 50 calls) and takes %d calls to reach a 60 cm cap; the legs-based ask is the room (%.2f m) in the first call and never changes",
                      nOld, oldSettledAt, nNew);
        check(nOld > n0y + 5.0 && oldSettledAt >= 4 && eq(nOldCap, n0y + room, 1e-12) && eq(nNew, n0y + room, 1e-12) && newChanged == 0, msg);
    }

    // ---- 3. the lines -------------------------------------------------------------------------------------------------------------------------------------------------------------------
    {
        f179::Counters c;
        for (int cx : {f179::kAdsStanding, f179::kAdsCrouched, f179::kHipArmed, f179::kHipUnarmed}) {
            for (int i = 0; i < 100; ++i) {
                f179::Sample sm;
                sm.ctx = cx;
                sm.beforeM = 0.06; sm.predM = 0.06;
                if (f179::guardActs(cx)) sm.pushM = i < 50 ? 0.04 : 0.0;
                else sm.wouldM = i < 50 ? 0.04 : 0.0;
                c.addGuard(sm);
            }
        }
        char hip[1300], ads[1300], adsC[1300], un[1300];
        f179::formatContext(hip, sizeof(hip), 10, "x", f179::kHipArmed, c.c[f179::kHipArmed], 4.0);
        f179::formatContext(ads, sizeof(ads), 10, "x", f179::kAdsStanding, c.c[f179::kAdsStanding], 4.0);
        f179::formatContext(adsC, sizeof(adsC), 10, "x", f179::kAdsCrouched, c.c[f179::kAdsCrouched], 4.0);
        f179::formatContext(un, sizeof(un), 10, "x", f179::kHipUnarmed, c.c[f179::kHipUnarmed], 4.0);
        check(std::strstr(hip, "hip armed 100 frames, the guard at 4.0 cm: pushed 50") != nullptr && std::strstr(ads, "ADS standing 100 frames, the guard at 4.0 cm, measuring only (nothing is moved): would push 50 frames") != nullptr &&
                  std::strstr(adsC, "ADS crouched 100 frames, the guard at 4.0 cm, measuring only (nothing is moved): would push 50 frames") != nullptr && std::strstr(ads, "pushed") == nullptr &&
                  std::strstr(un, "hip unarmed 100 frames, the guard at 4.0 cm, measuring only (nothing is moved): would push 50 frames") != nullptr,
              "F184: the sights' lines say measuring only and the frames it would push (as the unarmed hip's does); the hip armed's says pushed");
    }

    // ---- 4. the source sites ---------------------------------------------------------------------------------------------------------------------------------------------------------------
    {
        const std::string cpp = slurp("src/d3d11/native_on_foot.cpp");
        const auto has = [&](const std::string& needle) { return cpp.find(needle) != std::string::npos; };
        const auto at = [&](const std::string& needle) { return cpp.find(needle); };
        check(has("pc.f179Measure = !nofoot::f179::guardActs(pc.f179Ctx);") && !has("pc.f179Measure = pc.f179Ctx == nofoot::f179::kHipUnarmed;") &&
                  has("if (!pc.f179Measure) pc.f179PushPre = nofoot::f179::guardTarget(g179, N0, pc.f179Bias, T, pc.f179DistM, &pc.f179Bounded);") &&
                  has("if (g179.ok && !pc.f179Measure) {\n        const double thWithPost[3]") && has("if (g179.ok && !pc.f179Measure) {\n            const nofoot::PinMove formed = nofoot::pinMove(N0, desiredNeck);"),
              "F184: the guard's pre-reach push, the post-reach trim and the formed neck's residual are all gated by the measuring flag, which is set for every context but the hip armed");
        check(has("legsRoomM = legsRaiseRoomM(pl, count);") && has("pc.f179StandRaise = nofoot::f179::legsRaiseAskedM(legsRoomM, pc.postureW, pc.detachW);") &&
                  has("nofoot::f179::legsRaiseAskedM(legsRoomM, pc.postureW, pc.detachW)).y;") && !has("nofoot::f179::standingRaiseM(") && has("double legsRaiseRoomM(const pfz::ArmPlan& pl, uint32_t count) noexcept {") &&
                  has("const double legsRoom = legsRaiseRoomM(pl, count);") && has("if (std::isfinite(legsRoom)) room = std::min(room, legsRoom);"),
              "F184: the standing raise is asked of the legs' room on both the legacy target and the rest path's, from the one function F97a's cap uses; the eye-based ask is not called");
        const size_t legsAt = at("legsRoomM = legsRaiseRoomM(pl, count);"), wrapAt = at("at = nofoot::f179::withStandingRaise(at, N0[1], f97W, pc.f179StandRaise);"), capAt = at("const double legsRoom = legsRaiseRoomM(pl, count);");
        check(legsAt != std::string::npos && legsAt < wrapAt && wrapAt < capAt, "F184: the room is read before the target is made, and the cap reads the same room after it");
        check(has("source: the legs' room, not the eye\", f.ssSum / n97, f.srSum / n97);") && has("char tail97[168] = \"\";"), "F184: F97a's line in the sights says the raise's source: the legs' room, not the eye");
    }
}

}  // namespace f184test
