#pragma once
#include "gpu_span_d3d11.h"

namespace edvr {

// One shared frequency domain for the canonical immediate context and its
// actual calling OS thread. An existing domain rejects another owner. Direct
// desk callers may bind lazily through GpuTimer::begin; game hooks bind early.
// Custom operations and their module/state must outlive shutdown AND all
// timer resets. An empty operation set uses the typed native D3D11 calls.
bool gpuTimingBind(ID3D11Device*, ID3D11DeviceContext*,
                   const GpuSpanD3D11Ops& = {}) noexcept;
bool gpuTimingAccepts(ID3D11DeviceContext*) noexcept;
// Owner identity remains meaningful when measurement has stopped. Statistics
// readback may continue on that owner without requiring another GPU timer.
bool gpuTimingOwns(ID3D11DeviceContext*) noexcept;
// Explicit owner-thread shutdown may close an unfinished disjoint interval.
bool gpuTimingShutdown(ID3D11DeviceContext*) noexcept;
// Quiescent DLL unload only: stop measurement without context commands.
// Process termination must skip this too; no locks/COM work under that path.
void gpuTimingAbandon() noexcept;

// Six reusable frame markers share the same frequency scope as GpuTimer.
// Destruction is inert. reset(ctx) is owner-thread cleanup; reset() is strictly
// Release-only and disables the domain if leases remain. Custom operations
// and their state/module must outlive every explicit driver reset.
class GpuTimingFrameDriver final : public GpuSpanDriver {
    struct State;
    State* state_ = nullptr;
public:
    GpuTimingFrameDriver() noexcept = default;
    ~GpuTimingFrameDriver() override = default;
    GpuTimingFrameDriver(const GpuTimingFrameDriver&) = delete;
    GpuTimingFrameDriver& operator=(const GpuTimingFrameDriver&) = delete;
    bool bind(ID3D11Device*, ID3D11DeviceContext*) noexcept;
    GpuSpanOwner currentOwner() const noexcept;
    bool create(unsigned) noexcept override;
    bool begin(unsigned) noexcept override;
    bool timestamp(unsigned, unsigned) noexcept override;
    bool end(unsigned) noexcept override;
    GpuSpanPoll poll(unsigned, GpuSpanRawSample&) noexcept override;
    void destroy(unsigned) noexcept override;
    void reset(ID3D11DeviceContext* = nullptr) noexcept;
};

enum class GpuTimerPoll { Pending, Ready, Invalid };

// A reusable timestamp pair that borrows the domain's disjoint interval.
// No Begin/End/GetData/Flush/Release runs from destruction, including static
// destruction during process exit. Owners MUST reset during explicit cleanup.
// Copying is forbidden; move assignment explicitly resets its old destination.
class GpuTimer {
    struct State;
    State* state_ = nullptr;
public:
    GpuTimer() noexcept = default;
    ~GpuTimer() = default;
    GpuTimer(const GpuTimer&) = delete;
    GpuTimer& operator=(const GpuTimer&) = delete;
    GpuTimer(GpuTimer&&) noexcept;
    GpuTimer& operator=(GpuTimer&&) noexcept;

    bool begin(ID3D11Device*, ID3D11DeviceContext*) noexcept;
    bool end(ID3D11DeviceContext*) noexcept;
    // Wrong context/thread returns Pending and touches no mutable query state.
    // Ready/Invalid consumes the sample. S_FALSE never discards a sample;
    // elapsed-time expiry, errors and disjoint results are Invalid, not zero.
    // New samples also sweep expired inactive producers at most once per 100 ms.
    GpuTimerPoll poll(ID3D11DeviceContext*, double& ms) noexcept;
    // With the verified owner context, explicitly cancels an unfinished scope.
    // Without it this is Release-only: an unfinished scope disables its domain
    // until owner-thread shutdown. Call only when this timer is quiescent.
    void reset(ID3D11DeviceContext* = nullptr) noexcept;
};

} // namespace edvr
