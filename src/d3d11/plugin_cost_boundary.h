#pragma once

#include <cstdint>

#include "../common/plugin_cost.h"

namespace edvr::plugin_cost {

// Keep API cadence with the existing one-in-sixteen draw sample policy.
// The Present counter is available in VR and flat, including when menuTick
// has no configured menu to drive perfMonitorFrame.
constexpr uint32_t kPresentSampleEvery = 16;

struct PresentBoundary {
    bool apiSampleFrame = false;

    void reset() noexcept { apiSampleFrame = false; }

    bool close(uint32_t frameNo, bool closedCpuSampleFrame,
               EdvrPluginCostWindowV2* out) noexcept {
        const bool nextSampleFrame = (frameNo % kPresentSampleEvery) == 0;
        const bool ready = edvrPluginCostFrameBoundaryV2(
            frameNo, closedCpuSampleFrame ? 1u : 0u,
            apiSampleFrame ? 1u : 0u, nextSampleFrame ? 1u : 0u, 0u,
            out) != 0;
        // Read back what the collector actually published. A cold/shutdown
        // collector rejects the next flag and must remain cold here as well.
        apiSampleFrame = edvrPluginCostApiSampleFrame() != 0;
        return ready;
    }
};

}  // namespace edvr::plugin_cost
