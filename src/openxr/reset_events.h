#pragma once
#include "space_pose.h"
#include <array>
#include <mutex>

namespace edvr::openxr {
// One queued seated reset, as the game is told it. Shared by ResetEvents and the pose-cache poll ring (poll_ring.h).
struct SeatedResetItem {
  uint64_t originGeneration=0,nowMs=0;
  vr::TrackedDevicePose_t pose{};
};

// The VREvent_SeatedZeroPoseReset a queued reset becomes when the game polls it. ResetEvents::pop and the poll ring both build it here.
inline void seatedResetEvent(const SeatedResetItem& item,vr::ETrackingUniverseOrigin origin,uint64_t originGeneration,uint64_t nowMs,
                             vr::VREvent_t& event,vr::TrackedDevicePose_t& pose) {
  event={};
  event.eventType=vr::VREvent_SeatedZeroPoseReset;
  event.trackedDeviceIndex=vr::k_unTrackedDeviceIndexInvalid;
  event.data.seatedZeroPoseReset.bResetBySystemMenu=false;
  event.eventAgeSeconds=nowMs>=item.nowMs?float(double(nowMs-item.nowMs)/1000.0):0.f;
  pose=origin==vr::TrackingUniverseSeated&&originGeneration==item.originGeneration?
    item.pose:invalidHeadPose(true);
}

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
    const SeatedResetItem item=items_[head_];head_=(head_+1)%capacity;--count_;
    seatedResetEvent(item,origin,originGeneration,nowMs,event,pose);
    return true;
  }
  // The head reset, moved out for the pose-cache poll ring (the owner fills it). Only for the generation the queue was begun with.
  bool takeFor(uint64_t generation,SeatedResetItem& out) {
    std::lock_guard<std::mutex> lock(mutex_);
    if(!generation||generation!=generation_||!count_)return false;
    out=items_[head_];head_=(head_+1)%capacity;--count_;
    return true;
  }
  void retire(uint64_t generation) {
    std::lock_guard<std::mutex> lock(mutex_);
    if(generation&&generation==generation_){generation_=0;head_=count_=0;items_={};}
  }
 private:
  mutable std::mutex mutex_;
  std::array<SeatedResetItem,capacity> items_{};
  size_t head_=0,count_=0;uint64_t generation_=0;
};
} // namespace edvr::openxr
