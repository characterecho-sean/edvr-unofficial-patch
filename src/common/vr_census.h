#pragma once

// CPU-only, bounded startup evidence. Each DLL has its own ordinal; QPC and
// thread IDs relate the two logs. This does not establish context ownership or
// issue any GPU query. Enable before launch; changing the key requires restart.
#include <atomic>
#include <cstdint>
#include "config.h"
#include "log.h"

namespace edvr {
enum class VrCensusEvent : unsigned {
    WaitEnter, WaitExit, SubmitEnter, SubmitExit, PresentEnter, PresentExit,
    Draw, DrawIndexed, DrawInstanced, DrawIndexedInstanced,
    DrawIndirect, DrawIndexedIndirect, Dispatch, DispatchIndirect,
    Copy, CopyRegion, Update, Resolve, ClearRtv, ClearDsv, ExecuteList,
    Count
};
inline std::atomic<bool> g_vrCensusEnabled{false};
inline std::atomic<uint32_t> g_vrCensusCounts[static_cast<unsigned>(VrCensusEvent::Count)]{};
inline std::atomic<uint32_t> g_vrCensusOrdinal{0};

inline bool vrCensusEnabled() noexcept { return g_vrCensusEnabled.load(std::memory_order_relaxed); }
inline void vrCensusConfigure() {
    g_vrCensusEnabled.store(Config::get().getBool("advanced.openvr_census", false));
    if (vrCensusEnabled()) Log::get().note(
        "VR order census enabled: CPU observations only; at most 64 entries per "
        "frame-event kind and 16 per GPU-command kind. QPC joins gfx/vr logs; "
        "ordinals are local to each DLL. Missing observations are not proof of absence.");
}
inline void vrCensusNote(VrCensusEvent event, const void* subject = nullptr,
                         int detail = 0, uint32_t frame = 0) {
    if (!vrCensusEnabled()) return;
    const unsigned i = static_cast<unsigned>(event);
    if (i >= static_cast<unsigned>(VrCensusEvent::Count)) return;
    auto& count = g_vrCensusCounts[i];
    const uint32_t limit = i < static_cast<unsigned>(VrCensusEvent::Draw) ? 64u : 16u;
    uint32_t n = count.load(std::memory_order_relaxed);
    do { if (n >= limit) return; }
    while (!count.compare_exchange_weak(n, n + 1, std::memory_order_relaxed));
    static constexpr const char* names[] = {
        "WaitEnter", "WaitExit", "SubmitEnter", "SubmitExit", "PresentEnter", "PresentExit",
        "Draw", "DrawIndexed", "DrawInstanced", "DrawIndexedInstanced",
        "DrawIndirect", "DrawIndexedIndirect", "Dispatch", "DispatchIndirect",
        "Copy", "CopyRegion", "Update", "Resolve", "ClearRtv", "ClearDsv", "ExecuteList"
    };
    LARGE_INTEGER qpc{};
    QueryPerformanceCounter(&qpc);
    const auto ordinal = g_vrCensusOrdinal.fetch_add(1, std::memory_order_relaxed) + 1;
    Log::get().note("VR order census: qpc=%lld ordinal=%u thread=%lu event=%s "
                    "subject=%p detail=%d frame=%u sample=%u/%u",
                    qpc.QuadPart, ordinal, GetCurrentThreadId(), names[i], subject,
                    detail, frame, n + 1, limit);
}
// For GPU command events, detail is D3D11_DEVICE_CONTEXT_TYPE (0 immediate,
// 1 deferred); subject is the actual intercepted context, including foreign
// contexts. Enter/exit scopes only observe CPU order, never GPU completion.
struct VrCensusScope {
    VrCensusEvent end;
    const void* subject;
    int detail;
    uint32_t frame;
    VrCensusScope(VrCensusEvent begin, VrCensusEvent finish, const void* p,
                   int d = 0, uint32_t f = 0) : end(finish), subject(p), detail(d), frame(f) {
        vrCensusNote(begin, p, d, f);
    }
    ~VrCensusScope() { vrCensusNote(end, subject, detail, frame); }
};
}
