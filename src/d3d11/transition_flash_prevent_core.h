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

}  // namespace tfp
}  // namespace edvr
