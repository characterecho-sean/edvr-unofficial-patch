#pragma once

// The query shortcuts' policy (src\d3d11\flat_query_cut.h): which questions are answered from what the runtime already
// knows, which are also put to the D3D context and compared, and what a wrong shortcut does. The D3D side of each state,
// against a real context and a real setter sequence, is the engine rig's (engine_velocity_test, flat_lazy_tests.h); this is
// the arithmetic:
//   - one frame in 64 checks, exactly the frames whose number is a multiple of 64, and asks up to four questions per state
//   - an agreeing check changes nothing but the count of checks
//   - a disagreeing check counts, tells the caller to say so ONCE, and sends that state (only that state) back to asking
//     the context for the rest of the session, whatever frame it is
//   - the window's counts come out and go to zero, and the fell-back bits stay
//   - each mistake the policy could make -- never checking, never falling back, a fallen-back state still shortcutting,
//     a mismatch not counted -- is noticed by the same scenario

#include <cstdio>
#include <cstring>
#include <string>
#include "../../src/d3d11/flat_query_cut.h"

namespace flat_query_cut_rig {
using edvr::FlatQuery;
using edvr::FlatQueryPlan;

struct NeverSample : edvr::FlatQueryNoFaults { static constexpr bool neverSample = true; };
struct NoFallback : edvr::FlatQueryNoFaults { static constexpr bool noFallback = true; };
struct ShortcutWhenFellBack : edvr::FlatQueryNoFaults { static constexpr bool shortcutWhenFellBack = true; };
struct ForgetMismatch : edvr::FlatQueryNoFaults { static constexpr bool forgetMismatch = true; };

// The scenario every instantiation is held to. False, with the first thing that was wrong, when the policy is not the one
// the header describes.
template <class Faults>
bool scenario(std::string* why) {
    edvr::FlatQueryCutT<Faults> cut;
    auto fail = [&](const char* m) { if (why && why->empty()) *why = m; return false; };
    // The cadence: frames 0..639 check exactly the multiples of 64.
    unsigned checking = 0;
    for (uint64_t f = 0; f < 640; ++f) {
        cut.beginFrame(f);
        const bool expected = f % 64 == 0;
        if (cut.checkingThisFrame()) ++checking;
        if (cut.checkingThisFrame() != expected) return fail("a frame that is not a multiple of 64 checked, or one that is did not");
    }
    if (checking != 10) return fail("ten frames of 640 did not check");
    // A checking frame: four questions per state, then shortcuts. An ordinary frame: shortcuts only.
    cut.beginFrame(64);
    unsigned sampled = 0, shortcut = 0;
    for (int i = 0; i < 10; ++i) {
        const FlatQueryPlan p = cut.plan(FlatQuery::GameBlend);
        if (p == FlatQueryPlan::Sample) ++sampled;
        if (p == FlatQueryPlan::Shortcut) ++shortcut;
    }
    if (sampled != 4 || shortcut != 6) return fail("a checking frame did not ask four questions of a state and answer the rest from what is known");
    // Another state has its own four.
    unsigned other = 0;
    for (int i = 0; i < 10; ++i) if (cut.plan(FlatQuery::CoverageDepth) == FlatQueryPlan::Sample) ++other;
    if (other != 4) return fail("one state's questions used up another's");
    cut.beginFrame(65);
    for (int i = 0; i < 10; ++i) if (cut.plan(FlatQuery::GameBlend) != FlatQueryPlan::Shortcut) return fail("an ordinary frame asked the context");
    // An agreeing check changes nothing.
    cut.beginFrame(128);
    if (cut.plan(FlatQuery::TargetsKept) != FlatQueryPlan::Sample) return fail("the first question of a checking frame was not a check");
    if (cut.compared(FlatQuery::TargetsKept, true) || cut.fellBack(FlatQuery::TargetsKept)) return fail("an agreeing check was taken for a wrong shortcut");
    // A disagreeing one: said once, that state alone falls back.
    if (cut.plan(FlatQuery::GameTargets) != FlatQueryPlan::Sample) return fail("the first question of a state was not a check");
    if (!cut.compared(FlatQuery::GameTargets, false)) return fail("a disagreeing check did not tell the caller to say so");
    if (!cut.fellBack(FlatQuery::GameTargets)) return fail("a disagreeing check did not send the state back to asking");
    if (cut.fellBack(FlatQuery::GameBlend) || cut.fellBack(FlatQuery::TargetsKept)) return fail("one state's fallback reached another");
    if (cut.compared(FlatQuery::GameTargets, false)) return fail("the second disagreement said so again");
    // Fallen back: asked, on an ordinary frame and on a checking one, and counted as asked.
    for (uint64_t f : {129ull, 192ull}) {
        cut.beginFrame(f);
        if (cut.plan(FlatQuery::GameTargets) != FlatQueryPlan::Ask) return fail("a state that fell back was still answered from what is known");
    }
    // The window's counts, then nothing, and the fallen-back bit stays.
    const edvr::FlatQueryCounts first = cut.take();
    const unsigned t = static_cast<unsigned>(FlatQuery::GameTargets), b = static_cast<unsigned>(FlatQuery::GameBlend);
    if (first.mismatched[t] != 2) return fail("the window did not count both disagreements");
    if (first.asked[t] != 2) return fail("the window did not count the questions a fallen-back state asked");
    if (first.served[b] < 1 || first.sampled[b] != 4) return fail("the window's served and checked counts are not the ones the plans made");
    if (!(first.fellBack & (1u << t))) return fail("the fallen-back state is not in the window");
    const edvr::FlatQueryCounts second = cut.take();
    if (second.served[b] || second.sampled[b] || second.asked[t] || second.mismatched[t]) return fail("a window's counts were not zeroed when taken");
    if (!(second.fellBack & (1u << t))) return fail("the session's fallen-back bit went with the window");
    // The diagnostic switch.
    cut.fallBackAll();
    cut.beginFrame(256);
    for (unsigned i = 0; i < edvr::kFlatQueryCount; ++i)
        if (cut.plan(static_cast<FlatQuery>(i)) != FlatQueryPlan::Ask) return fail("fallBackAll left a state answering from what is known");
    cut.reset();
    cut.beginFrame(0);
    if (cut.fellBack(FlatQuery::GameTargets) || cut.plan(FlatQuery::GameTargets) != FlatQueryPlan::Sample) return fail("reset did not clear the session");
    return true;
}

}  // namespace flat_query_cut_rig

inline int flatQueryCutTests() {
    using namespace flat_query_cut_rig;
    int failures = 0;
    auto expect = [&](bool ok, const char* name) {
        if (!ok) { std::printf("FAIL: query cut %s\n", name); ++failures; }
    };
    std::string why;
    const bool shipped = scenario<edvr::FlatQueryNoFaults>(&why);
    if (!shipped) std::printf("  query cut: the shipped policy failed: %s\n", why.c_str());
    expect(shipped, "the policy holds to its scenario: cadence, four checks a state, said once, that state alone falls back, counts taken and zeroed");
    // The rig's checks notice each mistake.
    struct Mistake { bool (*run)(std::string*); const char* what; };
    const Mistake mistakes[] = {
        {&scenario<NeverSample>, "a policy that never checks"},
        {&scenario<NoFallback>, "a wrong shortcut that does not send its state back to asking"},
        {&scenario<ShortcutWhenFellBack>, "a fallen-back state that is still answered from what is known"},
        {&scenario<ForgetMismatch>, "a disagreement that is not counted"},
    };
    for (const Mistake& m : mistakes) {
        std::string reason;
        expect(!m.run(&reason), (std::string("the scenario does not notice: ") + m.what).c_str());
    }
    // Names and the fallback line.
    bool named = true;
    for (unsigned i = 0; i < edvr::kFlatQueryCount; ++i) {
        const char* n = edvr::flatQueryName(static_cast<FlatQuery>(i));
        named = named && n && std::strcmp(n, "?") != 0 && *n;
        for (unsigned j = 0; j < i; ++j) named = named && std::strcmp(n, edvr::flatQueryName(static_cast<FlatQuery>(j))) != 0;
    }
    expect(named, "every state has its own name");
    char line[400];
    const int n = edvr::flatQueryFallbackLine(line, sizeof(line), FlatQuery::GameBlend, "the blend state it held was not the bound one");
    expect(n > 0 && n < static_cast<int>(sizeof(line)) && std::strstr(line, "game blend state") && std::strstr(line, "asks the context for it from now on") &&
               std::strstr(line, "the blend state it held was not the bound one"),
           "the fallback line names the state and says what happens next");
    char cut[40];
    edvr::flatQueryFallbackLine(cut, sizeof(cut), FlatQuery::CoverageDepth, nullptr);
    expect(std::strlen(cut) < sizeof(cut), "the fallback line is cut to the buffer it is given");
    return failures;
}
