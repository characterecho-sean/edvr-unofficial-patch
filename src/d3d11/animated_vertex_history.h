#pragma once
#include <d3d11.h>
#include <wrl/client.h>
#include <algorithm>
#include <cstdint>
#include <vector>
#include "animated_history_ledger.h"
#include "panel_curve.h"
#include "shader_swap.h"
#include "temporal_shader_bytecode.h"

namespace edvr {
// Actual original-VS outputs, independent of world-source naming and of the
// consumer's depth/stencil ownership policy. No CPU pose or motion estimate.
class AnimatedVertexHistory {
    template<class T> using Ptr = Microsoft::WRL::ComPtr<T>;
public:
    // maxRecords bounds the live records (one per geometry key and same-frame occurrence). One equip frame in the 2026-10-06 flights reached
    // the old bound of 64 (the budget receipt's records=64); 128 gives that transient room. The byte bound below is what limits large draws.
    static constexpr unsigned maxVertices = 131072, maxRecords = 128;
    static constexpr unsigned maxBytes = 32 * 1024 * 1024;
    // How many records of one geometry key a frame may use. The default policy (VR: its map holds four priors) refuses the fifth. The
    // extended policy (the flat adapter, design section 104) allows maxExtendedOccurrences and hands each draw the window of four priors
    // nearest its own ordinal, so a mesh drawn dozens of times in a frame (the grenade's identical pieces) keeps a history per piece.
    static constexpr unsigned maxOccurrences = 4, maxExtendedOccurrences = 64;
    inline static const GUID bytecodeKey = {0x65a40e9c,0xa4ee,0x473d,{0x85,0x4a,0xeb,0x10,0x35,0x8e,0x4f,0x20}};
    struct Geometry {
        Ptr<ID3D11VertexShader> original;
        Ptr<ID3D11InputLayout> layout;
        Ptr<ID3D11Buffer> vertices, indices;
        unsigned count=0, start=0, offset=0, stride=0, indexOffset=0;
        int base=0;
        DXGI_FORMAT format=DXGI_FORMAT_UNKNOWN;
    };
    // Owns every resource reference. The position/identity planes are the
    // bounded history's ping-pong buffers, valid until that parity is reused;
    // an adapter must consume them in the captured frame, not retain snapshots.
    // Instance index is GPU-only: instanceByteOffset identifies the exact
    // original four-byte index copied for the identity dispatch. Identity xy
    // is t33 row[0].x/row[1].w; w remains zero, preserving the VR contract.
    struct Capture {
        Geometry geometry;
        Ptr<ID3D11Buffer> instanceBuffer;
        Ptr<ID3D11ShaderResourceView> pool;
        // Owned view of the existing four-byte GPU index scratch copy. Consume
        // immediately after this submission; the next identity submission
        // rewrites its contents. No additional copy or CPU readback is made.
        Ptr<ID3D11ShaderResourceView> instanceIndex;
        unsigned instanceByteOffset=0, frame=0, candidateCount=0, retainedIndexBytes=0;
        // How many records of this geometry key this frame had already used when this draw prepared: 0 for the first draw of the key,
        // 1 for the second, and so on. A draw's own occurrence number is occurrences+1.
        unsigned occurrences=0;
        // How many records of this key the frame before used, whatever the four candidates below are: above four, the extended policy
        // handed this draw the window of four nearest its ordinal (candidateCount stays at most four).
        unsigned priorRecords=0;
        Ptr<ID3D11ShaderResourceView> currentPositions, currentIdentity;
        Ptr<ID3D11ShaderResourceView> previousPositions[4], previousIdentity[4];
        const char* refusal=nullptr;
        // Section 104, the pistol's no-candidate bursts. Filled under the extended policy only (the flat adapter; VR is as it was). `key` is
        // the draw's geometry key as the ledger sees it. `missed` is set when no record of that exact key was a prior the frame before
        // used, and `miss` says why (animated_history_ledger.h). `acrossOffset` is set when the draw's one candidate is a record of the same
        // mesh at another place in the same buffers (HistoryGap::OffsetShift): the GPU identity decides whether it is the draw's own.
        bool missed=false, acrossOffset=false;
        HistoryClass miss;
        HistoryKey key;
    private:
        friend class AnimatedVertexHistory;
        Ptr<ID3D11GeometryShader> streamOutput;
        Ptr<ID3D11Buffer> positionBuffer;
        Ptr<ID3D11UnorderedAccessView> identityUav;
        D3D11_PRIMITIVE_TOPOLOGY topology=D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
        unsigned recordIndex=~0u;
        uint64_t recordEpoch=0;
        uint64_t ledgerToken=0;
    };
    struct Usage {
        unsigned recordCount=0,bytes=0;
        unsigned invalid=0,pending=0,current=0,prior=0,older=0;
        unsigned reclaimedRecords=0,reclaimedBytes=0;
    };

    static void rememberShader(ID3D11VertexShader* shader,const void* bytes,size_t size) {
        if(shader && bytes && size && size<=65536)
            shader->SetPrivateData(bytecodeKey,UINT(size),bytes);
    }
    bool initializeIdentity(ID3D11DeviceContext* ctx,ID3D11Device* dev,
                            const char* role="animated vertex identity",const char* owner="animated vertex history") {
        if(instanceView_)return true;
        identify_.Attach(shaderSwapCreateCs(ctx,kWeaponIdentityBytecode,sizeof(kWeaponIdentityBytecode),role,owner));
        if(!identify_)return failed_=true,false;
        D3D11_BUFFER_DESC d{};d.ByteWidth=16;d.BindFlags=D3D11_BIND_SHADER_RESOURCE;
        D3D11_SHADER_RESOURCE_VIEW_DESC v{};v.Format=DXGI_FORMAT_R32_UINT;
        v.ViewDimension=D3D11_SRV_DIMENSION_BUFFER;v.Buffer.NumElements=4;
        if(FAILED(dev->CreateBuffer(&d,nullptr,&instance_)) ||
           FAILED(dev->CreateShaderResourceView(instance_.Get(),&v,&instanceView_)))return failed_=true,false;
        return true;
    }

    // Split preparation/submission lets VR retain identity -> map clear -> SO
    // order and its existing exact stage timers. Flat may use capture() below.
    bool prepareCapture(ID3D11DeviceContext* ctx,unsigned count,unsigned instances,
                         unsigned start,int base,unsigned startInstance,unsigned frame,Capture& out,bool extended=false) {
        out=Capture{};out.frame=frame;
        // A refusal before the key is built is the ledger's "offered, no key" (section 104); one after it is the key's refusal, noted with it.
        auto refuse=[&](const char* why){out.refusal=why;if(extended)ledger_.noteUnkeyed(frame);return false;};
        if(failed_ || !ctx || ctx->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE ||
           instances!=1 || !count || count%3 || count>maxVertices)return refuse("draw-shape");
        ctx->IAGetPrimitiveTopology(&out.topology);
        if(out.topology!=D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST)return refuse("topology");
        Ptr<ID3D11GeometryShader> gs;Ptr<ID3D11HullShader> hs;Ptr<ID3D11DomainShader> dom;
        ctx->GSGetShader(&gs,nullptr,nullptr);ctx->HSGetShader(&hs,nullptr,nullptr);ctx->DSGetShader(&dom,nullptr,nullptr);
        if(gs || hs || dom)return refuse("tessellation-or-geometry-shader");
        Ptr<ID3D11Predicate> predicate;BOOL pred=FALSE;ctx->GetPredication(&predicate,&pred);
        if(predicate)return refuse("predication");
        ID3D11Buffer* targets[4]{};ctx->SOGetTargets(4,targets);bool busy=false;
        for(auto* p:targets)if(p){busy=true;p->Release();}if(busy)return refuse("stream-output-bound");
        ID3D11UnorderedAccessView* uavs[8]{};
        ctx->OMGetRenderTargetsAndUnorderedAccessViews(0,nullptr,nullptr,0,8,uavs);
        for(auto* p:uavs)if(p){busy=true;p->Release();}if(busy)return refuse("uav-bound");
        Geometry& key=out.geometry;
        ctx->VSGetShader(&key.original,nullptr,nullptr);ctx->IAGetInputLayout(&key.layout);
        ctx->IAGetVertexBuffers(1,1,&key.vertices,&key.stride,&key.offset);
        ctx->IAGetIndexBuffer(&key.indices,&key.format,&key.indexOffset);
        if(!key.original || !key.layout || !key.vertices || !key.indices)return refuse("geometry-binding");
        UINT codeSize=0;
        if(FAILED(key.original->GetPrivateData(bytecodeKey,&codeSize,nullptr)) || !codeSize)return refuse("original-bytecode-absent");
        UINT instanceStride=0,instanceOffset=0;
        ctx->IAGetVertexBuffers(0,1,&out.instanceBuffer,&instanceStride,&instanceOffset);
        ctx->VSGetShaderResources(33,1,&out.pool);
        if(!out.instanceBuffer || !out.pool || instanceStride!=8)return refuse("identity-binding");
        D3D11_BUFFER_DESC id{};out.instanceBuffer->GetDesc(&id);
        const uint64_t address=uint64_t(instanceOffset)+uint64_t(startInstance)*8;
        D3D11_SHADER_RESOURCE_VIEW_DESC pd{};out.pool->GetDesc(&pd);
        Ptr<ID3D11Resource> pr;out.pool->GetResource(&pr);Ptr<ID3D11Buffer> pb;
        if(address%4 || address+4>id.ByteWidth || pd.ViewDimension!=D3D11_SRV_DIMENSION_BUFFER ||
           pd.Buffer.FirstElement || FAILED(pr.As(&pb)))return refuse("identity-view");
        D3D11_BUFFER_DESC bd{};pb->GetDesc(&bd);
        if(bd.StructureByteStride!=336 || pd.Buffer.NumElements!=bd.ByteWidth/336)return refuse("pool-layout");
        out.instanceByteOffset=UINT(address);key.count=count;key.start=start;key.base=base;
        const unsigned next=frame&1,previous=1-next;
        const HistoryKey ledgerKey=historyKeyOf(key);
        out.key=ledgerKey;
        auto refuseKeyed=[&](const char* why,HistoryLedger::Outcome outcome){
            out.refusal=why;if(extended)ledger_.note(frame,ledgerKey,outcome);return false;};
        // The records of this key that the frame before used, in record order: with at most four, all of them are the candidates (the
        // GPU map picks the draw's own by its identity). With more (the extended policy only), the draw's own previous record is the one
        // at its own ordinal, since a steady frame draws the key's draws in the same order and the k-th takes the k-th record: the
        // candidates are the four nearest that ordinal, k-1 .. k+2, and the identity still decides among them. A draw whose prior is
        // outside the window finds none of its identity there and is refused locally by the map (no-prior or identity-differs).
        const unsigned limit=extended?maxExtendedOccurrences:maxOccurrences;
        unsigned prior[maxExtendedOccurrences];unsigned priorCount=0,occurrences=0;
        HistoryRecordFacts facts;unsigned shiftIndex=~0u;
        for(size_t i=0;i<records_.size();++i) {
            const Record& r=records_[i];
            if(!matches(r.geometry,key)) {
                // The same mesh drawn from another place in the same buffers, as a prior the frame before used (section 104).
                if(extended && sameMeshElsewhere(r.geometry,key) && usableAsPrior(r,frame,previous)) {
                    ++facts.shiftUsable;
                    if(r.frame[next]==frame || r.claimFrame==frame)++facts.shiftClaimed;
                    shiftIndex=unsigned(i);
                }
                continue;
            }
            facts.exactPresent=true;
            if(r.invalidated){facts.exactInvalidated=true;facts.exactInvalidReasons|=r.invalidReasons;}
            if(r.frame[next]==frame)++occurrences;
            if(r.frame[previous]!=~0u && r.frame[previous]+1==frame) {
                if(priorCount==limit)return refuseKeyed("occurrence-cap",HistoryLedger::RefusedOccurrence);
                prior[priorCount++]=unsigned(i);
            }
        }
        out.occurrences=occurrences;out.priorRecords=priorCount;
        if(occurrences>=limit)return refuseKeyed("occurrence-cap",HistoryLedger::RefusedOccurrence);
        if(extended && priorCount==0) {
            // No record of this exact key was a prior. The ledger says why. One case is rescued: the frame before drew this very mesh from
            // another place in the same buffers (the game re-packed or ring-allocated it), through exactly one record no other draw has taken.
            // The record is handed over as the draw's one candidate and the GPU identity still decides whether it is the draw's own; every
            // other pattern stays as it was.
            out.missed=true;
            out.miss=ledger_.classify(frame,ledgerKey,facts);
            if(out.miss.gap==HistoryGap::OffsetShift && shiftIndex<records_.size()) {
                prior[priorCount++]=shiftIndex;out.priorRecords=priorCount;out.acrossOffset=true;
                records_[shiftIndex].claimFrame=frame;
            }
        }
        {
            const unsigned take=(std::min)(priorCount,4u);
            const unsigned first=priorCount>4?(std::min)(occurrences>0?occurrences-1:0u,priorCount-4):0u;
            for(unsigned i=0;i<take;++i) {
                const Record& r=records_[prior[first+i]];
                out.previousPositions[i]=r.views[previous];out.previousIdentity[i]=r.identityViews[previous];
            }
            out.candidateCount=take;
        }
        auto found=std::find_if(records_.begin(),records_.end(),[&](const Record& r){return matches(r.geometry,key) && r.frame[next]!=frame;});
        Ptr<ID3D11Device> dev;ctx->GetDevice(&dev);
        if(found==records_.end()) {
            const uint64_t requested=uint64_t(count)*32;
            auto overBudget=[&]{return records_.size()>=maxRecords || uint64_t(bytes_)+requested>maxBytes;};
            // Under the extended policy a record that no frame from now on can use as a prior is reclaimable too (spentForHistory): the
            // grenade flight's budget receipt (frame 39036) held 128 records, 122 of them last used two frames before and none this
            // frame, and refused the first draw of the frame for the record limit with 15 percent of the bytes in use.
            auto reclaimable=[&](const Record& r){return r.invalidated || (extended && spentForHistory(r,frame));};
            Ptr<ID3D11GeometryShader> reclaimedCapture;
            if(overBudget()) {
                // Known writes invalidate correspondence immediately, but
                // keep the allocation for cheap same-key reuse. Reclaim only
                // when a different key actually needs its budget.
                size_t invalidCount=0;uint64_t invalidBytes=0;
                for(const auto& r:records_)if(reclaimable(r)) {
                    ++invalidCount;invalidBytes+=uint64_t(r.geometry.count)*32;
                }
                if(records_.size()-invalidCount>=maxRecords ||
                   uint64_t(bytes_)-invalidBytes+requested>maxBytes)return refuseKeyed("history-budget",HistoryLedger::RefusedBudget);
                for(auto it=records_.begin();it!=records_.end() && overBudget();) {
                    if(!reclaimable(*it)){++it;continue;}
                    if(!reclaimedCapture && it->geometry.original==key.original)
                        reclaimedCapture=it->capture;
                    const unsigned released=it->geometry.count*32;
                    bytes_-=released;++reclaimedRecords_;reclaimedBytes_+=released;
                    it=records_.erase(it);
                }
            }
            if(overBudget())return refuseKeyed("history-budget",HistoryLedger::RefusedBudget);
            Record record;record.geometry=key;record.capture=std::move(reclaimedCapture);
            if(!allocate(dev.Get(),record)){failed_=true;return refuseKeyed("resource-creation",HistoryLedger::RefusedOther);}
            bytes_+=count*32;records_.push_back(std::move(record));found=records_.end()-1;
        }
        found->invalidated=false;found->invalidReasons=0;
        if(extended)out.ledgerToken=ledger_.note(frame,ledgerKey,HistoryLedger::Captured);
        out.recordEpoch=found->mutationEpoch;
        out.recordIndex=unsigned(found-records_.begin());out.streamOutput=found->capture;
        out.positionBuffer=found->positions[next];out.identityUav=found->identityUavs[next];
        out.currentPositions=found->views[next];out.currentIdentity=found->identityViews[next];
        return true;
    }
    void submitIdentity(ID3D11DeviceContext* ctx,Capture& out) {
        out.instanceIndex=instanceView_;
        D3D11_BOX box{out.instanceByteOffset,0,0,out.instanceByteOffset+4,1,1};
        ctx->CopySubresourceRegion(instance_.Get(),0,0,0,0,out.instanceBuffer.Get(),0,&box);
        Ptr<ID3D11ComputeShader> saved;ID3D11ClassInstance* classes[256]{};UINT count=256;
        ctx->CSGetShader(&saved,classes,&count);ID3D11ShaderResourceView* srvs[2]{};
        ctx->CSGetShaderResources(0,2,srvs);Ptr<ID3D11UnorderedAccessView> uav;ctx->CSGetUnorderedAccessViews(0,1,&uav);
        ID3D11ShaderResourceView* in[2]={instanceView_.Get(),out.pool.Get()};
        ctx->CSSetShaderResources(0,2,in);ID3D11UnorderedAccessView* target=out.identityUav.Get();
        ctx->CSSetUnorderedAccessViews(0,1,&target,nullptr);ctx->CSSetShader(identify_.Get(),nullptr,0);ctx->Dispatch(1,1,1);
        ID3D11ShaderResourceView* none[2]{};ID3D11UnorderedAccessView* noUav=nullptr;
        ctx->CSSetShaderResources(0,2,none);ctx->CSSetUnorderedAccessViews(0,1,&noUav,nullptr);
        ctx->CSSetShader(saved.Get(),classes,count);ctx->CSSetShaderResources(0,2,srvs);
        UINT keep=~0u;ctx->CSSetUnorderedAccessViews(0,1,uav.GetAddressOf(),&keep);
        for(auto* p:srvs)if(p)p->Release();for(UINT i=0;i<count;++i)classes[i]->Release();
    }
    void bindPositions(ID3D11DeviceContext* ctx,const Capture& out) {
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_POINTLIST);ctx->GSSetShader(out.streamOutput.Get(),nullptr,0);
    }
    void drawPositions(ID3D11DeviceContext* ctx,PanelCurveDrawFn draw,unsigned startInstance,const Capture& out) {
        UINT zero=0;ID3D11Buffer* target=out.positionBuffer.Get();
        ctx->SOSetTargets(1,&target,&zero);
        draw(ctx,out.geometry.count,1,out.geometry.start,out.geometry.base,startInstance);
    }
    void restorePositions(ID3D11DeviceContext* ctx,const Capture& out) {
        ctx->SOSetTargets(0,nullptr,nullptr);ctx->GSSetShader(nullptr,nullptr,0);ctx->IASetPrimitiveTopology(out.topology);
        const unsigned parity=out.frame&1;
        if(out.recordIndex<records_.size() && !records_[out.recordIndex].invalidated &&
           records_[out.recordIndex].mutationEpoch==out.recordEpoch &&
           records_[out.recordIndex].views[parity]==out.currentPositions) {
            records_[out.recordIndex].frame[out.frame&1]=out.frame;
            ledger_.markPublished(out.ledgerToken);
            return;
        }
        // Pressure reclamation may shift a different record while this
        // Capture owns its views. Never publish an invalidated/erased record.
        auto found=std::find_if(records_.begin(),records_.end(),[&](const Record& r){
            return !r.invalidated && r.mutationEpoch==out.recordEpoch &&
                r.views[parity]==out.currentPositions;});
        if(found!=records_.end()){found->frame[parity]=out.frame;ledger_.markPublished(out.ledgerToken);}
    }
    void submitPositions(ID3D11DeviceContext* ctx,PanelCurveDrawFn draw,unsigned startInstance,const Capture& out) {
        bindPositions(ctx,out);drawPositions(ctx,draw,startInstance,out);restorePositions(ctx,out);
    }
    bool capture(ID3D11DeviceContext* ctx,PanelCurveDrawFn draw,unsigned count,unsigned instances,
                  unsigned start,int base,unsigned startInstance,unsigned frame,Capture& out,bool retainIndex=false,bool extended=false) {
        if(!draw){out=Capture{};out.refusal="missing-draw";return false;}
        if(!prepareCapture(ctx,count,instances,start,base,startInstance,frame,out,extended))return false;
        Ptr<ID3D11Device> dev;ctx->GetDevice(&dev);
        if(!initializeIdentity(ctx,dev.Get())){out.refusal="resource-creation";return false;}
        submitIdentity(ctx,out);submitPositions(ctx,draw,startInstance,out);
        if(retainIndex && !retainInstanceIndex(ctx,out)){out.refusal="index-snapshot-creation";return false;}
        return true;
    }
    // One instance per admitted draw: a single four-byte index describes every
    // emitted vertex. Deferred flat raster needs one owned scalar snapshot,
    // not a replicated per-vertex index plane. VR never requests this copy.
    bool retainInstanceIndex(ID3D11DeviceContext* ctx,Capture& out) {
        if(!out.instanceIndex)return false;
        Ptr<ID3D11Device> dev;ctx->GetDevice(&dev);Ptr<ID3D11Buffer> snapshot;
        D3D11_BUFFER_DESC d{};d.ByteWidth=4;d.BindFlags=D3D11_BIND_SHADER_RESOURCE;
        D3D11_SHADER_RESOURCE_VIEW_DESC v{};v.Format=DXGI_FORMAT_R32_UINT;
        v.ViewDimension=D3D11_SRV_DIMENSION_BUFFER;v.Buffer.NumElements=1;
        Ptr<ID3D11ShaderResourceView> view;
        if(FAILED(dev->CreateBuffer(&d,nullptr,&snapshot)) ||
           FAILED(dev->CreateShaderResourceView(snapshot.Get(),&v,&view)))return false;
        Ptr<ID3D11Resource> source;out.instanceIndex->GetResource(&source);
        D3D11_BOX box{0,0,0,4,1,1};ctx->CopySubresourceRegion(snapshot.Get(),0,0,0,0,source.Get(),0,&box);
        out.instanceIndex=std::move(view);out.retainedIndexBytes=4;return true;
    }
    void advance(unsigned frame) {
        reclaimedRecords_=reclaimedBytes_=0;
        for(auto it=records_.begin();it!=records_.end();)
            if((it->frame[0]==~0u || frame-it->frame[0]>2) && (it->frame[1]==~0u || frame-it->frame[1]>2)) {
                bytes_-=it->geometry.count*32;it=records_.erase(it);
            } else ++it;
    }
    // Returns the adapter's existing diagnostic reason bits: unknown / VB / IB.
    unsigned resourceWritten(ID3D11Resource* resource) {
        unsigned reasons=0;
        for(auto& r:records_)if(!resource || resource==r.geometry.vertices.Get() || resource==r.geometry.indices.Get()) {
            const unsigned why=!resource?1:resource==r.geometry.vertices.Get()?2:4;
            reasons|=why;
            r.frame[0]=r.frame[1]=~0u;r.invalidated=true;r.invalidReasons|=why;++r.mutationEpoch;
        }
        return reasons;
    }
    // Whether an offset-shift rescue still stands at the end of the frame (section 104). A mesh that moved vacates its old place; if another
    // draw has used the donor record this frame, the old place is drawn again and nothing moved: the two draws are siblings (two parts of one
    // object, equal in count and buffers) and the donor's positions are not this draw's. Asked when all of the frame's draws are in (the
    // adapter's prepareH), because the sibling may come after the rescue. A capture that was not rescued always holds.
    bool rescueHolds(const Capture& c,unsigned frame) const {
        if(!c.acrossOffset)return true;
        const unsigned next=frame&1,previous=1-next;
        for(const auto& r:records_)
            if(r.views[previous]==c.previousPositions[0] && r.frame[next]==frame)return false;
        return true;
    }
    // A draw the adapter turned away before the history was asked (its preflight): the ledger remembers that the frame was offered it.
    void noteNotOffered(unsigned frame) {ledger_.noteUnkeyed(frame);}
    const HistoryLedger& ledger() const{return ledger_;}
    bool failed() const{return failed_;}
    unsigned bytes() const{return bytes_;}
    size_t recordCount() const{return records_.size();}
    Usage accounting(unsigned frame) const {
        Usage u{};u.recordCount=unsigned(records_.size());u.bytes=bytes_;
        u.reclaimedRecords=reclaimedRecords_;u.reclaimedBytes=reclaimedBytes_;
        for(const auto& r:records_) {
            if(r.invalidated){++u.invalid;continue;}
            unsigned age=~0u;
            for(unsigned parity=0;parity<2;++parity)if(r.frame[parity]!=~0u)
                age=(std::min)(age,frame-r.frame[parity]);
            if(age==~0u)++u.pending;
            else if(age==0)++u.current;
            else if(age==1)++u.prior;
            else ++u.older;
        }
        return u;
    }
private:
    struct Record {
        Geometry geometry;Ptr<ID3D11Buffer> positions[2],identity[2];
        Ptr<ID3D11ShaderResourceView> views[2],identityViews[2];
        Ptr<ID3D11UnorderedAccessView> identityUavs[2];Ptr<ID3D11GeometryShader> capture;
        unsigned frame[2]={~0u,~0u};
        uint64_t mutationEpoch=0;bool invalidated=false;
        unsigned invalidReasons=0;   // the writes that invalidated it (resourceWritten's bits), until its key draws again
        unsigned claimFrame=~0u;     // the frame a draw of another key took it as its one candidate (the offset-shift rescue)
    };
    // A record no later frame can use as a prior: it has been used, none of its frames is this frame or the one before, so it is neither
    // this frame's nor a candidate (a prior is a record used the frame before). A pending record (never published) is not spent: a
    // prepared capture may still publish it.
    static bool spentForHistory(const Record& r,unsigned frame) {
        if(r.invalidated)return false;
        bool used=false;
        for(unsigned parity=0;parity<2;++parity) {
            if(r.frame[parity]==~0u)continue;
            used=true;
            if(frame-r.frame[parity]<2)return false;
        }
        return used;
    }
    static bool matches(const Geometry& a,const Geometry& b) {
        return a.original==b.original && a.layout==b.layout && a.vertices==b.vertices && a.indices==b.indices &&
               a.count==b.count && a.start==b.start && a.base==b.base && a.offset==b.offset && a.stride==b.stride &&
               a.format==b.format && a.indexOffset==b.indexOffset;
    }
    static HistoryKey historyKeyOf(const Geometry& g) {
        HistoryKey k;
        k.vs=g.original.Get();k.layout=g.layout.Get();k.vertices=g.vertices.Get();k.indices=g.indices.Get();
        k.count=g.count;k.start=g.start;k.offset=g.offset;k.stride=g.stride;k.indexOffset=g.indexOffset;
        k.format=unsigned(g.format);k.base=g.base;
        return k;
    }
    // The same mesh at another place: every field its vertex stream depends on equal (shader, layout, buffers, count, stride, index format)
    // and the placement (start, base, vertex-buffer offset, index-buffer offset) different. The offset-shift pattern's record test.
    static bool sameMeshElsewhere(const Geometry& a,const Geometry& b) {
        return a.original==b.original && a.layout==b.layout && a.vertices==b.vertices && a.indices==b.indices &&
               a.count==b.count && a.stride==b.stride && a.format==b.format &&
               (a.start!=b.start || a.base!=b.base || a.offset!=b.offset || a.indexOffset!=b.indexOffset);
    }
    // A prior the frame before used and nothing has invalidated since.
    static bool usableAsPrior(const Record& r,unsigned frame,unsigned previous) {
        return !r.invalidated && r.frame[previous]!=~0u && r.frame[previous]+1==frame;
    }
    bool allocate(ID3D11Device* dev,Record& r) {
        for(const auto& cached:records_)if(cached.geometry.original==r.geometry.original){r.capture=cached.capture;break;}
        if(!r.capture) {
            UINT size=0;
            if(FAILED(r.geometry.original->GetPrivateData(bytecodeKey,&size,nullptr)) || !size || size>65536)return false;
            std::vector<unsigned char> bytes(size);
            if(FAILED(r.geometry.original->GetPrivateData(bytecodeKey,&size,bytes.data())))return false;
            D3D11_SO_DECLARATION_ENTRY e{0,"SV_POSITION",0,0,4,0};UINT stride=16;
            if(FAILED(dev->CreateGeometryShaderWithStreamOutput(bytes.data(),size,&e,1,&stride,1,D3D11_SO_NO_RASTERIZED_STREAM,nullptr,&r.capture)))return false;
        }
        D3D11_BUFFER_DESC b{};b.ByteWidth=r.geometry.count*16;b.BindFlags=D3D11_BIND_STREAM_OUTPUT|D3D11_BIND_SHADER_RESOURCE;
        D3D11_SHADER_RESOURCE_VIEW_DESC s{};s.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;
        s.ViewDimension=D3D11_SRV_DIMENSION_BUFFER;s.Buffer.NumElements=r.geometry.count;
        for(int i=0;i<2;++i)if(FAILED(dev->CreateBuffer(&b,nullptr,&r.positions[i])) ||
                              FAILED(dev->CreateShaderResourceView(r.positions[i].Get(),&s,&r.views[i])))return false;
        b.ByteWidth=16;b.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_UNORDERED_ACCESS;
        b.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;b.StructureByteStride=16;
        for(int i=0;i<2;++i)if(FAILED(dev->CreateBuffer(&b,nullptr,&r.identity[i])) ||
                              FAILED(dev->CreateShaderResourceView(r.identity[i].Get(),nullptr,&r.identityViews[i])) ||
                              FAILED(dev->CreateUnorderedAccessView(r.identity[i].Get(),nullptr,&r.identityUavs[i])))return false;
        return true;
    }
    std::vector<Record> records_;
    HistoryLedger ledger_;
    Ptr<ID3D11ComputeShader> identify_;Ptr<ID3D11Buffer> instance_;Ptr<ID3D11ShaderResourceView> instanceView_;
    unsigned bytes_=0,reclaimedRecords_=0,reclaimedBytes_=0;bool failed_=false;
};
} // namespace edvr
