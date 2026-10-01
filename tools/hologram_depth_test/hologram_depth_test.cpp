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
ID3D11VertexShader* shaderSwapCreateVs(ID3D11DeviceContext* ctx,const void* bytes,size_t size,const char*,const char*) {
    ComPtr<ID3D11Device> dev;ctx->GetDevice(&dev);ID3D11VertexShader* shader=nullptr;
    if(FAILED(dev->CreateVertexShader(bytes,size,nullptr,&shader)))return nullptr;return shader;
}
ID3D11PixelShader* shaderSwapCreatePs(ID3D11DeviceContext* ctx,const void* bytes,size_t size,const char*,const char*) {
    ComPtr<ID3D11Device> dev;ctx->GetDevice(&dev);ID3D11PixelShader* shader=nullptr;
    if(FAILED(dev->CreatePixelShader(bytes,size,nullptr,&shader)))return nullptr;return shader;
}

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
ID3D11ComputeShader* shaderSwapCreateCs(ID3D11DeviceContext* ctx, const void* bytecode, size_t size,
    const char*, const char*) {
    ComPtr<ID3D11Device> dev;ctx->GetDevice(&dev);ID3D11ComputeShader* shader=nullptr;
    if(FAILED(dev->CreateComputeShader(bytecode,size,nullptr,&shader)))std::abort();return shader;
}
ID3D11ComputeShader* shaderSwapCompileCs(ID3D11DeviceContext* ctx, const char* source, size_t,
    const char*, const char*, const SwapMacro*, const char*) {
    auto code = ::compile(source, "cs_5_0");
    ComPtr<ID3D11Device> dev; ctx->GetDevice(&dev);
    ID3D11ComputeShader* shader = nullptr;
    if (FAILED(dev->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &shader))) std::abort();
    return shader;
}
float temporalPassDepthAt(float metres) { return 0.025f / metres; }
bool temporalPassPlanes(float* nearZ, float* farZ) { *nearZ = .025f; *farZ = 10000; return true; }
bool depthProbeSceneDepthFormat(uint32_t, uint32_t, int, ID3D11Texture2D** tex, uint32_t* fmt) {
    *tex = testScene; *fmt = DXGI_FORMAT_D32_FLOAT; return testScene != nullptr;
}
// Counts the one raw call that unbinds the whole output merger, no views and
// no depth view: the near-light pass's stage clear around its dispatch. The
// resolve's own draw keeps its depth view, and restoreOm hands over its saved
// array, so neither is counted; the near-light cases below read this.
unsigned g_rawOmFullClears = 0;
void vScreenSetRenderTargetsRaw(ID3D11DeviceContext* ctx, UINT n,
                               ID3D11RenderTargetView* const* rt, ID3D11DepthStencilView* ds) {
    if (n == 0 && !rt && !ds) ++g_rawOmFullClears;
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

// Observe production commands on the real WARP context, forwarding every
// call unchanged. Pixel readback helpers run outside this scope, so READ
// Maps and staging copies here belong to the production census alone.
struct CensusCommandSpy {
    using QueryFn = void (STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Asynchronous*);
    using GetDataFn = HRESULT (STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Asynchronous*, void*, UINT, UINT);
    using MapFn = HRESULT (STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT, D3D11_MAP, UINT, D3D11_MAPPED_SUBRESOURCE*);
    using CopyFn = void (STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, ID3D11Resource*);
    using DrawFn = void (STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT);
    using DispatchFn = void (STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT);
    static CensusCommandSpy* active;
    ID3D11DeviceContext* ctx;
    void** original;
    void* slots[128];
    unsigned begins = 0, ends = 0, polls = 0, readMaps = 0, stagingCopies = 0, draws = 0, dispatches = 0;
    static void begin(ID3D11DeviceContext* c, ID3D11Asynchronous* q) {
        ++active->begins; reinterpret_cast<QueryFn>(active->original[27])(c, q);
    }
    static void end(ID3D11DeviceContext* c, ID3D11Asynchronous* q) {
        ++active->ends; reinterpret_cast<QueryFn>(active->original[28])(c, q);
    }
    static HRESULT getData(ID3D11DeviceContext* c, ID3D11Asynchronous* q, void* data, UINT size, UINT flags) {
        ++active->polls; return reinterpret_cast<GetDataFn>(active->original[29])(c, q, data, size, flags);
    }
    static HRESULT map(ID3D11DeviceContext* c, ID3D11Resource* r, UINT sub, D3D11_MAP kind, UINT flags, D3D11_MAPPED_SUBRESOURCE* out) {
        if (kind == D3D11_MAP_READ) ++active->readMaps;
        return reinterpret_cast<MapFn>(active->original[14])(c, r, sub, kind, flags, out);
    }
    static void copy(ID3D11DeviceContext* c, ID3D11Resource* dst, ID3D11Resource* src) {
        D3D11_RESOURCE_DIMENSION kind; dst->GetType(&kind);
        if (kind == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
            D3D11_TEXTURE2D_DESC td{}; static_cast<ID3D11Texture2D*>(dst)->GetDesc(&td);
            if (td.Usage == D3D11_USAGE_STAGING) ++active->stagingCopies;
        }
        reinterpret_cast<CopyFn>(active->original[47])(c, dst, src);
    }
    static void draw(ID3D11DeviceContext* c, UINT n, UINT first) {
        ++active->draws; reinterpret_cast<DrawFn>(active->original[13])(c, n, first);
    }
    static void dispatch(ID3D11DeviceContext* c, UINT x, UINT y, UINT z) {
        ++active->dispatches; reinterpret_cast<DispatchFn>(active->original[41])(c, x, y, z);
    }
    void setTable(void** table) {
        DWORD old = 0, ignored = 0;
        check(VirtualProtect(ctx, sizeof(void*), PAGE_READWRITE, &old) != 0, "census spy: context vptr writable");
        *reinterpret_cast<void***>(ctx) = table;
        check(VirtualProtect(ctx, sizeof(void*), old, &ignored) != 0, "census spy: context protection restored");
    }
    explicit CensusCommandSpy(ID3D11DeviceContext* c) : ctx(c), original(*reinterpret_cast<void***>(c)) {
        check(active == nullptr, "census spy: one owner");
        std::memcpy(slots, original, sizeof(slots));
        slots[27] = reinterpret_cast<void*>(&begin); slots[28] = reinterpret_cast<void*>(&end);
        slots[29] = reinterpret_cast<void*>(&getData); slots[14] = reinterpret_cast<void*>(&map);
        slots[47] = reinterpret_cast<void*>(&copy); slots[13] = reinterpret_cast<void*>(&draw);
        slots[41] = reinterpret_cast<void*>(&dispatch);
        active = this; setTable(slots);
    }
    ~CensusCommandSpy() { setTable(original); active = nullptr; }
};
CensusCommandSpy* CensusCommandSpy::active = nullptr;
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
    // Not constexpr: reads g_cockpitMetres, set just above. The live call
    // (not a hand-computed literal) is what a covered dark pixel writes
    // now instead of its element's own depth (round 9).
    const float kFillerDepth = temporalPassDepthAt(g_cockpitMetres * kHoloFillerFraction);
    // A world marker hash never in g_holoMarkerDepthShaders, for the
    // fallback cases -- any value but kHoloWorldMarkerReticle's own.
    constexpr uint64_t kHoloUnlistedMarker = 0x1111111111111111ull;

    // The census must not change the rendered depth, and disabling it must
    // remove diagnostic commands rather than merely stop printing results.
    // Run on a sampled frame with both a world-marker and resolve query.
    {
        const float left[4] = {0.5f, 0.5f, 0.5f, 1.0f}, right[4] = {0.02f, 0.02f, 0.02f, 1.0f};
        std::vector<float> depths[2];
        for (unsigned pass = 0; pass < 2; ++pass) {
            holoDiagnosticsConfigure(pass != 0);
            frame = (frame | 15u) + 1u; g_frame = frame;
            ctx->ClearDepthStencilView(sceneDsv.Get(), D3D11_CLEAR_DEPTH, 0.0f, 0);
            originalDraw(kNear5m, left, right, blendSrcAlphaOne.Get(), false, kBlack, false);
            g_holoIsWorldMarker = true; g_holoDrawVs = kHoloUnlistedMarker;
            {
                CensusCommandSpy spy(ctx.Get());
                listedReissue();
                check(uiDepthHologramResolve(ctx.Get(), 0, sceneTex.Get(), 8, 8, nullptr), "census policy: production resolve runs");
                holoPollQueries(ctx.Get()); holoPollNearLightCounts(ctx.Get());
                check(spy.draws == 3 && spy.dispatches == 1, "census policy: both reissues and near-light/resolve run");
                if (pass == 0) {
                    check(spy.begins == 0 && spy.ends == 0 && spy.polls == 0, "census off: no query issue or poll");
                    check(spy.stagingCopies == 0 && spy.readMaps == 0, "census off: no staging copy or read Map");
                    for (auto* t : g_holoScratch[0].nearLightStage) check(t == nullptr, "census off: no staging allocation");
                } else {
                    check(spy.begins == 2 && spy.ends == 2 && spy.polls > 0, "census on: marker and resolve queries issued/polled");
                    check(spy.stagingCopies == 1 && spy.readMaps > 0, "census on: sampled map copy and nonwaiting read attempted");
                }
            }
            depths[pass] = privateDepth();
        }
        check(depths[0] == depths[1], "census policy: every production output depth agrees on/off");
        check(std::fabs(depths[0][1] - kNear5m) < 1e-5f && std::fabs(depths[0][6] - kFillerDepth) < 1e-5f,
              "census policy: bright depth and dark filler remain correct");

        // A scratch resize drops every query ring, the marker's included (it
        // used to release only the resolve's and leak the marker queries).
        // The census-on pass above left a marker query in the ring; hold one
        // reference of my own across the resize and read what is left.
        {
            ID3D11Query* marker = nullptr;
            for (auto* q : g_holoScratch[0].markerOcclusion) if (q) { marker = q; break; }
            check(marker != nullptr, "resize: the census-on pass left a marker query to watch");
            if (marker) {
                marker->AddRef();
                HoloScratch* resized = holoScratchFor(ctx.Get(), 0, 9, 9);
                check(resized != nullptr && resized->w == 9, "resize: the scratch is remade at the new size");
                const ULONG mine = (marker->AddRef(), marker->Release());
                check(mine == 1, "resize: the marker query ring's reference is released, not leaked");
                marker->Release();
                check(holoScratchFor(ctx.Get(), 0, 8, 8) != nullptr, "resize: back to the test's size");
            }
        }

        // Fill the census quota: the render path still runs, but no new
        // query or copy may be issued to throw its result away.
        g_holoPixelSampleCount = g_holoMarkerSampleCount = g_holoNearLightSampleCount = kHoloPixelSamples;
        frame = (frame | 15u) + 1u; g_frame = frame;
        originalDraw(kNear5m, left, right, blendSrcAlphaOne.Get(), false, kBlack, false);
        g_holoIsWorldMarker = true;
        {
            CensusCommandSpy spy(ctx.Get());
            listedReissue();
            check(uiDepthHologramResolve(ctx.Get(), 0, sceneTex.Get(), 8, 8, nullptr), "census quota: production resolve runs");
            check(spy.begins == 0 && spy.ends == 0 && spy.stagingCopies == 0, "census quota: no discarded query/copy work");
            check(spy.draws == 3 && spy.dispatches == 1, "census quota: output work retained");
        }
        // Leave commands explicitly pending before the live transition;
        // turning off must abandon them instead of polling stale samples.
        g_holoPixelSampleCount = g_holoMarkerSampleCount = g_holoNearLightSampleCount = 0;
        frame = (frame | 15u) + 1u; g_frame = frame;
        originalDraw(kNear5m, left, right, blendSrcAlphaOne.Get(), false, kBlack, false);
        listedReissue();
        check(uiDepthHologramResolve(ctx.Get(), 0, sceneTex.Get(), 8, 8, nullptr), "census live toggle: queued resolve");
        bool pending = false;
        for (unsigned i = 0; i < kHoloQueryRing; ++i)
            pending = pending || g_holoScratch[0].occlusionPending[i] || g_holoScratch[0].nearLightStagePending[i];
        check(pending, "census live toggle: actual commands pending before off");
        holoDiagnosticsConfigure(false);
        check(g_holoPixelSampleCount == 0 && g_holoMarkerSampleCount == 0 && g_holoNearLightSampleCount == 0,
              "census live off: prior samples cleared");
        for (const auto& s : g_holoScratch) for (unsigned i = 0; i < kHoloQueryRing; ++i)
            check(!s.occlusion[i] && !s.markerOcclusion[i] && !s.nearLightStage[i] &&
                  !s.occlusionPending[i] && !s.markerOcclusionPending[i] && !s.nearLightStagePending[i],
                  "census live off: resources and pending slots released");
        {
            frame = (frame | 15u) + 1u; g_frame = frame;
            originalDraw(kNear5m, left, right, blendSrcAlphaOne.Get(), false, kBlack, false);
            CensusCommandSpy spy(ctx.Get());
            listedReissue();
            check(uiDepthHologramResolve(ctx.Get(), 0, sceneTex.Get(), 8, 8, nullptr), "census live off: resolve continues");
            holoPollQueries(ctx.Get()); holoPollNearLightCounts(ctx.Get());
            g_holoWindowStartMs = GetTickCount64() - 30001;
            holoDepthWindowTick(ctx.Get());
            check(spy.begins == 0 && spy.ends == 0 && spy.polls == 0 && spy.readMaps == 0 && spy.stagingCopies == 0,
                  "census live off: no new commands or stale pending polling");
        }
        check(privateDepth() == depths[0], "census live off: output depth still equivalent");
        check(g_lastLog.find("GPU pixel census off") != std::string::npos &&
              g_lastLog.find("pixel counts unavailable") != std::string::npos &&
              g_lastLog.find("stamped pixels") == std::string::npos, "census off: heartbeat labels unavailable pixel counts");
        g_holoIsWorldMarker = false;
        // Existing numerical/census tests deliberately collect diagnostics.
        holoDiagnosticsConfigure(true);
        check(g_holoPixelSampleCount == 0 && g_holoMarkerSampleCount == 0 && g_holoNearLightSampleCount == 0,
              "census live on: starts with fresh samples");
    }

    // T1/T2: a listed quad at cockpit depth (5 m, inside the radius) over
    // far scene depth (0 = reversed-Z far, "the sky") -- SRC_ALPHA/ONE,
    // alpha 1: bright left half (0.5) clears the 0.05 floor, dim right
    // (0.02) does not, but is near-light covered (real light in the very
    // same, only, 8x8 block) so it takes the filler depth, not T1's own.
    {
        const float left[4] = {0.5f, 0.5f, 0.5f, 1.0f}, right[4] = {0.02f, 0.02f, 0.02f, 1.0f};
        originalDraw(kNear5m, left, right, blendSrcAlphaOne.Get(), false, kBlack, true);
        listedReissue();
        check(uiDepthHologramResolve(ctx.Get(), 0, sceneTex.Get(), 8, 8, nullptr), "resolve runs (T1/T2)");
        auto values = privateDepth();
        check(std::fabs(values[1] - kNear5m) < 1e-5f, "T1: bright half above the floor gets the element depth");
        check(std::fabs(values[6] - kFillerDepth) < 1e-5f, "T2: dark fringe near the bright half's light gets the filler depth");
    }

    // Alpha regression: (1,1,1, a=0.01) under SRC_ALPHA/ONE contributes
    // 0.01*1 = 0.01, below the floor -- the bug the formula max(luma,
    // a*luma) missed (it is just luma, alpha never actually applied).
    // Round 6 covered this (dark-but-cockpit-range, no distance limit),
    // which stopped distinguishing 0.01 from a regressed 1.0 by coverage
    // alone; round 7 narrows coverage back to near an element's own
    // light, and this whole quad is uniformly this dim colour -- no
    // pixel anywhere in it ever qualifies as light, so it goes back to
    // not covered, and the raw-contribution read added in round 6 is
    // what still guards the alpha formula itself.
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
        for (float v : privateDepth()) check(v == 0.0f, "alpha regression: dark with no light anywhere nearby is not covered");
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
    // own depth (50 m) is what the resolve stamps. kHoloUnlistedMarker,
    // not the reticle's own hash: this rig's toyVs has no TEXCOORD6/7, so
    // it cannot link against the reticle's own matched PS (round 8).
    {
        g_uiDepth[0].frameBoundary(); g_uiDepth[1].frameBoundary();
        ctx->ClearDepthStencilView(sceneDsv.Get(), D3D11_CLEAR_DEPTH, 0.0f, 0);
        const float rgba[4] = {0.5f, 0.5f, 0.5f, 1.0f};
        g_holoIsWorldMarker = true;
        originalDraw(kNear50m, rgba, rgba, blendSrcAlphaOne.Get(), false, kBlack, true);
        g_holoDrawVs = kHoloUnlistedMarker;
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
        // measure, and this whole quad's display is uniformly this dim,
        // so it is never near-light either (round 7): an exposure-dimmed
        // pixel with nothing bright nearby stays uncovered, same as
        // round 5.
        g_uiDepth[0].frameBoundary(); g_uiDepth[1].frameBoundary();
        const float bright[4] = {0.5f, 0.5f, 0.5f, 1.0f};
        drawIntoRtv(hdrRtv.Get(), kNear5m, bright, bright, blendSrcAlphaOne.Get(), kBlack, true);
        listedReissue();
        auto display02 = makeDisplay(0.02f);
        check(uiDepthHologramResolve(ctx.Get(), 0, sceneTex.Get(), 8, 8, display02.Get()), "resolve runs (HDR, floor on display)");
        for (float v : privateDepth())
            check(v == 0.0f, "HDR: a uniformly dim display, nowhere near light, is not covered");

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
    // pixels is the NEAR quad's own, not the corona's. Round 6 covered
    // that near quad's own dark pixel outright (dark-but-cockpit-range,
    // no distance limit); round 7 requires nearby light too, and this
    // near quad is uniformly black -- no light of its own anywhere -- so
    // it goes back to uncovered, the same as if the corona were not
    // there at all.
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
            check(v == 0.0f, "sun corona: the near quad's own dark pixel has no light of its own nearby, so stays uncovered");
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
    // "display" as stored, 0.02: below it, dark, and (round 7) uniformly
    // dim across the whole quad -- no light anywhere nearby, so it stays
    // uncovered.
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
            check(v == 0.0f, "sRGB: the same 0.02 through a plain view is dark with no light nearby, not covered");
        g_holoFloor = savedFloor;
    }

    // Dark-pixel cockpit-range coverage: a panel with a bright "glyph"
    // half and a literal black "gap" half, within the cockpit radius --
    // the glyph keeps the panel's own depth, the gap the filler, where it
    // used to leave the sky's. (T2 above uses a near-black 0.02 fringe
    // instead, for the floor threshold itself; this is the literal
    // glyph/gap scenario flight 20260924_175113/20260925_050051 named.)
    {
        g_uiDepth[0].frameBoundary(); g_uiDepth[1].frameBoundary();
        ctx->ClearDepthStencilView(sceneDsv.Get(), D3D11_CLEAR_DEPTH, 0.0f, 0);
        const float glyph[4] = {0.6f, 0.6f, 0.6f, 1.0f}, gap[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        originalDraw(kNear5m, glyph, gap, blendSrcAlphaOne.Get(), false, kBlack, true);
        listedReissue();
        check(uiDepthHologramResolve(ctx.Get(), 0, sceneTex.Get(), 8, 8, nullptr), "resolve runs (glyph/gap, cockpit range)");
        auto values = privateDepth();
        for (unsigned y = 0; y < 8; ++y) for (unsigned x = 0; x < 8; ++x) {
            const float expected = x < 4 ? kNear5m : kFillerDepth;
            check(std::fabs(values[y * 8 + x] - expected) < 1e-5f,
                  x < 4 ? "glyph/gap: the bright glyph keeps the panel's own depth within the cockpit radius"
                        : "glyph/gap: the black gap near it takes the filler depth within the cockpit radius");
        }
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
        g_holoDrawVs = kHoloUnlistedMarker;   // toyVs has no TEXCOORD6/7 (round 8)
        originalDraw(kNear50m, glyph, gap, blendSrcAlphaOne.Get(), false, kBlack, true);
        listedReissue();
        check(uiDepthHologramResolve(ctx.Get(), 0, sceneTex.Get(), 8, 8, nullptr), "resolve runs (glyph/gap, beyond radius, world marker)");
        auto values = privateDepth();
        check(std::fabs(values[1] - kNear50m) < 1e-5f, "glyph/gap beyond radius, world marker: the bright glyph half still covers");
        check(values[6] == 0.0f, "glyph/gap beyond radius, world marker: the gap half still does not");
        g_holoIsWorldMarker = false;
    }

    // Star: a glyph/gap element (as above) with one bright background
    // pixel already in the scene, in the gap half -- the star itself
    // must still fail the share test (its brightness is not this
    // element's own light), while the genuinely dark gap pixels around
    // it, near the glyph's real light in the same near-light block, take
    // the filler depth (the glyph itself keeps the element's own). The
    // element needs its own real light SOMEWHERE for near-light to cover
    // anything at all (a uniformly dark quad does not -- see the alpha
    // regression, HDR and sRGB cases above), so the glyph half is what
    // still makes this test meaningful.
    {
        g_uiDepth[0].frameBoundary(); g_uiDepth[1].frameBoundary();
        ctx->ClearDepthStencilView(sceneDsv.Get(), D3D11_CLEAR_DEPTH, 0.0f, 0);
        const float glyph[4] = {0.6f, 0.6f, 0.6f, 1.0f}, gap[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        drawIntoRtv(hdrRtv.Get(), kNear5m, glyph, gap, blendSrcAlphaOne.Get(), kBlack, true);
        listedReissue();
        // A one-pixel "star" already in the scene, unrelated to this
        // element's own blend: written directly into the HDR target
        // through a 1x1 viewport at (6,3) -- inside the gap half, away
        // from the glyph -- never through the contribution pass, so
        // Contribution stays 0 there just like the rest of the gap.
        {
            const float star[4] = {0.9f, 0.9f, 0.9f, 1.0f};
            const float data[12] = {star[0], star[1], star[2], star[3], star[0], star[1], star[2], star[3], 0, 0, 0, 0};
            ctx->UpdateSubresource(cbuf.Get(), 0, nullptr, data, 0, 0);
            ctx->OMSetRenderTargets(1, hdrRtv.GetAddressOf(), nullptr);
            ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFFu);
            ctx->VSSetShader(toyVs.Get(), nullptr, 0);
            ctx->PSSetShader(toyPs.Get(), nullptr, 0);
            const D3D11_VIEWPORT starVp{6, 3, 1, 1, 0, 1};
            ctx->RSSetViewports(1, &starVp);
            ctx->Draw(3, 0);
            ctx->RSSetViewports(1, &vp8);
        }
        // The display mirrors the scene: bright over the glyph half and
        // at the star, well under the 0.05 floor everywhere else in the
        // gap.
        std::vector<unsigned char> starDisplay(8 * 8 * 4, 2);
        for (unsigned y = 0; y < 8; ++y) for (unsigned x = 0; x < 4; ++x)
            for (unsigned c = 0; c < 4; ++c) starDisplay[4 * (y * 8 + x) + c] = 200;
        for (unsigned c = 0; c < 4; ++c) starDisplay[4 * (3 * 8 + 6) + c] = 230;
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
            const bool starPixel = (x == 6 && y == 3);
            const float expected = starPixel ? 0.0f : (x < 4 ? kNear5m : kFillerDepth);
            const char* label = starPixel ? "star: the bright background pixel itself is not covered"
                               : x < 4    ? "star: the glyph keeps the element's own depth"
                                          : "star: the dark gap pixels near the glyph take the filler depth";
            check(std::fabs(values[y * 8 + x] - expected) < 1e-5f, label);
        }
    }

    // The near-light radius, and the round-6 regression it fixes: a wide
    // (48x8, six blocks) panel lit only in its first 4 px (block 0). A
    // dark pixel 12 px from that light (x=15, block 1, block 0's
    // neighbour) takes the filler depth; one 30 px away (x=33, block 4)
    // does not, nor one 40 px away (x=43, block 5) -- open sky well
    // outside the light's own block or its neighbours, the flight
    // 20260925_080452 regression (a sky patch outside the weapons
    // panel's visible frame, inside its oversized null-PS footprint,
    // stamped by round 6's unbounded dark-pixel rule).
    //
    // Every earlier resolve above also queued a near-light census copy,
    // and nothing has polled the ring yet (production polls it every
    // frame boundary; this rig does not run one). A partial drain would
    // leave straggler slots still pending, marked pending forever until
    // some later poll catches them -- and the very next check below reads
    // g_holoNearLightSamples[0] expecting THIS test's own sample, not a
    // straggler from an unrelated earlier scene. Drain fully (three
    // stable rounds finding nothing new) before resetting the count.
    {
        const auto drainDeadline = GetTickCount64() + 2000;
        uint32_t totalDrained = 0, stableRounds = 0;
        while (stableRounds < 3 && GetTickCount64() < drainDeadline) {
            const uint32_t before = g_holoNearLightSampleCount;
            holoPollNearLightCounts(ctx.Get());
            const uint32_t got = g_holoNearLightSampleCount - before;
            totalDrained += got;
            stableRounds = got == 0 ? stableRounds + 1 : 0;
        }
        (void)totalDrained;
        bool anyPending = false;
        for (uint32_t i = 0; i < kHoloQueryRing; ++i) anyPending = anyPending || g_holoScratch[0].nearLightStagePending[i];
        check(!anyPending, "near-light radius: the census ring is drained before this test");
        g_holoNearLightSampleCount = 0;
    }
    {
        constexpr UINT kWideW = 48, kWideH = 8;
        D3D11_TEXTURE2D_DESC wsd{};
        wsd.Width = kWideW; wsd.Height = kWideH; wsd.MipLevels = wsd.ArraySize = 1;
        wsd.Format = DXGI_FORMAT_R32_TYPELESS; wsd.SampleDesc.Count = 1;
        wsd.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
        ComPtr<ID3D11Texture2D> wideScene; hr(dev->CreateTexture2D(&wsd, nullptr, &wideScene));
        D3D11_DEPTH_STENCIL_VIEW_DESC wdvd{};
        wdvd.Format = DXGI_FORMAT_D32_FLOAT; wdvd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
        ComPtr<ID3D11DepthStencilView> wideSceneDsv; hr(dev->CreateDepthStencilView(wideScene.Get(), &wdvd, &wideSceneDsv));
        D3D11_TEXTURE2D_DESC wtd{};
        wtd.Width = kWideW; wtd.Height = kWideH; wtd.MipLevels = wtd.ArraySize = 1;
        wtd.Format = DXGI_FORMAT_R8G8B8A8_UNORM; wtd.SampleDesc.Count = 1;
        wtd.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        ComPtr<ID3D11Texture2D> wideToy; hr(dev->CreateTexture2D(&wtd, nullptr, &wideToy));
        ComPtr<ID3D11RenderTargetView> wideToyRtv; hr(dev->CreateRenderTargetView(wideToy.Get(), nullptr, &wideToyRtv));

        g_uiDepth[0].frameBoundary(); g_uiDepth[1].frameBoundary();
        frame = (frame | 15u) + 1u; g_frame = frame;   // a sampled frame: the census copies every 16th
        ctx->ClearDepthStencilView(wideSceneDsv.Get(), D3D11_CLEAR_DEPTH, 0.0f, 0);
        const float light[4] = {0.6f, 0.6f, 0.6f, 1.0f}, black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        // kToyPsHlsl's own split is pos.x<4 regardless of viewport width,
        // so the light stays exactly 4 px wide at the left edge here.
        const float data[12] = {light[0], light[1], light[2], light[3], black[0], black[1], black[2], black[3], kNear5m, 0, 0, 0};
        ctx->UpdateSubresource(cbuf.Get(), 0, nullptr, data, 0, 0);
        const D3D11_VIEWPORT wideVp{0, 0, static_cast<float>(kWideW), static_cast<float>(kWideH), 0, 1};
        ctx->RSSetViewports(1, &wideVp);
        ctx->OMSetRenderTargets(1, wideToyRtv.GetAddressOf(), nullptr);
        ctx->OMSetBlendState(blendSrcAlphaOne.Get(), nullptr, 0xFFFFFFFFu);
        ctx->VSSetShader(toyVs.Get(), nullptr, 0);
        ctx->PSSetShader(toyPs.Get(), nullptr, 0);
        ctx->ClearRenderTargetView(wideToyRtv.Get(), kBlack);
        ctx->Draw(3, 0);
        // wideVp stays bound through the reissue: listedReissue()'s own
        // draws take whatever viewport is currently set (it sets none of
        // its own), so resetting to vp8 here would clip the capture to
        // its first 8 columns, leaving every pixel past x=8 with no
        // element depth at all.
        g_holoEye = 0; g_holoW = kWideW; g_holoH = kWideH;
        listedReissue();
        check(uiDepthHologramResolve(ctx.Get(), 0, wideScene.Get(), kWideW, kWideH, nullptr),
              "resolve runs (near-light radius)");
        ID3D11ShaderResourceView* wideSrv = nullptr;
        check(uiDepthTemporalDepth(kWideW, kWideH, 0, wideScene.Get(), &wideSrv), "near-light radius: private depth published");
        ComPtr<ID3D11Resource> wideRes; wideSrv->GetResource(&wideRes);
        auto wideValues = readDepth(dev.Get(), ctx.Get(), wideRes.Get());
        check(std::fabs(wideValues[15] - kFillerDepth) < 1e-5f, "near-light radius: 12 px from light, in its block's neighbour, takes the filler depth");
        check(wideValues[33] == 0.0f, "near-light radius: 30 px from light, two blocks further, is not covered");
        check(wideValues[43] == 0.0f, "near-light radius: 40 px from light is not covered -- the round-6 regression case");
        // The near-light census, live, right after the dispatch above:
        // holoPollNearLightCounts (DONOTWAIT) should find this one sample
        // once its copy has landed. The map itself only ever marks a block
        // whose OWN pixels qualify as light -- block 1's covered status
        // (the resolve's 3x3 lookaround) never writes back into the map --
        // so only block 0 (the light's own block) is set, out of six.
        g_holoNearLightSampleCount = 0;
        const auto nearLightDeadline = GetTickCount64() + 2000;
        while (g_holoNearLightSampleCount == 0 && GetTickCount64() < nearLightDeadline) holoPollNearLightCounts(ctx.Get());
        check(g_holoNearLightSampleCount > 0, "near-light census: the wide panel's map copy resolved");
        if (g_holoNearLightSampleCount) check(g_holoNearLightSamples[0] == 1, "near-light census: exactly 1 of 6 blocks was set");
        ctx->RSSetViewports(1, &vp8);   // restore for every test after this one
        g_holoEye = 0; g_holoW = 8; g_holoH = 8;
    }

    // The filler depth for a covered dark pixel. GREATER
    // (holoElementDepthState) means it only lands where the private copy
    // already holds something farther than the filler (~9.9 m here) --
    // sky, or a real surface beyond it -- never overwriting a nearer real
    // scene surface, exactly the pass-off behaviour over a hangar console
    // 3.9 m behind a HUD panel's gaps (flight dumps 115012/115037).
    {
        // (a) A real scene surface behind the element but nearer than the
        // filler (8 m behind a 5 m element, as the hangar's console at
        // 3.9 m sat behind its 1.6 m text) keeps its own depth in the
        // gaps, while the glyph half takes the element's own 5 m. The OLD
        // rule (the gap writes the element's own depth) FAILS here: 5 m
        // is nearer than 8 m, so GREATER would have let it through.
        g_uiDepth[0].frameBoundary(); g_uiDepth[1].frameBoundary();
        const float scene8m = temporalPassDepthAt(8.0f);
        ctx->ClearDepthStencilView(sceneDsv.Get(), D3D11_CLEAR_DEPTH, scene8m, 0);
        const float glyph[4] = {0.6f, 0.6f, 0.6f, 1.0f}, gap[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        originalDraw(kNear5m, glyph, gap, blendSrcAlphaOne.Get(), false, kBlack, true);
        listedReissue();
        check(uiDepthHologramResolve(ctx.Get(), 0, sceneTex.Get(), 8, 8, nullptr),
              "resolve runs (filler, nearer scene)");
        {
            auto values = privateDepth();
            for (unsigned y = 0; y < 8; ++y) for (unsigned x = 0; x < 8; ++x) {
                const bool glyphHalf = x < 4;
                check(std::fabs(values[y * 8 + x] - (glyphHalf ? kNear5m : scene8m)) < 1e-5f,
                      glyphHalf ? "(a) the glyph in front of an 8 m scene surface takes the element's own depth"
                                : "(a) a dark gap over a nearer 8 m scene surface keeps the scene's depth");
            }
        }

        // (b) and (c): the same element over sky (cleared 0, farther than
        // both the filler and the element's own 5 m -- 30 m would do
        // equally). Under the OLD rule (dark pixels write d, the element's
        // own depth) this gap would read kNear5m (0.005), not the filler
        // (~0.0025) -- (b) also FAILS if the PS still writes d for a
        // covered dark pixel.
        g_uiDepth[0].frameBoundary(); g_uiDepth[1].frameBoundary();
        ctx->ClearDepthStencilView(sceneDsv.Get(), D3D11_CLEAR_DEPTH, 0.0f, 0);
        originalDraw(kNear5m, glyph, gap, blendSrcAlphaOne.Get(), false, kBlack, true);
        listedReissue();
        check(uiDepthHologramResolve(ctx.Get(), 0, sceneTex.Get(), 8, 8, nullptr),
              "resolve runs (filler, over sky)");
        {
            auto values = privateDepth();
            for (unsigned y = 0; y < 8; ++y) for (unsigned x = 0; x < 8; ++x) {
                const bool glyphHalf = x < 4;
                check(std::fabs(values[y * 8 + x] - (glyphHalf ? kNear5m : kFillerDepth)) < 1e-5f,
                      glyphHalf ? "(c) a bright pixel in the same setup still takes the element's own depth"
                                : "(b) a dark gap over sky, farther than the filler, takes the filler depth");
            }
        }
    }

    // Round 10: a UI-covered pixel discards before any other test,
    // keeping whatever the private copy already held (its own exact
    // depth from the UI depth pass) instead of this pass's own
    // footprint -- the nearest listed draw, however transparent, which
    // can supply none of the pixel's actual light. (a) A bright,
    // UI-covered element at a nearer 0.6 m over a scene pre-seeded to a
    // farther-looking 1.5 m stamp: GREATER alone would accept the
    // element's write (0.6 m genuinely is nearer than 1.5 m), so only
    // the mask discard keeps the stamp's own depth -- the FAILING case
    // if the skip is removed. (b) The same draw with the mask bit
    // clear takes the element's own depth, as before this round. (c) A
    // dark gap next to UI-covered light still takes the filler: the
    // near-light CS does not test UI coverage, so the glyph's own
    // light still marks its block regardless of the glyph's own
    // discard.
    {
        // A mask for these tests, built directly into g_mask rather
        // than through maskFor (tied to the reissue system's own
        // g_rebindW/H): this rig only needs g_mask[0] populated with a
        // known pattern at the standard 8x8 size. Content is rewritten
        // per sub-test below.
        D3D11_TEXTURE2D_DESC md{};
        md.Width = 8; md.Height = 8; md.MipLevels = md.ArraySize = 1;
        md.Format = DXGI_FORMAT_R8_UNORM; md.SampleDesc.Count = 1;
        md.Usage = D3D11_USAGE_DEFAULT; md.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        std::vector<unsigned char> maskBits(8 * 8, 1);
        const D3D11_SUBRESOURCE_DATA msub{maskBits.data(), 8, 0};
        ComPtr<ID3D11Texture2D> maskTex; hr(dev->CreateTexture2D(&md, &msub, &maskTex));
        ComPtr<ID3D11ShaderResourceView> maskSrv; hr(dev->CreateShaderResourceView(maskTex.Get(), nullptr, &maskSrv));
        g_mask[0].tex = maskTex.Get();
        g_mask[0].srv = maskSrv.Get();
        g_mask[0].w = 8; g_mask[0].h = 8;
        g_mask[0].marked = true;

        const float uiStamp = temporalPassDepthAt(1.5f);
        const float elementDepth = temporalPassDepthAt(0.6f);
        const float bright[4] = {0.6f, 0.6f, 0.6f, 1.0f};

        // (a) Mask set everywhere: the nearer element never overwrites
        // the farther-looking stamp already in the private copy.
        g_uiDepth[0].frameBoundary(); g_uiDepth[1].frameBoundary();
        ctx->ClearDepthStencilView(sceneDsv.Get(), D3D11_CLEAR_DEPTH, uiStamp, 0);
        originalDraw(elementDepth, bright, bright, blendSrcAlphaOne.Get(), false, kBlack, true);
        listedReissue();
        check(uiDepthHologramResolve(ctx.Get(), 0, sceneTex.Get(), 8, 8, nullptr),
              "resolve runs (UI-covered mask)");
        for (float v : privateDepth())
            check(std::fabs(v - uiStamp) < 1e-5f,
                  "(a) UI-covered: keeps the UI stamp's own depth, not the nearer element's");

        // (b) The identical draw, mask bit cleared everywhere: back to
        // today's behaviour, the element's own (nearer) depth wins.
        for (auto& b : maskBits) b = 0;
        ctx->UpdateSubresource(maskTex.Get(), 0, nullptr, maskBits.data(), 8, 0);
        g_uiDepth[0].frameBoundary(); g_uiDepth[1].frameBoundary();
        ctx->ClearDepthStencilView(sceneDsv.Get(), D3D11_CLEAR_DEPTH, uiStamp, 0);
        originalDraw(elementDepth, bright, bright, blendSrcAlphaOne.Get(), false, kBlack, true);
        listedReissue();
        check(uiDepthHologramResolve(ctx.Get(), 0, sceneTex.Get(), 8, 8, nullptr),
              "resolve runs (mask bit clear)");
        for (float v : privateDepth())
            check(std::fabs(v - elementDepth) < 1e-5f,
                  "(b) no mask bit: the element's own depth wins, as before this round");

        // (c) glyph (x<4, bright) is UI-covered; gap (x>=4, dark) is
        // not. The scene is sky (farther than everything here), so the
        // filler write can land. The glyph's own discard must not stop
        // its light from covering the gap in the near-light map.
        for (unsigned y = 0; y < 8; ++y) for (unsigned x = 0; x < 4; ++x) maskBits[y * 8 + x] = 1;
        ctx->UpdateSubresource(maskTex.Get(), 0, nullptr, maskBits.data(), 8, 0);
        g_uiDepth[0].frameBoundary(); g_uiDepth[1].frameBoundary();
        ctx->ClearDepthStencilView(sceneDsv.Get(), D3D11_CLEAR_DEPTH, 0.0f, 0);
        const float dark[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        originalDraw(kNear5m, bright, dark, blendSrcAlphaOne.Get(), false, kBlack, true);
        listedReissue();
        check(uiDepthHologramResolve(ctx.Get(), 0, sceneTex.Get(), 8, 8, nullptr),
              "resolve runs (UI-covered glyph, gap not covered)");
        {
            auto values = privateDepth();
            for (unsigned y = 0; y < 8; ++y) for (unsigned x = 0; x < 8; ++x) {
                const float expected = x < 4 ? 0.0f : kFillerDepth;
                check(std::fabs(values[y * 8 + x] - expected) < 1e-5f,
                      x < 4 ? "(c) the UI-covered glyph itself keeps the sky it was pre-seeded with"
                            : "(c) the gap next to UI-covered light still takes the filler");
            }
        }

        g_mask[0] = Mask{};   // these raw pointers must not outlive the ComPtrs above
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

    // World markers, unlisted: kHoloUnlistedMarker never appears in
    // g_holoMarkerDepthShaders (round 8), so both cases below still take
    // the null PS, exactly as every world marker did before that table
    // existed. A broken game viewport (MinDepth == MaxDepth == 0,
    // mechanism i, flight 20260924_175113's own reading on the target
    // reticle) is overridden for the element-depth pass alone, and the
    // marker's real depth recovers from its own raster Z; a VS that
    // writes z=0 itself (mechanism ii) is not fixed by that override --
    // not covered, and its occlusion query reads zero samples, the
    // signature the census reports as "element-depth samples p50 0".
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
        g_holoDrawVs = kHoloUnlistedMarker;
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
        // viewport in place throughout. The override cannot fix this, and
        // this hash has no matched PS either (the next test below covers
        // the one hash that now does).
        g_holoMarkerSampleCount = 0;
        g_uiDepth[0].frameBoundary(); g_uiDepth[1].frameBoundary();
        ctx->ClearDepthStencilView(sceneDsv.Get(), D3D11_CLEAR_DEPTH, 0.0f, 0);
        g_holoIsWorldMarker = true;
        g_holoDrawVs = kHoloUnlistedMarker;
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

    // World marker, true depth (round 8): the reticle's own VS
    // (vs_71DD8B8B09060A81) forces z=0 but carries the real view
    // distance in clip W. A toy VS with that exact output signature
    // (TEXCOORD6 float4, TEXCOORD7 float3, SV_Position), z=0, clip W=50,
    // exercises the matched PS end to end: it links against that
    // signature, reads D3D11's own SV_Position.w in the pixel shader
    // (proving it is the raw clip W, not 1/W -- the formula below only
    // holds one of those two ways), and must stamp temporalPassDepthAt(50).
    {
        // x,y are pre-multiplied by clipW so the rasterizer's perspective
        // divide (by w = clipW, not the usual 1) cancels back out to a
        // full-screen triangle instead of shrinking toward the centre --
        // unlike kToyVsHlsl above, this VS deliberately varies w.
        auto markerVsCode = compile(
            "cbuffer C : register(b0) { float clipW; float3 pad; };\n"
            "void main(uint id : SV_VertexID, out float4 tc6 : TEXCOORD6,\n"
            "          out float3 tc7 : TEXCOORD7, out float4 pos : SV_Position) {\n"
            "    float2 uv = float2((id << 1) & 2, id & 2);\n"
            "    tc6 = float4(0, 0, 0, 0); tc7 = float3(0, 0, 0);\n"
            "    pos = float4((uv * 2.0 - 1.0) * clipW, 0.0, clipW);\n"
            "}\n", "vs_5_0");
        auto markerPsCode = compile(
            "struct In { float4 tc6 : TEXCOORD6; float3 tc7 : TEXCOORD7; float4 pos : SV_Position; };\n"
            "float4 main(In i) : SV_Target { return float4(0.5, 0.5, 0.5, 1.0); }\n", "ps_5_0");
        ComPtr<ID3D11VertexShader> markerVs; ComPtr<ID3D11PixelShader> markerPs;
        hr(dev->CreateVertexShader(markerVsCode->GetBufferPointer(), markerVsCode->GetBufferSize(), nullptr, &markerVs));
        hr(dev->CreatePixelShader(markerPsCode->GetBufferPointer(), markerPsCode->GetBufferSize(), nullptr, &markerPs));
        D3D11_BUFFER_DESC mbd{}; mbd.ByteWidth = 16; mbd.Usage = D3D11_USAGE_DEFAULT; mbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        ComPtr<ID3D11Buffer> markerCbuf; hr(dev->CreateBuffer(&mbd, nullptr, &markerCbuf));

        g_holoMarkerSampleCount = 0;
        g_uiDepth[0].frameBoundary(); g_uiDepth[1].frameBoundary();
        ctx->ClearDepthStencilView(sceneDsv.Get(), D3D11_CLEAR_DEPTH, 0.0f, 0);
        const float clipW[4] = {50.0f, 0, 0, 0};
        ctx->UpdateSubresource(markerCbuf.Get(), 0, nullptr, clipW, 0, 0);
        ctx->VSSetShader(markerVs.Get(), nullptr, 0);
        ctx->PSSetShader(markerPs.Get(), nullptr, 0);
        ctx->VSSetConstantBuffers(0, 1, markerCbuf.GetAddressOf());
        ctx->OMSetRenderTargets(1, toyRtvUnorm.GetAddressOf(), nullptr);
        ctx->OMSetBlendState(blendSrcAlphaOne.Get(), nullptr, 0xFFFFFFFFu);
        ctx->ClearRenderTargetView(toyRtvUnorm.Get(), kBlack);
        ctx->Draw(3, 0);
        g_holoEye = 0; g_holoW = 8; g_holoH = 8;
        g_holoIsWorldMarker = true;
        g_holoDrawVs = kHoloWorldMarkerReticle;
        listedReissue();
        ctx->VSSetShader(toyVs.Get(), nullptr, 0);   // restore for every test after this one
        ctx->PSSetShader(toyPs.Get(), nullptr, 0);
        ctx->VSSetConstantBuffers(0, 1, cbuf.GetAddressOf());
        ctx->PSSetConstantBuffers(0, 1, cbuf.GetAddressOf());

        check(uiDepthHologramResolve(ctx.Get(), 0, sceneTex.Get(), 8, 8, nullptr),
              "resolve runs (world marker, true depth)");
        for (float v : privateDepth())
            check(std::fabs(v - kNear50m) < 1e-5f,
                  "world marker: the listed reticle's own PS recovers its real depth from clip W");
        for (int tries = 0; tries < 50 && g_holoMarkerSampleCount == 0; ++tries) {
            holoPollQueries(ctx.Get());
            if (!g_holoMarkerSampleCount) Sleep(1);
        }
        check(g_holoMarkerSampleCount > 0 && g_holoMarkerSamples[g_holoMarkerSampleCount - 1] > 0,
              "world marker: the matched PS's occlusion query sees the recovered geometry");
        g_holoIsWorldMarker = false;
    }

    // The near-light pass against the game's output merger. At Submit the
    // game's last eye pass has left its views bound, and D3D11 answers a
    // shader view over a resource that is still bound as an output by
    // silently setting that view to NULL: t2 (the game's RT0, the share
    // test) would read 0, so the share test always passed, and t3 (the
    // submitted image, the floor test) would read 0, so no pixel cleared the
    // floor and the map came out empty. The resolve's own pixel-shader draw
    // clears the stage first and never saw it, which is why no scenario above
    // can tell a blind dispatch from a sighted one. These can. Every case is
    // one 8x8 block (any lit pixel lights the whole image's block); the
    // game's draw adds 0.1 on the left half and nothing on the right; the
    // submitted image is lit (0.5) on the left and dark (0) on the right.
    //   Share cases (RT0 held): the target was pre-filled to 0.8, so the 0.1
    //   is under half of the finished 0.9. A sighted dispatch finds no
    //   light: the block stays dark and the dark right half stays uncovered
    //   (all 0). A blind t2 reads the target as 0, the share test passes, the
    //   block lights, and the right half takes the filler depth.
    //   Floor cases (submitted image held): the target is black, so the 0.1
    //   is the whole finished pixel. A sighted dispatch lights the block: the
    //   left half keeps the element's depth and the right takes the filler.
    //   A blind t3 reads the image as 0, nothing clears the floor, the map is
    //   empty, and the right half stays 0.
    // Each case also checks what the pass counted (g_holoNearLightInputs, the
    // census line's own figures), that the stage was cleared exactly when an
    // input was held, that the output merger came back as the game left it
    // (same views, same slots, same depth view), and what the pass said: one
    // note per eye the first time each class of output merger is seen (none
    // bound, bound but neither input held, RT0 held, the image held, both) and
    // never again for that class, and only a held input earns the sentence
    // about reading black. A prelude flips eye 0 between the two classes with
    // nothing held first, to show that noise cannot use the notes up before a
    // class with a hold shows. The eye-1 cases run the same two holds through
    // the other eye: its counters move and eye 0's do not, and each eye keeps
    // its own seen set.
    {
        struct SplitDisplay {
            ComPtr<ID3D11Texture2D> tex;
            ComPtr<ID3D11ShaderResourceView> srv;
            ComPtr<ID3D11RenderTargetView> rtv;
        };
        // The submitted image's stand-in: R8G8B8A8_UNORM, leftV across x < 4
        // and rightV across x >= 4, and bindable as a render target so a case
        // can leave it bound as an output (makeDisplay above is flat, SRV only).
        auto makeSplitDisplay = [&](float leftV, float rightV) {
            D3D11_TEXTURE2D_DESC dd{};
            dd.Width = dd.Height = 8; dd.MipLevels = dd.ArraySize = 1;
            dd.Format = DXGI_FORMAT_R8G8B8A8_UNORM; dd.SampleDesc.Count = 1;
            dd.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET; dd.Usage = D3D11_USAGE_DEFAULT;
            const BYTE byteLeft = static_cast<BYTE>(leftV * 255.0f + 0.5f);
            const BYTE byteRight = static_cast<BYTE>(rightV * 255.0f + 0.5f);
            BYTE pixels[8 * 8 * 4];
            for (int i = 0; i < 64; ++i) {
                const BYTE b = (i % 8) < 4 ? byteLeft : byteRight;
                pixels[i*4] = b; pixels[i*4+1] = b; pixels[i*4+2] = b; pixels[i*4+3] = 255;
            }
            const D3D11_SUBRESOURCE_DATA init{pixels, 8 * 4, 0};
            SplitDisplay made;
            hr(dev->CreateTexture2D(&dd, &init, &made.tex));
            hr(dev->CreateShaderResourceView(made.tex.Get(), nullptr, &made.srv));
            hr(dev->CreateRenderTargetView(made.tex.Get(), nullptr, &made.rtv));
            return made;
        };
        // A depth target of the game's own, never referenced by production,
        // so the restore check can see the depth view come back too.
        D3D11_TEXTURE2D_DESC gameDd{};
        gameDd.Width = gameDd.Height = 8; gameDd.MipLevels = gameDd.ArraySize = 1;
        gameDd.Format = DXGI_FORMAT_D32_FLOAT; gameDd.SampleDesc.Count = 1; gameDd.BindFlags = D3D11_BIND_DEPTH_STENCIL;
        ComPtr<ID3D11Texture2D> gameDepthTex; hr(dev->CreateTexture2D(&gameDd, nullptr, &gameDepthTex));
        ComPtr<ID3D11DepthStencilView> gameDsv; hr(dev->CreateDepthStencilView(gameDepthTex.Get(), nullptr, &gameDsv));

        // Whether the output merger holds exactly r0 on slot 0, r1 on slot 1,
        // nothing on the other slots, and dsv.
        auto omHolds = [&](ID3D11RenderTargetView* r0, ID3D11RenderTargetView* r1, ID3D11DepthStencilView* dsv) {
            ID3D11RenderTargetView* got[8] = {}; ID3D11DepthStencilView* gotDsv = nullptr;
            ctx->OMGetRenderTargets(8, got, &gotDsv);
            bool same = got[0] == r0 && got[1] == r1 && gotDsv == dsv;
            for (int i = 2; i < 8; ++i) same = same && !got[i];
            for (auto* v : got) if (v) v->Release();
            if (gotDsv) gotDsv->Release();
            return same;
        };
        auto sameInputs = [](const HoloNearLightInputs& a, const HoloNearLightInputs& b) {
            return a.ran == b.ran && a.heldTarget == b.heldTarget && a.heldDisplay == b.heldDisplay &&
                   a.nullT2 == b.nullT2 && a.nullT3 == b.nullT3;
        };
        struct Run {
            std::vector<float> depth;    // the resolved eye's private depth copy after the resolve
            HoloNearLightInputs seen;    // what that eye's g_holoNearLightInputs gained over this resolve
            bool otherEyeQuiet = false;  // the other eye's counters did not move at all
            unsigned clears = 0;         // the full stage clears this resolve issued
            bool omBack = false;         // the output merger held exactly what was bound before it
            std::string note;            // the last log line the resolve wrote, "" when it wrote none
        };
        // One eye-frame: the game's element draw into hdrTex (cleared to
        // targetClear first), classified for `eye`, both listed reissues, then
        // the game's views bound as given, r0 on slot 0 and r1 on slot 1 when
        // they are not null (no render target at all when r0 is null), with a
        // depth view -- which is where Submit finds them -- and the resolve for
        // that eye.
        auto nearLightRun = [&](int eye, const char* what, float targetClear, const SplitDisplay& display,
                                ID3D11RenderTargetView* r0, ID3D11RenderTargetView* r1) {
            char label[256];
            g_uiDepth[0].frameBoundary(); g_uiDepth[1].frameBoundary();
            ctx->ClearDepthStencilView(sceneDsv.Get(), D3D11_CLEAR_DEPTH, 0.0f, 0);
            const float base[4] = {targetClear, targetClear, targetClear, 1.0f};
            const float add[4] = {0.1f, 0.1f, 0.1f, 1.0f}, none[4] = {0.0f, 0.0f, 0.0f, 1.0f};
            drawIntoRtv(hdrRtv.Get(), kNear5m, add, none, blendSrcAlphaOne.Get(), base, true);
            g_holoEye = eye;   // drawIntoRtv classifies every draw for eye 0
            listedReissue();
            ID3D11RenderTargetView* views[2] = {r0, r1};
            ctx->OMSetRenderTargets(r1 ? 2u : (r0 ? 1u : 0u), views, gameDsv.Get());
            std::snprintf(label, sizeof(label), "near-light inputs, %s: setup: the output merger holds the views the case bound", what);
            check(omHolds(r0, r1, gameDsv.Get()), label);
            const HoloNearLightInputs was[2] = {g_holoNearLightInputs[0], g_holoNearLightInputs[1]};
            const unsigned clearsWas = g_rawOmFullClears;
            std::snprintf(label, sizeof(label), "resolve runs (near-light inputs, %s)", what);
            g_lastLog.clear();
            check(uiDepthHologramResolve(ctx.Get(), eye, sceneTex.Get(), 8, 8, display.srv.Get()), label);
            Run run;
            run.note = g_lastLog;
            const HoloNearLightInputs& now = g_holoNearLightInputs[eye];
            run.seen = HoloNearLightInputs{now.ran - was[eye].ran, now.heldTarget - was[eye].heldTarget,
                                           now.heldDisplay - was[eye].heldDisplay, now.nullT2 - was[eye].nullT2,
                                           now.nullT3 - was[eye].nullT3};
            run.otherEyeQuiet = sameInputs(g_holoNearLightInputs[1 - eye], was[1 - eye]);
            run.clears = g_rawOmFullClears - clearsWas;
            run.omBack = omHolds(r0, r1, gameDsv.Get());
            ID3D11ShaderResourceView* depthSrv = nullptr;
            std::snprintf(label, sizeof(label), "private depth published (near-light inputs, %s)", what);
            check(uiDepthTemporalDepth(8, 8, eye, sceneTex.Get(), &depthSrv), label);
            ComPtr<ID3D11Resource> depthRes; depthSrv->GetResource(&depthRes);
            run.depth = readDepth(dev.Get(), ctx.Get(), depthRes.Get());
            return run;
        };
        // The counters, the stage clear and the output merger: the same
        // questions for every case, the expected holds being the case's own.
        auto expectInputs = [&](const Run& run, const char* what, uint32_t heldTarget, uint32_t heldDisplay) {
            char label[256];
            auto say = [&](const char* text) {
                std::snprintf(label, sizeof(label), "near-light inputs, %s: %s", what, text);
                return label;
            };
            check(run.seen.ran == 1, say("the dispatch is counted exactly once"));
            check(run.seen.heldTarget == heldTarget,
                  say("the census counts the game's RT0 as held exactly when it was still bound"));
            check(run.seen.heldDisplay == heldDisplay,
                  say("the census counts the submitted image as held exactly when it was still bound"));
            check(run.seen.nullT2 == 0 && run.seen.nullT3 == 0,
                  say("no input reads back NULL from the context: the stage clear let the views stick"));
            check(run.clears == ((heldTarget || heldDisplay) ? 1u : 0u),
                  say("the stage is cleared once when an input is held and never otherwise"));
            check(run.otherEyeQuiet, say("the other eye's counters did not move at all"));
            check(run.omBack, say("the output merger is put back exactly as the game left it"));
        };
        // What the run's resolve wrote: the note carries a piece of text, or
        // does not. Said with the case's own name so a failure names its case.
        auto noteHas = [&](const Run& run, const char* what, const char* piece) {
            char label[384];
            std::snprintf(label, sizeof(label), "near-light note, %s: the log line carries \"%s\"", what, piece);
            check(run.note.find(piece) != std::string::npos, label);
        };
        auto noteLacks = [&](const Run& run, const char* what, const char* piece) {
            char label[384];
            std::snprintf(label, sizeof(label), "near-light note, %s: the log line does not carry \"%s\"", what, piece);
            check(run.note.find(piece) == std::string::npos, label);
        };

        g_holoNearLightInputs[0] = g_holoNearLightInputs[1] = HoloNearLightInputs{};
        g_holoNearLightSeen[0] = g_holoNearLightSeen[1] = 0;
        g_holoIsWorldMarker = false; g_holoDrawVs = kHoloIconCore;
        const SplitDisplay display = makeSplitDisplay(0.5f, 0.0f);
        // The share cases' depth: nothing is covered. The floor cases' depth:
        // the lit left half keeps the element's own, the dark right half takes
        // the filler.
        auto expectUncovered = [&](const Run& run, const char* label) {
            for (float v : run.depth) check(v == 0.0f, label);
        };
        auto expectLitAndFiller = [&](const Run& run, const char* litLabel, const char* darkLabel) {
            for (unsigned y = 0; y < 8; ++y) for (unsigned x = 0; x < 8; ++x) {
                const float expected = x < 4 ? kNear5m : kFillerDepth;
                check(std::fabs(run.depth[y * 8 + x] - expected) < 1e-5f, x < 4 ? litLabel : darkLabel);
            }
        };

        // Flip-noise prelude. A game that flips between classes with nothing
        // held (no render target bound, an unrelated one bound) while it loads
        // must not use the notes up before a class with a hold shows. Eye 0,
        // eight visits alternating the two: one note per class, on its first
        // visit, and nothing on any later one; nothing is held, cleared or
        // counted as a hold, the share case's content covers nothing, and the
        // output merger is put back every time. Every visit is also the
        // no-hold control for the share case's all-uncovered result.
        for (int visit = 0; visit < 8; ++visit) {
            const bool noOutput = (visit % 2) == 0;   // class 0 on even visits, class 4 on odd ones
            char what[64];
            std::snprintf(what, sizeof(what), "flip noise %d, %s", visit,
                          noOutput ? "no output bound" : "unrelated output bound");
            const Run run = nearLightRun(0, what, 0.8f, display, noOutput ? nullptr : toyRtvUnorm.Get(), nullptr);
            expectUncovered(run, noOutput ? "near-light control (no output bound): the share case's content must give the same "
                                            "all-uncovered result with nothing held"
                                          : "near-light control (unrelated output bound): the share case's content must give the "
                                            "same all-uncovered result with nothing held");
            expectInputs(run, what, 0, 0);
            if (visit < 2) {
                noteHas(run, what, noOutput ? "eye 0, near-light pass: the output merger holds depth 8x8 fmt 40."
                                            : "eye 0, near-light pass: the output merger holds slot 0 8x8 fmt 27, depth 8x8 fmt 40.");
                noteLacks(run, what, "would have read black");
            } else {
                noteLacks(run, what, "near-light pass: the output merger holds");
            }
        }
        check(g_holoNearLightSeen[0] == 0x11u && g_holoNearLightSeen[1] == 0,
              "near-light notes: the prelude marked exactly classes 0 and 4 as seen on eye 0 and nothing on eye 1");

        // Share case, RT0 held (t2): the game's target stays bound on slot 0,
        // its depth view with it. The first RT0-held look at eye 0 is noted
        // although the prelude's noise came first: what the output merger
        // holds, the game's RT0 named, and what that would cost.
        {
            const char* what = "RT0 held";
            const Run run = nearLightRun(0, what, 0.8f, display, hdrRtv.Get(), nullptr);
            expectUncovered(run, "near-light t2 (RT0 still bound): a blind share test lit the block and the dark half took "
                                 "the filler depth; with the target seen, nothing is light and nothing is covered");
            expectInputs(run, what, 1, 0);
            noteHas(run, what, "eye 0, near-light pass: the output merger holds slot 0 8x8 fmt 10 (the game's RT0)");
            noteHas(run, what, "depth 8x8");
            noteHas(run, what, "the game's RT0 view (the share test)");
            noteLacks(run, what, "(the submitted image)");
        }

        // Floor case, the submitted image held (t3): it is the output on slot
        // 0. A class eye 0 has not shown yet (the image, not RT0), so the pass
        // says so: a note, naming the image this time.
        {
            const char* what = "image held";
            const Run run = nearLightRun(0, what, 0.0f, display, display.rtv.Get(), nullptr);
            expectLitAndFiller(run, "near-light t3 (image still bound): the lit half keeps the element's own depth",
                               "near-light t3 (image still bound): a blind floor test left the map empty and the dark "
                               "half uncovered; with the image seen it takes the filler depth");
            expectInputs(run, what, 0, 1);
            noteHas(run, what, "eye 0, near-light pass: the output merger holds slot 0 8x8 fmt 28 (the submitted image)");
            noteHas(run, what, "the submitted-image view (the floor)");
            noteLacks(run, what, "(the game's RT0)");
        }

        // Both held, the image on slot 1: the game's target on slot 0 and the
        // image behind it, so the hold check has to walk every slot. Another
        // new class, another note.
        {
            const char* what = "both held, image on slot 1";
            const Run run = nearLightRun(0, what, 0.0f, display, hdrRtv.Get(), display.rtv.Get());
            expectLitAndFiller(run, "near-light, both held (image on slot 1): the lit half keeps the element's own depth",
                               "near-light, both held (image on slot 1): the hold check missed the image on slot 1, "
                               "the map came out empty and the dark half stayed uncovered");
            expectInputs(run, what, 1, 1);
            noteHas(run, what, "slot 0 8x8 fmt 10 (the game's RT0), slot 1 8x8 fmt 28 (the submitted image)");
            noteHas(run, what, "both views (the share test and the floor)");
        }

        // The share case again with the game's target on slot 1 and an
        // unrelated output on slot 0 (the image unbound, so a blind t3 cannot
        // mask a blind t2). The RT0-held class was seen above, so the pass says
        // nothing this time; the slot-1 hold check is what this case is for.
        {
            const char* what = "RT0 held on slot 1";
            const Run run = nearLightRun(0, what, 0.8f, display, toyRtvUnorm.Get(), hdrRtv.Get());
            expectUncovered(run, "near-light t2 (RT0 on slot 1): the hold check missed the target on slot 1, a blind share "
                                 "test lit the block and the dark half took the filler depth");
            expectInputs(run, what, 1, 0);
            noteLacks(run, what, "near-light pass: the output merger holds");
        }

        // Aliasing: the game's target and the submitted image are ONE resource
        // (the display view is over hdrTex itself, as the toy-target share
        // cases above do), so the one bound slot is both inputs: both holds are
        // counted and the slot is named as both. Class 7 was seen above, so its
        // seen bit is cleared to let this first aliased look note again.
        {
            SplitDisplay aliased;
            aliased.tex = hdrTex;
            hr(dev->CreateShaderResourceView(hdrTex.Get(), nullptr, &aliased.srv));
            g_holoNearLightSeen[0] &= ~(1u << 7);
            const char* what = "target and image are one resource";
            const Run run = nearLightRun(0, what, 0.8f, aliased, hdrRtv.Get(), nullptr);
            expectUncovered(run, "near-light aliased (target and image are one resource): the share case's content must give "
                                 "the same all-uncovered result");
            expectInputs(run, what, 1, 1);
            noteHas(run, what, "eye 0, near-light pass: the output merger holds slot 0 8x8 fmt 10 "
                               "(the game's RT0 and the submitted image)");
            noteHas(run, what, "both views (the share test and the floor)");
        }

        // The same two holds through eye 1. Everything the pass keeps is per
        // eye: eye 1's counters move and eye 0's do not, and eye 1's first look
        // at each class notes although eye 0 has shown both classes already.
        check((g_holoNearLightSeen[0] & 0x60u) == 0x60u && g_holoNearLightSeen[1] == 0,
              "near-light notes, setup: eye 0 has seen the RT0-held and image-held classes, and eye 1 has seen nothing");
        {
            const char* what = "RT0 held, eye 1";
            const Run run = nearLightRun(1, what, 0.8f, display, hdrRtv.Get(), nullptr);
            expectUncovered(run, "near-light t2, eye 1 (RT0 still bound): a blind share test lit the block and the dark half "
                                 "took the filler depth; with the target seen, nothing is light and nothing is covered");
            expectInputs(run, what, 1, 0);
            noteHas(run, what, "eye 1, near-light pass: the output merger holds slot 0 8x8 fmt 10 (the game's RT0)");
            noteHas(run, what, "the game's RT0 view (the share test)");
            noteLacks(run, what, "eye 0, near-light pass");
        }
        {
            const char* what = "image held, eye 1";
            const Run run = nearLightRun(1, what, 0.0f, display, display.rtv.Get(), nullptr);
            expectLitAndFiller(run, "near-light t3, eye 1 (image still bound): the lit half keeps the element's own depth",
                               "near-light t3, eye 1 (image still bound): a blind floor test left the map empty and the "
                               "dark half uncovered; with the image seen it takes the filler depth");
            expectInputs(run, what, 0, 1);
            noteHas(run, what, "eye 1, near-light pass: the output merger holds slot 0 8x8 fmt 28 (the submitted image)");
            noteHas(run, what, "the submitted-image view (the floor)");
            noteLacks(run, what, "eye 0, near-light pass");
        }
        // Each eye's seen set is its own: eye 0 showed classes {0, 4, 5, 6, 7}
        // (none bound, unrelated output, RT0 held, image held, both), eye 1
        // only {5, 6}, and eye 1's two looks left eye 0's alone. Bit c is
        // class c.
        check(g_holoNearLightSeen[0] == 0xF1u && g_holoNearLightSeen[1] == 0x60u,
              "near-light notes: each eye keeps its own seen set, eye 0 {0,4,5,6,7} and eye 1 {5,6}");

        // The census line carries the window's figures, eye 0 then eye 1 in
        // every pair, and then starts over. Eye 0 dispatched thirteen times (the
        // prelude's eight, then RT0 held, the image, both, RT0 on slot 1 and the
        // aliased case; RT0 held in four of them, the image in three), eye 1
        // twice (RT0 in one, the image in one), and nothing ever read back NULL.
        g_holoWindowStartMs = GetTickCount64() - 30001;
        g_lastLog.clear();
        holoDepthWindowTick(ctx.Get());
        check(g_lastLog.find("near-light inputs (eye 0 / eye 1): dispatched 13 / 2, output merger held the game's RT0 "
                             "4 / 1 and the submitted image 3 / 1, views read back null t2 0 / 0 t3 0 / 0") != std::string::npos,
              "census: the near-light inputs sentence counts this block's dispatches, holds and null readbacks, eye 0 then eye 1");
        check(sameInputs(g_holoNearLightInputs[0], HoloNearLightInputs{}) &&
              sameInputs(g_holoNearLightInputs[1], HoloNearLightInputs{}),
              "census: the window reset clears the near-light input counters of both eyes");
        // The per-eye seen sets are left as the cases left them: only shutdown
        // re-arms them, checked at the end of the run.
    }

    // The periodic census prints while the key is on, once the window's
    // 30 s (wall clock) elapses -- forced by backdating the window's
    // start rather than waiting -- and prints nothing while it is off
    // (covered above, before any scratch existed).
    {
        // Every resolve above queued a near-light staging copy (not just
        // the world-marker draws, unlike the occlusion-query ring), and
        // the ring only holds 3 slots per eye, so several are still
        // pending from the world-marker tests just above. Drain them
        // first -- holoDepthWindowTick polls the same ring itself, and an
        // unpolled straggler landing during that call would make the
        // "zero near-light blocks" line below false.
        {
            const auto deadline = GetTickCount64() + 2000;
            uint32_t stableRounds = 0;
            while (stableRounds < 3 && GetTickCount64() < deadline) {
                const uint32_t before = g_holoNearLightSampleCount;
                holoPollNearLightCounts(ctx.Get());
                stableRounds = (g_holoNearLightSampleCount == before) ? stableRounds + 1 : 0;
            }
        }
        g_holoWindowStartMs = GetTickCount64() - 30001;
        g_holoWindowListed = g_holoWindowResolved = 0;
        g_holoWindowNoTarget = g_holoWindowFloorFallback = 0;
        g_holoWindowDeclinedNotCleared = g_holoWindowDeclinedNoPrivate = 0;
        g_holoWindowDeclinedNoProjection = g_holoWindowDeclinedFault = 0;
        g_holoPixelSampleCount = 0;
        g_holoWindowMarkerDraws = 0;
        g_holoMarkerSampleCount = 0;
        g_holoNearLightSampleCount = 0;
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
        check(g_lastLog.find("near-light blocks/eye-frame mean 0.0 (0 sampled)") != std::string::npos,
              "census: zero near-light blocks printed");
        check(g_holoWindowStartMs != 0, "census: the window reset after printing");
    }

    if (info) for (UINT64 i = 0; i < info->GetNumStoredMessages(); ++i) {
        SIZE_T size = 0; info->GetMessage(i, nullptr, &size); std::vector<unsigned char> storage(size);
        auto* m = reinterpret_cast<D3D11_MESSAGE*>(storage.data()); hr(info->GetMessage(i, m, &size));
        if (m->Severity <= D3D11_MESSAGE_SEVERITY_WARNING) {
            std::puts(m->pDescription); check(false, "D3D debug-layer warning/error");
        }
    }
    // Shutdown re-arms what the near-light pass remembers across frames: each
    // eye's seen set (still set by the cases above) and the census counters
    // (given leftovers here, the window tick having emptied them).
    check(g_holoNearLightSeen[0] != 0 && g_holoNearLightSeen[1] != 0,
          "shutdown, setup: both eyes still hold a seen set from the near-light cases");
    g_holoNearLightInputs[0].ran = 3; g_holoNearLightInputs[1].nullT3 = 2;
    ctx->ClearState(); uiDepthShutdown();
    check(g_holoNearLightSeen[0] == 0 && g_holoNearLightSeen[1] == 0,
          "shutdown: the near-light seen sets of both eyes are re-armed");
    check(g_holoNearLightInputs[0].ran == 0 && g_holoNearLightInputs[1].nullT3 == 0,
          "shutdown: the near-light census counters of both eyes are emptied");
    check(gpuTimingShutdown(ctx.Get()), "explicit shared timer shutdown before WARP release");
    std::printf("PASS: %d checks; the generic hologram/icon depth pass mirrors the game's own blend "
                "(including alpha), gates cockpit families by the cockpit radius before accumulation "
                "while a world marker contributes at any range, reads its floor off the displayed "
                "image and its share off the game's own HDR target (never each other), restores every "
                "piece of state it touches, and the family builder and periodic census behave.\n", checks);
    return 0;
}
