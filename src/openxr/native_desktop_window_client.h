#pragma once
#include "../common/native_desktop_window.h"
#include "../openvr/compat/openvr_v0_9_20.h"
#include <cmath>

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
    // Version 2 first (the field of view rides with each eye), then version 1 for a graphics half that has not got it.
    HRESULT result = E_NOINTERFACE;
    for (const uint32_t version : {EDVR_NATIVE_DESKTOP_WINDOW_VERSION_2, EDVR_NATIVE_DESKTOP_WINDOW_VERSION_1}) {
      EdvrNativeDesktopWindowRequest request{sizeof(request), version, device, generation};
      EdvrNativeDesktopWindowTable candidate{sizeof(candidate), version};
      result = reinterpret_cast<decltype(&edvrAcquireDesktopWindow)>(entry)(&request, &candidate);
      if (result != S_OK) { result = FAILED(result) ? result : E_NOINTERFACE; continue; }
      if (candidate.size != sizeof(candidate) || candidate.version != version || !candidate.context ||
          !owned(provider, candidate.noteEye) || !owned(provider, candidate.close)) { result = E_NOINTERFACE; continue; }
      table_ = candidate;
      return S_OK;
    }
    return result;
  }
  bool acquired() const { return table_.context != nullptr; }
  // 1 or 2: the version the graphics half answered with; 0 before acquire.
  uint32_t version() const { return table_.context ? table_.version : 0; }
  // The eye's finished texture and its bounds, after the eye was captured, and the field of view its pixels cover as {angleLeft, angleRight, angleUp, angleDown} in radians
  // (null: none; a version 1 graphics half ignores it). The caller keeps its own reference; the provider takes one of its own.
  void noteEye(unsigned eye, ID3D11Texture2D* texture, const vr::VRTextureBounds_t& bounds, const float* angles = nullptr) {
    if (!acquired() || !texture) return;
    const float box[4] = {bounds.uMin, bounds.vMin, bounds.uMax, bounds.vMax};
    if (table_.version >= EDVR_NATIVE_DESKTOP_WINDOW_VERSION_2) {
      const bool finite = angles && std::isfinite(angles[0]) && std::isfinite(angles[1]) && std::isfinite(angles[2]) && std::isfinite(angles[3]);
      reinterpret_cast<EdvrNativeDesktopWindowNoteEye2>(table_.noteEye)(table_.context, eye, texture, box, finite ? angles : nullptr);
    } else {
      table_.noteEye(table_.context, eye, texture, box);
    }
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
