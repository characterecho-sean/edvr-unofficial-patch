#pragma once

// F31: the head pose the runtime last answered to Elite's "now" request (GetDeviceToAbsoluteTrackingPose, head_pose_time.h), published for the
// next frame's input (native_frame_client.h, EdvrNativeFrameInput version 2). Written on the game thread that asks (openvr_system.cpp); read on
// the frame thread. A mutex: one 48-byte copy per call. The pose is the same rigid transform the render head is handed as physicalHead
// (rigid(head.pose), seated space), so the d3d11 half can take their delta without any origin correction.

#include <cstdint>
#include <cstring>
#include <mutex>

namespace edvr::openxr {

struct NowHeadPublication {
  std::mutex mutex;
  float pose[12]{};
  uint32_t sequence = 0;   // the number of answers published (the first is 1)
  bool valid = false;
};

inline NowHeadPublication& nowHeadPublication() {
  static NowHeadPublication publication;
  return publication;
}

// `m` is a 3x4 row-major rigid transform (HmdMatrix34_t::m).
inline void publishNowHead(const float m[12]) {
  NowHeadPublication& p = nowHeadPublication();
  std::lock_guard<std::mutex> lock(p.mutex);
  std::memcpy(p.pose, m, sizeof(p.pose));
  ++p.sequence;
  p.valid = true;
}

// False until an answer has been published.
inline bool readNowHead(float out[12], uint32_t& sequence) {
  NowHeadPublication& p = nowHeadPublication();
  std::lock_guard<std::mutex> lock(p.mutex);
  if (!p.valid) return false;
  std::memcpy(out, p.pose, sizeof(p.pose));
  sequence = p.sequence;
  return true;
}

}  // namespace edvr::openxr
