// Build gate for transition_flash_prevent_core.h's pure logic: the
// classifier (and that it truly ignores the padding lanes, not merely
// documents that it should), the validation and per-parent guard
// arithmetic (including LRU eviction), event grouping with the alternate
// latch, the ring window test's overflow safety, and the four 2026-09-23
// review findings whose logic lives here (finding 1: the per-call act gate;
// finding 2: H3's recent-pushes match and frame bucketing; finding 3: the
// acted-frame dedup; finding 4: dump-request window folding). No game, no
// Windows hook -- this drives the header directly, the kinematic_probe_test
// pattern (single-TU, production source compiled into the rig).
//
// Also covers pose_reader_watch_core.h's pure logic (advanced.eye_origin_
// readers, the same design doc's parts A2/B): Dr7's slot-0 composition,
// stack-range classification, the 60-frame stability gate, the unique-
// reader table's dedupe/cap arithmetic, and the dump-trigger filter that
// narrows eye_origin_trace's own dump condition when this instrument is on.
// Same rig rather than a second one -- both headers describe the one design
// doc and neither needs the game.
//
// The 2026-09-24 revision of transition_flash_eye_base_core.h (flight
// 062910: the F stand-in never matched a refilled mailbox) adds three more
// cells here: CHANGE 1's held-base guard (every refusal reason, flipped one
// at a time), CHANGE 2's consecutive-refilled counter that gates the writer
// watch alongside the existing stability gate, and CHANGE 3's dump-trigger
// predicate, which takes no eye-trace flag at all.
//
// The same day's static round 7 (the writer FUN_142874b20 and its caller,
// the controller FUN_1410730a0 -- design doc "Static round 7: the writer and
// its gates") adds four more: CHANGE 5's Dr7 slot-1 EXECUTE composition
// (armed/disarmed, leaving slot 0 untouched either way) and the writer's own
// body-extent classifier; CHANGE 6's offeredSubstituteUsable, the policy that
// prefers the writer's OFFERED matrix over the held base when it was entered
// for our ship, refused by its name gate, and still fresh; and CHANGE 7's
// per-frame classifier, which tells apart "the controller didn't run" from
// "it ran but the writer wasn't entered" from "entered but didn't write".
//
// The task of 2026-09-24 ("dump triggers become mode-switch edges") adds a
// last one: CHANGE 9's classifyModeSwitchEdge/updateConsecutiveUnrefilled,
// which turn "every un-refilled call dumps" (flight 091726: 5,041 of them in
// one low wake) into ENTRY/EXIT edges, plus glitch_scene.h's own fresh/
// decision print text for the per-frame dump row those edges feed.
//
// The same day's "render-time patch simulation (watch-only)" adds CHANGE 10's
// cells: patchEyeOrigin (the eye composer's own multiply, checked against
// the f13550 flight-100043 mailbox/scene numbers and a hand-computed
// rotation), premul4x4 (the acting build's full premultiply, hand-composed
// B*M and both identity sides), patchSceneChoice on the flight-derived
// cam/pool points plus every band edge, and the pending sim's N+1..N+3
// render-frame window. CHANGE 13 (the live-mailbox tap) adds mailboxPlausible
// -- the sanity gate a tap-time live read passes through -- checked against
// the measured tunnel base, a measured 5 km rebase, and the over-range and
// non-finite rejections.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include "../../src/d3d11/transition_flash_prevent_core.h"
#include "../../src/d3d11/pose_reader_watch_core.h"
#include "../../src/d3d11/transition_flash_eye_base_core.h"
#include "../../src/d3d11/glitch_scene.h"

using namespace edvr::tfp;
namespace prw = edvr::prw;
namespace tfeb = edvr::tfeb;

namespace {
unsigned checks = 0, failures = 0;
void check(bool ok, const char* name) {
    ++checks;
    if (!ok) { ++failures; std::fprintf(stderr, "FAIL: %s\n", name); }
}

// A pose as FUN_3CEE650's identity init leaves it: rotation is the 3x3
// identity at [r*4+c], translation at [12..14], padding [3],[7],[11],[15]
// left as whatever the caller's stack held -- here, deliberately different
// junk each side, since a comparison that touched them would disagree.
void identityPose(double p[16]) {
    for (int i = 0; i < 16; ++i) p[i] = 0.0;
    p[0] = p[5] = p[10] = 1.0;
    p[15] = 1.0;  // conventionally 1, but classify() must not depend on it
}

// --- Classifier ------------------------------------------------------------

void caseClassifyIgnoresPadding() {
    double cached[16], recompute[16];
    identityPose(cached);
    identityPose(recompute);
    // Identical real data, wildly different padding.
    cached[3] = 1e9; cached[7] = -1e9; cached[11] = 42.0; cached[15] = 7.0;
    recompute[3] = -3e8; recompute[7] = 2.5e8; recompute[11] = -99.0; recompute[15] = 0.0;
    const PoseDelta d = comparePose(cached, recompute);
    check(d.dt == 0.0 && d.dr == 0.0, "classify: padding lanes contribute nothing to dt/dr");
    check(classify(d) == PoseClass::Agree, "classify: identical real data agrees despite padding");
}

void caseClassifyThresholds() {
    double cached[16], recompute[16];
    identityPose(cached); identityPose(recompute);
    // Translation just inside "agree" (dt < 1e-3).
    recompute[12] = cached[12] + 0.0005;
    check(classify(comparePose(cached, recompute)) == PoseClass::Agree,
          "classify: 0.5mm translation agrees");
    // Translation in "near" (1e-3 < dt < 0.05).
    identityPose(recompute);
    recompute[12] = cached[12] + 0.01;
    check(classify(comparePose(cached, recompute)) == PoseClass::Near,
          "classify: 1cm translation is near, not agree or disagree");
    // Translation past "disagree" (dt > 0.05).
    identityPose(recompute);
    recompute[13] = cached[13] + 0.2;
    check(classify(comparePose(cached, recompute)) == PoseClass::Disagree,
          "classify: 20cm translation disagrees");
    // Rotation alone can disagree even with translation untouched.
    identityPose(recompute);
    recompute[0] = cached[0] + 0.01;  // dr > 1e-3
    const PoseDelta rotOnly = comparePose(cached, recompute);
    check(rotOnly.dt == 0.0 && classify(rotOnly) == PoseClass::Disagree,
          "classify: rotation-only delta can disagree on its own");
}

// --- Validation --------------------------------------------------------

void caseValidation() {
    check(!isValidated({0, 0}), "validated: no data at all");
    check(!isValidated({59, 0}), "validated: one short of 60 agreements");
    check(isValidated({60, 0}), "validated: 60 agreements, no disagreements");
    check(isValidated({96, 4}), "validated: 4/100 disagree (4%) passes");
    check(!isValidated({95, 5}), "validated: exactly 5% disagree does not pass (under 5%, not at)");
    check(isValidated({1901, 99}), "validated: 99/2000 (4.95%) at scale passes");
    check(!isValidated({1900, 100}), "validated: exactly 100/2000 (5%) at scale still fails");
}

// --- Per-parent guard table ----------------------------------------------

void caseGuardNewAndRecentAgree() {
    ParentGuardTable t;
    // A brand new parent is eligible on its very first call -- the
    // realistic H1 case: a freshly re-targeted parent's first sighting.
    ParentGuardTable::Verdict v = t.onClassified(0x1000, 100, PoseClass::Disagree);
    check(v.eligible, "guard: a brand-new parent is eligible coming into its first disagree");
    // Once it has disagreed without agreeing or going quiet, it is neither
    // "new" nor "recently agreed" -- the (new OR recently-agreed) clause
    // fails regardless of how short the streak still is. One free look per
    // appearance, then it must agree again before another act is
    // considered; this is what keeps the event latch (which only consults
    // eligibility once, at the event's first disagreement) from being
    // fooled by a parent whose FIRST sighting happened to be a false one.
    v = t.onClassified(0x1000, 101, PoseClass::Disagree);
    check(!v.eligible, "guard: still disagreeing next frame -- neither new nor recently agreed");
    v = t.onClassified(0x1000, 102, PoseClass::Disagree);
    check(!v.eligible, "guard: same, streak 2 coming in");
    v = t.onClassified(0x1000, 103, PoseClass::Disagree);
    check(!v.eligible, "guard: same, streak 3 coming in");
    check(v.justCrossedPersistent, "guard: this 4th consecutive disagree crosses kMaxStreak once");
    v = t.onClassified(0x1000, 104, PoseClass::Disagree);
    check(!v.eligible, "guard: streak 4 coming in stays ineligible");
    check(!v.justCrossedPersistent, "guard: the persistent line does not repeat every frame");
    // An agreement resets the streak AND makes the parent "recently
    // agreed", re-arming eligibility on the very next disagreement.
    t.onClassified(0x1000, 105, PoseClass::Agree);
    v = t.onClassified(0x1000, 106, PoseClass::Disagree);
    check(v.eligible, "guard: an agreement resets the streak, re-arming eligibility");
}

void caseGuardStreakStillGatesWithinAgreedRecently() {
    // The streak test is not redundant with (new OR recently-agreed): a
    // burst of disagreeing calls packed into frames still inside the
    // 3-frame "recently agreed" window can push the streak entering a
    // later call past kMaxStreak, and that must still read ineligible. A
    // synthetic multi-call burst at one frame, not a literal per-eye
    // count -- it isolates the arithmetic rather than claiming this many
    // calls land in a single frame in the field.
    ParentGuardTable t;
    t.onClassified(0x4000, 100, PoseClass::Agree);
    t.onClassified(0x4000, 101, PoseClass::Disagree);  // entering streak 0, exits 1
    t.onClassified(0x4000, 101, PoseClass::Disagree);  // entering streak 1, exits 2
    t.onClassified(0x4000, 101, PoseClass::Disagree);  // entering streak 2, exits 3
    t.onClassified(0x4000, 101, PoseClass::Disagree);  // entering streak 3, exits 4
    ParentGuardTable::Verdict v = t.onClassified(0x4000, 101, PoseClass::Disagree);  // entering streak 4
    check((101 - 100) <= ParentGuardTable::kRecentAgreeFrames,
          "guard: precondition -- frame 101 is still within 3 frames of the frame-100 agreement");
    check(!v.eligible, "guard: streak over the cap stays ineligible even inside the agreed-recently window");
}

void caseGuardUnseenAndStale() {
    ParentGuardTable t;
    t.onClassified(0x2000, 100, PoseClass::Agree);
    // 3 frames later, still within the "agreed recently" window.
    ParentGuardTable::Verdict v = t.onClassified(0x2000, 103, PoseClass::Disagree);
    check(v.eligible, "guard: agreed 3 frames ago is still 'recently'");
    // Far later (well past the 90-frame unseen threshold) with no
    // intervening agreement: eligible again because it counts as new.
    ParentGuardTable t2;
    t2.onClassified(0x3000, 100, PoseClass::Disagree);
    t2.onClassified(0x3000, 101, PoseClass::Disagree);
    t2.onClassified(0x3000, 102, PoseClass::Disagree);
    t2.onClassified(0x3000, 103, PoseClass::Disagree);  // streak now 4: not eligible
    ParentGuardTable::Verdict stale = t2.onClassified(0x3000, 103 + 91, PoseClass::Disagree);
    check(stale.eligible, "guard: unseen for over 90 frames counts as new again despite the streak");
}

void caseGuardLruEviction() {
    ParentGuardTable t;
    // Fill one past capacity, each a distinct parent touched once, in
    // increasing frame order -- parent 0 is the least recently touched and
    // must be the one evicted.
    for (uint32_t i = 0; i < ParentGuardTable::kCapacity + 1; ++i) {
        t.onClassified(0x10000 + i, i, PoseClass::Agree);
    }
    check(t.sizeForTest() == ParentGuardTable::kCapacity,
          "guard: the table never grows past its capacity");
    // Parent 0 was evicted; querying it again must behave like a fresh
    // entry (eligible, no memory of the earlier agreement) rather than
    // "agreed recently" against a frame number from a lifetime ago.
    ParentGuardTable::Verdict v =
        t.onClassified(0x10000, ParentGuardTable::kCapacity + 500, PoseClass::Disagree);
    check(v.eligible, "guard: an evicted parent returns as new, not stale-agreed");
}

// --- Mode / session cap ------------------------------------------------

void caseModeAndCap() {
    bool recognized = false;
    check(parseMode("OFF", &recognized) == Mode::Off && recognized, "parseMode: case-insensitive off");
    check(parseMode("Watch", &recognized) == Mode::Watch && recognized, "parseMode: case-insensitive watch");
    check(parseMode("ALTERNATE", &recognized) == Mode::Alternate && recognized, "parseMode: alternate");
    check(parseMode("banana", &recognized) == Mode::Off && !recognized,
          "parseMode: unrecognised text falls back to off and says so");

    check(modeAllowsActing(Mode::On, 1), "mode: on allows every event");
    check(!modeAllowsActing(Mode::Watch, 2), "mode: watch never allows acting");
    check(!modeAllowsActing(Mode::Alternate, 1), "mode: alternate's first event (odd) is watched");
    check(modeAllowsActing(Mode::Alternate, 2), "mode: alternate's second event (even) acts");
    check(!modeAllowsActing(Mode::Alternate, 3), "mode: alternate's third event (odd) is watched again");

    check(!sessionCapReached(199), "cap: 199 acted frames has not reached 200");
    check(sessionCapReached(200), "cap: exactly 200 has reached the cap");
    check(sessionCapReached(500), "cap: past the cap stays reached");
}

// --- Event grouping and the alternate latch -------------------------------

void caseEventGrouping() {
    // The gap is measured from the LAST call to note(), whether or not that
    // call itself started a new event -- "the previous one" in the design
    // is the previous disagreement, and note() is only ever called on one.
    EventTracker t;
    const uint32_t f1 = 1000;
    EventTracker::Note n = t.note(f1);
    check(n.eventNumber == 1 && n.isNewEvent, "events: the first disagreement starts event 1");

    const uint32_t f2 = f1 + 5;
    n = t.note(f2);  // 5 frames after the last call: same event
    check(n.eventNumber == 1 && !n.isNewEvent, "events: 5 frames after the last call stays event 1");

    const uint32_t f3 = f2 + EventTracker::kGapFrames;
    n = t.note(f3);  // exactly 90 after the last call: still the same event
    check(n.eventNumber == 1 && !n.isNewEvent, "events: exactly 90 frames after the last call is not yet new");

    const uint32_t f4 = f3 + EventTracker::kGapFrames + 1;
    n = t.note(f4);  // 91 after the last call (f3): a new event
    check(n.eventNumber == 2 && n.isNewEvent, "events: over 90 frames after the last call starts a new event");
}

void caseEventLatch() {
    EventTracker t;
    check(!t.isLatched(), "events: a fresh tracker starts unlatched");
    EventTracker::Note n = t.note(500);
    check(n.isNewEvent, "events: precondition -- first call is a new event");
    t.latch(Treatment::Act);
    check(t.isLatched() && t.treatment() == Treatment::Act, "events: latch sticks immediately");
    // A second call in the SAME event (both eyes, one frame) must not
    // re-decide anything -- isNewEvent stays false and the treatment holds
    // exactly the SubmitPairLatch property this mirrors.
    n = t.note(500);
    check(!n.isNewEvent && t.treatment() == Treatment::Act,
          "events: a second call in the same event keeps the latched treatment");
}

// --- Ring window selection ------------------------------------------------

void caseRingWindow() {
    check(ringFrameInWindow(70, 100), "ring: trigger-30 is in the window (inclusive)");
    check(!ringFrameInWindow(69, 100), "ring: trigger-31 is out");
    check(ringFrameInWindow(130, 100), "ring: trigger+30 is in the window (inclusive)");
    check(!ringFrameInWindow(131, 100), "ring: trigger+31 is out");
    // A trigger near the low end must clamp rather than underflow.
    check(ringFrameInWindow(0, 10), "ring: near-zero trigger clamps its low edge instead of wrapping");
    check(!ringFrameInWindow(41, 10), "ring: near-zero trigger's high edge is still just trigger+after");
    check(ringFrameInWindow(40, 10), "ring: near-zero trigger's high edge, inclusive");
    // A trigger near UINT32_MAX must not overflow trigger+after back to a
    // small number and falsely reject a real frame at the top of the range.
    const uint32_t hiTrigger = 0xFFFFFFF0u;
    check(ringFrameInWindow(0xFFFFFFFFu, hiTrigger),
          "ring: a trigger near UINT32_MAX still admits frames up to +30 without wrapping");
    check(!ringFrameInWindow(hiTrigger - 31, hiTrigger),
          "ring: still excludes a frame just past the low edge near the top of the range");
}

// --- Finding 1 (2026-09-23 review): the per-call act gate -----------------

void caseCallMayActGates() {
    check(callMayAct(Treatment::Act, true, false),
          "callMayAct: acted event, this call's own guard eligible, cap not reached -> acts");
    check(!callMayAct(Treatment::Act, true, true),
          "callMayAct: same, but the session cap is already reached -> refused");
    check(!callMayAct(Treatment::Act, false, false),
          "callMayAct: same, but THIS call's own guard is not eligible -> refused");
    check(!callMayAct(Treatment::Watch, true, false),
          "callMayAct: a watched event never acts, whatever the guard and cap say");
}

void caseEventActRequiresPerCallGuard() {
    // The bug this finding fixes: the OLD code read only
    // EventTracker::treatment(), so once an event latched Act, EVERY
    // disagreeing call inside it was acted on -- including a parent that
    // had been disagreeing on its own the whole time. Reproduce that shape:
    // two parents disagree in the same event, one fresh (opens it), one
    // already past its streak cap.
    ParentGuardTable table;
    EventTracker events;

    // Parent B has been drifting on its own for a few frames before this
    // transition and crosses the persistent-disagree streak.
    table.onClassified(0xB, 1, PoseClass::Disagree);
    table.onClassified(0xB, 2, PoseClass::Disagree);
    table.onClassified(0xB, 3, PoseClass::Disagree);
    const ParentGuardTable::Verdict bVerdict = table.onClassified(0xB, 4, PoseClass::Disagree);
    check(!bVerdict.eligible, "precondition: B's own guard is not eligible (streak over 3)");

    // A brand new parent A disagrees at the same frame -- a real transition
    // can affect more than one camera parent at once -- and opens event 1.
    const ParentGuardTable::Verdict aVerdict = table.onClassified(0xA, 4, PoseClass::Disagree);
    check(aVerdict.eligible, "precondition: a brand-new parent A is eligible coming into its first disagree");
    const EventTracker::Note note = events.note(4);
    check(note.isNewEvent, "precondition: this is a new event");
    events.latch(Treatment::Act);
    check(events.treatment() == Treatment::Act,
          "break-it precondition: the event itself latches Act (the old code's only test)");

    // A's own call may act; B's, in the very same event and frame, must not.
    check(callMayAct(events.treatment(), aVerdict.eligible, /*sessionCapReached=*/false),
          "finding 1: parent A, whose own guard opened this event, may act");
    check(!callMayAct(events.treatment(), bVerdict.eligible, /*sessionCapReached=*/false),
          "finding 1: parent B's persistent disagree streak still blocks it inside an acted event");
}

// --- Finding 3: the session cap counts frames, not calls -------------------

void caseActedFrameDedup() {
    check(isNewActedFrame(100, 0, false), "actedFrame: the first acted call ever is a new frame");
    check(!isNewActedFrame(100, 100, true), "actedFrame: a second call, same frame (the other eye), is not new");
    check(isNewActedFrame(101, 100, true), "actedFrame: the next frame is new again");
    check(!isNewActedFrame(101, 101, true), "actedFrame: a third call at the new frame is still not new");
}

// --- Finding 2: H3's recent-pushes match and per-frame bucketing ----------

void caseRecentPushesMinDistance() {
    RecentPushes pushes;
    check(!pushes.hasAny(), "RecentPushes: starts empty");
    const double p0[3] = {0, 0, 0};
    pushes.push(p0);
    check(pushes.hasAny() && pushes.filled() == 1, "RecentPushes: one push held");
    const double p1[3] = {10, 0, 0};
    pushes.push(p1);
    const double p2[3] = {20, 0, 0};
    pushes.push(p2);
    const double p3[3] = {30, 0, 0};
    pushes.push(p3);
    check(pushes.filled() == 4, "RecentPushes: fills to its 4-deep capacity");
    const double p4[3] = {40, 0, 0};
    pushes.push(p4);  // pushes p0 (the oldest) out
    check(pushes.filled() == 4, "RecentPushes: stays at capacity 4, does not grow further");

    // Held translations are now (most recent first) 40,30,20,10 -- 0 was
    // dropped. An observation near 20 should match slot 2, not slot 0: this
    // is the finding-2 fix -- comparing against only the single last push
    // (slot 0 == 40) would flood "mismatch" for exactly this shape.
    uint32_t slot = 0xFFFFFFFFu;
    const float near20[3] = {21.0f, 0.0f, 0.0f};
    const double d = pushes.minDistance(near20, &slot);
    check(std::fabs(d - 1.0) < 1e-9, "RecentPushes: minDistance finds the closest of the 4, not just the newest");
    check(slot == 2, "RecentPushes: reports which of the 4 slots matched (0 = most recent)");

    double out[3];
    pushes.translationAt(slot, out);
    check(out[0] == 20.0 && out[1] == 0.0 && out[2] == 0.0,
          "RecentPushes: translationAt returns the push that slot actually holds");
}

void caseIsNewH3Best() {
    check(isNewH3Best(false, 0.0, 5.0), "isNewH3Best: the first candidate is always the new best");
    check(isNewH3Best(true, 5.0, 4.999), "isNewH3Best: a strictly smaller candidate replaces the best");
    check(!isNewH3Best(true, 5.0, 5.0), "isNewH3Best: an equal candidate does not replace the first-seen best");
    check(!isNewH3Best(true, 5.0, 5.001), "isNewH3Best: a larger candidate is not the new best");
}

void caseClassifyH3FrameBuckets() {
    check(classifyH3Frame(false, false, 0.0) == H3Bucket::NoObservation,
          "H3 bucket: nothing observed this frame -> no observation / no push");
    check(classifyH3Frame(true, false, 0.0) == H3Bucket::NoObservation,
          "H3 bucket: observed, but nothing pushed yet to compare against -> same bucket");
    check(classifyH3Frame(true, true, 0.005) == H3Bucket::Under1Cm, "H3 bucket: 5mm is under 1cm");
    check(classifyH3Frame(true, true, 0.0099) == H3Bucket::Under1Cm, "H3 bucket: just under 1cm still counts");
    check(classifyH3Frame(true, true, 0.01) == H3Bucket::Under10Cm, "H3 bucket: exactly 1cm rolls into the next bucket");
    check(classifyH3Frame(true, true, 0.05) == H3Bucket::Under10Cm, "H3 bucket: 5cm is under 10cm");
    check(classifyH3Frame(true, true, 0.0999) == H3Bucket::Under10Cm, "H3 bucket: just under 10cm still counts");
    check(classifyH3Frame(true, true, 0.10) == H3Bucket::Under1M, "H3 bucket: exactly 10cm rolls into the next bucket");
    check(classifyH3Frame(true, true, 0.5) == H3Bucket::Under1M, "H3 bucket: 50cm is under 1m");
    check(classifyH3Frame(true, true, 0.999) == H3Bucket::Under1M, "H3 bucket: just under 1m still counts");
    check(classifyH3Frame(true, true, 1.0) == H3Bucket::OneMPlus, "H3 bucket: exactly 1m rolls into the top bucket");
    check(classifyH3Frame(true, true, 50.0) == H3Bucket::OneMPlus, "H3 bucket: far apart stays in the top bucket");
}

// --- Finding 4: dump-request window folding --------------------------------

void caseFoldDumpTrigger() {
    PendingDumpWindow w;
    check(!w.active, "foldDumpTrigger: a fresh window starts inactive");
    w = foldDumpTrigger(w, 100, 30);
    check(w.active && w.lowTriggerFrame == 100 && w.dueFrame == 130,
          "foldDumpTrigger: the first trigger opens the window at [trigger, trigger+defer]");

    // A second trigger, EARLIER than the first (say a detector verdict
    // landing just before our own event's trigger), widens the low edge
    // without moving the due frame backwards.
    w = foldDumpTrigger(w, 90, 30);
    check(w.lowTriggerFrame == 90, "foldDumpTrigger: a lower trigger frame pulls lowTriggerFrame down");
    check(w.dueFrame == 130, "foldDumpTrigger: a due frame that is not later leaves dueFrame alone");

    // A third trigger arriving before the dump was serviced, LATER than
    // both, pushes dueFrame out -- this is the "extend, don't drop" fix
    // (finding 4): the old code simply returned once a dump was pending,
    // without folding the new trigger in at all.
    w = foldDumpTrigger(w, 110, 30);
    check(w.lowTriggerFrame == 90, "foldDumpTrigger: a trigger between the edges moves neither edge down");
    check(w.dueFrame == 140, "foldDumpTrigger: the later trigger's own due frame becomes the new maximum");
}

void caseFrameInDumpWindow() {
    // lowTriggerFrame=90, dueFrame=140 (the merge above): window is
    // [90-30, 140] = [60, 140].
    check(!frameInDumpWindow(59, 90, 140), "frameInDumpWindow: just below the low edge is out");
    check(frameInDumpWindow(60, 90, 140), "frameInDumpWindow: the low edge itself is in (inclusive)");
    check(frameInDumpWindow(140, 90, 140), "frameInDumpWindow: the due frame itself is in (inclusive)");
    check(!frameInDumpWindow(141, 90, 140), "frameInDumpWindow: just past the due frame is out");
    // Saturates instead of underflowing near frame 0, the same footgun
    // ringFrameInWindow guards against above.
    check(frameInDumpWindow(0, 10, 40), "frameInDumpWindow: a low trigger near zero clamps rather than wraps");
}

// --- pose_reader_watch_core.h: Dr7 slot-0 composition ----------------------

void caseDr7ArmLeavesOtherSlotsAlone() {
    check(prw::armSlot0Dr7(0) == 0x000F0001u, "Dr7 arm: from a clear register, exactly L0/RW0/LEN0 come on");
    // Slot 1 already armed (L1=bit2, RW1=bits20-21, LEN1=bits22-23) plus a
    // reserved bit (10) some other debugger set: none of that is this
    // module's to touch.
    const uint32_t otherSlotBits = (1u << 2) | (0x3u << 20) | (0x3u << 22) | (1u << 10);
    const uint32_t armed = prw::armSlot0Dr7(otherSlotBits);
    check((armed & ~prw::kDr7Slot0Mask) == otherSlotBits,
          "Dr7 arm: every bit outside slot 0's mask survives untouched");
    check((armed & prw::kDr7Slot0Mask) == prw::kDr7Slot0ArmedBits,
          "Dr7 arm: slot 0's own bits read exactly L0=1, RW0=11b, LEN0=11b");
}

void caseDr7DisarmClearsOnlyL0() {
    const uint32_t armed = prw::armSlot0Dr7(0);
    const uint32_t disarmed = prw::disarmSlot0Dr7(armed);
    check((disarmed & prw::kDr7L0Bit) == 0, "Dr7 disarm: L0 comes off");
    check((disarmed & ~prw::kDr7L0Bit) == (armed & ~prw::kDr7L0Bit),
          "Dr7 disarm: RW0/LEN0 and every other bit are left exactly as they were");
    // Idempotent, and never disturbs a DIFFERENT slot's enable bit.
    const uint32_t withSlot2 = armed | (1u << 4);  // L2
    check((prw::disarmSlot0Dr7(withSlot2) & (1u << 4)) != 0,
          "Dr7 disarm: another slot's own enable bit is not this call's to clear");
}

void caseDr6Slot0Hit() {
    check(prw::dr6HasSlot0Hit(0x1), "Dr6: bit 0 set reads as our hit");
    check(!prw::dr6HasSlot0Hit(0x0), "Dr6: clear reads as not ours");
    check(!prw::dr6HasSlot0Hit(0x2), "Dr6: B1 alone (a slot this module never arms) is not ours");
    check(prw::dr6HasSlot0Hit(0x200F), "Dr6: our bit still reads even mixed with the other sticky bits");
}

// --- pose_reader_watch_core.h: stack-range classification -------------------

void caseStackRangeClassification() {
    check(prw::pointerOnStack(0x1000, 0x1000, 0x2000), "onStack: the low limit itself is in (inclusive)");
    check(!prw::pointerOnStack(0x2000, 0x1000, 0x2000), "onStack: the high limit itself is out (exclusive)");
    check(prw::pointerOnStack(0x1FFF, 0x1000, 0x2000), "onStack: just under the high limit is in");
    check(!prw::pointerOnStack(0x0FFF, 0x1000, 0x2000), "onStack: just under the low limit is out");
    check(!prw::pointerOnStack(0x5000, 0x1000, 0x2000), "onStack: far above the range is out");
    check(!prw::pointerOnStack(0x1500, 0x2000, 0x1000), "onStack: an inverted range is never 'on it'");
}

// --- pose_reader_watch_core.h: the 60-frame stability gate ------------------

void caseStabilityGateReachesStableAt60() {
    prw::StabilityState s;
    for (uint32_t frame = 1; frame <= 59; ++frame) {
        s = prw::observeAddress(s, 0xABCD0000ull);
        check(!prw::isStable(s), "stability: not yet stable before the 60th consecutive frame");
    }
    s = prw::observeAddress(s, 0xABCD0000ull);
    check(s.consecutive == 60 && prw::isStable(s), "stability: the 60th consecutive same address is stable");
}

void caseStabilityGateResetsOnChange() {
    prw::StabilityState s;
    for (uint32_t i = 0; i < 59; ++i) s = prw::observeAddress(s, 0x1000ull);
    check(!prw::isStable(s), "stability: precondition -- 59 in, not yet stable");
    s = prw::observeAddress(s, 0x2000ull);  // the address changes on what would have been frame 60
    check(s.address == 0x2000ull && s.consecutive == 1,
          "stability: a changed address restarts the run at 1, it does not carry the count over");
    check(!prw::isStable(s), "stability: restarted run is not stable");
}

void caseStabilityGateZeroNeverStable() {
    prw::StabilityState s;
    for (uint32_t i = 0; i < 200; ++i) s = prw::observeAddress(s, 0);
    check(!prw::isStable(s), "stability: a nobody-has-published (0) address never counts, however long");
}

// --- pose_reader_watch_core.h: the unique-reader dedupe table --------------

void caseReaderTableFindsExistingAndReportsNew() {
    prw::ReaderKey table[prw::kMaxReaders];
    table[0] = prw::ReaderKey{0x1000, 0x2000, 0x3000};
    table[1] = prw::ReaderKey{0x1500, 0, 0};
    const uint32_t used = 2;
    check(prw::findReaderSlot(table, used, prw::ReaderKey{0x1000, 0x2000, 0x3000}) == 0,
          "readerTable: an exact repeat of entry 0 finds slot 0");
    check(prw::findReaderSlot(table, used, prw::ReaderKey{0x1500, 0, 0}) == 1,
          "readerTable: an exact repeat of entry 1 finds slot 1");
    check(prw::findReaderSlot(table, used, prw::ReaderKey{0x1000, 0x2000, 0x9999}) == prw::kMaxReaders,
          "readerTable: same rip and first caller but a different second caller is a NEW key");
    check(prw::findReaderSlot(table, used, prw::ReaderKey{0x9999, 0x2000, 0x3000}) == prw::kMaxReaders,
          "readerTable: a different rip alone makes it a new key");
}

void caseReaderTableCap() {
    check(!prw::readerTableFull(prw::kMaxReaders - 1), "readerTable: one short of the cap is not full");
    check(prw::readerTableFull(prw::kMaxReaders), "readerTable: exactly at the cap is full");
    check(prw::readerTableFull(prw::kMaxReaders + 1), "readerTable: past the cap stays full");
}

// --- pose_reader_watch_core.h: the dump-trigger filter ----------------------

void caseDumpVerdictTriggerNarrowsWhenReadersOn() {
    check(prw::dumpVerdictTrigger(false, /*withheldClass=*/true, /*sceneReset=*/false),
          "dumpTrigger: readers off falls back to the old broad withheld-class trigger");
    check(!prw::dumpVerdictTrigger(false, false, false),
          "dumpTrigger: readers off, nothing withheld -> no trigger");
    check(!prw::dumpVerdictTrigger(true, /*withheldClass=*/true, /*sceneReset=*/false),
          "dumpTrigger: readers on -- a render-pass withhold ALONE no longer triggers "
          "(flight 195435's 10-of-12 problem)");
    check(prw::dumpVerdictTrigger(true, /*withheldClass=*/true, /*sceneReset=*/true),
          "dumpTrigger: readers on -- the scene-reset verdict still triggers");
    check(prw::dumpVerdictTrigger(true, /*withheldClass=*/false, /*sceneReset=*/true),
          "dumpTrigger: readers on -- scene-reset triggers even when the old broad test would not have");
}

// --- transition_flash_eye_base_core.h: the bit-exact reset-mailbox match --

void eyeBaseIdentityMailbox(float m[16]) {
    static constexpr float kIdentity[16] = {
        1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
    std::memcpy(m, kIdentity, sizeof(kIdentity));
}

float bitsToFloat(uint32_t bits) {
    float f;
    std::memcpy(&f, &bits, sizeof(f));
    return f;
}

void caseResetMailboxBitExact() {
    float m[16];
    eyeBaseIdentityMailbox(m);
    check(tfeb::isResetMailbox(m), "resetMailbox: the exact constant matches");

    // -0.0f vs +0.0f: IEEE == calls these equal, but their bits differ
    // (0x80000000 vs 0x00000000) -- a writer that stored -0.0 where the
    // constant holds +0.0 has still WRITTEN, and this must see that.
    eyeBaseIdentityMailbox(m);
    m[3] = bitsToFloat(0x80000000u);  // a padding lane, -0.0 instead of +0.0
    check(!tfeb::isResetMailbox(m), "resetMailbox: -0.0 where the constant holds +0.0 does not match");
    check(bitsToFloat(0x80000000u) == 0.0f, "resetMailbox: precondition -- IEEE == would have called this a match");

    // A NaN lane must not match either -- bit-different from any finite
    // constant value regardless of which lane it lands in.
    eyeBaseIdentityMailbox(m);
    m[12] = bitsToFloat(0x7FC00000u);  // quiet NaN in the translation row
    check(!tfeb::isResetMailbox(m), "resetMailbox: a NaN lane does not match");

    // Not tolerances: one ULP off a real value must not match either.
    eyeBaseIdentityMailbox(m);
    m[0] = bitsToFloat(0x3F800001u);  // 1.0f plus one ULP
    check(!tfeb::isResetMailbox(m), "resetMailbox: compares bits, not a tolerance");

    eyeBaseIdentityMailbox(m);
    check(tfeb::isResetMailbox(m), "resetMailbox: unchanged again after the mutating cases above");
}

// --- transition_flash_eye_base_core.h: the M/F agreement and validation ---

void caseEyeBaseAgreementThresholds() {
    float m[16], f[16];
    eyeBaseIdentityMailbox(m);
    eyeBaseIdentityMailbox(f);
    check(tfeb::eyeBaseAgrees(tfeb::compareEyeBase(m, f)), "eyeBase agree: identical M and F agree");

    eyeBaseIdentityMailbox(f);
    f[13] += 0.005f;  // 5mm: under the 0.01 dt threshold
    check(tfeb::eyeBaseAgrees(tfeb::compareEyeBase(m, f)), "eyeBase agree: 5mm translation delta still agrees");

    eyeBaseIdentityMailbox(f);
    f[13] += 0.02f;  // 2cm: over the 0.01 dt threshold
    check(!tfeb::eyeBaseAgrees(tfeb::compareEyeBase(m, f)), "eyeBase agree: 2cm translation delta disagrees");

    eyeBaseIdentityMailbox(f);
    f[5] += 1e-3f;  // a rotation lane, over the 1e-4 dr threshold
    check(!tfeb::eyeBaseAgrees(tfeb::compareEyeBase(m, f)), "eyeBase agree: a rotation-only delta can disagree alone");

    // Padding lanes ([3],[7],[11],[15]) are never compared.
    eyeBaseIdentityMailbox(f);
    f[15] = 99.0f;
    check(tfeb::eyeBaseAgrees(tfeb::compareEyeBase(m, f)), "eyeBase agree: padding lanes contribute nothing");
}

void caseEyeBaseValidationArithmetic() {
    check(!tfeb::isEyeBaseValidated({0, 0}), "eyeBaseValidated: no data at all");
    check(!tfeb::isEyeBaseValidated({119, 0}), "eyeBaseValidated: one short of 120 agreements");
    check(tfeb::isEyeBaseValidated({120, 0}), "eyeBaseValidated: 120 agreements, no disagreements");
    check(tfeb::isEyeBaseValidated({980, 19}), "eyeBaseValidated: 19/999 (~1.9%) passes");
    check(!tfeb::isEyeBaseValidated({960, 40}), "eyeBaseValidated: exactly 1000 with 40 (4%) fails (over 2%)");
    check(!tfeb::isEyeBaseValidated({196, 4}), "eyeBaseValidated: exactly 2% disagree does not pass (under 2%, not at)");
    check(tfeb::isEyeBaseValidated({4901, 99}), "eyeBaseValidated: 99/5000 (1.98%) at scale passes");
}

// --- transition_flash_eye_base_core.h: CHANGE 1, the held-base guard ------
// (task of 2026-09-24: F is retired as the candidate; the mailbox's own last
// known-refilled value, cached and aged in frames, is judged instead).

void caseHeldBaseGuardRefusals() {
    float goodM[16];
    eyeBaseIdentityMailbox(goodM);
    goodM[12] = 100.0f;  // a real translation, not the reset value

    // Baseline: every guard satisfied -> acts. currentFrame=100, cached at
    // frame 98 (age 2, the boundary itself -- see the "exactly 2" case
    // below), same ship, finite, not the reset value, cap not reached.
    check(tfeb::heldBaseMayAct(tfeb::heldBaseRefusal(Treatment::Act, true, 100, 98, 0x1000, 0x1000, goodM, false)),
          "heldBaseRefusal: every guard satisfied acts");

    // Each guard flipped alone refuses, with the rest held at the acting
    // baseline -- and names itself with the right HeldBaseRefusal.
    check(!tfeb::heldBaseMayAct(tfeb::heldBaseRefusal(Treatment::Watch, true, 100, 98, 0x1000, 0x1000, goodM, false)),
          "heldBaseRefusal: watch slot alone refuses");
    check(tfeb::heldBaseRefusal(Treatment::Watch, true, 100, 98, 0x1000, 0x1000, goodM, false) ==
              tfeb::HeldBaseRefusal::WatchSlot,
          "heldBaseRefusal: watch slot names itself");

    // Stale at 3 frames (age = 100 - 97 = 3, over kHeldBaseMaxAgeFrames=2).
    check(!tfeb::heldBaseMayAct(tfeb::heldBaseRefusal(Treatment::Act, true, 100, 97, 0x1000, 0x1000, goodM, false)),
          "heldBaseRefusal: stale at 3 frames refuses");
    check(tfeb::heldBaseRefusal(Treatment::Act, true, 100, 97, 0x1000, 0x1000, goodM, false) ==
              tfeb::HeldBaseRefusal::Stale,
          "heldBaseRefusal: stale at 3 frames names itself");
    // Exactly 2 frames old is still fresh (<=, not <).
    check(tfeb::heldBaseMayAct(tfeb::heldBaseRefusal(Treatment::Act, true, 100, 98, 0x1000, 0x1000, goodM, false)),
          "heldBaseRefusal: exactly 2 frames old still acts");
    // No cache at all (haveHeldBase=false) refuses as stale too, regardless
    // of the frame numbers passed.
    check(!tfeb::heldBaseMayAct(tfeb::heldBaseRefusal(Treatment::Act, false, 100, 98, 0x1000, 0x1000, goodM, false)),
          "heldBaseRefusal: no cache at all refuses");
    check(tfeb::heldBaseRefusal(Treatment::Act, false, 100, 98, 0x1000, 0x1000, goodM, false) ==
              tfeb::HeldBaseRefusal::Stale,
          "heldBaseRefusal: no cache at all names itself stale");

    // Pointer changed: the cached ship differs from this call's ship.
    check(!tfeb::heldBaseMayAct(tfeb::heldBaseRefusal(Treatment::Act, true, 100, 98, 0x1000, 0x2000, goodM, false)),
          "heldBaseRefusal: pointer changed refuses");
    check(tfeb::heldBaseRefusal(Treatment::Act, true, 100, 98, 0x1000, 0x2000, goodM, false) ==
              tfeb::HeldBaseRefusal::PointerChanged,
          "heldBaseRefusal: pointer changed names itself");

    // Non-finite: a single +Inf lane in the CACHED M.
    float infM[16];
    eyeBaseIdentityMailbox(infM);
    infM[9] = bitsToFloat(0x7F800000u);  // +Inf
    check(!tfeb::heldBaseMayAct(tfeb::heldBaseRefusal(Treatment::Act, true, 100, 98, 0x1000, 0x1000, infM, false)),
          "heldBaseRefusal: non-finite cache refuses");
    check(tfeb::heldBaseRefusal(Treatment::Act, true, 100, 98, 0x1000, 0x1000, infM, false) ==
              tfeb::HeldBaseRefusal::NotFinite,
          "heldBaseRefusal: non-finite cache names itself");

    // Reset value: the cache itself is the identity/reset mailbox -- caching
    // logic should never store this (only refilled calls are cached), but
    // the guard still refuses it if it somehow got in.
    float resetM[16];
    eyeBaseIdentityMailbox(resetM);
    check(!tfeb::heldBaseMayAct(tfeb::heldBaseRefusal(Treatment::Act, true, 100, 98, 0x1000, 0x1000, resetM, false)),
          "heldBaseRefusal: the cache itself being the reset value refuses");
    check(tfeb::heldBaseRefusal(Treatment::Act, true, 100, 98, 0x1000, 0x1000, resetM, false) ==
              tfeb::HeldBaseRefusal::Reset,
          "heldBaseRefusal: reset value names itself");

    // Session cap reached.
    check(!tfeb::heldBaseMayAct(tfeb::heldBaseRefusal(Treatment::Act, true, 100, 98, 0x1000, 0x1000, goodM, true)),
          "heldBaseRefusal: session cap reached refuses");
    check(tfeb::heldBaseRefusal(Treatment::Act, true, 100, 98, 0x1000, 0x1000, goodM, true) ==
              tfeb::HeldBaseRefusal::Cap,
          "heldBaseRefusal: cap names itself");
}

// --- transition_flash_eye_base_core.h: CHANGE 2, the writer-watch gate's --
// consecutive-refilled counter.

void caseWriterWatchGateConsecutiveRefilled() {
    uint32_t c = 0;
    for (int i = 0; i < 300; ++i) c = tfeb::updateConsecutiveRefilled(c, /*gameMode=*/2, /*unrefilled=*/false);
    check(c == 300, "consecutiveRefilled: 300 refilled mode!=1 calls in a row reaches 300");
    check(!tfeb::writerWatchGateSatisfied(false, c),
          "writerWatchGate: 300 refilled calls without a stable ship pointer still refuses");
    check(tfeb::writerWatchGateSatisfied(true, c),
          "writerWatchGate: stable pointer + 300 refilled calls satisfies the gate");
    check(!tfeb::writerWatchGateSatisfied(true, c - 1),
          "writerWatchGate: 299 is one short");

    // A mode==1 call is excluded from the sequence: neither breaks nor
    // extends the streak.
    const uint32_t afterMode1 = tfeb::updateConsecutiveRefilled(c, /*gameMode=*/1, /*unrefilled=*/false);
    check(afterMode1 == c, "consecutiveRefilled: a mode==1 call leaves the streak untouched");
    const uint32_t afterMode1Unrefilled = tfeb::updateConsecutiveRefilled(c, /*gameMode=*/1, /*unrefilled=*/true);
    check(afterMode1Unrefilled == c,
          "consecutiveRefilled: a mode==1 call leaves the streak untouched even if it LOOKS unrefilled");

    // The required cell: an un-refilled mode!=1 call resets the streak.
    const uint32_t resetFrom = tfeb::updateConsecutiveRefilled(250, /*gameMode=*/2, /*unrefilled=*/true);
    check(resetFrom == 0, "consecutiveRefilled: an un-refilled call resets the streak to 0");
    check(!tfeb::writerWatchGateSatisfied(true, resetFrom),
          "writerWatchGate: a freshly reset streak no longer satisfies the gate");
}

// --- transition_flash_eye_base_core.h: CHANGE 3, the dump trigger ---------

void caseEyeBaseDumpTriggerIndependentOfEyeTrace() {
    // eyeBaseDumpTrigger takes no eye-trace flag: an un-refilled call and a
    // scene-reset verdict are its only two inputs, so exhausting their four
    // combinations IS the independence proof -- advanced.eye_origin_trace
    // has nothing here it could gate.
    check(tfeb::eyeBaseDumpTrigger(true, false), "eyeBaseDumpTrigger: an un-refilled call alone triggers");
    check(tfeb::eyeBaseDumpTrigger(false, true), "eyeBaseDumpTrigger: a scene-reset verdict alone triggers");
    check(tfeb::eyeBaseDumpTrigger(true, true), "eyeBaseDumpTrigger: both together still triggers");
    check(!tfeb::eyeBaseDumpTrigger(false, false), "eyeBaseDumpTrigger: neither does not trigger");
}

void caseEyeBaseFiniteCheck() {
    float f[16];
    eyeBaseIdentityMailbox(f);
    check(tfeb::allFinite16(f), "allFinite16: an ordinary mailbox is finite");
    f[9] = bitsToFloat(0x7F800000u);  // +Inf
    check(!tfeb::allFinite16(f), "allFinite16: a single +Inf lane fails it");
    eyeBaseIdentityMailbox(f);
    f[2] = bitsToFloat(0x7FC00000u);  // NaN
    check(!tfeb::allFinite16(f), "allFinite16: a single NaN lane fails it");
}

// --- transition_flash_eye_base_core.h: the consumer's own extent ----------

void caseConsumerExtentClassifier() {
    check(!tfeb::rvaInsideConsumerExtent(tfeb::kConsumerExtentRva - 1),
          "consumerExtent: one byte before the entry is outside");
    check(tfeb::rvaInsideConsumerExtent(tfeb::kConsumerExtentRva),
          "consumerExtent: the entry point itself is inside");
    check(tfeb::rvaInsideConsumerExtent(tfeb::kConsumerExtentRva + tfeb::kConsumerExtentSize - 1),
          "consumerExtent: the last byte of the body is inside");
    check(!tfeb::rvaInsideConsumerExtent(tfeb::kConsumerExtentRva + tfeb::kConsumerExtentSize),
          "consumerExtent: exactly at the end (one past the body) is outside");
}

// --- transition_flash_eye_base_core.h: CHANGE 5, the writer's own extent --

void caseWriterExtentClassifier() {
    // Hardcoded literals, independent of kWriterExtentRva/kWriterExtentSize
    // themselves (verified against analysis\EliteDangerous64.exe's PE
    // section table directly, not just the decompiler dump): the boundary
    // checks below compare against the constants, so alone they cannot
    // catch a wrong SIZE (a mutation to 0xEE instead of 0xEF passed every
    // one of them during this cell's own break-it pass); this pins the
    // entry and one-past-the-end RVAs independently.
    check(tfeb::kWriterExtentRva == 0x2874B20u, "writerExtent: entry RVA is 0x2874B20");
    check(tfeb::kWriterExtentRva + tfeb::kWriterExtentSize == 0x2874C0Fu,
          "writerExtent: one past the end is 0x2874C0F (239 bytes)");
    check(!tfeb::rvaInsideWriterExtent(tfeb::kWriterExtentRva - 1),
          "writerExtent: one byte before the entry is outside");
    check(tfeb::rvaInsideWriterExtent(tfeb::kWriterExtentRva),
          "writerExtent: the entry point itself is inside");
    check(tfeb::rvaInsideWriterExtent(tfeb::kWriterExtentRva + tfeb::kWriterExtentSize - 1),
          "writerExtent: the last byte of the body is inside");
    check(!tfeb::rvaInsideWriterExtent(tfeb::kWriterExtentRva + tfeb::kWriterExtentSize),
          "writerExtent: exactly at the end (one past the body) is outside");
    // The writer and consumer extents must not overlap -- they are two
    // different functions the design doc found in two different rounds.
    check(!tfeb::rvaInsideConsumerExtent(tfeb::kWriterExtentRva),
          "writerExtent: the writer's entry is not inside the consumer's extent");
    check(!tfeb::rvaInsideWriterExtent(tfeb::kConsumerExtentRva),
          "writerExtent: the consumer's entry is not inside the writer's extent");
}

// --- transition_flash_eye_base_core.h: CHANGE 5, Dr7 slot 1 (EXECUTE) -----

void caseDr7ArmSlot1ExecuteLeavesSlot0Alone() {
    const uint32_t armed = tfeb::armSlot1ExecuteDr7(0);
    // Hardcoded literal, independent of kDr7L1Bit itself -- the same
    // property caseDr7ArmLeavesOtherSlotsAlone pins for slot 0's own
    // 0x000F0001u, so a wrong bit position in the constant (not just a
    // wrong mask) is caught here directly, not only via the mask-consistency
    // checks below.
    check(armed == 0x00000004u, "Dr7 slot1 arm: from a clear register, exactly bit 2 (L1) comes on");
    check((armed & tfeb::kDr7L1Bit) != 0, "Dr7 slot1 arm: L1 comes on");
    check(((armed >> 20) & 0x3u) == 0, "Dr7 slot1 arm: RW1 is 00b (execute)");
    check(((armed >> 22) & 0x3u) == 0, "Dr7 slot1 arm: LEN1 is 00b");

    // Slot 0 already armed (L0=bit0, RW0=bits16-17, LEN0=bits18-19) plus a
    // reserved bit (bit10): every one of those must survive untouched, the
    // same property caseDr7ArmLeavesOtherSlotsAlone asserts for slot 0's own
    // arm, mirrored here for slot 1.
    const uint32_t otherSlotBits = prw::kDr7L0Bit | (0x3u << 16) | (0x3u << 18) | (1u << 10);
    const uint32_t armedWithOthers = tfeb::armSlot1ExecuteDr7(otherSlotBits);
    check((armedWithOthers & ~tfeb::kDr7Slot1Mask) == otherSlotBits,
          "Dr7 slot1 arm: every bit outside slot 1's mask survives untouched");
    check((armedWithOthers & prw::kDr7L0Bit) != 0, "Dr7 slot1 arm: slot 0's own L0 is still set");
    check(((armedWithOthers >> 16) & 0x3u) == 0x3u, "Dr7 slot1 arm: slot 0's own RW0 is untouched");

    const uint32_t disarmed = tfeb::disarmSlot1Dr7(armedWithOthers);
    check((disarmed & tfeb::kDr7L1Bit) == 0, "Dr7 slot1 disarm: L1 clears");
    check((disarmed & otherSlotBits) == otherSlotBits,
          "Dr7 slot1 disarm: slot 0's bits (and the reserved bit) survive untouched");

    // disarmSlot1Dr7 only clears L1 -- RW1/LEN1 left behind are inert, the
    // same rule prw::disarmSlot0Dr7 states for its own slot. armSlot1Execute-
    // Dr7 always writes 00b/00b, so that path alone can never tell "left
    // behind" apart from "cleared to the same value" -- disarm a hand-built
    // Dr7 with non-zero RW1/LEN1 (as if some other debugger had set them)
    // directly, the same way caseDr7DisarmClearsOnlyL0 tests slot 0's own
    // disarm independently of that slot's own arm.
    const uint32_t nonZeroRw1Len1 = tfeb::kDr7L1Bit | (0x3u << 20) | (0x3u << 22);
    const uint32_t disarmedFromNonZero = tfeb::disarmSlot1Dr7(nonZeroRw1Len1);
    check((disarmedFromNonZero & tfeb::kDr7L1Bit) == 0, "Dr7 slot1 disarm: L1 clears from a non-zero RW1/LEN1 start");
    check(((disarmedFromNonZero >> 20) & 0xFu) == 0xFu,
          "Dr7 slot1 disarm: non-zero RW1/LEN1 bits are left behind, not cleared");
}

void caseDr6Slot1Hit() {
    check(tfeb::dr6HasSlot1Hit(tfeb::kDr6B1Bit), "Dr6: B1 alone reads as a slot-1 hit");
    check(!tfeb::dr6HasSlot1Hit(prw::kDr6B0Bit), "Dr6: B0 alone does not read as a slot-1 hit");
    check(tfeb::dr6HasSlot1Hit(prw::kDr6B0Bit | tfeb::kDr6B1Bit),
          "Dr6: B0 and B1 together still reads as a slot-1 hit");
    check(!tfeb::dr6HasSlot1Hit(0), "Dr6: no bits set is not a slot-1 hit");
}

// --- transition_flash_eye_base_core.h: CHANGE 6, the substitute policy ----
// (offered vs. held).

void caseOfferedSubstituteUsable() {
    float goodOffered[16];
    eyeBaseIdentityMailbox(goodOffered);
    goodOffered[12] = 55.0f;  // a real translation, not the reset value

    // Baseline: entered, did not write since, an offer captured this frame,
    // finite, not reset -> usable.
    check(tfeb::offeredSubstituteUsable(true, false, true, 100, 100, goodOffered),
          "offeredSubstituteUsable: entered, refused, fresh this frame -> offered");
    // Fresh also covers "the frame before".
    check(tfeb::offeredSubstituteUsable(true, false, true, 100, 99, goodOffered),
          "offeredSubstituteUsable: fresh from the previous frame -> offered");
    // Two frames old is stale (kOfferedMaxAgeFrames=1, tighter than the held
    // base's own 2-frame cap).
    check(!tfeb::offeredSubstituteUsable(true, false, true, 100, 98, goodOffered),
          "offeredSubstituteUsable: two frames old is stale -> refused");

    // Not entered at all (skip candidate 1: the controller's own gate) ->
    // held, regardless of anything else being otherwise usable.
    check(!tfeb::offeredSubstituteUsable(false, false, true, 100, 100, goodOffered),
          "offeredSubstituteUsable: the writer was not entered -> held");

    // Entered AND wrote: the name gate did NOT refuse it, so this call would
    // not even be unrefilled -- still must not be treated as offered-usable
    // if asked.
    check(!tfeb::offeredSubstituteUsable(true, true, true, 100, 100, goodOffered),
          "offeredSubstituteUsable: the writer wrote since the previous consume -> held");

    // No offer ever captured.
    check(!tfeb::offeredSubstituteUsable(true, false, false, 100, 100, goodOffered),
          "offeredSubstituteUsable: nothing captured -> held");

    // Stale, non-finite, and reset-value offers each refuse alone, with
    // every other guard held at the acting baseline.
    check(!tfeb::offeredSubstituteUsable(true, false, true, 100, 50, goodOffered),
          "offeredSubstituteUsable: a far-stale offer -> held");
    float infOffered[16];
    eyeBaseIdentityMailbox(infOffered);
    infOffered[9] = bitsToFloat(0x7F800000u);  // +Inf
    check(!tfeb::offeredSubstituteUsable(true, false, true, 100, 100, infOffered),
          "offeredSubstituteUsable: a non-finite offer -> held");
    float resetOffered[16];
    eyeBaseIdentityMailbox(resetOffered);
    check(!tfeb::offeredSubstituteUsable(true, false, true, 100, 100, resetOffered),
          "offeredSubstituteUsable: the offer itself is the reset value -> held");
}

// --- transition_flash_eye_base_core.h: CHANGE 7, per-frame classification -

void caseClassifyFrameWriter() {
    check(tfeb::classifyFrameWriter(0, 0, 0) == tfeb::FrameWriterClass::ControllerNotCalled,
          "classifyFrameWriter: no controller calls -> controller not called");
    check(tfeb::classifyFrameWriter(0, 5, 5) == tfeb::FrameWriterClass::ControllerNotCalled,
          "classifyFrameWriter: controller calls checked first, regardless of the other counts");
    check(tfeb::classifyFrameWriter(3, 0, 0) == tfeb::FrameWriterClass::ControllerCalledWriterNotEntered,
          "classifyFrameWriter: controller ran, writer never entered -> skip candidate 1");
    check(tfeb::classifyFrameWriter(3, 2, 0) == tfeb::FrameWriterClass::WriterEnteredNotWritten,
          "classifyFrameWriter: entered but did not write -> skip candidate 2 (the name gate)");
    check(tfeb::classifyFrameWriter(3, 2, 1) == tfeb::FrameWriterClass::WriterWrote,
          "classifyFrameWriter: entered and wrote -> an ordinary frame");
    check(tfeb::classifyFrameWriter(3, 2, 2) == tfeb::FrameWriterClass::WriterWrote,
          "classifyFrameWriter: writerWrote need not equal writerEntered to still read as WriterWrote");
}

// --- transition_flash_eye_base_core.h: CHANGE 9, mode-switch edges --------
// (task 2026-09-24: dump triggers become entry/exit edges instead of firing
// on every un-refilled call -- flight 091726's low wake alone was 5,041 of
// them, one dump trigger each.)

void caseModeSwitchEdgeEntryFiresOnce() {
    uint32_t c = 0;
    // A refilled call from a zero streak is not an edge -- nothing to enter.
    check(tfeb::classifyModeSwitchEdge(/*gameMode=*/2, /*unrefilled=*/false, c) == tfeb::ModeSwitchEdge::None,
          "modeSwitchEdge: a refilled call from a zero streak is not an edge");

    // The first un-refilled call after a refilled one: ENTRY.
    check(tfeb::classifyModeSwitchEdge(2, true, c) == tfeb::ModeSwitchEdge::Entry,
          "modeSwitchEdge: the first un-refilled call after a refilled one is ENTRY");
    c = tfeb::updateConsecutiveUnrefilled(c, 2, true);
    check(c == 1, "modeSwitchEdge: the streak counter reaches 1 after that call");

    // The second consecutive un-refilled call is not another entry -- entry
    // fires once per run, not on every call inside it.
    check(tfeb::classifyModeSwitchEdge(2, true, c) == tfeb::ModeSwitchEdge::None,
          "modeSwitchEdge: entry fires once -- the second un-refilled call in the run is not an edge");
}

void caseModeSwitchEdgeNoneInsideRun() {
    uint32_t c = 1;  // the entry has already fired; this run is under way
    for (int i = 0; i < 28; ++i) {
        check(tfeb::classifyModeSwitchEdge(2, true, c) == tfeb::ModeSwitchEdge::None,
              "modeSwitchEdge: no trigger inside an un-refilled run");
        c = tfeb::updateConsecutiveUnrefilled(c, 2, true);
    }
    check(c == 29, "modeSwitchEdge: 29 consecutive un-refilled calls (1 entry + 28 more, still no exit)");
}

void caseModeSwitchEdgeExitAfterThirty() {
    // One short of the exit run length: a refilled call here is not an exit.
    uint32_t c29 = 0;
    for (int i = 0; i < 29; ++i) c29 = tfeb::updateConsecutiveUnrefilled(c29, 2, true);
    check(c29 == 29, "modeSwitchEdge: 29 consecutive un-refilled calls, one short of the exit run");
    check(tfeb::classifyModeSwitchEdge(2, false, c29) == tfeb::ModeSwitchEdge::None,
          "modeSwitchEdge: exit fires only after >= 30 -- a refilled call after 29 is not one");

    // Exactly 30: the next refilled call IS the exit.
    uint32_t c30 = c29;
    c30 = tfeb::updateConsecutiveUnrefilled(c30, 2, true);
    check(c30 == 30, "modeSwitchEdge: 30 consecutive un-refilled calls reaches the exit run length");
    check(tfeb::classifyModeSwitchEdge(2, false, c30) == tfeb::ModeSwitchEdge::Exit,
          "modeSwitchEdge: a refilled call after 30 consecutive un-refilled ones is EXIT");

    // A mode==1 call neither breaks nor extends the run, and is never itself
    // an edge -- the same exclusion updateConsecutiveRefilled already
    // applies to the refilled-streak counter.
    const uint32_t afterMode1 = tfeb::updateConsecutiveUnrefilled(c30, /*gameMode=*/1, /*unrefilled=*/true);
    check(afterMode1 == c30, "modeSwitchEdge: a mode==1 call leaves the streak untouched");
    check(tfeb::classifyModeSwitchEdge(1, true, c30) == tfeb::ModeSwitchEdge::None,
          "modeSwitchEdge: a mode==1 call is never itself an edge");
}

void caseModeSwitchEdgeSingleFrameSkip() {
    uint32_t c = 0;
    // Refilled, then a single un-refilled frame: ENTRY.
    check(tfeb::classifyModeSwitchEdge(2, true, c) == tfeb::ModeSwitchEdge::Entry,
          "modeSwitchEdge: a single-frame skip's own un-refilled call is ENTRY");
    c = tfeb::updateConsecutiveUnrefilled(c, 2, true);
    check(c == 1, "modeSwitchEdge: the skip's streak reaches 1");

    // Refilled again immediately: not an exit -- the run never reached 30.
    check(tfeb::classifyModeSwitchEdge(2, false, c) == tfeb::ModeSwitchEdge::None,
          "modeSwitchEdge: a single-frame skip's recovery fires no exit (run length 1 < 30)");
}

// --- glitch_scene.h: CHANGE 9, the per-frame dump row's own fresh/decision
// print text (recordScenePosition's own zeroed-when-stale fold, glitch_
// frame.cpp, given a name a reader -- and this test -- can ask for).

void caseSceneGeometryFreshAndDecisionText() {
    check(std::strcmp(edvr::glitchSceneGeometryFreshText(true), "") == 0,
          "sceneGeometryFreshText: a fresh record prints no prefix");
    check(std::strcmp(edvr::glitchSceneGeometryFreshText(false), "STALE-") == 0,
          "sceneGeometryFreshText: a stale-geometry record prints as not fresh");
    check(std::strcmp(edvr::glitchSceneDecisionText(edvr::GlitchSceneDecision::Unknown), "unknown") == 0,
          "sceneDecisionText: Unknown prints as unknown");
    check(std::strcmp(edvr::glitchSceneDecisionText(edvr::GlitchSceneDecision::Coherent), "coherent") == 0,
          "sceneDecisionText: Coherent prints as coherent");
    check(std::strcmp(edvr::glitchSceneDecisionText(edvr::GlitchSceneDecision::CameraReset), "cameraReset") == 0,
          "sceneDecisionText: CameraReset prints as cameraReset");
}

// --- pose_reader_watch_core.h: the write-mode Dr7 slot-0 composition ------
// (design doc round 6, part B: the writer watch wants RW0=01b where the
// render-pose read watch above wants RW0=11b -- armSlot0Dr7's own new
// parameter, exercised here in the mode the pose path never asks for.)

void caseDr7ArmWriteModeSlot0() {
    const uint32_t armed = prw::armSlot0Dr7(0, prw::kDr7RwWrite);
    check((armed & prw::kDr7L0Bit) != 0, "Dr7 write-mode arm: L0 comes on");
    check(((armed >> 16) & 0x3u) == prw::kDr7RwWrite, "Dr7 write-mode arm: RW0 reads back 01b, not 11b");
    check(((armed >> 18) & 0x3u) == prw::kDr7Len4Bytes, "Dr7 write-mode arm: LEN0 is still 4 bytes");

    // Slot 1 already armed (L1=bit2, RW1=bits20-21, LEN1=bits22-23) plus a
    // reserved bit (bit10): every one of those must survive untouched, the
    // same property caseDr7ArmLeavesOtherSlotsAlone asserts for the
    // default read-or-write mode.
    const uint32_t otherSlotBits = (1u << 2) | (0x3u << 20) | (0x3u << 22) | (1u << 10);
    const uint32_t armedWithOthers = prw::armSlot0Dr7(otherSlotBits, prw::kDr7RwWrite);
    check((armedWithOthers & ~prw::kDr7Slot0Mask) == otherSlotBits,
          "Dr7 write-mode arm: every bit outside slot 0's mask survives untouched");
    check(((armedWithOthers >> 16) & 0x3u) == prw::kDr7RwWrite,
          "Dr7 write-mode arm: RW0 stays 01b even with other slots live");

    // The default argument reproduces the pose path's own read-or-write
    // shape exactly -- the "keep the pose path's behaviour identical" half
    // of the refactor.
    check(prw::armSlot0Dr7(0) == prw::armSlot0Dr7(0, prw::kDr7RwReadWrite),
          "Dr7 arm: the default RW mode is still read-or-write");
}
// --- transition_flash_eye_base_core.h: CHANGE 10, the render-time patch ----
// simulation's pure logic (task 2026-09-24, "render-time patch simulation
// (watch-only)"): the eye composer's own multiply (patchEyeOrigin), the
// acting build's full premultiply (premul4x4), and the scene-old/new
// selector the flight-100043 and 2026-09-12 evidence defines
// (patchSceneChoice), plus the pending sim's render-frame window.

bool near3(const float a[3], const float b[3], float eps) {
    return std::fabs(a[0] - b[0]) <= eps && std::fabs(a[1] - b[1]) <= eps &&
           std::fabs(a[2] - b[2]) <= eps;
}

void casePatchEyeOrigin() {
    const float I[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0};
    float out[3];

    // Identity base: composing the bad head-only eye against the identity is
    // exactly the bad frame's own shape, so the patch reproduces P.
    const float P[3] = {0.12f, -0.34f, 1.56f};
    tfeb::patchEyeOrigin(I, P, out);
    check(near3(out, P, 1e-6f), "patchEyeOrigin: identity base passes P through");

    // Pure translation: out = P + t. The translation lanes are the f13550
    // flight-100043 mailbox value itself.
    const float T[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, -0.660f, 11.066f, -7.725f, 0};
    const float wantT[3] = {P[0] - 0.660f, P[1] + 11.066f, P[2] - 7.725f};
    tfeb::patchEyeOrigin(T, P, out);
    check(near3(out, wantT, 1e-5f), "patchEyeOrigin: pure translation adds t");

    // The flight-100043 f13550 ordinary-frame numbers end to end: mailbox
    // translation (-0.660, +11.066, -7.725), head offset (0, +0.014,
    // +0.085) -- the rendered scene eye measured (-0.66, +11.08, -7.64).
    const float head[3] = {0.0f, 0.014f, 0.085f};
    const float wantScene[3] = {-0.660f, 11.080f, -7.640f};
    tfeb::patchEyeOrigin(T, head, out);
    check(near3(out, wantScene, 1e-3f), "patchEyeOrigin: the f13550 mailbox+head lands on the measured scene eye");

    // A known rotation: 90 degrees about Z (row 0 (0,-1,0), row 1 (1,0,0))
    // maps (1,2,3) to (2,-1,3).
    const float R[16] = {0, -1, 0, 0, 1, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0};
    const float P2[3] = {1, 2, 3};
    const float wantR[3] = {2, -1, 3};
    tfeb::patchEyeOrigin(R, P2, out);
    check(near3(out, wantR, 1e-6f), "patchEyeOrigin: 90-degree Z rotation rotates P");
}

void casePremul4x4() {
    const float I[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    const float B[16] = {2, 0, 0, 0, 0, 3, 0, 0, 0, 0, 4, 0, 1, 2, 3, 1};
    float out[16];

    tfeb::premul4x4(B, I, out);
    bool same = true;
    for (int i = 0; i < 16; ++i) same = same && out[i] == B[i];
    check(same, "premul4x4: B x I = B");

    tfeb::premul4x4(I, B, out);
    same = true;
    for (int i = 0; i < 16; ++i) same = same && out[i] == B[i];
    check(same, "premul4x4: I x B = B");

    // Hand-composed: B scales (2,3,4) with t(1,2,3); M swaps x/y with
    // t(5,6,7). B*M scales rows of M: row0 (0,2,0), row1 (3,0,0), row2
    // (0,0,4); the translation is t(B) through M's 3x3 plus t(M):
    // (1,2,3) -> (0*1+1*2+0*3, 1*1+0*2+0*3, 0*1+0*2+1*3) + (5,6,7)
    // = (7,7,10); the last row stays (0,0,0,1).
    const float M[16] = {0, 1, 0, 0, 1, 0, 0, 0, 0, 0, 1, 0, 5, 6, 7, 1};
    tfeb::premul4x4(B, M, out);
    check(out[0] == 0 && out[1] == 2 && out[2] == 0 && out[3] == 0 &&
          out[4] == 3 && out[5] == 0 && out[6] == 0 && out[7] == 0 &&
          out[8] == 0 && out[9] == 0 && out[10] == 4 && out[11] == 0 &&
          out[12] == 7 && out[13] == 7 && out[14] == 10 && out[15] == 1,
          "premul4x4: the hand-composed B*M (scale through swap plus translations) matches");
}

void casePatchSceneChoiceFlightPoints() {
    using tfeb::SceneChoice;
    // 2026-09-12 hyperspace exit: the pool is still in the old frame while
    // the camera rebases -- pool 0.0 against a 1600-unit camera step.
    check(tfeb::patchSceneChoice(1600.0f, 0.0f, true) == SceneChoice::Old,
          "patchSceneChoice: hyperspace exit (cam 1600, pool 0) is scene-old");
    // Flight 100043's scene-new resets: the pool stepped with the camera.
    check(tfeb::patchSceneChoice(2925.8f, 2927.2f, true) == SceneChoice::New,
          "patchSceneChoice: f13549 (2925.8/2927.2) is scene-new");
    check(tfeb::patchSceneChoice(13.5f, 14.7f, true) == SceneChoice::New,
          "patchSceneChoice: f13939 (13.5/14.7) is scene-new");
    check(tfeb::patchSceneChoice(3594.2f, 3567.3f, true) == SceneChoice::New,
          "patchSceneChoice: f22217 (3594.2/3567.3) is scene-new");
    check(tfeb::patchSceneChoice(3860.4f, 3861.7f, true) == SceneChoice::New,
          "patchSceneChoice: f23340 (3860.4/3861.7) is scene-new");
}

void casePatchSceneChoiceBands() {
    using tfeb::SceneChoice;
    // Band edges, fresh geometry: the new band is inclusive at both ends.
    check(tfeb::patchSceneChoice(100.0f, 50.0f, true) == SceneChoice::New,
          "patchSceneChoice: ratio exactly 0.5 is scene-new (inclusive)");
    check(tfeb::patchSceneChoice(100.0f, 200.0f, true) == SceneChoice::New,
          "patchSceneChoice: ratio exactly 2.0 is scene-new (inclusive)");
    // The old band is exclusive: exactly 0.25 is neither band.
    check(tfeb::patchSceneChoice(100.0f, 25.0f, true) == SceneChoice::Unclear,
          "patchSceneChoice: ratio exactly 0.25 is the dead band, not scene-old");
    // Inside the dead band, either side.
    check(tfeb::patchSceneChoice(100.0f, 30.0f, true) == SceneChoice::Unclear,
          "patchSceneChoice: ratio 0.3 above the old band is unclear");
    check(tfeb::patchSceneChoice(100.0f, 250.0f, true) == SceneChoice::Unclear,
          "patchSceneChoice: ratio 2.5 above the new band is unclear");
    // Just inside the old band.
    check(tfeb::patchSceneChoice(100.0f, 24.9f, true) == SceneChoice::Old,
          "patchSceneChoice: ratio 0.249 is scene-old");
    // Stale geometry is unclear whatever the numbers say -- a zeroed default
    // geometry (cam 0, pool 0) would otherwise read as scene-old 0.
    check(tfeb::patchSceneChoice(0.0f, 0.0f, false) == SceneChoice::Unclear,
          "patchSceneChoice: not-fresh geometry is unclear even at ratio 0");
    check(tfeb::patchSceneChoice(2925.8f, 2927.2f, false) == SceneChoice::Unclear,
          "patchSceneChoice: not-fresh geometry is unclear even at a scene-new ratio");
    // A zero camera step divides by the floor, not by zero.
    check(tfeb::patchSceneChoice(0.0f, 0.0f, true) == SceneChoice::Old,
          "patchSceneChoice: fresh zero-over-zero reads as scene-old 0 through the cam floor");
}

void casePatchSimWindow() {
    // Armed at the skip frame N, the sim covers exactly N+1..N+3 (the bad
    // render is N+2; one frame of skew slack either side).
    check(!tfeb::patchSimWindowCovers(13547, 13547), "patchSimWindow: the skip frame itself is not covered");
    check(tfeb::patchSimWindowCovers(13548, 13547), "patchSimWindow: N+1 is covered");
    check(tfeb::patchSimWindowCovers(13549, 13547), "patchSimWindow: N+2 (the bad render) is covered");
    check(tfeb::patchSimWindowCovers(13550, 13547), "patchSimWindow: N+3 is covered");
    check(!tfeb::patchSimWindowCovers(13551, 13547), "patchSimWindow: N+4 is past the window");
}

// --- transition_flash_eye_base_core.h: CHANGE 13, the tap-time LIVE-mailbox
// probe's sanity gate (flight N+1's finding: the consume-indexed NEW-base
// candidate is structurally stale at the bad render; the refill sits LIVE
// in the mailbox during the tap instead).

void caseLiveMailboxPlausible() {
    const float good[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0,
                            0.003f, 3.509f, 1613.249f, 1};   // the f15513 tunnel base itself
    check(tfeb::mailboxPlausible(good), "liveMailbox: the measured tunnel base passes");

    const float fiveKm[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0,
                              -229.8f, 663.0f, 274.5f, 1};   // a measured 5 km rebase
    check(tfeb::mailboxPlausible(fiveKm), "liveMailbox: a measured 5 km rebase passes");

    float bad[16];
    std::memcpy(bad, good, sizeof(bad));
    bad[12] = 2e7f;   // 20,000 km -- past the 1e7 gate
    check(!tfeb::mailboxPlausible(bad), "liveMailbox: a 2e7-unit translation is implausible");

    std::memcpy(bad, good, sizeof(bad));
    bad[7] = NAN;     // one non-finite lane anywhere
    check(!tfeb::mailboxPlausible(bad), "liveMailbox: a NaN lane is implausible");

    std::memcpy(bad, good, sizeof(bad));
    bad[0] = INFINITY;
    check(!tfeb::mailboxPlausible(bad), "liveMailbox: an Inf lane is implausible");

    // The gate does not screen the RESET value -- the caller does that with
    // isResetMailbox first; identity passes here by design.
    check(tfeb::mailboxPlausible(tfeb::kResetMailbox), "liveMailbox: the reset value is for isResetMailbox, not this gate");
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc != 2) return 2;
    if (!std::wcscmp(argv[1], L"--dry-run")) {
        std::puts("transition_flash_prevent_test: dry-run (no runtime, device or files)");
        return 0;
    }
    if (std::wcscmp(argv[1], L"--self-test")) return 2;
    caseClassifyIgnoresPadding();
    caseClassifyThresholds();
    caseValidation();
    caseGuardNewAndRecentAgree();
    caseGuardStreakStillGatesWithinAgreedRecently();
    caseGuardUnseenAndStale();
    caseGuardLruEviction();
    caseModeAndCap();
    caseEventGrouping();
    caseEventLatch();
    caseRingWindow();
    caseCallMayActGates();
    caseEventActRequiresPerCallGuard();
    caseActedFrameDedup();
    caseRecentPushesMinDistance();
    caseIsNewH3Best();
    caseClassifyH3FrameBuckets();
    caseFoldDumpTrigger();
    caseFrameInDumpWindow();
    caseDr7ArmLeavesOtherSlotsAlone();
    caseDr7DisarmClearsOnlyL0();
    caseDr6Slot0Hit();
    caseStackRangeClassification();
    caseStabilityGateReachesStableAt60();
    caseStabilityGateResetsOnChange();
    caseStabilityGateZeroNeverStable();
    caseReaderTableFindsExistingAndReportsNew();
    caseReaderTableCap();
    caseDumpVerdictTriggerNarrowsWhenReadersOn();
    caseResetMailboxBitExact();
    caseEyeBaseAgreementThresholds();
    caseEyeBaseValidationArithmetic();
    caseHeldBaseGuardRefusals();
    caseWriterWatchGateConsecutiveRefilled();
    caseEyeBaseDumpTriggerIndependentOfEyeTrace();
    caseEyeBaseFiniteCheck();
    caseConsumerExtentClassifier();
    caseDr7ArmWriteModeSlot0();
    caseWriterExtentClassifier();
    caseDr7ArmSlot1ExecuteLeavesSlot0Alone();
    caseDr6Slot1Hit();
    caseOfferedSubstituteUsable();
    caseClassifyFrameWriter();
    caseModeSwitchEdgeEntryFiresOnce();
    caseModeSwitchEdgeNoneInsideRun();
    caseModeSwitchEdgeExitAfterThirty();
    caseModeSwitchEdgeSingleFrameSkip();
    caseSceneGeometryFreshAndDecisionText();
    casePatchEyeOrigin();
    casePremul4x4();
    casePatchSceneChoiceFlightPoints();
    casePatchSceneChoiceBands();
    casePatchSimWindow();
    caseLiveMailboxPlausible();
    std::printf("transition_flash_prevent_test: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
