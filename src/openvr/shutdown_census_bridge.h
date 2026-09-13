#pragma once

#include "../common/shutdown_census_api.h"
#include <windows.h>
#include <cstdint>

namespace edvr {

struct ShutdownCensusSession {
    HMODULE module = nullptr;
    std::uint64_t token = 0;
    EndShutdownCensus end = nullptr;
    const char* reason = nullptr;
};

// Lookup-only bridge. A nonzero token keeps one reference to the already
// loaded paired graphics module until endShutdownCensus releases it.
ShutdownCensusSession beginShutdownCensus() noexcept;
bool endShutdownCensus(ShutdownCensusSession& session,
                       ShutdownCensusSnapshot& snapshot) noexcept;

} // namespace edvr
