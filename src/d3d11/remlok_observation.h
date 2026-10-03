#pragma once

#include <cstdint>
#include <type_traits>

namespace edvr { namespace remlok_observation {

// A source read that preserves short-circuit reachability for offline replay.
// Skipped reads remain {false, false, 0}; reached-but-unavailable reads can be
// represented as {true, false, 0} without inventing a value.
template <class T>
struct Read final {
    bool reached = false;
    bool known = false;
    T value{};
};

enum : std::uint32_t {
    kModeStock = 0,
    kModeOuter = 1,
    kModeHide = 2,
};

struct SelectorObservation final {
    Read<std::uint32_t> outerMode;
};

struct HelperObservation final {
    Read<std::uint32_t> modeBeforeGate;
    Read<bool> dsvNonNull;
    Read<bool> resolved;
    Read<bool> isTexture2D;
    Read<std::uint32_t> width;
    Read<std::uint32_t> height;
    Read<std::uint32_t> hideMode;
    Read<bool> swap;
};

struct MutationObservation final {
    Read<std::uint32_t> matchesBefore;
    Read<std::uint32_t> matchesAfter;
    Read<std::uint64_t> hiddenBefore;
    Read<std::uint64_t> hiddenAfter;
    Read<bool> pendingRightBefore;
    Read<bool> pendingRightAfter;
};

struct Observation final {
    SelectorObservation selector;
    HelperObservation helper;
    MutationObservation mutation;
};

static_assert(std::is_standard_layout<Observation>::value &&
                  std::is_trivially_copyable<Observation>::value,
              "RemLok observations must remain pointer-free PODs");

}}  // namespace edvr::remlok_observation
