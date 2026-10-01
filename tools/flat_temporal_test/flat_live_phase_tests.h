#pragma once

#include <cstdio>
#include <cstring>
#include "../../src/d3d11/flat_camera_phase.h"
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

    // ---- The phase count (2026-10-01; experimental.temporal_aa_jitter_follows_upscale; temporal_math.h) ----
    {
        using edvr::kTemporalJitterCount;
        // One frame of the live machine at `phases`, a clean accepted one; returns the offset the frame was given.
        auto frame = [&](FlatLivePhase& q, bool fourArgument, uint32_t phases, float* x, float* y) {
            if (fourArgument) q.beginFrame(true, true, 1920, 1080);
            else q.beginFrame(true, true, 1920, 1080, phases);
            *x = q.currentX; *y = q.currentY;
            q.noteApplied();
            q.finish(true, true);
        };
        // The default is today's eight: beginFrame's phase count defaults to it and says so, and a call that gives eight
        // explicitly draws the very same bits as one that gives nothing (the key's default is byte-identical).
        FlatLivePhase none, eight;
        none.beginFrame(true, false, 1920, 1080);
        none.finish(true, true);
        eight.beginFrame(true, false, 1920, 1080, kTemporalJitterCount);
        eight.finish(true, true);
        expect(none.phaseCount == kTemporalJitterCount && eight.phaseCount == kTemporalJitterCount,
               "phase count: the default is the fixed eight and the machine says so");
        bool bitSame = true, matchesFixed = true;
        uint32_t drawn = 0;
        for (uint32_t n = 0; n < 40; ++n) {
            const uint32_t before = none.phaseSequence;
            float ax = 9, ay = 9, bx = 9, by = 9;
            frame(none, true, 0, &ax, &ay);
            frame(eight, false, kTemporalJitterCount, &bx, &by);
            if (std::memcmp(&ax, &bx, 4) || std::memcmp(&ay, &by, 4)) bitSame = false;
            if (none.phaseSequence != before) {   // a frame that drew an offset (the opening frames are the warm-up's zero phases)
                float fx, fy;
                edvr::temporalJitter(before, &fx, &fy);
                if (ax != fx || ay != fy) matchesFixed = false;
                ++drawn;
            }
        }
        expect(bitSame, "phase count: an explicit eight draws the same bits as the default, frame for frame");
        expect(matchesFixed && drawn > 30, "phase count: ...and both are temporalJitter's fixed eight, frame for frame");

        // 32 phases: the sequence number counts on and the offset is its remainder.
        FlatLivePhase longer;
        longer.beginFrame(true, false, 1920, 1080, 32);
        longer.finish(true, true);
        expect(longer.phaseCount == 32, "phase count: the machine records the count it was asked for");
        bool follows = true, repeats = true;
        float firstRound[32] = {};
        uint32_t drawnLonger = 0;
        for (uint32_t n = 0; n < 2 + 64; ++n) {
            float x = 9, y = 9;
            const uint32_t before = longer.phaseSequence;
            frame(longer, false, 32, &x, &y);
            if (longer.phaseSequence == before) continue;   // a warm-up frame: no offset drawn
            ++drawnLonger;
            float ex, ey;
            edvr::temporalJitterPhase(before, 32, &ex, &ey);
            if (x != ex || y != ey) follows = false;
            if (before < 32) firstRound[before] = x;
            else if (x != firstRound[before % 32]) repeats = false;
        }
        expect(follows && drawnLonger >= 64, "phase count: at 32 each frame's offset is the sequence number's remainder in 32");
        expect(repeats, "phase count: ...so the 32-phase sequence repeats after 32 frames");
        longer.resetHistory();
        expect(longer.phaseCount == 32, "phase count: a history reset is not the count's to undo");

        // The count may change between frames (HMD quality, the key flipped live): the sequence number keeps counting and the
        // offset is its remainder in the count of the frame that draws it; nothing resets.
        FlatLivePhase live;
        live.beginFrame(true, false, 1920, 1080, 32);
        live.finish(true, true);
        bool changeOk = true;
        const uint32_t plan[] = {32, 32, 32, 32, 32, 32, 8, 8, 8, 32, 32, 18, 18, 8};
        for (uint32_t phases : plan) {
            float x = 9, y = 9;
            const uint32_t before = live.phaseSequence;
            frame(live, false, phases, &x, &y);
            if (live.phaseSequence != before) {
                float ex, ey;
                edvr::temporalJitterPhase(before, phases, &ex, &ey);
                if (x != ex || y != ey) changeOk = false;
            }
            if (live.phaseCount != phases || !live.previousAcceptedValid) changeOk = false;
        }
        expect(changeOk, "phase count: a change of count between frames draws the next offset from the new count and resets nothing");

        FlatLivePhase zeroCount;
        zeroCount.beginFrame(true, false, 1920, 1080, 0);
        expect(zeroCount.phaseCount == kTemporalJitterCount, "phase count: a count of zero reads as the fixed eight");
        FlatLivePhase off;
        off.beginFrame(false, false, 1920, 1080, 32);
        expect(off.phaseCount == 32 && zero(off) && off.phaseSequence == 0,
               "phase count: a disabled frame still records its count and draws no phase");
    }
    // flatCameraPhaseCount: which route may run a longer sequence, and from what sizes (flat_camera_phase.h).
    {
        using edvr::FlatCameraRoute;
        using edvr::flatCameraPhaseCount;
        using edvr::kTemporalJitterCount;
        expect(flatCameraPhaseCount(FlatCameraRoute::Upstream, true, 1920, 1080, 3840, 2160) == 32,
               "phase count: upstream with the key on and a render at half the output's size is 32");
        expect(flatCameraPhaseCount(FlatCameraRoute::Upstream, true, 2646, 2206, 4072, 3394) == 19,
               "phase count: ...and the 1.539x pair is 19");
        expect(flatCameraPhaseCount(FlatCameraRoute::Upstream, false, 1920, 1080, 3840, 2160) == kTemporalJitterCount,
               "phase count: the key off is the fixed eight on the upstream route, whatever the sizes");
        expect(flatCameraPhaseCount(FlatCameraRoute::Legacy, true, 1920, 1080, 3840, 2160) == kTemporalJitterCount &&
                   flatCameraPhaseCount(FlatCameraRoute::Off, true, 1920, 1080, 3840, 2160) == kTemporalJitterCount &&
                   flatCameraPhaseCount(FlatCameraRoute::None, true, 1920, 1080, 3840, 2160) == kTemporalJitterCount,
               "phase count: every route but upstream keeps the eight the Legacy lighting patch can take, key on or not");
        expect(flatCameraPhaseCount(FlatCameraRoute::Upstream, true, 1920, 1080, 1920, 1080) == kTemporalJitterCount &&
                   flatCameraPhaseCount(FlatCameraRoute::Upstream, true, 3840, 2160, 3840, 2160) == kTemporalJitterCount &&
                   flatCameraPhaseCount(FlatCameraRoute::Upstream, true, 3840, 2160, 1920, 1080) == kTemporalJitterCount,
               "phase count: a render at or above the size the upscaler resolves to (supersampling, DLAA, native) is eight");
        expect(flatCameraPhaseCount(FlatCameraRoute::Upstream, true, 1920, 1080, 0, 0) == kTemporalJitterCount &&
                   flatCameraPhaseCount(FlatCameraRoute::Upstream, true, 0, 0, 3840, 2160) == kTemporalJitterCount,
               "phase count: no plan yet (a size of zero) is eight");
    }
    return failures;
}
