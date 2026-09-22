#include "map_wait.h"

#include <atomic>
#include <windows.h>

// The totals (map_wait.h). Relaxed atomics: the line that reads them is a
// five-second summary, and a count that lands a Map late is not worth a
// fence on every one.
namespace {
std::atomic<uint64_t> g_readCalls{0}, g_readTicks{0}, g_writeCalls{0}, g_writeTicks{0},
                      g_longestTicks{0}, g_slowCalls{0};
uint64_t frequency() {
    static const uint64_t f = [] {
        LARGE_INTEGER q{};
        QueryPerformanceFrequency(&q);
        return q.QuadPart > 0 ? static_cast<uint64_t>(q.QuadPart) : 1u;
    }();
    return f;
}
} // namespace

namespace edvr {

namespace detail {
std::atomic<bool> g_mapWaitArmed{false};
}  // namespace detail

void mapWaitArm(bool on) {
    detail::g_mapWaitArmed.store(on, std::memory_order_relaxed);
    // Disarming drops whatever has accumulated. The totals are per reporting
    // window, and a window nobody closed is not a measurement -- carrying it
    // into the next runtime session would put one session's Map time on
    // another session's first line.
    if (!on) mapWaitTake();
}

void mapWaitNote(D3D11_MAP type, uint64_t ticks) {
    const bool read = type == D3D11_MAP_READ || type == D3D11_MAP_READ_WRITE;
    (read ? g_readCalls : g_writeCalls).fetch_add(1, std::memory_order_relaxed);
    (read ? g_readTicks : g_writeTicks).fetch_add(ticks, std::memory_order_relaxed);
    if (ticks * 10000u > frequency()) g_slowCalls.fetch_add(1, std::memory_order_relaxed);   // past 100 us
    uint64_t longest = g_longestTicks.load(std::memory_order_relaxed);
    while (ticks > longest &&
           !g_longestTicks.compare_exchange_weak(longest, ticks, std::memory_order_relaxed)) {}
}

MapWaitTotals mapWaitTake() {
    MapWaitTotals t;
    t.readCalls = g_readCalls.exchange(0, std::memory_order_relaxed);
    t.readTicks = g_readTicks.exchange(0, std::memory_order_relaxed);
    t.writeCalls = g_writeCalls.exchange(0, std::memory_order_relaxed);
    t.writeTicks = g_writeTicks.exchange(0, std::memory_order_relaxed);
    t.longestTicks = g_longestTicks.exchange(0, std::memory_order_relaxed);
    t.slowCalls = g_slowCalls.exchange(0, std::memory_order_relaxed);
    return t;
}

double mapWaitMs(uint64_t ticks) {
    return static_cast<double>(ticks) * 1000.0 / static_cast<double>(frequency());
}

} // namespace edvr
