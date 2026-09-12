#include "../../src/openxr/runtime_lifecycle.h"
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <mutex>
#include <thread>

using namespace edvr::openxr;
namespace {
unsigned checks=0,failures=0;
void check(bool value,const char* label) {
  ++checks;
  if(!value){++failures;std::printf("FAIL: %s\n",label);}
}
// A lock regression must fail the build instead of hanging indefinitely.
class Watchdog {
  std::mutex mutex_;std::condition_variable cv_;bool done_=false;
  std::thread thread_{[this]{
    std::unique_lock<std::mutex> lock(mutex_);
    if(!cv_.wait_for(lock,std::chrono::seconds(20),[this]{return done_;})) {
      std::fputs("FAIL: lifecycle test deadlock deadline\n",stderr);std::_Exit(1);
    }
  }};
 public:
  ~Watchdog() {
    {std::lock_guard<std::mutex> lock(mutex_);done_=true;cv_.notify_all();}
    thread_.join();
  }
};
const char* const versions[]={vr::IVRSystem_Version,vr::IVRCompositor_Version,
  vr::IVRChaperone_Version,vr::IVRExtendedDisplay_Version};

struct Backend final:RuntimeBackend {
  std::mutex mutex;std::condition_variable cv;
  bool blockStart=false,blockStop=false,enteredStart=false,enteredStop=false;
  bool complete=true,throwStart=false,stopSucceeds=true;
  vr::EVRInitError startResult=vr::VRInitError_None;
  std::atomic<unsigned> starts{0},stops{0};
  std::atomic<bool> cancelled{false},deadline{false},reentryGood{true};
  RuntimeLifecycle* owner=nullptr;
  // Opaque pointer identities are never dereferenced in this policy test.
  static RuntimeInterfaces bundle() {
    return {reinterpret_cast<vr::IVRSystem*>(uintptr_t(1)),
      reinterpret_cast<vr::IVRCompositor*>(uintptr_t(2)),
      reinterpret_cast<vr::IVRChaperone*>(uintptr_t(3)),
      reinterpret_cast<vr::IVRExtendedDisplay*>(uintptr_t(4))};
  }
  void enter(bool starting) {
    std::unique_lock<std::mutex> lock(mutex);
    (starting?enteredStart:enteredStop)=true;cv.notify_all();
    if(!cv.wait_for(lock,std::chrono::seconds(3),[&]{return !(starting?blockStart:blockStop);}))deadline=true;
  }
  bool await(bool starting) {
    std::unique_lock<std::mutex> lock(mutex);
    return cv.wait_for(lock,std::chrono::seconds(2),[&]{return starting?enteredStart:enteredStop;});
  }
  void release(bool starting) {
    std::lock_guard<std::mutex> lock(mutex);
    (starting?blockStart:blockStop)=false;cv.notify_all();
  }
  void reenter() {
    if(!owner)return;
    vr::EVRInitError error=vr::VRInitError_None;
    if(owner->token()==0||owner->running()||owner->getInterface(versions[0],&error)||
       error!=vr::VRInitError_Init_NotInitialized)reentryGood=false;
  }
  vr::EVRInitError start(uint32_t,const std::atomic<bool>& cancellation,RuntimeInterfaces& out)override {
    ++starts;reenter();enter(true);cancelled=cancellation.load();
    if(throwStart)throw 1;
    if(complete)out=bundle();
    return startResult;
  }
  bool stop()noexcept override {
    ++stops;reenter();enter(false);return stopSucceeds;
  }
};

void basicTests() {
  Backend backend;RuntimeLifecycle runtime(backend);backend.owner=&runtime;
  vr::EVRInitError error=vr::VRInitError_Unknown;
  check(runtime.token()==0&&!runtime.running(),"initial state");
  check(!runtime.init(&error,vr::VRApplication_Overlay)&&error==vr::VRInitError_Init_NotSupportedWithCompositor&&
    backend.starts==0&&runtime.token()==0,"non-scene rejected without mutation");
  check(!RuntimeLifecycle::valid(nullptr)&&!RuntimeLifecycle::valid("IVRSystem_012x")&&
    !RuntimeLifecycle::valid("ivrSystem_012")&&!RuntimeLifecycle::valid("IVRSystem_"),"invalid version shapes");
  for(const auto version:versions) {
    check(RuntimeLifecycle::valid(version),"supported exact version");
    check(!runtime.getInterface(version,&error)&&error==vr::VRInitError_Init_NotInitialized,"getter before init");
  }
  check(!runtime.getInterface(nullptr,&error)&&error==vr::VRInitError_Init_InvalidInterface,"null version");
  check(!runtime.getInterface("IVROverlay_011",&error)&&error==vr::VRInitError_Init_InterfaceNotFound,"unsupported getter");
  check(runtime.init(&error,vr::VRApplication_Scene)==1&&error==vr::VRInitError_None&&runtime.running(),"init publishes");
  const auto expected=Backend::bundle();
  void* identities[]={expected.system,expected.compositor,expected.chaperone,expected.display};
  for(unsigned i=0;i<4;++i)check(runtime.getInterface(versions[i],&error)==identities[i]&&error==vr::VRInitError_None,"exact owned identity");
  check(runtime.init(nullptr,vr::VRApplication_Scene)==1&&backend.starts==1,"repeat init stable without refcount");
  runtime.shutdown();
  check(!runtime.running()&&runtime.token()==2&&backend.stops==1,"shutdown invalidates once");
  for(const auto version:versions)check(!runtime.getInterface(version,&error)&&error==vr::VRInitError_Init_NotInitialized,"all getters retired");
  runtime.shutdown();check(backend.stops==1&&runtime.token()==2,"repeated shutdown idempotent");
  check(runtime.init(&error,vr::VRApplication_Scene)==3,"clean reinit");runtime.shutdown();
  check(backend.reentryGood&&!backend.deadline,"backend callbacks can read coordinator without lock recursion");
}

void failureTests() {
  for(unsigned mode=0;mode<3;++mode) {
    Backend backend;RuntimeLifecycle runtime(backend);backend.owner=&runtime;
    if(mode==0)backend.complete=false;
    if(mode==1)backend.throwStart=true;
    if(mode==2)backend.startResult=vr::VRInitError_Init_HmdNotFound;
    vr::EVRInitError error=vr::VRInitError_None;
    const auto expected=mode==0?vr::VRInitError_Init_InterfaceNotFound:
      mode==1?vr::VRInitError_Init_Internal:vr::VRInitError_Init_HmdNotFound;
    check(!runtime.init(&error,vr::VRApplication_Scene)&&error==expected,"partial/throw/error init fails accurately");
    check(backend.starts==1&&backend.stops==1&&!runtime.running()&&runtime.token()==1,"failed start cleaned exactly once");
    for(const auto version:versions)check(!runtime.getInterface(version,nullptr),"failure never publishes partial table");
    backend.complete=true;backend.throwStart=false;backend.startResult=vr::VRInitError_None;
    check(runtime.init(nullptr,vr::VRApplication_Scene)==2,"retry after clean failure");runtime.shutdown();
    check(backend.reentryGood&&!backend.deadline,"failure callbacks outside lock");
  }
  for(bool failedStart:{false,true}) {
    Backend backend;backend.stopSucceeds=false;backend.complete=!failedStart;
    RuntimeLifecycle runtime(backend);vr::EVRInitError error=vr::VRInitError_None;
    const auto first=runtime.init(&error,vr::VRApplication_Scene);
    check(failedStart?!first:first==1,"unclean-stop setup");
    runtime.shutdown();
    check(!runtime.init(&error,vr::VRApplication_Scene)&&error==vr::VRInitError_Init_Internal&&backend.starts==1,"unclean stop permanently blocks restart");
    runtime.shutdown();check(backend.stops==1&&!runtime.getInterface(versions[0],nullptr),"unclean stop not repeated or exposed");
  }
}

void cancellationTests() {
  Backend backend;backend.blockStart=true;RuntimeLifecycle runtime(backend);backend.owner=&runtime;
  uint32_t result=99;vr::EVRInitError workerError=vr::VRInitError_None,error=vr::VRInitError_None;
  std::thread initializer([&]{result=runtime.init(&workerError,vr::VRApplication_Scene);});
  check(backend.await(true),"startup entered bounded blocked backend");
  check(!runtime.init(&error,vr::VRApplication_Scene)&&error==vr::VRInitError_Init_Retry,"concurrent init retries");
  check(runtime.token()==1&&!runtime.running()&&!runtime.getInterface(versions[0],&error)&&
    error==vr::VRInitError_Init_NotInitialized,"startup reads see no incomplete interfaces");
  runtime.shutdown();
  check(runtime.token()==2&&backend.stops==0,"shutdown cancels without racing startup destruction");
  check(!runtime.init(&error,vr::VRApplication_Scene)&&error==vr::VRInitError_Init_ShuttingDown,"cancelled startup still owns cleanup");
  runtime.shutdown();check(runtime.token()==2&&backend.stops==0,"repeat cancellation idempotent");
  backend.release(true);initializer.join();
  check(result==0&&workerError==vr::VRInitError_Init_ShuttingDown&&backend.cancelled&&backend.stops==1,"initializer cleans cancelled start");
  check(runtime.init(nullptr,vr::VRApplication_Scene)==3,"restart after cancellation cleanup");
  {std::lock_guard<std::mutex> lock(backend.mutex);backend.blockStop=true;backend.enteredStop=false;}
  std::thread stopper([&]{runtime.shutdown();});
  check(backend.await(false),"shutdown entered bounded blocked backend");
  check(runtime.token()==4&&!runtime.running()&&!runtime.getInterface(versions[0],&error),"shutdown retires before backend returns");
  check(!runtime.init(&error,vr::VRApplication_Scene)&&error==vr::VRInitError_Init_ShuttingDown,"init rejected during stop");
  runtime.shutdown();check(backend.stops==2,"concurrent shutdown does not duplicate cleanup");
  backend.release(false);stopper.join();
  check(backend.reentryGood&&!backend.deadline,"concurrent callbacks completed without deadline");
}

void overflowTests() {
  Backend backend;RuntimeLifecycle runtime(backend,UINT32_MAX-2);vr::EVRInitError error=vr::VRInitError_None;
  check(runtime.init(&error,vr::VRApplication_Scene)==UINT32_MAX-1,"last usable init token");runtime.shutdown();
  check(runtime.token()==UINT32_MAX,"last shutdown token preserved");
  check(!runtime.init(&error,vr::VRApplication_Scene)&&error==vr::VRInitError_Init_TooManyObjects&&backend.starts==1,"token never wraps");
}
}
int main(int argc,char** argv) {
  if(argc!=2)return 2;
  if(std::strcmp(argv[1],"--dry-run")==0){std::puts("openxr_lifecycle_test: dry-run (no runtime, threads or writes)");return 0;}
  if(std::strcmp(argv[1],"--self-test")!=0)return 2;
  Watchdog watchdog;basicTests();failureTests();cancellationTests();overflowTests();
  std::printf("openxr_lifecycle_test: %u checks, %u failures\n",checks,failures);return failures?1:0;
}
