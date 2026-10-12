#pragma once
#include <cstring>

// Local refusal ("partial" temporal AA), redesigned 2026-09-26 after
// docs/review-flat-temporal-aa-2026-09-26.md findings 1, 4 and 5: the
// stamp-mask/mixed-phase form of partial AA never enabled in the flat
// profile and was unproven where it mixed unjittered geometry into a
// jittered scene. The semantics now: one raster phase across shared scene
// depth/colour. A scene draw that cannot be jittered for a reason specific
// to that one draw still goes out unjittered (the proxy cannot stop the
// game's own draw); the frame's history is invalidated and the runtime
// returns to observation until a refusal-free frame requalifies the
// contract. It is never claimed as treated. Pure, header-only policy shared
// by flat_runtime.cpp and tools/flat_temporal_test so the exact
// locally-refusable-reason set is one tested table instead of two
// hand-matched copies. See docs/design-flat-temporal-aa-2026-09-23.md.
namespace edvr {

// Certified Upstream ownership does not consume legacy recipe preparation or
// private-row bindings. Auditing those paths may report a refusal, but must
// not invalidate the raster phase or its accepted history, on draws or dispatches.
// Identity, viewport, extent, depth and foreign-work violations remain live.
inline bool flatUpstreamIgnoresLegacyRefusal(bool upstreamOwns, const char* reason) {
    if (!upstreamOwns || !reason) return false;
    return std::strcmp(reason, "unknown-scene-projection-recipe") == 0 ||
        std::strcmp(reason, "projection-preparation-refused") == 0 ||
        std::strcmp(reason, "draw-binding-refused") == 0;
}

// The exact per-draw failPhase reasons local refusal turns into
// invalidate-and-observe. Every other reason stays frame-global by calling
// failPhase directly: render-extent-changed, invalid-render-extent,
// scene-projection-depth-unassociated, scene-depth-changed,
// no-raster-application, anything reached only from the compute/dispatch
// path (qualifyProjection's cs!=0 calls, compute-binding-refused,
// compute-source-invalidated, unknown-context-state), and every
// selector/model refusal downstream of the resolve
// (already-treated-this-frame, producer-source-identity-mismatch,
// actual-handoff-or-depth-view-refused, incomplete-jitter-frame,
// engine-source-not-ready, and the resolver's own failure reasons).
inline bool flatLocalRefusalReason(const char* reason) {
    if (!reason) return false;
    return std::strcmp(reason, "unknown-scene-projection-recipe") == 0 ||
        std::strcmp(reason, "unchanged-shader-mismatch") == 0 ||
        std::strcmp(reason, "actual-shader-mismatch") == 0 ||
        std::strcmp(reason, "projection-viewport-mismatch") == 0 ||
        std::strcmp(reason, "projection-preparation-refused") == 0 ||
        std::strcmp(reason, "draw-binding-refused") == 0;
}

// Observation exit, one tested predicate shared by flat_runtime.cpp and the
// rig (reviews/flat-temporal-main-review-2026-09-26.md): observation entered
// by a per-draw-local refusal clears only on a frame with a positively
// qualified handoff -- the copy draw's selector selected -- AND complete
// coverage (no failPhase, no uncertain prefix, no foreign work, the same
// trio phase.finish uses). Anything else, empty frames included, keeps
// observing. The config toggle is NOT this predicate's business; see below.
inline bool flatObservationClears(bool observing, bool qualifiedHandoff, bool completeCoverage) {
    return observing && qualifiedHandoff && completeCoverage;
}

// The on->off config transition ends observation explicitly: off's per-frame
// retry resumes instead of staying latched by a persistent refusal (history
// is already invalid from the observing frames). off->on changes nothing
// until the next local refusal. Returns the observing state after the toggle.
inline bool flatObservationToggle(bool observing, bool wasWanted, bool wanted) {
    return observing && !(wasWanted && !wanted);
}

} // namespace edvr
