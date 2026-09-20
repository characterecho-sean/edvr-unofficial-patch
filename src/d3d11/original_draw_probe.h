#pragma once

#include <windows.h>
#include <d3d11.h>
#include <cstdint>

namespace edvr {

constexpr uint32_t kOriginalDrawProbeUnknown = ~uint32_t(0);

enum class OriginalDrawKind : uint8_t {
    Draw,
    DrawIndexed,
    DrawInstanced,
    DrawIndexedInstanced,
    DrawAuto,
    DrawInstancedIndirect,
    DrawIndexedInstancedIndirect,
    Count
};

enum class OriginalDrawPass : uint8_t {
    Unknown,
    EyeColor,
    SourceColor,
    DepthOnly,
    OtherColor,
    Unbound,
    Count
};

// These are the unhooked context-vtable functions. They must remain valid
// until originalDrawProbeShutdown; the probe never reads a game's query.
using OriginalDrawProbeQueryMark = void (STDMETHODCALLTYPE *)(
    ID3D11DeviceContext*, ID3D11Asynchronous*);
using OriginalDrawProbeQueryData = HRESULT (STDMETHODCALLTYPE *)(
    ID3D11DeviceContext*, ID3D11Asynchronous*, void*, UINT, UINT);

struct OriginalDrawProbeQueryOps {
    OriginalDrawProbeQueryMark begin = nullptr;
    OriginalDrawProbeQueryMark end = nullptr;
    OriginalDrawProbeQueryData getData = nullptr;
};

struct OriginalDrawProbeInput {
    OriginalDrawKind kind = OriginalDrawKind::Draw;
    uint32_t count = kOriginalDrawProbeUnknown;
    uint32_t instances = kOriginalDrawProbeUnknown;
    uint32_t startIndex = 0;
    int32_t baseVertex = 0;
    uint32_t startInstance = 0;
    uint64_t originalVsHash = 0;
    uint64_t originalPsHash = 0;
    uint32_t verdict = 0;
    int8_t eye = -1;
    OriginalDrawPass pass = OriginalDrawPass::Unknown;
    bool modified = false;
    bool eyeSized = false;
    bool sourceSized = false;
    bool gameCountingActive = false;
    bool gameDisjointActive = false;
    bool gameQueryOverflow = false;
};

struct OriginalDrawProbeFrameInfo {
    uint64_t sourceFrame = 0;
    ID3D11Resource* source = nullptr; // Identity annotation only; never retained.
    uint32_t width = 0;
    uint32_t height = 0;
};

struct OriginalDrawProbeTicket {
    uint32_t slot = ~uint32_t(0);
    uint32_t generation = 0;
    explicit operator bool() const noexcept { return slot != ~uint32_t(0); }
};

struct OriginalDrawProbeSnapshot {
    bool enabled = false;
    bool bound = false;
    bool collecting = false;
    bool draining = false;
    uint64_t window = 0;
    uint64_t frames = 0;
    uint64_t originalCalls = 0;
    uint64_t planned = 0;
    uint64_t targetMisses = 0;
    uint64_t submitted = 0;
    uint64_t ready = 0;
    uint64_t zeroSamples = 0;
    uint64_t nonzeroSamples = 0;
    uint64_t predicated = 0;
    uint64_t gameCounting = 0;
    uint64_t gameDisjoint = 0;
    uint64_t queryOverflow = 0;
    uint64_t noRaster = 0;
    uint64_t noWrite = 0;
    uint64_t unsupported = 0;
    uint64_t ringFull = 0;
    uint64_t notIssued = 0;
    uint64_t pipelineReady = 0;
    uint64_t pipelineInvalid = 0;
    uint64_t timedReady = 0;
    uint64_t timingUnavailable = 0;
    uint64_t timingInvalid = 0;
    uint64_t expired = 0;
    uint64_t calibrations = 0;
    uint64_t invalid = 0;
    uint64_t pending = 0;
    uint64_t bucketOverflow = 0;
    uint64_t commandLists = 0;
    uint64_t stencilFailOps = 0;
    uint64_t stencilDepthFailOps = 0;
    uint64_t uavBound = 0;
    uint64_t streamOutputBound = 0;
    uint64_t geometryShaderBound = 0;
    uint64_t hullShaderBound = 0;
    uint64_t domainShaderBound = 0;
    uint64_t targetedCalls = 0;
    uint64_t targetedCandidates = 0;
    uint64_t targetedFamilySamples = 0;
    uint64_t cohortReseeds = 0;
    uint64_t populationDrift = 0;
    uint64_t identityUnsupported = 0;
    uint64_t identityOver32 = 0;
    uint64_t identityIneligibleLayout = 0;
    uint64_t identityMissingPool = 0;
    uint64_t identityMissingInstanceBuffer = 0;
    uint64_t identityInvalidRecord = 0;
    uint64_t identityReadbackFailed = 0;
    uint64_t identityExpired = 0;
    uint64_t identitySkinned = 0;
    uint64_t recurrenceSamePayload = 0;
    uint64_t recurrencePayloadChanged = 0;
    uint64_t recurrenceIdChanged = 0;
    uint64_t recurrenceModelChanged = 0;
    uint64_t recurrencePoseFieldsChanged = 0;
    uint64_t recurrenceOtherModelFieldsChanged = 0;
    uint64_t recurrenceIdOnlyChanged = 0;
    uint64_t recurrenceBindingChanged = 0;
    uint64_t recurrenceGaps = 0;
    uint64_t recurrenceOutOfOrder = 0;
    uint64_t recurrenceZeroToVisible = 0;
    uint64_t recurrenceVisibleToZero = 0;
};

bool originalDrawProbeBind(
    ID3D11Device*, ID3D11DeviceContext*, const OriginalDrawProbeQueryOps&) noexcept;
void originalDrawProbeConfigure(bool enabled, bool targeted = false) noexcept;

// Called from the input-layout creation hook. Targeted identity capture accepts
// only the exact per-instance uint2 layout used by the three diagnosed families.
void originalDrawProbeRememberLayout(ID3D11InputLayout*,
    const D3D11_INPUT_ELEMENT_DESC*, UINT, uint64_t vertexShaderHash) noexcept;

// Call for every original native draw. Broad mode chooses three prior-frame
// population ordinals. Targeted mode uses metadata to count family-local
// ordinals and follows one held ordinal per exact material family for a
// 24-frame cohort; unknown and modified draws remain population-only calls.
bool originalDrawProbeSelect(ID3D11DeviceContext*,
    const OriginalDrawProbeInput* metadata = nullptr) noexcept;

// Call Begin only after Select returned true and selected-only metadata is
// available. End must follow the exact raw native draw call when Ticket is set.
OriginalDrawProbeTicket originalDrawProbeBegin(
    ID3D11DeviceContext*, const OriginalDrawProbeInput&) noexcept;
void originalDrawProbeEnd(
    ID3D11DeviceContext*, OriginalDrawProbeTicket, bool originalIssued) noexcept;

// Polls bounded pending work, closes/reports finite windows, and publishes the
// previous broad or per-family population for the next selection/cohort plan.
void originalDrawProbeFrame(
    ID3D11DeviceContext*, const OriginalDrawProbeFrameInfo&) noexcept;
void originalDrawProbeExecuteCommandListNote(ID3D11DeviceContext*) noexcept;
OriginalDrawProbeSnapshot originalDrawProbeSnapshot() noexcept;
// True only while this module is issuing shared GpuTimer query traffic through
// the hooked context. Hooks still forward it, but must not classify it as game.
bool originalDrawProbeInternalQuery() noexcept;

// Owner-context shutdown closes/reset shared timers. A null context is
// quiescent Release-only cleanup.
void originalDrawProbeShutdown(ID3D11DeviceContext* = nullptr) noexcept;

} // namespace edvr
