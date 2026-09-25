#include "flat_projection_scope.h"
#include "flat_compute_readback.h"

namespace edvr {
namespace {
bool same(const FlatProjectionCacheKey& a, const FlatProjectionCacheKey& b) {
    return a.identity == b.identity && a.identityGeneration == b.identityGeneration &&
        a.writeGeneration == b.writeGeneration && a.bankEpoch == b.bankEpoch && a.phase == b.phase;
}
bool same(const FlatProjectionPatchRequest& a, const FlatProjectionPatchRequest& b) {
    if (a.layout != b.layout || a.byteOffset != b.byteOffset) return false;
    if (a.layout != FlatProjectionPatchLayout::LightingUvRay) return true;
    const auto& x = a.lighting; const auto& y = b.lighting;
    return x.pixelX == y.pixelX && x.pixelY == y.pixelY && x.width == y.width && x.height == y.height &&
        x.tileWidth == y.tileWidth && x.gridX == y.gridX && x.gridY == y.gridY && x.sampleCount == y.sampleCount;
}
bool stageValid(FlatProjectionStage stage) {
    return stage == FlatProjectionStage::Vertex || stage == FlatProjectionStage::Pixel ||
        stage == FlatProjectionStage::Compute;
}
void get(ID3D11DeviceContext1* context, FlatProjectionStage stage, UINT slot,
         ID3D11Buffer** buffer, UINT* first, UINT* count) {
    switch (stage) {
    case FlatProjectionStage::Vertex: context->VSGetConstantBuffers1(slot, 1, buffer, first, count); break;
    case FlatProjectionStage::Pixel: context->PSGetConstantBuffers1(slot, 1, buffer, first, count); break;
    case FlatProjectionStage::Compute: context->CSGetConstantBuffers1(slot, 1, buffer, first, count); break;
    }
}
void set(ID3D11DeviceContext1* context, FlatProjectionStage stage, UINT slot,
         ID3D11Buffer* buffer, UINT first, UINT count) {
    switch (stage) {
    case FlatProjectionStage::Vertex: context->VSSetConstantBuffers1(slot, 1, &buffer, &first, &count); break;
    case FlatProjectionStage::Pixel: context->PSSetConstantBuffers1(slot, 1, &buffer, &first, &count); break;
    case FlatProjectionStage::Compute: context->CSSetConstantBuffers1(slot, 1, &buffer, &first, &count); break;
    }
}
}

bool FlatProjectionPrivateBuffer::initialize(ID3D11DeviceContext1* context,
    ID3D11Buffer* original, uint64_t identityGeneration) {
    invalidate(); prepared_.reset(); context_.Reset(); original_.Reset(); replacement_.Reset(); scratch_.reset();
    width_ = count_ = 0; uploads_ = 0;
    if (!context || !original || !identityGeneration || context->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE) return false;
    FlatComputeInternalScope internal;
    D3D11_BUFFER_DESC desc{}; original->GetDesc(&desc);
    if (!desc.ByteWidth || desc.ByteWidth > 65536 || desc.ByteWidth % 16 || desc.BindFlags != D3D11_BIND_CONSTANT_BUFFER) return false;
    Microsoft::WRL::ComPtr<ID3D11Device> device, sourceDevice;
    context->GetDevice(device.GetAddressOf()); original->GetDevice(sourceDevice.GetAddressOf());
    if (device.Get() != sourceDevice.Get()) return false;
    std::unique_ptr<unsigned char[]> scratch(new (std::nothrow) unsigned char[desc.ByteWidth]);
    if (!scratch) return false;
    desc.Usage = D3D11_USAGE_DYNAMIC; desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    desc.MiscFlags = desc.StructureByteStride = 0;
    if (FAILED(device->CreateBuffer(&desc, nullptr, replacement_.GetAddressOf()))) return false;
    try { prepared_ = std::make_shared<FlatProjectionPreparedState>(); }
    catch (const std::bad_alloc&) { replacement_.Reset(); return false; }
    scratch_ = std::move(scratch); original_ = original; context_ = context;
    width_ = desc.ByteWidth; identityGeneration_ = identityGeneration;
    prepared_->original = original_.Get(); prepared_->replacement = replacement_.Get();
    return true;
}

bool FlatProjectionPrivateBuffer::prepareSnapshot(const FlatProjectionShadowView& view,
    const FlatProjectionPatchRequest* requests, uint32_t count, const FlatProjectionJitter& jitter, uint32_t phase) {
    if (!context_ || !replacement_ || view.identity != original_.Get() ||
        view.identityGeneration != identityGeneration_ || !view.writeGeneration || !view.bankEpoch ||
        view.width != width_ || !view.bytes || !requests || !count || count > 8) { invalidate(); return false; }
    const auto key = flatProjectionCacheKey(view, phase);
    bool cached = ready_ && same(key_, key) && count_ == count && jitter_.ndcX == jitter.ndcX &&
        jitter_.ndcY == jitter.ndcY && jitter_.uvX == jitter.uvX && jitter_.uvY == jitter.uvY;
    for (uint32_t i = 0; cached && i < count; ++i) cached = same(requests_[i], requests[i]);
    if (cached) return true;
    invalidate();
    if (!prepared_ || prepared_->revision == UINT64_MAX) return false;
    if (!flatPrepareProjectionPatch(view.bytes, width_, requests, count, jitter, scratch_.get(), width_)) return false;
    FlatComputeInternalScope internal;
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context_->Map(replacement_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) return false;
    std::memcpy(mapped.pData, scratch_.get(), width_); context_->Unmap(replacement_.Get(), 0);
    key_ = key; jitter_ = jitter; count_ = count;
    for (uint32_t i = 0; i < count; ++i) requests_[i] = requests[i];
    ++uploads_; ++prepared_->revision; prepared_->ready = ready_ = true;
    return true;
}

FlatPrivateProjectionBinding FlatProjectionPrivateBuffer::binding(FlatProjectionStage stage,
    UINT slot, UINT first, UINT count) const {
    return {stage, slot, original_.Get(), ready_ ? replacement_.Get() : nullptr, first, count,
        prepared_, prepared_ ? prepared_->revision : 0};
}

bool FlatProjectionBindingPlan::initialize(ID3D11DeviceContext1* context,
    const FlatPrivateProjectionBinding* bindings, size_t count) {
    count_ = 0; context_.Reset();
    for (auto& saved : saved_) saved = Saved{};
    if (!context || !bindings || !count || count > kCapacity ||
        context->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE) return false;
    FlatComputeInternalScope internal;
    Microsoft::WRL::ComPtr<ID3D11Device> device;
    context->GetDevice(device.GetAddressOf());
    D3D11_FEATURE_DATA_D3D11_OPTIONS options{};
    if (FAILED(device->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS, &options, sizeof(options))) ||
        !options.ConstantBufferOffsetting) return false;
    for (size_t i = 0; i < count; ++i) {
        const auto& request = bindings[i];
        if (!stageValid(request.stage) || request.slot >= D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT ||
            !request.original || !request.replacement || request.original == request.replacement ||
            request.firstConstant > 4096 || request.constantCount == 0 || request.constantCount > 4096 ||
            request.firstConstant % 16 || request.constantCount % 16) return false;
        if (!request.prepared || !request.prepared->ready || !request.revision ||
            request.prepared->revision != request.revision || request.prepared->original != request.original ||
            request.prepared->replacement != request.replacement) return false;
        for (size_t j = 0; j < i; ++j)
            if (bindings[j].stage == request.stage && bindings[j].slot == request.slot) return false;
        D3D11_BUFFER_DESC original{}, replacement{};
        request.original->GetDesc(&original); request.replacement->GetDesc(&replacement);
        if (original.ByteWidth == 0 || original.ByteWidth > 65536 || original.ByteWidth != replacement.ByteWidth ||
            original.BindFlags != D3D11_BIND_CONSTANT_BUFFER || replacement.BindFlags != D3D11_BIND_CONSTANT_BUFFER) return false;
        Microsoft::WRL::ComPtr<ID3D11Device> sourceDevice, replacementDevice;
        request.original->GetDevice(sourceDevice.GetAddressOf());
        request.replacement->GetDevice(replacementDevice.GetAddressOf());
        if (device.Get() != sourceDevice.Get() || device.Get() != replacementDevice.Get()) return false;
        auto& saved = saved_[i];
        saved.original = request.original; saved.first = request.firstConstant; saved.count = request.constantCount;
        saved.stage = request.stage; saved.slot = request.slot; saved.replacement = request.replacement;
        saved.prepared = request.prepared; saved.revision = request.revision;
    }
    context_ = context; count_ = count;
    return true;
}

bool FlatProjectionBindingPlan::retargetPrepared(ID3D11DeviceContext1* context,
    const FlatPrivateProjectionBinding* bindings, size_t count) {
    if (activeScopes_) return false;
    count_ = 0; context_.Reset();
    for (auto& saved : saved_) saved = Saved{};
    if (!context || !bindings || !count || count > kCapacity) return false;
    // The runtime's tracked private buffers proved same-device size and bind
    // flags at creation. Keep all per-binding and token validation here; no
    // GetDesc/GetDevice/CheckFeatureSupport is allowed on a live draw.
    for (size_t i = 0; i < count; ++i) {
        const auto& request = bindings[i];
        if (!stageValid(request.stage) || request.slot >= D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT ||
            !request.original || !request.replacement || request.original == request.replacement ||
            request.firstConstant != 0 || request.constantCount == 0 || request.constantCount > 4096 ||
            request.constantCount % 16 || !request.prepared || !request.prepared->ready ||
            !request.revision || request.prepared->revision != request.revision ||
            request.prepared->original != request.original || request.prepared->replacement != request.replacement)
            return false;
        for (size_t j = 0; j < i; ++j)
            if (bindings[j].stage == request.stage && bindings[j].slot == request.slot) return false;
    }
    for (size_t i = 0; i < count; ++i) {
        const auto& request = bindings[i];
        auto& saved = saved_[i];
        saved.original = request.original; saved.replacement = request.replacement;
        saved.first = request.firstConstant; saved.count = request.constantCount;
        saved.stage = request.stage; saved.slot = request.slot;
        saved.prepared = request.prepared; saved.revision = request.revision;
    }
    context_ = context; count_ = count;
    return true;
}

bool FlatProjectionBindingPlan::refreshPrepared() {
    if (!context_ || !count_) return false;
    for (size_t i = 0; i < count_; ++i) {
        const auto& saved = saved_[i];
        if (!saved.prepared || !saved.prepared->ready || !saved.prepared->revision ||
            saved.prepared->original != saved.original.Get() || saved.prepared->replacement != saved.replacement.Get()) return false;
    }
    for (size_t i = 0; i < count_; ++i) saved_[i].revision = saved_[i].prepared->revision;
    return true;
}

FlatProjectionBindingScope::FlatProjectionBindingScope(const FlatProjectionBindingPlan& plan) {
    if (!plan.context_ || !plan.count_) return;
    FlatComputeInternalScope internal;
    // Validate the entire transaction before changing even the first stage.
    for (size_t i = 0; i < plan.count_; ++i) {
        const auto& expected = plan.saved_[i]; auto& saved = saved_[i];
        if (!expected.prepared || !expected.prepared->ready || expected.prepared->revision != expected.revision) return;
        get(plan.context_.Get(), expected.stage, expected.slot, saved.original.GetAddressOf(), &saved.first, &saved.count);
        if (saved.original.Get() != expected.original.Get() || saved.first != expected.first || saved.count != expected.count) return;
        saved.stage = expected.stage; saved.slot = expected.slot; saved.replacement = expected.replacement;
    }
    context_ = plan.context_; count_ = plan.count_;
    for (size_t i = 0; i < count_; ++i) {
        const auto& saved = saved_[i];
        set(context_.Get(), saved.stage, saved.slot, saved.replacement.Get(), saved.first, saved.count);
    }
    active_ = true;
    plan_ = &plan;
    ++plan.activeScopes_;
}

FlatProjectionBindingScope::~FlatProjectionBindingScope() {
    if (!active_) return;
    FlatComputeInternalScope internal;
    for (size_t i = count_; i-- > 0;) {
        const auto& saved = saved_[i];
        set(context_.Get(), saved.stage, saved.slot, saved.original.Get(), saved.first, saved.count);
    }
    --plan_->activeScopes_;
}
} // namespace edvr
