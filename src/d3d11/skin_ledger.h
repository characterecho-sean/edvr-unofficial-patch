// THE SKIN LEDGER (2026-10-08, the F2 settling instrument; docs/kinematic-motion-injection-2026-09-19.md, "F2 study").
//
// A log-only, dump-only capture that rides the eye run's existing ledger (object_probe.cpp, objectProbeArmLedger):
// no config key, nothing on screen, nothing in motion, and nothing at all while no run is armed. It exists to answer
// one question with data nobody has seen yet: is the bone palette the game leaves in its OTHER 8 MB buffer, during
// frame N, bit for bit the palette it used for frame N-1? (fRenderSkinningProcessorNode swaps GpuTransformData and
// PrevGpuTransformData every frame, FUN_144C54A20, RVA 0x4C54A20; the evidence is decompiled code and a parity
// pattern, not two consecutive frames.)
//
// What a run keeps, for the ledger's 20 frames:
//   * every palette-chain dispatch (cs_6FE04AF836BB1DBA, APPLY_BIND_POSE_TRANSFORMS_CS, one thread group a job):
//     its job table (t0, 16 bytes a job: srcBase, dstBase, bindBase, count), its CPU joint matrices (t2, 48 bytes a
//     bone), its bind poses (t1, once a run per buffer), and the views and buffer identities of t0 t1 t2 u0;
//   * both persistent palette buffers WHOLE at the first pool draw of each frame (bones<p>_<stamp>_<frame>.bin, the
//     first 3 MiB of each; that is what the old first-megabyte copy was meant to be);
//   * a line a frame and a RESULT line in the log; the checker (tools/skin_palette_check.py) does the rest offline.
//
// WHY THE OLD BONES FILES WERE ALL ZERO (measured 2026-10-08 on this machine's GPU and on WARP, boxcopy.cpp in the
// F2 notes): CopySubresourceRegion of a box out of a stride-48 STRUCTURED buffer reads nothing when the box is not a
// multiple of the stride; the ledger's box was 1,048,576 bytes (= 21845.33 rows). A box of 1,048,560 reads the rows,
// and so does a whole CopyResource, which is what this uses. The call is silently dropped, so staging stays zero.
//
// Pure logic only: no D3D, no globals. object_probe.cpp is the glue (the reads, the copies, the late readback) and
// tools/skin_ledger_test is the rig; tools/skin_ledger_test/mutants.py holds the rig to the rules below.
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace edvr {
namespace skin {

constexpr uint32_t kFrames = 20;                                // the ledger's window (object_probe.cpp kLedgerFrames)
constexpr uint64_t kChainHash = 0x6FE04AF836BB1DBAull;          // APPLY_BIND_POSE_TRANSFORMS_CS, 128x1x1, 70 lines
constexpr uint64_t kClearHash = 0x7B2A531B72941109ull;          // CLEAR_TRANSFORM_DATA_CS (identity fill)
constexpr uint32_t kMaxDispatches = 4;                          // chain dispatches kept a frame (the census showed one)
constexpr uint32_t kMaxBinds = 4;                               // distinct bind-pose buffers kept a run
constexpr uint32_t kMaxPalettes = 4;                            // object_probe.cpp kLedgerPalettes
constexpr uint32_t kRowBytes = 48;                              // a palette row: three float4, 3x4 row-major
constexpr uint32_t kJobBytes = 16;                              // a job: srcBase, dstBase, bindBase, count
constexpr uint32_t kKeepPaletteBytes = 65536u * kRowBytes;      // 3 MiB: rows 0..65535 (the largest palette seen used 29,573)
constexpr uint32_t kMaxCopyBytes = 16u << 20;                   // a whole-buffer copy above this is declined
constexpr uint32_t kRecordBytes = 336;                          // a t33 pool record

enum class Kind : uint8_t { Jobs = 0, Joints = 1, Bind = 2 };

// A buffer view as the context reported it (zero = not asked or not a buffer view).
struct View {
    uint32_t first = 0, num = 0, stride = 0, valid = 0;
};

// What the glue read off the context at a chain dispatch.
struct DispatchInfo {
    uint32_t x = 0, y = 0, z = 0;
    uint64_t t0 = 0, t1 = 0, t2 = 0, u0 = 0;    // resource identities: compared and written, never dereferenced
    View v0, v1, v2, vu;
    bool foreign = false;                        // recorded on a deferred context
};

// What the glue should copy for one chain dispatch.
struct ChainPlan {
    int disp = -1;                               // slot in the frame, -1 when over the cap or outside the window
    bool stageJobs = false, stageJoints = false, stageBind = false;
    int bind = -1;                               // index into the run's bind-pose list, -1 when over the cap
};

struct Dispatch {
    DispatchInfo info;
    int32_t bind = -1;
    int32_t jointsFrom = -1;                     // the dispatch of this frame that holds t2 (itself, or an earlier one)
    bool jobsStaged = false, jointsStaged = false;
    std::vector<uint8_t> jobs, joints;
    uint32_t groups() const { return info.x * (info.y ? info.y : 1u) * (info.z ? info.z : 1u); }
};

struct Frame {
    uint32_t dispatches = 0;                     // compute dispatches of any shader seen in the frame
    uint32_t clears = 0;                         // ... of them the identity fill
    uint32_t chainSeen = 0;                      // ... of them the palette chain
    uint32_t chainOver = 0;                      // chain dispatches beyond kMaxDispatches
    uint32_t declined = 0;                       // copies refused (size 0 or over the cap, or an unresolved binding)
    uint32_t lost = 0;                           // copies staged but never read back
    bool pool = false;                           // a pool draw bound a palette this frame
    int32_t bound = -1;                          // ... which of the learned palettes (-1 none learned yet)
    uint64_t boundId = 0;
    View t38;                                    // ... through which view (FirstElement 0 = rows addressed from the buffer's start)
    uint32_t palIssued = 0, palGot = 0;          // palette copies staged / read back
    std::vector<Dispatch> d;
};

struct Bind {
    uint64_t id = 0;
    View v;
    uint32_t bytes = 0;                          // the buffer's size as seen
    bool staged = false;
    std::vector<uint8_t> data;
};

struct PoolRef {
    const uint8_t* data = nullptr;
    size_t bytes = 0;
};

struct FrameStats {
    uint32_t groups = 0;                         // thread groups over the kept chain dispatches
    uint32_t jobs = 0;                           // jobs parsed out of the delivered job tables
    uint32_t jobsMissing = 0;                    // groups whose job table never arrived (or arrived short)
    uint64_t bones = 0;                          // sum of the jobs' counts
    uint32_t dstEnd = 0;                         // largest dstBase + count
    uint32_t gaps = 0, overlaps = 0;             // the running-sum check: jobs by dst, each starting where the last ended
    uint32_t poolSkinned = 0;                    // distinct nonzero word-0 values among the frame's t33 records
    uint32_t poolNotDst = 0;                     // ... of them that are not any job's dstBase
    bool poolKnown = false;
};

inline uint32_t rd32(const uint8_t* p) {
    uint32_t v;
    std::memcpy(&v, p, 4);
    return v;
}

class SkinLedger {
public:
    using Sink = std::function<void(const std::string&)>;

    bool armed() const { return armed_; }
    bool finished() const { return finished_; }
    uint32_t frame0() const { return frame0_; }

    // Arm for a run whose ledger frames are frame0 .. frame0 + kFrames - 1. Clears everything.
    void arm(uint32_t frame0) {
        reset();
        armed_ = true;
        frame0_ = frame0;
    }

    // Disarm and drop everything (the run is over, or was switched off).
    void reset() {
        armed_ = false;
        finished_ = false;
        frame0_ = 0;
        for (Frame& f : frames_) f = Frame();
        binds_.clear();
        for (uint32_t p = 0; p < kMaxPalettes; ++p) paletteId_[p] = 0, paletteBytes_[p] = 0;
        paletteCount_ = 0;
        skipped_ = 0;
        poolReleases_ = 0;
        foreign_ = 0;
    }

    bool inWindow(uint32_t f) const { return armed_ && f >= frame0_ && f - frame0_ < kFrames; }

    // ---- events: every one is a no-op while unarmed or outside the window ------------------------------------------

    // Any compute dispatch, with the content hash of the bound compute shader (0 = unknown).
    void noteDispatch(uint32_t f, uint64_t csHash) {
        if (!inWindow(f)) return;
        Frame& fr = frames_[f - frame0_];
        ++fr.dispatches;
        if (csHash == kClearHash) ++fr.clears;
        if (csHash == kChainHash) ++fr.chainSeen;
    }

    // A palette-chain dispatch: what to copy. One job table a dispatch, the joint matrices once a frame per buffer,
    // each bind-pose buffer once a run.
    ChainPlan planChain(uint32_t f, const DispatchInfo& info, uint32_t jobsBytes, uint32_t jointsBytes, uint32_t bindBytes) {
        ChainPlan plan;
        if (!inWindow(f)) return plan;
        Frame& fr = frames_[f - frame0_];
        if (fr.d.size() >= kMaxDispatches) {
            ++fr.chainOver;
            return plan;
        }
        Dispatch d;
        d.info = info;
        const int self = static_cast<int>(fr.d.size());
        for (int k = 0; k < self; ++k) {
            if (fr.d[k].jointsFrom == k && fr.d[k].info.t2 == info.t2 && info.t2 != 0) d.jointsFrom = k;
        }
        plan.stageJobs = info.t0 != 0 && jobsBytes != 0 && jobsBytes <= kMaxCopyBytes;
        if (!plan.stageJobs) ++fr.declined;
        if (d.jointsFrom < 0) {
            plan.stageJoints = info.t2 != 0 && jointsBytes != 0 && jointsBytes <= kMaxCopyBytes;
            if (plan.stageJoints) d.jointsFrom = self;
            else ++fr.declined;
        }
        if (info.t1 != 0) {
            for (size_t k = 0; k < binds_.size(); ++k) {
                if (binds_[k].id == info.t1) plan.bind = static_cast<int>(k);
            }
            if (plan.bind < 0 && binds_.size() < kMaxBinds && bindBytes != 0 && bindBytes <= kMaxCopyBytes) {
                Bind b;
                b.id = info.t1;
                b.v = info.v1;
                b.bytes = bindBytes;
                b.staged = true;
                binds_.push_back(b);
                plan.bind = static_cast<int>(binds_.size()) - 1;
                plan.stageBind = true;
            } else if (plan.bind < 0) {
                ++fr.declined;
            }
        } else {
            ++fr.declined;
        }
        d.bind = plan.bind;
        d.jobsStaged = plan.stageJobs;
        d.jointsStaged = plan.stageJoints;
        fr.d.push_back(std::move(d));
        plan.disp = self;
        return plan;
    }

    // A palette the glue learned (the buffers a pool draw binds at t38).
    void setPalette(uint32_t p, uint64_t id, uint32_t bytes) {
        if (!armed_ || p >= kMaxPalettes) return;
        paletteId_[p] = id;
        paletteBytes_[p] = bytes;
        if (p + 1 > paletteCount_) paletteCount_ = p + 1;
    }

    // The frame's first pool draw: which palette was bound at t38.
    void notePoolDraw(uint32_t f, int bound, uint64_t boundId, const View& t38 = View()) {
        if (!inWindow(f)) return;
        Frame& fr = frames_[f - frame0_];
        if (fr.pool) return;
        fr.pool = true;
        fr.bound = bound;
        fr.boundId = boundId;
        fr.t38 = t38;
    }

    void notePaletteIssued(uint32_t f, bool ok) {
        if (!inWindow(f)) return;
        if (ok) ++frames_[f - frame0_].palIssued;
        else ++skipped_;
    }
    void notePaletteGot(uint32_t f) {
        if (!inWindow(f)) return;
        ++frames_[f - frame0_].palGot;
    }

    // A staged copy that came back.
    void deliver(Kind k, uint32_t f, int idx, const uint8_t* data, uint32_t n) {
        if (!armed_ || !data) return;
        if (k == Kind::Bind) {
            if (idx >= 0 && static_cast<size_t>(idx) < binds_.size()) binds_[idx].data.assign(data, data + n);
            return;
        }
        if (!inWindow(f)) return;
        Frame& fr = frames_[f - frame0_];
        if (idx < 0 || static_cast<size_t>(idx) >= fr.d.size()) return;
        (k == Kind::Jobs ? fr.d[idx].jobs : fr.d[idx].joints).assign(data, data + n);
    }

    // A staged copy that was dropped (never ready, or the staging buffer could not be made).
    void noteLost(uint32_t f) {
        if (!inWindow(f)) return;
        ++frames_[f - frame0_].lost;
    }
    void noteSkipped() {
        if (armed_) ++skipped_;
    }
    void notePoolRelease() {
        if (armed_) ++poolReleases_;
    }
    // Dispatches recorded on deferred contexts (any thread): the glue counts them and reads none, so the run's state
    // stays the owner thread's. The count is handed over once, at the report.
    void noteForeign(uint32_t n) {
        if (armed_) foreign_ = n;
    }

    const Frame& frame(uint32_t i) const { return frames_[i]; }
    const std::vector<Bind>& binds() const { return binds_; }

    // ---- analysis ---------------------------------------------------------------------------------------------------

    FrameStats analyse(uint32_t i, const PoolRef* pool) const {
        FrameStats s;
        const Frame& fr = frames_[i];
        std::vector<std::pair<uint32_t, uint32_t>> jobs;   // (dst, count)
        std::vector<uint32_t> dsts;
        for (const Dispatch& d : fr.d) {
            const uint32_t groups = d.groups();
            s.groups += groups;
            const uint32_t have = static_cast<uint32_t>(d.jobs.size() / kJobBytes);
            const uint32_t take = std::min(groups, have);
            if (take < groups) s.jobsMissing += groups - take;
            for (uint32_t k = 0; k < take; ++k) {
                const uint8_t* j = d.jobs.data() + static_cast<size_t>(k) * kJobBytes;
                const uint32_t dst = rd32(j + 4), count = rd32(j + 12);
                ++s.jobs;
                s.bones += count;
                if (count == 0) continue;
                jobs.emplace_back(dst, count);
                dsts.push_back(dst);
                s.dstEnd = std::max(s.dstEnd, dst + count);
            }
        }
        std::sort(jobs.begin(), jobs.end());
        for (size_t k = 1; k < jobs.size(); ++k) {
            const uint32_t end = jobs[k - 1].first + jobs[k - 1].second;
            if (jobs[k].first > end) ++s.gaps;
            else if (jobs[k].first < end) ++s.overlaps;
        }
        if (pool && pool->data && pool->bytes >= kRecordBytes) {
            s.poolKnown = true;
            std::vector<uint32_t> bases;
            const size_t records = pool->bytes / kRecordBytes;
            for (size_t r = 0; r < records; ++r) {
                const uint32_t b = rd32(pool->data + r * kRecordBytes);
                if (b != 0) bases.push_back(b);
            }
            std::sort(bases.begin(), bases.end());
            bases.erase(std::unique(bases.begin(), bases.end()), bases.end());
            std::sort(dsts.begin(), dsts.end());
            s.poolSkinned = static_cast<uint32_t>(bases.size());
            // A frame with no job table (the chain was not dispatched in it, or its tables never arrived) has no dst to compare the bases with:
            // every base would be "not a job dst" and the finding would be the frame, not the pool (run 154827: a false positive).
            for (uint32_t b : bases) {
                if (!dsts.empty() && !std::binary_search(dsts.begin(), dsts.end(), b)) ++s.poolNotDst;
            }
        }
        return s;
    }

    // ---- the report -------------------------------------------------------------------------------------------------

    // One line a frame and the RESULT line, through `sink`; then, and only then, finished() is true. A second call
    // says nothing. `how` names what closed the window ("window closed" or "shutdown before the window closed"),
    // `fileStatus` what happened to skin_<stamp>.bin ("written", "WRITE FAILED", "not written").
    void report(const std::string& stamp, const PoolRef* pools, const char* how, const char* fileStatus, const Sink& sink) {
        if (!armed_ || finished_) return;
        uint32_t anyDispatch = 0, chainFrames = 0, poolFrames = 0, dataFrames = 0, palFrames = 0, palPairFrames = 0;
        uint32_t chainSeen = 0, lost = 0, declined = 0, over = 0, notDst = 0, gaps = 0, overlaps = 0, jobsMissing = 0;
        uint32_t maxEnd = 0;
        uint64_t jobsTotal = 0;
        char line[1024];
        for (uint32_t i = 0; i < kFrames; ++i) {
            const Frame& fr = frames_[i];
            const FrameStats s = analyse(i, pools ? &pools[i] : nullptr);
            uint32_t t0Bytes = 0, t2Bytes = 0;
            bool dataOk = !fr.d.empty();
            for (const Dispatch& d : fr.d) {
                t0Bytes += static_cast<uint32_t>(d.jobs.size());
                t2Bytes += static_cast<uint32_t>(d.joints.size());
                if (d.jobsStaged && d.jobs.empty()) dataOk = false;
                if (d.jointsStaged && d.joints.empty()) dataOk = false;
            }
            anyDispatch += fr.dispatches;
            chainSeen += fr.chainSeen;
            lost += fr.lost;
            declined += fr.declined;
            over += fr.chainOver;
            notDst += s.poolNotDst;
            gaps += s.gaps;
            overlaps += s.overlaps;
            jobsMissing += s.jobsMissing;
            jobsTotal += s.jobs;
            maxEnd = std::max(maxEnd, s.dstEnd);
            chainFrames += fr.chainSeen ? 1u : 0u;
            poolFrames += fr.pool ? 1u : 0u;
            dataFrames += dataOk ? 1u : 0u;
            palFrames += fr.palGot ? 1u : 0u;
            palPairFrames += fr.palGot >= 2 ? 1u : 0u;
            std::snprintf(line, sizeof(line),
                          "skin ledger frame %u: %u dispatches (chain %u kept %u over %u, identity fill %u), %u groups, %u jobs "
                          "(%u missing), %llu bones, dst end row %u, running sum gaps %u overlaps %u; pool draw %s (t38 = palette %d); "
                          "palettes copied %u read back %u; t33 skinned bases %s%u (not a job dst %u%s); t0 %u B t2 %u B; copies lost %u declined %u",
                          frame0_ + i, fr.dispatches, fr.chainSeen, static_cast<unsigned>(fr.d.size()), fr.chainOver, fr.clears,
                          s.groups, s.jobs, s.jobsMissing, static_cast<unsigned long long>(s.bones), s.dstEnd, s.gaps, s.overlaps,
                          fr.pool ? "yes" : "NO", fr.bound, fr.palIssued, fr.palGot, s.poolKnown ? "" : "? ", s.poolSkinned,
                          s.poolNotDst, s.poolKnown && s.jobs == 0 ? ", no job table to compare with" : "", t0Bytes, t2Bytes, fr.lost, fr.declined);
            sink(line);
        }
        const char* verdict;
        char why[256];
        if (anyDispatch == 0) {
            verdict = "NEVER RAN";
            std::snprintf(why, sizeof(why),
                          "no compute dispatch reached the hook in %u frames (the hook was not reached, or no scene was rendered)", kFrames);
        } else if (chainSeen == 0) {
            verdict = "NEVER RAN";
            std::snprintf(why, sizeof(why),
                          "%u compute dispatches seen, none was cs_%016llX (no skinned characters this run, or the shader's hash moved)",
                          anyDispatch, static_cast<unsigned long long>(kChainHash));
        } else if (lost || declined || skipped_ || jobsMissing || dataFrames < chainFrames || palFrames == 0) {
            // palFrames == 0: the chain ran and no palette was ever read back -- no pool draw bound one (the pool was not
            // recognised, or the run drew no scene) or its copy never came -- so there is nothing to compare: not a
            // success, and not "never ran" either.
            verdict = "BROKEN";
            std::snprintf(why, sizeof(why),
                          "the chain ran but %u copies were lost, %u declined, %u skipped, %u groups have no job table, "
                          "%u of %u chain frames hold their data, %u of %u pool-draw frames hold a palette, "
                          "pool draws seen in %u of %u frames",
                          lost, declined, skipped_, jobsMissing, dataFrames, chainFrames, palFrames, poolFrames, poolFrames, kFrames);
        } else if (chainFrames + 1 < kFrames || poolFrames + 1 < kFrames || palPairFrames + 2 < kFrames) {
            // A run arms mid-frame (its first frame may have missed the chain) and learns one palette in its first
            // frame, so a whole run is 19 chain frames, 19 pool frames and 18 frames with both palettes at the least.
            verdict = "PARTIAL";
            std::snprintf(why, sizeof(why),
                          "every copy that was asked for came back, but only %u of %u frames saw the chain, %u a pool draw, "
                          "%u both palettes",
                          chainFrames, kFrames, poolFrames, palPairFrames);
        } else {
            verdict = "RAN";
            std::snprintf(why, sizeof(why), "the chain, a pool draw and both palettes in every frame but the arming one");
        }
        std::snprintf(line, sizeof(line),
                      "skin ledger RESULT %s for eye run %s (%s): %s; frames with chain %u/%u, pool draw %u/%u, data %u/%u, palette %u/%u "
                      "(both %u); chain dispatches %u, jobs %llu, largest dst end row %u (kept rows %u), t33 bases not a job dst %u, "
                      "running sum gaps %u overlaps %u, pool releases %u, %u dispatches on deferred contexts counted and not read; "
                      "skin_%s.bin %s. tools\\skin_palette_check.py reads it.",
                      verdict, stamp.c_str(), how, why, chainFrames, kFrames, poolFrames, kFrames, dataFrames, kFrames, palFrames,
                      kFrames, palPairFrames, chainSeen, static_cast<unsigned long long>(jobsTotal), maxEnd,
                      kKeepPaletteBytes / kRowBytes, notDst, gaps, overlaps, poolReleases_, foreign_, stamp.c_str(), fileStatus);
        sink(line);
        finished_ = true;   // after the last line is out, never before: a run that died inside the sink did not report
    }

    // ---- the file ---------------------------------------------------------------------------------------------------

    // skin_<stamp>.bin, version 1, little-endian. tools/skin_palette_check.py reads it; the rig writes one with this
    // code and the checker's build gate reads that.
    //   header 104 B: "EDVRSKN1", u32 version frames frame0 nPalettes keepBytes rowBytes jobBytes nBinds, u64 chainHash
    //     clearHash, u64 paletteId[4], u32 paletteBytes[4]
    //   nBinds x { u64 id, u32 first num stride valid, u32 dataBytes, data }
    //   kFrames x { u32 frame flags(bit0 pool), i32 bound, u32 t38First, u64 boundId, u32 t38Num t38Valid, u32 dispatches
    //     clears chainSeen chainOver palIssued palGot nKept lost, nKept x { u32 x y z groups, u64 t0 t1 t2 u0,
    //     4 x (u32 first num stride valid), i32 bind jointsFrom, u32 jobsBytes jointsBytes, jobs, joints } }  (64 B header)
    std::vector<uint8_t> serialize() const {
        std::vector<uint8_t> o;
        auto u32 = [&](uint32_t v) { const uint8_t* p = reinterpret_cast<const uint8_t*>(&v); o.insert(o.end(), p, p + 4); };
        auto i32 = [&](int32_t v) { u32(static_cast<uint32_t>(v)); };
        auto u64 = [&](uint64_t v) { const uint8_t* p = reinterpret_cast<const uint8_t*>(&v); o.insert(o.end(), p, p + 8); };
        auto view = [&](const View& v) { u32(v.first); u32(v.num); u32(v.stride); u32(v.valid); };
        static const char kMagic[8] = {'E', 'D', 'V', 'R', 'S', 'K', 'N', '1'};
        o.insert(o.end(), kMagic, kMagic + 8);
        u32(1u); u32(kFrames); u32(frame0_); u32(paletteCount_); u32(kKeepPaletteBytes); u32(kRowBytes); u32(kJobBytes);
        u32(static_cast<uint32_t>(binds_.size()));
        u64(kChainHash); u64(kClearHash);
        for (uint32_t p = 0; p < kMaxPalettes; ++p) u64(paletteId_[p]);
        for (uint32_t p = 0; p < kMaxPalettes; ++p) u32(paletteBytes_[p]);
        for (const Bind& b : binds_) {
            u64(b.id);
            view(b.v);
            u32(static_cast<uint32_t>(b.data.size()));
            o.insert(o.end(), b.data.begin(), b.data.end());
        }
        for (uint32_t i = 0; i < kFrames; ++i) {
            const Frame& fr = frames_[i];
            u32(frame0_ + i); u32(fr.pool ? 1u : 0u); i32(fr.bound); u32(fr.t38.first); u64(fr.boundId);
            u32(fr.t38.num); u32(fr.t38.valid);
            u32(fr.dispatches); u32(fr.clears); u32(fr.chainSeen); u32(fr.chainOver);
            u32(fr.palIssued); u32(fr.palGot); u32(static_cast<uint32_t>(fr.d.size())); u32(fr.lost);
            for (const Dispatch& d : fr.d) {
                u32(d.info.x); u32(d.info.y); u32(d.info.z); u32(d.groups());
                u64(d.info.t0); u64(d.info.t1); u64(d.info.t2); u64(d.info.u0);
                view(d.info.v0); view(d.info.v1); view(d.info.v2); view(d.info.vu);
                i32(d.bind); i32(d.jointsFrom);
                u32(static_cast<uint32_t>(d.jobs.size())); u32(static_cast<uint32_t>(d.joints.size()));
                o.insert(o.end(), d.jobs.begin(), d.jobs.end());
                o.insert(o.end(), d.joints.begin(), d.joints.end());
            }
        }
        return o;
    }

    bool writeFile(const std::wstring& path) const {
        const std::vector<uint8_t> bytes = serialize();
        FILE* f = nullptr;
        if (_wfopen_s(&f, path.c_str(), L"wb") || !f) return false;
        const bool ok = std::fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
        return std::fclose(f) == 0 && ok;
    }

private:
    bool armed_ = false;
    bool finished_ = false;
    uint32_t frame0_ = 0;
    Frame frames_[kFrames];
    std::vector<Bind> binds_;
    uint64_t paletteId_[kMaxPalettes] = {};
    uint32_t paletteBytes_[kMaxPalettes] = {};
    uint32_t paletteCount_ = 0;
    uint32_t skipped_ = 0;
    uint32_t poolReleases_ = 0;
    uint32_t foreign_ = 0;
};

}  // namespace skin
}  // namespace edvr
