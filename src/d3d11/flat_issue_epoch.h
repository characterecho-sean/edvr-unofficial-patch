#pragma once
// Owner/trust epoch for the flat geometry Issue certificate.
//
// A successful fresh validation may be reused only while this epoch is unchanged.
// Any setter that can change the shaders, predication, depth view, depth-stencil
// state, VS b1 binding/range, or the certified buffer's bytes bumps it. An
// unhooked Context1 writer freezes reuse instead of guessing. The fresh
// OM/VS/PS queries stay the failure path.

#include <atomic>
#include <cstdint>

namespace edvr {

struct FlatIssueEpochState {
    std::atomic<uint64_t> epoch{1};
    std::atomic<uintptr_t> buffer{0};
    std::atomic<uint32_t> frozen{0};
    std::atomic<uint64_t> reuse{0};
    std::atomic<uint64_t> fresh{0};
    std::atomic<uint64_t> observeReuse{0};
    std::atomic<uint64_t> observeFresh{0};
    std::atomic<uint64_t> depthSets{0};
    std::atomic<uint64_t> stageSets{0};
    std::atomic<uint64_t> rtSets{0};
    std::atomic<uint64_t> cb1Sets{0};
    std::atomic<uint64_t> cbWrites{0};
    std::atomic<uint64_t> revokes{0};
};

inline FlatIssueEpochState& flatIssueEpochState() {
    static FlatIssueEpochState state;
    return state;
}
inline uint64_t flatIssueEpochLoad() noexcept {
    return flatIssueEpochState().epoch.load(std::memory_order_acquire);
}
inline bool flatIssueEpochFrozen() noexcept {
    return flatIssueEpochState().frozen.load(std::memory_order_acquire) != 0;
}
inline void flatIssueEpochFreeze() noexcept {
    flatIssueEpochState().frozen.store(1, std::memory_order_release);
}
inline void flatIssueEpochBump(std::atomic<uint64_t>* counter) noexcept {
    auto& state = flatIssueEpochState();
    state.epoch.fetch_add(1, std::memory_order_acq_rel);
    if (counter) counter->fetch_add(1, std::memory_order_relaxed);
}
inline void flatIssueEpochBumpDepth() noexcept { flatIssueEpochBump(&flatIssueEpochState().depthSets); }
inline void flatIssueEpochBumpStage() noexcept { flatIssueEpochBump(&flatIssueEpochState().stageSets); }
inline void flatIssueEpochBumpRt() noexcept { flatIssueEpochBump(&flatIssueEpochState().rtSets); }
inline void flatIssueEpochBumpCb() noexcept { flatIssueEpochBump(&flatIssueEpochState().cb1Sets); }
// Frame changes, ClearState, command lists and context swaps. No setter counter:
// a flight with fresh draws and quiet setters reads this instead.
inline void flatIssueEpochRevoke() noexcept {
    auto& state = flatIssueEpochState();
    state.epoch.fetch_add(1, std::memory_order_acq_rel);
    state.buffer.store(0, std::memory_order_release);
    state.revokes.fetch_add(1, std::memory_order_relaxed);
}
inline void flatIssueEpochPublishBuffer(uintptr_t buffer) noexcept {
    flatIssueEpochState().buffer.store(buffer, std::memory_order_release);
}
// A write of the buffer a live certificate names. A null resource is an unknown
// destination and revokes too. Any other resource leaves the epoch alone.
inline void flatIssueEpochNoteResource(const void* resource) noexcept {
    auto& state = flatIssueEpochState();
    const uintptr_t watched = state.buffer.load(std::memory_order_acquire);
    if (!watched) return;
    if (resource && reinterpret_cast<uintptr_t>(resource) != watched) return;
    state.epoch.fetch_add(1, std::memory_order_acq_rel);
    state.cbWrites.fetch_add(1, std::memory_order_relaxed);
}
inline void flatIssueEpochCountReuse() noexcept {
    flatIssueEpochState().reuse.fetch_add(1, std::memory_order_relaxed);
}
inline void flatIssueEpochCountFresh() noexcept {
    flatIssueEpochState().fresh.fetch_add(1, std::memory_order_relaxed);
}
inline void flatIssueEpochCountObserveReuse() noexcept {
    flatIssueEpochState().observeReuse.fetch_add(1, std::memory_order_relaxed);
}
inline void flatIssueEpochCountObserveFresh() noexcept {
    flatIssueEpochState().observeFresh.fetch_add(1, std::memory_order_relaxed);
}

struct FlatIssueEpochWindow {
    uint64_t reuse = 0, fresh = 0, observeReuse = 0, observeFresh = 0;
    uint64_t depthSets = 0, stageSets = 0, rtSets = 0, cb1Sets = 0, cbWrites = 0, revokes = 0;
    uint32_t frozen = 0;
};
inline uint64_t flatIssueEpochTake(std::atomic<uint64_t>& counter) noexcept {
    return counter.exchange(0, std::memory_order_relaxed);
}
inline FlatIssueEpochWindow flatIssueEpochTakeWindow() noexcept {
    auto& state = flatIssueEpochState();
    FlatIssueEpochWindow window;
    window.reuse = flatIssueEpochTake(state.reuse);
    window.fresh = flatIssueEpochTake(state.fresh);
    window.observeReuse = flatIssueEpochTake(state.observeReuse);
    window.observeFresh = flatIssueEpochTake(state.observeFresh);
    window.depthSets = flatIssueEpochTake(state.depthSets);
    window.stageSets = flatIssueEpochTake(state.stageSets);
    window.rtSets = flatIssueEpochTake(state.rtSets);
    window.cb1Sets = flatIssueEpochTake(state.cb1Sets);
    window.cbWrites = flatIssueEpochTake(state.cbWrites);
    window.revokes = flatIssueEpochTake(state.revokes);
    window.frozen = state.frozen.load(std::memory_order_acquire);
    return window;
}

}  // namespace edvr
