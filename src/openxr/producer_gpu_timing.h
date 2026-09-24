#pragma once
#include "../d3d11/gpu_span_d3d11.h"
#include <array>
#include <memory>
#include <mutex>

namespace edvr::openxr {

// GPU timing for the producer-side eye copy: on Elite's own device, brackets
// exactly CopyResource + Flush inside SharedTextureTransfer's producer
// executor callback (shared_texture_transfer.cpp). beginCopy/endCopy/poll
// are render-thread only, bound to the thread that first calls beginCopy;
// wrong-thread calls are safe no-ops. shutdown() alone is callable from any
// thread: it only releases queries (GpuSpanD3D11Driver's own destructor
// contract -- Release only, no context command), matching how
// DeviceGpuTiming::abandon() is used at session close. Logs its own
// native_producer_gpu trace lines directly; publishes to no ABI table.
class ProducerGpuTiming final {
public:
    static constexpr unsigned kSlots = 8;      // in-flight spans before poll() catches up
    static constexpr unsigned kCapacity = 512; // stored copy-ms samples per window
    static constexpr uint64_t kWindowMs = 30000;
    struct Distribution { double p50=0, p95=0, p99=0, max=0; };
    struct Summary { uint64_t windows=0, samples=0, disjointInvalid=0; };

    ProducerGpuTiming() noexcept = default;
    ~ProducerGpuTiming() = default;
    ProducerGpuTiming(const ProducerGpuTiming&) = delete;
    ProducerGpuTiming& operator=(const ProducerGpuTiming&) = delete;

    // Lazily creates the query ring on first call and logs
    // native_producer_gpu,enabled=1|0,reason=... exactly once, success or
    // not. Opens one span (a disjoint scope plus its first timestamp) if the
    // ring has room; otherwise a safe no-op -- the caller's copy proceeds
    // untimed either way, every time.
    void beginCopy(ID3D11Device* device, ID3D11DeviceContext* context,
                   const edvr::GpuSpanD3D11Ops& ops = {}) noexcept;
    // No-op unless beginCopy() opened a span on this call chain.
    void endCopy() noexcept;
    // Never blocks: polling uses D3D11_ASYNC_GETDATA_DONOTFLUSH throughout.
    // Retires every finished span into the open window's distribution, and
    // past kWindowMs since that window opened, closes and logs it -- even
    // with zero samples -- then arms the next one from nowMs.
    void poll(uint64_t nowMs) noexcept;

    // Any thread; release only (see class comment above). Logs nothing.
    void shutdown() noexcept;
    // Any thread, mutex-guarded: cumulative totals across every window this
    // object has closed so far. Used for native_producer_gpu_summary.
    Summary summary() const noexcept;

private:
    struct Slot { bool allocated=false, pending=false; };
    void logEnabled(bool ok, const char* reason) noexcept;
    void closeWindow(uint64_t nowMs) noexcept;
    Distribution distribution() const noexcept;

    mutable std::mutex mutex_;
    std::unique_ptr<edvr::GpuSpanD3D11Driver> driver_;
    std::array<Slot, kSlots> slots_{};
    int open_ = -1;
    DWORD thread_ = 0;
    bool attempted_ = false, initialized_ = false;

    std::array<double, kCapacity> values_{};
    unsigned valueCount_ = 0;
    uint64_t windowIndex_ = 0, windowBeganMs_ = 0;
    bool windowArmed_ = false;

    uint64_t windowsClosed_ = 0, totalSamples_ = 0, disjointInvalid_ = 0;
};

} // namespace edvr::openxr
