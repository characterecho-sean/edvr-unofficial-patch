// The flat profile's vehicle frame, the pure half (issue 186): the pose ring the emit tap fills and the resolve side reads, and the gate that decides whether the frame is used.
// No game, no Windows: tools\flat_mono_resolve_test drives it on plain memory.
//
// WHAT IT HOLDS. The root record (index 0) of the player vehicle's cockpit rig, as the emit sees it: position (three float words) and the packed quaternion (two words), taken at the
// emit, so they are what the pool holds by construction, keyed by the stamp the emit folds into its markers (the present-frame clock, the value the prep reads as EN[276].x). The
// resolve side asks for this frame's entry and last frame's; it gets a frame only when both exist, their stamps are adjacent and they come from the same rig.
#pragma once

#include <atomic>
#include <cstdint>

namespace edvr {
namespace vframe {

struct Pose {
    uint32_t pos[3] = {};    // float bits
    uint32_t quat[2] = {};   // the pool's packed words
};
struct Entry {
    uint32_t stamp = 0;
    uint32_t generation = 0;   // which rig it was read from (vehicle_rig.cpp bumps it when the cockpit rig, its handle or its record array changes)
    Pose pose;
};
// Why a resolve had no frame (kOk: it had one).
enum class Why : uint8_t { kOk = 0, kMissing, kNotAdjacent, kChanged };
inline const char* whyName(Why w) {
    switch (w) {
        case Why::kOk: return "ok";
        case Why::kMissing: return "missing";
        case Why::kNotAdjacent: return "not-adjacent";
        default: return "changed";
    }
}

// Eight slots, a slot a stamp (stamp & 7), each a seqlock over relaxed atomics. Writers are the emit tap's job threads: a writer that finds the slot busy skips (two writes of one stamp carry
// the same root pose, and a skipped write only costs that stamp its entry). Readers are the render thread: a torn read is "missing".
class Ring {
public:
    static constexpr uint32_t kSlots = 8;
    void record(uint32_t stamp, uint32_t generation, const Pose& p) {
        Slot& s = slot_[stamp & (kSlots - 1)];
        uint32_t seq = s.seq.load(std::memory_order_relaxed);
        if (seq & 1u) return;
        if (!s.seq.compare_exchange_strong(seq, seq + 1, std::memory_order_acquire, std::memory_order_relaxed)) return;
        s.stamp.store(stamp, std::memory_order_relaxed);
        s.generation.store(generation, std::memory_order_relaxed);
        for (int i = 0; i < 3; ++i) s.w[i].store(p.pos[i], std::memory_order_relaxed);
        for (int i = 0; i < 2; ++i) s.w[3 + i].store(p.quat[i], std::memory_order_relaxed);
        s.seq.store(seq + 2, std::memory_order_release);
    }
    // The entry for exactly this stamp; false when absent, another stamp's, or torn.
    bool read(uint32_t stamp, Entry* out) const {
        const Slot& s = slot_[stamp & (kSlots - 1)];
        for (int attempt = 0; attempt < 3; ++attempt) {
            const uint32_t s1 = s.seq.load(std::memory_order_acquire);
            if (s1 & 1u) continue;
            Entry e;
            e.stamp = s.stamp.load(std::memory_order_relaxed);
            e.generation = s.generation.load(std::memory_order_relaxed);
            for (int i = 0; i < 3; ++i) e.pose.pos[i] = s.w[i].load(std::memory_order_relaxed);
            for (int i = 0; i < 2; ++i) e.pose.quat[i] = s.w[3 + i].load(std::memory_order_relaxed);
            std::atomic_thread_fence(std::memory_order_acquire);
            if (s.seq.load(std::memory_order_relaxed) != s1) continue;
            if (e.stamp != stamp || s1 == 0) return false;
            *out = e;
            return true;
        }
        return false;
    }
    // Any entry whose stamp is older than `stamp` (not just its predecessor): what separates "not adjacent" from "nothing yet".
    bool hasOlderThan(uint32_t stamp) const {
        for (uint32_t i = 0; i < kSlots; ++i) {
            const Slot& s = slot_[i];
            const uint32_t s1 = s.seq.load(std::memory_order_acquire);
            if ((s1 & 1u) || s1 == 0) continue;
            const uint32_t st = s.stamp.load(std::memory_order_relaxed);
            if (static_cast<int32_t>(st - stamp) < 0 && static_cast<int32_t>(stamp - st) < 1000000) return true;
        }
        return false;
    }

private:
    struct Slot {
        std::atomic<uint32_t> seq{0};
        std::atomic<uint32_t> stamp{0}, generation{0};
        std::atomic<uint32_t> w[5]{};
    };
    Slot slot_[kSlots];
};

struct Frame {
    Pose now, prev;
};
// This frame's pose and last frame's, or why not. `generationNow` is the rig vehicle_rig.cpp is tracking now: an entry from any other rig is a vehicle change.
inline Why select(const Ring& ring, uint32_t stamp, uint32_t generationNow, Frame* out) {
    Entry now, prev;
    if (!ring.read(stamp, &now)) return Why::kMissing;
    if (!ring.read(stamp - 1u, &prev)) return ring.hasOlderThan(stamp) ? Why::kNotAdjacent : Why::kMissing;
    if (now.generation != generationNow || prev.generation != generationNow) return Why::kChanged;
    out->now = now.pose;
    out->prev = prev.pose;
    return Why::kOk;
}

// THE RECENT RIGS. The camera target names the vehicle of the instance coming next, ahead of it being drawn: in the countdown to supercruise and through the hyperspace tunnel the rig it names
// emits nothing while the one named before it goes on emitting (flight 12:03: 3,313 resolves had no frame, and the earlier rig's root emitted on 3,299 frames in those windows), and the cockpit in those
// seconds rides the earlier one. So a few are kept, each with its own ring, and the newest with this frame's entry and last frame's answers. In a steady state that is the one the target names.
constexpr uint32_t kRecentRigs = 4;
// rings[0..count) and generations[0..count) newest first. kOk: `out` is the answer and `chosen` its index. Otherwise the most telling reason: missing only when no rig had more to say.
inline Why selectRecent(const Ring* const* rings, const uint32_t* generations, uint32_t count, uint32_t stamp, Frame* out, uint32_t* chosen) {
    Why best = Why::kMissing;
    for (uint32_t i = 0; i < count; ++i) {
        const Why w = select(*rings[i], stamp, generations[i], out);
        if (w == Why::kOk) {
            *chosen = i;
            return w;
        }
        if (best == Why::kMissing) best = w;
    }
    return best;
}

// The gate. `known` is the journal watcher's answer for the flat profile (journalFlatOnFoot's return), `onFoot` its value. The vehicle frame is used only in a vehicle the journal says
// so of, with a frame in hand.
enum class Gate : uint8_t { kOn = 0, kNoStatus, kOnFoot };
inline Gate gate(bool known, bool onFoot) { return !known ? Gate::kNoStatus : (onFoot ? Gate::kOnFoot : Gate::kOn); }
inline const char* gateName(Gate g) {
    switch (g) {
        case Gate::kOn: return "on";
        case Gate::kNoStatus: return "off(no-status)";
        default: return "off(on-foot)";
    }
}
inline bool useFrame(bool known, bool onFoot, bool haveFrame) { return gate(known, onFoot) == Gate::kOn && haveFrame; }

}  // namespace vframe
}  // namespace edvr
