#include "flat_temporal.h"
#include "flat_temporal_model.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "../common/config.h"
#include "../common/log.h"
#include "../common/runtime_profile.h"
#include "binding_shadow.h"

namespace edvr {
namespace detail {
std::atomic<bool> g_flatTemporalCapturing{false};
std::atomic<DWORD> g_flatTemporalOwnerThread{0};
std::atomic<uint64_t> g_flatTemporalForeignCalls{0};
}
namespace {

// Discovery has fixed storage and a fixed deadline. It makes no GPU copy,
// allocates no texture, and never changes a game command. Tokens are valid
// only within one Present interval; COM addresses can be recycled afterward.
constexpr uint32_t kViewSlots = 1024;
constexpr uint32_t kTargets = 128;
constexpr uint32_t kEdges = 256;
constexpr uint32_t kCbs = 32;
constexpr uint32_t kCbBytes = 4096;
constexpr uint64_t kCaptureMs = 120000;
constexpr uint32_t kMaxPresents = 12000;
constexpr uint64_t kReportMs = 5000;

struct View {
    void* view = nullptr;
    void* resource = nullptr;
    uint32_t w = 0, h = 0, fmt = 0;
    bool resolved = false;
};
struct Target {
    void* rtv = nullptr;
    void* dsv = nullptr;
    void* color = nullptr;
    void* depth = nullptr;
    uint32_t w = 0, h = 0, fmt = 0;
    uint32_t draws = 0, depthDraws = 0, first = 0, last = 0;
    uint32_t clearColor = 0, clearDepth = 0;
    uint32_t vpW = 0, vpH = 0;
    void* vsCb = nullptr;
    uint64_t vsHash = 0;
};
struct Edge {
    void* src = nullptr;
    void* dst = nullptr;
    char kind = 0;  // S: bound shader input, R/C/V: game copy/resolve
    uint32_t count = 0, first = 0, last = 0;
};
struct Cb {
    void* resource = nullptr;
    const void* mapped = nullptr;
    uint32_t width = 0, copied = 0, writes = 0, draws = 0;
    uint32_t writeSeq = 0, drawSeq = 0;
    uint64_t writeEpoch = 0, drawEpoch = 0;
    uint64_t vsHash = 0;
    unsigned char bytes[kCbBytes] = {};
};
struct DepthClear {
    void* dsv = nullptr;
    uint32_t count = 0, flags = 0, seq = 0;
    float value = 0.0f;
};
struct State {
    ID3D11Device* device = nullptr;  // identity only
    uint64_t startedMs = 0, nextReportMs = 0;
    uint32_t presents = 0, rejectedPresents = 0, testPresents = 0;
    uint32_t serial = 0, framesWithDepth = 0, framesToOutput = 0;
    uint64_t epoch = 1;
    uint32_t totalDraws = 0, totalDepthDraws = 0, totalCopies = 0;
    uint32_t totalDispatches = 0, unknownLists = 0;
    uint32_t forwardedDraws = 0, forwardedCopies = 0;
    uint32_t viewOverflow = 0, targetOverflow = 0, edgeOverflow = 0, cbOverflow = 0;
    bool inheritedVrWork = false;
    FlatTemporalProof proof;
    bool forwardingPresent = false;
    uint32_t viewportW = 0, viewportH = 0;
    void* backbuffer = nullptr;
    uint32_t backW = 0, backH = 0, backFmt = 0;
    View views[kViewSlots] = {};
    Target targets[kTargets] = {};
    uint32_t targetCount = 0;
    Edge edges[kEdges] = {};
    uint32_t edgeCount = 0;
    Cb cbs[kCbs] = {};
    uint32_t cbCount = 0;
    DepthClear depthClears[kTargets] = {};
    uint32_t depthClearCount = 0, depthClearOverflow = 0;
    Target* current = nullptr;
    void* boundRtv = nullptr;
    void* boundDsv = nullptr;
    void* sampledTarget = nullptr;
    void* sampledSrv[4] = {};
};
State g;
std::atomic<bool> g_waitingForPresent{false};

uint64_t nowMs() { return GetTickCount64(); }

View* viewOf(void* p) {
    if (!p) return nullptr;
    const uintptr_t key = reinterpret_cast<uintptr_t>(p) >> 4;
    uint32_t index = static_cast<uint32_t>((key ^ (key >> 11)) & (kViewSlots - 1));
    for (uint32_t probe = 0; probe < kViewSlots; ++probe) {
        View& v = g.views[index];
        if (v.view == p) return &v;
        if (!v.view) {
            v.view = p;
            ResourceInfo info{};
            if (bindingResolve(p, &info) && info.isTexture2D) {
                v.resource = info.resource;
                v.w = info.a; v.h = info.b; v.fmt = info.fmt;
                v.resolved = true;
            }
            return &v;
        }
        index = (index + 1) & (kViewSlots - 1);
    }
    ++g.viewOverflow;
    return nullptr;
}

Target* targetOf(void* rtv, void* dsv) {
    Target* slot = flatFindOrAdd(g.targets, g.targetCount, g.targetOverflow,
        [=](const Target& t) { return t.rtv == rtv && t.dsv == dsv; });
    if (!slot) return nullptr;
    Target& t = *slot;
    if (t.rtv == rtv && t.dsv == dsv) return slot;
    t.rtv = rtv; t.dsv = dsv;
    if (View* v = viewOf(rtv)) {
        t.color = v->resource; t.w = v->w; t.h = v->h; t.fmt = v->fmt;
    }
    if (View* v = viewOf(dsv)) t.depth = v->resource;
    return slot;
}

void edge(void* src, void* dst, char kind) {
    if (!src || !dst) return;
    Edge* slot = flatFindOrAdd(g.edges, g.edgeCount, g.edgeOverflow,
        [=](const Edge& e) { return e.src == src && e.dst == dst && e.kind == kind; });
    if (!slot) return;
    Edge& e = *slot;
    if (e.count) { ++e.count; e.last = g.serial; return; }
    e.src = src; e.dst = dst; e.kind = kind;
    e.count = 1; e.first = e.last = g.serial;
}

Cb* cbOf(void* res, uint32_t width) {
    Cb* slot = flatFindOrAdd(g.cbs, g.cbCount, g.cbOverflow,
        [=](const Cb& cb) { return cb.resource == res; });
    if (!slot) return nullptr;
    Cb& cb = *slot;
    cb.resource = res; cb.width = width;
    return slot;
}

// GetType before GetDesc: a stale identity may now be a texture, and calling
// ID3D11Buffer::GetDesc on it would overwrite the stack with a larger desc.
uint32_t bufferWidth(ID3D11Resource* res) {
    if (!res) return 0;
    D3D11_RESOURCE_DIMENSION kind = D3D11_RESOURCE_DIMENSION_UNKNOWN;
    res->GetType(&kind);
    if (kind != D3D11_RESOURCE_DIMENSION_BUFFER) return 0;
    D3D11_BUFFER_DESC desc{};
    static_cast<ID3D11Buffer*>(res)->GetDesc(&desc);
    return (desc.BindFlags & D3D11_BIND_CONSTANT_BUFFER) ? desc.ByteWidth : 0;
}

uint32_t hashBytes(const unsigned char* data, uint32_t n) {
    uint32_t h = 2166136261u;
    for (uint32_t i = 0; i < n; ++i) h = (h ^ data[i]) * 16777619u;
    return h;
}

void printCb(const Target& t) {
    Cb* found = nullptr;
    for (uint32_t i = 0; i < g.cbCount; ++i) {
        if (g.cbs[i].resource == t.vsCb) { found = &g.cbs[i]; break; }
    }
    if (!found) {
        Log::get().note("flat discover CB: target=%p VS b0=%p no captured CPU write; projection owner unknown",
                        t.color, t.vsCb);
        return;
    }
    const Cb& cb = *found;
    if (cb.writeEpoch != g.epoch || cb.drawEpoch != g.epoch ||
        cb.writeSeq > cb.drawSeq) {
        Log::get().note("flat discover CB: target=%p VS b0=%p width=%u current-frame-write=%u current-frame-draw=%u write-before-draw=%u; retained address/bytes may be recycled or newer than draw, projection owner unknown",
                        t.color, cb.resource, cb.width,
                        cb.writeEpoch == g.epoch ? 1 : 0,
                        cb.drawEpoch == g.epoch ? 1 : 0,
                        cb.writeEpoch == g.epoch && cb.drawEpoch == g.epoch &&
                            cb.writeSeq <= cb.drawSeq ? 1 : 0);
        return;
    }
    Log::get().note("flat discover CB: target=%p VS b0=%p width=%u copied=%u hash=%08X writes=%u depth-draws=%u write-q=%u draw-q=%u vs=%016llX (same-frame candidate bytes only)",
                    t.color, cb.resource, cb.width, cb.copied,
                    hashBytes(cb.bytes, cb.copied), cb.writes, cb.draws,
                    cb.writeSeq, cb.drawSeq,
                    static_cast<unsigned long long>(cb.vsHash));
    // The known VR scene block has view rows at float 932. Printing them is
    // evidence to compare, not permission to assume flat uses the same block.
    if (cb.copied >= (932 + 12) * sizeof(float)) {
        float row[12];
        std::memcpy(row, cb.bytes + 932 * sizeof(float), sizeof(row));
        Log::get().note("flat discover CB candidate f932 rows: %.5g %.5g %.5g %.5g | %.5g %.5g %.5g %.5g | %.5g %.5g %.5g %.5g",
                        row[0], row[1], row[2], row[3], row[4], row[5], row[6], row[7],
                        row[8], row[9], row[10], row[11]);
    }
    uint32_t plausible = 0;
    for (uint32_t offset = 0; offset + 16 * sizeof(float) <= cb.copied;
         offset += 4 * sizeof(float)) {
        float m[16];
        std::memcpy(m, cb.bytes + offset, sizeof(m));
        const bool axis = std::isfinite(m[0]) && std::isfinite(m[5]) &&
            std::fabs(m[0]) > 0.1f && std::fabs(m[0]) < 10.0f &&
            std::fabs(m[5]) > 0.1f && std::fabs(m[5]) < 10.0f;
        const bool corners = std::isfinite(m[15]) && std::fabs(m[15]) < 0.01f &&
            std::fabs(m[1]) < 0.01f && std::fabs(m[4]) < 0.01f;
        const bool perspective = (std::isfinite(m[11]) && std::fabs(std::fabs(m[11]) - 1.0f) < 0.01f) ||
            (std::isfinite(m[14]) && std::fabs(std::fabs(m[14]) - 1.0f) < 0.01f);
        if (!axis || !corners || !perspective) continue;
        ++plausible;
        if (plausible <= 4) {
            Log::get().note("flat discover projection-like bytes: VSb0=%p byte-offset=%u diag=%.6g,%.6g,%.6g tail=%.6g,%.6g,%.6g,%.6g; shape only, draw consumption/inverse unproved",
                            cb.resource, offset, m[0], m[5], m[10],
                            m[11], m[12], m[14], m[15]);
        }
    }
    if (!plausible) Log::get().note("flat discover projection-like bytes: none in captured VS b0; projection owner unknown");
    else if (plausible > 4) Log::get().note("flat discover projection-like bytes: %u further candidates omitted", plausible - 4);
}

void report(uint64_t frame, const char* phase) {
    uint32_t frameDraws = 0, frameDepth = 0, outDraws = 0;
    for (uint32_t i = 0; i < g.targetCount; ++i) {
        const Target& t = g.targets[i];
        frameDraws += t.draws; frameDepth += t.depthDraws;
        if (t.color == g.backbuffer) outDraws += t.draws;
    }
    if (frameDepth) ++g.framesWithDepth;
    if (outDraws) ++g.framesToOutput;
    g.totalDraws += frameDraws; g.totalDepthDraws += frameDepth;
    Log::get().note("flat discover %s frame=%llu profile=flat request=%s treatment=refused reason=uncertified-scene-projection-depth-boundary certificate=%u presents=%u test=%u failed=%u depth-frames=%u output-frames=%u draws=%u depth-draws=%u copies=%u dispatches=%u unknown-lists=%u foreign-thread-calls=%llu overflow(view,target,edge,cb,clear)=%u,%u,%u,%u,%u output=%p %ux%u fmt=%u",
                    phase, static_cast<unsigned long long>(frame),
                    Config::get().requestedTemporalMode().c_str(),
                    flatTemporalEvidenceComplete(g.proof) ? 1 : 0, g.presents,
                    g.testPresents, g.rejectedPresents, g.framesWithDepth,
                    g.framesToOutput, g.totalDraws, g.totalDepthDraws,
                    g.totalCopies, g.totalDispatches, g.unknownLists,
                    static_cast<unsigned long long>(detail::g_flatTemporalForeignCalls.load(std::memory_order_relaxed)),
                    g.viewOverflow, g.targetOverflow, g.edgeOverflow, g.cbOverflow,
                    g.depthClearOverflow,
                    g.backbuffer, g.backW, g.backH, g.backFmt);
    // One frame's strongest depth-bearing target and up to its direct output
    // routes. This keeps a busy renderer's log bounded; raw pointer identities
    // are expressly only valid for this one frame.
    const Target* best = nullptr;
    for (uint32_t i = 0; i < g.targetCount; ++i) {
        const Target& t = g.targets[i];
        if (!best || t.depthDraws > best->depthDraws) best = &t;
    }
    if (best) {
        Log::get().note("flat discover candidate frame=%llu color=%p depth=%p target=%ux%u viewport=%ux%u fmt=%u draws=%u depth-draws=%u q=%u..%u clears(color,depth)=%u,%u VSb0=%p VS=%016llX; candidate only",
                        static_cast<unsigned long long>(frame), best->color, best->depth,
                        best->w, best->h, best->vpW, best->vpH,
                        best->fmt, best->draws, best->depthDraws,
                        best->first, best->last, best->clearColor, best->clearDepth,
                        best->vsCb, static_cast<unsigned long long>(best->vsHash));
        printCb(*best);
        bool foundClear = false;
        for (uint32_t i = 0; i < g.depthClearCount; ++i) {
            const DepthClear& clear = g.depthClears[i];
            if (clear.dsv != best->dsv) continue;
            foundClear = true;
            Log::get().note("flat discover matched depth clear: dsv=%p flags=%u depth=%.6g calls=%u last-q=%u versus candidate draws q=%u..%u; encoding/planes unproved",
                            best->dsv, clear.flags, clear.value, clear.count,
                            clear.seq, best->first, best->last);
            break;
        }
        if (!foundClear)
            Log::get().note("flat discover matched depth clear: none in sampled frame; depth ownership/encoding unproved");
    } else {
        Log::get().note("flat discover candidate: none in sampled frame; no scene certificate");
    }
    uint32_t printed = 0;
    for (uint32_t i = 0; i < g.edgeCount && printed < 10; ++i) {
        const Edge& e = g.edges[i];
        if (e.dst != g.backbuffer && (!best || e.src != best->color)) continue;
        Log::get().note("flat discover lineage frame=%llu kind=%c source=%p dest=%p calls=%u q=%u..%u (S means bound SRV, not proved shader read)",
                        static_cast<unsigned long long>(frame), e.kind,
                        e.src, e.dst, e.count, e.first, e.last);
        ++printed;
    }
    if (!printed) Log::get().note("flat discover lineage: no direct sampled/copy edge to output or from candidate in sampled frame");
    Log::get().note("flat discover forward-Present evidence: draws=%u copies=%u; Present1 and upstream mod effects are not observed, so mod order is unqualified",
                    g.forwardedDraws, g.forwardedCopies);
}

void clearFrame() {
    std::memset(g.views, 0, sizeof(g.views));
    std::memset(g.targets, 0, sizeof(g.targets));
    std::memset(g.edges, 0, sizeof(g.edges));
    std::memset(g.depthClears, 0, sizeof(g.depthClears));
    g.targetCount = g.edgeCount = g.serial = 0;
    g.depthClearCount = 0;
    ++g.epoch;
    g.current = nullptr; g.sampledTarget = nullptr;
    g.boundRtv = g.boundDsv = nullptr;
    g.forwardingPresent = false;
    std::memset(g.sampledSrv, 0, sizeof(g.sampledSrv));
}

}  // namespace

void flatTemporalStart(ID3D11Device* device) {
    if (!runtimeFlatProfile() || !device) return;
    detail::g_flatTemporalCapturing.store(false, std::memory_order_release);
    detail::g_flatTemporalOwnerThread.store(0, std::memory_order_release);
    detail::g_flatTemporalForeignCalls.store(0, std::memory_order_release);
    g = State{};
    g.device = device;
    g_waitingForPresent.store(true, std::memory_order_release);
    Log::get().note("flat temporal: discovery armed, awaiting first owned Present thread; then at most 120 s / 12000 Presents. Requested=%s; AA treatment refused until desktop camera, projection, depth and handoff are certified",
                    Config::get().requestedTemporalMode().c_str());
}

void flatTemporalArm() {
    if (!runtimeFlatProfile() || !g.device) return;
    const DWORD owner = detail::g_flatTemporalOwnerThread.load(std::memory_order_acquire);
    if (owner && owner != GetCurrentThreadId()) return;
    if (flatTemporalCapturing()) report(0, "rearmed");
    ID3D11Device* device = g.device;
    detail::g_flatTemporalCapturing.store(false, std::memory_order_release);
    flatTemporalStart(device);
    Log::get().note("flat temporal: dump_draws started a fresh bounded desktop discovery window");
}

void flatTemporalStop() {
    // Teardown can run under the loader lock on another thread. Set only
    // atomics; the static collector has no resources to release or free.
    detail::g_flatTemporalCapturing.store(false, std::memory_order_release);
    g_waitingForPresent.store(false, std::memory_order_release);
}

void flatTemporalBeforePresent(IDXGISwapChain* swap, uint64_t frame, UINT flags) {
    if (g_waitingForPresent.exchange(false, std::memory_order_acq_rel)) {
        const DWORD thread = GetCurrentThreadId();
        detail::g_flatTemporalOwnerThread.store(thread, std::memory_order_release);
        g.startedMs = nowMs();
        g.nextReportMs = g.startedMs + kReportMs;
        Log::get().note("flat temporal: first owned Present on thread %lu; warmup draws before this boundary were not observed; discovery now active",
                        static_cast<unsigned long>(thread));
        detail::g_flatTemporalCapturing.store(true, std::memory_order_release);
    }
    if (!flatTemporalCapturing() || !swap) return;
    ++g.serial;
    ID3D11Texture2D* buffer = nullptr;
    if (SUCCEEDED(swap->GetBuffer(0, __uuidof(ID3D11Texture2D),
                                  reinterpret_cast<void**>(&buffer))) && buffer) {
        if (g.backbuffer != buffer) {
            g.backbuffer = buffer;
            D3D11_TEXTURE2D_DESC d{}; buffer->GetDesc(&d);
            g.backW = d.Width; g.backH = d.Height;
            g.backFmt = static_cast<uint32_t>(d.Format);
        }
        buffer->Release();
    } else {
        g.backbuffer = nullptr;
    }
    if (flags & DXGI_PRESENT_TEST) ++g.testPresents;
    g.forwardingPresent = true;
    // The upstream mod chain may execute inside the forwarded Present. This
    // marks only EDVR's pre-forward boundary; hook order remains unqualified.
    (void)frame;
}

void flatTemporalAfterPresent(uint64_t frame, HRESULT result, UINT flags) {
    if (!flatTemporalCapturing()) return;
    ++g.serial;
    g.forwardingPresent = false;
    ++g.presents;
    if (FAILED(result) && !(flags & DXGI_PRESENT_TEST)) ++g.rejectedPresents;
    const uint64_t now = nowMs();
    const bool deadline = flatCaptureExpired(g.startedMs, now, g.presents,
                                             kCaptureMs, kMaxPresents);
    if (g.presents == 1 || now >= g.nextReportMs || deadline) {
        report(frame, deadline ? "final" : "sample");
        g.nextReportMs = now + kReportMs;
    } else {
        uint32_t depth = 0, output = 0;
        for (uint32_t i = 0; i < g.targetCount; ++i) {
            const Target& t = g.targets[i];
            g.totalDraws += t.draws; g.totalDepthDraws += t.depthDraws;
            depth += t.depthDraws;
            if (t.color == g.backbuffer) output += t.draws;
        }
        if (depth) ++g.framesWithDepth;
        if (output) ++g.framesToOutput;
    }
    if (deadline) {
        detail::g_flatTemporalCapturing.store(false, std::memory_order_release);
        Log::get().note("flat temporal: discovery complete; treatment stayed inactive: no flat scene/projection/depth/output certificate or verified jitter fallback");
    }
    clearFrame();
}

void flatTemporalBind(ID3D11RenderTargetView* rtv, ID3D11DepthStencilView* dsv) {
    if (!flatTemporalCapturing()) return;
    ++g.serial;
    g.boundRtv = rtv; g.boundDsv = dsv;
    g.current = nullptr;
    g.sampledTarget = nullptr;
}

void flatTemporalViewport(UINT count, const D3D11_VIEWPORT* vps) {
    if (!flatTemporalCapturing()) return;
    ++g.serial;
    if (count && vps) {
        g.viewportW = static_cast<uint32_t>(vps[0].Width);
        g.viewportH = static_cast<uint32_t>(vps[0].Height);
    } else {
        g.viewportW = g.viewportH = 0;
    }
}

void flatTemporalDraw(uint32_t count, uint32_t instances) {
    if (!flatTemporalCapturing()) return;
    ++g.serial;
    if (!g.current) {
        void* rtv = g.boundRtv ? g.boundRtv : bindingGet(BindSlot::Rtv0);
        void* dsv = g.boundDsv ? g.boundDsv : bindingGet(BindSlot::Dsv0);
        g.current = targetOf(rtv, dsv);
    }
    Target* t = g.current;
    if (!t) return;
    if (g.forwardingPresent) ++g.forwardedDraws;
    ++t->draws;
    if (!t->vpW) { t->vpW = g.viewportW; t->vpH = g.viewportH; }
    if (t->dsv) ++t->depthDraws;
    if (!t->first) t->first = g.serial;
    t->last = g.serial;
    if (!t->vsCb) {
        t->vsCb = bindingGet(BindSlot::VsCb0);
        t->vsHash = bindingShaderHash(BindSlot::Vs);
    }
    const void* cbPtr = bindingGet(BindSlot::VsCb0);
    for (uint32_t i = 0; i < g.cbCount; ++i) {
        Cb& cb = g.cbs[i];
        if (cb.resource == cbPtr) {
            if (cb.drawEpoch != g.epoch) cb.draws = 0;
            ++cb.draws; cb.drawSeq = g.serial;
            cb.drawEpoch = g.epoch;
            cb.vsHash = bindingShaderHash(BindSlot::Vs);
            break;
        }
    }
    if (g.sampledTarget != t->color) {
        g.sampledTarget = t->color;
        std::memset(g.sampledSrv, 0, sizeof(g.sampledSrv));
    }
    for (uint32_t i = 0; i < 4; ++i) {
        void* srv = bindingGet(static_cast<BindSlot>(static_cast<uint32_t>(BindSlot::PsSrv0) + i));
        if (srv == g.sampledSrv[i]) continue;
        g.sampledSrv[i] = srv;
        if (View* v = viewOf(srv)) edge(v->resource, t->color, 'S');
    }
    (void)count; (void)instances;
}

void flatTemporalClearColor(ID3D11RenderTargetView* rtv) {
    if (!flatTemporalCapturing()) return;
    ++g.serial;
    bool matched = false;
    for (uint32_t i = 0; i < g.targetCount; ++i)
        if (g.targets[i].rtv == rtv) { ++g.targets[i].clearColor; matched = true; }
    (void)matched;
}
void flatTemporalClearDepth(ID3D11DepthStencilView* dsv, UINT flags, float depth) {
    if (!flatTemporalCapturing()) return;
    ++g.serial;
    bool matched = false;
    for (uint32_t i = 0; i < g.targetCount; ++i)
        if (g.targets[i].dsv == dsv) { ++g.targets[i].clearDepth; matched = true; }
    (void)matched;
    DepthClear* clear = flatFindOrAdd(g.depthClears, g.depthClearCount,
        g.depthClearOverflow, [=](const DepthClear& x) { return x.dsv == dsv; });
    if (clear) {
        clear->dsv = dsv; clear->flags = flags; clear->value = depth;
        clear->seq = g.serial; ++clear->count;
    }
}
void flatTemporalTransfer(ID3D11Resource* dst, ID3D11Resource* src, char kind) {
    if (!flatTemporalCapturing()) return;
    ++g.serial; ++g.totalCopies;
    if (g.forwardingPresent) ++g.forwardedCopies;
    edge(src, dst, kind);
}
void flatTemporalDispatch() {
    if (!flatTemporalCapturing()) return;
    ++g.serial; ++g.totalDispatches;
}
void flatTemporalExecuteList(bool foreign) {
    if (!flatTemporalCapturing()) return;
    ++g.serial; ++g.unknownLists;
    g.proof.unknownDeferredWork = true;
    static uint32_t notes = 0;
    if (notes++ < 4) Log::get().note("flat discover ExecuteCommandList q=%u foreign=%u; deferred writes/camera order unqualified",
                                   g.serial, foreign ? 1 : 0);
}
void flatTemporalMap(ID3D11Resource* res, UINT sub, D3D11_MAP type, void* data) {
    if (!flatTemporalCapturing() || !res || !data || sub || type == D3D11_MAP_READ) return;
    const uint32_t width = bufferWidth(res);
    // The known VR scene block is at least 3776 bytes. Also sample a bound
    // smaller VS b0 buffer, but do not infer that either is flat's camera.
    if ((!width || width > 65536) || (width < 3776 && res != bindingGet(BindSlot::VsCb0))) return;
    if (Cb* cb = cbOf(res, width)) cb->mapped = data;
}
void flatTemporalUnmap(ID3D11Resource* res) {
    if (!flatTemporalCapturing() || !res) return;
    for (uint32_t i = 0; i < g.cbCount; ++i) {
        Cb& cb = g.cbs[i];
        if (cb.resource != res || !cb.mapped) continue;
        cb.copied = std::min(cb.width, kCbBytes);
        std::memcpy(cb.bytes, cb.mapped, cb.copied);
        cb.mapped = nullptr; cb.writeSeq = ++g.serial; ++cb.writes;
        cb.writeEpoch = g.epoch;
        return;
    }
}
void flatTemporalUpdate(ID3D11Resource* dst, const void* data, const D3D11_BOX* box) {
    if (!flatTemporalCapturing() || !dst || !data || box) return;
    const uint32_t width = bufferWidth(dst);
    if (!width || width > 65536 || (width < 3776 && dst != bindingGet(BindSlot::VsCb0))) return;
    if (Cb* cb = cbOf(dst, width)) {
        cb->copied = std::min(width, kCbBytes);
        std::memcpy(cb->bytes, data, cb->copied);
        cb->writeSeq = ++g.serial; ++cb->writes;
        cb->writeEpoch = g.epoch;
    }
}

}  // namespace edvr
