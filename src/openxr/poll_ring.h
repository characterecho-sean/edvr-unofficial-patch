#pragma once
#include "reset_events.h"
#include <array>
#include <cstdint>
#include <mutex>

namespace edvr::openxr {

// The seated resets the game has not polled yet (native_pose_cache). The owner fills it at each waitPoses, idle pump and queued reset,
// moving items out of ResetEvents only while this ring has room; PollNextEvent drains it on the caller's thread under this mutex, so it
// no longer hops to the owner. Strictly FIFO.
//
// Drop policy. The fill checks room() before it moves an item, so the fill never drops one: a reset that does not fit stays in ResetEvents,
// which keeps its own bound and refuses new resets (the host's recentre reports LIMIT_REACHED) as it always did. push() on a full ring
// refuses and counts the refusal in `dropped`; the owner never calls it then. A quit is not held here (quitEventPending, guarded by the host's
// quitMutex_), so it is not subject to this ring.
class PollRing {
 public:
  static constexpr size_t capacity=16;
  struct Item {
    uint64_t generation=0;
    SeatedResetItem reset;
  };
  bool room() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return count_<capacity;
  }
  bool push(const Item& item) {
    std::lock_guard<std::mutex> lock(mutex_);
    if(count_==capacity){++dropped_;return false;}
    items_[(head_+count_)%capacity]=item;++count_;return true;
  }
  // The head, when it belongs to `generation`; otherwise nothing is taken, the same refusal ResetEvents::pop makes.
  bool pop(uint64_t generation,Item& out) {
    std::lock_guard<std::mutex> lock(mutex_);
    if(!generation||!count_||items_[head_].generation!=generation)return false;
    out=items_[head_];head_=(head_+1)%capacity;--count_;++drained_;
    return true;
  }
  // Drops everything held for another generation: nothing can ask for it, and it would block the head.
  void retireExcept(uint64_t generation) {
    std::lock_guard<std::mutex> lock(mutex_);
    if(count_&&items_[head_].generation!=generation){head_=count_=0;}
  }
  void clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    head_=count_=0;
  }
  // The counts since the last take: items handed out from the ring, and push refusals.
  void takeCounts(uint64_t& drained,uint64_t& dropped) {
    std::lock_guard<std::mutex> lock(mutex_);
    drained=drained_;dropped=dropped_;drained_=dropped_=0;
  }
  size_t size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return count_;
  }
 private:
  mutable std::mutex mutex_;
  std::array<Item,capacity> items_{};
  size_t head_=0,count_=0;
  uint64_t drained_=0,dropped_=0;
};

} // namespace edvr::openxr
