#pragma once
// Build-specific, bounded capture of the KinematicRig render-record evaluator
// (FUN_14430EFE0, RVA 0x430EFE0) and a QPC bracket around the Kinematic job
// bodies. Feeds docs/settlement-flicker-2026-09-17.md's next-flight spec:
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

    enum class HookStatus:uint32_t {NotRun,Installed,IdentityMismatch,OpcodeMismatch,InstallFailed};

    struct JobStat {
        std::atomic<uint64_t> calls{0},totalNs{0},maxNs{0};
    };

    struct RecordState {
        uint64_t record=0,node=0,poseCtx=0,predPtr=0,predVtableRva=0;
        uint64_t pred2Ptr=0,pred2VtableRva=0,hash=0,count298=0;
        uint32_t flags=0,firstFrame=0,lastFrame=0;
        uint8_t bool234=0,predByte=0,gate2=0;
        uint64_t calls=0;
    };
    struct Transition {
        uint32_t recordId=0,frame=0;
        uint32_t oldFlags=0,newFlags=0;
        uint64_t oldHash=0,newHash=0;
    };
    struct VtableUse {uint64_t rva=0;uint32_t firstFrame=0;uint64_t hits=0;};
    struct Summary {
        uint64_t observed=0;uint32_t stored=0;
        uint64_t readFaults=0,recordOverflow=0,transitionOverflow=0,vtableOverflow=0;
        uint64_t transitions=0,vtables=0;
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
