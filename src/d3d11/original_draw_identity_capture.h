#pragma once

#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <array>
#include <cstdint>
#include <cstring>

namespace edvr {

constexpr uint32_t kOriginalDrawIdentityMaxInstances = 32;
constexpr uint32_t kOriginalDrawIdentityModelBytes = 336;

enum class OriginalDrawIdentityReason : uint8_t {
    None,
    Pending,
    InvalidArgument,
    UnsupportedContext,
    IneligibleLayout,
    MissingInstanceBuffer,
    InstanceLayoutMismatch,
    InstanceRangeOutOfBounds,
    MissingModelPool,
    ModelPoolLayoutMismatch,
    ResourceCreationFailed,
    DispatchFailed
};

enum class OriginalDrawIdentityPoll : uint8_t { Ready, Pending, Unavailable };

struct OriginalDrawIdentityRecord {
    uint32_t instanceAndModelDataIndex[2]{};
    std::array<uint8_t, kOriginalDrawIdentityModelBytes> modelRecord{};
    bool valid = false;
    // A nonzero bone base means t33 is not the complete skinned state: the
    // bone content in VS t38 is deliberately outside this bounded capture.
    bool skinned = false;
};

struct OriginalDrawIdentitySnapshot {
    uint32_t count = 0;
    uint32_t startInstance = 0;
    std::array<OriginalDrawIdentityRecord, kOriginalDrawIdentityMaxInstances> records{};
};

inline const GUID& originalDrawIdentityLayoutKey() noexcept {
    static const GUID key = {0x63f369a5,0xccd7,0x4ad3,{0xb0,0x67,0x18,0xda,0xca,0x71,0x56,0x33}};
    return key;
}

// Called from CreateInputLayout while the descriptors still exist. D3D11
// exposes no descriptor query on ID3D11InputLayout, so a private marker is the
// only selected-draw validation available later. Layouts can be created for a
// different compatible shader and reused by a selected family, so vsHash is
// intentionally not an eligibility condition; the draw selector owns that
// independent exact-shader check.
inline void originalDrawIdentityRememberLayout(ID3D11InputLayout* layout,
    const D3D11_INPUT_ELEMENT_DESC* elements, UINT count, uint64_t vsHash) noexcept {
    (void)vsHash;
    if (!layout || !elements) return;
    UINT running[16]{}; bool runningKnown[16]; for(bool& known:runningKnown)known=true;
    bool eligible = false;
    for (UINT i = 0; i < count; ++i) {
        const auto& e = elements[i];
        UINT bytes = 0;
        switch (e.Format) {
        case DXGI_FORMAT_R32G32_UINT: bytes = 8; break;
        case DXGI_FORMAT_R32G32B32A32_UINT:
        case DXGI_FORMAT_R32G32B32A32_FLOAT: bytes = 16; break;
        case DXGI_FORMAT_R32G32B32_FLOAT: bytes = 12; break;
        case DXGI_FORMAT_R32_UINT:
        case DXGI_FORMAT_R32_FLOAT: bytes = 4; break;
        default: break;
        }
        const UINT offset = e.AlignedByteOffset == D3D11_APPEND_ALIGNED_ELEMENT
            ? (e.InputSlot < 16 && runningKnown[e.InputSlot] ? running[e.InputSlot] : ~0u) : e.AlignedByteOffset;
        if (e.SemanticName && std::strcmp(e.SemanticName, "INSTANCEANDMODELDATAINDEX") == 0 &&
            e.SemanticIndex == 0 && e.Format == DXGI_FORMAT_R32G32_UINT &&
            e.InputSlot == 0 && offset == 0 &&
            e.InputSlotClass == D3D11_INPUT_PER_INSTANCE_DATA && e.InstanceDataStepRate == 1)
            eligible = true;
        if (e.InputSlot < 16) { if(bytes && offset!=~0u)running[e.InputSlot]=offset+bytes;else runningKnown[e.InputSlot]=false; }
    }
    if (eligible) { const uint32_t marker = 1; layout->SetPrivateData(originalDrawIdentityLayoutKey(), sizeof(marker), &marker); }
}

class OriginalDrawIdentityCaptureResources {
public:
    bool initialize(ID3D11Device* device) noexcept {
        if (!device) return false;
        if (shader_) return device_.Get() == device;
        static const char source[] = R"HLSL(
ByteAddressBuffer InstanceIds : register(t0);
struct Model { uint4 row[21]; };
StructuredBuffer<Model> Models : register(t1);
struct Result { uint4 header; uint4 model[21]; };
RWStructuredBuffer<Result> Results : register(u0);
cbuffer Params : register(b0) { uint Count; uint ModelCount; uint2 Padding; }
[numthreads(32,1,1)] void main(uint3 tid : SV_DispatchThreadID) {
    uint i=tid.x; if(i>=Count)return;
    uint2 ids=InstanceIds.Load2(i*8); Result r=(Result)0;
    r.header=uint4(ids,ids.x<ModelCount,0);
    if(ids.x<ModelCount) [unroll] for(uint j=0;j<21;++j) r.model[j]=Models[ids.x].row[j];
    Results[i]=r;
})HLSL";
        Microsoft::WRL::ComPtr<ID3DBlob> code, errors;
        if (FAILED(D3DCompile(source, sizeof(source)-1, "original-draw-identity", nullptr,
            nullptr, "main", "cs_5_0", D3DCOMPILE_ENABLE_STRICTNESS, 0, &code, &errors)) ||
            FAILED(device->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &shader_)))
            return false;
        D3D11_BUFFER_DESC cb{}; cb.ByteWidth=16; cb.Usage=D3D11_USAGE_DEFAULT; cb.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
        if (FAILED(device->CreateBuffer(&cb,nullptr,&params_))) { shader_.Reset(); return false; }
        device_=device; return true;
    }
private:
    friend class OriginalDrawIdentityCapture;
    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> shader_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> params_;
};

class OriginalDrawIdentityCapture {
    static constexpr UINT kGpuRecordBytes = 352;
public:
    bool capture(OriginalDrawIdentityCaptureResources& shared, ID3D11Device* device,
                 ID3D11DeviceContext* ctx, uint32_t instances, uint32_t startInstance) noexcept {
        if(pending_){reason_=OriginalDrawIdentityReason::Pending;return false;}
        count_=0; startInstance_=startInstance;
        if (!device || !ctx || !instances || instances>kOriginalDrawIdentityMaxInstances) return fail(OriginalDrawIdentityReason::InvalidArgument);
        if (ctx->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE) return fail(OriginalDrawIdentityReason::UnsupportedContext);
        Microsoft::WRL::ComPtr<ID3D11InputLayout> layout; ctx->IAGetInputLayout(&layout);
        uint32_t marker=0; UINT markerBytes=sizeof(marker);
        if (!layout || FAILED(layout->GetPrivateData(originalDrawIdentityLayoutKey(),&markerBytes,&marker)) || marker!=1)
            return fail(OriginalDrawIdentityReason::IneligibleLayout);

        Microsoft::WRL::ComPtr<ID3D11Buffer> instanceBuffer; UINT stride=0,offset=0;
        ctx->IAGetVertexBuffers(0,1,&instanceBuffer,&stride,&offset);
        if (!instanceBuffer) return fail(OriginalDrawIdentityReason::MissingInstanceBuffer);
        if (stride!=8 || (offset&3)) return fail(OriginalDrawIdentityReason::InstanceLayoutMismatch);
        D3D11_BUFFER_DESC ib{}; instanceBuffer->GetDesc(&ib);
        const uint64_t first=uint64_t(offset)+uint64_t(startInstance)*stride, bytes=uint64_t(instances)*8;
        if ((first&3) || first+bytes>ib.ByteWidth || first+bytes<first) return fail(OriginalDrawIdentityReason::InstanceRangeOutOfBounds);

        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> poolView; ctx->VSGetShaderResources(33,1,&poolView);
        if (!poolView) return fail(OriginalDrawIdentityReason::MissingModelPool);
        D3D11_SHADER_RESOURCE_VIEW_DESC sv{}; poolView->GetDesc(&sv);
        Microsoft::WRL::ComPtr<ID3D11Resource> poolResource; poolView->GetResource(&poolResource);
        Microsoft::WRL::ComPtr<ID3D11Buffer> pool; if(poolResource) poolResource.As(&pool);
        if (!pool || sv.ViewDimension!=D3D11_SRV_DIMENSION_BUFFER || sv.Format!=DXGI_FORMAT_UNKNOWN)
            return fail(OriginalDrawIdentityReason::ModelPoolLayoutMismatch);
        D3D11_BUFFER_DESC pb{}; pool->GetDesc(&pb);
        if (!(pb.MiscFlags&D3D11_RESOURCE_MISC_BUFFER_STRUCTURED) || pb.StructureByteStride!=kOriginalDrawIdentityModelBytes ||
            uint64_t(sv.Buffer.FirstElement)+sv.Buffer.NumElements>pb.ByteWidth/kOriginalDrawIdentityModelBytes)
            return fail(OriginalDrawIdentityReason::ModelPoolLayoutMismatch);
        if (!shared.initialize(device)) return fail(OriginalDrawIdentityReason::ResourceCreationFailed);
        if (!ensure(device)) return fail(OriginalDrawIdentityReason::ResourceCreationFailed);

        D3D11_BOX box{UINT(first),0,0,UINT(first+bytes),1,1};
        ctx->CopySubresourceRegion(instanceIds_.Get(),0,0,0,0,instanceBuffer.Get(),0,&box);
        const uint32_t params[4]={instances,sv.Buffer.NumElements,0,0}; ctx->UpdateSubresource(shared.params_.Get(),0,nullptr,params,0,0);

        ComputeState saved(ctx);
        ID3D11Buffer* cb=shared.params_.Get(); ID3D11ShaderResourceView* srvs[2]={instanceIdsView_.Get(),poolView.Get()};
        ID3D11UnorderedAccessView* uav=outputView_.Get(); UINT keep=~0u;
        ctx->CSSetShader(shared.shader_.Get(),nullptr,0); ctx->CSSetConstantBuffers(0,1,&cb);
        ctx->CSSetShaderResources(0,2,srvs); ctx->CSSetUnorderedAccessViews(0,1,&uav,&keep); ctx->Dispatch(1,1,1);
        saved.restore();
        ctx->CopyResource(staging_.Get(),output_.Get());
        pending_=true; count_=instances; reason_=OriginalDrawIdentityReason::Pending; return true;
    }

    OriginalDrawIdentityPoll poll(ID3D11DeviceContext* ctx, OriginalDrawIdentitySnapshot& out) noexcept {
        if (!pending_ || !ctx || !staging_) return OriginalDrawIdentityPoll::Unavailable;
        D3D11_MAPPED_SUBRESOURCE map{}; const HRESULT h=ctx->Map(staging_.Get(),0,D3D11_MAP_READ,D3D11_MAP_FLAG_DO_NOT_WAIT,&map);
        if (h==DXGI_ERROR_WAS_STILL_DRAWING) return OriginalDrawIdentityPoll::Pending;
        if (FAILED(h) || !map.pData) { pending_=false; reason_=OriginalDrawIdentityReason::DispatchFailed; return OriginalDrawIdentityPoll::Unavailable; }
        out={}; out.count=count_; out.startInstance=startInstance_;
        const auto* p=static_cast<const uint8_t*>(map.pData);
        for(UINT i=0;i<count_;++i){ const uint32_t* header=reinterpret_cast<const uint32_t*>(p+i*kGpuRecordBytes);
            auto& r=out.records[i]; r.instanceAndModelDataIndex[0]=header[0];r.instanceAndModelDataIndex[1]=header[1];r.valid=header[2]!=0;
            if(r.valid){std::memcpy(r.modelRecord.data(),p+i*kGpuRecordBytes+16,kOriginalDrawIdentityModelBytes);uint32_t bone=0;std::memcpy(&bone,r.modelRecord.data(),4);r.skinned=bone!=0;}}
        ctx->Unmap(staging_.Get(),0); pending_=false; reason_=OriginalDrawIdentityReason::None; return OriginalDrawIdentityPoll::Ready;
    }
    OriginalDrawIdentityReason reason() const noexcept { return reason_; }
private:
    struct ComputeState {
        ID3D11DeviceContext* c; Microsoft::WRL::ComPtr<ID3D11ComputeShader> shader; ID3D11ClassInstance* classes[256]{}; UINT classCount=256;
        Microsoft::WRL::ComPtr<ID3D11Buffer> cb; ID3D11ShaderResourceView* srv[2]{}; Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> uav;
        Microsoft::WRL::ComPtr<ID3D11Predicate> predicate; BOOL predicateValue=FALSE; bool restored=false;
        explicit ComputeState(ID3D11DeviceContext* x):c(x){c->CSGetShader(&shader,classes,&classCount);c->CSGetConstantBuffers(0,1,&cb);c->CSGetShaderResources(0,2,srv);c->CSGetUnorderedAccessViews(0,1,&uav);c->GetPredication(&predicate,&predicateValue);c->SetPredication(nullptr,FALSE);}
        void restore(){if(restored)return;ID3D11ShaderResourceView* noSrv[2]{};ID3D11UnorderedAccessView* noUav=nullptr;UINT keep=~0u;c->CSSetShaderResources(0,2,noSrv);c->CSSetUnorderedAccessViews(0,1,&noUav,&keep);c->CSSetShader(shader.Get(),classes,classCount);c->CSSetConstantBuffers(0,1,cb.GetAddressOf());c->CSSetShaderResources(0,2,srv);c->CSSetUnorderedAccessViews(0,1,uav.GetAddressOf(),&keep);c->SetPredication(predicate.Get(),predicateValue);for(auto* p:srv)if(p)p->Release();for(UINT i=0;i<classCount;++i)if(classes[i])classes[i]->Release();restored=true;}
        ~ComputeState(){restore();}
    };
    bool ensure(ID3D11Device* d) noexcept {
        if(instanceIds_ && instanceIdsView_ && output_ && outputView_ && staging_)return true;
        instanceIds_.Reset();instanceIdsView_.Reset();output_.Reset();outputView_.Reset();staging_.Reset();
        Microsoft::WRL::ComPtr<ID3D11Buffer> instanceIds,output,staging;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> instanceIdsView;
        Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> outputView;
        D3D11_BUFFER_DESC b{};b.ByteWidth=kOriginalDrawIdentityMaxInstances*8;b.BindFlags=D3D11_BIND_SHADER_RESOURCE;b.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
        if(FAILED(d->CreateBuffer(&b,nullptr,&instanceIds)))return false;
        D3D11_SHADER_RESOURCE_VIEW_DESC s{};s.Format=DXGI_FORMAT_R32_TYPELESS;s.ViewDimension=D3D11_SRV_DIMENSION_BUFFEREX;s.BufferEx.NumElements=b.ByteWidth/4;s.BufferEx.Flags=D3D11_BUFFEREX_SRV_FLAG_RAW;
        if(FAILED(d->CreateShaderResourceView(instanceIds.Get(),&s,&instanceIdsView)))return false;
        b={};b.ByteWidth=kOriginalDrawIdentityMaxInstances*kGpuRecordBytes;b.BindFlags=D3D11_BIND_UNORDERED_ACCESS;b.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;b.StructureByteStride=kGpuRecordBytes;
        if(FAILED(d->CreateBuffer(&b,nullptr,&output))||FAILED(d->CreateUnorderedAccessView(output.Get(),nullptr,&outputView)))return false;
        b.BindFlags=0;b.MiscFlags=0;b.StructureByteStride=0;b.Usage=D3D11_USAGE_STAGING;b.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        if(FAILED(d->CreateBuffer(&b,nullptr,&staging)))return false;
        instanceIds_=instanceIds;instanceIdsView_=instanceIdsView;output_=output;outputView_=outputView;staging_=staging;return true;
    }
    bool fail(OriginalDrawIdentityReason r) noexcept {reason_=r;return false;}
    Microsoft::WRL::ComPtr<ID3D11Buffer> instanceIds_,output_,staging_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> instanceIdsView_;
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> outputView_;
    OriginalDrawIdentityReason reason_=OriginalDrawIdentityReason::None; uint32_t count_=0,startInstance_=0; bool pending_=false;
};

} // namespace edvr
