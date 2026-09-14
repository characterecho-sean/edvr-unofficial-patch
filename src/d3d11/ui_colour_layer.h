#pragma once
// Same-format world colour and a separate scalar UI influence. The original
// pixel shader is fanned out to three outputs; one raster/depth/stencil test
// serves all three targets. No draw replay or CPU readback is required here.
#include <d3d11_1.h>
#include <wrl/client.h>
#include <array>
#include <vector>
#include "vscreen.h"

namespace edvr {
class UiColourLayer {
    template<class T> using Ptr=Microsoft::WRL::ComPtr<T>;
    struct Blend {
        Ptr<ID3D11BlendState> original, world, ui;
    };
    std::vector<Blend> blends;
    Ptr<ID3D11Texture2D> source, clean, influence;
    Ptr<ID3D11RenderTargetView> cleanRtv, influenceRtv;
    Ptr<ID3D11ShaderResourceView> cleanSrv, influenceSrv;
    Ptr<ID3D11PixelShader> savedPs;
    Ptr<ID3D11BlendState> savedBlend;
    Ptr<ID3D11DepthStencilView> savedDepth;
    std::array<Ptr<ID3D11RenderTargetView>,8> savedTargets;
    ID3D11ClassInstance* classes[256]{};
    UINT classCount=0,sampleMask=0;
    float factors[4]{};
    bool seeded=false,bound=false;
    static D3D11_BLEND_DESC defaults() {
        D3D11_BLEND_DESC d{};
        for(auto& r:d.RenderTarget) {
            r.SrcBlend=r.SrcBlendAlpha=D3D11_BLEND_ONE;
            r.DestBlend=r.DestBlendAlpha=D3D11_BLEND_ZERO;
            r.BlendOp=r.BlendOpAlpha=D3D11_BLEND_OP_ADD;
            r.RenderTargetWriteMask=15;
        }
        return d;
    }
    ID3D11BlendState* blend(ID3D11Device* dev,ID3D11BlendState* original,bool ui) {
        for(auto& b:blends)if(b.original.Get()==original)return ui?b.ui.Get():b.world.Get();
        // This first implementation accepts the measured RGB source-over,
        // additive and opaque states. Colour-dependent destination factors
        // require more than one scalar influence and must decline.
        D3D11_BLEND_DESC d=defaults();if(original)original->GetDesc(&d);
        if(original) {
            Ptr<ID3D11BlendState1> extended;
            if(SUCCEEDED(original->QueryInterface(IID_PPV_ARGS(&extended)))) {
                D3D11_BLEND_DESC1 d1{};extended->GetDesc1(&d1);
                if(d1.RenderTarget[0].LogicOpEnable)return nullptr;
            }
        }
        const auto r=d.RenderTarget[0];
        const bool independentSource=r.SrcBlend==D3D11_BLEND_ZERO || r.SrcBlend==D3D11_BLEND_ONE ||
            r.SrcBlend==D3D11_BLEND_SRC_COLOR || r.SrcBlend==D3D11_BLEND_INV_SRC_COLOR ||
            r.SrcBlend==D3D11_BLEND_SRC_ALPHA || r.SrcBlend==D3D11_BLEND_INV_SRC_ALPHA ||
            r.SrcBlend==D3D11_BLEND_BLEND_FACTOR || r.SrcBlend==D3D11_BLEND_INV_BLEND_FACTOR;
        if(d.AlphaToCoverageEnable || (r.RenderTargetWriteMask&7)!=7 ||
           (r.BlendEnable && (!independentSource || r.BlendOp!=D3D11_BLEND_OP_ADD ||
             (r.DestBlend!=D3D11_BLEND_ZERO && r.DestBlend!=D3D11_BLEND_ONE && r.DestBlend!=D3D11_BLEND_INV_SRC_ALPHA))))return nullptr;
        // Opaque UI needs a constant-one influence export, not the shader's
        // alpha. The target sprite is measured source-over; reject other UI.
        if(blends.size()>=128)return nullptr;
        Blend b;b.original=original;
        for(unsigned kind=0;kind<2;++kind) {
            const bool remove=kind==1;
            if(remove && (!r.BlendEnable || r.DestBlend==D3D11_BLEND_ZERO))continue;
            auto desc=d;desc.IndependentBlendEnable=TRUE;
            desc.RenderTarget[1]=r;
            if(remove)desc.RenderTarget[1].RenderTargetWriteMask=0;
            auto& a=desc.RenderTarget[2];a=defaults().RenderTarget[0];
            a.BlendEnable=TRUE;a.RenderTargetWriteMask=D3D11_COLOR_WRITE_ENABLE_RED;
            a.SrcBlend=remove && r.DestBlend==D3D11_BLEND_INV_SRC_ALPHA?D3D11_BLEND_ONE:D3D11_BLEND_ZERO;
            a.DestBlend=r.BlendEnable?r.DestBlend:D3D11_BLEND_ZERO;
            // Fanout exports source alpha to both x and w of target 2. R32
            // stores x; blending still receives the original source alpha.
            Ptr<ID3D11BlendState>& result=remove?b.ui:b.world;
            if(FAILED(dev->CreateBlendState(&desc,&result)))return nullptr;
        }
        blends.push_back(std::move(b));return ui?blends.back().ui.Get():blends.back().world.Get();
    }
public:
    bool ready()const{return seeded;}
    ID3D11Texture2D* original()const{return source.Get();}
    ID3D11Texture2D* colour()const{return seeded?clean.Get():nullptr;}
    ID3D11ShaderResourceView* colourView()const{return seeded?cleanSrv.Get():nullptr;}
    ID3D11ShaderResourceView* influenceView()const{return seeded?influenceSrv.Get():nullptr;}
    void frameBoundary(){seeded=false;}
    void invalidate(){seeded=false;}
    bool seed(ID3D11DeviceContext* ctx,ID3D11RenderTargetView* target) {
        if(bound || !ctx || !target)return false;
        seeded=false;
        Ptr<ID3D11Resource> resource;target->GetResource(&resource);
        Ptr<ID3D11Texture2D> texture;if(FAILED(resource.As(&texture)))return false;
        D3D11_TEXTURE2D_DESC td{};texture->GetDesc(&td);
        D3D11_RENDER_TARGET_VIEW_DESC vd{};target->GetDesc(&vd);
        if(td.Format!=DXGI_FORMAT_R11G11B10_FLOAT || td.SampleDesc.Count!=1 || td.ArraySize!=1 || td.MipLevels!=1 ||
           td.Usage!=D3D11_USAGE_DEFAULT || vd.ViewDimension!=D3D11_RTV_DIMENSION_TEXTURE2D || vd.Texture2D.MipSlice || vd.Format!=td.Format)return false;
        if(source.Get()!=texture.Get() || !clean || !influence) {
            source=texture;clean.Reset();influence.Reset();cleanRtv.Reset();influenceRtv.Reset();cleanSrv.Reset();influenceSrv.Reset();
            Ptr<ID3D11Device> dev;ctx->GetDevice(&dev);
            td.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;td.CPUAccessFlags=td.MiscFlags=0;
            if(FAILED(dev->CreateTexture2D(&td,nullptr,&clean)) || FAILED(dev->CreateRenderTargetView(clean.Get(),nullptr,&cleanRtv)) ||
               FAILED(dev->CreateShaderResourceView(clean.Get(),nullptr,&cleanSrv)))return false;
            td.Format=DXGI_FORMAT_R32_FLOAT;
            if(FAILED(dev->CreateTexture2D(&td,nullptr,&influence)) || FAILED(dev->CreateRenderTargetView(influence.Get(),nullptr,&influenceRtv)) ||
               FAILED(dev->CreateShaderResourceView(influence.Get(),nullptr,&influenceSrv)))return false;
        }
        ctx->CopyResource(clean.Get(),source.Get());const float zero[4]{};ctx->ClearRenderTargetView(influenceRtv.Get(),zero);
        seeded=true;return true;
    }
    bool begin(ID3D11DeviceContext* ctx,ID3D11PixelShader* fanout,bool removeUi) {
        if(!seeded || bound || !fanout)return false;
        ID3D11RenderTargetView* targets[8]{};ctx->OMGetRenderTargets(8,targets,&savedDepth);
        bool valid=targets[0]!=nullptr;
        for(unsigned i=0;i<8;++i){savedTargets[i].Attach(targets[i]);if(i&&targets[i])valid=false;}
        Ptr<ID3D11Resource> current;if(targets[0])targets[0]->GetResource(&current);
        valid=valid && current.Get()==source.Get();
        ID3D11UnorderedAccessView* uavs[8]{};ctx->OMGetRenderTargetsAndUnorderedAccessViews(0,nullptr,nullptr,0,8,uavs);
        for(auto* u:uavs)if(u){u->Release();valid=false;}
        if(!valid){for(auto& t:savedTargets)t.Reset();savedDepth.Reset();return false;}
        ctx->OMGetBlendState(&savedBlend,factors,&sampleMask);
        Ptr<ID3D11Device> dev;ctx->GetDevice(&dev);auto* next=blend(dev.Get(),savedBlend.Get(),removeUi);
        if(!next){for(auto& t:savedTargets)t.Reset();savedDepth.Reset();savedBlend.Reset();return false;}
        classCount=256;ctx->PSGetShader(&savedPs,classes,&classCount);
        if(classCount){for(UINT i=0;i<classCount;++i){classes[i]->Release();classes[i]=nullptr;}classCount=0;
            savedPs.Reset();savedBlend.Reset();savedDepth.Reset();for(auto& t:savedTargets)t.Reset();return false;}
        ID3D11RenderTargetView* mrt[3]={targets[0],cleanRtv.Get(),influenceRtv.Get()};
        vScreenSetRenderTargetsRaw(ctx,3,mrt,savedDepth.Get());ctx->OMSetBlendState(next,factors,sampleMask);ctx->PSSetShader(fanout,nullptr,0);
        bound=true;return true;
    }
    void end(ID3D11DeviceContext* ctx) {
        if(!bound)return;
        ID3D11RenderTargetView* targets[8]{};for(unsigned i=0;i<8;++i)targets[i]=savedTargets[i].Get();
        vScreenSetRenderTargetsRaw(ctx,8,targets,savedDepth.Get());ctx->OMSetBlendState(savedBlend.Get(),factors,sampleMask);
        ctx->PSSetShader(savedPs.Get(),nullptr,0);
        savedPs.Reset();savedBlend.Reset();savedDepth.Reset();for(auto& t:savedTargets)t.Reset();bound=false;
    }
};
}
