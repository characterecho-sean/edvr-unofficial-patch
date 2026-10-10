#pragma once
#include "engine_velocity.h"
#include "flat_mono_refusal.h"
#include <dxgiformat.h>
#include <cstdint>

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11ShaderResourceView;

namespace edvr {
// How the resolver isolates the game's pipeline state (flat_isolation_mode.h has the definition): the context state swap, or
// the explicit state capture a DXMT device gets.
enum class FlatContextIsolation : uint8_t;
// Mfx is MetalFX, the MetalFX temporal scaler invoked natively by DXMT
// (metal_fx_engine.h). It is a trained upscaler like Fsr and Dlss and consumes
// the same canonical inputs, so it shares their routing below; it exists in
// this enum only to be selected and named, never to route differently.
enum class FlatMonoResolveMode { Taa, Dlaa, Dlss, Fsr, Mfx };
// The mode as the logs and the HDR route's breadcrumbs (flat_hdr_crumbs.h) spell it.
inline const char* flatMonoResolveModeName(FlatMonoResolveMode mode) {
    return mode == FlatMonoResolveMode::Fsr ? "fsr" : mode == FlatMonoResolveMode::Dlss ? "dlss"
         : mode == FlatMonoResolveMode::Dlaa ? "dlaa" : mode == FlatMonoResolveMode::Mfx ? "mfx" : "taa";
}

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
    //
    // MFX lands here on purpose, not by accident: it is the fall-through
    // because Taa, Dlaa and Dlss have all returned above, and that is the
    // correct route for it. MetalFX is a trained upscaler with the same two
    // cases, and the one geometric fact DXMT's scaler requires is that the
    // evaluation size is at least the input size -- DXMT creates the scaler
    // with inputContentPropertiesEnabled and inputContentMinScale = 1.0
    // (dxmt/src/d3d11/d3d11_context_impl.cpp:5273), so a scale below 1.0 is
    // not available to it. This branch is exactly E >= R. The other end is
    // bounded already, and not by this function: flat_mono_frame.h refuses a
    // frame whose render width or height is under half the output, so E/R can
    // never exceed 2.0, and DXMT's inputContentMaxScale is 3.0. The scale MFX
    // can ask for is therefore inside what it is given on every frame that
    // reaches here. So MFX needs no negotiation of its own, and adding one
    // would be a second thing to keep right. Only the names differ, so the log
    // and the panel say mfx.
    const bool mfx = mode == FlatMonoResolveMode::Mfx;
    if (larger) {
        out.evalWidth = rW; out.evalHeight = rH; out.refused = false;
        out.name = mfx ? "mfx-native-aa-supersample" : "fsr-native-aa-supersample";
        return out;
    }
    out.evalWidth = dW; out.evalHeight = dH; out.refused = false;
    out.name = mfx ? (smaller ? "mfx-trained-upscale" : "mfx-native")
                   : (smaller ? "trained-upscale" : "trained-native");
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
    // instead of refusing history, whatever last frame's depth says (the blanket form). Only the flat runtime sets it, from
    // the verified menu copy; the VR world route never does. False (the default, and every frame outside that menu)
    // leaves the shader's arithmetic bit-identical to before the field.
    bool staticScene = false;
    // The steady-detail rule (always on in both callers, the VR world route and the flat profile on foot, which set it with no key; this
    // field is the resolver's contract and stays false until a caller sets it, and the rigs drive it both ways; design doc
    // section 82, the depth-validated steady detail): a pixel whose engine slot a later draw overdrew takes the camera term
    // instead of refusing its history ONLY where last frame's depth confirms the camera term -- the depth, in the best of the four
    // texels around the previous raster position, is within 1% (floor 1e-6, kFlatMonoStaleDepthRelative) of the depth this surface would
    // have had there had it not moved -- and is refused exactly as before wherever it does not. The check needs last frame's depth: TAA
    // keeps it already; the other backends get a second depth image, made on the first frame that asks, and from then on the depth the
    // backend is handed alternates between the two (never while this is false: those frames write the one image they always did). Masked
    // records, corrupt slots, the sky and the first-person pixels are untouched. staticScene (the menu's blanket form) wins when both are
    // set. False (the default) leaves every pixel, every resource and the prep's arithmetic exactly as before the field.
    bool steadyDetail = false;
    // The VR world route's refusal census and view (design doc section 82, stage 2 experiment build; flat_mono_refusal.h).
    // Both default to off, the flat profile never sets either, and a frame that asks for neither runs the prep and the finish
    // exactly as before (no class texture is made, bound or written, no census pass is dispatched).
    //   refusalCensus: count this frame's refused pixels by class. One resolve in kFlatMonoRefusalEvery that asks is sampled: the
    //     prep writes its class texture, a counting pass reduces it into a small buffer, the buffer is copied to a staging
    //     ring and read back a few frames later without waiting. flatMonoResolveTakeRefusalCensus() hands the sums over.
    //   refusalView: paint the prep's classes into the resolved image (the HDR route only, in the eye path's colours), so the
    //     refused pixels can be SEEN. Painted every frame it is set. A reset frame does neither.
    bool refusalCensus = false;
    uint32_t refusalView = 0;
    EngineVelocityViews engine{};
    uint64_t frame = 0;
    float deltaMs = 0;
    bool reset = true;
    FlatMonoResolveMode mode = FlatMonoResolveMode::Taa;
    uint32_t configuredDlssPreset = 0; // diagnostic attribution only
    // The HDR route (docs/design-flat-temporal-aa-2026-09-23.md section 81; flat_hdr_route.h). `color` is then the
    // game's HDR scene target H itself -- its shader view, R11G11B10_FLOAT at the render size -- not a tone-mapped
    // copy, and `depth`, the camera rows and the engine views are the same as on the copy route. The resolver copies H
    // into its private input, runs prep and the backend at E = R (the route refuses any other evaluation size) with
    // the backend in HDR mode, and writes the result BACK INTO H through a pixel-shader draw into a render-target view
    // it makes over H, so the game's own pass that reads H next sees the anti-aliased image. *output stays null: there
    // is nothing for the caller to swap into a binding. False, the default, is the copy route and every byte of it
    // unchanged.
    bool hdr = false;
    // Optional late-colour-overlay isolation. `color` remains the live HDR
    // destination and raw post-overlay image. The backend and prep consume
    // only cleanColor. The finish keeps the world under the coverage (the
    // backend's result where its history is trusted, the clean sample where
    // it is not) and adds what the overlays drew there, the raw sample minus
    // the clean one; a covered pixel whose history is refused is the raw
    // sample itself. A transparent part of an overlay adds nothing.
    ID3D11ShaderResourceView* cleanColor = nullptr;
    ID3D11ShaderResourceView* overlayCoverage = nullptr;
    // Conservative union of fragments rendered through a qualified alternate
    // camera on the flat HDR route. The mask is R8_UNORM at render size and
    // remains marked after later draws, even when they write identical depth.
    // Native HDR TAA uses current colour under this footprint. SDK modes also
    // require the qualified final-owner motion/depth contract below.
    // Null keeps the existing resolver path unchanged.
    ID3D11ShaderResourceView* untrustedCameraCoverage = nullptr;
    // Flat SDK input contract. The adapter certifies final camera ownership
    // and supplies actual animated foreground motion on the render grid.
    // RGBA32_FLOAT: xy = previous-current pixels with both raster phases out,
    // z = infinite reversed-Z device depth using foregroundDepthNear below,
    // w = 1 matched history, 2 newly seen geometry, 0 invalid or no fragment.
    // These inputs never alter native TAA or the VR first-person adapter.
    ID3D11ShaderResourceView* foregroundMotion = nullptr;
    bool foregroundRequired = false;
    bool foregroundQualified = false;
    bool foregroundResetRequired = false;
    uint64_t foregroundFrame = 0;
    // Zero means world near. A smaller positive common near keeps every
    // camera's device depth representable; the resolver scales world depth
    // only after computing world reprojection from the original raw depth.
    float foregroundDepthNear = 0;
    // The upscaler feature slot the backend evaluates on (dlaa.h, kUpscalerSlots; dlaa.cpp and fsr3_engine.cpp keep one
    // feature, one size key and one history per slot). 0 is the flat profile's and eye 0's -- the default, and every
    // caller before the VR world route. The VR world route passes 2 (vr_world_route.h, kVrWorldFeatureSlot), because its two
    // eyes own 0 and 1. The resolver's own continuity (its history, its TAA ping-pong) is one set: one caller per process at
    // a time, which the flat and VR profiles already are. A slot outside 0..kUpscalerSlots-1 refuses the frame
    // ("flat-resolve-invalid-slot") before anything is written.
    uint32_t slot = 0;
    // The first-person ("weapon") fold-in of the VR world route (docs/design-flat-temporal-aa-2026-09-23.md section 82).
    // The VR weapon-motion module rebuilds motion for first-person draws from their own animated vertices; the game's depth
    // texture carries a first-person stencil bit. Both come in as inputs of the prep kernel, both borrowed, BOTH OR NEITHER
    // (one without the other is treated as absent, counted in stats.firstPersonPartial). Null, the default, leaves the prep's
    // arithmetic bit-for-bit what it was.
    //   firstPersonMotion: R16G16B16A16_FLOAT, a Texture2D the size of the render (renderWidth x renderHeight). Per texel
    //     xy = previous minus current position in RENDER pixels, z = depth (fp16), w = 1 valid / 2 new-rejected / 0 uncovered.
    //   firstPersonStencil: the stencil plane view of the same depth texture, DXGI_FORMAT_X32_TYPELESS_G8X24_UINT, a
    //     Texture2D view; bit 0x10 is set where first-person draws wrote it.
    // A pair that fails validation (format, size, view dimension) is treated as absent, counted in stats.firstPersonRefused,
    // and named once in the log; it never refuses the frame. Where the stencil bit is set ("attached") the prep takes the
    // map's motion when the map is valid (w == 1, finite, its depth within max(|depth| * 0.0005, 3e-8) of the pixel's, its
    // previous position inside the frame, and the frame not a reset) and otherwise REJECTS the pixel's history; an attached
    // pixel never takes the engine or camera term. Every other pixel is treated exactly as without the inputs.
    ID3D11ShaderResourceView* firstPersonMotion = nullptr;
    ID3D11ShaderResourceView* firstPersonStencil = nullptr;
    // The phase term of the map's vector (stage 2, when the world and the first-person camera are jittered; the prep kernel's
    // "THE SEAM for a jittered world"). The map says previous minus current at the two frames' OWN raster phases, and the
    // backend wants both phases out of it. Read only when firstPersonMotion and firstPersonStencil are both valid:
    //   0 (the default) the map is used as it is given (the world is unjittered, or the map was built without a phase):
    //     bit-for-bit what the prep did before this field existed;
    //   1 the first-person camera carried the SAME phase as the world in both frames (the injector's role test, its
    //     Scene and FirstPerson roles): the map's vector gets (jitter.xy - jitter.zw) added, in render pixels, so the vector
    //     the backend sees excludes both phases, as the camera and engine terms do;
    //   2 the first-person camera's phase is not known to equal the world's in both frames (it was not injected in one of
    //     them): attached pixels REJECT their history this frame instead of taking a vector with an unknown term.
    // Any other value is treated as 2. The sign of mode 1 is proven on WARP against a map built from explicit positions
    // (tools\flat_mono_resolve_test).
    uint32_t firstPersonPhaseMode = 0;
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
    // The HDR route's plan (FlatMonoResolveFrame::hdr): the colour view is H's R11G11B10_FLOAT one, the resources
    // are the route's (fp16 output, an HDR input copy), and the fallback that must be ready is the pixel-shader one.
    bool hdr = false;
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
    // The HDR route's resolves and its pixel-shader spatial recoveries (written into H, so no output view).
    uint64_t hdrResolves = 0, hdrSpatial = 0;
    // The HDR route's calls (a resolve or a spatial recovery) that got through each step, for the route's 5 s census
    // (FlatHdrSteps, flat_hdr_route.h): the game's state swapped out, H copied into the private input, the prep dispatch,
    // the backend's success, the draw into H, the game's state put back. Counted whatever the breadcrumbs are doing.
    uint64_t hdrCaptured = 0, hdrCopied = 0, hdrPrepped = 0, hdrBackend = 0, hdrFinished = 0, hdrRestored = 0;
    // Which isolation each call used (flat_isolation_mode.h): the context state swap, which is every device but DXMT's, or the
    // explicit state capture (DXMT, or forced by a test rig); and the name of the one the renderer was last
    // initialised for ("swap" or "capture", static text; null before its first initialisation).
    uint64_t isolationSwaps = 0, isolationCaptures = 0;
    const char* isolation = nullptr;
    // The first-person inputs (FlatMonoResolveFrame::firstPersonMotion and firstPersonStencil): frames whose prep took them,
    // frames whose pair failed validation (firstPersonRefusal names the last reason, a static string, null until one has),
    // and frames that passed only one of the two (treated as absent, and not a refusal). A frame with neither counts nowhere.
    uint64_t firstPersonFrames = 0, firstPersonRefused = 0, firstPersonPartial = 0;
    const char* firstPersonRefusal = nullptr;
    // Of the frames counted in firstPersonFrames (their inputs bound), by FlatMonoResolveFrame::firstPersonPhaseMode: [0] mode 0
    // (the map's vector as given), [1] mode 1 (the two phases' difference added to it), [2] any other value (attached pixels
    // reject their history). The three sum to firstPersonFrames; a frame without bound inputs counts in none of them.
    uint64_t firstPersonPhaseFrames[3] = {};
    // The last resolve's EFFECTIVE reset (the requested one, or a lost history, a frame gap, an
    // invalid previous camera, a format change or a camera cut): what the pixel capture writes
    // as "reset" and what decides whether the frame is a live sample.
    bool lastReset = false;
};
FlatMonoResolveStats flatMonoResolveStats();
// Whether the last resolve reset (flatCaptureFrameLive, flat_pixel_capture_policy.h).
bool flatMonoResolveLastReset();
// The refusal census's samples read back since the last take, and starts over (FlatMonoResolveFrame::refusalCensus). Owner thread:
// it also polls the readback ring, with the resolver's own immediate context, before it hands the sums over. A census nobody asked
// for hands back zeros.
FlatMonoRefusalCensus flatMonoResolveTakeRefusalCensus();
// Owner thread, before rasterization. Validates planned dimensions/mode/source
// metadata, allocates renderer resources including the spatial fallback output,
// then checks external backend availability. A Ready result proves fallback
// output allocation, not size-specific NGX/FSR feature creation or frame input
// provenance. Invalid metadata causes no backend or renderer allocation call.
FlatMonoResolvePreflightResult flatMonoResolvePreflight(
    ID3D11Device*, ID3D11DeviceContext*, const FlatMonoResolvePreflight&);
// Owner immediate context only. Inputs borrowed for this call; successful output
// is AddRef'd and output-sized (null on the HDR route, FlatMonoResolveFrame::hdr, whose
// result is written back into the input target). The caller suppresses hook observations
// throughout this call. The game's pipeline state is isolated from the call's own work and its
// backends' and restored on every exit: by ID3D11DeviceContext1::SwapDeviceContextState (which
// needs D3D11.1) on every device but DXMT's, by the explicit state capture on DXMT's
// (flatMonoResolveSetIsolation). The caller supplies only jitter that was actually rendered into these inputs.
bool flatMonoResolve(ID3D11Device*, ID3D11DeviceContext*, const FlatMonoResolveFrame&,
                     ID3D11ShaderResourceView** output, const char** reason);
// The GPU census's timestamp pair (flat_cpu.h): begin is called just before the resolver's own
// dispatches and backend call, end when the call returns by any path. Owner thread. Null (the
// default, and in every rig) times nothing; installing them changes no command the resolver
// issues except the two queries.
using FlatMonoResolveSpanFn = void (*)(ID3D11DeviceContext*) noexcept;
void flatMonoResolveSetSpanHooks(FlatMonoResolveSpanFn begin, FlatMonoResolveSpanFn end);
// Recover an already rendered jittered frame after backend refusal. This
// spatial resolve uses no temporal history or SDK and borrows the same frame
// inputs; successful output is AddRef'd (on the HDR route it is written into H by a
// pixel-shader draw and *output stays null). It leaves history invalid. A normal
// flatMonoResolve call allocates this output before it asks a backend to run,
// so backend refusal reuses that allocation. Future nonzero-raster callers
// must preflight allocation before drawing; this API cannot recover from a
// device or allocation failure by itself.
bool flatMonoResolveSpatialFallback(ID3D11Device*, ID3D11DeviceContext*, const FlatMonoResolveFrame&,
                                    ID3D11ShaderResourceView** output, const char** reason);
// What the renderer's next initialisation is asked for (auto, swap or capture); test rigs only, production never calls it. Auto,
// the default, takes the swap everywhere but on a device that calls itself DXMT, where SwapDeviceContextState aborts the process
// and the explicit capture runs instead. flatMonoResolveReset() makes the next call initialise again, which a rig uses to change it.
void flatMonoResolveSetIsolation(FlatContextIsolation request);
// Owner thread: release renderer resources/history. Does not shut down shared SDKs.
void flatMonoResolveReset();
// Manual F10 diagnostic; owner-thread poll also runs when rendering is refused.
// Independent from history: arm/poll never change renderer state or parameters.
void flatMonoResolveArmPixels(uint64_t frame);
void flatMonoResolvePollPixels(ID3D11DeviceContext*,uint64_t frame);
// Owner thread: a refused/missing frame breaks only history, preserving resources.
void flatMonoResolveInvalidateHistory();
} // namespace edvr
