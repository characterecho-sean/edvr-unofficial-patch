#include "../../src/openxr/openvr_system.h"
#include "../../src/openxr/system_source.h"
#include "../../src/openxr/geometry_snapshot.h"
#include "../../src/openxr/system_publication.h"
#include "../../src/openxr/scope_zoom_state.h"   // B3: the runtime's decision to narrow eye 0's told frustum
#include "../../src/d3d11/scope_zoom_b3.h"      // B3: the frame thread's half of the same window (b3Step), for the two halves driven together

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <mutex>
#include <utility>
#include <thread>
#include <vector>
extern "C" vr::IVRSystem* openxrAbiCaller(vr::IVRSystem*);
// (windows.h stays out of this rig: its `near` macro breaks the headers the rig includes.)
extern "C" __declspec(dllimport) unsigned long __stdcall GetCurrentThreadId(void);
extern "C" int openxrAbiCallPose(vr::IVRSystem*, vr::ETrackingUniverseOrigin, float, vr::TrackedDevicePose_t*, uint32_t);


namespace {
using edvr::openxr::GeometryInput;
using edvr::openxr::GeometrySnapshot;
using edvr::openxr::SystemRead;
using edvr::openxr::SystemSource;

unsigned checks = 0, failures = 0;
void check(bool ok, const char* text) {
  ++checks;
  if (!ok) { ++failures; std::printf("FAIL: %s\n", text); }
}
bool near(float a, float b, float eps = 2e-4f) { return std::fabs(a - b) <= eps; }

GeometryInput geometry(uint64_t generation = 17, uint64_t sequence = 23) {
  GeometryInput in{};
  in.generation = generation; in.sequence = sequence; in.displayTime = 0x1020304050607080ll;
  in.viewFlags = XR_VIEW_STATE_ORIENTATION_VALID_BIT | XR_VIEW_STATE_POSITION_VALID_BIT;
  in.headFlags = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT;
  in.headPose.orientation.w = 1; in.headPose.position = {1, 2, 3};
  for (unsigned eye = 0; eye != 2; ++eye) {
    in.views[eye].pose.orientation.w = 1;
    in.views[eye].pose.position = {1, 2, eye ? 3.06f : 2.94f};
    in.views[eye].fov = {-0.6f, 0.7f, 0.5f, -0.4f};
    in.width[eye] = 1111 + eye * 17; in.height[eye] = 777 + eye * 9;
  }
  return in;
}

struct FakeSource final : SystemSource {
  mutable std::mutex mutex;
  SystemRead state{};
  GeometrySnapshot original{};
  uint64_t locatedGeneration = 0;
  float locatedPrediction = 0;
  vr::ETrackingUniverseOrigin locatedOrigin = vr::TrackingUniverseRawAndUncalibrated;
  unsigned locateCalls = 0, resetCalls = 0, pollCalls = 0;
  unsigned projectionNotes = 0; uint64_t notedSequence = 0; unsigned notedEye = 99;
  float notedNear = 0, notedFar = 0;
  unsigned queryAccepted=0,queryRejected=0;
  unsigned frequencyNotes=0;vr::ETrackedPropertyError frequencyError{};float frequencyValue=0;
  std::atomic<unsigned> propertyNotes{0},meshNotes{0};
  vr::ETrackedDeviceProperty lastProperty{};vr::ETrackedPropertyError lastError{};
  void notePropertyQuery(unsigned,vr::TrackedDeviceIndex_t,vr::ETrackedDeviceProperty p,vr::ETrackedPropertyError e) noexcept override {
    std::lock_guard<std::mutex> lock(mutex);++propertyNotes;lastProperty=p;lastError=e;
  }
  const char* lastMeshReason="";uint32_t lastMeshTriangles=0,lastMeshDropped=0;
  void noteHiddenMesh(unsigned,uint64_t,uint32_t triangles,const char* reason,uint32_t dropped) noexcept override {
    ++meshNotes;lastMeshReason=reason;lastMeshTriangles=triangles;lastMeshDropped=dropped;
  }
  // Every answer that carried a mesh, not rate limited (visual.desktop_window_crop = native zooms when the game was served one).
  std::atomic<unsigned> servedMask{0},servedNotes{0};
  void noteHiddenMeshServed(unsigned eye) noexcept override {servedMask.fetch_or(1u<<eye);++servedNotes;}
  // B3: the game's reads of the projection (raw: GetProjectionRaw), the ones that were eye 0's with the scope zoom held back from the raw getter, the span each answered and who asked.
  std::atomic<unsigned> reads[2][2]{},heldReads{0};
  std::atomic<float> lastSpan[2][2]{};std::atomic<uint32_t> lastCaller[2][2]{};
  void noteProjectionRead(unsigned eye,bool raw,bool zoomHeld,float span,uint32_t caller) noexcept override {
    if(eye<2){reads[eye][raw?1:0].fetch_add(1);lastSpan[eye][raw?1:0].store(span);lastCaller[eye][raw?1:0].store(caller);}
    if(eye==0&&zoomHeld)heldReads.fetch_add(1);
  }
  void noteFrequencyQuery(const SystemRead&,vr::TrackedDeviceIndex_t,vr::ETrackedPropertyError error,
      float value,unsigned) noexcept override {
    std::lock_guard<std::mutex> lock(mutex);++frequencyNotes;frequencyError=error;frequencyValue=value;
  }
  void noteProjectionQuery(const SystemRead&,unsigned,float,float,vr::EGraphicsAPIConvention,
      bool accepted,const vr::HmdMatrix44_t&,const void*) noexcept override {
    std::lock_guard<std::mutex> lock(mutex);if(accepted)++queryAccepted;else ++queryRejected;
  }
  std::atomic<unsigned> unsupportedCalls{0};
  // The first answer to Elite's eye-transform call at a world scale the last answer did not carry.
  unsigned worldNotes = 0; uint32_t worldNotedPercent = 0; uint64_t worldNotedSequence = 0;
  void noteWorldScaleRead(uint32_t percent, uint64_t sequence) noexcept override {
    std::lock_guard<std::mutex> lock(mutex); ++worldNotes; worldNotedPercent = percent; worldNotedSequence = sequence;
  }

  explicit FakeSource() {
    auto in = geometry();
    check(edvr::openxr::makeGeometrySnapshot(in, original), "make fake geometry snapshot");
    state.generation = in.generation; state.connected = true; state.geometryValid = true;
    state.recommendedWidth[0] = in.width[0]; state.recommendedWidth[1] = in.width[1];
    state.recommendedHeight[0] = in.height[0]; state.recommendedHeight[1] = in.height[1];
    state.focusKnown = true; state.focused = true; state.geometry = original;
    state.opticsValid = true; state.optics.generation = in.generation; state.optics.sequence = in.sequence;
    for (unsigned eye = 0; eye != 2; ++eye) {
      state.optics.raw[eye] = original.raw[eye];
      state.optics.eyeToHead[eye] = original.eyeToHead[eye];
    }
    state.adapterIndex = 7;
    std::strcpy(state.runtimeName, "fake-openxr-runtime");
    std::strcpy(state.systemName, "fake-headset");
    state.displayFrequencyAvailable = true; state.displayFrequency = 90.0f;
    state.seatedToStandingValid = true; state.rawToStandingValid = true;
    state.seatedToStanding.m[0][0] = 11; state.rawToStanding.m[1][1] = 22;
  }
  SystemRead read() const override { std::lock_guard<std::mutex> lock(mutex); return state; }
  bool locateHead(uint64_t generation, vr::ETrackingUniverseOrigin origin,
                  float prediction, vr::TrackedDevicePose_t& out) override {
    std::lock_guard<std::mutex> lock(mutex);
    ++locateCalls; locatedGeneration = generation; locatedOrigin = origin; locatedPrediction = prediction;
    out = {}; out.bDeviceIsConnected=true;out.bPoseIsValid = true; out.eTrackingResult = vr::TrackingResult_Running_OK;
    out.mDeviceToAbsoluteTracking.m[0][0] = float(generation);
    out.mDeviceToAbsoluteTracking.m[0][3] = prediction;
    return generation == state.generation && state.connected;
  }
  // The head-pose answer: the call the system made, as locateHeadFor was handed it, and the calls that never got there.
  unsigned forCalls = 0, failedNotes = 0;
  edvr::openxr::HeadCall lastCall{}, lastFailed{};
  float lastFailedPrediction = 0;
  bool locateHeadFor(uint64_t generation, vr::ETrackingUniverseOrigin origin, float prediction,
                     const edvr::openxr::HeadCall& call, vr::TrackedDevicePose_t& out) override {
    { std::lock_guard<std::mutex> lock(mutex); ++forCalls; lastCall = call; }
    return locateHead(generation, origin, prediction, out);
  }
  void noteHeadCallFailed(const edvr::openxr::HeadCall& call, float prediction) noexcept override {
    std::lock_guard<std::mutex> lock(mutex); ++failedNotes; lastFailed = call; lastFailedPrediction = prediction;
  }
  bool resetSeated(uint64_t generation) override {
    std::lock_guard<std::mutex> lock(mutex); ++resetCalls; return generation == state.generation;
  }
  bool pollEvent(uint64_t generation, vr::ETrackingUniverseOrigin origin,
                 vr::VREvent_t& event, vr::TrackedDevicePose_t& pose) override {
    std::lock_guard<std::mutex> lock(mutex); ++pollCalls; (void)origin;
    if (generation != state.generation || !state.connected) return false;
    event = {}; event.eventType = vr::VREvent_TrackedDeviceActivated; event.trackedDeviceIndex = 0;
    pose = {}; pose.bPoseIsValid = true;pose.mDeviceToAbsoluteTracking.m[2][3]=1234; return true;
  }
  void noteProjection(uint64_t sequence,uint32_t eye,float nearZ,float farZ) noexcept override {
    std::lock_guard<std::mutex> lock(mutex); ++projectionNotes;
    notedSequence=sequence;notedEye=eye;notedNear=nearZ;notedFar=farZ;
  }
  void unsupported(unsigned) noexcept override { ++unsupportedCalls; }
  void retire() { std::lock_guard<std::mutex> lock(mutex); state.connected = false; state.geometryValid = false; }
};

bool allZero(const void* p, size_t n) { const auto* b = static_cast<const unsigned char*>(p); for (size_t i=0;i<n;++i) if (b[i]) return false; return true; }

void boundaryTests(vr::IVRSystem* system,FakeSource& source) {
  const char expected[]="fake-headset";const uint32_t needed=sizeof(expected);
  for(uint32_t capacity:{0u,1u,needed-1,needed,needed+8}) {
    unsigned char bytes[64];std::memset(bytes,0xcc,sizeof(bytes));vr::ETrackedPropertyError error{};
    const auto got=system->GetStringTrackedDeviceProperty(0,vr::Prop_ModelNumber_String,reinterpret_cast<char*>(bytes),capacity,&error);
    check(got==needed&&error==(capacity<needed?vr::TrackedProp_BufferTooSmall:vr::TrackedProp_Success),"string exact required length/error");
    if(capacity>=needed)check(std::memcmp(bytes,expected,needed)==0,"exact string including terminator");
    else if(capacity)check(bytes[0]==0,"short string cleared");
    const uint32_t touched=capacity>=needed?needed:capacity?1:0;
    bool intact=true;for(unsigned n=touched;n<sizeof(bytes);++n)intact&=bytes[n]==0xcc;
    check(intact,"string suffix and capacity canary");
  }
  vr::ETrackedPropertyError e{};
  for(uint32_t capacity:{0u,needed+10})check(system->GetStringTrackedDeviceProperty(0,vr::Prop_ModelNumber_String,nullptr,capacity,&e)==needed&&e==vr::TrackedProp_BufferTooSmall,"null string destination reports required capacity");
  char buffer[8]{};
  check(system->GetStringTrackedDeviceProperty(99,vr::Prop_ModelNumber_String,buffer,sizeof(buffer),&e)==0&&e==vr::TrackedProp_InvalidDevice,"exact invalid-device property error");
  check(system->GetFloatTrackedDeviceProperty(0,vr::Prop_ModelNumber_String,&e)==0&&e==vr::TrackedProp_WrongDataType,"exact wrong-type property error");
  check(system->GetStringTrackedDeviceProperty(0,static_cast<vr::ETrackedDeviceProperty>(9999),buffer,sizeof(buffer),&e)==0&&e==vr::TrackedProp_UnknownProperty,"exact unknown-property error");
  check(system->GetStringTrackedDeviceProperty(0,vr::Prop_ManufacturerName_String,buffer,sizeof(buffer),&e)==0&&e==vr::TrackedProp_ValueNotProvidedByDevice,"manufacturer not fabricated");
  for(auto property:{vr::Prop_EdidVendorID_Int32,vr::Prop_EdidProductID_Int32})check(system->GetInt32TrackedDeviceProperty(0,property,&e)==0&&e==vr::TrackedProp_ValueNotProvidedByDevice,"EDID not fabricated");
  check(!system->GetBoolTrackedDeviceProperty(0,vr::Prop_ReportsTimeSinceVSync_Bool,&e)&&e==vr::TrackedProp_Success,"no physical-vsync reporting advertised");
  check(!system->GetBoolTrackedDeviceProperty(0,vr::Prop_DeviceIsWireless_Bool,&e)&&e==vr::TrackedProp_ValueNotProvidedByDevice,"unavailable known bool is not successful false");
  for(unsigned variant=0;variant<3;++variant){source.state.displayFrequencyAvailable=variant!=0;source.state.displayFrequency=variant==1?NAN:0;
    check(system->GetFloatTrackedDeviceProperty(0,vr::Prop_DisplayFrequency_Float,&e)==0&&e==vr::TrackedProp_ValueNotProvidedByDevice,"no display frequency inferred from frame cadence");}
  source.state.displayFrequencyAvailable=true;source.state.displayFrequency=90;
  vr::TrackedDevicePose_t poses[4]{};std::memset(poses+3,0xcd,sizeof(poses[3]));const auto canary=poses[3];
  system->GetDeviceToAbsoluteTrackingPose(vr::TrackingUniverseSeated,-.25f,poses,3);
  check(source.locatedOrigin==vr::TrackingUniverseSeated&&source.locatedPrediction==-.25f,"negative pose prediction preserved");
  check(std::memcmp(poses+3,&canary,sizeof(canary))==0,"pose array capacity canary");
  for(unsigned i=1;i<3;++i)check(!poses[i].bPoseIsValid&&!poses[i].bDeviceIsConnected&&poses[i].eTrackingResult==vr::TrackingResult_Uninitialized&&poses[i].mDeviceToAbsoluteTracking.m[0][0]==1,"non-HMD pose initialized invalid/disconnected");
  const auto before=source.locateCalls;
  system->GetDeviceToAbsoluteTrackingPose(static_cast<vr::ETrackingUniverseOrigin>(99),0,poses,1);
  system->GetDeviceToAbsoluteTrackingPose(vr::TrackingUniverseSeated,NAN,poses,1);
  check(source.locateCalls==before&&!poses[0].bPoseIsValid&&poses[0].bDeviceIsConnected,"invalid pose request never dispatches or fabricates tracking");
  struct EventGuard {vr::VREvent_t event;unsigned char suffix[8];} event{};std::memset(&event,0xcd,sizeof(event));
  const auto polls=source.pollCalls;const uint32_t shortSize=sizeof(event.event)-1;
  check(!system->PollNextEvent(&event.event,shortSize)&&source.pollCalls==polls,"short event buffer cannot consume queue");
  const auto* bytes=reinterpret_cast<const unsigned char*>(&event);
  check(allZero(bytes,shortSize)&&bytes[shortSize]==0xcd&&event.suffix[0]==0xcd,"event byte capacity respected");
  vr::TrackedDevicePose_t eventPose{};check(system->PollNextEventWithPose(vr::TrackingUniverseSeated,&event.event,sizeof(event.event),&eventPose)&&eventPose.mDeviceToAbsoluteTracking.m[2][3]==1234,"event uses associated historical pose");
  vr::VRControllerState_t controller;std::memset(&controller,0xcd,sizeof(controller));
  check(!system->GetControllerStateWithPose(vr::TrackingUniverseSeated,0,&controller,&eventPose)&&allZero(&controller,sizeof(controller))&&!eventPose.bPoseIsValid&&!eventPose.bDeviceIsConnected,"controller outputs initialized on failure");
  float seconds=12;uint64_t frame=123;check(!system->GetTimeSinceLastVsync(&seconds,&frame)&&seconds==0&&frame==0,"vsync unavailable outputs initialized");
  system->ApplyTransform(&eventPose,nullptr,nullptr);check(!eventPose.bPoseIsValid&&!eventPose.bDeviceIsConnected,"unsupported transform cannot claim valid pose");
}

void capabilityTests() {
  using namespace edvr::openxr;
  FakeSource source;OpenVRSystem system(source);vr::ETrackedPropertyError error{};
  const auto saved=source.state;
  check(near(system.GetFloatTrackedDeviceProperty(0,vr::Prop_UserIpdMeters_Float,&error),.12f)&&error==vr::TrackedProp_Success,
    "IPD uses full eye separation, including non-X displacement");
  source.state.geometryValid=false;source.state.opticsValid=true;
  source.state.optics.generation=source.state.generation;source.state.optics.sequence=1;
  std::memcpy(source.state.optics.eyeToHead,saved.geometry.eyeToHead,sizeof(source.state.optics.eyeToHead));
  check(near(system.GetFloatTrackedDeviceProperty(0,vr::Prop_UserIpdMeters_Float,nullptr),.12f),"IPD survives temporary tracking invalidation");
  source.state.optics.eyeToHead[1].m[0][3]=NAN;
  check(system.GetFloatTrackedDeviceProperty(0,vr::Prop_UserIpdMeters_Float,&error)==0&&error==vr::TrackedProp_ValueNotProvidedByDevice,
    "invalid eye separation never returns successful zero or NaN");
  source.state.optics.generation++;
  check(system.GetFloatTrackedDeviceProperty(0,vr::Prop_UserIpdMeters_Float,&error)==0&&error==vr::TrackedProp_ValueNotProvidedByDevice,"IPD rejects stale-generation optics");
  check(system.GetFloatTrackedDeviceProperty(1,vr::Prop_UserIpdMeters_Float,&error)==0&&error==vr::TrackedProp_InvalidDevice,"IPD validates device index");
  check(!system.GetBoolTrackedDeviceProperty(0,vr::Prop_UserIpdMeters_Float,&error)&&error==vr::TrackedProp_WrongDataType,"IPD preserves typed property contract");
  source.state=saved;
  auto masks=std::make_shared<NativeHiddenMasks>();masks->generation=source.state.generation;masks->revision=1;
  // Asymmetric view and non-unit tangents; the triangle maps to (0,0),
  // (0,1),(1,0) before the guard. This catches treating XR rays as UVs.
  for(unsigned e=0;e<2;++e) {
    source.state.geometry.raw[e]={-2,1,-1,3};
    masks->eyes[e].vertices={{-2,-1},{-2,3},{1,-1}};masks->eyes[e].indices={0,1,2};
    masks->guard[e][0]=masks->guard[e][1]=.01f;
  }
  source.state.hiddenMasks=masks;
  auto left=system.GetHiddenAreaMesh(vr::Eye_Left),right=system.GetHiddenAreaMesh(vr::Eye_Right);
  check(left.unTriangleCount==1&&right.unTriangleCount==1&&left.pVertexData!=right.pVertexData,"both eyes receive distinct immutable runtime meshes");
  check(near(left.pVertexData[0].v[0],.01f)&&near(left.pVertexData[0].v[1],.01f)&&
    left.pVertexData[1].v[1]>.9f&&left.pVertexData[2].v[0]>.9f,"mask axes and asymmetric projection match OpenVR UV convention");
  bool inside=true;
  for(unsigned i=0;i<3;++i)inside&=left.pVertexData[i].v[0]>=.00999f&&left.pVertexData[i].v[1]>=.00999f&&
      left.pVertexData[i].v[0]+left.pVertexData[i].v[1]<=1-.014f;
  check(inside,"every inset vertex stays strictly inside the original hidden triangle");
  const auto oldVertex=left.pVertexData[1];
  source.state.tangentShift[0][0]=.123f;
  check(system.GetHiddenAreaMesh(vr::Eye_Left).pVertexData==left.pVertexData,"jitter cannot allocate or move the retained mask");
  source.state.recommendedWidth[0]/=2;
  check(system.GetHiddenAreaMesh(vr::Eye_Left).pVertexData==left.pVertexData,"resolution changes retain normalized mask and guard");
  source.state.hiddenMasksCompatible=false;
  check(!system.GetHiddenAreaMesh(vr::Eye_Left).pVertexData,"experimental modified frustum does not receive a native mask");
  source.state.hiddenMasksCompatible=true;
  auto updated=std::make_shared<NativeHiddenMasks>(*masks);updated->revision=2;
  updated->eyes[0].indices={0,2,1};source.state.hiddenMasks=updated;
  check(system.GetHiddenAreaMesh(vr::Eye_Left).unTriangleCount==1&&
    std::memcmp(&oldVertex,&left.pVertexData[1],sizeof(oldVertex))==0,"runtime replacement preserves old caller pointers and both windings");
  updated=std::make_shared<NativeHiddenMasks>(*masks);updated->revision=3;updated->eyes[0].indices[0]=999;
  source.state.hiddenMasks=updated;
  check(!system.GetHiddenAreaMesh(vr::Eye_Left).pVertexData,"invalid index cannot escape conversion");
  source.state.hiddenMasks.reset();
  check(!system.GetHiddenAreaMesh(vr::Eye_Left).pVertexData&&left.pVertexData[1].v[1]==oldVertex.v[1],"mask invalidation does not free previously returned storage");
  for(unsigned revision=4;revision<75;++revision) {
    updated=std::make_shared<NativeHiddenMasks>(*masks);updated->revision=revision;source.state.hiddenMasks=updated;
    system.GetHiddenAreaMesh(vr::Eye_Left);
  }
  check(!system.GetHiddenAreaMesh(vr::Eye_Left).pVertexData&&left.pVertexData[1].v[1]==oldVertex.v[1],"mask retention limit refuses new data without dangling old pointers");
  {
    // Elite makes one vertex buffer per eye from the counts it is handed, and a
    // zero-byte buffer fails CreateBuffer with E_INVALIDARG, which Elite treats
    // as fatal (a Reverb G2 on SteamVR: left 29 triangles, right none, crash 4 s
    // in). An eye is therefore served a mesh only when the other eye is too.
    const NativeHiddenMask triangle=masks->eyes[0];
    NativeHiddenMask none,broken=triangle,sliver=triangle;
    broken.indices[0]=999;
    // Three nearly collinear points: a nonzero area, but the inset corners land
    // millions of units away. One such triangle used to discard the whole eye.
    sliver.vertices.push_back({-2,-1});sliver.vertices.push_back({1,3});sliver.vertices.push_back({-.5f,1.0000001f});
    sliver.indices.push_back(3);sliver.indices.push_back(4);sliver.indices.push_back(5);
    struct Served{vr::HiddenAreaMesh_t eye[2];const char* reason;uint32_t dropped;unsigned servedMask;unsigned servedNotes;};
    const auto serve=[&](const NativeHiddenMask& l,const NativeHiddenMask& r)->Served {
      FakeSource paired;OpenVRSystem pairedSystem(paired);paired.state=source.state;
      auto both=std::make_shared<NativeHiddenMasks>(*masks);both->revision=1;both->eyes[0]=l;both->eyes[1]=r;
      paired.state.hiddenMasks=both;
      Served out{};out.eye[0]=pairedSystem.GetHiddenAreaMesh(vr::Eye_Left);
      out.eye[1]=pairedSystem.GetHiddenAreaMesh(vr::Eye_Right);
      out.reason=paired.lastMeshReason;out.dropped=paired.lastMeshDropped;
      out.servedMask=paired.servedMask;out.servedNotes=paired.servedNotes;return out;
    };
    auto served=serve(triangle,triangle);
    check(served.eye[0].unTriangleCount==1&&served.eye[1].unTriangleCount==1&&!std::strcmp(served.reason,"runtime"),
      "two populated eyes are both served");
    check(served.servedMask==3&&served.servedNotes==2,
      "each served eye is reported as served, once per answer (the desktop window's crop rule reads it)");
    served=serve(triangle,none);
    check(!served.eye[0].pVertexData&&!served.eye[0].unTriangleCount&&!served.eye[1].pVertexData&&!served.eye[1].unTriangleCount,
      "a populated left with an empty right serves neither eye (the zero-byte vertex buffer)");
    check(!std::strcmp(served.reason,"empty"),"the empty eye says empty");
    check(served.servedMask==0&&served.servedNotes==0,"an answer that carried no mesh is never reported as served (control: the served case above is)");
    served=serve(none,triangle);
    check(!served.eye[0].pVertexData&&!served.eye[1].pVertexData&&!std::strcmp(served.reason,"unpaired"),
      "an empty left with a populated right serves neither eye, and the populated one says unpaired");
    check(served.servedMask==0&&served.servedNotes==0,"unpaired is not served either");
    served=serve(triangle,broken);
    check(!served.eye[0].pVertexData&&!served.eye[1].pVertexData&&!std::strcmp(served.reason,"invalid_mesh"),
      "a malformed right eye takes the left eye's mesh with it");
    check(served.servedMask==0,"a malformed right eye serves neither eye, and neither is reported as served");
    served=serve(triangle,sliver);
    check(served.eye[0].unTriangleCount==1&&served.eye[1].unTriangleCount==1&&served.dropped==1&&!std::strcmp(served.reason,"runtime"),
      "a sliver triangle is dropped alone, the eye's other triangle and the other eye stay");
    check(served.servedMask==3,"and both eyes are still reported as served");
  }
  source.state.connected=false;
  check(!system.GetHiddenAreaMesh(vr::Eye_Right).pVertexData&&
    system.GetFloatTrackedDeviceProperty(0,vr::Prop_UserIpdMeters_Float,&error)==0&&error==vr::TrackedProp_InvalidDevice,"retirement removes current IPD and mask availability");
  check(!system.GetHiddenAreaMesh(static_cast<vr::EVREye>(99)).pVertexData,"invalid eye returns empty mesh");
  FakeSource diagnostic;OpenVRSystem observed(diagnostic);
  for(unsigned i=0;i<100;++i)observed.GetStringTrackedDeviceProperty(0,vr::Prop_ManufacturerName_String,nullptr,0,nullptr);
  check(diagnostic.propertyNotes==1&&diagnostic.lastProperty==vr::Prop_ManufacturerName_String&&
    diagnostic.lastError==vr::TrackedProp_ValueNotProvidedByDevice,"property diagnostic records exact missing ID/error once, with null caller error pointer");
  for(unsigned i=0;i<200;++i)observed.GetInt32TrackedDeviceProperty(0,static_cast<vr::ETrackedDeviceProperty>(8000+i),nullptr);
  check(diagnostic.propertyNotes==128,"property diagnostics have a fixed lifetime budget");
  SystemPublication publication;SystemRead metadata{};metadata.connected=true;
  const auto generation=publication.begin(metadata);masks=std::make_shared<NativeHiddenMasks>();masks->generation=generation;
  check(publication.hiddenMasks(generation,masks)&&publication.publish(geometry(generation,1),false,false,nullptr,false)&&
    !publication.read().hiddenMasksCompatible,"mask compatibility publishes with matching geometry");
  publication.invalidate(generation);
  check(publication.read().hiddenMasks==masks,"recenter preserves immutable runtime mask");
  publication.retire(generation);
  check(!publication.read().hiddenMasks&&!publication.hiddenMasks(generation,masks),"retirement prevents mask resurrection");
  check(publication.begin(metadata)>generation&&!publication.hiddenMasks(generation,masks),"stale mask cannot enter replacement session");
}

void publicationTest() {
  edvr::openxr::SystemPublication publication;
  SystemRead metadata{}; metadata.connected = true; metadata.adapterIndex = 42;
  metadata.recommendedWidth[0]=640; metadata.recommendedWidth[1]=672;
  metadata.recommendedHeight[0]=480; metadata.recommendedHeight[1]=496;
  const uint64_t generation = publication.begin(metadata);
  check(generation != 0 && publication.begin(metadata) == 0, "publication cannot begin twice");
  FakeSource source;source.state=publication.read();edvr::openxr::OpenVRSystem system(source);
  uint32_t width=0,height=0;system.GetRecommendedRenderTargetSize(&width,&height);
  check(width==672&&height==496&&!source.state.geometryValid,"size API uses session recommendations before first located geometry");
  auto valid = geometry(generation, 1);
  check(publication.publish(valid, true, true), "publication accepts first valid frame");
  auto read = publication.read(); check(read.connected && read.geometryValid && read.opticsValid && read.generation == generation, "publication read has connected geometry and optics");
  check(read.optics.generation == generation && read.optics.sequence == 1 &&
        std::memcmp(read.optics.raw, read.geometry.raw, sizeof(read.optics.raw)) == 0 &&
        std::memcmp(read.optics.eyeToHead, read.geometry.eyeToHead, sizeof(read.optics.eyeToHead)) == 0,
        "publication caches unjittered optics from the accepted frame");
  check(read.recommendedWidth[0]==640&&read.recommendedWidth[1]==672&&
        read.recommendedHeight[0]==480&&read.recommendedHeight[1]==496,
        "publication stores session display dimensions before geometry");
  check(!publication.publish(geometry(generation, 1), true, true), "publication rejects stale sequence");
  const float shifts[2][2]={{.125f,-.25f},{-.375f,.5f}};
  check(publication.publish(geometry(generation, 2), true, true, shifts), "publication accepts finite tangent shifts");
  read=publication.read(); check(read.tangentShift[0][0]==.125f&&read.tangentShift[0][1]==-.25f&&
      read.tangentShift[1][0]==-.375f&&read.tangentShift[1][1]==.5f, "publication stores per-eye shifts");
  const float badShifts[2][2]={{NAN,0},{0,0}};
  publication.invalidate(generation);source.state=publication.read();
  // Even if a caller retains a stale copy of the old frame shifts, fallback
  // optics are explicitly unjittered.
  source.state.tangentShift[0][0]=9;source.state.tangentShift[0][1]=-9;
  width=height=0;system.GetRecommendedRenderTargetSize(&width,&height);
  check(width==672&&height==496&&!source.state.geometryValid,"explicit origin invalidation preserves size API");
  const unsigned projectionNotesBeforeFallback=source.projectionNotes;
  const auto retained=system.GetProjectionMatrix(vr::Eye_Left,.025f,50000.f,vr::API_DirectX);
  check(near(retained.m[0][0],2.f/(.8422884f+.6841368f)) &&
        near(retained.m[0][2],(.8422884f-.6841368f)/(.8422884f+.6841368f)),
        "origin invalidation preserves cached unjittered projection");
  float cachedLeft,cachedRight,cachedTop,cachedBottom;
  system.GetProjectionRaw(vr::Eye_Left,&cachedLeft,&cachedRight,&cachedTop,&cachedBottom);
  check(near(cachedLeft,-.6841368f) && near(cachedRight,.8422884f) &&
        near(cachedTop,-.4227932f) && near(cachedBottom,.5463025f) &&
        near(system.GetEyeToHeadTransform(vr::Eye_Left).m[2][3],.06f),
        "origin invalidation preserves cached raw FOV and eye placement (the eye's tz in Elite's handedness)");
  check(source.projectionNotes==projectionNotesBeforeFallback,
        "cached projection fallback does not advertise a stale temporal sequence");
  check(!publication.publish(geometry(generation, 3), true, true, badShifts), "publication rejects non-finite shifts");
  read=publication.read(); check(read.geometry.native.sequence==0&&!read.geometryValid&&read.opticsValid&&
      read.tangentShift[0][0]==0, "invalid shifts retire visible publication");
  check(read.recommendedWidth[0]==640&&read.recommendedWidth[1]==672&&
        read.recommendedHeight[0]==480&&read.recommendedHeight[1]==496,
        "invalid geometry preserves session display dimensions");
  check(!publication.publish(geometry(generation, 3), true, true), "invalid shift sequence cannot be replayed");
  auto invalid = geometry(generation, 2); invalid.headFlags = 0;
  invalid.sequence=4; check(!publication.publish(invalid, true, false), "publication rejects invalid tracking");
  read = publication.read(); check(read.connected && !read.geometryValid && read.opticsValid && read.focusKnown && !read.focused, "invalid tracking clears geometry but keeps HMD connected");
  check(read.optics.sequence == 3 && near(read.optics.raw[0].left,-.6841368f),
        "invalid tracking retains the last valid unjittered optics");
  check(read.recommendedWidth[0]==640&&read.recommendedWidth[1]==672&&
        read.recommendedHeight[0]==480&&read.recommendedHeight[1]==496,
        "invalid tracking preserves session display dimensions");
  check(read.tangentShift[0][0]==0&&read.tangentShift[0][1]==0&&read.tangentShift[1][0]==0&&read.tangentShift[1][1]==0, "invalid tracking clears shifts");
  check(!publication.publish(geometry(generation - 1, 3), true, true), "publication rejects stale generation");
  publication.retire(generation); read=publication.read(); check(!read.connected && !read.opticsValid && !read.recommendedWidth[0] && !read.recommendedWidth[1] &&
      !read.recommendedHeight[0] && !read.recommendedHeight[1], "retired publication clears display dimensions");
  metadata.tangentShift[0][0]=9;metadata.tangentShift[1][1]=-9;
  const auto fresh=publication.begin(metadata);check(fresh>generation,"new generation increases");
  read=publication.read();check(read.tangentShift[0][0]==0&&read.tangentShift[1][1]==0&&!read.opticsValid,"new generation clears shifts and optics");
  FakeSource freshState;freshState.state=read;edvr::openxr::OpenVRSystem freshSystem(freshState);
  float freshLeft=1,freshRight=1,freshTop=1,freshBottom=1;
  freshSystem.GetProjectionRaw(vr::Eye_Left,&freshLeft,&freshRight,&freshTop,&freshBottom);
  check(allZero(&freshLeft,sizeof(freshLeft))&&allZero(&freshRight,sizeof(freshRight))&&
        allZero(&freshTop,sizeof(freshTop))&&allZero(&freshBottom,sizeof(freshBottom)),
        "fresh generation has no optics fallback before its first valid frame");
  check(!publication.publish(geometry(generation,999),true,true),"old writer cannot enter new generation");
  publication.retire(generation);check(publication.read().connected,"old cleanup cannot retire new generation");
  check(publication.publish(geometry(fresh,1),true,true),"new generation starts sequence anew");
  std::atomic<bool> torn{false};std::atomic<unsigned> reads{0};
  auto sample=[&](uint64_t sequence){auto value=geometry(fresh,sequence);value.displayTime=XrTime(sequence*10);value.width[0]=uint32_t(sequence+100);value.headPose.position.x=float(sequence);return value;};
  publication.publish(sample(2),true,true);
  std::thread writer([&]{for(uint64_t n=3;n<5000;++n)publication.publish(sample(n),true,(n%2)==0);});
  auto reader=[&]{for(unsigned n=0;n<10000;++n){const auto value=publication.read();++reads;
    const auto sequence=value.geometry.native.sequence;
    if(value.generation!=fresh||!value.geometryValid||value.geometry.native.width[0]!=sequence+100||
       value.geometry.native.headPose.position.x!=float(sequence)||value.geometry.native.displayTime!=XrTime(sequence*10)||
       value.focused!=((sequence%2)==0))torn=true;
  }};
  std::thread one(reader),two(reader);writer.join();one.join();two.join();
  check(reads==20000&&!torn,"metadata and geometry stay coherent during concurrent publication");
}

void oddRecommendationTest() {
  using namespace edvr::openxr;
  SystemPublication publication;
  SystemRead metadata{}; metadata.connected = true;
  metadata.recommendedWidth[0] = 3964; metadata.recommendedWidth[1] = 3965;
  metadata.recommendedHeight[0] = metadata.recommendedHeight[1] = 3913;
  const uint64_t generation = publication.begin(metadata);
  check(generation != 0 && metadata.recommendedWidth[0] == 3964 && metadata.recommendedWidth[1] == 3965 &&
        metadata.recommendedHeight[0] == 3913 && metadata.recommendedHeight[1] == 3913,
        "odd bootstrap metadata remains unchanged at the caller");
  auto read = publication.read();
  check(read.recommendedWidth[0] == 3964 && read.recommendedWidth[1] == 3966 &&
        read.recommendedHeight[0] == 3914 && read.recommendedHeight[1] == 3914,
        "odd bootstrap recommendations are normalized per eye");

  FakeSource source; source.state = read;
  OpenVRSystem system(source);
  uint32_t width = 0, height = 0;
  system.GetRecommendedRenderTargetSize(&width, &height);
  check(width == 3966 && height == 3914 && !source.state.geometryValid,
        "OpenVR size query returns even bootstrap maxima before geometry");

  const uint32_t replacementWidth[2] = {4111, 4223};
  const uint32_t replacementHeight[2] = {3001, 3003};
  publication.recommend(replacementWidth, replacementHeight);
  read = publication.read();
  check(read.recommendedWidth[0] == 4112 && read.recommendedWidth[1] == 4224 &&
        read.recommendedHeight[0] == 3002 && read.recommendedHeight[1] == 3004,
        "recommend aligns every eye dimension to even");
  const auto beforeInvalid = read;
  const uint32_t badWidth[2] = {5001, 0};
  const uint32_t badHeight[2] = {3001, 3001};
  publication.recommend(badWidth, badHeight);
  read = publication.read();
  check(read.recommendedWidth[0] == beforeInvalid.recommendedWidth[0] &&
        read.recommendedWidth[1] == beforeInvalid.recommendedWidth[1] &&
        read.recommendedHeight[0] == beforeInvalid.recommendedHeight[0] &&
        read.recommendedHeight[1] == beforeInvalid.recommendedHeight[1],
        "invalid recommendation leaves all eyes unchanged");

  check(publication.publish(geometry(generation, 1), true, true), "valid tracking accepts geometry with even recommendations");
  auto invalid = geometry(generation, 2); invalid.headFlags = 0;
  check(!publication.publish(invalid, true, false), "invalid tracking rejects geometry");
  source.state = publication.read();
  system.GetRecommendedRenderTargetSize(&width, &height);
  check(width == 4224 && height == 3004 && !source.state.geometryValid,
        "invalid tracking retains even recommendations");
  publication.invalidate(generation);
  source.state = publication.read();
  system.GetRecommendedRenderTargetSize(&width, &height);
  check(width == 4224 && height == 3004 && !source.state.geometryValid,
        "recenter invalidation retains even recommendations");

  publication.retire(generation);
  source.state = publication.read();
  width = height = 0xdeadbeef;
  system.GetRecommendedRenderTargetSize(&width, &height);
  check(width == 0 && height == 0 && !source.state.recommendedWidth[0] &&
        !source.state.recommendedHeight[0], "retirement clears recommendations");
}

// Elite's "now" head pose, through the historical virtual ABI (docs/terrain-culling.md): which GetDeviceToAbsoluteTrackingPose calls are flagged for the drawn
// frame's display time, judged by a return address inside the executable and a prediction under 5 ms either way, and what reaches the source.
__declspec(noinline) void headPoseTests(vr::IVRSystem* system, FakeSource& source, edvr::openxr::OpenVRSystem& concrete) {
  using edvr::openxr::ExeModule;
  // ---- Elite's "now" head pose: which GetDeviceToAbsoluteTrackingPose calls are answered one display period after the drawn frame's display time ----
  {
    const ExeModule realExe = concrete.exeModule();
    concrete.useExeModule(realExe);
    vr::TrackedDevicePose_t poses[3]{};
    const auto lastCall = [&] { std::lock_guard<std::mutex> lock(source.mutex); return source.lastCall; };
    const unsigned callsBefore = [&] { std::lock_guard<std::mutex> lock(source.mutex); return source.forCalls; }();
    openxrAbiCallPose(system, vr::TrackingUniverseSeated, 0.0f, poses, 3);
    const edvr::openxr::HeadCall learned = lastCall();
    unsigned callsAfter = 0;
    { std::lock_guard<std::mutex> lock(source.mutex); callsAfter = source.forCalls; }
    const uintptr_t poseCaller = reinterpret_cast<uintptr_t>(&openxrAbiCallPose);
    check(callsAfter == callsBefore + 1 && learned.thread == GetCurrentThreadId() && learned.display && learned.rva < edvr::openxr::kFrameUnknown &&
              realExe.base + learned.rva > poseCaller && realExe.base + learned.rva - poseCaller < 0x100,
          "a pose call from inside the game's image (here this rig's own executable) with a prediction of 0 reaches the source with the CALLER's thread, its return address as an RVA, and the display flag set");
    const uintptr_t poseSite = realExe.base + learned.rva;
    const auto askAt = [&](float prediction) {
      vr::TrackedDevicePose_t p[2]{};
      openxrAbiCallPose(system, vr::TrackingUniverseSeated, prediction, p, 2);
      return lastCall();
    };
    // The prediction filter, on both signs and at its boundary.
    bool nowRequests = true, predictions = true;
    for (float p : {0.0f, -0.0f, 0.001f, 0.0049f, 0.004999f, -0.001f, -0.0049f, -0.004999f}) nowRequests = nowRequests && askAt(p).display;
    for (float p : {0.005f, -0.005f, 0.0051f, -0.0051f, 0.011f, -0.011f, 0.25f, -0.25f, 1.0f}) predictions = predictions && !askAt(p).display;
    check(nowRequests, "a prediction under 5 ms either way (4.999 ms, -4.999 ms, 0) from inside the image is Elite's \"now\": flagged for the display time");
    check(predictions, "...exactly 5 ms either way, a frame period (11 ms), a quarter second and a second are real predictions: not flagged");
    // The image: the same call, with the executable mapped elsewhere.
    concrete.useExeModule(ExeModule{poseSite + 0x1000, 0x40000000, 0, 0});
    const auto below = askAt(0.0f);
    concrete.useExeModule(ExeModule{poseSite - 0x10, 0x10, 0, 0});
    const auto pastEnd = askAt(0.0f);
    concrete.useExeModule(ExeModule{poseSite - 0x10, 0x11, 0, 0});
    const auto lastByte = askAt(0.0f);
    concrete.useExeModule(ExeModule{});
    const auto unknown = askAt(0.0f);
    check(!below.display && below.rva == edvr::openxr::kFrameOutside && !pastEnd.display && pastEnd.rva == edvr::openxr::kFrameOutside && lastByte.display && lastByte.rva == 0x10 &&
              !unknown.display && unknown.rva == edvr::openxr::kFrameOutside,
          "a return address before the image, one past its end, or any address when the image is unknown is outside it: not flagged, whatever the prediction; the image's last byte is inside");
    // No build is asked for: the same call is flagged under another build's stamp.
    concrete.useExeModule(ExeModule{poseSite - 0x4E3881, 0x40000000, 1788384821u, 104894465u});
    const auto otherBuild = askAt(0.0f);
    check(otherBuild.display && otherBuild.rva == 0x4E3881,
          "no build is asked for: an executable of any stamp and size flags the same call, so the fix survives a game update");
    concrete.useExeModule(realExe);
    // The answer is the source's, untouched: the filter changes the instant, never the pose.
    vr::TrackedDevicePose_t shifted[3]{};
    openxrAbiCallPose(system, vr::TrackingUniverseSeated, 0.0f, shifted, 3);
    check(shifted[0].bPoseIsValid && shifted[0].mDeviceToAbsoluteTracking.m[0][3] == 0.0f && !shifted[1].bPoseIsValid && !shifted[2].bPoseIsValid,
          "the pose a flagged call is handed back is the source's, and the other poses stay invalid");
    // Calls that never reach the source are counted, with who asked.
    unsigned notesBefore = 0;
    { std::lock_guard<std::mutex> lock(source.mutex); notesBefore = source.failedNotes; }
    openxrAbiCallPose(system, static_cast<vr::ETrackingUniverseOrigin>(99), 0.0f, poses, 1);
    openxrAbiCallPose(system, vr::TrackingUniverseSeated, NAN, poses, 1);
    unsigned notes = 0;
    edvr::openxr::HeadCall failed{};
    float failedPrediction = 0;
    { std::lock_guard<std::mutex> lock(source.mutex); notes = source.failedNotes; failed = source.lastFailed; failedPrediction = source.lastFailedPrediction; }
    check(notes == notesBefore + 2 && failed.thread == GetCurrentThreadId() && failed.rva == learned.rva && std::isnan(failedPrediction),
          "a bad origin and a prediction that is not a number are counted as failed pose calls, with the caller's thread and RVA");
    const bool wasConnected = source.state.connected;
    source.state.connected = false;
    unsigned countedBefore = 0;
    { std::lock_guard<std::mutex> lock(source.mutex); countedBefore = source.failedNotes; }
    openxrAbiCallPose(system, vr::TrackingUniverseSeated, 0.0f, poses, 1);
    { std::lock_guard<std::mutex> lock(source.mutex); check(source.failedNotes == countedBefore + 1, "...and so is a call with no live session"); }
    source.state.connected = wasConnected;
    concrete.useExeModule(realExe);
  }
}
// Two call sites of the raw getter, for the caller the scope zoom's report names (not folded into one: their bodies differ).
__declspec(noinline) float rawReadSiteA(edvr::openxr::OpenVRSystem& system) {float l=0,r=0,t=0,b=0;system.GetProjectionRaw(vr::Eye_Left,&l,&r,&t,&b);return l;}
volatile float g_rawReadSink=0.f;
__declspec(noinline) void rawReadSiteB(edvr::openxr::OpenVRSystem& system) {float l=0,r=0,t=0,b=0;system.GetProjectionRaw(vr::Eye_Left,&l,&r,&t,&b);g_rawReadSink=t+b;}
// B3, the scope zoom's window (scope_zoom_state.h, system_publication.h): eye 0's told frustum is narrowed, the projection matrix getter answers the narrowed one, the raw getter (which the game's
// UI scale reads) the frustum without the zoom, eye 1 is untouched, the hidden-area mesh is cut for the true frustum, and the runtime decides to narrow only in the cases it should.
void scopeZoomTest() {
  using edvr::openxr::ScopeZoomInput;using edvr::openxr::ScopeZoomOutput;using edvr::openxr::ScopeZoomWhy;using edvr::openxr::decideScopeZoom;using edvr::openxr::RawFov;using edvr::openxr::SystemPublication;
  namespace sw=edvr::scopewin;
  ScopeZoomInput in;
  in.answerCarries=true;in.share=1.f;in.shareDrawn=true;in.base={-0.6f,0.7f,0.5f,-0.4f};
  in.request.zoom=true;in.request.m=4.f;in.request.tc[0]=0.001f;in.request.tc[1]=-0.02f;
  auto out=decideScopeZoom(in);
  RawFov trueRaw{};edvr::openxr::fovToRaw(in.base,trueRaw);
  const double spanX=double(trueRaw.right)-trueRaw.left,spanY=double(trueRaw.bottom)-trueRaw.top;
  check(out.why==ScopeZoomWhy::kApplied&&near(float(out.narrowing),4.f,1e-3f),"scope zoom: asked, covered and drawn, the told frustum narrows by M");
  check(std::fabs((out.zoomTan[1]-out.zoomTan[0])-spanX/4.0)<1e-5&&std::fabs((out.zoomTan[3]-out.zoomTan[2])-spanY/4.0)<1e-5&&
        std::fabs((out.zoomTan[0]+out.zoomTan[1])*0.5-0.001)<1e-5&&std::fabs((out.zoomTan[2]+out.zoomTan[3])*0.5+0.02)<1e-5,"scope zoom: the window is the eye's span over M, centred on the asked direction");
  check(out.fov.angleLeft<out.fov.angleRight&&out.fov.angleDown<out.fov.angleUp&&out.fov.angleLeft>=in.base.angleLeft&&out.fov.angleRight<=in.base.angleRight&&
        out.fov.angleDown>=in.base.angleDown&&out.fov.angleUp<=in.base.angleUp,"scope zoom: and lies inside the frustum it narrows");
  auto no=[&](ScopeZoomInput x,ScopeZoomWhy why,const char* text){const auto o=decideScopeZoom(x);check(o.why==why&&std::memcmp(&o.fov,&x.base,sizeof(x.base))==0,text);};
  { auto x=in;x.request.zoom=false;no(x,ScopeZoomWhy::kNotAsked,"scope zoom: not asked leaves the frustum alone"); }
  { auto x=in;x.answerCarries=false;no(x,ScopeZoomWhy::kVersion,"scope zoom: an answer that is not version 11 carries no zoom, and says so when it asked"); }
  { auto x=in;x.answerCarries=false;x.request.zoom=false;no(x,ScopeZoomWhy::kNotAsked,"scope zoom: ... and nothing when it did not ask"); }
  { auto x=in;x.share=0.95f;no(x,ScopeZoomWhy::kShare,"scope zoom: the right eye's picture must cover the left wholly (a share under full weight refuses)"); }
  { auto x=in;x.shareDrawn=false;no(x,ScopeZoomWhy::kShareNotDrawn,"scope zoom: and the share must have been drawn the frame before"); }
  { auto x=in;x.shareDrawn=false;x.standing=true;x.notDrawnRun=0;const auto o=decideScopeZoom(x);
    check(o.why==ScopeZoomWhy::kApplied&&o.passedUnshared&&near(float(o.narrowing),4.f,1e-3f),"scope zoom: ... but a zoom that stands goes on past one frame of a share not drawn");
    x.notDrawnRun=1;no(x,ScopeZoomWhy::kShareNotDrawn,"scope zoom: ... not past a second in a row");
    x.notDrawnRun=0;x.standing=false;no(x,ScopeZoomWhy::kShareNotDrawn,"scope zoom: ... and not when it does not stand yet");
    x.standing=true;x.shareRefused=true;no(x,ScopeZoomWhy::kShareNotDrawn,"scope zoom: ... and not past a share that was refused");
    x.shareRefused=false;x.share=0.95f;no(x,ScopeZoomWhy::kShare,"scope zoom: ... and the share must still be at full weight");
    const auto drawn=decideScopeZoom(in);check(drawn.why==ScopeZoomWhy::kApplied&&!drawn.passedUnshared,"scope zoom: a drawn share passes nothing"); }
  { auto x=in;x.request.edge=edvr::scopewin::kEdgeStretched;check(decideScopeZoom(x).edge==edvr::scopewin::kEdgeStretched,"scope zoom: the fill mode comes out with the decision");
    x.request.zoom=false;check(decideScopeZoom(x).edge==edvr::scopewin::kEdgeStretched,"scope zoom: ... and with the strip alone");
    x.answerCarries=false;check(decideScopeZoom(x).edge==edvr::scopewin::kEdgeBlack,"scope zoom: ... but not from an answer that carries nothing"); }
  { auto x=in;x.shareRefused=true;no(x,ScopeZoomWhy::kShareNotDrawn,"scope zoom: ... and not refused"); }
  { auto x=in;x.request.tc[0]=1.5f;no(x,ScopeZoomWhy::kGeometry,"scope zoom: a centre outside the eye's span makes no window"); }
  { auto x=in;x.base={0.1f,0.1f,0.5f,-0.4f};no(x,ScopeZoomWhy::kGeometry,"scope zoom: an unusable frustum makes no window"); }
  { auto x=in;x.request.strip=0.7f;check(decideScopeZoom(x).strip==0.7f,"scope zoom: the strip crosses with the zoom");x.request.zoom=false;check(decideScopeZoom(x).strip==0.7f,"scope zoom: ... and without it (the strip is black before the zoom is asked for)");
    x.answerCarries=false;check(decideScopeZoom(x).strip==0.f,"scope zoom: ... but not from an answer that carries nothing"); }
  // the press as the runtime counts it, and its report: a refused frame before the press is carried into it, the frames are counted, and the press ends when a frame is not applied
  edvr::openxr::ScopeZoomRun run;
  ScopeZoomOutput refusedShare;refusedShare.why=ScopeZoomWhy::kShare;
  auto ev=run.frame(refusedShare,900000);
  check(!ev.opened&&!ev.closed&&ev.whyChanged&&!run.press.open&&run.pending[1]==1u,"scope zoom run: a refusal before any press is counted and said once");
  ev=run.frame(refusedShare,950000);
  check(!ev.whyChanged&&run.pending[1]==2u,"scope zoom run: ... and not said again for the same reason");
  ev=run.frame(out,1000000);
  check(ev.opened&&!ev.closed&&run.press.open&&run.press.refused[1]==2u&&run.pending[1]==0u,"scope zoom run: the first applied frame opens the press, which takes the refusals of the run of asks before it");
  { auto x=in;x.request.strip=1.f;ev=run.frame(decideScopeZoom(x),1500000); }   // one frame with the strip black
  ev=run.frame(out,2000000);
  check(!ev.opened&&!ev.closed&&run.press.frames==3u,"scope zoom run: the frames of a press are counted and none opens it again");
  run.press.rawReads=12;run.press.matrixReads=34;run.press.rawSpan=edvr::openxr::rawSpanV(trueRaw);run.press.matrixSpan=run.press.rawSpan/4.f;
  run.press.callerRva[0]=0x4E3C93u;run.press.callerReads[0]=9u;run.press.callerRva[1]=0x2842AE3u;run.press.callerReads[1]=3u;run.press.callerOther=0;
  EdvrNativeScopeZoomReport report{};run.press.fill(report);
  check(report.frames==3&&report.refused[1]==2&&report.refused[0]==0&&report.stripFrames==1&&report.meshWithheld==3&&near(report.seconds,1.f,1e-4f)&&near(report.peakM,4.f,1e-3f)&&
        report.rawReads==12&&report.matrixReads==34&&sw::reportReadable(&report)&&near(report.rawSpan/report.matrixSpan,4.f,1e-4f)&&report.rawCallerRva[0]==0x4E3C93u&&report.rawCallerReads[1]==3u&&report.rawCallerOther==0u,
        "scope zoom: a press counts its frames, its refusals by reason, the strip's and the mesh's frames, what the game read and who read the raw getter, and the report is readable");
  ScopeZoomOutput askedOff;
  ev=run.frame(askedOff,2500000);
  check(ev.closed&&std::strcmp(ev.closed,"asked_off")==0&&!ev.opened&&run.press.open,"scope zoom run: a frame no longer asked for ends the press (the host reports it and closes it)");
  run.press.open=false;
  ev=run.frame(refusedShare,2600000);run.frame(refusedShare,2700000);
  ev=run.frame(askedOff,2800000);
  check(run.pending[1]==0u&&!ev.closed,"scope zoom run: the refusals of a run of asks that ended are not carried into the next press");
  run.frame(refusedShare,3000000);
  ev=run.frame(out,3100000);
  check(ev.opened&&run.press.refused[1]==1u&&run.press.frames==1u,"scope zoom run: a press counts only the refusals of its own run of asks, and its own frames");
  ScopeZoomOutput geom;geom.why=ScopeZoomWhy::kGeometry;
  ev=run.frame(geom,3200000);
  check(ev.closed&&std::strcmp(ev.closed,"refused")==0&&ev.whyChanged&&run.pending[3]==1u,"scope zoom run: a refusal ends an open press, is said, and counts toward the next one of its run");
  run.press.open=false;
  run.frame(refusedShare,3300000);
  check(run.absent()==nullptr&&run.pending[1]==0u&&run.pending[3]==0u,"scope zoom run: a frame with no answer or no geometry ends the run of asks");
  run.frame(out,3400000);
  check(run.absent()!=nullptr&&std::strcmp(run.absent(),"no_geometry")==0,"scope zoom run: ... and says so when a press was open");
  // who read the raw getter: a fixed table of the first callers and the rest counted as other, lock-free
  edvr::openxr::ScopeZoomCallers callers;
  uint32_t rva[EDVR_NATIVE_SCOPE_ZOOM_REPORT_CALLERS]{},reads[EDVR_NATIVE_SCOPE_ZOOM_REPORT_CALLERS]{},other=7;
  callers.top(rva,reads,other);
  check(reads[0]==0u&&other==0u,"scope zoom callers: none read, none named");
  for(int i=0;i<4;++i)callers.note(0x4E3C93u);
  for(int i=0;i<2;++i)callers.note(0x2842AE3u);
  callers.note(0xFFFFFFFFu);
  for(uint32_t i=0;i<7;++i)callers.note(0x100u+i);
  callers.top(rva,reads,other);
  check(rva[0]==0x4E3C93u&&reads[0]==4u&&rva[1]==0x2842AE3u&&reads[1]==2u&&rva[2]==0xFFFFFFFFu&&reads[2]==1u&&rva[3]==0x100u&&reads[3]==1u&&other==6u&&
        reads[0]+reads[1]+reads[2]+reads[3]+other==14u,
        "scope zoom callers: the four busiest by reads, ties by first seen, every other read counted as other (nine callers for a table of eight), and the total is the reads");
  callers.reset();callers.top(rva,reads,other);
  check(reads[0]==0u&&reads[3]==0u&&other==0u,"scope zoom callers: a reset forgets them all");
  {
    std::thread a([&]{for(int i=0;i<20000;++i)callers.note(0x10u+uint32_t(i%3));});
    std::thread b([&]{for(int i=0;i<20000;++i)callers.note(0x10u+uint32_t(i%5));});
    a.join();b.join();callers.top(rva,reads,other);
    check(uint64_t(reads[0])+reads[1]+reads[2]+reads[3]+other==40000u,"scope zoom callers: two threads reading at once lose no read");
  }
  // the publication: matrix getter narrowed, raw getter true
  SystemPublication publication;SystemRead metadata{};metadata.connected=true;
  const auto generation=publication.begin(metadata);
  GeometryInput g=geometry(generation,1);
  RawFov told[2]{};edvr::openxr::fovToRaw(g.views[0].fov,told[0]);edvr::openxr::fovToRaw(g.views[1].fov,told[1]);
  GeometryInput narrowed=geometry(generation,2);narrowed.views[0].fov={-0.15f,0.19f,0.13f,-0.1f};
  check(publication.publish(narrowed,false,false,nullptr,false,told),"scope zoom: a frame with eye 0 narrowed publishes");
  FakeSource source;source.state=publication.read();edvr::openxr::OpenVRSystem system(source);
  float l,r,t0,b;
  system.GetProjectionRaw(vr::Eye_Left,&l,&r,&t0,&b);
  check(near(l,told[0].left)&&near(r,told[0].right)&&near(t0,told[0].top)&&near(b,told[0].bottom),"scope zoom: the raw getter, which the game's UI scale reads, answers eye 0's frustum without the zoom");
  const auto matrix=system.GetProjectionMatrix(vr::Eye_Left,.025f,50000.f,vr::API_DirectX);
  RawFov zoomRaw{};edvr::openxr::fovToRaw(narrowed.views[0].fov,zoomRaw);
  check(near(matrix.m[0][0],2.f/(zoomRaw.right-zoomRaw.left),1e-2f)&&near(matrix.m[1][1],2.f/(zoomRaw.bottom-zoomRaw.top),1e-2f)&&matrix.m[0][0]>2.f*(2.f/(told[0].right-told[0].left)),
        "scope zoom: the projection matrix getter, which the picture is drawn with, answers the narrowed frustum");
  float l1,r1,t1,b1;system.GetProjectionRaw(vr::Eye_Right,&l1,&r1,&t1,&b1);
  const auto matrix1=system.GetProjectionMatrix(vr::Eye_Right,.025f,50000.f,vr::API_DirectX);
  check(near(l1,told[1].left)&&near(r1,told[1].right)&&near(matrix1.m[0][0],2.f/(told[1].right-told[1].left),1e-2f),"scope zoom: eye 1 is untouched in both getters");
  check(source.reads[0][1].load()==1&&source.reads[0][0].load()==1&&source.reads[1][1].load()==1&&source.reads[1][0].load()==1&&source.heldReads.load()==2,
        "scope zoom: the reads of each getter are noted per eye, and eye 0's two are noted as held back from the raw getter");
  // what each getter ANSWERED, as the span the game was given (the matrix getter's from the matrix itself), not the frusta the runtime told: their ratio is the narrowing exactly
  {
    const float rawSpan=source.lastSpan[0][1].load(),matrixSpan=source.lastSpan[0][0].load();
    const double toldNarrowing=(double(told[0].bottom)-told[0].top)/(double(zoomRaw.bottom)-zoomRaw.top);
    check(rawSpan>0.f&&matrixSpan>0.f&&near(rawSpan,told[0].bottom-told[0].top,1e-5f)&&near(matrixSpan,zoomRaw.bottom-zoomRaw.top,1e-5f)&&std::fabs(double(rawSpan)/double(matrixSpan)-toldNarrowing)<1e-4*toldNarrowing,
          "scope zoom: each getter reports the vertical tangent span it answered eye 0 with, and their ratio is the narrowing told");
    check(near(edvr::openxr::matrixSpanV(matrix),zoomRaw.bottom-zoomRaw.top,1e-5f)&&near(edvr::openxr::rawSpanV(told[0]),told[0].bottom-told[0].top,1e-6f)&&edvr::openxr::matrixSpanV(vr::HmdMatrix44_t{})==0.f&&
          edvr::openxr::rawSpanV(RawFov{})==0.f,"scope zoom: the span of a frustum and of the matrix made from it agree, and an unusable one is 0");
    // an off-axis window: the angle sum the first build logged is not 1 / M, the tangent span is
    const double M=4.0;const double hx=0.5*(double(told[0].bottom)-told[0].top)/M;
    const double top=double(told[0].bottom)-2.0*hx,bot=double(told[0].bottom);   // a window against the frustum's upper edge
    const double angleTrue=std::atan(std::fabs(double(told[0].top)))+std::atan(std::fabs(double(told[0].bottom)));
    const double angleZoom=std::atan(std::fabs(top))+std::atan(std::fabs(bot));
    check(std::fabs(angleTrue/angleZoom-M)>0.05*M&&std::fabs((double(told[0].bottom)-told[0].top)/(bot-top)-M)<1e-9,
          "scope zoom: for a window off the axis the angle sum is not 1 / M, and the tangent span is");
  }
  // who asked: the caller's RVA, from the system object's own record of the game's image (exe_module.h frameRva), for the raw getter and the matrix getter alike
  {
    rawReadSiteA(system);
    const uint32_t rvaA=source.lastCaller[0][1].load();
    rawReadSiteB(system);
    const uint32_t rvaB=source.lastCaller[0][1].load();
    rawReadSiteA(system);
    check(rvaA!=rvaB&&rvaA!=edvr::openxr::kFrameOutside&&rvaB!=edvr::openxr::kFrameOutside&&rvaA<system.exeModule().size&&rvaB<system.exeModule().size&&source.lastCaller[0][1].load()==rvaA,
          "scope zoom: the raw getter says who read it, as an RVA in the game's image: two call sites differ and one site is the same twice");
    system.GetProjectionMatrix(vr::Eye_Left,.025f,50000.f,vr::API_DirectX);
    const uint32_t rvaM=source.lastCaller[0][0].load();
    check(rvaM!=edvr::openxr::kFrameOutside&&rvaM<system.exeModule().size,"scope zoom: the matrix getter says who read it too");
    const edvr::openxr::ExeModule realExe=system.exeModule();
    system.useExeModule(edvr::openxr::ExeModule{reinterpret_cast<uintptr_t>(&rawReadSiteA)-0x10,0x1000,0,0});
    rawReadSiteA(system);
    const uint32_t mapped=source.lastCaller[0][1].load();
    check(mapped>=0x10u&&mapped<0x400u,"scope zoom: the RVA is the return address less the image's base (a synthetic image mapped over the call site)");
    system.useExeModule(edvr::openxr::ExeModule{});
    rawReadSiteA(system);
    check(source.lastCaller[0][1].load()==edvr::openxr::kFrameOutside,"scope zoom: with no image to judge against, or a caller outside it, the RVA is the outside marker");
    system.useExeModule(realExe);
  }
  check(!source.state.hiddenMasksCompatible,"scope zoom: the hidden-area mesh is withheld (published as not compatible)");
  // optics keep the true frustum, so a frame without tracking cannot serve the narrowed one
  publication.invalidate(generation);source.state=publication.read();
  float lo,ro,to,bo;system.GetProjectionRaw(vr::Eye_Left,&lo,&ro,&to,&bo);
  const auto matrixOptics=system.GetProjectionMatrix(vr::Eye_Left,.025f,50000.f,vr::API_DirectX);
  check(near(lo,told[0].left)&&near(ro,told[0].right)&&near(matrixOptics.m[0][0],2.f/(told[0].right-told[0].left),1e-2f),"scope zoom: the cached optics hold the true frustum for both getters when the geometry is gone");
  // the next frame without the zoom publishes both getters alike again
  SystemPublication publication2;const auto gen2=publication2.begin(metadata);
  check(publication2.publish(geometry(gen2,1),false,false,nullptr,true,told),"scope zoom: a frame");
  check(publication2.publish(geometry(gen2,2),false,false,nullptr,true),"scope zoom: the next frame without the zoom");
  FakeSource source2;source2.state=publication2.read();edvr::openxr::OpenVRSystem system2(source2);
  float l2,r2,t2,b2;system2.GetProjectionRaw(vr::Eye_Left,&l2,&r2,&t2,&b2);
  check(!source2.state.toldRawSet&&near(l2,told[0].left)&&near(r2,told[0].right),"scope zoom: without the zoom the raw getter answers the game's geometry, as before");
  system2.GetProjectionMatrix(vr::Eye_Left,.025f,50000.f,vr::API_DirectX);
  check(source2.heldReads.load()==0,"scope zoom: and no read is noted as held back");
}
// B3, the two halves together. The frame thread's tick (scope_zoom_b3.h b3Step and b3IdleStep, which native_on_foot.cpp's tick calls) publishes the window once a tick; the provider's
// beginFrame reads it as the five words of a version 11 answer (scope_zoom_window.h); the runtime decides from them and the share (decideScopeZoom) and keeps the press (ScopeZoomRun).
// Driven frame by frame at 90 Hz with the game's WaitGetPoses after the frame boundary's tick, and before it (the one-frame latency the design states): ADS entry and exit, the share
// dropping, the key toggled and the weapon switched mid-press, a stalled frame thread, a lost tick, an answer that is not version 11, and a second press after a refused one. What must hold
// in all of them: the left eye is told the window only with the strip already black, within the latency of the request ending, and never from a request that went stale.
struct ZoomPipe {
  edvr::scopezoom::B3Loop loop;
  edvr::scopezoom::B3StepIn in;
  edvr::openxr::ScopeZoomRun run;
  uint64_t ms=100000;
  bool runtimeFirst=false,carries=true,shareDrawn=true,shareRefused=false;
  uint32_t edge=edvr::scopewin::kEdgeWorld;
  XrFovf base{-0.6f,0.7f,0.5f,-0.4f};
  struct Frame {bool applied=false,askedByTick=false,passed=false;uint32_t edge=0;float strip=0.f;edvr::openxr::ScopeZoomWhy why=edvr::openxr::ScopeZoomWhy::kNotAsked;const char* closed=nullptr;bool opened=false;};
  std::vector<Frame> frames;
  ZoomPipe() {
    edvr::scopewin::clear();
    in.keyOn=in.rendered=in.shareMode=in.qualifies=in.sightsFull=in.leftOk=true;
    in.share=1.0;in.dt=0.011;in.m=4.067;in.tcNow[0]=0.01;in.tcNow[1]=-0.02;
  }
  void runtime(Frame& f) {
    namespace sw=edvr::scopewin;
    uint32_t words[sw::kWords];
    sw::toWords(sw::read(ms+4),words);   // the provider's beginFrame: the frame thread's request, none once it is stale
    edvr::openxr::ScopeZoomInput zi;
    zi.answerCarries=carries;
    if(carries)zi.request=sw::fromWords(words);
    zi.share=float(in.share);zi.shareDrawn=shareDrawn;zi.shareRefused=shareRefused;zi.base=base;
    zi.standing=run.standing();zi.notDrawnRun=run.notDrawnRun;
    const auto out=edvr::openxr::decideScopeZoom(zi);
    const auto ev=run.frame(out,ms*1000);
    f.applied=out.why==edvr::openxr::ScopeZoomWhy::kApplied;f.strip=out.strip;f.why=out.why;f.closed=ev.closed;f.opened=ev.opened;f.edge=out.edge;f.passed=out.passedUnshared;
    if(ev.closed)run.press.open=false;   // the host's close
  }
  // One game frame. sightsDown: the tick is the sights-down one (b3IdleStep); stalled: the frame thread does not tick (a lost tick, or a stall) but the runtime still asks.
  Frame frame(bool sightsDown=false,bool stalled=false) {
    Frame f;ms+=11;
    if(runtimeFirst)runtime(f);
    if(!stalled) {
      if(sightsDown)edvr::scopewin::publish(edvr::scopezoom::b3IdleStep(loop,in.dt,edge),ms);
      else {in.edge=edge;const auto out=edvr::scopezoom::b3Step(loop,in);f.askedByTick=out.request.zoom;edvr::scopewin::publish(out.request,ms);}
    }
    if(!runtimeFirst)runtime(f);
    frames.push_back(f);
    return f;
  }
  int go(int n,bool down=false,bool stalled=false) {int applied=0;for(int i=0;i<n;++i)applied+=frame(down,stalled).applied?1:0;return applied;}
};

void scopeZoomPipelineTest() {
  using edvr::openxr::ScopeZoomWhy;
  for(int variant=0;variant<2;++variant) {   // 0: the game asks after the tick (the zoom reaches it the same frame); 1: before (a frame later)
    const char* tag=variant?"asked before the tick":"asked after the tick";
    const int late=variant;
    auto say=[&](const char* text){static char buffer[400];std::snprintf(buffer,sizeof(buffer),"scope zoom pipeline (%s): %s",tag,text);return buffer;};
    // ADS entry: black strip first, then the zoom, within a frame of the tick that asked
    {
      ZoomPipe p;p.runtimeFirst=variant==1;
      int firstApplied=-1,firstAsked=-1;bool stripBlack=true;
      for(int i=0;i<24;++i) {
        const auto f=p.frame();
        if(f.askedByTick&&firstAsked<0)firstAsked=i;
        if(f.applied&&firstApplied<0)firstApplied=i;
        if(f.applied)stripBlack=stripBlack&&f.strip>=0.999f;
      }
      check(firstAsked>=9&&firstAsked<=10&&firstApplied==firstAsked+late&&stripBlack,say("the zoom reaches the game on the frame the tick asked (a frame later when it asked before), with the strip black"));
      check(p.frames.back().applied&&p.run.press.open&&p.run.press.frames==unsigned(24-firstApplied),say("and stays applied, in one press"));
      // ADS exit: the sights come down. The request ends with the tick; the runtime stops within the latency
      int stillApplied=0;
      for(int i=0;i<6;++i)stillApplied+=p.frame(true).applied?1:0;
      check(stillApplied<=late&&!p.frames.back().applied&&p.run.press.frames>0,say("the sights down end the zoom at once (within the latency), and no frame after that is applied"));
      check(!p.frames[p.frames.size()-3].applied&&p.frames[p.frames.size()-1].why==ScopeZoomWhy::kNotAsked,say("the runtime reads nothing asked from then on"));
    }
    // the share between 0.9 and 0.999 while the window is held: the runtime refuses (the left eye keeps its own view), the window is not dropped, and it applies again when the share is full
    {
      ZoomPipe p;p.runtimeFirst=variant==1;
      p.go(14);
      p.in.share=0.95;
      int applied=p.go(5);
      check(applied<=late&&p.frames.back().why==ScopeZoomWhy::kShare&&p.loop.latched,say("a share at 0.95 refuses the narrowing (not at full weight) while the window stays held"));
      p.in.share=1.0;
      p.go(3);
      check(p.frames.back().applied,say("and the narrowing returns when the share is full again"));
    }
    // the share under 0.9: the window ends, the request ends
    {
      ZoomPipe p;p.runtimeFirst=variant==1;
      p.go(14);
      p.in.share=0.8;
      p.go(4);
      check(!p.frames.back().applied&&!p.loop.latched,say("a share under 0.9 ends the window and the zoom"));
    }
    // the key toggled mid-press, a weapon switch mid-press
    {
      ZoomPipe p;p.runtimeFirst=variant==1;
      p.go(14);
      p.in.keyOn=false;
      p.go(4);
      check(!p.frames.back().applied&&p.frames.back().why==ScopeZoomWhy::kNotAsked,say("the key off mid-press ends the zoom"));
      ZoomPipe w;w.runtimeFirst=variant==1;
      w.go(14);
      w.in.rendered=false;
      w.go(4);
      check(!w.frames.back().applied,say("a weapon switch to an entry that is not rendered ends the zoom"));
    }
    // a stalled frame thread: the request is read for 200 ms and not after; the strip decays after that
    {
      ZoomPipe p;p.runtimeFirst=variant==1;
      p.go(14);
      int appliedWhileStalled=0,lastApplied=-1;float stripAtEnd=1.f;
      for(int i=0;i<40;++i) {
        const auto f=p.frame(false,true);
        if(f.applied){++appliedWhileStalled;lastApplied=i;}
        stripAtEnd=f.strip;
      }
      const int silenceMs=(lastApplied+1)*11;
      check(appliedWhileStalled>=15&&appliedWhileStalled<=19&&silenceMs<=210&&!p.frames.back().applied&&stripAtEnd==0.f,
            say("a stalled frame thread leaves the zoom told for 200 ms of silence, then none at once, and the strip gone by 400 ms: no stalled tick can leave the left eye zoomed"));
    }
    // one tick lost: the last request stands
    {
      ZoomPipe p;p.runtimeFirst=variant==1;
      p.go(14);
      p.frame(false,true);
      p.frame(false,true);
      check(p.frames.back().applied&&p.frames[p.frames.size()-2].applied,say("a tick or two lost between ticks leaves the zoom applied"));
    }
    // an answer that is not version 11 carries nothing, and the strip with it
    {
      ZoomPipe p;p.runtimeFirst=variant==1;p.carries=false;
      const int applied=p.go(24);
      check(applied==0&&p.frames.back().strip==0.f&&p.run.press.frames==0u,say("an answer that is not version 11 is never narrowed, and darkens no strip"));
    }
    // a share the renderer did not draw: refused, whatever was asked
    {
      ZoomPipe p;p.runtimeFirst=variant==1;p.shareDrawn=false;
      const int applied=p.go(24);
      check(applied==0&&p.frames.back().why==ScopeZoomWhy::kShareNotDrawn,say("a share not drawn is never narrowed over"));
    }
    // two presses, the first ended by a refusal: the second reports only its own refusals
    {
      ZoomPipe p;p.runtimeFirst=variant==1;
      p.go(14);
      p.in.share=0.95;
      p.go(4);   // refused while held
      p.go(8,true);   // the sights come down: nothing asked, the run of asks ends
      check(p.run.pending[1]==0u&&p.run.pending[0]==0u&&p.run.pending[2]==0u&&p.run.pending[3]==0u,say("the sights down end the run of asks: the refusals of the press that was refused are not carried"));
      p.in.share=1.0;
      p.go(14);
      check(p.frames.back().applied&&p.run.press.open&&p.run.press.refused[1]==0u&&p.run.press.frames>0u,say("the second press opens with none of the first's refusals"));
    }
  }
  // B3-2: one frame in a row of a share that was not drawn does not end the zoom; two do; a share that was refused (not merely not drawn) does; and entering needs a drawn share
  for(int variant=0;variant<2;++variant) {
    const char* tag=variant?"asked before the tick":"asked after the tick";
    auto say=[&](const char* text){static char buffer[400];std::snprintf(buffer,sizeof(buffer),"scope zoom pipeline (%s, withheld share): %s",tag,text);return buffer;};
    {
      ZoomPipe p;p.runtimeFirst=variant==1;
      p.go(14);
      const unsigned before=p.run.press.frames;
      p.shareDrawn=false;
      const auto held=p.frame();
      p.shareDrawn=true;
      p.go(4);
      check(held.applied&&held.passed&&!held.closed&&p.run.press.open&&p.run.press.frames==before+5&&p.run.press.notDrawnPassed==1u&&p.frames.back().applied&&!p.frames.back().passed,
            say("one frame of a share not drawn passes without ending the zoom: one press, no split, the pass counted"));
      p.shareDrawn=false;p.frame();p.shareDrawn=true;p.go(1);p.shareDrawn=false;const auto again=p.frame();
      check(again.applied&&again.passed&&p.run.press.notDrawnPassed==3u&&!again.closed,say("and again after a drawn frame between (the run is one frame at a time)"));
    }
    {
      ZoomPipe p;p.runtimeFirst=variant==1;
      p.go(14);
      p.shareDrawn=false;
      const auto first=p.frame();
      const auto second=p.frame();
      check(first.applied&&first.passed&&!second.applied&&second.why==edvr::openxr::ScopeZoomWhy::kShareNotDrawn&&second.closed&&std::strcmp(second.closed,"refused")==0,
            say("two frames in a row end the zoom on the second: the left eye is narrowed on no frame beyond that"));
      p.shareDrawn=true;
      p.go(3);
      check(p.frames.back().applied&&p.run.press.open&&p.run.press.refused[2]==1u,say("and the zoom comes back with the share, the second press carrying its refusal"));
    }
    {
      ZoomPipe p;p.runtimeFirst=variant==1;
      p.go(14);
      p.shareDrawn=false;p.shareRefused=true;
      const auto f=p.frame();
      check(!f.applied&&f.why==edvr::openxr::ScopeZoomWhy::kShareNotDrawn&&f.closed,say("a share the renderer refused (views it could not map) is not passed"));
    }
    {
      ZoomPipe p;p.runtimeFirst=variant==1;
      p.go(8);
      p.shareDrawn=false;
      const int during=p.go(4);
      p.shareDrawn=true;
      const bool later=p.go(3)>0;
      check(during==0&&p.frames[11].why==edvr::openxr::ScopeZoomWhy::kShareNotDrawn&&!p.frames[11].passed&&later,say("before the zoom stands a share not drawn is refused, not passed, and the zoom starts when it is drawn"));
    }
    {   // the mode crosses with the strip before the zoom, with the zoom, and after it; an answer that is not version 11 carries none
      ZoomPipe p;p.runtimeFirst=variant==1;p.edge=edvr::scopewin::kEdgeView;
      p.frame();
      const auto first=p.frame();
      p.go(14);
      const auto zoomed=p.frames.back();
      const auto down=p.frame(true);
      check(first.edge==edvr::scopewin::kEdgeView&&zoomed.applied&&zoomed.edge==edvr::scopewin::kEdgeView&&down.edge==edvr::scopewin::kEdgeView,say("the fill mode reaches the runtime on the entry ramp, while the zoom stands and on the way out"));
      ZoomPipe q;q.runtimeFirst=variant==1;q.edge=edvr::scopewin::kEdgeStretched;q.carries=false;
      q.go(14);
      check(q.frames.back().edge==edvr::scopewin::kEdgeBlack,say("an answer that is not version 11 carries no mode: black"));
    }
  }
  // the compositor's fill, counted over a run of the strip
  {
    edvr::openxr::ScopeZoomFillTally t;
    using edvr::scopewin::kEdgeWorld;using edvr::scopewin::kEdgeBlack;
    t.note(0.f,false,true,0,false,false,0,0.f,0.f);
    check(!t.active&&t.frames[0]==0u,"scope zoom fill: no strip and no narrowing is no run, and counts nothing");
    t.note(0.3f,false,true,kEdgeWorld,true,false,0,0.f,0.f);t.note(0.6f,false,true,kEdgeWorld,true,false,0,0.f,0.f);
    t.note(1.f,true,true,kEdgeWorld,false,false,1,11.f,1.5f);t.note(1.f,true,true,kEdgeWorld,false,false,2,22.f,4.25f);t.note(1.f,true,true,kEdgeBlack,false,true,3,33.f,0.f);
    check(t.active&&t.frames[kEdgeWorld]==4u&&t.frames[kEdgeBlack]==1u&&t.copies==2u&&t.missing==1u&&t.ageFrames==3u&&near(t.ageMs,33.f,1e-3f)&&near(t.rotationMaxDeg,4.25f,1e-4f),
          "scope zoom fill: the frames by the fill drawn, the held copies made on the ramp, the narrowed frame black for want of a held picture, the held picture's age as of the last frame, and the largest turn");
    t.note(1.f,true,false,0,false,false,9,99.f,50.f);
    check(t.frames[kEdgeWorld]==4u&&t.ageFrames==3u&&t.rotationMaxDeg<5.f,"scope zoom fill: a frame with no share drawn counts nothing");
    t.note(0.f,false,true,0,false,false,0,0.f,0.f);
    check(!t.active&&t.frames[kEdgeWorld]==4u,"scope zoom fill: the run ends with the first frame with neither, and what it counted stays for the press's report");
    t.note(0.2f,false,true,kEdgeWorld,true,false,0,0.f,0.f);
    check(t.active&&t.frames[kEdgeWorld]==1u&&t.copies==1u&&t.missing==0u&&t.rotationMaxDeg==0.f,"scope zoom fill: the next run starts from nothing");
    edvr::openxr::ScopeZoomPress press;
    press.begin(1000);press.takeFill(t,kEdgeWorld);
    EdvrNativeScopeZoomReport report{};press.fill(report);
    check(report.edgeMode==kEdgeWorld&&report.fillFrames[1]==1u&&report.heldCopies==1u&&report.heldMissing==0u&&report.heldAgeFrames==t.ageFrames&&edvr::scopewin::reportReadable(&report),
          "scope zoom fill: the press takes the tally into its report");
  }
  // the report a press ends with is built from the run's press, and is what the d3d11 half is told
  {
    ZoomPipe p;
    p.go(14);
    EdvrNativeScopeZoomReport report{};p.run.press.fill(report);
    check(report.frames>=3u&&report.peakM>3.9f&&edvr::scopewin::reportReadable(&report),"scope zoom pipeline: the press the frames made fills a readable report");
  }
}
int selfTest() {
  scopeZoomTest();
  scopeZoomPipelineTest();
  publicationTest();
  oddRecommendationTest();
  capabilityTests();
  FakeSource source;
  edvr::openxr::OpenVRSystem concrete(source);
  vr::IVRSystem* system = openxrAbiCaller(&concrete);
  check(system != nullptr, "concrete object is reachable through IVRSystem");

  uint32_t width = 0xdeadbeef, height = 0xcafebabe;
  system->GetRecommendedRenderTargetSize(&width, &height);
  check(width == 1128 && height == 786, "recommended size returns both eye maxima");
  source.state.geometryValid=false; source.state.geometry={};
  width=height=0; system->GetRecommendedRenderTargetSize(&width,&height);
  check(width==1128&&height==786, "recommended size survives invalid live geometry");
  const auto retainedProjection=system->GetProjectionMatrix(vr::Eye_Left,.025f,50000.f,vr::API_DirectX);
  check(retainedProjection.m[0][0]>0.0f && near(retainedProjection.m[0][0],2.f/(.8422884f+.6841368f)),
        "projection survives invalid live geometry through optics cache");
  float retainedLeft=0,retainedRight=0,retainedTop=0,retainedBottom=0;
  system->GetProjectionRaw(vr::Eye_Left,&retainedLeft,&retainedRight,&retainedTop,&retainedBottom);
  check(near(retainedLeft,-.6841368f)&&near(retainedRight,.8422884f)&&
        near(retainedTop,-.4227932f)&&near(retainedBottom,.5463025f),
        "invalid live geometry uses unjittered cached raw FOV");
  source.state.geometry=source.original; source.state.geometryValid=true;
  vr::HmdMatrix44_t dx = system->GetProjectionMatrix(vr::Eye_Left, .025f, 50000.f, vr::API_DirectX);
  vr::HmdMatrix44_t gl = system->GetProjectionMatrix(vr::Eye_Right, .1f, 1000.f, vr::API_OpenGL);
  check(std::isfinite(dx.m[0][0]) && std::isfinite(gl.m[3][2]) && near(dx.m[3][2], -1), "by-value projection returns both conventions");
  float l=0,r=0,t=0,b=0; system->GetProjectionRaw(vr::Eye_Left,&l,&r,&t,&b);
  check(near(l, -0.6841368f) && near(r, 0.8422884f) && near(t, -0.4227932f) && near(b, 0.5463025f), "raw projection preserves all planes");
  {
    const float shifts[2][2]={{.125f,-.075f},{-.2f,.11f}};
    source.state.tangentShift[0][0]=shifts[0][0];source.state.tangentShift[0][1]=shifts[0][1];
    source.state.tangentShift[1][0]=shifts[1][0];source.state.tangentShift[1][1]=shifts[1][1];
    float sl,sr,st,sb; system->GetProjectionRaw(vr::Eye_Left,&sl,&sr,&st,&sb);
    check(near(sl,-.6841368f+shifts[0][0])&&near(sr,.8422884f+shifts[0][0])&&
          near(st,-.4227932f+shifts[0][1])&&near(sb,.5463025f+shifts[0][1]), "raw applies left shift");
    const auto shifted=system->GetProjectionMatrix(vr::Eye_Left,.1f,1000.f,vr::API_DirectX);
    check(near(shifted.m[0][2],(sl+sr)/(sr-sl))&&near(shifted.m[1][2],(st+sb)/(sb-st)), "matrix matches shifted raw");
    check(source.projectionNotes==3&&source.notedSequence==23&&source.notedEye==0&&
          near(source.notedNear,.1f)&&near(source.notedFar,1000.f), "matrix query notes exact frame");
    float rl,rr,rt,rb; system->GetProjectionRaw(vr::Eye_Right,&rl,&rr,&rt,&rb);
    const auto rightShifted=system->GetProjectionMatrix(vr::Eye_Right,.1f,1000.f,vr::API_OpenGL);
    check(near(rl,-.6841368f+shifts[1][0])&&near(rr,.8422884f+shifts[1][0])&&
          near(rt,-.4227932f+shifts[1][1])&&near(rb,.5463025f+shifts[1][1])&&
          near(rightShifted.m[0][2],(rl+rr)/(rr-rl))&&near(rightShifted.m[1][2],(rt+rb)/(rb-rt)),
          "raw and matrix stay coherent for right eye and OpenGL");
    source.state.tangentShift[0][0]=source.state.tangentShift[0][1]=0;
    source.state.tangentShift[1][0]=source.state.tangentShift[1][1]=0;
  }
  system->GetProjectionRaw(static_cast<vr::EVREye>(9),&l,&r,&t,&b);
  const float rawAfter[4] = {l,r,t,b};
  check(allZero(rawAfter, sizeof(rawAfter)), "invalid raw eye zeros outputs");
  auto eye0 = system->GetEyeToHeadTransform(vr::Eye_Left); auto eye1 = system->GetEyeToHeadTransform(vr::Eye_Right);
  check(near(eye0.m[2][3], .06f) && near(eye1.m[2][3], -.06f), "both eye transforms returned by value, in Elite's handedness (this fixture's eyes sit at tz -0.06 and +0.06)");
  {
    // The answer to the game is S*E*S, S = diag(1,1,-1) (docs\canted-projection.md), and nothing else moves. The transform the snapshot holds (the pair Elite is
    // told: the located eyes moved by the world scale, which is none here) is never touched, and the IPD property is made from it.
    const GeometrySnapshot kept = source.state.geometry;
    const SystemRead keptState = source.state;
    vr::ETrackedPropertyError ipdError = vr::TrackedProp_Success;
    const float ipd = system->GetFloatTrackedDeviceProperty(0, vr::Prop_UserIpdMeters_Float, &ipdError);
    auto on0 = system->GetEyeToHeadTransform(vr::Eye_Left); auto on1 = system->GetEyeToHeadTransform(vr::Eye_Right);
    check(near(on0.m[2][3], .06f) && near(on1.m[2][3], -.06f) && near(on0.m[0][3], kept.eyeToHead[0].m[0][3]) && near(on0.m[1][3], kept.eyeToHead[0].m[1][3]),
          "only tz changes sign for a pure translation (x and y stay)");
    check(std::memcmp(&source.state.geometry, &kept, sizeof(kept)) == 0 && std::memcmp(&source.state.optics, &keptState.optics, sizeof(keptState.optics)) == 0,
          "...while the snapshot and the cached optics are bit-identical afterwards (nothing reads the game-facing answer back)");
    check(std::fabs(system->GetFloatTrackedDeviceProperty(0, vr::Prop_UserIpdMeters_Float, &ipdError) - ipd) < 1e-7f && ipd > 0.1f,
          "...and the IPD property is the same after the calls (made from the snapshot's transform)");
    // A yawed, pitched eye shows the rotation entries: exactly R02, R12, R20, R21 and tz change sign.
    vr::HmdMatrix34_t yawed{};
    const double a = 10.0*3.14159265358979323846/180.0, p = 4.0*3.14159265358979323846/180.0;
    const double ry[3][3] = {{std::cos(a),0,std::sin(a)},{0,1,0},{-std::sin(a),0,std::cos(a)}};
    const double rx[3][3] = {{1,0,0},{0,std::cos(p),-std::sin(p)},{0,std::sin(p),std::cos(p)}};
    for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) { double v = 0; for (int k = 0; k < 3; ++k) v += ry[i][k]*rx[k][j]; yawed.m[i][j] = float(v); }
    yawed.m[0][3] = -.032f; yawed.m[1][3] = .004f; yawed.m[2][3] = -.011f;
    source.state.geometry.eyeToHead[0] = yawed;
    auto given = system->GetEyeToHeadTransform(vr::Eye_Left);
    bool flipped = true, kept2 = true;
    for (int i = 0; i < 3; ++i) for (int j = 0; j < 4; ++j) {
      const bool flips = (i == 2) != (j == 2);   // S*E*S: one index on the z row/column, not both (and the translation's z)
      if (flips) flipped = flipped && given.m[i][j] == 0.0f - yawed.m[i][j];
      else kept2 = kept2 && given.m[i][j] == yawed.m[i][j];
    }
    check(flipped && kept2 && yawed.m[0][2] != 0 && yawed.m[1][2] != 0 && yawed.m[2][0] != 0 && yawed.m[2][1] != 0,
          "a yawed and pitched eye: R02, R12, R20, R21 and tz are negated, the other seven entries are not");
    auto again = system->GetEyeToHeadTransform(vr::Eye_Left);
    check(std::memcmp(&again, &given, sizeof(given)) == 0 && std::memcmp(&source.state.geometry.eyeToHead[0], &yawed, sizeof(yawed)) == 0,
          "...asking again gives the same answer (the correction is not accumulated into the snapshot)");
    // With the tracking sample gone the cached optics answer, and are corrected the same way.
    source.state.geometry = kept; source.state.geometryValid = false;
    source.state.optics.eyeToHead[1] = yawed;
    auto cached = system->GetEyeToHeadTransform(vr::Eye_Right);
    check(cached.m[0][2] == 0.0f - yawed.m[0][2] && cached.m[2][3] == 0.0f - yawed.m[2][3] && cached.m[0][0] == yawed.m[0][0] && cached.m[0][3] == yawed.m[0][3],
          "no live geometry: the cached optics are corrected the same way");
    source.state = keptState;
  }
  {
    // World scale (common/world_scale.h; docs/world-scale.md): a snapshot made at p percent holds the located eyes moved about their midpoint to 100/p of their
    // distance, rotations untouched; Elite's answer is S*E*S of it as ever, and the IPD property is the separation it is told. 100 is today's answer to the bit.
    check(source.worldNotes == 0, "at 100 percent the eye-transform answers wrote no world scale note");
    const double yaw = 10.0*3.14159265358979323846/180.0;
    GeometryInput canted = geometry();
    canted.views[0].pose.orientation = {0.0f, float(std::sin(yaw/2)), 0.0f, float(std::cos(yaw/2))};
    canted.views[1].pose.orientation = {0.0f, float(-std::sin(yaw/2)), 0.0f, float(std::cos(yaw/2))};
    auto at = [&](uint32_t percent, GeometrySnapshot& out) { GeometryInput in = canted; in.worldScalePercent = percent; return edvr::openxr::makeGeometrySnapshot(in, out); };
    GeometrySnapshot s100{}, s130{}, s80{}, s50{}, s200{}, untouched{};
    check(at(100, s100) && at(130, s130) && at(80, s80) && at(50, s50) && at(200, s200) && edvr::openxr::makeGeometrySnapshot(canted, untouched), "snapshots at 50, 80, 100, 130 and 200 percent");
    check(std::memcmp(&s100.eyeToHead, &untouched.eyeToHead, sizeof(s100.eyeToHead)) == 0 && untouched.native.worldScalePercent == 100,
          "100 percent is the default and the same pair to the bit");
    const double was = edvr::openxr::eyeSeparation(s100.eyeToHead);
    bool aboutMidpoint = true, rotationsKept = true;
    const struct { const GeometrySnapshot* snap; double percent; } cases[] = {{&s130, 130}, {&s80, 80}, {&s50, 50}, {&s200, 200}};
    for (const auto& c : cases) {
      for (int axis = 0; axis < 3; ++axis) {
        const double mid = 0.5*(double(s100.eyeToHead[0].m[axis][3]) + s100.eyeToHead[1].m[axis][3]);
        for (int eye = 0; eye < 2; ++eye) {
          const double want = mid + (double(s100.eyeToHead[eye].m[axis][3]) - mid)*100.0/c.percent;
          aboutMidpoint = aboutMidpoint && std::fabs(double(c.snap->eyeToHead[eye].m[axis][3]) - want) < 1e-6;
        }
      }
      for (int eye = 0; eye < 2; ++eye) for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j)
        rotationsKept = rotationsKept && c.snap->eyeToHead[eye].m[i][j] == s100.eyeToHead[eye].m[i][j];
    }
    check(aboutMidpoint, "50, 80, 130 and 200 percent move both eye origins about their midpoint by 100 over the percent");
    check(rotationsKept && s130.eyeToHead[0].m[0][2] != 0.0f, "...and leave every rotation entry alone (a canted panel keeps its yaw)");
    check(std::fabs(edvr::openxr::eyeSeparation(s130.eyeToHead) - was*100.0/130.0) < 1e-6 && edvr::openxr::eyeSeparation(s130.eyeToHead) < was &&
              edvr::openxr::eyeSeparation(s80.eyeToHead) > was, "130 percent brings the eyes closer (a bigger world), 80 percent moves them apart");
    check(std::memcmp(&s130.headToLocal, &s100.headToLocal, sizeof(s100.headToLocal)) == 0 && std::memcmp(s130.raw, s100.raw, sizeof(s100.raw)) == 0 &&
              std::memcmp(&s130.native.views, &s100.native.views, sizeof(s100.native.views)) == 0 && s130.native.worldScalePercent == 130,
          "...while the head, the field of view and the located views the snapshot carries are unchanged");
    GeometrySnapshot refused{};
    GeometryInput bad = canted;
    bool refusedAll = true;
    for (uint32_t percent : {0u, 49u, 201u, 1000u}) { bad.worldScalePercent = percent; refusedAll = refusedAll && !edvr::openxr::makeGeometrySnapshot(bad, refused); }
    check(refusedAll, "a percent outside 50..200 (0 included) is no geometry");

    // Through the system: S*E*S of the told pair, the IPD property, and one note for each new value (never one per call).
    const SystemRead keptWorld = source.state;
    auto serve = [&](const GeometrySnapshot& snap) {
      source.state.geometry = snap; source.state.geometryValid = true; source.state.optics.generation = snap.native.generation; source.state.optics.sequence = snap.native.sequence;
      for (int eye = 0; eye < 2; ++eye) source.state.optics.eyeToHead[eye] = snap.eyeToHead[eye];
      source.state.optics.worldScalePercent = snap.native.worldScalePercent;
    };
    serve(s130);
    vr::ETrackedPropertyError scaledError = vr::TrackedProp_Success;
    const float scaledIpd = system->GetFloatTrackedDeviceProperty(0, vr::Prop_UserIpdMeters_Float, &scaledError);
    check(scaledError == vr::TrackedProp_Success && std::fabs(scaledIpd - float(edvr::openxr::eyeSeparation(s130.eyeToHead))) < 1e-7f &&
              std::fabs(scaledIpd - float(was*100.0/130.0)) < 1e-6f, "the IPD property is the separation Elite is told (130 percent)");
    bool handed = true;
    for (int eye = 0; eye < 2; ++eye) {
      const auto given = system->GetEyeToHeadTransform(eye ? vr::Eye_Right : vr::Eye_Left);
      const auto& held = s130.eyeToHead[eye];
      for (int i = 0; i < 3; ++i) for (int j = 0; j < 4; ++j) {
        const bool flips = (i == 2) != (j == 2);
        handed = handed && given.m[i][j] == (flips ? 0.0f - held.m[i][j] : held.m[i][j]);
      }
    }
    check(handed, "Elite's answer at 130 percent is S*E*S of the scaled pair, as it is of the located one at 100");
    for (int again = 0; again < 5; ++again) { system->GetEyeToHeadTransform(vr::Eye_Left); system->GetEyeToHeadTransform(vr::Eye_Right); }
    { std::lock_guard<std::mutex> lock(source.mutex);
      check(source.worldNotes == 1 && source.worldNotedPercent == 130 && source.worldNotedSequence == s130.native.sequence,
            "twelve answers at 130 percent wrote one note, naming 130 and the geometry sequence"); }
    serve(s100);
    system->GetEyeToHeadTransform(vr::Eye_Left); system->GetEyeToHeadTransform(vr::Eye_Right);
    { std::lock_guard<std::mutex> lock(source.mutex);
      check(source.worldNotes == 2 && source.worldNotedPercent == 100, "going back to 100 percent wrote one more note"); }
    // No live geometry: the cached optics answer, at the scale they were built with.
    serve(s80);
    source.state.geometryValid = false; source.state.geometry = {};
    const auto cached = system->GetEyeToHeadTransform(vr::Eye_Left);
    { std::lock_guard<std::mutex> lock(source.mutex);
      check(cached.m[0][3] == s80.eyeToHead[0].m[0][3] && cached.m[2][3] == 0.0f - s80.eyeToHead[0].m[2][3] && source.worldNotes == 3 && source.worldNotedPercent == 80 &&
                source.worldNotedSequence == s80.native.sequence, "no live geometry: the cached optics answer at 80 percent, and say so once"); }
    source.state = keptWorld;
  }
  headPoseTests(system, source, concrete);
  for(const auto e:{vr::Eye_Left,vr::Eye_Right})for (const auto api : {vr::API_DirectX, vr::API_OpenGL}) for (const auto planes : {std::pair<float,float>{.025f,50000.f}, {.1f,1000.f}, {1.f,50000.f}}) {
    const auto m = system->GetProjectionMatrix(e, planes.first, planes.second, api);
    const float left=std::tan(-.6f), right=std::tan(.7f), top=std::tan(-.4f), bottom=std::tan(.5f);
    const float a=2.f/(right-left), bb=2.f/(bottom-top), c=(right+left)/(right-left), d=(bottom+top)/(bottom-top);
    const float q=planes.second/(planes.second-planes.first), z=-(planes.second*planes.first)/(planes.second-planes.first);
    check(near(m.m[0][0],a)&&near(m.m[1][1],bb)&&near(m.m[0][2],c)&&near(m.m[1][2],d)&&near(m.m[2][2],api==vr::API_DirectX?-q:-(planes.second+planes.first)/(planes.second-planes.first))&&near(m.m[2][3],api==vr::API_DirectX?z:-2*planes.second*planes.first/(planes.second-planes.first))&&near(m.m[3][2],-1), "independent projection coefficients and planes");
    const double nearDepth=(double(m.m[2][2])*(-planes.first)+m.m[2][3])/planes.first;
    const double farDepth=(double(m.m[2][2])*(-planes.second)+m.m[2][3])/planes.second;
    check(std::fabs(nearDepth-(api==vr::API_DirectX?0:-1))<1e-5&&std::fabs(farDepth-1)<1e-5,"historical depth endpoints");
  }

  check(system->GetD3D9AdapterIndex() == -1, "D3D9 adapter is unavailable");
  int32_t output = 99; system->GetDXGIOutputInfo(&output); check(output == 7, "DXGI output info forwarded");
  check(system->IsDisplayOnDesktop() == false, "display mode default is direct");
  check(!system->SetDisplayVisibility(true), "display visibility is unavailable");

  vr::TrackedDevicePose_t poses[3]; std::memset(poses, 0xa5, sizeof(poses));
  system->GetDeviceToAbsoluteTrackingPose(vr::TrackingUniverseStanding, .125f, poses, 3);
  { std::lock_guard<std::mutex> lock(source.mutex); check(source.locatedGeneration == 17 && source.locatedPrediction == .125f && source.locatedOrigin == vr::TrackingUniverseStanding, "absolute pose forwards generation origin prediction"); }
  check(poses[0].bPoseIsValid && near(poses[0].mDeviceToAbsoluteTracking.m[0][0],17), "absolute pose output initialized");
  vr::TrackedDevicePose_t poseCanary; std::memset(&poseCanary, 0xa5, sizeof(poseCanary));
  system->GetDeviceToAbsoluteTrackingPose(vr::TrackingUniverseStanding, .125f, &poseCanary, 0);
  check(allZero(&poseCanary, sizeof(poseCanary)) == false && reinterpret_cast<unsigned char*>(&poseCanary)[0] == 0xa5, "zero pose capacity leaves output untouched");
  check(std::memcmp(&source.original, &source.state.geometry, sizeof(source.original)) == 0, "pose call never mutates source snapshot");
  system->ResetSeatedZeroPose(); check(source.resetCalls == 1, "reset seated forwards generation");
  check(system->GetSeatedZeroPoseToStandingAbsoluteTrackingPose().m[0][0] == 11, "seated transform returned");
  check(system->GetRawZeroPoseToStandingAbsoluteTrackingPose().m[1][1] == 22, "raw transform returned");

  vr::TrackedDeviceIndex_t indices[2] = {99,99};
  check(system->GetSortedTrackedDeviceIndicesOfClass(vr::TrackedDeviceClass_HMD,indices,1,vr::k_unTrackedDeviceIndex_Hmd) >= 1 && indices[0] == 0, "sorted device output honors capacity");
  check(system->GetTrackedDeviceActivityLevel(0) == vr::k_EDeviceActivityLevel_Unknown, "activity level unavailable is deterministic");
  check(system->GetTrackedDeviceIndexForControllerRole(vr::TrackedControllerRole_LeftHand) == vr::k_unTrackedDeviceIndexInvalid, "controller role unavailable");
  check(system->GetControllerRoleForTrackedDeviceIndex(99) == vr::TrackedControllerRole_Invalid, "invalid controller role default");
  check(system->GetTrackedDeviceClass(0) == vr::TrackedDeviceClass_HMD && system->IsTrackedDeviceConnected(0), "connected HMD identity");
  check(system->GetTrackedDeviceClass(99) == vr::TrackedDeviceClass_Invalid && !system->IsTrackedDeviceConnected(99), "invalid device identity");

  vr::ETrackedPropertyError error = vr::TrackedProp_Success;
  check(!system->GetBoolTrackedDeviceProperty(99, vr::Prop_DeviceIsWireless_Bool, &error) && error != vr::TrackedProp_Success, "invalid bool property reports error");
  error = vr::TrackedProp_Success; check(system->GetFloatTrackedDeviceProperty(0, vr::Prop_DisplayFrequency_Float, &error) == 90.f && error == vr::TrackedProp_Success, "float metadata property");
  error = vr::TrackedProp_Success; check(system->GetFloatTrackedDeviceProperty(0, vr::Prop_DeviceIsWireless_Bool, &error) == 0.f && error != vr::TrackedProp_Success, "wrong property type reports error");
  error = vr::TrackedProp_Success; check(system->GetInt32TrackedDeviceProperty(0, vr::Prop_DeviceClass_Int32, &error) == vr::TrackedDeviceClass_HMD && error == vr::TrackedProp_Success, "int metadata property");
  error = vr::TrackedProp_Success; check(system->GetUint64TrackedDeviceProperty(0, vr::Prop_CurrentUniverseId_Uint64, &error) == 0 && error != vr::TrackedProp_Success, "unavailable uint64 property reports error");
  const auto matrix = system->GetMatrix34TrackedDeviceProperty(99, vr::Prop_StatusDisplayTransform_Matrix34, &error); check(near(matrix.m[0][0],1) && near(matrix.m[1][1],1) && near(matrix.m[2][2],1), "invalid matrix property is identity");
  char text[64]; std::memset(text, 0xcc, sizeof(text)); uint32_t need = system->GetStringTrackedDeviceProperty(0, vr::Prop_TrackingSystemName_String, text, sizeof(text), &error);
  check(need == std::strlen("fake-openxr-runtime") + 1 && std::strcmp(text,"fake-openxr-runtime") == 0, "string property exact buffer");
  for (uint32_t capacity : {0u,1u,need-1,need+8}) { std::memset(text,0xcc,sizeof(text)); const uint32_t n=system->GetStringTrackedDeviceProperty(0,vr::Prop_TrackingSystemName_String,text,capacity,&error); check(n==need,"string property reports required size for every capacity"); if(capacity) check(text[0]==0 || capacity>=need,"small string buffer is terminated"); }
  check(std::strcmp(system->GetPropErrorNameFromEnum(vr::TrackedProp_InvalidDevice),"TrackedProp_InvalidDevice")==0,"property error name");

  vr::VREvent_t event; vr::TrackedDevicePose_t eventPose; check(system->PollNextEvent(&event, sizeof(event)), "event poll returns fake event");
  const unsigned pollsBeforeShort = source.pollCalls; std::memset(&event,0xcc,sizeof(event)); check(!system->PollNextEvent(&event, sizeof(event)-1) && source.pollCalls == pollsBeforeShort, "undersized event buffer does not consume an event");
  check(system->PollNextEventWithPose(vr::TrackingUniverseSeated,&event,sizeof(event),&eventPose), "event pose poll forwards origin");
  check(std::strcmp(system->GetEventTypeNameFromEnum(vr::VREvent_TrackedDeviceActivated),"VREvent_TrackedDeviceActivated")==0,"event name");
  check(system->GetHiddenAreaMesh(vr::Eye_Left).pVertexData == nullptr, "hidden mesh unavailable");
  vr::VRControllerState_t controller{}; vr::TrackedDevicePose_t controllerPose{};
  check(!system->GetControllerState(0,&controller) && !system->GetControllerStateWithPose(vr::TrackingUniverseStanding,0,&controller,&controllerPose), "default controller states unavailable");
  system->TriggerHapticPulse(0,0,1);
  check(std::strcmp(system->GetButtonIdNameFromEnum(vr::k_EButton_System),"k_EButton_System")==0,"button name");
  check(std::strcmp(system->GetControllerAxisTypeNameFromEnum(vr::k_eControllerAxis_TrackPad),"k_eControllerAxis_TrackPad")==0,"axis name");
  check(!system->CaptureInputFocus() && !system->IsInputFocusCapturedByAnotherProcess(), "input focus unavailable"); system->ReleaseInputFocus();
  char response[16]; std::memset(response,0xcc,sizeof(response)); const uint32_t responseNeed=system->DriverDebugRequest(0,"ping",response,sizeof(response)); check(responseNeed==0 && response[0]==0,"debug response bounded");
  check(system->PerformFirmwareUpdate(0)==vr::VRFirmwareError_Fail,"firmware update unavailable result"); system->AcknowledgeQuit_Exiting(); system->AcknowledgeQuit_UserPrompt();

  boundaryTests(system,source);
  {
    FakeSource diagnostic;edvr::openxr::OpenVRSystem observed(diagnostic);
    for(unsigned i=0;i<100;++i) {
      observed.GetProjectionMatrix(vr::Eye_Left,0.025f,50000,vr::API_DirectX);
      observed.GetProjectionMatrix(vr::Eye_Left,1,1000,vr::API_DirectX);
    }
    check(diagnostic.queryAccepted==2,"alternating normal clip planes do not consume diagnostic capacity");
    const auto ordinary=observed.GetProjectionMatrix(vr::Eye_Left,2,100000,vr::API_DirectX);
    const auto reversed=observed.GetProjectionMatrix(vr::Eye_Left,50000,1,vr::API_DirectX);
    const auto infinite=observed.GetProjectionMatrix(vr::Eye_Left,1,std::numeric_limits<float>::infinity(),vr::API_DirectX);
    const auto zeros=observed.GetProjectionMatrix(vr::Eye_Left,0,0,vr::API_DirectX);
    check(diagnostic.queryAccepted==3 && diagnostic.queryRejected==3,"later new planes and rejection reasons remain observable");
    check(ordinary.m[0][0]>0 && allZero(&reversed,sizeof(reversed)) && allZero(&infinite,sizeof(infinite)) && allZero(&zeros,sizeof(zeros)),
          "projection diagnostics preserve normal and rejected return matrices");
    for(unsigned i=3;i<80;++i)observed.GetProjectionMatrix(vr::Eye_Left,float(i),100000,vr::API_DirectX);
    check(diagnostic.queryAccepted==32 && diagnostic.queryRejected==3,"successful log cap cannot exhaust rejection capacity");
    { std::lock_guard<std::mutex> lock(diagnostic.mutex);diagnostic.state.geometryValid=false; }
    const auto cached=observed.GetProjectionMatrix(vr::Eye_Left,1,1000,vr::API_DirectX);
    check(cached.m[0][0]>0,"diagnostic capacity does not alter cached-optics availability");
  }

  {
    FakeSource diagnostic;edvr::openxr::OpenVRSystem observed(diagnostic);
    vr::ETrackedPropertyError e=vr::TrackedProp_Success;
    diagnostic.state.displayFrequencyAvailable=false;
    check(observed.GetFloatTrackedDeviceProperty(0,vr::Prop_DisplayFrequency_Float,&e)==0 &&
          e==vr::TrackedProp_ValueNotProvidedByDevice && diagnostic.frequencyNotes==1 &&
          diagnostic.frequencyError==e && diagnostic.frequencyValue==0,"frequency observer receives actual unavailable result");
    diagnostic.state.displayFrequencyAvailable=true;diagnostic.state.displayFrequency=90;
    check(observed.GetFloatTrackedDeviceProperty(0,vr::Prop_DisplayFrequency_Float,nullptr)==90 &&
          diagnostic.frequencyNotes==2 && diagnostic.frequencyError==vr::TrackedProp_Success &&
          diagnostic.frequencyValue==90,"frequency observer preserves available result and null error pointer");
    check(observed.GetFloatTrackedDeviceProperty(1,vr::Prop_DisplayFrequency_Float,&e)==0 &&
          e==vr::TrackedProp_InvalidDevice && diagnostic.frequencyNotes==3 && diagnostic.frequencyError==e,
          "invalid-device frequency result is preserved");
    observed.GetFloatTrackedDeviceProperty(0,vr::Prop_UserIpdMeters_Float,&e);
    check(diagnostic.frequencyNotes==3,"other float properties do not consume frequency samples");
  }
  FakeSource freshSource;edvr::openxr::OpenVRSystem fresh(freshSource);vr::IVRSystem* concurrent=openxrAbiCaller(&fresh);
  auto unsupportedCalls = [&] {for(unsigned n=0;n<1000;++n){concurrent->ComputeDistortion(vr::Eye_Left,0,0);concurrent->GetTimeSinceLastVsync(nullptr,nullptr);}};
  std::thread a(unsupportedCalls), c(unsupportedCalls);a.join();c.join();check(freshSource.unsupportedCalls==2,"exactly one diagnostic per unsupported slot across threads");
  source.retire(); check(!system->IsTrackedDeviceConnected(0) && system->GetTrackedDeviceClass(0)==vr::TrackedDeviceClass_Invalid, "retirement removes connected outputs");
  system->GetRecommendedRenderTargetSize(&width,&height);check(width==0&&height==0,"retirement removes geometry");
  system->GetDeviceToAbsoluteTrackingPose(vr::TrackingUniverseSeated,0,poses,3);check(!poses[0].bDeviceIsConnected&&!poses[0].bPoseIsValid,"retirement removes pose availability");
  std::printf("openxr_system_test: %u checks, %u failures\n", checks, failures); return failures ? 1 : 0;
}
}

int main(int argc, char** argv) {
  if (argc != 2) return 2;
  if (!std::strcmp(argv[1], "--dry-run")) { std::puts("Would test concrete IVRSystem ABI; no runtime/device/files."); return 0; }
  return !std::strcmp(argv[1], "--self-test") ? selfTest() : 2;
}
