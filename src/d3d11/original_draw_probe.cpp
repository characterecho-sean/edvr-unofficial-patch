#include "original_draw_probe.h"
#include "gpu_timing.h"
#include "../common/log.h"

#include <algorithm>
#include <array>
#include <new>
#include <wrl/client.h>

namespace edvr {
namespace {
using Microsoft::WRL::ComPtr;

constexpr uint32_t kSlotCount = 64;
constexpr uint32_t kBucketCount = 128;
constexpr uint32_t kTargetsPerFrame = 3;
constexpr uint64_t kCollectFrames = 600;
constexpr uint64_t kDrainFrames = 120;
constexpr uint64_t kPollDelayFrames = 4;
constexpr uint32_t kPollBudget = 16;
constexpr uint32_t kCalibrationLimit = 10;

enum class SlotPhase : uint8_t { Idle, Open, Pending };
enum class ResultState : uint8_t { Pending, Ready, Invalid };

struct DetailKey {
    uint64_t vs = 0, ps = 0;
    uint32_t verdict = 0, count = 0, instances = 0;
    uint32_t width = 0, height = 0, sampleMask = 0, stencilRef = 0;
    DXGI_FORMAT rtv = DXGI_FORMAT_UNKNOWN, dsv = DXGI_FORMAT_UNKNOWN;
    D3D11_PRIMITIVE_TOPOLOGY topology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    uint16_t flags = 0;
    uint8_t kind = 0, pass = 0, eye = 0, writeMask = 0, depthFunc = 0, cull = 0;
    bool operator==(const DetailKey& o) const noexcept {
        return vs == o.vs && ps == o.ps && verdict == o.verdict &&
            count == o.count && instances == o.instances && width == o.width && height == o.height &&
            sampleMask == o.sampleMask && stencilRef == o.stencilRef && rtv == o.rtv && dsv == o.dsv &&
            topology == o.topology && flags == o.flags && kind == o.kind &&
            pass == o.pass && eye == o.eye && writeMask == o.writeMask &&
            depthFunc == o.depthFunc && cull == o.cull;
    }
};

struct Totals {
    uint64_t samples = 0, zero = 0, nonzero = 0;
    uint64_t zeroTime = 0, nonzeroTime = 0;
    uint64_t statsValid = 0, iaVertices = 0, vsInvocations = 0;
    uint64_t cPrimitives = 0, psInvocations = 0;
    double zeroMs = 0, nonzeroMs = 0;
};

struct DetailBucket {
    bool used = false;
    DetailKey key{};
    Totals totals{};
};

struct Slot {
    SlotPhase phase = SlotPhase::Idle;
    ComPtr<ID3D11Query> occlusion;
    ComPtr<ID3D11Query> pipeline;
    GpuTimer timer;
    OriginalDrawProbeInput input{};
    DetailKey key{};
    D3D11_QUERY_DATA_PIPELINE_STATISTICS stats{};
    uint64_t samples = 0, submittedFrame = 0;
    uint32_t generation = 0;
    ResultState occState = ResultState::Pending;
    ResultState statsState = ResultState::Pending;
    ResultState timerState = ResultState::Pending;
    double gpuMs = 0;
    bool issued = false, calibration = false, timerUnavailable = false;
    bool timerNeedsReset = false;
};

struct Controller {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    OriginalDrawProbeQueryOps ops{};
    DWORD ownerThread = 0;
    bool enabled = false, collecting = false, draining = false;
    bool firstOriginal = false, firstSubmit = false, commandListSeen = false;
    uint64_t frameSerial = 0, sourceFrame = 0, window = 0;
    uint64_t collectFrames = 0, drainFrames = 0;
    uint64_t ordinal = 0, previousTotal = 0, randomState = 0x9e3779b97f4a7c15ull;
    std::array<uint64_t, kTargetsPerFrame> targets{};
    std::array<bool, kTargetsPerFrame> targetSeen{};
    uint32_t targetCount = 0, pollCursor = 0;
    OriginalDrawProbeSnapshot snapshot{};
    std::array<Slot, kSlotCount> slots{};
    std::array<DetailBucket, kBucketCount> buckets{};
    Totals aggregates[static_cast<unsigned>(OriginalDrawPass::Count)][3][2]{};
    uint64_t iaZero = 0, clipZero = 0, psZero = 0;
    uint64_t calibrationReady = 0;
    double calibrationMs = 0;

    bool owns(ID3D11DeviceContext* ctx) const noexcept {
        return ctx && ctx == context.Get() && (!ownerThread || ownerThread == GetCurrentThreadId());
    }
    bool adopt(ID3D11DeviceContext* ctx) noexcept {
        if (!owns(ctx)) return false;
        if (ownerThread) return true;
        if (!gpuTimingOwns(ctx)) return false;
        ownerThread = GetCurrentThreadId();
        return true;
    }
    uint64_t random() noexcept {
        randomState ^= randomState >> 12;
        randomState ^= randomState << 25;
        randomState ^= randomState >> 27;
        return randomState * 2685821657736338717ull;
    }
    uint64_t bounded(uint64_t bound) noexcept {
        if (!bound) return 0;
        const uint64_t threshold = uint64_t(0) - bound;
        const uint64_t reject = threshold % bound;
        for (;;) {
            const uint64_t value = random();
            if (value >= reject) return value % bound;
        }
    }
};

Controller* g = nullptr;
thread_local bool g_internalQuery = false;
struct InternalQuery {
    bool previous = g_internalQuery;
    InternalQuery() noexcept { g_internalQuery = true; }
    ~InternalQuery() { g_internalQuery = previous; }
};

uint8_t eyeIndex(int8_t eye) noexcept {
    return eye == 0 ? 0 : eye == 1 ? 1 : 2;
}

template<class T> void release(T*& value) noexcept {
    if (value) { value->Release(); value = nullptr; }
}

UINT rtvMip(const D3D11_RENDER_TARGET_VIEW_DESC& desc) noexcept {
    switch (desc.ViewDimension) {
    case D3D11_RTV_DIMENSION_TEXTURE1D: return desc.Texture1D.MipSlice;
    case D3D11_RTV_DIMENSION_TEXTURE1DARRAY: return desc.Texture1DArray.MipSlice;
    case D3D11_RTV_DIMENSION_TEXTURE2D: return desc.Texture2D.MipSlice;
    case D3D11_RTV_DIMENSION_TEXTURE2DARRAY: return desc.Texture2DArray.MipSlice;
    case D3D11_RTV_DIMENSION_TEXTURE3D: return desc.Texture3D.MipSlice;
    default: return 0;
    }
}

UINT dsvMip(const D3D11_DEPTH_STENCIL_VIEW_DESC& desc) noexcept {
    switch (desc.ViewDimension) {
    case D3D11_DSV_DIMENSION_TEXTURE1D: return desc.Texture1D.MipSlice;
    case D3D11_DSV_DIMENSION_TEXTURE1DARRAY: return desc.Texture1DArray.MipSlice;
    case D3D11_DSV_DIMENSION_TEXTURE2D: return desc.Texture2D.MipSlice;
    case D3D11_DSV_DIMENSION_TEXTURE2DARRAY: return desc.Texture2DArray.MipSlice;
    default: return 0;
    }
}

void textureDimensions(ID3D11View* view, UINT mip, uint32_t& width, uint32_t& height) noexcept {
    if (!view) return;
    ComPtr<ID3D11Resource> resource;
    view->GetResource(&resource);
    ComPtr<ID3D11Texture2D> texture;
    if (resource && SUCCEEDED(resource.As(&texture))) {
        D3D11_TEXTURE2D_DESC desc{};
        texture->GetDesc(&desc);
        width = std::max<UINT>(1, desc.Width >> mip);
        height = std::max<UINT>(1, desc.Height >> mip);
    }
}

DetailKey inspectPipeline(ID3D11DeviceContext* ctx,
                          const OriginalDrawProbeInput& input,
                          bool& noRaster, bool& noWrite) noexcept {
    DetailKey key{};
    key.vs = input.originalVsHash;
    key.ps = input.originalPsHash;
    key.verdict = input.verdict;
    key.count = input.count;
    key.instances = input.instances;
    key.kind = static_cast<uint8_t>(input.kind);
    key.pass = static_cast<uint8_t>(input.pass);
    key.eye = eyeIndex(input.eye);
    if (input.modified) key.flags |= 1u << 0;
    if (input.eyeSized) key.flags |= 1u << 1;
    if (input.sourceSized) key.flags |= 1u << 2;

    ctx->IAGetPrimitiveTopology(&key.topology);
    noRaster = key.topology == D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED ||
        input.count == 0 || input.instances == 0;

    UINT viewportCount = 0;
    ctx->RSGetViewports(&viewportCount, nullptr);
    noRaster |= viewportCount == 0;

    ID3D11RasterizerState* raster = nullptr;
    ctx->RSGetState(&raster);
    if (raster) {
        D3D11_RASTERIZER_DESC desc{};
        raster->GetDesc(&desc);
        if (desc.ScissorEnable) key.flags |= 1u << 3;
        key.cull = static_cast<uint8_t>(desc.CullMode);
        if (desc.CullMode == D3D11_CULL_FRONT) key.flags |= 1u << 9;
        if (desc.CullMode == D3D11_CULL_BACK) key.flags |= 1u << 10;
    }
    release(raster);

    std::array<ID3D11RenderTargetView*, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT> rtvs{};
    ID3D11DepthStencilView* dsv = nullptr;
    ctx->OMGetRenderTargets(static_cast<UINT>(rtvs.size()), rtvs.data(), &dsv);
    bool colorWrite = false;
    for (auto* rtv : rtvs) {
        if (!rtv) continue;
        D3D11_RENDER_TARGET_VIEW_DESC desc{};
        rtv->GetDesc(&desc);
        if (key.rtv == DXGI_FORMAT_UNKNOWN) key.rtv = desc.Format;
        if (!key.width) textureDimensions(rtv, rtvMip(desc), key.width, key.height);
    }
    if (dsv) {
        D3D11_DEPTH_STENCIL_VIEW_DESC desc{};
        dsv->GetDesc(&desc);
        key.dsv = desc.Format;
        if (!key.width) textureDimensions(dsv, dsvMip(desc), key.width, key.height);
    }

    ID3D11BlendState* blend = nullptr;
    FLOAT factors[4]{};
    ctx->OMGetBlendState(&blend, factors, &key.sampleMask);
    if (blend) {
        D3D11_BLEND_DESC desc{};
        blend->GetDesc(&desc);
        for (UINT i = 0; i < rtvs.size(); ++i) {
            const UINT j = desc.IndependentBlendEnable ? i : 0;
            if (rtvs[i]) {
                key.writeMask |= desc.RenderTarget[j].RenderTargetWriteMask;
                colorWrite |= desc.RenderTarget[j].RenderTargetWriteMask != 0;
            }
        }
    } else {
        for (auto* rtv : rtvs) colorWrite |= rtv != nullptr;
        key.writeMask = colorWrite ? D3D11_COLOR_WRITE_ENABLE_ALL : 0;
    }
    release(blend);

    ID3D11DepthStencilState* depth = nullptr;
    ctx->OMGetDepthStencilState(&depth, &key.stencilRef);
    bool depthWrite = dsv != nullptr;
    if (depth) {
        D3D11_DEPTH_STENCIL_DESC desc{};
        depth->GetDesc(&desc);
        key.depthFunc = static_cast<uint8_t>(desc.DepthFunc);
        depthWrite = dsv && ((desc.DepthEnable && desc.DepthWriteMask != D3D11_DEPTH_WRITE_MASK_ZERO) ||
            (desc.StencilEnable && desc.StencilWriteMask));
        if (desc.DepthEnable) key.flags |= 1u << 4;
        if (desc.DepthWriteMask != D3D11_DEPTH_WRITE_MASK_ZERO) key.flags |= 1u << 5;
        if (desc.StencilEnable) key.flags |= 1u << 6;
    } else if (dsv) {
        key.flags |= (1u << 4) | (1u << 5);
    }
    release(depth);

    ID3D11UnorderedAccessView* uavs[D3D11_PS_CS_UAV_REGISTER_COUNT]{};
    ctx->OMGetRenderTargetsAndUnorderedAccessViews(0, nullptr, nullptr, 0,
        D3D11_PS_CS_UAV_REGISTER_COUNT, uavs);
    bool uavWrite = false;
    for (auto*& uav : uavs) { uavWrite |= uav != nullptr; release(uav); }
    if (uavWrite) key.flags |= 1u << 7;

    ID3D11Buffer* so[D3D11_SO_BUFFER_SLOT_COUNT]{};
    ctx->SOGetTargets(D3D11_SO_BUFFER_SLOT_COUNT, so);
    bool soWrite = false;
    for (auto*& buffer : so) { soWrite |= buffer != nullptr; release(buffer); }
    if (soWrite) key.flags |= 1u << 8;

    noWrite = !colorWrite && !depthWrite && !uavWrite && !soWrite;
    for (auto*& rtv : rtvs) release(rtv);
    release(dsv);
    return key;
}

DetailBucket* bucketFor(Controller& c, const DetailKey& key) noexcept {
    DetailBucket* free = nullptr;
    for (auto& bucket : c.buckets) {
        if (bucket.used && bucket.key == key) return &bucket;
        if (!bucket.used && !free) free = &bucket;
    }
    if (!free) { ++c.snapshot.bucketOverflow; return nullptr; }
    free->used = true;
    free->key = key;
    return free;
}

void addResult(Totals& totals, bool zero, bool timed, double ms) noexcept {
    ++totals.samples;
    if (zero) {
        ++totals.zero;
        if (timed) { ++totals.zeroTime; totals.zeroMs += ms; }
    } else {
        ++totals.nonzero;
        if (timed) { ++totals.nonzeroTime; totals.nonzeroMs += ms; }
    }
}

void addStats(Totals& totals, const D3D11_QUERY_DATA_PIPELINE_STATISTICS& stats) noexcept {
    ++totals.statsValid;
    totals.iaVertices += stats.IAVertices;
    totals.vsInvocations += stats.VSInvocations;
    totals.cPrimitives += stats.CPrimitives;
    totals.psInvocations += stats.PSInvocations;
}

uint64_t pendingCount(const Controller& c) noexcept {
    uint64_t count = 0;
    for (const auto& slot : c.slots) if (slot.phase != SlotPhase::Idle) ++count;
    return count;
}

void retire(Controller& c, Slot& slot) noexcept {
    const bool validVisibility = slot.issued && slot.occState == ResultState::Ready;
    if (slot.calibration) {
        if (slot.timerState == ResultState::Ready) {
            ++c.calibrationReady;
            c.calibrationMs += slot.gpuMs;
        }
    } else if (validVisibility) {
        const bool zero = slot.samples == 0;
        ++c.snapshot.ready;
        if (zero) ++c.snapshot.zeroSamples; else ++c.snapshot.nonzeroSamples;
        const bool timed = slot.timerState == ResultState::Ready;
        auto& aggregate = c.aggregates[static_cast<unsigned>(slot.input.pass)]
                                      [eyeIndex(slot.input.eye)][slot.input.modified ? 1 : 0];
        addResult(aggregate, zero, timed, slot.gpuMs);
        auto* detail = bucketFor(c, slot.key);
        if (detail) addResult(detail->totals, zero, timed, slot.gpuMs);
        if (slot.statsState == ResultState::Ready) {
            ++c.snapshot.pipelineReady;
            addStats(aggregate, slot.stats);
            if (detail) addStats(detail->totals, slot.stats);
            if (!slot.stats.IAVertices && !slot.stats.IAPrimitives && !slot.stats.VSInvocations) ++c.iaZero;
            else if (!slot.stats.CPrimitives) ++c.clipZero;
            else if (!slot.stats.PSInvocations) ++c.psZero;
        }
        if (slot.timerState == ResultState::Ready) ++c.snapshot.timedReady;
        else if (slot.timerUnavailable) ++c.snapshot.timingUnavailable;
    }
    if (slot.occState == ResultState::Invalid || !slot.issued) ++c.snapshot.invalid;
    if (slot.statsState == ResultState::Invalid) ++c.snapshot.pipelineInvalid;
    if (slot.timerState == ResultState::Invalid && !slot.timerUnavailable)
        ++c.snapshot.timingInvalid;
    if (slot.timerNeedsReset) {
        InternalQuery internal;
        slot.timer.reset(c.context.Get());
    }
    slot.timerNeedsReset = false;
    slot.phase = SlotPhase::Idle;
}

void poll(Controller& c) noexcept {
    uint32_t visited = 0;
    uint32_t nextCursor = c.pollCursor;
    for (uint32_t step = 0; step < kSlotCount && visited < kPollBudget; ++step) {
        const uint32_t index = (c.pollCursor + step) % kSlotCount;
        auto& slot = c.slots[index];
        if (slot.phase != SlotPhase::Pending || c.frameSerial < slot.submittedFrame + kPollDelayFrames) continue;
        ++visited;
        nextCursor = (index + 1) % kSlotCount;
        if (slot.occState == ResultState::Pending) {
            const HRESULT hr = c.ops.getData(c.context.Get(), slot.occlusion.Get(), &slot.samples,
                sizeof(slot.samples), D3D11_ASYNC_GETDATA_DONOTFLUSH);
            if (hr == S_OK) slot.occState = ResultState::Ready;
            else if (hr != S_FALSE) slot.occState = ResultState::Invalid;
        }
        if (slot.statsState == ResultState::Pending) {
            const HRESULT hr = c.ops.getData(c.context.Get(), slot.pipeline.Get(), &slot.stats,
                sizeof(slot.stats), D3D11_ASYNC_GETDATA_DONOTFLUSH);
            if (hr == S_OK) slot.statsState = ResultState::Ready;
            else if (hr != S_FALSE) slot.statsState = ResultState::Invalid;
        }
        if (slot.timerState == ResultState::Pending) {
            InternalQuery internal;
            const auto status = slot.timer.poll(c.context.Get(), slot.gpuMs);
            if (status == GpuTimerPoll::Ready) slot.timerState = ResultState::Ready;
            else if (status == GpuTimerPoll::Invalid) slot.timerState = ResultState::Invalid;
        }
        if (slot.occState != ResultState::Pending && slot.statsState != ResultState::Pending &&
            slot.timerState != ResultState::Pending) retire(c, slot);
    }
    c.pollCursor = nextCursor;
    c.snapshot.pending = pendingCount(c);
}

void report(Controller& c, bool timedOut) noexcept {
    c.snapshot.pending = pendingCount(c);
    const char* state = c.snapshot.submitted ? (c.snapshot.ready ? "ready" : "no-data") : "no-data";
    Log::get().note(
        "Original draw diagnostic: window %llu %s%s; frames=%llu population=%llu planned=%llu "
        "missed=%llu submitted=%llu ready=%llu zero-passed-samples=%llu nonzero-passed-samples=%llu "
        "pending=%llu invalid=%llu. Occlusion values are passed samples, not visible pixels or material contribution.",
        static_cast<unsigned long long>(c.window), state, timedOut ? " (drain timeout)" : "",
        static_cast<unsigned long long>(c.snapshot.frames),
        static_cast<unsigned long long>(c.snapshot.originalCalls),
        static_cast<unsigned long long>(c.snapshot.planned),
        static_cast<unsigned long long>(c.snapshot.targetMisses),
        static_cast<unsigned long long>(c.snapshot.submitted),
        static_cast<unsigned long long>(c.snapshot.ready),
        static_cast<unsigned long long>(c.snapshot.zeroSamples),
        static_cast<unsigned long long>(c.snapshot.nonzeroSamples),
        static_cast<unsigned long long>(c.snapshot.pending),
        static_cast<unsigned long long>(c.snapshot.invalid));
    Log::get().note(
        "Original draw diagnostic contexts/guards: predicated=%llu counting=%llu external-disjoint-context=%llu query-overflow=%llu "
        "no-raster=%llu no-write=%llu command-list=%llu unsupported=%llu ring-full=%llu not-issued=%llu "
        "pipeline-ready=%llu pipeline-invalid=%llu timed-ready=%llu timing-unavailable=%llu "
        "timing-invalid=%llu expired=%llu bucket-overflow=%llu.",
        c.snapshot.predicated, c.snapshot.gameCounting, c.snapshot.gameDisjoint,
        c.snapshot.queryOverflow, c.snapshot.noRaster, c.snapshot.noWrite,
        c.snapshot.commandLists, c.snapshot.unsupported, c.snapshot.ringFull,
        c.snapshot.notIssued, c.snapshot.pipelineReady, c.snapshot.pipelineInvalid,
        c.snapshot.timedReady, c.snapshot.timingUnavailable, c.snapshot.timingInvalid,
        c.snapshot.expired, c.snapshot.bucketOverflow);
    Log::get().note(
        "Original draw diagnostic pipeline: IA/VS-empty=%llu zero-CPrimitives=%llu "
        "zero-PS-invocations=%llu; empty-bracket-calibration=%llu/%llu mean=%.6f ms, query-inclusive and not subtractable.",
        c.iaZero, c.clipZero, c.psZero, c.calibrationReady, c.snapshot.calibrations,
        c.calibrationReady ? c.calibrationMs / double(c.calibrationReady) : 0.0);
    for (unsigned pass = 0; pass < static_cast<unsigned>(OriginalDrawPass::Count); ++pass)
        for (unsigned eye = 0; eye < 3; ++eye) for (unsigned modified = 0; modified < 2; ++modified) {
            const auto& total = c.aggregates[pass][eye][modified];
            if (!total.samples) continue;
            Log::get().note(
                "Original draw diagnostic aggregate: pass=%u eye=%u modified=%u samples=%llu zero=%llu "
                "nonzero=%llu zero-gpu=%.6f/%llu ms nonzero-gpu=%.6f/%llu ms "
                "stats=%llu IAvertices=%llu VSinvocations=%llu CPrimitives=%llu PSinvocations=%llu.",
                pass, eye, modified, total.samples, total.zero, total.nonzero,
                total.zeroMs, total.zeroTime, total.nonzeroMs, total.nonzeroTime,
                total.statsValid, total.iaVertices, total.vsInvocations,
                total.cPrimitives, total.psInvocations);
        }
    std::array<const DetailBucket*, kBucketCount> ranked{};
    unsigned bucketTotal = 0;
    for (const auto& bucket : c.buckets) if (bucket.used) ranked[bucketTotal++] = &bucket;
    std::sort(ranked.begin(), ranked.begin() + bucketTotal,
        [](const DetailBucket* a, const DetailBucket* b) {
            const double aTime = a->totals.zeroMs + a->totals.nonzeroMs;
            const double bTime = b->totals.zeroMs + b->totals.nonzeroMs;
            if (aTime != bTime) return aTime > bTime;
            return a->totals.samples > b->totals.samples;
        });
    const unsigned shown = std::min<unsigned>(16, bucketTotal);
    Log::get().note("Original draw diagnostic detail coverage: ranked-by-query-inclusive-GPU shown=%u total=%u omitted=%u.",
                    shown, bucketTotal, bucketTotal - shown);
    for (unsigned emitted = 0; emitted < shown; ++emitted) {
        const auto& bucket = *ranked[emitted];
        Log::get().note(
            "Original draw diagnostic bucket %u: vs=%016llX ps=%016llX verdict=%u pass=%u eye=%u "
            "kind=%u count=%u instances=%u target=%ux%u "
            "modified=%u topology=%u rtv=%u dsv=%u depth-func=%u stencil-ref=%u cull=%u sample-mask=0x%X "
            "flags=0x%X write-mask=0x%X samples=%llu zero=%llu nonzero=%llu "
            "zero-gpu=%.6f/%llu ms nonzero-gpu=%.6f/%llu ms stats=%llu IAvertices=%llu "
            "VSinvocations=%llu CPrimitives=%llu PSinvocations=%llu.",
            emitted + 1, static_cast<unsigned long long>(bucket.key.vs),
            static_cast<unsigned long long>(bucket.key.ps), bucket.key.verdict,
            bucket.key.pass, bucket.key.eye, bucket.key.kind, bucket.key.count,
            bucket.key.instances, bucket.key.width, bucket.key.height, (bucket.key.flags & 1) != 0,
            unsigned(bucket.key.topology), unsigned(bucket.key.rtv), unsigned(bucket.key.dsv),
            bucket.key.depthFunc, bucket.key.stencilRef, bucket.key.cull, bucket.key.sampleMask,
            bucket.key.flags, bucket.key.writeMask,
            bucket.totals.samples, bucket.totals.zero, bucket.totals.nonzero,
            bucket.totals.zeroMs, bucket.totals.zeroTime,
            bucket.totals.nonzeroMs, bucket.totals.nonzeroTime, bucket.totals.statsValid,
            bucket.totals.iaVertices, bucket.totals.vsInvocations,
            bucket.totals.cPrimitives, bucket.totals.psInvocations);
    }
}

void resetWindow(Controller& c) noexcept {
    c.snapshot = {};
    c.snapshot.enabled = c.enabled;
    c.snapshot.bound = true;
    c.snapshot.collecting = c.enabled;
    c.snapshot.window = ++c.window;
    c.collecting = c.enabled;
    c.draining = false;
    c.collectFrames = c.drainFrames = 0;
    c.ordinal = c.previousTotal = 0;
    c.targetCount = 0;
    c.targetSeen = {};
    c.buckets = {};
    for (auto& pass : c.aggregates) for (auto& eye : pass)
        for (auto& modified : eye) modified = {};
    c.iaZero = c.clipZero = c.psZero = 0;
    c.calibrationReady = 0;
    c.calibrationMs = 0;
}

void plan(Controller& c) noexcept {
    c.targetCount = static_cast<uint32_t>(std::min<uint64_t>(kTargetsPerFrame, c.previousTotal));
    c.targetSeen = {};
    for (uint32_t i = 0; i < c.targetCount; ++i) {
        uint64_t candidate = 0;
        bool unique = false;
        while (!unique) {
            candidate = c.bounded(c.previousTotal);
            unique = true;
            for (uint32_t j = 0; j < i; ++j) unique &= c.targets[j] != candidate;
        }
        c.targets[i] = candidate;
    }
    std::sort(c.targets.begin(), c.targets.begin() + c.targetCount);
    c.snapshot.planned += c.targetCount;
}

Slot* idleSlot(Controller& c) noexcept {
    for (auto& slot : c.slots) if (slot.phase == SlotPhase::Idle) return &slot;
    return nullptr;
}

bool startQueries(Controller& c, Slot& slot, const OriginalDrawProbeInput& input,
                  const DetailKey& key, bool calibration) noexcept {
    InternalQuery internal;
    const bool timed = slot.timer.beginBorrowedFrame(c.device.Get(), c.context.Get());
    c.ops.begin(c.context.Get(), slot.occlusion.Get());
    c.ops.begin(c.context.Get(), slot.pipeline.Get());
    slot.phase = SlotPhase::Open;
    slot.input = input;
    slot.key = key;
    slot.stats = {};
    slot.samples = 0;
    slot.occState = slot.statsState = ResultState::Pending;
    slot.timerState = timed ? ResultState::Pending : ResultState::Invalid;
    slot.issued = false;
    slot.calibration = calibration;
    slot.timerUnavailable = !timed;
    slot.timerNeedsReset = false;
    return true;
}

void finishQueries(Controller& c, Slot& slot, bool issued) noexcept {
    c.ops.end(c.context.Get(), slot.pipeline.Get());
    c.ops.end(c.context.Get(), slot.occlusion.Get());
    if (!slot.timerUnavailable) {
        InternalQuery internal;
        if (!slot.timer.end(c.context.Get())) {
            slot.timerState = ResultState::Invalid;
            slot.timerNeedsReset = true;
        }
    }
    slot.issued = issued;
    slot.submittedFrame = c.frameSerial;
    slot.phase = SlotPhase::Pending;
}

void maybeCalibrate(Controller& c) noexcept {
    if (c.snapshot.calibrations >= kCalibrationLimit || c.collectFrames % 60 != 0) return;
    Slot* slot = idleSlot(c);
    if (!slot) return;
    OriginalDrawProbeInput input{};
    DetailKey key{};
    if (!startQueries(c, *slot, input, key, true)) return;
    finishQueries(c, *slot, true);
    ++c.snapshot.calibrations;
}

void discardPending(Controller& c) noexcept {
    for (auto& slot : c.slots) if (slot.phase != SlotPhase::Idle) {
        InternalQuery internal;
        slot.timer.reset(c.context.Get());
        slot.timerNeedsReset = false;
        slot.phase = SlotPhase::Idle;
    }
}
} // namespace

bool originalDrawProbeBind(ID3D11Device* device, ID3D11DeviceContext* context,
                           const OriginalDrawProbeQueryOps& ops) noexcept {
    if (!device || !context || !ops.begin || !ops.end || !ops.getData) return false;
    if (g) return g->device.Get() == device && g->owns(context);
    D3D11_DEVICE_CONTEXT_TYPE type = context->GetType();
    if (type != D3D11_DEVICE_CONTEXT_IMMEDIATE) return false;
    auto* candidate = new (std::nothrow) Controller;
    if (!candidate) return false;
    candidate->device = device;
    candidate->context = context;
    candidate->ops = ops;
    // Binding can occur on the installer thread. The first real frame adopts
    // only the already-established shared GPU timing owner thread.
    candidate->ownerThread = 0;
    D3D11_QUERY_DESC desc{D3D11_QUERY_OCCLUSION, 0};
    for (auto& slot : candidate->slots) {
        if (FAILED(device->CreateQuery(&desc, &slot.occlusion))) { delete candidate; return false; }
        desc.Query = D3D11_QUERY_PIPELINE_STATISTICS;
        if (FAILED(device->CreateQuery(&desc, &slot.pipeline))) { delete candidate; return false; }
        desc.Query = D3D11_QUERY_OCCLUSION;
    }
    g = candidate;
    return true;
}

void originalDrawProbeConfigure(bool enabled) noexcept {
    if (!g || !g->owns(g->context.Get()) || g->enabled == enabled) return;
    g->enabled = enabled;
    if (enabled) {
        if (pendingCount(*g)) discardPending(*g);
        resetWindow(*g);
        Log::get().note("Original draw diagnostic: enabled; 600-frame repeating windows, three prior-frame-uniform targets, 64 query slots, no draw suppression.");
    } else {
        g->collecting = false;
        g->draining = pendingCount(*g) != 0;
        g->snapshot.enabled = false;
        g->snapshot.collecting = false;
        g->snapshot.draining = g->draining;
        Log::get().note("Original draw diagnostic: disabled; outstanding results drain without waits or Flush.");
    }
}

bool originalDrawProbeSelect(ID3D11DeviceContext* context) noexcept {
    if (!g || !g->enabled || !g->collecting || !g->adopt(context)) return false;
    const uint64_t ordinal = g->ordinal++;
    ++g->snapshot.originalCalls;
    if (!g->firstOriginal) {
        g->firstOriginal = true;
        Log::get().note("Original draw diagnostic: first original native draw observed.");
    }
    for (uint32_t i = 0; i < g->targetCount; ++i) {
        if (g->targets[i] == ordinal) { g->targetSeen[i] = true; return true; }
    }
    return false;
}

OriginalDrawProbeTicket originalDrawProbeBegin(ID3D11DeviceContext* context,
                                               const OriginalDrawProbeInput& input) noexcept {
    OriginalDrawProbeTicket ticket{};
    if (!g || !g->enabled || !g->collecting || !g->owns(context) ||
        !gpuTimingOwns(context)) return ticket;
    if (input.pass >= OriginalDrawPass::Count || input.kind >= OriginalDrawKind::Count) {
        ++g->snapshot.unsupported;
        return ticket;
    }
    if (input.gameQueryOverflow) { ++g->snapshot.queryOverflow; return ticket; }
    if (input.gameCountingActive) { ++g->snapshot.gameCounting; return ticket; }
    if (input.gameDisjointActive) ++g->snapshot.gameDisjoint;
    if (g->commandListSeen) { ++g->snapshot.unsupported; return ticket; }
    ID3D11Predicate* predicate = nullptr;
    BOOL predicateValue = FALSE;
    context->GetPredication(&predicate, &predicateValue);
    const bool predicated = predicate != nullptr;
    release(predicate);
    if (predicated) { ++g->snapshot.predicated; return ticket; }

    bool noRaster = false, noWrite = false;
    const DetailKey key = inspectPipeline(context, input, noRaster, noWrite);
    if (noRaster) { ++g->snapshot.noRaster; return ticket; }
    if (noWrite) { ++g->snapshot.noWrite; return ticket; }
    Slot* slot = idleSlot(*g);
    if (!slot) { ++g->snapshot.ringFull; return ticket; }
    if (!startQueries(*g, *slot, input, key, false)) {
        ++g->snapshot.unsupported;
        return ticket;
    }
    ticket.slot = static_cast<uint32_t>(slot - g->slots.data());
    ticket.generation = ++slot->generation;
    ++g->snapshot.submitted;
    return ticket;
}

void originalDrawProbeEnd(ID3D11DeviceContext* context, OriginalDrawProbeTicket ticket,
                          bool originalIssued) noexcept {
    if (!g || !g->owns(context) || !ticket || ticket.slot >= kSlotCount) return;
    auto& slot = g->slots[ticket.slot];
    if (slot.phase != SlotPhase::Open || slot.generation != ticket.generation) return;
    if (!originalIssued) ++g->snapshot.notIssued;
    finishQueries(*g, slot, originalIssued);
    if (!g->firstSubmit) {
        g->firstSubmit = true;
        Log::get().note("Original draw diagnostic: first sparse raw-query bracket submitted.");
    }
    maybeCalibrate(*g);
}

void originalDrawProbeFrame(ID3D11DeviceContext* context,
                            const OriginalDrawProbeFrameInfo& info) noexcept {
    if (!g || !g->adopt(context) || !gpuTimingOwns(context)) return;
    ++g->frameSerial;
    g->sourceFrame = info.sourceFrame;
    poll(*g);
    for (uint32_t i = 0; i < g->targetCount; ++i)
        if (!g->targetSeen[i]) ++g->snapshot.targetMisses;
    g->previousTotal = g->ordinal;
    g->ordinal = 0;
    g->commandListSeen = false;
    if (g->collecting) {
        ++g->collectFrames;
        ++g->snapshot.frames;
        if (g->collectFrames >= kCollectFrames) {
            g->collecting = false;
            g->draining = true;
            g->targetCount = 0;
            g->snapshot.collecting = false;
            g->snapshot.draining = true;
        } else plan(*g);
    } else if (g->draining) {
        ++g->drainFrames;
        if (!pendingCount(*g) || g->drainFrames >= kDrainFrames) {
            const bool timedOut = pendingCount(*g) != 0;
            if (timedOut) {
                for (auto& slot : g->slots) if (slot.phase != SlotPhase::Idle) {
                    ++g->snapshot.expired;
                    if (slot.occState == ResultState::Pending) slot.occState = ResultState::Invalid;
                    if (slot.statsState == ResultState::Pending) slot.statsState = ResultState::Invalid;
                    if (slot.timerState == ResultState::Pending) {
                        slot.timerState = ResultState::Invalid;
                        slot.timerNeedsReset = true;
                    }
                    retire(*g, slot);
                }
            }
            report(*g, timedOut);
            if (g->enabled) resetWindow(*g);
            else g->draining = false;
        }
    }
    g->snapshot.collecting = g->collecting;
    g->snapshot.draining = g->draining;
    g->snapshot.pending = pendingCount(*g);
}

void originalDrawProbeExecuteCommandListNote(ID3D11DeviceContext* context) noexcept {
    if (!g || !g->enabled || !g->adopt(context)) return;
    g->commandListSeen = true;
    ++g->snapshot.commandLists;
}

OriginalDrawProbeSnapshot originalDrawProbeSnapshot() noexcept {
    if (!g) return {};
    auto value = g->snapshot;
    value.enabled = g->enabled;
    value.bound = true;
    value.collecting = g->collecting;
    value.draining = g->draining;
    value.pending = pendingCount(*g);
    return value;
}

bool originalDrawProbeInternalQuery() noexcept { return g_internalQuery; }

void originalDrawProbeShutdown(ID3D11DeviceContext* context) noexcept {
    auto* current = g;
    if (!current) return;
    if (context && !current->owns(context)) return;
    g = nullptr;
    if (context) {
        InternalQuery internal;
        for (auto& slot : current->slots) slot.timer.reset(context);
    } else {
        for (auto& slot : current->slots) slot.timer.reset();
    }
    delete current;
}
} // namespace edvr
