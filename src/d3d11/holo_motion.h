#pragma once
// Draw-time motion for unskinned cockpit holograms. These world-space pool
// transforms include rounding and animation absent from the headset pose.
// The 07:45 comms capture registers to these transforms within 0.012 pixels.
// History keys name the surface/mesh, never the reordered pool slot.
#include <d3d11.h>
#include <wrl/client.h>
#include <cstdint>
#include "shader_swap.h"

namespace edvr {
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
    uint index=Instance.Load(0), count,stride; Pool.GetDimensions(count,stride);
    if(index>=count) { Current[info.x]=n; return; }
    PoolRecord p=Pool[index];
    float scale=asfloat(p.data[0].y);
    uint2 packed=p.data[0].zw;
    float4 q=float4(packed.x&65535,packed.x>>16,packed.y&65535,packed.y>>16)*(2.0/65535.0)-1;
    float3 pos=asfloat(p.data[1].xyz)-scene[275].xyz;
    float3 x=turn(q,float3(scale,0,0)),y=turn(q,float3(0,scale,0)),z=turn(q,float3(0,0,scale));
    [unroll] for(uint r=0;r<3;++r) {
        float4 c=model[r==2?7:4+r];
        n.clip[r]=float4(dot(c.xyz,x),dot(c.xyz,y),dot(c.xyz,z),dot(c,float4(pos,1)));
    }
    bool valid=p.data[0].x==0 && isfinite(scale) && abs(scale)>1e-8 &&
        all(isfinite(pos)) && abs(dot(q,q)-1)<.002 &&
        all(abs(model[6].xyz)<1e-10) && model[6].w>0 &&
        n.clip[2].w>.025 && n.clip[2].w<limits.x;
    float3 a=cross(n.clip[1].xyz,n.clip[2].xyz),b=cross(n.clip[2].xyz,n.clip[0].xyz),c=cross(n.clip[0].xyz,n.clip[1].xyz);
    float det=dot(n.clip[0].xyz,a);
    valid=valid && isfinite(det) && abs(det)>1e-12;
    n.meta=float4(valid?1:0,limits.yz,0);
    uint matches=0,match=0;
    [loop] for(uint i=0;i<info.y;++i) {
        Record old=Previous[i]; bool same=old.meta.x==1;
        [unroll] for(uint j=0;j<8;++j) same=same && all(n.key[j]==old.key[j]);
        // Identical meshes can occur on multiple panels. Require one nearby
        // projected origin; an ambiguous match or a newly opened panel declines.
        float2 here=float2(n.clip[0].w,n.clip[1].w)/n.clip[2].w;
        float2 there=float2(old.clip[0].w,old.clip[1].w)/old.clip[2].w;
        same=same && all(abs(here-there)<.2) && old.clip[2].w>n.clip[2].w*.5 && old.clip[2].w<n.clip[2].w*2;
        if(same) { ++matches; match=i; }
    }
    if(valid && matches==1) {
        Record old=Previous[match]; float3 t=float3(n.clip[0].w,n.clip[1].w,n.clip[2].w);
        [unroll] for(uint row=0;row<3;++row) {
            float3 v=float3(dot(old.clip[row].xyz,a),dot(old.clip[row].xyz,b),dot(old.clip[row].xyz,c))/det;
            n.map[row]=float4(v,old.clip[row].w-dot(v,t));
        }
        n.meta.w=all(isfinite(n.map[0])) && all(isfinite(n.map[1])) && all(isfinite(n.map[2])) ? 1:0;
    }
    Current[info.x]=n;
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
    Ptr<ID3D11Texture2D> scene,coverage;
    Ptr<ID3D11RenderTargetView> rtv;
    Ptr<ID3D11ShaderResourceView> coverageSrv,instanceSrv;
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
        D3D11_BUFFER_DESC bd{}; bd.ByteWidth=kMax*kStride; bd.StructureByteStride=kStride;
        bd.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_STRUCTURED; bd.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_UNORDERED_ACCESS;
        for(auto& e:history) if(FAILED(dev->CreateBuffer(&bd,nullptr,&e.buffer)) || FAILED(dev->CreateShaderResourceView(e.buffer.Get(),nullptr,&e.srv)) ||
            FAILED(dev->CreateUnorderedAccessView(e.buffer.Get(),nullptr,&e.uav))) return false;
        bd={}; bd.ByteWidth=96; bd.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
        if(FAILED(dev->CreateBuffer(&bd,nullptr,&draw))) return false;
        bd={}; bd.ByteWidth=16; bd.BindFlags=D3D11_BIND_SHADER_RESOURCE; bd.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
        if(FAILED(dev->CreateBuffer(&bd,nullptr,&instance))) return false;
        D3D11_SHADER_RESOURCE_VIEW_DESC sd{}; sd.Format=DXGI_FORMAT_R32_TYPELESS; sd.ViewDimension=D3D11_SRV_DIMENSION_BUFFEREX;
        sd.BufferEx.NumElements=4; sd.BufferEx.Flags=D3D11_BUFFEREX_SRV_FLAG_RAW;
        if(FAILED(dev->CreateShaderResourceView(instance.Get(),&sd,&instanceSrv))) return false;
        shader.Attach(shaderSwapCompileCs(ctx,kHoloMotionBuild,sizeof(kHoloMotionBuild)-1,"main","holo motion",nullptr,"holo motion"));
        return shader!=nullptr;
    }
public:
    // Called only for the recognized holo VS/PS, one unskinned instance,
    // and the normal full-eye viewport. Does not replace the original draw.
    bool prepare(ID3D11DeviceContext* ctx,ID3D11Texture2D* source,const HoloDraw& args,float metres) {
        if(failed || !source || args.kind!='X' || args.instances!=1 || metres<=0 || ctx->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE) return false;
        D3D11_TEXTURE2D_DESC td{}; source->GetDesc(&td);
        UINT nvp=1; D3D11_VIEWPORT vp{}; ctx->RSGetViewports(&nvp,&vp);
        if(nvp!=1 || vp.TopLeftX!=0 || vp.TopLeftY!=0 || vp.Width!=td.Width || vp.Height!=td.Height || vp.MinDepth!=0 || vp.MaxDepth!=1) return false;
        Ptr<ID3D11Device> dev; ctx->GetDevice(&dev);
        if(scene.Get()!=source && !create(ctx,dev.Get(),source)) { failed=true; return false; }
        auto& now=history[write]; auto& prev=history[1-write]; if(now.count>=kMax) return false;
        Ptr<ID3D11Buffer> vb[2],ib; UINT strides[2]{},offsets[2]{},ibOffset=0; DXGI_FORMAT fmt;
        ID3D11Buffer* rawVb[2]{}; ctx->IAGetVertexBuffers(0,2,rawVb,strides,offsets);
        for(int i=0;i<2;++i) vb[i].Attach(rawVb[i]);
        ctx->IAGetIndexBuffer(&ib,&fmt,&ibOffset);
        if(!vb[0] || !vb[1] || !ib || strides[0]!=8 || strides[1]!=40) return false;
        D3D11_BUFFER_DESC vd{}; vb[0]->GetDesc(&vd);
        uint64_t at=uint64_t(offsets[0])+uint64_t(args.startInstance)*strides[0]; if(at+4>vd.ByteWidth) return false;
        Ptr<ID3D11ShaderResourceView> pool,surface; ctx->VSGetShaderResources(33,1,&pool); ctx->PSGetShaderResources(2,1,&surface);
        if(!pool || !surface) return false;
        D3D11_SHADER_RESOURCE_VIEW_DESC pd{}; pool->GetDesc(&pd); if(pd.ViewDimension!=D3D11_SRV_DIMENSION_BUFFER) return false;
        Ptr<ID3D11Resource> resource; pool->GetResource(&resource); Ptr<ID3D11Buffer> poolBuffer;
        if(FAILED(resource.As(&poolBuffer))) return false;
        D3D11_BUFFER_DESC pbd{}; poolBuffer->GetDesc(&pbd); if(pbd.StructureByteStride!=336) return false;
        ID3D11Buffer* cb[3]{}; ctx->VSGetConstantBuffers(0,3,cb);
        bool enough=true; const UINT minimum[3]={128,276*16,64};
        for(int i=0;i<3;++i) { D3D11_BUFFER_DESC bd{}; if(cb[i]) cb[i]->GetDesc(&bd); enough=enough && bd.ByteWidth>=minimum[i]; }
        if(!enough) { for(auto* p:cb) if(p) p->Release(); return false; }
        struct Data { UINT info[4],key[16]; float limits[4]; } data{};
        data.info[0]=now.count; data.info[1]=prev.count;
        IUnknown* objects[3]={surface.Get(),vb[1].Get(),ib.Get()};
        for(int i=0;i<3;++i) { uint64_t v=reinterpret_cast<uint64_t>(objects[i]); data.key[2*i]=UINT(v); data.key[2*i+1]=UINT(v>>32); now.sources[now.count][i]=objects[i]; }
        data.key[6]=UINT(args.base); data.key[7]=args.start; data.key[8]=UINT(fmt); data.key[9]=ibOffset;
        data.key[10]=strides[1]; data.key[11]=offsets[1]; data.key[12]=args.count;
        data.limits[0]=metres; data.limits[1]=float(w); data.limits[2]=float(h);
        ctx->UpdateSubresource(draw.Get(),0,nullptr,&data,0,0);
        D3D11_BOX box{UINT(at),0,0,UINT(at+4),1,1}; ctx->CopySubresourceRegion(instance.Get(),0,0,0,0,vb[0].Get(),0,&box);
        Ptr<ID3D11ComputeShader> saved; ID3D11ClassInstance* classes[256]{}; UINT nc=256;
        ID3D11Buffer* savedCb[4]{}; ID3D11ShaderResourceView* savedSrv[3]{}; Ptr<ID3D11UnorderedAccessView> savedUav;
        ctx->CSGetShader(&saved,classes,&nc); ctx->CSGetConstantBuffers(0,4,savedCb);
        ctx->CSGetShaderResources(0,3,savedSrv); ctx->CSGetUnorderedAccessViews(0,1,&savedUav);
        ID3D11Buffer* buildCb[4]={cb[0],cb[1],cb[2],draw.Get()};
        ID3D11ShaderResourceView* srvs[3]={prev.srv.Get(),pool.Get(),instanceSrv.Get()};
        ctx->CSSetShader(shader.Get(),nullptr,0); ctx->CSSetConstantBuffers(0,4,buildCb);
        ctx->CSSetShaderResources(0,3,srvs); ctx->CSSetUnorderedAccessViews(0,1,now.uav.GetAddressOf(),nullptr); ctx->Dispatch(1,1,1);
        ID3D11UnorderedAccessView* zeroUav=nullptr; ID3D11ShaderResourceView* zeroSrv[3]{};
        ctx->CSSetUnorderedAccessViews(0,1,&zeroUav,nullptr); ctx->CSSetShaderResources(0,3,zeroSrv);
        ctx->CSSetShaderResources(0,3,savedSrv); ctx->CSSetUnorderedAccessViews(0,1,savedUav.GetAddressOf(),nullptr);
        ctx->CSSetConstantBuffers(0,4,savedCb); ctx->CSSetShader(saved.Get(),classes,nc);
        for(UINT i=0;i<nc;++i) classes[i]->Release();
        for(auto* p:savedCb) if(p) p->Release(); for(auto* p:savedSrv) if(p) p->Release(); for(auto* p:cb) p->Release();
        ++now.count;
        if(!cleared) { const float zero[4]{}; ctx->ClearRenderTargetView(rtv.Get(),zero); cleared=true; }
        return true;
    }
    ID3D11RenderTargetView* target() const { return rtv.Get(); }
    ID3D11Buffer* info() const { return draw.Get(); }
    unsigned recordCount() const { return history[write].count; }
    void views(ID3D11Texture2D* source,ID3D11ShaderResourceView** out) const {
        out[0]=out[1]=nullptr;
        if(!failed && cleared && scene.Get()==source) { out[0]=coverageSrv.Get(); out[1]=history[write].srv.Get(); }
    }
    void frameBoundary() {
        write=1-write; auto& next=history[write];
        for(unsigned i=0;i<next.count;++i) for(auto& p:next.sources[i]) p.Reset();
        next.count=0; cleared=false;
    }
};
} // namespace edvr
