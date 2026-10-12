#pragma once

#include <cstdio>
#include <initializer_list>
#include "../../src/d3d11/flat_local_reject.h"
#include "../../src/d3d11/flat_live_phase.h"

inline int flatLocalRejectTests() {
    using edvr::flatLocalRefusalReason;
    using edvr::flatObservationClears;
    using edvr::flatObservationToggle;
    int failures = 0;
    auto expect = [&](bool ok, const char* name) {
        if (!ok) { std::printf("FAIL: local reject %s\n", name); ++failures; }
    };

    expect(flatLocalRefusalReason("unknown-scene-projection-recipe") &&
           flatLocalRefusalReason("unchanged-shader-mismatch") &&
           flatLocalRefusalReason("actual-shader-mismatch") &&
           flatLocalRefusalReason("projection-viewport-mismatch") &&
           flatLocalRefusalReason("projection-preparation-refused") &&
           flatLocalRefusalReason("draw-binding-refused"),
           "the six per-draw reasons are locally refusable");
    expect(!flatLocalRefusalReason("render-extent-changed") &&
           !flatLocalRefusalReason("invalid-render-extent") &&
           !flatLocalRefusalReason("scene-projection-depth-unassociated") &&
           !flatLocalRefusalReason("scene-depth-changed") &&
           !flatLocalRefusalReason("no-raster-application") &&
           !flatLocalRefusalReason("compute-binding-refused") &&
           !flatLocalRefusalReason("compute-source-invalidated") &&
           !flatLocalRefusalReason("unknown-context-state") &&
           !flatLocalRefusalReason("already-treated-this-frame") &&
           !flatLocalRefusalReason("producer-source-identity-mismatch") &&
           !flatLocalRefusalReason(nullptr),
           "frame-global reasons and a null reason are never locally refusable");

    // reviews/flat-temporal-main-review-2026-09-26.md: observation exit
    // requires a positively qualified handoff on a completely covered frame;
    // empty, failed, uncertain or foreign-work frames keep observing.
    expect(!flatObservationClears(true, false, true) &&
           !flatObservationClears(true, true, false) &&
           !flatObservationClears(true, false, false) &&
           flatObservationClears(true, true, true) &&
           !flatObservationClears(false, true, true),
           "observation clears only on a qualified, completely covered frame");
    expect(!flatObservationToggle(true, true, false) &&
           flatObservationToggle(true, true, true) &&
           flatObservationToggle(true, false, true) &&
           flatObservationToggle(true, false, false) &&
           !flatObservationToggle(false, true, false),
           "only the on-to-off toggle ends observation explicitly");

    // Captured Type 11 frames 7793..7795: two unused compute plans refused
    // during the Upstream audit. A phase already applied to geometry must
    // retain its accepted history exactly as it does with the audit off.
    auto frame = [](bool upstream, bool audit, const char* reason) {
        edvr::FlatLivePhase phase;
        for (unsigned i = 0; i < 2; ++i) {
            phase.beginFrame(true, i != 0, 1248, 780, 19);
            phase.finish(true, true);
        }
        phase.beginFrame(true, true, 1248, 780, 19);
        phase.noteApplied();
        if (audit) {
            for (unsigned i = 0; i < 2; ++i)
                if (!edvr::flatUpstreamIgnoresLegacyRefusal(upstream, reason))
                    phase.fail();
        }
        phase.finish(true, true);
        return phase;
    };
    const auto normal = frame(true, false, "projection-preparation-refused");
    const auto audited = frame(true, true, "projection-preparation-refused");
    expect(normal.previousAcceptedValid && audited.previousAcceptedValid &&
           !audited.failed && !audited.needsSpatialFallback() &&
           audited.currentX == normal.currentX && audited.currentY == normal.currentY &&
           audited.previousX == normal.previousX && audited.previousY == normal.previousY &&
           audited.phaseSequence == normal.phaseSequence && audited.applied == normal.applied,
           "unused Upstream compute preparation audit preserves raster phase and history");
    const auto legacy = frame(false, true, "projection-preparation-refused");
    expect(legacy.failed && legacy.needsSpatialFallback() && !legacy.previousAcceptedValid,
           "consumed Legacy preparation refusal invalidates history after raster application");
    for (const char* reason : {"render-extent-changed", "scene-depth-changed",
                              "scene-projection-depth-unassociated", "foreign-work",
                              "actual-shader-mismatch", "projection-viewport-mismatch",
                              "deferred-inverse-preparation-failed", "deferred-inverse-binding-failed",
                              "deferred-inverse-unproven"}) {
        const auto violated = frame(true, true, reason);
        expect(violated.failed && violated.needsSpatialFallback() &&
               !violated.previousAcceptedValid,
               "true Upstream scene or binding violation still invalidates history");
    }
    expect(!edvr::flatUpstreamIgnoresLegacyRefusal(true, nullptr),
           "null refusal is never bypassed");

    return failures;
}
