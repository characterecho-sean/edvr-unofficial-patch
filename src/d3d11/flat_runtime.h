#pragma once
#include <atomic>
#include <d3d11.h>
#include <dxgi.h>
#include "flat_compute_readback.h"
#include "flat_projection_scope.h"
#include "flat_substitution.h"
#include "flat_map_bounce.h"
#include "flat_mutation_diagnostic.h"
#include <optional>
namespace edvr {
struct FlatMapBounceD3DDriver {
    void retain(uintptr_t resource, uintptr_t context);
    void release(uintptr_t resource, uintptr_t context);
    uint64_t clockTicks();
    uint64_t ticksPerSecond();
    bool verify(void* real, const void* cached, size_t bytes);
};
using FlatMapBounce = flatmap::Runtime<FlatMapBounceD3DDriver>;
void flatRuntimeMapBouncePreMap(ID3D11Resource*);
void flatRuntimeMapBounceNoteMap(ID3D11DeviceContext*, ID3D11Resource*, UINT,
                                 D3D11_MAP, bool internal, bool success);
void* flatRuntimeMapBounceInstall(ID3D11DeviceContext*, ID3D11Resource*, UINT,
                                  D3D11_MAP, void* real);
FlatMapBounce::Lease flatRuntimeMapBounceBeginUnmap(ID3D11DeviceContext*, ID3D11Resource*);
bool flatRuntimeMapBounceSamplePending(uint32_t width);
void flatRuntimeMapBounceObserveCopy(uint32_t width, uint64_t ticks);
void flatRuntimeMapBounceTrackedMap(D3D11_MAP type);
void flatRuntimeMapBounceBankWrite(uint32_t width);
void flatRuntimeMapBounceRegistered(const D3D11_BUFFER_DESC& desc);
void flatRuntimeMapBounceNoteKind(ID3D11Resource*, bool buffer);
struct FlatMonoFrame;   // flat_mono_frame.h: the selector's result, passed by reference to the HDR route's treatment
extern std::atomic<bool> g_flatRuntimeLive;
inline bool flatRuntimeActive() { return g_flatRuntimeLive.load(std::memory_order_relaxed) && !g_flatComputeInternal; }
// Last qualified flat scene extent, published for the menu on any thread.
bool flatRuntimeNativeScale();
// The refusal state the F8 panel's settings warning follows (flat_standdown.h): true while the
// work is stood down for a chain-shape refusal that found an output copy (never for
// no-known-output-copy: a startup or loading frame has no final copy at all, and nothing in
// Elite's settings to turn off). It ends with the resume. reasonName is the selector's own name
// for the stand-down's current reason, standingDown says the work is paused (always, with the
// warning). Any thread; false with a treated, transiently refused or merely slow-to-start
// session, and with the mode off. The caller checks that a temporal mode is selected.
bool flatRuntimeStructuralRefusal(const char** reasonName, bool* standingDown);
// Whether the route's key (experimental.temporal_aa_before_post) is auto, so the game's final copy is admitted by its
// structure and Bloom and Depth of field never cause a refusal (flat_copy_structure.h, design section 83): published with the
// refusal state, so it is meaningful only while flatRuntimeStructuralRefusal is true. The F8 warning drops the Bloom and Depth
// of field advice while it holds.
bool flatRuntimeStructureAdmission();
// Whether EDVR's own TAA is selected and the scene renders above the output on both axes: with the refusal state, so meaningful
// only while flatRuntimeStructuralRefusal is true. Neither route resolves a chain the whitelist does not know there (the HDR
// route evaluates at the render size, the display-grid TAA at the output's), and the F8 warning says what to do about it.
bool flatRuntimeTaaAboveOutput();
// The scene's and the output's sizes as the final copy's admission last measured them (the R11G11B10F target the scene is drawn
// into, and the swap chain's), refreshed at every final copy that has a scene and true once one has since a resize last cleared
// it. The runtime's own measurement, never Elite's settings file. Any thread. The F8 warning and the stand-down line name the
// render size from it when the refusal is render-size-does-not-fit-output.
bool flatRuntimeSceneSizes(uint32_t* renderWidth, uint32_t* renderHeight, uint32_t* outputWidth, uint32_t* outputHeight);
// The upstream camera injector's read points into the phase machine: the
// current phase in render pixels and the validated resolve plan's render
// extent (w/h); applied is the machine's own applied count this frame.
void flatRuntimePhaseState(float* x, float* y, uint32_t* w, uint32_t* h, uint32_t* applied);
// The camera injector applied the phase at the source: fold it into the
// phase machine exactly as a scope application would.
void flatRuntimeNoteCameraApplied();
// Whether a legacy projection plan exists for the current frame (the
// ownership policy's legacyEligible input).
bool flatRuntimeLegacyPlanExists();
void flatRuntimePresent(IDXGISwapChain*, uint64_t frame, HRESULT, UINT flags);
void flatRuntimeBeforePresent();
void flatRuntimeResize();
void flatRuntimeConstantBuffers(UINT start, UINT count, ID3D11Buffer* const*);
void flatRuntimeUavs(UINT start, UINT count, ID3D11UnorderedAccessView* const*);
struct FlatRuntimeDispatchScope {
    std::optional<FlatProjectionBindingScope> projection;
    explicit FlatRuntimeDispatchScope(ID3D11DeviceContext*);
};
void flatRuntimeViewport(UINT, const D3D11_VIEWPORT*);
void flatRuntimeMap(ID3D11Resource*, D3D11_MAP, void*);
void flatRuntimeUnmap(ID3D11Resource*);
void flatRuntimeUpdate(ID3D11Resource*, const void*, const D3D11_BOX*);
void flatRuntimeWritten(ID3D11Resource*, FlatOverlayMutationOp provenance=FlatOverlayMutationOp::Written);
enum class FlatOverlayMutationRole : unsigned char { Unrelated, Hdr, Depth, Unknown };
inline FlatOverlayMutationRole flatRuntimeOverlayMutationRole(
    const void* resource, const void* hdr, const void* depth) {
    if (!resource) return FlatOverlayMutationRole::Unknown;
    if (resource == hdr) return FlatOverlayMutationRole::Hdr;
    if (resource == depth) return FlatOverlayMutationRole::Depth;
    return FlatOverlayMutationRole::Unrelated;
}
void flatRuntimeOverlayResourceMutation(ID3D11Resource*, FlatOverlayMutationOp,
                                       const FlatMutationDetails& = {});
void flatRuntimeOverlayViewMutation(ID3D11View*, FlatOverlayMutationOp,
                                   const FlatMutationDetails& = {});
void flatRuntimeOverlayForeignMutation();
void flatRuntimeUnknown();
void flatRuntimeOverlayUavBind(ID3D11DeviceContext*, UINT count,
                               ID3D11UnorderedAccessView* const*);
void flatRuntimeArmProjectionAudit(bool full = false);
void flatRuntimeCreateBuffer(ID3D11Buffer*, const void* initialData);
void flatRuntimeClearBindings();
void flatRuntimeReplayQueryBegin(ID3D11DeviceContext*, ID3D11Asynchronous*);
void flatRuntimeReplayQueryEnd(ID3D11DeviceContext*, ID3D11Asynchronous*);
// F10-only retained DSV clear chronology; called before the real clear.
void flatRuntimeWeaponFootprintClear(ID3D11DepthStencilView*, UINT clearFlags, UINT8 stencil);
void flatRuntimeWeaponFootprintBeforePresent(IDXGISwapChain*, UINT flags);
// A hooked call of the kind flat_substitution.h names has come, or the frame is ending: put the game's state back where
// engine motion's substitution is still bound (a lazy run of substituted producer draws), or forget it (the context
// lost its state). Every hook of that kind calls this before its real call; a load and a compare when nothing of EDVR's
// is bound, which is nearly always.
void flatRuntimeSubstitution(ID3D11DeviceContext* ctx, FlatSubstEvent event);
inline bool flatRuntimeNeedsActualDraw(bool footprintStarted, bool overlayPlanned, bool foregroundPlanned=false,
                                       bool untrustedPlanned=false) {
    return footprintStarted || overlayPlanned || foregroundPlanned || untrustedPlanned;
}
struct FlatRuntimeDrawScope {
    ID3D11DeviceContext* ctx = nullptr;
    ID3D11ShaderResourceView* original = nullptr;
    bool gameHadTarget6 = false;   // the game's own slot 6 was occupied under a substituted draw
    bool producer = false, replaced = false;
    bool drawCaptureStarted = false;
    void* drawPacket=nullptr;
    bool drawPacketExecuted=false;
    bool drawPacketOnly=false;
    bool drawPacketPriority=false;
    uint64_t drawPacketVs=0,drawPacketPs=0,drawPacketJitterBefore=0,drawPacketOverlayBefore=0;
    bool weaponFootprintStarted = false;
    bool overlayPlanned = false, overlayStarted = false, overlayEnded = false;
    bool overlayReplayPending = false;
    bool foregroundPlanned = false, foregroundStarted = false, foregroundEnded = false;
    bool untrustedPlanned = false, untrustedStarted = false, untrustedEnded = false;
    bool domainPlanned=false,domainStarted=false,domainForeign=false,domainPool=false;
    bool domainProtectedOverlay=false;
    bool domainPendingWorldNull=false;
    bool domainHdrWriter=false;
    bool domainBeforeWorld=false;
    bool domainSameWorld=false;
    ID3D11Texture2D* domainDepth=nullptr;
    uint64_t domainVs=0,domainPs=0;
    uint64_t domainCameraHash=0;
    uint64_t domainCbEpoch=0;
    uint32_t domainCbGeneration=0;
    unsigned domainWriterToken=0;
    unsigned domainFormat=0;
    unsigned domainWidth=0,domainHeight=0;
    float domainCamera[6][4]{};
    bool needsActualDraw() const { return drawPacket || domainPlanned || flatRuntimeNeedsActualDraw(weaponFootprintStarted, overlayPlanned,
                                                                     foregroundPlanned, untrustedPlanned); }
    ID3D11Texture2D* overlayHdr = nullptr;
    ID3D11DepthStencilView* overlayDsv = nullptr;
    uint32_t weaponFootprintSeq = 0;
    char weaponDrawKind = '?';
    uint32_t weaponDrawCount = 0, weaponDrawStart = 0, weaponDrawInstances = 0, weaponDrawStartInstance = 0;
    int32_t weaponDrawBase = 0;
    std::optional<FlatProjectionBindingScope> projection;
    FlatRuntimeDrawScope(ID3D11DeviceContext*, uint32_t instances,
                         char kind='?', uint32_t count=0, uint32_t start=0,
                         int32_t base=0, uint32_t startInstance=0);
    ~FlatRuntimeDrawScope();
    void beginActualDraw(ID3D11Buffer* indirectArgs=nullptr, UINT indirectOffset=0);
    void endActualDraw();
    bool recover(const char* reason);
    // The HDR route's treatment at its trigger draw (flat_hdr_route.h, design section 81): the resolve of the game's HDR
    // scene target H, written back into H, before the game's pass that reads it. `srvSlot` is the pixel-shader slot that
    // binds H for this draw. Declines quietly (the copy route then serves the frame) and never leaves H half-written.
    void treatHdr(const FlatMonoFrame& selected, uint32_t srvSlot);
};
} // namespace edvr
