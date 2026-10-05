#pragma once

#include "flat_animated_identity_ledger.h"
#include <d3d11.h>
#include <wrl/client.h>
#include <array>

namespace edvr {

// Owner-thread CPU upload witness. No GPU readback and no retained mapped
// pointer after Unmap. Demand happens at an original draw; an earlier upload
// is deliberately unavailable until a subsequent complete discard upload.
class FlatForegroundIdentity {
    template<class T> using Ptr=Microsoft::WRL::ComPtr<T>;
    struct Mapped {
        Ptr<ID3D11Resource> resource;
        const void* bytes=nullptr;
        UINT size=0;
        bool discard=false;
    };
    FlatAnimatedIdentityLedger ledger_;
    std::array<Mapped,6> mapped_{};
    struct Held {Ptr<ID3D11Resource> resource;uint64_t touch=0;};
    std::array<Held,4> instances_{};
    std::array<Held,2> pools_{};
    uint64_t clock_=0;
    template<size_t N> void hold(std::array<Held,N>& entries,ID3D11Resource* resource) {
        Held* chosen=nullptr;
        for(auto& e:entries)if(e.resource.Get()==resource){e.touch=++clock_;return;}
        for(auto& e:entries)if(!e.resource){chosen=&e;break;}
        if(!chosen){chosen=&entries[0];for(auto& e:entries)if(e.touch<chosen->touch)chosen=&e;}
        if(chosen->resource)ledger_.erase(chosen->resource.Get());
        chosen->resource=resource;chosen->touch=++clock_;
    }
public:
    using Identity=FlatAnimatedIdentityLedger::Identity;
    FlatAnimatedIdentityLedger& ledger() {return ledger_;}
    void reset() {ledger_.reset();for(auto& m:mapped_)m={};for(auto& e:instances_)e={};for(auto& e:pools_)e={};clock_=0;}
    void written(ID3D11Resource* resource) {ledger_.invalidate(resource);}
    void map(ID3D11Resource* resource,D3D11_MAP kind,const void* bytes) {
        if(!ledger_.beginMap(resource,kind))return;
        Ptr<ID3D11Buffer> buffer;
        if(!resource || FAILED(resource->QueryInterface(IID_PPV_ARGS(&buffer))))return;
        D3D11_BUFFER_DESC desc{};buffer->GetDesc(&desc);
        for(auto& m:mapped_)if(!m.resource || m.resource.Get()==resource) {
            m.resource=resource;m.bytes=bytes;m.size=desc.ByteWidth;
            m.discard=kind==D3D11_MAP_WRITE_DISCARD || kind==D3D11_MAP_WRITE || kind==D3D11_MAP_WRITE_NO_OVERWRITE;return;
        }
        ledger_.invalidate(resource);
    }
    void unmap(ID3D11Resource* resource) {
        for(auto& m:mapped_)if(m.resource.Get()==resource) {
            ledger_.endMap(resource,m.bytes,m.size,m.discard);m={};return;
        }
    }
    void update(ID3D11Resource* resource,const void* bytes,const D3D11_BOX* box) {
        Ptr<ID3D11Buffer> buffer;
        if(!resource || FAILED(resource->QueryInterface(IID_PPV_ARGS(&buffer))))return;
        D3D11_BUFFER_DESC desc{};buffer->GetDesc(&desc);
        if(!box || (box->left==0 && box->right==desc.ByteWidth &&
                    box->top==0 && box->bottom==1 && box->front==0 && box->back==1))
            ledger_.publishWhole(resource,bytes,desc.ByteWidth);
        else ledger_.invalidate(resource);
    }
    bool demandAndLookup(ID3D11DeviceContext* ctx,UINT startInstance,Identity& identity) {
        auto fail=[&](const char* why){identity={};identity.refusal=why;return false;};
        Ptr<ID3D11Buffer> instance;UINT stride=0,offset=0;
        ctx->IAGetVertexBuffers(0,1,&instance,&stride,&offset);
        Ptr<ID3D11ShaderResourceView> view;ctx->VSGetShaderResources(33,1,&view);
        if(!instance || !view || stride!=8)return fail("flat-identity-binding");
        D3D11_SHADER_RESOURCE_VIEW_DESC vd{};view->GetDesc(&vd);
        Ptr<ID3D11Resource> resource;view->GetResource(&resource);Ptr<ID3D11Buffer> pool;
        if(vd.ViewDimension!=D3D11_SRV_DIMENSION_BUFFER || vd.Buffer.FirstElement ||
           FAILED(resource.As(&pool)))return fail("flat-identity-view");
        D3D11_BUFFER_DESC id{},pd{};instance->GetDesc(&id);pool->GetDesc(&pd);
        if(id.ByteWidth>FlatAnimatedIdentityLedger::maxInstanceBytes || pd.StructureByteStride!=336 ||
           pd.ByteWidth%336 || pd.ByteWidth/336>FlatAnimatedIdentityLedger::maxPoolRows)
            return fail("flat-identity-resource-bound");
        hold(instances_,instance.Get());hold(pools_,pool.Get());
        if(vd.Buffer.NumElements!=pd.ByteWidth/336 ||
           !ledger_.demandInstances(instance.Get(),id.ByteWidth))
            return fail("flat-identity-cap-or-layout");
        uint32_t slot=0;uint64_t epoch=0;
        if(!ledger_.lookupInstance(instance.Get(),uint64_t(offset)+uint64_t(startInstance)*stride,slot,epoch))
            return fail("flat-identity-instance-upload-unobserved");
        if(!ledger_.demandPoolSlot(pool.Get(),pd.ByteWidth,pd.StructureByteStride,slot))
            return fail("flat-identity-pool-slot-bound");
        return ledger_.lookup(instance.Get(),uint64_t(offset)+uint64_t(startInstance)*stride,pool.Get(),identity);
    }
};
} // namespace edvr
