#pragma once
#include "../common/gpu_span_state.h"
#include <atomic>

struct ID3D11Device;
struct ID3D11DeviceContext;

namespace edvr {
// Bind/configure on the canonical immediate-context thread. No destructor
// callbacks; explicit abandon is quiescent and Release-only.
bool gpuFrameBind(ID3D11Device*, ID3D11DeviceContext*, bool enabled) noexcept;
void gpuFrameConfigure(bool enabled) noexcept;
void gpuFrameCommand(ID3D11DeviceContext*) noexcept;
bool gpuFrameInternal() noexcept;
void gpuFramePresent(ID3D11DeviceContext*, uint64_t nextSourceFrame) noexcept;
void gpuFrameAbandon() noexcept;

namespace detail {
// g_gpuFrameInternal mirrors gpuFrameCommand.cpp's own reentrancy flag
// (renamed out of its anonymous namespace so this header can see it, written
// only from this file's own thread via the Internal RAII guard).
// g_gpuFrameCommandLive is "is there a live, enabled controller bound",
// i.e. gpuFrameCommand's OWN combined `!c || !c->enabled || ...` test, minus
// the ctx-equals-the-owner's-context part -- that depends on the caller's
// argument, not on anything that can be precomputed. Kept in sync at every
// site that binds/abandons the controller or flips its enabled flag
// (gpu_frame_timing.cpp).
extern thread_local bool g_gpuFrameInternal;
extern std::atomic<bool> g_gpuFrameCommandLive;
}  // namespace detail

// gpuFrameCommand's own first tests, minus the ctx match (see above).
// Necessary, not sufficient: a call let through here can still find
// gpuFrameCommand returning at the ctx check, or at owns() -- and the
// foreign-thread poison() inside owns() is unaffected either way, since it
// is only ever reached (from here or directly) after passing these same
// tests for real inside gpuFrameCommand itself.
inline bool gpuFrameCommandMightAct() noexcept {
    return !detail::g_gpuFrameInternal &&
           detail::g_gpuFrameCommandLive.load(std::memory_order_relaxed);
}

struct GpuFrameSnapshot {
    bool enabled = false, haveResult = false;
    GpuSpanResult result{};
    uint64_t capturedAtMs = 0;
};
// CPU snapshots only. ApplicationRender samples contain the sum of bounded,
// non-overlapping producer GPU segments; runtime and transfer waits are not
// included. Consumers must reject samples older than two seconds; invalid
// samples replace previous valid values and retain original identity.
GpuFrameSnapshot gpuFrameSnapshot() noexcept;
// Non-destructive bounded completion history for benchmark collectors. Cursor
// is an event ordinal, independent of frame sequence; dropped reports count
// completions overwritten before the caller could observe them.
unsigned gpuFrameReadCompletions(uint64_t& cursor, GpuFrameSnapshot* out,
                                 unsigned capacity, uint64_t& dropped) noexcept;
const char* gpuFrameReason(GpuSpanReason) noexcept;
} // namespace edvr
