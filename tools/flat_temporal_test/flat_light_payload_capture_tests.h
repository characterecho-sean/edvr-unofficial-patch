#pragma once
#include "../../src/d3d11/flat_light_payload_capture.h"
#include <memory>
#include <cstdio>
#include <fstream>
#include <iterator>
namespace light_payload_test {
using namespace edvr::lightaudit;
inline void complete(Sample& s) {
    s={};s.generation=3;s.frame=101;s.q=29;s.h=1010;s.depth=2020;s.actualShaders=1;
    s.preForwardVs=kVs;s.preForwardPs=kPs;s.preForward=s.drawReturned=s.handoff=1;s.handoffQ=40;s.depthSurface.known=s.depthSurface.bindingMatches=1;
    for(unsigned stage=0;stage<2;++stage) {
        auto& cb=s.original[stage];cb.buffer=3000+stage;cb.stage=stage;cb.slot=2;
        cb.first=13+stage*16;cb.count=64;cb.width=65536;cb.rangeKnown=1;
        cb.before.tracked=cb.before.shadow=1;cb.before.generation=5;cb.before.mutation=7;
        cb.before.width=cb.width;cb.before.writeFrame=s.frame;cb.before.writeQ=17;
        cb.after=cb.before;
        for(unsigned i=0;i<kDeclaredBytes;++i)cb.payload[i]=static_cast<unsigned char>(i+stage);
        finish(cb,true,kDeclaredBytes,s.frame,s.q);
        auto& effective=s.effective[stage];effective.observed=cb;effective.association=1;
    }
    s.donorCount=2;
    for(unsigned i=0;i<2;++i){auto& d=s.donors[i];d.frame=s.frame;d.q=11+i;
        d.depthSurface.known=d.depthSurface.bindingMatches=1;d.b1=6000+i;d.depth=s.depth;d.current=d.supported=d.rangeMatches=1;
        d.original=s.original[0];d.original.row=270;d.original.bytes=kCameraBytes;d.original.first=i*16;
        d.original.width=65536;d.original.count=4096;d.original.buffer=d.b1;
        for(unsigned b=0;b<kCameraBytes;++b)d.camera[b]=d.original.payload[b]=static_cast<unsigned char>(b+i);
    }
    for(unsigned i=0;i<kInputs;++i){auto& in=s.inputs[i];in.bound=in.descriptorKnown=in.rangeKnown=1;
        in.stage=i?1:0;in.slot=i?i-1:0;in.resource=7000+i;in.view=8000+i;
        in.stride=i?0:48;in.contentsCaptured=0;
        if(i){in.producerKnown=1;in.producer.frame=s.frame;in.producer.lastQ=15;in.count=1;in.slices=1;in.producer.rangeKnown=1;in.producer.slices=1;}
    }
}
inline int run() {
    unsigned checks=0;int failures=0;
    const auto expect=[&](bool value,const char* name){++checks;if(!value){++failures;std::printf("FAIL: light payload %s\n",name);}};
    auto c=std::make_unique<Collector>();auto s=std::make_unique<Sample>();auto decoded=std::make_unique<Sample>();
    expect(!c->active()&&!c->begin(1,2,3,4),"unarmed entry cannot spend budgets");
    c->arm(1,9);expect(c->active()&&!c->attempts&&!c->pending,"armed never-ran differs from capture");
    expect(!c->expired(0)&&!c->expired(900)&&c->expired(901),"never-ran capture has independent bounded expiration");
    complete(*s);expect(bytesComplete(*s)&&recordComplete(*s),"all distinct donor versions and declared inputs recorded");
    expect(s->original[0].buffer!=s->original[1].buffer&&s->original[0].first!=s->original[1].first,"stages retain independent buffers and nonzero ranges");
    auto bytes=encode(*s);expect(decode(bytes.data(),bytes.size(),*decoded),"production wire roundtrip");
    expect(!std::memcmp(s.get(),decoded.get(),sizeof(Sample))&&decoded->donors[1].camera[95]==96&&decoded->original[1].payload[751]==240,"owned full declared payload and origin bytes survive serialization");
    expect(!decode(bytes.data(),bytes.size()-1,*decoded),"truncated payload refuses");
    bytes.back()^=1;expect(!decode(bytes.data(),bytes.size(),*decoded),"checksum rejects damaged payload");
    bytes=encode(*s);bytes[8]^=1;expect(!decode(bytes.data(),bytes.size(),*decoded),"wire version refuses");
    s->donorCount=kDonors+1;bytes=encode(*s);expect(!decode(bytes.data(),bytes.size(),*decoded),"serialized count cap validated");complete(*s);
    expect(range(16,47,65536,0,47)&&!range(16,46,65536,0,47)&&!range(UINT32_MAX,47,65536,0,47),"actual offset range coverage and overflow");
    auto cb=s->original[0];cb.after.mutation++;finish(cb,true,kDeclaredBytes,s->frame,s->q);
    expect(cb.status==Changed&&!cb.bytes,"mixed CPU write refuses bytes");
    cb=s->original[0];cb.before.mapped=1;finish(cb,true,kDeclaredBytes,s->frame,s->q);expect(cb.status==Mapped&&!cb.bytes,"mapped original refuses");
    cb=s->original[0];cb.before.pending=1;finish(cb,true,kDeclaredBytes,s->frame,s->q);expect(cb.status==Pending&&!cb.bytes,"pending original refuses");
    cb=s->original[0];cb.before.tracked=0;finish(cb,true,kDeclaredBytes,s->frame,s->q);expect(cb.status==Untracked&&!cb.bytes,"private fallback cannot masquerade as tracked original");
    cb=s->original[0];cb.before.writeFrame--;cb.after=cb.before;finish(cb,true,kDeclaredBytes,s->frame,s->q);
    expect(cb.bytes==kDeclaredBytes&&cb.fresh==Older,"older writes preserve bytes but not current-write claim");
    s->original[0]=cb;expect(bytesComplete(*s)&&!recordComplete(*s),"older originals remain payload-only");complete(*s);
    s->original[1]={};expect(!bytesComplete(*s)&&!recordComplete(*s),"zero initialized payload is not valid bytes");complete(*s);
    for(unsigned i=0;i<kInputs;++i){s->inputs[i].bound=0;expect(!recordComplete(*s),"each declared binding required");s->inputs[i].bound=1;}
    s->donors[0].depthSurface.known=0;expect(!recordComplete(*s),"current donor missing depth descriptor stays partial");complete(*s);
    s->donors[1].depthSurface.bindingMatches=0;expect(!recordComplete(*s),"current donor actual depth binding mismatch stays partial");complete(*s);
    s->inputs[1].producer.rangeKnown=0;expect(!recordComplete(*s),"unsupported producer RTV range stays partial");complete(*s);
    s->inputs[1].producerKnown=0;expect(!recordComplete(*s),"missing texture producer consumes remaining attempts");complete(*s);
    s->inputs[1].resourceAlias=1;expect(!recordComplete(*s),"depth resource equality alone cannot prove subresource association");
    s->inputs[1].subresourceAlias=1;s->inputs[1].producer.depthWrite=1;expect(recordComplete(*s),"recorded depth-view and depth-write observation retained");
    s->inputs[1].ledgerKnown=1;s->inputs[1].ledgerFrame=s->frame;s->inputs[1].ledgerQ=16;expect(!recordComplete(*s),"later texture write invalidates producer observation");complete(*s);
    s->inputs[0].rangeKnown=0;expect(!recordComplete(*s),"unsupported input view range not complete");complete(*s);
    s->effective[0].observed={};s->effective[0].observed.status=Untracked;s->effective[0].association=2;
    s->effective[0].expectedBytes=kDeclaredBytes;s->effective[0].expected[0]=99;
    expect(recordComplete(*s)&&!s->effective[0].observed.bytes&&s->effective[0].observed.status==Untracked&&s->original[0].payload[0]!=99,"uploaded-clone plan bytes remain separate from observed/original shadow");
    s->effective[0].expectedBytes=0;expect(!recordComplete(*s),"unavailable effective and unavailable clone bytes refuse useful stop");complete(*s);
    for(auto flag:{&s->uncertain,&s->donorOverflow,&s->writerOverflow,&s->producerOverflow,&s->writeEvictions,&s->mutation,&s->abandoned,&s->lateWrites}){
        *flag=1;expect(!recordComplete(*s),"partial ambiguity overflow mutation or abandoned record cannot stop early");*flag=0;
    }
    s->preForward=0;expect(!recordComplete(*s),"missing pre-forward explicit");s->preForward=1;s->drawReturned=0;expect(!recordComplete(*s),"missing draw return explicit");complete(*s);
    c->arm(1,3);Donor d=s->donors[0];d.frame=101;c->donor(d);c->donor(d);expect(c->historyCount==1,"same source version coalesces");
    ++d.original.before.generation;c->donor(d);++d.original.first;c->donor(d);expect(c->historyCount==3,"new source generation and range retain distinct versions");
    for(unsigned i=0;i<kDonors+2;++i){d.b1=10000+i;c->donor(d);}expect(c->historyCount==kDonors&&c->historyOverflow,"bounded donor overflow explicit");
    expect(c->begin(101,29,1010,2020)&&c->sample.donorCount==kDonors&&c->sample.donorOverflow,"anchor freezes every same-depth candidate including ambiguity");
    for(unsigned i=0;i<kWriters+2;++i)c->writer(101,1010,{1,2,2020,3,i,26});expect(c->sample.writerCount==kWriters&&c->sample.writerOverflow==2,"writer bounded overflow");
    c->writtenResource(2020);expect(c->sample.mutation,"depth mutation invalidates open evidence");
    c->finishFrame();expect(c->pending&&c->good==0&&!c->begin(192,1,1010,2020),"pending sample cannot be overwritten");c->saved(true);
    c->arm(1,3);
    for(unsigned i=0;i<4;++i){const uint64_t frame=101+i*kSpacing;expect(c->begin(frame,29,1010,2020),"bounded partial retry");c->sample=*s;c->sample.original[0].fresh=Older;c->finishFrame();c->saved(true);}
    expect(c->done&&c->attempts==4&&c->written==4&&!c->good&&!c->active(),"payload-only does not end at two samples");
    c->arm(1,3);
    for(unsigned i=0;i<3;++i){expect(c->begin(101+i*kSpacing,29,1010,2020),"useful retry with failed save");c->sample=*s;c->finishFrame();expect(c->good==(i?i-1:0),"unsaved complete records do not count");c->saved(i!=0);}
    expect(c->done&&c->good==2&&c->written==2&&c->writeFailures==1,"only two verified useful saves stop capture");
    c->arm(1,4);expect(c->begin(101,29,1010,2020),"rearm starts a new generation after owner drains previous");c->sample.abandoned=1;c->finishFrame();c->saved(true);expect(c->good==0,"cancelled snapshot cannot be useful");
    std::printf("light payload collector: %u checks, %d failed\n",checks,failures);return failures;
}
inline int wiring(const std::string& runtime,const std::string& inl,const std::string& scope) {
    int fail=0;const auto expect=[&](bool ok,const char* n){if(!ok){++fail;std::printf("FAIL: light payload wiring %s\n",n);}};
    const auto arm=runtime.find("armLightAudit(s,frame);");
    const auto wake=runtime.find("endStandDown(s, frame, \"an F10 audit asked for everything\");");
    const auto full=runtime.find("if(flatCaptureBulk(captureRequest))");
    expect(full<wake&&wake<arm&&arm<runtime.find("if(!s.projection)",arm),"manual Full arms after wake, before projection initialization");
    expect(runtime.find("finishLightAudit(s,\"arm-refused-projection-unavailable\")",arm)!=std::string::npos,"failed projection init drains new collector");
    expect(runtime.find("lightAuditFrameEnd(s,frame)")<runtime.find("const bool enabled = temporalModeEnabled(mode)"),"completed frame drains before AA-off return");
    const auto pre=runtime.find("lightPayloadToken=captureLightPayloadBefore");
    expect(pre<runtime.find("projection.emplace(*projectionPlan)",pre),"original capture precedes private binding substitution");
    expect(runtime.find("captureLightPreForward(state(),ctx,lightPayloadToken")<runtime.find("void FlatRuntimeDrawScope::endActualDraw()"),"pre-forward observation is in actual draw scope");
    const auto anchorForcesDraw=[](const std::string& header) {
        const auto begin=header.find("bool needsActualDraw() const {");if(begin==std::string::npos)return false;
        const auto end=header.find('}',begin),ret=header.find("return ",begin);
        if(end==std::string::npos||ret==std::string::npos||ret>=end)return false;
        const auto semi=header.find(';',ret);if(semi==std::string::npos||semi>=end)return false;
        const auto expression=header.substr(ret,semi-ret);
        return expression.find("return lightPayloadToken ||")!=std::string::npos||
            expression.find("|| lightPayloadToken ||")!=std::string::npos;
    };
    expect(anchorForcesDraw(scope),"diagnostic anchor forces actual draw callbacks");
    for(const char* replacement:{"false","(false && lightPayloadToken)"}) {
        auto mutant=scope;const auto method=mutant.find("bool needsActualDraw() const {");
        const auto token=method==std::string::npos?std::string::npos:mutant.find("lightPayloadToken",method);
        if(token!=std::string::npos)mutant.replace(token,std::strlen("lightPayloadToken"),replacement);
        expect(token!=std::string::npos&&!anchorForcesDraw(mutant),"removed or disabled diagnostic token fails callback wiring");
    }
    expect(inl.find("if(!lightLive(s))return 0;")!=std::string::npos&&inl.find("flatOriginCapture")==std::string::npos,"manual capture independent of F1 startup flag");
    expect(inl.find("c.expired(frame)")!=std::string::npos&&inl.find("manifest-verified=%u pending=0 drained=1")!=std::string::npos,"independent timeout and verified drained completion");
    return fail;
}
inline int report(const char* path) {
    std::ifstream f(path,std::ios::binary|std::ios::ate);
    if(!f||f.tellg()!=static_cast<std::streamoff>(sizeof(WireHeader)+sizeof(Sample))){std::puts("light payload: missing or wrong-size file");return 2;}
    f.seekg(0);std::vector<unsigned char> bytes(sizeof(WireHeader)+sizeof(Sample));f.read(reinterpret_cast<char*>(bytes.data()),bytes.size());
    auto s=std::make_unique<Sample>();if(!f||!decode(bytes.data(),bytes.size(),*s)){std::puts("light payload: invalid version/checksum/count/status");return 2;}
    std::printf("light payload v%u generation=%llu frame=%llu q=%u H=%llX depth=%llX pre-forward=%u returned=%u abandoned=%u bytes-complete=%u record-complete=%u authorization=UNKNOWN\n",kVersion,s->generation,s->frame,s->q,s->h,s->depth,s->preForward,s->drawReturned,s->abandoned,bytesComplete(*s),recordComplete(*s));
    const auto stamp=[](const Stamp& v){std::printf(" gen=%llu mutation=%llu write-generation=%llu bank=%llu write=%llu/%u tracked=%u mapped=%u pending=%u shadow=%u",v.generation,v.mutation,v.writeGeneration,v.bank,v.writeFrame,v.writeQ,v.tracked,v.mapped,v.pending,v.shadow);};
    const auto hex=[](const unsigned char* p,unsigned n){for(unsigned i=0;i<n;++i)std::printf("%02X",unsigned(p[i]));std::puts("");};
    const auto cb=[&](const char* label,unsigned stage,const Cb& b){std::printf("%s stage=%u cb=%llX range=%u/%u width=%u row=%u status=%u freshness=%u bytes=%u offset=%u",label,stage,b.buffer,b.first,b.count,b.width,b.row,b.status,b.fresh,b.bytes,b.byteOffset);stamp(b.before);std::puts("");std::printf(" after:");stamp(b.after);std::puts("");if(b.bytes)hex(b.payload,b.bytes);};
    const auto depth=[](const DepthSurface& d){std::printf("depth resource=%llX dsv=%llX dimension=%u known=%u binding-match=%u mip=%u slices=%u/%u size=%ux%u format=%u view-format=%u mips=%u array=%u samples=%u flags=%u\n",d.resource,d.view,d.viewDimension,d.known,d.bindingMatches,d.mip,d.firstSlice,d.slices,d.width,d.height,d.format,d.viewFormat,d.mips,d.array,d.samples,d.flags);};
    std::printf("pre-forward VS=%llX PS=%llX upstream-owner=%u/%u audited-frame-only; original=pre-private replacement, not necessarily unjittered\n",s->preForwardVs,s->preForwardPs,s->upstreamOwnerBefore,s->upstreamOwnerPreForward);
    depth(s->depthSurface);
    for(unsigned i=0;i<2;++i){cb("original",i,s->original[i]);const auto& e=s->effective[i];cb("pre-forward-observed",i,e.observed);std::printf("plan original=%llX replacement=%llX revision=%llu/%llu ready=%u association=%u range=%u/%u uploaded-CPU-bytes=%u (not GPU receipt)\n",e.planOriginal,e.planReplacement,e.planRevision,e.preparedRevision,e.planReady,e.association,e.planFirst,e.planCount,e.expectedBytes);if(e.expectedBytes)hex(e.expected,e.expectedBytes);}
    std::printf("donors=%u overflow=%u producers-overflow=%u write-evictions=%u writers=%u overflow=%u input-contents=UNCAPTURED\n",s->donorCount,s->donorOverflow,s->producerOverflow,s->writeEvictions,s->writerCount,s->writerOverflow);
    for(unsigned i=0;i<s->donorCount;++i){const auto& d=s->donors[i];std::printf("donor[%u] q=%u color=%llX depth=%llX b1=%llX VS=%llX PS=%llX camera-hash=%llX upload=%llu/%u current=%u supported=%u pool=%u range-match=%u\n",i,d.q,d.color,d.depth,d.b1,d.vs,d.ps,d.cameraHash,d.uploadFrame,d.uploadQ,d.current,d.supported,d.poolFamily,d.rangeMatches);cb("donor-original",0,d.original);hex(d.camera,kCameraBytes);depth(d.depthSurface);}
    for(unsigned i=0;i<kInputs;++i){const auto& in=s->inputs[i];std::printf("input stage=%u slot=%u bound=%u view=%llX resource=%llX dimension=%u format=%u descriptor=%u range-known=%u first/count=%u/%u slice/count=%u/%u size=%ux%u samples=%u stride=%u resource-alias=%u subresource-alias=%u producer-known=%u producer=%llu/%u..%u VS=%llX PS=%llX ledger=%u:%llu/%u contents=UNCAPTURED\n",in.stage,in.slot,in.bound,in.view,in.resource,in.dimension,in.format,in.descriptorKnown,in.rangeKnown,in.first,in.count,in.mips,in.slices,in.width,in.height,in.samples,in.stride,in.resourceAlias,in.subresourceAlias,in.producerKnown,in.producer.frame,in.producer.firstQ,in.producer.lastQ,in.producer.vs,in.producer.ps,in.ledgerKnown,in.ledgerFrame,in.ledgerQ);}
    for(const auto& in:s->inputs)std::printf("input producer view=%llX slot=%u dimension=%u range-known=%u mip=%u slices=%u/%u depth-write-observation=%u invalidated=%u\n",in.producer.view,in.producer.targetSlot,in.producer.viewDimension,in.producer.rangeKnown,in.producer.mip,in.producer.firstSlice,in.producer.slices,in.producer.depthWrite,in.producer.bad);
    for(unsigned i=0;i<s->writerCount;++i){const auto& w=s->writers[i];std::printf("H-writer q=%u VS=%llX PS=%llX depth=%llX dsv=%llX format=%u\n",w.q,w.vs,w.ps,w.depth,w.dsv,w.format);}
    return 0;
}
}
