#pragma once

#include <d3d11.h>
#include <memory>
#include <windows.h>

namespace edvr::openxr {

class ImmediateExecutor;
struct GpuWorkObserver;

// Experimental CPU-owned handoff for copying one producer-device texture to a
// private texture on a distinct consumer device. Construct this object on the
// consumer/XR owner thread and call every method below from that same thread.
// The producer context is touched only inside the supplied ImmediateExecutor.
//
// A consumer-acquire timeout leaves the published handoff pending; receive()
// can retry it without touching the original source or producer context. A
// terminal GPU/mutex fault retains the generation for diagnosis, with no reset
// or reinitialization path. The host must keep the object alive after failed
// shutdown. Destruction of an initialized transfer violates that contract and
// terminates; destruction never invokes either context or the executor.
class SharedTextureTransfer final {
 public:
  SharedTextureTransfer();
  SharedTextureTransfer(const SharedTextureTransfer&) = delete;
  SharedTextureTransfer& operator=(const SharedTextureTransfer&) = delete;
  ~SharedTextureTransfer();

  // Rejects SINGLETHREADED devices. The executor outlives this object's
  // initialized lifetime and synchronously retires started callbacks. The
  // consumer context is exclusively owned by this thread; serialize its other
  // users with these calls. Sources must be live COM objects with pixels
  // stable until copy returns.
  HRESULT initialize(ID3D11Device* producer, ID3D11Device* consumer,
                     ImmediateExecutor* producerExecutor) noexcept;
  // On success output receives a borrowed consumer-private texture. It remains
  // valid until the next copy attempt or successful shutdown; callers must
  // not Release the borrowed reference. Inputs are DEFAULT-usage RGBA/BGRA8,
  // one mip/slice, non-MSAA, without CPU access. No color conversion is done.
  // Invalid input and executor refusal clear output without poisoning the
  // transfer. A new copy returns E_PENDING if a previous call already left a
  // handoff pending. The call that first times out returns raw WAIT_TIMEOUT.
  // Require exactly S_OK for success: raw WAIT_TIMEOUT/WAIT_ABANDONED are
  // positive values. INFINITE is rejected. timeoutMs bounds each GPU wait,
  // not the complete operation or the executor's synchronous admission wait.
  HRESULT copy(ID3D11Texture2D* source, ID3D11Texture2D*& output,
               DWORD timeoutMs = 100, GpuWorkObserver* observer = nullptr,
               unsigned phase = 0) noexcept;
  // Retries a published consumer handoff. A producer key-0 timeout occurs
  // before publication and is returned by copy() for the caller to retry with
  // its next copy call.
  HRESULT receive(ID3D11Texture2D*& output, DWORD timeoutMs = 100,
                  GpuWorkObserver* observer = nullptr, unsigned phase = 0) noexcept;
  HRESULT shutdown(DWORD timeoutMs = 5000) noexcept;

  bool ready() const noexcept;
  bool pending() const noexcept;
  bool faulted() const noexcept;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace edvr::openxr
