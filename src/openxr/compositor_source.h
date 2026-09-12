#pragma once
#include "../openvr/compat/openvr_v0_9_20.h"
#include <cstdint>

namespace edvr::openxr {
// Short copied transaction. No XR handles or caller buffers escape a source
// operation. Render and gameplay poses are independent predictions, not eyes.
struct CompositorRead {
  uint64_t generation=0, sequence=0, originGeneration=0;
  int64_t renderTime=0, gameTime=0;
  vr::ETrackingUniverseOrigin origin=vr::TrackingUniverseSeated;
  bool connected=false, posesAvailable=false;
  vr::TrackedDevicePose_t renderPose{}, gamePose{};
  bool canRenderKnown=false, canRender=false;
};

// The source outlives the ABI object and owns serialization, lifetime leases,
// origin changes and cache publication. wait returns the same snapshot it
// published; cached reads never advance or locate a frame. A failed wait must
// invalidate its cache if it had begun an operation; a busy/stale rejected
// caller must not invalidate a different operation's published frame.
class CompositorSource {
 public:
  virtual ~CompositorSource()=default;
  virtual CompositorRead compositorRead() const=0;
  virtual vr::EVRCompositorError waitPoses(uint64_t generation,CompositorRead& out)=0;
  virtual bool setTrackingSpace(uint64_t generation,vr::ETrackingUniverseOrigin)=0;
  virtual vr::EVRCompositorError submitEye(uint64_t generation,vr::EVREye,const vr::Texture_t*,
      const vr::VRTextureBounds_t*,vr::EVRSubmitFlags)=0;
  // The source reports false if full historical semantics are unavailable.
  virtual bool clearSubmitted(uint64_t generation)=0;
  virtual bool handoff(uint64_t generation)=0;
  virtual void compositorUnsupported(unsigned slot) noexcept=0;
};
} // namespace edvr::openxr
