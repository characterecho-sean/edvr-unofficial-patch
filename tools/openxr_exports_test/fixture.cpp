// Deliberately headset-free DLL. Tests the five imported PE exports with real
// owned interface objects, without loading any VR runtime or graphics device.
#include "../../src/openxr/runtime_exports.h"
#include "../../src/openxr/openvr_system.h"
#include "../../src/openxr/openvr_compositor.h"
#include "../../src/openxr/openvr_auxiliary.h"
#include "../../src/openxr/space_pose.h"
using namespace edvr::openxr;
namespace {
class Fixture final:public RuntimeBackend,public SystemSource,public CompositorSource,public AuxiliarySource {
 public:
  std::atomic<uint32_t> generation{0};
  OpenVRSystem system{*this};OpenVRCompositor compositor{this};
  OpenVRChaperone chaperone{*this};OpenVRExtendedDisplay display{*this};
  vr::EVRInitError start(uint32_t token,const std::atomic<bool>&,RuntimeInterfaces& out)override {
    generation.store(token);out={&system,&compositor,&chaperone,&display};return vr::VRInitError_None;
  }
  bool stop()noexcept override{generation.store(0);return true;}
  SystemRead read()const override {
    SystemRead out{};out.generation=generation.load();out.connected=out.generation!=0;
    if(out.connected) {
      GeometryInput input{};input.generation=out.generation;input.sequence=1;
      input.headPose.orientation.w=1;input.headFlags=XR_SPACE_LOCATION_ORIENTATION_VALID_BIT|XR_SPACE_LOCATION_POSITION_VALID_BIT;
      input.viewFlags=XR_VIEW_STATE_ORIENTATION_VALID_BIT|XR_VIEW_STATE_POSITION_VALID_BIT;
      for(unsigned eye=0;eye<2;++eye){input.width[eye]=640;input.height[eye]=480;input.views[eye].pose.orientation.w=1;input.views[eye].fov={-.7f,.7f,.7f,-.7f};}
      out.geometryValid=makeGeometrySnapshot(input,out.geometry);
    }
    return out;
  }
  bool locateHead(uint64_t,vr::ETrackingUniverseOrigin,float,vr::TrackedDevicePose_t&)override{return false;}
  bool resetSeated(uint64_t)override{return false;}
  bool pollEvent(uint64_t,vr::ETrackingUniverseOrigin,vr::VREvent_t&,vr::TrackedDevicePose_t&)override{return false;}
  void unsupported(unsigned)noexcept override{}
  CompositorRead compositorRead()const override {
    CompositorRead out{};out.generation=generation.load();out.connected=out.generation!=0;return out;
  }
  vr::EVRCompositorError waitPoses(uint64_t,CompositorRead&)override{return vr::VRCompositorError_InvalidTexture;}
  bool setTrackingSpace(uint64_t,vr::ETrackingUniverseOrigin origin)override{return origin==vr::TrackingUniverseSeated;}
  vr::EVRCompositorError submitEye(uint64_t,vr::EVREye,const vr::Texture_t*,const vr::VRTextureBounds_t*,vr::EVRSubmitFlags)override{return vr::VRCompositorError_InvalidTexture;}
  bool clearSubmitted(uint64_t)override{return false;}
  bool handoff(uint64_t)override{return false;}
  void compositorUnsupported(unsigned)noexcept override{}
  AuxiliaryRead readAuxiliary()const override {
    const auto current=generation.load();return {current,current!=0,{640,640},{480,480}};
  }
  void auxiliaryUnsupported(unsigned,unsigned)noexcept override{}
};
}
extern "C" __declspec(dllexport) bool __cdecl edvrBindRuntimeFixture() {
  // Explicit test entry point runs after LoadLibrary, never in DllMain.
  static Fixture backend;static RuntimeLifecycle owner(backend);return bindRuntimeExports(owner);
}
