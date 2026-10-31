// F185 checks (native on-foot VR), run185 below. Header only, run by native_frame_test.
//
// F185 (native_on_foot_f185.h, TEMPORARY): experimental.native_on_foot_ads_left_reach, 0.80 to 1.00, default 0.92. In the sights with a two-handed gun the left shoulder is kept within this share of
// the left arm's full length of the left-hand grip, so the elbow keeps a bend; 1.00 is today's, bit for bit. The clavicle first (the arm groups turned about the up axis through the clavicle joint,
// at most 15 deg, by what brings the hand target to the target distance), then the pin's reach rule takes what is left from the body's move, to the target distance plus the clavicle's chord. The
// rig runs the dial and its weights, the reach radius, the protraction on a shoulder and a grip (the smallest turn that reaches, the 15 deg limit, the cases that do nothing), the rule on the
// pin's own function (the clavicle alone covers 0.92, 0.80 asks the body), the composition with F59's turn (one save), the counters and the line, the key's sites, and the source sites.
#pragma once

#include "../../src/d3d11/native_on_foot_core.h"
#include "../../src/d3d11/native_on_foot_f185.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>

namespace f185test {

namespace nf = edvr::nofoot;
namespace f185 = edvr::nofoot::f185;

constexpr double kPi = 3.14159265358979323846;

inline std::string slurp(const char* path) {
    std::ifstream f(path, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}
inline size_t countOf(const std::string& hay, const std::string& needle) {
    size_t n = 0, at = 0;
    while ((at = hay.find(needle, at)) != std::string::npos) { ++n; at += needle.size(); }
    return n;
}
inline double dist3(const double a[3], const double b[3]) { return std::sqrt((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2])); }

template <class Check>
inline void run185(Check&& check) {
    const auto eq = [](double a, double b, double tol) { return std::fabs(a - b) <= tol; };
    char msg[700];

    // ---- 1. the dial and its weights ----------------------------------------------------------------------------------------------------------------------------------------------------
    {
        check(f185::clampDial(0.92) == 0.92 && f185::clampDial(0.5) == 0.80 && f185::clampDial(1.5) == 1.0 && f185::clampDial(std::nan("")) == f185::kDialDefault &&
                  f185::clampDial(std::numeric_limits<double>::infinity()) == f185::kDialDefault && f185::kDialMin == 0.80 && f185::kDialMax == 1.00 && f185::kDialDefault == 0.92 && f185::kMaxTurnDeg == 15.0,
              "F185: the dial is held to 0.80..1.00; a value that is not a number reads the default 0.92; the clavicle turns at most 15 deg");
        check(f185::off(1.0) && f185::off(1.2) && !f185::off(0.99) && !f185::off(0.92) && !f185::off(std::nan("")), "F185: the dial at 1.00 is off (today's reach rule, no turn); a value that is not a number reads the default, which is on");
        check(f185::share(0.92, 0.0) == 1.0 && eq(f185::share(0.92, 1.0), 0.92, 1e-15) && eq(f185::share(0.92, 0.5), 0.96, 1e-15) && f185::share(1.0, 1.0) == 1.0 && f185::share(0.8, 2.0) == 0.8 &&
                  f185::share(0.8, -1.0) == 1.0 && f185::share(0.8, std::nan("")) == 1.0,
              "F185: the share of the arm's length goes from 1 at the hip to the dial in the sights with the detach weight (half the detach is half way); held to 0..1");
        check(eq(f185::targetM(0.592, 0.92, 1.0), 0.54464, 1e-9) && f185::targetM(0.592, 0.92, 0.0) == 0.592 && eq(f185::targetM(0.592, 0.80, 1.0), 0.4736, 1e-12),
              "F185: the target distance is the arm's full length (30.4 + 28.8 = 59.2 cm) times the share: 54.5 cm at 0.92, 47.4 at 0.80");
        check(eq(f185::clavicleChordM(0.14), 2.0 * 0.14 * std::sin(7.5 * kPi / 180.0), 1e-15) && eq(f185::clavicleChordM(0.14), 0.03655, 1e-5) && f185::clavicleChordM(0.0) == 0.0 && f185::clavicleChordM(std::nan("")) == 0.0 &&
                  f185::clavicleChordM(-1.0) == 0.0,
              "F185: a 14 cm clavicle turned the full 15 deg gives the shoulder 3.7 cm (its chord); none for no clavicle");
    }

    // ---- 2. the reach rule's radius -----------------------------------------------------------------------------------------------------------------------------------------------------
    {
        const double armLen = 0.592, today = armLen - nf::kReachMarginM, clav = 0.14;
        std::snprintf(msg, sizeof(msg), "F185: the reach radius is today's (the same double) at the dial 1.00 and at the hip; at 0.92 the clavicle's chord covers the whole of it (%.4f + %.4f is past today's %.4f): today's again; at 0.80 it is the target plus the chord (%.4f m)",
                      f185::targetM(armLen, 0.92, 1.0), f185::clavicleChordM(clav), today, f185::reachRadiusM(today, armLen, clav, 0.80, 1.0));
        check(f185::reachRadiusM(today, armLen, clav, 1.0, 1.0) == today && f185::reachRadiusM(today, armLen, clav, 0.80, 0.0) == today && f185::reachRadiusM(today, armLen, clav, 0.92, 1.0) == today &&
                  eq(f185::reachRadiusM(today, armLen, clav, 0.80, 1.0), 0.4736 + f185::clavicleChordM(clav), 1e-12) && f185::reachRadiusM(today, armLen, clav, 0.80, 1.0) < today &&
                  f185::reachRadiusM(today, armLen, clav, 0.80, 0.5) > f185::reachRadiusM(today, armLen, clav, 0.80, 1.0) && f185::reachRadiusM(today, armLen, 0.0, 0.92, 1.0) < today,
              msg);
        // the pin's own rule on a left shoulder 62 cm from the grip (the flight): today's radius takes the body to within 57.2, the dial 0.80 asks the body for the rest; 0.92 leaves it to the clavicle
        double v[2][3] = {{0.0, 0.0, -0.62}, {0.0, 0.0, -0.30}}, candT[3] = {}, fullT[3] = {}, cand92[3] = {}, full92[3] = {}, cand80[3] = {}, full80[3] = {};
        const bool have[2] = {true, true};
        bool f1 = true, f2 = true, f3 = true;
        double rToday[2] = {today, 0.572}, r92[2] = {f185::reachRadiusM(today, armLen, clav, 0.92, 1.0), 0.572}, r80[2] = {f185::reachRadiusM(today, armLen, clav, 0.80, 1.0), 0.572};
        nf::reachLimitDeltaFeasibleOnly(v, rToday, have, candT, fullT, &f1);
        nf::reachLimitDeltaFeasibleOnly(v, r92, have, cand92, full92, &f2);
        nf::reachLimitDeltaFeasibleOnly(v, r80, have, cand80, full80, &f3);
        std::snprintf(msg, sizeof(msg), "F185: the pin's reach rule on a shoulder 62 cm from the grip: today's radius moves the body %.1f cm; the dial 0.92 the same (the clavicle takes the 2.7 cm to the target); 0.80 moves it %.1f cm (the rest the clavicle's 3.7 cannot cover)",
                      100.0 * std::fabs(fullT[2]), 100.0 * std::fabs(full80[2]));
        check(std::memcmp(fullT, full92, sizeof(fullT)) == 0 && std::fabs(full80[2]) > std::fabs(fullT[2]) + 0.05 && eq(std::fabs(full80[2]) - std::fabs(fullT[2]), today - r80[0], 1e-9), msg);
    }

    // ---- 3. the clavicle's protraction ---------------------------------------------------------------------------------------------------------------------------------------------------
    {
        // the left clavicle joint, the shoulder 14 cm out and a little forward of it, and a grip ahead and below: the hand target at a chosen distance from the shoulder
        const double C[3] = {-0.04, 1.42, 0.00}, S[3] = {-0.18, 1.42, 0.02};
        const auto targetAt = [&](double d, double out[3]) {
            const double u[3] = {0.15, -0.15, 1.0}, n = std::sqrt(u[0] * u[0] + u[1] * u[1] + u[2] * u[2]);
            for (int i = 0; i < 3; ++i) out[i] = S[i] + u[i] / n * d;
        };
        const double goal = 0.5446;   // 0.92 of 59.2 cm
        double T56[3], T60[3], T64[3], T50[3];
        targetAt(0.565, T56);
        targetAt(0.60, T60);
        targetAt(0.64, T64);
        targetAt(0.50, T50);
        const f185::Turn a = f185::protract(C, S, T56, goal), b = f185::protract(C, S, T60, goal), c = f185::protract(C, S, T64, goal), none = f185::protract(C, S, T50, goal);
        std::snprintf(msg, sizeof(msg), "F185: a grip 56.5 cm from the shoulder is brought to the 54.5 cm target by the smallest turn (%.2f deg, the shoulder %.1f cm, %.4f m after); not capped",
                      a.angleRad * 180.0 / kPi, 100.0 * a.moveM, a.distAfter);
        check(a.applied && !a.capped && a.angleRad > 0.0 && a.angleRad * 180.0 / kPi < f185::kMaxTurnDeg && eq(a.distAfter, goal, 1e-7) && eq(a.distBefore, 0.565, 1e-9) && a.moveM > 0.0 && a.moveM < f185::clavicleChordM(0.14), msg);
        std::snprintf(msg, sizeof(msg), "F185: a grip 60 cm and one 64 cm from the shoulder take the full 15 deg (%.2f and %.2f deg), capped, and end closer (%.4f and %.4f m) but short of the target: the reach rule has the rest",
                      b.angleRad * 180.0 / kPi, c.angleRad * 180.0 / kPi, b.distAfter, c.distAfter);
        check(b.applied && c.applied && b.capped && c.capped && eq(b.angleRad, 15.0 * kPi / 180.0, 1e-12) && eq(c.angleRad, 15.0 * kPi / 180.0, 1e-12) && b.distAfter < b.distBefore && c.distAfter < c.distBefore &&
                  b.distAfter > goal && c.distAfter > goal && eq(b.moveM, f185::clavicleChordM(std::sqrt(0.14 * 0.14 + 0.02 * 0.02)), 1e-9),
              msg);
        check(!none.applied && none.angleRad == 0.0 && none.distAfter == none.distBefore && !none.capped, "F185: a shoulder already within the target distance is not turned");

        // the turn matrix is the turn the formula says: the pivoted shoulder lands where protract put it, and nearer the grip
        float M[16] = {};
        M[0] = M[5] = M[10] = M[15] = 1.0f;
        M[12] = static_cast<float>(S[0]); M[13] = static_cast<float>(S[1]); M[14] = static_cast<float>(S[2]);
        double R[9];
        f185::turnMatrix(b.angleRad, R);
        float out[16];
        nf::ikPivot(M, R, C, out);
        const double Snew[3] = {out[12], out[13], out[14]};
        const double chord[3] = {Snew[0] - S[0], Snew[1] - S[1], Snew[2] - S[2]};
        check(eq(Snew[1], S[1], 1e-6) && eq(nf::ikLen(chord), b.moveM, 1e-6) && dist3(Snew, T60) < dist3(S, T60) - 0.01 && eq(dist3(Snew, T60), b.distAfter, 1e-5) && eq(dist3(Snew, C), dist3(S, C), 1e-6),
              "F185: the turn matrix pivots the shoulder about the up axis through the clavicle joint (its height and its distance from the joint kept) onto the point protract found, nearer the grip");

        // the cases that do nothing: the shoulder on the up axis through the clavicle, the grip straight above it, a turn that would not shorten the distance
        const double Son[3] = {C[0], 1.42, C[2]}, Tabove[3] = {C[0], 2.4, C[2]}, Tsame[3] = {S[0] - 0.01, 1.0, S[2] + 0.5};
        check(!f185::protract(C, Son, T60, goal).applied && !f185::protract(C, S, Tabove, goal).applied && !f185::protract(C, S, T60, std::nan("")).applied,
              "F185: a shoulder on the clavicle's axis, a grip straight above the clavicle and a target distance that is not a number turn nothing");
        (void)Tsame;
        // the left shoulder's heading increases with the turn, as the formula and the matrix say: a shoulder on the right of the joint turns the other way
        const double Sr[3] = {0.10, 1.42, 0.02};
        double Tr[3] = {0.10, 1.2, 0.62};   // ahead of it
        const f185::Turn r = f185::protract(C, Sr, Tr, 0.55);
        check(r.applied && dist3(Sr, Tr) > 0.55 && r.distAfter < r.distBefore, "F185: it works the same on a shoulder to the right of the joint (the sign is the turn that shortens the distance)");
    }

    // ---- 4. composed with F59's turn: one save, both turns --------------------------------------------------------------------------------------------------------------------------------------
    {
        // the arm solve saves the pose once, before the first turn, and each turn pivots the pose as it stands: the second composes with the first, and the save puts the original back
        const double C[3] = {-0.04, 1.42, 0.00};
        float M0[16] = {}, save[16], cur[16], t1[16];
        M0[0] = M0[5] = M0[10] = M0[15] = 1.0f;
        M0[12] = -0.18f; M0[13] = 1.42f; M0[14] = 0.02f;
        double Ry[9], Rx[9];
        f185::turnMatrix(12.0 * kPi / 180.0, Ry);
        const double ax[3] = {1.0, 0.0, 0.0};
        nf::ikRotAxis(ax, 8.0 * kPi / 180.0, Rx);
        bool saved = false;
        std::memcpy(cur, M0, sizeof(cur));
        if (!saved) std::memcpy(save, cur, sizeof(save));
        nf::ikPivot(cur, Ry, C, t1);
        saved = true;
        std::memcpy(cur, t1, sizeof(cur));
        if (!saved) std::memcpy(save, cur, sizeof(save));   // not taken: the save is from before the first turn
        float t2[16];
        nf::ikPivot(cur, Rx, C, t2);
        // the same two turns on the position by hand
        double p[3] = {-0.18 - C[0], 1.42 - C[1], 0.02 - C[2]}, q[3];
        for (int c = 0; c < 3; ++c) q[c] = p[0] * Ry[c] + p[1] * Ry[3 + c] + p[2] * Ry[6 + c];
        double q2[3];
        for (int c = 0; c < 3; ++c) q2[c] = q[0] * Rx[c] + q[1] * Rx[3 + c] + q[2] * Rx[6 + c];
        check(eq(t2[12], q2[0] + C[0], 1e-6) && eq(t2[13], q2[1] + C[1], 1e-6) && eq(t2[14], q2[2] + C[2], 1e-6) && std::memcmp(save, M0, sizeof(save)) == 0,
              "F185: F59's assist turns the pose the protraction left (both turns compose about the clavicle joint) and the pose saved before the first one is the original, for a side not written");
    }

    // ---- 5. the counters and the line ---------------------------------------------------------------------------------------------------------------------------------------------------
    {
        f185::Counters c;
        for (int i = 0; i < 100; ++i) {
            f185::Sample s;
            s.dial = 0.92;
            s.targetM = 0.5446;
            s.turned = i < 70;
            s.capped = i < 10;
            s.turnDeg = s.turned ? 10.0 : 0.0;
            s.moveCm = s.turned ? 3.5 : 0.0;
            s.tookCm = i < 20 ? 2.0 : 0.0;
            s.reachBefore = 1.04;
            s.reachAfter = 0.92 + 0.0004 * i;
            c.add(s);
        }
        check(c.frames == 100 && c.turned == 70 && c.capped == 10 && c.reachN == 100 && eq(c.dialSum, 92.0, 1e-9) && eq(c.turnMax, 10.0, 1e-12) && eq(c.moveMax, 3.5, 1e-12) && eq(c.tookMax, 2.0, 1e-12) &&
                  eq(c.reachAfterMin, 0.92, 1e-12) && eq(c.reachAfterMax, 0.9596, 1e-12),
              "F185: the counters keep the solves, those the clavicle turned, those where the full turn was not enough, the turn, the shoulder's move, what the rule took and the reach ratios");
        f185::Counters m;
        m.merge(c);
        m.merge(c);
        check(m.frames == 200 && m.turned == 140 && m.capped == 20 && eq(m.turnMax, 10.0, 1e-12) && eq(m.reachAfterMax, 0.9596, 1e-12), "F185: two windows add up");
        char line[1300];
        const int len = f185::formatSummary(line, sizeof(line), 10, "$wpn_m_shotgun_plasma_doublebarrel_name;", c);
        std::snprintf(msg, sizeof(msg), "F185: the window's line names the item, the solves, the dial, the target distance, the clavicle's turn and the shoulder's move, the rule's take and the reach ratios before and after (%d bytes)", len);
        check(len > 0 && len < 1166 &&
                  std::strstr(line, "native on foot ADS left arm bend (F185, TEMPORARY) 10 s: item $wpn_m_shotgun_plasma_doublebarrel_name;; two-handed ADS solves 100, the dial experimental.native_on_foot_ads_left_reach mean 0.92; "
                                    "the target shoulder to hand distance mean 54.5 cm; the clavicle turned in 70 (the turn mean 10.0 deg, largest 10.0; the shoulder moved mean 3.5 cm, largest 3.5; the full 15 deg not enough in 10); "
                                    "the reach rule took mean 0.4 cm of the body's move for it (largest 2.0); the left arm's reach ratio before the clavicle mean 1.040, after it mean 0.940 (min 0.920 max 0.960)") != nullptr,
              msg);
        f185::Counters big;
        big.frames = big.turned = big.capped = big.reachN = 4000000000u;
        big.dialSum = big.targetSum = big.turnSum = big.moveSum = big.tookSum = big.reachBeforeSum = big.reachAfterSum = 1.0e12;
        big.turnMax = big.moveMax = big.tookMax = 1.0e6;
        big.reachAfterMin = -1.0e6; big.reachAfterMax = 1.0e6;
        char worst[1400];
        const int worstLen = f185::formatSummary(worst, sizeof(worst), 4000000000u, "$wpn_m_shotgun_plasma_doublebarrel_name_with_a_long_tail;", big);
        f185::Counters none;
        char zero[1300];
        check(worstLen > 0 && worstLen < 1166 && f185::formatSummary(zero, sizeof(zero), 1, nullptr, none) > 0 && std::strstr(zero, "item none") != nullptr && std::strstr(zero, "nan") == nullptr && std::strstr(zero, "inf") == nullptr,
              "F185: the line stays within the log's 1166 bytes at its worst, and a window with nothing prints zeros and 'none'");
    }

    // ---- 6. the key and the source sites ---------------------------------------------------------------------------------------------------------------------------------------------------------
    {
        const std::string cpp = slurp("src/d3d11/native_on_foot.cpp"), ini = slurp("edvr.ini"), logpin = slurp("src/d3d11/native_on_foot_logpin.h");
        const auto has = [&](const std::string& needle) { return cpp.find(needle) != std::string::npos; };
        const auto at = [&](const std::string& needle) { return cpp.find(needle); };
        const size_t keyAt = ini.find("\nnative_on_foot_ads_left_reach = 0.92\n"), uiAt = ini.find("# ui: ADS left arm reach | range 0.8..1 | step 0.01 | live\n"), hipAt = ini.find("\nnative_on_foot_hip_elbow_swivel_max_deg = "),
                     squareAt = ini.find("\nnative_on_foot_crouch_ads_square = ");
        check(has("std::atomic<float> g_adsLeftReach{static_cast<float>(nofoot::f185::kDialDefault)};") &&
                  has("nofoot::f185::clampDial(static_cast<double>(cfg.getFloat(\"experimental.native_on_foot_ads_left_reach\", static_cast<float>(nofoot::f185::kDialDefault))))") &&
                  has("if (first || reach != g_adsLeftReachLast)") && has("native on foot ADS left arm reach dial (F185, TEMPORARY): experimental.native_on_foot_ads_left_reach = %.2f") &&
                  has("g_adsLeftReach.store(static_cast<float>(reach), std::memory_order_relaxed);") && countOf(ini, "native_on_foot_ads_left_reach =") == 1 && keyAt != std::string::npos && uiAt != std::string::npos &&
                  uiAt < keyAt && hipAt < uiAt && keyAt < squareAt && ini.find("# TEMPORARY (F185): ADS left arm reach:") != std::string::npos,
              "F185: the key is read from [experimental] (default 0.92, held to 0.80..1.00), logged at the first read and at each change, stored for the pin and the arm solve, and shipped in edvr.ini with its ui row after the hip elbow row");
        // the pin's reach rule
        check(has("const double f185Dial = static_cast<double>(g_adsLeftReach.load(std::memory_order_relaxed));\n            pc.f185Gate = physicalPoleActive && (stableSupportPolicy || rifleStableAuthority);") &&
                  has("if (side == 0 && pc.f185Gate && !nofoot::f185::off(f185Dial)) {") && has("const double rn = nofoot::f185::reachRadiusM(R[0], armLen[0], clavLen[0], f185Dial, pc.detachW);") &&
                  has("nofoot::reachLimitDeltaFeasibleOnly(v, RToday, have, candidateToday, fullToday, &feasibleToday);") && has("pc.f185TookCm = 100.0 * w * std::sqrt("),
              "F185: the pin's reach rule takes the left radius from the dial (the target distance plus the clavicle's chord) for a two-handed gun in the sights only, and counts what that took of the body's move");
        // the arm solve
        const size_t protractAt = at("const nofoot::f185::Turn turn = nofoot::f185::protract(Cp185, S, target, o.f185TargetM);"), assistAt = at("if (mode != 2 && C < count) {\n            double Cp[3], excess = 0.0;");
        check(protractAt != std::string::npos && assistAt != std::string::npos && protractAt < assistAt &&
                  has("if (side == 0 && doSolveWrite && C < count && pinWanted && pinCall.valid && physicalPoleActive && physicalPoleAttempted[0] && !authoredPistolSupport &&\n            bodyDetachForCall >= nofoot::f185::kSettledDetach) {") &&
                  has("if (!nofoot::f185::off(f185Dial)) {") && has("nofoot::f185::turnMatrix(turn.angleRad, Rc185);") && has("if (!o.saved) std::memcpy(g_ikM0Save[j], cur, 64);") &&
                  countOf(cpp, "if (!o.saved) std::memcpy(g_ikM0Save[j], cur, 64);") == 2 && has("nofoot::ikPivot(cur, Rc185, Cp185, g_ikM0[j]);") && has("nofoot::ikPivot(cur, Rc, Cp, g_ikM0[j]);"),
              "F185: the clavicle's protraction is made in the arm solve before F59's assist, for the left hand's stable-pole solve with a two-handed gun (not a pistol) written in the settled sights, and the pose is saved once for both turns");
        // the counters, the line
        check(has("if (pl.ok[0] && so[0].f185Gate) {") && has("f.f185.add(s185);") && has("s185.reachAfter = so[0].reachA;") && has("if (f98.f185.frames) {") &&
                  has("nofoot::f185::formatSummary(line185, sizeof(line185), w.seconds, w.item, f98.f185);") && has("nofoot::f185::Counters f185;") && logpin.find("d.f185.merge(s.f185);") != std::string::npos,
              "F185: each settled two-handed ADS solve of the left arm is filed (the dial at 1.00 included), F98Left carries the counters, logpin merges them and the line is printed whenever a solve was counted");
        // the hip, pistols and tools: the gates
        check(!has("f185Gate = true;\n            o.f185Gate = true") && has("o.f185Gate = true;") && has("pinWanted && pinCall.valid && physicalPoleActive && physicalPoleAttempted[0] && !authoredPistolSupport"),
              "F185: a pistol (authored support), a tool, an item the left hand does not hold, and the hip (the detach weight under 0.9 here, the share 1 in the pin) are outside the gate");
    }
}

}  // namespace f185test
