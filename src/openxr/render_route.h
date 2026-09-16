#pragma once
#include <windows.h>
#include "render_thread_dispatcher.h"
#include "present_work_queue.h"
#include <chrono>
#include <functional>
#include <utility>

namespace edvr::openxr {
struct RenderRoute {
  RenderThreadDispatcher& dispatcher;
  PresentWorkQueue* present=nullptr;
  // How long the render thread was held inside the rendezvous for one
  // invoke -- read on THAT thread, around invokeOwner, so it is the park
  // itself and not the caller's round trip. measured stays false when the
  // rendezvous was never entered on the render thread. queued is true only
  // when invoke() routed the call there via present->invoke because the
  // caller was on some other thread; false means invoke() ran the call
  // directly on the calling thread instead (present==nullptr, or the
  // caller was already the render thread), so a reader who only checks
  // measured cannot tell the two apart.
  struct Park{double ms=0;bool measured=false;bool queued=false;};
  bool bind() {
    bool bound=false;
    if(present&&!present->isRenderThread())
      return present->invoke([&]{bound=dispatcher.bindCurrentThread();})&&bound;
    return dispatcher.bindCurrentThread();
  }
  bool invoke(std::function<void()> work,Park* park=nullptr) {
    using Clock=std::chrono::steady_clock;
    const auto parked=[&](std::function<void()>&& w){
      const auto began=Clock::now();
      const bool completed=dispatcher.invokeOwner(std::move(w));
      if(park){park->ms=std::chrono::duration<double,std::milli>(Clock::now()-began).count();park->measured=true;}
      return completed;
    };
    if(present&&!present->isRenderThread()) {
      bool completed=false;
      const bool ok=present->invoke([&]{completed=parked(std::move(work));})&&completed;
      if(park) park->queued=true;
      return ok;
    }
    return parked(std::move(work));
  }
};
class CountedGraphics final : public ImmediateExecutor {
 public:
  explicit CountedGraphics(RenderThreadDispatcher& dispatcher):dispatcher_(dispatcher){}
  bool invoke(std::function<void()> callback) override {
    bool correct=false;
    const bool completed=dispatcher_.invoke([&] {
      correct=dispatcher_.isRenderThread();
      if(!correct){++wrongThread;return;}
      ++calls;thread=GetCurrentThreadId();callback();
    });
    if(!completed||!correct)++rejected;
    return completed&&correct;
  }
  uint64_t calls=0,wrongThread=0,rejected=0;
  DWORD thread=0;
 private:
  RenderThreadDispatcher& dispatcher_;
};
} // namespace edvr::openxr
