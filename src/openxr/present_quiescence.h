#pragma once

// Coordinates the host's permanent stop request with the render thread's
// acknowledgement after its current Present has really returned.
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace edvr::openxr {

class PresentQuiescence {
 public:
  // Construction binds the object to the constructing (render) thread.
  PresentQuiescence() : renderThread_(std::this_thread::get_id()) {}
  PresentQuiescence(const PresentQuiescence&) = delete;
  PresentQuiescence& operator=(const PresentQuiescence&) = delete;

  // Requests a permanent stop and waits for the render thread's one terminal
  // acknowledgement. A deadline only bounds this caller; it never clears the
  // request or reopens admission. Calls from the bound thread are rejected.
  bool requestAndWait(
      std::chrono::milliseconds timeout = std::chrono::milliseconds(5000)) {
    if (std::this_thread::get_id() == renderThread_) return false;
    std::unique_lock<std::mutex> lock(mutex_);
    stopRequested_ = true;
    cv_.notify_all();
    if (acknowledged_) return acknowledgementSucceeded_;
    if (timeout.count() <= 0) return false;
    if (!cv_.wait_for(lock, timeout, [&] { return acknowledged_; })) return false;
    return acknowledgementSucceeded_;
  }

  bool stopRequested() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return stopRequested_;
  }

  // Called by the bound render thread only, after Present and callback lease
  // cleanup. The first call closes the state permanently; repeats are rejected.
  bool acknowledge(bool success) {
    if (std::this_thread::get_id() != renderThread_) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    if (!stopRequested_ || acknowledged_) return false;
    acknowledged_ = true;
    acknowledgementSucceeded_ = success;
    stopped_ = success;
    cv_.notify_all();
    return true;
  }

  bool stopped() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return stopped_;
  }

 private:
  const std::thread::id renderThread_;
  mutable std::mutex mutex_;
  std::condition_variable cv_;
  bool stopRequested_ = false;
  bool acknowledged_ = false;
  bool acknowledgementSucceeded_ = false;
  bool stopped_ = false;
};

}  // namespace edvr::openxr
