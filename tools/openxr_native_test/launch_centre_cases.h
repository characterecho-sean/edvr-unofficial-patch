#pragma once
#include "../../src/openxr/native_runtime_host.h"

namespace edvr::openxr::test {
namespace launch_fixture {
constexpr XrSpaceLocationFlags tracked=XR_SPACE_LOCATION_POSITION_VALID_BIT|
    XR_SPACE_LOCATION_ORIENTATION_VALID_BIT|XR_SPACE_LOCATION_POSITION_TRACKED_BIT|
    XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT;
// F189 (#144): this runtime knows the natural space and nothing else. The seat origin is EDVR's own arithmetic (seat_transform.h), so a call to xrCreateReferenceSpace or
// xrDestroySpace is counted here (creates, destroys) and must stay 0 whatever the origin does, and a locate against any space but the natural one fails argumentsValid.
// The old seated reference space is kept as an ORACLE (oldSpacePose below): what the runtime would have said for a space made at an origin, written out without
// seat_transform.h, so the cases check that file and do not repeat it.
struct Fake {
  XrPosef head{{0,0,0,1},{0,0,0}};
  XrResult createResult=XR_SUCCESS,locateResult=XR_SUCCESS;
  XrSpaceLocationFlags flags=tracked;
  XrSpaceVelocityFlags velocityFlags=0;XrVector3f linearVelocity{0,0,0},angularVelocity{0,0,0};   // what a locate with a velocity chained reports (flags 0: none, as before)
  std::vector<XrTime> times;   // the instant of every locate of the head against the natural space, in order
  unsigned creates=0,destroys=0,locates=0,converts=0;
  bool ready=false,argumentsValid=true;
  // Every existing case leaves this false, matching wait()'s old hardcoded
  // XR_FALSE exactly; frame_end_overlap_cases.h is the one that needs a
  // wait that actually admits pixels.
  bool shouldRender=false;
  XrReferenceSpaceType naturalType=XR_REFERENCE_SPACE_TYPE_LOCAL;
  XrSpace naturalSpace=reinterpret_cast<XrSpace>(3); // the default LOCAL fixture handle
  XrPosef localOriginInStage{{0,0,0,1},{0,0,0}};
  XrSpaceLocationFlags localOriginFlags=tracked;
  XrTime sampleTime=0;
  XrTime lastTime=0;   // the instant of the most recent locate, whichever space it was against (the pose-time cases)
  XrTime rejectTime=-1;XrResult rejectResult=XR_ERROR_TIME_INVALID;   // a locate at exactly this instant answers rejectResult (the pose-time cases)
  XrSpace lastDestroyed=XR_NULL_HANDLE;
};
inline Fake* active=nullptr;
inline XrInstance instance(){return reinterpret_cast<XrInstance>(1);}
inline XrSession session(){return reinterpret_cast<XrSession>(2);}
inline XrSpace local(){return reinterpret_cast<XrSpace>(3);}
inline XrSpace stage(){return reinterpret_cast<XrSpace>(100);}
inline XrSpace view(){return reinterpret_cast<XrSpace>(4);}
inline XrQuaternionf product(XrQuaternionf a,XrQuaternionf b) {
  return {a.w*b.x+a.x*b.w+a.y*b.z-a.z*b.y,a.w*b.y-a.x*b.z+a.y*b.w+a.z*b.x,
    a.w*b.z+a.x*b.y-a.y*b.x+a.z*b.w,a.w*b.w-a.x*b.x-a.y*b.y-a.z*b.z};
}
inline XrResult XRAPI_PTR poll(XrInstance,XrEventDataBuffer* out) {
  if(active->ready)return XR_EVENT_UNAVAILABLE;
  active->ready=true;
  XrEventDataSessionStateChanged event{XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED};
  event.session=session();event.state=XR_SESSION_STATE_READY;
  std::memcpy(out,&event,sizeof(event));return XR_SUCCESS;
}
inline XrResult XRAPI_PTR beginSession(XrSession,const XrSessionBeginInfo*){return XR_SUCCESS;}
inline XrResult XRAPI_PTR endSession(XrSession){return XR_SUCCESS;}
inline XrResult XRAPI_PTR wait(XrSession,const XrFrameWaitInfo*,XrFrameState* out) {
  out->predictedDisplayTime=200;out->predictedDisplayPeriod=10;out->shouldRender=active->shouldRender?XR_TRUE:XR_FALSE;return XR_SUCCESS;
}
inline XrResult XRAPI_PTR beginFrame(XrSession,const XrFrameBeginInfo*){return XR_SUCCESS;}
inline XrResult XRAPI_PTR endFrame(XrSession,const XrFrameEndInfo*){return XR_SUCCESS;}
inline XrResult XRAPI_PTR convert(XrInstance,const LARGE_INTEGER* qpc,XrTime* time) {
  ++active->converts;*time=qpc->QuadPart;return XR_SUCCESS;
}
inline XrResult XRAPI_PTR create(XrSession s,const XrReferenceSpaceCreateInfo* info,XrSpace* out) {
  ++active->creates;
  active->argumentsValid&=s==session()&&info->referenceSpaceType==active->naturalType;
  if(active->createResult!=XR_SUCCESS)return active->createResult;
  *out=reinterpret_cast<XrSpace>(uintptr_t(1000+active->creates));return XR_SUCCESS;
}
inline XrResult XRAPI_PTR destroy(XrSpace space) {
  ++active->destroys;active->lastDestroyed=space;return XR_SUCCESS;
}
inline XrResult XRAPI_PTR locate(XrSpace target,XrSpace base,XrTime time,XrSpaceLocation* out) {
  if(target==local()&&base==stage()) {
    ++active->locates;active->lastTime=time;
    if(active->locateResult!=XR_SUCCESS)return active->locateResult;
    out->locationFlags=active->localOriginFlags;out->pose=active->localOriginInStage;return XR_SUCCESS;
  }
  ++active->locates;active->lastTime=time;active->argumentsValid&=target==view()&&base==active->naturalSpace;
  if(time==active->rejectTime)return active->rejectResult;
  if(active->locateResult!=XR_SUCCESS)return active->locateResult;
  out->locationFlags=active->flags;out->pose=active->head;
  active->sampleTime=time;active->times.push_back(time);
  if(out->next) {
    auto* velocity=static_cast<XrSpaceVelocity*>(out->next);
    velocity->velocityFlags=active->velocityFlags;velocity->linearVelocity=active->linearVelocity;velocity->angularVelocity=active->angularVelocity;
  }
  return XR_SUCCESS;
}
// The old seated reference space, as an oracle (see Fake): the numbers xrLocateSpace gave in a space made with poseInReferenceSpace = origin. A pose is origin^-1 * natural
// (the quaternion product with the origin's conjugate, the transposed rotation of the offset), a velocity is turned by the transposed rotation.
inline XrPosef oldSpacePose(const XrPosef& origin,const XrPosef& natural) {
  XrPosef out=natural;
  const auto& q=origin.orientation;
  out.orientation=product({-q.x,-q.y,-q.z,q.w},natural.orientation);
  double r[3][3];detail::rotation(q,r);
  const auto& a=natural.position;const auto& b=origin.position;
  const double d[3]={double(a.x)-b.x,double(a.y)-b.y,double(a.z)-b.z};
  out.position={float(r[0][0]*d[0]+r[1][0]*d[1]+r[2][0]*d[2]),
    float(r[0][1]*d[0]+r[1][1]*d[1]+r[2][1]*d[2]),
    float(r[0][2]*d[0]+r[1][2]*d[1]+r[2][2]*d[2])};
  return out;
}
inline XrVector3f oldSpaceVelocity(const XrPosef& origin,const XrVector3f& natural) {
  double r[3][3];detail::rotation(origin.orientation,r);
  return {float(r[0][0]*natural.x+r[1][0]*natural.y+r[2][0]*natural.z),float(r[0][1]*natural.x+r[1][1]*natural.y+r[2][1]*natural.z),
    float(r[0][2]*natural.x+r[1][2]*natural.y+r[2][2]*natural.z)};
}
// A locate of `target` in the seated space an origin stands for: the runtime's answer in the natural space, then the oracle. What the old fixture's locate gave a caller that
// asked for the seated reference space.
inline XrResult locateSeated(const XrPosef& origin,XrSpace target,XrTime time,XrSpaceLocation* out) {
  const XrResult r=locate(target,active->naturalSpace,time,out);
  if(r==XR_SUCCESS)out->pose=oldSpacePose(origin,out->pose);
  return r;
}
// No existing case needs a per-frame stereo view locate (centreAtStartup and
// the treatment fixtures never reach waitPoses's owner body); frame_end_
// overlap_cases.h does, for locateGeometry's own xrLocateViews requirement.
inline XrResult XRAPI_PTR locateViews(XrSession,const XrViewLocateInfo* info,XrViewState* state,
    uint32_t capacity,uint32_t* count,XrView* views) {
  active->argumentsValid&=info&&info->space==active->naturalSpace;   // F189: the views too are located in the natural space only
  *count=2;
  if(capacity<2)return XR_SUCCESS;
  state->viewStateFlags=XR_VIEW_STATE_POSITION_VALID_BIT|XR_VIEW_STATE_ORIENTATION_VALID_BIT;
  for(unsigned eye=0;eye<2;++eye) {
    views[eye]={XR_TYPE_VIEW};views[eye].pose=active->head;
    views[eye].pose.position.x+=eye?.032f:-.032f;views[eye].fov={-.7f,.7f,.7f,-.7f};
  }
  return XR_SUCCESS;
}
struct Fixture {
  Fake fake;
  OwnerService owner;
  RenderThreadDispatcher dispatcher{owner};
  RenderRoute route{dispatcher};
  NativeRuntimeHost host{owner,dispatcher,route};
  bool initialized=false;
  explicit Fixture(bool useStage=false) {
    active=&fake;fake.naturalType=useStage?XR_REFERENCE_SPACE_TYPE_STAGE:XR_REFERENCE_SPACE_TYPE_LOCAL;
    fake.naturalSpace=useStage?stage():local();
    host.instance=instance();host.session=session();host.local=fake.naturalSpace;host.xrLocal=local();host.view=view();
    host.api.convertTime=convert;host.api.locateSpace=locate;
    host.api.createSpace=create;host.api.destroySpace=destroy;   // counted, never expected: the seat origin makes no reference space (F189)
    const Dispatch dispatch{poll,beginSession,endSession,wait,beginFrame,endFrame};
    SystemRead metadata{};metadata.connected=true;
    for(unsigned eye=0;eye<2;++eye){metadata.recommendedWidth[eye]=3072;metadata.recommendedHeight[eye]=3264;}
    host.geometryGeneration=host.geometry.begin(metadata);
    host.compositorGeneration=host.poses.begin();host.runtimeGeneration=host.gate.beginGeneration();
    initialized=host.seated.begin(session(),fake.naturalSpace,fake.naturalType)&&
      host.changes.begin(session(),fake.naturalType)&&
      host.resetEvents.begin(host.geometryGeneration)&&host.state.reset(dispatch,instance(),session(),XR_ENVIRONMENT_BLEND_MODE_OPAQUE)==XR_SUCCESS&&
      host.state.pollEvents()==XR_SUCCESS&&host.state.startIfReady()==XR_SUCCESS;
    GeometryInput geometry{};geometry.generation=host.geometryGeneration;geometry.sequence=1;geometry.displayTime=100;
    geometry.headPose=fake.head;geometry.headFlags=tracked;
    geometry.viewFlags=XR_VIEW_STATE_POSITION_VALID_BIT|XR_VIEW_STATE_ORIENTATION_VALID_BIT;
    for(unsigned eye=0;eye<2;++eye) {
      geometry.width[eye]=3072;geometry.height[eye]=3264;geometry.views[eye].pose=fake.head;
      geometry.views[eye].pose.position.x=eye?.032f:-.032f;geometry.views[eye].fov={-.7f,.7f,.7f,-.7f};
    }
    initialized&=host.geometry.publish(geometry,false,false);
    host.frameGeometry=geometry;host.frameGeometryAvailable=true;
  }
  ~Fixture() {
    host.boundary.clear();host.seated.shutdown();host.state.abandonAfterOwnerDestruction();
    host.gate.requestStop(host.runtimeGeneration);host.gate.finishGeneration(host.runtimeGeneration);
    host.runtimeGeneration=0;host.instance=XR_NULL_HANDLE;host.session=XR_NULL_HANDLE;
    host.local=host.xrLocal=host.view=XR_NULL_HANDLE;host.api={};active=nullptr;
  }
};
} // namespace launch_fixture

template<class Check> void runLaunchCentreCases(Check check) {
  using namespace launch_fixture;
  {
    Fixture f;auto& h=f.host;check(f.initialized,"startup fixture initialized without runtime/device");
    bool refresh=false;
    check(h.centreAtStartup(refresh)&&refresh&&h.seated.originSets()==0&&h.launchCentre.pending(),
      "placeholder leaves origin alone and requests another startup frame");
    f.fake.head={{0,1,0,0},{1,-2,3}};
    const auto generation=h.poses.read().originGeneration;
    check(h.centreAtStartup(refresh)&&refresh&&h.seated.originSets()==1&&!h.launchCentre.pending(),
      "real startup host centers backward heading and downward offset once");
    const XrPosef& stored=h.seated.origin();
    check(stored.position.x==1&&stored.position.y==-2&&stored.position.z==3&&
      std::fabs(stored.orientation.y)>0.999f&&stored.orientation.x==0&&stored.orientation.z==0,
      "new LOCAL origin has current position and upright heading");
    check(f.fake.argumentsValid&&f.fake.times.size()==3&&f.fake.times[1]==f.fake.times[2]&&f.fake.converts==2&&f.fake.locates==3&&
      h.resetPositionError<0.0001f&&h.resetYawError<0.0001f,"same-time verification uses the new origin (the natural space located twice at one instant)");
    check(f.fake.creates==0&&f.fake.destroys==0&&h.seated.space()==f.fake.naturalSpace,
      "the startup centre made no reference space: the seated handle is still the natural space");
    check(h.poses.read().originGeneration==generation+1&&!h.read().geometryValid&&!h.frameGeometryAvailable&&
      h.read().recommendedWidth[0]==3072&&h.read().recommendedHeight[1]==3264,
      "startup reset invalidates geometry and advances origin while retaining display dimensions");
    vr::VREvent_t event{};vr::TrackedDevicePose_t pose{};
    check(!h.resetEvents.pop(h.geometryGeneration,h.poses.read().originGeneration,vr::TrackingUniverseSeated,0,event,pose),
      "startup does not synthesize an application reset event");
    const auto calls=f.fake.locates;
    check(h.centreAtStartup(refresh)&&!refresh&&h.seated.originSets()==1&&f.fake.locates==calls,
      "completed startup cannot recenter again");
    XrSpaceLocation explicitHead{XR_TYPE_SPACE_LOCATION};explicitHead.pose=f.fake.head;explicitHead.locationFlags=tracked;
    auto lease=h.gate.tryEnter(h.runtimeGeneration);
    check(h.applySeatedReset(explicitHead,"seated_reset",true)&&h.seated.originSets()==2&&f.fake.creates==0&&f.fake.destroys==0,
      "explicit reset reuses natural origin and moves the origin: no reference space created or destroyed");
    check(h.seated.space()==f.fake.naturalSpace&&f.fake.argumentsValid,
      "the reset verifies against the natural space, whose handle never changes");
    check(h.resetEvents.pop(h.geometryGeneration,h.poses.read().originGeneration,vr::TrackingUniverseSeated,GetTickCount64(),event,pose)&&
      event.eventType==vr::VREvent_SeatedZeroPoseReset&&pose.bPoseIsValid,
      "explicit reset still publishes its application event");
  }
  {
    // F189: a runtime that would refuse a reference space is not asked for one, so it cannot refuse the centre (the old fixture failed startup here).
    Fixture f;bool refresh=false;f.fake.head.position.y=1;f.fake.createResult=XR_ERROR_RUNTIME_FAILURE;
    check(f.host.centreAtStartup(refresh)&&f.host.seated.space()==f.fake.naturalSpace&&f.host.seated.originSets()==1&&f.fake.creates==0&&
      f.host.lastResetResult==XR_SUCCESS,"a runtime that refuses reference spaces still centres: the origin is stored, none is asked for");
  }
  {
    Fixture f;bool refresh=false;f.fake.head.position.y=1;
    check(f.host.boundary.waitAndBegin()==XR_SUCCESS&&f.host.state.frameOpen(),"fixture opens a frame");
    check(!f.host.centreAtStartup(refresh)&&f.host.seated.originSets()==0&&f.fake.creates==0,"startup refuses to move the origin while frame is open (the gate is kept)");
  }
  {
    Fixture f;bool refresh=false;f.fake.flags=0;
    f.host.launchCentreSamples=1;f.host.launchCentreBegan=GetTickCount64()-2001;
    check(f.host.centreAtStartup(refresh)&&!refresh&&!f.host.launchCentre.pending()&&f.host.seated.originSets()==0&&f.fake.creates==0,
      "tracking deadline leaves original origin without a late recenter");
    f.fake.flags=tracked;f.fake.head.position.y=1;
    check(f.host.centreAtStartup(refresh)&&!refresh&&f.host.seated.originSets()==0&&f.fake.creates==0,"tracking recovery after startup cannot move world");
  }
  {
    Fixture f;bool refresh=false;f.fake.head.position.y=1;
    bool accepted=true;
    std::thread foreign([&]{accepted=f.host.centreAtStartup(refresh);});foreign.join();
    check(!accepted&&f.fake.locates==0&&f.host.seated.originSets()==0,"foreign thread cannot sample or move startup origin");
    f.fake.locateResult=XR_ERROR_TIME_INVALID;
    check(f.host.centreAtStartup(refresh)&&refresh&&f.host.seated.originSets()==0,"temporary current-time failure waits for tracking");
    f.fake.locateResult=XR_ERROR_RUNTIME_FAILURE;
    check(!f.host.centreAtStartup(refresh)&&f.host.seated.originSets()==0,"runtime locate failure cannot report successful centering");
    check(f.fake.creates==0&&f.fake.destroys==0,"...and the runtime was never asked for a reference space on any of these paths");
  }
}
template<class Check> void runFeatureHostCases(Check check) {
  using namespace launch_fixture;
  Fixture f;auto& h=f.host;
  h.frameSpace=local();h.frameWithheld=true;h.featureFrame.resubmitEnabled=1;
  check(!h.sceneLayerAvailable(),"first withheld pair has no invented replay image");
  h.previousPairValid=true;h.previousReference=h.poses.read().originGeneration;h.previousSpace=local();
  check(h.sceneLayerAvailable(),"saved stereo pair requires matching reference and space");
  ++h.previousReference;check(!h.sceneLayerAvailable(),"old reference cannot replay after recenter");--h.previousReference;
  h.previousSpace=view();check(!h.sceneLayerAvailable(),"different space cannot replay");h.previousSpace=local();
  h.featureFrame.resubmitEnabled=0;check(!h.sceneLayerAvailable(),"resubmit disabled uses zero layers");h.featureFrame.resubmitEnabled=1;
  // Explorer Cam's comfort fade is read from the provider's last answer, and only while a provider is here: with none acquired, whatever the kept answer holds
  // (the d3d11 half unloaded in the middle of a fade) reads 0, never black.
  h.featureFrameKnown=true;h.featureFrame.fadeAlpha=1.0f;
  check(h.comfortFade()==0.0f,"no provider acquired: a fade level left in the kept frame reads 0 (an unloaded d3d11 half never leaves the user in black)");
  h.featureFrameKnown=false;h.featureFrame.fadeAlpha=0.0f;
  h.sceneFinished(false,XR_SUCCESS);check(h.emptyWithholds==1&&h.previousPairValid,"zero-layer withhold retains prior good pair");
  h.sceneFinished(true,XR_SUCCESS);check(h.replayedPairs==1&&h.previousPairValid,"replay does not overwrite saved stereo pair");
  h.sceneFinished(true,XR_ERROR_RUNTIME_FAILURE);check(!h.previousPairValid,"failed endFrame cannot commit a replay pair");
  h.previousPairValid=true;h.invalidateOrigin("feature_fixture");check(!h.previousPairValid,"reference reset retires transition image");
  // F189: the origin is no longer a space of its own, so a pair kept before the origin moved (an eye alignment, a look turn, a pitch) is a pair a replay may draw: the one
  // handle is the same, the replay converts it with the origin it was located with, and the replays that crossed an origin change are counted.
  {
    Fixture g;auto& k=g.host;
    k.frameSpace=local();k.frameWithheld=true;k.featureFrame.resubmitEnabled=1;
    k.previousPairValid=true;k.previousReference=k.poses.read().originGeneration;k.previousSpace=local();
    k.previousOrigin=XrPosef{{0,0,0,1},{0,0,0}};k.frameOrigin=XrPosef{{0,0,0,1},{0.01f,0,0}};
    check(k.sceneLayerAvailable(),"F189: a kept pair from before an origin change is replayable (the handle is the natural space on both sides)");
    k.sceneFinished(true,XR_SUCCESS);
    check(k.replayedPairs==1&&k.replaysAcrossOrigin==1,"F189: a replay that crossed an origin change is counted");
    k.frameOrigin=k.previousOrigin;
    k.sceneFinished(true,XR_SUCCESS);
    check(k.replayedPairs==2&&k.replaysAcrossOrigin==1,"F189: a replay under the same origin is not counted as crossing one");
    k.sceneFinished(false,XR_SUCCESS);
    check(k.emptyWithholds==1&&k.replaysAcrossOrigin==1,"F189: a zero-layer withhold is not a replay across an origin");
  }
}
} // namespace edvr::openxr::test
