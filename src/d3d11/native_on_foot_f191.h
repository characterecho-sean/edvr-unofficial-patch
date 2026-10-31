// F191 (native on foot VR): the hip reticle is found by its marker type, not by pool slot 10. Header only, no game or D3D11 state, so native_frame_test runs the very
// functions the hooks use. Journal: the arc doc, F191.
//
// The game's marker keys are slots in a pool, not identities (analysis\onfoot_prompt_anchor_334925.txt, section 2). EDVR took the aim reticle for "key 10" because on
// the first rigs the reticle happened to get slot 10. On the pilot's rig another marker owner has held slot 10 from the first on-foot frame since 2026-10-27 10:36
// (rc.2-45): the reticle gets slot 11 (then 12, 13, 14 as it is re-created), and every test for key 10 missed it. The F18 centring and its "follows aim" line, the F21
// follow, the F52 tag and the F85c hit depth did not run for it; the reticle went down the world-marker path (forward_lead) and sat at the view's centre instead of
// on the aim. A different player's RC4 log shows the reticle on key 10 (`type 1001 ... [keys 10]`), and for a few seconds `[keys 10,11]`: the allocator hands out the
// next slot while the old reticle is still alive.
//
// The reticle's identity is its marker owner: F92c's SetMarker hook (native_on_foot.cpp, f87p::markerRecord) already sees every SetMarker's type and key, and the
// reticle is the one that passes type 1001. This tracker holds the key type 1001 used.
//
//  * observe(key, now): called by that hook for each type 1001 SetMarker (game thread(s): the HUD update). The key becomes the newest; the key it replaces is kept as the
//    other one for kOverlapMs after it was last seen, so the overlap while the allocator re-creates the reticle (`[keys 10,11]`, both alive) is covered. A key the other
//    one holds, seen again within kOverlapMs, only refreshes the other one (the two keys alternate within a frame): the newest does not flip, and one change is logged.
//  * isReticle(key, now): the question every site asks (the pre-hook and post-hook on game threads; the draw decision on the render thread). True for the newest
//    key, always (the reticle owns its slot as long as the allocator keeps it; the owner calls SetMarker every frame, 90 a second), and for the other one while it was
//    seen within kOverlapMs. Before any type 1001 call has been seen (the hook not installed: fix.native_on_foot_marker_fix off, or the first frame on foot) the answer is
//    the old one, key 10 (kFallbackKey).
//  * reset(): the frame thread, when the hook is installed and when it is restored: a new session starts from the fallback, so a key of the last session is never
//    taken for the reticle.
//
// Threads. One atomic 64-bit word holds the whole decision (newest key, other key, when the other was last seen), so a reader on any thread sees one consistent state:
// observe() is a compare-and-swap loop (safe with several writers; with the one HUD thread it never retries), isReticle() one relaxed load. The newest key's own
// last-seen time (newestSeenMs_) is a second word only the writers touch: it dates the key that becomes the other one when a new key arrives. Readers never read it. A
// key in either half of the word was, at some moment, a key type 1001 passed: a stale or torn read can only be late or early about a real reticle key by one frame, never
// name a key that type 1001 never used.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace edvr {
namespace nofoot {
namespace f191 {

constexpr uint32_t kReticleMarkerType = 1001u;   // the indicator owner's type for the reticle (nofoot::f87::kIndicatorTypeInteractable; the rig checks the two agree)
constexpr uint32_t kFallbackKey = 10u;           // the key taken for the reticle until a type 1001 call has been seen (what the code assumed before F191)
constexpr uint32_t kOverlapMs = 500u;            // how long a replaced key still counts, from the last SetMarker that carried it
constexpr uint32_t kNoKey = 0xFFFFu;             // no key held in a half of the word; a SetMarker key at or past it is not a pool slot (the pool's keys are under 128)

struct Observed {
    bool changed = false;           // the newest key changed
    uint32_t previous = kNoKey;     // the key it replaced (kNoKey: none was held since the last reset)
};

// Within the window, from `then` to `now` (milliseconds, 32 bits, wrapping). A stamp up to 1 s ahead of the caller's clock (another thread stamped it after the caller
// read the clock) is fresh; one far ahead is the wrap of an old stamp, not fresh.
inline bool withinMs(uint32_t now, uint32_t then, uint32_t windowMs) noexcept {
    const int32_t age = static_cast<int32_t>(now - then);
    return age >= -1000 && age <= static_cast<int32_t>(windowMs);
}

// The word: bits 0..15 the newest key, 16..31 the other key, 32..63 when the other key was last seen (milliseconds).
constexpr uint64_t pack(uint32_t newest, uint32_t other, uint32_t otherMs) noexcept {
    return static_cast<uint64_t>(newest & 0xFFFFu) | (static_cast<uint64_t>(other & 0xFFFFu) << 16) | (static_cast<uint64_t>(otherMs) << 32);
}
constexpr uint32_t newestOf(uint64_t s) noexcept { return static_cast<uint32_t>(s & 0xFFFFu); }
constexpr uint32_t otherOf(uint64_t s) noexcept { return static_cast<uint32_t>((s >> 16) & 0xFFFFu); }
constexpr uint32_t otherMsOf(uint64_t s) noexcept { return static_cast<uint32_t>(s >> 32); }

class ReticleKeys {
public:
    // The hook for a type 1001 SetMarker (any game thread). A key past the pool's range is ignored.
    Observed observe(uint32_t key, uint32_t nowMs) noexcept {
        Observed out;
        if (key >= kNoKey) return out;
        uint64_t s = state_.load(std::memory_order_relaxed);
        for (;;) {
            const uint32_t newest = newestOf(s), other = otherOf(s), otherMs = otherMsOf(s);
            if (key == newest) {   // the steady state, every frame: one load, one store of the time
                newestSeenMs_.store(nowMs, std::memory_order_relaxed);
                return out;
            }
            uint64_t next;
            bool changed = false;
            if (key == other && withinMs(nowMs, otherMs, kOverlapMs)) {
                next = pack(newest, other, nowMs);   // the older allocation, still alive beside the newest
            } else {
                const uint32_t seen = newest == kNoKey ? 0u : newestSeenMs_.load(std::memory_order_relaxed);
                next = pack(key, newest, seen);      // a new allocation: the newest becomes the other one, dated by its last SetMarker
                changed = true;
            }
            if (state_.compare_exchange_weak(s, next, std::memory_order_relaxed)) {
                if (changed) {
                    newestSeenMs_.store(nowMs, std::memory_order_relaxed);
                    out.changed = true;
                    out.previous = newest;
                }
                return out;
            }
        }
    }

    // Is `key` the reticle's, at `nowMs`? Any thread.
    bool isReticle(uint32_t key, uint32_t nowMs) const noexcept {
        const uint64_t s = state_.load(std::memory_order_relaxed);
        const uint32_t newest = newestOf(s);
        if (newest == kNoKey) return key == kFallbackKey;
        if (key >= kNoKey) return false;
        if (key == newest) return true;
        return key == otherOf(s) && withinMs(nowMs, otherMsOf(s), kOverlapMs);
    }

    // The key the reticle takes now: the newest, or the fallback before any call was seen. Any thread.
    uint32_t newest() const noexcept {
        const uint32_t n = newestOf(state_.load(std::memory_order_relaxed));
        return n == kNoKey ? kFallbackKey : n;
    }
    // Has a type 1001 call been seen since the last reset? Any thread.
    bool seen() const noexcept { return newestOf(state_.load(std::memory_order_relaxed)) != kNoKey; }

    // The frame thread, when the SetMarker hook goes in or comes out: back to the fallback.
    void reset() noexcept {
        state_.store(pack(kNoKey, kNoKey, 0u), std::memory_order_relaxed);
        newestSeenMs_.store(0u, std::memory_order_relaxed);
    }

    // The key as the log says it: "11 (type 1001)", or "10 (assumed: no type 1001 call seen yet)". Any thread.
    int describe(char* buf, size_t n) const noexcept {
        const bool s = seen();
        return std::snprintf(buf, n, s ? "%u (type %u)" : "%u (assumed: no type %u call seen yet)", static_cast<unsigned>(newest()), static_cast<unsigned>(kReticleMarkerType));
    }

private:
    std::atomic<uint64_t> state_{pack(kNoKey, kNoKey, 0u)};
    std::atomic<uint32_t> newestSeenMs_{0u};
};

// The line for a change of the newest key. `previous` is what observe() returned (kNoKey: the first call since the reset, where the key in force was the fallback).
inline int formatKeyChange(char* buf, size_t n, uint32_t previous, uint32_t key) noexcept {
    if (previous == kNoKey) {
        return key == kFallbackKey
                   ? std::snprintf(buf, n, "reticle key (F191): %u (type %u), the key the code assumed", static_cast<unsigned>(key), static_cast<unsigned>(kReticleMarkerType))
                   : std::snprintf(buf, n, "reticle key (F191): %u -> %u (type %u)", static_cast<unsigned>(kFallbackKey), static_cast<unsigned>(key), static_cast<unsigned>(kReticleMarkerType));
    }
    return std::snprintf(buf, n, "reticle key (F191): %u -> %u (type %u)", static_cast<unsigned>(previous), static_cast<unsigned>(key), static_cast<unsigned>(kReticleMarkerType));
}

}  // namespace f191
}  // namespace nofoot
}  // namespace edvr
