#include "flat_temporal.h"
#include "flat_temporal_model.h"
#include "flat_mono_frame.h"
#include "flat_compute_capture.h"
#include "flat_compute_model.h"
#include "flat_runtime.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <type_traits>

#include "../common/config.h"
#include "../common/log.h"
#include "../common/runtime_profile.h"
#include "binding_shadow.h"
#include "device_hook.h"
#include "engine_velocity.h"
#include "exposure_fix.h"

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
constexpr uint32_t kOutputEdges = 64;
constexpr uint32_t kLargeCbs = 32;
// Prior Epic frames admitted 32 small buffers and dropped at most 30 write
// observations: at most 62 distinct small buffers. Keep bounded headroom for
// their newly needed VS/PS projection slices without evicting scene buffers.
constexpr uint32_t kSmallCbs = 128;
constexpr uint32_t kCbs = kLargeCbs + kSmallCbs;
constexpr uint32_t kContracts = 224;
constexpr uint32_t kHandoffContracts = 32;  // reserved; 256 total records
constexpr uint32_t kMenuCopies = 4;
constexpr uint64_t kMenuCopyVs = 0xDEF19B035D5EDEDCull;
constexpr uint64_t kMenuCopyPs = 0xDED8796049C7BB4Aull;
constexpr uint32_t kCbBytes = kFlatCbExemplarBytes;
constexpr uint64_t kCaptureMs = 120000;
constexpr uint32_t kMaxUsefulFrames = 12000;
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
    uint32_t depthW = 0, depthH = 0, depthFmt = 0;
    uint32_t clearColor = 0, clearDepth = 0;
    uint32_t vpW = 0, vpH = 0;
    void* vsCb[2] = {};
    uint64_t vsHash = 0, psHash = 0;
};
struct Edge {
    const void* src = nullptr;
    const void* dst = nullptr;
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
    unsigned char camera[kFlatCameraBytes] = {};
    uint64_t cameraHash = 0;
    bool cameraValid = false;
    uint32_t b0Hash = 0, b2Hash = 0;
    unsigned char viewportRow[16] = {};
    bool viewportValid = false;
    unsigned char targetSizeRow[16] = {};
    bool targetSizeValid = false;
};
struct DepthClear {
    void* dsv = nullptr;
    uint32_t count = 0, flags = 0, seq = 0;
    float value = 0.0f;
};
struct MenuCopyView {
    void* view = nullptr;
    void* resource = nullptr;
    uint32_t w = 0, h = 0, fmt = 0, samples = 0, arrays = 0;
    uint32_t viewFmt = 0, viewDimension = 0, mip = 0, arraySlice = 0;
};
struct MenuCopy {
    uint32_t sequence = 0;
    uint64_t actualVs = 0, actualPs = 0;
    uint32_t viewportCount = 0;
    float viewport[6] = {};
    MenuCopyView source, destination, depth;
    uint32_t contractMatches = 0, targetMatches = 0, transferMatches = 0;
    uint32_t contractFirst = 0, contractLast = 0, contractDraws = 0;
    uint64_t contractVs = 0, contractPs = 0, cameraHash = 0;
    uint64_t cameraWriteEpoch = 0;
    uint32_t cameraWriteSeq = 0;
    void* contractDepth = nullptr;
    void* contractDsv = nullptr;
    uint32_t contractDepthW = 0, contractDepthH = 0, contractDepthFmt = 0;
    uint32_t targetFirst = 0, targetLast = 0, targetDraws = 0;
    uint64_t targetVs = 0, targetPs = 0;
    void* targetDepth = nullptr;
    void* targetDsv = nullptr;
    uint32_t targetDepthW = 0, targetDepthH = 0, targetDepthFmt = 0;
    uint32_t transferFirst = 0, transferLast = 0;
    char transferKind = 0;
};
struct State {
    ID3D11Device* device = nullptr;  // identity only
    uint64_t startedMs = 0, nextReportMs = 0;
    uint32_t presents = 0, rejectedPresents = 0, testPresents = 0;
    uint32_t serial = 0, usefulFrames = 0, framesWithDepth = 0, framesToOutput = 0;
    uint64_t epoch = 1;
    uint32_t totalDraws = 0, totalDepthDraws = 0, totalCopies = 0;
    uint32_t totalDispatches = 0, unknownLists = 0;
    uint32_t forwardedDraws = 0, forwardedCopies = 0;
    uint32_t viewOverflow = 0, targetOverflow = 0, edgeOverflow = 0;
    uint32_t outputEdgeOverflow = 0, largeCbOverflow = 0, smallCbOverflow = 0;
    bool inheritedVrWork = false;
    FlatTemporalProof proof;
    bool forwardingPresent = false;
    uint32_t viewportW = 0, viewportH = 0;
    float viewport[6] = {};
    uint32_t viewportCount = 0;
    void* backbuffer = nullptr;
    uint32_t backW = 0, backH = 0, backFmt = 0;
    View views[kViewSlots] = {};
    Target targets[kTargets] = {};
    uint32_t targetCount = 0;
    FlatCbExemplar exemplars[kTargets][2] = {};
    Edge edges[kEdges] = {};
    uint32_t edgeCount = 0;
    Edge outputEdges[kOutputEdges] = {};
    uint32_t outputEdgeCount = 0;
    Cb cbs[kCbs] = {};
    uint32_t largeCbCount = 0, smallCbCount = 0;
    FlatProjectionBinding projectionBindings[kFlatProjectionSlots] = {};
    uint32_t projectionBindCalls[2] = {};
    uint32_t projectionDetailsRemaining = 0;
    bool projectionManual = false;
    bool detailRefusalReported = false;
    uint32_t menuCopyReportsLeft = 0, menuCopyCount = 0, menuCopyOverflow = 0;
    MenuCopy menuCopies[kMenuCopies] = {};
    FlatContractRecord contracts[kContracts] = {};
    uint32_t contractCount = 0, contractOverflow = 0;
    FlatContractRecord handoffContracts[kHandoffContracts] = {};
    uint32_t handoffContractCount = 0, handoffContractOverflow = 0;
    uint32_t contractDraws[4] = {};
    uint32_t contractCamera[kFlatCameraAvailabilityCount] = {};
    uint32_t poolCameraDraws = 0;
    DepthClear depthClears[kTargets] = {};
    uint32_t depthClearCount = 0, depthClearOverflow = 0;
    Target* current = nullptr;
    void* sampledTarget = nullptr;
    void* sampledSrv[4] = {};
};
static_assert(std::is_trivially_copyable<State>::value,
              "flat discovery State must support direct static reset");
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
            if (bindingResolve(p, &info)) {
                v.resource = info.resource;
                if (info.isTexture2D) {
                    v.w = info.a; v.h = info.b; v.fmt = info.fmt;
                }
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
    if (View* v = viewOf(dsv)) {
        t.depth = v->resource;
        t.depthW = v->w; t.depthH = v->h; t.depthFmt = v->fmt;
    }
    return slot;
}

MenuCopyView menuCopyView(ID3D11View* view) {
    MenuCopyView result{};
    if (!view) return result;
    result.view = view;
    ID3D11Resource* resource = nullptr;
    view->GetResource(&resource);
    if (!resource) return result;
    result.resource = resource;
    ID3D11Texture2D* texture = nullptr;
    if (SUCCEEDED(resource->QueryInterface(__uuidof(ID3D11Texture2D),
        reinterpret_cast<void**>(&texture))) && texture) {
        D3D11_TEXTURE2D_DESC desc{};
        texture->GetDesc(&desc);
        result.w = desc.Width; result.h = desc.Height;
        result.fmt = static_cast<uint32_t>(desc.Format);
        result.samples = desc.SampleDesc.Count;
        result.arrays = desc.ArraySize;
        texture->Release();
    }
    resource->Release();
    return result;
}

void captureMenuCopy(ID3D11DeviceContext* ctx) {
    if (!g.projectionManual || !g.menuCopyReportsLeft ||
        !flatComputeCandidate() || !ctx) return;
    if (bindingShaderHash(BindSlot::Vs) != kMenuCopyVs ||
        bindingShaderHash(BindSlot::Ps) != kMenuCopyPs) return;
    if (g.menuCopyCount == kMenuCopies) { ++g.menuCopyOverflow; return; }

    MenuCopy& copy = g.menuCopies[g.menuCopyCount++];
    copy = MenuCopy{};
    copy.sequence = g.serial;
    ID3D11ShaderResourceView* source = nullptr;
    ID3D11RenderTargetView* output = nullptr;
    ID3D11DepthStencilView* depth = nullptr;
    ctx->PSGetShaderResources(0, 1, &source);
    ctx->OMGetRenderTargets(1, &output, &depth);
    ID3D11VertexShader* actualVs = nullptr;
    ID3D11PixelShader* actualPs = nullptr;
    ctx->VSGetShader(&actualVs, nullptr, nullptr);
    ctx->PSGetShader(&actualPs, nullptr, nullptr);
    copy.actualVs = lookupShaderHash(actualVs);
    copy.actualPs = lookupShaderHash(actualPs);
    if (actualVs) actualVs->Release();
    if (actualPs) actualPs->Release();
    D3D11_VIEWPORT viewport{};
    UINT viewportCount = 1;
    ctx->RSGetViewports(&viewportCount, &viewport);
    copy.viewportCount = viewportCount;
    if (viewportCount) {
        copy.viewport[0] = viewport.TopLeftX; copy.viewport[1] = viewport.TopLeftY;
        copy.viewport[2] = viewport.Width; copy.viewport[3] = viewport.Height;
        copy.viewport[4] = viewport.MinDepth; copy.viewport[5] = viewport.MaxDepth;
    }
    copy.source = menuCopyView(source);
    copy.destination = menuCopyView(output);
    copy.depth = menuCopyView(depth);
    if (source) {
        D3D11_SHADER_RESOURCE_VIEW_DESC desc{};
        source->GetDesc(&desc);
        copy.source.viewFmt = static_cast<uint32_t>(desc.Format);
        copy.source.viewDimension = static_cast<uint32_t>(desc.ViewDimension);
        if (desc.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2D)
            copy.source.mip = desc.Texture2D.MostDetailedMip;
        else if (desc.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2DARRAY) {
            copy.source.mip = desc.Texture2DArray.MostDetailedMip;
            copy.source.arraySlice = desc.Texture2DArray.FirstArraySlice;
        }
    }
    if (output) {
        D3D11_RENDER_TARGET_VIEW_DESC desc{};
        output->GetDesc(&desc);
        copy.destination.viewFmt = static_cast<uint32_t>(desc.Format);
        copy.destination.viewDimension = static_cast<uint32_t>(desc.ViewDimension);
        if (desc.ViewDimension == D3D11_RTV_DIMENSION_TEXTURE2D)
            copy.destination.mip = desc.Texture2D.MipSlice;
        else if (desc.ViewDimension == D3D11_RTV_DIMENSION_TEXTURE2DARRAY) {
            copy.destination.mip = desc.Texture2DArray.MipSlice;
            copy.destination.arraySlice = desc.Texture2DArray.FirstArraySlice;
        }
    }
    if (depth) {
        D3D11_DEPTH_STENCIL_VIEW_DESC desc{};
        depth->GetDesc(&desc);
        copy.depth.viewFmt = static_cast<uint32_t>(desc.Format);
        copy.depth.viewDimension = static_cast<uint32_t>(desc.ViewDimension);
        if (desc.ViewDimension == D3D11_DSV_DIMENSION_TEXTURE2D)
            copy.depth.mip = desc.Texture2D.MipSlice;
        else if (desc.ViewDimension == D3D11_DSV_DIMENSION_TEXTURE2DARRAY) {
            copy.depth.mip = desc.Texture2DArray.MipSlice;
            copy.depth.arraySlice = desc.Texture2DArray.FirstArraySlice;
        }
    }
    if (source) source->Release();
    if (output) output->Release();
    if (depth) depth->Release();

    // Record prior observed graphics work at the draw, while its sequence and
    // resource identities still describe this exact Present interval. These
    // collections do not observe arbitrary compute or unhooked GPU writes.
    for (uint32_t i = 0; i < g.contractCount + g.handoffContractCount; ++i) {
        const FlatContractRecord& r = i < g.contractCount ?
            g.contracts[i] : g.handoffContracts[i - g.contractCount];
        if (!copy.source.resource || r.key.color != copy.source.resource ||
            r.first >= copy.sequence) continue;
        ++copy.contractMatches;
        if (r.last <= copy.contractLast) continue;
        copy.contractFirst = r.first; copy.contractLast = r.last;
        copy.contractDraws = r.draws;
        copy.contractVs = r.key.vs; copy.contractPs = r.key.ps;
        copy.contractDepth = const_cast<void*>(r.key.depth);
        copy.contractDsv = const_cast<void*>(r.key.dsv);
        copy.contractDepthW = r.key.depthWidth;
        copy.contractDepthH = r.key.depthHeight;
        copy.contractDepthFmt = r.key.depthFormat;
        copy.cameraHash = r.key.cameraHash;
        copy.cameraWriteEpoch = r.firstWriteEpoch;
        copy.cameraWriteSeq = r.firstWriteSeq;
    }
    for (uint32_t i = 0; i < g.targetCount; ++i) {
        const Target& t = g.targets[i];
        if (!copy.source.resource || t.color != copy.source.resource ||
            !t.draws || t.first >= copy.sequence) continue;
        ++copy.targetMatches;
        if (t.last <= copy.targetLast) continue;
        copy.targetFirst = t.first; copy.targetLast = t.last;
        copy.targetDraws = t.draws;
        copy.targetVs = t.vsHash; copy.targetPs = t.psHash;
        copy.targetDepth = t.depth; copy.targetDsv = t.dsv;
        copy.targetDepthW = t.depthW; copy.targetDepthH = t.depthH;
        copy.targetDepthFmt = t.depthFmt;
    }
    for (uint32_t i = 0; i < g.edgeCount; ++i) {
        const Edge& e = g.edges[i];
        if (!copy.source.resource || e.dst != copy.source.resource ||
            (e.kind != 'R' && e.kind != 'C' && e.kind != 'V') ||
            e.first >= copy.sequence) continue;
        ++copy.transferMatches;
        if (e.last <= copy.transferLast) continue;
        copy.transferFirst = e.first; copy.transferLast = e.last;
        copy.transferKind = e.kind;
    }
}

void printMenuCopies(uint64_t frame) {
    if (!g.projectionManual || !g.menuCopyReportsLeft) return;
    --g.menuCopyReportsLeft;
    Log::get().note("flat menu-copy probe frame=%llu epoch=%llu shadow-pair-filter=DEF19B035D5EDEDC/DED8796049C7BB4A retained=%u capacity=%u overflow=%u reports-left=%u status=%s; actual shader/PS t0/OM RTV0/DSV getters reported separately; only two focused compute candidate frames are sampled, absent writer is not proof of no writer",
        static_cast<unsigned long long>(frame), static_cast<unsigned long long>(g.epoch), g.menuCopyCount, kMenuCopies,
        g.menuCopyOverflow, g.menuCopyReportsLeft,
        g.menuCopyCount ? "shadow-pair-observed" : "no-exact-shadow-pair-observed-in-candidate-frame");
    for (uint32_t i = 0; i < g.menuCopyCount; ++i) {
        const MenuCopy& c = g.menuCopies[i];
        Log::get().note("flat menu-copy draw frame=%llu epoch=%llu index=%u q=%u actual-VS=%016llX actual-PS=%016llX viewport-count-at-most-1=%u viewport=%.9g,%.9g,%.9g,%.9g,%.9g,%.9g; shader identities from getters, identities are frame-local, null getters are observed nulls",
            static_cast<unsigned long long>(frame), static_cast<unsigned long long>(g.epoch), i,
            c.sequence, static_cast<unsigned long long>(c.actualVs),
            static_cast<unsigned long long>(c.actualPs), c.viewportCount,
            c.viewport[0], c.viewport[1], c.viewport[2], c.viewport[3],
            c.viewport[4], c.viewport[5]);
        Log::get().note("flat menu-copy source frame=%llu index=%u PS0(view,resource,size,resource-fmt,view-fmt,dimension,mip,array-slice,samples,array-size)=%p,%p,%ux%u,%u,%u,%u,%u,%u,%u,%u RTV0=%p,%p,%ux%u,%u,%u,%u,%u,%u,%u,%u DSV=%p,%p,%ux%u,%u,%u,%u,%u,%u,%u,%u; non-Texture2D or unresolved resource has zero size, view dimension identifies unsupported layouts",
            static_cast<unsigned long long>(frame), i,
            c.source.view, c.source.resource, c.source.w, c.source.h, c.source.fmt,
            c.source.viewFmt, c.source.viewDimension, c.source.mip,
            c.source.arraySlice, c.source.samples, c.source.arrays,
            c.destination.view, c.destination.resource, c.destination.w,
            c.destination.h, c.destination.fmt, c.destination.viewFmt,
            c.destination.viewDimension, c.destination.mip,
            c.destination.arraySlice, c.destination.samples, c.destination.arrays,
            c.depth.view, c.depth.resource, c.depth.w, c.depth.h, c.depth.fmt,
            c.depth.viewFmt, c.depth.viewDimension, c.depth.mip,
            c.depth.arraySlice, c.depth.samples, c.depth.arrays);
        Log::get().note("flat menu-copy prior-graphics frame=%llu index=%u source=%p contract-matches=%u retained-latest(q,draws,VS,PS,depth,DSV,depth-size,fmt,camera-hash,camera-write-epoch,q)=%u..%u,%u,%016llX,%016llX,%p,%p,%ux%u,%u,%016llX,%llu,%u target-matches=%u latest-target(q,draws,first-VS,first-PS,depth,DSV,depth-size,fmt)=%u..%u,%u,%016llX,%016llX,%p,%p,%ux%u,%u transfers=%u latest-transfer(kind,q)=%c,%u..%u; aggregate target first shader is not necessarily its last writer, compute writes and unhooked work require separate evidence",
            static_cast<unsigned long long>(frame), i, c.source.resource,
            c.contractMatches, c.contractFirst, c.contractLast, c.contractDraws,
            static_cast<unsigned long long>(c.contractVs),
            static_cast<unsigned long long>(c.contractPs),
            c.contractDepth, c.contractDsv, c.contractDepthW, c.contractDepthH,
            c.contractDepthFmt, static_cast<unsigned long long>(c.cameraHash),
            static_cast<unsigned long long>(c.cameraWriteEpoch), c.cameraWriteSeq,
            c.targetMatches, c.targetFirst, c.targetLast, c.targetDraws,
            static_cast<unsigned long long>(c.targetVs),
            static_cast<unsigned long long>(c.targetPs),
            c.targetDepth, c.targetDsv, c.targetDepthW, c.targetDepthH,
            c.targetDepthFmt, c.transferMatches, c.transferKind ? c.transferKind : '-',
            c.transferFirst, c.transferLast);
        Log::get().note("flat menu-copy compute-check frame=%llu index=%u source=%p copy-q=%u; compare same-frame flat compute CS-UAV/graphics writer resource and q < copy-q; this passive record cannot certify compute writes; coverage-drops(view,target,contract,handoff,edge,unknown-lists)=%u,%u,%u,%u,%u,%u",
            static_cast<unsigned long long>(frame), i, c.source.resource,
            c.sequence, g.viewOverflow, g.targetOverflow, g.contractOverflow,
            g.handoffContractOverflow, g.edgeOverflow, g.unknownLists);
    }
}

void edge(void* src, void* dst, char kind) {
    flatRecordEdge(g.edges, g.edgeCount, g.edgeOverflow,
                   g.outputEdges, g.outputEdgeCount, g.outputEdgeOverflow,
                   src, dst, kind, g.backbuffer, g.serial);
}

Cb* cbOf(void* res, uint32_t width) {
    const uint32_t base = width >= 3776 ? 0 : kLargeCbs;
    uint32_t& count = width >= 3776 ? g.largeCbCount : g.smallCbCount;
    for (uint32_t i = 0; i < count; ++i)
        if (g.cbs[base + i].resource == res) return &g.cbs[base + i];
    if (count == (width >= 3776 ? kLargeCbs : kSmallCbs)) {
        ++(width >= 3776 ? g.largeCbOverflow : g.smallCbOverflow);
        return nullptr;
    }
    Cb& cb = g.cbs[base + count++];
    // Do not clear the 4 KiB payload each frame; copied remains zero until a
    // complete CPU write has filled it.
    cb.resource = res; cb.width = width; cb.mapped = nullptr;
    cb.copied = cb.writes = cb.draws = cb.writeSeq = cb.drawSeq = 0;
    cb.writeEpoch = cb.drawEpoch = cb.vsHash = 0;
    cb.cameraValid = false; cb.cameraHash = 0; cb.viewportValid = cb.targetSizeValid = false;
    cb.b0Hash = cb.b2Hash = 0;
    return &cb;
}

Cb* findCb(void* res) {
    if (!res) return nullptr;
    for (uint32_t i = 0; i < g.largeCbCount; ++i)
        if (g.cbs[i].resource == res) return &g.cbs[i];
    for (uint32_t i = 0; i < g.smallCbCount; ++i)
        if (g.cbs[kLargeCbs + i].resource == res) return &g.cbs[kLargeCbs + i];
    return nullptr;
}

void invalidateCb(void* res) {
    if (Cb* cb = findCb(res)) {
        cb->copied = 0;
        cb->writeEpoch = 0;
        cb->mapped = nullptr;
        cb->cameraValid = false; cb->cameraHash = 0;
        cb->viewportValid = cb->targetSizeValid = false;
        cb->b0Hash = cb->b2Hash = 0;
    }
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

void projectionHashes(Cb& cb) {
    const uint32_t b0Bytes = cb.copied > 64 ? std::min(cb.copied - 64, kFlatB0Bytes) : 0;
    cb.b0Hash = flatProjectionHash(cb.bytes + 64, b0Bytes);
    cb.b2Hash = flatProjectionHash(cb.bytes, std::min(cb.copied, kFlatB2Bytes));
}

void printCb(const Target& t, uint32_t slot, const FlatCbExemplar& cb) {
    if (!cb.copied) {
        Log::get().note("flat discover CB: target=%p VS b%u first-bound=%p no draw-frozen current-frame CPU write; projection owner unknown",
                        t.color, slot, t.vsCb[slot]);
        return;
    }
    Log::get().note("flat discover CB: target=%p VS b%u=%p width=%u copied=%u hash=%08X depth-draws=%u write-epoch=%llu draw-epoch=%llu write-q=%u draw-q=%u vs=%016llX (bytes frozen at this target draw; shader binding observed, consumption unproved)",
                    t.color, slot, cb.resource, cb.width, cb.copied,
                    hashBytes(cb.bytes, cb.copied), t.depthDraws,
                    static_cast<unsigned long long>(cb.writeEpoch),
                    static_cast<unsigned long long>(cb.drawEpoch),
                    cb.writeSeq, cb.drawSeq,
                    static_cast<unsigned long long>(cb.shader));
    // The known VR scene block has view rows at float 932. Printing them is
    // evidence to compare, not permission to assume flat uses the same block.
    if (cb.copied >= (932 + 12) * sizeof(float)) {
        float row[12];
        std::memcpy(row, cb.bytes + 932 * sizeof(float), sizeof(row));
        Log::get().note("flat discover CB VS b%u candidate f932 rows: %.5g %.5g %.5g %.5g | %.5g %.5g %.5g %.5g | %.5g %.5g %.5g %.5g",
                        slot, row[0], row[1], row[2], row[3], row[4], row[5], row[6], row[7],
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
            Log::get().note("flat discover projection-like bytes: VS b%u=%p byte-offset=%u diag=%.6g,%.6g,%.6g tail=%.6g,%.6g,%.6g,%.6g; shape only, shader consumption/inverse unproved",
                            slot, cb.resource, offset, m[0], m[5], m[10],
                            m[11], m[12], m[14], m[15]);
        }
    }
    if (!plausible) Log::get().note("flat discover projection-like bytes: none in frozen VS b%u; projection owner unknown", slot);
    else if (plausible > 4) Log::get().note("flat discover projection-like bytes: VS b%u %u further candidates omitted", slot, plausible - 4);
}

void printContracts(uint64_t frame, bool details) {
    Log::get().note("flat discover contract inventory frame=%llu world-retained=%u world-capacity=%u world-dropped-draw-observations=%u handoff-retained=%u handoff-capacity=%u handoff-dropped-draw-observations=%u eligible-draws(pool,screen,output)=%u,%u,%u camera-draws(available,missing-buffer,invalid-write,old-frame,later-write)=%u,%u,%u,%u,%u pool-camera-draws=%u; zero eligible is no matching draw, dropped observations are incomplete evidence; family declaration and PS bindings only, shader consumption unproved",
        static_cast<unsigned long long>(frame), g.contractCount, kContracts,
        g.contractOverflow, g.handoffContractCount, kHandoffContracts,
        g.handoffContractOverflow, g.contractDraws[kFlatContractPool],
        g.contractDraws[kFlatContractScreen], g.contractDraws[kFlatContractOutput],
        g.contractCamera[kFlatCameraAvailable], g.contractCamera[kFlatCameraMissingBuffer],
        g.contractCamera[kFlatCameraInvalidWrite], g.contractCamera[kFlatCameraOldFrame],
        g.contractCamera[kFlatCameraLaterWrite], g.poolCameraDraws);
    if (!details) return;
    for (uint32_t i = 0; i < g.contractCount + g.handoffContractCount; ++i) {
        const bool handoff = i >= g.contractCount;
        const FlatContractRecord& r = handoff ? g.handoffContracts[i - g.contractCount] : g.contracts[i];
        const FlatContractObservation& k = r.key;
        const char* kind = k.kind == kFlatContractPool ? "declared-pool-family" :
                           k.kind == kFlatContractOutput ? "output" : "screen-format";
        Log::get().note("flat discover contract frame=%llu index=%u bank=%s class=%s color=%p depth=%p rtv=%p dsv=%p target=%ux%u fmt=%u depth-size=%ux%u depth-fmt=%u VS=%016llX PS=%016llX VSb1=%p draws=%u q=%u..%u count=%u..%u instances=%u..%u viewport-count=%u viewport=%.9g,%.9g,%.9g,%.9g,%.9g,%.9g camera=%s hash=%016llX write-epoch=%llu..%llu write-q=%u..%u draw-epoch=%llu",
            static_cast<unsigned long long>(frame), i, handoff ? "handoff" : "world", kind, k.color, k.depth,
            k.rtv, k.dsv, k.width, k.height, k.format,
            k.depthWidth, k.depthHeight, k.depthFormat,
            static_cast<unsigned long long>(k.vs), static_cast<unsigned long long>(k.ps),
            k.b1, r.draws, r.first, r.last, r.firstCount, r.lastCount,
            r.firstInstances, r.lastInstances, k.viewportCount,
            k.viewport[0], k.viewport[1], k.viewport[2], k.viewport[3], k.viewport[4], k.viewport[5],
            k.camera ? "same-frame-write-frozen" : "unavailable",
            static_cast<unsigned long long>(k.cameraHash),
            static_cast<unsigned long long>(r.firstWriteEpoch),
            static_cast<unsigned long long>(r.lastWriteEpoch), r.firstWriteSeq, r.lastWriteSeq,
            static_cast<unsigned long long>(g.epoch));
        Log::get().note("flat discover contract sources frame=%llu index=%u PS0(view,resource)=%p,%p PS1=%p,%p PS2=%p,%p PS3=%p,%p; shadow-observed bindings, alias unbinds/higher slots unobserved",
            static_cast<unsigned long long>(frame), i,
            k.srvView[0], k.srvResource[0], k.srvView[1], k.srvResource[1],
            k.srvView[2], k.srvResource[2], k.srvView[3], k.srvResource[3]);
        if (!k.camera) continue;
        float rows[6][4];
        std::memcpy(rows, r.camera, sizeof(rows));
        for (uint32_t row = 0; row < 6; ++row)
            Log::get().note("flat discover contract camera frame=%llu index=%u row=%u %.9g %.9g %.9g %.9g; exact captured CPU bytes, consumption unproved",
                static_cast<unsigned long long>(frame), i, 270 + row,
                rows[row][0], rows[row][1], rows[row][2], rows[row][3]);
    }
}

FlatMonoFrame printMonoInput(uint64_t frame) {
    FlatMonoFrameInput input{};
    input.world = g.contracts; input.worldCount = g.contractCount;
    input.handoff = g.handoffContracts; input.handoffCount = g.handoffContractCount;
    input.output = g.backbuffer; input.outputWidth = g.backW;
    input.outputHeight = g.backH; input.outputFormat = g.backFmt;
    input.frame = frame; input.epoch = g.epoch;
    input.droppedViews = g.viewOverflow; input.droppedTargets = g.targetOverflow;
    input.droppedWorld = g.contractOverflow; input.droppedHandoff = g.handoffContractOverflow;
    input.droppedLargeCb = g.largeCbOverflow; input.droppedSmallCb = g.smallCbOverflow;
    input.unknownLists = g.unknownLists;
    input.foreignCalls = detail::g_flatTemporalForeignCalls.load(std::memory_order_relaxed);
    input.supportedPair = engineVelocityPoolFamilyPair;
    const FlatMonoFrame mono = flatSelectMonoFrame(input);
    // Always print a verdict, even for an empty report or refused metadata.
    // This is report-time selection only; it does not arm the motion producer.
    Log::get().note("flat discover mono-input frame=%llu epoch=%llu selector-called=1 status=%s reason=%s color=%p hdr=%p depth=%p dsv=%p depth-fmt=%u VSb1=%p camera-hash=%016llX near=%.9g render=%ux%u output=%p %ux%u supported-pair-draws=%u unsupported-pair-draws=%u source-q=%u..%u hdr-q=%u..%u tone-q=%u copy-q=%u later-output-q=%u; observational candidate, identities frame-local, observer=passive runtime-status=separate certificate=0",
        static_cast<unsigned long long>(mono.frame), static_cast<unsigned long long>(mono.epoch),
        mono.selected() ? "selected" : "refused", flatMonoReasonName(mono.reason),
        mono.color, mono.hdr, mono.depth, mono.dsv, mono.depthFormat, mono.sceneConstants,
        static_cast<unsigned long long>(mono.cameraHash), mono.nearPlane,
        mono.renderWidth, mono.renderHeight, mono.output, mono.outputWidth, mono.outputHeight,
        mono.supportedDraws, mono.unsupportedDraws, mono.sourceFirst, mono.sourceLast,
        mono.hdrFirst, mono.hdrLast, mono.toneSequence, mono.copySequence, mono.firstLaterOutput);
    // A handoff-chain refusal never opens the detail-report gate, because that
    // gate needs a selected frame -- the exact state this diagnoses. Dump the
    // retained records and their creation bytecode a bounded number of times
    // per session, so a post chain the settings changed (new tone variant, or
    // extra passes between tone and copy) names itself in the log and on disk.
    // Only spend a dump once handoff records exist: startup refuses with empty
    // tables, and the budget burned there once hid the broken chain itself.
    static uint32_t chainRefusalDumps = 0;
    if (!mono.selected() && g.handoffContractCount && chainRefusalDumps < 2 &&
        (mono.reason == FlatMonoReason::NoTonePass || mono.reason == FlatMonoReason::AmbiguousTonePass ||
         mono.reason == FlatMonoReason::InvalidTonePass || mono.reason == FlatMonoReason::NoOutputCopy ||
         mono.reason == FlatMonoReason::AmbiguousOutputCopy || mono.reason == FlatMonoReason::InvalidOutputCopy ||
         mono.reason == FlatMonoReason::BrokenLineage || mono.reason == FlatMonoReason::WrongOrder)) {
        ++chainRefusalDumps;
        printContracts(frame, true);
        for (uint32_t i = 0; i < g.handoffContractCount; ++i) {
            captureFlatProbeShader('v', g.handoffContracts[i].key.vs);
            captureFlatProbeShader('p', g.handoffContracts[i].key.ps);
        }
    }
    return mono;
}

void printProjectionPayload(uint64_t frame, uint32_t id, uint32_t offset,
                            const unsigned char* bytes, uint32_t copied) {
    // Four float4 rows per line, at most 17 rows per bounded payload. Printed
    // once per identical offset/byte payload in this selected report.
    const uint32_t rows = copied / 16;
    for (uint32_t row = 0; row < rows; row += 4) {
        char values[512] = {}, bits[160] = {};
        size_t used = 0, bitsUsed = 0;
        const uint32_t count = std::min(4u, rows - row);
        for (uint32_t n = 0; n < count; ++n) {
            float v[4]; std::memcpy(v, bytes + (row + n) * 16, sizeof(v));
            const int written = std::snprintf(values + used, sizeof(values) - used,
                "%s%.9g %.9g %.9g %.9g", n ? " | " : "", v[0], v[1], v[2], v[3]);
            if (written < 0 || static_cast<size_t>(written) >= sizeof(values) - used) break;
            used += static_cast<size_t>(written);
            uint32_t raw[4]; std::memcpy(raw, bytes + (row + n) * 16, sizeof(raw));
            const int bitCount = std::snprintf(bits + bitsUsed, sizeof(bits) - bitsUsed,
                "%s%08X %08X %08X %08X", n ? " | " : "", raw[0], raw[1], raw[2], raw[3]);
            if (bitCount < 0 || static_cast<size_t>(bitCount) >= sizeof(bits) - bitsUsed) break;
            bitsUsed += static_cast<size_t>(bitCount);
        }
        Log::get().note("flat discover projection-payload frame=%llu ref=%u rows=%u..%u values=%s bits-u32=%s",
            static_cast<unsigned long long>(frame), id, offset / 16 + row,
            offset / 16 + row + count - 1, values, bits);
    }
    // Normal D3D CB widths are 16-byte aligned; preserve any short diagnostic
    // tail too, so the printed bytes always reconstruct the recorded hash.
    if (copied % 16) {
        char tail[31] = {};
        for (uint32_t i = 0; i < copied % 16; ++i)
            std::snprintf(tail + i * 2, sizeof(tail) - i * 2, "%02X", bytes[rows * 16 + i]);
        Log::get().note("flat discover projection-payload-tail frame=%llu ref=%u byte-offset=%u bytes-hex=%s",
            static_cast<unsigned long long>(frame), id, offset + rows * 16, tail);
    }
}

void printProjections(uint64_t frame, const FlatMonoFrame& mono, bool details) {
    const char* status = !g.projectionManual ? "not-manually-armed" :
        !mono.selected() ? "selector-refused-no-projection-payload" :
        !details ? "detail-not-admitted" : "emitted";
    Log::get().note("flat discover projection-summary frame=%llu details=%s selector=%s detail-budget=%u bindings-observed(VS,PS)=%u,%u cb-pool-used(large,small)=%u,%u cb-pool-capacity=%u,%u cb-drops=%u,%u; VS b0 rows4..7, VS b2 rows0..16, PS b2 rows0..16; stage bindings are independent and unobserved is not unbound",
        static_cast<unsigned long long>(frame), status, flatMonoReasonName(mono.reason),
        g.projectionDetailsRemaining, g.projectionBindCalls[0], g.projectionBindCalls[1],
        g.largeCbCount, g.smallCbCount, kLargeCbs, kSmallCbs, g.largeCbOverflow, g.smallCbOverflow);
    if (!details || !mono.selected()) return;
    struct Payload { const unsigned char* bytes = nullptr; uint32_t copied = 0, hash = 0, offset = 0; };
    constexpr uint32_t kPayloads = 128; // bounded log payload; all binding metadata still prints
    Payload payloads[kPayloads] = {};
    uint32_t used = 0, records = 0, format60 = 0, dropped = 0;
    uint32_t coverage[kFlatProjectionSlots][8] = {};
    static const char* stages[kFlatProjectionSlots] = {"VSb0", "VSb2", "PSb2"};
    for (uint32_t index = 0; index < g.contractCount + g.handoffContractCount; ++index) {
        const auto& r = index < g.contractCount ? g.contracts[index] : g.handoffContracts[index - g.contractCount];
        const auto& k = r.key;
        if (k.depth != mono.depth && k.color != mono.hdr && k.color != mono.color && k.color != mono.output) continue;
        ++records;
        if (k.format == 60 && k.depth == mono.depth) format60 += r.draws;
        for (uint32_t slot = 0; slot < kFlatProjectionSlots; ++slot) {
            const auto& p = k.projection[slot];
            ++coverage[slot][static_cast<uint32_t>(p.status)];
            uint32_t ref = 0;
            if (p.copied) {
                const uint32_t offset = flatProjectionOffset(slot);
                for (uint32_t i = 0; i < used; ++i) {
                    const auto& known = payloads[i];
                    if (known.copied == p.copied && known.hash == p.hash && known.offset == offset &&
                        std::memcmp(known.bytes, r.projectionBytes(slot), p.copied) == 0) { ref = i + 1; break; }
                }
                if (!ref && used < kPayloads) {
                    payloads[used] = {r.projectionBytes(slot), p.copied, p.hash, offset};
                    ref = ++used;
                    printProjectionPayload(frame, ref, offset, r.projectionBytes(slot), p.copied);
                } else if (!ref) ++dropped;
            }
            Log::get().note("flat discover projection-binding frame=%llu index=%u stage=%s resource=%p width=%u offset=%u requested=%u copied=%u hash=%08X status=%s ref=%u write-epoch=%llu..%llu write-q=%u..%u draw-epoch=%llu q=%u..%u; bytes frozen at draw, ownership and correction eligibility unproved",
                static_cast<unsigned long long>(frame), index, stages[slot], p.resource, p.width,
                flatProjectionOffset(slot), flatProjectionCapacity(slot), p.copied, p.hash,
                flatProjectionStatusName(p.status), ref,
                static_cast<unsigned long long>(r.firstProjectionEpoch[slot]),
                static_cast<unsigned long long>(r.lastProjectionEpoch[slot]),
                r.firstProjectionSeq[slot], r.lastProjectionSeq[slot],
                static_cast<unsigned long long>(g.epoch), r.first, r.last);
        }
    }
    for (uint32_t slot = 0; slot < kFlatProjectionSlots; ++slot) {
        const auto* c = coverage[slot];
        Log::get().note("flat discover projection-coverage frame=%llu stage=%s records=%u state-counts(binding-unknown,unbound,write-missing,invalid,old,later,short,complete)=%u,%u,%u,%u,%u,%u,%u,%u selected-depth-format60-draws=%u payloads=%u payload-drops=%u detail-budget-left=%u; coverage counts availability only",
            static_cast<unsigned long long>(frame), stages[slot], records, c[0], c[1], c[2], c[3], c[4], c[5], c[6], c[7],
            format60, used, dropped, g.projectionDetailsRemaining);
    }
}

void report(uint64_t frame, const char* phase) {
    Log::get().note("flat discover %s frame=%llu profile=flat request=%s observer=passive runtime-status=separate certificate=%u presents=%u useful-frames=%u test=%u failed=%u depth-frames=%u output-frames=%u draws=%u depth-draws=%u copies=%u dispatches=%u unknown-lists=%u foreign-thread-calls=%llu frame-dropped-observations(view,target,edge,output-edge,large-cb,small-cb,clear)=%u,%u,%u,%u,%u,%u,%u output=%p %ux%u fmt=%u",
                    phase, static_cast<unsigned long long>(frame),
                    Config::get().requestedTemporalMode().c_str(),
                    flatTemporalEvidenceComplete(g.proof) ? 1 : 0, g.presents,
                    g.usefulFrames, g.testPresents, g.rejectedPresents, g.framesWithDepth,
                    g.framesToOutput, g.totalDraws, g.totalDepthDraws,
                    g.totalCopies, g.totalDispatches, g.unknownLists,
                    static_cast<unsigned long long>(detail::g_flatTemporalForeignCalls.load(std::memory_order_relaxed)),
                    g.viewOverflow, g.targetOverflow, g.edgeOverflow,
                    g.outputEdgeOverflow, g.largeCbOverflow, g.smallCbOverflow,
                    g.depthClearOverflow,
                    g.backbuffer, g.backW, g.backH, g.backFmt);
    const FlatMonoFrame mono = printMonoInput(frame);
    if (flatComputeManual()) {
        printContracts(frame, false);
        printProjections(frame, mono, false);
        Log::get().note("flat discover detail-policy frame=%llu decision=focused-compute-probe; legacy projection/target/edge payloads suppressed, two candidate-frame attempts have their own bounded report",
            static_cast<unsigned long long>(frame));
        return;
    }
    const auto admission = flatTakeDetailSample(g.projectionManual, frame,
        g.contractCount + g.handoffContractCount, mono.selected(),
        g.projectionDetailsRemaining, g.detailRefusalReported);
    const bool details = admission == FlatDetailAdmission::Selected || admission == FlatDetailAdmission::Refusal;
    Log::get().note("flat discover detail-policy frame=%llu decision=%s detail-budget-left=%u max-full-reports=2 payload-capacity-per-report=128; startup summary only, first useful manual refusal may consume one sample",
        static_cast<unsigned long long>(frame), flatDetailAdmissionName(admission), g.projectionDetailsRemaining);
    printContracts(frame, details);
    printProjections(frame, mono, details);
    if (!details) return;
    // At most 128 admitted target pairs per sampled frame. Every row matters:
    // depth-only passes and intermediate colour paths can outdraw the scene.
    uint32_t depthOnly = 0, colorDepth = 0, unresolved = 0;
    const Target* best = nullptr;
    for (uint32_t i = 0; i < g.targetCount; ++i) {
        const Target& t = g.targets[i];
        if (!t.draws) continue;
        if (!t.color && t.depth) ++depthOnly;
        if (t.color && t.depth) ++colorDepth;
        if (t.rtv && !t.color) ++unresolved;
        Log::get().note("flat discover target frame=%llu index=%u color=%p depth=%p rtv=%p dsv=%p target=%ux%u viewport=%ux%u fmt=%u draws=%u depth-draws=%u q=%u..%u clears(color,depth)=%u,%u VSb0=%p VSb1=%p VS=%016llX first-PS=%016llX depth-size=%ux%u depth-fmt=%u class=%s",
                        static_cast<unsigned long long>(frame), i,
                        t.color, t.depth, t.rtv, t.dsv, t.w, t.h,
                        t.vpW, t.vpH, t.fmt, t.draws, t.depthDraws,
                        t.first, t.last, t.clearColor, t.clearDepth,
                        t.vsCb[0], t.vsCb[1],
                        static_cast<unsigned long long>(t.vsHash),
                        static_cast<unsigned long long>(t.psHash), t.depthW, t.depthH, t.depthFmt,
                        flatSceneCandidateEligible(t.color, t.depth, t.depthDraws)
                            ? "color+depth-candidate" :
                        !t.color && t.depth ? "depth-only-excluded" : "other-unproved");
        if (flatSceneCandidateEligible(t.color, t.depth, t.depthDraws) &&
            (!best || t.depthDraws > best->depthDraws)) best = &t;
    }
    Log::get().note("flat discover inventory frame=%llu admitted=%u capacity=%u dropped-target-observations=%u depth-only=%u color+depth=%u unresolved-color-views=%u; target pointers are frame-local, classifications are not scene proof",
                    static_cast<unsigned long long>(frame), g.targetCount,
                    kTargets, g.targetOverflow, depthOnly, colorDepth, unresolved);
    if (best) {
        const uint32_t index = static_cast<uint32_t>(best - g.targets);
        for (uint32_t slot = 0; slot < 2; ++slot) printCb(*best, slot, g.exemplars[index][slot]);
        Log::get().note("flat discover candidate frame=%llu color=%p depth=%p target=%ux%u viewport=%ux%u fmt=%u draws=%u depth-draws=%u q=%u..%u clears(color,depth)=%u,%u VSb0=%p VSb1=%p VS=%016llX; strongest admitted color+depth shape only, not scene certificate",
                        static_cast<unsigned long long>(frame), best->color, best->depth,
                        best->w, best->h, best->vpW, best->vpH,
                        best->fmt, best->draws, best->depthDraws,
                        best->first, best->last, best->clearColor, best->clearDepth,
                        best->vsCb[0], best->vsCb[1],
                        static_cast<unsigned long long>(best->vsHash));
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
        Log::get().note("flat discover candidate: no admitted color+depth draw in sampled frame; depth-only targets excluded, no scene certificate");
    }
    for (uint32_t i = 0; i < g.outputEdgeCount; ++i) {
        const Edge& e = g.outputEdges[i];
        Log::get().note("flat discover output-lineage frame=%llu kind=%c source=%p dest=%p calls=%u q=%u..%u (S is PS slots 0..3 shadow-observed only; alias unbinds and higher slots unobserved)",
                        static_cast<unsigned long long>(frame), e.kind,
                        e.src, e.dst, e.count, e.first, e.last);
    }
    Log::get().note("flat discover output-lineage inventory retained=%u capacity=%u dropped-route-observations=%u output-known=%u; absent routes do not prove separation",
                    g.outputEdgeCount, kOutputEdges, g.outputEdgeOverflow,
                    g.backbuffer ? 1 : 0);
    for (uint32_t i = 0; i < g.edgeCount; ++i) {
        const Edge& e = g.edges[i];
        Log::get().note("flat discover general-lineage frame=%llu kind=%c source=%p dest=%p calls=%u q=%u..%u (S is PS slots 0..3 shadow-observed only; alias unbinds and higher slots unobserved)",
                        static_cast<unsigned long long>(frame), e.kind,
                        e.src, e.dst, e.count, e.first, e.last);
    }
    Log::get().note("flat discover general-lineage inventory retained=%u capacity=%u dropped-route-observations=%u; absent route is inconclusive",
                    g.edgeCount, kEdges, g.edgeOverflow);
    Log::get().note("flat discover forward-Present evidence: draws=%u copies=%u; Present1 and upstream mod effects are not observed, so mod order is unqualified",
                    g.forwardedDraws, g.forwardedCopies);
}

void clearFrame() {
    std::memset(g.views, 0, sizeof(g.views));
    std::memset(g.targets, 0, sizeof(g.targets));
    std::memset(g.edges, 0, sizeof(g.edges));
    std::memset(g.outputEdges, 0, sizeof(g.outputEdges));
    std::memset(g.depthClears, 0, sizeof(g.depthClears));
    for (uint32_t i = 0; i < g.targetCount; ++i)
        for (uint32_t slot = 0; slot < 2; ++slot)
            g.exemplars[i][slot].copied = 0;
    g.targetCount = g.edgeCount = g.outputEdgeCount = g.serial = 0;
    g.largeCbCount = g.smallCbCount = 0;
    g.viewOverflow = g.targetOverflow = g.edgeOverflow = 0;
    g.outputEdgeOverflow = g.largeCbOverflow = g.smallCbOverflow = g.depthClearOverflow = 0;
    // Entries are replaced on admission; no payload clearing/copying per draw.
    g.contractCount = g.contractOverflow = g.poolCameraDraws = 0;
    g.handoffContractCount = g.handoffContractOverflow = 0;
    g.menuCopyCount = g.menuCopyOverflow = 0;
    std::memset(g.contractDraws, 0, sizeof(g.contractDraws));
    std::memset(g.contractCamera, 0, sizeof(g.contractCamera));
    g.depthClearCount = 0;
    ++g.epoch;
    g.current = nullptr; g.sampledTarget = nullptr;
    g.forwardingPresent = false;
    std::memset(g.sampledSrv, 0, sizeof(g.sampledSrv));
}

}  // namespace

void flatTemporalStart(ID3D11Device* device) {
    if (!runtimeFlatProfile() || !device) return;
    detail::g_flatTemporalCapturing.store(false, std::memory_order_release);
    detail::g_flatTemporalOwnerThread.store(0, std::memory_order_release);
    detail::g_flatTemporalForeignCalls.store(0, std::memory_order_release);
    // State includes fixed CB exemplar payloads exceeding a default Windows
    // thread stack. Zero the static object directly, never assign State{}.
    std::memset(&g, 0, sizeof(g));
    g.epoch = 1;
    g.device = device;
    g_waitingForPresent.store(true, std::memory_order_release);
    Log::get().note("flat temporal: passive discovery armed, awaiting first owned Present thread; then at most 120 s / 12000 useful frames (all Presents counted separately). Requested=%s; treatment is reported separately by flat runtime",
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
    g.projectionDetailsRemaining = 2;
    g.menuCopyReportsLeft = 2;
    g.projectionManual = true;
    flatComputeArm(device, g.presents);
    flatRuntimeArmProjectionAudit();
    Log::get().note("flat temporal: dump_draws started a fresh bounded desktop discovery window");
}

void flatTemporalStop() {
    // Teardown can run under the loader lock on another thread. Set only
    // atomics; the static collector has no resources to release or free.
    detail::g_flatTemporalCapturing.store(false, std::memory_order_release);
    g_waitingForPresent.store(false, std::memory_order_release);
}

void flatTemporalBeforePresent(IDXGISwapChain* swap, uint64_t frame, UINT flags) {
    if (flatComputeReadbackPending() && !flatComputeCandidate() && detail::g_flatTemporalOwnerThread.load(std::memory_order_acquire) == GetCurrentThreadId())
        flatComputePoll(frame);
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
    uint32_t frameDraws = 0, frameDepth = 0, outputDraws = 0;
    for (uint32_t i = 0; i < g.targetCount; ++i) {
        const Target& t = g.targets[i];
        frameDraws += t.draws; frameDepth += t.depthDraws;
        if (t.color == g.backbuffer) outputDraws += t.draws;
    }
    g.totalDraws += frameDraws; g.totalDepthDraws += frameDepth;
    if (frameDraws) ++g.usefulFrames;
    if (frameDepth) ++g.framesWithDepth;
    if (outputDraws) ++g.framesToOutput;
    const bool deadline = flatCaptureExpired(g.startedMs, now, g.usefulFrames,
                                             kCaptureMs, kMaxUsefulFrames);
    if (flatComputeCandidate()) {
        FlatMonoFrame mono{};
        if (result == S_OK && !(flags & DXGI_PRESENT_TEST)) mono = printMonoInput(frame);
        flatComputeFinish(frame, mono);
        printMenuCopies(frame);
    }
    if (g.presents == 1 || now >= g.nextReportMs || deadline) {
        report(frame, deadline ? "final" : "sample");
        g.nextReportMs = now + kReportMs;
    }
    if (deadline) {
        detail::g_flatTemporalCapturing.store(false, std::memory_order_release);
        Log::get().note("flat temporal: passive discovery complete; flat runtime treatment continues independently and reports its own results");
    }
    clearFrame();
    if (!deadline && result == S_OK && !(flags & DXGI_PRESENT_TEST))
        flatComputeBoundary(frame, g.epoch, g.backW, g.backH);
}

void flatTemporalBind(ID3D11RenderTargetView* rtv, ID3D11DepthStencilView* dsv) {
    if (!flatTemporalCapturing()) return;
    ++g.serial;
    g.current = nullptr;
    g.sampledTarget = nullptr;
    (void)rtv; (void)dsv;  // the binding shadow records explicit nulls too
}

void flatTemporalViewport(UINT count, const D3D11_VIEWPORT* vps) {
    if (!flatTemporalCapturing()) return;
    ++g.serial;
    if (count && vps) {
        g.viewportW = static_cast<uint32_t>(vps[0].Width);
        g.viewportH = static_cast<uint32_t>(vps[0].Height);
        g.viewport[0] = vps[0].TopLeftX; g.viewport[1] = vps[0].TopLeftY;
        g.viewport[2] = vps[0].Width; g.viewport[3] = vps[0].Height;
        g.viewport[4] = vps[0].MinDepth; g.viewport[5] = vps[0].MaxDepth;
        g.viewportCount = count;
    } else {
        g.viewportW = g.viewportH = 0;
        std::memset(g.viewport, 0, sizeof(g.viewport)); g.viewportCount = 0;
    }
}

void flatTemporalConstantBuffers(bool pixelStage, UINT start, UINT count, ID3D11Buffer* const* buffers) {
    if (!flatTemporalCapturing()) return;
    ++g.serial;
    ++g.projectionBindCalls[pixelStage ? 1 : 0];
    auto observe = [&](uint32_t index, uint32_t slot) {
        if (start > slot || slot - start >= count) return;
        flatProjectionBind(g.projectionBindings[index], slot, start, count,
                           buffers ? buffers[slot - start] : nullptr);
    };
    if (pixelStage) observe(2, 2);
    else { observe(0, 0); observe(1, 2); }
}

void flatTemporalClearBindings() {
    if (!flatTemporalCapturing()) return;
    for (auto& binding : g.projectionBindings) {
        binding.resource = nullptr; binding.observed = true;
    }
}

void flatTemporalDraw(ID3D11DeviceContext* ctx, uint32_t count, uint32_t instances) {
    if (!flatTemporalCapturing()) return;
    ++g.serial;
    if (!g.forwardingPresent && flatComputeCandidate()) flatComputeProbeDraw(ctx,g.epoch,g.serial);
    if (!g.forwardingPresent) captureMenuCopy(ctx);
    if (!g.current) {
        // The shadow includes explicit null binds. Never replace a null bind
        // with an older non-null collector value.
        void* rtv = bindingGet(BindSlot::Rtv0);
        void* dsv = bindingGet(BindSlot::Dsv0);
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
    if (t->draws == 1) {
        t->vsCb[0] = bindingGet(BindSlot::VsCb0);
        t->vsCb[1] = bindingGet(BindSlot::VsCb1);
        t->vsHash = bindingShaderHash(BindSlot::Vs);
        t->psHash = bindingShaderHash(BindSlot::Ps);
    }
    const uint32_t targetIndex = static_cast<uint32_t>(t - g.targets);
    const uint64_t vsHash = bindingShaderHash(BindSlot::Vs);
    if (!g.forwardingPresent && flatComputeCandidate())
        flatComputeDraw(ctx, g.epoch, g.serial, vsHash, bindingShaderHash(BindSlot::Ps), t->depth, t->color);
    for (uint32_t slot = 0; slot < 2; ++slot) {
        const BindSlot bindSlot = slot ? BindSlot::VsCb1 : BindSlot::VsCb0;
        void* cbPtr = bindingGet(bindSlot);
        if (Cb* found = findCb(cbPtr)) {
            Cb& cb = *found;
            if (cb.drawEpoch != g.epoch) cb.draws = 0;
            ++cb.draws; cb.drawSeq = g.serial;
            cb.drawEpoch = g.epoch;
            cb.vsHash = vsHash;
            flatFreezeCbExemplar(g.exemplars[targetIndex][slot], cbPtr,
                                 cb.bytes, cb.width, cb.copied,
                                 cb.writeEpoch, cb.writeSeq, g.epoch,
                                 g.serial, vsHash);
        }
    }
    const FlatContractKind kind = flatContractKind(engineVelocityPoolFamilyVs(vsHash),
        t->color, t->depth, t->w, t->h, t->fmt, g.backW, g.backH,
        t->color && t->color == g.backbuffer);
    FlatContractObservation observation{};
    if (kind != kFlatContractNone) {
        ++g.contractDraws[kind];
        observation.kind = kind;
        observation.color = t->color; observation.depth = t->depth;
        observation.rtv = t->rtv; observation.dsv = t->dsv;
        observation.width = t->w; observation.height = t->h; observation.format = t->fmt;
        observation.depthWidth = t->depthW; observation.depthHeight = t->depthH;
        observation.depthFormat = t->depthFmt;
        observation.vs = vsHash; observation.ps = bindingShaderHash(BindSlot::Ps);
        observation.b1 = bindingGet(BindSlot::VsCb1);
        observation.sequence = g.serial; observation.count = count; observation.instances = instances;
        observation.viewportCount = g.viewportCount;
        std::memcpy(observation.viewport, g.viewport, sizeof(g.viewport));
        const Cb* cameraCb = findCb(const_cast<void*>(observation.b1));
        const FlatCameraAvailability availability = flatCameraAvailability(cameraCb != nullptr,
            cameraCb && cameraCb->cameraValid, cameraCb ? cameraCb->writeEpoch : 0,
            cameraCb ? cameraCb->writeSeq : 0, g.epoch, g.serial);
        ++g.contractCamera[availability];
        if (availability == kFlatCameraAvailable) {
            observation.camera = cameraCb->camera;
            observation.cameraHash = cameraCb->cameraHash;
            observation.writeEpoch = cameraCb->writeEpoch;
            observation.writeSeq = cameraCb->writeSeq;
            if (kind == kFlatContractPool) ++g.poolCameraDraws;
        }
        for (uint32_t i = 0; i < kFlatProjectionSlots; ++i) {
            const auto& binding = g.projectionBindings[i];
            const Cb* cb = findCb(const_cast<void*>(binding.resource));
            observation.projection[i] = flatObserveProjection(binding, cb != nullptr,
                cb ? cb->width : 0, cb ? cb->bytes : nullptr, cb ? cb->copied : 0,
                cb ? cb->writeEpoch : 0, cb ? cb->writeSeq : 0, g.epoch, g.serial,
                i, cb ? (i ? cb->b2Hash : cb->b0Hash) : 0);
        }
    }
    if (g.sampledTarget != t->color) {
        g.sampledTarget = t->color;
        std::memset(g.sampledSrv, 0, sizeof(g.sampledSrv));
    }
    for (uint32_t i = 0; i < 4; ++i) {
        void* srv = bindingGet(static_cast<BindSlot>(static_cast<uint32_t>(BindSlot::PsSrv0) + i));
        // Resolve a view only once per frame via the existing cache. No
        // context getters, GPU copies, or full CB copies are added at draws.
        View* v = kind != kFlatContractNone ? viewOf(srv) : nullptr;
        if (kind != kFlatContractNone) {
            observation.srvView[i] = srv;
            observation.srvResource[i] = v ? v->resource : nullptr;
        }
        if (srv == g.sampledSrv[i]) continue;
        g.sampledSrv[i] = srv;
        if (!v) v = viewOf(srv);
        if (v) edge(v->resource, t->color, 'S');
    }
    flatRecordContractReserved(g.contracts, g.contractCount, g.contractOverflow,
        g.handoffContracts, g.handoffContractCount, g.handoffContractOverflow, observation);
}

void flatTemporalClearColor(ID3D11RenderTargetView* rtv) {
    if (!flatTemporalCapturing()) return;
    ++g.serial;
    if (!g.forwardingPresent && flatComputeCandidate()) flatComputeProbeClear(rtv,g.epoch,g.serial,"RTV-clear");
    bool matched = false;
    for (uint32_t i = 0; i < g.targetCount; ++i)
        if (g.targets[i].rtv == rtv) { ++g.targets[i].clearColor; matched = true; }
    (void)matched;
}
void flatTemporalClearUav(ID3D11UnorderedAccessView* uav) {
    if (!flatTemporalCapturing()) return;
    ++g.serial;
    if (!g.forwardingPresent && flatComputeCandidate()) flatComputeProbeClear(uav,g.epoch,g.serial,"UAV-clear");
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
    if (!g.forwardingPresent && flatComputeCandidate()) flatComputeProbeTransfer(dst,src,g.epoch,g.serial,kind);
    if (g.forwardingPresent) ++g.forwardedCopies;
    invalidateCb(dst);  // a destination CB no longer has known CPU-write bytes
    edge(src, dst, kind);
}
void flatTemporalDispatch(ID3D11DeviceContext* ctx, UINT x, UINT y, UINT z, ID3D11Buffer* args, UINT offset) {
    if (!flatTemporalCapturing()) return;
    ++g.serial; ++g.totalDispatches;
    if (!g.forwardingPresent && flatComputeCandidate()) flatComputeDispatch(ctx, g.epoch, g.serial, x, y, z, args, offset);
}
void flatTemporalExecuteList(bool foreign) {
    if (!flatTemporalCapturing()) return;
    ++g.serial; ++g.unknownLists;
    if (flatComputeCandidate()) flatComputeProbeUnknown(g.epoch,g.serial);
    g.proof.unknownDeferredWork = true;
    g.current = nullptr;
    g.viewportW = g.viewportH = 0;
    std::memset(g.viewport, 0, sizeof(g.viewport)); g.viewportCount = 0;
    for (uint32_t i = 0; i < g.largeCbCount; ++i) invalidateCb(g.cbs[i].resource);
    for (uint32_t i = 0; i < g.smallCbCount; ++i)
        invalidateCb(g.cbs[kLargeCbs + i].resource);
    // Unknown deferred work may alter bindings even when the caller asks to
    // restore state. Await an observed setter; never label this as unbound.
    for (auto& binding : g.projectionBindings) binding = FlatProjectionBinding{};
    static uint32_t notes = 0;
    if (notes++ < 4) Log::get().note("flat discover ExecuteCommandList q=%u foreign=%u; deferred writes/camera order unqualified",
                                   g.serial, foreign ? 1 : 0);
}
void flatTemporalMap(ID3D11Resource* res, UINT sub, D3D11_MAP type, void* data) {
    if (!flatTemporalCapturing() || !res || !data || sub || type == D3D11_MAP_READ) return;
    const uint32_t width = bufferWidth(res);
    // A small b0/b1 write can precede the later bind. Keep bounded separate
    // large and small pools so unrelated small writes cannot evict scene-size
    // buffers. Neither size proves camera ownership.
    if (!width || width > 65536) { invalidateCb(res); return; }
    if (Cb* cb = cbOf(res, width)) {
        cb->copied = 0; cb->writeEpoch = 0;
        cb->cameraValid = false; cb->cameraHash = 0;
        cb->viewportValid = cb->targetSizeValid = false;
        cb->b0Hash = cb->b2Hash = 0;
        cb->mapped = data;
    }
}
void flatTemporalUnmap(ID3D11Resource* res) {
    if (!flatTemporalCapturing() || !res) return;
    if (Cb* found = findCb(res)) {
        Cb& cb = *found;
        if (!cb.mapped) return;
        cb.copied = std::min(cb.width, kCbBytes);
        std::memcpy(cb.bytes, cb.mapped, cb.copied);
        projectionHashes(cb);
        cb.cameraValid = flatCaptureCameraRows(cb.camera, cb.mapped, cb.width);
        cb.viewportValid = flatComputeCopyRow(cb.viewportRow, cb.mapped, cb.width, 281);
        cb.targetSizeValid = flatComputeCopyRow(cb.targetSizeRow, cb.mapped, cb.width, 332);
        cb.cameraHash = cb.cameraValid ? flatCameraHash(cb.camera) : 0;
        cb.mapped = nullptr; cb.writeSeq = ++g.serial; ++cb.writes;
        cb.writeEpoch = g.epoch;
    }
}
void flatTemporalUpdate(ID3D11Resource* dst, const void* data, const D3D11_BOX* box) {
    if (!flatTemporalCapturing() || !dst) return;
    if (!g.forwardingPresent && flatComputeCandidate()) {
        ++g.serial;
        flatComputeProbeUpdate(dst,g.epoch,g.serial,data && !box);
    }
    if (!data || box) { invalidateCb(dst); return; }
    const uint32_t width = bufferWidth(dst);
    if (!width || width > 65536) { invalidateCb(dst); return; }
    if (Cb* cb = cbOf(dst, width)) {
        cb->copied = std::min(width, kCbBytes);
        std::memcpy(cb->bytes, data, cb->copied);
        projectionHashes(*cb);
        cb->cameraValid = flatCaptureCameraRows(cb->camera, data, width);
        cb->viewportValid = flatComputeCopyRow(cb->viewportRow, data, width, 281);
        cb->targetSizeValid = flatComputeCopyRow(cb->targetSizeRow, data, width, 332);
        cb->cameraHash = cb->cameraValid ? flatCameraHash(cb->camera) : 0;
        cb->mapped = nullptr;
        cb->writeSeq = ++g.serial; ++cb->writes;
        cb->writeEpoch = g.epoch;
    }
}

bool flatTemporalCopyConstants(ID3D11Buffer* buffer, uint32_t offset, uint32_t bytes,
                              void* out, uint32_t& width, uint64_t& epoch, uint32_t& sequence) {
    const Cb* cb = findCb(buffer);
    if (!cb || !out || cb->writeEpoch != g.epoch || cb->writeSeq > g.serial) return false;
    width = cb->width; epoch = cb->writeEpoch; sequence = cb->writeSeq;
    if (offset == 281u*16u && bytes == 16 && cb->viewportValid) {
        std::memcpy(out, cb->viewportRow, 16); return true;
    }
    if (offset == 332u*16u && bytes == 16 && cb->targetSizeValid) {
        std::memcpy(out, cb->targetSizeRow, 16); return true;
    }
    if (offset > cb->copied || bytes > cb->copied - offset) return false;
    std::memcpy(out, cb->bytes + offset, bytes); return true;
}
}  // namespace edvr
