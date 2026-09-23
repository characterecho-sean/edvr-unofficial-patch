#pragma once
#include "../common/native_temporal.h"
#include "geometry_snapshot.h"
#include <algorithm>
#include <cstring>
#include <mutex>
#include <wrl/client.h>

namespace edvr::openxr {
// Unlike the menu, projection notes arrive on the game's system-query callers.
// Fence the table's lifetime independently of the XR owner's serialization.
// Provider begin/note/invalidate/close do CPU work only; treat is dispatched
// by the host on its producer. No client lock spans a producer dispatch/wait.
class NativeTemporalClient final {
 public:
  HRESULT acquire(HMODULE provider, ID3D11Device* device, uint64_t generation) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (table_.context) return E_PENDING;
    if (!provider || !device || !generation) return E_INVALIDARG;
    const auto entry = GetProcAddress(provider, "edvrAcquireNativeTemporal");
    if (!owned(provider, entry)) return E_NOINTERFACE;
    EdvrNativeTemporalRequest request{sizeof(request), EDVR_NATIVE_TEMPORAL_VERSION_1, device, generation};
    EdvrNativeTemporalTable candidate{sizeof(candidate), EDVR_NATIVE_TEMPORAL_VERSION_1};
    const auto result = reinterpret_cast<decltype(&edvrAcquireNativeTemporal)>(entry)(&request, &candidate);
    if (result != S_OK) return FAILED(result) ? result : E_NOINTERFACE;
    if (candidate.size != sizeof(candidate) || candidate.version != EDVR_NATIVE_TEMPORAL_VERSION_1 ||
        !candidate.context || !owned(provider, candidate.beginFrame) || !owned(provider, candidate.noteProjection) ||
        !owned(provider, candidate.treatEye) || !owned(provider, candidate.invalidate) || !owned(provider, candidate.close) ||
        !owned(provider, candidate.skipEye))
      return E_NOINTERFACE;
    table_ = candidate; generation_ = generation;
    return S_OK;
  }
  bool acquired() const { std::lock_guard<std::mutex> lock(mutex_); return table_.context != nullptr; }
  // askedWidth/askedHeight: what the game is told now, max over eyes (the
  // geometry's own widths are what the frame was rendered for); 0 unknown.
  HRESULT begin(const GeometryInput& geometry, uint64_t referenceGeneration, float (&shift)[2][2],
      const vr::HmdMatrix34_t* renderedHead = nullptr, uint32_t askedWidth = 0, uint32_t askedHeight = 0,
      float trueUp = 0.0f, float trueDown = 0.0f) {
    std::memset(shift, 0, sizeof(shift));
    GeometrySnapshot snapshot{};
    if (!makeGeometrySnapshot(geometry, snapshot) || !referenceGeneration) { invalidate(); return E_INVALIDARG; }
    std::lock_guard<std::mutex> lock(mutex_);
    if (!table_.context) return S_FALSE;
    EdvrNativeTemporalFrame frame{sizeof(frame), EDVR_NATIVE_TEMPORAL_VERSION_1};
    frame.generation = generation_; frame.referenceGeneration = referenceGeneration; frame.sequence = geometry.sequence;
    std::memcpy(frame.head, snapshot.headToLocal.m, sizeof(frame.head));
    if (renderedHead) std::memcpy(frame.head, renderedHead->m, sizeof(frame.head));
    for (unsigned eye = 0; eye < 2; ++eye) {
      std::memcpy(frame.eyeToHead[eye], snapshot.eyeToHead[eye].m, sizeof(frame.eyeToHead[eye]));
      const auto& raw = snapshot.raw[eye];
      frame.frusta[eye][0] = raw.left; frame.frusta[eye][1] = raw.right;
      frame.frusta[eye][2] = raw.top; frame.frusta[eye][3] = raw.bottom;
    }
    frame.recommendedWidth = (std::max)(geometry.width[0], geometry.width[1]);
    frame.recommendedHeight = (std::max)(geometry.height[0], geometry.height[1]);
    frame.askedWidth = askedWidth;
    frame.askedHeight = askedHeight;
    frame.trueUp = trueUp;
    frame.trueDown = trueDown;
    EdvrNativeTemporalProjection output{sizeof(output), EDVR_NATIVE_TEMPORAL_VERSION_1};
    const auto result = table_.beginFrame(table_.context, &frame, &output);
    if (result != S_OK) return result;
    if (output.size != sizeof(output) || output.version != EDVR_NATIVE_TEMPORAL_VERSION_1) return E_UNEXPECTED;
    for (const auto& eye : output.tangentShift) for (float value : eye) if (!std::isfinite(value)) return E_UNEXPECTED;
    std::memcpy(shift, output.tangentShift, sizeof(shift));
    return S_OK;
  }
  void noteProjection(uint64_t sequence, uint32_t eye, float nearZ, float farZ) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (table_.context) table_.noteProjection(table_.context, sequence, eye, nearZ, farZ);
  }
  HRESULT treat(uint64_t sequence, unsigned eye, ID3D11Texture2D* source, const vr::VRTextureBounds_t* bounds,
      Microsoft::WRL::ComPtr<ID3D11Texture2D>& output, vr::VRTextureBounds_t& outputBounds) {
    output.Reset(); outputBounds = {};
    std::lock_guard<std::mutex> lock(mutex_);
    if (!table_.context) return S_FALSE;
    const float box[4] = {bounds ? bounds->uMin : 0, bounds ? bounds->vMin : 0,
                         bounds ? bounds->uMax : 1, bounds ? bounds->vMax : 1};
    float outBox[4]{}; ID3D11Texture2D* raw = nullptr;
    const auto result = table_.treatEye(table_.context, sequence, eye, source, box, &raw, outBox);
    output.Attach(raw);
    if (result != S_OK) { output.Reset(); return result; }
    if (!output) return E_UNEXPECTED;
    for (float value : outBox) if (!std::isfinite(value) || value < 0 || value > 1) { output.Reset(); return E_UNEXPECTED; }
    if (outBox[0] == outBox[2] || outBox[1] == outBox[3]) { output.Reset(); return E_UNEXPECTED; }
    outputBounds = {outBox[0], outBox[1], outBox[2], outBox[3]};
    return S_OK;
  }
  HRESULT invalidate() {
    std::lock_guard<std::mutex> lock(mutex_);
    return table_.context ? table_.invalidate(table_.context) : S_FALSE;
  }
  HRESULT skip(uint64_t sequence, unsigned eye, bool jumpOnly, uint32_t verdict) {
    std::lock_guard<std::mutex> lock(mutex_);
    return table_.context ? table_.skipEye(table_.context, sequence, eye, jumpOnly ? 1u : 0u, verdict) : S_FALSE;
  }
  HRESULT close() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!table_.context) return S_FALSE;
    const auto result = table_.close(table_.context);
    if (SUCCEEDED(result)) table_ = {};
    return result;
  }
 private:
  template<class T> static bool owned(HMODULE provider, T address) {
    if (!address) return false;
    HMODULE implementation = nullptr;
    return GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(address), &implementation) && implementation == provider;
  }
  mutable std::mutex mutex_;
  EdvrNativeTemporalTable table_{};
  uint64_t generation_ = 0;
};
} // namespace edvr::openxr
