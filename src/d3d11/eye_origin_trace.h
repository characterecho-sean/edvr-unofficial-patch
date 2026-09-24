#pragma once
// Pure logic for advanced.eye_origin_trace: no Windows header, no game
// memory -- takes plain data in and hands plain data back, so
// tools\glitch_test can drive it without the game. glitch_frame.cpp is the
// only includer that also captures a real call stack or touches a file.
//
// WHY this exists: at a transition (hyperspace/high wake, low wake,
// supercruise) the eye's origin -- the scene constant buffer's float index
// 1100 (cb1[275], a 5376-byte buffer bound at VS slot b1) -- collapses to
// the head pose alone for one frame, and nobody knows which game code
// writes that value. A static address guess was flown and refuted. The
// robust anchor is the game's own call stack at the moment it writes the
// buffer the eye draws read -- glitch_frame.cpp already sees every such
// write (glitchFrameObserve); this header is what turns a pile of raw call
// stacks into something a flight log can show a human: a small table of
// the DISTINCT stacks seen, and a bounded automatic capture around the
// frames the detector would have withheld.
#include <cstdint>
#include <cstring>

namespace edvr {
namespace eot {

// ---------------------------------------------------------------------------
// The unique-stack table. One entry per DISTINCT '/'-joined RVA chain
// (game_call_probe.h's GameCallStack::rvas) observed at a camera-buffer
// write, deduped by content -- the FNV-1a-64 hash is the fast filter, the
// full string decides an exact match, so a hash collision opens a new
// entry rather than silently merging two different stacks' evidence.
// Capped at kMaxStacks: past that, intern() reports kStackOverflowId and
// counts the overflow instead of growing without bound, so a session that
// hammers a noisy call site cannot turn a bounded diagnostic into an
// unbounded one.
constexpr uint32_t kMaxStacks = 64;
constexpr uint8_t  kStackOverflowId = 255;  // on, but the table was already full
constexpr uint8_t  kStackNoneId     = 254;  // tracing was off, or nothing matched yet
constexpr uint32_t kStackChainCap   = 512;  // mirrors GameCallStack::rvas

struct StackEntry {
    uint64_t hash = 0;
    uint32_t count = 0;      // writes that hashed (and compared equal) here
    uint32_t eyeFrames = 0;  // frames this stack was the MATCHED eye-origin write
    char     chain[kStackChainCap] = {};  // exemplar: the first stack that landed here
};

class StackTable {
public:
    uint32_t used() const noexcept { return used_; }
    uint32_t overflowed() const noexcept { return overflow_; }
    const StackEntry& entry(uint32_t i) const noexcept { return entries_[i]; }

    // Find-or-add `chain` (a NUL-terminated RVA chain) by exact content,
    // using `hash` as the fast filter. Returns its id (0..kMaxStacks-1), or
    // kStackOverflowId when the table already holds kMaxStacks DIFFERENT
    // stacks. `chain` is stored truncated to kStackChainCap-1 bytes if
    // longer, matching GameCallStack::rvas's own bound.
    uint8_t intern(const char* chain, uint64_t hash) noexcept {
        if (!chain) chain = "";
        for (uint32_t i = 0; i < used_; ++i) {
            if (entries_[i].hash != hash) continue;
            if (std::strcmp(entries_[i].chain, chain) == 0) {
                ++entries_[i].count;
                return static_cast<uint8_t>(i);
            }
            // Same hash, different text: a real FNV-1a-64 collision. Keep
            // scanning -- it must not silently merge two different stacks.
        }
        if (used_ >= kMaxStacks) { ++overflow_; return kStackOverflowId; }
        StackEntry& e = entries_[used_];
        e.hash = hash;
        e.count = 1;
        e.eyeFrames = 0;
        std::strncpy(e.chain, chain, kStackChainCap - 1);
        e.chain[kStackChainCap - 1] = '\0';
        return static_cast<uint8_t>(used_++);
    }

    // Called once per frame a write matching this id turned out to be the
    // recognised eye draw's bound buffer (glitchFrameNoteSceneDraw). An
    // out-of-range id (kStackNoneId, kStackOverflowId, or a stale id from a
    // since-reset table) is always >= used_ here and is silently ignored --
    // the bound check IS the validity check, no separate sentinel test.
    void noteEyeOrigin(uint8_t id) noexcept {
        if (id < used_) ++entries_[id].eyeFrames;
    }

private:
    StackEntry entries_[kMaxStacks];
    uint32_t used_ = 0;
    uint32_t overflow_ = 0;
};

// ---------------------------------------------------------------------------
// A frame's stack-id mask: bit N set means stack id N wrote the camera
// buffer at least once this frame. Only ids 0..63 can ever be set -- exactly
// kMaxStacks -- so one uint64_t covers every real id with no gaps, and
// kStackNoneId/kStackOverflowId (both >= 64) never address a bit.
inline uint64_t addToMask(uint64_t mask, uint8_t stackId) noexcept {
    return stackId < 64 ? (mask | (uint64_t(1) << stackId)) : mask;
}

// ---------------------------------------------------------------------------
// Automatic dump scheduling. A verdict frame (the detector withheld it, or
// would have) schedules a dump of the ring window
// [triggerFrame-kWindowFrames, triggerFrame+kWindowFrames], serviced once
// triggerFrame+kWindowFrames has passed -- by then every frame the window
// promises is already in the ring. A second trigger arriving before that
// service widens the same window instead of losing one dump to the other.
//
// Deliberately this file's own copy of the shape transition_flash_prevent_
// core.h's PendingDumpWindow/foldDumpTrigger use (kWindowFrames=60 here
// against their 30) rather than a shared include: this instrument must keep
// working even if that module's internals move. See AGENTS.md on
// copy-culture ("kept as this file's own copy...so that file is never
// touched by a change here").
constexpr uint32_t kWindowFrames  = 60;
constexpr uint32_t kMaxAutoDumps  = 12;

struct PendingWindow {
    bool     active   = false;
    uint32_t lowFrame = 0;  // earliest trigger folded into this dump
    uint32_t dueFrame = 0;  // latest trigger+kWindowFrames; serviced here
};

inline PendingWindow foldTrigger(const PendingWindow& current, uint32_t triggerFrame) noexcept {
    const uint32_t due = triggerFrame + kWindowFrames;
    if (!current.active) return PendingWindow{true, triggerFrame, due};
    PendingWindow next = current;
    if (triggerFrame < next.lowFrame) next.lowFrame = triggerFrame;
    if (due > next.dueFrame) next.dueFrame = due;
    return next;
}

// Whether a ring entry stamped `frame` belongs in the dump the pending
// window above will produce: from lowFrame-kWindowFrames (saturating, so a
// trigger near frame 0 cannot underflow the comparison) up to dueFrame,
// inclusive.
inline bool frameInWindow(uint32_t frame, uint32_t lowFrame, uint32_t dueFrame) noexcept {
    const uint32_t lo = lowFrame > kWindowFrames ? lowFrame - kWindowFrames : 0;
    return frame >= lo && frame <= dueFrame;
}

}  // namespace eot
}  // namespace edvr
