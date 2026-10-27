// F138 checks (native on-foot VR): the view holds its height over the neck while the look is pitched. Header only, run by native_frame_test (run138).
// Convention: G = the game's camera (raw) less the animated neck less the headset's LOCAL height; the sink is the stance's G less this call's, clamped to [-40, +40] cm; the
// raise's target while pitched is the stance's plus w times the sink, w = smoothstep(10, 20 deg, |aim pitch|), 0 for a look down under look to aim (as flown); the end of
// an episode this tracker published in, with nothing drawn, gives the stance's target back once.
#pragma once

#include "../../src/d3d11/native_on_foot_f138.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>

namespace f138test {

namespace f138 = edvr::nofoot::f138;

// A tracker holding a stance: the raise's target +12 cm, the game's camera 1.46 m, the animated neck 1.30 m, the headset 0 m in LOCAL, LOCAL generation 3.
constexpr double kTarget = 0.12, kEye = 1.46, kNeck = 1.30, kHead = 0.0, kGen = 3;   // G = 1.46 - 1.30 - 0 = 0.16
inline f138::Tracker stanced() {
    f138::Tracker t;
    t.noteStance(kTarget, kEye, kNeck, kHead, static_cast<uint32_t>(kGen), true);
    return t;
}
// A pitched call whose camera has sunk by sinkM against the stance (headset where it was).
inline f138::In pitchedCall(double aimDeg, double sinkM, double headY = kHead) {
    f138::In in;
    in.aimPitchDeg = aimDeg;
    in.poseValid = true;
    in.generation = static_cast<uint32_t>(kGen);
    in.rawEyeY = kEye - sinkM + headY;   // G = rawEyeY - neck - head = 0.16 - sinkM
    in.neckY = kNeck;
    in.localHeadY = headY;
    return in;
}

template <class Check>
inline void run138(Check&& check) {
    {   // the weight: smoothstep(10, 20 deg) of |pitch|, 0 for a pitch that is not a number
        check(f138::pitchWeight(0.0) == 0.0 && f138::pitchWeight(9.9) == 0.0 && f138::pitchWeight(10.0) == 0.0, "F138: no weight up to 10 deg");
        check(std::fabs(f138::pitchWeight(15.0) - 0.5) < 1e-12 && std::fabs(f138::pitchWeight(-15.0) - 0.5) < 1e-12, "F138: half way at 15 deg, up or down (the magnitude)");
        check(f138::pitchWeight(20.0) == 1.0 && f138::pitchWeight(65.0) == 1.0 && f138::pitchWeight(-70.0) == 1.0, "F138: the full weight from 20 deg");
        check(f138::pitchWeight(12.0) < f138::pitchWeight(14.0) && f138::pitchWeight(14.0) < f138::pitchWeight(18.0), "F138: the weight rises with the pitch");
        check(f138::pitchWeight(std::nan("")) == 0.0 && f138::pitchWeight(INFINITY) == 0.0 && f138::weight(std::nan(""), false) == 0.0 && f138::weight(std::nan(""), true) == 0.0,
              "F138: an aim pitch that is not a number has no weight");
        // the mode: looking up applies in both, looking down under mouse look only (under look to aim the raise stays held, as flown)
        check(f138::weight(-40.0, true) == 0.0 && f138::weight(40.0, true) == 1.0 && f138::weight(-40.0, false) == 1.0 && f138::weight(40.0, false) == 1.0,
              "F138: look to aim at -40 gives w 0, at +40 w 1; mouse look at -40 gives w 1");
        check(f138::weight(-10.5, true) == 0.0 && std::fabs(f138::weight(15.0, true) - 0.5) < 1e-12 && std::fabs(f138::weight(-15.0, false) - 0.5) < 1e-12 && f138::weight(0.0, true) == 0.0,
              "F138: the smoothstep is the same up in both modes and down under mouse look");
        // the instrument's size and the settle's refusal are the same 10 deg
        check(!f138::pitched(9.99, false) && f138::pitched(10.0, false) && f138::pitched(-10.0, false) && f138::pitched(-62.0, false) && !f138::pitched(std::nan(""), false),
              "F138: a pitched call is |aim| of 10 deg or more");
        check(f138::pitched(40.0, true) && !f138::pitched(-40.0, true) && f138::pitched(-40.0, false), "F138: a look down under look to aim is not a pitched call (as flown)");
        check(!f138::settleRefused(9.99, 10.0, false) && f138::settleRefused(10.0, 10.0, false) && f138::settleRefused(-40.0, 10.0, false) && f138::settleRefused(70.0, 10.0, false) &&
                  !f138::settleRefused(std::nan(""), 10.0, false) && !f138::settleRefused(INFINITY, 10.0, false),
              "F138: the settle refuses a pitched aim under mouse look too: |pitch| of 10 deg or more, finite");
        check(!f138::settleRefused(-40.0, 10.0, true) && !f138::settleRefused(70.0, 10.0, true),
              "F138: with an item drawn the settle is as before F138 (the armed target is an absolute height): no refusal");
    }
    {   // G, the sink, the target
        check(std::fabs(f138::gOf(1.90, 1.50, 0.30) - 0.10) < 1e-12, "F138: G is the raw camera less the neck less the headset's LOCAL height (1.90 - 1.50 - 0.30 = 0.10)");
        check(f138::sinkOf(0.16, 0.16) == 0.0 && std::fabs(f138::sinkOf(0.16, 0.074) - 0.086) < 1e-12, "F138: the sink is the stance's G less this call's (8.6 cm)");
        check(f138::sinkOf(0.16, -0.60) == 0.40 && f138::sinkOf(0.16, 5.0) == -0.40, "F138: the sink is clamped to [-40, +40] cm");
        check(std::fabs(f138::sinkOf(0.16, 0.30) + 0.14) < 1e-12 && std::fabs(f138::sinkOf(0.16, 0.33) + 0.17) < 1e-12,
              "F138: a camera that rose 14 or 17 cm over the neck (looking down unarmed) is a sink of -14 or -17 cm, not cut to -10");
        check(std::fabs(f138::targetOf(0.12, 0.5, 0.20) - 0.22) < 1e-12 && f138::targetOf(0.12, 0.0, 0.40) == 0.12, "F138: the target is the stance's plus w times the sink");
        check(f138::stanceHolds(1.30, 1.30) && f138::stanceHolds(1.329, 1.30) && f138::stanceHolds(1.271, 1.30) && !f138::stanceHolds(1.331, 1.30) && !f138::stanceHolds(1.269, 1.30) &&
                  !f138::stanceHolds(std::nan(""), 1.30),
              "F138: the same stance is the animated neck within 3 cm of the reference's");
    }
    {   // applied: mouse look and look to aim alike, the target is the stance's plus w times the sink
        f138::Tracker t = stanced();
        const f138::Out o = t.step(pitchedCall(40.0, 0.086));
        check(o.pitched && o.considered && o.w == 1.0 && o.reason == f138::kApplied && o.sinkValid && std::fabs(o.sinkM - 0.086) < 1e-12 && o.publish &&
                  std::fabs(o.publishM - (kTarget + 0.086)) < 1e-12 && std::fabs(o.addedM - 0.086) < 1e-12,
              "F138: looking up 40 deg with the camera 8.6 cm down: the raise's target is the stance's +8.6 cm");
        const f138::Out half = t.step(pitchedCall(15.0, 0.10));
        check(half.reason == f138::kApplied && std::fabs(half.w - 0.5) < 1e-12 && std::fabs(half.publishM - (kTarget + 0.05)) < 1e-12 && std::fabs(half.addedM - 0.05) < 1e-12,
              "F138: at 15 deg half the sink is added");
        // unarmed looking down under mouse look (the F137 flight: the camera ROSE 12 to 17 cm over the neck at -37 to -65 deg): the same rule, the sink negative
        const f138::Out down = t.step(pitchedCall(-45.0, -0.14));
        check(down.pitched && down.reason == f138::kApplied && std::fabs(down.sinkM + 0.14) < 1e-12 && std::fabs(down.publishM - (kTarget - 0.14)) < 1e-12,
              "F138: looking down under mouse look with the camera 14 cm up: the raise's target is the stance's -14 cm (the magnitude of the pitch)");
        const f138::Out down17 = t.step(pitchedCall(-65.0, -0.17));
        check(down17.reason == f138::kApplied && std::fabs(down17.publishM - (kTarget - 0.17)) < 1e-12, "F138: and 17 cm up at -65 deg: -17 cm, not cut to -10");
        const f138::Out huge = t.step(pitchedCall(60.0, 0.70));
        check(huge.reason == f138::kApplied && std::fabs(huge.sinkM - 0.40) < 1e-12 && std::fabs(huge.publishM - (kTarget + 0.40)) < 1e-12, "F138: a sink of 70 cm is clamped to 40");
        const f138::Out rise = t.step(pitchedCall(60.0, -0.30));
        check(rise.reason == f138::kApplied && std::fabs(rise.sinkM + 0.30) < 1e-12 && std::fabs(rise.publishM - (kTarget - 0.30)) < 1e-12, "F138: a camera that rose 30 cm is taken back whole");
        const f138::Out riseMore = t.step(pitchedCall(60.0, -0.60));
        check(riseMore.reason == f138::kApplied && std::fabs(riseMore.sinkM + 0.40) < 1e-12 && std::fabs(riseMore.publishM - (kTarget - 0.40)) < 1e-12, "F138: a rise of 60 cm is clamped to -40");
        // the pilot's own head: his height in LOCAL stays in the view (G removes it from the camera's)
        const f138::Out head = t.step(pitchedCall(40.0, 0.05, 0.12));
        check(head.reason == f138::kApplied && std::fabs(head.sinkM - 0.05) < 1e-12 && std::fabs(head.headRiseM - 0.12) < 1e-12,
              "F138: the headset 12 cm up and the camera with it: the sink is the camera's own 5 cm and the head's 12 cm is reported beside it");
    }
    {   // look to aim: looking up applies, looking down stays as flown (w 0, not a pitched call, nothing published, the F109 gate's hold untouched)
        f138::Tracker t = stanced();
        f138::In up = pitchedCall(40.0, 0.086);
        up.lookToAim = true;
        const f138::Out o = t.step(up);
        check(o.pitched && o.considered && o.w == 1.0 && o.reason == f138::kApplied && o.publish && std::fabs(o.publishM - (kTarget + 0.086)) < 1e-12,
              "F138: look to aim looking up 40 deg applies, as mouse look does");
        f138::Tracker d = stanced();
        f138::In dn = pitchedCall(-40.0, -0.14);
        dn.lookToAim = true;
        const f138::Out od = d.step(dn);
        check(!od.pitched && !od.considered && od.w == 0.0 && !od.publish && !od.sinkValid && !od.returned && !od.notNeeded && !d.added && !d.episode,
              "F138: look to aim looking down at -40: w 0, not a pitched call, nothing published, no sink measured");
        // an up episode under look to aim then a look down: the episode ends there, the stance's target comes back
        f138::Out e = t.step(dn);
        check(e.returned && e.publish && e.publishM == kTarget && !e.pitched, "F138: look to aim up, then down: the up episode ends at the turn and gives the stance's target back");
        // the same look down under mouse look is a pitched call that applies
        f138::Tracker m = stanced();
        f138::In mn = pitchedCall(-40.0, -0.14);
        const f138::Out om = m.step(mn);
        check(om.pitched && om.w == 1.0 && om.publish && std::fabs(om.publishM - (kTarget - 0.14)) < 1e-12, "F138: mouse look at -40 applies (w 1)");
    }
    {   // the return edge: the first call after w is back to 0 publishes the stance's target once
        f138::Tracker t = stanced();
        t.step(pitchedCall(40.0, 0.10));
        const f138::Out edge = t.step(pitchedCall(5.0, 0.0));
        check(edge.returned && edge.publish && edge.publishM == kTarget && !edge.considered && !edge.pitched, "F138: back to a level look the first call publishes the stance's target exactly");
        const f138::Out again = t.step(pitchedCall(5.0, 0.0));
        check(!again.returned && !again.publish, "F138: and only once");
        // the weight reaching 0 at 10 deg is the edge too
        f138::Tracker u = stanced();
        u.step(pitchedCall(30.0, 0.10));
        const f138::Out ten = u.step(pitchedCall(10.0, 0.10));
        check(ten.returned && ten.pitched && ten.publishM == kTarget, "F138: at exactly 10 deg the weight is 0 and the stance's target returns");
        // nothing was added: no edge
        f138::Tracker n = stanced();
        const f138::Out none = n.step(pitchedCall(5.0, 0.0));
        check(!none.returned && !none.publish && !none.returnRefused && !none.notNeeded, "F138: a level look with no pitched episode before it publishes nothing and counts nothing");
        // an armed-only episode (an item drawn all through it: measured only, nothing published) gives nothing back at the edge
        f138::Tracker armedOnly = stanced();
        f138::In armedUp = pitchedCall(20.0, 0.10);
        armedUp.itemDrawn = true;
        const f138::Out armedPitched = armedOnly.step(armedUp);
        f138::In armedLevel = pitchedCall(5.0, 0.0);
        armedLevel.itemDrawn = true;
        const f138::Out armedEdge = armedOnly.step(armedLevel);
        check(armedPitched.reason == f138::kItemDrawn && !armedPitched.publish && !armedEdge.publish && !armedEdge.returned && !armedEdge.returnRefused && armedEdge.notNeeded &&
                  !armedOnly.step(armedLevel).notNeeded,
              "F138: an episode with an item drawn throughout publishes nothing at the edge: 'not needed', once (the armed target is an absolute height)");
        // drawing a weapon, aiming 20 deg down and coming level (mouse look) must not overwrite the armed raise with the unarmed stance's
        f138::Tracker draw = stanced();
        f138::In drawDown = pitchedCall(-20.0, -0.05);
        drawDown.itemDrawn = true;
        draw.step(drawDown);
        check(!draw.step(armedLevel).publish, "F138: weapon drawn, aim 20 deg down, level again: the unarmed stance's target is not published over the armed raise");
        // the camera away or the sights: hold, and the edge comes when they are over
        f138::Tracker a = stanced();
        a.step(pitchedCall(40.0, 0.10));
        f138::In away = pitchedCall(5.0, 0.0);
        away.cameraAway = true;
        check(!a.step(away).publish, "F138: no return while the camera is away");
        f138::In sights = pitchedCall(5.0, 0.0);
        sights.ads = true;
        check(!a.step(sights).publish, "F138: nor in the sights");
        check(a.step(pitchedCall(5.0, 0.0)).returned, "F138: it returns when they are over");
        // the settle published this call: its target replaces ours, no edge
        f138::Tracker s = stanced();
        s.step(pitchedCall(40.0, 0.10));
        f138::In owned = pitchedCall(5.0, 0.0);
        owned.settleOwned = true;
        const f138::Out so = s.step(owned);
        check(!so.publish && !so.returned && so.notNeeded && !s.added && !s.step(pitchedCall(5.0, 0.0)).publish, "F138: the settle's own publication ends the added sink, no second one from the edge (not needed)");
        // the stance changed while pitched: the stance's target is not the right one; the edge says so
        f138::Tracker c = stanced();
        c.step(pitchedCall(40.0, 0.10));
        f138::In crouch = pitchedCall(5.0, 0.0);
        crouch.neckY = kNeck - 0.20;
        const f138::Out cr = c.step(crouch);
        check(!cr.publish && cr.returnRefused && !c.added, "F138: a crouch since the stance: the edge publishes nothing and counts it");
        // an item drawn at the edge: nothing is published over the armed raise (the armed settle takes over), counted as not needed
        f138::Tracker i = stanced();
        i.step(pitchedCall(40.0, 0.10));
        f138::In item = pitchedCall(5.0, 0.0);
        item.itemDrawn = true;
        const f138::Out ie = i.step(item);
        check(!ie.publish && !ie.returned && !ie.returnRefused && ie.notNeeded && !i.added && !i.episode,
              "F138: an applied episode with an item drawn at the edge: nothing is published, 'not needed'");
        // an applied episode with nothing drawn publishes once
        f138::Tracker applied = stanced();
        applied.step(pitchedCall(40.0, 0.10));
        const f138::Out appliedEdge = applied.step(pitchedCall(5.0, 0.0));
        check(appliedEdge.returned && appliedEdge.publish && appliedEdge.publishM == kTarget && !appliedEdge.notNeeded && !applied.step(pitchedCall(5.0, 0.0)).publish,
              "F138: an applied episode publishes the stance's target once at the edge");
        // an aim that is unknown (G absent this call) neither applies nor returns
        f138::Tracker k = stanced();
        k.step(pitchedCall(40.0, 0.10));
        f138::In blind = pitchedCall(40.0, 0.10);
        blind.aimPitchDeg = std::nan("");
        const f138::Out bl = k.step(blind);
        check(!bl.pitched && !bl.publish && !bl.returned && k.added && k.step(pitchedCall(5.0, 0.0)).returned, "F138: an unknown aim pitch changes nothing (the raise does not flap), the edge comes at the next level one");
    }
    {   // every refusal has a counted reason; measured only with an item drawn
        const auto reasonOf = [](const f138::Tracker& base, const f138::In& in) {
            f138::Tracker t = base;
            return t.step(in);
        };
        const f138::Tracker have = stanced();
        f138::In in = pitchedCall(40.0, 0.10);
        in.itemDrawn = true;
        const f138::Out item = reasonOf(have, in);
        check(item.reason == f138::kItemDrawn && item.sinkValid && std::fabs(item.sinkM - 0.10) < 1e-12 && !item.publish && item.addedM == 0.0, "F138: an item drawn: the sink is measured and nothing is published");
        in = pitchedCall(40.0, 0.10);
        const f138::Tracker none{};
        check(reasonOf(none, in).reason == f138::kNoReference && !reasonOf(none, in).publish && !reasonOf(none, in).sinkValid, "F138: no reference yet");
        in.generation = 4;
        check(reasonOf(have, in).reason == f138::kGeneration && !reasonOf(have, in).publish, "F138: a recentre (the LOCAL pose's generation changed) drops the reference");
        in = pitchedCall(40.0, 0.10);
        in.neckY = kNeck + 0.031;
        check(reasonOf(have, in).reason == f138::kStance, "F138: the animated neck moved 3.1 cm: another stance");
        in.neckY = kNeck + 0.029;
        check(reasonOf(have, in).reason == f138::kApplied, "F138: 2.9 cm is the same stance");
        in = pitchedCall(40.0, 0.10);
        in.ads = true;
        check(reasonOf(have, in).reason == f138::kAds && !reasonOf(have, in).publish, "F138: in the sights");
        in = pitchedCall(40.0, 0.10);
        in.cameraAway = true;
        check(reasonOf(have, in).reason == f138::kCameraAway, "F138: the camera away from the commander");
        in = pitchedCall(40.0, 0.10);
        in.poseValid = false;
        check(reasonOf(have, in).reason == f138::kPoseInvalid, "F138: the LOCAL pose is not valid");
        in = pitchedCall(40.0, 0.10);
        in.rawEyeY = std::nan("");
        check(reasonOf(have, in).reason == f138::kInputs, "F138: a camera height that is not a number");
        in = pitchedCall(40.0, 0.10);
        in.settleOwned = true;
        check(reasonOf(have, in).reason == f138::kSettleOwned && !reasonOf(have, in).publish, "F138: the settle published this call");
        // a stance noted with an invalid pose drops the reference
        f138::Tracker bad = stanced();
        bad.noteStance(0.12, kEye, kNeck, kHead, 3, false);
        check(!bad.ref.have && reasonOf(bad, pitchedCall(40.0, 0.10)).reason == f138::kNoReference, "F138: a stance settled without a valid LOCAL pose leaves no reference (a stale one is not used)");
        // a call under 10 deg has no reason to give
        const f138::Out level = reasonOf(have, pitchedCall(7.0, 0.1));
        check(!level.pitched && !level.considered && !level.publish, "F138: a level look is not considered");
    }
    {   // never shown as added when (b) never ran
        f138::Window w;
        f138::Tracker none;   // no reference: every pitched call refused
        for (int i = 0; i < 5; ++i) {
            f138::Call c;
            c.aimDeg = 40.0;
            c.out = none.step(pitchedCall(40.0, 0.1));
            w.add(c);
        }
        char text[200];
        f138::notAppliedText(w.why, text, sizeof(text));
        check(w.calls == 5 && w.appliedN == 0 && w.measuredN == 0 && w.addedSum == 0.0 && std::strcmp(text, "no reference 5") == 0,
              "F138: a build whose reference never came: no call is counted as added, the line says 'no reference 5'");
        f138::Window empty;
        f138::notAppliedText(empty.why, text, sizeof(text));
        check(std::strcmp(text, "none") == 0, "F138: nothing refused reads 'none'");
    }
    {   // the window: a second of calls, the means, the bins
        f138::Tracker t = stanced();
        f138::Window w;
        double aims[4] = {25.0, 40.0, 40.0, -20.0};
        double sinks[4] = {0.04, 0.086, 0.10, 0.02};
        for (int i = 0; i < 4; ++i) {
            f138::Call c;
            c.aimDeg = aims[i];
            c.headValid = true;
            c.headDeg = 3.0;
            c.lookToAim = i == 3;
            c.carriedUpCm = 5.0;
            c.carriedFwdCm = -2.0;
            c.viewOverNeckCm = 7.0;
            c.settleRefusedAim = i != 3;
            c.out = t.step(pitchedCall(aims[i], sinks[i], 0.05));
            w.add(c);
            w.addInside(i == 1);
        }
        f138::Call item;
        item.aimDeg = 40.0;
        item.itemDrawn = true;
        f138::In ii = pitchedCall(40.0, 0.06);
        ii.itemDrawn = true;
        item.out = t.step(ii);
        w.add(item);
        check(w.calls == 5 && w.modeCalls[0] == 4 && w.modeCalls[1] == 1 && w.itemCalls == 1, "F138: five pitched calls, one in look to aim, one with an item");
        check(w.appliedN == 4 && w.measuredN == 1 && w.consideredN == 5 && w.sinkN == 5 && std::fabs(w.sinkMax - 10.0) < 1e-9, "F138: four added, one measured only, the largest sink 10 cm");
        check(w.aimMin == -20.0 && w.aimMax == 40.0 && w.settleRefusedAim == 3 && w.insideOf == 4 && w.inside == 1, "F138: the aim's range, the settle refusals, INSIDE 1 of 4");
        // the bins: 40 deg is [40,50), 25 is [20,30), -20 is [-20,-10), 0 is [0,10)
        check(f138::binOf(-90.0) == 0 && f138::binOf(-80.0) == 1 && f138::binOf(-0.1) == 8 && f138::binOf(0.0) == 9 && f138::binOf(10.0) == 10 && f138::binOf(89.9) == 17 &&
                  f138::binOf(90.0) == 17 && f138::binOf(-120.0) == 0 && f138::binOf(40.0) == 13 && f138::binOf(25.0) == 11 && f138::binOf(-20.0) == 7,
              "F138: the bins are 10 deg wide from -90 to +90");
        const f138::Group& g = w.group[0][0];
        check(g.bin[13].n == 2 && g.bin[11].n == 1 && w.group[0][1].bin[13].n == 1 && w.group[1][0].bin[7].n == 1, "F138: calls land by mode and item class");
        char text[400];
        const size_t n = f138::binsText(g, text, sizeof(text));
        check(n > 0 && std::string(text).find("[20,30) 1 +4.0 +5.0 -2.0 +5.0") != std::string::npos && std::string(text).find("[40,50) 2 +9.3 +5.0 -2.0 +5.0") != std::string::npos,
              "F138: a bin's line is its range, calls, mean sink, carried up, carried forward and the head's height (cm)");
        f138::Group other;
        other.bin[13].n = 3;   // no sink data
        check(f138::binsText(other, text, sizeof(text)) > 0 && std::string(text).find("[40,50) 3 -- +0.0 +0.0 --") != std::string::npos, "F138: a bin with no sink data says '--'");
        f138::Group sum = g;
        sum.merge(g);
        check(sum.bin[13].n == 4 && sum.bin[11].n == 2 && std::fabs(sum.bin[13].sinkSum - 2.0 * g.bin[13].sinkSum) < 1e-12, "F138: 10 s of bins are the seconds' merged");
        char tiny[24];
        const size_t cut = f138::binsText(g, tiny, sizeof(tiny));
        check(cut < sizeof(tiny) && std::strlen(tiny) == cut, "F138: the bins' text stops before the buffer's end");
        char mode[96];
        w.modeText(mode, sizeof(mode));
        check(std::string(mode).find("mixed (mouse look 4 calls and look to aim 1)") == 0, "F138: both modes in a second say so");
        f138::Window m0, m1;
        f138::Call a;
        a.out.pitched = true;
        m0.add(a);
        a.lookToAim = true;
        m1.add(a);
        m0.modeText(mode, sizeof(mode));
        const bool mouse = std::strcmp(mode, "mouse look") == 0;
        m1.modeText(mode, sizeof(mode));
        check(mouse && std::strcmp(mode, "look to aim") == 0, "F138: one mode reads as its name");
        check(m0.any() && !f138::Window{}.any(), "F138: a window with a pitched call has something to say, an empty one not");
        f138::Window r;
        f138::Out edge;
        edge.returned = true;
        r.addReturn(edge);
        edge.returned = false;
        edge.returnRefused = true;
        r.addReturn(edge);
        edge.returnRefused = false;
        edge.notNeeded = true;
        r.addReturn(edge);
        check(r.any() && r.returned == 1 && r.returnRefused == 1 && r.notNeeded == 1, "F138: a return alone makes the second worth a line, and 'not needed' is counted beside it");
        f138::Window nn;
        nn.addReturn(edge);
        check(nn.any() && nn.notNeeded == 1, "F138: an episode end that gave nothing back is still said");
        r.clear();
        check(!r.any() && r.calls == 0 && r.aimMin == 1e9, "F138: the window is cleared after its line");
    }
}

}  // namespace f138test
