// The VR on-foot world route's runtime on WARP (docs/design-flat-temporal-aa-2026-09-23.md, section 82).
//
// vr_world_route.cpp is linked AS SHIPPED, with the real binding shadow, the real Config and the real flat resolver (its
// backends stubbed the way tools\flat_mono_resolve_test stubs them: they see the textures the SDK would, and their image
// quality is separate). The world is drawn at 128x72 instead of 5040x2835: a synthetic copy of the census retake's chain
// (design doc section 82), draw by draw, through the same per-draw entry the hooks call. What is pinned:
//   - KEY OFF: nothing runs, nothing is logged, nothing is held;
//   - the happy path: the resolver is called once at the tone, on slot 2, on H, with the route's flags; warm-up, ownership,
//     and the published state the eye shift and the layer read;
//   - every refusal the selector names: no resolver call, the eye route serves the frame, the reason in the log;
//   - a CURVED screen (fix.panel_curvature above 0) does not hold the route off: it resolves, owns and may take frames as a flat screen
//     does, its 5 s line names the curve (pending, stood-down, or curvature/columns/gain) and each window's strips (a delta of
//     panel_curve's cumulative count), and its OWNS line says so; with the key off the curve changes nothing; with curvature 0 the
//     lines are what they were (curve=off);
//   - the route's own calls pass the hooks' internal flags, and leave the game's pipeline state as they found it;
//   - late writes into H latch the route off; a scene reset releases and resets; a frame gap resets the history;
//   - the key going off while owned lets go of everything;
//   - STAGE 2 (the world jitter), against a MODEL of the camera injector (the real one hooks the game's process; the model
//     keeps its contract: a window the route opens once a frame, closes at the trigger, counters the route reads): the
//     window opens only while the route is Warming or Owned AND the last frame named the screen's source (so a map frame's
//     refreshes inject nothing, through the grace frames too), the phase reaches the resolver only when a scene call took
//     it, the global jitter key (experimental.temporal_aa_jitter; the route has no jitter key of its own) zeroes it, a hook that
//     is not live leaves the world unjittered, a STOP fires on an injection on a frame the route shut and on an injected kind
//     other than 3, and the key off leaves the injector alone;
//   - the depth-checked steady detail is always on: the resolver's depth check counts the route's resolves with no key set.
// --self-test runs it; --dry-run says what it would do and writes nothing.

#include "../../src/d3d11/vr_world_route.h"
#include "../../src/d3d11/vr_world_route_math.h"
#include "../../src/d3d11/binding_shadow.h"
#include "../../src/d3d11/flat_mono_resolve.h"
#include "../../src/d3d11/dlaa.h"
#include "../../src/d3d11/fsr3_engine.h"
#include "../../src/d3d11/engine_velocity.h"
#include "../../src/d3d11/flat_camera_inject.h"
#include "../../src/d3d11/gpu_census.h"
#include "../../src/d3d11/panel_curve.h"
#include "../../src/d3d11/ui_layer.h"
#include "../../src/d3d11/vr_camera_census.h"
#include "../../src/d3d11/vr_world_mips.h"
#include "../../src/d3d11/vscreen.h"
#include "../../src/d3d11/weapon_motion.h"
#include "../../src/common/config.h"
#include "../../src/common/log.h"
#include "../../src/common/runtime_profile.h"
#include "../../src/common/system_d3d11.h"
#include <d3d11_1.h>
#include <wrl/client.h>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
using Microsoft::WRL::ComPtr;

namespace {
int g_failures = 0, g_checks = 0;
void check(bool ok, const char* what) {
    ++g_checks;
    if (ok) { std::printf("  ok    %s\n", what); return; }
    ++g_failures;
    std::printf("  FAIL  %s\n", what);
}

constexpr uint32_t kW = 128, kH = 72;
// The stub world the route's neighbours answer from.
bool g_gate = true, g_layerLive = true, g_named = true, g_rowsKnown = true, g_viewsReady = true;
bool g_realMismatch = false;   // the real context's t1 is not what the shadow says (the route verifies the actual binding)
uint32_t g_panelW = kW, g_panelH = kH;
const ID3D11Texture2D* g_namedDepth = nullptr;
float g_rows[6][4];
int g_mipsResets = 0;
std::vector<std::string> g_log;
// What the stub backend saw.
int g_backendCalls = 0;
struct BackendCall { int slot; bool reset, hdr, internalFlags; uint32_t w, h, outW, outH; DXGI_FORMAT colour; float jx, jy; };
std::vector<BackendCall> g_calls;
ComPtr<ID3D11ShaderResourceView> g_slotsSrv, g_poolSrv, g_weaponMapSrv;   // the weapon map the stub weapon_motion answers with (null: no weapon drew)
ComPtr<ID3D11Buffer> g_sceneNow, g_scenePrev;

// Real rows (Epic frame 71751, b1[270..275]): the shape the game composes for a kind-3 camera (flat_mono_resolve_test).
constexpr float kEpicRows[6][4] = {
    {.674860716f, -.714774430f, 0, .684166729f},
    {-.804393589f, -.069881566f, 0, .664024174f},
    {-.240084499f, -1.77504551f, 0, -.301641792f},
    {0, 0, .0250000004f, 0},
    {.684166729f, .664024174f, -.301641792f, 0},
    {-21.0930309f, -24.5114784f, -1.11009693f, 0}};
}  // namespace

// ---- the camera injector, MODELLED -------------------------------------------------------------------------------------------
// flat_camera_inject.cpp is not linked: it hooks the game's process. This is its contract as the route sees it (the VR section
// of flat_camera_inject.h): the route steps it once a frame (flatCameraVrFrame), closes its window at the trigger, reads its
// counters; the scenarios play the game's camera refreshes against it (gameWorldRefreshes / gameEyeRefreshes) and can make it
// misbehave, to see what the route does about it.
struct InjectorModel {
    int frameCalls = 0, injectCalls = 0, closeCalls = 0;
    std::vector<edvr::FlatCameraVrFrame> frames;   // every flatCameraVrFrame call, in order
    std::vector<int> order;                  // 1 = flatCameraVrFrame, 2 = flatCameraVrCloseWindow, 3 = the resolver's backend call
    bool hookLive = true;
    bool misbehave = false;                  // writes cameras on a frame the route shut (a broken injector)
    bool refuseOne = false;                  // the next scene call fails to write
    bool wrongKind = false;                  // injects one kind-5 call
    bool injecting = false, windowOpen = false, pendingFlush = false;
    bool gateOpen = false;                   // the relay stays in the game's call path: open while injecting or until the flush, closed by the next step
    float phaseX = 0.0f, phaseY = 0.0f;
    edvr::FlatCameraVrCounters c;
    std::vector<edvr::FlatCameraVrCounters> ended;   // the counters of each frame that ended, pushed when the next step resets them
    std::vector<edvr::FlatCameraVrExcluded> excluded;
};
InjectorModel g_inj;
// The counters of the frame that ended last (zeros when none did).
edvr::FlatCameraVrCounters lastEnded() { return g_inj.ended.empty() ? edvr::FlatCameraVrCounters{} : g_inj.ended.back(); }
// What the game does each frame: kind-3 scene calls and (with a weapon drawn) first-person calls before the tone, auxiliary
// kind-3 calls, shadow cameras, and the kind-5 eye cameras after it.
struct GameCameras { int scene = 48, firstPerson = 0, aux = 0, shadow = 10, eyes = 6; };
GameCameras g_game;
void gameWorldRefreshes() {
    edvr::FlatCameraVrCounters& c = g_inj.c;
    const bool nonzero = g_inj.phaseX != 0.0f || g_inj.phaseY != 0.0f;
    const auto screenCall = [&](bool firstPerson) {
        ++c.calls;
        if (g_inj.misbehave) {   // a broken injector: writes whatever the route asked
            (firstPerson ? c.firstPersonInjected : c.sceneInjected)++;
            ++c.injectedKind[3];
            g_inj.pendingFlush = true;
            return;
        }
        if (g_inj.injecting && g_inj.windowOpen) {
            if (!nonzero) { ++c.warming; return; }                   // a zero phase admits and writes nothing
            if (g_inj.refuseOne && !firstPerson) { g_inj.refuseOne = false; ++c.sceneRefused; return; }
            (firstPerson ? c.firstPersonInjected : c.sceneInjected)++;
            ++c.injectedKind[3];
            g_inj.pendingFlush = true;
            return;
        }
        if (g_inj.injecting) { ++c.afterTrigger; return; }           // the window is closed
        ++c.stale;                                                   // pass-through: the first call restores what was written
        g_inj.pendingFlush = false;
    };
    for (int i = 0; i < g_game.scene; ++i) screenCall(false);
    for (int i = 0; i < g_game.firstPerson; ++i) screenCall(true);
    c.calls += static_cast<uint32_t>(g_game.aux + g_game.shadow);
    c.auxiliary += static_cast<uint32_t>(g_game.aux);
    c.otherKind += static_cast<uint32_t>(g_game.shadow);
}
void gameEyeRefreshes() {
    g_inj.c.calls += static_cast<uint32_t>(g_game.eyes);
    g_inj.c.unsupported += static_cast<uint32_t>(g_game.eyes);
    if (g_inj.wrongKind) { ++g_inj.c.injectedKind[5]; g_inj.wrongKind = false; }
}

namespace edvr {
bool flatCameraVrFrame(const FlatCameraVrFrame& f) {
    ++g_inj.frameCalls;
    if (f.inject) ++g_inj.injectCalls;
    g_inj.frames.push_back(f);
    g_inj.order.push_back(1);
    g_inj.ended.push_back(g_inj.c);
    g_inj.c = FlatCameraVrCounters{};
    g_inj.injecting = f.inject && g_inj.hookLive;
    g_inj.windowOpen = g_inj.injecting;
    g_inj.gateOpen = g_inj.injecting || g_inj.pendingFlush;
    g_inj.phaseX = f.phaseX; g_inj.phaseY = f.phaseY;
    return g_inj.hookLive;
}
void flatCameraVrCloseWindow() { ++g_inj.closeCalls; g_inj.order.push_back(2); g_inj.windowOpen = false; }
FlatCameraVrCounters flatCameraVrCounters() { return g_inj.c; }
size_t flatCameraVrExcluded(FlatCameraVrExcluded* out, size_t max) {
    size_t n = 0;
    for (; n < g_inj.excluded.size() && n < max; ++n) out[n] = g_inj.excluded[n];
    return n;
}
const char* flatCameraVrStatus() { return g_inj.hookLive ? "installed" : "failed"; }
bool flatCameraVrQuiet() { return !g_inj.gateOpen; }
}  // namespace edvr

namespace edvr {
// panel_curve.h's state (panelCurveWants and panelCurveInfo are inline over these): a scenario sets the curvature the way the config
// would, and the strip's readiness, gain and re-issue count the way panel_curve.cpp would. panel_curve.cpp is not linked.
namespace detail {
bool g_panelCurveStoodDown = false;
float g_panelCurveCurvature = 0.0f;
int g_panelCurveSegments = kDefaultSegments;
float g_panelCurveGain = 0.0f;
bool g_panelCurveReady = false;
uint64_t g_panelCurveReissues = 0;   // cumulative, as in the DLL: it only grows, and the route prints each window's delta of it
}  // namespace detail
thread_local bool g_flatComputeInternal = false;
Log& Log::get() { static Log instance; return instance; }
Log::~Log() = default;
void Log::note(const char* fmt, ...) {
    char line[1536]{};
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);
    g_log.emplace_back(line);
}
bool vScreenPanelSize(uint32_t* w, uint32_t* h) { if (w) *w = g_panelW; if (h) *h = g_panelH; return true; }
bool uiLayerWorldScreenHeld() { return g_gate; }
bool uiLayerLiveForWorldRoute() { return g_layerLive; }
const char* uiLayerNotLiveReason() { return g_layerLive ? nullptr : "fix.ui_quality is 0, so the UI layer is off"; }
bool vrCameraCensusWanted() { return false; }
void vrCameraCensusFrameBoundary() {}
void vrCameraCensusEyeDraw(ID3D11DeviceContext*, uint32_t) {}
ID3D11ShaderResourceView* vrWorldMipsScreen(ID3D11DeviceContext*, ID3D11Texture2D*, uint64_t) { return nullptr; }
ID3D11SamplerState* vrWorldMipsSampler(ID3D11Device*, const D3D11_SAMPLER_DESC&) { return nullptr; }
void vrWorldMipsReset() { ++g_mipsResets; }
bool engineVelocitySourceIsNamed(const ID3D11Texture2D* depth) { return g_named && depth && depth == g_namedDepth; }
bool engineVelocitySourceCameraRows(float (&rows)[6][4]) {
    if (!g_rowsKnown) return false;
    std::memcpy(rows, g_rows, sizeof(rows));
    return true;
}
bool engineVelocitySourceViews(ID3D11Texture2D* depth, EngineVelocityViews* out) {
    if (out) *out = EngineVelocityViews{};
    if (!g_viewsReady || !out || depth != g_namedDepth) return false;
    out->slots = g_slotsSrv.Get(); out->pool = g_poolSrv.Get(); out->sceneNow = g_sceneNow.Get(); out->scenePrev = g_scenePrev.Get();
    out->slots->AddRef(); out->pool->AddRef(); out->sceneNow->AddRef(); out->scenePrev->AddRef();
    return true;
}
ID3D11ShaderResourceView* weaponMotionView() { return g_weaponMapSrv.Get(); }
bool gpuCensusBegin(ID3D11DeviceContext*, GpuCensusSection) noexcept { return false; }
void gpuCensusEnd(ID3D11DeviceContext*, GpuCensusSection) noexcept {}
bool dlaaAvailable(ID3D11Device*, const char**) { return true; }
bool fsr3Available(ID3D11Device*, const char**) { return true; }
bool dlaaEvaluate(ID3D11DeviceContext* c, int slot, ID3D11Texture2D* colour, ID3D11Texture2D*, ID3D11Texture2D*, ID3D11Texture2D* out,
                  ID3D11Texture2D*, uint32_t w, uint32_t h, uint32_t outW, uint32_t outH, float jx, float jy, bool reset, float,
                  const char**, bool hdr) {
    ++g_backendCalls;
    g_inj.order.push_back(3);
    BackendCall b{};
    b.slot = slot; b.reset = reset; b.hdr = hdr; b.w = w; b.h = h; b.outW = outW; b.outH = outH; b.jx = jx; b.jy = jy;
    b.internalFlags = g_vrWorldInternal && g_flatComputeInternal;
    D3D11_TEXTURE2D_DESC d{};
    colour->GetDesc(&d);
    b.colour = d.Format;
    g_calls.push_back(b);
    c->ClearState();   // an SDK may alter every stage: the resolver's isolation must contain it
    ComPtr<ID3D11Device> dev;
    c->GetDevice(dev.GetAddressOf());
    ComPtr<ID3D11UnorderedAccessView> uav;
    if (FAILED(dev->CreateUnorderedAccessView(out, nullptr, uav.GetAddressOf()))) return false;
    const float green[4] = {0, 1, 0, 1};
    c->ClearUnorderedAccessViewFloat(uav.Get(), green);
    return true;
}
bool fsr3Evaluate(ID3D11DeviceContext*, unsigned, ID3D11Texture2D*, ID3D11Texture2D*, ID3D11Texture2D*, ID3D11Texture2D*,
                  ID3D11Texture2D*, uint32_t, uint32_t, uint32_t, uint32_t, float, float, bool, float, float, float, float,
                  const char**, bool, bool) { return false; }
}  // namespace edvr

namespace {
using namespace edvr;

ComPtr<ID3D11Texture2D> makeTexture(ID3D11Device* d, UINT w, UINT h, DXGI_FORMAT fmt, UINT binds) {
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = w; desc.Height = h; desc.ArraySize = desc.MipLevels = 1; desc.SampleDesc.Count = 1;
    desc.Format = fmt; desc.BindFlags = binds; desc.Usage = D3D11_USAGE_DEFAULT;
    ComPtr<ID3D11Texture2D> t;
    check(SUCCEEDED(d->CreateTexture2D(&desc, nullptr, t.GetAddressOf())), "fixture texture");
    return t;
}
ComPtr<ID3D11RenderTargetView> rtvOf(ID3D11Device* d, ID3D11Texture2D* t, DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN) {
    D3D11_RENDER_TARGET_VIEW_DESC v{};
    v.Format = fmt; v.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    ComPtr<ID3D11RenderTargetView> r;
    check(SUCCEEDED(d->CreateRenderTargetView(t, fmt == DXGI_FORMAT_UNKNOWN ? nullptr : &v, r.GetAddressOf())), "fixture RTV");
    return r;
}
ComPtr<ID3D11ShaderResourceView> srvOf(ID3D11Device* d, ID3D11Resource* t) {
    ComPtr<ID3D11ShaderResourceView> s;
    check(SUCCEEDED(d->CreateShaderResourceView(t, nullptr, s.GetAddressOf())), "fixture SRV");
    return s;
}

struct World {
    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11DeviceContext> ctx;
    ComPtr<ID3D11Texture2D> h, depth, depth2, g2, tone, copy, tiny, other, weapon, weaponSmall;
    ComPtr<ID3D11RenderTargetView> rh, rg2, rtone, rsmall;
    ComPtr<ID3D11DepthStencilView> dsv, dsv2;
    ComPtr<ID3D11ShaderResourceView> sh, scopy, sother, sweapon, sweaponSmall;
    uint64_t seq = 0;
};

bool build(World& w) {
    const auto createDevice = systemD3D11CreateDevice();
    check(createDevice != nullptr, "system D3D11 factory");
    if (!createDevice) return false;
    D3D_FEATURE_LEVEL level{};
    HRESULT hr = createDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, w.dev.GetAddressOf(), &level, w.ctx.GetAddressOf());
    check(SUCCEEDED(hr), "WARP device");
    if (FAILED(hr)) return false;
    ID3D11Device* d = w.dev.Get();
    w.h = makeTexture(d, kW, kH, DXGI_FORMAT_R11G11B10_FLOAT, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
    w.copy = makeTexture(d, kW, kH, DXGI_FORMAT_R11G11B10_FLOAT, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
    w.g2 = makeTexture(d, kW, kH, DXGI_FORMAT_R10G10B10A2_UNORM, D3D11_BIND_RENDER_TARGET);
    w.tone = makeTexture(d, kW, kH, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
    w.other = makeTexture(d, kW, kH, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_SHADER_RESOURCE);
    w.tiny = makeTexture(d, kW / 8, kH / 8, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_RENDER_TARGET);
    w.depth = makeTexture(d, kW, kH, DXGI_FORMAT_R32G8X24_TYPELESS, D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE);
    w.depth2 = makeTexture(d, kW, kH, DXGI_FORMAT_R32G8X24_TYPELESS, D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE);
    D3D11_DEPTH_STENCIL_VIEW_DESC dd{};
    dd.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT; dd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
    check(SUCCEEDED(d->CreateDepthStencilView(w.depth.Get(), &dd, w.dsv.GetAddressOf())) &&
              SUCCEEDED(d->CreateDepthStencilView(w.depth2.Get(), &dd, w.dsv2.GetAddressOf())), "fixture DSVs");
    w.rh = rtvOf(d, w.h.Get()); w.rg2 = rtvOf(d, w.g2.Get()); w.rtone = rtvOf(d, w.tone.Get()); w.rsmall = rtvOf(d, w.tiny.Get());
    w.sh = srvOf(d, w.h.Get()); w.scopy = srvOf(d, w.copy.Get()); w.sother = srvOf(d, w.other.Get());
    w.weapon = makeTexture(d, kW, kH, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D11_BIND_SHADER_RESOURCE);
    w.weaponSmall = makeTexture(d, kW / 2, kH / 2, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D11_BIND_SHADER_RESOURCE);
    w.sweapon = srvOf(d, w.weapon.Get()); w.sweaponSmall = srvOf(d, w.weaponSmall.Get());
    const float gray[4] = {0.25f, 0.25f, 0.25f, 1.0f};
    w.ctx->ClearRenderTargetView(w.rh.Get(), gray);
    // The engine's source data the resolver's prep reads: slots (R32G32), the pool (structured, stride 336), two scene buffers.
    ComPtr<ID3D11Texture2D> slots = makeTexture(d, kW, kH, DXGI_FORMAT_R32G32_FLOAT, D3D11_BIND_SHADER_RESOURCE);
    g_slotsSrv = srvOf(d, slots.Get());
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = 336 * 4; bd.BindFlags = D3D11_BIND_SHADER_RESOURCE; bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED; bd.StructureByteStride = 336;
    ComPtr<ID3D11Buffer> pool;
    check(SUCCEEDED(d->CreateBuffer(&bd, nullptr, pool.GetAddressOf())), "fixture pool");
    g_poolSrv = srvOf(d, pool.Get());
    bd = {};
    bd.ByteWidth = 277 * 16; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    check(SUCCEEDED(d->CreateBuffer(&bd, nullptr, g_sceneNow.GetAddressOf())) && SUCCEEDED(d->CreateBuffer(&bd, nullptr, g_scenePrev.GetAddressOf())), "fixture scene buffers");
    g_namedDepth = w.depth.Get();
    std::memcpy(g_rows, kEpicRows, sizeof(g_rows));
    return true;
}

// One game draw, as the VR thunks present it to the route: the binding shadow as the hooks left it, then the route's entry.
void draw(World& w, ID3D11RenderTargetView* rtv, ID3D11DepthStencilView* dsv, std::initializer_list<ID3D11ShaderResourceView*> srvs = {},
          uint64_t vs = 0x1111, uint64_t ps = 0x2222) {
    bindingSet(BindSlot::Rtv0, rtv);
    bindingSet(BindSlot::Dsv0, dsv);
    ID3D11ShaderResourceView* bound[4] = {};
    size_t n = 0;
    for (auto* s : srvs) if (n < 4) bound[n++] = s;
    for (uint32_t i = 0; i < 4; ++i) bindingSet(static_cast<BindSlot>(static_cast<uint32_t>(BindSlot::PsSrv0) + i), bound[i]);
    bindingSetShader(BindSlot::Vs, reinterpret_cast<void*>(uintptr_t(0x10)), vs);
    bindingSetShader(BindSlot::Ps, reinterpret_cast<void*>(uintptr_t(0x20)), ps);
    ID3D11ShaderResourceView* real[4] = {bound[0], bound[1], bound[2], bound[3]};
    if (g_realMismatch && real[1]) real[1] = w.sother.Get();
    w.ctx->PSSetShaderResources(0, 4, real);   // the real context, for the route's own PSGetShaderResources verification
    ID3D11RenderTargetView* r = rtv;
    w.ctx->OMSetRenderTargets(1, &r, dsv);
    ++w.seq;
    if (g_vrWorldWants) vrWorldRouteDraw(w.ctx.Get());
}

struct FrameOpts {
    bool lateWrite = false;       // something writes H after the tone (a copy into it)
    bool secondDepth = false;     // H is drawn with two different depth targets
    bool noTone = false;          // the frame has H but no consumer
    int gbufferDraws = 4;         // draws into one target pair before H: the run the route skips at two compares
    bool holdBoundary = false;    // stop after the eye refreshes: the frame's flags are still up and the boundary is the caller's
};
// The retake chain at tiny scale (design doc section 82): the world into the G-buffer, H's draws (with the screen depth), the
// exposure reduction on a COPY of H, late draws, the 630x354-style tiny draws reading H, the tone, then what follows it.
void frame(World& w, const FrameOpts& o = {}) {
    g_game.firstPerson = g_weaponMapSrv ? 6 : 0;   // a weapon drawn: its own first-person camera refreshes too
    gameWorldRefreshes();   // the world's camera calls come ahead of the draws they serve, and all before the tone
    for (int i = 0; i < o.gbufferDraws; ++i) draw(w, w.rg2.Get(), w.dsv.Get());
    for (int i = 0; i < 6; ++i) draw(w, w.rh.Get(), (o.secondDepth && i == 3) ? w.dsv2.Get() : w.dsv.Get());
    // q 8157, the copy of H, is not a draw: nothing reaches the route.
    draw(w, w.rsmall.Get(), nullptr, {w.scopy.Get()});
    for (int i = 0; i < 12; ++i) draw(w, w.rh.Get(), w.dsv.Get(), {w.sother.Get(), w.scopy.Get()});
    for (int i = 0; i < 3; ++i) draw(w, w.rsmall.Get(), nullptr, {w.sh.Get()});   // reads H, but an eighth per axis: rule (iv)
    if (!o.noTone) draw(w, w.rtone.Get(), nullptr, {w.sother.Get(), w.sh.Get()}, 0xF9CFC798F21E9AEAull, 0xFEE777E92850B390ull);   // the tone
    if (o.lateWrite && g_vrWorldWatchWrites) vrWorldRouteNoteWrite(w.h.Get());   // the hooks' own guard
    draw(w, w.rtone.Get(), nullptr, {w.sother.Get()});   // the game copy, the HUD, the eye composites: writes elsewhere
    gameEyeRefreshes();   // the eye cameras (kind 5) refresh after the tone, at the eye composites
    if (o.holdBoundary) return;
    vrWorldRouteFrameBoundary();
    bindingFrameBoundary();
}

size_t countLines(const char* needle) {
    size_t n = 0;
    for (const auto& l : g_log) if (l.find(needle) != std::string::npos) ++n;
    return n;
}
// The last 5 s line the route logged ("" when it has logged none).
std::string lastWindowLine() {
    std::string out;
    for (const auto& l : g_log) if (l.find("vr world route 5s:") != std::string::npos) out = l;
    return out;
}
// The one line that says the route took the world (the OWNS line), "" when there is none or more than one.
std::string ownsLine() {
    std::string out;
    size_t n = 0;
    for (const auto& l : g_log) if (l.find("OWNS the world") != std::string::npos) { out = l; ++n; }
    return n == 1 ? out : std::string();
}
void configure(bool routeOn) {
    Config::get().set("experimental.temporal_aa_on_foot_world", routeOn ? "auto" : "off");
    Config::get().set("fix.temporal_aa", "dlss");
}
void reset(World& w) {
    // Every scenario starts from a cleared route: the key off for a boundary lets go of everything.
    configure(false);
    // The camera injector may be mid-flush from the last scenario (it restores what it wrote on its first pass-through call):
    // give the route the frames it needs to wind it down, then wipe the model.
    for (int i = 0; i < 4; ++i) { gameWorldRefreshes(); vrWorldRouteFrameBoundary(); }
    g_log.clear(); g_calls.clear(); g_backendCalls = 0; g_mipsResets = 0;
    g_inj = InjectorModel{}; g_game = GameCameras{};
    Config::get().set("experimental.temporal_aa_jitter", "on");
    g_gate = g_layerLive = g_named = g_rowsKnown = g_viewsReady = true;
    g_panelW = kW; g_panelH = kH; g_realMismatch = false;
    g_namedDepth = w.depth.Get();
    edvr::detail::g_panelCurveStoodDown = false; edvr::detail::g_panelCurveCurvature = 0.0f;
    edvr::detail::g_panelCurveSegments = edvr::detail::kDefaultSegments;
    edvr::detail::g_panelCurveReady = false; edvr::detail::g_panelCurveGain = 0.0f;
    // g_panelCurveReissues is NOT zeroed: the route prints each window's delta against the last count it saw (it keeps that across the key
    // going off, as in the DLL), and a cumulative count that went backwards would print a wrapped number. A scenario adds to it instead.
}

void stage2(World& w);
void scenarios(World& w) {
    g_runtimeProfile = RuntimeProfile::Vr;

    // 1. KEY OFF: nothing runs, nothing is logged, nothing is held.
    reset(w);
    for (int i = 0; i < 30; ++i) frame(w);
    check(g_backendCalls == 0 && !g_vrWorldWants && !vrWorldRouteOwnsNextFrame() && !vrWorldRouteTreatedThisFrame() &&
              vrWorldRouteState() == VrWorldState::Off && !vrWorldRouteLayerMayTake() && !vrWorldRouteEnabled(),
          "key off: 30 frames of the chain: no resolve, no draw watched, nothing owned, the state Off");
    check(g_log.empty() && g_mipsResets == 0, "key off: not one log line and nothing released");
    check(g_inj.frameCalls == 0 && g_inj.closeCalls == 0 && g_inj.frames.empty() && g_inj.c.calls > 0,
          "key off: the camera injector is never touched (no frame step, no window close), although the game's cameras refreshed every frame");

    // 1b. KEY OFF with a curved screen: the curve does not wake a route that is off (its state is read only inside the key-auto branch).
    reset(w);
    edvr::detail::g_panelCurveCurvature = 0.3f; edvr::detail::g_panelCurveReady = true; edvr::detail::g_panelCurveGain = 35.556f;
    for (int i = 0; i < 30; ++i) frame(w);
    check(g_backendCalls == 0 && !g_vrWorldWants && !vrWorldRouteOwnsNextFrame() && !vrWorldRouteTreatedThisFrame() &&
              vrWorldRouteState() == VrWorldState::Off && !vrWorldRouteLayerMayTake() && !vrWorldRouteEnabled() && g_log.empty() && g_mipsResets == 0,
          "key off with a curved screen: 30 frames: no resolve, no draw watched, nothing owned, not one log line");
    check(g_inj.frameCalls == 0 && g_inj.closeCalls == 0 && g_inj.frames.empty(),
          "key off with a curved screen: the camera injector is never touched either");

    // 2. THE HAPPY PATH.
    reset(w);
    configure(true);
    vrWorldRouteFrameBoundary();   // the first boundary with the key on: Observing, wanting draws
    check(vrWorldRouteState() == VrWorldState::Observing && g_vrWorldWants && vrWorldRouteEnabled(), "auto: the first boundary leaves the route Observing and wanting draws");
    frame(w);
    check(g_backendCalls == 1 && g_vrWorldWants && vrWorldRouteState() == VrWorldState::Warming,
          "auto: the tone triggers one resolve in the first frame, and the route is Warming");
    check(!g_calls.empty() && g_calls[0].slot == int(kVrWorldFeatureSlot), "auto: the backend was asked for the world's own upscaler slot (2)");
    check(!g_calls.empty() && g_calls[0].hdr && g_calls[0].reset && g_calls[0].internalFlags &&
              g_calls[0].w == kW && g_calls[0].h == kH && g_calls[0].outW == kW && g_calls[0].outH == kH && g_calls[0].colour == DXGI_FORMAT_R11G11B10_FLOAT,
          "auto: the backend saw the HDR flag, a RESET, both internal flags, H's size and format, at E = R = D");
    check(!g_vrWorldInternal && !g_flatComputeInternal, "auto: the internal flags are down again after the call");
    check(countLines("first trigger at frame=") == 1, "auto: the first trigger is logged once");
    for (int i = 1; i < int(kVrWorldWarmFrames) - 1; ++i) frame(w);
    check(g_backendCalls == int(kVrWorldWarmFrames) - 1 && vrWorldRouteState() == VrWorldState::Warming && !vrWorldRouteOwnsNextFrame(),
          "auto: one treated frame short of the warm-up the route is still Warming and the eye shift is still on");
    check(g_calls.size() >= 2 && !g_calls[1].reset, "auto: the second frame continues the history (no reset)");
    frame(w);
    check(vrWorldRouteState() == VrWorldState::Owned && vrWorldRouteOwnsNextFrame() && countLines("OWNS the world") == 1,
          "auto: the kVrWorldWarmFrames-th treated frame makes it Owned, publishes it for the eye shift, and says so once");
    check(!vrWorldRouteTreatedThisFrame(), "auto: the flags of a new frame are clear until its tone");
    // A frame while owned: the route resolves at the tone and the layer may then take the screen.
    bool tookMay = false;
    gameWorldRefreshes();
    for (int i = 0; i < 3; ++i) draw(w, w.rg2.Get(), w.dsv.Get());
    for (int i = 0; i < 6; ++i) draw(w, w.rh.Get(), w.dsv.Get());
    for (int i = 0; i < 3; ++i) draw(w, w.rsmall.Get(), nullptr, {w.sh.Get()});
    check(!vrWorldRouteLayerMayTake(), "owned: before the tone of the frame the layer may not take the screen");
    draw(w, w.rtone.Get(), nullptr, {w.sother.Get(), w.sh.Get()});
    tookMay = vrWorldRouteLayerMayTake();
    check(tookMay && vrWorldRouteTreatedThisFrame(), "owned: once the route treated the frame (at the tone) the layer may take the screen draw");
    // The game's pipeline state is as the route found it (the SDK stub cleared every stage inside the resolver's isolation).
    ComPtr<ID3D11RenderTargetView> rtvNow;
    ComPtr<ID3D11ShaderResourceView> srv1;
    w.ctx->OMGetRenderTargets(1, rtvNow.GetAddressOf(), nullptr);
    w.ctx->PSGetShaderResources(1, 1, srv1.GetAddressOf());
    check(rtvNow.Get() == w.rtone.Get() && srv1.Get() == w.sh.Get(),
          "owned: after the resolve the game's render target and t1 are exactly what the tone draw had bound (state isolation)");
    vrWorldRouteNoteEyeTaken(0, 77);
    check(vrWorldRouteDoorLayerOnly(0, 77) && !vrWorldRouteDoorLayerOnly(1, 77) && !vrWorldRouteDoorLayerOnly(0, 78),
          "owned: the layer's take of eye 0 in sequence 77 is what makes the door layer-only, for that eye and sequence only");
    // The temporal door and then the sharpen pass both ask, so the same eye and sequence is asked twice: the answer is the
    // same and the 5 s line counts it once. The window prints at the first boundary after five seconds (real time).
    check(vrWorldRouteDoorLayerOnly(0, 77), "owned: the door's second question about the same eye and sequence gets the same answer");
    Sleep(5100);
    vrWorldRouteFrameBoundary();
    bindingFrameBoundary();
    {
        bool lineOk = false, stateOk = false;
        for (const auto& l : g_log) {
            if (l.find("vr world route 5s:") == std::string::npos) continue;
            lineOk = l.find("eye-takes=1 door-layer-only=1 ") != std::string::npos;
            stateOk = l.find("state=owned") != std::string::npos && l.find("selection=selected") != std::string::npos;
        }
        check(lineOk, "owned: the 5 s line counts the eye's take once and the layer-only door once (asked twice)");
        check(stateOk, "owned: the 5 s line names the state (owned) and the selection (selected)");
        // With fix.panel_curvature 0 nothing about the flat route changes: the line says curve=off and no strips, the OWNS line has no word about a curve.
        check(lastWindowLine().find(" curve=off curve-reissues=0 ") != std::string::npos && !ownsLine().empty() &&
                  ownsLine().find("curved") == std::string::npos && countLines("fix.panel_curvature") == 0,
              "owned: a flat screen: the 5 s line says curve=off curve-reissues=0, and the OWNS line and the log say nothing about a curve");
        // THE STEADY DETAIL IS ALWAYS ON (its key retired 2026-10-01): the route hands the resolver steadyDetail = true, so the resolver's
        // depth check counted the route's non-reset resolves although the census key is off, and the window printed the refusal line for
        // those frames alone, saying steady-detail=on. A route that left the steady detail off would count none and print no such line.
        std::string refusal;
        for (const auto& l : g_log) if (l.find("vr world route refusal 5s:") != std::string::npos) refusal = l;
        unsigned long long ran = 0, skipped = 0;
        const char kCheck[] = " depth-check=";
        const size_t at = refusal.find(kCheck);
        bool parsed = false;
        if (at != std::string::npos) {   // " depth-check=<ran>/<skipped> steady-detail=..."
            char* end = nullptr;
            ran = std::strtoull(refusal.c_str() + at + sizeof(kCheck) - 1, &end, 10);
            if (end && *end == '/') { skipped = std::strtoull(end + 1, nullptr, 10); parsed = true; }
        }
        check(parsed && ran + skipped >= 1 && refusal.find(" census=off ") != std::string::npos && refusal.find(" steady-detail=on view=off") != std::string::npos,
              "owned: the steady detail is on with no key: the resolver's depth check counted the route's resolves (the census key is off) and the 5 s window printed them on its refusal line");
    }
    check(!vrWorldRouteDoorLayerOnly(0, 77), "owned: the per-frame tags are cleared at the boundary");
    const auto stats = flatMonoResolveStats();
    check(stats.hdrResolves >= uint64_t(kVrWorldWarmFrames), "auto: the resolver counted its HDR resolves");

    // A long run of draws into one uninteresting target pair (the shadow atlas, the G-buffer) must not hide what follows it.
    {
        const int before = g_backendCalls;
        FrameOpts o;
        o.gbufferDraws = 6000;
        frame(w, o);
        check(g_backendCalls == before + 1, "run skip: 6000 draws into one G-buffer target pair, then H and the tone: the tone still triggers the resolve");
    }

    // The weapon fold-in seam: a map and the depth's stencil view go to the resolver together or not at all.
    {
        const auto before = flatMonoResolveStats();
        frame(w);
        check(flatMonoResolveStats().firstPersonFrames == before.firstPersonFrames && flatMonoResolveStats().firstPersonRefused == before.firstPersonRefused,
              "weapon: no weapon map this frame: the resolver gets no first-person inputs (the flat path's weapon handling)");
        g_weaponMapSrv = w.sweapon;
        frame(w);
        check(flatMonoResolveStats().firstPersonFrames == before.firstPersonFrames + 1 && flatMonoResolveStats().firstPersonRefused == before.firstPersonRefused,
              "weapon: a weapon map and the stencil view reach the resolver together and are taken");
        g_weaponMapSrv = w.sweaponSmall;
        const int calls = g_backendCalls;
        frame(w);
        check(g_backendCalls == calls + 1 && flatMonoResolveStats().firstPersonRefused == before.firstPersonRefused + 1,
              "weapon: a map of the wrong size is refused and counted by the resolver, and the frame is still treated without it");
        g_weaponMapSrv.Reset();
    }
    // 3. LATE WRITES latch the route off.
    frame(w, {true});
    frame(w, {true});
    check(vrWorldRouteState() == VrWorldState::Owned, "latch: two treated frames with a write into H after the tone do not trip it");
    frame(w, {true});
    check(vrWorldRouteState() == VrWorldState::Latched && !vrWorldRouteOwnsNextFrame() && countLines("turned off at frame=") == 1 &&
              countLines("wrote the scene HDR after the trigger") == 1,
          "latch: the third one latches the route off, releases the eye shift, and names what wrote H");
    const int callsBefore = g_backendCalls;
    for (int i = 0; i < 5; ++i) frame(w);
    check(g_backendCalls == callsBefore && !g_vrWorldWants, "latch: a latched route does not watch draws or resolve");

    // 4. REFUSALS: each selector reason, the eye route serves the frame.
    struct Refusal { const char* what; const char* reason; void (*set)(World&, bool); };
    const Refusal refusals[] = {
        {"the source was not named this frame", "depth-not-screen-motion-source", [](World&, bool on) { g_named = !on; }},
        {"the engine views are not ready", "engine-views-unavailable", [](World&, bool on) { g_viewsReady = !on; }},
        {"the source camera's rows are unknown", "camera-rows-unavailable", [](World&, bool on) { g_rowsKnown = !on; }},
        {"H is not the screen's size", "hdr-not-screen-sized", [](World&, bool on) { g_panelW = on ? kW * 2 : kW; g_panelH = on ? kH * 2 : kH; }},
    };
    for (const auto& r : refusals) {
        reset(w);
        configure(true);
        vrWorldRouteFrameBoundary();
        r.set(w, true);
        const size_t lines = g_log.size();
        (void)lines;
        for (int i = 0; i < 4; ++i) frame(w);
        char what[200];
        std::snprintf(what, sizeof(what), "refusal: %s: no resolve in 4 frames, the state never warms, and the reason %s is logged", r.what, r.reason);
        check(g_backendCalls == 0 && vrWorldRouteState() == VrWorldState::Observing && countLines(r.reason) >= 1, what);
        r.set(w, false);
        frame(w);
        check(g_backendCalls == 1, "refusal: and the next frame with the fact restored is treated");
    }
    {   // the shadow says H is bound at t1 but the real context disagrees: the route verifies the actual binding once
        reset(w);
        configure(true);
        vrWorldRouteFrameBoundary();
        g_realMismatch = true;
        for (int i = 0; i < 3; ++i) frame(w);
        check(g_backendCalls == 0 && countLines("actual-hdr-binding-or-depth-view-refused") >= 1,
              "refusal: the shadow nominated a draw whose real t1 is not H: the route verifies the actual binding and does not resolve");
        g_realMismatch = false;
    }
    {   // H drawn with two depth targets: the route cannot name its depth
        reset(w);
        configure(true);
        vrWorldRouteFrameBoundary();
        for (int i = 0; i < 3; ++i) frame(w, {false, true});
        check(g_backendCalls == 0 && countLines("hdr-depth-not-single") >= 1, "refusal: H drawn with two different depth targets is not resolved (hdr-depth-not-single)");
    }
    {   // a frame with H and no consumer
        reset(w);
        configure(true);
        vrWorldRouteFrameBoundary();
        for (int i = 0; i < 3; ++i) frame(w, {false, false, true});
        check(g_backendCalls == 0, "refusal: H with no consumer draws no resolve");
    }
    {   // the layer not live: the route stays off, with one line saying why
        reset(w);
        configure(true);
        g_layerLive = false;
        for (int i = 0; i < 5; ++i) frame(w);
        check(g_backendCalls == 0 && !g_vrWorldWants && countLines("the route stays off") == 1 && countLines("fix.ui_quality") >= 1,
              "layer not live: five frames, no resolve, draws not watched, ONE line saying the route stays off and why");
        g_layerLive = true;
        frame(w); frame(w);
        check(g_backendCalls >= 1, "layer not live: live again, the route treats");
    }
    {   // the layer not live with a curved screen: the layer is the only thing that holds the route off, and the line names ITS reason
        reset(w);
        configure(true);
        edvr::detail::g_panelCurveCurvature = 0.3f;
        g_layerLive = false;
        for (int i = 0; i < 5; ++i) frame(w);
        check(g_backendCalls == 0 && !g_vrWorldWants && countLines("the route stays off") == 1 && countLines("fix.ui_quality") >= 1 &&
                  countLines("fix.panel_curvature") == 0,
              "curved screen, layer not live: five frames, no resolve, ONE line saying the route stays off, and it names the layer's reason, not the curve");
        g_layerLive = true;
        frame(w); frame(w);
        check(g_backendCalls >= 1, "curved screen, layer live again: the route treats");
    }
    {   // THE CURVED SCREEN (fix.panel_curvature above 0) no longer holds the route off. The layer re-issues the game's own strip, so the route
        // resolves, owns and takes frames as it does for a flat screen, and says which curve (the OWNS line, the 5 s line's curve= and curve-reissues=).
        reset(w);
        configure(true);
        edvr::detail::g_panelCurveCurvature = 0.3f;   // asked for, no strip drawn yet: the substitution is still learning the panel's SIZE
        vrWorldRouteFrameBoundary();
        check(vrWorldRouteState() == VrWorldState::Observing && g_vrWorldWants && vrWorldRouteEnabled(),
              "curved screen: the first boundary with the key auto leaves the route Observing and wanting draws (a curve does not hold it off)");
        for (int i = 0; i < int(kVrWorldWarmFrames); ++i) frame(w);
        check(g_backendCalls == int(kVrWorldWarmFrames) && g_vrWorldWants && vrWorldRouteState() == VrWorldState::Owned && vrWorldRouteOwnsNextFrame(),
              "curved screen: the resolver is called on every frame, the route owns after the warm-up and the eye shift is off, as for a flat screen");
        check(countLines("the route stays off") == 0 && countLines("fix.panel_curvature") == 0,
              "curved screen: not one line says the route stays off or names fix.panel_curvature");
        check(!ownsLine().empty() &&
                  ownsLine().find("; the screen is set to curve (curve=pending): the strip is not built yet, so the game and the layer both draw the flat quad until it is") !=
                      std::string::npos &&
                  ownsLine().find("the layer draws the same strip") == std::string::npos,
              "curved screen: the OWNS line is printed once and says what is true while no strip is built: curve=pending, the flat quad is drawn");
        FrameOpts held;
        held.holdBoundary = true;
        frame(w, held);   // an owned frame, the boundary not yet run: once the route treated it (at the tone) the layer may take the screen draw
        check(vrWorldRouteLayerMayTake() && vrWorldRouteTreatedThisFrame(), "curved screen: once the route treated the frame the layer may take the (curved) screen draw");
        vrWorldRouteFrameBoundary();
        bindingFrameBoundary();
        // The 5 s line: three windows of real time. Pending and no strips; then the strip's numbers and the window's strips; then the next
        // window's own count (a delta of panel_curve's cumulative count, not the count itself).
        Sleep(5100);
        frame(w);
        std::string window = lastWindowLine();
        check(window.find(" curve=pending curve-reissues=0 ") != std::string::npos && window.find("state=owned") != std::string::npos,
              "curved screen: the 5 s line names the curve before its strip is drawn: curve=pending curve-reissues=0");
        edvr::detail::g_panelCurveReady = true;
        edvr::detail::g_panelCurveGain = 35.556f;
        edvr::detail::g_panelCurveReissues += 12;
        Sleep(5100);
        frame(w);
        window = lastWindowLine();
        check(window.find(" curve=0.300/64/35.556 curve-reissues=12 ") != std::string::npos,
              "curved screen: once the strip is ready the 5 s line names curvature/columns/gain (curve=0.300/64/35.556) and the strips the window re-issued (12)");
        edvr::detail::g_panelCurveReissues += 18;
        Sleep(5100);
        frame(w);
        window = lastWindowLine();
        check(window.find(" curve=0.300/64/35.556 curve-reissues=18 ") != std::string::npos && window.find("curve-reissues=30") == std::string::npos,
              "curved screen: curve-reissues is each window's own count (30 strips in all, 18 of them in this window)");
        check(vrWorldRouteState() == VrWorldState::Owned && g_backendCalls >= int(kVrWorldWarmFrames) + 3,
              "curved screen: and the route stayed Owned and kept resolving through the three windows");
    }
    {   // the OWNS line names the numbers when the strip is already in hand at ownership (the usual case: the substitution drew it frames ago)
        reset(w);
        configure(true);
        edvr::detail::g_panelCurveCurvature = 0.3f; edvr::detail::g_panelCurveReady = true; edvr::detail::g_panelCurveGain = 35.556f;
        vrWorldRouteFrameBoundary();
        for (int i = 0; i < int(kVrWorldWarmFrames); ++i) frame(w);
        check(g_backendCalls == int(kVrWorldWarmFrames) && vrWorldRouteState() == VrWorldState::Owned &&
                  ownsLine().find("; the screen is curved (curve=0.300/64/35.556): the layer draws the same strip the game's own draw is substituted with, "
                                  "so the bend and the placement are the game's") != std::string::npos &&
                  ownsLine().find("flat quad") == std::string::npos,
              "curved screen: with the strip in hand the OWNS line names its numbers (curve=0.300/64/35.556) and says the layer draws the game's own strip");
    }
    {   // the identity test's column count alone (curvature 0, advanced.panel_curvature_segments off its default) asks for the substitution too: a curve like any other
        reset(w);
        configure(true);
        edvr::detail::g_panelCurveSegments = edvr::detail::kDefaultSegments + 8;
        edvr::detail::g_panelCurveReady = true; edvr::detail::g_panelCurveGain = 20.0f;
        vrWorldRouteFrameBoundary();
        for (int i = 0; i < int(kVrWorldWarmFrames); ++i) frame(w);
        check(g_backendCalls == int(kVrWorldWarmFrames) && vrWorldRouteState() == VrWorldState::Owned && countLines("the route stays off") == 0 &&
                  ownsLine().find("the screen is curved (curve=0.000/72/20.000)") != std::string::npos,
              "curved screen: a non-default segment count (the identity test) is a curve the route runs with, and the OWNS line names it (curve=0.000/72/20.000)");
    }
    {   // a substitution that stood itself down (a fault, or a SIZE that is no panel's) draws the game's own quad again: the route still runs
        reset(w);
        configure(true);
        edvr::detail::g_panelCurveCurvature = 0.3f; edvr::detail::g_panelCurveStoodDown = true;
        vrWorldRouteFrameBoundary();
        for (int i = 0; i < int(kVrWorldWarmFrames); ++i) frame(w);
        check(g_backendCalls == int(kVrWorldWarmFrames) && vrWorldRouteState() == VrWorldState::Owned && countLines("the route stays off") == 0 &&
                  ownsLine().find("; the screen is set to curve but the curve stood down (curve=stood-down): the game draws its own flat quad and the layer re-issues it flat") !=
                      std::string::npos &&
                  ownsLine().find("the layer draws the same strip") == std::string::npos,
              "curved screen: a substitution that stood itself down does not hold the route off, and the OWNS line says the curve stood down and the flat quad is drawn");
    }
    {   // the gate lost while owned, then regained
        reset(w);
        configure(true);
        vrWorldRouteFrameBoundary();
        for (int i = 0; i < int(kVrWorldWarmFrames); ++i) frame(w);
        check(vrWorldRouteState() == VrWorldState::Owned, "gate: owned after the warm-up");
        g_gate = false;
        frame(w);
        check(vrWorldRouteState() == VrWorldState::Observing && !vrWorldRouteOwnsNextFrame() && !g_vrWorldWants && countLines("on-foot-gate-lost") == 1,
              "gate: the gate lost releases the world at once (on-foot-gate-lost), puts the eye shift back and stops watching draws");
    }

    // 5. A SCENE RESET releases and resets; a FRAME GAP resets the history.
    reset(w);
    configure(true);
    vrWorldRouteFrameBoundary();
    for (int i = 0; i < int(kVrWorldWarmFrames); ++i) frame(w);
    g_calls.clear();
    vrWorldRouteNoteSceneReset();
    frame(w);   // the frame still draws and treats; the boundary releases
    check(g_calls.size() == 1 && g_calls[0].reset, "scene reset: the next resolve after the event resets the history");
    check(vrWorldRouteState() == VrWorldState::Observing && !vrWorldRouteOwnsNextFrame() && countLines("(scene-reset)") == 1,
          "scene reset: an owned route is released at the boundary (scene-reset) and the eye shift is back");
    frame(w);
    check(g_calls.size() == 2 && !g_calls[1].reset, "scene reset: and the history continues from there");
    g_named = false;
    frame(w);   // a refused frame: a gap
    g_named = true;
    frame(w);
    check(g_calls.back().reset, "frame gap: a treat after an untreated frame resets the history");
    frame(w);
    check(!g_calls.back().reset, "frame gap: and the frame after that continues it");

    // 6. THE KEY GOES OFF while owned: everything is let go.
    reset(w);
    configure(true);
    vrWorldRouteFrameBoundary();
    for (int i = 0; i < int(kVrWorldWarmFrames); ++i) frame(w);
    const uint64_t resetsBefore = flatMonoResolveStats().fullResets;
    const int mipsBefore = g_mipsResets;
    configure(false);
    vrWorldRouteFrameBoundary();
    check(vrWorldRouteState() == VrWorldState::Off && !vrWorldRouteOwnsNextFrame() && !g_vrWorldWants && countLines("(key-off)") == 1,
          "key off while owned: released at once (key-off), the eye shift back on, draws no longer watched");
    check(flatMonoResolveStats().fullResets == resetsBefore + 1 && g_mipsResets == mipsBefore + 1,
          "key off while owned: the resolver's textures and the mipped screen are let go, once");
    vrWorldRouteFrameBoundary();
    check(flatMonoResolveStats().fullResets == resetsBefore + 1 && g_mipsResets == mipsBefore + 1, "key off: and not again on the boundaries after");
    stage2(w);
}


// ---- STAGE 2: the world jitter against the injector model --------------------------------------------------------------------
// The last step the route took of the injector (an empty frame when it took none, so a regression fails a check instead of
// reading past the end of an empty vector).
FlatCameraVrFrame lastStep() { return g_inj.frames.empty() ? FlatCameraVrFrame{} : g_inj.frames.back(); }
bool lastCallInject() { return lastStep().inject; }
float phaseOfLastCall() {
    return std::fabs(lastStep().phaseX) + std::fabs(lastStep().phaseY);
}
void startRoute(World& w) {
    reset(w);
    configure(true);
    vrWorldRouteFrameBoundary();
}
// Frames until the route is Owned and the phase machine is past its warm-up (the world's phase is non-zero in the window the
// last boundary opened).
void ownedAndJittering(World& w) {
    startRoute(w);
    for (int i = 0; i < int(kVrWorldWarmFrames) + 2; ++i) frame(w);
}
size_t countInjectedFrames() {
    size_t n = 0;
    for (const auto& f : g_inj.frames) n += f.inject ? 1 : 0;
    return n;
}

void stage2(World& w) {
    // A. THE PHASE: the window opens after the first treated frame (Warming), the phase machine's warm-up gives two zero
    // phases and then the flat profile's sequence, the resolver gets the phase the scene calls took, and closes it at the trigger.
    startRoute(w);
    check(g_inj.frameCalls == 0, "jitter: in Observing (no treated frame yet) the injector is not driven at all");
    frame(w);   // the first treated frame: the route is Warming from its boundary on
    check(vrWorldRouteState() == VrWorldState::Warming && g_inj.injectCalls == 1 && lastCallInject() && phaseOfLastCall() == 0.0f,
          "jitter: the first boundary after a treated frame (Warming, it named its source) opens the window, with the phase machine's first zero phase");
    check(g_calls.size() == 1 && g_calls[0].jx == 0.0f && g_calls[0].jy == 0.0f,
          "jitter: the first resolve is unjittered (the route had not opened a window for it)");
    {
        const FlatCameraVrFrame f0 = lastStep();
        check(f0.renderW == kW && f0.renderH == kH && std::fabs(f0.screenAspect - float(kW) / float(kH)) < 1e-6f && !f0.observe,
              "jitter: the window is told the render size of the screen's H and its aspect (the role test's anchor); no census is asked for");
    }
    frame(w);   // frame 2: window open, zero phase (warming): the injector counts the screen calls and writes nothing
    check(g_inj.injectCalls == 2 && phaseOfLastCall() == 0.0f && g_calls.size() == 2 && g_calls[1].jx == 0.0f,
          "jitter: the second frame is still the phase machine's warm-up: zero phase, resolver jitter zero");
    frame(w);   // frame 3
    check(g_inj.injectCalls == 3 && phaseOfLastCall() != 0.0f,
          "jitter: after two treated zero-phase frames the next window carries a non-zero phase");
    const float px = lastStep().phaseX, py = lastStep().phaseY;
    check(std::fabs(px) <= 0.5f && std::fabs(py) <= 0.5f, "jitter: the phase is a sub-pixel shift of the render grid (within half a pixel)");
    g_inj.order.clear();
    frame(w);   // frame 4: the first jittered frame
    check(g_calls.size() == 4 && g_calls[3].jx == px && g_calls[3].jy == py,
          "jitter: the resolver's jitter input is exactly the phase the scene calls took (render pixels, positive right/down)");
    {
        // The order inside the frame: the window closes at the trigger, before the resolver runs.
        int close = -1, resolve = -1;
        for (size_t i = 0; i < g_inj.order.size(); ++i) { if (g_inj.order[i] == 2 && close < 0) close = int(i); if (g_inj.order[i] == 3 && resolve < 0) resolve = int(i); }
        check(close >= 0 && resolve >= 0 && close < resolve, "window: the injection window is closed at the trigger, before the resolver's own call");
        check(g_inj.closeCalls == 3, "window: the window is closed once a frame while the injector is engaged (frames 2, 3 and 4; frame 1 had none)");
    }
    check(countLines("the world is JITTERED from frame=") == 1 && countLines("camera rows disagree with the phase") == 0,
          "jitter: the first jittered frame says so once, and the consecutive rows agree with the phases they carry");
    for (int i = 0; i < 6; ++i) frame(w);
    check(vrWorldRouteState() == VrWorldState::Owned && lastEnded().unsupported == 6 && lastEnded().injectedKind[5] == 0 &&
              lastEnded().injectedKind[3] > 0,
          "kinds: the eye cameras (kind 5) refresh every frame and are counted Unsupported, never injected; only kind 3 is");
    check(countLines("STOP") == 0 && countLines("EXCLUDED") == 0, "jitter: nothing STOPs and nothing is excluded in a healthy run");
    {
        float maxAbs = 0.0f;
        for (size_t i = 3; i < g_calls.size(); ++i) maxAbs = std::fmax(maxAbs, std::fabs(g_calls[i].jx) + std::fabs(g_calls[i].jy));
        check(maxAbs > 0.0f, "jitter: over the run the resolver saw a non-zero phase");
    }
    // The eye shift's partner: the route is Owned and publishes it for native_temporal exactly as before (nothing about it changed).
    check(vrWorldRouteOwnsNextFrame() && vrWorldRouteWorldPhase(nullptr, nullptr), "jitter: an owned route reports a live world phase and still owns the next frame");
    {
        float x = 9.0f, y = 9.0f;
        check(vrWorldRouteWorldPhase(&x, &y) && x == lastStep().phaseX && y == lastStep().phaseY,
              "jitter: worldPhase() answers with the phase the running frame was given");
    }

    // B. THE NAMING RULE (design-world-camera-motion-2026-09-30.md section 5): a map frame refreshes about thirty kind-3 cameras
    // and must never pick up the world's phase, through the route's grace frames too.
    ownedAndJittering(w);
    check(vrWorldRouteState() == VrWorldState::Owned && lastCallInject() && phaseOfLastCall() != 0.0f, "naming: owned and jittering before the map opens");
    const size_t injectedBefore = countInjectedFrames();
    g_named = false;       // the map opens: nothing names the screen's source
    g_game.scene = 30;     // and about thirty kind-3 cameras refresh a frame
    frame(w);              // M1: its window was decided from the last world frame, so it is open; it names nothing
    check(lastEnded().sceneInjected == 30 && lastEnded().injectedKind[3] == 30,
          "naming: M1's window WAS open: nothing can say a frame is a map before its cameras refresh, so its thirty kind-3 cameras took the phase, this once");
    check(countInjectedFrames() == injectedBefore + 0, "naming: (the window count does not move at M1's end: the next window is shut)");
    check(countLines("named no source for the screen but its camera window was open") == 1,
          "naming: the first map frame had its window open (nothing could say it was a map before its cameras refreshed): counted and said once");
    check(!lastCallInject(), "NAMING: the window for the frame AFTER an unnamed frame is SHUT: the injector is told to pass everything through");
    const size_t afterM1 = g_inj.injectCalls;
    frame(w);              // M2: shut; grace frame 2 of 3 (the route is still Owned)
    check(vrWorldRouteState() == VrWorldState::Owned && !lastCallInject() && g_inj.injectCalls == afterM1 &&
              lastEnded().sceneInjected == 0 && lastEnded().warming == 0 && lastEnded().stale == 30,
          "NAMING: a second unnamed frame through the route's grace: still Owned, the window shut, its thirty kind-3 refreshes injected nothing");
    check(countLines("STOP") == 0, "naming: a shut window that stays shut is no STOP");
    frame(w);              // M3: the third miss releases the world
    check(vrWorldRouteState() == VrWorldState::Observing && countLines("RELEASED the world") == 1 && !lastCallInject() &&
              countLines("last decline: depth-not-screen-motion-source x3") == 1,
          "naming: three unnamed frames release the route, the window stays shut, and the RELEASED line names the last decline and its run");
    for (int i = 0; i < 10; ++i) frame(w);
    check(!lastCallInject() && g_inj.injectCalls == afterM1 && lastEnded().sceneInjected == 0,
          "NAMING: ten more map frames with the route released: not one more window opened, nothing injected");
    check(countLines("declined at frame=") == 3, "decline log: a run of thirteen declines logs three lines (the cap is per run, not per session)");
    // The map closes: the world names its source again.
    g_named = true;
    g_game.scene = 48;
    frame(w);              // W1: the route is Observing, the window shut; it treats the frame (unjittered)
    check(vrWorldRouteState() == VrWorldState::Warming && g_calls.back().jx == 0.0f && g_calls.back().jy == 0.0f && lastCallInject() && phaseOfLastCall() == 0.0f,
          "naming: the world's first frame after the map is resolved unjittered; the next window opens (Warming, named) with the warm-up's zero phase");
    frame(w); frame(w);
    check(phaseOfLastCall() != 0.0f && countLines("declined at frame=") == 3,
          "naming: two more zero-phase frames, then the phase is back; and the decline log was not spent by the map");
    // A second map: the decline log re-arms (a treated frame ended the first run).
    g_named = false;
    for (int i = 0; i < 6; ++i) frame(w);
    check(countLines("declined at frame=") == 6, "decline log: a second map's run of declines logs its own three lines");
    g_named = true;

    // A single unnamed frame in a world (H2 says it does not happen for three; one is within the grace): the window shuts for
    // one frame and the phase machine starts its warm-up again.
    ownedAndJittering(w);
    g_named = false;
    frame(w);
    g_named = true;
    check(!lastCallInject(), "naming: one unnamed frame shuts the next frame's window");
    frame(w);   // named again, window shut: treated unjittered
    check(vrWorldRouteState() == VrWorldState::Owned && lastCallInject() && phaseOfLastCall() == 0.0f,
          "naming: the frame after it names its source again: the window reopens, with a zero phase (the warm-up restarts)");
    frame(w); frame(w);
    check(phaseOfLastCall() != 0.0f, "naming: and the phase is back two frames later");

    // The STOP: an injector that writes through a shut window.
    ownedAndJittering(w);
    g_named = false;
    frame(w);   // M1 (open, expected)
    g_inj.misbehave = true;
    frame(w);   // M2: the route shut the window; the broken injector writes anyway
    check(countLines("STOP at frame=") == 1 && countLines("on a frame whose window the route had shut (decision: unnamed)") == 1,
          "STOP: camera calls injected on a frame the route had shut are a STOP that names the decision that shut it");
    g_inj.misbehave = false;
    g_named = true;
    for (int i = 0; i < 12; ++i) frame(w);
    check(!lastCallInject() && countLines("STOP at frame=") == 1,
          "STOP: the injector is switched off for the session: the route never opens a window again (and does not repeat the line)");
    configure(false);
    vrWorldRouteFrameBoundary();
    configure(true);
    vrWorldRouteFrameBoundary();
    for (int i = 0; i < int(kVrWorldWarmFrames) + 1; ++i) frame(w);
    check(lastCallInject(), "STOP: flipping the route key off and auto again clears it");

    // The STOP on an injected kind other than 3.
    ownedAndJittering(w);
    g_inj.wrongKind = true;
    frame(w);
    check(countLines("camera calls of a kind other than 3 were INJECTED") == 1 && countLines("5: 1") == 1,
          "STOP: an injected kind-5 (eye) call is a STOP: only kind 3 may be injected");
    frame(w);
    check(!lastCallInject(), "STOP: and the injector is off for the session");

    // C. THE GLOBAL JITTER KEY. The route has no jitter key of its own (retired 2026-10-01): with the route on and nothing else set, the
    // world it owns is jittered (A, B and every scenario above prove it), and experimental.temporal_aa_jitter off is the one setting
    // that keeps the route and zeroes the world phase. Off from the start: the route owns the world and resolves it unjittered, exactly
    // flight 1's behaviour, and the injector is never touched.
    reset(w);
    Config::get().set("experimental.temporal_aa_jitter", "off");
    configure(true);
    vrWorldRouteFrameBoundary();
    for (int i = 0; i < int(kVrWorldWarmFrames) + 6; ++i) frame(w);
    {
        float maxAbs = 0.0f;
        for (const auto& c : g_calls) maxAbs = std::fmax(maxAbs, std::fabs(c.jx) + std::fabs(c.jy));
        check(vrWorldRouteState() == VrWorldState::Owned && g_backendCalls >= int(kVrWorldWarmFrames) + 6 && maxAbs == 0.0f,
              "global key: with experimental.temporal_aa_jitter off the route owns the world and resolves it unjittered, exactly flight 1's behaviour");
    }
    check(g_inj.frameCalls == 0 && g_inj.closeCalls == 0,
          "global key: with it off from the start the injector is never installed, stepped or closed");
    {
        float x = 9.0f, y = 9.0f;
        check(!vrWorldRouteWorldPhase(&x, &y) && x == 0.0f && y == 0.0f, "global key: worldPhase() is false and zero");
    }
    Config::get().set("experimental.temporal_aa_jitter", "on");   // flipped live, from the menu
    for (int i = 0; i < 6; ++i) frame(w);
    check(vrWorldRouteState() == VrWorldState::Owned && g_inj.injectCalls > 0 && lastCallInject() && phaseOfLastCall() != 0.0f,
          "global key: flipped on live while owned, the next boundary opens the window and, after the warm-up, the phase is non-zero");
    {
        float maxAbs = 0.0f;
        for (size_t i = g_calls.size() - 2; i < g_calls.size(); ++i) maxAbs = std::fmax(maxAbs, std::fabs(g_calls[i].jx) + std::fabs(g_calls[i].jy));
        check(maxAbs > 0.0f, "global key: and the resolver sees it (the world is jittered with no other setting touched)");
    }
    Config::get().set("experimental.temporal_aa_jitter", "off");   // flipped off live
    frame(w);   // the frame that was running when the key flipped: its window was already open (the key is read at the boundary)
    check(!lastCallInject() && vrWorldRouteState() == VrWorldState::Owned,
          "global key: flipped off live, the boundary shuts the next frame's window at once, and the route still owns the world");
    frame(w);
    check(g_calls.back().jx == 0.0f && g_calls.back().jy == 0.0f && lastEnded().sceneInjected == 0,
          "global key: and that frame is unjittered: the resolver's phase is zero again and nothing was injected");
    const int afterOff = g_inj.frameCalls;
    for (int i = 0; i < 5; ++i) frame(w);
    check(g_inj.frameCalls == afterOff + 0 && !g_inj.injecting && !g_inj.pendingFlush && !g_inj.gateOpen,
          "global key: the injector was passed through until what it wrote was restored, then its relay gate was closed and it was left alone (no further steps)");
    Config::get().set("experimental.temporal_aa_jitter", "on");
    for (int i = 0; i < 6; ++i) frame(w);
    check(lastCallInject() && phaseOfLastCall() != 0.0f, "global key: on again: the world is jittered again, with nothing else to switch");

    // D. THE HOOK IS NOT LIVE: the world stays unjittered and says so once.
    reset(w);
    g_inj.hookLive = false;
    configure(true);
    vrWorldRouteFrameBoundary();
    for (int i = 0; i < int(kVrWorldWarmFrames) + 4; ++i) frame(w);
    {
        float maxAbs = 0.0f;
        for (const auto& c : g_calls) maxAbs = std::fmax(maxAbs, std::fabs(c.jx) + std::fabs(c.jy));
        check(vrWorldRouteState() == VrWorldState::Owned && maxAbs == 0.0f && countLines("the camera hook is not available (failed)") == 1,
              "no hook: the route still owns the world, unjittered, and says once that the camera hook is not available");
    }

    // E. A PARTIAL INJECTION: some of the frame's scene cameras took the phase and one did not: the frame is not resolved.
    ownedAndJittering(w);
    g_inj.refuseOne = true;
    const size_t callsBeforeRefuse = g_calls.size();
    frame(w);
    check(g_calls.size() == callsBeforeRefuse && countLines("camera-injection-incomplete") >= 1,
          "coverage: a frame part of whose world cameras took the phase is declined (camera-injection-incomplete), never resolved with one phase");
    frame(w);
    check(!g_calls.empty() && lastCallInject() && phaseOfLastCall() == 0.0f,
          "coverage: and the phase machine starts its warm-up again (a zero phase in the next window)");

    // E2. NO SCENE CALL: the window was open with a phase and the game refreshed no scene camera before the tone: the frame is resolved
    // unjittered (a phase nothing wrote is never claimed) and the line says why.
    ownedAndJittering(w);
    g_game.scene = 0;
    const size_t callsBeforeNoScene = g_calls.size();
    frame(w);
    check(g_calls.size() == callsBeforeNoScene + 1 && g_calls.back().jx == 0.0f && g_calls.back().jy == 0.0f &&
              countLines("jitter is wanted but no scene camera call was injected") == 1,
          "no scene call: a frame whose window was open and whose scene cameras never refreshed is resolved unjittered, and says why");

    // F. EXCLUDED CAMERAS: auxiliary kind-3 calls are counted and named once, never injected.
    ownedAndJittering(w);
    g_game.aux = 7;
    FlatCameraVrExcluded e1{}, e2{};
    e1.aspect = 1.0f; e1.fov = 1.0472f; e1.nearZ = 0.1f; e1.farZ = 5000.0f; e1.callerRva = 0x594FE1; e1.calls = 5;
    e2.aspect = 1.0f; e2.fov = 1.5708f; e2.nearZ = 0.5f; e2.farZ = 9000.0f; e2.callerRva = 0x594EAB; e2.calls = 2;
    g_inj.excluded.push_back(e1);
    frame(w);
    check(countLines("camera call EXCLUDED, not a screen view") == 1 && lastEnded().auxiliary == 7,
          "roles: an auxiliary kind-3 signature is named once, with its aspect and caller, and the calls are counted");
    g_inj.excluded.push_back(e2);
    for (int i = 0; i < 3; ++i) frame(w);
    check(countLines("camera call EXCLUDED, not a screen view") == 2, "roles: a second signature is named once more; the first is not repeated");

    // G. THE KEY GOES OFF while the injector is engaged: it is passed through, restored, and the boundary's early return is back.
    ownedAndJittering(w);
    check(g_inj.injecting, "key off live: (precondition) the injector is engaged");
    configure(false);
    vrWorldRouteFrameBoundary();
    check(!lastCallInject() && vrWorldRouteState() == VrWorldState::Off, "key off live: the window is shut at the boundary where the key went off");
    for (int i = 0; i < 4; ++i) frame(w);
    const int frozen = g_inj.frameCalls;
    for (int i = 0; i < 5; ++i) frame(w);
    check(g_inj.frameCalls == frozen && !g_inj.injecting && !g_inj.pendingFlush && !g_inj.gateOpen,
          "key off live: the injector's relay gate is closed and it is left alone again once what it wrote is restored (the boundary returns at its first test)");

    // H. THE GATE IS LOST while owned and jittering: released, and the window shuts.
    ownedAndJittering(w);
    g_gate = false;
    frame(w);
    check(vrWorldRouteState() == VrWorldState::Observing && !lastCallInject(), "gate lost: the release shuts the window at the same boundary");
    g_gate = true;
}

int runSelfTest() {
    World w;
    if (!build(w)) { std::printf("vr world route gpu: could not build the fixture\n"); return 1; }
    scenarios(w);
    if (g_failures) {
        std::printf("vr world route gpu: %d of %d checks FAILED\n", g_failures, g_checks);
        return 1;
    }
    std::printf("vr world route gpu: PASS (%d checks)\n", g_checks);
    return 0;
}
}  // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::strcmp(argv[1], "--self-test") == 0) return runSelfTest();
    if (argc == 2 && std::strcmp(argv[1], "--dry-run") == 0) {
        std::printf("vr world route gpu test: would run the route's runtime on WARP against a synthetic chain; writes no files.\n");
        return 0;
    }
    std::printf("usage: vr_world_route_gpu_test --self-test|--dry-run\n");
    return 2;
}
