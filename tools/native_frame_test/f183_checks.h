// F183 checks (native on-foot VR), run183 below. Header only, run by native_frame_test.
//
// F183 (native_on_foot_f183.h, native_on_foot_core.h pinNeckTargetAnim, native_on_foot_f179.h): one stable horizontal heading for the pin. The eye's heading while the eye's forward has a
// horizontal length of 0.5 or more (bit for bit what it was), blended toward the body's forward down to 0.17, the body's forward outright where the eye's heading points behind it; used for the
// neck target's rest offset, every reader of pc.hEye (F97a, F179b, the samples) and F168/F171/F178's axes. The neck guard bounded: a push at most the distance plus 10 cm, the filtered error within
// +-5 cm and reset when the stable heading turns over 20 deg between calls, both counted on the line. The rig runs the pin's own target function on random level-ish eyes (bit for bit), an eye 70
// deg up (blended), the aim clamp's 85.2 deg, an eye past vertical, a sweep through the band (no jump over 2 deg for a 1 deg step), F168's move where the old axis was gone, the guard's bound and
// reset, the counters and the lines, and the source sites.
#pragma once

#include "../../src/d3d11/native_on_foot_core.h"
#include "../../src/d3d11/native_on_foot_f168.h"
#include "../../src/d3d11/native_on_foot_f179.h"
#include "../../src/d3d11/native_on_foot_f183.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

namespace f183test {

namespace nf = edvr::nofoot;
namespace f179 = edvr::nofoot::f179;
namespace f183 = edvr::nofoot::f183;

constexpr double kPi = 3.14159265358979323846;
inline double rad(double deg) { return deg * kPi / 180.0; }

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

// An eye looking along yaw psi (about the up axis, positive toward +x) and pitch theta (up positive; past 90 the view is over the top): forward and up in the model's axes (x right, y up, z forward).
inline void eyeAxes(double psiDeg, double thetaDeg, double fwd[3], double up[3]) {
    const double p = rad(psiDeg), t = rad(thetaDeg);
    fwd[0] = std::sin(p) * std::cos(t); fwd[1] = std::sin(t); fwd[2] = std::cos(p) * std::cos(t);
    up[0] = -std::sin(p) * std::sin(t); up[1] = std::cos(t); up[2] = -std::cos(p) * std::sin(t);
}
struct Pin {
    nf::PinAnimTarget pa;
    double T[3];
    double eye[3] = {0.0, 1.62, 0.3};
};
// The pin's own neck target for an eye, no calibration: what the hook calls (pinNeckTargetAnim with the eye known).
inline void pinOf(Pin& p, const double fwd[3], const double up[3], bool eyeKnown = true) {
    const double p44[3] = {0.0, 1.5, 0.0}, n0[3] = {0.0, 1.35, 0.0};
    const double rows[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    const nf::PinEyeCal cal{};
    nf::pinNeckTargetAnim(p.eye, fwd, p44, rows, n0, cal, p.T, &p.pa, 0.0, false, up, 1.0, eyeKnown);
}
inline uint64_t next(uint64_t& s) {
    s = s * 6364136223846793005ull + 1442695040888963407ull;
    return s >> 11;
}
inline double unitRand(uint64_t& s) { return static_cast<double>(next(s) & 0xFFFFFFFFull) / 4294967296.0; }

template <class Check>
inline void run183(Check&& check) {
    const auto eq = [](double a, double b, double tol) { return std::fabs(a - b) <= tol; };
    char msg[700];

    // ---- 1. level-ish eyes: bit for bit what the pin had ---------------------------------------------------------------------------------------------------------------------------------------
    {
        uint64_t s = 12345;
        int n = 0, same = 0, notChanged = 0, targetSame = 0;
        for (int i = 0; i < 6000; ++i) {
            const double psi = -180.0 + 360.0 * unitRand(s), theta = -59.0 + 118.0 * unitRand(s);
            double fwd[3], up[3];
            eyeAxes(psi, theta, fwd, up);
            Pin p;
            pinOf(p, fwd, up);
            double old[3];   // the heading as the pin took it before F183
            if (!nf::pinHeadHeading(fwd, up, old)) nf::pinHeading(fwd, old);
            ++n;
            if (std::memcmp(old, p.pa.hEye, sizeof(old)) == 0) ++same;
            if (!p.pa.stable.changed && p.pa.stable.valid && !p.pa.stable.blended && !p.pa.stable.replaced) ++notChanged;
            double rest[3] = {0.09 * old[0], 0.155, 0.09 * old[2]}, t[3];
            for (int k = 0; k < 3; ++k) t[k] = p.eye[k] - rest[k];
            if (std::memcmp(t, p.T, sizeof(t)) == 0) ++targetSame;
        }
        std::snprintf(msg, sizeof(msg), "F183: on %d random eyes within 59 deg of level (every yaw, behind the body too) the pin's heading and neck target are bit for bit what they were (%d heading, %d unchanged flag, %d target)", n, same, notChanged, targetSame);
        check(same == n && notChanged == n && targetSame == n, msg);
        // no eye rows (the body's own forward stands for the eye's): nothing changed, nothing counted
        const double fwd0[3] = {0.0, 0.0, 1.0}, up0[3] = {0.0, 1.0, 0.0};
        Pin p;
        pinOf(p, fwd0, up0, false);
        check(!p.pa.stable.valid && !p.pa.stable.changed && p.pa.hEye[0] == 0.0 && p.pa.hEye[2] == 1.0, "F183: a call with no eye rows keeps the body's forward as the pin always did and counts nothing");
    }

    // ---- 2. an eye near vertical: blended; at the aim clamp and past vertical: the body's forward ----------------------------------------------------------------------------------------
    {
        double fwd[3], up[3];
        eyeAxes(30.0, 70.0, fwd, up);
        Pin p70;
        pinOf(p70, fwd, up);
        const double dot = p70.pa.hEye[2], angle = std::acos(std::fmin(1.0, p70.pa.hEye[2] * std::cos(rad(30.0)) + p70.pa.hEye[0] * std::sin(rad(30.0)))) * 180.0 / kPi;
        std::snprintf(msg, sizeof(msg), "F183: an eye 70 deg up and 30 deg off the body is blended toward the body's forward (w %.3f of the 0.5 to 0.17 band, the heading %.1f deg from the eye's, %.1f deg from the body's)",
                      p70.pa.stable.w, p70.pa.stable.angleDeg, std::acos(dot) * 180.0 / kPi);
        check(p70.pa.stable.valid && p70.pa.stable.changed && p70.pa.stable.blended && !p70.pa.stable.replaced && eq(p70.pa.stable.hl, std::cos(rad(70.0)), 1e-9) &&
                  eq(p70.pa.stable.w, (0.5 - std::cos(rad(70.0))) / 0.33, 1e-9) && angle > 5.0 && angle < 25.0 && eq(p70.pa.stable.angleDeg, angle, 1e-6) &&
                  eq(p70.pa.hEye[0] * p70.pa.hEye[0] + p70.pa.hEye[2] * p70.pa.hEye[2], 1.0, 1e-12) && p70.pa.hEye[1] == 0.0,
              msg);
        // the aim clamp (+85.2 deg) and straight up: the body's forward exactly, whatever the yaw
        bool clampOk = true;
        for (double psi : {-120.0, -30.0, 0.0, 25.0, 80.0, 170.0}) {
            for (double th : {82.0, 85.2, 90.0}) {
                eyeAxes(psi, th, fwd, up);
                Pin p;
                pinOf(p, fwd, up);
                clampOk = clampOk && p.pa.stable.changed && p.pa.stable.w == 1.0 && p.pa.hEye[0] == 0.0 && p.pa.hEye[2] == 1.0;
            }
        }
        check(clampOk, "F183: at 80 deg and more (the horizontal length under 0.17 from 80.2: 82, the game's +85.2 clamp, straight up) the heading is the body's forward exactly, at every yaw");
        // past vertical: the eye's forward reversed (pitch 115 deg, 30 deg off): the heading the pin took was behind the body; the body's forward outright, counted replaced
        eyeAxes(30.0, 115.0, fwd, up);
        double raw[3];
        if (!nf::pinHeadHeading(fwd, up, raw)) nf::pinHeading(fwd, raw);
        Pin pp;
        pinOf(pp, fwd, up);
        std::snprintf(msg, sizeof(msg), "F183: an eye past vertical (115 deg up: the horizontal part of its forward reversed, the pin's heading z %.2f) gets the body's forward and counts replaced", raw[2]);
        check(raw[2] < 0.0 && pp.pa.stable.replaced && pp.pa.stable.changed && pp.pa.hEye[0] == 0.0 && pp.pa.hEye[2] == 1.0 && pp.pa.stable.w == 1.0 && pp.pa.stable.angleDeg > 120.0, msg);
        // the pitch is symmetric: looking steeply down is near vertical too
        eyeAxes(30.0, -75.0, fwd, up);
        Pin pd;
        pinOf(pd, fwd, up);
        check(pd.pa.stable.blended && pd.pa.stable.w > 0.5 && pd.pa.stable.w < 1.0, "F183: the blend is about the eye's forward nearing vertical, up or down (75 deg down is blended)");
        // a level eye looking behind the body keeps its heading (the headset turned that way is not the view past vertical)
        eyeAxes(170.0, 5.0, fwd, up);
        Pin pb;
        pinOf(pb, fwd, up);
        check(!pb.pa.stable.changed && pb.pa.hEye[2] < 0.0, "F183: a level eye looking behind the body keeps its own heading (the horizontal length 0.5 or more is not touched)");
    }

    // ---- 3. continuity across the blend band and through vertical -----------------------------------------------------------------------------------------------------------------------------
    {
        bool smooth = true;
        double worst = 0.0;
        for (double yaw : {10.0, 30.0}) {
            double prev[2] = {0.0, 0.0};
            bool have = false;
            for (int th = 40; th <= 105; ++th) {
                double fwd[3], up[3];
                eyeAxes(yaw, static_cast<double>(th), fwd, up);
                Pin p;
                pinOf(p, fwd, up);
                const double h[2] = {p.pa.hEye[0], p.pa.hEye[2]};
                if (have) {
                    const double turn = f183::headingTurnDeg(prev, h);
                    worst = std::fmax(worst, turn);
                    smooth = smooth && turn <= 2.0;
                }
                prev[0] = h[0]; prev[1] = h[1];
                have = true;
            }
        }
        std::snprintf(msg, sizeof(msg), "F183: through the band and over the top (40 to 105 deg, an eye 10 and 30 deg off the body) a 1 deg step of pitch never turns the heading more than 2 deg (largest %.2f deg)", worst);
        check(smooth, msg);
        // the weight is linear in the horizontal length: 0 at 0.5, 1 at 0.17
        const double f1[3] = {0.5, 0.866025403784, 0.0}, f2[3] = {0.17, 0.985476, 0.0}, f3[3] = {0.335, 0.94224, 0.0};   // forwards whose horizontal length is 0.5, 0.17 and 0.335
        const double eyeHeading[3] = {1.0, 0.0, 0.0};   // 90 deg off the body
        const f183::Info a = f183::stableHeading(f1, eyeHeading), b = f183::stableHeading(f2, eyeHeading), c = f183::stableHeading(f3, eyeHeading);
        check(eq(a.hl, 0.5, 1e-9) && !a.changed && eq(b.hl, 0.17, 1e-5) && b.w >= 0.9999 && eq(c.w, 0.5, 5e-3) && c.blended,
              "F183: the blend weight is 0 from a horizontal length of 0.5 up, 1 at 0.17 and below, linear between (0.335 is half)");
    }

    // ---- 4. F168/F171/F178's axes: the stable heading keeps the move where the eye's own forward had none ---------------------------------------------------------------------------------
    {
        double fwd[3], up[3];
        eyeAxes(20.0, 85.2, fwd, up);
        double out[3];
        const bool oldMove = nf::f168::shiftVector(10.0, 1.0, 1.0, fwd, out);   // the eye's own forward: horizontal length 0.084, under the 0.1 floor
        Pin p;
        pinOf(p, fwd, up);
        const double fwdPin[3] = {p.pa.hEye[0], fwd[1], p.pa.hEye[2]};
        const bool newMove = nf::f168::shiftVector(10.0, 1.0, 1.0, fwdPin, out);
        std::snprintf(msg, sizeof(msg), "F183: at the aim clamp the eye's own forward had no horizontal part to move along (F168's move dropped: %s); on the stable heading the move is made along the body's forward (%.3f m at z)",
                      oldMove ? "no" : "yes", out[2]);
        check(!oldMove && newMove && eq(out[2], 0.10, 1e-12) && out[0] == 0.0, msg);
        // past vertical the eye's forward pointed the move the wrong way; the stable heading does not
        eyeAxes(0.0, 115.0, fwd, up);
        double raw[3];
        nf::f168::shiftVector(10.0, 1.0, 1.0, fwd, raw);
        Pin pp;
        pinOf(pp, fwd, up);
        const double fwdPast[3] = {pp.pa.hEye[0], fwd[1], pp.pa.hEye[2]};
        double fixed[3];
        nf::f168::shiftVector(10.0, 1.0, 1.0, fwdPast, fixed);
        check(raw[2] < 0.0 && fixed[2] > 0.0, "F183: past vertical the eye's own forward reversed F168's move along z; on the stable heading it keeps the body's forward");
    }

    // ---- 5. the neck guard bounded -------------------------------------------------------------------------------------------------------------------------------------------------------
    {
        const double eye[3] = {0.0, 1.6, 0.0}, chestFar[3] = {0.0, 1.4, 0.5}, chestNear[3] = {0.0, 1.4, 0.0};   // the chest 0.5 m ahead of the eye (the eye 50 cm behind it), and level with it
        const f179::Geometry gFar = f179::makeGeometry(true, eye, 0.0, 1.0, true, chestFar);
        const f179::Geometry gNear = f179::makeGeometry(true, eye, 0.0, 1.0, true, chestNear);
        const double n0[3] = {0.0, 1.35, 0.0};
        double tFar[3] = {0.0, 1.35, 0.0}, tNear[3] = {0.0, 1.35, 0.0};
        bool bFar = false, bNear = false;
        const double pFar = f179::guardTarget(gFar, n0, 0.0, tFar, 0.04, &bFar), pNear = f179::guardTarget(gNear, n0, 0.0, tNear, 0.04, &bNear);
        std::snprintf(msg, sizeof(msg), "F183: a push is at most the distance plus 10 cm in a frame (the eye 50 cm behind the chest asks 54 cm: %.1f cm, bounded); a deficit under it is whole (%.1f cm) and not counted", 100.0 * pFar, 100.0 * pNear);
        check(gFar.ok && gNear.ok && bFar && eq(pFar, 0.14, 1e-12) && eq(tFar[2], -0.14, 1e-9) && !bNear && eq(pNear, 0.04, 1e-12) && eq(tNear[2], -0.04, 1e-12), msg);
        bool b1 = false, b2 = false, b3 = false;
        check(eq(f179::deficitBoundedM(-0.09, 0.04, &b1), 0.13, 1e-12) && !b1 && eq(f179::deficitBoundedM(-0.11, 0.04, &b2), 0.14, 1e-12) && b2 && f179::deficitBoundedM(0.5, 0.04, &b3) == 0.0 && !b3 &&
                  f179::deficitM(-1.0, 0.04) == f179::deficitBoundedM(-1.0, 0.04, nullptr) && eq(f179::deficitM(0.0, 0.10), 0.10, 1e-12) && f179::kPushExtraCm == 10.0,
              "F183: the bound sits at the distance plus 10 cm (14 cm at 4 cm; 13 cm is whole, 15 cm is bounded and counted); deficitM is the same bound; a push the eye does not ask for is 0");
        bool b4 = false;
        check(eq(f179::deficitBoundedM(-0.5, 0.04, &b4, 0.05), 0.05, 1e-12) && b4 && eq(f179::budgetLeftM(0.04, 0.10), 0.04, 1e-12) && f179::budgetLeftM(0.04, 0.20) == 0.0 && eq(f179::budgetLeftM(0.04, 0.0), 0.14, 1e-12),
              "F183: the stages of a frame share the one bound: a stage after a 10 cm push has 4 cm of the 14 left, and none after 14 or more");
        // the filtered error is within +-5 cm
        double b = 0.0;
        for (int i = 0; i < 20; ++i) b = f179::nextBias(b, 0.5);
        double bn = 0.0;
        for (int i = 0; i < 20; ++i) bn = f179::nextBias(bn, -0.5);
        check(f179::kBiasLimitM == 0.05 && b == 0.05 && bn == -0.05 && eq(f179::nextBias(0.0, 0.02), 0.01, 1e-12) && f179::nextBias(0.03, std::nan("")) == 0.03,
              "F183: the guard's filtered prediction error is held within +-5 cm (it was 50), the gain unchanged");
        // the reset: the stable heading turning over 20 deg between calls
        const double a0[2] = {0.0, 1.0};
        double a19[2] = {std::sin(rad(19.0)), std::cos(rad(19.0))}, a21[2] = {std::sin(rad(21.0)), std::cos(rad(21.0))}, flip[2] = {std::sin(rad(30.0)), std::cos(rad(30.0))}, zero2[2] = {0.0, 0.0};
        check(f183::headingTurnDeg(a0, a19) <= f183::kResetDeg && f183::headingTurnDeg(a0, a21) > f183::kResetDeg && f183::headingTurnDeg(a0, flip) > f183::kResetDeg && std::isnan(f183::headingTurnDeg(a0, zero2)) &&
                  f183::kResetDeg == 20.0,
              "F183: a heading turn of 19 deg between calls keeps the filtered error and one of 21 deg (the flight's 30 deg flip) resets it");
        // the counters and the line
        f179::Counters c;
        for (int i = 0; i < 100; ++i) {
            f179::Sample sm;
            sm.ctx = f179::kAdsStanding;
            sm.pushM = 0.05;
            sm.bounded = i < 7;
            sm.biasReset = i < 3;
            c.addGuard(sm);
        }
        f179::Counters m;
        m.merge(c);
        m.merge(c);
        char line[1300], meas[1300];
        const int len = f179::formatContext(line, sizeof(line), 10, "$wpn_m_shotgun_plasma_doublebarrel_name;", f179::kAdsStanding, c.c[f179::kAdsStanding], 4.0);
        f179::Sample mm;
        mm.ctx = f179::kHipUnarmed;
        mm.wouldM = 0.02;
        mm.bounded = true;
        f179::Counters cm;
        cm.addGuard(mm);
        f179::formatContext(meas, sizeof(meas), 10, "x", f179::kHipUnarmed, cm.c[f179::kHipUnarmed], 4.0);
        check(c.c[f179::kAdsStanding].bounded == 7 && c.c[f179::kAdsStanding].resets == 3 && m.c[f179::kAdsStanding].bounded == 14 && m.c[f179::kAdsStanding].resets == 6 && len > 0 &&
                  std::strstr(line, ", under 3 cm 0, bounded pushes 7, error resets 3") != nullptr && std::strstr(meas, ", bounded pushes 1, error resets 0") != nullptr,
              "F183: the bounded pushes and the error resets are counted per context, merged, and end F179b's line (the acting and the measuring format)");
        f179::Counters big;
        for (int cx = 0; cx < f179::kCtxN; ++cx) {
            f179::Ctx1& k = big.c[cx];
            k.frames = k.pushed = k.offPostN = k.beforeN = k.predN = k.afterN = k.under9 = k.wouldN = k.wouldFrames = k.bounded = k.resets = 4000000000u;
            k.pushSum = k.offPostSum = k.predSum = k.afterSum = k.wouldSum = -1.0e9;
            k.pushMax = k.offPostMax = k.wouldMax = 1.0e9;
            k.beforeSum = -1.0e9; k.beforeMin = -1.0e9; k.afterMin = -1.0e9;
        }
        bool within = true;
        for (int cx = 0; cx < f179::kCtxN; ++cx) {
            char w[1400];
            const int wl = f179::formatContext(w, sizeof(w), 4000000000u, "$wpn_m_shotgun_plasma_doublebarrel_name_with_a_long_tail;", cx, big.c[cx], 20.0);
            within = within && wl > 0 && wl < 1166;
        }
        check(within, "F183: every context's line stays within the log's 1166 bytes with the new ending at its worst");
    }

    // ---- 6. the counters and the heading's line ---------------------------------------------------------------------------------------------------------------------------------------
    {
        f183::Counters c;
        f183::Info level, blended, replaced, none;
        level.valid = true;
        blended.valid = blended.changed = blended.blended = true;
        blended.angleDeg = 31.5;
        replaced.valid = replaced.changed = replaced.replaced = true;
        replaced.angleDeg = 154.0;
        for (int i = 0; i < 60; ++i) c.add(level, -8.0 + 0.1 * i, -8.0);
        for (int i = 0; i < 20; ++i) c.add(blended, 70.0 + 0.5 * i, 72.0);
        for (int i = 0; i < 10; ++i) c.add(replaced, 85.2, 95.0);
        c.add(none, 85.2, 99.0);   // no eye rows: not counted
        check(c.frames == 90 && c.blended == 20 && c.replaced == 10 && eq(c.angleMax, 154.0, 1e-12) && eq(c.aimMin, -8.0, 1e-12) && eq(c.aimMax, 85.2, 1e-12) && eq(c.eyeMax, 95.0, 1e-12),
              "F183: the counters keep the pin calls with an eye, those blended and replaced, the largest angle and the aim's and the eye's pitch ranges (a call with no eye is not counted)");
        f183::Counters m;
        m.merge(c);
        m.merge(c);
        check(m.frames == 180 && m.blended == 40 && m.replaced == 20 && eq(m.aimMax, 85.2, 1e-12) && eq(m.aimMin, -8.0, 1e-12), "F183: two windows add up");
        char line[500];
        const int len = f183::formatLine(line, sizeof(line), 1, c);
        std::snprintf(msg, sizeof(msg), "F183: the window's line names the frames, those blended, replaced, the largest angle and the pitch ranges (%d bytes)", len);
        check(len > 0 && len < 300 &&
                  std::strstr(line, "native on foot heading (F183) 1 s: frames 90, blended 20, replaced 10 (past vertical); the largest angle between the eye's heading and the stable one 154.0 deg; "
                                    "the aim pitch -8.0 to +85.2 deg, the eye's pitch up to +95.0 deg") != nullptr,
              msg);
        f183::Counters empty;
        char z[500];
        check(f183::formatLine(z, sizeof(z), 1, empty) > 0 && std::strstr(z, "frames 0, blended 0, replaced 0") != nullptr && std::strstr(z, "nan") == nullptr && std::strstr(z, "inf") == nullptr &&
                  f183::kAimOverDeg == 80.0 && f183::kEyeBlendDeg == 60.0,
              "F183: a window with no frame prints zeros, never a number that is not one");
    }

    // ---- 7. the source sites --------------------------------------------------------------------------------------------------------------------------------------------------------------
    {
        const std::string cpp = slurp("src/d3d11/native_on_foot.cpp"), core = slurp("src/d3d11/native_on_foot_core.h"), logpin = slurp("src/d3d11/native_on_foot_logpin.h");
        const auto has = [&](const std::string& needle) { return cpp.find(needle) != std::string::npos; };
        check(core.find("#include \"native_on_foot_f183.h\"") != std::string::npos && core.find("pa.stable = f183::stableHeading(eyeFwd, pa.hEye, eyeKnown);") != std::string::npos &&
                  core.find("const f183::Info st = f183::stableHeading(eyeFwd, hEye);") != std::string::npos && core.find("double wcCap = 1.0, bool eyeKnown = true) noexcept {") != std::string::npos &&
                  core.find("rest[3] = {restForward * pa.hEye[0], restUp, restForward * pa.hEye[2]};") != std::string::npos,
              "F183: pinNeckTargetAnim and the physical rest target take their heading from the stable one; everything after reads pa.hEye");
        check(has("localPitchValid, upMOk ? upUnit : nullptr, f169Cap, cycSrc != kCycNone);\n            pc.f183 = pa.stable;") && has("localPitchValid, upMOk ? upUnit : nullptr, 1.0, cycSrc != kCycNone);\n            pc.f183 = pa.stable;") &&
                  countOf(cpp, "pc.f183 = pa.stable;") == 2 && has("pc.hEye[0] = pa.hEye[0];") && has("pc.off[0] = pa.vUsed[0] * pa.hEye[2] - pa.vUsed[2] * pa.hEye[0];"),
              "F183: both pin target calls hand the stable heading to pc.hEye (every reader: pc.off, F97a, F179b, the samples) and keep the eye unknown out of the count");
        check(has("double fwdPin[3] = {fwdM[0], fwdM[1], fwdM[2]};") && has("if (pc.f183.changed) {\n            fwdPin[0] = pc.hEye[0];\n            fwdPin[2] = pc.hEye[1];") &&
                  has("std::memcpy(pc.f168Fwd, fwdPin, sizeof(pc.f168Fwd));") && has("nofoot::f168::shiftVector(pc.f168DialCm, pc.postureW, pc.detachW, fwdPin, pc.f168Shift)") &&
                  has("std::memcpy(pc.f171Fwd, fwdPin, sizeof(pc.f171Fwd));") && has("nofoot::f171::shiftVector(pc.f171DialCm, pc.postureW, pc.detachW, fwdPin, pc.f171Shift)") &&
                  has("nofoot::f178::forwardVector(pc.f178FwdDialCm, pc.postureW, pc.detachW, fwdPin, pc.f178FwdShift)") && has("nofoot::f178::leftVector(pc.f178LeftDialCm, pc.postureW, pc.detachW, fwdPin, pc.f178LeftShift)") &&
                  !has("pc.detachW, fwdM, pc.f"),
              "F183: F168's, F171's and F178's forward and left axes take the stable heading where it is not the eye's own (the eye's forward untouched otherwise)");
        check(has("const bool headingTurned = g_f179Bias.hHave && nofoot::f183::headingTurnDeg(lastH, pc.hEye) > nofoot::f183::kResetDeg;") && has("|| headingTurned) g_f179Bias = F179Bias{};") &&
                  has("pc.f179BiasReset = headingTurned;") && has("nofoot::f179::guardTarget(g179, N0, pc.f179Bias, T, pc.f179DistM, &pc.f179Bounded)") &&
                  has("nofoot::f179::guardTarget(g179, N0, pc.f179Bias, physicalTarget, pc.f179DistM, &pc.f179Bounded,") && countOf(cpp, "nofoot::f179::deficitBoundedM(") == 3 && !has("nofoot::f179::deficitM(") &&
                  has("gs.bounded = pc.f179Bounded;") && has("gs.biasReset = pc.f179BiasReset;") && has("nofoot::f179::budgetLeftM(pc.f179DistM, pc.f179PushPre)") &&
                  has("nofoot::f179::budgetLeftM(pc.f179DistM, pc.f179OffPost)") && has("nofoot::f179::budgetLeftM(pc.f179DistM, pc.f179PushPre + pc.f179OffPost)"),
              "F183: the guard's three deficits and its pre-reach push are bounded and counted, the filtered error is reset when the stable heading turns over 20 deg, both go to the sample");
        check(has("st.f183.add(pc.f183, pc.f86Ok ? pc.f86AimPitch : std::nan(\"\"), pc.scenePitchValid ? pc.scenePitchDeg : std::nan(\"\"));") && has("if (st.f183.frames) {") &&
                  has("nofoot::f183::formatLine(line183, sizeof(line183), ws, st.f183);") && has("nofoot::f183::Counters f183;") && logpin.find("d.f183.merge(s.f183);") != std::string::npos,
              "F183: the pin's stats count the stable heading per call, merge it and print its line for a window with frames");
    }
}

}  // namespace f183test
