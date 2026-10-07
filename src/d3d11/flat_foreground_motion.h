#pragma once
#include "animated_vertex_history.h"
#include "flat_animated_identity_ledger.h"
#include "flat_foreground_identity_sample.h"
#include "flat_foreground_receipt.h"
#include "flat_foreground_sibling.h"
#include "cs_stage_save.h"
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
// used by VR. No diagnostic timer or per-weapon selector is here, and no readback but one: the sampled identity words (section 104,
// flat_foreground_identity_sample.h), copied to staging and read frames later without waiting. It decides nothing.
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
        // The draw's shader pair, for the examples the no-candidate and identity lines name (section 104). Naming only: nothing reads them.
        uint64_t vs=0,ps=0;
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
        // The pistol's no-candidate bursts (section 104). missBy counts, by pattern, the submitted draws that found no record of their exact
        // geometry key that the frame before used (animated_history_ledger.h: every pattern, zeros included). acrossOffset is the part the
        // history rescued (HistoryGap::OffsetShift: the same mesh at another place in the same buffers, handed over as the one candidate),
        // so the draws still in noCandidate are the misses less the rescued. frames counts the frames that submitted a draw, framesMissing
        // those with at least one miss, framesAllMissing those where every submitted draw missed. resetFrames counts the frames H asked
        // the backend to reset history for, nearChanges those where the common near plane moved.
        uint64_t missBy[kHistoryGapCount]={};
        uint64_t acrossOffset=0,frames=0,framesMissing=0,framesAllMissing=0,resetFrames=0,nearChanges=0;
        // The rescues withdrawn at the end of the frame because another draw used the donor record that frame: two parts of one object, not a
        // mesh that moved (AnimatedVertexHistory::rescueHolds). The prior is dropped and the map says no prior, as it did before the rescue.
        uint64_t rescueCancelled=0;
        // The sampled identity readback (flat_foreground_identity_sample.h): the draws sampled, by what the map will have decided for them.
        uint64_t identitySamples=0;
        uint64_t identityBy[kIdentityVerdictCount]={};
        // The sibling pass (flat_foreground_sibling.h): the frames it ran for, its dispatches, the frames read back, unread or failed (a resource
        // it could not make), and every draw without history by the pattern the history gave it and what the pass made of it (draws, read back
        // from the GPU a few frames late).
        uint64_t siblingFrames=0,siblingDispatches=0,siblingReads=0,siblingNotReady=0,siblingFailed=0;
        uint64_t siblingBy[kSiblingPatterns][kSiblingOutcomes]={};
        void add(const CaptureStats& o) {
            attempts+=o.attempts;gpuAttempts+=o.gpuAttempts;submitted+=o.submitted;preflightRefused+=o.preflightRefused;
            warmedAfterRefusal+=o.warmedAfterRefusal;noCandidate+=o.noCandidate;noPriorPool+=o.noPriorPool;noPriorNear+=o.noPriorNear;
            noPriorAbsent+=o.noPriorAbsent;priorsOne+=o.priorsOne;priorsSeveral+=o.priorsSeveral;repeated+=o.repeated;
            coveredOccurrence+=o.coveredOccurrence;coveredBudget+=o.coveredBudget;coveredOther+=o.coveredOther;windowed+=o.windowed;
            for(unsigned i=0;i<kHistoryGapCount;++i)missBy[i]+=o.missBy[i];
            acrossOffset+=o.acrossOffset;frames+=o.frames;framesMissing+=o.framesMissing;framesAllMissing+=o.framesAllMissing;
            resetFrames+=o.resetFrames;nearChanges+=o.nearChanges;rescueCancelled+=o.rescueCancelled;identitySamples+=o.identitySamples;
            for(unsigned i=0;i<kIdentityVerdictCount;++i)identityBy[i]+=o.identityBy[i];
            siblingFrames+=o.siblingFrames;siblingDispatches+=o.siblingDispatches;siblingReads+=o.siblingReads;
            siblingNotReady+=o.siblingNotReady;siblingFailed+=o.siblingFailed;
            for(unsigned p=0;p<kSiblingPatterns;++p)for(unsigned k=0;k<kSiblingOutcomes;++k)siblingBy[p][k]+=o.siblingBy[p][k];
        }
    };
    // The first draws of a window that found no same-key prior, one per pattern, with the whole key and the nearest entry's (section 104).
    struct MissExample {
        unsigned frame=0;HistoryClass miss;HistoryKey key;bool rescued=false;uint64_t vs=0,ps=0;
    };
    static constexpr unsigned kMissExamples=6,kIdentityExamples=4;
    // Hands over (and forgets) the examples gathered since the last call.
    unsigned takeMissExamples(MissExample* out,unsigned capacity) {
        const unsigned n=(std::min)(missExampleCount_,capacity);
        for(unsigned i=0;i<n;++i)out[i]=missExamples_[i];
        missExampleCount_=0;return n;
    }
    unsigned takeIdentityExamples(FlatIdentitySampler::Sample* out,unsigned capacity) {
        const unsigned n=(std::min)(identityExampleCount_,capacity);
        for(unsigned i=0;i<n;++i)out[i]=identityExamples_[i];
        identityExampleCount_=0;return n;
    }
    // The longest run of consecutive frames with a same-key miss since the last call (the window's), and the identity samples that could
    // not be read or taken.
    unsigned takeLongestMissRun() {const unsigned n=longestRun_;longestRun_=0;return n;}
    uint64_t identitySkipped() const{return sampler_.skipped();}
    uint64_t identityNotReady() const{return sampler_.notReady();}
    // Reads the identity samples that are ready (all of them, waiting, in a rig). The capture does it once a frame; a rig asks directly.
    void pollIdentity(ID3D11DeviceContext* ctx,unsigned frame,bool wait=false) {
        sampler_.poll(ctx,frame,wait,[&](const FlatIdentitySampler::Sample& s){
            ++stats_.identitySamples;++stats_.identityBy[unsigned(s.verdict)];
            // A draw that had priors and no identity match is a receiver the pass cannot see coming: the CPU knows only draws with no prior.
            // Arm it for a while, so the next frames' draws with priors are matched against their siblings too.
            if(s.verdict!=IdentityVerdict::Match && s.frame+kSiblingArmedFrames>siblingArmedUntil_)siblingArmedUntil_=s.frame+kSiblingArmedFrames;
            if(s.verdict!=IdentityVerdict::Match && identityExampleCount_<kIdentityExamples)identityExamples_[identityExampleCount_++]=s;
        });
    }
    // Reads the sibling outcomes that are ready (all of them, waiting, in a rig). The capture does it once a frame beside the identity samples.
    void pollSibling(ID3D11DeviceContext* ctx,unsigned frame,bool wait=false) {
        if(!ctx)return;
        for(SiblingSlot& s:siblingSlots_) {
            if(!s.pending)continue;
            if(!wait && frame-s.frame<2)continue;
            D3D11_MAPPED_SUBRESOURCE m{};
            const HRESULT hr=ctx->Map(s.stage.Get(),0,D3D11_MAP_READ,wait?0u:D3D11_MAP_FLAG_DO_NOT_WAIT,&m);
            if(FAILED(hr) || !m.pData) {
                if(frame-s.frame>8){s.pending=false;++stats_.siblingNotReady;}
                continue;
            }
            const float* fit=static_cast<const float*>(m.pData);
            for(unsigned d=0;d<s.draws;++d) {
                const float mode=fit[d*8+2],matched=fit[d*8+6];
                if(matched!=0)continue;
                const unsigned outcome=unsigned(mode+.5f);
                if(outcome>=1 && outcome<=kSiblingOutcomes)++stats_.siblingBy[s.pattern[d]][outcome-1];
            }
            ctx->Unmap(s.stage.Get(),0);
            s.pending=false;++stats_.siblingReads;
        }
    }
    // Whether the last prepareH ran the sibling pass for its frame (a rig asks).
    bool siblingRan() const{return siblingFrame_==frame_ && siblingOk_;}
    // FOR THE RIGS ONLY: the donors' records and the fit table of the last frame the pass ran for, read back and waited for. `draws` is the
    // frame's draw count; donors[i] is draw i's record as the first shader wrote it, fit[i] its two float4 (x, y, mode, spread; donor draws,
    // donor vertices, matched, 0). False when the pass has not run.
    bool readSiblingTables(ID3D11DeviceContext* ctx,std::vector<SiblingDonor>& donors,std::vector<std::array<float,8>>& fit,unsigned& draws) {
        draws=0;donors.clear();fit.clear();
        if(!ctx || !siblingOk_ || !donors_ || !fit_)return false;
        Ptr<ID3D11Device> dev;ctx->GetDevice(&dev);
        const unsigned n=siblingDraws_;
        const auto readBack=[&](ID3D11Buffer* source,unsigned elements,std::vector<uint32_t>& words) {
            D3D11_BUFFER_DESC d{};d.ByteWidth=elements*16;d.Usage=D3D11_USAGE_STAGING;d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
            d.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;d.StructureByteStride=16;
            Ptr<ID3D11Buffer> stage;if(FAILED(dev->CreateBuffer(&d,nullptr,&stage)))return false;
            const D3D11_BOX box{0,0,0,elements*16,1,1};ctx->CopySubresourceRegion(stage.Get(),0,0,0,0,source,0,&box);
            D3D11_MAPPED_SUBRESOURCE m{};if(FAILED(ctx->Map(stage.Get(),0,D3D11_MAP_READ,0,&m)))return false;
            words.assign(static_cast<const uint32_t*>(m.pData),static_cast<const uint32_t*>(m.pData)+elements*4);ctx->Unmap(stage.Get(),0);return true;
        };
        std::vector<uint32_t> d,f;
        if(!readBack(donors_.Get(),n*3,d) || !readBack(fit_.Get(),n*2,f))return false;
        const auto real=[](uint32_t bits){float v;std::memcpy(&v,&bits,4);return v;};
        for(unsigned i=0;i<n;++i) {
            SiblingDonor s;const uint32_t* r=&d[i*12];
            s.matched=r[0]!=0;s.vertices=r[1];s.identityX=r[2];s.identityY=r[3];
            s.mean[0]=real(r[4]);s.mean[1]=real(r[5]);
            s.lo[0]=real(r[8]);s.lo[1]=real(r[9]);s.hi[0]=real(r[10]);s.hi[1]=real(r[11]);
            donors.push_back(s);
            std::array<float,8> row{};for(unsigned k=0;k<8;++k)row[k]=real(f[i*8+k]);fit.push_back(row);
        }
        draws=n;return true;
    }
    // FOR THE RIGS ONLY. The base map's reasons (3, 5, 6 for a draw with no history of its own) are what the sibling pass replaces for a draw
    // that has none; a rig that holds the base contract apart (tools\weapon_motion_test, the identity and covered-draw scenes) turns the pass
    // off for its scenes and on again. The runtime never calls it: the pass is on.
    static void rigSiblingPass(bool on){siblingPassEnabled()=on;}
    struct RigSiblingPass {
        bool was;
        explicit RigSiblingPass(bool on):was(siblingPassEnabled()){siblingPassEnabled()=on;}
        ~RigSiblingPass(){siblingPassEnabled()=was;}
        RigSiblingPass(const RigSiblingPass&)=delete;RigSiblingPass& operator=(const RigSiblingPass&)=delete;
    };
    const CaptureStats& stats() const{return stats_;}
    // The map the last prepareH drew (RGBA32F at its render size), or null before the first. For the offline bench's test export, which reads it
    // back in its own process; the next prepareH rewrites it.
    Ptr<ID3D11ShaderResourceView> mapView() const{return view_;}
    const EdvrFlatForegroundBudgetReceipt& budgetReceipt() const{return budgetReceipt_;}
    void reset(){*this=FlatForegroundMotion{};}
    void beginFrame(unsigned frame) {
        if(frame==frame_)return;
        // The frame that ends: the draws it submitted and how many of them found no same-key prior (section 104's frame counters).
        if(frameSubmitted_) {
            ++stats_.frames;
            if(frameMissed_) {
                ++stats_.framesMissing;
                missRun_=(lastMissFrame_ && frame_==lastMissFrame_+1)?missRun_+1:1;lastMissFrame_=frame_;
                if(missRun_>longestRun_)longestRun_=missRun_;
            }
            if(frameMissed_==frameSubmitted_)++stats_.framesAllMissing;
        }
        frameSubmitted_=frameMissed_=0;
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
        // The identity samples a few frames old are read once a frame, here, without waiting.
        if(polledFrame_!=frame){polledFrame_=frame;pollIdentity(ctx,frame);pollSibling(ctx,frame);}
        // The map's empty clear follows the identity mode of the frame's draws, refused ones included (prepareH).
        if(inputs.gpuIdentity)gpuAttempted_=true;
        // reject: the frame's refusal (nothing says whose pixels these are). defer: this draw's alone (drawRefusal): the draw is not in
        // the map, and the caller covers its pixels per pixel when it has marked them. Either way the history was never asked, and the
        // ledger remembers that this frame offered it a draw (section 104: a frame whose draws were all turned away here has no prior).
        auto reject=[&](const char* reason){++stats_.preflightRefused;history_.noteNotOffered(frame);fail(reason);return false;};
        auto defer=[&](const char* reason){++stats_.preflightRefused;history_.noteNotOffered(frame);drawRefusal_=reason;return false;};
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
        ++frameSubmitted_;
        if(d.capture.missed) {
            ++frameMissed_;
            // The first draw of each pattern in the window, with the whole key, for the line that names it.
            bool seen=false;
            for(unsigned i=0;i<missExampleCount_;++i)seen=seen||missExamples_[i].miss.gap==d.capture.miss.gap;
            if(!seen && missExampleCount_<kMissExamples) {
                MissExample& e=missExamples_[missExampleCount_++];
                e.frame=frame;e.miss=d.capture.miss;e.key=d.capture.key;e.rescued=d.capture.acrossOffset;e.vs=inputs.vs;e.ps=inputs.ps;
            }
        }
        // One draw in thirteen that the map will match by identity has its identity words read back a few frames later (section 104): the
        // priors the map is handed, which are fewer than the history's candidates when the adapter filtered some.
        if(inputs.gpuIdentity && d.priorCount && d.capture.currentIdentity) {
            ID3D11ShaderResourceView* identities[4]{};
            for(unsigned i=0;i<d.priorCount;++i)identities[i]=d.priors[i].identity.Get();
            sampler_.consider(ctx,frame,d.capture.currentIdentity.Get(),identities,d.priorCount,d.capture.key,inputs.vs,inputs.ps);
        }
        current_.push_back(std::move(d));return !refusal_;
    }
    bool prepareH(ID3D11DeviceContext* ctx,ID3D11ShaderResourceView* owners,ID3D11ShaderResourceView* rawDepth,
                  const float worldCamera[6][4],unsigned frame,unsigned width,unsigned height,Output& out) {
        out=Output{};out.frame=frame;
        auto refuse=[&](const char* reason){out.refusal=reason;return false;};
        if(frame!=frame_ || refusal_)return refuse(refusal_?refusal_:"foreground-frame");
        out.coveredDraws=covered_;
        // The frame's draws are all in: an offset-shift rescue whose donor another draw used this frame was two parts of one object, not a
        // mesh that moved. Its prior is withdrawn (section 104, rescueHolds); the map then says no prior for it, as before the rescue.
        for(auto& d:current_)
            if(d.capture.acrossOffset && d.priorCount && !history_.rescueHolds(d.capture,frame)){d.priorCount=0;++stats_.rescueCancelled;}
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
        if(previousNear_ && previousNear_!=commonNear){out.resetRequired=true;++stats_.nearChanges;}
        previousNear_=commonNear;
        // The sibling pass (flat_foreground_sibling.h): the draws with no history of their own take their siblings' motion. Compute only: the
        // map's state is bound after it, so nothing of it needs undoing but the compute stage, which the pass saves and restores itself.
        const bool siblings=runSibling(ctx,width,height,commonNear,frame);
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
        for(unsigned drawIndex=0;drawIndex<current_.size();++drawIndex) {
            const Draw& d=current_[drawIndex];
            Settings c=settingsFor(d,width,height,commonNear);
            c.sibling[0]=(siblings && d.inputs.gpuIdentity)?1u:0u;c.sibling[1]=drawIndex;c.sibling[2]=d.capture.geometry.count;
            ctx->UpdateSubresource(settings_.Get(),0,nullptr,&c,0,0);
            ID3D11ShaderResourceView* vsViews[15]{};
            vsViews[0]=d.capture.currentPositions.Get();
            // t1-t4 the priors' positions, t5 the draw's identity, t6-t9 the priors' identities, t10 the draw's own raw instance index. The
            // priors' slots are not bound: the match is by identity (flat_foreground_motion_shader.h), a slot is not an identity.
            if(d.inputs.gpuIdentity){for(unsigned i=0;i<d.priorCount;++i){vsViews[1+i]=d.priors[i].positions.Get();
                    vsViews[6+i]=d.priors[i].identity.Get();}
                vsViews[5]=d.capture.currentIdentity.Get();vsViews[10]=d.capture.instanceIndex.Get();
                // t11 the sibling pass's fit, one pair of float4 a draw by its index in the frame (flat_foreground_motion_shader.h).
                if(siblings)vsViews[11]=fitSrv_.Get();}
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
        out.motion=view_;out.qualified=true;out.depthNear=commonNear;
        if(out.resetRequired)++stats_.resetFrames;
        return true;
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
        float priorPhase[4][4]{};uint32_t identityMode[4]{};uint32_t sibling[4]{};};
    struct FitSettings {uint32_t counts[4]{};float limits[4]{};};
    static bool& siblingPassEnabled(){static bool on=true;return on;}
    static constexpr unsigned kSiblingArmedFrames=30,kSiblingSlots=4;
    static constexpr unsigned kSiblingDraws=AnimatedVertexHistory::maxRecords;
    // What the map's shaders are told about a draw (the sibling fields are the caller's).
    static Settings settingsFor(const Draw& d,unsigned width,unsigned height,float commonNear) {
        Settings c{};c.extentPhase[0]=float(width);c.extentPhase[1]=float(height);c.extentPhase[2]=d.inputs.phaseX;c.extentPhase[3]=d.inputs.phaseY;
        c.previousPhaseDepth[0]=d.oldInputs.phaseX;c.previousPhaseDepth[1]=d.oldInputs.phaseY;c.previousPhaseDepth[2]=commonNear;
        c.expected[0]=d.inputs.identity.slot;c.expected[1]=d.inputs.identity.skeleton;c.expected[2]=d.inputs.identity.allocation;c.expected[3]=d.oldPositions?1u:2u;
        c.provenance[0]=d.inputs.writerToken;
        c.identityMode[0]=d.inputs.gpuIdentity?1u:0u;c.identityMode[1]=d.priorCount;
        for(unsigned i=0;i<d.priorCount;++i){c.priorPhase[i][0]=d.priors[i].phaseX;c.priorPhase[i][1]=d.priors[i].phaseY;}
        return c;
    }
    struct SiblingSlot {Ptr<ID3D11Buffer> stage;bool pending=false;unsigned frame=0,draws=0;uint8_t pattern[kSiblingDraws]{};};
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
        // Why the exact key had no prior (section 104), whichever of the filings below the draw then takes.
        if(d.capture.missed) {
            ++stats_.missBy[unsigned(d.capture.miss.gap)];
            if(d.capture.acrossOffset)++stats_.acrossOffset;
        }
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
    // The sibling pass's resources, made when the first frame needs them: the two compute shaders, the donors' records (three uint4 a draw,
    // written by the first), the draws' own identity words (copied in), the fit table (two float4 a draw, written by the second and read by the
    // map's vertex shader), the second's limits, and the staging the fit is read back through.
    bool createSibling(ID3D11DeviceContext* ctx) {
        if(donorCs_ && fitCs_ && donors_ && receivers_ && fit_ && fitSettings_)return true;
        Ptr<ID3D11Device> dev;ctx->GetDevice(&dev);
        if(!donorCs_)donorCs_.Attach(shaderSwapCreateCs(ctx,kFlatForegroundDonorBytecode,sizeof(kFlatForegroundDonorBytecode),"flat foreground donor","flat foreground motion"));
        if(!fitCs_)fitCs_.Attach(shaderSwapCreateCs(ctx,kFlatForegroundFitBytecode,sizeof(kFlatForegroundFitBytecode),"flat foreground fit","flat foreground motion"));
        if(!donorCs_ || !fitCs_)return false;
        const auto structured=[&](unsigned elements,UINT bind,Ptr<ID3D11Buffer>& buffer) {
            D3D11_BUFFER_DESC d{};d.ByteWidth=elements*16;d.Usage=D3D11_USAGE_DEFAULT;d.BindFlags=bind;
            d.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;d.StructureByteStride=16;
            return SUCCEEDED(dev->CreateBuffer(&d,nullptr,&buffer));
        };
        const auto views=[&](ID3D11Buffer* buffer,unsigned elements,Ptr<ID3D11ShaderResourceView>& srv,Ptr<ID3D11UnorderedAccessView>* uav) {
            D3D11_SHADER_RESOURCE_VIEW_DESC s{};s.Format=DXGI_FORMAT_UNKNOWN;s.ViewDimension=D3D11_SRV_DIMENSION_BUFFER;s.Buffer.NumElements=elements;
            if(FAILED(dev->CreateShaderResourceView(buffer,&s,&srv)))return false;
            if(!uav)return true;
            D3D11_UNORDERED_ACCESS_VIEW_DESC u{};u.Format=DXGI_FORMAT_UNKNOWN;u.ViewDimension=D3D11_UAV_DIMENSION_BUFFER;u.Buffer.NumElements=elements;
            return SUCCEEDED(dev->CreateUnorderedAccessView(buffer,&u,&*uav));
        };
        if(!donors_ && (!structured(kSiblingDraws*3,D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_UNORDERED_ACCESS,donors_) ||
                        !views(donors_.Get(),kSiblingDraws*3,donorsSrv_,&donorsUav_))){donors_.Reset();return false;}
        if(!receivers_ && (!structured(kSiblingDraws,D3D11_BIND_SHADER_RESOURCE,receivers_) ||
                           !views(receivers_.Get(),kSiblingDraws,receiversSrv_,nullptr))){receivers_.Reset();return false;}
        if(!fit_ && (!structured(kSiblingDraws*2,D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_UNORDERED_ACCESS,fit_) ||
                     !views(fit_.Get(),kSiblingDraws*2,fitSrv_,&fitUav_))){fit_.Reset();return false;}
        if(!fitSettings_){D3D11_BUFFER_DESC d{};d.ByteWidth=sizeof(FitSettings);d.Usage=D3D11_USAGE_DEFAULT;d.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
            if(FAILED(dev->CreateBuffer(&d,nullptr,&fitSettings_)))return false;}
        return true;
    }
    // The pass, once a frame and only when a draw needs it: a draw with no prior (the CPU knows), or any draw for a while after the identity
    // sampler saw one that had priors and no match (only the GPU knows). The first shader runs once for each draw that has priors; the
    // second once over every draw. Returns whether the fit table is the frame's.
    bool runSibling(ID3D11DeviceContext* ctx,unsigned width,unsigned height,float commonNear,unsigned frame) {
        const unsigned n=unsigned(current_.size());
        if(!n || n>kSiblingDraws)return false;
        if(!siblingPassEnabled())return false;
        if(siblingFrame_==frame && siblingDraws_==n)return siblingOk_;
        siblingFrame_=frame;siblingDraws_=n;siblingOk_=false;
        bool receiver=frame<=siblingArmedUntil_;
        for(const auto& d:current_)if(d.inputs.gpuIdentity && d.priorCount==0)receiver=true;
        if(!receiver)return false;
        if(!createSibling(ctx)){++stats_.siblingFailed;return false;}
        SiblingSlot* slot=nullptr;
        for(SiblingSlot& s:siblingSlots_)if(!s.pending){slot=&s;break;}
        if(slot && !slot->stage) {
            Ptr<ID3D11Device> dev;ctx->GetDevice(&dev);
            D3D11_BUFFER_DESC d{};d.ByteWidth=kSiblingDraws*2*16;d.Usage=D3D11_USAGE_STAGING;d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
            d.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;d.StructureByteStride=16;
            if(FAILED(dev->CreateBuffer(&d,nullptr,&slot->stage)))slot=nullptr;
        }
        CsStageSave saved;saved.save(ctx);
        const UINT zero[4]{};ctx->ClearUnorderedAccessViewUint(donorsUav_.Get(),zero);
        ctx->CSSetShader(donorCs_.Get(),nullptr,0);
        ctx->CSSetConstantBuffers(0,1,settings_.GetAddressOf());
        ID3D11UnorderedAccessView* uav=donorsUav_.Get();ctx->CSSetUnorderedAccessViews(0,1,&uav,nullptr);
        unsigned dispatches=0;
        for(unsigned i=0;i<n;++i) {
            const Draw& d=current_[i];
            if(d.capture.currentIdentity) {
                Ptr<ID3D11Resource> identity;d.capture.currentIdentity->GetResource(&identity);
                const D3D11_BOX box{0,0,0,16,1,1};
                if(identity)ctx->CopySubresourceRegion(receivers_.Get(),0,i*16,0,0,identity.Get(),0,&box);
            }
            if(!d.inputs.gpuIdentity || !d.priorCount)continue;
            Settings c=settingsFor(d,width,height,commonNear);
            c.sibling[1]=i;c.sibling[2]=d.capture.geometry.count;
            ctx->UpdateSubresource(settings_.Get(),0,nullptr,&c,0,0);
            ID3D11ShaderResourceView* views[11]{};
            views[0]=d.capture.currentPositions.Get();
            for(unsigned k=0;k<d.priorCount;++k){views[1+k]=d.priors[k].positions.Get();views[6+k]=d.priors[k].identity.Get();}
            views[5]=d.capture.currentIdentity.Get();views[10]=d.capture.instanceIndex.Get();
            ctx->CSSetShaderResources(0,11,views);
            ctx->Dispatch(1,1,1);++dispatches;
        }
        ID3D11ShaderResourceView* noViews[11]{};ctx->CSSetShaderResources(0,11,noViews);
        ID3D11UnorderedAccessView* noUav=nullptr;ctx->CSSetUnorderedAccessViews(0,1,&noUav,nullptr);
        FitSettings limits{};limits.counts[0]=n;limits.limits[0]=kFlatSiblingSpreadPixels;limits.limits[1]=float(kFlatSiblingMinVertices);
        ctx->UpdateSubresource(fitSettings_.Get(),0,nullptr,&limits,0,0);
        ctx->CSSetShader(fitCs_.Get(),nullptr,0);
        ctx->CSSetConstantBuffers(0,1,fitSettings_.GetAddressOf());
        ID3D11ShaderResourceView* inputs[2]={donorsSrv_.Get(),receiversSrv_.Get()};ctx->CSSetShaderResources(0,2,inputs);
        uav=fitUav_.Get();ctx->CSSetUnorderedAccessViews(0,1,&uav,nullptr);
        ctx->Dispatch((n+63)/64,1,1);++dispatches;
        ctx->CSSetUnorderedAccessViews(0,1,&noUav,nullptr);ctx->CSSetShaderResources(0,2,noViews);
        saved.restore(ctx);
        stats_.siblingDispatches+=dispatches;++stats_.siblingFrames;
        if(slot) {
            // Each draw filed under what the history gave it: the pattern for a draw with no prior, the draw's own candidates for one the adapter
            // passed none of, and identity-differs for one with priors (it counts only if the GPU found no match).
            for(unsigned i=0;i<n;++i) {
                const Draw& d=current_[i];
                slot->pattern[i]=uint8_t(d.priorCount?kSiblingIdentityDiffers:d.capture.missed?unsigned(d.capture.miss.gap):kSiblingPriorFiltered);
            }
            const D3D11_BOX box{0,0,0,n*2*16,1,1};
            ctx->CopySubresourceRegion(slot->stage.Get(),0,0,0,0,fit_.Get(),0,&box);
            slot->pending=true;slot->frame=frame;slot->draws=n;
        }
        siblingOk_=true;return true;
    }
    // Section 104's frame counters and examples: the draws this frame submitted and the ones among them that missed, the run of
    // consecutive missing frames, the examples not yet handed over, and the sampled identity readback.
    unsigned frameSubmitted_=0,frameMissed_=0,missRun_=0,longestRun_=0,lastMissFrame_=0,polledFrame_=~0u;
    MissExample missExamples_[kMissExamples];unsigned missExampleCount_=0;
    FlatIdentitySampler::Sample identityExamples_[kIdentityExamples];unsigned identityExampleCount_=0;
    FlatIdentitySampler sampler_;
    Ptr<ID3D11ComputeShader> donorCs_,fitCs_;
    Ptr<ID3D11Buffer> donors_,receivers_,fit_,fitSettings_;
    Ptr<ID3D11ShaderResourceView> donorsSrv_,receiversSrv_,fitSrv_;
    Ptr<ID3D11UnorderedAccessView> donorsUav_,fitUav_;
    std::array<SiblingSlot,kSiblingSlots> siblingSlots_{};
    unsigned siblingFrame_=~0u,siblingDraws_=~0u,siblingArmedUntil_=0;bool siblingOk_=false;
    AnimatedVertexHistory history_;std::vector<Draw> current_,previous_;unsigned frame_=0,width_=0,height_=0;
    const char* refusal_=nullptr,*drawRefusal_=nullptr;float previousNear_=0;unsigned covered_=0;bool gpuAttempted_=false;
    Ptr<ID3D11VertexShader> vs_;Ptr<ID3D11PixelShader> ps_;Ptr<ID3D11Buffer> settings_;
    Ptr<ID3D11BlendState> blend_;Ptr<ID3D11DepthStencilState> depth_;
    Ptr<ID3D11Texture2D> texture_;Ptr<ID3D11RenderTargetView> target_;Ptr<ID3D11ShaderResourceView> view_;
};
} // namespace edvr
