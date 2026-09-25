#include "flat_projection_runtime.h"
#include "flat_compute_readback.h"
#include <cstring>

namespace edvr {
namespace {
bool same(const FlatProjectionJitter& a, const FlatProjectionJitter& b) {
    return a.ndcX == b.ndcX && a.ndcY == b.ndcY && a.uvX == b.uvX && a.uvY == b.uvY;
}
bool same(const FlatProjectionPatchRequest& a, const FlatProjectionPatchRequest& b) {
    if (a.layout != b.layout || a.byteOffset != b.byteOffset) return false;
    if (a.layout != FlatProjectionPatchLayout::LightingUvRay) return true;
    const auto& x = a.lighting; const auto& y = b.lighting;
    return x.pixelX == y.pixelX && x.pixelY == y.pixelY && x.width == y.width && x.height == y.height &&
           x.tileWidth == y.tileWidth && x.gridX == y.gridX && x.gridY == y.gridY && x.sampleCount == y.sampleCount;
}
bool same(const FlatProjectionRuntimeRequest& a, const FlatProjectionRuntimeRequest& b) {
    if (a.stage != b.stage || a.slot != b.slot || a.original != b.original ||
        a.firstConstant != b.firstConstant || a.constantCount != b.constantCount ||
        a.patchCount != b.patchCount) return false;
    for (uint32_t i = 0; i < a.patchCount; ++i) if (!same(a.patches[i], b.patches[i])) return false;
    return true;
}
bool sameStructure(const FlatProjectionPatchRequest& a, const FlatProjectionPatchRequest& b) {
    if (a.layout != b.layout || a.byteOffset != b.byteOffset) return false;
    if (a.layout != FlatProjectionPatchLayout::LightingUvRay) return true;
    const auto& x = a.lighting; const auto& y = b.lighting;
    return x.width == y.width && x.height == y.height && x.tileWidth == y.tileWidth &&
           x.gridX == y.gridX && x.gridY == y.gridY && x.sampleCount == y.sampleCount;
}
bool sameStructure(const FlatProjectionRuntimeRequest& a, const FlatProjectionRuntimeRequest& b) {
    if (a.stage != b.stage || a.slot != b.slot || a.original != b.original ||
        a.firstConstant != b.firstConstant || a.constantCount != b.constantCount ||
        a.patchCount != b.patchCount) return false;
    for (uint32_t i = 0; i < a.patchCount; ++i)
        if (!sameStructure(a.patches[i], b.patches[i])) return false;
    return true;
}
bool zero(const FlatProjectionJitter& j) {
    return j.ndcX == 0 && j.ndcY == 0 && j.uvX == 0 && j.uvY == 0;
}
bool lightingMatchesShadow(const FlatProjectionRuntimeRequest& request,
                           const FlatProjectionShadowView& view) {
    for (uint32_t i = 0; i < request.patchCount; ++i) {
        const auto& patch = request.patches[i];
        if (patch.layout != FlatProjectionPatchLayout::LightingUvRay) continue;
        // Epic's qualified CS b0 stores UINT image dimensions in row 0.xy,
        // then UINT cluster dimensions and tile width in row 1.xyzw.
        if (request.stage != FlatProjectionStage::Compute || request.slot != 0 ||
            patch.byteOffset != 10u * 16u || view.width < 32) return false;
        uint32_t image[4]{}, grid[4]{};
        std::memcpy(image, view.bytes, sizeof(image));
        std::memcpy(grid, view.bytes + 16, sizeof(grid));
        const auto& expected = patch.lighting;
        if (image[0] != expected.width || image[1] != expected.height ||
            grid[0] != expected.gridX || grid[1] != expected.gridY ||
            grid[2] != 32 || grid[3] != expected.tileWidth) return false;
    }
    return true;
}
void get(ID3D11DeviceContext1* context, FlatProjectionStage stage, UINT slot,
         ID3D11Buffer** buffer, UINT* first, UINT* count) {
    switch (stage) {
    case FlatProjectionStage::Vertex: context->VSGetConstantBuffers1(slot, 1, buffer, first, count); break;
    case FlatProjectionStage::Pixel: context->PSGetConstantBuffers1(slot, 1, buffer, first, count); break;
    case FlatProjectionStage::Compute: context->CSGetConstantBuffers1(slot, 1, buffer, first, count); break;
    }
}
}

bool FlatProjectionRuntime::owner() const {
    return context_ && owner_ == GetCurrentThreadId();
}
bool FlatProjectionRuntime::refuse(FlatProjectionRuntimeRefusal reason) {
    status_.last = reason;
    ++status_.refusals[static_cast<uint32_t>(reason)];
    if (attempt_.active) {
        failure_ = {};
        failure_.valid = true;
        failure_.reason = reason;
        failure_.branch = attempt_.branch ? attempt_.branch : "unspecified";
        failure_.inPrepare = attempt_.inPrepare;
        failure_.allowAllocation = attempt_.allowAllocation;
        failure_.exactPlan = attempt_.exactPlan;
        failure_.topologyPlan = attempt_.topologyPlan;
        failure_.phase = attempt_.phase;
        failure_.requestIndex = attempt_.requestIndex;
        failure_.patchIndex = attempt_.patchIndex;
        failure_.actualBuffer = attempt_.actualBuffer;
        failure_.actualFirst = attempt_.actualFirst;
        failure_.actualCount = attempt_.actualCount;
        if (owner() && attempt_.requests && attempt_.requestIndex < attempt_.count &&
            attempt_.requestIndex < FlatProjectionBindingPlan::kCapacity) {
            const auto& r = attempt_.requests[attempt_.requestIndex];
            failure_.stage = r.stage; failure_.slot = r.slot; failure_.buffer = r.original;
            failure_.firstConstant = r.firstConstant; failure_.constantCount = r.constantCount;
            failure_.patchCount = r.patchCount;
            if (attempt_.patchIndex < r.patchCount && attempt_.patchIndex < 8) {
                failure_.patchLayout = r.patches[attempt_.patchIndex].layout;
                failure_.patchOffset = r.patches[attempt_.patchIndex].byteOffset;
            }
            if (Tracked* entry = find(r.original)) {
                failure_.tracked = true;
                failure_.trackedGeneration = entry->generation;
                failure_.trackedWidth = entry->width;
                failure_.mutationSerial = entry->mutationSerial;
                failure_.mapped = entry->mapped; failure_.pending = entry->pending;
                failure_.promoted = entry->promoted; failure_.privateReady = entry->privateReady;
                FlatProjectionShadowView view{};
                failure_.shadowPresent = shadows_.lookup(r.original, entry->generation, view);
                if (failure_.shadowPresent) {
                    failure_.shadowWriteGeneration = view.writeGeneration;
                    failure_.shadowBankEpoch = view.bankEpoch;
                }
            }
        }
        attempt_.active = false;
    }
    return false;
}
void FlatProjectionRuntime::beginFailureAttempt(const FlatProjectionRuntimeRequest* requests,
    uint32_t count, uint32_t phase, bool inPrepare, bool allowAllocation) {
    failure_.valid = false;
    attempt_.active = true;
    attempt_.requests = requests; attempt_.count = count; attempt_.phase = phase;
    attempt_.inPrepare = inPrepare; attempt_.allowAllocation = allowAllocation;
    attempt_.exactPlan = attempt_.topologyPlan = false;
    attempt_.requestIndex = attempt_.patchIndex = ~0u;
    attempt_.branch = nullptr; attempt_.actualBuffer = nullptr;
    attempt_.actualFirst = attempt_.actualCount = 0;
}
bool FlatProjectionRuntime::refuseAt(FlatProjectionRuntimeRefusal reason,
    const char* branch, uint32_t requestIndex, uint32_t patchIndex) {
    attempt_.branch = branch;
    attempt_.requestIndex = requestIndex;
    attempt_.patchIndex = patchIndex;
    return refuse(reason);
}
bool FlatProjectionRuntime::initialize(ID3D11DeviceContext* context) {
    if (context_ && !owner()) return refuse(FlatProjectionRuntimeRefusal::WrongThread);
    reset();
    if (!context || context->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE ||
        FAILED(context->QueryInterface(IID_PPV_ARGS(context_.GetAddressOf()))))
        return refuse(FlatProjectionRuntimeRefusal::NoContext1);
    owner_ = GetCurrentThreadId();
    return true;
}
void FlatProjectionRuntime::reset() {
    if (context_ && owner_ != GetCurrentThreadId()) return;
    for (auto& item : cold_) clearCold(item);
    for (auto& plan : plans_) {
        plan.plan.initialize(nullptr, nullptr, 0);
        plan.used = false; plan.count = 0;
    }
    livePlan_.plan.retargetPrepared(nullptr, nullptr, 0);
    livePlan_.used = false; livePlan_.count = 0;
    for (auto& entry : tracked_) {
        entry.privateBuffer.initialize(nullptr, nullptr, 0);
        entry.buffer.Reset(); entry.generation = 0; entry.width = 0;
        entry.mapped = entry.promoted = entry.privateReady = entry.pending = false;
        entry.mutationSerial = 1; entry.mutationOverflow = false;
        entry.mapBytes = nullptr;
    }
    shadows_.reset(); context_.Reset(); owner_ = 0; nextGeneration_ = 1;
    coldEnabled_ = false; coldAttempts_ = 0; planCapabilityReady_ = false;
    status_ = {};
    failure_ = {}; attempt_ = {};
}
void FlatProjectionRuntime::enableColdReadback(bool enabled) {
    if (!owner()) return;
    coldEnabled_ = enabled;
    // Disabling prevents new copies; already queued resources remain retained
    // until their event completes or the bounded poll timeout expires.
}
void FlatProjectionRuntime::mutate(Tracked& entry) {
    if (entry.mutationSerial == UINT64_MAX) entry.mutationOverflow = true;
    else ++entry.mutationSerial;
}
void FlatProjectionRuntime::clearCold(ColdReadback& item) {
    if (!item.used) return;
    for (auto& entry : tracked_)
        if (entry.buffer.Get() == item.source.Get() && entry.generation == item.generation)
            entry.pending = false;
    item.source.Reset(); item.staging.Reset(); item.query.Reset();
    item.used = false; item.generation = item.mutationSerial = 0;
    item.width = item.age = 0;
    if (status_.coldPending) --status_.coldPending;
}
bool FlatProjectionRuntime::queueCold(Tracked& entry) {
    if (!coldEnabled_ || entry.pending || entry.mapped || entry.mutationOverflow ||
        coldAttempts_ >= kColdAttempts) return false;
    ColdReadback* item = nullptr;
    for (auto& candidate : cold_) if (!candidate.used) { item = &candidate; break; }
    if (!item) return false;
    ++coldAttempts_;
    FlatComputeInternalScope internal;
    Microsoft::WRL::ComPtr<ID3D11Device> device, sourceDevice;
    context_->GetDevice(device.GetAddressOf());
    entry.buffer->GetDevice(sourceDevice.GetAddressOf());
    if (!device || device.Get() != sourceDevice.Get()) {
        ++status_.coldFailed; return false;
    }
    D3D11_BUFFER_DESC desc{}; entry.buffer->GetDesc(&desc);
    if (desc.ByteWidth != entry.width || desc.BindFlags != D3D11_BIND_CONSTANT_BUFFER) {
        ++status_.coldFailed; return false;
    }
    desc.Usage = D3D11_USAGE_STAGING; desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.MiscFlags = desc.StructureByteStride = 0;
    Microsoft::WRL::ComPtr<ID3D11Buffer> staging;
    if (FAILED(device->CreateBuffer(&desc, nullptr, staging.GetAddressOf()))) {
        ++status_.coldFailed; return false;
    }
    D3D11_QUERY_DESC queryDesc{}; queryDesc.Query = D3D11_QUERY_EVENT;
    Microsoft::WRL::ComPtr<ID3D11Query> query;
    if (FAILED(device->CreateQuery(&queryDesc, query.GetAddressOf()))) {
        ++status_.coldFailed; return false;
    }
    context_->CopyResource(staging.Get(), entry.buffer.Get());
    context_->End(query.Get()); // no Flush; owner Present polls later
    item->used = true; item->source = entry.buffer; item->staging = staging;
    item->query = query; item->generation = entry.generation;
    item->mutationSerial = entry.mutationSerial; item->width = entry.width;
    item->age = 0; entry.pending = true;
    ++status_.coldQueued; ++status_.coldPending;
    return true;
}
void FlatProjectionRuntime::pollColdReadbacks() {
    if (!owner()) return;
    FlatComputeInternalScope internal;
    for (auto& item : cold_) if (item.used) {
        if (++item.age > kColdMaxAge) {
            ++status_.coldTimeouts; clearCold(item); continue;
        }
        BOOL complete = FALSE;
        const HRESULT event = context_->GetData(item.query.Get(), &complete,
                                                sizeof(complete), D3D11_ASYNC_GETDATA_DONOTFLUSH);
        if (event == S_FALSE || (event == S_OK && !complete)) continue;
        if (FAILED(event)) { ++status_.coldFailed; clearCold(item); continue; }
        Tracked* entry = find(item.source.Get());
        if (!entry || entry->generation != item.generation ||
            entry->mutationSerial != item.mutationSerial || entry->mutationOverflow ||
            entry->mapped) {
            ++status_.coldStale; clearCold(item); continue;
        }
        D3D11_MAPPED_SUBRESOURCE mapped{};
        const HRESULT map = context_->Map(item.staging.Get(), 0, D3D11_MAP_READ,
                                          D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
        if (map == DXGI_ERROR_WAS_STILL_DRAWING) continue;
        if (FAILED(map) || !mapped.pData) {
            ++status_.coldFailed; clearCold(item); continue;
        }
        const bool captured = shadows_.captureFullWrite(entry->buffer.Get(), entry->generation,
                                                          mapped.pData, item.width);
        context_->Unmap(item.staging.Get(), 0);
        if (captured) ++status_.coldCompleted;
        else ++status_.coldFailed;
        clearCold(item);
    }
}
FlatProjectionRuntime::Tracked* FlatProjectionRuntime::find(ID3D11Resource* resource) {
    if (!resource) return nullptr;
    for (auto& entry : tracked_) if (entry.buffer.Get() == resource) return &entry;
    return nullptr;
}
void FlatProjectionRuntime::discard(Tracked& entry) {
    if (entry.buffer) shadows_.releaseBuffer(entry.buffer.Get(), entry.generation);
    entry.privateBuffer.initialize(nullptr, nullptr, 0);
    entry.buffer.Reset(); entry.generation = 0; entry.width = 0;
    entry.mapped = entry.promoted = entry.privateReady = entry.pending = false;
    entry.mutationSerial = 1; entry.mutationOverflow = false;
    entry.mapBytes = nullptr;
}
FlatProjectionRuntime::Tracked* FlatProjectionRuntime::track(ID3D11Resource* resource) {
    if (Tracked* known = find(resource)) return known;
    if (!resource || !shadows_.ready()) return nullptr;
    Microsoft::WRL::ComPtr<ID3D11Buffer> buffer;
    if (FAILED(resource->QueryInterface(IID_PPV_ARGS(buffer.GetAddressOf())))) return nullptr;
    D3D11_BUFFER_DESC desc{}; buffer->GetDesc(&desc);
    if (desc.BindFlags != D3D11_BIND_CONSTANT_BUFFER || desc.ByteWidth < 16 ||
        desc.ByteWidth > 65536 || desc.ByteWidth % 16) return nullptr;
    Tracked* free = nullptr;
    for (auto& entry : tracked_) if (!entry.buffer) { free = &entry; break; }
    // Never evict a named/preflighted buffer or one in a Map transaction.
    if (!free) for (auto& entry : tracked_) if (!entry.promoted && !entry.mapped && !entry.pending) {
        free = &entry; discard(entry); break;
    }
    if (!free || nextGeneration_ == UINT64_MAX) {
        refuse(FlatProjectionRuntimeRefusal::NoCapacity); return nullptr;
    }
    free->generation = nextGeneration_++;
    if (!shadows_.registerBuffer(buffer.Get(), free->generation, desc.ByteWidth)) {
        free->generation = 0; refuse(FlatProjectionRuntimeRefusal::NoCapacity); return nullptr;
    }
    free->buffer = buffer; free->width = desc.ByteWidth;
    free->mutationSerial = 1; free->mutationOverflow = false;
    return free;
}
bool FlatProjectionRuntime::observeCreateBuffer(ID3D11Buffer* buffer, const void* initialData) {
    if (!owner()) return refuse(FlatProjectionRuntimeRefusal::WrongThread);
    Tracked* entry = track(buffer);
    if (!entry) return refuse(FlatProjectionRuntimeRefusal::NoCapacity);
    mutate(*entry);
    entry->privateBuffer.invalidate();
    if (!initialData) {
        shadows_.invalidate(buffer, entry->generation); ++status_.invalidations;
        return false;
    }
    if (!shadows_.captureFullWrite(buffer, entry->generation, initialData, entry->width))
        return refuse(FlatProjectionRuntimeRefusal::MissingFullWrite);
    ++status_.initialWrites; ++status_.fullWrites;
    return true;
}
void FlatProjectionRuntime::observeMap(ID3D11Resource* resource, D3D11_MAP type, const void* bytes) {
    if (!owner() || type == D3D11_MAP_READ) return;
    Tracked* entry = track(resource);
    if (!entry) return;
    mutate(*entry);
    entry->privateBuffer.invalidate();
    if (!bytes || !shadows_.beginMap(entry->buffer.Get(), entry->generation)) {
        shadows_.invalidate(entry->buffer.Get(), entry->generation);
        ++status_.invalidations; entry->mapped = false; entry->mapBytes = nullptr;
        return;
    }
    entry->mapped = true; entry->mapBytes = bytes;
}
void FlatProjectionRuntime::observeUnmap(ID3D11Resource* resource) {
    if (!owner()) return;
    Tracked* entry = find(resource);
    if (!entry || !entry->mapped) return;
    const bool complete = shadows_.finishMapFull(entry->buffer.Get(), entry->generation,
                                                  entry->mapBytes, entry->width);
    entry->mapped = false; entry->mapBytes = nullptr;
    if (complete) ++status_.fullWrites;
    else { ++status_.invalidations; refuse(FlatProjectionRuntimeRefusal::MissingFullWrite); }
}
void FlatProjectionRuntime::observeUpdate(ID3D11Resource* resource, const void* bytes,
                                          const D3D11_BOX* box) {
    if (!owner()) return;
    Tracked* entry = track(resource);
    if (!entry) return;
    mutate(*entry);
    entry->privateBuffer.invalidate();
    if (!bytes || box || entry->mapped) {
        shadows_.invalidate(entry->buffer.Get(), entry->generation);
        entry->mapped = false; entry->mapBytes = nullptr;
        ++status_.invalidations;
        return;
    }
    if (shadows_.captureFullWrite(entry->buffer.Get(), entry->generation, bytes, entry->width))
        ++status_.fullWrites;
    else refuse(FlatProjectionRuntimeRefusal::MissingFullWrite);
}
void FlatProjectionRuntime::invalidate(ID3D11Resource* resource) {
    if (!owner()) return;
    Tracked* entry = find(resource);
    if (!entry) return;
    mutate(*entry);
    entry->privateBuffer.invalidate();
    shadows_.invalidate(entry->buffer.Get(), entry->generation);
    entry->mapped = false; entry->mapBytes = nullptr;
    ++status_.invalidations;
}
void FlatProjectionRuntime::invalidateAll() {
    if (!owner()) return;
    for (auto& entry : tracked_) if (entry.buffer) {
        mutate(entry);
        entry.privateBuffer.invalidate();
        shadows_.invalidate(entry.buffer.Get(), entry.generation);
        entry.mapped = false; entry.mapBytes = nullptr;
        ++status_.invalidations;
    }
}

bool FlatProjectionRuntime::copyConstants(ID3D11Buffer* buffer, uint32_t offset,
                                         uint32_t count, void* out) {
    if (!owner() || !out || !count) return false;
    Tracked* entry = find(buffer);
    FlatProjectionShadowView view{};
    if (!entry || !shadows_.lookup(buffer, entry->generation, view) ||
        offset > view.width || count > view.width - offset) return false;
    std::memcpy(out, view.bytes + offset, count);
    return true;
}
FlatProjectionShadowMetadata FlatProjectionRuntime::constantsMetadata(ID3D11Buffer* buffer) {
    FlatProjectionShadowMetadata result{};
    if (!owner()) return result;
    const Tracked* entry = find(buffer);
    if (!entry) return result;
    result.tracked = true; result.mapped = entry->mapped; result.pending = entry->pending;
    result.width = entry->width; result.generation = entry->generation;
    result.mutationSerial = entry->mutationSerial;
    FlatProjectionShadowView view{};
    result.shadowPresent = shadows_.lookup(buffer, entry->generation, view);
    if (result.shadowPresent) {
        result.writeGeneration = view.writeGeneration; result.bankEpoch = view.bankEpoch;
    }
    return result;
}

bool FlatProjectionRuntime::sameRecipe(const CachedPlan& plan,
    const FlatProjectionRuntimeRequest* requests, uint32_t count,
    const FlatProjectionJitter& jitter, uint32_t phase) const {
    if (!plan.used || plan.count != count || plan.phase != phase || !same(plan.jitter, jitter)) return false;
    for (uint32_t i = 0; i < count; ++i) if (!same(plan.requests[i], requests[i])) return false;
    return true;
}
FlatProjectionRuntime::CachedPlan* FlatProjectionRuntime::findPlan(
    const FlatProjectionRuntimeRequest* requests, uint32_t count,
    const FlatProjectionJitter& jitter, uint32_t phase) {
    for (auto& plan : plans_) if (sameRecipe(plan, requests, count, jitter, phase)) return &plan;
    if (sameRecipe(livePlan_, requests, count, jitter, phase)) return &livePlan_;
    return nullptr;
}
bool FlatProjectionRuntime::sameTopology(const CachedPlan& plan,
    const FlatProjectionRuntimeRequest* requests, uint32_t count) const {
    if (!plan.used || plan.count != count) return false;
    for (uint32_t i = 0; i < count; ++i)
        if (!sameStructure(plan.requests[i], requests[i])) return false;
    return true;
}
FlatProjectionRuntime::CachedPlan* FlatProjectionRuntime::findTopology(
    const FlatProjectionRuntimeRequest* requests, uint32_t count) {
    for (auto& plan : plans_) if (sameTopology(plan, requests, count)) return &plan;
    if (sameTopology(livePlan_, requests, count)) return &livePlan_;
    return nullptr;
}
void FlatProjectionRuntime::invalidatePreparedPlans() {
    // A plan pointer may outlive the request that produced it. Refusing a draw
    // revokes every handed-out token until its exact recipe is prepared again.
    for (auto& entry : tracked_) if (entry.privateReady) entry.privateBuffer.invalidate();
}
bool FlatProjectionRuntime::actualBindings(const FlatProjectionRuntimeRequest* requests,
                                            uint32_t count) {
    FlatComputeInternalScope internal;
    for (uint32_t i = 0; i < count; ++i) {
        const auto& r = requests[i];
        if (r.firstConstant != 0) return refuseAt(FlatProjectionRuntimeRefusal::UnsupportedRange, "binding-request-range", i);
        ID3D11Buffer* actual = nullptr; UINT first = 0, constants = 0;
        get(context_.Get(), r.stage, r.slot, &actual, &first, &constants);
        const bool match = actual == r.original && first == r.firstConstant &&
                            constants == r.constantCount;
        if (!match) {
            attempt_.actualBuffer = actual;
            attempt_.actualFirst = first; attempt_.actualCount = constants;
        }
        if (actual) actual->Release();
        if (!match) return refuseAt(first != 0 ? FlatProjectionRuntimeRefusal::UnsupportedRange :
                                                FlatProjectionRuntimeRefusal::BindingMismatch,
                                    "binding-mismatch", i);
    }
    return true;
}
bool FlatProjectionRuntime::preflight(const FlatProjectionRuntimeRequest* requests,
    uint32_t count, const FlatProjectionJitter& jitter, uint32_t phase, bool allowAllocation) {
    beginFailureAttempt(requests, count, phase, false, allowAllocation);
    if (!owner()) return refuseAt(FlatProjectionRuntimeRefusal::WrongThread, "preflight-owner");
    if (!requests || !count || count > FlatProjectionBindingPlan::kCapacity ||
        !flat_projection_detail::finite(jitter)) return refuseAt(FlatProjectionRuntimeRefusal::InvalidRecipe, "preflight-shape");
    CachedPlan* cached = findPlan(requests, count, jitter, phase);
    attempt_.exactPlan = cached != nullptr;
    if (!cached) cached = findTopology(requests, count);
    attempt_.topologyPlan = cached != nullptr;
    bool liveRetarget = false;
    if (!cached) {
        if (!allowAllocation) {
            if (!planCapabilityReady_)
                return refuseAt(FlatProjectionRuntimeRefusal::PlanFailure, "topology-first-seen-live-no-capability", 0);
            if (!livePlan_.plan.idle())
                return refuseAt(FlatProjectionRuntimeRefusal::PlanFailure, "topology-live-plan-in-use", 0);
            cached = &livePlan_;
            liveRetarget = true;
        }
    }
    if (!cached) {
        for (auto& slot : plans_) if (!slot.used) { cached = &slot; break; }
        if (!cached) return refuseAt(FlatProjectionRuntimeRefusal::NoCapacity, "plan-capacity", 0);
    }
    FlatPrivateProjectionBinding bindings[FlatProjectionBindingPlan::kCapacity]{};
    for (uint32_t i = 0; i < count; ++i) {
        const auto& r = requests[i];
        if (!r.original || r.slot >= D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT ||
            r.firstConstant != 0 || !r.constantCount || r.constantCount > 4096 ||
            r.constantCount % 16 || !r.patchCount || r.patchCount > 8 ||
            (r.stage != FlatProjectionStage::Vertex && r.stage != FlatProjectionStage::Pixel &&
             r.stage != FlatProjectionStage::Compute))
            return refuseAt(r.firstConstant ? FlatProjectionRuntimeRefusal::UnsupportedRange :
                                             FlatProjectionRuntimeRefusal::InvalidRecipe,
                            "request-invalid", i);
        for (uint32_t p = 0; p < r.patchCount; ++p) {
            const auto& patch = r.patches[p];
            const uint32_t span = patch.layout == FlatProjectionPatchLayout::InverseUvRay ||
                                  patch.layout == FlatProjectionPatchLayout::LightingUvRay ? 48u : 64u;
            if (uint64_t(patch.byteOffset) + span > uint64_t(r.constantCount) * 16u)
                return refuseAt(FlatProjectionRuntimeRefusal::UnsupportedRange, "patch-range", i, p);
        }
        for (uint32_t j = 0; j < i; ++j)
            if (r.stage == requests[j].stage && r.slot == requests[j].slot)
                return refuseAt(FlatProjectionRuntimeRefusal::InvalidRecipe, "duplicate-binding", i);
        // A static CB created before this opt-in interval has no write hook
        // history. Register only this explicitly admitted identity, once; a
        // cold GPU snapshot may then establish its full current contents.
        Tracked* entry = find(r.original);
        if (!entry && allowAllocation) {
            attempt_.branch = "buffer-track-capacity";
            attempt_.requestIndex = i;
            entry = track(r.original);
            if (!entry && failure_.valid) return false;
        }
        if (!entry) return refuseAt(FlatProjectionRuntimeRefusal::UnknownBuffer, "untracked-live-buffer", i);
        FlatProjectionShadowView view{};
        if (!shadows_.lookup(r.original, entry->generation, view)) {
            if (allowAllocation) queueCold(*entry);
            return refuseAt(FlatProjectionRuntimeRefusal::MissingFullWrite, "shadow-missing-full-write", i);
        }
        if (!lightingMatchesShadow(r, view))
            return refuseAt(FlatProjectionRuntimeRefusal::InvalidRecipe, "lighting-shadow-mismatch", i);
        if (!entry->privateReady) {
            if (!allowAllocation) return refuseAt(FlatProjectionRuntimeRefusal::PrivateFailure, "private-first-seen-live", i);
            if (!entry->privateBuffer.initialize(context_.Get(), entry->buffer.Get(), entry->generation))
                return refuseAt(FlatProjectionRuntimeRefusal::PrivateFailure, "private-create", i);
            entry->privateReady = true;
        }
        if (!entry->privateBuffer.prepare(shadows_, r.patches, r.patchCount, jitter, phase))
            return refuseAt(FlatProjectionRuntimeRefusal::InvalidRecipe, "private-patch-preparation", i);
        bindings[i] = entry->privateBuffer.binding(r.stage, r.slot, r.firstConstant, r.constantCount);
        entry->promoted = true;
    }
    if (liveRetarget) cached->used = false;
    if (!(liveRetarget ? cached->plan.retargetPrepared(context_.Get(), bindings, count) :
          cached->used ? cached->plan.refreshPrepared() :
                         cached->plan.initialize(context_.Get(), bindings, count)))
        return refuseAt(FlatProjectionRuntimeRefusal::PlanFailure, "plan-initialize-or-refresh");
    if (!liveRetarget) planCapabilityReady_ = true;
    else ++status_.livePlanRetargets;
    cached->used = true; cached->count = count; cached->phase = phase; cached->jitter = jitter;
    for (uint32_t i = 0; i < count; ++i) cached->requests[i] = requests[i];
    ++status_.preflights;
    attempt_.active = false;
    return true;
}
const FlatProjectionBindingPlan* FlatProjectionRuntime::prepare(
    const FlatProjectionRuntimeRequest* requests, uint32_t count,
    const FlatProjectionJitter& jitter, uint32_t phase) {
    beginFailureAttempt(requests, count, phase, true, false);
    if (!owner()) { refuseAt(FlatProjectionRuntimeRefusal::WrongThread, "prepare-owner"); return nullptr; }
    if (!requests || !count || count > FlatProjectionBindingPlan::kCapacity) {
        invalidatePreparedPlans();
        refuseAt(FlatProjectionRuntimeRefusal::InvalidRecipe, "prepare-shape"); return nullptr;
    }
    CachedPlan* cached = findPlan(requests, count, jitter, phase);
    attempt_.exactPlan = cached != nullptr;
    // An exact plan already proves topology; scan only on the failure path.
    attempt_.topologyPlan = cached != nullptr || findTopology(requests, count) != nullptr;
    if (!cached) {
        invalidatePreparedPlans();
        refuseAt(FlatProjectionRuntimeRefusal::PlanFailure, "prepare-no-exact-plan", 0); return nullptr;
    }
    auto fail = [&](FlatProjectionRuntimeRefusal reason, const char* branch, uint32_t index = ~0u) -> const FlatProjectionBindingPlan* {
        invalidatePreparedPlans();
        refuseAt(reason, branch, index);
        return nullptr;
    };
    if (!actualBindings(requests, count)) {
        invalidatePreparedPlans();
        return nullptr;
    }
    for (uint32_t i = 0; i < count; ++i) {
        const auto& r = requests[i];
        Tracked* entry = find(r.original);
        if (!entry) return fail(FlatProjectionRuntimeRefusal::UnknownBuffer, "prepare-untracked-buffer", i);
        FlatProjectionShadowView view{};
        if (!shadows_.lookup(r.original, entry->generation, view)) {
            return fail(FlatProjectionRuntimeRefusal::MissingFullWrite, "prepare-shadow-missing-full-write", i);
        }
        if (!lightingMatchesShadow(r, view)) {
            return fail(FlatProjectionRuntimeRefusal::InvalidRecipe, "prepare-lighting-shadow-mismatch", i);
        }
        if (!entry->privateBuffer.prepare(shadows_, r.patches, r.patchCount, jitter, phase)) {
            return fail(FlatProjectionRuntimeRefusal::PrivateFailure, "prepare-private-patch", i);
        }
    }
    if (!cached->plan.refreshPrepared()) {
        return fail(FlatProjectionRuntimeRefusal::PlanFailure, "prepare-plan-refresh");
    }
    ++status_.prepared;
    attempt_.active = false;
    if (zero(jitter)) { ++status_.zeroPhaseReady; return nullptr; }
    return &cached->plan;
}

} // namespace edvr
