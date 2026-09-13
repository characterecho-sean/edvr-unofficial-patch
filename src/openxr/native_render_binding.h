#pragma once

#include "native_graphics_client.h"
#include "render_boundary_client.h"
#include "present_work_queue.h"
#include <atomic>
#include <condition_variable>
#include <mutex>

namespace edvr::openxr {

// CPU startup wiring shared with the future game backend. Acquire on the Init
// caller; the first actual callback binds the render queue without the host
// supplying a thread ID or device. No XR resources are needed until readiness.
// This object and its callback module outlive every callback/queued invoker.
// Shutdown must close admission and join users before release returns S_OK.
class NativeRenderBinding final {
 public:
  NativeRenderBinding() = default;
  NativeRenderBinding(const NativeRenderBinding&) = delete;
  NativeRenderBinding& operator=(const NativeRenderBinding&) = delete;
  ~NativeRenderBinding() { work_.close(); }

  HRESULT acquire(const std::wstring& trustedAbsolutePath) {
    if (attempted_) return E_UNEXPECTED; // a binding owns one queue generation
    attempted_ = true;
    HRESULT result = graphics_.acquire(trustedAbsolutePath);
    if (result != S_OK) return result;
    const EdvrRenderBoundaryRequest request{sizeof(request), EDVR_RENDER_BOUNDARY_VERSION_1,
        graphics_.device(), &callback, this};
    result = boundary_.acquire(graphics_.provider(), request);
    if (result != S_OK) graphics_.reset();
    return result;
  }

  bool waitForRender(std::chrono::milliseconds timeout = std::chrono::milliseconds(5000)) {
    std::unique_lock<std::mutex> lock(mutex_);
    return changed_.wait_for(lock, timeout, [&] { return ready_ || failed_; }) &&
        ready_ && !failed_;
  }
  HRESULT close() {
    const HRESULT result = boundary_.close();
    work_.close();
    { std::lock_guard<std::mutex> lock(mutex_); failed_ = true; }
    changed_.notify_all();
    return result;
  }
  HRESULT release() {
    const HRESULT result = close();
    if (result != S_OK) return result;
    const HRESULT released = boundary_.release();
    if (released == S_OK) graphics_.reset();
    return released;
  }

  PresentWorkQueue& work() { return work_; }
  ID3D11Device* device() const { return graphics_.device(); }
  ID3D11DeviceContext* context() const { return graphics_.context(); }
  HMODULE provider() const { return graphics_.provider(); }
  DWORD renderThread() const { return thread_.load(std::memory_order_acquire); }
  uint64_t callbacks() const { return callbacks_.load(std::memory_order_acquire); }
  bool callbackActive() const { return active_.load(std::memory_order_acquire); }
  bool correct() const { return correct_.load(std::memory_order_acquire); }

 private:
  static HRESULT WINAPI callback(void* user, ID3D11Device* device, ID3D11DeviceContext* context) {
    auto& self = *static_cast<NativeRenderBinding*>(user);
    const DWORD thread = GetCurrentThreadId();
    bool accepted = device == self.device() && context == self.context();
    {
      std::lock_guard<std::mutex> lock(self.mutex_);
      if (!self.ready_ && !self.failed_ && accepted) {
        accepted = self.work_.bindCurrentThread();
        if (accepted) {
          self.thread_.store(thread, std::memory_order_release);
          self.ready_ = true;
        }
      }
      accepted = accepted && self.ready_ && !self.failed_ && self.renderThread() == thread;
      if (!accepted) self.failed_ = true;
    }
    self.changed_.notify_all();
    if (!accepted) {
      self.correct_.store(false, std::memory_order_release);
      self.work_.close();
      return E_ACCESSDENIED;
    }
    self.callbacks_.fetch_add(1, std::memory_order_relaxed);
    self.active_.store(true, std::memory_order_release);
    self.work_.pump();
    self.active_.store(false, std::memory_order_release);
    return S_OK;
  }

  // Destruction order keeps device/provider references alive through boundary
  // release. The boundary client refuses destruction with an active callback.
  NativeGraphicsClient graphics_;
  PresentWorkQueue work_;
  std::mutex mutex_;
  std::condition_variable changed_;
  bool attempted_ = false, ready_ = false, failed_ = false;
  std::atomic<DWORD> thread_{0};
  std::atomic<uint64_t> callbacks_{0};
  std::atomic<bool> active_{false}, correct_{true};
  RenderBoundaryClient boundary_;
};

} // namespace edvr::openxr
