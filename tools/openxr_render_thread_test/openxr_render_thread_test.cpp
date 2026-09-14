#include "../../src/openxr/render_thread_dispatcher.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
using edvr::openxr::OwnerService;
using edvr::openxr::RenderThreadDispatcher;
namespace {
std::atomic<unsigned> checks{0}, failures{0};
void check(bool value, const char* label) {
  ++checks;
  if (!value) { ++failures; std::printf("FAIL: %s\n", label); }
}
class Event {
 public:
  void signal() { std::lock_guard<std::mutex> lock(mutex_); ready_=true; cv_.notify_all(); }
  bool wait(std::chrono::seconds timeout=std::chrono::seconds(5)) {
    std::unique_lock<std::mutex> lock(mutex_);
    return cv_.wait_for(lock,timeout,[&]{return ready_;});
  }
 private:
  std::mutex mutex_; std::condition_variable cv_; bool ready_=false;
};
struct Watchdog {
  Event finished;
  // Event's bounded wait also bounds a broken test that never wakes its caller.
  std::thread worker{[&]{if(!finished.wait(std::chrono::seconds(20))){std::fputs("FAIL: handoff watchdog\n",stderr);std::_Exit(1);}}};
  ~Watchdog(){finished.signal();worker.join();}
};
struct Service { OwnerService owner; ~Service(){owner.stop();} };
// OwnerService exposes a count, not a queue notification. Poll that exact
// admission predicate with a deadline; elapsed time never stands in for it.
template<class Predicate> bool observed(Predicate predicate) {
  std::mutex mutex; std::condition_variable cv;
  std::unique_lock<std::mutex> lock(mutex);
  const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
  while(!predicate()) {
    if(std::chrono::steady_clock::now()>=deadline)return false;
    cv.wait_for(lock,std::chrono::milliseconds(1));
  }
  return true;
}
void basic() {
  Service service; auto& owner=service.owner; RenderThreadDispatcher render(owner);
  check(!render.invokeOwner([]{}),"unbound owner work rejected");
  check(owner.start(),"owner starts");
  check(render.bindCurrentThread()&&render.isRenderThread(),"bind caller once");
  check(!render.bindCurrentThread(),"cannot rebind");
  check(!render.invoke([]{}),"render caller cannot request itself");
  check(owner.invoke([&]{check(!render.invoke([]{}),"owner cannot render without boundary");}),"outside-boundary probe");
  std::thread foreign([&]{check(!render.isRenderThread()&&!render.invokeOwner([]{}),"foreign caller rejected");});
  foreign.join();
  check(!render.invokeOwner([]{throw 1;}),"owner exception reported");
  const auto caller=std::this_thread::get_id();
  unsigned sequence=0;
  check(render.invokeOwner([&] {
    check(owner.isOwner()&&std::this_thread::get_id()!=caller,"XR owner is separate");
    check(!render.invokeOwner([]{}),"owner cannot nest caller boundary");
    check(!render.invoke([]{throw 2;}),"graphics exception reported to owner");
    for(unsigned i=0;i<2;++i)check(render.invoke([&] {
      check(render.isRenderThread()&&std::this_thread::get_id()==caller,"graphics stays on render caller");
      check(!render.invokeOwner([]{}),"render callback cannot nest owner boundary");
      check(sequence++==i,"graphics jobs ordered");
    }),"graphics succeeds after exception");
  }),"owner/graphics exception recovery");
  check(sequence==2,"both graphics jobs completed before return");
  struct Capture {
    std::function<void()> retire;
    ~Capture(){retire();}
  };
  std::atomic<bool> graphicsRetired{false},ownerRetired{false};
  auto ownerCapture=std::make_shared<Capture>();
  ownerCapture->retire=[&]{ownerRetired=true;check(!render.invoke([]{}),"capture destructor has no owner-work admission");};
  check(render.invokeOwner([&, kept=std::move(ownerCapture)] {
    auto graphicsCapture=std::make_shared<Capture>();
    graphicsCapture->retire=[&]{graphicsRetired=true;check(!render.invokeOwner([]{}),"graphics destructor reentry rejected");};
    check(render.invoke([kept=std::move(graphicsCapture)]{}),"owned graphics capture");
    check(graphicsRetired,"graphics capture retired before owner resumes");
  }),"owned owner capture");
  check(ownerRetired,"owner capture retired before render caller resumes");
  render.close();
  check(!render.invokeOwner([]{}),"closed rejects new boundary");
}
void unrelatedOwner() {
  Service service; auto& owner=service.owner; RenderThreadDispatcher render(owner);
  check(owner.start()&&render.bindCurrentThread(),"unrelated owner setup");
  Event entered,release;
  check(owner.submit([&]{entered.signal();check(release.wait(),"release earlier owner work");
    check(!render.invoke([]{}),"earlier queued owner work cannot borrow later boundary");}),"earlier work queued");
  check(entered.wait(),"earlier owner active");
  std::thread releaseEarlier([&]{check(observed([&]{return owner.pending()==1;}),"boundary really queued");release.signal();});
  check(render.invokeOwner([&]{check(render.invoke([]{}),"designated owner work has admission");}),"designated boundary completes");
  releaseEarlier.join();
}
void queueCancellation() {
  Service service; auto& owner=service.owner;
  check(owner.start(),"queue setup"); Event entered,release,allCancelled;
  std::atomic<unsigned> completed{0},ran{0}; std::atomic<bool> stopped{false};
  check(owner.submit([&]{entered.signal();check(release.wait(),"active work released");}),"blocking owner accepted");
  check(entered.wait(),"blocking owner started");
  for(unsigned i=0;i<OwnerService::kQueueCapacity;++i)
    check(owner.submit([&]{++ran;},[&](bool ok){
      check(!ok,"queued cancellation reports failure");
      if(++completed==OwnerService::kQueueCapacity)allCancelled.signal();
    }),"bounded queue slot accepted");
  check(!owner.submit([]{},[&](bool){++completed;}),"full queue rejects without completion");
  std::thread stopper([&]{stopped=owner.stop();});
  check(allCancelled.wait(),"cancellation completes before active owner stops");
  check(!stopped&&ran==0,"stop waits for active work; cancelled work never runs");
  release.signal();stopper.join();
  check(stopped&&completed==OwnerService::kQueueCapacity,"exactly one completion per accepted cancelled request");
  check(!owner.submit([]{},[&](bool){++completed;})&&completed==OwnerService::kQueueCapacity,"closed submit does not call completion");
  check(owner.start(),"service restarts"); Event completion;
  check(owner.submit([]{},[&](bool ok){check(ok&&owner.pending()==0,"completion outside service lock");completion.signal();throw 3;}),"throwing completion accepted");
  check(completion.wait()&&owner.invoke([]{}),"throwing completion cannot kill owner");
}
void cancelledBoundary() {
  Service service; auto& owner=service.owner; RenderThreadDispatcher render(owner);
  check(owner.start()&&render.bindCurrentThread(),"cancelled boundary setup");
  Event entered,release; std::atomic<bool> ran{false},stopped{false};
  check(owner.submit([&]{entered.signal();check(release.wait(),"blocked owner release");}),"block owner before boundary");
  check(entered.wait(),"owner blocked");
  std::thread stopper([&]{check(observed([&]{return owner.pending()==1;}),"caller request actually queued");stopped=owner.stop();});
  check(!render.invokeOwner([&]{ran=true;}),"cancelled owner request wakes waiting render caller");
  check(!ran&&!stopped,"cancelled callback absent; active owner still drains");
  release.signal();stopper.join();render.close();
  check(stopped,"cancelled boundary joined");
}
void closeActive() {
  Service service; auto& owner=service.owner; RenderThreadDispatcher render(owner);
  check(owner.start()&&render.bindCurrentThread(),"active graphics setup");
  Event entered,closed; std::atomic<bool> stopped{false}; bool graphicsResult=false;
  std::thread stopper([&]{check(entered.wait(),"active graphics observed");render.close();closed.signal();stopped=owner.stop();});
  check(!render.invokeOwner([&]{graphicsResult=render.invoke([&]{
    entered.signal();check(closed.wait(),"close returns while graphics active");
    check(!stopped,"shutdown cannot pass active graphics callback");
  });}),"closed boundary reports cancellation after draining");
  stopper.join();check(graphicsResult&&stopped,"started graphics completes before owner stop returns");
}
void closeInside() {
  Service service; auto& owner=service.owner; RenderThreadDispatcher render(owner);
  check(owner.start()&&render.bindCurrentThread(),"reentrant close setup");
  check(!render.invokeOwner([&]{
    check(render.invoke([&]{render.close();}),"close inside graphics does not strand active callback");
    check(!render.invoke([]{}),"no graphics admitted after close");
  }),"reentrant close drains without deadlock");
}
int selfTest() {
  Watchdog watchdog;
  basic();unrelatedOwner();queueCancellation();cancelledBoundary();closeActive();closeInside();
  std::printf("openxr_render_thread_test: %u checks, %u failures\n",checks.load(),failures.load());
  return failures ? 1 : 0;
}
}
int main(int argc,char** argv) {
  if(argc==2&&std::string(argv[1])=="--dry-run") {
    std::puts("Would test render handoff with bounded CPU threads; no threads, files, device or runtime created.");return 0;
  }
  if(argc==2&&std::string(argv[1])=="--self-test")return selfTest();
  std::fputs("usage: openxr_render_thread_test --dry-run|--self-test\n",stderr);return 2;
}
