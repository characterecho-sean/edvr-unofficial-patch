#pragma once

// Owner selection between the legacy qualified-jitter path and the upstream
// camera injector, per docs/design-flat-camera-integration.md's C2-C table
// (revised per the C2-plan review's R5). One owner is selected BEFORE any
// mutation in the frame; the losing route's graphics AND compute mutations
// are suppressed; safe outcomes are named; ownership switches wait for
// outstanding work; per-view-group ownership never lets frame-wide counts
// hide a mixed frame.
//
// This header is the production-intended policy: C3 wires it into
// flat_runtime.cpp where refuseDraw/observing decide today
// (flat_runtime.cpp:970-986, 1830). The legacy semantics those lines encode
// are the inputs' contract, cited where they apply. Header-only and pure:
// the policy allocates nothing and performs no I/O, so it can run on the
// render thread and in the offline coexistence rig unchanged.

#include <cstdint>

namespace edvr {

// The routes that can jitter a frame.
enum class FlatCameraOwner : uint8_t { None, Legacy, Upstream };

// What a frame gets when no route may mutate it, or when mutation completes.
// All are valid states; a stable supported scene must make bounded progress
// out of the non-terminal ones.
enum class FlatCameraOutcome : uint8_t {
    Treated,     // an owner was selected and mutation proceeded
    Observing,   // collecting evidence; refusal-free frames requalify
    Unprepared,  // evidence complete, resources not yet negotiated
    Unsupported, // a named-unproven domain (e.g. projection kinds 4/5)
    Recovery,    // history invalidated; bounded re-warming in progress
};

// One view-group's inputs for the selection. Groups: main, cockpit,
// auxiliary (shadow/reflection/env passes). Per the setter map, auxiliary
// domains own and re-dirty their own camera structs per pass; they are
// separate groups, never summed into frame-wide counts.
struct FlatCameraGroupInput {
    bool upstreamCertified = false;   // the producer chain for this group's camera is
                                      // the mapped refresh and its projection kind is
                                      // in the proven set (1 ortho, 3 trig, default)
    bool upstreamUnsupported = false; // an explicitly named unproven domain (kinds 4/5)
    bool legacyEligible = false;      // the legacy selector would treat this group
    bool legacyObserving = false;     // the legacy selector is in observation here
    bool lateProjectionRewrite = false; // a true late rewrite was observed this frame
    bool uncoveredSecondary = false;  // an uncovered secondary camera was observed
};

struct FlatCameraOwnershipDecision {
    FlatCameraOwner owner = FlatCameraOwner::None;
    bool legacyGraphicsSuppressed = false; // legacy draw-path scope mutation off
    bool legacyComputeSuppressed = false;  // legacy dispatch-path scope mutation off
    FlatCameraOutcome outcome = FlatCameraOutcome::Observing;
    const char* reason = "";
    bool historyReset = false;          // closure/history invalidated this decision
    bool preserveCameraInputs = false;  // original inputs kept for motion reconstruction
};

// Cross-frame ownership state, per view-group. Two identities are kept
// strictly separate (the C2-work review's R3): the owner of the most recent
// CLEANLY CLOSED frame (history identity) and the work selected/applied in
// the CURRENT frame (which a mid-frame re-selection must not mix with).
struct FlatCameraOwnershipState {
    FlatCameraOwner lastOwner = FlatCameraOwner::None;
    FlatCameraOwner current = FlatCameraOwner::None;
    bool appliedThisFrame = false;
    bool historyValid = false;
};

// The frame boundary. Last frame's work is RETIRED here: a completed frame
// never blocks a later switch (the review's "closed legacy work blocks
// upstream indefinitely" defect came from missing exactly this transition).
inline void flatCameraOwnerBegin(FlatCameraOwnershipState& s) {
    s.current = FlatCameraOwner::None;
    s.appliedThisFrame = false;
}

// A mutation reached a draw this frame (flat_live_phase.h:49-52's
// noteApplied discipline). The application is also the adoption: the
// frame's owner becomes the decision's, and a later re-selection in the
// same frame must not change owner once this is set -- the mixed-phase
// guard.
inline void flatCameraOwnerNoteApplied(FlatCameraOwnershipState& s,
                                       const FlatCameraOwnershipDecision& d) {
    if (d.owner != FlatCameraOwner::None) s.current = d.owner;
    s.appliedThisFrame = true;
}

// The selection, called once per group per frame BEFORE any mutation (the
// consumption boundary the C2 plan's A6 split names). Pure: no allocation,
// no globals, no side effects beyond the returned decision.
inline FlatCameraOwnershipDecision flatCameraOwnerSelect(
    const FlatCameraOwnershipState& s, const FlatCameraGroupInput& in) {
    FlatCameraOwnershipDecision d;

    // A true late rewrite or an uncovered secondary camera invalidates
    // closure/history and produces a named safe outcome, never an owner
    // change riding on poisoned evidence (flat_runtime.cpp:970-986's
    // refuseDraw semantics: the frame fails coherently and observation
    // resumes).
    if (in.lateProjectionRewrite || in.uncoveredSecondary) {
        d.outcome = FlatCameraOutcome::Recovery;
        d.reason = in.lateProjectionRewrite ? "late-projection-rewrite"
                                            : "uncovered-secondary-camera";
        d.historyReset = true;
        d.preserveCameraInputs = true;
        return d;
    }

    // An explicitly named unproven domain: upstream never owns, by name;
    // the legacy route may still own if its own selector is eligible and no
    // upstream work already applied this frame.
    if (in.upstreamUnsupported) {
        if (in.legacyEligible && !(s.current == FlatCameraOwner::Upstream && s.appliedThisFrame)) {
            d.owner = FlatCameraOwner::Legacy;
            d.outcome = FlatCameraOutcome::Treated;
            d.reason = "legacy-eligible-upstream-unsupported";
            // The same history-identity switch the other two Legacy/Upstream
            // branches make: the fallback hysteresis reaches Legacy through here,
            // and the frames before it were Upstream's. One-shot by construction:
            // the first clean close under Legacy moves lastOwner and ends it.
            if (s.lastOwner == FlatCameraOwner::Upstream) {
                d.historyReset = true;
                d.preserveCameraInputs = true;
            }
        } else {
            d.outcome = FlatCameraOutcome::Unsupported;
            d.reason = "upstream-unsupported-projection-kind";
        }
        return d;
    }

    if (in.upstreamCertified) {
        // Mid-frame mix guard: legacy work already applied this frame keeps
        // the frame's owner; switching now would mix phases.
        if (s.current == FlatCameraOwner::Legacy && s.appliedThisFrame) {
            d.owner = FlatCameraOwner::Legacy;
            d.outcome = FlatCameraOutcome::Treated;
            d.reason = "switch-deferred-applied-legacy";
            return d;
        }
        d.owner = FlatCameraOwner::Upstream;
        d.outcome = FlatCameraOutcome::Treated;
        d.reason = in.legacyEligible ? "upstream-certified-legacy-suppressed"
                                     : "upstream-certified-legacy-not-eligible";
        // Suppression follows the SELECTED OWNER for every mutation in this
        // group, independently of overall legacy eligibility: an unqualified
        // group can still contain individually qualified graphics/compute
        // draws, and under upstream ownership none of them may mutate
        // (the review's contract correction).
        d.legacyGraphicsSuppressed = true;
        d.legacyComputeSuppressed = true;
        if (s.lastOwner == FlatCameraOwner::Legacy) {
            // A switch of history identity: history resets, and the original
            // camera inputs are preserved so motion reconstruction still has
            // an unjittered reference.
            d.historyReset = true;
            d.preserveCameraInputs = true;
        }
        return d;
    }

    if (in.legacyEligible) {
        // The reverse guard, symmetric: upstream work already applied this
        // frame keeps the frame's owner.
        if (s.current == FlatCameraOwner::Upstream && s.appliedThisFrame) {
            d.owner = FlatCameraOwner::Upstream;
            d.outcome = FlatCameraOutcome::Treated;
            d.reason = "switch-deferred-applied-upstream";
            d.legacyGraphicsSuppressed = true;
            d.legacyComputeSuppressed = true;
            return d;
        }
        d.owner = FlatCameraOwner::Legacy;
        d.outcome = FlatCameraOutcome::Treated;
        d.reason = "legacy-eligible-no-upstream";
        if (s.lastOwner == FlatCameraOwner::Upstream) {
            d.historyReset = true;
            d.preserveCameraInputs = true;
        }
        return d;
    }
    d.outcome = FlatCameraOutcome::Observing;
    d.reason = in.legacyObserving ? "legacy-observing-no-upstream"
                                  : "no-route-eligible";
    return d;
}

// The frame close for one group. A failed closure ALWAYS invalidates
// history (FlatLivePhase::finish's rejection semantics); a clean closure
// moves the frame's owner into the history identity.
inline void flatCameraOwnerClose(FlatCameraOwnershipState& s,
                                 const FlatCameraOwnershipDecision& d, bool applied,
                                 bool cleanClosure) {
    if (d.owner != FlatCameraOwner::None) s.current = d.owner;
    s.appliedThisFrame = s.appliedThisFrame || applied;
    if (cleanClosure) {
        if (d.owner != FlatCameraOwner::None) s.lastOwner = d.owner;
        s.historyValid = true;
    } else {
        s.historyValid = false;
    }
}

} // namespace edvr
