#pragma once
#include <windows.h>
#include "render_thread_dispatcher.h"
#include "present_work_queue.h"
#include <functional>
#include <utility>

namespace edvr::openxr {
struct RenderRoute {
  RenderThreadDispatcher& dispatcher;
  PresentWorkQueue* present=nullptr;
  bool bind() {
    bool bound=false;
    if(present&&!present->isRenderThread())
      return present->invoke([&]{bound=dispatcher.bindCurrentThread();})&&bound;
    return dispatcher.bindCurrentThread();
  }
  bool invoke(std::function<void()> work) {
    if(present&&!present->isRenderThread()) {
      bool completed=false;
      return present->invoke([&]{completed=dispatcher.invokeOwner(std::move(work));})&&completed;
    }
    return dispatcher.invokeOwner(std::move(work));
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
