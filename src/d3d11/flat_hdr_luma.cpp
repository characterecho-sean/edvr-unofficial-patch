// The flat HDR route's luminance census (flat_hdr_luma.h). TEMPORARY: the 2026-10-09 DLAA dimming entry.
#include "flat_hdr_luma.h"

#include "flat_compute_readback.h"  // FlatComputeInternalScope
#include "shader_swap.h"
#include "temporal_shader_bytecode.h"
#include "../common/log.h"

#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>

#include <cstdio>
#include <cstring>

namespace edvr {
namespace {

template <class T>
using Ptr = Microsoft::WRL::ComPtr<T>;

// One sample: the pass over H before the resolve (side 0) and after it (side 1), each its own buffer and staging copy.
struct Slot {
    Ptr<ID3D11Buffer> buffer[2], stage[2];
    Ptr<ID3D11UnorderedAccessView> uav[2];
    enum State { Free, InCounted, Pending } state = Free;
    uint32_t width = 0, height = 0;
    char backend[16] = {};
    unsigned preset = 0;
    bool fixed = false;
    float threshold = 0;
    ULONGLONG queuedMs = 0;
};

struct Window {
    unsigned samples = 0;
    uint32_t width = 0, height = 0;
    char backend[16] = {};
    unsigned preset = 0;
    bool fixed = false;
    double inMean = 0, outMean = 0, inP99 = 0, outP99 = 0, inP999 = 0, outP999 = 0, inMax = 0, outMax = 0;
    double starsIn = 0, starsOut = 0, starsScaledOut = 0, peaksIn = 0, peaksOut = 0, threshold = 0;
    uint32_t badIn = 0, badOut = 0;
};

ID3D11Device* g_device = nullptr;   // identity only: a different device drops everything below
Ptr<ID3D11ComputeShader> g_cs;
Ptr<ID3D11Buffer> g_params;
Slot g_slots[2];
int g_active = -1;                  // the slot whose input was counted this frame
ULONGLONG g_lastSampleMs = 0, g_windowStartMs = 0;
double g_lastInMean = 0, g_lastRatio = 1;
Window g_window;
uint32_t g_hazards = 0, g_dropped = 0, g_failures = 0;
bool g_armedSaid = false;

void releaseAll() {
    for (Slot& s : g_slots) s = Slot{};
    g_cs.Reset(); g_params.Reset(); g_device = nullptr; g_active = -1;
}

bool ready(ID3D11DeviceContext* ctx) {
    Ptr<ID3D11Device> dev; ctx->GetDevice(&dev);
    if (!dev) return false;
    if (dev.Get() != g_device) { releaseAll(); g_device = dev.Get(); }
    if (!g_cs) g_cs.Attach(shaderSwapCreateCs(ctx, kFlatHdrLumaBytecode, sizeof(kFlatHdrLumaBytecode), "flat hdr luma census", "flat hdr luma"));
    if (!g_cs) return false;
    if (!g_params) {
        D3D11_BUFFER_DESC bd{}; bd.ByteWidth = 32; bd.Usage = D3D11_USAGE_DEFAULT; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        if (FAILED(dev->CreateBuffer(&bd, nullptr, &g_params))) return false;
    }
    for (Slot& s : g_slots) for (int side = 0; side < 2; ++side) {
        if (s.buffer[side]) continue;
        D3D11_BUFFER_DESC bd{}; bd.ByteWidth = kFlatHdrLumaWords * 4; bd.Usage = D3D11_USAGE_DEFAULT;
        bd.BindFlags = D3D11_BIND_UNORDERED_ACCESS; bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
        if (FAILED(dev->CreateBuffer(&bd, nullptr, &s.buffer[side]))) return false;
        D3D11_UNORDERED_ACCESS_VIEW_DESC ud{}; ud.Format = DXGI_FORMAT_R32_TYPELESS; ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        ud.Buffer.NumElements = kFlatHdrLumaWords; ud.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
        if (FAILED(dev->CreateUnorderedAccessView(s.buffer[side].Get(), &ud, &s.uav[side]))) return false;
        D3D11_BUFFER_DESC sd{}; sd.ByteWidth = kFlatHdrLumaWords * 4; sd.Usage = D3D11_USAGE_STAGING; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(dev->CreateBuffer(&sd, nullptr, &s.stage[side]))) return false;
    }
    return true;
}

// One counting pass of `h` into the slot's side; the game's CS slot 0 (shader, SRV, UAV, CB) is put back exactly.
bool count(ID3D11DeviceContext* ctx, ID3D11ShaderResourceView* h, Slot& s, int side, float thresholdB) {
    D3D11_SHADER_RESOURCE_VIEW_DESC vd{}; h->GetDesc(&vd);
    if (vd.ViewDimension != D3D11_SRV_DIMENSION_TEXTURE2D) { ++g_failures; return false; }
    const float floorL = g_lastInMean > 0 ? static_cast<float>(0.01 * g_lastInMean) : 1e-4f;
    const float params[8] = {0, 0, s.threshold, thresholdB, floorL, 0, 0, 0};
    float packed[8]; std::memcpy(packed, params, sizeof(packed));
    std::memcpy(&packed[0], &s.width, 4); std::memcpy(&packed[1], &s.height, 4);
    ctx->UpdateSubresource(g_params.Get(), 0, nullptr, packed, 0, 0);
    Ptr<ID3D11ComputeShader> prior; ID3D11ClassInstance* classes[256]{}; UINT classCount = 256;
    ctx->CSGetShader(&prior, classes, &classCount);
    Ptr<ID3D11ShaderResourceView> savedSrv; ctx->CSGetShaderResources(0, 1, &savedSrv);
    Ptr<ID3D11UnorderedAccessView> savedUav; ctx->CSGetUnorderedAccessViews(0, 1, &savedUav);
    Ptr<ID3D11Buffer> savedCb; ctx->CSGetConstantBuffers(0, 1, &savedCb);
    const UINT zero[4] = {0, 0, 0, 0};
    ctx->ClearUnorderedAccessViewUint(s.uav[side].Get(), zero);
    ID3D11Buffer* cb = g_params.Get(); ID3D11UnorderedAccessView* uav = s.uav[side].Get();
    ctx->CSSetShader(g_cs.Get(), nullptr, 0); ctx->CSSetConstantBuffers(0, 1, &cb);
    ctx->CSSetShaderResources(0, 1, &h); ctx->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
    // A target still bound for output refuses the read silently (the view reads back null): counted, not dispatched.
    Ptr<ID3D11ShaderResourceView> bound; ctx->CSGetShaderResources(0, 1, &bound);
    const bool ok = bound.Get() == h;
    if (ok) ctx->Dispatch(32, 32, 1); else ++g_hazards;
    ID3D11ShaderResourceView* nullSrv = nullptr; ID3D11UnorderedAccessView* nullUav = nullptr;
    ctx->CSSetShaderResources(0, 1, &nullSrv); ctx->CSSetUnorderedAccessViews(0, 1, &nullUav, nullptr);
    ctx->CSSetShader(prior.Get(), classes, classCount);
    ID3D11Buffer* oldCb = savedCb.Get(); ctx->CSSetConstantBuffers(0, 1, &oldCb);
    ID3D11ShaderResourceView* oldSrv = savedSrv.Get(); ctx->CSSetShaderResources(0, 1, &oldSrv);
    ID3D11UnorderedAccessView* oldUav = savedUav.Get(); UINT keep = ~0u; ctx->CSSetUnorderedAccessViews(0, 1, &oldUav, &keep);
    for (UINT i = 0; i < classCount; ++i) if (classes[i]) classes[i]->Release();
    if (ok) ctx->CopyResource(s.stage[side].Get(), s.buffer[side].Get());
    return ok;
}

void flush(ULONGLONG now) {
    Window& w = g_window;
    if (w.samples) {
        const double n = w.samples;
        Log::get().note("flat hdr luma: backend=%s preset=%s exposure=%s samples=%u size=%ux%u in-mean=%.5g out-mean=%.5g ratio=%.4f "
                        "in-p99=%.4g out-p99=%.4g in-p99.9=%.4g out-p99.9=%.4g in-max=%.4g out-max=%.4g "
                        "stars-in=%.1f stars-out=%.1f stars-scaled-out=%.1f peaks-in=%.1f peaks-out=%.1f star-threshold=%.4g "
                        "bad-in=%u bad-out=%u hazards=%u dropped=%u failures=%u",
                        w.backend, flatHdrLumaPresetName(w.preset), w.fixed ? "fixed" : "auto", w.samples, w.width, w.height,
                        w.inMean / n, w.outMean / n, w.inMean > 0 ? w.outMean / w.inMean : 0.0,
                        w.inP99 / n, w.outP99 / n, w.inP999 / n, w.outP999 / n, w.inMax, w.outMax,
                        w.starsIn / n, w.starsOut / n, w.starsScaledOut / n, w.peaksIn / n, w.peaksOut / n, w.threshold / n,
                        w.badIn, w.badOut, g_hazards, g_dropped, g_failures);
    }
    g_window = Window{};
    g_windowStartMs = now;
}

}  // namespace

bool flatHdrLumaBegin(ID3D11DeviceContext* ctx, ID3D11ShaderResourceView* h, uint32_t width, uint32_t height) {
    g_active = -1;
    if (!ctx || !h || !width || !height) return false;
    const ULONGLONG now = GetTickCount64();
    if (g_lastSampleMs && now - g_lastSampleMs < 1000) return false;
    int slot = -1;
    for (int i = 0; i < 2; ++i) if (g_slots[i].state == Slot::Free) { slot = i; break; }
    if (slot < 0) { ++g_dropped; g_lastSampleMs = now; return false; }
    FlatComputeInternalScope internal;
    if (!ready(ctx)) { ++g_failures; g_lastSampleMs = now; return false; }
    if (!g_armedSaid) {
        g_armedSaid = true;
        Log::get().note("flat hdr luma: armed (temporary instrument, the DLAA dimming entry): H before and after the resolve, once a second, "
                        "one line every 5 s");
    }
    Slot& s = g_slots[slot];
    s.width = width; s.height = height;
    s.threshold = g_lastInMean > 0 ? static_cast<float>(4.0 * g_lastInMean) : 1.0f;
    g_lastSampleMs = now;
    if (!count(ctx, h, s, 0, s.threshold)) return false;
    s.state = Slot::InCounted; g_active = slot;
    return true;
}

void flatHdrLumaEnd(ID3D11DeviceContext* ctx, ID3D11ShaderResourceView* h, const char* backend, unsigned preset, bool exposureFixed) {
    if (g_active < 0 || !ctx || !h) return;
    Slot& s = g_slots[g_active]; g_active = -1;
    FlatComputeInternalScope internal;
    std::snprintf(s.backend, sizeof(s.backend), "%s", backend ? backend : "unknown");
    s.preset = preset; s.fixed = exposureFixed;
    if (!count(ctx, h, s, 1, static_cast<float>(s.threshold * g_lastRatio))) { s.state = Slot::Free; return; }
    s.state = Slot::Pending; s.queuedMs = GetTickCount64();
}

void flatHdrLumaAbandon() {
    if (g_active >= 0) g_slots[g_active].state = Slot::Free;
    g_active = -1;
}

void flatHdrLumaPoll(ID3D11DeviceContext* ctx) {
    if (!ctx) return;
    const ULONGLONG now = GetTickCount64();
    if (!g_windowStartMs) g_windowStartMs = now;
    FlatComputeInternalScope internal;
    for (Slot& s : g_slots) {
        if (s.state != Slot::Pending) continue;
        D3D11_MAPPED_SUBRESOURCE in{}, out{};
        const HRESULT hrIn = ctx->Map(s.stage[0].Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &in);
        if (hrIn == DXGI_ERROR_WAS_STILL_DRAWING) continue;
        const HRESULT hrOut = SUCCEEDED(hrIn) ? ctx->Map(s.stage[1].Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &out) : hrIn;
        if (hrOut == DXGI_ERROR_WAS_STILL_DRAWING) { ctx->Unmap(s.stage[0].Get(), 0); continue; }
        if (FAILED(hrIn) || FAILED(hrOut)) {
            if (SUCCEEDED(hrIn)) ctx->Unmap(s.stage[0].Get(), 0);
            ++g_failures; s.state = Slot::Free; continue;
        }
        const FlatHdrLumaSample a = flatHdrLumaDecode(static_cast<const uint32_t*>(in.pData), s.width, s.height);
        const FlatHdrLumaSample b = flatHdrLumaDecode(static_cast<const uint32_t*>(out.pData), s.width, s.height);
        ctx->Unmap(s.stage[1].Get(), 0); ctx->Unmap(s.stage[0].Get(), 0);
        s.state = Slot::Free;
        Window& w = g_window;
        if (w.samples && (std::strcmp(w.backend, s.backend) || w.preset != s.preset || w.fixed != s.fixed ||
                          w.width != s.width || w.height != s.height))
            flush(now);   // a change of backend, preset, exposure or size closes the window: a line never mixes two
        std::memcpy(w.backend, s.backend, sizeof(w.backend)); w.preset = s.preset; w.fixed = s.fixed;
        w.width = s.width; w.height = s.height;
        ++w.samples;
        w.inMean += a.mean; w.outMean += b.mean; w.inP99 += a.p99; w.outP99 += b.p99; w.inP999 += a.p999; w.outP999 += b.p999;
        if (a.max > w.inMax) w.inMax = a.max;
        if (b.max > w.outMax) w.outMax = b.max;
        w.starsIn += a.starsA; w.starsOut += b.starsA; w.starsScaledOut += b.starsB; w.peaksIn += a.peaks; w.peaksOut += b.peaks;
        w.threshold += s.threshold; w.badIn += a.bad; w.badOut += b.bad;
        if (a.mean > 0) { g_lastInMean = a.mean; g_lastRatio = b.mean > 0 ? b.mean / a.mean : 1.0; }
    }
    if (now - g_windowStartMs >= 5000) flush(now);
}

}  // namespace edvr
