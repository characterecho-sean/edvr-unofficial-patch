#pragma once
#include <d3d11.h>
#include <memory>
#include <thread>
#include <wrl/client.h>

#include "../openvr/compat/openvr_v0_9_20.h"

namespace edvr::openxr { class ImmediateExecutor; class SharedTextureTransfer; }

namespace edvr::openxr {

class SkyboxCapture final {
 public:
  SkyboxCapture();
  ~SkyboxCapture();
  SkyboxCapture(const SkyboxCapture&) = delete;
  SkyboxCapture& operator=(const SkyboxCapture&) = delete;

  // Retains this device and its immediate context. The caller serializes all
  // operations with other context users, including destruction. No game
  // pipeline bindings are modified; set enqueues and flushes copy commands.
  HRESULT initialize(ID3D11Device* device);
  HRESULT initializeShared(ID3D11Device* producer, ID3D11Device* consumer,
                           ImmediateExecutor* producerExecutor);
  // Supports six independent D3D11 2D faces in Front, Back, Left, Right,
  // Top, Bottom order. Counts 1/2 (lat-long) remain unavailable. All sources
  // must stay valid until return. A rejected update keeps the prior set.
  vr::EVRCompositorError set(const vr::Texture_t* textures, uint32_t count);
  void clear();
  void shutdown();
  HRESULT shutdownShared(DWORD timeoutMs = 5000);
  bool ready() const {
    return sharedInitialized_ ? (complete_ && sharedOwner_ == std::this_thread::get_id())
                              : (initialized_ && complete_);
  }
  // Borrowed private copy, valid until successful set, clear or shutdown.
  ID3D11Texture2D* texture(unsigned face) const;
  vr::EColorSpace colorSpace(unsigned face) const;

 private:
  struct Face {
    Microsoft::WRL::ComPtr<ID3D11Texture2D> copy;
    vr::EColorSpace color = vr::ColorSpace_Auto;
    bool captured = false;
  };

  struct SharedBank {
    std::unique_ptr<SharedTextureTransfer> transfers[6];
  };

  vr::EVRCompositorError setShared(const vr::Texture_t* textures, uint32_t count);

  Face faces_[6];
  Microsoft::WRL::ComPtr<ID3D11Device> device_;
  Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
  bool initialized_ = false;
  bool complete_ = false;
  SharedBank sharedBanks_[2];
  Microsoft::WRL::ComPtr<ID3D11Device> sharedProducer_;
  Microsoft::WRL::ComPtr<ID3D11Device> sharedConsumer_;
  std::thread::id sharedOwner_{};
  ImmediateExecutor* sharedExecutor_ = nullptr;
  int committedBank_ = -1;
  bool sharedInitialized_ = false;
};

}  // namespace edvr::openxr
