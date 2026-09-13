#include "skybox_capture.h"

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

SkyboxCapture::~SkyboxCapture() { shutdown(); }

HRESULT SkyboxCapture::initialize(ID3D11Device* device) {
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

void SkyboxCapture::clear() {
  for (Face& face : faces_) {
    face.copy.Reset();
    face.color = vr::ColorSpace_Auto;
    face.captured = false;
  }
  complete_ = false;
}

void SkyboxCapture::shutdown() {
  clear();
  context_.Reset();
  device_.Reset();
  initialized_ = false;
}

vr::EVRCompositorError SkyboxCapture::set(const vr::Texture_t* textures,
                                          uint32_t count) {
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
