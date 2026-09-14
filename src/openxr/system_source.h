#pragma once
#include "geometry_snapshot.h"

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
  bool seatedToStandingValid=false, rawToStandingValid=false;
  vr::HmdMatrix34_t seatedToStanding{}, rawToStanding{};
  // Game-facing per-eye tangent offsets. Cached optics remains unjittered.
  float tangentShift[2][2]{};
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
  virtual void unsupported(unsigned slot) noexcept=0;
};
}
