#pragma once
#include "gpu_span_d3d11.h"
#include "../common/gpu_disjoint_clock.h"

namespace edvr {
// Reuse the reviewed native query lifecycle without allocating timestamp
// markers: callers keep their timestamp pairs and borrow this clock's one
// disjoint record. Explicit clock.shutdown must precede backend destruction.
class DisjointD3D11Backend final : public DisjointBackend {
    GpuSpanD3D11Driver driver_;
public:
    DisjointD3D11Backend(ID3D11Device* dev, ID3D11DeviceContext* ctx,
                         const GpuSpanD3D11Ops& ops = {}) noexcept
        : driver_(dev, ctx, ops, 0) {}
    GpuSpanOwner currentOwner() const noexcept override { return driver_.currentOwner(); }
    bool create(unsigned slot) noexcept override { return driver_.create(slot); }
    bool begin(unsigned slot) noexcept override { return driver_.begin(slot); }
    bool end(unsigned slot) noexcept override { return driver_.end(slot); }
    DisjointResult poll(unsigned slot) noexcept override {
        GpuSpanRawSample raw{};
        const auto result = driver_.poll(slot, raw);
        if (result == GpuSpanPoll::Pending) return {};
        if (result == GpuSpanPoll::Failed)
            return {DisjointStatus::Failed, 0, false, DisjointReason::DriverFailure};
        return {DisjointStatus::Ready, raw.frequency, raw.disjoint, DisjointReason::None};
    }
    void destroy(unsigned slot) noexcept override { driver_.destroy(slot); }
};
} // namespace edvr
