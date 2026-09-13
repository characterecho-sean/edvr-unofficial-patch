#pragma once

#include "render_route.h"
#include "render_shutdown.h"
#include <chrono>
#include <functional>
#include <utility>

namespace edvr::openxr {

// The two render-boundary steps used by native shutdown. This is intentionally
// a small sequencing helper: it does not add a fallback thread or infer
// application quiescence. The caller retains its generation when either step
// cannot be serviced. The first route invocation keeps RenderRoute's existing
// five-second pending deadline; timeout applies to the final boundary request.
struct RenderShutdownCoordinatorResult {
  bool drainInvoked = false;
  bool drainSucceeded = false;
  RenderShutdownResult boundary{};
  bool finalizerSucceeded = false;
};

template<class Drain, class Finalizer>
RenderShutdownCoordinatorResult runRenderShutdown(
    RenderRoute& route, PresentWorkQueue& present, RenderThreadDispatcher& dispatcher,
    OwnerService& owner, Drain&& drain, Finalizer&& finalizer,
    std::chrono::milliseconds timeout = std::chrono::milliseconds(5000)) {
  RenderShutdownCoordinatorResult result;
  auto drainFn = std::forward<Drain>(drain);
  result.drainInvoked = route.invoke([&result, drainFn = std::move(drainFn)]() mutable {
    result.drainSucceeded = static_cast<bool>(drainFn());
  });
  if (!result.drainInvoked || !result.drainSucceeded) return result;

  auto finalizerFn = std::forward<Finalizer>(finalizer);
  result.boundary = shutdownAtRenderBoundary(
      present, dispatcher, owner,
      [&result, finalizerFn = std::move(finalizerFn)]() mutable {
        result.finalizerSucceeded = static_cast<bool>(finalizerFn());
      }, timeout);
  return result;
}

} // namespace edvr::openxr
