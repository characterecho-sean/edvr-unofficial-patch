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

    // ---- The cycle length (advanced.temporal_aa_jitter_phases; temporal_math.h) ----
    {
        using edvr::kTemporalJitterCount;
        // One clean, accepted frame of the live machine; returns the offset the frame was given. `withCount` false is the call the VR world
        // route makes (no count named).
        auto frame = [&](FlatLivePhase& q, bool withCount, uint32_t phases, float* x, float* y) {
            if (withCount) q.beginFrame(true, true, 1920, 1080, phases);
            else q.beginFrame(true, true, 1920, 1080);
            *x = q.currentX; *y = q.currentY;
            q.noteApplied();
            q.finish(true, true);
        };
        // The default is today's eight: a beginFrame that names no count says eight, and one that names eight draws the very same bits,
        // frame for frame, as temporalJitter's eight (the key's default changes nothing, and the VR world route, which names none,
        // keeps its sequence).
        FlatLivePhase none, eight;
        none.beginFrame(true, false, 1920, 1080);
        none.finish(true, true);
        eight.beginFrame(true, false, 1920, 1080, kTemporalJitterCount);
        eight.finish(true, true);
        expect(none.phaseCount == kTemporalJitterCount && eight.phaseCount == kTemporalJitterCount,
               "cycle length: the default is the fixed eight and the machine says so");
        bool bitSame = true, matchesFixed = true;
        uint32_t drawn = 0;
        for (uint32_t n = 0; n < 40; ++n) {
            const uint32_t before = none.phaseSequence;
            float ax = 9, ay = 9, bx = 9, by = 9;
            frame(none, false, 0, &ax, &ay);
            frame(eight, true, kTemporalJitterCount, &bx, &by);
            if (std::memcmp(&ax, &bx, 4) || std::memcmp(&ay, &by, 4)) bitSame = false;
            if (none.phaseSequence != before) {   // a frame that drew an offset (the first two are the warm-up's zero phases)
                float fx, fy;
                edvr::temporalJitter(before, &fx, &fy);
                if (ax != fx || ay != fy) matchesFixed = false;
                ++drawn;
            }
        }
        expect(bitSame, "cycle length: naming eight draws the same bits as naming none, frame for frame");
        expect(matchesFixed && drawn > 30, "cycle length: ...and both are temporalJitter's fixed eight, frame for frame");

        // Longer cycles: the sequence number counts on and the offset is its remainder in the count.
        for (const uint32_t count : {16u, 32u, 64u}) {
            char what[160];
            FlatLivePhase longer;
            longer.beginFrame(true, false, 1920, 1080, count);
            longer.finish(true, true);
            std::snprintf(what, sizeof(what), "cycle length %u: the machine records the count it was asked for", count);
            expect(longer.phaseCount == count, what);
            bool follows = true, repeats = true;
            float firstX[64] = {}, firstY[64] = {};
            uint32_t drawnLonger = 0;
            for (uint32_t n = 0; n < 2 + 3 * count; ++n) {
                float x = 9, y = 9;
                const uint32_t before = longer.phaseSequence;
                frame(longer, true, count, &x, &y);
                if (longer.phaseSequence == before) continue;   // a warm-up frame: no offset drawn
                ++drawnLonger;
                float ex, ey;
                edvr::temporalJitter(before, &ex, &ey, count);
                if (x != ex || y != ey) follows = false;
                if (before < count) { firstX[before] = x; firstY[before] = y; }
                else if (x != firstX[before % count] || y != firstY[before % count]) repeats = false;
            }
            std::snprintf(what, sizeof(what), "cycle length %u: each frame's offset is the sequence number's remainder in %u", count, count);
            expect(follows && drawnLonger >= 3 * count, what);
            std::snprintf(what, sizeof(what), "cycle length %u: the sequence repeats after %u frames", count, count);
            expect(repeats, what);
            longer.resetHistory();
            std::snprintf(what, sizeof(what), "cycle length %u: a history reset is not the count's to undo", count);
            expect(longer.phaseCount == count && longer.phaseSequence == 0, what);
        }

        // The count may change between frames: the sequence number keeps counting, the offset is its remainder in the count of the frame
        // that draws it, and nothing resets (the runtime resets on a change of the KEY; this is the machine's own contract).
        FlatLivePhase live;
        live.beginFrame(true, false, 1920, 1080, 32);
        live.finish(true, true);
        bool changeOk = true;
        const uint32_t plan[] = {32, 32, 32, 32, 32, 32, 8, 8, 8, 16, 16, 64, 64, 8};
        for (const uint32_t phases : plan) {
            float x = 9, y = 9;
            const uint32_t before = live.phaseSequence;
            frame(live, true, phases, &x, &y);
            if (live.phaseSequence != before) {
                float ex, ey;
                edvr::temporalJitter(before, &ex, &ey, phases);
                if (x != ex || y != ey) changeOk = false;
            }
            if (live.phaseCount != phases || !live.previousAcceptedValid) changeOk = false;
        }
        expect(changeOk, "cycle length: a change of count between frames draws the next offset from the new count and resets nothing");

        FlatLivePhase zeroCount;
        zeroCount.beginFrame(true, false, 1920, 1080, 0);
        expect(zeroCount.phaseCount == kTemporalJitterCount, "cycle length: a count of zero reads as the fixed eight");
        FlatLivePhase off;
        off.beginFrame(false, false, 1920, 1080, 32);
        expect(off.phaseCount == 32 && zero(off) && off.phaseSequence == 0,
               "cycle length: a disabled frame still records its count and draws no phase");
    }
    // flatCameraPhaseCount: which route may run a longer cycle (flat_camera_phase.h).
    {
        using edvr::FlatCameraRoute;
        using edvr::flatCameraPhaseCount;
        using edvr::kTemporalJitterCount;
        expect(flatCameraPhaseCount(FlatCameraRoute::Upstream, 8) == 8 && flatCameraPhaseCount(FlatCameraRoute::Upstream, 16) == 16 &&
                   flatCameraPhaseCount(FlatCameraRoute::Upstream, 32) == 32 && flatCameraPhaseCount(FlatCameraRoute::Upstream, 64) == 64,
               "cycle length: the upstream route runs the count the key asks for");
        expect(flatCameraPhaseCount(FlatCameraRoute::Legacy, 16) == kTemporalJitterCount &&
                   flatCameraPhaseCount(FlatCameraRoute::Legacy, 64) == kTemporalJitterCount &&
                   flatCameraPhaseCount(FlatCameraRoute::Off, 32) == kTemporalJitterCount &&
                   flatCameraPhaseCount(FlatCameraRoute::None, 32) == kTemporalJitterCount,
               "cycle length: every route but upstream keeps the eight the legacy lighting patch can take, whatever the key says");
        expect(flatCameraPhaseCount(FlatCameraRoute::Upstream, 0) == kTemporalJitterCount,
               "cycle length: no count (zero) is the fixed eight on the upstream route too");
    }
    return failures;
}
