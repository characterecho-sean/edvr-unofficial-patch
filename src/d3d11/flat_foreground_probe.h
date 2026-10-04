#pragma once

// Bounded, passive evidence for the early first-person pool pass. The private
// MRT records passing fragments; none of these observations admits a temporal
// frame or changes the selector's mixed-camera refusal.
#include "flat_overlay_layer.h"
#include "flat_foreground_ownership.h"
#include "flat_camera_phase.h"
#include "flat_context_state.h"
#include "flat_context_isolation.h"
#include "temporal_shader_bytecode.h"
#include "../common/log.h"
#include <wrl/client.h>
#include <cstdint>
#include <cstring>
#include <string>

namespace edvr {
class FlatForegroundProbe {
    template<class T> using Ptr=Microsoft::WRL::ComPtr<T>;
public:
    struct Draw {
        uint64_t vs=0,ps=0;
        char kind='?';
        uint32_t count=0,start=0,instances=0,startInstance=0;
        int32_t base=0;
        float camera[6][4]{};
        float phaseX=0,phaseY=0;
    };
private:
    static constexpr uint32_t kLimit=2,kSeparation=60,kPollLimit=120;
    static constexpr uint64_t kMemoryLimit=128ull*1024*1024;
    FlatOverlayLayer layer_;
    Ptr<ID3D11Texture2D> color_,depth_,cohortDepth_;
    Ptr<ID3D11DepthStencilView> dsv_;
    Ptr<ID3D11ShaderResourceView> cohortDepthView_;
    Ptr<ID3D11ComputeShader> shader_;
    Ptr<ID3D11Buffer> constants_,counters_,staging_;
    Ptr<ID3D11UnorderedAccessView> counterUav_;
    uint64_t frame_=0,firstFrame_=0;
    uint32_t reported_=0,firstSeq_=0,lastSeq_=0,worldSeq_=0,consumerSeq_=0;
    uint32_t width_=0,height_=0,planned_=0,draws_=0,polls_=0;
    uint32_t depthDrawsBeforeWorld_=0,stencilDrawsBeforeWorld_=0;
    uint32_t depthDrawsAfterWorld_=0,stencilDrawsAfterWorld_=0;
    uint32_t depthMutations_=0,stencilClears_=0;
    bool active_=false,drawOpen_=false,worldSeen_=false,consumerSeen_=false,pending_=false;
    bool depthWriteAll_=true,stencilReplace16_=true,fullViewport_=true;
    bool phaseMatch_=false,foreign_=false;
    Draw firstDraw_{};
    std::string failure_;

    void fail(const char* reason) { if(failure_.empty())failure_=reason?reason:"unknown"; }
    void releaseGpu() {
        layer_.reset();color_.Reset();depth_.Reset();dsv_.Reset();cohortDepth_.Reset();cohortDepthView_.Reset();
        shader_.Reset();constants_.Reset();counters_.Reset();staging_.Reset();counterUav_.Reset();
    }
    void report(const char* status,const FlatForegroundOwnershipCounts* counts=nullptr) {
        // The status is written before consuming the sample budget. A zero
        // coverage count and an unavailable shader have distinct signatures.
        const FlatForegroundOwnershipCounts zero{};const auto& c=counts?*counts:zero;
        Log::get().note("flat foreground ownership: frame=%llu status=%s reason=%s first=%u last=%u world=%u consumer=%u draws=%u planned=%u VS=%016llX PS=%016llX args=%c/%u/%u/%d/%u/%u depth-write-all=%u stencil-replace16=%u full-viewport=%u same-phase=%u depth-draws-gap=%u stencil-draws-gap=%u depth-draws-after-world=%u stencil-draws-after-world=%u explicit-depth-mutations=%u stencil-clears=%u foreign=%u total=%u covered=%u surviving-exact-depth=%u surviving-marked16=%u surviving-unmarked=%u overwritten=%u mark-without-surviving-coverage=%u total-stencil16=%u; exact-depth equality is evidence, not exclusive ownership",
            (unsigned long long)frame_,status,failure_.empty()?"none":failure_.c_str(),
            firstSeq_,lastSeq_,worldSeq_,consumerSeq_,draws_,planned_,
            (unsigned long long)firstDraw_.vs,(unsigned long long)firstDraw_.ps,
            firstDraw_.kind,firstDraw_.count,firstDraw_.start,firstDraw_.base,firstDraw_.instances,firstDraw_.startInstance,
            depthWriteAll_?1u:0u,stencilReplace16_?1u:0u,fullViewport_?1u:0u,phaseMatch_?1u:0u,
            depthDrawsBeforeWorld_,stencilDrawsBeforeWorld_,depthDrawsAfterWorld_,stencilDrawsAfterWorld_,
            depthMutations_,stencilClears_,foreign_?1u:0u,c.total,c.covered,c.survivingExactDepth,
            c.survivingMarked16,c.survivingUnmarked,c.overwritten,c.markedWithoutSurvivingCoverage,c.totalStencil16);
        if(!reported_)firstFrame_=frame_;
        ++reported_;
        active_=drawOpen_=worldSeen_=consumerSeen_=pending_=false;
        if(reported_==kLimit)releaseGpu();
    }
    static bool depthState(ID3D11DeviceContext* ctx,bool* writesDepth,bool* writesStencil16) {
        Ptr<ID3D11DepthStencilState> state;UINT ref=0;ctx->OMGetDepthStencilState(&state,&ref);
        D3D11_DEPTH_STENCIL_DESC d{};if(state)state->GetDesc(&d);
        *writesDepth=d.DepthEnable && d.DepthWriteMask==D3D11_DEPTH_WRITE_MASK_ALL;
        *writesStencil16=d.StencilEnable && (d.StencilWriteMask&16)!=0;
        return state && *writesDepth && *writesStencil16 && (ref&16)!=0 &&
            d.FrontFace.StencilPassOp==D3D11_STENCIL_OP_REPLACE &&
            d.BackFace.StencilPassOp==D3D11_STENCIL_OP_REPLACE;
    }
    bool ensureDepthClone(ID3D11DeviceContext* ctx) {
        if(cohortDepth_)return true;
        D3D11_TEXTURE2D_DESC d{};depth_->GetDesc(&d);
        if(d.Format!=DXGI_FORMAT_R32G8X24_TYPELESS || d.ArraySize!=1 || d.MipLevels!=1 ||
           d.SampleDesc.Count!=1 || d.Width!=width_ || d.Height!=height_ ||
           uint64_t(width_)*height_*13u>kMemoryLimit) {fail("depth-format-shape-or-budget");return false;}
        Ptr<ID3D11Device> dev;ctx->GetDevice(&dev);if(!dev){fail("device-unavailable");return false;}
        D3D11_TEXTURE2D_DESC mirror=d;mirror.BindFlags=D3D11_BIND_SHADER_RESOURCE;
        mirror.CPUAccessFlags=mirror.MiscFlags=0;mirror.Usage=D3D11_USAGE_DEFAULT;
        if(FAILED(dev->CreateTexture2D(&mirror,nullptr,&cohortDepth_))){fail("depth-clone-create");return false;}
        D3D11_SHADER_RESOURCE_VIEW_DESC v{};v.Format=DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
        v.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2D;v.Texture2D.MipLevels=1;
        if(FAILED(dev->CreateShaderResourceView(cohortDepth_.Get(),&v,&cohortDepthView_))){fail("depth-clone-view");return false;}
        return true;
    }
    bool ensureCompute(ID3D11Device* dev) {
        if(shader_ && constants_ && counters_ && counterUav_ && staging_)return true;
        if(FAILED(dev->CreateComputeShader(kFlatForegroundOwnershipBytecode,sizeof(kFlatForegroundOwnershipBytecode),nullptr,&shader_)))
            {fail("ownership-CS-create");return false;}
        D3D11_BUFFER_DESC b{};b.ByteWidth=16;b.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
        if(FAILED(dev->CreateBuffer(&b,nullptr,&constants_))){fail("ownership-CB-create");return false;}
        b={};b.ByteWidth=sizeof(FlatForegroundOwnershipCounts);b.BindFlags=D3D11_BIND_UNORDERED_ACCESS;
        b.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;b.StructureByteStride=sizeof(uint32_t);
        if(FAILED(dev->CreateBuffer(&b,nullptr,&counters_))){fail("ownership-counter-create");return false;}
        D3D11_UNORDERED_ACCESS_VIEW_DESC u{};u.Format=DXGI_FORMAT_UNKNOWN;
        u.ViewDimension=D3D11_UAV_DIMENSION_BUFFER;u.Buffer.NumElements=kFlatForegroundOwnershipCounterCount;
        if(FAILED(dev->CreateUnorderedAccessView(counters_.Get(),&u,&counterUav_))){fail("ownership-counter-UAV");return false;}
        b={};b.ByteWidth=sizeof(FlatForegroundOwnershipCounts);b.Usage=D3D11_USAGE_STAGING;b.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        if(FAILED(dev->CreateBuffer(&b,nullptr,&staging_))){fail("ownership-staging-create");return false;}
        return true;
    }
public:
    bool active() const {return active_;}
    uint32_t reported() const {return reported_;}
    bool plan(uint64_t frame,uint32_t seq,ID3D11Texture2D* color,ID3D11Texture2D* depth,
              ID3D11DepthStencilView* dsv,const Draw& draw,bool worldAlreadyNamed=false) {
        if(!frame || !color || !depth || !dsv || pending_ || reported_>=kLimit ||
           (reported_ && frame<firstFrame_+kSeparation))return false;
        if(!active_) {
            firstSeq_=lastSeq_=worldSeq_=consumerSeq_=0;
            width_=height_=planned_=draws_=polls_=0;
            depthDrawsBeforeWorld_=stencilDrawsBeforeWorld_=0;
            depthDrawsAfterWorld_=stencilDrawsAfterWorld_=depthMutations_=stencilClears_=0;
            drawOpen_=worldSeen_=consumerSeen_=pending_=foreign_=false;
            depthWriteAll_=stencilReplace16_=fullViewport_=true;phaseMatch_=false;failure_.clear();
            color_.Reset();depth_.Reset();dsv_.Reset();cohortDepth_.Reset();cohortDepthView_.Reset();
            active_=true;frame_=frame;firstSeq_=seq;firstDraw_=draw;
            color_=color;depth_=depth;dsv_=dsv;
            D3D11_TEXTURE2D_DESC d{};color->GetDesc(&d);width_=d.Width;height_=d.Height;
            if(uint64_t(width_)*height_*13u>kMemoryLimit)fail("probe-combined-memory-budget");
            if(worldAlreadyNamed)fail("foreground-after-world-source");
            layer_.beginFrame(frame);
            Log::get().note("flat foreground ownership: armed frame=%llu seq=%u limit=2 second-earliest=%llu; automatic first-person Pool cohort, no F10, selector remains strict",
                (unsigned long long)frame,seq,(unsigned long long)(firstFrame_?firstFrame_+kSeparation:0));
        }
        if(frame_!=frame || color_.Get()!=color || depth_.Get()!=depth || dsv_.Get()!=dsv) {
            fail("cohort-resource-changed");return false;
        }
        if(!planned_)firstSeq_=seq;
        lastSeq_=seq;++planned_;
        return failure_.empty();
    }
    bool beginDraw(ID3D11DeviceContext* ctx,uint64_t frame) {
        if(!active_ || frame_!=frame || drawOpen_ || !ctx)return false;
        bool writesDepth=false,writesStencil=false;
        const bool mark=depthState(ctx,&writesDepth,&writesStencil);
        depthWriteAll_ &= writesDepth;stencilReplace16_ &= mark;
        D3D11_VIEWPORT vp{};UINT n=1;ctx->RSGetViewports(&n,&vp);
        fullViewport_ &= n==1 && vp.TopLeftX==0 && vp.TopLeftY==0 &&
            vp.Width==width_ && vp.Height==height_ && vp.MinDepth==0 && vp.MaxDepth==1;
        const char* reason=nullptr;
        drawOpen_=layer_.beginDraw(ctx,frame,color_.Get(),dsv_.Get(),&reason,true);
        if(!drawOpen_)fail(reason?reason:"private-coverage-refused");
        return drawOpen_;
    }
    void endDraw(ID3D11DeviceContext* ctx) {
        if(!active_)return;
        if(drawOpen_) {layer_.endDraw(ctx);drawOpen_=false;++draws_;}
        else fail("draw-not-bracketed");
        if(const char* r=layer_.refusal())fail(r);
    }
    void noteSameDepthDraw(ID3D11DeviceContext* ctx,ID3D11Texture2D* depth,bool cohort) {
        if(!active_ || !ctx || depth!=depth_.Get() || cohort || consumerSeen_)return;
        bool dw=false,sw=false;depthState(ctx,&dw,&sw);
        if(worldSeen_) {depthDrawsAfterWorld_+=dw;stencilDrawsAfterWorld_+=sw;}
        else {depthDrawsBeforeWorld_+=dw;stencilDrawsBeforeWorld_+=sw;}
    }
    void noteDepthMutation(ID3D11Resource* resource,bool stencilClear=false) {
        if(!active_ || resource!=depth_.Get() || consumerSeen_)return;
        ++depthMutations_;if(stencilClear)++stencilClears_;
    }
    void noteStencilClear(ID3D11Resource* resource) {
        if(active_ && resource==depth_.Get() && !consumerSeen_)++stencilClears_;
    }
    void noteForeign() {if(active_)foreign_=true;}
    void noteScopeIncomplete() {if(active_)fail("draw-scope-incomplete");}
    void worldSource(ID3D11DeviceContext* ctx,uint64_t frame,uint32_t seq,ID3D11Texture2D* depth,
                     ID3D11DepthStencilView* dsv) {
        if(!active_ || frame_!=frame || worldSeen_)return;
        worldSeen_=true;worldSeq_=seq;
        if(!draws_ || depth!=depth_.Get() || dsv!=dsv_.Get()) {fail("first-world-source-or-cohort-missing");return;}
        Ptr<ID3D11Predicate> predicate;BOOL predicateValue=FALSE;
        ctx->GetPredication(&predicate,&predicateValue);
        if(predicate){fail("world-source-predicated-depth-clone");return;}
        FlatComputeInternalScope internal;
        if(!ensureDepthClone(ctx))return;
        ctx->CopyResource(cohortDepth_.Get(),depth_.Get());
    }
    void consumer(ID3D11DeviceContext* ctx,uint64_t frame,uint32_t seq,ID3D11Texture2D* depth,
                  ID3D11DepthStencilView* dsv,const unsigned char* worldCamera,bool foreign) {
        if(!active_ || frame_!=frame || consumerSeen_)return;
        consumerSeen_=true;consumerSeq_=seq;foreign_=foreign_||foreign;
        float world[6][4]{};if(worldCamera)std::memcpy(world,worldCamera,sizeof(world));
        phaseMatch_=worldCamera && flatCameraCenteredPairAtPhase(world,firstDraw_.camera,
            firstDraw_.phaseX,firstDraw_.phaseY,width_,height_);
        if(!worldSeen_ || !cohortDepthView_)fail("no-world-depth-clone");
        if(depth!=depth_.Get() || dsv!=dsv_.Get())fail("consumer-depth-identity");
        if(!layer_.ready(frame,color_.Get()))fail(layer_.refusal()?layer_.refusal():"coverage-not-ready");
        if(!failure_.empty()){report("partial");return;}
        Ptr<ID3D11Device> dev;ctx->GetDevice(&dev);if(!dev || !ensureCompute(dev.Get())){report("failed");return;}
        D3D11_SHADER_RESOURCE_VIEW_DESC vd{};vd.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2D;vd.Texture2D.MipLevels=1;
        Ptr<ID3D11ShaderResourceView> currentDepth,stencil;
        vd.Format=DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
        if(FAILED(dev->CreateShaderResourceView(depth,&vd,&currentDepth))){fail("consumer-depth-view");report("failed");return;}
        vd.Format=DXGI_FORMAT_X32_TYPELESS_G8X24_UINT;
        if(FAILED(dev->CreateShaderResourceView(depth,&vd,&stencil))){fail("consumer-stencil-view");report("failed");return;}
        Ptr<ID3D11DeviceContext1> c1;if(FAILED(ctx->QueryInterface(IID_PPV_ARGS(&c1))) || !c1){fail("context1-unavailable");report("failed");return;}
        ID3D11Buffer* so[D3D11_SO_BUFFER_SLOT_COUNT]{};
        ctx->SOGetTargets(D3D11_SO_BUFFER_SLOT_COUNT,so);
        bool streamOutputBound=false;
        for(auto* b:so)if(b){streamOutputBound=true;b->Release();}
        if(streamOutputBound){fail("consumer-stream-output-bound");report("partial");return;}
        FlatComputeInternalScope internal;
        const auto dxmt=flatDetectDxmt(dev.Get(),ctx);
        FlatContextState saved;saved.capture(c1.Get(),flatContextRanges(dev->GetFeatureLevel(),dxmt.dxmt()),false);
        ctx->ClearState();
        const UINT zeros[4]{};ctx->ClearUnorderedAccessViewUint(counterUav_.Get(),zeros);
        const UINT extent[4]={width_,height_,0,0};ctx->UpdateSubresource(constants_.Get(),0,nullptr,extent,0,0);
        ID3D11ShaderResourceView* views[4]={layer_.coverageView(),cohortDepthView_.Get(),currentDepth.Get(),stencil.Get()};
        ctx->CSSetShaderResources(0,4,views);ID3D11UnorderedAccessView* uav=counterUav_.Get();
        ctx->CSSetUnorderedAccessViews(0,1,&uav,nullptr);ID3D11Buffer* cb=constants_.Get();
        ctx->CSSetConstantBuffers(0,1,&cb);ctx->CSSetShader(shader_.Get(),nullptr,0);
        ctx->Dispatch((width_+7)/8,(height_+7)/8,1);
        ctx->CopyResource(staging_.Get(),counters_.Get());
        ctx->ClearState();saved.restore(c1.Get(),false);
        pending_=true;polls_=0;
        Log::get().note("flat foreground ownership: queued frame=%llu consumer=%u readback=pending; 8 GPU counters, no CPU/GPU wait",
            (unsigned long long)frame_,consumerSeq_);
    }
    void present(ID3D11DeviceContext* ctx,uint64_t completedFrame) {
        if(!active_)return;
        if(!consumerSeen_ && completedFrame>=frame_) {fail("no-HDR-consumer");report("partial");return;}
        if(!pending_ || !ctx)return;
        D3D11_MAPPED_SUBRESOURCE mapped{};
        FlatComputeInternalScope internal;
        const HRESULT hr=ctx->Map(staging_.Get(),0,D3D11_MAP_READ,D3D11_MAP_FLAG_DO_NOT_WAIT,&mapped);
        if(SUCCEEDED(hr)) {
            FlatForegroundOwnershipCounts c{};std::memcpy(&c,mapped.pData,sizeof(c));ctx->Unmap(staging_.Get(),0);
            report("complete",&c);return;
        }
        if(hr==DXGI_ERROR_WAS_STILL_DRAWING && ++polls_<kPollLimit)return;
        fail(hr==DXGI_ERROR_WAS_STILL_DRAWING?"readback-timeout":"readback-map-failed");report("failed");
    }
    void logStatus() const {
        Log::get().note("flat foreground ownership 5s: enabled=1 limit=2 reported=%u active=%u frame=%llu marked=%u world=%u consumer=%u pending=%u polls=%u reason=%s; no F10, zero covered is measured only in a complete report",
            reported_,active_?1u:0u,(unsigned long long)frame_,layer_.markedDraws(),worldSeen_?1u:0u,
            consumerSeen_?1u:0u,pending_?1u:0u,polls_,failure_.empty()?"none":failure_.c_str());
    }
    void reset() {releaseGpu();active_=drawOpen_=worldSeen_=consumerSeen_=pending_=false;}
};
} // namespace edvr
