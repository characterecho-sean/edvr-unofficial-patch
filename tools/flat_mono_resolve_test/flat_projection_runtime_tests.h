#pragma once
#include "../../src/d3d11/flat_projection_runtime.h"
#include "../../src/d3d11/flat_map_bounce.h"

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
        struct BounceDriver {
            void retain(uintptr_t r, uintptr_t c) {
                reinterpret_cast<ID3D11Resource*>(r)->AddRef();
                reinterpret_cast<ID3D11DeviceContext*>(c)->AddRef();
            }
            void release(uintptr_t r, uintptr_t c) {
                reinterpret_cast<ID3D11DeviceContext*>(c)->Release();
                reinterpret_cast<ID3D11Resource*>(r)->Release();
            }
            uint64_t clockTicks() { LARGE_INTEGER t{}; QueryPerformanceCounter(&t); return uint64_t(t.QuadPart); }
            uint64_t ticksPerSecond() { LARGE_INTEGER t{}; QueryPerformanceFrequency(&t); return uint64_t(t.QuadPart); }
            bool verify(void* real, const void* cached, size_t n) { return std::memcmp(real,cached,n)==0; }
        } bounceDriver;
        auto bounce=std::make_unique<flatmap::Runtime<BounceDriver>>(bounceDriver);
        bounce->setMode(flatmap::Mode::On);
        bounce->preMap(reinterpret_cast<uintptr_t>(dynamic.Get()));
        D3D11_MAPPED_SUBRESOURCE mapped{};
        const HRESULT hr=ctx->Map(dynamic.Get(),0,D3D11_MAP_WRITE_DISCARD,0,&mapped);
        check(SUCCEEDED(hr),"mapped source CPU write");
        if(SUCCEEDED(hr)){
            const auto source=runtime.bounceSource(dynamic.Get());
            check(source.eligible && source.width==sizeof(raw),
                  "tracked dynamic WARP CB qualifies before map invalidation");
            flatmap::MapRequest bounceRequest{};
            bounceRequest.resource=reinterpret_cast<uintptr_t>(dynamic.Get());
            bounceRequest.context=reinterpret_cast<uintptr_t>(ctx.Get());
            bounceRequest.real=mapped.pData;bounceRequest.width=source.width;bounceRequest.frame=1;
            bounceRequest.discard=bounceRequest.eligible=bounceRequest.success=true;
            bounceRequest.seed=source.seed;bounceRequest.seedValid=source.seedValid;
            mapped.pData=bounce->install(bounceRequest);
            check(mapped.pData!=bounceRequest.real && reinterpret_cast<uintptr_t>(mapped.pData)%64==0,
                  "WARP map bounce gives the observer an aligned cached pointer");
            runtime.observeMap(dynamic.Get(),D3D11_MAP_WRITE_DISCARD,mapped.pData);
            check(!runtime.copyConstants(dynamic.Get(),0,sizeof(copied),copied),
                  "mapped transaction cannot publish diagnostic bytes");
            std::memcpy(mapped.pData,raw,sizeof(raw));
            auto bounceLease=bounce->beginUnmap(bounceRequest.resource,bounceRequest.context);
            check(bounceLease.bounced() &&
                  std::memcmp(bounceRequest.real,raw,sizeof(raw))==0,
                  "bounce flushes game bytes to real WARP mapping before observers");
            runtime.observeUnmap(dynamic.Get()); // before real Unmap
            bounceLease.finish();
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
    // Section 72 step 3 (review finding 3): the 32-plan cache retires instead
    // of refusing at 33 -- stale plans release first, then the least-recently
    // used idle plan, and a retired plan's buffers demote back to evictable.
    {
        FlatProjectionRuntime capacity;
        check(capacity.initialize(base), "capacity runtime initialized");
        constexpr UINT kTopologies = 40;
        ComPtr<ID3D11Buffer> buffers[kTopologies]{};
        unsigned char blob[1024]; std::memset(blob, 7, sizeof(blob));
        FlatProjectionJitter pz{};
        check(flatProjectionJitter(.25f, -.25f, 960, 540, pz), "capacity phase conversion");
        bool allPreflighted = true;
        for (UINT i = 0; i < kTopologies; ++i) {
            D3D11_BUFFER_DESC bd{}; bd.ByteWidth = sizeof(blob); bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            D3D11_SUBRESOURCE_DATA bi{}; bi.pSysMem = blob;
            if (FAILED(device->CreateBuffer(&bd, &bi, buffers[i].GetAddressOf()))) { allPreflighted = false; break; }
            capacity.observeCreateBuffer(buffers[i].Get(), blob);
            FlatProjectionRuntimeRequest rq{};
            rq.stage = FlatProjectionStage::Vertex; rq.slot = 1; rq.original = buffers[i].Get();
            rq.firstConstant = 0; rq.constantCount = sizeof(blob) / 16; rq.patchCount = 1;
            rq.patches[0] = {FlatProjectionPatchLayout::ForwardColumns, 0, {}};
            if (!capacity.preflight(&rq, 1, pz, i + 1)) { allPreflighted = false; break; }
        }
        check(allPreflighted && capacity.status().preflights == kTopologies &&
              capacity.status().planRetiredLru == kTopologies - 32,
              "the plan cache retires the least-recently-used idle plan past 32 instead of refusing");
        // Invalidate a NEW plan's shadow: stale retirement must choose it
        // over any LRU victim (buffers[0]'s plan was LRU-retired long ago).
        capacity.invalidate(buffers[kTopologies - 2].Get());
        {
            D3D11_BUFFER_DESC bd{}; bd.ByteWidth = sizeof(blob); bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            D3D11_SUBRESOURCE_DATA bi{}; bi.pSysMem = blob;
            ComPtr<ID3D11Buffer> extra;
            check(SUCCEEDED(device->CreateBuffer(&bd, &bi, extra.GetAddressOf())), "stale-case CB created");
            capacity.observeCreateBuffer(extra.Get(), blob);
            FlatProjectionRuntimeRequest rq{};
            rq.stage = FlatProjectionStage::Vertex; rq.slot = 1; rq.original = extra.Get();
            rq.firstConstant = 0; rq.constantCount = sizeof(blob) / 16; rq.patchCount = 1;
            rq.patches[0] = {FlatProjectionPatchLayout::ForwardColumns, 0, {}};
            check(capacity.preflight(&rq, 1, pz, kTopologies + 1) &&
                  capacity.status().planRetiredStale >= 1,
                  "a plan whose shadow is invalidated retires stale before any LRU eviction");
        }
        UINT first = 0, count = sizeof(blob) / 16; auto* lastBound = buffers[kTopologies - 1].Get();
        ctx->VSSetConstantBuffers1(1, 1, &lastBound, &first, &count);
        FlatProjectionRuntimeRequest last{};
        last.stage = FlatProjectionStage::Vertex; last.slot = 1; last.original = buffers[kTopologies - 1].Get();
        last.firstConstant = 0; last.constantCount = sizeof(blob) / 16; last.patchCount = 1;
        last.patches[0] = {FlatProjectionPatchLayout::ForwardColumns, 0, {}};
        const auto* lastPlan = capacity.prepare(&last, 1, pz, kTopologies);
        check(lastPlan != nullptr, "a surviving plan still prepares after retirements");
        if (lastPlan) { FlatProjectionBindingScope scope(*lastPlan); check(scope.active(), "surviving plan binds"); }
        capacity.reset();
    }
    // Gate-2 review reproductions: ownership is transactional. Live retargets
    // balance references exactly once, a failed preflight pins nothing, and
    // buffer pressure retires before refusing admission.
    {
        FlatProjectionRuntime refs;
        check(refs.initialize(base), "refs runtime initialized");
        unsigned char blob[1024]; std::memset(blob, 7, sizeof(blob));
        auto mk = [&](FlatProjectionRuntime& rt, ComPtr<ID3D11Buffer>& out) {
            D3D11_BUFFER_DESC bd{}; bd.ByteWidth = sizeof(blob); bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            D3D11_SUBRESOURCE_DATA bi{}; bi.pSysMem = blob;
            check(SUCCEEDED(device->CreateBuffer(&bd, &bi, out.GetAddressOf())), "refs CB created");
            rt.observeCreateBuffer(out.Get(), blob);
        };
        auto rqOf = [](ID3D11Buffer* b, UINT constants) {
            FlatProjectionRuntimeRequest rq{};
            rq.stage = FlatProjectionStage::Vertex; rq.slot = 1; rq.original = b;
            rq.firstConstant = 0; rq.constantCount = constants; rq.patchCount = 1;
            rq.patches[0] = {FlatProjectionPatchLayout::ForwardColumns, 0, {}};
            return rq;
        };
        FlatProjectionJitter pz{};
        check(flatProjectionJitter(.25f, -.25f, 960, 540, pz), "refs phase conversion");
        ComPtr<ID3D11Buffer> warm, live1, live2;
        mk(refs, warm); mk(refs, live1); mk(refs, live2);
        auto warmA = rqOf(warm.Get(), sizeof(blob) / 16);
        check(refs.preflight(&warmA, 1, pz, 1) && refs.status().planRefsLive == 1,
              "one warm plan holds exactly one reference");
        auto warmB = rqOf(live1.Get(), sizeof(blob) / 16);
        check(refs.preflight(&warmB, 1, pz, 2) && refs.status().planRefsLive == 2,
              "a second warm plan holds its own reference");
        // The live retarget path wants an already privately-ready buffer with
        // a first-seen topology: same buffer, new structure.
        auto liveReq1 = rqOf(live1.Get(), sizeof(blob) / 16);
        liveReq1.patchCount = 2;
        liveReq1.patches[1] = {FlatProjectionPatchLayout::ForwardColumns, 64, {}};
        check(refs.preflight(&liveReq1, 1, pz, 3, false) && refs.status().planRefsLive == 3,
              "the first live retarget records its references exactly once");
        auto liveReq2 = rqOf(live1.Get(), sizeof(blob) / 16);
        liveReq2.patchCount = 2;
        liveReq2.patches[1] = {FlatProjectionPatchLayout::ForwardColumns, 128, {}};
        check(refs.preflight(&liveReq2, 1, pz, 4, false) && refs.status().planRefsLive == 3,
              "repeated live retargets abandon no references");
        FlatProjectionRuntimeRequest two[2]{rqOf(live1.Get(), sizeof(blob) / 16), rqOf(live2.Get(), 0)};
        const auto beforeFail = refs.status().planRefsLive;
        check(!refs.preflight(two, 2, pz, 4) && refs.status().planRefsLive == beforeFail,
              "a failed multi-buffer preflight pins no ownerless buffer");
        refs.reset();
    }
    {
        FlatProjectionRuntime pressure;
        check(pressure.initialize(base), "pressure runtime initialized");
        unsigned char blob[1024]; std::memset(blob, 7, sizeof(blob));
        ComPtr<ID3D11Buffer> pins[64]{};
        auto mk = [&](ComPtr<ID3D11Buffer>& out) {
            D3D11_BUFFER_DESC bd{}; bd.ByteWidth = sizeof(blob); bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            D3D11_SUBRESOURCE_DATA bi{}; bi.pSysMem = blob;
            check(SUCCEEDED(device->CreateBuffer(&bd, &bi, out.GetAddressOf())), "pressure CB created");
            pressure.observeCreateBuffer(out.Get(), blob);
        };
        FlatProjectionJitter pz{};
        check(flatProjectionJitter(.25f, -.25f, 960, 540, pz), "pressure phase conversion");
        for (UINT i = 0; i < 64; ++i) {
            mk(pins[i]);
            FlatProjectionRuntimeRequest two[2]{};
            two[0].stage = FlatProjectionStage::Vertex; two[0].slot = 1; two[0].original = pins[i].Get();
            two[0].firstConstant = 0; two[0].constantCount = sizeof(blob) / 16; two[0].patchCount = 1;
            two[0].patches[0] = {FlatProjectionPatchLayout::ForwardColumns, 0, {}};
            // The second request is invalid on its face; before staged
            // promotion the first buffer stayed promoted ownerless.
            if (pressure.preflight(two, 2, pz, i + 1)) { check(false, "invalid second request must refuse"); break; }
        }
        ComPtr<ID3D11Buffer> sixtyFive;
        mk(sixtyFive);
        FlatProjectionRuntimeRequest rq65{};
        rq65.stage = FlatProjectionStage::Vertex; rq65.slot = 1; rq65.original = sixtyFive.Get();
        rq65.firstConstant = 0; rq65.constantCount = sizeof(blob) / 16; rq65.patchCount = 1;
        rq65.patches[0] = {FlatProjectionPatchLayout::ForwardColumns, 0, {}};
        check(pressure.preflight(&rq65, 1, pz, 65),
              "the 65th buffer tracks after 64 failed preflights (no ownerless pins)");
        pressure.reset();
    }
    // Gate-2 review F2: a buffer referenced by two plans stays promoted until
    // BOTH retire, so single-shot retirement frees nothing. With the whole
    // bank shared pairwise across 32 four-binding plans, the 65th buffer must
    // still track: the bounded loop retires plans until a buffer unpins.
    {
        FlatProjectionRuntime shared;
        check(shared.initialize(base), "shared-refs runtime initialized");
        unsigned char blob[1024]; std::memset(blob, 7, sizeof(blob));
        FlatProjectionJitter pz{};
        check(flatProjectionJitter(.25f, -.25f, 960, 540, pz), "shared-refs phase conversion");
        ComPtr<ID3D11Buffer> pins[64]{};
        for (UINT i = 0; i < 64; ++i) {
            D3D11_BUFFER_DESC bd{}; bd.ByteWidth = sizeof(blob); bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            D3D11_SUBRESOURCE_DATA bi{}; bi.pSysMem = blob;
            check(SUCCEEDED(device->CreateBuffer(&bd, &bi, pins[i].GetAddressOf())), "shared-refs CB created");
            shared.observeCreateBuffer(pins[i].Get(), blob);
        }
        bool plansReady = true;
        for (UINT plan = 0; plan < 32 && plansReady; ++plan) {
            // Plans 0-15 give buffers 0-63 their first reference, plans 16-31
            // their second: every buffer is pinned by exactly two plans. The
            // second reference binds different slots, because the buffer
            // identity and slot are both part of a plan's topology -- same
            // slots would re-preflight the first-reference plan instead of
            // filling the bank.
            FlatProjectionRuntimeRequest four[4]{};
            for (UINT j = 0; j < 4; ++j) {
                four[j].stage = FlatProjectionStage::Vertex;
                four[j].slot = 1 + (plan / 16) * 4 + j;
                four[j].original = pins[(plan % 16) * 4 + j].Get();
                four[j].firstConstant = 0; four[j].constantCount = sizeof(blob) / 16;
                four[j].patchCount = 1;
                four[j].patches[0] = {FlatProjectionPatchLayout::ForwardColumns, 0, {}};
            }
            plansReady = shared.preflight(four, 4, pz, plan + 1);
        }
        check(plansReady && shared.status().planRefsLive == 128,
              "32 four-binding plans pin all 64 buffers exactly twice");
        ComPtr<ID3D11Buffer> sixtyFive;
        {
            D3D11_BUFFER_DESC bd{}; bd.ByteWidth = sizeof(blob); bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            D3D11_SUBRESOURCE_DATA bi{}; bi.pSysMem = blob;
            check(SUCCEEDED(device->CreateBuffer(&bd, &bi, sixtyFive.GetAddressOf())), "shared-refs 65th CB created");
        }
        check(shared.observeCreateBuffer(sixtyFive.Get(), blob) &&
              shared.status().planRetiredLru >= 2,
              "the 65th buffer tracks while every slot is pinned twice (bounded retirement loop)");
        FlatProjectionRuntimeRequest rq65{};
        rq65.stage = FlatProjectionStage::Vertex; rq65.slot = 1; rq65.original = sixtyFive.Get();
        rq65.firstConstant = 0; rq65.constantCount = sizeof(blob) / 16; rq65.patchCount = 1;
        rq65.patches[0] = {FlatProjectionPatchLayout::ForwardColumns, 0, {}};
        check(shared.preflight(&rq65, 1, pz, 65),
              "a plan for the 65th buffer preflights after the retirements");
        shared.reset();
    }
}
