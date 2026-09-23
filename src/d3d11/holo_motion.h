#pragma once
// Draw-time motion for unskinned cockpit holograms. These world-space pool
// transforms include rounding and animation absent from the headset pose.
// The 07:45 comms capture registers to these transforms within 0.012 pixels.
// History keys name the surface/mesh, never the reordered pool slot.
#include <d3d11.h>
#include <wrl/client.h>
#include <atomic>
#include <cstdint>
#include <unordered_map>
#include <algorithm>
#include "shader_swap.h"
#include "eye_draw_snapshot.h"

namespace edvr {
// "Some HoloMotion may hold geometry", for ui_depth.h's inline write guard.
//
// resourceWritten below is a no-op for a non-null resource unless that
// resource is a key of `geometry`, and every Unmap, Copy and Update the game
// makes reaches it twice (both eyes' g_holoMotion) through
// uiDepthMotionResourceWritten -- an unordered_map find each, about 1100
// times a frame over terrain: 98 innermost samples of the 1355-frame window
// of 2026-09-22 (parked-5), the largest EDVR-owned item on the Unmap path.
// The maps are empty unless a smoke corona was accepted in the last few
// frames.
//
// Set TRUE at the one insertion site (prepare, corona path), before anything
// that can re-enter the write hooks; recomputed from g_holoMotion's own maps
// at uiDepthFrameBoundary, after their frameBoundary() pruning. Nothing else
// inserts, and erasures (pruning, clear, reassignment) can only empty a map,
// so FALSE always means g_holoMotion's maps are empty -- the only instances
// the guarded call reaches. Relaxed: every writer and reader is on the
// owner context's thread, the same thread that already touches the maps
// unlocked.
namespace detail {
inline std::atomic<bool> g_holoGeometryTracked{false};
}  // namespace detail

struct HoloDraw {
    char kind=0;
    uint32_t count=0, instances=0, start=0;
    int32_t base=0;
    uint32_t startInstance=0;
};

constexpr char kHoloMotionBuild[] = R"HLSL(
struct Record { uint4 key[8]; float4 clip[3]; float4 map[3]; float4 meta; };
cbuffer Model : register(b0) { float4 model[8]; }
cbuffer Scene : register(b1) { float4 scene[276]; }
cbuffer Material : register(b2) { float4 material[4]; }
cbuffer Draw : register(b3) { uint4 info; uint4 mesh[4]; float4 limits; }
StructuredBuffer<Record> Previous : register(t0);
struct PoolRecord { uint4 data[21]; };
StructuredBuffer<PoolRecord> Pool : register(t1);
ByteAddressBuffer Instance : register(t2);
RWStructuredBuffer<Record> Current : register(u0);
// Preserve the game's non-normalized UNORM16 quaternion operation exactly.
float3 turn(float4 q, float3 v) {
    return (2*q.w*q.w-1)*v + 2*dot(q.xyz,v)*q.xyz + 2*q.w*cross(q.xyz,v);
}
[numthreads(1,1,1)] void main(uint3 id : SV_DispatchThreadID) {
    Record n=(Record)0;
    // b2 animates the material's glow coordinates every frame; it does
    // not move vertices or the primary surface UVs and is not identity.
    [unroll] for(uint k=0;k<4;++k) n.key[k]=mesh[k];
    bool valid=true;
    if(info.z==1 || info.z==4 || info.z==5) {
        [unroll] for(uint r=0;r<3;++r) n.clip[r]=model[r==2?7:4+r];
        // Planet material constants shade the sphere; they are not geometry
        // identity. The captured surface's mesh and texture identify it.
        if(info.z==1) [unroll] for(uint k=0;k<4;++k) n.key[4+k]=asuint(material[k]);
        if(info.z==5) {
            n.key[4]=asuint(material[0].w);
            n.key[5]=asuint(material[1].x);
            n.key[6]=asuint(material[1].y);
            valid=all(isfinite(material[0].w)) && all(isfinite(material[1].xy)) &&
                  material[0].w>0 && material[1].x>0 && material[1].y>0;
        }
        valid=valid && all(abs(model[6].xyz)<1e-10) && model[6].w>0;
    } else {
        float3 scale,pos; float4 q;
        if(info.z==2) {
            uint at=id.x*60;
            pos=asfloat(Instance.Load3(at))-scene[275].xyz;
            q=asfloat(Instance.Load4(at+16)); scale=asfloat(Instance.Load3(at+32));
            n.key[4]=Instance.Load4(at+16); n.key[5]=uint4(Instance.Load3(at+32),Instance.Load(at+12));
            // Geometry lies at local Z=0. A comparable unused Z axis avoids
            // an ill-conditioned inverse for billion-metre orbital ellipses.
            scale.z=max(abs(scale.x),abs(scale.y));
            // The RGBA float4 at byte offset 44 is the instance
            // colour/alpha. It is useful only for disambiguating the
            // fallback below: intensity and alpha can animate, while
            // chromaticity identifies the stroke.
            n.key[6]=Instance.Load4(at+44);
        } else {
            uint index=Instance.Load(0), count,stride; Pool.GetDimensions(count,stride);
            if(index>=count) { Current[info.x]=n; return; }
            PoolRecord p=Pool[index]; valid=p.data[0].x==0;
            scale=asfloat(p.data[0].y);
            uint2 packed=p.data[0].zw;
            q=float4(packed.x&65535,packed.x>>16,packed.y&65535,packed.y>>16)*(2.0/65535.0)-1;
            pos=asfloat(p.data[1].xyz)-scene[275].xyz;
        }
        float3 x=turn(q,float3(scale.x,0,0)),y=turn(q,float3(0,scale.y,0)),z=turn(q,float3(0,0,scale.z));
        [unroll] for(uint r=0;r<3;++r) {
            uint row=r==2?3:r;
            float4 c=info.z==2 ? float4(scene[270][row],scene[271][row],scene[272][row],scene[273][row]) : model[r==2?7:4+r];
            n.clip[r]=float4(dot(c.xyz,x),dot(c.xyz,y),dot(c.xyz,z),dot(c,float4(pos,1)));
        }
        valid=valid && all(isfinite(scale)) && all(abs(scale)>1e-8) && all(isfinite(pos)) && abs(dot(q,q)-1)<.002;
        if(info.z==0) valid=valid && all(abs(model[6].xyz)<1e-10) && model[6].w>0 && n.clip[2].w<limits.x;
        // Sprite VS uses the same pool transform and clip X/Y/W, but forces
        // clip Z=W and may billboard at planetary distances. UV tiles name
        // distinct atlas quads; changing brightness does not move geometry.
        if(info.z==3) n.key[4]=asuint(material[1]);
    }
    valid=valid && n.clip[2].w>.025 && all(isfinite(n.clip[0])) && all(isfinite(n.clip[1])) && all(isfinite(n.clip[2]));
    float3 a=cross(n.clip[1].xyz,n.clip[2].xyz),b=cross(n.clip[2].xyz,n.clip[0].xyz),c=cross(n.clip[0].xyz,n.clip[1].xyz);
    float det=dot(n.clip[0].xyz,a);
    valid=valid && isfinite(det) && abs(det)>1e-12;
    n.meta=float4(valid?1:0,limits.yz,0);
    uint matches=0,match=0;
    [loop] for(uint i=0;i<info.y;++i) {
        Record old=Previous[i]; bool same=old.meta.x==1;
        [unroll] for(uint j=0;j<8;++j) {
            // Mode 2 keeps key[6] as diagnostic identity data, but colour
            // intensity/alpha changes must not make an otherwise exact
            // orbital record miss the existing strict path.
            if(info.z!=2 || j!=6) same=same && all(n.key[j]==old.key[j]);
        }
        // Identical meshes can occur on multiple panels. Require one nearby
        // projected origin; an ambiguous match or a newly opened panel declines.
        float2 here=float2(n.clip[0].w,n.clip[1].w)/n.clip[2].w;
        float2 there=float2(old.clip[0].w,old.clip[1].w)/old.clip[2].w;
        same=same && all(abs(here-there)<.2) && old.clip[2].w>n.clip[2].w*.5 && old.clip[2].w<n.clip[2].w*2;
        if(same) {
            // Identical ring passes may repeat the same geometry/material.
            // They are interchangeable only when their complete transforms
            // are identical. Cockpit ambiguity remains a hard rejection.
            bool duplicate=(info.z==1 || info.z==4) && matches==1;
            [unroll] for(uint row=0;row<3;++row) duplicate=duplicate && all(old.clip[row]==Previous[match].clip[row]);
            if(!duplicate) { ++matches; match=i; }
        }
    }
    // A changing orbital instance can alter its quaternion/scale while
    // retaining the same draw/mesh, width and colour family. Only use this
    // relaxed identity when the original exact search found no candidate;
    // every candidate must be unique and retain the same continuity checks.
    if(valid && info.z==2 && matches==0) {
        uint fallbackMatches=0,fallbackMatch=0;
        float3 newRgb=asfloat(n.key[6].xyz); float newMax=max(newRgb.x,max(newRgb.y,newRgb.z));
        bool newRgbValid=all(isfinite(newRgb)) && isfinite(newMax) && newMax>0;
        [loop] for(uint i=0;i<info.y;++i) {
            Record old=Previous[i]; bool candidate=old.meta.x==1;
            [unroll] for(uint j=0;j<4;++j) candidate=candidate && all(n.key[j]==old.key[j]);
            candidate=candidate && n.key[5].w==old.key[5].w;
            float3 oldRgb=asfloat(old.key[6].xyz); float oldMax=max(oldRgb.x,max(oldRgb.y,oldRgb.z));
            bool oldRgbValid=all(isfinite(oldRgb)) && isfinite(oldMax) && oldMax>0;
            bool sameChroma=false;
            if(newRgbValid && oldRgbValid) sameChroma=all(abs(newRgb/newMax-oldRgb/oldMax)<=1e-6);
            candidate=candidate && sameChroma;
            float2 here=float2(n.clip[0].w,n.clip[1].w)/n.clip[2].w;
            float2 there=float2(old.clip[0].w,old.clip[1].w)/old.clip[2].w;
            candidate=candidate && all(abs(here-there)<.2) && old.clip[2].w>n.clip[2].w*.5 && old.clip[2].w<n.clip[2].w*2;
            if(candidate) { ++fallbackMatches; fallbackMatch=i; }
        }
        if(fallbackMatches==1) { matches=1; match=fallbackMatch; }
    }
    if(valid && matches==1) {
        Record old=Previous[match]; float3 t=float3(n.clip[0].w,n.clip[1].w,n.clip[2].w);
        [unroll] for(uint row=0;row<3;++row) {
            float3 v=float3(dot(old.clip[row].xyz,a),dot(old.clip[row].xyz,b),dot(old.clip[row].xyz,c))/det;
            n.map[row]=float4(v,old.clip[row].w-dot(v,t));
        }
        n.meta.w=all(isfinite(n.map[0])) && all(isfinite(n.map[1])) && all(isfinite(n.map[2])) ? 1:0;
    }
    Current[info.x+id.x]=n;
}
)HLSL";

class HoloMotion {
    template<class T> using Ptr=Microsoft::WRL::ComPtr<T>;
    static constexpr unsigned kMax=128, kStride=15*16;
    struct History {
        Ptr<ID3D11Buffer> buffer;
        Ptr<ID3D11ShaderResourceView> srv;
        Ptr<ID3D11UnorderedAccessView> uav;
        // Retain every resource whose address participates in a history key.
        Ptr<IUnknown> sources[kMax][3];
        unsigned count=0;
    };
    History history[2];
    unsigned write=0,w=0,h=0;
    bool cleared=false,failed=false;
    struct GeometryStamp { unsigned epoch=0,seen=0; };
    std::unordered_map<ID3D11Resource*,GeometryStamp> geometry;
    unsigned geometryEpoch=1,unknownEpoch=0,frame=0;
    Ptr<ID3D11Texture2D> scene,coverage;
    // Optional world-only coverage twin.  It is deliberately separate from
    // the live HC surface: callers may omit a mode-3 sprite from the clean
    // colour path while retaining the normal UI motion inputs.
    Ptr<ID3D11Texture2D> separatedCoverage;
    ID3D11Texture2D* separatedScene=nullptr;
    bool separatedReady=false;
    Ptr<ID3D11RenderTargetView> rtv;
    Ptr<ID3D11BlendState> motionBlendState;
    Ptr<ID3D11ShaderResourceView> coverageSrv,instanceSrv,separatedCoverageSrv;
    Ptr<ID3D11RenderTargetView> separatedRtv;
    Ptr<ID3D11Buffer> draw,instance;
    Ptr<ID3D11ComputeShader> shader;
    bool create(ID3D11DeviceContext* ctx,ID3D11Device* dev,ID3D11Texture2D* source) {
        D3D11_TEXTURE2D_DESC td{}; source->GetDesc(&td);
        if(td.SampleDesc.Count!=1 || td.ArraySize!=1) return false;
        *this=HoloMotion{}; scene=source; w=td.Width; h=td.Height;
        td={}; td.Width=w; td.Height=h; td.MipLevels=td.ArraySize=td.SampleDesc.Count=1;
        td.Format=DXGI_FORMAT_R32G32_FLOAT; td.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;
        if(FAILED(dev->CreateTexture2D(&td,nullptr,&coverage)) || FAILED(dev->CreateRenderTargetView(coverage.Get(),nullptr,&rtv)) ||
           FAILED(dev->CreateShaderResourceView(coverage.Get(),nullptr,&coverageSrv))) return false;
        D3D11_BLEND_DESC blend{}; blend.IndependentBlendEnable=TRUE;
        blend.RenderTarget[0].RenderTargetWriteMask=D3D11_COLOR_WRITE_ENABLE_RED;
        blend.RenderTarget[1].BlendEnable=TRUE;
        blend.RenderTarget[1].SrcBlend=D3D11_BLEND_SRC_ALPHA;
        blend.RenderTarget[1].DestBlend=D3D11_BLEND_INV_SRC_ALPHA;
        blend.RenderTarget[1].BlendOp=D3D11_BLEND_OP_ADD;
        blend.RenderTarget[1].SrcBlendAlpha=D3D11_BLEND_ONE;
        blend.RenderTarget[1].DestBlendAlpha=D3D11_BLEND_INV_SRC_ALPHA;
        blend.RenderTarget[1].BlendOpAlpha=D3D11_BLEND_OP_ADD;
        blend.RenderTarget[1].RenderTargetWriteMask=D3D11_COLOR_WRITE_ENABLE_ALL;
        if(FAILED(dev->CreateBlendState(&blend,&motionBlendState))) return false;
        D3D11_BUFFER_DESC bd{}; bd.ByteWidth=kMax*kStride; bd.StructureByteStride=kStride;
        bd.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_STRUCTURED; bd.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_UNORDERED_ACCESS;
        for(auto& e:history) if(FAILED(dev->CreateBuffer(&bd,nullptr,&e.buffer)) || FAILED(dev->CreateShaderResourceView(e.buffer.Get(),nullptr,&e.srv)) ||
            FAILED(dev->CreateUnorderedAccessView(e.buffer.Get(),nullptr,&e.uav))) return false;
        bd={}; bd.ByteWidth=96; bd.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
        if(FAILED(dev->CreateBuffer(&bd,nullptr,&draw))) return false;
        bd={}; bd.ByteWidth=4096; bd.BindFlags=D3D11_BIND_SHADER_RESOURCE; bd.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
        if(FAILED(dev->CreateBuffer(&bd,nullptr,&instance))) return false;
        D3D11_SHADER_RESOURCE_VIEW_DESC sd{}; sd.Format=DXGI_FORMAT_R32_TYPELESS; sd.ViewDimension=D3D11_SRV_DIMENSION_BUFFEREX;
        sd.BufferEx.NumElements=1024; sd.BufferEx.Flags=D3D11_BUFFEREX_SRV_FLAG_RAW;
        if(FAILED(dev->CreateShaderResourceView(instance.Get(),&sd,&instanceSrv))) return false;
        shader.Attach(shaderSwapCompileCs(ctx,kHoloMotionBuild,sizeof(kHoloMotionBuild)-1,"main","holo motion",nullptr,"holo motion"));
        return shader!=nullptr;
    }
public:
    // Allocate the optional clean-world HC twin at the same size/format as
    // the live coverage.  No allocation occurs unless a caller explicitly
    // opts into separated coverage for the frame.
    bool ensureSeparated(ID3D11DeviceContext* ctx, ID3D11Texture2D* source) {
        if (!ctx || !source || scene.Get()!=source) return false;
        if (separatedCoverage && separatedRtv && separatedCoverageSrv && separatedScene==source) return true;
        Ptr<ID3D11Device> dev; ctx->GetDevice(&dev); if (!dev) return false;
        D3D11_TEXTURE2D_DESC td{}; source->GetDesc(&td);
        td.MipLevels=1; td.ArraySize=1;
        td.Format=DXGI_FORMAT_R32G32_FLOAT; td.SampleDesc.Count=1; td.SampleDesc.Quality=0;
        td.Usage=D3D11_USAGE_DEFAULT; td.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;
        td.CPUAccessFlags=0; td.MiscFlags=0;
        separatedCoverage.Reset(); separatedRtv.Reset(); separatedCoverageSrv.Reset();
        separatedScene=source; separatedReady=false;
        if (FAILED(dev->CreateTexture2D(&td,nullptr,&separatedCoverage)) ||
            FAILED(dev->CreateRenderTargetView(separatedCoverage.Get(),nullptr,&separatedRtv)) ||
            FAILED(dev->CreateShaderResourceView(separatedCoverage.Get(),nullptr,&separatedCoverageSrv))) {
            separatedCoverage.Reset(); separatedRtv.Reset(); separatedCoverageSrv.Reset(); separatedScene=nullptr; separatedReady=false; return false;
        }
        return true;
    }
    // Seed the clean twin from the live HC before the first separated draw.
    // This is a GPU copy; no readback and no change to the live surface.
    bool snapshotSeparated(ID3D11DeviceContext* ctx) {
        if (!ctx || !coverage || !separatedCoverage) return false;
        ctx->CopyResource(separatedCoverage.Get(), coverage.Get()); separatedReady=true; return true;
    }
    void invalidateSeparated() { separatedReady=false; }
    ID3D11RenderTargetView* separatedTarget() const { return separatedRtv.Get(); }
    ID3D11ShaderResourceView* separatedView() const { return separatedCoverageSrv.Get(); }
    bool separatedValid(ID3D11Texture2D* source) const {
        return separatedReady && separatedScene==source && separatedCoverageSrv && separatedRtv;
    }
    // Called only for the recognized holo VS/PS, one unskinned instance,
    // and the normal full-eye viewport. Does not replace the original draw.
    // mode 0: cockpit pool, 1: ring local-to-clip, 2: orbital instance stream,
    // 3: planar sprite pool (local Y=0, original VS forces clip Z=W),
    // 4: affine planet/solar local-to-clip, with no material animation key.
    // Mode 4 requires an explicit surface slot: planet t0 or solar art t1.
    // 5: exact corona streak affine clip rows; one draw, stride-44 layout,
    // surface slot 1, and CB2 width identity in the history key.
    bool prepare(ID3D11DeviceContext* ctx,ID3D11Texture2D* source,const HoloDraw& args,float metres,unsigned mode=0,unsigned surfaceSlot=2) {
        const unsigned count=mode==2?args.instances:1;
        const bool corona=mode==5;
        if(failed || !source || mode>5 || (mode==3 && args.count!=6) || (mode==2 ? (args.kind!='N' || args.startInstance!=0 || count==0 || count>64) : corona ? (args.kind!='X' || args.startInstance!=0 || args.instances!=1) : (args.kind!='X' || args.instances!=1)) || metres<=0 || ctx->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE) return false;
        if(corona) {
            Ptr<ID3D11GeometryShader> gs; Ptr<ID3D11HullShader> hs; Ptr<ID3D11DomainShader> ds; ID3D11ClassInstance* ci[256]{}; UINT nc=256;
            ctx->GSGetShader(&gs,ci,&nc); for(UINT i=0;i<nc;++i) if(ci[i]) ci[i]->Release(); nc=256; ctx->HSGetShader(&hs,ci,&nc); for(UINT i=0;i<nc;++i) if(ci[i]) ci[i]->Release(); nc=256; ctx->DSGetShader(&ds,ci,&nc); for(UINT i=0;i<nc;++i) if(ci[i]) ci[i]->Release();
            ID3D11Buffer* so[4]{}; ctx->SOGetTargets(4,so); bool stream=false; for(auto* p:so) { if(p) {stream=true;p->Release();} }
            ID3D11Predicate* pred=nullptr; BOOL predValue=FALSE; ctx->GetPredication(&pred,&predValue); if(pred) pred->Release();
            ID3D11UnorderedAccessView* uav[8]{}; ctx->OMGetRenderTargetsAndUnorderedAccessViews(0,nullptr,nullptr,0,8,uav); bool writable=false; for(auto* p:uav) { if(p) {writable=true;p->Release();} }
            if(gs || hs || ds || stream || pred || writable) return false;
        }
        const bool pooled=mode==0 || mode==3;
        D3D11_TEXTURE2D_DESC td{}; source->GetDesc(&td);
        UINT nvp=1; D3D11_VIEWPORT vp{}; ctx->RSGetViewports(&nvp,&vp);
        if(nvp!=1 || vp.TopLeftX!=0 || vp.TopLeftY!=0 || vp.Width!=td.Width || vp.Height!=td.Height || vp.MinDepth!=0 || vp.MaxDepth!=1) return false;
        Ptr<ID3D11Device> dev; ctx->GetDevice(&dev);
        if(scene.Get()!=source && !create(ctx,dev.Get(),source)) { failed=true; return false; }
        auto& now=history[write]; auto& prev=history[1-write]; if(now.count+count>kMax) return false;
        Ptr<ID3D11Buffer> vb[2],ib; UINT strides[2]{},offsets[2]{},ibOffset=0; DXGI_FORMAT fmt;
        ID3D11Buffer* rawVb[2]{}; ctx->IAGetVertexBuffers(0,2,rawVb,strides,offsets);
        for(int i=0;i<2;++i) vb[i].Attach(rawVb[i]);
        ctx->IAGetIndexBuffer(&ib,&fmt,&ibOffset);
        if(!vb[0] || (pooled && (!vb[1] || !ib || strides[0]!=8 || strides[1]!=40)) ||
           ((mode==1 || mode==4 || corona) && !ib) || (mode==4 && strides[0]!=12) || (mode==2 && (!vb[1] || strides[0]!=16 || strides[1]!=60)) || (corona && strides[0]!=44)) return false;
        if(corona) {
            const bool newVb=geometry.find(vb[0].Get())==geometry.end(), newIb=geometry.find(ib.Get())==geometry.end();
            if(geometry.size() + unsigned(newVb) + unsigned(newIb) > 256) return false;
            Ptr<ID3D11InputLayout> layout; ctx->IAGetInputLayout(&layout);
            EyeDrawSnapshot::Layout fields[32]{}; UINT bytes=sizeof(fields);
            if(!layout || FAILED(layout->GetPrivateData(EyeDrawSnapshot::effectLayoutKey(),&bytes,fields)) || bytes!=2*sizeof(fields[0])) return false;
            bool pos=false,uv=false;
            for(unsigned i=0;i<2;++i) {
                pos=pos || (_stricmp(fields[i].semantic,"POSITION")==0 && fields[i].index==0 && fields[i].slot==0 && fields[i].offset==0 && fields[i].format==DXGI_FORMAT_R32G32B32_FLOAT && fields[i].classification==D3D11_INPUT_PER_VERTEX_DATA);
                uv=uv || (_stricmp(fields[i].semantic,"TEXCOORD")==0 && fields[i].index==0 && fields[i].slot==0 && fields[i].offset==36 && fields[i].format==DXGI_FORMAT_R32G32_FLOAT && fields[i].classification==D3D11_INPUT_PER_VERTEX_DATA);
            }
            if(!pos || !uv) return false;
            D3D11_BUFFER_DESC vd{}; vb[0]->GetDesc(&vd); D3D11_BUFFER_DESC id{}; ib->GetDesc(&id);
            if((vd.BindFlags&D3D11_BIND_UNORDERED_ACCESS) || (vd.BindFlags&D3D11_BIND_STREAM_OUTPUT) || (id.BindFlags&D3D11_BIND_UNORDERED_ACCESS) || (id.BindFlags&D3D11_BIND_STREAM_OUTPUT)) return false;
        }
        unsigned stream=mode==2?1:0,bytes=mode==2?count*60:4;
        D3D11_BUFFER_DESC vd{}; vb[stream]->GetDesc(&vd);
        uint64_t at=uint64_t(offsets[stream])+uint64_t(args.startInstance)*strides[stream];
        if(mode!=1 && mode!=4 && at+bytes>vd.ByteWidth) return false;
        Ptr<ID3D11ShaderResourceView> pool,surface;
        if(mode==0 && surfaceSlot!=1 && surfaceSlot!=2) return false;
        if(mode==4 && surfaceSlot>1) return false;
        if(corona && surfaceSlot!=1) return false;
        if(mode!=2) ctx->PSGetShaderResources(mode==1?3:mode==3?0:surfaceSlot,1,&surface);
        if(pooled) {
            ctx->VSGetShaderResources(33,1,&pool); if(!pool || !surface) return false;
            D3D11_SHADER_RESOURCE_VIEW_DESC pd{}; pool->GetDesc(&pd); if(pd.ViewDimension!=D3D11_SRV_DIMENSION_BUFFER) return false;
            Ptr<ID3D11Resource> resource; pool->GetResource(&resource); Ptr<ID3D11Buffer> poolBuffer;
            if(FAILED(resource.As(&poolBuffer))) return false;
            D3D11_BUFFER_DESC pbd{}; poolBuffer->GetDesc(&pbd); if(pbd.StructureByteStride!=336) return false;
        }
        if((mode==1 || mode==4 || corona) && !surface) return false;
        ID3D11Buffer* cb[3]{}; ctx->VSGetConstantBuffers(0,3,cb);
        bool enough=true; const UINT minimum[3]={mode==2?0u:128u,276*16,(mode==2 || mode==4)?0u:corona?32u:mode==3?32u:64u};
        for(int i=0;i<3;++i) { D3D11_BUFFER_DESC bd{}; if(cb[i]) cb[i]->GetDesc(&bd); enough=enough && bd.ByteWidth>=minimum[i]; }
        if(!enough) { for(auto* p:cb) if(p) p->Release(); return false; }
        if(corona) { geometry[vb[0].Get()].seen=frame; geometry[ib.Get()].seen=frame; detail::g_holoGeometryTracked.store(true,std::memory_order_relaxed); }
        struct Data { UINT info[4],key[16]; float limits[4]; } data{};
        data.info[0]=now.count; data.info[1]=prev.count; data.info[2]=mode; data.info[3]=count;
        IUnknown* objects[3]={surface.Get(),vb[pooled?1:0].Get(),mode==2?nullptr:ib.Get()};
        for(int i=0;i<3;++i) { uint64_t v=reinterpret_cast<uint64_t>(objects[i]); data.key[2*i]=UINT(v); data.key[2*i+1]=UINT(v>>32); now.sources[now.count][i]=objects[i]; }
        data.key[6]=UINT(args.base); data.key[7]=args.start; data.key[8]=mode==2?0:UINT(fmt); data.key[9]=mode==2?0:ibOffset;
        data.key[10]=strides[pooled?1:0]; data.key[11]=offsets[pooled?1:0]; data.key[12]=args.count;
        data.key[14]=(mode==4 || corona)?surfaceSlot:0; data.key[15]=mode;
        if(corona) data.key[13]=std::max(geometry[vb[0].Get()].epoch,std::max(geometry[ib.Get()].epoch,unknownEpoch));
        data.limits[0]=metres; data.limits[1]=float(w); data.limits[2]=float(h);
        ctx->UpdateSubresource(draw.Get(),0,nullptr,&data,0,0);
        if(mode!=1 && mode!=4 && !corona) { D3D11_BOX box{UINT(at),0,0,UINT(at+bytes),1,1}; ctx->CopySubresourceRegion(instance.Get(),0,0,0,0,vb[stream].Get(),0,&box); }
        Ptr<ID3D11ComputeShader> saved; ID3D11ClassInstance* classes[256]{}; UINT nc=256;
        ID3D11Buffer* savedCb[4]{}; ID3D11ShaderResourceView* savedSrv[3]{}; Ptr<ID3D11UnorderedAccessView> savedUav;
        ctx->CSGetShader(&saved,classes,&nc); ctx->CSGetConstantBuffers(0,4,savedCb);
        ctx->CSGetShaderResources(0,3,savedSrv); ctx->CSGetUnorderedAccessViews(0,1,&savedUav);
        ID3D11Buffer* buildCb[4]={cb[0],cb[1],cb[2],draw.Get()};
        ID3D11ShaderResourceView* srvs[3]={prev.srv.Get(),pool.Get(),instanceSrv.Get()};
        ctx->CSSetShader(shader.Get(),nullptr,0); ctx->CSSetConstantBuffers(0,4,buildCb);
        ctx->CSSetShaderResources(0,3,srvs); ctx->CSSetUnorderedAccessViews(0,1,now.uav.GetAddressOf(),nullptr); ctx->Dispatch(count,1,1);
        ID3D11UnorderedAccessView* zeroUav=nullptr; ID3D11ShaderResourceView* zeroSrv[3]{};
        ctx->CSSetUnorderedAccessViews(0,1,&zeroUav,nullptr); ctx->CSSetShaderResources(0,3,zeroSrv);
        ctx->CSSetShaderResources(0,3,savedSrv); ctx->CSSetUnorderedAccessViews(0,1,savedUav.GetAddressOf(),nullptr);
        ctx->CSSetConstantBuffers(0,4,savedCb); ctx->CSSetShader(saved.Get(),classes,nc);
        for(UINT i=0;i<nc;++i) classes[i]->Release();
        for(auto* p:savedCb) if(p) p->Release(); for(auto* p:savedSrv) if(p) p->Release(); for(auto* p:cb) if(p) p->Release();
        now.count+=count;
        if(!cleared) { const float zero[4]{}; ctx->ClearRenderTargetView(rtv.Get(),zero); cleared=true; }
        return true;
    }
    ID3D11RenderTargetView* target() const { return rtv.Get(); }
    ID3D11BlendState* motionBlend() const { return motionBlendState.Get(); }
    // Mode-5 writes are tracked independently of mesh-motion history. The
    // write hook only touches resources registered by an accepted corona.
    bool resourceWritten(ID3D11Resource* resource,uint64_t first=0,uint64_t end=~uint64_t(0)) {
        if(first>=end) return true;
        if(!resource) {
            if(++geometryEpoch==0) { geometryEpoch=1; unknownEpoch=0; geometry.clear(); for(auto& hist:history){for(unsigned i=0;i<hist.count;++i)for(auto& p:hist.sources[i])p.Reset();hist.count=0;} cleared=false; }
            unknownEpoch=geometryEpoch; return true;
        }
        // Empty is the common state (no corona accepted lately): a find on an
        // empty map hashes the key and misses, so this answers the same.
        if(geometry.empty()) return false;
        auto it=geometry.find(resource); if(it==geometry.end()) return false;
        if(++geometryEpoch==0) { geometryEpoch=1; unknownEpoch=0; geometry.clear(); for(auto& hist:history){for(unsigned i=0;i<hist.count;++i)for(auto& p:hist.sources[i])p.Reset();hist.count=0;} cleared=false; return true; }
        it->second.epoch=geometryEpoch; return true;
    }
    // For detail::g_holoGeometryTracked's recompute (ui_depth.cpp).
    bool tracksGeometry() const { return !geometry.empty(); }
    void noteFrame() {
        ++frame;
        for(auto it=geometry.begin();it!=geometry.end();) {
            if(it->second.seen+2<frame) it=geometry.erase(it); else ++it;
        }
    }
    ID3D11Buffer* info() const { return draw.Get(); }
    unsigned recordCount() const { return history[write].count; }
    void views(ID3D11Texture2D* source,ID3D11ShaderResourceView** out) const {
        out[0]=out[1]=nullptr;
        if(!failed && cleared && scene.Get()==source) { out[0]=coverageSrv.Get(); out[1]=history[write].srv.Get(); }
    }
    void frameBoundary() {
        separatedReady=false;
        write=1-write; auto& next=history[write];
        for(unsigned i=0;i<next.count;++i) for(auto& p:next.sources[i]) p.Reset();
        next.count=0; cleared=false; noteFrame();
    }
};
} // namespace edvr
