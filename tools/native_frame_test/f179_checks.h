// F179 checks (native on-foot VR), run179 below. Header only, run by native_frame_test.
//
// F179a (native_on_foot_f179.h): the body stays up in standing ADS. With the standing share (F178's weight, (1 - F163's crouched weight) x the detach weight) the neck target keeps the raise of
// the hip's rule in the sights: the neck at the eye as drawn less the eye height key, a raise only, capped by the legs' room as F97a caps it (legRaiseRoom: no foot lifted). Crouched, at the hip
// and unarmed it is the earlier value bit for bit.
//
// F179b: the neck guard. The eye as drawn is kept at least 10 cm ahead of the chest along the eye's horizontal heading (F97a's measure): a deficit predicted from the body's move comes off the
// target before the reach rule, then off the post-reach forward add (F169, F178), then off the move last. The rig pins the deficit, the axis, the order of the three, that the guard has the last
// word over random scenes, the context, the counters and the line, the F97a line's bins, and the source sites.
#pragma once

#include "../../src/d3d11/native_on_foot_core.h"
#include "../../src/d3d11/native_on_foot_f159.h"
#include "../../src/d3d11/native_on_foot_f165.h"
#include "../../src/d3d11/native_on_foot_f168.h"
#include "../../src/d3d11/native_on_foot_f178.h"
#include "../../src/d3d11/native_on_foot_f179.h"
#include "../../src/d3d11/native_on_foot_f97.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

namespace f179test {

namespace nf = edvr::nofoot;
namespace f97 = edvr::nofoot::f97;
namespace f179 = edvr::nofoot::f179;

inline std::string slurp(const char* path) {
    std::ifstream f(path, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// One scene through the pin's three stages, as the pin makes them: the guard on the target, the reach rule's correction, the post-reach add trimmed, the residual taken off last.
struct Scene {
    double eye[3] = {0.0, 1.55, 0.0}, chest[3] = {0.0, 1.30, -0.35}, n0[3] = {0.0, 1.35, -0.30};
    double target[3] = {0.0, 1.35, -0.30};   // the neck target before the guard
    double reach[3] = {};                    // the reach rule's correction (horizontal)
    double post[3] = {};                     // the post-reach add (horizontal)
    double hx = 0.0, hz = 1.0, bias = 0.0;
};
struct Result {
    bool ok = false, bounded = false;   // F183: bounded: a stage asked more than the frame's bound (the distance plus 10 cm, shared by the three stages) allows
    double pre = 0.0, offPost = 0.0, residual = 0.0, aheadBefore = 0.0, aheadAfter = 0.0, thFinal[3] = {}, targetOut[3] = {};
};
inline Result runScene(const Scene& s, double distM = f179::kNeckGuardAheadM) {
    Result r;
    const f179::Geometry g = f179::makeGeometry(true, s.eye, s.hx, s.hz, true, s.chest);
    r.ok = g.ok;
    if (!g.ok) return r;
    double t[3] = {s.target[0], s.target[1], s.target[2]};
    r.pre = f179::guardTarget(g, s.n0, s.bias, t, distM, &r.bounded);
    for (int i = 0; i < 3; ++i) r.targetOut[i] = t[i];
    const nf::PinMove mv = nf::pinMove(s.n0, t);
    double th[3] = {mv.th[0] + s.reach[0], 0.0, mv.th[2] + s.reach[2]};
    double post[3] = {s.post[0], 0.0, s.post[2]};
    const double withPost[3] = {th[0] + post[0], 0.0, th[2] + post[2]};
    r.aheadBefore = g.aheadWith(withPost) + s.bias - 0.0;
    const double deficit = f179::deficitBoundedM(g.aheadWith(withPost) + s.bias, distM, &r.bounded, f179::budgetLeftM(distM, r.pre));
    double res[3] = {};
    if (deficit > 0.0) {
        const f179::Trim tr = f179::trimPost(post, g.h, deficit);
        if (tr.takenM > 0.0) {
            post[0] *= tr.scale;
            post[2] *= tr.scale;
            r.offPost = tr.takenM;
        }
        if (tr.residualM > 0.0) {
            f179::pushBack(res, g.h, tr.residualM);
            r.residual = tr.residualM;
        }
    }
    for (int i = 0; i < 3; ++i) r.thFinal[i] = th[i] + post[i] + res[i];
    r.aheadAfter = g.aheadWith(r.thFinal) + s.bias;
    return r;
}

template <class Check>
inline void run179(Check&& check) {
    const auto eq = [](double a, double b, double tol) { return std::fabs(a - b) <= tol; };
    char msg[640];

    // ---- F179a 1. the standing raise: the rule ---------------------------------------------------------------------------------------------------------------------------------------------
    {
        // the flight's scene: the animated neck 1.290 m, the eye 1.595 m, the key 15 cm: the neck at the eye less the key is 1.445 m, a raise of 15.5 cm
        const double n0 = 1.290, eye = 1.595, key = 0.15;
        check(eq(f179::standingRaiseM(n0, eye, key, 0.0, 1.0), 0.155, 1e-12), "F179a: standing in the sights (crouched weight 0, detach 1) the raise asked is the eye as drawn less the eye height key, over the animated neck: 1.595 - 0.15 - 1.290 = 15.5 cm");
        check(f179::standingRaiseM(n0, eye, key, 1.0, 1.0) == 0.0 && f179::standingRaiseM(n0, eye, key, 0.0, 0.0) == 0.0 && f179::standingRaiseM(n0, eye, key, 1.0, 0.0) == 0.0 &&
                  f179::standingRaiseM(n0, eye, key, 0.5, 1.0) > 0.0775 - 1e-12 && f179::standingRaiseM(n0, eye, key, 0.5, 1.0) < 0.0775 + 1e-12 && eq(f179::standingRaiseM(n0, eye, key, 0.0, 0.5), 0.0775, 1e-12),
              "F179a: the raise is the standing share's: 0 crouched (crouched weight 1), 0 at the hip (detach 0), half at half a crouch or half the detach, whole standing in the sights");
        check(f179::standingRaiseM(1.50, 1.595, 0.15, 0.0, 1.0) == 0.0 && f179::standingRaiseM(1.446, 1.595, 0.15, 0.0, 1.0) == 0.0 && f179::standingRaiseM(1.60, 1.595, 0.15, 0.0, 1.0) == 0.0,
              "F179a: a neck already at the eye less the key, or above it, is not lowered: the raise is 0 (F97a's lowering is the crouched share's and stays as it was)");
        check(f179::standingRaiseM(std::nan(""), eye, key, 0.0, 1.0) == 0.0 && f179::standingRaiseM(n0, std::nan(""), key, 0.0, 1.0) == 0.0 && f179::standingRaiseM(n0, eye, std::nan(""), 0.0, 1.0) == 0.0 &&
                  eq(f179::standingRaiseM(n0, eye, key, std::nan(""), 1.0), 0.155, 1e-12) && f179::standingRaiseM(n0, eye, key, 0.0, std::nan("")) == 0.0,
              "F179a: a neck, eye or key that is not a number asks nothing; a crouched weight that is not a number reads standing (as F161 reads it), a detach that is not a number the hip");
        double last = -1.0;
        bool rising = true;
        for (int i = 0; i <= 10; ++i) {
            const double r = f179::standingRaiseM(n0, eye, key, 0.0, i / 10.0);
            rising = rising && r >= last;
            last = r;
        }
        check(rising && eq(last, 0.155, 1e-12), "F179a: through a press (the detach weight rising, standing) the raise rises without a step from 0 to the whole 15.5 cm");
    }

    // ---- F179a 2. the armed target with the raise: the earlier value bit for bit where the share is 0 ---------------------------------------------------------------------------------------------
    {
        const double n0 = 1.290, key = 0.15;
        bool identical = true;
        // crouched (any posture weight 1), at the hip (detach 0) and unarmed (F88's weight 0): the target is F97a's, field for field
        const double eyes[3] = {1.53, 1.595, 1.40};
        for (double eye : eyes)
            for (double t93 : {1.29, 1.33, 1.38})
                for (double dw : {0.0, 0.3, 1.0})
                    for (double w : {0.0, 0.5, 1.0})
                        for (double cw : {0.0, 0.4, 1.0}) {
                            const f97::ArmedTarget base = f97::armedNeckTarget(n0, t93, eye, key, dw, w, cw, t93);
                            const double sr = f179::standingRaiseM(n0, eye, key, cw, dw);
                            const f97::ArmedTarget a = f179::withStandingRaise(base, n0, w, sr);
                            if (cw >= 1.0 || dw <= 0.0 || w <= 0.0)
                                identical = identical && std::memcmp(&a, &base, sizeof(a)) == 0;
                            else
                                identical = identical && a.y >= base.y && a.asked >= base.asked && a.raiseF93 == base.raiseF93 && a.lowerEye == base.lowerEye;
                        }
        check(identical, "F179a: crouched (weight 1), at the hip (detach 0) and unarmed (F88's weight 0) F97a's armed target is returned field for field; elsewhere the raise only lifts it and F93's and the lowering's figures stay");
        // standing in the sights: F97a releases the raise with the detach (the flight: neck 1.290 = animated); F179a keeps the eye-less-key raise, F88's weight on it
        const f97::ArmedTarget released = f97::armedNeckTarget(n0, n0, 1.595, key, 1.0, 1.0, 0.0, n0);
        const f97::ArmedTarget held = f179::withStandingRaise(released, n0, 1.0, f179::standingRaiseM(n0, 1.595, key, 0.0, 1.0));
        const f97::ArmedTarget half = f179::withStandingRaise(released, n0, 0.5, f179::standingRaiseM(n0, 1.595, key, 0.0, 1.0));
        std::snprintf(msg, sizeof(msg), "F179a: the flight's scene (neck 1.290, eye 1.595, key 15 cm, standing in the sights): F97a alone leaves the neck at %.3f m, with the standing raise it asks %.3f m (%+.1f cm), half weight %.3f",
                      released.y, held.y, 100.0 * (held.y - n0), half.y);
        check(eq(released.y, n0, 1e-12) && eq(held.y, 1.445, 1e-12) && eq(held.asked, 0.155, 1e-12) && eq(half.y, n0 + 0.5 * 0.155, 1e-12), msg);
        // F93's own raise (the hip's +4 cm) larger than the standing raise stays: the larger of the two
        const f97::ArmedTarget big = f97::armedNeckTarget(n0, n0 + 0.30, 1.595, key, 0.0, 1.0, 0.0, n0 + 0.30);
        const f97::ArmedTarget kept = f179::withStandingRaise(big, n0, 1.0, 0.05);
        check(std::memcmp(&kept, &big, sizeof(kept)) == 0, "F179a: a demand F97a already asks above the standing raise is kept as it was");
        // not a number
        const f97::ArmedTarget nn = f179::withStandingRaise(released, std::nan(""), 1.0, 0.1);
        check(std::isnan(nn.y) == std::isnan(released.y) && f179::withStandingRaise(released, n0, 1.0, std::nan("")).y == released.y && f179::withStandingRaise(released, n0, 1.0, -0.05).y == released.y,
              "F179a: a neck, or a raise that is not a number or is negative, leaves the target as it came");
    }

    // ---- F179a 3. the legs' cap: no lifted foot -----------------------------------------------------------------------------------------------------------------------------------------------------
    {
        // legs hip to foot 0.805 m over a 0.90 m leg (the flight's 0.895 ratio), the animated leg's own distance as long: the room is the planting rule's reach less the hip's height now
        const double l1 = 0.45, l2 = 0.45, hipY = 0.805, footY = 0.0;
        const double room = f97::legRaiseRoom(hipY, footY, l1, l2, hipY, nf::kStandReach);
        const double asked = 0.155;
        const double delta = asked < room ? asked : room;   // the pin's cap: min(the raise, the room)
        const double hipAfter = hipY + delta;
        const f97::ArmedTarget standing = f179::withStandingRaise(f97::armedNeckTarget(1.290, 1.290, 1.595, 0.15, 1.0, 1.0, 0.0, 1.290), 1.290, 1.0, f179::standingRaiseM(1.290, 1.595, 0.15, 0.0, 1.0));
        std::snprintf(msg, sizeof(msg), "F179a: the raise asked (%.1f cm) beyond the legs' room (%.1f cm for a 0.90 m leg at 0.895) is capped to the room: the hip stands %.3f over the foot, within the planting reach %.3f; no foot lifts",
                      100.0 * (standing.y - 1.290), 100.0 * room, hipAfter - footY, nf::kStandReach * (l1 + l2));
        check(standing.y - 1.290 > room && room > 0.0 && delta == room && hipAfter - footY <= nf::kStandReach * (l1 + l2) + 1e-9, msg);
        const double roomAt80 = f97::legRaiseRoom(0.80, 0.0, l1, l2, 0.80, nf::kStandReach);
        check(0.03 < roomAt80 && std::min(0.03, roomAt80) == 0.03, "F179a: a raise inside the room is written whole");
        check(f97::legRaiseRoom(0.90, 0.0, l1, l2, 0.90, nf::kStandReach) < 0.0 + 1e-3, "F179a: legs already straight leave no room (the raise would be capped to nothing)");
    }

    // ---- F179b 1. the deficit and the axis -----------------------------------------------------------------------------------------------------------------------------------------------------
    {
        check(eq(f179::deficitM(0.06), 0.04, 1e-12) && f179::deficitM(0.12) == 0.0 && f179::deficitM(0.10) == 0.0 && eq(f179::deficitM(-0.05), 0.15, 1e-12) && f179::deficitM(std::nan("")) == 0.0 &&
                  eq(f179::deficitM(0.0), 0.10, 1e-12) && f179::kNeckGuardAheadCm == 10.0,
              "F179b: the deficit is 10 cm less the eye ahead of the chest: 6 cm ahead pushes 4, 12 cm ahead pushes 0, 5 cm behind pushes to 10 (15 cm), a figure that is not a number pushes nothing");
        bool along = true, alongNeg = true, level = true, reaches = true, perpendicular = true;
        for (int deg = 0; deg < 360; deg += 30) {
            const double a = deg * 3.14159265358979323846 / 180.0;
            const double hx = 2.0 * std::sin(a), hz = 2.0 * std::cos(a);   // not a unit: the guard normalises it
            Scene s;
            s.hx = hx;
            s.hz = hz;
            s.eye[0] = 0.4; s.eye[1] = 1.55; s.eye[2] = -0.2;
            // the chest 6 cm behind the eye along the heading
            s.chest[0] = s.eye[0] - 0.06 * std::sin(a); s.chest[1] = 1.30; s.chest[2] = s.eye[2] - 0.06 * std::cos(a);
            s.n0[0] = s.chest[0]; s.n0[1] = 1.35; s.n0[2] = s.chest[2];
            s.target[0] = s.n0[0]; s.target[1] = 1.35; s.target[2] = s.n0[2];
            const Result r = runScene(s);
            const double dx = r.targetOut[0] - s.target[0], dz = r.targetOut[2] - s.target[2];
            along = along && r.ok && eq(r.pre, 0.04, 1e-12) && eq(dx, -0.04 * std::sin(a), 1e-12) && eq(dz, -0.04 * std::cos(a), 1e-12);
            level = level && r.targetOut[1] == s.target[1];
            perpendicular = perpendicular && eq(dx * std::cos(a) - dz * std::sin(a), 0.0, 1e-12);
            reaches = reaches && eq(r.aheadAfter, 0.10, 1e-12);
            // the chest 12 cm ahead of the eye... (behind the eye by -12 cm): no push
            Scene t = s;
            t.chest[0] = t.eye[0] - 0.12 * std::sin(a); t.chest[2] = t.eye[2] - 0.12 * std::cos(a);
            t.n0[0] = t.chest[0]; t.n0[2] = t.chest[2]; t.target[0] = t.n0[0]; t.target[2] = t.n0[2];
            const Result q = runScene(t);
            alongNeg = alongNeg && q.ok && q.pre == 0.0 && std::memcmp(q.targetOut, t.target, sizeof(q.targetOut)) == 0;
        }
        check(along && level && perpendicular && reaches, "F179b: at every heading (every 30 deg, the heading not a unit) a chest 6 cm behind the eye moves the target 4 cm back along the heading, none across it and none vertical, and the eye ends 10 cm ahead");
        check(alongNeg, "F179b: a chest 12 cm behind the eye pushes nothing and leaves the target bit for bit");
        // a pitched eye (its height far from the chest's) changes nothing: the measure is along the horizontal heading
        Scene p;
        p.eye[1] = 1.90; p.chest[2] = -0.06; p.n0[2] = -0.06; p.target[2] = -0.06;
        Scene q = p;
        q.eye[1] = 1.20;
        check(runScene(p).ok && eq(runScene(p).pre, runScene(q).pre, 1e-15) && eq(runScene(p).pre, 0.04, 1e-12), "F179b: the eye's height (a pitched look) does not enter: the measure is along the horizontal heading only");
        // a heading with no horizontal part is not read
        Scene h;
        h.hx = 0.1; h.hz = 0.1;
        Scene h2;
        h2.hx = 0.0; h2.hz = 0.0;
        Scene h3;
        h3.hx = std::nan(""); h3.hz = 1.0;
        check(!runScene(h).ok && !runScene(h2).ok && !runScene(h3).ok && runScene(Scene{}).ok, "F179b: a heading under 0.5 horizontal (F97a's own gate), none or not a number is not read: the guard does not run");
        // the chest or the eye not readable
        const double bad[3] = {std::nan(""), 0.0, 0.0}, good[3] = {0.0, 1.0, 0.0};
        check(!f179::makeGeometry(false, good, 0.0, 1.0, true, good).ok && !f179::makeGeometry(true, good, 0.0, 1.0, false, good).ok && !f179::makeGeometry(true, bad, 0.0, 1.0, true, good).ok &&
                  !f179::makeGeometry(true, good, 0.0, 1.0, true, bad).ok && f179::makeGeometry(true, good, 0.0, 1.0, true, good).ok,
              "F179b: an eye or a chest that is not read, or not a number, is no geometry");
        double t0[3] = {0.1, 1.3, 0.2};
        f179::Geometry gbad;
        check(f179::guardTarget(gbad, good, 0.0, t0) == 0.0 && t0[0] == 0.1 && t0[1] == 1.3 && t0[2] == 0.2, "F179b: with no geometry the target is untouched");
    }

    // ---- F179b 2. the order: the target first, the post-reach add next, the move last; the guard has the last word ----------------------------------------------------------------------------------
    {
        // the deficit before the reach rule is taken off the target
        Scene a;   // the chest 6 cm behind the eye
        a.chest[2] = -0.06; a.n0[2] = -0.06; a.target[2] = -0.06;
        a.post[2] = 0.145;   // a +14.5 cm forward dial added after the reach rule
        Result r = runScene(a);
        check(r.ok && eq(r.pre, 0.04, 1e-12) && eq(r.targetOut[2], -0.06 - 0.04, 1e-12), "F179b: a deficit found before the reach rule goes to the target, so the reach rule works from it");
        // a deficit that only the post-reach add causes is taken off that add, first
        Scene b;   // the chest 25 cm behind the eye, the dial moves the body 14.5 cm toward the gun: ahead 10.5 before it... 25 - 14.5 = 10.5 -> no deficit; 20 cm behind: 5.5 -> deficit 4.5
        b.chest[2] = -0.20; b.n0[2] = -0.20; b.target[2] = -0.20;
        b.post[2] = 0.145;
        r = runScene(b);
        std::snprintf(msg, sizeof(msg), "F179b: the +14.5 cm post-reach add alone leaves the eye 5.5 cm ahead of the chest (20 cm less 14.5): the guard takes %.1f cm off that add (no push on the target: %.1f), the residual %.1f, and the eye ends %.1f cm ahead",
                      100.0 * r.offPost, 100.0 * r.pre, 100.0 * r.residual, 100.0 * r.aheadAfter);
        check(r.ok && r.pre == 0.0 && eq(r.offPost, 0.045, 1e-12) && r.residual == 0.0 && eq(r.aheadAfter, 0.10, 1e-12) && eq(r.thFinal[2], 0.10, 1e-12), msg);
        // an add that cannot cover the deficit is taken off whole, and the rest is the residual taken off the move
        Scene c;
        c.chest[2] = -0.06; c.n0[2] = -0.06; c.target[2] = -0.06;
        c.reach[2] = 0.08;   // the reach rule pulls the body forward after the target was pushed back
        c.post[2] = 0.03;
        r = runScene(c);
        check(r.ok && eq(r.pre, 0.04, 1e-12) && eq(r.offPost, 0.03, 1e-12) && eq(r.residual, 0.08, 1e-12) && eq(r.aheadAfter, 0.10, 1e-12) && r.thFinal[2] < 0.0,
              "F179b: the reach rule's pull (8 cm) after the 4 cm push leaves the eye 1 cm behind the chest with the add: the add goes whole (3 cm) and the rest (8 cm) comes off the move last: the guard has the last word");
        // an add across the heading has nothing along it to give: the whole deficit is the residual
        Scene d;
        d.chest[2] = -0.06; d.n0[2] = -0.06; d.target[2] = -0.06;
        d.reach[2] = 0.08;
        d.post[0] = 0.20;   // sideways
        r = runScene(d);
        check(r.ok && r.offPost == 0.0 && eq(r.residual, 0.08, 1e-12) && eq(r.thFinal[0], 0.20, 1e-15) && eq(r.aheadAfter, 0.10, 1e-12), "F179b: a post-reach add across the heading gives nothing; its sideways part stays whole and the deficit is taken off the move");
        // trim
        const double post[3] = {0.0, 0.0, 0.14}, h[3] = {0.0, 0.0, 1.0};
        const f179::Trim t1 = f179::trimPost(post, h, 0.04), t2 = f179::trimPost(post, h, 0.20), t3 = f179::trimPost(post, h, 0.0), t4 = f179::trimPost(post, h, -0.1);
        check(eq(t1.scale, 1.0 - 0.04 / 0.14, 1e-12) && eq(t1.takenM, 0.04, 1e-15) && t1.residualM == 0.0 && t2.scale == 0.0 && eq(t2.takenM, 0.14, 1e-15) && eq(t2.residualM, 0.06, 1e-12) &&
                  t3.scale == 1.0 && t3.takenM == 0.0 && t3.residualM == 0.0 && t4.scale == 1.0 && t4.residualM == 0.0,
              "F179b: the trim takes the deficit off the add along the heading (4 of 14 cm), all of it at most (14 cm), and nothing for no deficit");
        const double back[3] = {0.0, 0.0, -0.10};
        const f179::Trim t5 = f179::trimPost(back, h, 0.04);
        check(t5.scale == 1.0 && t5.takenM == 0.0 && eq(t5.residualM, 0.04, 1e-15), "F179b: an add that goes backward along the heading has nothing to give");
    }

    // ---- F179b 3. the guard has the last word over many scenes ----------------------------------------------------------------------------------------------------------------------------------
    {
        // a deterministic spread of scenes: the chest 0 to 40 cm behind the eye along a heading, the target within 30 cm of the neck, the reach rule's pull -10 to +15 cm, the add 0 to 20 cm
        // forward or sideways, a prediction error -5 to +5 cm. Whatever they are, the eye is predicted at least 10 cm ahead after the three stages (the bias included).
        uint32_t seed = 12345u;
        const auto rnd = [&seed](double lo, double hi) {
            seed = seed * 1664525u + 1013904223u;
            return lo + (hi - lo) * (static_cast<double>(seed >> 8) / 16777216.0);
        };
        int scenes = 0, held = 0, pushed = 0, noPush = 0, capped = 0;
        bool inBound = true;
        double worst = 1e9;
        for (int i = 0; i < 4000; ++i) {
            Scene s;
            const double a = rnd(0.0, 6.2831853);
            s.hx = std::sin(a); s.hz = std::cos(a);
            const double behind = rnd(0.0, 0.40);
            s.eye[0] = rnd(-1.0, 1.0); s.eye[2] = rnd(-1.0, 1.0);
            s.chest[0] = s.eye[0] - behind * s.hx + rnd(-0.1, 0.1) * s.hz; s.chest[2] = s.eye[2] - behind * s.hz - rnd(-0.1, 0.1) * s.hx;
            s.n0[0] = s.chest[0] + rnd(-0.05, 0.05); s.n0[2] = s.chest[2] + rnd(-0.05, 0.05);
            s.target[0] = s.n0[0] + rnd(-0.3, 0.3); s.target[2] = s.n0[2] + rnd(-0.3, 0.3);
            const double reach = rnd(-0.10, 0.15), add = rnd(0.0, 0.20), side = rnd(-0.1, 0.1);
            s.reach[0] = reach * s.hx; s.reach[2] = reach * s.hz;
            s.post[0] = add * s.hx + side * s.hz; s.post[2] = add * s.hz - side * s.hx;
            s.bias = rnd(-0.05, 0.05);
            const Result r = runScene(s);
            ++scenes;
            worst = std::min(worst, r.aheadAfter);
            const double pushTotal = r.pre + r.offPost + r.residual;
            inBound = inBound && pushTotal <= f179::pushCapM(f179::kNeckGuardAheadM) + 1e-9;   // F183: the push of the frame is at most the distance plus 10 cm
            if (r.aheadAfter >= 0.10 - 1e-9) ++held;
            else if (r.bounded && pushTotal >= f179::pushCapM(f179::kNeckGuardAheadM) - 1e-9) ++capped;   // F183: the scene asked more than the bound allows: the eye ends short of 10 cm
            if (pushTotal > 0.0) ++pushed; else ++noPush;
        }
        std::snprintf(msg, sizeof(msg), "F179b (F183: the push of a frame at most the distance plus 10 cm): over %d scenes (the chest 0-40 cm behind the eye, the target, the reach pull -10..+15 cm, the post-reach add 0-20 cm, a prediction error of +-5 cm) the predicted eye ends 10 cm ahead of the chest or more in %d, and in the %d that asked more than 20 cm the push is held to 20 (the least %.4f m); %d needed the guard, %d did not",
                      scenes, held, capped, worst, pushed, noPush);
        check(inBound && held + capped == scenes && capped > 0 && capped < scenes / 4 && pushed > 500 && noPush > 100, msg);
        // a scene that needs nothing is left bit for bit: the target, and no add taken off
        Scene ok;
        ok.chest[2] = -0.50; ok.n0[2] = -0.50; ok.target[2] = -0.50;
        ok.reach[2] = 0.05; ok.post[2] = 0.10;
        const Result r = runScene(ok);
        check(r.ok && r.pre == 0.0 && r.offPost == 0.0 && r.residual == 0.0 && std::memcmp(r.targetOut, ok.target, sizeof(r.targetOut)) == 0, "F179b: a body far enough behind the eye is left bit for bit: no push, no add taken off");
    }

    // ---- F179b 4. the filtered prediction error ------------------------------------------------------------------------------------------------------------------------------------------------
    {
        double b = 0.0;
        for (int i = 0; i < 12; ++i) b = f179::nextBias(b, -0.04);
        check(eq(b, -0.04, 1e-4) && f179::nextBias(0.0, -0.04) == -0.02 && f179::nextBias(0.1, std::nan("")) == 0.1 && f179::nextBias(0.0, 3.0) == f179::kBiasLimitM && f179::nextBias(0.0, -3.0) == -f179::kBiasLimitM && f179::kBiasLimitM == 0.05,
              "F179b (F183: held to 5 cm, it was 50): the prediction error measured on the call before is filtered (half of each new error), converges on a steady error (-4 cm in 12 calls), is held to 5 cm and ignores one that is not a number");
        // a model that misses by 4 cm (the torso turns the chest forward; the filter holds 5 cm at most since F183): the stateless guard misses it, the filtered one holds from the fourth call on
        Scene s;
        s.chest[2] = -0.10; s.n0[2] = -0.10; s.target[2] = -0.10;
        s.post[2] = 0.0;
        const double miss = -0.04;   // the chest ends 4 cm further forward than the translation says
        double bias = 0.0;
        int heldAfter = 0, heldStateless = 0, heldEarly = 0;
        for (int call = 0; call < 10; ++call) {
            s.bias = bias;
            const Result r = runScene(s);
            const double measured = r.aheadAfter - s.bias + miss;   // the raw prediction (with the guard's push) plus the model's miss
            if (call >= 5 && measured >= 0.10 - 0.005) ++heldAfter;
            if (call < 3 && measured >= 0.10 - 0.005) ++heldEarly;
            Scene st = s;
            st.bias = 0.0;
            if (runScene(st).aheadAfter + miss >= 0.10 - 0.005) ++heldStateless;
            bias = f179::nextBias(bias, measured - (r.aheadAfter - s.bias));
        }
        check(heldAfter == 5 && heldEarly == 0 && heldStateless == 0,
              "F179b: a model that misses the chest by 4 cm is not held by the prediction alone; the filter (the error halves each call, held to 5 cm since F183) holds it to within 5 mm from the sixth call (about 60 ms at 90 calls a second)");
    }

    // ---- F179b 5. the contexts and the counters -------------------------------------------------------------------------------------------------------------------------------------------------
    {
        check(f179::contextOf(false, 0.0, 0.0) == f179::kHipUnarmed && f179::contextOf(true, 0.0, 0.0) == f179::kHipArmed && f179::contextOf(true, 1.0, 0.0) == f179::kAdsStanding &&
                  f179::contextOf(true, 1.0, 1.0) == f179::kAdsCrouched && f179::contextOf(true, 0.04, 1.0) == f179::kHipArmed && f179::contextOf(true, 0.05, 0.49) == f179::kAdsStanding &&
                  f179::contextOf(true, 0.05, 0.5) == f179::kAdsCrouched && f179::contextOf(true, 0.0, 1.0) == f179::kHipArmed && f179::contextOf(false, 1.0, 0.0) == f179::kAdsStanding,
              "F179b: the contexts are hip unarmed, hip armed, ADS standing and ADS crouched: the sights from the detach weight 0.05, crouched from the crouched weight 0.5; unarmed counted apart from armed");
        check(std::strcmp(f179::ctxName(0), "hip unarmed") == 0 && std::strcmp(f179::ctxName(1), "hip armed") == 0 && std::strcmp(f179::ctxName(2), "ADS standing") == 0 &&
                  std::strcmp(f179::ctxName(3), "ADS crouched") == 0,
              "F179b: the contexts' names");
        f179::Counters c;
        for (int i = 0; i < 100; ++i) {
            f179::Sample s;
            s.ctx = f179::kAdsStanding;
            s.pushM = i < 20 ? 0.04 : 0.0;
            s.offPostM = i < 10 ? 0.02 : 0.0;
            s.beforeM = i < 20 ? 0.06 : 0.20;
            s.predM = i < 20 ? 0.10 : 0.20;
            c.addGuard(s);
            c.addMeasured(f179::kAdsStanding, i < 90 ? 0.10 : 0.08);
        }
        for (int i = 0; i < 50; ++i) {   // unarmed hip frames, counted apart: no push
            f179::Sample s;
            s.ctx = f179::kHipUnarmed;
            s.beforeM = 0.34; s.predM = 0.34;
            c.addGuard(s);
            c.addMeasured(f179::kHipUnarmed, 0.33);
        }
        const f179::Ctx1& ads = c.c[f179::kAdsStanding];
        const f179::Ctx1& hip = c.c[f179::kHipUnarmed];
        check(ads.frames == 100 && ads.pushed == 20 && ads.offPostN == 10 && ads.afterN == 100 && ads.under9 == 10 && eq(ads.pushSum, 0.8, 1e-12) && ads.pushMax == 0.04 && eq(ads.afterMin, 0.08, 1e-15) &&
                  hip.frames == 50 && hip.pushed == 0 && hip.under9 == 0 && hip.afterN == 50 && c.c[f179::kHipArmed].frames == 0 && c.frames() == 150,
              "F179b: the counters keep each context's frames, those pushed, those with the add taken off, the measured frames and those under 9 cm; the unarmed hip is counted apart from the sights");
        f179::Counters m;
        m.merge(c);
        m.merge(c);
        check(m.c[f179::kAdsStanding].frames == 200 && m.c[f179::kAdsStanding].pushed == 40 && m.c[f179::kAdsStanding].under9 == 20 && m.c[f179::kAdsStanding].pushMax == 0.04 &&
                  eq(m.c[f179::kAdsStanding].afterMin, 0.08, 1e-15) && m.c[f179::kHipUnarmed].frames == 100 && eq(m.c[f179::kAdsStanding].beforeSum, 2.0 * ads.beforeSum, 1e-12),
              "F179b: two windows add up");
        char line[1400];
        // F184: the acting format belongs to the hip armed alone (the sights measure only: F184's rig); the figures are the same ones, named for the context that acts
        const int len = f179::formatContext(line, sizeof(line), 10, "$wpn_m_shotgun_plasma_doublebarrel_name;", f179::kHipArmed, ads);
        check(len > 0 && std::strstr(line, "native on foot neck guard (F179b) 10 s: item $wpn_m_shotgun_plasma_doublebarrel_name;; hip armed 100 frames, the guard at 10.0 cm: pushed 20, the push mean 4.0 cm (largest 4.0); "
                                           "off the post-reach dial add 10 frames, mean 2.0 cm (largest 2.0); the eye ahead of the chest before the guard (as the pose and dials would put it) min +6.0 mean +17.2 cm, "
                                           "after it as measured (100 frames) min +8.0 mean +9.8, predicted after mean +18.0, under 9 cm 10") != nullptr,
              "F179b: the context's line names the tag, the item, the context, the frames, the pushes (mean over the pushed, largest), the add taken off, the eye ahead of the chest before and after, the prediction and the frames under 9 cm");
        // a window with no push still prints: the guard ran
        char quiet[1400];
        f179::formatContext(quiet, sizeof(quiet), 1, "x", f179::kHipArmed, hip);   // F180c: the unarmed hip's own line measures only (F180's rig); the same figures as an acting context's
        check(std::strstr(quiet, "hip armed 50 frames, the guard at 10.0 cm: pushed 0, the push mean 0.0 cm (largest 0.0); off the post-reach dial add 0 frames, mean 0.0 cm (largest 0.0); the eye ahead of the chest before the guard "
                                  "(as the pose and dials would put it) min +34.0 mean +34.0 cm, after it as measured (50 frames) min +33.0 mean +33.0, predicted after mean +34.0, under 9 cm 0") != nullptr,
              "F179b: a window with no push still prints its figures (the guard ran)");
        // the worst a window holds, and counts no window could hold
        f179::Ctx1 big;
        big.frames = 900; big.pushed = 900; big.offPostN = 900; big.beforeN = 900; big.predN = 900; big.afterN = 900; big.under9 = 900;
        big.pushSum = -50.0 * 900.0; big.pushMax = 50.0; big.offPostSum = 50.0 * 900.0; big.offPostMax = 50.0; big.beforeSum = -50.0 * 900.0; big.beforeMin = -50.0; big.predSum = -50.0 * 900.0;
        big.afterSum = -50.0 * 900.0; big.afterMin = -50.0;
        char worst[1400];
        const int worstLen = f179::formatContext(worst, sizeof(worst), 10, "$wpn_m_shotgun_plasma_doublebarrel_name_with_a_long_tail;", f179::kAdsCrouched, big);
        f179::Ctx1 absurd = big;
        absurd.frames = absurd.pushed = absurd.offPostN = absurd.beforeN = absurd.predN = absurd.afterN = absurd.under9 = 4000000000u;
        absurd.pushSum = absurd.beforeSum = absurd.afterSum = -1.0e12;
        char absurdLine[1200];
        const int absurdLen = f179::formatContext(absurdLine, sizeof(absurdLine), 4000000000u, "$wpn_m_shotgun_plasma_doublebarrel_name_with_a_long_tail;", f179::kAdsCrouched, absurd);
        f179::Ctx1 none;
        char noneLine[1400];
        const int noneLen = f179::formatContext(noneLine, sizeof(noneLine), 1, nullptr, 7, none);
        std::snprintf(msg, sizeof(msg), "F179b: the line is within the log's 1166 bytes (1200 less its stamp and truncation marker) with every figure at its worst (%d bytes typical, %d worst); a window with no frame, a context out of range and an item not named print zeros, never a number that is not one",
                      len, worstLen);
        check(worstLen > 0 && worstLen < 1166 && len < 1166 && absurdLen > 0 && std::strlen(absurdLine) < sizeof(absurdLine) && noneLen > 0 && std::strstr(noneLine, "item none") != nullptr &&
                  std::strstr(noneLine, "nan") == nullptr && std::strstr(noneLine, "inf") == nullptr && std::strstr(noneLine, "ADS crouched 0 frames") != nullptr,
              msg);
    }

    // ---- F97a's bins in the sights ---------------------------------------------------------------------------------------------------------------------------------------------------------
    {
        bool hipSame = true;
        for (int s = 0; s < f97::kStanceN; ++s) hipSame = hipSame && f97::binOf(s, false) == s && std::strcmp(f97::binName(s), f97::stanceName(s)) == 0 && !f97::binInSights(s);
        check(hipSame && f97::kBinN == 8 && f97::binOf(f97::kStanding, true) == 4 && f97::binOf(f97::kMoving, true) == 7 && f97::binInSights(4) && f97::binInSights(7) &&
                  std::strcmp(f97::binName(4), "standing in the sights") == 0 && std::strcmp(f97::binName(5), "crouched in the sights") == 0 &&
                  std::strcmp(f97::binName(6), "moving crouched in the sights") == 0 && std::strcmp(f97::binName(7), "moving in the sights") == 0 &&
                  f97::stanceBin(false, false) == f97::kStanding && f97::stanceBin(true, true) == f97::kCrouchMoving,
              "F97a (F179a): the line's bins are the four stances at the hip, as ever, and the same four in the sights (a call from the detach weight 0.05): standing in the sights is bin 4");
    }

    // ---- the source sites -----------------------------------------------------------------------------------------------------------------------------------------------------------------
    {
        const std::string cpp = slurp("src/d3d11/native_on_foot.cpp"), ini = slurp("edvr.ini"), logpin = slurp("src/d3d11/native_on_foot_logpin.h");
        check(!cpp.empty() && !ini.empty() && !logpin.empty(), "F179: the source pins read src/d3d11/native_on_foot.cpp, edvr.ini and native_on_foot_logpin.h (run the rig from the repository root)");
        const auto has = [&](const std::string& needle) { return cpp.find(needle) != std::string::npos; };
        const auto at = [&](const std::string& needle) { return cpp.find(needle); };
        // F179a: the body's neck target (T), right after F97a's, and the rest path's; the eye alignment raise is not touched by it
        check(has("at = nofoot::f179::withStandingRaise(at, N0[1], f97W, pc.f179StandRaise);") && has("pc.f179StandRaise = nofoot::f179::legsRaiseAskedM(legsRoomM, pc.postureW, pc.detachW);") &&
                  has("nofoot::f97::ArmedTarget restAt = nofoot::f179::withStandingRaise(") && has("T[1] = at.y;"),
              "F179a: the standing raise is made on the body's neck target T (right after F97a's armed target, before T[1] is set) and on the rest path's physical target (F193: restAt, which the camera rule may replace), from the eye as drawn at the hip less the key");
        const size_t armed = at("nofoot::f97::ArmedTarget at = nofoot::f97::armedNeckTarget("), wrap = at("at = nofoot::f179::withStandingRaise(at, N0[1]"), setT = at("T[1] = at.y;"),
                     f95 = at("F95a/F98c: the raise's target only from a settled stance");
        check(armed != std::string::npos && armed < wrap && wrap < setT && setT < f95,
              "F179a: the raise is added to T before the settle that publishes the camera's raise reads it: F138's and F172's eye alignment raise (f94TargetPublish) is not the variable moved");
        check(!(has("f94TargetPublish(pc.f179") || has("f94TargetPublish(nofoot::f179") || has("g_f138HeldM = pc.f179") || has("g_f138HeldM += pc.f179")),
              "F179a: nothing of F179a reaches the eye alignment raise F138 and F172 publish");
        // F179b: the guard on the target (legacy and rest path), the post-reach add, the formed neck, the prediction, the stats and the bias
        check(has("pc.f179PushPre = nofoot::f179::guardTarget(g179, N0, pc.f179Bias, T, pc.f179DistM, &pc.f179Bounded);") && has("if (g179.ok && !pc.f179Measure) pc.f179PushPre = nofoot::f179::guardTarget(g179, N0, pc.f179Bias, physicalTarget, pc.f179DistM, &pc.f179Bounded,") &&
                  has("g179 = nofoot::f179::makeGeometry(pc.f97EyeDrawnOk, pc.f97EyeDrawn, pc.hEye[0], pc.hEye[1], chestOk, chest0);") && has("const uint32_t chestJ = nofoot::kIkSpine[2];"),
              "F179b: the guard reads the eye as drawn, the chest 24 as animated (kIkSpine[2]) and F97a's heading, and pushes the target back before the reach rule on the legacy target and the rest path's");
        const size_t guardPre = at("pc.f179PushPre = nofoot::f179::guardTarget(g179, N0, pc.f179Bias, T, pc.f179DistM, &pc.f179Bounded);"), formMove = at("mv = nofoot::pinMove(N0, T);"), reachRule = at("const bool active = nofoot::reachLimitDeltaFeasibleOnly(v, R, have, candidate, full, &feasible);"),
                     postTrim = at("nofoot::f179::trimPost(f168Post, g179.h, deficit)"), addPost = at("nofoot::f168::addAfterReach(mv.th, pc.reach, f168Post);"), addRes = at("nofoot::f168::addAfterReach(mv.th, pc.reach, guardRes);");
        check(guardPre != std::string::npos && guardPre < formMove && formMove < reachRule && reachRule < postTrim && postTrim < addPost && addPost < addRes,
              "F179b: the order in the pin is the guard on the target, the move formed, the reach rule, the post-reach add trimmed, the add made, the residual taken off last");
        check(has("pc.f168Post[i] *= trim.scale;") && has("pc.f178FwdPost[i] *= trim.scale;") && has("pc.f179OffPost += trim.takenM;") && has("nofoot::f179::pushBack(guardRes, g179.h, trim.residualM);") &&
                  has("const double reachOnly = legacyReach[i] - f168Post[i] - guardRes[i];") && has("const double deficit = nofoot::f179::deficitBoundedM(g179.aheadWith(formed.th) + pc.f179Bias, pc.f179DistM, &pc.f179Bounded,"),
              "F179b: the add taken off is scaled out of the crouched and the standing forward dial's post vectors (their 'as written' figures see it), the residual is its own vector, and the rest path looks again at the neck it forms");
        check(has("pc.f179PredRaw = g179.aheadWith(mv.th);") && has("pc.f179Before = pc.f179PredRaw - (pc.f179PushPre + pc.f179OffPost + pc.f179PushRes);") && has("st.f179.addGuard(gs);") &&
                  has("st.f179.addMeasured(pc.f179Ctx, pc.f97ChestFwd, nofoot::f179::heldBelowM(pc.f179DistM));") && has("g_f179Bias.m = nofoot::f179::nextBias(g_f179Bias.m, pinCall.f97ChestFwd - pinCall.f179PredRaw);") &&
                  has("nofoot::f179::formatContext(line179, sizeof(line179), ws, w.item, cx, k, nofoot::f179::clampGuardCm(static_cast<double>(w.guardKey)));") && has("if (!k.frames) continue;"),
              "F179b: the prediction after the guard and the figure before it are taken from the move as formed, the guard's call and the measured figure are filed, the error feeds the filter, and each context with frames is printed");
        check(logpin.find("d.f179.merge(s.f179);") != std::string::npos && logpin.find("d.ssSum += s.ssSum;") != std::string::npos && logpin.find("d.srSum += s.srSum;") != std::string::npos,
              "F179: logpin merges the neck guard's counters and the standing share's sums into the window");
        check(has("nofoot::f97::binOf(nofoot::f97::stanceBin(nofoot::pinCrouchedWc(pc.wc), pc.f95Moving), !nofoot::f159::atHip(pc.detachW))") && has("nofoot::f97::binName(sb)") &&
                  has("; the standing share (F179a) mean %.2f, its raise asked %+.1f cm") && has("f.ssSum += pc.f179StandW;") && has("} f97[nofoot::f97::kBinN];"),
              "F179a: the F97a line's bins are the stance and whether in the sights (the detach weight from 0.05), a standing call in the sights is told from a hip call by its bin's name, and the sights' bins say the standing share and its raise");
        // no new keys
        check(ini.find("f179") == std::string::npos && ini.find("F179") == std::string::npos && ini.find("stand_ads_raise") == std::string::npos ,
              "F179: both are behaviour: edvr.ini has no new key");
    }
}

}  // namespace f179test
