#pragma once

#include <d3d11.h>
#include <memory>
#include <thread>
#include <wrl/client.h>
#include "submission_measurement.h"
#include "../openvr/compat/openvr_v0_9_20.h"

namespace edvr::openxr { class ImmediateExecutor; class SharedTextureTransfer; struct GpuWorkObserver; }

namespace edvr::openxr {

class EyeCapture final {
 public:
  EyeCapture();
  ~EyeCapture();
  EyeCapture(const EyeCapture&) = delete;
  EyeCapture& operator=(const EyeCapture&) = delete;

  // The device is retained, as is its immediate context. The caller owns
  // context serialization and keeps the device usable until shutdown.
  HRESULT initialize(ID3D11Device* device);
  // Shared mode constructs each transfer on this consumer/XR owner thread.
  // The producer context is used only through producerExecutor. Call
  // shutdownShared successfully before normal shutdown or destruction.
  HRESULT initializeShared(ID3D11Device* producer, ID3D11Device* consumer,
                           ImmediateExecutor* producerExecutor);
  HRESULT shutdownShared(DWORD timeoutMs = 5000);
  void reset();
  void shutdown();
  // Exchange complete capture buffers after successful frame submission. No
  // pixels are copied: the other pair remains immutable while this one is
  // reused for the next frame. Both captures must have identical ownership.
  bool exchangeBuffers(EyeCapture& other);

  vr::EVRCompositorError capture(vr::EVREye eye, const vr::Texture_t* texture,
                                 const vr::VRTextureBounds_t* bounds = nullptr,
                                 vr::EVRSubmitFlags flags = vr::Submit_Default,
                                 bool copyPixels = true,
                                 GpuWorkObserver* observer = nullptr,
                                 bool deferConsumer = false,
                                 TransferWallTimes* times = nullptr);
  // Deferred shared captures have no readable texture until this owner-only
  // operation succeeds. It never calls the producer. reset discards their
  // metadata; transfer retirement still drains any outstanding handoff.
  HRESULT completePending(GpuWorkObserver* observer = nullptr,
                          TransferWallTimes* times = nullptr);
  bool hasPending() const;

  // Borrowed pointers: pixels remain immutable until reset, shutdown, or the
  // next capture of the same eye. The owner supplies once-per-frame ordering;
  // this class deliberately has no frame-order policy. capture only enqueues
  // a copy on the immediate context and does not change pipeline bindings.
  ID3D11Texture2D* texture(vr::EVREye eye) const;
  vr::VRTextureBounds_t bounds(vr::EVREye eye) const;
  vr::EColorSpace colorSpace(vr::EVREye eye) const;
  // Cached by captured resource and color interpretation. Views travel with
  // exchanged buffers and retire with their texture on reset/resize/close.
  // Like capture, this is serialized by the caller (the XR owner in shared
  // mode). The returned COM reference belongs to the caller.
  HRESULT shaderView(vr::EVREye eye, ID3D11ShaderResourceView** output) const;
  uint64_t shaderViewsCreated() const { return shaderViewsCreated_; }

 private:
  vr::EVRCompositorError captureShared(vr::EVREye eye, const vr::Texture_t* texture,
                                       const vr::VRTextureBounds_t* bounds,
                                       vr::EVRSubmitFlags flags, bool copyPixels,
                                       GpuWorkObserver* observer, bool deferConsumer,
                                       TransferWallTimes* times);
  struct Eye {
    Microsoft::WRL::ComPtr<ID3D11Texture2D> copy;
    mutable Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> shaderViews[2];
    vr::VRTextureBounds_t bounds{0.f, 0.f, 1.f, 1.f};
    vr::EColorSpace colorSpace = vr::ColorSpace_Auto;
    bool captured = false;
    bool pending = false;
  } eyes_[2];
  Microsoft::WRL::ComPtr<ID3D11Device> device_;
  Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
  Microsoft::WRL::ComPtr<ID3D11Device> sharedProducer_;
  Microsoft::WRL::ComPtr<ID3D11Device> sharedConsumer_;
  std::unique_ptr<SharedTextureTransfer> sharedTransfers_[2];
  std::thread::id sharedOwner_{};
  ImmediateExecutor* sharedExecutor_ = nullptr;
  bool sharedInitialized_ = false;
  bool initialized_ = false;
  mutable uint64_t shaderViewsCreated_ = 0;
};

} // namespace edvr::openxr
