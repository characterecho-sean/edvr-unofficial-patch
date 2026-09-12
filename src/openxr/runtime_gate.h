#pragma once

#include <cstdint>
#include <limits>
#include <mutex>

namespace edvr::openxr {

// CPU-only lifetime gate for operations which may be inside an OpenXR call.
// The caller owns the actual runtime resources and must keep this object alive
// until every Lease has been released. One teardown owner must serialize
// canDestroy -> resource destruction -> finishGeneration; these calls are not
// a competing-destroyer election. Stopping does not interrupt an XR wait.
class RuntimeGate {
 public:
  class Lease {
   public:
    Lease() noexcept = default;
    Lease(const Lease&) = delete;
    Lease& operator=(const Lease&) = delete;

    Lease(Lease&& other) noexcept
        : gate_(other.gate_), generation_(other.generation_) {
      other.gate_ = nullptr;
      other.generation_ = 0;
    }

    Lease& operator=(Lease&& other) noexcept {
      if (this != &other) {
        release();
        gate_ = other.gate_;
        generation_ = other.generation_;
        other.gate_ = nullptr;
        other.generation_ = 0;
      }
      return *this;
    }

    ~Lease() { release(); }

    bool valid() const noexcept { return gate_ != nullptr; }
    explicit operator bool() const noexcept { return valid(); }
    uint64_t generation() const noexcept { return valid() ? generation_ : 0; }

   private:
    friend class RuntimeGate;
    Lease(RuntimeGate* gate, uint64_t generation) noexcept
        : gate_(gate), generation_(generation) {}

    void release() noexcept {
      if (gate_ != nullptr) {
        gate_->release(generation_);
        gate_ = nullptr;
        generation_ = 0;
      }
    }

    RuntimeGate* gate_ = nullptr;
    uint64_t generation_ = 0;
  };

  RuntimeGate() = default;
  RuntimeGate(const RuntimeGate&) = delete;
  RuntimeGate& operator=(const RuntimeGate&) = delete;

  // Returns zero when a prior generation has not been stopped and destroyed,
  // or when the monotonic generation counter is exhausted.
  uint64_t beginGeneration() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ != State::Inactive || activeLeases_ != 0 ||
        generation_ == (std::numeric_limits<uint64_t>::max)()) {
      return 0;
    }
    ++generation_;
    state_ = State::Running;
    return generation_;
  }

  // Never wait for an outstanding operation (only the short state mutex).
  // A stopping or stale generation cannot enter.
  Lease tryEnter(uint64_t generation) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ != State::Running || generation == 0 || generation != generation_ ||
        activeLeases_ != 0) {
      return Lease();
    }
    activeLeases_ = 1;
    return Lease(this, generation);
  }

  bool requestStop(uint64_t generation) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ != State::Running || generation == 0 || generation != generation_) {
      return false;
    }
    state_ = State::Stopping;
    return true;
  }

  bool canDestroy(uint64_t generation) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return generation != 0 && generation == generation_ &&
           state_ == State::Stopping && activeLeases_ == 0;
  }

  bool finishGeneration(uint64_t generation) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (generation == 0 || generation != generation_ ||
        state_ != State::Stopping || activeLeases_ != 0) {
      return false;
    }
    state_ = State::Inactive;
    return true;
  }

 private:
  enum class State { Inactive, Running, Stopping };

  void release(uint64_t generation) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    // A stale lease must never decrement a later generation. In normal use a
    // new generation cannot begin until this count is zero; the comparison is
    // still intentional defense against misuse and future changes.
    if (generation == generation_ && activeLeases_ != 0) {
      activeLeases_ = 0;
    }
  }

  mutable std::mutex mutex_;
  uint64_t generation_ = 0;
  unsigned activeLeases_ = 0;
  State state_ = State::Inactive;
};

using RuntimeOperationGate = RuntimeGate;

}  // namespace edvr::openxr
