// TEMPORARY instrument: the cockpit glass probe. cockpit_glass_probe.h says what it logs and why.
#include "cockpit_glass_probe.h"

#include "binding_shadow.h"            // bindingShaderHash: the hashes the draw path already holds
#include "cockpit_glass_probe_core.h"
#include "flat_compute_readback.h"     // FlatComputeInternalScope: this probe's own copies and maps step past vscreen's hooks

#include "../common/config.h"
#include "../common/log.h"

#include <windows.h>
#include <d3d11_1.h>
#include <wrl/client.h>

#include <cmath>
#include <cstdio>
#include <cstring>

namespace edvr {

namespace detail {
bool g_cockpitGlassProbeLive = false;
}  // namespace detail

namespace {

template <class T>
using Ptr = Microsoft::WRL::ComPtr<T>;
using namespace glass_probe;

constexpr ULONGLONG kIntervalMs = 30000;
constexpr uint32_t kSampleCap = 12;
constexpr uint32_t kReadbackDelayFrames = 2;
constexpr uint32_t kReadbackGiveUpFrames = 120;
constexpr UINT kSrvSlots = 12;
constexpr UINT kSamplerSlots = 3;
constexpr UINT kCbMaxBytes = 16u << 20;   // a whole-buffer copy of anything larger is refused and said
constexpr UINT kListMaxBytes = 128u << 20;   // t0 was 43.7 MB in the probe flight of 2026-11-03; a copy clips here, and the grid line then says the buffer held fewer clusters
constexpr uint32_t kLightsPrinted = 8;       // lights per list printed in full, from the depths the glass lies at
constexpr float kGlassNearM = 0.5f, kGlassFarM = 8.0f;   // the depths taken as the glass's, for choosing which lights to print

bool g_configRead = false;
uint32_t g_configGeneration = 0;
bool g_armed = false;

uint64_t g_frame = 0;
ULONGLONG g_windowStart = 0;
uint32_t g_psDraws[kCanopyPsCount] = {};
uint32_t g_vsDraws[kCanopyVsCount] = {};
uint32_t g_otherPsDraws = 0;   // canopy VS, PS none of the nine
uint64_t g_otherPs[4] = {};
uint32_t g_windowDraws = 0;

uint32_t g_samples = 0;
ULONGLONG g_lastSample = 0;

// One readback: a staging copy made at the draw, mapped at a later boundary.
struct Readback {
    Ptr<ID3D11Buffer> staging;
    UINT offset = 0;   // where register 0 (or byte 0) of the bound range sits in the staging copy
    UINT bytes = 0;    // bytes of the bound range present from there
    bool done = true;
};
struct Pending {
    bool on = false;
    uint32_t sample = 0;
    uint64_t frame = 0;
    Readback cb1, cb2, light, lists, records;
    bool cb1Kept = false;   // cb1Bytes holds the registers the lists are read with
    uint8_t cb1Bytes[kCb1Regs * 16] = {};
} g_pending;

const char* blendName(D3D11_BLEND b) {
    static const char* const k[] = {"?", "ZERO", "ONE", "SRC_COLOR", "INV_SRC_COLOR", "SRC_ALPHA", "INV_SRC_ALPHA", "DEST_ALPHA",
                                    "INV_DEST_ALPHA", "DEST_COLOR", "INV_DEST_COLOR", "SRC_ALPHA_SAT", "?", "?", "BLEND_FACTOR",
                                    "INV_BLEND_FACTOR", "SRC1_COLOR", "INV_SRC1_COLOR", "SRC1_ALPHA", "INV_SRC1_ALPHA"};
    const unsigned i = static_cast<unsigned>(b);
    return i < sizeof k / sizeof k[0] ? k[i] : "?";
}

void resetWindow(ULONGLONG now) {
    g_windowStart = now;
    for (auto& c : g_psDraws) c = 0;
    for (auto& c : g_vsDraws) c = 0;
    for (auto& h : g_otherPs) h = 0;
    g_otherPsDraws = 0;
    g_windowDraws = 0;
}

void logWindow(ULONGLONG now) {
    char ps[400], vs[160];
    size_t n = 0;
    ps[0] = vs[0] = '\0';
    for (int i = 0; i < kCanopyPsCount; ++i)
        if (g_psDraws[i] && n < sizeof ps)
            n += std::snprintf(ps + n, sizeof ps - n, " %016llX(%s)=%u", static_cast<unsigned long long>(kCanopyPs[i].hash),
                               kCanopyPs[i].name, g_psDraws[i]);
    n = 0;
    for (int i = 0; i < kCanopyVsCount; ++i)
        if (g_vsDraws[i] && n < sizeof vs)
            n += std::snprintf(vs + n, sizeof vs - n, " %016llX(%s)=%u", static_cast<unsigned long long>(kCanopyVs[i].hash),
                               kCanopyVs[i].name, g_vsDraws[i]);
    Log::get().note("glass probe: window %.0f s draws=%u ps:%s vs:%s other-ps=%u [%016llX %016llX %016llX %016llX] samples=%u/%u",
                    (now - g_windowStart) / 1000.0, g_windowDraws, ps[0] ? ps : " none", vs[0] ? vs : " none", g_otherPsDraws,
                    static_cast<unsigned long long>(g_otherPs[0]), static_cast<unsigned long long>(g_otherPs[1]),
                    static_cast<unsigned long long>(g_otherPs[2]), static_cast<unsigned long long>(g_otherPs[3]), g_samples, kSampleCap);
}

// A staging copy of `src` (a buffer), bytes [first, first + want) clipped to its size. A constant buffer is copied whole (a box on a
// constant buffer source is not something every driver takes); `offset` then says where `first` sits in the copy.
bool copyBuffer(ID3D11DeviceContext* ctx, ID3D11Buffer* src, UINT first, UINT want, bool whole, Readback* out, const char* what,
                uint32_t sample) {
    *out = Readback{};
    if (!src) return false;
    D3D11_BUFFER_DESC sd{};
    src->GetDesc(&sd);
    if (first >= sd.ByteWidth) {
        Log::get().note("glass probe: sample %u %s not copied: the bound range starts at byte %u of a %u-byte buffer", sample, what, first,
                        sd.ByteWidth);
        return false;
    }
    if (whole && sd.ByteWidth > kCbMaxBytes) {
        Log::get().note("glass probe: sample %u %s not copied: %u bytes is past the probe's %u-byte cap", sample, what, sd.ByteWidth,
                        kCbMaxBytes);
        return false;
    }
    const UINT avail = sd.ByteWidth - first;
    const UINT take = want < avail ? want : avail;
    Ptr<ID3D11Device> dev;
    ctx->GetDevice(&dev);
    D3D11_BUFFER_DESC d{};
    d.ByteWidth = whole ? sd.ByteWidth : take;
    d.Usage = D3D11_USAGE_STAGING;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    const HRESULT hr = dev->CreateBuffer(&d, nullptr, &out->staging);
    if (FAILED(hr)) {
        Log::get().note("glass probe: sample %u %s not copied: staging buffer of %u bytes failed (0x%08lX)", sample, what, d.ByteWidth,
                        static_cast<unsigned long>(hr));
        out->staging.Reset();
        return false;
    }
    if (whole) {
        ctx->CopyResource(out->staging.Get(), src);
        out->offset = first;
    } else {
        const D3D11_BOX box{first, 0, 0, first + take, 1, 1};
        ctx->CopySubresourceRegion(out->staging.Get(), 0, 0, 0, 0, src, 0, &box);
        out->offset = 0;
    }
    out->bytes = take;
    out->done = false;
    return true;
}

void logOm(ID3D11DeviceContext* ctx, uint32_t sample) {
    Ptr<ID3D11BlendState> bs;
    FLOAT factor[4] = {};
    UINT mask = 0;
    ctx->OMGetBlendState(&bs, factor, &mask);
    if (bs) {
        D3D11_BLEND_DESC d{};
        bs->GetDesc(&d);
        for (int i = 0; i < 2; ++i) {
            const auto& r = d.RenderTarget[i];
            Log::get().note("glass probe: sample %u om blend rt%d enable=%d %s*src %s %s*dst (op %d) alpha %s*src %s*dst (op %d) write=0x%X "
                            "a2c=%d independent=%d factor=%.3g,%.3g,%.3g,%.3g samplemask=0x%X",
                            sample, i, r.BlendEnable ? 1 : 0, blendName(r.SrcBlend), "+", blendName(r.DestBlend),
                            static_cast<int>(r.BlendOp), blendName(r.SrcBlendAlpha), blendName(r.DestBlendAlpha),
                            static_cast<int>(r.BlendOpAlpha), r.RenderTargetWriteMask, d.AlphaToCoverageEnable ? 1 : 0,
                            d.IndependentBlendEnable ? 1 : 0, factor[0], factor[1], factor[2], factor[3], mask);
        }
    } else {
        Log::get().note("glass probe: sample %u om blend none (opaque default)", sample);
    }
    Ptr<ID3D11DepthStencilState> ds;
    UINT ref = 0;
    ctx->OMGetDepthStencilState(&ds, &ref);
    if (ds) {
        D3D11_DEPTH_STENCIL_DESC d{};
        ds->GetDesc(&d);
        Log::get().note("glass probe: sample %u om depth enable=%d write=%d func=%d stencil=%d read=0x%X write=0x%X ref=%u",
                        sample, d.DepthEnable ? 1 : 0, static_cast<int>(d.DepthWriteMask), static_cast<int>(d.DepthFunc),
                        d.StencilEnable ? 1 : 0, d.StencilReadMask, d.StencilWriteMask, ref);
    } else {
        Log::get().note("glass probe: sample %u om depth none (default: test less, write all)", sample);
    }
    Ptr<ID3D11RasterizerState> rs;
    ctx->RSGetState(&rs);
    if (rs) {
        D3D11_RASTERIZER_DESC d{};
        rs->GetDesc(&d);
        Log::get().note("glass probe: sample %u om raster fill=%d cull=%d frontccw=%d bias=%d slope=%.3g clip=%d scissor=%d ms=%d", sample,
                        static_cast<int>(d.FillMode), static_cast<int>(d.CullMode), d.FrontCounterClockwise ? 1 : 0, d.DepthBias,
                        d.SlopeScaledDepthBias, d.DepthClipEnable ? 1 : 0, d.ScissorEnable ? 1 : 0, d.MultisampleEnable ? 1 : 0);
    } else {
        Log::get().note("glass probe: sample %u om raster none (default: solid, cull back)", sample);
    }
    ID3D11RenderTargetView* rtv[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
    ID3D11DepthStencilView* dsv = nullptr;
    ctx->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtv, &dsv);
    for (UINT i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i) {
        if (!rtv[i]) continue;
        D3D11_RENDER_TARGET_VIEW_DESC vd{};
        rtv[i]->GetDesc(&vd);
        Ptr<ID3D11Resource> res;
        rtv[i]->GetResource(&res);
        Ptr<ID3D11Texture2D> tex;
        D3D11_TEXTURE2D_DESC td{};
        if (res && SUCCEEDED(res.As(&tex))) tex->GetDesc(&td);
        Log::get().note("glass probe: sample %u om rtv%u fmt=%d dim=%d %ux%u samples=%u texfmt=%d", sample, i, static_cast<int>(vd.Format),
                        static_cast<int>(vd.ViewDimension), td.Width, td.Height, td.SampleDesc.Count, static_cast<int>(td.Format));
        rtv[i]->Release();
    }
    if (dsv) {
        D3D11_DEPTH_STENCIL_VIEW_DESC vd{};
        dsv->GetDesc(&vd);
        Ptr<ID3D11Resource> res;
        dsv->GetResource(&res);
        Ptr<ID3D11Texture2D> tex;
        D3D11_TEXTURE2D_DESC td{};
        if (res && SUCCEEDED(res.As(&tex))) tex->GetDesc(&td);
        Log::get().note("glass probe: sample %u om dsv fmt=%d flags=0x%X %ux%u samples=%u", sample, static_cast<int>(vd.Format), vd.Flags,
                        td.Width, td.Height, td.SampleDesc.Count);
        dsv->Release();
    } else {
        Log::get().note("glass probe: sample %u om dsv none", sample);
    }
}

// The PS's views; t1 (the directional-light buffer) also gets its copy, and t0 and t2 (the light lists and records) theirs. Returns whether t1's copy was made.
bool logSrvs(ID3D11DeviceContext* ctx, uint32_t sample, Readback* light, Readback* lists, Readback* records) {
    ID3D11ShaderResourceView* srv[kSrvSlots] = {};
    ctx->PSGetShaderResources(0, kSrvSlots, srv);
    bool copied = false;
    for (UINT i = 0; i < kSrvSlots; ++i) {
        if (!srv[i]) {
            Log::get().note("glass probe: sample %u srv t%u none", sample, i);
            continue;
        }
        D3D11_SHADER_RESOURCE_VIEW_DESC vd{};
        srv[i]->GetDesc(&vd);
        Ptr<ID3D11Resource> res;
        srv[i]->GetResource(&res);
        Ptr<ID3D11Buffer> buf;
        Ptr<ID3D11Texture2D> t2;
        if (res && SUCCEEDED(res.As(&buf))) {
            D3D11_BUFFER_DESC bd{};
            buf->GetDesc(&bd);
            UINT firstEl = 0, numEl = 0, flags = 0;
            if (vd.ViewDimension == D3D11_SRV_DIMENSION_BUFFEREX) {
                firstEl = vd.BufferEx.FirstElement;
                numEl = vd.BufferEx.NumElements;
                flags = vd.BufferEx.Flags;
            } else if (vd.ViewDimension == D3D11_SRV_DIMENSION_BUFFER) {
                firstEl = vd.Buffer.FirstElement;
                numEl = vd.Buffer.NumElements;
            }
            Log::get().note("glass probe: sample %u srv t%u buffer dim=%d fmt=%d first=%u count=%u flags=0x%X bytes=%u stride=%u usage=%d "
                            "bind=0x%X misc=0x%X",
                            sample, i, static_cast<int>(vd.ViewDimension), static_cast<int>(vd.Format), firstEl, numEl, flags,
                            bd.ByteWidth, bd.StructureByteStride, static_cast<int>(bd.Usage), bd.BindFlags, bd.MiscFlags);
            // A raw view counts 4-byte elements; a structured one counts its stride.
            const UINT unit = (flags & D3D11_BUFFEREX_SRV_FLAG_RAW) ? 4u : (bd.StructureByteStride ? bd.StructureByteStride : 4u);
            if (i == 1) copied = copyBuffer(ctx, buf.Get(), firstEl * unit, kLightBytes, false, light, "t1", sample);
            if (i == 0) copyBuffer(ctx, buf.Get(), firstEl * unit, kListMaxBytes, false, lists, "t0", sample);
            if (i == 2) copyBuffer(ctx, buf.Get(), firstEl * unit, kListMaxBytes, false, records, "t2", sample);
        } else if (res && SUCCEEDED(res.As(&t2))) {
            D3D11_TEXTURE2D_DESC td{};
            t2->GetDesc(&td);
            Log::get().note("glass probe: sample %u srv t%u tex2d dim=%d fmt=%d %ux%u mips=%u array=%u texfmt=%d samples=%u misc=0x%X", sample,
                            i, static_cast<int>(vd.ViewDimension), static_cast<int>(vd.Format), td.Width, td.Height, td.MipLevels,
                            td.ArraySize, static_cast<int>(td.Format), td.SampleDesc.Count, td.MiscFlags);
        } else {
            Log::get().note("glass probe: sample %u srv t%u other dim=%d fmt=%d", sample, i, static_cast<int>(vd.ViewDimension),
                            static_cast<int>(vd.Format));
        }
        srv[i]->Release();
    }
    if (!copied) Log::get().note("glass probe: sample %u t1 not copied (no buffer view in t1)", sample);
    return copied;
}

void logSamplers(ID3D11DeviceContext* ctx, uint32_t sample) {
    ID3D11SamplerState* s[kSamplerSlots] = {};
    ctx->PSGetSamplers(0, kSamplerSlots, s);
    for (UINT i = 0; i < kSamplerSlots; ++i) {
        if (!s[i]) {
            Log::get().note("glass probe: sample %u smp s%u none", sample, i);
            continue;
        }
        D3D11_SAMPLER_DESC d{};
        s[i]->GetDesc(&d);
        Log::get().note("glass probe: sample %u smp s%u filter=0x%X address=%d,%d,%d aniso=%u cmp=%d lod=%.3g..%.3g bias=%.3g", sample, i,
                        static_cast<unsigned>(d.Filter), static_cast<int>(d.AddressU), static_cast<int>(d.AddressV),
                        static_cast<int>(d.AddressW), d.MaxAnisotropy, static_cast<int>(d.ComparisonFunc), d.MinLOD, d.MaxLOD,
                        d.MipLODBias);
        s[i]->Release();
    }
}

// PS b1 and b2, with the bound range when the context is an 11.1 one (a bound range moves register 0 off byte 0).
void logAndCopyCbs(ID3D11DeviceContext* ctx, uint32_t sample, Readback* cb1, Readback* cb2) {
    ID3D11Buffer* cb[2] = {};
    UINT first[2] = {0, 0}, num[2] = {0, 0};
    bool ranged = false;
    Ptr<ID3D11DeviceContext1> ctx1;
    if (SUCCEEDED(ctx->QueryInterface(IID_PPV_ARGS(&ctx1))) && ctx1) {
        ctx1->PSGetConstantBuffers1(1, 2, cb, first, num);
        ranged = true;
    } else {
        ctx->PSGetConstantBuffers(1, 2, cb);
    }
    for (int i = 0; i < 2; ++i) {
        const char* what = i == 0 ? "cb1" : "cb2";
        if (!cb[i]) {
            Log::get().note("glass probe: sample %u cb b%d none", sample, i + 1);
            continue;
        }
        D3D11_BUFFER_DESC d{};
        cb[i]->GetDesc(&d);
        Log::get().note("glass probe: sample %u cb b%d bytes=%u usage=%d cpu=0x%X range=%s first=%u num=%u", sample, i + 1, d.ByteWidth,
                        static_cast<int>(d.Usage), d.CPUAccessFlags, ranged ? "reported" : "not-reported", first[i], num[i]);
        // A context that reports no range binds the whole buffer from register 0.
        const UINT regs = i == 0 ? kCb1Regs : kCb2Regs;
        const UINT want = (ranged && num[i] && num[i] < regs ? num[i] : regs) * 16;
        copyBuffer(ctx, cb[i], first[i] * 16, want, true, i == 0 ? cb1 : cb2, what, sample);
        cb[i]->Release();
    }
}

void takeSample(ID3D11DeviceContext* ctx, char kind, UINT count, UINT instances, uint64_t vsH, uint64_t psH, int vi, int pi,
                ULONGLONG now) {
    const uint32_t sample = ++g_samples;
    g_lastSample = now;
    Log::get().note("glass probe: sample %u draw frame=%llu kind=%c count=%u instances=%u vs=%016llX(%s) ps=%016llX(%s)", sample,
                    static_cast<unsigned long long>(g_frame), kind, count, instances, static_cast<unsigned long long>(vsH),
                    vi >= 0 ? kCanopyVs[vi].name : "not canopy", static_cast<unsigned long long>(psH),
                    pi >= 0 ? kCanopyPs[pi].name : "not canopy");
    FlatComputeInternalScope internal;   // the copies below are this probe's, not the game's
    logOm(ctx, sample);
    logSamplers(ctx, sample);
    Pending p;
    p.sample = sample;
    p.frame = g_frame;
    logSrvs(ctx, sample, &p.light, &p.lists, &p.records);
    logAndCopyCbs(ctx, sample, &p.cb1, &p.cb2);
    p.on = !p.cb1.done || !p.cb2.done || !p.light.done || !p.lists.done || !p.records.done;
    g_pending = p;
    if (!p.on) Log::get().note("glass probe: sample %u nothing to read back", sample);
}

void logReadback(const char* what, uint32_t sample, const uint8_t* data, UINT bytes, const RegSpan* spans, size_t spanCount,
                 uint32_t wholeRegs) {
    char line[1000];
    if (spans) {
        for (size_t s = 0; s < spanCount; ++s)
            for (uint32_t r = spans[s].first, end = uint32_t(spans[s].first) + spans[s].count; r < end; r += 4) {
                const uint32_t left = end - r;
                formatRegs(data, bytes, r, left < 4 ? left : 4, line, sizeof line);
                Log::get().note("glass probe: sample %u %s %s", sample, what, line);
            }
    } else {
        for (uint32_t r = 0; r < wholeRegs; r += 4) {
            formatRegs(data, bytes, r, wholeRegs - r < 4 ? wholeRegs - r : 4, line, sizeof line);
            Log::get().note("glass probe: sample %u %s %s", sample, what, line);
        }
    }
}

// True when this readback is finished (read, failed or nothing to read).
bool tryRead(ID3D11DeviceContext* ctx, Readback& rb, const char* what, uint32_t sample, const RegSpan* spans, size_t spanCount,
             uint32_t wholeRegs) {
    if (rb.done) return true;
    D3D11_MAPPED_SUBRESOURCE m{};
    const HRESULT hr = ctx->Map(rb.staging.Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m);
    if (hr == DXGI_ERROR_WAS_STILL_DRAWING) return false;
    rb.done = true;
    if (FAILED(hr)) {
        Log::get().note("glass probe: sample %u %s readback map failed (0x%08lX)", sample, what, static_cast<unsigned long>(hr));
        rb.staging.Reset();
        return true;
    }
    logReadback(what, sample, static_cast<const uint8_t*>(m.pData) + rb.offset, rb.bytes, spans, spanCount, wholeRegs);
    // The frame block is kept for the light lists, which are read with its grid and record offsets once their own copies are ready.
    if (&rb == &g_pending.cb1 && rb.bytes >= sizeof g_pending.cb1Bytes) {
        std::memcpy(g_pending.cb1Bytes, static_cast<const uint8_t*>(m.pData) + rb.offset, sizeof g_pending.cb1Bytes);
        g_pending.cb1Kept = true;
    }
    ctx->Unmap(rb.staging.Get(), 0);
    rb.staging.Reset();
    return true;
}

// What the light lists held at this canopy draw. Every list gets its line, zeros included: all zeros at a lit hangar is the answer that the lists do not carry its lights, and it reads differently
// from a sample with no line at all, which is a copy that failed (said by copyBuffer or the map line).
void logLightLists(uint32_t sample, const uint8_t* t0, UINT t0Bytes, const uint8_t* t2, UINT t2Bytes) {
    const uint8_t* cb = g_pending.cb1Bytes;
    if (!g_pending.cb1Kept) {
        Log::get().note("glass probe: sample %u light lists not read: the frame block was not read back whole", sample);
        return;
    }
    const uint32_t nx = u32At(cb, 228 * 16), ny = u32At(cb, 228 * 16 + 4), ns = u32At(cb, 228 * 16 + 8), tile = u32At(cb, 228 * 16 + 12);
    const float scale = f32At(cb, 227 * 16 + 8), bias = f32At(cb, 227 * 16 + 12);
    const int lo = sliceOfDepth(kGlassNearM, scale, bias, ns), hi = sliceOfDepth(kGlassFarM, scale, bias, ns);
    const uint64_t grid = static_cast<uint64_t>(nx) * ny * ns;
    Log::get().note("glass probe: sample %u light lists: grid %ux%ux%u (%llu clusters), tile %u, t0 copy %u bytes holds %u of them, t2 copy %u bytes; slices at 0.5, 1, 2, 4, 8 m: %d %d %d %d %d", sample, nx, ny, ns,
                    static_cast<unsigned long long>(grid), tile, t0Bytes, clustersHeld(nx, ny, ns, t0Bytes), t2Bytes, lo, sliceOfDepth(1.0f, scale, bias, ns), sliceOfDepth(2.0f, scale, bias, ns),
                    sliceOfDepth(4.0f, scale, bias, ns), hi);
    for (int l = 0; l < kLightListCount; ++l) {
        uint32_t ids[kLightsPrinted] = {};
        uint32_t idCount = 0;
        const ListCensus c = censusList(t0, t0Bytes, nx, ny, ns, kLightLists[l], lo, hi, ids, kLightsPrinted, &idCount);
        char slices[420];
        size_t n = 0;
        slices[0] = '\0';
        for (uint32_t s = 0; s < kMaxSlices && n < sizeof slices - 16; ++s)
            if (c.bySlice[s]) n += std::snprintf(slices + n, sizeof slices - n, " %u:%u", s, c.bySlice[s]);
        Log::get().note("glass probe: sample %u list %s: %u clusters hold any, %u ids, %u distinct lights, at most %u in a cluster, %u past capacity, %u ids past the table; clusters by slice:%s", sample, kLightLists[l].name,
                        c.clusters, c.ids, c.distinct, c.most, c.overCapacity, c.idsPast, n ? slices : " none");
        if (l != kListPoint && l != kListSpot) continue;
        // The records offsets: cb1[249].zw for a point, cb1[250].zw for a spot.
        const uint32_t reg = l == kListPoint ? 249u : 250u;
        const uint32_t a = u32At(cb, reg * 16 + 8), b = u32At(cb, reg * 16 + 12);
        for (uint32_t i = 0; i < idCount; ++i) {
            LightRecord r;
            const bool ok = l == kListPoint ? readPoint(t2, t2Bytes, a, b, ids[i], &r) : readSpot(t2, t2Bytes, a, b, ids[i], &r);
            if (!ok) {
                Log::get().note("glass probe: sample %u %s id %u: the record is past t2's %u bytes (bases %u, %u)", sample, kLightLists[l].name, ids[i], t2Bytes, a, b);
                continue;
            }
            Log::get().note("glass probe: sample %u %s id %u at the glass's depths: pos %.2f %.2f %.2f (%.1f m), radius %.2f, intensity %.5g, colour %.3f %.3f %.3f, source radius %.3f, cos outer %.3f inner %.3f, shadow %d, cookie %d",
                            sample, kLightLists[l].name, ids[i], r.x, r.y, r.z, std::sqrt(r.x * r.x + r.y * r.y + r.z * r.z), r.radius, r.intensity, r.r, r.g, r.b, r.sourceRadius, r.cosOuter, r.cosInner, r.shadow, r.cookie);
        }
    }
}

// True when both list copies are finished (read, failed or absent). They are mapped together, and only once the frame block has been read.
bool tryReadLights(ID3D11DeviceContext* ctx, uint32_t sample) {
    Readback& l = g_pending.lists;
    Readback& r = g_pending.records;
    if (l.done && r.done) return true;
    if (l.done || r.done) {   // one copy of the two was never made: copyBuffer said why, and there is nothing to read the other with
        Log::get().note("glass probe: sample %u light lists not read: %s was not copied", sample, l.done ? "t0" : "t2");
        l = Readback{};
        r = Readback{};
        return true;
    }
    D3D11_MAPPED_SUBRESOURCE lm{}, rm{};
    HRESULT hr = ctx->Map(l.staging.Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &lm);
    if (hr == DXGI_ERROR_WAS_STILL_DRAWING) return false;
    if (SUCCEEDED(hr)) {
        const HRESULT hr2 = ctx->Map(r.staging.Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &rm);
        if (hr2 == DXGI_ERROR_WAS_STILL_DRAWING) {
            ctx->Unmap(l.staging.Get(), 0);
            return false;
        }
        hr = hr2;
        if (SUCCEEDED(hr)) {
            logLightLists(sample, static_cast<const uint8_t*>(lm.pData) + l.offset, l.bytes, static_cast<const uint8_t*>(rm.pData) + r.offset, r.bytes);
            ctx->Unmap(r.staging.Get(), 0);
        }
        ctx->Unmap(l.staging.Get(), 0);
    }
    if (FAILED(hr)) Log::get().note("glass probe: sample %u light lists map failed (0x%08lX)", sample, static_cast<unsigned long>(hr));
    l = Readback{};
    r = Readback{};
    return true;
}

void finishPending(ID3D11DeviceContext* ctx) {
    if (!g_pending.on || g_frame - g_pending.frame < kReadbackDelayFrames) return;
    FlatComputeInternalScope internal;
    const uint32_t s = g_pending.sample;
    const bool a = tryRead(ctx, g_pending.cb1, "cb1", s, kCb1Spans, sizeof kCb1Spans / sizeof kCb1Spans[0], 0);
    const bool b = tryRead(ctx, g_pending.cb2, "cb2", s, nullptr, 0, kCb2Regs);
    const bool c = tryRead(ctx, g_pending.light, "t1", s, nullptr, 0, kLightBytes / 16);
    const bool d = a && tryReadLights(ctx, s);
    if (a && b && c && d) {
        g_pending = Pending{};
        Log::get().note("glass probe: sample %u read back", s);
    } else if (g_frame - g_pending.frame > kReadbackGiveUpFrames) {
        Log::get().note("glass probe: sample %u readback still not ready after %u frames; dropped", s, kReadbackGiveUpFrames);
        g_pending = Pending{};
    }
}

}  // namespace

void cockpitGlassProbeDraw(ID3D11DeviceContext* ctx, char kind, UINT count, UINT instances) {
    if (!g_armed || !ctx) return;
    const uint64_t vsH = bindingShaderHash(BindSlot::Vs);
    const uint64_t psH = bindingShaderHash(BindSlot::Ps);
    const int vi = canopyVsIndex(vsH);
    const int pi = canopyPsIndex(psH);
    if (vi < 0 && pi < 0) return;
    ++g_windowDraws;
    if (vi >= 0) ++g_vsDraws[vi];
    if (pi >= 0) {
        ++g_psDraws[pi];
    } else {
        ++g_otherPsDraws;
        for (auto& h : g_otherPs) {
            if (h == psH) break;
            if (!h) { h = psH; break; }
        }
    }
    const ULONGLONG now = GetTickCount64();
    if (sampleDue(g_pending.on, g_samples, kSampleCap, now, g_lastSample, kIntervalMs))
        takeSample(ctx, kind, count, instances, vsH, psH, vi, pi, now);
}

void cockpitGlassProbeFrameBoundary(ID3D11DeviceContext* ctx) {
    ++g_frame;
    Config& cfg = Config::get();
    const uint32_t generation = cfg.generation();   // read before the value (config.h): a swap between costs one more read, never a stale one
    if (!g_configRead || generation != g_configGeneration) {
        g_configRead = true;
        g_configGeneration = generation;
        const std::string v = cfg.getString("experimental.cockpit_glass_probe", "off");
        const bool on = probeKeyOn(v.c_str());
        if (on != g_armed) {
            const ULONGLONG now = GetTickCount64();
            if (on) {
                resetWindow(now);
                Log::get().note("glass probe: armed (experimental.cockpit_glass_probe = on): watching the owner context's draws for the canopy "
                                "(CockpitGlass_Forward: 3 VS, 9 PS). A 'sample 1 draw' line follows the first canopy draw; a window line every "
                                "30 s counts them, and draws=0 there means no canopy draw reached the hooks");
            } else {
                logWindow(now);
                Log::get().note("glass probe: off (experimental.cockpit_glass_probe = %s)", v.c_str());
            }
            g_armed = on;
            detail::g_cockpitGlassProbeLive = on;
        }
    }
    if (!g_armed) return;
    if (ctx) finishPending(ctx);
    const ULONGLONG now = GetTickCount64();
    if (now - g_windowStart >= kIntervalMs) {
        logWindow(now);
        resetWindow(now);
    }
}

}  // namespace edvr
