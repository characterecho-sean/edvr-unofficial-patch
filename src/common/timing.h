// One clock, and the rule about when to use it.
//
// THE RULE. If a constant answers "how long", it is milliseconds. If it answers
// "how many of these events", it is a count. Writing the first as the second is
// the bug this header exists to prevent, and it was everywhere: Elite in VR runs
// at 72Hz, 90Hz or 120Hz depending on the headset, so a threshold of 900 frames
// described in its own comment as "ten seconds at 90Hz" was 12.5 seconds on a
// Quest and 7.5 on a Pimax. Two of the three supported rates got a duration
// nobody chose.
//
// The tell is in the comments. Before this header, `kRetryCooldown = 3600` was
// documented as "a minute apart" in camera_view.cpp, "forty seconds" in
// camera_view.h, and "about a minute" in the line printed to the user. Three
// different durations for one constant, because the frame count was standing in
// for a time and each author converted it at whatever rate they had in mind.
//
// WHY NOT DERIVE FRAMES FROM A MEASURED RATE. Because it reinstates the worse
// half of the bug. A loading screen has been measured at 1790fps, and frames
// are not paced by the display then at all -- 1800 of them go by in a second.
// Anything that converts a duration into a frame count, however it gets the
// rate, still counts those. A clock does not.
//
// WHAT STAYS A COUNT, deliberately, with the reason written at each one:
//   - budgets over frames the game actually RENDERED (glitch_frame's give-up
//     counters, exposure_fix's), which are measures of work done and are
//     specifically designed to ignore loading screens;
//   - counters compared across the two DLLs (head_offset's staleness), where
//     both halves tick on the same event and a shared stall must not age it;
//   - evidence counts, where one frame is one independent sample of the camera;
//   - short debounces whose unit is the composite itself.
// Converting any of those to time would be the same mistake pointing the other
// way.
//
// RESOLUTION. GetTickCount64 is monotonic, needs no syscall, and is accurate to
// about 15.6ms. Every duration converted here is 150ms or longer, so the worst
// case is a tenth of the shortest one. Anything that needs to resolve a single
// frame is a count, by the rule above, and does not come here.
#pragma once

#include <cstdint>

#include <windows.h>

namespace edvr {

// Swapped by the test harnesses so fixtures can step time deterministically
// instead of sleeping. Fixtures drive frames synthetically -- gate_test walks a
// gate through hundreds of frames in microseconds -- so a real clock would make
// every duration-gated branch untestable, and the ones that did run would be
// timing-dependent. Being able to set the rate is also what lets the same
// fixture assert the same outcome at 72, 90 and 120Hz.
inline uint64_t (*g_clockForTest)() = nullptr;

// Monotonic milliseconds. The only clock this project reads.
inline uint64_t nowMs() {
    return g_clockForTest ? g_clockForTest() : GetTickCount64();
}

// A stamp meaning "this run started now", guaranteed nonzero.
//
// 0 is the "not started" sentinel, so a stamp that happens to BE 0 would read
// as a run that never began and its duration would never elapse. The real
// clock passes 0 once per boot; a test clock starts there by default and would
// hit it on the first stamp of every fixture. Costs one millisecond of
// precision on that single tick, against a resolution that is already 15.6.
inline uint64_t stampMs() {
    const uint64_t t = nowMs();
    return t ? t : 1;
}

// Has `ms` passed since the stamp `since`?
//
// A `since` of 0 means "not started", and answers false however long it has
// been. That is the safe direction for every caller here: a stopwatch nobody
// started has not run out, whereas treating 0 as the epoch would fire every
// duration-gated branch on the first frame of the session.
inline bool elapsedMs(uint64_t since, uint64_t ms) {
    return since != 0 && (nowMs() - since) >= ms;
}

// The standard headset rates, for the harnesses that assert rate-invariance and
// for comments that would otherwise pick one and call it the rate.
constexpr uint32_t kRate72 = 72;
constexpr uint32_t kRate90 = 90;
constexpr uint32_t kRate120 = 120;

}  // namespace edvr
