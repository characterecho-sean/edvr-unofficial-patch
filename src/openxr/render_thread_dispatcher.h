#pragma once

#include "immediate_executor.h"
#include "owner_service.h"
#include <condition_variable>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

namespace edvr::openxr {

// A synchronous rendezvous, not a graphics worker. The bound render caller
// already owns its context exclusively and keeps that ownership throughout
// invokeOwner. While waiting it executes graphics requests from that ONE owner
// callback. Idle/other queued owner work has no permission to request graphics.
// The bound thread must remain alive until admission closes and calls drain.
class RenderThreadDispatcher final : public ImmediateExecutor {
 public:
  explicit RenderThreadDispatcher(OwnerService& owner)
      : owner_(owner), state_(std::make_shared<State>()) {}
  RenderThreadDispatcher(const RenderThreadDispatcher&) = delete;
  RenderThreadDispatcher& operator=(const RenderThreadDispatcher&) = delete;
  ~RenderThreadDispatcher() override {
    std::lock_guard<std::mutex> lock(state_->mutex);
    if (state_->boundary || state_->graphics) std::terminate();
    state_->closed = true;
  }

  bool bindCurrentThread() {
    std::lock_guard<std::mutex> lock(state_->mutex);
    if (state_->bound || state_->closed) return false;
    state_->bound = true;
    state_->renderThread = std::this_thread::get_id();
    return true;
  }
  bool isRenderThread() const {
    std::lock_guard<std::mutex> lock(state_->mutex);
    return state_->bound && state_->renderThread == std::this_thread::get_id();
  }

  bool invoke(std::function<void()> callback) override {
    if (!callback || !owner_.isOwner()) return false;
    const auto state = state_;
    std::shared_ptr<GraphicsJob> job;
    try { job = std::make_shared<GraphicsJob>(std::move(callback)); }
    catch (...) { return false; }
    std::unique_lock<std::mutex> lock(state->mutex);
    if (state->closed || !state->boundary || !state->boundary->ownerExecuting ||
        state->graphics || state->renderThread == std::this_thread::get_id())
      return false;
    state->graphics = job;
    state->cv.notify_all();
    // close cancels pending work; a started callback must finish. Returning
    // merely because admission closed would free the owner's borrowed stack.
    state->cv.wait(lock, [&] { return job->done; });
    return job->success;
  }

  bool invokeOwner(std::function<void()> callback) {
    if (!callback || owner_.isOwner()) return false;
    const auto state = state_;
    std::shared_ptr<Boundary> boundary;
    try { boundary = std::make_shared<Boundary>(); }
    catch (...) { return false; }
    {
      std::lock_guard<std::mutex> lock(state->mutex);
      if (state->closed || !state->bound || state->boundary ||
          state->renderThread != std::this_thread::get_id()) return false;
      state->boundary = boundary;
    }
    bool accepted = false;
    try {
      // Only heap-owned coordination state is captured by completion. It may
      // finish unwinding after this caller returns without touching this object.
      accepted = owner_.submit(
        [state, boundary, callback = std::move(callback)]() mutable {
          {
            std::lock_guard<std::mutex> lock(state->mutex);
            if (state->closed) { boundary->cancelled = true; return; }
            boundary->ownerExecuting = true;
          }
          struct EndOwnerWork {
            std::shared_ptr<State> state;
            std::shared_ptr<Boundary> boundary;
            ~EndOwnerWork() {
              std::lock_guard<std::mutex> lock(state->mutex);
              boundary->ownerExecuting = false;
            }
          } end{state, boundary};
          callback();
        },
        [state, boundary](bool success) {
          {
            std::lock_guard<std::mutex> lock(state->mutex);
            boundary->success = success && !boundary->cancelled;
            boundary->done = true;
          }
          state->cv.notify_all();
        });
    } catch (...) { accepted = false; }
    if (!accepted) {
      std::lock_guard<std::mutex> lock(state->mutex);
      state->boundary.reset();
      return false;
    }

    for (;;) {
      std::shared_ptr<GraphicsJob> job;
      {
        std::unique_lock<std::mutex> lock(state->mutex);
        state->cv.wait(lock, [&] { return state->graphics || boundary->done; });
        if (state->graphics) {
          job = state->graphics;
          job->running = true;
        } else {
          const bool success = boundary->success;
          // Only the render caller retires the boundary, after all callbacks
          // and their user captures have finished. close never clears it.
          state->boundary.reset();
          return success;
        }
      }
      bool success = true;
      try { job->callback(); } catch (...) { success = false; }
      job->callback = {};
      {
        std::lock_guard<std::mutex> lock(state->mutex);
        job->success = success;
        job->done = true;
        state->graphics.reset();
      }
      state->cv.notify_all();
    }
  }

  // Nonblocking admission closure, callable from either thread or a callback.
  // The host must subsequently stop/join the owner and join its render caller
  // before destroying this dispatcher. There is no detached or timed-out work.
  void close() {
    const auto state = state_;
    std::shared_ptr<GraphicsJob> cancelled;
    {
      std::lock_guard<std::mutex> lock(state->mutex);
      state->closed = true;
      if (state->boundary) state->boundary->cancelled = true;
      if (state->graphics && !state->graphics->running)
        cancelled = std::move(state->graphics);
    }
    if (cancelled) {
      cancelled->callback = {};
      {
        std::lock_guard<std::mutex> lock(state->mutex);
        cancelled->success = false;
        cancelled->done = true;
      }
    }
    state->cv.notify_all();
  }

 private:
  struct GraphicsJob {
    explicit GraphicsJob(std::function<void()> fn) : callback(std::move(fn)) {}
    std::function<void()> callback;
    bool running = false, done = false, success = false;
  };
  struct Boundary {
    bool ownerExecuting = false, done = false, success = false, cancelled = false;
  };
  struct State {
    std::mutex mutex;
    std::condition_variable cv;
    std::thread::id renderThread{};
    bool bound = false, closed = false;
    std::shared_ptr<Boundary> boundary;
    std::shared_ptr<GraphicsJob> graphics;
  };
  OwnerService& owner_;
  const std::shared_ptr<State> state_;
};

} // namespace edvr::openxr
