#include "kinematic_eval_probe.h"
#include "kinematic_eval_hook.h"
#include <cmath>
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

void KinematicEvalProbe::clearLocked() {
    // New capture boundary: in-flight job brackets that entered before this
    // bump drop their samples instead of committing into the new capture
    // (2026-09-20 review finding 8). Then wait out a mid-commit bracket
    // (three stores between the seq toggles) so the zeroing itself never
    // leaves a seq odd.
    jobGeneration_.fetch_add(1,std::memory_order_acq_rel);
    summary_=Summary{};
    index_.clear();records_.clear();transitions_.clear();
    vtableIndex_.clear();vtables_.clear();
    ownershipIndex_.clear();ownerships_.clear();
    riglinkIndex_.clear();riglinks_.clear();
    samples_.clear();events_.clear();seenThisFrame_=0;
    clockSamples_.clear();
    clockSeeded_=false;gapRelogsThisFrame_=0;
    for(uint32_t i=0;i<kJobCount;++i) {
        while(jobs_[i].seq.load(std::memory_order_acquire)&1u) {}
        jobs_[i].calls.store(0,std::memory_order_relaxed);
        jobs_[i].totalNs.store(0,std::memory_order_relaxed);
        jobs_[i].maxNs.store(0,std::memory_order_relaxed);
    }
}

bool KinematicEvalProbe::arm(uint32_t meshFrame) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    // Executable validation lives INSIDE the attach, under the hook
    // installation mutex: validating unlocked here raced a concurrent
    // tracker install that had patched the prologue but not yet published
    // ready, and the arm then rejected the supported executable
    // (2026-09-20 review finding 5).
    const char* result=attachKinematicEvalHooks(this);
    hookStatus_=std::strcmp(result,"installed")==0?HookStatus::Installed
        :std::strcmp(result,"opcode_mismatch")==0?HookStatus::OpcodeMismatch
        :std::strcmp(result,"identity_mismatch")==0?HookStatus::IdentityMismatch
        :HookStatus::InstallFailed;
    if(hookStatus_!=HookStatus::Installed)return false;
    imageBase_=reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    // The arm stamp is the legacy mesh clock; it only seeds frame_ as a
    // baseline for lastSampledFrame compares. The first notePresentFrame
    // (the real per-present clock feed) overwrites it -- without flushing,
    // so no artificial empty frame is counted (review finding 7).
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

void KinematicEvalProbe::setFrame(uint32_t meshFrame) noexcept {
    if(!active_.load(std::memory_order_acquire)) {
        frame_.store(meshFrame,std::memory_order_release);
        return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    const uint32_t prev=frame_.exchange(meshFrame,std::memory_order_acq_rel);
    if(prev==meshFrame)return;
    flushFrameStatsLocked();
}

void KinematicEvalProbe::notePresentFrame(uint32_t presentFrame,uint32_t meshClock) noexcept {
    // Gated on active like the rest of the probe: the clock log lives and
    // dies with the capture lifecycle rather than running unarmed.
    if(!active_.load(std::memory_order_acquire))return;
    std::lock_guard<std::mutex> lock(mutex_);
    if(!clockSeeded_) {
        // Seed WITHOUT flushing: sampling was gated on clockSeeded_, so the
        // frame that just "ended" could not accept a single sample -- flushing
        // it fabricates an empty frame and forces min_frame_records to zero
        // even with a healthy feed (2026-09-20 review finding 7). Only real
        // present-domain transitions close frames, as the tracker does.
        clockSeeded_=true;
        frame_.store(presentFrame,std::memory_order_release);
    } else {
        const uint32_t prev=frame_.exchange(presentFrame,std::memory_order_acq_rel);
        if(prev!=presentFrame)flushFrameStatsLocked();
    }
    // The mesh counter resets to 0 on every config re-poll (meshMotionShutdown
    // via the once-per-second re-configure), so absolute mesh values are
    // only meaningful between configure events; the per-window
    // mesh-vs-present ratio is the signal.
    if(clockSamples_.size()<kClockSampleCap) {
        ClockSample c;c.present=presentFrame;c.mesh=meshClock;
        clockSamples_.push_back(c);
    } else ++summary_.clockSampleOverflow;
}

void KinematicEvalProbe::flushFrameStatsLocked() noexcept {
    // Only real present-domain transitions reach here: the clock seed does
    // not flush (review finding 7), so every counted frame could accept
    // samples. A zero here is a genuine empty frame, never an artifact.
    ++summary_.framesCounted;
    if(seenThisFrame_==0)++summary_.zeroRecordFrames;
    if(seenThisFrame_>summary_.maxFrameRecords)summary_.maxFrameRecords=seenThisFrame_;
    if(seenThisFrame_<summary_.minFrameRecords)summary_.minFrameRecords=seenThisFrame_;
    seenThisFrame_=0;
    gapRelogsThisFrame_=0;
}

void KinematicEvalProbe::noteIdentityEventLocked(uint32_t recordId,uint32_t frame,
        uint32_t kind,uint32_t gapLen,uint64_t oldNode,uint64_t newNode) noexcept {
    if(events_.size()>=kIdentityEventCap){++summary_.identityEventOverflow;return;}
    IdentityEvent e;
    e.recordId=recordId;e.frame=frame;e.kind=kind;e.gapLen=gapLen;
    e.oldNode=oldNode;e.newNode=newNode;
    events_.push_back(e);
}

// Frame-aligned pose sample: one per record per frame, decoded from the xf
// block the caller already read (no extra guarded reads for the transform).
void KinematicEvalProbe::samplePoseLocked(uint32_t recordId,RecordState& r,
        const uint64_t* xf) noexcept {
    // 094158: arm() seeds frame_ with a mesh-domain stamp; records first
    // sampled under it were re-stamped at the first present-domain tick,
    // firing 3,073 fake gap_len=2 events and saturating the identity log at
    // frame one. Until the clock is seeded the probe behaves exactly as the
    // pre-pose-history build: the xf/transition logic in observe() still
    // runs, only the sampling paths gate. First-sight baselines are kept
    // once the clock is live (they carry identity context).
    if(!clockSeeded_)return;
    const uint32_t frame=frame_.load(std::memory_order_acquire);
    // The dup gate yields to an identity reset (hasPrevSample==false): the
    // observation that detected the new node re-baselines immediately.
    if(r.hasPrevSample&&r.lastSampledFrame==frame) {
        ++r.dupInFrame;++summary_.dupInFrame;
        return; // second observation of the same record in one frame
    }
    // tx/ty/tz = 3 floats at record+0x170 (xf[8] low/high, xf[9] low);
    // q0..q3 = 4 uint16 lanes at +0x17C..0x184 (xf[9] bits 32..63, xf[10]
    // bits 0..31). Raw bits; decode is offline.
    const uint32_t txb=uint32_t(xf[8]&0xFFFFFFFFull);
    const uint32_t tyb=uint32_t(xf[8]>>32);
    const uint32_t tzb=uint32_t(xf[9]&0xFFFFFFFFull);
    const uint16_t q0=uint16_t((xf[9]>>32)&0xFFFFull);
    const uint16_t q1=uint16_t((xf[9]>>48)&0xFFFFull);
    const uint16_t q2=uint16_t(xf[10]&0xFFFFull);
    const uint16_t q3=uint16_t((xf[10]>>16)&0xFFFFull);
    float tx,ty,tz;
    std::memcpy(&tx,&txb,4);std::memcpy(&ty,&tyb,4);std::memcpy(&tz,&tzb,4);
    ++r.framesSampled;
    ++seenThisFrame_;
    const bool gapResume=r.hasPrevSample&&frame>r.lastSampledFrame+1;
    if(gapResume) {
        const uint32_t gapLen=frame-r.lastSampledFrame-1;
        ++r.gaps;++summary_.gapEvents;
        if(gapLen>r.maxGap)r.maxGap=gapLen;
        noteIdentityEventLocked(recordId,frame,1,gapLen,0,0);
    }
    // Identity is handled by the caller: observe() reads the node BEFORE the
    // motion compares and, on a change, clears hasPrevSample so neither this
    // jump nor the xf diff crosses the identity boundary (2026-09-20 review
    // finding 3). No node read here any more.
    uint8_t pose[20];
    std::memcpy(pose,&txb,4);std::memcpy(pose+4,&tyb,4);std::memcpy(pose+8,&tzb,4);
    std::memcpy(pose+12,&q0,2);std::memcpy(pose+14,&q1,2);
    std::memcpy(pose+16,&q2,2);std::memcpy(pose+18,&q3,2);
    if(r.hasPrevSample) {
        float ptx,pty,ptz;uint32_t pb;
        std::memcpy(&pb,r.prevPose,4);std::memcpy(&ptx,&pb,4);
        std::memcpy(&pb,r.prevPose+4,4);std::memcpy(&pty,&pb,4);
        std::memcpy(&pb,r.prevPose+8,4);std::memcpy(&ptz,&pb,4);
        const float dx=tx-ptx,dy=ty-pty,dz=tz-ptz;
        const float jump=std::sqrt(dx*dx+dy*dy+dz*dz);
        // Non-finite input or overflow must not reach the JSON floats
        // (finding 9): reject the measurement, keep the raw bits below.
        if(std::isfinite(jump)) {
            if(jump>r.maxJump)r.maxJump=jump;
            r.totalJump+=jump;
        } else ++summary_.nonFinitePose;
        if(std::memcmp(pose+12,r.prevPose+12,8)!=0) {
            ++r.quatChangeFrames;++summary_.quatChangeFrames;
        }
    }
    // Mover sample log: first-ever sample, any pose change, or the first
    // sample after a gap (continuity marker even if unchanged).
    const bool poseChanged=!r.hasPrevSample||std::memcmp(pose,r.prevPose,sizeof(pose))!=0;
    if(poseChanged||gapResume) {
        // Backstop: at most 256 gap-resume re-log appends per frame; the
        // IdentityEvent log itself stays capped with its own overflow
        // counter.
        if(gapResume&&gapRelogsThisFrame_>=256u) {
            ++summary_.gapRelogSkipped;
        } else if(samples_.size()<kPoseSampleCap) {
            PoseSample s;
            s.recordId=recordId;s.frame=frame;
            s.tx=tx;s.ty=ty;s.tz=tz;
            s.q0=q0;s.q1=q1;s.q2=q2;s.q3=q3;
            samples_.push_back(s);
            ++summary_.poseSamples;
            if(gapResume)++gapRelogsThisFrame_;
        } else ++summary_.poseSampleOverflow;
    }
    std::memcpy(r.prevPose,pose,sizeof(pose));
    r.hasPrevSample=true;
    r.lastSampledFrame=frame;
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

void KinematicEvalProbe::observe(uintptr_t descriptor,uintptr_t renderRecord,
                                 uint32_t jobMask) noexcept {
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
        r.jobMask=jobMask;
        r.epoch1b8=epoch1b8;r.descEpoch38=descEpoch38;
        r.firstFrame=frame;r.lastFrame=frame;r.calls=1;
        // No state from a partial read: a record straddling an unreadable
        // page would otherwise be created with zero-filled fields and a zero
        // transform baseline, and the first successful later read would
        // fabricate motion from zero to the real pose (2026-09-20 review
        // finding 2). The record simply gets created on a later call.
        if(!ok){++summary_.readFaults;return;}
        if(!guardedRead(static_cast<uintptr_t>(record)+0x130,r.xfFirst,sizeof(r.xfFirst))) {
            ++summary_.readFaults;return;
        }
        std::memcpy(r.xfLatest,r.xfFirst,sizeof(r.xfFirst));
        // First sample comes from the xfFirst block already read above.
        samplePoseLocked(static_cast<uint32_t>(records_.size()),r,r.xfFirst);
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
    r.jobMask|=jobMask;
    if(r.epoch1b8!=epoch1b8){++summary_.epochChanges;r.epoch1b8=epoch1b8;}
    r.descEpoch38=descEpoch38;
    // Identity BEFORE motion: a changed node ends the previous occupant's
    // history at this pointer; neither the xf diff nor the pose jump may
    // cross the boundary (2026-09-20 review finding 3: a node swap charged
    // the new occupant's pose as a 90 m jump plus an xf_mover on the old
    // occupant's record).
    bool identityReset=false;
    {
        bool nodeOk=true;
        const uint64_t nodeNow=read64(record+0x18,nodeOk);
        if(!nodeOk)++summary_.readFaults;
        else if(nodeNow!=r.node) {
            ++r.nodeChanges;++summary_.nodeChangeEvents;
            noteIdentityEventLocked(it->second,frame,2,0,r.node,nodeNow);
            r.node=nodeNow;
            r.hasPrevSample=false; // the boundary is never a jump
            identityReset=true;
        }
    }
    // Engine-truth motion: bit-exact compare of the world transform block.
    uint64_t xf[11];
    if(guardedRead(static_cast<uintptr_t>(record)+0x130,xf,sizeof(xf))) {
        if(identityReset) {
            // The new occupant's transform is its baseline, uncharged.
            std::memcpy(r.xfLatest,xf,sizeof(xf));
        } else if(std::memcmp(xf,r.xfLatest,sizeof(xf))!=0) {
            std::memcpy(r.xfLatest,xf,sizeof(xf));
            ++r.xfChanges;r.lastXfChangeFrame=frame;
            ++summary_.xfChanges;
            if(r.xfChanges==1)++summary_.xfMovers;
        }
        // Frame-aligned sampling rides the block already read above.
        samplePoseLocked(it->second,r,xf);
    } else ++summary_.readFaults;
    bool hashOk=true;
    const uint64_t hash=read64(record+0x268,hashOk);
    if(!hashOk) {
        // A faulted hash reads as zero: comparing it would emit a false
        // transition to zero and a second false one on recovery (finding
        // 2). Keep the last valid pair and skip the compare.
        ++summary_.readFaults;
    } else if(r.flags!=flags||r.hash!=hash) {
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

void KinematicEvalProbe::noteRigLink(uintptr_t rig,uintptr_t poseCtx) noexcept {
    if(!rig)return;
    if(!active_.load(std::memory_order_acquire))return;
    std::lock_guard<std::mutex> lock(mutex_);
    ++summary_.riglinkChecks;
    bool ok=true;
    // FUN_14431AFE0 param_1 IS the rig (decomp: state check at +0x380 first).
    const uint64_t collection=read64(rig+0x348,ok); // rig's collection slot
    const uint32_t rigState=read32(rig+0x380,ok);   // 4 = render-ready
    if(!ok){++summary_.readFaults;return;}
    if(rigState==4)++summary_.riglinkState4;
    if(!collection)++summary_.riglinkNullCollection;
    const uint32_t frame=frame_.load(std::memory_order_acquire);
    const auto it=riglinkIndex_.find(rig);
    if(it!=riglinkIndex_.end()) {
        RigLinkUse& r=riglinks_[it->second];
        ++r.hits;r.poseCtx=poseCtx;r.collection=collection;r.rigState=rigState;
        return;
    }
    if(riglinks_.size()>=kRigLinkCap){++summary_.riglinkOverflow;return;}
    RigLinkUse r;
    r.rig=rig;r.poseCtx=poseCtx;r.collection=collection;r.rigState=rigState;
    r.gameObj=read64(rig+0x20,ok);      // associated game-object interface
    r.descriptor=read64(rig+0x50,ok);   // rig descriptor
    if(collection) {
        r.owner=read64(collection+0x18,ok);    // shared-global owner candidate
        r.collEpoch90=read64(collection+0x90,ok);
    }
    if(!ok)++summary_.readFaults;
    r.firstFrame=frame;r.hits=1;
    riglinkIndex_.emplace(rig,static_cast<uint32_t>(riglinks_.size()));
    riglinks_.push_back(r);
}

void KinematicEvalProbe::notePhysQueue(uint32_t entryCount,uint32_t exitCount) noexcept {
    // exit < entry means the queue was drained or reset mid-run; only the
    // post-reset residue counts as appended, and the reset cadence itself
    // is the lifecycle evidence the node capture waits on.
    const bool reset=exitCount<entryCount;
    const uint32_t delta=reset?exitCount:exitCount-entryCount;
    physQueueRuns_.fetch_add(1,std::memory_order_relaxed);
    physQueueAppended_.fetch_add(delta,std::memory_order_relaxed);
    if(reset)physQueueResets_.fetch_add(1,std::memory_order_relaxed);
    uint32_t prev=physQueueMaxDelta_.load(std::memory_order_relaxed);
    while(prev<delta &&
          !physQueueMaxDelta_.compare_exchange_weak(prev,delta,std::memory_order_relaxed)){}
    // Only non-trivial pairs are kept: with the cap at 64, zero-delta runs
    // from a static scene would crowd out the reset/append pattern that
    // decodes the lifecycle. Zero runs still count in runs_/appended_.
    if(exitCount!=entryCount) {
        std::lock_guard<std::mutex> lock(mutex_);
        if(physQueueSamples_.size()<kPhysQueueSampleCap)
            physQueueSamples_.emplace_back(entryCount,exitCount);
    }
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
     <<",\"ownership_overflow\":"<<summary_.ownershipOverflow
     <<",\"riglink_checks\":"<<summary_.riglinkChecks
     <<",\"riglink_state4\":"<<summary_.riglinkState4
     <<",\"riglink_null_collection\":"<<summary_.riglinkNullCollection
     <<",\"riglink_overflow\":"<<summary_.riglinkOverflow
     <<",\"xf_movers\":"<<summary_.xfMovers
     <<",\"xf_changes\":"<<summary_.xfChanges
     <<",\"pose_samples\":"<<summary_.poseSamples
     <<",\"pose_sample_overflow\":"<<summary_.poseSampleOverflow
     <<",\"identity_event_overflow\":"<<summary_.identityEventOverflow
     <<",\"dup_in_frame\":"<<summary_.dupInFrame
     <<",\"gap_events\":"<<summary_.gapEvents
     <<",\"node_change_events\":"<<summary_.nodeChangeEvents
     <<",\"quat_change_frames\":"<<summary_.quatChangeFrames
     <<",\"frames_counted\":"<<summary_.framesCounted
     <<",\"zero_record_frames\":"<<summary_.zeroRecordFrames
     <<",\"min_frame_records\":"<<summary_.minFrameRecords
     <<",\"max_frame_records\":"<<summary_.maxFrameRecords
     <<",\"clock_sample_overflow\":"<<summary_.clockSampleOverflow
     <<",\"gap_relog_skipped\":"<<summary_.gapRelogSkipped
     <<",\"non_finite_pose\":"<<summary_.nonFinitePose
     <<"},"
     <<"\"vtables\":[";
    for(size_t i=0;i<vtables_.size();++i) {
        if(i)j<<',';
        j<<"{\"rva\":\"0x"<<std::hex<<vtables_[i].rva<<std::dec<<"\",\"first_frame\":"<<vtables_[i].firstFrame
         <<",\"hits\":"<<vtables_[i].hits<<'}';
    }
    j<<"],\"jobs\":[";
    for(uint32_t i=0;i<kJobCount;++i) {
        if(i)j<<',';
        // Seqlocked read: a job bracket commits calls/totalNs/maxNs between
        // two seq toggles outside this mutex; retry until the snapshot is
        // coherent (2026-09-20 review finding 8). Bounded: a stuck writer
        // (impossible absent a killed thread) must not hang the dump.
        uint64_t calls=0,total=0,mx=0;
        for(uint32_t tries=0;;++tries) {
            const uint32_t s0=jobs_[i].seq.load(std::memory_order_acquire);
            if((s0&1u)&&tries<10000000u)continue;
            calls=jobs_[i].calls.load(std::memory_order_relaxed);
            total=jobs_[i].totalNs.load(std::memory_order_relaxed);
            mx=jobs_[i].maxNs.load(std::memory_order_relaxed);
            std::atomic_thread_fence(std::memory_order_acquire);
            if(jobs_[i].seq.load(std::memory_order_acquire)==s0)break;
            if(tries>=10000000u)break;
        }
        j<<"{\"name\":\""<<kJobNames[i]<<"\",\"calls\":"<<calls<<",\"total_ns\":"<<total
         <<",\"mean_ns\":"<<(calls?total/calls:0)
         <<",\"max_ns\":"<<mx<<'}';
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
         <<",\"job_mask\":"<<r.jobMask
         <<",\"epoch1b8\":\"0x"<<std::hex<<r.epoch1b8
         <<"\",\"desc_epoch38\":\"0x"<<r.descEpoch38<<std::dec
         <<"\",\"xf_changes\":"<<r.xfChanges<<",\"last_xf_change\":"<<r.lastXfChangeFrame
         <<",\"frames_sampled\":"<<r.framesSampled
         <<",\"dup_in_frame\":"<<r.dupInFrame
         <<",\"gaps\":"<<r.gaps
         <<",\"max_gap\":"<<r.maxGap
         <<",\"node_changes\":"<<r.nodeChanges
         <<",\"quat_change_frames\":"<<r.quatChangeFrames
         <<",\"max_jump\":"<<r.maxJump
         <<",\"total_jump\":"<<r.totalJump
         <<",\"xf_first\":[";
        for(int k=0;k<11;++k){if(k)j<<',';j<<"\"0x"<<std::hex<<r.xfFirst[k]<<std::dec<<"\"";}
        j<<"],\"xf_latest\":[";
        for(int k=0;k<11;++k){if(k)j<<',';j<<"\"0x"<<std::hex<<r.xfLatest[k]<<std::dec<<"\"";}
        j<<"]}";
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
    j<<"],\"riglinks\":[";
    for(size_t i=0;i<riglinks_.size();++i) {
        if(i)j<<',';
        const RigLinkUse& r=riglinks_[i];
        j<<"{\"rig\":\"0x"<<std::hex<<r.rig<<"\",\"pose_ctx\":\"0x"<<r.poseCtx
         <<"\",\"collection\":\"0x"<<r.collection<<"\",\"game_obj\":\"0x"<<r.gameObj
         <<"\",\"descriptor\":\"0x"<<r.descriptor<<"\",\"owner\":\"0x"<<r.owner
         <<"\",\"coll_epoch90\":\"0x"<<r.collEpoch90<<std::dec
         <<"\",\"rig_state\":"<<r.rigState<<",\"first_frame\":"<<r.firstFrame
         <<",\"hits\":"<<r.hits
         <<",\"collection_known\":"<<(r.collection&&ownershipIndex_.count(r.collection)?1:0)<<'}';
    }
    j<<"],\"pose_events\":[";
    for(size_t i=0;i<events_.size();++i) {
        if(i)j<<',';
        const IdentityEvent& e=events_[i];
        j<<"{\"record\":"<<e.recordId<<",\"frame\":"<<e.frame
         <<",\"kind\":"<<e.kind<<",\"gap_len\":"<<e.gapLen
         <<",\"old_node\":\"0x"<<std::hex<<e.oldNode
         <<"\",\"new_node\":\"0x"<<e.newNode<<std::dec<<"\"}";
    }
    j<<"],\"mover_samples\":[";
    for(size_t i=0;i<samples_.size();++i) {
        if(i)j<<',';
        const PoseSample& s=samples_[i];
        uint32_t txb,tyb,tzb;
        std::memcpy(&txb,&s.tx,4);std::memcpy(&tyb,&s.ty,4);std::memcpy(&tzb,&s.tz,4);
        j<<"{\"record\":"<<s.recordId<<",\"frame\":"<<s.frame
         <<",\"t\":[\"0x"<<std::hex<<txb<<"\",\"0x"<<tyb<<"\",\"0x"<<tzb<<std::dec<<"\"]"
         <<",\"q\":["<<s.q0<<','<<s.q1<<','<<s.q2<<','<<s.q3<<"]}";
    }
    j<<"],\"clock_samples\":[";
    for(size_t i=0;i<clockSamples_.size();++i) {
        if(i)j<<',';
        j<<"{\"present\":"<<clockSamples_[i].present
         <<",\"mesh\":"<<clockSamples_[i].mesh<<'}';
    }
    // Per-session counters (relaxed atomics written outside this mutex): a
    // torn cross-field read is acceptable -- flight analysis reads orders
    // of magnitude, and the gate's fixture runs quiescent.
    j<<"],\"phys_queue\":{\"runs\":"<<physQueueRuns_.load(std::memory_order_relaxed)
     <<",\"appended\":"<<physQueueAppended_.load(std::memory_order_relaxed)
     <<",\"max_delta\":"<<physQueueMaxDelta_.load(std::memory_order_relaxed)
     <<",\"resets\":"<<physQueueResets_.load(std::memory_order_relaxed)
     <<",\"samples\":[";
    for(size_t i=0;i<physQueueSamples_.size();++i) {
        if(i)j<<',';
        j<<"{\"entry\":"<<physQueueSamples_[i].first
         <<",\"exit\":"<<physQueueSamples_[i].second<<'}';
    }
    j<<"]}}"; // samples, phys_queue, kinematicEval
}

void KinematicEvalProbe::selfTestPopulateForJson() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    clearLocked();
    // Every value is distinctive so tools\kinematic_json_selftest.py can
    // assert an exact round-trip. The three historical writer bugs each
    // dropped or misplaced a quote: after a hex value, inside an array,
    // at record termination -- so the fixture exercises all three shapes.
    RecordState mover{};
    mover.record=0x1234ABCD5678EF00ull;
    mover.node=0x1111222233334444ull;
    mover.poseCtx=0xAABBCCDDEEFF0011ull;
    mover.predPtr=0x7777888899990000ull;
    mover.predVtableRva=0x4301234ull;
    mover.pred2Ptr=0x5555666677778888ull;
    mover.pred2VtableRva=0x4305678ull;
    mover.hash=0xDEADBEEFCAFEF00Dull;
    mover.count298=0x1Full;
    mover.epoch1b8=0x1122334455667788ull;
    mover.descEpoch38=0x8877665544332211ull;
    for(int k=0;k<11;++k){mover.xfFirst[k]=0x1000ull+uint64_t(k);mover.xfLatest[k]=0x1000ull+uint64_t(k);}
    mover.xfLatest[8]=0x0000800000008000ull;   // translation word moved
    mover.xfLatest[9]=0x0000FFFF00008000ull;
    mover.xfLatest[10]=0x000080000000FFFFull;  // packed-quat word changed
    mover.flags=0xC3u;
    mover.firstFrame=10u;mover.lastFrame=42u;
    mover.xfChanges=7u;mover.lastXfChangeFrame=42u;
    mover.bool234=1u;mover.predByte=0x5Au;mover.gate2=2u;
    mover.jobMask=0x15u; // render + both physics-side bits, distinctive
    mover.calls=123u;
    mover.framesSampled=40u;mover.dupInFrame=2u;
    mover.gaps=1u;mover.maxGap=3u;mover.nodeChanges=1u;mover.quatChangeFrames=6u;
    mover.maxJump=0.125f;mover.totalJump=4.5;
    mover.lastSampledFrame=42u;mover.hasPrevSample=true;

    RecordState still{};
    still.record=0x2222333344445555ull;
    still.node=0x6666777788889999ull;
    for(int k=0;k<11;++k){still.xfFirst[k]=0x0000800000008000ull;still.xfLatest[k]=0x0000800000008000ull;}
    still.flags=0x41u;
    still.jobMask=0x1u; // render-only, distinctive from the mover's 0x15
    still.firstFrame=10u;still.lastFrame=42u;
    still.calls=120u;
    still.framesSampled=40u;still.lastSampledFrame=42u;still.hasPrevSample=true;

    index_[mover.record]=0;index_[still.record]=1;
    records_.push_back(mover);records_.push_back(still);

    Transition t{};
    t.recordId=0u;t.frame=42u;t.oldFlags=0x1u;t.newFlags=0xC3u;
    t.oldHash=0x1111111111111111ull;t.newHash=mover.hash;
    transitions_.push_back(t);

    VtableUse v{};
    v.rva=0x4301234ull;v.firstFrame=10u;v.hits=123u;
    vtableIndex_[v.rva]=0;vtables_.push_back(v);

    OwnershipUse o{};
    o.collection=0xAAAA0000BBBB0001ull;
    o.owner=0xAAAA0000BBBB0002ull;
    o.alias20=0xAAAA0000BBBB0003ull;
    o.epoch90=0xEEEEull;
    o.backPtr=o.collection;
    o.ownerState=4u;o.jobId=1u;o.firstFrame=10u;o.hits=55u;
    ownershipIndex_[o.collection]=0;ownerships_.push_back(o);

    RigLinkUse rl{};
    rl.rig=0xF00D000000000001ull;
    rl.poseCtx=0xF00D000000000002ull;
    rl.collection=o.collection;      // known -> collection_known = 1
    rl.gameObj=0xF00D000000000003ull;
    rl.descriptor=0xF00D000000000004ull;
    rl.owner=0xF00D000000000005ull;
    rl.collEpoch90=0xEEEEull;
    rl.rigState=4u;rl.firstFrame=10u;rl.hits=58u;
    riglinkIndex_[rl.rig]=0;riglinks_.push_back(rl);

    PoseSample s1{};
    s1.recordId=0u;s1.frame=42u;
    s1.tx=1.0f;s1.ty=2.0f;s1.tz=3.0f;  // bits 0x3F800000/0x40000000/0x40400000
    s1.q0=32768u;s1.q1=32768u;s1.q2=32768u;s1.q3=65535u;
    samples_.push_back(s1);
    PoseSample s2{};
    s2.recordId=0u;s2.frame=43u;
    s2.tx=0.5f;s2.ty=2.0f;s2.tz=3.0f;  // tx bits 0x3F000000
    s2.q0=32768u;s2.q1=32768u;s2.q2=32768u;s2.q3=65534u;
    samples_.push_back(s2);

    IdentityEvent e1{};
    e1.recordId=1u;e1.frame=30u;e1.kind=1u;e1.gapLen=3u;
    e1.oldNode=still.node;e1.newNode=still.node;
    events_.push_back(e1);
    IdentityEvent e2{};
    e2.recordId=0u;e2.frame=41u;e2.kind=2u;
    e2.oldNode=0x1111222233334444ull;e2.newNode=0x99990000AAAABBBBull;
    events_.push_back(e2);

    // Same mesh value twice: the staleness signature from flight 083323.
    ClockSample c1{};c1.present=1001u;c1.mesh=13081u;clockSamples_.push_back(c1);
    ClockSample c2{};c2.present=1002u;c2.mesh=13081u;clockSamples_.push_back(c2);

    jobs_[0].calls.store(2,std::memory_order_relaxed);
    jobs_[0].totalNs.store(1000,std::memory_order_relaxed);
    jobs_[0].maxNs.store(700,std::memory_order_relaxed);
    jobs_[3].calls.store(1,std::memory_order_relaxed);
    jobs_[3].totalNs.store(42,std::memory_order_relaxed);
    jobs_[3].maxNs.store(42,std::memory_order_relaxed);

    // max_delta 5 exceeds both kept samples' deltas (4, 1): the counter is
    // independent of the sample cap, and the gate asserts exactly that.
    physQueueRuns_.store(3,std::memory_order_relaxed);
    physQueueAppended_.store(7,std::memory_order_relaxed);
    physQueueMaxDelta_.store(5,std::memory_order_relaxed);
    physQueueResets_.store(1,std::memory_order_relaxed);
    physQueueSamples_.push_back({10u,14u});
    physQueueSamples_.push_back({20u,21u});

    summary_.observed=987654321ull;
    summary_.readFaults=3;summary_.recordOverflow=1;summary_.transitionOverflow=2;summary_.vtableOverflow=4;
    summary_.transitions=1;summary_.vtables=1;
    summary_.epochMatches=11;summary_.epochMismatches=22;summary_.epochChanges=33;
    summary_.ownershipChecks=44;summary_.ownerBackPtrMatch=45;summary_.ownerState4=46;summary_.ownershipOverflow=47;
    summary_.riglinkChecks=48;summary_.riglinkState4=49;summary_.riglinkNullCollection=50;summary_.riglinkOverflow=51;
    summary_.xfMovers=52;summary_.xfChanges=53;
    summary_.poseSamples=2;summary_.poseSampleOverflow=5;summary_.identityEventOverflow=7;
    summary_.dupInFrame=2;summary_.gapEvents=1;summary_.nodeChangeEvents=1;summary_.quatChangeFrames=6;
    summary_.framesCounted=40;summary_.zeroRecordFrames=1;
    summary_.minFrameRecords=2;summary_.maxFrameRecords=17;
    summary_.clockSampleOverflow=3;
    summary_.gapRelogSkipped=11;
    summary_.nonFinitePose=9;
}

} // namespace edvr
