// Does ANY feature still want to see draws?
//
// WHY THIS EXISTS
//
// beginPanelOverride opens with a forty-term condition naming every feature
// that subscribes to the draw path. It is the answer to one question -- "is
// anybody listening" -- and it was asked once per eye-pass draw, about 18k
// times a frame, almost entirely through one-line getters in other .cpp files
// that this build cannot inline (/O2, no /GL). 658 innermost samples of the
// 1349-frame window of 2026-09-22, the largest single entry in the profile.
//
// So the answer is sampled instead of recomputed. vScreenFrameBoundary walks
// the full condition once a frame and stores it here; the draw path reads one
// bool.
//
// THE DIRECTION THAT MATTERS
//
// Stale-TRUE is harmless: every feature below the gate still tests its own
// predicate, so the cost is a few microseconds of work nobody consumes.
// Stale-FALSE starves a feature, and that is the failure this file is shaped
// to prevent. The comment beginPanelOverride carries records three separate
// occasions when a feature was silently starved by that condition; a sample
// taken once a frame would have made a fourth if arming were left to chance.
//
// It is not. The boundary re-samples, AND anything that turns a subscriber on
// between two boundaries raises the gate itself through drawGateArm(). A
// spurious raise costs one frame of work; a missed one costs a census its
// first frame, which is the frame it counts from. The arming paths that must
// call it are the ones that can fire between two boundary samples:
// drawCensusRequest and drawCensusAutoRequest (the hotkey and the auto
// trigger, both reached from inside a frame), the advanced.census_at_ms
// schedule, and quadProbeRequest. A live settings change is covered whole,
// because vScreenRefreshConfig re-samples the real condition after its
// configure sweep rather than merely raising the flag.
//
// Raise it if you are not sure. That is the cheap mistake.
#pragma once

#include <atomic>

namespace edvr {

namespace detail {
// Atomic, not a plain bool, purely so the arming sites need not prove which
// thread they are on. A relaxed load is a plain mov on x86-64; the draw path
// pays nothing for the guarantee.
extern std::atomic<bool> g_drawGateWanted;
}  // namespace detail

// The draw path's one load.
inline bool drawGateWanted() {
    return detail::g_drawGateWanted.load(std::memory_order_relaxed);
}

// "Something I just armed subscribes to draws." Safe to call at any time and
// from any thread; the next frame boundary re-samples the truth.
inline void drawGateArm() {
    detail::g_drawGateWanted.store(true, std::memory_order_relaxed);
}

// The re-sample, from vScreenFrameBoundary and vScreenRefreshConfig only.
inline void drawGateSet(bool wanted) {
    detail::g_drawGateWanted.store(wanted, std::memory_order_relaxed);
}

}  // namespace edvr
