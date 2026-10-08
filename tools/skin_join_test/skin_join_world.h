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
    Plan plan;
    JoinResult result;
    uint32_t decline = 0;
    std::vector<uint32_t> poseNext;
    // history: whether the certificates pass for this frame; poseAll: every job base has a pose record
    void frame(const Built& b, bool history = true, bool poseAll = true, bool offerSnapshot = true, uint32_t consumerTid = 1) {
        decline = feeder.step(offerSnapshot ? &b.snap : nullptr, consumerTid, history, uint32_t(b.jobs.size()), parity, plan);
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


} // namespace skin_join_world
