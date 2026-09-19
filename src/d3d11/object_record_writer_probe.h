#pragma once
// Build-specific, bounded capture of the CPU records which later feed the
// AtlasModel t33 upload.  The game-code hook supplies register state before
// calling the original dictionary lookup; this class only records evidence.
#include <windows.h>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

namespace edvr {

class ObjectRecordWriterProbe {
public:
    static constexpr uint32_t kExpectedTimestamp=1788384820u;
    static constexpr uint32_t kExpectedImageSize=104894464u;
    static constexpr uintptr_t kLookupRva=0x3696FA0u;
    static constexpr uint32_t kRecordBytes=0x150u;
    static constexpr uint32_t kRecordCap=65536u;
    static constexpr uint64_t kByteCap=64ull*1024*1024;
    static constexpr uint32_t kNone=~0u;

    enum class HookStatus:uint32_t {NotRun,Installed,IdentityMismatch,OpcodeMismatch,InstallFailed,Finished};
    struct Pending {uint32_t record=kNone;uint64_t sequence=0,epoch=0;};
    struct Snapshot {
        std::string status="not_applicable";
        uint64_t offset=0;
        std::vector<uint8_t> data;
    };
    struct Record {
        uint32_t id=0;uint64_t sequence=0,returnRva=0;
        uint32_t threadId=0,observedFrame=0;uint64_t completionSequence=0;
        std::string writer,contextStatus="unavailable",lookupStatus="pending";
        uint64_t owner=0,key=0,builder=0,object=0,entry=0;
        Snapshot record,keySnapshot,builderSnapshot,objectSnapshot,entrySnapshot;
    };
    struct Upload {uint32_t attempt=0,resource=0;uint64_t generation=0,cutoff=0;};
    struct Summary {
        uint64_t observed=0;uint32_t stored=0,completed=0;uint64_t retainedBytes=0;
        uint64_t recordOverflow=0,byteBudgetDeclines=0,readFaults=0;
        uint64_t contextFailures=0,unwindFailures=0,completionFailures=0;
        uint64_t declinedManagement=0,declinedUnknown=0;
    };

    bool arm(uint32_t meshFrame) noexcept;
    void armForTest(uint32_t meshFrame) noexcept;
    void finish() noexcept;
    void reset() noexcept;
    void setFrame(uint32_t meshFrame) noexcept;
    bool active() const noexcept{return active_.load(std::memory_order_acquire);}
    bool ready() const noexcept;
    void* targetAddress() const noexcept;

    Pending beginLookup(uintptr_t lookupReturnAddress,uintptr_t dictionary,
                        uintptr_t key,const CONTEXT& captured) noexcept;
    void completeLookup(Pending pending,uintptr_t entry) noexcept;
#ifdef EDVR_RECORD_WRITER_TEST
    // Exercise the production unwind from a real fixture frame while applying
    // an explicitly nominated game-return recipe.  Test executable RVAs are
    // intentionally not accepted by beginLookup itself.
    Pending beginLookupUnwindRvaForTest(uintptr_t fixtureReturn,uint64_t gameReturnRva,
                                        uintptr_t dictionary,uintptr_t key,
                                        const CONTEXT& captured) noexcept;
#endif
    Pending beginLookupRvaForTest(uint64_t returnRva,uintptr_t callerRsp,
                                  uintptr_t dictionary,uintptr_t key,
                                  const CONTEXT& caller) noexcept;
    uint64_t sampleUploadCutoff() const noexcept;
    void noteUpload(uint32_t attempt,uint32_t resource,uint64_t generation,uint64_t cutoff) noexcept;

    Summary summary() const noexcept;
    HookStatus hookStatus() const noexcept;
    const char* hookStatusText() const noexcept;
    const std::vector<Record>& recordsForTest() const noexcept{return records_;}
    const std::vector<Upload>& uploadsForTest() const noexcept{return uploads_;}
    bool writeBinary(FILE* file,uint64_t& offset,bool& binOk) noexcept;
    void writeJson(std::ostringstream& json) const;

private:
    mutable std::mutex mutex_;
    std::atomic<bool> active_{false};
    std::atomic<uint32_t> frame_{0};
    uintptr_t imageBase_=0;
    HookStatus hookStatus_=HookStatus::NotRun;
    Summary summary_{};
    uint64_t eventSequence_=0,epoch_=0;
    std::vector<Record> records_;
    std::vector<Upload> uploads_;

    void clearLocked();
    bool validateExecutableLocked() noexcept;
    Pending beginRvaLocked(uint64_t returnRva,uintptr_t callerRsp,
                           uintptr_t dictionary,uintptr_t key,const CONTEXT& caller);
    bool unwindCaller(uintptr_t returnAddress,const CONTEXT& captured,
                      CONTEXT& caller) noexcept;
    Snapshot captureLocked(uintptr_t address,size_t bytes,bool applicable);
    static bool guardedRead(uintptr_t address,void* output,size_t bytes) noexcept;
    static const char* hookStatusName(HookStatus status) noexcept;
    static const char* topStatus(const Summary& summary,HookStatus hook) noexcept;
};

extern ObjectRecordWriterProbe objectRecordWriterProbe;

} // namespace edvr
