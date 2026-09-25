#pragma once
#include <d3d11_1.h>
#include <wrl/client.h>
#include <cstddef>
#include "flat_projection_bindings.h"

namespace edvr {
// This is the binding mechanism, not shader admission. Callers must qualify the
// shader, source bytes and raster phase before constructing a request. Resources
// must be prepared before rasterization. No allocation or upload occurs here.
enum class FlatProjectionStage { Vertex, Pixel, Compute };
struct FlatProjectionPreparedState {
    ID3D11Buffer* original = nullptr;
    ID3D11Buffer* replacement = nullptr;
    uint64_t revision = 0;
    bool ready = false;
};
struct FlatPrivateProjectionBinding {
    FlatProjectionStage stage = FlatProjectionStage::Vertex;
    UINT slot = 0;
    ID3D11Buffer* original = nullptr;
    ID3D11Buffer* replacement = nullptr;
    UINT firstConstant = 0;
    UINT constantCount = 4096;
    std::shared_ptr<const FlatProjectionPreparedState> prepared;
    uint64_t revision = 0;
};
class FlatProjectionRuntime;

// Preflight allocates once. Prepare uploads only when source provenance, phase,
// or patch requests change. Never prepare while this buffer is bound by a scope.
// A refused preparation hides the old replacement until a new complete upload.
class FlatProjectionPrivateBuffer {
public:
    ~FlatProjectionPrivateBuffer() { invalidate(); }
    bool initialize(ID3D11DeviceContext1*, ID3D11Buffer* original, uint64_t identityGeneration);
    template<size_t N, size_t W>
    bool prepare(const FlatProjectionShadowBank<N,W>& bank,
                 const FlatProjectionPatchRequest* requests, uint32_t count,
                 const FlatProjectionJitter& jitter, uint32_t phase) {
        FlatProjectionShadowView view{};
        if (!bank.lookup(original_.Get(), identityGeneration_, view)) { invalidate(); return false; }
        return prepareSnapshot(view, requests, count, jitter, phase);
    }
    void invalidate() { ready_ = false; if (prepared_) prepared_->ready = false; }
    FlatPrivateProjectionBinding binding(FlatProjectionStage, UINT slot, UINT first = 0, UINT count = 4096) const;
    uint64_t uploads() const { return uploads_; }
private:
    bool prepareSnapshot(const FlatProjectionShadowView&, const FlatProjectionPatchRequest*,
                         uint32_t, const FlatProjectionJitter&, uint32_t);
    Microsoft::WRL::ComPtr<ID3D11DeviceContext1> context_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> original_, replacement_;
    std::unique_ptr<unsigned char[]> scratch_;
    std::shared_ptr<FlatProjectionPreparedState> prepared_;
    FlatProjectionCacheKey key_{};
    FlatProjectionJitter jitter_{};
    FlatProjectionPatchRequest requests_[8]{};
    uint64_t identityGeneration_ = 0, uploads_ = 0;
    uint32_t width_ = 0, count_ = 0;
    bool ready_ = false;
};

// Immediate-context owner thread only. A scope atomically validates all inputs
// before changing any binding, and restores exact buffers AND D3D11.1 ranges.
// Internal calls bypass observers, leaving their game-binding shadow unchanged.
class FlatProjectionBindingPlan {
public:
    static constexpr size_t kCapacity = 8;
    bool initialize(ID3D11DeviceContext1*, const FlatPrivateProjectionBinding*, size_t);
    // After successful uploads into the same resources, refresh only the tokens.
    // Failed preparation or a destroyed private-buffer owner rejects old plans.
    bool refreshPrepared();
private:
    friend class FlatProjectionRuntime;
    // Runtime-only live retarget: the caller has already established Context1
    // offsetting support and created each replacement from its tracked source
    // on this context's device. Revalidates bindings/tokens without queries or
    // allocations; never call while a scope from this plan is active.
    bool retargetPrepared(ID3D11DeviceContext1*, const FlatPrivateProjectionBinding*, size_t);
    bool idle() const { return activeScopes_ == 0; }
    friend class FlatProjectionBindingScope;
    struct Saved {
        FlatProjectionStage stage{};
        UINT slot = 0, first = 0, count = 0;
        Microsoft::WRL::ComPtr<ID3D11Buffer> original, replacement;
        std::shared_ptr<const FlatProjectionPreparedState> prepared;
        uint64_t revision = 0;
    };
    Microsoft::WRL::ComPtr<ID3D11DeviceContext1> context_;
    Saved saved_[kCapacity]{};
    size_t count_ = 0;
    mutable uint32_t activeScopes_ = 0;
};

// Build the plan at preflight; the hot scope performs only actual range/buffer
// validation, binding and restoration. No feature/descriptor/device queries.
class FlatProjectionBindingScope {
public:
    // The source plan must outlive this scope; the runtime owns cached plans
    // for its entire lifetime and will not retarget an active scope's plan.
    explicit FlatProjectionBindingScope(const FlatProjectionBindingPlan&);
    ~FlatProjectionBindingScope();
    FlatProjectionBindingScope(const FlatProjectionBindingScope&) = delete;
    FlatProjectionBindingScope& operator=(const FlatProjectionBindingScope&) = delete;
    bool active() const { return active_; }
private:
    Microsoft::WRL::ComPtr<ID3D11DeviceContext1> context_;
    const FlatProjectionBindingPlan* plan_ = nullptr;
    FlatProjectionBindingPlan::Saved saved_[FlatProjectionBindingPlan::kCapacity]{};
    size_t count_ = 0;
    bool active_ = false;
};
} // namespace edvr
