#pragma once
// The whole-camera omission repair on cameras whose phase the game landed a float32 step away from the request
// (flat_deferred_landed_phase_captures.h: 2560x1440 DLAA, donor camera at 1.0001x the requested phase, camera
// a few metres from the world origin). The translation proof used to refuse them: the x-row's z coefficient had
// cancelled to 1e-5, the proof carried the cancellation's rounding only for the forward rows, and the world
// multiply (about 11 m) turned that 2e-8 into a 2e-7 residual against a 1e-9 bound. The allowance now follows
// the multiply. These tests pin the acceptance and, as important, what stays refused.
#include <cstdio>
#include <cstring>
#include <cmath>
#include "../../src/d3d11/flat_deferred_reject.h"
#include "../../src/d3d11/flat_projection_bindings.h"
#include "flat_deferred_landed_phase_captures.h"
namespace deferred_landed_phase {
inline unsigned run() {
    using namespace edvr;
    unsigned failures=0; char name[160];
    auto expect=[&](bool ok, const char* what){if(!ok){std::printf("FAIL: deferred landed phase %s\n", what); ++failures;}};
    expect(sizeof(kCaptures)/sizeof(kCaptures[0])==11, "the probe flights keep their eleven distinct captures");
    unsigned phasesSeen=0; uint32_t seen[16]{};
    for(const auto& cap:kCaptures) {
        unsigned char donor[96], original[112], repaired[112], scratch[112];
        const bool bytes=deferred_rc5_camera::hexBytes(cap.donor,donor,96)&&deferred_rc5_camera::hexBytes(cap.original,original,112);
        float px,py; std::memcpy(&px,&cap.phaseX,4); std::memcpy(&py,&cap.phaseY,4);
        FlatProjectionJitter jitter{};
        const bool phase=bytes&&flatProjectionJitter(px,py,cap.width,cap.height,jitter);
        std::snprintf(name,sizeof(name),"frame %u decodes at %ux%u",cap.frame,cap.width,cap.height); expect(phase&&cap.width==2560&&cap.height==1440,name);
        if(!phase) continue;
        { bool known=false; for(unsigned i=0;i<phasesSeen;++i) known=known||seen[i]==cap.phaseX; if(!known&&phasesSeen<16) seen[phasesSeen++]=cap.phaseX; }
        std::snprintf(name,sizeof(name),"frame %u the plain matcher reports the forward rows",cap.frame);
        expect(flatMatchDeferredCamera(donor,original)==FlatDeferredCameraMatch::Forward,name);
        const bool repairedOk=flatRepairDeferredWholeOmission(donor,original,jitter,repaired);
        std::snprintf(name,sizeof(name),"frame %u whole-camera omission repairs",cap.frame); expect(repairedOk,name);
        FlatDeferredReject why;
        std::snprintf(name,sizeof(name),"frame %u the explanation accepts too",cap.frame); expect(flatExplainWholeOmission(donor,original,jitter,why),name);
        if(!repairedOk) { std::printf("  frame %u refused at %s/%s residual %.3g bound %.3g\n",cap.frame,why.stage,why.check,why.residual,why.bound); continue; }
        std::snprintf(name,sizeof(name),"frame %u corrected rows strictly match",cap.frame);
        expect(flatMatchDeferredCamera(donor,repaired,true)==FlatDeferredCameraMatch::Match,name);
        unsigned char entire[784]{}, clone[784]{}; std::memcpy(entire+160,original,112);
        const FlatProjectionPatchRequest patches[2]={{FlatProjectionPatchLayout::ForwardDp4,160,{}},
                                                     {FlatProjectionPatchLayout::InverseUvRay,224,{}}};
        std::snprintf(name,sizeof(name),"frame %u repair equals the forward and inverse patch clone",cap.frame);
        expect(flatPrepareProjectionPatch(entire,784,patches,2,jitter,clone,784)&&std::memcmp(clone+160,repaired,112)==0,name);
        // What must stay refused on these same cameras.
        auto scaled=[&](float f){auto j=jitter;j.ndcX*=f;j.ndcY*=f;j.uvX*=f;j.uvY*=f;return j;};
        FlatProjectionJitter none{};
        std::snprintf(name,sizeof(name),"frame %u zero phase",cap.frame); expect(!flatRepairDeferredWholeOmission(donor,original,none,scratch),name);
        std::snprintf(name,sizeof(name),"frame %u wrong-sign phase",cap.frame); expect(!flatRepairDeferredWholeOmission(donor,original,scaled(-1.f),scratch),name);
        std::snprintf(name,sizeof(name),"frame %u half phase",cap.frame); expect(!flatRepairDeferredWholeOmission(donor,original,scaled(.5f),scratch),name);
        std::snprintf(name,sizeof(name),"frame %u double phase",cap.frame); expect(!flatRepairDeferredWholeOmission(donor,original,scaled(2.f),scratch),name);
        std::snprintf(name,sizeof(name),"frame %u already repaired rows",cap.frame); expect(!flatRepairDeferredWholeOmission(donor,repaired,jitter,scratch),name);
        for(unsigned index:{0u,5u,9u}) {
            unsigned char nudged[112]; std::memcpy(nudged,original,112);
            float coefficient; std::memcpy(&coefficient,nudged+index*4,4); coefficient+=2e-5f; std::memcpy(nudged+index*4,&coefficient,4);
            std::snprintf(name,sizeof(name),"frame %u forward coefficient %u 2e-5 off",cap.frame,index);
            expect(!flatRepairDeferredWholeOmission(donor,nudged,jitter,scratch),name);
        }
        // A camera displaced from where its CB2 was built: the translation proof must still see it. The allowance
        // follows the world multiply, so this is the window that matters.
        for(unsigned index:{20u,21u,22u}) {
            unsigned char moved[96]; std::memcpy(moved,donor,96);
            float world; std::memcpy(&world,moved+index*4,4); world+=1e-3f; std::memcpy(moved+index*4,&world,4);
            std::snprintf(name,sizeof(name),"frame %u camera world %u moved by 1e-3",cap.frame,index);
            expect(!flatRepairDeferredWholeOmission(moved,original,jitter,scratch),name);
        }
        unsigned char movedX[96]; std::memcpy(movedX,donor,96);
        float wx; std::memcpy(&wx,movedX+20*4,4); wx+=1e-4f; std::memcpy(movedX+20*4,&wx,4);
        std::snprintf(name,sizeof(name),"frame %u camera world x moved by 1e-4",cap.frame);
        expect(!flatRepairDeferredWholeOmission(movedX,original,jitter,scratch),name);
        unsigned char badCamera[96]; std::memcpy(badCamera,donor,96); reinterpret_cast<float*>(badCamera)[0]+=.01f;
        std::snprintf(name,sizeof(name),"frame %u mismatched camera",cap.frame); expect(!flatRepairDeferredWholeOmission(badCamera,original,jitter,scratch),name);
        unsigned char altered[112]; std::memcpy(altered,original,112);
        float wrong; std::memcpy(&wrong,altered+16*4+12,4); wrong+=.01f; std::memcpy(altered+16*4+12,&wrong,4);
        std::snprintf(name,sizeof(name),"frame %u unrelated inverse mismatch",cap.frame); expect(!flatRepairDeferredWholeOmission(donor,altered,jitter,scratch),name);
        unsigned char skewed[112]; std::memcpy(skewed,original,112);
        float skew; std::memcpy(&skew,skewed+16*1+4,4); skew+=.01f; std::memcpy(skewed+16*1+4,&skew,4);
        std::snprintf(name,sizeof(name),"frame %u unrelated forward mismatch",cap.frame); expect(!flatRepairDeferredWholeOmission(donor,skewed,jitter,scratch),name);
    }
    expect(phasesSeen>=7, "the captures cover seven distinct x phases of the cycle (the eighth is zero, which the repair never sees)");
    std::printf("flat deferred landed phase: %u failure(s)\n",failures);
    return failures;
}
}
