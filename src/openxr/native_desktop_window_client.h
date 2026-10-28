#pragma once
#include "../common/native_desktop_window.h"
#include "../openvr/compat/openvr_v0_9_20.h"

namespace edvr::openxr {
// visual.desktop_window (docs\desktop-window.md): hands each captured eye's finished texture to the graphics half, which draws it into Elite's desktop window at
// Elite's Present. Optional: a graphics half without the export, or a setting of game, changes nothing. Called on the XR owner thread only; the provider guards its
// own state and never draws or releases on the calling thread.
class NativeDesktopWindowClient final {
 public:
  HRESULT acquire(HMODULE provider, ID3D11Device* device, uint64_t generation) {
    if (table_.context) return E_PENDING;
    if (!provider || !device || !generation) return E_INVALIDARG;
    const auto entry = GetProcAddress(provider, "edvrAcquireDesktopWindow");
    if (!entry || !owned(provider, entry)) return E_NOINTERFACE;
    EdvrNativeDesktopWindowRequest request{sizeof(request), EDVR_NATIVE_DESKTOP_WINDOW_VERSION_1, device, generation};
    EdvrNativeDesktopWindowTable candidate{sizeof(candidate), EDVR_NATIVE_DESKTOP_WINDOW_VERSION_1};
    const auto result = reinterpret_cast<decltype(&edvrAcquireDesktopWindow)>(entry)(&request, &candidate);
    if (result != S_OK) return FAILED(result) ? result : E_NOINTERFACE;
    if (candidate.size != sizeof(candidate) || candidate.version != EDVR_NATIVE_DESKTOP_WINDOW_VERSION_1 || !candidate.context ||
        !owned(provider, candidate.noteEye) || !owned(provider, candidate.close)) return E_NOINTERFACE;
    table_ = candidate;
    return S_OK;
  }
  bool acquired() const { return table_.context != nullptr; }
  // The eye's finished texture and its bounds, after the eye was captured. The caller keeps its own reference; the provider takes one of its own.
  void noteEye(unsigned eye, ID3D11Texture2D* texture, const vr::VRTextureBounds_t& bounds) {
    if (!acquired() || !texture) return;
    const float box[4] = {bounds.uMin, bounds.vMin, bounds.uMax, bounds.vMax};
    table_.noteEye(table_.context, eye, texture, box);
  }
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
    return GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
        GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(address), &implementation)
        && implementation == provider;
  }
  EdvrNativeDesktopWindowTable table_{};
};
} // namespace edvr::openxr
