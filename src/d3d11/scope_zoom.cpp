// The scope lens's door (scope_zoom.h; docs\design-vr-scope-magnification-2026-10-28.md, section 8). Two compute passes over the aiming eye's picture after the upscaler and RCAS:
//   1. scopeZoomRays: 128 rays from c over the eye's depth find the hole of the scope's frame, and write the radii, the box and pass 2's indirect arguments into one raw buffer;
//   2. scopeZoomMagnify, dispatched indirectly over the box: a pixel inside the hole takes the Catmull-Rom sample of a copy of the window at c + (p - c) / M.
// Nothing is read back to the CPU except that buffer, a ring of staging copies mapped without waiting, for the press line.
#include "scope_zoom.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>

#include "../common/log.h"
#include "gpu_census.h"
#include "gpu_timing.h"
#include "scope_zoom_shaders.h"
#include "shader_swap.h"
#include "temporal_shader_bytecode.h"
#include "ui_layer_shaders.h"   // uiLayerFrameView: the 8-bit families a pass can run over

namespace edvr {
namespace {

namespace sz = scopezoom;
template <class T>
using Ptr = Microsoft::WRL::ComPtr<T>;

// ---- the frame thread's record, by seqlock ----------------------------------------------------------------------------------------------------------------------------------
struct Pub {
    std::atomic<uint32_t> seq{0};
    ScopeZoomFrame f;
};
Pub g_pub;
std::atomic<bool> g_active{false};       // the last published verdict was not kIdle
std::atomic<int> g_inflight{0};          // rays in flight to the CPU

uint64_t nowUs() noexcept {
    static const double perUs = [] {
        LARGE_INTEGER f;
        return QueryPerformanceFrequency(&f) && f.QuadPart > 0 ? static_cast<double>(f.QuadPart) / 1.0e6 : 1.0;
    }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return static_cast<uint64_t>(static_cast<double>(c.QuadPart) / perUs);
}
bool readFrame(ScopeZoomFrame* out) noexcept {
    for (int tries = 0; tries < 3; ++tries) {
        const uint32_t s1 = g_pub.seq.load(std::memory_order_acquire);
        if ((s1 & 1u) || s1 == 0) continue;
        *out = g_pub.f;
        std::atomic_thread_fence(std::memory_order_acquire);
        if (g_pub.seq.load(std::memory_order_relaxed) == s1) return true;
    }
    return false;
}

// ---- the press's counters --------------------------------------------------------------------------------------------------------------------------------------------------
std::mutex g_mu;
sz::LensDoorStats g_stats;
uint32_t g_pressId = 0;

template <class F>
void count(F&& f) {
    std::lock_guard<std::mutex> lock(g_mu);
    f(g_stats);
}

// ---- device children --------------------------------------------------------------------------------------------------------------------------------------------------------
constexpr int kStage = 4;    // staging copies of the rays buffer in flight
constexpr int kTimers = 6;   // GPU timer slots (a pair each: the rays pass, the magnify pass)

struct StageSlot {
    Ptr<ID3D11Buffer> buf;
    bool inUse = false, settled = false;
    uint32_t press = 0;
};
struct TimerSlot {
    GpuTimer t[2];
    bool inUse = false;
    int state[2] = {0, 0};   // 0 untimed, 1 pending, 2 ready, 3 invalid
    double ms[2] = {0.0, 0.0};
};
struct State {
    Ptr<ID3D11Device> dev;
    Ptr<ID3D11ComputeShader> csRays, csMag;
    bool csTried = false, csOk = false;
    Ptr<ID3D11Buffer> cbRays, cbMag;
    Ptr<ID3D11SamplerState> lin;
    Ptr<ID3D11Buffer> lens;
    Ptr<ID3D11UnorderedAccessView> lensUav;
    Ptr<ID3D11ShaderResourceView> lensSrv;
    StageSlot stage[kStage];
    Ptr<ID3D11Texture2D> rcTex;
    Ptr<ID3D11ShaderResourceView> rcSrv;
    uint32_t rcW = 0, rcH = 0;
    DXGI_FORMAT rcFmt = DXGI_FORMAT_UNKNOWN;
    Ptr<ID3D11Texture2D> ownTex;
    Ptr<ID3D11UnorderedAccessView> ownUav;
    uint32_t ownW = 0, ownH = 0;
    DXGI_FORMAT ownFmt = DXGI_FORMAT_UNKNOWN;
    void* frameUavRes = nullptr;
    Ptr<ID3D11UnorderedAccessView> frameUav;
    DXGI_FORMAT frameUavView = DXGI_FORMAT_UNKNOWN;
    Ptr<ID3D11Texture2D> frameUavHold;
};
State g_s;
TimerSlot g_timers[kTimers];   // apart from State: a GpuTimer is never moved
bool g_firstNoted = false, g_failNoted = false;

void releaseAll(ID3D11DeviceContext* ctx) {
    for (TimerSlot& t : g_timers) {
        t.t[0].reset(ctx);
        t.t[1].reset(ctx);
        t.inUse = false;
        t.state[0] = t.state[1] = 0;
    }
    g_inflight.store(0, std::memory_order_relaxed);
    g_s = State{};
    g_firstNoted = false;
}

void failOnce(const char* what) {
    if (g_failNoted) return;
    g_failNoted = true;
    Log::get().note("scope lens: %s; the lens is declined on every frame it would have run (counted as format refused in the press lines).", what);
}

bool makeCb(ID3D11Device* dev, UINT bytes, Ptr<ID3D11Buffer>* out) {
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = bytes;
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    return SUCCEEDED(dev->CreateBuffer(&bd, nullptr, out->ReleaseAndGetAddressOf()));
}

bool ensureDevice(ID3D11Device* dev, ID3D11DeviceContext* ctx) {
    if (g_s.dev.Get() != dev) {
        releaseAll(ctx);
        g_s.dev = dev;
    }
    if (!g_s.csTried) {
        g_s.csTried = true;
        g_s.csRays.Attach(shaderSwapCreateCs(ctx, kScopeZoomRaysBytecode, sizeof(kScopeZoomRaysBytecode), "scope_zoom_rays_cs", "scope zoom"));
        g_s.csMag.Attach(shaderSwapCreateCs(ctx, kScopeZoomMagnifyBytecode, sizeof(kScopeZoomMagnifyBytecode), "scope_zoom_magnify_cs", "scope zoom"));
        D3D11_SAMPLER_DESC sd{};
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.MaxLOD = D3D11_FLOAT32_MAX;
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = sz::kLensBufferBytes;
        bd.Usage = D3D11_USAGE_DEFAULT;
        bd.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
        bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS | D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS;
        D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};
        ud.Format = DXGI_FORMAT_R32_TYPELESS;
        ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        ud.Buffer.NumElements = sz::kLensBufferBytes / 4;
        ud.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
        D3D11_SHADER_RESOURCE_VIEW_DESC vd{};
        vd.Format = DXGI_FORMAT_R32_TYPELESS;
        vd.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
        vd.BufferEx.NumElements = sz::kLensBufferBytes / 4;
        vd.BufferEx.Flags = D3D11_BUFFEREX_SRV_FLAG_RAW;
        bool ok = g_s.csRays && g_s.csMag && makeCb(dev, sizeof(ScopeZoomRaysParams), &g_s.cbRays) && makeCb(dev, sizeof(ScopeZoomMagnifyParams), &g_s.cbMag) &&
                  SUCCEEDED(dev->CreateSamplerState(&sd, g_s.lin.ReleaseAndGetAddressOf())) && SUCCEEDED(dev->CreateBuffer(&bd, nullptr, g_s.lens.ReleaseAndGetAddressOf())) &&
                  SUCCEEDED(dev->CreateUnorderedAccessView(g_s.lens.Get(), &ud, g_s.lensUav.ReleaseAndGetAddressOf())) &&
                  SUCCEEDED(dev->CreateShaderResourceView(g_s.lens.Get(), &vd, g_s.lensSrv.ReleaseAndGetAddressOf()));
        D3D11_BUFFER_DESC st{};
        st.ByteWidth = sz::kLensBufferBytes;
        st.Usage = D3D11_USAGE_STAGING;
        st.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        for (StageSlot& s : g_s.stage) ok = ok && SUCCEEDED(dev->CreateBuffer(&st, nullptr, s.buf.ReleaseAndGetAddressOf()));
        g_s.csOk = ok;
        if (!ok) failOnce("the passes' shaders, buffers or sampler could not be made");
    }
    return g_s.csOk;
}

// ---- the GPU price ------------------------------------------------------------------------------------------------------------------------------------------------------------
void pollTimers(ID3D11DeviceContext* ctx) {
    if (!gpuTimingOwns(ctx)) return;
    for (TimerSlot& t : g_timers) {
        if (!t.inUse) continue;
        for (int i = 0; i < 2; ++i) {
            if (t.state[i] != 1) continue;
            double ms = 0.0;
            const GpuTimerPoll r = t.t[i].poll(ctx, ms);
            if (r == GpuTimerPoll::Pending) continue;
            t.state[i] = r == GpuTimerPoll::Ready ? 2 : 3;
            t.ms[i] = ms;
        }
        if (t.state[0] == 1 || t.state[1] == 1) continue;
        if (t.state[0] == 2 && t.state[1] == 2) count([&](sz::LensDoorStats& s) { sz::lensNoteGpu(&s, t.ms[0], t.ms[1]); });
        t.inUse = false;
        t.state[0] = t.state[1] = 0;
    }
}
TimerSlot* acquireTimer(ID3D11Device* dev, ID3D11DeviceContext* ctx) {
    if (!gpuTimingAccepts(ctx) && !gpuTimingBind(dev, ctx)) return nullptr;
    for (TimerSlot& t : g_timers) {
        if (t.inUse) continue;
        t.inUse = true;
        t.state[0] = t.state[1] = 0;
        return &t;
    }
    return nullptr;
}
void beginTimer(TimerSlot* t, int i, ID3D11Device* dev, ID3D11DeviceContext* ctx) {
    if (t && t->t[i].begin(dev, ctx)) t->state[i] = 1;
}
void endTimer(TimerSlot* t, int i, ID3D11DeviceContext* ctx) {
    if (t && t->state[i] == 1 && !t->t[i].end(ctx)) t->state[i] = 3;
}

// ---- the rays, read back ------------------------------------------------------------------------------------------------------------------------------------------------------
void pollRays(ID3D11DeviceContext* ctx) {
    for (StageSlot& s : g_s.stage) {
        if (!s.inUse) continue;
        D3D11_MAPPED_SUBRESOURCE m{};
        const HRESULT hr = ctx->Map(s.buf.Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m);
        if (hr == DXGI_ERROR_WAS_STILL_DRAWING) continue;
        sz::LensReadback rb;
        const bool got = SUCCEEDED(hr) && m.pData && sz::lensDecode(m.pData, sz::kLensBufferBytes, &rb);
        if (SUCCEEDED(hr)) ctx->Unmap(s.buf.Get(), 0);
        const uint32_t press = s.press;
        const bool settled = s.settled;
        s.inUse = false;
        g_inflight.fetch_sub(1, std::memory_order_relaxed);
        if (!got) continue;
        count([&](sz::LensDoorStats& st) {
            if (press == g_pressId) sz::lensNoteRays(&st, rb, settled);
        });
    }
}
StageSlot* freeStage() {
    for (StageSlot& s : g_s.stage)
        if (!s.inUse) return &s;
    return nullptr;
}

// ---- the frame's route --------------------------------------------------------------------------------------------------------------------------------------------------------
bool makeTex(ID3D11Device* dev, uint32_t w, uint32_t h, DXGI_FORMAT texFmt, DXGI_FORMAT viewFmt, UINT bind, Ptr<ID3D11Texture2D>* tex, Ptr<ID3D11ShaderResourceView>* srv,
             Ptr<ID3D11UnorderedAccessView>* uav) {
    D3D11_TEXTURE2D_DESC td{};
    td.Width = w;
    td.Height = h;
    td.MipLevels = td.ArraySize = 1;
    td.Format = texFmt;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = bind;
    if (FAILED(dev->CreateTexture2D(&td, nullptr, tex->ReleaseAndGetAddressOf()))) return false;
    if (srv) {
        D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.Format = viewFmt;
        sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        sd.Texture2D.MipLevels = 1;
        if (FAILED(dev->CreateShaderResourceView(tex->Get(), &sd, srv->ReleaseAndGetAddressOf()))) return false;
    }
    if (uav) {
        D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};
        ud.Format = viewFmt;
        ud.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
        if (FAILED(dev->CreateUnorderedAccessView(tex->Get(), &ud, uav->ReleaseAndGetAddressOf()))) return false;
    }
    return true;
}

// The CS stage's slot 0 and the output merger's targets, saved and put back around the passes (the ripple's idiom, and temporal_pass.cpp's for the depth: a shader view over a
// texture a depth-stencil view still has bound is nulled by D3D without a word).
struct StageSave {
    ID3D11ComputeShader* cs = nullptr;
    ID3D11ShaderResourceView* srv[2] = {};
    ID3D11UnorderedAccessView* uav = nullptr;
    ID3D11Buffer* cb = nullptr;
    ID3D11SamplerState* smp = nullptr;
    ID3D11RenderTargetView* rtv[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
    ID3D11DepthStencilView* dsv = nullptr;
    bool om = false;
    void save(ID3D11DeviceContext* ctx, bool saveOm) {
        ctx->CSGetShader(&cs, nullptr, nullptr);
        ctx->CSGetShaderResources(0, 2, srv);
        ctx->CSGetUnorderedAccessViews(0, 1, &uav);
        ctx->CSGetConstantBuffers(0, 1, &cb);
        ctx->CSGetSamplers(0, 1, &smp);
        om = saveOm;
        if (saveOm) ctx->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtv, &dsv);
    }
    void restore(ID3D11DeviceContext* ctx) {
        ID3D11ShaderResourceView* nullSrv[2] = {};
        ID3D11UnorderedAccessView* nullUav = nullptr;
        ctx->CSSetShaderResources(0, 2, nullSrv);
        ctx->CSSetUnorderedAccessViews(0, 1, &nullUav, nullptr);
        ctx->CSSetShader(cs, nullptr, 0);
        ctx->CSSetShaderResources(0, 2, srv);
        ctx->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
        ctx->CSSetConstantBuffers(0, 1, &cb);
        ctx->CSSetSamplers(0, 1, &smp);
        if (om) {
            ctx->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtv, dsv);
            for (auto* v : rtv)
                if (v) v->Release();
            if (dsv) dsv->Release();
        }
        if (cs) cs->Release();
        for (auto* s : srv)
            if (s) s->Release();
        if (uav) uav->Release();
        if (cb) cb->Release();
        if (smp) smp->Release();
    }
};

// The door census's span for the lens (gpu_census.h DoorScopeZoom): the two passes, timed on the section's turn.
struct CensusSpan {
    explicit CensusSpan(ID3D11DeviceContext* ctx) : ctx_(ctx) { gpuCensusBegin(ctx_, GpuCensusSection::DoorScopeZoom); }
    ~CensusSpan() { gpuCensusEnd(ctx_, GpuCensusSection::DoorScopeZoom); }
    CensusSpan(const CensusSpan&) = delete;
    CensusSpan& operator=(const CensusSpan&) = delete;

private:
    ID3D11DeviceContext* ctx_;
};

bool writeCb(ID3D11DeviceContext* ctx, ID3D11Buffer* cb, const void* data, size_t bytes) {
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(ctx->Map(cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m)) || !m.pData) return false;
    std::memcpy(m.pData, data, bytes);
    ctx->Unmap(cb, 0);
    return true;
}

}  // namespace

// ---- the public side ------------------------------------------------------------------------------------------------------------------------------------------------------------
void scopeZoomPublish(const ScopeZoomFrame& frame) {
    g_pub.seq.fetch_add(1, std::memory_order_acq_rel);
    g_pub.f = frame;
    g_pub.f.stampUs = nowUs();
    g_pub.seq.fetch_add(1, std::memory_order_release);
    g_active.store(frame.verdict != static_cast<uint32_t>(sz::LensVerdict::kIdle), std::memory_order_relaxed);
}

bool scopeZoomWanted() { return g_active.load(std::memory_order_relaxed) || g_inflight.load(std::memory_order_relaxed) > 0; }

void scopeZoomPressBegin(uint32_t press) {
    std::lock_guard<std::mutex> lock(g_mu);
    g_stats = sz::LensDoorStats{};
    g_pressId = press;
}
void scopeZoomPressTake(sz::LensDoorStats* out) {
    if (!out) return;
    std::lock_guard<std::mutex> lock(g_mu);
    *out = g_stats;
    sz::lensFinish(out);
}

void scopeZoomRelease() {
    ID3D11DeviceContext* ctx = nullptr;
    if (g_s.dev) g_s.dev->GetImmediateContext(&ctx);
    releaseAll(ctx);
    if (ctx) ctx->Release();
    g_active.store(false, std::memory_order_relaxed);
}

void scopeZoomDoor(const ScopeZoomDoorArgs& a, Ptr<ID3D11Texture2D>* copy) {
    if (copy) copy->Reset();
    if (a.eye != 1 || !a.frame || !a.region) return;
    Ptr<ID3D11Device> dev;
    a.frame->GetDevice(&dev);
    Ptr<ID3D11DeviceContext> ctx;
    if (dev) dev->GetImmediateContext(&ctx);
    if (!dev || !ctx) return;
    if (g_s.dev.Get() == dev.Get()) {
        pollTimers(ctx.Get());
        pollRays(ctx.Get());
    }
    ScopeZoomFrame f;
    if (!readFrame(&f)) return;
    const auto verdict = static_cast<sz::LensVerdict>(f.verdict);
    if (verdict == sz::LensVerdict::kIdle) return;
    if (verdict == sz::LensVerdict::kNoS) {
        count([](sz::LensDoorStats& s) { ++s.noS; });
        return;
    }
    if (verdict == sz::LensVerdict::kNotInTable) {
        count([](sz::LensDoorStats& s) { ++s.notInTable; });
        return;
    }
    // A qualifying frame. Its c must be this young and on the picture.
    const uint64_t t = nowUs();
    const bool young = t >= f.stampUs && sz::lensFresh(t - f.stampUs);
    const bool usable = std::isfinite(f.uv[0]) && std::isfinite(f.uv[1]) && std::isfinite(f.jitterUv[0]) && std::isfinite(f.jitterUv[1]) && std::isfinite(f.m) && f.m > 0.0f &&
                        f.uv[0] >= 0.0f && f.uv[0] <= 1.0f && f.uv[1] >= 0.0f && f.uv[1] <= 1.0f && sz::tangentsUsable(f.tan4);
    if (!young || !usable) {
        count([](sz::LensDoorStats& s) { ++s.staleC; });
        return;
    }
    if (!(f.m >= 1.001f)) {   // the first frame of the ease: M is 1 and there is nothing to do
        count([](sz::LensDoorStats& s) { ++s.noop; });
        return;
    }
    if (!a.resolved) {   // the game's own picture, not the upscaler's: the frame route refuses it
        count([](sz::LensDoorStats& s) { ++s.formatRefused; });
        return;
    }
    const ScopeZoomDepth depth = g_scopeZoomDepth[1];
    if (!depth.srv || !depth.w || !depth.h || !(depth.nearDepth > 0.0f)) {
        count([](sz::LensDoorStats& s) { ++s.depthUnavailable; });
        return;
    }

    D3D11_TEXTURE2D_DESC fd{};
    a.frame->GetDesc(&fd);
    const DXGI_FORMAT view = uiLayerFrameView(fd.Format);
    const uint32_t* region = a.region;
    if (view == DXGI_FORMAT_UNKNOWN || fd.SampleDesc.Count != 1 || fd.ArraySize != 1 || fd.MipLevels != 1 || region[2] <= region[0] || region[3] <= region[1] ||
        region[2] > fd.Width || region[3] > fd.Height || !ensureDevice(dev.Get(), ctx.Get())) {
        count([](sz::LensDoorStats& s) { ++s.formatRefused; });
        return;
    }
    const double regionW = region[2] - region[0], regionH = region[3] - region[1];
    const double cx = region[0] + (static_cast<double>(f.uv[0]) - f.jitterUv[0]) * regionW;
    const double cy = region[1] + (static_cast<double>(f.uv[1]) - f.jitterUv[1]) * regionH;
    double tc[2];
    sz::lensTangentAtUv(f.tan4, f.uv[0], f.uv[1], tc);
    const double pxPerTanX = regionW / (static_cast<double>(f.tan4[1]) - f.tan4[0]), pxPerTanY = regionH / (static_cast<double>(f.tan4[3]) - f.tan4[2]);
    const sz::LensWindow wnd = sz::lensWindow(cx, cy, tc[0], tc[1], pxPerTanX, pxPerTanY, region);
    if (wnd.empty() || !(cx >= region[0] && cx < region[2] && cy >= region[1] && cy < region[3])) {   // c is not on the picture
        count([](sz::LensDoorStats& s) { ++s.staleC; });
        return;
    }
    const uint32_t winW = static_cast<uint32_t>(wnd.x1 - wnd.x0), winH = static_cast<uint32_t>(wnd.y1 - wnd.y0);

    // The destination: the frame itself through its UAV, or EDVR's own copy of it.
    const bool inPlace = a.inPlace && (fd.BindFlags & D3D11_BIND_UNORDERED_ACCESS);
    ID3D11UnorderedAccessView* destUav = nullptr;
    if (inPlace) {
        if (g_s.frameUavRes != static_cast<void*>(a.frame) || !g_s.frameUav || g_s.frameUavView != view) {
            g_s.frameUav.Reset();
            g_s.frameUavHold.Reset();
            g_s.frameUavRes = nullptr;
            D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};
            ud.Format = view;
            ud.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
            if (FAILED(dev->CreateUnorderedAccessView(a.frame, &ud, g_s.frameUav.ReleaseAndGetAddressOf())) || !g_s.frameUav) {
                g_s.frameUav.Reset();
                count([](sz::LensDoorStats& s) { ++s.formatRefused; });
                return;
            }
            g_s.frameUavRes = a.frame;
            g_s.frameUavHold = a.frame;
            g_s.frameUavView = view;
        }
        destUav = g_s.frameUav.Get();
    } else {
        if (!g_s.ownTex || g_s.ownW != fd.Width || g_s.ownH != fd.Height || g_s.ownFmt != fd.Format) {
            g_s.ownW = g_s.ownH = 0;
            if (!makeTex(dev.Get(), fd.Width, fd.Height, fd.Format, view, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS, &g_s.ownTex, nullptr, &g_s.ownUav)) {
                g_s.ownTex.Reset();
                g_s.ownUav.Reset();
                count([](sz::LensDoorStats& s) { ++s.formatRefused; });
                return;
            }
            g_s.ownW = fd.Width;
            g_s.ownH = fd.Height;
            g_s.ownFmt = fd.Format;
        }
        destUav = g_s.ownUav.Get();
    }
    // The copy of the window: the one image pass 2 reads. Its texture only grows.
    if (!g_s.rcTex || g_s.rcW < winW || g_s.rcH < winH || g_s.rcFmt != fd.Format) {
        const uint32_t w = (std::max(winW, g_s.rcFmt == fd.Format ? g_s.rcW : 0u) + 63u) & ~63u, h = (std::max(winH, g_s.rcFmt == fd.Format ? g_s.rcH : 0u) + 63u) & ~63u;
        g_s.rcW = g_s.rcH = 0;
        if (!makeTex(dev.Get(), w, h, fd.Format, view, D3D11_BIND_SHADER_RESOURCE, &g_s.rcTex, &g_s.rcSrv, nullptr)) {
            g_s.rcTex.Reset();
            g_s.rcSrv.Reset();
            count([](sz::LensDoorStats& s) { ++s.formatRefused; });
            return;
        }
        g_s.rcW = w;
        g_s.rcH = h;
        g_s.rcFmt = fd.Format;
    }

    ScopeZoomRaysParams p1{};
    p1.cRen[0] = static_cast<float>(f.uv[0] * depth.w);
    p1.cRen[1] = static_cast<float>(f.uv[1] * depth.h);
    p1.cRen[2] = static_cast<float>(depth.w);
    p1.cRen[3] = static_cast<float>(depth.h);
    p1.cOut[0] = static_cast<float>(cx);
    p1.cOut[1] = static_cast<float>(cy);
    p1.cOut[2] = static_cast<float>(regionW);
    p1.cOut[3] = static_cast<float>(regionH);
    std::memcpy(p1.tanE, f.tan4, sizeof(p1.tanE));
    p1.win[0] = static_cast<float>(wnd.x0);
    p1.win[1] = static_cast<float>(wnd.y0);
    p1.win[2] = static_cast<float>(wnd.x1);
    p1.win[3] = static_cast<float>(wnd.y1);
    p1.knobs[0] = depth.nearDepth;
    p1.knobs[1] = static_cast<float>(std::cos(sz::kLensMaxDeg / sz::kDeg));
    p1.knobs[2] = static_cast<float>(sz::kLensMaxEscaped);
    p1.knobs[3] = static_cast<float>(sz::kLensStepCap);
    ScopeZoomMagnifyParams p2{};
    p2.cM[0] = static_cast<float>(cx);
    p2.cM[1] = static_cast<float>(cy);
    p2.cM[2] = f.m;
    p2.cM[3] = static_cast<float>(sz::kLensFeatherPx);
    p2.win[0] = static_cast<float>(wnd.x0);
    p2.win[1] = static_cast<float>(wnd.y0);
    p2.win[2] = static_cast<float>(winW);
    p2.win[3] = static_cast<float>(winH);
    p2.scale[0] = static_cast<float>(regionW / depth.w);
    p2.scale[1] = static_cast<float>(regionH / depth.h);
    if (!writeCb(ctx.Get(), g_s.cbRays.Get(), &p1, sizeof(p1)) || !writeCb(ctx.Get(), g_s.cbMag.Get(), &p2, sizeof(p2))) {
        count([](sz::LensDoorStats& s) { ++s.formatRefused; });
        return;
    }

    {
        CensusSpan census(ctx.Get());
        TimerSlot* timer = acquireTimer(dev.Get(), ctx.Get());
        StageSave saved;
        saved.save(ctx.Get(), true);
        ID3D11ShaderResourceView* nullSrv[2] = {};
        ID3D11UnorderedAccessView* nullUav = nullptr;
        // Pass 1. The game's depth target may still be bound at the output merger, and D3D nulls a shader view over a bound target: the stage is cleared for the dispatch.
        ctx->OMSetRenderTargets(0, nullptr, nullptr);
        ctx->CSSetShaderResources(0, 2, nullSrv);
        ctx->CSSetUnorderedAccessViews(0, 1, &nullUav, nullptr);
        beginTimer(timer, 0, dev.Get(), ctx.Get());
        ctx->CSSetShader(g_s.csRays.Get(), nullptr, 0);
        ID3D11Buffer* cb1 = g_s.cbRays.Get();
        ctx->CSSetConstantBuffers(0, 1, &cb1);
        ID3D11ShaderResourceView* zSrv[2] = {depth.srv, nullptr};
        ctx->CSSetShaderResources(0, 2, zSrv);
        ID3D11UnorderedAccessView* lensUav = g_s.lensUav.Get();
        ctx->CSSetUnorderedAccessViews(0, 1, &lensUav, nullptr);
        ctx->Dispatch(1, 1, 1);
        ctx->CSSetShaderResources(0, 2, nullSrv);
        ctx->CSSetUnorderedAccessViews(0, 1, &nullUav, nullptr);
        endTimer(timer, 0, ctx.Get());
        // The rays, for the press line: a staging copy mapped later without waiting.
        if (StageSlot* st = freeStage()) {
            ctx->CopyResource(st->buf.Get(), g_s.lens.Get());
            st->inUse = true;
            st->press = f.press;
            st->settled = f.m >= f.mTarget * 0.999f;
            g_inflight.fetch_add(1, std::memory_order_relaxed);
        }
        // Pass 2. The window of the picture is copied (and, for a frame EDVR may not write, the whole of it into the copy pass 2 writes).
        beginTimer(timer, 1, dev.Get(), ctx.Get());
        const D3D11_BOX box{static_cast<UINT>(wnd.x0), static_cast<UINT>(wnd.y0), 0, static_cast<UINT>(wnd.x1), static_cast<UINT>(wnd.y1), 1};
        ctx->CopySubresourceRegion(g_s.rcTex.Get(), 0, 0, 0, 0, a.frame, 0, &box);
        if (!inPlace) ctx->CopyResource(g_s.ownTex.Get(), a.frame);
        ctx->CSSetShader(g_s.csMag.Get(), nullptr, 0);
        ID3D11Buffer* cb2 = g_s.cbMag.Get();
        ctx->CSSetConstantBuffers(0, 1, &cb2);
        ID3D11SamplerState* smp = g_s.lin.Get();
        ctx->CSSetSamplers(0, 1, &smp);
        ID3D11ShaderResourceView* mSrv[2] = {g_s.rcSrv.Get(), g_s.lensSrv.Get()};
        ctx->CSSetShaderResources(0, 2, mSrv);
        ctx->CSSetUnorderedAccessViews(0, 1, &destUav, nullptr);
        ctx->DispatchIndirect(g_s.lens.Get(), sz::kLensOffArgs);
        ctx->CSSetShaderResources(0, 2, nullSrv);
        ctx->CSSetUnorderedAccessViews(0, 1, &nullUav, nullptr);
        endTimer(timer, 1, ctx.Get());
        saved.restore(ctx.Get());
    }
    if (!inPlace && copy) *copy = g_s.ownTex;
    count([&](sz::LensDoorStats& s) {
        ++s.dispatched;
        sz::lensNoteC(&s, cx, cy);
        s.depthNear = depth.nearDepth;
    });
    if (!g_firstNoted) {
        g_firstNoted = true;
        Log::get().note("scope lens: the right eye's picture (%ux%u, DXGI format %d) is magnified %s, M %.2f about c (%.1f, %.1f) px, the depth the upscaler used (%ux%u) read for the hole.",
                        fd.Width, fd.Height, static_cast<int>(fd.Format), inPlace ? "in place" : "in EDVR's copy of it", static_cast<double>(f.m), cx, cy, depth.w, depth.h);
    }
}

}  // namespace edvr
