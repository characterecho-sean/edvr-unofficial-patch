#pragma once
#include <cstdint>

namespace edvr {
enum class FlatForegroundPlanDecision : uint8_t {
    Skip, SkipAfterWorld, Start, Extend, RejectActiveAfterWorld
};

// Sampling begins only before the first named world source. A cohort already
// in progress may not acquire more fragments once that source has begun.
inline constexpr FlatForegroundPlanDecision flatForegroundPlanDecision(
    uint64_t frame, bool hasResources, bool active, bool pending,
    bool worldAlreadyNamed, bool worldSeen, uint32_t reported,
    uint64_t firstReportedFrame) {
    if (!frame || !hasResources || pending || reported >= 2 ||
        (reported && frame < firstReportedFrame + 60))
        return FlatForegroundPlanDecision::Skip;
    if (active)
        return worldAlreadyNamed || worldSeen
            ? FlatForegroundPlanDecision::RejectActiveAfterWorld
            : FlatForegroundPlanDecision::Extend;
    return worldAlreadyNamed
        ? FlatForegroundPlanDecision::SkipAfterWorld
        : FlatForegroundPlanDecision::Start;
}
} // namespace edvr
