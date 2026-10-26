#pragma once

// F42 (proof for now_pose = render_locked): every head pose handed to Elite is hashed by frame and path. Once a second the line reports the mean and
// the largest number of distinct poses per frame, the "now" answers from the latched frame and from the other paths, the fallbacks out of the
// latched path, and the 95th percentile of the largest rotation angle between two samples of one frame. Under render_locked the expected values are
// 1 distinct pose a frame, 0 fallbacks and 0.000 degrees. The frame is set by the owner at each waitPoses; samples are taken on any thread.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>

namespace edvr::openxr {

enum class PoseSamplePath : unsigned { RenderLatch = 0, GameArray, NowLocked, NowOther, Count };

// Set by locateHeadFor on the calling thread: true when the "now" answer was the latched frame's pose (read by openvr_system's sample).
inline thread_local bool t_nowAnsweredLocked = false;

class HeadPoseSampleProof {
 public:
  static constexpr unsigned kPerFrame = 16;

  // Owner, at each waitPoses: closes the previous frame and starts this one.
  void setFrame(uint64_t frame) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (frame == frame_) return;
    closeLocked();
    frame_ = frame;
  }

  // `m` is a 3x4 row-major pose (the openvr matrix), `valid` its bPoseIsValid.
  void sample(const float m[12], bool valid, PoseSamplePath path) {
    std::lock_guard<std::mutex> lock(mutex_);
    ++paths_[unsigned(path) % unsigned(PoseSamplePath::Count)];
    uint64_t h = 1469598103934665603ull;
    const unsigned char* b = reinterpret_cast<const unsigned char*>(m);
    for (size_t i = 0; i < 12 * sizeof(float); ++i) {
      h ^= b[i];
      h *= 1099511628211ull;
    }
    h ^= valid ? 1u : 0u;
    h *= 1099511628211ull;
    for (unsigned i = 0; i < count_; ++i)
      if (hash_[i] == h) return;
    if (count_ < kPerFrame) {
      hash_[count_] = h;
      std::memcpy(pose_[count_], m, sizeof(pose_[0]));
      ++count_;
    }
  }

  // A "now" call the latched path could not answer (it fell through to the cache or the owner hop).
  void noteFallback() {
    std::lock_guard<std::mutex> lock(mutex_);
    ++fallbacks_;
  }

  // Once a second, from the owner's pose-gap publication (the same cadence as the gap line).
  template <class Sink>
  void flushIfDue(uint64_t nowMs, Sink&& sink) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (lastMs_ == 0) {
      lastMs_ = nowMs;
      return;
    }
    if (nowMs - lastMs_ < 1000) return;
    lastMs_ = nowMs;
    std::vector<double> angles = angles_;
    std::sort(angles.begin(), angles.end());
    const double p95 = angles.empty() ? 0.0 : angles[static_cast<size_t>(0.95 * double(angles.size() - 1))];
    const double meanDistinct = frames_ ? double(sumDistinct_) / double(frames_) : 0.0;
    char line[320];
    std::snprintf(line, sizeof(line),
                  "head pose samples per frame: mean %.2f, max %u distinct over %llu frames; now answers: latched %llu, other %llu; "
                  "fallbacks from the latched path %llu; angle_delta p95 %.3f deg (render %llu, game %llu)",
                  meanDistinct, maxDistinct_, static_cast<unsigned long long>(frames_),
                  static_cast<unsigned long long>(paths_[unsigned(PoseSamplePath::NowLocked)]),
                  static_cast<unsigned long long>(paths_[unsigned(PoseSamplePath::NowOther)]),
                  static_cast<unsigned long long>(fallbacks_), p95,
                  static_cast<unsigned long long>(paths_[unsigned(PoseSamplePath::RenderLatch)]),
                  static_cast<unsigned long long>(paths_[unsigned(PoseSamplePath::GameArray)]));
    sink(static_cast<const char*>(line));
    sumDistinct_ = 0;
    maxDistinct_ = 0;
    frames_ = 0;
    fallbacks_ = 0;
    angles_.clear();
    std::memset(paths_, 0, sizeof(paths_));
  }

 private:
  // The largest rotation angle, in degrees, between the frame's first pose and the others: arccos((trace(Ra Rb^T) - 1) / 2).
  static double rotationDeg(const float a[12], const float b[12]) {
    double tr = 0.0;
    for (unsigned i = 0; i < 3; ++i)
      for (unsigned k = 0; k < 3; ++k) tr += double(a[4 * i + k]) * double(b[4 * i + k]);
    const double c = std::max(-1.0, std::min(1.0, (tr - 1.0) * 0.5));
    return std::acos(c) * 180.0 / 3.14159265358979323846;
  }

  void closeLocked() {
    if (count_ == 0) return;
    double maxAngle = 0.0;
    for (unsigned i = 1; i < count_; ++i) maxAngle = std::max(maxAngle, rotationDeg(pose_[0], pose_[i]));
    sumDistinct_ += count_;
    maxDistinct_ = std::max(maxDistinct_, count_);
    ++frames_;
    angles_.push_back(maxAngle);
    count_ = 0;
  }

  std::mutex mutex_;
  uint64_t frame_ = 0;
  unsigned count_ = 0;
  uint64_t hash_[kPerFrame] = {};
  float pose_[kPerFrame][12] = {};
  uint64_t lastMs_ = 0;
  uint64_t sumDistinct_ = 0, frames_ = 0, fallbacks_ = 0;
  unsigned maxDistinct_ = 0;
  std::vector<double> angles_;
  uint64_t paths_[unsigned(PoseSamplePath::Count)] = {};
};

inline HeadPoseSampleProof g_headPoseProof;

}  // namespace edvr::openxr
