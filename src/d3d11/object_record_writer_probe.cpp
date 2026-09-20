#include "object_record_writer_probe.h"
#include "object_record_writer_hook.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <iomanip>
#include <utility>

namespace edvr {
namespace {
constexpr uint64_t kManagementReturn=0x434E316u;
constexpr uint64_t kWriterReturns[]={0x369CE91u,0x42B42EFu,0x42B4ED6u,0x43130AAu,0x434D149u};
constexpr uint64_t kOwnershipDirectReturn=0x431B212u;
constexpr uint64_t kOwnershipVirtualReturn=0x431B21Fu;
constexpr uint64_t kOwnershipDirectTarget=0x4321940u;
constexpr uint64_t kOwnershipOpcodeWindowRva=0x431B200u;
constexpr size_t kOwnershipOpcodeWindowBytes=40;
struct Callsite {uintptr_t call;uint8_t bytes[5];};
constexpr Callsite kCalls[]={
    {0x369CE8Cu,{0xE8,0x0F,0xA1,0xFF,0xFF}},
    {0x42B42EAu,{0xE8,0xB1,0x2C,0x3E,0xFF}},
    {0x42B4ED1u,{0xE8,0xCA,0x20,0x3E,0xFF}},
    {0x43130A5u,{0xE8,0xF6,0x3E,0x38,0xFF}},
    {0x434D144u,{0xE8,0x57,0x9E,0x34,0xFF}},
    {0x434E311u,{0xE8,0x8A,0x8C,0x34,0xFF}},
    // The outer 42B4420 call preserves R12 for the primary writer recipe.
    {0x42B4843u,{0xE8,0xE8,0xF8,0xFF,0xFF}},
};
constexpr uint8_t kLookupPrologue[16]={0x48,0x89,0x5C,0x24,0x10,0x48,0x89,0x6C,0x24,0x18,0x56,0x57,0x41,0x56,0x48,0x83};
constexpr uint8_t kOwnershipVirtualCall[3]={0xFF,0x50,0x50};
bool ownershipOpcodeWindowMatches(uint64_t windowRva,const uint8_t* bytes,size_t byteCount) noexcept {
    constexpr uint64_t directCallRva=kOwnershipDirectReturn-5;
    constexpr uint64_t virtualCallRva=kOwnershipVirtualReturn-sizeof(kOwnershipVirtualCall);
    if(!bytes||windowRva>directCallRva||windowRva>virtualCallRva)return false;
    const uint64_t directOffset=directCallRva-windowRva,virtualOffset=virtualCallRva-windowRva;
    if(directOffset>byteCount||byteCount-directOffset<5||
       virtualOffset>byteCount||byteCount-virtualOffset<sizeof(kOwnershipVirtualCall))return false;
    if(bytes[directOffset]!=0xE8||std::memcmp(bytes+virtualOffset,kOwnershipVirtualCall,sizeof(kOwnershipVirtualCall))!=0)return false;
    int32_t displacement=0;std::memcpy(&displacement,bytes+directOffset+1,sizeof(displacement));
    const int64_t target=int64_t(windowRva+directOffset+5)+int64_t(displacement);
    return target==int64_t(kOwnershipDirectTarget);
}
bool subtract(uintptr_t value,uintptr_t amount,uintptr_t& out) noexcept {
    if(value<amount)return false;out=value-amount;return true;
}
bool add(uintptr_t value,uintptr_t amount,uintptr_t& out) noexcept {
    if(value>UINTPTR_MAX-amount)return false;out=value+amount;return true;
}
void jsonAddress(std::ostringstream& j,uint64_t value){j<<"\"0x"<<std::hex<<value<<std::dec<<'\"';}
bool ownershipWriter(uint64_t rva) noexcept {
    return rva==0x42B42EFu||rva==0x42B4ED6u||rva==0x43130AAu;
}
}

ObjectRecordWriterProbe objectRecordWriterProbe;

bool ObjectRecordWriterProbe::guardedRead(uintptr_t address,void* output,size_t bytes) noexcept {
    if(!bytes)return true;if(!address||!output||address>UINTPTR_MAX-bytes)return false;
    __try {std::memcpy(output,reinterpret_cast<const void*>(address),bytes);return true;}
    __except(EXCEPTION_EXECUTE_HANDLER){return false;}
}

void ObjectRecordWriterProbe::clearLocked() {
    summary_=Summary{};ownershipSummary_=OwnershipSummary{};eventSequence_=0;if(++epoch_==0)++epoch_;
    records_.clear();ownerships_.clear();ownershipIndex_.clear();ancestorTraces_.clear();uploads_.clear();imageBase_=0;ownershipOpcodesValid_=false;
    hookStatus_=HookStatus::NotRun;active_.store(false,std::memory_order_release);
}

bool ObjectRecordWriterProbe::validateExecutableBaseLocked(uintptr_t imageBase) noexcept {
    imageBase_=imageBase;
    IMAGE_DOS_HEADER dos{};IMAGE_NT_HEADERS nt{};
    if(!guardedRead(imageBase_,&dos,sizeof(dos))||dos.e_magic!=IMAGE_DOS_SIGNATURE||dos.e_lfanew<=0||
       !guardedRead(imageBase_+uintptr_t(dos.e_lfanew),&nt,sizeof(nt))||nt.Signature!=IMAGE_NT_SIGNATURE||
       nt.FileHeader.TimeDateStamp!=kExpectedTimestamp||nt.OptionalHeader.SizeOfImage!=kExpectedImageSize) {
        hookStatus_=HookStatus::IdentityMismatch;return false;
    }
    std::array<uint8_t,sizeof(kLookupPrologue)> prologue{};
    if(!guardedRead(imageBase_+kLookupRva,prologue.data(),prologue.size())||
       (std::memcmp(prologue.data(),kLookupPrologue,prologue.size())!=0 &&
        !objectRecordWriterHookMatches(imageBase_+kLookupRva))) {
        hookStatus_=HookStatus::OpcodeMismatch;return false;
    }
    for(const auto& c:kCalls) {
        uint8_t got[5]{};if(!guardedRead(imageBase_+c.call,got,sizeof(got))||std::memcmp(got,c.bytes,sizeof(got))!=0) {
            hookStatus_=HookStatus::OpcodeMismatch;return false;
        }
    }
    std::array<uint8_t,kOwnershipOpcodeWindowBytes> ownershipWindow{};
    ownershipOpcodesValid_=guardedRead(imageBase_+kOwnershipOpcodeWindowRva,ownershipWindow.data(),ownershipWindow.size())&&
        ownershipOpcodeWindowMatches(kOwnershipOpcodeWindowRva,ownershipWindow.data(),ownershipWindow.size());
    return true;
}

bool ObjectRecordWriterProbe::validateExecutableLocked() noexcept {
    return validateExecutableBaseLocked(reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)));
}

bool ObjectRecordWriterProbe::arm(uint32_t meshFrame) noexcept {
    active_.store(false,std::memory_order_release);detachObjectRecordWriterHook(this);
    {
        std::lock_guard<std::mutex> lock(mutex_);clearLocked();frame_.store(meshFrame,std::memory_order_relaxed);
        if(!validateExecutableLocked())return false;
        // The hook publishes this probe only after its original-call path is
        // ready.  Set the cheap gate first so that first published call cannot
        // be mistaken for a disabled one.
        active_.store(true,std::memory_order_release);
    }
    const char* result=attachObjectRecordWriterHook(this);
    std::lock_guard<std::mutex> lock(mutex_);
    if(result&&std::strcmp(result,"installed")==0){hookStatus_=HookStatus::Installed;return true;}
    active_.store(false,std::memory_order_release);
    if(result&&std::strcmp(result,"identity_mismatch")==0)hookStatus_=HookStatus::IdentityMismatch;
    else if(result&&std::strcmp(result,"opcode_mismatch")==0)hookStatus_=HookStatus::OpcodeMismatch;
    else hookStatus_=HookStatus::InstallFailed;
    return false;
}
void ObjectRecordWriterProbe::armForTest(uint32_t meshFrame) noexcept {
    active_.store(false,std::memory_order_release);detachObjectRecordWriterHook(this);
    std::lock_guard<std::mutex> lock(mutex_);clearLocked();frame_.store(meshFrame,std::memory_order_relaxed);
    imageBase_=reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));hookStatus_=HookStatus::Installed;
    ownershipOpcodesValid_=true;
    active_.store(true,std::memory_order_release);
}
void ObjectRecordWriterProbe::finish() noexcept {
    active_.store(false,std::memory_order_release);detachObjectRecordWriterHook(this);
    std::lock_guard<std::mutex> lock(mutex_);if(hookStatus_==HookStatus::Installed)hookStatus_=HookStatus::Finished;
}
void ObjectRecordWriterProbe::reset() noexcept {
    active_.store(false,std::memory_order_release);detachObjectRecordWriterHook(this);
    std::lock_guard<std::mutex> lock(mutex_);clearLocked();
}
void ObjectRecordWriterProbe::setFrame(uint32_t meshFrame) noexcept {frame_.store(meshFrame,std::memory_order_relaxed);}
bool ObjectRecordWriterProbe::ready() const noexcept {std::lock_guard<std::mutex> lock(mutex_);return imageBase_&&hookStatus_==HookStatus::NotRun;}
void* ObjectRecordWriterProbe::targetAddress() const noexcept {std::lock_guard<std::mutex> lock(mutex_);return imageBase_?reinterpret_cast<void*>(imageBase_+kLookupRva):nullptr;}

ObjectRecordWriterProbe::Snapshot ObjectRecordWriterProbe::captureLocked(uintptr_t address,size_t bytes,bool applicable,bool ownership) {
    Snapshot out;if(!applicable){out.status="not_applicable";return out;}if(!address){out.status="null_pointer";return out;}
    if(bytes>kByteCap-summary_.retainedBytes){out.status="byte_budget";if(ownership)++ownershipSummary_.byteBudgetDeclines;else ++summary_.byteBudgetDeclines;return out;}
    out.data.resize(bytes);if(!guardedRead(address,out.data.data(),bytes)){out.data.clear();out.status="read_fault";if(ownership)++ownershipSummary_.readFaults;else ++summary_.readFaults;return out;}
    out.status="available";summary_.retainedBytes+=bytes;return out;
}

bool ObjectRecordWriterProbe::unwindOne(CONTEXT& c) noexcept {
    const DWORD64 oldRip=c.Rip,oldRsp=c.Rsp;DWORD64 base=0;PRUNTIME_FUNCTION function=nullptr;
    __try {function=RtlLookupFunctionEntry(c.Rip,&base,nullptr);}
    __except(EXCEPTION_EXECUTE_HANDLER){return false;}
    if(function) {
        DWORD64 establisher=0;PVOID handlerData=nullptr;
        __try {RtlVirtualUnwind(UNW_FLAG_NHANDLER,base,c.Rip,function,&c,&handlerData,&establisher,nullptr);}
        __except(EXCEPTION_EXECUTE_HANDLER){return false;}
    } else {
        uintptr_t next=0;if(!guardedRead(uintptr_t(c.Rsp),&next,sizeof(next))||c.Rsp>UINTPTR_MAX-sizeof(uintptr_t))return false;
        c.Rip=next;c.Rsp+=sizeof(uintptr_t);
    }
    return c.Rsp>oldRsp&&c.Rip!=oldRip;
}

bool ObjectRecordWriterProbe::unwindCaller(uintptr_t returnAddress,const CONTEXT& captured,CONTEXT& caller) noexcept {
    caller=captured;
    for(uint32_t frame=0;frame<16;++frame) {
        if(caller.Rip==returnAddress)return true;
        if(!unwindOne(caller))return false;
    }
    return false;
}

ObjectRecordWriterProbe::OwnershipCandidate ObjectRecordWriterProbe::unwindOwnership(
    const CONTEXT& writer,uintptr_t fixtureReturn,uint64_t fixtureRva) noexcept {
    OwnershipCandidate out;if(!ownershipOpcodesValid_){out.status="opcode_mismatch";return out;}CONTEXT c=writer;
    for(uint32_t depth=1;depth<=kOwnershipUnwindDepth;++depth) {
        if(!unwindOne(c)){out.status="unwind_failed";return out;}
        AncestorFrame frame;frame.address=c.Rip;
        if(c.Rip>=imageBase_&&c.Rip<imageBase_+kExpectedImageSize){frame.moduleRelative=true;frame.moduleRva=c.Rip-imageBase_;}
        if(out.frameCount<out.frames.size())out.frames[out.frameCount++]=frame;
        uint64_t matched=0;
        if(fixtureReturn) {if(c.Rip==fixtureReturn)matched=fixtureRva;}
        else if(c.Rip==imageBase_+kOwnershipDirectReturn)matched=kOwnershipDirectReturn;
        else if(c.Rip==imageBase_+kOwnershipVirtualReturn)matched=kOwnershipVirtualReturn;
        if(matched){out.status="matched";out.returnRva=matched;out.depth=depth;out.context=c;return out;}
    }
    out.status="ancestor_missing";return out;
}

ObjectRecordWriterProbe::Pending ObjectRecordWriterProbe::beginLookup(
    uintptr_t returnAddress,uintptr_t dictionary,uintptr_t key,const CONTEXT& captured) noexcept {
    if(!active_.load(std::memory_order_acquire))return {};
    CONTEXT caller{};if(!unwindCaller(returnAddress,captured,caller)) {
        std::lock_guard<std::mutex> lock(mutex_);++summary_.unwindFailures;++summary_.contextFailures;return {};
    }
    if(returnAddress<imageBase_){std::lock_guard<std::mutex> lock(mutex_);++summary_.declinedUnknown;return {};}
    const uint64_t rva=uint64_t(returnAddress-imageBase_);
    OwnershipCandidate ownership;if(ownershipWriter(rva))ownership=unwindOwnership(caller);
    std::lock_guard<std::mutex> lock(mutex_);if(!active_.load(std::memory_order_relaxed))return {};
    try {return beginRvaLocked(rva,uintptr_t(caller.Rsp),dictionary,key,caller,ownershipWriter(rva)?&ownership:nullptr);}
    catch(...) {++summary_.recordOverflow;recountRetainedBytesLocked();return {};}
}
#ifdef EDVR_RECORD_WRITER_TEST
bool ObjectRecordWriterProbe::ownershipOpcodeWindowMatchesForTest(
    uint64_t windowRva,const uint8_t* bytes,size_t byteCount) noexcept {
    return ownershipOpcodeWindowMatches(windowRva,bytes,byteCount);
}
bool ObjectRecordWriterProbe::executableOpcodesMatchAtBaseForTest(uintptr_t imageBase) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);clearLocked();
    return validateExecutableBaseLocked(imageBase)&&ownershipOpcodesValid_;
}
void ObjectRecordWriterProbe::setOwnershipOpcodesValidForTest(bool valid) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);ownershipOpcodesValid_=valid;
}
ObjectRecordWriterProbe::Pending ObjectRecordWriterProbe::beginLookupUnwindRvaForTest(
    uintptr_t fixtureReturn,uint64_t gameReturnRva,uintptr_t dictionary,
    uintptr_t key,const CONTEXT& captured) noexcept {
    if(!active_.load(std::memory_order_acquire))return {};
    CONTEXT caller{};if(!unwindCaller(fixtureReturn,captured,caller)) {
        std::lock_guard<std::mutex> lock(mutex_);++summary_.unwindFailures;++summary_.contextFailures;return {};
    }
    std::lock_guard<std::mutex> lock(mutex_);if(!active_.load(std::memory_order_relaxed))return {};
    try {return beginRvaLocked(gameReturnRva,uintptr_t(caller.Rsp),dictionary,key,caller,nullptr);}
    catch(...) {++summary_.recordOverflow;recountRetainedBytesLocked();return {};}
}

ObjectRecordWriterProbe::Pending ObjectRecordWriterProbe::beginLookupOwnershipUnwindRvaForTest(
    uintptr_t fixtureWriterReturn,uint64_t gameWriterReturnRva,
    uintptr_t fixtureOwnerReturn,uint64_t gameOwnerReturnRva,
    uintptr_t dictionary,uintptr_t key,const CONTEXT& captured) noexcept {
    if(!active_.load(std::memory_order_acquire))return {};
    CONTEXT caller{};if(!unwindCaller(fixtureWriterReturn,captured,caller)) {
        std::lock_guard<std::mutex> lock(mutex_);++summary_.unwindFailures;++summary_.contextFailures;return {};
    }
    OwnershipCandidate ownership=unwindOwnership(caller,fixtureOwnerReturn,gameOwnerReturnRva);
    std::lock_guard<std::mutex> lock(mutex_);if(!active_.load(std::memory_order_relaxed))return {};
    try {return beginRvaLocked(gameWriterReturnRva,uintptr_t(caller.Rsp),dictionary,key,caller,&ownership);}
    catch(...) {++summary_.recordOverflow;recountRetainedBytesLocked();return {};}
}

ObjectRecordWriterProbe::Pending ObjectRecordWriterProbe::beginLookupOwnershipRvaForTest(
    uint64_t writerReturnRva,uintptr_t callerRsp,uintptr_t dictionary,uintptr_t key,
    const CONTEXT& caller,uint64_t ownerReturnRva,const CONTEXT& owner,uint32_t unwindDepth) noexcept {
    if(!active_.load(std::memory_order_acquire))return {};
    OwnershipCandidate ownership;ownership.status="matched";ownership.returnRva=ownerReturnRva;
    ownership.depth=unwindDepth;ownership.context=owner;
    std::lock_guard<std::mutex> lock(mutex_);
    try {return beginRvaLocked(writerReturnRva,callerRsp,dictionary,key,caller,&ownership);}
    catch(...) {++summary_.recordOverflow;recountRetainedBytesLocked();return {};}
}
#endif
ObjectRecordWriterProbe::Pending ObjectRecordWriterProbe::beginLookupRvaForTest(
    uint64_t returnRva,uintptr_t callerRsp,uintptr_t dictionary,uintptr_t key,const CONTEXT& caller) noexcept {
    if(!active_.load(std::memory_order_acquire))return {};
    std::lock_guard<std::mutex> lock(mutex_);try{return beginRvaLocked(returnRva,callerRsp,dictionary,key,caller,nullptr);}
    catch(...) {++summary_.recordOverflow;recountRetainedBytesLocked();return {};}
}

ObjectRecordWriterProbe::Pending ObjectRecordWriterProbe::beginRvaLocked(
    uint64_t rva,uintptr_t callerRsp,uintptr_t dictionary,uintptr_t key,const CONTEXT& c,
    const OwnershipCandidate* ownership) {
    if(rva==kManagementReturn){++summary_.declinedManagement;return {};}
    bool known=false;for(auto value:kWriterReturns)known=known||rva==value;if(!known){++summary_.declinedUnknown;return {};}
    ++summary_.observed;if(records_.size()>=kRecordCap){++summary_.recordOverflow;return {};}
    Record r;r.id=uint32_t(records_.size());r.sequence=++eventSequence_;r.returnRva=rva;r.threadId=GetCurrentThreadId();
    r.observedFrame=frame_.load(std::memory_order_relaxed);r.key=key;
    uintptr_t owner=0,record=0;bool contextOk=subtract(dictionary,0x260,owner);r.owner=owner;
    size_t builderBytes=0,objectBytes=0;
    switch(rva) {
    case 0x369CE91u:r.writer="direct_369ce91";contextOk=add(callerRsp,0x50,record)&&contextOk;break;
    case 0x42B42EFu: {
        r.writer="primary_42b42ef";uintptr_t builder=0;
        contextOk=add(callerRsp,0x30,record)&&subtract(key,0x40,builder)&&contextOk;
        r.builder=builder;r.object=uintptr_t(c.R12);builderBytes=0x60;objectBytes=kContextBytes;break;
    }
    case 0x42B4ED6u: {
        r.writer="inline_42b4ed6";contextOk=add(uintptr_t(c.Rbp),0x1C0,record)&&contextOk;uintptr_t objectAt=0;
        if(subtract(uintptr_t(c.Rbp),0x68,objectAt))contextOk=guardedRead(objectAt,&r.object,sizeof(r.object))&&contextOk;else contextOk=false;
        objectBytes=kContextBytes;break;
    }
    case 0x43130AAu:r.writer="direct_43130aa";contextOk=add(callerRsp,0x60,record)&&contextOk;break;
    case 0x434D149u:r.writer="helper_434d149";record=uintptr_t(c.Rbx);r.object=uintptr_t(c.Rdi);objectBytes=0x1A0;break;
    }
    if(!contextOk||!record){++summary_.contextFailures;r.contextStatus="unavailable";}
    r.record=captureLocked(record,kRecordBytes,contextOk&&record);
    r.keySnapshot=captureLocked(uintptr_t(r.key),0x20,contextOk);
    r.builderSnapshot=captureLocked(uintptr_t(r.builder),builderBytes,builderBytes!=0);
    r.objectSnapshot=captureLocked(uintptr_t(r.object),objectBytes,objectBytes!=0);
    r.entrySnapshot.status="not_applicable";
    if(contextOk)r.contextStatus="partial"; // Entry bytes arrive after the original lookup returns.
    const size_t oldOwnerships=ownerships_.size(),oldTraces=ancestorTraces_.size();
    const OwnershipSummary oldOwnershipSummary=ownershipSummary_;
    try {
        if(ownershipWriter(rva)) {
            if(ownership)captureOwnershipLocked(r,*ownership);
            else {r.ownershipStatus="ancestor_missing";++ownershipSummary_.attempted;++ownershipSummary_.ancestorMissing;}
        } else {r.ownershipStatus="unsupported_writer";++ownershipSummary_.unsupportedWriter;}
        const Pending pending{r.id,r.sequence,epoch_};records_.push_back(std::move(r));summary_.stored=uint32_t(records_.size());return pending;
    } catch(...) {
        for(auto it=ownershipIndex_.begin();it!=ownershipIndex_.end();) {
            if(it->second>=oldOwnerships)it=ownershipIndex_.erase(it);else ++it;
        }
        if(ownerships_.size()>oldOwnerships)ownerships_.resize(oldOwnerships);
        if(ancestorTraces_.size()>oldTraces)ancestorTraces_.resize(oldTraces);
        ownershipSummary_=oldOwnershipSummary;recountRetainedBytesLocked();throw;
    }
}

ObjectRecordWriterProbe::PointerIdentity ObjectRecordWriterProbe::identifyPointerLocked(uintptr_t address) {
    PointerIdentity out;out.address=address;if(!address)return out;
    uintptr_t vtable=0;if(!guardedRead(address,&vtable,sizeof(vtable))){out.status="read_fault";++ownershipSummary_.readFaults;return out;}
    out.vtable=vtable;
    if(vtable>=imageBase_&&vtable<imageBase_+kExpectedImageSize){out.status="module_relative";out.vtableRva=vtable-imageBase_;}
    else out.status="outside_image";
    return out;
}

void ObjectRecordWriterProbe::retainAncestorTraceLocked(const Record& record,const OwnershipCandidate& candidate) {
    std::vector<AncestorFrame> frames(candidate.frames.begin(),candidate.frames.begin()+candidate.frameCount);
    for(const auto& trace:ancestorTraces_) {
        if(trace.status!=candidate.status||trace.returnRva!=record.returnRva||trace.frames.size()!=frames.size())continue;
        bool same=true;for(size_t i=0;i<frames.size();++i)
            same=same&&trace.frames[i].address==frames[i].address&&trace.frames[i].moduleRva==frames[i].moduleRva&&trace.frames[i].moduleRelative==frames[i].moduleRelative;
        if(same)return;
    }
    if(ancestorTraces_.size()>=kAncestorTraceCap){++ownershipSummary_.ancestorTraceOverflow;return;}
    ancestorTraces_.push_back({record.id,record.returnRva,candidate.status,std::move(frames)});
    ownershipSummary_.ancestorTraces=ancestorTraces_.size();
}

void ObjectRecordWriterProbe::captureOwnershipLocked(Record& r,const OwnershipCandidate& candidate) {
    const uint64_t retainedBeforeOwnership=summary_.retainedBytes;
    ++ownershipSummary_.attempted;
    if(std::strcmp(candidate.status,"matched")!=0) {
        r.ownershipStatus=candidate.status;
        if(std::strcmp(candidate.status,"opcode_mismatch")==0)++ownershipSummary_.opcodeMismatch;
        else if(std::strcmp(candidate.status,"unwind_failed")==0)++ownershipSummary_.unwindFailed;
        else ++ownershipSummary_.ancestorMissing;
        if(std::strcmp(candidate.status,"unwind_failed")==0||std::strcmp(candidate.status,"ancestor_missing")==0)retainAncestorTraceLocked(r,candidate);
        return;
    }
    Ownership o;o.firstSequence=r.sequence;o.observedFrame=r.observedFrame;o.threadId=r.threadId;
    o.unwindDepth=candidate.depth;o.ancestorReturnRva=candidate.returnRva;
    if(candidate.returnRva==kOwnershipDirectReturn)o.branch="direct_4321940";
    else if(candidate.returnRva==kOwnershipVirtualReturn)o.branch="virtual_50";
    else {r.ownershipStatus="ancestor_missing";++ownershipSummary_.ancestorMissing;retainAncestorTraceLocked(r,candidate);return;}
    o.writerOwner=r.owner;o.outer=uintptr_t(candidate.context.Rdi);o.collectionOwner=uintptr_t(candidate.context.Rbx);
    uintptr_t tupleCollection=0;
    if(!add(uintptr_t(o.outer),0x348,tupleCollection)||!guardedRead(tupleCollection,&tupleCollection,sizeof(tupleCollection))) {
        r.ownershipStatus="tuple_read_fault";++ownershipSummary_.tupleReadFault;return;
    }
    if(tupleCollection!=o.collectionOwner){r.ownershipStatus="tuple_mismatch";++ownershipSummary_.tupleMismatch;return;}
    uintptr_t registryAt=0;
    if(!add(uintptr_t(o.collectionOwner),0x2E0,registryAt)||!guardedRead(registryAt,&o.registry,sizeof(o.registry))) {
        r.ownershipStatus="registry_read_fault";++ownershipSummary_.registryReadFault;return;
    }
    uintptr_t contextRegistryAt=0,contextRegistry=0;
    if(r.returnRva==0x42B42EFu||r.returnRva==0x42B4ED6u) {
        o.writerRelation="inline_registry_plus_78";o.context=r.object;
        uintptr_t expectedRegistry=0;
        if(!subtract(uintptr_t(r.owner),0x78,expectedRegistry)||!add(uintptr_t(o.context),0x30,contextRegistryAt)||
           !guardedRead(contextRegistryAt,&contextRegistry,sizeof(contextRegistry))) {
            r.ownershipStatus="registry_read_fault";++ownershipSummary_.registryReadFault;return;
        }
        if(expectedRegistry!=o.registry||contextRegistry!=o.registry){r.ownershipStatus="registry_mismatch";++ownershipSummary_.registryMismatch;return;}
    } else if(r.returnRva==0x43130AAu) {
        o.writerRelation="direct_collection_plus_300";uintptr_t expectedCollection=0;
        if(!subtract(uintptr_t(r.owner),0x300,expectedCollection)||expectedCollection!=o.collectionOwner) {
            r.ownershipStatus="tuple_mismatch";++ownershipSummary_.tupleMismatch;return;
        }
        uintptr_t baseAt=0,countAt=0,base=0,collectionRecord=0;uint64_t count=0;
        if(!subtract(uintptr_t(r.key),0x250,collectionRecord)||
           !add(uintptr_t(o.collectionOwner),0x280,baseAt)||!add(uintptr_t(o.collectionOwner),0x298,countAt)||
           !guardedRead(baseAt,&base,sizeof(base))||!guardedRead(countAt,&count,sizeof(count))) {
            r.ownershipStatus="record_read_fault";++ownershipSummary_.recordReadFault;return;
        }
        o.collectionRecord=collectionRecord;
        const uintptr_t record=uintptr_t(o.collectionRecord);
        if(!count||count>(UINTPTR_MAX-base)/0x2F0u||record<base||record>=base+uintptr_t(count)*0x2F0u||
           (record-base)%0x2F0u!=0) {
            r.ownershipStatus="record_range_mismatch";++ownershipSummary_.recordRangeMismatch;return;
        }
        const uint64_t index=(record-base)/0x2F0u;
        if(index>UINT32_MAX){r.ownershipStatus="record_range_mismatch";++ownershipSummary_.recordRangeMismatch;return;}
        o.recordIndex=uint32_t(index);o.hasRecordIndex=true;o.recordStatus="validated";
        uintptr_t contextAt=0;
        if(!add(record,0x290,contextAt)||!guardedRead(contextAt,&o.context,sizeof(o.context))) {
            r.ownershipStatus="record_read_fault";++ownershipSummary_.recordReadFault;return;
        }
        if(o.context) {
            if(!add(uintptr_t(o.context),0x30,contextRegistryAt)||!guardedRead(contextRegistryAt,&contextRegistry,sizeof(contextRegistry))) {
                r.ownershipStatus="registry_read_fault";++ownershipSummary_.registryReadFault;return;
            }
            if(contextRegistry!=o.registry){r.ownershipStatus="registry_mismatch";++ownershipSummary_.registryMismatch;return;}
        }
    } else {r.ownershipStatus="unsupported_writer";++ownershipSummary_.unsupportedWriter;--ownershipSummary_.attempted;return;}

    bool metadataOk=true;
    auto readField=[&](uintptr_t offset,void* value,size_t bytes){uintptr_t at=0;const bool ok=add(uintptr_t(o.outer),offset,at)&&guardedRead(at,value,bytes);if(!ok){metadataOk=false;++ownershipSummary_.readFaults;}return ok;};
    readField(0x20,&o.gameObject,sizeof(o.gameObject));readField(0x50,&o.descriptor,sizeof(o.descriptor));
    readField(0x180,&o.provider,sizeof(o.provider));const bool tokenOk=readField(0x1B8,&o.parentToken,sizeof(o.parentToken));
    const bool parentOk=readField(0x1C0,&o.parent,sizeof(o.parent));
    if(!tokenOk||!parentOk)o.parentStatus="read_fault";
    else if(o.parentToken!=UINT32_MAX)o.parentStatus="unresolved";
    else if(!o.parent)o.parentStatus="resolved_absent";
    else o.parentStatus="available";

    o.outerIdentity=identifyPointerLocked(uintptr_t(o.outer));o.gameObjectIdentity=identifyPointerLocked(uintptr_t(o.gameObject));
    o.descriptorIdentity=identifyPointerLocked(uintptr_t(o.descriptor));o.providerIdentity=identifyPointerLocked(uintptr_t(o.provider));
    o.parentIdentity=identifyPointerLocked(uintptr_t(o.parent));

    uint64_t hash=1469598103934665603ull;
    auto mix=[&](uint64_t value){hash^=value;hash*=1099511628211ull;};
    for(uint64_t value:{o.writerOwner,uint64_t(o.observedFrame),uint64_t(o.threadId),o.ancestorReturnRva,o.outer,o.collectionOwner,o.registry,o.context,o.collectionRecord,o.gameObject,o.descriptor,o.provider,o.parent,uint64_t(o.parentToken)})mix(value);
    const auto range=ownershipIndex_.equal_range(hash);
    for(auto it=range.first;it!=range.second;++it) {
        const Ownership& x=ownerships_[it->second];
        if(x.writerOwner==o.writerOwner&&x.observedFrame==o.observedFrame&&x.threadId==o.threadId&&x.ancestorReturnRva==o.ancestorReturnRva&&
           x.outer==o.outer&&x.collectionOwner==o.collectionOwner&&x.registry==o.registry&&x.context==o.context&&x.collectionRecord==o.collectionRecord&&
           x.gameObject==o.gameObject&&x.descriptor==o.descriptor&&x.provider==o.provider&&x.parent==o.parent&&x.parentToken==o.parentToken) {
            r.ownershipId=x.id;r.ownershipStatus="linked";++ownershipSummary_.linked;++ownershipSummary_.deduplicated;return;
        }
    }
    if(ownerships_.size()>=kOwnershipCap){r.ownershipStatus="record_overflow";++ownershipSummary_.recordOverflow;return;}

    o.outerSnapshot=captureLocked(uintptr_t(o.outer),kOuterBytes,true,true);
    o.collectionSnapshot=captureLocked(uintptr_t(o.collectionOwner),kCollectionBytes,true,true);
    o.contextSnapshot=captureLocked(uintptr_t(o.context),kContextBytes,true,true);
    o.recordTailSnapshot=captureLocked(uintptr_t(o.collectionRecord)+0x280,kRecordTailBytes,o.hasRecordIndex,true);
    o.gameObjectPrefix=captureLocked(uintptr_t(o.gameObject),kOpaquePrefixBytes,true,true);
    o.descriptorPrefix=captureLocked(uintptr_t(o.descriptor),kOpaquePrefixBytes,true,true);
    o.providerPrefix=captureLocked(uintptr_t(o.provider),kOpaquePrefixBytes,true,true);
    const bool parentApplicable=o.parentStatus=="available";
    o.parentPrefix=captureLocked(uintptr_t(o.parent),kOpaquePrefixBytes,parentApplicable,true);
    auto bytesEqual=[&](const Snapshot& s,size_t at,const void* value,size_t bytes){return s.status!="available"||(at+bytes<=s.data.size()&&std::memcmp(s.data.data()+at,value,bytes)==0);};
    bool stable=bytesEqual(o.outerSnapshot,0x348,&o.collectionOwner,sizeof(o.collectionOwner))&&
        bytesEqual(o.outerSnapshot,0x20,&o.gameObject,sizeof(o.gameObject))&&bytesEqual(o.outerSnapshot,0x50,&o.descriptor,sizeof(o.descriptor))&&
        bytesEqual(o.outerSnapshot,0x180,&o.provider,sizeof(o.provider))&&bytesEqual(o.outerSnapshot,0x1B8,&o.parentToken,sizeof(o.parentToken))&&
        bytesEqual(o.outerSnapshot,0x1C0,&o.parent,sizeof(o.parent))&&bytesEqual(o.collectionSnapshot,0x2E0,&o.registry,sizeof(o.registry))&&
        bytesEqual(o.contextSnapshot,0x30,&o.registry,sizeof(o.registry));
    if(o.hasRecordIndex)stable=stable&&bytesEqual(o.recordTailSnapshot,0x10,&o.context,sizeof(o.context));
    for(const auto& pair:{std::pair<const Snapshot*,const PointerIdentity*>(&o.gameObjectPrefix,&o.gameObjectIdentity),
                         {&o.descriptorPrefix,&o.descriptorIdentity},{&o.providerPrefix,&o.providerIdentity},{&o.parentPrefix,&o.parentIdentity}})
        if(pair.first->status=="available")stable=stable&&bytesEqual(*pair.first,0,&pair.second->vtable,sizeof(pair.second->vtable));
    if(!stable){r.ownershipStatus="cache_conflict";++ownershipSummary_.cacheConflicts;summary_.retainedBytes=retainedBeforeOwnership;return;}
    const auto complete=[](const Snapshot& s){return s.status=="available"||s.status=="not_applicable"||s.status=="null_pointer";};
    const bool identityOk=o.outerIdentity.status!="read_fault"&&o.gameObjectIdentity.status!="read_fault"&&o.descriptorIdentity.status!="read_fault"&&o.providerIdentity.status!="read_fault"&&o.parentIdentity.status!="read_fault";
    o.status=metadataOk&&identityOk&&complete(o.outerSnapshot)&&complete(o.collectionSnapshot)&&complete(o.contextSnapshot)&&
        complete(o.recordTailSnapshot)&&complete(o.gameObjectPrefix)&&complete(o.descriptorPrefix)&&complete(o.providerPrefix)&&complete(o.parentPrefix)?"captured":"partial";
    o.id=uint32_t(ownerships_.size());r.ownershipId=o.id;r.ownershipStatus="linked";
    ownerships_.push_back(std::move(o));ownershipIndex_.emplace(hash,r.ownershipId);
    ++ownershipSummary_.linked;ownershipSummary_.stored=ownerships_.size();
}

void ObjectRecordWriterProbe::recountRetainedBytesLocked() noexcept {
    summary_.retainedBytes=0;
    for(const auto& r:records_)for(const Snapshot* s:{&r.record,&r.keySnapshot,&r.builderSnapshot,&r.objectSnapshot,&r.entrySnapshot})summary_.retainedBytes+=s->data.size();
    for(const auto& o:ownerships_)for(const Snapshot* s:{&o.outerSnapshot,&o.collectionSnapshot,&o.contextSnapshot,&o.recordTailSnapshot,
        &o.gameObjectPrefix,&o.descriptorPrefix,&o.providerPrefix,&o.parentPrefix})summary_.retainedBytes+=s->data.size();
}

void ObjectRecordWriterProbe::completeLookup(Pending pending,uintptr_t entry) noexcept {
    if(pending.record==kNone)return;std::lock_guard<std::mutex> lock(mutex_);
    if(pending.epoch!=epoch_||pending.record>=records_.size()||records_[pending.record].sequence!=pending.sequence){++summary_.completionFailures;return;}
    Record& r=records_[pending.record];r.entry=entry;r.completionSequence=++eventSequence_;
    try {r.entrySnapshot=captureLocked(entry,0x38,true);}catch(...){r.entrySnapshot.data.clear();r.entrySnapshot.status="byte_budget";++summary_.byteBudgetDeclines;}
    ++summary_.completed;
    const auto available=[](const Snapshot& x){return x.status=="available"||x.status=="not_applicable";};
    if(r.contextStatus!="unavailable")r.contextStatus=available(r.record)&&available(r.keySnapshot)&&available(r.builderSnapshot)&&available(r.objectSnapshot)&&available(r.entrySnapshot)?"complete":"partial";
    if(r.entrySnapshot.status=="available")r.lookupStatus="complete";else {r.lookupStatus="completion_failed";++summary_.completionFailures;}
}

uint64_t ObjectRecordWriterProbe::sampleUploadCutoff() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);return eventSequence_;
}
void ObjectRecordWriterProbe::noteUpload(uint32_t attempt,uint32_t resource,uint64_t generation,uint64_t cutoff) noexcept {
    if(attempt==kNone)return;std::lock_guard<std::mutex> lock(mutex_);
    try {uploads_.push_back({attempt,resource,generation,cutoff});}catch(...){++summary_.contextFailures;}
}
ObjectRecordWriterProbe::Summary ObjectRecordWriterProbe::summary() const noexcept {std::lock_guard<std::mutex> lock(mutex_);return summary_;}
ObjectRecordWriterProbe::OwnershipSummary ObjectRecordWriterProbe::ownershipSummary() const noexcept {std::lock_guard<std::mutex> lock(mutex_);return ownershipSummary_;}
ObjectRecordWriterProbe::HookStatus ObjectRecordWriterProbe::hookStatus() const noexcept {std::lock_guard<std::mutex> lock(mutex_);return hookStatus_;}
const char* ObjectRecordWriterProbe::hookStatusText() const noexcept {std::lock_guard<std::mutex> lock(mutex_);return hookStatusName(hookStatus_);}

bool ObjectRecordWriterProbe::writeBinary(FILE* file,uint64_t& offset,bool& binOk) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    for(auto& r:records_)for(Snapshot* s:{&r.record,&r.keySnapshot,&r.builderSnapshot,&r.objectSnapshot,&r.entrySnapshot}) {
        s->offset=offset;if(s->status!="available")continue;
        if(!file||!binOk){s->status=file?"bin_write_failed":"bin_open_failed";s->data.clear();continue;}
        if(fwrite(s->data.data(),1,s->data.size(),file)!=s->data.size()){s->status="bin_write_failed";s->data.clear();binOk=false;continue;}
        offset+=s->data.size();
    }
    for(auto& o:ownerships_)for(Snapshot* s:{&o.outerSnapshot,&o.collectionSnapshot,&o.contextSnapshot,&o.recordTailSnapshot,
        &o.gameObjectPrefix,&o.descriptorPrefix,&o.providerPrefix,&o.parentPrefix}) {
        s->offset=offset;if(s->status!="available")continue;
        if(!file||!binOk){s->status=file?"bin_write_failed":"bin_open_failed";s->data.clear();o.status="partial";continue;}
        if(fwrite(s->data.data(),1,s->data.size(),file)!=s->data.size()){s->status="bin_write_failed";s->data.clear();o.status="partial";binOk=false;continue;}
        offset+=s->data.size();
    }
    recountRetainedBytesLocked();
    return binOk;
}

const char* ObjectRecordWriterProbe::hookStatusName(HookStatus x) noexcept {
    switch(x){case HookStatus::Installed:return "installed";case HookStatus::IdentityMismatch:return "identity_mismatch";case HookStatus::OpcodeMismatch:return "opcode_mismatch";case HookStatus::InstallFailed:return "install_failed";case HookStatus::Finished:return "finished";default:return "not_run";}
}
const char* ObjectRecordWriterProbe::topStatus(const Summary& s,HookStatus h) noexcept {
    if(h==HookStatus::NotRun)return "not_run";
    if(h==HookStatus::IdentityMismatch||h==HookStatus::OpcodeMismatch||h==HookStatus::InstallFailed)return "unavailable";
    if(s.stored&&s.observed==s.stored&&s.completed==s.stored&&!s.recordOverflow&&!s.byteBudgetDeclines&&!s.readFaults&&!s.contextFailures&&!s.unwindFailures&&!s.completionFailures&&!s.declinedUnknown)return "captured";
    return "partial";
}
const char* ObjectRecordWriterProbe::ownershipTopStatus(const OwnershipSummary& s,HookStatus h) noexcept {
    if(h==HookStatus::NotRun)return "not_run";
    if(h==HookStatus::IdentityMismatch||h==HookStatus::OpcodeMismatch||h==HookStatus::InstallFailed)return "unavailable";
    if(s.attempted&&s.linked==s.attempted&&!s.ancestorMissing&&!s.unwindFailed&&!s.tupleReadFault&&!s.tupleMismatch&&
       !s.registryReadFault&&!s.registryMismatch&&!s.recordRangeMismatch&&!s.recordReadFault&&!s.cacheConflicts&&
       !s.recordOverflow&&!s.byteBudgetDeclines&&!s.readFaults&&!s.opcodeMismatch)return "captured";
    return "partial";
}
void ObjectRecordWriterProbe::writeJson(std::ostringstream& j) const {
    std::lock_guard<std::mutex> lock(mutex_);j<<"  \"record_writers\":{\"version\":2,\"status\":\""<<topStatus(summary_,hookStatus_)<<"\",\"hook_status\":\""<<hookStatusName(hookStatus_)
      <<"\",\"ownership_status\":\""<<ownershipTopStatus(ownershipSummary_,hookStatus_)<<"\",\"limits\":{\"records\":"<<kRecordCap<<",\"bytes\":"<<kByteCap
      <<",\"ownership_records\":"<<kOwnershipCap<<",\"ownership_unwind_depth\":"<<kOwnershipUnwindDepth<<",\"ownership_traces\":"<<kAncestorTraceCap
      <<",\"ownership_trace_frames\":"<<kAncestorTraceFrameCap<<",\"ownership_outer_bytes\":"<<kOuterBytes<<",\"ownership_collection_bytes\":"<<kCollectionBytes
      <<",\"ownership_context_bytes\":"<<kContextBytes<<",\"ownership_record_tail_bytes\":"<<kRecordTailBytes<<",\"ownership_opaque_prefix_bytes\":"<<kOpaquePrefixBytes
      <<"},\"summary\":{\"observed\":"<<summary_.observed<<",\"stored\":"<<summary_.stored<<",\"completed\":"<<summary_.completed
      <<",\"retained_bytes\":"<<summary_.retainedBytes<<",\"record_overflow\":"<<summary_.recordOverflow<<",\"byte_budget_declines\":"<<summary_.byteBudgetDeclines
      <<",\"read_faults\":"<<summary_.readFaults<<",\"context_failures\":"<<summary_.contextFailures<<",\"unwind_failures\":"<<summary_.unwindFailures<<",\"completion_failures\":"<<summary_.completionFailures<<",\"declined_management\":"<<summary_.declinedManagement
      <<",\"declined_unknown\":"<<summary_.declinedUnknown<<"},\"ownership_summary\":{\"attempted\":"<<ownershipSummary_.attempted<<",\"linked\":"<<ownershipSummary_.linked
      <<",\"stored\":"<<ownershipSummary_.stored<<",\"deduplicated\":"<<ownershipSummary_.deduplicated<<",\"unsupported_writer\":"<<ownershipSummary_.unsupportedWriter
      <<",\"ancestor_missing\":"<<ownershipSummary_.ancestorMissing<<",\"unwind_failed\":"<<ownershipSummary_.unwindFailed<<",\"opcode_mismatch\":"<<ownershipSummary_.opcodeMismatch
      <<",\"tuple_read_fault\":"<<ownershipSummary_.tupleReadFault<<",\"tuple_mismatch\":"<<ownershipSummary_.tupleMismatch<<",\"registry_read_fault\":"<<ownershipSummary_.registryReadFault
      <<",\"registry_mismatch\":"<<ownershipSummary_.registryMismatch<<",\"record_range_mismatch\":"<<ownershipSummary_.recordRangeMismatch<<",\"record_read_fault\":"<<ownershipSummary_.recordReadFault
      <<",\"cache_conflicts\":"<<ownershipSummary_.cacheConflicts<<",\"record_overflow\":"<<ownershipSummary_.recordOverflow<<",\"byte_budget_declines\":"<<ownershipSummary_.byteBudgetDeclines
      <<",\"read_faults\":"<<ownershipSummary_.readFaults<<",\"ancestor_traces\":"<<ownershipSummary_.ancestorTraces<<",\"ancestor_trace_overflow\":"<<ownershipSummary_.ancestorTraceOverflow<<"},\"uploads\":[";
    for(size_t i=0;i<uploads_.size();++i){if(i)j<<',';const auto& u=uploads_[i];j<<"{\"attempt\":"<<u.attempt<<",\"resource\":"<<u.resource<<",\"generation\":"<<u.generation<<",\"cutoff\":"<<u.cutoff<<'}';}
    j<<"],\"records\":[";
    auto snapshot=[&](const Snapshot& s){j<<"{\"status\":\""<<s.status<<"\",\"offset\":"<<s.offset<<",\"bytes\":"<<(s.status=="available"?s.data.size():0)<<'}';};
    for(size_t i=0;i<records_.size();++i){if(i)j<<',';const auto& r=records_[i];j<<"{\"id\":"<<r.id<<",\"sequence\":"<<r.sequence<<",\"writer\":\""<<r.writer<<"\",\"return_rva\":";jsonAddress(j,r.returnRva);
        j<<",\"thread_id\":"<<r.threadId<<",\"observed_frame\":"<<r.observedFrame<<",\"completion_sequence\":"<<r.completionSequence<<",\"lookup_status\":\""<<r.lookupStatus<<"\",\"owner\":";jsonAddress(j,r.owner);j<<",\"key\":";jsonAddress(j,r.key);j<<",\"builder\":";jsonAddress(j,r.builder);j<<",\"object\":";jsonAddress(j,r.object);j<<",\"entry\":";jsonAddress(j,r.entry);
        j<<",\"ownership_id\":";if(r.ownershipId==kNone)j<<"null";else j<<r.ownershipId;j<<",\"ownership_status\":\""<<r.ownershipStatus<<'\"';
        j<<",\"context_status\":\""<<r.contextStatus<<"\",\"record\":";snapshot(r.record);j<<",\"key_snapshot\":";snapshot(r.keySnapshot);j<<",\"builder_snapshot\":";snapshot(r.builderSnapshot);j<<",\"object_snapshot\":";snapshot(r.objectSnapshot);j<<",\"entry_snapshot\":";snapshot(r.entrySnapshot);j<<'}';}
    j<<"],\"ownerships\":[";
    auto identity=[&](const PointerIdentity& x){j<<"{\"address\":";jsonAddress(j,x.address);j<<",\"vtable\":";jsonAddress(j,x.vtable);j<<",\"status\":\""<<x.status<<"\",\"vtable_rva\":";if(x.status=="module_relative")jsonAddress(j,x.vtableRva);else j<<"null";j<<'}';};
    for(size_t i=0;i<ownerships_.size();++i){if(i)j<<',';const auto& o=ownerships_[i];
        j<<"{\"id\":"<<o.id<<",\"first_sequence\":"<<o.firstSequence<<",\"observed_frame\":"<<o.observedFrame<<",\"thread_id\":"<<o.threadId<<",\"status\":\""<<o.status
         <<"\",\"branch\":\""<<o.branch<<"\",\"ancestor_return_rva\":";jsonAddress(j,o.ancestorReturnRva);j<<",\"unwind_depth\":"<<o.unwindDepth<<",\"writer_relation\":\""<<o.writerRelation
         <<"\",\"writer_owner\":";jsonAddress(j,o.writerOwner);j<<",\"outer\":";jsonAddress(j,o.outer);j<<",\"collection_owner\":";jsonAddress(j,o.collectionOwner);j<<",\"registry\":";jsonAddress(j,o.registry);j<<",\"context\":";jsonAddress(j,o.context);j<<",\"collection_record\":";jsonAddress(j,o.collectionRecord);
        j<<",\"record_index\":";if(o.hasRecordIndex)j<<o.recordIndex;else j<<"null";j<<",\"record_status\":\""<<o.recordStatus<<"\",\"game_object\":";jsonAddress(j,o.gameObject);j<<",\"descriptor\":";jsonAddress(j,o.descriptor);j<<",\"provider\":";jsonAddress(j,o.provider);j<<",\"parent\":";jsonAddress(j,o.parent);
        j<<",\"parent_token\":"<<o.parentToken<<",\"parent_status\":\""<<o.parentStatus<<"\",\"outer_identity\":";identity(o.outerIdentity);j<<",\"game_object_identity\":";identity(o.gameObjectIdentity);j<<",\"descriptor_identity\":";identity(o.descriptorIdentity);j<<",\"provider_identity\":";identity(o.providerIdentity);j<<",\"parent_identity\":";identity(o.parentIdentity);
        j<<",\"outer_snapshot\":";snapshot(o.outerSnapshot);j<<",\"collection_snapshot\":";snapshot(o.collectionSnapshot);j<<",\"context_snapshot\":";snapshot(o.contextSnapshot);j<<",\"record_tail_snapshot\":";snapshot(o.recordTailSnapshot);j<<",\"game_object_prefix\":";snapshot(o.gameObjectPrefix);j<<",\"descriptor_prefix\":";snapshot(o.descriptorPrefix);j<<",\"provider_prefix\":";snapshot(o.providerPrefix);j<<",\"parent_prefix\":";snapshot(o.parentPrefix);j<<'}';}
    j<<"],\"ancestor_traces\":[";
    for(size_t i=0;i<ancestorTraces_.size();++i){if(i)j<<',';const auto& t=ancestorTraces_[i];j<<"{\"status\":\""<<t.status<<"\",\"writer_record\":"<<t.writerRecord<<",\"return_rva\":";jsonAddress(j,t.returnRva);j<<",\"frames\":[";
        for(size_t n=0;n<t.frames.size();++n){if(n)j<<',';const auto& f=t.frames[n];j<<"{\"address\":";jsonAddress(j,f.address);j<<",\"status\":\""<<(f.moduleRelative?"module_relative":"outside_image")<<"\",\"module_rva\":";if(f.moduleRelative)jsonAddress(j,f.moduleRva);else j<<"null";j<<'}';}j<<"]}";}
    j<<"]}";
}

} // namespace edvr
