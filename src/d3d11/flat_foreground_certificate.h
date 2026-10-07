#pragma once
#include "flat_foreground_motion.h"
#include "flat_projection_runtime.h"
#include <d3d11_1.h>
namespace edvr {
// Exact previous-draw input certificate for duplicate pose identities.
// Missing CPU CB bytes or unwitnessed resource epochs remain incomplete.
inline FlatForegroundMotion::Certificate flatForegroundCertificate(
    ID3D11DeviceContext* ctx,FlatProjectionRuntime* projection,FlatAnimatedIdentityLedger& ledger) {
    using Microsoft::WRL::ComPtr;
    FlatForegroundMotion::Certificate out;bool complete=projection!=nullptr;
    auto append=[&](const void* data,size_t bytes) {
        if(bytes>65536-out.constants.size()){complete=false;return;}
        const auto* p=static_cast<const unsigned char*>(data);out.constants.insert(out.constants.end(),p,p+bytes);
    };
    auto resource=[&](ID3D11Resource* p) {
        if(!p)return;
        if(out.resourceCount==out.resources.size()){complete=false;return;}
        auto& r=out.resources[out.resourceCount++];r.resource=p;
        r.epoch=ledger.demandMutation(p);if(!r.epoch)complete=false;
    };
    ID3D11Buffer* buffers[14]{};UINT first[14]{},counts[14]{};
    ComPtr<ID3D11DeviceContext1> context1;
    if(SUCCEEDED(ctx->QueryInterface(IID_PPV_ARGS(&context1))))context1->VSGetConstantBuffers1(0,14,buffers,first,counts);
    else {ctx->VSGetConstantBuffers(0,14,buffers);for(auto& n:counts)n=4096;}
    for(UINT slot=0;slot<14;++slot) {
        ComPtr<ID3D11Buffer> buffer;buffer.Attach(buffers[slot]);
        const uintptr_t pointer=reinterpret_cast<uintptr_t>(buffer.Get());
        append(&slot,sizeof(slot));append(&pointer,sizeof(pointer));append(&first[slot],sizeof(UINT));append(&counts[slot],sizeof(UINT));
        if(!buffer)continue;
        D3D11_BUFFER_DESC desc{};buffer->GetDesc(&desc);append(&desc.ByteWidth,sizeof(UINT));
        if(desc.ByteWidth>65536-out.constants.size()){complete=false;continue;}
        std::vector<unsigned char> bytes(desc.ByteWidth);
        if(!projection || !projection->copyConstants(buffer.Get(),0,desc.ByteWidth,bytes.data()))complete=false;
        else append(bytes.data(),bytes.size());
    }
    ID3D11ShaderResourceView* views[128]{};ctx->VSGetShaderResources(0,128,views);
    for(UINT slot=0;slot<128;++slot) {
        ComPtr<ID3D11ShaderResourceView> view;view.Attach(views[slot]);
        const uintptr_t pointer=reinterpret_cast<uintptr_t>(view.Get());append(&slot,sizeof(slot));append(&pointer,sizeof(pointer));
        if(!view)continue;
        D3D11_SHADER_RESOURCE_VIEW_DESC desc{};view->GetDesc(&desc);append(&desc,sizeof(desc));
        ComPtr<ID3D11Resource> p;view->GetResource(&p);resource(p.Get());
    }
    out.complete=complete;return out;
}
} // namespace edvr
