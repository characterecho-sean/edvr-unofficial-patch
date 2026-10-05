#pragma once
#include "animated_vertex_history.h"
#include "flat_animated_identity_ledger.h"
#include <array>
#include <cmath>
#include <cstring>

namespace edvr {
// Flat ownership and camera adapter for the same bounded original-VS capture
// used by VR. No readback, diagnostic timer, or per-weapon selector is here.
class FlatForegroundMotion {
    template<class T> using Ptr=Microsoft::WRL::ComPtr<T>;
public:
    struct ResourceEpoch {Ptr<ID3D11Resource> resource;uint64_t epoch=0;};
    struct Certificate {
        bool complete=false;
        std::vector<unsigned char> constants;
        std::array<ResourceEpoch,128> resources{};
        unsigned resourceCount=0;
    };
    struct Inputs {
        float camera[6][4]{};float phaseX=0,phaseY=0;
        FlatAnimatedIdentityLedger::Identity identity;
        Certificate certificate;
    };
    struct Output {
        Ptr<ID3D11ShaderResourceView> motion;
        bool qualified=false,resetRequired=false;
        unsigned frame=0;float depthNear=0;const char* refusal=nullptr;
    };
    void reset(){*this=FlatForegroundMotion{};}
    void beginFrame(unsigned frame) {
        if(frame==frame_)return;
        if(frame==frame_+1)previous_=std::move(current_);else previous_.clear();
        current_.clear();frame_=frame;refusal_=nullptr;history_.advance(frame);
    }
    void fail(const char* reason){if(!refusal_)refusal_=reason?reason:"foreground-contract";}
    const char* refusal() const{return refusal_;}
    void resourceWritten(ID3D11Resource* resource) {
        const unsigned kind=history_.resourceWritten(resource);
        if(!kind)return;
        auto erase=[&](std::vector<Draw>& draws){draws.erase(std::remove_if(draws.begin(),draws.end(),[&](const Draw& d){
            return !resource || d.capture.geometry.vertices.Get()==resource || d.capture.geometry.indices.Get()==resource;}),draws.end());};
        erase(previous_);const size_t before=current_.size();erase(current_);
        if(kind&1)fail("foreground-unknown-resource-write");
        else if(before!=current_.size())fail("foreground-captured-geometry-written");
    }
    bool capture(ID3D11DeviceContext* ctx,PanelCurveDrawFn draw,unsigned count,unsigned instances,
                 unsigned start,int base,unsigned startInstance,unsigned frame,const Inputs& inputs) {
        beginFrame(frame);Draw d;d.inputs=inputs;
        if(!ctx){fail("foreground-missing-context");return false;}
        Ptr<ID3D11VertexShader> activeVs;UINT classCount=0;ctx->VSGetShader(&activeVs,nullptr,&classCount);
        if(classCount){fail("foreground-dynamic-VS-linkage");return false;}
        if(d.inputs.certificate.constants.size()>65536 || d.inputs.certificate.resourceCount>128){
            d.inputs.certificate=Certificate{};fail("foreground-certificate-bound");}
        if(current_.size()>=AnimatedVertexHistory::maxRecords){fail("foreground-draw-bound");return false;}
        if(!history_.capture(ctx,draw,count,instances,start,base,startInstance,frame,d.capture,true)){
            fail(d.capture.refusal);return false;}
        ctx->RSGetState(&d.raster);UINT n=1;ctx->RSGetViewports(&n,&d.viewport);
        d.scissorCount=16;ctx->RSGetScissorRects(&d.scissorCount,d.scissors.data());
        if(!inputs.identity.refusal && inputs.identity.instanceEpoch && inputs.identity.poolEpoch &&
           inputs.identity.slot<=0x7ffffeu && std::isfinite(inputs.camera[3][2]) && inputs.camera[3][2]>0 &&
           std::isfinite(inputs.phaseX) && std::isfinite(inputs.phaseY))d.identityKnown=true;
        else fail(inputs.identity.refusal?inputs.identity.refusal:"foreground-identity-or-camera");
        for(const auto& old:previous_) {
            if(!d.identityKnown || !old.identityKnown || !sameIdentity(d.inputs.identity,old.inputs.identity) ||
               !samePool(d.capture,old.capture) || d.inputs.camera[3][2]!=old.inputs.camera[3][2])continue;
            for(unsigned i=0;i<d.capture.candidateCount;++i)if(d.capture.previousPositions[i].Get()==old.capture.currentPositions.Get()) {
                if(!d.oldPositions){d.oldPositions=d.capture.previousPositions[i];d.oldIdentity=d.capture.previousIdentity[i];d.oldInputs=old.inputs;}
                else if(!equivalent(d.oldInputs,old.inputs)){d.ambiguous=true;fail("foreground-ambiguous-prior-inputs");}
            }
        }
        current_.push_back(std::move(d));return !refusal_;
    }
    bool prepareH(ID3D11DeviceContext* ctx,ID3D11ShaderResourceView* owners,ID3D11ShaderResourceView* rawDepth,
                  const float worldCamera[6][4],unsigned frame,unsigned width,unsigned height,Output& out) {
        out=Output{};out.frame=frame;
        auto refuse=[&](const char* reason){out.refusal=reason;return false;};
        if(frame!=frame_ || refusal_)return refuse(refusal_?refusal_:"foreground-frame");
        if(!ctx || !owners || !rawDepth || !width || !height || uint64_t(width)*height>16*1024*1024)
            return refuse("foreground-H-resources");
        if(!textureExtent(owners,width,height,true) || !textureExtent(rawDepth,width,height,false))
            return refuse("foreground-H-resource-shape");
        Ptr<ID3D11GeometryShader> geometry;Ptr<ID3D11HullShader> hull;Ptr<ID3D11DomainShader> domain;Ptr<ID3D11Predicate> predicate;
        ctx->GSGetShader(&geometry,nullptr,nullptr);ctx->HSGetShader(&hull,nullptr,nullptr);ctx->DSGetShader(&domain,nullptr,nullptr);
        BOOL predicateValue=FALSE;ctx->GetPredication(&predicate,&predicateValue);
        Ptr<ID3D11VertexShader> inspectedVs;Ptr<ID3D11PixelShader> inspectedPs;
        UINT vsClasses=0,psClasses=0;ctx->VSGetShader(&inspectedVs,nullptr,&vsClasses);ctx->PSGetShader(&inspectedPs,nullptr,&psClasses);
        bool unsupported=geometry || hull || domain || predicate || vsClasses || psClasses;
        ID3D11Buffer* so[4]{};ctx->SOGetTargets(4,so);for(auto* p:so)if(p){unsupported=true;p->Release();}
        ID3D11UnorderedAccessView* uavs[8]{};ctx->OMGetRenderTargetsAndUnorderedAccessViews(0,nullptr,nullptr,0,8,uavs);
        for(auto* p:uavs)if(p){unsupported=true;p->Release();}
        if(unsupported)return refuse("foreground-H-unsupported-pipeline");
        float commonNear=worldCamera[3][2];if(!std::isfinite(commonNear) || commonNear<=0)return refuse("foreground-world-commonNear");
        for(const auto& d:current_) {
            if(!d.identityKnown || d.ambiguous)return refuse("foreground-unqualified-draw");
            if(d.viewport.TopLeftX || d.viewport.TopLeftY || d.viewport.Width!=float(width) || d.viewport.Height!=float(height) ||
               d.viewport.MinDepth!=0 || d.viewport.MaxDepth!=1)return refuse("foreground-viewport");
            commonNear=(std::min)(commonNear,d.inputs.camera[3][2]);
            if(!d.oldPositions)out.resetRequired=true;
        }
        if(!initialize(ctx,width,height))return refuse("foreground-map-create");
        if(previousNear_ && previousNear_!=commonNear)out.resetRequired=true;
        previousNear_=commonNear;
        // H is bracketed independently of original geometry capture. Restore
        // every binding touched here, including all eight OM render targets.
        Ptr<ID3D11VertexShader> oldVs;Ptr<ID3D11PixelShader> oldPs;Ptr<ID3D11InputLayout> oldLayout;
        Ptr<ID3D11RasterizerState> oldRaster;Ptr<ID3D11BlendState> oldBlend;Ptr<ID3D11DepthStencilState> oldDepth;
        Ptr<ID3D11Buffer> oldVsCb,oldPsCb;UINT oldRef=0,oldMask=0;float oldFactors[4]{};
        ID3D11RenderTargetView* oldTargets[8]{};ID3D11DepthStencilView* oldDsv=nullptr;
        ID3D11ShaderResourceView* oldVsViews[5]{},*oldPsViews[2]{};D3D11_PRIMITIVE_TOPOLOGY oldTopology;
        D3D11_VIEWPORT oldViewport[16]{};UINT oldViewportCount=16;D3D11_RECT oldScissors[16]{};UINT oldScissorCount=16;
        ctx->VSGetShader(&oldVs,nullptr,nullptr);ctx->PSGetShader(&oldPs,nullptr,nullptr);ctx->IAGetInputLayout(&oldLayout);
        ctx->RSGetState(&oldRaster);ctx->OMGetBlendState(&oldBlend,oldFactors,&oldMask);ctx->OMGetDepthStencilState(&oldDepth,&oldRef);
        ctx->VSGetConstantBuffers(0,1,&oldVsCb);ctx->PSGetConstantBuffers(0,1,&oldPsCb);
        ctx->VSGetShaderResources(0,5,oldVsViews);ctx->PSGetShaderResources(0,2,oldPsViews);
        ctx->OMGetRenderTargets(8,oldTargets,&oldDsv);ctx->IAGetPrimitiveTopology(&oldTopology);
        ctx->RSGetViewports(&oldViewportCount,oldViewport);ctx->RSGetScissorRects(&oldScissorCount,oldScissors);
        ctx->OMSetRenderTargets(1,target_.GetAddressOf(),nullptr);ctx->OMSetBlendState(blend_.Get(),nullptr,~0u);ctx->OMSetDepthStencilState(depth_.Get(),0);
        float zero[4]{};ctx->ClearRenderTargetView(target_.Get(),zero);
        ctx->VSSetShader(vs_.Get(),nullptr,0);ctx->PSSetShader(ps_.Get(),nullptr,0);ctx->IASetInputLayout(nullptr);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->VSSetConstantBuffers(0,1,settings_.GetAddressOf());ctx->PSSetConstantBuffers(0,1,settings_.GetAddressOf());
        ID3D11ShaderResourceView* psViews[2]={owners,rawDepth};ctx->PSSetShaderResources(0,2,psViews);
        for(const auto& d:current_) {
            Settings c{};c.extentPhase[0]=float(width);c.extentPhase[1]=float(height);c.extentPhase[2]=d.inputs.phaseX;c.extentPhase[3]=d.inputs.phaseY;
            c.previousPhaseDepth[0]=d.oldInputs.phaseX;c.previousPhaseDepth[1]=d.oldInputs.phaseY;c.previousPhaseDepth[2]=commonNear;
            c.expected[0]=d.inputs.identity.slot;c.expected[1]=d.inputs.identity.skeleton;c.expected[2]=d.inputs.identity.allocation;c.expected[3]=d.oldPositions?1u:2u;
            ctx->UpdateSubresource(settings_.Get(),0,nullptr,&c,0,0);
            ID3D11ShaderResourceView* vsViews[5]={d.capture.currentPositions.Get(),d.oldPositions.Get(),d.capture.currentIdentity.Get(),d.oldIdentity.Get(),d.capture.instanceIndex.Get()};
            ctx->VSSetShaderResources(0,5,vsViews);ctx->RSSetState(d.raster.Get());ctx->RSSetViewports(1,&d.viewport);ctx->RSSetScissorRects(d.scissorCount,d.scissors.data());
            ctx->Draw(d.capture.geometry.count,0);
        }
        ID3D11ShaderResourceView* nullVs[5]{},*nullPs[2]{};ctx->VSSetShaderResources(0,5,nullVs);ctx->PSSetShaderResources(0,2,nullPs);
        ctx->OMSetRenderTargets(8,oldTargets,oldDsv);ctx->OMSetBlendState(oldBlend.Get(),oldFactors,oldMask);ctx->OMSetDepthStencilState(oldDepth.Get(),oldRef);
        ctx->VSSetShader(oldVs.Get(),nullptr,0);ctx->PSSetShader(oldPs.Get(),nullptr,0);ctx->IASetInputLayout(oldLayout.Get());ctx->IASetPrimitiveTopology(oldTopology);
        ctx->VSSetConstantBuffers(0,1,&oldVsCb);ctx->PSSetConstantBuffers(0,1,&oldPsCb);ctx->VSSetShaderResources(0,5,oldVsViews);ctx->PSSetShaderResources(0,2,oldPsViews);
        ctx->RSSetState(oldRaster.Get());ctx->RSSetViewports(oldViewportCount,oldViewport);ctx->RSSetScissorRects(oldScissorCount,oldScissors);
        for(auto* p:oldTargets)if(p)p->Release();if(oldDsv)oldDsv->Release();
        for(auto* p:oldVsViews)if(p)p->Release();for(auto* p:oldPsViews)if(p)p->Release();
        out.motion=view_;out.qualified=true;out.depthNear=commonNear;return true;
    }
private:
    struct Draw {
        AnimatedVertexHistory::Capture capture;Inputs inputs,oldInputs;
        Ptr<ID3D11ShaderResourceView> oldPositions,oldIdentity;Ptr<ID3D11RasterizerState> raster;
        D3D11_VIEWPORT viewport{};std::array<D3D11_RECT,16> scissors{};UINT scissorCount=0;
        bool identityKnown=false,ambiguous=false;
    };
    struct Settings {float extentPhase[4]{},previousPhaseDepth[4]{};uint32_t expected[4]{};};
    static bool sameIdentity(const FlatAnimatedIdentityLedger::Identity& a,const FlatAnimatedIdentityLedger::Identity& b) {
        return a.slot==b.slot && a.skeleton==b.skeleton && a.allocation==b.allocation;
    }
    static bool samePool(const AnimatedVertexHistory::Capture& a,const AnimatedVertexHistory::Capture& b) {
        Ptr<ID3D11Resource> x,y;a.pool->GetResource(&x);b.pool->GetResource(&y);return x==y;
    }
    static bool textureExtent(ID3D11ShaderResourceView* view,unsigned width,unsigned height,bool owner) {
        D3D11_SHADER_RESOURCE_VIEW_DESC v{};view->GetDesc(&v);
        if(v.ViewDimension!=D3D11_SRV_DIMENSION_TEXTURE2D || v.Texture2D.MostDetailedMip ||
           (owner?v.Format!=DXGI_FORMAT_R32G32_FLOAT:
             (v.Format!=DXGI_FORMAT_R32_FLOAT && v.Format!=DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS)))return false;
        Ptr<ID3D11Resource> resource;view->GetResource(&resource);Ptr<ID3D11Texture2D> texture;
        if(FAILED(resource.As(&texture)))return false;D3D11_TEXTURE2D_DESC d{};texture->GetDesc(&d);
        return d.Width==width && d.Height==height && d.ArraySize==1 && d.SampleDesc.Count==1;
    }
    static bool equivalent(const Inputs& a,const Inputs& b) {
        const auto& x=a.certificate;const auto& y=b.certificate;
        if(!x.complete || !y.complete || x.constants!=y.constants || x.resourceCount!=y.resourceCount ||
           std::memcmp(a.camera,b.camera,sizeof(a.camera)) || a.phaseX!=b.phaseX || a.phaseY!=b.phaseY)return false;
        for(unsigned i=0;i<x.resourceCount;++i)if(!x.resources[i].epoch || x.resources[i].epoch!=y.resources[i].epoch ||
            x.resources[i].resource!=y.resources[i].resource)return false;
        return true;
    }
    bool initialize(ID3D11DeviceContext* ctx,unsigned width,unsigned height) {
        Ptr<ID3D11Device> dev;ctx->GetDevice(&dev);
        if(!vs_)vs_.Attach(shaderSwapCreateVs(ctx,kFlatForegroundMotionVsBytecode,sizeof(kFlatForegroundMotionVsBytecode),"flat foreground motion","flat foreground motion"));
        if(!ps_)ps_.Attach(shaderSwapCreatePs(ctx,kFlatForegroundMotionPsBytecode,sizeof(kFlatForegroundMotionPsBytecode),"flat foreground motion","flat foreground motion"));
        if(!vs_ || !ps_)return false;
        if(!settings_){D3D11_BUFFER_DESC d{};d.ByteWidth=sizeof(Settings);d.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
            if(FAILED(dev->CreateBuffer(&d,nullptr,&settings_)))return false;
            D3D11_BLEND_DESC b{};b.RenderTarget[0].RenderTargetWriteMask=15;
            D3D11_DEPTH_STENCIL_DESC z{};if(FAILED(dev->CreateBlendState(&b,&blend_)) || FAILED(dev->CreateDepthStencilState(&z,&depth_)))return false;}
        if(width_!=width || height_!=height || !view_){target_.Reset();view_.Reset();texture_.Reset();
            D3D11_TEXTURE2D_DESC d{};d.Width=width;d.Height=height;d.MipLevels=d.ArraySize=d.SampleDesc.Count=1;
            d.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;d.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;
            if(FAILED(dev->CreateTexture2D(&d,nullptr,&texture_)) || FAILED(dev->CreateRenderTargetView(texture_.Get(),nullptr,&target_)) || FAILED(dev->CreateShaderResourceView(texture_.Get(),nullptr,&view_)))return false;
            width_=width;height_=height;}
        return true;
    }
    AnimatedVertexHistory history_;std::vector<Draw> current_,previous_;unsigned frame_=0,width_=0,height_=0;
    const char* refusal_=nullptr;float previousNear_=0;
    Ptr<ID3D11VertexShader> vs_;Ptr<ID3D11PixelShader> ps_;Ptr<ID3D11Buffer> settings_;
    Ptr<ID3D11BlendState> blend_;Ptr<ID3D11DepthStencilState> depth_;
    Ptr<ID3D11Texture2D> texture_;Ptr<ID3D11RenderTargetView> target_;Ptr<ID3D11ShaderResourceView> view_;
};
} // namespace edvr
