#pragma once

#include "owner_service.h"
#include "present_work_queue.h"
#include "render_thread_dispatcher.h"
#include <functional>
#include <utility>

namespace edvr::openxr {

struct RenderShutdownResult {
  bool entered = false;
  bool joined = false;
};

// The host first drains all submitted GPU work. A foreign Shutdown caller
// then waits for the finalizer to run while the bound render caller is held
// inside its Present callback, after DXGI and existing frame work have ended.
// This excludes subsequent game rendering without owning the game's loop.
//
// The callback lease and its user object must remain alive until the request
// AND callback have unwound; the foreign caller closes/releases them afterward.
// The finalizer may not request more render-dispatcher work or wait for the
// render caller to dispatch window messages. A runtime that requires that
// message progress needs a different teardown contract, not nested pumping of
// arbitrary game messages while the context is borrowed.
//
// A queued deadline cancels before entry. Once entered, the finalizer and its
// borrowed captures finish before return. Failure does not permit resource
// destruction; the host retains them and prevents another runtime generation.
inline RenderShutdownResult shutdownAtRenderBoundary(
    PresentWorkQueue& queue, RenderThreadDispatcher& dispatcher,
    OwnerService& owner, std::function<void()> finalizer,
    std::chrono::milliseconds timeout = std::chrono::milliseconds(5000)) {
  RenderShutdownResult result;
  auto finish = [&] {
    if (!queue.isRenderThread() || !dispatcher.isRenderThread()) return;
    result.entered = true;
    dispatcher.close();
    result.joined = owner.stop(std::move(finalizer));
  };
  // A Shutdown call already on the render thread owns that thread directly;
  // enqueueing and waiting for its own next Present would deadlock.
  if (queue.isRenderThread()) finish();
  else if (!queue.invoke(finish, timeout)) return {};
  return result;
}

} // namespace edvr::openxr
