// The HUD layer's edge smoothing (docs/design-flat-ui-quality-2026-10-05.md, "2026-10-24: edge smoothing for the HUD layer"): the production SMAA passes of
// src/d3d11/ui_layer_smaa.h -- the build's bytecode, the lookup textures, the metrics, the samplers -- on WARP, over a 64x64 layer in the layer's own
// convention: premultiplied light in rgb, transmittance in alpha, cleared to (0, 0, 0, 1).
//
// What the pass must do to such a layer, and what it must not:
//   (a) a shallow staircase (rise 1 over run 4) of opaque white (rgb 1, a 0) on the clear: the steps take intermediate values in rgb AND in alpha, and the
//       summed opacity (the sum of 1 - a, which is what the composite's box filter would keep) holds within a few percent;
//   (b) the same staircase in dark opaque (rgb 0, a 0): rgb stays 0 and alpha takes intermediate values. That edge shows in alpha alone, so this is the
//       four-channel edge signal; the map's colour-only edge pass over the same layer, run through the same weights and blend, leaves it a staircase;
//   (c) an all-clear layer comes out exactly (0, 0, 0, 1) everywhere;
//   (d) a straight band across the whole layer, horizontal and vertical, comes out unchanged;
// plus the rest of the contract: the compute-stage slots a pass touched come back (the two samplers) or are left null, a size change re-makes the targets, and
// a creation that failed at a size is told once and not retried at that size.

#pragma once

#include "../../src/d3d11/ui_layer_smaa.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace uilayersmaagpu {

struct Px {
    uint8_t r, g, b, a;
};
constexpr uint32_t kSize = 64;

inline std::vector<Px> clearLayer(uint32_t w, uint32_t h) { return std::vector<Px>(size_t(w) * h, Px{0, 0, 0, 255}); }

// Everything on or below the line y = 16 + x / 4 (a step every four columns, one row high) is `fill`; the rest is the clear.
inline std::vector<Px> staircase(uint32_t w, uint32_t h, Px fill) {
    std::vector<Px> px = clearLayer(w, h);
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x)
            if (y >= 16 + x / 4) px[size_t(y) * w + x] = fill;
    return px;
}

// A band across the whole layer: rows y0..y1 (horizontal) or columns x0..x1 (vertical), inclusive.
inline std::vector<Px> band(uint32_t w, uint32_t h, bool horizontal, uint32_t from, uint32_t to, Px fill) {
    std::vector<Px> px = clearLayer(w, h);
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x) {
            const uint32_t c = horizontal ? y : x;
            if (c >= from && c <= to) px[size_t(y) * w + x] = fill;
        }
    return px;
}

// The pass over `in`: ensure, run, and the output read back. Returns false when the pass could not be had.
inline bool smooth(ID3D11Device* device, ID3D11DeviceContext* context, edvr::UiLayerSmaa& s, const std::vector<Px>& in, uint32_t w, uint32_t h,
                   std::vector<Px>& out) {
    ComPtr<ID3D11Texture2D> layer = ::texture(device, w, h, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_SHADER_RESOURCE, in.data(), w * 4);
    if (!layer) return false;
    ComPtr<ID3D11ShaderResourceView> srv = ::view(device, layer.Get());
    if (!srv) return false;
    edvr::UiLayerSmaaFail fail;
    const edvr::UiLayerSmaaState state = edvr::uiLayerSmaaEnsure(s, device, w, h, &fail);
    if (state != edvr::UiLayerSmaaState::kReady && state != edvr::UiLayerSmaaState::kMade) {
        std::printf("  (SMAA layer pass: %s hr=0x%08X)\n", fail.what ? fail.what : "stood down", static_cast<unsigned>(fail.hr));
        return false;
    }
    ID3D11ShaderResourceView* result = edvr::uiLayerSmaaRun(s, context, srv.Get());
    if (!result || result != s.outSrv.Get()) return false;
    std::vector<unsigned char> bytes;
    if (!readWhole(context, s.out.Get(), bytes, 4) || bytes.size() != size_t(w) * h * 4) return false;
    out.resize(size_t(w) * h);
    std::memcpy(out.data(), bytes.data(), bytes.size());
    return true;
}

// Pixels strictly between 0 and 255 in a channel.
inline unsigned between(const std::vector<Px>& px, uint8_t Px::*channel) {
    unsigned n = 0;
    for (const Px& p : px)
        if (p.*channel > 0 && p.*channel < 255) ++n;
    return n;
}
// The layer's opacity, the sum of 1 - a.
inline double opacity(const std::vector<Px>& px) {
    double sum = 0.0;
    for (const Px& p : px) sum += (255.0 - p.a) / 255.0;
    return sum;
}
inline bool same(const std::vector<Px>& a, const std::vector<Px>& b) {
    return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size() * sizeof(Px)) == 0;
}

}  // namespace uilayersmaagpu

inline void uiLayerSmaaGpuTests(ID3D11Device* device, ID3D11DeviceContext* context) {
    using namespace uilayersmaagpu;
    const uint32_t n = kSize;
    const Px white{255, 255, 255, 0}, dark{0, 0, 0, 0};
    std::vector<Px> out;

    {
        edvr::UiLayerSmaa s;
        // (a) the white staircase.
        const std::vector<Px> in = staircase(n, n, white);
        const bool ran = smooth(device, context, s, in, n, n, out);
        check(ran, "HUD layer SMAA: the passes run on a 64x64 layer");
        if (ran) {
            const unsigned rgb = between(out, &Px::r), alpha = between(out, &Px::a);
            const double before = opacity(in), after = opacity(out);
            std::printf("  (white staircase: %u pixels between in r, %u in a; opacity %.2f -> %.2f)\n", rgb, alpha, before, after);
            check(rgb >= 24 && alpha >= 24, "HUD layer SMAA: a shallow white staircase takes intermediate values along its steps, in rgb and in alpha");
            check(std::abs(after - before) <= 0.03 * before, "HUD layer SMAA: the white staircase's summed opacity holds within 3%");
            check(!same(out, in), "HUD layer SMAA: the staircase is changed");
            // The pixels away from the steps are what they were.
            check(out[size_t(0) * n + 0].r == 0 && out[size_t(0) * n + 0].a == 255 && out[size_t(n - 1) * n + (n - 1)].r == 255 &&
                      out[size_t(n - 1) * n + (n - 1)].a == 0,
                  "HUD layer SMAA: the clear and the opaque fill, away from the steps, are as they were");
        }

        // (b) the dark staircase: the edge is in alpha alone.
        const std::vector<Px> darkIn = staircase(n, n, dark);
        const bool darkRan = smooth(device, context, s, darkIn, n, n, out);
        check(darkRan, "HUD layer SMAA: the passes run on the dark staircase");
        if (darkRan) {
            const unsigned alpha = between(out, &Px::a);
            unsigned colour = 0;
            for (const Px& p : out) colour += p.r + p.g + p.b;
            const double before = opacity(darkIn), after = opacity(out);
            std::printf("  (dark staircase: %u pixels between in a; rgb sum %u; opacity %.2f -> %.2f)\n", alpha, colour, before, after);
            check(alpha >= 24, "HUD layer SMAA: a dark opaque staircase takes intermediate alpha along its steps (the four-channel edge signal)");
            check(colour == 0, "HUD layer SMAA: the dark staircase's rgb stays 0");
            check(std::abs(after - before) <= 0.03 * before, "HUD layer SMAA: the dark staircase's summed opacity holds within 3%");

            // The control: the map's colour-only edge pass in the edge pass's place. Dark against the clear is no edge in rgb, so the same weights and
            // blend leave the layer as it was. This is what the four-channel pass is for.
            ComPtr<ID3D11ComputeShader> colourOnly;
            check(SUCCEEDED(device->CreateComputeShader(edvr::kFlatMapSmaaEdgesBytecode, sizeof(edvr::kFlatMapSmaaEdgesBytecode), nullptr,
                                                        colourOnly.GetAddressOf())),
                  "HUD layer SMAA: the map's colour-only edge shader is made for the control");
            if (colourOnly) {
                edvr::UiLayerSmaa control;
                edvr::UiLayerSmaaFail fail;
                check(edvr::uiLayerSmaaEnsure(control, device, n, n, &fail) == edvr::UiLayerSmaaState::kMade, "HUD layer SMAA: the control's targets are made");
                control.edgesCs = colourOnly;
                std::vector<Px> controlOut;
                ComPtr<ID3D11Texture2D> layer = ::texture(device, n, n, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_SHADER_RESOURCE, darkIn.data(), n * 4);
                ComPtr<ID3D11ShaderResourceView> srv = ::view(device, layer.Get());
                std::vector<unsigned char> bytes;
                if (edvr::uiLayerSmaaRun(control, context, srv.Get()) && readWhole(context, control.out.Get(), bytes, 4)) {
                    controlOut.resize(size_t(n) * n);
                    std::memcpy(controlOut.data(), bytes.data(), bytes.size());
                }
                check(same(controlOut, darkIn), "HUD layer SMAA: control, the colour-only edge pass leaves the dark staircase untouched (so the alpha channel in the edge test is what smooths it)");
            }
        }

        // (c) the all-clear layer, through the same resources.
        const std::vector<Px> clear = clearLayer(n, n);
        const bool clearRan = smooth(device, context, s, clear, n, n, out);
        check(clearRan, "HUD layer SMAA: the passes run on an all-clear layer");
        if (clearRan) {
            bool exact = true;
            for (const Px& p : out) exact = exact && p.r == 0 && p.g == 0 && p.b == 0 && p.a == 255;
            check(exact, "HUD layer SMAA: an all-clear layer comes out exactly (0, 0, 0, 1) everywhere");
        }

        // (d) straight bands across the whole layer.
        for (int pass = 0; pass < 4; ++pass) {
            const bool horizontal = (pass & 1) == 0;
            const Px fill = pass < 2 ? white : dark;
            const std::vector<Px> bandIn = band(n, n, horizontal, 24, 39, fill);
            const bool ok = smooth(device, context, s, bandIn, n, n, out);
            check(ok && same(out, bandIn),
                  pass == 0   ? "HUD layer SMAA: a full-width horizontal white band comes out unchanged"
                  : pass == 1 ? "HUD layer SMAA: a full-height vertical white band comes out unchanged"
                  : pass == 2 ? "HUD layer SMAA: a full-width horizontal dark band comes out unchanged"
                              : "HUD layer SMAA: a full-height vertical dark band comes out unchanged");
        }
    }

    // The compute stage: the two samplers come back, the resource slots and the unordered-access slot a pass used are left empty.
    {
        edvr::UiLayerSmaa s;
        D3D11_SAMPLER_DESC sd{};
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
        sd.MaxLOD = D3D11_FLOAT32_MAX;
        ComPtr<ID3D11SamplerState> marker0, marker1;
        check(SUCCEEDED(device->CreateSamplerState(&sd, marker0.GetAddressOf())), "HUD layer SMAA: the first marker sampler is made");
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_MIRROR;
        check(SUCCEEDED(device->CreateSamplerState(&sd, marker1.GetAddressOf())), "HUD layer SMAA: the second marker sampler is made");
        ID3D11SamplerState* markers[2] = {marker0.Get(), marker1.Get()};
        context->CSSetSamplers(0, 2, markers);
        const bool ran = smooth(device, context, s, staircase(n, n, white), n, n, out);
        check(ran, "HUD layer SMAA: the passes run with the caller's samplers bound");
        ID3D11SamplerState* after[2] = {};
        context->CSGetSamplers(0, 2, after);
        check(after[0] == marker0.Get() && after[1] == marker1.Get(), "HUD layer SMAA: the caller's samplers on s0 and s1 are put back");
        for (ID3D11SamplerState* sampler : after)
            if (sampler) sampler->Release();
        ID3D11ShaderResourceView* srvs[3] = {};
        ID3D11UnorderedAccessView* uav = nullptr;
        context->CSGetShaderResources(0, 3, srvs);
        context->CSGetUnorderedAccessViews(0, 1, &uav);
        check(!srvs[0] && !srvs[1] && !srvs[2] && !uav, "HUD layer SMAA: no resource view and no unordered-access view is left bound on the compute stage");
        for (ID3D11ShaderResourceView* view : srvs)
            if (view) view->Release();
        if (uav) uav->Release();
        ID3D11SamplerState* none[2] = {};
        context->CSSetSamplers(0, 2, none);
    }

    // The sizes: the targets follow the layer, the same size is left alone, and what failed at a size is told once and not tried again there.
    {
        edvr::UiLayerSmaa s;
        edvr::UiLayerSmaaFail fail;
        check(edvr::uiLayerSmaaEnsure(s, device, 64, 64, &fail) == edvr::UiLayerSmaaState::kMade && s.w == 64 && s.h == 64, "HUD layer SMAA: the first use makes the targets");
        ID3D11Texture2D* first = s.out.Get();
        check(edvr::uiLayerSmaaEnsure(s, device, 64, 64, &fail) == edvr::UiLayerSmaaState::kReady && s.out.Get() == first,
              "HUD layer SMAA: the same size leaves the targets as they are");
        check(edvr::uiLayerSmaaEnsure(s, device, 48, 40, &fail) == edvr::UiLayerSmaaState::kMade && s.w == 48 && s.h == 40, "HUD layer SMAA: a new size re-makes the targets");
        D3D11_TEXTURE2D_DESC od{}, ed{}, wd{};
        s.out->GetDesc(&od);
        s.edges->GetDesc(&ed);
        s.weights->GetDesc(&wd);
        check(od.Width == 48 && od.Height == 40 && od.Format == DXGI_FORMAT_R8G8B8A8_UNORM && ed.Width == 48 && ed.Height == 40 &&
                  ed.Format == DXGI_FORMAT_R8G8_UNORM && wd.Width == 48 && wd.Height == 40 && wd.Format == DXGI_FORMAT_R8G8B8A8_UNORM,
              "HUD layer SMAA: the output and weights are R8G8B8A8 and the edges R8G8, all at the layer's size");
        s.releaseTargets();
        check(!s.ready() && !s.out && !s.edges && !s.weights && !s.metrics, "HUD layer SMAA: releasing the targets leaves none");
        check(s.edgesCs && s.weightsCs && s.blendCs && s.areaSrv && s.searchSrv && s.linear && s.point, "HUD layer SMAA: the passes, samplers and lookups stay with the device");
        s.releaseAll();
        check(!s.edgesCs && !s.weightsCs && !s.blendCs && !s.areaSrv && !s.searchSrv && !s.linear && !s.point && !s.area && !s.search, "HUD layer SMAA: releasing all leaves nothing");

        // A creation that fails (no device to make it on) is told once for the size, and not tried again there, even with a device.
        edvr::UiLayerSmaaFail told;
        check(edvr::uiLayerSmaaEnsure(s, nullptr, 96, 96, &told) == edvr::UiLayerSmaaState::kFailed && told.what && FAILED(told.hr),
              "HUD layer SMAA: a creation that fails at a size answers kFailed with what failed and the HRESULT");
        edvr::UiLayerSmaaFail again;
        check(edvr::uiLayerSmaaEnsure(s, device, 96, 96, &again) == edvr::UiLayerSmaaState::kStoodDown && !again.what && again.hr == S_OK && !s.ready(),
              "HUD layer SMAA: the next call at that size stands down without trying again and without saying it twice");
        s.releaseTargets();
        check(edvr::uiLayerSmaaEnsure(s, device, 96, 96, &again) == edvr::UiLayerSmaaState::kStoodDown,
              "HUD layer SMAA: releasing the layer's targets does not clear what failed at a size");
        check(edvr::uiLayerSmaaEnsure(s, device, 80, 80, &again) == edvr::UiLayerSmaaState::kMade && s.ready(), "HUD layer SMAA: another size tries again and succeeds");
        s.releaseAll();
        check(edvr::uiLayerSmaaEnsure(s, device, 96, 96, &again) == edvr::UiLayerSmaaState::kMade,
              "HUD layer SMAA: a device change (release all) clears what failed, so the size is tried afresh");
    }
}
