#pragma once
#include "flat_forward_light_tests.h"
#include "../../src/d3d11/flat_phase_failure_capture.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>

namespace flat_phase_failure_test {
using namespace edvr;
using namespace edvr::forward_light;
using namespace edvr::phase_failure;

inline FlatProjectionJitter phase(float x,float y) { return {x,y,x*.5f,-y*.5f}; }

// The shared scene fixture independently constructs a proper rotation R and
// clip rows F=(sx*R.x+actual.x*R.z,sy*R.y+actual.y*R.z,near,R.z).
// These assertions concern that geometry, not the diagnostic's implementation.
inline int algebraTests() {
    unsigned checks=0;int failures=0;
    const auto expect=[&](bool ok,const char* what){++checks;if(!ok){std::printf("FAIL: phase failure algebra %s\n",what);++failures;}};
    const auto phaseValueClose=[](double actual,double wanted,double tolerance=2.e-6){return std::abs(actual-wanted)<=tolerance;};
    const auto inspect=[&](const forward_light_test::Fixture& f,const FlatProjectionJitter& expected) {
        const auto before=f;
        const Reason decision=f.result(expected);
        const Analysis report=analyze(f.bytes(),reinterpret_cast<const unsigned char*>(f.cb),expected);
        expect(report.reason==decision,"diagnostic and unchanged admission agree");
        expect(!std::memcmp(f.camera,before.camera,sizeof(f.camera))&&!std::memcmp(f.cb,before.cb,sizeof(f.cb)),
               "analysis preserves every original camera and 752-byte constant bit");
        return report;
    };
    const FlatProjectionJitter expected=phase(.00125f,-.00175f);
    {
        const auto valid=forward_light_test::scene(expected.ndcX,expected.ndcY);
        const auto r=inspect(valid,expected);
        expect(r.reason==Reason::None&&r.stage==Stage::Accepted,"matching nonzero upstream phase accepts");
        expect(phaseValueClose(r.fitted[0],expected.ndcX)&&phaseValueClose(r.fitted[1],expected.ndcY),"fitted phase recovers both independently injected coordinates");
        expect(phaseValueClose(r.scale[0],1.25)&&phaseValueClose(r.scale[1],2.),"fitted scales recover native projection axes");
        expect(r.resolved[0]&&r.resolved[1],"both expected shifts survive float precision");
        auto unused=valid;unused.cb[6][0]=(std::numeric_limits<float>::quiet_NaN)();
        expect(inspect(unused,expected).reason==Reason::None,"unused inverse-view bytes cannot veto matching used rows");
    }
    {
        const auto unphased=forward_light_test::scene();
        const auto r=inspect(unphased,expected);
        expect(r.reason==Reason::Phase&&r.stage==Stage::PhaseResidual&&r.axis==0,
               "unphased actual forward first fails the x-axis phase residual");
        expect(phaseValueClose(r.fitted[0],0)&&phaseValueClose(r.fitted[1],0),"unphased payload reports actual zero instead of expected jitter");
        expect(r.column>=0&&r.column<4&&std::abs(r.residual)>r.bound&&r.bound>=0&&phaseValueClose(r.lhs-r.rhs,r.residual,1.e-12),
               "failed component names a real over-bound algebraic residual");
        expect(inspect(unphased,{}).reason==Reason::None,"the identical payload is a valid zero-phase bootstrap");
    }
    const float actualPhases[][2]={{-expected.ndcX,-expected.ndcY},{expected.ndcX*2,expected.ndcY*2},
                                 {-.00075f,.0005f},{expected.ndcX,0}};
    for(const auto& actual:actualPhases) {
        const auto f=forward_light_test::scene(actual[0],actual[1]);
        const auto r=inspect(f,expected);
        expect(r.reason==Reason::Phase&&r.stage==Stage::PhaseResidual,"wrong-sign scaled prior-epoch or single-axis phase refuses");
        expect(phaseValueClose(r.fitted[0],actual[0])&&phaseValueClose(r.fitted[1],actual[1]),"fit exposes the actual wrong phase independently on both axes");
        expect(r.axis==(actual[0]==expected.ndcX?1:0)&&std::abs(r.residual)>r.bound,"first failure names the earliest mismatching axis");
    }
    {
        const auto f=forward_light_test::scene(expected.ndcX,expected.ndcY,.5f,3.75f);
        const auto r=inspect(f,expected);
        expect(r.reason==Reason::None&&phaseValueClose(r.scale[0],.5)&&phaseValueClose(r.scale[1],3.75),"projection scale changes preserve phase and are measured separately");
        const auto lost=forward_light_test::scene(expected.ndcX,expected.ndcY,1.e20f,1.e20f,false,false,true);
        const auto unresolved=inspect(lost,expected);
        expect(unresolved.reason==Reason::Phase&&unresolved.stage==Stage::UnresolvedPhase&&unresolved.axis==0&&!unresolved.resolved[0],
               "nonzero shifts erased by a huge float axis are unresolved rather than certified");
    }
    {
        auto f=forward_light_test::scene(expected.ndcX,expected.ndcY);
        f.camera[5][0]+=.5f;
        const auto r=inspect(f,expected);
        expect(r.reason==Reason::Origin&&r.stage==Stage::PriorGate,"origin disagreement remains a prior admission failure");
        f=forward_light_test::scene(expected.ndcX,expected.ndcY);f.camera[3][0]=1;
        expect(inspect(f,expected).reason==Reason::Forward,"unsupported donor projection fails before phase analysis");
        f=forward_light_test::scene(expected.ndcX,expected.ndcY);f.cb[3][0]*=1.2f;
        expect(inspect(f,expected).reason!=Reason::None,"nonorthogonal used view cannot become accepted through diagnostics");
        auto bad=expected;bad.uvX=(std::numeric_limits<float>::quiet_NaN)();
        expect(inspect(forward_light_test::scene(),bad).reason==Reason::OriginalMissing,"nonfinite ancillary phase retains existing refusal");
        expect(analyze(nullptr,reinterpret_cast<const unsigned char*>(f.cb),expected).reason==Reason::OriginalMissing&&
               analyze(f.bytes(),nullptr,expected).reason==Reason::OriginalMissing,"absent payloads produce explicit missing evidence");
    }
    {
        const auto sheared=inspect(forward_light_test::scene(0,0,1.25f,2,true),{});
        expect(sheared.reason==Reason::View&&sheared.stage==Stage::PriorGate,"coherent sheared view reports its first failed admission gate");
        const auto conditioned=inspect(forward_light_test::scene(0,0,0,2),{});
        expect(conditioned.reason==Reason::View&&conditioned.stage==Stage::PriorGate,
               "a View refusal stays a prior gate instead of inventing a later phase failure");
    }
    // Exact scalar bits from the independently captured Steam sample_1,
    // frame12922/q715; this exercises actual float-rounding, not ideal matrices.
    {
        const uint32_t camera[]={0xBEE295BDu,0xBFAE6CCDu,0,0xB6CEA54Bu,0xBF443A67u,0x3F4968A7u,0,0xB77599F5u,0x3769FB9Au,0xB651B873u,0,0xBF800000u,0,0,0x3CCCCCCDu,0,0xB6CEA54Bu,0xB77599F5u,0xBF800000u,0,0xC070D2A0u,0x400ABF6Au,0xC18DD12Bu,0};
        const uint32_t view[]={0xBF000006u,0xBF5DB3D6u,0x37842DE3u,0xBB702DAFu,0xBF5DB3D6u,0x3F000006u,0xB605484Au,0xC08AF7A7u,0xB6CEA54Bu,0xB77599F5u,0xBF800000u,0xC18DD127u};
        const uint32_t forward[]={0xBEE295BDu,0xBF443A67u,0x3769FB9Au,0xBB5494C6u,0xBFAE6CCDu,0x3F4968A7u,0xB651B873u,0xC0DAAA79u,0,0,0,0x3CCCCCCDu,0xB6CEA54Bu,0xB77599F5u,0xBF800000u,0xC18DD127u};
        forward_light_test::Fixture f{};std::memcpy(f.camera,camera,sizeof(camera));std::memcpy(f.cb+2,view,sizeof(view));std::memcpy(f.cb+10,forward,sizeof(forward));
        expect(inspect(f,{}).reason==Reason::None,"captured zero-phase rows retain operation-count certificate");
        const auto r=inspect(f,expected);
        expect(r.reason==Reason::Phase&&phaseValueClose(r.fitted[0],0)&&phaseValueClose(r.fitted[1],0),"captured native rows expose missing nonzero raster phase");
    }
    std::printf("phase failure algebra: %u checks, %d failure(s)\n",checks,failures);
    return failures;
}
} // namespace flat_phase_failure_test
namespace flat_phase_failure_test {
inline Receipt receipt(uint64_t frame=10,uint32_t q=30,bool fail=false) {
    Receipt r{};r.frame=frame;r.q=q;r.context=101;r.device=102;r.hdr=103;r.dsv=104;
    r.vs=forward_light::kVs;r.ps=forward_light::kPs;r.width=r.phaseWidth=3840;r.height=r.phaseHeight=2160;
    r.raw=forward_light_test::surface(105);r.linear=forward_light_test::surface(106);
    r.source=forward_light_test::source();r.source.frame=frame;
    r.publication={r.raw,r.linear,107,frame,q-1,true,true,true};
    r.donor.buffer=r.source.buffer;r.donor.generation=r.source.generation;r.donor.bank=r.source.bank;
    r.donor.first=r.source.first;r.donor.count=r.source.count;r.donor.writeGeneration=r.source.writeGeneration;
    r.donor.writeFrame=frame;r.donor.writeQ=5;r.owner=1;r.applied=fail?1:0;
    r.original[0].buffer=108;r.original[1].buffer=109;
    for(auto& id:r.original){id.generation=2;id.bank=3;id.mutation=4;id.writeGeneration=5;id.writeFrame=frame;
        id.width=752;id.count=47;id.bytes=752;id.writeQ=q-2;id.hash=0x12345678;}
    const auto f=forward_light_test::scene();
    std::memcpy(r.camera,f.camera,sizeof(r.camera));std::memcpy(r.cb[0],f.cb,sizeof(r.cb[0]));std::memcpy(r.cb[1],f.cb,sizeof(r.cb[1]));
    // Distinct opaque bytes in unused rows must survive storage verbatim.
    r.cb[0][0]=0x31;r.cb[1][0]=0xA7;r.cb[0][751]=0xB3;r.cb[1][751]=0x4D;
    if(fail){r.ndc=phase(.00125f,-.00175f);r.pixel[0]=2.4f;r.pixel[1]=1.89f;}
    r.diagnostic=analyze(r.camera,r.cb[0],r.ndc);r.originalReason=match(r.camera,r.cb[0],r.ndc);r.priorAccepted=true;r.lastSelectedDomain=fail;
    r.selectedHdr=r.hdr;r.selectedRaw=r.raw.resource;r.selectedFrame=frame-1;
    return r;
}
inline void complete(Collector& c,const Receipt& r) {
    c.issue(r.frame,r.q);c.returned(r.frame,r.q);
    c.selected(r.frame,r.hdr,r.raw.resource,r.source.buffer,r.camera);
}
inline bool samePayload(const Receipt& a,const Receipt& b) {
    return !std::memcmp(a.camera,b.camera,sizeof(a.camera))&&!std::memcmp(a.cb,b.cb,sizeof(a.cb));
}
inline int collectorTests() {
    unsigned checks=0;int failures=0;
    const auto expect=[&](bool ok,const char* what){++checks;if(!ok){std::printf("FAIL: phase failure collector %s\n",what);++failures;}};
    const auto state=[](const Collector& c,const char* text){return !std::strcmp(c.state(),text);};
    const Receipt zero=receipt(),failure=receipt(11,31,true);
    expect(zero.diagnostic.reason==Reason::None&&failure.diagnostic.reason==Reason::Phase,"real matrix fixtures enter the intended admission paths");
    {
        Collector c;unsigned payloadWork=0,analysisWork=0;
        // Same lazy boundary as the production observer: the fixture's input
        // exists already, but no Receipt copy or analysis happens until admitted.
        const auto observe=[&](const Receipt& input){
            if(c.frozen||c.discardSuccessfulNonzero(input.originalReason,input.pixel[0],input.pixel[1]))return -1;
            ++payloadWork;Receipt copied=input;++analysisWork;
            copied.diagnostic=analyze(copied.camera,copied.cb[0],copied.ndc);
            return c.offer(copied);
        };
        c.note(true);c.phase(zero.frame,0,0,zero.width,zero.height);
        const int slot=observe(zero);complete(c,zero);
        expect(slot>=0&&c.valid[slot]&&payloadWork==1&&analysisWork==1&&c.calls==1,
            "zero-phase receipt still copies analyzes and commits the issued returned selected bootstrap");
        const auto saved=c.committed[slot];const auto oldHistory=c.historyCount;
        const float discardedPhases[][2]={{.25f,0},{0,-.125f},{-.25f,.375f}};
        for(const auto& phaseValue:discardedPhases){
            auto success=zero;success.pixel[0]=phaseValue[0];success.pixel[1]=phaseValue[1];
            expect(flatProjectionJitter(success.pixel[0],success.pixel[1],success.width,success.height,success.ndc),"nonzero fixture has the actual pixel-to-NDC phase");
            const auto phased=forward_light_test::scene(success.ndc.ndcX,success.ndc.ndcY);
            std::memcpy(success.camera,phased.camera,sizeof(success.camera));
            std::memcpy(success.cb[0],phased.cb,sizeof(success.cb[0]));std::memcpy(success.cb[1],phased.cb,sizeof(success.cb[1]));
            success.originalReason=match(success.camera,success.cb[0],success.ndc);
            c.note(true);expect(success.originalReason==Reason::None&&observe(success)<0,"actual successful phase on either pixel axis discards before receipt construction");
        }
        expect(payloadWork==1&&analysisWork==1&&c.calls==4&&c.entries==4&&c.zeroSamples==1&&
            !c.primaryFailures&&!c.noSelectedDomain&&!c.frozen&&c.historyCount==oldHistory&&samePayload(c.committed[slot],saved),
            "early discarded successes keep considered-call accounting without changing bootstrap cohort phase history or capture state");
        c.note(true);observe(failure);
        expect(payloadWork==2&&analysisWork==2&&c.calls==5&&c.primaryFailures==1&&c.frozen&&c.matched&&
            samePayload(c.paired,zero)&&samePayload(c.failure,failure),
            "phase failure after discarded successes still copies full bytes and freezes its selected zero bootstrap pair");
        observe(zero);observe(failure);
        expect(payloadWork==2&&analysisWork==2&&c.calls==5,"existing frozen observer boundary performs no later payload work or call increment");
        const auto captured=c.failure;c.lifecycle();
        expect(c.frozen&&c.matched&&samePayload(c.failure,captured),"lifecycle keeps the same frozen one-pair receipt after early discards");
    }
    {
        Collector c;
        expect(!c.discardSuccessfulNonzero(Reason::None,-0.f,0.f)&&!c.discardSuccessfulNonzero(Reason::None,0.f,-0.f),
            "both signed-zero pixel components remain bootstrap eligible");
        expect(!c.discardSuccessfulNonzero(Reason::Phase,0,0)&&!c.discardSuccessfulNonzero(Reason::Phase,.25f,-.125f)&&
            !c.discardSuccessfulNonzero(Reason::View,.25f,0)&&!c.calls,
            "only successful nonzero receipts are early discarded; every phase refusal and other decision remains unchanged");
    }
    {
        Collector c;
        expect(state(c,"never-called")&&!c.frozen&&!c.emitted,"startup distinguishes a never-called instrument");
        const int slot=c.offer(zero);
        expect(slot>=0&&state(c,"awaiting-issued-selected-bootstrap"),"matching planned zero-phase input only starts a pending bootstrap");
        c.returned(zero.frame,zero.q);c.selected(zero.frame,zero.hdr,zero.raw.resource,zero.source.buffer,zero.camera);
        expect(!c.bootstrap[slot].returned&&!c.bootstrap[slot].selected,"return and selection before CPU issue grant no bootstrap authority");
        c.issue(zero.frame,zero.q+1);c.returned(zero.frame+1,zero.q);
        expect(!c.bootstrap[slot].issued&&!c.bootstrap[slot].returned,"foreign draw ordinal and frame cannot acknowledge the pending draw");
        c.issue(zero.frame,zero.q);c.selected(zero.frame,zero.hdr,zero.raw.resource,zero.source.buffer,zero.camera);
        expect(c.bootstrap[slot].issued&&!c.bootstrap[slot].selected,"issued but unreturned work remains ineligible");
        c.returned(zero.frame,zero.q);
        auto wrong=zero;wrong.camera[3]^=1;
        c.selected(zero.frame,zero.hdr,zero.raw.resource,zero.source.buffer,wrong.camera);
        c.selected(zero.frame,zero.hdr+1,zero.raw.resource,zero.source.buffer,zero.camera);
        c.selected(zero.frame,zero.hdr,zero.raw.resource+1,zero.source.buffer,zero.camera);
        c.selected(zero.frame,zero.hdr,zero.raw.resource,zero.source.buffer+1,zero.camera);
        expect(!c.bootstrap[slot].selected,"selected resolve must name the same H raw-depth source and camera bytes");
        c.selected(zero.frame,zero.hdr,zero.raw.resource,zero.source.buffer,zero.camera);
        expect(state(c,"awaiting-first-phase-refusal"),"successful matching selection commits the returned zero bootstrap");
        auto unrelated=failure;unrelated.lastSelectedDomain=false;c.offer(unrelated);
        expect(!c.frozen&&c.noSelectedDomain==1,"nonprimary phase refusal cannot spend the first capture");
        c.offer(failure);
        expect(c.frozen&&c.matched&&state(c,"captured-pair")&&!c.emitted,"first primary refusal freezes a pair before emission");
        expect(c.paired.frame==zero.frame&&c.failure.frame==failure.frame&&samePayload(c.paired,zero)&&samePayload(c.failure,failure),
               "paired and refused original bytes belong to their exact draws");
        const auto pair=c.paired,failed=c.failure;const auto pairHash=payloadHash(pair),failedHash=payloadHash(failed);
        auto overwrite=failure;overwrite.cb[0][0]^=0xFF;overwrite.camera[0]^=0xFF;
        c.offer(overwrite);c.abandon(zero.frame,zero.q);c.lifecycle();complete(c,zero);
        expect(c.frozen&&c.matched&&!c.emitted&&samePayload(c.paired,pair)&&samePayload(c.failure,failed)&&
               payloadHash(c.paired)==pairHash&&payloadHash(c.failure)==failedHash,
               "later draws cancellation lifecycle and callbacks cannot replace or damage frozen evidence");
        c.emitted=true;
        expect(state(c,"emitted-pair"),"emission becomes visible only after the frozen pair has been available");
        c.lifecycle();c.offer(zero);c.offer(failure);
        expect(c.emitted&&c.frozen&&c.matched&&samePayload(c.failure,failed),"resize or F7 lifecycle cannot rearm an emitted process-lifetime capture");
    }
    // Each missing receipt yields an explicit missing-bootstrap capture, never
    // promotes a planned or current-frame draw into a returned earlier draw.
    for(unsigned missing=0;missing<5;++missing) {
        Collector c;auto z=zero;
        if(missing==4)z.frame=failure.frame;
        c.offer(z);
        if(missing!=0)c.issue(z.frame,z.q);
        if(missing!=1)c.returned(z.frame,z.q);
        if(missing!=2&&missing!=3)c.selected(z.frame,z.hdr,z.raw.resource,z.source.buffer,z.camera);
        if(missing==3)c.abandon(z.frame,z.q);
        c.offer(failure);
        expect(c.frozen&&!c.matched&&samePayload(c.failure,failure)&&state(c,"captured-missing-bootstrap"),
               "unissued unreturned unselected abandoned and same-frame bootstrap each remain missing");
        c.emitted=true;
        expect(state(c,"emitted-missing-bootstrap"),"missing-bootstrap outcome has its own terminal emission status");
    }
    // Alter one physical cohort coordinate at a time. Write stamps may advance
    // between frames; resource generation, bank, range and view identity may not.
    for(unsigned coordinate=0;coordinate<21;++coordinate) {
        Collector c;c.offer(zero);complete(c,zero);auto different=failure;
        switch(coordinate){
        case 0:++different.context;break;case 1:++different.device;break;case 2:++different.hdr;break;
        case 3:++different.raw.resource;break;case 4:--different.raw.width;break;case 5:--different.raw.height;break;
        case 6:++different.raw.mip;break;case 7:++different.raw.slice;break;case 8:++different.raw.samples;break;
        case 9:++different.raw.slices;break;case 10:different.raw.known=false;break;
        case 11:++different.linear.resource;break;case 12:++different.linear.mip;break;case 13:++different.linear.slice;break;
        case 14:--different.width;break;case 15:--different.height;break;case 16:++different.source.buffer;break;
        case 17:++different.source.generation;break;case 18:++different.source.bank;break;
        case 19:++different.source.first;break;case 20:--different.source.count;break;
        }
        c.offer(different);
        expect(c.frozen&&!c.matched,"a different cohort cannot borrow zero-phase authority from the selected primary domain");
        expect(selectedCohort(zero,different)==(coordinate>=11&&coordinate<=13),"last-selected source matching checks every physical cohort field while allowing a new linear publication");
    }
    {
        Collector c;c.offer(zero);complete(c,zero);auto current=failure;
        ++current.source.writeGeneration;++current.source.writeQ;++current.donor.writeGeneration;
        ++current.original[0].writeGeneration;++current.original[1].mutation;
        c.offer(current);
        expect(c.matched,"normal per-frame write and original-buffer stamps do not split a stable source cohort");
    }
    {
        Collector c;c.offer(zero);complete(c,zero);c.lifecycle();c.issue(zero.frame,zero.q);c.returned(zero.frame,zero.q);
        c.offer(failure);
        expect(c.frozen&&!c.matched&&c.cancelled==1,"lifecycle invalidates pre-resize cohort and stale callbacks cannot revive it");
    }
    {
        Collector c;c.offer(zero);complete(c,zero);auto latest=zero;latest.frame=11;latest.q=40;latest.source.frame=11;
        latest.cb[1][0]^=0x55;c.offer(latest);complete(c,latest);
        c.offer(receipt(12,41,true));
        expect(c.matched&&c.paired.frame==11&&samePayload(c.paired,latest),"a stable cohort retains its newest completed zero-phase input");
    }
    {
        Collector c;c.offer(zero);complete(c,zero);auto latest=zero;latest.frame=11;latest.q=40;c.offer(latest);
        c.offer(receipt(12,41,true));
        expect(c.frozen&&c.matched&&c.paired.frame==zero.frame&&samePayload(c.paired,zero),"a newer pending observation preserves the last completed selected bootstrap");
    }
    {
        Collector c;c.offer(zero);complete(c,zero);auto latest=zero;latest.frame=11;latest.q=40;
        c.offer(latest);c.issue(latest.frame,latest.q);c.abandon(latest.frame,latest.q);c.returned(latest.frame,latest.q);
        c.offer(receipt(12,41,true));
        expect(c.matched&&c.paired.frame==zero.frame&&samePayload(c.paired,zero),"abandoning a newer pending draw preserves the last successfully selected bootstrap");
    }
    {
        Collector c;Receipt samples[kSlots+1]{};
        for(unsigned i=0;i<kSlots+1;++i){samples[i]=receipt(10+i,30+i);samples[i].hdr+=i*10;
            samples[i].raw.resource+=i*10;samples[i].linear.resource+=i*10;samples[i].source.buffer+=i*10;
            c.offer(samples[i]);complete(c,samples[i]);}
        unsigned occupied=0;for(bool used:c.used)occupied+=used?1u:0u;
        expect(kSlots==8&&occupied==8&&c.overflow==1&&!c.frozen,"ninth distinct domain records bounded eviction without refusing or growing the collector");
        for(unsigned i=0;i<kSlots+1;++i){Collector copy=c;auto f=samples[i];f.frame=100;f.q=200;
            f.pixel[0]=failure.pixel[0];f.pixel[1]=failure.pixel[1];f.ndc=failure.ndc;f.diagnostic=failure.diagnostic;f.originalReason=failure.originalReason;f.lastSelectedDomain=true;
            copy.offer(f);
            expect(copy.frozen&&copy.matched==(i!=0),"the latest eight distinct domains remain pairable and the oldest is explicitly missing");}
    }
    {
        Collector c;c.note(false);c.note(true);
        expect(c.entries==2&&c.notArmed==1&&!c.calls&&state(c,"awaiting-issued-selected-bootstrap"),
               "called-but-unarmed remains distinguishable from never-called and offered inputs");
        auto wrong=failure;wrong.originalReason=Reason::View;c.offer(wrong);
        expect(!c.frozen&&!c.primaryFailures,"diagnostic Phase cannot create a refusal absent from the production decision");
        wrong=failure;wrong.diagnostic.reason=Reason::None;c.offer(wrong);
        expect(c.frozen&&c.failure.originalReason==Reason::Phase&&c.failure.diagnostic.reason==Reason::None,
               "production phase refusal still captures any diagnostic disagreement as evidence");
    }
    {
        Collector c;
        for(unsigned i=0;i<10;++i)c.phase(i+1,.125f*i,-.25f*i,3840,2160);
        c.phase(10,999,999,1,1);
        c.offer(zero);complete(c,zero);c.offer(receipt(100,200,true));
        expect(c.frozen&&c.failure.recentCount==8&&c.failure.recent[0].frame==3&&c.failure.recent[7].frame==10,
               "capture carries the latest eight requested phase frames in chronological order");
        bool exact=true;
        for(unsigned i=0;i<8;++i){const auto& p=c.failure.recent[i];const unsigned tick=i+2;
            exact=exact&&p.epoch==c.failure.epoch&&p.width==3840&&p.height==2160&&p.pixel[0]==.125f*tick&&p.pixel[1]==-.25f*tick;}
        expect(exact,"per-draw repeated phase calls neither duplicate nor overwrite the frame request");
        c.lifecycle();c.phase(101,5,6,100,100);
        expect(c.failure.recentCount==8&&c.failure.recent[7].frame==10,"frozen request history survives resize and later phase callbacks verbatim");
        Collector reset;reset.phase(1,1,2,3840,2160);reset.lifecycle();reset.offer(zero);complete(reset,zero);reset.offer(failure);
        expect(reset.failure.recentCount==0,"a fresh lifecycle cannot attach prior-epoch requested phases to a new capture");
    }
    {
        auto selected=zero,current=failure;selected.epoch=current.epoch=1;
        expect(selectedCohort(selected,current),"last-selected source identity survives ordinary per-frame write changes");
        ++current.epoch;
        expect(!selectedCohort(selected,current),"pre-teardown selected source cannot authorize a new lifecycle epoch");
    }
    std::printf("phase failure collector: %u checks, %d failure(s)\n",checks,failures);
    return failures;
}
} // namespace flat_phase_failure_test

inline int flatPhaseFailureCaptureTests() {
    return flat_phase_failure_test::algebraTests()+flat_phase_failure_test::collectorTests();
}