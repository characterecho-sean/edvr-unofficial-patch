#pragma once
#include "space_pose.h"
#include <array>
#include <mutex>

namespace edvr::openxr {
// Application seated resets only. Runtime focus/quit/reference-space events
// need separately qualified OpenVR mappings; none are invented here. The host
// serializes reset writers and checks capacity before changing the XR origin.
class ResetEvents {
 public:
  static constexpr size_t capacity=16;
  bool begin(uint64_t generation) {
    std::lock_guard<std::mutex> lock(mutex_);
    if(generation_||!generation)return false;
    generation_=generation;head_=count_=0;return true;
  }
  bool room(uint64_t generation) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return generation&&generation==generation_&&count_<capacity;
  }
  bool push(uint64_t generation,uint64_t originGeneration,uint64_t nowMs,
            const vr::TrackedDevicePose_t& seatedPose) {
    std::lock_guard<std::mutex> lock(mutex_);
    if(!generation||generation!=generation_||!originGeneration||count_==capacity)return false;
    items_[(head_+count_)%capacity]={originGeneration,nowMs,seatedPose};++count_;return true;
  }
  bool pop(uint64_t generation,uint64_t originGeneration,vr::ETrackingUniverseOrigin origin,
           uint64_t nowMs,vr::VREvent_t& event,vr::TrackedDevicePose_t& pose) {
    std::lock_guard<std::mutex> lock(mutex_);
    if(!generation||generation!=generation_||!count_)return false;
    const Item item=items_[head_];head_=(head_+1)%capacity;--count_;
    event={};event.eventType=vr::VREvent_SeatedZeroPoseReset;
    event.trackedDeviceIndex=vr::k_unTrackedDeviceIndexInvalid;
    event.data.seatedZeroPoseReset.bResetBySystemMenu=false;
    event.eventAgeSeconds=nowMs>=item.nowMs?float(double(nowMs-item.nowMs)/1000.0):0.f;
    pose=origin==vr::TrackingUniverseSeated&&originGeneration==item.originGeneration?
      item.pose:invalidHeadPose(true);
    return true;
  }
  void retire(uint64_t generation) {
    std::lock_guard<std::mutex> lock(mutex_);
    if(generation&&generation==generation_){generation_=0;head_=count_=0;items_={};}
  }
 private:
  struct Item {uint64_t originGeneration=0,nowMs=0;vr::TrackedDevicePose_t pose{};};
  mutable std::mutex mutex_;
  std::array<Item,capacity> items_{};
  size_t head_=0,count_=0;uint64_t generation_=0;
};
} // namespace edvr::openxr
