#pragma once

#include <d3d11.h>
#include <wrl/client.h>
#include "../openvr/compat/openvr_v0_9_20.h"

namespace edvr::openxr {

class EyeCapture final {
 public:
  EyeCapture() = default;
  ~EyeCapture();
  EyeCapture(const EyeCapture&) = delete;
  EyeCapture& operator=(const EyeCapture&) = delete;

  // The device is retained, as is its immediate context. The caller owns
  // context serialization and keeps the device usable until shutdown.
  HRESULT initialize(ID3D11Device* device);
  void reset();
  void shutdown();

  vr::EVRCompositorError capture(vr::EVREye eye, const vr::Texture_t* texture,
                                 const vr::VRTextureBounds_t* bounds = nullptr,
                                 vr::EVRSubmitFlags flags = vr::Submit_Default,
                                 bool copyPixels = true);

  // Borrowed pointers: pixels remain immutable until reset, shutdown, or the
  // next capture of the same eye. The owner supplies once-per-frame ordering;
  // this class deliberately has no frame-order policy. capture only enqueues
  // a copy on the immediate context and does not change pipeline bindings.
  ID3D11Texture2D* texture(vr::EVREye eye) const;
  vr::VRTextureBounds_t bounds(vr::EVREye eye) const;
  vr::EColorSpace colorSpace(vr::EVREye eye) const;

 private:
  struct Eye {
    Microsoft::WRL::ComPtr<ID3D11Texture2D> copy;
    vr::VRTextureBounds_t bounds{0.f, 0.f, 1.f, 1.f};
    vr::EColorSpace colorSpace = vr::ColorSpace_Auto;
    bool captured = false;
  } eyes_[2];
  Microsoft::WRL::ComPtr<ID3D11Device> device_;
  Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
  bool initialized_ = false;
};

} // namespace edvr::openxr
