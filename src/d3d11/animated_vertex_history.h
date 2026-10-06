#pragma once
#include <d3d11.h>
#include <wrl/client.h>
#include <algorithm>
#include <cstdint>
#include <vector>
#include "panel_curve.h"
#include "shader_swap.h"
#include "temporal_shader_bytecode.h"

namespace edvr {
// Actual original-VS outputs, independent of world-source naming and of the
// consumer's depth/stencil ownership policy. No CPU pose or motion estimate.
class AnimatedVertexHistory {
    template<class T> using Ptr = Microsoft::WRL::ComPtr<T>;
public:
    static constexpr unsigned maxVertices = 131072, maxRecords = 64;
    static constexpr unsigned maxBytes = 32 * 1024 * 1024;
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
        Ptr<ID3D11ShaderResourceView> currentPositions, currentIdentity;
        Ptr<ID3D11ShaderResourceView> previousPositions[4], previousIdentity[4];
        const char* refusal=nullptr;
    private:
        friend class AnimatedVertexHistory;
        Ptr<ID3D11GeometryShader> streamOutput;
        Ptr<ID3D11Buffer> positionBuffer;
        Ptr<ID3D11UnorderedAccessView> identityUav;
        D3D11_PRIMITIVE_TOPOLOGY topology=D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
        unsigned recordIndex=~0u;
        uint64_t recordEpoch=0;
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
                         unsigned start,int base,unsigned startInstance,unsigned frame,Capture& out) {
        out=Capture{};out.frame=frame;
        auto refuse=[&](const char* why){out.refusal=why;return false;};
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
        unsigned occurrences=0;
        for(const auto& r:records_)if(matches(r.geometry,key)) {
            if(r.frame[next]==frame)++occurrences;
            if(r.frame[previous]!=~0u && r.frame[previous]+1==frame) {
                if(out.candidateCount==4)return refuse("occurrence-cap");
                out.previousPositions[out.candidateCount]=r.views[previous];
                out.previousIdentity[out.candidateCount++]=r.identityViews[previous];
            }
        }
        if(occurrences==4)return refuse("occurrence-cap");
        auto found=std::find_if(records_.begin(),records_.end(),[&](const Record& r){return matches(r.geometry,key) && r.frame[next]!=frame;});
        Ptr<ID3D11Device> dev;ctx->GetDevice(&dev);
        if(found==records_.end()) {
            const uint64_t requested=uint64_t(count)*32;
            auto overBudget=[&]{return records_.size()>=maxRecords || uint64_t(bytes_)+requested>maxBytes;};
            Ptr<ID3D11GeometryShader> reclaimedCapture;
            if(overBudget()) {
                // Known writes invalidate correspondence immediately, but
                // keep the allocation for cheap same-key reuse. Reclaim only
                // when a different key actually needs its budget.
                size_t invalidCount=0;uint64_t invalidBytes=0;
                for(const auto& r:records_)if(r.invalidated) {
                    ++invalidCount;invalidBytes+=uint64_t(r.geometry.count)*32;
                }
                if(records_.size()-invalidCount>=maxRecords ||
                   uint64_t(bytes_)-invalidBytes+requested>maxBytes)return refuse("history-budget");
                for(auto it=records_.begin();it!=records_.end() && overBudget();) {
                    if(!it->invalidated){++it;continue;}
                    if(!reclaimedCapture && it->geometry.original==key.original)
                        reclaimedCapture=it->capture;
                    const unsigned released=it->geometry.count*32;
                    bytes_-=released;++reclaimedRecords_;reclaimedBytes_+=released;
                    it=records_.erase(it);
                }
            }
            if(overBudget())return refuse("history-budget");
            Record record;record.geometry=key;record.capture=std::move(reclaimedCapture);
            if(!allocate(dev.Get(),record)){failed_=true;return refuse("resource-creation");}
            bytes_+=count*32;records_.push_back(std::move(record));found=records_.end()-1;
        }
        found->invalidated=false;
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
            return;
        }
        // Pressure reclamation may shift a different record while this
        // Capture owns its views. Never publish an invalidated/erased record.
        auto found=std::find_if(records_.begin(),records_.end(),[&](const Record& r){
            return !r.invalidated && r.mutationEpoch==out.recordEpoch &&
                r.views[parity]==out.currentPositions;});
        if(found!=records_.end())found->frame[parity]=out.frame;
    }
    void submitPositions(ID3D11DeviceContext* ctx,PanelCurveDrawFn draw,unsigned startInstance,const Capture& out) {
        bindPositions(ctx,out);drawPositions(ctx,draw,startInstance,out);restorePositions(ctx,out);
    }
    bool capture(ID3D11DeviceContext* ctx,PanelCurveDrawFn draw,unsigned count,unsigned instances,
                  unsigned start,int base,unsigned startInstance,unsigned frame,Capture& out,bool retainIndex=false) {
        if(!draw){out=Capture{};out.refusal="missing-draw";return false;}
        if(!prepareCapture(ctx,count,instances,start,base,startInstance,frame,out))return false;
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
            reasons|=!resource?1:resource==r.geometry.vertices.Get()?2:4;
            r.frame[0]=r.frame[1]=~0u;r.invalidated=true;++r.mutationEpoch;
        }
        return reasons;
    }
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
    };
    static bool matches(const Geometry& a,const Geometry& b) {
        return a.original==b.original && a.layout==b.layout && a.vertices==b.vertices && a.indices==b.indices &&
               a.count==b.count && a.start==b.start && a.base==b.base && a.offset==b.offset && a.stride==b.stride &&
               a.format==b.format && a.indexOffset==b.indexOffset;
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
    Ptr<ID3D11ComputeShader> identify_;Ptr<ID3D11Buffer> instance_;Ptr<ID3D11ShaderResourceView> instanceView_;
    unsigned bytes_=0,reclaimedRecords_=0,reclaimedBytes_=0;bool failed_=false;
};
} // namespace edvr
