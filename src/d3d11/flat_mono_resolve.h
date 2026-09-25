#pragma once
#include "engine_velocity.h"
#include <dxgiformat.h>
#include <cstdint>

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11ShaderResourceView;

namespace edvr {
enum class FlatMonoResolveMode { Taa, Dlaa, Dlss, Fsr };
struct FlatMonoResolveFrame {
    ID3D11ShaderResourceView* color = nullptr;
    ID3D11ShaderResourceView* depth = nullptr;
    uint32_t renderWidth = 0, renderHeight = 0, outputWidth = 0, outputHeight = 0;
    float camera[6][4] = {}, previousCamera[6][4] = {}; // unjittered b1[270..275]
    // Actual raster phases in render pixels, positive right/down. Camera rows
    // and engine scene snapshots above remain raw and unjittered. Zero defaults
    // preserve the current runtime until projection coverage is qualified.
    float jitterX = 0, jitterY = 0, previousJitterX = 0, previousJitterY = 0;
    EngineVelocityViews engine{};
    uint64_t frame = 0;
    float deltaMs = 0;
    bool reset = true;
    FlatMonoResolveMode mode = FlatMonoResolveMode::Taa;
    uint32_t configuredDlssPreset = 0; // diagnostic attribution only
};
// Planned input metadata available before the game's next raster phase. This
// intentionally carries no frame resources: preflight can allocate the
// renderer/output and check SDK availability without trying to create a
// size-specific feature from incomplete or stale inputs.
struct FlatMonoResolvePreflight {
    uint32_t renderWidth = 0, renderHeight = 0, outputWidth = 0, outputHeight = 0;
    FlatMonoResolveMode mode = FlatMonoResolveMode::Taa;
    DXGI_FORMAT colorViewFormat = DXGI_FORMAT_UNKNOWN;
    DXGI_FORMAT depthViewFormat = DXGI_FORMAT_UNKNOWN;
    bool colorViewIsTexture2D = true, depthViewIsTexture2D = true;
    uint32_t colorMostDetailedMip = 0, depthMostDetailedMip = 0;
    uint32_t colorViewMipLevels = 1, depthViewMipLevels = 1;
    uint32_t colorResourceMipLevels = 1, colorArraySize = 1, colorSampleCount = 1;
    uint32_t depthResourceMipLevels = 1, depthArraySize = 1, depthSampleCount = 1;
};
enum class FlatMonoResolvePreflightStatus : uint8_t {
    Ready, InvalidMetadata, RendererUnavailable, FallbackUnavailable, BackendUnavailable
};
struct FlatMonoResolvePreflightResult {
    FlatMonoResolvePreflightStatus status = FlatMonoResolvePreflightStatus::InvalidMetadata;
    bool rendererReady = false;
    bool spatialFallbackReady = false;
    bool backendAvailable = false;
    // DLSS/DLAA/FSR feature creation still needs the validated live textures
    // and therefore remains a possible late failure after this preflight.
    bool backendFeatureCreationDeferred = false;
    const char* reason = "flat-preflight-not-run";
    bool readyForRasterJitter() const {
        return status == FlatMonoResolvePreflightStatus::Ready && rendererReady &&
               spatialFallbackReady && backendAvailable;
    }
};
// Owner-thread cumulative diagnostics. A full renderer reset preserves these
// counts so a session summary can expose repeated state or texture rebuilds.
struct FlatMonoResolveStats {
    uint64_t calls = 0;
    uint64_t initializations = 0, contextPointerMismatches = 0;
    uint64_t allocations = 0, fullResets = 0, invalidations = 0;
    uint64_t acceptedResets = 0, acceptedContinues = 0;
    uint64_t requestedResets = 0, lostHistory = 0, frameGaps = 0;
    uint64_t invalidPreviousCameras = 0, formatChanges = 0, cameraCuts = 0;
    uint64_t backendFailures = 0;
    uint64_t currentContinueRun = 0, longestContinueRun = 0;
};
FlatMonoResolveStats flatMonoResolveStats();
// Owner thread, before rasterization. Validates planned dimensions/mode/source
// metadata, allocates renderer resources including the spatial fallback output,
// then checks external backend availability. A Ready result proves fallback
// output allocation, not size-specific NGX/FSR feature creation or frame input
// provenance. Invalid metadata causes no backend or renderer allocation call.
FlatMonoResolvePreflightResult flatMonoResolvePreflight(
    ID3D11Device*, ID3D11DeviceContext*, const FlatMonoResolvePreflight&);
// Owner immediate context only. Inputs borrowed for this call; successful output
// is AddRef'd and output-sized. The caller suppresses hook observations throughout
// this call. D3D11.1 context-state isolation is required and restored on every exit.
// The caller supplies only jitter that was actually rendered into these inputs.
bool flatMonoResolve(ID3D11Device*, ID3D11DeviceContext*, const FlatMonoResolveFrame&,
                     ID3D11ShaderResourceView** output, const char** reason);
// Recover an already rendered jittered frame after backend refusal. This
// spatial resolve uses no temporal history or SDK and borrows the same frame
// inputs; successful output is AddRef'd. It leaves history invalid. A normal
// flatMonoResolve call allocates this output before it asks a backend to run,
// so backend refusal reuses that allocation. Future nonzero-raster callers
// must preflight allocation before drawing; this API cannot recover from a
// device or allocation failure by itself.
bool flatMonoResolveSpatialFallback(ID3D11Device*, ID3D11DeviceContext*, const FlatMonoResolveFrame&,
                                    ID3D11ShaderResourceView** output, const char** reason);
// Owner thread: release renderer resources/history. Does not shut down shared SDKs.
void flatMonoResolveReset();
// Manual F10 diagnostic; owner-thread poll also runs when rendering is refused.
// Independent from history: arm/poll never change renderer state or parameters.
void flatMonoResolveArmPixels(uint64_t frame);
void flatMonoResolvePollPixels(ID3D11DeviceContext*,uint64_t frame);
// Owner thread: a refused/missing frame breaks only history, preserving resources.
void flatMonoResolveInvalidateHistory();
} // namespace edvr
