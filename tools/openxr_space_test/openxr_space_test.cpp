#include "../../src/openxr/seated_space.h"
#include "../../src/openxr/reset_events.h"
#include "../../src/openxr/system_publication.h"
#include "../../src/openxr/compositor_publication.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace edvr::openxr;
namespace {
unsigned checks=0,failures=0;
void check(bool ok,const char* label){++checks;if(!ok){++failures;std::printf("FAIL: %s\n",label);}}
const XrSession session=reinterpret_cast<XrSession>(1);
const XrSpace local=reinterpret_cast<XrSpace>(2);
struct Fake {
  XrResult createResult=XR_SUCCESS,destroyResult=XR_SUCCESS;
  bool nullOutput=false,consumeFailedDestroy=true;uintptr_t next=10;
  XrPosef expected{{0,0,0,1},{1,2,3}};
  unsigned creates=0;std::vector<XrSpace> alive,destroyed;
};
Fake* fake=nullptr;
XrResult XRAPI_PTR create(XrSession supplied,const XrReferenceSpaceCreateInfo* info,XrSpace* out) {
  ++fake->creates;
  check(supplied==session&&info&&info->type==XR_TYPE_REFERENCE_SPACE_CREATE_INFO&&!info->next,"create typed session ABI");
  check(info->referenceSpaceType==XR_REFERENCE_SPACE_TYPE_LOCAL,"create uses natural LOCAL");
  check(std::memcmp(&info->poseInReferenceSpace,&fake->expected,sizeof(XrPosef))==0,"exact origin passed");
  if(fake->createResult==XR_SUCCESS||fake->createResult==XR_SESSION_LOSS_PENDING) {
    *out=fake->nullOutput?XR_NULL_HANDLE:reinterpret_cast<XrSpace>(fake->next++);
    if(*out)fake->alive.push_back(*out);
  } else *out=reinterpret_cast<XrSpace>(99); // failure outputs never transfer ownership
  return fake->createResult;
}
XrResult XRAPI_PTR destroy(XrSpace space) {
  check(space!=local,"borrowed LOCAL never destroyed");
  const auto found=std::find(fake->alive.begin(),fake->alive.end(),space);
  check(found!=fake->alive.end(),"destroy a live owned space once");
  if(found!=fake->alive.end()&&(fake->destroyResult==XR_SUCCESS||fake->consumeFailedDestroy))fake->alive.erase(found);
  fake->destroyed.push_back(space);return fake->destroyResult;
}
void spaces() {
  Fake runtime;fake=&runtime;SeatedSpace owned;
  check(!owned.begin({create,destroy},nullptr,local),"null session rejected");
  check(!owned.begin({create,destroy},session,nullptr),"null local rejected");
  check(!owned.begin({nullptr,destroy},session,local),"missing create rejected");
  check(!owned.begin({create,nullptr},session,local),"missing destroy rejected");
  check(owned.begin({create,destroy},session,local)&&owned.space()==local,"starts with borrowed LOCAL");
  check(!owned.begin({create,destroy},session,local),"double begin rejected");
  XrPosef bad=runtime.expected;bad.orientation.w=2;
  check(owned.replace(bad)==XR_ERROR_POSE_INVALID&&owned.space()==local&&runtime.creates==0,"bad pose no runtime mutation");
  check(owned.replace(runtime.expected)==XR_SUCCESS,"first reset");
  const auto first=owned.space();
  check(first!=local&&first&&runtime.alive.size()==1&&runtime.destroyed.empty(),"first reset ownership");
  runtime.expected.position={4,5,6};
  check(owned.replace(runtime.expected)==XR_SUCCESS,"second reset");
  check(owned.space()!=first&&runtime.alive.size()==1&&runtime.destroyed==std::vector<XrSpace>{first},"replace destroys old child only");
  check(owned.shutdown()==XR_SUCCESS&&!owned.space()&&runtime.alive.empty(),"shutdown final child");
  check(owned.shutdown()==XR_SUCCESS&&runtime.destroyed.size()==2,"idempotent shutdown");
  check(owned.begin({create,destroy},session,local)&&owned.space()==local,"reinit borrowed state");
  owned.shutdown();

  for(XrResult result:{XR_ERROR_RUNTIME_FAILURE,XR_ERROR_SESSION_LOST,XR_SESSION_LOSS_PENDING}) {
    Fake failure;fake=&failure;SeatedSpace item;item.begin({create,destroy},session,local);
    check(item.replace(failure.expected)==XR_SUCCESS,"failure fixture first child");
    failure.createResult=result;
    check(item.replace(failure.expected)==result&&!item.space(),"creation failure retires origin");
    const auto calls=failure.creates;
    check(item.replace(failure.expected)==XR_ERROR_CALL_ORDER_INVALID&&failure.creates==calls,"retired origin no retry");
    check(item.shutdown()==XR_SUCCESS&&failure.alive.empty(),"cleanup owned and pending handles");
    check(std::find(failure.destroyed.begin(),failure.destroyed.end(),reinterpret_cast<XrSpace>(99))==failure.destroyed.end(),"negative output ignored");
  }
  for(XrResult result:{XR_ERROR_RUNTIME_FAILURE,XR_SESSION_LOSS_PENDING}) {
    Fake failure;fake=&failure;SeatedSpace item;item.begin({create,destroy},session,local);
    item.replace(failure.expected);const auto old=item.space();failure.destroyResult=result;
    check(item.replace(failure.expected)==result&&!item.space(),"uncertain previous destroy retires origin");
    failure.destroyResult=XR_SUCCESS;check(item.shutdown()==XR_SUCCESS&&failure.alive.empty(),"new child retained for shutdown");
    check(std::count(failure.destroyed.begin(),failure.destroyed.end(),old)==1,"uncertain destroy never retried");
  }
  {
    Fake failure;fake=&failure;failure.consumeFailedDestroy=false;
    SeatedSpace item;item.begin({create,destroy},session,local);item.replace(failure.expected);
    const auto old=item.space();failure.destroyResult=XR_ERROR_HANDLE_INVALID;
    check(item.replace(failure.expected)==XR_ERROR_HANDLE_INVALID&&!item.space(),"unconsumed failed destroy retires");
    failure.destroyResult=XR_SUCCESS;item.shutdown();
    check(failure.alive==std::vector<XrSpace>{old}&&std::count(failure.destroyed.begin(),failure.destroyed.end(),old)==1,
      "uncertain old child remains only for implicit session destruction");
    failure.alive.clear(); // xrDestroySession destroys all remaining children.
  }
  Fake nullRuntime;fake=&nullRuntime;SeatedSpace item;item.begin({create,destroy},session,local);nullRuntime.nullOutput=true;
  check(item.replace(nullRuntime.expected)==XR_ERROR_RUNTIME_FAILURE&&!item.space(),"null successful output rejected");
  item.shutdown();check(nullRuntime.destroyed.empty(),"null handle not destroyed");
}
void events() {
  ResetEvents queue;vr::VREvent_t event{};auto pose=invalidHeadPose(true);pose.bPoseIsValid=true;
  pose.eTrackingResult=vr::TrackingResult_Running_OK;pose.mDeviceToAbsoluteTracking.m[0][3]=3;
  vr::TrackedDevicePose_t output{};
  check(!queue.begin(0)&&queue.begin(1)&&!queue.begin(2),"queue init generation");
  check(!queue.room(2)&&queue.room(1),"queue capacity matching generation");
  check(!queue.push(2,1,0,pose)&&!queue.push(1,0,0,pose),"stale/zero origin push rejected");
  check(queue.push(1,4,1000,pose),"event push");
  check(!queue.pop(2,4,vr::TrackingUniverseSeated,1250,event,output),"stale pop does not consume");
  check(queue.pop(1,4,vr::TrackingUniverseSeated,1250,event,output),"event pop");
  check(event.eventType==vr::VREvent_SeatedZeroPoseReset&&event.trackedDeviceIndex==vr::k_unTrackedDeviceIndexInvalid&&
    !event.data.seatedZeroPoseReset.bResetBySystemMenu&&event.eventAgeSeconds==.25f,"historical reset event fields and age");
  check(output.bPoseIsValid&&output.mDeviceToAbsoluteTracking.m[0][3]==3,"event-time pose retained");
  check(!queue.pop(1,4,vr::TrackingUniverseSeated,2000,event,output),"event consumed once");
  queue.push(1,4,1000,pose);
  check(queue.pop(1,5,vr::TrackingUniverseSeated,2000,event,output)&&!output.bPoseIsValid,"old origin pose invalidated but event retained");
  for(auto origin:{vr::TrackingUniverseStanding,vr::TrackingUniverseRawAndUncalibrated}) {
    queue.push(1,5,2000,pose);
    check(queue.pop(1,5,origin,1900,event,output)&&!output.bPoseIsValid&&event.eventAgeSeconds==0,"unavailable origin invalid and clock reversal bounded");
  }
  for(unsigned i=0;i<ResetEvents::capacity;++i)check(queue.push(1,5,2000+i,pose),"bounded fill");
  check(!queue.room(1)&&!queue.push(1,5,3000,pose),"full queue rejects mutation");
  for(unsigned i=0;i<ResetEvents::capacity;++i)
    check(queue.pop(1,5,vr::TrackingUniverseSeated,3000,event,output)&&std::fabs(event.eventAgeSeconds-float(1000-i)/1000)<1e-6,"FIFO wraps correctly");
  queue.push(1,5,0,pose);queue.retire(2);
  check(queue.pop(1,5,vr::TrackingUniverseSeated,0,event,output),"stale retirement ignored");
  queue.push(1,5,0,pose);queue.retire(1);
  check(!queue.pop(1,5,vr::TrackingUniverseSeated,0,event,output)&&queue.begin(2)&&queue.room(2),"retire clears all old events");
}
void publications() {
  CompositorPublication compositor;const auto generation=compositor.begin();
  auto sample=compositor.read();sample.sequence=1;sample.posesAvailable=true;
  sample.renderPose=sample.gamePose=invalidHeadPose(true);sample.renderTime=123;sample.gameTime=134;
  check(compositor.publish(sample),"publication initial pose");
  check(!compositor.resetOrigin(generation+1)&&compositor.read().posesAvailable,"stale reset preserves cache");
  check(compositor.resetOrigin(generation),"same-enum reset increments origin");
  const auto reset=compositor.read();
  check(!reset.posesAvailable&&reset.renderTime==0&&reset.gameTime==0&&reset.originGeneration==sample.originGeneration+1&&reset.origin==sample.origin,"reset invalidates both predictions");
  ++sample.sequence;check(!compositor.publish(sample),"old origin candidate rejected");
  sample.originGeneration=reset.originGeneration;check(compositor.publish(sample),"new origin candidate accepted");

  SystemPublication system;SystemRead meta{};meta.connected=true;const auto gen=system.begin(meta);
  GeometryInput geometry{};geometry.generation=gen;geometry.sequence=1;geometry.headFlags=XR_SPACE_LOCATION_POSITION_VALID_BIT|XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
  geometry.viewFlags=XR_VIEW_STATE_POSITION_VALID_BIT|XR_VIEW_STATE_ORIENTATION_VALID_BIT;geometry.headPose.orientation.w=1;
  for(unsigned i=0;i<2;++i){geometry.width[i]=geometry.height[i]=100;geometry.views[i].pose.orientation.w=1;geometry.views[i].fov={-.7f,.7f,.7f,-.7f};}
  check(system.publish(geometry,true,true),"System geometry fixture");
  system.invalidate(gen+1);check(system.read().geometryValid,"stale System invalidation ignored");
  system.invalidate(gen);check(!system.read().geometryValid&&system.read().connected,"System invalidated while connected");
  check(!system.publish(geometry,true,true),"old frame cannot repopulate invalidated cache");
  ++geometry.sequence;check(system.publish(geometry,true,true),"next frame repopulates System geometry");
}
} // namespace
int main(int argc,char** argv) {
  if(argc==2&&std::string(argv[1])=="--dry-run"){std::puts("openxr_space_test: dry-run (no runtime or writes)");return 0;}
  if(argc!=2||std::string(argv[1])!="--self-test")return 2;
  spaces();events();publications();
  std::printf("openxr_space_test: %u checks, %u failures\n",checks,failures);return failures?1:0;
}
