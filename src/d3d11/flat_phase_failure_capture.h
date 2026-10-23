#pragma once
#include "flat_forward_light.h"
namespace edvr { namespace phase_failure {
using forward_light::Reason;
constexpr unsigned kSlots=8,kBytes=752;
enum class Stage:uint32_t {Accepted,PriorGate,Condition,UnresolvedPhase,PhaseResidual};
inline const char* stageName(Stage s){const char* names[]={"accepted","prior-gate","condition","unresolved-phase","phase-residual"};const auto i=static_cast<unsigned>(s);return i<5?names[i]:"invalid";}
struct Analysis {
    Reason reason=Reason::Never;Stage stage=Stage::PriorGate;int axis=-1,column=-1;
    double fitted[2]{},scale[2]{},dephasedScale[2]{},lhs=0,rhs=0,residual=0,bound=0;
    bool resolved[2]{};
};
// Diagnostic evidence only. The axis decision and error envelope are shared
// with production; used R/F still fit actual phase independently of expected.
inline Analysis analyze(const unsigned char* camera,const unsigned char* bytes,const FlatProjectionJitter& phase) {
    Analysis out;out.reason=forward_light::match(camera,bytes,phase);
    if(!camera||!bytes)return out;
    float cb[47][4];std::memcpy(cb,bytes,sizeof(cb));
    double zz=0;for(unsigned c=0;c<3;++c)zz+=double(cb[4][c])*cb[4][c];
    const double shift[]={phase.ndcX,phase.ndcY};
    for(unsigned r=0;r<2;++r){for(unsigned c=0;c<3;++c){out.fitted[r]+=double(cb[10+r][c])*cb[4][c];out.scale[r]+=double(cb[10+r][c])*cb[2+r][c];out.dephasedScale[r]+=(double(cb[10+r][c])-shift[r]*cb[13][c])*cb[2+r][c];}
        out.fitted[r]=zz>0?out.fitted[r]/zz:std::numeric_limits<double>::quiet_NaN();}
    if(out.reason!=Reason::None&&out.reason!=Reason::Phase)return out;
    for(unsigned r=0;r<2;++r){const auto axis=forward_light::phaseAxis(cb,r,shift[r]);out.axis=static_cast<int>(r);out.resolved[r]=axis.resolved;
        if(axis.reason==Reason::None)continue;
        out.column=axis.column;out.lhs=axis.lhs;out.rhs=axis.rhs;out.residual=axis.residual;out.bound=axis.bound;
        out.stage=axis.stage==forward_light::PhaseAxisStage::Condition?Stage::Condition:
            axis.stage==forward_light::PhaseAxisStage::Unresolved?Stage::UnresolvedPhase:Stage::PhaseResidual;return out;}
    out.stage=out.reason==Reason::None?Stage::Accepted:Stage::PriorGate;out.axis=out.column=-1;return out;
}
struct Requested {uint64_t epoch=0,frame=0;uint32_t width=0,height=0;float pixel[2]{};};
struct Receipt {
    uint64_t epoch=0,frame=0,context=0,device=0,hdr=0,dsv=0,vs=0,ps=0,selectedHdr=0,selectedRaw=0,selectedFrame=0;
    uint32_t q=0,width=0,height=0,phaseWidth=0,phaseHeight=0,owner=0,applied=0,domainVersions=0;
    float pixel[2]{},previousPixel[2]{},viewport[6]{};FlatProjectionJitter ndc{};
    forward_light::Surface raw{},linear{};forward_light::Publication publication{};
    forward_light::Source source{},selectedSource{};forward_light::ConstantIdentity donor{},original[2]{};
    unsigned char camera[96]{},cb[2][kBytes]{};
    Analysis diagnostic{};Reason originalReason=Reason::Never;
    Requested recent[8]{};uint32_t recentCount=0;
    uint32_t donorFlags=0;bool donorStampMatches=false;
    bool priorAccepted=false,lastSelectedDomain=false,issued=false,returned=false,selected=false;
};
inline bool cohort(const Receipt& a,const Receipt& b) {
    return a.epoch==b.epoch&&a.context&&a.context==b.context&&a.device&&a.device==b.device&&a.hdr&&a.hdr==b.hdr&&
        forward_light::plane(a.raw,b.raw)&&forward_light::plane(a.linear,b.linear)&&a.width==b.width&&a.height==b.height&&
        forward_light::sourceIdentity(a.source,b.source);
}
inline bool selectedCohort(const Receipt& a,const Receipt& b) {
    return a.epoch==b.epoch&&a.context&&a.context==b.context&&a.device&&a.device==b.device&&a.hdr&&a.hdr==b.hdr&&
        forward_light::plane(a.raw,b.raw)&&a.width==b.width&&a.height==b.height&&forward_light::sourceIdentity(a.source,b.source);
}
inline uint32_t hash(const unsigned char* p,unsigned n,uint32_t h=2166136261u){for(unsigned i=0;i<n;++i)h=(h^p[i])*16777619u;return h;}
inline uint32_t payloadHash(const Receipt& r,uint32_t h=2166136261u){h=hash(r.camera,96,h);h=hash(r.cb[0],kBytes,h);return hash(r.cb[1],kBytes,h);}
struct Collector {
    Receipt bootstrap[kSlots]{},committed[kSlots]{},failure{},paired{},lastSelection{};bool selectionKnown=false;bool used[kSlots]{},valid[kSlots]{};
    Requested history[8]{};uint32_t historyCount=0,historyNext=0;
    uint64_t epoch=1,entries=0,calls=0,primaryFailures=0,notArmed=0,noSelectedDomain=0,zeroSamples=0,cancelled=0,resets=0;
    uint32_t overflow=0;bool frozen=false,emitted=false,matched=false;
    void note(bool armed){++entries;if(!armed)++notArmed;}
    void phase(uint64_t frame,float x,float y,uint32_t width,uint32_t height){if(frozen)return;
        if(historyCount&&history[(historyNext+7)%8].frame==frame)return;
        history[historyNext]={epoch,frame,width,height,{x,y}};historyNext=(historyNext+1)%8;if(historyCount<8)++historyCount;
    }
    // calls counts considered receipts (the legacy eligibleSnapshots log field),
    // including successful nonzero phases discarded before payload work.
    // The runtime's existing frozen guard runs before this entry.
    bool discardSuccessfulNonzero(Reason reason,float pixelX,float pixelY) {
        if(reason!=Reason::None||(pixelX==0&&pixelY==0))return false;
        ++calls;return true;
    }
    int offer(Receipt r) {
        ++calls;if(frozen)return -1;r.epoch=epoch;r.recentCount=historyCount;
        for(unsigned i=0;i<historyCount;++i)r.recent[i]=history[(historyNext+8-historyCount+i)%8];
        if(r.originalReason==Reason::None&&r.pixel[0]==0&&r.pixel[1]==0){++zeroSamples;
            for(unsigned i=0;i<kSlots;++i)if(used[i]&&cohort(bootstrap[i],r)){bootstrap[i]=r;return static_cast<int>(i);}
            for(unsigned i=0;i<kSlots;++i)if(!used[i]){used[i]=true;bootstrap[i]=r;return static_cast<int>(i);}
            ++overflow;unsigned oldest=0;for(unsigned i=1;i<kSlots;++i)if(bootstrap[i].frame<bootstrap[oldest].frame)oldest=i;
            bootstrap[oldest]=r;valid[oldest]=false;return static_cast<int>(oldest);}
        if(r.originalReason!=Reason::Phase)return -1;
        ++primaryFailures;if(!r.lastSelectedDomain){++noSelectedDomain;return -1;}
        failure=r;frozen=true;
        for(unsigned i=0;i<kSlots;++i)if(valid[i]&&committed[i].returned&&committed[i].issued&&committed[i].selected&&
            committed[i].frame<r.frame&&cohort(committed[i],r)){paired=committed[i];matched=true;break;}
        return -1;
    }
    void issue(uint64_t frame,uint32_t q){if(frozen)return;for(unsigned i=0;i<kSlots;++i)if(used[i]&&bootstrap[i].frame==frame&&bootstrap[i].q==q)bootstrap[i].issued=true;}
    void returned(uint64_t frame,uint32_t q){if(frozen)return;for(unsigned i=0;i<kSlots;++i)if(used[i]&&bootstrap[i].frame==frame&&bootstrap[i].q==q&&bootstrap[i].issued)bootstrap[i].returned=true;}
    void selected(uint64_t frame,uint64_t hdr,uint64_t raw,uint64_t source,const unsigned char* camera){if(frozen)return;for(unsigned i=0;i<kSlots;++i)if(used[i]){auto& r=bootstrap[i];if(r.frame==frame&&r.hdr==hdr&&r.raw.resource==raw&&r.source.buffer==source&&r.returned&&r.issued&&!std::memcmp(r.camera,camera,96)){r.selected=true;committed[i]=r;valid[i]=true;}}}
    void abandon(uint64_t frame,uint32_t q){if(frozen)return;for(unsigned i=0;i<kSlots;++i)if(used[i]&&bootstrap[i].frame==frame&&bootstrap[i].q==q){bootstrap[i]=valid[i]?committed[i]:Receipt{};used[i]=valid[i];++cancelled;}}
    void lifecycle(){++epoch;++resets;if(frozen)return;selectionKnown=false;historyCount=historyNext=0;for(unsigned i=0;i<kSlots;++i){cancelled+=used[i]?1u:0u;used[i]=valid[i]=false;}}
    const char* state()const {if(emitted)return matched?"emitted-pair":"emitted-missing-bootstrap";if(frozen)return matched?"captured-pair":"captured-missing-bootstrap";if(!entries&&!calls)return "never-called";for(unsigned i=0;i<kSlots;++i)if(valid[i])return "awaiting-first-phase-refusal";return "awaiting-issued-selected-bootstrap";}
};
} }
