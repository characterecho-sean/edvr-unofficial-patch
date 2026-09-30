#include "fixed_shader_source.h"
// fix.ui_quality's GPU half that the rig must run exactly as the DLL does:
// the composite's HLSL and the blend-state translation between D3D11's
// descriptions and ui_layer_math.h's UiBlendRt. Header-only; the DLL
// (ui_layer.cpp) and tools/ui_quality_test both include it.
#pragma once

#include <d3d11.h>

#include "ui_layer_math.h"

namespace edvr {

static_assert(uiblend::kZero == D3D11_BLEND_ZERO && uiblend::kOne == D3D11_BLEND_ONE &&
                  uiblend::kSrcColor == D3D11_BLEND_SRC_COLOR &&
                  uiblend::kInvSrcColor == D3D11_BLEND_INV_SRC_COLOR &&
                  uiblend::kSrcAlpha == D3D11_BLEND_SRC_ALPHA &&
                  uiblend::kInvSrcAlpha == D3D11_BLEND_INV_SRC_ALPHA &&
                  uiblend::kDestAlpha == D3D11_BLEND_DEST_ALPHA &&
                  uiblend::kInvDestAlpha == D3D11_BLEND_INV_DEST_ALPHA &&
                  uiblend::kDestColor == D3D11_BLEND_DEST_COLOR &&
                  uiblend::kInvDestColor == D3D11_BLEND_INV_DEST_COLOR &&
                  uiblend::kSrcAlphaSat == D3D11_BLEND_SRC_ALPHA_SAT &&
                  uiblend::kBlendFactor == D3D11_BLEND_BLEND_FACTOR &&
                  uiblend::kInvBlendFactor == D3D11_BLEND_INV_BLEND_FACTOR &&
                  uiblend::kSrc1Color == D3D11_BLEND_SRC1_COLOR &&
                  uiblend::kInvSrc1Color == D3D11_BLEND_INV_SRC1_COLOR &&
                  uiblend::kSrc1Alpha == D3D11_BLEND_SRC1_ALPHA &&
                  uiblend::kInvSrc1Alpha == D3D11_BLEND_INV_SRC1_ALPHA &&
                  uiblend::kOpAdd == D3D11_BLEND_OP_ADD,
              "ui_layer_math.h's blend numbers are D3D11's");

inline UiBlendRt uiLayerBlendRtFrom(const D3D11_RENDER_TARGET_BLEND_DESC& r) {
    UiBlendRt b;
    b.enable = r.BlendEnable != FALSE;
    b.src = static_cast<uint8_t>(r.SrcBlend);
    b.dst = static_cast<uint8_t>(r.DestBlend);
    b.op = static_cast<uint8_t>(r.BlendOp);
    b.srcA = static_cast<uint8_t>(r.SrcBlendAlpha);
    b.dstA = static_cast<uint8_t>(r.DestBlendAlpha);
    b.opA = static_cast<uint8_t>(r.BlendOpAlpha);
    b.mask = static_cast<uint8_t>(r.RenderTargetWriteMask);
    return b;
}

static_assert(uids::kAlways == D3D11_COMPARISON_ALWAYS && uids::kKeep == D3D11_STENCIL_OP_KEEP,
              "ui_layer_math.h's depth-stencil numbers are D3D11's");

// A bound depth-stencil state (null = D3D11's default: depth on, LESS,
// writing; stencil off) and the view's read-only flags, as UiDsState.
inline UiDsState uiLayerDsStateFrom(const D3D11_DEPTH_STENCIL_DESC* d, UINT viewFlags) {
    UiDsState s;
    if (!d) {
        s.depthEnable = true;
        s.depthFunc = static_cast<uint8_t>(D3D11_COMPARISON_LESS);
        s.depthWriteAll = true;
    } else {
        s.depthEnable = d->DepthEnable != FALSE;
        s.depthFunc = static_cast<uint8_t>(d->DepthFunc);
        s.depthWriteAll = d->DepthWriteMask == D3D11_DEPTH_WRITE_MASK_ALL;
        s.stencilEnable = d->StencilEnable != FALSE;
        s.readMask = d->StencilReadMask;
        s.writeMask = d->StencilWriteMask;
        auto face = [](const D3D11_DEPTH_STENCILOP_DESC& f) {
            UiDsFace o;
            o.fail = static_cast<uint8_t>(f.StencilFailOp);
            o.depthFail = static_cast<uint8_t>(f.StencilDepthFailOp);
            o.pass = static_cast<uint8_t>(f.StencilPassOp);
            o.func = static_cast<uint8_t>(f.StencilFunc);
            return o;
        };
        s.front = face(d->FrontFace);
        s.back = face(d->BackFace);
    }
    s.readOnlyDepth = (viewFlags & D3D11_DSV_READ_ONLY_DEPTH) != 0;
    s.readOnlyStencil = (viewFlags & D3D11_DSV_READ_ONLY_STENCIL) != 0;
    return s;
}

// The layer's blend state description for a converted UiBlendRt: one
// render target, no alpha-to-coverage, no independent blend.
inline D3D11_BLEND_DESC uiLayerBlendDesc(const UiBlendRt& b) {
    D3D11_BLEND_DESC d{};
    d.AlphaToCoverageEnable = FALSE;
    d.IndependentBlendEnable = FALSE;
    D3D11_RENDER_TARGET_BLEND_DESC& r = d.RenderTarget[0];
    r.BlendEnable = b.enable ? TRUE : FALSE;
    r.SrcBlend = static_cast<D3D11_BLEND>(b.src);
    r.DestBlend = static_cast<D3D11_BLEND>(b.dst);
    r.BlendOp = static_cast<D3D11_BLEND_OP>(b.op);
    r.SrcBlendAlpha = static_cast<D3D11_BLEND>(b.srcA);
    r.DestBlendAlpha = static_cast<D3D11_BLEND>(b.dstA);
    r.BlendOpAlpha = static_cast<D3D11_BLEND_OP>(b.opA);
    r.RenderTargetWriteMask = b.mask;
    return d;
}

// The layer's clear: nothing drawn, all of the frame showing through; and
// the per-channel transmittance's, which a multiply scales.
constexpr float kUiLayerClear[4] = {0.0f, 0.0f, 0.0f, 1.0f};
constexpr float kUiLayerMultClear[4] = {1.0f, 1.0f, 1.0f, 1.0f};

// The composite: one dispatch per eye over the frame's region, into an
// EDVR-owned texture of the region's size in the frame's own format. Each
// output pixel reads the frame at its pixel and the layer (and, when a
// multiply drew this frame, the per-channel transmittance M) over its exact
// footprint (ui_layer_math.h's uiLayerFootprint, separable, at most 4x4
// taps), then out = L.rgb + F.rgb * L.a * M.rgb in the stored (UNORM-view)
// space the game's own composite blended in, the frame's alpha kept. mode 1
// is the `advanced.temporal_aa_debug = ui_layer` view: the layer over black,
// with a dark blue wash where it covers or tints anything, so a translucent
// backing that is nearly black still shows as covered.


// The cbuffer above, laid out to match: four 16-byte rows.
struct UiLayerCompositeParams {
    int32_t region[4];
    float uv[4];
    float layerSize[2];
    uint32_t outSize[2];
    uint32_t mode;
    uint32_t useMult;
    uint32_t pad[2];
};
static_assert(sizeof(UiLayerCompositeParams) == 64, "the cbuffer is four 16-byte rows");

// The composite's format allowlist: the 8-bit families, read and written
// through their plain UNORM view (the space the game's UI composites blend
// in, measured 2026-09-06: RGBA8_TYPELESS viewed as UNORM). sRGB-typed
// frames and every other family are refused, not guessed at.
inline DXGI_FORMAT uiLayerFrameView(DXGI_FORMAT f) {
    switch (f) {
        case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        case DXGI_FORMAT_R8G8B8A8_UNORM:
            return DXGI_FORMAT_R8G8B8A8_UNORM;
        case DXGI_FORMAT_B8G8R8A8_TYPELESS:
        case DXGI_FORMAT_B8G8R8A8_UNORM:
            return DXGI_FORMAT_B8G8R8A8_UNORM;
        default:
            return DXGI_FORMAT_UNKNOWN;
    }
}

// The render-target views the layer accepts draws from: the game's 8-bit
// UNORM view of its post-tonemap target. An sRGB view (hardware blending in
// linear light) would need an sRGB layer and a decode at the composite;
// none has been measured, so it is refused and named in the log.
inline bool uiLayerLdrView(DXGI_FORMAT view) {
    return view == DXGI_FORMAT_R8G8B8A8_UNORM || view == DXGI_FORMAT_B8G8R8A8_UNORM;
}

// the crisp-HUD half's of fix.ui_quality coverage pass (ui_layer.cpp's tonemap re-issue): the HDR
// HUD layer's alpha -- transmittance, the layer's own convention -- written
// into the 8-bit layer's alpha, replacing it, with the colour channels
// masked off (the re-issued tonemap draw just wrote them). One full-screen
// triangle; the two layers are the same size by construction (the caller
// declines when they are not), so the sample is the pixel itself.




}  // namespace edvr
