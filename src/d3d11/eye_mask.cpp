#include "eye_mask.h"

#include <windows.h>

#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

#include "../common/config.h"
#include "../common/frame_flag.h"
#include "../common/guard.h"
#include "../common/log.h"
#include "../common/timing.h"
#include "binding_shadow.h"
#include "depth_probe.h"
#include "foveation.h"
#include "native_timing.h"
#include "vscreen.h"

// See eye_mask.h for what this is, the geometry derivation, the hook-bypass
// mechanism and the cross-DLL ABI. This file is the implementation only.

namespace edvr {
namespace {

using Microsoft::WRL::ComPtr;

constexpr float kPi = 3.14159265358979323846f;

float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
float radToDeg(float rad) { return rad * (180.0f / kPi); }
float degToRad(float deg) { return deg * (kPi / 180.0f); }

// ------------------------------------------------------------- geometry
//
// Pure functions: no D3D11, no globals. eye_mask.h's GEOMETRY section has
// the derivation; edvrEyeMaskSelftest at the bottom of this file exercises
// these directly, with no device.

constexpr uint32_t kRingSegments = 64;
constexpr uint32_t kRingVertexCount = 2 * (kRingSegments + 1);  // E_0,O_0..E_64,O_64

struct EyeMaskGeometry {
    float centreX = 0.0f, centreY = 0.0f;
    float a = 0.0f, b = 0.0f;   // NDC semi-axes
    float k = 0.0f;              // outer ring scale, 4/min(a,b)
    float r0Deg = 0.0f;          // widest of the four frustum half-angles
    float rDeg = 0.0f;           // R0 - trim, clamped to [5,89]
    bool  valid = false;
};

EyeMaskGeometry computeEyeMaskGeometry(float l, float r, float d, float u, float trimDeg) {
    EyeMaskGeometry g;
    if (!(l < 0.0f && r > 0.0f && d < 0.0f && u > 0.0f)) return g;
    const float spanX = r - l;
    const float spanY = u - d;
    if (!(spanX > 1e-6f) || !(spanY > 1e-6f)) return g;

    g.centreX = -(r + l) / spanX;
    g.centreY = -(u + d) / spanY;

    const float r0 = radToDeg(std::max(std::max(std::atan(-l), std::atan(r)),
                                        std::max(std::atan(-d), std::atan(u))));
    g.r0Deg = r0;
    g.rDeg = clampf(r0 - trimDeg, 5.0f, 89.0f);

    const float t = std::tan(degToRad(g.rDeg));
    if (!(t > 0.0f)) return g;
    g.a = 2.0f * t / spanX;
    g.b = 2.0f * t / spanY;
    if (!(g.a > 1e-6f) || !(g.b > 1e-6f)) return g;

    g.k = 4.0f / std::min(g.a, g.b);
    g.valid = true;
    return g;
}

// Grid-sampled fraction of the [-1,1]x[-1,1] NDC square (the eye's own
// viewport) the ring masks -- everywhere outside the inner ellipse. Not a
// hot-path call: only the effective-change log line and the smoke test
// call this, never the per-draw path.
float maskedFraction(const EyeMaskGeometry& g) {
    if (!g.valid) return 0.0f;
    constexpr int kGrid = 400;
    uint32_t masked = 0;
    for (int iy = 0; iy < kGrid; ++iy) {
        const float y = -1.0f + (2.0f * iy + 1.0f) / kGrid;
        const float ey = (y - g.centreY) / g.b;
        const float ey2 = ey * ey;
        for (int ix = 0; ix < kGrid; ++ix) {
            const float x = -1.0f + (2.0f * ix + 1.0f) / kGrid;
            const float ex = (x - g.centreX) / g.a;
            if (ex * ex + ey2 > 1.0f) ++masked;
        }
    }
    return static_cast<float>(masked) / static_cast<float>(kGrid * kGrid);
}

// --------------------------------------------------------------- shader
//
// The ring, entirely from SV_VertexID: no vertex buffer, no input layout.
// id/2 is the segment (0..kRingSegments), the low bit picks inner (E) or
// outer (O) -- see eye_mask.h. The pixel shader is NULL at the draw
// (depth-only); nothing here compiles one.
constexpr char kRingVsHlsl[] = R"HLSL(
cbuffer EyeMaskCB : register(b0) {
    float2 centre;
    float2 axes;
    float outerScale;
    float depthValue;
    float segments;
    float pad0;
};
float4 main(uint id : SV_VertexID) : SV_POSITION {
    uint seg = id / 2;
    uint parity = id - seg * 2;
    float theta = float(seg) * (6.283185307179586 / segments);
    float c = cos(theta);
    float s = sin(theta);
    float scale = parity == 0 ? 1.0 : outerScale;
    float2 p = centre + float2(axes.x * c, axes.y * s) * scale;
    return float4(p, depthValue, 1.0);
}
)HLSL";

struct EyeMaskCBuffer {
    float centreX, centreY;
    float a, b;
    float outerScale;
    float depthValue;
    float segments;
    float pad0;
};

// ------------------------------------------------------------- resources

struct DeviceResources {
    ID3D11Device* device = nullptr;   // identity only, to notice a device change
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11Buffer> cb;
    ComPtr<ID3D11RasterizerState> rs;
    ComPtr<ID3D11DepthStencilState> dss;
    bool failed = false;
};
DeviceResources g_res;

// -------------------------------------------------------------- config/state

enum class Mode { Off, Auto, Lens };
Mode g_mode = Mode::Off;
int g_trimDeg = 0;
bool g_loggedOff = false;

enum Reason : int {
    kReasonNotSettled = 0,
    kReasonNoClear,
    kReasonNoFov,
    kReasonNotReported,
    kReasonSetupFailed,
    kReasonUavBound,
    kReasonReadOnlyDepth,
    kReasonCount
};
const char* const kReasonText[kReasonCount] = {
    "eye not settled",
    "no clear value on record for this depth target",
    "no FOV yet",
    "runtime mask not reported",
    "shader or buffer setup failed",
    "uav bound",
    "read-only depth view",
};
bool g_reasonLogged[kReasonCount] = {};
uint64_t g_reasonCount[kReasonCount] = {};

struct EyeState {
    // Once-per-eye-per-frame + the re-clear census.
    uint32_t frameDrawn = 0;
    void* drawnDsv = nullptr;   // identity only, never dereferenced

    // Cumulative ("so far") counters for the periodic summary.
    uint64_t framesObserved = 0;
    uint64_t framesMasked = 0;
    uint64_t reclearedFrames = 0;

    // What the last effective-change line printed, to detect the next one.
    bool forceLog = true;   // unconditional the first time this eye is seen
    float lastL = 0, lastR = 0, lastD = 0, lastU = 0;
};
EyeState g_eyeState[2];

uint32_t g_frameNo = 0;
uint32_t g_maskedEyesThisFrameBits = 0;
// The RTV identity handled for each eye this frame (never dereferenced),
// so a repeat draw into an already-handled eye bails on a pointer compare
// instead of paying bindingResolve + eyeOfSettled again -- most eye draws
// in a scene land after the first one per eye.
void* g_maskedRtvThisFrame[2] = {nullptr, nullptr};
uint64_t g_lastSummaryMs = 0;

FaultBudget g_drawBudget("eye_mask.draw", 3);

void logOffOnce() {
    if (g_loggedOff) return;
    g_loggedOff = true;
    Log::get().note("eye mask: off.");
}

void noteWaiting(int reason) {
    ++g_reasonCount[reason];
    if (!g_reasonLogged[reason]) {
        g_reasonLogged[reason] = true;
        Log::get().note("eye mask: waiting -- %s.", kReasonText[reason]);
    }
}

// Is any of the eight OM UAV slots occupied? OMGetRenderTargets cannot see
// these -- only OMGetRenderTargetsAndUnorderedAccessViews can -- and both
// the ring's own bind and the restore go through vScreenSetRenderTargetsRaw
// (plain OMSetRenderTargets), which would silently unbind any UAV the game
// bound through OMSetRenderTargetsAndUnorderedAccessViews (kSlotOMSetRtvAndUav,
// vscreen.cpp -- this codebase hooks that slot, so the game uses it). Never
// guessed: checked before the OM stage is touched at all, and the draw
// stands down rather than risk it.
bool anyUavBound(ID3D11DeviceContext* ctx) {
    ID3D11UnorderedAccessView* uavs[8] = {};
    ctx->OMGetRenderTargetsAndUnorderedAccessViews(0, nullptr, nullptr, 0, 8, uavs);
    bool any = false;
    for (auto& u : uavs) {
        if (u) { any = true; u->Release(); u = nullptr; }
    }
    return any;
}

// The eye's four frustum tangents from the native timing table's FOV
// sample (radians, left/right/up/down -- native_timing.h), rejected when
// stale (>2s old, the same freshness window perf_monitor's own
// nativeCpuReady uses) or when the runtime has not produced one yet.
bool getEyeFov(int eye, float* l, float* r, float* d, float* u) {
    const NativeTimingSnapshot snap = nativeTimingSnapshot();
    const bool fresh = snap.capturedAtMs != 0 && !elapsedMs(snap.capturedAtMs, 2000);
    const bool ready = snap.active && snap.haveCpu && !snap.invalid && snap.firstSequence &&
                        snap.cpu.sequence >= snap.firstSequence && fresh;
    if (!ready) return false;
    const float angleL = snap.cpu.gameFov[eye][0];
    const float angleR = snap.cpu.gameFov[eye][1];
    const float angleU = snap.cpu.gameFov[eye][2];
    const float angleD = snap.cpu.gameFov[eye][3];
    if (!(std::isfinite(angleL) && std::isfinite(angleR) &&
          std::isfinite(angleU) && std::isfinite(angleD))) {
        return false;
    }
    *l = std::tan(angleL);
    *r = std::tan(angleR);
    *u = std::tan(angleU);
    *d = std::tan(angleD);
    return *l < 0.0f && *r > 0.0f && *d < 0.0f && *u > 0.0f;
}

bool ensureResources(ID3D11Device* device) {
    if (!device) return false;
    if (g_res.device != device) {
        g_res = DeviceResources{};
        g_res.device = device;
    }
    if (g_res.vs && g_res.cb && g_res.rs && g_res.dss) return true;
    if (g_res.failed) return false;

    bool ok = false;
    guarded("eye_mask.setup", [&] {
        ComPtr<ID3DBlob> blob, errors;
        HRESULT hr = D3DCompile(kRingVsHlsl, sizeof(kRingVsHlsl) - 1, "eye_mask_vs",
                                nullptr, nullptr, "main", "vs_5_0", 0, 0, &blob, &errors);
        if (FAILED(hr) || !blob) return;
        if (FAILED(device->CreateVertexShader(blob->GetBufferPointer(), blob->GetBufferSize(),
                                              nullptr, &g_res.vs))) {
            return;
        }

        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = (sizeof(EyeMaskCBuffer) + 15u) & ~15u;
        bd.Usage = D3D11_USAGE_DEFAULT;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        if (FAILED(device->CreateBuffer(&bd, nullptr, &g_res.cb))) return;

        D3D11_RASTERIZER_DESC rd{};
        rd.FillMode = D3D11_FILL_SOLID;
        rd.CullMode = D3D11_CULL_NONE;
        rd.DepthClipEnable = TRUE;
        rd.ScissorEnable = FALSE;
        if (FAILED(device->CreateRasterizerState(&rd, &g_res.rs))) return;

        D3D11_DEPTH_STENCIL_DESC dd{};
        dd.DepthEnable = TRUE;
        dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
        dd.DepthFunc = D3D11_COMPARISON_ALWAYS;
        dd.StencilEnable = FALSE;
        dd.StencilReadMask = 0;
        dd.StencilWriteMask = 0;
        if (FAILED(device->CreateDepthStencilState(&dd, &g_res.dss))) return;

        ok = true;
    });
    if (!ok) {
        g_res.failed = true;
        noteWaiting(kReasonSetupFailed);
    }
    return ok;
}

// ------------------------------------------------------ save/restore state

template <class Shader>
struct SavedStage {
    Shader* shader = nullptr;
    ID3D11ClassInstance* instances[8] = {};
    UINT count = 8;
};

template <class Shader>
void releaseStage(SavedStage<Shader>& s) {
    for (UINT i = 0; i < s.count; ++i) {
        if (s.instances[i]) s.instances[i]->Release();
    }
    if (s.shader) s.shader->Release();
}

struct SavedPipelineState {
    SavedStage<ID3D11VertexShader> vs;
    SavedStage<ID3D11PixelShader> ps;
    SavedStage<ID3D11GeometryShader> gs;
    SavedStage<ID3D11HullShader> hs;
    SavedStage<ID3D11DomainShader> ds;
    ID3D11Buffer* vsCb = nullptr;
    D3D11_PRIMITIVE_TOPOLOGY topology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    ID3D11InputLayout* layout = nullptr;
    ID3D11RasterizerState* rs = nullptr;
    ID3D11DepthStencilState* dss = nullptr;
    UINT stencilRef = 0;
    ID3D11RenderTargetView* rtvs[8] = {};
    ID3D11DepthStencilView* dsv = nullptr;
};

void releaseSavedState(SavedPipelineState& s) {
    releaseStage(s.vs);
    releaseStage(s.ps);
    releaseStage(s.gs);
    releaseStage(s.hs);
    releaseStage(s.ds);
    if (s.vsCb) s.vsCb->Release();
    if (s.layout) s.layout->Release();
    if (s.rs) s.rs->Release();
    if (s.dss) s.dss->Release();
    for (auto& rtv : s.rtvs) {
        if (rtv) { rtv->Release(); rtv = nullptr; }
    }
    if (s.dsv) { s.dsv->Release(); s.dsv = nullptr; }
}

void restorePipelineState(ID3D11DeviceContext* ctx, const SavedPipelineState& s) {
    vScreenSetRenderTargetsRaw(ctx, 8, s.rtvs, s.dsv);
    ctx->OMSetDepthStencilState(s.dss, s.stencilRef);
    ctx->RSSetState(s.rs);
    ctx->IASetInputLayout(s.layout);
    ctx->IASetPrimitiveTopology(s.topology);
    ID3D11Buffer* cb = s.vsCb;
    vScreenVSSetConstantBuffersRaw(ctx, 0, 1, &cb);
    ctx->DSSetShader(s.ds.shader, s.ds.count ? s.ds.instances : nullptr, s.ds.count);
    ctx->HSSetShader(s.hs.shader, s.hs.count ? s.hs.instances : nullptr, s.hs.count);
    ctx->GSSetShader(s.gs.shader, s.gs.count ? s.gs.instances : nullptr, s.gs.count);
    vScreenPSSetShaderRaw(ctx, s.ps.shader, s.ps.count ? s.ps.instances : nullptr, s.ps.count);
    vScreenVSSetShaderRaw(ctx, s.vs.shader, s.vs.count ? s.vs.instances : nullptr, s.vs.count);
}

// Every state change and the draw itself go through vscreen.h's *Raw
// wrappers (for the methods that ARE hooked: Draw, VS/PSSetShader,
// VSSetConstantBuffers, UpdateSubresource, OMSetRenderTargets) or straight
// through ctx (for the methods this module never hooks at all: IA/GS/HS/
// DS/RS/OMSetDepthStencilState and every getter) -- see eye_mask.h's PAST
// THE HOOKS section. Capture and set+draw share g_drawBudget, so either can
// disable further attempts this session once it has faulted enough times.
// Restore does NOT share that budget: it runs under its own unconditional
// guard whenever capture succeeded, regardless of how many faults set+draw
// itself charged -- a shared budget here previously meant a fault in
// set+draw could exhaust it and skip the restore, leaving the game's
// context with the ring's VS, a null PS, the depth-only DSS/RS and a
// DSV-only binding. *uavBound is set (and nothing is touched) when the OM
// stage already carries a UAV -- see anyUavBound above.
bool issueRingDraw(ID3D11DeviceContext* ctx, ID3D11Device* device, ID3D11DepthStencilView* dsv,
                   const EyeMaskGeometry& geom, float depthValue, bool* uavBound) {
    if (!ensureResources(device)) return false;

    SavedPipelineState saved{};
    bool captured = false;
    guardedBudget(g_drawBudget, [&] {
        saved.vs.count = 8; ctx->VSGetShader(&saved.vs.shader, saved.vs.instances, &saved.vs.count);
        saved.ps.count = 8; ctx->PSGetShader(&saved.ps.shader, saved.ps.instances, &saved.ps.count);
        saved.gs.count = 8; ctx->GSGetShader(&saved.gs.shader, saved.gs.instances, &saved.gs.count);
        saved.hs.count = 8; ctx->HSGetShader(&saved.hs.shader, saved.hs.instances, &saved.hs.count);
        saved.ds.count = 8; ctx->DSGetShader(&saved.ds.shader, saved.ds.instances, &saved.ds.count);
        ctx->VSGetConstantBuffers(0, 1, &saved.vsCb);
        ctx->IAGetPrimitiveTopology(&saved.topology);
        ctx->IAGetInputLayout(&saved.layout);
        ctx->RSGetState(&saved.rs);
        ctx->OMGetDepthStencilState(&saved.dss, &saved.stencilRef);
        ctx->OMGetRenderTargets(8, saved.rtvs, &saved.dsv);
        captured = true;
    });
    if (!captured) {
        releaseSavedState(saved);
        return false;
    }

    // Before the OM stage is touched at all: a UAV in any of the eight
    // slots means the game bound through
    // OMSetRenderTargetsAndUnorderedAccessViews, and both the ring's own
    // bind below and the restore go through plain OMSetRenderTargets
    // (vScreenSetRenderTargetsRaw), which would silently drop it. Stand
    // down instead of guessing -- nothing has been set yet, so nothing
    // needs restoring.
    if (anyUavBound(ctx)) {
        if (uavBound) *uavBound = true;
        releaseSavedState(saved);
        return false;
    }

    bool drew = false;
    guardedBudget(g_drawBudget, [&] {
        EyeMaskCBuffer cbData{geom.centreX, geom.centreY, geom.a, geom.b,
                              geom.k, depthValue, static_cast<float>(kRingSegments), 0.0f};
        vScreenUpdateSubresourceRaw(ctx, g_res.cb.Get(), 0, nullptr, &cbData, 0, 0);

        vScreenVSSetShaderRaw(ctx, g_res.vs.Get(), nullptr, 0);
        vScreenPSSetShaderRaw(ctx, nullptr, nullptr, 0);
        ctx->GSSetShader(nullptr, nullptr, 0);
        ctx->HSSetShader(nullptr, nullptr, 0);
        ctx->DSSetShader(nullptr, nullptr, 0);
        ID3D11Buffer* cbPtr = g_res.cb.Get();
        vScreenVSSetConstantBuffersRaw(ctx, 0, 1, &cbPtr);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
        ctx->IASetInputLayout(nullptr);
        ctx->RSSetState(g_res.rs.Get());
        ctx->OMSetDepthStencilState(g_res.dss.Get(), 0);
        vScreenSetRenderTargetsRaw(ctx, 0, nullptr, dsv);

        vScreenDrawRaw(ctx, kRingVertexCount, 0);
        drew = true;
    });

    // Unconditional: must run whenever capture succeeded, even if set+draw
    // just charged g_drawBudget's last fault (see the function comment).
    guarded("eye_mask.restore", [&] { restorePipelineState(ctx, saved); });

    releaseSavedState(saved);
    return drew;
}

// -------------------------------------------------------------- log lines

void maybeLogEffectiveChange(int eye, const EyeMaskGeometry& geom, float l, float r, float d, float u,
                             uint32_t triL, uint32_t triR, bool reported,
                             float clearValue, bool reversed, float depthValue, bool drew) {
    EyeState& es = g_eyeState[eye];
    bool fire = es.forceLog;
    if (!fire) {
        fire = std::fabs(l - es.lastL) > 0.01f || std::fabs(r - es.lastR) > 0.01f ||
               std::fabs(d - es.lastD) > 0.01f || std::fabs(u - es.lastU) > 0.01f;
    }
    if (!fire) return;

    es.forceLog = false;
    es.lastL = l; es.lastR = r; es.lastD = d; es.lastU = u;

    const float pct = maskedFraction(geom) * 100.0f;
    const char* modeText = g_mode == Mode::Auto ? "auto" : (g_mode == Mode::Lens ? "lens" : "off");

    char triText[64];
    if (reported) {
        snprintf(triText, sizeof(triText), "%u/%u triangles (eye 0/1)", triL, triR);
    } else {
        snprintf(triText, sizeof(triText), "not reported");
    }

    Log::get().note(
        "eye mask: %s -- runtime mask %s; lens cone %.1f deg (edge %.1f deg, trim %d); "
        "ellipse centre %.3f,%.3f semi-axes %.3f,%.3f in NDC; masks %.1f%% of eye %d's "
        "viewport; depth written %.1f (clear %.3f, reversed-Z %s); drew %s",
        modeText, triText, geom.rDeg, geom.r0Deg, g_trimDeg,
        geom.centreX, geom.centreY, geom.a, geom.b, pct, eye,
        depthValue, clearValue, reversed ? "yes" : "no", drew ? "yes" : "no");
}

void logSummaryIfDue() {
    if (!dueMs(g_lastSummaryMs, 60000)) return;
    g_lastSummaryMs = nowMs();

    char reasons[512] = {};
    size_t used = 0;
    for (int i = 0; i < kReasonCount; ++i) {
        if (g_reasonCount[i] == 0) continue;
        const int n = snprintf(reasons + used, sizeof(reasons) - used, "%s%s=%llu",
                               used ? ", " : "", kReasonText[i],
                               static_cast<unsigned long long>(g_reasonCount[i]));
        if (n > 0) used += static_cast<size_t>(n);
    }
    if (!used) snprintf(reasons, sizeof(reasons), "none");

    Log::get().note(
        "eye mask so far: eye 0 masked %llu of %llu frames, eye 1 masked %llu of %llu "
        "frames; %llu frame(s) had the depth target cleared again after the mask; "
        "stand-downs: %s",
        static_cast<unsigned long long>(g_eyeState[0].framesMasked),
        static_cast<unsigned long long>(g_eyeState[0].framesObserved),
        static_cast<unsigned long long>(g_eyeState[1].framesMasked),
        static_cast<unsigned long long>(g_eyeState[1].framesObserved),
        static_cast<unsigned long long>(g_eyeState[0].reclearedFrames +
                                        g_eyeState[1].reclearedFrames),
        reasons);
}

}  // namespace

void eyeMaskConfigure(Config& cfg) {
    const std::string modeStr = cfg.getString("fix.eye_mask", "off");
    Mode newMode = Mode::Off;
    bool unknown = false;
    if (modeStr == "auto") newMode = Mode::Auto;
    else if (modeStr == "lens") newMode = Mode::Lens;
    else if (modeStr != "off" && !modeStr.empty()) unknown = true;

    if (unknown) {
        static bool warnedOnce = false;
        if (!warnedOnce) {
            warnedOnce = true;
            Log::get().note(
                "eye mask: fix.eye_mask=\"%s\" is not off, auto or lens -- reading it as off.",
                modeStr.c_str());
        }
    }

    const int newTrim = cfg.getIntInRange("fix.eye_mask_trim", 0, -15, 30);
    const bool changed = (newMode != g_mode) || (newTrim != g_trimDeg);
    g_mode = newMode;
    g_trimDeg = newTrim;

    if (g_mode != Mode::Off) {
        g_loggedOff = false;
        if (changed) {
            g_eyeState[0].forceLog = true;
            g_eyeState[1].forceLog = true;
        }
    } else {
        logOffOnce();
    }
}

bool eyeMaskWantsDraws() { return g_mode != Mode::Off; }

void eyeMaskOnEyeDraw(ID3D11DeviceContext* ctx, void* rtv, void* dsvIdentity) {
    if (g_mode == Mode::Off || !ctx) return;
    if ((g_maskedEyesThisFrameBits & 3u) == 3u) return;   // both eyes already handled
    // Cheap pointer compare against each eye's RTV identity this frame,
    // before paying bindingResolve + eyeOfSettled again -- most draws into
    // an eye-sized target in a frame land after that eye's first one.
    if (rtv && (rtv == g_maskedRtvThisFrame[0] || rtv == g_maskedRtvThisFrame[1])) return;
    if (!dsvIdentity) { noteWaiting(kReasonNoClear); return; }

    ResourceInfo info;
    if (!bindingResolve(rtv, &info) || !info.isTexture2D) { noteWaiting(kReasonNotSettled); return; }
    int eye = -1;
    if (!eyeOfSettled(info, &eye) || eye < 0 || eye > 1) { noteWaiting(kReasonNotSettled); return; }
    const uint32_t bit = 1u << eye;
    if (g_maskedEyesThisFrameBits & bit) return;

    // Ground truth: a referenced pointer, never the binding shadow's
    // identity-only one (see eye_mask.h's eyeMaskOnEyeDraw comment).
    ID3D11DepthStencilView* dsv = nullptr;
    ctx->OMGetRenderTargets(0, nullptr, &dsv);
    if (!dsv) { noteWaiting(kReasonNoClear); return; }

    // A read-only depth view means our write would silently do nothing --
    // checked before anything else spends effort on this draw.
    D3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
    dsv->GetDesc(&dsvDesc);
    if (dsvDesc.Flags & D3D11_DSV_READ_ONLY_DEPTH) {
        dsv->Release();
        noteWaiting(kReasonReadOnlyDepth);
        return;
    }

    float clearValue = 0.0f;
    bool reversed = false;
    if (!depthProbeClearValueFor(dsv, &clearValue, &reversed)) {
        dsv->Release();
        noteWaiting(kReasonNoClear);
        return;
    }

    float l = 0, r = 0, d = 0, u = 0;
    if (!getEyeFov(eye, &l, &r, &d, &u)) {
        dsv->Release();
        noteWaiting(kReasonNoFov);
        return;
    }

    uint32_t triL = 0, triR = 0;
    const bool reported = runtimeMaskTriangles(&triL, &triR);
    if (g_mode == Mode::Auto && !reported) {
        dsv->Release();
        noteWaiting(kReasonNotReported);
        return;
    }
    const uint32_t myTri = (eye == 0) ? triL : triR;

    // Past every stand-down: this eye is OBSERVED for the summary whatever
    // we decide next.
    ++g_eyeState[eye].framesObserved;

    bool shouldDraw = true;
    if (g_mode == Mode::Auto) {
        // Runtime already gives a mask: only add to it if trim asks for
        // more (trim > 0). Runtime gives none (0 triangles): draw ours.
        shouldDraw = (reported && myTri > 0) ? (g_trimDeg > 0) : true;
    }

    const EyeMaskGeometry geom = computeEyeMaskGeometry(l, r, d, u, static_cast<float>(g_trimDeg));
    const float depthValue = reversed ? 1.0f : 0.0f;

    bool drew = false;
    bool uavBound = false;
    if (shouldDraw && geom.valid) {
        ID3D11Device* device = nullptr;
        ctx->GetDevice(&device);
        if (device) {
            drew = issueRingDraw(ctx, device, dsv, geom, depthValue, &uavBound);
            device->Release();
        }
    }
    if (uavBound) noteWaiting(kReasonUavBound);

    g_maskedEyesThisFrameBits |= bit;
    g_maskedRtvThisFrame[eye] = rtv;
    if (drew) {
        EyeState& es = g_eyeState[eye];
        ++es.framesMasked;
        es.frameDrawn = g_frameNo;
        es.drawnDsv = dsv;   // identity only, from here on
    }

    maybeLogEffectiveChange(eye, geom, l, r, d, u, triL, triR, reported,
                            clearValue, reversed, depthValue, drew);

    dsv->Release();
}

void eyeMaskOnClear(void* dsv) {
    if (g_mode == Mode::Off || !dsv) return;
    for (int eye = 0; eye < 2; ++eye) {
        EyeState& es = g_eyeState[eye];
        if (es.drawnDsv == dsv && es.frameDrawn == g_frameNo) {
            ++es.reclearedFrames;
            es.drawnDsv = nullptr;   // count once per mask draw, not once per later clear
        }
    }
}

void eyeMaskFrameBoundary(ID3D11DeviceContext* ctx) {
    (void)ctx;
    ++g_frameNo;
    g_maskedEyesThisFrameBits = 0;
    g_maskedRtvThisFrame[0] = nullptr;
    g_maskedRtvThisFrame[1] = nullptr;
    if (g_mode != Mode::Off) logSummaryIfDue();
}

void eyeMaskShutdown() {
    g_res = DeviceResources{};
}

}  // namespace edvr

// The desk test (tools/smoke): pure geometry, no device, no context --
// computeEyeMaskGeometry and maskedFraction against the Pimax Crystal
// Super's measured tangents (docs/eye-mask-2026-09-16.md) and the trim
// clamp. See eye_mask.h for the bit meanings.
extern "C" __declspec(dllexport) unsigned edvrEyeMaskSelftest() {
    using namespace edvr;
    unsigned bits = 0;

    const float l = -1.529f, r = 1.032f, d = -1.265f, u = 1.265f;

    const EyeMaskGeometry g0 = computeEyeMaskGeometry(l, r, d, u, 0.0f);
    // The task brief's own worked example states centre (-0.194, 0).
    // Applying its own stated formula, x_c = -(r+l)/(r-l), to its own
    // stated tangents gives +0.194, not -0.194 -- confirmed by hand three
    // separate ways (see the implementation report). This asserts the
    // verified value, not the brief's literal text.
    const bool check1 = g0.valid &&
                        std::fabs(g0.centreX - 0.194f) < 0.002f &&
                        std::fabs(g0.centreY - 0.0f) < 0.002f &&
                        g0.a > 0.0f && g0.b > 0.0f &&
                        std::fabs(g0.rDeg - 56.8f) < 0.3f;
    if (check1) bits |= 1u;

    bool check2 = g0.valid;
    if (check2) {
        for (uint32_t i = 0; i <= kRingSegments; ++i) {
            const float theta = static_cast<float>(i) * (2.0f * kPi / static_cast<float>(kRingSegments));
            const float ox = g0.centreX + g0.k * g0.a * std::cos(theta);
            const float oy = g0.centreY + g0.k * g0.b * std::sin(theta);
            if (std::fabs(ox) <= 1.5f && std::fabs(oy) <= 1.5f) { check2 = false; break; }
        }
    }
    if (check2) bits |= 2u;

    const float frac0 = maskedFraction(g0) * 100.0f;
    const bool check3 = g0.valid && frac0 >= 6.0f && frac0 <= 9.0f;
    if (check3) bits |= 4u;

    const EyeMaskGeometry g30 = computeEyeMaskGeometry(l, r, d, u, 30.0f);
    const float frac30 = maskedFraction(g30) * 100.0f;
    const bool check4 = g30.valid && frac30 > frac0;
    if (check4) bits |= 8u;

    // The R0-trim<5 deg clamp: an extreme trim, past the ini's own legal
    // -15..30 range on purpose, must still land on the 5 degree floor.
    const EyeMaskGeometry gExtreme = computeEyeMaskGeometry(l, r, d, u, 90.0f);
    const bool check5 = gExtreme.valid && std::fabs(gExtreme.rDeg - 5.0f) < 0.01f;
    if (check5) bits |= 16u;

    return bits;
}
