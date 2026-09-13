#include "../../src/openxr/present_work_queue.h"
#include "../../src/openxr/present_quiescence.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using edvr::openxr::PresentWorkQueue;
using edvr::openxr::PresentQuiescence;
namespace {
std::atomic<unsigned> checks{0}, failures{0};
void check(bool value, const char* label) {
  ++checks; if (!value) { ++failures; std::printf("FAIL: %s\n", label); }
}
class Event {
 public:
  void signal() { std::lock_guard<std::mutex> lock(m_); ready_=true; cv_.notify_all(); }
  bool wait(std::chrono::seconds timeout=std::chrono::seconds(3)) {
    std::unique_lock<std::mutex> lock(m_);
    return cv_.wait_for(lock, timeout, [&]{return ready_;});
  }
 private:
  std::mutex m_; std::condition_variable cv_; bool ready_=false;
};
struct Watchdog {
  Event done;
  std::thread thread{[&]{ if (!done.wait(std::chrono::seconds(20))) std::_Exit(1); }};
  ~Watchdog() { done.signal(); thread.join(); }
};
bool reaches(const std::function<bool()>& predicate) {
  const auto until=std::chrono::steady_clock::now()+std::chrono::seconds(2);
  while (!predicate()) {
    if (std::chrono::steady_clock::now() >= until) return false;
    std::this_thread::yield();
  }
  return true;
}

void basicAndOrdering() {
  PresentWorkQueue q;
  check(!q.pump() && !q.invoke([]{}) && !q.invoke(std::function<void()>{}), "unbound and null calls rejected");
  check(q.bindCurrentThread() && q.isRenderThread(), "bind render thread once");
  check(!q.bindCurrentThread(), "rebind rejected");
  check(!q.invoke([]{}), "render thread invoke rejected");
  std::atomic<bool> foreignPump{true};
  std::thread foreign([&]{ foreignPump=q.pump(); }); foreign.join();
  check(!foreignPump, "foreign pump rejected");
  unsigned sequence=0; bool first=false, second=false;
  std::thread caller([&]{
    first=q.invoke([&]{ first=true; check(sequence++==0,"first request order"); });
    second=q.invoke([&]{ second=true; check(sequence++==1,"second request order"); });
  });
  check(reaches([&]{return q.pending()==1;}), "first request queued");
  check(q.pump(), "first callback succeeds");
  check(reaches([&]{return q.pending()==1;}), "second request queued");
  check(q.pump(), "second callback succeeds"); caller.join();
  check(first&&second&&sequence==2, "ordered callers complete");
  q.close(); check(!q.invoke([]{}), "closed admission rejected");
}

void exceptionAndCaptureLifetime() {
  PresentWorkQueue q; check(q.bindCurrentThread(), "exception setup");
  std::atomic<bool> retired{false};
  struct Capture {
    Capture(std::atomic<bool>* value,PresentWorkQueue* queue):retired(value),queue(queue){}
    std::atomic<bool>* retired;PresentWorkQueue* queue;
    ~Capture(){check(!queue->pump()&&!queue->invoke([]{}),"capture destructor cannot reenter bound render queue");*retired=true;}
  };
  bool result=false; std::atomic<bool> retiredAtReturn{false};
  std::thread caller([&]{
    auto capture=std::make_shared<Capture>(&retired,&q);
    result=q.invoke([capture=std::move(capture)]{ throw 7; });
    retiredAtReturn=retired.load();
  });
  check(reaches([&]{return q.pending()==1;}), "exception request queued");
  check(!q.pump(), "callback exception reported"); caller.join();
  check(!result&&retiredAtReturn, "exception recovers and destroys capture before wake");
  bool recovered=false;
  std::thread recovery([&]{recovered=q.invoke([&]{check(q.isRenderThread(),"recovered callback runs on render caller");});});
  check(reaches([&]{return q.pending()==1;}),"recovery queued after exception");
  check(q.pump(),"queue remains usable after callback exception");recovery.join();
  check(recovered,"recovered caller returns success");
  q.close();
}

void reentrantAndClose() {
  PresentWorkQueue q; check(q.bindCurrentThread(), "reentrant setup");
  std::atomic<bool> nested{true}, closedInside{false};
  bool result=false;
  std::thread caller([&]{ result=q.invoke([&]{
    nested=q.pump(); check(!q.invoke([]{}),"callback cannot synchronously enqueue itself");q.close(); closedInside=true;
  }); });
  check(reaches([&]{return q.pending()==1;}), "reentrant request queued");
  check(q.pump(), "active callback with close succeeds"); caller.join();
  check(result&&!nested&&closedInside, "reentrant pump rejected and close drains active");
  check(!q.invoke([]{}), "close inside callback closes admission");
}

void cancellationAndActiveClose() {
  PresentWorkQueue q; check(q.bindCurrentThread(), "cancellation setup");
  std::atomic<bool> ran{false}; bool result=false;
  std::thread caller([&]{ result=q.invoke([&]{ran=true;}, std::chrono::milliseconds(1)); });
  caller.join(); check(!result&&!ran, "queued timeout cancels without present"); check(q.pending()==0,"cancel retires queue slot");

  Event entered, release; std::atomic<bool> returned{false}; std::atomic<bool> closeSawBlocked{false};
  bool activeResult=false;
  std::thread activeCaller([&]{ activeResult=q.invoke([&]{ entered.signal(); check(release.wait(), "active callback release"); }, std::chrono::milliseconds(500)); returned=true; });
  check(reaches([&]{return q.pending()==1;}), "active request queued");
  std::thread closer([&]{
    check(entered.wait(), "active callback entered");
    const auto timeoutProof=std::chrono::steady_clock::now()+std::chrono::milliseconds(650);
    while (std::chrono::steady_clock::now()<timeoutProof) std::this_thread::yield();
    q.close(); closeSawBlocked=!returned.load(); release.signal();
  });
  check(q.pump(), "active callback starts");
  closer.join(); activeCaller.join();
  check(closeSawBlocked, "close does not detach active waiter after timeout");
  check(activeResult&&returned, "active callback finishes before waiter wake");
}

void capacity() {
  PresentWorkQueue q; check(q.bindCurrentThread(), "capacity setup");
  std::vector<std::thread> callers;
  std::atomic<unsigned> rejected{0}, completed{0}, ran{0};
  for (unsigned i=0;i<PresentWorkQueue::kQueueCapacity;++i)
    callers.emplace_back([&]{ if (!q.invoke([&]{++ran;}, std::chrono::seconds(2))) ++rejected; else ++completed; });
  check(reaches([&]{return q.pending()==PresentWorkQueue::kQueueCapacity;}), "sixteen queued requests admitted");
  std::thread extra([&]{ if (!q.invoke([]{}, std::chrono::milliseconds(1))) ++rejected; }); extra.join();
  check(rejected==1, "seventeenth request rejected");
  q.close(); for (auto& t:callers) t.join();
  check(q.pending()==0 && completed==0 && ran==0 && rejected==PresentWorkQueue::kQueueCapacity+1,
        "close cancels all sixteen queued requests without callbacks");
}

void quiescenceStopAndLateAcknowledgement() {
  std::unique_ptr<PresentQuiescence> gate;
  Event constructed, releasePresent, acknowledged;
  std::thread render([&] {
    gate = std::make_unique<PresentQuiescence>();
    auto& value = *gate; constructed.signal();
    check(releasePresent.wait(), "simulated Present returns before acknowledgement");
    check(value.acknowledge(true), "render acknowledgement accepted after Present");
    check(!value.acknowledge(true), "repeat acknowledgement rejected");
    acknowledged.signal();
  });
  check(constructed.wait(), "quiescence render object constructed");
  bool first = false;
  std::thread waiter([&] { first = gate->requestAndWait(std::chrono::milliseconds(20)); });
  check(reaches([&] { return gate->stopRequested(); }), "external stop remains requested");
  waiter.join(); check(!first && !gate->stopped(), "deadline returns false before Present acknowledgement");
  releasePresent.signal(); check(acknowledged.wait(), "late acknowledgement completed");
  check(gate->stopRequested() && gate->stopped(), "late acknowledgement permanently stops admission");
  check(gate->requestAndWait(std::chrono::milliseconds(1)), "repeated waiter observes successful outcome");
  render.join();
}

void quiescenceFailureAndWaiters() {
  std::unique_ptr<PresentQuiescence> gate; Event constructed, requested, release;
  std::atomic<bool> stopObserved{false};
  std::thread render([&] {
    gate=std::make_unique<PresentQuiescence>(); auto& value=*gate; constructed.signal();
    check(reaches([&] { return value.stopRequested(); }), "render observes permanent stop request");
    stopObserved = value.stopRequested();
    requested.signal();
    check(value.acknowledge(false), "failure acknowledgement accepted");
    check(!value.stopped(), "failure acknowledgement does not report stopped");
    release.wait();
  });
  check(constructed.wait(), "failure object constructed");
  bool one=true, two=true;
  std::thread a([&] { one=gate->requestAndWait(std::chrono::seconds(2)); });
  check(requested.wait(), "render saw request before failure acknowledgement");
  std::thread b([&] { two=gate->requestAndWait(std::chrono::seconds(2)); });
  a.join(); b.join(); release.signal(); render.join();
  check(!one && !two && stopObserved, "two waiters receive one failure outcome");
}

void quiescenceWrongThreadAndSelfWait() {
  std::unique_ptr<PresentQuiescence> gate; Event constructed, release;
  Event finished;
  std::thread render([&] {
    gate=std::make_unique<PresentQuiescence>(); auto& value=*gate;
    check(!value.acknowledge(true)&&!value.stopped(), "acknowledgement before stop request rejected");
    constructed.signal(); release.wait();
    check(!value.requestAndWait(std::chrono::milliseconds(1)), "bound render thread cannot wait");
    check(value.acknowledge(true), "bound render thread can acknowledge");
    finished.signal();
  });
  check(constructed.wait(), "self-wait object constructed");
  std::atomic<bool> wrongAck{true}; std::thread foreign([&] { wrongAck=gate->acknowledge(true); }); foreign.join();
  check(!wrongAck && !gate->stopRequested(), "wrong-thread acknowledgement rejected without requesting stop");
  check(!gate->requestAndWait(std::chrono::milliseconds(0))&&gate->stopRequested(),
    "zero deadline requests stop without a premature successful result");
  release.signal(); check(finished.wait(), "self-wait rejection leaves render thread usable"); render.join();
}

int selfTest() {
  Watchdog watchdog;
  basicAndOrdering(); exceptionAndCaptureLifetime(); reentrantAndClose();
  cancellationAndActiveClose(); capacity();
  quiescenceStopAndLateAcknowledgement(); quiescenceWrongThreadAndSelfWait();
  quiescenceFailureAndWaiters();
  std::printf("openxr_present_queue_test: %u checks, %u failures\n", checks.load(), failures.load());
  return failures ? 1 : 0;
}
}
int main(int argc, char** argv) {
  if (argc==2 && std::string(argv[1])=="--dry-run") {
    std::puts("Would test CPU-only OpenXR present work handoff; no threads, files, device or runtime created."); return 0;
  }
  if (argc==2 && std::string(argv[1])=="--self-test") return selfTest();
  std::fputs("usage: openxr_present_queue_test --dry-run|--self-test\n", stderr); return 2;
}
