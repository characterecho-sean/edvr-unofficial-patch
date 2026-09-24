#include "flat_compute_capture.h"
#include "flat_compute_model.h"
#include "flat_compute_readback.h"
#include "exposure_fix.h"
#include "../common/log.h"
#include <wrl/client.h>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <memory>
#include <new>

namespace edvr {
namespace {
using Microsoft::WRL::ComPtr;
constexpr uint32_t kRecords = 64, kDraws = 8, kBounds = 4;
struct View {
    const void* view = nullptr; const void* resource = nullptr;
    uint32_t dimension = 0, format = 0, width = 0, height = 0, stride = 0;
    uint32_t first = 0, elements = 0, flags = 0, mip = 0, mips = 0;
};
struct Constants {
    const void* resource = nullptr;
    uint32_t width = 0, copied = 0, writeSequence = 0;
    uint64_t writeEpoch = 0;
    uint8_t bytes[480]{};
    const char* status = "unbound";
    bool pending = false;
};
struct Dispatch {
    uint64_t shader = 0; uint32_t sequence = 0, xyz[3]{};
    const void* arguments = nullptr; uint32_t argumentOffset = 0;
    View srv[12]{}, uav[3]{}; Constants cb;
};
struct Draw {
    uint64_t vs = 0, ps = 0; uint32_t sequence = 0;
    const void* depth = nullptr; const void* color = nullptr;
    const void* constants = nullptr; uint32_t width = 0;
    uint32_t rowIndex[4]{}, valid = 0, writeSequence[4]{};
    uint64_t writeEpoch[4]{}; uint8_t rows[4][16]{};
    View views[2]{}; bool pixel = false;
};
struct Bounds {
    uint32_t dispatch = 0, sequence = 0; View view;
    uint32_t counts[3]{}; uint64_t offsets[12]{};
    bool sparse = false, completed = false;
    std::unique_ptr<uint8_t[]> waiting;
    uint32_t waitingBytes = 0;
};
struct Sample {
    uint32_t id = 0, used = 0, perHash[8]{}, dropped[8]{};
    uint32_t identityQueries = 0, identityDrops = 0, otherDispatches = 0, draws = 0, drawDrops = 0, bounds = 0, boundsDrops = 0;
    uint32_t drawsByRole[2]{}, drawDropsByRole[2]{}, boundsByRole[2]{}, boundsDropsByRole[2]{};
    uint16_t cbFallback = 0;
    uint64_t epoch = 0, present = 0;
    Dispatch records[kRecords]{}; Draw graphics[kDraws]{}; Bounds grid[kBounds]{};
    FlatMonoFrame mono{}; bool finished = false;
};
struct State {
    ComPtr<ID3D11DeviceContext> ctx;
    FlatComputeAttempts attempts;
    uint64_t present = 0, nextMs = 0;
    bool manual = false;
    Sample samples[2]{};
};
// Deliberately process-lifetime: DLL teardown may be under the loader lock.
// Pending objects are retired only on the owning Present thread, never in Stop.
State& state() { static State* s = new State; return *s; }
std::atomic<bool> active{false};
Sample* current() {
    State& s = state(); const uint32_t n = flatComputeAttempt(s.attempts);
    return n ? &s.samples[n - 1] : nullptr;
}
uint32_t hashBytes(const void* ptr, uint32_t bytes) {
    const auto* p = static_cast<const uint8_t*>(ptr); uint32_t h = 2166136261u;
    for (uint32_t i = 0; i < bytes; ++i) h = (h ^ p[i]) * 16777619u;
    return h;
}
void resourceDesc(ID3D11Resource* res, View& v) {
    if (!res) return;
    v.resource = res;
    D3D11_RESOURCE_DIMENSION kind{}; res->GetType(&kind);
    if (kind == D3D11_RESOURCE_DIMENSION_BUFFER) {
        D3D11_BUFFER_DESC d{}; static_cast<ID3D11Buffer*>(res)->GetDesc(&d);
        v.width = d.ByteWidth; v.stride = d.StructureByteStride;
    } else if (kind == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
        D3D11_TEXTURE2D_DESC d{}; static_cast<ID3D11Texture2D*>(res)->GetDesc(&d);
        v.width = d.Width; v.height = d.Height;
    }
}
View describe(ID3D11ShaderResourceView* view) {
    View v{}; if (!view) return v; v.view = view;
    D3D11_SHADER_RESOURCE_VIEW_DESC d{}; view->GetDesc(&d);
    v.dimension = d.ViewDimension; v.format = d.Format;
    if (d.ViewDimension == D3D11_SRV_DIMENSION_BUFFER) { v.first = d.Buffer.FirstElement; v.elements = d.Buffer.NumElements; }
    if (d.ViewDimension == D3D11_SRV_DIMENSION_BUFFEREX) { v.first = d.BufferEx.FirstElement; v.elements = d.BufferEx.NumElements; v.flags = d.BufferEx.Flags; }
    if (d.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2D) { v.mip = d.Texture2D.MostDetailedMip; v.mips = d.Texture2D.MipLevels; }
    ComPtr<ID3D11Resource> res; view->GetResource(&res); resourceDesc(res.Get(), v); return v;
}
View describe(ID3D11UnorderedAccessView* view) {
    View v{}; if (!view) return v; v.view = view;
    D3D11_UNORDERED_ACCESS_VIEW_DESC d{}; view->GetDesc(&d);
    v.dimension = d.ViewDimension; v.format = d.Format;
    if (d.ViewDimension == D3D11_UAV_DIMENSION_BUFFER) { v.first = d.Buffer.FirstElement; v.elements = d.Buffer.NumElements; v.flags = d.Buffer.Flags; }
    if (d.ViewDimension == D3D11_UAV_DIMENSION_TEXTURE2D) v.mip = d.Texture2D.MipSlice;
    ComPtr<ID3D11Resource> res; view->GetResource(&res); resourceDesc(res.Get(), v); return v;
}
void printView(const Sample& s, uint32_t record, const char* stage, uint32_t slot, const View& v) {
    Log::get().note("flat compute view sample=%u frame=%llu record=%u stage=%s slot=%u view=%p resource=%p dimension=%u fmt=%u size=%ux%u stride=%u first=%u elements=%u flags=%u mip=%u mips=%u; actual getter, identities snapshot-local",
        s.id, static_cast<unsigned long long>(s.present), record, stage, slot, v.view, v.resource,
        v.dimension, v.format, v.width, v.height, v.stride, v.first, v.elements, v.flags, v.mip, v.mips);
}
void printRows(const Sample& s, uint32_t record, const char* stage, uint32_t firstRow, const uint8_t* bytes, uint32_t count) {
    for (uint32_t row = 0; row < count; row += 4) {
        char bits[160]{}; size_t used = 0; const uint32_t n = std::min(4u, count - row);
        for (uint32_t j = 0; j < n; ++j) {
            uint32_t raw[4]; std::memcpy(raw, bytes + (row+j)*16, 16);
            const int len = std::snprintf(bits+used, sizeof(bits)-used, "%s%08X %08X %08X %08X", j ? " | " : "", raw[0],raw[1],raw[2],raw[3]);
            if (len < 0 || size_t(len) >= sizeof(bits)-used) break;
            used += size_t(len);
        }
        Log::get().note("flat compute payload sample=%u frame=%llu record=%u stage=%s rows=%u..%u bits-u32=%s",
            s.id, static_cast<unsigned long long>(s.present), record, stage, firstRow+row, firstRow+row+n-1, bits);
    }
}
void printConstants(const Sample& s, uint32_t index) {
    const auto& r = s.records[index]; const auto& c = r.cb;
    Log::get().note("flat compute constants sample=%u frame=%llu record=%u CSb0=%p width=%u copied=%u hash=%08X status=%s write-epoch=%llu write-q=%u dispatch-epoch=%llu dispatch-q=%u",
        s.id, static_cast<unsigned long long>(s.present), index, c.resource,c.width,c.copied,
        c.copied ? hashBytes(c.bytes,c.copied) : 0, c.status,
        static_cast<unsigned long long>(c.writeEpoch), c.writeSequence,
        static_cast<unsigned long long>(s.epoch), r.sequence);
    if (c.copied) printRows(s,index,"CSb0",0,c.bytes,c.copied/16);
}
bool indices(const Constants& c, Bounds& b) {
    if (c.copied < 32) return false;
    std::memcpy(b.counts,c.bytes+16,12);
    return flatComputeCornerOffsets(b.counts,b.view.first,b.view.elements,b.view.width,b.view.stride,b.offsets) == FlatComputeRefusal::None;
}
void printBounds(Sample& s, uint32_t slot, const uint8_t* data, uint32_t size) {
    Bounds& b = s.grid[slot]; b.completed = true;
    if (!b.sparse && !indices(s.records[b.dispatch].cb,b)) {
        Log::get().note("flat compute bounds sample=%u frame=%llu job=%u status=grid-indices-unavailable-after-readback bytes=%u",s.id,static_cast<unsigned long long>(s.present),slot,size); return;
    }
    for (uint32_t i=0;i<12;++i) {
        const uint64_t offset=b.sparse ? uint64_t(i)*32 : b.offsets[i];
        if (offset+32>size) { Log::get().note("flat compute bounds sample=%u job=%u status=readback-range-invalid",s.id,slot); return; }
        uint32_t raw[8]; std::memcpy(raw,data+size_t(offset),32);
        float xyz[8]; std::memcpy(xyz,raw,32);
        Log::get().note("flat compute bounds sample=%u frame=%llu job=%u record=%u dispatch-q=%u resource=%p first=%u grid=%u,%u,%u corner=%u source-byte=%llu min=%.9g,%.9g,%.9g max=%.9g,%.9g,%.9g bits-u32=%08X,%08X,%08X,%08X,%08X,%08X,%08X,%08X; GPU input captured before dispatch, producer history unproved",
            s.id,static_cast<unsigned long long>(s.present),slot,b.dispatch,b.sequence,b.view.resource,b.view.first,b.counts[0],b.counts[1],b.counts[2],i,
            static_cast<unsigned long long>(b.offsets[i]),xyz[0],xyz[1],xyz[2],xyz[4],xyz[5],xyz[6],raw[0],raw[1],raw[2],raw[3],raw[4],raw[5],raw[6],raw[7]);
    }
}
void completed(const FlatComputeReadbackResult& result, void*) {
    State& g=state(); if (!result.token.sampleId || result.token.sampleId>2) return;
    Sample& s=g.samples[result.token.sampleId-1]; const uint32_t job=result.token.jobId;
    Log::get().note("flat compute readback sample=%u frame=%llu job=%u epoch=%llu q=%llu status=%u hr=%08X bytes=%u associated=%u finished=%u",
        s.id,static_cast<unsigned long long>(s.present),job,static_cast<unsigned long long>(result.token.frameEpoch),
        static_cast<unsigned long long>(result.token.sequence),static_cast<unsigned>(result.status),static_cast<unsigned>(result.hr),result.byteCount,s.mono.selected()?1:0,s.finished?1:0);
    if (job<kRecords) {
        auto& c=s.records[job].cb;c.pending=false;
        if(result.status!=FlatComputeReadbackStatus::Complete)c.status="gpu-readback-failed";
    }
    if (result.status!=FlatComputeReadbackStatus::Complete) {
        for(auto& b:s.grid)if(b.dispatch==job&&b.waiting){
            Log::get().note("flat compute bounds sample=%u dispatch-record=%u status=CB-dependency-failed",s.id,job);
            b.waiting.reset();b.waitingBytes=0;
        }
        return;
    }
    if (job<kRecords) {
        Constants& c=s.records[job].cb; c.copied=std::min(result.byteCount,480u);
        std::memcpy(c.bytes,result.bytes,c.copied); c.status="gpu-input-before-dispatch";
        printConstants(s,job);
        for(uint32_t i=0;i<kBounds;++i){auto& b=s.grid[i];if(b.dispatch==job&&b.waiting){printBounds(s,i,b.waiting.get(),b.waitingBytes);b.waiting.reset();b.waitingBytes=0;}}
    } else if (job<kRecords+kBounds) {
        auto& b=s.grid[job-kRecords];
        if(!b.sparse&&s.records[b.dispatch].cb.pending){
            b.waiting.reset(new(std::nothrow) uint8_t[result.byteCount]);
            if(b.waiting){std::memcpy(b.waiting.get(),result.bytes,result.byteCount);b.waitingBytes=result.byteCount;}
            else Log::get().note("flat compute bounds sample=%u job=%u status=dependency-buffer-allocation-failed",s.id,job);
        } else printBounds(s,job-kRecords,result.bytes,result.byteCount);
    } else if(job>=128&&job<128+kDraws){
        auto& d=s.graphics[job-128];
        for(uint32_t i=0;i<(d.pixel?3u:2u);++i){
            const uint32_t offset=d.rowIndex[i]*16;
            if(offset>result.byteCount||16>result.byteCount-offset)continue;
            std::memcpy(d.rows[i],result.bytes+offset,16);d.valid|=1u<<i;
            Log::get().note("flat compute graphics-row sample=%u frame=%llu record=%u row=%u status=GPU-input-before-draw draw-q=%u",
                s.id,static_cast<unsigned long long>(s.present),job-128,d.rowIndex[i],d.sequence);
            printRows(s,job-128,d.pixel?"PSb1":"VSb1",d.rowIndex[i],d.rows[i],1);
        }
    }
}
HRESULT queue(ID3D11DeviceContext* ctx,ID3D11Buffer* buffer,FlatComputeReadbackKind kind,Sample& s,uint32_t job,uint32_t sequence,const uint64_t* offsets,uint32_t bytes) {
    FlatComputeReadbackRequest request{}; request.kind=kind; request.source=buffer;
    request.token={s.id,job,s.epoch,sequence}; request.offsets=offsets; request.offsetCount=offsets?12u:0u;
    request.wholeBytes=bytes; request.presentOrdinal=state().present;
    return flatComputeReadbackSchedule(ctx,request);
}
void snapshotConstants(ID3D11DeviceContext* ctx,ID3D11Buffer* buffer,Sample& s,uint32_t index) {
    auto& c=s.records[index].cb; if(!buffer)return;c.resource=buffer;
    D3D11_BUFFER_DESC d{};buffer->GetDesc(&d);c.width=d.ByteWidth;
    const uint32_t n=std::min(d.ByteWidth,480u);
    if(flatTemporalCopyConstants(buffer,0,n,c.bytes,c.width,c.writeEpoch,c.writeSequence)) {
        c.copied=n;c.status="same-frame-CPU-write";return;
    }
    const int bank=flatComputeHashIndex(s.records[index].shader);
    if(bank<0||!flatComputeClaimCbFallback(s.cbFallback,static_cast<uint32_t>(bank))){c.status="CPU-write-missing-hash-fallback-already-used";return;}
    const HRESULT hr=queue(ctx,buffer,FlatComputeReadbackKind::ConstantBuffer,s,index,s.records[index].sequence,nullptr,d.ByteWidth);
    c.status=SUCCEEDED(hr)?"gpu-readback-pending":"cpu-write-missing-readback-refused";
    c.pending=SUCCEEDED(hr);
}
void snapshotBounds(ID3D11DeviceContext* ctx,ID3D11ShaderResourceView* view,Sample& s,uint32_t record,uint32_t slot) {
    if(!view)return;
    const uint32_t role=slot==2?0u:1u;
    if(s.boundsByRole[role]==2){++s.boundsDrops;++s.boundsDropsByRole[role];return;}
    const uint32_t jobSlot=role*2+s.boundsByRole[role];
    const View v=s.records[record].srv[slot];
    if(v.stride!=32||!v.elements||v.height){++s.boundsDrops;return;}
    ComPtr<ID3D11Resource> resource;view->GetResource(&resource);
    ComPtr<ID3D11Buffer> buffer;if(FAILED(resource.As(&buffer))){++s.boundsDrops;return;}
    Bounds& b=s.grid[jobSlot];b.dispatch=record;b.sequence=s.records[record].sequence;b.view=v;
    b.sparse=indices(s.records[record].cb,b);
    if(!b.sparse&&!s.records[record].cb.pending){
        ++s.boundsDrops;++s.boundsDropsByRole[role];
        Log::get().note("flat compute bounds-refused sample=%u record=%u q=%u reason=own-grid-CB-missing-or-invalid cb-status=%s; no copy queued, another dispatch may use reserved slot",
            s.id,record,b.sequence,s.records[record].cb.status);return;
    }
    const HRESULT hr=queue(ctx,buffer.Get(),b.sparse?FlatComputeReadbackKind::SparseBounds:FlatComputeReadbackKind::WholeBounds,
        s,kRecords+jobSlot,b.sequence,b.sparse?b.offsets:nullptr,b.sparse?0:v.width);
    Log::get().note("flat compute bounds-queued sample=%u epoch=%llu record=%u q=%u job=%u resource=%p mode=%s hr=%08X bytes=%u",
        s.id,static_cast<unsigned long long>(s.epoch),record,b.sequence,jobSlot,v.resource,b.sparse?"12x32-byte-boxes":"whole-fallback-indices-unknown",static_cast<unsigned>(hr),b.sparse?384:v.width);
    if(SUCCEEDED(hr)){++s.bounds;++s.boundsByRole[role];}else{++s.boundsDrops;++s.boundsDropsByRole[role];}
}
}

bool flatComputeCandidate() noexcept { return active.load(std::memory_order_relaxed); }
bool flatComputeManual() noexcept { return state().manual; }
void flatComputeArm(ID3D11Device* device,uint64_t present) {
    if(!device)return; State& s=state(); active.store(false,std::memory_order_relaxed);
    flatComputeReadbackCancel(completed,nullptr);
    // All COM work occurs here on the known Present owner, never in Stop/DllMain.
    {FlatComputeInternalScope internal;device->GetImmediateContext(s.ctx.ReleaseAndGetAddressOf());}
    for(auto& sample:s.samples) sample=Sample{};
    s.present=present;s.nextMs=GetTickCount64()+5000;s.manual=true;flatComputeArm(s.attempts,present);
    Log::get().note("flat compute armed attempts=2 spacing-ms=5000 max-dispatch-records=64 per-hash=8 actual-binding-getters=sample-frames-only bounds-jobs-per-frame=4 sparse-bytes=384 whole-fallback-cap=4194304 async-poll=no-flush-no-wait; AA/jitter inactive");
}
void flatComputePoll(uint64_t present) {
    State& s=state();s.present=present;
    if(s.ctx&&flatComputeReadbackPending())flatComputeReadbackPoll(s.ctx.Get(),present,completed,nullptr);
}
void flatComputeBoundary(uint64_t present,uint64_t nextEpoch) {
    State& s=state();s.present=present;
    if(!s.manual||flatComputeCandidate()||GetTickCount64()<s.nextMs)return;
    if(!flatComputeBeginAfterOwnedPresent(s.attempts,present))return;
    Sample* sample=current();sample->id=flatComputeAttempt(s.attempts);sample->epoch=nextEpoch;
    active.store(true,std::memory_order_relaxed);
    Log::get().note("flat compute candidate-open sample=%u after-present=%llu epoch=%llu; selection pending until candidate Present",sample->id,static_cast<unsigned long long>(present),static_cast<unsigned long long>(nextEpoch));
}
void flatComputeFinish(uint64_t present,const FlatMonoFrame& mono) {
    Sample* s=current();if(!s||!flatComputeCandidate())return;
    active.store(false,std::memory_order_relaxed);s->present=present;s->mono=mono;s->finished=true;
    Log::get().note("flat compute candidate sample=%u frame=%llu epoch=%llu association=%s reason=%s records=%u graphics=%u graphics-dropped=%u identity-queries=%u identity-dropped=%u other-dispatches=%u bounds-jobs=%u bounds-dropped=%u selected-depth=%p HDR=%p; identities and bytecode roles only, correction inactive",
        s->id,static_cast<unsigned long long>(present),static_cast<unsigned long long>(s->epoch),mono.selected()?"selected":"unassociated",flatMonoReasonName(mono.reason),
        s->used,s->draws,s->drawDrops,s->identityQueries,s->identityDrops,s->otherDispatches,s->bounds,s->boundsDrops,mono.depth,mono.hdr);
    for(uint32_t h=0;h<8;++h)Log::get().note("flat compute bank sample=%u hash=%016llX retained=%u dropped=%u capacity=8",s->id,static_cast<unsigned long long>(kFlatComputeHashes[h]),s->perHash[h],s->dropped[h]);
    Log::get().note("flat compute reservations sample=%u graphics(PS-cluster,VS-flare)=%u,%u dropped=%u,%u bounds(074CB,593EA)=%u,%u dropped=%u,%u; independent capacities graphics4+4 bounds2+2 identity-query-cap=4096",
        s->id,s->drawsByRole[0],s->drawsByRole[1],s->drawDropsByRole[0],s->drawDropsByRole[1],s->boundsByRole[0],s->boundsByRole[1],s->boundsDropsByRole[0],s->boundsDropsByRole[1]);
    if(mono.selected()){
        Log::get().note("flat compute camera sample=%u frame=%llu b1=%p hash=%016llX near=%.9g render=%ux%u output=%ux%u source-q=%u..%u HDR-q=%u..%u tone=%u copy=%u later-output=%u",
            s->id,static_cast<unsigned long long>(present),mono.sceneConstants,static_cast<unsigned long long>(mono.cameraHash),mono.nearPlane,mono.renderWidth,mono.renderHeight,mono.outputWidth,mono.outputHeight,mono.sourceFirst,mono.sourceLast,mono.hdrFirst,mono.hdrLast,mono.toneSequence,mono.copySequence,mono.firstLaterOutput);
        printRows(*s,0,"selected-VSb1",270,reinterpret_cast<const uint8_t*>(mono.camera),6);
    }
    for(uint32_t i=0;i<s->used;++i){const auto& r=s->records[i];
        Log::get().note("flat compute dispatch sample=%u frame=%llu record=%u CS=%016llX q=%u groups=%u,%u,%u indirect-buffer=%p offset=%u dimensions=%s",s->id,static_cast<unsigned long long>(present),i,static_cast<unsigned long long>(r.shader),r.sequence,r.xyz[0],r.xyz[1],r.xyz[2],r.arguments,r.argumentOffset,r.arguments?"GPU-arguments-unread":"direct");
        uint32_t srvMask=0,uavMask=0;for(uint32_t j=0;j<12;++j)if(r.srv[j].view)srvMask|=1u<<j;for(uint32_t j=0;j<3;++j)if(r.uav[j].view)uavMask|=1u<<j;
        Log::get().note("flat compute slots sample=%u record=%u SRV-bound-mask=%03X UAV-bound-mask=%X; zero bits are getter-observed nulls",s->id,i,srvMask,uavMask);
        printConstants(*s,i);for(uint32_t j=0;j<12;++j)if(r.srv[j].view)printView(*s,i,"CS-SRV",j,r.srv[j]);for(uint32_t j=0;j<3;++j)if(r.uav[j].view)printView(*s,i,"CS-UAV",j,r.uav[j]);
    }
    for(uint32_t i=0;i<kDraws;++i){const auto& d=s->graphics[i];if(!d.sequence)continue;
        Log::get().note("flat compute graphics sample=%u frame=%llu record=%u VS=%016llX PS=%016llX q=%u color=%p depth=%p stage=%s b1=%p width=%u valid-row-mask=%u",s->id,static_cast<unsigned long long>(present),i,static_cast<unsigned long long>(d.vs),static_cast<unsigned long long>(d.ps),d.sequence,d.color,d.depth,d.pixel?"PS":"VS",d.constants,d.width,d.valid);
        for(uint32_t j=0;j<(d.pixel?3u:2u);++j){Log::get().note("flat compute graphics-row sample=%u record=%u row=%u status=%s write-epoch=%llu write-q=%u draw-q=%u",s->id,i,d.rowIndex[j],(d.valid&(1u<<j))?"same-frame-CPU-write":"CPU-write-unavailable",static_cast<unsigned long long>(d.writeEpoch[j]),d.writeSequence[j],d.sequence);if(d.valid&(1u<<j))printRows(*s,i,d.pixel?"PSb1":"VSb1",d.rowIndex[j],d.rows[j],1);}
        printView(*s,i,d.pixel?"PS-SRV":"VS-SRV",0,d.views[0]);if(d.pixel)printView(*s,i,"PS-SRV",2,d.views[1]);
    }
    flatComputeFinishAttempt(state().attempts);state().nextMs=GetTickCount64()+5000;
}
void flatComputeDispatch(ID3D11DeviceContext* ctx,uint64_t epoch,uint32_t sequence,UINT x,UINT y,UINT z,ID3D11Buffer* args,UINT offset) {
    Sample* s=current();if(!s||!flatComputeCandidate()||s->epoch!=epoch)return;
    if(s->identityQueries>=4096){++s->identityDrops;return;}++s->identityQueries;
    ComPtr<ID3D11ComputeShader> shader;ctx->CSGetShader(&shader,nullptr,nullptr);
    const uint64_t hash=lookupShaderHash(shader.Get());const int bank=flatComputeHashIndex(hash);
    if(bank<0){++s->otherDispatches;return;}if(s->perHash[bank]==8){++s->dropped[bank];return;}
    const uint32_t index=s->used++;++s->perHash[bank];auto& r=s->records[index];r.shader=hash;r.sequence=sequence;r.xyz[0]=x;r.xyz[1]=y;r.xyz[2]=z;r.arguments=args;r.argumentOffset=offset;
    ID3D11ShaderResourceView* srv[12]{};ID3D11UnorderedAccessView* uav[3]{};ComPtr<ID3D11Buffer> cb;
    ctx->CSGetShaderResources(0,12,srv);ctx->CSGetUnorderedAccessViews(0,3,uav);ctx->CSGetConstantBuffers(0,1,&cb);
    for(uint32_t i=0;i<12;++i)r.srv[i]=describe(srv[i]);for(uint32_t i=0;i<3;++i)r.uav[i]=describe(uav[i]);
    snapshotConstants(ctx,cb.Get(),*s,index);
    const auto role=flatComputeRole(hash);if(role==FlatComputeRole::BoundsT2)snapshotBounds(ctx,srv[2],*s,index,2);if(role==FlatComputeRole::BoundsT1)snapshotBounds(ctx,srv[1],*s,index,1);
    for(auto* v:srv)if(v)v->Release();for(auto* v:uav)if(v)v->Release();
}
void flatComputeDraw(ID3D11DeviceContext* ctx,uint64_t epoch,uint32_t sequence,uint64_t vs,uint64_t ps,const void* depth,const void* color) {
    if(!flatComputeCandidate()||(ps!=0x4E4FF61E8A08FC7Eull&&vs!=0x94D5C556DFD6D705ull))return;
    Sample* s=current();if(!s||s->epoch!=epoch)return;
    // Confirm actual stage identities for the two specific graph consumers.
    ComPtr<ID3D11VertexShader> actualVs;ComPtr<ID3D11PixelShader> actualPs;ctx->VSGetShader(&actualVs,nullptr,nullptr);ctx->PSGetShader(&actualPs,nullptr,nullptr);
    vs=lookupShaderHash(actualVs.Get());ps=lookupShaderHash(actualPs.Get());if(ps!=0x4E4FF61E8A08FC7Eull&&vs!=0x94D5C556DFD6D705ull)return;
    const uint32_t role=ps==0x4E4FF61E8A08FC7Eull?0u:1u;
    if(s->drawsByRole[role]==4){++s->drawDrops;++s->drawDropsByRole[role];return;}
    const uint32_t drawIndex=role*4+s->drawsByRole[role]++;++s->draws;
    auto& d=s->graphics[drawIndex];d.vs=vs;d.ps=ps;d.sequence=sequence;d.depth=depth;d.color=color;d.pixel=role==0;
    ComPtr<ID3D11Buffer> cb;ID3D11ShaderResourceView* views[3]{};
    if(d.pixel){ctx->PSGetConstantBuffers(1,1,&cb);ctx->PSGetShaderResources(0,3,views);d.rowIndex[0]=227;d.rowIndex[1]=228;d.rowIndex[2]=253;}
    else{ctx->VSGetConstantBuffers(1,1,&cb);ctx->VSGetShaderResources(0,1,views);d.rowIndex[0]=281;d.rowIndex[1]=332;}
    d.constants=cb.Get();if(cb){D3D11_BUFFER_DESC desc{};cb->GetDesc(&desc);d.width=desc.ByteWidth;for(uint32_t i=0;i<(d.pixel?3u:2u);++i)if(flatTemporalCopyConstants(cb.Get(),d.rowIndex[i]*16,16,d.rows[i],d.width,d.writeEpoch[i],d.writeSequence[i]))d.valid|=1u<<i;}
    const uint32_t allRows=d.pixel?7u:3u;
    if(cb&&d.valid!=allRows&&flatComputeClaimCbFallback(s->cbFallback,8+role)){
        const HRESULT hr=queue(ctx,cb.Get(),FlatComputeReadbackKind::ConstantBuffer,*s,128+drawIndex,sequence,nullptr,d.width);
        Log::get().note("flat compute graphics-CB-queued sample=%u record=%u stage=%s resource=%p width=%u hr=%08X; one fallback reserved per graphics role",s->id,drawIndex,d.pixel?"PSb1":"VSb1",cb.Get(),d.width,static_cast<unsigned>(hr));
    }
    d.views[0]=describe(views[0]);if(d.pixel)d.views[1]=describe(views[2]);for(auto* v:views)if(v)v->Release();
}
}
