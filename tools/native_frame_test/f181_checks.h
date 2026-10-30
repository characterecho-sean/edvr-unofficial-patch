// F181 checks (native on-foot VR), run181 below. Header only, run by native_frame_test.
//
// F181a (native_on_foot_f181.h, TEMPORARY): the left elbow's swivel capped at the hip with a two-handed gun. experimental.native_on_foot_hip_elbow_swivel_max_deg (0..90, default 25, 90 off): a solve
// whose swivel from the stance's plane (F98a's plane angle) is over the cap is brought to p - hipShare x (p - cap) by turning the pole about the shoulder-to-hand axis toward the plane, on the
// side it was on; the hip share is 1 less the detach weight. The rig runs capSwivel on synthetic plane-angle functions (a V about the plane, symmetric and not), the counters and the line, and
// pins the source sites. F181b: the pilot's values as the defaults of six dials (the atomic, the fallback and the shipped line pinned per key).
#pragma once

#include "../../src/d3d11/native_on_foot_core.h"
#include "../../src/d3d11/native_on_foot_f181.h"
#include "../../src/d3d11/native_on_foot_f179.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>

namespace f181test {

namespace nf = edvr::nofoot;
namespace f181 = edvr::nofoot::f181;

constexpr double kPi = 3.14159265358979323846;
inline double rad(double deg) { return deg * kPi / 180.0; }

inline std::string slurp(const char* path) {
    std::ifstream f(path, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}
// The value of `key = value` on a line of its own in the ini text; NaN when the key is not there exactly once.
inline double iniValue(const std::string& ini, const std::string& key) {
    const std::string needle = "\n" + key + " = ";
    const size_t at = ini.find(needle);
    if (at == std::string::npos || ini.find(needle, at + 1) != std::string::npos) return std::nan("");
    return std::strtod(ini.c_str() + at + needle.size(), nullptr);
}

template <class Check>
inline void run181(Check&& check) {
    const auto eq = [](double a, double b, double tol) { return std::fabs(a - b) <= tol; };
    char msg[640];

    // ---- 1. the dial and the targets ---------------------------------------------------------------------------------------------------------------------------------------------------------------
    {
        check(f181::clampCapDeg(25.0) == 25.0 && f181::clampCapDeg(-5.0) == 0.0 && f181::clampCapDeg(120.0) == 90.0 && f181::clampCapDeg(0.0) == 0.0 && f181::clampCapDeg(90.0) == 90.0 &&
                  f181::clampCapDeg(std::nan("")) == 25.0 && f181::clampCapDeg(std::numeric_limits<double>::infinity()) == 25.0 && f181::kCapDefaultDeg == 25.0 && f181::kCapMinDeg == 0.0 && f181::kCapMaxDeg == 90.0,
              "F181a: the cap is held to 0..90 deg; a value that is not a number reads the default 25");
        check(f181::capOff(90.0) && f181::capOff(120.0) && !f181::capOff(89.9) && !f181::capOff(0.0) && !f181::capOff(25.0) && !f181::capOff(std::nan("")), "F181a: the cap at 90 is off (the game's pose is left as it is); 0 is a cap");
        check(f181::hipShare(0.0) == 1.0 && f181::hipShare(1.0) == 0.0 && eq(f181::hipShare(0.25), 0.75, 1e-15) && f181::hipShare(2.0) == 0.0 && f181::hipShare(-1.0) == 1.0 && f181::hipShare(std::nan("")) == 1.0,
              "F181a: the hip share is 1 less the detach weight (whole at the hip, none in the sights, 0..1), a detach weight that is not a number reads the hip");
        check(f181::targetDeg(53.0, 25.0, 1.0) == 25.0 && eq(f181::targetDeg(53.0, 25.0, 0.5), 39.0, 1e-12) && f181::targetDeg(53.0, 25.0, 0.0) == 53.0 && f181::targetDeg(20.0, 25.0, 1.0) == 20.0 &&
                  f181::targetDeg(25.0, 25.0, 1.0) == 25.0 && f181::targetDeg(53.0, 90.0, 1.0) == 53.0 && f181::targetDeg(53.0, 0.0, 1.0) == 0.0 && std::isnan(f181::targetDeg(std::nan(""), 25.0, 1.0)),
              "F181a: a plane angle over the cap is brought to p less the hip share of what is over it (53 at the cap 25: 25 at the hip, 39 at half the share, 53 in the sights); one at or under the cap, a cap of 90 and a share of 0 leave it");
    }

    // ---- 2. capSwivel on a plane angle that is a V about the plane ---------------------------------------------------------------------------------------------------------------------------------
    {
        // the plane angle of a swivel is its distance (deg) from the swivel that lies in the stance's plane, thetaZ; the elbow starts 53 deg off it (the rifle's 52 to 55)
        const double thetaZ = rad(30.0);
        const auto vee = [&](double th) { return std::fabs(nf::ikWrapPi(th - thetaZ)) * 180.0 / kPi; };
        const double start = thetaZ + rad(53.0);
        const f181::Cap c = f181::capSwivel(start, vee(start), 25.0, vee);
        std::snprintf(msg, sizeof(msg), "F181a: 53 deg off the plane with the cap at 25 the pole is turned %.2f deg to %.2f deg off the plane (the swivel %.4f rad from the plane's)", c.turnedDeg, c.planeAfter,
                      nf::ikWrapPi(c.theta - thetaZ));
        check(c.applied && eq(vee(c.theta), 25.0, 1e-4) && eq(c.planeAfter, 25.0, 1e-4) && eq(c.turnedDeg, 28.0, 1e-3) && nf::ikWrapPi(c.theta - thetaZ) > 0.0, msg);
        // the other side of the plane: the elbow keeps its side
        const double startMinus = thetaZ - rad(53.0);
        const f181::Cap m = f181::capSwivel(startMinus, vee(startMinus), 25.0, vee);
        check(m.applied && eq(vee(m.theta), 25.0, 1e-4) && nf::ikWrapPi(m.theta - thetaZ) < 0.0 && eq(m.turnedDeg, 28.0, 1e-3), "F181a: on the other side of the plane it is brought to the cap on that side: it does not cross the plane");
        // not over the cap: untouched, bit for bit
        const double under = thetaZ + rad(18.5);
        const f181::Cap u = f181::capSwivel(under, vee(under), f181::targetDeg(vee(under), 25.0, 1.0), vee);
        check(!u.applied && u.theta == under && u.turnedDeg == 0.0, "F181a: the shotgun's 18.5 deg under the cap of 25 is untouched, the swivel bit for bit");
        const f181::Cap atCap = f181::capSwivel(thetaZ + rad(25.0), 25.0, f181::targetDeg(25.0, 25.0, 1.0), vee);
        check(!atCap.applied, "F181a: a plane angle at the cap is untouched");
        // the hip share: half the share goes half way (39 deg), none in the sights
        const f181::Cap half = f181::capSwivel(start, vee(start), f181::targetDeg(vee(start), 25.0, 0.5), vee);
        const f181::Cap sights = f181::capSwivel(start, vee(start), f181::targetDeg(vee(start), 25.0, f181::hipShare(1.0)), vee);
        check(half.applied && eq(vee(half.theta), 39.0, 1e-4) && !sights.applied && sights.theta == start,
              "F181a: through the press the cap lets go with the detach weight: half the hip share brings 53 deg to 39, the sights (detach 1) do not touch it");
        bool continuous = true;
        double last = 1e9;
        for (int i = 0; i <= 20; ++i) {
            const double detach = i / 20.0;
            const f181::Cap k = f181::capSwivel(start, vee(start), f181::targetDeg(vee(start), 25.0, f181::hipShare(detach)), vee);
            const double after = k.applied ? vee(k.theta) : vee(start);
            continuous = continuous && after >= (i == 0 ? 0.0 : last) - 1e-6 && after <= 53.0 + 1e-9;
            last = after;
        }
        check(continuous && eq(last, 53.0, 1e-9), "F181a: the plane angle after the cap rises without a step from the cap (25 at the hip) to the pole's own (53 in the sights) as the detach weight rises");
        // a cap of 0: into the plane; the cap off: not applied
        const f181::Cap zero = f181::capSwivel(start, vee(start), f181::targetDeg(vee(start), 0.0, 1.0), vee);
        check(zero.applied && vee(zero.theta) < 1e-3, "F181a: a cap of 0 puts the elbow in the stance's plane");
        check(!f181::capSwivel(start, vee(start), f181::targetDeg(vee(start), 90.0, 1.0), vee).applied, "F181a: the cap at 90 is off: not applied");
        // across the wrap of the swivel's angle
        const double zWrap = rad(170.0);
        const auto veeWrap = [&](double th) { return std::fabs(nf::ikWrapPi(th - zWrap)) * 180.0 / kPi; };
        const double wrapStart = zWrap + rad(53.0);   // past +-180
        const f181::Cap w = f181::capSwivel(nf::ikWrapPi(wrapStart), veeWrap(wrapStart), 25.0, veeWrap);
        check(w.applied && eq(veeWrap(w.theta), 25.0, 1e-4) && nf::ikWrapPi(w.theta - zWrap) > 0.0 && std::fabs(w.theta) <= kPi + 1e-12, "F181a: across the wrap of the angle the pole is brought to the cap on the same side, the result wrapped");
        // not a V: the plane angle falls faster on one side, rises again beyond the plane; the first crossing on the side it was on
        const auto skew = [&](double th) {
            const double d = nf::ikWrapPi(th - thetaZ) * 180.0 / kPi;
            return d >= 0.0 ? 1.5 * d : -0.8 * d;
        };
        const double skewStart = thetaZ + rad(60.0);   // plane angle 90
        const f181::Cap sk = f181::capSwivel(skewStart, skew(skewStart), 25.0, skew);
        check(sk.applied && eq(skew(sk.theta), 25.0, 1e-4) && nf::ikWrapPi(sk.theta - thetaZ) > 0.0, "F181a: a plane angle that falls faster on one side than the other is brought to the cap on the side it was on");
        // never falls to the target (the plane angle is 80 wherever it is looked at), unmeasurable (180), not a number
        const auto flat = [](double) { return 80.0; };
        check(!f181::capSwivel(0.3, 80.0, 25.0, flat).applied && f181::capSwivel(0.3, 80.0, 25.0, flat).theta == 0.3, "F181a: a plane angle that never falls to the target leaves the swivel as it was");
        const auto unreadable = [](double) { return 180.0; };
        check(!f181::capSwivel(0.3, 53.0, 25.0, unreadable).applied && !f181::capSwivel(std::nan(""), 53.0, 25.0, vee).applied && !f181::capSwivel(0.3, std::nan(""), 25.0, vee).applied &&
                  !f181::capSwivel(0.3, 53.0, std::nan(""), vee).applied,
              "F181a: a plane angle that cannot be measured, or a swivel, plane angle or target that is not a number, leaves the swivel as it was");
    }

    // ---- 3. the counters and the line ---------------------------------------------------------------------------------------------------------------------------------------------------------------
    {
        f181::Counters c;
        for (int i = 0; i < 100; ++i) c.add(i < 80, i < 80 ? 28.0 : 0.0, i < 80 ? 25.0 : (i < 95 ? 20.0 : 29.0), 25.0);
        check(c.hip == 100 && c.capped == 80 && eq(c.cutSum, 80.0 * 28.0, 1e-9) && c.cutMax == 28.0 && c.over == 5,
              "F181a: the counters keep the two-handed hip solves, those capped, the cut's sum and largest and the solves still 2 deg or more over the cap after it (5 at 29 deg against 25 + 2)");
        f181::Counters off;
        for (int i = 0; i < 10; ++i) off.add(false, 0.0, 60.0, 90.0);
        check(off.hip == 10 && off.over == 0, "F181a: with the cap off no solve is over it");
        f181::Counters m;
        m.merge(c);
        m.merge(c);
        check(m.hip == 200 && m.capped == 160 && m.over == 10 && m.cutMax == 28.0 && eq(m.cutSum, 2.0 * c.cutSum, 1e-9), "F181a: two windows add up");
        char line[1400] = "native on foot left elbow (F98a) 1 s: item x; 100 solves";
        const int len = f181::appendSummary(line, sizeof(line), c, 25.0);
        check(len > 0 && std::strstr(line, "100 solves; capped 80 of 100 solves (the cut mean 28.0 deg, largest 28.0; the cap 25 deg; still 2 deg or more over it after: 5)") != nullptr,
              "F181a: the F98a line's ending names the solves capped of the two-handed hip solves, the cut's mean and largest, the cap and the solves still over it after it");
        char none[400] = "native on foot left elbow (F98a) 1 s: item x";
        f181::Counters empty;
        f181::appendSummary(none, sizeof(none), empty, 25.0);
        check(std::strstr(none, "; capped 0 of 0 solves (the cut mean 0.0 deg, largest 0.0; the cap 25 deg; still 2 deg or more over it after: 0)") != nullptr && std::strstr(none, "nan") == nullptr,
              "F181a: a window with no such solve says 0 of 0, never a number that is not one");
        char tight[40] = "native on foot left elbow (F98a)";
        check(f181::appendSummary(tight, sizeof(tight), c, 25.0) >= 0 && std::strlen(tight) < sizeof(tight), "F181a: a line with no room is not written past");
        f181::Counters big;
        big.hip = big.capped = big.over = 4000000000u;
        big.cutSum = 1.0e13;
        big.cutMax = 179.9;
        char worst[400] = "";
        const int wl = f181::appendSummary(worst, sizeof(worst), big, 90.0);
        std::snprintf(msg, sizeof(msg), "F181a: the ending is %d bytes at its worst, which F98a's line (about 1000 bytes of the log's 1166 at its worst) has room for", wl);
        check(wl > 0 && wl < 150, msg);
    }

    // ---- 4. F181b: the pilot's values as the defaults ------------------------------------------------------------------------------------------------------------------------------------------------
    {
        const std::string cpp = slurp("src/d3d11/native_on_foot.cpp"), ini = slurp("edvr.ini");
        check(!cpp.empty() && !ini.empty(), "F181: the default pins read edvr.ini and src/d3d11/native_on_foot.cpp (run the rig from the repository root)");
        const auto has = [&](const std::string& needle) { return cpp.find(needle) != std::string::npos; };
        struct Row { const char* key; const char* atomic; const char* fallbackNeedle; double value; };
        // the pilot's live Frontier edvr.ini after the F180 flight: each default is the atomic's initial value, the cfg fallback and the shipped line
        const Row rows[] = {
            {"native_on_foot_crouch_ads_body_forward_cm", "std::atomic<float> g_crouchAdsBodyCm{1.0f};", "cfg.getFloat(\"experimental.native_on_foot_crouch_ads_body_forward_cm\", 1.0f)", 1.0},
            {"native_on_foot_crouch_ads_body_left_cm", "std::atomic<float> g_crouchAdsBodyLeftCm{23.0f};", "cfg.getFloat(\"experimental.native_on_foot_crouch_ads_body_left_cm\", 23.0f)", 23.0},
            {"native_on_foot_stand_ads_body_left_cm", "std::atomic<float> g_standAdsBodyLeftCm{23.0f};", "cfg.getFloat(\"experimental.native_on_foot_stand_ads_body_left_cm\", 23.0f)", 23.0},
            {"native_on_foot_stand_ads_body_forward_cm", "std::atomic<float> g_standAdsBodyCm{4.0f};", "cfg.getFloat(\"experimental.native_on_foot_stand_ads_body_forward_cm\", 4.0f)", 4.0},
            {"native_on_foot_stand_ads_spine_straighten", "std::atomic<float> g_standAdsSpine{0.9f};", "cfg.getFloat(\"experimental.native_on_foot_stand_ads_spine_straighten\", 0.9f)", 0.9},
            {"native_on_foot_neck_guard_cm", "std::atomic<float> g_neckGuardCm{static_cast<float>(nofoot::f179::kNeckGuardDefaultCm)};",
             "cfg.getFloat(\"experimental.native_on_foot_neck_guard_cm\", static_cast<float>(nofoot::f179::kNeckGuardDefaultCm))", 2.0},
            {"native_on_foot_crouch_ads_square", "std::atomic<float> g_crouchAdsSquare{0.0f};", "cfg.getFloat(\"experimental.native_on_foot_crouch_ads_square\", 0.0f)", 0.0},
            {"native_on_foot_pistol_sprint_swing_phase", "std::atomic<float> g_pistolSprintPhase{0.5f};", "cfg.getFloat(\"experimental.native_on_foot_pistol_sprint_swing_phase\", 0.5f)", 0.5},
        };
        bool all = true;
        std::string wrong;
        for (const Row& r : rows) {
            const bool ok = has(r.atomic) && has(r.fallbackNeedle) && iniValue(ini, r.key) == r.value;
            all = all && ok;
            if (!ok) wrong += std::string(wrong.empty() ? "" : ", ") + r.key;
        }
        std::snprintf(msg, sizeof(msg), "F181b: the pilot's values are the defaults of the eight experimental dials in the atomic, the cfg fallback and the shipped line (crouched forward 1, crouched left 23, standing left 23, standing forward 4, spine "
                                        "straighten 0.9, neck guard 2; the torso square 0 and the swing 0.5 unchanged); not so: %s", wrong.empty() ? "none" : wrong.c_str());
        check(all, msg);
        check(nf::f179::kNeckGuardDefaultCm == 2.0 && nf::f179::clampGuardCm(std::nan("")) == 2.0 && !has("std::atomic<float> g_neckGuardCm{static_cast<float>(nofoot::f179::kNeckGuardAheadCm)}") &&
                  !has("static_cast<float>(nofoot::f179::kNeckGuardAheadCm)"),
              "F181b: the neck guard's default is one constant (2 cm) the atomic, the fallback and the clamp's not-a-number read share; F179's own 10 cm remains only as the pure functions' default argument, which the pin never uses");
        // a live edvr.ini that holds a value keeps it: the config read is the key's value, and the default is only the fallback of a missing key (the install keeps an existing ini)
        check(has("const double cm = nofoot::f168::clampDialCm(static_cast<double>(cfg.getFloat(\"experimental.native_on_foot_crouch_ads_body_forward_cm\", 1.0f)));"),
              "F181b: the dial is read from the config with the default only as the fallback of a missing key: a live ini that holds a value keeps it");
    }

    // ---- 5. the source sites -------------------------------------------------------------------------------------------------------------------------------------------------------------------------
    {
        const std::string cpp = slurp("src/d3d11/native_on_foot.cpp"), ini = slurp("edvr.ini"), logpin = slurp("src/d3d11/native_on_foot_logpin.h");
        check(!cpp.empty() && !ini.empty() && !logpin.empty(), "F181: the source pins read the sources and edvr.ini (run the rig from the repository root)");
        const auto has = [&](const std::string& needle) { return cpp.find(needle) != std::string::npos; };
        const auto at = [&](const std::string& needle) { return cpp.find(needle); };
        check(has("std::atomic<float> g_hipElbowSwivelMaxDeg{static_cast<float>(nofoot::f181::kCapDefaultDeg)};") &&
                  has("nofoot::f181::clampCapDeg(static_cast<double>(cfg.getFloat(\"experimental.native_on_foot_hip_elbow_swivel_max_deg\", static_cast<float>(nofoot::f181::kCapDefaultDeg))))") &&
                  has("if (first || deg != g_hipElbowSwivelMaxDegLast)") && has("native on foot hip elbow swivel cap dial (F181a, TEMPORARY): experimental.native_on_foot_hip_elbow_swivel_max_deg = %.1f deg") &&
                  has("g_hipElbowSwivelMaxDeg.store(static_cast<float>(deg), std::memory_order_relaxed);") && nf::f181::kCapDefaultDeg == 25.0,
              "F181a: the key is read from [experimental] with the default 25, held to 0..90, logged at the first read and at each change, and stored for the solve");
        // the gate: the left hand's stable pole with a two-handed gun, set where F165's and F180a's blend is
        const size_t pistolBranch = at("if (side == 0 && authoredPistolSupport) {"), stableBranch = at("const nofoot::IkStableSwivel stable = nofoot::ikStableSwivel(tb, seed, outSign, elbowOutM);"),
                     gate = at("f181Eligible = true;   // F181a: the same gate: every two-handed gun, the left hand's stable pole"), w180 = at("const double w165 = nofoot::f180::elbowBlendWeight(pinCall.postureW, pinCall.detachW);");
        check(pistolBranch != std::string::npos && pistolBranch < stableBranch && stableBranch < gate && gate < w180 && has("bool f181Eligible = false;"),
              "F181a: the cap's gate is set in the left hand's stable-pole branch that is not the pistol's, in F165's and F180a's own `side == 0 && pinWanted && pinCall.valid` test: two-handed guns only; pistols, tools and unarmed are not in it");
        // the cap: after the hinge measure, before the swivel choice that follows, from the hip share, through capSwivel on the same measure
        const size_t hsOk = at("nofoot::HingeSwings hs = nofoot::ikHingeSwings(S, F0, H0, E, tb.hand,"), capAt = at("if (f181Eligible) {\n            const double capDeg"), next = at("// F76 armed full-grip solving follows a transported physical pole.");
        check(hsOk != std::string::npos && hsOk < capAt && capAt < next &&
                  has("const double target = nofoot::f181::targetDeg(planeBefore, capDeg, nofoot::f181::hipShare(pinCall.detachW));") && has("if (target < planeBefore) {") &&
                  has("const nofoot::f181::Cap cap = nofoot::f181::capSwivel(theta, planeBefore, target, planeAt);") &&
                  has("const nofoot::HingeSwings c = nofoot::ikHingeSwings(S, F0, H0, Ec, tb.hand, useRow ? rowN0 : nullptr, hl.lastValid && now - hl.lastMs < 200 ? hl.last : nullptr);"),
              "F181a: the cap sits after the hinge is measured and before the swivel choice, takes its target from the hip share, and measures the plane angle of each swivel it tries with the same ikHingeSwings call F98a's figure comes from");
        const std::string capBlock = capAt != std::string::npos && next != std::string::npos ? cpp.substr(capAt, next - capAt) : std::string();
        check(!capBlock.empty() && capBlock.find("physicalPoleCommit") == std::string::npos && capBlock.find("s_physicalPole") == std::string::npos && capBlock.find("seed.") == std::string::npos &&
                  capBlock.find("ikSwivelClamp") != std::string::npos,
              "F181a: the cap does not touch the pole's seed or what is committed from it (it is made again on every solve from the pole), and holds the ceiling as blendPole does");
        check(has("o.f181Hip = nofoot::f159::atHip(pinCall.detachW);") && has("if (so[0].f181Hip) f.f181.add(so[0].f181Capped, so[0].f181CutDeg, so[0].hPlane,") && has("nofoot::f181::Counters f181;") &&
                  has("nofoot::f181::appendSummary(line98, sizeof(line98), f98.f181,") && has("Log::get().note(\"%s\", line98);") && logpin.find("d.f181.merge(s.f181);") != std::string::npos,
              "F181a: the two-handed hip solves are counted after the cap (the plane angle as finally solved), F98Left carries the counters, logpin merges them and the F98a line ends with them");
        // the ini key: beside the standing ADS dials' neighbours, after the neck guard, with its ui row
        const size_t guardKey = ini.find("\nnative_on_foot_neck_guard_cm = "), capKey = ini.find("\nnative_on_foot_hip_elbow_swivel_max_deg = 25.0\n"), squareKey = ini.find("\nnative_on_foot_crouch_ads_square = ");
        check(guardKey != std::string::npos && capKey != std::string::npos && squareKey != std::string::npos && guardKey < capKey && capKey < squareKey && ini.find("# TEMPORARY (F181): Hip elbow swivel max") != std::string::npos &&
                  ini.find("# ui: Hip elbow swivel max | range 0..90 | step 1 | unit deg | live\n") != std::string::npos,
              "F181a: edvr.ini has the key at 25.0, TEMPORARY, with its ui: row (0..90 deg, step 1, live), after the neck guard distance and before the torso square");
    }
}

}  // namespace f181test
