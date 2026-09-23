#pragma once
// engine_velocity_test: the EMIT half against a fake engine.
//
// A fake rig record, owner, dictionary entry and 8-record nodes, laid out as
// build 332841 lays them (engine_velocity_emit.h has the offsets and the
// decompile lines they come from). fakeProducer() does what FUN_144312E00
// does -- one 0x150-byte record per LOD with both pose blocks copied from the
// rig record and GARBAGE at +0x120 (nothing writes it), appended to the tail
// node, owner+0x2A4 counted -- and fakeCopier() does what the copier does:
// whole records, in list order, into a fake pool. The checks read the pool.

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <random>
#include <vector>

#include "../../src/d3d11/engine_velocity_emit.h"

namespace emit_tests {
namespace ev = edvr::engine_velocity_emit;

struct Harness { void (*check)(bool, const char*) = nullptr; };

constexpr size_t kRecordBytes = 0x2F0, kOwnerBytes = 0x2B0, kEntryBytes = 0x38;
constexpr size_t kNodeBytes = 0xAA0 + 8 * 8;

struct Node { alignas(16) uint8_t bytes[kNodeBytes]; };

struct Fake {
    alignas(16) uint8_t record[kRecordBytes] = {};
    alignas(16) uint8_t owner[kOwnerBytes] = {};
    alignas(16) uint8_t entry[kEntryBytes] = {};
    std::vector<std::unique_ptr<Node>> nodes;
    std::mt19937 garbage{12345};

    uintptr_t rec() { return reinterpret_cast<uintptr_t>(record); }
    uintptr_t own() { return reinterpret_cast<uintptr_t>(owner); }
    uintptr_t ent() { return reinterpret_cast<uintptr_t>(entry); }
    uintptr_t anchor() { return ent() + ev::kEntryAnchor; }
    template <class T> static T get(uintptr_t a) { T v; std::memcpy(&v, reinterpret_cast<const void*>(a), sizeof(T)); return v; }
    template <class T> static void put(uintptr_t a, T v) { std::memcpy(reinterpret_cast<void*>(a), &v, sizeof(T)); }

    Fake() { reset(); }
    // A fresh frame's lists: the engine frees the entries each frame.
    void reset() {
        nodes.clear();
        put<uint64_t>(anchor(), anchor());          // empty list: the anchor points at itself
        put<uint64_t>(ent() + ev::kEntryTail, anchor());
        put<int32_t>(own() + ev::kOwnerCount, 0);
    }
    void setPose(const ev::Pose& p, uint64_t node) {
        std::memcpy(record + ev::kRecordPos, &p.w[0], 12);
        std::memcpy(record + ev::kRecordQuat, &p.w[3], 8);
        put<uint64_t>(rec() + ev::kRecordNode, node);
    }
    uintptr_t tail() { return get<uint64_t>(ent() + ev::kEntryTail); }
    // FUN_144312E00's append, k times.
    void producer(int k) {
        alignas(16) uint8_t item[0x150];
        for (auto& b : item) b = static_cast<uint8_t>(garbage());
        std::memset(item, 0, 0x120);                  // initializer: base 0, material zero...
        std::memcpy(item + ev::kItemQuat, record + ev::kRecordQuat, 8);
        std::memcpy(item + ev::kItemPos, record + ev::kRecordPos, 12);
        std::memcpy(item + ev::kItemPrevPos, record + ev::kRecordPos, 12);   // the same-frame copy
        std::memcpy(item + ev::kItemPrevQuat, record + ev::kRecordQuat, 8);
        // +0x120..+0x123: never written by the engine -- left as garbage.
        for (int i = 0; i < k; ++i) {
            uintptr_t t = tail();
            if (t == anchor() || get<uint64_t>(t + ev::kNodeCount) == ev::kNodeCapacity) {
                nodes.push_back(std::make_unique<Node>());
                std::memset(nodes.back()->bytes, 0, kNodeBytes);
                const uintptr_t n = reinterpret_cast<uintptr_t>(nodes.back()->bytes);
                put<uint64_t>(n, anchor());                              // [0] next: the last links to the anchor
                put<uint64_t>(n + ev::kNodePrev, t);                     // [1] prev
                if (t != anchor()) put<uint64_t>(t, n); else put<uint64_t>(anchor(), n);
                put<uint64_t>(ent() + ev::kEntryTail, n);
                t = n;
            }
            const uint64_t c = get<uint64_t>(t + ev::kNodeCount);
            std::memcpy(reinterpret_cast<void*>(t + ev::kNodeRecords + c * ev::kItemBytes), item, 0x150);
            put<uint64_t>(t + ev::kNodeCount, c + 1);
            put<int32_t>(own() + ev::kOwnerCount, get<int32_t>(own() + ev::kOwnerCount) + 1);
        }
    }
    // The copier: whole records, list order.
    std::vector<std::array<uint8_t, 0x150>> copier() {
        std::vector<std::array<uint8_t, 0x150>> pool;
        for (uintptr_t n = get<uint64_t>(anchor()); n && n != anchor(); n = get<uint64_t>(n)) {
            const uint64_t c = get<uint64_t>(n + ev::kNodeCount);
            for (uint64_t i = 0; i < c; ++i) {
                std::array<uint8_t, 0x150> r;
                std::memcpy(r.data(), reinterpret_cast<const void*>(n + ev::kNodeRecords + i * ev::kItemBytes), 0x150);
                pool.push_back(r);
            }
        }
        return pool;
    }
};

// The fake dictionary: FUN_143696FA0's contract -- (owner+0x260, rig+0x250) ->
// the entry. One fake engine at a time.
Fake* g_fake = nullptr;
bool g_lookupRefuses = false;
uintptr_t __fastcall fakeLookup(uintptr_t dictionary, uintptr_t key) {
    if (!g_fake || g_lookupRefuses) return 0;
    if (dictionary != g_fake->own() + ev::kOwnerDictionary || key != g_fake->rec() + ev::kRecordKey) return 0;
    return g_fake->ent();
}

inline ev::Pose pose(float x, float y, float z, uint16_t a, uint16_t b, uint16_t c, uint16_t d) {
    ev::Pose p;
    std::memcpy(&p.w[0], &x, 4); std::memcpy(&p.w[1], &y, 4); std::memcpy(&p.w[2], &z, 4);
    p.w[3] = uint32_t(a) | (uint32_t(b) << 16);
    p.w[4] = uint32_t(c) | (uint32_t(d) << 16);
    return p;
}
inline ev::Pose blockNow(const std::array<uint8_t, 0x150>& r) {
    ev::Pose p; std::memcpy(&p.w[0], r.data() + ev::kItemPos, 12); std::memcpy(&p.w[3], r.data() + ev::kItemQuat, 8); return p;
}
inline ev::Pose blockPrev(const std::array<uint8_t, 0x150>& r) {
    ev::Pose p; std::memcpy(&p.w[0], r.data() + ev::kItemPrevPos, 12); std::memcpy(&p.w[3], r.data() + ev::kItemPrevQuat, 8); return p;
}
inline uint32_t marker(const std::array<uint8_t, 0x150>& r) { uint32_t m; std::memcpy(&m, r.data() + ev::kItemMarker, 4); return m; }

// One emission: the producer's k records then the bracket, as the relay runs them.
inline void emitOnce(Fake& f, ev::Table& table, ev::Stats& s, uint32_t frame, int k) {
    const int32_t before = Fake::get<int32_t>(f.own() + ev::kOwnerCount);
    f.producer(k);
    const int32_t after = Fake::get<int32_t>(f.own() + ev::kOwnerCount);
    ev::observe(f.rec(), f.own(), before, after, frame, &fakeLookup, table, s);
}

inline void run(const Harness& h) {
    auto table = std::make_unique<ev::Table>();
    ev::Stats s;
    auto f = std::make_unique<Fake>();
    g_fake = f.get();
    const ev::Pose p1 = pose(10.0f, 2.0f, -30.0f, 32767, 32767, 32767, 65534);
    const ev::Pose p2 = pose(10.4f, 0.2f, -30.6f, 32767, 32767, 33000, 65533);
    const ev::Pose p3 = pose(10.8f, -1.6f, -31.2f, 32767, 32767, 33233, 65531);

    // Frame 100: first seen -> the current pose, joined.
    f->setPose(p1, 0xAAAA);
    emitOnce(*f, *table, s, 100, 2);
    auto pool = f->copier();
    h.check(pool.size() == 2, "the producer appended two LOD records");
    for (auto& r : pool) {
        h.check(blockPrev(r) == p1 && blockNow(r) == p1, "first seen: the previous block is the current pose");
        h.check(marker(r) == (ev::kJoined ^ ev::markerHash(p1, p1)), "first seen: joined marker over both blocks");
    }
    h.check(s.firstSeen == 1 && s.itemsJoined == 2 && s.itemsMoving == 0, "first-seen counters");

    // Frame 101: moved -> last frame's pose in the second block.
    f->reset();
    f->setPose(p2, 0xAAAA);
    emitOnce(*f, *table, s, 101, 1);
    pool = f->copier();
    h.check(pool.size() == 1 && blockNow(pool[0]) == p2 && blockPrev(pool[0]) == p1, "moved: previous block holds frame 100's pose");
    h.check(marker(pool[0]) == (ev::kJoined ^ ev::markerHash(p2, p1)), "moved: marker hashes the current and previous blocks");
    h.check(s.itemsMoving == 1 && s.recordsMoving == 1, "moving counters");

    // Frame 101 again, same pose (another emission in one frame): the same previous.
    emitOnce(*f, *table, s, 101, 1);
    pool = f->copier();
    h.check(pool.size() == 2 && blockPrev(pool[1]) == p1, "a repeat in the frame reuses the frame's previous pose");
    h.check(s.repeats == 1, "repeat counted");

    // Frame 101, a DIFFERENT pose under the same tick: ambiguous -> masked.
    f->setPose(p3, 0xAAAA);
    emitOnce(*f, *table, s, 101, 1);
    pool = f->copier();
    h.check(pool.size() == 3 && blockPrev(pool[2]) == p3 && marker(pool[2]) == (ev::kMasked ^ ev::markerHash(p3, p3)),
            "a pose change within one frame masks the record, never invents a previous");
    h.check(s.sameFrameChanges == 1 && s.itemsMasked == 1, "same-frame change counted");

    // Frame 102 from the frame-101 entry (p2): moved to p3.
    f->reset();
    emitOnce(*f, *table, s, 102, 1);
    pool = f->copier();
    h.check(blockPrev(pool[0]) == p2, "frame 102 reads frame 101's first pose");

    // Frame 105: a gap -> no previous.
    f->reset();
    f->setPose(p1, 0xAAAA);
    emitOnce(*f, *table, s, 105, 1);
    pool = f->copier();
    h.check(blockPrev(pool[0]) == p1 && s.gaps == 1, "a gap gives the current pose, counted");

    // Frame 106: the pointer reused by another object (node changed) -> reset.
    f->reset();
    f->setPose(p2, 0xBBBB);
    emitOnce(*f, *table, s, 106, 1);
    pool = f->copier();
    h.check(blockPrev(pool[0]) == p2 && s.identityResets == 1, "a reused record pointer starts over");

    // Seven then two into one list: the call's records span two nodes.
    f->reset();
    f->setPose(p3, 0xBBBB);
    emitOnce(*f, *table, s, 107, 7);
    emitOnce(*f, *table, s, 107, 2);   // tail had 7: one goes in it, one in a new node
    pool = f->copier();
    h.check(pool.size() == 9 && f->nodes.size() == 2, "nine records over two nodes");
    for (auto& r : pool) h.check(blockPrev(r) == p2, "every record of both calls, across the node boundary, got frame 106's pose");

    // The disagreement gate: an appended record whose pose is not the rig
    // record's is left alone.
    f->reset();
    f->setPose(p1, 0xBBBB);
    {
        const int32_t before = Fake::get<int32_t>(f->own() + ev::kOwnerCount);
        f->producer(1);
        const uintptr_t item = f->tail() + ev::kNodeRecords;
        float wrong = 99.0f;
        std::memcpy(reinterpret_cast<void*>(item + ev::kItemPos), &wrong, 4);
        uint32_t garbageMarker = 0;
        std::memcpy(&garbageMarker, reinterpret_cast<const void*>(item + ev::kItemMarker), 4);
        const uint64_t disagreeBefore = s.disagreements;
        ev::observe(f->rec(), f->own(), before, Fake::get<int32_t>(f->own() + ev::kOwnerCount), 108, &fakeLookup, *table, s);
        uint32_t after = 0;
        std::memcpy(&after, reinterpret_cast<const void*>(item + ev::kItemMarker), 4);
        h.check(s.disagreements == disagreeBefore + 1 && after == garbageMarker, "a disagreeing record is counted and not written");
    }

    // Nothing appended, drained, too many, lookup refused: counted, no writes.
    const uint64_t callsBefore = s.calls;
    ev::observe(f->rec(), f->own(), 5, 5, 109, &fakeLookup, *table, s);
    ev::observe(f->rec(), f->own(), 9, 3, 109, &fakeLookup, *table, s);
    ev::observe(f->rec(), f->own(), 0, 8, 109, &fakeLookup, *table, s);
    h.check(s.calls == callsBefore + 3 && s.drained == 1 && s.tooMany == 1, "empty, drained and over-7 calls declined");
    f->reset();
    g_lookupRefuses = true;
    emitOnce(*f, *table, s, 110, 1);
    g_lookupRefuses = false;
    h.check(s.locateFailures == 1, "a refused lookup is a locate failure, nothing written");

    // The table: a full probe window masks rather than forgets; entries two
    // frames stale are reclaimed.
    {
        auto t = std::make_unique<ev::Table>();   // about a megabyte: never on the stack
        ev::Stats ts;
        ev::Pose prev;
        uint32_t masked = 0;
        for (uint64_t r = 1; r <= 40000; ++r)
            if (t->resolve(0x100000 + r * 0x2F0, 1, 7, p1, prev, ts) == ev::Table::Result::Masked) ++masked;
        h.check(masked > 0 && ts.overflow == masked, "past capacity the table masks (counted), never guesses");
        uint32_t reclaimed = 0;
        for (uint64_t r = 1; r <= 1000; ++r)
            if (t->resolve(0x900000 + r * 0x2F0, 1, 9, p1, prev, ts) == ev::Table::Result::Joined) ++reclaimed;
        h.check(reclaimed == 1000, "entries two frames stale are reclaimed for new records");
    }

    // The hash is order-sensitive over both blocks (a swapped pair must not validate).
    h.check(ev::markerHash(p1, p2) != ev::markerHash(p2, p1), "marker hash distinguishes current from previous");
    g_fake = nullptr;
    std::printf("  emit: %llu calls, joined %llu, masked %llu, disagreements %llu (the gate's own fixture)\n",
                (unsigned long long)s.calls.load(), (unsigned long long)s.itemsJoined.load(),
                (unsigned long long)s.itemsMasked.load(), (unsigned long long)s.disagreements.load());
}

} // namespace emit_tests
