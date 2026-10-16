// Who owns a flat discovery window: the thread of the first real Present.
//
// Platform-independent so tools\flat_temporal_test can drive it from two
// threads. flat_temporal.cpp passes its own atomics; the rig passes its own.
// A DXGI_PRESENT_TEST call presents nothing, so it is not a frame boundary
// and must not decide which thread's Presents bound the window: a thread that
// only probes would own a window whose real Presents come from another thread,
// the deadline (owner-only) would never run, and every observer call from the
// real thread would count as foreign while collection stayed armed.
#pragma once
#include <atomic>
#include <cstdint>

namespace edvr {

constexpr uint32_t kFlatPresentTestFlag = 0x00000001u;  // DXGI_PRESENT_TEST

// Called at every Present while the window is armed. True exactly once per
// arming: for the first Present that is not a test, which becomes the owner.
template<class Tid>
inline bool flatOwnerClaimOnPresent(std::atomic<bool>& waiting,
                                    std::atomic<Tid>& owner,
                                    Tid thread, uint32_t flags) {
    if (flags & kFlatPresentTestFlag) return false;   // presents nothing: not a frame boundary, claims nothing, leaves the wait armed
    if (!waiting.exchange(false, std::memory_order_acq_rel)) return false;
    owner.store(thread, std::memory_order_release);
    return true;
}

// The deadline closes a collection only from the owner's Present, paused or not.
template<class Tid>
inline bool flatOwnerMayExpire(const std::atomic<bool>& capturing,
                               const std::atomic<Tid>& owner, Tid thread) {
    return capturing.load(std::memory_order_acquire) &&
           owner.load(std::memory_order_acquire) == thread;
}

// The observers' tail check (flatTemporalCapturing): only the owner observes;
// any other thread is counted and refused.
template<class Tid>
inline bool flatObserveAsOwner(const std::atomic<Tid>& owner,
                               std::atomic<uint64_t>& foreignCalls, Tid thread) {
    const Tid o = owner.load(std::memory_order_acquire);
    if (o != 0 && thread == o) return true;
    foreignCalls.fetch_add(1, std::memory_order_relaxed);
    return false;
}

}  // namespace edvr
