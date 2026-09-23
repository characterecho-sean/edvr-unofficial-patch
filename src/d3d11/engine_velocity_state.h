#pragma once
// Engine-record velocity's two state checks for a substituted pool draw, kept
// apart from engine_velocity.cpp so tools/engine_velocity_test runs them on
// WARP exactly as the DLL does (the 2026-09-23 review of engine motion,
// reviews\engine-motion-review-2026-09-23.md, item 4, and the flight of
// 2026-09-23's MRT6 bind question).
//
//   1. THE BLEND STATE. The slot target (MRT6) must hold exactly what the
//      patched pixel shader wrote: an inherited blend -- target 0's state
//      applies to every target when the game leaves independent blend off --
//      turns (slot 1, depth 0.5) over the cleared (-1, 0) into (slot 0, depth
//      0.5) under ONE+ONE, a valid-looking slot with a valid depth. So the
//      draw runs under a DERIVED state: the game's own for every target it
//      has (with independent blend off, target 0's state copied to each, which
//      is what D3D applies), and target 6 unblended, writing R and G only.
//      AlphaToCoverage is kept: it removes coverage from every target at once,
//      depth included, so ownership stays exact.
//   2. THE BINDING. MRT6 joins the game's own targets only when every one of
//      them and the depth view are single-sample 2D textures at mip 0 of the
//      slot target's size, and the depth is 32-bit float (the consumer's
//      equality test compares the depth this fragment wrote, bit for bit,
//      with the stored one). Anything else is refused and counted, never
//      bound: a mismatched set is dropped by the runtime, which would drop the
//      game's own draw with it.
#include <d3d11.h>
#include <d3d11_1.h>
#include <wrl/client.h>

#include <cstdint>

#include "dxbc_engine_velocity.h"   // kEngineVelocityTarget

namespace edvr {

// The derived state's description.
inline D3D11_BLEND_DESC engineVelocityDerivedBlend(const D3D11_BLEND_DESC& game) {
    D3D11_BLEND_DESC d = game;
    if (!game.IndependentBlendEnable)
        for (UINT i = 1; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i) d.RenderTarget[i] = game.RenderTarget[0];
    d.IndependentBlendEnable = TRUE;
    D3D11_RENDER_TARGET_BLEND_DESC& t = d.RenderTarget[kEngineVelocityTarget];
    t.BlendEnable = FALSE;
    t.SrcBlend = D3D11_BLEND_ONE;
    t.DestBlend = D3D11_BLEND_ZERO;
    t.BlendOp = D3D11_BLEND_OP_ADD;
    t.SrcBlendAlpha = D3D11_BLEND_ONE;
    t.DestBlendAlpha = D3D11_BLEND_ZERO;
    t.BlendOpAlpha = D3D11_BLEND_OP_ADD;
    t.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN;
    return d;
}

// What a null blend state means (the D3D11 default).
inline D3D11_BLEND_DESC engineVelocityDefaultBlend() {
    D3D11_BLEND_DESC d{};
    for (auto& t : d.RenderTarget) {
        t.BlendEnable = FALSE;
        t.SrcBlend = D3D11_BLEND_ONE;
        t.DestBlend = D3D11_BLEND_ZERO;
        t.BlendOp = D3D11_BLEND_OP_ADD;
        t.SrcBlendAlpha = D3D11_BLEND_ONE;
        t.DestBlendAlpha = D3D11_BLEND_ZERO;
        t.BlendOpAlpha = D3D11_BLEND_OP_ADD;
        t.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    }
    return d;
}

// The derived state object for the game's (null = the default), or null when
// the game's cannot be expressed without changing its own targets: a D3D11.1
// logic op, which a D3D11_BLEND_DESC cannot carry.
inline Microsoft::WRL::ComPtr<ID3D11BlendState> engineVelocityCreateDerivedBlend(ID3D11Device* device,
                                                                               ID3D11BlendState* game,
                                                                               const char** refused) {
    Microsoft::WRL::ComPtr<ID3D11BlendState> out;
    if (refused) *refused = nullptr;
    D3D11_BLEND_DESC desc = engineVelocityDefaultBlend();
    if (game) {
        Microsoft::WRL::ComPtr<ID3D11BlendState1> game1;
        if (SUCCEEDED(game->QueryInterface(IID_PPV_ARGS(&game1))) && game1) {
            D3D11_BLEND_DESC1 d1{};
            game1->GetDesc1(&d1);
            for (const auto& t : d1.RenderTarget)
                if (t.LogicOpEnable) { if (refused) *refused = "the game's blend state uses a logic op"; return out; }
        }
        game->GetDesc(&desc);
    }
    const D3D11_BLEND_DESC derived = engineVelocityDerivedBlend(desc);
    if (!device || FAILED(device->CreateBlendState(&derived, &out))) {
        out.Reset();
        if (refused) *refused = "CreateBlendState failed";
    }
    return out;
}

// Why a pass binding was refused MRT6, first failing test.
enum class EngineVelocityBindRefusal : int {
    None = 0,
    DepthView,     // the depth view is not a 2D texture at mip 0
    DepthFormat,   // the depth is not 32-bit float
    DepthSize,     // the depth texture is multisampled, or not the slot target's size
    TargetView,    // a render target is not a single-sample 2D texture at mip 0
    TargetSize,    // a render target's size is not the slot target's
    Count
};
inline const char* engineVelocityBindRefusalName(EngineVelocityBindRefusal r) {
    switch (r) {
    case EngineVelocityBindRefusal::DepthView: return "depth view not a 2D texture at mip 0";
    case EngineVelocityBindRefusal::DepthFormat: return "depth not 32-bit float";
    case EngineVelocityBindRefusal::DepthSize: return "depth multisampled or not the slot target's size";
    case EngineVelocityBindRefusal::TargetView: return "a render target not a single-sample 2D texture at mip 0";
    case EngineVelocityBindRefusal::TargetSize: return "a render target not the slot target's size";
    default: return "none";
    }
}

// rt: the eight bound render-target views (slot 6 ignored: it is MRT6's own).
inline EngineVelocityBindRefusal engineVelocityValidateTargets(ID3D11RenderTargetView* const* rt, ID3D11DepthStencilView* dsv,
                                                              UINT width, UINT height) {
    using R = EngineVelocityBindRefusal;
    if (!dsv) return R::DepthView;
    D3D11_DEPTH_STENCIL_VIEW_DESC dd{};
    dsv->GetDesc(&dd);
    if (dd.ViewDimension != D3D11_DSV_DIMENSION_TEXTURE2D || dd.Texture2D.MipSlice != 0) return R::DepthView;
    if (dd.Format != DXGI_FORMAT_D32_FLOAT && dd.Format != DXGI_FORMAT_D32_FLOAT_S8X24_UINT) return R::DepthFormat;
    {
        Microsoft::WRL::ComPtr<ID3D11Resource> res;
        dsv->GetResource(&res);
        Microsoft::WRL::ComPtr<ID3D11Texture2D> tex;
        if (!res || FAILED(res.As(&tex))) return R::DepthView;
        D3D11_TEXTURE2D_DESC td{};
        tex->GetDesc(&td);
        if (td.SampleDesc.Count != 1 || td.Width != width || td.Height != height) return R::DepthSize;
    }
    for (UINT i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i) {
        if (i == kEngineVelocityTarget || !rt[i]) continue;
        D3D11_RENDER_TARGET_VIEW_DESC rd{};
        rt[i]->GetDesc(&rd);
        if (rd.ViewDimension != D3D11_RTV_DIMENSION_TEXTURE2D || rd.Texture2D.MipSlice != 0) return R::TargetView;
        Microsoft::WRL::ComPtr<ID3D11Resource> res;
        rt[i]->GetResource(&res);
        Microsoft::WRL::ComPtr<ID3D11Texture2D> tex;
        if (!res || FAILED(res.As(&tex))) return R::TargetView;
        D3D11_TEXTURE2D_DESC td{};
        tex->GetDesc(&td);
        if (td.SampleDesc.Count != 1) return R::TargetView;
        if (td.Width != width || td.Height != height) return R::TargetSize;
    }
    return R::None;
}

}  // namespace edvr
