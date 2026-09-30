// The game's HDR -> display tonemap draw, recognised STRUCTURALLY, for the
// crisp-HUD half of fix.ui_quality's tonemap re-issue (ui_layer.cpp, Phase 1),
// which re-issues the admitted draw once per eye with the HDR HUD layer in
// the HDR source slot -- and must therefore never guess which slot that is.
// (It was shared with the HUD layer census, Phase 0 of
// docs/cockpit-hud-layer-design-2026-09-27.md, removed 2026-09-29; "the
// census's G-B" below is that census's flight.)
//
// The structure (eye_tonemap_snapshot.h:170-191's checks, flight-verified in
// the census's G-B): a 3-vertex, 1-instance draw (the caller prefilters
// kind/count), ONE 2D render target at mip 0 with NO depth-stencil view, the
// blend's render-target-0 blend disabled, and PS b2 at least 256 bytes (272
// measured). The full tonemap adds the measured SRV shape: VS t0 the scalar
// exposure (R32), PS t0 the colour LUT as a 3D view, and the HDR source a 2D
// view at the per-PS slot the table below names. The EDHM swap (vs
// 642017A6FEDAE0E8, same PS) binds NO exposure at VS t0 (flight 1, G-B), so
// the exposure's presence is reported, never required, and a re-issue binds
// whatever the admitted draw itself holds.
//
// Header-only and free of Log/Config: both callers decide what a result
// means and how to say it.
#pragma once

#include <d3d11.h>
#include <wrl/client.h>

#include <cstdint>

namespace edvr {

namespace tone_admit_detail {
template <class T>
using Ptr = Microsoft::WRL::ComPtr<T>;
}  // namespace tone_admit_detail

// The measured pair (eye_tonemap_snapshot.h:139) and the EDHM swap
// (docs/edhm-black-cockpit-2026-09-15.md:161), flight-verified by the census.
constexpr uint64_t kToneVsMeasured = 0x2D78DC3FD2C0C543ull;
constexpr uint64_t kTonePsMeasured = 0x99C21CEB7A699821ull;
constexpr uint64_t kToneVsEdhm = 0x642017A6FEDAE0E8ull;

// The PS SRV slot that carries the HDR source, per admitted tone pixel
// shader. The measured pair's ps reads it at t1 (G-B, three flights; the EDHM
// swap keeps the same PS and slot). An unknown PS gets NO slot: the census
// still reads t1 so its variant line describes the measured shape, but a
// re-issue declines rather than guess.
struct TonePsEntry {
    uint64_t ps;
    int hdrSlot;
};
constexpr TonePsEntry kTonePsTable[] = {{kTonePsMeasured, 1}};

inline int tonemapHdrSlot(uint64_t ps) {
    for (const TonePsEntry& e : kTonePsTable)
        if (e.ps == ps) return e.hdrSlot;
    return -1;
}

struct ToneAdmit {
    // The output.
    void* rtvRes = nullptr;
    uint32_t rtvW = 0, rtvH = 0;
    DXGI_FORMAT rtvFmt = DXGI_FORMAT_UNKNOWN;
    // PS b2's size (the tonemap's constants; 272 measured).
    uint32_t b2 = 0;
    // VS t0: the scalar exposure (R32). Null in the EDHM swap -- reported,
    // never required.
    void* vsT0Res = nullptr;
    // PS t0: the colour LUT, a 3D view in the measured variant.
    void* psT0Res = nullptr;
    bool psT0Is3D = false;
    // The HDR source: the slot the ps table names (hdrSlot; -1 when the ps is
    // not in the table), the slot actually read (hdrSlotRead -- t1 when the
    // ps is unknown, so the census's line still describes the measured
    // shape), what is bound there, and whether it is a 2D view.
    int hdrSlot = -1;
    int hdrSlotRead = -1;
    void* hdrRes = nullptr;
    uint32_t hdrW = 0, hdrH = 0;
    DXGI_FORMAT hdrFmt = DXGI_FORMAT_UNKNOWN;
    bool hdr2D = false;
    // The census's "full match": exposure present, the LUT a 3D view, the HDR
    // source a 2D view. The EDHM swap is deliberately not full.
    bool full = false;
};

// The structure test plus the SRV shape read. ps is the bound pixel shader's
// hash, for the slot table. True = the STRUCTURE matched (a tonemap-shaped
// draw); out.full and the SRV fields say how far beyond that it went.
inline bool tonemapAdmitStructure(ID3D11DeviceContext* ctx, uint64_t ps, ToneAdmit& out) {
    using tone_admit_detail::Ptr;
    ID3D11RenderTargetView* rtvs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
    Ptr<ID3D11DepthStencilView> dsv;
    ctx->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtvs, &dsv);
    bool one = rtvs[0] != nullptr && dsv == nullptr;
    for (uint32_t i = 1; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i) one = one && !rtvs[i];
    Ptr<ID3D11RenderTargetView> rt;
    rt.Attach(rtvs[0]);
    for (uint32_t i = 1; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i)
        if (rtvs[i]) rtvs[i]->Release();
    if (!one || !rt) return false;
    D3D11_RENDER_TARGET_VIEW_DESC rd{};
    rt->GetDesc(&rd);
    if (rd.ViewDimension != D3D11_RTV_DIMENSION_TEXTURE2D || rd.Texture2D.MipSlice) return false;
    Ptr<ID3D11BlendState> blend;
    FLOAT factors[4]{};
    UINT mask = 0;
    ctx->OMGetBlendState(&blend, factors, &mask);
    if (blend) {
        D3D11_BLEND_DESC bd{};
        blend->GetDesc(&bd);
        if (bd.RenderTarget[0].BlendEnable) return false;
    }
    Ptr<ID3D11Buffer> cb;
    ctx->PSGetConstantBuffers(2, 1, &cb);
    if (!cb) return false;
    D3D11_BUFFER_DESC cbd{};
    cb->GetDesc(&cbd);
    if (cbd.ByteWidth < 256) return false;
    out.b2 = cbd.ByteWidth;
    out.rtvFmt = rd.Format;
    Ptr<ID3D11Resource> res;
    rt->GetResource(&res);
    if (res) {
        out.rtvRes = res.Get();
        Ptr<ID3D11Texture2D> t2;
        if (SUCCEEDED(res->QueryInterface(IID_PPV_ARGS(&t2)))) {
            D3D11_TEXTURE2D_DESC td{};
            t2->GetDesc(&td);
            out.rtvW = td.Width;
            out.rtvH = td.Height;
        }
    }
    Ptr<ID3D11ShaderResourceView> vsT0, psT0;
    ctx->VSGetShaderResources(0, 1, &vsT0);
    ctx->PSGetShaderResources(0, 1, &psT0);
    if (vsT0) {
        Ptr<ID3D11Resource> r;
        vsT0->GetResource(&r);
        out.vsT0Res = r.Get();
    }
    if (psT0) {
        D3D11_SHADER_RESOURCE_VIEW_DESC vd{};
        psT0->GetDesc(&vd);
        out.psT0Is3D = vd.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE3D;
        Ptr<ID3D11Resource> r;
        psT0->GetResource(&r);
        out.psT0Res = r.Get();
    }
    out.hdrSlot = tonemapHdrSlot(ps);
    const int slot = out.hdrSlot >= 0 ? out.hdrSlot : 1;  // unknown ps: describe t1
    Ptr<ID3D11ShaderResourceView> hdr;
    ctx->PSGetShaderResources(static_cast<UINT>(slot), 1, &hdr);
    if (hdr) {
        out.hdrSlotRead = slot;
        D3D11_SHADER_RESOURCE_VIEW_DESC vd{};
        hdr->GetDesc(&vd);
        out.hdrFmt = vd.Format;
        Ptr<ID3D11Resource> r;
        hdr->GetResource(&r);
        out.hdrRes = r.Get();
        out.hdr2D = vd.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2D;
        if (r && out.hdr2D) {
            Ptr<ID3D11Texture2D> t2;
            if (SUCCEEDED(r->QueryInterface(IID_PPV_ARGS(&t2)))) {
                D3D11_TEXTURE2D_DESC td{};
                t2->GetDesc(&td);
                out.hdrW = td.Width;
                out.hdrH = td.Height;
            }
        }
    }
    out.full = out.vsT0Res != nullptr && out.psT0Is3D && out.hdrRes != nullptr && out.hdr2D;
    return true;
}

// Is the admitted draw genuinely THE tonemap, not merely tonemap-shaped
// (review R1: the census measures hundreds-thousands of structure matches a
// window that fail this -- SMAA and the post passes after the tonemap among
// them)? The full SRV shape: exposure bound at VS t0 -- or its KNOWN absence
// in the EDHM swap (vs kToneVsEdhm binds none, flight 1's G-B) -- PS t0 a 3D
// LUT view, and a 2D HDR source view. ui_layer.cpp's crisp path gates its
// failed-consumer stand-down on this; the census's "full match" lines keep
// ToneAdmit::full's stricter reading (the EDHM swap stays not-full there).
inline bool tonemapAdmitFull(const ToneAdmit& t, uint64_t vs) {
    const bool exposure = t.vsT0Res != nullptr || vs == kToneVsEdhm;
    return exposure && t.psT0Is3D && t.hdrRes != nullptr && t.hdr2D;
}

}  // namespace edvr
