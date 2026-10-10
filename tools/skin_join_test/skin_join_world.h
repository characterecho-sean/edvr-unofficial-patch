#pragma once
// The worlds the skin join rigs run (skin_join_test on the CPU, skin_join_gpu_test on WARP): entities with jobs, the hook's snapshot and
// the dispatch's job table they imply, and a driver that feeds frames through JoinFeeder and cpuJoin.
#include <algorithm>
#include <vector>
#include "skin_join.h"

namespace skin_join_world {
using namespace edvr::skinjoin;

struct Ent {
    uint64_t key = 0, vtable = 0xA11CE, mesh = 0;
    std::vector<std::pair<uint32_t, uint32_t>> jobs;   // (bind, count): the first is the entity's primary job
};
using World = std::vector<Ent>;

struct Built {
    Snapshot snap;
    std::vector<JobRow> jobs;
};
// Bases are assigned as the game does: running sums in list order. `reverseTable` lays the jobs out in a different table order.
inline Built build(const World& w, uint64_t seq, bool reverseTable = false, uint32_t tid = 1) {
    Built b;
    b.snap.seq = seq;
    b.snap.node = 0x1000;
    b.snap.tid = tid;
    uint32_t base = 1;   // row 0 is never a base: the shaders read word 0 == 0 as "not skinned"
    uint32_t n = 0;
    for (const Ent& e : w) {
        Entry& en = b.snap.e[n++];
        en.key = e.key;
        en.vtable = e.vtable;
        en.mesh = e.mesh ? e.mesh : e.key + 0x100;
        en.dst = base;
        en.count = e.jobs.front().second;
        for (const auto& j : e.jobs) {
            b.jobs.push_back(JobRow{0, base, j.first, j.second});
            base += j.second;
        }
    }
    b.snap.n = n;
    b.snap.end = base;
    if (reverseTable) std::reverse(b.jobs.begin(), b.jobs.end());
    return b;
}

inline World steady() {
    World w;
    w.push_back(Ent{101, 0xA11CE, 0, {{1000, 40}, {1001, 12}, {1002, 6}}});
    w.push_back(Ent{102, 0xA11CE, 0, {{2000, 50}, {2001, 8}}});
    w.push_back(Ent{103, 0xB0B, 0, {{3000, 30}}});
    return w;
}

// A driver: frames in, joins out.
struct Run {
    JoinFeeder feeder;
    std::vector<JobRow> prevJobs;
    std::vector<DstInfo> prevInfo = std::vector<DstInfo>(kMaxRows);
    std::vector<uint32_t> prevPose = std::vector<uint32_t>(kMaxRows, 0);
    uint32_t prevHookOk = 0;
    uint32_t parity = 0;
    uint32_t prevRows = kMaxRows;   // the previous palette buffer's capacity the plan carries (a test shrinks it)
    Plan plan;
    JoinResult result;
    uint32_t decline = 0;
    std::vector<uint32_t> poseNext;
    // history: whether the certificates pass for this frame; poseAll: every job base has a pose record
    void frame(const Built& b, bool history = true, bool poseAll = true, bool offerSnapshot = true, uint32_t consumerTid = 1) {
        decline = feeder.step(offerSnapshot ? &b.snap : nullptr, consumerTid, history, uint32_t(b.jobs.size()), parity, plan);
        plan.prevRows = prevRows;
        result = cpuJoin(plan, b.jobs, prevJobs, prevInfo, prevPose, prevHookOk);
        prevHookOk = result.prevHookOk;
        prevJobs = b.jobs;
        prevInfo = result.dstInfo;
        std::fill(prevPose.begin(), prevPose.end(), 0u);
        if (poseAll) for (const JobRow& j : b.jobs) prevPose[j.dst] = j.dst;
        parity ^= 1u;
    }
    uint32_t joined(uint32_t dst) const { return result.join[dst]; }
};

inline uint32_t baseOf(const Built& b, uint32_t bind) {
    for (const JobRow& j : b.jobs) if (j.bind == bind) return j.dst;
    return 0;
}

// ---- the pose table's worlds (skin_join.h, "the pose table's CPU reference") --------------------------------------------------
// A pool record as 84 words (a base and a pose that `salt` tells apart: words 1 and 4 move with it; word 7 is not compared).
inline std::vector<uint32_t> mkRecord(uint32_t base, uint32_t salt, uint32_t word7 = 0xFFFFFFFFu) {
    std::vector<uint32_t> r(84, 0);
    r[0] = base; r[1] = 0x3F800000u + salt; r[2] = 11; r[3] = 13; r[4] = 17 + salt; r[5] = 19; r[6] = 23; r[7] = word7 == 0xFFFFFFFFu ? 999 + salt : word7;
    return r;
}
// A pool, the instance stream and the draws' windows into it, built the way a frame is: the stream also holds stale entries no draw reads.
struct PoseWorld {
    std::vector<uint32_t> records;       // 84 words each
    PoseRefs refs;
    uint32_t entryBase = 100;            // the stream's span starts at entry 100 (the copy's first entry)
    uint32_t record(uint32_t base, uint32_t salt) {
        const auto r = mkRecord(base, salt);
        records.insert(records.end(), r.begin(), r.end());
        return uint32_t(records.size() / 84 - 1);
    }
    uint32_t entry(uint32_t rec) {       // one stream entry naming `rec`; returns its absolute entry index
        refs.stream.push_back(rec);
        refs.stream.push_back(0);
        return entryBase + uint32_t(refs.stream.size() / 2 - 1);
    }
    void draw(uint32_t rec) {            // a draw of one instance reading `rec`
        const uint32_t e = entry(rec);
        refs.ranges.push_back(e);
        refs.ranges.push_back(1);
    }
    void drawMany(const std::vector<uint32_t>& recs) {   // a draw of several instances: consecutive entries
        const uint32_t first = entry(recs[0]);
        for (size_t i = 1; i < recs.size(); ++i) entry(recs[i]);
        refs.ranges.push_back(first);
        refs.ranges.push_back(uint32_t(recs.size()));
    }
    void finish(bool complete = true) { refs.complete = complete; refs.first = entryBase; }
    std::vector<PoseWords> words() const {
        std::vector<PoseWords> pool(records.size() / 84);
        for (size_t i = 0; i < pool.size(); ++i) std::memcpy(pool[i].w, &records[i * 84], 32);
        return pool;
    }
};
// Bases 21..29, each a scenario of the rule (the numbers in the cases that read this are these):
//   21 the live record first, a stale second record after it (the F12 flight's shape)   22 the stale record first, the live one after it
//   23 two records both read by draws, disagreeing                                        24 two records disagreeing, none read
//   25 one record, not read                                                              26 one record, read
//   27 a read record and an unread one of the same pose                                   28 one live record and two different stale ones
//   29 the live record read by two draws (both eyes) and one stale one
// The stream also names stale records (entries no draw's window covers).
inline PoseWorld frameWorld() {
    PoseWorld w;
    const uint32_t a = w.record(21, 0), b = w.record(21, 1);
    const uint32_t c = w.record(22, 1), d = w.record(22, 0);
    const uint32_t e = w.record(23, 0), f = w.record(23, 1);
    const uint32_t g = w.record(24, 0), h = w.record(24, 1);
    const uint32_t i = w.record(25, 0);
    const uint32_t j = w.record(26, 0);
    const uint32_t k = w.record(27, 0), l = w.record(27, 0);
    const uint32_t m = w.record(28, 0), n = w.record(28, 1), o = w.record(28, 2);
    const uint32_t pp = w.record(29, 0), q = w.record(29, 1);
    w.entry(b); w.entry(c); w.entry(h); w.entry(n); w.entry(o); w.entry(q); w.entry(l);
    w.draw(a); w.draw(d); w.draw(e); w.draw(f); w.draw(j); w.draw(k);
    w.drawMany({m, pp, pp});
    w.draw(pp);
    w.finish(true);
    (void)g; (void)i;
    return w;
}


} // namespace skin_join_world
