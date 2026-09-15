#pragma once
#include <chrono>

namespace edvr::openxr {
// Optional wall-clock brackets. These never wait for GPU completion and must
// not be interpreted as GPU busy time or added to enclosing wall intervals.
class SubmissionWallScope final {
 public:
  explicit SubmissionWallScope(double* output) noexcept : output_(output) {
    if(output_) began_=Clock::now();
  }
  ~SubmissionWallScope() {
    if(output_) *output_+=std::chrono::duration<double,std::milli>(Clock::now()-began_).count();
  }
 private:
  using Clock=std::chrono::steady_clock;
  double* output_;
  Clock::time_point began_{};
};
struct TransferWallTimes {
  double producerDispatch=0, producerAcquire=0, producerFlush=0;
  double consumerAcquire=0, consumerFlush=0;
};
struct StereoWallTimes {
  double acquire=0, wait=0, draw=0, release=0;
};
} // namespace edvr::openxr
