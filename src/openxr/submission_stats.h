#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace edvr::openxr {
// One bounded, owner-only window per session. No allocation, file I/O or GPU
// synchronization while collecting. A report is requested once, when full.
class SubmissionStats final {
 public:
  static constexpr unsigned warmup = 64, capacity = 256;
  struct Sample {
    uint64_t sequence = 0, callbacks = 0;
    double submitMs = 0, dispatchMs = 0, treatmentMs = 0;
    uint32_t width[2]{}, height[2]{};
  };
  struct Distribution { double p50 = 0, p95 = 0, p99 = 0; };
  bool add(const Sample& sample) {
    if (full() || !sample.sequence || sample.sequence <= lastSequence_ ||
        !std::isfinite(sample.submitMs) || !std::isfinite(sample.dispatchMs) ||
        !std::isfinite(sample.treatmentMs) || sample.submitMs < 0 ||
        sample.dispatchMs < 0 || sample.treatmentMs < 0 ||
        sample.treatmentMs > sample.dispatchMs ||
        !sample.width[0] || !sample.width[1] || !sample.height[0] || !sample.height[1]) return false;
    lastSequence_ = sample.sequence;
    if (seen_ && (sample.width[0] != shape_.width[0] || sample.width[1] != shape_.width[1] ||
        sample.height[0] != shape_.height[0] || sample.height[1] != shape_.height[1])) {
      seen_ = count_ = 0; // never aggregate different submitted sizes
    }
    shape_ = sample;
    if (++seen_ <= warmup) return false;
    samples_[count_++] = sample;
    return full();
  }
  bool full() const { return count_ == capacity; }
  unsigned count() const { return count_; }
  const Sample& first() const { return samples_[0]; }
  const Sample& last() const { return samples_[count_ ? count_ - 1 : 0]; }
  double meanCallbacks() const {
    uint64_t total = 0; for (unsigned i = 0; i < count_; ++i) total += samples_[i].callbacks;
    return count_ ? double(total) / count_ : 0;
  }
  Distribution distribution(double Sample::*field) const {
    std::array<double, capacity> values{};
    for (unsigned i = 0; i < count_; ++i) values[i] = samples_[i].*field;
    std::sort(values.begin(), values.begin() + count_);
    const auto percentile = [&](unsigned percent) {
      return count_ ? values[(count_ * percent + 99) / 100 - 1] : 0;
    };
    return {percentile(50), percentile(95), percentile(99)};
  }
 private:
  std::array<Sample, capacity> samples_{};
  Sample shape_{};
  uint64_t lastSequence_ = 0;
  unsigned seen_ = 0, count_ = 0;
};
} // namespace edvr::openxr
