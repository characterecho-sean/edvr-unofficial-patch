#pragma once
#include "../common/gpu_span_state.h"

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
