#pragma once

#if !defined(EDVR_VSCREEN_PREDICATE_TEST)
#error "This test interface is available only to the VScreen predicate rig."
#endif

#include "../../src/d3d11/draw_ladder_trace.h"

struct ID3D11DeviceContext;

namespace edvr {

struct VScreenPredicateTestResult final {
    draw_ladder_trace::Token token{};
    draw_ladder::SiteResult siteResult{};
    std::uint32_t glareClampAfter = 0;
};

// Serialized test-owner calls only: temporarily replaces g_state and restores
// the thread-local draw flags touched by the real owner-context visitor.
bool vScreenPredicateTestVisit(std::uint16_t siteId,
                               ID3D11DeviceContext* context,
                               ID3D11DeviceContext* ownerContext,
                               std::uint32_t glareClamp,
                               bool distanceEnabled,
                               bool traceEnabled,
                               VScreenPredicateTestResult* result) noexcept;

} // namespace edvr
