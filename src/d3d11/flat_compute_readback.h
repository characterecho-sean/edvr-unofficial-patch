#pragma once
#include <atomic>
#include <cstdint>
#include <d3d11.h>

namespace edvr {
// Suppress discovery observations of this probe's own D3D calls. Nest-safe.
extern thread_local bool g_flatComputeInternal;
struct FlatComputeInternalScope {
    bool previous = g_flatComputeInternal;
    FlatComputeInternalScope() noexcept { g_flatComputeInternal = true; }
    ~FlatComputeInternalScope() { g_flatComputeInternal = previous; }
};

enum class FlatComputeReadbackKind { SparseBounds, WholeBounds, ConstantBuffer };
enum class FlatComputeReadbackStatus { Complete, Timeout, DeviceFailure, Cancelled };
struct FlatComputeReadbackToken {
    uint32_t sampleId = 0, jobId = 0;
    uint64_t frameEpoch = 0, sequence = 0;
};
struct FlatComputeReadbackRequest {
    FlatComputeReadbackKind kind = FlatComputeReadbackKind::SparseBounds;
    FlatComputeReadbackToken token{};
    ID3D11Buffer* source = nullptr;
    const uint64_t* offsets = nullptr; // Absolute buffer byte offsets; copied immediately.
    uint32_t offsetCount = 0;          // 1..12 chunks of 32 bytes, packed in result.
    uint32_t wholeBytes = 0;           // Whole modes: exactly source ByteWidth.
    uint64_t presentOrdinal = 0;
};
struct FlatComputeReadbackResult {
    FlatComputeReadbackToken token{};
    FlatComputeReadbackKind kind = FlatComputeReadbackKind::SparseBounds;
    FlatComputeReadbackStatus status = FlatComputeReadbackStatus::DeviceFailure;
    HRESULT hr = E_FAIL;
    const uint8_t* bytes = nullptr; // Valid only during callback; null on failure.
    uint32_t byteCount = 0;
};
using FlatComputeReadbackCallback = void (*)(const FlatComputeReadbackResult&, void*);

// Owner immediate-context thread only. Never call Cancel from Stop/DllMain.
// Queue storage intentionally survives process teardown; COM jobs retire here.
HRESULT flatComputeReadbackSchedule(ID3D11DeviceContext*, const FlatComputeReadbackRequest&);
void flatComputeReadbackPoll(ID3D11DeviceContext*, uint64_t presentOrdinal,
    FlatComputeReadbackCallback, void* user);
void flatComputeReadbackCancel(FlatComputeReadbackCallback, void* user);
extern std::atomic<bool> g_flatComputeReadbackPending;
inline bool flatComputeReadbackPending() noexcept {
    return g_flatComputeReadbackPending.load(std::memory_order_acquire);
}
}
