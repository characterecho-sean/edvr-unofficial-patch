#pragma once
#include "../../src/d3d11/flat_deferred_failure_witness.h"
#include <algorithm>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
#include <vector>

namespace deferred_failure_payload_test {
using namespace edvr;
inline std::vector<unsigned char> decode(const char* line) {
    const std::string text(line);const auto at=text.find(" hex=");
    if(at==std::string::npos)return {};
    const auto hex=text.substr(at+5);if(hex=="unavailable"||hex.size()%2)return {};
    const auto digit=[](char c)->int{return c>='0'&&c<='9'?c-'0':c>='A'&&c<='F'?c-'A'+10:-1;};
    std::vector<unsigned char> bytes;
    for(size_t i=0;i<hex.size();i+=2){const int a=digit(hex[i]),b=digit(hex[i+1]);if(a<0||b<0)return {};bytes.push_back(static_cast<unsigned char>((a<<4)|b));}
    return bytes;
}
inline std::string region(const char* start,const char* stop,size_t limit=24000) {
    std::ifstream input("src/d3d11/flat_runtime.cpp");std::string line,out;bool reading=false;
    while(std::getline(input,line)) {
        if(!reading&&line.find(start)!=std::string::npos)reading=true;
        if(!reading)continue;
        if(!out.empty()&&line.find(stop)!=std::string::npos)break;
        out+=line+'\n';if(out.size()>limit)return {};
    }
    return out;
}
inline unsigned run() {
    unsigned failures=0,checks=0;
    const auto expect=[&](bool ok,const char* message){++checks;if(!ok){++failures;std::printf("FAIL: deferred failure payload %s\n",message);}};
    static_assert(kFlatDeferredCameraBytes==112,"Diagnostic payload must cover the complete deferred original");
    unsigned char original[112]{},donor[96]{};
    const uint32_t patterns[]={0x80000000u,0u,0x7FC12345u,0xFFC54321u,0x7F800000u,0xFF800000u,0x00000001u,0xFFFFFFFFu};
    for(unsigned i=0;i<28;++i){const uint32_t word=patterns[i%8]^(i>=8?i*0x10001u:0u);std::memcpy(original+i*4,&word,4);}
    for(unsigned i=0;i<24;++i){const uint32_t word=patterns[(i+3)%8]^(i>=8?i*0x10001u:0u);std::memcpy(donor+i*4,&word,4);}
    FlatDeferredFailureWitness w;w.frame=6862;w.q=770;w.hdr=reinterpret_cast<void*>(uintptr_t(0x12345678));
    w.contextAvailable=true;w.copied=true;w.original=original;w.donorCamera=donor;w.relation=FlatDeferredCameraMatch::Forward;
    std::memcpy(&w.phaseX,&patterns[0],4);std::memcpy(&w.phaseY,&patterns[2],4);
    char lines[4][1000]{};
    expect(flatFormatDeferredFailureWitness(w,lines),"complete same-call witness formats");
    expect(decode(lines[2])==std::vector<unsigned char>(original,original+112),"all 112 original bytes preserve signed zero, NaN payloads and endpoints");
    expect(decode(lines[3])==std::vector<unsigned char>(donor,donor+96),"all 96 donor bytes survive independently without float conversion");
    expect(std::string(lines[0]).find("phase-bits=80000000/7FC12345")!=std::string::npos,"pre-failure phase preserves signed zero and NaN bits");
    expect(std::string(lines[0]).find("relation=forward-mismatch same-call=1")!=std::string::npos,"relation and same-call provenance remain explicit");
    char join[128]{};std::snprintf(join,sizeof(join),"frame=%llu q=%u H=%p",static_cast<unsigned long long>(w.frame),w.q,w.hdr);
    for(const auto& line:lines)expect(std::string(line).find(join)!=std::string::npos,"every paired line joins the exact frame/q/H event");
    unsigned char before[sizeof(w)],bytesBefore[112];std::memcpy(before,&w,sizeof(w));std::memcpy(bytesBefore,original,112);
    flatFormatDeferredFailureWitness(w,lines);
    expect(!std::memcmp(before,&w,sizeof(w))&&!std::memcmp(bytesBefore,original,112),"formatter mutates neither input metadata nor camera bytes");
    // The source phase can reset after the witness is copied; formatting must
    // retain the requested values rather than sample that later state.
    w.applied=0xFEDCBA9876543210ull;
    float sourceX=w.phaseX,sourceY=w.phaseY;uint64_t sourceApplied=w.applied;auto captured=w;sourceX=sourceY=0;sourceApplied=0;
    expect(flatFormatDeferredFailureWitness(captured,lines)&&sourceX==0&&sourceY==0&&
        sourceApplied==0&&std::string(lines[0]).find("phase-bits=80000000/7FC12345")!=std::string::npos&&
        std::string(lines[0]).find("applied=18364758544493064720")!=std::string::npos,"captured phase and 64-bit application count survive subsequent source reset");
    w.original=reinterpret_cast<const unsigned char*>(uintptr_t(1));w.copied=false;w.donorCamera=nullptr;
    expect(flatFormatDeferredFailureWitness(w,lines)&&decode(lines[2]).empty()&&
        std::string(lines[2]).find("bytes=0 hex=unavailable")!=std::string::npos,"uncopied poison pointer is never read or described as zero data");
    expect(std::string(lines[3]).find("bytes=0 hex=unavailable")!=std::string::npos,"missing donor is explicit, not a fabricated zero camera");
    w.contextAvailable=false;w.copied=true;
    expect(flatFormatDeferredFailureWitness(w,lines)&&std::string(lines[1]).find("available=0")!=std::string::npos,"unavailable same-call context prevents reading even a copied poison pointer");
    w.contextAvailable=true;w.original=nullptr;
    expect(flatFormatDeferredFailureWitness(w,lines)&&decode(lines[2]).empty(),"copied flag alone cannot substitute for an available original");
    std::memset(original,0,sizeof(original));std::memset(donor,0,sizeof(donor));w.original=original;w.donorCamera=donor;
    expect(flatFormatDeferredFailureWitness(w,lines)&&decode(lines[2]).size()==112&&decode(lines[3]).size()==96&&
        std::string(lines[1]).find("available=1")!=std::string::npos,"valid all-zero payloads remain distinguishable from missing data");
    FlatDeferredFailureWitness absent;
    expect(flatFormatDeferredFailureWitness(absent,lines)&&std::string(lines[0]).find("same-call=0")!=std::string::npos&&
        std::string(lines[0]).find("relation=unavailable")!=std::string::npos&&decode(lines[2]).empty()&&decode(lines[3]).empty(),"binding-only failure defaults to explicit unavailable payloads");
    const auto max64=std::numeric_limits<uint64_t>::max();const auto max32=std::numeric_limits<uint32_t>::max();
    w.frame=w.calls=w.vs=w.ps=w.applied=w.generation=w.mutation=w.writeGeneration=w.bank=w.writeFrame=w.donorWriteFrame=w.donorHash=max64;
    w.q=w.first=w.count=w.width=w.writeQ=w.donorWriteQ=w.donorFirst=w.donorLast=max32;
    w.hdr=w.depth=w.buffer=w.donorColor=w.donorB1=reinterpret_cast<void*>(std::numeric_limits<uintptr_t>::max());
    struct Bounded {char lines[4][1000];unsigned char guard[16];} bounded;std::memset(&bounded,0xA5,sizeof(bounded));
    expect(flatFormatDeferredFailureWitness(w,bounded.lines),"worst numeric identities and both complete payloads fit fixed buffers");
    expect(std::string(bounded.lines[0]).find("applied=18446744073709551615")!=std::string::npos,"application counter retains its full 64-bit value");
    for(const auto& line:bounded.lines)expect(std::memchr(line,0,sizeof(line))!=nullptr,"every worst-case line terminates below 1000 bytes");
    expect(std::all_of(std::begin(bounded.guard),std::end(bounded.guard),[](unsigned char c){return c==0xA5;}),"fixed line formatting does not overwrite adjacent storage");
    // This is the production budget helper, not an alternate clocked formatter.
    expect(flatDeferredFailureWitnessDue(1,0,0),"first actual failure reports without manual arm or elapsed startup time");
    expect(!flatDeferredFailureWitnessDue(2,4999,0)&&flatDeferredFailureWitnessDue(2,5000,0),"periodic detail budget has the exact existing 5000-ms boundary");
    expect(!flatDeferredFailureWitnessDue(3,5001,5000)&&flatDeferredFailureWitnessDue(4,10000,5000),"admitted periodic event starts another bounded interval");
    const auto record=region("void recordDeferredFailure(","void expireDeferredFailure(",12000);
    const auto budget=record.find("if(flatDeferredFailureWitnessDue(");
    const auto format=record.find("flatFormatDeferredFailureWitness(witness,lines)");
    const auto stamp=record.find("s.deferredFailureLastLog=now;");
    expect(budget!=std::string::npos&&format!=std::string::npos&&stamp!=std::string::npos&&budget<format&&format<stamp&&
        record.find("flatFormatDeferredFailureWitness",format+1)==std::string::npos,"single formatter call is inside the existing admitted failure detail branch");
    const auto queries=record.find("VSGetConstantBuffers1(");
    expect(queries!=std::string::npos&&record.find("FlatDeferredFailureWitness witness=sameCall?*sameCall:FlatDeferredFailureWitness{};")<queries&&
        record.find("if(!sameCall){witness.frame=s.prefix.frame;witness.q=s.prefix.sequence+1;")<queries&&
        record.rfind("witness.phaseX=s.phase.currentX;witness.phaseY=s.phase.currentY;witness.applied=s.phase.applied;")<queries&&
        record.find("if(donor.camera){std::memcpy(donorCamera,donor.camera,sizeof(donorCamera));witness.donorCamera=donorCamera;}")<queries,
        "same-call phase is retained and default phase plus available donor bytes freeze before old context queries");
    expect(record.find("deferredFailureCapture.request")>stamp&&record.find("cameraProbe")==std::string::npos,
        "detail reporting does not depend on Full/probe/manual snapshot admission");
    const auto verifier=region("const FlatProjectionBindingPlan* verifyDeferredHdrCamera(","void reportHdrProjectionCapture(");
    const auto lambda=verifier.find("const auto reportFailure=");const auto end=verifier.find("};",lambda);
    const auto builder=lambda!=std::string::npos&&end!=std::string::npos?verifier.substr(lambda,end-lambda):std::string{};
    expect(!builder.empty()&&builder.find("witness.original=copied?raw:nullptr;")!=std::string::npos&&
        builder.find("witness.relation=match;")!=std::string::npos&&builder.find("recordDeferredFailure(s,k,why,actual,input,donor,firstWitness,&witness)")!=std::string::npos&&
        builder.find("GetConstantBuffers")==std::string::npos&&builder.find("copyConstants(")==std::string::npos&&builder.find("constantsMetadata(")==std::string::npos,
        "failure-only builder reuses verifier bytes and identities without new context or shadow reads");
    expect(verifier.find("const float requestedPhaseX=s.phase.currentX,requestedPhaseY=s.phase.currentY;")<lambda&&
        verifier.find("const uint64_t requestedApplied=s.phase.applied")<lambda&&verifier.find("requestedFrame=s.prefix.frame")<lambda&&
        verifier.find("const uint32_t requestedQ=s.prefix.sequence+1;")<lambda&&
        builder.find("witness.phaseX=requestedPhaseX;witness.phaseY=requestedPhaseY;witness.applied=requestedApplied;")!=std::string::npos&&
        builder.find("witness.frame=requestedFrame;witness.q=requestedQ;")!=std::string::npos&&lambda<verifier.find("flatRepairDeferredMissingPhase("),
        "verifier snapshots requested phase/count/event before repair and preparation and passes those values to failure report");
    for(const char* reason:{"deferred-inverse-preparation-failed","deferred-whole-preparation-failed","deferred-inverse-unproven"}) {
        const auto report=verifier.find(std::string("reportFailure(\"")+reason+"\")");
        const auto fail=verifier.find(std::string("failPhase(s,\"")+reason+"\")");
        expect(report!=std::string::npos&&fail!=std::string::npos&&report<fail,"verification failure reports before failPhase can zero requested phase");
    }
    const auto binding=region("        if(!deferredProjection->active()) {","    // Typed light authority",4000);
    expect(binding.find("recordDeferredFailure(s,k,\"deferred-inverse-binding-failed\"")<binding.find("failPhase(s,\"deferred-inverse-binding-failed\"")&&
        binding.find("&witness")==std::string::npos,"binding failure reports before phase reset without claiming original verifier bytes");
    std::printf("flat deferred failure payload: %u checks, %u failure(s)\n",checks,failures);return failures;
}
} // namespace deferred_failure_payload_test
