#pragma once
#include "gpu_work_observer.h"
#include "../common/native_timing.h"
#include "../d3d11/gpu_span_d3d11.h"
#include <array>
#include <mutex>
#include <memory>

namespace edvr::openxr {
// One disjoint scope covers four narrow intervals on a separate XR context.
// CPU lifecycle calls only change policy. Context calls belong to actual GPU
// work boundaries, never pose wait, reset, shutdown or blocking readback.
class DeviceGpuTiming final : public GpuWorkObserver {
public:
    static constexpr unsigned kSlots = 8;
    DeviceGpuTiming() noexcept = default;
    ~DeviceGpuTiming() override;
    DeviceGpuTiming(const DeviceGpuTiming&) = delete;
    DeviceGpuTiming& operator=(const DeviceGpuTiming&) = delete;
    bool initialize(ID3D11Device*, ID3D11DeviceContext*, const edvr::GpuSpanD3D11Ops& = {}) noexcept;
    bool beginFrame(uint64_t sequence, bool enabled) noexcept;
    bool acceptFrame(uint64_t sequence) noexcept;
    void invalidate() noexcept;
    unsigned poll(EdvrNativeDeviceGpuSample* out, unsigned capacity) noexcept;
    // Permanent retirement with the device: Release only, never resume queries.
    void abandon() noexcept;
    void beginGpuWork(unsigned phase, ID3D11DeviceContext*) noexcept override;
    void endGpuWork(unsigned phase, ID3D11DeviceContext*) noexcept override;
private:
    struct Slot {
        uint64_t sequence=0, completedAtMs=0;
        unsigned phaseMask=0, phaseCount=0;
        std::array<unsigned,4> order{};
        int openPhase=-1;
        bool allocated=false, queryOpen=false, pending=false, accepted=false;
    };
    bool owner(ID3D11DeviceContext*) const noexcept;
    void retireLocked() noexcept;
    void failCurrentLocked(uint32_t status) noexcept;
    void retireFrameLocked(bool all) noexcept;
    std::mutex mutex_;
    std::unique_ptr<edvr::GpuSpanD3D11Driver> driver_;
    ID3D11DeviceContext* context_=nullptr; // driver retains it
    DWORD thread_=0;
    std::array<Slot,kSlots> slots_{};
    int active_=-1;
    uint64_t sequence_=0, lastSequence_=0, lastPublishedSequence_=0, statusAtMs_=0;
    bool initialized_=false, abandoned_=false, enabled_=false, stopped_=false;
    bool frameStarted_=false, frameAccepted_=false, statusPending_=false;
    uint32_t frameStatus_=EdvrNativeGpuPending;
    unsigned retireMask_=0;
};
}
