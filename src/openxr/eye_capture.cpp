#include "eye_capture.h"
#include <cmath>
#include <utility>

namespace edvr::openxr {
namespace {
using Microsoft::WRL::ComPtr;

bool validEye(vr::EVREye e) { return e == vr::Eye_Left || e == vr::Eye_Right; }
unsigned eyeIndex(vr::EVREye e) { return static_cast<unsigned>(e); }

bool sameDevice(ID3D11Device* expected, ID3D11Texture2D* source) {
  if (!expected || !source) return false;
  ComPtr<ID3D11Device> owner;
  source->GetDevice(&owner);
  if (!owner) return false;
  ComPtr<IUnknown> a, b;
  return SUCCEEDED(expected->QueryInterface(IID_PPV_ARGS(&a))) &&
         SUCCEEDED(owner->QueryInterface(IID_PPV_ARGS(&b))) && a.Get() == b.Get();
}

bool supportedFormat(DXGI_FORMAT f) {
  return f == DXGI_FORMAT_R8G8B8A8_TYPELESS || f == DXGI_FORMAT_R8G8B8A8_UNORM ||
         f == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || f == DXGI_FORMAT_B8G8R8A8_TYPELESS ||
         f == DXGI_FORMAT_B8G8R8A8_UNORM || f == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
}

DXGI_FORMAT typelessFamily(DXGI_FORMAT f) {
  switch (f) {
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_TYPELESS;
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_TYPELESS;
    default: return DXGI_FORMAT_UNKNOWN;
  }
}

bool validBounds(const vr::VRTextureBounds_t& b) {
  const float v[] = {b.uMin, b.vMin, b.uMax, b.vMax};
  for (float x : v) if (!std::isfinite(x) || x < 0.f || x > 1.f) return false;
  return b.uMin != b.uMax && b.vMin != b.vMax;
}

}

EyeCapture::~EyeCapture() { shutdown(); }

HRESULT EyeCapture::initialize(ID3D11Device* device) {
  shutdown();
  if (!device) return E_INVALIDARG;
  if (FAILED(device->GetDeviceRemovedReason())) return DXGI_ERROR_DEVICE_REMOVED;
  device_ = device;
  device_->GetImmediateContext(&context_);
  if (!context_) { shutdown(); return E_FAIL; }
  initialized_ = true;
  return S_OK;
}

void EyeCapture::reset() {
  for (auto& e : eyes_) {
    e.copy.Reset();
    e.bounds = {0.f, 0.f, 1.f, 1.f};
    e.colorSpace = vr::ColorSpace_Auto;
    e.captured = false;
  }
}

void EyeCapture::shutdown() {
  reset();
  context_.Reset();
  device_.Reset();
  initialized_ = false;
}

vr::EVRCompositorError EyeCapture::capture(vr::EVREye eye, const vr::Texture_t* texture,
                                            const vr::VRTextureBounds_t* bounds,
                                            vr::EVRSubmitFlags flags, bool copyPixels) {
  if (!initialized_ || !device_ || !context_) return vr::VRCompositorError_InvalidTexture;
  if (!validEye(eye) || !texture || !texture->handle || flags != vr::Submit_Default ||
      texture->eType != vr::API_DirectX || texture->eColorSpace < vr::ColorSpace_Auto ||
      texture->eColorSpace > vr::ColorSpace_Linear)
    return validEye(eye) ? vr::VRCompositorError_InvalidTexture : vr::VRCompositorError_IndexOutOfRange;

  vr::VRTextureBounds_t b = bounds ? *bounds : vr::VRTextureBounds_t{0.f, 0.f, 1.f, 1.f};
  if (!validBounds(b)) return vr::VRCompositorError_InvalidTexture;
  if (FAILED(device_->GetDeviceRemovedReason())) return vr::VRCompositorError_InvalidTexture;

  // OpenVR supplies a live COM object. Query its type before using texture
  // vtable slots; a buffer or another resource is not a Texture2D.
  ComPtr<ID3D11Texture2D> source;
  if (FAILED(static_cast<IUnknown*>(texture->handle)->QueryInterface(IID_PPV_ARGS(&source))))
    return vr::VRCompositorError_InvalidTexture;
  if (!sameDevice(device_.Get(), source.Get())) return vr::VRCompositorError_TextureIsOnWrongDevice;
  D3D11_TEXTURE2D_DESC sd{};
  source->GetDesc(&sd);
  if (!sd.Width || !sd.Height || sd.ArraySize != 1 || sd.MipLevels != 1 ||
      sd.SampleDesc.Count != 1 || sd.SampleDesc.Quality != 0)
    return vr::VRCompositorError_InvalidTexture;
  if (!supportedFormat(sd.Format)) return vr::VRCompositorError_TextureUsesUnsupportedFormat;

  // A no-render submission still proves that the source is acceptable, but
  // intentionally leaves the previously accepted eye untouched.
  if (!copyPixels) return vr::VRCompositorError_None;

  const unsigned i = eyeIndex(eye);
  auto& dst = eyes_[i];
  D3D11_TEXTURE2D_DESC td = sd;
  td.Format = typelessFamily(sd.Format);
  td.Usage = D3D11_USAGE_DEFAULT;
  td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  td.CPUAccessFlags = 0;
  td.MiscFlags = 0;
  bool reuse = false;
  if (dst.copy) {
    D3D11_TEXTURE2D_DESC old{}; dst.copy->GetDesc(&old);
    reuse = old.Width == td.Width && old.Height == td.Height && old.Format == td.Format && dst.copy.Get() != source.Get();
  }
  ComPtr<ID3D11Texture2D> fresh;
  if (!reuse) {
    if (FAILED(device_->CreateTexture2D(&td, nullptr, &fresh)))
      return vr::VRCompositorError_InvalidTexture;
  }
  ID3D11Texture2D* target = reuse ? dst.copy.Get() : fresh.Get();
  context_->CopyResource(target, source.Get());
  if (FAILED(device_->GetDeviceRemovedReason())) return vr::VRCompositorError_InvalidTexture;
  if (!reuse) dst.copy = std::move(fresh);
  dst.bounds = b;
  dst.colorSpace = texture->eColorSpace;
  dst.captured = true;
  return vr::VRCompositorError_None;
}

ID3D11Texture2D* EyeCapture::texture(vr::EVREye eye) const {
  return validEye(eye) && eyes_[eyeIndex(eye)].captured ? eyes_[eyeIndex(eye)].copy.Get() : nullptr;
}

vr::VRTextureBounds_t EyeCapture::bounds(vr::EVREye eye) const {
  return validEye(eye) && eyes_[eyeIndex(eye)].captured ? eyes_[eyeIndex(eye)].bounds : vr::VRTextureBounds_t{0.f, 0.f, 1.f, 1.f};
}

vr::EColorSpace EyeCapture::colorSpace(vr::EVREye eye) const {
  return validEye(eye) && eyes_[eyeIndex(eye)].captured ? eyes_[eyeIndex(eye)].colorSpace : vr::ColorSpace_Auto;
}
} // namespace edvr::openxr
