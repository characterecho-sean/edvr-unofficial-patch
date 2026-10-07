#pragma once
#include "animated_vertex_history.h"
#include "flat_animated_identity_ledger.h"
#include "flat_foreground_receipt.h"
#include <d3d11_1.h>
#include <array>
#include <cmath>
#include <cstring>

namespace edvr {
// The largest render size H may qualify at (design doc section 104). The first-person map is RGBA32F at the render size, so 64M
// pixels (8192x8192) is 1 GB. It covers a 4K screen supersampled 2.0 (7680x4320, 33.2M pixels). The bound it replaced, 16M,
// refused every on-foot frame of a 4K screen above SS 1.42, and those frames fell back to the spatial recovery with no AA.
inline constexpr uint64_t kFlatForegroundMaxPixels = 64ull * 1024 * 1024;
inline bool flatForegroundExtentAllowed(unsigned width, unsigned height) {
    return width && height && uint64_t(width) * height <= kFlatForegroundMaxPixels;
}
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
        unsigned writerToken=0;
        FlatAnimatedIdentityLedger::Identity identity;
        Certificate certificate;
        bool gpuIdentity=false;
        bool beforeWorld=false;
    };
    struct Output {
        Ptr<ID3D11ShaderResourceView> motion;
        bool qualified=false,resetRequired=false;
        unsigned frame=0;float depthNear=0;const char* refusal=nullptr;
        // The draws of this frame that were refused their capture and are covered per pixel (coverDraw): marked first-person in the
        // owner plane, without a sample in the map, so the prep refuses their pixels' history and gives them no motion. A qualified map
        // with coveredDraws above zero is a frame the old whole-frame refusal would have lost.
        unsigned coveredDraws=0;
    };
    struct CaptureStats {
        uint64_t attempts=0,gpuAttempts=0,submitted=0,preflightRefused=0,warmedAfterRefusal=0;
        // What the previous frame had for each submitted draw (the weapon's history, section 104), counted where the CPU decides it. A draw
        // is in exactly one of: noCandidate (history holds no record of its geometry from the frame before), noPriorPool / noPriorNear /
        // noPriorAbsent (the history has candidates and the adapter passed none on: the previous draw's pool or near differs, or no previous
        // draw is named by any candidate), priorsOne, priorsSeveral (the GPU map matches among them by identity). repeated counts the draws
        // that are not the first of their geometry in the frame (occurrence above 1), whichever of the above they are.
        uint64_t noCandidate=0,noPriorPool=0,noPriorNear=0,noPriorAbsent=0,priorsOne=0,priorsSeveral=0,repeated=0;
        // Draws refused their capture and covered per pixel (coverDraw), by cause: the occurrence cap, the history budget, and every other
        // refusal (a preflight bound, an unreadable identity, a draw shape the original-VS capture cannot take). windowed counts the draws
        // that were handed the window of four priors out of more (the extended policy: a mesh drawn more than four times in a frame).
        uint64_t coveredOccurrence=0,coveredBudget=0,coveredOther=0,windowed=0;
        void add(const CaptureStats& o) {
            attempts+=o.attempts;gpuAttempts+=o.gpuAttempts;submitted+=o.submitted;preflightRefused+=o.preflightRefused;
            warmedAfterRefusal+=o.warmedAfterRefusal;noCandidate+=o.noCandidate;noPriorPool+=o.noPriorPool;noPriorNear+=o.noPriorNear;
            noPriorAbsent+=o.noPriorAbsent;priorsOne+=o.priorsOne;priorsSeveral+=o.priorsSeveral;repeated+=o.repeated;
            coveredOccurrence+=o.coveredOccurrence;coveredBudget+=o.coveredBudget;coveredOther+=o.coveredOther;windowed+=o.windowed;
        }
    };
    const CaptureStats& stats() const{return stats_;}
    // The map the last prepareH drew (RGBA32F at its render size), or null before the first. For the offline bench's test export, which reads it
    // back in its own process; the next prepareH rewrites it.
    Ptr<ID3D11ShaderResourceView> mapView() const{return view_;}
    const EdvrFlatForegroundBudgetReceipt& budgetReceipt() const{return budgetReceipt_;}
    void reset(){*this=FlatForegroundMotion{};}
    void beginFrame(unsigned frame) {
        if(frame==frame_)return;
        if(frame==frame_+1)previous_=std::move(current_);else previous_.clear();
        current_.clear();frame_=frame;refusal_=nullptr;budgetReceipt_={};
        knownMutations_=unknownMutations_=0;covered_=0;gpuAttempted_=false;drawRefusal_=nullptr;history_.advance(frame);
    }
    // The frame's refusal: sticky, the first reason wins, and prepareH refuses while it stands.
    void fail(const char* reason){if(!refusal_)refusal_=reason?reason:"foreground-contract";}
    const char* refusal() const{return refusal_;}
    // Why the last capture() could not take its draw, or null (it did, whatever the frame's refusal). Not the frame's refusal: the draw is
    // not in the map. The caller decides what the draw's pixels become. If it marked them first-person in the owner plane it calls
    // coverDraw(), and they are refused per pixel (no sample: the prep gives them no history and no motion, never the camera term);
    // if it did not mark them, nothing says whose pixels they are, and it calls fail() with the reason, as every capture failure once did.
    const char* drawRefusal() const{return drawRefusal_;}
    void coverDraw(const char* reason) {
        ++covered_;
        if(reason && !std::strcmp(reason,"occurrence-cap"))++stats_.coveredOccurrence;
        else if(reason && !std::strcmp(reason,"history-budget"))++stats_.coveredBudget;
        else ++stats_.coveredOther;
    }
    unsigned coveredDraws() const{return covered_;}
    void resourceWritten(ID3D11Resource* resource) {
        const unsigned kind=history_.resourceWritten(resource);
        // Count only geometry invalidations, not unrelated resource-write notifications.
        if(!resource)++unknownMutations_;else if(kind&6)++knownMutations_;
        if(!kind)return;
        auto erase=[&](std::vector<Draw>& draws){draws.erase(std::remove_if(draws.begin(),draws.end(),[&](const Draw& d){
            return !resource || d.capture.geometry.vertices.Get()==resource || d.capture.geometry.indices.Get()==resource;}),draws.end());};
        erase(previous_);const size_t before=current_.size();erase(current_);
        if(kind&1)fail("foreground-unknown-resource-write");
        else if(before!=current_.size())fail("foreground-captured-geometry-written");
    }
    bool capture(ID3D11DeviceContext* ctx,PanelCurveDrawFn draw,unsigned count,unsigned instances,
                  unsigned start,int base,unsigned startInstance,unsigned frame,const Inputs& inputs) {
        beginFrame(frame);++stats_.attempts;drawRefusal_=nullptr;Draw d;d.inputs=inputs;
        // The map's empty clear follows the identity mode of the frame's draws, refused ones included (prepareH).
        if(inputs.gpuIdentity)gpuAttempted_=true;
        // reject: the frame's refusal (nothing says whose pixels these are). defer: this draw's alone (drawRefusal): the draw is not in
        // the map, and the caller covers its pixels per pixel when it has marked them.
        auto reject=[&](const char* reason){++stats_.preflightRefused;fail(reason);return false;};
        auto defer=[&](const char* reason){++stats_.preflightRefused;drawRefusal_=reason;return false;};
        if(!ctx)return reject("foreground-missing-context");
        if(!inputs.writerToken || inputs.writerToken>0xffffffu)return reject("foreground-writer-token-unavailable");
        if(!count || count%3 || count>AnimatedVertexHistory::maxVertices)return defer("foreground-primitive-bound");
        // An unidentified draw cannot match any later draw's authoritative
        // identity. Do not submit SO just to retain an unusable history record.
        // Other sticky frame failures still permit valid next-frame warming.
        d.identityKnown=(inputs.gpuIdentity || (!inputs.identity.refusal && inputs.identity.instanceEpoch &&
            inputs.identity.poolEpoch && inputs.identity.slot<=0x7ffffeu)) &&
            std::isfinite(inputs.camera[3][2]) && inputs.camera[3][2]>0 &&
            std::isfinite(inputs.phaseX) && std::isfinite(inputs.phaseY);
        if(!d.identityKnown)return defer(inputs.identity.refusal?inputs.identity.refusal:"foreground-identity-or-camera");
        Ptr<ID3D11VertexShader> activeVs;UINT classCount=0;ctx->VSGetShader(&activeVs,nullptr,&classCount);
        if(classCount)return defer("foreground-dynamic-VS-linkage");
        if(!inputs.gpuIdentity && (d.inputs.certificate.constants.size()>65536 || d.inputs.certificate.resourceCount>128)){
            return defer("foreground-certificate-bound");}
        if(current_.size()>=AnimatedVertexHistory::maxRecords)return defer("foreground-draw-bound");
        const bool warming=refusal_!=nullptr;++stats_.gpuAttempts;
        if(!history_.capture(ctx,draw,count,instances,start,base,startInstance,frame,d.capture,true,true)){
            // The budget receipt names the first budget refusal of the frame, covered or not.
            if(d.capture.refusal && std::strcmp(d.capture.refusal,"history-budget")==0 &&
               !budgetReceipt_.valid) {
                const auto usage=history_.accounting(frame);
                auto& r=budgetReceipt_;r.valid=1;r.requestedBytes=count*32;
                r.records=usage.recordCount;r.bytes=usage.bytes;r.invalid=usage.invalid;r.pending=usage.pending;
                r.current=usage.current;r.prior=usage.prior;r.older=usage.older;
                r.reclaimedRecords=usage.reclaimedRecords;r.reclaimedBytes=usage.reclaimedBytes;
                r.recordLimitHit=usage.recordCount>=AnimatedVertexHistory::maxRecords;
                r.byteLimitHit=uint64_t(usage.bytes)+r.requestedBytes>AnimatedVertexHistory::maxBytes;
                r.currentDraws=static_cast<unsigned>(current_.size());
                r.previousDraws=static_cast<unsigned>(previous_.size());
                for(const auto& old:current_)if(old.inputs.beforeWorld)++r.beforeWorldCurrent;
                for(const auto& old:previous_)if(old.inputs.beforeWorld)++r.beforeWorldPrevious;
                r.knownMutations=knownMutations_;r.unknownMutations=unknownMutations_;
            }
            drawRefusal_=d.capture.refusal?d.capture.refusal:"history-refused";return false;}
        ++stats_.submitted;if(warming)++stats_.warmedAfterRefusal;
        if(d.capture.priorRecords>d.capture.candidateCount)++stats_.windowed;
        ctx->RSGetState(&d.raster);UINT n=1;ctx->RSGetViewports(&n,&d.viewport);
        d.scissorCount=16;ctx->RSGetScissorRects(&d.scissorCount,d.scissors.data());
        for(const auto& old:previous_) {
            if(!d.identityKnown || !old.identityKnown || !samePool(d.capture,old.capture) ||
               d.inputs.camera[3][2]!=old.inputs.camera[3][2] ||
               (inputs.gpuIdentity && !old.inputs.gpuIdentity))continue;
            if(!inputs.gpuIdentity && !sameIdentity(d.inputs.identity,old.inputs.identity))continue;
            for(unsigned i=0;i<d.capture.candidateCount;++i)if(d.capture.previousPositions[i].Get()==old.capture.currentPositions.Get()) {
                if(inputs.gpuIdentity) {
                    if(d.priorCount<d.priors.size()) {
                        auto& prior=d.priors[d.priorCount++];prior.positions=d.capture.previousPositions[i];
                        prior.identity=d.capture.previousIdentity[i];
                        prior.phaseX=old.inputs.phaseX;prior.phaseY=old.inputs.phaseY;
                    }
                } else if(!d.oldPositions){d.oldPositions=d.capture.previousPositions[i];d.oldIdentity=d.capture.previousIdentity[i];d.oldInputs=old.inputs;}
                else if(!equivalent(d.oldInputs,old.inputs)){d.ambiguous=true;fail("foreground-ambiguous-prior-inputs");}
            }
        }
        tally(d);
        current_.push_back(std::move(d));return !refusal_;
    }
    bool prepareH(ID3D11DeviceContext* ctx,ID3D11ShaderResourceView* owners,ID3D11ShaderResourceView* rawDepth,
                  const float worldCamera[6][4],unsigned frame,unsigned width,unsigned height,Output& out) {
        out=Output{};out.frame=frame;
        auto refuse=[&](const char* reason){out.refusal=reason;return false;};
        if(frame!=frame_ || refusal_)return refuse(refusal_?refusal_:"foreground-frame");
        out.coveredDraws=covered_;
        if(!ctx || !owners || !rawDepth || !flatForegroundExtentAllowed(width,height))
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
            // GPU class-2 samples reject first-seen/ambiguous history locally.
            // Only the legacy CPU adapter needs this whole-frame request.
            if(!d.inputs.gpuIdentity && !d.oldPositions)out.resetRequired=true;
        }
        if(!initialize(ctx,width,height))return refuse("foreground-map-create");
        if(previousNear_ && previousNear_!=commonNear)out.resetRequired=true;
        previousNear_=commonNear;
        // H is bracketed independently of original geometry capture. Restore
        // every binding touched here, including all eight OM render targets.
        Ptr<ID3D11VertexShader> oldVs;Ptr<ID3D11PixelShader> oldPs;Ptr<ID3D11InputLayout> oldLayout;
        Ptr<ID3D11RasterizerState> oldRaster;Ptr<ID3D11BlendState> oldBlend;Ptr<ID3D11DepthStencilState> oldDepth;
        Ptr<ID3D11Buffer> oldVsCb,oldPsCb;UINT oldVsFirst=0,oldVsCount=4096,oldPsFirst=0,oldPsCount=4096;
        Ptr<ID3D11DeviceContext1> context1;ctx->QueryInterface(IID_PPV_ARGS(&context1));
        UINT oldRef=0,oldMask=0;float oldFactors[4]{};
        ID3D11RenderTargetView* oldTargets[8]{};ID3D11DepthStencilView* oldDsv=nullptr;
        ID3D11ShaderResourceView* oldVsViews[15]{},*oldPsViews[2]{};D3D11_PRIMITIVE_TOPOLOGY oldTopology;
        D3D11_VIEWPORT oldViewport[16]{};UINT oldViewportCount=16;D3D11_RECT oldScissors[16]{};UINT oldScissorCount=16;
        ctx->VSGetShader(&oldVs,nullptr,nullptr);ctx->PSGetShader(&oldPs,nullptr,nullptr);ctx->IAGetInputLayout(&oldLayout);
        ctx->RSGetState(&oldRaster);ctx->OMGetBlendState(&oldBlend,oldFactors,&oldMask);ctx->OMGetDepthStencilState(&oldDepth,&oldRef);
        if(context1){context1->VSGetConstantBuffers1(0,1,&oldVsCb,&oldVsFirst,&oldVsCount);
            context1->PSGetConstantBuffers1(0,1,&oldPsCb,&oldPsFirst,&oldPsCount);}
        else {ctx->VSGetConstantBuffers(0,1,&oldVsCb);ctx->PSGetConstantBuffers(0,1,&oldPsCb);}
        ctx->VSGetShaderResources(0,15,oldVsViews);ctx->PSGetShaderResources(0,2,oldPsViews);
        ctx->OMGetRenderTargets(8,oldTargets,&oldDsv);ctx->IAGetPrimitiveTopology(&oldTopology);
        ctx->RSGetViewports(&oldViewportCount,oldViewport);ctx->RSGetScissorRects(&oldScissorCount,oldScissors);
        ctx->OMSetRenderTargets(1,target_.GetAddressOf(),nullptr);ctx->OMSetBlendState(blend_.Get(),nullptr,~0u);ctx->OMSetDepthStencilState(depth_.Get(),0);
        // A frame whose draws were all refused has an empty current_, and its map still follows the GPU convention: z of -1 marks "no
        // sample" so the prep keeps the pixel's own depth, where the legacy clear's z of 0 would hand it a depth of zero.
        const bool gpuFrame=gpuAttempted_ || std::any_of(current_.begin(),current_.end(),[](const Draw& d){return d.inputs.gpuIdentity;});
        float empty[4]={0,0,gpuFrame?-1.0f:0.0f,0};ctx->ClearRenderTargetView(target_.Get(),empty);
        ctx->VSSetShader(vs_.Get(),nullptr,0);ctx->PSSetShader(ps_.Get(),nullptr,0);ctx->IASetInputLayout(nullptr);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->VSSetConstantBuffers(0,1,settings_.GetAddressOf());ctx->PSSetConstantBuffers(0,1,settings_.GetAddressOf());
        ID3D11ShaderResourceView* psViews[2]={owners,rawDepth};ctx->PSSetShaderResources(0,2,psViews);
        for(const auto& d:current_) {
            Settings c{};c.extentPhase[0]=float(width);c.extentPhase[1]=float(height);c.extentPhase[2]=d.inputs.phaseX;c.extentPhase[3]=d.inputs.phaseY;
            c.previousPhaseDepth[0]=d.oldInputs.phaseX;c.previousPhaseDepth[1]=d.oldInputs.phaseY;c.previousPhaseDepth[2]=commonNear;
            c.expected[0]=d.inputs.identity.slot;c.expected[1]=d.inputs.identity.skeleton;c.expected[2]=d.inputs.identity.allocation;c.expected[3]=d.oldPositions?1u:2u;
            c.provenance[0]=d.inputs.writerToken;
            c.identityMode[0]=d.inputs.gpuIdentity?1u:0u;c.identityMode[1]=d.priorCount;
            for(unsigned i=0;i<d.priorCount;++i){c.priorPhase[i][0]=d.priors[i].phaseX;c.priorPhase[i][1]=d.priors[i].phaseY;}
            ctx->UpdateSubresource(settings_.Get(),0,nullptr,&c,0,0);
            ID3D11ShaderResourceView* vsViews[15]{};
            vsViews[0]=d.capture.currentPositions.Get();
            // t1-t4 the priors' positions, t5 the draw's identity, t6-t9 the priors' identities, t10 the draw's own raw instance index. The
            // priors' slots are not bound: the match is by identity (flat_foreground_motion_shader.h), a slot is not an identity.
            if(d.inputs.gpuIdentity){for(unsigned i=0;i<d.priorCount;++i){vsViews[1+i]=d.priors[i].positions.Get();
                    vsViews[6+i]=d.priors[i].identity.Get();}
                vsViews[5]=d.capture.currentIdentity.Get();vsViews[10]=d.capture.instanceIndex.Get();}
            else {vsViews[1]=d.oldPositions.Get();vsViews[5]=d.capture.currentIdentity.Get();
                vsViews[6]=d.oldIdentity.Get();vsViews[10]=d.capture.instanceIndex.Get();}
            ctx->VSSetShaderResources(0,15,vsViews);ctx->RSSetState(d.raster.Get());ctx->RSSetViewports(1,&d.viewport);ctx->RSSetScissorRects(d.scissorCount,d.scissors.data());
            ctx->Draw(d.capture.geometry.count,0);
        }
        ID3D11ShaderResourceView* nullVs[15]{},*nullPs[2]{};ctx->VSSetShaderResources(0,15,nullVs);ctx->PSSetShaderResources(0,2,nullPs);
        ctx->OMSetRenderTargets(8,oldTargets,oldDsv);ctx->OMSetBlendState(oldBlend.Get(),oldFactors,oldMask);ctx->OMSetDepthStencilState(oldDepth.Get(),oldRef);
        ctx->VSSetShader(oldVs.Get(),nullptr,0);ctx->PSSetShader(oldPs.Get(),nullptr,0);ctx->IASetInputLayout(oldLayout.Get());ctx->IASetPrimitiveTopology(oldTopology);
        if(context1){context1->VSSetConstantBuffers1(0,1,oldVsCb.GetAddressOf(),&oldVsFirst,&oldVsCount);
            context1->PSSetConstantBuffers1(0,1,oldPsCb.GetAddressOf(),&oldPsFirst,&oldPsCount);}
        else {ctx->VSSetConstantBuffers(0,1,&oldVsCb);ctx->PSSetConstantBuffers(0,1,&oldPsCb);}
        ctx->VSSetShaderResources(0,15,oldVsViews);ctx->PSSetShaderResources(0,2,oldPsViews);
        ctx->RSSetState(oldRaster.Get());ctx->RSSetViewports(oldViewportCount,oldViewport);ctx->RSSetScissorRects(oldScissorCount,oldScissors);
        for(auto* p:oldTargets)if(p)p->Release();if(oldDsv)oldDsv->Release();
        for(auto* p:oldVsViews)if(p)p->Release();for(auto* p:oldPsViews)if(p)p->Release();
        out.motion=view_;out.qualified=true;out.depthNear=commonNear;return true;
    }
private:
    CaptureStats stats_{};
    EdvrFlatForegroundBudgetReceipt budgetReceipt_{};
    unsigned knownMutations_=0,unknownMutations_=0;
    struct Draw {
        AnimatedVertexHistory::Capture capture;Inputs inputs,oldInputs;
        struct Prior {Ptr<ID3D11ShaderResourceView> positions,identity;float phaseX=0,phaseY=0;};
        std::array<Prior,4> priors{};unsigned priorCount=0;
        Ptr<ID3D11ShaderResourceView> oldPositions,oldIdentity;Ptr<ID3D11RasterizerState> raster;
        D3D11_VIEWPORT viewport{};std::array<D3D11_RECT,16> scissors{};UINT scissorCount=0;
        bool identityKnown=false,ambiguous=false;
    };
    struct Settings {float extentPhase[4]{},previousPhaseDepth[4]{};uint32_t expected[4]{},provenance[4]{};
        float priorPhase[4][4]{};uint32_t identityMode[4]{};};
    static bool sameIdentity(const FlatAnimatedIdentityLedger::Identity& a,const FlatAnimatedIdentityLedger::Identity& b) {
        return a.slot==b.slot && a.skeleton==b.skeleton && a.allocation==b.allocation;
    }
    static bool samePool(const AnimatedVertexHistory::Capture& a,const AnimatedVertexHistory::Capture& b) {
        Ptr<ID3D11Resource> x,y;a.pool->GetResource(&x);b.pool->GetResource(&y);return x==y;
    }
    // Files a submitted draw under what its history found (CaptureStats). A draw whose candidates reached no prior is filed by the first test
    // that refused the previous draws its candidates name: the pool, then the near; none of them refusing, the previous draw is not there.
    void tally(const Draw& d) {
        if(d.capture.occurrences>0)++stats_.repeated;
        if(d.capture.candidateCount==0){++stats_.noCandidate;return;}
        if(d.inputs.gpuIdentity?d.priorCount>0:bool(d.oldPositions)) {
            if(d.inputs.gpuIdentity)++(d.priorCount==1?stats_.priorsOne:stats_.priorsSeveral);
            return;
        }
        bool poolRefused=false,nearRefused=false;
        for(const auto& old:previous_) {
            bool named=false;
            for(unsigned i=0;i<d.capture.candidateCount && !named;++i)named=d.capture.previousPositions[i].Get()==old.capture.currentPositions.Get();
            if(!named)continue;
            if(!samePool(d.capture,old.capture))poolRefused=true;
            else if(d.inputs.camera[3][2]!=old.inputs.camera[3][2])nearRefused=true;
        }
        ++(poolRefused?stats_.noPriorPool:nearRefused?stats_.noPriorNear:stats_.noPriorAbsent);
    }
    static bool textureExtent(ID3D11ShaderResourceView* view,unsigned width,unsigned height,bool owner) {
        D3D11_SHADER_RESOURCE_VIEW_DESC v{};view->GetDesc(&v);
        if(v.ViewDimension!=D3D11_SRV_DIMENSION_TEXTURE2D || v.Texture2D.MostDetailedMip ||
           (owner?v.Format!=DXGI_FORMAT_R32G32B32A32_FLOAT:
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
    const char* refusal_=nullptr,*drawRefusal_=nullptr;float previousNear_=0;unsigned covered_=0;bool gpuAttempted_=false;
    Ptr<ID3D11VertexShader> vs_;Ptr<ID3D11PixelShader> ps_;Ptr<ID3D11Buffer> settings_;
    Ptr<ID3D11BlendState> blend_;Ptr<ID3D11DepthStencilState> depth_;
    Ptr<ID3D11Texture2D> texture_;Ptr<ID3D11RenderTargetView> target_;Ptr<ID3D11ShaderResourceView> view_;
};
} // namespace edvr
