#pragma once

// Lookup-only diagnostic bridge between the paired proxies. This never starts
// graphics or VR. Callers initialize size/version; rejected requests leave the
// snapshot untouched. A nonzero token owns one window until its matching end.
#include <windows.h>
#include <cstdint>
#include <type_traits>

namespace edvr {
inline constexpr std::uint32_t kShutdownCensusVersion = 2;
enum class ShutdownOwnerState : std::uint32_t {
    Unobserved = 0, Alive = 1, Exited = 2, Unavailable = 3,
};
// A passive snapshot, never permission to borrow the application's context.
// The retained handle identifies the original observed caller despite ID reuse.
struct ShutdownRenderSnapshot {
    std::uint32_t ownerThread = 0;
    ShutdownOwnerState ownerState = ShutdownOwnerState::Unobserved;
    std::uint32_t ownerError = 0;
    std::uint32_t ownerChanged = 0;
    std::uint32_t lastEnterThread = 0;
    std::uint32_t lastExitThread = 0;
    std::uint32_t activityInvalid = 0;
    std::uint32_t reserved = 0;
    std::uint64_t activePresents = 0;
    std::uint64_t enteredPresents = 0;
    std::uint64_t exitedPresents = 0;
    std::int64_t lastEnterQpc = 0;
    std::int64_t lastExitQpc = 0;
    std::int64_t lastServiceQpc = 0;
};
static_assert(sizeof(ShutdownRenderSnapshot) == 80);
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
    ShutdownRenderSnapshot renderBegin{};
    ShutdownRenderSnapshot renderEnd{};
};
static_assert(sizeof(ShutdownCensusSnapshot) == 232);
static_assert(std::is_standard_layout_v<ShutdownCensusSnapshot> &&
              std::is_trivially_copyable_v<ShutdownCensusSnapshot>);

using BeginShutdownCensus = std::uint64_t (WINAPI*)(std::uint32_t version);
using EndShutdownCensus = BOOL (WINAPI*)(std::uint64_t token, ShutdownCensusSnapshot* result);
} // namespace edvr
