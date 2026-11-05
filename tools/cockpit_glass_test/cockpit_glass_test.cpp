// visual.cockpit_glass = legacy (docs\cockpit-glass-legacy-port.md sections 8 to 11): src\d3d11\cockpit_glass_core.h held to its contract, the dirt streak generator
// (src\d3d11\cockpit_glass_streaks.h: deterministic, tileable, the legacy mean and max, streaky) and the build's three pixel shaders (kCockpitGlass*PsBytecode, from
// tools\temporal_shader_build) run on a WARP device with the D3D debug layer, drawn with a vertex shader that carries the game's canopy vertex shaders' output
// signature, so the draw links as the game's does. The pixel it draws is compared with the legacy model worked out here, in C++, from the same inputs:
//   - no mask: the legacy alpha base (0.02) and no colour;
//   - dirt under the sun (vertex B 0.72): the measured mask, colour scale and streak, Lambert on the legacy normal over pi, divided by the alpha written;
//   - a cascade whose moment map puts an occluder between the light and the pixel: dark; one with the occluder behind the pixel: lit as without a cascade;
//   - the reflection probe ambient with the sun off (a hangar), a lit crack, the glare lobes at the measured values, the tint alpha, no streak texture (no dirt);
//   - the ice and condensation overlays: the same dirt light, premultiplied, alpha written 0, from their own vertex shaders' mask orders and texture slots.
// Run with --dump <file> to write the generated streak texture as raw bytes (512 x 512). Output under obj\ and System32's d3d11.
#include "../../src/d3d11/cockpit_glass_core.h"
#include "../../src/d3d11/cockpit_glass_shader.h"
#include "../../src/d3d11/cockpit_glass_streaks.h"
#include "temporal_shader_bytecode.h"

#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <chrono>
#include <complex>
#include <cstring>
#include <utility>
#include <vector>

using Microsoft::WRL::ComPtr;
namespace cg = edvr::cockpit_glass;

static int g_failures = 0;
static void check(bool ok, const char* what) {
    if (!ok) {
        std::printf("FAIL: %s\n", what);
        ++g_failures;
    }
}
static void hr(HRESULT h, const char* what) {
    if (FAILED(h)) {
        std::printf("FAIL: %s (0x%08X)\n", what, static_cast<unsigned>(h));
        std::exit(1);
    }
}

// ---- the pure half --------------------------------------------------------------------------------------------------
static void testCore() {
    bool recognised = false;
    check(cg::parseLook("legacy", &recognised) == cg::Look::Legacy && recognised, "legacy is the legacy look");
    check(cg::parseLook("LEGACY", &recognised) == cg::Look::Legacy && recognised, "the key is not case sensitive");
    check(cg::parseLook("odyssey", &recognised) == cg::Look::Odyssey && recognised, "odyssey is the stock look");
    check(cg::parseLook(nullptr, &recognised) == cg::Look::Odyssey && recognised, "no value is the stock look, recognised");
    check(cg::parseLook("", &recognised) == cg::Look::Odyssey && !recognised, "an empty value is stock and not recognised");
    check(cg::parseLook("legacyx", &recognised) == cg::Look::Odyssey && !recognised, "a near miss is stock and not recognised");
    check(cg::parseLook("on", &recognised) == cg::Look::Odyssey && !recognised, "on is not a look");
    check(cg::parseLook("legacy", nullptr) == cg::Look::Legacy, "the recognised flag is optional");

    const uint64_t other = 0x70F56E2EB7032E1Aull;   // the intact quality 0-1 shader: stock
    check(cg::swapWanted(cg::Look::Legacy, true, cg::kReplacedPsHash), "legacy with a replacement swaps the intact quality-3 canopy shader");
    check(!cg::swapWanted(cg::Look::Odyssey, true, cg::kReplacedPsHash), "odyssey never swaps");
    check(!cg::swapWanted(cg::Look::Legacy, false, cg::kReplacedPsHash), "no replacement, no swap");
    check(!cg::swapWanted(cg::Look::Legacy, true, other), "another canopy shader stays stock");
    check(!cg::swapWanted(cg::Look::Legacy, true, 0), "an unhashed shader stays stock");
    check(!cg::swapWanted(cg::Look::Legacy, true, cg::kCanopyVsHash), "the vertex shader's hash is not the pixel shader's");

    check(cg::windowDue(30000, 0, 30000) && !cg::windowDue(29999, 0, 30000), "the window line is due at 30 s");
    char line[200];
    cg::formatWindow(line, sizeof line, 30.0, 0, 3050, 0, cg::replacementName(cg::Replacement::Refused));
    check(std::strcmp(line, "cockpit glass: window 30 s legacy binds=0 canopy-seen=3050 ice-overlays=0 replacement=refused") == 0, "binds=0 with canopy-seen above 0 reads as it should");
    cg::formatWindow(line, sizeof line, 30.0, 3050, 3050, 90, cg::replacementName(cg::Replacement::Ready));
    check(std::strcmp(line, "cockpit glass: window 30 s legacy binds=3050 canopy-seen=3050 ice-overlays=90 replacement=ready") == 0, "a working window reads binds equal to canopy-seen");
    check(std::strcmp(cg::replacementName(cg::Replacement::NotTried), "not-made-yet") == 0, "an untried replacement is named");

    check(cg::dirtLevelOf(100) == 1.0f && cg::dirtLevelOf(0) == 0.0f && cg::dirtLevelOf(250) == 2.5f && cg::dirtLevelOf(300) == 3.0f, "the dirt level is the percent over 100: 100 is the look as shipped");
    check(cg::dirtLevelOf(-5) == 0.0f && cg::dirtLevelOf(1000) == 3.0f, "a percent outside 0 to 300 is held to the key's range");
    check(cg::kDirtPercentDefault == 100 && cg::kLevelSlot == 13, "the key ships at 100 and the level lives at b13");
    check(cg::dirtRowDim(cg::Look::Odyssey) && !cg::dirtRowDim(cg::Look::Legacy), "the Legacy glass dirt row is dimmed with odyssey and live with legacy");
    check(cg::overlayFor(0x7F770D61DEA7CD30ull) == cg::Overlay::Ice && cg::overlayFor(0x42747CC92CF844C4ull) == cg::Overlay::Condensation, "the quality-3 ice and condensation shaders are overlaid");
    check(cg::overlayFor(0xF90EA0D834A8B44Eull) == cg::Overlay::None && cg::overlayFor(0x4BF640EED290C885ull) == cg::Overlay::None &&
              cg::overlayFor(0x3B4EDBBAF1BA87BFull) == cg::Overlay::None && cg::overlayFor(0x2A4B54153FBF97EEull) == cg::Overlay::None &&
              cg::overlayFor(cg::kReplacedPsHash) == cg::Overlay::None && cg::overlayFor(0) == cg::Overlay::None,
          "the quality 0-2 ice and condensation shaders (other slot layouts), the intact one and an unhashed bind are not overlaid");
    check(cg::kLiveInputCount == 7 && cg::kLiveInputs[6].isPosition && cg::kLiveInputs[1].isUint, "the signature table has the game's seven inputs");
    for (size_t i = 0; i < cg::kLiveInputCount; ++i) check(cg::kLiveInputs[i].reg == i, "the game's inputs sit in registers 0 to 6 in order");
}

// ---- the device ------------------------------------------------------------------------------------------------------
struct Rig {
    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11DeviceContext> ctx;
    ComPtr<ID3D11InfoQueue> queue;
    bool debugLayer = false;
    Rig() {
        D3D_FEATURE_LEVEL fl;
        HRESULT h = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_DEBUG, nullptr, 0, D3D11_SDK_VERSION, &dev, &fl, &ctx);
        if (h == DXGI_ERROR_SDK_COMPONENT_MISSING)
            h = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &dev, &fl, &ctx);
        else
            debugLayer = SUCCEEDED(h);
        hr(h, "CreateDevice");
        dev.As(&queue);
    }
    unsigned problems() {
        unsigned n = 0;
        if (!queue) return 0;
        for (UINT64 i = 0; i < queue->GetNumStoredMessagesAllowedByRetrievalFilter(); ++i) {
            SIZE_T sz = 0;
            queue->GetMessage(i, nullptr, &sz);
            std::vector<char> b(sz);
            auto* m = reinterpret_cast<D3D11_MESSAGE*>(b.data());
            queue->GetMessage(i, m, &sz);
            if (m->Severity <= D3D11_MESSAGE_SEVERITY_WARNING) {
                std::printf("debug layer: %s\n", m->pDescription);
                ++n;
            }
        }
        queue->ClearStoredMessages();
        return n;
    }
};

static const char kVs[] = R"HLSL(
struct VO {
    float4 masks : __USER_KEY_COCKPITGLASSMASKS;
    nointerpolation uint faceInvariant : __USER_VERTEX_FACEINVARIANT;
    float3 lightingNormal : __USER_VERTEX_M_LIGHTINGNORMAL;
    float3 lightingPosition : __USER_VERTEX_M_LIGHTINGPOSITION;
    float3 lightingTangent : __USER_VERTEX_M_LIGHTINGTANGENT;
    float2 texcoord : __USER_VERTEX_M_TEXCOORD;
    float4 position : SV_POSITION;
};
cbuffer Values : register(b0) { float4 masks; float4 normal; float4 position; float4 tangent; float4 uv; }
VO main(uint id : SV_VertexID) {
    float2 p = id == 0 ? float2(-1, -1) : (id == 1 ? float2(-1, 3) : float2(3, -1));
    VO o;
    o.masks = masks;
    o.faceInvariant = 0;
    o.lightingNormal = normal.xyz;
    o.lightingPosition = position.xyz;
    o.lightingTangent = tangent.xyz;
    o.texcoord = uv.xy;
    o.position = float4(p, 0.5, 1);
    return o;
}
)HLSL";

struct Float4 { float v[4]; };

static float halfToFloat(uint16_t h) {
    const uint32_t sign = (h >> 15) & 1, exp = (h >> 10) & 31, man = h & 1023;
    float f;
    if (exp == 0) f = std::ldexp(static_cast<float>(man), -24);
    else if (exp == 31) f = man ? std::nanf("") : HUGE_VALF;
    else f = std::ldexp(static_cast<float>(man | 1024), static_cast<int>(exp) - 25);
    return sign ? -f : f;
}
static uint16_t floatToHalf(float f) {
    uint32_t x;
    std::memcpy(&x, &f, 4);
    const uint32_t sign = (x >> 16) & 0x8000;
    int exp = static_cast<int>((x >> 23) & 0xFF) - 127 + 15;
    const uint32_t man = x & 0x7FFFFF;
    if (exp <= 0) return static_cast<uint16_t>(sign);
    if (exp >= 31) return static_cast<uint16_t>(sign | 0x7C00);
    return static_cast<uint16_t>(sign | (exp << 10) | (man >> 13));
}

enum Variant { kIntact = 0, kIce = 1, kCondensation = 2 };

struct Case {
    Variant variant = kIntact;
    bool streaks = true;      // the streak texture bound at t12 (its texel is 1, so the dirt texture reads the legacy max, 0.09)
    bool levelBound = true;   // the dirt level block bound at b13
    float dirtLevel = 1.0f;   // its x: visual.cockpit_glass_dirt / 100, 1 = the look as shipped
    float masks[4] = {0, 0, 0, 0};   // as the vertex shader of the variant passes them
    float normal[3] = {0, 0, 1}, position[3] = {0, 0, 5}, tangent[3] = {1, 0, 0};
    float tint = 0, alphaMultiplier = 1;
    float sunColour[3] = {0, 0, 0}, sunDir[3] = {0, 0, 1};
    bool sunOn = true;
    int cascades = 0;
    float occluder = 0.9f;   // the moment map's occluder, in the shader's depth space (1 - clip z)
    bool probe = false;
    float probeColour[3] = {0, 0, 0};
    bool crack = false;   // the crack texture's alpha is full and its normal flat, and the crack colour is 0.3 grey
};

struct Pixel { float r, g, b, a; };

class Canopy {
public:
    Canopy() {
        ComPtr<ID3DBlob> vs, err;
        hr(D3DCompile(kVs, sizeof kVs - 1, "test canopy vs", nullptr, nullptr, "main", "vs_5_0", 0, 0, &vs, &err), "the test vertex shader compiles");
        hr(rig.dev->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &vsh), "CreateVertexShader");
        hr(rig.dev->CreatePixelShader(edvr::kCockpitGlassLegacyPsBytecode, sizeof(edvr::kCockpitGlassLegacyPsBytecode), nullptr, &psh[kIntact]), "the intact bytecode creates");
        hr(rig.dev->CreatePixelShader(edvr::kCockpitGlassIceOverlayPsBytecode, sizeof(edvr::kCockpitGlassIceOverlayPsBytecode), nullptr, &psh[kIce]), "the ice overlay bytecode creates");
        hr(rig.dev->CreatePixelShader(edvr::kCockpitGlassCondensationOverlayPsBytecode, sizeof(edvr::kCockpitGlassCondensationOverlayPsBytecode), nullptr, &psh[kCondensation]),
           "the condensation overlay bytecode creates");
        D3D11_SAMPLER_DESC sd{};
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR; sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_WRAP; sd.MaxLOD = D3D11_FLOAT32_MAX;
        hr(rig.dev->CreateSamplerState(&sd, &s0), "s0");
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT; sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        hr(rig.dev->CreateSamplerState(&sd, &s1), "s1");
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        hr(rig.dev->CreateSamplerState(&sd, &s2), "s2");
        D3D11_TEXTURE2D_DESC td{};
        td.Width = td.Height = 4; td.MipLevels = 1; td.ArraySize = 1; td.Format = DXGI_FORMAT_R16G16B16A16_FLOAT; td.SampleDesc.Count = 1;
        td.BindFlags = D3D11_BIND_RENDER_TARGET;
        hr(rig.dev->CreateTexture2D(&td, nullptr, &target), "target");
        hr(rig.dev->CreateRenderTargetView(target.Get(), nullptr, &rtv), "rtv");
        td.BindFlags = 0; td.Usage = D3D11_USAGE_STAGING; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        hr(rig.dev->CreateTexture2D(&td, nullptr, &staging), "staging");
        // t6: no crack (all zero). t9: the blemish map's x (dirt) and y (imperfection) channels full.
        crack = texture1x1(0, 0, 0, 0);
        crackFull = texture1x1(128, 128, 0, 255);
        streakFull = texture1x1(255, 0, 0, 0);
        blemish = texture1x1(255, 255, 0, 0);
    }

    Pixel draw(const Case& c) {
        // b1: the frame block, 336 registers (the game binds 5,376 bytes; the shader declares 327).
        std::vector<Float4> frame(336, Float4{{0, 0, 0, 0}});
        auto setu = [&](int reg, int comp, uint32_t u) { std::memcpy(&frame[reg].v[comp], &u, 4); };
        frame[56] = {{c.tint, c.alphaMultiplier, 0, 0}};
        frame[90] = {{0, 1.0f / 256.0f, 256.0f, 0}};
        frame[227] = {{0, 0, 1.0f, 0}};   // the cluster depth slice: log(z) * 1 + 0, clamped to the one slice
        setu(228, 0, 1); setu(228, 1, 1); setu(228, 2, 1); setu(228, 3, 16);   // one tile column and row, one slice, 16 px tiles
        setu(245, 1, c.sunOn ? 1 : 0);
        setu(253, 1, 0); setu(253, 2, 1024);   // probe volumes at byte 0 of t2, their headers at byte 1024
        frame[261] = {{0.2f, 0.001f, 0, 0}};   // the moment shadow's light-bleed cutoff and bias
        frame[277] = {{1, 0, 0, 0}}; frame[278] = {{0, 1, 0, 0}}; frame[279] = {{0, 0, 1, 0}};   // the camera basis: identity, so lighting space is view space
        frame[297] = {{4, 4, 0.25f, 0.25f}};
        frame[298] = {{1, 0, 0, 0}}; frame[299] = {{0, 1, 0, 0}}; frame[300] = {{0, 0, 1, 0}}; frame[301] = {{0, 0, 0, 0}};
        frame[302] = {{1, 1, 0.1f, 0}};   // cascade 0: clip = (x, y, z / 10)
        frame[310] = {{0, 0, 0, 0}};
        frame[318] = {{0.5f, 0.5f, 0.5f, 0.5f}};
        setu(326, 0, static_cast<uint32_t>(c.cascades));
        frame[326].v[1] = 0.0f; frame[326].v[2] = 0.0f; frame[326].v[3] = 0.0f;
        std::vector<Float4> material(18, Float4{{0, 0, 0, 0}});
        material[0] = {{0.5f, 0, 0, 0}};   // the dirt alpha multiplier
        material[6] = {{0, 0, 0.5f, -0.2f}};   // the vertex masks' scale and offset: mask = saturate((B - 0.2) * 0.5) * CB2[0].x
        material[7] = {{0, 0, 1.0f, 0}};   // the crack's occlusion scale
        material[8] = {{0, 0.3f, 0.3f, 0.3f}};   // the crack colour
        material[13] = {{0, 0, 0, 0}};
        material[15] = {{1, 1, 0, 0}};
        material[17] = {{0, 0, 0.5f, 0}};   // crack threshold 0.5
        // t0: the cluster record 0 (count at +28, probe list at +2212), t1: the sun, t2: the probe volume and header.
        std::vector<uint32_t> lists(4096 / 4, 0), sun(48 / 4, 0), records(2048 / 4, 0);
        if (c.probe) {
            lists[28 / 4] = 1;
            lists[2212 / 4] = 0;   // probe id 0
            const float centre[4] = {0, 0, 0, 0};
            for (int i = 0; i < 4; ++i) std::memcpy(&records[(80 + 4 * i) / 4], &centre[i], 4);
            const float axes[3][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}};
            for (int a = 0; a < 3; ++a)
                for (int i = 0; i < 4; ++i) std::memcpy(&records[(112 + 16 * a + 4 * i) / 4], &axes[a][i], 4);
            const float intensity = 1.0f;
            std::memcpy(&records[(1024 + 4) / 4], &intensity, 4);   // header: slice 0, intensity 1, blend 0, type 0 (a global probe)
        }
        std::memcpy(&sun[0], c.sunColour, 12);
        std::memcpy(&sun[4], c.sunDir, 12);
        ComPtr<ID3D11Buffer> cb1 = constantBuffer(frame.data(), 16 * 336), cb2 = constantBuffer(material.data(), 16 * 18);
        Float4 vsv[5] = {{{c.masks[0], c.masks[1], c.masks[2], c.masks[3]}}, {{c.normal[0], c.normal[1], c.normal[2], 0}}, {{c.position[0], c.position[1], c.position[2], 0}},
                         {{c.tangent[0], c.tangent[1], c.tangent[2], 0}}, {{0.5f, 0.5f, 0, 0}}};
        ComPtr<ID3D11Buffer> vcb = constantBuffer(vsv, sizeof vsv);
        ComPtr<ID3D11ShaderResourceView> t0 = rawView(lists.data(), 4096), t1 = rawView(sun.data(), 48), t2 = rawView(records.data(), 2048);
        ComPtr<ID3D11ShaderResourceView> t3 = cubeArray(c.probeColour), t5 = moments(c.occluder);
        auto* ctx = rig.ctx.Get();
        const float clear[4] = {-1, -1, -1, -1};
        ctx->ClearRenderTargetView(rtv.Get(), clear);
        ID3D11RenderTargetView* rtvs[1] = {rtv.Get()};
        ctx->OMSetRenderTargets(1, rtvs, nullptr);
        D3D11_VIEWPORT vp{0, 0, 4, 4, 0, 1};
        ctx->RSSetViewports(1, &vp);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->VSSetShader(vsh.Get(), nullptr, 0);
        ctx->VSSetConstantBuffers(0, 1, vcb.GetAddressOf());
        ctx->PSSetShader(psh[c.variant].Get(), nullptr, 0);
        ID3D11Buffer* cbs[2] = {cb1.Get(), cb2.Get()};
        ctx->PSSetConstantBuffers(1, 2, cbs);
        const Float4 levelData{{c.dirtLevel, 0, 0, 0}};
        ComPtr<ID3D11Buffer> levelCb = constantBuffer(&levelData, sizeof levelData);
        ID3D11Buffer* level = c.levelBound ? levelCb.Get() : nullptr;
        ctx->PSSetConstantBuffers(cg::kLevelSlot, 1, &level);
        ID3D11ShaderResourceView* crackView = c.crack ? crackFull.Get() : crack.Get();
        const bool overlay = c.variant != kIntact;   // the ice and condensation draws keep the crack map at t7 and the blemish map at t11
        ID3D11ShaderResourceView* srvs[13] = {t0.Get(), t1.Get(), t2.Get(), t3.Get(), nullptr, t5.Get(), overlay ? nullptr : crackView, overlay ? crackView : nullptr, nullptr,
                                              overlay ? nullptr : blemish.Get(), nullptr, overlay ? blemish.Get() : nullptr, c.streaks ? streakFull.Get() : nullptr};
        ctx->PSSetShaderResources(0, 13, srvs);
        ID3D11SamplerState* smp[3] = {s0.Get(), s1.Get(), s2.Get()};
        ctx->PSSetSamplers(0, 3, smp);
        ctx->Draw(3, 0);
        ctx->CopyResource(staging.Get(), target.Get());
        D3D11_MAPPED_SUBRESOURCE m{};
        hr(ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m), "Map");
        const uint16_t* row = reinterpret_cast<const uint16_t*>(static_cast<const uint8_t*>(m.pData) + 2 * m.RowPitch);
        Pixel p{halfToFloat(row[8]), halfToFloat(row[9]), halfToFloat(row[10]), halfToFloat(row[11])};
        ctx->Unmap(staging.Get(), 0);
        ID3D11ShaderResourceView* none[13] = {};
        ctx->PSSetShaderResources(0, 13, none);
        return p;
    }

    Rig rig;

private:
    ComPtr<ID3D11VertexShader> vsh;
    ComPtr<ID3D11PixelShader> psh[3];
    ComPtr<ID3D11SamplerState> s0, s1, s2;
    ComPtr<ID3D11Texture2D> target, staging;
    ComPtr<ID3D11RenderTargetView> rtv;
    ComPtr<ID3D11ShaderResourceView> crack, crackFull, blemish, streakFull;

    ComPtr<ID3D11Buffer> constantBuffer(const void* data, UINT bytes) {
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = bytes; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        D3D11_SUBRESOURCE_DATA init{data, 0, 0};
        ComPtr<ID3D11Buffer> b;
        hr(rig.dev->CreateBuffer(&bd, &init, &b), "constant buffer");
        return b;
    }
    ComPtr<ID3D11ShaderResourceView> rawView(const void* data, UINT bytes) {
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = bytes; bd.BindFlags = D3D11_BIND_SHADER_RESOURCE; bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
        D3D11_SUBRESOURCE_DATA init{data, 0, 0};
        ComPtr<ID3D11Buffer> b;
        hr(rig.dev->CreateBuffer(&bd, &init, &b), "raw buffer");
        D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
        sv.Format = DXGI_FORMAT_R32_TYPELESS; sv.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
        sv.BufferEx.FirstElement = 0; sv.BufferEx.NumElements = bytes / 4; sv.BufferEx.Flags = D3D11_BUFFEREX_SRV_FLAG_RAW;
        ComPtr<ID3D11ShaderResourceView> v;
        hr(rig.dev->CreateShaderResourceView(b.Get(), &sv, &v), "raw view");
        return v;
    }
    ComPtr<ID3D11ShaderResourceView> texture1x1(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
        D3D11_TEXTURE2D_DESC td{};
        td.Width = td.Height = 1; td.MipLevels = 1; td.ArraySize = 1; td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        const uint8_t px[4] = {r, g, b, a};
        D3D11_SUBRESOURCE_DATA init{px, 4, 4};
        ComPtr<ID3D11Texture2D> t;
        hr(rig.dev->CreateTexture2D(&td, &init, &t), "1x1 texture");
        ComPtr<ID3D11ShaderResourceView> v;
        hr(rig.dev->CreateShaderResourceView(t.Get(), nullptr, &v), "1x1 view");
        return v;
    }
    ComPtr<ID3D11ShaderResourceView> cubeArray(const float rgb[3]) {
        D3D11_TEXTURE2D_DESC td{};
        td.Width = td.Height = 2; td.MipLevels = 1; td.ArraySize = 6; td.Format = DXGI_FORMAT_R16G16B16A16_FLOAT; td.SampleDesc.Count = 1;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE; td.MiscFlags = D3D11_RESOURCE_MISC_TEXTURECUBE;
        uint16_t texel[4] = {floatToHalf(rgb[0]), floatToHalf(rgb[1]), floatToHalf(rgb[2]), floatToHalf(1.0f)};
        uint16_t face[4 * 4];
        for (int i = 0; i < 4; ++i) std::memcpy(face + 4 * i, texel, 8);
        D3D11_SUBRESOURCE_DATA init[6];
        for (auto& i : init) i = {face, 16, 0};
        ComPtr<ID3D11Texture2D> t;
        hr(rig.dev->CreateTexture2D(&td, init, &t), "cube");
        D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
        sv.Format = td.Format; sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBEARRAY;
        sv.TextureCubeArray.MipLevels = 1; sv.TextureCubeArray.NumCubes = 1;
        ComPtr<ID3D11ShaderResourceView> v;
        hr(rig.dev->CreateShaderResourceView(t.Get(), &sv, &v), "cube view");
        return v;
    }
    // A 4x4 moment map (float32 here; the game's is 16 bits a channel) holding one occluder at depth z in the shader's space. The shader decodes q = M (b - offset); this stores b = M^-1 m + offset for m = (z, z^2, z^3, z^4).
    ComPtr<ID3D11ShaderResourceView> moments(float z) {
        static const double M[4][4] = {{0.222774, 0.077197, 0.792699, 0.031942}, {0.154968, 0.139463, 0.796342, -0.172282},
                                       {0.145199, 0.212020, 0.725869, -0.275801}, {0.163127, 0.259143, 0.653909, -0.337613}};
        double a[4][8];
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 8; ++j) a[i][j] = j < 4 ? M[i][j] : (j - 4 == i ? 1.0 : 0.0);
        for (int c = 0; c < 4; ++c) {
            int p = c;
            for (int r = c + 1; r < 4; ++r) if (std::fabs(a[r][c]) > std::fabs(a[p][c])) p = r;
            for (int j = 0; j < 8; ++j) std::swap(a[c][j], a[p][j]);
            const double d = a[c][c];
            for (int j = 0; j < 8; ++j) a[c][j] /= d;
            for (int r = 0; r < 4; ++r) {
                if (r == c) continue;
                const double f = a[r][c];
                for (int j = 0; j < 8; ++j) a[r][j] -= f * a[c][j];
            }
        }
        const double m[4] = {z, double(z) * z, double(z) * z * z, double(z) * z * z * z};
        float texel[4];
        for (int i = 0; i < 4; ++i) {
            double b = 0;
            for (int j = 0; j < 4; ++j) b += a[i][4 + j] * m[j];
            if (i == 0) b += 0.035956;
            texel[i] = static_cast<float>(b);
        }
        D3D11_TEXTURE2D_DESC td{};
        td.Width = td.Height = 4; td.MipLevels = 1; td.ArraySize = 1; td.Format = DXGI_FORMAT_R32G32B32A32_FLOAT; td.SampleDesc.Count = 1; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        std::vector<float> data(16 * 4);
        for (int i = 0; i < 16; ++i) std::memcpy(&data[4 * i], texel, 16);
        D3D11_SUBRESOURCE_DATA init{data.data(), 4 * 16, 0};
        ComPtr<ID3D11Texture2D> t;
        hr(rig.dev->CreateTexture2D(&td, &init, &t), "moment map");
        ComPtr<ID3D11ShaderResourceView> v;
        hr(rig.dev->CreateShaderResourceView(t.Get(), nullptr, &v), "moment view");
        return v;
    }
};

static const float kInvPi = 0.318310f;   // live's factor on the direct light, which the port carries

static bool closeTo(float a, float b, float rel) { return std::fabs(a - b) <= rel * std::fabs(b) + 1e-6f; }

// A radix-2 FFT, for the band energies (the texture is a power of two a side).
static void fft1(std::vector<std::complex<double>>& v, bool) {
    const size_t n = v.size();
    for (size_t i = 1, j = 0; i < n; ++i) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(v[i], v[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        const double ang = -2.0 * 3.14159265358979323846 / static_cast<double>(len);
        const std::complex<double> wl(std::cos(ang), std::sin(ang));
        for (size_t i = 0; i < n; i += len) {
            std::complex<double> w(1);
            for (size_t k = 0; k < len / 2; ++k, w *= wl) {
                const std::complex<double> u = v[i + k], t = v[i + k + len / 2] * w;
                v[i + k] = u + t;
                v[i + k + len / 2] = u - t;
            }
        }
    }
}

struct StreakStats {
    double std = 0, p5 = 0, p50 = 0, p90 = 0, p95 = 0, p99 = 0, meanOverMax = 0;
    double band[4] = {0, 0, 0, 0};   // spectral energy by cycles per tile: 0-4, 4-16, 16-64, 64 and up
    double local = 0, global = 0;    // fibre coherence: local, and how well the regions' directions agree
    double elongation[4] = {0, 0, 0, 0};   // per band (cycles per tile 1-4, 4-16, 16-64, 64 and up): the spectrum's second-moment eigenvalue ratio, 1 for isotropic noise
    double angle[4] = {0, 0, 0, 0};        // and the strokes' direction in degrees (image coordinates, y down)
    double bristle = 0;   // fine-band gradient energy along the stroke direction over the same across it, averaged over the tile: well below 1 where fibres run the strokes' length
};

// The figures section 13 of the doc compares (tools-side, on the stored bytes normalised to the texture's max): the same definitions as the analysis script used on the
// legacy texture's statistics.
static StreakStats streakStats(const std::vector<uint8_t>& bytes, int n) {
    StreakStats st;
    std::vector<double> a(bytes.size());
    double mx = 0, mean = 0;
    for (size_t i = 0; i < a.size(); ++i) { a[i] = bytes[i]; mx = a[i] > mx ? a[i] : mx; }
    for (double& v : a) { v /= mx; mean += v; }
    mean /= static_cast<double>(a.size());
    for (double v : a) st.std += (v - mean) * (v - mean);
    st.std = std::sqrt(st.std / static_cast<double>(a.size()));
    std::vector<double> sorted = a;
    std::sort(sorted.begin(), sorted.end());
    st.p5 = sorted[sorted.size() * 5 / 100];
    st.p50 = sorted[sorted.size() / 2];
    st.p95 = sorted[sorted.size() * 95 / 100];
    st.p90 = sorted[sorted.size() * 90 / 100];
    st.p99 = sorted[sorted.size() * 99 / 100];
    st.meanOverMax = mean;
    // Band energy.
    std::vector<std::complex<double>> grid(a.size());
    for (size_t i = 0; i < a.size(); ++i) grid[i] = a[i] - mean;
    std::vector<std::complex<double>> line(n);
    for (int y = 0; y < n; ++y) {
        for (int x = 0; x < n; ++x) line[x] = grid[static_cast<size_t>(y) * n + x];
        fft1(line, false);
        for (int x = 0; x < n; ++x) grid[static_cast<size_t>(y) * n + x] = line[x];
    }
    for (int x = 0; x < n; ++x) {
        for (int y = 0; y < n; ++y) line[y] = grid[static_cast<size_t>(y) * n + x];
        fft1(line, false);
        for (int y = 0; y < n; ++y) grid[static_cast<size_t>(y) * n + x] = line[y];
    }
    double total = 0;
    for (int y = 0; y < n; ++y)
        for (int x = 0; x < n; ++x) {
            const double fy = y < n / 2 ? y : y - n, fx = x < n / 2 ? x : x - n;
            const double r = std::sqrt(fx * fx + fy * fy), e = std::norm(grid[static_cast<size_t>(y) * n + x]);
            total += e;
            st.band[r <= 4 ? 0 : r <= 16 ? 1 : r <= 64 ? 2 : 3] += e;
        }
    for (double& b : st.band) b /= total;
    // Per band, how elongated the pattern is and which way it runs: the spectrum's second moment, each frequency weighted by its energy over r squared.
    const double lows[4] = {1, 4, 16, 64}, highs[4] = {4, 16, 64, 1e9};
    for (int bnd = 0; bnd < 4; ++bnd) {
        double xx = 0, yy = 0, xy = 0;
        for (int y = 0; y < n; ++y)
            for (int x = 0; x < n; ++x) {
                const double fy = y < n / 2 ? y : y - n, fx = x < n / 2 ? x : x - n;
                const double r2 = fx * fx + fy * fy, r = std::sqrt(r2);
                if (!(r > lows[bnd] && r <= highs[bnd])) continue;
                const double e = std::norm(grid[static_cast<size_t>(y) * n + x]) / r2;
                xx += e * fx * fx; yy += e * fy * fy; xy += e * fx * fy;
            }
        const double mid = 0.5 * (xx + yy), rad = std::sqrt(0.25 * (xx - yy) * (xx - yy) + xy * xy);
        st.elongation[bnd] = (mid + rad) / std::max(mid - rad, 1e-12);
        const double across = 0.5 * std::atan2(2 * xy, xx - yy) * 180.0 / 3.14159265358979323846;
        st.angle[bnd] = std::fmod(std::fmod(across + 90.0, 180.0) + 180.0, 180.0);
    }
    // Structure tensor on a window of n / 16 texels (at least 8), wrapping.
    const int w = std::max(8, n / 16);
    auto box = [&](const std::vector<double>& f) {
        std::vector<double> tmp(f.size()), out(f.size());
        for (int y = 0; y < n; ++y)
            for (int x = 0; x < n; ++x) {
                double sum = 0;
                for (int k = 0; k < w; ++k) sum += f[static_cast<size_t>(y) * n + (x + k) % n];
                tmp[static_cast<size_t>(y) * n + x] = sum / w;
            }
        for (int y = 0; y < n; ++y)
            for (int x = 0; x < n; ++x) {
                double sum = 0;
                for (int k = 0; k < w; ++k) sum += tmp[static_cast<size_t>((y + k) % n) * n + x];
                out[static_cast<size_t>(y) * n + x] = sum / w;
            }
        return out;
    };
    std::vector<double> gxx(a.size()), gyy(a.size()), gxy(a.size());
    for (int y = 0; y < n; ++y)
        for (int x = 0; x < n; ++x) {
            const double gx = (a[static_cast<size_t>(y) * n + (x + 1) % n] - a[static_cast<size_t>(y) * n + (x + n - 1) % n]) / 2;
            const double gy = (a[static_cast<size_t>((y + 1) % n) * n + x] - a[static_cast<size_t>((y + n - 1) % n) * n + x]) / 2;
            gxx[static_cast<size_t>(y) * n + x] = gx * gx; gyy[static_cast<size_t>(y) * n + x] = gy * gy; gxy[static_cast<size_t>(y) * n + x] = gx * gy;
        }
    const std::vector<double> jxx = box(gxx), jyy = box(gyy), jxy = box(gxy);
    std::complex<double> acc(0);
    double localSum = 0;
    for (size_t i = 0; i < a.size(); ++i) {
        const double theta = 0.5 * std::atan2(2 * jxy[i], jxx[i] - jyy[i]);
        const double loc = std::sqrt((jxx[i] - jyy[i]) * (jxx[i] - jyy[i]) + 4 * jxy[i] * jxy[i]) / (jxx[i] + jyy[i] + 1e-12);
        acc += std::polar(loc, 2 * theta);
        localSum += loc;
    }
    st.local = localSum / static_cast<double>(a.size());
    st.global = std::abs(acc) / static_cast<double>(a.size()) / std::max(st.local, 1e-12);
    // Bristles: high-pass the image (minus a 5 x 5 mean), then compare the gradient energy along the nominal stroke direction with the energy across it.
    {
        const double ang = edvr::glass_streaks::kFlowAngle, dx = std::cos(ang), dy = std::sin(ang);
        std::vector<double> hp(a.size());
        for (int y = 0; y < n; ++y)
            for (int x = 0; x < n; ++x) {
                double m5 = 0;
                for (int j = -2; j <= 2; ++j)
                    for (int i = -2; i <= 2; ++i) m5 += a[static_cast<size_t>((y + j + n) % n) * n + (x + i + n) % n];
                hp[static_cast<size_t>(y) * n + x] = a[static_cast<size_t>(y) * n + x] - m5 / 25.0;
            }
        double along = 0, across = 0;
        for (int y = 0; y < n; ++y)
            for (int x = 0; x < n; ++x) {
                const double gx = (hp[static_cast<size_t>(y) * n + (x + 1) % n] - hp[static_cast<size_t>(y) * n + (x + n - 1) % n]) / 2;
                const double gy = (hp[static_cast<size_t>((y + 1) % n) * n + x] - hp[static_cast<size_t>((y + n - 1) % n) * n + x]) / 2;
                const double ga = gx * dx + gy * dy, gc = -gx * dy + gy * dx;
                along += ga * ga;
                across += gc * gc;
            }
        st.bristle = along / std::max(across, 1e-12);
    }
    return st;
}

// ---- the streak generator ----------------------------------------------------------------------------------------------
static void testStreaks(const char* dumpPath) {
    namespace gs = edvr::glass_streaks;
    const int n = gs::kSize;
    std::vector<uint8_t> a(static_cast<size_t>(n) * n), b(a.size());
    const auto t0 = std::chrono::steady_clock::now();
    gs::generate(a.data());
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    gs::generate(b.data());
    check(a == b, "the generator is deterministic: two calls give the same bytes");
    double mean = 0;
    int max = 0;
    for (uint8_t v : a) { mean += v; max = v > max ? v : max; }
    mean /= static_cast<double>(a.size()) * 255.0;
    check(max == 255, "the brightest texel is 255 (stored normalised)");
    check(std::fabs(mean - gs::kTargetMean) < 0.005, "the stored mean is the legacy mean over the legacy max (0.024 / 0.09)");
    check(std::fabs(mean * gs::kStreakMax - gs::kLegacyMean) < 0.0005 && gs::kStreakMax == gs::kLegacyMax, "times the shader's scale, the linear mean is the legacy 0.024 and the max the legacy 0.09");
    auto at = [&](int x, int y) { return static_cast<double>(a[static_cast<size_t>(((y % n) + n) % n) * n + ((x % n) + n) % n]) / 255.0; };
    double interior = 0, seam = 0;
    for (int y = 0; y < n; ++y)
        for (int x = 0; x < n - 1; ++x) interior += std::fabs(at(x, y) - at(x + 1, y));
    for (int y = 0; y < n - 1; ++y)
        for (int x = 0; x < n; ++x) interior += std::fabs(at(x, y) - at(x, y + 1));
    interior /= 2.0 * static_cast<double>(n) * (n - 1);
    for (int y = 0; y < n; ++y) seam += std::fabs(at(0, y) - at(n - 1, y));
    for (int x = 0; x < n; ++x) seam += std::fabs(at(x, 0) - at(x, n - 1));
    seam /= 2.0 * n;
    check(seam <= 1.2 * interior, "it tiles: the difference across the wrap is no larger than between neighbours inside");
    // The character: the figures of the legacy dirt texture that section 13 of the doc measured (summary statistics only; the generator never saw its pixels).
    const StreakStats st = streakStats(a, n);
    std::printf("streaks: %dx%d generated in %.1f ms, stored mean %.4f; std %.3f, p5 %.3f p50 %.3f p95 %.3f; energy by cycles per tile 0-4 %.3f 4-16 %.3f 16-64 %.3f 64+ %.3f; "
                "coherence local %.3f global %.3f; elongation %.1f %.1f %.1f %.1f, stroke angle %.1f %.1f %.1f %.1f; bristle ratio %.2f; neighbour difference %.4f, seam difference %.4f\n",
                n, n, ms, mean, st.std, st.p5, st.p50, st.p95, st.band[0], st.band[1], st.band[2], st.band[3], st.local, st.global, st.elongation[0], st.elongation[1], st.elongation[2], st.elongation[3], st.angle[0], st.angle[1], st.angle[2], st.angle[3], st.bristle, interior, seam);
    check(std::fabs(st.std - 0.093) < 0.02, "the contrast is the legacy texture's (std about 0.093 of the max)");
    // The distribution relative to the mean (section 13): the legacy texture's percentile ratios p5 0.571, p50 0.884, p90 1.430, p95 1.591, p99 2.135, each within 8%, so the
    // bright parts are as bright, relative to the mean, as legacy's and no more.
    const double ratios[5] = {st.p5 / st.meanOverMax, st.p50 / st.meanOverMax, st.p90 / st.meanOverMax, st.p95 / st.meanOverMax, st.p99 / st.meanOverMax};
    const double legacyRatios[5] = {0.571, 0.884, 1.430, 1.591, 2.135};
    bool ratiosOk = true;
    for (int i = 0; i < 5; ++i) ratiosOk = ratiosOk && std::fabs(ratios[i] - legacyRatios[i]) < 0.08 * legacyRatios[i];
    std::printf("percentile ratios to the mean: p5 %.3f p50 %.3f p90 %.3f p95 %.3f p99 %.3f (legacy 0.571 0.884 1.430 1.591 2.135)\n", ratios[0], ratios[1], ratios[2], ratios[3], ratios[4]);
    check(ratiosOk, "the percentiles over the mean are the legacy texture's (p5, p50, p90, p95, p99) within 8%: a floor, no black holes, no bright sweeps");
    // The spectrum figures are secondary sanity checks: a painted texture honestly gives other band energies and a more elongated, more coherent fine structure than the legacy
    // texture's (section 13), so the band energies are held to within 0.2 of legacy's (0.418, 0.236, 0.161, 0.185), every band must be elongated (at least 2) and run the legacy
    // texture's diagonal (within 15 degrees of 44.5, 53.4, 52.2, 55.0).
    const double legacyBand[4] = {0.418, 0.236, 0.161, 0.185}, legacyAngle[4] = {44.5, 53.4, 52.2, 55.0};
    bool bands = true, strokesOk = true;
    for (int i = 0; i < 4; ++i) {
        bands = bands && std::fabs(st.band[i] - legacyBand[i]) < 0.2;
        strokesOk = strokesOk && st.elongation[i] >= 2.0 && std::fabs(st.angle[i] - legacyAngle[i]) < 15.0;
    }
    check(bands, "the energy by band is the legacy texture's within 0.2");
    check(strokesOk, "each band is elongated (at least 2) and its strokes run the legacy texture's diagonal within 15 degrees");
    check(st.bristle < 0.5, "bristles exist: fine-band gradient energy along the stroke direction is well below the energy across it");
    check(st.global >= 0.85, "the strokes run one way: global direction agreement at least 0.85");
    check(st.local > 0.3 && st.local < 0.95, "the strokes are coherent locally (legacy 0.41; the painted wipes read higher)");
    std::vector<std::vector<uint8_t>> mips;
    gs::buildMips(a.data(), n, mips);
    check(static_cast<int>(mips.size()) == gs::kMips && mips.back().size() == 1, "the chain has 10 levels, 512 down to 1");
    // The patch multiplier the shader takes from mip kPatchMip (placement, section 13): 1 + contrast x (stored - mean) / mean, clamped to 0.4 .. 1.6. Over that mip's texels it
    // stays in range and averages about 1; and the shader's constants are the header's.
    {
        const std::vector<uint8_t>& m = mips[gs::kPatchMip];
        double lo = 1e9, hi = -1e9, sum = 0, mult_lo = 1e9, mult_hi = -1e9, mult_sum = 0;
        for (uint8_t v : m) {
            const double sv = v / 255.0, mult = std::min(1.6, std::max(0.4, 1.0 + gs::kPatchContrast * (sv - gs::kStreakStoredMean) / gs::kStreakStoredMean));
            lo = std::min(lo, sv); hi = std::max(hi, sv); sum += sv;
            mult_lo = std::min(mult_lo, mult); mult_hi = std::max(mult_hi, mult); mult_sum += mult;
        }
        std::printf("patch mip %d: %zu texels, stored %.3f to %.3f (mean %.4f); multiplier %.3f to %.3f, mean %.3f\n", gs::kPatchMip, m.size(), lo, hi, sum / m.size(), mult_lo, mult_hi,
                    mult_sum / m.size());
        check(mult_lo >= 0.4 && mult_hi <= 1.6 && std::fabs(mult_sum / m.size() - 1.0) < 0.03, "the patch multiplier stays within 0.4 to 1.6 over the mip and averages about 1");
        check(std::fabs(sum / m.size() - gs::kStreakStoredMean) < 0.003, "the texture's stored mean is the one the patch multiplier is normalised around");
        check(std::strstr(edvr::kCockpitGlassLegacyPsHlsl, "kPatchContrast = 0.7") && std::strstr(edvr::kCockpitGlassLegacyPsHlsl, "kPatchMip = 5.0") &&
                  std::strstr(edvr::kCockpitGlassLegacyPsHlsl, "kStreakStoredMean = 0.2687") && std::strstr(edvr::kCockpitGlassLegacyPsHlsl, "kPatchScale = 0.19"),
              "the shader's patch constants are the header's");
    }
    bool means = true;
    for (const auto& m : mips) {
        double mm = 0;
        for (uint8_t v : m) mm += v;
        means = means && std::fabs(mm / static_cast<double>(m.size()) / 255.0 - mean) < 0.01;
    }
    check(means, "every mip level keeps the mean");
    if (dumpPath) {
        FILE* f = std::fopen(dumpPath, "wb");
        if (f) { std::fwrite(a.data(), 1, a.size(), f); std::fclose(f); }
    }
}

// ---- the shader ----------------------------------------------------------------------------------------------------------
// The legacy model's lit normal on flat glass, for a vertex normal (0,0,1) in a view basis that is the identity: the glass is lit from inside, so it is -N.
static float expectedNdl(const Case& c) {
    const float n[3] = {0.0f, 0.0f, -1.0f};
    const float dot = -(n[0] * c.sunDir[0] + n[1] * c.sunDir[1] + n[2] * c.sunDir[2]);
    return dot < 0 ? 0 : (dot > 1 ? 1 : dot);
}

static void testShader() {
    Canopy canopy;
    // The measured values (section 10), as the test has them: the dirt mask multiplier and colour scale, the alpha base, the glare strengths and exponents, the streak max.
    const float kDirtMask = 0.025f, kDirtColour = 0.97f, kBaseAlpha = 0.02f, kStreakMax = 0.09f;
    const float vertexB = 0.72f;
    const float liveMask = std::fmin(std::fmax((vertexB - 0.2f) * 0.5f, 0.0f), 1.0f) * 0.5f;   // what live's vertex shader passes for B = 0.72 (CB2[6] = (.., .., 0.5, -0.2), CB2[0].x = 0.5)
    const float kDirtStrength = 4.0f;   // chosen, not measured
    const float dirtWeight = liveMask / 0.25f;   // live's mask over CB2[6].z * CB2[0].x: B - 0.2 = 0.52, no offset added back
    // Placement: grime = film + edge x smoothstep(dirtWeight / 0.8); the test's 1 x 1 streak texel is 1, so the patch multiplier is clamped to 1.6.
    auto grimeOf = [](float w) {
        const float t = std::fmin(std::fmax(w / 0.8f, 0.0f), 1.0f);
        return 0.4f + 0.3f * (t * t * (3.0f - 2.0f * t));
    };
    const float albedo = kStreakMax * (kDirtMask * grimeOf(dirtWeight) * 1.6f) * kDirtColour * kDirtStrength;
    const float sun = 1000.0f;

    Case none;   // no mask at all
    const Pixel zero = canopy.draw(none);
    check(zero.r == 0 && zero.g == 0 && zero.b == 0 && closeTo(zero.a, kBaseAlpha, 0.01f), "no mask draws the legacy alpha base and no colour");

    // Dirt under the sun: the dirt mask from live's mask, the streak texel 1 (so 0.09), Lambert on the legacy normal over pi, divided by the alpha written.
    Case dirt;
    dirt.masks[1] = liveMask;
    dirt.sunColour[0] = dirt.sunColour[1] = dirt.sunColour[2] = sun;
    dirt.sunDir[1] = -0.5f; dirt.sunDir[2] = 1.0f;
    const Pixel d = canopy.draw(dirt);
    const float ndl = expectedNdl(dirt);
    const float dirtRadiance = ndl * sun * kInvPi * albedo;
    const float dirtWant = dirtRadiance / kBaseAlpha / 256.0f;
    std::printf("dirt under the sun: got (%g %g %g %g), wanted rgb %g alpha %g (radiance %g)\n", d.r, d.g, d.b, d.a, dirtWant, kBaseAlpha, dirtRadiance);
    check(closeTo(d.a, kBaseAlpha, 0.01f), "the dirt does not occlude: alpha stays the legacy base");
    check(closeTo(d.r, dirtWant, 0.02f) && closeTo(d.g, dirtWant, 0.02f) && closeTo(d.b, dirtWant, 0.02f),
          "dirt under the sun is grey: streak x (0.025 x vertex B) x 0.97, Lambert on the legacy normal over pi, divided by the alpha, scaled by the HDR write scale");
    // Placement (CHOSEN, section 13): the film is everywhere, so a pixel with mask exactly 0 still gets dirt, mask 0 and mask 1e-4 give nearly the same (no step), and the edge
    // gather at its maximum is at most (0.4 + 0.3) / 0.4 times the film.
    {
        Case zeroMask = dirt, tiny = dirt, edgeMax = dirt;
        zeroMask.masks[1] = 0.0f;
        tiny.masks[1] = 1e-4f;
        edgeMax.masks[1] = 0.2f;   // dirtWeight 0.8: the gather is full
        const Pixel z0 = canopy.draw(zeroMask), t = canopy.draw(tiny), em = canopy.draw(edgeMax);
        const float filmWant = ndl * sun * kInvPi * (kStreakMax * (kDirtMask * grimeOf(0.0f) * 1.6f) * kDirtColour * kDirtStrength) / kBaseAlpha / 256.0f;
        std::printf("film: mask 0 gives %g (wanted %g), mask 1e-4 gives %g, edge at its maximum gives %g (%.3f times the film)\n", z0.r, filmWant, t.r, em.r, em.r / z0.r);
        check(z0.r > 0 && closeTo(z0.r, filmWant, 0.02f), "a pixel with mask exactly 0 still gets the film");
        check(closeTo(t.r, z0.r, 0.01f), "mask 0 and mask 1e-4 give nearly the same result: no step, no kink");
        check(em.r > z0.r && em.r <= z0.r * (0.4f + 0.3f) / 0.4f * 1.01f && closeTo(em.r, z0.r * 1.75f, 0.02f), "the edge gather at its maximum is no more than (0.4 + 0.3) / 0.4 times the film");
    }
    Case bare = dirt;
    bare.streaks = false;
    const Pixel nb = canopy.draw(bare);
    check(nb.r == 0 && nb.g == 0 && nb.b == 0 && closeTo(nb.a, kBaseAlpha, 0.01f), "with no streak texture the replacement draws without dirt (never stock)");

    // A cascade whose moment map holds an occluder in front of the pixel (shadowed) and one behind it (lit).
    Case shadowed = dirt;
    shadowed.cascades = 1;
    shadowed.position[0] = shadowed.position[1] = 0.1f; shadowed.position[2] = 5.0f;   // clip = (0.1, 0.1, 0.5): inside the cascade, zf = 0.5
    shadowed.occluder = 0.1f;
    const Pixel s = canopy.draw(shadowed);
    Case lit = shadowed;
    lit.occluder = 0.9f;
    const Pixel l = canopy.draw(lit);
    Case cascadeless = dirt;
    cascadeless.position[0] = cascadeless.position[1] = 0.1f;
    const Pixel c0 = canopy.draw(cascadeless);
    std::printf("shadow: unshadowed %g, occluder near the light %g, occluder behind the pixel %g\n", c0.r, s.r, l.r);
    check(std::isfinite(s.r) && std::isfinite(l.r), "the shadow solve is finite");
    check(s.r < 0.1f * c0.r, "an occluder between the light and the pixel darkens the dirt");
    check(closeTo(l.r, c0.r, 0.02f), "an occluder behind the pixel leaves the dirt as lit as no cascade");

    // A hangar: the sun off (colour 1e-13), a probe in the cluster. The cube is 0.25 grey: ambient = the cube(N) x the read scale 256 = 64, times the albedo,
    // not scaled by 1/pi.
    const float ambient = 0.25f * 256.0f;
    Case hangar = dirt;
    hangar.sunColour[0] = hangar.sunColour[1] = hangar.sunColour[2] = 1e-13f;
    hangar.probe = true;
    hangar.probeColour[0] = hangar.probeColour[1] = hangar.probeColour[2] = 0.25f;
    const Pixel h = canopy.draw(hangar);
    const float hangarWant = ambient * albedo / kBaseAlpha / 256.0f;
    std::printf("hangar: got (%g %g %g %g), wanted rgb %g\n", h.r, h.g, h.b, h.a, hangarWant);
    check(closeTo(h.r, hangarWant, 0.03f) && closeTo(h.a, kBaseAlpha, 0.01f), "a hangar canopy is lit by the probe's cube alone, through the cluster lists and the probe records");
    // The sun and the probe together: the direct light over pi and the ambient add before the albedo.
    Case both = dirt;
    both.probe = true;
    both.probeColour[0] = both.probeColour[1] = both.probeColour[2] = 0.25f;
    const Pixel bo = canopy.draw(both);
    const float bothWant = (ndl * sun * kInvPi + ambient) * albedo / kBaseAlpha / 256.0f;
    std::printf("dirt under the sun with a probe: got (%g %g %g %g), wanted rgb %g\n", bo.r, bo.g, bo.b, bo.a, bothWant);
    check(closeTo(bo.r, bothWant, 0.02f), "the sun's light over pi and the probe ambient add before the albedo");
    Case dark = hangar;
    dark.probe = false;
    const Pixel k = canopy.draw(dark);
    check(k.r == 0 && k.g == 0 && k.b == 0 && std::isfinite(k.a) && closeTo(k.a, kBaseAlpha, 0.01f), "with the sun off and no probe the dirt is dark and finite");

    // A crack: the vertex mask past its threshold and the crack texture's alpha full. The lit normal is the crack's (flat here, so the inward vertex normal),
    // alpha 1 (the crack on the base), the crack colour 0.3 lit by the whole sun over pi.
    Case crack = dirt;
    crack.masks[0] = 1.0f; crack.masks[1] = 0.0f;
    crack.crack = true;
    crack.sunDir[0] = 0.0f; crack.sunDir[1] = 0.0f; crack.sunDir[2] = 1.0f;
    const Pixel cr = canopy.draw(crack);
    std::printf("crack under the sun: got (%g %g %g %g), wanted rgb %g\n", cr.r, cr.g, cr.b, cr.a, 300.0f * kInvPi / 256.0f);
    check(closeTo(cr.a, 1.0f, 0.01f) && closeTo(cr.r, 300.0f * kInvPi / 256.0f, 0.02f), "a crack is opaque, in the crack colour, lit by the sun on the crack's normal over pi");
    // The crack glare at the measured values: strength 0.02, gloss 450, so (450 + 6) / 32 toward the light; the crack's normal faces the light's far side, so Lambert is 0.
    Case glare = crack;
    glare.sunDir[2] = -1.0f;
    const Pixel g = canopy.draw(glare);
    const float crackGlare = sun * kInvPi * 0.02f * (450.0f + 6.0f) / 32.0f / 256.0f;
    std::printf("glare in a crack: got (%g %g %g %g), wanted rgb %g\n", g.r, g.g, g.b, g.a, crackGlare);
    check(closeTo(g.r, crackGlare, 0.02f), "the crack's glare lobe is normal-independent: 0.02 x (450 + 6) / 32 toward the light, over pi");
    // The smudge glare at the measured values: mask 0.5 of live's imperfection mask, strength 0.001, gloss 120, the light straight ahead (dot(L, V) = 1). It adds light and
    // does not occlude: alpha stays the base.
    Case smudge;
    smudge.masks[2] = 0.5f;
    smudge.sunColour[0] = smudge.sunColour[1] = smudge.sunColour[2] = sun;
    smudge.sunDir[2] = -1.0f;
    const Pixel sm = canopy.draw(smudge);
    const float smudgeRadiance = 0.5f * 0.001f * (120.0f + 6.0f) / 32.0f * sun * kInvPi;
    std::printf("smudge glare: got (%g %g %g %g), wanted rgb %g (radiance %g)\n", sm.r, sm.g, sm.b, sm.a, smudgeRadiance / kBaseAlpha / 256.0f, smudgeRadiance);
    check(closeTo(sm.a, kBaseAlpha, 0.01f) && closeTo(sm.r, smudgeRadiance / kBaseAlpha / 256.0f, 0.03f), "the smudge glare is 0.5 x 0.001 x (120 + 6) / 32 over pi, adds light and does not occlude");
    // The tint alpha replaces the base when it is larger: alpha = tint, and the colour is divided by it.
    Case tinted = dirt;
    tinted.tint = 0.5f;
    const Pixel ti = canopy.draw(tinted);
    check(closeTo(ti.a, 0.5f, 0.01f) && closeTo(ti.r, dirtRadiance / 0.5f / 256.0f, 0.02f), "a tint alpha above the base is the alpha, and the colour is divided by it");

    // The overlays: the same dirt light, premultiplied (no division), alpha written 0, from their own vertex shaders' mask orders and texture slots. The intact pixel's
    // alpha times colour is what the blend adds, and it is what an overlay writes.
    const float intactAdded = d.r * d.a;
    Case ice = dirt;
    ice.variant = kIce;
    ice.masks[0] = 0.0f; ice.masks[1] = 0.5f; ice.masks[2] = liveMask; ice.masks[3] = 0.0f;   // (crack, ice, dirt, imperfection)
    const Pixel io = canopy.draw(ice);
    std::printf("ice overlay: got (%g %g %g %g), wanted rgb %g alpha 0\n", io.r, io.g, io.b, io.a, dirtRadiance / 256.0f);
    check(io.a == 0 && closeTo(io.r, dirtRadiance / 256.0f, 0.02f) && closeTo(io.r, intactAdded, 0.02f),
          "the ice overlay adds the same light as the intact variant's dirt term, with alpha written 0");
    Case condensation = dirt;
    condensation.variant = kCondensation;
    condensation.masks[0] = 0.0f; condensation.masks[1] = liveMask; condensation.masks[2] = 0.0f; condensation.masks[3] = 0.5f;   // (crack, dirt, imperfection, condensation)
    const Pixel co = canopy.draw(condensation);
    std::printf("condensation overlay: got (%g %g %g %g), wanted rgb %g alpha 0\n", co.r, co.g, co.b, co.a, dirtRadiance / 256.0f);
    check(co.a == 0 && closeTo(co.r, dirtRadiance / 256.0f, 0.02f), "the condensation overlay adds the same light from its own mask order, with alpha written 0");
    Case iceBare = ice;
    iceBare.masks[2] = 0.0f;
    const Pixel ib = canopy.draw(iceBare);
    const float ibWant = ndl * sun * kInvPi * (kStreakMax * (kDirtMask * grimeOf(0.0f) * 1.6f) * kDirtColour * kDirtStrength) / 256.0f;
    check(ib.a == 0 && closeTo(ib.r, ibWant, 0.03f), "an overlay pixel with no vertex mask still gets the film, and writes no alpha");
    Case iceNoStreaks = ice;
    iceNoStreaks.streaks = false;
    const Pixel ins = canopy.draw(iceNoStreaks);
    check(ins.r == 0 && ins.g == 0 && ins.b == 0, "with no streak texture the overlay adds no dirt");

    // The dirt level (visual.cockpit_glass_dirt / 100, b13): it scales the dirt albedo and nothing else, in the intact variant and in both overlays. 1 is the look as shipped.
    {
        Case half = dirt, twice = dirt, none0 = dirt, unbound = dirt;
        half.dirtLevel = 0.5f;
        twice.dirtLevel = 2.0f;
        none0.dirtLevel = 0.0f;
        unbound.levelBound = false;
        const Pixel lvHalf = canopy.draw(half), lvTwice = canopy.draw(twice), lvZero = canopy.draw(none0), lvUnbound = canopy.draw(unbound);
        std::printf("dirt level: 0 gives %g, 0.5 gives %g, 1 gives %g, 2 gives %g\n", lvZero.r, lvHalf.r, d.r, lvTwice.r);
        check(closeTo(lvHalf.r, d.r * 0.5f, 0.02f) && closeTo(lvTwice.r, d.r * 2.0f, 0.02f), "the dirt level scales the dirt: 50 % is half the light, 200 % twice");
        check(lvZero.r == 0 && lvZero.g == 0 && lvZero.b == 0 && closeTo(lvZero.a, kBaseAlpha, 0.01f), "a level of 0 is a clean pane: no dirt light, the legacy alpha base still drawn");
        check(lvUnbound.r == 0 && lvUnbound.g == 0 && lvUnbound.b == 0 && closeTo(lvUnbound.a, kBaseAlpha, 0.01f), "with no level block bound the replacement draws without dirt (never stock)");
        check(closeTo(lvHalf.a, kBaseAlpha, 0.01f) && closeTo(lvTwice.a, kBaseAlpha, 0.01f), "the level does not touch the alpha");
        Case iceTwice = ice, iceZero = ice, condensationTwice = condensation;
        iceTwice.dirtLevel = 2.0f;
        iceZero.dirtLevel = 0.0f;
        condensationTwice.dirtLevel = 2.0f;
        const Pixel lvIceTwice = canopy.draw(iceTwice), lvIceZero = canopy.draw(iceZero), lvCondTwice = canopy.draw(condensationTwice);
        check(closeTo(lvIceTwice.r, io.r * 2.0f, 0.02f) && closeTo(lvCondTwice.r, co.r * 2.0f, 0.02f), "both overlays take the level too: 200 % adds twice the dirt light");
        check(lvIceZero.r == 0 && lvIceZero.g == 0 && lvIceZero.b == 0 && lvIceZero.a == 0, "an overlay at a level of 0 adds nothing");
    }

    const unsigned problems = canopy.rig.problems();
    std::printf("D3D debug layer: %s\n", canopy.rig.debugLayer ? "on" : "not installed here (the signature check in temporal_shader_build still holds the link)");
    check(problems == 0, "the debug layer reports nothing: the draws link with the game's canopy, ice and condensation vertex shaders' outputs");
}

int main(int argc, char** argv) {
    testCore();
    testStreaks(argc > 2 && !std::strcmp(argv[1], "--dump") ? argv[2] : nullptr);
    testShader();
    std::printf("cockpit glass test: %s\n", g_failures ? "FAILED" : "passed");
    return g_failures ? 1 : 0;
}
