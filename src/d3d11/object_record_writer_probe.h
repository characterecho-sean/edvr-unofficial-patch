#pragma once
// Build-specific, bounded capture of the CPU records which later feed the
// AtlasModel t33 upload.  The game-code hook supplies register state before
// calling the original dictionary lookup; this class only records evidence.
#include <windows.h>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace edvr {

class ObjectRecordWriterProbe {
public:
    static constexpr uint32_t kExpectedTimestamp=1788384820u;
    static constexpr uint32_t kExpectedImageSize=104894464u;
    static constexpr uintptr_t kLookupRva=0x3696FA0u;
    static constexpr uint32_t kRecordBytes=0x150u;
    static constexpr uint32_t kRecordCap=65536u;
    static constexpr uint32_t kOwnershipCap=8192u;
    static constexpr uint32_t kOwnershipUnwindDepth=32u;
    static constexpr uint32_t kAncestorTraceCap=32u;
    static constexpr uint32_t kAncestorTraceFrameCap=32u;
    static constexpr uint64_t kByteCap=64ull*1024*1024;
    static constexpr uint32_t kOuterBytes=0x460u;
    static constexpr uint32_t kCollectionBytes=0x2F0u;
    static constexpr uint32_t kContextBytes=0xC0u;
    static constexpr uint32_t kRecordTailBytes=0x18u;
    static constexpr uint32_t kOpaquePrefixBytes=0x80u;
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
        std::string ownershipStatus="unsupported_writer";
        uint64_t owner=0,key=0,builder=0,object=0,entry=0;
        uint32_t ownershipId=kNone;
        Snapshot record,keySnapshot,builderSnapshot,objectSnapshot,entrySnapshot;
    };
    struct PointerIdentity {
        uint64_t address=0,vtable=0,vtableRva=0;
        std::string status="null_pointer";
    };
    struct Ownership {
        uint32_t id=0,observedFrame=0,threadId=0,unwindDepth=0;
        uint64_t firstSequence=0,ancestorReturnRva=0;
        std::string status="partial",branch,writerRelation,recordStatus="not_applicable",parentStatus="read_fault";
        uint64_t writerOwner=0,outer=0,collectionOwner=0,registry=0,context=0,collectionRecord=0;
        uint64_t gameObject=0,descriptor=0,provider=0,parent=0;
        uint32_t recordIndex=0,parentToken=0;
        bool hasRecordIndex=false;
        PointerIdentity outerIdentity,gameObjectIdentity,descriptorIdentity,providerIdentity,parentIdentity;
        Snapshot outerSnapshot,collectionSnapshot,contextSnapshot,recordTailSnapshot;
        Snapshot gameObjectPrefix,descriptorPrefix,providerPrefix,parentPrefix;
    };
    struct AncestorFrame {uint64_t address=0,moduleRva=0;bool moduleRelative=false;};
    struct AncestorTrace {
        uint32_t writerRecord=0;uint64_t returnRva=0;std::string status;
        std::vector<AncestorFrame> frames;
    };
    struct Upload {uint32_t attempt=0,resource=0;uint64_t generation=0,cutoff=0;};
    struct Summary {
        uint64_t observed=0;uint32_t stored=0,completed=0;uint64_t retainedBytes=0;
        uint64_t recordOverflow=0,byteBudgetDeclines=0,readFaults=0;
        uint64_t contextFailures=0,unwindFailures=0,completionFailures=0;
        uint64_t declinedManagement=0,declinedUnknown=0;
    };
    struct OwnershipSummary {
        uint64_t attempted=0,linked=0,stored=0,deduplicated=0,unsupportedWriter=0;
        uint64_t ancestorMissing=0,unwindFailed=0,tupleReadFault=0,tupleMismatch=0;
        uint64_t registryReadFault=0,registryMismatch=0,recordRangeMismatch=0,recordReadFault=0;
        uint64_t cacheConflicts=0,recordOverflow=0,byteBudgetDeclines=0,readFaults=0;
        uint64_t opcodeMismatch=0;
        uint64_t ancestorTraces=0,ancestorTraceOverflow=0;
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
    Pending beginLookupOwnershipUnwindRvaForTest(
        uintptr_t fixtureWriterReturn,uint64_t gameWriterReturnRva,
        uintptr_t fixtureOwnerReturn,uint64_t gameOwnerReturnRva,
        uintptr_t dictionary,uintptr_t key,const CONTEXT& captured) noexcept;
    Pending beginLookupOwnershipRvaForTest(
        uint64_t writerReturnRva,uintptr_t callerRsp,uintptr_t dictionary,uintptr_t key,
        const CONTEXT& caller,uint64_t ownerReturnRva,const CONTEXT& owner,
        uint32_t unwindDepth=1) noexcept;
#endif
    Pending beginLookupRvaForTest(uint64_t returnRva,uintptr_t callerRsp,
                                  uintptr_t dictionary,uintptr_t key,
                                  const CONTEXT& caller) noexcept;
    uint64_t sampleUploadCutoff() const noexcept;
    void noteUpload(uint32_t attempt,uint32_t resource,uint64_t generation,uint64_t cutoff) noexcept;

    Summary summary() const noexcept;
    OwnershipSummary ownershipSummary() const noexcept;
    HookStatus hookStatus() const noexcept;
    const char* hookStatusText() const noexcept;
    const std::vector<Record>& recordsForTest() const noexcept{return records_;}
    const std::vector<Ownership>& ownershipsForTest() const noexcept{return ownerships_;}
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
    OwnershipSummary ownershipSummary_{};
    uint64_t eventSequence_=0,epoch_=0;
    std::vector<Record> records_;
    std::vector<Ownership> ownerships_;
    std::unordered_multimap<uint64_t,uint32_t> ownershipIndex_;
    std::vector<AncestorTrace> ancestorTraces_;
    bool ownershipOpcodesValid_=false;
    std::vector<Upload> uploads_;

    void clearLocked();
    bool validateExecutableLocked() noexcept;
    struct OwnershipCandidate {
        const char* status="ancestor_missing";
        uint64_t returnRva=0;uint32_t depth=0;CONTEXT context{};
        std::array<AncestorFrame,kAncestorTraceFrameCap> frames{};uint32_t frameCount=0;
    };
    Pending beginRvaLocked(uint64_t returnRva,uintptr_t callerRsp,
                           uintptr_t dictionary,uintptr_t key,const CONTEXT& caller,
                           const OwnershipCandidate* ownership);
    bool unwindCaller(uintptr_t returnAddress,const CONTEXT& captured,
                      CONTEXT& caller) noexcept;
    bool unwindOne(CONTEXT& context) noexcept;
    OwnershipCandidate unwindOwnership(const CONTEXT& writer,
                                       uintptr_t fixtureReturn=0,uint64_t fixtureRva=0) noexcept;
    void captureOwnershipLocked(Record& record,const OwnershipCandidate& candidate);
    void retainAncestorTraceLocked(const Record& record,const OwnershipCandidate& candidate);
    PointerIdentity identifyPointerLocked(uintptr_t address);
    Snapshot captureLocked(uintptr_t address,size_t bytes,bool applicable,bool ownership=false);
    void recountRetainedBytesLocked() noexcept;
    static bool guardedRead(uintptr_t address,void* output,size_t bytes) noexcept;
    static const char* hookStatusName(HookStatus status) noexcept;
    static const char* topStatus(const Summary& summary,HookStatus hook) noexcept;
    static const char* ownershipTopStatus(const OwnershipSummary& summary,HookStatus hook) noexcept;
};

extern ObjectRecordWriterProbe objectRecordWriterProbe;

} // namespace edvr
