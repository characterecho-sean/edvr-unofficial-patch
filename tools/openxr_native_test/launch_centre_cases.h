#pragma once
#include "../../src/openxr/native_runtime_host.h"

namespace edvr::openxr::test {
namespace launch_fixture {
constexpr XrSpaceLocationFlags tracked=XR_SPACE_LOCATION_POSITION_VALID_BIT|
    XR_SPACE_LOCATION_ORIENTATION_VALID_BIT|XR_SPACE_LOCATION_POSITION_TRACKED_BIT|
    XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT;
struct Fake {
  XrPosef head{{0,0,0,1},{0,0,0}},origin{{0,0,0,1},{0,0,0}};
  XrResult createResult=XR_SUCCESS,locateResult=XR_SUCCESS;
  XrSpaceLocationFlags flags=tracked;
  unsigned creates=0,destroys=0,locates=0,converts=0;
  bool ready=false,argumentsValid=true;
  XrTime sampleTime=0,verifyTime=0;
  XrSpace lastDestroyed=XR_NULL_HANDLE;
};
inline Fake* active=nullptr;
inline XrInstance instance(){return reinterpret_cast<XrInstance>(1);}
inline XrSession session(){return reinterpret_cast<XrSession>(2);}
inline XrSpace local(){return reinterpret_cast<XrSpace>(3);}
inline XrSpace view(){return reinterpret_cast<XrSpace>(4);}
inline XrSpace owned(){return reinterpret_cast<XrSpace>(uintptr_t(4+active->creates));}
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
  out->predictedDisplayTime=200;out->predictedDisplayPeriod=10;out->shouldRender=XR_FALSE;return XR_SUCCESS;
}
inline XrResult XRAPI_PTR beginFrame(XrSession,const XrFrameBeginInfo*){return XR_SUCCESS;}
inline XrResult XRAPI_PTR endFrame(XrSession,const XrFrameEndInfo*){return XR_SUCCESS;}
inline XrResult XRAPI_PTR convert(XrInstance,const LARGE_INTEGER* qpc,XrTime* time) {
  ++active->converts;*time=qpc->QuadPart;return XR_SUCCESS;
}
inline XrResult XRAPI_PTR create(XrSession s,const XrReferenceSpaceCreateInfo* info,XrSpace* out) {
  ++active->creates;
  active->argumentsValid&=s==session()&&info->referenceSpaceType==XR_REFERENCE_SPACE_TYPE_LOCAL;
  if(active->createResult!=XR_SUCCESS)return active->createResult;
  active->origin=info->poseInReferenceSpace;*out=owned();return XR_SUCCESS;
}
inline XrResult XRAPI_PTR destroy(XrSpace space) {
  ++active->destroys;active->lastDestroyed=space;return XR_SUCCESS;
}
inline XrResult XRAPI_PTR locate(XrSpace target,XrSpace base,XrTime time,XrSpaceLocation* out) {
  ++active->locates;active->argumentsValid&=target==view()&&(base==local()||base==owned());
  if(active->locateResult!=XR_SUCCESS)return active->locateResult;
  out->locationFlags=active->flags;out->pose=active->head;
  if(base==local())active->sampleTime=time;
  else {
    active->verifyTime=time;
    const auto& q=active->origin.orientation;
    out->pose.orientation=product({-q.x,-q.y,-q.z,q.w},active->head.orientation);
    double r[3][3];detail::rotation(q,r);
    const auto& a=active->head.position;const auto& b=active->origin.position;
    const double d[3]={double(a.x)-b.x,double(a.y)-b.y,double(a.z)-b.z};
    out->pose.position={float(r[0][0]*d[0]+r[1][0]*d[1]+r[2][0]*d[2]),
      float(r[0][1]*d[0]+r[1][1]*d[1]+r[2][1]*d[2]),
      float(r[0][2]*d[0]+r[1][2]*d[1]+r[2][2]*d[2])};
  }
  if(out->next)static_cast<XrSpaceVelocity*>(out->next)->velocityFlags=0;
  return XR_SUCCESS;
}
struct Fixture {
  Fake fake;
  OwnerService owner;
  RenderThreadDispatcher dispatcher{owner};
  RenderRoute route{dispatcher};
  NativeRuntimeHost host{owner,dispatcher,route};
  bool initialized=false;
  Fixture() {
    active=&fake;host.instance=instance();host.session=session();host.local=local();host.view=view();
    host.api.convertTime=convert;host.api.locateSpace=locate;
    const Dispatch dispatch{poll,beginSession,endSession,wait,beginFrame,endFrame};
    SystemRead metadata{};metadata.connected=true;
    for(unsigned eye=0;eye<2;++eye){metadata.recommendedWidth[eye]=3072;metadata.recommendedHeight[eye]=3264;}
    host.geometryGeneration=host.geometry.begin(metadata);
    host.compositorGeneration=host.poses.begin();host.runtimeGeneration=host.gate.beginGeneration();
    initialized=host.seated.begin({create,destroy},session(),local())&&host.changes.begin(session())&&
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
    host.local=host.view=XR_NULL_HANDLE;host.api={};active=nullptr;
  }
};
} // namespace launch_fixture

template<class Check> void runLaunchCentreCases(Check check) {
  using namespace launch_fixture;
  {
    Fixture f;auto& h=f.host;check(f.initialized,"startup fixture initialized without runtime/device");
    bool refresh=false;
    check(h.centreAtStartup(refresh)&&refresh&&f.fake.creates==0&&h.launchCentre.pending(),
      "placeholder leaves origin alone and requests another startup frame");
    f.fake.head={{0,1,0,0},{1,-2,3}};
    const auto generation=h.poses.read().originGeneration;
    check(h.centreAtStartup(refresh)&&refresh&&f.fake.creates==1&&!h.launchCentre.pending(),
      "real startup host centers backward heading and downward offset once");
    check(f.fake.origin.position.x==1&&f.fake.origin.position.y==-2&&f.fake.origin.position.z==3&&
      std::fabs(f.fake.origin.orientation.y)>0.999f&&f.fake.origin.orientation.x==0&&f.fake.origin.orientation.z==0,
      "new LOCAL origin has current position and upright heading");
    check(f.fake.argumentsValid&&f.fake.sampleTime==f.fake.verifyTime&&f.fake.converts==2&&f.fake.locates==3&&
      h.resetPositionError<0.0001f&&h.resetYawError<0.0001f,"same-time verification uses new reference space");
    check(h.poses.read().originGeneration==generation+1&&!h.read().geometryValid&&!h.frameGeometryAvailable&&
      h.read().recommendedWidth[0]==3072&&h.read().recommendedHeight[1]==3264,
      "startup reset invalidates geometry and advances origin while retaining display dimensions");
    vr::VREvent_t event{};vr::TrackedDevicePose_t pose{};
    check(!h.resetEvents.pop(h.geometryGeneration,h.poses.read().originGeneration,vr::TrackingUniverseSeated,0,event,pose),
      "startup does not synthesize an application reset event");
    const auto calls=f.fake.locates;
    check(h.centreAtStartup(refresh)&&!refresh&&f.fake.creates==1&&f.fake.locates==calls,
      "completed startup cannot recenter again");
    XrSpaceLocation explicitHead{XR_TYPE_SPACE_LOCATION};explicitHead.pose=f.fake.head;explicitHead.locationFlags=tracked;
    auto lease=h.gate.tryEnter(h.runtimeGeneration);
    check(h.applySeatedReset(explicitHead,"seated_reset",true)&&f.fake.creates==2&&f.fake.destroys==1,
      "explicit reset reuses natural origin and retires prior seated space");
    check(f.fake.lastDestroyed==reinterpret_cast<XrSpace>(5)&&h.seated.space()==reinterpret_cast<XrSpace>(6)&&
      f.fake.argumentsValid,"reset retires previous handle and verifies against distinct new handle");
    check(h.resetEvents.pop(h.geometryGeneration,h.poses.read().originGeneration,vr::TrackingUniverseSeated,GetTickCount64(),event,pose)&&
      event.eventType==vr::VREvent_SeatedZeroPoseReset&&pose.bPoseIsValid,
      "explicit reset still publishes its application event");
  }
  {
    Fixture f;bool refresh=false;f.fake.head.position.y=1;f.fake.createResult=XR_ERROR_RUNTIME_FAILURE;
    check(!f.host.centreAtStartup(refresh)&&!f.host.seated.space()&&f.host.lastResetResult==XR_ERROR_RUNTIME_FAILURE,
      "reference-space creation failure cannot report centered startup");
  }
  {
    Fixture f;bool refresh=false;f.fake.head.position.y=1;
    check(f.host.boundary.waitAndBegin()==XR_SUCCESS&&f.host.state.frameOpen(),"fixture opens a frame");
    check(!f.host.centreAtStartup(refresh)&&f.fake.creates==0,"startup refuses to replace a space while frame is open");
  }
  {
    Fixture f;bool refresh=false;f.fake.flags=0;
    f.host.launchCentreSamples=1;f.host.launchCentreBegan=GetTickCount64()-2001;
    check(f.host.centreAtStartup(refresh)&&!refresh&&!f.host.launchCentre.pending()&&f.fake.creates==0,
      "tracking deadline leaves original origin without a late recenter");
    f.fake.flags=tracked;f.fake.head.position.y=1;
    check(f.host.centreAtStartup(refresh)&&!refresh&&f.fake.creates==0,"tracking recovery after startup cannot move world");
  }
  {
    Fixture f;bool refresh=false;f.fake.head.position.y=1;
    bool accepted=true;
    std::thread foreign([&]{accepted=f.host.centreAtStartup(refresh);});foreign.join();
    check(!accepted&&f.fake.locates==0&&f.fake.creates==0,"foreign thread cannot sample or replace startup origin");
    f.fake.locateResult=XR_ERROR_TIME_INVALID;
    check(f.host.centreAtStartup(refresh)&&refresh&&f.fake.creates==0,"temporary current-time failure waits for tracking");
    f.fake.locateResult=XR_ERROR_RUNTIME_FAILURE;
    check(!f.host.centreAtStartup(refresh)&&f.fake.creates==0,"runtime locate failure cannot report successful centering");
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
  h.sceneFinished(false,XR_SUCCESS);check(h.emptyWithholds==1&&h.previousPairValid,"zero-layer withhold retains prior good pair");
  h.sceneFinished(true,XR_SUCCESS);check(h.replayedPairs==1&&h.previousPairValid,"replay does not overwrite saved stereo pair");
  h.sceneFinished(true,XR_ERROR_RUNTIME_FAILURE);check(!h.previousPairValid,"failed endFrame cannot commit a replay pair");
  h.previousPairValid=true;h.invalidateOrigin("feature_fixture");check(!h.previousPairValid,"reference reset retires transition image");
  vr::TrackedDevicePose_t pose=invalidHeadPose(true);pose.bPoseIsValid=true;
  pose.mDeviceToAbsoluteTracking.m[0][3]=10;pose.mDeviceToAbsoluteTracking.m[1][3]=20;pose.mDeviceToAbsoluteTracking.m[2][3]=30;
  const float offset[3]={-.25f,.25f,-1.25f};auto physical=pose;
  applyNativeHeadOffset(pose,offset,1.57079632679f);
  const auto& m=pose.mDeviceToAbsoluteTracking.m;
  check(std::fabs(m[0][2]-1)<.0001f&&std::fabs(m[2][0]+1)<.0001f&&m[0][3]==9.75f&&m[1][3]==20.25f&&m[2][3]==28.75f,
    "Explorer yaw rotates orientation but adds tracking-coordinate translation without rotating origin");
  check(physical.mDeviceToAbsoluteTracking.m[2][3]==30&&physical.mDeviceToAbsoluteTracking.m[0][0]==1,"game offset leaves physical pose untouched");
  const vr::VRTextureBounds_t reversed{.9f,.8f,.1f,.2f};
  const auto crop=nativeCropBounds(&reversed,.25f,.1f,.75f,.9f);
  check(std::fabs(crop.uMin-.7f)<.0001f&&std::fabs(crop.uMax-.3f)<.0001f&&std::fabs(crop.vMin-.74f)<.0001f&&std::fabs(crop.vMax-.26f)<.0001f,
    "guard crop composes within original subrect and preserves both flips");
}
} // namespace edvr::openxr::test
