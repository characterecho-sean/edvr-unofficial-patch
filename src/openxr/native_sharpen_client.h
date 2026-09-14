#pragma once
#include "../common/native_sharpen.h"
#include "../openvr/compat/openvr_v0_9_20.h"
#include <cmath>
#include <wrl/client.h>

namespace edvr::openxr {
// The XR owner serializes calls. Acquire/treat run on the bound producer;
// invalidate/close are CPU only and need no producer rendezvous.
class NativeSharpenClient final {
 public:
  HRESULT acquire(HMODULE provider, ID3D11Device* device, uint64_t generation) {
    if (table_.context) return E_PENDING;
    if (!provider || !device || !generation) return E_INVALIDARG;
    const auto entry = GetProcAddress(provider, "edvrAcquireNativeSharpen");
    if (!owned(provider, entry)) return E_NOINTERFACE;
    EdvrNativeSharpenRequest request{sizeof(request), EDVR_NATIVE_SHARPEN_VERSION_1, device, generation};
    EdvrNativeSharpenTable candidate{sizeof(candidate), EDVR_NATIVE_SHARPEN_VERSION_1};
    const auto result = reinterpret_cast<decltype(&edvrAcquireNativeSharpen)>(entry)(&request, &candidate);
    if (result != S_OK) return FAILED(result) ? result : E_NOINTERFACE;
    if (candidate.size != sizeof(candidate) || candidate.version != EDVR_NATIVE_SHARPEN_VERSION_1 ||
        !candidate.context || !owned(provider, candidate.treatEye) ||
        !owned(provider, candidate.invalidate) || !owned(provider, candidate.close)) return E_NOINTERFACE;
    table_ = candidate;
    return S_OK;
  }
  bool acquired() const { return table_.context != nullptr; }
  HRESULT treat(uint64_t sequence, unsigned eye, ID3D11Texture2D* source, const vr::VRTextureBounds_t* bounds,
      Microsoft::WRL::ComPtr<ID3D11Texture2D>& output, vr::VRTextureBounds_t& outputBounds) {
    output.Reset(); outputBounds = {};
    if (!acquired()) return S_FALSE;
    const float box[4] = {bounds ? bounds->uMin : 0, bounds ? bounds->vMin : 0,
                         bounds ? bounds->uMax : 1, bounds ? bounds->vMax : 1};
    float outBox[4]{}; ID3D11Texture2D* raw = nullptr;
    const auto result = table_.treatEye(table_.context, sequence, eye, source, box, &raw, outBox);
    output.Attach(raw);
    if (result != S_OK) { output.Reset(); return result; }
    if (!output) return E_UNEXPECTED;
    for (float value : outBox) if (!std::isfinite(value) || value < 0 || value > 1) {
      output.Reset(); return E_UNEXPECTED;
    }
    if (outBox[0] == outBox[2] || outBox[1] == outBox[3]) { output.Reset(); return E_UNEXPECTED; }
    outputBounds = {outBox[0], outBox[1], outBox[2], outBox[3]};
    return S_OK;
  }
  HRESULT invalidate() { return acquired() ? table_.invalidate(table_.context) : S_FALSE; }
  HRESULT close() {
    if (!acquired()) return S_FALSE;
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
  EdvrNativeSharpenTable table_{};
};
} // namespace edvr::openxr
