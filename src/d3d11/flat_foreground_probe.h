#pragma once

// Bounded, passive evidence for the early first-person pool pass. The private
// MRT records passing fragments; none of these observations admits a temporal
// frame or changes the selector's mixed-camera refusal.
#include "flat_overlay_layer.h"
#include "flat_foreground_probe_policy.h"
#include "flat_foreground_ownership.h"
#include "flat_camera_phase.h"
#include "flat_context_state.h"
#include "flat_isolation_mode.h"
#include "temporal_shader_bytecode.h"
#include "../common/log.h"
#include <wrl/client.h>
#include <cstdint>
#include <cstring>
#include <array>
#include <string>

namespace edvr {
class FlatForegroundProbe {
    template<class T> using Ptr=Microsoft::WRL::ComPtr<T>;
public:
    struct Draw {
        uint64_t vs=0,ps=0;
        char kind='?';
        uint32_t seq=0,targetKind=0,format=0;
        uint64_t cameraHash=0;
        bool supported=false,family=false,fullViewport=false;
        uint32_t count=0,start=0,instances=0,startInstance=0;
        int32_t base=0;
        float camera[6][4]{};
        float phaseX=0,phaseY=0;
    };
private:
    static constexpr uint32_t kLimit=2,kSeparation=60,kPollLimit=120;
    static constexpr uint32_t kChronologyLimit=64;
    static constexpr uint64_t kMemoryLimit=128ull*1024*1024;
    struct Event {
        Draw draw{};
        char role='?';
        bool stateBound=false,attempted=false,merged=false;
        UINT stencilRef=0,dsvFlags=0;
        D3D11_DEPTH_STENCIL_DESC state{};
    };
    FlatOverlayLayer layer_;
    Ptr<ID3D11Texture2D> color_,depth_,cohortDepth_,ownerDepth_,unionCoverage_;
    Ptr<ID3D11DepthStencilView> dsv_;
    Ptr<ID3D11ShaderResourceView> cohortDepthView_,ownerDepthView_,unionCoverageView_;
    Ptr<ID3D11UnorderedAccessView> ownerDepthUav_,unionCoverageUav_;
    Ptr<ID3D11ComputeShader> shader_,mergeShader_;
    Ptr<ID3D11Buffer> constants_,counters_,staging_;
    Ptr<ID3D11UnorderedAccessView> counterUav_;
    uint64_t frame_=0,firstFrame_=0;
    uint32_t reported_=0,firstSeq_=0,lastSeq_=0,worldSeq_=0,consumerSeq_=0;
    uint32_t width_=0,height_=0,planned_=0,draws_=0,merged_=0,polls_=0,lateSkipped_=0;
    uint32_t depthDrawsBeforeWorld_=0,stencilDrawsBeforeWorld_=0;
    uint32_t sameDepthDrawsAfterWorld_=0;
    uint32_t depthMutations_=0,stencilClears_=0;
    bool active_=false,drawOpen_=false,worldSeen_=false,consumerSeen_=false,pending_=false;
    bool depthWriteAll_=true,stencilReplace16_=true,fullViewport_=true;
    bool phaseMatch_=false,foreign_=false;
    std::array<Event,kChronologyLimit> events_{};
    uint32_t eventCount_=0,eventDropped_=0,currentEvent_=kChronologyLimit;
    Draw firstDraw_{};
    std::string failure_;

    void fail(const char* reason) { if(failure_.empty())failure_=reason?reason:"unknown"; }
    Event* appendEvent(char role,const Draw& draw,ID3D11DeviceContext* ctx=nullptr) {
        if(eventCount_==kChronologyLimit){++eventDropped_;return nullptr;}
        Event& e=events_[eventCount_++];e={};e.role=role;e.draw=draw;
        if(ctx)captureState(ctx,e);
        return &e;
    }
    static void captureState(ID3D11DeviceContext* ctx,Event& e) {
        Ptr<ID3D11DepthStencilState> state;
        ctx->OMGetDepthStencilState(&state,&e.stencilRef);
        Ptr<ID3D11DepthStencilView> dsv;ctx->OMGetRenderTargets(0,nullptr,&dsv);
        if(dsv){D3D11_DEPTH_STENCIL_VIEW_DESC view{};dsv->GetDesc(&view);e.dsvFlags=view.Flags;}
        e.stateBound=state!=nullptr;
        if(state)state->GetDesc(&e.state);
        else {
            e.state.DepthEnable=TRUE;
            e.state.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ALL;
            e.state.DepthFunc=D3D11_COMPARISON_LESS;
        }
    }
    void releaseGpu() {
        layer_.reset();color_.Reset();depth_.Reset();dsv_.Reset();cohortDepth_.Reset();cohortDepthView_.Reset();
        ownerDepth_.Reset();ownerDepthView_.Reset();ownerDepthUav_.Reset();
        unionCoverage_.Reset();unionCoverageView_.Reset();unionCoverageUav_.Reset();
        shader_.Reset();mergeShader_.Reset();constants_.Reset();counters_.Reset();staging_.Reset();counterUav_.Reset();
    }
    void report(const char* status,const FlatForegroundOwnershipCounts* counts=nullptr) {
        // The status is written before consuming the sample budget. A zero
        // coverage count and an unavailable shader have distinct signatures.
        const FlatForegroundOwnershipCounts zero{};const auto& c=counts?*counts:zero;
        Log::get().note("flat foreground ownership: frame=%llu status=%s counts=%s reason=%s first=%u last=%u world=%u consumer=%u draws=%u merged=%u planned=%u events=%u dropped=%u VS=%016llX PS=%016llX args=%c/%u/%u/%d/%u/%u depth-write-all=%u stencil-replace16=%u full-viewport=%u same-phase=%u depth-draws-gap=%u stencil-draws-gap=%u after-world-same-DS-draws=%u after-world-DS-state=unobserved explicit-depth-mutations=%u stencil-clears=%u foreign=%u total=%u covered=%u surviving-exact-depth=%u surviving-marked16=%u surviving-unmarked=%u overwritten=%u mark-without-surviving-coverage=%u total-stencil16=%u; exact-depth equality is evidence, not exclusive ownership",
            (unsigned long long)frame_,status,counts?"measured":"unavailable",failure_.empty()?"none":failure_.c_str(),
            firstSeq_,lastSeq_,worldSeq_,consumerSeq_,draws_,merged_,planned_,eventCount_,eventDropped_,
            (unsigned long long)firstDraw_.vs,(unsigned long long)firstDraw_.ps,
            firstDraw_.kind,firstDraw_.count,firstDraw_.start,firstDraw_.base,firstDraw_.instances,firstDraw_.startInstance,
            depthWriteAll_?1u:0u,stencilReplace16_?1u:0u,fullViewport_?1u:0u,phaseMatch_?1u:0u,
            depthDrawsBeforeWorld_,stencilDrawsBeforeWorld_,sameDepthDrawsAfterWorld_,
            depthMutations_,stencilClears_,foreign_?1u:0u,c.total,c.covered,c.survivingExactDepth,
            c.survivingMarked16,c.survivingUnmarked,c.overwritten,c.markedWithoutSurvivingCoverage,c.totalStencil16);
        for(uint32_t i=0;i<eventCount_;++i) {
            const Event& e=events_[i];const auto& d=e.draw;const auto& ds=e.state;
            Log::get().note("flat foreground chronology: frame=%llu index=%u role=%c seq=%u VS=%016llX PS=%016llX draw=%c/%u/%u/%d/%u/%u supported=%u family=%u target-kind=%u fmt=%u full-viewport=%u camera=%016llX near=%.8g phase=%.6g/%.6g attempted=%u merged=%u DS-bound=%u DSV-flags=%u depth-enable=%u depth-write=%u depth-func=%u stencil-enable=%u ref=%u read-mask=%u write-mask=%u front-func=%u front-fail=%u front-depth-fail=%u front-pass=%u back-func=%u back-fail=%u back-depth-fail=%u back-pass=%u; state-enabled writes are not measured pixel writes",
                (unsigned long long)frame_,i,e.role,d.seq,(unsigned long long)d.vs,(unsigned long long)d.ps,
                d.kind,d.count,d.start,d.base,d.instances,d.startInstance,d.supported?1u:0u,d.family?1u:0u,
                d.targetKind,d.format,d.fullViewport?1u:0u,(unsigned long long)d.cameraHash,d.camera[3][2],
                d.phaseX,d.phaseY,e.attempted?1u:0u,e.merged?1u:0u,e.stateBound?1u:0u,e.dsvFlags,
                ds.DepthEnable?1u:0u,ds.DepthWriteMask==D3D11_DEPTH_WRITE_MASK_ALL?1u:0u,
                unsigned(ds.DepthFunc),ds.StencilEnable?1u:0u,e.stencilRef,unsigned(ds.StencilReadMask),
                unsigned(ds.StencilWriteMask),unsigned(ds.FrontFace.StencilFunc),
                unsigned(ds.FrontFace.StencilFailOp),unsigned(ds.FrontFace.StencilDepthFailOp),
                unsigned(ds.FrontFace.StencilPassOp),unsigned(ds.BackFace.StencilFunc),
                unsigned(ds.BackFace.StencilFailOp),unsigned(ds.BackFace.StencilDepthFailOp),
                unsigned(ds.BackFace.StencilPassOp));
        }
        if(!reported_)firstFrame_=frame_;
        ++reported_;
        active_=drawOpen_=worldSeen_=consumerSeen_=pending_=false;
        if(reported_==kLimit)releaseGpu();
    }
    static bool depthState(ID3D11DeviceContext* ctx,bool* writesDepth,bool* writesStencil16) {
        Ptr<ID3D11DepthStencilState> state;UINT ref=0;ctx->OMGetDepthStencilState(&state,&ref);
        D3D11_DEPTH_STENCIL_DESC d{};if(state)state->GetDesc(&d);
        else {d.DepthEnable=TRUE;d.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ALL;d.DepthFunc=D3D11_COMPARISON_LESS;}
        *writesDepth=d.DepthEnable && d.DepthWriteMask==D3D11_DEPTH_WRITE_MASK_ALL;
        *writesStencil16=d.StencilEnable && (d.StencilWriteMask&16)!=0;
        return state && *writesDepth && *writesStencil16 && (ref&16)!=0 &&
            d.FrontFace.StencilPassOp==D3D11_STENCIL_OP_REPLACE &&
            d.BackFace.StencilPassOp==D3D11_STENCIL_OP_REPLACE;
    }
    bool ensureDepthClone(ID3D11DeviceContext* ctx) {
        if(cohortDepth_ && ownerDepth_ && ownerDepthView_ && ownerDepthUav_ &&
           unionCoverage_ && unionCoverageView_ && unionCoverageUav_)return true;
        D3D11_TEXTURE2D_DESC d{};depth_->GetDesc(&d);
        if(d.Format!=DXGI_FORMAT_R32G8X24_TYPELESS || d.ArraySize!=1 || d.MipLevels!=1 ||
           d.SampleDesc.Count!=1 || d.Width!=width_ || d.Height!=height_ ||
           uint64_t(width_)*height_*14u>kMemoryLimit) {fail("depth-format-shape-or-budget");return false;}
        Ptr<ID3D11Device> dev;ctx->GetDevice(&dev);if(!dev){fail("device-unavailable");return false;}
        D3D11_TEXTURE2D_DESC mirror=d;mirror.BindFlags=D3D11_BIND_SHADER_RESOURCE;
        mirror.CPUAccessFlags=mirror.MiscFlags=0;mirror.Usage=D3D11_USAGE_DEFAULT;
        if(FAILED(dev->CreateTexture2D(&mirror,nullptr,&cohortDepth_))){fail("depth-clone-create");return false;}
        D3D11_SHADER_RESOURCE_VIEW_DESC v{};v.Format=DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
        v.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2D;v.Texture2D.MipLevels=1;
        if(FAILED(dev->CreateShaderResourceView(cohortDepth_.Get(),&v,&cohortDepthView_))){fail("depth-clone-view");return false;}
        D3D11_TEXTURE2D_DESC owned=mirror;
        owned.Format=DXGI_FORMAT_R32_FLOAT;
        owned.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_UNORDERED_ACCESS;
        if(FAILED(dev->CreateTexture2D(&owned,nullptr,&ownerDepth_)) ||
           FAILED(dev->CreateShaderResourceView(ownerDepth_.Get(),nullptr,&ownerDepthView_)) ||
           FAILED(dev->CreateUnorderedAccessView(ownerDepth_.Get(),nullptr,&ownerDepthUav_)))
            {fail("owner-depth-create-or-view");return false;}
        owned.Format=DXGI_FORMAT_R8_UNORM;
        if(FAILED(dev->CreateTexture2D(&owned,nullptr,&unionCoverage_)) ||
           FAILED(dev->CreateShaderResourceView(unionCoverage_.Get(),nullptr,&unionCoverageView_)) ||
           FAILED(dev->CreateUnorderedAccessView(unionCoverage_.Get(),nullptr,&unionCoverageUav_)))
            {fail("union-coverage-create-or-view");return false;}
        const FLOAT zeros[4]{};
        FlatComputeInternalScope internal;
        ctx->ClearUnorderedAccessViewFloat(unionCoverageUav_.Get(),zeros);
        return true;
    }
    bool ensureCompute(ID3D11Device* dev) {
        if(shader_) {
            Ptr<ID3D11Device> existing;shader_->GetDevice(&existing);
            if(existing.Get()!=dev){fail("compute-device-changed");return false;}
        }
        if(shader_ && mergeShader_ && constants_ && counters_ && counterUav_ && staging_)return true;
        shader_.Reset();mergeShader_.Reset();constants_.Reset();counters_.Reset();counterUav_.Reset();staging_.Reset();
        if(FAILED(dev->CreateComputeShader(kFlatForegroundOwnershipBytecode,sizeof(kFlatForegroundOwnershipBytecode),nullptr,&shader_)))
            {fail("ownership-CS-create");return false;}
        if(FAILED(dev->CreateComputeShader(kFlatForegroundMergeBytecode,sizeof(kFlatForegroundMergeBytecode),nullptr,&mergeShader_)))
            {fail("foreground-merge-CS-create");return false;}
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
    bool mergeCurrentDraw(ID3D11DeviceContext* ctx) {
        if(!ctx || !layer_.coverageReady(frame_,color_.Get())){fail("draw-coverage-unavailable");return false;}
        Ptr<ID3D11Predicate> predicate;BOOL predicateValue=FALSE;
        ctx->GetPredication(&predicate,&predicateValue);
        if(predicate){fail("merge-predication-bound");return false;}
        ID3D11Buffer* so[D3D11_SO_BUFFER_SLOT_COUNT]{};
        ctx->SOGetTargets(D3D11_SO_BUFFER_SLOT_COUNT,so);
        bool streamOutputBound=false;
        for(auto* b:so)if(b){streamOutputBound=true;b->Release();}
        if(streamOutputBound){fail("merge-stream-output-bound");return false;}
        if(!ensureDepthClone(ctx))return false;
        Ptr<ID3D11Device> dev;ctx->GetDevice(&dev);
        if(!dev || !ensureCompute(dev.Get()))return false;
        Ptr<ID3D11DeviceContext1> c1;
        if(FAILED(ctx->QueryInterface(IID_PPV_ARGS(&c1))) || !c1){fail("merge-context1-unavailable");return false;}
        FlatComputeInternalScope internal;
        // The copy is submitted immediately after this original draw, before
        // any unrelated game draw can alter the shared depth/stencil resource.
        ctx->CopyResource(cohortDepth_.Get(),depth_.Get());
        const auto dxmt=flatDetectDxmt(dev.Get(),ctx);
        FlatContextState saved;saved.capture(c1.Get(),flatContextRanges(dev->GetFeatureLevel(),dxmt.dxmt()),false);
        ctx->ClearState();
        const UINT extent[4]={width_,height_,0,0};ctx->UpdateSubresource(constants_.Get(),0,nullptr,extent,0,0);
        ID3D11ShaderResourceView* views[2]={layer_.coverageView(),cohortDepthView_.Get()};
        ID3D11UnorderedAccessView* uavs[2]={unionCoverageUav_.Get(),ownerDepthUav_.Get()};
        ctx->CSSetShaderResources(0,2,views);ctx->CSSetUnorderedAccessViews(0,2,uavs,nullptr);
        ID3D11Buffer* cb=constants_.Get();ctx->CSSetConstantBuffers(0,1,&cb);
        ctx->CSSetShader(mergeShader_.Get(),nullptr,0);
        ctx->Dispatch((width_+7)/8,(height_+7)/8,1);
        ctx->ClearState();saved.restore(c1.Get(),false);
        ++merged_;
        return true;
    }
public:
    bool active() const {return active_;}
    bool worldSeen() const {return worldSeen_;}
    bool needsPreWorldState(ID3D11Texture2D* depth) const {
        return active_ && !worldSeen_ && !consumerSeen_ && depth==depth_.Get();
    }
    uint32_t reported() const {return reported_;}
    bool plan(uint64_t frame,uint32_t seq,ID3D11Texture2D* color,ID3D11Texture2D* depth,
              ID3D11DepthStencilView* dsv,const Draw& draw,bool worldAlreadyNamed=false) {
        const auto decision=flatForegroundPlanDecision(frame,color&&depth&&dsv,active_,pending_,
            worldAlreadyNamed,worldSeen_,reported_,firstFrame_);
        if(decision==FlatForegroundPlanDecision::Skip)return false;
        if(decision==FlatForegroundPlanDecision::SkipAfterWorld){++lateSkipped_;return false;}
        if(decision==FlatForegroundPlanDecision::RejectActiveAfterWorld){
            fail("cohort-continued-after-world-source");return false;
        }
        if(decision==FlatForegroundPlanDecision::Start) {
            firstSeq_=lastSeq_=worldSeq_=consumerSeq_=0;
            width_=height_=planned_=draws_=merged_=polls_=0;
            depthDrawsBeforeWorld_=stencilDrawsBeforeWorld_=0;
            sameDepthDrawsAfterWorld_=depthMutations_=stencilClears_=0;
            drawOpen_=worldSeen_=consumerSeen_=pending_=foreign_=false;
            eventCount_=eventDropped_=0;currentEvent_=kChronologyLimit;
            depthWriteAll_=stencilReplace16_=fullViewport_=true;phaseMatch_=false;failure_.clear();
            color_.Reset();depth_.Reset();dsv_.Reset();cohortDepth_.Reset();cohortDepthView_.Reset();
            ownerDepth_.Reset();ownerDepthView_.Reset();ownerDepthUav_.Reset();
            unionCoverage_.Reset();unionCoverageView_.Reset();unionCoverageUav_.Reset();
            active_=true;frame_=frame;firstSeq_=seq;firstDraw_=draw;
            color_=color;depth_=depth;dsv_=dsv;
            D3D11_TEXTURE2D_DESC d{};color->GetDesc(&d);width_=d.Width;height_=d.Height;
            if(uint64_t(width_)*height_*14u>kMemoryLimit)fail("probe-combined-memory-budget");
            layer_.beginFrame(frame);
            Log::get().note("flat foreground ownership: armed frame=%llu seq=%u limit=2 second-earliest=%llu; automatic first-person Pool cohort, no F10, selector remains strict",
                (unsigned long long)frame,seq,(unsigned long long)(firstFrame_?firstFrame_+kSeparation:0));
        }
        if(frame_!=frame || color_.Get()!=color || depth_.Get()!=depth || dsv_.Get()!=dsv) {
            fail("cohort-resource-changed");return false;
        }
        if(!planned_)firstSeq_=seq;
        lastSeq_=seq;++planned_;
        currentEvent_=kChronologyLimit;
        if(auto* e=appendEvent('F',draw))currentEvent_=uint32_t(e-events_.data());
        return failure_.empty();
    }
    bool beginDraw(ID3D11DeviceContext* ctx,uint64_t frame) {
        if(!active_ || frame_!=frame || drawOpen_ || !ctx)return false;
        bool writesDepth=false,writesStencil=false;
        const bool mark=depthState(ctx,&writesDepth,&writesStencil);
        if(currentEvent_<eventCount_){captureState(ctx,events_[currentEvent_]);events_[currentEvent_].attempted=true;}
        depthWriteAll_ &= writesDepth;stencilReplace16_ &= mark;
        if(!writesDepth){fail("foreground-depth-write-off");return false;}
        D3D11_DEPTH_STENCIL_VIEW_DESC depthView{};dsv_->GetDesc(&depthView);
        if(depthView.Flags&D3D11_DSV_READ_ONLY_DEPTH)
            {fail("foreground-read-only-depth-view");return false;}
        D3D11_VIEWPORT vp{};UINT n=1;ctx->RSGetViewports(&n,&vp);
        fullViewport_ &= n==1 && vp.TopLeftX==0 && vp.TopLeftY==0 &&
            vp.Width==width_ && vp.Height==height_ && vp.MinDepth==0 && vp.MaxDepth==1;
        const char* reason=nullptr;
        drawOpen_=layer_.beginDraw(ctx,frame,color_.Get(),dsv_.Get(),&reason,true,true);
        if(!drawOpen_)fail(reason?reason:"private-coverage-refused");
        return drawOpen_;
    }
    void endDraw(ID3D11DeviceContext* ctx,bool completed=true) {
        if(!active_)return;
        if(drawOpen_) {
            layer_.endDraw(ctx);drawOpen_=false;
            if(completed) {
                ++draws_;
                if(mergeCurrentDraw(ctx) && currentEvent_<eventCount_)events_[currentEvent_].merged=true;
            }
        }
        else fail("draw-not-bracketed");
        if(const char* r=layer_.refusal())fail(r);
    }
    void noteSameDepthDraw(ID3D11DeviceContext* ctx,ID3D11Texture2D* depth,const Draw& evidence) {
        if(!active_ || !ctx || worldSeen_ || depth!=depth_.Get() || consumerSeen_)return;
        bool dw=false,sw=false;depthState(ctx,&dw,&sw);
        depthDrawsBeforeWorld_+=dw;stencilDrawsBeforeWorld_+=sw;
        appendEvent('G',evidence,ctx);
    }
    void noteAfterWorldDraw(ID3D11Texture2D* depth) {
        if(active_ && worldSeen_ && !consumerSeen_ && depth==depth_.Get())++sameDepthDrawsAfterWorld_;
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
    void worldSource(ID3D11DeviceContext*,uint64_t frame,uint32_t seq,ID3D11Texture2D* depth,
                     ID3D11DepthStencilView* dsv) {
        if(!active_ || frame_!=frame || worldSeen_)return;
        worldSeen_=true;worldSeq_=seq;
        if(!draws_ || depth!=depth_.Get() || dsv!=dsv_.Get())fail("first-world-source-or-cohort-missing");
    }
    void consumer(ID3D11DeviceContext* ctx,uint64_t frame,uint32_t seq,ID3D11Texture2D* depth,
                  ID3D11DepthStencilView* dsv,const unsigned char* worldCamera,bool foreign) {
        if(!active_ || frame_!=frame || consumerSeen_)return;
        consumerSeen_=true;consumerSeq_=seq;foreign_=foreign_||foreign;
        float world[6][4]{};if(worldCamera)std::memcpy(world,worldCamera,sizeof(world));
        phaseMatch_=worldCamera && flatCameraCenteredPairAtPhase(world,firstDraw_.camera,
            firstDraw_.phaseX,firstDraw_.phaseY,width_,height_);
        if(!worldSeen_)fail("no-world-source");
        if(depth!=depth_.Get() || dsv!=dsv_.Get())fail("consumer-depth-identity");
        if(!phaseMatch_)fail("foreground-world-phase-mismatch");
        if(merged_!=planned_)fail("eligible-draw-subset-only");
        if(eventDropped_)fail("chronology-cap");
        if(foreign_)fail("foreign-work-during-probe");
        if(depthMutations_)fail("shared-DS-explicit-mutation");
        if(stencilClears_)fail("shared-stencil-clear");
        if(!merged_ || !unionCoverageView_ || !ownerDepthView_ || depth!=depth_.Get() || dsv!=dsv_.Get()) {
            if(!merged_)fail("coverage-not-merged");
            report("partial-unavailable");return;
        }
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
        if(streamOutputBound){fail("consumer-stream-output-bound");report("partial-unavailable");return;}
        FlatComputeInternalScope internal;
        const auto dxmt=flatDetectDxmt(dev.Get(),ctx);
        FlatContextState saved;saved.capture(c1.Get(),flatContextRanges(dev->GetFeatureLevel(),dxmt.dxmt()),false);
        ctx->ClearState();
        const UINT zeros[4]{};ctx->ClearUnorderedAccessViewUint(counterUav_.Get(),zeros);
        const UINT extent[4]={width_,height_,0,0};ctx->UpdateSubresource(constants_.Get(),0,nullptr,extent,0,0);
        ID3D11ShaderResourceView* views[4]={unionCoverageView_.Get(),ownerDepthView_.Get(),currentDepth.Get(),stencil.Get()};
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
        if(!consumerSeen_ && completedFrame>=frame_) {fail("no-HDR-consumer");report("partial-unavailable");return;}
        if(!pending_ || !ctx)return;
        D3D11_MAPPED_SUBRESOURCE mapped{};
        FlatComputeInternalScope internal;
        const HRESULT hr=ctx->Map(staging_.Get(),0,D3D11_MAP_READ,D3D11_MAP_FLAG_DO_NOT_WAIT,&mapped);
        if(SUCCEEDED(hr)) {
            FlatForegroundOwnershipCounts c{};std::memcpy(&c,mapped.pData,sizeof(c));ctx->Unmap(staging_.Get(),0);
            report(failure_.empty()?"complete":"partial-measured",&c);return;
        }
        if(hr==DXGI_ERROR_WAS_STILL_DRAWING && ++polls_<kPollLimit)return;
        fail(hr==DXGI_ERROR_WAS_STILL_DRAWING?"readback-timeout":"readback-map-failed");report("failed");
    }
    void logStatus() const {
        Log::get().note("flat foreground ownership 5s: enabled=1 limit=2 reported=%u late-skipped=%u active=%u frame=%llu marked=%u merged=%u planned=%u world=%u consumer=%u pending=%u polls=%u reason=%s; no F10, zero covered is measured only when counts=measured",
            reported_,lateSkipped_,active_?1u:0u,(unsigned long long)frame_,layer_.markedDraws(),merged_,planned_,worldSeen_?1u:0u,
            consumerSeen_?1u:0u,pending_?1u:0u,polls_,failure_.empty()?"none":failure_.c_str());
    }
    void reset() {releaseGpu();active_=drawOpen_=worldSeen_=consumerSeen_=pending_=false;}
};
} // namespace edvr
