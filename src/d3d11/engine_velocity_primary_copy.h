#pragma once
// Private-pool history: identity comes from an owned emission and the native
// copier's source address, never from a pose/content match. Source relocation
// or reuse must invalidate/transfer its claim before another copier can see it.
#include "engine_velocity_emit.h"
#include <d3d11.h>
#include "engine_velocity_primary_copy_shader.h"
#include "temporal_shader_bytecode.h"
#include <wrl/client.h>
#include <array>
#include <unordered_map>
#include <vector>
#include <mutex>
#include <memory>
#include <unordered_set>

namespace edvr { namespace engine_velocity_primary_copy {
namespace emit=engine_velocity_emit;
using Microsoft::WRL::ComPtr;
constexpr uint32_t kWords=84,kBytes=336,kMaxEmissions=16384,kMaxPatches=16384,kMaxPools=32;
struct Patch { uint32_t slot=0,marker=0; std::array<uint32_t,kWords> native{}; emit::Pose previous{}; };
static_assert(sizeof(Patch)==364,"GPU structured patch layout");
struct Emission { Patch patch; uint32_t frame=0; };
struct Pool {
    ComPtr<ID3D11Buffer> held;
    uintptr_t base=0; uint32_t bytes=0,frame=0;
    uint64_t sequence=0,generation=0;
    bool active=false,valid=false;
    std::unordered_map<uint32_t,Patch> patches;
};
enum class ScatterFailure { None,Arguments,Descriptor,Bound,Shader,Upload,Map,View,Counts,Unexpected };
inline const char* scatterFailureName(ScatterFailure f) {
    constexpr const char* names[]={"none","arguments","descriptor","clone_bound","shader","upload","map","view","counts","unexpected"};
    return names[static_cast<unsigned>(f)];
}
struct Stats { ScatterFailure lastScatterFailure=ScatterFailure::None; uint64_t emissions=0,copies=0,joined=0,declined=0,invalidated=0,overflows=0,scatterBatches=0,scatterRows=0,scatterFailed=0,scatterEmpty=0,sourceResets=0,copierNoLease=0,copierAmbiguous=0,copierInvalidRange=0,mergePlans=0,mergeFailed=0,clearCalls=0,clearedClaims=0,clearFailed=0;
    uint64_t mergeCalls=0,mergeWithoutClaims=0,clearNoClaims=0,mergeNodes=0,clearNodes=0,activeClaims=0,activePlans=0;
};
inline std::mutex g_mutex,g_gpuMutex;
inline std::unordered_map<uintptr_t,Emission> g_emissions;
inline std::unordered_map<ID3D11Buffer*,Pool> g_pools;
inline Stats g_stats;
inline uint32_t g_overflowFrame=~0u;
inline uint32_t g_emissionFrame=~0u;
inline uint64_t g_emissionEpoch=1;
inline emit::Pose current(const uint32_t* r) { return emit::Pose{{r[4],r[5],r[6],r[2],r[3]}}; }
inline void overflow(uint32_t frame) {
    g_overflowFrame=frame;++g_stats.overflows;g_emissions.clear();++g_emissionEpoch;
    for(auto& entry:g_pools)entry.second.patches.clear();
}
inline void invalidateEmission(uintptr_t source) noexcept {
    std::lock_guard<std::mutex> lock(g_mutex);
    if(source)g_emissions.erase(source);else {g_emissions.clear();++g_emissionEpoch;++g_stats.invalidated;++g_stats.sourceResets;}
}
inline void invalidateMapped(uintptr_t base) noexcept {
    std::lock_guard<std::mutex> lock(g_mutex);
    for(auto& entry:g_pools)if(entry.second.active && (!base || entry.second.base==base)) {
        entry.second.patches.clear();entry.second.valid=false;++g_stats.invalidated;
    }
}
inline bool recordEmission(uintptr_t source,const uint32_t native[kWords],const emit::Pose& previous,
                           uint32_t marker,uint32_t frame) noexcept {
    std::lock_guard<std::mutex> lock(g_mutex);
    if(g_emissionFrame!=frame){g_emissions.clear();g_emissionFrame=frame;++g_emissionEpoch;++g_stats.sourceResets;}
    if(!source || !native || g_overflowFrame==frame)return false;
    const emit::Pose now=current(native);
    const bool joined=marker==(emit::kJoined^emit::markerHash(now,previous,frame));
    const bool masked=marker==(emit::kMasked^emit::markerHash(now,previous,frame));
    // Current-only native rigid records; all second-block bytes stay native.
    if(native[0] || native[1]!=0x3F800000u || native[77]!=native[1] ||
       native[73]!=native[4] || native[74]!=native[5] || native[75]!=native[6] ||
       native[78]!=native[2] || native[79]!=native[3] || (!joined && !masked) || (masked && previous!=now)) {
        g_emissions.erase(source);++g_stats.declined;return false;
    }
    try {
        auto old=g_emissions.find(source);
        if(old!=g_emissions.end() && old->second.frame==frame) {
            const auto& p=old->second.patch;
            if(p.marker!=marker || p.previous!=previous || std::memcmp(p.native.data(),native,kBytes)) {
                g_emissions.erase(old);++g_stats.declined;return false;
            }
            return true;
        }
        if(g_emissions.size()>=kMaxEmissions){overflow(frame);return false;}
        Emission e;e.frame=frame;e.patch.marker=marker;e.patch.previous=previous;
        std::memcpy(e.patch.native.data(),native,kBytes);g_emissions[source]=e;++g_stats.emissions;
        return true;
    } catch(...) {overflow(frame);return false;}
}
inline bool beginMap(ID3D11Buffer* resource,void* base,uint32_t bytes,uint32_t stride,D3D11_MAP type,
                     uint64_t sequence,uint32_t frame) noexcept {
    std::lock_guard<std::mutex> lock(g_mutex);
    if(!resource)return false;
    try {
        auto old=g_pools.find(resource);
        if(old==g_pools.end() && g_pools.size()>=kMaxPools){overflow(frame);return false;}
        Pool& p=g_pools[resource];if(!p.held)p.held=resource;
        if(p.active || sequence<=p.sequence){p.patches.clear();p.valid=false;p.active=false;++g_stats.declined;return false;}
        if(type!=D3D11_MAP_WRITE_NO_OVERWRITE || p.frame!=frame){p.patches.clear();++p.generation;}
        p.base=reinterpret_cast<uintptr_t>(base);p.bytes=bytes;p.frame=frame;p.sequence=sequence;p.active=true;
        p.valid=base && stride==kBytes && bytes>=kBytes && bytes%kBytes==0 &&
            (type==D3D11_MAP_WRITE_DISCARD || type==D3D11_MAP_WRITE_NO_OVERWRITE) && g_overflowFrame!=frame;
        if(!p.valid){p.patches.clear();++g_stats.declined;}
        return p.valid;
    } catch(...) {overflow(frame);return false;}
}
inline void copier(uintptr_t base,uint32_t stride,uintptr_t source,uint32_t firstSlot,uint32_t count,uint32_t frame) noexcept {
    std::lock_guard<std::mutex> lock(g_mutex);++g_stats.copies;
    if(!count)return;
    Pool* pool=nullptr;
    uint32_t matches=0;
    for(auto& entry:g_pools)if(entry.second.active && entry.second.base==base) {
        pool=&entry.second;++matches;
    }
    if(matches>1){++g_stats.copierAmbiguous;for(auto& entry:g_pools)if(entry.second.active && entry.second.base==base){entry.second.patches.clear();entry.second.valid=false;++g_stats.invalidated;}pool=nullptr;}
    if(!matches)++g_stats.copierNoLease;
    const bool range=pool && stride==kBytes && count && firstSlot<=pool->bytes/kBytes && count<=pool->bytes/kBytes-firstSlot;
    if(pool) {
        if(!range){pool->patches.clear();pool->valid=false;++g_stats.invalidated;++g_stats.copierInvalidRange;}
        else for(uint32_t i=0;i<count;++i)g_stats.invalidated+=pool->patches.erase(firstSlot+i);
    }
    // Claims are consumed even on a refused copy: an allocator reuse cannot
    // inherit an emission after that source item has been drained.
    if(count>kMaxPatches || !source || source>UINTPTR_MAX-uint64_t(count)*kBytes) {
        g_emissions.clear();++g_emissionEpoch;++g_stats.declined;return;
    }
    for(uint32_t i=0;i<count;++i) {
        auto e=g_emissions.find(source+uintptr_t(i)*kBytes);if(e==g_emissions.end())continue;
        const Emission emission=e->second;g_emissions.erase(e);
        if(!range || !pool->active || !pool->valid || pool->frame!=frame || emission.frame!=frame || g_overflowFrame==frame) {++g_stats.declined;continue;}
        std::array<uint32_t,kWords> src{},dst{};
        if(!emit::read(source+uintptr_t(i)*kBytes,src.data(),kBytes) ||
           !emit::read(base+uintptr_t(firstSlot+i)*kBytes,dst.data(),kBytes) ||
           src!=emission.patch.native || dst!=emission.patch.native) {++g_stats.declined;continue;}
        try {
            if(pool->patches.size()>=kMaxPatches){overflow(frame);return;}
            Patch patch=emission.patch;patch.slot=firstSlot+i;pool->patches[patch.slot]=patch;++g_stats.joined;
        } catch(...) {overflow(frame);return;}
    }
}
inline void endMap(ID3D11Buffer* resource,uint64_t sequence) noexcept {
    std::lock_guard<std::mutex> lock(g_mutex);auto i=g_pools.find(resource);if(i==g_pools.end())return;
    Pool& p=i->second;
    if(!p.active || p.sequence!=sequence){p.patches.clear();p.valid=false;++g_stats.declined;}
    p.active=false;
}
inline void forget(ID3D11Buffer* resource) noexcept {
    std::lock_guard<std::mutex> lock(g_mutex);g_pools.erase(resource);
}
inline Stats stats();
inline std::vector<Patch> patches(ID3D11Buffer* resource,uint32_t frame) {
    std::lock_guard<std::mutex> lock(g_mutex);std::vector<Patch> out;
    const auto i=g_pools.find(resource);
    if(i==g_pools.end() || i->second.active || !i->second.valid || i->second.frame!=frame || g_overflowFrame==frame)return out;
    out.reserve(i->second.patches.size());for(const auto& p:i->second.patches)out.push_back(p.second);return out;
}
inline bool privateBound(ID3D11DeviceContext* ctx,ID3D11Buffer* buffer) {
    ID3D11ShaderResourceView* views[D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT]{};
    auto check=[&](auto getter){
        (ctx->*getter)(0,D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT,views);bool found=false;
        for(auto*& v:views)if(v){ComPtr<ID3D11Resource> r;v->GetResource(&r);found=found || r.Get()==buffer;v->Release();v=nullptr;}
        return found;
    };
    if(check(&ID3D11DeviceContext::CSGetShaderResources) || check(&ID3D11DeviceContext::VSGetShaderResources) ||
       check(&ID3D11DeviceContext::PSGetShaderResources) || check(&ID3D11DeviceContext::GSGetShaderResources) ||
       check(&ID3D11DeviceContext::HSGetShaderResources) || check(&ID3D11DeviceContext::DSGetShaderResources))return true;
    ID3D11UnorderedAccessView* uavs[D3D11_PS_CS_UAV_REGISTER_COUNT]{};
    ctx->CSGetUnorderedAccessViews(0,D3D11_PS_CS_UAV_REGISTER_COUNT,uavs);bool found=false;
    for(auto* v:uavs)if(v){ComPtr<ID3D11Resource> r;v->GetResource(&r);found=found || r.Get()==buffer;v->Release();}
    ctx->OMGetRenderTargetsAndUnorderedAccessViews(0,nullptr,nullptr,0,D3D11_PS_CS_UAV_REGISTER_COUNT,uavs);
    for(auto* v:uavs)if(v){ComPtr<ID3D11Resource> r;v->GetResource(&r);found=found || r.Get()==buffer;v->Release();}
    return found;
}
struct MergeTransfer { uintptr_t target=0; Emission emission; };
struct MergePlan { std::vector<MergeTransfer> transfers; std::vector<uintptr_t> addresses; uint64_t epoch=0; uint32_t frame=0; bool invalid=false; };
inline std::unordered_set<MergePlan*> g_mergePlans;
inline Stats stats() {
    std::lock_guard<std::mutex> lock(g_mutex);Stats result=g_stats;
    result.activeClaims=g_emissions.size();result.activePlans=g_mergePlans.size();return result;
}
inline void invalidateClaims() {g_emissions.clear();++g_emissionEpoch;++g_stats.declined;++g_stats.mergeFailed;}
// 36819D0 frees every list node in this typed336 dictionary. Destination
// clear precedes a merge of unrelated source collections, so only claims in
// nodes actually being freed may be revoked on a successful bounded walk.
template<bool SkipNoClaims> inline void invalidateDictionaryImpl(uintptr_t dictionary) noexcept {
    std::lock_guard<std::mutex> lock(g_mutex);++g_stats.clearCalls;
    // Only source claims and detached merge plans consume this walk. Copied
    // GPU-pool certificates are independent and must stay intact. Future
    // emissions register fresh ownership after this lock is released.
    if constexpr(SkipNoClaims) {
        if(g_emissions.empty() && g_mergePlans.empty()){++g_stats.clearNoClaims;return;}
    }
    auto fail=[](){g_emissions.clear();++g_emissionEpoch;++g_stats.sourceResets;++g_stats.clearFailed;};
    try {
        uintptr_t buckets=0;uint64_t count=0;
        if(!dictionary || dictionary>UINTPTR_MAX-0x20 ||
           !emit::read(dictionary+0x10,&buckets,8) || !emit::read(dictionary+0x18,&count,8) ||
           count>kMaxEmissions || (count && (!buckets || buckets>UINTPTR_MAX-count*8))) {fail();return;}
        std::unordered_set<uintptr_t> entries,nodes,addresses;
        for(uint64_t b=0;b<count;++b){
            const uintptr_t sentinel=buckets+uintptr_t(b)*8;uintptr_t entry=0;
            if(!emit::read(sentinel,&entry,8)){fail();return;}
            while(entry!=sentinel){
                if(!entry || entry>UINTPTR_MAX-0x38 || entries.size()>=kMaxEmissions || !entries.insert(entry).second){fail();return;}
                uintptr_t nextEntry=0,links[2]{};const uintptr_t anchor=entry+0x28;
                if(!emit::read(entry,&nextEntry,8) || !emit::read(anchor,links,16)){fail();return;}
                uintptr_t node=links[0],previous=anchor;
                while(node!=anchor){
                    uint64_t header[4]{};
                    if(!node || node>UINTPTR_MAX-(emit::kNodeRecords+8*kBytes) || nodes.size()>=kMaxEmissions/8 ||
                       !nodes.insert(node).second || !emit::read(node,header,32) || header[1]!=previous || header[3]>8){fail();return;}
                    ++g_stats.clearNodes;
                    for(uint32_t i=0;i<8;++i)addresses.insert(node+emit::kNodeRecords+uintptr_t(i)*kBytes);
                    previous=node;node=uintptr_t(header[0]);
                }
                if(links[1]!=previous){fail();return;}
                entry=nextEntry;
            }
        }
        for(auto address:addresses){const auto n=g_emissions.erase(address);g_stats.invalidated+=n;g_stats.clearedClaims+=n;}
        // A plan owns detached claims between the two native merge callbacks.
        // Drop only plans touching this clear, never unrelated live owners.
        for(auto* plan:g_mergePlans)for(auto address:plan->addresses)if(addresses.count(address)){plan->invalid=true;break;}
    }catch(...){fail();}
}
inline void invalidateDictionary(uintptr_t dictionary) noexcept {invalidateDictionaryImpl<true>(dictionary);}
// 434E740: full eight-item nodes splice at the HEAD, preserving a nonempty
// destination tail; partial nodes fill the
// destination tail, then shift any remainder within their original node.
// Claims are staged before ALL source slots/overwritten destinations clear.
inline void* beginMergeOpaque(uintptr_t destination,uintptr_t source,uint32_t frame) noexcept {
    std::lock_guard<std::mutex> lock(g_mutex);
    ++g_stats.mergeCalls;
    // Even an empty plan must retain its original clear/unwind semantics:
    // a producer may register a claim between this callback and its end.
    if(g_emissions.empty() && g_mergePlans.empty())++g_stats.mergeWithoutClaims;
    try {
        uintptr_t dstLinks[2]{},srcLinks[2]{};
        if(!destination || !source || destination==source ||
           !emit::read(destination,dstLinks,sizeof(dstLinks)) || !emit::read(source,srcLinks,sizeof(srcLinks))) {invalidateClaims();return nullptr;}
        auto plan=std::make_unique<MergePlan>();plan->epoch=g_emissionEpoch;plan->frame=frame;
        if(g_mergePlans.size()>=kMaxPools){invalidateClaims();return nullptr;}
        std::vector<uintptr_t> clear;
        std::unordered_set<uintptr_t> visited;
        uintptr_t tail=dstLinks[1],node=srcLinks[0],previous=source;
        uint64_t tailCount=0;
        if(dstLinks[0]==destination) {
            if(tail!=destination){invalidateClaims();return nullptr;}
        } else {
            uintptr_t next=0;
            if(!tail || !emit::read(tail,&next,sizeof(next)) || next!=destination ||
               !emit::read(tail+emit::kNodeCount,&tailCount,8) || tailCount>8) {invalidateClaims();return nullptr;}
        }
        while(node!=source) {
            uint64_t header[4]{};
            if(!node || node==destination || node==tail || node>UINTPTR_MAX-(emit::kNodeRecords+8*kBytes) ||
               visited.size()>=kMaxEmissions/8 || !visited.insert(node).second ||
               !emit::read(node,header,sizeof(header)) || header[1]!=previous || header[3]==0 || header[3]>8) {invalidateClaims();return nullptr;}
            ++g_stats.mergeNodes;
            const uint32_t count=uint32_t(header[3]);
            uint32_t fill=0;
            if(count!=8 && tail!=destination && tailCount!=8)fill=std::min(count,uint32_t(8-tailCount));
            for(uint32_t i=0;i<8;++i)clear.push_back(node+emit::kNodeRecords+uintptr_t(i)*kBytes);
            for(uint32_t i=0;i<fill;++i)clear.push_back(tail+emit::kNodeRecords+uintptr_t(tailCount+i)*kBytes);
            for(uint32_t i=0;i<count;++i) {
                const uintptr_t old=node+emit::kNodeRecords+uintptr_t(i)*kBytes;
                const auto claim=g_emissions.find(old);
                if(claim==g_emissions.end() || claim->second.frame!=frame)continue;
                std::array<uint32_t,kWords> native{};
                if(!emit::read(old,native.data(),kBytes)){invalidateClaims();return nullptr;}
                if(native!=claim->second.patch.native)continue;
                const uintptr_t target=i<fill?tail+emit::kNodeRecords+uintptr_t(tailCount+i)*kBytes:
                    node+emit::kNodeRecords+uintptr_t(i-fill)*kBytes;
                plan->transfers.push_back({target,claim->second});
            }
            tailCount+=fill;
            if(count==8){if(tail==destination){tail=node;tailCount=8;}}
            else if(count>fill){tail=node;tailCount=count-fill;}
            previous=node;node=uintptr_t(header[0]);
        }
        if(srcLinks[1]!=previous){invalidateClaims();return nullptr;}
        for(uintptr_t address:clear)g_emissions.erase(address);
        plan->addresses=std::move(clear);g_mergePlans.insert(plan.get());
        ++g_stats.mergePlans;
        return plan.release();
    } catch(...) {invalidateClaims();return nullptr;}
}
inline void endMergeOpaque(void* opaque,bool completed) noexcept {
    std::unique_ptr<MergePlan> plan(static_cast<MergePlan*>(opaque));if(!plan)return;
    std::lock_guard<std::mutex> lock(g_mutex);
    g_mergePlans.erase(plan.get());
    if(plan->invalid || plan->epoch!=g_emissionEpoch){++g_stats.mergeFailed;return;}
    if(!completed){invalidateClaims();return;}
    try {
        for(const auto& move:plan->transfers) {
            std::array<uint32_t,kWords> native{};
            if(!emit::read(move.target,native.data(),kBytes) || native!=move.emission.patch.native) {
                g_emissions.erase(move.target);++g_stats.declined;continue;
            }
            // A concurrently re-registered target is not this merge's owner.
            if(g_emissions.find(move.target)!=g_emissions.end()){invalidateClaims();return;}
            if(g_emissions.size()>=kMaxEmissions){overflow(plan->frame);return;}
            g_emissions.emplace(move.target,move.emission);
        }
    } catch(...) {invalidateClaims();}
}
// Owned by one eye/source private pool, not by the shared compute cache:
// left/right alternate every frame. Held resource identity prevents an old
// view being reused for a different allocation at the same address.
struct OutputCache {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11Buffer> pool;
    ComPtr<ID3D11UnorderedAccessView> view;
};
struct GpuCache {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11ComputeShader> shader;
    ComPtr<ID3D11Buffer> upload,count;
    ComPtr<ID3D11ShaderResourceView> input;
    uint32_t capacity=0;
};
inline GpuCache g_gpu;
inline bool apply(ID3D11DeviceContext* ctx,ID3D11Buffer* privatePool,ID3D11Buffer* sourcePool,uint32_t frame,OutputCache& output) noexcept {
    auto fail=[](ScatterFailure why){std::lock_guard<std::mutex> lock(g_mutex);++g_stats.scatterFailed;g_stats.lastScatterFailure=why;return false;};
    if(!ctx || !privatePool || !sourcePool || privatePool==sourcePool)return fail(ScatterFailure::Arguments);
    try {
        const auto work=patches(sourcePool,frame);if(work.empty()){std::lock_guard<std::mutex> lock(g_mutex);++g_stats.scatterEmpty;return true;}
        std::lock_guard<std::mutex> gpu(g_gpuMutex);
        D3D11_BUFFER_DESC d{},native{};privatePool->GetDesc(&d);sourcePool->GetDesc(&native);
        if(d.ByteWidth!=native.ByteWidth || d.StructureByteStride!=kBytes ||
           !(d.BindFlags&D3D11_BIND_UNORDERED_ACCESS) || !(d.MiscFlags&D3D11_RESOURCE_MISC_BUFFER_STRUCTURED))return fail(ScatterFailure::Descriptor);
        if(privateBound(ctx,privatePool))return fail(ScatterFailure::Bound);
        ComPtr<ID3D11Device> device;ctx->GetDevice(&device);
        if(g_gpu.device.Get()!=device.Get()){g_gpu={};g_gpu.device=device;}
        if(output.device.Get()!=device.Get() || output.pool.Get()!=privatePool)output={};
        if(!g_gpu.shader && FAILED(device->CreateComputeShader(kEnginePrimaryCopyScatterBytecode,sizeof(kEnginePrimaryCopyScatterBytecode),nullptr,&g_gpu.shader)))return fail(ScatterFailure::Shader);
        if(g_gpu.capacity<work.size()) {
            g_gpu.upload.Reset();g_gpu.input.Reset();g_gpu.capacity=0;
            uint32_t capacity=64;while(capacity<work.size())capacity*=2;
            D3D11_BUFFER_DESC upload{};upload.ByteWidth=capacity*sizeof(Patch);upload.BindFlags=D3D11_BIND_SHADER_RESOURCE;
            upload.Usage=D3D11_USAGE_DYNAMIC;upload.CPUAccessFlags=D3D11_CPU_ACCESS_WRITE;
            upload.StructureByteStride=sizeof(Patch);upload.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
            if(FAILED(device->CreateBuffer(&upload,nullptr,&g_gpu.upload)) ||
               FAILED(device->CreateShaderResourceView(g_gpu.upload.Get(),nullptr,&g_gpu.input)))return fail(ScatterFailure::Upload);
            g_gpu.capacity=capacity;
        }
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if(FAILED(ctx->Map(g_gpu.upload.Get(),0,D3D11_MAP_WRITE_DISCARD,0,&mapped)))return fail(ScatterFailure::Map);
        std::memcpy(mapped.pData,work.data(),work.size()*sizeof(Patch));ctx->Unmap(g_gpu.upload.Get(),0);
        if(!output.view) {
            ComPtr<ID3D11UnorderedAccessView> view;
            if(FAILED(device->CreateUnorderedAccessView(privatePool,nullptr,&view)))return fail(ScatterFailure::View);
            output.device=device;output.pool=privatePool;output.view=std::move(view);
        }
        const uint32_t counts[4]={UINT(work.size()),d.ByteWidth/kBytes,0,0};
        if(!g_gpu.count){D3D11_BUFFER_DESC cb{};cb.ByteWidth=16;cb.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
            if(FAILED(device->CreateBuffer(&cb,nullptr,&g_gpu.count)))return fail(ScatterFailure::Counts);}
        ctx->UpdateSubresource(g_gpu.count.Get(),0,nullptr,counts,0,0);
        ComPtr<ID3D11ComputeShader> savedShader;ID3D11ClassInstance* classes[256]{};UINT numClasses=256;
        ctx->CSGetShader(&savedShader,classes,&numClasses);
        ComPtr<ID3D11ShaderResourceView> savedInput;ctx->CSGetShaderResources(0,1,&savedInput);
        ComPtr<ID3D11UnorderedAccessView> savedOutput;ctx->CSGetUnorderedAccessViews(0,1,&savedOutput);
        ComPtr<ID3D11Buffer> savedCount;ctx->CSGetConstantBuffers(0,1,&savedCount);
        ctx->CSSetShader(g_gpu.shader.Get(),nullptr,0);ctx->CSSetShaderResources(0,1,g_gpu.input.GetAddressOf());
        ctx->CSSetUnorderedAccessViews(0,1,output.view.GetAddressOf(),nullptr);ctx->CSSetConstantBuffers(0,1,g_gpu.count.GetAddressOf());
        ctx->Dispatch((UINT(work.size())+63)/64,1,1);
        ID3D11UnorderedAccessView* empty=nullptr;ctx->CSSetUnorderedAccessViews(0,1,&empty,nullptr);
        ctx->CSSetShaderResources(0,1,savedInput.GetAddressOf());ctx->CSSetUnorderedAccessViews(0,1,savedOutput.GetAddressOf(),nullptr);
        ctx->CSSetConstantBuffers(0,1,savedCount.GetAddressOf());ctx->CSSetShader(savedShader.Get(),classes,numClasses);
        for(UINT i=0;i<numClasses;++i)if(classes[i])classes[i]->Release();
        std::lock_guard<std::mutex> lock(g_mutex);++g_stats.scatterBatches;g_stats.scatterRows+=work.size();return true;
    } catch(...) {return fail(ScatterFailure::Unexpected);}
}
inline void reset() noexcept {
    std::lock_guard<std::mutex> gpu(g_gpuMutex);g_gpu={};
    std::lock_guard<std::mutex> lock(g_mutex);g_emissions.clear();g_pools.clear();g_mergePlans.clear();g_stats={};g_overflowFrame=g_emissionFrame=~0u;++g_emissionEpoch;
}
} }
