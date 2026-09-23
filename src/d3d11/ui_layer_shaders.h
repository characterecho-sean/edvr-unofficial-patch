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
constexpr char kUiLayerCompositeHlsl[] = R"HLSL(
Texture2D<float4> Frame : register(t0);
Texture2D<float4> Layer : register(t1);
Texture2D<float4> Mult : register(t2);
RWTexture2D<float4> Out : register(u0);
cbuffer P : register(b0) {
    int4   region;     // the frame's region in Frame: x0, y0, x1, y1 (exclusive)
    float4 uv;         // the layer's rectangle for that region: u0, v0, u1, v1
    float2 layerSize;  // the layer, texels
    uint2  outSize;    // the region's size, which is the output's
    uint   mode;       // 0 the composite, 1 the ui_layer debug view
    uint   useMult;    // 1: a multiply drew into M this frame
    uint2  pad;
};
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= outSize.x || id.y >= outSize.y) return;
    int2 p = region.xy + int2(id.xy);
    float4 f = Frame.Load(int3(p, 0));
    float2 span = (uv.zw - uv.xy) * layerSize / float2(outSize);
    float2 x0 = uv.xy * layerSize + float2(id.xy) * span;
    float2 x1 = x0 + span;
    int2 k0 = max(int2(floor(x0)), int2(0, 0));
    int2 k1 = min(int2(ceil(x1)) - 1, int2(layerSize) - 1);
    float4 acc = float4(0, 0, 0, 0);
    float3 accM = float3(0, 0, 0);
    float wsum = 0;
    [loop] for (int j = k0.y; j <= min(k1.y, k0.y + 3); ++j) {
        float wy = min(x1.y, (float)j + 1.0) - max(x0.y, (float)j);
        if (wy <= 0) continue;
        [loop] for (int i = k0.x; i <= min(k1.x, k0.x + 3); ++i) {
            float wx = min(x1.x, (float)i + 1.0) - max(x0.x, (float)i);
            if (wx <= 0) continue;
            acc += Layer.Load(int3(i, j, 0)) * (wx * wy);
            if (useMult != 0) accM += Mult.Load(int3(i, j, 0)).rgb * (wx * wy);
            wsum += wx * wy;
        }
    }
    float4 l = wsum > 0 ? acc / wsum : float4(0, 0, 0, 1);
    float3 m = (useMult != 0 && wsum > 0) ? accM / wsum : float3(1, 1, 1);
    float3 c = mode == 1 ? l.rgb + float3(0.0, 0.08, 0.25) * (1.0 - l.a * dot(m, 1.0 / 3.0))
                         : l.rgb + f.rgb * l.a * m;
    Out[id.xy] = float4(saturate(c), f.a);
}
)HLSL";

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

}  // namespace edvr
