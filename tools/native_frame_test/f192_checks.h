// F192 checks (native on-foot VR), run192 below. Header only, run by native_frame_test.
//
// F192 (native_on_foot_f192.h, native_on_foot.cpp): the left elbow is held at the body-down pole through a weapon draw. The weight dh is 1 from the native epoch's first call with a two-handed
// gun armed, held until the Status.json item flip plus 0.5 s or 2 s from the epoch's start (whichever comes first), then eased to 0 over 0.3 s; the pole blend takes the larger of F180's weight and
// dh and F181's cap share is scaled by (1 - dh). The flight edvr_gfx_20261031_080600: the shotgun-to-plasma-AR swap at 08:11:16.726, Status.json naming the item at 17.616 (0.9 s later).
// The rig runs the state machine on those timings (the epoch start, the hold until the flip and 0.5 s, the timeout, the ramp, a stall, a pistol and a tool not held, a second switch inside
// the ramp, the holster), the weights on the real F165 blend and F181 target, the counters and lines, and the source sites.
#pragma once

#include "../../src/d3d11/native_on_foot_core.h"
#include "../../src/d3d11/native_on_foot_f159.h"
#include "../../src/d3d11/native_on_foot_f165.h"
#include "../../src/d3d11/native_on_foot_f181.h"
#include "../../src/d3d11/native_on_foot_f192.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

namespace f192test {

namespace nf = edvr::nofoot;
namespace f192 = edvr::nofoot::f192;
namespace f165 = edvr::nofoot::f165;
namespace f181 = edvr::nofoot::f181;

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

template <class Check>
inline void run192(Check&& check) {
    const auto eq = [](double a, double b, double tol) { return std::fabs(a - b) <= tol; };
    char buf[400];

    // ---- 1. who is held: two-handed guns on a native epoch -----------------------------------------------------------------------------------------------------------------------
    {
        check(f192::eligible(true, true) && !f192::eligible(false, true) && !f192::eligible(true, false),
              "F192: a draw is held for a two-handed gun armed on a native epoch (only the primary slot's selection has one), and only then (nothing armed, or no native epoch: no hold)");
        check(f192::usedWeight(1.0, true, false) == 0.0 && f192::usedWeight(1.0, false, true) == 0.0 && f192::usedWeight(1.0, true, true) == 0.0 && f192::usedWeight(0.7, false, false) == 0.7 &&
                  f192::usedWeight(std::nan(""), false, false) == 0.0 && f192::usedWeight(1.7, false, false) == 1.0 && f192::usedWeight(-0.2, false, false) == 0.0,
              "F192: at the arm solve a support-cup tool and an authored pistol take no weight whatever the state says; a weight past 0 to 1 is held to it and one that is not a number reads 0");
    }

    // ---- 2. the epoch start, the hold until the flip and 0.5 s --------------------------------------------------------------------------------------------------------------------
    {
        // the flight's swap: the epoch began at 16.726, Status.json named the item at 17.616 (0.890 s later); calls every 11 ms
        f192::DrawHold h;
        const double t0 = 16.726, flip = 17.616;
        const f192::Out first = h.step(true, 7, false, t0);
        check(first.begun && first.weight == 1.0 && !first.lineDue, "F192: the epoch's first eligible call begins the hold at weight 1");
        bool held = true, beganOnce = true, lineEarly = false, flipSeen = false;
        double lastW = 1.0;
        int calls = 0;
        for (int i = 1; i <= 138; ++i) {   // calls every 10 ms from the epoch's start; Status.json names the item at the 89th (0.89 s in: the flight's 17.616)
            const double t = t0 + 0.01 * i;
            const bool flipped = i >= 89;
            const f192::Out o = h.step(true, 7, flipped, t);
            ++calls;
            held = held && o.weight == 1.0;
            beganOnce = beganOnce && !o.begun;
            lineEarly = lineEarly || o.lineDue;
            flipSeen = flipSeen || flipped;
            lastW = o.weight;
        }
        check(held && beganOnce && !lineEarly && flipSeen && lastW == 1.0 && calls > 100,
              "F192: held at 1 for every call from the epoch's start through the Status.json flip (0.89 s) and 0.5 s after it: no new begin, no line yet");
        // 17.616 + 0.5 = 18.116: the ramp starts there, 0.3 s long
        const f192::Out atEnd = h.step(true, 7, true, flip + 0.5);
        check(eq(atEnd.weight, 1.0, 1e-9) && !atEnd.lineDue, "F192: at the end of the hold (flip + 0.5 s) the weight is still 1 (the ramp starts from it)");
        const f192::Out mid = h.step(true, 7, true, flip + 0.5 + 0.15);
        check(eq(mid.weight, 0.5, 1e-9) && !mid.lineDue, "F192: 0.15 s into the 0.3 s ramp the weight is 0.5");
        const f192::Out near1 = h.step(true, 7, true, flip + 0.5 + 0.2999);
        check(near1.weight > 0.0 && near1.weight < 0.01 && !near1.lineDue, "F192: just before the ramp's end the weight is just above 0");
        const f192::Out done = h.step(true, 7, true, flip + 0.5 + 0.3 + 1e-6);
        check(done.weight == 0.0 && done.lineDue && done.line.epoch == 7 && done.line.why == f192::kWhyFlip && done.line.cut == f192::kCutNone && eq(done.line.heldMs, (flip + 0.5 - t0) * 1e3, 1e-6) &&
                  eq(done.line.rampDoneMs, (flip + 0.5 + 0.3 - t0) * 1e3, 1e-6),
              "F192: at the ramp's end the weight is 0 and the draw's line is due: epoch 7, ended by the flip, held 1390 ms, the ramp done at 1690 ms after the epoch began");
        const f192::Out after = h.step(true, 7, true, flip + 2.0);
        check(after.weight == 0.0 && !after.lineDue && !after.begun, "F192: after the ramp the same epoch stays at 0 (no restart, no second line)");
        f192::formatLine(buf, sizeof(buf), done.line);
        check(std::strstr(buf, "left elbow draw hold (F192): epoch 7, held 1390 ms") != nullptr && std::strstr(buf, "the item flip in Status.json and 0.5 s") != nullptr &&
                  std::strstr(buf, "ramp done at 1690 ms after the epoch began") != nullptr && std::strstr(buf, "ran its course") != nullptr,
              "F192: the line says the epoch, how long it was held, why it ended, and when the ramp was done");
    }

    // ---- 3. the timeout: 2 s from the epoch's start when Status.json does not name the item (or late) ----------------------------------------------------------------------------
    {
        f192::DrawHold h;
        h.step(true, 9, false, 100.0);
        bool held = true;
        for (double t = 100.01; t < 101.99; t += 0.01) held = held && h.step(true, 9, false, t).weight == 1.0;
        const f192::Out at2 = h.step(true, 9, false, 102.0);
        const f192::Out mid = h.step(true, 9, false, 102.15);
        const f192::Out done = h.step(true, 9, false, 102.3 + 1e-6);
        check(held && eq(at2.weight, 1.0, 1e-9) && eq(mid.weight, 0.5, 1e-9) && done.weight == 0.0 && done.lineDue && done.line.why == f192::kWhyTimeout && eq(done.line.heldMs, 2000.0, 1e-6) &&
                  eq(done.line.rampDoneMs, 2300.0, 1e-6),
              "F192: with no flip the hold stands 2 s from the epoch's start, then the 0.3 s ramp: held 2000 ms, the ramp done at 2300 ms, ended by the timeout");
        f192::formatLine(buf, sizeof(buf), done.line);
        check(std::strstr(buf, "the 2 s timeout") != nullptr, "F192: the timeout's line says the timeout ended it");
        // a flip at 1.8 s would end the hold at 2.3 s: the timeout comes first
        f192::DrawHold g;
        g.step(true, 4, false, 0.0);
        for (double t = 0.01; t < 1.8; t += 0.01) g.step(true, 4, false, t);
        g.step(true, 4, true, 1.8);
        const f192::Out atT = g.step(true, 4, true, 2.0), mid2 = g.step(true, 4, true, 2.15), end2 = g.step(true, 4, true, 2.3 + 1e-6);
        check(eq(atT.weight, 1.0, 1e-9) && eq(mid2.weight, 0.5, 1e-9) && end2.lineDue && end2.line.why == f192::kWhyTimeout, "F192: a flip at 1.8 s (flip + 0.5 s = 2.3 s) is beaten by the 2 s timeout");
        // a flip at 0.2 s ends it at 0.7 s
        f192::DrawHold e;
        e.step(true, 5, false, 0.0);
        e.step(true, 5, true, 0.2);
        const f192::Out before = e.step(true, 5, true, 0.69), atFlip = e.step(true, 5, true, 0.7), halfway = e.step(true, 5, true, 0.85);
        check(before.weight == 1.0 && eq(atFlip.weight, 1.0, 1e-9) && eq(halfway.weight, 0.5, 1e-9), "F192: a flip at 0.2 s ends the hold at 0.7 s, the ramp 0.3 s long");
    }

    // ---- 4. a stalled game: one call long after the hold's end -----------------------------------------------------------------------------------------------------------------------
    {
        f192::DrawHold h;
        h.step(true, 3, false, 0.0);
        const f192::Out o = h.step(true, 3, false, 10.0);
        check(o.weight == 0.0 && o.lineDue && o.line.cut == f192::kCutNone && o.line.why == f192::kWhyTimeout && eq(o.line.heldMs, 2000.0, 1e-6) && eq(o.line.rampDoneMs, 2300.0, 1e-6),
              "F192: a call 10 s after the epoch began (a stall) finds the hold ended by the timeout at 2 s and the ramp done: weight 0, the line due with the true times");
    }

    // ---- 5. a pistol and a tool are not held; the holster ------------------------------------------------------------------------------------------------------------------------------
    {
        f192::DrawHold h;
        bool never = true;
        for (double t = 0.0; t < 3.0; t += 0.01) {
            const f192::Out a = h.step(f192::eligible(true, false), 11, false, t);   // a pistol: armed, no native epoch (it is in the secondary slot)
            const f192::Out b = h.step(f192::eligible(true, false), 12, false, t);   // a tool: the same
            never = never && a.weight == 0.0 && b.weight == 0.0 && !a.begun && !b.begun && !a.lineDue && !b.lineDue;
        }
        check(never, "F192: a pistol and a tool (armed, no native epoch) draw through three seconds of calls and the hold never begins: weight 0, no begin, no line");
        // the holster: a rifle held at 0.5 s, then nothing armed: the weight is 0 at once, the unfinished draw gets its line (cut), nothing is held after
        f192::DrawHold r;
        r.step(true, 21, false, 0.0);
        const f192::Out still = r.step(true, 21, false, 0.5);
        const f192::Out away = r.step(false, 21, false, 0.51);
        check(still.weight == 1.0 && away.weight == 0.0 && away.lineDue && away.line.cut == f192::kCutGone && away.line.why == f192::kWhyNone && eq(away.line.heldMs, 510.0, 1e-6) &&
                  std::isnan(away.line.rampDoneMs),
              "F192: putting the gun away at 0.51 s ends the hold at once (weight 0), with its line: cut short, held 510 ms, no ramp");
        f192::formatLine(buf, sizeof(buf), away.line);
        check(std::strstr(buf, "cut short: the gun was put away") != nullptr && std::strstr(buf, "ramp not done") != nullptr && std::strstr(buf, "not ended") != nullptr,
              "F192: a cut draw's line says it was cut short and the ramp was not done");
        // a draw from the fists after it begins a fresh hold
        const f192::Out again = r.step(true, 22, false, 5.0);
        check(again.begun && again.weight == 1.0 && !again.lineDue, "F192: a draw from the fists after the holster (a new epoch) gets a fresh hold");
        const f192::Out silent = r.step(false, 22, false, 5.1);
        const f192::Out silent2 = r.step(false, 22, false, 5.2);
        check(silent.lineDue && !silent2.lineDue && silent2.weight == 0.0, "F192: the line of an unarmed state is said once");
    }

    // ---- 6. a second switch inside the ramp restarts the hold -------------------------------------------------------------------------------------------------------------------------
    {
        f192::DrawHold h;
        h.step(true, 30, false, 0.0);
        h.step(true, 30, true, 0.1);
        const f192::Out ramping = h.step(true, 30, true, 0.7);   // flip 0.1 + 0.5 = 0.6: the ramp began at 0.6, 0.1 s in
        check(ramping.weight < 1.0 && ramping.weight > 0.5, "F192: 0.1 s into the ramp the weight is two thirds");
        const f192::Out second = h.step(true, 31, false, 0.75);   // another switch, inside the ramp
        check(second.begun && second.weight == 1.0 && second.lineDue && second.line.epoch == 30 && second.line.cut == f192::kCutReplaced && second.line.why == f192::kWhyFlip &&
                  std::isnan(second.line.rampDoneMs),
              "F192: a second switch inside the ramp restarts the hold at weight 1 and gives the first draw its line (cut short: another draw began)");
        bool held = true;
        for (double t = 0.76; t < 2.74; t += 0.01) held = held && h.step(true, 31, false, t).weight == 1.0;
        const f192::Out atEnd = h.step(true, 31, false, 2.75), mid = h.step(true, 31, false, 2.9), done = h.step(true, 31, false, 3.05 + 1e-6);
        check(held && eq(atEnd.weight, 1.0, 1e-9) && eq(mid.weight, 0.5, 1e-9) && done.weight == 0.0 && done.lineDue && done.line.epoch == 31 && eq(done.line.heldMs, 2000.0, 1e-6),
              "F192: the restarted hold runs its own 2 s from the second epoch's start (2.75 s), then its own ramp");
        f192::formatLine(buf, sizeof(buf), second.line);
        check(std::strstr(buf, "cut short: another draw began") != nullptr, "F192: the cut line names the replacing draw");
    }

    // ---- 7. the weights on the real pole blend and the hip cap ---------------------------------------------------------------------------------------------------------------------------
    {
        check(f192::blendWeight(0.0, 1.0) == 1.0 && f192::blendWeight(1.0, 0.0) == 1.0 && f192::blendWeight(0.4, 0.7) == 0.7 && f192::blendWeight(0.9, 0.2) == 0.9 && f192::blendWeight(0.0, 0.0) == 0.0 &&
                  f192::blendWeight(std::nan(""), 0.3) == 0.3,
              "F192: the pole blend takes the larger of F180's weight and the draw hold; no hold leaves F180's weight (0 at the hip) exactly");
        check(f192::capShare(1.0, 0.0) == 1.0 && f192::capShare(1.0, 1.0) == 0.0 && eq(f192::capShare(1.0, 0.25), 0.75, 1e-15) && eq(f192::capShare(0.5, 0.5), 0.25, 1e-15) &&
                  f192::capShare(0.0, 0.3) == 0.0,
              "F192: F181's hip share is scaled by (1 - the hold): whole with no hold, 0 under a whole one");
        // the cap's target: a plane angle of 60 deg over the cap of 25 deg is brought to 25 at the hip; under the hold it is left alone; half held, half way
        const double cap = 25.0, plane = 60.0;
        check(eq(f181::targetDeg(plane, cap, f192::capShare(f181::hipShare(0.0), 0.0)), 25.0, 1e-12) && f181::targetDeg(plane, cap, f192::capShare(f181::hipShare(0.0), 1.0)) == plane &&
                  eq(f181::targetDeg(plane, cap, f192::capShare(f181::hipShare(0.0), 0.5)), 42.5, 1e-12),
              "F192: through the whole hold the cap does not pull the elbow toward the animated arm (the target stays at the plane angle), half way it cuts half, with none it is F181's cut");
        // the real blend: the flight's draw. The stable pole had no feasible swivel and put the elbow at the bound, 43 deg off the body-down pole (F165's rig), at the hip (w = 0).
        constexpr double kPi = nf::kIkPi;
        constexpr double kUp[3] = {0.0, 1.0, 0.0};
        const double ref[3] = {0.0, -1.0, 0.0};
        const double outSign = -1.0, outM = nf::kIkAdsElbowOutCm / 100.0, trim = nf::kIkAdsTrimDeg * kPi / 180.0;
        const double S[3] = {-0.19, 0.85, 0.0}, T[3] = {S[0] + 0.14, S[1] + 0.03, S[2] + 0.47};
        const nf::IkTwoBone tb = nf::ikTwoBoneFrame(S, T, 0.30, 0.28, ref);
        const nf::IkPistolDownSwivel down = nf::ikPistolDownSwivel(tb, ref, kUp, outM, trim);
        double lateral[3];
        nf::ikCross(tb.dir, kUp, lateral);
        nf::ikUnit(lateral, lateral);
        const double toLeft = nf::ikWrapPi(tb.angleOf(lateral) - down.theta) >= 0.0 ? 1.0 : -1.0;
        nf::IkPhysicalPole seed;
        seed.valid = nf::ikElbowRadial(tb, nf::ikWrapPi(down.theta + toLeft * 43.0 / (180.0 / kPi)), seed.reference);
        for (int i = 0; i < 3; ++i) {
            seed.previous[i] = seed.reference[i];
            seed.axis[i] = tb.dir[i];
        }
        const nf::IkStableSwivel stable = nf::ikStableSwivel(tb, seed, outSign, outM);
        const double stableTheta = nf::ikWrapPi(stable.theta);
        double Es[3], Ed[3], Eh[3];
        tb.elbowAt(stableTheta, Es);
        tb.elbowAt(down.theta, Ed);
        const f165::Blend none = f165::blendPole(tb, stableTheta, f192::blendWeight(0.0, 0.0), seed.previous, outSign, outM, trim);
        check(down.ok && stable.ok && !none.applied && std::memcmp(&none.theta, &stableTheta, sizeof(stableTheta)) == 0,
              "F192: with no hold and F180's weight 0 (the hip) the stable pole's swivel comes through bit for bit, nothing applied");
        const f165::Blend held = f165::blendPole(tb, stableTheta, f192::blendWeight(0.0, 1.0), seed.previous, outSign, outM, trim);
        tb.elbowAt(held.theta, Eh);
        check(held.applied && std::fabs(nf::ikWrapPi(held.theta - down.theta)) < 1e-12 && Eh[1] - S[1] < -0.10 && Es[1] - Eh[1] > 0.05 && Eh[1] <= Ed[1] + 1e-12,
              "F192: at the hip under a whole hold the elbow is at the body-down pole from the first stable frame: over 10 cm under the shoulder, over 5 cm under where the stable pole put it");
        // the ramp back: the elbow only rises toward the stable pole's as dh falls, continuously, never over the shoulder
        bool rises = true, calm = true;
        double lastY = Eh[1], lastTheta = held.theta;
        for (int i = 99; i >= 0; --i) {
            const f165::Blend b = f165::blendPole(tb, stableTheta, f192::blendWeight(0.0, i / 100.0), seed.previous, outSign, outM, trim);
            double E[3];
            tb.elbowAt(b.theta, E);
            rises = rises && E[1] >= lastY - 1e-12 && E[1] - S[1] <= 1e-9;
            calm = calm && std::fabs(nf::ikWrapPi(b.theta - lastTheta)) < 0.02;
            lastY = E[1];
            lastTheta = b.theta;
        }
        check(rises && calm, "F192: as the hold's weight falls from 1 to 0 in a hundred steps the elbow only rises toward the stable pole's, no step over 1.2 deg, never over the shoulder");
        // F180's weight in the sights (1) is untouched by a hold
        check(f192::blendWeight(1.0, 0.6) == 1.0, "F192: in the sights (F180's weight 1) a draw hold changes nothing");
    }

    // ---- 8. the counters and lines ---------------------------------------------------------------------------------------------------------------------------------------------------
    {
        f192::Counters c;
        for (int i = 0; i < 30; ++i) c.add(0.0, false);
        for (int i = 0; i < 40; ++i) c.add(1.0, true);
        for (int i = 0; i < 10; ++i) c.add(0.5, true);
        c.add(1.0, false);
        c.begun = 2;
        f192::Counters d;
        d.add(1.0, true);
        d.begun = 1;
        c.merge(d);
        check(c.solves == 82 && c.held == 52 && c.applied == 51 && c.begun == 3 && eq(c.wSum, 40.0 + 5.0 + 1.0 + 1.0, 1e-12), "F192: the counters count the solves, those held, those applied and the draws begun, and merge");
        std::snprintf(buf, sizeof(buf), "line");
        f192::appendSummary(buf, sizeof(buf), c);
        check(std::strstr(buf, "line; draw hold (F192): held 52 of 82 solves (applied 51, mean weight 0.90, draws begun 3)") != nullptr,
              "F192: the F159 line's ending says how many solves were held of how many, how many applied, the mean weight and the draws begun");
        std::snprintf(buf, sizeof(buf), "line");
        f192::appendSummary(buf, sizeof(buf), f192::Counters{});
        check(std::strcmp(buf, "line") == 0, "F192: a window with no stable-pole solve appends nothing");
        char tiny[12] = "abcdefghij";
        f192::appendSummary(tiny, sizeof(tiny), c);
        check(std::strlen(tiny) < sizeof(tiny), "F192: with no room the ending is cut off, never written past the buffer");
    }

    // ---- 9. the source sites ----------------------------------------------------------------------------------------------------------------------------------------------------------------
    {
        const std::string cpp = slurp("src/d3d11/native_on_foot.cpp"), core = slurp("src/d3d11/native_on_foot_core.h");
        const auto has = [&](const std::string& needle) { return cpp.find(needle) != std::string::npos; };
        const auto at = [&](const std::string& needle) { return cpp.find(needle); };
        check(has("static nofoot::f192::DrawHold s_f192Hold;") &&
                  has("const bool flipped = f83NativeEpochIdentity && s_f83JournalStartSet && itemSerialForCall != s_f83JournalStartSerial;") &&
                  has("s_f192Hold.step(nofoot::f192::eligible(armedNow, f83NativeEpochIdentity), f83SelectionForCall.epoch, flipped, tS);") &&
                  countOf(cpp, "s_f192Hold.step(") == 1,
              "F192: the hold is stepped once a call, keyed on the native epoch the pole identity uses, with the flip read from the held item's serial against the epoch's start serial");
        const size_t identityAt = at("const uint64_t physicalPoleIdentity = f83NativeEpochIdentity ?"), stepAt = at("s_f192Hold.step("), useAt = at("const double dh = nofoot::f192::usedWeight(f192Dh, supportCup, authoredPistolSupport);");
        check(identityAt != std::string::npos && stepAt != std::string::npos && useAt != std::string::npos && identityAt < stepAt && stepAt < useAt,
              "F192: the weight is made after the pole's identity and before the solves that use it");
        check(has("const double wBlend = nofoot::f192::blendWeight(w165, dh);") && has("blendPole(tb, poleTheta165, wBlend, seed.previous, outSign, elbowOutM, trimRad)") &&
                  has("o.f165Applied = w165 > 0.0;") && has("o.f192Applied = dh > 0.0;") &&
                  has("nofoot::f181::targetDeg(planeBefore, capDeg, nofoot::f192::capShare(nofoot::f181::hipShare(pinCall.detachW), o.f192Dh))"),
              "F192: the left stable pole blends by the larger of F180's weight and the hold (F165's count stays F165's) and F181's share is scaled by (1 - dh)");
        check(has("if (so[0].f181Hip && !(so[0].f192Dh > 0.0)) f.f181.add(") && has("if (so[0].f192Gate) f.f192.add(so[0].f192Dh, so[0].f192Applied);") && has("if (f192Began) ++f.f192.begun;") &&
                  has("nofoot::f192::appendSummary(line159, sizeof(line159), f98.f192);") && has("nofoot::f192::Counters f192;"),
              "F192: the window counts the held solves (F181's count leaves them out), prints them on the F159 line and counts the draws begun");
        check(has("sample.f192Dh = so[0].f192Dh;") && has("sample.f192AnimElbowDyM = so[0].f192AnimElbowDy;") && has("sample.f181PlaneBeforeDeg = so[0].f181PlaneBeforeDeg;") &&
                  has("left elbow draw hold (F192) dh %.2f, the animated elbow over the shoulder %s, F181 plane angle before the cut %s") &&
                  core.find("double f192Dh = 0.0, f192AnimElbowDyM = std::nan(\"\"), f181PlaneBeforeDeg = std::nan(\"\");") != std::string::npos &&
                  has("o.f192AnimElbowDy = F0[1] - S[1];") && has("o.f181PlaneBeforeDeg = planeBefore;"),
              "F192: the F76 sample carries dh, the animated elbow's height over the shoulder and F181's plane angle before the cut, and the F80 line prints them");
        check(has("nofoot::f192::formatLine(holdLine, sizeof(holdLine), hold.line);") && has("if (hold.lineDue) {"), "F192: a draw's account is logged as one line when it is complete");
    }
}

}  // namespace f192test
