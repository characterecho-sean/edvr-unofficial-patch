#include "celestial_motion.h"
#include "binding_shadow.h"
#include "depth_probe.h"
#include "shader_swap.h"
#include "vscreen.h"
#include "gpu_interval.h"
#include "../common/log.h"
#include <wrl/client.h>
#include <utility>
#include <cstdio>
#include <cstring>

namespace edvr {

// The two flags celestialMotionLive reads without a call (celestial_motion.h).
// Out here rather than in the anonymous namespace below purely so the header
// can see them; written only from this file, on the render thread, exactly as
// before.
namespace detail {
bool g_celestialMotionEnabled = false;
bool g_celestialMotionFailed = false;
// celestialMotionAnyWatched() (celestial_motion.h). Kept in sync at every
// site below that changes g_watched[*].buffer, rather than recomputed from
// celestialMotionLive() -- see the header for why those two are not the
// same condition.
bool g_celestialMotionAnyWatched = false;
}  // namespace detail

namespace {
using Microsoft::WRL::ComPtr;
constexpr uint64_t kTerrainDepth = 0xACE405F428C17EF6ull;
constexpr unsigned kRecords = 512;
constexpr unsigned kRecordBytes = 17 * 16;

// The key describes the unchanged sampled terrain patch, not draw order,
// its mutable constant-buffer address, or its current position. Retained
// texture views prevent pointer reuse while a history record refers to them.
// A missing/ambiguous key (including a LOD change) declines for that frame.
constexpr char kBuildHlsl[] = R"HLSL(
struct Record { uint4 key[12]; float4 q; float4 t; float4 r[3]; };
// Per-draw snapshot, 26 float4's per record. In is a typed Buffer, not a
// StructuredBuffer (see createRecords for why), so there is no struct to
// declare here -- just the layout: model = In[index*26+0 .. +7] (8),
// scene = In[index*26+8 .. +11] (4), patch = In[index*26+12 .. +25] (14).
cbuffer Batch : register(b0) { uint4 batch; }
StructuredBuffer<Record> Previous : register(t0);
Buffer<float4> In : register(t1);
Buffer<uint4> Keys : register(t2);
RWStructuredBuffer<Record> Current : register(u0);
float3 rotate(float4 q, float3 v) { return v + 2 * cross(q.xyz, cross(q.xyz,v) + q.w*v); }
float4 multiply(float4 a, float4 b) {
    return float4(a.w*b.xyz + b.w*a.xyz + cross(a.xyz,b.xyz), a.w*b.w-dot(a.xyz,b.xyz));
}
// A local-space eye can contain dozens of patches. Serial comparison of
// every 192-byte key stalls one GPU lane for milliseconds across the eye.
// Search independent predecessors in parallel, retaining exact full keys
// and the unique-match rule (including duplicates in different lanes).
groupshared uint matchCounts[64],matchIndices[64];
[numthreads(64,1,1)] void main(uint3 gid : SV_GroupID, uint lane : SV_GroupIndex) {
    const uint index = batch.x + gid.x; const uint base = index*26;
    Record n = (Record)0;
    n.key[0] = asuint(In[base+12]);
    [unroll] for (uint k=0;k<5;++k) n.key[k+1] = asuint(In[base+15+k]);
    [unroll] for (uint k=0;k<4;++k) n.key[k+6] = asuint(In[base+22+k]);
    n.key[10] = Keys[index*2]; n.key[11] = Keys[index*2+1];
    n.q = normalize(In[base+20]); n.t = float4(In[base+13].xyz,0);
    // Confirm this shader's local-to-clip chain still reduces to view-space
    // perspective: scene[270..273] * model[9..11] == model[4..7].
    bool valid = all(isfinite(n.q)) && all(isfinite(n.t)) &&
        abs(dot(In[base+20],In[base+20])-1) < 0.002 && In[base+2].w > 0 &&
        abs(In[base+3].z-1) < 0.0001 && abs(In[base+2].z) < 0.0001;
    [unroll] for (uint c=0;c<4;++c) {
        float4 col = In[base+8]*In[base+5][c] + In[base+9]*In[base+6][c] +
                     In[base+10]*In[base+7][c] + (c==3 ? In[base+11] : 0);
        float4 expected = float4(In[base+0][c],In[base+1][c],In[base+2][c],In[base+3][c]);
        valid = valid && all(abs(col-expected) < 0.0002);
    }
    uint found=0, match=0;
    [loop] for (uint i=lane;i<batch.y;i+=64) {
        bool same=true;
        [unroll] for (uint k=0;k<12;++k) same = same && all(n.key[k]==Previous[i].key[k]);
        if (same) { ++found; match=i; }
    }
    matchCounts[lane]=found;matchIndices[lane]=match;
    GroupMemoryBarrierWithGroupSync();
    [unroll] for (uint step=32;step;step>>=1) {
        if (lane<step) {
            matchCounts[lane]+=matchCounts[lane+step];
            matchIndices[lane]=max(matchIndices[lane],matchIndices[lane+step]);
        }
        GroupMemoryBarrierWithGroupSync();
    }
    if (lane!=0) return;
    found=matchCounts[0];match=matchIndices[0];
    if (valid && found==1) {
        Record p=Previous[match];
        float4 dq=normalize(multiply(p.q,float4(-n.q.xyz,n.q.w)));
        float3 a=rotate(dq,float3(1,0,0)), b=rotate(dq,float3(0,1,0)), c=rotate(dq,float3(0,0,1));
        float3 t=p.t.xyz-rotate(dq,n.t.xyz);
        n.r[0]=float4(a.x,b.x,c.x,t.x);
        n.r[1]=float4(a.y,b.y,c.y,t.y);
        n.r[2]=float4(a.z,b.z,c.z,t.z);
        n.t.w=all(isfinite(t)) && all(isfinite(dq)) && abs(dot(p.q,p.q)-1)<0.002 ? 1 : 0;
    }
    // Invalid current geometry must not become a valid predecessor.
    if (!valid) n.q=0;
    Current[index]=n;
}
)HLSL";
constexpr char kIndexHlsl[] = R"HLSL(
cbuffer Draw : register(b13) { uint4 info; uint4 texKey[2]; }
uint main(float4 p:SV_Position):SV_Target { return info.x+1; }
struct Coverage { uint index:SV_Target0; float depth:SV_Target1; };
Coverage original(float4 p:SV_Position) {
    Coverage o; o.index=info.x+1; o.depth=p.z; return o;
}
)HLSL";

struct Records {
    ComPtr<ID3D11Buffer> buffer;
    ComPtr<ID3D11ShaderResourceView> srv;
    ComPtr<ID3D11UnorderedAccessView> uav;
    ComPtr<ID3D11ShaderResourceView> sources[kRecords][4];
    // Per-draw snapshot of the live VS constants (416 B/record) and the
    // four SRV pointer keys, consumed by flush()'s batched build dispatch.
    ComPtr<ID3D11Buffer> inputs;
    ComPtr<ID3D11ShaderResourceView> inputsSrv;
    ComPtr<ID3D11Buffer> keys;
    ComPtr<ID3D11ShaderResourceView> keysSrv;
    UINT keyData[kRecords][8];
    // CPU-shadow mirror of `inputs`: rowData holds the bytes a captured
    // slot would otherwise have reached only via CopySubresourceRegion;
    // rowCpu's bits 0/1/2 say which of the three segments came from the
    // shadow this draw. flush() must not re-upload a segment whose bit is
    // clear -- it was already written by the GPU copy at draw time.
    uint8_t rowData[kRecords][416];
    uint8_t rowCpu[kRecords];
    unsigned count=0, built=0;
};
struct Eye {
    ComPtr<ID3D11Texture2D> scene, index, depth;
    ComPtr<ID3D11RenderTargetView> rtv;
    ComPtr<ID3D11DepthStencilView> dsv;
    ComPtr<ID3D11ShaderResourceView> indexSrv, depthSrv;
    ComPtr<ID3D11Texture2D> originalDepth;
    ComPtr<ID3D11RenderTargetView> originalDepthRtv;
    ComPtr<ID3D11ShaderResourceView> originalDepthSrv;
    Records records[2];
    unsigned write=0, width=0, height=0;
    bool cleared=false, original=false;
};
Eye g_eyes[2];
bool g_noted=false, g_capNoted=false;
// The three VS constant buffers (b0/b1/b2) only ever contribute these three
// byte ranges to the terrain draw's per-row capture. begin(), flush() and
// every tee below share this one table, so nothing ever shadows more than
// what is actually read.
constexpr struct { UINT srcOffset, rowOffset, length; } kSegment[3] = {
    {64, 0, 128},
    {4320, 128, 64},
    {0, 192, 224},
};
// CPU shadow of the terrain draw's three watched constant buffers.
// Identity only: no reference held (the game may destroy the buffer), so
// `buffer` is compared but never dereferenced outside begin(). `shadow`
// holds only slot i's own segment: kSegment[i].length bytes starting at
// shadow offset 0, copied from source offset kSegment[i].srcOffset --
// never the whole constant buffer.
struct WatchedCb {
    ID3D11Buffer* buffer=nullptr;
    UINT bytes=0;
    void* mapped=nullptr;
    bool valid=false;
    uint8_t shadow[224];
};
WatchedCb g_watched[3];
unsigned g_cpuSlots=0, g_gpuSlots=0, g_rewatches=0;
ComPtr<ID3D11ComputeShader> g_build;
ComPtr<ID3D11PixelShader> g_index, g_indexOriginal;
ComPtr<ID3D11Buffer> g_draw;
ComPtr<ID3D11Buffer> g_batch;
ComPtr<ID3D11BlendState> g_blend;
ComPtr<ID3D11DepthStencilState> g_depth;
ComPtr<ID3D11Buffer> g_dump;
unsigned g_dumpCount=0;
GpuIntervals<16> g_gpu;
unsigned g_costFrames=0,g_costDraws=0,g_originalDraws=0;
GpuIntervals<16> g_buildGpu;
unsigned g_buildBatches=0;
// The hook's own CPU, wall time inside begin() past its first test and
// inside celestialMotionEnd(), so a flight can set the render thread's
// time in this hook against the benchmark's CPU figure (the terrain
// frame-time arc, 2026-09-17). Two counter reads per terrain draw.
uint64_t g_hookCpuTicks=0, g_hookCpuCalls=0;
// The CPU shadow's own memcpy cost: only the copy inside Unmapped/Written,
// and only when it lands on a watched slot, so this measures exactly the
// work the GPU-copy fallback would not have paid.
uint64_t g_teeTicks=0, g_teeCalls=0;
uint64_t hookCpuFrequency() {
    static const uint64_t f=[]{ LARGE_INTEGER q{}; QueryPerformanceFrequency(&q); return q.QuadPart>0?static_cast<uint64_t>(q.QuadPart):1u; }();
    return f;
}
struct HookCpu {
    LARGE_INTEGER t0{};
    HookCpu(){ QueryPerformanceCounter(&t0); }
    ~HookCpu(){ LARGE_INTEGER t1{}; QueryPerformanceCounter(&t1); if(t1.QuadPart>t0.QuadPart) g_hookCpuTicks+=static_cast<uint64_t>(t1.QuadPart-t0.QuadPart); ++g_hookCpuCalls; }
};
void reportCost() {
    const auto& t=g_gpu.totals; const auto& b=g_buildGpu.totals;
    const double hookUs=static_cast<double>(g_hookCpuTicks)*1e6/static_cast<double>(hookCpuFrequency());
    const double teeUs=static_cast<double>(g_teeTicks)*1e6/static_cast<double>(hookCpuFrequency());
    if(g_costDraws || t.samples || t.invalid || t.skipped || g_buildBatches || b.samples || b.invalid || b.skipped)
        Log::get().note("terrain motion GPU: %u patches (%u original draws, %u reissues) in %u frames; completed=%u skipped=%u invalid=%u, %.3f us/patch bracket (copies, the coverage draw and restore; no dispatch; every 64th draw; no wait/flush; separate from EDVR-at-door GPU). Batched build: %u batches, %.3f us/eye (%u samples, %u skipped). Hook CPU: %.2f us/call over %llu calls, %.3f ms/frame. Constants: %u slots from the CPU shadow, %u by GPU copy, %u re-watches; tee copies %.2f us each over %llu writes, %.3f ms/frame.",
            g_costDraws,g_originalDraws,g_costDraws-g_originalDraws,g_costFrames,t.samples,t.skipped,t.invalid,t.samples?t.ms*1000/t.samples:0.0,
            g_buildBatches,b.samples?b.ms*1000/b.samples:0.0,b.samples,b.skipped,
            g_hookCpuCalls?hookUs/static_cast<double>(g_hookCpuCalls):0.0,static_cast<unsigned long long>(g_hookCpuCalls),
            g_costFrames?hookUs/1000.0/static_cast<double>(g_costFrames):0.0,
            g_cpuSlots,g_gpuSlots,g_rewatches,
            g_teeCalls?teeUs/static_cast<double>(g_teeCalls):0.0,static_cast<unsigned long long>(g_teeCalls),
            g_costFrames?teeUs/1000.0/static_cast<double>(g_costFrames):0.0);
}
struct Saved {
    ID3D11RenderTargetView* rt[8]{};
    ID3D11DepthStencilView* ds=nullptr;
    ComPtr<ID3D11PixelShader> ps;
    ID3D11ClassInstance* classes[256]{};
    UINT classCount=256, stencil=0, sampleMask=0;
    ComPtr<ID3D11Buffer> cb;
    ComPtr<ID3D11BlendState> blend;
    ComPtr<ID3D11DepthStencilState> depth;
    FLOAT factor[4]{};
    bool active=false,timed=false;
} g_saved;

bool createRecords(ID3D11Device* dev, Records& r) {
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth=kRecords*kRecordBytes; bd.StructureByteStride=kRecordBytes;
    bd.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    bd.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_UNORDERED_ACCESS;
    if (!SUCCEEDED(dev->CreateBuffer(&bd,nullptr,&r.buffer)) ||
        !SUCCEEDED(dev->CreateShaderResourceView(r.buffer.Get(),nullptr,&r.srv)) ||
        !SUCCEEDED(dev->CreateUnorderedAccessView(r.buffer.Get(),nullptr,&r.uav))) return false;
    // Draw-time snapshot the batched build reads instead of the game's
    // now-overwritten live constants. A boxed CopySubresourceRegion into a
    // MISC_BUFFER_STRUCTURED destination silently drops its data under
    // WARP (no debug-layer message either); a plain buffer with an
    // explicit typed SRV is the shape a boxed GPU-to-GPU copy actually
    // lands in, confirmed against a plain-buffer A/B.
    D3D11_BUFFER_DESC id{};
    id.ByteWidth=kRecords*416; id.BindFlags=D3D11_BIND_SHADER_RESOURCE;
    if (!SUCCEEDED(dev->CreateBuffer(&id,nullptr,&r.inputs))) return false;
    D3D11_SHADER_RESOURCE_VIEW_DESC isd{};
    isd.Format=DXGI_FORMAT_R32G32B32A32_FLOAT; isd.ViewDimension=D3D11_SRV_DIMENSION_BUFFER;
    isd.Buffer.FirstElement=0; isd.Buffer.NumElements=kRecords*26;
    if (!SUCCEEDED(dev->CreateShaderResourceView(r.inputs.Get(),&isd,&r.inputsSrv))) return false;
    D3D11_BUFFER_DESC kd{};
    kd.ByteWidth=kRecords*2*16; kd.BindFlags=D3D11_BIND_SHADER_RESOURCE;
    if (!SUCCEEDED(dev->CreateBuffer(&kd,nullptr,&r.keys))) return false;
    D3D11_SHADER_RESOURCE_VIEW_DESC ksd{};
    ksd.Format=DXGI_FORMAT_R32G32B32A32_UINT; ksd.ViewDimension=D3D11_SRV_DIMENSION_BUFFER;
    ksd.Buffer.FirstElement=0; ksd.Buffer.NumElements=kRecords*2;
    return SUCCEEDED(dev->CreateShaderResourceView(r.keys.Get(),&ksd,&r.keysSrv));
}
bool createEye(ID3D11Device* dev, ID3D11Texture2D* scene, Eye& e) {
    D3D11_TEXTURE2D_DESC src{}; scene->GetDesc(&src);
    if (src.SampleDesc.Count!=1 || src.ArraySize!=1 || !src.Width || !src.Height) return false;
    e=Eye{}; e.scene=scene; e.width=src.Width; e.height=src.Height;
    D3D11_TEXTURE2D_DESC td{};
    td.Width=e.width; td.Height=e.height; td.MipLevels=td.ArraySize=td.SampleDesc.Count=1;
    td.Format=DXGI_FORMAT_R32_UINT; td.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(dev->CreateTexture2D(&td,nullptr,&e.index)) ||
        FAILED(dev->CreateRenderTargetView(e.index.Get(),nullptr,&e.rtv)) ||
        FAILED(dev->CreateShaderResourceView(e.index.Get(),nullptr,&e.indexSrv))) return false;
    return createRecords(dev,e.records[0]) && createRecords(dev,e.records[1]);
}
bool createPrivateDepth(ID3D11Device* dev,Eye& e) {
    D3D11_TEXTURE2D_DESC td{};td.Width=e.width;td.Height=e.height;
    td.MipLevels=td.ArraySize=td.SampleDesc.Count=1;
    td.Format=DXGI_FORMAT_R32_TYPELESS; td.BindFlags=D3D11_BIND_DEPTH_STENCIL|D3D11_BIND_SHADER_RESOURCE;
    D3D11_DEPTH_STENCIL_VIEW_DESC dd{}; dd.Format=DXGI_FORMAT_D32_FLOAT; dd.ViewDimension=D3D11_DSV_DIMENSION_TEXTURE2D;
    D3D11_SHADER_RESOURCE_VIEW_DESC sd{}; sd.Format=DXGI_FORMAT_R32_FLOAT;
    sd.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2D; sd.Texture2D.MipLevels=1;
    return SUCCEEDED(dev->CreateTexture2D(&td,nullptr,&e.depth)) &&
        SUCCEEDED(dev->CreateDepthStencilView(e.depth.Get(),&dd,&e.dsv)) &&
        SUCCEEDED(dev->CreateShaderResourceView(e.depth.Get(),&sd,&e.depthSrv));
}
bool ensure(ID3D11DeviceContext* ctx, ID3D11Device* dev) {
    if (g_build && g_index && g_indexOriginal && g_draw && g_batch && g_blend && g_depth) return true;
    g_build.Attach(shaderSwapCompileCs(ctx,kBuildHlsl,sizeof(kBuildHlsl)-1,"main","terrain motion",nullptr,"terrain motion"));
    g_index.Attach(shaderSwapCompilePs(ctx,kIndexHlsl,sizeof(kIndexHlsl)-1,"main","terrain coverage",nullptr,"terrain motion"));
    g_indexOriginal.Attach(shaderSwapCompilePs(ctx,kIndexHlsl,sizeof(kIndexHlsl)-1,"original","terrain original coverage",nullptr,"terrain motion"));
    D3D11_BUFFER_DESC bd{}; bd.ByteWidth=48; bd.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
    D3D11_BUFFER_DESC bbd{}; bbd.ByteWidth=16; bbd.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
    D3D11_BLEND_DESC blend{}; blend.RenderTarget[0].RenderTargetWriteMask=D3D11_COLOR_WRITE_ENABLE_RED;
    D3D11_DEPTH_STENCIL_DESC depth{}; depth.DepthEnable=TRUE;
    depth.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ALL; depth.DepthFunc=D3D11_COMPARISON_GREATER_EQUAL;
    return g_build && g_index && g_indexOriginal && SUCCEEDED(dev->CreateBuffer(&bd,nullptr,&g_draw)) &&
        SUCCEEDED(dev->CreateBuffer(&bbd,nullptr,&g_batch)) &&
        SUCCEEDED(dev->CreateBlendState(&blend,&g_blend)) && SUCCEEDED(dev->CreateDepthStencilState(&depth,&g_depth));
}
void fail() {
    detail::g_celestialMotionFailed=true;
    Log::get().note("terrain motion: resource setup failed; original scene and camera motion retained.");
}
} // namespace

void celestialMotionConfigure(bool enabled) {
    if (detail::g_celestialMotionEnabled!=enabled) celestialMotionShutdown();
    detail::g_celestialMotionEnabled=enabled;
}
static bool begin(ID3D11DeviceContext* ctx, uint64_t vs, bool original) {
    if (!detail::g_celestialMotionEnabled || detail::g_celestialMotionFailed || vs!=kTerrainDepth || g_saved.active) return false;
    HookCpu cpu;
    if (original) {
        // The observed prepass has no PS: adding colour outputs cannot
        // replace game shading/discard/depth export. Keep its actual DSV,
        // depth/stencil state, sample mask, viewport and geometry intact.
        ComPtr<ID3D11PixelShader> ps; ctx->PSGetShader(&ps,nullptr,nullptr);
        if (ps) return false;
        ComPtr<ID3D11BlendState> blend;ctx->OMGetBlendState(&blend,nullptr,nullptr);
        D3D11_BLEND_DESC bd{};if(blend)blend->GetDesc(&bd);
        if(bd.AlphaToCoverageEnable)return false;
    }
    // A depth prepass can have no colour target. Identify the eye by its
    // actual scene-depth resource, not by colour-target order.
    auto* bound=static_cast<ID3D11DepthStencilView*>(bindingGet(BindSlot::Dsv0));
    if (!bound) return false;
    ComPtr<ID3D11Resource> res; bound->GetResource(&res);
    ComPtr<ID3D11Texture2D> scene; if (FAILED(res.As(&scene))) return false;
    if (!depthProbeIsSceneDepth(scene.Get())) return false;
    D3D11_TEXTURE2D_DESC td{}; scene->GetDesc(&td);
    int eye=-1;
    for (int i=0;i<2;++i) {
        ID3D11Texture2D* pair=nullptr; uint32_t fmt=0;
        if (depthProbeSceneDepthFormat(td.Width,td.Height,i,&pair,&fmt) && pair==scene.Get()) { eye=i; break; }
    }
    if (eye<0) return false;
    Eye& e=g_eyes[eye];
    ComPtr<ID3D11Device> dev; ctx->GetDevice(&dev);
    if (!ensure(ctx,dev.Get())) { fail(); return false; }
    if (e.scene.Get()!=scene.Get() && !createEye(dev.Get(),scene.Get(),e)) { fail(); return false; }
    // One layer cannot mix private depth testing and scene depth testing.
    // An unexpected mixed prepass retains camera motion for excess draws.
    if (e.cleared && e.original!=original) return false;
    if (!original && !e.depth && !createPrivateDepth(dev.Get(),e)) { fail(); return false; }
    if (original && !e.originalDepth) {
        D3D11_TEXTURE2D_DESC od{}; od.Width=e.width; od.Height=e.height;
        od.MipLevels=od.ArraySize=od.SampleDesc.Count=1;
        od.Format=DXGI_FORMAT_R32_FLOAT; od.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(dev->CreateTexture2D(&od,nullptr,&e.originalDepth)) ||
            FAILED(dev->CreateRenderTargetView(e.originalDepth.Get(),nullptr,&e.originalDepthRtv)) ||
            FAILED(dev->CreateShaderResourceView(e.originalDepth.Get(),nullptr,&e.originalDepthSrv))) { fail(); return false; }
    }
    Records& now=e.records[e.write]; Records& prev=e.records[1-e.write];
    if (now.count==kRecords) {
        if (!g_capNoted) { g_capNoted=true; Log::get().note("terrain motion: 512 draws per eye reached; excess patches retain camera motion."); }
        return false;
    }
    ID3D11Buffer* cb[3]{}; ctx->VSGetConstantBuffers(0,3,cb);
    bool enough=true;
    const UINT minimum[3]={13*16,280*16,18*16};
    UINT byteWidth[3]{};
    for (int i=0;i<3;++i) {
        D3D11_BUFFER_DESC bd{}; if (cb[i]) cb[i]->GetDesc(&bd);
        byteWidth[i]=bd.ByteWidth;
        enough=enough && bd.ByteWidth>=minimum[i];
    }
    if (!enough) { for (auto* b:cb) if (b) b->Release(); return false; }
    // Re-watch a slot whenever its bound buffer pointer changed; the shadow
    // cannot be trusted for a buffer we have not been tee'd on since it was
    // (re)bound, so the first draw after a re-watch always takes the GPU copy.
    for (int i=0;i<3;++i) {
        if (cb[i]!=g_watched[i].buffer) {
            g_watched[i].buffer=cb[i]; g_watched[i].bytes=byteWidth[i];
            g_watched[i].valid=false; g_watched[i].mapped=nullptr;
            ++g_rewatches;
        }
    }
    detail::g_celestialMotionAnyWatched = g_watched[0].buffer || g_watched[1].buffer || g_watched[2].buffer;
    if((++g_costDraws&63u)==0)g_saved.timed=g_gpu.begin(ctx);
    if(original)++g_originalDraws;
    ID3D11ShaderResourceView* sources[4]{}; ctx->VSGetShaderResources(0,4,sources);
    const unsigned rec=now.count;
    UINT data[12]={rec,prev.count,0,0};
    ctx->UpdateSubresource(g_draw.Get(),0,nullptr,data,0,0);
    for (int i=0;i<4;++i) {
        const auto key=reinterpret_cast<uint64_t>(sources[i]);
        now.keyData[rec][i*2]=static_cast<UINT>(key); now.keyData[rec][i*2+1]=static_cast<UINT>(key>>32);
        now.sources[rec][i].Attach(sources[i]);
    }
    // The game rewrites b0/b2 every patch; a deferred build cannot read
    // them live. Prefer the CPU shadow captured at the write (Map/Unmap or
    // UpdateSubresource); fall back to the GPU copy for a slot whose
    // shadow is not known to be current. Previous state must remain the
    // draw-time state on the GPU, not its end-of-frame contents (asserted
    // by the regression rig).
    uint8_t cpuMask=0;
    for (int i=0;i<3;++i) {
        const auto& seg=kSegment[i];
        if (g_watched[i].valid) {
            memcpy(&now.rowData[rec][seg.rowOffset],g_watched[i].shadow,seg.length);
            cpuMask|=static_cast<uint8_t>(1u<<i); ++g_cpuSlots;
        } else {
            const D3D11_BOX box{seg.srcOffset,0,0,seg.srcOffset+seg.length,1,1};
            ctx->CopySubresourceRegion(now.inputs.Get(),0,rec*416u+seg.rowOffset,0,0,cb[i],0,&box);
            ++g_gpuSlots;
        }
    }
    now.rowCpu[rec]=cpuMask;
    for (auto* b:cb) b->Release();
    ++now.count;
    if (!e.cleared) {
        const float zero[4]{}; ctx->ClearRenderTargetView(e.rtv.Get(),zero);
        if(original)ctx->ClearRenderTargetView(e.originalDepthRtv.Get(),zero);
        else ctx->ClearDepthStencilView(e.dsv.Get(),D3D11_CLEAR_DEPTH,0,0);
        e.cleared=true; e.original=original;
    }
    ctx->OMGetRenderTargets(8,g_saved.rt,&g_saved.ds);
    ctx->PSGetShader(&g_saved.ps,g_saved.classes,&g_saved.classCount);
    ctx->PSGetConstantBuffers(13,1,&g_saved.cb);
    ctx->OMGetBlendState(&g_saved.blend,g_saved.factor,&g_saved.sampleMask);
    ctx->OMGetDepthStencilState(&g_saved.depth,&g_saved.stencil);
    if(original) {
        ID3D11RenderTargetView* rt[2]={e.rtv.Get(),e.originalDepthRtv.Get()};
        vScreenSetRenderTargetsRaw(ctx,2,rt,g_saved.ds);
        ctx->OMSetBlendState(g_blend.Get(),nullptr,g_saved.sampleMask);
    } else {
        vScreenSetRenderTargetsRaw(ctx,1,e.rtv.GetAddressOf(),e.dsv.Get());
        ctx->OMSetBlendState(g_blend.Get(),nullptr,0xffffffff);
        ctx->OMSetDepthStencilState(g_depth.Get(),0);
    }
    ctx->PSSetShader(original?g_indexOriginal.Get():g_index.Get(),nullptr,0); ctx->PSSetConstantBuffers(13,1,g_draw.GetAddressOf());
    g_saved.active=true;
    if (!g_noted) {
        g_noted=true;
        Log::get().note("terrain motion: draw-time GPU transform history and private coverage active (%ux%u); null-PS prepasses captured in original draw, other passes reissued; 512 patches per eye, TAA/DLSS; missing or changed patches retain camera motion.",td.Width,td.Height);
    }
    return true;
}
bool celestialMotionBegin(ID3D11DeviceContext* ctx,uint64_t vs) { return begin(ctx,vs,false); }
bool celestialMotionBeginOriginal(ID3D11DeviceContext* ctx,uint64_t vs) { return begin(ctx,vs,true); }
void celestialMotionEnd(ID3D11DeviceContext* ctx) {
    if (!g_saved.active) return;
    HookCpu cpu;
    vScreenSetRenderTargetsRaw(ctx,8,g_saved.rt,g_saved.ds);
    ctx->PSSetShader(g_saved.ps.Get(),g_saved.classes,g_saved.classCount);
    ctx->PSSetConstantBuffers(13,1,g_saved.cb.GetAddressOf());
    ctx->OMSetBlendState(g_saved.blend.Get(),g_saved.factor,g_saved.sampleMask);
    ctx->OMSetDepthStencilState(g_saved.depth.Get(),g_saved.stencil);
    for (auto* p:g_saved.rt) if (p) p->Release();
    if (g_saved.ds) g_saved.ds->Release();
    for (UINT i=0;i<g_saved.classCount;++i) g_saved.classes[i]->Release();
    if(g_saved.timed)g_gpu.end(ctx);
    g_saved=Saved{};
}
// Batch the still-pending [built,count) records of one eye into a single
// dispatch: the per-draw hook only ever snapshots and swaps render
// targets, and the actual GPU work happens once, here, on demand.
static void flush(ID3D11DeviceContext* ctx, Eye& e) {
    Records& now=e.records[e.write]; Records& prev=e.records[1-e.write];
    if (now.count<=now.built) return;
    const unsigned built=now.built, count=now.count;
    // Upload the CPU-shadowed segments of [built,count) into `inputs`
    // ahead of the dispatch. A segment whose bit is clear in rowCpu was
    // already written by the GPU copy at draw time and must not be
    // touched here -- flush() may run more than once per eye per frame
    // with growing `built`, so this covers exactly the new rows.
    {
        bool allCpu=true;
        for (unsigned i=built;i<count && allCpu;++i) allCpu=now.rowCpu[i]==7;
        if (allCpu) {
            const D3D11_BOX box{built*416u,0,0,count*416u,1,1};
            ctx->UpdateSubresource(now.inputs.Get(),0,&box,&now.rowData[built][0],0,0);
        } else {
            for (unsigned i=built;i<count;++i) {
                for (unsigned s=0;s<3;++s) if (now.rowCpu[i]&(1u<<s)) {
                    const auto& seg=kSegment[s];
                    const D3D11_BOX box{i*416u+seg.rowOffset,0,0,i*416u+seg.rowOffset+seg.length,1,1};
                    ctx->UpdateSubresource(now.inputs.Get(),0,&box,&now.rowData[i][seg.rowOffset],0,0);
                }
            }
        }
    }
    // Preserve every touched compute binding, including dynamic linkage.
    ComPtr<ID3D11ComputeShader> cs; ID3D11ClassInstance* classes[256]{}; UINT nc=256;
    ComPtr<ID3D11Buffer> savedCb; ID3D11ShaderResourceView* savedSrv[3]{};
    ComPtr<ID3D11UnorderedAccessView> savedUav;
    ctx->CSGetShader(&cs,classes,&nc); ctx->CSGetConstantBuffers(0,1,&savedCb);
    ctx->CSGetShaderResources(0,3,savedSrv); ctx->CSGetUnorderedAccessViews(0,1,&savedUav);
    const bool timed=g_buildGpu.begin(ctx);
    const D3D11_BOX keyBox{built*32u,0,0,count*32u,1,1};
    ctx->UpdateSubresource(now.keys.Get(),0,&keyBox,&now.keyData[built][0],0,0);
    UINT batchData[4]={built,prev.count,0,0};
    ctx->UpdateSubresource(g_batch.Get(),0,nullptr,batchData,0,0);
    ctx->CSSetShader(g_build.Get(),nullptr,0); ctx->CSSetConstantBuffers(0,1,g_batch.GetAddressOf());
    ID3D11ShaderResourceView* srvs[3]={prev.srv.Get(),now.inputsSrv.Get(),now.keysSrv.Get()};
    ctx->CSSetShaderResources(0,3,srvs);
    ctx->CSSetUnorderedAccessViews(0,1,now.uav.GetAddressOf(),nullptr);
    ctx->Dispatch(count-built,1,1);
    ++g_buildBatches;
    if(timed)g_buildGpu.end(ctx);
    ID3D11UnorderedAccessView* nullUav=nullptr; ID3D11ShaderResourceView* nullSrvs[3]{};
    ctx->CSSetUnorderedAccessViews(0,1,&nullUav,nullptr); ctx->CSSetShaderResources(0,3,nullSrvs);
    ctx->CSSetShaderResources(0,3,savedSrv); ctx->CSSetUnorderedAccessViews(0,1,savedUav.GetAddressOf(),nullptr);
    ctx->CSSetConstantBuffers(0,1,savedCb.GetAddressOf()); ctx->CSSetShader(cs.Get(),classes,nc);
    for (UINT i=0;i<nc;++i) classes[i]->Release();
    for (auto* s:savedSrv) if (s) s->Release();
    now.built=count;
}
void celestialMotionFrameBoundary(ID3D11DeviceContext* ctx) {
    if (!detail::g_celestialMotionEnabled) return;
    // Build any eye the temporal pass never consumed this frame, so next
    // frame's "previous" table is complete before write swaps under it.
    if(ctx) { g_gpu.poll(ctx); g_buildGpu.poll(ctx); for (auto& e:g_eyes) flush(ctx,e); }
    if(++g_costFrames%1800==0)reportCost();
    for (auto& e:g_eyes) {
        e.write=1-e.write; Records& next=e.records[e.write];
        for (unsigned i=0;i<next.count;++i) for (auto& view:next.sources[i]) view.Reset();
        next.count=0; next.built=0; e.cleared=false;
    }
}
void celestialMotionViews(ID3D11DeviceContext* ctx, ID3D11Texture2D* scene, ID3D11ShaderResourceView** views) {
    views[0]=views[1]=views[2]=nullptr;
    if (!detail::g_celestialMotionEnabled || detail::g_celestialMotionFailed || !scene) return;
    for (auto& e:g_eyes) if (e.scene.Get()==scene && e.cleared && e.records[e.write].count) {
        if (ctx) flush(ctx,e);
        views[0]=e.indexSrv.Get(); views[1]=e.original?e.originalDepthSrv.Get():e.depthSrv.Get(); views[2]=e.records[e.write].srv.Get();
        return;
    }
}
void celestialMotionShutdown() {
    for (auto& e:g_eyes) e=Eye{};
    g_build.Reset(); g_index.Reset(); g_indexOriginal.Reset(); g_draw.Reset(); g_batch.Reset(); g_blend.Reset(); g_depth.Reset();
    g_dump.Reset(); g_dumpCount=0;
    g_gpu.reset();g_costFrames=g_costDraws=g_originalDraws=0;
    g_buildGpu.reset();g_buildBatches=0;
    detail::g_celestialMotionFailed=g_noted=g_capNoted=false;
    for (auto& w:g_watched) { w.buffer=nullptr; w.valid=false; w.mapped=nullptr; }
    detail::g_celestialMotionAnyWatched = false;
}
// Hot: the game Maps roughly 1100 buffers a frame over terrain. Every tee
// starts with the pointer compares below and returns immediately once
// nothing further matches -- no GetDesc, no logging, no allocation.
void celestialMotionConstantsMapped(ID3D11Resource* resource, void* data) {
    for (auto& w:g_watched) if (resource==w.buffer) { w.mapped=data; return; }
}
void celestialMotionConstantsUnmapped(ID3D11Resource* resource) {
    // D3D11_MAP_WRITE_DISCARD hands back a whole new allocation, but only
    // this slot's own segment is ever copied out of it -- the same bytes
    // begin() reads from the shadow, never the rest of the buffer.
    for (int i=0;i<3;++i) if (resource==g_watched[i].buffer) {
        WatchedCb& w=g_watched[i]; const auto& seg=kSegment[i];
        if (w.mapped && w.bytes>=seg.srcOffset+seg.length) {
            LARGE_INTEGER t0{}; QueryPerformanceCounter(&t0);
            memcpy(w.shadow,static_cast<const uint8_t*>(w.mapped)+seg.srcOffset,seg.length);
            LARGE_INTEGER t1{}; QueryPerformanceCounter(&t1);
            if (t1.QuadPart>t0.QuadPart) g_teeTicks+=static_cast<uint64_t>(t1.QuadPart-t0.QuadPart);
            ++g_teeCalls;
            w.valid=true;
        }
        w.mapped=nullptr;
        return;
    }
}
void celestialMotionConstantsWritten(ID3D11Resource* resource, const void* data, const D3D11_BOX* box) {
    if (!data) return;  // the runtime rejects the call; nothing was written
    for (int i=0;i<3;++i) if (resource==g_watched[i].buffer) {
        WatchedCb& w=g_watched[i]; const auto& seg=kSegment[i];
        const UINT left=box?box->left:0u;
        const UINT right=box?box->right:w.bytes;
        if (right>w.bytes || left>right) return;
        // Copy only the overlap between the write and this slot's segment.
        const UINT s0=seg.srcOffset, s1=seg.srcOffset+seg.length;
        const UINT lo=left>s0?left:s0, hi=right<s1?right:s1;
        if (lo<hi) {
            LARGE_INTEGER t0{}; QueryPerformanceCounter(&t0);
            memcpy(w.shadow+(lo-s0),static_cast<const uint8_t*>(data)+(lo-left),hi-lo);
            LARGE_INTEGER t1{}; QueryPerformanceCounter(&t1);
            if (t1.QuadPart>t0.QuadPart) g_teeTicks+=static_cast<uint64_t>(t1.QuadPart-t0.QuadPart);
            ++g_teeCalls;
            // A partial write onto an invalid shadow leaves it invalid; only
            // a write covering the whole segment can make it valid again.
            if (lo==s0 && hi==s1) w.valid=true;
        }
        return;
    }
}
void celestialMotionConstantsUnknownWrite(ID3D11Resource* resource) {
    for (auto& w:g_watched) if (!resource || resource==w.buffer) { w.valid=false; w.mapped=nullptr; }
}
void celestialMotionConstantsCensus(unsigned* cpuSlots, unsigned* gpuSlots, unsigned* rewatches) {
    if (cpuSlots) *cpuSlots=g_cpuSlots;
    if (gpuSlots) *gpuSlots=g_gpuSlots;
    if (rewatches) *rewatches=g_rewatches;
}
void celestialMotionStageDump(ID3D11DeviceContext* ctx, ID3D11Texture2D* scene) {
    g_dump.Reset(); g_dumpCount=0;
    if (!detail::g_celestialMotionEnabled || detail::g_celestialMotionFailed) return;
    for (auto& e:g_eyes) if (e.scene.Get()==scene && e.cleared) {
        flush(ctx,e);
        const auto& r=e.records[e.write];
        if (!r.count) return;
        D3D11_BUFFER_DESC bd{}; bd.ByteWidth=kRecords*kRecordBytes;
        bd.Usage=D3D11_USAGE_STAGING; bd.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Device> dev; ctx->GetDevice(&dev);
        if (SUCCEEDED(dev->CreateBuffer(&bd,nullptr,&g_dump))) {
            ctx->CopyResource(g_dump.Get(),r.buffer.Get()); g_dumpCount=r.count;
        }
        return;
    }
}
void celestialMotionWriteDump(ID3D11DeviceContext* ctx,const wchar_t* directory,const wchar_t* stamp) {
    reportCost();
    if (!g_dump) {
        Log::get().note("terrain motion: eye run %ls has no terrain records.",stamp);
        return;
    }
    D3D11_MAPPED_SUBRESOURCE map{};
    HRESULT hr=ctx->Map(g_dump.Get(),0,D3D11_MAP_READ,D3D11_MAP_FLAG_DO_NOT_WAIT,&map);
    if (SUCCEEDED(hr)) {
        wchar_t path[MAX_PATH]; _snwprintf_s(path,MAX_PATH,_TRUNCATE,L"%s\\eye_%s_Terrain.bin",directory,stamp);
        FILE* file=nullptr; _wfopen_s(&file,path,L"wb"); bool ok=false;
        unsigned matched=0;
        for (unsigned i=0;i<g_dumpCount;++i) {
            const auto* p=reinterpret_cast<const float*>(static_cast<const char*>(map.pData)+i*kRecordBytes);
            if (p[55]==1) ++matched;
        }
        if (file) {
            const uint32_t header[2]={g_dumpCount,kRecordBytes};
            ok=fwrite("EDVRTRN1",1,8,file)==8 && fwrite(header,sizeof(header),1,file)==1 &&
                fwrite(map.pData,kRecordBytes,g_dumpCount,file)==g_dumpCount;
            fclose(file);
        }
        ctx->Unmap(g_dump.Get(),0);
        Log::get().note("terrain motion: eye run %ls matched %u/%u patch transforms; record file %s.",stamp,matched,g_dumpCount,ok?"written":"FAILED");
    } else Log::get().note("terrain motion: eye run %ls record readback unavailable (0x%08X).",stamp,static_cast<unsigned>(hr));
    g_dump.Reset(); g_dumpCount=0;
}
} // namespace edvr
