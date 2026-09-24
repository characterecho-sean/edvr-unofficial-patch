#pragma once

#include "flat_projection_scope.h"
#include <d3d11_1.h>
#include <wrl/client.h>
#include <cstdint>

namespace edvr {

// The caller supplies only exact shader-qualified contracts. This module does
// not infer shader admission or decide whether a frame may use nonzero jitter.
struct FlatProjectionRuntimeRequest {
    FlatProjectionStage stage = FlatProjectionStage::Vertex;
    UINT slot = 0;
    ID3D11Buffer* original = nullptr;
    UINT firstConstant = 0;
    UINT constantCount = 4096;
    FlatProjectionPatchRequest patches[8]{};
    uint32_t patchCount = 0;
};

enum class FlatProjectionRuntimeRefusal : uint8_t {
    None, WrongThread, NoContext1, NoCapacity, UnknownBuffer, MissingFullWrite,
    UnsupportedRange, BindingMismatch, InvalidRecipe, PrivateFailure, PlanFailure
};
struct FlatProjectionRuntimeStatus {
    FlatProjectionRuntimeRefusal last = FlatProjectionRuntimeRefusal::None;
    uint64_t refusals[11]{};
    uint64_t fullWrites = 0, invalidations = 0, initialWrites = 0;
    uint64_t preflights = 0, prepared = 0, zeroPhaseReady = 0;
    uint64_t coldQueued = 0, coldCompleted = 0, coldStale = 0;
    uint64_t coldFailed = 0, coldPending = 0, coldTimeouts = 0;
};

class FlatProjectionRuntime {
public:
    // initialize adopts the current thread as owner. Earlier or foreign
    // CreateBuffer calls cannot mutate the unsynchronized shadow bank.
    bool initialize(ID3D11DeviceContext* context);
    void reset();
    void enableColdReadback(bool enabled);
    void pollColdReadbacks(); // owner Present; never flushes or blocks on Map
    bool observeCreateBuffer(ID3D11Buffer* buffer, const void* initialData);
    void observeMap(ID3D11Resource* resource, D3D11_MAP type, const void* bytes);
    void observeUnmap(ID3D11Resource* resource);
    void observeUpdate(ID3D11Resource* resource, const void* bytes, const D3D11_BOX* box);
    void invalidate(ID3D11Resource* resource);
    void invalidateAll();

    // Owner-thread diagnostic copy from a current complete shadow. Never
    // exposes retained byte pointers; missing/stale/ranged data leaves out alone.
    bool copyConstants(ID3D11Buffer*, uint32_t byteOffset, uint32_t byteCount, void* out);

    // Preflight is required before a nonzero raster phase. Warm calls may
    // allocate private buffers and a structural binding plan. With allocation
    // disabled, only an existing topology and private buffers may be retargeted
    // to the current phase; no descriptors/devices are queried or cold reads queued.
    // Prepare requires the exact most recently preflighted recipe and never allocates.
    bool preflight(const FlatProjectionRuntimeRequest* requests, uint32_t count,
                   const FlatProjectionJitter& jitter, uint32_t phase,
                   bool allowAllocation = true);
    const FlatProjectionBindingPlan* prepare(const FlatProjectionRuntimeRequest* requests,
                                             uint32_t count, const FlatProjectionJitter& jitter,
                                             uint32_t phase);
    FlatProjectionRuntimeStatus status() const { return status_; }
private:
    static constexpr uint32_t kBuffers = 64, kPlans = 32;
    static constexpr uint32_t kColdPending = 8, kColdAttempts = 16, kColdMaxAge = 120;
    struct Tracked {
        Microsoft::WRL::ComPtr<ID3D11Buffer> buffer;
        uint64_t generation = 0;
        uint32_t width = 0;
        bool mapped = false, promoted = false, privateReady = false, pending = false;
        bool mutationOverflow = false;
        uint64_t mutationSerial = 1;
        const void* mapBytes = nullptr;
        FlatProjectionPrivateBuffer privateBuffer;
    };
    struct CachedPlan {
        bool used = false;
        FlatProjectionRuntimeRequest requests[FlatProjectionBindingPlan::kCapacity]{};
        uint32_t count = 0, phase = 0;
        FlatProjectionJitter jitter{};
        FlatProjectionBindingPlan plan;
    };
    struct ColdReadback {
        bool used = false;
        Microsoft::WRL::ComPtr<ID3D11Buffer> source, staging;
        Microsoft::WRL::ComPtr<ID3D11Query> query;
        uint64_t generation = 0, mutationSerial = 0;
        uint32_t width = 0, age = 0;
    };
    FlatProjectionShadowBank<kBuffers> shadows_;
    Tracked tracked_[kBuffers]{};
    CachedPlan plans_[kPlans]{};
    ColdReadback cold_[kColdPending]{};
    Microsoft::WRL::ComPtr<ID3D11DeviceContext1> context_;
    DWORD owner_ = 0;
    uint64_t nextGeneration_ = 1;
    uint32_t coldAttempts_ = 0;
    bool coldEnabled_ = false;
    FlatProjectionRuntimeStatus status_{};

    bool owner() const;
    bool refuse(FlatProjectionRuntimeRefusal reason);
    Tracked* find(ID3D11Resource* resource);
    Tracked* track(ID3D11Resource* resource);
    void discard(Tracked& entry);
    void mutate(Tracked& entry);
    bool queueCold(Tracked& entry);
    void clearCold(ColdReadback& item);
    bool sameRecipe(const CachedPlan& plan, const FlatProjectionRuntimeRequest* requests,
                    uint32_t count, const FlatProjectionJitter& jitter, uint32_t phase) const;
    bool sameTopology(const CachedPlan& plan, const FlatProjectionRuntimeRequest* requests,
                      uint32_t count) const;
    CachedPlan* findPlan(const FlatProjectionRuntimeRequest* requests, uint32_t count,
                         const FlatProjectionJitter& jitter, uint32_t phase);
    CachedPlan* findTopology(const FlatProjectionRuntimeRequest* requests, uint32_t count);
    void invalidatePreparedPlans();
    bool actualBindings(const FlatProjectionRuntimeRequest* requests, uint32_t count);
};

} // namespace edvr
