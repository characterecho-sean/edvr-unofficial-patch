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
//
// The history rules are the 2026-09-23 review's (reviews\engine-motion-
// review-2026-09-23.md, items 1 and 2): missing history is MASKED, never
// asserted as zero motion; ambiguity and failed provenance carry into the
// next frame; items are validated before the table moves.

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
// The compose's reading of a record: 1 joined, 2 masked, 3 neither.
inline uint32_t kindOf(const std::array<uint8_t, 0x150>& r) {
    const uint32_t h = ev::markerHash(blockNow(r), blockPrev(r));
    if (marker(r) == (ev::kJoined ^ h)) return 1;
    if (marker(r) == (ev::kMasked ^ h)) return 2;
    return 3;
}

// One emission: the producer's k records then the bracket, as the relay runs them.
inline void emitOnce(Fake& f, ev::Table& table, ev::Stats& s, uint32_t frame, int k, ev::Census* census = nullptr) {
    const int32_t before = Fake::get<int32_t>(f.own() + ev::kOwnerCount);
    f.producer(k);
    const int32_t after = Fake::get<int32_t>(f.own() + ev::kOwnerCount);
    ev::observe(f.rec(), f.own(), before, after, frame, &fakeLookup, table, s, census);
}
// A fresh frame for the fake, one emission at pose p, and the one record it made.
inline std::array<uint8_t, 0x150> frameOf(Fake& f, ev::Table& table, ev::Stats& s, uint32_t frame, const ev::Pose& p,
                                          uint64_t node, ev::Census* census = nullptr) {
    f.reset();
    f.setPose(p, node);
    emitOnce(f, table, s, frame, 1, census);
    return f.copier().at(0);
}

inline void run(const Harness& h) {
    auto table = std::make_unique<ev::Table>();
    ev::Stats s;
    auto f = std::make_unique<Fake>();
    g_fake = f.get();
    const ev::Pose p1 = pose(10.0f, 2.0f, -30.0f, 32767, 32767, 32767, 65534);
    const ev::Pose p2 = pose(10.4f, 0.2f, -30.6f, 32767, 32767, 33000, 65533);
    const ev::Pose p3 = pose(10.8f, -1.6f, -31.2f, 32767, 32767, 33233, 65531);
    const ev::Pose p4 = pose(11.2f, -3.0f, -31.8f, 32767, 32767, 33400, 65530);

    // Frame 100: first seen -> MASKED (no history exists), the pose the baseline.
    f->setPose(p1, 0xAAAA);
    emitOnce(*f, *table, s, 100, 2);
    auto pool = f->copier();
    h.check(pool.size() == 2, "the producer appended two LOD records");
    for (auto& r : pool) {
        h.check(blockPrev(r) == p1 && blockNow(r) == p1, "first seen: both blocks the current pose");
        h.check(kindOf(r) == 2, "first seen is MASKED, not joined with a zero motion (review item 1)");
    }
    h.check(s.firstSeen == 1 && s.itemsMasked == 2 && s.itemsJoined == 0, "first-seen counters");

    // Frame 101: the next uninterrupted frame joins, with frame 100's pose.
    f->reset();
    f->setPose(p2, 0xAAAA);
    emitOnce(*f, *table, s, 101, 1);
    pool = f->copier();
    h.check(pool.size() == 1 && blockNow(pool[0]) == p2 && blockPrev(pool[0]) == p1 && kindOf(pool[0]) == 1,
            "the frame after first sight joins: previous block holds frame 100's pose");
    h.check(s.itemsMoving == 1 && s.recordsMoving == 1, "moving counters");

    // Frame 101 again, same pose (another emission in one frame): the same previous.
    emitOnce(*f, *table, s, 101, 1);
    pool = f->copier();
    h.check(pool.size() == 2 && blockPrev(pool[1]) == p1 && kindOf(pool[1]) == 1, "a repeat in the frame reuses the frame's previous pose");
    h.check(s.repeats == 1, "repeat counted");

    // Frame 101, a DIFFERENT pose under the same tick: ambiguous -> masked.
    f->setPose(p3, 0xAAAA);
    emitOnce(*f, *table, s, 101, 1);
    pool = f->copier();
    h.check(pool.size() == 3 && blockPrev(pool[2]) == p3 && kindOf(pool[2]) == 2,
            "a pose change within one frame masks the record, never invents a previous");
    h.check(s.sameFrameChanges == 1, "same-frame change counted");

    // Frame 102: the later same-tick pose, held still. The frame-101 pose is not
    // certified, so there is NO object velocity: masked, re-baselined (review
    // item 2's regression: no p3-to-p2 vector).
    pool = {frameOf(*f, *table, s, 102, p3, 0xAAAA)};
    h.check(kindOf(pool[0]) == 2 && blockPrev(pool[0]) == p3, "after a same-tick change the next frame is masked, no false velocity");
    h.check(s.uncertifiedHistory == 1, "uncertified history counted");
    // Frame 103: joined again from the clean frame-102 baseline -- still, zero motion.
    const uint64_t movingBefore = s.itemsMoving;
    pool = {frameOf(*f, *table, s, 103, p3, 0xAAAA)};
    h.check(kindOf(pool[0]) == 1 && blockPrev(pool[0]) == p3 && s.itemsMoving == movingBefore,
            "the frame after the re-baseline joins with zero motion");

    // Frame 106: a gap of three frames -> masked; 107 joins from it.
    pool = {frameOf(*f, *table, s, 106, p1, 0xAAAA)};
    h.check(kindOf(pool[0]) == 2 && blockPrev(pool[0]) == p1 && s.gaps == 1 && s.gapAge[1] == 1,
            "a gap masks (counted, age 3 in the 3-4 bucket)");
    pool = {frameOf(*f, *table, s, 107, p2, 0xAAAA)};
    h.check(kindOf(pool[0]) == 1 && blockPrev(pool[0]) == p1, "the frame after a gap joins");

    // Frame 108: the pointer reused by another object (node changed) -> masked; 109 joins.
    pool = {frameOf(*f, *table, s, 108, p3, 0xBBBB)};
    h.check(kindOf(pool[0]) == 2 && s.identityResets == 1, "a reused record pointer masks");
    pool = {frameOf(*f, *table, s, 109, p2, 0xBBBB)};
    h.check(kindOf(pool[0]) == 1 && blockPrev(pool[0]) == p3, "the frame after a reuse joins");

    // Seven then two into one list: the call's records span two nodes.
    f->reset();
    f->setPose(p3, 0xBBBB);
    emitOnce(*f, *table, s, 110, 7);
    emitOnce(*f, *table, s, 110, 2);   // tail had 7: one goes in it, one in a new node
    pool = f->copier();
    h.check(pool.size() == 9 && f->nodes.size() == 2, "nine records over two nodes");
    for (auto& r : pool) h.check(blockPrev(r) == p2 && kindOf(r) == 1, "every record of both calls, across the node boundary, got frame 109's pose");

    // The review's provenance repro, across the following frames: A in 111,
    // then pose B with its appended item disagreeing (X) in 112, then C in 113.
    // 113 must NOT join with previous B; 114 joins from C.
    pool = {frameOf(*f, *table, s, 111, p1, 0xBBBB)};
    h.check(kindOf(pool[0]) == 1, "A joined");
    {
        f->reset();
        f->setPose(p2, 0xBBBB);
        const int32_t before = Fake::get<int32_t>(f->own() + ev::kOwnerCount);
        f->producer(1);
        const uintptr_t item = f->tail() + ev::kNodeRecords;
        float wrong = 99.0f;
        std::memcpy(reinterpret_cast<void*>(item + ev::kItemPos), &wrong, 4);
        uint32_t garbageMarker = 0;
        std::memcpy(&garbageMarker, reinterpret_cast<const void*>(item + ev::kItemMarker), 4);
        const uint64_t disagreeBefore = s.disagreements, unprovenBefore = s.callsUnproven;
        ev::observe(f->rec(), f->own(), before, Fake::get<int32_t>(f->own() + ev::kOwnerCount), 112, &fakeLookup, *table, s);
        uint32_t after = 0;
        std::memcpy(&after, reinterpret_cast<const void*>(item + ev::kItemMarker), 4);
        h.check(s.disagreements == disagreeBefore + 1 && s.callsUnproven == unprovenBefore + 1 && after == garbageMarker,
                "a disagreeing record is counted and not written");
    }
    pool = {frameOf(*f, *table, s, 113, p3, 0xBBBB)};
    h.check(kindOf(pool[0]) == 2 && blockPrev(pool[0]) == p3, "the frame after a failed call is masked, never joined with its pose (review item 2)");
    pool = {frameOf(*f, *table, s, 114, p4, 0xBBBB)};
    h.check(kindOf(pool[0]) == 1 && blockPrev(pool[0]) == p3, "the clean frame after joins");

    // A MIXED call: two items, the second disagreeing. The valid one is written
    // masked, the failed one not at all, and the next frame is masked too.
    {
        f->reset();
        f->setPose(p1, 0xBBBB);
        const int32_t before = Fake::get<int32_t>(f->own() + ev::kOwnerCount);
        f->producer(2);
        const uintptr_t second = f->tail() + ev::kNodeRecords + ev::kItemBytes;
        float wrong = -7.0f;
        std::memcpy(reinterpret_cast<void*>(second + ev::kItemPos + 4), &wrong, 4);
        uint32_t garbageMarker = 0;
        std::memcpy(&garbageMarker, reinterpret_cast<const void*>(second + ev::kItemMarker), 4);
        ev::observe(f->rec(), f->own(), before, Fake::get<int32_t>(f->own() + ev::kOwnerCount), 115, &fakeLookup, *table, s);
        pool = f->copier();
        h.check(kindOf(pool[0]) == 2 && blockPrev(pool[0]) == p1, "a mixed call writes its valid item masked");
        uint32_t after = 0;
        std::memcpy(&after, reinterpret_cast<const void*>(second + ev::kItemMarker), 4);
        h.check(after == garbageMarker, "a mixed call leaves its failed item unwritten");
    }
    pool = {frameOf(*f, *table, s, 116, p2, 0xBBBB)};
    h.check(kindOf(pool[0]) == 2, "the frame after a mixed call is masked");
    pool = {frameOf(*f, *table, s, 117, p3, 0xBBBB)};
    h.check(kindOf(pool[0]) == 1 && blockPrev(pool[0]) == p2, "then joins");

    // A located-nowhere call in a joined frame taints it: the next frame masks.
    pool = {frameOf(*f, *table, s, 118, p4, 0xBBBB)};
    h.check(kindOf(pool[0]) == 1, "joined before the locate failure");
    g_lookupRefuses = true;
    emitOnce(*f, *table, s, 118, 1);
    g_lookupRefuses = false;
    h.check(s.locateFailures == 1 && s.taints >= 1, "a refused lookup is a locate failure and taints the frame");
    pool = {frameOf(*f, *table, s, 119, p4, 0xBBBB)};
    h.check(kindOf(pool[0]) == 2, "the frame after a tainted one is masked");
    pool = {frameOf(*f, *table, s, 120, p1, 0xBBBB)};
    h.check(kindOf(pool[0]) == 1 && blockPrev(pool[0]) == p4, "then joins");

    // A rig record that cannot be read: counted, nothing written, no crash.
    const uint64_t faultsBefore = s.readFaults;
    ev::observe(0x10, f->own(), 0, 1, 121, &fakeLookup, *table, s);
    h.check(s.readFaults == faultsBefore + 1, "an unreadable rig record is a read fault");

    // Nothing appended, drained, too many: counted, no writes.
    const uint64_t callsBefore = s.calls;
    ev::observe(f->rec(), f->own(), 5, 5, 122, &fakeLookup, *table, s);
    ev::observe(f->rec(), f->own(), 9, 3, 122, &fakeLookup, *table, s);
    ev::observe(f->rec(), f->own(), 0, 8, 122, &fakeLookup, *table, s);
    h.check(s.calls == callsBefore + 3 && s.drained == 1 && s.tooMany == 1, "empty, drained and over-7 calls declined");

    // The table: a full probe window masks rather than forgets; entries two
    // frames stale are reclaimed.
    {
        auto t = std::make_unique<ev::Table>();   // about a megabyte: never on the stack
        ev::Stats ts;
        ev::Pose prev;
        for (uint64_t r = 1; r <= 40000; ++r) t->resolve(0x100000 + r * 0x2F0, 1, 7, p1, true, prev, ts);
        h.check(ts.overflow > 0 && ts.firstSeen + ts.overflow == 40000, "past capacity the table masks (counted), never guesses");
        const uint64_t firstBefore = ts.firstSeen;
        for (uint64_t r = 1; r <= 1000; ++r) t->resolve(0x900000 + r * 0x2F0, 1, 9, p1, true, prev, ts);
        h.check(ts.firstSeen == firstBefore + 1000, "entries two frames stale are reclaimed for new records");
        uint32_t joined = 0;
        for (uint64_t r = 1; r <= 1000; ++r)
            if (t->resolve(0x900000 + r * 0x2F0, 1, 10, p2, true, prev, ts) == ev::Table::Result::Joined && prev == p1) ++joined;
        h.check(joined == 1000, "reclaimed entries join on their next uninterrupted frame");
    }

    // The census, directly: a record moving while evaluated but not drawn,
    // then moving while drawn.
    {
        auto census = std::make_unique<ev::Census>();
        ev::Stats cs;
        uint64_t sampledRecord = 0;
        for (uint64_t r = 0x7000; !sampledRecord; r += 0x2F0) if (ev::Census::sampled(r)) sampledRecord = r;
        h.check(census->note(sampledRecord, 10, p1, true, cs) == ev::kNoTick, "a new census record has no earlier evaluation");
        h.check(census->note(sampledRecord, 11, p2, false, cs) == 10, "the census returns the last evaluated frame");
        census->note(sampledRecord, 11, p2, false, cs);   // a second call in the frame: still not drawn
        census->note(sampledRecord, 12, p2, true, cs);    // finalises 11: moved, not drawn
        h.check(cs.censusMovingUndrawn == 1 && cs.censusMovingDrawn == 0, "a record moving while evaluated but not drawn is counted as such");
        census->note(sampledRecord, 13, p3, true, cs);    // finalises 12: not moved
        census->note(sampledRecord, 14, p4, true, cs);    // finalises 13: moved, drawn
        h.check(cs.censusMovingDrawn == 1 && cs.censusMovingUndrawn == 1, "a record moving while drawn is counted as such");
        h.check(cs.censusFrames == 5, "one census record-frame per evaluated frame");
    }
    // ...and through the bracket: a gap after an evaluation without items is
    // told apart from a gap with no evaluation.
    {
        std::vector<std::unique_ptr<Fake>> spare;
        std::unique_ptr<Fake> g;
        while (!g) {
            auto c = std::make_unique<Fake>();
            if (ev::Census::sampled(c->rec())) g = std::move(c); else spare.push_back(std::move(c));
        }
        g_fake = g.get();
        auto census = std::make_unique<ev::Census>();
        auto t = std::make_unique<ev::Table>();
        ev::Stats gs;
        frameOf(*g, *t, gs, 200, p1, 0xCCCC, census.get());
        ev::observe(g->rec(), g->own(), 0, 0, 201, &fakeLookup, *t, gs, census.get());   // evaluated, nothing appended
        frameOf(*g, *t, gs, 203, p1, 0xCCCC, census.get());                                // gap of 3
        h.check(gs.gaps == 1 && gs.gapsEvaluatedBetween == 1 && gs.gapsUnevaluated == 0,
                "a gap after an evaluation without items is classed as evaluated-between");
        frameOf(*g, *t, gs, 207, p1, 0xCCCC, census.get());                                // gap of 4, nothing between
        h.check(gs.gaps == 2 && gs.gapsUnevaluated == 1, "a gap with no evaluation between is classed as such");
        g_fake = f.get();
    }

    // The hash is order-sensitive over both blocks (a swapped pair must not validate).
    h.check(ev::markerHash(p1, p2) != ev::markerHash(p2, p1), "marker hash distinguishes current from previous");
    g_fake = nullptr;
    std::printf("  emit: %llu calls, joined %llu, masked %llu, disagreements %llu, unproven %llu (the gates' own fixtures)\n",
                (unsigned long long)s.calls.load(), (unsigned long long)s.itemsJoined.load(),
                (unsigned long long)s.itemsMasked.load(), (unsigned long long)s.disagreements.load(),
                (unsigned long long)s.callsUnproven.load());
}

} // namespace emit_tests
