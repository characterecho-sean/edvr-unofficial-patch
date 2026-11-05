#pragma once
// The depth-of-field blur's average on WARP: the production shader over frames that differ the way the jittered blur does, a few very bright ones among dim ones.
//   1  the mean of eight frames, and it does not depend on which slot holds which frame: over a whole jitter cycle a still scene's blur comes out the same every frame
//   2  three frames bound and five slots empty: the mean of the three, not dimmed by the empty slots
//   3  a size that is not a multiple of the thread group: the last row and column are written, each texel from its own position
//   4  R11G11B10 frames, which have no alpha, keep the alpha at 1
#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>
#include "temporal_shader_bytecode.h"

inline int flatDofBlurGpuTests(ID3D11Device* device, ID3D11DeviceContext* context) {
    using Microsoft::WRL::ComPtr;
    int failures = 0;
    auto check = [&](bool ok, const char* why) {
        if (!ok) {
            std::printf("FAIL: dof blur GPU %s\n", why);
            ++failures;
        }
    };
    ComPtr<ID3D11ComputeShader> cs;
    check(SUCCEEDED(device->CreateComputeShader(edvr::kFlatDofBlurBytecode, sizeof(edvr::kFlatDofBlurBytecode), nullptr, &cs)), "shader creates");
    if (!cs) return failures;

    const UINT w = 20, h = 12;
    // The red of each frame: dim, with the two bright phases a glint gives.
    const float levels[8] = {0.1f, 0.1f, 40.0f, 0.1f, 3.0f, 0.1f, 0.1f, 0.2f};
    auto makeFrame = [&](float level, ComPtr<ID3D11ShaderResourceView>* srv) {
        std::vector<float> texels(size_t(w) * h * 4);
        std::vector<uint16_t> half(texels.size());
        const auto toHalf = [](float v) {
            // Plain positive values only.
            if (v <= 0.0f) return uint16_t(0);
            int e = 0;
            const float m = std::frexp(v, &e);
            const int exponent = e + 14;
            const int mantissa = int((m * 2.0f - 1.0f) * 1024.0f + 0.5f);
            return uint16_t((exponent << 10) + mantissa);
        };
        for (UINT y = 0; y < h; ++y)
            for (UINT x = 0; x < w; ++x) {
                const size_t at = (size_t(y) * w + x) * 4;
                half[at] = toHalf(level + 0.25f * float(x));
                half[at + 1] = toHalf(1.0f + 0.5f * float(y));
                half[at + 2] = toHalf(level * 0.5f);
                half[at + 3] = toHalf(0.5f);
            }
        D3D11_TEXTURE2D_DESC d{};
        d.Width = w;
        d.Height = h;
        d.MipLevels = 1;
        d.ArraySize = 1;
        d.Format = DXGI_FORMAT_R16G16B16A16_TYPELESS;  // as the game's blur
        d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA init{};
        init.pSysMem = half.data();
        init.SysMemPitch = w * 8;
        ComPtr<ID3D11Texture2D> t;
        if (FAILED(device->CreateTexture2D(&d, &init, &t))) return false;
        D3D11_SHADER_RESOURCE_VIEW_DESC v{};
        v.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        v.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        v.Texture2D.MipLevels = 1;
        return SUCCEEDED(device->CreateShaderResourceView(t.Get(), &v, srv->GetAddressOf()));
    };
    ComPtr<ID3D11ShaderResourceView> frames[8];
    bool made = true;
    for (int i = 0; i < 8; ++i) made = makeFrame(levels[i], &frames[i]) && made;
    check(made, "frames create");
    if (!made) return failures;

    D3D11_TEXTURE2D_DESC od{};
    od.Width = w;
    od.Height = h;
    od.MipLevels = 1;
    od.ArraySize = 1;
    od.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    od.SampleDesc.Count = 1;
    od.Usage = D3D11_USAGE_DEFAULT;
    od.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> out, staging;
    ComPtr<ID3D11UnorderedAccessView> uav;
    bool ok = SUCCEEDED(device->CreateTexture2D(&od, nullptr, &out)) && SUCCEEDED(device->CreateUnorderedAccessView(out.Get(), nullptr, &uav));
    od.Usage = D3D11_USAGE_STAGING;
    od.BindFlags = 0;
    od.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ok = ok && SUCCEEDED(device->CreateTexture2D(&od, nullptr, &staging));
    check(ok, "output creates");
    if (!ok) return failures;

    const auto fromHalf = [](uint16_t b) {
        const int e = (b >> 10) & 31, m = b & 1023;
        if (e == 0) return float(m) * std::ldexp(1.0f, -24);
        return (1.0f + float(m) / 1024.0f) * std::ldexp(1.0f, e - 15);
    };
    // One run of the production shader, the way flat_dof_blur.cpp binds it. The result is every texel's RGBA.
    auto run = [&](const std::vector<ID3D11ShaderResourceView*>& bound, float weight, ID3D11UnorderedAccessView* target, ID3D11Texture2D* source, std::vector<float>* result) {
        const float constants[4] = {weight, 0, 0, 0};
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = sizeof(constants);
        bd.Usage = D3D11_USAGE_IMMUTABLE;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        D3D11_SUBRESOURCE_DATA init{};
        init.pSysMem = constants;
        ComPtr<ID3D11Buffer> cb;
        if (FAILED(device->CreateBuffer(&bd, &init, &cb))) return false;
        ID3D11ShaderResourceView* slots[8] = {};
        for (size_t i = 0; i < bound.size() && i < 8; ++i) slots[i] = bound[i];
        const UINT keep = ~0u;
        context->CSSetShader(cs.Get(), nullptr, 0);
        context->CSSetShaderResources(0, 8, slots);
        context->CSSetUnorderedAccessViews(0, 1, &target, &keep);
        ID3D11Buffer* b = cb.Get();
        context->CSSetConstantBuffers(0, 1, &b);
        context->Dispatch((w + 7) / 8, (h + 7) / 8, 1);
        ID3D11ShaderResourceView* noSrv[8] = {};
        ID3D11UnorderedAccessView* noUav = nullptr;
        context->CSSetShaderResources(0, 8, noSrv);
        context->CSSetUnorderedAccessViews(0, 1, &noUav, &keep);
        context->CSSetShader(nullptr, nullptr, 0);
        context->CopyResource(staging.Get(), source);
        D3D11_MAPPED_SUBRESOURCE map{};
        if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &map))) return false;
        result->resize(size_t(w) * h * 4);
        for (UINT y = 0; y < h; ++y) {
            const auto* row = reinterpret_cast<const uint16_t*>(static_cast<const uint8_t*>(map.pData) + size_t(y) * map.RowPitch);
            for (UINT i = 0; i < w * 4; ++i) (*result)[(size_t(y) * w * 4) + i] = fromHalf(row[i]);
        }
        context->Unmap(staging.Get(), 0);
        return true;
    };
    const auto near1 = [](float a, float b) { return std::fabs(a - b) <= 0.01f * std::max(1.0f, std::fabs(b)); };

    std::vector<ID3D11ShaderResourceView*> all;
    for (auto& f : frames) all.push_back(f.Get());
    std::vector<float> first;
    check(run(all, 1.0f / 8.0f, uav.Get(), out.Get(), &first), "eight frames run");
    float mean = 0.0f;
    for (float l : levels) mean += l;
    mean /= 8.0f;
    if (first.size() == size_t(w) * h * 4) {
        check(near1(first[0], mean), "the red is the mean of the eight frames");
        check(near1(first[2], mean * 0.5f), "the blue is the mean of the eight frames");
        check(near1(first[3], 0.5f), "the alpha is averaged like any channel");
        const size_t last = (size_t(h - 1) * w + (w - 1)) * 4;
        check(near1(first[last], mean + 0.25f * float(w - 1)), "the last texel is written, from its own column");
        check(near1(first[last + 1], 1.0f + 0.5f * float(h - 1)), "the last texel is written, from its own row");
    }

    // The ring's next frame: every frame is in another slot. A whole cycle's sum does not change.
    std::vector<ID3D11ShaderResourceView*> rotated(all);
    std::rotate(rotated.begin(), rotated.begin() + 3, rotated.end());
    std::vector<float> second;
    check(run(rotated, 1.0f / 8.0f, uav.Get(), out.Get(), &second), "rotated frames run");
    bool same = second.size() == first.size() && !first.empty();
    for (size_t i = 0; same && i < first.size(); ++i) same = near1(second[i], first[i]);
    check(same, "a whole cycle gives the same image whichever slot holds which frame");

    std::vector<ID3D11ShaderResourceView*> three(all.begin(), all.begin() + 3);
    std::vector<float> partial;
    check(run(three, 1.0f / 3.0f, uav.Get(), out.Get(), &partial), "three frames run");
    if (partial.size() == size_t(w) * h * 4) check(near1(partial[0], (levels[0] + levels[1] + levels[2]) / 3.0f), "empty slots do not dim the mean of the frames bound");

    // R11G11B10 frames: no alpha in the format, and the shader reads 1.
    {
        D3D11_TEXTURE2D_DESC d{};
        d.Width = w;
        d.Height = h;
        d.MipLevels = 1;
        d.ArraySize = 1;
        d.Format = DXGI_FORMAT_R11G11B10_FLOAT;
        d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        std::vector<uint32_t> packed(size_t(w) * h, 0u);
        D3D11_SUBRESOURCE_DATA init{};
        init.pSysMem = packed.data();
        init.SysMemPitch = w * 4;
        ComPtr<ID3D11Texture2D> t;
        ComPtr<ID3D11ShaderResourceView> v;
        const bool built = SUCCEEDED(device->CreateTexture2D(&d, &init, &t)) && SUCCEEDED(device->CreateShaderResourceView(t.Get(), nullptr, &v));
        check(built, "an R11G11B10 frame creates");
        if (built) {
            std::vector<float> rgb;
            check(run({v.Get(), v.Get()}, 0.5f, uav.Get(), out.Get(), &rgb), "R11G11B10 frames run");
            if (rgb.size() == size_t(w) * h * 4) check(near1(rgb[3], 1.0f), "a format with no alpha keeps the alpha at 1");
        }
    }
    return failures;
}
