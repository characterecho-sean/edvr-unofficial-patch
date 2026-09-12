#pragma once

#include <condition_variable>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

namespace edvr::openxr {

// CPU-only executor for the native runtime owner. It deliberately knows
// nothing about OpenXR or Win32. start and stop are explicit: the destructor
// terminates if the caller forgot to complete an external stop, so it can
// never detach a callback or silently run user code during destruction.
class OwnerService final {
 public:
  static constexpr std::size_t kQueueCapacity = 16;
  using IdleCallback = std::function<void()>;

  OwnerService() = default;
  OwnerService(const OwnerService&) = delete;
  OwnerService& operator=(const OwnerService&) = delete;
  ~OwnerService() { if (worker_.joinable()||joining_) std::terminate(); }

  bool start(IdleCallback idle = {}) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (running_ || worker_.joinable() || stopping_ || stopCallers_ != 0) return false;
    const bool previousOutcome=finalOutcome_;
    idle_ = std::move(idle);
    stopping_ = false;
    cancellationReady_ = false; idleFailed_ = false; finalOutcome_ = true; finalizer_ = {};
    running_ = true;
    try { worker_ = std::thread(&OwnerService::run, this); }
    catch (...) { running_ = false; idle_ = {}; finalOutcome_=previousOutcome;return false; }
    return true;
  }

  // Returns false for a rejected, cancelled, or throwing invocation. An
  // executing callback always completes before this method returns. Queued
  // requests can be cancelled. Constructing the by-value std::function argument
  // may throw before entry; allocations made inside invoke are contained.
  bool invoke(std::function<void()> callback) {
    if (!callback) return false;
    if (isOwner()) {
      { std::lock_guard<std::mutex> lock(mutex_); if (!running_ || stopping_) return false; }
      try { callback(); } catch (...) { return false; }
      return true;
    }
    std::shared_ptr<Request> request;
    try { request = std::make_shared<Request>(std::move(callback)); }
    catch (...) { return false; }
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (!running_ || stopping_ || queue_.size() >= kQueueCapacity) return false;
      try { queue_.push_back(request); } catch (...) { return false; }
    }
    cv_.notify_one();
    std::unique_lock<std::mutex> waitLock(request->mutex);
    request->cv.wait(waitLock, [&] { return request->done; });
    return request->success;
  }

  // First stop closes admission and owns the finalizer; later calls cannot
  // replace it. Queued requests are cancelled before finalization. Active
  // work finishes, then the finalizer runs on the owner before the join.
  // Owner-thread calls request stop but return false; an external caller must
  // still join. No callback is interrupted, detached or timed out.
  bool stop(std::function<void()> finalizer = {}) {
    std::unique_lock<std::mutex> lock(mutex_);
    const bool owner=isOwnerLocked();
    if(!owner)++stopCallers_;
    std::deque<std::shared_ptr<Request>> cancelled;
    if((running_||worker_.joinable())&&!stopping_) {
      stopping_=true;finalizer_=std::move(finalizer);cancelled.swap(queue_);
      // Captures may have destructors. Release them without the service mutex,
      // before reporting cancellation and allowing final resource cleanup.
      lock.unlock();
      for(auto& request:cancelled){request->callback={};finish(request,false);}
      cancelled.clear();
      lock.lock();cancellationReady_=true;cv_.notify_all();
    }
    if(owner)return false;
    auto done = [&] { --stopCallers_; cv_.notify_all(); };
    if (!worker_.joinable() && !running_) { const bool outcome=finalOutcome_; done(); return outcome; }
    if (joining_) {
      cv_.wait(lock, [&] { return !joining_ && !worker_.joinable(); });
      const bool outcome=finalOutcome_; done(); return outcome;
    }
    joining_ = true;
    std::thread joined=std::move(worker_);
    cv_.notify_all();
    lock.unlock();
    if (joined.joinable()) joined.join();
    lock.lock();
    running_ = false; stopping_ = false; joining_ = false; ownerId_ = {};
    const bool outcome=finalOutcome_;done();return outcome;
  }

  bool isOwner() const { std::lock_guard<std::mutex> lock(mutex_); return isOwnerLocked(); }
  bool running() const { std::lock_guard<std::mutex> lock(mutex_); return running_ && !stopping_; }
  std::size_t pending() const { std::lock_guard<std::mutex> lock(mutex_); return queue_.size(); }

 private:
  struct Request {
    explicit Request(std::function<void()> fn) : callback(std::move(fn)) {}
    std::function<void()> callback; std::mutex mutex; std::condition_variable cv;
    bool done = false, success = false;
  };

  bool isOwnerLocked() const { return ownerId_ == std::this_thread::get_id(); }
  static void finish(const std::shared_ptr<Request>& request, bool success) {
    std::lock_guard<std::mutex> lock(request->mutex);
    request->success = success; request->done = true; request->cv.notify_one();
  }
  void run() {
    { std::lock_guard<std::mutex> lock(mutex_); ownerId_ = std::this_thread::get_id(); }
    auto lastIdle = std::chrono::steady_clock::now();
    for (;;) {
      std::shared_ptr<Request> request;
      bool idle=false;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait_for(lock, std::chrono::milliseconds(5), [&] { return stopping_ || !queue_.empty(); });
        if(stopping_) {
          cv_.wait(lock,[&]{return cancellationReady_;});
          auto finalizer = std::move(finalizer_);
          lock.unlock();
          bool outcome = true; if (finalizer) { try { finalizer(); } catch (...) { outcome = false; } }
          finalizer={};
          // Clear idle captures on this owner, outside the service mutex.
          idle_={};
          lock.lock();finalOutcome_=outcome&&!idleFailed_;ownerId_={};cv_.notify_all();break;
        }
        if (!queue_.empty()) { request = queue_.front(); queue_.pop_front(); }
        else idle=bool(idle_);
      }
      if (request) {
        bool success = true; try { request->callback(); } catch (...) { success = false; }
        request->callback={};
        finish(request, success);
        auto now = std::chrono::steady_clock::now();
        if (idle_ && now - lastIdle >= std::chrono::milliseconds(5)) {
          bool between=false;
          { std::lock_guard<std::mutex> lock(mutex_); between=!stopping_; }
          if (between) { try { idle_(); } catch (...) {idleFailed_=true;} lastIdle = std::chrono::steady_clock::now(); }
        }
      } else if (idle) {
        try { idle_(); } catch (...) {idleFailed_=true;}
        lastIdle = std::chrono::steady_clock::now();
      }
    }
  }

  mutable std::mutex mutex_;
  std::condition_variable cv_;
  std::deque<std::shared_ptr<Request>> queue_;
  std::thread worker_;
  IdleCallback idle_;
  std::thread::id ownerId_{};
  bool running_ = false, stopping_ = false, joining_ = false;
  bool cancellationReady_ = false, idleFailed_ = false, finalOutcome_ = true;
  std::function<void()> finalizer_;
  unsigned stopCallers_ = 0;
};

} // namespace edvr::openxr
