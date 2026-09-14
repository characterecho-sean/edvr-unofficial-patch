#pragma once

// CPU-only handoff for work which must be performed by a bound render thread.
// The host owns the render-thread pump and must join all callers before
// destroying this object.
#include <chrono>
#include <exception>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>

namespace edvr::openxr {

class PresentWorkQueue {
 public:
  static constexpr std::size_t kQueueCapacity = 16;

  PresentWorkQueue() = default;
  PresentWorkQueue(const PresentWorkQueue&) = delete;
  PresentWorkQueue& operator=(const PresentWorkQueue&) = delete;

  // Binds once for the lifetime of the queue.  Rebinding is never allowed.
  bool bindCurrentThread() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (bound_ || closed_) return false;
    bound_ = true;
    renderThread_ = std::this_thread::get_id();
    return true;
  }

  bool isRenderThread() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return bound_ && renderThread_ == std::this_thread::get_id();
  }

  // Queues a callback for the render thread and waits for its completion.
  // Requests made by the render thread, before binding, or after close are
  // rejected. A timeout cancels only work which is still queued; it never
  // detaches a callback already running.
  // Constructing the by-value std::function may throw before entry. Captures
  // must not synchronously wait for further work from this same queue while
  // retiring. The bound render thread must stay alive until all callers join.
  bool invoke(std::function<void()> callback,
              std::chrono::milliseconds pendingTimeout =
                  std::chrono::milliseconds(5000)) {
    if (!callback) return false;
    std::shared_ptr<Request> request;
    try {
      request = std::make_shared<Request>(std::move(callback));
    } catch (...) {
      return false;
    }
    std::unique_lock<std::mutex> lock(mutex_);
    if (!bound_ || closed_ || renderThread_ == std::this_thread::get_id() ||
        queue_.size() >= kQueueCapacity) {
      lock.unlock();
      request->callback = std::function<void()>();
      return false;
    }
    try {
      queue_.push_back(request);
    } catch (...) {
      lock.unlock();
      request.reset();
      return false;
    }
    ++activeInvokers_;
    cv_.notify_one();
    const auto deadline = std::chrono::steady_clock::now() + pendingTimeout;
    for (;;) {
      if (request->done) {
        const bool result=request->success; --activeInvokers_; return result;
      }
      if (cv_.wait_until(lock, deadline, [&] { return request->done; })) {
        const bool result=request->success; --activeInvokers_; return result;
      }
      // The deadline elapsed. Claim cancellation only while still queued.
      if (!request->running && !request->done) {
        for (auto it = queue_.begin(); it != queue_.end(); ++it) {
          if (*it == request) {
            queue_.erase(it);
            request->cancelled = true;
            std::function<void()> release = std::move(request->callback);
            lock.unlock();
            release = std::function<void()>();
            lock.lock();
            request->done = true;
            request->success = false;
            cv_.notify_all();
            --activeInvokers_;
            return false;
          }
        }
      }
      // A pump won the race and is running the callback. Keep waiting.
      cv_.wait(lock, [&] { return request->done; });
      const bool result=request->success; --activeInvokers_; return result;
    }
  }

  // Executes at most one queued callback. Only the bound render thread may
  // call this, and nested pump calls (including from a callback) are rejected.
  bool pump() {
    std::shared_ptr<Request> request;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (!bound_ || closed_ || renderThread_ != std::this_thread::get_id() ||
          active_ || queue_.empty()) return false;
      request = queue_.front();
      queue_.pop_front();
      request->running = true;
      active_ = true;
    }
    bool success = true;
    try { request->callback(); }
    catch (...) { success = false; }
    // Destroy captures before taking the coordination lock and waking invoke.
    request->callback = std::function<void()>();
    {
      std::lock_guard<std::mutex> lock(mutex_);
      request->success = success;
      request->running = false;
      request->done = true;
      active_ = false;
      cv_.notify_all();
    }
    return success;
  }

  // Nonblocking. It closes admission and retires every queued request. An
  // active callback is allowed to finish, including when it calls close().
  void close() {
    for (;;) {
      std::shared_ptr<Request> request;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        closed_ = true;
        if (queue_.empty()) { cv_.notify_all(); return; }
        request = queue_.front();
        queue_.pop_front();
        request->cancelled = true;
      }
      std::function<void()> release = std::move(request->callback);
      release = std::function<void()>();
      {
        std::lock_guard<std::mutex> lock(mutex_);
        request->done = true;
        request->success = false;
        cv_.notify_all();
      }
    }
  }

  // Read-only diagnostic useful to host integration and tests.
  std::size_t pending() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return queue_.size();
  }

  // Destruction is intentionally nonblocking: the host contract requires
  // close(), then joining all callers and the render thread, before this runs.
  ~PresentWorkQueue() {
    std::lock_guard<std::mutex> lock(mutex_);
    if(!closed_ || !queue_.empty() || active_ || activeInvokers_!=0)std::terminate();
  }

 private:
  struct Request {
    explicit Request(std::function<void()> fn) : callback(std::move(fn)) {}
    std::function<void()> callback;
    bool running = false;
    bool cancelled = false;
    bool done = false;
    bool success = false;
  };

  mutable std::mutex mutex_;
  std::condition_variable cv_;
  std::deque<std::shared_ptr<Request>> queue_;
  std::thread::id renderThread_{};
  bool bound_ = false;
  bool closed_ = false;
  bool active_ = false;
  std::size_t activeInvokers_ = 0;
};

} // namespace edvr::openxr
