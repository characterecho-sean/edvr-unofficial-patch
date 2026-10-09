#pragma once
// Capture-only copies of the engine ownership inputs actually consumed by
// trained motion preparation. No allocation or GPU copy unless a run is armed.
#include <d3d11.h>
#include <wrl/client.h>

namespace edvr { namespace eye_engine_capture {
enum class Result { NotReached, NotBound, MissingView, Unsupported, StageFailed, Staged };
inline const char* name(Result r) {
    switch (r) {
    case Result::NotReached: return "capture_not_reached";
    case Result::NotBound: return "engine_not_bound";
    case Result::MissingView: return "view_unavailable";
    case Result::Unsupported: return "texture_unsupported";
    case Result::StageFailed: return "stage_failed";
    case Result::Staged: return "staged";
    }
    return "invalid_status";
}
inline Result stage(ID3D11DeviceContext* ctx, bool bound, ID3D11ShaderResourceView* view,
                    ID3D11Texture2D** out) {
    if (!bound) return Result::NotBound;
    if (!view) return Result::MissingView;
    Microsoft::WRL::ComPtr<ID3D11Resource> resource;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> source;
    view->GetResource(&resource);
    if (!resource || FAILED(resource.As(&source))) return Result::Unsupported;
    D3D11_TEXTURE2D_DESC d{}; source->GetDesc(&d);
    if (d.SampleDesc.Count != 1 || d.ArraySize != 1 || d.MipLevels != 1)
        return Result::Unsupported;
    d.Usage = D3D11_USAGE_STAGING; d.BindFlags = 0;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ; d.MiscFlags = 0;
    Microsoft::WRL::ComPtr<ID3D11Device> device;
    ctx->GetDevice(&device);
    if (!device || FAILED(device->CreateTexture2D(&d, nullptr, out)) || !*out)
        return Result::StageFailed;
    ctx->CopyResource(*out, source.Get());
    return Result::Staged;
}
inline Result stageBuffer(ID3D11DeviceContext* ctx,bool bound,ID3D11Buffer* source,ID3D11Buffer** out) {
    if(!bound)return Result::NotBound;
    if(!source)return Result::MissingView;
    D3D11_BUFFER_DESC d{};source->GetDesc(&d);
    if(!d.ByteWidth)return Result::Unsupported;
    d.Usage=D3D11_USAGE_STAGING;d.BindFlags=0;d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    d.MiscFlags=0;d.StructureByteStride=0;
    Microsoft::WRL::ComPtr<ID3D11Device> device;ctx->GetDevice(&device);
    if(!device || FAILED(device->CreateBuffer(&d,nullptr,out)) || !*out)return Result::StageFailed;
    ctx->CopyResource(*out,source);return Result::Staged;
}
} }
