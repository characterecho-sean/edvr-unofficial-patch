#include "temporal_shader_bytecode.h"   // kMapIconFacingVsBytecode and the rest: the build's compiles of galaxy_map_vs.h
#include "galaxy_map_ui.h"

#include <windows.h>

#include <d3d11.h>
#include <wrl/client.h>

#include <cstdio>
#include <cstring>
#include <new>

#include "../common/guard.h"
#include "../common/log.h"
#include "flat_compute_readback.h"   // FlatComputeInternalScope: EDVR's own calls do not reach the game's tees
#include "shader_swap.h"
#include "vscreen.h"                 // the raw VS/PS/SRV setters: the binding shadow keeps naming the game's

namespace edvr {
namespace mapui {

Counters g_counters;

namespace {

using Microsoft::WRL::ComPtr;

constexpr uint32_t kRing = 4;       // eye-frames in flight per eye
constexpr uint32_t kPerFrame = 4;   // probes per eye-frame (the arrow's shader draws other sprites some frames; each is a probe and only the arrow's can count)
constexpr unsigned kMaxFlipNotes = 16;

struct Group {
    uint64_t seq = 0;
    uint32_t n = 0;
    ComPtr<ID3D11Query> q[kPerFrame];
};
struct EyeRing {
    Group g[kRing];
    uint32_t head = 0, count = 0;
    bool over = false;
    uint64_t probedSeq = 0;   // the last eye-frame a probe was issued for
};

struct State {
    ID3D11Device* device = nullptr;   // identity only
    ComPtr<ID3D11VertexShader> swapVs[kSwapCount];
    bool swapTried[kSwapCount] = {};
    ComPtr<ID3D11VertexShader> probeVs;
    ComPtr<ID3D11PixelShader> probePs;
    bool probeTried = false;
    ComPtr<ID3D11VertexShader> savedVs;
    bool bound = false;
    EyeRing eye[2];
    bool failed = false;
    const char* said[4] = {};
    unsigned flips = 0;
    std::atomic<bool> resetPending{false};
};

State* g_st = nullptr;

State& st() {
    if (!g_st) g_st = new State;   // never freed: D3D objects released at process exit would run in a dead device's shadow
    return *g_st;
}

const unsigned char* const kBytecode[kSwapCount] = {kMapIconFacingVsBytecode, kMapIconUvFacingVsBytecode, kMapArrowDepthVsBytecode};
const size_t kBytecodeLen[kSwapCount] = {sizeof(kMapIconFacingVsBytecode), sizeof(kMapIconUvFacingVsBytecode), sizeof(kMapArrowDepthVsBytecode)};
const char* const kSwapName[kSwapCount] = {"map_icon_facing_vs", "map_icon_uv_facing_vs", "map_arrow_depth_vs"};
const char* const kSwapLabel[kSwapCount] = {"C31238331D8AC3D4 (icons)", "12382D2EA45E9632 (labels)", "4EF6DDB075A927FA (arrow)"};

// A new device drops everything made for the old one.
void bindDevice(State& s, ID3D11DeviceContext* ctx) {
    ID3D11Device* dev = nullptr;
    ctx->GetDevice(&dev);
    if (dev == s.device) {
        if (dev) dev->Release();
        return;
    }
    for (auto& v : s.swapVs) v.Reset();
    for (bool& t : s.swapTried) t = false;
    s.probeVs.Reset();
    s.probePs.Reset();
    s.probeTried = false;
    s.savedVs.Reset();
    s.bound = false;
    for (EyeRing& r : s.eye) r = EyeRing{};
    s.device = dev;
    if (dev) dev->Release();   // identity only
}

void refreshOver(State& s) {
    const bool now = s.failed || s.eye[0].over || s.eye[1].over;
    if (now == g_over.load(std::memory_order_relaxed)) return;
    g_over.store(now, std::memory_order_relaxed);
    if (s.flips < kMaxFlipNotes) {
        ++s.flips;
        Log::get().note("galaxy map: the arrow's tip is %s a panel%s (flip %u of the first %u said; later ones are in the 30 s line)", now ? "on" : "off",
                        s.failed ? " (no mask or no query: the game's arrow)" : "", s.flips, kMaxFlipNotes);
    }
}

void resetRings(State& s) {
    for (EyeRing& r : s.eye) r = EyeRing{};
    s.failed = false;
    g_over.store(false, std::memory_order_relaxed);
}

// The oldest eye-frames whose queries have all come back, never waited for: the frame in progress (seq) is left alone.
void drain(State& s, ID3D11DeviceContext* ctx, int eye, uint64_t seq) {
    EyeRing& r = s.eye[eye];
    while (r.count) {
        Group& g = r.g[r.head];
        if (g.seq == seq) return;
        uint64_t sum = 0;
        bool broken = false;
        for (uint32_t k = 0; k < g.n; ++k) {
            UINT64 samples = 0;
            const HRESULT h = ctx->GetData(g.q[k].Get(), &samples, sizeof(samples), D3D11_ASYNC_GETDATA_DONOTFLUSH);
            if (h == S_FALSE) return;   // not yet: the next probe asks again
            if (h != S_OK) { broken = true; break; }
            sum += samples;
        }
        if (broken) {
            for (uint32_t k = 0; k < g.n; ++k) g.q[k].Reset();
            g.n = 0;
            r.head = (r.head + 1) % kRing;
            --r.count;
            g_counters.probeFaults.fetch_add(1, std::memory_order_relaxed);
            noteUnavailable("occlusion query readback");
            continue;
        }
        g_counters.answered.fetch_add(1, std::memory_order_relaxed);
        if (sum > 0) g_counters.framesOver.fetch_add(1, std::memory_order_relaxed);
        else g_counters.framesClear.fetch_add(1, std::memory_order_relaxed);
        r.over = sum > 0;
        g.n = 0;
        r.head = (r.head + 1) % kRing;
        --r.count;
        refreshOver(s);
    }
}

} // namespace

void setGates(bool vr, bool pointer) {
    const bool wasPointer = g_pointerOpen.exchange(pointer, std::memory_order_relaxed);
    g_vrOpen.store(vr, std::memory_order_relaxed);
    if (wasPointer && !pointer) {
        g_over.store(false, std::memory_order_relaxed);
        if (g_st) g_st->resetPending.store(true, std::memory_order_release);   // the render thread lets the queries go at its next probe
    }
}

void noteUnavailable(const char* what) {
    State& s = st();
    g_counters.maskUnavailable.fetch_add(1, std::memory_order_relaxed);
    s.failed = true;
    bool said = false;
    for (const char*& slot : s.said) {
        if (slot && std::strcmp(slot, what) == 0) { said = true; break; }
        if (!slot) { slot = what; break; }
    }
    if (!said) Log::get().note("galaxy map: %s unavailable: the arrow stays the game's, at the panels' depth", what);
    refreshOver(s);
}

bool swapBegin(ID3D11DeviceContext* ctx, uint64_t vs) {
    uint32_t which;
    if (vs == kVsIconA) which = kSwapIconA;
    else if (vs == kVsIconB) which = kSwapIconB;
    else if (vs == kVsArrow) which = kSwapArrow;
    else return false;
    if (!ctx) return false;
    State& s = st();
    if (s.bound) return false;
    if (which == kSwapArrow) {
        if (!pointerOpen()) return false;
        if (over()) {
            g_counters.arrowKept.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
    } else if (!vrOpen()) {
        return false;
    }
    FlatComputeInternalScope internal;
    bindDevice(s, ctx);
    if (!s.swapVs[which] && !s.swapTried[which]) {
        s.swapTried[which] = true;
        s.swapVs[which].Attach(shaderSwapCreateVs(ctx, kBytecode[which], kBytecodeLen[which], kSwapName[which], "galaxy map"));
        if (!s.swapVs[which])
            Log::get().note("galaxy map: the replacement for vertex shader %s could not be made: it is drawn as the game made it", kSwapLabel[which]);
        else
            Log::get().note("galaxy map: the replacement for vertex shader %s is made (%u bytes of bytecode)", kSwapLabel[which], static_cast<unsigned>(kBytecodeLen[which]));
    }
    ID3D11VertexShader* ours = s.swapVs[which].Get();
    if (!ours) return false;
    ctx->VSGetShader(&s.savedVs, nullptr, nullptr);
    vScreenVSSetShaderRaw(ctx, ours, nullptr, 0);
    s.bound = true;
    g_counters.swapped[which].fetch_add(1, std::memory_order_relaxed);
    return true;
}

void swapEnd(ID3D11DeviceContext* ctx) {
    State& s = st();
    if (!s.bound) return;
    s.bound = false;
    if (ctx) {
        FlatComputeInternalScope internal;
        vScreenVSSetShaderRaw(ctx, s.savedVs.Get(), nullptr, 0);
    }
    s.savedVs.Reset();
}

void noteMaskIssue(uint64_t vs, int eye, uint64_t seq) {
    const int i = panelIndex(vs);
    if (i >= 0) g_counters.maskIssued[i].fetch_add(1, std::memory_order_relaxed);
    if (eye >= 0 && eye < 2 && g_st && g_st->eye[eye].probedSeq == seq) g_counters.maskLate.fetch_add(1, std::memory_order_relaxed);
}

void probeIssue(ID3D11DeviceContext* ctx, int eye, uint64_t seq, ID3D11ShaderResourceView* mask, const ProbeDraw& pd) {
    if (!ctx || eye < 0 || eye > 1 || !mask || !pd.draw) return;
    State& s = st();
    FlatComputeInternalScope internal;
    bindDevice(s, ctx);
    if (s.resetPending.exchange(false, std::memory_order_acq_rel)) resetRings(s);
    if (!s.probeVs || !s.probePs) {
        if (!s.probeTried) {
            s.probeTried = true;
            s.probeVs.Attach(shaderSwapCreateVs(ctx, kMapProbeVsBytecode, sizeof(kMapProbeVsBytecode), "map_probe_vs", "galaxy map"));
            s.probePs.Attach(shaderSwapCreatePs(ctx, kMapProbePsBytecode, sizeof(kMapProbePsBytecode), "map_probe_ps", "galaxy map"));
        }
        if (!s.probeVs || !s.probePs) {
            noteUnavailable("the arrow probe's shaders");
            return;
        }
    }
    drain(s, ctx, eye, seq);
    EyeRing& r = s.eye[eye];
    Group* grp = nullptr;
    bool pushed = false;
    if (r.count && r.g[(r.head + r.count - 1) % kRing].seq == seq) {
        grp = &r.g[(r.head + r.count - 1) % kRing];
    } else {
        if (r.count == kRing) {
            g_counters.ringFull.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        grp = &r.g[(r.head + r.count) % kRing];
        grp->seq = seq;
        grp->n = 0;
        ++r.count;
        pushed = true;
    }
    auto undo = [&] { if (pushed) { --r.count; } };
    if (grp->n >= kPerFrame) {
        g_counters.perFrameFull.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    ComPtr<ID3D11Query>& q = grp->q[grp->n];
    if (!q) {
        ComPtr<ID3D11Device> dev;
        ctx->GetDevice(&dev);
        D3D11_QUERY_DESC qd{D3D11_QUERY_OCCLUSION, 0};
        if (!dev || FAILED(dev->CreateQuery(&qd, &q))) {
            undo();
            noteUnavailable("the occlusion query");
            return;
        }
    }
    r.probedSeq = seq;
    D3D11_PRIMITIVE_TOPOLOGY topology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    ID3D11VertexShader* vs0 = nullptr;
    ID3D11PixelShader* ps0 = nullptr;
    ID3D11ShaderResourceView* srv0 = nullptr;
    ctx->IAGetPrimitiveTopology(&topology);
    ctx->VSGetShader(&vs0, nullptr, nullptr);
    ctx->PSGetShader(&ps0, nullptr, nullptr);
    ctx->PSGetShaderResources(0, 1, &srv0);
    bool began = false, ended = false;
    const bool ran = guarded("galaxy map probe", [&] {
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_POINTLIST);
        vScreenVSSetShaderRaw(ctx, s.probeVs.Get(), nullptr, 0);
        vScreenPSSetShaderRaw(ctx, s.probePs.Get(), nullptr, 0);
        ID3D11ShaderResourceView* m = mask;
        vScreenPSSetShaderResourcesRaw(ctx, 0, 1, &m);
        ctx->Begin(q.Get());
        began = true;
        pd.draw(pd.user);
        ctx->End(q.Get());
        ended = true;
    });
    // Whatever happened, the game's state goes back: topology, shaders, and its t0.
    guarded("galaxy map probe restore", [&] {
        if (began && !ended) ctx->End(q.Get());
        ctx->IASetPrimitiveTopology(topology);
        vScreenVSSetShaderRaw(ctx, vs0, nullptr, 0);
        vScreenPSSetShaderRaw(ctx, ps0, nullptr, 0);
        vScreenPSSetShaderResourcesRaw(ctx, 0, 1, &srv0);
    });
    if (vs0) vs0->Release();
    if (ps0) ps0->Release();
    if (srv0) srv0->Release();
    if (!ran || !ended) {
        q.Reset();
        undo();
        g_counters.probeFaults.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    ++grp->n;
    g_counters.probes.fetch_add(1, std::memory_order_relaxed);
}

} // namespace mapui
} // namespace edvr
