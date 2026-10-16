// fix.sharp_weapons' GPU pieces that the DLL (ui_layer.cpp) and the WARP rig (tools/sharp_weapons_test) must run identically: the blend
// states of the tonemap re-issues, the source pass's recorder, and the mapping from a plan step (weapons_fire.h) to the HDR source and the
// blend it is issued with. Header-only; no game state, no Log.
#pragma once

#include <d3d11.h>
#include <wrl/client.h>

#include <cstdint>

#include "weapons_fire.h"

namespace edvr {

static_assert(D3D11_BLEND_OP_ADD == 1 && D3D11_BLEND_OP_SUBTRACT == 2, "weapons_fire.h's ToneWrite numbers are D3D11's blend operations");

// The blend states the passes use, made once per device.
struct SharpPassStates {
    Microsoft::WRL::ComPtr<ID3D11BlendState> sourceBlend;   // the source pass: target 0 replaced, target 1 ADDED (colour only)
    Microsoft::WRL::ComPtr<ID3D11BlendState> toneReplace;   // E.rgb = src
    Microsoft::WRL::ComPtr<ID3D11BlendState> toneAdd;       // E.rgb = src + E.rgb
    Microsoft::WRL::ComPtr<ID3D11BlendState> toneSubtract;  // E.rgb = src - E.rgb  (the lit term minus the base term already in E)

    bool ready() const { return sourceBlend && toneReplace && toneAdd && toneSubtract; }

    bool create(ID3D11Device* dev) {
        if (!dev) return false;
        if (ready()) return true;
        const auto rgb = [](D3D11_RENDER_TARGET_BLEND_DESC& r, bool enable, D3D11_BLEND_OP op) {
            r.BlendEnable = enable ? TRUE : FALSE;
            r.SrcBlend = D3D11_BLEND_ONE;
            r.DestBlend = enable ? D3D11_BLEND_ONE : D3D11_BLEND_ZERO;
            r.BlendOp = op;
            r.SrcBlendAlpha = D3D11_BLEND_ONE;
            r.DestBlendAlpha = D3D11_BLEND_ZERO;
            r.BlendOpAlpha = D3D11_BLEND_OP_ADD;
            r.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN | D3D11_COLOR_WRITE_ENABLE_BLUE;
        };
        D3D11_BLEND_DESC d{};
        rgb(d.RenderTarget[0], false, D3D11_BLEND_OP_ADD);
        if (FAILED(dev->CreateBlendState(&d, toneReplace.ReleaseAndGetAddressOf()))) return false;
        rgb(d.RenderTarget[0], true, D3D11_BLEND_OP_ADD);
        if (FAILED(dev->CreateBlendState(&d, toneAdd.ReleaseAndGetAddressOf()))) return false;
        rgb(d.RenderTarget[0], true, D3D11_BLEND_OP_SUBTRACT);
        if (FAILED(dev->CreateBlendState(&d, toneSubtract.ReleaseAndGetAddressOf()))) return false;
        D3D11_BLEND_DESC m{};
        m.IndependentBlendEnable = TRUE;
        rgb(m.RenderTarget[0], false, D3D11_BLEND_OP_ADD);
        rgb(m.RenderTarget[1], true, D3D11_BLEND_OP_ADD);
        return SUCCEEDED(dev->CreateBlendState(&m, sourceBlend.ReleaseAndGetAddressOf()));
    }
    void reset() {
        sourceBlend.Reset();
        toneReplace.Reset();
        toneAdd.Reset();
        toneSubtract.Reset();
    }
    ID3D11BlendState* tone(ToneWrite w) const {
        return w == ToneWrite::kAdd ? toneAdd.Get() : w == ToneWrite::kSubtract ? toneSubtract.Get() : toneReplace.Get();
    }
};

// What one plan step is issued with: the HDR source bound at the admitted slot, and the blend over E.
struct SharpStepBinding {
    ID3D11ShaderResourceView* source = nullptr;
    ID3D11BlendState* blend = nullptr;
};
inline SharpStepBinding sharpStepBinding(ToneStep step, ID3D11ShaderResourceView* hud, ID3D11ShaderResourceView* weapons,
                                         ID3D11ShaderResourceView* scratch, const SharpPassStates& states) {
    SharpStepBinding b;
    const ToneSource s = toneStepSource(step);
    b.source = s == ToneSource::kHud ? hud : s == ToneSource::kWeapons ? weapons : scratch;
    b.blend = states.tone(toneStepWrite(step));
    return b;
}

// The source pass, recorded on a deferred context (the pattern of the HUD's coverage pass: state recorded off the game's context,
// executed on the immediate one with its state restored by `execute`): F -> S replaced, W += F_up, at the layer's size.
using SharpExecute = void (*)(ID3D11DeviceContext*, ID3D11CommandList*, int);
inline bool sharpSourcePass(ID3D11DeviceContext* immediate, ID3D11DeviceContext* deferred, ID3D11VertexShader* vs, ID3D11PixelShader* ps,
                            const SharpPassStates& states, ID3D11ShaderResourceView* f, ID3D11RenderTargetView* scratch,
                            ID3D11RenderTargetView* weapons, uint32_t layerW, uint32_t layerH, SharpExecute execute) {
    if (!immediate || !deferred || !vs || !ps || !states.sourceBlend || !f || !scratch || !weapons || !layerW || !layerH || !execute)
        return false;
    ID3D11RenderTargetView* targets[2] = {scratch, weapons};
    deferred->IASetInputLayout(nullptr);
    deferred->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    deferred->VSSetShader(vs, nullptr, 0);
    deferred->PSSetShader(ps, nullptr, 0);
    deferred->PSSetShaderResources(0, 1, &f);
    deferred->OMSetRenderTargets(2, targets, nullptr);
    deferred->OMSetBlendState(states.sourceBlend.Get(), nullptr, 0xFFFFFFFFu);
    const D3D11_VIEWPORT vp{0.0f, 0.0f, static_cast<float>(layerW), static_cast<float>(layerH), 0.0f, 1.0f};
    deferred->RSSetViewports(1, &vp);
    deferred->Draw(3, 0);
    ID3D11ShaderResourceView* none = nullptr;
    deferred->PSSetShaderResources(0, 1, &none);
    Microsoft::WRL::ComPtr<ID3D11CommandList> list;
    if (FAILED(deferred->FinishCommandList(FALSE, list.ReleaseAndGetAddressOf())) || !list) return false;
    execute(immediate, list.Get(), 1);
    return true;
}

}  // namespace edvr
