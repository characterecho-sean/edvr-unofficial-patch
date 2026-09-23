#pragma once
// Pure logic for the transition-flash engine fix (docs/design-transition-
// flash-engine-fix-2026-09-23.md). No Windows header, no game memory, no
// CodeHook -- everything here takes plain data in and hands plain data
// back, so tools\transition_flash_prevent_test can drive it without the
// game. transition_flash_prevent.cpp is the only includer that also touches
// the process.
#include <cmath>
#include <cstdint>

namespace edvr {
namespace tfp {

// ---------------------------------------------------------------------------
// Pose comparison. A pose is the 16-double block FUN_143cee650's identity
// init writes: 4 rows of 4, rotation at [r*4+c] for r,c in 0..2, translation
// at [12],[13],[14]. Lanes [3],[7],[11],[15] are PADDING -- uninitialised in
// the identity write -- and every comparison below ignores them.
struct PoseDelta {
    double dt = 0.0;  // Euclidean translation difference, metres
    double dr = 0.0;  // max abs difference over the 3x3 rotation block
};

inline PoseDelta comparePose(const double cached[16], const double recompute[16]) noexcept {
    PoseDelta d;
    double sq = 0.0;
    for (int a = 12; a <= 14; ++a) {
        const double e = cached[a] - recompute[a];
        sq += e * e;
    }
    // classify() compares against metre thresholds, so this needs the real
    // distance, not its square; at most 512 calls a frame (the recompute
    // budget), so std::sqrt here is nothing.
    d.dt = std::sqrt(sq);
    double maxAbs = 0.0;
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            const int i = r * 4 + c;
            double e = cached[i] - recompute[i];
            if (e < 0) e = -e;
            if (e > maxAbs) maxAbs = e;
        }
    }
    d.dr = maxAbs;
    return d;
}

enum class PoseClass : uint8_t { Agree = 0, Near = 1, Disagree = 2 };

// agree: dt<1e-3 m and dr<1e-5. disagree: dt>0.05 or dr>1e-3. else near.
inline PoseClass classify(const PoseDelta& d) noexcept {
    if (d.dt < 1e-3 && d.dr < 1e-5) return PoseClass::Agree;
    if (d.dt > 0.05 || d.dr > 1e-3) return PoseClass::Disagree;
    return PoseClass::Near;
}

// ---------------------------------------------------------------------------
// Validation: at least 60 agreements and disagreements under 5% of
// (agree+disagree) so far this session. "near" does not count either way.
struct ValidationCounts {
    uint64_t agree = 0;
    uint64_t disagree = 0;
};

inline bool isValidated(const ValidationCounts& c) noexcept {
    if (c.agree < 60) return false;
    const uint64_t total = c.agree + c.disagree;
    if (total == 0) return false;
    // c.disagree < 0.05 * total, without floating point on the count path:
    // 20 * disagree < total.
    return 20ull * c.disagree < total;
}

// ---------------------------------------------------------------------------
// Per-parent guard table: is THIS parent in good enough standing to act on.
// A small fixed table, LRU by parent pointer, evicted on the frame least
// recently touched -- capacity kCapacity, matching the "small table, ~256"
// of the design.
class ParentGuardTable {
public:
    static constexpr uint32_t kCapacity = 256;
    // "new" if not seen in this many frames (or never seen at all).
    static constexpr uint32_t kUnseenFramesForNew = 90;
    // "agreed within the last 3 frames".
    static constexpr uint32_t kRecentAgreeFrames = 3;
    // consecutive-disagree streak has to stay at or under this to act.
    static constexpr uint32_t kMaxStreak = 3;

    struct Verdict {
        bool eligible = false;          // may this parent be acted on right now
        bool justCrossedPersistent = false;  // log the persistent-disagree line once
    };

    // Call once per classified decode-from-compose call (agree, near or
    // disagree), in frame order per parent. Returns whether the parent was
    // eligible to act coming INTO this call (the streak/recency state before
    // this call's own class is folded in), which is what the event latch at
    // the first disagreement of a new event should act on.
    Verdict onClassified(uint64_t parent, uint32_t frame, PoseClass cls) noexcept {
        Entry& e = findOrEvict(parent);
        const bool wasNew = !e.used || (frame - e.lastSeenFrame) > kUnseenFramesForNew;
        // A parent that goes quiet for the "new" window carries no stale
        // streak into its return -- without this, one persistent-disagree
        // crossing early in a session would block that parent for good,
        // which is a worse failure than the false positive it guards
        // against. A truly fresh entry already reads streak==0 here.
        if (wasNew) { e.streak = 0; e.loggedPersistent = false; }
        const bool agreedRecently =
            e.hasAgreed && (frame - e.lastAgreeFrame) <= kRecentAgreeFrames;
        Verdict v;
        v.eligible = (wasNew || agreedRecently) && e.streak <= kMaxStreak;

        e.used = true;
        e.parent = parent;
        e.lastSeenFrame = frame;
        if (cls == PoseClass::Agree) {
            e.hasAgreed = true;
            e.lastAgreeFrame = frame;
            e.streak = 0;
            e.loggedPersistent = false;
        } else if (cls == PoseClass::Disagree) {
            if (e.streak < 0xFFFFFFFFu) ++e.streak;
            if (e.streak > kMaxStreak && !e.loggedPersistent) {
                e.loggedPersistent = true;
                v.justCrossedPersistent = true;
            }
        }
        // Near: bookkeeping only (lastSeenFrame above); neither resets nor
        // extends the disagree streak -- it is neither an agreement nor one
        // of the disagreements the streak exists to bound.
        return v;
    }

    uint32_t sizeForTest() const noexcept {
        uint32_t n = 0;
        for (const auto& e : table_) if (e.used) ++n;
        return n;
    }

private:
    struct Entry {
        uint64_t parent = 0;
        uint32_t lastSeenFrame = 0;
        uint32_t lastAgreeFrame = 0;
        uint32_t streak = 0;
        bool used = false;
        bool hasAgreed = false;
        bool loggedPersistent = false;
    };
    Entry table_[kCapacity]{};

    // LRU by each entry's OWN lastSeenFrame (set by the caller on every
    // touch), not by the frame of this particular call -- so eviction does
    // not need the current frame at all.
    Entry& findOrEvict(uint64_t parent) noexcept {
        int freeSlot = -1;
        int oldestSlot = 0;
        uint32_t oldestFrame = 0xFFFFFFFFu;
        for (uint32_t i = 0; i < kCapacity; ++i) {
            Entry& e = table_[i];
            if (e.used && e.parent == parent) return e;
            if (!e.used && freeSlot < 0) freeSlot = static_cast<int>(i);
            if (e.lastSeenFrame < oldestFrame) { oldestFrame = e.lastSeenFrame; oldestSlot = static_cast<int>(i); }
        }
        const int slot = freeSlot >= 0 ? freeSlot : oldestSlot;
        table_[slot] = Entry{};
        return table_[slot];
    }
};

// ---------------------------------------------------------------------------
// Mode, parsed from advanced.transition_flash_prevent.
enum class Mode : uint8_t { Off = 0, Watch = 1, On = 2, Alternate = 3 };

// Case-insensitive against off/watch/on/alternate; unrecognised text falls
// back to Off and reports itself unrecognised, the uiQualityParse shape.
inline Mode parseMode(const char* text, bool* recognized) noexcept {
    if (recognized) *recognized = true;
    if (!text) { if (recognized) *recognized = false; return Mode::Off; }
    auto eq = [](const char* a, const char* b) noexcept {
        while (*a && *b) {
            char ca = *a, cb = *b;
            if (ca >= 'A' && ca <= 'Z') ca = char(ca - 'A' + 'a');
            if (cb >= 'A' && cb <= 'Z') cb = char(cb - 'A' + 'a');
            if (ca != cb) return false;
            ++a; ++b;
        }
        return *a == *b;
    };
    if (eq(text, "off")) return Mode::Off;
    if (eq(text, "watch")) return Mode::Watch;
    if (eq(text, "on")) return Mode::On;
    if (eq(text, "alternate")) return Mode::Alternate;
    if (recognized) *recognized = false;
    return Mode::Off;
}

enum class Treatment : uint8_t { Watch = 0, Act = 1 };

// alternate: first event watched, second acted, third watched... (1-indexed).
inline Treatment alternateTreatmentFor(uint32_t eventNumber) noexcept {
    return (eventNumber % 2 == 0) ? Treatment::Act : Treatment::Watch;
}

// Whether the mode alone (before validation/per-parent/session guards) makes
// this event a candidate to act: mode on, or alternate on an "acted" slot.
inline bool modeAllowsActing(Mode mode, uint32_t eventNumber) noexcept {
    if (mode == Mode::On) return true;
    if (mode == Mode::Alternate) return alternateTreatmentFor(eventNumber) == Treatment::Act;
    return false;
}

// Session-wide acted-frame cap: a plain arithmetic guard, pulled out for its
// own test cell since it is easy to get the boundary wrong (< vs <=).
inline bool sessionCapReached(uint64_t actedFrames, uint64_t cap = 200) noexcept {
    return actedFrames >= cap;
}

// ---------------------------------------------------------------------------
// Event grouping: a disagreement more than kGapFrames after the previous one
// starts a new, numbered event. The treatment decided for an event is
// latched so every call in the event -- both eyes, every frame of it --
// gets the same answer (frame_flag.h's SubmitPairLatch is the same shape,
// for the same reason: a decision re-taken mid-transition is what splits a
// pair of simultaneous callers).
class EventTracker {
public:
    static constexpr uint32_t kGapFrames = 90;

    struct Note {
        uint32_t eventNumber = 0;
        bool isNewEvent = false;
    };

    // Call on every disagreeing decode-from-compose call, frame-ordered.
    Note note(uint32_t frame) noexcept {
        const bool isNew = !any_ || (frame - lastFrame_) > kGapFrames;
        if (isNew) {
            ++eventNumber_;
            latched_ = false;
        }
        any_ = true;
        lastFrame_ = frame;
        return {eventNumber_, isNew};
    }

    // Called exactly once per event, when note() answers isNewEvent, after
    // the caller has evaluated mode/alternate/validated/per-parent guards
    // for that first disagreement.
    void latch(Treatment t) noexcept { treatment_ = t; latched_ = true; }
    bool isLatched() const noexcept { return latched_; }
    Treatment treatment() const noexcept { return treatment_; }
    uint32_t currentEvent() const noexcept { return eventNumber_; }

private:
    bool any_ = false;
    uint32_t lastFrame_ = 0;
    uint32_t eventNumber_ = 0;
    bool latched_ = false;
    Treatment treatment_ = Treatment::Watch;
};

// ---------------------------------------------------------------------------
// Finding 1 of the 2026-09-23 review: EventTracker::treatment() above
// decides only the EVENT's watched/acted answer, so every call inside one
// event -- both eyes, every parent that happens to disagree while it is
// open -- reads the same thing. Whether THIS call may actually overwrite
// the compose's output is a second, per-call question: this call's own
// parent must currently read eligible from ParentGuardTable::onClassified,
// and the session cap must not already be reached. Without this, a parent
// that has been disagreeing for a while gets silently overwritten the
// moment some OTHER, well-behaved parent happens to open an event.
inline bool callMayAct(Treatment eventTreatment, bool guardEligible, bool sessionCapReached) noexcept {
    return eventTreatment == Treatment::Act && guardEligible && !sessionCapReached;
}

// Finding 3: the session-wide acted-frame cap (sessionCapReached above) has
// to count distinct acted FRAMES, not acted CALLS -- several calls, both
// eyes and sometimes more than one parent, can all act within one frame.
// Pure comparison: is `frame` one the caller has not already counted?
inline bool isNewActedFrame(uint32_t frame, uint32_t lastActedFrame, bool hasActedFrame) noexcept {
    return !hasActedFrame || frame != lastActedFrame;
}

// ---------------------------------------------------------------------------
// Ring window selection: does a ring entry stamped `frame` belong in the
// dump window [triggerFrame-before, triggerFrame+after]? Saturating on the
// low side and widened on the high side so a trigger near frame 0 or near
// UINT32_MAX cannot wrap the comparison -- the exact footgun documented
// against advanced.camera_buffer_offset in glitch_frame.cpp.
inline bool ringFrameInWindow(uint32_t frame, uint32_t triggerFrame,
                              uint32_t before = 30, uint32_t after = 30) noexcept {
    const uint32_t lo = triggerFrame > before ? triggerFrame - before : 0;
    const uint64_t hi = uint64_t(triggerFrame) + uint64_t(after);
    return frame >= lo && uint64_t(frame) <= hi;
}

// ---------------------------------------------------------------------------
// Finding 4: dump-request merging. An automatic trigger (our own event, the
// detector's verdict) is serviced kDumpWindowFrames after it fires, so by
// the time the dump actually runs the ring already holds the frames up to
// triggerFrame+kDumpWindowFrames and the window
// [triggerFrame-kDumpWindowFrames, triggerFrame+kDumpWindowFrames] is
// complete. A second trigger arriving before a pending dump is serviced
// must not be dropped -- foldDumpTrigger widens the same pending dump
// instead, and frameInDumpWindow decides which ring entries the resulting
// dump keeps. The Pause/history-key trigger is different (it wants the
// WHOLE ring, not a window) and is not built from these two -- see the
// wholeRing flag the .cpp keeps beside its own PendingDumpWindow.
constexpr uint32_t kDumpWindowFrames = 30;

struct PendingDumpWindow {
    bool active = false;
    uint32_t lowTriggerFrame = 0;  // earliest trigger folded into this dump
    uint32_t dueFrame = 0;         // latest trigger+defer folded in; serviced here
};

inline PendingDumpWindow foldDumpTrigger(const PendingDumpWindow& current, uint32_t triggerFrame,
                                          uint32_t deferFrames) noexcept {
    const uint32_t due = triggerFrame + deferFrames;
    if (!current.active) return PendingDumpWindow{true, triggerFrame, due};
    PendingDumpWindow next = current;
    if (triggerFrame < next.lowTriggerFrame) next.lowTriggerFrame = triggerFrame;
    if (due > next.dueFrame) next.dueFrame = due;
    return next;
}

// Whether a ring entry stamped `frame` belongs in the dump the pending
// window above will produce: from lowTriggerFrame-before (saturating, the
// same underflow guard ringFrameInWindow uses above) up to dueFrame itself,
// inclusive.
inline bool frameInDumpWindow(uint32_t frame, uint32_t lowTriggerFrame, uint32_t dueFrame,
                               uint32_t before = kDumpWindowFrames) noexcept {
    const uint32_t lo = lowTriggerFrame > before ? lowTriggerFrame - before : 0;
    return frame >= lo && frame <= dueFrame;
}

// ---------------------------------------------------------------------------
// H3 (finding 2 of the 2026-09-23 review): the design's link check between
// the engine's last few pushed translations and what glitch_frame.cpp
// separately reads off the scene camera. Comparing against a SINGLE last
// push floods "mismatch" the moment the sim runs a frame ahead of the
// render thread -- a pipelined push then never lines up with the
// observation that follows it. Keeping the last 4 gives the observation a
// real chance to match whichever of them it actually belongs to.
class RecentPushes {
public:
    static constexpr uint32_t kCount = 4;

    // Most recent first: after push(), slot 0 is what was just pushed: the
    // previous slot 0..2 shift down and slot 3 (once full) is dropped.
    void push(const double t[3]) noexcept {
        for (uint32_t i = kCount - 1; i > 0; --i) {
            slot_[i][0] = slot_[i - 1][0];
            slot_[i][1] = slot_[i - 1][1];
            slot_[i][2] = slot_[i - 1][2];
        }
        slot_[0][0] = t[0]; slot_[0][1] = t[1]; slot_[0][2] = t[2];
        if (filled_ < kCount) ++filled_;
    }

    bool hasAny() const noexcept { return filled_ > 0; }
    uint32_t filled() const noexcept { return filled_; }

    void translationAt(uint32_t slot, double out[3]) const noexcept {
        if (slot >= filled_) { out[0] = out[1] = out[2] = 0.0; return; }
        out[0] = slot_[slot][0]; out[1] = slot_[slot][1]; out[2] = slot_[slot][2];
    }

    // The minimum Euclidean distance from `pos` to any push held so far, and
    // which slot (0 = most recent) produced it. hasAny() must be true.
    double minDistance(const float pos[3], uint32_t* slotOut) const noexcept {
        double best = 0.0; uint32_t bestSlot = 0;
        for (uint32_t i = 0; i < filled_; ++i) {
            double sq = 0.0;
            for (int a = 0; a < 3; ++a) {
                const double e = double(pos[a]) - slot_[i][a];
                sq += e * e;
            }
            const double d = std::sqrt(sq);
            if (i == 0 || d < best) { best = d; bestSlot = i; }
        }
        if (slotOut) *slotOut = bestSlot;
        return best;
    }

private:
    double slot_[kCount][3] = {};
    uint32_t filled_ = 0;
};

// Within one frame, many observation calls can each test against the last 4
// pushes (glitchFrameObserve runs for every observed 5376-byte constant
// buffer, not just the eye camera's); the frame's verdict is its BEST
// (smallest-distance) match, not its first or its last. Pure comparison so
// "is this candidate the new best" is tested on its own.
inline bool isNewH3Best(bool hasComparisonYet, double currentBest, double candidate) noexcept {
    return !hasComparisonYet || candidate < currentBest;
}

enum class H3Bucket : uint8_t { Under1Cm = 0, Under10Cm = 1, Under1M = 2, OneMPlus = 3, NoObservation = 4 };

// Classifies one finished frame's best H3 match. "No observation / no push"
// covers both a frame with no 5376-byte buffer observed at all, and one with
// observations but nothing yet pushed to compare against (RecentPushes
// empty) -- both say nothing about whether the link held this frame.
inline H3Bucket classifyH3Frame(bool hasObservation, bool hasComparison, double minDist) noexcept {
    if (!hasObservation || !hasComparison) return H3Bucket::NoObservation;
    if (minDist < 0.01) return H3Bucket::Under1Cm;
    if (minDist < 0.10) return H3Bucket::Under10Cm;
    if (minDist < 1.0) return H3Bucket::Under1M;
    return H3Bucket::OneMPlus;
}

}  // namespace tfp
}  // namespace edvr
