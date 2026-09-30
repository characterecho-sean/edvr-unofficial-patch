#pragma once
#include "engine_velocity.h"
#include <dxgiformat.h>
#include <cstdint>

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11ShaderResourceView;

namespace edvr {
enum class FlatMonoResolveMode { Taa, Dlaa, Dlss, Fsr };

// The three sizes of the staged program's gate 2 (docs/design-flat-temporal-aa-2026-09-23.md
// section 72): the game's render size R, the temporal evaluation size E and
// the present size D, with the effective route named honestly -- refused
// pairings refuse, they do not silently substitute. The resolve's own
// refusals use failReason verbatim (flat_mono_resolve.cpp).
struct FlatResolveRoute {
    uint32_t evalWidth = 0, evalHeight = 0;   // E; zero when refused
    const char* name = "invalid";
    const char* failReason = "flat-resolve-invalid-route";  // stable refusal token
    bool refused = true;
};
inline FlatResolveRoute flatResolveRoute(FlatMonoResolveMode mode,
                                         uint32_t rW, uint32_t rH, uint32_t dW, uint32_t dH) {
    FlatResolveRoute out{};
    if (!rW || !rH || !dW || !dH) return out;
    const bool smaller = rW < dW || rH < dH, larger = rW > dW || rH > dH;
    if (mode == FlatMonoResolveMode::Taa) {
        // The current TAA evaluates on the display grid: allocation, dispatch
        // and shader indexing all run at D, sampling render-sized input -- a
        // fused display-grid TAA, honestly named. The agreed render-grid TAA
        // with one explicit R -> D conversion is the deferred gate-2 design
        // step; the route reports what actually runs today (gate-2 review G2-2).
        out.evalWidth = dW; out.evalHeight = dH; out.refused = false;
        out.name = !smaller && !larger ? "taa-native"
                 : larger ? "taa-display-grid-down" : "taa-display-grid-up";
        return out;
    }
    if (mode == FlatMonoResolveMode::Dlaa) {
        if (smaller) {
            out.name = "dlaa-requires-native";
            out.failReason = "flat-dlaa-requires-native-render-size";
            return out;
        }
        out.evalWidth = rW; out.evalHeight = rH; out.refused = false;
        out.name = larger ? "dlaa-supersample" : "dlaa-native";
        return out;
    }
    if (mode == FlatMonoResolveMode::Dlss) {
        if (larger) {
            // The honest NVIDIA reading of "DLSS" with supersampling: DLSS at
            // 100% is DLAA, so evaluate at R and let the game's own copy
            // downsample E = R to D. One final scaling step, nothing hidden.
            out.evalWidth = rW; out.evalHeight = rH; out.refused = false;
            out.name = "dlss-as-dlaa-supersample";
            return out;
        }
        out.evalWidth = dW; out.evalHeight = dH; out.refused = false;
        out.name = smaller ? "trained-upscale" : "trained-native";
        return out;
    }
    // FSR: Native AA is the 1.0x case of the same upscaler (equal render and
    // upscale sizes, already exercised on the R == D route), so supersampling
    // mirrors NVIDIA: evaluate at E = R and let the game's copy downsample.
    if (larger) {
        out.evalWidth = rW; out.evalHeight = rH; out.refused = false;
        out.name = "fsr-native-aa-supersample";
        return out;
    }
    out.evalWidth = dW; out.evalHeight = dH; out.refused = false;
    out.name = smaller ? "trained-upscale" : "trained-native";
    return out;
}
struct FlatMonoResolveFrame {
    ID3D11ShaderResourceView* color = nullptr;
    ID3D11ShaderResourceView* depth = nullptr;
    uint32_t renderWidth = 0, renderHeight = 0, outputWidth = 0, outputHeight = 0;
    // Optional negotiated evaluation size override (gate 2 step 4): nonzero
    // overrides the route's default E. Only the driver sets it, from the
    // vendor's queried ranges; the resolver uses it only when the route
    // itself is not refused.
    uint32_t evalWidth = 0, evalHeight = 0;
    float camera[6][4] = {}, previousCamera[6][4] = {}; // b1[270..275], unjittered unless rowsJitter* below says otherwise
    // Actual raster phases in render pixels, positive right/down. Camera rows
    // and engine scene snapshots above remain raw and unjittered. Zero defaults
    // preserve the current runtime until projection coverage is qualified.
    float jitterX = 0, jitterY = 0, previousJitterX = 0, previousJitterY = 0;
    // The raster phase the ROWS THEMSELVES carry, same unit and sign as the phases
    // above. Nonzero only when the game derived b1[270..275] from a jittered
    // frustum (the upstream camera injector, flat_camera_phase.h): rows 0..3 then
    // hold x += ndcX*w, y += ndcY*w, and the resolver removes it from camera,
    // previousCamera and the engine's scene snapshots before any reprojection.
    // Zero (the default, and every path before the C3 wiring) leaves every row
    // untouched and the shader's arithmetic bit-identical to before the field.
    // On a reset the previous rows are the current rows, so previous is ignored.
    float rowsJitterX = 0, rowsJitterY = 0, previousRowsJitterX = 0, previousRowsJitterY = 0;
    // The 3D main menu (2026-09-29): the frame's contract came through the verified menu HDR copy, so
    // the scene is a ship on its pedestal and nothing moves but the camera. Only then does a pixel whose
    // engine slot was overdrawn by a draw that never wrote it (an unkeyed hull) take the camera term
    // instead of refusing history. False (the default, and every frame outside that menu) leaves the
    // shader's arithmetic bit-identical to before the field.
    bool staticScene = false;
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
    // Negotiated evaluation size override, same contract as the frame's: the
    // preflight allocates at the E the resolve will evaluate at (gate-2 review
    // F1), because the resolve's resource cache keys on E.
    uint32_t evalWidth = 0, evalHeight = 0;
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
    // The last resolve's EFFECTIVE reset (the requested one, or a lost history, a frame gap, an
    // invalid previous camera, a format change or a camera cut): what the pixel capture writes
    // as "reset" and what decides whether the frame is a live sample.
    bool lastReset = false;
};
FlatMonoResolveStats flatMonoResolveStats();
// Whether the last resolve reset (flatCaptureFrameLive, flat_pixel_capture_policy.h).
bool flatMonoResolveLastReset();
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
