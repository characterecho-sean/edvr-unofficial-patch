#include "../../src/openxr/owner_service.h"
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
using edvr::openxr::OwnerService;
using Clock=std::chrono::steady_clock;
namespace {
unsigned checks=0,failures=0;
void check(bool value,const char* label) {
  ++checks;if(!value){++failures;std::printf("FAIL: %s\n",label);}
}
// Test assertions are made on the main test thread only. Cross-thread results
// use atomics, mutexes, or the invoke/join completion barrier.
struct Gate {
  std::mutex mutex;std::condition_variable cv;bool entered=false,released=false;
  std::atomic<bool> expired{false};
  void hold() {
    std::unique_lock<std::mutex> lock(mutex);entered=true;cv.notify_all();
    if(!cv.wait_for(lock,std::chrono::seconds(3),[&]{return released;}))expired=true;
  }
  bool await() {
    std::unique_lock<std::mutex> lock(mutex);
    return cv.wait_for(lock,std::chrono::seconds(2),[&]{return entered;});
  }
  void release(){std::lock_guard<std::mutex> lock(mutex);released=true;cv.notify_all();}
};
template<class Predicate> bool until(Predicate predicate) {
  const auto deadline=Clock::now()+std::chrono::seconds(2);
  while(!predicate()) {
    if(Clock::now()>=deadline)return false;
    std::this_thread::yield();
  }
  return true;
}
class Watchdog {
  std::mutex mutex;std::condition_variable cv;bool done=false;
  std::thread worker{[this]{
    std::unique_lock<std::mutex> lock(mutex);
    if(!cv.wait_for(lock,std::chrono::seconds(20),[&]{return done;})) {
      std::fputs("FAIL: owner service test deadlock deadline\n",stderr);std::_Exit(1);
    }
  }};
 public:
  ~Watchdog(){ {std::lock_guard<std::mutex> lock(mutex);done=true;cv.notify_all();} worker.join();}
};

void basicAndIdle() {
  OwnerService service;
  std::atomic<unsigned> idleCalls{0};std::atomic<bool> idleOwner{true},idleWithQueuedWork{false};
  check(!service.running()&&!service.isOwner()&&!service.invoke([]{}),"unstarted rejects calls");
  check(service.start([&]{
    if(!service.isOwner())idleOwner=false;
    if(service.pending())idleWithQueuedWork=true;
    ++idleCalls;
  }),"start owner");
  check(!service.start(),"double start rejected");
  check(until([&]{return idleCalls.load()>0;}),"idle callback without incoming requests");
  std::thread::id ownerId;bool owner=false,nested=false,nestedOwner=false;
  check(service.invoke([&]{ownerId=std::this_thread::get_id();owner=service.isOwner();
    nested=service.invoke([&]{nestedOwner=service.isOwner();});}),"invoke and inline reentry return");
  check(owner&&nested&&nestedOwner&&ownerId!=std::this_thread::get_id(),"actual owner differs from caller");
  check(!service.invoke({})&&!service.invoke([]{throw 7;}),"empty and throwing callbacks fail");
  int afterException=0;check(service.invoke([&]{afterException=7;})&&afterException==7,"exception does not poison next call");
  Gate hold;bool blockedResult=false;
  std::thread blocker([&]{blockedResult=service.invoke([&]{hold.hold();});});
  check(hold.await(),"block owner for deterministic queue");
  std::array<bool,4> results{};std::atomic<unsigned> active{0},maxActive{0},calls{0};
  std::vector<std::thread> callers;
  for(unsigned i=0;i<results.size();++i)callers.emplace_back([&,i]{results[i]=service.invoke([&]{
    const auto current=++active;auto high=maxActive.load();
    while(current>high&&!maxActive.compare_exchange_weak(high,current)){}
    // Long enough to pass one idle cadence, while other work remains queued.
    const auto finish=Clock::now()+std::chrono::milliseconds(7);
    while(Clock::now()<finish)std::this_thread::yield();
    ++calls;--active;
  });});
  check(until([&]{return service.pending()==results.size();}),"all competing calls queued before release");
  hold.release();blocker.join();for(auto& thread:callers)thread.join();
  check(blockedResult&&!hold.expired,"blocked caller completes after release");
  for(bool result:results)check(result,"each concurrent caller completes");
  check(calls==4&&maxActive==1,"callbacks execute exactly once and serially");
  check(idleWithQueuedWork&&idleOwner,"event pump is not starved by queued work");
  bool finalizedOnOwner=false;
  check(service.stop([&]{finalizedOnOwner=service.isOwner();}),"stop joins");
  check(finalizedOnOwner&&!service.running()&&!service.isOwner(),"finalizer owns thread then identity retires");
}

void cancellationAndJoin() {
  OwnerService service;check(service.start(),"cancellation setup");
  Gate hold;std::atomic<bool> activeFinished{false};bool activeResult=false;
  std::thread active([&]{activeResult=service.invoke([&]{hold.hold();activeFinished=true;});});
  check(hold.await(),"active callback remains blocked");
  std::atomic<unsigned> queuedExecutions{0},queuedReturns{0};
  std::array<bool,OwnerService::kQueueCapacity> queuedResults{};
  std::vector<std::thread> queued;
  for(unsigned i=0;i<queuedResults.size();++i)queued.emplace_back([&,i]{
    queuedResults[i]=service.invoke([&]{++queuedExecutions;});++queuedReturns;
  });
  check(until([&]{return service.pending()==OwnerService::kQueueCapacity;}),"queue exactly full while active blocked");
  check(!service.invoke([&]{++queuedExecutions;}),"next callback rejected without execution");
  std::atomic<unsigned> finalizers{0};bool finalizedOnOwner=false,finalizedAfterActive=false,finalizerInvoke=true;
  bool firstStop=false,secondStop=false;
  std::thread first([&]{firstStop=service.stop([&]{
    ++finalizers;finalizedOnOwner=service.isOwner();finalizedAfterActive=activeFinished;
    finalizerInvoke=service.invoke([]{});
  });});
  check(until([&]{return !service.running();}),"stop closes admission before active finishes");
  check(until([&]{return queuedReturns==OwnerService::kQueueCapacity;}),"queued callers cancelled while active still blocked");
  check(!activeFinished&&finalizers==0&&service.pending()==0,"no cleanup before active callback finishes");
  check(!service.start()&&!service.invoke([]{}),"restart and invocation rejected during stop");
  std::atomic<bool> secondEntered{false};
  std::thread second([&]{secondEntered=true;secondStop=service.stop([&]{++finalizers;});});
  check(until([&]{return secondEntered.load();}),"second external stop caller launched");
  hold.release();active.join();for(auto& thread:queued)thread.join();first.join();second.join();
  for(bool result:queuedResults)check(!result,"cancelled callback returned false");
  check(activeResult&&activeFinished&&!hold.expired&&queuedExecutions==0,"active finishes but cancelled callbacks never execute");
  check(firstStop&&secondStop&&finalizers==1,"concurrent stoppers share one finalizer");
  check(finalizedOnOwner&&finalizedAfterActive&&!finalizerInvoke,"owner cleanup follows active work with admission closed");
  check(service.stop()&&!service.isOwner(),"repeated stop preserves result and retired identity");
  check(service.start(),"restart after all joins");
  unsigned newGenerationCalls=0;
  check(service.invoke([&]{++newGenerationCalls;})&&newGenerationCalls==1,"new generation executes fresh work");
  check(service.stop(),"restart clean shutdown");
}

void selfStopAndFailures() {
  OwnerService service;check(service.start(),"owner stop setup");
  bool ownerStop=true,finalOwner=false;unsigned finalizers=0;
  check(service.invoke([&]{ownerStop=service.stop([&]{++finalizers;finalOwner=service.isOwner();});}),"owner can request stop inside active callback");
  check(!ownerStop&&!service.running()&&!service.invoke([]{}),"owner request never self-joins");
  check(service.stop([&]{++finalizers;})&&finalizers==1&&finalOwner,"external join retains original finalizer");
  check(service.start(),"throwing finalizer setup");
  check(!service.stop([]{throw 1;}),"finalizer exception contained and reported");
  check(!service.stop()&&!service.running(),"failed finalizer result retained across repeated stop");
  check(service.start()&&service.stop(),"successful restart resets failure outcome");
  std::atomic<bool> idleEntered{false};
  check(service.start([&]{idleEntered=true;throw 1;}),"throwing idle setup");
  check(until([&]{return idleEntered.load();}),"idle failure actually exercised");
  check(!service.stop(),"idle failure reported at join");
}
}
int main(int argc,char** argv) {
  if(argc!=2)return 2;
  if(std::strcmp(argv[1],"--dry-run")==0){std::puts("openxr_owner_test: dry-run (no threads, runtime or writes)");return 0;}
  if(std::strcmp(argv[1],"--self-test")!=0)return 2;
  Watchdog watchdog;basicAndIdle();cancellationAndJoin();selfStopAndFailures();
  std::printf("openxr_owner_test: %u checks, %u failures\n",checks,failures);return failures?1:0;
}
