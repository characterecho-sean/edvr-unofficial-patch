#pragma once
#include "geometry_snapshot.h"

namespace edvr::openxr {
// One copied read transaction. No borrowed runtime strings, pointers or handles.
// A live HMD may have temporarily invalid tracking. Geometry is native, before
// the future game-facing jitter/crop/pose policies are applied.
struct SystemRead {
  uint64_t generation=0;
  bool connected=false, geometryValid=false, focusKnown=false, focused=false;
  GeometrySnapshot geometry{};
  int32_t adapterIndex=-1;
  char runtimeName[XR_MAX_RUNTIME_NAME_SIZE]{};
  char systemName[XR_MAX_SYSTEM_NAME_SIZE]{};
  bool displayFrequencyAvailable=false;
  float displayFrequency=0;
  bool seatedToStandingValid=false, rawToStandingValid=false;
  vr::HmdMatrix34_t seatedToStanding{}, rawToStanding{};
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
  virtual void unsupported(unsigned slot) noexcept=0;
};
}
