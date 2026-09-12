#pragma once
#include <d3d11.h>
#include <wrl/client.h>
#include <cstdint>
#include <utility>
#include "shader_swap.h"
#include "gpu_interval.h"

namespace edvr {
// Compare the UI before projection. Screen-space colour differences confuse
// actual edits with head movement, jitter and the scene behind a translucent
// panel. Exact comparison is safe here: both views decode identical formats.
constexpr char kUiContentCs[] = R"HLSL(
Texture2D<float4> Current:register(t0), Before:register(t1);
Texture2D<float> Age:register(t2);
RWTexture2D<float> Next:register(u0);
[numthreads(8,8,1)] void main(uint3 id:SV_DispatchThreadID){
    uint w,h;Next.GetDimensions(w,h);if(any(id.xy>=uint2(w,h)))return;
    float4 a=Current.Load(int3(id.xy,0)),b=Before.Load(int3(id.xy,0));
    a.rgb*=a.a;b.rgb*=b.a; // RGB behind zero alpha is not visible content.
    Next[id.xy]=any(a!=b)?1:max(Age.Load(int3(id.xy,0))-1.0/32.0,0);
}
)HLSL";

// Used by the existing coverage draw with exactly the game's surface UVs.
// Max over the sample footprint retains erased, low-alpha and subpixel edits.
#define EDVR_UI_CHANGE_INPUT R"HLSL(
Texture2D<float> UiEdits:register(t14);
float uiEdit(float2 uv){
    uint w,h;UiEdits.GetDimensions(w,h);if(w==0||h==0)return 0;
    int2 p=int2(floor(uv*float2(w,h)-.5));float edit=0;
    [unroll]for(int y=0;y<2;++y)[unroll]for(int x=0;x<2;++x)
        edit=max(edit,UiEdits.Load(int3(clamp(p+int2(x,y),0,int2(w,h)-1),0)));
    return edit;
}
)HLSL"

class UiContent {
    template<class T> using Ptr=Microsoft::WRL::ComPtr<T>;
    struct Entry {
        Ptr<ID3D11Texture2D> source, before, age[2];
        Ptr<ID3D11ShaderResourceView> beforeView, view[2];
        Ptr<ID3D11UnorderedAccessView> out[2];
        DXGI_FORMAT format=DXGI_FORMAT_UNKNOWN;
        uint32_t frame=0, bytes=0;
        unsigned read=0;
        bool ready=false;
    };
    Entry entries[24];
    Ptr<ID3D11ComputeShader> shader;
    bool failed=false;
public:
    static constexpr uint32_t kBudget=64*1024*1024;
    struct Totals {uint32_t updates=0, hits=0, declined=0, evicted=0, resets=0;} totals;
    uint32_t allocated=0;
    GpuIntervals<32> gpu;

    ID3D11ShaderResourceView* prepare(ID3D11DeviceContext* ctx,
                                     ID3D11ShaderResourceView* surface,uint32_t frame,bool sample=false) {
        if(!ctx || !surface || failed) {++totals.declined;return nullptr;}
        Ptr<ID3D11Resource> resource;surface->GetResource(&resource);
        Ptr<ID3D11Texture2D> source;
        if(FAILED(resource.As(&source))) {++totals.declined;return nullptr;}
        D3D11_TEXTURE2D_DESC td{};source->GetDesc(&td);
        D3D11_SHADER_RESOURCE_VIEW_DESC sd{};surface->GetDesc(&sd);
        unsigned bpp=0;
        switch(sd.Format){
        case DXGI_FORMAT_R8G8B8A8_UNORM:case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        case DXGI_FORMAT_B8G8R8A8_UNORM:case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:bpp=4;break;
        case DXGI_FORMAT_R16G16B16A16_FLOAT:bpp=8;break;
        default:break;
        }
        const uint64_t pixels=uint64_t(td.Width)*td.Height,bytes=pixels*(bpp+2);
        // Only GPU-rendered UI surfaces, never a static sprite/glyph atlas.
        if(!bpp || !(td.BindFlags&D3D11_BIND_RENDER_TARGET) || td.ArraySize!=1 ||
           td.SampleDesc.Count!=1 || sd.ViewDimension!=D3D11_SRV_DIMENSION_TEXTURE2D ||
           sd.Texture2D.MostDetailedMip!=0 || td.MipLevels!=1 || bytes>kBudget) {
            ++totals.declined;return nullptr;
        }
        Entry* found=nullptr;
        for(auto& e:entries) if(e.source.Get()==source.Get() && e.format==sd.Format) {found=&e;break;}
        if(found && found->ready && found->frame==frame) {++totals.hits;return found->view[found->read].Get();}
        Ptr<ID3D11Device> dev;ctx->GetDevice(&dev);
        if(!shader) shader.Attach(shaderSwapCompileCs(ctx,kUiContentCs,sizeof(kUiContentCs)-1,"main","UI source edits",nullptr,"ui content"));
        if(!shader) {failed=true;++totals.declined;return nullptr;}
        if(!found){
            while(!found){
                if(allocated+bytes<=kBudget) for(auto& e:entries) if(!e.source) {found=&e;break;}
                if(found)break;
                Entry* oldest=nullptr;
                for(auto& e:entries) if(e.source && e.frame!=frame && (!oldest || frame-e.frame>frame-oldest->frame))oldest=&e;
                if(!oldest){++totals.declined;return nullptr;}
                allocated-=oldest->bytes;*oldest={};++totals.evicted;
            }
            Entry e;e.source=source;e.format=sd.Format;e.bytes=uint32_t(bytes);
            td.Usage=D3D11_USAGE_DEFAULT;td.CPUAccessFlags=td.MiscFlags=0;
            td.BindFlags=D3D11_BIND_SHADER_RESOURCE;
            sd.Texture2D.MipLevels=1;
            if(FAILED(dev->CreateTexture2D(&td,nullptr,&e.before)) ||
               FAILED(dev->CreateShaderResourceView(e.before.Get(),&sd,&e.beforeView))) {++totals.declined;return nullptr;}
            td.Format=DXGI_FORMAT_R8_UNORM;td.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_UNORDERED_ACCESS;
            for(unsigned i=0;i<2;++i) if(FAILED(dev->CreateTexture2D(&td,nullptr,&e.age[i])) ||
                FAILED(dev->CreateShaderResourceView(e.age[i].Get(),nullptr,&e.view[i])) ||
                FAILED(dev->CreateUnorderedAccessView(e.age[i].Get(),nullptr,&e.out[i]))) {++totals.declined;return nullptr;}
            *found=std::move(e);allocated+=found->bytes;
        }
        auto& e=*found;
        if(sample)gpu.begin(ctx);
        if(!e.ready || frame-e.frame!=1){
            const float zero[4]{};ctx->ClearUnorderedAccessViewFloat(e.out[e.read].Get(),zero);++totals.resets;
        }else{
            Ptr<ID3D11ComputeShader> saved;ID3D11ClassInstance* classes[256]{};UINT nc=256;
            ID3D11ShaderResourceView* savedSrv[3]{};Ptr<ID3D11UnorderedAccessView> savedUav;
            ctx->CSGetShader(&saved,classes,&nc);ctx->CSGetShaderResources(0,3,savedSrv);ctx->CSGetUnorderedAccessViews(0,1,&savedUav);
            ID3D11ShaderResourceView* in[3]={surface,e.beforeView.Get(),e.view[e.read].Get()};
            ctx->CSSetShader(shader.Get(),nullptr,0);ctx->CSSetShaderResources(0,3,in);
            ctx->CSSetUnorderedAccessViews(0,1,e.out[1-e.read].GetAddressOf(),nullptr);ctx->Dispatch((td.Width+7)/8,(td.Height+7)/8,1);
            ID3D11UnorderedAccessView* zeroUav=nullptr;ID3D11ShaderResourceView* zeroSrv[3]{};
            ctx->CSSetUnorderedAccessViews(0,1,&zeroUav,nullptr);ctx->CSSetShaderResources(0,3,zeroSrv);
            ctx->CSSetShaderResources(0,3,savedSrv);ctx->CSSetUnorderedAccessViews(0,1,savedUav.GetAddressOf(),nullptr);ctx->CSSetShader(saved.Get(),classes,nc);
            for(UINT i=0;i<nc;++i)classes[i]->Release();for(auto* p:savedSrv)if(p)p->Release();
            e.read=1-e.read;++totals.updates;
        }
        // Preserve the version sampled by this frame's first composite,
        // before the game repaints it. Shared by both eyes and mesh draws.
        ctx->CopySubresourceRegion(e.before.Get(),0,0,0,0,source.Get(),0,nullptr);
        if(sample)gpu.end(ctx);
        e.frame=frame;e.ready=true;return e.view[e.read].Get();
    }
    void retire(uint32_t frame){
        for(auto& e:entries) if(e.source && frame-e.frame>120){allocated-=e.bytes;e={};++totals.evicted;}
    }
    void reset() noexcept {
        gpu.reset();
        for(auto& e:entries) e={};
        shader.Reset(); failed=false; totals={}; allocated=0;
    }
};
} // namespace edvr
