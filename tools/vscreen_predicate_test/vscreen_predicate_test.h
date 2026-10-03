#pragma once

#if !defined(EDVR_VSCREEN_PREDICATE_TEST)
#error "This test interface is available only to the VScreen predicate rig."
#endif

#include "../../src/d3d11/draw_ladder_trace.h"
#include "../../src/d3d11/eye_census_observation.h"

struct ID3D11DeviceContext;

namespace edvr {

struct VScreenPredicateTestResult final {
    draw_ladder_trace::Token token{};
    draw_ladder::SiteResult siteResult{};
    std::uint32_t glareClampAfter = 0;
    std::uint64_t censusSkippedAfter = 0;
};

struct VScreenEyeCensusTestFilter final {
    std::uint8_t mode = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

struct VScreenEyeCensusTestRule final {
    char kind = 'N';
    std::uint32_t count = 0;
    std::uint32_t countHigh = 0;
    std::uint64_t vsHash = 0;
    VScreenEyeCensusTestFilter filters[4]{};
};

// Called only by a serialized typed COM callback during the active test visit.
struct VScreenEyeCensusTestMutation final {
    bool changeFilter = false;
    std::uint32_t ruleIndex = 0;
    std::uint32_t filterIndex = 0;
    VScreenEyeCensusTestFilter filter{};
    bool changeLoopCount = false;
    std::uint32_t loopCount = 0;
    bool changeCounter = false;
    std::uint64_t counter = 0;
};
bool vScreenEyeCensusPredicateTestMutate(
    const VScreenEyeCensusTestMutation& mutation) noexcept;

// Serialized test-owner calls only: temporarily replaces g_state and restores
// the thread-local draw flags touched by the real owner-context visitor.
bool vScreenPredicateTestVisit(std::uint16_t siteId,
                               ID3D11DeviceContext* context,
                               ID3D11DeviceContext* ownerContext,
                               std::uint32_t glareClamp,
                               bool distanceEnabled,
                               bool traceEnabled,
                               VScreenPredicateTestResult* result) noexcept;

bool vScreenEyeCensusPredicateTestVisit(
    ID3D11DeviceContext* context, char kind, std::uint32_t count,
    const VScreenEyeCensusTestRule* rules, std::uint32_t ruleCount,
    void* const psSrvs[4], std::uint64_t heldVsHash,
    std::uint64_t censusSkippedSeed, bool traceEnabled,
    VScreenPredicateTestResult* result) noexcept;

} // namespace edvr
