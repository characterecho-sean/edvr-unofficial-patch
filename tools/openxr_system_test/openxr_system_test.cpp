#include "../../src/openxr/openvr_system.h"
#include "../../src/openxr/system_source.h"
#include "../../src/openxr/geometry_snapshot.h"
#include "../../src/openxr/system_publication.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <mutex>
#include <string>
#include <utility>
#include <thread>
#include <vector>
extern "C" vr::IVRSystem* openxrAbiCaller(vr::IVRSystem*);
extern "C" int openxrAbiCallRaw(vr::IVRSystem*, vr::EVREye, float*);   // out[0..3] = left, right, top, bottom; out[4] spare
extern "C" int openxrAbiOuterRaw(vr::IVRSystem*, vr::EVREye, float*);   // calls openxrAbiCallRaw
extern "C" int openxrAbiCallMatrix(vr::IVRSystem*, vr::EVREye, vr::HmdMatrix44_t*);
extern "C" int openxrAbiCallEyeToHead(vr::IVRSystem*, vr::EVREye, vr::HmdMatrix34_t*);


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
  void noteFrequencyQuery(const SystemRead&,vr::TrackedDeviceIndex_t,vr::ETrackedPropertyError error,
      float value,unsigned) noexcept override {
    std::lock_guard<std::mutex> lock(mutex);++frequencyNotes;frequencyError=error;frequencyValue=value;
  }
  void noteProjectionQuery(const SystemRead&,unsigned,float,float,vr::EGraphicsAPIConvention,
      bool accepted,const vr::HmdMatrix44_t&,const void*) noexcept override {
    std::lock_guard<std::mutex> lock(mutex);if(accepted)++queryAccepted;else ++queryRejected;
  }
  std::vector<std::string> callerLines;
  void noteCallerLine(const char* line) noexcept override {
    std::lock_guard<std::mutex> lock(mutex);callerLines.emplace_back(line);
  }
  unsigned eyeNotes=0;unsigned notedEyeIndex=99;vr::HmdMatrix34_t notedGiven{},notedLocated{};
  void noteEyeToHead(const SystemRead&,unsigned eye,const vr::HmdMatrix34_t& given,const vr::HmdMatrix34_t& located) noexcept override {
    std::lock_guard<std::mutex> lock(mutex);++eyeNotes;notedEyeIndex=eye;notedGiven=given;notedLocated=located;
  }
  std::atomic<unsigned> unsupportedCalls{0};

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
    struct Served{vr::HiddenAreaMesh_t eye[2];const char* reason;uint32_t dropped;};
    const auto serve=[&](const NativeHiddenMask& l,const NativeHiddenMask& r)->Served {
      FakeSource paired;OpenVRSystem pairedSystem(paired);paired.state=source.state;
      auto both=std::make_shared<NativeHiddenMasks>(*masks);both->revision=1;both->eyes[0]=l;both->eyes[1]=r;
      paired.state.hiddenMasks=both;
      Served out{};out.eye[0]=pairedSystem.GetHiddenAreaMesh(vr::Eye_Left);
      out.eye[1]=pairedSystem.GetHiddenAreaMesh(vr::Eye_Right);
      out.reason=paired.lastMeshReason;out.dropped=paired.lastMeshDropped;return out;
    };
    auto served=serve(triangle,triangle);
    check(served.eye[0].unTriangleCount==1&&served.eye[1].unTriangleCount==1&&!std::strcmp(served.reason,"runtime"),
      "two populated eyes are both served");
    served=serve(triangle,none);
    check(!served.eye[0].pVertexData&&!served.eye[0].unTriangleCount&&!served.eye[1].pVertexData&&!served.eye[1].unTriangleCount,
      "a populated left with an empty right serves neither eye (the zero-byte vertex buffer)");
    check(!std::strcmp(served.reason,"empty"),"the empty eye says empty");
    served=serve(none,triangle);
    check(!served.eye[0].pVertexData&&!served.eye[1].pVertexData&&!std::strcmp(served.reason,"unpaired"),
      "an empty left with a populated right serves neither eye, and the populated one says unpaired");
    served=serve(triangle,broken);
    check(!served.eye[0].pVertexData&&!served.eye[1].pVertexData&&!std::strcmp(served.reason,"invalid_mesh"),
      "a malformed right eye takes the left eye's mesh with it");
    served=serve(triangle,sliver);
    check(served.eye[0].unTriangleCount==1&&served.eye[1].unTriangleCount==1&&served.dropped==1&&!std::strcmp(served.reason,"runtime"),
      "a sliver triangle is dropped alone, the eye's other triangle and the other eye stay");
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
        near(system.GetEyeToHeadTransform(vr::Eye_Left).m[2][3],-.06f),
        "origin invalidation preserves cached raw FOV and eye placement");
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

// The terrain-culling arc's census and selective-lie probe, through the historical virtual ABI (docs\terrain-culling.md): frame 1 is the immediate caller in the
// game's image and not an adapter in this DLL, the probe answers exactly the callers it is asked to and nobody else, and a tracking shift rides along.
__declspec(noinline) void callerCensusTests(vr::IVRSystem* system, FakeSource& source, edvr::openxr::OpenVRSystem& concrete) {
  using edvr::openxr::CullProbe;
  using edvr::openxr::ExeModule;
  using edvr::openxr::ProjectionCallers;
  namespace rva = edvr::openxr::cull_rva;
  const ExeModule real = concrete.callers().module();
  check(real.base != 0 && real.size != 0, "the census knows the executable it reports RVAs against");
  float truth[5]{};
  // ---- frame 1 is the caller, directly ----
  const unsigned linesBefore = [&] { std::lock_guard<std::mutex> lock(source.mutex); return unsigned(source.callerLines.size()); }();
  openxrAbiOuterRaw(system, vr::Eye_Left, truth);
  ProjectionCallers::Entry entries[ProjectionCallers::kCapacity];
  unsigned n = concrete.callers().snapshot(entries, ProjectionCallers::kCapacity);
  const uintptr_t callerFunction = reinterpret_cast<uintptr_t>(&openxrAbiCallRaw);
  const ProjectionCallers::Entry* direct = nullptr;
  for (unsigned i = 0; i < n; ++i)
    if (entries[i].method == ProjectionCallers::Raw && entries[i].eye == 0 && entries[i].rva1 < edvr::openxr::kFrameUnknown &&
        real.base + entries[i].rva1 > callerFunction && real.base + entries[i].rva1 - callerFunction < 0x100) direct = &entries[i];
  check(direct != nullptr, "the vtable call lands in OpenVRSystem itself: frame 1 is a return address a few bytes into the game-side caller, not an adapter in between");
  if (!direct) return;
  const uintptr_t outer = reinterpret_cast<uintptr_t>(&openxrAbiOuterRaw), me = reinterpret_cast<uintptr_t>(&callerCensusTests);
  check(direct->rva2 < edvr::openxr::kFrameUnknown && real.base + direct->rva2 > outer && real.base + direct->rva2 - outer < 0x100,
        "...and the first-sight stack capture gives frame 2: the return address in the function that called that caller");
  {
    // Frame 3 is checked on the line the census logged: the return address in this test, one function further up.
    std::lock_guard<std::mutex> lock(source.mutex);
    const std::string line = source.callerLines.size() > linesBefore ? source.callerLines[linesBefore] : std::string();
    const size_t last = line.rfind("<- exe+0x");
    const unsigned long rva3 = last == std::string::npos ? 0ul : std::strtoul(line.c_str() + last + 9, nullptr, 16);
    const bool inOuter = real.base + rva3 > outer && real.base + rva3 - outer < 0x100;
    check(rva3 != 0 && rva3 != direct->rva2 && !inOuter && real.base + rva3 > me && real.base + rva3 - me < 0x10000, "...and frame 3: the return address in the test, one function further up");
  }
  {
    std::lock_guard<std::mutex> lock(source.mutex);
    const std::string* line = source.callerLines.size() > linesBefore ? &source.callerLines[linesBefore] : nullptr;
    check(line && line->rfind("projection callers: GetProjectionRaw exe+0x", 0) == 0 && line->find(" <- exe+0x") != std::string::npos && line->find("outside") == std::string::npos &&
          line->find(" eye 0 tid ") != std::string::npos, "...and one first-sight line says so");
  }
  vr::HmdMatrix44_t matrix{};
  vr::HmdMatrix34_t head{};
  openxrAbiCallMatrix(system, vr::Eye_Left, &matrix);
  openxrAbiCallEyeToHead(system, vr::Eye_Right, &head);
  n = concrete.callers().snapshot(entries, ProjectionCallers::kCapacity);
  bool matrixSeen = false, eyeSeen = false;
  for (unsigned i = 0; i < n; ++i) {
    matrixSeen = matrixSeen || (entries[i].method == ProjectionCallers::Matrix && entries[i].eye == 0 && real.base + entries[i].rva1 - reinterpret_cast<uintptr_t>(&openxrAbiCallMatrix) < 0x100);
    eyeSeen = eyeSeen || (entries[i].method == ProjectionCallers::EyeToHead && entries[i].eye == 1 && real.base + entries[i].rva1 - reinterpret_cast<uintptr_t>(&openxrAbiCallEyeToHead) < 0x100);
  }
  check(matrixSeen && eyeSeen, "GetProjectionMatrix and GetEyeToHeadTransform are recorded with their own callers and eyes");
  // A call made before there is any geometry to answer with is a caller all the same.
  {
    const SystemRead keptState = source.state;
    const uintptr_t eyeCaller = reinterpret_cast<uintptr_t>(&openxrAbiCallEyeToHead);
    const auto countFrom = [&] {
      ProjectionCallers::Entry e[ProjectionCallers::kCapacity];
      const unsigned m = concrete.callers().snapshot(e, ProjectionCallers::kCapacity);
      unsigned total = 0;
      for (unsigned i = 0; i < m; ++i)
        if (e[i].method == ProjectionCallers::EyeToHead && e[i].eye == 1 && e[i].rva1 < edvr::openxr::kFrameUnknown && real.base + e[i].rva1 > eyeCaller && real.base + e[i].rva1 - eyeCaller < 0x100)
          total += unsigned(e[i].count);
      return total;
    };
    const unsigned countBefore = countFrom();
    source.state.opticsValid = false; source.state.geometryValid = false;
    vr::HmdMatrix34_t none{};
    openxrAbiCallEyeToHead(system, vr::Eye_Right, &none);
    check(allZero(&none, sizeof(none)) && countFrom() == countBefore + 1, "a GetEyeToHeadTransform call with no geometry yet answers zeros and is still counted");
    source.state = keptState;
  }
  // ---- the probe ----
  source.state.tangentShift[0][0] = 0.01f; source.state.tangentShift[0][1] = -0.02f;   // a jitter shift in the answer the probe starts from
  source.state.cullProbe = 0;
  openxrAbiCallMatrix(system, vr::Eye_Left, &matrix);
  openxrAbiCallEyeToHead(system, vr::Eye_Right, &head);
  float honest[5]{}, honestRight[5]{};
  openxrAbiCallRaw(system, vr::Eye_Left, honest);
  openxrAbiCallRaw(system, vr::Eye_Right, honestRight);
  const float h = std::fmax(std::fabs(honest[0]), std::fabs(honest[1])), v = std::fmax(std::fabs(honest[2]), std::fabs(honest[3]));
  const float wide[4] = {-h, h, -v, v};
  const auto sameAs = [](const float* got, const float* want) { return std::memcmp(got, want, 4 * sizeof(float)) == 0; };
  const auto ask = [&](uint32_t group, float* out) { source.state.cullProbe = group; std::memset(out, 0, 5 * sizeof(float)); openxrAbiCallRaw(system, vr::Eye_Left, out); };
  check(near(honest[0], -.6841368f + 0.01f) && near(honest[1], .8422884f + 0.01f) && h > std::fabs(honest[0]) && v > 0.0f,
        "(the starting answer carries the jitter shift, and is asymmetric, so the superset differs from it)");
  // Pretend the game image starts so that this call site is one of the named ones, and ask every group about it.
  const uintptr_t callSite = real.base + direct->rva1;
  struct Site { uint32_t rva; bool camera, ui, sky, sizes, other; };
  const Site sites[] = {
    {rva::kSkyFov, false, false, true, false, false},
    {rva::kSizes[0], false, false, false, true, false}, {rva::kSizes[3], false, false, false, true, false},
    {rva::kEyeFov, false, false, false, false, true},   // frame 2 here is neither the camera setter nor the ui scale: other
    {0x5000, false, false, false, false, true},
  };
  for (const Site& site : sites) {
    concrete.callers().useModule(ExeModule{callSite - site.rva, 0x40000000, edvr::openxr::kBuild332841Stamp, edvr::openxr::kBuild332841ImageSize});
    float got[5]{};
    ask(1, got);
    check(sameAs(got, wide), "all: the superset of the answer it would have had, the shift included");
    ask(0, got);
    check(sameAs(got, honest), "off: exactly the honest answer");
    ask(2, got); const bool camera = sameAs(got, wide);
    ask(3, got); const bool ui = sameAs(got, wide);
    ask(4, got); const bool sky = sameAs(got, wide);
    ask(5, got); const bool sizes = sameAs(got, wide);
    ask(6, got); const bool other = sameAs(got, wide);
    check(camera == site.camera && ui == site.ui && sky == site.sky && sizes == site.sizes && other == site.other,
          "each group is answered wide for exactly its own callers, and every other caller gets its honest answer, bit for bit");
    ask(0, got);
  }
  // The right eye, and an invalid eye, through the same caller.
  concrete.callers().useModule(ExeModule{callSite - rva::kSkyFov, 0x40000000, edvr::openxr::kBuild332841Stamp, edvr::openxr::kBuild332841ImageSize});
  source.state.cullProbe = 4;
  float rightWide[5]{}, invalid[5]{};
  openxrAbiCallRaw(system, vr::Eye_Right, rightWide);
  openxrAbiCallRaw(system, static_cast<vr::EVREye>(9), invalid);
  const float rh = std::fmax(std::fabs(honestRight[0]), std::fabs(honestRight[1])), rv = std::fmax(std::fabs(honestRight[2]), std::fabs(honestRight[3]));
  const float rightWant[4] = {-rh, rh, -rv, rv};
  check(sameAs(rightWide, rightWant), "the right eye is widened from its own answer");
  check(allZero(invalid, 4 * sizeof(float)), "an invalid eye still zeroes its outputs under the probe (a zero stays +0)");
  // Nothing else changes: not the matrix, not the eye transform.
  vr::HmdMatrix44_t matrixWide{};
  vr::HmdMatrix34_t headWide{};
  source.state.cullProbe = 1;
  openxrAbiCallMatrix(system, vr::Eye_Left, &matrixWide);
  openxrAbiCallEyeToHead(system, vr::Eye_Right, &headWide);
  check(std::memcmp(&matrix, &matrixWide, sizeof(matrix)) == 0 && std::memcmp(&head, &headWide, sizeof(head)) == 0,
        "GetProjectionMatrix and GetEyeToHeadTransform answer the same under any probe, bit for bit");
  source.state.cullProbe = 0;
  source.state.tangentShift[0][0] = source.state.tangentShift[0][1] = 0;
  concrete.callers().useModule(real);
}
int selfTest() {
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
  check(near(eye0.m[2][3], -.06f) && near(eye1.m[2][3], .06f), "both eye transforms returned by value");
  {
    // advanced.canted_eye_fix (docs\canted-projection.md): the answer to the game is S*E*S, S = diag(1,1,-1), and nothing else moves. The located transform the
    // snapshot holds (the native frame tables and the layer read it there) is never touched, and the IPD property is made from the located one.
    const GeometrySnapshot kept = source.state.geometry;
    const SystemRead keptState = source.state;
    unsigned before = source.eyeNotes;
    source.state.cantedEyeFix = false;
    auto off0 = system->GetEyeToHeadTransform(vr::Eye_Left);
    check(std::memcmp(&off0, &kept.eyeToHead[0], sizeof(off0)) == 0 && source.eyeNotes == before + 1 && source.notedEyeIndex == 0 &&
          std::memcmp(&source.notedGiven, &source.notedLocated, sizeof(source.notedGiven)) == 0,
          "canted_eye_fix off: the game is given the located transform, and the note says given == located");
    vr::ETrackedPropertyError ipdError = vr::TrackedProp_Success;
    const float ipdOff = system->GetFloatTrackedDeviceProperty(0, vr::Prop_UserIpdMeters_Float, &ipdError);
    source.state.cantedEyeFix = true;
    auto on0 = system->GetEyeToHeadTransform(vr::Eye_Left); auto on1 = system->GetEyeToHeadTransform(vr::Eye_Right);
    check(near(on0.m[2][3], .06f) && near(on1.m[2][3], -.06f) && near(on0.m[0][3], kept.eyeToHead[0].m[0][3]) && near(on0.m[1][3], kept.eyeToHead[0].m[1][3]),
          "canted_eye_fix on: only tz changes sign for a pure translation (x and y stay)");
    check(source.notedEyeIndex == 1 && std::memcmp(&source.notedGiven, &on1, sizeof(on1)) == 0 && std::memcmp(&source.notedLocated, &kept.eyeToHead[1], sizeof(on1)) == 0,
          "...and the note carries the given and the located transform of that call");
    check(std::memcmp(&source.state.geometry, &kept, sizeof(kept)) == 0 && std::memcmp(&source.state.optics, &keptState.optics, sizeof(keptState.optics)) == 0,
          "...while the snapshot and the cached optics are bit-identical afterwards (nothing reads the game-facing answer back)");
    check(std::fabs(system->GetFloatTrackedDeviceProperty(0, vr::Prop_UserIpdMeters_Float, &ipdError) - ipdOff) < 1e-7f && ipdOff > 0.1f,
          "...and the IPD property is the same on and off (made from the located transform)");
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
    source.state = keptState; source.state.cantedEyeFix = false;
  }
  callerCensusTests(system, source, concrete);
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
