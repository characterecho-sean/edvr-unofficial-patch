// F138 checks (native on-foot VR): the view holds its height over the neck while the look is pitched. Header only, run by native_frame_test (run138).
// Convention: G = the game's camera (raw) less the animated neck less the headset's LOCAL height; the sink is the stance's G less this call's, clamped to [-40, +40] cm; the
// raise's target while pitched is the stance's plus w times the sink, w = smoothstep(20, 30 deg, |aim pitch|) since F139 (it was 10 to 20), 0 for a look down under look to aim
// (as flown); the settle's refusal (so the unarmed reference's gate) is from 20 deg since F139, with nothing drawn; the instrument counts a call from 10 deg. The end of
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
    {   // the weight: smoothstep(20, 30 deg) of |pitch| (F139; it was 10 to 20), 0 for a pitch that is not a number
        check(f138::kWeightLoDeg == 20.0 && f138::kWeightHiDeg == 30.0 && f138::kSettleGateDeg == 20.0 && f138::kPitchedDeg == 10.0,
              "F139: the weight runs 20 to 30 deg, the settle's gate is 20 deg, the instrument still counts from 10");
        check(f138::pitchWeight(0.0) == 0.0 && f138::pitchWeight(10.0) == 0.0 && f138::pitchWeight(-11.5) == 0.0 && f138::pitchWeight(19.9) == 0.0 && f138::pitchWeight(20.0) == 0.0,
              "F138: no weight up to 20 deg (the pilot's resting mouse aim of -10.5 to -12 deg has none)");
        check(std::fabs(f138::pitchWeight(25.0) - 0.5) < 1e-12 && std::fabs(f138::pitchWeight(-25.0) - 0.5) < 1e-12, "F138: half way at 25 deg, up or down (the magnitude)");
        check(f138::pitchWeight(30.0) == 1.0 && f138::pitchWeight(65.0) == 1.0 && f138::pitchWeight(-70.0) == 1.0, "F138: the full weight from 30 deg");
        check(f138::pitchWeight(22.0) < f138::pitchWeight(24.0) && f138::pitchWeight(24.0) < f138::pitchWeight(28.0), "F138: the weight rises with the pitch");
        check(f138::pitchWeight(std::nan("")) == 0.0 && f138::pitchWeight(INFINITY) == 0.0 && f138::weight(std::nan(""), false) == 0.0 && f138::weight(std::nan(""), true) == 0.0,
              "F138: an aim pitch that is not a number has no weight");
        // the mode: looking up applies in both, looking down under mouse look only (under look to aim the raise stays held, as flown)
        check(f138::weight(-40.0, true) == 0.0 && f138::weight(40.0, true) == 1.0 && f138::weight(-40.0, false) == 1.0 && f138::weight(40.0, false) == 1.0,
              "F138: look to aim at -40 gives w 0, at +40 w 1; mouse look at -40 gives w 1");
        check(f138::weight(-10.5, true) == 0.0 && std::fabs(f138::weight(25.0, true) - 0.5) < 1e-12 && std::fabs(f138::weight(-25.0, false) - 0.5) < 1e-12 && f138::weight(0.0, true) == 0.0,
              "F138: the smoothstep is the same up in both modes and down under mouse look");
        // the instrument counts from 10 deg; the settle refuses from kSettleGateDeg (F139: 20 deg), the weight starts there too
        check(!f138::pitched(9.99, false) && f138::pitched(10.0, false) && f138::pitched(-10.0, false) && f138::pitched(-62.0, false) && !f138::pitched(std::nan(""), false),
              "F138: a pitched call (the instrument's) is |aim| of 10 deg or more");
        check(f138::pitched(40.0, true) && !f138::pitched(-40.0, true) && f138::pitched(-40.0, false), "F138: a look down under look to aim is not a pitched call (as flown)");
        const double gate = f138::kSettleGateDeg;
        check(!f138::settleRefused(19.99, gate, false) && f138::settleRefused(20.0, gate, false) && f138::settleRefused(-40.0, gate, false) && f138::settleRefused(70.0, gate, false) &&
                  !f138::settleRefused(std::nan(""), gate, false) && !f138::settleRefused(INFINITY, gate, false),
              "F138: the settle refuses a pitched aim under mouse look too: |pitch| of 20 deg or more, finite");
        check(!f138::settleRefused(-10.5, gate, false) && !f138::settleRefused(-12.0, gate, false) && !f138::settleRefused(-15.0, gate, false) && !f138::settleRefused(10.0, gate, false),
              "F139: the pilot's resting mouse aim (-10.5 to -12 deg, head level) is not refused: the unarmed settle, and so the reference, is taken there");
        check(f138::settleRefused(10.0, 10.0, false) && !f138::settleRefused(9.99, 10.0, false),
              "F138: the gate is the caller's number (look to aim's own F109 gate, 10 deg, lives in the hook and is not this function's)");
        check(!f138::settleRefused(-40.0, gate, true) && !f138::settleRefused(70.0, gate, true),
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
        const f138::Out half = t.step(pitchedCall(25.0, 0.10));
        check(half.reason == f138::kApplied && std::fabs(half.w - 0.5) < 1e-12 && std::fabs(half.publishM - (kTarget + 0.05)) < 1e-12 && std::fabs(half.addedM - 0.05) < 1e-12,
              "F138: at 25 deg half the sink is added");
        // F139: between 10 and 20 deg the call is the instrument's (pitched) but the weight is 0: considered nowhere, nothing published, no episode begun
        f138::Tracker lowT = stanced();
        const f138::Out low = lowT.step(pitchedCall(-11.5, 0.03));
        check(low.pitched && !low.considered && low.w == 0.0 && !low.publish && !low.sinkValid && !lowT.episode && !lowT.added,
              "F139: a resting mouse aim of -11.5 deg is a pitched call of the instrument with no weight: nothing is added and no episode begins");
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
        // the weight reaching 0 at 20 deg is the edge too (F139; it was 10)
        f138::Tracker u = stanced();
        u.step(pitchedCall(40.0, 0.10));
        const f138::Out ten = u.step(pitchedCall(20.0, 0.10));
        check(ten.returned && ten.pitched && ten.publishM == kTarget, "F138: at exactly 20 deg the weight is 0 and the stance's target returns");
        f138::Tracker u2 = stanced();
        u2.step(pitchedCall(40.0, 0.10));
        const f138::Out fifteen = u2.step(pitchedCall(15.0, 0.10));
        check(fifteen.returned && fifteen.pitched && !fifteen.considered && fifteen.publishM == kTarget, "F139: a look that comes down to 15 deg is below the weight: the stance's target returns");
        // nothing was added: no edge
        f138::Tracker n = stanced();
        const f138::Out none = n.step(pitchedCall(5.0, 0.0));
        check(!none.returned && !none.publish && !none.returnRefused && !none.notNeeded, "F138: a level look with no pitched episode before it publishes nothing and counts nothing");
        // an armed-only episode (an item drawn all through it: measured only, nothing published) gives nothing back at the edge
        f138::Tracker armedOnly = stanced();
        f138::In armedUp = pitchedCall(30.0, 0.10);
        armedUp.itemDrawn = true;
        const f138::Out armedPitched = armedOnly.step(armedUp);
        f138::In armedLevel = pitchedCall(5.0, 0.0);
        armedLevel.itemDrawn = true;
        const f138::Out armedEdge = armedOnly.step(armedLevel);
        check(armedPitched.reason == f138::kItemDrawn && !armedPitched.publish && !armedEdge.publish && !armedEdge.returned && !armedEdge.returnRefused && armedEdge.notNeeded &&
                  !armedOnly.step(armedLevel).notNeeded,
              "F138: an episode with an item drawn throughout publishes nothing at the edge: 'not needed', once (the armed target is an absolute height)");
        // drawing a weapon, aiming 30 deg down and coming level (mouse look) must not overwrite the armed raise with the unarmed stance's
        f138::Tracker draw = stanced();
        f138::In drawDown = pitchedCall(-30.0, -0.05);
        drawDown.itemDrawn = true;
        draw.step(drawDown);
        check(!draw.step(armedLevel).publish, "F138: weapon drawn, aim 30 deg down, level again: the unarmed stance's target is not published over the armed raise");
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
        // F139: from 10 to 20 deg the call is counted by the instrument but has no weight, so no reason either
        const f138::Out resting = reasonOf(have, pitchedCall(-12.0, 0.03));
        check(resting.pitched && !resting.considered && !resting.publish && resting.reason == f138::kApplied && resting.w == 0.0, "F139: a pitch of -12 deg is pitched but not considered");
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
        double aims[4] = {25.0, 40.0, 40.0, -25.0};
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
        check(w.aimMin == -25.0 && w.aimMax == 40.0 && w.settleRefusedAim == 3 && w.insideOf == 4 && w.inside == 1, "F138: the aim's range, the settle refusals, INSIDE 1 of 4");
        // the bins: 40 deg is [40,50), 25 is [20,30), -25 is [-30,-20), -20 is [-20,-10), 0 is [0,10)
        check(f138::binOf(-90.0) == 0 && f138::binOf(-80.0) == 1 && f138::binOf(-0.1) == 8 && f138::binOf(0.0) == 9 && f138::binOf(10.0) == 10 && f138::binOf(89.9) == 17 &&
                  f138::binOf(90.0) == 17 && f138::binOf(-120.0) == 0 && f138::binOf(40.0) == 13 && f138::binOf(25.0) == 11 && f138::binOf(-20.0) == 7,
              "F138: the bins are 10 deg wide from -90 to +90");
        const f138::Group& g = w.group[0][0];
        check(g.bin[13].n == 2 && g.bin[11].n == 1 && w.group[0][1].bin[13].n == 1 && w.group[1][0].bin[6].n == 1, "F138: calls land by mode and item class");
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

    // ---- F170: the running reference, for a look-up before the first settle ---------------------------------------------------------------------------------------------------------------
    // The flight edvr_gfx_20261028_215408: holstered at 21:58:14, the item flag cleared in the second 21:58:21, the pilot looked up through 20 deg in that second and held +85 deg to 21:58:39,
    // and the first unarmed settle of the flight came at 21:58:40: no reference on 1533 calls, INSIDE 90 of 90. The running reference is taken from every level call with nothing drawn.
    {
        // a level stretch of n calls at 90 Hz from t0, the held target and G of the stance (G = 0.16)
        const auto level = [](f138::Tracker& t, double t0, int n, double target = kTarget, uint32_t gen = static_cast<uint32_t>(kGen), double sinkM = 0.0, double neck = kNeck) {
            for (int i = 0; i < n; ++i) t.noteRunning(t0 + i / 90.0, target, kEye - sinkM, neck, kHead, gen, true);
            return t0 + n / 90.0;
        };
        // 1. no settle ever, 3 level calls (the three the flight's second 21:58:21 counted at +11 to +15 deg), then the look straight up: the sink is MEASURED on the running reference and nothing
        // is published (F171: the flight edvr_gfx_20261029_043951 ran the raise to its clamp on a running reference's sink)
        f138::Tracker t;
        const double tEnd = level(t, 100.0, 3);
        check(t.run.have && !t.ref.have && std::fabs(t.run.g - 0.16) < 1e-12 && std::fabs(t.run.target - kTarget) < 1e-12 && t.run.generation == 3, "F170: three level calls with nothing drawn are a running reference: G 0.16, the target in force");
        (void)tEnd;
        const f138::Out up = t.step(pitchedCall(60.0, 0.086));
        check(up.reached && up.source == f138::kSrcRunning && up.reason == f138::kRunningMeasured && !up.publish && up.sinkValid && std::fabs(up.sinkM - 0.086) < 1e-12 && up.addedM == 0.0 && !t.added,
              "F171: a look-up straight after a holster at level, with no settle, measures the sink (8.6 cm) on the running reference and publishes nothing: its own reason, 'running reference, measured only', and the tracker has added nothing");
        check(std::strcmp(f138::reasonName(f138::kRunningMeasured), "running reference, measured only") == 0 && f138::kReasons == 11 && f138::kRunningMeasured == 10,
              "F171: the reason is named 'running reference, measured only'");
        // the settled path is as it was: the same look-up on a settled reference adds the sink and publishes the target plus it
        f138::Tracker settledOnly = stanced();
        const f138::Out settledUp = settledOnly.step(pitchedCall(60.0, 0.086));
        check(settledUp.source == f138::kSrcSettled && settledUp.reason == f138::kApplied && settledUp.publish && std::fabs(settledUp.publishM - (kTarget + 0.086)) < 1e-12 && std::fabs(settledUp.addedM - 0.086) < 1e-12 &&
                  settledOnly.added,
              "F171: the settled reference is unchanged: it adds the sink (8.6 cm) and publishes the target plus it");
        // a running-only episode ends with nothing to give back (it published nothing)
        const f138::Out runningEnd = t.step(pitchedCall(5.0, 0.0));
        check(!runningEnd.publish && !runningEnd.returned && !runningEnd.returnRefused && runningEnd.notNeeded, "F171: and its end gives nothing back: not needed");
        // the same look-up with no level call at all has nothing to measure against (no default reference is built): refused, source none
        f138::Tracker bare;
        const f138::Out blind = bare.step(pitchedCall(60.0, 0.086));
        check(blind.reached && blind.source == f138::kSrcNone && blind.reason == f138::kNoReference && !blind.publish && !blind.sinkValid,
              "F170: a look-up with no level call before it has no reference: refused as 'no reference', source none (the line says so)");
        // 2. the settled reference wins when there is one (here a different stance, G 0.20, target 0.30), and the sink and the target come from it
        f138::Tracker both;
        level(both, 100.0, 30, 0.05);
        both.noteStance(0.30, 1.50, kNeck, kHead, static_cast<uint32_t>(kGen), true);   // G = 1.50 - 1.30 = 0.20
        check(both.ref.have && both.run.have, "F170: both references exist");
        const f138::Out wins = both.step(pitchedCall(60.0, 0.10));   // G now 0.06: against the settled G 0.20 the sink is 0.14; against the running 0.16 it would be 0.10
        check(wins.source == f138::kSrcSettled && wins.reason == f138::kApplied && std::fabs(wins.sinkM - 0.14) < 1e-12 && std::fabs(wins.publishM - (0.30 + 0.14)) < 1e-12,
              "F170: the settled reference takes priority when it is usable: its G and its target, not the running one's");
        // 3. a settled reference that cannot be used (a recentre moved the LOCAL generation) gives way to a running one in the new frame
        f138::Tracker moved = stanced();
        level(moved, 100.0, 30, kTarget, 4u);
        const f138::Out recentred = moved.step([&] { f138::In in = pitchedCall(60.0, 0.08); in.generation = 4; return in; }());
        check(recentred.source == f138::kSrcRunning && recentred.reason == f138::kRunningMeasured && !recentred.publish && std::fabs(recentred.sinkM - 0.08) < 1e-12,
              "F170: after a recentre the settled reference is refused (another LOCAL frame) and the running one, taken in the new one, stands in (F171: for the measurement; it publishes nothing)");
        f138::Tracker movedOnly = stanced();
        const f138::Out stale = movedOnly.step([&] { f138::In in = pitchedCall(60.0, 0.08); in.generation = 4; return in; }());
        check(stale.reason == f138::kGeneration && stale.source == f138::kSrcNone && !stale.publish, "F170: with no running reference the refusal is the old one (the LOCAL pose's generation changed)");
        // 4. the smoothing: G steps from 0.16 to 0.10 (the camera 6 cm down against the neck); 0.3 s later the running G is 95% of the way, 0.1 s in it is 63%
        f138::Tracker sm;
        double ts = level(sm, 100.0, 90);   // one second at G 0.16
        const double t0 = ts;
        for (int i = 0; i < 27; ++i) sm.noteRunning(t0 + i / 90.0, kTarget, kEye - 0.06, kNeck, kHead, 3u, true);
        check(std::fabs(sm.run.g - (0.10 + 0.06 * std::exp(-3.0))) < 1e-9 && sm.run.g > 0.10 && sm.run.g < 0.16, "F170: the running G follows a step in 0.3 s to 95% (the residual is exp(-3) of the step)");
        f138::Tracker sm2;
        ts = level(sm2, 100.0, 90);
        for (int i = 0; i < 9; ++i) sm2.noteRunning(ts + i / 90.0, kTarget, kEye - 0.06, kNeck, kHead, 3u, true);
        check(std::fabs(sm2.run.g - (0.10 + 0.06 * std::exp(-1.0))) < 1e-9, "F170: and 63% of it in 0.1 s: one first-order filter whose 95% time is f138::kRunSmoothS");
        // the target is smoothed with it
        f138::Tracker sm3;
        ts = level(sm3, 100.0, 90, 0.10);
        for (int i = 0; i < 27; ++i) sm3.noteRunning(ts + i / 90.0, 0.20, kEye, kNeck, kHead, 3u, true);
        check(std::fabs(sm3.run.target - (0.20 - 0.10 * std::exp(-3.0))) < 1e-9, "F170: the target in force is smoothed over the same 0.3 s");
        // 5. the stance guard: the animated neck 5 cm away (a crouch, the fists' hunch) starts the reference over from that sample, not a blend
        f138::Tracker guard;
        level(guard, 100.0, 30);
        guard.noteRunning(101.0, 0.50, kEye - 0.30, kNeck - 0.05, kHead, 3u, true);
        check(std::fabs(guard.run.g - (1.46 - 0.30 - (1.30 - 0.05))) < 1e-12 && std::fabs(guard.run.target - 0.50) < 1e-12 && std::fabs(guard.run.neckY - (kNeck - 0.05)) < 1e-12,
              "F170: a neck more than 3 cm from the reference's starts it over from that call (the same stance guard as the settled one)");
        f138::Tracker guard2;
        level(guard2, 100.0, 30);
        guard2.noteRunning(101.0, kTarget, kEye, kNeck + 0.029, kHead, 3u, true);
        check(guard2.run.neckY > kNeck && guard2.run.neckY < kNeck + 0.029, "F170: 2.9 cm is the same stance: smoothed, not restarted");
        // a new LOCAL generation, a gap of a second or more and a pose that is not valid start it over, or drop it
        f138::Tracker gen;
        level(gen, 100.0, 30);
        gen.noteRunning(101.0, 0.40, kEye, kNeck, kHead, 4u, true);
        check(gen.run.generation == 4 && std::fabs(gen.run.target - 0.40) < 1e-12, "F170: a new LOCAL generation starts the reference over");
        f138::Tracker gap;
        level(gap, 100.0, 30);
        gap.noteRunning(105.0, 0.40, kEye, kNeck, kHead, 3u, true);
        check(std::fabs(gap.run.target - 0.40) < 1e-12, "F170: a gap longer than a second starts it over (the call stream paused)");
        f138::Tracker bad;
        level(bad, 100.0, 30);
        bad.noteRunning(101.0, kTarget, kEye, kNeck, kHead, 3u, false);
        check(!bad.run.have, "F170: a pose that is not valid drops the running reference (a stale one is not used)");
        f138::Tracker nan;
        level(nan, 100.0, 30);
        const double gBefore = nan.run.g;
        nan.noteRunning(101.0, kTarget, std::nan(""), kNeck, kHead, 3u, true);
        check(nan.run.have && nan.run.g == gBefore, "F170: a call with a camera height that is not a number is skipped, the reference stays");
        f138::Tracker same;
        level(same, 100.0, 30);
        const double sameG = same.run.g;
        same.noteRunning(100.0 + 29.0 / 90.0, kTarget, kEye - 0.5, kNeck, kHead, 3u, true);
        check(same.run.g == sameG, "F170: two samples at one instant do not move it");
        // 6. nothing is taken while this tracker's own sunk target is in force (F171: only the settled reference publishes one now; the running reference beside it still stands aside)
        f138::Tracker inForce = stanced();
        level(inForce, 100.0, 30);
        inForce.step(pitchedCall(60.0, 0.086));
        check(inForce.added, "F170: the sink was added (from the settled reference)");
        const double targetBefore = inForce.run.target;
        inForce.noteRunning(102.0, kTarget + 0.086, kEye, kNeck, kHead, 3u, true);
        check(inForce.run.target == targetBefore, "F170: the running reference does not take the sunk target it published itself");
        // 7. the end of the episode gives the stance's target back, once
        const f138::Out edge = inForce.step(pitchedCall(5.0, 0.0));
        check(edge.returned && edge.publish && std::fabs(edge.publishM - kTarget) < 1e-12 && !inForce.step(pitchedCall(5.0, 0.0)).publish,
              "F170: back to a level look the stance's target (the one in force before the look) is published once");
        // 8. the reset (the on-foot camera's edge) clears it
        t.reset();
        check(!t.run.have && !t.ref.have && t.runT == 0.0, "F170: the reset clears the running reference with the settled one");
        // 9. with an item drawn the sink is measured on the running reference and nothing is published
        f138::Tracker armed;
        level(armed, 100.0, 30);
        f138::In ai = pitchedCall(60.0, 0.09);
        ai.itemDrawn = true;
        const f138::Out am = armed.step(ai);
        check(am.reason == f138::kItemDrawn && am.source == f138::kSrcRunning && am.sinkValid && !am.publish, "F170: an item drawn: measured on the running reference, nothing published");
    }
    {   // the window counts the references used and names them
        f138::Tracker t;
        for (int i = 0; i < 3; ++i) t.noteRunning(100.0 + i / 90.0, kTarget, kEye, kNeck, kHead, 3u, true);
        f138::Window w;
        for (int i = 0; i < 5; ++i) {
            f138::Call c;
            c.aimDeg = 60.0;
            c.out = t.step(pitchedCall(60.0, 0.05));
            w.add(c);
        }
        f138::Tracker none;
        for (int i = 0; i < 2; ++i) {
            f138::Call c;
            c.aimDeg = 60.0;
            c.out = none.step(pitchedCall(60.0, 0.05));
            w.add(c);
        }
        f138::Tracker away;
        {
            f138::Call c;
            c.aimDeg = 60.0;
            f138::In in = pitchedCall(60.0, 0.05);
            in.cameraAway = true;
            c.out = away.step(in);   // a camera away: no reference chosen, counted nowhere
            w.add(c);
        }
        char text[96];
        f138::referenceText(w.srcN, text, sizeof(text));
        check(w.srcN[f138::kSrcRunning] == 5 && w.srcN[f138::kSrcNone] == 2 && w.srcN[f138::kSrcSettled] == 0 && w.calls == 8 && std::strcmp(text, "settled 0, running 5, none 2") == 0,
              "F170: the window counts the considered calls that chose a reference by the one they used; the line reads 'settled 0, running 5, none 2' (a camera away chose none)");
        f138::Window empty;
        f138::referenceText(empty.srcN, text, sizeof(text));
        check(std::strcmp(text, "settled 0, running 0, none 0") == 0 && std::strcmp(f138::sourceName(f138::kSrcSettled), "settled") == 0 && std::strcmp(f138::sourceName(f138::kSrcRunning), "running") == 0 &&
                  std::strcmp(f138::sourceName(f138::kSrcNone), "none") == 0,
              "F170: an empty window reads zeros; the three sources are named settled, running, none");
    }

    // ---- F171: the running reference measures only; the reference's figures on the line -----------------------------------------------------------------------------------------------------
    // The flight edvr_gfx_20261029_043951, 04:44:25 to 04:44:35: unarmed, mouse look, aim +52 to +85 deg, the reference used "running" on every applied call, the sink -25.9 to -40.0 cm (negative),
    // the raise it added ran to -50 cm on 90 of 90 ticks. The window says why nothing was added, and what the reference was.
    {
        f138::Tracker t;
        for (int i = 0; i < 3; ++i) t.noteRunning(100.0 + i / 90.0, kTarget, kEye, kNeck, kHead, 3u, true);
        f138::Window w;
        for (int i = 0; i < 6; ++i) {
            f138::Call c;
            c.aimDeg = 60.0;
            f138::In in = pitchedCall(60.0, 0.05);
            in.tS = 102.0 + i / 90.0;
            c.out = t.step(in);
            w.add(c);
        }
        char why[260];
        f138::notAppliedText(w.why, why, sizeof(why));
        check(w.appliedN == 0 && w.why[f138::kRunningMeasured] == 6 && w.consideredN == 6 && w.sinkN == 6 && w.pubN == 0 && std::strcmp(why, "running reference, measured only 6") == 0,
              "F171: six pitched calls on the running reference are considered, measured (the sink is in the window), added none and published none; the line's not-applied list names 'running reference, measured only 6'");
        // the reference's figures: the running one started over at 100.0 (the first sample), so its age at 102.0 is 2.0 s, its target 12 cm, its G 16 cm; G now 11 cm
        char detail[240];
        f138::referenceDetailText(w, detail, sizeof(detail));
        check(w.refN == 6 && std::strcmp(f138::sourceName(w.refSrcLast), "running") == 0 &&
                  std::strstr(detail, "reference detail (F171; the calls that had one 6): last source running, age 2.0 s, target +12.0 cm, G +16.0 cm, G now +11.0 cm; target published +0.0 cm in 0 calls") != nullptr,
              "F171: the line gives the reference's source, age (s since it started over), target and G, G now, and the mean target published (none: +0.0 cm in 0 calls)");
        // a settled reference taken at 100.0 and used at 103.5: age 3.5 s, and it publishes: the mean target published is its target plus the sink
        f138::Tracker s;
        s.noteStance(0.10, kEye, kNeck, kHead, static_cast<uint32_t>(kGen), true, 100.0);
        f138::Window ws;
        for (int i = 0; i < 4; ++i) {
            f138::Call c;
            c.aimDeg = 60.0;
            f138::In in = pitchedCall(60.0, 0.04);
            in.tS = 103.5;
            c.out = s.step(in);
            ws.add(c);
        }
        f138::referenceDetailText(ws, detail, sizeof(detail));
        check(ws.appliedN == 4 && ws.pubN == 4 && std::strstr(detail, "last source settled, age 3.5 s, target +10.0 cm, G +16.0 cm, G now +12.0 cm; target published +14.0 cm in 4 calls") != nullptr,
              "F171: a settled reference taken at 100.0 and used at 103.5 reads age 3.5 s, and what it published is its target plus the 4 cm sink: +14.0 cm in 4 calls");
        // a tracker with no time: the age is unknown, counted for none (0.0 s on the line, 0 of the calls it is a mean of)
        f138::Tracker noTime = stanced();
        f138::Window wn;
        f138::Call cn;
        cn.aimDeg = 60.0;
        cn.out = noTime.step(pitchedCall(60.0, 0.04));
        wn.add(cn);
        check(wn.refN == 1 && wn.refAgeN == 0 && std::isnan(cn.out.refAgeS), "F171: without a time the age is not a number and is not counted");
        // no reference at all: the detail reads zeros and source none
        f138::Window wz;
        f138::referenceDetailText(wz, detail, sizeof(detail));
        check(std::strstr(detail, "the calls that had one 0): last source none, age 0.0 s, target +0.0 cm, G +0.0 cm, G now +0.0 cm; target published +0.0 cm in 0 calls") != nullptr, "F171: an empty window reads zeros and source none");
        // the whole line, at its worst, inside the log's 1200 bytes
        f138::Window big;
        for (int i = 0; i < 90; ++i) {
            f138::Call c;
            c.aimDeg = i % 2 ? 85.0 : -85.0;
            c.lookToAim = i % 3 == 0;
            c.itemDrawn = i % 5 == 0;
            c.headValid = true;
            c.headDeg = -89.9;
            c.carriedUpCm = -50.0;
            c.carriedFwdCm = -50.0;
            c.viewOverNeckCm = -50.0;
            c.out.pitched = true;
            c.out.considered = true;
            c.out.w = 1.0;
            c.out.reason = i % 11;
            c.out.reached = true;
            c.out.source = i % 3;
            c.out.sinkValid = true;
            c.out.sinkM = -0.40;
            c.out.headRiseM = -0.40;
            c.out.refValid = true;
            c.out.refAgeS = 99999.9;
            c.out.refTargetM = -0.50;
            c.out.refGM = -0.50;
            c.out.gNowM = -0.50;
            c.out.publish = true;
            c.out.publishM = -0.50;
            c.out.addedM = -0.40;
            big.add(c);
        }
        big.returned = big.returnRefused = big.notNeeded = 99;
        char line[1300];
        const int len = f138::formatPitched(line, sizeof(line), big, "$wpn_m_shotgun_plasma_doublebarrel_name_with_a_long_tail in 18 of 90 calls", 99);
        // a realistic second: one reason, the running reference, a rifle's name
        char typical[1300];
        const int typicalLen = f138::formatPitched(typical, sizeof(typical), w, "none", 0);
        char msg[300];
        std::snprintf(msg, sizeof(msg), "F171: the 1 s line is inside the log's 1166 bytes (1200 less its stamp and truncation marker) with every figure at its worst (%d bytes; a typical second %d)", len, typicalLen);
        check(len > 0 && len < 1166 && typicalLen > 0 && typicalLen < 1000 && std::strstr(typical, "not applied: running reference, measured only 6; reference used (F170; the calls that chose one): settled 0, running 6, none 0; reference detail (F171;") != nullptr &&
                  std::strstr(typical, "; EDVR carried up ") != nullptr,
              msg);
    }

    // ---- F170: the window is not read twice ------------------------------------------------------------------------------------------------------------------------------------------------
    // The flight edvr_gfx_20261028_215408, 21:58:40 to 21:59:00: every F138 1 s line was the same ("aim pitch +56.3 ... 0 of 29 pitched calls | not applied: no reference 27") while the pilot looked
    // level. The frame thread asked the hook to clear the window after each take, and the hook cleared it at its next write, which only a pitched call makes.
    {
        f138::Mailbox box;
        const auto writeOne = [&](double aim) {
            box.write([&](f138::Window& win) {
                f138::Call c;
                c.aimDeg = aim;
                c.out.pitched = true;
                win.add(c);
            });
        };
        f138::Window got;
        check(box.take(&got) && !got.any(), "F170: a take before any write reads an empty window");
        for (int i = 0; i < 29; ++i) writeOne(56.0);
        check(box.take(&got) && got.calls == 29 && got.any(), "F170: a take after 29 pitched calls reads 29");
        // the pilot looks level: the hook writes nothing; the next 20 takes (20 s) read nothing, where the old window was printed 20 times
        int printed = 0;
        for (int s = 0; s < 20; ++s) printed += box.take(&got) && got.any() ? 1 : 0;
        check(printed == 0 && got.calls == 0 && got.aimMin == 1e9, "F170: twenty seconds with no pitched call print nothing (the old window was read again each second)");
        // a pitched call again: only that call is read, not the 29 before it
        writeOne(40.0);
        check(box.take(&got) && got.calls == 1 && got.aimMin == 40.0 && got.aimMax == 40.0, "F170: the next pitched call reads alone, without the window it follows");
        check(box.take(&got) && !got.any(), "F170: and the second after it reads empty again");
        // a second with writes between two quiet ones
        for (int i = 0; i < 3; ++i) writeOne(30.0);
        const bool first = box.take(&got) && got.calls == 3;
        const bool quiet = box.take(&got) && !got.any();
        writeOne(35.0);
        writeOne(35.0);
        const bool again = box.take(&got) && got.calls == 2 && got.aimMin == 35.0;
        check(first && quiet && again, "F170: writes, a quiet second, writes: 3, nothing, 2 (each second its own)");
    }
}

}  // namespace f138test
