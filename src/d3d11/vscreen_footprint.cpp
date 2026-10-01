// The footprint instrument (vscreen_footprint.h says what it measures and why this way).
//
// THREADS. The draw hook and the frame boundary both run on the game's render thread (the owner context's thread, which is
// also the thread that calls Present), so the state below has no lock. Nothing here allocates on the per-draw path past
// the first sample's staging buffer, and the per-draw cost when no sample is due is one load, one tick count and a compare.
//
// WHAT THE LOG WOULD SHOW IF THIS NEVER RAN: no `vscreen footprint` line at all -- not even the one-time arming line. A run
// that is armed but sees no composite (the screen is not on show, the shader pair changed) prints the 30 s line with
// draws=0; one that sees it and cannot read a source prints skipped= with the reasons in why=. tools\edvr_log.py
// --vscreen-fit says which of those it is.
#include "vscreen_footprint.h"

#include <windows.h>
#include <d3d11_1.h>
#include <wrl/client.h>

#include <cmath>
#include <cstring>
#include <string>

#include "../common/config.h"
#include "../common/guard.h"
#include "../common/log.h"
#include "../common/native_render_settings.h"
#include "../common/runtime_profile.h"
#include "../common/vscreen_auto_state.h"
#include "../common/vscreen_fit.h"
#include "flat_compute_readback.h"   // FlatComputeInternalScope: the hooks step past the instrument's own copies and Map

namespace edvr {

namespace detail {
bool g_footprintWanted = false;
uint64_t g_footprintWindowMs = 30000;   // the line's cadence
uint64_t g_footprintSampleMs = 500;     // about two samples a second: a median of ~60 a window, and nothing a frame
uint64_t g_footprintConfigMs = 1000;    // the arming condition is re-read this often
}

namespace {

using Microsoft::WRL::ComPtr;

constexpr uint32_t kReadLagFrames = 3;       // a copy has run by then; mapping it sooner is what stalls
constexpr uint32_t kGiveUpFrames = 90;       // a copy still not ready after about a second is abandoned and counted late
constexpr uint32_t kMinPersistSamples = 12;  // an on-foot median needs at least this many samples (six seconds of them)
constexpr double kPersistDelta = 0.002;      // a relative change in the median that rewrites the file (7 px of 3504)

// The staging buffer's layout: the four sources, packed.
constexpr uint32_t kOffModel = 0, kBytesModel = 48;     // cb0 rows 9..11: three float4
constexpr uint32_t kOffClip = 48, kBytesClip = 64;      // cb1 rows 270..273: four float4 (the clip matrix's columns)
constexpr uint32_t kOffVerts = 112, kBytesVerts = 80;   // the quad's four vertices, 20 bytes each: float3 position, float2 UV
constexpr uint32_t kOffSize = 192, kBytesSize = 8;      // the per-instance SIZE float2
constexpr uint32_t kStagingBytes = 256;
constexpr uint32_t kVertexStride = 20, kSizeStride = 8;
constexpr uint32_t kModelRowFirst = 9, kClipRowFirst = 270;   // composite-vs.asm: dp4 cb0[9..11], mul cb1[270..273]

enum SkipWhy : uint32_t {
    kSkipCb0, kSkipCb1, kSkipCbSmall, kSkipVb0, kSkipVbSmall, kSkipSize, kSkipDevice, kSkipMap, kSkipCorners, kSkipImplausible,
    kSkipCount
};
const char* const kSkipNames[kSkipCount] = {"no-cb0", "no-cb1", "cb-small", "vb0-stride", "vb-small", "no-size",
                                            "device", "map", "corners", "implausible"};

struct Window {
    uint32_t draws = 0, samples = 0, onFoot = 0, other = 0, skipped = 0, late = 0;
    uint32_t skipBy[kSkipCount] = {};
    vscreenfit::FractionStore footFrac, footHeight, footApplied, otherFrac;
    void clear() {
        draws = samples = onFoot = other = skipped = late = 0;
        for (uint32_t& n : skipBy) n = 0;
        footFrac.clear(); footHeight.clear(); footApplied.clear(); otherFrac.clear();
    }
};

struct Pending {
    bool valid = false;
    uint64_t frame = 0;        // the frame that issued the copies
    bool onFoot = false;       // the gate when it was issued
    float applied = 1.0f;      // the panel distance the draw's constants carried
    uint32_t waited = 0;       // frames the Map said "still drawing"
};

struct State {
    // A raw pointer, released by hand (vscreenFootprintShutdown, or a new device): this state is a static, and a COM smart pointer
    // in one would Release at DLL detach, after the real d3d11.dll may already be unloaded.
    ID3D11Buffer* staging = nullptr;
    ID3D11Device* stagingDevice = nullptr;   // identity only
    Pending pending;
    uint64_t frame = 0;
    bool onFoot = false;
    uint64_t lastSampleMs = 0;
    uint64_t configMs = 0;
    int armedState = -1;                     // -1 undecided, 0 not armed, 1 armed: the once-per-change line
    bool stoodDownNoted = false;
    uint64_t windowStartMs = 0;
    uint32_t windowNo = 0;
    Window win;
    vscreenfit::FractionStore session;       // the session's on-foot fractions, each normalised to panel distance 1.0
    bool wroteOnce = false;
    double lastWrittenFrac1 = 0.0;
};
State g;
FaultBudget g_budget("vscreenFootprint", 5);

void skip(SkipWhy why) {
    ++g.win.skipped;
    ++g.win.skipBy[why];
}

void releaseStaging() {
    if (g.staging) g.staging->Release();
    g.staging = nullptr;
    g.stagingDevice = nullptr;
}

// One composite draw's four sources into the staging buffer. Returns false (counted) when a source is not what the
// measurement assumes; the caller still marks the time so a source that is never right is retried twice a second, not
// at every draw.
bool issueCopies(ID3D11DeviceContext* ctx, float applied, int baseVertex, unsigned startInstance) {
    State& s = g;
    ComPtr<ID3D11DeviceContext1> ctx1;
    ctx->QueryInterface(IID_PPV_ARGS(&ctx1));

    ID3D11Buffer* cbRaw[2] = {nullptr, nullptr};
    UINT first[2] = {0, 0}, num[2] = {0, 0};
    if (ctx1) ctx1->VSGetConstantBuffers1(0, 2, cbRaw, first, num);
    else ctx->VSGetConstantBuffers(0, 2, cbRaw);
    ComPtr<ID3D11Buffer> cb0, cb1;
    cb0.Attach(cbRaw[0]);
    cb1.Attach(cbRaw[1]);

    ID3D11Buffer* vbRaw[4] = {nullptr, nullptr, nullptr, nullptr};
    UINT strides[4] = {0, 0, 0, 0}, offsets[4] = {0, 0, 0, 0};
    ctx->IAGetVertexBuffers(0, 4, vbRaw, strides, offsets);
    ComPtr<ID3D11Buffer> vb[4];
    for (int i = 0; i < 4; ++i) vb[i].Attach(vbRaw[i]);

    if (!cb0) { skip(kSkipCb0); return false; }
    if (!cb1) { skip(kSkipCb1); return false; }
    D3D11_BUFFER_DESC d0{}, d1{};
    cb0->GetDesc(&d0);
    cb1->GetDesc(&d1);
    const uint32_t off0 = (first[0] + kModelRowFirst) * 16u, off1 = (first[1] + kClipRowFirst) * 16u;
    if (d0.ByteWidth < off0 + kBytesModel || d1.ByteWidth < off1 + kBytesClip) { skip(kSkipCbSmall); return false; }

    if (!vb[0] || strides[0] != kVertexStride || baseVertex < 0) { skip(kSkipVb0); return false; }
    D3D11_BUFFER_DESC dv{};
    vb[0]->GetDesc(&dv);
    const uint32_t offV = offsets[0] + static_cast<uint32_t>(baseVertex) * kVertexStride;
    if (dv.ByteWidth < offV + kBytesVerts) { skip(kSkipVbSmall); return false; }

    int pick = -1;   // SIZE: the first bound slot 1..3 with a float2 stride (panel_curve.cpp's own rule)
    for (int i = 1; i < 4; ++i)
        if (vb[i] && strides[i] == kSizeStride) { pick = i; break; }
    if (pick < 0) { skip(kSkipSize); return false; }
    D3D11_BUFFER_DESC ds{};
    vb[pick]->GetDesc(&ds);
    const uint32_t offS = offsets[pick] + startInstance * kSizeStride;
    if (ds.ByteWidth < offS + kBytesSize) { skip(kSkipVbSmall); return false; }

    ComPtr<ID3D11Device> dev;
    cb0->GetDevice(&dev);
    if (!dev) { skip(kSkipDevice); return false; }
    if (!s.staging || s.stagingDevice != dev.Get()) {
        releaseStaging();
        D3D11_BUFFER_DESC sd{};
        sd.ByteWidth = kStagingBytes;
        sd.Usage = D3D11_USAGE_STAGING;
        sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(dev->CreateBuffer(&sd, nullptr, &s.staging)) || !s.staging) { s.staging = nullptr; skip(kSkipDevice); return false; }
        s.stagingDevice = dev.Get();
    }
    auto copy = [&](uint32_t dst, ID3D11Buffer* src, uint32_t off, uint32_t bytes) {
        const D3D11_BOX box = {off, 0, 0, off + bytes, 1, 1};
        ctx->CopySubresourceRegion(s.staging, 0, dst, 0, 0, src, 0, &box);
    };
    copy(kOffModel, cb0.Get(), off0, kBytesModel);
    copy(kOffClip, cb1.Get(), off1, kBytesClip);
    copy(kOffVerts, vb[0].Get(), offV, kBytesVerts);
    copy(kOffSize, vb[pick].Get(), offS, kBytesSize);

    s.pending = Pending{};
    s.pending.valid = true;
    s.pending.frame = s.frame;
    s.pending.onFoot = s.onFoot;
    s.pending.applied = applied;
    return true;
}

// A sample whose copy has had time to run: map it without waiting, push the corners through the vertex shader's arithmetic,
// keep the width.
void collect(ID3D11DeviceContext* ctx) {
    State& s = g;
    if (!s.pending.valid || !s.staging) return;
    if (s.frame - s.pending.frame < kReadLagFrames) return;
    // (The caller holds the FlatComputeInternalScope, OUTSIDE the fault guard: a fault caught by the guard does not unwind
    // this frame, so a scope created here would leave the hooks' bypass flag set for the rest of the session.)
    D3D11_MAPPED_SUBRESOURCE m{};
    const HRESULT hr = ctx->Map(s.staging, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m);
    if (hr == DXGI_ERROR_WAS_STILL_DRAWING) {
        if (++s.pending.waited >= kGiveUpFrames) { ++s.win.late; s.pending.valid = false; }
        return;
    }
    const Pending p = s.pending;
    s.pending.valid = false;
    if (FAILED(hr) || !m.pData) { skip(kSkipMap); return; }
    uint8_t bytes[kStagingBytes];
    std::memcpy(bytes, m.pData, sizeof(bytes));
    ctx->Unmap(s.staging, 0);

    float model[12], clip[16], positions[4][3], size[2];
    std::memcpy(model, bytes + kOffModel, kBytesModel);
    std::memcpy(clip, bytes + kOffClip, kBytesClip);
    for (int i = 0; i < 4; ++i) std::memcpy(positions[i], bytes + kOffVerts + i * kVertexStride, 12);
    std::memcpy(size, bytes + kOffSize, kBytesSize);
    const vscreenfit::Footprint fp = vscreenfit::footprintOfQuad(model, clip, positions, size);
    if (!fp.valid) { skip(kSkipCorners); return; }
    const double frac = fp.widthFraction();
    if (!vscreenfit::plausibleFraction(frac)) { skip(kSkipImplausible); return; }
    const double applied = p.applied > 0.0f ? static_cast<double>(p.applied) : 1.0;
    ++s.win.samples;
    if (p.onFoot) {
        ++s.win.onFoot;
        s.win.footFrac.add(frac);
        s.win.footHeight.add(fp.heightFraction());
        s.win.footApplied.add(applied);
        s.session.add(frac * applied);   // at distance 1.0: what the next launch rescales
    } else {
        ++s.win.other;
        s.win.otherFrac.add(frac);
    }
}

void refreshWanted() {
    State& s = g;
    const bool vr = runtimeVrProfile();
    const std::string width = Config::get().getString("fix.vscreen_res_width", "auto");
    const bool autoKey = vr && vscreenfit::keyTextIsAuto(width.c_str());
    const bool want = autoKey && g_budget.shouldRun();
    detail::g_footprintWanted = want;
    const int state = want ? 1 : 0;
    if (state == s.armedState) return;
    s.armedState = state;
    char line[700];
    if (want) {
        vscreenfit::formatArmedLine(line, sizeof(line));
        Log::get().note("%s", line);
    } else if (autoKey) {
        if (!s.stoodDownNoted) {
            s.stoodDownNoted = true;
            vscreenfit::formatStoodDownLine(line, sizeof(line));
            Log::get().note("%s", line);
        }
    } else if (vr) {
        vscreenfit::formatNotArmedLine(line, sizeof(line), width.c_str());
        Log::get().note("%s", line);
    }
}

// The window's line, and the on-foot median stored for the next launch.
void closeWindow(uint64_t now) {
    State& s = g;
    Config& cfg = Config::get();
    vscreenfit::WindowLine w;
    w.window = ++s.windowNo;
    EdvrNativeRenderSizing sz{};
    if (edvrQueryNativeRenderSizing(EDVR_NATIVE_RENDER_SIZING_VERSION_1, sizeof(sz), &sz) && sz.valid == 1 && sz.activeWidth[0] &&
        sz.activeHeight[0]) {
        w.eyeKnown = true;
        w.eyeW = sz.activeWidth[0];
        w.eyeH = sz.activeHeight[0];
    }
    w.samples = s.win.samples;
    w.onFoot = s.win.onFoot;
    w.other = s.win.other;
    w.skipped = s.win.skipped;
    w.late = s.win.late;
    w.draws = s.win.draws;
    w.distance = cfg.getFloat("fix.panel_distance", 1.0f);
    {
        size_t used = 0;
        for (uint32_t i = 0; i < kSkipCount; ++i) {
            if (!s.win.skipBy[i]) continue;
            const int n = std::snprintf(w.skipWhy + used, sizeof(w.skipWhy) - used, "%s%s:%u", used ? "," : "", kSkipNames[i], s.win.skipBy[i]);
            if (n < 0 || used + static_cast<size_t>(n) >= sizeof(w.skipWhy)) break;
            used += static_cast<size_t>(n);
        }
    }
    double v = 0.0, lo = 0.0, hi = 0.0;
    if (s.win.footFrac.median(&v)) {
        w.haveFoot = true;
        w.footFrac = v;
        s.win.footFrac.range(&lo, &hi);
        w.footLo = lo;
        w.footHi = hi;
        if (s.win.footHeight.median(&v)) w.footH = v;
        if (s.win.footApplied.median(&v)) w.applied = v;
    }
    if (s.win.otherFrac.median(&v)) {
        w.haveOther = true;
        w.otherFrac = v;
    }
    double frac1 = 0.0;
    if (s.session.median(&frac1)) {
        w.haveSession = true;
        w.sessionFrac1 = frac1;
        w.sessionN = static_cast<uint32_t>(s.session.total());
        if (s.session.total() >= kMinPersistSamples) {
            const bool changed =
                !s.wroteOnce || std::fabs(frac1 - s.lastWrittenFrac1) > kPersistDelta * s.lastWrittenFrac1;
            if (changed) {
                vscreenfit::Record r;
                r.fractionAtUnit = frac1;
                r.eyeWidth = w.eyeKnown ? w.eyeW : 0u;
                r.distance = w.applied > 0.0 ? w.applied : 1.0;
                r.samples = static_cast<uint32_t>(s.session.total());
                noteMeasuredPanelFootprint(cfg.logDir(), r);
                s.wroteOnce = true;
                s.lastWrittenFrac1 = frac1;
            }
            w.persisted = true;
            w.persistedFrac1 = s.lastWrittenFrac1;
        }
    }
    {
        // What the next launch would fit if the world route runs then: the same arithmetic the resolver uses
        // (vscreen_fit.h), from this session's median when there is one, else the calibration seed.
        uint32_t eyeForFit = w.eyeKnown ? w.eyeW : 0u;
        if (!eyeForFit) lastKnownEyeWidth(cfg.logDir(), &eyeForFit);
        if (eyeForFit) {
            vscreenfit::Inputs in;
            in.eyeWidth = eyeForFit;
            in.distance = w.distance;
            in.haveFootprint = w.haveSession;
            in.fractionAtUnit = w.sessionFrac1;
            in.route.keyAuto = true;
            in.route.runtime = vscreenfit::RuntimeKind::EdvrOpenXr;
            const vscreenfit::Decision d = vscreenfit::decide(in);
            w.fitWidth = d.width;
            w.legacyWidth = d.legacyWidth;
        }
    }
    char line[900];
    vscreenfit::formatWindowLine(line, sizeof(line), w);
    Log::get().note("%s", line);
    s.win.clear();
    s.windowStartMs = now;
}

}  // namespace

void vscreenFootprintCompositeDraw(ID3D11DeviceContext* ctx, float appliedDistance, int baseVertex, unsigned startInstance) {
    if (!detail::g_footprintWanted || !ctx) return;
    State& s = g;
    ++s.win.draws;
    if (s.pending.valid) return;
    const uint64_t now = GetTickCount64();
    if (now - s.lastSampleMs < detail::g_footprintSampleMs) return;
    s.lastSampleMs = now;   // marked first: a source that is never right is retried twice a second, not at every draw
    FlatComputeInternalScope internal;
    guardedBudget(g_budget, [&] { issueCopies(ctx, appliedDistance, baseVertex, startInstance); });
}

void vscreenFootprintFrameBoundary(ID3D11DeviceContext* ownerCtx, bool onFoot) {
    State& s = g;
    // The flat profile has no composite to measure and no world route: nothing is read, nothing is written, nothing is said.
    if (!runtimeVrProfile()) {
        detail::g_footprintWanted = false;
        return;
    }
    ++s.frame;
    s.onFoot = onFoot;
    const uint64_t now = GetTickCount64();
    if (s.configMs == 0 || now - s.configMs >= detail::g_footprintConfigMs) {
        s.configMs = now;
        refreshWanted();
    }
    if (!detail::g_footprintWanted) {
        s.pending.valid = false;
        return;
    }
    if (!ownerCtx) return;
    {
        FlatComputeInternalScope internal;   // outside the guard, for the reason collect() says
        guardedBudget(g_budget, [&] { collect(ownerCtx); });
    }
    if (s.windowStartMs == 0) s.windowStartMs = now;
    if (now - s.windowStartMs >= detail::g_footprintWindowMs) closeWindow(now);
}

bool vscreenFootprintSamplePending() { return g.pending.valid; }

void vscreenFootprintShutdown() {
    g.pending.valid = false;
    releaseStaging();
    detail::g_footprintWanted = false;
}

}  // namespace edvr
