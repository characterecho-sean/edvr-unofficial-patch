#pragma once
// Build-specific, bounded capture of the KinematicRig render-record evaluator
// (FUN_14430EFE0, RVA 0x430EFE0) and a QPC bracket around the Kinematic job
// bodies, plus a rig-link capture on FUN_14431AFE0 (RVA 0x431AFE0): the
// direct rig -> *(rig+0x348) collection link the job descriptor cannot give,
// joined against the ownership capture at dump time.
// Feeds docs/settlement-flicker-2026-09-17.md's next-flight spec:
// the predicate vtable at record+0x2C0, the skip flag at render-record+0x688,
// the content hash at record+0x268, and per-job-family CPU attribution.
// The hooks supply register state; this class only records evidence.
#include <windows.h>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace edvr {

class KinematicEvalProbe {
public:
    static constexpr uint32_t kExpectedTimestamp=1788384820u;
    static constexpr uint32_t kExpectedImageSize=104894464u;
    static constexpr uintptr_t kEvalRva=0x430EFE0u;
    static constexpr uint32_t kRecordCap=4096u;
    static constexpr uint32_t kTransitionCap=8192u;
    static constexpr uint32_t kVtableCap=128u;
    static constexpr uint32_t kJobCount=6u;
    static constexpr uint32_t kOwnershipCap=64u;
    static constexpr uintptr_t kRigEvalRva=0x431AFE0u;
    static constexpr uint32_t kRigLinkCap=256u;
    // Per-rig draw-item builder FUN_1442B4420 (decomp_42B4420.txt): appends
    // 0x150-byte items to per-bucket lists (bucket = *(model+0x20)), counting
    // each append at bucket+0x2A4. The census-join hook site (perf arc).
    static constexpr uintptr_t kBucketBuildRva=0x42B4420u;
    // The two DIRECT producers of the same bucket lists: the bucket arrives
    // as param_2, so their brackets skip the entry walk and read param_2+0x2A4
    // directly. 0 = FUN_144312E00 (decomp_4312E00.txt; the collection batch's
    // second item class via FUN_144312040; FUN_14431305C is inside it).
    // 1 = FUN_14369C9C0 (decomp_369C9C0.txt; FUN_1436A0F50's four call
    // sites -- the non-eye passes' builder family).
    static constexpr uintptr_t kDirectBuildRvas[2]={0x4312E00u,0x369C9C0u};
    static constexpr uint32_t kDirectProducerCount=2u;
    // Sized to cover a full capture window's mover demand: 31,514 samples
    // measured on flight 094158. 24 B/sample => ~768 KB, fine for a
    // diagnostic.
    static constexpr uint32_t kPoseSampleCap=32768u;
    static constexpr uint32_t kIdentityEventCap=256u;
    static constexpr uint32_t kClockSampleCap=4096u;
    // Dirty-queue (entry,exit) pairs: non-trivial runs only, so 64 covers
    // the append/reset pattern rather than a static scene's zero runs.
    static constexpr uint32_t kPhysQueueSampleCap=64u;
    static constexpr uint32_t kPhysNodeCap=256u; // node-capture ring (arc 19:45: peak delta 133)

    enum class HookStatus:uint32_t {NotRun,Installed,IdentityMismatch,OpcodeMismatch,InstallFailed};

    struct JobStat {
        // Seqlock: the bracket writer toggles seq odd/even around the three
        // counter stores; the JSON writer retries until it reads an even seq
        // unchanged across the snapshot. Never reset (parity must survive
        // clearLocked, which only waits for even).
        std::atomic<uint32_t> seq{0};
        std::atomic<uint64_t> calls{0},totalNs{0},maxNs{0};
    };

    struct RecordState {
        uint64_t record=0,node=0,poseCtx=0,predPtr=0,predVtableRva=0;
        uint64_t pred2Ptr=0,pred2VtableRva=0,hash=0,count298=0;
        uint64_t epoch1b8=0,descEpoch38=0;
        // record+0x130..0x188: the static 4x4 (bit-static in flight),
        // the updater's per-frame translation at +0x170, and the full
        // 8-byte packed quaternion at +0x17C (4x uint16 lanes,
        // component = (lane-32768)/32767; decoded 2026-09-20). Raw
        // bits; first/latest snapshots plus a change counter are the
        // engine-truth mover/static classification.
        uint64_t xfFirst[11]={},xfLatest[11]={};
        uint32_t flags=0,firstFrame=0,lastFrame=0;
        uint32_t xfChanges=0,lastXfChangeFrame=0;
        uint8_t bool234=0,predByte=0,gate2=0;
        // Bitmask of the jobs (bracket TLS, 1u<<jobId) under which this record
        // was EVER observed: the engine-truth mover/static discriminator after
        // the render path proved mover-blind and the captured record fields
        // proved homogeneous (2026-09-20 17:10 entry, clustering null).
        uint32_t jobMask=0;
        uint64_t calls=0;
        // Frame-aligned pose sampling: one sample per record per frame,
        // decoded from the same xf block the change compare already reads.
        uint32_t lastSampledFrame=0,framesSampled=0,dupInFrame=0;
        uint32_t gaps=0,maxGap=0,nodeChanges=0,quatChangeFrames=0;
        float maxJump=0;double totalJump=0;
        // 20-byte pose bits of the previous sample (12 translation + 8 quat).
        uint8_t prevPose[20]={};
        bool hasPrevSample=false;
    };
    struct Transition {
        uint32_t recordId=0,frame=0;
        uint32_t oldFlags=0,newFlags=0;
        uint64_t oldHash=0,newHash=0;
    };
    struct VtableUse {uint64_t rva=0;uint32_t firstFrame=0;uint64_t hits=0;};
    // One row per distinct collection seen at a job-body entry (jobs 0 and 1).
    // Proves or refutes collection+0x18 == KinematicRig: backPtr == collection
    // means *(owner+0x348) points back at the collection (the rig's own
    // collection slot), and ownerState == 4 is the rig's render-ready state.
    struct OwnershipUse {
        uint64_t collection=0,owner=0,alias20=0,epoch90=0,backPtr=0;
        uint32_t ownerState=0,jobId=0,firstFrame=0;
        uint64_t hits=0;
    };
    // One row per distinct rig seen at FUN_14431AFE0 entry (rig = param_1,
    // pose context = param_2). collection = *(rig+0x348): the direct
    // rig -> collection link the job descriptor cannot give. The dump joins
    // each row's collection against the ownership capture.
    struct RigLinkUse {
        uint64_t rig=0,poseCtx=0,collection=0,gameObj=0,descriptor=0;
        uint64_t owner=0,collEpoch90=0;
        uint32_t rigState=0,firstFrame=0;
        uint64_t hits=0;
    };
    // One frame-aligned pose sample for a mover-class record.
    struct PoseSample {
        uint32_t recordId=0,frame=0;
        float tx=0,ty=0,tz=0;                 // record+0x170
        uint16_t q0=0,q1=0,q2=0,q3=0;         // record+0x17C lanes, raw
    };
    // Identity event: kind 1 = gap-resume (record unseen for gapLen frames,
    // then re-seen), kind 2 = node-change (node pointer changed while the
    // record pointer stayed continuously seen -- possible slot reuse).
    struct IdentityEvent {
        uint32_t recordId=0,frame=0,kind=0,gapLen=0;
        uint64_t oldNode=0,newNode=0;
    };
    // One present-clock sample, appended once per owned Present while
    // armed (the mesh clock it was once compared against retired with mesh
    // motion on 2026-09-23).
    struct ClockSample {uint32_t present=0;};
    struct Summary {
        uint64_t observed=0;uint32_t stored=0;
        uint64_t readFaults=0,recordOverflow=0,transitionOverflow=0,vtableOverflow=0;
        uint64_t transitions=0,vtables=0;
        uint64_t epochMatches=0,epochMismatches=0,epochChanges=0;
        uint64_t ownershipChecks=0,ownerBackPtrMatch=0,ownerState4=0,ownershipOverflow=0;
        uint64_t riglinkChecks=0,riglinkState4=0,riglinkNullCollection=0,riglinkOverflow=0;
        uint64_t xfMovers=0,xfChanges=0;
        uint64_t poseSamples=0,poseSampleOverflow=0,identityEventOverflow=0;
        uint64_t dupInFrame=0,gapEvents=0,nodeChangeEvents=0,quatChangeFrames=0;
        uint64_t framesCounted=0,zeroRecordFrames=0,maxFrameRecords=0;
        uint64_t minFrameRecords=~0ull; // the first real frame sets it
        uint64_t clockSampleOverflow=0;
        uint64_t gapRelogSkipped=0; // gap-resume re-logs past the per-frame cap
        // Pose samples whose translation jump was NaN/Inf (non-finite input
        // or overflow): rejected from maxJump/totalJump so the JSON stays
        // valid; the raw bits are still kept in prevPose/mover_samples.
        uint64_t nonFinitePose=0;
    };

    bool arm(uint32_t meshFrame) noexcept;
    void finish() noexcept;
    void reset() noexcept;
    void setFrame(uint32_t meshFrame) noexcept;
    // The probe's clock feed: exactly once per owned Present, from
    // device_hook. Replaces setFrame as the clock source (the mesh clock
    // was refuted by flight 083323). Gated on active like the rest of the
    // probe: the clock log lives and dies with the capture lifecycle.
    void notePresentFrame(uint32_t presentFrame) noexcept;
    bool active() const noexcept{return active_.load(std::memory_order_acquire);}
    HookStatus hookStatus() const noexcept{return hookStatus_;}
    const char* hookStatusText() const noexcept;

    // Called from the eval hook's replacement before the original runs.
    // jobMask: the bracket TLS bits (1u<<jobId) active at observation.
    void observe(uintptr_t descriptor,uintptr_t renderRecord,uint32_t jobMask=0) noexcept;
    // Job brackets call these around the original body.
    JobStat* jobStats() noexcept{return jobs_;}
    // Capture generation, bumped by clearLocked (arm/reset): a bracket that
    // entered before the bump drops its sample instead of committing an
    // old-capture completion into the new capture's counters.
    uint64_t jobGeneration() const noexcept{return jobGeneration_.load(std::memory_order_acquire);}
    // Called at job-body entry for jobs 0 (descriptor arg; collection =
    // read64(arg+0x10)-0x300) and 1 (collection arg) before the timed bracket.
    void noteOwnership(uint32_t jobId,uintptr_t arg) noexcept;
    // Called at FUN_14431AFE0 entry: rig = param_1, pose context = param_2.
    void noteRigLink(uintptr_t rig,uintptr_t poseCtx) noexcept;
    // The job-2 bracket reads the physics dirty-queue append counter at job
    // entry and exit (descriptor +0x18 points at it; decomp_432B2A0,
    // param_1[3]). Counts only, per-session like jobs[] -- NOT cleared by
    // clearLocked -- and never gated on active(): a tracker-only flight
    // harvests the lifecycle too.
    void notePhysQueue(uint32_t entryCount,uint32_t exitCount) noexcept;
    // The job-2 node capture (lifecycle decoded on flight 193356, kinematic
    // arc 19:45 entry; sanctioned 2026-09-20 20:27): node pointers walked
    // from queue[entry..exit) at job-2 exit by the bracket (array base
    // descriptor +0x10, 8-byte entries; decomp_432B2A0 lines 257-268). The
    // probe keeps the last kPhysNodeCap per session -- movers re-append
    // every frame, so the tail holds every active mover at dump time;
    // overflow counts every walked node no longer in the ring. Invariant:
    // total == nodes-in-ring + overflow. Same per-session discipline as
    // notePhysQueue: never gated on active(), NOT cleared by clearLocked.
    void notePhysNodes(const uint64_t* nodes,uint32_t count,uint32_t overflow) noexcept;
    // The draw-item-builder bracket (FUN_1442B4420) reads each touched
    // bucket's item counter (bucket+0x2A4) at entry and exit; buckets is the
    // deduped set size, items the summed positive deltas, negDeltas the
    // buckets whose counter fell (mid-call drain). flags: bit0 the entry
    // walk faulted or found no sane array, bit1 an exit read faulted (that
    // call's items dropped), bit2 the walk hit its bucket cap. Per-session,
    // never gated on active(), NOT cleared by clearLocked -- same
    // discipline as notePhysQueue, so tracker-only flights harvest it.
    static constexpr uint32_t kBucketFlagEntryWild=1u;
    static constexpr uint32_t kBucketFlagExitFault=2u;
    static constexpr uint32_t kBucketFlagOverflow=4u;
    void noteBucketBuild(uint32_t buckets,uint64_t items,uint32_t negDeltas,
                         uint32_t flags) noexcept;
    // The direct-producer brackets (bucket = param_2): per-call counter
    // deltas, no walk. A fault on either read drops the call's items and
    // counts in readFaults. Same per-session discipline as noteBucketBuild.
    void noteDirectBuild(uint32_t producer,uint64_t items,uint32_t negDeltas,
                         bool readFault) noexcept;

    Summary summary() const noexcept;
    void writeJson(std::ostringstream& json) const;
    // Deterministic fixture for tools\kinematic_json_test: fills every
    // production writeJson path with distinctive values so the build gate
    // can assert an exact round-trip (Sean, 2026-09-20: three writer
    // failures proved compilation alone is not evidence).
    void selfTestPopulateForJson() noexcept;

private:
    mutable std::mutex mutex_;
    std::atomic<bool> active_{false};
    std::atomic<uint32_t> frame_{0};
    uintptr_t imageBase_=0;
    HookStatus hookStatus_=HookStatus::NotRun;
    Summary summary_{};
    std::unordered_map<uint64_t,uint32_t> index_;
    std::vector<RecordState> records_;
    std::vector<Transition> transitions_;
    std::unordered_map<uint64_t,uint32_t> vtableIndex_;
    std::vector<VtableUse> vtables_;
    std::unordered_map<uint64_t,uint32_t> ownershipIndex_;
    std::vector<OwnershipUse> ownerships_;
    std::unordered_map<uint64_t,uint32_t> riglinkIndex_;
    std::vector<RigLinkUse> riglinks_;
    std::vector<PoseSample> samples_;
    std::vector<IdentityEvent> events_;
    std::vector<ClockSample> clockSamples_;
    uint32_t seenThisFrame_=0;
    // False until the first notePresentFrame after arm: the mesh-domain arm
    // stamp fired 3,073 fake gap events on 094158, so all pose sampling
    // waits for the present-domain clock.
    bool clockSeeded_=false;
    uint32_t gapRelogsThisFrame_=0; // per-frame gap-resume re-log budget (256)
    JobStat jobs_[kJobCount];
    std::atomic<uint64_t> jobGeneration_{0};
    // Physics dirty-queue counts (job 2, notePhysQueue): per-session like
    // jobs_ -- deliberately NOT cleared by clearLocked. Counters are relaxed
    // atomics written lock-free from the bracket; the verbatim (entry,exit)
    // pairs sit under mutex_.
    std::atomic<uint64_t> physQueueRuns_{0};
    std::atomic<uint64_t> physQueueAppended_{0};
    std::atomic<uint32_t> physQueueMaxDelta_{0};
    std::atomic<uint64_t> physQueueResets_{0};
    std::vector<std::pair<uint32_t,uint32_t>> physQueueSamples_;
    // Job-2 node capture (notePhysNodes): same discipline as the counters
    // above -- relaxed atomics written lock-free, ring under mutex_.
    std::atomic<uint64_t> physNodeTotal_{0};
    std::atomic<uint64_t> physNodeOverflow_{0};
    std::vector<uint64_t> physNodeRing_; // last kPhysNodeCap nodes, oldest first
    // Draw-item-builder bucket counts (noteBucketBuild): same per-session
    // discipline as the phys counters -- relaxed atomics, no mutex side.
    std::atomic<uint64_t> bucketCalls_{0};
    std::atomic<uint64_t> bucketItems_{0};
    std::atomic<uint64_t> bucketEmptyCalls_{0};   // walk sane, no buckets found
    std::atomic<uint64_t> bucketEntryWild_{0};    // entry walk faulted/insane
    std::atomic<uint64_t> bucketExitFault_{0};    // exit read faulted
    std::atomic<uint64_t> bucketNegDeltas_{0};    // mid-call drain events
    std::atomic<uint64_t> bucketOverflowCalls_{0};// bucket cap hit
    std::atomic<uint32_t> bucketMaxBuckets_{0};
    std::atomic<uint32_t> bucketMaxItems_{0};
    // Direct producers (noteDirectBuild): one stat set per producer.
    struct DirectBuildStat {
        std::atomic<uint64_t> calls{0};
        std::atomic<uint64_t> items{0};
        std::atomic<uint64_t> negDeltas{0};
        std::atomic<uint64_t> readFaults{0};
        std::atomic<uint32_t> maxItems{0};
    };
    DirectBuildStat direct_[kDirectProducerCount];

    void clearLocked();
    void noteVtableLocked(uint64_t rva,uint32_t frame) noexcept;
    void noteIdentityEventLocked(uint32_t recordId,uint32_t frame,uint32_t kind,
        uint32_t gapLen,uint64_t oldNode,uint64_t newNode) noexcept;
    void samplePoseLocked(uint32_t recordId,RecordState& r,const uint64_t* xf) noexcept;
    void flushFrameStatsLocked() noexcept;
    static bool guardedRead(uintptr_t address,void* output,size_t bytes) noexcept;
    static uint64_t read64(uintptr_t address,bool& ok) noexcept;
    static uint32_t read32(uintptr_t address,bool& ok) noexcept;
    static uint8_t read8(uintptr_t address,bool& ok) noexcept;
    uint64_t vtableRvaLocked(uintptr_t object) noexcept;
};

extern KinematicEvalProbe kinematicEvalProbe;

} // namespace edvr
