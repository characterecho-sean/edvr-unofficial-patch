// EDVR's own GPU timing queries, counted at the End call and named by owner (perf_monitor.cpp prints the window line beside "draw hook CPU:"). The
// 2026-11-02 profile put hookedEnd (every ID3D11DeviceContext::End, EDVR's timers' included) at +0.19 ms with DLAA; this says whose queries they are
// before anyone changes one. An owner tags the timing calls it makes with GpuQueryOwnerScope (GpuIntervals does it from its template argument); a query with no
// tag counts under "other". Written every window, zeros included. tools\edvr_log.py --perf-windows parses it; its self-test holds the fragments to this file.
#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace edvr {

enum class GpuQueryOwner : uint8_t { Other, ScreenMotion, WeaponMotion, GpuCensus, ParticleCopy, UiDepth, UiContent, Count };
constexpr size_t kGpuQueryOwners = static_cast<size_t>(GpuQueryOwner::Count);

inline const char* gpuQueryOwnerName(size_t o) {
    static const char* const names[kGpuQueryOwners] = {"other", "screen_motion", "weapon_motion", "gpu_census", "particle_copy", "ui_depth", "ui_content"};
    return o < kGpuQueryOwners ? names[o] : "unknown";
}

namespace detail {
inline thread_local GpuQueryOwner g_gpuQueryOwner = GpuQueryOwner::Other;
inline std::atomic<uint32_t> g_gpuQueryEnds[kGpuQueryOwners];
}  // namespace detail

// One load and one store: the counting thread is the render thread (a timer is used on the owner context only), and the perf monitor reads.
inline void gpuQueryNoteEnd() noexcept {
    std::atomic<uint32_t>& a = detail::g_gpuQueryEnds[static_cast<size_t>(detail::g_gpuQueryOwner)];
    a.store(a.load(std::memory_order_relaxed) + 1u, std::memory_order_relaxed);
}

struct GpuQueryOwnerScope {
    GpuQueryOwner previous;
    explicit GpuQueryOwnerScope(GpuQueryOwner owner) noexcept : previous(detail::g_gpuQueryOwner) { detail::g_gpuQueryOwner = owner; }
    ~GpuQueryOwnerScope() { detail::g_gpuQueryOwner = previous; }
    GpuQueryOwnerScope(const GpuQueryOwnerScope&) = delete;
    GpuQueryOwnerScope& operator=(const GpuQueryOwnerScope&) = delete;
};

struct GpuQueryCounts {
    uint32_t ends[kGpuQueryOwners] = {};
};
inline GpuQueryCounts gpuQueryCountsNow() {
    GpuQueryCounts c;
    for (size_t o = 0; o < kGpuQueryOwners; ++o) c.ends[o] = detail::g_gpuQueryEnds[o].load(std::memory_order_relaxed);
    return c;
}

// window = the Ends since the last line, frames = the frames it covered. Per-frame means, three decimals.
inline void formatGpuQueryWindow(char* out, size_t cap, unsigned frame, unsigned frames, const GpuQueryCounts& window) {
    uint64_t total = 0;
    for (size_t o = 0; o < kGpuQueryOwners; ++o) total += window.ends[o];
    int n = std::snprintf(out, cap, "gpu timing queries: 1800-frame window ending %u; %.3f EDVR timing-query Ends a frame over %u frames (%llu in all); by owner a frame: ", frame,
                          frames ? double(total) / frames : 0.0, frames, static_cast<unsigned long long>(total));
    if (n < 0 || static_cast<size_t>(n) >= cap) return;
    size_t at = static_cast<size_t>(n);
    for (size_t o = 0; o < kGpuQueryOwners; ++o) {
        n = std::snprintf(out + at, cap - at, "%s%s %.3f", o ? ", " : "", gpuQueryOwnerName(o), frames ? double(window.ends[o]) / frames : 0.0);
        if (n < 0 || static_cast<size_t>(n) >= cap - at) return;
        at += static_cast<size_t>(n);
    }
    std::snprintf(out + at, cap - at, ".");
}

}  // namespace edvr
