#pragma once
#include "holo_motion.h"
#include "vscreen.h"

namespace edvr {
constexpr uint64_t kPlanetSurfaceVs=0x71DD9863DCFC0986ull,kPlanetSurfacePs=0x43E5E6EB67AC751Bull;
constexpr uint64_t kSolarSurfaceVs=0x4D516EF05C68FFA5ull,kSolarSurfacePs=0x147E748F4CD3AE9Aull;
// Original VS and raster state, original visible depth, no game colour/depth
// writes. The planet PS has no discard/depth export. The solar colour PS
// discards against linear scene depth, but its prepass writes (B-.001)/W;
// only surviving colour samples write B/W. EQUAL at the original colour
// depth therefore inherits that visibility without resampling its t0.
// Later cockpit/UI depth still wins in the temporal consumer. No distance
// cutoff or readback is involved. The caller verifies the exact VS/PS pair.
constexpr char kPlanetCoverageHlsl[]=R"HLSL(
cbuffer Motion:register(b12){uint4 info;}
float2 main(float4 p:SV_Position):SV_Target{return float2(info.x+1,p.z);}
)HLSL";
class PlanetCoverage {
    template<class T>using Ptr=Microsoft::WRL::ComPtr<T>;
    Ptr<ID3D11PixelShader> shader;
    Ptr<ID3D11DepthStencilState> equal;
    Ptr<ID3D11BlendState> write;
    Ptr<ID3D11DepthStencilView> savedDepth;
    Ptr<ID3D11DepthStencilState> savedDs;
    Ptr<ID3D11BlendState> savedBlend;
    Ptr<ID3D11PixelShader> savedPs;
    Ptr<ID3D11Buffer> savedInfo;
    ID3D11ClassInstance* classes[256]{};
    ID3D11RenderTargetView* targets[8]{};
    UINT classCount=0,reference=0,sampleMask=0;
    float factors[4]{};
    bool active=false;
public:
    bool begin(ID3D11DeviceContext* ctx,ID3D11Texture2D* scene,HoloMotion& motion,const HoloDraw& draw,bool solar=false) {
        if(active || !ctx || !scene || draw.kind!='X' || draw.instances!=1 || !draw.count || draw.count%3)return false;
        Ptr<ID3D11Predicate> predicate;BOOL value=FALSE;ctx->GetPredication(&predicate,&value);if(predicate)return false;
        Ptr<ID3D11GeometryShader> gs;Ptr<ID3D11HullShader> hs;Ptr<ID3D11DomainShader> ds;
        ctx->GSGetShader(&gs,nullptr,nullptr);ctx->HSGetShader(&hs,nullptr,nullptr);ctx->DSGetShader(&ds,nullptr,nullptr);
        if(gs || hs || ds)return false;
        ID3D11UnorderedAccessView* uavs[8]{};ctx->OMGetRenderTargetsAndUnorderedAccessViews(0,nullptr,nullptr,0,8,uavs);
        bool busy=false;for(auto* p:uavs)if(p){busy=true;p->Release();}
        ID3D11Buffer* so[4]{};ctx->SOGetTargets(4,so);for(auto* p:so)if(p){busy=true;p->Release();}
        if(busy)return false;
        Ptr<ID3D11DepthStencilState> depth;UINT ref=0;ctx->OMGetDepthStencilState(&depth,&ref);
        D3D11_DEPTH_STENCIL_DESC dd{};if(depth)depth->GetDesc(&dd);else{dd.DepthEnable=TRUE;dd.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ALL;}
        if(!dd.DepthEnable || dd.DepthWriteMask!=D3D11_DEPTH_WRITE_MASK_ALL)return false;
        Ptr<ID3D11BlendState> blend;ctx->OMGetBlendState(&blend,nullptr,nullptr);
        D3D11_BLEND_DESC bd{};if(blend)blend->GetDesc(&bd);if(bd.RenderTarget[0].BlendEnable && !solar)return false;
        if(!shader) {
            Ptr<ID3D11Device> dev;ctx->GetDevice(&dev);
            shader.Attach(shaderSwapCompilePs(ctx,kPlanetCoverageHlsl,sizeof(kPlanetCoverageHlsl)-1,"main","planet coverage",nullptr,"planet motion"));
            D3D11_DEPTH_STENCIL_DESC d{};d.DepthEnable=TRUE;d.DepthFunc=D3D11_COMPARISON_EQUAL;
            D3D11_BLEND_DESC b{};b.RenderTarget[0].RenderTargetWriteMask=3;
            if(!shader || FAILED(dev->CreateDepthStencilState(&d,&equal)) || FAILED(dev->CreateBlendState(&b,&write))){shader.Reset();equal.Reset();write.Reset();return false;}
        }
        if(!motion.prepare(ctx,scene,draw,1,4,solar?1:0))return false;
        ctx->OMGetRenderTargets(8,targets,&savedDepth);ctx->OMGetDepthStencilState(&savedDs,&reference);
        ctx->OMGetBlendState(&savedBlend,factors,&sampleMask);classCount=256;
        ctx->PSGetShader(&savedPs,classes,&classCount);ctx->PSGetConstantBuffers(12,1,&savedInfo);
        auto* target=motion.target();vScreenSetRenderTargetsRaw(ctx,1,&target,savedDepth.Get());
        ctx->OMSetDepthStencilState(equal.Get(),0);ctx->OMSetBlendState(write.Get(),nullptr,sampleMask);
        auto* info=motion.info();ctx->PSSetConstantBuffers(12,1,&info);ctx->PSSetShader(shader.Get(),nullptr,0);
        active=true;return true;
    }
    void end(ID3D11DeviceContext* ctx) {
        if(!active)return;
        vScreenSetRenderTargetsRaw(ctx,8,targets,savedDepth.Get());ctx->OMSetDepthStencilState(savedDs.Get(),reference);
        ctx->OMSetBlendState(savedBlend.Get(),factors,sampleMask);ctx->PSSetShader(savedPs.Get(),classes,classCount);
        ctx->PSSetConstantBuffers(12,1,savedInfo.GetAddressOf());
        for(auto*& p:targets)if(p){p->Release();p=nullptr;}for(UINT i=0;i<classCount;++i){classes[i]->Release();classes[i]=nullptr;}
        savedDepth.Reset();savedDs.Reset();savedBlend.Reset();savedPs.Reset();savedInfo.Reset();active=false;
    }
};
} // namespace edvr
