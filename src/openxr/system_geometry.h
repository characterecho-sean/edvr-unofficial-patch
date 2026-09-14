#pragma once
#include "geometry_snapshot.h"
#include <mutex>
#include <algorithm>

namespace edvr::openxr {
// Readers copy one complete immutable generation/frame. No lock covers an XR
// call, graphics work, or a wait. The externally serialized owner publishes
// only after the frame operation that produced the record has succeeded.
class GeometryStore {
 public:
  uint64_t beginGeneration() {
    std::lock_guard<std::mutex> lock(mutex_);
    valid_=false;retired_=true;sequence_=0;
    if(generation_==(std::numeric_limits<uint64_t>::max)())return 0;
    ++generation_;retired_=false;return generation_;
  }
  bool publish(const GeometryInput& input) {
    GeometrySnapshot candidate{};const bool valid=makeGeometrySnapshot(input,candidate);
    std::lock_guard<std::mutex> lock(mutex_);
    if(retired_||!input.generation||input.generation!=generation_||input.sequence<=sequence_)return false;
    sequence_=input.sequence;valid_=valid;
    if(valid)snapshot_=candidate;
    return valid;
  }
  bool invalidate(uint64_t generation,uint64_t sequence) {
    std::lock_guard<std::mutex> lock(mutex_);
    if(retired_||generation!=generation_||sequence<sequence_)return false;
    sequence_=sequence;valid_=false;return true;
  }
  void retire(uint64_t generation) {
    std::lock_guard<std::mutex> lock(mutex_);
    if(generation==generation_){valid_=false;retired_=true;}
  }
  bool read(GeometrySnapshot& out)const {
    std::lock_guard<std::mutex> lock(mutex_);
    if(!valid_||retired_)return false;
    out=snapshot_;return true;
  }
 private:
  mutable std::mutex mutex_;
  GeometrySnapshot snapshot_{};
  uint64_t generation_=0,sequence_=0;
  bool valid_=false,retired_=true;
};

// Read transaction for the future IVRSystem_012 implementation. All getters on
// this value use the same record, even if the source publishes another frame.
// This is not an implementation/advertisement of the 44-method VR interface.
class SystemGeometry {
 public:
  explicit SystemGeometry(const GeometryStore& source):valid_(source.read(snapshot_)){}
  bool valid()const{return valid_;}
  uint64_t generation()const{return valid_?snapshot_.native.generation:0;}
  uint64_t sequence()const{return valid_?snapshot_.native.sequence:0;}
  bool recommendedSize(uint32_t& width,uint32_t& height)const {
    if(!valid_)return false;
    // Historical OpenVR exposes one recommendation for both eyes.
    width=(std::max)(snapshot_.native.width[0],snapshot_.native.width[1]);
    height=(std::max)(snapshot_.native.height[0],snapshot_.native.height[1]);return true;
  }
  bool projection(vr::EVREye eye,float nearZ,float farZ,vr::EGraphicsAPIConvention convention,vr::HmdMatrix44_t& out)const {
    if(!valid_||!validEye(eye))return false;
    return projectionMatrix(snapshot_.native.views[unsigned(eye)].fov,nearZ,farZ,convention,out);
  }
  bool raw(vr::EVREye eye,RawFov& out)const {
    if(!valid_||!validEye(eye))return false;out=snapshot_.raw[unsigned(eye)];return true;
  }
  bool eyeToHead(vr::EVREye eye,vr::HmdMatrix34_t& out)const {
    if(!valid_||!validEye(eye))return false;out=snapshot_.eyeToHead[unsigned(eye)];return true;
  }
  bool headToLocal(vr::HmdMatrix34_t& out)const {
    if(!valid_)return false;out=snapshot_.headToLocal;return true;
  }
 private:
  static bool validEye(vr::EVREye eye){return eye==vr::Eye_Left||eye==vr::Eye_Right;}
  GeometrySnapshot snapshot_{};
  bool valid_=false;
};
}
