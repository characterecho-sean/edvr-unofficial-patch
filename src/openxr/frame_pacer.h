#pragma once
#include <windows.h>
#include <openxr/openxr.h>
#include <condition_variable>
#include <mutex>
#include <thread>
#include "native_trace.h"

namespace edvr::openxr {
// One xrWaitFrame at a time on its own thread, so the owner can hand the game
// a frame before the runtime's pacing point and block at Submit instead
// (OpenXR Toolkit's "Turbo mode"). The owner kicks, takes and stops; the
// worker calls nothing but xrWaitFrame. The spec synchronizes xrWaitFrame
// only against other xrWaitFrame calls, so it may run while the owner is in
// xrBeginFrame, xrEndFrame or xrLocateViews.
class FramePacer final {
 public:
  FramePacer() = default;
  ~FramePacer() { stop(); }
  FramePacer(const FramePacer&) = delete;
  FramePacer& operator=(const FramePacer&) = delete;
  // edvr_openxr.ini frame_thread_priority: set by the owner before the first
  // kick() so a freshly spawned worker thread picks it up at the top of run().
  bool priorityHigh = true;
  // Owner. Stops a live worker first; nothing may be pending.
  void bind(PFN_xrWaitFrame wait, XrSession session) { stop(); wait_ = wait; session_ = session; }
  bool bound() const { return wait_ && session_; }
  // Owner: start one xrWaitFrame. False when unbound or one is already pending.
  bool kick() {
    if (!bound()) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    if (pending_) return false;
    pending_ = true; done_ = false;
    if (!worker_.joinable()) { quit_ = false; worker_ = std::thread([this] { run(); }); }
    cv_.notify_all();
    return true;
  }
  bool pending() const { std::lock_guard<std::mutex> lock(mutex_); return pending_; }
  bool ready() const { std::lock_guard<std::mutex> lock(mutex_); return pending_ && done_; }
  // Owner: block until the pending wait returns and hand over its result.
  // Unbounded on purpose: the synchronous path never had a timeout either,
  // so a runtime that never returns hangs the game exactly as before.
  bool take(XrResult& result, XrFrameState& state) {
    std::unique_lock<std::mutex> lock(mutex_);
    if (!pending_) return false;
    cv_.wait(lock, [this] { return done_; });
    result = result_; state = state_; pending_ = done_ = false;
    return true;
  }
  // Owner: join the worker, waiting for an in-flight xrWaitFrame to return.
  // A returned-but-untaken result is dropped; callers drain before stopping.
  void stop() {
    { std::lock_guard<std::mutex> lock(mutex_); quit_ = true; }
    cv_.notify_all();
    if (worker_.joinable()) worker_.join();
    std::lock_guard<std::mutex> lock(mutex_); pending_ = done_ = quit_ = false;
  }
 private:
  void run() {
    if (priorityHigh) raiseCurrentThreadPriority("pacer");
    std::unique_lock<std::mutex> lock(mutex_);
    for (;;) {
      cv_.wait(lock, [this] { return quit_ || (pending_ && !done_); });
      if (quit_) return;
      lock.unlock();
      XrFrameWaitInfo info{XR_TYPE_FRAME_WAIT_INFO}; XrFrameState state{XR_TYPE_FRAME_STATE};
      const XrResult r = wait_(session_, &info, &state);
      lock.lock();
      result_ = r; state_ = state; done_ = true;
      cv_.notify_all();
    }
  }
  PFN_xrWaitFrame wait_ = nullptr;
  XrSession session_ = XR_NULL_HANDLE;
  mutable std::mutex mutex_;
  std::condition_variable cv_;
  std::thread worker_;
  bool pending_ = false, done_ = false, quit_ = false;
  XrResult result_ = XR_SUCCESS;
  XrFrameState state_{XR_TYPE_FRAME_STATE};
};
}  // namespace edvr::openxr
