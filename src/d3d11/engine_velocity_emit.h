#pragma once
// Engine-record velocity, the EMIT half (fix.engine_motion=on, phase 1): pure
// logic with no D3D in it, so tools/engine_velocity_test drives it with a
// fake owner, dictionary and node lists laid out as the engine lays them.
//
// The engine (build 332841; docs/kinematic-motion-injection-2026-09-19.md,
// 2026-09-23 "Phase 1 built", and the decompiles it cites):
//   FUN_144312E00(rig record, owner, masks, tail) builds the record's
//   0x150-byte pool record on its stack -- byte-for-byte the t33 record --
//   copying rig+0x170/+0x178 (position) and +0x17C/+0x180 (packed
//   quaternion) into BOTH pose blocks (+0x08/+0x10 and +0x124/+0x138), then
//   for each LOD with a view mask (k <= 7) appends the whole record to the
//   tail node of the list FUN_143696FA0(owner+0x260, rig+0x250) returns and
//   increments owner+0x2A4 (0x144313191/0x144313195). Nodes hold 8 records:
//   [0] next, [1] prev (the first/last link to entry+0x28), count at +0x18,
//   records at +0x20+i*0x150. The owner is per collection and has one
//   writer, the job running the call; the copier 0x4C81BE0 later copies
//   whole records into the mapped pool. Nothing on the CPU reads record
//   bytes +0x120..+0x13F except those whole-record copies, and nothing
//   writes +0x120 at all (it is uninitialised stack in every producer).
//
// So the bracket, after the forward, looks the entry up again (a pure read on
// a hit, no lock), takes the call's k records off the tail, checks each one's
// current pose against the rig record bit for bit (the disagreement gate),
// and writes the record's PREVIOUS engine pose into its second block with a
// self-checking marker at +0x120: tag ^ markerHash(both blocks), the same
// hash the compose's ENGINE_MOTION_HLSL block (temporal_shader_source.h)
// recomputes on the GPU.
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>

namespace edvr {
namespace engine_velocity_emit {

constexpr uint32_t kJoined = 0x7FC0ED01u;
constexpr uint32_t kMasked = 0x7FC0ED02u;

constexpr uintptr_t kRecordNode = 0x18;     // the reuse discriminator the tracker keys on
constexpr uintptr_t kRecordPos = 0x170;     // f32 x3
constexpr uintptr_t kRecordQuat = 0x17C;    // u16 x4
constexpr uintptr_t kRecordKey = 0x250;     // FUN_143696FA0's key argument
constexpr uintptr_t kOwnerDictionary = 0x260;
constexpr uintptr_t kOwnerCount = 0x2A4;
constexpr uintptr_t kEntryAnchor = 0x28;    // the list's sentinel (first node at +0x28, tail at +0x30)
constexpr uintptr_t kEntryTail = 0x30;
constexpr uintptr_t kNodePrev = 0x08;
constexpr uintptr_t kNodeCount = 0x18;
constexpr uintptr_t kNodeRecords = 0x20;
constexpr uint64_t kNodeCapacity = 8;
constexpr uintptr_t kItemBytes = 0x150;
constexpr uintptr_t kItemQuat = 0x08;
constexpr uintptr_t kItemPos = 0x10;
constexpr uintptr_t kItemMarker = 0x120;
constexpr uintptr_t kItemPrevPos = 0x124;
constexpr uintptr_t kItemPrevQuat = 0x138;
constexpr int32_t kMaxAppended = 7;         // one record per LOD with a mask

// A pose in the rig record's own order: position (3 x f32 bits), then the
// packed quaternion (2 x u32 = 4 x u16).
struct Pose {
    uint32_t w[5] = {};
    bool operator==(const Pose& o) const { return std::memcmp(w, o.w, sizeof(w)) == 0; }
    bool operator!=(const Pose& o) const { return !(*this == o); }
};

// The ENGINE_MOTION_HLSL block's engineMarkerHash, word for word: the current block
// (position, quaternion) then the previous block (position, quaternion).
inline uint32_t markerHash(const Pose& now, const Pose& prev) {
    uint32_t h = 0x811C9DC5u;
    auto mix = [&](uint32_t v) { h = (h ^ v) * 0x01000193u; h ^= h >> 13; };
    for (uint32_t v : now.w) mix(v);
    for (uint32_t v : prev.w) mix(v);
    return h;
}

// Counters. Relaxed atomics: several job threads emit at once.
struct Stats {
    std::atomic<uint64_t> calls{0}, callsWithItems{0}, itemsAppended{0}, drained{0}, tooMany{0};
    std::atomic<uint64_t> readFaults{0}, locateFailures{0}, disagreements{0}, writeFaults{0};
    std::atomic<uint64_t> itemsJoined{0}, itemsMoving{0}, itemsMasked{0};
    std::atomic<uint64_t> recordsMoving{0}, firstSeen{0}, gaps{0}, identityResets{0};
    std::atomic<uint64_t> repeats{0}, sameFrameChanges{0}, overflow{0};
    void clear() {
        for (auto* c : {&calls, &callsWithItems, &itemsAppended, &drained, &tooMany, &readFaults,
                        &locateFailures, &disagreements, &writeFaults, &itemsJoined, &itemsMoving,
                        &itemsMasked, &recordsMoving, &firstSeen, &gaps, &identityResets, &repeats,
                        &sameFrameChanges, &overflow})
            c->store(0, std::memory_order_relaxed);
    }
};
inline void bump(std::atomic<uint64_t>& c, uint64_t n = 1) { c.fetch_add(n, std::memory_order_relaxed); }

// The previous-pose table: per engine record (pointer, with record+0x18 as
// the reuse discriminator, the tracker's rule), the pose its last emission
// carried and the present frame it was emitted in. Sixteen shards, each an
// open-addressed array with its own lock; an entry not emitted in the last
// two frames holds no usable history, so its slot is reclaimable -- the table
// cannot fill with dead records.
class Table {
public:
    static constexpr uint32_t kShards = 16, kSlots = 1024, kProbe = 32;
    enum class Result { Joined, Masked };

    // The previous pose for this emission (the current pose when there is
    // none: first seen, a gap, a reused pointer), or Masked when the table
    // cannot say (full, or the pose changed between two emissions in one
    // frame -- two frames' data under one clock tick, which is ambiguous).
    Result resolve(uint64_t record, uint64_t node, uint32_t frame, const Pose& pose, Pose& prev, Stats& s) {
        Shard& shard = shards_[shardOf(record)];
        std::lock_guard<std::mutex> lock(shard.mutex);
        const uint32_t start = slotOf(record);
        Entry* found = nullptr;
        Entry* reuse = nullptr;
        for (uint32_t i = 0; i < kProbe; ++i) {
            Entry& e = shard.entries[(start + i) & (kSlots - 1)];
            if (e.record == record) { found = &e; break; }
            if (e.record == 0) { if (!reuse) reuse = &e; break; }
            if (!reuse && frame - e.frame >= 2u) reuse = &e;
        }
        if (found) {
            if (found->node != node) {
                *found = Entry{record, node, frame, pose, pose};
                prev = pose;
                bump(s.identityResets);
                return Result::Joined;
            }
            const uint32_t age = frame - found->frame;
            if (age == 0) {
                if (pose != found->now) { bump(s.sameFrameChanges); prev = pose; return Result::Masked; }
                prev = found->prev;
                bump(s.repeats);
                return Result::Joined;
            }
            if (age == 1) {
                found->prev = found->now;
                found->now = pose;
                found->frame = frame;
                prev = found->prev;
                if (prev != pose) bump(s.recordsMoving);
                return Result::Joined;
            }
            *found = Entry{record, node, frame, pose, pose};
            prev = pose;
            bump(s.gaps);
            return Result::Joined;
        }
        if (!reuse) { bump(s.overflow); prev = pose; return Result::Masked; }
        *reuse = Entry{record, node, frame, pose, pose};
        prev = pose;
        bump(s.firstSeen);
        return Result::Joined;
    }
    void clear() {
        for (auto& shard : shards_) {
            std::lock_guard<std::mutex> lock(shard.mutex);
            for (auto& e : shard.entries) e = Entry{};
        }
    }
    // Entries emitted at frame or frame-1 (the live population), for the log.
    uint32_t live(uint32_t frame) {
        uint32_t n = 0;
        for (auto& shard : shards_) {
            std::lock_guard<std::mutex> lock(shard.mutex);
            for (const auto& e : shard.entries) if (e.record && frame - e.frame < 2u) ++n;
        }
        return n;
    }

private:
    struct Entry {
        uint64_t record = 0, node = 0;
        uint32_t frame = 0;
        Pose now{}, prev{};
    };
    struct Shard {
        std::mutex mutex;
        Entry entries[kSlots];
    };
    static uint64_t mix(uint64_t x) {
        x ^= x >> 33; x *= 0xff51afd7ed558ccdull; x ^= x >> 33; x *= 0xc4ceb9fe1a85ec53ull; x ^= x >> 33;
        return x;
    }
    static uint32_t shardOf(uint64_t record) { return static_cast<uint32_t>(mix(record) >> 60) & (kShards - 1); }
    static uint32_t slotOf(uint64_t record) { return static_cast<uint32_t>(mix(record)) & (kSlots - 1); }
    Shard shards_[kShards];
};

// Guarded memory access: a fault drops the item or the call, never the flight.
inline bool read(uintptr_t address, void* out, size_t bytes) noexcept {
    if (!address) return false;
    __try { std::memcpy(out, reinterpret_cast<const void*>(address), bytes); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
inline bool write(uintptr_t address, const void* in, size_t bytes) noexcept {
    if (!address) return false;
    __try { std::memcpy(reinterpret_cast<void*>(address), in, bytes); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
inline uint64_t read64(uintptr_t address, bool& ok) noexcept {
    uint64_t v = 0;
    if (!read(address, &v, 8)) ok = false;
    return v;
}

// FUN_143696FA0: (owner+0x260, rig+0x250) -> the dictionary entry.
using LookupFn = uintptr_t (__fastcall*)(uintptr_t dictionary, uintptr_t key);
inline uintptr_t guardedLookup(LookupFn lookup, uintptr_t dictionary, uintptr_t key) noexcept {
    __try { return lookup(dictionary, key); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

// The k records the call appended, oldest first: the tail node's last k, or
// all of the tail's and the previous (full) node's last k-n.
inline bool collectItems(uintptr_t entry, int32_t k, uintptr_t* items) noexcept {
    bool ok = true;
    const uintptr_t anchor = entry + kEntryAnchor;
    const uintptr_t tail = static_cast<uintptr_t>(read64(entry + kEntryTail, ok));
    if (!ok || !tail || tail == anchor) return false;
    const uint64_t n = read64(tail + kNodeCount, ok);
    if (!ok || n == 0 || n > kNodeCapacity) return false;
    if (static_cast<uint64_t>(k) <= n) {
        for (int32_t j = 0; j < k; ++j)
            items[j] = tail + kNodeRecords + static_cast<uintptr_t>(n - k + j) * kItemBytes;
        return true;
    }
    const uintptr_t previous = static_cast<uintptr_t>(read64(tail + kNodePrev, ok));
    if (!ok || !previous || previous == anchor) return false;
    if (read64(previous + kNodeCount, ok) != kNodeCapacity || !ok) return false;
    const int32_t m = k - static_cast<int32_t>(n);
    for (int32_t j = 0; j < m; ++j)
        items[j] = previous + kNodeRecords + static_cast<uintptr_t>(kNodeCapacity - m + j) * kItemBytes;
    for (uint64_t j = 0; j < n; ++j) items[m + j] = tail + kNodeRecords + static_cast<uintptr_t>(j) * kItemBytes;
    return true;
}

inline bool readRecordPose(uintptr_t record, Pose& pose, uint64_t& node) noexcept {
    bool ok = true;
    node = read64(record + kRecordNode, ok);
    return ok && read(record + kRecordPos, &pose.w[0], 12) && read(record + kRecordQuat, &pose.w[3], 8);
}
inline bool readItemPose(uintptr_t item, Pose& pose) noexcept {
    return read(item + kItemPos, &pose.w[0], 12) && read(item + kItemQuat, &pose.w[3], 8);
}

// The whole bracket. frame = the present-frame clock at the call.
inline void observe(uintptr_t record, uintptr_t owner, int32_t before, int32_t after, uint32_t frame,
                    LookupFn lookup, Table& table, Stats& s) noexcept {
    bump(s.calls);
    if (after <= before) { if (after < before) bump(s.drained); return; }
    const int32_t k = after - before;
    if (k > kMaxAppended) { bump(s.tooMany); return; }
    bump(s.callsWithItems);
    bump(s.itemsAppended, static_cast<uint64_t>(k));
    Pose pose;
    uint64_t node = 0;
    if (!readRecordPose(record, pose, node)) { bump(s.readFaults); return; }
    const uintptr_t entry = lookup ? guardedLookup(lookup, owner + kOwnerDictionary, record + kRecordKey) : 0;
    uintptr_t items[kMaxAppended] = {};
    if (!entry || !collectItems(entry, k, items)) { bump(s.locateFailures); return; }
    Pose prev;
    const Table::Result result = table.resolve(record, node, frame, pose, prev, s);
    for (int32_t j = 0; j < k; ++j) {
        Pose itemPose;
        if (!readItemPose(items[j], itemPose)) { bump(s.readFaults); continue; }
        // The disagreement gate: the engine's copy in the record must be the
        // rig record's pose bit for bit, or this is not our record (or the
        // layout moved): write nothing.
        if (itemPose != pose) { bump(s.disagreements); continue; }
        const bool joined = result == Table::Result::Joined;
        const Pose& written = joined ? prev : itemPose;   // masked: the engine's own same-frame copy
        const uint32_t marker = (joined ? kJoined : kMasked) ^ markerHash(itemPose, written);
        if (!write(items[j] + kItemPrevPos, &written.w[0], 12) || !write(items[j] + kItemPrevQuat, &written.w[3], 8) ||
            !write(items[j] + kItemMarker, &marker, 4)) {
            bump(s.writeFaults);
            continue;
        }
        if (joined) { bump(s.itemsJoined); if (written != itemPose) bump(s.itemsMoving); }
        else bump(s.itemsMasked);
    }
}

} // namespace engine_velocity_emit
} // namespace edvr
