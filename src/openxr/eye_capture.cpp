#include "eye_capture.h"
#include "shared_texture_transfer.h"
#include "gpu_work_observer.h"
#include <cmath>
#include <exception>
#include <new>
#include <thread>
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

EyeCapture::EyeCapture() = default;
EyeCapture::~EyeCapture() {
  if (sharedInitialized_) std::terminate();
  shutdown();
}

HRESULT EyeCapture::initialize(ID3D11Device* device) {
  if (sharedInitialized_) return E_PENDING;
  shutdown();
  if (!device) return E_INVALIDARG;
  if (FAILED(device->GetDeviceRemovedReason())) return DXGI_ERROR_DEVICE_REMOVED;
  device_ = device;
  device_->GetImmediateContext(&context_);
  if (!context_) { shutdown(); return E_FAIL; }
  initialized_ = true;
  return S_OK;
}

HRESULT EyeCapture::initializeShared(ID3D11Device* producer, ID3D11Device* consumer,
                                     ImmediateExecutor* producerExecutor) {
  if (sharedInitialized_) return E_PENDING;
  if (initialized_) shutdown();
  if (!producer || !consumer || !producerExecutor) return E_INVALIDARG;
  try {
    // Validate ownership and the producer executor before publishing shared
    // mode. Pixel resources remain lazy until the first accepted texture.
    auto first = std::make_unique<SharedTextureTransfer>();
    const HRESULT validated = first->initialize(producer, consumer, producerExecutor);
    if (validated != S_OK) return validated;
    sharedTransfers_[0] = std::move(first);
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

void EyeCapture::reset() {
  for (auto& e : eyes_) {
    for (auto& view : e.shaderViews) view.Reset();
    e.copy.Reset();
    e.bounds = {0.f, 0.f, 1.f, 1.f};
    e.colorSpace = vr::ColorSpace_Auto;
    e.captured = false;
  }
}

bool EyeCapture::exchangeBuffers(EyeCapture& other) {
  if (this == &other || initialized_ != other.initialized_ ||
      sharedInitialized_ != other.sharedInitialized_) return false;
  if (sharedInitialized_) {
    if (sharedOwner_ != std::this_thread::get_id() || sharedOwner_ != other.sharedOwner_ ||
        sharedProducer_ != other.sharedProducer_ || sharedConsumer_ != other.sharedConsumer_ ||
        sharedExecutor_ != other.sharedExecutor_) return false;
  } else if (!initialized_ || device_ != other.device_) return false;
  for (unsigned eye = 0; eye < 2; ++eye) {
    std::swap(eyes_[eye], other.eyes_[eye]);
    std::swap(sharedTransfers_[eye], other.sharedTransfers_[eye]);
  }
  return true;
}

void EyeCapture::shutdown() {
  if (sharedInitialized_) std::terminate();
  reset();
  context_.Reset();
  device_.Reset();
  initialized_ = false;
}

HRESULT EyeCapture::shutdownShared(DWORD timeoutMs) {
  if (!sharedInitialized_) return S_OK;
  if (sharedOwner_ != std::this_thread::get_id()) return E_ACCESSDENIED;
  if (timeoutMs == INFINITE) return E_INVALIDARG;
  HRESULT result = S_OK;
  for (auto& transfer : sharedTransfers_) {
    if (!transfer) continue;
    const HRESULT current = transfer->shutdown(timeoutMs);
    if (current != S_OK && result == S_OK) result = current;
  }
  if (result != S_OK) return result;
  reset();
  for (auto& transfer : sharedTransfers_) transfer.reset();
  sharedProducer_.Reset();
  sharedConsumer_.Reset();
  sharedExecutor_ = nullptr;
  sharedOwner_ = {};
  sharedInitialized_ = false;
  return S_OK;
}

vr::EVRCompositorError EyeCapture::captureShared(
    vr::EVREye eye, const vr::Texture_t* texture,
    const vr::VRTextureBounds_t* bounds, vr::EVRSubmitFlags flags,
    bool copyPixels, GpuWorkObserver* observer) {
  if (!sharedInitialized_ || sharedOwner_ != std::this_thread::get_id() ||
      !sharedProducer_ || !sharedConsumer_ || !sharedExecutor_)
    return vr::VRCompositorError_InvalidTexture;
  if (!validEye(eye) || !texture || !texture->handle || flags != vr::Submit_Default ||
      texture->eType != vr::API_DirectX ||
      texture->eColorSpace < vr::ColorSpace_Auto ||
      texture->eColorSpace > vr::ColorSpace_Linear)
    return validEye(eye) ? vr::VRCompositorError_InvalidTexture
                         : vr::VRCompositorError_IndexOutOfRange;
  const vr::VRTextureBounds_t b = bounds ? *bounds :
      vr::VRTextureBounds_t{0.f, 0.f, 1.f, 1.f};
  if (!validBounds(b)) return vr::VRCompositorError_InvalidTexture;
  if (FAILED(sharedProducer_->GetDeviceRemovedReason()))
    return vr::VRCompositorError_InvalidTexture;
  ComPtr<ID3D11Texture2D> source;
  if (FAILED(static_cast<IUnknown*>(texture->handle)->QueryInterface(
          IID_PPV_ARGS(&source))))
    return vr::VRCompositorError_InvalidTexture;
  if (!sameDevice(sharedProducer_.Get(), source.Get()))
    return vr::VRCompositorError_TextureIsOnWrongDevice;
  D3D11_TEXTURE2D_DESC desc{};
  source->GetDesc(&desc);
  if (!desc.Width || !desc.Height || desc.ArraySize != 1 || desc.MipLevels != 1 ||
      desc.SampleDesc.Count != 1 || desc.SampleDesc.Quality != 0 ||
      desc.Usage != D3D11_USAGE_DEFAULT || desc.CPUAccessFlags != 0)
    return vr::VRCompositorError_InvalidTexture;
  if (!supportedFormat(desc.Format))
    return vr::VRCompositorError_TextureUsesUnsupportedFormat;
  if (!copyPixels) return vr::VRCompositorError_None;

  const unsigned index = eyeIndex(eye);
  try {
    if (!sharedTransfers_[index])
      sharedTransfers_[index] = std::make_unique<SharedTextureTransfer>();
    auto& transfer = sharedTransfers_[index];
    if (!transfer->ready() && !transfer->pending() && !transfer->faulted()) {
      const HRESULT initialized = transfer->initialize(
          sharedProducer_.Get(), sharedConsumer_.Get(), sharedExecutor_);
      if (initialized != S_OK) return vr::VRCompositorError_InvalidTexture;
    }
    if (transfer->pending()) {
      ID3D11Texture2D* previous = nullptr;
      if (transfer->receive(previous, 100) != S_OK)
        return vr::VRCompositorError_InvalidTexture;
    }
    ID3D11Texture2D* output = nullptr;
    const HRESULT copied = transfer->copy(source.Get(), output, 100, observer, index);
    if (copied != S_OK || !output) return vr::VRCompositorError_InvalidTexture;
    if (eyes_[index].copy.Get() != output)
      for (auto& view : eyes_[index].shaderViews) view.Reset();
    eyes_[index].copy = output;
    eyes_[index].bounds = b;
    eyes_[index].colorSpace = texture->eColorSpace;
    eyes_[index].captured = true;
    return vr::VRCompositorError_None;
  } catch (...) {
    return vr::VRCompositorError_InvalidTexture;
  }
}

vr::EVRCompositorError EyeCapture::capture(vr::EVREye eye, const vr::Texture_t* texture,
                                            const vr::VRTextureBounds_t* bounds,
                                            vr::EVRSubmitFlags flags, bool copyPixels,
                                            GpuWorkObserver* observer) {
  if (sharedInitialized_) return captureShared(eye, texture, bounds, flags, copyPixels, observer);
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
  if (!reuse) {
    for (auto& view : dst.shaderViews) view.Reset();
    dst.copy = std::move(fresh);
  }
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

HRESULT EyeCapture::shaderView(vr::EVREye eye, ID3D11ShaderResourceView** output) const {
  if (!output) return E_POINTER;
  *output = nullptr;
  if (sharedInitialized_ && sharedOwner_ != std::this_thread::get_id()) return E_ACCESSDENIED;
  if (!texture(eye)) return E_INVALIDARG;
  const auto& e = eyes_[eyeIndex(eye)];
  if (e.colorSpace != vr::ColorSpace_Auto && e.colorSpace != vr::ColorSpace_Gamma &&
      e.colorSpace != vr::ColorSpace_Linear) return E_INVALIDARG;
  const bool gamma = e.colorSpace != vr::ColorSpace_Linear;
  auto& view = e.shaderViews[gamma ? 1 : 0];
  if (!view) {
    D3D11_TEXTURE2D_DESC td{}; e.copy->GetDesc(&td);
    D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
    if (td.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS)
      sd.Format = gamma ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM;
    else if (td.Format == DXGI_FORMAT_B8G8R8A8_TYPELESS)
      sd.Format = gamma ? DXGI_FORMAT_B8G8R8A8_UNORM_SRGB : DXGI_FORMAT_B8G8R8A8_UNORM;
    else return E_INVALIDARG;
    sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; sd.Texture2D.MipLevels = 1;
    ComPtr<ID3D11Device> device; e.copy->GetDevice(&device);
    const HRESULT result = device->CreateShaderResourceView(e.copy.Get(), &sd, &view);
    if (FAILED(result)) return result;
    ++shaderViewsCreated_;
  }
  return view.CopyTo(output);
}
} // namespace edvr::openxr
