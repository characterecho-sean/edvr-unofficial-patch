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
    runtime.invalidate(original.Get());
    check(!runtime.copyConstants(original.Get(),0,sizeof(copied),copied) &&
          copied[0]==123,"invalidated shadow cannot establish diagnostic camera identity");
    check(runtime.prepare(&request,1,phase,1)==nullptr &&
          runtime.status().last==FlatProjectionRuntimeRefusal::MissingFullWrite,
          "copy or unknown write invalidates private preparation");
    raw[0]=2;
    runtime.observeUpdate(original.Get(),raw,nullptr);
    check(runtime.preflight(&request,1,phase,1) && runtime.prepare(&request,1,phase,1)!=nullptr,
          "full update restores provenance without allocating another plan");
    UINT rangedFirst=16,rangedCount=16;
    ctx->VSSetConstantBuffers1(1,1,&bound,&rangedFirst,&rangedCount);
    check(runtime.prepare(&request,1,phase,1)==nullptr &&
          runtime.status().last==FlatProjectionRuntimeRefusal::UnsupportedRange,
          "unqualified Context1 range is detected by getter and refused");
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
        ctx->Flush(); // test-only progress; production queue/poll never flushes
        for(unsigned i=0;i<120 && !runtime.status().coldCompleted;++i){
            runtime.pollColdReadbacks();Sleep(1);
        }
        check(runtime.status().coldCompleted==1 && runtime.status().coldPending==0 &&
              runtime.preflight(&coldReq,1,zero,0) &&
              runtime.prepare(&coldReq,1,zero,0)==nullptr,
              "stable cold CB publishes complete shadow for later preparation");
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
    runtime.reset();
}
