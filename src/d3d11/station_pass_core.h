#pragma once
// The station's pass rules (built v14.27, 2026-10-13). Pure: no Windows, no engine, so the rig drives them with scripted and random interleavings.
// The v14.26 tick read the gathered entries of "the previous parity" and treated whatever it found as the frame: a pass with no store callbacks in it, or
// with only part of a frame's, read as "the own pad is absent and the camera unknown", and the pad's positions read across such passes read as motion.
// These are the rules that replace it:
//   PassGate: the verdict on one drain. A pass is INFORMATIVE only when it is complete (no writer was still inside) and it held at least one store
//     callback. An empty or unfinished pass is no evidence: the tick holds the pad, the camera, the latch samples and the rig publication as they were.
//   SeenWindow: the pads and explicit parts the passes saw, each kept for kKeep informative passes after the one that saw it. A pass that carries only
//     part of a frame's callbacks (the frame's burst split across the tick) cannot make a record absent: absent means unseen for the whole window.
//   CamHold: the camera is held through a failed resolution for kCamHoldTicks informative passes, then it is unknown.
//   StableGate: a change counts once it has held for a number of ticks (the waiting line writes on stable changes only).
#include <cstddef>
#include <cstdint>

namespace edvr::sealed {

enum class PassVerdict : uint8_t { kInformative, kEmpty, kDeferred };

constexpr uint32_t kSeenKeep = 2;        // informative passes a seen record stays after the pass that saw it
constexpr uint32_t kCamHoldTicks = 30;   // informative passes a failed camera resolution is covered by the last good camera
constexpr uint32_t kWaitStableTicks = 8; // ticks a waiting-line change must hold before the line is written

struct PassGate {
    uint64_t informative = 0, empty = 0, deferred = 0, thin = 0;   // the window's counts
    uint64_t callsMin = ~0ull, callsMax = 0, callsSum = 0;         // calls per informative pass, this window
    uint32_t refCalls = 0;                                         // the usual calls in an informative pass (a slow average)
    uint32_t run = 0;                                              // informative passes in a row

    // The verdict for one drain: complete (no writer was inside), the passes it closed and the callbacks they held.
    PassVerdict step(bool complete, uint32_t passes, uint64_t calls) noexcept {
        if (!complete) {
            ++deferred;
            run = 0;
            return PassVerdict::kDeferred;
        }
        if (passes == 0 || calls == 0) {
            ++empty;
            run = 0;
            return PassVerdict::kEmpty;
        }
        ++informative;
        ++run;
        if (refCalls != 0 && calls * 2 < refCalls) ++thin;   // under half the usual: informative, but the window covers for it
        const uint32_t c = calls > 0xFFFFFFFFull ? 0xFFFFFFFFu : static_cast<uint32_t>(calls);
        refCalls = refCalls == 0 ? c : static_cast<uint32_t>(static_cast<int64_t>(refCalls) + (static_cast<int64_t>(c) - static_cast<int64_t>(refCalls)) / 8);
        if (refCalls == 0) refCalls = 1;
        if (calls < callsMin) callsMin = calls;
        if (calls > callsMax) callsMax = calls;
        callsSum += calls;
        return PassVerdict::kInformative;
    }
    void clearWindow() noexcept {
        informative = empty = deferred = thin = 0;
        callsMin = ~0ull;
        callsMax = callsSum = 0;
    }
};

// T needs a member `addr`. put() keeps one entry per address (the newest reading); endPass() ages every entry and drops those older than kSeenKeep.
template <typename T, size_t N>
struct SeenWindow {
    T items[N] = {};
    uint8_t age[N] = {};
    size_t n = 0;

    void put(const T& v) noexcept {
        for (size_t i = 0; i < n; ++i) {
            if (items[i].addr == v.addr) {
                items[i] = v;
                age[i] = 0;
                return;
            }
        }
        if (n < N) {
            items[n] = v;
            age[n] = 0;
            ++n;
            return;
        }
        size_t o = 0;
        for (size_t i = 1; i < n; ++i) {
            if (age[i] > age[o]) o = i;
        }
        items[o] = v;
        age[o] = 0;
    }
    bool has(uintptr_t addr) const noexcept {
        for (size_t i = 0; i < n; ++i) {
            if (items[i].addr == addr) return true;
        }
        return false;
    }
    // After an informative pass has been used: everything ages by one; what is older than the window goes.
    void endPass() noexcept {
        size_t w = 0;
        for (size_t i = 0; i < n; ++i) {
            if (static_cast<uint32_t>(age[i]) + 1 > kSeenKeep) continue;
            if (w != i) {
                items[w] = items[i];
            }
            age[w] = static_cast<uint8_t>(age[i] + 1);
            ++w;
        }
        n = w;
    }
    void clear() noexcept { n = 0; }
};

struct CamHold {
    uint32_t failRun = 0;
    bool valid = false;
    // One resolution attempt in an informative pass. Returns whether a camera is known afterwards.
    bool resolved(bool ok) noexcept {
        if (ok) {
            failRun = 0;
            valid = true;
        } else if (++failRun > kCamHoldTicks) {
            valid = false;
        }
        return valid;
    }
};

// Built v14.33: the mechanism release is armed only after the seal has seen every latched bay record still for kMechArmPasses informative passes in a row
// (about 1 s). A planetary port reports r1 1 while the gate ramps are still closing: poses taken at the seal were mid-motion and the closing read as a
// launch (flight 1718577b). Until it arms, mechanism motion neither releases nor counts; every other release works from the first frame. The sealed
// poses the re-seal compares against are taken when it arms.
constexpr uint32_t kMechArmPasses = 60;

struct MechArm {
    uint32_t run = 0;
    bool armed = false;
    void reset() noexcept {
        run = 0;
        armed = false;
    }
    // One informative pass while sealed. allStill: every latched record was read this pass and none moved. True on the pass it arms.
    bool step(bool allStill) noexcept {
        if (armed) return false;
        run = allStill ? run + 1 : 0;
        if (run >= kMechArmPasses) {
            armed = true;
            return true;
        }
        return false;
    }
};

struct StableGate {
    uint32_t run = 0;
    // changed: this tick's value differs from the last tick's. True on the tick the new value has held for `need` ticks.
    bool step(bool changed, uint32_t need) noexcept {
        run = changed ? 0 : run + 1;
        return run == need;
    }
};

}  // namespace edvr::sealed
