#pragma once
#include "../../src/d3d11/flat_projection_runtime.h"

void projectionRuntimeTests(ID3D11Device* device, ID3D11DeviceContext* base) {
    using namespace edvr;
    ComPtr<ID3D11DeviceContext1> ctx;
    check(SUCCEEDED(base->QueryInterface(IID_PPV_ARGS(ctx.GetAddressOf()))), "runtime Context1 available");
    if (!ctx) return;
    float raw[64]{};
    raw[0]=1; raw[5]=1; raw[10]=1; raw[15]=1;
    D3D11_BUFFER_DESC desc{}; desc.ByteWidth=sizeof(raw); desc.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
    D3D11_SUBRESOURCE_DATA initial{}; initial.pSysMem=raw;
    ComPtr<ID3D11Buffer> original;
    check(SUCCEEDED(device->CreateBuffer(&desc,&initial,original.GetAddressOf())), "runtime source CB");
    if (!original) return;
    FlatProjectionRuntime runtime;
    check(!runtime.observeCreateBuffer(original.Get(),raw), "pre-init creation never mutates bank");
    check(runtime.initialize(base), "runtime owner initialized");
    check(runtime.observeCreateBuffer(original.Get(),raw), "full initial CB captured");
    float copied[16]{};
    check(runtime.copyConstants(original.Get(),0,sizeof(copied),copied) &&
          std::memcmp(copied,raw,sizeof(copied))==0,"diagnostic copy uses full current raw shadow");
    copied[0]=123;
    check(!runtime.copyConstants(original.Get(),sizeof(raw)-16,sizeof(copied),copied) &&
          copied[0]==123,"out-of-range diagnostic copy preserves destination");
    UINT first=0,count=4096;auto* bound=original.Get();
    ctx->VSSetConstantBuffers1(1,1,&bound,&first,&count);
    FlatProjectionRuntimeRequest request{};
    request.stage=FlatProjectionStage::Vertex;request.slot=1;request.original=original.Get();
    request.firstConstant=0;request.constantCount=4096;request.patchCount=1;
    request.patches[0]={FlatProjectionPatchLayout::ForwardColumns,0,{}};
    FlatProjectionJitter zero{},phase{};
    check(flatProjectionJitter(.25f,-.25f,960,540,phase), "runtime phase conversion");
    check(runtime.preflight(&request,1,zero,0) && runtime.prepare(&request,1,zero,0)==nullptr &&
          runtime.status().zeroPhaseReady==1, "zero phase audits readiness without binding substitute");
    check(runtime.preflight(&request,1,phase,1), "nonzero recipe preflighted before draw");
    const auto* plan=runtime.prepare(&request,1,phase,1);
    check(plan!=nullptr,"known full write prepares qualified draw");
    if(plan){
        FlatProjectionBindingScope scope(*plan);
        check(scope.active(),"runtime plan binds private CB");
        ComPtr<ID3D11Buffer> seen;UINT f=999,n=0;
        ctx->VSGetConstantBuffers1(1,1,seen.GetAddressOf(),&f,&n);
        check(seen.Get()!=original.Get() && f==0 && n==4096,"private binding preserves exact range");
    }
    {ComPtr<ID3D11Buffer> seen;UINT f=999,n=0;
     ctx->VSGetConstantBuffers1(1,1,seen.GetAddressOf(),&f,&n);
     check(seen.Get()==original.Get() && f==0 && n==4096,"scope restores original binding");}
    const auto plansBeforePhases=runtime.status().preflights;
    const auto zeroBeforePhases=runtime.status().zeroPhaseReady;
    bool phasesReady=true;
    for(uint32_t i=0;i<64;++i){
        FlatProjectionJitter current{};
        const float pixelX=float(int(i%8)-4)/16.0f;
        const float pixelY=float(int((i/8)%8)-4)/16.0f;
        phasesReady=flatProjectionJitter(pixelX,pixelY,960,540,current) &&
            runtime.preflight(&request,1,current,100+i,false);
        const auto* currentPlan=phasesReady ? runtime.prepare(&request,1,current,100+i) : nullptr;
        const bool zeroPhase=pixelX==0.0f && pixelY==0.0f;
        phasesReady=phasesReady && (zeroPhase ? currentPlan==nullptr : currentPlan!=nullptr);
        if(!phasesReady) break;
        if(zeroPhase) continue;
        FlatProjectionBindingScope scope(*currentPlan);
        phasesReady=scope.active();
        if(!phasesReady) break;
    }
    check(phasesReady && runtime.status().preflights==plansBeforePhases+64 &&
          runtime.status().zeroPhaseReady==zeroBeforePhases+1,
          "64 live phases reuse one preflighted topology without plan exhaustion");
    FlatProjectionRuntimeRequest missingTopology=request;missingTopology.slot=2;
    missingTopology.patchCount=2;
    missingTopology.patches[1]={FlatProjectionPatchLayout::ForwardColumns,64,{}};
    ctx->VSSetConstantBuffers1(2,1,&bound,&first,&count);
    const auto queuedBefore=runtime.status().coldQueued;
    check(runtime.preflight(&missingTopology,1,phase,1,false) &&
          runtime.prepare(&missingTopology,1,phase,1)!=nullptr &&
          runtime.status().coldQueued==queuedBefore,
          "live first-seen two-patch topology uses existing private buffer without cold readback");
    check(!runtime.failure().valid,"live first-seen success has no failure snapshot");
    check(runtime.preflight(&missingTopology,1,phase,1),
          "warm preflight can refresh a live structural plan");
    check(!runtime.failure().valid,"successful preflight clears previous failure snapshot");
    check(runtime.prepare(&request,1,phase,1)==nullptr &&
          runtime.status().last==FlatProjectionRuntimeRefusal::PlanFailure,
          "prepare requires a newly preflighted phase after topology retarget");
    check(runtime.failure().valid && runtime.failure().inPrepare &&
          std::strcmp(runtime.failure().branch,"prepare-no-exact-plan")==0,
          "prepare failure distinguishes phase-plan timing from absent source bytes");
    {FlatProjectionBindingScope stale(*plan);
     check(!stale.active(),"failed prepare invalidates old plan token");}
    check(runtime.preflight(&request,1,phase,1,false) &&
          runtime.prepare(&request,1,phase,1)!=nullptr,
          "live retarget recovers old topology without allocating");
    runtime.invalidate(original.Get());
    runtime.enableColdReadback(true);
    const auto queuedAfterInvalidation=runtime.status().coldQueued;
    check(!runtime.preflight(&request,1,phase,1,false) &&
          runtime.status().last==FlatProjectionRuntimeRefusal::MissingFullWrite &&
          runtime.status().coldQueued==queuedAfterInvalidation,
          "live preflight does not queue a cold readback for missing source bytes");
    {const auto failure=runtime.failure();
     check(failure.valid && std::strcmp(failure.branch,"shadow-missing-full-write")==0 &&
           failure.requestIndex==0 && failure.stage==FlatProjectionStage::Vertex &&
           failure.slot==1 && failure.firstConstant==0 && failure.constantCount==4096 &&
           failure.trackedGeneration!=0 && !failure.shadowPresent && !failure.pending &&
           failure.patchCount==1 && !failure.inPrepare,
           "missing full write snapshot identifies exact tracked source and cold state");}
    runtime.enableColdReadback(false);
    check(!runtime.copyConstants(original.Get(),0,sizeof(copied),copied) &&
          copied[0]==123,"invalidated shadow cannot establish diagnostic camera identity");
    check(runtime.prepare(&request,1,phase,1)==nullptr &&
          runtime.status().last==FlatProjectionRuntimeRefusal::MissingFullWrite,
          "copy or unknown write invalidates private preparation");
    raw[0]=2;
    runtime.observeUpdate(original.Get(),raw,nullptr);
    check(runtime.preflight(&request,1,phase,1,false) && runtime.prepare(&request,1,phase,1)!=nullptr,
          "late full update restores provenance in no-allocation mode");
    check(!runtime.failure().valid,"successful prepare clears stale missing-write snapshot");
    UINT rangedFirst=16,rangedCount=16;
    ctx->VSSetConstantBuffers1(1,1,&bound,&rangedFirst,&rangedCount);
    check(runtime.prepare(&request,1,phase,1)==nullptr &&
          runtime.status().last==FlatProjectionRuntimeRefusal::UnsupportedRange,
          "unqualified Context1 range is detected by getter and refused");
    {const auto failure=runtime.failure();
     check(failure.valid && std::strcmp(failure.branch,"binding-mismatch")==0 &&
           failure.actualBuffer==original.Get() && failure.actualFirst==16 &&
           failure.actualCount==16 && failure.shadowPresent,
           "binding refusal captures observed range without losing source shadow state");}
    ctx->VSSetConstantBuffers1(1,1,&bound,&first,&count);
    runtime.invalidateAll();
    check(runtime.prepare(&request,1,phase,1)==nullptr,"unknown command list invalidates all shadows");
    check(runtime.status().initialWrites==1 && runtime.status().fullWrites==2 &&
          runtime.status().invalidations>=2,"readiness counters distinguish writes from invalidation");
    uint32_t lightingWords[120]{};
    lightingWords[0]=960;lightingWords[1]=540;
    lightingWords[4]=8;lightingWords[5]=5;lightingWords[6]=32;lightingWords[7]=120;
    float lightingRay[3][4]={{1,0,0,2},{0,1,0,3},{4,5,6,7}};
    std::memcpy(lightingWords+40,lightingRay,sizeof(lightingRay));
    D3D11_BUFFER_DESC lightDesc{};lightDesc.ByteWidth=sizeof(lightingWords);
    lightDesc.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
    D3D11_SUBRESOURCE_DATA lightInitial{};lightInitial.pSysMem=lightingWords;
    ComPtr<ID3D11Buffer> light;
    check(SUCCEEDED(device->CreateBuffer(&lightDesc,&lightInitial,light.GetAddressOf())),"lighting source CB");
    if(light){
        check(runtime.observeCreateBuffer(light.Get(),lightingWords),"lighting initial full write");
        auto* lightBound=light.Get();ctx->CSSetConstantBuffers1(0,1,&lightBound,&first,&count);
        FlatProjectionRuntimeRequest lit{};lit.stage=FlatProjectionStage::Compute;
        lit.slot=0;lit.original=light.Get();lit.patchCount=1;
        lit.patches[0]={FlatProjectionPatchLayout::LightingUvRay,10u*16u,
            {.25f,-.25f,960,540,120,8,5,1}};
        check(runtime.preflight(&lit,1,phase,1) && runtime.prepare(&lit,1,phase,1)!=nullptr,
              "lighting contract agrees with actual UINT image/grid/tile CB rows");
        bool lightingPhasesReady=true;
        const auto lightingZeroBefore=runtime.status().zeroPhaseReady;
        for(uint32_t i=0;i<64;++i){
            const float pixelX=float(int(i%8)-4)/16.0f;
            const float pixelY=float(int((i/8)%8)-4)/16.0f;
            FlatProjectionJitter current{};
            lit.patches[0].lighting.pixelX=pixelX;
            lit.patches[0].lighting.pixelY=pixelY;
            lightingPhasesReady=flatProjectionJitter(pixelX,pixelY,960,540,current) &&
                runtime.preflight(&lit,1,current,200+i,false);
            const auto* currentPlan=lightingPhasesReady ? runtime.prepare(&lit,1,current,200+i) : nullptr;
            const bool zeroPhase=pixelX==0.0f && pixelY==0.0f;
            if(!lightingPhasesReady || (zeroPhase ? currentPlan!=nullptr : currentPlan==nullptr)){
                lightingPhasesReady=false;break;
            }
            if(zeroPhase) continue;
            FlatProjectionBindingScope scope(*currentPlan);
            if(!scope.active()){lightingPhasesReady=false;break;}
        }
        check(lightingPhasesReady && runtime.status().zeroPhaseReady==lightingZeroBefore+1,
              "lighting phase metadata retargets structural topology across 64 phases");
        lit.patches[0].lighting.pixelX=.25f;
        lit.patches[0].lighting.pixelY=-.25f;
        lightingWords[7]=128;runtime.observeUpdate(light.Get(),lightingWords,nullptr);
        check(!runtime.preflight(&lit,1,phase,1) &&
              runtime.status().last==FlatProjectionRuntimeRefusal::InvalidRecipe,
              "lighting tile mismatch refuses despite caller metadata");
        lightingWords[7]=120;runtime.observeUpdate(light.Get(),lightingWords,nullptr);
        check(runtime.preflight(&lit,1,phase,1),"lighting metadata recovery");
    }
    D3D11_BUFFER_DESC dynamicDesc{};dynamicDesc.ByteWidth=sizeof(raw);
    dynamicDesc.BindFlags=D3D11_BIND_CONSTANT_BUFFER;dynamicDesc.Usage=D3D11_USAGE_DYNAMIC;
    dynamicDesc.CPUAccessFlags=D3D11_CPU_ACCESS_WRITE;
    ComPtr<ID3D11Buffer> dynamic;
    check(SUCCEEDED(device->CreateBuffer(&dynamicDesc,nullptr,dynamic.GetAddressOf())),"mapped source CB");
    if(dynamic){
        D3D11_MAPPED_SUBRESOURCE mapped{};
        const HRESULT hr=ctx->Map(dynamic.Get(),0,D3D11_MAP_WRITE_DISCARD,0,&mapped);
        check(SUCCEEDED(hr),"mapped source CPU write");
        if(SUCCEEDED(hr)){
            runtime.observeMap(dynamic.Get(),D3D11_MAP_WRITE_DISCARD,mapped.pData);
            check(!runtime.copyConstants(dynamic.Get(),0,sizeof(copied),copied),
                  "mapped transaction cannot publish diagnostic bytes");
            std::memcpy(mapped.pData,raw,sizeof(raw));
            runtime.observeUnmap(dynamic.Get()); // before real Unmap
            ctx->Unmap(dynamic.Get(),0);
            check(runtime.copyConstants(dynamic.Get(),0,sizeof(copied),copied) &&
                  std::memcmp(copied,raw,sizeof(copied))==0,
                  "completed full map publishes raw diagnostic bytes");
            FlatProjectionRuntimeRequest mappedReq=request;
            mappedReq.original=dynamic.Get();mappedReq.slot=2;
            auto* mappedBound=dynamic.Get();ctx->VSSetConstantBuffers1(2,1,&mappedBound,&first,&count);
            check(runtime.preflight(&mappedReq,1,zero,0) &&
                  runtime.prepare(&mappedReq,1,zero,0)==nullptr,
                  "Map/Unmap full shadow qualifies zero-phase audit");
        }
    }
    ComPtr<ID3D11Buffer> cold, stale;
    check(SUCCEEDED(device->CreateBuffer(&desc,&initial,cold.GetAddressOf())) &&
          SUCCEEDED(device->CreateBuffer(&desc,&initial,stale.GetAddressOf())),
          "preexisting static CB fixtures");
    if(cold && stale){
        FlatProjectionRuntimeRequest coldReq=request;
        coldReq.original=cold.Get();coldReq.slot=3;
        auto* coldBound=cold.Get();ctx->VSSetConstantBuffers1(3,1,&coldBound,&first,&count);
        check(!runtime.preflight(&coldReq,1,zero,0) &&
              runtime.status().last==FlatProjectionRuntimeRefusal::MissingFullWrite &&
              runtime.status().coldQueued==0,
              "cold readback disabled by default, missing bytes explicit");
        runtime.enableColdReadback(true);
        check(!runtime.preflight(&coldReq,1,zero,0) &&
              runtime.status().coldQueued==1 && runtime.status().coldPending==1,
              "exact admitted missing buffer queues one bounded readback");
        {const auto failure=runtime.failure();
         check(failure.valid && failure.tracked && failure.pending && !failure.shadowPresent &&
               !failure.promoted && failure.buffer==cold.Get() && failure.trackedWidth==sizeof(raw) &&
               std::strcmp(failure.branch,"shadow-missing-full-write")==0,
               "cold missing write reports pending readback before first promotion");}
        ctx->Flush(); // test-only progress; production queue/poll never flushes
        for(unsigned i=0;i<120 && !runtime.status().coldCompleted;++i){
            runtime.pollColdReadbacks();Sleep(1);
        }
        check(runtime.status().coldCompleted==1 && runtime.status().coldPending==0 &&
              runtime.preflight(&coldReq,1,zero,0) &&
              runtime.prepare(&coldReq,1,zero,0)==nullptr,
              "stable cold CB publishes complete shadow for later preparation");
        check(!runtime.failure().valid,"successful cold preparation clears pending failure context");
        FlatProjectionRuntimeRequest staleReq=request;
        staleReq.original=stale.Get();staleReq.slot=4;
        auto* staleBound=stale.Get();ctx->VSSetConstantBuffers1(4,1,&staleBound,&first,&count);
        check(!runtime.preflight(&staleReq,1,zero,0) && runtime.status().coldQueued==2,
              "second exact missing buffer queued");
        runtime.invalidate(stale.Get()); // unknown intervening write, no CPU bytes
        ctx->Flush();
        for(unsigned i=0;i<120 && !runtime.status().coldStale;++i){
            runtime.pollColdReadbacks();Sleep(1);
        }
        runtime.enableColdReadback(false);
        check(runtime.status().coldStale==1 && runtime.status().coldPending==0 &&
              !runtime.preflight(&staleReq,1,zero,0) &&
              runtime.status().last==FlatProjectionRuntimeRefusal::MissingFullWrite,
               "intervening write cannot publish stale GPU snapshot");
    }
    // Reproduce the captured late VS b1 contract: a 5,376-byte source was
    // already tracked and privately promoted, but its second ForwardColumns
    // patch at byte 720 had not appeared during warm-up.
    float lateRaw[1344]{};
    for(unsigned baseOffset : {656u/4u,720u/4u})
        for(unsigned diagonal : {0u,5u,10u,15u}) lateRaw[baseOffset+diagonal]=1.0f;
    D3D11_BUFFER_DESC lateDesc=desc;lateDesc.ByteWidth=sizeof(lateRaw);
    D3D11_SUBRESOURCE_DATA lateInit{};lateInit.pSysMem=lateRaw;
    ComPtr<ID3D11Buffer> late, unready;
    check(SUCCEEDED(device->CreateBuffer(&lateDesc,&lateInit,late.GetAddressOf())) &&
          SUCCEEDED(device->CreateBuffer(&lateDesc,&lateInit,unready.GetAddressOf())),
          "late topology source buffers created");
    if(late && unready){
        FlatProjectionRuntime tail;
        check(tail.initialize(base) && tail.observeCreateBuffer(late.Get(),lateRaw),
              "late topology source has complete tracked shadow");
        auto* lateBound=late.Get();ctx->VSSetConstantBuffers1(1,1,&lateBound,&first,&count);
        FlatProjectionRuntimeRequest warm=request;
        warm.original=late.Get();warm.patches[0].byteOffset=656;
        check(tail.preflight(&warm,1,phase,1) && tail.prepare(&warm,1,phase,1)!=nullptr,
              "late source has a warm private buffer and capability-tested plan");
        const auto* oldPlan=tail.prepare(&warm,1,phase,1);
        FlatProjectionRuntimeRequest two=warm;
        two.patchCount=2;two.patches[1]={FlatProjectionPatchLayout::ForwardColumns,720,{}};
        const auto queued=tail.status().coldQueued;
        const auto prepared=tail.status().prepared;
        check(tail.preflight(&two,1,phase,2,false) && tail.prepare(&two,1,phase,2)!=nullptr &&
              tail.status().coldQueued==queued && tail.status().prepared==prepared+1 &&
              tail.status().livePlanRetargets==1,
              "first-seen live two-patch topology reuses prepared source without cold work");
        {FlatProjectionBindingScope staleScope(*oldPlan);
         check(!staleScope.active(),"old plan token cannot bind after shared private buffer retarget");}
        const auto* livePlan=tail.prepare(&two,1,phase,2);
        if(livePlan){
            FlatProjectionBindingScope active(*livePlan);
            check(active.active(),"late live plan binds the existing private buffer");
            FlatProjectionRuntimeRequest another=two;another.slot=2;
            ctx->VSSetConstantBuffers1(2,1,&lateBound,&first,&count);
            check(!tail.preflight(&another,1,phase,3,false) &&
                  std::strcmp(tail.failure().branch,"topology-live-plan-in-use")==0,
                  "live plan cannot retarget while its prior scope is active");
        }
        FlatProjectionRuntimeRequest unknown=two;unknown.original=unready.Get();
        auto* unreadyBound=unready.Get();ctx->VSSetConstantBuffers1(1,1,&unreadyBound,&first,&count);
        check(!tail.preflight(&unknown,1,phase,3,false) &&
              std::strcmp(tail.failure().branch,"untracked-live-buffer")==0,
              "untracked live source is still refused");
        check(tail.observeCreateBuffer(unready.Get(),lateRaw) &&
              !tail.preflight(&unknown,1,phase,3,false) &&
              std::strcmp(tail.failure().branch,"private-first-seen-live")==0,
              "complete shadow does not authorize live private-buffer creation");
        tail.invalidate(unready.Get());
        check(!tail.preflight(&unknown,1,phase,3,false) &&
              std::strcmp(tail.failure().branch,"shadow-missing-full-write")==0,
              "invalidated shadow is refused before live plan retarget");
        ctx->VSSetConstantBuffers1(1,1,&lateBound,&first,&count);
        check(tail.preflight(&two,1,phase,2,false),"ready live topology recovers after refusals");
        UINT badFirst=16,badCount=16;
        ctx->VSSetConstantBuffers1(1,1,&lateBound,&badFirst,&badCount);
        check(tail.prepare(&two,1,phase,2)==nullptr &&
              tail.status().last==FlatProjectionRuntimeRefusal::UnsupportedRange &&
              std::strcmp(tail.failure().branch,"binding-mismatch")==0,
              "live plan still checks actual Context1 range before binding");
        ctx->VSSetConstantBuffers1(1,1,&lateBound,&first,&count);
    }
    runtime.reset();
}
