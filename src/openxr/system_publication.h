#pragma once
#include "system_source.h"
#include "game_render_size.h"
#include <cmath>
#include <mutex>

namespace edvr::openxr {
// Read-side state only. The runtime owner serializes publications and all XR
// resource operations; the mutex here never spans an XR call or GPU work.
class SystemPublication {
 public:
  uint64_t begin(SystemRead metadata) {
    std::lock_guard<std::mutex> lock(mutex_);
    if(active_||generation_==(std::numeric_limits<uint64_t>::max)())return 0;
    metadata.generation=++generation_;metadata.geometry={};metadata.geometryValid=false;
    metadata.optics={};metadata.opticsValid=false;
    metadata.hiddenMasks.reset();metadata.hiddenMasksCompatible=true;
    // The game may query this metadata before any valid pose is available.
    // Normalize here as well as on live updates so startup cannot cache an
    // odd size while temporal rendering later uses an even output target.
    for(unsigned eye=0;eye<2;++eye) {
      metadata.recommendedWidth[eye]=gameFacingDimension(metadata.recommendedWidth[eye]);
      metadata.recommendedHeight[eye]=gameFacingDimension(metadata.recommendedHeight[eye]);
    }
    metadata.tangentShift[0][0]=metadata.tangentShift[0][1]=0.0f;
    metadata.tangentShift[1][0]=metadata.tangentShift[1][1]=0.0f;
    state_=metadata;sequence_=0;active_=true;return generation_;
  }
  bool publish(const GeometryInput& input,bool focusKnown,bool focused,
               const float (*tangentShift)[2]=nullptr,bool masksCompatible=true) {
    GeometrySnapshot candidate{};const bool valid=makeGeometrySnapshot(input,candidate);
    std::lock_guard<std::mutex> lock(mutex_);
    if(!active_||input.generation!=generation_||input.sequence<=sequence_)return false;
    sequence_=input.sequence;
    state_.hiddenMasksCompatible=masksCompatible;
    // Optics are session-generation calibration. Publish the last validated
    // unjittered FOV and eye placement before applying the frame's tangent
    // jitter. A bad tracking sample or a bad jitter request must not erase the
    // calibration needed by game-facing projection queries.
    if(valid) {
      state_.optics.generation=candidate.native.generation;
      state_.optics.sequence=candidate.native.sequence;
      for(unsigned eye=0;eye<2;++eye) {
        state_.optics.raw[eye]=candidate.raw[eye];
        state_.optics.eyeToHead[eye]=candidate.eyeToHead[eye];
      }
      state_.opticsValid=true;
    }
    state_.geometryValid=valid;
    state_.focusKnown=focusKnown;state_.focused=focused;
    bool shiftsValid=true; RawFov shifted{};
    if(tangentShift) for(unsigned eye=0;eye<2;++eye)
      shiftsValid &= shiftedRawFov(candidate.raw[eye],tangentShift[eye][0],
                                   tangentShift[eye][1],shifted);
    if(!valid||!shiftsValid) {
      state_.geometryValid=false;state_.geometry={};
      state_.tangentShift[0][0]=state_.tangentShift[0][1]=0.0f;
      state_.tangentShift[1][0]=state_.tangentShift[1][1]=0.0f;
      return false;
    }
    state_.geometry=candidate;
    for(unsigned eye=0;eye<2;++eye)for(unsigned axis=0;axis<2;++axis)
      state_.tangentShift[eye][axis]=tangentShift?tangentShift[eye][axis]:0.0f;
    return true;
  }
  void invalidate(uint64_t generation) {
    std::lock_guard<std::mutex> lock(mutex_);
    if(active_&&generation==generation_){state_.geometryValid=false;state_.geometry={};
      state_.tangentShift[0][0]=state_.tangentShift[0][1]=0.0f;
      state_.tangentShift[1][0]=state_.tangentShift[1][1]=0.0f;
    }
  }
  void retire(uint64_t generation) {
    std::lock_guard<std::mutex> lock(mutex_);
    if(generation==generation_){state_={};active_=false;}
  }
  SystemRead read() const {
    std::lock_guard<std::mutex> lock(mutex_);return state_;
  }
  bool displayFrequency(uint64_t generation,float hz,bool estimated) {
    if(!std::isfinite(hz)||hz<=0)return false;
    std::lock_guard<std::mutex> lock(mutex_);
    if(!active_||generation!=generation_)return false;
    state_.displayFrequency=hz;state_.displayFrequencyAvailable=true;
    state_.displayFrequencyEstimated=estimated;return true;
  }
  bool hiddenMasks(uint64_t generation,std::shared_ptr<const NativeHiddenMasks> masks) {
    std::lock_guard<std::mutex> lock(mutex_);
    if(!active_||generation!=generation_||(masks&&masks->generation!=generation))return false;
    state_.hiddenMasks=std::move(masks);return true;
  }
  void recommend(const uint32_t (&width)[2], const uint32_t (&height)[2]) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!active_) return;
    for (unsigned eye=0; eye<2; ++eye) {
      if (!width[eye] || !height[eye] || width[eye]>16384 || height[eye]>16384) return;
    }
    for (unsigned eye=0; eye<2; ++eye) {
      state_.recommendedWidth[eye]=gameFacingDimension(width[eye]);
      state_.recommendedHeight[eye]=gameFacingDimension(height[eye]);
    }
  }
 private:
  mutable std::mutex mutex_;
  SystemRead state_{};
  uint64_t generation_=0,sequence_=0;
  bool active_=false;
};
}
