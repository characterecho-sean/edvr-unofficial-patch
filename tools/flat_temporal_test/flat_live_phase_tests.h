#pragma once

#include <cstdio>
#include "../../src/d3d11/flat_live_phase.h"

inline int flatLivePhaseTests() {
    using edvr::FlatLivePhase;
    int failures = 0;
    auto expect = [&](bool ok, const char* name) {
        if (!ok) { std::printf("FAIL: live phase %s\n", name); ++failures; }
    };
    auto zero = [](const FlatLivePhase& p) {
        return p.currentX == 0.0f && p.currentY == 0.0f;
    };
    auto warm = [](FlatLivePhase& p, unsigned count = 2) {
        for (unsigned i = 0; i < count; ++i) {
            p.beginFrame(true, i != 0 || p.previousAcceptedValid, 1920, 1080);
            p.finish(true, true);
        }
    };

    FlatLivePhase p;
    p.beginFrame(true, false, 1920, 1080);
    expect(zero(p) && p.warmFrames == 0 && !p.previousAcceptedValid,
           "first frame starts at zero without accepted history");
    p.finish(true, true);
    expect(p.warmFrames == 1 && p.previousAcceptedValid &&
           p.previousX == 0.0f && p.previousY == 0.0f,
           "first complete zero phase earns one warm frame");
    p.beginFrame(true, true, 1920, 1080);
    expect(zero(p), "second frame remains at zero");
    p.finish(true, true);
    p.beginFrame(true, true, 1920, 1080);
    float expectedX = 0.0f, expectedY = 0.0f;
    edvr::temporalJitter(0, &expectedX, &expectedY);
    expect(p.currentX == expectedX && p.currentY == expectedY &&
           p.phaseSequence == 1 && p.warmFrames == 2,
           "third frame chooses the first nonzero temporal phase");
    const float chosenX = p.currentX, chosenY = p.currentY;
    p.noteApplied();
    p.noteApplied();
    expect(p.applied == 2 && p.currentX == chosenX && p.currentY == chosenY,
           "multiple draws share one phase");
    p.finish(true, true);
    expect(p.previousAcceptedValid && p.previousX == chosenX &&
           p.previousY == chosenY, "only accepted clean phase becomes previous");
    p.beginFrame(true, true, 1920, 1080);
    edvr::temporalJitter(1, &expectedX, &expectedY);
    expect(p.currentX == expectedX && p.currentY == expectedY &&
           p.previousX == chosenX && p.previousY == chosenY,
           "next frame advances sequence and retains accepted previous phase");

    p.fail();
    expect(zero(p) && p.failed && !p.needsSpatialFallback() && p.applied == 0,
           "refusal before first applied draw zeros remaining phase");
    p.noteApplied();
    expect(p.applied == 0, "failed frame cannot record a later applied draw");
    p.finish(true, true);
    expect(!p.previousAcceptedValid && p.previousX == 0.0f &&
           p.previousY == 0.0f && p.warmFrames == 0,
           "early refusal invalidates temporal history despite accepted resolve");
    warm(p);
    p.beginFrame(true, true, 1920, 1080);
    const float heldX = p.currentX, heldY = p.currentY;
    p.noteApplied();
    p.fail();
    expect(p.currentX == heldX && p.currentY == heldY && p.applied == 1 &&
           p.needsSpatialFallback(),
           "later refusal holds phase and requires spatial fallback");
    p.finish(false, false);
    expect(!p.previousAcceptedValid && p.warmFrames == 0 &&
           p.previousX == 0.0f && p.previousY == 0.0f,
           "compromised frame does not enter history");
    warm(p);
    p.beginFrame(true, true, 1920, 1080);
    expect(!zero(p), "two complete clean zero frames restore jitter after failure");
    p.finish(false, true);
    expect(!p.previousAcceptedValid && p.warmFrames == 0,
           "rejected temporal resolve clears warm credit");
    warm(p);
    p.beginFrame(true, true, 1920, 1080);
    p.finish(true, false);
    expect(!p.previousAcceptedValid && p.warmFrames == 0,
           "incomplete coverage clears warm credit and previous phase");

    warm(p);
    p.beginFrame(true, true, 2560, 1440);
    expect(zero(p) && p.warmFrames == 0 && !p.previousAcceptedValid,
           "render size change resets phase admission");
    p.finish(true, true);
    p.beginFrame(true, false, 2560, 1440);
    expect(zero(p) && p.warmFrames == 0 && !p.previousAcceptedValid,
           "incompatible previous frame resets warmup");
    p.finish(true, true);
    p.beginFrame(false, true, 2560, 1440);
    expect(zero(p) && p.warmFrames == 0 && !p.previousAcceptedValid &&
           p.phaseSequence == 0, "disabled path resets history and sequence");
    p.resetHistory();
    expect(zero(p) && p.previousX == 0.0f && p.previousY == 0.0f,
           "explicit history reset clears both phases");
    return failures;
}
