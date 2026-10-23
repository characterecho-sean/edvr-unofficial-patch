#pragma once
// Diagnostic only: game-owned current rigid poses around a flat camera-origin jump.
// The injected previous pose is deliberately absent: EDVR synthesizes it.
#include "flat_origin_capture.h"
#include <cstddef>
#include <cstdint>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>
#include "../common/log.h"

namespace edvr {

struct FlatOriginWitnessSample {
    uintptr_t record=0, node=0, owner=0;
    uint32_t pose[5]{}; // raw record+0x170 position, +0x17c packed quaternion
    bool proven=false, primary=false, changedWithinFrame=false;
};

struct FlatOriginWitnessReport {
    enum class Verdict : uint8_t { Inconclusive, UnchangedRecords, CoherentRecordShift };
    uint64_t resolverFrame=0;
    uint32_t engineAtEvent=0, sampleFrame=0;
    bool clockMatch=false;
    float now[3]{}, previous[3]{}, cameraDelta[3]{};
    float medianDelta[3]{}, cameraMinusMedian[3]{};
    bool worldEdgeValid=false, cameraMinusWorldValid=false;
    float edgeMedian[5][3]{};
    uint16_t edgeCount[5]{},edgeCoherent[5]{};
    float farEdgeMedian[5][3]{};
    uint16_t farEdgeCount[5]{},farEdgeCoherent[5]{};
    float deltaMs=0;
    uint16_t preStable=0, matchedAt=0, coherent=0, owners=0, differentOwners=0;
    uint16_t farRecords=0,farOwners=0;
    uint16_t missingAt=0, changedQuat=0, movingOutliers=0, sameFrameChanges=0;
    uint32_t observed[6]{}, occupied[6]{}, collisions[6]{}, skippedLock=0;
    bool jumpKept=false, classifiedReanchor=false, reset=false, unavailable=false;
    bool hadFrame[6]{}; // -3,-2,-1,0,+1,+2
    char reason[64]{};
    Verdict verdict=Verdict::Inconclusive;
    struct Row {
        uintptr_t record=0,node=0,owner=0;
        float pos[6][3]{};
        float distanceM=0;
        uint8_t present=0,proven=0,quatChanged=0,primary=0;
        bool preStable=false;
    } rows[8]{};
    uint8_t rowCount=0;
};

// Pure bounded collector, also used by the focused rig. All calls are serialized
// by the runtime's short try-lock wrapper; this class does no I/O or allocation.
class FlatOriginWitnessCore {
public:
    static constexpr uint32_t kFrames=8, kSlots=256, kPending=16;
    void observe(uintptr_t record,uintptr_t node,uintptr_t owner,const uint32_t pose[5],uint32_t frame,
                 bool proven,bool primary) noexcept;
    bool event(uint64_t resolverFrame,uint32_t engineAtEvent,const float now[3],const float previous[3],
               float deltaMs,bool kept,bool reanchor,bool reset,const char* unavailableReason=nullptr) noexcept;
    // Return one completed report per call; call repeatedly until false.
    bool complete(uint32_t engineFrame,FlatOriginWitnessReport& out,bool force=false) noexcept;
    uint32_t droppedNear() const noexcept { return droppedShortToward_; }
    uint32_t droppedLongToward() const noexcept { return droppedLongToward_; }
    uint32_t droppedOrdinary() const noexcept { return droppedOrdinary_; }
    uint32_t droppedUnavailable() const noexcept { return droppedUnavailableShort_+droppedUnavailableOther_; }
    uint32_t droppedUnavailableShort() const noexcept { return droppedUnavailableShort_; }
    uint32_t droppedUnavailableOther() const noexcept { return droppedUnavailableOther_; }
    uint32_t pending() const noexcept { return pendingCount_; }
    uint32_t events() const noexcept { return events_; }
    void setSkippedLock(uint32_t count) noexcept { skippedLock_=count; }
    void clear() noexcept;
private:
    struct Frame {
        uint32_t stamp=~0u,observed=0,occupied=0,collisions=0;
        FlatOriginWitnessSample slots[kSlots]{};
    } frames_[kFrames]{};
    struct Event {
        uint64_t resolverFrame=0;
        uint32_t engineAtEvent=0,sampleFrame=0;
        float now[3]{},previous[3]{},deltaMs=0;
        bool kept=false,reanchor=false,reset=false,unavailable=false;
        char reason[64]{};
    } pending_[kPending]{};
    uint32_t pendingCount_=0, events_=0;
    uint32_t shortTowardUsed_=0,longTowardUsed_=0,ordinaryUsed_=0,unavailableShortUsed_=0,unavailableOtherUsed_=0;
    uint32_t droppedShortToward_=0,droppedLongToward_=0,droppedOrdinary_=0,droppedUnavailableShort_=0,droppedUnavailableOther_=0,skippedLock_=0;
    Frame* frame(uint32_t stamp) noexcept;
    const Frame* find(uint32_t stamp) const noexcept;
    static uint64_t hash(uintptr_t record,uintptr_t node) noexcept;
    static const FlatOriginWitnessSample* findSample(const Frame*,uintptr_t record,uintptr_t node) noexcept;
    static void position(const uint32_t pose[5],float out[3]) noexcept;
    void assess(const Event&,FlatOriginWitnessReport&) const noexcept;
};

// Runtime API. The frame tick comes from the unconditional owned-Present clock;
// an inactive emit hook is therefore visible even when it never calls observe.
void flatOriginWitnessObserve(uintptr_t record,uintptr_t node,uintptr_t owner,const uint32_t pose[5],
                              uint32_t engineFrame,bool proven,bool primary) noexcept;
void flatOriginWitnessFrameBoundary(uint32_t engineFrame,bool flatProfile,bool emitHookLive) noexcept;
void flatOriginWitnessEvent(uint64_t resolverFrame,const float now[3],const float previous[3],float deltaMs,
                            bool jumpKept,bool classifiedReanchor,bool reset) noexcept;
void flatOriginWitnessUnavailable(uint64_t resolverFrame,const float now[3],const float previous[3],
                                  float deltaMs,const char* reason) noexcept;
void flatOriginWitnessShutdown() noexcept;

} // namespace edvr

namespace edvr {

inline uint64_t FlatOriginWitnessCore::hash(uintptr_t record,uintptr_t node) noexcept {
    uint64_t x=static_cast<uint64_t>(record)^(static_cast<uint64_t>(node)*0x9E3779B97F4A7C15ull);
    x^=x>>33;x*=0xff51afd7ed558ccdull;x^=x>>33;x*=0xc4ceb9fe1a85ec53ull;return x^(x>>33);
}
inline FlatOriginWitnessCore::Frame* FlatOriginWitnessCore::frame(uint32_t stamp) noexcept {
    Frame& f=frames_[stamp%kFrames];
    if(f.stamp!=stamp){f=Frame{};f.stamp=stamp;}
    return &f;
}
inline const FlatOriginWitnessCore::Frame* FlatOriginWitnessCore::find(uint32_t stamp) const noexcept {
    const Frame& f=frames_[stamp%kFrames];return f.stamp==stamp?&f:nullptr;
}
inline const FlatOriginWitnessSample* FlatOriginWitnessCore::findSample(const Frame* f,uintptr_t record,uintptr_t node) noexcept {
    if(!f)return nullptr;
    const auto& s=f->slots[hash(record,node)%kSlots];
    return s.record==record && s.node==node?&s:nullptr;
}
inline void FlatOriginWitnessCore::position(const uint32_t pose[5],float out[3]) noexcept {
    for(unsigned i=0;i<3;++i)std::memcpy(&out[i],pose+i,4);
}
inline void FlatOriginWitnessCore::observe(uintptr_t record,uintptr_t node,uintptr_t owner,const uint32_t pose[5],
                                           uint32_t stamp,bool proven,bool primary) noexcept {
    if(!record || !node || !pose)return;
    Frame& f=*frame(stamp);++f.observed;
    const uint64_t h=hash(record,node);
    FlatOriginWitnessSample& s=f.slots[h%kSlots];
    if(s.record && (s.record!=record || s.node!=node)) {
        ++f.collisions;
        // Deterministic minimum-rank occupant: a later stable record can
        // replace a busy earlier one, while the same identity maps every tick.
        if(h>=hash(s.record,s.node))return;
    } else if(!s.record)++f.occupied;
    const bool changed=s.record==record && s.node==node && std::memcmp(s.pose,pose,5*4)!=0;
    const bool wasChanged=s.record==record && s.node==node && s.changedWithinFrame;
    s.record=record;s.node=node;s.owner=owner;
    std::memcpy(s.pose,pose,5*4);
    s.proven=proven;s.primary=primary;s.changedWithinFrame=changed||wasChanged;
}
inline bool FlatOriginWitnessCore::event(uint64_t resolverFrame,uint32_t engineAtEvent,const float now[3],
                                        const float previous[3],float deltaMs,bool kept,bool reanchor,bool reset,
                                        const char* unavailableReason) noexcept {
    if(!now || !previous)return false;
    const bool unavailable=unavailableReason!=nullptr;
    const double nx=now[0],ny=now[1],nz=now[2],px=previous[0],py=previous[1],pz=previous[2];
    const double step=std::sqrt((nx-px)*(nx-px)+(ny-py)*(ny-py)+(nz-pz)*(nz-pz));
    if(!std::isfinite(step) || step<50.0)return false;
    const bool nearOrigin=std::sqrt(nx*nx+ny*ny+nz*nz)<std::sqrt(px*px+py*py+pz*pz) &&
                    std::sqrt(nx*nx+ny*ny+nz*nz)<step;
    const bool shortToward=nearOrigin && step<=512.0;
    uint32_t *used=unavailable?(shortToward?&unavailableShortUsed_:&unavailableOtherUsed_):
        (shortToward?&shortTowardUsed_:(nearOrigin?&longTowardUsed_:&ordinaryUsed_));
    uint32_t *dropped=unavailable?(shortToward?&droppedUnavailableShort_:&droppedUnavailableOther_):
        (shortToward?&droppedShortToward_:(nearOrigin?&droppedLongToward_:&droppedOrdinary_));
    const uint32_t budget=unavailable?(shortToward?8u:4u):(shortToward?16u:8u);
    const uint32_t pendingLimit=shortToward?kPending:kPending-4;
    if(*used>=budget || pendingCount_>=pendingLimit){++*dropped;return false;}
    ++*used;++events_;
    Event& e=pending_[pendingCount_++];e=Event{};
    e.resolverFrame=resolverFrame;e.engineAtEvent=engineAtEvent;
    // flatRuntimePresent names the next draw frame after Present; the resolver
    // for that frame and the emit clock should have the same present stamp.
    e.sampleFrame=static_cast<uint32_t>(resolverFrame);
    std::memcpy(e.now,now,sizeof(e.now));std::memcpy(e.previous,previous,sizeof(e.previous));
    e.deltaMs=deltaMs;e.kept=kept;e.reanchor=reanchor;e.reset=reset;e.unavailable=unavailable;
    if(unavailableReason){std::strncpy(e.reason,unavailableReason,sizeof(e.reason)-1);e.reason[sizeof(e.reason)-1]=0;}
    return true;
}
inline void FlatOriginWitnessCore::assess(const Event& e,FlatOriginWitnessReport& r) const noexcept {
    r=FlatOriginWitnessReport{};
    r.resolverFrame=e.resolverFrame;r.engineAtEvent=e.engineAtEvent;r.sampleFrame=e.sampleFrame;
    r.clockMatch=static_cast<uint32_t>(e.resolverFrame)==e.engineAtEvent;
    std::memcpy(r.now,e.now,sizeof(r.now));std::memcpy(r.previous,e.previous,sizeof(r.previous));
    r.deltaMs=e.deltaMs;r.jumpKept=e.kept;r.classifiedReanchor=e.reanchor;r.reset=e.reset;r.unavailable=e.unavailable;
    std::memcpy(r.reason,e.reason,sizeof(r.reason));r.skippedLock=skippedLock_;
    for(unsigned c=0;c<3;++c)r.cameraDelta[c]=e.now[c]-e.previous[c];
    const Frame* fs[6]{};
    for(unsigned i=0;i<6;++i){fs[i]=find(e.sampleFrame+i-3);r.hadFrame[i]=fs[i]!=nullptr;if(fs[i]){
        r.observed[i]=fs[i]->observed;r.occupied[i]=fs[i]->occupied;r.collisions[i]=fs[i]->collisions;
    }}
    const Frame* source=fs[1]?fs[1]:fs[2];
    if(!source)return;
    uintptr_t ownerIds[kSlots]{},farOwnerIds[kSlots]{};unsigned ownerN=0,farOwnerN=0;
    float edges[5][3][kSlots]{},farEdges[5][3][kSlots]{};
    unsigned edgeN[5]{},farEdgeN[5]{};
    // Two passes put baseline-stable records in the bounded raw row printout
    // first, without hiding moving or identity-reused records altogether.
    for(unsigned pass=0;pass<2;++pass)for(const auto& candidate:source->slots){
        if(!candidate.record)continue;
        const auto* s1=findSample(fs[1],candidate.record,candidate.node);
        const auto* s2=findSample(fs[2],candidate.record,candidate.node);
        const auto* s0=findSample(fs[0],candidate.record,candidate.node);
        const bool stableEarly=s0 && s1 && s0->proven && s1->proven && !s0->changedWithinFrame &&
            !s1->changedWithinFrame && s0->owner==s1->owner && std::memcmp(s0->pose,s1->pose,5*4)==0;
        const bool stableLate=s1 && s2 && s1->proven && s2->proven && !s1->changedWithinFrame &&
            !s2->changedWithinFrame && s1->owner==s2->owner && std::memcmp(s1->pose,s2->pose,5*4)==0;
        const bool stable=stableEarly||stableLate;
        if((pass==0)!=stable)continue;
        if(pass==0){
            ++r.preStable;
            bool newOwner=true;for(unsigned j=0;j<ownerN;++j)if(ownerIds[j]==candidate.owner)newOwner=false;
            if(newOwner)ownerIds[ownerN++]=candidate.owner;
        }
        const FlatOriginWitnessSample* samples[6]{};
        for(unsigned i=0;i<6;++i)samples[i]=findSample(fs[i],candidate.record,candidate.node);
        float distanceM=0;
        const auto* spatial=samples[2]?samples[2]:(samples[1]?samples[1]:samples[0]);
        if(spatial){float p[3];position(spatial->pose,p);float d2=0;for(unsigned c=0;c<3;++c){const float d=p[c]-e.previous[c];d2+=d*d;}
            if(std::isfinite(d2))distanceM=std::sqrt(d2);}
        if(stable && distanceM>=250.0f){++r.farRecords;bool newFarOwner=true;
            for(unsigned j=0;j<farOwnerN;++j)if(farOwnerIds[j]==candidate.owner)newFarOwner=false;
            if(newFarOwner)farOwnerIds[farOwnerN++]=candidate.owner;}
        if(stable){
            if(!samples[3] || !samples[3]->proven){++r.missingAt;}
            else {++r.matchedAt;if(samples[3]->changedWithinFrame)++r.sameFrameChanges;
                if(std::memcmp(samples[3]->pose+3,candidate.pose+3,8)!=0)++r.changedQuat;}
            for(unsigned edge=0;edge<5;++edge){
                const auto* a=samples[edge];const auto* b=samples[edge+1];
                if(!a || !b || !a->proven || !b->proven || a->changedWithinFrame || b->changedWithinFrame ||
                   a->owner!=b->owner || std::memcmp(a->pose+3,b->pose+3,8)!=0)continue;
                float p[3],q[3];position(a->pose,p);position(b->pose,q);
                bool finite=true;for(unsigned c=0;c<3;++c)finite=finite&&std::isfinite(p[c])&&std::isfinite(q[c]);
                if(!finite)continue;
                for(unsigned c=0;c<3;++c)edges[edge][c][edgeN[edge]]=q[c]-p[c];
                ++edgeN[edge];
                if(distanceM>=250.0f){for(unsigned c=0;c<3;++c)farEdges[edge][c][farEdgeN[edge]]=q[c]-p[c];
                    ++farEdgeN[edge];}
            }
        }
        int rowIndex=r.rowCount<8?static_cast<int>(r.rowCount++):-1;
        if(rowIndex<0 && stable){
            bool ownerPrinted=false;for(unsigned j=0;j<r.rowCount;++j)if(r.rows[j].owner==candidate.owner)ownerPrinted=true;
            float worst=1e30f;
            for(unsigned j=0;j<r.rowCount;++j){
                unsigned same=0;for(unsigned k=0;k<r.rowCount;++k)if(r.rows[k].owner==r.rows[j].owner)++same;
                if((!ownerPrinted && same>1) || (ownerPrinted && r.rows[j].owner==candidate.owner && distanceM>r.rows[j].distanceM+100.0f))
                    if(r.rows[j].distanceM<worst){worst=r.rows[j].distanceM;rowIndex=static_cast<int>(j);}
            }
        }
        if(rowIndex>=0){auto& row=r.rows[rowIndex];row=FlatOriginWitnessReport::Row{};
            row.record=candidate.record;row.node=candidate.node;row.owner=candidate.owner;row.preStable=stable;row.distanceM=distanceM;
            for(unsigned i=0;i<6;++i)if(samples[i]){
                row.present|=static_cast<uint8_t>(1u<<i);position(samples[i]->pose,row.pos[i]);
                if(samples[i]->proven)row.proven|=static_cast<uint8_t>(1u<<i);
                if(samples[i]->primary)row.primary|=static_cast<uint8_t>(1u<<i);
                if(samples[i]->changedWithinFrame || std::memcmp(samples[i]->pose+3,candidate.pose+3,8)!=0)
                    row.quatChanged|=static_cast<uint8_t>(1u<<i);
            }}
        if(pass==1 && r.rowCount>=8)break;
    }
    r.owners=static_cast<uint16_t>(ownerN);r.differentOwners=ownerN?static_cast<uint16_t>(ownerN-1):0;
    r.farOwners=static_cast<uint16_t>(farOwnerN);
    for(unsigned edge=0;edge<5;++edge){
        r.edgeCount[edge]=static_cast<uint16_t>(edgeN[edge]);
        r.farEdgeCount[edge]=static_cast<uint16_t>(farEdgeN[edge]);
        if(edgeN[edge]){
            for(unsigned c=0;c<3;++c){float sorted[kSlots];std::memcpy(sorted,edges[edge][c],edgeN[edge]*sizeof(float));
                std::sort(sorted,sorted+edgeN[edge]);r.edgeMedian[edge][c]=sorted[edgeN[edge]/2];}
            const float* m=r.edgeMedian[edge];
            for(unsigned j=0;j<edgeN[edge];++j){float d2=0;for(unsigned c=0;c<3;++c){const float d=edges[edge][c][j]-m[c];d2+=d*d;}
                if(d2<=0.25f)++r.edgeCoherent[edge];}
        }
        if(farEdgeN[edge]){
            for(unsigned c=0;c<3;++c){float sorted[kSlots];std::memcpy(sorted,farEdges[edge][c],farEdgeN[edge]*sizeof(float));
                std::sort(sorted,sorted+farEdgeN[edge]);r.farEdgeMedian[edge][c]=sorted[farEdgeN[edge]/2];}
            const float* m=r.farEdgeMedian[edge];
            for(unsigned j=0;j<farEdgeN[edge];++j){float d2=0;for(unsigned c=0;c<3;++c){const float d=farEdges[edge][c][j]-m[c];d2+=d*d;}
                if(d2<=0.25f)++r.farEdgeCoherent[edge];}
        }
    }
    // Only the exact -1->0 edge describes this event. Neighbor-edge motion
    // remains raw evidence of a clock/producer offset, not a reanchor label.
    r.worldEdgeValid=r.edgeCount[2]>0;
    r.cameraMinusWorldValid=r.worldEdgeValid && r.clockMatch;
    if(r.worldEdgeValid)std::memcpy(r.medianDelta,r.edgeMedian[2],sizeof(r.medianDelta));
    if(r.cameraMinusWorldValid)for(unsigned c=0;c<3;++c)r.cameraMinusMedian[c]=r.cameraDelta[c]-r.medianDelta[c];
    r.coherent=r.edgeCoherent[2];
    r.movingOutliers=r.edgeCount[2]-r.edgeCoherent[2];
    if(!r.clockMatch || ownerN<2 || r.farOwners<2 || r.preStable<3 || r.edgeCount[1]<3 || r.edgeCount[2]<3 ||
       r.edgeCount[3]<3 || r.edgeCoherent[2]*4<r.edgeCount[2]*3)return;
    const auto length=[](const float a[3]){return std::sqrt(a[0]*a[0]+a[1]*a[1]+a[2]*a[2]);};
    if(r.farEdgeCount[2]<2 || r.farEdgeCoherent[2]*4<r.farEdgeCount[2]*3)return;
    float farResidual[3];for(unsigned c=0;c<3;++c)farResidual[c]=r.farEdgeMedian[2][c]-r.edgeMedian[2][c];
    if(length(farResidual)>0.5f)return;
    const float atM=length(r.edgeMedian[2]);
    if(length(r.edgeMedian[1])>0.5f || length(r.edgeMedian[3])>0.5f)return; // phase not established
    if(atM<=0.5f)r.verdict=FlatOriginWitnessReport::Verdict::UnchangedRecords;
    else if(atM>=1.0f)r.verdict=FlatOriginWitnessReport::Verdict::CoherentRecordShift;
}
inline bool FlatOriginWitnessCore::complete(uint32_t engineFrame,FlatOriginWitnessReport& out,bool force) noexcept {
    if(!pendingCount_)return false;
    // Start of frame sample+3 means the +2 draw frame has finished.
    if(!force && engineFrame<pending_[0].sampleFrame+3)return false;
    assess(pending_[0],out);
    for(unsigned j=1;j<pendingCount_;++j)pending_[j-1]=pending_[j];
    --pendingCount_;return true;
}
inline void FlatOriginWitnessCore::clear() noexcept {
    for(auto& f:frames_)f=Frame{};
    pendingCount_=events_=shortTowardUsed_=longTowardUsed_=ordinaryUsed_=unavailableShortUsed_=unavailableOtherUsed_=0;
    droppedShortToward_=droppedLongToward_=droppedOrdinary_=droppedUnavailableShort_=droppedUnavailableOther_=skippedLock_=0;
}

namespace flat_origin_witness_runtime {
inline std::mutex mutex;
inline FlatOriginWitnessCore core;
inline std::atomic<uint32_t> skipped{0};
inline std::atomic<uint32_t> callbacks{0},selected{0},duplicate{0};
inline std::atomic<uint32_t> clock{0};
inline std::atomic<bool> active{false};
inline bool hookLive=false;
inline uint32_t heartbeat=0;
inline void print(const FlatOriginWitnessReport& r) noexcept {
    const char* verdict=r.verdict==FlatOriginWitnessReport::Verdict::UnchangedRecords?"unchanged-game-record-poses":
        r.verdict==FlatOriginWitnessReport::Verdict::CoherentRecordShift?"coherent-game-record-shift":"inconclusive";
    Log::get().note("flat origin witness: resolver-frame=%llu engine-at-event=%u sample-frame=%u clocks-match=%u pose-pattern=%s decision-stage=pre-backend planned-kept=%u reanchor-rule=%u planned-reset=%u unavailable=%u reason=%s delta-ms=%.9g camera-now=(%.9g,%.9g,%.9g) camera-previous=(%.9g,%.9g,%.9g) camera-delta=(%.9g,%.9g,%.9g) world-edge-valid=%u world-edge(-1>0)=(%.9g,%.9g,%.9g) camera-minus-world-valid=%u camera-minus-world=(%.9g,%.9g,%.9g) pre-stable=%u matched-at=%u coherent=%u owners=%u far250m=%u far-owners=%u moving-outliers=%u missing-at=%u quat-changed=%u same-frame=%u",
        static_cast<unsigned long long>(r.resolverFrame),r.engineAtEvent,r.sampleFrame,r.clockMatch?1u:0u,verdict,
        r.jumpKept?1u:0u,r.classifiedReanchor?1u:0u,r.reset?1u:0u,r.unavailable?1u:0u,r.reason,r.deltaMs,
        r.now[0],r.now[1],r.now[2],r.previous[0],r.previous[1],r.previous[2],
        r.cameraDelta[0],r.cameraDelta[1],r.cameraDelta[2],r.worldEdgeValid?1u:0u,r.medianDelta[0],r.medianDelta[1],r.medianDelta[2],
        r.cameraMinusWorldValid?1u:0u,r.cameraMinusMedian[0],r.cameraMinusMedian[1],r.cameraMinusMedian[2],r.preStable,r.matchedAt,r.coherent,r.owners,r.farRecords,r.farOwners,
        r.movingOutliers,r.missingAt,r.changedQuat,r.sameFrameChanges);
    Log::get().note("flat origin witness edges: resolver-frame=%llu offsets=-3>-2,-2>-1,-1>0,0>+1,+1>+2 present=%u%u%u%u%u%u count=%u,%u,%u,%u,%u coherent=%u,%u,%u,%u,%u median=(%.9g,%.9g,%.9g)/(%.9g,%.9g,%.9g)/(%.9g,%.9g,%.9g)/(%.9g,%.9g,%.9g)/(%.9g,%.9g,%.9g) observed=%u,%u,%u,%u,%u,%u occupied=%u,%u,%u,%u,%u,%u collisions=%u,%u,%u,%u,%u,%u skipped-lock=%u; EDVR previous pose is synthesized, not an origin witness",
        static_cast<unsigned long long>(r.resolverFrame),r.hadFrame[0]?1u:0u,r.hadFrame[1]?1u:0u,r.hadFrame[2]?1u:0u,
        r.hadFrame[3]?1u:0u,r.hadFrame[4]?1u:0u,r.hadFrame[5]?1u:0u,
        r.edgeCount[0],r.edgeCount[1],r.edgeCount[2],r.edgeCount[3],r.edgeCount[4],
        r.edgeCoherent[0],r.edgeCoherent[1],r.edgeCoherent[2],r.edgeCoherent[3],r.edgeCoherent[4],
        r.edgeMedian[0][0],r.edgeMedian[0][1],r.edgeMedian[0][2],r.edgeMedian[1][0],r.edgeMedian[1][1],r.edgeMedian[1][2],
        r.edgeMedian[2][0],r.edgeMedian[2][1],r.edgeMedian[2][2],r.edgeMedian[3][0],r.edgeMedian[3][1],r.edgeMedian[3][2],
        r.edgeMedian[4][0],r.edgeMedian[4][1],r.edgeMedian[4][2],
        r.observed[0],r.observed[1],r.observed[2],r.observed[3],r.observed[4],r.observed[5],
        r.occupied[0],r.occupied[1],r.occupied[2],r.occupied[3],r.occupied[4],r.occupied[5],
        r.collisions[0],r.collisions[1],r.collisions[2],r.collisions[3],r.collisions[4],r.collisions[5],r.skippedLock);
    Log::get().note("flat origin witness far edges: resolver-frame=%llu distance>=250m counts=%u,%u,%u,%u,%u coherent=%u,%u,%u,%u,%u median=(%.9g,%.9g,%.9g)/(%.9g,%.9g,%.9g)/(%.9g,%.9g,%.9g)/(%.9g,%.9g,%.9g)/(%.9g,%.9g,%.9g); distance and owner identity do not prove static world geometry",
        static_cast<unsigned long long>(r.resolverFrame),r.farEdgeCount[0],r.farEdgeCount[1],r.farEdgeCount[2],r.farEdgeCount[3],r.farEdgeCount[4],
        r.farEdgeCoherent[0],r.farEdgeCoherent[1],r.farEdgeCoherent[2],r.farEdgeCoherent[3],r.farEdgeCoherent[4],
        r.farEdgeMedian[0][0],r.farEdgeMedian[0][1],r.farEdgeMedian[0][2],r.farEdgeMedian[1][0],r.farEdgeMedian[1][1],r.farEdgeMedian[1][2],
        r.farEdgeMedian[2][0],r.farEdgeMedian[2][1],r.farEdgeMedian[2][2],r.farEdgeMedian[3][0],r.farEdgeMedian[3][1],r.farEdgeMedian[3][2],
        r.farEdgeMedian[4][0],r.farEdgeMedian[4][1],r.farEdgeMedian[4][2]);
    for(unsigned i=0;i<r.rowCount;++i){const auto& w=r.rows[i];
        Log::get().note("flat origin witness record: resolver-frame=%llu record=%p node=%p owner=%p pre-stable=%u distance-m=%.9g present=%02X proven=%02X quat-or-same-tick-changed=%02X primary-path=%02X scale=unit-if-primary-proven/secondary-unread pos(-3,-2,-1,0,+1,+2)=(%.9g,%.9g,%.9g)/(%.9g,%.9g,%.9g)/(%.9g,%.9g,%.9g)/(%.9g,%.9g,%.9g)/(%.9g,%.9g,%.9g)/(%.9g,%.9g,%.9g)",
            static_cast<unsigned long long>(r.resolverFrame),reinterpret_cast<void*>(w.record),reinterpret_cast<void*>(w.node),
            reinterpret_cast<void*>(w.owner),w.preStable?1u:0u,w.distanceM,w.present,w.proven,w.quatChanged,w.primary,
            w.pos[0][0],w.pos[0][1],w.pos[0][2],w.pos[1][0],w.pos[1][1],w.pos[1][2],
            w.pos[2][0],w.pos[2][1],w.pos[2][2],w.pos[3][0],w.pos[3][1],w.pos[3][2],
            w.pos[4][0],w.pos[4][1],w.pos[4][2],w.pos[5][0],w.pos[5][1],w.pos[5][2]);}
}
inline void finishPending(bool force,uint32_t frame) noexcept {
    FlatOriginWitnessReport report;
    core.setSkippedLock(skipped.load(std::memory_order_relaxed));
    while(core.complete(frame,report,force))print(report);
}
} // namespace flat_origin_witness_runtime

inline void flatOriginWitnessObserve(uintptr_t record,uintptr_t node,uintptr_t owner,const uint32_t pose[5],
                                     uint32_t engineFrame,bool proven,bool primary) noexcept {
    if (!flatOriginCaptureEnabled()) return;
    using namespace flat_origin_witness_runtime;
    if(!active.load(std::memory_order_relaxed))return;
    callbacks.fetch_add(1,std::memory_order_relaxed);
    uint64_t h=static_cast<uint64_t>(record)^(static_cast<uint64_t>(node)*0x9E3779B97F4A7C15ull);
    h^=h>>33;h*=0xff51afd7ed558ccdull;h^=h>>33;h*=0xc4ceb9fe1a85ec53ull;h^=h>>33;
    if(((h>>32)&3u)!=0)return; // one in four identities, all 256 slot indices remain available
    selected.fetch_add(1,std::memory_order_relaxed);
    struct Last {uintptr_t record=0,node=0;uint32_t frame=~0u,pose[5]{};bool proven=false;};
    thread_local Last last;
    if(last.record==record && last.node==node && last.frame==engineFrame && last.proven==proven &&
       std::memcmp(last.pose,pose,5*4)==0){duplicate.fetch_add(1,std::memory_order_relaxed);return;}
    last.record=record;last.node=node;last.frame=engineFrame;last.proven=proven;std::memcpy(last.pose,pose,5*4);
    std::unique_lock<std::mutex> lock(mutex,std::try_to_lock);
    if(!lock.owns_lock()){skipped.fetch_add(1,std::memory_order_relaxed);return;}
    core.observe(record,node,owner,pose,engineFrame,proven,primary);
}
inline void flatOriginWitnessFrameBoundary(uint32_t engineFrame,bool flatProfile,bool emitHookLive) noexcept {
    if (!flatOriginCaptureEnabled()) return;
    using namespace flat_origin_witness_runtime;
    clock.store(engineFrame,std::memory_order_release);
    std::lock_guard<std::mutex> lock(mutex);
    const bool was=active.load(std::memory_order_relaxed);
    if(!flatProfile){if(was){finishPending(true,engineFrame);Log::get().note("flat origin witness: stopped frame=%u events=%u pending=0 dropped(short-toward,long-toward,ordinary,unavailable-short,unavailable-other)=%u,%u,%u,%u,%u callbacks=%u selected=%u duplicate=%u skipped-lock=%u",
        engineFrame,core.events(),core.droppedNear(),core.droppedLongToward(),core.droppedOrdinary(),core.droppedUnavailableShort(),core.droppedUnavailableOther(),callbacks.load(),selected.load(),duplicate.load(),skipped.load());core.clear();}
        active.store(false,std::memory_order_release);heartbeat=0;hookLive=false;return;}
    if(!was){core.clear();skipped.store(0);callbacks.store(0);selected.store(0);duplicate.store(0);heartbeat=0;Log::get().note("flat origin witness: armed engine-frame=%u emit-hook=%u sample-hash=1/4 ring=8x256 pending=16 budgets(short-toward<=512m,long-toward,ordinary,unavailable-short,unavailable-other)=16,8,8,8,4; CPU current game poses only",engineFrame,emitHookLive?1u:0u);}
    active.store(true,std::memory_order_release);hookLive=emitHookLive;
    finishPending(false,engineFrame);
    if(++heartbeat>=300){heartbeat=0;Log::get().note("flat origin witness: heartbeat engine-frame=%u emit-hook=%u events=%u pending=%u dropped(short-toward,long-toward,ordinary,unavailable-short,unavailable-other)=%u,%u,%u,%u,%u callbacks=%u selected-1-in-4=%u duplicate=%u skipped-lock=%u",
        engineFrame,hookLive?1u:0u,core.events(),core.pending(),core.droppedNear(),core.droppedLongToward(),core.droppedOrdinary(),core.droppedUnavailableShort(),core.droppedUnavailableOther(),callbacks.load(),selected.load(),duplicate.load(),skipped.load());}
}
inline void flatOriginWitnessEvent(uint64_t resolverFrame,const float now[3],const float previous[3],float deltaMs,
                                   bool jumpKept,bool classifiedReanchor,bool reset) noexcept {
    if (!flatOriginCaptureEnabled()) return;
    using namespace flat_origin_witness_runtime;
    std::lock_guard<std::mutex> lock(mutex);
    const uint32_t ef=clock.load(std::memory_order_acquire);
    if(!active.load(std::memory_order_relaxed)){Log::get().note("flat origin witness: candidate inactive resolver-frame=%llu engine-frame=%u",static_cast<unsigned long long>(resolverFrame),ef);return;}
    core.event(resolverFrame,ef,now,previous,deltaMs,jumpKept,classifiedReanchor,reset);
}
inline void flatOriginWitnessUnavailable(uint64_t resolverFrame,const float now[3],const float previous[3],
                                         float deltaMs,const char* reason) noexcept {
    if (!flatOriginCaptureEnabled()) return;
    using namespace flat_origin_witness_runtime;
    std::lock_guard<std::mutex> lock(mutex);
    const uint32_t ef=clock.load(std::memory_order_acquire);
    if(!active.load(std::memory_order_relaxed)){Log::get().note("flat origin witness: candidate unavailable and inactive resolver-frame=%llu engine-frame=%u reason=%s",static_cast<unsigned long long>(resolverFrame),ef,reason?reason:"unknown");return;}
    core.event(resolverFrame,ef,now,previous,deltaMs,false,false,false,reason?reason:"unknown");
}
inline void flatOriginWitnessShutdown() noexcept {
    if (!flatOriginCaptureEnabled()) return;
    using namespace flat_origin_witness_runtime;
    std::lock_guard<std::mutex> lock(mutex);
    if(!active.load(std::memory_order_relaxed))return;
    const uint32_t ef=clock.load(std::memory_order_acquire);
    const uint32_t lost=core.pending();finishPending(true,ef);
    Log::get().note("flat origin witness: shutdown engine-frame=%u events=%u incomplete-at-shutdown=%u dropped(short-toward,long-toward,ordinary,unavailable-short,unavailable-other)=%u,%u,%u,%u,%u callbacks=%u selected=%u duplicate=%u skipped-lock=%u",
        ef,core.events(),lost,core.droppedNear(),core.droppedLongToward(),core.droppedOrdinary(),core.droppedUnavailableShort(),core.droppedUnavailableOther(),callbacks.load(),selected.load(),duplicate.load(),skipped.load());
    core.clear();active.store(false,std::memory_order_release);
}

} // namespace edvr
