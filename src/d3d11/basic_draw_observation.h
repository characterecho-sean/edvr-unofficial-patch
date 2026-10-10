#pragma once

#include <cstdint>
#include <type_traits>

namespace edvr {

template <class T>
struct BasicDrawRead final {
    bool reached = false;
    bool known = false;
    T value{};
};

enum class BasicDrawFactKind : std::uint8_t {
    kContext = 15,
    kDistance = 16,
};

struct BasicDrawContextObservation final {
    BasicDrawRead<std::uintptr_t> contextIdentity;
    BasicDrawRead<std::uintptr_t> ownerContextIdentity;
    BasicDrawRead<std::uint32_t> glareClampBefore;
    BasicDrawRead<std::uint32_t> glareClampAfter;
};

struct BasicDrawDistanceObservation final {
    BasicDrawRead<bool> distanceEnabled;
};

struct BasicDrawObservation final {
    std::uint16_t siteId = 0;
    BasicDrawFactKind kind = BasicDrawFactKind::kContext;
    BasicDrawContextObservation context;
    BasicDrawDistanceObservation distance;
};

static_assert(std::is_standard_layout<BasicDrawObservation>::value &&
                  std::is_trivially_copyable<BasicDrawObservation>::value,
              "Basic draw observations must remain pointer-free PODs");

} // namespace edvr
