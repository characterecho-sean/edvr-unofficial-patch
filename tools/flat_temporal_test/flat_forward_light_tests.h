#pragma once
#include "../../src/d3d11/flat_forward_light.h"
#include <cstdio>
#include <limits>

namespace forward_light_test {
using namespace edvr;
using namespace edvr::forward_light;
struct Fixture {
    float camera[6][4]{},cb[47][4]{};
    Reason result(const FlatProjectionJitter& phase={})const {
        return match(reinterpret_cast<const unsigned char*>(camera),reinterpret_cast<const unsigned char*>(cb),phase);
    }
    const unsigned char* bytes()const{return reinterpret_cast<const unsigned char*>(camera);}
};
// Independent world-to-view construction. These rational axes are orthonormal,
// right-handed, and neither identity nor an axis permutation. Forward rows are
// derived from clip=(sx*view.x+dx*view.z,sy*view.y+dy*view.z,near,view.z).
inline Fixture scene(float dx=0,float dy=0,float sx=1.25f,float sy=2.0f,bool shear=false,bool reflect=false,bool zeroOrigin=false) {
    Fixture f{};
    double r[3][3]={{.36,.48,-.8},{-.8,.6,shear?.125:0},{.48,.64,.6}};
    if(reflect)for(double& x:r[1])x=-x;
    const double origin[3]={zeroOrigin?0:3.25,zeroOrigin?0:-2.,zeroOrigin?0:7.5};
    for(unsigned a=0;a<3;++a) {
        double t=0;
        for(unsigned c=0;c<3;++c){f.cb[2+a][c]=static_cast<float>(r[a][c]);t-=r[a][c]*origin[c];}
        f.cb[2+a][3]=static_cast<float>(t);f.camera[5][a]=static_cast<float>(origin[a]);
    }
    for(unsigned a=0;a<4;++a) {
        double translation=0;
        for(unsigned c=0;c<3;++c) {
            const double coefficient=a==0?sx*r[0][c]+dx*r[2][c]:a==1?sy*r[1][c]+dy*r[2][c]:a==2?0:r[2][c];
            f.cb[10+a][c]=static_cast<float>(coefficient);f.camera[c][a]=static_cast<float>(coefficient);
            translation-=coefficient*origin[c];
        }
        f.cb[10+a][3]=a==2?.025f:static_cast<float>(translation);
    }
    f.camera[3][2]=.025f;
    for(unsigned c=0;c<3;++c)f.camera[4][c]=static_cast<float>(r[2][c]);
    return f;
}
inline Surface surface(uint64_t resource) {
    Surface s{};s.resource=resource;s.width=3840;s.height=2160;s.slices=1;s.samples=1;s.known=true;return s;
}
inline Source source() {
    Source s{};s.buffer=101;s.generation=3;s.bank=2;s.writeGeneration=7;s.frame=19;s.writeQ=5;s.first=13;s.count=4096;return s;
}
inline int run() {
    unsigned checks=0;int failures=0;
    const auto expect=[&](bool ok,const char* text){++checks;if(!ok){++failures;std::printf("FAIL: forward light %s\n",text);}};
    auto f=scene();expect(f.result()==Reason::None,"centered nontrivial view and absolute forward agree");
    FlatProjectionJitter phase{};expect(flatProjectionJitter(.375f,-.25f,3840,2160,phase),"pixel-to-phase construction");
    for(float sign:{1.f,-1.f}) {
        auto p=phase;p.ndcX*=sign;p.ndcY*=sign;p.uvX*=sign;p.uvY*=sign;
        auto actual=scene(p.ndcX,p.ndcY);
        expect(actual.result(p)==Reason::None,"independently derived plus/minus clip displacement accepts");
        // Independent point oracle observes the intended displacement in NDC;
        // it never calls either production jitter helper or match's phase undo.
        const double point[3]={4.5,1.25,11.};double view[3]{};
        const double axes[3][3]={{.36,.48,-.8},{-.8,.6,0},{.48,.64,.6}};
        const double origin[3]={3.25,-2,7.5};
        for(unsigned a=0;a<3;++a)for(unsigned c=0;c<3;++c)view[a]+=axes[a][c]*(point[c]-origin[c]);
        double clip[4]{};
        for(unsigned a=0;a<4;++a){clip[a]=actual.cb[10+a][3];for(unsigned c=0;c<3;++c)clip[a]+=actual.cb[10+a][c]*point[c];}
        expect(std::abs(clip[0]/clip[3]-(1.25*view[0]/view[2]+p.ndcX))<2e-6&&
               std::abs(clip[1]/clip[3]-(2.*view[1]/view[2]+p.ndcY))<2e-6,"point oracle independently sees both signed NDC offsets");
        auto patched=scene();float columns[4][4],dp4[4][4];
        std::memcpy(columns,patched.camera,sizeof(columns));std::memcpy(dp4,patched.cb+10,sizeof(dp4));
        expect(flatJitterForwardColumns(columns,p)&&flatJitterForwardDp4(dp4,p),"actual upstream layouts apply signed phase");
        std::memcpy(patched.camera,columns,sizeof(columns));std::memcpy(patched.cb+10,dp4,sizeof(dp4));
        expect(patched.result(p)==Reason::None,"production phase layouts match independent view basis");
        expect(f.result(p)==Reason::Phase,"two actual mutually consistent UNPHASED matrices cannot prove expected nonzero phase");
        expect(scene(0,0,1e20f,1e20f).result(p)!=Reason::None,"rotated huge UNPHASED matrices cannot bury required phase below their rounding bounds");
        expect(actual.result()==Reason::Phase,"phased actual matrices cannot claim zero phase");
        expect(scene(-p.ndcX,-p.ndcY).result(p)==Reason::Phase,"wrong signed phase refuses");
        expect(scene(p.ndcX,0).result(p)==Reason::Phase&&scene(0,p.ndcY).result(p)==Reason::Phase,"each missing phase axis refuses");
        expect(!phaseReceipt(true,p.ndcX,p.ndcY,0)&&phaseReceipt(true,p.ndcX,p.ndcY,1),"nonzero numeric match still needs actual upstream application receipt");
    }
    expect(phaseReceipt(true,0,0,0)&&!phaseReceipt(false,0,0,1),"zero bootstrap needs ownership but no nonzero patch receipt");
    const float nan=(std::numeric_limits<float>::quiet_NaN)(),inf=(std::numeric_limits<float>::infinity)();
    expect(!phaseReceipt(true,nan,0,1)&&!phaseReceipt(true,0,inf,1),"nonfinite phase receipt refuses");
    auto badPhase=phase;badPhase.uvX=nan;expect(f.result(badPhase)!=Reason::None,"nonfinite ancillary phase refuses");
    expect(match(nullptr,reinterpret_cast<const unsigned char*>(f.cb),{})==Reason::OriginalMissing&&
           match(f.bytes(),nullptr,{})==Reason::OriginalMissing,"missing observed bytes explicit");
    auto bad=f;bad.camera[5][0]+=1;expect(bad.result()==Reason::Origin,"changed geometry origin refuses actual used transform");
    bad=f;bad.cb[10][3]+=1;expect(bad.result()==Reason::Origin,"relative-camera translation cannot stand in for absolute-world forward");
    bad=f;bad.camera[3][0]=1;expect(bad.result()==Reason::Forward,"donor relative translation is not silently treated as origin");
    bad=f;bad.cb[2][0]*=-1;expect(bad.result()!=Reason::None,"used orientation cannot disagree with donor forward");
    expect(scene(0,0,1.25f,2.f,true).result()==Reason::View,"coherent forward plus sheared used view refuses");
    expect(scene(0,0,1.25f,2.f,false,true).result()!=Reason::None,"coherent reflected view violates captured proper-rotation contract");
    expect(scene(0,0,(std::numeric_limits<float>::denorm_min)(),2).result()!=Reason::None,"subnormal projection axis is not a usable conditioned camera");
    auto enormous=scene(0,0,(std::numeric_limits<float>::max)(),2,false,false,true);
    expect(enormous.result()!=Reason::None,"finite projection with subnormal reciprocal axis refuses");
    bad=f;bad.cb[3][0]*=1.2f;expect(bad.result()!=Reason::None,"nonorthonormal used view refuses");
    expect(scene(.125f,-.2f).result()==Reason::Phase,"native asymmetric projection has no centered zero-phase authority");
    bad=f;bad.camera[0][2]=.01f;bad.cb[12][0]=.01f;expect(bad.result()==Reason::Forward,"unsupported finite-far projection refuses");
    bad=f;bad.camera[3][2]=bad.cb[12][3]=0;expect(bad.result()==Reason::Forward,"nonpositive near plane refuses");
    expect(scene(0,0,0,2).result()!=Reason::None&&scene(0,0,-1,2).result()!=Reason::None,"zero and reversed projection axes refuse");
    for(unsigned a:{2u,3u,4u,10u,11u,12u,13u})for(unsigned c=0;c<4;++c){bad=f;bad.cb[a][c]=nan;expect(bad.result()==Reason::OriginalMissing,"every consumed light scalar requires finite bytes");}
    for(unsigned a=0;a<6;++a)for(unsigned c=0;c<4;++c){bad=f;bad.camera[a][c]=inf;expect(bad.result()==Reason::OriginalMissing,"every donor scalar requires finite bytes");}
    bad=f;for(unsigned a=6;a<10;++a)for(float& x:bad.cb[a])x=nan;
    expect(bad.result()==Reason::None,"unused inverse view grants no origin authority and cannot veto used matrices");
    // Exact captured float bits: Steam b025e8fd sample_1, frame12922/q715,
    // current geometry group and original VS/PS CB2. Only consumed rows matter.
    const uint32_t capturedCamera[]={0xBEE295BDu,0xBFAE6CCDu,0x00000000u,0xB6CEA54Bu,0xBF443A67u,0x3F4968A7u,0x00000000u,0xB77599F5u,0x3769FB9Au,0xB651B873u,0x00000000u,0xBF800000u,0x00000000u,0x00000000u,0x3CCCCCCDu,0x00000000u,0xB6CEA54Bu,0xB77599F5u,0xBF800000u,0x00000000u,0xC070D2A0u,0x400ABF6Au,0xC18DD12Bu,0x00000000u};
    const uint32_t capturedView[]={0xBF000006u,0xBF5DB3D6u,0x37842DE3u,0xBB702DAFu,0xBF5DB3D6u,0x3F000006u,0xB605484Au,0xC08AF7A7u,0xB6CEA54Bu,0xB77599F5u,0xBF800000u,0xC18DD127u};
    const uint32_t capturedForward[]={0xBEE295BDu,0xBF443A67u,0x3769FB9Au,0xBB5494C6u,0xBFAE6CCDu,0x3F4968A7u,0xB651B873u,0xC0DAAA79u,0x00000000u,0x00000000u,0x00000000u,0x3CCCCCCDu,0xB6CEA54Bu,0xB77599F5u,0xBF800000u,0xC18DD127u};
    Fixture captured{};std::memcpy(captured.camera,capturedCamera,sizeof(capturedCamera));std::memcpy(captured.cb+2,capturedView,sizeof(capturedView));std::memcpy(captured.cb+10,capturedForward,sizeof(capturedForward));
    expect(captured.result()==Reason::None,"actual saved scalar bytes accept under operation-count bounds");
    captured.camera[5][0]+=.01f;expect(captured.result()==Reason::Origin,"captured fixture cannot hide centimeter origin disagreement");
    auto raw=surface(1000),linear=surface(2000);auto src=source();Ledger ledger{};ledger.begin(src.frame);
    bad=f;bad.camera[5][0]+=100;
    ledger.observe(raw,Role::ProjectionOnly,true,true,src,bad.bytes(),10);
    expect(ledger.used==1&&ledger.domains[0].hasProjection&&!ledger.domains[0].origins,"typed projection-only depth cannot donate origin");
    ledger.observe(raw,Role::Origin,true,true,src,f.bytes(),11);
    expect(ledger.domains[0].origins==1&&ledger.domains[0].failure==Reason::None&&!std::memcmp(ledger.domains[0].camera,f.camera,96),"world role establishes origin independently of projection-only origin bytes");
    auto version=src;++version.writeGeneration;version.writeQ=6;ledger.observe(raw,Role::Origin,true,true,version,f.bytes(),12);
    expect(ledger.domains[0].versions==3&&ledger.domains[0].origins==2&&ledger.domains[0].failure==Reason::None,"same source and range across current identical versions stay consistent");
    bad=f;bad.camera[5][1]+=.5f;ledger.observe(raw,Role::Origin,true,true,version,bad.bytes(),13);
    ledger.observe(raw,Role::Origin,true,true,version,f.bytes(),14);
    expect(ledger.domains[0].failure==Reason::Ambiguous,"supported nonpool conflicting origin remains sticky after matching observation");
    ledger.reset(src.frame);ledger.observe(raw,Role::Origin,true,true,src,f.bytes(),10);ledger.observe(raw,Role::Unknown,false,false,{},nullptr,11);
    expect(ledger.domains[0].failure==Reason::None,"unknown depth-readonly work does not poison source");
    ledger.observe(raw,Role::Unknown,true,false,{},nullptr,12);expect(ledger.domains[0].failure==Reason::UnknownWriter,"unknown actual depth writer poisons origin domain");
    for(unsigned field=0;field<7;++field){ledger.reset(src.frame);auto stale=src;bool current=true;const unsigned char* camera=f.bytes();
        if(field==0)current=false;if(field==1)stale.frame--;if(field==2)stale.writeQ=0;if(field==3)stale.writeQ=11;if(field==4)stale.buffer=0;if(field==5)stale.generation=0;if(field==6)camera=nullptr;
        ledger.observe(raw,Role::Origin,true,current,stale,camera,10);expect(ledger.domains[0].failure==Reason::StaleDonor,"current tracked source and preceding write required");}
    for(unsigned field=0;field<5;++field){ledger.reset(src.frame);ledger.observe(raw,Role::Origin,true,true,src,f.bytes(),10);auto changed=src;
        if(field==0)changed.buffer++;if(field==1)changed.generation++;if(field==2)changed.bank++;if(field==3)changed.first++;if(field==4)changed.count--;
        ledger.observe(raw,Role::Origin,true,true,changed,f.bytes(),11);expect(ledger.domains[0].failure==Reason::Ambiguous,"buffer generation bank and range independently establish source identity");}
    expect(typedRole(0x84F6596FAF22CCFAull,0)==Role::ProjectionOnly&&typedRole(0x84F6596FAF22CCFAull,1)==Role::Unknown,"projection role requires exact shader pair");
    expect(typedRole(0x989E043933A369ABull,0xBD43EB1E0AAEFC7Aull)==Role::Origin&&typedRole(0x7B0DC42D383F694Cull,0x0DF03E64DF9DBEF1ull)==Role::Origin,"known world shader pairs have typed origin roles");
    expect(typedRole(kVs,kPs)==Role::Unknown&&typedRole(1,2)==Role::Unknown,"light pair cannot donate its own geometry origin");
    const auto published=[&](){ledger.reset(src.frame);auto token=ledger.publish(raw,linear,20);ledger.issued(token);ledger.returned(token);return token;};
    Reason why=Reason::None;ledger.reset(src.frame);expect(!ledger.find(raw,linear,30,why)&&why==Reason::MissingProducer,"resource identity alone supplies no converter");
    auto token=ledger.publish(raw,linear,20);expect(!ledger.find(raw,linear,30,why)&&why==Reason::PendingProducer,"planned publication is not actual issue");
    ledger.returned(token);expect(!ledger.find(raw,linear,30,why)&&why==Reason::PendingProducer,"return before actual issue supplies no receipt");
    ledger.issued(token);expect(!ledger.find(raw,linear,30,why)&&why==Reason::PendingProducer,"actual issue without return stays pending");
    ledger.returned(token);expect(ledger.find(raw,linear,30,why)&&why==Reason::None,"same raw view through returned converter to linear view proves order");
    expect(!ledger.find(raw,linear,20,why)&&why==Reason::Lineage&&!ledger.find(raw,linear,19,why),"converter must strictly precede light draw");
    expect(ledger.find(raw,linear,UINT32_MAX,why)!=nullptr,"largest draw ordinal compares without addition overflow");
    for(unsigned field=0;field<8;++field){auto other=raw;
        if(field==0)other.resource++;if(field==1)other.width--;if(field==2)other.height--;if(field==3)other.mip++;if(field==4)other.slice++;if(field==5)other.slices++;if(field==6)other.samples++;if(field==7)other.known=false;
        expect(!plane(raw,other)&&!ledger.find(other,linear,30,why),"raw resource with differing view or subresource cannot alias DSV proof");
        other.resource=linear.resource+(field==0?1:0);expect(!ledger.find(raw,other,30,why),"linear output view must independently match all subresource fields");}
    ledger.domain(raw);auto absent=raw;absent.known=false;expect(ledger.domain(absent)->failure==Reason::Lineage,"unknown descriptor cannot reuse existing raw domain");
    ledger.reset(src.frame);expect(!ledger.domain(absent),"unknown surface cannot create donor domain");
    token=published();ledger.abandon(token);expect(!ledger.find(raw,linear,30,why)&&why==Reason::MutatedProducer,"abandoned actual converter cannot authorize light");
    for(auto resource:{raw.resource,linear.resource,uint64_t(0)}){published();ledger.mutation(resource);expect(!ledger.find(raw,linear,30,why)&&why==Reason::MutatedProducer,"raw or output clear/copy mutation invalidates converter receipt");}
    ledger.reset(src.frame);ledger.observe(raw,Role::Origin,true,true,src,f.bytes(),10);token=ledger.publish(raw,linear,20);ledger.issued(token);ledger.returned(token);
    ledger.observe(raw,Role::Origin,false,true,src,f.bytes(),21);expect(ledger.find(raw,linear,30,why)!=nullptr,"depth-readonly supported draw after converter preserves publication");
    ledger.mutation(raw.resource,false);expect(!ledger.find(raw,linear,30,why),"actual depth mutation invalidates converted output");
    expect(ledger.domains[0].failure==Reason::None,"ordinary depth write can preserve camera source while invalidating conversion");
    ledger.mutation(raw.resource);token=ledger.publish(raw,linear,22);ledger.issued(token);ledger.returned(token);
    expect(ledger.find(raw,linear,30,why)&&ledger.domains[0].failure==Reason::StaleDonor,"republishing after raw clear never revives poisoned donor");
    ledger.begin(src.frame+1);expect(!ledger.used&&!ledger.overflow&&!ledger.find(raw,linear,30,why),"frame advance drops donor and publication evidence");
    ledger.reset(src.frame);for(unsigned i=0;i<kDomains;++i)ledger.domain(surface(100+i));expect(ledger.used==kDomains&&!ledger.overflow,"all bounded domain slots usable");
    expect(!ledger.domain(surface(999))&&ledger.overflow&&!ledger.find(raw,linear,30,why)&&why==Reason::Overflow,"domain capacity loss refuses closed");
    ledger.reset(src.frame);for(unsigned i=0;i<kPublications;++i){auto out=surface(300+i);token=ledger.publish(raw,out,20);ledger.issued(token);ledger.returned(token);}
    expect(!ledger.overflow&&ledger.publish(raw,surface(999),21)==0&&ledger.overflow,"publication capacity loss explicit at first excess");
    ledger.reset(src.frame);expect(!ledger.used&&!ledger.overflow,"explicit mode reset clears all refusal state");
    ConstantIdentity id{};id.buffer=1;id.generation=2;id.mutation=3;id.writeGeneration=4;id.bank=5;id.writeFrame=19;id.first=13;id.count=47;id.width=784;id.writeQ=18;id.bytes=kBytes;id.hash=99;
    expect(same(id,id)&&!same(ConstantIdentity{},ConstantIdentity{}),"complete actual constant stamp stable; absent stamp cannot self-certify");
#define EDVR_FORWARD_STAMP_CHANGE(field) {auto changed=id;++changed.field;expect(!same(id,changed),"before/after " #field " difference refuses identity");}
    EDVR_FORWARD_STAMP_CHANGE(buffer) EDVR_FORWARD_STAMP_CHANGE(generation) EDVR_FORWARD_STAMP_CHANGE(mutation)
    EDVR_FORWARD_STAMP_CHANGE(writeGeneration) EDVR_FORWARD_STAMP_CHANGE(bank) EDVR_FORWARD_STAMP_CHANGE(writeFrame)
    EDVR_FORWARD_STAMP_CHANGE(first) EDVR_FORWARD_STAMP_CHANGE(count) EDVR_FORWARD_STAMP_CHANGE(width)
    EDVR_FORWARD_STAMP_CHANGE(writeQ) EDVR_FORWARD_STAMP_CHANGE(bytes) EDVR_FORWARD_STAMP_CHANGE(hash)
#undef EDVR_FORWARD_STAMP_CHANGE
    std::printf("forward light core: %u checks, %d failed\n",checks,failures);return failures;
}
} // namespace forward_light_test
