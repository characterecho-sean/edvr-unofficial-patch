#include "kinematic_eval_probe.h"
#include "kinematic_eval_hook.h"
#include <cstring>

namespace edvr {

KinematicEvalProbe kinematicEvalProbe;

namespace {
const char* const kJobNames[KinematicEvalProbe::kJobCount]={
    "Kinematic::UpdateRenderDataJob",   // body RVA 0x4321940 (run thunk 0x42DF520)
    "Kinematic::RenderDataBatch",       // body RVA 0x4320340 (run thunk 0x42DFAF0)
    "Kinematic::UpdatePhysicsObjectsJob",// body RVA 0x432B2A0 (run thunk 0x42DF540)
    "Kinematic::PrePhysicsAdvanceJob",  // body RVA 0x42DF530 (run is the body)
    "Kinematic::PrePhysicsAdvanceCurveJob",// body RVA 0x42DF550 (run is the body)
    "Kinematic::Unnamed_BA0",           // body RVA 0x4321810 (run thunk 0x42DFBA0)
};
}

bool KinematicEvalProbe::guardedRead(uintptr_t address,void* output,size_t bytes) noexcept {
    __try {std::memcpy(output,reinterpret_cast<const void*>(address),bytes);return true;}
    __except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
uint64_t KinematicEvalProbe::read64(uintptr_t address,bool& ok) noexcept {
    uint64_t v=0;if(!guardedRead(address,&v,sizeof(v)))ok=false;return v;
}
uint32_t KinematicEvalProbe::read32(uintptr_t address,bool& ok) noexcept {
    uint32_t v=0;if(!guardedRead(address,&v,sizeof(v)))ok=false;return v;
}
uint8_t KinematicEvalProbe::read8(uintptr_t address,bool& ok) noexcept {
    uint8_t v=0;if(!guardedRead(address,&v,sizeof(v)))ok=false;return v;
}

bool KinematicEvalProbe::validateExecutableLocked() noexcept {
    const uintptr_t base=reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    if(!base)return false;
    uint64_t peOff=0;
    bool ok=true;
    peOff=read32(base+0x3C,ok);
    if(!ok||peOff>0x1000)return false;
    const uint32_t timestamp=read32(base+peOff+8,ok);
    const uint32_t imageSize=read32(base+peOff+0x50,ok);
    if(!ok||timestamp!=kExpectedTimestamp||imageSize!=kExpectedImageSize)return false;
    // The evaluator's prologue: push rbx; push rbp; push rsi; push rdi;
    // push r12; push r14; push r15; sub rsp,0x70. Checked before hooking.
    static const uint8_t kEvalPrologue[14]={0x40,0x53,0x55,0x56,0x57,0x41,0x54,0x41,0x56,0x41,0x57,0x48,0x83,0xEC};
    uint8_t got[sizeof(kEvalPrologue)]{};
    if(!guardedRead(base+kEvalRva,got,sizeof(got)))return false;
    if(std::memcmp(got,kEvalPrologue,sizeof(kEvalPrologue))!=0 &&
       !kinematicEvalHooksMatch(base+kEvalRva))return false;
    imageBase_=base;
    return true;
}

void KinematicEvalProbe::clearLocked() {
    summary_=Summary{};
    index_.clear();records_.clear();transitions_.clear();
    vtableIndex_.clear();vtables_.clear();
    ownershipIndex_.clear();ownerships_.clear();
    for(uint32_t i=0;i<kJobCount;++i) {
        jobs_[i].calls.store(0,std::memory_order_relaxed);
        jobs_[i].totalNs.store(0,std::memory_order_relaxed);
        jobs_[i].maxNs.store(0,std::memory_order_relaxed);
    }
}

bool KinematicEvalProbe::arm(uint32_t meshFrame) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if(!validateExecutableLocked()) {
        hookStatus_=HookStatus::IdentityMismatch;
        return false;
    }
    // attach is called on every arm, not just the first: finish/reset detach
    // the observer gate, and the already-installed path only verifies the
    // patch and re-stores the observer -- one mutex, one prologue read.
    const char* result=attachKinematicEvalHooks(this);
    hookStatus_=std::strcmp(result,"installed")==0?HookStatus::Installed
        :std::strcmp(result,"opcode_mismatch")==0?HookStatus::OpcodeMismatch
        :HookStatus::InstallFailed;
    if(hookStatus_!=HookStatus::Installed)return false;
    frame_.store(meshFrame,std::memory_order_release);
    clearLocked();
    active_.store(true,std::memory_order_release);
    return true;
}

void KinematicEvalProbe::finish() noexcept {
    active_.store(false,std::memory_order_release);
    detachKinematicEvalHooks(this);
}

void KinematicEvalProbe::reset() noexcept {
    active_.store(false,std::memory_order_release);
    detachKinematicEvalHooks(this);
    std::lock_guard<std::mutex> lock(mutex_);
    clearLocked();
}

uint64_t KinematicEvalProbe::vtableRvaLocked(uintptr_t object) noexcept {
    if(!object)return 0;
    bool ok=true;
    const uint64_t vtable=read64(object,ok);
    if(!ok||!vtable){++summary_.readFaults;return 0;}
    if(vtable>=imageBase_&&vtable<imageBase_+kExpectedImageSize)return vtable-imageBase_;
    return 0; // foreign vtable: still a useful signal, recorded as 0 with the pointer kept
}

void KinematicEvalProbe::noteVtableLocked(uint64_t rva,uint32_t frame) noexcept {
    if(!rva)return;
    const auto it=vtableIndex_.find(rva);
    if(it!=vtableIndex_.end()){++vtables_[it->second].hits;return;}
    if(vtables_.size()>=kVtableCap){++summary_.vtableOverflow;return;}
    vtableIndex_.emplace(rva,static_cast<uint32_t>(vtables_.size()));
    VtableUse v;v.rva=rva;v.firstFrame=frame;v.hits=1;
    vtables_.push_back(v);
    ++summary_.vtables;
}

void KinematicEvalProbe::observe(uintptr_t descriptor,uintptr_t renderRecord) noexcept {
    if(!active_.load(std::memory_order_acquire))return;
    if(!descriptor||!renderRecord)return;
    std::lock_guard<std::mutex> lock(mutex_);
    ++summary_.observed;
    bool ok=true;
    const uint64_t record=read64(descriptor+0x10,ok);   // descriptor+0x10: the stride-0x2F0 record
    const uint32_t flags=read32(renderRecord+0x688,ok); // skip bit 0x1000; gate bits 0xFF0
    const uint8_t predByte=read8(descriptor+0x49,ok);   // predicate liveness byte
    const uint8_t gate2=read8(descriptor+0x58,ok);      // second gate byte
    if(!ok){++summary_.readFaults;return;}
    if(!record){++summary_.readFaults;return;}
    // Epoch pair (offline chain, 2026-09-19 21:58 entry): descriptor+0x38 is
    // the collection epoch [collection+0x90]; record+0x1B8 is the record's
    // last-refresh epoch. Equality says the record was refreshed this epoch.
    const uint64_t epoch1b8=read64(record+0x1B8,ok);
    const uint64_t descEpoch38=read64(descriptor+0x38,ok);
    if(!ok){++summary_.readFaults;return;}
    if(epoch1b8==descEpoch38)++summary_.epochMatches;else ++summary_.epochMismatches;
    const uint32_t frame=frame_.load(std::memory_order_acquire);
    auto it=index_.find(record);
    if(it==index_.end()) {
        if(records_.size()>=kRecordCap){++summary_.recordOverflow;return;}
        RecordState r;
        r.record=record;
        r.node=read64(record+0x18,ok);
        r.poseCtx=read64(record+0x290,ok);
        r.hash=read64(record+0x268,ok);
        r.count298=read64(record+0x298,ok);
        r.predPtr=read64(record+0x2C0,ok);
        r.pred2Ptr=read64(record+0x2C8,ok);
        r.bool234=read8(record+0x234,ok);
        r.flags=flags;r.predByte=predByte;r.gate2=gate2;
        r.epoch1b8=epoch1b8;r.descEpoch38=descEpoch38;
        r.firstFrame=frame;r.lastFrame=frame;r.calls=1;
        if(!ok)++summary_.readFaults;
        r.predVtableRva=vtableRvaLocked(static_cast<uintptr_t>(r.predPtr));
        r.pred2VtableRva=vtableRvaLocked(static_cast<uintptr_t>(r.pred2Ptr));
        noteVtableLocked(r.predVtableRva,frame);
        noteVtableLocked(r.pred2VtableRva,frame);
        index_.emplace(record,static_cast<uint32_t>(records_.size()));
        records_.push_back(r);
        ++summary_.stored;
        return;
    }
    RecordState& r=records_[it->second];
    ++r.calls;r.lastFrame=frame;
    if(r.epoch1b8!=epoch1b8){++summary_.epochChanges;r.epoch1b8=epoch1b8;}
    r.descEpoch38=descEpoch38;
    bool hashOk=true;
    const uint64_t hash=read64(record+0x268,hashOk);
    if(!hashOk)++summary_.readFaults;
    if(r.flags!=flags||r.hash!=hash) {
        if(transitions_.size()<kTransitionCap) {
            Transition t;t.recordId=it->second;t.frame=frame;
            t.oldFlags=r.flags;t.newFlags=flags;t.oldHash=r.hash;t.newHash=hash;
            transitions_.push_back(t);
            ++summary_.transitions;
        } else ++summary_.transitionOverflow;
        r.flags=flags;r.hash=hash;
    }
}

void KinematicEvalProbe::noteOwnership(uint32_t jobId,uintptr_t arg) noexcept {
    if(jobId>1||!arg)return; // only jobs 0 (descriptor) and 1 (collection)
    if(!active_.load(std::memory_order_acquire))return;
    std::lock_guard<std::mutex> lock(mutex_);
    bool ok=true;
    // Job 0 (UpdateRenderDataJob body) takes the FUN_14431AFE0 descriptor;
    // its +0x10 is the ADDRESS of collection+0x300. Job 1 (render-data
    // batch) takes the collection itself.
    const uint64_t collection=
        jobId==0?read64(arg+0x10,ok)-0x300:static_cast<uint64_t>(arg);
    if(!ok){++summary_.readFaults;return;}
    const uint64_t owner=read64(collection+0x18,ok);   // backing owner (ctor param_3)
    const uint64_t alias20=read64(collection+0x20,ok); // ctor: vtable slot +0x60 result
    const uint64_t epoch90=read64(collection+0x90,ok); // collection epoch
    if(!ok){++summary_.readFaults;return;}
    uint64_t backPtr=0;uint32_t ownerState=0;
    if(owner) {
        backPtr=read64(owner+0x348,ok);                // rig's collection slot?
        ownerState=read32(owner+0x380,ok);             // rig state machine (4 = render-ready)
        if(!ok){++summary_.readFaults;return;}
    }
    ++summary_.ownershipChecks;
    if(backPtr==collection)++summary_.ownerBackPtrMatch;
    if(ownerState==4)++summary_.ownerState4;
    const uint32_t frame=frame_.load(std::memory_order_acquire);
    const auto it=ownershipIndex_.find(collection);
    if(it!=ownershipIndex_.end()) {
        OwnershipUse& o=ownerships_[it->second];
        ++o.hits;o.owner=owner;o.alias20=alias20;o.epoch90=epoch90;
        o.backPtr=backPtr;o.ownerState=ownerState;
        return;
    }
    if(ownerships_.size()>=kOwnershipCap){++summary_.ownershipOverflow;return;}
    OwnershipUse o;
    o.collection=collection;o.owner=owner;o.alias20=alias20;o.epoch90=epoch90;
    o.backPtr=backPtr;o.ownerState=ownerState;o.jobId=jobId;
    o.firstFrame=frame;o.hits=1;
    ownershipIndex_.emplace(collection,static_cast<uint32_t>(ownerships_.size()));
    ownerships_.push_back(o);
}

KinematicEvalProbe::Summary KinematicEvalProbe::summary() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return summary_;
}

const char* KinematicEvalProbe::hookStatusText() const noexcept {
    switch(hookStatus_) {
    case HookStatus::NotRun: return "not_run";
    case HookStatus::Installed: return "installed";
    case HookStatus::IdentityMismatch: return "identity_mismatch";
    case HookStatus::OpcodeMismatch: return "opcode_mismatch";
    case HookStatus::InstallFailed: return "install_failed";
    }
    return "unknown";
}

void KinematicEvalProbe::writeJson(std::ostringstream& j) const {
    std::lock_guard<std::mutex> lock(mutex_);
    j<<"\"kinematicEval\":{\"status\":\""<<hookStatusText()<<"\","
     <<"\"summary\":{\"observed\":"<<summary_.observed<<",\"records\":"<<records_.size()
     <<",\"transitions\":"<<summary_.transitions<<",\"vtables\":"<<vtables_.size()
     <<",\"read_faults\":"<<summary_.readFaults<<",\"record_overflow\":"<<summary_.recordOverflow
     <<",\"transition_overflow\":"<<summary_.transitionOverflow
     <<",\"vtable_overflow\":"<<summary_.vtableOverflow
     <<",\"epoch_matches\":"<<summary_.epochMatches
     <<",\"epoch_mismatches\":"<<summary_.epochMismatches
     <<",\"epoch_changes\":"<<summary_.epochChanges
     <<",\"ownership_checks\":"<<summary_.ownershipChecks
     <<",\"owner_backptr_match\":"<<summary_.ownerBackPtrMatch
     <<",\"owner_state4\":"<<summary_.ownerState4
     <<",\"ownership_overflow\":"<<summary_.ownershipOverflow<<"},"
     <<"\"vtables\":[";
    for(size_t i=0;i<vtables_.size();++i) {
        if(i)j<<',';
        j<<"{\"rva\":\"0x"<<std::hex<<vtables_[i].rva<<std::dec<<"\",\"first_frame\":"<<vtables_[i].firstFrame
         <<",\"hits\":"<<vtables_[i].hits<<'}';
    }
    j<<"],\"jobs\":[";
    for(uint32_t i=0;i<kJobCount;++i) {
        if(i)j<<',';
        const uint64_t calls=jobs_[i].calls.load(std::memory_order_relaxed);
        const uint64_t total=jobs_[i].totalNs.load(std::memory_order_relaxed);
        j<<"{\"name\":\""<<kJobNames[i]<<"\",\"calls\":"<<calls<<",\"total_ns\":"<<total
         <<",\"mean_ns\":"<<(calls?total/calls:0)
         <<",\"max_ns\":"<<jobs_[i].maxNs.load(std::memory_order_relaxed)<<'}';
    }
    j<<"],\"records\":[";
    for(size_t i=0;i<records_.size();++i) {
        if(i)j<<',';
        const RecordState& r=records_[i];
        j<<"{\"id\":"<<i<<",\"record\":\"0x"<<std::hex<<r.record<<std::dec
         <<"\",\"first_frame\":"<<r.firstFrame<<",\"last_frame\":"<<r.lastFrame<<",\"calls\":"<<r.calls
         <<",\"node\":\"0x"<<std::hex<<r.node<<"\",\"pose_ctx\":\"0x"<<r.poseCtx
         <<"\",\"pred\":\"0x"<<r.predPtr<<"\",\"pred_vtable_rva\":\"0x"<<r.predVtableRva
         <<"\",\"pred2\":\"0x"<<r.pred2Ptr<<"\",\"pred2_vtable_rva\":\"0x"<<r.pred2VtableRva<<std::dec
         <<"\",\"hash\":\"0x"<<std::hex<<r.hash<<std::dec<<"\",\"count298\":"<<r.count298
         <<",\"flags\":\"0x"<<std::hex<<r.flags<<std::dec
         <<"\",\"bool234\":"<<uint32_t(r.bool234)<<",\"pred_byte\":"<<uint32_t(r.predByte)
         <<",\"gate2\":"<<uint32_t(r.gate2)
         <<",\"epoch1b8\":\"0x"<<std::hex<<r.epoch1b8
         <<"\",\"desc_epoch38\":\"0x"<<r.descEpoch38<<std::dec<<"\"}";
    }
    j<<"],\"transitions\":[";
    for(size_t i=0;i<transitions_.size();++i) {
        if(i)j<<',';
        const Transition& t=transitions_[i];
        j<<"{\"record\":"<<t.recordId<<",\"frame\":"<<t.frame
         <<",\"old_flags\":\"0x"<<std::hex<<t.oldFlags<<"\",\"new_flags\":\"0x"<<t.newFlags
         <<"\",\"old_hash\":\"0x"<<t.oldHash<<"\",\"new_hash\":\"0x"<<t.newHash<<std::dec<<"\"}";
    }
    j<<"],\"ownership\":[";
    for(size_t i=0;i<ownerships_.size();++i) {
        if(i)j<<',';
        const OwnershipUse& o=ownerships_[i];
        j<<"{\"collection\":\"0x"<<std::hex<<o.collection<<"\",\"owner\":\"0x"<<o.owner
         <<"\",\"alias20\":\"0x"<<o.alias20<<"\",\"epoch90\":\"0x"<<o.epoch90
         <<"\",\"backptr\":\"0x"<<o.backPtr<<"\",\"owner_state\":"<<std::dec<<o.ownerState
         <<",\"job\":"<<o.jobId<<",\"first_frame\":"<<o.firstFrame<<",\"hits\":"<<o.hits<<'}';
    }
    j<<"]}";
}

} // namespace edvr
