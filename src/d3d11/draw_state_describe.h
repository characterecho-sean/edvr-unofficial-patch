// Describing a draw's bound D3D11 state as log text, shared by the modules
// that report on the game's draws: fix.ui_quality's layer (ui_layer.cpp,
// whose refusal lines name the state that decided them) and holo_families.h;
// the HUD layer census (removed 2026-09-29) was a third, whose first-state
// lines did the same.
//
// These five lived file-local in ui_layer.cpp until the census needed the
// exact vocabulary the layer's lines already spoke -- a second spelling of
// the same blend or depth-stencil state would have made the two modules'
// lines incomparable for no reason. Moved, not copied: ui_layer.cpp includes
// this header, and behaviour there is unchanged except that viewName now
// names R32_FLOAT and R10G10B10A2_TYPELESS where it used to say "another
// format" (the census meets both; the layer never logs them today).
//
// The one behavioural piece dsStateOf carried for the layer -- the once-a-
// session note about a stencil test against a stencil-less view -- stays
// with the layer (ui_layer.cpp's dsStateOfNoted): its wording is about the
// layer's own copy, and a shared helper must not speak for one caller.
#pragma once

#include <d3d11_1.h>   // ID3D11BlendState1, for the logic-op refusal
#include <wrl/client.h>

#include <cstdio>

#include "ui_layer_math.h"     // UiBlendRt, UiBlendShape, UiDsState, uiLayerBlendShape
#include "ui_layer_shaders.h"  // uiLayerBlendRtFrom, uiLayerDsStateFrom

namespace edvr {

inline const char* viewName(DXGI_FORMAT f) {
    switch (f) {
        case DXGI_FORMAT_R8G8B8A8_UNORM: return "R8G8B8A8_UNORM";
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return "R8G8B8A8_UNORM_SRGB";
        case DXGI_FORMAT_B8G8R8A8_UNORM: return "B8G8R8A8_UNORM";
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return "B8G8R8A8_UNORM_SRGB";
        case DXGI_FORMAT_R11G11B10_FLOAT: return "R11G11B10_FLOAT";
        case DXGI_FORMAT_R10G10B10A2_UNORM: return "R10G10B10A2_UNORM";
        case DXGI_FORMAT_R10G10B10A2_TYPELESS: return "R10G10B10A2_TYPELESS";
        case DXGI_FORMAT_R16G16B16A16_FLOAT: return "R16G16B16A16_FLOAT";
        case DXGI_FORMAT_R8G8B8A8_TYPELESS: return "R8G8B8A8_TYPELESS";
        case DXGI_FORMAT_B8G8R8A8_TYPELESS: return "B8G8R8A8_TYPELESS";
        case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: return "D32_FLOAT_S8X24_UINT";
        case DXGI_FORMAT_D24_UNORM_S8_UINT: return "D24_UNORM_S8_UINT";
        case DXGI_FORMAT_D32_FLOAT: return "D32_FLOAT";
        case DXGI_FORMAT_D16_UNORM: return "D16_UNORM";
        case DXGI_FORMAT_R32G8X24_TYPELESS: return "R32G8X24_TYPELESS";
        case DXGI_FORMAT_R24G8_TYPELESS: return "R24G8_TYPELESS";
        case DXGI_FORMAT_R32_TYPELESS: return "R32_TYPELESS";
        case DXGI_FORMAT_R32_FLOAT: return "R32_FLOAT";
        case DXGI_FORMAT_R16_TYPELESS: return "R16_TYPELESS";
        default: return "another format";
    }
}

// The census's own notation for a blend (bl=enable src,dst,op/srcA,dstA,opA
// and the write mask) and a depth-stencil state, for those lines.
inline void describeBlend(const UiBlendRt& b, char* out, size_t n) {
    _snprintf_s(out, n, _TRUNCATE, "bl=%u%u,%u,%u/%u,%u,%u bm=%X", b.enable ? 1u : 0u, b.src, b.dst,
                b.op, b.srcA, b.dstA, b.opA, b.mask);
}
inline void describeDs(const UiDsState& s, UINT ref, DXGI_FORMAT viewFmt, char* out, size_t n) {
    _snprintf_s(out, n, _TRUNCATE,
                "depth %u func %u write %u; stencil %u ref %u read %02X write %02X front "
                "func %u ops %u,%u,%u back func %u ops %u,%u,%u; %s%s%s",
                s.depthEnable ? 1u : 0u, s.depthFunc, s.depthWriteAll ? 1u : 0u,
                s.stencilEnable ? 1u : 0u, ref, s.readMask, s.writeMask, s.front.func, s.front.fail,
                s.front.depthFail, s.front.pass, s.back.func, s.back.fail, s.back.depthFail,
                s.back.pass, viewName(viewFmt), s.readOnlyDepth ? " read-only depth" : "",
                s.readOnlyStencil ? " read-only stencil" : "");
}

// The shape of a bound blend state, alpha-to-coverage and logic ops refused.
inline UiBlendShape shapeOf(ID3D11BlendState* bs, UiBlendRt* rtOut) {
    UiBlendRt rt;  // null state: D3D11's default -- blending off, all written
    if (bs) {
        D3D11_BLEND_DESC d{};
        bs->GetDesc(&d);
        rt = uiLayerBlendRtFrom(d.RenderTarget[0]);
        if (rtOut) *rtOut = rt;
        if (d.AlphaToCoverageEnable) return UiBlendShape::kRefused;
        Microsoft::WRL::ComPtr<ID3D11BlendState1> bs1;
        if (SUCCEEDED(bs->QueryInterface(__uuidof(ID3D11BlendState1),
                                         reinterpret_cast<void**>(bs1.GetAddressOf()))) &&
            bs1) {
            D3D11_BLEND_DESC1 d1{};
            bs1->GetDesc1(&d1);
            if (d1.RenderTarget[0].LogicOpEnable) return UiBlendShape::kRefused;
        }
    } else if (rtOut) {
        *rtOut = rt;
    }
    return uiLayerBlendShape(rt);
}

// The depth-stencil state bound now, as UiDsState, with its reference and
// the bound view's format. No logging: ui_layer.cpp's dsStateOfNoted adds
// the layer's own once-a-session note on top of this.
inline UiDsState dsStateOf(ID3D11DeviceContext* ctx, ID3D11DepthStencilView* dsv, UINT* refOut,
                           DXGI_FORMAT* viewFmtOut) {
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> dss;
    UINT ref = 0;
    ctx->OMGetDepthStencilState(&dss, &ref);
    D3D11_DEPTH_STENCIL_DESC d{};
    if (dss) dss->GetDesc(&d);
    UINT flags = 0;
    DXGI_FORMAT viewFmt = DXGI_FORMAT_UNKNOWN;
    if (dsv) {
        D3D11_DEPTH_STENCIL_VIEW_DESC vd{};
        dsv->GetDesc(&vd);
        flags = vd.Flags;
        viewFmt = vd.Format;
    }
    if (refOut) *refOut = ref;
    if (viewFmtOut) *viewFmtOut = viewFmt;
    UiDsState s = uiLayerDsStateFrom(dss ? &d : nullptr, flags);
    // Decided here, once per draw: a stencil test against a view with no
    // stencil plane is no test at all (review P3-6).
    s.stencilPlane = viewFmt == DXGI_FORMAT_D32_FLOAT_S8X24_UINT ||
                     viewFmt == DXGI_FORMAT_D24_UNORM_S8_UINT;
    return s;
}

}  // namespace edvr
