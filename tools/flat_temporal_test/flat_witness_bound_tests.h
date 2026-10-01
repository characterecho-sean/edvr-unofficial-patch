#pragma once

// The camera-write witness's bound (src\d3d11\flat_witness_bound.h): the cost of the stack walks is
// bounded by the policy, not by the number of camera writes the game makes.
//
// What it holds to:
//   - a witness that keeps learning keeps walking, up to the budget; a run of walks that learn
//     nothing disarms it; a new site or buffer resets the run
//   - a hundred thousand writes at one known site ask for 33 walks (the one that learns it, then the
//     stable run), not a hundred thousand
//   - a disarmed witness asks for nothing and its counters stay put; the write counts are the caller's
//     and never depend on it
//   - re-arming restores it exactly, and the same stream costs the same again

#include <cstdint>
#include <cstdio>
#include <cstring>
#include "../../src/d3d11/flat_witness_bound.h"

inline int flatWitnessBoundTests() {
    using namespace edvr;
    int failures = 0;
    auto expect = [&](bool ok, const char* name) {
        if (!ok) { std::printf("FAIL: witness bound %s\n", name); ++failures; }
    };

    expect(kFlatWitnessStableWalks == 32 && kFlatWitnessWalkBudget == 128 && kFlatWitnessStableWalks < kFlatWitnessWalkBudget,
           "the documented bound: 32 walks in a row that learn nothing, 128 walks in all");

    {
        FlatWitnessBound b;
        expect(b.wantsWalk() && b.walks == 0 && b.stableRun == 0 && !b.stopped && b.why == FlatWitnessStop::None,
               "a fresh witness is armed and has walked nothing");
    }

    // A run of walks that learn nothing disarms it, at the 32nd.
    {
        FlatWitnessBound b;
        FlatWitnessStop last = FlatWitnessStop::None;
        bool armedThrough = true;
        for (uint32_t i = 1; i < kFlatWitnessStableWalks; ++i) {
            last = b.noteWalk(false);
            armedThrough = armedThrough && last == FlatWitnessStop::None && b.wantsWalk();
        }
        expect(armedThrough && b.walks == 31, "31 walks that learn nothing leave it armed");
        last = b.noteWalk(false);
        expect(last == FlatWitnessStop::Stable && !b.wantsWalk() && b.walks == 32 && b.why == FlatWitnessStop::Stable,
               "the 32nd disarms it, for stability");
    }

    // A walk that learns resets the run.
    {
        FlatWitnessBound b;
        for (int i = 0; i < 31; ++i) b.noteWalk(false);
        b.noteWalk(true);
        expect(b.stableRun == 0 && b.wantsWalk(), "a new site or buffer resets the run of walks that learned nothing");
        for (int i = 0; i < 31; ++i) b.noteWalk(false);
        expect(b.wantsWalk() && b.walks == 63, "31 more still leave it armed: 63 walks in all");
        expect(b.noteWalk(false) == FlatWitnessStop::Stable && b.walks == 64, "and the 32nd since the last lesson disarms it");
    }

    // A witness that learns on every walk stops at the budget.
    {
        FlatWitnessBound b;
        bool armedThrough = true;
        for (uint32_t i = 1; i < kFlatWitnessWalkBudget; ++i)
            armedThrough = armedThrough && b.noteWalk(true) == FlatWitnessStop::None && b.wantsWalk();
        expect(armedThrough && b.walks == 127, "127 walks that each learn leave it armed");
        expect(b.noteWalk(true) == FlatWitnessStop::Budget && !b.wantsWalk() && b.walks == 128 && b.why == FlatWitnessStop::Budget,
               "the 128th disarms it, for the budget");
    }

    // The cost is the policy's: writes at one known site.
    {
        FlatWitnessBound b;
        uint64_t walks = 0;
        for (uint64_t write = 0; write < 100000; ++write) {
            if (!b.wantsWalk()) continue;
            ++walks;
            b.noteWalk(write == 0);   // the first write teaches it the site; the rest are known
        }
        expect(walks == 1 + kFlatWitnessStableWalks && b.stopped, "a hundred thousand writes at one known site ask for 33 walks");
    }

    // A stream that keeps teaching it: a new site every 20 walks, five in all, then nothing new.
    auto slowLearner = [](FlatWitnessBound& b) {
        uint64_t walks = 0;
        for (uint64_t write = 0; write < 100000; ++write) {
            if (!b.wantsWalk()) continue;
            ++walks;
            b.noteWalk(walks <= 100 && (walks - 1) % 20 == 0);   // walks 1, 21, 41, 61, 81 teach it
        }
        return walks;
    };
    {
        FlatWitnessBound b;
        const uint64_t walks = slowLearner(b);
        expect(walks == 81 + kFlatWitnessStableWalks && b.stopped && b.why == FlatWitnessStop::Stable,
               "five lessons 20 walks apart: it walks until 32 after the last, 113 in all, well inside the budget");
    }

    // Disarmed: inert.
    {
        FlatWitnessBound b;
        for (uint32_t i = 0; i < kFlatWitnessStableWalks; ++i) b.noteWalk(false);
        const uint32_t walks = b.walks, run = b.stableRun;
        const FlatWitnessStop again = b.noteWalk(true);
        expect(again == FlatWitnessStop::None && b.walks == walks && b.stableRun == run && b.stopped && !b.wantsWalk(),
               "a walk noted after the witness is disarmed changes nothing");
    }

    // Re-armed: as new, and the same stream costs the same again.
    {
        FlatWitnessBound b;
        const uint64_t first = slowLearner(b);
        expect(b.stopped, "the first stream disarmed it");
        b.rearm();
        expect(b.wantsWalk() && b.walks == 0 && b.stableRun == 0 && !b.stopped && b.why == FlatWitnessStop::None,
               "an F10 audit's re-arm restores it exactly");
        const uint64_t second = slowLearner(b);
        expect(second == first && b.stopped, "and the same stream costs the same again");
    }

    // The words the log uses.
    expect(std::strcmp(FlatWitnessBound::stopName(FlatWitnessStop::None), "armed") == 0 &&
           std::strcmp(FlatWitnessBound::stopName(FlatWitnessStop::Stable), "the producer sites were stable") == 0 &&
           std::strcmp(FlatWitnessBound::stopName(FlatWitnessStop::Budget), "the walk budget is spent") == 0,
           "the log names the state: armed, stable, or the budget spent");

    return failures;
}
