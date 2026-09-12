#pragma once
#include "compositor_source.h"
#include <mutex>
#include <limits>

namespace edvr::openxr {
// Cached reads use this short mutex only. The source's runtime operation lease
// serializes writers, origin replacement and retirement around XR operations.
class CompositorPublication {
 public:
  uint64_t begin(vr::ETrackingUniverseOrigin origin=vr::TrackingUniverseSeated) {
    std::lock_guard<std::mutex> lock(mutex_);
    if(state_.generation||generation_==(std::numeric_limits<uint64_t>::max)()||!validOrigin(origin))return 0;
    state_={};state_.generation=++generation_;state_.originGeneration=1;
    state_.origin=origin;state_.connected=true;return generation_;
  }
  bool publish(const CompositorRead& candidate) {
    std::lock_guard<std::mutex> lock(mutex_);
    if(!live(candidate.generation)||candidate.originGeneration!=state_.originGeneration||candidate.origin!=state_.origin||
       !candidate.sequence||candidate.sequence<=state_.sequence||!candidate.posesAvailable||!candidate.connected)return false;
    // Focus state comes from lifecycle publication, not a stale frame copy.
    const bool known=state_.canRenderKnown,canRender=state_.canRender;
    state_=candidate;state_.canRenderKnown=known;state_.canRender=canRender;return true;
  }
  // Call only after the owner actually changed the runtime origin. Unknown or
  // unsupported spaces must never become the published selected origin.
  bool changeOrigin(uint64_t generation,vr::ETrackingUniverseOrigin origin) {
    std::lock_guard<std::mutex> lock(mutex_);
    if(!live(generation)||!validOrigin(origin))return false;
    if(origin==state_.origin)return true;
    if(state_.originGeneration==(std::numeric_limits<uint64_t>::max)())return false;
    ++state_.originGeneration;state_.origin=origin;clearPoses();return true;
  }
  void invalidate(uint64_t generation) {
    std::lock_guard<std::mutex> lock(mutex_);if(live(generation))clearPoses();
  }
  void focus(uint64_t generation,bool known,bool canRender) {
    std::lock_guard<std::mutex> lock(mutex_);
    if(live(generation)){state_.canRenderKnown=known;state_.canRender=known&&canRender;}
  }
  void retire(uint64_t generation) {
    std::lock_guard<std::mutex> lock(mutex_);if(live(generation))state_={};
  }
  CompositorRead read() const {std::lock_guard<std::mutex> lock(mutex_);return state_;}
 private:
  static bool validOrigin(vr::ETrackingUniverseOrigin origin) {
    return origin==vr::TrackingUniverseSeated||origin==vr::TrackingUniverseStanding||origin==vr::TrackingUniverseRawAndUncalibrated;
  }
  bool live(uint64_t generation) const {return generation&&generation==state_.generation;}
  void clearPoses() {
    state_.posesAvailable=false;state_.renderTime=state_.gameTime=0;state_.renderPose=state_.gamePose={};
  }
  mutable std::mutex mutex_;
  CompositorRead state_{};
  uint64_t generation_=0;
};
} // namespace edvr::openxr
