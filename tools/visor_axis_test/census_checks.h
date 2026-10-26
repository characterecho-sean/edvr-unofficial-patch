// F121b: the census compute shader (src\d3d11\visor_census_shader.h, the build's bytecode) against visorcensus::cpuModel and the decode, on WARP. Included by
// visor_axis_test.cpp after its check(), hr() and the D3D headers; takes the shader bytes so the rig runs the build's and a desk run can compile the text.
//   V9a  a mixed R11G11B10 image (a base colour, one Inf texel, one NaN texel and a black one on tapped pixels): the shader's words decode to the CPU model's counts, box,
//        largest value and (within 1/512) mean luma; the image size is not a multiple of the tap grid;
//   V9b  an all-black image: every tap black, mean 0, none non-finite;   V9c an all-NaN image: every tap non-finite and NaN, none black;
//   V9d  the exposure texels: read when the view is readable (finite, and a NaN texel read as NaN), not read when the dispatch says so;
//   V9e  the decode refuses another dispatch's words (the sequence, the width, the height) and words that cannot be a census.
#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

#include "visor_census_core.h"

namespace censuschecks {

namespace vc = edvr::visorcensus;
using Microsoft::WRL::ComPtr;

// R11G11B10_FLOAT patterns. 11-bit float: 5 exponent bits (bias 15), 6 mantissa bits; 10-bit: 5 and 5. Exact for the values the checks use.
constexpr uint32_t kR_half = 0x380, kG_quarter = 0x340, kB_two = 0x200;      // 0.5, 0.25, 2.0
constexpr uint32_t kG_inf = 0x7C0, kB_nan = 0x3E1;                            // Inf (11-bit), NaN (10-bit)
inline uint32_t pack(uint32_t r, uint32_t g, uint32_t b) { return r | (g << 11) | (b << 22); }

struct Px { uint32_t packed; float rgb[3]; };
inline Px base() { return {pack(kR_half, kG_quarter, kB_two), {0.5f, 0.25f, 2.0f}}; }
inline Px black() { return {0u, {0.0f, 0.0f, 0.0f}}; }
inline Px infG() { return {pack(kR_half, kG_inf, kB_two), {0.5f, std::numeric_limits<float>::infinity(), 2.0f}}; }
inline Px nanB() { return {pack(kR_half, kG_quarter, kB_nan), {0.5f, 0.25f, std::numeric_limits<float>::quiet_NaN()}}; }

struct Image {
    uint32_t w, h;
    std::vector<uint32_t> packed;
    std::vector<float> rgb;
    Image(uint32_t w_, uint32_t h_, const Px& fill) : w(w_), h(h_), packed(static_cast<size_t>(w_) * h_, fill.packed), rgb(static_cast<size_t>(w_) * h_ * 3) {
        for (size_t i = 0; i < packed.size(); ++i) std::memcpy(&rgb[i * 3], fill.rgb, sizeof(fill.rgb));
    }
    void set(uint32_t x, uint32_t y, const Px& p) {
        packed[static_cast<size_t>(y) * w + x] = p.packed;
        std::memcpy(&rgb[(static_cast<size_t>(y) * w + x) * 3], p.rgb, sizeof(p.rgb));
    }
};

struct Gpu {
    ID3D11Device* dev;
    ID3D11DeviceContext* ctx;
    ComPtr<ID3D11ComputeShader> cs;
    ComPtr<ID3D11Buffer> out, stage, cb;
    ComPtr<ID3D11UnorderedAccessView> uav;
};

inline bool makeGpu(Gpu& g, const void* bytes, size_t len) {
    if (FAILED(g.dev->CreateComputeShader(bytes, len, nullptr, &g.cs))) return false;
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = vc::kWords * 4u; bd.Usage = D3D11_USAGE_DEFAULT; bd.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED; bd.StructureByteStride = 4;
    D3D11_UNORDERED_ACCESS_VIEW_DESC ud{}; ud.Format = DXGI_FORMAT_UNKNOWN; ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER; ud.Buffer.NumElements = vc::kWords;
    D3D11_BUFFER_DESC sd{}; sd.ByteWidth = vc::kWords * 4u; sd.Usage = D3D11_USAGE_STAGING; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    D3D11_BUFFER_DESC cd{}; cd.ByteWidth = 16; cd.Usage = D3D11_USAGE_DYNAMIC; cd.BindFlags = D3D11_BIND_CONSTANT_BUFFER; cd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    return SUCCEEDED(g.dev->CreateBuffer(&bd, nullptr, &g.out)) && SUCCEEDED(g.dev->CreateUnorderedAccessView(g.out.Get(), &ud, &g.uav)) &&
           SUCCEEDED(g.dev->CreateBuffer(&sd, nullptr, &g.stage)) && SUCCEEDED(g.dev->CreateBuffer(&cd, nullptr, &g.cb));
}

// The production sequence of remlok_fix.cpp's censusRun, minus the game's state: clear the output, write the constants, bind, dispatch 8 x 8 groups, copy to the staging
// buffer, read it. Returns false on a D3D failure.
inline bool run(Gpu& g, ID3D11ShaderResourceView* hdr, ID3D11ShaderResourceView* expo, uint32_t w, uint32_t h, uint32_t seq, uint32_t* words) {
    const UINT zero[4] = {0, 0, 0, 0};
    g.ctx->ClearUnorderedAccessViewUint(g.uav.Get(), zero);
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(g.ctx->Map(g.cb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return false;
    const uint32_t cb[4] = {w, h, seq, expo ? 1u : 0u};
    std::memcpy(m.pData, cb, sizeof(cb));
    g.ctx->Unmap(g.cb.Get(), 0);
    g.ctx->CSSetShader(g.cs.Get(), nullptr, 0);
    g.ctx->CSSetConstantBuffers(0, 1, g.cb.GetAddressOf());
    ID3D11ShaderResourceView* views[2] = {hdr, expo};
    g.ctx->CSSetShaderResources(0, 2, views);
    ID3D11UnorderedAccessView* uavs[1] = {g.uav.Get()};
    g.ctx->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
    g.ctx->Dispatch(vc::kThreads / 8u, vc::kThreads / 8u, 1);
    ID3D11UnorderedAccessView* nul[1] = {nullptr};
    g.ctx->CSSetUnorderedAccessViews(0, 1, nul, nullptr);
    ID3D11ShaderResourceView* nulv[2] = {nullptr, nullptr};
    g.ctx->CSSetShaderResources(0, 2, nulv);
    g.ctx->CopyResource(g.stage.Get(), g.out.Get());
    if (FAILED(g.ctx->Map(g.stage.Get(), 0, D3D11_MAP_READ, 0, &m))) return false;
    std::memcpy(words, m.pData, vc::kWords * 4u);
    g.ctx->Unmap(g.stage.Get(), 0);
    return true;
}

inline ComPtr<ID3D11ShaderResourceView> imageView(Gpu& g, const Image& im) {
    D3D11_TEXTURE2D_DESC d{}; d.Width = im.w; d.Height = im.h; d.MipLevels = 1; d.ArraySize = 1; d.Format = DXGI_FORMAT_R11G11B10_FLOAT; d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA sd{im.packed.data(), im.w * 4u, 0};
    ComPtr<ID3D11Texture2D> t; ComPtr<ID3D11ShaderResourceView> v;
    if (SUCCEEDED(g.dev->CreateTexture2D(&d, &sd, &t))) g.dev->CreateShaderResourceView(t.Get(), nullptr, &v);
    return v;
}
inline ComPtr<ID3D11ShaderResourceView> expoView(Gpu& g, float e0, float e1) {
    const float texels[6] = {e0, e1, 1.0f, 1.0f, 1.0f, 1.0f};
    D3D11_TEXTURE2D_DESC d{}; d.Width = 6; d.Height = 1; d.MipLevels = 1; d.ArraySize = 1; d.Format = DXGI_FORMAT_R32_FLOAT; d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA sd{texels, 6 * 4u, 0};
    ComPtr<ID3D11Texture2D> t; ComPtr<ID3D11ShaderResourceView> v;
    if (SUCCEEDED(g.dev->CreateTexture2D(&d, &sd, &t))) g.dev->CreateShaderResourceView(t.Get(), nullptr, &v);
    return v;
}

inline bool sameCensus(const vc::Census& a, const vc::Census& b) {
    const bool maxEq = a.maxFinite == b.maxFinite;
    return a.nonFinite == b.nonFinite && a.nan == b.nan && a.inf == b.inf && a.black == b.black && maxEq && std::fabs(a.meanLuma - b.meanLuma) < 0.002 &&
           (!a.nonFinite || (a.minX == b.minX && a.minY == b.minY && a.maxX == b.maxX && a.maxY == b.maxY));
}

template <class Check>
inline void census(ID3D11Device* dev, ID3D11DeviceContext* ctx, const void* bytes, size_t len, Check check) {
    Gpu g{dev, ctx, {}, {}, {}, {}, {}};
    const bool made = makeGpu(g, bytes, len);
    check(made, "V9.the-census-shader-and-its-buffers-are-made");
    if (!made) return;

    // V9a: a mixed image whose size is not a multiple of the tap grid. The tapped pixels (tap s -> ((2 s + 1) extent) / 512): x 1 and 638 of 640, y 0 and 359 of 360.
    {
        Image im(640, 360, base());
        im.set(vc::tapPixel(0, 640), vc::tapPixel(0, 360), infG());
        im.set(vc::tapPixel(255, 640), vc::tapPixel(255, 360), nanB());
        im.set(vc::tapPixel(100, 640), vc::tapPixel(50, 360), black());
        check(vc::tapPixel(0, 640) == 1 && vc::tapPixel(255, 640) == 638 && vc::tapPixel(255, 360) == 359 && vc::tapPixel(0, 360) == 0,
              "V9a.the-tap-positions-are-the-ones-the-comment-says");
        uint32_t words[vc::kWords];
        auto v = imageView(g, im);
        check(v && run(g, v.Get(), nullptr, 640, 360, 7, words), "V9a.the-dispatch-runs");
        vc::Census got, want = vc::cpuModel(im.rgb.data(), 640, 360, false, 0, 0);
        const bool dec = vc::decode(words, 7, 640, 360, &got);
        check(dec, "V9a.the-words-decode");
        check(want.nonFinite == 2 && want.nan == 1 && want.inf == 1 && want.black >= 1, "V9a.the-cpu-model-sees-two-non-finite-taps-and-a-black-one");
        check(dec && sameCensus(got, want), "V9a.the-shader's-census-equals-the-cpu-model (counts, box, largest value, mean luma)");
        check(dec && got.nonFinite == 2 && got.nan == 1 && got.inf == 1 && got.minX == 1 && got.minY == 0 && got.maxX == 638 && got.maxY == 359 && got.maxFinite == 2.0f,
              "V9a.two-non-finite-taps-one-NaN-one-Inf-boxed-from-(1,0)-to-(638,359)-and-the-largest-finite-value-is-2");
        check(dec && !got.expoValid, "V9a.the-exposure-is-not-read-when-the-dispatch-says-so");
        // the mean: all base taps, but three that are not
        const double lum = 0.5 * 0.2126 + 0.25 * 0.7152 + 2.0 * 0.0722;
        check(dec && std::fabs(got.meanLuma - lum * (vc::kSamples - 3) / vc::kSamples) < 0.002, "V9a.the-mean-luma-is-the-base-colour's-less-the-three-taps");
    }

    // V9b / V9c: all black, all NaN.
    {
        uint32_t words[vc::kWords];
        Image dark(300, 200, black());
        auto v = imageView(g, dark);
        vc::Census got;
        const bool ran = v && run(g, v.Get(), nullptr, 300, 200, 1, words) && vc::decode(words, 1, 300, 200, &got);
        check(ran && got.black == vc::kSamples && got.nonFinite == 0 && got.meanLuma == 0.0 && got.maxFinite == 0.0f, "V9b.an-all-black-image-is-65536-black-taps-and-nothing-else");
        Image bad(300, 200, nanB());
        auto v2 = imageView(g, bad);
        const bool ran2 = v2 && run(g, v2.Get(), nullptr, 300, 200, 2, words) && vc::decode(words, 2, 300, 200, &got);
        check(ran2 && got.nonFinite == vc::kSamples && got.nan == vc::kSamples && got.inf == 0 && got.black == 0 && got.minX == 0 && got.maxX == 299 && got.minY == 0 && got.maxY == 199,
              "V9c.an-all-NaN-image-is-every-tap-non-finite-all-NaN-none-black-boxed-over-the-whole-image");
    }

    // V9d: the exposure texels.
    {
        uint32_t words[vc::kWords];
        Image im(64, 64, base());
        auto v = imageView(g, im);
        auto e = expoView(g, 1.5f, 8.0f);
        vc::Census got;
        bool ran = v && e && run(g, v.Get(), e.Get(), 64, 64, 3, words) && vc::decode(words, 3, 64, 64, &got);
        check(ran && got.expoValid && got.expo0 == 1.5f && got.expo1 == 8.0f, "V9d.the-first-two-exposure-texels-are-read-(1.5-and-8)");
        auto en = expoView(g, std::numeric_limits<float>::quiet_NaN(), 2.0f);
        ran = v && en && run(g, v.Get(), en.Get(), 64, 64, 4, words) && vc::decode(words, 4, 64, 64, &got);
        check(ran && got.expoValid && vc::bitsNonFinite(vc::floatToBits(got.expo0)) && got.expo1 == 2.0f, "V9d.a-NaN-exposure-texel-is-read-as-non-finite");
        vc::Window w;
        w.add(0, got);
        check(w.expoRead == 1 && w.expoNonFinite == 1, "V9d.the-window-counts-the-non-finite-exposure");
    }

    // V9e: the decode's refusals.
    {
        uint32_t words[vc::kWords];
        Image im(64, 64, base());
        auto v = imageView(g, im);
        vc::Census got;
        const bool ran = v && run(g, v.Get(), nullptr, 64, 64, 9, words);
        check(ran && vc::decode(words, 9, 64, 64, &got), "V9e.the-matching-dispatch-decodes");
        check(!vc::decode(words, 10, 64, 64, &got) && !vc::decode(words, 9, 65, 64, &got) && !vc::decode(words, 9, 64, 65, &got), "V9e.another-sequence-width-or-height-is-refused");
        uint32_t bad[vc::kWords];
        std::memcpy(bad, words, sizeof(bad));
        bad[vc::kNan] = 5;   // NaN + Inf != non-finite
        check(!vc::decode(bad, 9, 64, 64, &got), "V9e.counts-that-do-not-add-up-are-refused");
        std::memcpy(bad, words, sizeof(bad));
        bad[vc::kBlack] = vc::kSamples + 1;
        check(!vc::decode(bad, 9, 64, 64, &got), "V9e.more-black-taps-than-taps-is-refused");
        check(!vc::decode(nullptr, 9, 64, 64, &got) && !vc::decode(words, 9, 64, 64, nullptr), "V9e.null-arguments-are-refused");
    }
}

}  // namespace censuschecks
