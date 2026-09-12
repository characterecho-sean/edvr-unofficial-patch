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

namespace edvr {
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
cbuffer Model : register(b0) { float4 model[13]; }
cbuffer Scene : register(b1) { float4 scene[280]; }
cbuffer Terrain : register(b2) { float4 patch[18]; }
cbuffer Draw : register(b3) { uint4 info; uint4 texKey[2]; }
StructuredBuffer<Record> Previous : register(t0);
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
[numthreads(64,1,1)] void main(uint lane : SV_GroupIndex) {
    Record n = (Record)0;
    n.key[0] = asuint(patch[0]);
    [unroll] for (uint k=0;k<5;++k) n.key[k+1] = asuint(patch[k+3]);
    [unroll] for (uint k=0;k<4;++k) n.key[k+6] = asuint(patch[k+10]);
    n.key[10] = texKey[0]; n.key[11] = texKey[1];
    n.q = normalize(patch[8]); n.t = float4(patch[1].xyz,0);
    // Confirm this shader's local-to-clip chain still reduces to view-space
    // perspective: scene[270..273] * model[9..11] == model[4..7].
    bool valid = all(isfinite(n.q)) && all(isfinite(n.t)) &&
        abs(dot(patch[8],patch[8])-1) < 0.002 && model[6].w > 0 &&
        abs(model[7].z-1) < 0.0001 && abs(model[6].z) < 0.0001;
    [unroll] for (uint c=0;c<4;++c) {
        float4 col = scene[270]*model[9][c] + scene[271]*model[10][c] +
                     scene[272]*model[11][c] + (c==3 ? scene[273] : 0);
        float4 expected = float4(model[4][c],model[5][c],model[6][c],model[7][c]);
        valid = valid && all(abs(col-expected) < 0.0002);
    }
    uint found=0, match=0;
    [loop] for (uint i=lane;i<info.y;i+=64) {
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
    Current[info.x]=n;
}
)HLSL";
constexpr char kIndexHlsl[] = R"HLSL(
cbuffer Draw : register(b13) { uint4 info; uint4 texKey[2]; }
uint main(float4 p:SV_Position):SV_Target { return info.x+1; }
)HLSL";

struct Records {
    ComPtr<ID3D11Buffer> buffer;
    ComPtr<ID3D11ShaderResourceView> srv;
    ComPtr<ID3D11UnorderedAccessView> uav;
    ComPtr<ID3D11ShaderResourceView> sources[kRecords][4];
    unsigned count=0;
};
struct Eye {
    ComPtr<ID3D11Texture2D> scene, index, depth;
    ComPtr<ID3D11RenderTargetView> rtv;
    ComPtr<ID3D11DepthStencilView> dsv;
    ComPtr<ID3D11ShaderResourceView> indexSrv, depthSrv;
    Records records[2];
    unsigned write=0, width=0, height=0;
    bool cleared=false;
};
Eye g_eyes[2];
bool g_enabled=false, g_failed=false, g_noted=false, g_capNoted=false;
ComPtr<ID3D11ComputeShader> g_build;
ComPtr<ID3D11PixelShader> g_index;
ComPtr<ID3D11Buffer> g_draw;
ComPtr<ID3D11BlendState> g_blend;
ComPtr<ID3D11DepthStencilState> g_depth;
ComPtr<ID3D11Buffer> g_dump;
unsigned g_dumpCount=0;
GpuIntervals<16> g_gpu;
unsigned g_costFrames=0,g_costDraws=0;
void reportCost() {
    const auto& t=g_gpu.totals;
    if(g_costDraws || t.samples || t.invalid || t.skipped)
        Log::get().note("terrain motion GPU: %u patch reissues in %u frames; completed=%u skipped=%u invalid=%u, %.3f us/patch (transform search, coverage reissue and restore; every 64th draw; no wait/flush; separate from EDVR-at-door GPU).",
            g_costDraws,g_costFrames,t.samples,t.skipped,t.invalid,t.samples?t.ms*1000/t.samples:0.0);
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
    return SUCCEEDED(dev->CreateBuffer(&bd,nullptr,&r.buffer)) &&
        SUCCEEDED(dev->CreateShaderResourceView(r.buffer.Get(),nullptr,&r.srv)) &&
        SUCCEEDED(dev->CreateUnorderedAccessView(r.buffer.Get(),nullptr,&r.uav));
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
    td.Format=DXGI_FORMAT_R32_TYPELESS; td.BindFlags=D3D11_BIND_DEPTH_STENCIL|D3D11_BIND_SHADER_RESOURCE;
    D3D11_DEPTH_STENCIL_VIEW_DESC dd{}; dd.Format=DXGI_FORMAT_D32_FLOAT; dd.ViewDimension=D3D11_DSV_DIMENSION_TEXTURE2D;
    D3D11_SHADER_RESOURCE_VIEW_DESC sd{}; sd.Format=DXGI_FORMAT_R32_FLOAT;
    sd.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2D; sd.Texture2D.MipLevels=1;
    return SUCCEEDED(dev->CreateTexture2D(&td,nullptr,&e.depth)) &&
        SUCCEEDED(dev->CreateDepthStencilView(e.depth.Get(),&dd,&e.dsv)) &&
        SUCCEEDED(dev->CreateShaderResourceView(e.depth.Get(),&sd,&e.depthSrv)) &&
        createRecords(dev,e.records[0]) && createRecords(dev,e.records[1]);
}
bool ensure(ID3D11DeviceContext* ctx, ID3D11Device* dev) {
    if (g_build && g_index && g_draw && g_blend && g_depth) return true;
    g_build.Attach(shaderSwapCompileCs(ctx,kBuildHlsl,sizeof(kBuildHlsl)-1,"main","terrain motion",nullptr,"terrain motion"));
    g_index.Attach(shaderSwapCompilePs(ctx,kIndexHlsl,sizeof(kIndexHlsl)-1,"main","terrain coverage",nullptr,"terrain motion"));
    D3D11_BUFFER_DESC bd{}; bd.ByteWidth=48; bd.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
    D3D11_BLEND_DESC blend{}; blend.RenderTarget[0].RenderTargetWriteMask=D3D11_COLOR_WRITE_ENABLE_RED;
    D3D11_DEPTH_STENCIL_DESC depth{}; depth.DepthEnable=TRUE;
    depth.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ALL; depth.DepthFunc=D3D11_COMPARISON_GREATER_EQUAL;
    return g_build && g_index && SUCCEEDED(dev->CreateBuffer(&bd,nullptr,&g_draw)) &&
        SUCCEEDED(dev->CreateBlendState(&blend,&g_blend)) && SUCCEEDED(dev->CreateDepthStencilState(&depth,&g_depth));
}
void fail() {
    g_failed=true;
    Log::get().note("terrain motion: resource setup failed; original scene and camera motion retained.");
}
} // namespace

void celestialMotionConfigure(bool enabled) {
    if (g_enabled!=enabled) celestialMotionShutdown();
    g_enabled=enabled;
}
bool celestialMotionBegin(ID3D11DeviceContext* ctx, uint64_t vs) {
    if (!g_enabled || g_failed || vs!=kTerrainDepth || g_saved.active) return false;
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
    Records& now=e.records[e.write]; Records& prev=e.records[1-e.write];
    if (now.count==kRecords) {
        if (!g_capNoted) { g_capNoted=true; Log::get().note("terrain motion: 512 draws per eye reached; excess patches retain camera motion."); }
        return false;
    }
    ID3D11Buffer* cb[3]{}; ctx->VSGetConstantBuffers(0,3,cb);
    bool enough=true;
    const UINT minimum[3]={13*16,280*16,18*16};
    for (int i=0;i<3;++i) {
        D3D11_BUFFER_DESC bd{}; if (cb[i]) cb[i]->GetDesc(&bd);
        enough=enough && bd.ByteWidth>=minimum[i];
    }
    if (!enough) { for (auto* b:cb) if (b) b->Release(); return false; }
    if((++g_costDraws&63u)==0)g_saved.timed=g_gpu.begin(ctx);
    ID3D11ShaderResourceView* sources[4]{}; ctx->VSGetShaderResources(0,4,sources);
    UINT data[12]={now.count,prev.count,0,0};
    for (int i=0;i<4;++i) {
        const auto key=reinterpret_cast<uint64_t>(sources[i]);
        data[4+i*2]=static_cast<UINT>(key); data[5+i*2]=static_cast<UINT>(key>>32);
        now.sources[now.count][i].Attach(sources[i]);
    }
    ctx->UpdateSubresource(g_draw.Get(),0,nullptr,data,0,0);
    // Preserve every touched compute binding, including dynamic linkage.
    ComPtr<ID3D11ComputeShader> cs; ID3D11ClassInstance* classes[256]{}; UINT nc=256;
    ID3D11Buffer* savedCb[4]{}; ComPtr<ID3D11ShaderResourceView> srv;
    ComPtr<ID3D11UnorderedAccessView> uav;
    ctx->CSGetShader(&cs,classes,&nc); ctx->CSGetConstantBuffers(0,4,savedCb);
    ctx->CSGetShaderResources(0,1,&srv); ctx->CSGetUnorderedAccessViews(0,1,&uav);
    ID3D11Buffer* buildCb[4]={cb[0],cb[1],cb[2],g_draw.Get()};
    ctx->CSSetShader(g_build.Get(),nullptr,0); ctx->CSSetConstantBuffers(0,4,buildCb);
    ctx->CSSetShaderResources(0,1,prev.srv.GetAddressOf());
    ctx->CSSetUnorderedAccessViews(0,1,now.uav.GetAddressOf(),nullptr);
    ctx->Dispatch(1,1,1);
    ID3D11UnorderedAccessView* nullUav=nullptr; ID3D11ShaderResourceView* nullSrv=nullptr;
    ctx->CSSetUnorderedAccessViews(0,1,&nullUav,nullptr); ctx->CSSetShaderResources(0,1,&nullSrv);
    ctx->CSSetShaderResources(0,1,srv.GetAddressOf()); ctx->CSSetUnorderedAccessViews(0,1,uav.GetAddressOf(),nullptr);
    ctx->CSSetConstantBuffers(0,4,savedCb); ctx->CSSetShader(cs.Get(),classes,nc);
    for (UINT i=0;i<nc;++i) classes[i]->Release();
    for (auto* b:savedCb) if (b) b->Release();
    for (auto* b:cb) b->Release();
    ++now.count;
    if (!e.cleared) {
        const float zero[4]{}; ctx->ClearRenderTargetView(e.rtv.Get(),zero);
        ctx->ClearDepthStencilView(e.dsv.Get(),D3D11_CLEAR_DEPTH,0,0); e.cleared=true;
    }
    ctx->OMGetRenderTargets(8,g_saved.rt,&g_saved.ds);
    ctx->PSGetShader(&g_saved.ps,g_saved.classes,&g_saved.classCount);
    ctx->PSGetConstantBuffers(13,1,&g_saved.cb);
    ctx->OMGetBlendState(&g_saved.blend,g_saved.factor,&g_saved.sampleMask);
    ctx->OMGetDepthStencilState(&g_saved.depth,&g_saved.stencil);
    vScreenSetRenderTargetsRaw(ctx,1,e.rtv.GetAddressOf(),e.dsv.Get());
    ctx->OMSetBlendState(g_blend.Get(),nullptr,0xffffffff);
    ctx->OMSetDepthStencilState(g_depth.Get(),0);
    ctx->PSSetShader(g_index.Get(),nullptr,0); ctx->PSSetConstantBuffers(13,1,g_draw.GetAddressOf());
    g_saved.active=true;
    if (!g_noted) {
        g_noted=true;
        Log::get().note("terrain motion: draw-time GPU transform history and private coverage active (%ux%u); 512 patches per eye, TAA/DLSS; missing or changed patches retain camera motion.",td.Width,td.Height);
    }
    return true;
}
void celestialMotionEnd(ID3D11DeviceContext* ctx) {
    if (!g_saved.active) return;
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
void celestialMotionFrameBoundary(ID3D11DeviceContext* ctx) {
    if (!g_enabled) return;
    if(ctx)g_gpu.poll(ctx);
    if(++g_costFrames%1800==0)reportCost();
    for (auto& e:g_eyes) {
        e.write=1-e.write; Records& next=e.records[e.write];
        for (unsigned i=0;i<next.count;++i) for (auto& view:next.sources[i]) view.Reset();
        next.count=0; e.cleared=false;
    }
}
void celestialMotionViews(ID3D11Texture2D* scene, ID3D11ShaderResourceView** views) {
    views[0]=views[1]=views[2]=nullptr;
    if (!g_enabled || g_failed || !scene) return;
    for (auto& e:g_eyes) if (e.scene.Get()==scene && e.cleared && e.records[e.write].count) {
        views[0]=e.indexSrv.Get(); views[1]=e.depthSrv.Get(); views[2]=e.records[e.write].srv.Get();
        return;
    }
}
void celestialMotionShutdown() {
    for (auto& e:g_eyes) e=Eye{};
    g_build.Reset(); g_index.Reset(); g_draw.Reset(); g_blend.Reset(); g_depth.Reset();
    g_dump.Reset(); g_dumpCount=0;
    g_gpu={};g_costFrames=g_costDraws=0;
    g_failed=g_noted=g_capNoted=false;
}
void celestialMotionStageDump(ID3D11DeviceContext* ctx, ID3D11Texture2D* scene) {
    g_dump.Reset(); g_dumpCount=0;
    if (!g_enabled || g_failed) return;
    for (auto& e:g_eyes) if (e.scene.Get()==scene && e.cleared) {
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
