#pragma once
#include "system_source.h"
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
    state_=metadata;sequence_=0;active_=true;return generation_;
  }
  bool publish(const GeometryInput& input,bool focusKnown,bool focused) {
    GeometrySnapshot candidate{};const bool valid=makeGeometrySnapshot(input,candidate);
    std::lock_guard<std::mutex> lock(mutex_);
    if(!active_||input.generation!=generation_||input.sequence<=sequence_)return false;
    sequence_=input.sequence;state_.geometryValid=valid;
    state_.focusKnown=focusKnown;state_.focused=focused;
    if(valid)state_.geometry=candidate;else state_.geometry={};return valid;
  }
  void retire(uint64_t generation) {
    std::lock_guard<std::mutex> lock(mutex_);
    if(generation==generation_){state_={};active_=false;}
  }
  SystemRead read() const {
    std::lock_guard<std::mutex> lock(mutex_);return state_;
  }
 private:
  mutable std::mutex mutex_;
  SystemRead state_{};
  uint64_t generation_=0,sequence_=0;
  bool active_=false;
};
}
