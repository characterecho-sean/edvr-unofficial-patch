// The flat camera-write witness's stack walks, bounded.
//
// The witness (flat_runtime.cpp, cameraWitness) captures the writer's stack once per unique game call
// site -- passive evidence for where the game's camera producers are (design-flat-camera-integration.md,
// C0/C1). Each capture is a CaptureStackBackTrace plus, for every frame it looks at, a VirtualQuery and
// a GetModuleFileNameA, on the render thread, in the path of every camera constant-buffer write. The
// motion-CPU review (reviews/flat-motion-cpu-review-2026-09-29.md, C1) found that it stopped only when
// all 16 site slots were claimed, which a game with fewer producers never does: the walk ran on every
// camera write for the whole session, at thousands of writes a second.
//
// Now a walk is asked for only while the witness is armed, and it disarms itself:
//   - after kFlatWitnessStableWalks walks in a row that learned nothing (no new site, no new buffer at
//     a known one): the producer population is presumed stable, or
//   - after kFlatWitnessWalkBudget walks in all, whatever they learned.
// The write counts never stop. An F10 audit re-arms the walks (and forgets the sites, so the audit's
// window reports the producers it sees, not the ones from minutes ago).
//
// Pure, so tools\flat_temporal_test can hold the bound: a hundred thousand writes at one known site ask
// for a few dozen walks, not a hundred thousand.
#pragma once

#include <cstdint>

namespace edvr {

constexpr uint32_t kFlatWitnessStableWalks = 32;   // consecutive walks that learned nothing
constexpr uint32_t kFlatWitnessWalkBudget = 128;   // walks in all, per arm

enum class FlatWitnessStop : uint8_t { None, Stable, Budget };

struct FlatWitnessBound {
    uint32_t walks = 0;       // stack walks since armed
    uint32_t stableRun = 0;   // consecutive walks that learned nothing
    bool stopped = false;
    FlatWitnessStop why = FlatWitnessStop::None;

    // Whether the write in hand should walk the stack.
    bool wantsWalk() const { return !stopped; }
    // After a walk. `learned`: it found a site or a buffer the witness did not know. Returns the reason
    // this walk disarmed the witness, or None.
    FlatWitnessStop noteWalk(bool learned) {
        if (stopped) return FlatWitnessStop::None;
        ++walks;
        stableRun = learned ? 0 : stableRun + 1;
        if (stableRun >= kFlatWitnessStableWalks) return stop(FlatWitnessStop::Stable);
        if (walks >= kFlatWitnessWalkBudget) return stop(FlatWitnessStop::Budget);
        return FlatWitnessStop::None;
    }
    void rearm() {
        walks = 0;
        stableRun = 0;
        stopped = false;
        why = FlatWitnessStop::None;
    }
    static const char* stopName(FlatWitnessStop why) {
        return why == FlatWitnessStop::Stable ? "the producer sites were stable"
             : why == FlatWitnessStop::Budget ? "the walk budget is spent" : "armed";
    }

private:
    FlatWitnessStop stop(FlatWitnessStop reason) {
        stopped = true;
        why = reason;
        return reason;
    }
};

}  // namespace edvr
