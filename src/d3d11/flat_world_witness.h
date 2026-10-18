#pragma once
// Passive, bounded observations of game-owned world-object admission records.
// Names, owner pointers and distance are provenance clues, never proof of static geometry.
#include "flat_origin_capture.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include "../common/log.h"

namespace edvr {

enum FlatWorldWitnessValid : uint8_t {
    kPoseCenter=1u<<0, kStoreCenter=1u<<1, kRawCenter=1u<<2,
    kRadius=1u<<3, kViewMask=1u<<4, kAdmitted=1u<<5,
    kNode=1u<<6, kName=1u<<7
};

struct FlatWorldWitnessInput {
    uintptr_t record=0,node=0,owner=0;
    uintptr_t bucket=0;
    uint64_t nodeValidator=0,viewMask=0;
    uint32_t frame=0;
    float center[3]{},poseCenter[3]{},storeCenter[3]{},rawCenter[3]{},radius=0;
    float sourceRadius[3]{}; // already-read pose/store/raw radii; sourceValidMask governs usability
    bool ownerReadValid=false;
    uint8_t source=4,admitted=0,nameClass=0,validMask=0;
    uint8_t sourceValidMask=0; // pose/store/raw sphere validity, independent of primary kRadius
    char name[49]{};
};

struct FlatWorldWitnessReport {
    uint64_t resolverFrame=0;
    uint32_t engineAtEvent=0,sampleFrame=0;
    bool clockMatch=false,complete=false,unavailable=false;
    bool plannedKept=false,reanchorRule=false,plannedReset=false;
    float now[3]{},previous[3]{},cameraDelta[3]{},deltaMs=0;
    char reason[48]{};
    bool hadFrame[6]{};
    uint32_t observed[6]{},occupied[6]{},collision[6]{},noCenter[6]{};
    uint32_t sourceCount[5]{};
    uint32_t fieldPresent[8]{},failedReason[11]{};
    uint16_t matchedEdge[5]{},preStableEdge[5]{},farEdge[5]{},preStableFarEdge[5]{};
    uint16_t preStableFarWeakEdge[5]{};
    float medianEdge[5][3]{},farMedianEdge[5][3]{},preStableFarMedianEdge[5][3]{};
    bool edgeValid[5]{},farEdgeValid[5]{},preStableFarEdgeValid[5]{};
    uint16_t alternativeCount[3]{};
    float alternativeMedian[3][3]{};
    uint16_t preStable=0,farRecords=0,farOwners=0,sourceChanges=0,metadataChanges=0,identityTurnover=0,weakIdentity=0;
    uint32_t callbacks=0,hashSuppressed=0,capSuppressed=0,frameMismatch=0,
             selected=0,seedSelected=0,tailSelected=0,attempted=0,duplicate=0,claimSaturated=0,
             readFailures=0,lockSkipped=0,observeStale=0;
    bool hookReady=false,gateOpen=false,active=false;
    struct Row {
        uintptr_t record=0,node=0,owner=0;
        uintptr_t bucket[6]{};
        uint64_t viewMask[6]{};
        float radius[6]{},alternativeRadius[2][3]{};
        bool ownerReadValid[6]{};
        uint8_t sourceValidMask[6]{};
        uint64_t nodeValidator=0;
        float pos[6][3]{},distanceM=0;
        uint8_t present=0,source[6]{},validMask[6]{},admitted[6]{},nameClass[6]{};
        bool alternativeValid[3]{};
        float alternativeBefore[3][3]{},alternativeAt[3][3]{};
        bool preStable=false,farFromCamera=false,sourceChanged=false,metadataChanged=false;
        char name[49]{};
    } rows[8]{};
    uint8_t rowCount=0;
};

// Pure collector. Caller serializes its methods; it never reads game memory.
class FlatWorldWitnessCore {
public:
    static constexpr uint32_t kFrames=8,kSlots=256,kPending=16;
    void clear() noexcept { *this=FlatWorldWitnessCore{}; }
    void observe(const FlatWorldWitnessInput& in) noexcept;
    void readFailure(uint32_t frame,uint8_t reason) noexcept;
    bool event(uint64_t resolverFrame,uint32_t engineAtEvent,const float now[3],const float previous[3],
               float deltaMs,bool kept,bool reanchor,bool reset,const char* unavailableReason=nullptr) noexcept;
    bool complete(uint32_t frame,FlatWorldWitnessReport& out,bool force=false) noexcept;
    uint32_t pending() const noexcept {return pendingCount_;}
    uint32_t events() const noexcept {return events_;}
    uint32_t dropped(uint32_t i) const noexcept {return i<6?dropped_[i]:0;}
private:
    struct Frame {
        uint32_t stamp=~0u,observed=0,occupied=0,collision=0,noCenter=0;
        uint32_t sourceCount[5]{},fieldPresent[8]{},failedReason[11]{};
        FlatWorldWitnessInput slots[kSlots]{};
    } frames_[kFrames]{};
    struct Event {
        uint64_t resolverFrame=0;
        uint32_t engineAtEvent=0;
        float now[3]{},previous[3]{},deltaMs=0;
        bool kept=false,reanchor=false,reset=false,unavailable=false;
        char reason[48]{};
    } pending_[kPending]{};
    uint32_t pendingCount_=0,events_=0,used_[6]{},dropped_[6]{};
    Frame* frame(uint32_t stamp) noexcept;
    const Frame* find(uint32_t stamp) const noexcept;
    static const FlatWorldWitnessInput* findInput(const Frame*,uintptr_t record) noexcept;
    void assess(const Event&,FlatWorldWitnessReport&) const noexcept;
};

bool flatWorldWitnessSelect(uintptr_t record,uint32_t frame) noexcept;
void flatWorldWitnessObserve(const FlatWorldWitnessInput& in) noexcept;
void flatWorldWitnessReadFailure(uint32_t frame,uint8_t reason) noexcept;
void flatWorldWitnessFrameBoundary(uint32_t frame,bool active,bool hookReady,bool gateOpen) noexcept;
void flatWorldWitnessEvent(uint64_t resolverFrame,const float now[3],const float previous[3],float deltaMs,
                           bool plannedKept,bool reanchorRule,bool plannedReset) noexcept;
void flatWorldWitnessUnavailable(uint64_t resolverFrame,const float now[3],const float previous[3],
                                 float deltaMs,const char* reason) noexcept;
void flatWorldWitnessShutdown() noexcept;

namespace flat_world_witness_detail {
inline uint64_t hash(uintptr_t x) noexcept {
    uint64_t v=static_cast<uint64_t>(x);
    v^=v>>33;v*=0xff51afd7ed558ccdull;v^=v>>33;v*=0xc4ceb9fe1a85ec53ull;return v^(v>>33);
}
// The tail sampler consumes bits 0..5; retain all collector slots for selected records.
inline unsigned slotIndex(uintptr_t record) noexcept {return unsigned((hash(record)>>6)&255u);}
inline bool finite3(const float v[3]) noexcept {
    return std::isfinite(v[0])&&std::isfinite(v[1])&&std::isfinite(v[2]);
}
inline float distance(const float a[3],const float b[3]) noexcept {
    const double x=double(a[0])-b[0],y=double(a[1])-b[1],z=double(a[2])-b[2];
    return static_cast<float>(std::sqrt(x*x+y*y+z*z));
}
inline bool centerValid(const FlatWorldWitnessInput& s) noexcept {
    const uint8_t bit=s.source==1?kPoseCenter:s.source==2?kStoreCenter:s.source==3?kRawCenter:0;
    return bit && (s.validMask&bit) && (s.sourceValidMask&bit) && (s.validMask&kRadius) && finite3(s.center);
}
inline bool sameIdentity(const FlatWorldWitnessInput& a,const FlatWorldWitnessInput& b) noexcept {
    if(a.record!=b.record||a.node!=b.node||a.owner!=b.owner)return false;
    if((a.validMask&kNode)!=(b.validMask&kNode))return false;
    if((a.validMask&kNode)&&a.nodeValidator!=b.nodeValidator)return false;
    return true;
}
inline bool sameMetadata(const FlatWorldWitnessInput& a,const FlatWorldWitnessInput& b) noexcept {
    return a.source==b.source && a.nameClass==b.nameClass && a.admitted==b.admitted &&
        a.viewMask==b.viewMask && a.validMask==b.validMask && a.sourceValidMask==b.sourceValidMask &&
        std::strncmp(a.name,b.name,sizeof(a.name))==0;
}
inline void safeCopy(char* dst,size_t cap,const char* src) noexcept {
    if(!cap)return;size_t i=0;
    if(src)for(;i+1<cap&&src[i];++i){unsigned char c=static_cast<unsigned char>(src[i]);dst[i]=c>=32&&c<127?char(c):'_';}
    dst[i]=0;
}
inline float median(float* values,unsigned n) noexcept {
    if(!n)return 0;std::sort(values,values+n);return values[n/2];
}
} // namespace flat_world_witness_detail

inline FlatWorldWitnessCore::Frame* FlatWorldWitnessCore::frame(uint32_t stamp) noexcept {
    Frame& f=frames_[stamp%kFrames];if(f.stamp!=stamp){f=Frame{};f.stamp=stamp;}return &f;
}
inline const FlatWorldWitnessCore::Frame* FlatWorldWitnessCore::find(uint32_t stamp) const noexcept {
    const Frame& f=frames_[stamp%kFrames];return f.stamp==stamp?&f:nullptr;
}
inline const FlatWorldWitnessInput* FlatWorldWitnessCore::findInput(const Frame* f,uintptr_t record) noexcept {
    if(!f||!record)return nullptr;const auto& s=f->slots[flat_world_witness_detail::slotIndex(record)];
    return s.record==record?&s:nullptr;
}
inline void FlatWorldWitnessCore::observe(const FlatWorldWitnessInput& in) noexcept {
    using namespace flat_world_witness_detail;
    if(!in.record)return;Frame* f=frame(in.frame);++f->observed;
    const unsigned source=in.source<=4?in.source:4;++f->sourceCount[source];
    for(unsigned b=0;b<8;++b)if(in.validMask&(1u<<b))++f->fieldPresent[b];
    if(!centerValid(in))++f->noCenter;
    const unsigned slot=slotIndex(in.record);FlatWorldWitnessInput& old=f->slots[slot];
    if(old.record&&old.record!=in.record){++f->collision;
        // Deterministic rank permits turnover and gives later static records a chance.
        if(hash(in.record)>=hash(old.record))return;
    }
    if(!old.record)++f->occupied;
    old=in;old.name[sizeof(old.name)-1]=0;
}
inline void FlatWorldWitnessCore::readFailure(uint32_t stamp,uint8_t reason) noexcept {
    Frame* f=frame(stamp);++f->failedReason[reason<=10?reason:10];
}
inline bool FlatWorldWitnessCore::event(uint64_t resolverFrame,uint32_t engineAtEvent,
    const float now[3],const float previous[3],float deltaMs,bool kept,bool reanchor,bool reset,
    const char* unavailableReason) noexcept {
    using namespace flat_world_witness_detail;
    if(!now||!previous||!finite3(now)||!finite3(previous))return false;
    const float d=distance(now,previous);
    const bool toward=d>0 && std::sqrt(double(now[0])*now[0]+double(now[1])*now[1]+double(now[2])*now[2])<
                           std::sqrt(double(previous[0])*previous[0]+double(previous[1])*previous[1]+double(previous[2])*previous[2]);
    // 0 short inward, 1 long inward, 2 outward/other, 3 reset, 4 short unavailable, 5 other unavailable.
    const unsigned bucket=unavailableReason?(toward&&d<=512?4u:5u):reset?3u:toward&&d<=512?0u:toward?1u:2u;
    const unsigned quota[6]={16,8,8,8,8,4};
    if(used_[bucket]>=quota[bucket]||pendingCount_>=kPending-(bucket==0?0u:4u)){++dropped_[bucket];return false;}
    ++used_[bucket];++events_;Event& e=pending_[pendingCount_++];e=Event{};
    e.resolverFrame=resolverFrame;e.engineAtEvent=engineAtEvent;
    std::memcpy(e.now,now,sizeof(e.now));std::memcpy(e.previous,previous,sizeof(e.previous));
    e.deltaMs=deltaMs;e.kept=kept;e.reanchor=reanchor;e.reset=reset;e.unavailable=unavailableReason!=nullptr;
    safeCopy(e.reason,sizeof(e.reason),unavailableReason);return true;
}

inline void FlatWorldWitnessCore::assess(const Event& e,FlatWorldWitnessReport& out) const noexcept {
    using namespace flat_world_witness_detail;
    out.resolverFrame=e.resolverFrame;out.engineAtEvent=e.engineAtEvent;
    out.sampleFrame=static_cast<uint32_t>(e.resolverFrame);out.clockMatch=e.resolverFrame==e.engineAtEvent;
    out.deltaMs=e.deltaMs;out.plannedKept=e.kept;out.reanchorRule=e.reanchor;
    out.plannedReset=e.reset;out.unavailable=e.unavailable;safeCopy(out.reason,sizeof(out.reason),e.reason);
    std::memcpy(out.now,e.now,sizeof(out.now));std::memcpy(out.previous,e.previous,sizeof(out.previous));
    for(unsigned j=0;j<3;++j)out.cameraDelta[j]=e.now[j]-e.previous[j];
    const Frame* f[6]{};for(unsigned t=0;t<6;++t){
        const int64_t stamp=static_cast<int64_t>(out.sampleFrame)+int(t)-3;
        if(stamp<0||stamp>UINT32_MAX)continue;
        f[t]=find(static_cast<uint32_t>(stamp));out.hadFrame[t]=f[t]!=nullptr;
        if(!f[t])continue;
        out.observed[t]=f[t]->observed;out.occupied[t]=f[t]->occupied;
        out.collision[t]=f[t]->collision;out.noCenter[t]=f[t]->noCenter;
        for(unsigned i=0;i<5;++i)out.sourceCount[i]+=f[t]->sourceCount[i];
        for(unsigned i=0;i<8;++i)out.fieldPresent[i]+=f[t]->fieldPresent[i];
        for(unsigned i=0;i<11;++i)out.failedReason[i]+=f[t]->failedReason[i];
    }
    // The -1 frame defines cohort identity. Every edge is computed independently;
    // raw moving/attached records remain in the report instead of being relabelled static.
    if(!f[2])return;
    float dx[5][kSlots]{},dy[5][kSlots]{},dz[5][kSlots]{};
    float fdx[5][kSlots]{},fdy[5][kSlots]{},fdz[5][kSlots]{};
    float sfdx[5][kSlots]{},sfdy[5][kSlots]{},sfdz[5][kSlots]{};
    float adx[3][kSlots]{},ady[3][kSlots]{},adz[3][kSlots]{};
    uintptr_t farOwner[8]{};
    for(const FlatWorldWitnessInput& base:f[2]->slots){
        if(!base.record)continue;
        if(!(base.validMask&kNode)||!base.nodeValidator)++out.weakIdentity;
        const FlatWorldWitnessInput* s[6]{};
        for(unsigned t=0;t<6;++t){const auto* q=findInput(f[t],base.record);
            if(q&&sameIdentity(base,*q))s[t]=q;else if(q)++out.identityTurnover;
        }
        const bool baseline=s[0]&&s[1]&&centerValid(*s[0])&&centerValid(*s[1])&&
            sameMetadata(*s[0],*s[1])&&distance(s[0]->center,s[1]->center)<0.05f;
        const bool preStable=baseline&&s[2]&&centerValid(*s[2])&&
            sameMetadata(*s[1],*s[2])&&distance(s[1]->center,s[2]->center)<0.05f;
        if(preStable)++out.preStable;
        const float dist=centerValid(base)?distance(base.center,e.previous):0;
        const bool farFromCamera=centerValid(base)&&dist>=250;
        if(farFromCamera){++out.farRecords;bool ownerSeen=false;for(uintptr_t o:farOwner)if(o==base.owner&&o){ownerSeen=true;break;}
            if(!ownerSeen)for(uintptr_t& o:farOwner)if(!o){o=base.owner;++out.farOwners;break;}}
        bool sourceChanged=false,metadataChanged=false;
        for(unsigned t=1;t<6;++t)if(s[t-1]&&s[t]){
            if(s[t-1]->source!=s[t]->source)sourceChanged=true;
            if(!sameMetadata(*s[t-1],*s[t]))metadataChanged=true;
            if(s[t-1]->source!=s[t]->source||!centerValid(*s[t-1])||!centerValid(*s[t]))continue;
            const unsigned edge=t-1,n=out.matchedEdge[edge];if(n>=kSlots)continue;
            const float* a=s[t-1]->center;const float* b=s[t]->center;
            dx[edge][n]=b[0]-a[0];dy[edge][n]=b[1]-a[1];dz[edge][n]=b[2]-a[2];
            ++out.matchedEdge[edge];if(preStable)++out.preStableEdge[edge];
            if(farFromCamera){const unsigned fn=out.farEdge[edge];fdx[edge][fn]=b[0]-a[0];fdy[edge][fn]=b[1]-a[1];fdz[edge][fn]=b[2]-a[2];++out.farEdge[edge];}
            if(farFromCamera&&preStable){const unsigned sn=out.preStableFarEdge[edge];
                sfdx[edge][sn]=b[0]-a[0];sfdy[edge][sn]=b[1]-a[1];sfdz[edge][sn]=b[2]-a[2];++out.preStableFarEdge[edge];
                if(!(base.validMask&kNode)||!base.nodeValidator)++out.preStableFarWeakEdge[edge];}
        }
        if(sourceChanged)++out.sourceChanges;if(metadataChanged)++out.metadataChanges;
        FlatWorldWitnessReport::Row row{};row.record=base.record;row.node=base.node;row.owner=base.owner;
        row.nodeValidator=base.nodeValidator;row.preStable=preStable;row.farFromCamera=farFromCamera;
        row.sourceChanged=sourceChanged;row.metadataChanged=metadataChanged;row.distanceM=dist;
        safeCopy(row.name,sizeof(row.name),base.name);
        if(s[2]&&s[3]){
            const uint8_t bits[3]={kPoseCenter,kStoreCenter,kRawCenter};
            const float* before[3]={s[2]->poseCenter,s[2]->storeCenter,s[2]->rawCenter};
            const float* after[3]={s[3]->poseCenter,s[3]->storeCenter,s[3]->rawCenter};
            for(unsigned a=0;a<3;++a)if((s[2]->sourceValidMask&s[2]->validMask&bits[a])&&
                (s[3]->sourceValidMask&s[3]->validMask&bits[a])&&finite3(before[a])&&finite3(after[a])){
                row.alternativeValid[a]=true;
                std::memcpy(row.alternativeBefore[a],before[a],sizeof(row.alternativeBefore[a]));
                std::memcpy(row.alternativeAt[a],after[a],sizeof(row.alternativeAt[a]));
                const unsigned n=out.alternativeCount[a];if(n<kSlots){
                    adx[a][n]=after[a][0]-before[a][0];ady[a][n]=after[a][1]-before[a][1];adz[a][n]=after[a][2]-before[a][2];
                    ++out.alternativeCount[a];
                }
            }
        }
        for(unsigned t=0;t<6;++t)if(s[t]){
            row.present|=uint8_t(1u<<t);row.source[t]=s[t]->source;row.validMask[t]=s[t]->validMask;
            row.admitted[t]=s[t]->admitted;row.nameClass[t]=s[t]->nameClass;
            row.bucket[t]=s[t]->bucket;row.viewMask[t]=s[t]->viewMask;row.radius[t]=s[t]->radius;
            row.ownerReadValid[t]=s[t]->ownerReadValid;row.sourceValidMask[t]=s[t]->sourceValidMask;
            if(t==2||t==3)std::memcpy(row.alternativeRadius[t-2],s[t]->sourceRadius,sizeof(s[t]->sourceRadius));
            if(centerValid(*s[t]))std::memcpy(row.pos[t],s[t]->center,sizeof(row.pos[t]));
        }
        // Prefer far, pre-stable records for the bounded printed subset, while
        // also preserving near/attached records. Full aggregates use all slots.
        const int score=(preStable?4:0)+(farFromCamera?2:0)+((row.present&0x0c)==0x0c?1:0);
        unsigned at=out.rowCount;
        for(unsigned i=0;i<out.rowCount;++i){const auto& old=out.rows[i];
            const int oldScore=(old.preStable?4:0)+(old.farFromCamera?2:0)+((old.present&0x0c)==0x0c?1:0);
            if(score>oldScore){at=i;break;}}
        if(at<8){if(out.rowCount<8)++out.rowCount;
            for(unsigned i=out.rowCount-1;i>at;--i)out.rows[i]=out.rows[i-1];out.rows[at]=row;}
    }
    for(unsigned i=0;i<5;++i){const unsigned n=out.matchedEdge[i],fn=out.farEdge[i];
        if(n){out.edgeValid[i]=true;out.medianEdge[i][0]=median(dx[i],n);out.medianEdge[i][1]=median(dy[i],n);out.medianEdge[i][2]=median(dz[i],n);}
        if(fn){out.farEdgeValid[i]=true;out.farMedianEdge[i][0]=median(fdx[i],fn);out.farMedianEdge[i][1]=median(fdy[i],fn);out.farMedianEdge[i][2]=median(fdz[i],fn);}
        const unsigned sn=out.preStableFarEdge[i];if(sn){out.preStableFarEdgeValid[i]=true;
            out.preStableFarMedianEdge[i][0]=median(sfdx[i],sn);out.preStableFarMedianEdge[i][1]=median(sfdy[i],sn);out.preStableFarMedianEdge[i][2]=median(sfdz[i],sn);}
    }
    for(unsigned a=0;a<3;++a){const unsigned n=out.alternativeCount[a];if(n){
        out.alternativeMedian[a][0]=median(adx[a],n);out.alternativeMedian[a][1]=median(ady[a],n);out.alternativeMedian[a][2]=median(adz[a],n);
    }}
}
inline bool FlatWorldWitnessCore::complete(uint32_t frame,FlatWorldWitnessReport& out,bool force) noexcept {
    if(!pendingCount_)return false;
    const Event e=pending_[0];if(!force&&uint64_t(frame)<e.resolverFrame+3)return false;
    for(unsigned j=1;j<pendingCount_;++j)pending_[j-1]=pending_[j];--pendingCount_;
    out=FlatWorldWitnessReport{};assess(e,out);out.complete=!force&&uint64_t(frame)>=e.resolverFrame+3;return true;
}

namespace flat_world_witness_runtime {
inline FlatWorldWitnessCore core;
inline std::mutex mutex;
inline std::atomic<bool> active{false};
inline std::atomic<uint32_t> currentFrame{0},attempts{0},callbacks{0},hashSuppressed{0},capSuppressed{0},frameMismatch{0},selected{0},readFailures{0},lockSkipped{0},observeStale{0};
inline std::atomic<uint32_t> duplicate{0},claimSaturated{0};
inline std::atomic<uint32_t> seedClaims{0},seedSelected{0},tailSelected{0};
inline std::atomic<uint64_t> claims[1024]{};
inline bool ready=false,gate=false;inline uint32_t heartbeat=0;
inline bool lastReady=false,lastGate=false;
inline void counters(FlatWorldWitnessReport& r) noexcept {
    r.callbacks=callbacks.load();r.hashSuppressed=hashSuppressed.load();r.capSuppressed=capSuppressed.load();
    r.frameMismatch=frameMismatch.load();r.selected=selected.load();r.attempted=attempts.load(); // requests include cap refusals, not additional reads
    r.duplicate=duplicate.load();r.claimSaturated=claimSaturated.load();
    r.seedSelected=seedSelected.load();r.tailSelected=tailSelected.load();
    r.readFailures=readFailures.load();r.lockSkipped=lockSkipped.load();r.observeStale=observeStale.load();
    r.active=active.load();r.hookReady=ready;r.gateOpen=gate;
}
inline void report(const FlatWorldWitnessReport& r) noexcept {
    Log::get().note("flat world witness pre-stable far edges: resolver-frame=%llu clocks-match=%u offsets=-3>-2,-2>-1,-1>0,0>+1,+1>+2 count=%u,%u,%u,%u,%u weak-identity=%u,%u,%u,%u,%u valid=%u,%u,%u,%u,%u median=(%.9g,%.9g,%.9g)/(%.9g,%.9g,%.9g)/(%.9g,%.9g,%.9g)/(%.9g,%.9g,%.9g)/(%.9g,%.9g,%.9g) weak-identity-records=%u seed-selected=%u tail-selected=%u; observational pre-stable and distance>=250m intersection, provenance unverified",
        static_cast<unsigned long long>(r.resolverFrame),r.clockMatch?1u:0u,
        r.preStableFarEdge[0],r.preStableFarEdge[1],r.preStableFarEdge[2],r.preStableFarEdge[3],r.preStableFarEdge[4],
        r.preStableFarWeakEdge[0],r.preStableFarWeakEdge[1],r.preStableFarWeakEdge[2],r.preStableFarWeakEdge[3],r.preStableFarWeakEdge[4],
        r.preStableFarEdgeValid[0],r.preStableFarEdgeValid[1],r.preStableFarEdgeValid[2],r.preStableFarEdgeValid[3],r.preStableFarEdgeValid[4],
        r.preStableFarMedianEdge[0][0],r.preStableFarMedianEdge[0][1],r.preStableFarMedianEdge[0][2],
        r.preStableFarMedianEdge[1][0],r.preStableFarMedianEdge[1][1],r.preStableFarMedianEdge[1][2],
        r.preStableFarMedianEdge[2][0],r.preStableFarMedianEdge[2][1],r.preStableFarMedianEdge[2][2],
        r.preStableFarMedianEdge[3][0],r.preStableFarMedianEdge[3][1],r.preStableFarMedianEdge[3][2],
        r.preStableFarMedianEdge[4][0],r.preStableFarMedianEdge[4][1],r.preStableFarMedianEdge[4][2],r.weakIdentity,r.seedSelected,r.tailSelected);
    Log::get().note("flat world witness: resolver-frame=%llu engine-at-event=%u sample-frame=%u clocks-match=%u complete=%u decision-stage=pre-backend planned-kept=%u reanchor-rule=%u planned-reset=%u unavailable=%u reason=%s delta-ms=%.9g camera-now=(%.9g,%.9g,%.9g) camera-previous=(%.9g,%.9g,%.9g) camera-delta=(%.9g,%.9g,%.9g) world-edge-valid=%u world-edge(-1>0)=(%.9g,%.9g,%.9g) far-edge-valid=%u far-edge(-1>0)=(%.9g,%.9g,%.9g); raw world-object observations only",
        static_cast<unsigned long long>(r.resolverFrame),r.engineAtEvent,r.sampleFrame,r.clockMatch?1u:0u,r.complete?1u:0u,r.plannedKept?1u:0u,r.reanchorRule?1u:0u,r.plannedReset?1u:0u,r.unavailable?1u:0u,r.reason,r.deltaMs,
        r.now[0],r.now[1],r.now[2],r.previous[0],r.previous[1],r.previous[2],r.cameraDelta[0],r.cameraDelta[1],r.cameraDelta[2],r.edgeValid[2]?1u:0u,
        r.medianEdge[2][0],r.medianEdge[2][1],r.medianEdge[2][2],r.farEdgeValid[2]?1u:0u,r.farMedianEdge[2][0],r.farMedianEdge[2][1],r.farMedianEdge[2][2]);
    Log::get().note("flat world witness edges: resolver-frame=%llu offsets=-3>-2,-2>-1,-1>0,0>+1,+1>+2 frames=%u%u%u%u%u%u matched=%u,%u,%u,%u,%u pre-stable=%u,%u,%u,%u,%u far250m=%u,%u,%u,%u,%u median=(%.9g,%.9g,%.9g)/(%.9g,%.9g,%.9g)/(%.9g,%.9g,%.9g)/(%.9g,%.9g,%.9g)/(%.9g,%.9g,%.9g) far-median=(%.9g,%.9g,%.9g)/(%.9g,%.9g,%.9g)/(%.9g,%.9g,%.9g)/(%.9g,%.9g,%.9g)/(%.9g,%.9g,%.9g)",
        static_cast<unsigned long long>(r.resolverFrame),r.hadFrame[0],r.hadFrame[1],r.hadFrame[2],r.hadFrame[3],r.hadFrame[4],r.hadFrame[5],
        r.matchedEdge[0],r.matchedEdge[1],r.matchedEdge[2],r.matchedEdge[3],r.matchedEdge[4],r.preStableEdge[0],r.preStableEdge[1],r.preStableEdge[2],r.preStableEdge[3],r.preStableEdge[4],r.farEdge[0],r.farEdge[1],r.farEdge[2],r.farEdge[3],r.farEdge[4],
        r.medianEdge[0][0],r.medianEdge[0][1],r.medianEdge[0][2],r.medianEdge[1][0],r.medianEdge[1][1],r.medianEdge[1][2],r.medianEdge[2][0],r.medianEdge[2][1],r.medianEdge[2][2],r.medianEdge[3][0],r.medianEdge[3][1],r.medianEdge[3][2],r.medianEdge[4][0],r.medianEdge[4][1],r.medianEdge[4][2],
        r.farMedianEdge[0][0],r.farMedianEdge[0][1],r.farMedianEdge[0][2],r.farMedianEdge[1][0],r.farMedianEdge[1][1],r.farMedianEdge[1][2],r.farMedianEdge[2][0],r.farMedianEdge[2][1],r.farMedianEdge[2][2],r.farMedianEdge[3][0],r.farMedianEdge[3][1],r.farMedianEdge[3][2],r.farMedianEdge[4][0],r.farMedianEdge[4][1],r.farMedianEdge[4][2]);
    Log::get().note("flat world witness coverage: resolver-frame=%llu active=%u hook-ready=%u gate-open=%u callbacks=%u selected=%u attempts-current-frame=%u duplicate=%u claim-saturated=%u hash-suppressed=%u cap-suppressed=%u frame-mismatch=%u read-failures=%u lock-skipped=%u observe-stale=%u observed=%u,%u,%u,%u,%u,%u occupied=%u,%u,%u,%u,%u,%u collision=%u,%u,%u,%u,%u,%u no-center=%u,%u,%u,%u,%u,%u sources(0..4)=%u,%u,%u,%u,%u pre-stable=%u far250m=%u far-owners=%u source-changes=%u metadata-changes=%u identity-turnover=%u",
        static_cast<unsigned long long>(r.resolverFrame),r.active,r.hookReady,r.gateOpen,r.callbacks,r.selected,r.attempted,r.duplicate,r.claimSaturated,r.hashSuppressed,r.capSuppressed,r.frameMismatch,r.readFailures,r.lockSkipped,r.observeStale,
        r.observed[0],r.observed[1],r.observed[2],r.observed[3],r.observed[4],r.observed[5],r.occupied[0],r.occupied[1],r.occupied[2],r.occupied[3],r.occupied[4],r.occupied[5],r.collision[0],r.collision[1],r.collision[2],r.collision[3],r.collision[4],r.collision[5],r.noCenter[0],r.noCenter[1],r.noCenter[2],r.noCenter[3],r.noCenter[4],r.noCenter[5],r.sourceCount[0],r.sourceCount[1],r.sourceCount[2],r.sourceCount[3],r.sourceCount[4],r.preStable,r.farRecords,r.farOwners,r.sourceChanges,r.metadataChanges,r.identityTurnover);
    Log::get().note("flat world witness fields: resolver-frame=%llu present(pose,store,raw,radius,view,admitted,node,name)=%u,%u,%u,%u,%u,%u,%u,%u failed(reason1..10)=%u,%u,%u,%u,%u,%u,%u,%u,%u,%u; source/name/owner/distance do not establish static-world provenance",
        static_cast<unsigned long long>(r.resolverFrame),r.fieldPresent[0],r.fieldPresent[1],r.fieldPresent[2],r.fieldPresent[3],r.fieldPresent[4],r.fieldPresent[5],r.fieldPresent[6],r.fieldPresent[7],
        r.failedReason[1],r.failedReason[2],r.failedReason[3],r.failedReason[4],r.failedReason[5],r.failedReason[6],r.failedReason[7],r.failedReason[8],r.failedReason[9],r.failedReason[10]);
    Log::get().note("flat world witness alternative edges: resolver-frame=%llu edge=-1>0 source(pose,store,raw) counts=%u,%u,%u valid=%u,%u,%u median=(%.9g,%.9g,%.9g)/(%.9g,%.9g,%.9g)/(%.9g,%.9g,%.9g); source-specific raw fields, provenance unverified",
        static_cast<unsigned long long>(r.resolverFrame),r.alternativeCount[0],r.alternativeCount[1],r.alternativeCount[2],r.alternativeCount[0]!=0,r.alternativeCount[1]!=0,r.alternativeCount[2]!=0,
        r.alternativeMedian[0][0],r.alternativeMedian[0][1],r.alternativeMedian[0][2],r.alternativeMedian[1][0],r.alternativeMedian[1][1],r.alternativeMedian[1][2],r.alternativeMedian[2][0],r.alternativeMedian[2][1],r.alternativeMedian[2][2]);
    for(unsigned i=0;i<r.rowCount;++i){const auto& x=r.rows[i];
        Log::get().note("flat world witness record metadata: resolver-frame=%llu record=%p offsets=-3,-2,-1,0,+1,+2 bucket=%p,%p,%p,%p,%p,%p owner-read-valid=%u,%u,%u,%u,%u,%u view-mask(hex)=%016llX,%016llX,%016llX,%016llX,%016llX,%016llX selected-radius=%.9g,%.9g,%.9g,%.9g,%.9g,%.9g source-valid(pose=1,store=2,raw=4)=%02X,%02X,%02X,%02X,%02X,%02X radius(-1>0,pose/store/raw)=(%.9g>%.9g)/(%.9g>%.9g)/(%.9g>%.9g); missing fields follow record present/valid masks, source validity governs alternative radii",
            static_cast<unsigned long long>(r.resolverFrame),reinterpret_cast<void*>(x.record),
            reinterpret_cast<void*>(x.bucket[0]),reinterpret_cast<void*>(x.bucket[1]),reinterpret_cast<void*>(x.bucket[2]),
            reinterpret_cast<void*>(x.bucket[3]),reinterpret_cast<void*>(x.bucket[4]),reinterpret_cast<void*>(x.bucket[5]),
            x.ownerReadValid[0],x.ownerReadValid[1],x.ownerReadValid[2],x.ownerReadValid[3],x.ownerReadValid[4],x.ownerReadValid[5],
            static_cast<unsigned long long>(x.viewMask[0]),static_cast<unsigned long long>(x.viewMask[1]),static_cast<unsigned long long>(x.viewMask[2]),
            static_cast<unsigned long long>(x.viewMask[3]),static_cast<unsigned long long>(x.viewMask[4]),static_cast<unsigned long long>(x.viewMask[5]),
            x.radius[0],x.radius[1],x.radius[2],x.radius[3],x.radius[4],x.radius[5],
            x.sourceValidMask[0],x.sourceValidMask[1],x.sourceValidMask[2],x.sourceValidMask[3],x.sourceValidMask[4],x.sourceValidMask[5],
            x.alternativeRadius[0][0],x.alternativeRadius[1][0],x.alternativeRadius[0][1],x.alternativeRadius[1][1],x.alternativeRadius[0][2],x.alternativeRadius[1][2]);
        Log::get().note("flat world witness record: resolver-frame=%llu record=%p node=%p owner=%p validator=%llu name=%s pre-stable=%u far250m=%u distance-m=%.9g present=%02X sources=%u,%u,%u,%u,%u,%u valid=%02X,%02X,%02X,%02X,%02X,%02X admitted=%u,%u,%u,%u,%u,%u name-class=%u,%u,%u,%u,%u,%u source-changed=%u metadata-changed=%u pos(-3,-2,-1,0,+1,+2)=(%.9g,%.9g,%.9g)/(%.9g,%.9g,%.9g)/(%.9g,%.9g,%.9g)/(%.9g,%.9g,%.9g)/(%.9g,%.9g,%.9g)/(%.9g,%.9g,%.9g)",
            static_cast<unsigned long long>(r.resolverFrame),reinterpret_cast<void*>(x.record),reinterpret_cast<void*>(x.node),reinterpret_cast<void*>(x.owner),static_cast<unsigned long long>(x.nodeValidator),x.name,x.preStable,x.farFromCamera,x.distanceM,x.present,
            x.source[0],x.source[1],x.source[2],x.source[3],x.source[4],x.source[5],x.validMask[0],x.validMask[1],x.validMask[2],x.validMask[3],x.validMask[4],x.validMask[5],x.admitted[0],x.admitted[1],x.admitted[2],x.admitted[3],x.admitted[4],x.admitted[5],x.nameClass[0],x.nameClass[1],x.nameClass[2],x.nameClass[3],x.nameClass[4],x.nameClass[5],x.sourceChanged,x.metadataChanged,
            x.pos[0][0],x.pos[0][1],x.pos[0][2],x.pos[1][0],x.pos[1][1],x.pos[1][2],x.pos[2][0],x.pos[2][1],x.pos[2][2],x.pos[3][0],x.pos[3][1],x.pos[3][2],x.pos[4][0],x.pos[4][1],x.pos[4][2],x.pos[5][0],x.pos[5][1],x.pos[5][2]);
        Log::get().note("flat world witness record alternatives: resolver-frame=%llu record=%p edge=-1>0 valid(pose,store,raw)=%u,%u,%u pose=(%.9g,%.9g,%.9g)>(%.9g,%.9g,%.9g) store=(%.9g,%.9g,%.9g)>(%.9g,%.9g,%.9g) raw=(%.9g,%.9g,%.9g)>(%.9g,%.9g,%.9g)",
            static_cast<unsigned long long>(r.resolverFrame),reinterpret_cast<void*>(x.record),x.alternativeValid[0],x.alternativeValid[1],x.alternativeValid[2],
            x.alternativeBefore[0][0],x.alternativeBefore[0][1],x.alternativeBefore[0][2],x.alternativeAt[0][0],x.alternativeAt[0][1],x.alternativeAt[0][2],
            x.alternativeBefore[1][0],x.alternativeBefore[1][1],x.alternativeBefore[1][2],x.alternativeAt[1][0],x.alternativeAt[1][1],x.alternativeAt[1][2],
            x.alternativeBefore[2][0],x.alternativeBefore[2][1],x.alternativeBefore[2][2],x.alternativeAt[2][0],x.alternativeAt[2][1],x.alternativeAt[2][2]);
    }
}
inline void finish(bool force,uint32_t frame) noexcept {
    FlatWorldWitnessReport r;while(core.complete(frame,r,force)){counters(r);report(r);}
}
inline void totals(const char* label,uint32_t frame) noexcept {
    Log::get().note("flat world witness: %s engine-frame=%u active=%u hook-ready-at-frame-start=%u gate-open-at-frame-start=%u events=%u pending=%u dropped(short-inward,long-inward,other,reset,unavailable-short,unavailable-other)=%u,%u,%u,%u,%u,%u callbacks=%u selected=%u attempts-current-frame=%u duplicate=%u claim-saturated=%u hash-suppressed=%u cap-suppressed=%u frame-mismatch=%u read-failures=%u lock-skipped=%u observe-stale=%u seed-selected=%u tail-selected=%u",
        label,frame,active.load()?1u:0u,ready?1u:0u,gate?1u:0u,core.events(),core.pending(),core.dropped(0),core.dropped(1),core.dropped(2),core.dropped(3),core.dropped(4),core.dropped(5),callbacks.load(),selected.load(),attempts.load(),duplicate.load(),claimSaturated.load(),hashSuppressed.load(),capSuppressed.load(),frameMismatch.load(),readFailures.load(),lockSkipped.load(),observeStale.load(),seedSelected.load(),tailSelected.load());
}
} // namespace flat_world_witness_runtime

inline bool flatWorldWitnessSelect(uintptr_t record,uint32_t frame) noexcept {
    if (!flatOriginCaptureEnabled()) return false;
    using namespace flat_world_witness_runtime;
    if(!active.load(std::memory_order_acquire))return false;
    ++callbacks;
    if(frame!=currentFrame.load(std::memory_order_acquire)){++frameMismatch;return false;}
    const uint64_t h=flat_world_witness_detail::hash(record);
    const bool seedCandidate=seedClaims.load(std::memory_order_relaxed)<32;
    if(!record||(!seedCandidate&&(h&63u)!=0)){++hashSuppressed;return false;}
    const uint64_t tag=(uint64_t(frame)<<32)|(uint32_t(h>>16)|1u);
    // Disjoint from sample bits 0..5 and collector index bits 6..13.
    const unsigned base=unsigned((h>>14)&1023u);
    bool claimed=false;
    for(unsigned p=0;p<4;++p){auto& slot=claims[(base+p)&1023u];uint64_t prior=slot.load(std::memory_order_relaxed);
        if(prior==tag){++duplicate;return false;}
        if(uint32_t(prior>>32)==frame)continue;
        if(slot.compare_exchange_strong(prior,tag,std::memory_order_relaxed)){claimed=true;break;}
        if(prior==tag){++duplicate;return false;}
    }
    if(!claimed){++claimSaturated;return false;}
    // Dedup precedes the seed reservation: a hot repeated record cannot
    // consume the unfiltered prefix. Races beyond 32 use the normal tail gate.
    const bool seeded=seedCandidate&&seedClaims.fetch_add(1,std::memory_order_relaxed)<32;
    if(!seeded&&(h&63u)!=0){++hashSuppressed;return false;}
    const uint32_t n=attempts.fetch_add(1,std::memory_order_relaxed);
    if(n>=128){++capSuppressed;return false;}
    if(!active.load(std::memory_order_acquire)||frame!=currentFrame.load(std::memory_order_acquire)){++frameMismatch;return false;}
    ++selected;if(seeded)++seedSelected;else ++tailSelected;return true;
}
inline void flatWorldWitnessObserve(const FlatWorldWitnessInput& in) noexcept {
    if (!flatOriginCaptureEnabled()) return;
    using namespace flat_world_witness_runtime;
    if(!active.load(std::memory_order_acquire)||in.frame!=currentFrame.load(std::memory_order_acquire)){++observeStale;return;}
    if(!mutex.try_lock()){++lockSkipped;return;}
    if(!active.load(std::memory_order_acquire)||in.frame!=currentFrame.load(std::memory_order_acquire)){++observeStale;mutex.unlock();return;}
    core.observe(in);mutex.unlock();
}
inline void flatWorldWitnessReadFailure(uint32_t frame,uint8_t reason) noexcept {
    if (!flatOriginCaptureEnabled()) return;
    using namespace flat_world_witness_runtime;
    ++readFailures;if(!active.load(std::memory_order_acquire)||frame!=currentFrame.load(std::memory_order_acquire)){++observeStale;return;}
    if(!mutex.try_lock()){++lockSkipped;return;}
    if(!active.load(std::memory_order_acquire)||frame!=currentFrame.load(std::memory_order_acquire)){++observeStale;mutex.unlock();return;}
    core.readFailure(frame,reason);mutex.unlock();
}
inline void flatWorldWitnessFrameBoundary(uint32_t frame,bool enabled,bool hookReady,bool gateOpen) noexcept {
    if (!flatOriginCaptureEnabled()) return;
    using namespace flat_world_witness_runtime;
    std::lock_guard<std::mutex> lock(mutex);
    const bool was=active.load(std::memory_order_relaxed);
    if(!enabled){active.store(false,std::memory_order_release);if(was){finish(true,frame);totals("stopped",frame);}ready=hookReady;gate=gateOpen;return;}
    // Close selection while per-frame budgets change; do not expose the next
    // stamp until its counters are ready. In-flight reads recheck under lock.
    active.store(false,std::memory_order_release);
    if(!was){core.clear();for(auto& claim:claims)claim.store(0,std::memory_order_relaxed);attempts=0;callbacks=0;hashSuppressed=0;capSuppressed=0;frameMismatch=0;selected=0;duplicate=0;claimSaturated=0;seedSelected=0;tailSelected=0;readFailures=0;lockSkipped=0;observeStale=0;heartbeat=0;}
    attempts.store(0,std::memory_order_relaxed);seedClaims.store(0,std::memory_order_relaxed);
    currentFrame.store(frame,std::memory_order_release);
    ready=hookReady;gate=gateOpen;active.store(true,std::memory_order_release);
    finish(false,frame);
    if(!was){Log::get().note("flat world witness: armed engine-frame=%u active=1 hook-ready-at-frame-start=%u gate-open-at-frame-start=%u seed=32-unique/frame tail-hash=1/64 read-attempt-cap=128/frame ring=8x256 pending=16 pre-backend-candidates; first gate status can lag one Present",frame,hookReady?1u:0u,gateOpen?1u:0u);lastReady=hookReady;lastGate=gateOpen;}
    else if(lastReady!=hookReady||lastGate!=gateOpen){totals("hook-status-change",frame);lastReady=hookReady;lastGate=gateOpen;}
    if(++heartbeat>=300){heartbeat=0;totals("heartbeat",frame);}
}
inline void flatWorldWitnessEvent(uint64_t resolverFrame,const float now[3],const float previous[3],float deltaMs,
    bool plannedKept,bool reanchorRule,bool plannedReset) noexcept {
    if (!flatOriginCaptureEnabled()) return;
    using namespace flat_world_witness_runtime;
    std::lock_guard<std::mutex> lock(mutex);
    if(!active.load(std::memory_order_relaxed)){Log::get().note("flat world witness: candidate inactive resolver-frame=%llu engine-frame=%u",static_cast<unsigned long long>(resolverFrame),currentFrame.load());return;}
    core.event(resolverFrame,currentFrame.load(),now,previous,deltaMs,plannedKept,reanchorRule,plannedReset);
}
inline void flatWorldWitnessUnavailable(uint64_t resolverFrame,const float now[3],const float previous[3],float deltaMs,const char* reason) noexcept {
    if (!flatOriginCaptureEnabled()) return;
    using namespace flat_world_witness_runtime;
    std::lock_guard<std::mutex> lock(mutex);
    if(!active.load(std::memory_order_relaxed)){Log::get().note("flat world witness: unavailable inactive resolver-frame=%llu engine-frame=%u reason=%s",static_cast<unsigned long long>(resolverFrame),currentFrame.load(),reason?reason:"unknown");return;}
    core.event(resolverFrame,currentFrame.load(),now,previous,deltaMs,false,false,false,reason?reason:"unknown");
}
inline void flatWorldWitnessShutdown() noexcept {
    if (!flatOriginCaptureEnabled()) return;
    using namespace flat_world_witness_runtime;
    active.store(false,std::memory_order_release);
    std::lock_guard<std::mutex> lock(mutex);
    finish(true,currentFrame.load());totals("shutdown",currentFrame.load());
}
} // namespace edvr
