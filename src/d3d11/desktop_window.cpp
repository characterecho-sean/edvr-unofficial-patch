// visual.desktop_window: see desktop_window.h for the shape of the feature and docs\desktop-window.md for the design and what the first flight must show.
//
// Threads. noteEye (the handoff) runs on the XR owner thread and only takes the mutex, swaps a reference and stamps a tick. Everything else runs on the render
// thread, in Elite's Present hook, before the real Present: the config read, the window checks, the draw, the log lines. The window's style and rectangle are changed from
// a thread-pool callback with SWP_ASYNCWINDOWPOS, never from the hook: Elite's window procedure could otherwise run re-entrantly inside Present. The skip census
// (desktopWindowNoteSkippedDraw) is written by the draw hooks, which are the same thread.
#include "temporal_shader_bytecode.h"
#include "desktop_window.h"
#include "desktop_window_cursor.h"

#include <windows.h>

#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <string>

#include "../common/config.h"
#include "../common/desktop_window_census.h"
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
    float fov[4] = {};          // {angleLeft, angleRight, angleUp, angleDown}, radians: the field of view the pixels cover (version 2 of the export)
    bool haveFov = false;
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
DesktopWindowCensus g_census;                  // the skipped draws of the current 10 s window; render thread only (desktop_window_census.h)
FaultBudget g_sampleBudget("desktopWindowSample", 3);
int g_contextToken = 0;                        // the table's context: any address that is ours

HRESULT noteEyeWith(void* context, uint32_t eye, ID3D11Texture2D* texture, const float* bounds, const float* fov) {
    if (context != &g_contextToken || eye > 1 || !texture || !bounds) return E_INVALIDARG;
    for (int i = 0; i < 4; ++i) {
        if (!std::isfinite(bounds[i])) return E_INVALIDARG;
    }
    bool haveFov = fov != nullptr;
    for (int i = 0; haveFov && i < 4; ++i) haveFov = std::isfinite(fov[i]);
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
        h.haveFov = haveFov;
        if (haveFov) std::memcpy(h.fov, fov, sizeof h.fov);
        h.serial = ++g_serial;
        g_retirePending.store(g_retiredCount, std::memory_order_relaxed);
    }
    g_holding.store(true, std::memory_order_relaxed);
    g_lastTick[eye].store(stampMs(), std::memory_order_release);
    g_handoffs[eye].fetch_add(1, std::memory_order_relaxed);
    if (spill) spill->Release();
    return S_OK;
}

// Version 1 of the export: no field of view. Version 2: with it.
HRESULT WINAPI noteEye(void* context, uint32_t eye, ID3D11Texture2D* texture, const float* bounds) { return noteEyeWith(context, eye, texture, bounds, nullptr); }
HRESULT WINAPI noteEye2(void* context, uint32_t eye, ID3D11Texture2D* texture, const float* bounds, const float* fov) { return noteEyeWith(context, eye, texture, bounds, fov); }

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
    // round 6: the picture's shape and the window's style, as the settings say now, and as the activation's window state was built for
    DesktopWindowAspect aspectOpt = DesktopWindowAspect::Full, activeAspect = DesktopWindowAspect::Full;
    DesktopWindowStyle styleOpt = DesktopWindowStyle::Window, activeStyle = DesktopWindowStyle::Window;
    bool optionsLogged = false;
    bool keepHeightOnce = false;        // the picture's shape changed with the window untouched: the next correction keeps the client height
    bool noteNoFov = false;
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
    bool haveGame = false;                       // the game's style and rect, remembered at the first look of an activation, to put back
    int gameRect[4] = {};
    LONG_PTR gameStyle = 0, gameExStyle = 0;
    LONG_PTR ourStyle = 0;                       // desktopWindowStyleFor(activeStyle, the game's)
    bool styleSeenOurs = false;                  // our style has been seen on the window since it was last asked for; losing it after that is the game's doing
    bool pending = false, pendingStyle = false;  // a request of ours is in flight (and it asked for the style)
    int target[4] = {};                          // the rectangle of the request in flight
    bool haveSettled = false;
    int settledRect[4] = {};                     // the window's rect when its client last had the picture's shape: where a later activation starts
    int settledClient[2] = {};                   // its client size then: what the next resize is measured against
    bool havePrev = false;
    int prevRect[4] = {};                        // the rect at the last look, to see the user's moves and resizes
    double aspect = 0;                           // the picture's shape at the last Present
    unsigned requests = 0, fights = 0, unsettled = 0;
    unsigned userMoves = 0, userResizes = 0;     // seen since the last 10 s line
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
        S.aspectOpt = DesktopWindowAspect::Full;
        S.styleOpt = DesktopWindowStyle::Window;
        return;
    }
    {   // round 6: the picture's shape and the window's style, live like the mode; each change (and the first read) is one line
        const std::string rawAspect = cfg.getString("visual.desktop_window_aspect", "full");
        const std::string rawStyle = cfg.getString("visual.desktop_window_style", "window");
        bool aspectKnown = true, styleKnown = true;
        const DesktopWindowAspect aspectOpt = desktopWindowParseAspect(rawAspect.c_str(), &aspectKnown);
        const DesktopWindowStyle styleOpt = desktopWindowParseStyle(rawStyle.c_str(), &styleKnown);
        if (!aspectKnown) Log::get().note("desktop window: unknown aspect \"%.40s\" (it is full, 1:1, 4:3, 16:10, 16:9 or 21:9); treated as full", rawAspect.c_str());
        if (!styleKnown) Log::get().note("desktop window: unknown style \"%.40s\" (it is window or borderless); treated as window", rawStyle.c_str());
        if (!S.optionsLogged || aspectOpt != S.aspectOpt) {
            if (S.optionsLogged) Log::get().note("desktop window: aspect %s (was %s)", desktopWindowAspectName(aspectOpt), desktopWindowAspectName(S.aspectOpt));
            else Log::get().note("desktop window: aspect %s", desktopWindowAspectName(aspectOpt));
        }
        if (!S.optionsLogged || styleOpt != S.styleOpt) {
            if (S.optionsLogged) Log::get().note("desktop window: style %s (was %s)", desktopWindowStyleName(styleOpt), desktopWindowStyleName(S.styleOpt));
            else Log::get().note("desktop window: style %s", desktopWindowStyleName(styleOpt));
        }
        S.optionsLogged = true;
        S.aspectOpt = aspectOpt;
        S.styleOpt = styleOpt;
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
    float fov[2][4] = {};
    bool haveFov[2] = {};
    uint64_t serial[2] = {};
};
void snapshot(Snap& s, unsigned need) {
    std::lock_guard<std::mutex> lock(g_mutex);
    for (unsigned eye = 0; eye < 2; ++eye) {
        if (!(need & (1u << eye)) || !g_held[eye].texture) continue;
        s.texture[eye] = g_held[eye].texture;   // ComPtr copy: its own reference
        std::memcpy(s.bounds[eye], g_held[eye].bounds, sizeof s.bounds[eye]);
        std::memcpy(s.fov[eye], g_held[eye].fov, sizeof s.fov[eye]);
        s.haveFov[eye] = g_held[eye].haveFov;
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
// The window is an ordinary one while the feature is active (docs\desktop-window.md, "The window"): framed and sizable, moved and resized by the user as they like. EDVR
// keeps only the picture's shape on it, and the draw keeps the picture undistorted (with bars) in between. A request changes the style (SetWindowLongPtrW, then
// SWP_FRAMECHANGED) and the rectangle together, from a pool thread; it is posted to the window's own thread and never made from the Present hook.
struct MoveRequest {
    HWND hwnd;
    int x, y, w, h;
    bool setStyle;
    LONG_PTR style;
};
VOID CALLBACK moveWindowCallback(PTP_CALLBACK_INSTANCE, PVOID context) {
    std::unique_ptr<MoveRequest> r(static_cast<MoveRequest*>(context));
    if (!IsWindow(r->hwnd)) return;
    UINT flags = SWP_ASYNCWINDOWPOS | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER;
    if (r->setStyle) {
        SetWindowLongPtrW(r->hwnd, GWL_STYLE, r->style);
        flags |= SWP_FRAMECHANGED;
    }
    SetWindowPos(r->hwnd, nullptr, r->x, r->y, r->w, r->h, flags);
}
// rect is {left, top, right, bottom}. The call returns at once; the change happens on a pool thread.
bool requestWindow(HWND hwnd, const int rect[4], bool setStyle, LONG_PTR style) {
    MoveRequest* r = new (std::nothrow) MoveRequest{hwnd, rect[0], rect[1], rect[2] - rect[0], rect[3] - rect[1], setStyle, style};
    if (!r) return false;
    if (!TrySubmitThreadpoolCallback(moveWindowCallback, r, nullptr)) {
        delete r;
        return false;
    }
    return true;
}

// What a style adds around a client (the window's size less its client's), asked of the system for the window's DPI where user32 has the call (Windows 10 1607), else the
// plain one. Used only before our style is on the window; after that the frame is measured from the window itself.
bool predictFrame(HWND hwnd, LONG_PTR style, LONG_PTR exStyle, int* dx, int* dy) {
    using AdjustForDpi = BOOL(WINAPI*)(LPRECT, DWORD, BOOL, DWORD, UINT);
    using DpiForWindow = UINT(WINAPI*)(HWND);
    static AdjustForDpi adjust = nullptr;
    static DpiForWindow dpiOf = nullptr;
    static bool resolved = false;
    if (!resolved) {
        resolved = true;
        if (const HMODULE user = GetModuleHandleW(L"user32.dll")) {
            adjust = reinterpret_cast<AdjustForDpi>(GetProcAddress(user, "AdjustWindowRectExForDpi"));
            dpiOf = reinterpret_cast<DpiForWindow>(GetProcAddress(user, "GetDpiForWindow"));
        }
    }
    RECT probe{0, 0, 100, 100};
    const UINT dpi = dpiOf ? dpiOf(hwnd) : 0;
    const BOOL ok = adjust && dpi ? adjust(&probe, static_cast<DWORD>(style), FALSE, static_cast<DWORD>(exStyle), dpi)
                                  : AdjustWindowRectEx(&probe, static_cast<DWORD>(style), FALSE, static_cast<DWORD>(exStyle));
    if (!ok) return false;
    *dx = (probe.right - probe.left) - 100;
    *dy = (probe.bottom - probe.top) - 100;
    return true;
}

// The window's thread is in a move or size loop: the user has hold of it.
bool windowInMoveSize(HWND hwnd) {
    const DWORD thread = GetWindowThreadProcessId(hwnd, nullptr);
    GUITHREADINFO info{};
    info.cbSize = sizeof info;
    return thread && GetGUIThreadInfo(thread, &info) && (info.flags & GUI_INMOVESIZE) != 0;
}

DwArea areaOf(const RECT& r) {
    DwArea a;
    a.left = r.left; a.top = r.top; a.right = r.right; a.bottom = r.bottom;
    return a;
}

struct Ask {
    int rect[4] = {};
    int clientW = 0, clientH = 0;       // the client the rect is meant to give
    bool setStyle = false;
    LONG_PTR fromStyle = 0, toStyle = 0;
    const char* why = "";
};
// Makes the request, remembers it as in flight, says so (the first kRequestLogCap).
void issue(HWND hwnd, const int cur[4], int curClientW, int curClientH, const Ask& a, uint64_t now) {
    std::memcpy(S.target, a.rect, sizeof S.target);
    S.pending = true;
    S.pendingStyle = a.setStyle;
    if (a.setStyle) S.styleSeenOurs = false;   // asked for; it counts as seen once it is observed on the window
    S.lastRequestMs = now;
    S.presentsSinceReshape = 0;
    ++S.requests;
    const bool queued = requestWindow(hwnd, a.rect, a.setStyle, a.toStyle);
    if (S.requests <= kRequestLogCap)
        Log::get().note("desktop window: reshape request %u%s (%s): window %d,%d %dx%d -> %d,%d %dx%d (client %dx%d now, %dx%d wanted); style 0x%08lX -> 0x%08lX; "
                        "aspect %s, %s%s", S.requests,
                        S.requests == kRequestLogCap ? " (the last one logged)" : "", a.why, cur[0], cur[1], cur[2] - cur[0], cur[3] - cur[1], a.rect[0], a.rect[1],
                        a.rect[2] - a.rect[0], a.rect[3] - a.rect[1], curClientW, curClientH, a.clientW, a.clientH, static_cast<unsigned long>(a.fromStyle),
                        static_cast<unsigned long>(a.toStyle), desktopWindowAspectName(S.activeAspect), desktopWindowStyleName(S.activeStyle),
                        queued ? "" : " -- NOT QUEUED (the thread pool refused it)");
}

// Where the (framed) window starts: where it last had the picture's shape this session, if that still lies inside a monitor's work area, reshaped to the current shape keeping
// its client height and clamped to the work area (round 6; with the shape unchanged that is the same window); else the largest window of the picture's shape the work area of
// the window's monitor holds, centred. False when nothing fits.
bool startRect(const RECT& work, double aspect, int frameDx, int frameDy, Ask& out) {
    if (S.haveSettled) {
        const RECT r = {S.settledRect[0], S.settledRect[1], S.settledRect[2], S.settledRect[3]};
        MONITORINFO mi{};
        mi.cbSize = sizeof mi;
        const HMONITOR monitor = MonitorFromRect(&r, MONITOR_DEFAULTTONEAREST);
        if (monitor && GetMonitorInfoW(monitor, &mi) && desktopWindowRectInside(S.settledRect, areaOf(mi.rcWork))) {
            // The frame it had (the window's size less its client's) is measured, not predicted: it is the same style.
            const int settledDx = S.settledRect[2] - S.settledRect[0] - S.settledClient[0], settledDy = S.settledRect[3] - S.settledRect[1] - S.settledClient[1];
            const DwCorrection fit = desktopWindowCorrect(aspect, S.settledRect[0], S.settledRect[1], S.settledClient[0], S.settledClient[1], 0, 0, settledDx, settledDy,
                                                          areaOf(mi.rcWork), kDesktopWindowMinClientHeight, true);
            if (fit.ok) {
                out.rect[0] = fit.left; out.rect[1] = fit.top; out.rect[2] = fit.right; out.rect[3] = fit.bottom;
                out.clientW = fit.clientWidth;
                out.clientH = fit.clientHeight;
                return true;
            }
        }
    }
    const DwWindowPlan plan = desktopWindowPlan(aspect, work.left, work.top, work.right, work.bottom, frameDx, frameDy);
    if (!plan.ok) return false;
    out.rect[0] = plan.left; out.rect[1] = plan.top; out.rect[2] = plan.right; out.rect[3] = plan.bottom;
    out.clientW = plan.clientWidth;
    out.clientH = plan.clientHeight;
    return true;
}

// Reads the window and, when it needs it, asks for a change. Read-only Win32 calls on the render thread; the change itself is the pool thread's. Reads at most every
// kEvaluateEveryMs, requests at most every kRequestEveryMs. The rules (docs\desktop-window.md):
//   the user in a move or size loop: nothing; our style missing: ask for it (a fight if it was there and went); the client off the picture's shape, or the window larger
//   than the work area: ask for the dimension the user changed least to follow the other; a position alone is never corrected; a settled window is remembered.
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
    if (windowInMoveSize(hwnd)) return;   // the user has hold of it: the draw keeps the picture undistorted, with bars
    RECT wr{}, cr{};
    if (!GetWindowRect(hwnd, &wr) || !GetClientRect(hwnd, &cr)) return;
    const LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE), exStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    MONITORINFO mi{};
    mi.cbSize = sizeof mi;
    const HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    if (!monitor || !GetMonitorInfoW(monitor, &mi)) return;
    const DwArea work = areaOf(mi.rcWork);
    const int cur[4] = {wr.left, wr.top, wr.right, wr.bottom};
    const int clientW = cr.right - cr.left, clientH = cr.bottom - cr.top;
    const bool first = !S.haveGame;
    if (first) {   // the game's own window, remembered to put back
        std::memcpy(S.gameRect, cur, sizeof S.gameRect);
        S.gameStyle = style;
        S.gameExStyle = exStyle;
        S.activeStyle = S.styleOpt;
        S.activeAspect = S.aspectOpt;
        S.ourStyle = static_cast<LONG_PTR>(desktopWindowStyleFor(S.activeStyle, static_cast<uint32_t>(style)));
        S.haveGame = true;
    } else {
        // A live change of the style or the shape is not a fight (round 6): our style is worked out again for the new target, what the fight detection knew is forgotten, and the
        // change is asked for now. A shape change alone keeps the window's client height and top-left.
        if (S.activeStyle != S.styleOpt) {
            S.activeStyle = S.styleOpt;
            S.ourStyle = static_cast<LONG_PTR>(desktopWindowStyleFor(S.activeStyle, static_cast<uint32_t>(S.gameStyle)));
            S.styleSeenOurs = false;
            S.pending = false;
            S.pendingStyle = false;
            S.havePrev = false;
            S.unsettled = 0;
            S.lastRequestMs = 0;
        }
        if (S.activeAspect != S.aspectOpt) {
            S.activeAspect = S.aspectOpt;
            S.keepHeightOnce = true;
            S.pending = false;
            S.pendingStyle = false;
            S.lastRequestMs = 0;
        }
    }
    const bool borderless = S.activeStyle == DesktopWindowStyle::Borderless;
    const bool ours = desktopWindowStyleIsOurs(static_cast<uint32_t>(style), static_cast<uint32_t>(S.ourStyle));

    // A request of ours in flight: did it land? One that has not after kRequestEveryMs did not.
    if (S.pending) {
        if (desktopWindowRectsNear(cur, S.target, kNearPx) && (!S.pendingStyle || ours)) {
            S.pending = false;
            S.pendingStyle = false;
            S.unsettled = 0;
            if (ours) S.styleSeenOurs = true;
            std::memcpy(S.prevRect, cur, sizeof S.prevRect);   // our own change is not the user's
            S.havePrev = true;
        } else if (now - S.lastRequestMs >= kRequestEveryMs) {
            S.pending = false;
            S.pendingStyle = false;
            if (++S.unsettled >= kUnsettledAllowed) {
                S.gaveUp = true;
                Log::get().note("desktop window: %u requests in a row did not land; the window is no longer reshaped this session (the picture is still drawn, undistorted)",
                                S.unsettled);
                return;
            }
        } else {
            return;   // give it time
        }
    }

    // The style. Losing ours after it was there is the game re-applying its own mode: a fight, counted, and the game wins after too many.
    if (S.styleSeenOurs && !ours) {
        S.styleSeenOurs = false;
        S.havePrev = false;
        ++S.fights;
        Log::get().note("desktop window: the game changed the window's style away from ours (now 0x%08lX); that is %u time(s)", static_cast<unsigned long>(style), S.fights);
        if (S.fights > kFightsAllowed) {
            S.gaveUp = true;
            Log::get().note("desktop window: the game changed the style back more than %u times; the window is no longer reshaped this session (the picture is still drawn, "
                            "undistorted)", kFightsAllowed);
            return;
        }
    }
    if (!ours) {
        if (now - S.lastRequestMs < kRequestEveryMs) return;
        Ask ask;
        ask.setStyle = true;
        ask.fromStyle = style;
        ask.toStyle = S.ourStyle;
        S.keepHeightOnce = false;   // the style request carries its own rectangle (a shape change alone is for the correction below)
        if (borderless) {   // the whole monitor the window is on, no frame
            const DwWindowPlan full = desktopWindowBorderlessPlan(mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right, mi.rcMonitor.bottom);
            if (!full.ok) return;
            ask.rect[0] = full.left; ask.rect[1] = full.top; ask.rect[2] = full.right; ask.rect[3] = full.bottom;
            ask.clientW = full.clientWidth;
            ask.clientH = full.clientHeight;
            ask.why = S.fights ? "the game's style came back" : "full-screen borderless";
        } else {
            int frameDx = 0, frameDy = 0;
            if (!predictFrame(hwnd, S.ourStyle, exStyle, &frameDx, &frameDy)) return;
            ask.why = S.fights ? "the game's style came back" : "framed window";
            if (!startRect(mi.rcWork, aspect, frameDx, frameDy, ask)) {
                if (!S.noteShape) {
                    S.noteShape = true;
                    Log::get().note("desktop window: the monitor's work area cannot hold a window of this shape (aspect %.4f); the window is not reshaped", aspect);
                }
                return;
            }
        }
        issue(hwnd, cur, clientW, clientH, ask, now);
        return;
    }
    S.styleSeenOurs = true;

    // Full-screen borderless (round 6): the whole monitor the window is on. No snapping and no user moves; the picture is fitted inside with black bars, which the draw does.
    if (borderless) {
        const DwWindowPlan full = desktopWindowBorderlessPlan(mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right, mi.rcMonitor.bottom);
        std::memcpy(S.prevRect, cur, sizeof S.prevRect);
        S.havePrev = true;
        S.keepHeightOnce = false;   // a shape change meant for the framed window does not outlive it
        const int want[4] = {full.left, full.top, full.right, full.bottom};
        if (!full.ok || desktopWindowRectsNear(cur, want, kNearPx) || now - S.lastRequestMs < kRequestEveryMs) return;
        Ask ask;
        std::memcpy(ask.rect, want, sizeof ask.rect);
        ask.clientW = full.clientWidth;
        ask.clientH = full.clientHeight;
        ask.fromStyle = ask.toStyle = style;
        ask.why = "the monitor's rectangle";
        issue(hwnd, cur, clientW, clientH, ask, now);
        return;
    }

    // Ours. The user's moves and resizes, counted (a drag is one: the loop above kept the rect from changing under us); a position alone is never corrected.
    if (S.havePrev && !desktopWindowRectsNear(cur, S.prevRect, kNearPx)) {
        const bool resized = std::abs((cur[2] - cur[0]) - (S.prevRect[2] - S.prevRect[0])) > kNearPx || std::abs((cur[3] - cur[1]) - (S.prevRect[3] - S.prevRect[1])) > kNearPx;
        if (resized) ++S.userResizes;
        else ++S.userMoves;
    }
    std::memcpy(S.prevRect, cur, sizeof S.prevRect);
    S.havePrev = true;
    const int outerW = cur[2] - cur[0], outerH = cur[3] - cur[1];
    const bool off = desktopWindowAspectOff(clientW, clientH, aspect, kNearPx);
    const bool tooLarge = desktopWindowTooLarge(outerW, outerH, work);
    if (!off && !tooLarge) {   // settled: where a later activation starts, and the client size the next resize is measured against
        std::memcpy(S.settledRect, cur, sizeof S.settledRect);
        S.settledClient[0] = clientW;
        S.settledClient[1] = clientH;
        S.haveSettled = true;
        return;
    }
    if (now - S.lastRequestMs < kRequestEveryMs) return;
    const DwCorrection fix = desktopWindowCorrect(aspect, cur[0], cur[1], clientW, clientH, S.haveSettled ? S.settledClient[0] : 0, S.haveSettled ? S.settledClient[1] : 0,
                                                  outerW - clientW, outerH - clientH, work, kDesktopWindowMinClientHeight, S.keepHeightOnce);
    if (!fix.ok) return;
    Ask ask;
    ask.rect[0] = fix.left; ask.rect[1] = fix.top; ask.rect[2] = fix.right; ask.rect[3] = fix.bottom;
    ask.clientW = fix.clientWidth;
    ask.clientH = fix.clientHeight;
    ask.fromStyle = ask.toStyle = style;
    ask.why = off ? (fix.keptHeight ? "the shape, keeping the height" : "the shape, keeping the width") : "larger than the work area";
    S.keepHeightOnce = false;   // asked for (or nothing to ask): the next correction is the user's again
    if (desktopWindowRectsNear(cur, ask.rect, kNearPx)) return;   // the clamp leaves it where it is
    issue(hwnd, cur, clientW, clientH, ask, now);
}

// Back where Elite had it, if the style is still ours. Returns the words for the log.
void restoreWindow(char* out, size_t size) {
    if (!S.haveGame || !S.hwnd || !IsWindow(S.hwnd)) {
        std::snprintf(out, size, "no reshape was made");
    } else {
        RECT wr{}, cr{};
        GetWindowRect(S.hwnd, &wr);
        GetClientRect(S.hwnd, &cr);
        const LONG_PTR style = GetWindowLongPtrW(S.hwnd, GWL_STYLE);
        const int cur[4] = {wr.left, wr.top, wr.right, wr.bottom};
        const bool ours = desktopWindowStyleIsOurs(static_cast<uint32_t>(style), static_cast<uint32_t>(S.ourStyle));
        const bool gamesOwn = desktopWindowStyleIsOurs(static_cast<uint32_t>(style), static_cast<uint32_t>(S.gameStyle));
        if (ours) {
            // Where a later activation starts, if the window was at the picture's shape just now.
            if (S.activeStyle == DesktopWindowStyle::Window && !desktopWindowAspectOff(cr.right - cr.left, cr.bottom - cr.top, S.aspect, kNearPx) && !windowInMoveSize(S.hwnd)) {
                std::memcpy(S.settledRect, cur, sizeof S.settledRect);
                S.haveSettled = true;
            }
            const bool setStyle = !desktopWindowStyleIsOurs(static_cast<uint32_t>(S.gameStyle), static_cast<uint32_t>(S.ourStyle));
            const bool queued = requestWindow(S.hwnd, S.gameRect, setStyle, S.gameStyle);
            std::snprintf(out, size, "%s to %d,%d %dx%d, style 0x%08lX%s", queued ? "restored" : "restore NOT QUEUED", S.gameRect[0], S.gameRect[1],
                          S.gameRect[2] - S.gameRect[0], S.gameRect[3] - S.gameRect[1], static_cast<unsigned long>(S.gameStyle), setStyle ? " back" : "");
        } else if (gamesOwn && desktopWindowRectsNear(cur, S.gameRect, kNearPx)) {
            std::snprintf(out, size, "never left the game's place");
        } else {
            std::snprintf(out, size, "left alone (the game changed the style itself: now 0x%08lX, ours was 0x%08lX; window %d,%d %dx%d)", static_cast<unsigned long>(style),
                          static_cast<unsigned long>(S.ourStyle), cur[0], cur[1], cur[2] - cur[0], cur[3] - cur[1]);
        }
    }
    S.haveGame = false;
    S.pending = false;
    S.pendingStyle = false;
    S.styleSeenOurs = false;
    S.havePrev = false;
    S.unsettled = 0;
}

// The activation ends. Said once, with its reason and what became of the window.
void leave(const char* reason) {
    g_desktopWindowBack.store(nullptr, std::memory_order_relaxed);
    desktopWindowCursorSetActive(false);   // the cursor hooks pass everything through again
    if (!S.active) return;
    S.active = false;
    char window[200];
    restoreWindow(window, sizeof window);
    char line[500];
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
    uint32_t texW[2] = {}, texH[2] = {};
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
        texW[eye] = d.Width;
        texH[eye] = d.Height;
    }
    // The picture's shape (round 6). `full`: each eye as its bounds sub-rect is. Any other value: the whole picture's shape (each eye half of it with both eyes), and each
    // eye is cropped to its shape, centred on where it looks straight ahead and moved only as far as it takes to stay inside the image.
    DwSize shape[2];
    desktopWindowShapes(S.aspectOpt, S.mode, eyeSize, shape);
    double aspect = 0;
    if (!desktopWindowAspect(S.mode, shape, &aspect)) {
        std::snprintf(why, whySize, "the eye image has no size inside its bounds");
        return Outcome::Declined;
    }
    const double ratio = desktopWindowAspectRatio(S.aspectOpt);
    float cropBounds[2][4] = {};
    double centreU[2] = {0.5, 0.5}, centreV[2] = {0.5, 0.5};
    bool fromFov[2] = {};
    for (unsigned eye = 0; eye < 2; ++eye) {
        if (!(need & (1u << eye))) continue;
        if (ratio > 0) {
            fromFov[eye] = snap.haveFov[eye] && desktopWindowProjectionCentre(snap.fov[eye], &centreU[eye], &centreV[eye]);
            if (!fromFov[eye]) {
                centreU[eye] = centreV[eye] = 0.5;
                if (!S.noteNoFov) {
                    S.noteNoFov = true;
                    Log::get().note("desktop window: an eye image came with no field of view; its crop is centred on the middle of its bounds");
                }
            }
        }
        desktopWindowCrop(snap.bounds[eye], texW[eye], texH[eye], ratio > 0 ? (S.mode == DesktopWindowMode::BothEyes ? ratio * 0.5 : ratio) : 0.0, fromFov[eye], centreU[eye],
                          centreV[eye], cropBounds[eye]);
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
    const unsigned count = desktopWindowEyeRects(S.mode, shownPx, shape, rects, eyeIndex);
    if (!count) {
        std::snprintf(why, whySize, "the picture has no pixels in the swap chain's buffer");
        return Outcome::Declined;
    }

    // ---- committed: from here Elite's draws into the buffer are skipped and ours is made ----
    const bool first = !S.active;
    if (first) {
        S.active = true;
        desktopWindowCursorSetActive(true);   // the cursor hide is allowed from here on (the game's mouse lock is left as it is)
        ++S.activations;
        S.nextStatsMs = now + kTenSecondsMs;
        S.hwnd = hwnd;
        S.chainW = bd.Width;
        S.chainH = bd.Height;
        S.lastSerial[0] = S.lastSerial[1] = 0;
        S.presentsSinceReshape = 0;
        S.lastEvaluateMs = 0;
        S.userMoves = S.userResizes = 0;
        g_census.reset();
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
            Log::get().note("desktop window: active #%u (%s; picture %s; window %s): %s, aspect %.4f; swap chain %ux%u DXGI format %d swap effect %d scaling %s buffers %u windowed %d; "
                            "window style 0x%08lX exstyle 0x%08lX rect %ld,%ld %ldx%ld client %dx%d; monitor %ld,%ld,%ld,%ld work %ld,%ld,%ld,%ld",
                            S.activations, desktopWindowModeName(S.mode), desktopWindowAspectName(S.aspectOpt), desktopWindowStyleName(S.styleOpt), eyes, aspect, bd.Width, bd.Height, static_cast<int>(sc.BufferDesc.Format),
                            static_cast<int>(sc.SwapEffect), S.scaling == DwScaling::None ? "none" : S.scaling == DwScaling::AspectRatio ? "aspect_ratio_stretch" : "stretch",
                            sc.BufferCount, sc.Windowed ? 1 : 0, static_cast<unsigned long>(GetWindowLongPtrW(hwnd, GWL_STYLE)),
                            static_cast<unsigned long>(GetWindowLongPtrW(hwnd, GWL_EXSTYLE)), wr.left, wr.top, wr.right - wr.left, wr.bottom - wr.top, clientW, clientH,
                            mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right, mi.rcMonitor.bottom, mi.rcWork.left, mi.rcWork.top, mi.rcWork.right, mi.rcWork.bottom);
            if (ratio > 0) {   // the crop of each eye, once per activation: where it looks straight ahead, and the bounds it was cropped to
                for (unsigned eye = 0; eye < 2; ++eye) {
                    if (!(need & (1u << eye))) continue;
                    Log::get().note("desktop window: crop %s, %s eye: looks straight ahead at u=%.4f v=%.4f of its image (%s); crop bounds u %.4f..%.4f v %.4f..%.4f of %ux%u",
                                    desktopWindowAspectName(S.aspectOpt), eye ? "right" : "left", centreU[eye], centreV[eye], fromFov[eye] ? "from its field of view" : "no field of view: the middle",
                                    cropBounds[eye][0], cropBounds[eye][2], cropBounds[eye][1], cropBounds[eye][3], texW[eye], texH[eye]);
                }
            }
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
    S.aspect = aspect;
    {   // where SetCursorPos at the client's centre would be, for the cursor line
        POINT centre = {clientW / 2, clientH / 2};
        if (ClientToScreen(hwnd, &centre)) desktopWindowCursorPublishCentre(centre.x, centre.y);
    }
    desktopWindowCursorHide(hwnd);   // the cursor over the picture, while the window has focus (round 4)
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
        draws[i].bounds = cropBounds[eyeIndex[i]];
        draws[i].rect = rects[i];
    }
    drawEyes(ctx.Get(), rtv.Get(), draws, count);
    ++S.drawn;
    return Outcome::Drew;
}

}  // namespace

namespace {
// Is render target 0, as the context itself says, the swap chain's buffer? The binding shadow said so when the draw was skipped; this asks the context.
bool renderTargetIsBuffer(ID3D11DeviceContext* ctx) {
    ID3D11RenderTargetView* rtv = nullptr;
    ctx->OMGetRenderTargets(1, &rtv, nullptr);
    if (!rtv) return false;
    ID3D11Resource* resource = nullptr;
    rtv->GetResource(&resource);
    const bool same = resource && static_cast<void*>(resource) == g_desktopWindowBack.load(std::memory_order_relaxed);
    if (resource) resource->Release();
    rtv->Release();
    return same;
}
}  // namespace

void desktopWindowNoteSkippedDraw(ID3D11DeviceContext* ctx, char kind, uint64_t vsHash, uint64_t psHash, bool depthBound) {
    // One writer (the render thread's draw hooks): a load and a store, not a locked add.
    const uint64_t n = g_skipped.load(std::memory_order_relaxed) + 1;
    g_skipped.store(n, std::memory_order_relaxed);
    g_census.note(vsHash, psHash, kind, depthBound);
    if ((n & 31u) == 0 && ctx) {   // one in 32 is checked against the context
        bool onBuffer = false;
        if (guardedBudget(g_sampleBudget, [&] { onBuffer = renderTargetIsBuffer(ctx); })) g_census.sample(onBuffer);
    }
}

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
                                   g_handoffs[1].load(std::memory_order_relaxed), S.stale, S.clientW, S.clientH, S.chainW, S.chainH, S.userMoves, S.userResizes);
            Log::get().note("%s", line);
            S.userMoves = S.userResizes = 0;
            char census[640];
            g_census.format(census, sizeof census);
            Log::get().note("%s", census);
            g_census.reset();
            char cursor[640];
            desktopWindowCursorTakeLine(cursor, sizeof cursor);
            Log::get().note("%s", cursor);
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
    // Version 1 (no field of view) or 2 (with it); the table answers with the version asked, and its noteEye slot holds that version's function.
    if (!table || table->size != sizeof(*table) ||
        (table->version != EDVR_NATIVE_DESKTOP_WINDOW_VERSION_1 && table->version != EDVR_NATIVE_DESKTOP_WINDOW_VERSION_2))
        return E_INVALIDARG;
    const uint32_t version = table->version;
    *table = {sizeof(*table), version};
    if (!request || request->size != sizeof(*request) || request->version != version || !request->gameDevice || !request->generation) return E_INVALIDARG;
    table->context = &edvr::g_contextToken;
    table->noteEye = version == EDVR_NATIVE_DESKTOP_WINDOW_VERSION_2 ? reinterpret_cast<EdvrNativeDesktopWindowNoteEye>(&edvr::noteEye2) : &edvr::noteEye;
    table->close = edvr::closeProvider;
    return S_OK;
}
