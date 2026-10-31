// F193 checks (native on-foot VR), run193 below. Header only, run by native_frame_test.
//
// F193 (native_on_foot_f193.h, native_on_foot.cpp; behaviour, no key since the pilot flew it on and said to retire the A/B key): in the sights the body's neck target is the game's camera eye less the
// eye height key, both stances, as the hip gives it; a lowering is allowed, a raise stays held to the legs' room. The pilot: crouching or jumping with the sights up, the arms lag the body; at
// the hip they stay in place. The flight edvr_gfx_20261031_080600: the steady states (standing in the sights the neck 1.400 against the camera at about 1.54; crouched 0.82 against 0.96, both
// the camera less 14 to 15 cm), a jump in the sights (the camera flat, the body's vertical against the animated neck climbing +7 to +44 cm), a crouch entered in the sights (the body snapped 48 cm
// in 3 frames, 70 to 90 ms after the camera stopped). The rig runs the rule against the old one (F97a's armedNeckTarget and F179a's legs' room raise, the functions the pin used) on those, the
// continuity from the hip, the lowering and the legs' cap, the stability stop and its fall back to the old rule, a toy loop (a rule that reads the drawn eye runs away; this one does not), the lines and the source sites.
#pragma once

#include "../../src/d3d11/native_on_foot_core.h"
#include "../../src/d3d11/native_on_foot_f163.h"
#include "../../src/d3d11/native_on_foot_f171c.h"
#include "../../src/d3d11/native_on_foot_f179.h"
#include "../../src/d3d11/native_on_foot_f193.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

namespace f193test {

namespace nf = edvr::nofoot;
namespace f97 = edvr::nofoot::f97;
namespace f179 = edvr::nofoot::f179;
namespace f193 = edvr::nofoot::f193;

inline std::string slurp(const char* path) {
    std::ifstream f(path, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// One call's neck target as the pin forms it, for both rules (the cpp's block, with the toy inputs). The old rule: F97a's armedNeckTarget at the call's detach weight, with F179a's standing raise asked
// of the legs' room. The hip's demand: the same at detach 0. The new: F193's cameraTarget when the rule is wanted.
struct Inputs {
    double n0y = 1.268, eyeLevelY = 1.54, h = 0.14, legsRoom = 0.132, postureW = 0.0, detachW = 1.0, armedW = 1.0, carriedRaise = 0.0;
    double t93Y() const { return n0y; }   // F93's target at the sights is the animated neck (pinDetachTarget with the detach weight 1)
};
inline f97::ArmedTarget oldRule(const Inputs& in) {
    // the cpp passes T[1] after the detach (n0y at detach 1, the hip's t93 blended out by the weight) and t93BeforeDetachY + the view shift as the hip's; the toy's hip target is the eye-based one
    const double t93Hip = in.eyeLevelY - nf::kPinRestUpM;
    const double t93 = t93Hip + (in.n0y - t93Hip) * in.detachW;
    f97::ArmedTarget at = f97::armedNeckTarget(in.n0y, t93, in.eyeLevelY + in.carriedRaise, in.h, in.detachW, in.armedW, in.postureW, t93Hip);
    return f179::withStandingRaise(at, in.n0y, in.armedW, f179::legsRaiseAskedM(in.legsRoom, in.postureW, in.detachW));
}
inline f193::Result newRule(const Inputs& in) {
    const double t93Hip = in.eyeLevelY - nf::kPinRestUpM;
    const f97::ArmedTarget hip = f97::armedNeckTarget(in.n0y, t93Hip, in.eyeLevelY + in.carriedRaise, in.h, 0.0, in.armedW, in.postureW, t93Hip);
    return f193::cameraTarget(hip, in.n0y, in.armedW, in.eyeLevelY, in.h, in.legsRoom, in.detachW);
}
// What the pin's block does: the old rule, replaced by the new when it is wanted.
inline double callerY(bool stopped, const Inputs& in) {
    f97::ArmedTarget at = oldRule(in);
    if (f193::wanted(stopped, in.detachW, in.armedW)) {
        const f193::Result r = newRule(in);
        if (r.ok) at = r.at;
    }
    return at.y;
}

template <class Check>
inline void run193(Check&& check) {
    const auto eq = [](double a, double b, double tol) { return std::fabs(a - b) <= tol; };
    char buf[800];

    // ---- 1. when the rule runs ------------------------------------------------------------------------------------------------------------------------------------------------------
    {
        check(f193::wanted(false, 1.0, 1.0) && f193::wanted(false, 0.02, 0.3), "F193: the rule runs with no stop in this press, the sights up (any detach weight above 0) and a weapon drawn");
        check(!f193::wanted(true, 1.0, 1.0) && !f193::wanted(false, 0.0, 1.0) && !f193::wanted(false, 1.0, 0.0) && !f193::wanted(false, std::nan(""), 1.0),
              "F193: a stop in this press, the hip (detach weight 0) and nothing drawn (weight 0) leave the old rule");
        check(f193::sourceOf(true, 1.0) == f193::kSrcCamera && f193::sourceOf(false, 1.0) == f193::kSrcLegs && f193::sourceOf(false, 0.0) == f193::kSrcHip &&
                  std::strcmp(f193::sourceName(f193::kSrcCamera), "camera") == 0 && std::strcmp(f193::sourceName(f193::kSrcLegs), "legs") == 0 && std::strcmp(f193::sourceName(f193::kSrcHip), "hip") == 0,
              "F193: the F85a line's source is the camera's rule, the legs' room rule in the sights, or the hip");
    }

    // ---- 2. the steady states: the same as the old rule within 1 cm, standing and crouched ---------------------------------------------------------------------------------------------
    {
        Inputs st;   // standing in the sights: the neck 1.400 (the animated neck 1.268 and the legs' room 0.132), the camera at 1.54, the key 14 cm
        const f97::ArmedTarget o = oldRule(st);
        const f193::Result n = newRule(st);
        check(n.ok && eq(o.y, 1.400, 1e-12) && eq(n.at.y, 1.400, 1e-12) && std::fabs(o.y - n.at.y) < 0.01 && !n.capped && eq(n.eyeAskM, 0.132, 1e-12),
              "F193: standing in the sights (camera 1.54, key 14 cm, animated neck 1.268, the legs' room 13.2 cm) the new neck is 1.400 as the old rule's: within 1 cm");
        Inputs cr;   // crouched in the sights: the camera at 0.96, the animated neck 0.86, the crouched weight 1
        cr.n0y = 0.86;
        cr.eyeLevelY = 0.96;
        cr.postureW = 1.0;
        cr.legsRoom = 0.02;
        const f97::ArmedTarget oc = oldRule(cr);
        const f193::Result nc = newRule(cr);
        check(nc.ok && eq(oc.y, 0.82, 1e-12) && eq(nc.at.y, 0.82, 1e-12) && std::fabs(oc.y - nc.at.y) < 0.01 && eq(nc.eyeAskM, -0.04, 1e-12),
              "F193: crouched in the sights (camera 0.96, animated neck 0.86) the new neck is 0.82 as the old rule's: within 1 cm, a lowering of 4 cm");
        // the crossfade no longer moves the body's height: the posture weight changes nothing in the sights
        bool same = true;
        for (int i = 0; i <= 10; ++i) {
            Inputs m = st;
            m.postureW = i / 10.0;
            same = same && newRule(m).at.y == n.at.y;
        }
        check(same, "F193: in the sights the posture weight (F163's crossfade) moves the neck's height not at all: the same y at every weight from standing to crouched");
        // the two stances differ only by where the camera is: the neck is always one key under the camera, 14 cm, when the legs allow
        check(eq(n.at.y - st.eyeLevelY, -0.14, 1e-12) && eq(nc.at.y - cr.eyeLevelY, -0.14, 1e-12), "F193: the neck stands one key (14 cm) under the camera's eye in both stances, as it does at the hip");
        // the figures the cpp stores: the ask before the cap and after it
        check(eq(n.askedM, 0.132, 1e-12) && eq(nc.askedM, -0.04, 1e-12), "F193: the ask is said before the cap (the camera less the key over the animated neck) and after it");
    }

    // ---- 3. a jump with a flat camera: the neck does not rise ----------------------------------------------------------------------------------------------------------------------------------
    {
        // the flight's jump in the sights (event 46): the camera flat; the animation tucks the legs, the animated neck falls and the legs' room grows (the F97a line: raised +28.3 cm, capped by the legs 44)
        bool newFlat = true, oldRose = false, askGrew = true, cappedNever = true;
        double lastAsk = -1.0;
        double oldMax = -1e9;
        for (int i = 0; i <= 20; ++i) {
            const double u = i / 20.0;
            Inputs in;
            in.n0y = 1.268 - 0.27 * u;
            in.legsRoom = 0.132 + 0.31 * u;   // 13.2 cm to 44 cm
            const f193::Result n = newRule(in);
            const f97::ArmedTarget o = oldRule(in);
            newFlat = newFlat && n.ok && eq(n.at.y, 1.400, 1e-12);
            oldMax = std::fmax(oldMax, o.y);
            askGrew = askGrew && n.askedM >= lastAsk - 1e-12;
            lastAsk = n.askedM;
            cappedNever = cappedNever && !n.capped;
        }
        oldRose = oldMax > 1.400 + 0.03;
        check(newFlat && oldRose && askGrew && cappedNever,
              "F193: a jump with a flat camera: the new neck stays at 1.400 (the camera less the key) through the whole tuck while the old rule's rises (past 1.43 as the legs' room grows 13 to 44 cm)");
        // the raise over the animated neck is what the legs give only when the camera needs it: the legs' room too small caps the raise (a foot must not lift)
        Inputs tight;
        tight.n0y = 0.90;
        tight.legsRoom = 0.30;   // the camera asks 1.40 - 0.90 = +50 cm
        const f193::Result c = newRule(tight);
        check(c.ok && c.capped && eq(c.askedM, 0.30, 1e-12) && eq(c.eyeAskM, 0.50, 1e-12) && eq(c.at.y, 1.20, 1e-12),
              "F193: a raise stays held to the legs' room: asked +50 cm over the animated neck, the room 30 cm, the neck 1.20 and the cap said");
        // no legs read: no raise at all, a lowering still
        Inputs noLegs;
        noLegs.legsRoom = std::nan("");
        const f193::Result nl = newRule(noLegs);
        Inputs noLegsLow = noLegs;
        noLegsLow.n0y = 1.5;
        const f193::Result nll = newRule(noLegsLow);
        check(nl.ok && nl.capped && eq(nl.at.y, 1.268, 1e-12) && nll.ok && !nll.capped && eq(nll.at.y, 1.40, 1e-12),
              "F193: with no legs read the raise is 0 (the neck stays at the animated one) and a lowering is made as it is");
    }

    // ---- 4. a crouch entered in the sights: the neck follows the camera frame by frame ------------------------------------------------------------------------------------------------------
    {
        // the flight's event 44: the camera falls 58 cm (1.54 to 0.96) in 3 frames; the animated neck follows only as the animation crouches, the posture weight later still (F163's smoothstep on the neck's drop)
        const double cam[] = {1.54, 1.34, 1.14, 0.96, 0.96, 0.96, 0.96, 0.96, 0.96, 0.96};
        const double neck[] = {1.268, 1.268, 1.268, 1.26, 1.20, 1.10, 1.00, 0.92, 0.88, 0.86};
        bool follows = true;
        double worstOldGap = 0.0;
        for (int i = 0; i < 10; ++i) {
            Inputs in;
            in.n0y = neck[i];
            in.eyeLevelY = cam[i];
            in.postureW = nf::f163::postureWeight(1.543 - neck[i]);
            in.legsRoom = i < 3 ? 0.132 : 0.02;
            const f193::Result n = newRule(in);
            const f97::ArmedTarget o = oldRule(in);
            follows = follows && n.ok && eq(n.at.y, cam[i] - 0.14, 1e-12);
            worstOldGap = std::fmax(worstOldGap, std::fabs(o.y - (cam[i] - 0.14)));
        }
        check(follows && worstOldGap > 0.25,
              "F193: a crouch entered in the sights: the new neck is the camera less 14 cm on every frame of the entry (1.40, 1.20, 1.00, 0.82), the old rule's lagging it by over 25 cm at its worst");
    }

    // ---- 5. lowering is allowed, and the press and release are continuous from the hip -----------------------------------------------------------------------------------------------------
    {
        Inputs low;
        low.n0y = 1.50;
        low.eyeLevelY = 1.54;   // the camera less the key is 1.40: the animated neck stands 10 cm too high
        const f193::Result l = newRule(low);
        check(l.ok && eq(l.eyeAskM, -0.10, 1e-12) && eq(l.askedM, -0.10, 1e-12) && eq(l.at.y, 1.40, 1e-12) && !l.capped && l.at.lowerEye < 0.0,
              "F193: a lowering is allowed: an animated neck 10 cm over the camera's less the key is brought down to it, and the legs' cap does not come into it");
        // the hip's demand is the weight's other end: at detach 0 the result is the hip's, bit for bit; at 1 the camera's; between, between
        Inputs hipIn;
        hipIn.detachW = 0.0;
        const double t93Hip = hipIn.eyeLevelY - nf::kPinRestUpM;
        const f97::ArmedTarget hip = f97::armedNeckTarget(hipIn.n0y, t93Hip, hipIn.eyeLevelY, hipIn.h, 0.0, hipIn.armedW, hipIn.postureW, t93Hip);
        const f193::Result at0 = f193::cameraTarget(hip, hipIn.n0y, hipIn.armedW, hipIn.eyeLevelY, hipIn.h, hipIn.legsRoom, 0.0);
        check(at0.ok && std::memcmp(&at0.at.y, &hip.y, sizeof(double)) == 0 && std::memcmp(&at0.at.asked, &hip.asked, sizeof(double)) == 0,
              "F193: at detach weight 0 the blend is the hip's demand and its neck, bit for bit");
        bool monotone = true, ends = true;
        double last = hip.y;
        const double camY = 1.40;
        for (int i = 0; i <= 100; ++i) {
            const f193::Result r = f193::cameraTarget(hip, hipIn.n0y, 1.0, hipIn.eyeLevelY, hipIn.h, hipIn.legsRoom, i / 100.0);
            const double y = r.at.y;
            monotone = monotone && ((hip.y <= camY) ? y >= last - 1e-12 : y <= last + 1e-12);
            last = y;
            if (i == 100) ends = ends && eq(y, camY, 1e-12);
        }
        check(monotone && ends, "F193: from the hip to the sights in a hundred steps of the detach weight the neck moves steadily from the hip's height to the camera's less the key, and arrives at it");
        // the armed weight scales the move as the old rule's does
        const f193::Result half = f193::cameraTarget(hip, 1.268, 0.5, 1.54, 0.14, 0.132, 1.0);
        check(eq(half.at.y, 1.268 + 0.5 * 0.132, 1e-12), "F193: the armed weight (F88's) scales the move: at 0.5 half of it over the animated neck");
        check(!f193::cameraTarget(hip, 1.268, 0.0, 1.54, 0.14, 0.132, 1.0).ok && !f193::cameraTarget(hip, std::nan(""), 1.0, 1.54, 0.14, 0.132, 1.0).ok &&
                  !f193::cameraTarget(hip, 1.268, 1.0, std::nan(""), 0.14, 0.132, 1.0).ok && !f193::cameraTarget(hip, 1.268, 1.0, 1.54, std::nan(""), 0.132, 1.0).ok,
              "F193: nothing drawn or an input that is not a number: not ok, the caller keeps the old rule's target");
    }

    // ---- 6. a stop in the press, or the hip, is the old rule, bit for bit ---------------------------------------------------------------------------------------------------------------------
    {
        bool same = true, differ = false;
        for (int i = 0; i < 40; ++i) {
            Inputs in;
            in.n0y = 1.268 - 0.3 * std::fabs(std::sin(i * 0.3));
            in.eyeLevelY = 1.54 - 0.4 * std::fabs(std::sin(i * 0.17));
            in.legsRoom = 0.05 + 0.3 * std::fabs(std::sin(i * 0.23));
            in.postureW = (i % 5) / 4.0;
            in.detachW = (i % 7) / 6.0;
            const double stopped = callerY(true, in), old = oldRule(in).y, on = callerY(false, in);
            same = same && std::memcmp(&stopped, &old, sizeof(double)) == 0;
            if (in.detachW > 0.0 && std::fabs(on - old) > 1e-4) differ = true;
            if (in.detachW == 0.0) same = same && std::memcmp(&on, &old, sizeof(double)) == 0;
        }
        check(same && differ, "F193: with a stop in this press the neck is the old rule's bit for bit (40 varied frames); at the hip even with no stop; with no stop the rule does change the sights");
    }

    // ---- 7. the stability stop -----------------------------------------------------------------------------------------------------------------------------------------------------------------
    {
        f193::StopGuard g;
        // the target moves 4 cm a frame, the camera 2 mm: four frames are not a stop, the fifth is
        double target = 1.40;
        bool early = false;
        f193::StopGuard::Out o = g.step(true, true, target, 1.54);   // the first frame has no step
        check(!o.stoppedNow && std::isnan(o.targetStep), "F193: the first frame of a press has no step to judge");
        for (int i = 1; i <= 4; ++i) {
            target += 0.04;
            o = g.step(true, true, target, 1.54 + 0.002 * i);
            early = early || o.stoppedNow || g.stopped;
        }
        check(!early && g.run == 4, "F193: four frames of the target moving 4 cm with the camera steady (2 mm) are not a stop");
        target += 0.04;
        o = g.step(true, true, target, 1.54 + 0.002 * 5);
        check(o.stoppedNow && g.stopped && g.stops == 1 && eq(o.targetStep, 0.04, 1e-12) && o.cameraStep < 0.0021,
              "F193: the fifth frame in a row (the target over 3 cm, the camera under 1 cm) stops the rule: said once, latched, counted");
        o = g.step(true, false, target + 0.04, 1.55);   // the old rule runs now: the guard keeps its latch
        check(!o.stoppedNow && g.stopped && g.stops == 1, "F193: the stop holds for the rest of the press (the old rule runs; nothing is said again)");
        o = g.step(true, false, target, 1.55);
        check(g.stopped && g.stops == 1, "F193: and again the next frame");
        g.step(false, false, 1.0, 1.0);   // the sights come down: the press is over
        check(!g.stopped && g.stops == 1 && g.run == 0, "F193: the press's end clears the latch and keeps the session's count");
        // the next press starts clean and can stop again
        bool second = false;
        double t2 = 1.40;
        g.step(true, true, t2, 1.54);
        for (int i = 0; i < 5; ++i) {
            t2 += 0.05;
            second = g.step(true, true, t2, 1.54).stoppedNow || second;
        }
        check(second && g.stops == 2 && g.stopped, "F193: the next press runs the rule again and can stop again (two stops this session)");
        // a camera that moves 1 cm or more, or a target step of 3 cm or less, resets the run
        f193::StopGuard h;
        h.step(true, true, 1.40, 1.54);
        double t = 1.40;
        for (int i = 0; i < 4; ++i) {
            t += 0.05;
            h.step(true, true, t, 1.54);
        }
        t += 0.05;
        o = h.step(true, true, t, 1.54 + 0.012);   // the camera moved 1.2 cm
        check(!o.stoppedNow && h.run == 0, "F193: a camera that moves 1 cm or more in a frame resets the run (the target is following it)");
        for (int i = 0; i < 4; ++i) {
            t += 0.05;
            h.step(true, true, t, 1.54 + 0.012);
        }
        t += 0.02;
        o = h.step(true, true, t, 1.54 + 0.012);   // a 2 cm step is under the limit
        check(!o.stoppedNow && h.run == 0, "F193: a target step of 3 cm or less resets the run");
        // a legitimate fast crouch: the target and the camera fall together, 5 cm a frame for twenty frames: never a stop
        f193::StopGuard c;
        double cam = 1.54, tg = 1.40;
        bool any = false;
        c.step(true, true, tg, cam);
        for (int i = 0; i < 20; ++i) {
            cam -= 0.05;
            tg -= 0.05;
            any = c.step(true, true, tg, cam).stoppedNow || any;
        }
        check(!any && c.stops == 0, "F193: a crouch the rule follows (the target and the camera both falling 5 cm a frame for twenty frames) is never a stop");
        // the gap: a frame where the camera rule did not run (the old rule's) resets the run
        f193::StopGuard d;
        d.step(true, true, 1.40, 1.54);
        double td = 1.40;
        for (int i = 0; i < 4; ++i) {
            td += 0.05;
            d.step(true, true, td, 1.54);
        }
        td += 0.05;
        d.step(true, false, td, 1.54);
        check(d.run == 0 && !d.stopped, "F193: a frame the camera rule did not make resets the run");
        // a target that is not a number is no step
        f193::StopGuard e;
        e.step(true, true, 1.40, 1.54);
        o = e.step(true, true, std::nan(""), 1.54);
        check(!o.stoppedNow && e.run == 0, "F193: a target or a camera that is not a number counts no step");
    }

    // ---- 8. a toy loop: the drawn eye rides on the written body; the rule reads the game's camera ---------------------------------------------------------------------------------------------
    {
        // the camera the game gives is the character's point of view (steady here) plus what EDVR adds: the raise (F94's, falling in the sights, F160) and the sight lock. The drawn eye rides on the
        // written neck (the lock puts it on the sight line which is fixed to the body the camera rides on): drawn = neck + 0.14 + 0. A rule that reads the drawn eye is a pure integrator of its own
        // error and runs away at the legs' cap; the rule that reads the game's camera (the drawn one less what EDVR added, the pose carrying the raise at k) settles at the camera less the key.
        const double pov = 1.54, key = 0.14, k = 0.95;
        double yDrawnRule = 1.40, yCamRule = 1.40, raise = 0.10;
        double worstCamErr = 0.0;
        for (int i = 0; i < 60; ++i) {
            // the drawn eye rides on the written neck (plus the carried raise): reading it, the neck target is the drawn eye less the key, which is where the neck already is: nothing pulls it back
            const double drawn = yDrawnRule + key + 0.004;   // the lock's residual error rides on it: 4 mm up a frame
            yDrawnRule = drawn - key;
            // the game's camera: the point of view; the pose carried the raise at k, the pin took out all of it: the (1 - k) share of the raise is what stays in E
            const double e = pov - (1.0 - k) * raise;
            const f97::ArmedTarget hip = f97::armedNeckTarget(1.268, pov - nf::kPinRestUpM, pov, key, 0.0, 1.0, 0.0, pov - nf::kPinRestUpM);
            const f193::Result r = f193::cameraTarget(hip, 1.268, 1.0, e, key, 0.5, 1.0);
            yCamRule = r.at.y;
            raise *= 0.7;   // F160: the raise falls in the sights
            if (i >= 20) worstCamErr = std::fmax(worstCamErr, std::fabs(yCamRule - (pov - key)));
        }
        check(yDrawnRule > 1.40 + 0.2 && worstCamErr < 1e-3 && eq(yCamRule, pov - key, 1e-6),
              "F193: the toy loop: a rule that reads the drawn eye (which rides on the written body) climbs 4 mm a frame without end; the camera rule, with the pose carrying the raise at 0.95, settles on the camera less the key");
    }

    // ---- 9. the window and its lines -----------------------------------------------------------------------------------------------------------------------------------------------------------
    {
        f193::Window w;
        check(!w.any(), "F193: an empty window says nothing");
        for (int i = 0; i < 90; ++i) {
            f193::Sample s;
            s.camera = true;
            s.neckMinusCamM = -0.14 + (i % 3 == 0 ? 0.001 : -0.001);
            s.camStepM = i ? 0.002 : std::nan("");
            s.tgtStepM = i ? 0.002 : std::nan("");
            w.add(s);
        }
        f193::Sample st;
        st.camera = false;
        st.stoppedNow = true;
        st.neckMinusCamM = -0.05;
        st.camStepM = 0.001;
        st.tgtStepM = 0.04;
        w.add(st);
        check(w.any() && w.calls == 91 && w.camera == 90 && w.stops == 1 && w.offN == 91 && w.steps == 90 && eq(w.offMax, -0.05, 1e-12) && eq(w.tgtStepMax, 0.04, 1e-12) && eq(w.camStepMax, 0.002, 1e-12),
              "F193: the window counts the calls in the sights, those on the camera rule, the stops, the offset's extremes and the steps");
        const int len = f193::formatLine(buf, sizeof(buf), 1, w, 14.0, 2);
        check(len > 0 && len < 760 && std::strstr(buf, "native on foot ADS body height (F193) 1 s: rule camera (the camera's in 90 of 91 calls in the sights);") != nullptr &&
                  std::strstr(buf, "TEMPORARY") == nullptr && std::strstr(buf, "body_follows_camera") == nullptr &&
                  std::strstr(buf, "neck target - camera eye mean -13.9 min -14.1 max -5.0 cm") != nullptr && std::strstr(buf, "(the camera rule reads minus the eye height key, 14.0 cm)") != nullptr &&
                  std::strstr(buf, "stops 1 (session 2)") != nullptr,
              "F193: the 1 s line says the rule (camera), the calls on it, the eye height key, the neck target less the camera's eye (mean, min, max), the steps and the stops, in under 760 bytes");
        f193::Window legs;
        for (int i = 0; i < 90; ++i) {
            f193::Sample s;
            s.neckMinusCamM = 0.02;
            legs.add(s);
        }
        f193::formatLine(buf, sizeof(buf), 1, legs, 14.0, 0);
        check(std::strstr(buf, "rule legs (the camera's in 0 of 90 calls in the sights);") != nullptr && std::strstr(buf, "body_follows_camera") == nullptr,
              "F193: after a stop (the old rule for the rest of the press) the line says the legs' rule ran, in no call on the camera");
        f193::formatStop(buf, sizeof(buf), 0.045, 0.002, 1);
        check(std::strstr(buf, "native on foot ADS body height stop (F193): the neck target") != nullptr && std::strstr(buf, "TEMPORARY") == nullptr && std::strstr(buf, "over 3 cm a frame for 5 frames") != nullptr && std::strstr(buf, "under 1 cm") != nullptr &&
                  std::strstr(buf, "target 4.5 cm, camera 0.20 cm") != nullptr && std::strstr(buf, "the legs' room rule holds for the rest of this press") != nullptr,
              "F193: the stop's line gives the limits, the last frame's steps and that the old rule holds for the rest of the press");
        // the mailbox: written by the pin hook, taken by the frame thread, cleared between
        nf::f171c::Mailbox<f193::Window> box;
        f193::Sample s;
        s.camera = true;
        s.neckMinusCamM = -0.14;
        box.write([&](f193::Window& win) noexcept { win.add(s); });
        f193::Window got;
        check(box.take(&got) && got.calls == 1 && got.camera == 1, "F193: the window goes through the mailbox the other 1 s lines use");
        f193::Window again;
        check(box.take(&again) && !again.any(), "F193: a second take with no write in between reads an empty window");
    }

    // ---- 10. the source sites ------------------------------------------------------------------------------------------------------------------------------------------------------------------
    {
        const std::string cpp = slurp("src/d3d11/native_on_foot.cpp"), ini = slurp("edvr.ini");
        const auto has = [&](const std::string& needle) { return cpp.find(needle) != std::string::npos; };
        const auto at = [&](const std::string& needle) { return cpp.find(needle); };
        // The A/B key is retired (the pilot flew it on and said so): nothing reads it, no flag holds it, edvr.ini has neither its entry nor its menu row. The needles are the key's and the flag's
        // stems, cut short on purpose, so that the tree holds no spelling of the retired names outside the docs.
        check(cpp.find("native_on_foot_ads_body_follows") == std::string::npos && cpp.find("g_adsBodyFollows") == std::string::npos && cpp.find("followOn") == std::string::npos &&
                  ini.find("native_on_foot_ads_body_follows") == std::string::npos && ini.find("ADS: body follows the camera") == std::string::npos && ini.find("(F193, A/B)") == std::string::npos &&
                  ini.find("\nnative_on_foot_pistol_sprint_swing_phase = 0.5\n") != std::string::npos,
              "F193: the A/B key is gone: nothing reads it, no flag holds it, edvr.ini has neither its entry nor its menu row, and the camera rule is the behaviour");
        // the rule's site: the camera is pc.eyeYSelected (E), never the drawn eye or the carried raise added back
        check(has("if (nofoot::f193::wanted(g_f193Stop.stopped, pc.detachW, f97W)) {") &&
                  has("f193r = nofoot::f193::cameraTarget(hipAt, N0[1], f97W, pc.eyeYSelected, f97H, legsRoomM, pc.detachW);") && has("if (f193r.ok) at = f193r.at;") &&
                  has("at = nofoot::f179::withStandingRaise(at, N0[1], f97W, pc.f179StandRaise);"),
              "F193: the rule replaces the old target only when wanted and ok (the old rule is computed first and stands otherwise), and reads the pin's E (pc.eyeYSelected)");
        const size_t ruleAt = at("f193r = nofoot::f193::cameraTarget(hipAt, N0[1], f97W, pc.eyeYSelected, f97H, legsRoomM, pc.detachW);");
        const size_t ruleEnd = at("pc.f193Camera = f193r.ok;");
        const std::string ruleBlock = ruleAt != std::string::npos && ruleEnd != std::string::npos && ruleAt < ruleEnd ? cpp.substr(ruleAt, ruleEnd - ruleAt) : std::string();
        check(!ruleBlock.empty() && ruleBlock.find("f97EyeDrawn") == std::string::npos && ruleBlock.find("f97EyeOverE") == std::string::npos && ruleBlock.find("pc.f94") == std::string::npos &&
                  ruleBlock.find("f179StandRaise") == std::string::npos && ruleBlock.find("pc.nPin") == std::string::npos && ruleBlock.find("pc.delta") == std::string::npos &&
                  ruleBlock.find("g_pfz") == std::string::npos && ruleBlock.find("resFinal") == std::string::npos && ruleBlock.find("mv.") == std::string::npos,
              "F193: between the rule's call and its record nothing reads the drawn eye (f97EyeDrawn), the carried raise (f97EyeOverE, f94*), the old standing raise, or anything the pin writes (the neck as pinned or moved, the pose buffers)");
        check(has("const nofoot::f193::StopGuard::Out stop = g_f193Stop.step(pc.detachW > 0.0, f193r.ok, f193r.ok ? f193r.at.y : at.y, pc.eyeYSelected);") && has("if (stop.stoppedNow) {") &&
                  has("f193r = nofoot::f193::Result{};   // this frame is written by the old rule as well") && has("nofoot::f193::formatStop(stopLine, sizeof(stopLine), stop.targetStep, stop.cameraStep, g_f193Stop.stops);") &&
                  has("g_f193Box.write([&](nofoot::f193::Window& win) noexcept { win.add(s193); });") && has("nofoot::f193::StopGuard g_f193Stop;"),
              "F193: the stop is stepped once a fresh frame with the target the call made and the pin's E, the frame that trips it is written by the old rule, and the sample goes to the window");
        check(has("if (pc.f193Camera) {   // F193: the camera rule on this target too") && has("const nofoot::f193::Result restR = nofoot::f193::cameraTarget(restHipAt, N0[1], f97W, pc.eyeYSelected, f97H, legsRoomM, pc.detachW);") &&
                  has("physicalTarget[1] = restAt.y;"),
              "F193: the rest target (the neck-yaw path) takes the same rule on the same decision, from its own hip demand");
        check(has("nofoot::f193::formatLine(bodyLine, sizeof(bodyLine), 1, bw, static_cast<double>(g_f94EyeHeightCm.load(std::memory_order_relaxed)), g_f193Stops.load(std::memory_order_relaxed));") &&
                  has("if (g_f193Box.take(&bw) && bw.any()) {") &&
                  has("g_f193Box.clearPending.store(true, std::memory_order_relaxed);"),
              "F193: the 1 s line is taken from the window with the other pin lines and cleared at an edge");
        check(has("fp.n0y = static_cast<float>(pc.N0[1]);") && has("fp.legsRoomM = static_cast<float>(pc.f193LegsRoomM);") && has("fp.rawEyeY = static_cast<float>(pc.rawEyeY);") && has("fp.source = pc.f193Source;") &&
                  has("sf.n0y = pin.n0y;") && has("ADS neck (F193): animated neck y %.3f m, F163 posture w %.2f, legs' room %s, standing raise asked %+.1f cm, camera eye y %.3f m, neck target y %.3f m, rule %s"),
              "F193: the F85a event lines carry the animated neck's y, the posture weight, the legs' room, the standing raise, the camera's eye, the neck target and the rule");
    }
}

}  // namespace f193test
