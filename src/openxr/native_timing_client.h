#pragma once

#include "../common/native_timing.h"
#include "../common/native_present_trace.h"
#include <mutex>

namespace edvr::openxr {

// The table is a borrowed capability. Calls are individually serialized so
// close cannot race a callback, but this lock is never held across a host
// graphics dispatch (the host calls these methods from inside that dispatch).
class NativeTimingClient final {
 public:
  HRESULT acquire(HMODULE provider, ID3D11Device* device, uint64_t generation) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (table_.context) return E_PENDING;
    if (!provider || !device || !generation) return E_INVALIDARG;
    const auto entry = GetProcAddress(provider, "edvrAcquireNativeTiming");
    if (!owned(provider, entry)) return E_NOINTERFACE;
    EdvrNativeTimingRequest request{sizeof(request), EDVR_NATIVE_TIMING_VERSION_3, device, generation};
    EdvrNativeTimingTable candidate{sizeof(candidate), EDVR_NATIVE_TIMING_VERSION_3};
    const auto result = reinterpret_cast<decltype(&edvrAcquireNativeTiming)>(entry)(&request, &candidate);
    if (result != S_OK) return FAILED(result) ? result : E_NOINTERFACE;
    if (candidate.size != sizeof(candidate) || candidate.version != EDVR_NATIVE_TIMING_VERSION_3 ||
        !candidate.context || !owned(provider, candidate.waitBegin) ||
        !owned(provider, candidate.waitEnd) || !owned(provider, candidate.gpuEye) ||
        !owned(provider, candidate.publishCpu) || !owned(provider, candidate.invalidate) ||
        !owned(provider, candidate.close) || !owned(provider, candidate.gpuEnabled) ||
        !owned(provider, candidate.publishDeviceGpu)) {
      if (candidate.context && owned(provider, candidate.close)) candidate.close(candidate.context);
      return E_NOINTERFACE;
    }
    table_ = candidate;
    const auto trace = GetProcAddress(provider, "edvrReadNativePresentTrace");
    presentTrace_ = owned(provider, trace) ?
        reinterpret_cast<decltype(&edvrReadNativePresentTrace)>(trace) : nullptr;
    provider_ = provider;
    generation_ = generation;
    return S_OK;
  }

  bool acquired() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return table_.context != nullptr;
  }
  bool presentTraceAvailable() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return table_.context && presentTrace_;
  }
  bool readPresentTrace(uint64_t beginUs, uint64_t endUs, EdvrNativePresentTrace& out) {
    std::lock_guard<std::mutex> lock(mutex_);
    out = {sizeof(out), EDVR_NATIVE_PRESENT_TRACE_VERSION_1};
    return table_.context && presentTrace_ &&
        presentTrace_(table_.context, beginUs, endUs, &out) == S_OK;
  }
  uint64_t waitBegin() {
    std::lock_guard<std::mutex> lock(mutex_);
    return table_.context ? table_.waitBegin(table_.context) : 0;
  }
  HRESULT waitEnd(uint64_t sequence, bool valid, int64_t predictedPeriodNs) {
    std::lock_guard<std::mutex> lock(mutex_);
    return table_.context ? table_.waitEnd(table_.context, sequence, valid ? 1u : 0u, predictedPeriodNs) : S_FALSE;
  }
  bool gpuEye(uint64_t sequence, unsigned eye, bool begin, bool accepted, ID3D11Texture2D* texture) {
    std::lock_guard<std::mutex> lock(mutex_);
    return table_.context && table_.gpuEye(table_.context, sequence, eye, begin ? 1u : 0u,
                                            accepted ? 1u : 0u, texture) != 0;
  }
  // CPU-only application segment marker. It uses the existing timing table
  // callback with the reserved eye value 2, so the v2 capability layout stays
  // binary compatible. No D3D work is issued by this path.
  bool applicationSegment(uint64_t sequence, bool begin) {
    std::lock_guard<std::mutex> lock(mutex_);
    return table_.context && table_.gpuEye(table_.context, sequence, 2u,
                                            begin ? 1u : 0u, 1u, nullptr) != 0;
  }
  // Admit a possible next GPU segment after Submit's transfer/rendezvous has
  // returned. The actual segment opens only on the next producer command (or
  // the next treatment callback), never on this admission call.
  bool producerResume(uint64_t sequence) {
    std::lock_guard<std::mutex> lock(mutex_);
    return table_.context && table_.gpuEye(table_.context, sequence, 3u, 1u, 1u, nullptr) != 0;
  }
  bool producerPause(uint64_t sequence) {
    std::lock_guard<std::mutex> lock(mutex_);
    return table_.context && table_.gpuEye(table_.context, sequence, 4u, 0u, 1u, nullptr) != 0;
  }
  bool producerSegmentEnd(uint64_t sequence, unsigned eye, ID3D11Texture2D* texture) {
    std::lock_guard<std::mutex> lock(mutex_);
    return eye < 2 && table_.context && table_.gpuEye(table_.context, sequence, 5u + eye,
                                                       0u, 1u, texture) != 0;
  }
  HRESULT publishCpu(const EdvrNativeTimingFrame& frame) {
    std::lock_guard<std::mutex> lock(mutex_);
    return table_.context ? table_.publishCpu(table_.context, &frame) : S_FALSE;
  }
  bool gpuEnabled() {
    std::lock_guard<std::mutex> lock(mutex_);
    return table_.context && table_.gpuEnabled(table_.context) == 1;
  }
  HRESULT publishDeviceGpu(const EdvrNativeDeviceGpuSample& sample) {
    std::lock_guard<std::mutex> lock(mutex_);
    return table_.context ? table_.publishDeviceGpu(table_.context, &sample) : S_FALSE;
  }
  HRESULT invalidate() {
    std::lock_guard<std::mutex> lock(mutex_);
    return table_.context ? table_.invalidate(table_.context) : S_FALSE;
  }
  HRESULT close() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!table_.context) return S_FALSE;
    const auto result = table_.close(table_.context);
    table_ = {}; presentTrace_ = nullptr; provider_ = nullptr; generation_ = 0;
    return result;
  }

 private:
  template<class T> static bool owned(HMODULE provider, T address) {
    if (!address) return false;
    HMODULE implementation = nullptr;
    const bool found = GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
        GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(address), &implementation) != FALSE;
    return found && implementation == provider;
  }
  mutable std::mutex mutex_;
  EdvrNativeTimingTable table_{};
  decltype(&edvrReadNativePresentTrace) presentTrace_ = nullptr;
  HMODULE provider_ = nullptr;
  uint64_t generation_ = 0;
};
} // namespace edvr::openxr
