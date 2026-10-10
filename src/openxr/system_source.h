#pragma once
#include "geometry_snapshot.h"
#include "visibility_mask.h"

namespace edvr::openxr {
// Session-generation optics that remain meaningful when the current tracking
// sample is unavailable. This deliberately contains no world-space head pose:
// projection and eye placement can be reused across a recenter without
// accidentally reusing stale tracking or per-frame tangent jitter. The raw FOV
// is the game-facing value, so any bounded cull widening remains paired with
// the recommendations that produced it.
struct SystemOptics {
  uint64_t generation=0, sequence=0;
  RawFov raw[2]{};
  vr::HmdMatrix34_t eyeToHead[2]{};
};
// One copied read transaction. No borrowed runtime strings, pointers or handles.
// A live HMD may have temporarily invalid tracking. geometry is the current
// game-facing frame record; optics is the stable, unjittered calibration cache.
struct SystemRead {
  uint64_t generation=0;
  bool connected=false, geometryValid=false, focusKnown=false, focused=false;
  bool opticsValid=false;
  // Session-stable view configuration recommendations. These remain available
  // while tracking/pose geometry is temporarily invalid.
  uint32_t recommendedWidth[2]{}, recommendedHeight[2]{};
  GeometrySnapshot geometry{};
  SystemOptics optics{};
  int32_t adapterIndex=-1;
  char runtimeName[XR_MAX_RUNTIME_NAME_SIZE]{};
  char systemName[XR_MAX_SYSTEM_NAME_SIZE]{};
  bool displayFrequencyAvailable=false;
  float displayFrequency=0;
  // True only for the compatibility placeholder, never a measured panel rate.
  bool displayFrequencyEstimated=false;
  std::shared_ptr<const NativeHiddenMasks> hiddenMasks;
  bool hiddenMasksCompatible=true;
  bool seatedToStandingValid=false, rawToStandingValid=false;
  vr::HmdMatrix34_t seatedToStanding{}, rawToStanding{};
  // Game-facing per-eye tangent offsets. Cached optics remains unjittered.
  float tangentShift[2][2]{};
};

// Who called GetDeviceToAbsoluteTrackingPose and which instant it is to be located at. Taken on the CALLER's thread before the hop to
// the owner thread, since the owner is the wrong place to ask "who called". `rva` is the game executable's return RVA (or
// kFrameOutside/kFrameUnknown); `display` is the filter's verdict (head_pose_time.h answeredAtDisplayTime): Elite's own request for
// "now" is located one display period past the latest frame's display time, everyone else at now + prediction.
struct HeadCall {
  uint32_t thread=0,rva=0;
  bool display=false;
};

// The source outlives the concrete IVRSystem object and protects its resources
// against concurrent calls/shutdown. read() is a short copy with no XR wait.
// Operations use the generation from that read; retired generations must fail.
// No callback may advance the frame loop or interpret cached display time as now.
class SystemSource {
 public:
  virtual ~SystemSource()=default;
  virtual SystemRead read() const =0;
  virtual bool locateHead(uint64_t generation,vr::ETrackingUniverseOrigin origin,
                          float prediction,vr::TrackedDevicePose_t& out)=0;
  // The same, for a caller that is known: the instrument records `call`, and a call with `display` set is located one display period past the
  // latest frame's display time instead of now + prediction. The default is the plain call, so a source with no use for either keeps working unchanged.
  virtual bool locateHeadFor(uint64_t generation,vr::ETrackingUniverseOrigin origin,
                             float prediction,const HeadCall& call,vr::TrackedDevicePose_t& out) {
    (void)call;return locateHead(generation,origin,prediction,out);
  }
  // A pose call that never reached locateHead (no live session, a bad origin, a prediction that is not a number). Counted by the
  // pose-gap instrument; may be called from any thread.
  virtual void noteHeadCallFailed(const HeadCall&,float) noexcept {}
  virtual bool resetSeated(uint64_t generation)=0;
  // Return an event-time pose in the requested origin, or initialized invalid
  // pose if unavailable. No synthesized focus/quit events are implied here.
  virtual bool pollEvent(uint64_t generation,vr::ETrackingUniverseOrigin origin,
                         vr::VREvent_t& event,vr::TrackedDevicePose_t& pose)=0;
  // Successful projection queries may be observed by a temporal consumer.
  virtual void noteProjection(uint64_t sequence,uint32_t eye,float nearZ,float farZ) noexcept {
    (void)sequence;(void)eye;(void)nearZ;(void)farZ;
  }
  // Bounded diagnostic hook for game-facing geometry queries. Implementations
  // must not dispatch XR/graphics work or wait for a frame.
  virtual void noteGeometryQuery(unsigned slot,const SystemRead& snapshot) noexcept {
    (void)slot;(void)snapshot;
  }
  virtual void noteProjectionQuery(const SystemRead&,unsigned,float,float,
      vr::EGraphicsAPIConvention,bool,const vr::HmdMatrix44_t&,const void*) noexcept {}
  virtual void noteFrequencyQuery(const SystemRead&,vr::TrackedDeviceIndex_t,
      vr::ETrackedPropertyError,float,unsigned) noexcept {}
  virtual void notePropertyQuery(unsigned,vr::TrackedDeviceIndex_t,
      vr::ETrackedDeviceProperty,vr::ETrackedPropertyError) noexcept {}
  virtual void noteHiddenMesh(unsigned,uint64_t,uint32_t,const char*,uint32_t) noexcept {}
  virtual void unsupported(unsigned slot) noexcept=0;
};
}
