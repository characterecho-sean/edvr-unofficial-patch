#pragma once

// Elite's "now" head pose, answered at the drawn frame's display time (docs\terrain-culling.md, "What EDVR does now").
//
// THE DEFECT. Elite builds its planet-terrain culling camera from a head pose it asks IVRSystem::GetDeviceToAbsoluteTrackingPose for with a
// prediction of about 0 s, i.e. "now", while it DRAWS with the pose WaitGetPoses handed the render thread, located at the frame's
// predictedDisplayTime. Located at the wall clock, the culling pose sat 41-44 ms before the drawn one (flight 3, 2026-10-09) and was
// turned up to 4.1 degrees from it, in step with head speed (r = +0.98): the terrain tiles chosen for the leading edge of a head turn were
// the wrong ones, and the black squares were the gap. Answering those requests at the display time removes it.
//
// THE FILTER. Which requests are Elite's own "now"? Not a build's return address (a game update would silently stop the fix): any call
// whose return address lies inside the game executable's mapped image and whose prediction is under 5 ms either way. A real prediction (a
// caller that wants the future), a caller outside the image (an overlay, a tool) and a call before any frame has been waited are answered
// as they always were, at the wall clock plus the prediction.
#include "exe_module.h"

#include <cmath>
#include <cstdint>
#include <cstdio>

namespace edvr::openxr {

// A prediction this close to zero is a request for "now". The bound is exclusive: exactly 5 ms is a prediction.
constexpr float kNowPredictionLimitSeconds = 0.005f;

inline bool isNowRequest(float predictionSeconds) {
  return std::fabs(predictionSeconds) < kNowPredictionLimitSeconds;   // (a NaN is false: the caller refuses it before this)
}

// Is this call one the display time answers? `returnRva` is the caller's return address as an RVA in the game executable (frameRva):
// a value below kFrameUnknown is inside the image.
inline bool answeredAtDisplayTime(uint32_t returnRva, float predictionSeconds) {
  return returnRva < kFrameUnknown && isNowRequest(predictionSeconds);
}

// The instant such a call is located at: the latest frame's predictedDisplayTime, or false when there is none yet (no frame waited, a display
// time that is not positive) and the call falls back to now + prediction.
inline bool displayTimeTarget(int64_t latestDisplayTime, int64_t* out) {
  if (latestDisplayTime <= 0) return false;
  *out = latestDisplayTime;
  return true;
}

// The log lines that say the fix is acting, and for whom. The first caller to qualify writes one line; each further DISTINCT return RVA that
// qualifies writes one more, up to kFurtherCap, so a caller that turns up after a game update is named. One more line says the cap was reached.
//   head pose: Elite's "now" requests are answered at the drawn frame's display time (first from exe+0x...)
//   head pose: another Elite caller of "now" is answered at the drawn frame's display time (exe+0x...)
//   head pose: more than 8 further callers of "now"; the rest are not logged
class HeadPoseSightings {
 public:
  static constexpr unsigned kFurtherCap = 8;
  template <class Sink>
  void note(uint32_t returnRva, Sink&& sink) {
    for (unsigned i = 0; i < count_; ++i)
      if (seen_[i] == returnRva) return;
    char line[160];
    if (count_ == 0) {
      std::snprintf(line, sizeof(line), "head pose: Elite's \"now\" requests are answered at the drawn frame's display time (first from exe+0x%X)", returnRva);
    } else if (count_ <= kFurtherCap) {
      std::snprintf(line, sizeof(line), "head pose: another Elite caller of \"now\" is answered at the drawn frame's display time (exe+0x%X)", returnRva);
    } else {
      if (!capSaid_) {
        capSaid_ = true;
        std::snprintf(line, sizeof(line), "head pose: more than %u further callers of \"now\"; the rest are not logged", kFurtherCap);
        sink(static_cast<const char*>(line));
      }
      return;
    }
    seen_[count_++] = returnRva;
    sink(static_cast<const char*>(line));
  }
  unsigned distinct() const { return count_; }

 private:
  uint32_t seen_[kFurtherCap + 1]{};
  unsigned count_ = 0;
  bool capSaid_ = false;
};

}  // namespace edvr::openxr
