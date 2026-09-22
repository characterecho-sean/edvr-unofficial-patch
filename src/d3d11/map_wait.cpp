#include "map_wait.h"

namespace edvr {

namespace detail {
std::atomic<bool> g_mapWaitArmed{false};
std::atomic<uint64_t> g_readCalls{0}, g_readTicks{0}, g_writeCalls{0}, g_writeTicks{0},
                      g_longestTicks{0}, g_slowCalls{0};
}  // namespace detail

void mapWaitArm(bool on) {
    detail::g_mapWaitArmed.store(on, std::memory_order_relaxed);
    // Disarming drops whatever has accumulated. The totals are per reporting
    // window, and a window nobody closed is not a measurement -- carrying it
    // into the next runtime session would put one session's Map time on
    // another session's first line.
    if (!on) mapWaitTake();
}

MapWaitTotals mapWaitTake() {
    MapWaitTotals t;
    t.readCalls = detail::g_readCalls.exchange(0, std::memory_order_relaxed);
    t.readTicks = detail::g_readTicks.exchange(0, std::memory_order_relaxed);
    t.writeCalls = detail::g_writeCalls.exchange(0, std::memory_order_relaxed);
    t.writeTicks = detail::g_writeTicks.exchange(0, std::memory_order_relaxed);
    t.longestTicks = detail::g_longestTicks.exchange(0, std::memory_order_relaxed);
    t.slowCalls = detail::g_slowCalls.exchange(0, std::memory_order_relaxed);
    return t;
}

double mapWaitMs(uint64_t ticks) {
    return static_cast<double>(ticks) * 1000.0 / static_cast<double>(detail::mapWaitFrequency());
}

} // namespace edvr
