#pragma once

#include "geometry_snapshot.h"
#include <openxr/openxr.h>
#include <cstdint>

namespace edvr::openxr {

// A small, externally serialized policy for LOCAL reference-space changes.
// The runtime applies natural-origin compensation; this object only orders notifications
// and exposes the epoch at which cached pose history must be invalidated.
class ReferenceChanges final {
 public:
  bool begin(XrSession session) noexcept {
    if (active_ || session == XR_NULL_HANDLE) return false;
    clearState(); active_ = true; session_ = session; epoch_ = 1; return true;
  }

  bool note(const XrEventDataReferenceSpaceChangePending& event) noexcept {
    if (!active_) return false;
    if (event.session != session_ || event.referenceSpaceType != XR_REFERENCE_SPACE_TYPE_LOCAL)
      return true;
    if (event.type != XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING || event.next != nullptr ||
        (event.poseValid != XR_FALSE && event.poseValid != XR_TRUE)) { retire(); return false; }
    if (event.poseValid == XR_TRUE && !detail::poseValid(event.poseInPreviousSpace)) {
      retire(); return false;
    }
    if (count_ == kCapacity) { retire(); return false; }
    unsigned at = count_;
    while (at && pending_[at - 1].changeTime > event.changeTime) {
      pending_[at] = pending_[at - 1]; --at;
    }
    pending_[at].changeTime = event.changeTime; ++count_;
    return true;
  }

  bool advance(XrTime target, uint64_t& applied) noexcept {
    if (!active_ || (advanced_ && target < lastTarget_)) { retire(); return false; }
    uint64_t n = 0;
    while (n < count_ && pending_[n].changeTime <= target) ++n;
    if (n > (UINT64_MAX - epoch_)) { retire(); return false; }
    epoch_ += n;
    for (unsigned i = static_cast<unsigned>(n); i < count_; ++i) pending_[i - n] = pending_[i];
    count_ -= static_cast<unsigned>(n); lastTarget_ = target; advanced_ = true; applied = n;
    return true;
  }

  // Query only; callers use this to reject a future gameplay prediction that
  // would cross an origin discontinuity. It never consumes or advances events.
  bool crosses(XrTime from, XrTime to) const noexcept {
    if (!active_ || to <= from) return false;
    for (unsigned i = 0; i < count_; ++i)
      if (from < pending_[i].changeTime && pending_[i].changeTime <= to) return true;
    return false;
  }

  uint64_t epoch() const noexcept { return active_ ? epoch_ : 0; }
  bool active() const noexcept { return active_; }
  void clear() noexcept { clearState(); }

 private:
  struct Pending { XrTime changeTime = 0; };
  static constexpr unsigned kCapacity = 16;
  void clearState() noexcept {
    session_ = XR_NULL_HANDLE; count_ = 0; epoch_ = 0; lastTarget_ = 0;
    advanced_ = false; active_ = false;
  }
  void retire() noexcept { clearState(); }

  XrSession session_ = XR_NULL_HANDLE;
  Pending pending_[kCapacity]{};
  unsigned count_ = 0;
  uint64_t epoch_ = 0;
  XrTime lastTarget_ = 0;
  bool advanced_ = false, active_ = false;
};

} // namespace edvr::openxr
