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

// The GPU pass profiler's own frequency scopes (gpu_pass_profile.cpp): one per captured frame, held in its slot from open until
// its result is read and released. Open refuses while ANY scope is open (a census timer, the application-frame ring, or another
// slot), so the profiler never borrows a scope it did not open and each frame's stamps lie inside exactly that frame's scope.
// GPU pass profile v3: one slot per captured frame (slot = frame index), so this must be at least the profiler's kMaxCaptureFrames;
// gpu_pass_profile.cpp static_asserts it. v2 had 4, and every frame from the fifth was refused as "already open".
constexpr unsigned kPassScopeSlots = 128;
bool gpuTimingPassScopeOpen(ID3D11DeviceContext*, unsigned slot, uint64_t now) noexcept;
// True while any scope on the shared clock is open (not necessarily the profiler's): the profiler waits at the boundary for it to end.
bool gpuTimingPassScopeHeld() noexcept;
bool gpuTimingPassScopeClose(ID3D11DeviceContext*, unsigned slot, uint64_t now) noexcept;
enum class GpuPassScopeStatus { Pending, Ready, Failed };
struct GpuPassScopeResult {
    GpuPassScopeStatus status = GpuPassScopeStatus::Pending;
    uint64_t frequency = 0;     // valid when Ready
    const char* reason = "";    // said when Failed
};
GpuPassScopeResult gpuTimingPassScopePoll(ID3D11DeviceContext*, unsigned slot, uint64_t now) noexcept;
void gpuTimingPassScopeRelease(ID3D11DeviceContext*, unsigned slot, uint64_t now) noexcept;

// The GPU pass profiler's yield (TEMPORARY, Built v13.6). While a capture is armed or running, the timers and the application-frame timing
// refuse NEW leases on the shared clock, so the profiler's frame scopes can open; a refusal is counted, never a stall. gpuTimingSetYield is the
// profiler's switch (set at the press, cleared at the last profiled frame's close); gpuTimingTakeYieldRefusals returns and clears the count of
// refused timer leases (the application frames are counted by gpuFrameTakeYieldSkips, gpu_frame_timing.h).
void gpuTimingSetYield(bool on) noexcept;
bool gpuTimingYielding() noexcept;
uint64_t gpuTimingTakeYieldRefusals() noexcept;

// The GPU census's own frequency scope (2026-10-14). Every census span (a GpuTimer pair) borrows a scope that is already open; with none open,
// each span opens a TIMESTAMP_DISJOINT of its own, takes one of the clock's eight records, and holds it until its result is polled four or more
// frames later. The application-frame clock (advanced.app_gpu_timing) kept one scope open across the render, so every census span borrowed it; with
// that clock off (the default since the AA-off cuts) a frame's two to nine spans needed two to nine records each and the pool ran out: 85-90% of
// spans failed in the 2026-10-13 flight. EnsureCensus opens ONE scope on the first timed span of a frame (refused while another scope is open or
// the profiler yields) and EndCensus closes it at the frame boundary. Owner context only; the caller opens it only while the application-frame clock is off.
bool gpuTimingCensusScopeEnsure(ID3D11DeviceContext*, uint64_t now) noexcept;
void gpuTimingCensusScopeEnd(ID3D11DeviceContext*, uint64_t now) noexcept;
bool gpuTimingCensusScopeOpen() noexcept;

enum class GpuTimerPoll { Pending, Ready, Invalid };

// A reusable timestamp pair that borrows the domain's disjoint interval.
// No Begin/End/GetData/Flush/Release runs from destruction, including static
// destruction during process exit. Owners MUST reset during explicit cleanup.
// Copying is forbidden; move assignment explicitly resets its old destination.
class GpuTimer {
    struct State;
    State* state_ = nullptr;
    bool beginImpl(ID3D11Device*, ID3D11DeviceContext*, bool borrowedFrame) noexcept;
public:
    GpuTimer() noexcept = default;
    ~GpuTimer() = default;
    GpuTimer(const GpuTimer&) = delete;
    GpuTimer& operator=(const GpuTimer&) = delete;
    GpuTimer(GpuTimer&&) noexcept;
    GpuTimer& operator=(GpuTimer&&) noexcept;

    bool begin(ID3D11Device*, ID3D11DeviceContext*) noexcept;
    // Requires the shared application-frame frequency scope to be open. This
    // never starts a standalone TIMESTAMP_DISJOINT query.
    bool beginBorrowedFrame(ID3D11Device*, ID3D11DeviceContext*) noexcept;
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
