#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace edvr::openxr {
// Bounded owner-only windows, rearmed every 30 seconds. No allocation, file
// I/O or GPU synchronization while collecting. A full window reports once.
class SubmissionStats final {
 public:
  static constexpr unsigned warmup = 64, capacity = 256;
  struct Sample {
    uint64_t sequence = 0, callbacks = 0;
    uint64_t featureEpoch = 0;
    double submitMs = 0, dispatchMs = 0, treatmentMs = 0;
    double producerDispatchMs=0, producerAcquireMs=0, producerFlushMs=0;
    double consumerAcquireMs=0, consumerFlushMs=0, receiveMs=0;
    double xrAcquireMs=0, xrWaitMs=0, xrDrawMs=0, xrReleaseMs=0, endFrameMs=0;
    uint32_t width[2]{}, height[2]{};
    uint32_t outputWidth[2]{}, outputHeight[2]{}, treatments[2]{};
  };
  struct Distribution { double p50 = 0, p95 = 0, p99 = 0; };
  // In-progress windows are not truncated by the timer. Interrupted sequences
  // restart warmup; elapsed time alone never constitutes a valid sample.
  bool advance(uint64_t nowMs) {
    if(!armed_) { armed_=true; windowBeganMs_=nowMs; return true; }
    if(!full()||nowMs<windowBeganMs_||nowMs-windowBeganMs_<30000) return false;
    seen_=count_=0;lastSequence_=0;shape_={};windowBeganMs_=nowMs;++window_;
    return true;
  }
  uint64_t window() const { return window_; }
  bool add(const Sample& sample) {
    if (full() || !sample.sequence || sample.sequence <= lastSequence_ ||
        !std::isfinite(sample.submitMs) || !std::isfinite(sample.dispatchMs) ||
        !std::isfinite(sample.treatmentMs) || sample.submitMs < 0 ||
        sample.dispatchMs < 0 || sample.treatmentMs < 0 ||
        sample.treatmentMs > sample.dispatchMs ||
        !sample.width[0] || !sample.width[1] || !sample.height[0] || !sample.height[1]) return false;
    for(double value:{sample.producerDispatchMs,sample.producerAcquireMs,sample.producerFlushMs,
        sample.consumerAcquireMs,sample.consumerFlushMs,sample.receiveMs,sample.xrAcquireMs,
        sample.xrWaitMs,sample.xrDrawMs,sample.xrReleaseMs,sample.endFrameMs})
      if(!std::isfinite(value)||value<0) return false;
    const bool gap=lastSequence_&&sample.sequence!=lastSequence_+1;
    lastSequence_ = sample.sequence;
    if (seen_ && (sample.width[0] != shape_.width[0] || sample.width[1] != shape_.width[1] ||
        sample.height[0] != shape_.height[0] || sample.height[1] != shape_.height[1] ||
        sample.outputWidth[0]!=shape_.outputWidth[0]||sample.outputWidth[1]!=shape_.outputWidth[1]||
        sample.outputHeight[0]!=shape_.outputHeight[0]||sample.outputHeight[1]!=shape_.outputHeight[1]||
        sample.treatments[0]!=shape_.treatments[0]||sample.treatments[1]!=shape_.treatments[1]||
        sample.featureEpoch!=shape_.featureEpoch||gap)) {
      seen_ = count_ = 0; // never mix sizes, treatments or interrupted pairs
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
  uint64_t windowBeganMs_ = 0, window_ = 1;
  bool armed_ = false;
  unsigned seen_ = 0, count_ = 0;
};
} // namespace edvr::openxr
