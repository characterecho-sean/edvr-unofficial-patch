#pragma once
// The crisp HUD's alpha transfer, shared by ui_layer.cpp and the WARP rig.
// Record only EDVR-owned state on its deferred context; execute with the
// game's immediate-context state restored, through the caller's raw entry.
#include <d3d11.h>
#include <cstdint>
#include <wrl/client.h>

namespace edvr {
struct UiCoverageBindings {
    ID3D11ShaderResourceView* source = nullptr;
    ID3D11RenderTargetView* target = nullptr;
    ID3D11VertexShader* vs = nullptr;
    ID3D11PixelShader* ps = nullptr;
    ID3D11BlendState* blend = nullptr;
    uint32_t width = 0, height = 0;
};
using UiCoverageExecute = void (*)(ID3D11DeviceContext*, ID3D11CommandList*, int);
inline bool uiCoverageRecord(ID3D11DeviceContext* deferred, const UiCoverageBindings& b,
                             Microsoft::WRL::ComPtr<ID3D11CommandList>& list) {
    if (!deferred || !b.source || !b.target || !b.vs || !b.ps || !b.blend || !b.width || !b.height)
        return false;
    deferred->IASetInputLayout(nullptr);
    deferred->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    deferred->VSSetShader(b.vs, nullptr, 0);
    deferred->PSSetShader(b.ps, nullptr, 0);
    deferred->PSSetShaderResources(0, 1, &b.source);
    deferred->OMSetRenderTargets(1, &b.target, nullptr);
    deferred->OMSetBlendState(b.blend, nullptr, 0xFFFFFFFFu);
    const D3D11_VIEWPORT vp{0.0f, 0.0f, static_cast<float>(b.width), static_cast<float>(b.height), 0.0f, 1.0f};
    deferred->RSSetViewports(1, &vp);
    deferred->Draw(3, 0);
    ID3D11ShaderResourceView* none = nullptr;
    deferred->PSSetShaderResources(0, 1, &none);
    return SUCCEEDED(deferred->FinishCommandList(FALSE, list.ReleaseAndGetAddressOf())) && list;
}
inline bool uiCoverageExecuteUncached(ID3D11DeviceContext* ctx, ID3D11DeviceContext* deferred,
                                      const UiCoverageBindings& b, UiCoverageExecute execute) {
    if (!ctx || !execute) return false;
    Microsoft::WRL::ComPtr<ID3D11CommandList> list;
    if (!uiCoverageRecord(deferred, b, list)) return false;
    execute(ctx, list.Get(), 1);
    return true;
}

// A list contains view references and a fixed viewport, never captured pixels
// or per-frame constants. Re-execution reads the source texture's current data.
// Keep one cache per Eye, and reset it before replacing that Eye's resources.
class UiCoverageCommandCache {
    Microsoft::WRL::ComPtr<ID3D11CommandList> list_;
    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> immediate_, deferred_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> source_;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> target_;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> vs_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> ps_;
    Microsoft::WRL::ComPtr<ID3D11BlendState> blend_;
    uint32_t width_ = 0, height_ = 0;

    bool matches(ID3D11DeviceContext* ctx, ID3D11DeviceContext* deferred,
                 const UiCoverageBindings& b) const noexcept {
        return list_ && immediate_.Get() == ctx && deferred_.Get() == deferred &&
            source_.Get() == b.source && target_.Get() == b.target &&
            vs_.Get() == b.vs && ps_.Get() == b.ps && blend_.Get() == b.blend &&
            width_ == b.width && height_ == b.height;
    }
    static bool onDevice(ID3D11DeviceChild* child, ID3D11Device* device) {
        Microsoft::WRL::ComPtr<ID3D11Device> actual;
        child->GetDevice(&actual);
        return actual.Get() == device;
    }
public:
    void reset() noexcept {
        // Drop commands first: they retain every referenced view and shader.
        list_.Reset(); source_.Reset(); target_.Reset(); vs_.Reset(); ps_.Reset(); blend_.Reset();
        immediate_.Reset(); deferred_.Reset(); device_.Reset(); width_ = height_ = 0;
    }
    bool hasCommands() const noexcept { return !!list_; }
    bool execute(ID3D11DeviceContext* ctx, ID3D11DeviceContext* deferred,
                 const UiCoverageBindings& b, UiCoverageExecute executeRaw) {
        if (!ctx || !deferred || !executeRaw || !b.source || !b.target || !b.vs ||
            !b.ps || !b.blend || !b.width || !b.height) { reset(); return false; }
        if (!matches(ctx, deferred, b)) {
            reset();
            Microsoft::WRL::ComPtr<ID3D11Device> device;
            ctx->GetDevice(&device);
            if (!device || ctx->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE ||
                deferred->GetType() != D3D11_DEVICE_CONTEXT_DEFERRED ||
                !onDevice(deferred, device.Get()) || !onDevice(b.source, device.Get()) ||
                !onDevice(b.target, device.Get()) || !onDevice(b.vs, device.Get()) ||
                !onDevice(b.ps, device.Get()) || !onDevice(b.blend, device.Get())) return false;
            Microsoft::WRL::ComPtr<ID3D11CommandList> recorded;
            if (!uiCoverageRecord(deferred, b, recorded)) return false;
            device_ = device; immediate_ = ctx; deferred_ = deferred;
            source_ = b.source; target_ = b.target; vs_ = b.vs; ps_ = b.ps; blend_ = b.blend;
            width_ = b.width; height_ = b.height; list_ = recorded;
        }
        executeRaw(ctx, list_.Get(), 1);
        return true;
    }
};
}
