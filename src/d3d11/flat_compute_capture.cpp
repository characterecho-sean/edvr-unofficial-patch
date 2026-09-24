#include "flat_compute_capture.h"
#include "flat_compute_model.h"
#include "flat_compute_readback.h"
#include "exposure_fix.h"
#include "device_hook.h"
#include "../common/log.h"
#include <wrl/client.h>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <memory>
#include <new>
#include <utility>

namespace edvr {
namespace {
using Microsoft::WRL::ComPtr;
constexpr uint32_t kRecords = 64, kDraws = 8, kBounds = 4, kProbeWrites = 96;
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
    ComPtr<ID3D11Resource> heldT3, heldT4, heldU0;
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
struct ProbeWrite {
    struct Cb {
        ComPtr<ID3D11Buffer> held;
        std::unique_ptr<uint8_t[]> bytes;
        uint32_t width = 0, copied = 0, writeSequence = 0, snapshotSequence = 0;
        uint64_t writeEpoch = 0;
        const char* status = "unbound";
    };
    ComPtr<ID3D11Resource> held, sourceHeld[12], vertexHeld, depthHeld, copiedFromHeld;
    View output{}, source[12]{}, vertexSource{}, depth{}, copiedFromView{};
    Cb cb[6]{};
    const void* copiedFrom = nullptr;
    uint64_t vs = 0, ps = 0, cs = 0;
    uint32_t first = 0, last = 0, count = 0, slot = 0;
    const char* kind = nullptr;
};
struct Sample {
    uint32_t id = 0, used = 0, perHash[8]{}, dropped[8]{};
    uint32_t identityQueries = 0, identityDrops = 0, otherDispatches = 0, draws = 0, drawDrops = 0, bounds = 0, boundsDrops = 0;
    uint32_t drawsByRole[2]{}, drawDropsByRole[2]{}, boundsByRole[2]{}, boundsDropsByRole[2]{};
    uint16_t cbFallback = 0;
    uint64_t epoch = 0, present = 0;
    Dispatch records[kRecords]{}; Draw graphics[kDraws]{}; Bounds grid[kBounds]{};
    ProbeWrite probe[kProbeWrites]{};
    uint32_t probeUsed = 0, probeDrops = 0, probeUnknown = 0;
    uint32_t outputWidth = 0, outputHeight = 0, probeExtentRejected = 0;
    uint32_t probeDrawQueries = 0, probeDrawQueryDrops = 0;
    uint32_t probeDispatchQueries = 0, probeDispatchQueryDrops = 0;
    uint32_t probeFallbacks = 0, probeFallbackDrops = 0;
    FlatMonoFrame mono{}; bool finished = false;
};
struct State {
    ComPtr<ID3D11DeviceContext> ctx;
    FlatComputeAttempts attempts;
    uint64_t present = 0, nextMs = 0;
    bool manual = false;
    struct Learned { uint64_t vs=0, ps=0, cs=0; } learned[8]{};
    uint32_t learnedUsed = 0;
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
const char* probeCbLabel(bool compute, uint32_t slot) {
    static const char* graphics[] = {"writer-VSb0", "writer-VSb1", "writer-VSb2",
                                     "writer-PSb0", "writer-PSb1", "writer-PSb2"};
    static const char* computeStage[] = {"writer-CSb0", "writer-CSb1", "writer-CSb2"};
    return compute ? computeStage[slot] : graphics[slot];
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
        if (!v.format) v.format = d.Format;
    }
}
View describe(ID3D11ShaderResourceView* view) {
    View v{}; if (!view) return v; v.view = view;
    D3D11_SHADER_RESOURCE_VIEW_DESC d{}; view->GetDesc(&d);
    v.dimension = d.ViewDimension; v.format = d.Format;
    if (d.ViewDimension == D3D11_SRV_DIMENSION_BUFFER) { v.first = d.Buffer.FirstElement; v.elements = d.Buffer.NumElements; }
    if (d.ViewDimension == D3D11_SRV_DIMENSION_BUFFEREX) { v.first = d.BufferEx.FirstElement; v.elements = d.BufferEx.NumElements; v.flags = d.BufferEx.Flags; }
    if (d.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2D) { v.mip = d.Texture2D.MostDetailedMip; v.mips = d.Texture2D.MipLevels; }
    if (d.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2DARRAY) {
        v.mip=d.Texture2DArray.MostDetailedMip;v.mips=d.Texture2DArray.MipLevels;
        v.first=d.Texture2DArray.FirstArraySlice;v.elements=d.Texture2DArray.ArraySize;
    }
    ComPtr<ID3D11Resource> res; view->GetResource(&res); resourceDesc(res.Get(), v); return v;
}
View describe(ID3D11UnorderedAccessView* view) {
    View v{}; if (!view) return v; v.view = view;
    D3D11_UNORDERED_ACCESS_VIEW_DESC d{}; view->GetDesc(&d);
    v.dimension = d.ViewDimension; v.format = d.Format;
    if (d.ViewDimension == D3D11_UAV_DIMENSION_BUFFER) { v.first = d.Buffer.FirstElement; v.elements = d.Buffer.NumElements; v.flags = d.Buffer.Flags; }
    if (d.ViewDimension == D3D11_UAV_DIMENSION_TEXTURE2D) v.mip = d.Texture2D.MipSlice;
    if (d.ViewDimension == D3D11_UAV_DIMENSION_TEXTURE2DARRAY) {
        v.mip=d.Texture2DArray.MipSlice;v.first=d.Texture2DArray.FirstArraySlice;v.elements=d.Texture2DArray.ArraySize;
    }
    ComPtr<ID3D11Resource> res; view->GetResource(&res); resourceDesc(res.Get(), v); return v;
}
View describe(ID3D11RenderTargetView* view) {
    View v{}; if (!view) return v; v.view = view;
    D3D11_RENDER_TARGET_VIEW_DESC d{}; view->GetDesc(&d);
    v.dimension = d.ViewDimension; v.format = d.Format;
    if (d.ViewDimension == D3D11_RTV_DIMENSION_TEXTURE2D) v.mip = d.Texture2D.MipSlice;
    if (d.ViewDimension == D3D11_RTV_DIMENSION_TEXTURE2DARRAY) {
        v.mip=d.Texture2DArray.MipSlice;v.first=d.Texture2DArray.FirstArraySlice;v.elements=d.Texture2DArray.ArraySize;
    }
    ComPtr<ID3D11Resource> res; view->GetResource(&res); resourceDesc(res.Get(), v); return v;
}
View describe(ID3D11DepthStencilView* view) {
    View v{}; if (!view) return v; v.view = view;
    D3D11_DEPTH_STENCIL_VIEW_DESC d{}; view->GetDesc(&d);
    v.dimension = d.ViewDimension; v.format = d.Format;
    if (d.ViewDimension == D3D11_DSV_DIMENSION_TEXTURE2D) v.mip = d.Texture2D.MipSlice;
    if (d.ViewDimension == D3D11_DSV_DIMENSION_TEXTURE2DARRAY) {
        v.mip=d.Texture2DArray.MipSlice;v.first=d.Texture2DArray.FirstArraySlice;v.elements=d.Texture2DArray.ArraySize;
    }
    ComPtr<ID3D11Resource> res; view->GetResource(&res); resourceDesc(res.Get(), v); return v;
}
void printView(const Sample& s, uint32_t record, const char* stage, uint32_t slot, const View& v) {
    Log::get().note("flat compute view sample=%u frame=%llu record=%u stage=%s slot=%u view=%p resource=%p dimension=%u fmt=%u size=%ux%u stride=%u first=%u elements=%u flags=%u mip=%u mips=%u; actual getter, identities snapshot-local",
        s.id, static_cast<unsigned long long>(s.present), record, stage, slot, v.view, v.resource,
        v.dimension, v.format, v.width, v.height, v.stride, v.first, v.elements, v.flags, v.mip, v.mips);
}
bool probeR32(const View& v) {
    return v.resource && (v.format == DXGI_FORMAT_R32_FLOAT ||
                          v.format == DXGI_FORMAT_R32_TYPELESS);
}
bool probeCandidate(const Sample& s, const View& v) {
    return probeR32(v) && flatR32ProbeExtent(v.width,v.height,
        s.outputWidth,s.outputHeight) != FlatR32ProbeExtent::Unsupported;
}
void probeRecord(Sample& s, uint32_t sequence, const char* kind, uint32_t slot,
                 const View& output, uint64_t vs, uint64_t ps, uint64_t cs,
                 const View* sources = nullptr, const View* depth = nullptr,
                 const void* copiedFrom = nullptr) {
    if (!probeCandidate(s,output)) return;
    auto fillSources=[&](ProbeWrite& event) {
        if(sources)for(uint32_t j=0;j<12;++j){event.source[j]=sources[j];
            event.sourceHeld[j]=static_cast<ID3D11Resource*>(const_cast<void*>(sources[j].resource));}
        if(depth){event.depth=*depth;event.depthHeld=static_cast<ID3D11Resource*>(const_cast<void*>(depth->resource));}
        event.copiedFrom=copiedFrom;
        event.copiedFromHeld=static_cast<ID3D11Resource*>(const_cast<void*>(copiedFrom));
        resourceDesc(event.copiedFromHeld.Get(),event.copiedFromView);
    };
    // One immutable record per write. Coalescing can mix a later CB upload with
    // an earlier GPU fallback and misidentify the pixels consumed by lighting.
    if (s.probeUsed == kProbeWrites) { ++s.probeDrops; return; }
    auto& event = s.probe[s.probeUsed++];
    event.held = static_cast<ID3D11Resource*>(const_cast<void*>(output.resource));
    event.output = output; event.first = event.last = sequence; event.count = 1;
    event.kind = kind; event.slot = slot; event.vs = vs; event.ps = ps; event.cs = cs;
    fillSources(event);
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
        if(job>=256&&job<256+kProbeWrites*6){auto& c=s.probe[(job-256)/6].cb[(job-256)%6];c.status="gpu-readback-failed";}
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
    } else if(job>=256&&job<256+kProbeWrites*6){
        const uint32_t record=(job-256)/6, slot=(job-256)%6;
        auto& c=s.probe[record].cb[slot];
        if(result.byteCount==c.width&&result.bytes){
            c.copied=0;
            c.bytes.reset(new(std::nothrow) uint8_t[result.byteCount]);
            if(c.bytes){std::memcpy(c.bytes.get(),result.bytes,result.byteCount);c.copied=result.byteCount;c.status="gpu-input-before-writer";
                Log::get().note("flat R32 writer-GPU-CB sample=%u record=%u stage=%s slot=%u width=%u hash=%08X snapshot-q=%u status=complete",
                    s.id,record,s.probe[record].cs?"CS":slot<3?"VS":"PS",s.probe[record].cs?slot:slot%3,
                    c.width,hashBytes(c.bytes.get(),c.copied),c.snapshotSequence);
                printRows(s,record,probeCbLabel(s.probe[record].cs!=0,slot),
                    0,c.bytes.get(),c.copied/16);
            }
            else c.status="gpu-payload-allocation-failed";
        }else c.status="gpu-size-mismatch";
    }
}
HRESULT queue(ID3D11DeviceContext* ctx,ID3D11Buffer* buffer,FlatComputeReadbackKind kind,Sample& s,uint32_t job,uint32_t sequence,const uint64_t* offsets,uint32_t bytes) {
    FlatComputeReadbackRequest request{}; request.kind=kind; request.source=buffer;
    request.token={s.id,job,s.epoch,sequence}; request.offsets=offsets; request.offsetCount=offsets?12u:0u;
    request.wholeBytes=bytes; request.presentOrdinal=state().present;
    return flatComputeReadbackSchedule(ctx,request);
}
bool learnedWriter(const State& state, const ProbeWrite& w) {
    for(uint32_t i=0;i<state.learnedUsed;++i){const auto& l=state.learned[i];
        if(l.vs==w.vs&&l.ps==w.ps&&l.cs==w.cs)return true;}
    return false;
}
void probeCaptureCbs(ID3D11DeviceContext* ctx, Sample& s, uint32_t index) {
    auto& w=s.probe[index];if(!w.vs&&!w.ps&&!w.cs)return;
    ID3D11Buffer* buffers[6]{};
    if(w.cs)ctx->CSGetConstantBuffers(0,3,buffers);
    else{ctx->VSGetConstantBuffers(0,3,buffers);ctx->PSGetConstantBuffers(0,3,buffers+3);}
    for(uint32_t i=0;i<6;++i){
        auto& c=w.cb[i];ID3D11Buffer* buffer=buffers[i];
        if(!buffer)continue;
        if(std::strcmp(c.status,"gpu-copy-pending")==0)continue;
        D3D11_BUFFER_DESC d{};buffer->GetDesc(&d);c.held=buffer;c.width=d.ByteWidth;c.snapshotSequence=w.last;
        const uint32_t n=std::min(d.ByteWidth,4096u);
        c.bytes.reset(new(std::nothrow) uint8_t[n]);c.copied=0;
        if(!c.bytes){c.status="CPU-payload-allocation-failed";continue;}
        if(flatTemporalCopyConstants(buffer,0,n,c.bytes.get(),c.width,c.writeEpoch,c.writeSequence)){
            c.copied=n;c.status=n==d.ByteWidth?"same-frame-CPU-complete":"same-frame-CPU-prefix-only";
        }else c.status="CPU-write-unavailable";
        if(s.id==2&&learnedWriter(state(),w)&&c.copied!=d.ByteWidth){
            if(s.probeFallbacks>=6){c.status="gpu-fallback-cap";++s.probeFallbackDrops;continue;}
            const HRESULT hr=queue(ctx,buffer,FlatComputeReadbackKind::ConstantBuffer,s,
                256+index*6+i,w.last,nullptr,d.ByteWidth);
            if(SUCCEEDED(hr)){++s.probeFallbacks;c.status="gpu-copy-pending";}
            else{c.status="gpu-schedule-failed";++s.probeFallbackDrops;}
        }
    }
    for(auto* buffer:buffers)if(buffer)buffer->Release();
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
    // Sample contains retained COM identities and bounded records. Avoid a
    // large automatic temporary on the game's Present thread.
    for(auto& sample:s.samples){auto fresh=std::make_unique<Sample>();sample=std::move(*fresh);}
    s.learnedUsed=0;
    s.present=present;s.nextMs=GetTickCount64()+5000;s.manual=true;flatComputeArm(s.attempts,present);
    Log::get().note("flat compute armed attempts=2 spacing-ms=5000 max-dispatch-records=64 per-hash=8 actual-binding-getters=sample-frames-only bounds-jobs-per-frame=4 sparse-bytes=384 whole-fallback-cap=4194304 async-poll=no-flush-no-wait; passive capture, projection jitter inactive");
}
void flatComputePoll(uint64_t present) {
    State& s=state();s.present=present;
    if(s.ctx&&flatComputeReadbackPending())flatComputeReadbackPoll(s.ctx.Get(),present,completed,nullptr);
}
void flatComputeBoundary(uint64_t present,uint64_t nextEpoch,uint32_t outputWidth,uint32_t outputHeight) {
    State& s=state();s.present=present;
    if(!s.manual||flatComputeCandidate()||GetTickCount64()<s.nextMs)return;
    if(!flatComputeBeginAfterOwnedPresent(s.attempts,present))return;
    Sample* sample=current();sample->id=flatComputeAttempt(s.attempts);sample->epoch=nextEpoch;
    sample->outputWidth=outputWidth;sample->outputHeight=outputHeight;
    active.store(true,std::memory_order_relaxed);
    Log::get().note("flat compute candidate-open sample=%u after-present=%llu epoch=%llu output=%ux%u R32-extents=native-or-exact-three-quarter; selection pending until candidate Present",sample->id,static_cast<unsigned long long>(present),static_cast<unsigned long long>(nextEpoch),outputWidth,outputHeight);
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
        if(d.vs==0x94D5C556DFD6D705ull){captureFlatProbeShader('v',d.vs);if(d.ps)captureFlatProbeShader('p',d.ps);}
    }
    uint32_t matchedConsumers=0, matchedWriters=0;bool printed[kProbeWrites]{};
    for(uint32_t i=0;i<s->used;++i){const auto& consumer=s->records[i];
        if(consumer.shader!=0x5998146D464F5C0Eull&&consumer.shader!=0xEB0245DE0BB23BB6ull&&
           consumer.shader!=0x46A041BDEB7440CBull&&consumer.shader!=0x823CC578F5510B24ull)continue;
        if(!consumer.srv[3].resource||!mono.selected()||consumer.srv[4].resource!=mono.depth||
           consumer.uav[0].resource!=mono.hdr)continue;
        ++matchedConsumers;
        const auto& depth=consumer.srv[3];
        Log::get().note("flat R32 consumer sample=%u frame=%llu q=%u CS=%016llX t3=%p view=%p fmt=%u size=%ux%u mip=%u scene-depth=%p HDR=%p size-match=%u",
            s->id,static_cast<unsigned long long>(present),consumer.sequence,
            static_cast<unsigned long long>(consumer.shader),depth.resource,depth.view,depth.format,
            depth.width,depth.height,depth.mip,mono.depth,mono.hdr,
            ((depth.width==mono.renderWidth&&depth.height==mono.renderHeight)||
             (depth.width==mono.outputWidth&&depth.height==mono.outputHeight))?1u:0u);
        for(uint32_t j=0;j<s->probeUsed;++j){const auto& w=s->probe[j];
            const auto relation=flatR32WriterRelation(w.output.resource,w.first,w.last,
                depth.resource,consumer.sequence);
            if(relation==FlatR32WriterRelation::Unrelated)continue;
            if(printed[j])continue;printed[j]=true;++matchedWriters;
            Log::get().note("flat R32 writer-candidate sample=%u frame=%llu record=%u q=%u..%u count=%u relation=%s kind=%s slot=%u view=%p resource=%p fmt=%u size=%ux%u mip=%u VS=%016llX PS=%016llX CS=%016llX DSV=%p copied-from=%p; bound output, shader writes require bytecode proof",
                s->id,static_cast<unsigned long long>(present),j,w.first,w.last,w.count,
                relation==FlatR32WriterRelation::BeforeConsumer?"before-consumer":"spans-consumer-incomplete",w.kind,w.slot,
                w.output.view,w.output.resource,w.output.format,w.output.width,w.output.height,w.output.mip,
                static_cast<unsigned long long>(w.vs),static_cast<unsigned long long>(w.ps),
                static_cast<unsigned long long>(w.cs),w.depth.resource,w.copiedFrom);
            for(uint32_t slot=0;slot<12;++slot)if(w.source[slot].resource)printView(*s,j,w.cs?"writer-CS-SRV":"writer-PS-SRV",slot,w.source[slot]);
            if(w.vertexSource.resource)printView(*s,j,"writer-VS-SRV",0,w.vertexSource);
            if(w.copiedFromView.resource)printView(*s,j,"writer-copy-source",0,w.copiedFromView);
            for(uint32_t cb=0;cb<6;++cb){const auto& c=w.cb[cb];if(!c.held)continue;
                const char* stage=w.cs?"CS":cb<3?"VS":"PS";
                Log::get().note("flat R32 writer-CB sample=%u record=%u stage=%s slot=%u resource=%p width=%u copied=%u hash=%08X status=%s write-epoch=%llu write-q=%u snapshot-q=%u",
                    s->id,j,stage,w.cs?cb:cb%3,c.held.Get(),c.width,c.copied,
                    c.copied?hashBytes(c.bytes.get(),c.copied):0,c.status,
                    static_cast<unsigned long long>(c.writeEpoch),c.writeSequence,c.snapshotSequence);
                if(c.copied)printRows(*s,j,probeCbLabel(w.cs!=0,cb),
                    0,c.bytes.get(),c.copied/16);
            }
            if(w.vs)captureFlatProbeShader('v',w.vs);
            if(w.ps)captureFlatProbeShader('p',w.ps);
            if(w.cs)captureFlatProbeShader('c',w.cs);
            if(s->id==1&&(w.vs||w.ps||w.cs)&&w.last<consumer.sequence&&!learnedWriter(state(),w)){
                State& root=state();if(root.learnedUsed<8){auto& learned=root.learned[root.learnedUsed++];
                    learned.vs=w.vs;learned.ps=w.ps;learned.cs=w.cs;}
            }
        }
    }
    Log::get().note("flat R32 provenance sample=%u frame=%llu output=%ux%u extent-policy=native-or-exact-three-quarter extent-rejected=%u consumers=%u matched-writer-records=%u candidate-writers=%u dropped=%u draw-queries=%u draw-query-drops=%u dispatch-queries=%u dispatch-query-drops=%u unknown-command-lists=%u learned-hashes=%u gpu-fallbacks=%u fallback-drops=%u verdict=%s; matching requires same resource plus selected scene depth/HDR; unhooked writes remain unproven",
        s->id,static_cast<unsigned long long>(present),s->outputWidth,s->outputHeight,
        s->probeExtentRejected,matchedConsumers,matchedWriters,s->probeUsed,
        s->probeDrops,s->probeDrawQueries,s->probeDrawQueryDrops,s->probeDispatchQueries,s->probeDispatchQueryDrops,
        s->probeUnknown,state().learnedUsed,s->probeFallbacks,s->probeFallbackDrops,
        !s->outputWidth||!s->outputHeight?"output-extent-unavailable":
        !mono.selected()?"no-selected-scene":s->probeDrops||s->probeUnknown||s->probeDrawQueryDrops||s->probeDispatchQueryDrops?"incomplete":
        !matchedConsumers?"no-selected-lighting-consumer":!matchedWriters?"no-writer-observed":"bound-writer-candidate-recorded");
    flatComputeFinishAttempt(state().attempts);state().nextMs=GetTickCount64()+5000;
}
void flatComputeProbeDraw(ID3D11DeviceContext* ctx, uint64_t epoch, uint32_t sequence) {
    Sample* s=current(); if(!ctx||!s||!flatComputeCandidate()||s->epoch!=epoch)return;
    if(s->probeDrawQueries==4096){++s->probeDrawQueryDrops;return;}++s->probeDrawQueries;
    ID3D11RenderTargetView* rtv[8]{}; ID3D11UnorderedAccessView* uav[8]{};
    ComPtr<ID3D11DepthStencilView> dsv;
    ctx->OMGetRenderTargets(8,rtv,&dsv);
    ctx->OMGetRenderTargetsAndUnorderedAccessViews(0,nullptr,nullptr,0,8,uav);
    View output[16]{}; bool any=false;
    for(uint32_t i=0;i<8;++i){
        output[i]=describe(rtv[i]);output[8+i]=describe(uav[i]);
        if(probeR32(output[i])&&!probeCandidate(*s,output[i]))++s->probeExtentRejected;
        if(probeR32(output[8+i])&&!probeCandidate(*s,output[8+i]))++s->probeExtentRejected;
        any|=probeCandidate(*s,output[i])||probeCandidate(*s,output[8+i]);
    }
    if(any){
        ComPtr<ID3D11VertexShader> vs; ComPtr<ID3D11PixelShader> ps;
        ctx->VSGetShader(&vs,nullptr,nullptr);ctx->PSGetShader(&ps,nullptr,nullptr);
        const uint64_t vsHash=lookupShaderHash(vs.Get()), psHash=lookupShaderHash(ps.Get());
        ID3D11ShaderResourceView* srvs[12]{};ComPtr<ID3D11ShaderResourceView> vs0;
        ctx->PSGetShaderResources(0,12,srvs);ctx->VSGetShaderResources(0,1,&vs0);
        View sources[12]{};for(uint32_t i=0;i<12;++i)sources[i]=describe(srvs[i]);
        const View depth=describe(dsv.Get());
        for(uint32_t i=0;i<8;++i){
            if(probeCandidate(*s,output[i]))probeRecord(*s,sequence,"RTV",i,output[i],vsHash,psHash,0,sources,&depth);
            if(probeCandidate(*s,output[8+i]))probeRecord(*s,sequence,"OM-UAV",i,output[8+i],vsHash,psHash,0,sources,&depth);
        }
        for(uint32_t i=0;i<12;++i)if(srvs[i])srvs[i]->Release();
        // The stock path may use VS t0; the retained PS slots remain independently identified.
        for(uint32_t i=0;i<s->probeUsed;++i){auto& event=s->probe[i];
            if(event.last==sequence&&event.vs==vsHash&&event.ps==psHash){
                if(vs0){event.vertexSource=describe(vs0.Get());
                    event.vertexHeld=static_cast<ID3D11Resource*>(const_cast<void*>(event.vertexSource.resource));}
                probeCaptureCbs(ctx,*s,i);
            }}
    }
    for(auto* v:rtv)if(v)v->Release();for(auto* v:uav)if(v)v->Release();
}
void flatComputeProbeDispatch(ID3D11DeviceContext* ctx, uint64_t epoch, uint32_t sequence) {
    Sample* s=current();if(!ctx||!s||!flatComputeCandidate()||s->epoch!=epoch)return;
    if(s->probeDispatchQueries==4096){++s->probeDispatchQueryDrops;return;}++s->probeDispatchQueries;
    ID3D11UnorderedAccessView* uav[8]{};ctx->CSGetUnorderedAccessViews(0,8,uav);
    View output[8]{};bool any=false;for(uint32_t i=0;i<8;++i){output[i]=describe(uav[i]);
        if(probeR32(output[i])&&!probeCandidate(*s,output[i]))++s->probeExtentRejected;
        any|=probeCandidate(*s,output[i]);}
    if(any){
        ComPtr<ID3D11ComputeShader> shader;ctx->CSGetShader(&shader,nullptr,nullptr);
        ID3D11ShaderResourceView* srv[12]{};ctx->CSGetShaderResources(0,12,srv);
        View sources[12]{};for(uint32_t i=0;i<12;++i)sources[i]=describe(srv[i]);
        const uint64_t hash=lookupShaderHash(shader.Get());
        for(uint32_t i=0;i<8;++i)if(probeCandidate(*s,output[i]))probeRecord(*s,sequence,"CS-UAV",i,output[i],0,0,hash,sources);
        for(uint32_t i=0;i<s->probeUsed;++i)if(s->probe[i].last==sequence&&s->probe[i].cs==hash)
            probeCaptureCbs(ctx,*s,i);
        for(auto* v:srv)if(v)v->Release();
    }
    for(auto* v:uav)if(v)v->Release();
}
void flatComputeProbeClear(ID3D11View* view, uint64_t epoch, uint32_t sequence, const char* kind) {
    Sample* s=current();if(!view||!s||!flatComputeCandidate()||s->epoch!=epoch)return;
    ComPtr<ID3D11Resource> res;view->GetResource(&res);View output{};
    ComPtr<ID3D11RenderTargetView> rtv;ComPtr<ID3D11UnorderedAccessView> uav;
    if(SUCCEEDED(view->QueryInterface(IID_PPV_ARGS(&rtv))))output=describe(rtv.Get());
    else if(SUCCEEDED(view->QueryInterface(IID_PPV_ARGS(&uav))))output=describe(uav.Get());
    else{output.view=view;resourceDesc(res.Get(),output);}
    if(probeR32(output)&&!probeCandidate(*s,output))++s->probeExtentRejected;
    probeRecord(*s,sequence,kind,0,output,0,0,0);
}
void flatComputeProbeTransfer(ID3D11Resource* dst, ID3D11Resource* src,
                              uint64_t epoch, uint32_t sequence, char kind) {
    Sample* s=current();if(!dst||!s||!flatComputeCandidate()||s->epoch!=epoch)return;
    View output{};resourceDesc(dst,output);
    if(probeR32(output)&&!probeCandidate(*s,output))++s->probeExtentRejected;
    probeRecord(*s,sequence,kind=='R'?"copy-resource":
                kind=='C'?"copy-region-subresource-unknown":
                kind=='V'?"resolve-subresource-unknown":"transfer-unknown",0,output,0,0,0,nullptr,nullptr,src);
}
void flatComputeProbeUpdate(ID3D11Resource* dst, uint64_t epoch, uint32_t sequence,
                            bool complete) {
    Sample* s=current();if(!dst||!s||!flatComputeCandidate()||s->epoch!=epoch)return;
    View output{};resourceDesc(dst,output);
    if(probeR32(output)&&!probeCandidate(*s,output))++s->probeExtentRejected;
    probeRecord(*s,sequence,complete?"CPU-update":"CPU-update-subresource-unknown",0,output,0,0,0);
}
void flatComputeProbeUnknown(uint64_t epoch, uint32_t sequence) {
    Sample* s=current();if(s&&flatComputeCandidate()&&s->epoch==epoch)++s->probeUnknown;
}
void flatComputeDispatch(ID3D11DeviceContext* ctx,uint64_t epoch,uint32_t sequence,UINT x,UINT y,UINT z,ID3D11Buffer* args,UINT offset) {
    Sample* s=current();if(!s||!flatComputeCandidate()||s->epoch!=epoch)return;
    // The R32 producer may be a different CS from the eight grid roles.
    // Inspect output views before the allowlist filters the existing probe.
    flatComputeProbeDispatch(ctx, epoch, sequence);
    if(s->identityQueries>=4096){++s->identityDrops;return;}++s->identityQueries;
    ComPtr<ID3D11ComputeShader> shader;ctx->CSGetShader(&shader,nullptr,nullptr);
    const uint64_t hash=lookupShaderHash(shader.Get());const int bank=flatComputeHashIndex(hash);
    if(bank<0){++s->otherDispatches;return;}if(s->perHash[bank]==8){++s->dropped[bank];return;}
    const uint32_t index=s->used++;++s->perHash[bank];auto& r=s->records[index];r.shader=hash;r.sequence=sequence;r.xyz[0]=x;r.xyz[1]=y;r.xyz[2]=z;r.arguments=args;r.argumentOffset=offset;
    ID3D11ShaderResourceView* srv[12]{};ID3D11UnorderedAccessView* uav[3]{};ComPtr<ID3D11Buffer> cb;
    ctx->CSGetShaderResources(0,12,srv);ctx->CSGetUnorderedAccessViews(0,3,uav);ctx->CSGetConstantBuffers(0,1,&cb);
    for(uint32_t i=0;i<12;++i)r.srv[i]=describe(srv[i]);for(uint32_t i=0;i<3;++i)r.uav[i]=describe(uav[i]);
    if(r.srv[3].resource)r.heldT3=static_cast<ID3D11Resource*>(const_cast<void*>(r.srv[3].resource));
    if(r.srv[4].resource)r.heldT4=static_cast<ID3D11Resource*>(const_cast<void*>(r.srv[4].resource));
    if(r.uav[0].resource)r.heldU0=static_cast<ID3D11Resource*>(const_cast<void*>(r.uav[0].resource));
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
