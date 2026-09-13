#include "shared_texture_transfer.h"

#include "immediate_executor.h"
#include <d3d11_1.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <exception>
#include <thread>
#include <utility>

namespace edvr::openxr {
namespace {

using Microsoft::WRL::ComPtr;

enum class PixelFamily { RGBA8, BGRA8 };

struct FormatChoice {
  PixelFamily family;
  DXGI_FORMAT shared;
  DXGI_FORMAT privateFormat;
};

bool formatChoice(DXGI_FORMAT format, FormatChoice& choice) noexcept {
  switch (format) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
      choice = {PixelFamily::RGBA8, DXGI_FORMAT_R8G8B8A8_UNORM,
                DXGI_FORMAT_R8G8B8A8_TYPELESS};
      return true;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
      choice = {PixelFamily::BGRA8, DXGI_FORMAT_B8G8R8A8_UNORM,
                DXGI_FORMAT_B8G8R8A8_TYPELESS};
      return true;
    default:
      return false;
  }
}

bool sameLuid(const LUID& a, const LUID& b) noexcept {
  return a.HighPart == b.HighPart && a.LowPart == b.LowPart;
}

bool sameDevice(ID3D11Device* first, ID3D11Device* second) noexcept {
  if (!first || !second) return false;
  ComPtr<IUnknown> firstIdentity, secondIdentity;
  if (FAILED(first->QueryInterface(IID_PPV_ARGS(&firstIdentity))) ||
      FAILED(second->QueryInterface(IID_PPV_ARGS(&secondIdentity)))) return false;
  return firstIdentity.Get() == secondIdentity.Get();
}

bool adapterLuid(ID3D11Device* device, LUID& luid) noexcept {
  ComPtr<IDXGIDevice> dxgiDevice;
  ComPtr<IDXGIAdapter> adapter;
  DXGI_ADAPTER_DESC description{};
  if (!device || FAILED(device->QueryInterface(IID_PPV_ARGS(&dxgiDevice))) ||
      FAILED(dxgiDevice->GetAdapter(&adapter)) || !adapter ||
      FAILED(adapter->GetDesc(&description))) return false;
  luid = description.AdapterLuid;
  return true;
}

bool validSource(const D3D11_TEXTURE2D_DESC& description) noexcept {
  return description.Width != 0 && description.Height != 0 &&
      description.Width <= D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION &&
      description.Height <= D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION &&
      description.MipLevels == 1 && description.ArraySize == 1 &&
      description.SampleDesc.Count == 1 && description.SampleDesc.Quality == 0 &&
      description.Usage == D3D11_USAGE_DEFAULT && description.CPUAccessFlags == 0;
}

HRESULT timeoutResult() noexcept {
  return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
}

}  // namespace

struct SharedTextureTransfer::Impl final {
  enum class Stage { None, ConsumerAcquire };

  explicit Impl() : owner(std::this_thread::get_id()) {}

  bool onOwner() const noexcept {
    return owner == std::this_thread::get_id();
  }

  bool hasResources() const noexcept {
    return sharedProducer && sharedConsumer && privateTexture &&
        producerMutex && consumerMutex && consumerEvent;
  }

  void clearResources() noexcept {
    consumerEvent.Reset();
    consumerMutex.Reset();
    producerMutex.Reset();
    privateTexture.Reset();
    sharedConsumer.Reset();
    sharedProducer.Reset();
    sourceWidth = sourceHeight = 0;
    sourceFamily = PixelFamily::RGBA8;
  }

  HRESULT validateInitialize(ID3D11Device* producerDevice,
                             ID3D11Device* consumerDevice,
                             ImmediateExecutor* executor) noexcept {
    if (!onOwner() || initialized || pendingStage != Stage::None || faulted ||
        !producerDevice || !consumerDevice || !executor ||
        sameDevice(producerDevice, consumerDevice) ||
        (producerDevice->GetCreationFlags() & D3D11_CREATE_DEVICE_SINGLETHREADED) ||
        (consumerDevice->GetCreationFlags() & D3D11_CREATE_DEVICE_SINGLETHREADED))
      return E_INVALIDARG;

    LUID consumerLuid{};
    if (!adapterLuid(consumerDevice, consumerLuid)) return E_NOINTERFACE;

    ComPtr<ID3D11DeviceContext> producerImmediate;
    LUID producerLuid{};
    bool producerValid = false;
    bool invoked = false;
    try {
      invoked = executor->invoke([&] {
        producerValid = adapterLuid(producerDevice, producerLuid);
        producerDevice->GetImmediateContext(&producerImmediate);
        producerValid = producerValid && producerImmediate &&
            sameLuid(producerLuid, consumerLuid);
      });
    } catch (...) {}
    if (!invoked || !producerValid) return invoked ? E_INVALIDARG : E_ABORT;

    ComPtr<ID3D11DeviceContext> consumerImmediate;
    consumerDevice->GetImmediateContext(&consumerImmediate);
    if (!consumerImmediate) return E_NOINTERFACE;

    ComPtr<ID3D11Device1> consumer1;
    if (FAILED(consumerDevice->QueryInterface(IID_PPV_ARGS(&consumer1))) || !consumer1)
      return E_NOINTERFACE;

    producer = producerDevice;
    consumer = consumerDevice;
    producerContext = std::move(producerImmediate);
    consumerContext = std::move(consumerImmediate);
    producerExecutor = executor;
    consumerDevice1 = std::move(consumer1);
    initialized = true;
    return S_OK;
  }

  HRESULT createResources(const D3D11_TEXTURE2D_DESC& sourceDescription,
                          const FormatChoice& choice, DWORD timeoutMs) noexcept {
    if (hasResources() && sourceWidth == sourceDescription.Width &&
        sourceHeight == sourceDescription.Height && sourceFamily == choice.family)
      return S_OK;

    if (hasResources()) {
      const HRESULT drained = drainConsumer(timeoutMs);
      if (drained != S_OK) return drained;
    }

    D3D11_TEXTURE2D_DESC sharedDescription = sourceDescription;
    sharedDescription.Format = choice.shared;
    sharedDescription.Usage = D3D11_USAGE_DEFAULT;
    // These resources are copy destinations, never render targets. Keeping
    // the RTV flag off also excludes the producer allocation from the game's
    // FSS/surface-size hooks, which otherwise rewrite matching RT dimensions.
    sharedDescription.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    sharedDescription.CPUAccessFlags = 0;
    sharedDescription.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE |
        D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
    D3D11_TEXTURE2D_DESC privateDescription = sharedDescription;
    privateDescription.Format = choice.privateFormat;
    privateDescription.MiscFlags = 0;

    ComPtr<ID3D11Texture2D> nextSharedProducer;
    ComPtr<IDXGIKeyedMutex> nextProducerMutex;
    HANDLE nextHandle = nullptr;
    HRESULT producerResult = E_FAIL;
    bool callbackRan = false;
    try {
      (void)producerExecutor->invoke([&] {
        callbackRan = true;
        producerResult = producer->CreateTexture2D(&sharedDescription, nullptr,
                                                   &nextSharedProducer);
        if (producerResult != S_OK) return;
        producerResult = nextSharedProducer.As(&nextProducerMutex);
        if (producerResult != S_OK) return;
        ComPtr<IDXGIResource1> resource;
        producerResult = nextSharedProducer.As(&resource);
        if (producerResult != S_OK) return;
        producerResult = resource->CreateSharedHandle(nullptr,
            DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
            nullptr, &nextHandle);
      });
    } catch (...) {}
    if (!callbackRan) {
      if (nextHandle) CloseHandle(nextHandle);
      return E_ABORT;
    }
    if (producerResult != S_OK) {
      if (nextHandle) CloseHandle(nextHandle);
      return producerResult;
    }

    ComPtr<ID3D11Texture2D> nextSharedConsumer;
    ComPtr<IDXGIKeyedMutex> nextConsumerMutex;
    ComPtr<ID3D11Texture2D> nextPrivate;
    ComPtr<ID3D11Query> nextEvent;
    HRESULT result = consumerDevice1->OpenSharedResource1(
        nextHandle, IID_PPV_ARGS(&nextSharedConsumer));
    CloseHandle(nextHandle);
    nextHandle = nullptr;
    if (result == S_OK && nextSharedConsumer)
      result = nextSharedConsumer.As(&nextConsumerMutex);
    if (result == S_OK && nextConsumerMutex)
      result = consumer->CreateTexture2D(&privateDescription, nullptr, &nextPrivate);
    if (result == S_OK && nextPrivate) {
      const D3D11_QUERY_DESC queryDescription{D3D11_QUERY_EVENT, 0};
      result = consumer->CreateQuery(&queryDescription, &nextEvent);
    }
    if (result != S_OK || !nextSharedConsumer || !nextConsumerMutex ||
        !nextPrivate || !nextEvent) {
      if (nextHandle) CloseHandle(nextHandle);
      return result == S_OK ? E_FAIL : result;
    }

    // No handoff is pending here, so replacing a resized/reformatted slot
    // cannot strand a producer mutex. Old COM resources are released only
    // after the prior transfer has completed.
    clearResources();
    sharedProducer = std::move(nextSharedProducer);
    sharedConsumer = std::move(nextSharedConsumer);
    privateTexture = std::move(nextPrivate);
    producerMutex = std::move(nextProducerMutex);
    consumerMutex = std::move(nextConsumerMutex);
    consumerEvent = std::move(nextEvent);
    sourceWidth = sourceDescription.Width;
    sourceHeight = sourceDescription.Height;
    sourceFamily = choice.family;
    return S_OK;
  }

  HRESULT publish(ID3D11Texture2D*& output) noexcept {
    if (!privateTexture) return E_FAIL;
    output = privateTexture.Get();
    return S_OK;
  }

  HRESULT producerCopy(ID3D11Texture2D* source, DWORD timeoutMs) noexcept {
    if (!source || !hasResources()) return E_INVALIDARG;
    HRESULT mutexResult = E_FAIL;
    bool callbackRan = false;
    bool callbackCompleted = false;
    try {
      (void)producerExecutor->invoke([&] {
        callbackRan = true;
        mutexResult = producerMutex->AcquireSync(0, timeoutMs);
        if (mutexResult != S_OK) { callbackCompleted = true; return; }
        producerContext->CopyResource(sharedProducer.Get(), source);
        producerContext->Flush();
        const HRESULT deviceResult = producer->GetDeviceRemovedReason();
        const HRESULT releaseResult = producerMutex->ReleaseSync(1);
        mutexResult = deviceResult == S_OK ? releaseResult : deviceResult;
        callbackCompleted = true;
      });
    } catch (...) {}
    if (!callbackRan) return E_ABORT;
    if (!callbackCompleted) { faulted = true; return E_FAIL; }
    if (mutexResult == WAIT_TIMEOUT) {
      pendingStage = Stage::None;
      return mutexResult;
    }
    if (mutexResult != S_OK) {
      faulted = true;
      return mutexResult;
    }
    pendingStage = Stage::ConsumerAcquire;
    return consume(timeoutMs);
  }

  HRESULT consume(DWORD timeoutMs) noexcept {
    if (!hasResources() || pendingStage != Stage::ConsumerAcquire) return E_INVALIDARG;
    HRESULT mutexResult = consumerMutex->AcquireSync(1, timeoutMs);
    if (mutexResult == WAIT_TIMEOUT) {
      pendingStage = Stage::ConsumerAcquire;
      return mutexResult;
    }
    if (mutexResult != S_OK) {
      faulted = true;
      return mutexResult;
    }
    consumerContext->CopyResource(privateTexture.Get(), sharedConsumer.Get());
    consumerContext->Flush();
    const HRESULT deviceResult = consumer->GetDeviceRemovedReason();
    const HRESULT releaseResult = consumerMutex->ReleaseSync(0);
    mutexResult = deviceResult == S_OK ? releaseResult : deviceResult;
    if (mutexResult != S_OK) {
      faulted = true;
      return mutexResult;
    }
    pendingStage = Stage::None;
    return S_OK;
  }

  HRESULT receiveInternal(ID3D11Texture2D*& output, DWORD timeoutMs) noexcept {
    output = nullptr;
    if (pendingStage != Stage::ConsumerAcquire) return E_UNEXPECTED;
    const HRESULT consumed = consume(timeoutMs);
    if (consumed != S_OK) return consumed;
    return publish(output);
  }

  HRESULT drainConsumer(DWORD timeoutMs) noexcept {
    if (!hasResources()) return S_OK;
    if (!consumerContext || !consumerEvent) {
      faulted = true;
      return E_FAIL;
    }
    consumerContext->End(consumerEvent.Get());
    consumerContext->Flush();
    const ULONGLONG started = GetTickCount64();
    for (;;) {
      const HRESULT deviceResult = consumer->GetDeviceRemovedReason();
      if (deviceResult != S_OK) {
        faulted = true;
        return deviceResult;
      }
      BOOL complete = FALSE;
      // Let the driver flush while polling. On RTX 5090, DONOTFLUSH stalled
      // resize drains despite End/Flush; flags=0 completed the same fixture
      // with the same deadline. This still touches only the consumer context.
      const HRESULT result = consumerContext->GetData(
          consumerEvent.Get(), &complete, sizeof(complete), 0);
      if (result == S_OK && complete) return S_OK;
      if (FAILED(result)) {
        faulted = true;
        return result;
      }
      if (GetTickCount64() - started >= timeoutMs) return timeoutResult();
      Sleep(1);
    }
  }

  HRESULT shutdown(DWORD timeoutMs) noexcept {
    if (!onOwner()) return E_ACCESSDENIED;
    if (timeoutMs == INFINITE) return E_INVALIDARG;
    if (!initialized) return S_OK;
    if (faulted) return E_FAIL;
    if (pendingStage == Stage::ConsumerAcquire) {
      ID3D11Texture2D* ignored = nullptr;
      const HRESULT received = receiveInternal(ignored, timeoutMs);
      if (received != S_OK) return received == WAIT_TIMEOUT ? E_PENDING : received;
    }
    const HRESULT drained = drainConsumer(timeoutMs);
    if (drained != S_OK) return drained;
    clearResources();
    pendingStage = Stage::None;
    consumerDevice1.Reset();
    consumerContext.Reset();
    producerContext.Reset();
    consumer.Reset();
    producer.Reset();
    producerExecutor = nullptr;
    initialized = false;
    return S_OK;
  }

  std::thread::id owner;
  ComPtr<ID3D11Device> producer;
  ComPtr<ID3D11Device> consumer;
  ComPtr<ID3D11DeviceContext> producerContext;
  ComPtr<ID3D11DeviceContext> consumerContext;
  ComPtr<ID3D11Device1> consumerDevice1;
  ImmediateExecutor* producerExecutor = nullptr;
  bool initialized = false;
  bool faulted = false;
  Stage pendingStage = Stage::None;
  ComPtr<ID3D11Texture2D> sharedProducer;
  ComPtr<ID3D11Texture2D> sharedConsumer;
  ComPtr<ID3D11Texture2D> privateTexture;
  ComPtr<IDXGIKeyedMutex> producerMutex;
  ComPtr<IDXGIKeyedMutex> consumerMutex;
  ComPtr<ID3D11Query> consumerEvent;
  UINT sourceWidth = 0, sourceHeight = 0;
  PixelFamily sourceFamily = PixelFamily::RGBA8;
};

SharedTextureTransfer::SharedTextureTransfer() : impl_(new Impl()) {}

SharedTextureTransfer::~SharedTextureTransfer() {
  if (impl_ && impl_->initialized) std::terminate();
}

HRESULT SharedTextureTransfer::initialize(ID3D11Device* producer,
                                          ID3D11Device* consumer,
                                          ImmediateExecutor* producerExecutor) noexcept {
  if (!impl_) return E_FAIL;
  try {
    return impl_->validateInitialize(producer, consumer, producerExecutor);
  } catch (...) {
    return E_FAIL;
  }
}

HRESULT SharedTextureTransfer::copy(ID3D11Texture2D* source,
                                    ID3D11Texture2D*& output,
                                    DWORD timeoutMs) noexcept {
  output = nullptr;
  if (!impl_ || !impl_->onOwner()) return E_ACCESSDENIED;
  if (timeoutMs == INFINITE) return E_INVALIDARG;
  if (!impl_->initialized || impl_->faulted) return E_FAIL;
  try {
    if (impl_->pendingStage != Impl::Stage::None) return E_PENDING;
    if (!source) return E_INVALIDARG;
    D3D11_TEXTURE2D_DESC sourceDescription{};
    source->GetDesc(&sourceDescription);
    FormatChoice choice{};
    if (!validSource(sourceDescription) || !formatChoice(sourceDescription.Format, choice))
      return E_INVALIDARG;
    ComPtr<ID3D11Device> sourceDevice;
    source->GetDevice(&sourceDevice);
    if (!sameDevice(sourceDevice.Get(), impl_->producer.Get())) return E_INVALIDARG;
    const HRESULT resources = impl_->createResources(sourceDescription, choice, timeoutMs);
    if (resources != S_OK) return resources;
    const HRESULT copied = impl_->producerCopy(source, timeoutMs);
    return copied == S_OK ? impl_->publish(output) : copied;
  } catch (...) {
    impl_->faulted = true;
    return E_FAIL;
  }
}

HRESULT SharedTextureTransfer::receive(ID3D11Texture2D*& output,
                                       DWORD timeoutMs) noexcept {
  output = nullptr;
  if (!impl_ || !impl_->onOwner()) return E_ACCESSDENIED;
  if (timeoutMs == INFINITE) return E_INVALIDARG;
  if (!impl_->initialized || impl_->faulted) return E_FAIL;
  try {
    return impl_->receiveInternal(output, timeoutMs);
  } catch (...) {
    impl_->faulted = true;
    return E_FAIL;
  }
}

HRESULT SharedTextureTransfer::shutdown(DWORD timeoutMs) noexcept {
  if (!impl_) return E_FAIL;
  try {
    return impl_->shutdown(timeoutMs);
  } catch (...) {
    impl_->faulted = true;
    return E_FAIL;
  }
}

bool SharedTextureTransfer::ready() const noexcept {
  return impl_ && impl_->onOwner() && impl_->initialized && !impl_->faulted &&
      impl_->pendingStage == Impl::Stage::None;
}

bool SharedTextureTransfer::pending() const noexcept {
  return impl_ && impl_->onOwner() && impl_->pendingStage != Impl::Stage::None;
}

bool SharedTextureTransfer::faulted() const noexcept {
  return impl_ && impl_->onOwner() && impl_->faulted;
}

}  // namespace edvr::openxr
