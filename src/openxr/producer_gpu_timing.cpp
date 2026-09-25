#include "producer_gpu_timing.h"
#include "native_trace.h"
#include <algorithm>
#include <cmath>

namespace edvr::openxr {
namespace {
constexpr double kMaxFieldMs = 600000.0;
}

void ProducerGpuTiming::logEnabled(bool ok, const char* reason) noexcept {
    nativeTracePrintf("native_producer_gpu,enabled=%d,reason=%s\n", ok?1:0, reason);
}

void ProducerGpuTiming::beginCopy(ID3D11Device* device, ID3D11DeviceContext* context,
                                  const edvr::GpuSpanD3D11Ops& ops) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!attempted_) {
        attempted_ = true;
        thread_ = GetCurrentThreadId();
        if (!device || !context) { logEnabled(false, "null_device_or_context"); }
        else if (context->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE) { logEnabled(false, "not_immediate_context"); }
        else {
            auto candidate = std::unique_ptr<edvr::GpuSpanD3D11Driver>(new (std::nothrow)
                edvr::GpuSpanD3D11Driver(device, context, ops, 2));
            if (!candidate || !candidate->isBound()) { logEnabled(false, "driver_not_bound"); }
            else { driver_ = std::move(candidate); initialized_ = true; logEnabled(true, "ok"); }
        }
    }
    if (!initialized_ || GetCurrentThreadId() != thread_ || open_ >= 0) return;
    unsigned index = kSlots;
    for (unsigned i = 0; i < kSlots; ++i) if (!slots_[i].pending) { index = i; break; }
    if (index == kSlots) return; // ring exhausted; this copy goes untimed
    auto& s = slots_[index];
    if (!s.allocated) {
        if (!driver_->create(index)) return; // leave allocated=false; retry next call
        s.allocated = true;
    }
    if (!driver_->begin(index) || !driver_->timestamp(index, 0)) {
        driver_->destroy(index); s = {};
        return;
    }
    open_ = static_cast<int>(index);
}

void ProducerGpuTiming::endCopy() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (open_ < 0) return;
    const unsigned index = static_cast<unsigned>(open_);
    open_ = -1;
    auto& s = slots_[index];
    if (!driver_->timestamp(index, 1) || !driver_->end(index)) {
        driver_->destroy(index); s = {};
        return;
    }
    s.pending = true;
}

ProducerGpuTiming::Distribution ProducerGpuTiming::distribution() const noexcept {
    std::array<double, kCapacity> sorted = values_;
    std::sort(sorted.begin(), sorted.begin() + valueCount_);
    const auto percentile = [&](unsigned percent) {
        return valueCount_ ? sorted[(valueCount_ * percent + 99) / 100 - 1] : 0.0;
    };
    return {percentile(50), percentile(95), percentile(99), valueCount_ ? sorted[valueCount_ - 1] : 0.0};
}

void ProducerGpuTiming::closeWindow(uint64_t nowMs) noexcept {
    unsigned dropped = 0;
    for (unsigned i = 0; i < kSlots; ++i) {
        auto& s = slots_[i];
        if (s.pending) { ++dropped; driver_->destroy(i); s = {}; }
    }
    const auto dist = distribution();
    const uint64_t window = ++windowIndex_;
    ++windowsClosed_; totalSamples_ += valueCount_;
    nativeTracePrintf("native_producer_gpu,window=%llu,samples=%u,pending_dropped=%u,copy=%.4f/%.4f/%.4f/%.4f,units=gpu_ms\n",
        (unsigned long long)window, valueCount_, dropped, dist.p50, dist.p95, dist.p99, dist.max);
    valueCount_ = 0;
    windowBeganMs_ = nowMs;
}

void ProducerGpuTiming::poll(uint64_t nowMs) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!initialized_ || GetCurrentThreadId() != thread_) return;
    if (!windowArmed_) { windowArmed_ = true; windowBeganMs_ = nowMs; }
    for (unsigned i = 0; i < kSlots; ++i) {
        auto& s = slots_[i];
        if (!s.pending) continue;
        GpuSpanRawSample raw{};
        const auto result = driver_->poll(i, raw);
        if (result == GpuSpanPoll::Pending) continue;
        s.pending = false;
        bool valid = false;
        if (result == GpuSpanPoll::Ready && raw.timestampsReady && !raw.disjoint && raw.frequency) {
            const auto begin = raw.ticks[0], end = raw.ticks[1];
            if (end >= begin) {
                const double ms = double(end - begin) * 1000.0 / double(raw.frequency);
                if (std::isfinite(ms) && ms >= 0 && ms <= kMaxFieldMs) {
                    if (valueCount_ < kCapacity) values_[valueCount_++] = ms;
                    valid = true;
                }
            }
        }
        if (!valid) ++disjointInvalid_; // disjoint, malformed ticks, or a poll-level failure alike
        // A Ready slot is Idle again with its queries intact: keep it, so the
        // copy path creates none in steady state. Only a failed slot is
        // released, to be re-created on its next use.
        if (result != GpuSpanPoll::Ready) { driver_->destroy(i); s = {}; }
    }
    if (nowMs < windowBeganMs_ || nowMs - windowBeganMs_ >= kWindowMs) closeWindow(nowMs);
}

void ProducerGpuTiming::shutdown() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    // Release only: GpuSpanD3D11Driver's destructor issues no context
    // command, only COM Release, which its own header blesses as safe off
    // the render thread at permanent teardown -- the same contract
    // DeviceGpuTiming::abandon() already leans on at session close.
    driver_.reset();
    initialized_ = false;
    open_ = -1;
    slots_ = {};
}

ProducerGpuTiming::Summary ProducerGpuTiming::summary() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return {windowsClosed_, totalSamples_, disjointInvalid_};
}

} // namespace edvr::openxr
