#include "original_draw_probe.h"
#include "original_draw_identity_capture.h"
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
constexpr uint32_t kTargetFamilyCount = 3;
constexpr uint32_t kCohortFrames = 24;

constexpr uint64_t kFamilyVs[kTargetFamilyCount] = {
    0xEB5234DB6ADB491Dull, 0x5B4D8E894EEDA8B4ull, 0xBBE58E40FE88EC80ull};
constexpr uint64_t kFamilyPs[kTargetFamilyCount] = {
    0xCB9F297EFF264251ull, 0x4375B72964F386CDull, 0xDB3E8D20CF53FBC0ull};

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
    uint8_t frontFail = 0, frontDepthFail = 0, backFail = 0, backDepthFail = 0;
    bool operator==(const DetailKey& o) const noexcept {
        return vs == o.vs && ps == o.ps && verdict == o.verdict &&
            count == o.count && instances == o.instances && width == o.width && height == o.height &&
            sampleMask == o.sampleMask && stencilRef == o.stencilRef && rtv == o.rtv && dsv == o.dsv &&
            topology == o.topology && flags == o.flags && kind == o.kind &&
            pass == o.pass && eye == o.eye && writeMask == o.writeMask &&
            depthFunc == o.depthFunc && cull == o.cull && frontFail == o.frontFail &&
            frontDepthFail == o.frontDepthFail && backFail == o.backFail &&
            backDepthFail == o.backDepthFail;
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

struct BindingIdentity {
    std::array<ComPtr<ID3D11RenderTargetView>, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT> rtvs;
    ComPtr<ID3D11DepthStencilView> dsv;
    ComPtr<ID3D11InputLayout> layout;
    std::array<ComPtr<ID3D11Buffer>, D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT> vertexBuffers;
    std::array<UINT, D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT> vertexStrides{};
    std::array<UINT, D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT> vertexOffsets{};
    ComPtr<ID3D11Buffer> indexBuffer;
    ComPtr<ID3D11ShaderResourceView> modelPool;
    std::array<ComPtr<ID3D11ShaderResourceView>, 4> psResources;
    ComPtr<ID3D11BlendState> blend;
    ComPtr<ID3D11DepthStencilState> depth;
    ComPtr<ID3D11RasterizerState> raster;
    std::array<D3D11_VIEWPORT, D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE> viewports{};
    std::array<D3D11_RECT, D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE> scissors{};
    float blendFactors[4]{};
    UINT viewportCount = 0, scissorCount = 0, indexOffset = 0, sampleMask = 0, stencilRef = 0;
    DXGI_FORMAT indexFormat = DXGI_FORMAT_UNKNOWN;
    D3D11_PRIMITIVE_TOPOLOGY topology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    uint32_t count = 0, instances = 0, startIndex = 0, startInstance = 0;
    int32_t baseVertex = 0;
    uint8_t family = 0xff, pass = 0, eye = 0, kind = 0;
};

bool sameBinding(const BindingIdentity& a, const BindingIdentity& b) noexcept {
    if (a.dsv.Get() != b.dsv.Get() || a.layout.Get() != b.layout.Get() ||
        a.indexBuffer.Get() != b.indexBuffer.Get() || a.modelPool.Get() != b.modelPool.Get() ||
        a.blend.Get() != b.blend.Get() || a.depth.Get() != b.depth.Get() ||
        a.raster.Get() != b.raster.Get() || a.viewportCount != b.viewportCount ||
        a.scissorCount != b.scissorCount || a.sampleMask != b.sampleMask ||
        a.stencilRef != b.stencilRef || a.topology != b.topology ||
        a.indexOffset != b.indexOffset || a.indexFormat != b.indexFormat ||
        a.count != b.count || a.instances != b.instances || a.startIndex != b.startIndex ||
        a.startInstance != b.startInstance || a.baseVertex != b.baseVertex ||
        a.family != b.family || a.pass != b.pass || a.eye != b.eye || a.kind != b.kind ||
        std::memcmp(a.blendFactors, b.blendFactors, sizeof(a.blendFactors)) != 0 ||
        std::memcmp(a.viewports.data(), b.viewports.data(), a.viewportCount * sizeof(D3D11_VIEWPORT)) != 0 ||
        std::memcmp(a.scissors.data(), b.scissors.data(), a.scissorCount * sizeof(D3D11_RECT)) != 0)
        return false;
    for (size_t i = 0; i < a.rtvs.size(); ++i)
        if (a.rtvs[i].Get() != b.rtvs[i].Get()) return false;
    for (size_t i = 0; i < a.vertexBuffers.size(); ++i)
        if (a.vertexBuffers[i].Get() != b.vertexBuffers[i].Get() ||
            a.vertexStrides[i] != b.vertexStrides[i] || a.vertexOffsets[i] != b.vertexOffsets[i]) return false;
    for (size_t i = 0; i < a.psResources.size(); ++i)
        if (a.psResources[i].Get() != b.psResources[i].Get()) return false;
    return true;
}

BindingIdentity captureBinding(ID3D11DeviceContext* ctx, const OriginalDrawProbeInput& input,
                               uint8_t family) noexcept {
    BindingIdentity b{};
    ID3D11RenderTargetView* rtvs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
    ctx->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtvs, &b.dsv);
    for (size_t i = 0; i < b.rtvs.size(); ++i) b.rtvs[i].Attach(rtvs[i]);
    ctx->IAGetInputLayout(&b.layout);
    ID3D11Buffer* vbs[D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT]{};
    ctx->IAGetVertexBuffers(0, D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT, vbs,
                            b.vertexStrides.data(), b.vertexOffsets.data());
    for (size_t i = 0; i < b.vertexBuffers.size(); ++i) b.vertexBuffers[i].Attach(vbs[i]);
    ctx->IAGetIndexBuffer(&b.indexBuffer, &b.indexFormat, &b.indexOffset);
    ctx->IAGetPrimitiveTopology(&b.topology);
    ctx->VSGetShaderResources(33, 1, &b.modelPool);
    ID3D11ShaderResourceView* ps[4]{};
    ctx->PSGetShaderResources(0, 4, ps);
    for (size_t i = 0; i < b.psResources.size(); ++i) b.psResources[i].Attach(ps[i]);
    b.viewportCount = static_cast<UINT>(b.viewports.size());
    ctx->RSGetViewports(&b.viewportCount, b.viewports.data());
    b.scissorCount = static_cast<UINT>(b.scissors.size());
    ctx->RSGetScissorRects(&b.scissorCount, b.scissors.data());
    ctx->OMGetBlendState(&b.blend, b.blendFactors, &b.sampleMask);
    ctx->OMGetDepthStencilState(&b.depth, &b.stencilRef);
    ctx->RSGetState(&b.raster);
    b.count = input.count; b.instances = input.instances;
    b.startIndex = input.startIndex; b.baseVertex = input.baseVertex;
    b.startInstance = input.startInstance; b.family = family;
    b.pass = static_cast<uint8_t>(input.pass);
    b.eye = input.eye == 0 ? 0 : input.eye == 1 ? 1 : 2;
    b.kind = static_cast<uint8_t>(input.kind);
    return b;
}

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
    uint8_t family = 0xff;
    uint64_t cohortGeneration = 0;
    OriginalDrawIdentityCapture identityCapture;
    OriginalDrawIdentitySnapshot identity{};
    OriginalDrawIdentityPoll identityState = OriginalDrawIdentityPoll::Unavailable;
    BindingIdentity binding{};
};

struct RecurrenceHistory {
    bool valid = false, zero = false;
    uint64_t cohortGeneration = 0, submittedFrame = 0;
    BindingIdentity binding{};
    OriginalDrawIdentitySnapshot identity{};
};

struct Controller {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    OriginalDrawProbeQueryOps ops{};
    OriginalDrawIdentityCaptureResources identityResources;
    DWORD ownerThread = 0;
    bool enabled = false, targeted = false, collecting = false, draining = false;
    bool firstOriginal = false, firstSubmit = false, firstTargetCandidate = false;
    bool firstIdentityPayload = false, commandListSeen = false;
    uint64_t frameSerial = 0, sourceFrame = 0, window = 0;
    uint64_t collectFrames = 0, drainFrames = 0;
    uint64_t ordinal = 0, previousTotal = 0, randomState = 0x9e3779b97f4a7c15ull;
    std::array<uint64_t, kTargetsPerFrame> targets{};
    std::array<bool, kTargetsPerFrame> targetSeen{};
    uint32_t targetCount = 0, pollCursor = 0;
    std::array<uint64_t, kTargetFamilyCount> familyOrdinal{};
    std::array<uint64_t, kTargetFamilyCount> previousFamilyTotal{};
    std::array<uint64_t, kTargetFamilyCount> familyTarget{};
    std::array<bool, kTargetFamilyCount> familyTargetValid{};
    std::array<bool, kTargetFamilyCount> familyTargetSeen{};
    uint32_t cohortAge = 0;
    uint64_t cohortGeneration = 0;
    OriginalDrawProbeSnapshot snapshot{};
    std::array<Slot, kSlotCount> slots{};
    std::array<DetailBucket, kBucketCount> buckets{};
    Totals aggregates[static_cast<unsigned>(OriginalDrawPass::Count)][3][2]{};
    Totals familyAggregates[kTargetFamilyCount][static_cast<unsigned>(OriginalDrawPass::Count)][2]{};
    std::array<RecurrenceHistory, kTargetFamilyCount> recurrence{};
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

int targetFamily(const OriginalDrawProbeInput* input) noexcept {
    if (!input || input->modified) return -1;
    for (uint32_t i = 0; i < kTargetFamilyCount; ++i)
        if (input->originalVsHash == kFamilyVs[i] && input->originalPsHash == kFamilyPs[i])
            return static_cast<int>(i);
    return -1;
}

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
        key.frontFail = static_cast<uint8_t>(desc.FrontFace.StencilFailOp);
        key.frontDepthFail = static_cast<uint8_t>(desc.FrontFace.StencilDepthFailOp);
        key.backFail = static_cast<uint8_t>(desc.BackFace.StencilFailOp);
        key.backDepthFail = static_cast<uint8_t>(desc.BackFace.StencilDepthFailOp);
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

    ID3D11GeometryShader* gs = nullptr; ID3D11HullShader* hs = nullptr; ID3D11DomainShader* ds = nullptr;
    ctx->GSGetShader(&gs, nullptr, nullptr); ctx->HSGetShader(&hs, nullptr, nullptr);
    ctx->DSGetShader(&ds, nullptr, nullptr);
    if (gs) key.flags |= 1u << 11;
    if (hs) key.flags |= 1u << 12;
    if (ds) key.flags |= 1u << 13;
    release(gs); release(hs); release(ds);

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

bool sameIdentity(const OriginalDrawIdentitySnapshot& a,
                  const OriginalDrawIdentitySnapshot& b) noexcept {
    if (a.count != b.count || a.startInstance != b.startInstance) return false;
    for (uint32_t i = 0; i < a.count; ++i) {
        const auto& x = a.records[i]; const auto& y = b.records[i];
        if (x.valid != y.valid || x.skinned != y.skinned ||
            std::memcmp(x.instanceAndModelDataIndex, y.instanceAndModelDataIndex,
                        sizeof(x.instanceAndModelDataIndex)) != 0 ||
            x.modelRecord != y.modelRecord) return false;
    }
    return true;
}

void observeRecurrence(Controller& c, const Slot& slot, bool zero) noexcept {
    if (slot.family >= kTargetFamilyCount) return;
    auto& history = c.recurrence[slot.family];
    const bool ordered = !history.valid || slot.submittedFrame > history.submittedFrame;
    if (!ordered) { ++c.snapshot.recurrenceOutOfOrder; return; }
    bool usable = slot.identityState == OriginalDrawIdentityPoll::Ready;
    if (usable) for (uint32_t i = 0; i < slot.identity.count; ++i) {
        if (!slot.identity.records[i].valid) { ++c.snapshot.identityInvalidRecord; usable = false; }
        if (slot.identity.records[i].skinned) { ++c.snapshot.identitySkinned; usable = false; }
    }
    if (!usable) { history = {}; return; }
    if (!c.firstIdentityPayload) {
        c.firstIdentityPayload = true;
        Log::get().note("Original draw targeted recurrence: first complete unskinned ID+t33 payload retired.");
    }
    const bool adjacent = history.valid && slot.submittedFrame == history.submittedFrame + 1;
    const bool sameCohort = history.valid && history.cohortGeneration == slot.cohortGeneration;
    if (history.valid && (!adjacent || !sameCohort)) ++c.snapshot.recurrenceGaps;
    if (adjacent && sameCohort) {
        if (!sameBinding(history.binding, slot.binding)) {
            ++c.snapshot.recurrenceBindingChanged;
        } else if (!sameIdentity(history.identity, slot.identity)) {
            ++c.snapshot.recurrencePayloadChanged;
            bool idsChanged = history.identity.count != slot.identity.count;
            bool modelChanged = idsChanged;
            bool poseChanged = false, otherChanged = false;
            const uint32_t n = std::min(history.identity.count, slot.identity.count);
            for (uint32_t i = 0; i < n; ++i) {
                idsChanged |= std::memcmp(history.identity.records[i].instanceAndModelDataIndex,
                    slot.identity.records[i].instanceAndModelDataIndex, 8) != 0;
                const auto& oldBytes = history.identity.records[i].modelRecord;
                const auto& newBytes = slot.identity.records[i].modelRecord;
                const bool recordChanged = oldBytes != newBytes;
                modelChanged |= recordChanged;
                if (recordChanged) for (uint32_t byte = 0; byte < kOriginalDrawIdentityModelBytes; ++byte) {
                    if (oldBytes[byte] == newBytes[byte]) continue;
                    if ((byte >= 4 && byte < 28) || (byte >= 288 && byte < 320)) poseChanged = true;
                    else otherChanged = true;
                }
            }
            if (idsChanged) ++c.snapshot.recurrenceIdChanged;
            if (modelChanged) ++c.snapshot.recurrenceModelChanged;
            if (poseChanged) ++c.snapshot.recurrencePoseFieldsChanged;
            if (otherChanged) ++c.snapshot.recurrenceOtherModelFieldsChanged;
            if (idsChanged && !modelChanged) ++c.snapshot.recurrenceIdOnlyChanged;
        } else {
            ++c.snapshot.recurrenceSamePayload;
            if (history.zero && !zero) ++c.snapshot.recurrenceZeroToVisible;
            if (!history.zero && zero) ++c.snapshot.recurrenceVisibleToZero;
        }
    }
    history.valid = true; history.zero = zero;
    history.cohortGeneration = slot.cohortGeneration;
    history.submittedFrame = slot.submittedFrame;
    history.binding = slot.binding; history.identity = slot.identity;
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
        observeRecurrence(c, slot, zero);
        ++c.snapshot.ready;
        if (zero) ++c.snapshot.zeroSamples; else ++c.snapshot.nonzeroSamples;
        const bool timed = slot.timerState == ResultState::Ready;
        auto& aggregate = c.aggregates[static_cast<unsigned>(slot.input.pass)]
                                      [eyeIndex(slot.input.eye)][slot.input.modified ? 1 : 0];
        addResult(aggregate, zero, timed, slot.gpuMs);
        Totals* familyAggregate = nullptr;
        if (slot.family < kTargetFamilyCount) {
            ++c.snapshot.targetedFamilySamples;
            familyAggregate = &c.familyAggregates[slot.family]
                [static_cast<unsigned>(slot.input.pass)][slot.input.modified ? 1 : 0];
            addResult(*familyAggregate, zero, timed, slot.gpuMs);
        }
        auto* detail = bucketFor(c, slot.key);
        if (detail) addResult(detail->totals, zero, timed, slot.gpuMs);
        if (slot.statsState == ResultState::Ready) {
            ++c.snapshot.pipelineReady;
            addStats(aggregate, slot.stats);
            if (familyAggregate) addStats(*familyAggregate, slot.stats);
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
    slot.binding = {};
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
            slot.timerState != ResultState::Pending &&
            slot.identityState == OriginalDrawIdentityPoll::Pending) {
            slot.identityState = slot.identityCapture.poll(c.context.Get(), slot.identity);
            if (slot.identityState == OriginalDrawIdentityPoll::Unavailable)
                ++c.snapshot.identityReadbackFailed;
        }
        if (slot.occState != ResultState::Pending && slot.statsState != ResultState::Pending &&
            slot.timerState != ResultState::Pending &&
            slot.identityState != OriginalDrawIdentityPoll::Pending) {
            // The rotating query cursor can encounter a newer ready sample
            // before an older one. Preserve per-family submission order without
            // waiting on the GPU or increasing the query polling budget.
            bool olderPending = false;
            if (!slot.calibration && slot.family < kTargetFamilyCount)
                for (const auto& older : c.slots)
                    if (older.phase == SlotPhase::Pending && !older.calibration &&
                        older.family == slot.family &&
                        older.submittedFrame < slot.submittedFrame) {
                        olderPending = true;
                        break;
                    }
            if (!olderPending) retire(c, slot);
        }
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
        "Original draw diagnostic selected pipeline states: stencil-fail-op=%llu stencil-depth-fail-op=%llu UAV=%llu stream-output=%llu GS=%llu HS=%llu DS=%llu. These are eligibility observations only; original draws are never suppressed.",
        c.snapshot.stencilFailOps, c.snapshot.stencilDepthFailOps, c.snapshot.uavBound,
        c.snapshot.streamOutputBound, c.snapshot.geometryShaderBound,
        c.snapshot.hullShaderBound, c.snapshot.domainShaderBound);
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
    for (unsigned family = 0; family < kTargetFamilyCount; ++family)
        for (unsigned pass = 0; pass < static_cast<unsigned>(OriginalDrawPass::Count); ++pass)
            for (unsigned modified = 0; modified < 2; ++modified) {
                const auto& total = c.familyAggregates[family][pass][modified];
                if (!total.samples) continue;
                Log::get().note(
                    "Original draw targeted family aggregate: family=%u vs=%016llX ps=%016llX pass=%u modified=%u samples=%llu zero=%llu nonzero=%llu zero-gpu=%.6f/%llu ms nonzero-gpu=%.6f/%llu ms stats=%llu IAvertices=%llu VSinvocations=%llu CPrimitives=%llu PSinvocations=%llu. Query timing is inclusive and cohort-biased; count and instance variants are deliberately coalesced.",
                    family, kFamilyVs[family], kFamilyPs[family], pass, modified,
                    total.samples, total.zero, total.nonzero, total.zeroMs, total.zeroTime,
                    total.nonzeroMs, total.nonzeroTime, total.statsValid, total.iaVertices,
                    total.vsInvocations, total.cPrimitives, total.psInvocations);
            }
    if (c.targeted) Log::get().note(
        "Original draw targeted recurrence controller v1: candidates=%llu cohort-reseeds=%llu population-drift-frames=%llu misses=%llu. Held family-local cohorts are biased followups; they do not estimate a population zero percentage.",
        c.snapshot.targetedCandidates, c.snapshot.cohortReseeds,
        c.snapshot.populationDrift, c.snapshot.targetMisses);
    if (c.targeted) Log::get().note(
        "Original draw targeted identity: unsupported=%llu over32-or-zero=%llu layout=%llu missing-instance=%llu missing-pool=%llu invalid-record=%llu readback-failed=%llu expired=%llu skinned=%llu; same-full-payload=%llu payload-changed=%llu ID-changed=%llu model-bytes-changed=%llu pose-field-changed=%llu other-model-field-changed=%llu ID-only-changed=%llu binding-changed=%llu gaps=%llu out-of-order=%llu zero-to-visible=%llu visible-to-zero=%llu. Pose fields mean byte ranges [4,28) and [288,320), a diagnostic grouping rather than object identity. Transitions require the same retained bindings, draw arguments, viewport/scissor and exact IDs plus 336-byte t33 records in one uninterrupted cohort. Fingerprint/payload duplicates are observed recurrence, never proven object IDs; shader constants, texture contents and t38 skinning remain uncaptured.",
        c.snapshot.identityUnsupported, c.snapshot.identityOver32,
        c.snapshot.identityIneligibleLayout, c.snapshot.identityMissingInstanceBuffer,
        c.snapshot.identityMissingPool, c.snapshot.identityInvalidRecord,
        c.snapshot.identityReadbackFailed, c.snapshot.identityExpired,
        c.snapshot.identitySkinned, c.snapshot.recurrenceSamePayload,
        c.snapshot.recurrencePayloadChanged, c.snapshot.recurrenceIdChanged,
        c.snapshot.recurrenceModelChanged, c.snapshot.recurrencePoseFieldsChanged,
        c.snapshot.recurrenceOtherModelFieldsChanged, c.snapshot.recurrenceIdOnlyChanged,
        c.snapshot.recurrenceBindingChanged,
        c.snapshot.recurrenceGaps, c.snapshot.recurrenceOutOfOrder,
        c.snapshot.recurrenceZeroToVisible, c.snapshot.recurrenceVisibleToZero);
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
            "modified=%u topology=%u rtv=%u dsv=%u depth-func=%u stencil-ref=%u stencil-ops=%u/%u/%u/%u cull=%u sample-mask=0x%X "
            "flags=0x%X write-mask=0x%X samples=%llu zero=%llu nonzero=%llu "
            "zero-gpu=%.6f/%llu ms nonzero-gpu=%.6f/%llu ms stats=%llu IAvertices=%llu "
            "VSinvocations=%llu CPrimitives=%llu PSinvocations=%llu.",
            emitted + 1, static_cast<unsigned long long>(bucket.key.vs),
            static_cast<unsigned long long>(bucket.key.ps), bucket.key.verdict,
            bucket.key.pass, bucket.key.eye, bucket.key.kind, bucket.key.count,
            bucket.key.instances, bucket.key.width, bucket.key.height, (bucket.key.flags & 1) != 0,
            unsigned(bucket.key.topology), unsigned(bucket.key.rtv), unsigned(bucket.key.dsv),
            bucket.key.depthFunc, bucket.key.stencilRef, bucket.key.frontFail,
            bucket.key.frontDepthFail, bucket.key.backFail, bucket.key.backDepthFail,
            bucket.key.cull, bucket.key.sampleMask,
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
    c.familyOrdinal = {};
    c.previousFamilyTotal = {};
    c.familyTarget = {};
    c.familyTargetValid = {};
    c.familyTargetSeen = {};
    c.cohortAge = 0;
    c.cohortGeneration = 0;
    c.recurrence = {};
    c.buckets = {};
    for (auto& pass : c.aggregates) for (auto& eye : pass)
        for (auto& modified : eye) modified = {};
    for (auto& family : c.familyAggregates) for (auto& pass : family)
        for (auto& modified : pass) modified = {};
    c.iaZero = c.clipZero = c.psZero = 0;
    c.calibrationReady = 0;
    c.calibrationMs = 0;
}

void plan(Controller& c) noexcept {
    if (c.targeted) {
        const bool haveCohort = std::any_of(c.familyTargetValid.begin(), c.familyTargetValid.end(),
                                            [](bool v) { return v; });
        if (!haveCohort || c.cohortAge >= kCohortFrames) {
            c.familyTargetValid = {};
            c.familyTargetSeen = {};
            c.cohortAge = 0;
            ++c.cohortGeneration;
            ++c.snapshot.cohortReseeds;
            for (uint32_t family = 0; family < kTargetFamilyCount; ++family) {
                if (!c.previousFamilyTotal[family]) continue;
                c.familyTarget[family] = c.bounded(c.previousFamilyTotal[family]);
                c.familyTargetValid[family] = true;
                ++c.snapshot.planned;
            }
        } else {
            for (bool valid : c.familyTargetValid) if (valid) ++c.snapshot.planned;
        }
        ++c.cohortAge;
        return;
    }
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

void countIdentityFailure(Controller& c, const OriginalDrawProbeInput& input,
                          OriginalDrawIdentityReason reason) noexcept {
    ++c.snapshot.identityUnsupported;
    if (input.instances == 0 || input.instances > kOriginalDrawIdentityMaxInstances)
        ++c.snapshot.identityOver32;
    else if (reason == OriginalDrawIdentityReason::IneligibleLayout)
        ++c.snapshot.identityIneligibleLayout;
    else if (reason == OriginalDrawIdentityReason::MissingModelPool ||
             reason == OriginalDrawIdentityReason::ModelPoolLayoutMismatch)
        ++c.snapshot.identityMissingPool;
    else if (reason == OriginalDrawIdentityReason::MissingInstanceBuffer ||
             reason == OriginalDrawIdentityReason::InstanceLayoutMismatch ||
             reason == OriginalDrawIdentityReason::InstanceRangeOutOfBounds)
        ++c.snapshot.identityMissingInstanceBuffer;
}

void discardPending(Controller& c) noexcept {
    for (auto& slot : c.slots) if (slot.phase != SlotPhase::Idle) {
        InternalQuery internal;
        slot.timer.reset(c.context.Get());
        slot.timerNeedsReset = false;
        if (slot.identityState == OriginalDrawIdentityPoll::Pending) {
            slot.identityCapture = OriginalDrawIdentityCapture{};
            slot.identityState = OriginalDrawIdentityPoll::Unavailable;
        }
        slot.binding = {};
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

void originalDrawProbeRememberLayout(ID3D11InputLayout* layout,
    const D3D11_INPUT_ELEMENT_DESC* elements, UINT count, uint64_t vertexShaderHash) noexcept {
    originalDrawIdentityRememberLayout(layout, elements, count, vertexShaderHash);
}

void originalDrawProbeConfigure(bool enabled, bool targeted) noexcept {
    if (!g || !g->owns(g->context.Get()) ||
        (g->enabled == enabled && (!enabled || g->targeted == targeted))) return;
    const bool modeChanged = g->enabled && enabled && g->targeted != targeted;
    g->enabled = enabled;
    g->targeted = targeted;
    if (enabled) {
        if (pendingCount(*g)) discardPending(*g);
        resetWindow(*g);
        Log::get().note(targeted
            ? "Original draw diagnostic: targeted recurrence enabled; one held family-local ordinal per known VS/PS family, 24-frame cohorts, at most three query brackets per frame, no draw suppression. Cohorts are intentionally biased followups, not population-uniform samples."
            : "Original draw diagnostic: enabled; 600-frame repeating windows, three prior-frame-uniform targets, 64 query slots, no draw suppression.");
        if (modeChanged) Log::get().note("Original draw diagnostic: sampling mode changed; pending work and cohort history reset.");
    } else {
        g->collecting = false;
        g->familyTargetValid = {};
        g->familyTargetSeen = {};
        g->targetCount = 0;
        g->draining = pendingCount(*g) != 0;
        g->snapshot.enabled = false;
        g->snapshot.collecting = false;
        g->snapshot.draining = g->draining;
        Log::get().note("Original draw diagnostic: disabled; outstanding results drain without waits or Flush.");
    }
}

bool originalDrawProbeSelect(ID3D11DeviceContext* context,
                             const OriginalDrawProbeInput* metadata) noexcept {
    if (!g || !g->enabled || !g->collecting || !g->adopt(context)) return false;
    const uint64_t ordinal = g->ordinal++;
    ++g->snapshot.originalCalls;
    if (!g->firstOriginal) {
        g->firstOriginal = true;
        Log::get().note("Original draw diagnostic: first original native draw observed.");
    }
    if (g->collecting && g->targeted) {
        ++g->snapshot.targetedCalls;
        const int family = targetFamily(metadata);
        if (family < 0) return false;
        ++g->snapshot.targetedCandidates;
        if (!g->firstTargetCandidate) {
            g->firstTargetCandidate = true;
            Log::get().note("Original draw targeted recurrence: first exact VS/PS family candidate observed.");
        }
        const auto index = static_cast<uint32_t>(family);
        const uint64_t localOrdinal = g->familyOrdinal[index]++;
        if (g->familyTargetValid[index] && g->familyTarget[index] == localOrdinal) {
            g->familyTargetSeen[index] = true;
            return true;
        }
        return false;
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
    if (key.frontFail != D3D11_STENCIL_OP_KEEP || key.backFail != D3D11_STENCIL_OP_KEEP)
        ++g->snapshot.stencilFailOps;
    if (key.frontDepthFail != D3D11_STENCIL_OP_KEEP || key.backDepthFail != D3D11_STENCIL_OP_KEEP)
        ++g->snapshot.stencilDepthFailOps;
    if (key.flags & (1u << 7)) ++g->snapshot.uavBound;
    if (key.flags & (1u << 8)) ++g->snapshot.streamOutputBound;
    if (key.flags & (1u << 11)) ++g->snapshot.geometryShaderBound;
    if (key.flags & (1u << 12)) ++g->snapshot.hullShaderBound;
    if (key.flags & (1u << 13)) ++g->snapshot.domainShaderBound;
    if (noRaster) { ++g->snapshot.noRaster; return ticket; }
    if (noWrite) { ++g->snapshot.noWrite; return ticket; }
    Slot* slot = idleSlot(*g);
    if (!slot) { ++g->snapshot.ringFull; return ticket; }
    const int family = targetFamily(&input);
    slot->family = family < 0 ? 0xff : static_cast<uint8_t>(family);
    slot->cohortGeneration = g->cohortGeneration;
    slot->identity = {};
    slot->identityState = OriginalDrawIdentityPoll::Unavailable;
    slot->binding = {};
    if (g->targeted && family >= 0) {
        slot->binding = captureBinding(context, input, slot->family);
        if (slot->identityCapture.capture(g->identityResources, g->device.Get(), context,
                                          input.instances, input.startInstance))
            slot->identityState = OriginalDrawIdentityPoll::Pending;
        else
            countIdentityFailure(*g, input, slot->identityCapture.reason());
    }
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
    if (g->targeted) {
        for (uint32_t family = 0; family < kTargetFamilyCount; ++family) {
            if (g->familyTargetValid[family] && !g->familyTargetSeen[family])
                ++g->snapshot.targetMisses;
            if (g->previousFamilyTotal[family] && g->familyOrdinal[family] != g->previousFamilyTotal[family])
                ++g->snapshot.populationDrift;
            g->previousFamilyTotal[family] = g->familyOrdinal[family];
        }
        g->familyOrdinal = {};
        g->familyTargetSeen = {};
    } else if (g->collecting) {
        for (uint32_t i = 0; i < g->targetCount; ++i)
            if (!g->targetSeen[i]) ++g->snapshot.targetMisses;
    }
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
            g->familyTargetValid = {};
            g->familyTargetSeen = {};
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
                    if (slot.identityState == OriginalDrawIdentityPoll::Pending) {
                        ++g->snapshot.identityExpired;
                        slot.identityCapture = OriginalDrawIdentityCapture{};
                        slot.identityState = OriginalDrawIdentityPoll::Unavailable;
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
