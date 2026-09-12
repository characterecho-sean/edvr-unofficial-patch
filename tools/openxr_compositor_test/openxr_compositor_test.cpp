#include "../../src/openxr/openvr_compositor.h"
#include "../../src/openxr/compositor_publication.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <limits>
#include <mutex>
#include <thread>

using namespace edvr::openxr;
extern "C" vr::IVRCompositor* compositorAbiCaller(vr::IVRCompositor*);
namespace {
unsigned checks=0,failures=0;
void check(bool yes,const char* what){++checks;if(!yes){++failures;std::printf("FAIL: %s\n",what);}}
vr::TrackedDevicePose_t pose(float x) {
  vr::TrackedDevicePose_t p{};for(unsigned i=0;i<3;++i)p.mDeviceToAbsoluteTracking.m[i][i]=1;
  p.mDeviceToAbsoluteTracking.m[0][3]=x;p.vVelocity={{x+1,x+2,x+3}};p.vAngularVelocity={{x+4,x+5,x+6}};
  p.bDeviceIsConnected=p.bPoseIsValid=true;p.eTrackingResult=vr::TrackingResult_Running_OK;return p;
}
bool invalid(const vr::TrackedDevicePose_t& p,bool connected=false) {
  if(p.bPoseIsValid||p.bDeviceIsConnected!=connected||p.eTrackingResult!=(connected?vr::TrackingResult_Running_OutOfRange:vr::TrackingResult_Uninitialized))return false;
  for(unsigned i=0;i<3;++i){if(p.vVelocity.v[i]!=0||p.vAngularVelocity.v[i]!=0)return false;
    for(unsigned j=0;j<4;++j)if(p.mDeviceToAbsoluteTracking.m[i][j]!=(i==j?1.f:0.f))return false;}
  return true;
}
bool equal(const vr::TrackedDevicePose_t& a,const vr::TrackedDevicePose_t& b) {
  return a.bPoseIsValid==b.bPoseIsValid&&a.bDeviceIsConnected==b.bDeviceIsConnected&&a.eTrackingResult==b.eTrackingResult&&
    !std::memcmp(&a.mDeviceToAbsoluteTracking,&b.mDeviceToAbsoluteTracking,sizeof(a.mDeviceToAbsoluteTracking))&&
    !std::memcmp(&a.vVelocity,&b.vVelocity,sizeof(a.vVelocity))&&!std::memcmp(&a.vAngularVelocity,&b.vAngularVelocity,sizeof(a.vAngularVelocity));
}
struct Fake : CompositorSource {
  CompositorRead state{};
  std::atomic<unsigned> waits{0};unsigned reads=0,sets=0,submits=0,clears=0,handoffs=0,diagnostics[29]{};
  bool setOk=true,clearOk=true,handoffOk=true,wrongGeneration=false;
  vr::EVRCompositorError waitResult=vr::VRCompositorError_None,submitResult=vr::VRCompositorError_None;
  uint64_t seenGeneration=0;vr::EVREye seenEye=vr::Eye_Left;const vr::Texture_t* seenTexture=nullptr;
  const vr::VRTextureBounds_t* seenBounds=nullptr;vr::EVRSubmitFlags seenFlags=vr::Submit_Default;
  std::mutex blockMutex;std::condition_variable cv;bool block=false,entered=false,release=false,timedOut=false;
  Fake() {state.generation=7;state.originGeneration=3;state.sequence=10;state.connected=state.posesAvailable=true;
    state.renderPose=pose(1);state.gamePose=pose(2);state.renderTime=100;state.gameTime=111;state.canRenderKnown=state.canRender=true;}
  CompositorRead compositorRead() const override {return state;} // immutable while threaded fixture runs
  vr::EVRCompositorError waitPoses(uint64_t generation,CompositorRead& out) override {
    ++waits;seenGeneration=generation;
    if(block){std::unique_lock<std::mutex> lock(blockMutex);entered=true;cv.notify_all();
      if(!cv.wait_for(lock,std::chrono::seconds(3),[&]{return release;})){timedOut=true;return vr::VRCompositorError_InvalidTexture;}}
    out=state;if(wrongGeneration)++out.generation;return waitResult;
  }
  bool setTrackingSpace(uint64_t generation,vr::ETrackingUniverseOrigin origin) override {
    ++sets;seenGeneration=generation;if(!setOk)return false;state.origin=origin;return true;
  }
  vr::EVRCompositorError submitEye(uint64_t generation,vr::EVREye eye,const vr::Texture_t* texture,
      const vr::VRTextureBounds_t* bounds,vr::EVRSubmitFlags flags) override {
    ++submits;seenGeneration=generation;seenEye=eye;seenTexture=texture;seenBounds=bounds;seenFlags=flags;return submitResult;
  }
  bool clearSubmitted(uint64_t generation) override {++clears;seenGeneration=generation;return clearOk;}
  bool handoff(uint64_t generation) override {++handoffs;seenGeneration=generation;return handoffOk;}
  void compositorUnsupported(unsigned slot) noexcept override {if(slot<29)++diagnostics[slot];}
};
void buffersAndCache() {
  Fake f;OpenVRCompositor object(&f);auto* c=compositorAbiCaller(&object);
  struct Guarded {uint64_t before;vr::TrackedDevicePose_t poses[17];uint64_t after;} render,game;
  const auto reset=[&]{std::memset(&render,0xA5,sizeof(render));std::memset(&game,0xA5,sizeof(game));};
  const auto guards=[&] {return render.before==0xA5A5A5A5A5A5A5A5ULL&&render.after==render.before&&game.before==render.before&&game.after==render.before;};
  for(unsigned rc:{0u,1u,16u})for(unsigned gc:{0u,1u,16u}) {
    reset();auto r=render,g=game;const auto oldWaits=f.waits.load();
    check(c->WaitGetPoses(rc?render.poses:nullptr,rc,gc?game.poses:nullptr,gc)==vr::VRCompositorError_None&&f.waits==oldWaits+1&&f.seenGeneration==7,"ABI waits once with exact generation");
    if(rc)check(equal(render.poses[0],f.state.renderPose),"independent render pose and velocities");
    if(gc)check(equal(game.poses[0],f.state.gamePose),"independent gameplay pose and velocities");
    for(unsigned i=1;i<rc;++i)check(invalid(render.poses[i]),"non-HMD render disconnected");
    for(unsigned i=1;i<gc;++i)check(invalid(game.poses[i]),"non-HMD gameplay disconnected");
    check(!std::memcmp(render.poses+rc,r.poses+rc,sizeof(render.poses)-rc*sizeof(render.poses[0]))&&
      !std::memcmp(game.poses+gc,g.poses+gc,sizeof(game.poses)-gc*sizeof(game.poses[0]))&&guards(),"no writes beyond capacities");
  }
  for(bool last:{false,true}) {
    reset();const auto r=render;const auto oldWaits=f.waits.load();
    auto result=last?c->GetLastPoses(render.poses,17,nullptr,0):c->WaitGetPoses(render.poses,17,nullptr,0);
    check(result==vr::VRCompositorError_InvalidTexture&&f.waits==oldWaits&&invalid(render.poses[0])&&
      !std::memcmp(&render.poses[16],&r.poses[16],sizeof(render.poses[16]))&&guards(),"oversized count rejected with bounded initialization");
    reset();result=last?c->GetLastPoses(nullptr,1,game.poses,1):c->WaitGetPoses(nullptr,1,game.poses,1);
    check(result==vr::VRCompositorError_InvalidTexture&&invalid(game.poses[0])&&f.waits==oldWaits&&guards(),"null positive count initializes other buffer but never waits");
  }
  reset();const auto oldWaits=f.waits.load();
  for(unsigned n=0;n<3;++n)check(c->GetLastPoses(render.poses,16,game.poses,1)==vr::VRCompositorError_None&&
    equal(render.poses[0],f.state.renderPose)&&equal(game.poses[0],f.state.gamePose)&&f.waits==oldWaits,"cached reads never wait");
  check(c->GetLastPoses(render.poses,1,render.poses,1)==vr::VRCompositorError_None&&equal(render.poses[0],f.state.gamePose),"aliased outputs use final gameplay write");
  f.wrongGeneration=true;check(c->WaitGetPoses(render.poses,1,game.poses,1)==vr::VRCompositorError_InvalidTexture&&invalid(render.poses[0])&&invalid(game.poses[0]),"wrong generation cannot publish valid poses");f.wrongGeneration=false;
  f.waitResult=vr::VRCompositorError_DoNotHaveFocus;check(c->WaitGetPoses(render.poses,1,game.poses,1)==f.waitResult&&invalid(render.poses[0]),"source wait failure preserved");f.waitResult=vr::VRCompositorError_None;
  f.state.renderPose.bPoseIsValid=false;f.state.renderPose.eTrackingResult=vr::TrackingResult_Running_OK;
  f.state.renderPose.vVelocity.v[0]=std::numeric_limits<float>::quiet_NaN();
  check(c->GetLastPoses(render.poses,1,game.poses,1)==vr::VRCompositorError_None&&invalid(render.poses[0],true)&&equal(game.poses[0],f.state.gamePose),"invalid tracking clears stale pose and Running_OK independently");
  f.state.posesAvailable=false;check(c->GetLastPoses(render.poses,1,game.poses,1)==vr::VRCompositorError_InvalidTexture&&invalid(render.poses[0],true),"live unavailable cache is connected invalid");
  for(unsigned index:{0u,15u,16u}) {
    check(c->GetLastPoseForTrackedDeviceIndex(index,&render.poses[0],&game.poses[0])==
      (index==16?vr::VRCompositorError_IndexOutOfRange:vr::VRCompositorError_None)&&invalid(render.poses[0],index==0),"single-device index and unavailable policy");
    check(c->GetLastPoseForTrackedDeviceIndex(index,nullptr,nullptr)==(index==16?vr::VRCompositorError_IndexOutOfRange:vr::VRCompositorError_None),"single-device optional outputs");
  }
  f.state.generation=0;f.state.posesAvailable=true;check(c->GetLastPoses(render.poses,1,nullptr,0)==vr::VRCompositorError_InvalidTexture&&invalid(render.poses[0]),"retired generation cannot expose poses");
}
void methods() {
  Fake f;OpenVRCompositor object(&f);auto* c=compositorAbiCaller(&object);
  for(auto origin:{vr::TrackingUniverseStanding,vr::TrackingUniverseRawAndUncalibrated,vr::TrackingUniverseSeated}) {
    c->SetTrackingSpace(origin);check(c->GetTrackingSpace()==origin&&f.seenGeneration==7,"accepted origin comes from source");
  }
  f.setOk=false;c->SetTrackingSpace(vr::TrackingUniverseStanding);check(c->GetTrackingSpace()==vr::TrackingUniverseSeated&&f.diagnostics[0]==1,"failed origin retains previous space");
  const auto sets=f.sets;c->SetTrackingSpace(vr::ETrackingUniverseOrigin(99));check(f.sets==sets&&f.diagnostics[0]==1,"invalid origin never dispatched");
  vr::Texture_t texture{reinterpret_cast<void*>(123),vr::API_DirectX,vr::ColorSpace_Linear};vr::VRTextureBounds_t bounds{.8f,1,.1f,0};
  for(auto eye:{vr::Eye_Right,vr::Eye_Left}) {
    f.submitResult=vr::VRCompositorError_TextureIsOnWrongDevice;
    check(c->Submit(eye,&texture,&bounds,vr::Submit_GlRenderBuffer)==f.submitResult&&f.seenEye==eye&&f.seenTexture==&texture&&
      f.seenBounds==&bounds&&f.seenFlags==vr::Submit_GlRenderBuffer&&f.seenGeneration==7,"submit ABI preserves complete arguments and result");
  }
  const auto submits=f.submits;check(c->Submit(vr::EVREye(2),&texture)==vr::VRCompositorError_IndexOutOfRange&&f.submits==submits,"invalid eye rejected before source");
  c->ClearLastSubmittedFrame();c->PostPresentHandoff();check(f.clears==1&&f.handoffs==1&&f.seenGeneration==7,"clear and handoff callbacks");
  f.clearOk=f.handoffOk=false;c->ClearLastSubmittedFrame();c->ClearLastSubmittedFrame();c->PostPresentHandoff();c->PostPresentHandoff();
  check(f.diagnostics[6]==1&&f.diagnostics[7]==1,"partial clear and unavailable handoff diagnosed once");
  struct Timing {uint64_t before[2];vr::Compositor_FrameTiming timing;uint64_t after[2];} timing;
  for(unsigned size:{0u,4u,unsigned(sizeof(timing.timing)),unsigned(sizeof(timing.timing)+64)}) {
    std::memset(&timing,0xCB,sizeof(timing));timing.timing.m_nSize=size;const auto before=timing;
    check(!c->GetFrameTiming(&timing.timing,99)&&!std::memcmp(&timing,&before,sizeof(timing)),"unavailable timing leaves complete caller buffer untouched");
  }
  check(!c->GetFrameTiming(nullptr)&&c->GetFrameTimeRemaining()==0,"null timing and no invented duration");
  check(c->CanRenderScene(),"explicit known focus usable");f.state.connected=false;check(!c->CanRenderScene(),"disconnected cannot render");f.state.connected=true;f.state.canRenderKnown=false;
  for(unsigned n=0;n<2;++n) {
    c->FadeToColor(.5f,.2f,.3f,.4f,.8f,true);c->FadeGrid(.5f,true);
    check(c->SetSkyboxOverride(&texture,6)==vr::VRCompositorError_InvalidTexture,"six-face skybox never fakes success");
    c->ClearSkyboxOverride();c->CompositorBringToFront();c->CompositorGoToBack();c->CompositorQuit();
    check(!c->IsFullscreen()&&!c->GetCurrentSceneFocusProcess()&&!c->GetLastFrameRenderer()&&!c->CanRenderScene(),"unavailable window and process metadata");
    c->ShowMirrorWindow();c->HideMirrorWindow();check(!c->IsMirrorWindowVisible(),"no invented mirror window");
    c->CompositorDumpImages();check(!c->ShouldAppRenderWithLowResources(),"unavailable resource hint");
    c->ForceInterleavedReprojectionOn(true);c->ForceReconnectProcess();c->SuspendRendering(true);
    c->GetFrameTiming(nullptr);c->GetFrameTimeRemaining();
  }
  for(unsigned slot=8;slot<29;++slot)check(f.diagnostics[slot]==1,"each unsupported historical slot diagnosed once");
  f.state.generation=0;check(c->Submit(vr::Eye_Left,&texture)==vr::VRCompositorError_InvalidTexture&&f.submits==submits,"retired source cannot submit");
}
void publication() {
  CompositorPublication p;check(!p.begin(vr::ETrackingUniverseOrigin(42)),"invalid initial origin rejected");const auto gen=p.begin();
  check(gen&&!p.begin(),"publication one live generation");auto candidate=p.read();candidate.sequence=1;candidate.posesAvailable=true;candidate.renderPose=pose(3);candidate.gamePose=pose(4);candidate.renderTime=12;candidate.gameTime=15;
  p.focus(gen,true,true);check(p.publish(candidate)&&p.read().canRender,"frame preserves latest focus");
  check(!p.publish(candidate),"duplicate sequence rejected");auto copy=p.read();copy.renderPose=pose(99);check(p.read().renderPose.mDeviceToAbsoluteTracking.m[0][3]==3,"snapshot is a copy");
  candidate.sequence=2;check(p.changeOrigin(gen,vr::TrackingUniverseStanding)&&!p.read().posesAvailable,"accepted origin invalidates both cached predictions");
  check(!p.publish(candidate),"old origin generation cannot republish poses");candidate.origin=vr::TrackingUniverseStanding;candidate.originGeneration=p.read().originGeneration;
  check(p.publish(candidate),"new origin frame published");p.invalidate(gen);check(!p.read().posesAvailable&&p.read().renderTime==0&&p.read().gameTime==0,"invalidate clears both pose times");
  p.retire(gen);check(!p.read().generation&&!p.read().connected,"retirement hides old generation");const auto next=p.begin();check(next>gen&&!p.publish(candidate),"reinit rejects old publisher");
  p.invalidate(gen);p.retire(gen);check(p.read().generation==next,"stale retirement and invalidation harmless");
}
void blocked() {
  Fake f;f.block=true;OpenVRCompositor object(&f);auto* c=compositorAbiCaller(&object);
  bool waited=false;std::thread worker([&]{vr::TrackedDevicePose_t r{},g{};waited=c->WaitGetPoses(&r,1,&g,1)==vr::VRCompositorError_None&&equal(r,f.state.renderPose)&&equal(g,f.state.gamePose);});
  {std::unique_lock<std::mutex> lock(f.blockMutex);check(f.cv.wait_for(lock,std::chrono::seconds(2),[&]{return f.entered;}),"source wait entered");}
  vr::TrackedDevicePose_t r{},g{};check(c->GetLastPoses(&r,1,&g,1)==vr::VRCompositorError_None&&equal(r,f.state.renderPose)&&equal(g,f.state.gamePose)&&f.waits==1,"cached ABI getters proceed during blocked wait");
  {std::lock_guard<std::mutex> lock(f.blockMutex);f.release=true;f.cv.notify_all();}worker.join();check(waited&&!f.timedOut,"blocked wait returns coherent snapshot");
}
}
int main(int argc,char** argv) {
  if(argc!=2)return 2;
  if(!std::strcmp(argv[1],"--dry-run")){std::puts("Would test owned compositor ABI and cache; no files or runtime calls.");return 0;}
  if(std::strcmp(argv[1],"--self-test"))return 2;
  buffersAndCache();methods();publication();blocked();std::printf("openxr_compositor_test: %u checks, %u failures\n",checks,failures);return failures?1:0;
}
