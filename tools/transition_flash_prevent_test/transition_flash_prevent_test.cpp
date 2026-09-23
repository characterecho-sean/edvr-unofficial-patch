// Build gate for transition_flash_prevent_core.h's pure logic: the
// classifier (and that it truly ignores the padding lanes, not merely
// documents that it should), the validation and per-parent guard
// arithmetic (including LRU eviction), event grouping with the alternate
// latch, and the ring window test's overflow safety. No game, no Windows
// hook -- this drives the header directly, the kinematic_probe_test
// pattern (single-TU, production source compiled into the rig).
#include <cstdint>
#include <cstdio>
#include <cwchar>
#include "../../src/d3d11/transition_flash_prevent_core.h"

using namespace edvr::tfp;

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
    std::printf("transition_flash_prevent_test: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
