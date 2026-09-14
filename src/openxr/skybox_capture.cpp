#include "skybox_capture.h"
#include "shared_texture_transfer.h"

#include <exception>
#include <new>
#include <thread>
#include <utility>

using Microsoft::WRL::ComPtr;

namespace edvr::openxr {
namespace {

bool IsSupportedFormat(DXGI_FORMAT format) {
  return format == DXGI_FORMAT_R8G8B8A8_TYPELESS ||
         format == DXGI_FORMAT_R8G8B8A8_UNORM ||
         format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ||
         format == DXGI_FORMAT_B8G8R8A8_TYPELESS ||
         format == DXGI_FORMAT_B8G8R8A8_UNORM ||
         format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
}

DXGI_FORMAT TypelessFamily(DXGI_FORMAT format) {
  if (format == DXGI_FORMAT_R8G8B8A8_TYPELESS ||
      format == DXGI_FORMAT_R8G8B8A8_UNORM ||
      format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) {
    return DXGI_FORMAT_R8G8B8A8_TYPELESS;
  }
  return DXGI_FORMAT_B8G8R8A8_TYPELESS;
}

bool SameDevice(ID3D11Device* expected, ID3D11Texture2D* source) {
  if (!expected || !source) return false;

  ComPtr<ID3D11Device> owner;
  source->GetDevice(&owner);
  ComPtr<IUnknown> expectedIdentity;
  ComPtr<IUnknown> ownerIdentity;
  return owner &&
         SUCCEEDED(expected->QueryInterface(IID_PPV_ARGS(&expectedIdentity))) &&
         SUCCEEDED(owner->QueryInterface(IID_PPV_ARGS(&ownerIdentity))) &&
         expectedIdentity.Get() == ownerIdentity.Get();
}

}  // namespace

SkyboxCapture::SkyboxCapture() = default;
SkyboxCapture::~SkyboxCapture() {
  if (sharedInitialized_) std::terminate();
  shutdown();
}

HRESULT SkyboxCapture::initialize(ID3D11Device* device) {
  if (sharedInitialized_) return E_PENDING;
  shutdown();
  if (!device) return E_INVALIDARG;
  if (FAILED(device->GetDeviceRemovedReason())) return DXGI_ERROR_DEVICE_REMOVED;

  device_ = device;
  device_->GetImmediateContext(&context_);
  if (!context_) {
    shutdown();
    return E_FAIL;
  }
  initialized_ = true;
  return S_OK;
}

HRESULT SkyboxCapture::initializeShared(ID3D11Device* producer,
                                        ID3D11Device* consumer,
                                        ImmediateExecutor* producerExecutor) {
  if (sharedInitialized_) return E_PENDING;
  if (initialized_) shutdown();
  if (!producer || !consumer || !producerExecutor) return E_INVALIDARG;
  try {
    auto first = std::make_unique<SharedTextureTransfer>();
    const HRESULT validated = first->initialize(producer, consumer, producerExecutor);
    if (validated != S_OK) return validated;
    sharedBanks_[0].transfers[0] = std::move(first);
    sharedProducer_ = producer;
    sharedConsumer_ = consumer;
    sharedExecutor_ = producerExecutor;
    sharedOwner_ = std::this_thread::get_id();
    sharedInitialized_ = true;
    return S_OK;
  } catch (...) {
    sharedProducer_.Reset();
    sharedConsumer_.Reset();
    sharedExecutor_ = nullptr;
    sharedOwner_ = {};
    return E_OUTOFMEMORY;
  }
}

void SkyboxCapture::clear() {
  for (Face& face : faces_) {
    face.copy.Reset();
    face.color = vr::ColorSpace_Auto;
    face.captured = false;
  }
  complete_ = false;
  if (sharedInitialized_) committedBank_ = -1;
}

void SkyboxCapture::shutdown() {
  if (sharedInitialized_) std::terminate();
  clear();
  context_.Reset();
  device_.Reset();
  initialized_ = false;
}

HRESULT SkyboxCapture::shutdownShared(DWORD timeoutMs) {
  if (!sharedInitialized_) return S_OK;
  if (sharedOwner_ != std::this_thread::get_id()) return E_ACCESSDENIED;
  if (timeoutMs == INFINITE) return E_INVALIDARG;
  HRESULT result = S_OK;
  for (auto& bank : sharedBanks_) {
    for (auto& transfer : bank.transfers) {
      if (!transfer) continue;
      const HRESULT current = transfer->shutdown(timeoutMs);
      if (current != S_OK && result == S_OK) result = current;
    }
  }
  if (result != S_OK) return result;
  clear();
  for (auto& bank : sharedBanks_) {
    for (auto& transfer : bank.transfers) transfer.reset();
  }
  sharedProducer_.Reset();
  sharedConsumer_.Reset();
  sharedExecutor_ = nullptr;
  sharedOwner_ = {};
  committedBank_ = -1;
  sharedInitialized_ = false;
  return S_OK;
}

vr::EVRCompositorError SkyboxCapture::setShared(const vr::Texture_t* textures,
                                                uint32_t count) {
  if (!sharedInitialized_ || sharedOwner_ != std::this_thread::get_id() ||
      !sharedProducer_ || !sharedConsumer_ || !sharedExecutor_ ||
      !textures || count != 6)
    return vr::VRCompositorError_InvalidTexture;
  if (FAILED(sharedProducer_->GetDeviceRemovedReason()))
    return vr::VRCompositorError_InvalidTexture;

  ComPtr<ID3D11Texture2D> sources[6];
  D3D11_TEXTURE2D_DESC descriptions[6]{};
  for (unsigned face = 0; face < 6; ++face) {
    const vr::Texture_t& texture = textures[face];
    if (!texture.handle || texture.eType != vr::API_DirectX ||
        texture.eColorSpace < vr::ColorSpace_Auto ||
        texture.eColorSpace > vr::ColorSpace_Linear)
      return vr::VRCompositorError_InvalidTexture;
    if (FAILED(static_cast<IUnknown*>(texture.handle)->QueryInterface(
            IID_PPV_ARGS(&sources[face]))))
      return vr::VRCompositorError_InvalidTexture;
    if (!SameDevice(sharedProducer_.Get(), sources[face].Get()))
      return vr::VRCompositorError_TextureIsOnWrongDevice;
    sources[face]->GetDesc(&descriptions[face]);
    const D3D11_TEXTURE2D_DESC& desc = descriptions[face];
    if (!desc.Width || !desc.Height || desc.ArraySize != 1 ||
        desc.MipLevels != 1 || desc.SampleDesc.Count != 1 ||
        desc.SampleDesc.Quality != 0 || desc.Usage != D3D11_USAGE_DEFAULT ||
        desc.CPUAccessFlags != 0)
      return vr::VRCompositorError_InvalidTexture;
    if (!IsSupportedFormat(desc.Format))
      return vr::VRCompositorError_TextureUsesUnsupportedFormat;
  }

  const int inactive = committedBank_ == 0 ? 1 : 0;
  auto& bank = sharedBanks_[inactive];
  ComPtr<ID3D11Texture2D> outputs[6];
  try {
    for (unsigned face = 0; face < 6; ++face) {
      auto& transfer = bank.transfers[face];
      if (!transfer) transfer = std::make_unique<SharedTextureTransfer>();
      if (!transfer->ready() && !transfer->pending() && !transfer->faulted()) {
        const HRESULT initialized = transfer->initialize(
            sharedProducer_.Get(), sharedConsumer_.Get(), sharedExecutor_);
        if (initialized != S_OK) return vr::VRCompositorError_InvalidTexture;
      }
      if (transfer->pending()) {
        ID3D11Texture2D* pendingOutput = nullptr;
        if (transfer->receive(pendingOutput) != S_OK)
          return vr::VRCompositorError_InvalidTexture;
      }
      if (transfer->faulted()) return vr::VRCompositorError_InvalidTexture;
      ID3D11Texture2D* output = nullptr;
      if (transfer->copy(sources[face].Get(), output) != S_OK || !output)
        return vr::VRCompositorError_InvalidTexture;
      outputs[face] = output;
    }
  } catch (...) {
    return vr::VRCompositorError_InvalidTexture;
  }

  // Only this final fixed-size commit changes the published all-six set.
  for (unsigned face = 0; face < 6; ++face) {
    faces_[face].copy = outputs[face];
    faces_[face].color = textures[face].eColorSpace;
    faces_[face].captured = true;
  }
  committedBank_ = inactive;
  complete_ = true;
  return vr::VRCompositorError_None;
}

vr::EVRCompositorError SkyboxCapture::set(const vr::Texture_t* textures,
                                          uint32_t count) {
  if (sharedInitialized_) return setShared(textures, count);
  if (!initialized_ || !device_ || !context_ || !textures || count != 6) {
    return vr::VRCompositorError_InvalidTexture;
  }
  if (FAILED(device_->GetDeviceRemovedReason())) {
    return vr::VRCompositorError_InvalidTexture;
  }

  ComPtr<ID3D11Texture2D> sources[6];
  D3D11_TEXTURE2D_DESC descriptions[6]{};
  ComPtr<ID3D11Texture2D> fresh[6];

  // Validate every face before changing any device or published state.
  for (unsigned face = 0; face < 6; ++face) {
    const vr::Texture_t& texture = textures[face];
    if (!texture.handle || texture.eType != vr::API_DirectX ||
        texture.eColorSpace < vr::ColorSpace_Auto ||
        texture.eColorSpace > vr::ColorSpace_Linear) {
      return vr::VRCompositorError_InvalidTexture;
    }

    HRESULT query = static_cast<IUnknown*>(texture.handle)->QueryInterface(
        IID_PPV_ARGS(&sources[face]));
    if (FAILED(query)) return vr::VRCompositorError_InvalidTexture;
    if (!SameDevice(device_.Get(), sources[face].Get())) {
      return vr::VRCompositorError_TextureIsOnWrongDevice;
    }

    sources[face]->GetDesc(&descriptions[face]);
    const D3D11_TEXTURE2D_DESC& desc = descriptions[face];
    if (!desc.Width || !desc.Height || desc.ArraySize != 1 ||
        desc.MipLevels != 1 || desc.SampleDesc.Count != 1 ||
        desc.SampleDesc.Quality != 0) {
      return vr::VRCompositorError_InvalidTexture;
    }
    if (!IsSupportedFormat(desc.Format)) {
      return vr::VRCompositorError_TextureUsesUnsupportedFormat;
    }
  }

  // Allocate all six private textures before enqueuing any copy.
  for (unsigned face = 0; face < 6; ++face) {
    D3D11_TEXTURE2D_DESC desc = descriptions[face];
    desc.Format = TypelessFamily(desc.Format);
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    desc.CPUAccessFlags = 0;
    desc.MiscFlags = 0;
    if (FAILED(device_->CreateTexture2D(&desc, nullptr, &fresh[face]))) {
      return vr::VRCompositorError_InvalidTexture;
    }
  }

  for (unsigned face = 0; face < 6; ++face) {
    context_->CopyResource(fresh[face].Get(), sources[face].Get());
  }
  context_->Flush();
  if (FAILED(device_->GetDeviceRemovedReason())) {
    return vr::VRCompositorError_InvalidTexture;
  }

  for (unsigned face = 0; face < 6; ++face) {
    faces_[face].copy = std::move(fresh[face]);
    faces_[face].color = textures[face].eColorSpace;
    faces_[face].captured = true;
  }
  complete_ = true;
  return vr::VRCompositorError_None;
}

ID3D11Texture2D* SkyboxCapture::texture(unsigned face) const {
  return face < 6 && complete_ && faces_[face].captured
             ? faces_[face].copy.Get()
             : nullptr;
}

vr::EColorSpace SkyboxCapture::colorSpace(unsigned face) const {
  return face < 6 && complete_ && faces_[face].captured
             ? faces_[face].color
             : vr::ColorSpace_Auto;
}

}  // namespace edvr::openxr
