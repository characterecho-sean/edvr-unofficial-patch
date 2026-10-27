#pragma once

// The HUD layer's edge smoothing (flat profile; fix.hud_antialiasing; docs/design-flat-ui-quality-2026-10-05.md, "2026-10-24: edge smoothing for the
// HUD layer"): SMAA 1x over the layer's finished 8-bit content, so the composite that downsamples it reads smoothed edges. The shaders are
// ui_layer_smaa_shader.h's edge entry and the map's weights and blend (map_smaa_shader.h), all built offline (tools/temporal_shader_build) with SMAA.hlsl
// as upstream wrote it; the lookup textures are map_smaa.cpp's. Nothing here logs: ui_layer.cpp reads the state and the failure this returns and says it.
//
// One UiLayerSmaa belongs to one eye's layer. uiLayerSmaaEnsure makes what is missing at the layer's size (the targets are re-made when the size moves), and a
// creation that fails at a size is remembered for that size: every later call there answers kStoodDown without trying again. uiLayerSmaaRun is the three
// dispatches. Everything a pass touches on the compute stage is put back by the caller except the two samplers, which this puts back itself.

#include "map_smaa.h"
#include "temporal_shader_bytecode.h"

#include <d3d11.h>
#include <wrl/client.h>

#include <cstdint>

namespace edvr {

// What a creation that failed was doing, and the HRESULT it came back with.
struct UiLayerSmaaFail {
    const char* what = nullptr;
    HRESULT hr = S_OK;
};

enum class UiLayerSmaaState : uint8_t {
    kReady,      // usable; nothing was made this call
    kMade,       // usable; the targets were made (or re-made at a new size) this call
    kFailed,     // not usable; this call's creation failed (UiLayerSmaaFail says what)
    kStoodDown,  // not usable; a creation already failed at this size and was said
};

struct UiLayerSmaa {
    template <class T>
    using Ptr = Microsoft::WRL::ComPtr<T>;
    // Per device: the passes, the two samplers and SMAA's immutable lookup textures.
    Ptr<ID3D11ComputeShader> edgesCs, weightsCs, blendCs;
    Ptr<ID3D11SamplerState> linear, point;
    Ptr<ID3D11Texture2D> area, search;
    Ptr<ID3D11ShaderResourceView> areaSrv, searchSrv;
    // Per layer size: the metrics (1/w, 1/h, w, h), the edges (R8G8), the weights and the blended output (R8G8B8A8).
    Ptr<ID3D11Buffer> metrics;
    Ptr<ID3D11Texture2D> edges, weights, out;
    Ptr<ID3D11UnorderedAccessView> edgesUav, weightsUav, outUav;
    Ptr<ID3D11ShaderResourceView> edgesSrv, weightsSrv, outSrv;
    uint32_t w = 0, h = 0;               // the size the targets stand at; 0: none
    uint32_t failedW = 0, failedH = 0;   // the size a creation failed at; 0: none

    // The size-bound targets go (the layer was released or re-made); what failed at a size stays remembered.
    void releaseTargets() {
        metrics.Reset();
        edges.Reset();
        weights.Reset();
        out.Reset();
        edgesUav.Reset();
        weightsUav.Reset();
        outUav.Reset();
        edgesSrv.Reset();
        weightsSrv.Reset();
        outSrv.Reset();
        w = h = 0;
    }
    // Every child of the device goes, and the failure with them: a new device may make what the last could not.
    void releaseAll() {
        releaseTargets();
        edgesCs.Reset();
        weightsCs.Reset();
        blendCs.Reset();
        linear.Reset();
        point.Reset();
        area.Reset();
        search.Reset();
        areaSrv.Reset();
        searchSrv.Reset();
        failedW = failedH = 0;
    }
    bool ready() const { return outSrv && w && h; }
};

namespace uilayersmaa_detail {

inline bool formatStores(ID3D11Device* dev, DXGI_FORMAT format, HRESULT* hr) {
    D3D11_FEATURE_DATA_FORMAT_SUPPORT2 support{};
    support.InFormat = format;
    *hr = dev->CheckFeatureSupport(D3D11_FEATURE_FORMAT_SUPPORT2, &support, sizeof(support));
    if (FAILED(*hr)) return false;
    if (!(support.OutFormatSupport2 & D3D11_FORMAT_SUPPORT2_UAV_TYPED_STORE)) {
        *hr = E_FAIL;
        return false;
    }
    return true;
}

// A texture of the layer's size with an unordered-access and a shader view, over the format's own typed views.
inline bool makeTarget(ID3D11Device* dev, uint32_t w, uint32_t h, DXGI_FORMAT format, UiLayerSmaa::Ptr<ID3D11Texture2D>& tex,
                       UiLayerSmaa::Ptr<ID3D11UnorderedAccessView>& uav, UiLayerSmaa::Ptr<ID3D11ShaderResourceView>& srv, HRESULT* hr) {
    D3D11_TEXTURE2D_DESC td{};
    td.Width = w;
    td.Height = h;
    td.MipLevels = td.ArraySize = 1;
    td.Format = format;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
    *hr = dev->CreateTexture2D(&td, nullptr, tex.ReleaseAndGetAddressOf());
    if (FAILED(*hr)) return false;
    *hr = dev->CreateUnorderedAccessView(tex.Get(), nullptr, uav.ReleaseAndGetAddressOf());
    if (FAILED(*hr)) return false;
    *hr = dev->CreateShaderResourceView(tex.Get(), nullptr, srv.ReleaseAndGetAddressOf());
    return SUCCEEDED(*hr);
}

// One of SMAA's two immutable lookup textures (AreaTex R8G8, SearchTex R8), made into locals and published only whole.
inline bool makeLookup(ID3D11Device* dev, const MapSmaaTable& table, DXGI_FORMAT format, UiLayerSmaa::Ptr<ID3D11Texture2D>& tex,
                       UiLayerSmaa::Ptr<ID3D11ShaderResourceView>& srv, HRESULT* hr) {
    D3D11_TEXTURE2D_DESC td{};
    td.Width = table.width;
    td.Height = table.height;
    td.MipLevels = td.ArraySize = 1;
    td.Format = format;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_IMMUTABLE;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA init{};
    init.pSysMem = table.bytes;
    init.SysMemPitch = table.pitch;
    UiLayerSmaa::Ptr<ID3D11Texture2D> made;
    UiLayerSmaa::Ptr<ID3D11ShaderResourceView> view;
    *hr = dev->CreateTexture2D(&td, &init, made.GetAddressOf());
    if (FAILED(*hr)) return false;
    *hr = dev->CreateShaderResourceView(made.Get(), nullptr, view.GetAddressOf());
    if (FAILED(*hr)) return false;
    tex = made;
    srv = view;
    return true;
}

}  // namespace uilayersmaa_detail

// Makes what is missing for a layer of w x h. A failure is told once per size: the first call that fails fills `fail` and answers kFailed, the calls after it
// at that size answer kStoodDown without creating anything. A different size tries again.
inline UiLayerSmaaState uiLayerSmaaEnsure(UiLayerSmaa& s, ID3D11Device* dev, uint32_t w, uint32_t h, UiLayerSmaaFail* fail) {
    using namespace uilayersmaa_detail;
    if (s.ready() && s.w == w && s.h == h) return UiLayerSmaaState::kReady;
    if (s.failedW == w && s.failedH == h && w && h) return UiLayerSmaaState::kStoodDown;
    s.releaseTargets();
    HRESULT hr = S_OK;
    const auto stand = [&](const char* what) {
        s.releaseTargets();
        s.failedW = w;
        s.failedH = h;
        if (fail) {
            fail->what = what;
            fail->hr = hr;
        }
        return UiLayerSmaaState::kFailed;
    };
    if (!dev || !w || !h) {
        hr = E_INVALIDARG;
        return stand("no device or an empty layer");
    }
    // The edges and the weights are written through typed unordered-access views of their formats.
    if (!formatStores(dev, DXGI_FORMAT_R8G8_UNORM, &hr)) return stand("no typed unordered-access store for R8G8_UNORM on this GPU");
    if (!formatStores(dev, DXGI_FORMAT_R8G8B8A8_UNORM, &hr)) return stand("no typed unordered-access store for R8G8B8A8_UNORM on this GPU");
    if (!s.edgesCs && FAILED(hr = dev->CreateComputeShader(kUiLayerSmaaEdgesBytecode, sizeof(kUiLayerSmaaEdgesBytecode), nullptr, s.edgesCs.GetAddressOf())))
        return stand("the edge shader could not be created");
    if (!s.weightsCs &&
        FAILED(hr = dev->CreateComputeShader(kFlatMapSmaaWeightsBytecode, sizeof(kFlatMapSmaaWeightsBytecode), nullptr, s.weightsCs.GetAddressOf())))
        return stand("the weights shader could not be created");
    if (!s.blendCs && FAILED(hr = dev->CreateComputeShader(kFlatMapSmaaBlendBytecode, sizeof(kFlatMapSmaaBlendBytecode), nullptr, s.blendCs.GetAddressOf())))
        return stand("the blend shader could not be created");
    // SMAA.hlsl's LinearSampler (s0) and PointSampler (s1): clamped, mip point.
    if (!s.linear) {
        D3D11_SAMPLER_DESC sd{};
        sd.Filter = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.MaxLOD = D3D11_FLOAT32_MAX;
        if (FAILED(hr = dev->CreateSamplerState(&sd, s.linear.GetAddressOf()))) return stand("the linear sampler could not be created");
    }
    if (!s.point) {
        D3D11_SAMPLER_DESC sd{};
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.MaxLOD = D3D11_FLOAT32_MAX;
        if (FAILED(hr = dev->CreateSamplerState(&sd, s.point.GetAddressOf()))) return stand("the point sampler could not be created");
    }
    if (!s.areaSrv && !makeLookup(dev, mapSmaaAreaTable(), DXGI_FORMAT_R8G8_UNORM, s.area, s.areaSrv, &hr)) return stand("SMAA's area texture could not be created");
    if (!s.searchSrv && !makeLookup(dev, mapSmaaSearchTable(), DXGI_FORMAT_R8_UNORM, s.search, s.searchSrv, &hr))
        return stand("SMAA's search texture could not be created");
    // (1/width, 1/height, width, height): the cbuffer SMAA.hlsl's SMAA_RT_METRICS names. Immutable: it moves only with the size.
    {
        const float metrics[4] = {1.0f / static_cast<float>(w), 1.0f / static_cast<float>(h), static_cast<float>(w), static_cast<float>(h)};
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = sizeof(metrics);
        bd.Usage = D3D11_USAGE_IMMUTABLE;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        D3D11_SUBRESOURCE_DATA init{};
        init.pSysMem = metrics;
        if (FAILED(hr = dev->CreateBuffer(&bd, &init, s.metrics.GetAddressOf()))) return stand("the metrics constant buffer could not be created");
    }
    if (!makeTarget(dev, w, h, DXGI_FORMAT_R8G8_UNORM, s.edges, s.edgesUav, s.edgesSrv, &hr)) return stand("the edges texture (R8G8) could not be created");
    if (!makeTarget(dev, w, h, DXGI_FORMAT_R8G8B8A8_UNORM, s.weights, s.weightsUav, s.weightsSrv, &hr))
        return stand("the weights texture (R8G8B8A8) could not be created");
    if (!makeTarget(dev, w, h, DXGI_FORMAT_R8G8B8A8_UNORM, s.out, s.outUav, s.outSrv, &hr)) return stand("the smoothed output (R8G8B8A8) could not be created");
    s.w = w;
    s.h = h;
    return UiLayerSmaaState::kMade;
}

// The three passes on `layer` (an R8G8B8A8_UNORM view of the size uiLayerSmaaEnsure made the targets for): edges, then weights, then the blend into the output.
// Returns the output's view, valid until the next run, or null when there is nothing to run. The compute shader, constant buffer 0, resource slots 0-2 and
// unordered-access slot 0 are left as the last pass set them (slots 0-2 and the unordered-access slot null); the caller saves and restores those. The two
// samplers are saved and put back here.
inline ID3D11ShaderResourceView* uiLayerSmaaRun(UiLayerSmaa& s, ID3D11DeviceContext* ctx, ID3D11ShaderResourceView* layer) {
    if (!ctx || !layer || !s.ready() || !s.edgesCs || !s.weightsCs || !s.blendCs) return nullptr;
    ID3D11SamplerState* savedSamplers[2] = {};
    ctx->CSGetSamplers(0, 2, savedSamplers);
    ID3D11SamplerState* samplers[2] = {s.linear.Get(), s.point.Get()};
    ID3D11Buffer* metrics = s.metrics.Get();
    ctx->CSSetSamplers(0, 2, samplers);
    ctx->CSSetConstantBuffers(0, 1, &metrics);
    const UINT groupsX = (s.w + 7u) / 8u, groupsY = (s.h + 7u) / 8u;
    ID3D11ShaderResourceView* const noSrv[3] = {};
    ID3D11UnorderedAccessView* noUav = nullptr;
    {
        // The layer's colour edges, four channels (R8G8).
        ID3D11ShaderResourceView* in = layer;
        ID3D11UnorderedAccessView* target = s.edgesUav.Get();
        ctx->CSSetShader(s.edgesCs.Get(), nullptr, 0);
        ctx->CSSetShaderResources(0, 1, &in);
        ctx->CSSetUnorderedAccessViews(0, 1, &target, nullptr);
        ctx->Dispatch(groupsX, groupsY, 1);
        ctx->CSSetShaderResources(0, 3, noSrv);
        ctx->CSSetUnorderedAccessViews(0, 1, &noUav, nullptr);
    }
    {
        // The blending weights: the edges, with SMAA's area and search textures.
        ID3D11ShaderResourceView* in[3] = {s.edgesSrv.Get(), s.areaSrv.Get(), s.searchSrv.Get()};
        ID3D11UnorderedAccessView* target = s.weightsUav.Get();
        ctx->CSSetShader(s.weightsCs.Get(), nullptr, 0);
        ctx->CSSetShaderResources(0, 3, in);
        ctx->CSSetUnorderedAccessViews(0, 1, &target, nullptr);
        ctx->Dispatch(groupsX, groupsY, 1);
        ctx->CSSetShaderResources(0, 3, noSrv);
        ctx->CSSetUnorderedAccessViews(0, 1, &noUav, nullptr);
    }
    {
        // The neighbourhood blend of the layer by those weights, all four channels.
        ID3D11ShaderResourceView* in[2] = {layer, s.weightsSrv.Get()};
        ID3D11UnorderedAccessView* target = s.outUav.Get();
        ctx->CSSetShader(s.blendCs.Get(), nullptr, 0);
        ctx->CSSetShaderResources(0, 2, in);
        ctx->CSSetUnorderedAccessViews(0, 1, &target, nullptr);
        ctx->Dispatch(groupsX, groupsY, 1);
        ctx->CSSetShaderResources(0, 3, noSrv);
        ctx->CSSetUnorderedAccessViews(0, 1, &noUav, nullptr);
    }
    ctx->CSSetSamplers(0, 2, savedSamplers);
    for (ID3D11SamplerState* saved : savedSamplers)
        if (saved) saved->Release();
    return s.outSrv.Get();
}

}  // namespace edvr
