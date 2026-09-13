#pragma once

// Lookup-only diagnostic bridge between the paired proxies. This never starts
// graphics or VR. Callers initialize size/version; rejected requests leave the
// snapshot untouched. A nonzero token owns one window until its matching end.
#include <windows.h>
#include <cstdint>
#include <type_traits>

namespace edvr {
inline constexpr std::uint32_t kShutdownCensusVersion = 1;
struct ShutdownCensusSnapshot {
    std::uint32_t size = sizeof(ShutdownCensusSnapshot);
    std::uint32_t version = kShutdownCensusVersion;
    std::uint64_t token = 0;
    std::int64_t beginQpc = 0;
    std::int64_t endQpc = 0;
    std::uint64_t samples = 0;
    std::int64_t firstQpc = 0;
    std::int64_t lastQpc = 0;
    std::uint32_t firstThread = 0;
    std::uint32_t lastThread = 0;
    std::uint32_t mixedThreads = 0;
    std::uint32_t saturated = 0;
};
static_assert(sizeof(ShutdownCensusSnapshot) == 72);
static_assert(std::is_standard_layout_v<ShutdownCensusSnapshot> &&
              std::is_trivially_copyable_v<ShutdownCensusSnapshot>);

using BeginShutdownCensus = std::uint64_t (WINAPI*)(std::uint32_t version);
using EndShutdownCensus = BOOL (WINAPI*)(std::uint64_t token, ShutdownCensusSnapshot* result);
} // namespace edvr
