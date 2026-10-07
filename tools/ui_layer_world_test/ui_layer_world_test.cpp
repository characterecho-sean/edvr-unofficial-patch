// The VR on-foot world route's layer half, THE REAL CODE on WARP (docs/design-flat-temporal-aa-2026-09-23.md, section 82).
//
// src/d3d11/ui_layer.cpp is linked WHOLE -- the gatherer (uiLayerDecide), the re-issue (uiLayerWorldReissueBegin /
// End), the layer's own machinery they reuse (beginInner, restore, the composite), the door's preflight
// (uiLayerWorldDoorGap) -- and its neighbours are stubbed: the binding shadow is a few slots this rig writes, the
// route (vr_world_route.h) and the mips module (vr_world_mips.h) answer what the case asks, the raw OM/RS entry points
// go straight to the context. vscreen.cpp's own helper (worldScreenReissue) is four lines and is reproduced at the
// call site below; tools/ui_quality_test scans the real one.
//
// What this proves, each against the production functions and real D3D11 (WARP):
//   1. KEY OFF. With the route not owning the frame the screen composite is left in the picture (kWorldScreen),
//      nothing is pending, the mips module and the route are never asked.
//   2. THE RE-ISSUE. On an owned frame the decision is never a take (false), the game's own draw lands in its eye
//      image unchanged, and the re-issue draws the same quad into the eye's LAYER through the map (eye pixels x the
//      layer's scale), opaque, from the route's mipped screen through the route's trilinear sampler -- proven with a
//      mip chain of one flat colour a level, where the layer shows the level the map's minification selects (mip 1) and
//      the game's eye shows mip 0 -- and the composite of that layer over a frame IS the screen where the quad is and
//      the frame where it is not. The route is told which eye was taken, once, for that sequence.
//   3. EVERY CHANGED STATE COMES BACK: targets, depth target, viewport, scissor, blend (object, factor, mask), depth
//      state, PS slots 0 and 1 (textures and samplers), shaders -- exactly the game's objects, after a landed
//      re-issue and after every refusal.
//   4. EVERY REFUSAL: a named counter, the game's state untouched, nothing taken, the route never told (no mips, no
//      sampler, no sampler bound, no source texture, a blending draw (a substituted one too), depth state, not armed,
//      the draw's bindings changed before the re-issue, abandoned).
//   4b. A CURVED SCREEN is no refusal: the game's draw is the curve substitution's strip (uiLayerDecide's `substituted`), the plan
//      accepts it like a flat one, the re-issue lands in the layer exactly as a flat draw's does and takes the eye; and a strip
//      draw that did not happen (End told landed = false) closes the bracket without taking the eye, counted as a fault.
//   5. THE HOOKS STEP ASIDE: every raw entry the re-issue calls runs with VrWorldInternalScope up, without an outer one.
//   6. THE DOOR'S PREFLIGHT and the accessors, the lost-draw counter, the stats.
//   7. THE ON-FOOT MAPS GATE (experimental.on_foot_maps_sharp, docs/design-world-camera-motion-2026-09-30.md, Phase 1), the layer's
//      half: the key off is today's gate frame by frame and logs nothing; the key on follows the naming (hold 2, release 3), a
//      taken 2D screen marks the eye and the door's predicate answers for it while nothing else went into an eye-sized target, and
//      every line is made in its order (testMapsGate below).
// Exit codes: 0 pass, 1 a check failed, 2 usage. --dry-run touches nothing; --hardware runs the same checks on the default adapter.
#include <windows.h>

#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi.h>
#include <wrl/client.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "../../src/common/config.h"
#include "../../src/common/guard.h"
#include "../../src/common/log.h"
#include "../../src/common/system_d3d11.h"
#include "../../src/d3d11/binding_shadow.h"
#include "../../src/d3d11/depth_probe.h"
#include "../../src/d3d11/device_hook.h"
#include "../../src/d3d11/gpu_census.h"
#include "../../src/d3d11/gpu_timing.h"
#include "../../src/d3d11/journal_watch.h"
#include "../../src/d3d11/shader_swap.h"
#include "../../src/d3d11/ui_depth.h"
#include "../../src/d3d11/ui_layer.h"
#include "../../src/d3d11/ui_layer_math.h"
#include "../../src/d3d11/ui_maps_math.h"
#include "../../src/d3d11/ui_panel_scale.h"
#include "../../src/d3d11/ui_surfaces.h"
#include "../../src/d3d11/vr_world_mips.h"
#include "../../src/d3d11/vr_world_route.h"
#include "../../src/d3d11/vscreen.h"

using Microsoft::WRL::ComPtr;

namespace {
unsigned g_checks = 0, g_fails = 0;
void check(bool ok, const char* what) {
    ++g_checks;
    if (!ok) {
        ++g_fails;
        std::printf("FAIL: %s\n", what);
    }
}

constexpr uint32_t kEyeW = 64, kEyeH = 48;   // the eye images the game draws the screen into
constexpr uint32_t kDoorW = 96, kDoorH = 72;  // the frame the door hands on: the layer's size at 1.0
constexpr uint32_t kScreenW = 144, kScreenH = 96;

// ------------------------------------------------------------------------------------ what the stubs answer
struct Stubs {
    // the route (vr_world_route.h)
    bool mayTake = false, enabled = false;
    unsigned tookCalls = 0;
    uint32_t tookEye = 9;
    uint64_t tookSeq = 0;
    // the mips module (vr_world_mips.h)
    ID3D11ShaderResourceView* mipsAnswer = nullptr;
    ID3D11SamplerState* samplerAnswer = nullptr;
    unsigned mipsCalls = 0, samplerCalls = 0, mipsInternal = 0, samplerInternal = 0;   // ...and how many ran with the route's internal scope up
    uint64_t lastMipsFrame = 0;
    const void* lastMipsScreen = nullptr;
    // the draw-time jitter the native temporal channel reports
    uint64_t seq = 0;
    bool jitterOk = true;
    float jx = 0.0f, jy = 0.0f;
    // the journal, the eye table
    bool journalKnown = true, journalOnFoot = true;
    const void* eyeRes[2] = {nullptr, nullptr};
    // the on-foot maps gate's neighbours: the route's own answer to the door's question, the draws into eye-sized targets the hooks
    // counted this frame (vscreen.cpp), the depth probe's count of the 2D screen's depth
    bool routeDoor = false;
    uint32_t eyeDraws = 0;
    bool depthKnown = false;
    uint32_t depthDraws = 0;
    // raw entries: did any run with the route's internal scope up / down
    unsigned rawCalls = 0, rawInternal = 0;
};
Stubs g_stubs;
}  // namespace

// ============================================================================== the neighbours, stubbed
namespace edvr {
thread_local bool g_flatComputeInternal = false;
thread_local bool g_vrWorldInternal = false;
namespace detail {
BindingSlot g_bindingSlots[static_cast<size_t>(BindSlot::Count)];
}  // namespace detail
void breadcrumb(const char*) {}  // production guard.cpp's crash-channel dependency

// screen_motion.h's two flags (screenMotionLive is inline over them): the rig sets them the way fix.temporal_aa would.
namespace detail {
bool g_screenMotionEnabled = true;
bool g_screenMotionFailed = false;
}  // namespace detail

// ui_depth.h's two flags (uiDepthWantsDraws is inline over them): the layer's 30 s composite line says NOT COUNTED when the interface depth
// pass is off or stood down (ui_scene_composites.h). The pass is on here, as fix.temporal_aa = dlss would have it.
namespace detail {
bool g_uiDepthOn = true;
bool g_uiDepthStoodDown = false;
}  // namespace detail
bool vrWorldRouteEnabled() { return g_stubs.enabled; }
bool vrWorldRouteLayerMayTake() { return g_stubs.mayTake; }
bool vrWorldRouteDoorLayerOnly(uint32_t, uint64_t) { return g_stubs.routeDoor; }
uint32_t vScreenEyeDrawsThisFrame() { return g_stubs.eyeDraws; }
void vrWorldRouteNoteEyeTaken(uint32_t eye, uint64_t sequence) {
    ++g_stubs.tookCalls;
    g_stubs.tookEye = eye;
    g_stubs.tookSeq = sequence;
}
ID3D11ShaderResourceView* vrWorldMipsScreen(ID3D11DeviceContext*, ID3D11Texture2D* screen, uint64_t frame) {
    ++g_stubs.mipsCalls;
    if (g_vrWorldInternal && g_flatComputeInternal) ++g_stubs.mipsInternal;
    g_stubs.lastMipsFrame = frame;
    g_stubs.lastMipsScreen = screen;
    return g_stubs.mipsAnswer;
}
ID3D11SamplerState* vrWorldMipsSampler(ID3D11Device*, const D3D11_SAMPLER_DESC&) {
    ++g_stubs.samplerCalls;
    if (g_vrWorldInternal && g_flatComputeInternal) ++g_stubs.samplerInternal;
    return g_stubs.samplerAnswer;
}
bool nativeTemporalDrawJitter(uint32_t eye, uint64_t* sequence, float* jx, float* jy, uint32_t* w, uint32_t* h) {
    if (!g_stubs.jitterOk || eye > 1) return false;
    if (sequence) *sequence = g_stubs.seq;
    if (jx) *jx = g_stubs.jx;
    if (jy) *jy = g_stubs.jy;
    if (w) *w = kEyeW;
    if (h) *h = kEyeH;
    return true;
}
// The binding shadow's resolver, from the views themselves.
bool bindingResolve(void* view, ResourceInfo* out) {
    if (!view || !out) return false;
    ComPtr<ID3D11Resource> res;
    static_cast<ID3D11View*>(view)->GetResource(&res);
    ComPtr<ID3D11Texture2D> tex;
    if (!res || FAILED(res.As(&tex))) return false;
    D3D11_TEXTURE2D_DESC d{};
    tex->GetDesc(&d);
    *out = ResourceInfo{};
    out->isTexture2D = true;
    out->a = d.Width;
    out->b = d.Height;
    out->fmt = d.Format;
    out->resource = res.Get();
    return true;
}
bool bindingResolveResource(void*, ResourceInfo*) { return false; }
bool depthProbeDrawsAtSize(uint32_t, uint32_t, uint32_t* draws) {
    if (draws) *draws = g_stubs.depthDraws;
    return g_stubs.depthKnown;
}
bool deviceHookHmdQuality(float*) { return false; }
bool gpuCensusBegin(ID3D11DeviceContext*, GpuCensusSection) noexcept { return false; }
void gpuCensusEnd(ID3D11DeviceContext*, GpuCensusSection) noexcept {}
void gpuCensusNoteSeedTarget(const GpuCensusSeedTarget&) noexcept {}
bool gpuTimingBind(ID3D11Device*, ID3D11DeviceContext*, const GpuSpanD3D11Ops&) noexcept { return false; }
bool gpuTimingAccepts(ID3D11DeviceContext*) noexcept { return false; }
bool gpuTimingOwns(ID3D11DeviceContext*) noexcept { return false; }
bool GpuTimer::begin(ID3D11Device*, ID3D11DeviceContext*) noexcept { return false; }
bool GpuTimer::beginBorrowedFrame(ID3D11Device*, ID3D11DeviceContext*) noexcept { return false; }
bool GpuTimer::end(ID3D11DeviceContext*) noexcept { return false; }
GpuTimerPoll GpuTimer::poll(ID3D11DeviceContext*, double&) noexcept { return GpuTimerPoll::Pending; }
void GpuTimer::reset(ID3D11DeviceContext*) noexcept {}
bool journalWatchActive() { return true; }
bool journalGuiFocus(uint32_t*) { return false; }
bool journalOnFootKnown() { return g_stubs.journalKnown; }
bool journalOnFoot() { return g_stubs.journalOnFoot; }
ID3D11VertexShader* shaderSwapCreateVs(ID3D11DeviceContext* ctx, const void* bytes, size_t n, const char*, const char*) {
    ComPtr<ID3D11Device> dev;
    ctx->GetDevice(&dev);
    ID3D11VertexShader* s = nullptr;
    return dev && SUCCEEDED(dev->CreateVertexShader(bytes, n, nullptr, &s)) ? s : nullptr;
}
ID3D11ComputeShader* shaderSwapCreateCs(ID3D11DeviceContext* ctx, const void* bytes, size_t n, const char*, const char*) {
    ComPtr<ID3D11Device> dev;
    ctx->GetDevice(&dev);
    ID3D11ComputeShader* s = nullptr;
    return dev && SUCCEEDED(dev->CreateComputeShader(bytes, n, nullptr, &s)) ? s : nullptr;
}
ID3D11PixelShader* shaderSwapCreatePs(ID3D11DeviceContext* ctx, const void* bytes, size_t n, const char*, const char*) {
    ComPtr<ID3D11Device> dev;
    ctx->GetDevice(&dev);
    ID3D11PixelShader* s = nullptr;
    return dev && SUCCEEDED(dev->CreatePixelShader(bytes, n, nullptr, &s)) ? s : nullptr;
}
int uiDepthEyeOfTarget(const void* res, uint32_t, uint32_t, uint32_t) {
    return res && res == g_stubs.eyeRes[0] ? 0 : res && res == g_stubs.eyeRes[1] ? 1 : -1;
}
int uiDepthEyeOfTargetReadOnly(const void* res) { return uiDepthEyeOfTarget(res, 0, 0, 0); }
void uiPanelScaleSetTarget(float) {}
void uiPanelScaleFrameBoundary() {}
void uiPanelScaleLog() {}
void orbitalWidthLog(const char*) {}
// The supercruise bars' private pass (supercruise_bars.h): this rig never draws one, so the layer is never asked to take the family
// (ui_layer.cpp asks the module only for it) and the 30 s line is not read.
bool supercruiseBarsReady(ID3D11DeviceContext*, const char** why) {
    if (why) *why = "";
    return false;
}
void supercruiseBarsLog(const char*) {}
void uiSurfacesSetTarget(float) {}
void uiSurfacesFrameBoundary() {}
void uiSurfacesLogAtlas() {}
bool vScreenIsEyeSized(uint32_t w, uint32_t h) { return w == kEyeW && h == kEyeH; }
bool vScreenPanelSize(uint32_t* w, uint32_t* h) {
    if (w) *w = kScreenW;
    if (h) *h = kScreenH;
    return true;
}
// The raw entries go straight to the context; each notes whether the route's internal scope was up.
static void rawNote() {
    ++g_stubs.rawCalls;
    if (g_vrWorldInternal && g_flatComputeInternal) ++g_stubs.rawInternal;
}
void vScreenSetRenderTargetsRaw(ID3D11DeviceContext* c, uint32_t n, ID3D11RenderTargetView* const* r, ID3D11DepthStencilView* d) {
    rawNote();
    c->OMSetRenderTargets(n, r, d);
}
void vScreenRSSetViewportsRaw(ID3D11DeviceContext* c, uint32_t n, const D3D11_VIEWPORT* v) {
    rawNote();
    c->RSSetViewports(n, v);
}
void vScreenClearRenderTargetViewRaw(ID3D11DeviceContext* c, ID3D11RenderTargetView* r, const float col[4]) {
    rawNote();
    c->ClearRenderTargetView(r, col);
}
void vScreenPSSetShaderResourcesRaw(ID3D11DeviceContext* c, uint32_t start, uint32_t n, ID3D11ShaderResourceView* const* s) {
    rawNote();
    c->PSSetShaderResources(start, n, s);
}
void vScreenOMSetBlendStateRaw(ID3D11DeviceContext* c, ID3D11BlendState* s, const float f[4], uint32_t m) {
    rawNote();
    c->OMSetBlendState(s, f, m);
}
void vScreenPSSetShaderRaw(ID3D11DeviceContext* c, ID3D11PixelShader* s, ID3D11ClassInstance* const* ci, uint32_t n) {
    rawNote();
    c->PSSetShader(s, ci, n);
}
void vScreenCopyResourceRaw(ID3D11DeviceContext* c, ID3D11Resource* d, ID3D11Resource* s) {
    rawNote();
    c->CopyResource(d, s);
}
void vScreenExecuteCommandListRaw(ID3D11DeviceContext* c, ID3D11CommandList* l, int restore) {
    rawNote();
    c->ExecuteCommandList(l, restore ? TRUE : FALSE);
}
}  // namespace edvr

using namespace edvr;

namespace {
// ============================================================================================ the harness
struct Tex {
    ComPtr<ID3D11Texture2D> tex;
    ComPtr<ID3D11RenderTargetView> rtv;
    ComPtr<ID3D11ShaderResourceView> srv;
    uint32_t w = 0, h = 0;
};

constexpr uint32_t kRed = 0xFF0000FFu, kGreen = 0xFF00FF00u, kBlue = 0xFFFF0000u, kWhite = 0xFFFFFFFFu;
constexpr uint32_t kBlack = 0xFF000000u, kGrey = 0xFF808080u, kVoid = 0xFF282828u;

struct Rig {
    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11DeviceContext> ctx;
    Tex eye[2], screen, frame[2], tiny;
    ComPtr<ID3D11Texture2D> mipsTex;
    ComPtr<ID3D11ShaderResourceView> mipsSrv, otherSrv;
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps;
    ComPtr<ID3D11Buffer> rectCb;
    ComPtr<ID3D11SamplerState> gameSampler, otherSampler, mipSampler;
    ComPtr<ID3D11RasterizerState> raster;
    ComPtr<ID3D11BlendState> blend, premul, maxBlend;
    ComPtr<ID3D11DepthStencilState> dsOff, dsOn;
    ComPtr<ID3D11Texture2D> depth;
    ComPtr<ID3D11DepthStencilView> dsv;
    uint64_t seq = 1;
};

Tex makeTex(Rig& r, uint32_t w, uint32_t h, DXGI_FORMAT fmt, UINT bind, DXGI_FORMAT viewFmt, uint32_t color) {
    Tex t;
    t.w = w;
    t.h = h;
    D3D11_TEXTURE2D_DESC d{};
    d.Width = w;
    d.Height = h;
    d.MipLevels = d.ArraySize = 1;
    d.Format = fmt;
    d.SampleDesc.Count = 1;
    d.BindFlags = bind;
    std::vector<uint32_t> px(static_cast<size_t>(w) * h, color);
    D3D11_SUBRESOURCE_DATA sd{px.data(), w * 4, 0};
    if (FAILED(r.dev->CreateTexture2D(&d, &sd, &t.tex))) return t;
    if (bind & D3D11_BIND_RENDER_TARGET) {
        D3D11_RENDER_TARGET_VIEW_DESC rd{};
        rd.Format = viewFmt;
        rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        r.dev->CreateRenderTargetView(t.tex.Get(), &rd, &t.rtv);
    }
    if (bind & D3D11_BIND_SHADER_RESOURCE) {
        D3D11_SHADER_RESOURCE_VIEW_DESC vd{};
        vd.Format = viewFmt;
        vd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        vd.Texture2D.MipLevels = 1;
        r.dev->CreateShaderResourceView(t.tex.Get(), &vd, &t.srv);
    }
    return t;
}

std::vector<uint32_t> readPixels(Rig& r, ID3D11Texture2D* tex) {
    D3D11_TEXTURE2D_DESC d{};
    tex->GetDesc(&d);
    d.BindFlags = 0;
    d.Usage = D3D11_USAGE_STAGING;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    d.MiscFlags = 0;
    ComPtr<ID3D11Texture2D> st;
    std::vector<uint32_t> out(static_cast<size_t>(d.Width) * d.Height, 0);
    if (FAILED(r.dev->CreateTexture2D(&d, nullptr, &st))) return out;
    r.ctx->CopyResource(st.Get(), tex);
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(r.ctx->Map(st.Get(), 0, D3D11_MAP_READ, 0, &m))) return out;
    for (uint32_t y = 0; y < d.Height; ++y)
        std::memcpy(&out[static_cast<size_t>(y) * d.Width], static_cast<const uint8_t*>(m.pData) + y * m.RowPitch, d.Width * 4);
    r.ctx->Unmap(st.Get(), 0);
    return out;
}
uint32_t at(const std::vector<uint32_t>& p, uint32_t w, uint32_t x, uint32_t y) { return p[static_cast<size_t>(y) * w + x]; }

// Every pixel of [x0,x1) x [y0,y1) is `want`.
bool region(const std::vector<uint32_t>& p, uint32_t w, uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1, uint32_t want) {
    for (uint32_t y = y0; y < y1; ++y)
        for (uint32_t x = x0; x < x1; ++x)
            if (at(p, w, x, y) != want) return false;
    return true;
}

// Every pixel of the rectangle is within `tol` of `want` on each channel (alpha exact).
bool regionNear(const std::vector<uint32_t>& p, uint32_t w, uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1, uint32_t want, int tol) {
    for (uint32_t y = y0; y < y1; ++y)
        for (uint32_t x = x0; x < x1; ++x) {
            const uint32_t got = at(p, w, x, y);
            for (int shift = 0; shift < 24; shift += 8) {
                const int d = static_cast<int>((got >> shift) & 0xFF) - static_cast<int>((want >> shift) & 0xFF);
                if (d > tol || d < -tol) return false;
            }
            if ((got >> 24) != (want >> 24)) return false;
        }
    return true;
}

const char kShaders[] = R"HLSL(
cbuffer Q : register(b0) { float4 rect; };           // x0 y0 x1 y1, NDC
struct V { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
V vsMain(uint id : SV_VertexID) {
    V v;
    float2 c = float2((id & 1) ? 1.0 : 0.0, (id & 2) ? 1.0 : 0.0);   // strip: (0,0) (1,0) (0,1) (1,1)
    v.pos = float4(lerp(rect.x, rect.z, c.x), lerp(rect.w, rect.y, c.y), 0, 1);
    v.uv = c;
    return v;
}
Texture2D t0 : register(t0);
SamplerState s0 : register(s0);
float4 psMain(V v) : SV_Target { return t0.Sample(s0, v.uv); }
)HLSL";

bool compile(const char* entry, const char* profile, ComPtr<ID3DBlob>* out) {
    ComPtr<ID3DBlob> err;
    const HRESULT hr = D3DCompile(kShaders, sizeof(kShaders) - 1, "ui_layer_world_test", nullptr, nullptr, entry, profile,
                                  D3DCOMPILE_ENABLE_STRICTNESS, 0, out->GetAddressOf(), &err);
    if (FAILED(hr) && err) std::printf("%s\n", static_cast<const char*>(err->GetBufferPointer()));
    return SUCCEEDED(hr);
}

// Four mips of flat colour: 144x96 red, 72x48 green, 36x24 blue, 18x12 white.
bool makeMips(Rig& r) {
    D3D11_TEXTURE2D_DESC d{};
    d.Width = kScreenW;
    d.Height = kScreenH;
    d.MipLevels = 4;
    d.ArraySize = 1;
    d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    const uint32_t colours[4] = {kRed, kGreen, kBlue, kWhite};
    std::vector<uint32_t> level[4];
    D3D11_SUBRESOURCE_DATA sd[4];
    for (uint32_t m = 0; m < 4; ++m) {
        const uint32_t w = kScreenW >> m, h = kScreenH >> m;
        level[m].assign(static_cast<size_t>(w) * h, colours[m]);
        sd[m] = {level[m].data(), w * 4, 0};
    }
    if (FAILED(r.dev->CreateTexture2D(&d, sd, &r.mipsTex))) return false;
    D3D11_SHADER_RESOURCE_VIEW_DESC vd{};
    vd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    vd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    vd.Texture2D.MipLevels = 4;
    return SUCCEEDED(r.dev->CreateShaderResourceView(r.mipsTex.Get(), &vd, &r.mipsSrv));
}

bool setup(Rig& r, bool hardware) {
    D3D_FEATURE_LEVEL fl{};
    const auto create = edvr::systemD3D11CreateDevice();
    if (!create || FAILED(create(nullptr, hardware ? D3D_DRIVER_TYPE_HARDWARE : D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &r.dev, &fl, &r.ctx))) return false;
    check(edvr::reportSystemD3D11Only("ui_layer_world_test"), "the rig runs on System32's d3d11.dll and on no other d3d11.dll");
    for (int e = 0; e < 2; ++e) {
        r.eye[e] = makeTex(r, kEyeW, kEyeH, DXGI_FORMAT_R8G8B8A8_TYPELESS, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE,
                           DXGI_FORMAT_R8G8B8A8_UNORM, kVoid);
        r.frame[e] = makeTex(r, kDoorW, kDoorH, DXGI_FORMAT_R8G8B8A8_TYPELESS, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE,
                             DXGI_FORMAT_R8G8B8A8_UNORM, kBlack);
        g_stubs.eyeRes[e] = r.eye[e].tex.Get();
    }
    r.screen = makeTex(r, kScreenW, kScreenH, DXGI_FORMAT_R8G8B8A8_TYPELESS, D3D11_BIND_SHADER_RESOURCE, DXGI_FORMAT_R8G8B8A8_UNORM, kRed);
    r.tiny = makeTex(r, 16, 16, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_SHADER_RESOURCE, DXGI_FORMAT_R8G8B8A8_UNORM, kBlue);
    r.otherSrv = r.tiny.srv;
    if (!r.eye[0].rtv || !r.eye[1].rtv || !r.screen.srv || !r.frame[0].srv || !makeMips(r)) return false;
    ComPtr<ID3DBlob> v, p;
    if (!compile("vsMain", "vs_5_0", &v) || !compile("psMain", "ps_5_0", &p)) return false;
    if (FAILED(r.dev->CreateVertexShader(v->GetBufferPointer(), v->GetBufferSize(), nullptr, &r.vs)) ||
        FAILED(r.dev->CreatePixelShader(p->GetBufferPointer(), p->GetBufferSize(), nullptr, &r.ps)))
        return false;
    // The quad: eye pixels [8,56) x [8,40) in NDC.
    const float rect[4] = {-1.0f + 2.0f * 8 / kEyeW, 1.0f - 2.0f * 8 / kEyeH, -1.0f + 2.0f * 56 / kEyeW, 1.0f - 2.0f * 40 / kEyeH};
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = 16;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    D3D11_SUBRESOURCE_DATA sd{rect, 0, 0};
    if (FAILED(r.dev->CreateBuffer(&bd, &sd, &r.rectCb))) return false;
    D3D11_SAMPLER_DESC smp{};
    smp.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    smp.AddressU = smp.AddressV = smp.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    smp.MaxLOD = 0.0f;   // the game's own: no mips
    smp.ComparisonFunc = D3D11_COMPARISON_NEVER;
    if (FAILED(r.dev->CreateSamplerState(&smp, &r.gameSampler))) return false;
    smp.AddressU = D3D11_TEXTURE_ADDRESS_WRAP;
    if (FAILED(r.dev->CreateSamplerState(&smp, &r.otherSampler))) return false;
    smp.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    smp.AddressU = smp.AddressV = smp.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    smp.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(r.dev->CreateSamplerState(&smp, &r.mipSampler))) return false;
    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    rd.ScissorEnable = TRUE;
    if (FAILED(r.dev->CreateRasterizerState(&rd, &r.raster))) return false;
    D3D11_BLEND_DESC bl{};
    bl.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (FAILED(r.dev->CreateBlendState(&bl, &r.blend))) return false;
    bl.RenderTarget[0].BlendEnable = TRUE;   // premultiplied over: a draw that blends
    bl.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
    bl.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    bl.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bl.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    bl.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    bl.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    if (FAILED(r.dev->CreateBlendState(&bl, &r.premul))) return false;
    bl.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;   // a MAX blend: no form the layer can convert
    bl.RenderTarget[0].DestBlend = D3D11_BLEND_ONE;
    bl.RenderTarget[0].BlendOp = D3D11_BLEND_OP_MAX;
    bl.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    bl.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
    bl.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_MAX;
    if (FAILED(r.dev->CreateBlendState(&bl, &r.maxBlend))) return false;
    D3D11_DEPTH_STENCIL_DESC dso{};
    dso.DepthEnable = FALSE;
    dso.DepthFunc = D3D11_COMPARISON_ALWAYS;
    if (FAILED(r.dev->CreateDepthStencilState(&dso, &r.dsOff))) return false;
    dso.DepthEnable = TRUE;
    dso.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    dso.DepthFunc = D3D11_COMPARISON_LESS;
    if (FAILED(r.dev->CreateDepthStencilState(&dso, &r.dsOn))) return false;
    D3D11_TEXTURE2D_DESC dd{};
    dd.Width = kEyeW;
    dd.Height = kEyeH;
    dd.MipLevels = dd.ArraySize = 1;
    dd.Format = DXGI_FORMAT_D32_FLOAT;
    dd.SampleDesc.Count = 1;
    dd.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    if (FAILED(r.dev->CreateTexture2D(&dd, nullptr, &r.depth)) || FAILED(r.dev->CreateDepthStencilView(r.depth.Get(), nullptr, &r.dsv))) return false;
    g_stubs.mipsAnswer = r.mipsSrv.Get();
    g_stubs.samplerAnswer = r.mipSampler.Get();
    return true;
}

// The game's state for the screen composite draw into `eye`: every slot the re-issue could touch set to a sentinel.
void bindGame(Rig& r, int eye, ID3D11BlendState* blend = nullptr, ID3D11DepthStencilState* ds = nullptr) {
    ID3D11RenderTargetView* rtv = r.eye[eye].rtv.Get();
    const float voidColour[4] = {40.0f / 255.0f, 40.0f / 255.0f, 40.0f / 255.0f, 1.0f};
    r.ctx->ClearRenderTargetView(rtv, voidColour);   // the game clears its eye image each frame
    r.ctx->OMSetRenderTargets(1, &rtv, r.dsv.Get());
    const D3D11_VIEWPORT vp{0, 0, static_cast<float>(kEyeW), static_cast<float>(kEyeH), 0, 1};
    r.ctx->RSSetViewports(1, &vp);
    r.ctx->RSSetState(r.raster.Get());
    const D3D11_RECT sc{2, 2, 60, 44};
    r.ctx->RSSetScissorRects(1, &sc);
    const float bf[4] = {0.1f, 0.2f, 0.3f, 0.4f};
    r.ctx->OMSetBlendState(blend ? blend : r.blend.Get(), bf, 0xFFFFFFFDu);
    r.ctx->OMSetDepthStencilState(ds ? ds : r.dsOff.Get(), 7);
    ID3D11ShaderResourceView* srvs[2] = {r.screen.srv.Get(), r.otherSrv.Get()};
    r.ctx->PSSetShaderResources(0, 2, srvs);
    ID3D11SamplerState* smp[2] = {r.gameSampler.Get(), r.otherSampler.Get()};
    r.ctx->PSSetSamplers(0, 2, smp);
    r.ctx->VSSetShader(r.vs.Get(), nullptr, 0);
    r.ctx->PSSetShader(r.ps.Get(), nullptr, 0);
    ID3D11Buffer* cb = r.rectCb.Get();
    r.ctx->VSSetConstantBuffers(0, 1, &cb);
    r.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    auto& slot = detail::g_bindingSlots[static_cast<size_t>(BindSlot::Rtv0)];
    slot.ptr = rtv;
    ++slot.gen;
}

// Everything the re-issue could have changed, as raw pointers and values: equal before and after means restored.
struct Snap {
    ID3D11RenderTargetView* rtv[8] = {};
    ID3D11DepthStencilView* dsv = nullptr;
    UINT nvp = 16;
    D3D11_VIEWPORT vp[16] = {};
    UINT nsc = 16;
    D3D11_RECT sc[16] = {};
    ID3D11RasterizerState* rs = nullptr;
    ID3D11BlendState* bs = nullptr;
    float bf[4] = {};
    UINT mask = 0;
    ID3D11DepthStencilState* dss = nullptr;
    UINT ref = 0;
    ID3D11ShaderResourceView* srv[4] = {};
    ID3D11SamplerState* smp[4] = {};
    ID3D11PixelShader* ps = nullptr;
    ID3D11VertexShader* vs = nullptr;
};
Snap take(ID3D11DeviceContext* c) {
    Snap s;
    ID3D11RenderTargetView* rtv[8] = {};
    c->OMGetRenderTargets(8, rtv, &s.dsv);
    for (int i = 0; i < 8; ++i) {
        s.rtv[i] = rtv[i];
        if (rtv[i]) rtv[i]->Release();
    }
    if (s.dsv) s.dsv->Release();
    c->RSGetViewports(&s.nvp, s.vp);
    c->RSGetScissorRects(&s.nsc, s.sc);
    c->RSGetState(&s.rs);
    if (s.rs) s.rs->Release();
    c->OMGetBlendState(&s.bs, s.bf, &s.mask);
    if (s.bs) s.bs->Release();
    c->OMGetDepthStencilState(&s.dss, &s.ref);
    if (s.dss) s.dss->Release();
    ID3D11ShaderResourceView* srv[4] = {};
    c->PSGetShaderResources(0, 4, srv);
    ID3D11SamplerState* smp[4] = {};
    c->PSGetSamplers(0, 4, smp);
    for (int i = 0; i < 4; ++i) {
        s.srv[i] = srv[i];
        s.smp[i] = smp[i];
        if (srv[i]) srv[i]->Release();
        if (smp[i]) smp[i]->Release();
    }
    c->PSGetShader(&s.ps, nullptr, nullptr);
    if (s.ps) s.ps->Release();
    c->VSGetShader(&s.vs, nullptr, nullptr);
    if (s.vs) s.vs->Release();
    return s;
}
bool same(const Snap& a, const Snap& b) {
    return std::memcmp(a.rtv, b.rtv, sizeof(a.rtv)) == 0 && a.dsv == b.dsv && a.nvp == b.nvp &&
           std::memcmp(a.vp, b.vp, sizeof(D3D11_VIEWPORT) * a.nvp) == 0 && a.nsc == b.nsc &&
           std::memcmp(a.sc, b.sc, sizeof(D3D11_RECT) * a.nsc) == 0 && a.rs == b.rs && a.bs == b.bs &&
           std::memcmp(a.bf, b.bf, sizeof(a.bf)) == 0 && a.mask == b.mask && a.dss == b.dss && a.ref == b.ref &&
           std::memcmp(a.srv, b.srv, sizeof(a.srv)) == 0 && std::memcmp(a.smp, b.smp, sizeof(a.smp)) == 0 && a.ps == b.ps && a.vs == b.vs;
}

// The door's side of a frame, as native_temporal / native_sharpen call it: the eye's submitted texture, the pass's
// output (the black frame), and the door that saw it -- which is what arms the layer for the NEXT frame.
void doorFrame(Rig& r, uint64_t seq) {
    for (uint32_t e = 0; e < 2; ++e) {
        uiLayerNoteSubmitted(seq, e, r.eye[e].tex.Get());
        uiLayerNoteTemporal(seq, e, r.frame[e].tex.Get());
        uiLayerDoorSeen(seq, e, r.frame[e].tex.Get());
    }
}

// vscreen.cpp's worldScreenReissue, line for line (the draw is the game's own: the quad once more).
bool reissue(Rig& r) {
    VrWorldInternalScope internal;
    if (!uiLayerWorldReissueBegin(r.ctx.Get())) return false;
    r.ctx->Draw(4, 0);
    uiLayerWorldReissueEnd(r.ctx.Get());
    return true;
}

// One screen-composite draw as forwardWithVerdict handles it: the decision, the game's own draw, the re-issue.
struct Drawn {
    bool taken = false, pendingAfterDecide = false, reissued = false;
};
Drawn drawScreen(Rig& r, bool substituted = false) {
    Drawn d;
    d.taken = uiLayerDecide(r.ctx.Get(), static_cast<int>(UiLayerFamily::kScreen), true, substituted);
    d.pendingAfterDecide = uiLayerWorldReissuePending();
    r.ctx->Draw(4, 0);  // the game's own issue, always
    if (d.pendingAfterDecide) d.reissued = reissue(r);
    else uiLayerWorldReissueAbandon();
    return d;
}

// A fresh sequence with the door's previous frame noted for both eyes, so the layer is armed for it.
uint64_t nextArmed(Rig& r) {
    doorFrame(r, r.seq);
    ++r.seq;
    g_stubs.seq = r.seq;
    return r.seq;
}

UiLayerWorldStats statsNow() { return uiLayerWorldStats(); }

// ======================================================================================== the scenarios
void testKeyOff(Rig& r) {
    g_stubs.mayTake = false;
    const uint64_t seq = nextArmed(r);
    bindGame(r, 0);
    const Snap before = take(r.ctx.Get());
    const auto s0 = statsNow();
    const unsigned mips0 = g_stubs.mipsCalls, took0 = g_stubs.tookCalls;
    const Drawn d = drawScreen(r);
    check(!d.taken && !d.pendingAfterDecide && !d.reissued, "key off: the screen composite is not taken and nothing is pending");
    const auto s1 = statsNow();
    check(s1.screenDecided[static_cast<size_t>(UiLayerDecision::kWorldScreen)] == s0.screenDecided[static_cast<size_t>(UiLayerDecision::kWorldScreen)] + 1 &&
              s1.screenAsked == s0.screenAsked + 1 && s1.reissued == s0.reissued,
          "key off: the draw is counted as kWorldScreen, left in the picture, exactly as before the route");
    check(g_stubs.mipsCalls == mips0 && g_stubs.samplerCalls == 0 && g_stubs.tookCalls == took0,
          "key off: the mips module and the route are never asked");
    check(same(before, take(r.ctx.Get())), "key off: the game's state is exactly as it was");
    const auto px = readPixels(r, r.eye[0].tex.Get());
    check(region(px, kEyeW, 8, 8, 56, 40, kRed) && at(px, kEyeW, 2, 2) == kVoid, "key off: the game's draw landed in its eye image (the quad, red, on the void)");
    check(!uiLayerWorldReissueBegin(r.ctx.Get()), "key off: Begin with nothing pending does nothing");
    (void)seq;
}

void testReissue(Rig& r) {
    g_stubs.mayTake = true;
    const uint64_t seq = nextArmed(r);
    bindGame(r, 0);
    const Snap before = take(r.ctx.Get());
    const auto s0 = statsNow();
    const unsigned raw0 = g_stubs.rawCalls, internal0 = g_stubs.rawInternal, took0 = g_stubs.tookCalls, mips0 = g_stubs.mipsCalls;
    const unsigned mipsInternal0 = g_stubs.mipsInternal, samplerInternal0 = g_stubs.samplerInternal, samplerCalls0 = g_stubs.samplerCalls;
    const Drawn d = drawScreen(r);
    check(g_stubs.mipsInternal == mipsInternal0 + 1 && g_stubs.samplerInternal == samplerInternal0 + (g_stubs.samplerCalls - samplerCalls0) && g_stubs.samplerCalls > samplerCalls0,
          "the mips module is asked, for the screen and the sampler, with the route's internal scope up (its copy and its mips step past vscreen's hooks)");
    check(!d.taken, "the route's mode is never a take: uiLayerDecide answers false for the game's draw");
    check(d.pendingAfterDecide, "...and holds the draw for the re-issue that follows the game's own");
    check(d.reissued, "the re-issue ran");
    check(g_stubs.mipsCalls == mips0 + 1 && g_stubs.lastMipsFrame == seq && g_stubs.lastMipsScreen == r.screen.tex.Get() && g_stubs.samplerCalls >= 1,
          "the mips module is asked once, for this frame's sequence and the draw's own screen texture; the sampler for the draw's own");
    check(same(before, take(r.ctx.Get())), "EVERY changed state is back: targets, depth target, viewport, scissor, blend object/factor/mask, depth state, PS slots 0 and 1, samplers, shaders");
    check(g_stubs.tookCalls == took0 + 1 && g_stubs.tookEye == 0 && g_stubs.tookSeq == seq, "the route is told eye 0 was taken, for this sequence, once");
    check(!uiLayerWorldReissuePending() && !uiLayerRedirecting(), "nothing is pending and the layer is not redirecting afterwards");
    const auto s1 = statsNow();
    check(s1.reissued == s0.reissued + 1 && s1.screenDecided[static_cast<size_t>(UiLayerDecision::kRedirect)] == s0.screenDecided[static_cast<size_t>(UiLayerDecision::kRedirect)],
          "the draw is counted as a re-issue -- and NOT as a screen draw redirected into the layer");
    check(g_stubs.rawCalls > raw0 && g_stubs.rawInternal == internal0 + (g_stubs.rawCalls - raw0),
          "every raw entry the re-issue called ran with the route's internal scope up (the hooks step aside)");

    // The game's eye image: the game's own draw, unchanged by the re-issue (its own texture, its own sampler).
    const auto eyePx = readPixels(r, r.eye[0].tex.Get());
    check(region(eyePx, kEyeW, 8, 8, 56, 40, kRed) && at(eyePx, kEyeW, 7, 20) == kVoid && at(eyePx, kEyeW, 56, 20) == kVoid && at(eyePx, kEyeW, 30, 7) == kVoid &&
              at(eyePx, kEyeW, 30, 40) == kVoid,
          "the game's eye image is what the game drew: the quad from ITS screen texture (mip 0, red) on the void, untouched by the re-issue");

    // The layer: composited over a grey frame, then over the black one the door hands on.
    const int gap = uiLayerWorldDoorGap(seq, 0, r.frame[0].tex.Get());
    check(gap == 0, "the door's preflight: the layer holds this frame's eye, able to composite over the black frame");
    const uint32_t whole[4] = {0, 0, kDoorW, kDoorH};
    const float uv[4] = {0, 0, 1, 1};
    ID3D11Texture2D* out = uiLayerComposite(seq, 0, r.frame[0].tex.Get(), whole, uv);
    check(out != nullptr, "the composite runs over the black frame");
    if (out) {
        const auto px = readPixels(r, out);
        // eye pixels [8,56) x [8,40) at the map's scale 1.5 -> layer pixels [12,84) x [12,60)
        check(region(px, kDoorW, 12, 12, 84, 60, kGreen),
              "the layer holds the screen where the quad is -- the MIP LEVEL the map's 2x minification selects (mip 1: green), through the route's trilinear sampler, not the game's mip-0 red");
        check(region(px, kDoorW, 0, 0, kDoorW, 12, kBlack) && region(px, kDoorW, 0, 60, kDoorW, kDoorH, kBlack) && region(px, kDoorW, 0, 12, 12, 60, kBlack) &&
                  region(px, kDoorW, 84, 12, kDoorW, 60, kBlack),
              "...and over the black frame everything the quad does not cover is black (the layer over black IS the eye)");
        out->Release();
    }
    check(uiLayerWorldDoorGap(seq, 0, r.frame[0].tex.Get()) == static_cast<int>(UiWorldDoorGap::kAlreadyDone), "once composited, the preflight says so");

    // Opaque means opaque: over a mid-grey frame the quad still replaces it.
    {
        // 128/255, not 0.5: half of 255 is 127.5, and the float-to-UNORM rounding of a tie is the adapter's (WARP gives 128, a
        // hardware adapter 127), so the clear would differ between --self-test and --hardware. 128/255 converts to 128 on both.
        const float level = 128.0f / 255.0f;
        const float grey[4] = {level, level, level, 1.0f};
        r.ctx->ClearRenderTargetView(r.frame[1].rtv.Get(), grey);
        bindGame(r, 1);
        const Drawn d1 = drawScreen(r);
        check(d1.reissued && g_stubs.tookEye == 1 && g_stubs.tookSeq == seq, "the other eye, same frame: re-issued, and the route is told eye 1");
        check(g_stubs.mipsCalls == mips0 + 2, "...the mips module is asked again per eye (it answers a second eye from its cache)");
        ID3D11Texture2D* o1 = uiLayerComposite(seq, 1, r.frame[1].tex.Get(), whole, uv);
        check(o1 != nullptr, "eye 1's composite runs");
        if (o1) {
            const auto px = readPixels(r, o1);
            check(region(px, kDoorW, 12, 12, 84, 60, kGreen) && at(px, kDoorW, 2, 2) == kGrey && at(px, kDoorW, 90, 66) == kGrey,
                  "over a grey frame: the screen replaces it where the quad is (opaque), the frame shows through where it is not");
            o1->Release();
        }
        const float black[4] = {0, 0, 0, 1};
        r.ctx->ClearRenderTargetView(r.frame[1].rtv.Get(), black);
    }
    check(uiLayerWorldStats().lostDraws == 0, "nothing was left in the game's frame: no lost draws");
}

// The eye shift is normally off while the route owns the frame (native_temporal begin), so the jitter the decision reads is
// zero -- but the frame the route entered, or was released on, is one late either way, and the layer must cancel whatever shift
// the game drew with, exactly as for any UI draw. An exaggerated, exactly representable jitter (1 eye pixel right, 1 down): the
// game's quad moves by it in the eye; the layer's must NOT move.
void testJitterCancel(Rig& r) {
    g_stubs.mayTake = true;
    const uint64_t seq = nextArmed(r);
    g_stubs.jx = 1.0f;
    g_stubs.jy = 1.0f;
    const float base[4] = {-1.0f + 2.0f * 8 / kEyeW, 1.0f - 2.0f * 8 / kEyeH, -1.0f + 2.0f * 56 / kEyeW, 1.0f - 2.0f * 40 / kEyeH};
    const float dx = 2.0f * g_stubs.jx / kEyeW, dy = -2.0f * g_stubs.jy / kEyeH;   // the game's projection: content jx right, jy down
    const float shifted[4] = {base[0] + dx, base[1] + dy, base[2] + dx, base[3] + dy};
    r.ctx->UpdateSubresource(r.rectCb.Get(), 0, nullptr, shifted, 0, 0);
    bindGame(r, 0);
    const Drawn d = drawScreen(r);
    check(d.reissued, "a jittered frame: re-issued");
    const auto eyePx = readPixels(r, r.eye[0].tex.Get());
    check(region(eyePx, kEyeW, 9, 9, 57, 41, kRed) && at(eyePx, kEyeW, 8, 20) == kVoid && at(eyePx, kEyeW, 57, 20) == kVoid,
          "the game's own quad moved by the jitter (one eye pixel right and down), as the game drew it");
    const uint32_t whole[4] = {0, 0, kDoorW, kDoorH};
    const float uv[4] = {0, 0, 1, 1};
    ID3D11Texture2D* out = uiLayerComposite(seq, 0, r.frame[0].tex.Get(), whole, uv);
    check(out != nullptr, "the composite runs");
    if (out) {
        const auto px = readPixels(r, out);
        check(region(px, kDoorW, 12, 12, 84, 60, kGreen) && at(px, kDoorW, 11, 30) == kBlack && at(px, kDoorW, 84, 30) == kBlack &&
                  at(px, kDoorW, 40, 11) == kBlack && at(px, kDoorW, 40, 60) == kBlack,
              "the layer's quad did NOT move: the jitter the decision read (1, 1 eye pixels) was cancelled through the map, to the layer pixel");
        out->Release();
    }
    g_stubs.jx = g_stubs.jy = 0.0f;
    r.ctx->UpdateSubresource(r.rectCb.Get(), 0, nullptr, base, 0, 0);
}

// A refusal: the game's draw still landed, nothing was taken, the game's state is untouched, the route was not told,
// and the counter says why.
template <class Arrange>
void refusal(Rig& r, const char* name, UiWorldRefuse reason, UiLayerDecision decision, Arrange arrange, bool substituted = false) {
    g_stubs.mayTake = true;
    g_stubs.mipsAnswer = r.mipsSrv.Get();
    g_stubs.samplerAnswer = r.mipSampler.Get();
    nextArmed(r);
    bindGame(r, 0);
    arrange();
    const Snap before = take(r.ctx.Get());
    const auto s0 = statsNow();
    const unsigned took0 = g_stubs.tookCalls;
    const Drawn d = drawScreen(r, substituted);
    char msg[320];
    std::snprintf(msg, sizeof(msg), "%s: not taken, not re-issued, the game's state untouched", name);
    check(!d.taken && !d.reissued && same(before, take(r.ctx.Get())) && !uiLayerWorldReissuePending() && !uiLayerRedirecting(), msg);
    const auto s1 = statsNow();
    std::snprintf(msg, sizeof(msg), "%s: the route is not told, and the refusal is counted", name);
    const bool byRoute = reason != UiWorldRefuse::kNone && s1.refused[static_cast<size_t>(reason)] == s0.refused[static_cast<size_t>(reason)] + 1;
    const bool byDecision = decision != UiLayerDecision::kRedirect &&
                            s1.screenDecided[static_cast<size_t>(decision)] == s0.screenDecided[static_cast<size_t>(decision)] + 1;
    const bool counted = byRoute || byDecision;
    check(g_stubs.tookCalls == took0 && counted && s1.reissued == s0.reissued, msg);
    std::snprintf(msg, sizeof(msg), "%s: the layer holds nothing of this frame, so the door cannot go layer-only for the eye", name);
    check(uiLayerWorldDoorGap(g_stubs.seq, 0, r.frame[0].tex.Get()) == static_cast<int>(UiWorldDoorGap::kNoContent), msg);
    g_stubs.mipsAnswer = r.mipsSrv.Get();
    g_stubs.samplerAnswer = r.mipSampler.Get();
}

void testRefusals(Rig& r) {
    refusal(r, "no mipped screen", UiWorldRefuse::kMipsNull, UiLayerDecision::kRedirect, [&] { g_stubs.mipsAnswer = nullptr; });
    refusal(r, "no trilinear sampler", UiWorldRefuse::kSamplerNull, UiLayerDecision::kRedirect, [&] { g_stubs.samplerAnswer = nullptr; });
    refusal(r, "no sampler bound at slot 0", UiWorldRefuse::kNoSampler, UiLayerDecision::kRedirect, [&] {
        ID3D11SamplerState* none = nullptr;
        r.ctx->PSSetSamplers(0, 1, &none);
    });
    refusal(r, "no texture at slot 0", UiWorldRefuse::kNoSource, UiLayerDecision::kRedirect, [&] {
        ID3D11ShaderResourceView* none = nullptr;
        r.ctx->PSSetShaderResources(0, 1, &none);
    });
    // (A curved screen is no refusal: its draw is the curve substitution's strip and the plan accepts it like a flat draw, testCurvedScreen below.
    // It is refused for the same other reasons a flat draw is: here, a blend.)
    refusal(r, "a blending draw", UiWorldRefuse::kNotOpaque, UiLayerDecision::kRedirect, [&] {
        bindGame(r, 0, r.premul.Get());
    });
    refusal(r, "a substituted (curved) draw that blends", UiWorldRefuse::kNotOpaque, UiLayerDecision::kRedirect, [&] {
        bindGame(r, 0, r.premul.Get());
    }, /*substituted=*/true);
    refusal(r, "a blend the layer cannot convert", UiWorldRefuse::kNone, UiLayerDecision::kBlendRefused, [&] {
        bindGame(r, 0, r.maxBlend.Get());
    });
    // the door already ran for this eye this frame (gate G1): the layer's composite has been, nothing more can be taken
    {
        g_stubs.mayTake = true;
        const uint64_t seq = nextArmed(r);
        bindGame(r, 0);
        uiLayerNoteSubmitted(seq, 0, r.eye[0].tex.Get());
        uiLayerNoteTemporal(seq, 0, r.frame[0].tex.Get());
        uiLayerDoorSeen(seq, 0, r.frame[0].tex.Get());
        const Snap before = take(r.ctx.Get());
        const auto s0 = statsNow();
        const Drawn d = drawScreen(r);
        check(!d.taken && !d.reissued && same(before, take(r.ctx.Get())) &&
                  statsNow().screenDecided[static_cast<size_t>(UiLayerDecision::kLate)] == s0.screenDecided[static_cast<size_t>(UiLayerDecision::kLate)] + 1,
              "late (the eye's door already ran this frame): counted as kLate, not re-issued, the game's state untouched");
    }
    refusal(r, "a depth-tested draw", UiWorldRefuse::kDepthState, UiLayerDecision::kDepthStencilTest, [&] {
        bindGame(r, 0, nullptr, r.dsOn.Get());
    });
    // not armed: the door did not run for the previous sequence
    {
        g_stubs.mayTake = true;
        ++r.seq;
        g_stubs.seq = r.seq + 5;  // a sequence the door never saw
        bindGame(r, 0);
        const Snap before = take(r.ctx.Get());
        const auto s0 = statsNow();
        const Drawn d = drawScreen(r);
        const auto s1 = statsNow();
        check(!d.taken && !d.reissued && same(before, take(r.ctx.Get())), "not armed: not taken, not re-issued, the game's state untouched");
        check(s1.screenDecided[static_cast<size_t>(UiLayerDecision::kNotArmed)] == s0.screenDecided[static_cast<size_t>(UiLayerDecision::kNotArmed)] + 1,
              "not armed: counted as the decision's own kNotArmed");
        g_stubs.seq = r.seq;
    }
    // the eye could not be told
    {
        g_stubs.mayTake = true;
        nextArmed(r);
        bindGame(r, 0);
        const void* keep = g_stubs.eyeRes[0];
        g_stubs.eyeRes[0] = nullptr;
        const auto s0 = statsNow();
        const Drawn d = drawScreen(r);
        g_stubs.eyeRes[0] = keep;
        check(!d.taken && !d.reissued && statsNow().screenDecided[static_cast<size_t>(UiLayerDecision::kNoEye)] == s0.screenDecided[static_cast<size_t>(UiLayerDecision::kNoEye)] + 1,
              "an eye that cannot be told: counted as kNoEye, nothing re-issued");
    }
    // the draw's bindings changed between its decision and the re-issue
    {
        g_stubs.mayTake = true;
        nextArmed(r);
        bindGame(r, 0);
        const auto s0 = statsNow();
        const unsigned took0 = g_stubs.tookCalls;
        const bool taken = uiLayerDecide(r.ctx.Get(), static_cast<int>(UiLayerFamily::kScreen), true, false);
        check(!taken && uiLayerWorldReissuePending(), "changed bindings: the decision held the draw");
        r.ctx->Draw(4, 0);
        ID3D11ShaderResourceView* other = r.otherSrv.Get();
        r.ctx->PSSetShaderResources(0, 1, &other);   // something else at slot 0 by the time the re-issue asks
        const Snap mid = take(r.ctx.Get());
        VrWorldInternalScope internal;
        const bool began = uiLayerWorldReissueBegin(r.ctx.Get());
        check(!began && same(mid, take(r.ctx.Get())) && !uiLayerRedirecting(), "...the re-issue finds slot 0 is not the decided draw's texture: refused, the state untouched");
        const auto s1 = statsNow();
        check(s1.refused[static_cast<size_t>(UiWorldRefuse::kStateChanged)] == s0.refused[static_cast<size_t>(UiWorldRefuse::kStateChanged)] + 1 && g_stubs.tookCalls == took0,
              "...counted as a changed binding, the route not told");
    }
    // the target changed
    {
        g_stubs.mayTake = true;
        nextArmed(r);
        bindGame(r, 0);
        const unsigned took0 = g_stubs.tookCalls;
        const bool taken = uiLayerDecide(r.ctx.Get(), static_cast<int>(UiLayerFamily::kScreen), true, false);
        r.ctx->Draw(4, 0);
        bindGame(r, 1);   // another eye's target is bound when the re-issue asks
        VrWorldInternalScope internal;
        check(!taken && !uiLayerWorldReissueBegin(r.ctx.Get()) && g_stubs.tookCalls == took0, "a changed render target: refused at the re-issue");
    }
    // abandoned: the game's draw was swallowed
    {
        g_stubs.mayTake = true;
        nextArmed(r);
        bindGame(r, 0);
        const bool taken = uiLayerDecide(r.ctx.Get(), static_cast<int>(UiLayerFamily::kScreen), true, false);
        check(!taken && uiLayerWorldReissuePending(), "abandon: held");
        uiLayerWorldReissueAbandon();
        VrWorldInternalScope internal;
        check(!uiLayerWorldReissuePending() && !uiLayerWorldReissueBegin(r.ctx.Get()), "abandon: the flag is down and Begin finds nothing to do");
    }
    // a new family decision forgets a pending re-issue; the after-UI retry's kAfterUi decision does not
    {
        g_stubs.mayTake = true;
        nextArmed(r);
        bindGame(r, 0);
        uiLayerDecide(r.ctx.Get(), static_cast<int>(UiLayerFamily::kScreen), true, false);
        check(uiLayerWorldReissuePending(), "retry: the screen draw's re-issue is pending");
        uiLayerDecide(r.ctx.Get(), static_cast<int>(UiLayerFamily::kAfterUi), true, false, 0);
        check(uiLayerWorldReissuePending(), "...and the after-UI retry's decision (kAfterUi) leaves it alone");
        uiLayerDecide(r.ctx.Get(), static_cast<int>(UiLayerFamily::kPanel), true, false);
        check(!uiLayerWorldReissuePending(), "...while a real family's decision forgets it");
        {
            // The flag is what vscreen reads; the plan behind it is forgotten as well. Nothing in the DLL calls Begin without
            // the flag, so this is the only way to see that the plan itself went: a Begin here finds nothing to re-issue.
            VrWorldInternalScope internal;
            check(!uiLayerWorldReissueBegin(r.ctx.Get()), "...and the plan with it: a Begin after that decision finds nothing to re-issue");
        }
        uiLayerWorldReissueAbandon();
    }
}

// A CURVED screen (fix.panel_curvature above 0). The game's draw is the curve substitution's strip, which uiLayerDecide is told as `substituted`;
// the route's re-issue repeats that strip (vscreen.cpp worldScreenReissueCurved, between Begin and End), and End is told whether the strip's draw
// happened. This rig draws the quad for both issues, as for a flat screen: what is proven here is the layer's half -- the plan accepts a substituted
// draw like a flat one, Begin and End land it and take the eye, and a draw that did not happen takes nothing.
void testCurvedScreen(Rig& r) {
    g_stubs.mayTake = true;
    g_stubs.mipsAnswer = r.mipsSrv.Get();
    g_stubs.samplerAnswer = r.mipSampler.Get();
    const uint32_t whole[4] = {0, 0, kDoorW, kDoorH};
    const float uv[4] = {0, 0, 1, 1};
    const auto layerOf = [&](uint64_t seq) {
        std::vector<uint32_t> px;
        if (ID3D11Texture2D* out = uiLayerComposite(seq, 0, r.frame[0].tex.Get(), whole, uv)) {
            px = readPixels(r, out);
            out->Release();
        }
        return px;
    };
    // The flat draw first, as the control: what its layer holds.
    uint64_t seq = nextArmed(r);
    bindGame(r, 0);
    const Drawn flat = drawScreen(r, false);
    const std::vector<uint32_t> flatLayer = layerOf(seq);
    check(flat.reissued && !flat.taken && !flatLayer.empty() && region(flatLayer, kDoorW, 12, 12, 84, 60, kGreen),
          "a flat draw (the control): re-issued, and its layer holds the screen where the quad is");

    // A substituted draw: planned and re-issued exactly like it.
    seq = nextArmed(r);
    bindGame(r, 0);
    const Snap before = take(r.ctx.Get());
    const auto s0 = statsNow();
    const unsigned took0 = g_stubs.tookCalls;
    const Drawn d = drawScreen(r, /*substituted=*/true);
    check(!d.taken && d.pendingAfterDecide && d.reissued,
          "a substituted (curved) screen draw: the decision is the route's (never a take), the draw is held for the re-issue and the re-issue lands -- as for a flat one");
    const auto s1 = statsNow();
    bool refusedAny = false;
    for (size_t i = 0; i < static_cast<size_t>(UiWorldRefuse::kCount); ++i) refusedAny = refusedAny || s1.refused[i] != s0.refused[i];
    check(!refusedAny && s1.reissued == s0.reissued + 1 &&
              s1.screenDecided[static_cast<size_t>(UiLayerDecision::kRedirect)] == s0.screenDecided[static_cast<size_t>(UiLayerDecision::kRedirect)],
          "...no refusal of any kind is counted (a curved screen is not one), and the draw is counted as a re-issue, not as a screen draw redirected into the layer");
    check(g_stubs.tookCalls == took0 + 1 && g_stubs.tookEye == 0 && g_stubs.tookSeq == seq, "...the route is told eye 0 was taken, for this sequence, once");
    check(same(before, take(r.ctx.Get())) && !uiLayerWorldReissuePending() && !uiLayerRedirecting(),
          "...every changed state is back, and nothing is pending or redirecting afterwards");
    {   // The eye is the route's for this frame, as a flat screen's is: a post pass the game leaves in its eye image is counted as lost.
        ID3D11ShaderResourceView* post = r.eye[1].srv.Get();
        r.ctx->PSSetShaderResources(0, 1, &post);
        detail::g_bindingSlots[static_cast<size_t>(BindSlot::PsSrv0)].ptr = post;
        uiLayerNoteOther(r.ctx.Get(), 3, true, false, false, false, 1, 0, 'D');
        detail::g_bindingSlots[static_cast<size_t>(BindSlot::PsSrv0)].ptr = nullptr;
        check(statsNow().lostDraws == s1.lostDraws + 1, "...the eye is the route's, as a flat screen's is: a post pass the game leaves in its eye image is counted as a lost draw");
    }
    const std::vector<uint32_t> curvedLayer = layerOf(seq);
    check(!curvedLayer.empty() && curvedLayer == flatLayer,
          "...and the layer holds exactly what a flat draw's layer holds (the substitution changes nothing about how the layer takes the draw)");

    // The strip's draw did not happen (panel_curve.h panelCurveReissue returned false after Begin): End(landed = false) closes the bracket
    // and takes nothing -- the eye route serves the eye -- and the refusal is counted as a fault.
    seq = nextArmed(r);
    bindGame(r, 0);
    const Snap beforeFault = take(r.ctx.Get());
    const auto f0 = statsNow();
    const unsigned tookF0 = g_stubs.tookCalls;
    const bool taken = uiLayerDecide(r.ctx.Get(), static_cast<int>(UiLayerFamily::kScreen), true, /*substituted=*/true);
    r.ctx->Draw(4, 0);   // the game's own issue
    check(!taken && uiLayerWorldReissuePending(), "a re-issue whose strip draw faults: the decision held the draw");
    {
        VrWorldInternalScope internal;   // worldScreenReissueCurved's scope, around Begin and End
        const bool began = uiLayerWorldReissueBegin(r.ctx.Get());
        check(began && uiLayerRedirecting(), "...Begin lands: the layer is bound, with the mipped screen and the sampler at PS slot 0");
        // (the draw between Begin and End did not happen)
        uiLayerWorldReissueEnd(r.ctx.Get(), /*landed=*/false);
    }
    const auto f1 = statsNow();
    check(same(beforeFault, take(r.ctx.Get())) && !uiLayerRedirecting() && !uiLayerWorldReissuePending(),
          "...End(landed = false) puts every changed state back and closes the bracket");
    check(g_stubs.tookCalls == tookF0 && f1.reissued == f0.reissued &&
              f1.refused[static_cast<size_t>(UiWorldRefuse::kFault)] == f0.refused[static_cast<size_t>(UiWorldRefuse::kFault)] + 1,
          "...the eye is NOT taken (the route is not told, nothing is counted as a re-issue) and the refusal is counted as a fault");
    {   // The eye is not the route's: a draw the game then leaves in its own eye image (a post pass over it) is not counted as lost, as it is
        // for a re-issued eye (above and testLostDraws) -- the eye route serves this eye.
        ID3D11ShaderResourceView* post = r.eye[1].srv.Get();
        r.ctx->PSSetShaderResources(0, 1, &post);
        detail::g_bindingSlots[static_cast<size_t>(BindSlot::PsSrv0)].ptr = post;
        uiLayerNoteOther(r.ctx.Get(), 3, true, false, false, false, 1, 0, 'D');
        detail::g_bindingSlots[static_cast<size_t>(BindSlot::PsSrv0)].ptr = nullptr;
        check(statsNow().lostDraws == f1.lostDraws, "...the eye is not the route's: a post pass the game leaves in its eye image is not counted as a lost draw");
    }
    uiLayerWorldReissueEnd(r.ctx.Get(), false);   // a second End without a Begin changes nothing
    check(statsNow().refused[static_cast<size_t>(UiWorldRefuse::kFault)] == f1.refused[static_cast<size_t>(UiWorldRefuse::kFault)] && g_stubs.tookCalls == tookF0,
          "...and a stray End(landed = false) counts nothing more");
    // Nothing is left stuck: the next frame's draw lands and takes the eye as usual.
    const uint64_t next = nextArmed(r);
    bindGame(r, 0);
    const Drawn again = drawScreen(r, true);
    check(again.reissued && g_stubs.tookCalls == tookF0 + 1 && g_stubs.tookSeq == next, "...and the next frame's substituted draw lands and takes the eye (nothing is stuck)");
    // A draw that landed, told so by default, is what it always was: End(ctx) and End(ctx, true) are one thing (every flat caller passes nothing).
    const uint64_t last = nextArmed(r);
    bindGame(r, 0);
    uiLayerDecide(r.ctx.Get(), static_cast<int>(UiLayerFamily::kScreen), true, false);
    r.ctx->Draw(4, 0);
    const unsigned tookL0 = g_stubs.tookCalls;
    {
        VrWorldInternalScope internal;
        const bool began = uiLayerWorldReissueBegin(r.ctx.Get());
        r.ctx->Draw(4, 0);
        uiLayerWorldReissueEnd(r.ctx.Get(), /*landed=*/true);
        check(began && g_stubs.tookCalls == tookL0 + 1 && g_stubs.tookSeq == last, "End(landed = true) takes the eye, exactly as End(ctx) with no second argument does");
    }
}

void testBeginWithoutOuterScope(Rig& r) {
    // Begin and End carry the internal scope themselves: no outer VrWorldInternalScope here.
    g_stubs.mayTake = true;
    nextArmed(r);
    bindGame(r, 0);
    uiLayerDecide(r.ctx.Get(), static_cast<int>(UiLayerFamily::kScreen), true, false);
    r.ctx->Draw(4, 0);
    const unsigned raw0 = g_stubs.rawCalls, internal0 = g_stubs.rawInternal;
    const bool began = uiLayerWorldReissueBegin(r.ctx.Get());
    if (began) {
        r.ctx->Draw(4, 0);
        uiLayerWorldReissueEnd(r.ctx.Get());
    }
    check(began && g_stubs.rawCalls > raw0 && g_stubs.rawInternal == internal0 + (g_stubs.rawCalls - raw0),
          "Begin and End set the internal scope themselves: every raw entry they call ran with it up");
    check(!g_vrWorldInternal && !g_flatComputeInternal, "...and leave it down (no scope leaks out of either)");
    uiLayerWorldReissueEnd(r.ctx.Get());   // a second End without a Begin is harmless
    check(!uiLayerRedirecting(), "...a stray End does nothing");
}

void testLostDraws(Rig& r) {
    // After the screen was re-issued into an eye, a draw the game leaves in that eye's image is lost while the route owns
    // the eye: counted (a post pass over the eye that writes it, the case uiLayerNoteOther names).
    g_stubs.mayTake = true;
    const uint64_t seq = nextArmed(r);
    bindGame(r, 0);
    const Drawn d = drawScreen(r);
    check(d.reissued, "lost draws: the screen was re-issued into eye 0");
    const auto s0 = statsNow();
    // a pass into the same eye image that samples an eye-sized input: left in the game's frame as a post pass
    ID3D11ShaderResourceView* post = r.eye[1].srv.Get();   // eye-sized, not the screen
    r.ctx->PSSetShaderResources(0, 1, &post);
    detail::g_bindingSlots[static_cast<size_t>(BindSlot::PsSrv0)].ptr = post;
    uiLayerNoteOther(r.ctx.Get(), 3, true, false, false, false, 1, 0, 'D');
    const auto s1 = statsNow();
    check(s1.lostDraws == s0.lostDraws + 1, "a post pass into a re-issued eye is left in the game's frame and COUNTED as lost");
    (void)seq;
    detail::g_bindingSlots[static_cast<size_t>(BindSlot::PsSrv0)].ptr = nullptr;
}

// fix.ui_quality 125: the layer is 1.25x the door's frame (120x90 for a 96x72 frame) and the composite box-filters it down. The
// re-issue maps the eye onto that layer exactly as it maps onto the 1.0 one, so the screen lands where it does at 100.
void test125(Rig& r) {
    auto& cfg = Config::get();
    cfg.set("fix.ui_quality", "125");
    uiLayerConfigure(cfg);
    g_stubs.mayTake = true;
    const uint64_t seq = nextArmed(r);
    bindGame(r, 0);
    const Drawn d = drawScreen(r);
    check(d.reissued, "fix.ui_quality 125: the screen is re-issued into the 1.25x layer");
    const uint32_t whole[4] = {0, 0, kDoorW, kDoorH};
    const float uv[4] = {0, 0, 1, 1};
    ID3D11Texture2D* out = uiLayerComposite(seq, 0, r.frame[0].tex.Get(), whole, uv);
    check(out != nullptr, "...and the composite box-filters the 120x90 layer down to the 96x72 frame");
    if (out) {
        const auto px = readPixels(r, out);
        // The quad is 90x60 layer pixels for 144x96 texels: 1.6 texels a pixel, LOD log2(1.6) = 0.678 -- the trilinear blend of
        // mip 0 (red) and mip 1 (green), (82, 173, 0): the map's scale selects the level, not the layer's size alone.
        check(regionNear(px, kDoorW, 12, 12, 84, 60, 0xFF00AD52u, 3) && region(px, kDoorW, 0, 0, kDoorW, 12, kBlack) && region(px, kDoorW, 0, 60, kDoorW, kDoorH, kBlack) &&
                  region(px, kDoorW, 0, 12, 12, 60, kBlack) && region(px, kDoorW, 84, 12, kDoorW, 60, kBlack),
              "...the quad lands exactly where it does at 100 ([12,84) x [12,60)), sampled at the LOD its 1.25x layer pixels select (0.678: mip 0 and mip 1 blended)");
        out->Release();
    }
    cfg.set("fix.ui_quality", "100");
    uiLayerConfigure(cfg);
}

void testAccessors(Rig& r) {
    check(uiLayerLiveForWorldRoute() && uiLayerNotLiveReason() == nullptr, "the layer is live with fix.ui_quality on, a temporal mode on and the jitter as shipped");
    check(uiLayerWorldScreenHeld(), "the gate holds the screen (the journal says on foot)");
    auto& cfg = Config::get();
    cfg.set("fix.ui_quality", "off");
    uiLayerConfigure(cfg);
    check(!uiLayerLiveForWorldRoute() && uiLayerNotLiveReason() && std::strcmp(uiLayerNotLiveReason(), "fix.ui_quality is off") == 0 && !uiLayerLive(),
          "fix.ui_quality off: not live, with its reason, and the draw path's own gate agrees");
    cfg.set("fix.ui_quality", "100");
    cfg.set("fix.temporal_aa", "off");
    uiLayerConfigure(cfg);
    check(!uiLayerLiveForWorldRoute() && uiLayerNotLiveReason() && std::strstr(uiLayerNotLiveReason(), "fix.temporal_aa"), "no temporal mode: not live, with its reason");
    cfg.set("fix.temporal_aa", "dlss");
    cfg.set("advanced.temporal_aa_jitter_sign", "flip_x");
    uiLayerConfigure(cfg);
    check(!uiLayerLiveForWorldRoute() && uiLayerNotLiveReason() && std::strstr(uiLayerNotLiveReason(), "jitter"), "jitter not as shipped: not live, with its reason");
    cfg.set("advanced.temporal_aa_jitter_sign", "as_is");
    uiLayerConfigure(cfg);
    check(uiLayerLiveForWorldRoute() && uiLayerNotLiveReason() == nullptr && uiLayerLive(), "back as shipped: live again");
    (void)r;
}

void testDoorGaps(Rig& r) {
    g_stubs.mayTake = true;
    const uint64_t seq = nextArmed(r);
    bindGame(r, 0);
    // before any re-issue this frame: the layer holds nothing of it
    check(uiLayerWorldDoorGap(seq, 0, r.frame[0].tex.Get()) == static_cast<int>(UiWorldDoorGap::kNoContent),
          "the preflight before the re-issue: the layer holds nothing of this frame");
    drawScreen(r);
    check(uiLayerWorldDoorGap(seq, 0, r.frame[0].tex.Get()) == 0, "...and after it: ready");
    check(uiLayerWorldDoorGap(seq, 2, r.frame[0].tex.Get()) == static_cast<int>(UiWorldDoorGap::kNoLayer) &&
              uiLayerWorldDoorGap(seq, 0, nullptr) == static_cast<int>(UiWorldDoorGap::kNoLayer),
          "an eye that does not exist, or no frame: no layer");
    Tex wide = makeTex(r, 200, 72, DXGI_FORMAT_R8G8B8A8_TYPELESS, D3D11_BIND_SHADER_RESOURCE, DXGI_FORMAT_R8G8B8A8_UNORM, kBlack);
    check(uiLayerWorldDoorGap(seq, 0, wide.tex.Get()) == static_cast<int>(UiWorldDoorGap::kAspect), "a frame of another shape than the layer: refused (the door's size is changing)");
    Tex half = makeTex(r, kDoorW, kDoorH, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D11_BIND_SHADER_RESOURCE, DXGI_FORMAT_R16G16B16A16_FLOAT, 0);
    check(uiLayerWorldDoorGap(seq, 0, half.tex.Get()) == static_cast<int>(UiWorldDoorGap::kCannotComposite),
          "a frame the composite does not run over (half-float): refused, so the black frame is never handed to a door that cannot put a picture over it");
    check(uiLayerWorldDoorGap(seq, 1, r.frame[1].tex.Get()) == static_cast<int>(UiWorldDoorGap::kNoContent), "the other eye, nothing re-issued into it yet: no content");
    const uint32_t whole[4] = {0, 0, kDoorW, kDoorH};
    const float unit[4] = {0, 0, 1, 1};
    ID3D11Texture2D* out = uiLayerComposite(seq, 0, r.frame[0].tex.Get(), whole, unit);
    if (out) out->Release();
}
}  // namespace

// ================================================================ the on-foot maps gate (experimental.on_foot_maps_sharp)
// The layer's half, the real ui_layer.cpp (docs/design-world-camera-motion-2026-09-30.md, Phase 1; the pure half and the source pins
// are tools\on_foot_maps_test). The boundary is driven the way vscreen.cpp drives it, once a frame, with the frame's naming told
// first (uiLayerNoteScreenNamed: screen_motion.cpp's one call); the 2D screen composite is taken the way forwardWithVerdict takes it
// (decide, Begin around the game's own draw, End). What it proves, against the production functions:
//   1. KEY OFF: the gate is today's journal-or-depth, frame by frame over a scripted run of every input, and nothing of the feature
//      is on, marked, asked or logged.
//   2. KEY ON: the gate follows the naming alone -- held on the second named frame, released on the third unnamed -- carried from
//      today's gate at the switch; a taken screen composite marks the eye, and the door's predicate answers for it only while every
//      draw into an eye-sized target was taken; the route's re-issue is not a take; the key off, the layer off and screen motion off
//      each hand the gate back to today's, with a line saying why.
//   3. THE LINES: every one of them, in the order the script makes them, and none while the key is off.
bool boundaryFrame(Rig& r, bool named) {
    if (named) uiLayerNoteScreenNamed();
    uiLayerFrameBoundary(r.ctx.Get());
    return uiLayerWorldScreenHeld();
}

// A taken 2D screen composite, as forwardWithVerdict handles it: the decision, the layer's Begin around the game's own draw, End.
bool takeScreen(Rig& r, int eye) {
    bindGame(r, eye);
    const bool decided = uiLayerDecide(r.ctx.Get(), static_cast<int>(UiLayerFamily::kScreen), true, false);
    if (!decided) return false;
    const bool layered = uiLayerBegin(r.ctx.Get());
    r.ctx->Draw(4, 0);
    if (layered) uiLayerEnd(r.ctx.Get());
    return layered;
}

std::vector<std::string> mapsLines(const std::wstring& dir) {
    std::vector<std::string> out;
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW((dir + L"\\*.log").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        std::ifstream in(dir + L"\\" + fd.cFileName, std::ios::binary);
        std::string line;
        while (std::getline(in, line)) {
            const size_t at = line.find("on foot maps sharp");
            if (at == std::string::npos) continue;
            while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
            out.push_back(line.substr(at));
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return out;
}

void testMapsGate(Rig& r) {
    auto& cfg = Config::get();
    wchar_t temp[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, temp);
    const std::wstring dir = std::wstring(temp) + L"edvr_ui_layer_world_maps_" + std::to_wstring(GetCurrentProcessId());
    CreateDirectoryW(dir.c_str(), nullptr);
    check(Log::get().open(dir, L"uilwmaps"), "maps: the rig's log opens in a temp directory (its lines are read back at the end)");

    // ---- 1. KEY OFF: today's gate, frame by frame, and nothing of the feature anywhere.
    cfg.set("experimental.on_foot_maps_sharp", "off");
    uiLayerConfigure(cfg);
    {
        UiOnFootGate refJournal;
        UiWorldScreenGate refDepth;
        uint32_t bits = 7;
        bool same = true, off = true, doorOff = true;
        const uint32_t counts[4] = {0, 10, 100, 5000};
        unsigned held = 0, open = 0;
        // 400 frames in four stretches, so both signals and the depth's hysteresis (2 busy frames to hold, 90 quiet ones to let go) are
        // all exercised: the journal alone with no depth count; the depth busy with the journal aboard; the depth quiet long enough to
        // let go; every input at random.
        for (unsigned i = 0; i < 400; ++i) {
            bits = bits * 1664525u + 1013904223u;
            g_stubs.journalKnown = true;                       // (an unknown reading holds for 3 s of the wall clock; the pure rig has it)
            if (i < 100) {
                g_stubs.journalOnFoot = (bits >> 8) & 1u;
                g_stubs.depthKnown = false;
                g_stubs.depthDraws = 0;
            } else if (i < 130) {
                g_stubs.journalOnFoot = false;
                g_stubs.depthKnown = true;
                g_stubs.depthDraws = 5000;
            } else if (i < 300) {
                g_stubs.journalOnFoot = false;
                g_stubs.depthKnown = true;
                g_stubs.depthDraws = 3;
            } else {
                g_stubs.journalOnFoot = (bits >> 8) & 1u;
                g_stubs.depthKnown = ((bits >> 9) & 3u) != 0;
                g_stubs.depthDraws = counts[(bits >> 12) & 3u];
            }
            const bool named = ((bits >> 16) & 1u) != 0;       // the naming is told every other frame: with the key off nobody reads it
            // The frozen copy of today's gate (ui_layer.cpp onFootGateTick before the maps gate): both steps run every frame, then OR.
            const bool wantJournal = uiLayerOnFootStep(refJournal, true, g_stubs.journalOnFoot, 1000 + i * 11);
            const bool wantDepth = uiLayerWorldScreenStep(refDepth, g_stubs.depthKnown, g_stubs.depthKnown ? g_stubs.depthDraws : 0);
            const bool want = wantJournal || wantDepth;
            const bool got = boundaryFrame(r, named);
            if (i >= 2) same = same && got == want;            // the layer's gates were seeded by the rig's earlier frames: two frames converge them
            off = off && !uiLayerMapsOn();
            held += got ? 1u : 0u;
            open += got ? 0u : 1u;
            g_stubs.routeDoor = false;
            doorOff = doorOff && !uiLayerDoorLayerOnly(0, r.seq) && !uiLayerDoorLayerOnly(1, r.seq);
        }
        check(same, "maps, key off: the gate is the journal's on-foot reading OR the screen's own depth, frame by frame over 400 scripted inputs");
        check(held > 100 && open > 100, "maps, key off: (setup) the script held the gate and opened it, both for a good part of the run");
        check(off && doorOff, "maps, key off: the maps gate is never on and the door's predicate answers for the route's question alone (false here)");
        g_stubs.routeDoor = true;
        check(uiLayerDoorLayerOnly(0, r.seq) && uiLayerDoorLayerOnly(1, 77), "maps, key off: ... and answers true exactly when the route says so");
        g_stubs.routeDoor = false;
    }

    // ---- 2. KEY ON, from a world held by the journal.
    g_stubs.journalKnown = g_stubs.journalOnFoot = true;
    g_stubs.depthKnown = false;
    g_stubs.depthDraws = 0;
    boundaryFrame(r, true);
    boundaryFrame(r, true);
    check(uiLayerWorldScreenHeld(), "maps: (setup) on foot, the journal holds the screen");
    cfg.set("experimental.on_foot_maps_sharp", "on");
    uiLayerConfigure(cfg);
    check(boundaryFrame(r, true) && uiLayerMapsOn(), "maps, key on: the first boundary switches the naming in, carrying the world today's gate held");
    check(boundaryFrame(r, true) && boundaryFrame(r, true) && uiLayerWorldScreenHeld(), "maps: named frames keep the world held");
    // The journal still says on foot throughout: with the key on it is not asked.
    check(boundaryFrame(r, false) && boundaryFrame(r, false), "maps: two unnamed frames (a map's first two) release nothing");
    check(!boundaryFrame(r, false) && uiLayerMapsOn(), "maps: the third unnamed frame releases the panel to the layer, though the journal still says on foot");
    check(!boundaryFrame(r, false) && !boundaryFrame(r, false), "maps: a map that goes on stays with the layer");

    // The take: the layer takes the 2D screen composite, marks the eye, and the door's predicate answers for it.
    {
        g_stubs.mayTake = false;
        g_stubs.eyeDraws = 0;
        const uint64_t seq = nextArmed(r);
        g_stubs.eyeDraws = 1;   // the game drew one composite into an eye-sized target
        const bool took0 = takeScreen(r, 0);
        check(took0, "maps: with the gate on and the panel the layer's, the 2D screen composite is TAKEN (decided, Begin, drawn into the layer)");
        const auto px = readPixels(r, r.eye[0].tex.Get());
        check(at(px, kEyeW, 30, 20) == kVoid, "maps: ... and the game's eye image holds nothing of it (the draw went into the layer)");
        check(uiLayerDoorLayerOnly(0, seq), "maps: the door runs layer-only for the eye whose 2D screen was taken when every eye draw was taken");
        check(uiLayerDoorLayerOnly(0, seq), "maps: ... and answers the same the second time it is asked (the sharpen door asks again)");
        check(!uiLayerDoorLayerOnly(1, seq), "maps: the other eye, whose screen was not taken, is not layer-only");
        check(!uiLayerDoorLayerOnly(0, seq + 1) && !uiLayerDoorLayerOnly(0, 0), "maps: another sequence, or sequence 0, never matches the mark");
        check(uiLayerWorldDoorGap(seq, 0, r.frame[0].tex.Get()) == 0, "maps: the door's preflight finds the layer holding the eye's frame");
        g_stubs.eyeDraws = 2;   // the game drew a second thing into an eye-sized target that the layer did not take
        check(!uiLayerDoorLayerOnly(0, seq), "maps: one eye draw the layer did not take leaves the eye with something else in it: it keeps the upscaler");
        const bool took1 = takeScreen(r, 1);
        check(took1 && uiLayerDoorLayerOnly(0, seq) && uiLayerDoorLayerOnly(1, seq),
              "maps: with both composites taken and both eye draws counted, both eyes are layer-only");
        const uint32_t whole[4] = {0, 0, kDoorW, kDoorH};
        const float uv[4] = {0, 0, 1, 1};
        ID3D11Texture2D* out = uiLayerComposite(seq, 0, r.frame[0].tex.Get(), whole, uv);
        check(out != nullptr, "maps: the layer composites over the door's black frame");
        if (out) {
            const auto lp = readPixels(r, out);
            check(region(lp, kDoorW, 12, 12, 84, 60, kRed) && at(lp, kDoorW, 2, 2) == kBlack,
                  "maps: the layer holds the screen the game drew (its own texture, sharp) where the quad is, over black");
            out->Release();
        }
    }

    // The hand-back, and the route's re-issue is not a take.
    check(!boundaryFrame(r, true) && boundaryFrame(r, true), "maps: the second named frame in a row holds the world again");
    {
        g_stubs.mayTake = true;
        g_stubs.eyeDraws = 0;
        const uint64_t seq = nextArmed(r);
        g_stubs.eyeDraws = 1;   // the game's own composite lands in the eye; the re-issue is the layer's one counted draw: the counts would pass
        bindGame(r, 0);
        const Drawn d = drawScreen(r);
        check(!d.taken && d.reissued, "maps: with the route owning the world the screen composite is re-issued, not taken, as before the key");
        g_stubs.routeDoor = false;
        check(!uiLayerDoorLayerOnly(0, seq), "maps: a re-issue is not a take: the maps gate's door answers no for it");
        g_stubs.routeDoor = true;
        check(uiLayerDoorLayerOnly(0, seq), "maps: ... and the route's own answer comes first");
        g_stubs.routeDoor = false;
        g_stubs.mayTake = false;
    }

    // A second map opens; then the key goes off while the layer holds it: the gate is today's again at once.
    boundaryFrame(r, false);
    boundaryFrame(r, false);
    check(!boundaryFrame(r, false), "maps: (setup) the second map is released to the layer on its third unnamed frame");
    {
        g_stubs.mayTake = false;
        g_stubs.eyeDraws = 0;
        const uint64_t seq = nextArmed(r);
        g_stubs.eyeDraws = 1;
        check(takeScreen(r, 0) && uiLayerDoorLayerOnly(0, seq), "maps: (setup) a taken screen the door would run layer-only for");
        cfg.set("experimental.on_foot_maps_sharp", "off");
        uiLayerConfigure(cfg);
        check(boundaryFrame(r, false) && !uiLayerMapsOn(), "maps, key off live: the gate is the journal's again at once (on foot: held), the maps gate off");
        check(!uiLayerDoorLayerOnly(0, seq), "maps, key off live: ... and the door's predicate no longer answers for a mark made under the key");
    }

    // The key back on while a map is showing: carried from today's gate (held), released in the usual three.
    cfg.set("experimental.on_foot_maps_sharp", "on");
    uiLayerConfigure(cfg);
    check(boundaryFrame(r, false) && uiLayerMapsOn(), "maps, key on live: carried from today's gate: the world, though the screen names nothing");
    check(boundaryFrame(r, false) && !boundaryFrame(r, false), "maps: ... and released on the third unnamed frame after the switch");

    // The layer not live (fix.ui_quality off): the naming stops deciding, and says why.
    cfg.set("fix.ui_quality", "off");
    uiLayerConfigure(cfg);
    check(!uiLayerLive(), "maps: (setup) fix.ui_quality off: the layer is not live");
    boundaryFrame(r, false);
    check(!uiLayerMapsOn(), "maps, layer not live: the maps gate is off (a layer that does not run decides nothing)");
    cfg.set("fix.ui_quality", "100");
    uiLayerConfigure(cfg);
    check(uiLayerLive(), "maps: (setup) fix.ui_quality back: the layer is live");
    check(boundaryFrame(r, false) && uiLayerMapsOn(), "maps: the naming decides again, carried from today's gate (on foot: the world)");

    // Screen motion not live (fix.temporal_aa off): the gate is today's, with a line.
    detail::g_screenMotionEnabled = false;
    check(boundaryFrame(r, false) && !uiLayerMapsOn(), "maps, screen motion not live: the gate is today's (on foot: held), the maps gate off");
    boundaryFrame(r, false);
    detail::g_screenMotionEnabled = true;
    check(boundaryFrame(r, false) && uiLayerMapsOn(), "maps: screen motion back: the naming decides again");

    cfg.set("experimental.on_foot_maps_sharp", "off");
    uiLayerConfigure(cfg);
    boundaryFrame(r, false);
    check(!uiLayerMapsOn(), "maps: (cleanup) key off");

    // ---- 3. THE LINES, in the order the script made them.
    Log::get().close();
    const std::vector<std::string> lines = mapsLines(dir);
    struct Want { const char* start; const char* piece; };
    const Want want[] = {
        {"on foot maps sharp: ON at frame=", "the world (the journal: on foot)"},
        {"on foot maps sharp: the layer TAKES the 2D screen at frame=", "no world camera named its source for 3 frames in a row"},
        {"on foot maps sharp: the layer took the 2D screen for eye 0", "the eye keeps the upscaler"},
        {"on foot maps sharp: the layer HANDS BACK the 2D screen at frame=", "a world camera named the screen's source for 2 frames in a row"},
        {"on foot maps sharp: the layer TAKES the 2D screen at frame=", "no world camera named its source for 3 frames in a row"},
        {"on foot maps sharp: OFF at frame=", "(the key went off)"},
        {"on foot maps sharp: ON at frame=", "the world (the journal: on foot)"},
        {"on foot maps sharp: the layer TAKES the 2D screen at frame=", "no world camera named its source for 3 frames in a row"},
        {"on foot maps sharp: OFF at frame=", "the layer held a panel at that moment"},
        {"on foot maps sharp: experimental.on_foot_maps_sharp is on but", "fix.ui_quality is off"},
        {"on foot maps sharp: ON at frame=", "the world (the journal: on foot)"},
        {"on foot maps sharp: OFF at frame=", "(screen motion is not live)"},
        {"on foot maps sharp: experimental.on_foot_maps_sharp is on but", "screen motion is not live"},
        {"on foot maps sharp: ON at frame=", "the world (the journal: on foot)"},
        {"on foot maps sharp: OFF at frame=", "(the key went off)"},
    };
    bool orderOk = lines.size() == sizeof(want) / sizeof(want[0]);
    std::string first = "(none)";
    for (size_t i = 0; orderOk && i < lines.size(); ++i) {
        if (lines[i].compare(0, std::strlen(want[i].start), want[i].start) != 0 || lines[i].find(want[i].piece) == std::string::npos) {
            orderOk = false;
            first = lines[i];
        }
    }
    check(orderOk, "maps: the lines the script makes are ON, TAKES, not-empty, HANDS BACK, TAKES, OFF (key), ON, TAKES, OFF (layer), not-live (layer), ON, "
                   "OFF (screen motion), not-live (screen motion), ON, OFF (key), in that order and none before the first");
    if (!orderOk) {
        std::printf("      got %zu line(s), want %zu; the first that differs: %s\n", lines.size(), sizeof(want) / sizeof(want[0]), first.c_str());
        for (const auto& l : lines) std::printf("      | %s\n", l.c_str());
    }
    // Remove the rig's temp log.
    {
        WIN32_FIND_DATAW fd{};
        HANDLE h = FindFirstFileW((dir + L"\\*.log").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do { DeleteFileW((dir + L"\\" + fd.cFileName).c_str()); } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
        RemoveDirectoryW(dir.c_str());
    }
}

// ================================================================ what the 5 s window says across the transitions of the first flight
// The first flight of the maps gate (docs/design-world-camera-motion-2026-09-30.md, 8.10; Frontier, 2026-10-01 08:32:55.975) ended its
// second panel period in a boarding: on foot with the route owning the world, then the ship's cockpit. The naming stopped with the last
// 2D screen composite, three unnamed frames released the gate, the route let go on the same boundary (RELEASED on-foot-gate-lost), and
// from then on every window read panel frames, no screen taken, no eye through the layer-only door -- because a cockpit draws no 2D
// screen composite, which nothing in the line could say. The window line now carries screen-draws=: every 2D screen composite the
// layer's decision SAW while the gate was on, taken or not. This replays the flight's transition and the one it did not take, against the
// production layer, and reads the window lines back from its real log:
//   a map opened with the route owning the world (composites re-issued, then taken, both eyes layer-only), closed again (taken until the
//   second named frame holds the world), the route owning again; then the boarding: three unnamed frames, the gate and the route let go,
//   and no composite follows. The first window is every one of those frames and its numbers are checked against the rig's own count of
//   what it drew and what the layer did with it; the second is the cockpit alone -- the flight's windows: panel frames, nothing drawn,
//   nothing taken, no door.
// Two real 5 s windows, so the rig sleeps twice (the window's length is the layer's own constant).
static unsigned windowNumber(const std::string& line, const char* key) {
    const std::string needle = std::string(" ") + key + "=";
    const size_t at = line.find(needle);
    return at == std::string::npos ? ~0u : static_cast<unsigned>(std::strtoul(line.c_str() + at + needle.size(), nullptr, 10));
}

void testMapsTransitions(Rig& r) {
    auto& cfg = Config::get();
    wchar_t temp[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, temp);
    const std::wstring dir = std::wstring(temp) + L"edvr_ui_layer_world_maps_windows_" + std::to_wstring(GetCurrentProcessId());
    CreateDirectoryW(dir.c_str(), nullptr);
    check(Log::get().open(dir, L"uilwwin"), "windows: the rig's log opens in a temp directory (its window lines are read back at the end)");

    // On foot by the journal, the key on: the first boundary carries the world, held.
    g_stubs.journalKnown = g_stubs.journalOnFoot = true;
    g_stubs.depthKnown = false;
    g_stubs.depthDraws = 0;
    g_stubs.routeDoor = false;
    g_stubs.mayTake = false;
    cfg.set("experimental.on_foot_maps_sharp", "on");
    uiLayerConfigure(cfg);
    check(boundaryFrame(r, true) && uiLayerMapsOn(), "windows: (setup) the key on, on foot: the naming decides, carried from today's gate (the world)");

    // What the rig drew and what the layer did with it, window by window (zeroed after each window line). The setup boundary above was the
    // window's first frame, a world frame: the layer counts it, so the frame counts start at one.
    unsigned reissued = 0, taken = 0, wrong = 0, doorMissed = 0, doorWrong = 0, worldFrames = 1, panelFrames = 0;
    // A frame while the route owns the world: both eyes' composites are drawn and re-issued into the layer, never taken.
    auto worldFrame = [&](bool named) {
        nextArmed(r);
        g_stubs.eyeDraws = 2;
        for (int e = 0; e < 2; ++e) {
            bindGame(r, e);
            const Drawn d = drawScreen(r);
            if (d.reissued && !d.taken) ++reissued; else ++wrong;
        }
        const bool held = boundaryFrame(r, named);
        ++(held ? worldFrames : panelFrames);
        return held;
    };
    // A frame while the layer holds the panel (a map, a menu): both eyes' composites are taken and the door runs layer-only for both.
    auto mapFrame = [&](bool named) {
        const uint64_t seq = nextArmed(r);
        g_stubs.eyeDraws = 2;
        for (int e = 0; e < 2; ++e) {
            if (takeScreen(r, e)) ++taken; else ++wrong;
        }
        if (!(uiLayerDoorLayerOnly(0, seq) && uiLayerDoorLayerOnly(1, seq))) ++doorMissed;
        const bool held = boundaryFrame(r, named);
        ++(held ? worldFrames : panelFrames);
        return held;
    };
    // A frame in a cockpit: no 2D screen composite is drawn at all; the door is asked and has nothing to run for.
    auto cockpitFrame = [&]() {
        const uint64_t seq = nextArmed(r);
        g_stubs.eyeDraws = 0;
        if (uiLayerDoorLayerOnly(0, seq) || uiLayerDoorLayerOnly(1, seq)) ++doorWrong;
        const bool held = boundaryFrame(r, false);
        ++(held ? worldFrames : panelFrames);
        return held;
    };

    // The world, the route owning it; then a map opens (the naming stops, the third unnamed frame releases the gate and the route
    // lets go with it) and the layer takes every composite; then the map closes (two named frames hold the world; the composites
    // drawn in them are still the panel's and are taken) and the route owns the world again.
    g_stubs.mayTake = true;
    for (int i = 0; i < 10; ++i) worldFrame(true);
    bool held = true;
    for (int i = 0; i < 3; ++i) held = worldFrame(false);
    check(!held, "windows: (setup) three unnamed frames release the gate, as a map's opening does");
    g_stubs.mayTake = false;
    for (int i = 0; i < 12; ++i) mapFrame(false);
    mapFrame(true);
    held = mapFrame(true);
    check(held, "windows: (setup) two named frames hold the world again");
    g_stubs.mayTake = true;
    for (int i = 0; i < 3; ++i) worldFrame(true);
    // The boarding: the composites stop, three unnamed frames release the gate, the route lets go on the same boundary; the cockpit follows.
    for (int i = 0; i < 3; ++i) held = cockpitFrame();
    check(!held, "windows: (setup) the boarding: three frames with no composite and no naming release the gate");
    g_stubs.mayTake = false;
    for (int i = 0; i < 5; ++i) cockpitFrame();
    Sleep(5100);                                     // the window is 5 s of the wall clock
    cockpitFrame();                                  // this boundary writes the first window's line
    const unsigned w1Reissued = reissued, w1Taken = taken, w1Frames = worldFrames + panelFrames, w1World = worldFrames, w1Panel = panelFrames;
    const unsigned w1Wrong = wrong, w1DoorMissed = doorMissed, w1DoorWrong = doorWrong;
    reissued = taken = wrong = doorMissed = doorWrong = worldFrames = panelFrames = 0;

    // The cockpit alone, a whole window of it: the flight's windows after the boarding.
    for (int i = 0; i < 30; ++i) cockpitFrame();
    Sleep(5100);
    cockpitFrame();                                  // the second window's line
    const unsigned w2Frames = worldFrames + panelFrames, w2World = worldFrames, w2Panel = panelFrames, w2Wrong = wrong, w2DoorWrong = doorWrong;

    cfg.set("experimental.on_foot_maps_sharp", "off");
    uiLayerConfigure(cfg);
    boundaryFrame(r, false);
    check(!uiLayerMapsOn(), "windows: (cleanup) key off");
    g_stubs.mayTake = false;
    g_stubs.eyeDraws = 0;

    check(w1Wrong == 0 && w1DoorMissed == 0 && w1DoorWrong == 0 && w2Wrong == 0 && w2DoorWrong == 0,
          "windows: (setup) every composite the rig drew went the way the scenario says (re-issued while the route owns, taken while the layer holds the panel), the door ran layer-only for every taken eye and for none in a cockpit");
    check(w1Reissued > 0 && w1Taken > 0 && w1World > 0 && w1Panel > 0,
          "windows: (setup) the first window holds both kinds of composite and both gates");

    Log::get().close();
    const std::vector<std::string> lines = mapsLines(dir);
    std::vector<std::string> win;
    for (const auto& l : lines)
        if (l.rfind("on foot maps sharp 5s:", 0) == 0) win.push_back(l);
    check(win.size() == 2, "windows: the real layer wrote one line for each of the two 5 s windows");
    if (win.size() == 2) {
        const std::string& a = win[0];
        const std::string& b = win[1];
        // The first window: every composite counted once, taken or not; only the layer's own takes are takes; the door only for those.
        check(windowNumber(a, "screen-draws") == w1Reissued + w1Taken,
              "windows: the first window's screen-draws is every composite the decision saw (re-issued ones and taken ones): the layer counts what it was asked, not what it took");
        check(windowNumber(a, "screen-takes") == w1Taken, "windows: ... its screen-takes is only the taken ones (a re-issue is not a take)");
        check(windowNumber(a, "door-layer-only") == w1Taken, "windows: ... its door-layer-only is the eyes of the taken ones");
        check(windowNumber(a, "door-not-empty") == 0, "windows: ... no eye kept the upscaler");
        check(windowNumber(a, "frames") == w1Frames && windowNumber(a, "world-frames") == w1World && windowNumber(a, "panel-frames") == w1Panel,
              "windows: ... it counts the frames of the run and which gate each ended in");
        check(windowNumber(a, "holds") == 1 && windowNumber(a, "releases") == 2, "windows: ... one hold (the map closing) and two releases (the map opening, the boarding)");
        // The second window: the flight's own shape.
        check(windowNumber(b, "world-frames") == 0 && windowNumber(b, "panel-frames") == w2Panel && w2World == 0 && w2Frames == w2Panel,
              "windows: the cockpit window is whole-panel: panel frames and no world frame");
        check(windowNumber(b, "screen-takes") == 0 && windowNumber(b, "door-layer-only") == 0 && windowNumber(b, "door-not-empty") == 0,
              "windows: ... nothing taken, no eye through the layer-only door (what the first flight's windows said)");
        check(windowNumber(b, "screen-draws") == 0,
              "windows: ... and screen-draws says why: no 2D screen composite was drawn, so there was nothing to take");
        check(b.size() > 15 && b.compare(b.size() - 15, 15, " screen-draws=0") == 0 && a.find(" screen-draws=") != std::string::npos &&
                  a.find(" screen-draws=") > a.find(" not-live-frames="),
              "windows: screen-draws is the line's last token, after not-live-frames (the reader's pattern ends with it)");
    } else {
        for (const auto& l : lines) std::printf("      | %s\n", l.c_str());
    }
    {   // Remove the rig's temp log.
        WIN32_FIND_DATAW fd{};
        HANDLE h = FindFirstFileW((dir + L"\\*.log").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do { DeleteFileW((dir + L"\\" + fd.cFileName).c_str()); } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
        RemoveDirectoryW(dir.c_str());
    }
}

// --bench (not part of the gate): what the on-foot maps gate costs the CPU, measured against the real boundary and the real door
// predicate. The boundary's own work (the journal and depth steps, the layer's warm compile, the window bookkeeping) is the same in
// every column, so the difference between the columns is the feature's.
void benchMaps(Rig& r) {
    auto& cfg = Config::get();
    LARGE_INTEGER freq, a, b;
    QueryPerformanceFrequency(&freq);
    g_stubs.journalKnown = g_stubs.journalOnFoot = true;
    g_stubs.depthKnown = false;
    const uint32_t n = 200000;
    auto perCall = [&](const char* what, auto&& body) {
        for (uint32_t i = 0; i < 2000; ++i) body(i);   // warm
        QueryPerformanceCounter(&a);
        for (uint32_t i = 0; i < n; ++i) body(i);
        QueryPerformanceCounter(&b);
        const double ns = 1e9 * static_cast<double>(b.QuadPart - a.QuadPart) / static_cast<double>(freq.QuadPart) / n;
        std::printf("  %-64s %8.1f ns\n", what, ns);
        return ns;
    };
    std::puts("ui_layer_world_test --bench: CPU of the on-foot maps gate, per call");
    cfg.set("experimental.on_foot_maps_sharp", "off");
    uiLayerConfigure(cfg);
    perCall("frame boundary, key off, first run (cold caches)", [&](uint32_t) { uiLayerFrameBoundary(r.ctx.Get()); });
    const double off = perCall("frame boundary, key off (the journal and depth gate as today)", [&](uint32_t) { uiLayerFrameBoundary(r.ctx.Get()); });
    cfg.set("experimental.on_foot_maps_sharp", "on");
    uiLayerConfigure(cfg);
    const double named = perCall("frame boundary, key on, a world camera named the source", [&](uint32_t) {
        uiLayerNoteScreenNamed();
        uiLayerFrameBoundary(r.ctx.Get());
    });
    const double unnamed = perCall("frame boundary, key on, nothing named (a map)", [&](uint32_t) { uiLayerFrameBoundary(r.ctx.Get()); });
    const double told = perCall("uiLayerNoteScreenNamed alone (screen_motion.cpp, once per named frame)", [&](uint32_t) { uiLayerNoteScreenNamed(); });
    // The door's question, key on after a take: the route's answer first, then the mark and the counts.
    g_stubs.eyeDraws = 0;
    const uint64_t seq = nextArmed(r);
    g_stubs.eyeDraws = 1;
    takeScreen(r, 0);
    volatile bool sink = false;
    const double doorOn = perCall("door predicate, key on, a take in this sequence", [&](uint32_t) { sink = sink | uiLayerDoorLayerOnly(0, seq); });
    const double doorMiss = perCall("door predicate, key on, no take for the eye", [&](uint32_t) { sink = sink | uiLayerDoorLayerOnly(1, seq); });
    cfg.set("experimental.on_foot_maps_sharp", "off");
    uiLayerConfigure(cfg);
    uiLayerFrameBoundary(r.ctx.Get());
    const double doorOff = perCall("door predicate, key off (the route's question and one load)", [&](uint32_t) { sink = sink | uiLayerDoorLayerOnly(0, seq); });
    std::printf("the feature's cost a frame: boundary +%.1f ns named / +%.1f ns unnamed, the naming told +%.1f ns, four door questions %.1f ns "
                "(a take) / %.1f ns (key off)\n",
                named - off, unnamed - off, told, 4 * doorOn, 4 * doorOff);
    (void)doorMiss;
}

int main(int argc, char** argv) {
    if (argc == 2 && std::strcmp(argv[1], "--dry-run") == 0) {
        std::puts("ui_layer_world_test: dry-run (no device, no files)");
        return 0;
    }
    if (argc == 2 && std::strcmp(argv[1], "--bench") == 0) {
        Rig r;
        if (!setup(r, false)) {
            std::puts("FAIL: a device, the textures and the test shaders");
            return 1;
        }
        auto& cfg = Config::get();
        cfg.set("fix.ui_quality", "100");
        cfg.set("fix.temporal_aa", "dlss");
        cfg.set("advanced.temporal_aa_jitter_sign", "as_is");
        cfg.set("advanced.temporal_aa_jitter_lag", "0");
        uiLayerConfigure(cfg);
        uiLayerFrameBoundary(r.ctx.Get());
        r.seq = 1;
        benchMaps(r);
        uiLayerShutdown();
        return 0;
    }
    // --hardware: the same checks on the default hardware adapter instead of WARP, by hand (the gate runs --self-test; a build
    // machine may have no GPU).
    const bool hardware = argc == 2 && std::strcmp(argv[1], "--hardware") == 0;
    if (argc != 2 || (std::strcmp(argv[1], "--self-test") != 0 && !hardware)) {
        std::puts("usage: ui_layer_world_test --self-test | --hardware | --dry-run");
        return 2;
    }
    Rig r;
    if (!setup(r, hardware)) {
        std::puts("FAIL: a device, the textures and the test shaders");
        return 1;
    }
    if (hardware) {   // name the adapter the hardware run used, so a pass is attributable
        ComPtr<IDXGIDevice> dxgi;
        ComPtr<IDXGIAdapter> adapter;
        DXGI_ADAPTER_DESC desc{};
        if (SUCCEEDED(r.dev.As(&dxgi)) && SUCCEEDED(dxgi->GetAdapter(&adapter)) && SUCCEEDED(adapter->GetDesc(&desc)))
            std::printf("[ui_layer_world_test] hardware adapter: %ls\n", desc.Description);
    }
    auto& cfg = Config::get();
    cfg.set("fix.ui_quality", "100");
    cfg.set("fix.temporal_aa", "dlss");
    cfg.set("advanced.temporal_aa_jitter_sign", "as_is");
    cfg.set("advanced.temporal_aa_jitter_lag", "0");
    // The on-foot maps gate ships ON since 2026-10-01 (an ini with no line reads on: tools\config_test and tools\on_foot_maps_test
    // hold the fallback). The cases before the maps section pin the world-screen gate as the journal and the screen's own depth
    // give it, which is the maps key off, and the maps section sets the key itself, so the rig starts with it off: the new default
    // must not leak in and make the first maps case start from a gate that is already on.
    cfg.set("experimental.on_foot_maps_sharp", "off");
    uiLayerConfigure(cfg);
    check(uiLayerLive(), "the layer is live for the rig (fix.ui_quality 100, dlss, jitter as shipped)");
    // The first boundary computes the world-screen gate (the journal: on foot) and warms the layer's shaders.
    uiLayerFrameBoundary(r.ctx.Get());
    check(uiLayerWorldScreenHeld(), "the gate holds the screen after the first boundary");
    // The door's first frame, so sequence 2 is armed.
    r.seq = 1;
    testKeyOff(r);
    testReissue(r);
    testJitterCancel(r);
    testRefusals(r);
    testCurvedScreen(r);
    testBeginWithoutOuterScope(r);
    testLostDraws(r);
    testDoorGaps(r);
    test125(r);
    testAccessors(r);
    testMapsGate(r);
    testMapsTransitions(r);
    uiLayerShutdown();
    std::printf("ui_layer_world_test: %u checks, %u failures\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
