#include "object_record_writer_probe.h"
#include "object_record_writer_hook.h"
#include <array>
#include <cstring>
#include <iomanip>

namespace edvr {
namespace {
constexpr uint64_t kManagementReturn=0x434E316u;
constexpr uint64_t kWriterReturns[]={0x369CE91u,0x42B42EFu,0x42B4ED6u,0x43130AAu,0x434D149u};
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
bool subtract(uintptr_t value,uintptr_t amount,uintptr_t& out) noexcept {
    if(value<amount)return false;out=value-amount;return true;
}
bool add(uintptr_t value,uintptr_t amount,uintptr_t& out) noexcept {
    if(value>UINTPTR_MAX-amount)return false;out=value+amount;return true;
}
void jsonAddress(std::ostringstream& j,uint64_t value){j<<"\"0x"<<std::hex<<value<<std::dec<<'\"';}
}

ObjectRecordWriterProbe objectRecordWriterProbe;

bool ObjectRecordWriterProbe::guardedRead(uintptr_t address,void* output,size_t bytes) noexcept {
    if(!bytes)return true;if(!address||!output||address>UINTPTR_MAX-bytes)return false;
    __try {std::memcpy(output,reinterpret_cast<const void*>(address),bytes);return true;}
    __except(EXCEPTION_EXECUTE_HANDLER){return false;}
}

void ObjectRecordWriterProbe::clearLocked() {
    summary_=Summary{};eventSequence_=0;if(++epoch_==0)++epoch_;
    records_.clear();uploads_.clear();imageBase_=0;
    hookStatus_=HookStatus::NotRun;active_.store(false,std::memory_order_release);
}

bool ObjectRecordWriterProbe::validateExecutableLocked() noexcept {
    imageBase_=reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
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
    return true;
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

ObjectRecordWriterProbe::Snapshot ObjectRecordWriterProbe::captureLocked(uintptr_t address,size_t bytes,bool applicable) {
    Snapshot out;if(!applicable){out.status="not_applicable";return out;}if(!address){out.status="null_pointer";return out;}
    if(bytes>kByteCap-summary_.retainedBytes){out.status="byte_budget";++summary_.byteBudgetDeclines;return out;}
    out.data.resize(bytes);if(!guardedRead(address,out.data.data(),bytes)){out.data.clear();out.status="read_fault";++summary_.readFaults;return out;}
    out.status="available";summary_.retainedBytes+=bytes;return out;
}

bool ObjectRecordWriterProbe::unwindCaller(uintptr_t returnAddress,const CONTEXT& captured,CONTEXT& caller) noexcept {
    caller=captured;
    for(uint32_t frame=0;frame<16;++frame) {
        if(caller.Rip==returnAddress)return true;
        const DWORD64 oldRip=caller.Rip,oldRsp=caller.Rsp;DWORD64 imageBase=0;PRUNTIME_FUNCTION function=nullptr;
        __try {function=RtlLookupFunctionEntry(caller.Rip,&imageBase,nullptr);}
        __except(EXCEPTION_EXECUTE_HANDLER){return false;}
        if(function) {
            DWORD64 establisher=0;PVOID handlerData=nullptr;
            __try {RtlVirtualUnwind(UNW_FLAG_NHANDLER,imageBase,caller.Rip,function,&caller,&handlerData,&establisher,nullptr);}
            __except(EXCEPTION_EXECUTE_HANDLER){return false;}
        } else {
            uintptr_t next=0;if(!guardedRead(uintptr_t(caller.Rsp),&next,sizeof(next)))return false;
            caller.Rip=next;caller.Rsp+=sizeof(uintptr_t);
        }
        if(caller.Rsp<=oldRsp||caller.Rip==oldRip)return false;
    }
    return false;
}

ObjectRecordWriterProbe::Pending ObjectRecordWriterProbe::beginLookup(
    uintptr_t returnAddress,uintptr_t dictionary,uintptr_t key,const CONTEXT& captured) noexcept {
    if(!active_.load(std::memory_order_acquire))return {};
    CONTEXT caller{};if(!unwindCaller(returnAddress,captured,caller)) {
        std::lock_guard<std::mutex> lock(mutex_);++summary_.unwindFailures;++summary_.contextFailures;return {};
    }
    std::lock_guard<std::mutex> lock(mutex_);if(!active_.load(std::memory_order_relaxed))return {};
    if(returnAddress<imageBase_){++summary_.declinedUnknown;return {};}
    try {return beginRvaLocked(uint64_t(returnAddress-imageBase_),uintptr_t(caller.Rsp),dictionary,key,caller);}
    catch(...) {++summary_.recordOverflow;summary_.retainedBytes=0;for(const auto& r:records_)for(const Snapshot* s:{&r.record,&r.keySnapshot,&r.builderSnapshot,&r.objectSnapshot,&r.entrySnapshot})summary_.retainedBytes+=s->data.size();return {};}
}
#ifdef EDVR_RECORD_WRITER_TEST
ObjectRecordWriterProbe::Pending ObjectRecordWriterProbe::beginLookupUnwindRvaForTest(
    uintptr_t fixtureReturn,uint64_t gameReturnRva,uintptr_t dictionary,
    uintptr_t key,const CONTEXT& captured) noexcept {
    if(!active_.load(std::memory_order_acquire))return {};
    CONTEXT caller{};if(!unwindCaller(fixtureReturn,captured,caller)) {
        std::lock_guard<std::mutex> lock(mutex_);++summary_.unwindFailures;++summary_.contextFailures;return {};
    }
    std::lock_guard<std::mutex> lock(mutex_);if(!active_.load(std::memory_order_relaxed))return {};
    try {return beginRvaLocked(gameReturnRva,uintptr_t(caller.Rsp),dictionary,key,caller);}
    catch(...) {++summary_.recordOverflow;summary_.retainedBytes=0;for(const auto& r:records_)for(const Snapshot* s:{&r.record,&r.keySnapshot,&r.builderSnapshot,&r.objectSnapshot,&r.entrySnapshot})summary_.retainedBytes+=s->data.size();return {};}
}
#endif
ObjectRecordWriterProbe::Pending ObjectRecordWriterProbe::beginLookupRvaForTest(
    uint64_t returnRva,uintptr_t callerRsp,uintptr_t dictionary,uintptr_t key,const CONTEXT& caller) noexcept {
    if(!active_.load(std::memory_order_acquire))return {};
    std::lock_guard<std::mutex> lock(mutex_);try{return beginRvaLocked(returnRva,callerRsp,dictionary,key,caller);}
    catch(...) {++summary_.recordOverflow;summary_.retainedBytes=0;for(const auto& r:records_)for(const Snapshot* s:{&r.record,&r.keySnapshot,&r.builderSnapshot,&r.objectSnapshot,&r.entrySnapshot})summary_.retainedBytes+=s->data.size();return {};}
}

ObjectRecordWriterProbe::Pending ObjectRecordWriterProbe::beginRvaLocked(
    uint64_t rva,uintptr_t callerRsp,uintptr_t dictionary,uintptr_t key,const CONTEXT& c) {
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
        r.builder=builder;r.object=uintptr_t(c.R12);builderBytes=0x60;objectBytes=0xB0;break;
    }
    case 0x42B4ED6u: {
        r.writer="inline_42b4ed6";contextOk=add(uintptr_t(c.Rbp),0x1C0,record)&&contextOk;uintptr_t objectAt=0;
        if(subtract(uintptr_t(c.Rbp),0x68,objectAt))contextOk=guardedRead(objectAt,&r.object,sizeof(r.object))&&contextOk;else contextOk=false;
        objectBytes=0xB0;break;
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
    const Pending pending{r.id,r.sequence,epoch_};records_.push_back(std::move(r));summary_.stored=uint32_t(records_.size());return pending;
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
    summary_.retainedBytes=0;
    for(const auto& r:records_)for(const Snapshot* s:{&r.record,&r.keySnapshot,&r.builderSnapshot,&r.objectSnapshot,&r.entrySnapshot})
        if(s->status=="available")summary_.retainedBytes+=s->data.size();
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
void ObjectRecordWriterProbe::writeJson(std::ostringstream& j) const {
    std::lock_guard<std::mutex> lock(mutex_);j<<"  \"record_writers\":{\"version\":1,\"status\":\""<<topStatus(summary_,hookStatus_)<<"\",\"hook_status\":\""<<hookStatusName(hookStatus_)
      <<"\",\"limits\":{\"records\":"<<kRecordCap<<",\"bytes\":"<<kByteCap<<"},\"summary\":{\"observed\":"<<summary_.observed<<",\"stored\":"<<summary_.stored<<",\"completed\":"<<summary_.completed
      <<",\"retained_bytes\":"<<summary_.retainedBytes<<",\"record_overflow\":"<<summary_.recordOverflow<<",\"byte_budget_declines\":"<<summary_.byteBudgetDeclines
      <<",\"read_faults\":"<<summary_.readFaults<<",\"context_failures\":"<<summary_.contextFailures<<",\"unwind_failures\":"<<summary_.unwindFailures<<",\"completion_failures\":"<<summary_.completionFailures<<",\"declined_management\":"<<summary_.declinedManagement
      <<",\"declined_unknown\":"<<summary_.declinedUnknown<<"},\"uploads\":[";
    for(size_t i=0;i<uploads_.size();++i){if(i)j<<',';const auto& u=uploads_[i];j<<"{\"attempt\":"<<u.attempt<<",\"resource\":"<<u.resource<<",\"generation\":"<<u.generation<<",\"cutoff\":"<<u.cutoff<<'}';}
    j<<"],\"records\":[";
    auto snapshot=[&](const Snapshot& s){j<<"{\"status\":\""<<s.status<<"\",\"offset\":"<<s.offset<<",\"bytes\":"<<(s.status=="available"?s.data.size():0)<<'}';};
    for(size_t i=0;i<records_.size();++i){if(i)j<<',';const auto& r=records_[i];j<<"{\"id\":"<<r.id<<",\"sequence\":"<<r.sequence<<",\"writer\":\""<<r.writer<<"\",\"return_rva\":";jsonAddress(j,r.returnRva);
        j<<",\"thread_id\":"<<r.threadId<<",\"observed_frame\":"<<r.observedFrame<<",\"completion_sequence\":"<<r.completionSequence<<",\"lookup_status\":\""<<r.lookupStatus<<"\",\"owner\":";jsonAddress(j,r.owner);j<<",\"key\":";jsonAddress(j,r.key);j<<",\"builder\":";jsonAddress(j,r.builder);j<<",\"object\":";jsonAddress(j,r.object);j<<",\"entry\":";jsonAddress(j,r.entry);
        j<<",\"context_status\":\""<<r.contextStatus<<"\",\"record\":";snapshot(r.record);j<<",\"key_snapshot\":";snapshot(r.keySnapshot);j<<",\"builder_snapshot\":";snapshot(r.builderSnapshot);j<<",\"object_snapshot\":";snapshot(r.objectSnapshot);j<<",\"entry_snapshot\":";snapshot(r.entrySnapshot);j<<'}';}
    j<<"]}";
}

} // namespace edvr
