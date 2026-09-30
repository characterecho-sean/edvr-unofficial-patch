#pragma once
#include "../../src/d3d11/engine_velocity_primary_copy.h"
#include <cstring>

namespace primary_copy_tests {
namespace copy=edvr::engine_velocity_primary_copy;
namespace emit=edvr::engine_velocity_emit;
using Microsoft::WRL::ComPtr;
using Record=std::array<uint32_t,84>;
// Forward the real device factory and count/inject failure only for the
// private pool under test. The production apply() path remains intact.
struct UavCreationSpy {
    using CreateFn=HRESULT (STDMETHODCALLTYPE*)(ID3D11Device*,ID3D11Resource*,const D3D11_UNORDERED_ACCESS_VIEW_DESC*,ID3D11UnorderedAccessView**);
    inline static UavCreationSpy* active=nullptr;
    ID3D11Device* device;void** original;void* slots[43];
    unsigned calls=0;bool failNext=false;
    static HRESULT STDMETHODCALLTYPE create(ID3D11Device* d,ID3D11Resource* resource,const D3D11_UNORDERED_ACCESS_VIEW_DESC* desc,ID3D11UnorderedAccessView** out) {
        ++active->calls;
        if(active->failNext){active->failNext=false;if(out)*out=nullptr;return E_OUTOFMEMORY;}
        return reinterpret_cast<CreateFn>(active->original[8])(d,resource,desc,out);
    }
    void setTable(void** table) {
        DWORD old=0,ignored=0;
        if(!VirtualProtect(device,sizeof(void*),PAGE_READWRITE,&old))std::abort();
        *reinterpret_cast<void***>(device)=table;
        if(!VirtualProtect(device,sizeof(void*),old,&ignored))std::abort();
    }
    explicit UavCreationSpy(ID3D11Device* d):device(d),original(*reinterpret_cast<void***>(d)) {
        if(active)std::abort();std::memcpy(slots,original,sizeof(slots));slots[8]=reinterpret_cast<void*>(&create);
        active=this;setTable(slots);
    }
    ~UavCreationSpy(){setTable(original);active=nullptr;}
};
inline Record record(float x) {
    Record r{};r.fill(0x12345678u);r[0]=0;r[1]=r[77]=0x3F800000u;
    r[2]=r[78]=0x7FFF7FFFu;r[3]=r[79]=0xFFFE7FFFu;r[72]=0;
    const float p[3]={x,2,3};std::memcpy(&r[4],p,12);std::memcpy(&r[73],p,12);return r;
}
inline emit::Pose previous(const Record& r) {auto p=copy::current(r.data());float x=0;std::memcpy(&x,&p.w[0],4);x-=1;std::memcpy(&p.w[0],&x,4);return p;}
inline bool claim(Record& r,uint32_t frame) {const auto p=previous(r);return copy::recordEmission(reinterpret_cast<uintptr_t>(r.data()),r.data(),p,emit::kJoined^emit::markerHash(copy::current(r.data()),p,frame),frame);}
struct Node { alignas(16) unsigned char data[32+8*336+64]{};
    uintptr_t ptr(){return reinterpret_cast<uintptr_t>(data);}
    Record& item(unsigned i){return *reinterpret_cast<Record*>(data+32+i*336);}
    void links(uintptr_t anchor,uint64_t n){uint64_t h[4]={anchor,anchor,0,n};std::memcpy(data,h,32);}
};
struct Dictionary {
    alignas(16) unsigned char data[48]{},entry[56]{};uintptr_t bucket=0;
    uintptr_t ptr(){return reinterpret_cast<uintptr_t>(data);}
    uintptr_t anchor(){return reinterpret_cast<uintptr_t>(entry)+0x28;}
    void set(Node* node){
        const uintptr_t bp=reinterpret_cast<uintptr_t>(&bucket);const uint64_t count=1;
        std::memcpy(data+0x10,&bp,8);std::memcpy(data+0x18,&count,8);
        bucket=reinterpret_cast<uintptr_t>(entry);std::memcpy(entry,&bp,8);
        const uintptr_t p=node?node->ptr():anchor();std::memcpy(entry+0x28,&p,8);std::memcpy(entry+0x30,&p,8);
        if(node)node->links(anchor(),1);
    }
};
inline void runObservers(ID3D11Device* device,void (*check)(bool,const char*)) {
    constexpr uint32_t frame=42;constexpr unsigned repeats=256;
    Node node;Dictionary dictionary;dictionary.set(&node);node.item(0)=record(80);
    uintptr_t destination[2]={reinterpret_cast<uintptr_t>(destination),reinterpret_cast<uintptr_t>(destination)};
    copy::reset();rig_allocations::start();
    for(unsigned i=0;i<repeats;++i)copy::invalidateDictionaryImpl<false>(dictionary.ptr());
    const auto clearBefore=rig_allocations::stop();const auto clearOld=copy::stats();
    copy::reset();rig_allocations::start();
    for(unsigned i=0;i<repeats;++i)copy::invalidateDictionary(dictionary.ptr());
    const auto clearAfter=rig_allocations::stop();const auto clearNew=copy::stats();
    check(clearBefore>0 && clearAfter==0 && clearOld.clearNodes==repeats && clearNew.clearNodes==0 &&
          clearNew.clearCalls==repeats && clearNew.clearNoClaims==repeats,
          "primary observer: no-claim clear bypass avoids measured heap allocations and native traversal");
    copy::reset();rig_allocations::start();
    for(unsigned i=0;i<repeats;++i)copy::endMergeOpaque(copy::beginMergeOpaque(reinterpret_cast<uintptr_t>(destination),dictionary.anchor(),frame),true);
    const auto mergeBefore=rig_allocations::stop();const auto mergeOld=copy::stats();
    copy::reset();rig_allocations::start();
    for(unsigned i=0;i<repeats;++i)copy::endMergeOpaque(copy::beginMergeOpaque(reinterpret_cast<uintptr_t>(destination),dictionary.anchor(),frame),true);
    const auto mergeAfter=rig_allocations::stop();const auto mergeNew=copy::stats();
    check(mergeBefore>0 && mergeAfter==mergeBefore && mergeOld.mergeNodes==repeats && mergeNew.mergeNodes==repeats &&
          mergeNew.mergeCalls==repeats && mergeNew.mergeWithoutClaims==repeats && mergeNew.mergePlans==repeats,
          "primary observer: empty native merge plan allocation/traversal remains unchanged for unwind safety");
    std::printf("primary observer no-claim production work / %u calls: clear allocations %llu -> %llu, nodes %llu -> %llu; merge allocations %llu -> %llu, nodes %llu -> %llu\n",
                repeats,static_cast<unsigned long long>(clearBefore),static_cast<unsigned long long>(clearAfter),
                static_cast<unsigned long long>(clearOld.clearNodes),static_cast<unsigned long long>(clearNew.clearNodes),
                static_cast<unsigned long long>(mergeBefore),static_cast<unsigned long long>(mergeAfter),
                static_cast<unsigned long long>(mergeOld.mergeNodes),static_cast<unsigned long long>(mergeNew.mergeNodes));
    D3D11_BUFFER_DESC d{};d.ByteWidth=672;d.BindFlags=D3D11_BIND_SHADER_RESOURCE;d.StructureByteStride=336;d.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    ComPtr<ID3D11Buffer> buffer;check(SUCCEEDED(device->CreateBuffer(&d,nullptr,&buffer)),"primary observer: retained pool fixture");
    auto exercise=[&](bool bypass){
        copy::reset();const auto epoch=copy::g_emissionEpoch;
        Node source;Dictionary dict;dict.set(&source);source.item(0)=record(80);Record other=record(90),mapped[2]={source.item(0),other};
        auto clear=[&](){if(bypass)copy::invalidateDictionary(dict.ptr());else copy::invalidateDictionaryImpl<false>(dict.ptr());};
        uintptr_t dst[2]={reinterpret_cast<uintptr_t>(dst),reinterpret_cast<uintptr_t>(dst)};
        auto begin=[&](){return copy::beginMergeOpaque(reinterpret_cast<uintptr_t>(dst),dict.anchor(),frame);};
        clear();check(claim(source.item(0),frame) && claim(other,frame),"primary observer: fresh and unmapped unrelated claims after empty clear");clear();
        check(!copy::g_emissions.count(reinterpret_cast<uintptr_t>(&source.item(0))) && copy::g_emissions.count(reinterpret_cast<uintptr_t>(&other)),"primary observer: owned clear preserves unrelated unmapped source");
        copy::beginMap(buffer.Get(),mapped,sizeof(mapped),336,D3D11_MAP_WRITE_DISCARD,1,frame);
        copy::copier(reinterpret_cast<uintptr_t>(mapped),336,reinterpret_cast<uintptr_t>(&source.item(0)),0,1,frame);copy::endMap(buffer.Get(),1);
        check(copy::patches(buffer.Get(),frame).empty(),"primary observer: freed same-address identical bytes cannot inherit a certificate");
        claim(source.item(0),frame);copy::beginMap(buffer.Get(),mapped,sizeof(mapped),336,D3D11_MAP_WRITE_DISCARD,2,frame);
        copy::copier(reinterpret_cast<uintptr_t>(mapped),336,reinterpret_cast<uintptr_t>(&source.item(0)),0,1,frame);copy::endMap(buffer.Get(),2);clear();
        check(copy::patches(buffer.Get(),frame).size()==1,"primary observer: native clear preserves independent copied GPU certificate");
        copy::invalidateEmission(reinterpret_cast<uintptr_t>(&other));claim(source.item(0),frame);
        void* pending=begin();check(pending && copy::g_emissions.empty() && copy::stats().activePlans==1,"primary observer: detached claim blocks no-owner shortcut");
        const auto skips=copy::stats().clearNoClaims;clear();check(copy::stats().clearNoClaims==skips,"primary observer: active plan clear performs native ownership walk");copy::endMergeOpaque(pending,true);
        check(copy::g_emissions.empty() && copy::stats().activePlans==0,"primary observer: clear revokes detached plan before end");
        // Snapshot semantics: a plan opened with no claims cannot pick up a
        // later emission. Native merge/clear may reenter between begin/end.
        void* emptyPlan=begin();check(emptyPlan!=nullptr,"primary observer: empty merge retains its native relay plan token");
        claim(source.item(0),frame);check(copy::stats().activeClaims==1,"primary observer: emission may arrive inside native merge");
        void* nested=begin();check(nested!=nullptr,"primary observer: reentrant merge sees late claim and cannot bypass");
        clear();copy::endMergeOpaque(nested,true);copy::endMergeOpaque(emptyPlan,true);
        check(copy::g_emissions.empty() && copy::stats().activePlans==0,"primary observer: nested clear/end cannot resurrect late claims");
        // Also cover late arrival without a nested clear: original empty
        // plan has no transfers and must preserve the newly arrived claim.
        emptyPlan=begin();claim(source.item(0),frame);copy::endMergeOpaque(emptyPlan,true);
        check(copy::g_emissions.count(reinterpret_cast<uintptr_t>(&source.item(0)))==1,"primary observer: late claim survives empty-plan end unchanged");
        copy::invalidateEmission(reinterpret_cast<uintptr_t>(&source.item(0)));
        emptyPlan=begin();claim(source.item(0),frame);const auto beforeUnwind=copy::g_emissionEpoch;copy::endMergeOpaque(emptyPlan,false);
        check(copy::g_emissions.empty() && copy::g_emissionEpoch!=beforeUnwind,"primary observer: unsuccessful empty-plan unwind revokes late claims");
        emptyPlan=begin();claim(source.item(0),frame);clear();claim(other,frame);
        const auto clearedEpoch=copy::g_emissionEpoch;copy::endMergeOpaque(emptyPlan,false);
        check(copy::g_emissions.count(reinterpret_cast<uintptr_t>(&other))==1 && copy::g_emissionEpoch==clearedEpoch,
              "primary observer: clear-invalidated empty-plan unwind preserves unrelated later owners");
        copy::invalidateEmission(reinterpret_cast<uintptr_t>(&other));emptyPlan=begin();claim(other,frame+1);
        const auto futureEpoch=copy::g_emissionEpoch;copy::endMergeOpaque(emptyPlan,false);
        check(copy::g_emissions.count(reinterpret_cast<uintptr_t>(&other))==1 && copy::g_emissionEpoch==futureEpoch,
              "primary observer: stale empty-plan unwind preserves owners registered in a newer epoch");
        copy::invalidateEmission(reinterpret_cast<uintptr_t>(&other));
        claim(source.item(0),frame+1);
        const auto retained=copy::patches(buffer.Get(),frame);
        check(retained.size()==1 && copy::patches(buffer.Get(),frame+1).empty(),"primary observer: retained output freshness remains frame-specific");
        const auto& current=copy::g_emissions.at(reinterpret_cast<uintptr_t>(&source.item(0)));
        std::vector<uint32_t> result={uint32_t(copy::g_emissions.size()),copy::g_emissionFrame,copy::g_overflowFrame,uint32_t(copy::g_emissionEpoch-epoch),current.patch.marker};
        result.insert(result.end(),current.patch.native.begin(),current.patch.native.end());result.insert(result.end(),current.patch.previous.w,current.patch.previous.w+5);
        result.insert(result.end(),retained[0].native.begin(),retained[0].native.end());result.push_back(retained[0].marker);result.push_back(retained[0].slot);
        copy::reset();return result;
    };
    const auto before=exercise(false),after=exercise(true);
    check(before==after,"primary observer: original/bypassed ownership, generation and exact output certificate bytes match");
    // Skip means uninspected, never a synthetic successful native validation.
    copy::reset();const auto epoch=copy::g_emissionEpoch;copy::invalidateDictionary(0);
    check(copy::stats().clearCalls==1 && copy::stats().clearNoClaims==1 && copy::stats().clearFailed==0 && copy::g_emissionEpoch==epoch,
          "primary observer: no-owner malformed dictionary is explicitly skipped without changing future epoch");
    copy::reset();
}
inline void run(ID3D11Device* device,ID3D11DeviceContext* ctx,void (*check)(bool,const char*)) {
    copy::reset();copy::OutputCache output;const uint32_t frame=42;
    Record src[2]={record(10),record(20)},mapped[2]={src[0],src[1]};
    D3D11_BUFFER_DESC d{};d.ByteWidth=sizeof(src);d.StructureByteStride=336;
    d.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;d.BindFlags=D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA initial{src,0,0};ComPtr<ID3D11Buffer> native,privatePool;
    check(SUCCEEDED(device->CreateBuffer(&d,&initial,&native)),"primary copy: native pool fixture");
    d.BindFlags|=D3D11_BIND_UNORDERED_ACCESS;
    check(SUCCEEDED(device->CreateBuffer(&d,&initial,&privatePool)),"primary copy: private pool fixture");
    if(!native || !privatePool)return;
    auto begin=[&](uint64_t seq,D3D11_MAP type=D3D11_MAP_WRITE_DISCARD,uint32_t tick=42){
        return copy::beginMap(native.Get(),mapped,sizeof(mapped),336,type,seq,tick);
    };
    auto forward=[&](Record* source,uint32_t first,uint32_t count,uint32_t tick=42){
        copy::copier(reinterpret_cast<uintptr_t>(mapped),336,reinterpret_cast<uintptr_t>(source),first,count,tick);
    };
    check(claim(src[0],frame) && claim(src[1],frame),"primary copy: two independently certified source addresses");
    check(begin(1),"primary copy: discard opens exact mapped resource lease");forward(src,0,2);
    check(copy::patches(native.Get(),frame).empty(),"primary copy: active mapped GPU source never scattered");
    copy::endMap(native.Get(),1);check(copy::patches(native.Get(),frame).size()==2,"primary copy: authoritative source/destination copy joins both slots");
    ctx->CopyResource(privatePool.Get(),native.Get());
    {
        UavCreationSpy spy(device);
        for(unsigned i=0;i<4;++i){ctx->CopyResource(privatePool.Get(),native.Get());check(copy::apply(ctx,privatePool.Get(),native.Get(),frame,output),"primary copy: repeated scatter on same private clone");}
        std::printf("  primary copy UAV factory: %u creates for 4 same-pool scatters\n",spy.calls);
        check(spy.calls==1,"primary copy cache: four repeated scatters create one UAV");
    }
    auto read=[&](ID3D11Buffer* buffer){
        D3D11_BUFFER_DESC bd{};buffer->GetDesc(&bd);bd.Usage=D3D11_USAGE_STAGING;bd.BindFlags=0;bd.MiscFlags=0;bd.StructureByteStride=0;bd.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Buffer> staging;device->CreateBuffer(&bd,nullptr,&staging);ctx->CopyResource(staging.Get(),buffer);
        D3D11_MAPPED_SUBRESOURCE m{};Record out[2]{};if(SUCCEEDED(ctx->Map(staging.Get(),0,D3D11_MAP_READ,0,&m))){std::memcpy(out,m.pData,sizeof(out));ctx->Unmap(staging.Get(),0);}return std::array<Record,2>{out[0],out[1]};
    };
    const auto patched=read(privatePool.Get()),unchanged=read(native.Get());
    check(unchanged[0]==src[0] && unchanged[1]==src[1],"primary copy: native GPU bytes remain bit-identical");
    for(unsigned n=0;n<2;++n){auto expected=src[n];const auto p=previous(src[n]);expected[72]=emit::kJoined^emit::markerHash(copy::current(src[n].data()),p,frame);expected[73]=p.w[0];expected[74]=p.w[1];expected[75]=p.w[2];expected[78]=p.w[3];expected[79]=p.w[4];check(patched[n]==expected,"primary copy: only private marker and previous pose bytes change");}
    // Separate eye owners must reuse both views while alternating. A shared
    // last-pool cache would recreate on every switch and fail this count.
    ComPtr<ID3D11Buffer> rightPool;check(SUCCEEDED(device->CreateBuffer(&d,&initial,&rightPool)),"primary cache: second eye pool");
    copy::OutputCache rightOutput;output={};
    {
        UavCreationSpy spy(device);
        for(unsigned i=0;i<4;++i) {
            ctx->CopyResource(privatePool.Get(),native.Get());ctx->CopyResource(rightPool.Get(),native.Get());
            check(copy::apply(ctx,privatePool.Get(),native.Get(),frame,output),"primary cache: left eye scatter");
            check(copy::apply(ctx,rightPool.Get(),native.Get(),frame,rightOutput),"primary cache: right eye scatter");
        }
        check(spy.calls==2,"primary cache: eight alternating eye scatters create two UAVs");
        std::printf("  primary copy alternating UAV factory: %u creates for 8 stereo scatters\n",spy.calls);
    }
    check(read(privatePool.Get())==patched && read(rightPool.Get())==patched,"primary cache: both eye row outputs exact");
    {
        UavCreationSpy spy(device);
        check(copy::apply(ctx,rightPool.Get(),native.Get(),frame,output),"primary cache: replacement allocation scatters");
        check(spy.calls==1 && output.pool.Get()==rightPool.Get(),"primary cache: replacement cannot inherit old pool view");
        ComPtr<ID3D11Resource> resource;output.view->GetResource(&resource);
        check(resource.Get()==rightPool.Get(),"primary cache: cached UAV owns replacement pool identity");
    }
    check(read(rightPool.Get())==patched,"primary cache: replacement row output exact");
    // A failed factory call cannot leave the previous allocation's view in
    // the cache or silently treat zero work as success. Retry creates it.
    {
        UavCreationSpy spy(device);spy.failNext=true;
        check(!copy::apply(ctx,privatePool.Get(),native.Get(),frame,output) &&
              copy::stats().lastScatterFailure==copy::ScatterFailure::View,"primary cache: factory failure remains counted refusal");
        check(!output.view && !output.pool && !output.device,"primary cache: failed replacement leaves no stale view identity");
        check(copy::apply(ctx,privatePool.Get(),native.Get(),frame,output),"primary cache: factory failure retries successfully");
        check(spy.calls==2,"primary cache: failure plus retry each call factory once");
    }
    check(read(privatePool.Get())==patched,"primary cache: retry row output exact");
    // Descriptor failures still decline before UAV use, even with a warm
    // cache. Cache reuse must not bypass the existing safety checks.
    ComPtr<ID3D11Buffer> noUav;D3D11_BUFFER_DESC invalid=d;invalid.BindFlags=D3D11_BIND_SHADER_RESOURCE;
    check(SUCCEEDED(device->CreateBuffer(&invalid,&initial,&noUav)),"primary cache: invalid descriptor fixture");
    {
        UavCreationSpy spy(device);
        check(!copy::apply(ctx,noUav.Get(),native.Get(),frame,output) &&
              copy::stats().lastScatterFailure==copy::ScatterFailure::Descriptor,"primary cache: invalid pool descriptor still refused");
        check(spy.calls==0,"primary cache: descriptor failure creates nothing");
    }
    check(read(noUav.Get())==std::array<Record,2>{src[0],src[1]},"primary cache: invalid pool remains unchanged");
    {
        output={};check(!output.view && !output.pool && !output.device,"primary cache: owner reset releases all cached references");
        UavCreationSpy spy(device);
        check(copy::apply(ctx,privatePool.Get(),native.Get(),frame,output),"primary cache: owner reset recreates view");
        check(spy.calls==1,"primary cache: reset does not reuse old view");
    }
    // Use another real WARP device. Reuse across devices must build that
    // device's resources. Inject the foreign-pool factory refusal rather
    // than ask WARP to use an invalid cross-device resource (which removes
    // the device); the command observer proves the old view is not reused.
    {
        ComPtr<ID3D11Device> otherDevice;ComPtr<ID3D11DeviceContext> otherCtx;D3D_FEATURE_LEVEL level;
        check(SUCCEEDED(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,
              &otherDevice,&level,&otherCtx)),"primary cache: second WARP device");
        ComPtr<ID3D11Buffer> otherNative,otherPrivate;D3D11_BUFFER_DESC nd=d;nd.BindFlags=D3D11_BIND_SHADER_RESOURCE;
        check(SUCCEEDED(otherDevice->CreateBuffer(&nd,&initial,&otherNative)) &&
              SUCCEEDED(otherDevice->CreateBuffer(&d,&initial,&otherPrivate)),"primary cache: second device pools");
        check(claim(src[0],frame) && claim(src[1],frame),"primary cache: second device row claims");
        check(copy::beginMap(otherNative.Get(),mapped,sizeof(mapped),336,D3D11_MAP_WRITE_DISCARD,1,frame),"primary cache: second device lease");
        forward(src,0,2);copy::endMap(otherNative.Get(),1);
        {
            UavCreationSpy spy(otherDevice.Get());
            check(copy::apply(otherCtx.Get(),otherPrivate.Get(),otherNative.Get(),frame,output),"primary cache: device replacement scatters");
            check(spy.calls==1 && output.device.Get()==otherDevice.Get() && output.pool.Get()==otherPrivate.Get(),
                  "primary cache: replacement device owns cache identity");
        }
        {
            UavCreationSpy spy(device);spy.failNext=true;
            check(!copy::apply(ctx,otherPrivate.Get(),otherNative.Get(),frame,output) &&
                  copy::stats().lastScatterFailure==copy::ScatterFailure::View,"primary cache: device-change factory refusal counted");
            check(spy.calls==1 && !output.view,"primary cache: foreign context clears old cache and attempts validated factory");
        }
        D3D11_BUFFER_DESC stage=d;stage.Usage=D3D11_USAGE_STAGING;stage.BindFlags=stage.MiscFlags=stage.StructureByteStride=0;stage.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Buffer> staging;check(SUCCEEDED(otherDevice->CreateBuffer(&stage,nullptr,&staging)),"primary cache: other device readback");
        otherCtx->CopyResource(staging.Get(),otherPrivate.Get());D3D11_MAPPED_SUBRESOURCE m{};
        check(SUCCEEDED(otherCtx->Map(staging.Get(),0,D3D11_MAP_READ,0,&m)),"primary cache: other device read Map");
        check(std::memcmp(m.pData,patched.data(),sizeof(src))==0,"primary cache: device replacement and refusal preserve exact rows");
        otherCtx->Unmap(staging.Get(),0);copy::forget(otherNative.Get());
    }
    check(copy::apply(ctx,privatePool.Get(),native.Get(),frame,output),"primary cache: original device cache restored for subsequent tests");
    // GPU guard sees the post-CopyResource bytes, not the earlier CPU lease.
    auto mismatched=src[0];mismatched[30]^=1;Record guarded[2]={mismatched,src[1]};
    ctx->UpdateSubresource(privatePool.Get(),0,nullptr,guarded,0,0);
    check(copy::apply(ctx,privatePool.Get(),native.Get(),frame,output),"primary copy: guarded batch dispatched");
    check(read(privatePool.Get())[0]==mismatched,"primary copy: GPU full-record guard rejects late material overwrite");
    ComPtr<ID3D11ShaderResourceView> cloneSrv;device->CreateShaderResourceView(privatePool.Get(),nullptr,&cloneSrv);
    ctx->PSSetShaderResources(7,1,cloneSrv.GetAddressOf());
    check(!copy::apply(ctx,privatePool.Get(),native.Get(),frame,output) && copy::stats().lastScatterFailure==copy::ScatterFailure::Bound,
          "primary copy: active clone SRV refuses UAV scatter with counted failure");
    ID3D11ShaderResourceView* nullSrv=nullptr;ctx->PSSetShaderResources(7,1,&nullSrv);
    ComPtr<ID3D11UnorderedAccessView> cloneUav;device->CreateUnorderedAccessView(privatePool.Get(),nullptr,&cloneUav);
    ctx->OMSetRenderTargetsAndUnorderedAccessViews(0,nullptr,nullptr,0,1,cloneUav.GetAddressOf(),nullptr);
    check(!copy::apply(ctx,privatePool.Get(),native.Get(),frame,output),"primary copy: OM UAV alias refuses scatter");
    ID3D11UnorderedAccessView* nullUav=nullptr;ctx->OMSetRenderTargetsAndUnorderedAccessViews(0,nullptr,nullptr,0,1,&nullUav,nullptr);
    ComPtr<ID3D11Buffer> other,oldCount;device->CreateBuffer(&d,&initial,&other);
    ComPtr<ID3D11ShaderResourceView> oldSrv;device->CreateShaderResourceView(native.Get(),nullptr,&oldSrv);
    ComPtr<ID3D11UnorderedAccessView> oldUav;device->CreateUnorderedAccessView(other.Get(),nullptr,&oldUav);
    D3D11_BUFFER_DESC cb{};cb.ByteWidth=16;cb.BindFlags=D3D11_BIND_CONSTANT_BUFFER;device->CreateBuffer(&cb,nullptr,&oldCount);
    ctx->CSSetShader(copy::g_gpu.shader.Get(),nullptr,0);ctx->CSSetShaderResources(0,1,oldSrv.GetAddressOf());
    ctx->CSSetUnorderedAccessViews(0,1,oldUav.GetAddressOf(),nullptr);ctx->CSSetConstantBuffers(0,1,oldCount.GetAddressOf());
    check(copy::apply(ctx,privatePool.Get(),native.Get(),frame,output),"primary copy: scatter with prior compute bindings");
    ComPtr<ID3D11ComputeShader> restoredShader;ComPtr<ID3D11ShaderResourceView> restoredSrv;
    ComPtr<ID3D11UnorderedAccessView> restoredUav;ComPtr<ID3D11Buffer> restoredCount;
    ctx->CSGetShader(&restoredShader,nullptr,nullptr);ctx->CSGetShaderResources(0,1,&restoredSrv);
    ctx->CSGetUnorderedAccessViews(0,1,&restoredUav);ctx->CSGetConstantBuffers(0,1,&restoredCount);
    check(restoredShader.Get()==copy::g_gpu.shader.Get() && restoredSrv.Get()==oldSrv.Get() && restoredUav.Get()==oldUav.Get() && restoredCount.Get()==oldCount.Get(),
          "primary copy: compute shader/SRV/UAV/constants restored exactly");
    ctx->CSSetShader(nullptr,nullptr,0);ctx->CSSetShaderResources(0,1,&nullSrv);ctx->CSSetUnorderedAccessViews(0,1,&nullUav,nullptr);
    ID3D11Buffer* nullBuffer=nullptr;ctx->CSSetConstantBuffers(0,1,&nullBuffer);
    check(begin(2,D3D11_MAP_WRITE_NO_OVERWRITE),"primary copy: append lease opens");forward(src,0,1);copy::endMap(native.Get(),2);
    check(copy::patches(native.Get(),frame).size()==1,"primary copy: consumed source cannot recertify overwritten slot; disjoint append retained");
    ctx->CopyResource(privatePool.Get(),native.Get());copy::apply(ctx,privatePool.Get(),native.Get(),frame,output);
    check(read(privatePool.Get())[0]==src[0] && read(privatePool.Get())[1]==patched[1],
          "primary copy: partial dispatch uses current row count; cached extra rows cannot repatch revoked slots");
    check(begin(3),"primary copy: discard resets all certificates");copy::endMap(native.Get(),3);
    check(copy::patches(native.Get(),frame).empty(),"primary copy: discard does not carry prior slot ownership");
    claim(src[0],frame);mapped[0][8]^=1;begin(4);forward(src,0,1);copy::endMap(native.Get(),4);
    check(copy::patches(native.Get(),frame).empty(),"primary copy: full destination mismatch refuses same-pose material aliases");mapped[0]=src[0];
    claim(src[0],frame);begin(5);src[0][9]^=1;forward(src,0,1);copy::endMap(native.Get(),5);
    check(copy::patches(native.Get(),frame).empty(),"primary copy: source mutation/reuse invalidates pending exact emission");src[0]=mapped[0];
    claim(src[0],frame);begin(6,D3D11_MAP_WRITE_NO_OVERWRITE,frame+1);forward(src,0,1,frame+1);copy::endMap(native.Get(),6);
    check(copy::patches(native.Get(),frame+1).empty(),"primary copy: older emission cannot become current map history");
    claim(src[0],frame);begin(7);forward(src,0,1);copy::invalidateMapped(reinterpret_cast<uintptr_t>(mapped));copy::endMap(native.Get(),7);
    check(copy::patches(native.Get(),frame).empty(),"primary copy: uncertain copier metadata invalidates entire mapped lease");
    claim(src[0],frame);check(!begin(8,D3D11_MAP_WRITE),"primary copy: arbitrary whole-buffer writes refuse ownership");forward(src,0,1);copy::endMap(native.Get(),8);
    check(copy::patches(native.Get(),frame).empty(),"primary copy: unknown map writes cannot leak previous certificates");
    check(!begin(8),"primary copy: replayed map sequence refused");
    claim(src[0],frame);begin(9);forward(src,0,1);forward(src,0,0);
    check(copy::g_pools.at(native.Get()).patches.size()==1,"primary copy: zero-count copy does not revoke a valid range");
    copy::invalidateMapped(0);copy::endMap(native.Get(),9);
    check(copy::patches(native.Get(),frame).empty(),"primary copy: unknown mapped-base fault revokes every active lease");
    claim(src[0],frame);begin(10);forward(src,0,1);
    copy::invalidateMapped(reinterpret_cast<uintptr_t>(src));copy::endMap(native.Get(),10);
    check(copy::patches(native.Get(),frame).size()==1,"primary copy: foreign non336 mapped resource preserves disjoint pool lease");
    ComPtr<ID3D11Buffer> secondNative;D3D11_BUFFER_DESC nd{};native->GetDesc(&nd);device->CreateBuffer(&nd,&initial,&secondNative);
    claim(src[0],frame);begin(11);copy::beginMap(secondNative.Get(),mapped,sizeof(mapped),336,D3D11_MAP_WRITE_DISCARD,1,frame);
    forward(src,0,1);copy::endMap(native.Get(),11);copy::endMap(secondNative.Get(),1);
    check(copy::patches(native.Get(),frame).empty() && copy::patches(secondNative.Get(),frame).empty(),"primary copy: ambiguous active mapped-base aliases refuse and invalidate both pools");
    copy::forget(secondNative.Get());
    // Destination clear must not erase the queued source of the next merge.
    copy::reset();Node cleared;Dictionary dictionary;dictionary.set(&cleared);cleared.item(0)=record(80);
    claim(cleared.item(0),frame);claim(src[0],frame);begin(1);forward(src,0,1);copy::endMap(native.Get(),1);
    claim(src[1],frame);const auto clearEpoch=copy::g_emissionEpoch;copy::invalidateDictionary(dictionary.ptr());
    check(!copy::g_emissions.count(reinterpret_cast<uintptr_t>(&cleared.item(0))) &&
          copy::g_emissions.count(reinterpret_cast<uintptr_t>(&src[1])) && copy::g_emissionEpoch==clearEpoch,
          "primary copy: destination dictionary clear preserves disjoint queued source owners");
    check(copy::patches(native.Get(),frame).size()==1,"primary copy: CPU node free preserves already-copied GPU certificates");
    mapped[0]=cleared.item(0);begin(2);forward(&cleared.item(0),0,1);copy::endMap(native.Get(),2);
    check(copy::patches(native.Get(),frame).empty(),"primary copy: same-address identical-payload reuse after free cannot inherit source identity");
    dictionary.set(nullptr);const auto calls=copy::stats().clearCalls;copy::invalidateDictionary(dictionary.ptr());
    check(copy::stats().clearCalls==calls+1 && copy::stats().clearFailed==0 && copy::g_emissions.count(reinterpret_cast<uintptr_t>(&src[1])),
          "primary copy: normal empty dictionary clear is witnessed without poisoning unrelated claims");
    dictionary.set(&cleared);uintptr_t cycle=cleared.ptr();std::memcpy(cleared.data,&cycle,8);copy::invalidateDictionary(dictionary.ptr());
    check(copy::g_emissions.empty() && copy::stats().clearFailed==1,"primary copy: malformed dictionary cycle invalidates whole pending source epoch");

    // Native partial-node merge: one prefix goes to the destination's last
    // free slot; two items shift left. Identical payloads cannot merge owners.
    copy::reset();Node destination,source;uintptr_t da[2]={destination.ptr(),destination.ptr()},sa[2]={source.ptr(),source.ptr()};
    destination.links(reinterpret_cast<uintptr_t>(da),7);source.links(reinterpret_cast<uintptr_t>(sa),3);
    for(unsigned i=0;i<3;++i){source.item(i)=record(float(30+i));claim(source.item(i),frame);}
    const auto s0=source.item(0),s1=source.item(1),s2=source.item(2);
    void* plan=copy::beginMergeOpaque(reinterpret_cast<uintptr_t>(da),reinterpret_cast<uintptr_t>(sa),frame);
    check(plan!=nullptr && copy::g_emissions.empty(),"primary copy: pre-merge stages claims then clears every original source address");
    destination.item(7)=s0;std::memmove(&source.item(0),&source.item(1),2*336);
    copy::endMergeOpaque(plan,true);
    check(copy::g_emissions.size()==3 && copy::g_emissions.at(reinterpret_cast<uintptr_t>(&source.item(0))).patch.native==s1 &&
          copy::g_emissions.at(reinterpret_cast<uintptr_t>(&source.item(1))).patch.native==s2,
          "primary copy: authoritative partial merge preserves distinct shifted identities");
    mapped[0]=source.item(0);mapped[1]=source.item(1);begin(1);forward(&source.item(0),0,2);copy::endMap(native.Get(),1);
    check(copy::patches(native.Get(),frame).size()==2,"primary copy: relocated merge claims join native copier destination");
    check(copy::g_emissions.count(reinterpret_cast<uintptr_t>(&source.item(2)))==0,"primary copy: shifted old source address cannot alias later reuse");
    source.links(reinterpret_cast<uintptr_t>(sa),9);
    check(copy::beginMergeOpaque(reinterpret_cast<uintptr_t>(da),reinterpret_cast<uintptr_t>(sa),frame)==nullptr && copy::g_emissions.empty(),
          "primary copy: invalid merge count invalidates whole pending-emission epoch");
    // Full nodes insert at HEAD. A later partial node must still fill the
    // preexisting partial TAIL, including the exhausted/freed-node case.
    copy::reset();Node tail,full,partial;uintptr_t d2[2]={tail.ptr(),tail.ptr()},s2a[2]={full.ptr(),partial.ptr()};
    tail.links(reinterpret_cast<uintptr_t>(d2),3);full.links(reinterpret_cast<uintptr_t>(s2a),8);partial.links(reinterpret_cast<uintptr_t>(s2a),2);
    uintptr_t next=partial.ptr(),prev=full.ptr();std::memcpy(full.data,&next,8);std::memcpy(partial.data+8,&prev,8);
    for(unsigned i=0;i<8;++i){full.item(i)=record(float(100+i));claim(full.item(i),frame);}
    for(unsigned i=0;i<2;++i){partial.item(i)=record(float(200+i));claim(partial.item(i),frame);}
    void* mixed=copy::beginMergeOpaque(reinterpret_cast<uintptr_t>(d2),reinterpret_cast<uintptr_t>(s2a),frame);
    check(mixed!=nullptr,"primary copy: partial-tail/full-head/partial-source merge planned");
    tail.item(3)=partial.item(0);tail.item(4)=partial.item(1);copy::endMergeOpaque(mixed,true);
    check(copy::g_emissions.size()==10 && copy::g_emissions.count(reinterpret_cast<uintptr_t>(&tail.item(3))) &&
          copy::g_emissions.count(reinterpret_cast<uintptr_t>(&tail.item(4))) && !copy::g_emissions.count(reinterpret_cast<uintptr_t>(&partial.item(0))),
          "primary copy: full head preserves old tail and exhausted partial claims relocate exactly");
    copy::reset();Node planned;Dictionary plannedDictionary;plannedDictionary.set(&planned);planned.item(0)=record(400);claim(planned.item(0),frame);
    uintptr_t emptyDestination[2];emptyDestination[0]=emptyDestination[1]=reinterpret_cast<uintptr_t>(emptyDestination);
    void* pending=copy::beginMergeOpaque(reinterpret_cast<uintptr_t>(emptyDestination),plannedDictionary.anchor(),frame);
    claim(src[1],frame);copy::invalidateDictionary(plannedDictionary.ptr());copy::endMergeOpaque(pending,true);
    check(!copy::g_emissions.count(reinterpret_cast<uintptr_t>(&planned.item(0))) && copy::g_emissions.count(reinterpret_cast<uintptr_t>(&src[1])),
          "primary copy: node free revokes detached overlapping merge plan without erasing unrelated source owners");
    claim(src[0],frame);const auto epoch=copy::g_emissionEpoch;copy::invalidateEmission(0);
    check(copy::g_emissions.empty() && copy::g_emissionEpoch!=epoch,"primary copy: unknown append invalidates source epoch before allocator reuse");
    copy::reset();std::vector<Record> many(copy::kMaxEmissions+1,record(300));
    for(auto& r:many)claim(r,frame);
    check(copy::g_emissions.empty() && copy::stats().overflows==1 && !claim(src[0],frame),"primary copy: capacity overflow poisons all same-frame claims");
    copy::forget(native.Get());copy::reset();runObservers(device,check);
}
}
