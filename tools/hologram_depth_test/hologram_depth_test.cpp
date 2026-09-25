// Exercise the generic hologram/icon depth pass (ui_depth.cpp, "GENERIC
// HOLOGRAM/ICON DEPTH COVERAGE") on D3D11 WARP, in the shape of
// tools\ui_depth_test: the production .cpp is included directly and this
// rig supplies its own binding shadow and the handful of cross-TU stubs
// it needs, so EDVR_BINDING_SHADOW_EXTERNAL asks the header for
// declarations rather than the inline production ones.
//
// The classifier (uiDepthHologramOnEyeDraw) reads the binding shadow,
// which is stubbed to abort here exactly as tools\ui_depth_test stubs it
// for the same production file's uiDepthOnEyeDraw -- these tests drive
// the two reissues and the resolve directly, the way a real draw's
// classify result (g_holoEye/g_holoW/g_holoH) would, and check the
// feature-off path through the classifier's own first line, which
// returns before touching the shadow at all.
#define EDVR_BINDING_SHADOW_EXTERNAL 1
#include "../../src/d3d11/ui_depth.cpp"
#include <d3dcompiler.h>
#include <d3d11sdklayers.h>
#include <wrl/client.h>
#include <DirectXPackedVector.h>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;
ComPtr<ID3DBlob> compile(const char*, const char*);
namespace edvr {
ID3D11Texture2D* testScene = nullptr;
std::string g_lastLog;
// ui_depth.cpp's UI content census reads this (objectProbeLedgerActive,
// object_probe.h) to gate its per-draw logging; this rig never arms an
// eye run, so it stays false -- object_probe.cpp itself is not linked in.
namespace detail { bool g_objectProbeOn = false; bool g_objectProbeLedgerOn = false; }
Log& Log::get() { static Log instance; return instance; }
Log::~Log() = default;
// Unlike tools\ui_depth_test's no-op stub, this one FORMATS the message:
// several tests here check the actual text (the census line, the canopy
// refusal), which nothing in that rig needed.
void Log::note(const char* fmt, ...) {
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    g_lastLog = buf;
}
int64_t qpcNow() { LARGE_INTEGER t; QueryPerformanceCounter(&t); return t.QuadPart; }
int64_t qpcFrequency() { LARGE_INTEGER t; QueryPerformanceFrequency(&t); return t.QuadPart; }
int guardFilter(unsigned long, const char*) { return EXCEPTION_EXECUTE_HANDLER; }
void FaultBudget::charge() { --m_remaining; }
// Configuration is outside these render-pass tests, exactly as in
// tools\ui_depth_test: abort rather than silently supply fake state.
// holoBuildFamilyList takes its spec as a plain string for exactly this
// reason -- it is tested directly, below, with no Config in the loop.
bool Config::getBool(const char*, bool) const { std::abort(); }
float Config::getFloat(const char*, float) const { std::abort(); }
std::string Config::getString(const char*, const char*) const { std::abort(); }
void* bindingGet(BindSlot) { std::abort(); }
uint32_t bindingGeneration(BindSlot) { std::abort(); }
uint64_t bindingShaderHash(BindSlot) { std::abort(); }
bool bindingResolve(void*, ResourceInfo*) { std::abort(); }
bool bindingResolveResource(void*, ResourceInfo*) { std::abort(); }
bool depthProbeIsSceneDepth(const void*) { std::abort(); }
uint64_t lookupShaderHash(void*) { std::abort(); }
ID3D11VertexShader* shaderSwapCompileVs(ID3D11DeviceContext* ctx, const char* source, size_t,
    const char*, const char*, const SwapMacro*, const char*) {
    auto code = ::compile(source, "vs_5_0");
    ComPtr<ID3D11Device> dev; ctx->GetDevice(&dev);
    ID3D11VertexShader* shader = nullptr;
    if (FAILED(dev->CreateVertexShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &shader))) std::abort();
    return shader;
}
ID3D11PixelShader* shaderSwapCompilePs(ID3D11DeviceContext* ctx, const char* source, size_t,
    const char*, const char*, const SwapMacro*, const char*) {
    auto code = ::compile(source, "ps_5_0");
    ComPtr<ID3D11Device> dev; ctx->GetDevice(&dev);
    ID3D11PixelShader* shader = nullptr;
    if (FAILED(dev->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &shader))) std::abort();
    return shader;
}
ID3D11ComputeShader* shaderSwapCompileCs(ID3D11DeviceContext*, const char*, size_t,
    const char*, const char*, const SwapMacro*, const char*) { std::abort(); }
float temporalPassDepthAt(float metres) { return 0.025f / metres; }
bool temporalPassPlanes(float* nearZ, float* farZ) { *nearZ = .025f; *farZ = 10000; return true; }
bool depthProbeSceneDepthFormat(uint32_t, uint32_t, int, ID3D11Texture2D** tex, uint32_t* fmt) {
    *tex = testScene; *fmt = DXGI_FORMAT_D32_FLOAT; return testScene != nullptr;
}
void vScreenSetRenderTargetsRaw(ID3D11DeviceContext* ctx, UINT n,
                               ID3D11RenderTargetView* const* rt, ID3D11DepthStencilView* ds) {
    ctx->OMSetRenderTargets(n, rt, ds);
}
void vScreenDrawRaw(ID3D11DeviceContext* ctx, UINT vertexCount, UINT startVertex) {
    ctx->Draw(vertexCount, startVertex);
}
void vScreenVSSetShaderRaw(ID3D11DeviceContext* ctx, ID3D11VertexShader* vs,
                           ID3D11ClassInstance* const* classInstances, UINT numClassInstances) {
    ctx->VSSetShader(vs, classInstances, numClassInstances);
}
void vScreenPSSetShaderRaw(ID3D11DeviceContext* ctx, ID3D11PixelShader* ps,
                           ID3D11ClassInstance* const* classInstances, UINT numClassInstances) {
    ctx->PSSetShader(ps, classInstances, numClassInstances);
}
void vScreenOMSetBlendStateRaw(ID3D11DeviceContext* ctx, ID3D11BlendState* state,
                               const float blendFactor[4], UINT sampleMask) {
    ctx->OMSetBlendState(state, blendFactor, sampleMask);
}
void vScreenUpdateSubresourceRaw(ID3D11DeviceContext* ctx, ID3D11Resource* dstResource,
                                 UINT dstSubresource, const D3D11_BOX* dstBox,
                                 const void* srcData, UINT srcRowPitch, UINT srcDepthPitch) {
    ctx->UpdateSubresource(dstResource, dstSubresource, dstBox, srcData, srcRowPitch, srcDepthPitch);
}
void vScreenRSSetViewportsRaw(ID3D11DeviceContext* ctx, UINT n, const D3D11_VIEWPORT* vps) {
    ctx->RSSetViewports(n, vps);
}
void vScreenClearRenderTargetViewRaw(ID3D11DeviceContext* ctx, ID3D11RenderTargetView* rtv,
                                     const float colour[4]) {
    ctx->ClearRenderTargetView(rtv, colour);
}
}  // namespace edvr

using namespace edvr;
int checks = 0;
void check(bool ok, const char* label) {
    ++checks;
    if (!ok) { std::printf("FAIL: %s\n", label); std::exit(1); }
}
void hr(HRESULT result) { check(SUCCEEDED(result), "D3D operation"); }
ComPtr<ID3DBlob> compile(const char* hlsl, const char* profile) {
    ComPtr<ID3DBlob> blob, errors;
    HRESULT result = D3DCompile(hlsl, std::strlen(hlsl), nullptr, nullptr, nullptr,
                                "main", profile, D3DCOMPILE_ENABLE_STRICTNESS, 0, &blob, &errors);
    if (FAILED(result) && errors) std::puts(static_cast<const char*>(errors->GetBufferPointer()));
    hr(result);
    return blob;
}
// Reads back an 8x8 single-channel float/typeless resource, row major --
// the same technique tools\ui_depth_test\ui_depth_test.cpp uses to read
// the private depth copy: CopyResource at the byte level between two
// textures of the same typeless format, then reinterpret the staged
// bytes as IEEE float, which is exactly what a D32_FLOAT/R32_FLOAT depth
// value already is underneath its typeless tag.
std::vector<float> readDepth(ID3D11Device* dev, ID3D11DeviceContext* ctx, ID3D11Resource* res) {
    ComPtr<ID3D11Texture2D> tex; hr(res->QueryInterface(IID_PPV_ARGS(&tex)));
    D3D11_TEXTURE2D_DESC td{}; tex->GetDesc(&td);
    td.Usage = D3D11_USAGE_STAGING; td.BindFlags = 0; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> stage; hr(dev->CreateTexture2D(&td, nullptr, &stage));
    ctx->CopyResource(stage.Get(), tex.Get());
    D3D11_MAPPED_SUBRESOURCE map{}; hr(ctx->Map(stage.Get(), 0, D3D11_MAP_READ, 0, &map));
    std::vector<float> values(td.Width * td.Height);
    for (UINT y = 0; y < td.Height; ++y) for (UINT x = 0; x < td.Width; ++x) {
        const auto* p = static_cast<const unsigned char*>(map.pData) + y * map.RowPitch + x * 4;
        values[y * td.Width + x] = *reinterpret_cast<const float*>(p);
    }
    ctx->Unmap(stage.Get(), 0);
    return values;
}

// The contribution scratch's R channel, RGBA16F -- for the one check that
// needs the raw accumulated light rather than what the resolve did with
// it (the alpha-regression case, where round 6's dark-pixel rule made
// coverage alone stop distinguishing a correct low contribution from a
// regressed one).
std::vector<float> readContribR(ID3D11Device* dev, ID3D11DeviceContext* ctx, ID3D11Resource* res) {
    ComPtr<ID3D11Texture2D> tex; hr(res->QueryInterface(IID_PPV_ARGS(&tex)));
    D3D11_TEXTURE2D_DESC td{}; tex->GetDesc(&td);
    td.Usage = D3D11_USAGE_STAGING; td.BindFlags = 0; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> stage; hr(dev->CreateTexture2D(&td, nullptr, &stage));
    ctx->CopyResource(stage.Get(), tex.Get());
    D3D11_MAPPED_SUBRESOURCE map{}; hr(ctx->Map(stage.Get(), 0, D3D11_MAP_READ, 0, &map));
    std::vector<float> values(td.Width * td.Height);
    for (UINT y = 0; y < td.Height; ++y) for (UINT x = 0; x < td.Width; ++x) {
        const auto* p = static_cast<const unsigned char*>(map.pData) + y * map.RowPitch + x * 8;
        values[y * td.Width + x] = DirectX::PackedVector::XMConvertHalfToFloat(*reinterpret_cast<const uint16_t*>(p));
    }
    ctx->Unmap(stage.Get(), 0);
    return values;
}

// The "game's" own draw, replayed by pureDrawReissue in production: a
// full-screen triangle (the same SV_VertexID trick as the resolve's own
// VS) at a controllable NDC z, with INDEPENDENT RGBA for its left
// (x<4) and right (x>=4) halves, so one draw covers two outcomes at once
// -- tools\ui_depth_test's own convention (its indices 1 and 6).
constexpr char kToyVsHlsl[] =
    "cbuffer C : register(b0) { float4 left; float4 right; float4 zPad; };\n"
    "float4 main(uint id : SV_VertexID) : SV_Position {\n"
    "    float2 uv = float2((id << 1) & 2, id & 2);\n"
    "    return float4(uv * 2.0 - 1.0, zPad.x, 1.0);\n"
    "}\n";
constexpr char kToyPsHlsl[] =
    "cbuffer C : register(b0) { float4 left; float4 right; float4 zPad; };\n"
    "float4 main(float4 pos : SV_Position) : SV_Target {\n"
    "    return pos.x < 4.0 ? left : right;\n"
    "}\n";

int main() {
    ComPtr<ID3D11Device> dev; ComPtr<ID3D11DeviceContext> ctx;
    D3D_FEATURE_LEVEL level;
    HRESULT created = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
        D3D11_CREATE_DEVICE_DEBUG, nullptr, 0, D3D11_SDK_VERSION, &dev, &level, &ctx);
    if (created == DXGI_ERROR_SDK_COMPONENT_MISSING)
        created = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0,
            nullptr, 0, D3D11_SDK_VERSION, &dev, &level, &ctx);
    hr(created);
    check(gpuTimingBind(dev.Get(), ctx.Get()), "bind canonical WARP timer owner");
    ComPtr<ID3D11InfoQueue> info; dev.As(&info);

    detail::g_uiDepthOn = true;
    detail::g_uiDepthStoodDown = false;

    // Test: the feature off -- declined at the classifier's own first
    // line, before it ever touches the (aborting) binding shadow; no
    // scratch has been allocated yet, so this also proves "off" makes
    // none, run before any other test would allocate one.
    {
        detail::g_holoDepthOn = false;
        g_holoEye = -1;
        check(!uiDepthHologramOnEyeDraw(ctx.Get()), "off: classifier declines when the feature is off");
        check(g_holoEye == -1, "off: no eye recorded when off");
        ID3D11ShaderResourceView* contribSrv = nullptr;
        check(!uiDepthHologramContribution(8, 8, 0, &contribSrv), "off: no contribution view published when off");
        check(g_holoScratch[0].contribTex == nullptr, "off: no scratch allocation while off");
        check(g_holoScratch[0].depthTex == nullptr, "off: no scratch allocation while off (depth half)");
        check(g_holoScratch[0].radiusTex == nullptr, "off: no scratch allocation while off (radius half)");
        g_lastLog.clear();
        holoDepthWindowTick(ctx.Get());
        check(g_lastLog.empty(), "off: the periodic census prints nothing");
    }
    detail::g_holoDepthOn = true;
    g_cockpitMetres = 10.0f;
    g_holoFloor = 0.05f;
    g_holoShare = 0.5f;

    // The scene: an 8x8 depth target the "game" already drew into, and
    // that later game draws would still test against.
    D3D11_TEXTURE2D_DESC sd{};
    sd.Width = sd.Height = 8; sd.MipLevels = sd.ArraySize = 1;
    sd.Format = DXGI_FORMAT_R32_TYPELESS; sd.SampleDesc.Count = 1;
    sd.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> sceneTex; hr(dev->CreateTexture2D(&sd, nullptr, &sceneTex));
    D3D11_DEPTH_STENCIL_VIEW_DESC dvd{};
    dvd.Format = DXGI_FORMAT_D32_FLOAT; dvd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
    ComPtr<ID3D11DepthStencilView> sceneDsv; hr(dev->CreateDepthStencilView(sceneTex.Get(), &dvd, &sceneDsv));
    testScene = sceneTex.Get();

    // The toy "game" colour target: typeless, like the real eye target
    // (R8G8B8A8_TYPELESS in the field), with both a plain and an sRGB
    // view over the SAME storage -- whichever the "game" binds for a
    // draw decides that draw's blend space.
    D3D11_TEXTURE2D_DESC td{};
    td.Width = td.Height = 8; td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_TYPELESS; td.SampleDesc.Count = 1;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> toyTex; hr(dev->CreateTexture2D(&td, nullptr, &toyTex));
    D3D11_RENDER_TARGET_VIEW_DESC rvUnorm{}; rvUnorm.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    rvUnorm.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    ComPtr<ID3D11RenderTargetView> toyRtvUnorm; hr(dev->CreateRenderTargetView(toyTex.Get(), &rvUnorm, &toyRtvUnorm));
    D3D11_RENDER_TARGET_VIEW_DESC rvSrgb{}; rvSrgb.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    rvSrgb.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    ComPtr<ID3D11RenderTargetView> toyRtvSrgb; hr(dev->CreateRenderTargetView(toyTex.Get(), &rvSrgb, &toyRtvSrgb));
    D3D11_SHADER_RESOURCE_VIEW_DESC svUnorm{}; svUnorm.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    svUnorm.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; svUnorm.Texture2D.MipLevels = 1;
    ComPtr<ID3D11ShaderResourceView> toySrvUnorm; hr(dev->CreateShaderResourceView(toyTex.Get(), &svUnorm, &toySrvUnorm));

    // The HDR scene target the elements actually blend into (float, no
    // sRGB anything -- R11G11B10_FLOAT in the field, R16G16B16A16_FLOAT
    // here) -- and a second copy with no SHADER_RESOURCE bind, for the
    // "unviewable target" case. Separate from the toy target above: the
    // whole point of this block is that this one and the "display"
    // texture below are NOT the same resource.
    D3D11_TEXTURE2D_DESC hd{};
    hd.Width = hd.Height = 8; hd.MipLevels = hd.ArraySize = 1;
    hd.Format = DXGI_FORMAT_R16G16B16A16_FLOAT; hd.SampleDesc.Count = 1;
    hd.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> hdrTex; hr(dev->CreateTexture2D(&hd, nullptr, &hdrTex));
    ComPtr<ID3D11RenderTargetView> hdrRtv; hr(dev->CreateRenderTargetView(hdrTex.Get(), nullptr, &hdrRtv));
    D3D11_TEXTURE2D_DESC hdNoSrv = hd; hdNoSrv.BindFlags = D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> hdrTexNoSrv; hr(dev->CreateTexture2D(&hdNoSrv, nullptr, &hdrTexNoSrv));
    ComPtr<ID3D11RenderTargetView> hdrRtvNoSrv; hr(dev->CreateRenderTargetView(hdrTexNoSrv.Get(), nullptr, &hdrRtvNoSrv));

    // A flat-colour "display" texture+SRV standing in for the submitted,
    // tonemapped image -- a UNORM resource wholly separate from the HDR
    // target above.
    auto makeDisplay = [&](float v) {
        D3D11_TEXTURE2D_DESC dd{};
        dd.Width = dd.Height = 8; dd.MipLevels = dd.ArraySize = 1;
        dd.Format = DXGI_FORMAT_R8G8B8A8_UNORM; dd.SampleDesc.Count = 1;
        dd.BindFlags = D3D11_BIND_SHADER_RESOURCE; dd.Usage = D3D11_USAGE_DEFAULT;
        const BYTE b = static_cast<BYTE>(v * 255.0f + 0.5f);
        BYTE pixels[8 * 8 * 4];
        for (int i = 0; i < 64; ++i) { pixels[i*4] = b; pixels[i*4+1] = b; pixels[i*4+2] = b; pixels[i*4+3] = 255; }
        D3D11_SUBRESOURCE_DATA sd{pixels, 8 * 4, 0};
        ComPtr<ID3D11Texture2D> tex; hr(dev->CreateTexture2D(&dd, &sd, &tex));
        ComPtr<ID3D11ShaderResourceView> srv; hr(dev->CreateShaderResourceView(tex.Get(), nullptr, &srv));
        return srv;
    };

    auto vsCode = compile(kToyVsHlsl, "vs_5_0");
    auto psCode = compile(kToyPsHlsl, "ps_5_0");
    ComPtr<ID3D11VertexShader> toyVs; ComPtr<ID3D11PixelShader> toyPs;
    hr(dev->CreateVertexShader(vsCode->GetBufferPointer(), vsCode->GetBufferSize(), nullptr, &toyVs));
    hr(dev->CreatePixelShader(psCode->GetBufferPointer(), psCode->GetBufferSize(), nullptr, &toyPs));
    D3D11_BUFFER_DESC bd{}; bd.ByteWidth = 48; bd.Usage = D3D11_USAGE_DEFAULT; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    ComPtr<ID3D11Buffer> cbuf; hr(dev->CreateBuffer(&bd, nullptr, &cbuf));
    D3D11_RASTERIZER_DESC rs{}; rs.FillMode = D3D11_FILL_SOLID; rs.CullMode = D3D11_CULL_NONE; rs.DepthClipEnable = TRUE;
    ComPtr<ID3D11RasterizerState> raster; hr(dev->CreateRasterizerState(&rs, &raster));
    D3D11_VIEWPORT vp8{0, 0, 8, 8, 0, 1};

    auto makeBlend = [&](BOOL enable, D3D11_BLEND src, D3D11_BLEND dst) {
        D3D11_BLEND_DESC d{};
        d.RenderTarget[0].BlendEnable = enable;
        d.RenderTarget[0].SrcBlend = src; d.RenderTarget[0].DestBlend = dst; d.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        d.RenderTarget[0].SrcBlendAlpha = src; d.RenderTarget[0].DestBlendAlpha = dst; d.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        d.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        ComPtr<ID3D11BlendState> state; hr(dev->CreateBlendState(&d, &state));
        return state;
    };
    ComPtr<ID3D11BlendState> blendSrcAlphaOne = makeBlend(TRUE, D3D11_BLEND_SRC_ALPHA, D3D11_BLEND_ONE);
    ComPtr<ID3D11BlendState> blendPremultiplied = makeBlend(TRUE, D3D11_BLEND_ONE, D3D11_BLEND_INV_SRC_ALPHA);

    ctx->IASetInputLayout(nullptr);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->RSSetState(raster.Get());
    ctx->RSSetViewports(1, &vp8);
    ctx->VSSetShader(toyVs.Get(), nullptr, 0);
    ctx->PSSetShader(toyPs.Get(), nullptr, 0);
    ctx->VSSetConstantBuffers(0, 1, cbuf.GetAddressOf());
    ctx->PSSetConstantBuffers(0, 1, cbuf.GetAddressOf());

    uint32_t frame = 0;
    const float kBlack[4] = {0, 0, 0, 0};
    // Issues the "game's" own draw for real -- RTV bound (plain or sRGB
    // view), its own blend state, no depth-stencil view at all (several
    // covered families draw with depth off and nothing bound, which the
    // classifier now accepts) -- then g_holoEye/W/H stand in for a fresh
    // classify. clearColour null leaves the target's current content in
    // place (a second draw over the first, for the grey-background share
    // case); newFrame false accumulates onto the SAME eye/frame's
    // scratch, for the sun-corona case (two listed draws, one frame).
    auto drawIntoRtv = [&](ID3D11RenderTargetView* rtv, float z, const float leftRgba[4],
                          const float rightRgba[4], ID3D11BlendState* blend,
                          const float* clearColour, bool newFrame) {
        if (newFrame) { ++frame; g_frame = frame; }
        if (clearColour) ctx->ClearRenderTargetView(rtv, clearColour);
        float data[12] = {leftRgba[0], leftRgba[1], leftRgba[2], leftRgba[3],
                          rightRgba[0], rightRgba[1], rightRgba[2], rightRgba[3], z, 0, 0, 0};
        ctx->UpdateSubresource(cbuf.Get(), 0, nullptr, data, 0, 0);
        ctx->OMSetRenderTargets(1, &rtv, nullptr);
        ctx->OMSetBlendState(blend, nullptr, 0xFFFFFFFFu);
        ctx->Draw(3, 0);
        g_holoEye = 0; g_holoW = 8; g_holoH = 8;
    };
    auto originalDraw = [&](float z, const float leftRgba[4], const float rightRgba[4],
                            ID3D11BlendState* blend, bool srgbView, const float* clearColour, bool newFrame) {
        drawIntoRtv(srgbView ? toyRtvSrgb.Get() : toyRtvUnorm.Get(), z, leftRgba, rightRgba, blend, clearColour, newFrame);
    };
    auto listedReissue = [&]() {
        check(uiDepthHologramContributionBegin(ctx.Get()), "contribution begins");
        ctx->Draw(3, 0);
        uiDepthHologramContributionEnd(ctx.Get());
        check(uiDepthHologramElementDepthBegin(ctx.Get()), "element depth begins");
        ctx->Draw(3, 0);
        uiDepthHologramElementDepthEnd(ctx.Get());
    };
    auto privateDepth = [&]() {
        ID3D11ShaderResourceView* srv = nullptr;
        check(uiDepthTemporalDepth(8, 8, 0, sceneTex.Get(), &srv), "private depth published");
        ComPtr<ID3D11Resource> res; srv->GetResource(&res);
        return readDepth(dev.Get(), ctx.Get(), res.Get());
    };
    constexpr float kNear5m = 0.005f;    // 0.025 / 5
    constexpr float kNear50m = 0.0005f;  // 0.025 / 50, beyond the 10 m radius

    // T1/T2: a listed quad at cockpit depth (5 m, inside the radius) over
    // far scene depth (0 = reversed-Z far, "the sky") -- SRC_ALPHA/ONE,
    // alpha 1: bright left half (0.5) clears the 0.05 floor, dim right
    // (0.02) does not -- round 6 changed T2: dark, but inside a cockpit-
    // range element's own footprint, so it now takes the element's depth
    // too (the dark-pixel rule below), where it used to leave the sky's.
    {
        const float left[4] = {0.5f, 0.5f, 0.5f, 1.0f}, right[4] = {0.02f, 0.02f, 0.02f, 1.0f};
        originalDraw(kNear5m, left, right, blendSrcAlphaOne.Get(), false, kBlack, true);
        listedReissue();
        check(uiDepthHologramResolve(ctx.Get(), 0, sceneTex.Get(), 8, 8, nullptr), "resolve runs (T1/T2)");
        auto values = privateDepth();
        check(std::fabs(values[1] - kNear5m) < 1e-5f, "T1: bright half above the floor gets the element depth");
        check(std::fabs(values[6] - kNear5m) < 1e-5f, "T2: dark fringe inside the cockpit-range footprint now gets it too");
    }

    // Alpha regression: (1,1,1, a=0.01) under SRC_ALPHA/ONE contributes
    // 0.01*1 = 0.01, below the floor -- the bug the formula max(luma,
    // a*luma) missed (it is just luma, alpha never actually applied).
    // Round 6: dark-but-cockpit-range now covers regardless of exactly how
    // dark, so coverage alone no longer distinguishes 0.01 from a
    // regressed 1.0 here; read the raw contribution instead, which still
    // does.
    {
        g_uiDepth[0].frameBoundary(); g_uiDepth[1].frameBoundary();
        const float rgba[4] = {1.0f, 1.0f, 1.0f, 0.01f};
        originalDraw(kNear5m, rgba, rgba, blendSrcAlphaOne.Get(), false, kBlack, true);
        listedReissue();
        ID3D11ShaderResourceView* contribSrv = nullptr;
        check(uiDepthHologramContribution(8, 8, 0, &contribSrv), "alpha regression: raw contribution view published");
        ComPtr<ID3D11Resource> contribRes; contribSrv->GetResource(&contribRes);
        for (float v : readContribR(dev.Get(), ctx.Get(), contribRes.Get()))
            check(std::fabs(v - 0.01f) < 1e-3f, "alpha regression: contribution is alpha-weighted (0.01), not luma-only (1.0)");
        check(uiDepthHologramResolve(ctx.Get(), 0, sceneTex.Get(), 8, 8, nullptr), "resolve runs (alpha regression)");
        for (float v : privateDepth()) check(std::fabs(v - kNear5m) < 1e-5f, "alpha regression: dark, cockpit-range, now covered");
    }

    // Premultiplied: ONE/INV_SRC_ALPHA, (0.3,0.3,0.3,0.3) over black --
    // SrcBlend ONE is untouched by the DEST_* remap, so contribution is
    // the raw 0.3, above the floor.
    {
        g_uiDepth[0].frameBoundary(); g_uiDepth[1].frameBoundary();
        const float rgba[4] = {0.3f, 0.3f, 0.3f, 0.3f};
        originalDraw(kNear5m, rgba, rgba, blendPremultiplied.Get(), false, kBlack, true);
        listedReissue();
        check(uiDepthHologramResolve(ctx.Get(), 0, sceneTex.Get(), 8, 8, nullptr), "resolve runs (premultiplied)");
        for (float v : privateDepth()) check(std::fabs(v - kNear5m) < 1e-5f, "premultiplied: covered");
    }

    // Nothing listed: the scratch is never prepared this frame, so the
    // resolve declines outright and the private copy is exactly whatever
    // seeded it (simulating some OTHER, unrelated coverage that frame).
    {
        ++frame; g_frame = frame;
        g_uiDepth[0].frameBoundary(); g_uiDepth[1].frameBoundary();
        ctx->ClearDepthStencilView(sceneDsv.Get(), D3D11_CLEAR_DEPTH, 0.3f, 0);
        check(g_uiDepth[0].acquire(ctx.Get(), sceneTex.Get()) != nullptr,
              "nothing listed: private copy seeded (simulating unrelated coverage)");
        check(!uiDepthHologramResolve(ctx.Get(), 0, sceneTex.Get(), 8, 8, nullptr), "nothing listed: resolve declines");
        for (float v : privateDepth()) check(std::fabs(v - 0.3f) < 1e-6f, "nothing listed: depth untouched");
    }

    // Beyond radius: bright, but at 50 m against a 10 m radius -- the
    // contribution pass still clears/tests against radiusDepth, so this
    // element never contributes; its element depth DOES now write (the
    // scratch clears to 0, not radiusDepth, since a world marker below
    // needs exactly that), but with contribution E=0 the floor test's own
    // e-fallback (display is null in every case on this page) reads dark,
    // and cockpitRange is false at 50 m against the radius, so the resolve
    // discards it there now (round 6) rather than on the floor alone --
    // same outcome, confirmed by this same check still passing.
    {
        g_uiDepth[0].frameBoundary(); g_uiDepth[1].frameBoundary();
        ctx->ClearDepthStencilView(sceneDsv.Get(), D3D11_CLEAR_DEPTH, 0.0f, 0);
        const float rgba[4] = {0.5f, 0.5f, 0.5f, 1.0f};
        originalDraw(kNear50m, rgba, rgba, blendSrcAlphaOne.Get(), false, kBlack, true);
        listedReissue();
        check(uiDepthHologramResolve(ctx.Get(), 0, sceneTex.Get(), 8, 8, nullptr), "resolve runs (beyond radius)");
        for (float v : privateDepth()) check(v == 0.0f, "beyond radius: leaves the sky's depth");
    }

    // World marker: the identical 50 m draw, but classified as a world
    // marker (g_holoIsWorldMarker, set directly here exactly as g_holoEye
    // is elsewhere on this page -- the classifier itself is untestable
    // under the aborting binding shadow). ContributionBegin now picks the
    // DepthEnable-FALSE state, so this element DOES contribute, and its
    // own depth (50 m) is what the resolve stamps.
    {
        g_uiDepth[0].frameBoundary(); g_uiDepth[1].frameBoundary();
        ctx->ClearDepthStencilView(sceneDsv.Get(), D3D11_CLEAR_DEPTH, 0.0f, 0);
        const float rgba[4] = {0.5f, 0.5f, 0.5f, 1.0f};
        g_holoIsWorldMarker = true;
        originalDraw(kNear50m, rgba, rgba, blendSrcAlphaOne.Get(), false, kBlack, true);
        g_holoDrawVs = kHoloWorldMarkerReticle;
        listedReissue();
        check(uiDepthHologramResolve(ctx.Get(), 0, sceneTex.Get(), 8, 8, nullptr), "resolve runs (world marker)");
        for (float v : privateDepth())
            check(std::fabs(v - kNear50m) < 1e-5f, "world marker: beyond radius, covered with its own depth");

        // The same draw, but back to a cockpit family: DepthEnable TRUE
        // against the radius scratch again, so it is excluded exactly as
        // the plain "beyond radius" case above.
        g_uiDepth[0].frameBoundary(); g_uiDepth[1].frameBoundary();
        ctx->ClearDepthStencilView(sceneDsv.Get(), D3D11_CLEAR_DEPTH, 0.0f, 0);
        g_holoIsWorldMarker = false;
        originalDraw(kNear50m, rgba, rgba, blendSrcAlphaOne.Get(), false, kBlack, true);
        g_holoDrawVs = kHoloIconCore;
        listedReissue();
        check(uiDepthHologramResolve(ctx.Get(), 0, sceneTex.Get(), 8, 8, nullptr), "resolve runs (same draw, cockpit family)");
        for (float v : privateDepth())
            check(v == 0.0f, "world marker: the same draw classified as cockpit is not covered");
    }

    // Behind nearer scene: a listed element at 5 m, but the REAL scene
    // (the private copy's own seed) is nearer still (reversed-Z 0.5) --
    // the resolve's own GREATER test against the already-seeded private
    // copy is what excludes this, not anything upstream of it.
    {
        g_uiDepth[0].frameBoundary(); g_uiDepth[1].frameBoundary();
        ctx->ClearDepthStencilView(sceneDsv.Get(), D3D11_CLEAR_DEPTH, 0.5f, 0);
        const float rgba[4] = {0.5f, 0.5f, 0.5f, 1.0f};
        originalDraw(kNear5m, rgba, rgba, blendSrcAlphaOne.Get(), false, kBlack, true);
        listedReissue();
        check(uiDepthHologramResolve(ctx.Get(), 0, sceneTex.Get(), 8, 8, nullptr), "resolve runs (behind nearer)");
        for (float v : privateDepth()) check(std::fabs(v - 0.5f) < 1e-6f, "behind nearer scene: untouched");
    }

    // Share: the toy target and the toy display happen to be the same
    // resource here (target is auto-tracked from RTV0; display is passed
    // explicitly), so these are a degenerate but valid case -- the
    // dedicated HDR block below is where they differ. An element that
    // adds exactly +0.1 over an RT pre-cleared to 0.8 is not a big enough
    // share of the finished 0.9 (share 0.5 wants at least 0.45); the same
    // over black (finished 0.1) is; with display null the floor falls
    // back to the contribution's own space (the target is still viewable,
    // so the share test still runs, and still passes: the fallback
    // counter moves, not the no-target one).
    {
        ctx->ClearDepthStencilView(sceneDsv.Get(), D3D11_CLEAR_DEPTH, 0.0f, 0);
        const float rgba[4] = {0.1f, 0.1f, 0.1f, 1.0f};
        const float grey[4] = {0.8f, 0.8f, 0.8f, 1.0f};
        g_uiDepth[0].frameBoundary(); g_uiDepth[1].frameBoundary();
        originalDraw(kNear5m, rgba, rgba, blendSrcAlphaOne.Get(), false, grey, true);
        listedReissue();
        check(uiDepthHologramResolve(ctx.Get(), 0, sceneTex.Get(), 8, 8, toySrvUnorm.Get()), "resolve runs (share, over grey)");
        for (float v : privateDepth()) check(v == 0.0f, "share: +0.1 over 0.8 is not enough of the finished pixel");

        g_uiDepth[0].frameBoundary(); g_uiDepth[1].frameBoundary();
        originalDraw(kNear5m, rgba, rgba, blendSrcAlphaOne.Get(), false, kBlack, true);
        listedReissue();
        check(uiDepthHologramResolve(ctx.Get(), 0, sceneTex.Get(), 8, 8, toySrvUnorm.Get()), "resolve runs (share, over black)");
        for (float v : privateDepth()) check(std::fabs(v - kNear5m) < 1e-5f, "share: +0.1 over black is covered");

        g_uiDepth[0].frameBoundary(); g_uiDepth[1].frameBoundary();
        originalDraw(kNear5m, rgba, rgba, blendSrcAlphaOne.Get(), false, kBlack, true);
        listedReissue();
        const uint32_t fallbackBefore = g_holoWindowFloorFallback;
        check(uiDepthHologramResolve(ctx.Get(), 0, sceneTex.Get(), 8, 8, nullptr), "resolve runs (share, no display)");
        for (float v : privateDepth()) check(std::fabs(v - kNear5m) < 1e-5f, "share: null display still covers via the floor fallback");
        check(g_holoWindowFloorFallback == fallbackBefore + 1, "share: the floor-fallback counter moved exactly once");
    }

    // HDR: the target the elements blend into (float, no tonemapping) and
    // the display image (UNORM, tonemapped) are now genuinely different
    // resources at different brightness -- flight 20260924_155636's own
    // mismatch (HDR luma ~0.078, display luma ~0.273), reproduced here and
    // read correctly on both sides.
    {
        // 0.078 luma over black in the HDR target, display 0.27: the
        // flight's own failure -- the old code compared 0.078 against
        // 0.27 and refused it.
        g_uiDepth[0].frameBoundary(); g_uiDepth[1].frameBoundary();
        ctx->ClearDepthStencilView(sceneDsv.Get(), D3D11_CLEAR_DEPTH, 0.0f, 0);
        const float dim[4] = {0.078f, 0.078f, 0.078f, 1.0f};
        drawIntoRtv(hdrRtv.Get(), kNear5m, dim, dim, blendSrcAlphaOne.Get(), kBlack, true);
        listedReissue();
        auto display27 = makeDisplay(0.27f);
        check(uiDepthHologramResolve(ctx.Get(), 0, sceneTex.Get(), 8, 8, display27.Get()), "resolve runs (HDR, the flight's own case)");
        for (float v : privateDepth()) check(std::fabs(v - kNear5m) < 1e-5f, "HDR: 0.078 HDR luma against 0.27 display luma is covered");

        // +0.1 over an HDR target pre-filled to 0.8 (finished 0.9), display
        // 0.9: not covered, the share test in the target's own space.
        g_uiDepth[0].frameBoundary(); g_uiDepth[1].frameBoundary();
        const float grey[4] = {0.8f, 0.8f, 0.8f, 1.0f};
        const float add[4] = {0.1f, 0.1f, 0.1f, 1.0f};
        drawIntoRtv(hdrRtv.Get(), kNear5m, add, add, blendSrcAlphaOne.Get(), grey, true);
        listedReissue();
        auto display9 = makeDisplay(0.9f);
        check(uiDepthHologramResolve(ctx.Get(), 0, sceneTex.Get(), 8, 8, display9.Get()), "resolve runs (HDR, share)");
        for (float v : privateDepth()) check(v == 0.0f, "HDR: +0.1 over 0.8 is not enough of the finished HDR pixel");

        // Over black, display 0.02, floor 0.05: the floor reads the
        // DISPLAY, not the (otherwise ample) HDR light -- dark by that
        // measure. Round 6: dark inside this element's own footprint,
        // within the cockpit radius, now takes its depth anyway (an
        // exposure-dimmed pixel is exactly the "gap" case, just from
        // tonemapping instead of a text gap).
        g_uiDepth[0].frameBoundary(); g_uiDepth[1].frameBoundary();
        const float bright[4] = {0.5f, 0.5f, 0.5f, 1.0f};
        drawIntoRtv(hdrRtv.Get(), kNear5m, bright, bright, blendSrcAlphaOne.Get(), kBlack, true);
        listedReissue();
        auto display02 = makeDisplay(0.02f);
        check(uiDepthHologramResolve(ctx.Get(), 0, sceneTex.Get(), 8, 8, display02.Get()), "resolve runs (HDR, floor on display)");
        for (float v : privateDepth())
            check(std::fabs(v - kNear5m) < 1e-5f, "HDR: a dim display pixel inside the cockpit-range footprint is covered anyway");

        // A target with no SHADER_RESOURCE bind: the share test is skipped
        // outright (the no-target counter moves), the floor on the display
        // image alone still covers it.
        g_uiDepth[0].frameBoundary(); g_uiDepth[1].frameBoundary();
        drawIntoRtv(hdrRtvNoSrv.Get(), kNear5m, bright, bright, blendSrcAlphaOne.Get(), kBlack, true);
        listedReissue();
        const uint32_t noTargetBefore = g_holoWindowNoTarget;
        check(uiDepthHologramResolve(ctx.Get(), 0, sceneTex.Get(), 8, 8, display27.Get()), "resolve runs (HDR, unviewable target)");
        for (float v : privateDepth()) check(std::fabs(v - kNear5m) < 1e-5f, "HDR: an unviewable target is covered on the floor alone");
        check(g_holoWindowNoTarget == noTargetBefore + 1, "HDR: the no-target counter moved exactly once");

        // Display null over the (viewable) HDR target: the contribution
        // floor applies, and its counter moves.
        g_uiDepth[0].frameBoundary(); g_uiDepth[1].frameBoundary();
        drawIntoRtv(hdrRtv.Get(), kNear5m, bright, bright, blendSrcAlphaOne.Get(), kBlack, true);
        listedReissue();
        const uint32_t fallbackBefore2 = g_holoWindowFloorFallback;
        check(uiDepthHologramResolve(ctx.Get(), 0, sceneTex.Get(), 8, 8, nullptr), "resolve runs (HDR, null display)");
        for (float v : privateDepth()) check(std::fabs(v - kNear5m) < 1e-5f, "HDR: null display falls back to the contribution floor");
        check(g_holoWindowFloorFallback == fallbackBefore2 + 1, "HDR: the floor-fallback counter moved exactly once more");
    }

    // Sun corona: a bright listed quad beyond the radius, plus a listed
    // quad with zero light inside it, over the same pixels in the same
    // eye/frame. The far quad still fails the CONTRIBUTION pass's radius
    // test (E stays 0 throughout: its own brightness never reaches these
    // pixels, on its own -- see "beyond radius" above, where nothing
    // nearer overlaps it and it stays uncovered). Its element depth does
    // write (the scratch clears to 0), but the near quad's own element
    // depth (5 m, nearer) overwrites it: the resolved geometry at these
    // pixels is the NEAR quad's own, not the corona's. Round 6: that near
    // quad is genuinely dark and inside the cockpit radius -- exactly the
    // gap case -- so it now takes its own (5 m) depth, never the corona's
    // 50 m. This is the near quad's own dark pixel, not the corona
    // reaching anything; a corona with nothing nearer overlapping it is
    // still excluded ("beyond radius" above).
    {
        g_uiDepth[0].frameBoundary(); g_uiDepth[1].frameBoundary();
        ctx->ClearDepthStencilView(sceneDsv.Get(), D3D11_CLEAR_DEPTH, 0.0f, 0);
        const float bright[4] = {1.0f, 1.0f, 1.0f, 1.0f};
        originalDraw(kNear50m, bright, bright, blendSrcAlphaOne.Get(), false, kBlack, true);
        listedReissue();
        const float dark[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        originalDraw(kNear5m, dark, dark, blendSrcAlphaOne.Get(), false, nullptr, false);
        listedReissue();
        check(uiDepthHologramResolve(ctx.Get(), 0, sceneTex.Get(), 8, 8, nullptr), "resolve runs (sun corona)");
        for (float v : privateDepth())
            check(std::fabs(v - kNear5m) < 1e-5f, "sun corona: the near quad's own dark pixel takes its own depth, never the corona's");
    }

    // State: CULL_BACK and a 1x1 viewport bound before the resolve --
    // still covers the expected pixels (the resolve sets its own RS and
    // viewport), and afterwards RSGetState/RSGetViewports read back
    // exactly what was bound beforehand.
    {
        g_uiDepth[0].frameBoundary(); g_uiDepth[1].frameBoundary();
        ctx->ClearDepthStencilView(sceneDsv.Get(), D3D11_CLEAR_DEPTH, 0.0f, 0);
        const float rgba[4] = {0.5f, 0.5f, 0.5f, 1.0f};
        originalDraw(kNear5m, rgba, rgba, blendSrcAlphaOne.Get(), false, kBlack, true);
        listedReissue();
        D3D11_RASTERIZER_DESC cullBackDesc{};
        cullBackDesc.FillMode = D3D11_FILL_SOLID; cullBackDesc.CullMode = D3D11_CULL_BACK; cullBackDesc.DepthClipEnable = TRUE;
        ComPtr<ID3D11RasterizerState> cullBack; hr(dev->CreateRasterizerState(&cullBackDesc, &cullBack));
        ctx->RSSetState(cullBack.Get());
        D3D11_VIEWPORT tiny{0, 0, 1, 1, 0, 1};
        ctx->RSSetViewports(1, &tiny);
        check(uiDepthHologramResolve(ctx.Get(), 0, sceneTex.Get(), 8, 8, nullptr), "resolve runs under a hostile RS/viewport");
        for (float v : privateDepth()) check(std::fabs(v - kNear5m) < 1e-5f, "state: covered regardless of the externally bound RS/viewport");
        ComPtr<ID3D11RasterizerState> rsAfter; ctx->RSGetState(&rsAfter);
        check(rsAfter.Get() == cullBack.Get(), "state: RS restored to what was bound before the resolve");
        D3D11_VIEWPORT vpAfter[2]{}; UINT vpCountAfter = 2;
        ctx->RSGetViewports(&vpCountAfter, vpAfter);
        check(vpCountAfter == 1 && vpAfter[0].Width == 1 && vpAfter[0].Height == 1,
              "state: viewport restored to what was bound before the resolve");
        ctx->RSSetState(raster.Get());
        ctx->RSSetViewports(1, &vp8);
    }

    // sRGB: the game's own RTV0 view decides the blend space. A linear PS
    // value of 0.02, drawn through an sRGB view (so this scratch -- which
    // never auto-encodes -- holds it raw, i.e. linear), reads as display
    // brightness ~0.15 once encoded for the floor test: above a 0.1
    // floor. The identical value through a plain view is already
    // "display" as stored, 0.02: below it, dark -- and, round 6, inside
    // this cockpit-range element's own footprint, so it is covered anyway.
    {
        const float savedFloor = g_holoFloor;
        g_holoFloor = 0.1f;
        const float rgba[4] = {0.02f, 0.02f, 0.02f, 1.0f};
        g_uiDepth[0].frameBoundary(); g_uiDepth[1].frameBoundary();
        ctx->ClearDepthStencilView(sceneDsv.Get(), D3D11_CLEAR_DEPTH, 0.0f, 0);
        originalDraw(kNear5m, rgba, rgba, blendSrcAlphaOne.Get(), true /*sRGB view*/, kBlack, true);
        listedReissue();
        check(uiDepthHologramResolve(ctx.Get(), 0, sceneTex.Get(), 8, 8, nullptr), "resolve runs (sRGB view)");
        for (float v : privateDepth()) check(std::fabs(v - kNear5m) < 1e-5f, "sRGB: linear 0.02 through an sRGB view is covered");

        g_uiDepth[0].frameBoundary(); g_uiDepth[1].frameBoundary();
        originalDraw(kNear5m, rgba, rgba, blendSrcAlphaOne.Get(), false /*plain view*/, kBlack, true);
        listedReissue();
        check(uiDepthHologramResolve(ctx.Get(), 0, sceneTex.Get(), 8, 8, nullptr), "resolve runs (plain view)");
        for (float v : privateDepth())
            check(std::fabs(v - kNear5m) < 1e-5f, "sRGB: the same 0.02 through a plain view is dark, cockpit-range, and covered");
        g_holoFloor = savedFloor;
    }

    // Dark-pixel cockpit-range coverage (round 6): a panel with a bright
    // "glyph" half and a literal black "gap" half, within the cockpit
    // radius -- both take the panel's own depth now, where the gap used
    // to leave the sky's. (T2 above uses a near-black 0.02 fringe instead,
    // for the floor threshold itself; this is the literal glyph/gap
    // scenario flight 20260924_175113/20260925_050051 named.)
    {
        g_uiDepth[0].frameBoundary(); g_uiDepth[1].frameBoundary();
        ctx->ClearDepthStencilView(sceneDsv.Get(), D3D11_CLEAR_DEPTH, 0.0f, 0);
        const float glyph[4] = {0.6f, 0.6f, 0.6f, 1.0f}, gap[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        originalDraw(kNear5m, glyph, gap, blendSrcAlphaOne.Get(), false, kBlack, true);
        listedReissue();
        check(uiDepthHologramResolve(ctx.Get(), 0, sceneTex.Get(), 8, 8, nullptr), "resolve runs (glyph/gap, cockpit range)");
        for (float v : privateDepth())
            check(std::fabs(v - kNear5m) < 1e-5f, "glyph/gap: both the bright glyph and the black gap are covered within the cockpit radius");
    }

    // The same panel beyond the radius: the glyph is not covered (its
    // contribution is radius-gated to E=0, as in "beyond radius" above),
    // and now neither is the gap (cockpitRange is false at 50 m). A world
    // marker at the same range is unaffected: its glyph half still covers
    // (contribution is unconditional for a world marker), its gap half
    // still does not -- "far elements... never claim dark pixels" applies
    // to a world marker exactly as it does to a cockpit family.
    {
        g_uiDepth[0].frameBoundary(); g_uiDepth[1].frameBoundary();
        ctx->ClearDepthStencilView(sceneDsv.Get(), D3D11_CLEAR_DEPTH, 0.0f, 0);
        const float glyph[4] = {0.6f, 0.6f, 0.6f, 1.0f}, gap[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        originalDraw(kNear50m, glyph, gap, blendSrcAlphaOne.Get(), false, kBlack, true);
        listedReissue();
        check(uiDepthHologramResolve(ctx.Get(), 0, sceneTex.Get(), 8, 8, nullptr), "resolve runs (glyph/gap, beyond radius, cockpit family)");
        for (float v : privateDepth()) check(v == 0.0f, "glyph/gap beyond radius: neither half is covered for a cockpit family");

        g_uiDepth[0].frameBoundary(); g_uiDepth[1].frameBoundary();
        ctx->ClearDepthStencilView(sceneDsv.Get(), D3D11_CLEAR_DEPTH, 0.0f, 0);
        g_holoIsWorldMarker = true;
        g_holoDrawVs = kHoloWorldMarkerReticle;
        originalDraw(kNear50m, glyph, gap, blendSrcAlphaOne.Get(), false, kBlack, true);
        listedReissue();
        check(uiDepthHologramResolve(ctx.Get(), 0, sceneTex.Get(), 8, 8, nullptr), "resolve runs (glyph/gap, beyond radius, world marker)");
        auto values = privateDepth();
        check(std::fabs(values[1] - kNear50m) < 1e-5f, "glyph/gap beyond radius, world marker: the bright glyph half still covers");
        check(values[6] == 0.0f, "glyph/gap beyond radius, world marker: the gap half still does not");
        g_holoIsWorldMarker = false;
    }

    // Star: a dark cockpit footprint (near-zero contribution everywhere)
    // with one bright background pixel already in the scene, showing
    // through -- the star itself must still fail the share test (its
    // brightness is not this element's own light), while the genuinely
    // dark pixels around it, now within the cockpit radius, are covered.
    {
        g_uiDepth[0].frameBoundary(); g_uiDepth[1].frameBoundary();
        ctx->ClearDepthStencilView(sceneDsv.Get(), D3D11_CLEAR_DEPTH, 0.0f, 0);
        const float dark[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        drawIntoRtv(hdrRtv.Get(), kNear5m, dark, dark, blendSrcAlphaOne.Get(), kBlack, true);
        listedReissue();
        // A one-pixel "star" already in the scene, unrelated to this
        // element's own blend: written directly into the HDR target
        // through a 1x1 viewport at (3,3), never through the contribution
        // pass, so Contribution stays 0 there just like everywhere else.
        {
            const float star[4] = {0.9f, 0.9f, 0.9f, 1.0f};
            const float data[12] = {star[0], star[1], star[2], star[3], star[0], star[1], star[2], star[3], 0, 0, 0, 0};
            ctx->UpdateSubresource(cbuf.Get(), 0, nullptr, data, 0, 0);
            ctx->OMSetRenderTargets(1, hdrRtv.GetAddressOf(), nullptr);
            ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFFu);
            ctx->VSSetShader(toyVs.Get(), nullptr, 0);
            ctx->PSSetShader(toyPs.Get(), nullptr, 0);
            const D3D11_VIEWPORT starVp{3, 3, 1, 1, 0, 1};
            ctx->RSSetViewports(1, &starVp);
            ctx->Draw(3, 0);
            ctx->RSSetViewports(1, &vp8);
        }
        // The display mirrors the star: bright at (3,3), well under the
        // 0.05 floor everywhere else.
        std::vector<unsigned char> starDisplay(8 * 8 * 4, 2);
        for (unsigned c = 0; c < 4; ++c) starDisplay[4 * (3 * 8 + 3) + c] = 230;
        D3D11_TEXTURE2D_DESC dd{};
        dd.Width = dd.Height = 8; dd.MipLevels = dd.ArraySize = 1;
        dd.Format = DXGI_FORMAT_R8G8B8A8_UNORM; dd.SampleDesc.Count = 1;
        dd.BindFlags = D3D11_BIND_SHADER_RESOURCE; dd.Usage = D3D11_USAGE_DEFAULT;
        const D3D11_SUBRESOURCE_DATA sub{starDisplay.data(), 8 * 4, 0};
        ComPtr<ID3D11Texture2D> starTex; hr(dev->CreateTexture2D(&dd, &sub, &starTex));
        ComPtr<ID3D11ShaderResourceView> starSrv; hr(dev->CreateShaderResourceView(starTex.Get(), nullptr, &starSrv));
        check(uiDepthHologramResolve(ctx.Get(), 0, sceneTex.Get(), 8, 8, starSrv.Get()), "resolve runs (star)");
        auto values = privateDepth();
        for (unsigned y = 0; y < 8; ++y) for (unsigned x = 0; x < 8; ++x) {
            const bool starPixel = (x == 3 && y == 3);
            check(std::fabs(values[y * 8 + x] - (starPixel ? 0.0f : kNear5m)) < 1e-5f,
                  starPixel ? "star: the bright background pixel itself is not covered"
                            : "star: the dark pixels around it are covered");
        }
    }

    // The family builder covers the eleven built-ins (the holo panel, the
    // icon core, the corona, its two stalks, the target sphere and the
    // five contact markers) and refuses the canopy, however it is named,
    // and says so once. The world-marker list is separate, fixed, and
    // reported by its own function -- holoWorldMarkerList takes no spec,
    // so advanced.temporal_aa_hologram_families cannot add to it.
    {
        uint64_t fam[kMaxHashes];
        const uint32_t famCount = holoBuildFamilyList("8C091FFD08644E02", fam, kMaxHashes);
        check(famCount == 11, "family builder: eleven built-in families");
        check(inList(fam, famCount, kHoloTargetSphere), "family builder: the target sphere is built in");
        check(inList(fam, famCount, kHoloContactA) && inList(fam, famCount, kHoloContactE),
              "family builder: the contact markers are built in");
        check(!inList(fam, famCount, kHoloCanopy), "family builder: the canopy is refused");
        check(g_lastLog.find("canopy") != std::string::npos, "family builder: the refusal is logged");

        uint64_t world[kMaxHashes];
        const uint32_t worldCount = holoWorldMarkerList(world, kMaxHashes);
        check(worldCount == 1, "family builder: one world marker");
        check(inList(world, worldCount, kHoloWorldMarkerReticle),
              "family builder: the target reticle is the world marker");
        check(!inList(fam, famCount, kHoloWorldMarkerReticle),
              "family builder: the world marker is not one of the cockpit families");
    }

    // World markers, true depth: a broken game viewport (MinDepth ==
    // MaxDepth == 0, mechanism i, flight 20260924_175113's own reading on
    // the target reticle) is overridden for the element-depth pass alone,
    // and the marker's real depth recovers; a VS that writes z=0 itself
    // (mechanism ii) is not fixed by that override -- not covered, and its
    // occlusion query reads zero samples, the signature the census reports
    // as "element-depth samples p50 0".
    {
        g_holoWorldMarkerNoted = false;   // force a fresh one-time diagnostic line
        g_holoWindowMarkerDraws = 0;
        g_holoMarkerSampleCount = 0;
        g_uiDepth[0].frameBoundary(); g_uiDepth[1].frameBoundary();
        ctx->ClearDepthStencilView(sceneDsv.Get(), D3D11_CLEAR_DEPTH, 0.0f, 0);
        const float rgba[4] = {0.5f, 0.5f, 0.5f, 1.0f};
        const D3D11_VIEWPORT brokenVp{0, 0, 8, 8, 0, 0};   // mechanism (i): MinDepth == MaxDepth == 0
        ctx->RSSetViewports(1, &brokenVp);
        g_holoIsWorldMarker = true;
        g_holoDrawVs = kHoloWorldMarkerReticle;
        originalDraw(kNear50m, rgba, rgba, blendSrcAlphaOne.Get(), false, kBlack, true);
        listedReissue();
        check(uiDepthHologramResolve(ctx.Get(), 0, sceneTex.Get(), 8, 8, nullptr),
              "resolve runs (world marker, broken viewport)");
        for (float v : privateDepth())
            check(std::fabs(v - kNear50m) < 1e-5f,
                  "world marker: a MinDepth==MaxDepth==0 game viewport is overridden and the real depth recovers");
        check(g_lastLog.find("first world-marker draw") != std::string::npos,
              "world marker: the one-time diagnostic line fires");
        check(g_lastLog.find("depth 0.000..0.000") != std::string::npos,
              "world marker: the diagnostic names the broken viewport it saw");
        for (int tries = 0; tries < 50 && g_holoMarkerSampleCount == 0; ++tries) {
            holoPollQueries(ctx.Get());
            if (!g_holoMarkerSampleCount) Sleep(1);
        }
        check(g_holoMarkerSampleCount > 0 && g_holoMarkerSamples[g_holoMarkerSampleCount - 1] > 0,
              "world marker: the element-depth occlusion query sees the recovered geometry");
        ctx->RSSetViewports(1, &vp8);   // restore for every test after this one

        // Mechanism (ii): the VS itself outputs z=0 (this rig's toy VS
        // takes z as a direct per-draw input, kToyVsHlsl), a normal
        // viewport in place throughout. The override cannot fix this.
        g_holoMarkerSampleCount = 0;
        g_uiDepth[0].frameBoundary(); g_uiDepth[1].frameBoundary();
        ctx->ClearDepthStencilView(sceneDsv.Get(), D3D11_CLEAR_DEPTH, 0.0f, 0);
        g_holoIsWorldMarker = true;
        g_holoDrawVs = kHoloWorldMarkerReticle;
        originalDraw(0.0f, rgba, rgba, blendSrcAlphaOne.Get(), false, kBlack, true);
        listedReissue();
        check(uiDepthHologramResolve(ctx.Get(), 0, sceneTex.Get(), 8, 8, nullptr),
              "resolve runs (world marker, VS z=0)");
        for (float v : privateDepth()) check(v == 0.0f, "world marker: a VS that writes z=0 itself is not stamped");
        for (int tries = 0; tries < 50 && g_holoMarkerSampleCount == 0; ++tries) {
            holoPollQueries(ctx.Get());
            if (!g_holoMarkerSampleCount) Sleep(1);
        }
        check(g_holoMarkerSampleCount > 0 && g_holoMarkerSamples[g_holoMarkerSampleCount - 1] == 0,
              "world marker: z=0 in the VS reads zero element-depth samples even with the viewport override in place");
        g_holoIsWorldMarker = false;
    }

    // The periodic census prints while the key is on, once the window's
    // 30 s (wall clock) elapses -- forced by backdating the window's
    // start rather than waiting -- and prints nothing while it is off
    // (covered above, before any scratch existed).
    {
        g_holoWindowStartMs = GetTickCount64() - 30001;
        g_holoWindowListed = g_holoWindowResolved = 0;
        g_holoWindowNoTarget = g_holoWindowFloorFallback = 0;
        g_holoWindowDeclinedNotCleared = g_holoWindowDeclinedNoPrivate = 0;
        g_holoWindowDeclinedNoProjection = g_holoWindowDeclinedFault = 0;
        g_holoPixelSampleCount = 0;
        g_holoWindowMarkerDraws = 0;
        g_holoMarkerSampleCount = 0;
        g_lastLog.clear();
        holoDepthWindowTick(ctx.Get());
        check(g_lastLog.find("hologram depth:") != std::string::npos, "census: the line printed");
        check(g_lastLog.find("resolved eye-frames 0") != std::string::npos, "census: zero resolved eye-frames printed");
        check(g_lastLog.find("share test skipped 0 (no target view)") != std::string::npos, "census: zero skipped-share printed");
        check(g_lastLog.find("floor on contribution 0 (no display view)") != std::string::npos, "census: zero floor-fallback printed");
        check(g_lastLog.find("declined 0 ") != std::string::npos, "census: zero declined printed");
        check(g_lastLog.find("world markers 0.00 draws/frame") != std::string::npos, "census: zero world-marker draws printed");
        check(g_lastLog.find("element-depth samples p50 0 (occlusion, 0 sampled)") != std::string::npos,
              "census: zero world-marker samples printed");
        check(g_holoWindowStartMs != 0, "census: the window reset after printing");
    }

    if (info) for (UINT64 i = 0; i < info->GetNumStoredMessages(); ++i) {
        SIZE_T size = 0; info->GetMessage(i, nullptr, &size); std::vector<unsigned char> storage(size);
        auto* m = reinterpret_cast<D3D11_MESSAGE*>(storage.data()); hr(info->GetMessage(i, m, &size));
        if (m->Severity <= D3D11_MESSAGE_SEVERITY_WARNING) {
            std::puts(m->pDescription); check(false, "D3D debug-layer warning/error");
        }
    }
    ctx->ClearState(); uiDepthShutdown();
    check(gpuTimingShutdown(ctx.Get()), "explicit shared timer shutdown before WARP release");
    std::printf("PASS: %d checks; the generic hologram/icon depth pass mirrors the game's own blend "
                "(including alpha), gates cockpit families by the cockpit radius before accumulation "
                "while a world marker contributes at any range, reads its floor off the displayed "
                "image and its share off the game's own HDR target (never each other), restores every "
                "piece of state it touches, and the family builder and periodic census behave.\n", checks);
    return 0;
}
