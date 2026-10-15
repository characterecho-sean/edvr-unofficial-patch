// The GPU pass profiler (gpu_pass_profile.h): the owner-context hooks, the frame state machine, and the CSV and summary
// output. Pure classification, grouping and CSV text live in gpu_pass_profile_core.h (tools\gpu_pass_profile_test).
#include "gpu_pass_profile.h"
#include "gpu_pass_profile_core.h"
#include "binding_shadow.h"
#include "eye_draw_snapshot.h"   // Built v13.7 (3): EyeDrawSnapshot::kHud, the HUD family (the families are the game's, not copied here)
#include "holo_material.h"       // kHoloGuiFxOffVs, the holo GUI family
#include "holo_families.h"       // kHoloTargetSphere, the holo target sphere family
#include "skin_ledger.h"         // Built v13.7 (1): skin::kChainHash and skin::kClearHash, the named compute shaders
#include "exposure_fix.h"        // lookupShaderHash: a compute shader's content hash at its dispatch (bindingShaderHash(Cs) is never set)
#include "gpu_pass_delay_shader.h"   // v5: the front-loaded delay's HLSL (compiled at the first capture that asks for a delay)
#include "gpu_timing.h"
#include "gpu_frame_timing.h"   // Built v13.6: the application-frame yield (gpuFrameYieldBegin, gpuFrameTakeYieldSkips)
#include "../common/config.h"
#include "../common/log.h"
#include <windows.h>
#include <d3d11.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace edvr {

namespace detail {
bool g_gpuPassCapturing = false;
}

namespace {
using namespace gpupass;

// One scope slot per captured frame: the timing module's slots must cover the profiler's (v2 had 4 and refused frame 5 on).
static_assert(kPassScopeSlotCount == kPassScopeSlots, "the profiler's slot count must equal gpu_timing.h's kPassScopeSlots");
static_assert(kPassScopeSlots >= kMaxCaptureFrames, "every capture frame needs its own pass scope slot");

// Boundaries a pending frame may wait for its result before it is said to be not ready. At 90 Hz, 240 is about 2.7 s.
constexpr uint32_t kMaxPolls = 240;

struct SegRec {
    PassFacts facts;
    PassHashes hashes;      // the VS:PS pairs of the segment's draws
    PassHashes csHashes;    // Built v13.7 (1): the compute shaders bound at its dispatches (the pixel half is 0)
    uint64_t tris = 0;
    bool delay = false;     // v5: this segment is the front-loaded delay at the frame's start (excluded from every class and total)
};

// Built v13.7 (3): the HUD, holo and UI families, read from the game's headers (EyeDrawSnapshot::kHud, kHoloGuiFxOffVs, kHoloTargetSphere).
gpupass::VsFamily hudFamilies() {
    gpupass::VsFamily f;
    f.vs[0] = EyeDrawSnapshot::kHud;
    f.vs[1] = kHoloGuiFxOffVs;
    f.vs[2] = kHoloTargetSphere;
    f.n = 3;
    return f;
}

// v4: the exposure reduction chain's compute shaders (reviews\vr-view-caches-2026-10-13.md, candidate 1): B8E727E94D44D891 (24x24 groups,
// the 8-byte structured UAV) and F1FB2EEFB662F3AA (the exposure shader, once per eye). Their 252x252 post passes are classed by shape.
gpupass::VsFamily exposureFamily() {
    gpupass::VsFamily f;
    f.vs[0] = 0xB8E727E94D44D891ull;
    f.vs[1] = 0xF1FB2EEFB662F3AAull;
    f.n = 2;
    return f;
}

// GPU pass profile v2: the light volume shapes of reviews\sealed-remaining-costs-2026-10-13.md (instanced into the eye-size target, the
// lighting pass). Full hashes as the flat recipes name them (flat_projection_recipes.h): 0357BBB2DEE43C1F a 14-index box, 8289669D93A18C1D
// an 84-index sphere or cone, 24DE25E496342EB8 and A1B7CFCD0BE7493E the smaller instanced volumes.
gpupass::VsFamily lightingFamilies() {
    gpupass::VsFamily f;
    f.vs[0] = 0x0357BBB2DEE43C1Full;
    f.vs[1] = 0x8289669D93A18C1Dull;
    f.vs[2] = 0x24DE25E496342EB8ull;
    f.vs[3] = 0xA1B7CFCD0BE7493Eull;
    f.n = 4;
    return f;
}

// Built v13.7 (1): the compute shaders the repo names (skin_ledger.h), for the top_cs_dispatches column. Others print as hex.
const gpupass::ComputeName* computeNames() {
    static const gpupass::ComputeName kNames[] = {
        {skin::kChainHash, "APPLY_BIND_POSE_TRANSFORMS_CS"},
        {skin::kClearHash, "CLEAR_TRANSFORM_DATA_CS"},
    };
    return kNames;
}
size_t computeNameCount() { return 2; }

struct Bound {
    uint32_t colours = 0;
    TargetShape colour;
    bool hasDepth = false;
    TargetShape depth;
};

struct ViewShape {
    void* view;
    TargetShape shape;
};

enum class FrameState : uint8_t { Idle, Open, Pending, Ready, Failed };

struct FrameRec {
    FrameState state = FrameState::Idle;
    const char* failure = "";
    StampBudget budget;          // budget.used is the stamp count; budget.overflow counts boundaries not stamped
    uint32_t polls = 0;
    uint64_t frequency = 0;
    double frameMs = 0;
    EyeSize eye;                 // the headset's eye texture (vscreen): the size passes are classified against
    EyeSize matched;             // the largest colour target with a matching depth in this frame (reported beside eye)
    bool delayed = false;        // v5: the delay was placed at this frame's start
    double gapMs = 0, delayMs = 0, busyMs = 0;   // v5: this frame's no-work gap, delay, and busy time (segmentTotals)
    std::vector<SegRec> segs;    // segs[i] is the segment that starts at stamp i and ends at stamp i + 1
    std::vector<uint64_t> ticks;
    std::vector<Row> rows;
};

ID3D11DeviceContext* g_ctx = nullptr;   // the owner context, while a frame is open
// GPU pass profile v2: the timestamp pool holds kMaxStampsPerFrame queries for each frame of the largest capture so far, slot-major.
std::vector<ID3D11Query*> g_pool;
uint32_t g_poolFrames = 0;              // frames the pool holds (0: no pool)
inline ID3D11Query*& poolAt(uint32_t slot, uint32_t i) { return g_pool[static_cast<size_t>(slot) * kMaxStampsPerFrame + i]; }
bool g_requested = false;   // hotkey pressed; the batch starts at the next boundary
uint32_t g_requestFrames = kDefaultCaptureFrames;   // the count the press asked for (the hotkey's value)
bool g_batch = false;       // a batch is starting frames
bool g_haveBatch = false;   // a batch is not yet written
uint32_t g_frameCount = kDefaultCaptureFrames;      // the frames of the batch in progress (or last written)
uint32_t g_started = 0;     // frames of the batch begun so far
uint32_t g_waits = 0;       // consecutive boundaries that found the shared clock held by another scope (startAction)
// v5 (delay): the delay the press asked for, the delay of the batch in progress, the iteration count calibrated for it (0: no delay), and
// the flag that keeps the delay dispatch out of the pass notes (its segment is the delay, not a pass).
uint32_t g_requestDelayMs = kDefaultDelayMs;
uint32_t g_batchDelayMs = 0;
uint32_t g_delayIters = 0;
bool g_delayIssuing = false;
int g_active = -1;          // the frame open now, or -1
std::vector<FrameRec> g_frames = std::vector<FrameRec>(kMaxCaptureFrames);
Bound g_bound;              // the targets bound now, as the segments see them
int g_edvrDepth = 0;        // EDVR census sections open now
std::vector<ViewShape> g_views;   // view -> shape, per capture (a view's shape is read once while capturing)

void releasePool() {
    for (ID3D11Query*& q : g_pool)
        if (q) {
            q->Release();
            q = nullptr;
        }
    g_pool.clear();
    g_pool.shrink_to_fit();
    g_poolFrames = 0;
}

// The timestamp pool for a capture of `frames`, created on the owner thread at the first capture that needs it, never on the hot path.
// A later capture asking for more frames than the pool holds replaces it (the one hitch, logged with its time). Nothing is in flight
// when this is called: the batch that held the old pool is written first.
bool ensurePool(ID3D11DeviceContext* ctx, uint32_t frames) {
    if (!poolNeedsGrowth(g_poolFrames, frames)) return true;
    LARGE_INTEGER freq{}, t0{}, t1{};
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t0);
    ID3D11Device* dev = nullptr;
    ctx->GetDevice(&dev);
    if (!dev) return false;
    releasePool();
    const uint32_t want = poolFramesFor(frames);
    const D3D11_QUERY_DESC desc{D3D11_QUERY_TIMESTAMP, 0};
    g_pool.assign(static_cast<size_t>(want) * kMaxStampsPerFrame, nullptr);
    bool ok = true;
    for (ID3D11Query*& q : g_pool) {
        if (FAILED(dev->CreateQuery(&desc, &q)) || !q) {
            q = nullptr;
            ok = false;
            break;
        }
    }
    dev->Release();
    QueryPerformanceCounter(&t1);
    if (!ok) {
        releasePool();
        return false;
    }
    g_poolFrames = want;
    const double ms = freq.QuadPart ? 1000.0 * static_cast<double>(t1.QuadPart - t0.QuadPart) / static_cast<double>(freq.QuadPart) : 0.0;
    Log::get().note("gpu pass profile: created the timestamp query pool for %u frames (%zu queries) in %.1f ms; this is a one-time "
                    "hitch at the first capture of this size",
                    want, g_pool.size(), ms);
    return true;
}

// ---- v5: the front-loaded delay (TEMPORARY). One compute dispatch, one thread, `iterations` dependent multiply-adds; it puts the GPU
// the whole frame's work behind it so the passes that follow run without waiting on the CPU's submission. Measurement only.
ID3D11Device* g_delayDevice = nullptr;             // the device the objects below belong to (one reference held)
ID3D11ComputeShader* g_delayCs = nullptr;
ID3D11Buffer* g_delayCb = nullptr;
ID3D11Buffer* g_delayUav = nullptr;
ID3D11UnorderedAccessView* g_delayUavView = nullptr;
bool g_delayCompileFailed = false;                 // compile failed once: no further attempt in this session

void releaseDelay() {
    if (g_delayUavView) g_delayUavView->Release();
    if (g_delayUav) g_delayUav->Release();
    if (g_delayCb) g_delayCb->Release();
    if (g_delayCs) g_delayCs->Release();
    if (g_delayDevice) g_delayDevice->Release();
    g_delayUavView = nullptr;
    g_delayUav = nullptr;
    g_delayCb = nullptr;
    g_delayCs = nullptr;
    g_delayDevice = nullptr;
}

// d3dcompiler_47.dll's D3DCompile, loaded once. Raw types: the macro and include arguments are null, so their pointer types are not needed.
typedef HRESULT(WINAPI* PfnD3DCompile)(LPCVOID, SIZE_T, LPCSTR, const void*, void*, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);
bool compileDelayShader(ID3DBlob** code) {
    static PfnD3DCompile compile = nullptr;
    if (!compile) {
        HMODULE mod = LoadLibraryW(L"d3dcompiler_47.dll");
        if (mod) compile = reinterpret_cast<PfnD3DCompile>(GetProcAddress(mod, "D3DCompile"));
        if (!compile) return false;
    }
    ID3DBlob* errors = nullptr;
    const HRESULT hr = compile(kGpuPassDelayHlsl, sizeof(kGpuPassDelayHlsl) - 1, "gpu_pass_delay", nullptr, nullptr, "main", "cs_5_0",
                               0x8000u /* D3DCOMPILE_OPTIMIZATION_LEVEL3 */, 0, code, &errors);
    if (errors) errors->Release();
    return SUCCEEDED(hr) && *code;
}

// The shader and its buffers, created once per device. False when any of them could not be made (noted once).
bool ensureDelayObjects(ID3D11DeviceContext* ctx) {
    if (g_delayCompileFailed) return false;
    ID3D11Device* dev = nullptr;
    ctx->GetDevice(&dev);
    if (!dev) return false;
    if (g_delayDevice && g_delayDevice != dev) releaseDelay();
    if (!g_delayDevice) g_delayDevice = dev;
    else dev->Release();
    if (g_delayCs && g_delayCb && g_delayUav && g_delayUavView) return true;
    if (!g_delayCs) {
        ID3DBlob* code = nullptr;
        if (!compileDelayShader(&code)) {
            g_delayCompileFailed = true;
            Log::get().note("gpu pass profile: the delay shader did not compile (d3dcompiler_47.dll missing or refused it); no delay this session");
            return false;
        }
        const HRESULT hr = g_delayDevice->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &g_delayCs);
        code->Release();
        if (FAILED(hr) || !g_delayCs) {
            g_delayCompileFailed = true;
            Log::get().note("gpu pass profile: the delay shader compiled but did not create (0x%08X); no delay this session", static_cast<unsigned>(hr));
            return false;
        }
    }
    D3D11_BUFFER_DESC cbd{};
    cbd.ByteWidth = 16;
    cbd.Usage = D3D11_USAGE_DEFAULT;
    cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    D3D11_BUFFER_DESC ud{};
    ud.ByteWidth = 16;
    ud.Usage = D3D11_USAGE_DEFAULT;
    ud.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    D3D11_UNORDERED_ACCESS_VIEW_DESC uvd{};
    uvd.Format = DXGI_FORMAT_R32_FLOAT;
    uvd.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    uvd.Buffer.FirstElement = 0;
    uvd.Buffer.NumElements = 4;
    if (!g_delayCb && FAILED(g_delayDevice->CreateBuffer(&cbd, nullptr, &g_delayCb))) return false;
    if (!g_delayUav && FAILED(g_delayDevice->CreateBuffer(&ud, nullptr, &g_delayUav))) return false;
    if (!g_delayUavView && FAILED(g_delayDevice->CreateUnorderedAccessView(g_delayUav, &uvd, &g_delayUavView))) return false;
    return true;
}

// One delay dispatch on ctx. The game's compute bindings (CS, CB0, UAV0) are read first and put back after, through the context's own
// methods, so the binding shadow sees the same calls it would for a game. Notes from the dispatch are suppressed (g_delayIssuing).
void runDelayDispatch(ID3D11DeviceContext* ctx, uint32_t iterations) {
    ID3D11ComputeShader* prevCs = nullptr;
    ID3D11Buffer* prevCb = nullptr;
    ID3D11UnorderedAccessView* prevUav = nullptr;
    ctx->CSGetShader(&prevCs, nullptr, nullptr);
    ctx->CSGetConstantBuffers(0, 1, &prevCb);
    ctx->CSGetUnorderedAccessViews(0, 1, &prevUav);
    const UINT data[4] = {iterations, 0, 0, 0};
    ctx->UpdateSubresource(g_delayCb, 0, nullptr, data, 0, 0);
    ctx->CSSetShader(g_delayCs, nullptr, 0);
    ctx->CSSetConstantBuffers(0, 1, &g_delayCb);
    ID3D11UnorderedAccessView* uav = g_delayUavView;
    const UINT keepCounts = 0xFFFFFFFFu;
    ctx->CSSetUnorderedAccessViews(0, 1, &uav, &keepCounts);
    g_delayIssuing = true;
    ctx->Dispatch(1, 1, 1);
    g_delayIssuing = false;
    ctx->CSSetShader(prevCs, nullptr, 0);
    ctx->CSSetConstantBuffers(0, 1, &prevCb);
    ctx->CSSetUnorderedAccessViews(0, 1, &prevUav, &keepCounts);
    if (prevCs) prevCs->Release();
    if (prevCb) prevCb->Release();
    if (prevUav) prevUav->Release();
}

// The iteration count for delayMs, from a timestamp-bracketed trial dispatch: one blocking read-back, once per capture at the batch start
// (before any frame scope opens). The trial's length grows until it is measurable (at least 0.5 ms). 0 when the calibration fails; the
// reason is noted and the capture runs with no delay.
uint32_t calibrateDelay(ID3D11DeviceContext* ctx, uint32_t delayMs) {
    if (!ensureDelayObjects(ctx)) return 0;
    D3D11_QUERY_DESC dq{D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
    D3D11_QUERY_DESC tq{D3D11_QUERY_TIMESTAMP, 0};
    ID3D11Query *disjoint = nullptr, *q0 = nullptr, *q1 = nullptr;
    if (FAILED(g_delayDevice->CreateQuery(&dq, &disjoint)) || FAILED(g_delayDevice->CreateQuery(&tq, &q0)) ||
        FAILED(g_delayDevice->CreateQuery(&tq, &q1))) {
        if (disjoint) disjoint->Release();
        if (q0) q0->Release();
        if (q1) q1->Release();
        Log::get().note("gpu pass profile: the delay calibration could not create its queries; no delay this capture");
        return 0;
    }
    auto readBlocking = [&](ID3D11Query* q, void* out, UINT size) {
        const uint64_t deadline = GetTickCount64() + 2000;
        for (;;) {
            const HRESULT hr = ctx->GetData(q, out, size, 0);
            if (hr == S_OK) return true;
            if (FAILED(hr) || GetTickCount64() > deadline) return false;
        }
    };
    uint64_t trial = 1000000;
    double msPerIteration = 0.0;
    for (int attempt = 0; attempt < 3 && msPerIteration <= 0.0; ++attempt) {
        ctx->Begin(disjoint);
        ctx->End(q0);
        runDelayDispatch(ctx, static_cast<uint32_t>(trial));
        ctx->End(q1);
        ctx->End(disjoint);
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dd{};
        UINT64 t0 = 0, t1 = 0;
        if (!readBlocking(disjoint, &dd, sizeof dd) || !readBlocking(q0, &t0, sizeof t0) || !readBlocking(q1, &t1, sizeof t1)) break;
        if (dd.Disjoint || !dd.Frequency) continue;
        const double ms = static_cast<double>(t1 - t0) * 1000.0 / static_cast<double>(dd.Frequency);
        if (ms < 0.5 && attempt < 2) {
            trial *= 4;
            continue;
        }
        msPerIteration = ms / static_cast<double>(trial);
        Log::get().note("gpu pass profile: delay calibration: a trial of %llu iterations took %.3f ms (%.2f ns an iteration)",
                        static_cast<unsigned long long>(trial), ms, msPerIteration * 1.0e6);
    }
    disjoint->Release();
    q0->Release();
    q1->Release();
    const uint32_t iterations = delayIterations(msPerIteration, delayMs);
    if (!iterations) Log::get().note("gpu pass profile: the delay calibration did not measure a trial; no delay this capture");
    return iterations;
}

// The size and format of the view's own mip, and whether its resource is a cube map. A view is read once per capture.
TargetShape shapeOf(void* view, bool depth) {
    for (const ViewShape& v : g_views)
        if (v.view == view) return v.shape;
    TargetShape t;
    uint32_t mip = 0;
    ID3D11Resource* res = nullptr;
    if (depth) {
        auto* dsv = static_cast<ID3D11DepthStencilView*>(view);
        D3D11_DEPTH_STENCIL_VIEW_DESC d{};
        dsv->GetDesc(&d);
        t.format = d.Format;
        if (d.ViewDimension == D3D11_DSV_DIMENSION_TEXTURE2DARRAY) {
            t.slices = d.Texture2DArray.ArraySize;
            mip = d.Texture2DArray.MipSlice;
        } else if (d.ViewDimension == D3D11_DSV_DIMENSION_TEXTURE2D) {
            mip = d.Texture2D.MipSlice;
        }
        dsv->GetResource(&res);
    } else {
        auto* rtv = static_cast<ID3D11RenderTargetView*>(view);
        D3D11_RENDER_TARGET_VIEW_DESC d{};
        rtv->GetDesc(&d);
        t.format = d.Format;
        if (d.ViewDimension == D3D11_RTV_DIMENSION_TEXTURE2DARRAY) {
            t.slices = d.Texture2DArray.ArraySize;
            mip = d.Texture2DArray.MipSlice;
        } else if (d.ViewDimension == D3D11_RTV_DIMENSION_TEXTURE2D) {
            mip = d.Texture2D.MipSlice;
        }
        rtv->GetResource(&res);
    }
    if (res) {
        ID3D11Texture2D* tex = nullptr;
        if (SUCCEEDED(res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&tex))) && tex) {
            D3D11_TEXTURE2D_DESC td{};
            tex->GetDesc(&td);
            t.w = (td.Width >> mip) ? (td.Width >> mip) : 1u;
            t.h = (td.Height >> mip) ? (td.Height >> mip) : 1u;
            t.cube = (td.MiscFlags & D3D11_RESOURCE_MISC_TEXTURECUBE) != 0;
            tex->Release();
        }
        res->Release();
    }
    g_views.push_back(ViewShape{view, t});
    return t;
}

SegRec newSeg() {
    SegRec s;
    s.facts.edvr = g_edvrDepth > 0;
    s.facts.colours = g_bound.colours;
    s.facts.colour = g_bound.colour;
    s.facts.hasDepth = g_bound.hasDepth;
    s.facts.depth = g_bound.depth;
    return s;
}

// One timestamp on the owner context (g_ctx). reserve keeps room for the frame's end stamp. False when refused (counted).
bool stamp(FrameRec& fr, uint32_t slot, uint32_t reserve) {
    if (!fr.budget.take(reserve)) return false;
    g_ctx->End(poolAt(slot, fr.budget.used - 1));
    return true;
}

// A pass boundary: a stamp, and a new segment that starts here with the targets bound now. A refused stamp merges what
// follows into the current segment (the overflow is counted and said).
void boundary(uint32_t slot) {
    FrameRec& fr = g_frames[slot];
    if (!stamp(fr, slot, 1)) return;
    fr.segs.push_back(newSeg());
}

SegRec* curSeg() {
    if (g_active < 0) return nullptr;
    FrameRec& fr = g_frames[g_active];
    return fr.segs.empty() ? nullptr : &fr.segs.back();
}

bool live(ID3D11DeviceContext* ctx) { return g_active >= 0 && ctx == g_ctx; }

bool unresolved() {
    for (uint32_t i = 0; i < g_started; ++i)
        if (g_frames[i].state == FrameState::Open || g_frames[i].state == FrameState::Pending) return true;
    return false;
}

enum class BeginResult : uint8_t { Begun, Held, Failed };

// Held: another scope holds the shared clock right now. The frame is not begun and the boundary retries it (not a failure).
BeginResult beginFrame(ID3D11DeviceContext* ctx, uint32_t slot, EyeSize eye, uint64_t now) {
    FrameRec& fr = g_frames[slot];
    fr.state = FrameState::Idle;
    fr.failure = "";
    fr.budget = StampBudget{};
    fr.polls = 0;
    fr.frequency = 0;
    fr.frameMs = 0;
    fr.eye = eye;
    fr.segs.clear();
    fr.ticks.clear();
    fr.rows.clear();
    fr.delayed = g_delayIters > 0 && slot > 0;   // v5: frame 0 of a batch is the reference frame (no delay); the rest carry the delay
    fr.gapMs = fr.delayMs = fr.busyMs = 0;
    if (!ensurePool(ctx, g_frameCount)) {
        fr.state = FrameState::Failed;
        fr.failure = "the timestamp query pool could not be created";
        return BeginResult::Failed;
    }
    if (!gpuTimingPassScopeOpen(ctx, slot, now)) {
        if (gpuTimingPassScopeHeld()) return BeginResult::Held;
        fr.state = FrameState::Failed;
        fr.failure = "the frame's GPU timing scope could not open (its slot still holds a lease, or the clock is unavailable)";
        return BeginResult::Failed;
    }
    g_ctx = ctx;
    g_active = static_cast<int>(slot);
    g_views.clear();
    g_edvrDepth = 0;
    g_bound = Bound{};
    if (void* rtv = bindingGet(BindSlot::Rtv0)) {
        g_bound.colours = 1;
        g_bound.colour = shapeOf(rtv, false);
    }
    if (void* dsv = bindingGet(BindSlot::Dsv0)) {
        g_bound.hasDepth = true;
        g_bound.depth = shapeOf(dsv, true);
    }
    detail::g_gpuPassCapturing = true;
    fr.state = FrameState::Open;
    stamp(fr, slot, 1);   // the frame's start, the first stamp of a fresh budget
    fr.segs.push_back(newSeg());
    if (fr.delayed) {
        // v5: the delay is the frame's first segment: one dispatch, then a boundary, so the delay has its own stamps and the passes start
        // after it. Its notes are suppressed (g_delayIssuing), and the segment is excluded from every class and total.
        fr.segs.back().delay = true;
        runDelayDispatch(ctx, g_delayIters);
        boundary(slot);
    }
    return BeginResult::Begun;
}

void failFrame(ID3D11DeviceContext* ctx, uint32_t slot, uint64_t now, const char* why) {
    FrameRec& fr = g_frames[slot];
    fr.state = FrameState::Failed;
    fr.failure = why;
    gpuTimingPassScopeRelease(ctx, slot, now);
}

// Ends the open frame: its end stamp, then its scope. The scope stays held (Pending) until its result is read.
void closeFrame(ID3D11DeviceContext* ctx, uint32_t slot, uint64_t now) {
    FrameRec& fr = g_frames[slot];
    const bool stamped = stamp(fr, slot, 0);   // g_ctx is still set: stamp uses it
    g_active = -1;
    g_ctx = nullptr;
    detail::g_gpuPassCapturing = false;
    const bool closed = gpuTimingPassScopeClose(ctx, slot, now);
    if (!stamped) return failFrame(ctx, slot, now, "the end stamp was refused (the frame was full)");
    if (!closed) return failFrame(ctx, slot, now, "closing the GPU timing scope was refused");
    fr.state = FrameState::Pending;
}

// Turns a frame's stamps into segment rows. A segment with no draw, dispatch or clear carries its time into the next one
// (a boundary with nothing between it and the next). Classification sees the whole frame, so cube runs can be found.
void buildRows(FrameRec& fr, uint32_t slot) {
    const double toMs = fr.frequency ? 1000.0 / static_cast<double>(fr.frequency) : 0.0;
    const uint32_t stamps = fr.budget.used;
    fr.frameMs = stamps >= 2 ? static_cast<double>(fr.ticks[stamps - 1] - fr.ticks[0]) * toMs : 0.0;
    const size_t segCount = stamps >= 1 ? (fr.segs.size() < stamps - 1 ? fr.segs.size() : stamps - 1) : 0;
    std::vector<PassFacts> facts;
    std::vector<double> ms;
    std::vector<const SegRec*> kept;
    std::vector<double> gapAfter;             // v5: parallel to facts: the no-work time directly after each pass
    std::vector<double> segMs;                // v5: every segment's time, with its kind, for segmentTotals
    std::vector<SegKind> segKind;
    double carry = 0;
    for (size_t i = 0; i < segCount; ++i) {
        const double d = static_cast<double>(fr.ticks[i + 1] - fr.ticks[i]) * toMs;
        const SegRec& s = fr.segs[i];
        if (s.delay) {   // v5: the delay is not a pass: no class, no row; its time is reported on its own
            segMs.push_back(d);
            segKind.push_back(SegKind::Delay);
            continue;
        }
        if (!s.facts.draws && !s.facts.dispatches && !s.facts.clears) {
            carry += d;
            segMs.push_back(d);
            segKind.push_back(SegKind::Empty);
            if (!gapAfter.empty()) gapAfter.back() += d;
            continue;
        }
        segMs.push_back(d);
        segKind.push_back(SegKind::Work);
        PassFacts pf = s.facts;
        const std::vector<HashTally> topPair = s.hashes.top();   // Built v13.7 (3): the most-drawn VS, for the HUD family rule
        pf.topVs = topPair.empty() ? 0 : topPair[0].vs;
        const std::vector<HashTally> topCsPair = s.csHashes.top();   // v4: the most-dispatched compute shader, for the exposure rule
        pf.topCs = topCsPair.empty() ? 0 : topCsPair[0].vs;
        facts.push_back(pf);
        ms.push_back(d + carry);
        gapAfter.push_back(0.0);
        carry = 0;
        kept.push_back(&s);
    }
    if (carry > 0 && !ms.empty()) ms.back() += carry;
    const SegTotals totals = segmentTotals(segMs, segKind);
    fr.delayMs = totals.delay;
    fr.gapMs = totals.gap;
    fr.busyMs = totals.busy;
    std::vector<PassClass> cls(facts.size());
    const VsFamily hud = hudFamilies();
    const VsFamily lighting = lightingFamilies();
    const VsFamily exposure = exposureFamily();
    for (size_t k = 0; k < facts.size(); ++k) cls[k] = classifyPass(facts[k], fr.eye, hud, lighting, exposure);
    markCubeRuns(facts, cls, fr.eye);
    fr.matched = matchedEye(facts);
    uint32_t index = 0;
    for (size_t k = 0; k < facts.size(); ++k) {
        const PassFacts& f = facts[k];
        const SegRec& s = *kept[k];
        Row r;
        r.frame = slot;
        r.index = index++;
        r.cls = cls[k];
        r.edvr = f.edvr;
        r.ms = ms[k];
        r.draws = f.draws;
        r.dispatches = f.dispatches;
        r.clears = f.clears;
        r.tris = s.tris;
        r.gapAfterMs = gapAfter[k];
        r.colours = f.colours;
        r.rt = f.colours ? targetText(f.colour) : "-";
        r.rtFormat = f.colours ? f.colour.format : 0;
        r.slices = f.colours ? f.colour.slices : 1;
        r.cube = f.colours && f.colour.cube;
        r.dsv = f.hasDepth ? targetText(f.depth) : "-";
        r.dsvFormat = f.hasDepth ? f.depth.format : 0;
        r.top = topText(s.hashes.top());
        r.topCs = csText(s.csHashes.top(), computeNames(), computeNameCount());   // Built v13.7 (1): the compute shaders this pass dispatched with
        r.ident = identText(s.hashes.top(), s.csHashes.top(), computeNames(), computeNameCount());
        r.cs = s.csHashes.all();
        fr.rows.push_back(r);
    }
}

// Reads a pending frame: its scope's result, then its stamps without a flush. Ready frames get their rows.
void pollFrame(ID3D11DeviceContext* ctx, uint32_t slot, uint64_t now) {
    FrameRec& fr = g_frames[slot];
    if (fr.state != FrameState::Pending) return;
    const GpuPassScopeResult r = gpuTimingPassScopePoll(ctx, slot, now);
    if (r.status == GpuPassScopeStatus::Pending) {
        if (++fr.polls > kMaxPolls) failFrame(ctx, slot, now, "the GPU had not reported its scope after 240 boundaries");
        return;
    }
    if (r.status == GpuPassScopeStatus::Failed) return failFrame(ctx, slot, now, r.reason);
    const uint32_t stamps = fr.budget.used;
    fr.ticks.assign(stamps, 0);
    for (uint32_t i = 0; i < stamps; ++i) {
        UINT64 v = 0;
        const HRESULT hr = ctx->GetData(poolAt(slot, i), &v, sizeof(v), D3D11_ASYNC_GETDATA_DONOTFLUSH);
        if (hr == S_FALSE) {
            if (++fr.polls > kMaxPolls) failFrame(ctx, slot, now, "a timestamp was still not ready after 240 boundaries");
            return;
        }
        if (FAILED(hr)) return failFrame(ctx, slot, now, "GetData failed on a timestamp");
        fr.ticks[i] = v;
    }
    fr.frequency = r.frequency;
    buildRows(fr, slot);
    // The segments are read into rows; they are freed now, so a long capture holds the rows only (120 frames of segments would not fit).
    std::vector<SegRec>().swap(fr.segs);
    std::vector<uint64_t>().swap(fr.ticks);
    fr.state = FrameState::Ready;
    gpuTimingPassScopeRelease(ctx, slot, now);
}

// The CSV, in the log directory, named by the local time. False when it could not be written.
bool writeCsv(const std::vector<Row>& rows, std::string& nameOut) {
    SYSTEMTIME st{};
    GetLocalTime(&st);
    char name[80];
    std::snprintf(name, sizeof name, "gpu_pass_profile_%04u%02u%02u-%02u%02u%02u.csv", st.wYear, st.wMonth, st.wDay,
                  st.wHour, st.wMinute, st.wSecond);
    nameOut = name;
    const std::wstring dir = Config::get().logDir();
    ensureDirectory(dir);
    std::wstring path = dir + L"\\";
    for (const char* c = name; *c; ++c) path += static_cast<wchar_t>(*c);
    FILE* f = _wfopen(path.c_str(), L"wb");
    if (!f) return false;
    std::string text = csvHeader();
    text += "\n";
    for (const Row& r : rows) {
        text += csvRow(r);
        text += "\n";
    }
    const size_t wrote = std::fwrite(text.data(), 1, text.size(), f);
    std::fclose(f);
    return wrote == text.size();
}

// The batch's output: the CSV, one line per frame that was not read, the means, the class lines and the top passes.
void emitBatch() {
    std::vector<Row> rows;
    std::vector<double> frameMs;
    std::vector<bool> ok;
    uint32_t overflow = 0;
    bool eyeNoted = false;
    // v5 (delay): per-frame gap, busy and delay, for the delayed frames and the reference frame(s) (not delayed, in a batch with a delay).
    std::vector<double> dGap, dBusy, dDelay, rGap, rBusy;
    for (uint32_t i = 0; i < g_started; ++i) {
        const FrameRec& fr = g_frames[i];
        overflow += fr.budget.overflow;
        if (fr.state == FrameState::Ready) {
            if (!eyeNoted) {
                eyeNoted = true;
                const bool agree = fr.matched.w == fr.eye.w && fr.matched.h == fr.eye.h;
                Log::get().note("gpu pass profile: eye %ux%u (the headset's eye texture); the largest colour target with a matching depth "
                                "%ux%u%s", fr.eye.w, fr.eye.h, fr.matched.w, fr.matched.h, agree ? "" : " -- DIFFERENT, check the eye size");
            }
            frameMs.push_back(fr.frameMs);
            ok.push_back(true);
            rows.insert(rows.end(), fr.rows.begin(), fr.rows.end());
            if (fr.delayed) {
                dGap.push_back(fr.gapMs);
                dBusy.push_back(fr.busyMs);
                dDelay.push_back(fr.delayMs);
            } else if (g_batchDelayMs > 0) {
                rGap.push_back(fr.gapMs);
                rBusy.push_back(fr.busyMs);
            }
        } else {
            Log::get().note("gpu pass profile: frame %u not captured: %s", i + 1, fr.failure);
            frameMs.push_back(0);
            ok.push_back(false);
        }
    }
    const size_t suspects = markSuspects(rows);   // v5: flags each row (the CSV's suspect column)
    const Summary s = summarise(rows, frameMs, ok);
    std::string csvName;
    if (writeCsv(rows, csvName))
        Log::get().note("gpu pass profile: %zu pass rows written to the log directory as %s", rows.size(), csvName.c_str());
    else
        Log::get().note("gpu pass profile: the CSV could not be written to the log directory (%zu rows)", rows.size());
    if (!s.frames) {
        Log::get().note("gpu pass profile: no frame was read, so there are no means (the frame lines above say why)");
        return;
    }
    Log::get().note("gpu pass profile: means over %u frames: frame %.3f ms = elite %.3f ms + edvr %.3f ms "
                    "(unattributed %.3f ms)",
                    s.frames, s.frameMs, s.eliteMs, s.edvrMs, s.frameMs - s.eliteMs - s.edvrMs);
    // v5 (delay): the summary the flight reads. Medians over the delayed frames; the reference frame's own line below.
    if (!dDelay.empty()) {
        Log::get().note("gpu pass profile: delay %u ms (measured %.3f ms) placed at frame start; inter-pass gaps median %.3f ms per frame; "
                        "frame busy %.3f ms",
                        g_batchDelayMs, medianOf(dDelay), medianOf(dGap), medianOf(dBusy));
    } else if (g_batchDelayMs > 0) {
        Log::get().note("gpu pass profile: delay %u ms requested but not placed (%s)", g_batchDelayMs,
                        g_delayIters == 0 ? "the calibration failed; the note above says why" : "no delayed frame was read");
    }
    if (!rGap.empty())
        Log::get().note("gpu pass profile: reference frame (no delay): inter-pass gaps %.3f ms per frame; frame busy %.3f ms",
                        medianOf(rGap), medianOf(rBusy));
    if (g_batchDelayMs > 0)
        Log::get().note("gpu pass profile: %zu suspect passes (over %.0f times their median and followed by a gap over %.2f ms); "
                        "marked suspect in the CSV", suspects, kSuspectFactor, kLargeGapMs);
    for (size_t c = 0; c < static_cast<size_t>(PassClass::Count); ++c) {
        const ClassMean& m = s.cls[c];
        if (m.passes > 0)
            Log::get().note("gpu pass profile: class %-18s %8.3f ms  %6.1f passes  median pass %.4f ms", passClassName(static_cast<PassClass>(c)),
                            m.ms, m.passes, m.median);   // Built v13.7 (4): the median beside the mean (a single spike moves the mean, not this)
    }
    for (size_t k = 0; k < s.top.size(); ++k) {
        const Summary::Group& g = s.top[k];
        Log::get().note("gpu pass profile: top %zu by median: %.4f ms median  %.3f ms mean  class=%s  draws=%.1f  passes=%.1f  %s",
                        k + 1, g.median, g.ms, passClassName(g.cls), g.draws, g.passes, g.sig.c_str());
    }
    // The compute shaders, by dispatches per frame (all dispatches, not the per-pass top three).
    for (const CsTotal& t : s.cs) {
        const char* name = "";
        for (size_t k = 0; k < computeNameCount(); ++k)
            if (computeNames()[k].hash == t.hash) {
                name = computeNames()[k].name;
                break;
            }
        Log::get().note("gpu pass profile: top_cs_dispatches: %016llx%s%s%s  %.1f dispatches a frame",
                        static_cast<unsigned long long>(t.hash), *name ? "(" : "", name, *name ? ")" : "", t.perFrame);
    }
    if (overflow)
        Log::get().note("gpu pass profile: %u boundaries were not stamped (a frame holds %u stamps); the passes after the cap "
                        "merge into the last one stamped",
                        overflow, kMaxStampsPerFrame);
}

}  // namespace

namespace {
// Built v13.6: the capture is over (its last frame's scope is closed): the shared clock is released to the application-frame timing and the
// census timers, and the counts of what yielded are logged once for the capture.
void endYield() {
    gpuTimingSetYield(false);
    const uint64_t frames = gpuFrameTakeYieldSkips();
    const uint64_t refused = gpuTimingTakeYieldRefusals();
    Log::get().note("app gpu timing: paused for the GPU pass profile, %llu frames; census timer leases refused: %llu.",
                    static_cast<unsigned long long>(frames), static_cast<unsigned long long>(refused));
}
}  // namespace

// ---- hooks (owner context only) ----

void gpuPassProfileRequest() {
    if (g_requested || g_batch || g_haveBatch) {
        Log::get().note("gpu pass profile: a capture is already armed or running; this press is refused");
        return;
    }
    // GPU pass profile v2: the count comes from the hotkey's value ("KEY COUNT"), read at the press.
    const CaptureSpec spec = parseCaptureSpec(Config::get().getString("hotkey.gpu_pass_profile", ""));
    g_requestFrames = spec.frames;
    g_requestDelayMs = spec.delayMs;
    g_requested = true;
    // Built v13.6: from the press until the last profiled frame's scope is closed, the application-frame timing and the census timers yield the
    // shared clock (gpu_timing.h); the profiler's frame scopes then open. Cleared by endYield at the batch's end.
    gpuTimingSetYield(true);
    Log::get().note("gpu pass profile: armed. The next %u whole frames are measured from the next frame boundary, with a %u ms "
                    "delay placed at the start of each frame after the first (the first is the reference frame); the lines follow "
                    "once the GPU has reported them. The application GPU timing and the census timers pause until the capture's "
                    "last frame is closed.",
                    g_requestFrames, g_requestDelayMs);
}

std::string gpuPassProfileKeyPart(const std::string& value) { return parseCaptureSpec(value).key; }

void gpuPassProfileNoteOm(ID3D11DeviceContext* ctx, uint32_t n, ID3D11RenderTargetView* const* rtvs,
                          ID3D11DepthStencilView* dsv) {
    if (!live(ctx)) return;
    Bound b;
    for (uint32_t i = 0; rtvs && i < n && i < 8; ++i) {
        if (!rtvs[i]) continue;
        if (!b.colours) b.colour = shapeOf(rtvs[i], false);
        ++b.colours;
    }
    if (dsv) {
        b.hasDepth = true;
        b.depth = shapeOf(dsv, true);
    }
    g_bound = b;
    boundary(static_cast<uint32_t>(g_active));
}

void gpuPassProfileNoteClear(ID3D11DeviceContext* ctx, void*, void*) {
    if (!live(ctx)) return;
    boundary(static_cast<uint32_t>(g_active));
    if (SegRec* s = curSeg()) ++s->facts.clears;
}

void gpuPassProfileNoteDraw(ID3D11DeviceContext* ctx, char kind, uint32_t count, uint32_t instances) {
    if (!live(ctx)) return;
    SegRec* s = curSeg();
    if (!s) return;
    ++s->facts.draws;
    const bool triangles = kind == 'D' || kind == 'I' || kind == 'N' || kind == 'X';
    if (triangles) s->tris += static_cast<uint64_t>(count / 3) * (instances ? instances : 1u);
    s->hashes.note(bindingShaderHash(BindSlot::Vs), bindingShaderHash(BindSlot::Ps));
}

void gpuPassProfileNoteDispatch(ID3D11DeviceContext* ctx, uint32_t, uint32_t, uint32_t) {
    if (!live(ctx) || g_delayIssuing) return;   // v5: the delay's own dispatch is not a pass
    boundary(static_cast<uint32_t>(g_active));
    if (SegRec* s = curSeg()) {
        ++s->facts.dispatches;
        // GPU pass profile v2: the compute shader's content hash is looked up here, at the dispatch, during a capture only. The binding
        // shadow records the Cs pointer but never its hash (CSSetShader does not record it, so bindingShaderHash(Cs) was always 0), and a
        // hash taken in the CSSetShader hook would cost on every compute bind in the game. Zero means the registry never met the shader.
        s->csHashes.note(lookupShaderHash(bindingGet(BindSlot::Cs)), 0);
    }
}

void gpuPassProfileNoteEdvr(ID3D11DeviceContext* ctx, bool enter) {
    if (!live(ctx)) return;
    g_edvrDepth = enter ? g_edvrDepth + 1 : (g_edvrDepth > 0 ? g_edvrDepth - 1 : 0);
    boundary(static_cast<uint32_t>(g_active));
}

// ---- frame edge ----

void gpuPassProfileFrameBoundary(ID3D11DeviceContext* ctx, uint32_t frameNo, uint32_t eyeW, uint32_t eyeH) {
    if (!ctx || (!g_requested && !g_batch && !g_haveBatch)) return;
    if (!gpuTimingOwns(ctx)) {
        if (g_requested) {
            g_requested = false;
            endYield();
            Log::get().note("gpu pass profile: this context does not own the GPU timing domain, so nothing was captured");
        }
        return;
    }
    const uint64_t now = GetTickCount64();
    if (g_active >= 0) closeFrame(ctx, static_cast<uint32_t>(g_active), now);
    // Built v13.6: an application frame open across this boundary (its lease holds the clock) is ended here, so this capture's frame can open.
    if (g_requested || g_batch) gpuFrameYieldBegin(ctx);
    for (uint32_t i = 0; i < g_started; ++i) pollFrame(ctx, i, now);
    if (g_requested && !g_batch && !g_haveBatch) {
        g_requested = false;
        g_frameCount = g_requestFrames;
        if (!ensurePool(ctx, g_frameCount)) {
            endYield();
            Log::get().note("gpu pass profile: the timestamp query pool could not be created; nothing was captured");
            return;
        }
        // v5 (delay): the iteration count for this capture, calibrated once here (the batch starts with no frame scope open).
        g_batchDelayMs = g_requestDelayMs;
        g_delayIters = g_batchDelayMs > 0 ? calibrateDelay(ctx, g_batchDelayMs) : 0;
        g_batch = true;
        g_haveBatch = true;
        g_started = 0;
        g_waits = 0;
        Log::get().note("gpu pass profile: capturing %u frames from frame %u", g_frameCount, frameNo);
    }
    if (g_batch) {
        // v3: a held clock is waited for at the boundary (the holder's scope ends and the frame starts on a later boundary); only a frame
        // that genuinely faults is skipped, and the batch goes on past it.
        const StartAction a = startAction(g_started, g_frameCount, kPassScopeSlotCount, gpuTimingPassScopeHeld(), g_waits);
        if (a == StartAction::Begin) {
            const uint32_t slot = g_started;
            const BeginResult r = beginFrame(ctx, slot, EyeSize{eyeW, eyeH}, now);
            if (r == BeginResult::Held) {
                ++g_waits;   // taken between the check and the open: wait like any other holder
            } else {
                g_waits = 0;
                ++g_started;
                if (r == BeginResult::Failed)
                    Log::get().note("gpu pass profile: frame %u not captured: %s", slot + 1, g_frames[slot].failure);
            }
        } else if (a == StartAction::Wait) {
            ++g_waits;
        } else if (a == StartAction::Abort) {
            if (g_started < g_frameCount && gpuTimingPassScopeHeld())
                Log::get().note("gpu pass profile: frame %u could not start: another GPU timing scope held the shared clock for %u "
                                "boundaries; the batch ends with %u of %u frames",
                                g_started + 1, g_waits, g_started, g_frameCount);
            else
                Log::get().note("gpu pass profile: frame %u could not start: no scope slot; the batch ends with %u of %u frames",
                                g_started + 1, g_started, g_frameCount);
            g_batch = false;
            endYield();
        } else {
            // Done: the last frame's scope was closed at the top of this boundary: the capture holds the clock no longer (Built v13.6).
            g_batch = false;
            endYield();
        }
    }
    if (g_haveBatch && !g_batch && !unresolved()) {
        emitBatch();
        g_haveBatch = false;
    }
}

void gpuPassProfileShutdown() {
    if (g_active >= 0 || g_batch || g_haveBatch)
        Log::get().note("gpu pass profile: shut down during a capture; the batch was dropped unwritten");
    g_requested = false;
    g_batch = false;
    g_haveBatch = false;
    g_active = -1;
    g_ctx = nullptr;
    detail::g_gpuPassCapturing = false;
    gpuTimingSetYield(false);   // Built v13.6: no yield outlives the profiler
    releasePool();
    releaseDelay();   // v5: the delay's shader and buffers
}

}  // namespace edvr
