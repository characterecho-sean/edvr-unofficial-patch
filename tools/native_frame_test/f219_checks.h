// F219 checks (native on-foot VR), run219 below. Header only, run by native_frame_test.
//
// F219 (native_on_foot_f219.h, native_on_foot.cpp): at the hip each arm's written elbow may swing about the shoulder-to-hand axis only 4 deg a frame (360 deg/s of the time between the calls). The
// rig runs the signed angle (sign, magnitude, the degenerate parts), the limit on an angle, the step for a dt, the memory's freshness, the whole limit on the real two-bone frame (a hop of 15 deg
// comes out at 4 deg at 90 Hz, the hand point and the bone lengths unchanged, either side, either orientation of the swivel, a straight arm not limited), the counts and line, F181's 45 deg search,
// and pins the source sites (where the limit sits and what it leaves alone).
#pragma once

#include "../../src/d3d11/native_on_foot_core.h"
#include "../../src/d3d11/native_on_foot_f181.h"
#include "../../src/d3d11/native_on_foot_f219.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

namespace f219test {

namespace nf = edvr::nofoot;
namespace f219 = edvr::nofoot::f219;

inline std::string slurp(const char* path) {
    std::ifstream f(path, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// A frame that swings the elbow the other way for a rising theta, to show the sign is measured and not assumed.
struct MirrorFrame {
    double S[3], hand[3], dir[3], u[3], v[3], a, h;
    void elbowAt(double theta, double E[3]) const noexcept {
        const double c = std::cos(-theta), s = std::sin(-theta);
        for (int i = 0; i < 3; ++i) E[i] = S[i] + dir[i] * a + h * (c * u[i] + s * v[i]);
    }
};

template <class Check>
inline void run219(Check&& check) {
    const auto eq = [](double a, double b, double tol) { return std::fabs(a - b) <= tol; };
    const double kPi = f219::kPi;
    const auto rad = [&](double deg) { return deg * kPi / 180.0; };
    char msg[400];

    // ---- 1. the signed angle ----------------------------------------------------------------------------------------------------------------------------------------------------------------------
    {
        const double axis[3] = {0.0, 0.0, 1.0};
        const double prev[3] = {0.2, 0.0, 0.3}, up[3] = {0.0, 0.2, -0.1}, down[3] = {0.0, -0.2, 0.5}, same[3] = {0.1, 0.0, 0.9};
        double a = 0.0;
        check(f219::signedAngleAbout(prev, up, axis, &a) && eq(a, rad(90.0), 1e-12), "F219: a quarter turn about the axis reads +90 deg, the part along the axis ignored");
        check(f219::signedAngleAbout(prev, down, axis, &a) && eq(a, rad(-90.0), 1e-12), "F219: the other way reads -90 deg");
        check(f219::signedAngleAbout(prev, same, axis, &a) && eq(a, 0.0, 1e-12), "F219: the same direction round the axis reads 0, whatever the part along it");
        const double opp[3] = {-0.3, 0.0, 0.0};
        check(f219::signedAngleAbout(prev, opp, axis, &a) && eq(std::fabs(a), kPi, 1e-9), "F219: the opposite side reads a half turn");
        const double tilted[3] = {0.0, 0.0, 1.0}, axisX[3] = {1.0, 0.0, 0.0}, py[3] = {0.0, 0.1, 0.0}, pz[3] = {0.0, 0.0, 0.1};
        check(f219::signedAngleAbout(py, pz, axisX, &a) && eq(a, rad(90.0), 1e-12) && f219::signedAngleAbout(pz, py, axisX, &a) && eq(a, rad(-90.0), 1e-12), "F219: right-handed about +x: y to z is +90 deg");
        (void)tilted;
        const double tiny[3] = {0.0049, 0.0, 0.7}, edge[3] = {0.005, 0.0, 0.7}, none[3] = {0.0, 0.0, 1.0}, okv[3] = {0.0, 0.1, 0.0};
        check(!f219::signedAngleAbout(tiny, okv, axis, &a) && !f219::signedAngleAbout(okv, tiny, axis, &a) && !f219::signedAngleAbout(none, okv, axis, &a) && f219::signedAngleAbout(edge, okv, axis, &a),
              "F219: a perpendicular part under 5 mm (either vector) is refused, one of 5 mm is read");
        const double nanv[3] = {std::nan(""), 0.0, 0.0};
        check(!f219::signedAngleAbout(nanv, okv, axis, &a) && !f219::signedAngleAbout(okv, nanv, axis, &a), "F219: a vector that is not a number is refused");
    }

    // ---- 2. the limit and the step ----------------------------------------------------------------------------------------------------------------------------------------------------------------
    {
        check(f219::limitAngle(0.5, 0.07) == 0.07 && f219::limitAngle(-0.5, 0.07) == -0.07 && f219::limitAngle(0.03, 0.07) == 0.03 && f219::limitAngle(-0.03, 0.07) == -0.03 && f219::limitAngle(0.0, 0.07) == 0.0,
              "F219: an angle is held to plus and minus the step and left alone inside it");
        check(eq(f219::maxStepRad(1.0 / 90.0), rad(4.0), 1e-12), "F219: at 90 Hz the step is 4 deg");
        check(eq(f219::maxStepRad(0.0), rad(1.5), 1e-12) && eq(f219::maxStepRad(-1.0), rad(1.5), 1e-12) && eq(f219::maxStepRad(std::nan("")), rad(1.5), 1e-12),
              "F219: a repeat call or an unreadable dt gets the 1/240 s floor (1.5 deg), never nothing");
        check(eq(f219::maxStepRad(3.0), rad(18.0), 1e-12) && eq(f219::maxStepRad(f219::kMaxDtS), rad(18.0), 1e-12), "F219: a long gap counts 0.05 s (18 deg)");
        check(f219::kMaxRateDegS == 360.0 && f219::kFreshMs == 200 && f219::kMinRadiusM == 0.005 && f219::kMaxDtS == 0.05 && eq(f219::kMinDtS, 1.0 / 240.0, 1e-15), "F219: the constants are the brief's");
    }

    // ---- 3. the memory ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------
    {
        f219::ElbowMemory m;
        const double rel[3] = {0.1, -0.2, 0.05};
        check(!m.fresh(1000, 7, 3), "F219: a memory never made is not fresh");
        m.remember(rel, 1000, 7, 3);
        check(m.fresh(1000, 7, 3) && m.fresh(1200, 7, 3) && !m.fresh(1201, 7, 3), "F219: a memory is fresh for 200 ms");
        check(!m.fresh(1100, 8, 3) && !m.fresh(1100, 7, 4), "F219: another item or another plan makes it stale");
        check(!m.fresh(999, 7, 3), "F219: a clock that went back makes it stale");
        check(m.rel[0] == 0.1 && m.rel[1] == -0.2 && m.rel[2] == 0.05, "F219: it keeps the vector");
        m.reset();
        check(!m.have && !m.fresh(1000, 7, 3) && m.rel[0] == 0.0, "F219: reset forgets it");
    }

    // ---- 4. the limit on the real two-bone frame --------------------------------------------------------------------------------------------------------------------------------------------------
    {
        const double S[3] = {0.0, 1.3, 0.0}, T[3] = {0.0, 1.0, 0.4}, ref[3] = {1.0, 0.2, 0.0};
        const double l1 = 0.3, l2 = 0.28;
        const nf::IkTwoBone tb = nf::ikTwoBoneFrame(S, T, l1, l2, ref);
        check(tb.ok, "F219: the rig's arm frame is built");
        const double dt = 1.0 / 90.0;
        const auto swingOf = [&](const f219::ElbowMemory& mem, double theta) {
            double E[3], rel[3], axis[3], phi = 0.0;
            tb.elbowAt(theta, E);
            for (int i = 0; i < 3; ++i) {
                rel[i] = E[i] - tb.S[i];
                axis[i] = tb.hand[i] - tb.S[i];
            }
            const double l = nf::ikLen(axis);
            for (double& x : axis) x /= l;
            f219::signedAngleAbout(mem.rel, rel, axis, &phi);
            return phi * 180.0 / kPi;
        };
        const auto memoryAt = [&](double theta) {
            double E[3], rel[3];
            tb.elbowAt(theta, E);
            for (int i = 0; i < 3; ++i) rel[i] = E[i] - tb.S[i];
            f219::ElbowMemory m;
            m.remember(rel, 1000, 1, 1);
            return m;
        };
        for (int sgn = -1; sgn <= 1; sgn += 2) {
            const double theta0 = 0.4;
            const f219::ElbowMemory mem = memoryAt(theta0);
            double theta = theta0 + sgn * rad(15.0);
            const double handBefore[3] = {tb.hand[0], tb.hand[1], tb.hand[2]};
            const f219::Result r = f219::limitSwivel(tb, mem, dt, &theta);
            double E[3];
            tb.elbowAt(theta, E);
            const double es[3] = {E[0] - S[0], E[1] - S[1], E[2] - S[2]}, eh[3] = {tb.hand[0] - E[0], tb.hand[1] - E[1], tb.hand[2] - E[2]};
            std::snprintf(msg, sizeof(msg), "F219: a hop of %+d deg in one frame at 90 Hz comes out at %+.3f deg (cut %.3f deg, limited %d, measured %d)", sgn * 15, swingOf(mem, theta), r.cutDeg, r.limited, r.measured);
            check(r.measured && r.limited && eq(swingOf(mem, theta), sgn * 4.0, 1e-9) && eq(r.cutDeg, 11.0, 1e-9) && eq(r.swingDeg, sgn * 15.0, 1e-9), msg);
            check(tb.hand[0] == handBefore[0] && tb.hand[1] == handBefore[1] && tb.hand[2] == handBefore[2] && eq(nf::ikLen(es), l1, 1e-9) && eq(nf::ikLen(eh), l2, 1e-9),
                  "F219: the hand point does not move and both bones keep their length");
        }
        {   // inside the limit: untouched, bit for bit
            const f219::ElbowMemory mem = memoryAt(0.4);
            double theta = 0.4 + rad(3.0);
            const double before = theta;
            const f219::Result r = f219::limitSwivel(tb, mem, dt, &theta);
            check(r.measured && !r.limited && theta == before && r.cutDeg == 0.0 && eq(r.swingDeg, 3.0, 1e-9), "F219: a swing of 3 deg is inside the limit: the swivel is not touched");
        }
        {   // a hop through 150 deg (the flight's -141 deg frames), and a 0.05 s gap that allows 18 deg
            const f219::ElbowMemory mem = memoryAt(-1.0);
            double theta = -1.0 - rad(150.0);
            const f219::Result r = f219::limitSwivel(tb, mem, dt, &theta);
            check(r.limited && eq(swingOf(mem, theta), -4.0, 1e-9) && eq(r.cutDeg, 146.0, 1e-9), "F219: a hop of 150 deg is held to 4 deg");
            double th2 = -1.0 + rad(15.0);
            const f219::Result r2 = f219::limitSwivel(tb, mem, 0.05, &th2);
            check(r2.measured && !r2.limited && eq(swingOf(mem, th2), 15.0, 1e-9), "F219: after a 0.05 s gap 15 deg is inside the 18 deg limit");
        }
        {   // a swivel that turns the elbow the other way: the sign is measured
            MirrorFrame mf{};
            for (int i = 0; i < 3; ++i) {
                mf.S[i] = tb.S[i];
                mf.hand[i] = tb.hand[i];
                mf.dir[i] = tb.dir[i];
                mf.u[i] = tb.u[i];
                mf.v[i] = tb.v[i];
            }
            mf.a = tb.a;
            mf.h = tb.h;
            double E0[3], rel0[3];
            mf.elbowAt(0.4, E0);
            for (int i = 0; i < 3; ++i) rel0[i] = E0[i] - mf.S[i];
            f219::ElbowMemory mem;
            mem.remember(rel0, 1000, 1, 1);
            double theta = 0.4 + rad(15.0);   // here a rising theta turns the elbow the other way: -15 deg
            const f219::Result r = f219::limitSwivel(mf, mem, dt, &theta);
            double E1[3], rel1[3], axis[3], phi = 0.0;
            mf.elbowAt(theta, E1);
            for (int i = 0; i < 3; ++i) {
                rel1[i] = E1[i] - mf.S[i];
                axis[i] = mf.dir[i];
            }
            f219::signedAngleAbout(rel0, rel1, axis, &phi);
            check(r.limited && eq(phi, rad(-4.0), 1e-9) && eq(theta, 0.4 + rad(4.0), 1e-9), "F219: a swivel that turns the elbow the other way is turned back the other way");
        }
        {   // an elbow on the axis (a straight arm): no angle to read, no limit
            f219::ElbowMemory mem;
            const double alongAxis[3] = {tb.dir[0] * 0.5, tb.dir[1] * 0.5, tb.dir[2] * 0.5};
            mem.remember(alongAxis, 1000, 1, 1);
            double theta = 2.0;
            const f219::Result r = f219::limitSwivel(tb, mem, dt, &theta);
            check(!r.measured && !r.limited && theta == 2.0, "F219: a remembered elbow on the axis cannot be read: no limit");
        }
    }

    // ---- 5. the counts and the line ---------------------------------------------------------------------------------------------------------------------------------------------------------------
    {
        f219::Stats st;
        check(!st.any(), "F219: a window with no hip solve has nothing to say");
        for (int i = 0; i < 100; ++i) st.add(0, i < 30, i < 30 ? 10.0 + i * 0.1 : 0.0);
        for (int i = 0; i < 50; ++i) st.add(1, false, 0.0);
        st.add(2, true, 99.0);
        st.add(-1, true, 99.0);
        check(st.any() && st.solves[0] == 100 && st.limited[0] == 30 && eq(st.cutMaxDeg[0], 12.9, 1e-9) && st.solves[1] == 50 && st.limited[1] == 0 && st.cutMaxDeg[1] == 0.0,
              "F219: the window counts each side's solves, those limited and the largest cut; a side that is not 0 or 1 is not counted");
        char line[400];
        f219::formatLine(line, sizeof(line), 10, st);
        check(std::string(line) == "native on foot elbow step (F219) 10 s: left solves 100 limited 30 largest cut 12.9 deg; right solves 50 limited 0 largest cut 0.0 deg", line);
        st.clear();
        check(!st.any(), "F219: clear empties the window");
    }

    // ---- 6. F181's search is held to 45 deg of turn -----------------------------------------------------------------------------------------------------------------------------------------------
    {
        check(eq(nf::f181::kMaxTurnRad, rad(45.0), 1e-15), "F219: the cap's search turns the pole at most 45 deg");
    }

    // ---- 7. the sites -----------------------------------------------------------------------------------------------------------------------------------------------------------------------------
    {
        const std::string cpp = slurp("src/d3d11/native_on_foot.cpp"), rec = slurp("src/common/onfoot_recorder_core.h");
        const auto has = [&](const std::string& hay, const char* needle) { return hay.find(needle) != std::string::npos; };
        const size_t cap = cpp.find("nofoot::f181::capSwivel(theta, planeBefore, target, planeAt)"), f76 = cpp.find("static double s_swivelPrevious[2] = {};"), keep = cpp.find("s_swivelPrevious[side] = theta; s_swivelHave[side] = true;"),
                     limit = cpp.find("nofoot::f219::limitSwivel(tb, mem, dt, &theta)"), hinge = cpp.find("// The verification of the hinge row"), remember = cpp.find("mem.remember(relNow, now, f219Item, pl.generation);");
        check(cap != std::string::npos && f76 != std::string::npos && keep != std::string::npos && limit != std::string::npos && hinge != std::string::npos && remember != std::string::npos && cap < f76 && f76 < keep &&
                  keep < limit && limit < remember && remember < hinge,
              "F219: the limit sits after the cap and the F76 swivel choice, which keeps its previous swivel first, and before the arm's bones are built");
        const std::string block = limit != std::string::npos && hinge != std::string::npos && limit < hinge ? cpp.substr(limit, hinge - limit) : std::string();
        check(!block.empty() && !has(block, "physicalPoleCommit") && !has(block, "s_swivelPrevious") && !has(block, "stablePoleReferenceTheta") && !has(block, "seed.") && !has(block, "g_bendApplied"),
              "F219: the limit does not touch the pole's seed, its reference theta, the previous swivel or the physical pole commit");
        check(has(cpp, "const bool atHip = pinCall.valid && nofoot::f159::atHip(pinCall.detachW);") && has(cpp, "if (!doSolveWrite) {\n                mem.reset();") && has(cpp, "mem.fresh(now, f219Item, pl.generation)") &&
                  has(cpp, "const nofoot::HingeSwings hsLimited = nofoot::ikHingeSwings(S, F0, H0, E, tb.hand, useRow ? rowN0 : nullptr, hl.lastValid && now - hl.lastMs < 200 ? hl.last : nullptr);") &&
                  has(cpp, "o.f219Limited = true;"),
              "F219: only at the hip, the memory forgotten by a side that does not write and used only fresh, the hinge measured again as the cap does");
        check(has(cpp, "nofoot::f219::formatLine(stepLine") && has(cpp, "g_f219Box.write(") && has(cpp, "g_f219Box.clearPending.store(true"), "F219: the window's line is printed from the solve's window and cleared at an edge");
        check(has(cpp, "r.l_swivel_limited = o.f219Limited;") && has(cpp, "r.r_swivel_limited = o.f219Limited;") && has(cpp, "r.l_swivel_cut_deg = static_cast<float>(o.f219CutDeg);") &&
                  has(cpp, "r.r_swivel_cut_deg = static_cast<float>(o.f219CutDeg);") && has(rec, "S(u8, p##swivel_limited)") && has(rec, "S(f32, p##swivel_cut_deg)"),
              "F219: the ik record carries the limit and the cut for each arm");
    }
}

}  // namespace f219test
