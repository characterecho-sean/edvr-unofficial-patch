// visual.desktop_window: see desktop_window.h for the shape of the feature and docs\desktop-window.md for the design and what the first flight must show.
//
// Threads. noteEye (the handoff) runs on the XR owner thread and only takes the mutex, swaps a reference and stamps a tick. Everything else runs on the render
// thread, in Elite's Present hook, before the real Present: the config read, the window checks, the draw, the log lines. The window is moved from a thread-pool
// callback with SWP_ASYNCWINDOWPOS, never from the hook: Elite's window procedure could otherwise run re-entrantly inside Present.
#include "temporal_shader_bytecode.h"
#include "desktop_window.h"

#include <windows.h>

#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <string>

#include "../common/config.h"
#include "../common/desktop_window_math.h"
#include "../common/guard.h"
#include "../common/log.h"
#include "../common/native_desktop_window.h"
#include "../common/timing.h"
#include "graphics_runtime.h"
#include "shader_swap.h"
#include "vr_world_route.h"

namespace edvr {

std::atomic<void*> g_desktopWindowBack{nullptr};

namespace {
using Microsoft::WRL::ComPtr;

FaultBudget g_budget("desktopWindow", 4);

constexpr uint64_t kTenSecondsMs = 10000;
constexpr uint64_t kEvaluateEveryMs = 100;      // the window is read (not moved) at most this often
constexpr uint64_t kRequestEveryMs = 1000;      // a move is requested at most this often
constexpr int kNearPx = 2;                      // a window within this of its target is at it
constexpr unsigned kFightsAllowed = 10;         // the game moves the window off our target more than this and we stop for the session
constexpr unsigned kUnsettledAllowed = 20;      // requests in a row after which the window never reached its target: we stop for the session
constexpr unsigned kRequestLogCap = 32;
constexpr unsigned kActivationLogCap = 8;

// ---- the handoff: the latest finished texture of each eye, from the XR owner thread ----
struct Held {
    ID3D11Texture2D* texture = nullptr;
    float bounds[4] = {0, 0, 1, 1};
    uint64_t serial = 0;
};
std::mutex g_mutex;
Held g_held[2];
constexpr unsigned kRetireMax = 8;
ID3D11Texture2D* g_retired[kRetireMax] = {};   // displaced by a newer handoff; released on the render thread, where the device is used
unsigned g_retiredCount = 0;
uint64_t g_serial = 0;
std::atomic<unsigned> g_retirePending{0};
std::atomic<unsigned> g_wantMask{0};           // the eyes the setting needs, as the last Present read it; noteEye holds nothing else
std::atomic<uint64_t> g_lastTick[2] = {};      // stampMs of each eye's last handoff, 0 never
std::atomic<uint64_t> g_handoffs[2] = {};
std::atomic<bool> g_holding{false};
std::atomic<uint64_t> g_skipped{0};
int g_contextToken = 0;                        // the table's context: any address that is ours

HRESULT WINAPI noteEye(void* context, uint32_t eye, ID3D11Texture2D* texture, const float* bounds) {
    if (context != &g_contextToken || eye > 1 || !texture || !bounds) return E_INVALIDARG;
    for (int i = 0; i < 4; ++i) {
        if (!std::isfinite(bounds[i])) return E_INVALIDARG;
    }
    if (!(g_wantMask.load(std::memory_order_relaxed) & (1u << eye))) return S_FALSE;
    texture->AddRef();
    ID3D11Texture2D* spill = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        Held& h = g_held[eye];
        if (h.texture) {
            if (g_retiredCount < kRetireMax) g_retired[g_retiredCount++] = h.texture;
            else spill = h.texture;
        }
        h.texture = texture;
        std::memcpy(h.bounds, bounds, sizeof h.bounds);
        h.serial = ++g_serial;
        g_retirePending.store(g_retiredCount, std::memory_order_relaxed);
    }
    g_holding.store(true, std::memory_order_relaxed);
    g_lastTick[eye].store(stampMs(), std::memory_order_release);
    g_handoffs[eye].fetch_add(1, std::memory_order_relaxed);
    if (spill) spill->Release();
    return S_OK;
}

HRESULT WINAPI closeProvider(void* context) {
    if (context != &g_contextToken) return E_INVALIDARG;
    g_lastTick[0].store(0, std::memory_order_release);   // the next Present sees no fresh eye, stops, and lets everything go
    g_lastTick[1].store(0, std::memory_order_release);
    return S_OK;
}

void drainRetired() {
    if (!g_retirePending.load(std::memory_order_relaxed)) return;
    ID3D11Texture2D* out[kRetireMax];
    unsigned n = 0;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        n = g_retiredCount;
        std::memcpy(out, g_retired, sizeof(ID3D11Texture2D*) * n);
        g_retiredCount = 0;
        g_retirePending.store(0, std::memory_order_relaxed);
    }
    for (unsigned i = 0; i < n; ++i) out[i]->Release();
}

// ---- the render thread's own state ----
struct SrvUse {
    ComPtr<ID3D11ShaderResourceView> view;
    bool ok = false;
    bool srgb = false;
    DXGI_FORMAT source = DXGI_FORMAT_UNKNOWN;
    char reason[96] = {};
};
struct SrvEntry {
    ComPtr<ID3D11Texture2D> keep;   // the entry keeps its texture alive, so its address cannot be reused while the entry stands
    SrvUse use;
};
constexpr unsigned kSrvCache = 6;

struct Pipe {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps;
    ComPtr<ID3D11Buffer> cb;
    ComPtr<ID3D11SamplerState> sampler;
    ComPtr<ID3D11RasterizerState> raster;
    ComPtr<ID3D11DepthStencilState> depth;
    ComPtr<ID3D11BlendState> blend;
    bool tried = false;
    bool ready = false;
};

struct Session {
    // the setting
    bool modeRead = false, modeFlat = false;
    uint32_t modeGeneration = 0;
    DesktopWindowMode mode = DesktopWindowMode::Game;
    bool settingLogged = false;
    // the pipeline
    Pipe pipe;
    SrvEntry srv[kSrvCache];
    unsigned srvNext = 0;
    // the activation
    bool active = false;
    bool dead = false;                  // a fault or a disabled graphics runtime: no more work this session
    HWND hwnd = nullptr;
    unsigned activations = 0;
    uint64_t seen = 0, drawn = 0, stale = 0;
    uint64_t presentsSinceReshape = 0;
    uint64_t nextStatsMs = 0;
    uint64_t lastSerial[2] = {};
    uint32_t chainW = 0, chainH = 0;
    int clientW = 0, clientH = 0;
    bool scalingKnown = false;
    DwScaling scaling = DwScaling::Stretch;
    IDXGISwapChain* scalingChain = nullptr;   // identity only, to notice a different chain
    // the window
    bool haveGameRect = false, haveTarget = false, atTargetSeen = false;
    int gameRect[4] = {};
    int target[4] = {};
    unsigned requests = 0, fights = 0, unsettled = 0;
    bool gaveUp = false;
    uint64_t lastRequestMs = 0, lastEvaluateMs = 0;
    // log-once memory
    char declined[8][128] = {};
    unsigned declinedCount = 0;
    bool noteExclusive = false, noteZoomed = false, noteShape = false;
};
Session S;

void declineOnce(const char* text) {
    for (unsigned i = 0; i < S.declinedCount; ++i) {
        if (!std::strcmp(S.declined[i], text)) return;
    }
    if (S.declinedCount < 8) {
        std::snprintf(S.declined[S.declinedCount], sizeof S.declined[0], "%s", text);
        ++S.declinedCount;
    }
    Log::get().note("desktop window: not drawing -- %s", text);
}

// ---- the setting ----
void refreshMode(bool flat) {
    Config& cfg = Config::get();
    const uint32_t generation = cfg.generation();
    if (S.modeRead && generation == S.modeGeneration && flat == S.modeFlat) return;
    S.modeRead = true;
    S.modeGeneration = generation;
    S.modeFlat = flat;
    if (flat) {   // the flat profile never runs it, and says nothing about it
        S.mode = DesktopWindowMode::Game;
        return;
    }
    const std::string raw = cfg.getString("visual.desktop_window", "game");
    bool known = true;
    const DesktopWindowMode mode = desktopWindowParse(raw.c_str(), &known);
    if (!known) Log::get().note("desktop window: unknown setting \"%.40s\" (it is game, left_eye, right_eye or both_eyes); treated as game", raw.c_str());
    if (!S.settingLogged || mode != S.mode) {
        char line[160];
        desktopWindowSettingLine(line, sizeof line, desktopWindowModeName(mode), S.settingLogged ? desktopWindowModeName(S.mode) : nullptr);
        Log::get().note("%s", line);
        S.settingLogged = true;
    }
    S.mode = mode;
}

// ---- the held eyes ----
void releaseHeld() {
    g_holding.store(false, std::memory_order_relaxed);
    ID3D11Texture2D* out[2 + kRetireMax];
    unsigned n = 0;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        for (Held& h : g_held) {
            if (h.texture) out[n++] = h.texture;
            h = Held{};
        }
        for (unsigned i = 0; i < g_retiredCount; ++i) out[n++] = g_retired[i];
        g_retiredCount = 0;
        g_retirePending.store(0, std::memory_order_relaxed);
    }
    for (unsigned i = 0; i < n; ++i) out[i]->Release();
    for (SrvEntry& e : S.srv) e = SrvEntry{};
    S.srvNext = 0;
}

struct Snap {
    ComPtr<ID3D11Texture2D> texture[2];
    float bounds[2][4] = {};
    uint64_t serial[2] = {};
};
void snapshot(Snap& s, unsigned need) {
    std::lock_guard<std::mutex> lock(g_mutex);
    for (unsigned eye = 0; eye < 2; ++eye) {
        if (!(need & (1u << eye)) || !g_held[eye].texture) continue;
        s.texture[eye] = g_held[eye].texture;   // ComPtr copy: its own reference
        std::memcpy(s.bounds[eye], g_held[eye].bounds, sizeof s.bounds[eye]);
        s.serial[eye] = g_held[eye].serial;
    }
}

SrvUse srvFor(ID3D11Device* device, ID3D11Texture2D* texture) {
    for (SrvEntry& e : S.srv) {
        if (e.keep.Get() == texture) return e.use;
    }
    SrvEntry& e = S.srv[S.srvNext];
    S.srvNext = (S.srvNext + 1) % kSrvCache;
    e = SrvEntry{};
    e.keep = texture;
    D3D11_TEXTURE2D_DESC d{};
    texture->GetDesc(&d);
    e.use.source = d.Format;
    const DwFormats f = desktopWindowFormats(d.Format, DXGI_FORMAT_R8G8B8A8_UNORM);
    if (!f.ok) {
        std::snprintf(e.use.reason, sizeof e.use.reason, "%s", f.reason);
    } else if (d.SampleDesc.Count != 1 || d.ArraySize != 1 || !(d.BindFlags & D3D11_BIND_SHADER_RESOURCE)) {
        std::snprintf(e.use.reason, sizeof e.use.reason, "the eye image is not a single-sample, single-slice shader resource (samples %u, slices %u, bind 0x%X)",
                      d.SampleDesc.Count, d.ArraySize, d.BindFlags);
    } else {
        D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.Format = f.view;
        sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        sd.Texture2D.MostDetailedMip = 0;
        sd.Texture2D.MipLevels = 1;
        const HRESULT hr = device->CreateShaderResourceView(texture, &sd, &e.use.view);
        if (FAILED(hr)) {
            std::snprintf(e.use.reason, sizeof e.use.reason, "a shader view of the eye image could not be made (0x%08X, DXGI format %d)", static_cast<unsigned>(hr),
                          static_cast<int>(d.Format));
            e.use.view.Reset();
        } else {
            e.use.ok = true;
            e.use.srgb = f.srgb;
        }
    }
    return e.use;
}

// ---- the draw's pipeline ----
bool ensurePipe(ID3D11Device* device, ID3D11DeviceContext* ctx) {
    Pipe& p = S.pipe;
    if (p.device.Get() != device) {
        p = Pipe{};
        p.device = device;
    }
    if (p.tried) return p.ready;
    p.tried = true;
    p.vs.Attach(shaderSwapCreateVs(ctx, kDesktopWindowVsBytecode, sizeof(kDesktopWindowVsBytecode), "desktop_window_vs", "desktop window"));
    p.ps.Attach(shaderSwapCreatePs(ctx, kDesktopWindowPsBytecode, sizeof(kDesktopWindowPsBytecode), "desktop_window_ps", "desktop window"));
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = 32;
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    D3D11_DEPTH_STENCIL_DESC dd{};   // depth and stencil off
    D3D11_BLEND_DESC bld{};
    bld.RenderTarget[0].BlendEnable = FALSE;
    bld.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    const bool made = p.vs && p.ps && SUCCEEDED(device->CreateBuffer(&bd, nullptr, &p.cb)) && SUCCEEDED(device->CreateSamplerState(&sd, &p.sampler)) &&
                      SUCCEEDED(device->CreateRasterizerState(&rd, &p.raster)) && SUCCEEDED(device->CreateDepthStencilState(&dd, &p.depth)) &&
                      SUCCEEDED(device->CreateBlendState(&bld, &p.blend));
    p.ready = made;
    if (!made) declineOnce("the draw's shaders or states could not be created");
    return made;
}

// ---- the window ----
struct MoveRequest {
    HWND hwnd;
    int x, y, w, h;
};
VOID CALLBACK moveWindowCallback(PTP_CALLBACK_INSTANCE, PVOID context) {
    std::unique_ptr<MoveRequest> r(static_cast<MoveRequest*>(context));
    if (IsWindow(r->hwnd))
        SetWindowPos(r->hwnd, nullptr, r->x, r->y, r->w, r->h, SWP_ASYNCWINDOWPOS | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
}
// rect is {left, top, right, bottom}. The call returns at once; the move happens on a pool thread and is posted to the window's own thread.
bool requestMove(HWND hwnd, const int rect[4]) {
    MoveRequest* r = new (std::nothrow) MoveRequest{hwnd, rect[0], rect[1], rect[2] - rect[0], rect[3] - rect[1]};
    if (!r) return false;
    if (!TrySubmitThreadpoolCallback(moveWindowCallback, r, nullptr)) {
        delete r;
        return false;
    }
    return true;
}

// Reads the window and, when it is not where the picture's shape wants it, asks for it to be moved. Read-only Win32 calls on the render thread; the move itself is
// the pool thread's. Rate limited (kEvaluateEveryMs reads, kRequestEveryMs requests).
void evaluateWindow(IDXGISwapChain* swap, HWND hwnd, double aspect, uint64_t now) {
    if (S.gaveUp || now - S.lastEvaluateMs < kEvaluateEveryMs) return;
    S.lastEvaluateMs = now;
    if (!IsWindow(hwnd) || IsIconic(hwnd)) return;
    BOOL fullscreen = FALSE;
    if (SUCCEEDED(swap->GetFullscreenState(&fullscreen, nullptr)) && fullscreen) {
        if (!S.noteExclusive) {
            S.noteExclusive = true;
            Log::get().note("desktop window: the swap chain is in exclusive fullscreen; the window is not reshaped (the picture is still drawn, undistorted)");
        }
        return;
    }
    if (IsZoomed(hwnd)) {
        if (!S.noteZoomed) {
            S.noteZoomed = true;
            Log::get().note("desktop window: the window is maximized; it is not reshaped (the picture is still drawn, undistorted)");
        }
        return;
    }
    RECT wr{};
    if (!GetWindowRect(hwnd, &wr)) return;
    const LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE), exStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    MONITORINFO mi{};
    mi.cbSize = sizeof mi;
    const HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    if (!monitor || !GetMonitorInfoW(monitor, &mi)) return;
    const RECT bound = (style & WS_CAPTION) == WS_CAPTION ? mi.rcWork : mi.rcMonitor;
    RECT probe{0, 0, 100, 100};
    if (!AdjustWindowRectEx(&probe, static_cast<DWORD>(style), FALSE, static_cast<DWORD>(exStyle))) return;
    const DwWindowPlan plan = desktopWindowPlan(aspect, bound.left, bound.top, bound.right, bound.bottom, (probe.right - probe.left) - 100, (probe.bottom - probe.top) - 100);
    if (!plan.ok) {
        if (!S.noteShape) {
            S.noteShape = true;
            Log::get().note("desktop window: the monitor's %s cannot hold a window of this shape (aspect %.4f); the window is not reshaped",
                            (style & WS_CAPTION) == WS_CAPTION ? "work area" : "area", aspect);
        }
        return;
    }
    const int cur[4] = {wr.left, wr.top, wr.right, wr.bottom};
    const int want[4] = {plan.left, plan.top, plan.right, plan.bottom};
    if (S.haveTarget) {
        if (desktopWindowRectsNear(cur, S.target, kNearPx)) {
            S.atTargetSeen = true;
            S.unsettled = 0;
        } else if (S.atTargetSeen) {
            // It was at our target and is not: the game (or the user) moved it. Counted; the game wins after too many.
            S.atTargetSeen = false;
            ++S.fights;
            Log::get().note("desktop window: the window was moved off its target (now %d,%d %dx%d); that is %u time(s)", cur[0], cur[1], cur[2] - cur[0], cur[3] - cur[1], S.fights);
            if (S.fights > kFightsAllowed) {
                S.gaveUp = true;
                Log::get().note("desktop window: the game moved the window off its target more than %u times; the window is no longer reshaped this session (the picture "
                                "is still drawn, undistorted)", kFightsAllowed);
                return;
            }
        }
    }
    if (desktopWindowRectsNear(cur, want, kNearPx)) {
        if (!S.haveTarget || !desktopWindowRectsNear(want, S.target, kNearPx)) std::memcpy(S.target, want, sizeof S.target);
        if (S.haveTarget) {
            S.atTargetSeen = true;
            S.unsettled = 0;
        }
        return;
    }
    if (now - S.lastRequestMs < kRequestEveryMs) return;
    if (S.unsettled >= kUnsettledAllowed) {
        S.gaveUp = true;
        Log::get().note("desktop window: %u requests in a row and the window never reached its target; the window is no longer reshaped this session (the picture "
                        "is still drawn, undistorted)", S.unsettled);
        return;
    }
    // The first request of an activation, or the window is not where we last put it: what is there now is the game's.
    if (!S.haveTarget || !desktopWindowRectsNear(cur, S.target, kNearPx)) {
        std::memcpy(S.gameRect, cur, sizeof S.gameRect);
        S.haveGameRect = true;
    }
    std::memcpy(S.target, want, sizeof S.target);
    S.haveTarget = true;
    S.atTargetSeen = false;
    ++S.unsettled;
    S.lastRequestMs = now;
    S.presentsSinceReshape = 0;
    ++S.requests;
    RECT client{};
    GetClientRect(hwnd, &client);
    const bool queued = requestMove(hwnd, want);
    if (S.requests <= kRequestLogCap)
        Log::get().note("desktop window: reshape request %u%s: window %d,%d %dx%d -> %d,%d %dx%d (client %ldx%ld now, %dx%d wanted)%s", S.requests,
                        S.requests == kRequestLogCap ? " (the last one logged)" : "", cur[0], cur[1], cur[2] - cur[0], cur[3] - cur[1], want[0], want[1],
                        want[2] - want[0], want[3] - want[1], client.right - client.left, client.bottom - client.top, plan.clientWidth, plan.clientHeight,
                        queued ? "" : " -- NOT QUEUED (the thread pool refused it)");
}

// Back where Elite had it, if it is still where we put it. Returns the words for the log.
void restoreWindow(char* out, size_t size) {
    if (!S.haveTarget || !S.hwnd || !IsWindow(S.hwnd)) {
        std::snprintf(out, size, "no reshape was made");
    } else {
        RECT wr{};
        GetWindowRect(S.hwnd, &wr);
        const int cur[4] = {wr.left, wr.top, wr.right, wr.bottom};
        if (S.haveGameRect && desktopWindowRectsNear(cur, S.target, kNearPx)) {
            const bool queued = requestMove(S.hwnd, S.gameRect);
            std::snprintf(out, size, "%s to %d,%d %dx%d", queued ? "restored" : "restore NOT QUEUED", S.gameRect[0], S.gameRect[1], S.gameRect[2] - S.gameRect[0],
                          S.gameRect[3] - S.gameRect[1]);
        } else if (S.haveGameRect && desktopWindowRectsNear(cur, S.gameRect, kNearPx)) {
            std::snprintf(out, size, "never left the game's place");
        } else {
            std::snprintf(out, size, "left alone (the game moved it to %d,%d %dx%d)", cur[0], cur[1], cur[2] - cur[0], cur[3] - cur[1]);
        }
    }
    S.haveTarget = false;
    S.haveGameRect = false;
    S.atTargetSeen = false;
    S.unsettled = 0;
}

// The activation ends. Said once, with its reason and what became of the window.
void leave(const char* reason) {
    g_desktopWindowBack.store(nullptr, std::memory_order_relaxed);
    if (!S.active) return;
    S.active = false;
    char window[160];
    restoreWindow(window, sizeof window);
    char line[400];
    desktopWindowStoppedLine(line, sizeof line, reason, S.drawn, S.seen, window);
    Log::get().note("%s", line);
}

// ---- the draw ----
struct SavedState {
    ID3D11RenderTargetView* rtv[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
    ID3D11DepthStencilView* dsv = nullptr;
    ID3D11BlendState* blend = nullptr;
    FLOAT blendFactor[4] = {};
    UINT sampleMask = 0xFFFFFFFFu;
    ID3D11DepthStencilState* depth = nullptr;
    UINT stencilRef = 0;
    ID3D11RasterizerState* raster = nullptr;
    UINT viewportCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    D3D11_VIEWPORT viewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] = {};
    ID3D11InputLayout* layout = nullptr;
    D3D11_PRIMITIVE_TOPOLOGY topology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    ID3D11VertexShader* vs = nullptr;
    ID3D11HullShader* hs = nullptr;
    ID3D11DomainShader* ds = nullptr;
    ID3D11GeometryShader* gs = nullptr;
    ID3D11PixelShader* ps = nullptr;
    ID3D11ShaderResourceView* psSrv = nullptr;
    ID3D11SamplerState* psSampler = nullptr;
    ID3D11Buffer* psCb = nullptr;

    void capture(ID3D11DeviceContext* c) {
        c->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtv, &dsv);
        c->OMGetBlendState(&blend, blendFactor, &sampleMask);
        c->OMGetDepthStencilState(&depth, &stencilRef);
        c->RSGetState(&raster);
        c->RSGetViewports(&viewportCount, viewports);
        c->IAGetInputLayout(&layout);
        c->IAGetPrimitiveTopology(&topology);
        c->VSGetShader(&vs, nullptr, nullptr);
        c->HSGetShader(&hs, nullptr, nullptr);
        c->DSGetShader(&ds, nullptr, nullptr);
        c->GSGetShader(&gs, nullptr, nullptr);
        c->PSGetShader(&ps, nullptr, nullptr);
        c->PSGetShaderResources(0, 1, &psSrv);
        c->PSGetSamplers(0, 1, &psSampler);
        c->PSGetConstantBuffers(0, 1, &psCb);
    }
    // Puts everything back and lets go of every reference taken: nothing of this stays alive past the hook, the buffer's views least of all.
    void restore(ID3D11DeviceContext* c) {
        c->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtv, dsv);
        c->OMSetBlendState(blend, blendFactor, sampleMask);
        c->OMSetDepthStencilState(depth, stencilRef);
        c->RSSetState(raster);
        c->RSSetViewports(viewportCount, viewportCount ? viewports : nullptr);
        c->IASetInputLayout(layout);
        c->IASetPrimitiveTopology(topology);
        c->VSSetShader(vs, nullptr, 0);
        c->HSSetShader(hs, nullptr, 0);
        c->DSSetShader(ds, nullptr, 0);
        c->GSSetShader(gs, nullptr, 0);
        c->PSSetShader(ps, nullptr, 0);
        c->PSSetShaderResources(0, 1, &psSrv);
        c->PSSetSamplers(0, 1, &psSampler);
        c->PSSetConstantBuffers(0, 1, &psCb);
        for (ID3D11RenderTargetView*& v : rtv) { if (v) v->Release(); v = nullptr; }
        if (dsv) dsv->Release();
        if (blend) blend->Release();
        if (depth) depth->Release();
        if (raster) raster->Release();
        if (layout) layout->Release();
        if (vs) vs->Release();
        if (hs) hs->Release();
        if (ds) ds->Release();
        if (gs) gs->Release();
        if (ps) ps->Release();
        if (psSrv) psSrv->Release();
        if (psSampler) psSampler->Release();
        if (psCb) psCb->Release();
        dsv = nullptr; blend = nullptr; depth = nullptr; raster = nullptr; layout = nullptr; vs = nullptr; hs = nullptr; ds = nullptr; gs = nullptr; ps = nullptr;
        psSrv = nullptr; psSampler = nullptr; psCb = nullptr;
    }
};

struct EyeDraw {
    ID3D11ShaderResourceView* view;
    const float* bounds;
    DwPixelRect rect;
};

// Clears the buffer to black and draws the eyes, on Elite's immediate context, under the world route's internal scope so EDVR's own hooks and census step aside.
void drawEyes(ID3D11DeviceContext* ctx, ID3D11RenderTargetView* target, const EyeDraw* eyes, unsigned count) {
    VrWorldInternalScope internal;
    SavedState saved;
    saved.capture(ctx);
    const Pipe& p = S.pipe;
    ctx->OMSetRenderTargets(1, &target, nullptr);
    ctx->OMSetBlendState(p.blend.Get(), nullptr, 0xFFFFFFFFu);
    ctx->OMSetDepthStencilState(p.depth.Get(), 0);
    ctx->RSSetState(p.raster.Get());
    const FLOAT black[4] = {0.f, 0.f, 0.f, 1.f};
    ctx->ClearRenderTargetView(target, black);
    ctx->IASetInputLayout(nullptr);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(p.vs.Get(), nullptr, 0);
    ctx->HSSetShader(nullptr, nullptr, 0);
    ctx->DSSetShader(nullptr, nullptr, 0);
    ctx->GSSetShader(nullptr, nullptr, 0);
    ctx->PSSetShader(p.ps.Get(), nullptr, 0);
    ID3D11SamplerState* sampler = p.sampler.Get();
    ID3D11Buffer* cb = p.cb.Get();
    ctx->PSSetSamplers(0, 1, &sampler);
    ctx->PSSetConstantBuffers(0, 1, &cb);
    for (unsigned i = 0; i < count; ++i) {
        const EyeDraw& e = eyes[i];
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(ctx->Map(cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)) || !mapped.pData) continue;
        float* c = static_cast<float*>(mapped.pData);
        c[0] = e.bounds[0]; c[1] = e.bounds[1]; c[2] = e.bounds[2]; c[3] = e.bounds[3];
        c[4] = (e.bounds[2] - e.bounds[0]) / static_cast<float>(e.rect.w);   // the bounds' span over one destination pixel
        c[5] = (e.bounds[3] - e.bounds[1]) / static_cast<float>(e.rect.h);
        c[6] = c[7] = 0.f;
        ctx->Unmap(cb, 0);
        const D3D11_VIEWPORT vp = {static_cast<FLOAT>(e.rect.x), static_cast<FLOAT>(e.rect.y), static_cast<FLOAT>(e.rect.w), static_cast<FLOAT>(e.rect.h), 0.f, 1.f};
        ctx->RSSetViewports(1, &vp);
        ctx->PSSetShaderResources(0, 1, &e.view);
        ctx->Draw(3, 0);
    }
    ID3D11ShaderResourceView* none = nullptr;
    ctx->PSSetShaderResources(0, 1, &none);   // the eye textures are not left bound
    saved.restore(ctx);
}

enum class Outcome { Drew, Declined };

// The work of one Present while the feature is wanted and the eyes are fresh. Declined: nothing was skipped or drawn this Present, and the reason is in `why`.
Outcome presentActive(IDXGISwapChain* swap, ID3D11Device* device, uint64_t now, char* why, size_t whySize) {
    const unsigned need = desktopWindowEyeMask(S.mode);
    Snap snap;
    snapshot(snap, need);
    // Only the images held now keep a cached view: a texture EDVR has replaced (a new render size, the menu closed) is let go here, not kept alive by the cache.
    for (SrvEntry& e : S.srv) {
        if (e.keep && e.keep.Get() != snap.texture[0].Get() && e.keep.Get() != snap.texture[1].Get()) e = SrvEntry{};
    }
    SrvUse use[2];
    DwSize eyeSize[2];
    for (unsigned eye = 0; eye < 2; ++eye) {
        if (!(need & (1u << eye))) continue;
        if (!snap.texture[eye]) {
            std::snprintf(why, whySize, "no image of the %s eye is held", eye ? "right" : "left");
            return Outcome::Declined;
        }
        use[eye] = srvFor(device, snap.texture[eye].Get());
        if (!use[eye].ok) {
            std::snprintf(why, whySize, "%s", use[eye].reason);
            return Outcome::Declined;
        }
        D3D11_TEXTURE2D_DESC d{};
        snap.texture[eye]->GetDesc(&d);
        eyeSize[eye] = desktopWindowEyeSize(d.Width, d.Height, snap.bounds[eye]);
    }
    double aspect = 0;
    if (!desktopWindowAspect(S.mode, eyeSize, &aspect)) {
        std::snprintf(why, whySize, "the eye image has no size inside its bounds");
        return Outcome::Declined;
    }
    ComPtr<ID3D11DeviceContext> ctx;
    device->GetImmediateContext(&ctx);
    if (!ctx || !ensurePipe(device, ctx.Get())) {
        std::snprintf(why, whySize, "the draw's shaders or states could not be created");
        return Outcome::Declined;
    }
    ComPtr<ID3D11Texture2D> back;
    if (FAILED(swap->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(back.GetAddressOf()))) || !back) {
        std::snprintf(why, whySize, "the swap chain's buffer could not be read");
        return Outcome::Declined;
    }
    D3D11_TEXTURE2D_DESC bd{};
    back->GetDesc(&bd);
    if (bd.SampleDesc.Count != 1) {
        std::snprintf(why, whySize, "the swap chain's buffer is multisampled");
        return Outcome::Declined;
    }
    // One format answer for the eyes the mode draws: the target view must be the same for both.
    DwFormats formats;
    bool haveFormats = false;
    for (unsigned eye = 0; eye < 2; ++eye) {
        if (!(need & (1u << eye))) continue;
        const DwFormats f = desktopWindowFormats(use[eye].source, bd.Format);
        if (!f.ok) {
            std::snprintf(why, whySize, "%s", f.reason);
            return Outcome::Declined;
        }
        if (haveFormats && f.target != formats.target) {
            std::snprintf(why, whySize, "the two eye images need different target formats");
            return Outcome::Declined;
        }
        formats = f;
        haveFormats = true;
    }
    DXGI_SWAP_CHAIN_DESC sc{};
    if (FAILED(swap->GetDesc(&sc)) || !sc.OutputWindow) {
        std::snprintf(why, whySize, "the swap chain's window could not be read");
        return Outcome::Declined;
    }
    const HWND hwnd = sc.OutputWindow;
    RECT cr{};
    GetClientRect(hwnd, &cr);
    const int clientW = cr.right - cr.left, clientH = cr.bottom - cr.top;
    if (clientW <= 0 || clientH <= 0) {
        std::snprintf(why, whySize, "the window's client area is empty (minimized)");
        return Outcome::Declined;
    }
    if (swap != S.scalingChain) {
        S.scalingChain = swap;
        S.scalingKnown = true;
        S.scaling = DwScaling::Stretch;   // a legacy blit-model chain always stretches
        ComPtr<IDXGISwapChain1> chain1;
        DXGI_SWAP_CHAIN_DESC1 d1{};
        if (SUCCEEDED(swap->QueryInterface(IID_PPV_ARGS(&chain1))) && SUCCEEDED(chain1->GetDesc1(&d1)))
            S.scaling = d1.Scaling == DXGI_SCALING_NONE ? DwScaling::None : d1.Scaling == DXGI_SCALING_ASPECT_RATIO_STRETCH ? DwScaling::AspectRatio : DwScaling::Stretch;
    }
    D3D11_RENDER_TARGET_VIEW_DESC rd{};
    rd.Format = formats.target;
    rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    ComPtr<ID3D11RenderTargetView> rtv;
    const HRESULT rtvHr = device->CreateRenderTargetView(back.Get(), &rd, &rtv);
    if (FAILED(rtvHr) || !rtv) {
        std::snprintf(why, whySize, "a target view of the swap chain's buffer could not be made (0x%08X, DXGI format %d as %d)", static_cast<unsigned>(rtvHr),
                      static_cast<int>(bd.Format), static_cast<int>(formats.target));
        return Outcome::Declined;
    }
    const DwRect shown = desktopWindowShown(S.scaling, bd.Width, bd.Height, clientW, clientH, aspect);
    const DwPixelRect shownPx = desktopWindowSnap(shown, static_cast<int>(bd.Width), static_cast<int>(bd.Height));
    DwPixelRect rects[2];
    unsigned eyeIndex[2];
    const unsigned count = desktopWindowEyeRects(S.mode, shownPx, eyeSize, rects, eyeIndex);
    if (!count) {
        std::snprintf(why, whySize, "the picture has no pixels in the swap chain's buffer");
        return Outcome::Declined;
    }

    // ---- committed: from here Elite's draws into the buffer are skipped and ours is made ----
    const bool first = !S.active;
    if (first) {
        S.active = true;
        ++S.activations;
        S.nextStatsMs = now + kTenSecondsMs;
        S.hwnd = hwnd;
        S.chainW = bd.Width;
        S.chainH = bd.Height;
        S.lastSerial[0] = S.lastSerial[1] = 0;
        S.presentsSinceReshape = 0;
        S.lastEvaluateMs = 0;
        if (S.activations <= kActivationLogCap) {
            RECT wr{};
            GetWindowRect(hwnd, &wr);
            MONITORINFO mi{};
            mi.cbSize = sizeof mi;
            const HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
            if (!monitor || !GetMonitorInfoW(monitor, &mi)) mi = MONITORINFO{};
            char eyes[96];
            if (S.mode == DesktopWindowMode::BothEyes)
                std::snprintf(eyes, sizeof eyes, "eyes %.0fx%.0f and %.0fx%.0f", eyeSize[0].w, eyeSize[0].h, eyeSize[1].w, eyeSize[1].h);
            else
                std::snprintf(eyes, sizeof eyes, "%s eye %.0fx%.0f", S.mode == DesktopWindowMode::LeftEye ? "left" : "right", eyeSize[eyeIndex[0]].w, eyeSize[eyeIndex[0]].h);
            Log::get().note("desktop window: active #%u (%s): %s, aspect %.4f; swap chain %ux%u DXGI format %d swap effect %d scaling %s buffers %u windowed %d; "
                            "window style 0x%08lX exstyle 0x%08lX rect %ld,%ld %ldx%ld client %dx%d; monitor %ld,%ld,%ld,%ld work %ld,%ld,%ld,%ld",
                            S.activations, desktopWindowModeName(S.mode), eyes, aspect, bd.Width, bd.Height, static_cast<int>(sc.BufferDesc.Format),
                            static_cast<int>(sc.SwapEffect), S.scaling == DwScaling::None ? "none" : S.scaling == DwScaling::AspectRatio ? "aspect_ratio_stretch" : "stretch",
                            sc.BufferCount, sc.Windowed ? 1 : 0, static_cast<unsigned long>(GetWindowLongPtrW(hwnd, GWL_STYLE)),
                            static_cast<unsigned long>(GetWindowLongPtrW(hwnd, GWL_EXSTYLE)), wr.left, wr.top, wr.right - wr.left, wr.bottom - wr.top, clientW, clientH,
                            mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right, mi.rcMonitor.bottom, mi.rcWork.left, mi.rcWork.top, mi.rcWork.right, mi.rcWork.bottom);
        }
    }
    if (bd.Width != S.chainW || bd.Height != S.chainH) {
        Log::get().note("desktop window: swap chain now %ux%u (was %ux%u); %llu Presents since the last reshape request", bd.Width, bd.Height, S.chainW, S.chainH,
                        static_cast<unsigned long long>(S.presentsSinceReshape));
        S.chainW = bd.Width;
        S.chainH = bd.Height;
    }
    S.clientW = clientW;
    S.clientH = clientH;
    g_desktopWindowBack.store(back.Get(), std::memory_order_relaxed);   // identity for the draw skip; the reference below is released before the hook returns
    ++S.presentsSinceReshape;
    evaluateWindow(swap, hwnd, aspect, now);

    bool stale = false;
    for (unsigned eye = 0; eye < 2; ++eye) {
        if (!(need & (1u << eye))) continue;
        if (snap.serial[eye] == S.lastSerial[eye]) stale = true;
        S.lastSerial[eye] = snap.serial[eye];
    }
    if (stale) ++S.stale;

    EyeDraw draws[2];
    for (unsigned i = 0; i < count; ++i) {
        draws[i].view = use[eyeIndex[i]].view.Get();
        draws[i].bounds = snap.bounds[eyeIndex[i]];
        draws[i].rect = rects[i];
    }
    drawEyes(ctx.Get(), rtv.Get(), draws, count);
    ++S.drawn;
    return Outcome::Drew;
}

}  // namespace

void desktopWindowNoteSkippedDraw() { g_skipped.fetch_add(1, std::memory_order_relaxed); }

bool desktopWindowPresent(IDXGISwapChain* swap, ID3D11Device* device, unsigned presentFlags, bool flatProfile) {
    if (presentFlags & DXGI_PRESENT_TEST) return false;
    if (S.dead) return false;
    drainRetired();
    refreshMode(flatProfile);
    const unsigned need = flatProfile ? 0u : desktopWindowEyeMask(S.mode);
    g_wantMask.store(need, std::memory_order_relaxed);
    if (!need && !S.active && !g_holding.load(std::memory_order_relaxed)) return false;

    const uint64_t now = stampMs();
    bool fresh = need != 0;
    for (unsigned eye = 0; eye < 2 && fresh; ++eye) {
        if (!(need & (1u << eye))) continue;
        const uint64_t tick = g_lastTick[eye].load(std::memory_order_acquire);
        if (!tick || (tick < now ? now - tick : 0) > kDesktopWindowStaleMs) fresh = false;
    }
    char why[200] = {};
    bool drew = false;
    if (need && fresh && swap && device) {
        if (graphicsRuntimeDisabled() || !g_budget.shouldRun()) {
            S.dead = true;
            leave(graphicsRuntimeDisabled() ? "the graphics runtime is disabled" : "stood down after faults in the draw");
            releaseHeld();
            return true;
        }
        ++S.seen;
        Outcome outcome = Outcome::Declined;
        if (!guardedBudget(g_budget, [&] { outcome = presentActive(swap, device, now, why, sizeof why); })) {
            std::snprintf(why, sizeof why, "a fault in the draw");
            outcome = Outcome::Declined;
        }
        drew = outcome == Outcome::Drew;
        if (!drew) {
            if (S.active) {
                leave(why);
            } else {
                declineOnce(why);
            }
        }
    }
    if (drew) {
        if (now >= S.nextStatsMs) {
            S.nextStatsMs = now + kTenSecondsMs;
            char line[320];
            desktopWindowStatsLine(line, sizeof line, S.drawn, S.seen, g_skipped.load(std::memory_order_relaxed), g_handoffs[0].load(std::memory_order_relaxed),
                                   g_handoffs[1].load(std::memory_order_relaxed), S.stale, S.clientW, S.clientH, S.chainW, S.chainH);
            Log::get().note("%s", line);
        }
        return true;
    }
    // Not drawing: nothing is skipped any more, the window goes back, and the held images are let go.
    const bool wasActive = S.active;
    if (S.active) {
        leave(!need ? (flatProfile ? "the flat profile is active" : "the setting is now game") : "no eye image for 500 ms (3D is off, or no frames arrive)");
    } else {
        g_desktopWindowBack.store(nullptr, std::memory_order_relaxed);
    }
    if (g_holding.load(std::memory_order_relaxed) && (!need || !fresh)) releaseHeld();
    return wasActive || need != 0;
}

}  // namespace edvr

extern "C" HRESULT WINAPI edvrAcquireDesktopWindow(const EdvrNativeDesktopWindowRequest* request, EdvrNativeDesktopWindowTable* table) {
    if (!table || table->size != sizeof(*table) || table->version != EDVR_NATIVE_DESKTOP_WINDOW_VERSION_1) return E_INVALIDARG;
    *table = {sizeof(*table), EDVR_NATIVE_DESKTOP_WINDOW_VERSION_1};
    if (!request || request->size != sizeof(*request) || request->version != EDVR_NATIVE_DESKTOP_WINDOW_VERSION_1 || !request->gameDevice ||
        !request->generation)
        return E_INVALIDARG;
    table->context = &edvr::g_contextToken;
    table->noteEye = edvr::noteEye;
    table->close = edvr::closeProvider;
    return S_OK;
}
