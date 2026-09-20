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

    enum class HookStatus:uint32_t {NotRun,Installed,IdentityMismatch,OpcodeMismatch,InstallFailed};

    struct JobStat {
        std::atomic<uint64_t> calls{0},totalNs{0},maxNs{0};
    };

    struct RecordState {
        uint64_t record=0,node=0,poseCtx=0,predPtr=0,predVtableRva=0;
        uint64_t pred2Ptr=0,pred2VtableRva=0,hash=0,count298=0;
        uint64_t epoch1b8=0,descEpoch38=0;
        // record+0x130..0x170: the world transform block (4x4, raw bits;
        // the doc's 3x4 at +0x130..0x16C sits inside it, +0x170 is the
        // next field). First/latest snapshots plus a change counter are
        // the engine-truth mover/static classification.
        uint64_t xfFirst[8]={},xfLatest[8]={};
        uint32_t flags=0,firstFrame=0,lastFrame=0;
        uint32_t xfChanges=0,lastXfChangeFrame=0;
        uint8_t bool234=0,predByte=0,gate2=0;
        uint64_t calls=0;
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
    struct Summary {
        uint64_t observed=0;uint32_t stored=0;
        uint64_t readFaults=0,recordOverflow=0,transitionOverflow=0,vtableOverflow=0;
        uint64_t transitions=0,vtables=0;
        uint64_t epochMatches=0,epochMismatches=0,epochChanges=0;
        uint64_t ownershipChecks=0,ownerBackPtrMatch=0,ownerState4=0,ownershipOverflow=0;
        uint64_t riglinkChecks=0,riglinkState4=0,riglinkNullCollection=0,riglinkOverflow=0;
        uint64_t xfMovers=0,xfChanges=0;
    };

    bool arm(uint32_t meshFrame) noexcept;
    void finish() noexcept;
    void reset() noexcept;
    void setFrame(uint32_t meshFrame) noexcept{frame_.store(meshFrame,std::memory_order_release);}
    bool active() const noexcept{return active_.load(std::memory_order_acquire);}
    HookStatus hookStatus() const noexcept{return hookStatus_;}
    const char* hookStatusText() const noexcept;

    // Called from the eval hook's replacement before the original runs.
    void observe(uintptr_t descriptor,uintptr_t renderRecord) noexcept;
    // Job brackets call these around the original body.
    JobStat* jobStats() noexcept{return jobs_;}
    // Called at job-body entry for jobs 0 (descriptor arg; collection =
    // read64(arg+0x10)-0x300) and 1 (collection arg) before the timed bracket.
    void noteOwnership(uint32_t jobId,uintptr_t arg) noexcept;
    // Called at FUN_14431AFE0 entry: rig = param_1, pose context = param_2.
    void noteRigLink(uintptr_t rig,uintptr_t poseCtx) noexcept;

    Summary summary() const noexcept;
    void writeJson(std::ostringstream& json) const;

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
    JobStat jobs_[kJobCount];

    void clearLocked();
    bool validateExecutableLocked() noexcept;
    void noteVtableLocked(uint64_t rva,uint32_t frame) noexcept;
    static bool guardedRead(uintptr_t address,void* output,size_t bytes) noexcept;
    static uint64_t read64(uintptr_t address,bool& ok) noexcept;
    static uint32_t read32(uintptr_t address,bool& ok) noexcept;
    static uint8_t read8(uintptr_t address,bool& ok) noexcept;
    uint64_t vtableRvaLocked(uintptr_t object) noexcept;
};

extern KinematicEvalProbe kinematicEvalProbe;

} // namespace edvr
