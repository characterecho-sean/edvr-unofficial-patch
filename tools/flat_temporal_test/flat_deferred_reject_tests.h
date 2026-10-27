#pragma once
// flatExplainWholeOmission (flat_deferred_reject.h) is the log-only account of why
// flatRepairDeferredWholeOmission refused. It must never disagree with the repair: it accepts exactly
// when the repair does, and every refusal names a stage and a check. Run on the thirty-one real
// captures that repair, the thirty-five inverse-only captures it refuses, and the negatives the
// repair's own rig uses.
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include "../../src/d3d11/flat_deferred_reject.h"
#include "flat_deferred_forward_omission_captures.h"
namespace deferred_reject {
inline unsigned run() {
    using namespace edvr;
    unsigned failures=0; char name[160];
    auto expect=[&](bool ok, const char* what){if(!ok){std::printf("FAIL: deferred reject %s\n", what); ++failures;}};
    unsigned refusedExplained=0;
    // One comparison: the repair and the explanation agree, and a refusal is named.
    auto agree=[&](const unsigned char* donor, const unsigned char* cb2, const FlatProjectionJitter& jitter, const char* label) {
        unsigned char scratch[112];
        const bool repair=flatRepairDeferredWholeOmission(donor,cb2,jitter,scratch);
        FlatDeferredReject why;
        const bool explained=flatExplainWholeOmission(donor,cb2,jitter,why);
        std::snprintf(name,sizeof(name),"%s: the explanation accepts exactly when the repair does",label);
        expect(repair==explained,name);
        std::snprintf(name,sizeof(name),"%s: a refusal names its stage and check, an acceptance names none",label);
        const bool named=repair?(std::strcmp(why.stage,"accepted")==0&&why.check[0]=='\0'):
            (std::strcmp(why.stage,"accepted")!=0&&why.stage[0]!='\0'&&why.check[0]!='\0');
        expect(named,name);
        if(!repair&&named)++refusedExplained;
        return why;
    };
    for(const auto& cap:deferred_forward_omission::kCaptures) {
        unsigned char donor[96], original[112], repaired[112];
        float px,py; std::memcpy(&px,&cap.phaseX,4); std::memcpy(&py,&cap.phaseY,4);
        FlatProjectionJitter jitter{};
        if(!(deferred_forward_omission::hexBytes(cap.donor,donor,96)&&deferred_forward_omission::hexBytes(cap.original,original,112)&&
             flatProjectionJitter(px,py,cap.width,cap.height,jitter))) { expect(false,"capture decodes"); continue; }
        std::snprintf(name,sizeof(name),"frame %u",cap.frame);
        agree(donor,original,jitter,name);
        if(!flatRepairDeferredWholeOmission(donor,original,jitter,repaired)) continue;
        FlatProjectionJitter none{};
        std::snprintf(name,sizeof(name),"frame %u zero phase",cap.frame);
        auto why=agree(donor,original,none,name);
        expect(std::strcmp(why.stage,"gate")==0&&std::strcmp(why.check,"zero-phase")==0,"zero phase is a gate refusal");
        auto scaled=[&](float f){auto j=jitter;j.ndcX*=f;j.ndcY*=f;j.uvX*=f;j.uvY*=f;return j;};
        std::snprintf(name,sizeof(name),"frame %u wrong-sign phase",cap.frame); agree(donor,original,scaled(-1.f),name);
        std::snprintf(name,sizeof(name),"frame %u half phase",cap.frame); agree(donor,original,scaled(.5f),name);
        std::snprintf(name,sizeof(name),"frame %u already repaired rows",cap.frame); agree(donor,repaired,jitter,name);
        for(unsigned index:{0u,5u,9u}) {
            unsigned char nudged[112]; std::memcpy(nudged,original,112);
            float coefficient; std::memcpy(&coefficient,nudged+index*4,4); coefficient+=2e-5f; std::memcpy(nudged+index*4,&coefficient,4);
            std::snprintf(name,sizeof(name),"frame %u forward coefficient %u 2e-5 off",cap.frame,index);
            why=agree(donor,nudged,jitter,name);
            expect(std::strcmp(why.stage,"match")==0||std::strcmp(why.stage,"dephase")==0,"a nudged forward coefficient is refused by the match or the dephase proof");
        }
        unsigned char badCamera[96]; std::memcpy(badCamera,donor,96); reinterpret_cast<float*>(badCamera)[0]+=.01f;
        std::snprintf(name,sizeof(name),"frame %u mismatched camera",cap.frame); agree(badCamera,original,jitter,name);
        unsigned char altered[112]; std::memcpy(altered,original,112);
        float wrong; std::memcpy(&wrong,altered+16*4+12,4); wrong+=.01f; std::memcpy(altered+16*4+12,&wrong,4);
        std::snprintf(name,sizeof(name),"frame %u unrelated inverse mismatch",cap.frame); agree(donor,altered,jitter,name);
        unsigned char skewed[112]; std::memcpy(skewed,original,112);
        float skew; std::memcpy(&skew,skewed+16*1+4,4); skew+=.01f; std::memcpy(skewed+16*1+4,&skew,4);
        std::snprintf(name,sizeof(name),"frame %u unrelated forward mismatch",cap.frame); agree(donor,skewed,jitter,name);
        // A phase the repair accepts at this size, taken to a size where the same pixel phase is a different ndc.
        FlatProjectionJitter large{};
        if(flatProjectionJitter(px,py,2560,1440,large)) {
            std::snprintf(name,sizeof(name),"frame %u at 2560x1440",cap.frame); agree(donor,original,large,name);
        }
    }
    for(const auto& cap:deferred_rc5_camera::kCaptures) {
        unsigned char donor[96], original[112];
        float px,py; std::memcpy(&px,&cap.phaseX,4); std::memcpy(&py,&cap.phaseY,4);
        FlatProjectionJitter jitter{};
        if(!(deferred_rc5_camera::hexBytes(cap.donor,donor,96)&&deferred_rc5_camera::hexBytes(cap.original,original,112)&&
             flatProjectionJitter(px,py,cap.width,cap.height,jitter))) { expect(false,"rc.5 capture decodes"); continue; }
        std::snprintf(name,sizeof(name),"rc.5 frame %u",cap.frame);
        const auto why=agree(donor,original,jitter,name);
        expect(std::strcmp(why.stage,"match")==0&&std::strcmp(why.check,"inverse")==0,"an inverse-only camera is refused at the match stage, on the inverse");
    }
    expect(refusedExplained>=31*8,"the negatives were exercised");
    // The explanation is wired where the repair refuses, behind the same call, and the repair header is not its home.
    std::ifstream in("src/d3d11/flat_runtime.cpp"); std::string verifier,line;
    while(std::getline(in,line)) verifier+=line+'\n';
    const auto repairCall=verifier.find("flatRepairDeferredWholeOmission(donor.camera,raw,jitter,certified)");
    const auto note=verifier.find("noteWholeOmissionRefused(s,requestedFrame,requestedQ,k.width,k.height,donor.camera,raw,jitter,");
    const auto guard=verifier.rfind("if(nonzero && match==FlatDeferredCameraMatch::Forward && !missingPhase && !wholeOmission)",note);
    expect(repairCall!=std::string::npos&&note!=std::string::npos&&guard!=std::string::npos&&repairCall<guard&&guard<note,
           "the refusal is logged only for a nonzero forward-mismatch frame both repairs refused, after the repair ran");
    std::ifstream header("src/d3d11/flat_deferred_camera.h"); std::string camera;
    while(std::getline(header,line)) camera+=line+'\n';
    expect(camera.find("flat_deferred_reject")==std::string::npos&&camera.find("FlatDeferredReject")==std::string::npos,
           "the repair header carries no diagnostic");
    std::printf("flat deferred reject: %u failure(s)\n",failures);
    return failures;
}
}
