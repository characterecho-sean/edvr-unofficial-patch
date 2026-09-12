#pragma once
#include "../common/gpu_span_state.h"
#include <d3d11.h>

namespace edvr {
// Optional typed seam for deterministic failure tests with REAL COM queries.
// A failed begin must issue no Begin. An end failure may have issued End;
// closure is therefore never retried. Callbacks/user outlive the driver.
struct GpuSpanD3D11Ops {
    void* user = nullptr;
    HRESULT (*createQuery)(void*, ID3D11Device*, const D3D11_QUERY_DESC*, ID3D11Query**) = nullptr;
    HRESULT (*begin)(void*, ID3D11DeviceContext*, ID3D11Asynchronous*) = nullptr;
    HRESULT (*end)(void*, ID3D11DeviceContext*, ID3D11Asynchronous*) = nullptr;
    HRESULT (*getData)(void*, ID3D11DeviceContext*, ID3D11Asynchronous*, void*, UINT, UINT) = nullptr;
    void (*releaseQuery)(void*, ID3D11Query*) noexcept = nullptr;
};

// The frequency-only mode backs the shared production timer service. The
// six-marker frame mode remains a desk adapter; no outer game bracket is active.
class GpuSpanD3D11Driver final : public GpuSpanDriver {
public:
    static constexpr unsigned kSlots = GpuSpanState::kSlots;
    GpuSpanD3D11Driver(ID3D11Device*, ID3D11DeviceContext*,
                       const GpuSpanD3D11Ops& = {}, unsigned markerCount = 6) noexcept;
    ~GpuSpanD3D11Driver() override;
    GpuSpanD3D11Driver(const GpuSpanD3D11Driver&) = delete;
    GpuSpanD3D11Driver& operator=(const GpuSpanD3D11Driver&) = delete;
    bool isBound() const noexcept { return bound_; }
    GpuSpanOwner currentOwner() const noexcept;
    bool create(unsigned) noexcept override;
    bool begin(unsigned) noexcept override;
    bool timestamp(unsigned, unsigned) noexcept override;
    bool end(unsigned) noexcept override;
    GpuSpanPoll poll(unsigned, GpuSpanRawSample&) noexcept override;
    void destroy(unsigned) noexcept override;
    bool shutdown() noexcept;
private:
    enum class State { Empty, Idle, Open, Pending, Failed };
    struct Slot {
        State state = State::Empty;
        ID3D11Query* disjoint = nullptr;
        ID3D11Query* stamps[6]{};
        unsigned issued = 0, ready = 0;
        bool disjointReady = false;
        GpuSpanRawSample raw{};
    };
    ID3D11Device* device_ = nullptr;
    ID3D11DeviceContext* context_ = nullptr;
    GpuSpanOwner owner_{}; // Immutable after construction, including thread.
    GpuSpanD3D11Ops ops_{};
    const unsigned markerCount_; // Zero for a shared frequency-only backend.
    Slot slots_[kSlots]{};
    int open_ = -1;
    bool bound_ = false, stopped_ = false, shut_ = false;
    bool ownerThread() const noexcept;
    bool removed() const noexcept;
    void release(unsigned) noexcept;
};
} // namespace edvr
