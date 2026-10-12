#pragma once
#include "../../src/d3d11/flat_projection_scope.h"
#include "../../src/d3d11/flat_compute_readback.h"
#include <d3dcompiler.h>

void projectionScopeTests(ID3D11Device* device, ID3D11DeviceContext* base) {
    using namespace edvr;
    ComPtr<ID3D11DeviceContext1> ctx;
    check(SUCCEEDED(base->QueryInterface(IID_PPV_ARGS(ctx.GetAddressOf()))), "projection context1 available");
    if (!ctx) return;
    float raw[256]{}; raw[64]=2; raw[65]=3; raw[66]=4; raw[67]=5;
    auto make=[&](const float* values) {
        D3D11_BUFFER_DESC desc{}; desc.ByteWidth=sizeof(raw); desc.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
        D3D11_SUBRESOURCE_DATA data{}; data.pSysMem=values; ComPtr<ID3D11Buffer> result;
        check(SUCCEEDED(device->CreateBuffer(&desc,&data,result.GetAddressOf())), "projection buffer allocation"); return result;
    };
    auto original=make(raw);
    if (!original) return;
    FlatProjectionShadowBank<2,1024> shadows;
    check(shadows.registerBuffer(original.Get(),1,sizeof(raw)) &&
        shadows.captureFullWrite(original.Get(),1,raw,sizeof(raw)),"full raw projection shadow");
    FlatProjectionShadowView snapshot{};
    check(shadows.lookup(original.Get(),1,snapshot),"current full projection snapshot");
    FlatProjectionPatchRequest patch{FlatProjectionPatchLayout::ForwardColumns,256,{}};
    FlatProjectionJitter jitter{};
    check(flatProjectionJitter(1,0,16,16,jitter),"projection phase");
    FlatProjectionPrivateBuffer privateBuffer;
    check(privateBuffer.initialize(ctx.Get(),original.Get(),1),"projection preflight allocation");
    check(privateBuffer.prepare(shadows,&patch,1,jitter,1) && privateBuffer.uploads()==1,"private projection upload");
    check(privateBuffer.prepare(shadows,&patch,1,jitter,1) && privateBuffer.uploads()==1,"same provenance and phase reuses private upload");
    check(shadows.invalidate(original.Get(),1),"source invalidation");
    check(!privateBuffer.prepare(shadows,&patch,1,jitter,1) &&
        privateBuffer.binding(FlatProjectionStage::Vertex,1).replacement==nullptr,"bad provenance hides stale private buffer");
    check(shadows.captureFullWrite(original.Get(),1,raw,sizeof(raw)),"source full write restores provenance");
    check(privateBuffer.prepare(shadows,&patch,1,jitter,1) && privateBuffer.uploads()==2,"valid recovery refreshes private upload");
    auto badPatch=patch;badPatch.byteOffset=sizeof(raw)-16;
    check(!privateBuffer.prepare(shadows,&badPatch,1,jitter,1) &&
        privateBuffer.binding(FlatProjectionStage::Vertex,1).replacement==nullptr,"invalid span hides stale private buffer");
    check(privateBuffer.prepare(shadows,&patch,1,jitter,1),"recovery from refused span");
    const auto priorUploads=privateBuffer.uploads();
    check(privateBuffer.prepare(shadows,&patch,1,jitter,2) && privateBuffer.uploads()==priorUploads+1,"phase change invalidates upload key");
    check(raw[64]==2 && reinterpret_cast<const float*>(snapshot.bytes)[64]==2,"raw camera and engine snapshot remains unmodified");
    auto* replacement=privateBuffer.binding(FlatProjectionStage::Vertex,1).replacement;
    if(!replacement)return;
    float certificate[256]{};
    check(privateBuffer.copyUploadedConstants(replacement,0,sizeof(certificate),certificate),"ready private upload offers exact complete certificate");
    certificate[0]=713;
    check(!privateBuffer.copyUploadedConstants(original.Get(),0,4,certificate) && certificate[0]==713,"wrong resource cannot borrow private upload certificate");
    UINT first=16, count=16; auto* buffer=original.Get();
    ctx->VSSetConstantBuffers1(1,1,&buffer,&first,&count);
    ctx->PSSetConstantBuffers1(2,1,&buffer,&first,&count);
    ctx->CSSetConstantBuffers1(0,1,&buffer,&first,&count);
    FlatPrivateProjectionBinding bindings[]={privateBuffer.binding(FlatProjectionStage::Vertex,1,first,count),
        privateBuffer.binding(FlatProjectionStage::Pixel,2,first,count),
        privateBuffer.binding(FlatProjectionStage::Compute,0,first,count)};
    FlatProjectionBindingPlan plan;
    check(plan.initialize(ctx.Get(),bindings,3),"projection binding preflight");
    float uploaded=0;
    check(plan.copyUploadedConstants(FlatProjectionStage::Vertex,1,256,4,&uploaded) && uploaded==2.625f,
        "plan certificate copies the uploaded private revision");
    uploaded=713;
    check(!plan.copyUploadedConstants(FlatProjectionStage::Vertex,3,256,4,&uploaded) && uploaded==713 &&
        !plan.copyUploadedConstants(FlatProjectionStage::Vertex,1,sizeof(raw)-1,4,&uploaded),
        "plan certificate rejects absent stage-slot and out-of-bounds slice");
    auto checkBindings=[&](ID3D11Buffer* expected) {
        for (unsigned stage=0;stage<3;++stage) {
            ComPtr<ID3D11Buffer> observed;UINT offset=0,length=0;
            if(stage==0)ctx->VSGetConstantBuffers1(1,1,observed.GetAddressOf(),&offset,&length);
            if(stage==1)ctx->PSGetConstantBuffers1(2,1,observed.GetAddressOf(),&offset,&length);
            if(stage==2)ctx->CSGetConstantBuffers1(0,1,observed.GetAddressOf(),&offset,&length);
            check(observed.Get()==expected && offset==first && length==count,"projection stage identity and exact range");
        }
    };
    const char* code="cbuffer Input:register(b0){float4 value;} RWStructuredBuffer<float4> output:register(u0);"
        "[numthreads(1,1,1)] void main(){output[0]=value;}";
    ComPtr<ID3DBlob> shaderBytes, errors;
    check(SUCCEEDED(D3DCompile(code,std::strlen(code),nullptr,nullptr,nullptr,"main","cs_5_0",0,0,
        shaderBytes.GetAddressOf(),errors.GetAddressOf())),"projection fixture shader");
    if(!shaderBytes)return;
    ComPtr<ID3D11ComputeShader> shader;
    check(SUCCEEDED(device->CreateComputeShader(shaderBytes->GetBufferPointer(),shaderBytes->GetBufferSize(),nullptr,
        shader.GetAddressOf())),"projection fixture CS");
    D3D11_BUFFER_DESC desc{};desc.ByteWidth=16;desc.StructureByteStride=16;
    desc.BindFlags=D3D11_BIND_UNORDERED_ACCESS;desc.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    ComPtr<ID3D11Buffer> output;check(SUCCEEDED(device->CreateBuffer(&desc,nullptr,output.GetAddressOf())),"projection result buffer");
    ComPtr<ID3D11UnorderedAccessView> uav;
    check(output && SUCCEEDED(device->CreateUnorderedAccessView(output.Get(),nullptr,uav.GetAddressOf())),"projection result UAV");
    desc.Usage=D3D11_USAGE_STAGING;desc.BindFlags=desc.MiscFlags=desc.StructureByteStride=0;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Buffer> staging;check(SUCCEEDED(device->CreateBuffer(&desc,nullptr,staging.GetAddressOf())),"projection result staging");
    if(!shader || !output || !uav || !staging)return;
    auto dispatch=[&](float expected) {
        auto* view=uav.Get();ctx->CSSetUnorderedAccessViews(0,1,&view,nullptr);ctx->CSSetShader(shader.Get(),nullptr,0);
        ctx->Dispatch(1,1,1);ID3D11UnorderedAccessView* none=nullptr;ctx->CSSetUnorderedAccessViews(0,1,&none,nullptr);
        ctx->CopyResource(staging.Get(),output.Get());D3D11_MAPPED_SUBRESOURCE map{};
        const HRESULT hr=ctx->Map(staging.Get(),0,D3D11_MAP_READ,0,&map);check(SUCCEEDED(hr),"projection result readback");
        if(SUCCEEDED(hr)){const auto* values=static_cast<const float*>(map.pData);
            check(values[0]==expected && values[1]==3 && values[2]==4 && values[3]==5,"dispatch consumes full private CB at original range");ctx->Unmap(staging.Get(),0);}
    };
    dispatch(2);
    {
        FlatProjectionBindingScope scope(plan);check(scope.active(),"qualified projection binding transaction");
        check(!g_flatComputeInternal,"projection observer suppression ends before game work");
        checkBindings(replacement);dispatch(2.625f);
        // A nested private operation may clear state or throw/return early.
        ctx->ClearState();
    }
    checkBindings(original.Get());dispatch(2);
    bindings[1].firstConstant=0;
    check(plan.initialize(ctx.Get(),bindings,3),"stale range plan is structurally valid");
    {FlatProjectionBindingScope refused(plan);check(!refused.active(),"stale second range refuses entire transaction");}
    checkBindings(original.Get());bindings[1].firstConstant=first;
    bindings[1]=bindings[0];
    check(!plan.initialize(ctx.Get(),bindings,3),"duplicate stage-slot preflight refused");
    {FlatProjectionBindingScope refused(plan);check(!refused.active(),"invalid plan cannot bind");}
    checkBindings(original.Get());
    {
        FlatComputeInternalScope outer;
        check(plan.initialize(ctx.Get(),bindings,1),"nested scope plan");
        FlatProjectionBindingScope nested(plan);check(nested.active(),"nested observer guard accepted");
        check(g_flatComputeInternal,"nested suppression retains outer guard");
    }
    check(!g_flatComputeInternal,"nested suppression restored");checkBindings(original.Get());
    check(shadows.invalidate(original.Get(),1) && !privateBuffer.prepare(shadows,&patch,1,jitter,2),"failed prepare invalidates existing plan token");
    certificate[0]=713;
    check(!privateBuffer.copyUploadedConstants(replacement,0,4,certificate) && certificate[0]==713,"invalidated private upload cannot certify stale bytes");
    check(!plan.copyUploadedConstants(FlatProjectionStage::Vertex,1,0,4,certificate),"plan certificate rejects invalidated upload");
    {FlatProjectionBindingScope refused(plan);check(!refused.active(),"old plan cannot bind after failed prepare");}
    check(!plan.refreshPrepared(),"invalid token cannot refresh plan");checkBindings(original.Get());
    check(shadows.captureFullWrite(original.Get(),1,raw,sizeof(raw)) &&
        privateBuffer.prepare(shadows,&patch,1,jitter,2),"recover source and private upload");
    {FlatProjectionBindingScope refused(plan);check(!refused.active(),"new upload requires explicit plan token refresh");}
    check(!plan.copyUploadedConstants(FlatProjectionStage::Vertex,1,0,4,certificate),"plan certificate rejects unrefreshed revision");
    check(plan.refreshPrepared(),"refresh tokens without repeated resource preflight");
    {FlatProjectionBindingScope refreshed(plan);check(refreshed.active(),"refreshed plan uses current preparation");}
    checkBindings(original.Get());
    FlatProjectionBindingPlan retiredPlan;
    {
        FlatProjectionPrivateBuffer temporary;
        check(temporary.initialize(ctx.Get(),original.Get(),1) && temporary.prepare(shadows,&patch,1,jitter,2),"temporary private buffer");
        auto binding=temporary.binding(FlatProjectionStage::Vertex,1,first,count);
        check(retiredPlan.initialize(ctx.Get(),&binding,1),"plan retains prepared GPU resource");
    }
    {FlatProjectionBindingScope refused(retiredPlan);check(!refused.active(),"destroyed preparation owner invalidates retained plan");}
    checkBindings(original.Get());
    ComPtr<ID3D11DeviceContext1> deferred;
    ComPtr<ID3D11DeviceContext> deferredBase;
    if(SUCCEEDED(device->CreateDeferredContext(0,deferredBase.GetAddressOf())))deferredBase.As(&deferred);
    check(bool(deferred),"deferred projection test fixture");
    check(!plan.initialize(deferred.Get(),bindings,1),"deferred context refuses projection plan");
    {FlatProjectionBindingScope refused(plan);check(!refused.active(),"deferred context refuses projection scope");}
    {
        UINT f=0,n=sizeof(raw)/16;auto* source=original.Get();ctx->VSSetConstantBuffers1(1,1,&source,&f,&n);
        auto full=privateBuffer.binding(FlatProjectionStage::Vertex,1,f,n);full.wholeBuffer=true;
        FlatProjectionBindingPlan fullPlan;check(fullPlan.initialize(ctx.Get(),&full,1),"full-size ranged-origin whole-mode fixture");
        {FlatProjectionBindingScope scope(fullPlan);}
        ComPtr<ID3D11Buffer> restored;UINT rf=0,rn=0;ctx->VSGetConstantBuffers1(1,1,restored.GetAddressOf(),&rf,&rn);
        check(restored.Get()==original.Get() && rf==f && rn==n,"whole install failure or success restores exact full-size ranged origin");
    }
    {
        float wholeRaw[196]{};wholeRaw[56]=2;wholeRaw[59]=3;
        D3D11_BUFFER_DESC bd{};bd.ByteWidth=sizeof(wholeRaw);bd.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
        D3D11_SUBRESOURCE_DATA data{};data.pSysMem=wholeRaw;ComPtr<ID3D11Buffer> whole;
        check(SUCCEEDED(device->CreateBuffer(&bd,&data,whole.GetAddressOf())),"49-constant whole buffer fixture");
        FlatProjectionShadowBank<1,1024> bank;FlatProjectionPrivateBuffer clone;
        FlatProjectionPatchRequest inverse{FlatProjectionPatchLayout::InverseUvRay,14*16,{}};
        check(bank.registerBuffer(whole.Get(),1,sizeof(wholeRaw)) && bank.captureFullWrite(whole.Get(),1,wholeRaw,sizeof(wholeRaw)) &&
            clone.initialize(ctx.Get(),whole.Get(),1) && clone.prepare(bank,&inverse,1,jitter,1),"whole buffer inverse-only upload");
        auto binding=clone.binding(FlatProjectionStage::Vertex,2,0,49);FlatProjectionBindingPlan wholePlan;
        check(!wholePlan.initialize(ctx.Get(),&binding,1),"49 constants remain invalid in ranged mode");
        binding.wholeBuffer=true;
        check(wholePlan.initialize(ctx.Get(),&binding,1),"explicit whole mode admits exact natural count");
        float changed=0;check(wholePlan.copyUploadedConstants(FlatProjectionStage::Vertex,2,14*16+12,4,&changed) && changed==2.875f,
            "whole inverse certificate contains uploaded correction only");
        auto invalid=binding;invalid.constantCount=48;FlatProjectionBindingPlan invalidPlan;
        check(!invalidPlan.initialize(ctx.Get(),&invalid,1),"whole mode rejects nonnatural count");
        invalid=binding;invalid.firstConstant=16;
        check(!invalidPlan.initialize(ctx.Get(),&invalid,1),"whole mode rejects nonzero offset");
        auto* originalWhole=whole.Get();ctx->VSSetConstantBuffers(2,1,&originalWhole);
        ComPtr<ID3D11Buffer> observed;UINT naturalFirst=0,naturalCount=0;
        ctx->VSGetConstantBuffers1(2,1,observed.GetAddressOf(),&naturalFirst,&naturalCount);
        const bool natural=observed.Get()==whole.Get() && naturalFirst==0 && naturalCount==49;
        bool bound=false;
        {FlatProjectionBindingScope scope(wholePlan);check(scope.active()==natural,"whole scope requires actual natural GetCB1 range");
            bound=scope.active();
            if(scope.active()) {ComPtr<ID3D11Buffer> ordinary,ranged;UINT f=0,n=0;
                ctx->VSGetConstantBuffers(2,1,ordinary.GetAddressOf());ctx->VSGetConstantBuffers1(2,1,ranged.GetAddressOf(),&f,&n);
                check(ordinary.Get()==binding.replacement && ranged.Get()==binding.replacement && f==0 && n==49,
                    "whole install attests ordinary and Context1 private binding");}}
        std::printf("projection scope natural49: first=%u count=%u qualified=%u bound=%u\n",naturalFirst,naturalCount,natural?1u:0u,bound?1u:0u);
        observed.Reset();UINT restoredFirst=0,restoredCount=0;
        ctx->VSGetConstantBuffers1(2,1,observed.GetAddressOf(),&restoredFirst,&restoredCount);
        check(observed.Get()==whole.Get() && restoredFirst==naturalFirst && restoredCount==naturalCount,
            "whole scope restores original natural binding or refuses without mutation");
        ID3D11Buffer* absent=nullptr;ctx->VSSetConstantBuffers(2,1,&absent);
        {FlatProjectionBindingScope scope(wholePlan);check(!scope.active(),"whole scope rejects changed actual buffer");}
    }
    ctx->ClearState();
}
