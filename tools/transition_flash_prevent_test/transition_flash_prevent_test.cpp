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
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cwchar>
#include "../../src/d3d11/transition_flash_prevent_core.h"
#include "../../src/d3d11/pose_reader_watch_core.h"

using namespace edvr::tfp;
namespace prw = edvr::prw;

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
    std::printf("transition_flash_prevent_test: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
