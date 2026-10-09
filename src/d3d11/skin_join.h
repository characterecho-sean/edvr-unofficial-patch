#pragma once

// F2: which skinned entity is which, from one frame to the next, exactly.
//
// The previous frame's bone palette is the game's own previous buffer, bit for bit (the skin ledger, run 154827). What
// it cannot say is whose rows are whose: a palette row belongs to a "base" (the first row of a job in the chain
// dispatch's t0 table), and nothing in D3D names the entity behind a base. Two sources are used, in order:
//
//   1. THE HOOK (skin_entity_hook.cpp): a read-only observer of the game's job assembly. After the game has assigned
//      every entry its base it reads the entry list: per entry its address (the key), vtable, mesh data and base. An
//      entity owns the contiguous rows from its base to the next entry's base; its children (attached meshes) are the
//      later jobs inside that range. Entity continuity across two frames is the key; the layout is then checked row by
//      row (same offset inside the entity, same bind, same bone count).
//   2. THE PREFIX JOIN: with no hook, or on any frame the hook disagrees with the dispatch's own table, the job table is
//      compared with last frame's. Jobs 0..k-1 are the same entities in the same slots while their (bind, count) tuples
//      are equal; job k and everything after it have no history for that frame.
//
// Both run on the GPU in one pass (JoinCS, the HLSL in temporal_shader_source.h, which this file's cpuJoin() mirrors
// statement for statement; skin_join_gpu_test holds the two together). The CPU side here is pure: the snapshot types,
// the continuity map, the plan the GPU reads, the history certificates, the counters and their log line.
//
// RESIDUALS, stated once. Hook source: an entry destroyed and a new one created at the SAME address with the same vtable,
// mesh data and bone count between two consecutive snapshots (one frame) is taken for the same entity. Prefix source:
// an entity removed and one with the same (bind, count) inserted at the same list position in one frame. The first
// needs the game's allocator to hand back the freed block within a frame; neither was seen in the logs. A join that fails
// for any reason gives no history, never a guess.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace edvr {
namespace skinjoin {

constexpr uint64_t kChainHash = 0x6FE04AF836BB1DBAull;   // cs_6FE04AF836BB1DBA, APPLY_BIND_POSE_TRANSFORMS_CS: one group per job (skin_ledger.h's kChainHash)
constexpr uint32_t kMaxEntries = 1024;     // entries one snapshot can hold
constexpr uint32_t kMaxRows = 65536;       // palette rows the tables cover (the ledger's kept rows)
constexpr uint32_t kMaxJobs = 8192;        // dispatch groups the join reads
constexpr uint32_t kNone = 0xFFFFFFFFu;
constexpr uint32_t kMaxBonesPerJob = 4096;

struct JobRow {                            // the game's t0 row: structured, stride 16
    uint32_t src, dst, bind, count;
};

struct Entry {                             // one list entry as the hook read it (32 bytes)
    uint64_t key;                          // the entry's address
    uint64_t vtable;                       // its first qword
    uint64_t mesh;                         // its mesh data pointer (+0x38)
    uint32_t dst;                          // its base (+0xA8), assigned by the game's own assembly
    uint32_t count;                        // its bone count (the first ushort of the mesh data)
};

enum SnapshotFlags : uint32_t {
    kSnapFault = 1,          // a read faulted during the walk
    kSnapOverflow = 2,       // more entries than a snapshot holds
    kSnapImplausible = 4,    // a value the walk could not believe (checked by checkSnapshot)
    kSnapNodeChanged = 8,    // the node pointer differs from the one the hook first saw
};

struct Snapshot {
    uint64_t seq = 0;                      // 1, 2, 3 ... per call of the game's function
    uint64_t node = 0;                     // the processor node (rcx)
    uint32_t tid = 0;                      // the calling thread
    uint32_t n = 0;                        // entries
    uint32_t end = 0;                      // the node's running base after the last entry (+0xC4)
    uint32_t flags = 0;
    Entry e[kMaxEntries];
};

// Does the list look like what the game's assembly makes? Each entity's primary job inside its own range (which also forces the bases to
// increase: a base at or below its predecessor's leaves that primary job no room), the last range ending at `end`.
inline bool checkSnapshot(const Snapshot& s, const char** why = nullptr) {
    const char* w = "";
    bool ok = true;
    if (s.flags & (kSnapFault | kSnapOverflow | kSnapNodeChanged)) { w = "walk flags"; ok = false; }
    else if (s.n == 0 || s.n > kMaxEntries) { w = "entry count"; ok = false; }
    else if (s.end == 0 || s.end > kMaxRows) { w = "end row"; ok = false; }
    else {
        for (uint32_t i = 0; i < s.n && ok; ++i) {
            const Entry& e = s.e[i];
            const uint32_t next = i + 1 < s.n ? s.e[i + 1].dst : s.end;
            if (!e.key || !e.mesh || e.count == 0 || e.count > kMaxBonesPerJob) { w = "entry fields"; ok = false; }
            else if (e.dst + e.count > next) { w = "range"; ok = false; }
        }
        if (ok && s.e[0].dst > s.end) { w = "first base"; ok = false; }
    }
    if (why) *why = w;
    return ok;
}

// ---- the plan the GPU reads ------------------------------------------------------------------------------------------
enum PlanFlags : uint32_t {
    kPlanHistory = 1,        // the previous frame's palette and pose table are certified: joins may be written
    kPlanHook = 2,           // the hook's list is usable and consecutive with the previous frame's: try the hook join
};
// Word layout of the buffer JoinCS reads (ByteAddressBuffer).
constexpr uint32_t kPlanHeaderWords = 8;
constexpr uint32_t kPlanRsAt = kPlanHeaderWords;                         // rs[0..m]
constexpr uint32_t kPlanCountAt = kPlanRsAt + kMaxEntries + 1;           // count[0..m-1]
constexpr uint32_t kPlanPrevIdxAt = kPlanCountAt + kMaxEntries;          // prevIdx[0..m-1]
constexpr uint32_t kPlanPrevRsAt = kPlanPrevIdxAt + kMaxEntries;         // prevRs[0..prevM]
constexpr uint32_t kPlanWords = kPlanPrevRsAt + kMaxEntries + 1;
// header: [0] flags [1] m [2] prevM [3] end [4] prevEnd [5] jobs [6] prevJobs [7] frame parity

struct Plan {
    uint32_t flags = 0;
    uint32_t m = 0, prevM = 0, end = 0, prevEnd = 0;
    uint32_t jobs = 0, prevJobs = 0, parity = 0;
    uint32_t rs[kMaxEntries + 1]{};
    uint32_t count[kMaxEntries]{};
    uint32_t prevIdx[kMaxEntries]{};
    uint32_t prevRs[kMaxEntries + 1]{};
    void words(std::vector<uint32_t>& out) const {
        out.assign(kPlanWords, 0);
        out[0] = flags; out[1] = m; out[2] = prevM; out[3] = end; out[4] = prevEnd;
        out[5] = jobs; out[6] = prevJobs; out[7] = parity;
        std::memcpy(&out[kPlanRsAt], rs, sizeof(rs));
        std::memcpy(&out[kPlanCountAt], count, sizeof(count));
        std::memcpy(&out[kPlanPrevIdxAt], prevIdx, sizeof(prevIdx));
        std::memcpy(&out[kPlanPrevRsAt], prevRs, sizeof(prevRs));
    }
};

// The reasons the CPU side declines the hook for a frame (the GPU may still decline it on its own check).
enum HookDecline : uint32_t {
    kDeclineNone = 0,        // hook join offered
    kDeclineNoHistory,       // the history certificates failed: no join at all this frame
    kDeclineNoSnapshot,      // the hook has produced nothing yet (or stood down)
    kDeclineStale,           // no new call since the last dispatch
    kDeclineGap,             // more than one call since the last dispatch
    kDeclineUnusable,        // the list failed checkSnapshot
    kDeclineNoPrevious,      // no usable previous list to compare with
    kDeclineNodeChanged,     // a different processor node than last frame's
    kDeclineCount
};
inline const char* declineName(uint32_t d) {
    static const char* names[kDeclineCount] = {"offered", "no history", "no snapshot", "stale", "gap", "unusable", "no previous", "node changed"};
    return d < kDeclineCount ? names[d] : "?";
}

class JoinFeeder {
public:
    struct Counters {
        uint64_t steps = 0, offered = 0, sameThread = 0, otherThread = 0;
        uint64_t decline[kDeclineCount]{};
    };

    // One chain dispatch. `latest` is the newest snapshot (or null); `history` the certificates' verdict for this frame.
    // Fills `plan` (always) and returns why the hook was not offered (kDeclineNone: offered).
    uint32_t step(const Snapshot* latest, uint32_t consumerTid, bool history, uint32_t jobs, uint32_t parity, Plan& plan) {
        ++c_.steps;
        plan = Plan{};
        plan.jobs = jobs;
        plan.parity = parity;
        plan.prevJobs = lastJobs_;
        lastJobs_ = jobs;
        if (history) plan.flags |= kPlanHistory;
        uint32_t decline = kDeclineNone;
        if (!history) decline = kDeclineNoHistory;
        else if (!latest) decline = kDeclineNoSnapshot;
        else if (latest->seq == lastSeq_ && haveLast_) decline = kDeclineStale;
        else if (!usable(*latest)) decline = kDeclineUnusable;
        else if (!haveLast_) decline = kDeclineNoPrevious;
        else if (latest->seq != lastSeq_ + 1) decline = kDeclineGap;
        else if (latest->node != last_.node) decline = kDeclineNodeChanged;
        if (latest && latest->seq != lastSeq_ ) {
            if (latest->tid == consumerTid) ++c_.sameThread; else ++c_.otherThread;
        }
        if (decline == kDeclineNone) {
            plan.flags |= kPlanHook;
            plan.m = latest->n;
            plan.prevM = last_.n;
            plan.end = latest->end;
            plan.prevEnd = last_.end;
            std::unordered_map<uint64_t, uint32_t> previous;
            previous.reserve(last_.n * 2);
            std::vector<uint8_t> duplicated(last_.n, 0);
            for (uint32_t i = 0; i < last_.n; ++i) {
                const auto at = previous.emplace(last_.e[i].key, i);
                if (!at.second) duplicated[at.first->second] = 1;
            }
            std::unordered_map<uint64_t, uint32_t> listed;
            listed.reserve(latest->n * 2);
            for (uint32_t i = 0; i < latest->n; ++i) ++listed[latest->e[i].key];
            for (uint32_t i = 0; i < latest->n; ++i) {
                const Entry& e = latest->e[i];
                plan.rs[i] = e.dst;
                plan.count[i] = e.count;
                plan.prevIdx[i] = kNone;
                const bool twice = listed[e.key] > 1;
                const auto p = previous.find(e.key);
                if (twice || p == previous.end() || duplicated[p->second]) continue;
                const Entry& q = last_.e[p->second];
                if (q.vtable == e.vtable && q.mesh == e.mesh && q.count == e.count) plan.prevIdx[i] = p->second;
            }
            plan.rs[latest->n] = latest->end;
            for (uint32_t i = 0; i < last_.n; ++i) plan.prevRs[i] = last_.e[i].dst;
            plan.prevRs[last_.n] = last_.end;
            ++c_.offered;
        } else {
            ++c_.decline[decline];
        }
        // The next frame's "previous": this list when it is usable, nothing otherwise. A stale call changes nothing.
        if (latest && !(latest->seq == lastSeq_ && haveLast_)) {
            if (usable(*latest)) {
                last_ = *latest;
                haveLast_ = true;
            } else {
                haveLast_ = false;
            }
            lastSeq_ = latest->seq;
        }
        return decline;
    }
    void reset() { haveLast_ = false; lastSeq_ = 0; lastJobs_ = 0; c_ = Counters{}; }
    const Counters& counters() const { return c_; }

private:
    static bool usable(const Snapshot& s) { return checkSnapshot(s); }
    Snapshot last_;
    bool haveLast_ = false;
    uint64_t lastSeq_ = 0;
    uint32_t lastJobs_ = 0;
    Counters c_;
};

// ---- the history certificates ----------------------------------------------------------------------------------------
// The previous palette is valid for this frame only if the chain ran in the frame before this one, the game swapped its
// two buffers, and the buffer holding last frame's rows is large enough to hold the rows now in use.
enum HistoryVerdict : uint32_t {
    kHistoryOk = 0,
    kHistoryFirst,           // no earlier chain dispatch seen
    kHistoryGap,             // the earlier one was not in the previous frame
    kHistorySame,            // the same buffer twice: the game did not swap
    kHistoryShrunk,          // the previous buffer cannot hold the rows in use
    kHistoryPose,            // the pose table for the previous frame was not built
    kHistoryCount
};
inline const char* historyName(uint32_t v) {
    static const char* names[kHistoryCount] = {"ok", "first", "gap", "same buffer", "shrunk", "no pose"};
    return v < kHistoryCount ? names[v] : "?";
}

class PaletteHistory {
public:
    struct Counters {
        uint64_t frames = 0;
        uint64_t verdict[kHistoryCount]{};
    };
    // `frame` increases by one per presented frame; `bufferId` identifies the buffer the chain wrote (u0); `byteWidth` is
    // that buffer's size; `rowsInUse` the rows this frame's jobs cover (the largest dst + count).
    // `poseBuiltLastFrame` says EDVR built a pose table from the previous frame's pool.
    uint32_t note(uint64_t frame, uint64_t bufferId, uint64_t byteWidth, uint32_t rowsInUse, bool poseBuiltLastFrame) {
        ++c_.frames;
        uint32_t v = kHistoryOk;
        if (!have_) v = kHistoryFirst;
        else if (frame != lastFrame_ + 1) v = kHistoryGap;
        else if (bufferId == lastBuffer_) v = kHistorySame;
        else if (lastBytes_ / 48 < rowsInUse) v = kHistoryShrunk;
        else if (!poseBuiltLastFrame) v = kHistoryPose;
        have_ = true;
        lastFrame_ = frame;
        lastBuffer_ = bufferId;
        lastBytes_ = byteWidth;
        ++c_.verdict[v];
        return v;
    }
    uint64_t previousBuffer() const { return previousBuffer_; }
    void rememberPrevious() { previousBuffer_ = lastBuffer_; }
    void reset() { have_ = false; c_ = Counters{}; }
    const Counters& counters() const { return c_; }

private:
    bool have_ = false;
    uint64_t lastFrame_ = 0, lastBuffer_ = 0, lastBytes_ = 0, previousBuffer_ = 0;
    Counters c_;
};

// ---- the GPU's counters ----------------------------------------------------------------------------------------------
enum Stat : uint32_t {
    kStatFrames = 0,         // JoinCS runs
    kStatHookUsed,           // frames joined by the hook's list
    kStatPrefixUsed,         // frames joined by the table prefix (history certified, hook not used)
    kStatNoHistory,          // frames with no join (certificates failed)
    kStatHookDisagree,       // the hook was offered and the dispatch's own table disagreed (the frame fell back to the prefix)
    kStatPrevNotVerified,    // the hook was offered, agreed, but the previous frame had not been verified
    kStatJobs,               // jobs seen
    kStatJoined,             // jobs with a previous base
    kStatFailNoPrevEntity,   // hook: entity new, or its identity changed
    kStatFailRange,          // hook: the entity's row range changed length
    kStatFailLayout,         // hook: the job's offset holds a different (bind, count) than before
    kStatFailPrefix,         // prefix: past the first difference
    kStatFailPose,           // the previous pose table has no (or a conflicted) record at the previous base
    kStatFailCap,            // base or count outside the tables
    kStatDupBase,            // two jobs with one base
    kStatPrevHookOk,         // state: the previous frame's hook verification passed (not a counter)
    kStatMismatchBits,       // OR of the disagreement causes (below) over the window
    kStatPoseRecords,        // records scattered into a pose table
    kStatPoseConflicts,      // records that disagreed with another record of the same base
    kStatLastJobs,           // the last frame's job count (not a counter)
    kStatLastEntities,       // the last frame's entity count (not a counter)
    kStatWords = 24
};
enum MismatchBits : uint32_t {
    kMmNotInRange = 1,       // a job outside every entity's range
    kMmHeadCount = 2,        // a job at an entity's base with a different count than the hook's
    kMmHeads = 4,            // a different number of entity bases than the hook's
    kMmSum = 8,              // the jobs' counts do not add up to the hook's end
    kMmEntitySum = 16,       // an entity's jobs do not tile its range
    kMmNoPlan = 32,          // the plan was empty
};

// ---- the CPU reference of JoinCS -------------------------------------------------------------------------------------
struct DstInfo { uint32_t bind = 0, count = 0; };

struct JoinResult {
    std::vector<uint32_t> join;            // kMaxRows: previous base, 0 = none
    std::vector<DstInfo> dstInfo;          // kMaxRows: the (bind, count) of the job starting there
    uint32_t stats[kStatWords]{};
    uint32_t prevHookOk = 0;               // the state word after this frame
};

// Mirrors JoinCS exactly. `prevHookOk`: the state word the previous frame left. `prevPoseW0[row]`: word 0 of the previous
// pose table's record at that base (0 = none or conflicted).
inline JoinResult cpuJoin(const Plan& plan, const std::vector<JobRow>& jobs, const std::vector<JobRow>& prevJobs,
                          const std::vector<DstInfo>& prevDstInfo, const std::vector<uint32_t>& prevPoseW0,
                          uint32_t prevHookOk) {
    JoinResult r;
    r.join.assign(kMaxRows, 0);
    r.dstInfo.assign(kMaxRows, DstInfo{});
    uint32_t* s = r.stats;
    const uint32_t n = std::min<uint32_t>(uint32_t(jobs.size()), kMaxJobs);
    s[kStatFrames] = 1;
    s[kStatJobs] = n;
    s[kStatLastJobs] = n;
    s[kStatLastEntities] = plan.m;
    s[kStatPrevHookOk] = prevHookOk;
    // A: the table of this frame's jobs by base
    std::vector<uint8_t> valid(n, 0);
    for (uint32_t j = 0; j < n; ++j) {
        const JobRow& jb = jobs[j];
        if (jb.count == 0 || jb.dst >= kMaxRows || jb.count > kMaxRows - jb.dst) { ++s[kStatFailCap]; continue; }
        if (r.dstInfo[jb.dst].count != 0) { ++s[kStatDupBase]; continue; }
        r.dstInfo[jb.dst] = DstInfo{jb.bind, jb.count};
        valid[j] = 1;
    }
    // B: the hook's list against the table
    uint32_t mismatch = 0;
    bool hookOk = false;
    std::vector<uint32_t> entityOf(n, kNone);
    const bool offered = (plan.flags & kPlanHook) != 0;
    if (offered) {
        if (plan.m == 0 || plan.m > kMaxEntries) mismatch |= kMmNoPlan;
        uint32_t heads = 0;
        uint64_t sum = 0;
        std::vector<uint64_t> entitySum(plan.m, 0);
        for (uint32_t j = 0; j < n && !(mismatch & kMmNoPlan); ++j) {
            if (!valid[j]) continue;
            const JobRow& jb = jobs[j];
            // the last entity whose base is <= dst
            uint32_t lo = 0, hi = plan.m;
            while (lo < hi) { const uint32_t mid = (lo + hi) / 2; if (plan.rs[mid] <= jb.dst) lo = mid + 1; else hi = mid; }
            if (lo == 0 || jb.dst >= plan.rs[plan.m]) { mismatch |= kMmNotInRange; continue; }
            const uint32_t i = lo - 1;
            entityOf[j] = i;
            entitySum[i] += jb.count;
            sum += jb.count;
            if (jb.dst == plan.rs[i]) {
                ++heads;
                if (jb.count != plan.count[i]) mismatch |= kMmHeadCount;
            }
        }
        if (!(mismatch & kMmNoPlan)) {
            if (heads != plan.m) mismatch |= kMmHeads;
            if (sum != uint64_t(plan.end) - plan.rs[0]) mismatch |= kMmSum;
            for (uint32_t i = 0; i < plan.m; ++i) if (entitySum[i] != uint64_t(plan.rs[i + 1]) - plan.rs[i]) { mismatch |= kMmEntitySum; break; }
        }
        hookOk = mismatch == 0;
        if (!hookOk) { ++s[kStatHookDisagree]; s[kStatMismatchBits] = mismatch; }
    }
    // C: which source this frame uses
    const bool history = (plan.flags & kPlanHistory) != 0;
    const bool useHook = history && offered && hookOk && prevHookOk;
    if (history && offered && hookOk && !prevHookOk) ++s[kStatPrevNotVerified];
    r.prevHookOk = offered && hookOk ? 1 : 0;
    s[kStatPrevHookOk] = r.prevHookOk;
    if (!history) { s[kStatNoHistory] = 1; return r; }
    if (useHook) s[kStatHookUsed] = 1; else s[kStatPrefixUsed] = 1;
    // prefix limit: the first job whose (bind, count) differs from last frame's job at the same table position
    uint32_t prefix = std::min<uint32_t>(n, std::min<uint32_t>(plan.prevJobs, uint32_t(prevJobs.size())));
    if (!useHook) {
        for (uint32_t k = 0; k < prefix; ++k)
            if (jobs[k].bind != prevJobs[k].bind || jobs[k].count != prevJobs[k].count) { prefix = k; break; }
    }
    // D: the join
    for (uint32_t j = 0; j < n; ++j) {
        if (!valid[j]) continue;
        const JobRow& jb = jobs[j];
        uint32_t prevDst = kNone;
        if (useHook) {
            const uint32_t i = entityOf[j];
            const uint32_t ip = i == kNone ? kNone : plan.prevIdx[i];
            if (ip == kNone || ip >= plan.prevM) { ++s[kStatFailNoPrevEntity]; continue; }
            if (plan.rs[i + 1] - plan.rs[i] != plan.prevRs[ip + 1] - plan.prevRs[ip]) { ++s[kStatFailRange]; continue; }
            prevDst = plan.prevRs[ip] + (jb.dst - plan.rs[i]);
            if (prevDst >= kMaxRows || prevDstInfo[prevDst].count != jb.count || prevDstInfo[prevDst].bind != jb.bind) { ++s[kStatFailLayout]; continue; }
        } else {
            if (j >= prefix) { ++s[kStatFailPrefix]; continue; }
            prevDst = prevJobs[j].dst;
        }
        if (prevDst == 0 || prevDst >= kMaxRows || prevPoseW0[prevDst] != prevDst) { ++s[kStatFailPose]; continue; }
        r.join[jb.dst] = prevDst;
        ++s[kStatJoined];
    }
    return r;
}

// ---- the periodic line -----------------------------------------------------------------------------------------------
// One line per window. `d` = the GPU counters' deltas over the window (kStatWords), `f` and `p` the CPU side's deltas.
struct WindowCpu {
    uint64_t steps = 0, offered = 0, sameThread = 0, otherThread = 0;
    uint64_t decline[kDeclineCount]{};
    uint64_t history[kHistoryCount]{};
};
inline std::string joinLine(const uint32_t (&d)[kStatWords], const WindowCpu& c, bool hookArmed, const char* hookState) {
    const uint32_t frames = d[kStatFrames];
    const char* source = d[kStatHookUsed] && d[kStatPrefixUsed] ? "hook+prefix" : d[kStatHookUsed] ? "hook" : d[kStatPrefixUsed] ? "prefix" : "none";
    char b[1024];
    std::snprintf(b, sizeof(b),
        "skin join: source=%s hook=%s frames=%u (hook %u, prefix %u, no history %u) jobs=%u joined=%u failed: new-entity %u, range %u, layout %u, "
        "prefix %u, pose %u, cap %u, dup-base %u | hook/t0 disagreements %u (causes 0x%x), unverified-previous %u | offered %llu, declined "
        "[%s %llu, %s %llu, %s %llu, %s %llu, %s %llu, %s %llu], threads same %llu other %llu | history [%s %llu, %s %llu, %s %llu, %s %llu, %s %llu] | "
        "pose records %u conflicts %u | last frame: %u jobs, %u entities",
        source, hookArmed ? hookState : "off", frames, d[kStatHookUsed], d[kStatPrefixUsed], d[kStatNoHistory], d[kStatJobs], d[kStatJoined],
        d[kStatFailNoPrevEntity], d[kStatFailRange], d[kStatFailLayout], d[kStatFailPrefix], d[kStatFailPose], d[kStatFailCap], d[kStatDupBase],
        d[kStatHookDisagree], d[kStatMismatchBits], d[kStatPrevNotVerified], (unsigned long long)c.offered,
        declineName(kDeclineNoHistory), (unsigned long long)c.decline[kDeclineNoHistory], declineName(kDeclineNoSnapshot), (unsigned long long)c.decline[kDeclineNoSnapshot],
        declineName(kDeclineStale), (unsigned long long)c.decline[kDeclineStale], declineName(kDeclineGap), (unsigned long long)c.decline[kDeclineGap],
        declineName(kDeclineUnusable), (unsigned long long)c.decline[kDeclineUnusable], declineName(kDeclineNoPrevious), (unsigned long long)c.decline[kDeclineNoPrevious],
        (unsigned long long)c.sameThread, (unsigned long long)c.otherThread,
        historyName(kHistoryFirst), (unsigned long long)c.history[kHistoryFirst], historyName(kHistoryGap), (unsigned long long)c.history[kHistoryGap],
        historyName(kHistorySame), (unsigned long long)c.history[kHistorySame], historyName(kHistoryShrunk), (unsigned long long)c.history[kHistoryShrunk],
        historyName(kHistoryPose), (unsigned long long)c.history[kHistoryPose],
        d[kStatPoseRecords], d[kStatPoseConflicts], d[kStatLastJobs], d[kStatLastEntities]);
    return b;
}

} // namespace skinjoin
} // namespace edvr
