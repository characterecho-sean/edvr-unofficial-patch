#include "temporal_shader_bytecode.h"
// fix.ui_quality -- the UI layer half. ui_layer.h says what it is and why;
// ui_layer_math.h holds its arithmetic and ui_layer_shaders.h its composite,
// both shared with tools/ui_quality_test; ui_panel_scale.* is the other half
// (every render-to-texture panel at the target's size, by the game's own panel
// formula), ui_surfaces.* its instruments; docs/ui-layer-2026-09-23.md is the
// design as built.
//
// THREADS. Everything here runs on the game's render thread: the draws, the
// door (native_temporal.cpp and native_sharpen.cpp run inside the game's
// Submit, which the runtime serves synchronously while the game waits), and
// the frame boundary (Present). The one other thread that matters -- the XR
// owner, which may open a frame -- is only read, under native_temporal's own
// lock, through nativeTemporalDrawJitter.
#include "ui_layer.h"

#include "ui_layer_math.h"
#include "ui_scene_composites.h"  // the census of the interface composites left in the scene, and its line
#include "ui_maps_math.h"   // the on-foot maps gate: the key, the step, the door's predicate, the lines
#include "ui_holo_remap.h"
#include "ui_layer_draw_timing.h"
#include "ui_layer_shaders.h"
#include "ui_layer_coverage.h"
#include "ui_layer_seed.h"  // the Seeder: the game's depth-stencil at the layer's size
#include "ui_layer_seed_census.h"
#include "ui_layer_seed_timing.h"
#include "draw_state_describe.h"  // viewName, describeBlend/describeDs, shapeOf, dsStateOf
#include "tonemap_admit.h"  // the tonemap draw's structural admission, shared with the census

#include "binding_shadow.h"
#include "depth_probe.h"   // depthProbeDrawsAtSize: the world-screen gate's own count
#include "device_hook.h"   // deviceHookHmdQuality, for the configure line
#include "gpu_timing.h"
#include "gpu_census.h"    // issue #38: the per-feature GPU cost census
#include "graphics_runtime.h"
#include "journal_watch.h" // the on-foot gate's reading: Status.json's Flags2 bit 0, GuiFocus
#include "screen_motion.h" // screenMotionLive: the maps gate decides by screen motion's naming only while it runs
#include "shader_swap.h"
#include "ui_depth.h"      // uiDepthEyeOfTarget: the eye, by the pass's own table
#include "ui_panel_scale.h" // the engine-side panel sizing, configured and ticked with the key
#include "orbital_width.h"  // the orbit lines' half-width at the same factor: its 30 s line follows the panels'
#include "supercruise_bars.h"  // the supercruise bars' private geometry-shader pass: its readiness and its 30 s line
#include "ui_surfaces.h"   // the instruments: the target, the frame count, the atlas line
#include "vscreen.h"       // the raw OM/RS entry points, vScreenIsEyeSized, vScreenPanelSize
#include "vr_world_mips.h"  // the VR world route: the mipped screen and its sampler (the re-issue's inputs)
#include "vr_world_route.h" // ...and what the route publishes: may the layer take, which eye was taken

#include "../common/config.h"
#include "../common/guard.h"
#include "../common/log.h"
#include "../common/periodic_work.h"
#include "../common/runtime_profile.h"  // runtimeFlatProfile: the flat profile's half of the key (panels, the flat layer)
#include "../common/temporal_mode.h"

#include <windows.h>

#include <d3d11_1.h>
#include <wrl/client.h>

#include <cmath>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>

namespace edvr {

namespace detail {
bool g_uiLayerLive = false;
bool g_uiLayerWatching = false;
bool g_uiSeedDiagnostics = false;
bool g_uiLayerRedirecting = false;
bool g_uiLayerIssueBlocked = false;
bool g_uiLayerCrispOn = false;      // the HDR HUD take/re-issue armed this frame (with the layer)
bool g_uiLayerCrispPending = false; // a tonemap draw was admitted; its re-issue follows its draw
bool g_uiLayerWorldReissue = false; // the VR world route: the screen composite just decided is re-issued after the game's draw
bool g_uiLayerMapsOn = false;       // the on-foot maps gate: the layer and screen motion live -- naming decides the gate
uint64_t g_uiLayerGateFrame = 0;    // frame boundaries the layer has seen: what a naming is attributed to (ui_layer.h)
uint64_t g_uiLayerNamedAt = ~0ull;  // the gate frame in which a draw last named the screen's source (never: ~0)
}  // namespace detail

namespace {

template <class T>
using Ptr = Microsoft::WRL::ComPtr<T>;

constexpr uint64_t kTotalsMs = 30000;
constexpr uint32_t kWatchPerFrame = 64;
// First-seen lines are one per (family, decision, vs, ps): Phase 3's eight
// take hologram hashes share the one kHoloGeneric row, so each needs its
// own entry here (48 barely covered the named families and their refusals).
constexpr uint32_t kMaxFamilyLines = 80;
constexpr uint32_t kMaxAfterLines = 16;
// The crisp-HUD missing-consumer deadline (review R1): this many consecutive
// content-frames with no tonemap publication stands the HDR path down. The
// tonemap runs every frame when the path works, so a streak this long says
// the consumer is not coming; ~a third of a second at 90 Hz.
constexpr uint32_t kCrispHdrMissFrames = 30;

// ------------------------------------------------------------ configuration

float g_target = 0.0f;     // 0 off, 1.0 (the file's 100), 1.25 (125)
bool g_temporal = false;   // a temporal mode is on (the layer's door exists)
constexpr bool g_jitterAsShipped = true;  // the eye jitter is always as shipped
bool g_stoodDown = false;
bool g_crispStoodDown = false;  // the HDR HUD path alone (a failure of its own); the LDR take stays
std::string g_keyText = "?";
bool g_keyNoted = false;
bool g_aliasNoted = false;  // the old spelling's note, once a session

// The layer's size target is fix.ui_quality's value; the HDR HUD layer and
// the 8-bit layer it tonemaps into share a size by construction (the HDR
// path arms with the layer -- the crisp-HUD take is fix.ui_quality's, not
// a key of its own).
float layerTarget() {
    return g_target > 0.0f ? g_target : 1.0f;
}

void refreshLive() {
    // ui_layer_math.h's uiLayerLiveFor: the one statement of "the layer is live", which the VR world route reads
    // through uiLayerLiveForWorldRoute (tools/ui_quality_test pins it equal to the expression it replaced).
    detail::g_uiLayerLive = uiLayerLiveFor(g_target, g_temporal, g_jitterAsShipped, g_stoodDown);
    detail::g_uiLayerCrispOn = detail::g_uiLayerLive && !g_crispStoodDown;
}

// The HDR HUD path fails alone: the cockpit HUD families (the holo panels,
// the flight HUD, the target sprite, the holograms) stay in the game's
// frame exactly as stock (their kHdrTarget refusal), and the LDR take is
// untouched.
// The flat profile backs off instead of standing down for the session (2026-10-09 11:32 flight: one missed tonemap on
// the first take ended the session's layer, while route changes, menus and frames with no 3D scene are normal in flat).
// The back-off lifts itself at the frame boundary after kFlatBackOffMs, with a line each way. VR is unchanged.
constexpr uint64_t kFlatBackOffMs = 30000;
uint64_t g_flatBackOffUntilMs = 0;
uint32_t g_flatBackOffs = 0;
// Escalating (2026-10-09 12:31 flight: one miss cost 30 s of the old smear): 2 s, 4, 8, 16, then 30 s, starting over
// at 2 s once the layer has run five minutes clean since its last back-off.
constexpr uint64_t kFlatBackOffFirstMs = 2000, kFlatBackOffCleanMs = 300000;
uint64_t g_flatBackOffLastMs = 0;
uint32_t g_flatBackOffStep = 0;
void flatBackOff(const char* what, const char* why) {
    const uint64_t now = GetTickCount64();
    if (g_flatBackOffLastMs && now - g_flatBackOffLastMs >= kFlatBackOffCleanMs) g_flatBackOffStep = 0;
    uint64_t ms = kFlatBackOffFirstMs << (g_flatBackOffStep < 4 ? g_flatBackOffStep : 4);
    if (ms > kFlatBackOffMs) ms = kFlatBackOffMs;
    ++g_flatBackOffStep;
    g_flatBackOffLastMs = now;
    g_flatBackOffUntilMs = now + ms;
    ++g_flatBackOffs;
    Log::get().note("ui quality: flat layer: %s backs off for %llu s (back-off %u this session) -- %s. The cockpit HUD is "
                    "drawn as it always was until it re-arms.",
                    what, static_cast<unsigned long long>(ms / 1000), g_flatBackOffs, why ? why : "a refusal");
}

void crispStandDown(const char* why) {
    if (g_crispStoodDown) return;
    g_crispStoodDown = true;
    refreshLive();
    if (runtimeFlatProfile()) {
        flatBackOff("the HDR HUD path", why);
        return;
    }
    Log::get().note(
        "ui quality: the HDR HUD path stands down for the rest of the session -- %s. The "
        "cockpit's holo panels, flight HUD, target sprite and holograms are drawn as they always "
        "were (the rest of the layer is unaffected). Turning fix.ui_quality off and on re-arms it.",
        why ? why : "a refusal");
}

void standDown(const char* why) {
    if (g_stoodDown) return;
    g_stoodDown = true;
    refreshLive();
    if (detail::g_uiLayerIssueBlocked) {
        Log::get().note("ui quality: layer stood down -- %s. Original hologram shader/b13 state remains "
                        "untrusted after two restoration attempts; the game's draws and replay on the owner "
                        "context are suppressed for the rest of this frame, and the frame boundary then puts "
                        "back whichever of the two is still EDVR's (the saved original references are retained "
                        "until then) and lifts the suppression. The layer itself stays stood down for the "
                        "session.", why ? why : "a restoration fault");
        g_flatBackOffUntilMs = 0;  // untrusted shader state: the flat profile does not re-arm from this one either
        return;
    }
    if (runtimeFlatProfile()) {
        flatBackOff("the layer", why);
        return;
    }
    Log::get().note(
        "ui quality: the layer stands down for the rest of the session -- %s. The UI goes into "
        "the game's frame as before (and gets the UI depth and reactive mask again); the "
        "panels stay as they are. Turning fix.ui_quality off and on re-arms it.",
        why ? why : "a refusal");
}

// ------------------------------------------------- the flat profile's mono adapter (flat_ui_layer.cpp)
//
// The flat profile has one picture, no native temporal channel and no vScreen eye size. Its adapter hands the shared
// machinery below what those two would: before each decision and each tonemap admission, the flat frame's number (the
// sequence the door and the layer key on), the raster phase that draw's camera carries in render pixels (where the
// game put its pixels, right and down, as nativeTemporalDrawJitter reports the eye's), and the scene's render size R
// (the "eye-sized" target). Eye 0 only. In the VR profiles none of it is read.
struct FlatDraw {
    bool valid = false;
    uint64_t seq = 0;
    float jx = 0.0f, jy = 0.0f;
    uint32_t renderW = 0, renderH = 0;
};
FlatDraw g_flatDraw;
int g_lastDecision = 0;  // the last uiLayerDecide's UiLayerDecision, for the flat adapter's refusal names

bool layerDrawJitter(uint32_t eye, uint64_t* seq, float* jx, float* jy, uint32_t* w, uint32_t* h) {
    if (!runtimeFlatProfile()) return nativeTemporalDrawJitter(eye, seq, jx, jy, w, h);
    if (!g_flatDraw.valid || eye != 0) return false;
    if (seq) *seq = g_flatDraw.seq;
    if (jx) *jx = g_flatDraw.jx;
    if (jy) *jy = g_flatDraw.jy;
    if (w) *w = g_flatDraw.renderW;
    if (h) *h = g_flatDraw.renderH;
    return true;
}

bool layerEyeSized(uint32_t w, uint32_t h) {
    if (!runtimeFlatProfile()) return vScreenIsEyeSized(w, h);
    return g_flatDraw.renderW && w == g_flatDraw.renderW && h == g_flatDraw.renderH;
}

// ------------------------------------------------------------------ per eye

// A layer's own depth-stencil target, seeded from the game's own (the
// Seeder) at the layer's size with the jitter cancelled, at the first tested
// draw of the frame (and again whenever the game writes its own buffer, or a
// later draw reads bits not seeded). The 8-bit layer has one; the crisp-HUD half's of fix.ui_quality
// HDR HUD layer has its own at its own size.
struct LayerDs {
    Ptr<ID3D11Texture2D> tex;
    Ptr<ID3D11DepthStencilView> dsv;
    uint32_t w = 0, h = 0;
    DXGI_FORMAT viewFmt = DXGI_FORMAT_UNKNOWN;
    uint64_t seq = 0;              // seeded for this frame (0: not, or stale)
    const void* source = nullptr;  // the game's depth texture it was seeded from
    uint8_t seededMask = 0;        // stencil bits seeded
    bool seededDepth = false;
};

struct Eye {
    UiCoverageCommandCache coverage;
    // The layer: R8G8B8A8_UNORM, cleared to (0, 0, 0, 1) at the first draw
    // of each frame -- premultiplied colour and transmittance.
    Ptr<ID3D11Texture2D> tex;
    Ptr<ID3D11RenderTargetView> rtv;
    Ptr<ID3D11ShaderResourceView> srv;
    uint32_t w = 0, h = 0;
    uint32_t basisW = 0, basisH = 0;  // the door size the layer was made for
    uint64_t seq = 0;            // the frame whose draws it holds
    uint32_t draws = 0;          // redirected into it for that frame
    uint64_t compositedSeq = 0;  // the last frame the door composited (or tried)
    const void* target = nullptr;  // the game's target those draws left (identity)
    // The frame whose 2D screen draw the VR world route re-issued into this layer (End of the re-issue): the eye
    // is the layer's alone that frame, and a draw the game left in its own eye image after it is lost (counted).
    uint64_t worldSeq = 0;
    // The frame whose 2D screen composite the on-foot maps gate gave the layer: TAKEN into this layer, not re-issued (ui_maps_math.h).
    // With nothing else drawn into an eye-sized target that frame, the layer holds the eye's whole picture and the door runs
    // layer-only for it (uiLayerDoorLayerOnly).
    uint64_t screenTakenSeq = 0;
    // The identity is still a link of the pre-UI chain the crisp re-issue opened
    // (the tonemap's output A, before the game's post pass carried it to B): a
    // small pass reading it carries it on, once (uiLayerFollowReader,
    // ui_layer_math.h). Cleared by that follow and by the first taken UI draw.
    bool chainOpen = false;

    // The per-channel transmittance a multiply leaves (made at the first
    // multiply, cleared to 1 at the first multiply of each frame).
    Ptr<ID3D11Texture2D> mTex;
    Ptr<ID3D11RenderTargetView> mRtv;
    Ptr<ID3D11ShaderResourceView> mSrv;
    uint32_t mW = 0, mH = 0;
    uint64_t mSeq = 0;

    // The 8-bit layer's depth-stencil target, for UI that TESTS depth or
    // stencil.
    LayerDs ds;
    // The copy of the game's depth-stencil the seed reads (shared by both
    // layers' seeds: it is the game's size, not the layer's).
    Ptr<ID3D11Texture2D> dsCopy;
    Ptr<ID3D11ShaderResourceView> dsCopyDepth, dsCopyStencil;
    uint32_t dsCopyW = 0, dsCopyH = 0;
    DXGI_FORMAT dsCopyFmt = DXGI_FORMAT_UNKNOWN;

    // the crisp-HUD half's of fix.ui_quality HDR HUD layer: R16G16B16A16_FLOAT at the 8-bit layer's
    // size (the door's output x the same target), holding the cockpit HUD
    // families' (holo panels, flight HUD, target sprite, holograms)
    // premultiplied radiance and transmittance in the layer's own
    // alpha convention, cleared at the first taken HUD draw of each
    // eye-frame. The game's tonemap draw, re-issued over the 8-bit layer
    // with this as its HDR source, tonemaps the HUD into the 8-bit layer's
    // colour; this layer's alpha then replaces the 8-bit layer's (the
    // coverage pass), and the door composite shows it as any other UI.
    Ptr<ID3D11Texture2D> hdrTex;
    Ptr<ID3D11RenderTargetView> hdrRtv;
    Ptr<ID3D11ShaderResourceView> hdrSrv;
    uint32_t hdrW = 0, hdrH = 0;
    uint64_t hdrSeq = 0;            // the frame whose HUD draws it holds
    uint32_t hdrDraws = 0;
    const void* hdrTarget = nullptr;  // the HDR target those draws left (identity)
    LayerDs hdrDs;                    // its own depth-stencil, seeded the same way
    uint64_t hdrToneSeq = 0;          // the frame the tonemap re-issue ran for
    // Consecutive content-frames no tonemap re-issue published (review R1's
    // frame deadline): counted beside hdrLost at the per-frame clear, reset
    // where a publication lands. The stand-down is at kCrispHdrMissFrames.
    uint32_t hdrMissStreak = 0;

    // The door.
    UiLayerDoorState door;
    const void* temporalOut = nullptr;  // the pass's output (identity), this frame
    uint64_t temporalOutSeq = 0;
    bool doorFromPass = false;          // this frame's door input was the pass's

    // The eye check: where the target the UI left was copied to this frame,
    // and what the game submitted for this eye (identities).
    const void* copiedTo = nullptr;
    uint64_t copiedSeq = 0;
    const void* submitted = nullptr;
    uint64_t submittedSeq = 0;

    // The composite's output: the frame's region, the frame's format.
    Ptr<ID3D11Texture2D> out;
    Ptr<ID3D11UnorderedAccessView> outUav;
    uint32_t outW = 0, outH = 0;
    DXGI_FORMAT outFmt = DXGI_FORMAT_UNKNOWN;
    // The view over the frame, cached on exactly that resource.
    void* frameRes = nullptr;
    Ptr<ID3D11ShaderResourceView> frameSrv;
    DXGI_FORMAT frameView = DXGI_FORMAT_UNKNOWN;
    // The copy-through, for a frame that refuses a shader view.
    Ptr<ID3D11Texture2D> copy;
    Ptr<ID3D11ShaderResourceView> copySrv;
    uint32_t copyW = 0, copyH = 0;
    DXGI_FORMAT copyFmt = DXGI_FORMAT_UNKNOWN;
};
Eye g_eye[2];

// ------------------------------------------------------------ the draw path

// The target Rtv0 names, once per binding generation.
struct TargetCache {
    uint32_t gen = 0;
    int kind = 0;
    ResourceInfo info;
    DXGI_FORMAT view = DXGI_FORMAT_UNKNOWN;
};
TargetCache g_tc;

// One decided draw at a time: forwardWithVerdict decides, then brackets each
// issue with Begin/End, on the render thread, before the next draw arrives.
struct Draw {
    bool decided = false, active = false;
    bool saved = false;    // the game's state is held below: restore it on any exit
    bool counted = false;  // this decision's draw is counted (a fallback re-issue is not)
    bool hdr = false;      // the crisp-HUD half of fix.ui_quality: the draw goes to the HDR HUD layer, not the 8-bit one
    bool reissue = false;  // the VR world route's re-issue of the 2D screen (the game's own draw lands in its eye): not a take
    int eye = -1;
    UiLayerFamily family = UiLayerFamily::kNone;
    uint64_t holoPsHash = 0;
    ID3D11PixelShader* holoOriginal = nullptr; // borrowed from the game's binding
    ID3D11PixelShader* holoPatched = nullptr;  // borrowed from the bounded cache
    ID3D11ShaderResourceView* holoDepthCheck = nullptr; // saved query ref, including a partial SEH getter
    bool holoRestoreOk = true; // failed restore must never replay a patched PS at stock coordinates
    uint64_t seq = 0;
    const void* targetRes = nullptr;
    uint32_t targetW = 0, targetH = 0;
    float jx = 0.0f, jy = 0.0f;  // this frame's jitter, in the target's pixels
    UiBlendShape shape = UiBlendShape::kRefused;  // as decided
    UiDsEffect ds;               // what it does with the game's depth-stencil
    bool rawDepthWritePotential = false; // before the original DSV's read-only mask
    uint8_t stencilRead = 0;     // its stencil read mask, for the seed
    // Saved at Begin, put back at End.
    ID3D11RenderTargetView* rtv[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
    ID3D11DepthStencilView* dsv = nullptr;
    D3D11_VIEWPORT vp[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] = {};
    UINT vpCount = 0;
    D3D11_RECT sc[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] = {};
    UINT scCount = 0;
    bool scissorSet = false;
    ID3D11BlendState* blend = nullptr;
    FLOAT factor[4] = {};
    UINT sampleMask = 0xFFFFFFFFu;
    // The write-back's own save (the game's targets, while its depth-only
    // re-issue runs with no colour target bound).
    ID3D11RenderTargetView* wbRtv[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
    ID3D11DepthStencilView* wbDsv = nullptr;
    bool wbActive = false;
    // The route's timers open across the game's own issue (the price, below):
    // a multiply's second issue, and a write-back's re-issue. -1: none.
    UiDrawRouteScope drawRoute;
    int wbRouteSlot = -1;
};
Draw g_draw;
// The SV_Position remap (ui_holo_remap.h): the two hologram sphere programs and, since 2026-09-30, the frosted
// base. The "holo" names are the machinery's history; every counter below is per program slot or a total.
ui_holo_remap::Cache g_holoCache;
ui_holo_remap::Binding g_holoBinding;
constexpr size_t kRemapPrograms = ui_holo_remap::kProgramCount;
uint64_t g_holoEligible = 0, g_holoPrepared = 0, g_holoTaken = 0, g_holoRefused = 0;
uint64_t g_holoTakenBy[kRemapPrograms]{}, g_holoRefusedBy[kRemapPrograms]{};
bool g_holoPrepareNoted[kRemapPrograms]{}, g_holoTakeNoted[kRemapPrograms]{},
    g_holoRefusalNoted[kRemapPrograms]{};
std::atomic<uint64_t> g_holoCaptureCalls{0}, g_holoCaptured{0};
std::atomic<bool> g_holoCaptureNoted[kRemapPrograms]{}, g_holoCaptureRefusalNoted[kRemapPrograms]{};
static_assert(ui_holo_remap::kVs == kHoloTargetSphere, "hologram classifier identity");
uint64_t g_lastRedirectSeq = 0;
bool g_familyEngaged[static_cast<size_t>(UiLayerFamily::kCount)] = {};
uint32_t g_watchBudget = kWatchPerFrame;

FaultBudget g_drawBudget("uiLayer.draw", 4);
FaultBudget g_compositeBudget("uiLayer.composite", 4);

// Converted blend states, keyed by the converted description (a handful).
struct BlendEntry {
    D3D11_BLEND_DESC desc{};
    Ptr<ID3D11BlendState> state;
};
BlendEntry g_blends[16];
uint32_t g_blendCount = 0;

// The seed's machinery: ui_layer_seed.h's Seeder (rig-tested in
// tools/ui_layer_seed_test/seed_test.cpp) on a deferred context, executed on
// the immediate one with its state restored.
std::unique_ptr<edvr_layer_seed::Seeder> g_seeder;
Ptr<ID3D11DeviceContext> g_deferred;
bool g_seederTried = false;
// Conservative across both eyes and LDR/HDR caches. A redirected depth
// writer may also change another cache's game source through raw write-back.
UiLayerPrivateDepthGuard g_privateDepthGuard;

// ------------------------------------------------- the crisp-HUD half's of fix.ui_quality declines

// Why a tonemap re-issue did not run for an admitted tonemap draw (or an HDR
// take was never possible). Each is counted on the crisp window line and
// NAMED once a session (first-seen per reason and shader pair below); the
// game's own draw is always untouched.
enum class CrispToneDecline : uint8_t {
    kNoHdrSlot = 0,  // the eye's own full-shape tonemap read no eye's HDR HUD source while its content was outstanding
    kNoContent,      // the eye's HDR layer holds nothing this frame (no HUD draws taken)
    kLayerBusy,      // the 8-bit layer already holds this frame's content (ordering guard)
    kSecondTonemap,  // this eye was already re-tonemapped this frame (never double-tonemap)
    kSizeMismatch,   // the HDR and 8-bit layers differ in size (the sample must be 1:1)
    kStateDrift,     // the bindings at the re-issue are not the admitted draw's
    kUav,            // a pixel-shader UAV is bound (the re-issue would write it twice)
    kLayerFailed,    // the 8-bit or HDR layer could not be created
    kCount
};

const char* crispToneDeclineName(CrispToneDecline d) {
    switch (d) {
        case CrispToneDecline::kNoHdrSlot:
            return "no PS slot of the admitted draw reads an eye's HDR HUD source (a settings-tier "
                   "or EDHM variant the identity read cannot place)";
        case CrispToneDecline::kNoContent: return "the eye's HDR layer holds nothing this frame";
        case CrispToneDecline::kLayerBusy:
            return "the 8-bit layer already holds this frame's UI (the menus draw after the "
                   "tonemap -- this should not happen)";
        case CrispToneDecline::kSecondTonemap:
            return "this eye was already re-tonemapped this frame (the layer is never "
                   "double-tonemapped)";
        case CrispToneDecline::kSizeMismatch:
            return "the HDR and 8-bit layers differ in size (the coverage sample must be 1:1)";
        case CrispToneDecline::kStateDrift:
            return "the bindings at the re-issue are not the admitted draw's";
        case CrispToneDecline::kUav:
            return "a pixel-shader UAV is bound (the re-issue would write it twice)";
        case CrispToneDecline::kLayerFailed: return "the 8-bit or HDR layer could not be created";
        default: return "?";
    }
}

// --------------------------------------------------------------- counters

struct Window {
    uint64_t frames = 0;
    uint64_t decided[static_cast<size_t>(UiLayerFamily::kCount)]
                    [static_cast<size_t>(UiLayerDecision::kCount)] = {};
    uint64_t redirected = 0, refusedAtIssue = 0;
    uint64_t viewportRemaps = 0, scissorRemaps = 0, jitterCancels = 0, clears = 0;
    uint64_t multiplies = 0, writeBacks = 0, dsTested = 0, seeds = 0, seedFailures = 0;
    uint64_t lostLayers = 0, doors = 0, treated = 0, composites = 0, overGameImage = 0;
    uint64_t compositeRefused = 0, afterWrites = 0, afterReads = 0;
    // afterWrites' own breakdown: taken into the layer after the UI (kept
    // over it), left as a post pass (an eye-sized input), or refused at
    // issue (decide-time, via decided[kAfterUi][...], plus begin-time below).
    uint64_t afterTaken = 0, afterPostPass = 0, afterRefused = 0, afterDeclined = 0;
    // The reads that carried an eye's identity through the game's post pass
    // into the target the interface draws to (uiLayerFollowReader): about one
    // per eye-frame the crisp re-issue ran. Zero beside a nonzero afterReads
    // in a HUD frame says the follow never ran.
    uint64_t afterFollowed = 0;
    uint64_t eyeMatched = 0, eyeSwapped = 0, eyeUntold = 0, seedStale = 0;
    uint64_t depthOnlySeedPreservedWriters = 0;
    uint64_t privateDepthPotentialBegins = 0;
    // the crisp-HUD half of fix.ui_quality: HUD draws taken into the HDR layer, tonemap re-issues,
    // coverage passes, HDR layers whose content never reached a tonemap, and
    // the re-issue's declines by reason (each also named once a session).
    uint64_t hdrRedirected = 0, hdrReissued = 0, hdrCoveragePasses = 0, hdrLost = 0;
    uint64_t hdrDrawTimingEligible = 0, hdrDrawTimingDisabled = 0, hdrDrawTimingIssued = 0;
    uint64_t hdrDrawTimingAborted = 0;
    uint64_t hdrDeclined[static_cast<size_t>(CrispToneDecline::kCount)] = {};
    // The VR world route (ui_layer.h): 2D screen draws re-issued into the layer, draws the game left in an eye
    // image after its screen was re-issued (lost while the route owns that eye), and the route's own refusals by
    // reason (the decision's refusals are in decided[kScreen][...] as for any family).
    uint64_t worldReissued = 0, worldLeftDraws = 0;
    uint64_t worldRefused[static_cast<size_t>(UiWorldRefuse::kCount)] = {};
    // The world-screen gate: frames read, 2D screen draws that asked, and the
    // frames each signal held the screen in the picture.
    uint64_t gateReads = 0, screenAsked = 0;
    uint64_t heldJournal = 0, heldDepth = 0, heldBoth = 0, depthCounted = 0;
    uint32_t depthMax = 0;
    // The screen's depth at most, by Status.json's GuiFocus: 0..11 the named
    // values, 12 another, 13 GuiFocus not known.
    static constexpr size_t kFocusSlots = 14;
    uint32_t focusMax[kFocusSlots] = {};
    bool focusSeen[kFocusSlots] = {};
    // The route's price: timers never had (no free one) and samples that did
    // not measure, by stage.
    uint64_t routeUntimed[static_cast<size_t>(UiRouteStage::kCount)] = {};
    uint64_t routeInvalid[static_cast<size_t>(UiRouteStage::kCount)] = {};
    uint64_t routeLate[static_cast<size_t>(UiRouteStage::kCount)] = {};
    // The family census: the menu panel's (0) and the loading screen's (1)
    // composite vertex shader, by how the family rule answered.
    uint64_t probe[2][static_cast<size_t>(UiFamilyWhy::kCount)] = {};
    // The composite census (ui_scene_composites.h): every draw into an eye that samples a learned
    // interface surface, taken or left in the game's frame, the left ones by shaders and family.
    UiSceneCompositeWindow scene;
};
Window g_win;

// A change of the door's size (a FOV trim or cull-guard adoption, an HMD
// Quality change, a per-eye width): the families the layer took in the two
// seconds before it are watched for their first draw after it -- said, with
// the delay -- and any not taken again within two seconds is named as
// dropped, with what the family rule made of its composites since.
constexpr uint64_t kSizeChangeWatchMs = 2000;
uint64_t g_familyLastRedirectMs[static_cast<size_t>(UiLayerFamily::kCount)] = {};
struct SizeChange {
    bool open = false;
    uint64_t ms = 0, frames = 0;
    uint32_t fromW = 0, fromH = 0, toW = 0, toH = 0;
    uint32_t engaged = 0, reengaged = 0;  // bit per UiLayerFamily
    bool droppedSaid = false;
    uint64_t probe[2][static_cast<size_t>(UiFamilyWhy::kCount)] = {};  // since the change
};
SizeChange g_sizeChange;
// Pixel shaders seen with a composite vertex shader and no learned surface
// that the rule does not know: named once each in the census line.
uint64_t g_unknownPs[2][4] = {};

// The world-screen gate (ui_layer_math.h): the journal's on-foot reading and
// the screen's own depth, read once a frame at the boundary, so every draw
// of a frame sees one answer.
UiOnFootGate g_onFoot;
UiWorldScreenGate g_world;
int8_t g_screenHeld = -1;  // -1 never read; 0 the screen is taken; 1 it is the world: left
uint64_t g_heldSinceMs = 0;
bool g_journalOffNoted = false;

// The on-foot maps gate (ui_maps_math.h). It is latched at the frame boundary into detail::g_uiLayerMapsOn, so every draw of a
// frame sees one answer.
struct Maps {
    UiMapsKey keyCfg = UiMapsKey::On;     // the gate is always on
    bool active = false;                  // the world camera's naming decided the gate at the last boundary
    UiMapsGate gate;                      // the step's state (carried from today's gate when the feature switches in)
    uint64_t episodeFrames = 0;           // frames in the current world period, or panel period
    uint64_t episodeStartMs = 0;
    uint64_t panelLayerOnly = 0;          // this panel period's eyes through the layer-only door
    uint64_t panelNotEmpty = 0;           // ... and the eyes the layer took the screen for but the upscaler kept
    uint64_t layerOnlySeq[2] = {0, 0};    // per eye, the last sequence counted: the two doors ask for each eye
    uint64_t notEmptySeq[2] = {0, 0};
    uint32_t notEmptyLines = 0;           // the "took the screen but the eye holds more" line, the first few only
    const char* notLiveWhy = nullptr;     // the reason the key is on and nothing changes (one line per reason)
    uint64_t windowStartMs = 0;
    UiMapsWindow win;
};
Maps g_maps;
uint32_t g_frameTakenDraws = 0;           // draws taken into either layer since the last frame boundary (the door's emptiness test)
constexpr uint64_t kMapsWindowMs = 5000;

// ------------------------------------------------------------ the price
//
// ui_layer_math.h's route: each stage's GPU interval timed with GpuTimer (no
// Flush, no wait) in a FIFO -- begun at the head, read at the tail, oldest
// first, stopping at the first not ready -- so each eye's samples arrive in
// the order they were issued, and summed per eye-frame.
struct RouteSlot {
    GpuTimer timer;
    bool inUse = false;
    UiRouteStage stage = UiRouteStage::kClear;
    int eye = 0;
    uint64_t seq = 0;
    UiRouteCoverageFlags coverage;
};
constexpr uint32_t kRouteRing = 512;  // live timer slots; one interval each. 64 covered the
                                      // pre-crisp route (~8/eye-frame); the crisp take adds a
                                      // seed and a write-back per HUD draw-group (tens a frame
                                      // at 0.5-quality), and a full ring silently drops whole
                                      // eye-frames' timings (routeBegin's -1, "no free timer")
                                      // -- the phase-3 review saw 60,895 of them with the
                                      // tonemap/coverage timings absent. 512 covers >200
                                      // intervals a frame with margin.
RouteSlot g_route[kRouteRing];
uint32_t g_routeHead = 0, g_routeTail = 0;  // monotonic; a slot is index % kRouteRing
uint64_t g_routeLatestSeq = 0;              // the newest frame a stage began in
constexpr size_t kStages = static_cast<size_t>(UiRouteStage::kCount);
constexpr size_t kRouteTotal = kStages;     // the stats slot of the whole route's sum
constexpr size_t kRouteWithMoved = kStages + 1;
constexpr bool g_hdrDrawTimingOn = false;  // the per-draw timing census is off
UiSeedCensus g_seedCensus;
UiHdrSeedGpuProbe g_hdrSeedGpu;
UiHdrSeedGpuProbe::Token g_hdrSeedActive = 0; // explicit outer-guard recovery for SEH
UiRouteSum g_stageSum[kStages][2];
UiRouteFrameTotals g_routeTotals[2];
UiRouteCoverage g_routeCoverage;
uint32_t g_routeCombinedArmedPending = 0;
uint32_t g_routeHdrMovedPending = 0;
// One window's per-eye-frame sums, by stage and for the route. Reservoir-
// sampled (routeSample): the percentiles stay unbiased over the whole 30 s
// window at ANY interval rate -- the old first-N cap silently kept only the
// window's first seconds once the rate outgrew it (the phase-3 review
// measured 60,895 intervals with no free timer and absent tonemap/coverage
// timings at 125%).
constexpr uint32_t kRouteSamples = 8192;    // 30 s of both eyes at 136 Hz
struct RouteStats {
    float v[kRouteSamples];
    uint32_t n = 0;
    uint64_t seen = 0;     // reservoir R: replace slot j with probability k/seen
    uint32_t rng = 0x9E3779B9u;  // a per-stat LCG; deterministic is fine for stats
};
RouteStats g_routeStats[kStages + 2];

void routeSample(size_t stat, double ms) {
    RouteStats& r = g_routeStats[stat];
    ++r.seen;
    if (r.n < kRouteSamples) {
        r.v[r.n++] = static_cast<float>(ms);
        return;
    }
    r.rng = r.rng * 1664525u + 1013904223u;
    const uint64_t j = (static_cast<uint64_t>(r.rng) * r.seen) >> 32;  // uniform in [0, seen)
    if (j < kRouteSamples) r.v[j] = static_cast<float>(ms);
}

// Apply a loss to queued flags once per stage/eye-frame. Future intervals
// consult the same ledger. HDR loss leaves the machinery total intact.
void routeApplyCoverage(int eye, uint64_t seq) {
    for (uint32_t i = g_routeTail; i != g_routeHead; ++i) {
        RouteSlot& s = g_route[i % kRouteRing];
        if (s.eye != eye || s.seq != seq) continue;
        const bool combined = s.coverage.combined;
        g_routeCoverage.apply(s.stage, eye, seq, s.coverage);
        if (combined && !s.coverage.combined) --g_routeCombinedArmedPending;
    }
}

void routeMovedMissing(int eye, uint64_t seq) {
    // A disabled draw is absent from both moved-only and combined coverage,
    // including an on/off toggle within one source sequence. Machinery stays
    // valid. Steady diagnostics-off frames need no scan of the pending ring.
    constexpr UiRouteStage stage = UiRouteStage::kHdrMovedDraw;
    uiRouteLost(g_stageSum[static_cast<size_t>(stage)][eye], seq);
    g_routeTotals[eye].lost(stage, seq);
    if (g_routeCoverage.missing(stage, eye, seq) &&
        (g_routeCombinedArmedPending || g_routeHdrMovedPending))
        routeApplyCoverage(eye, seq);
}

// Opens a timer for one stage of one eye-frame; -1 when none could be had
// (the ring full, or the timing domain not ours), and that eye-frame's sums
// are then dropped rather than reported short.
int routeBegin(ID3D11DeviceContext* ctx, UiRouteStage stage, int eye, uint64_t seq) {
    if (!ctx || eye < 0 || eye > 1 || !seq) return -1;
    const size_t si = static_cast<size_t>(stage);
    RouteSlot& s = g_route[g_routeHead % kRouteRing];
    bool ok = false;
    if (!s.inUse) {
        Ptr<ID3D11Device> dev;
        ctx->GetDevice(&dev);
        ok = dev && (gpuTimingAccepts(ctx) || gpuTimingBind(dev.Get(), ctx)) &&
             s.timer.begin(dev.Get(), ctx);
    }
    if (!ok) {
        ++g_win.routeUntimed[si];
        uiRouteLost(g_stageSum[si][eye], seq);
        g_routeTotals[eye].lost(stage, seq);
        if (g_routeCoverage.missing(stage, eye, seq)) routeApplyCoverage(eye, seq);
        return -1;
    }
    s.inUse = true;
    s.stage = stage;
    s.eye = eye;
    s.seq = seq;
    s.coverage = g_routeCoverage.begin(stage, eye, seq, g_hdrDrawTimingOn);
    if (s.coverage.combined) ++g_routeCombinedArmedPending;
    if (stage == UiRouteStage::kHdrMovedDraw) ++g_routeHdrMovedPending;
    if (seq > g_routeLatestSeq) g_routeLatestSeq = seq;
    const int idx = static_cast<int>(g_routeHead % kRouteRing);
    ++g_routeHead;
    return idx;
}

void routeEnd(ID3D11DeviceContext* ctx, int idx) {
    if (idx < 0 || idx >= static_cast<int>(kRouteRing) || !ctx) return;
    g_route[idx].timer.end(ctx);  // a failed end reads back as not measured
}

void routeAbort(ID3D11DeviceContext* ctx, int idx) {
    if (idx < 0 || idx >= static_cast<int>(kRouteRing) || !ctx) return;
    // Keep the FIFO entry: routePoll consumes it as Invalid and spoils its
    // stage/combined eye-frame. Reset cancels its lease on the owner context.
    g_route[idx].timer.reset(ctx);
}

// The ready samples, oldest first, into their sums; then every sum no
// sample can still reach is closed: all the timers of its frame are read,
// and a later frame has begun.
void routePoll(ID3D11DeviceContext* ctx) {
    if (!ctx || !gpuTimingOwns(ctx)) return;
    double closed = 0.0;
    while (g_routeTail != g_routeHead) {
        RouteSlot& s = g_route[g_routeTail % kRouteRing];
        double ms = 0.0;
        const GpuTimerPoll r = s.timer.poll(ctx, ms);
        if (r == GpuTimerPoll::Pending) break;
        const bool valid = r == GpuTimerPoll::Ready;
        const size_t si = static_cast<size_t>(s.stage);
        if (!valid) ++g_win.routeInvalid[si];
        if (uiRouteLate(g_stageSum[si][s.eye], s.seq)) ++g_win.routeLate[si];
        if (uiRouteAdd(g_stageSum[si][s.eye], s.seq, ms, valid && s.coverage.stage, &closed))
            routeSample(si, closed);
        double combined = 0.0;
        const unsigned totals = g_routeTotals[s.eye].add(s.stage, s.seq, ms, valid,
                                                       s.coverage.combined, closed, combined,
                                                       s.coverage.machinery);
        if (totals & 1) routeSample(kRouteTotal, closed);
        if (totals & 2) routeSample(kRouteWithMoved, combined);
        s.inUse = false;
        if (s.coverage.combined) --g_routeCombinedArmedPending;
        if (s.stage == UiRouteStage::kHdrMovedDraw) --g_routeHdrMovedPending;
        ++g_routeTail;
    }
    uint64_t before[2] = {g_routeLatestSeq, g_routeLatestSeq};
    for (uint32_t i = g_routeTail; i != g_routeHead; ++i) {
        const RouteSlot& s = g_route[i % kRouteRing];
        if (s.seq < before[s.eye]) before[s.eye] = s.seq;
    }
    for (int e = 0; e < 2; ++e) {
        for (size_t si = 0; si < kStages; ++si) {
            if (uiRouteClose(g_stageSum[si][e], before[e], &closed)) routeSample(si, closed);
        }
        double combined = 0.0;
        const unsigned totals = g_routeTotals[e].close(before[e], closed, combined);
        if (totals & 1) routeSample(kRouteTotal, closed);
        if (totals & 2) routeSample(kRouteWithMoved, combined);
    }
}

// "median/p95 (n)" of one window's sums, or "-" when the stage never ran.
void appendPrice(std::string& s, size_t stat) {
    RouteStats& r = g_routeStats[stat];
    if (!r.n) {
        s += "-";
        return;
    }
    const double p95 = uiLayerPercentile(r.v, r.n, 0.95);  // sorts; the median reads the same order
    const double med = uiLayerSortedPercentile(r.v, r.n, 0.5);
    char buf[64];
    _snprintf_s(buf, sizeof(buf), _TRUNCATE, "%.3f/%.3f (%u)", med, p95, r.n);
    s += buf;
}

uint64_t g_winStartMs = 0;
uint64_t g_sessionRedirected = 0;
// Phase-0 timing (src/common/periodic_work.h) of the 30 s totals and price
// lines, built on Elite's thread at the frame boundary: appendPrice sorts up
// to kRouteSamples floats for each of the price stats. Context: the samples
// held then (what the sorts had to order). One run per window that logs, so
// nothing is written while fix.ui_quality is off and nothing was redirected.
PeriodicWork g_workTotals{"ui_layer_totals", "samples"};

// First-seen lines, deduplicated.
struct FamilySeen {
    uint8_t family, decision;
    uint64_t vs, ps;
};
FamilySeen g_familySeen[kMaxFamilyLines];
uint32_t g_familySeenCount = 0;
struct AfterSeen {
    char kind;
    uint64_t vs, ps;
};
AfterSeen g_afterSeen[kMaxAfterLines];
uint32_t g_afterSeenCount = 0;
bool g_engageNoted = false, g_compositeNoted = false;

bool structuralDecision(UiLayerDecision d) {
    // The transient ones -- not armed yet, late this frame -- are counted on
    // the totals line and never get a first-seen line of their own.
    return d != UiLayerDecision::kNotArmed && d != UiLayerDecision::kLate &&
           d != UiLayerDecision::kNotUi;
}

// Once per (family, outcome, shader pair): what it is, where it draws, what
// was decided -- with the blend or depth-stencil state that decided it, so a
// refusal names the numbers rather than a category.
void noteFamily(UiLayerFamily f, UiLayerDecision d, const char* detail) {
    if (!structuralDecision(d)) return;
    const uint64_t vs = bindingShaderHash(BindSlot::Vs), ps = bindingShaderHash(BindSlot::Ps);
    for (uint32_t i = 0; i < g_familySeenCount; ++i) {
        const FamilySeen& s = g_familySeen[i];
        if (s.family == uint8_t(f) && s.decision == uint8_t(d) && s.vs == vs && s.ps == ps) return;
    }
    if (g_familySeenCount >= kMaxFamilyLines) return;
    g_familySeen[g_familySeenCount++] = {uint8_t(f), uint8_t(d), vs, ps};
    Log::get().note("ui quality: layer: %s (vs %016llX ps %016llX) into a %ux%u %s target: %s%s%s.",
                    uiLayerFamilyName(f), static_cast<unsigned long long>(vs),
                    static_cast<unsigned long long>(ps), g_tc.info.a, g_tc.info.b,
                    viewName(g_tc.view), uiLayerDecisionName(d), detail && *detail ? " -- " : "",
                    detail ? detail : "");
}

// ------------------------------------------------------------ the layer

bool makeTarget(ID3D11Device* dev, uint32_t w, uint32_t h, DXGI_FORMAT format,
                Ptr<ID3D11Texture2D>* tex, Ptr<ID3D11RenderTargetView>* rtv,
                Ptr<ID3D11ShaderResourceView>* srv) {
    D3D11_TEXTURE2D_DESC d{};
    d.Width = w;
    d.Height = h;
    d.MipLevels = d.ArraySize = 1;
    d.Format = format;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(dev->CreateTexture2D(&d, nullptr, tex->ReleaseAndGetAddressOf())) ||
        FAILED(dev->CreateRenderTargetView(tex->Get(), nullptr, rtv->ReleaseAndGetAddressOf())) ||
        FAILED(dev->CreateShaderResourceView(tex->Get(), nullptr, srv->ReleaseAndGetAddressOf()))) {
        tex->Reset();
        rtv->Reset();
        srv->Reset();
        return false;
    }
    return true;
}

bool ensureLayer(ID3D11Device* dev, Eye& e, uint32_t w, uint32_t h, int eye) {
    if (e.tex && e.w == w && e.h == h) return true;
    e.coverage.reset();
    const bool resized = e.tex != nullptr;
    e.w = e.h = 0;
    e.seq = 0;
    e.draws = 0;
    e.chainOpen = false;
    if (!dev || !w || !h ||
        !makeTarget(dev, w, h, DXGI_FORMAT_R8G8B8A8_UNORM, &e.tex, &e.rtv, &e.srv))
        return false;
    e.w = w;
    e.h = h;
    Log::get().note("ui quality: layer: %s eye's layer %s at %ux%u (R8G8B8A8_UNORM, %.1f MB).",
                    eye == 0 ? "left" : "right", resized ? "re-created" : "created", w, h,
                    uiLayerMB(uiLayerBytes(w, h)));
    return true;
}

bool ensureLayerFor(ID3D11DeviceContext* ctx, int eye) {
    Eye& e = g_eye[eye];
    const UiLayerSize s = uiLayerSize(e.door.fullW, e.door.fullH, layerTarget());
    if (!s.w || !s.h) return false;
    if (e.tex && e.w == s.w && e.h == s.h) {
        e.basisW = e.door.fullW;
        e.basisH = e.door.fullH;
        return true;
    }
    Ptr<ID3D11Device> dev;
    ctx->GetDevice(&dev);
    if (!ensureLayer(dev.Get(), e, s.w, s.h, eye)) return false;
    e.basisW = e.door.fullW;
    e.basisH = e.door.fullH;
    return true;
}

// the crisp-HUD half's of fix.ui_quality HDR HUD layer, at the 8-bit layer's size: R16G16B16A16_FLOAT
// (8 bytes a pixel -- the memory line says what that costs).
bool ensureHdrLayerFor(ID3D11DeviceContext* ctx, int eye) {
    Eye& e = g_eye[eye];
    const UiLayerSize s = uiLayerSize(e.door.fullW, e.door.fullH, layerTarget());
    if (!s.w || !s.h) return false;
    if (e.hdrTex && e.hdrW == s.w && e.hdrH == s.h) return true;
    e.coverage.reset();
    const bool resized = e.hdrTex != nullptr;
    e.hdrW = e.hdrH = 0;
    e.hdrSeq = 0;
    e.hdrDraws = 0;
    Ptr<ID3D11Device> dev;
    ctx->GetDevice(&dev);
    if (!dev ||
        !makeTarget(dev.Get(), s.w, s.h, DXGI_FORMAT_R16G16B16A16_FLOAT, &e.hdrTex, &e.hdrRtv,
                    &e.hdrSrv)) {
        e.hdrTex.Reset();
        e.hdrRtv.Reset();
        e.hdrSrv.Reset();
        return false;
    }
    e.hdrW = s.w;
    e.hdrH = s.h;
    Log::get().note(
        "crisp hud: layer: %s eye's HDR HUD layer %s at %ux%u (R16G16B16A16_FLOAT, %.1f MB) -- "
        "the cockpit's holo panels, flight HUD, target sprite and holograms are drawn into it "
        "and tonemapped over the finished eye.",
        eye == 0 ? "left" : "right", resized ? "re-created" : "created", s.w, s.h,
        uiLayerMB(uiLayerBytes(s.w, s.h, 8)));
    return true;
}

// The per-channel transmittance, at the layer's size, made at the first
// multiply the eye sees.
bool ensureMult(ID3D11DeviceContext* ctx, Eye& e, int eye) {
    if (e.mTex && e.mW == e.w && e.mH == e.h) return true;
    Ptr<ID3D11Device> dev;
    ctx->GetDevice(&dev);
    e.mW = e.mH = 0;
    e.mSeq = 0;
    if (!dev || !e.w ||
        !makeTarget(dev.Get(), e.w, e.h, DXGI_FORMAT_R8G8B8A8_UNORM, &e.mTex, &e.mRtv, &e.mSrv))
        return false;
    e.mW = e.w;
    e.mH = e.h;
    Log::get().note("ui quality: layer: %s eye's multiply transmittance created at %ux%u (%.1f MB) "
                    "-- a draw that tints the frame by its colour went into the layer.",
                    eye == 0 ? "left" : "right", e.w, e.h, uiLayerMB(uiLayerBytes(e.w, e.h)));
    return true;
}

void releaseLayerDs(LayerDs& l) {
    l.tex.Reset();
    l.dsv.Reset();
    l.w = l.h = 0;
    l.viewFmt = DXGI_FORMAT_UNKNOWN;
    l.seq = 0;
    l.source = nullptr;
    l.seededMask = 0;
    l.seededDepth = false;
}

// Every layer, transmittance, depth target and composite output released
// (the door state kept): the key went off, the pass went away, or the layer
// stood down. About 380 MB at 1.25 on a 4340x4284 eye is not left resident
// for a feature that is off.
bool releaseLayers() {
    bool any = false;
    for (Eye& e : g_eye) {
        any = any || e.tex || e.mTex || e.ds.tex || e.dsCopy || e.out || e.copy || e.frameSrv ||
              e.hdrTex || e.hdrDs.tex;
        e.coverage.reset();
        e.tex.Reset();
        e.rtv.Reset();
        e.srv.Reset();
        e.w = e.h = e.basisW = e.basisH = 0;
        e.seq = 0;
        e.draws = 0;
        e.worldSeq = 0;
        e.screenTakenSeq = 0;
        e.chainOpen = false;
        e.mTex.Reset();
        e.mRtv.Reset();
        e.mSrv.Reset();
        e.mW = e.mH = 0;
        e.mSeq = 0;
        releaseLayerDs(e.ds);
        e.hdrTex.Reset();
        e.hdrRtv.Reset();
        e.hdrSrv.Reset();
        e.hdrW = e.hdrH = 0;
        e.hdrSeq = 0;
        e.hdrDraws = 0;
        e.hdrTarget = nullptr;
        releaseLayerDs(e.hdrDs);
        e.hdrToneSeq = 0;
        e.hdrMissStreak = 0;
        e.dsCopy.Reset();
        e.dsCopyDepth.Reset();
        e.dsCopyStencil.Reset();
        e.dsCopyW = e.dsCopyH = 0;
        e.dsCopyFmt = DXGI_FORMAT_UNKNOWN;
        e.out.Reset();
        e.outUav.Reset();
        e.outW = e.outH = 0;
        e.outFmt = DXGI_FORMAT_UNKNOWN;
        e.frameSrv.Reset();
        e.frameRes = nullptr;
        e.frameView = DXGI_FORMAT_UNKNOWN;
        e.copy.Reset();
        e.copySrv.Reset();
        e.copyW = e.copyH = 0;
        e.copyFmt = DXGI_FORMAT_UNKNOWN;
    }
    return any;
}

ID3D11BlendState* cachedBlend(ID3D11DeviceContext* ctx, const UiBlendRt& conv) {
    const D3D11_BLEND_DESC want = uiLayerBlendDesc(conv);
    for (uint32_t i = 0; i < g_blendCount; ++i) {
        if (std::memcmp(&g_blends[i].desc, &want, sizeof(want)) == 0) return g_blends[i].state.Get();
    }
    if (g_blendCount >= 16) return nullptr;
    Ptr<ID3D11Device> dev;
    ctx->GetDevice(&dev);
    Ptr<ID3D11BlendState> state;
    if (!dev || FAILED(dev->CreateBlendState(&want, &state)) || !state) return nullptr;
    g_blends[g_blendCount].desc = want;
    g_blends[g_blendCount].state = state;
    return g_blends[g_blendCount++].state.Get();
}

// The layer's own read of the bound depth-stencil state: dsStateOf itself
// is shared with the HUD layer census now (draw_state_describe.h), but the
// once-a-session note about a stencil test against a stencil-less view
// speaks of the layer's copy, so it stays the layer's.
UiDsState dsStateOfNoted(ID3D11DeviceContext* ctx, ID3D11DepthStencilView* dsv, UINT* refOut,
                         DXGI_FORMAT* viewFmtOut) {
    DXGI_FORMAT localFmt = DXGI_FORMAT_UNKNOWN;
    UiDsState s = dsStateOf(ctx, dsv, refOut, viewFmtOut ? viewFmtOut : &localFmt);
    const DXGI_FORMAT viewFmt = viewFmtOut ? *viewFmtOut : localFmt;
    // Decided here, once per draw: a stencil test against a view with no
    // stencil plane is no test at all (review P3-6), said once.
    static bool stencillessNoted = false;
    if (dsv && s.stencilEnable && !s.stencilPlane && !stencillessNoted) {
        stencillessNoted = true;
        Log::get().note("ui quality: layer: a draw enables the stencil test against a %s depth "
                        "target, which has no stencil: D3D11 passes it, and so does the layer's "
                        "copy in the same format -- nothing is seeded for it.",
                        viewName(viewFmt));
    }
    return s;
}

// Typeless family and the two shader views of a depth-stencil view format;
// false for one the Seeder does not read (D16).
bool dsFormats(DXGI_FORMAT view, DXGI_FORMAT* typeless, DXGI_FORMAT* depthSrv,
               DXGI_FORMAT* stencilSrv) {
    switch (view) {
        case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
            *typeless = DXGI_FORMAT_R32G8X24_TYPELESS;
            *depthSrv = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
            *stencilSrv = DXGI_FORMAT_X32_TYPELESS_G8X24_UINT;
            return true;
        case DXGI_FORMAT_D24_UNORM_S8_UINT:
            *typeless = DXGI_FORMAT_R24G8_TYPELESS;
            *depthSrv = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
            *stencilSrv = DXGI_FORMAT_X24_TYPELESS_G8_UINT;
            return true;
        case DXGI_FORMAT_D32_FLOAT:
            *typeless = DXGI_FORMAT_R32_TYPELESS;
            *depthSrv = DXGI_FORMAT_R32_FLOAT;
            *stencilSrv = DXGI_FORMAT_UNKNOWN;
            return true;
        default:
            return false;
    }
}

bool ensureSeeder(ID3D11Device* dev) {
    if (g_seeder && g_deferred) return true;
    if (g_seederTried || !dev) return false;
    g_seederTried = true;
    try {
        auto seeder = std::make_unique<edvr_layer_seed::Seeder>();
        seeder->init(dev);
        Ptr<ID3D11DeviceContext> deferred;
        if (FAILED(dev->CreateDeferredContext(0, &deferred)) || !deferred) {
            Log::get().note("ui quality: layer: no deferred context for the depth-stencil seed; "
                            "depth- or stencil-tested UI stays in the picture.");
            return false;
        }
        g_seeder = std::move(seeder);
        g_deferred = deferred;
        Log::get().note(
            "ui quality: layer: the depth-stencil seed is ready (stencil written %s) -- UI that "
            "tests the game's depth or stencil is drawn against the game's own buffer resampled "
            "to the layer's size.",
            g_seeder->usesSpecifiedStencilRef() ? "in one pass" : "one pass per bit");
        return true;
    } catch (const std::exception& ex) {
        Log::get().note("ui quality: layer: the depth-stencil seed could not be built (%s); "
                        "depth- or stencil-tested UI stays in the picture.",
                        ex.what());
        return false;
    }
}

// Can the layer reproduce this draw's tests against its own seeded copy?
bool dsReproducible(ID3D11DeviceContext* ctx, ID3D11DepthStencilView* dsv, uint32_t targetW,
                    uint32_t targetH, char* why, size_t whyN) {
    if (!dsv) return false;
    D3D11_DEPTH_STENCIL_VIEW_DESC vd{};
    dsv->GetDesc(&vd);
    DXGI_FORMAT tf, df, sf;
    if (vd.ViewDimension != D3D11_DSV_DIMENSION_TEXTURE2D || vd.Texture2D.MipSlice != 0) {
        _snprintf_s(why, whyN, _TRUNCATE, "the depth target is not a single 2D level");
        return false;
    }
    if (vd.Flags != 0) {
        _snprintf_s(why, whyN, _TRUNCATE, "the depth target is bound read-only");
        return false;
    }
    if (!dsFormats(vd.Format, &tf, &df, &sf)) {
        _snprintf_s(why, whyN, _TRUNCATE, "the depth format %s is not one the seed reads",
                    viewName(vd.Format));
        return false;
    }
    Ptr<ID3D11Resource> res;
    dsv->GetResource(&res);
    Ptr<ID3D11Texture2D> tex;
    if (!res || FAILED(res.As(&tex))) return false;
    D3D11_TEXTURE2D_DESC td{};
    tex->GetDesc(&td);
    if (td.ArraySize != 1 || td.SampleDesc.Count != 1 || td.Width != targetW ||
        td.Height != targetH) {
        _snprintf_s(why, whyN, _TRUNCATE, "the depth target is %ux%u x%u samples, not the %ux%u eye",
                    td.Width, td.Height, td.SampleDesc.Count, targetW, targetH);
        return false;
    }
    Ptr<ID3D11Device> dev;
    ctx->GetDevice(&dev);
    if (!ensureSeeder(dev.Get())) {
        _snprintf_s(why, whyN, _TRUNCATE, "the depth-stencil seed is unavailable");
        return false;
    }
    return true;
}

// The bytes one pixel of a depth-stencil target takes in the formats the game uses: what the creation line and
// the GPU census (which says which size its seed figure was measured at) report the target's memory from.
uint32_t dsBytesPerPixel(DXGI_FORMAT f) { return f == DXGI_FORMAT_D32_FLOAT_S8X24_UINT ? 8u : 4u; }

// The layer's depth-stencil target, seeded from the game's own at the
// layer's size with this frame's jitter cancelled: the Seeder maps a layer
// pixel p to the game pixel floor(p * game / layer + jitter), which is the
// redirected viewport's own map inverted. `l` is the layer's own target
// (the 8-bit layer's ds, or the HDR HUD layer's hdrDs) at outW x outH; the
// copy of the game's buffer the seed reads is the eye's, shared.
bool seedLayerDepth(ID3D11DeviceContext* ctx, Eye& e, LayerDs& l, int eye, uint32_t outW,
                    uint32_t outH, ID3D11DepthStencilView* gameDsv, uint8_t stencilMask,
                    bool needDepth, UiRouteStage stage, const char* who, const UiSeedPlan& seedPlan) {
    if (!gameDsv || !g_seeder || !g_deferred) return false;
    Ptr<ID3D11Resource> res;
    gameDsv->GetResource(&res);
    Ptr<ID3D11Texture2D> tex;
    if (!res || FAILED(res.As(&tex))) return false;
    D3D11_TEXTURE2D_DESC td{};
    tex->GetDesc(&td);
    D3D11_DEPTH_STENCIL_VIEW_DESC vd{};
    gameDsv->GetDesc(&vd);
    DXGI_FORMAT tf, df, sf;
    if (!dsFormats(vd.Format, &tf, &df, &sf)) return false;
    Ptr<ID3D11Device> dev;
    ctx->GetDevice(&dev);
    if (!dev) return false;
    // The copy the seed reads: the game's texture as it stands, so a view of
    // it is never bound while the game's own depth view is.
    if (!e.dsCopy || e.dsCopyW != td.Width || e.dsCopyH != td.Height || e.dsCopyFmt != tf) {
        e.dsCopy.Reset();
        e.dsCopyDepth.Reset();
        e.dsCopyStencil.Reset();
        e.dsCopyW = e.dsCopyH = 0;
        D3D11_TEXTURE2D_DESC cd{};
        cd.Width = td.Width;
        cd.Height = td.Height;
        cd.MipLevels = cd.ArraySize = 1;
        cd.Format = tf;
        cd.SampleDesc.Count = 1;
        cd.Usage = D3D11_USAGE_DEFAULT;
        cd.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_DEPTH_STENCIL;
        D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        sd.Texture2D.MipLevels = 1;
        sd.Format = df;
        if (FAILED(dev->CreateTexture2D(&cd, nullptr, &e.dsCopy)) ||
            FAILED(dev->CreateShaderResourceView(e.dsCopy.Get(), &sd, &e.dsCopyDepth))) {
            e.dsCopy.Reset();
            e.dsCopyDepth.Reset();
            return false;
        }
        if (sf != DXGI_FORMAT_UNKNOWN) {
            sd.Format = sf;
            if (FAILED(dev->CreateShaderResourceView(e.dsCopy.Get(), &sd, &e.dsCopyStencil))) {
                e.dsCopy.Reset();
                e.dsCopyDepth.Reset();
                return false;
            }
        }
        e.dsCopyW = td.Width;
        e.dsCopyH = td.Height;
        e.dsCopyFmt = tf;
    }
    // The layer's own target, at the layer's size, in the game's format.
    if (!l.tex || l.w != outW || l.h != outH || l.viewFmt != vd.Format) {
        l.tex.Reset();
        l.dsv.Reset();
        l.w = l.h = 0;
        l.seq = 0;
        D3D11_TEXTURE2D_DESC ld{};
        ld.Width = outW;
        ld.Height = outH;
        ld.MipLevels = ld.ArraySize = 1;
        ld.Format = tf;
        ld.SampleDesc.Count = 1;
        ld.Usage = D3D11_USAGE_DEFAULT;
        ld.BindFlags = D3D11_BIND_DEPTH_STENCIL;
        D3D11_DEPTH_STENCIL_VIEW_DESC lv{};
        lv.Format = vd.Format;
        lv.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
        if (FAILED(dev->CreateTexture2D(&ld, nullptr, &l.tex)) ||
            FAILED(dev->CreateDepthStencilView(l.tex.Get(), &lv, &l.dsv))) {
            l.tex.Reset();
            l.dsv.Reset();
            return false;
        }
        l.w = outW;
        l.h = outH;
        l.viewFmt = vd.Format;
        const uint32_t bpp = dsBytesPerPixel(vd.Format);
        Log::get().note("ui quality: layer: %s eye's depth-stencil target created at %ux%u (%s, "
                        "%.1f MB; with the copy of the game's it reads, %.1f MB) -- %s tests "
                        "the game's depth or stencil.",
                        eye == 0 ? "left" : "right", outW, outH, viewName(vd.Format),
                        uiLayerMB(uiLayerBytes(outW, outH, bpp)),
                        uiLayerMB(uiLayerBytes(outW, outH, bpp) +
                                  uiLayerBytes(td.Width, td.Height, bpp)),
                        who);
    }
    // The GPU census's section for the HDR HUD layer's seed (issue #38): every seed counted, a few timed on the
    // section's turn, the target's size noted. The 8-bit layer's seed is counted nowhere, so the section holds one
    // layer's seeds and the line's figure is that layer's. RAII, so the failed-recording exit below closes it too.
    // Wrap it around the route interval, not inside it: the census times the call site, the route times its own.
    GpuCensusSeedScope census(ctx, stage == UiRouteStage::kHdrSeed,
                              {outW, outH, dsBytesPerPixel(vd.Format), td.Width, td.Height, viewName(vd.Format)});
    // The seed's price (review P3-4): the copy and the Seeder's passes, one
    // interval of this eye-frame's route.
    const int timer = routeBegin(ctx, stage, eye, g_draw.seq);
    UiHdrSeedTimingMeta seedMeta;
    if (stage == UiRouteStage::kHdrSeed && g_hdrDrawTimingOn) {
        seedMeta.seq = g_draw.seq; seedMeta.eye = eye; seedMeta.reason = seedPlan.reason;
        seedMeta.sourceW = td.Width; seedMeta.sourceH = td.Height;
        seedMeta.width = outW; seedMeta.height = outH;
        seedMeta.stencilMask = stencilMask; seedMeta.needsDepth = needDepth;
        seedMeta.passes = seedPlan.passes;
    }
    const auto seedToken = stage == UiRouteStage::kHdrSeed
        ? g_hdrSeedGpu.beginCopy(dev.Get(), ctx, g_hdrDrawTimingOn, seedMeta) : 0;
    if (seedToken) g_hdrSeedActive = seedToken;
    // Observation must not change replay, including a failed timing begin.
    struct SeedProbeScope {
        ID3D11DeviceContext* ctx;
        UiHdrSeedGpuProbe::Token token;
        ~SeedProbeScope() {
            if (token) {
                g_hdrSeedGpu.cancel(ctx, token, UiHdrSeedGpuProbe::Cancel::ExecutionFailed);
                g_hdrSeedActive = 0;
            }
        }
    } seedProbe{ctx, seedToken};
    vScreenCopyResourceRaw(ctx, e.dsCopy.Get(), tex.Get());
    if (seedToken) g_hdrSeedGpu.endCopy(ctx, seedToken);
    bool recorded = false;
    try {
        g_seeder->seed(g_deferred.Get(), e.dsCopyDepth.Get(), e.dsCopyStencil.Get(), l.dsv.Get(),
                       td.Width, td.Height, outW, outH, g_draw.jx, g_draw.jy, stencilMask,
                       needDepth);
        recorded = true;
    } catch (const std::exception&) {
        recorded = false;
    }
    Ptr<ID3D11CommandList> list;
    const HRESULT fin = g_deferred->FinishCommandList(FALSE, &list);
    if (!recorded || FAILED(fin) || !list) {
        if (seedToken) g_hdrSeedGpu.cancel(ctx, seedToken, UiHdrSeedGpuProbe::Cancel::RecordingFailed);
        seedProbe.token = 0;
        if (seedToken) g_hdrSeedActive = 0;
        routeEnd(ctx, timer);
        return false;
    }
    if (seedToken) g_hdrSeedGpu.beginWork(ctx, seedToken);
    vScreenExecuteCommandListRaw(ctx, list.Get(), 1);
    if (seedToken) g_hdrSeedGpu.endWork(ctx, seedToken);
    seedProbe.token = 0;
    if (seedToken) g_hdrSeedActive = 0;
    routeEnd(ctx, timer);
    // What the seed wrote: with SV_StencilRef, one pass copies the depth and
    // all eight stencil bits whenever it draws at all; without it, the depth
    // when asked and one pass per asked bit (the rest cleared to 0).
    const bool hasStencil = e.dsCopyStencil != nullptr;
    const bool full = hasStencil && g_seeder->usesSpecifiedStencilRef() && (needDepth || stencilMask);
    l.seq = g_draw.seq;
    l.source = tex.Get();
    l.seededMask = full ? 0xFF : (hasStencil ? stencilMask : 0);
    l.seededDepth = needDepth || full;
    ++g_win.seeds;
    return true;
}

// The resource a depth-stencil view is over, as an identity.
const void* dsvResource(ID3D11DepthStencilView* dsv) {
    if (!dsv) return nullptr;
    Ptr<ID3D11Resource> res;
    dsv->GetResource(&res);
    return res.Get();
}

void releaseSaved() {
    // Keep original shader/CB references after an unrecoverable setter fault.
    // The owner draw gate stays up, and the references with it, even when the
    // per-draw state is reset: the frame boundary's settle needs both
    // (settleIssueFence, below).
    if (!g_holoBinding.needsRestore()) g_holoBinding.clear();
    ui_holo_remap::release(g_draw.holoDepthCheck);
    for (auto*& r : g_draw.rtv) {
        if (r) r->Release();
        r = nullptr;
    }
    if (g_draw.dsv) g_draw.dsv->Release();
    g_draw.dsv = nullptr;
    if (g_draw.blend) g_draw.blend->Release();
    g_draw.blend = nullptr;
    g_draw.saved = false;
}

UINT boundCount(ID3D11RenderTargetView* const* rtvs) {
    UINT n = 0;
    for (UINT i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i) {
        if (rtvs[i]) n = i + 1;
    }
    return n;
}

void restore(ID3D11DeviceContext* ctx) {
    // Restore the private shader/CB first, with independent guarded setters:
    // an OM/RS restoration fault must not leave the game's PS replaced.
    const auto holoRestore = g_holoBinding.finish(ctx, vScreenPSSetShaderRaw);
    g_draw.holoRestoreOk = holoRestore.restored;
    // Raises the fence: the game's owner-context draws are dropped until the
    // frame boundary settles the binding (settleIssueFence).
    if (!holoRestore.restored) detail::g_uiLayerIssueBlocked = true;
    if (holoRestore.retried)
        standDown(holoRestore.restored ? "a hologram shader/b13 restoration fault recovered on the bounded retry"
                                     : "hologram shader/b13 restoration failed; original state untrusted, owner draw issues suppressed until the frame boundary puts it back");
    vScreenSetRenderTargetsRaw(ctx, boundCount(g_draw.rtv), g_draw.rtv, g_draw.dsv);
    vScreenRSSetViewportsRaw(ctx, g_draw.vpCount, g_draw.vp);
    if (g_draw.scissorSet) ctx->RSSetScissorRects(g_draw.scCount, g_draw.sc);
    ctx->OMSetBlendState(g_draw.blend, g_draw.factor, g_draw.sampleMask);
}

// ------------------------------------------------------ a door size change

// Which composite vertex shader a census slot is (0 the menu panel's, 1 the
// loading screen's), or -1.
int probeSlot(uint64_t vs) {
    return vs == kUiVsPanel ? 0 : vs == kUiVsLoader ? 1 : -1;
}

// "N as UI by a learned surface, N by its shader pair alone, not UI: N ..."
// for one composite's counts.
std::string probeText(const uint64_t (&counts)[static_cast<size_t>(UiFamilyWhy::kCount)], int slot) {
    std::string s;
    char buf[160];
    _snprintf_s(buf, sizeof(buf), _TRUNCATE, "%llu as UI by a learned surface, %llu by its shader pair alone",
                static_cast<unsigned long long>(counts[static_cast<size_t>(UiFamilyWhy::kLearnedSurface)]),
                static_cast<unsigned long long>(counts[static_cast<size_t>(UiFamilyWhy::kShaderPair)]));
    s += buf;
    static const UiFamilyWhy kNot[] = {UiFamilyWhy::kNotEyeTarget, UiFamilyWhy::kNotPostTonemap,
                                       UiFamilyWhy::kExcluded, UiFamilyWhy::kNoSurface,
                                       UiFamilyWhy::kScreen, UiFamilyWhy::kOther};
    bool any = false;
    for (UiFamilyWhy w : kNot) {
        const uint64_t n = counts[static_cast<size_t>(w)];
        if (!n) continue;
        _snprintf_s(buf, sizeof(buf), _TRUNCATE, "%s%llu %s", any ? ", " : "; not taken as its family: ",
                    static_cast<unsigned long long>(n),
                    w == UiFamilyWhy::kScreen ? "as the 2D screen" : uiFamilyWhyName(w));
        s += buf;
        any = true;
    }
    if (slot >= 0 && slot < 2) {
        bool named = false;
        for (uint64_t ps : g_unknownPs[slot]) {
            if (!ps) continue;
            _snprintf_s(buf, sizeof(buf), _TRUNCATE, "%s%016llX", named ? ", " : " (unknown ps ",
                        static_cast<unsigned long long>(ps));
            s += buf;
            named = true;
        }
        if (named) s += ")";
    }
    return s;
}

// The door handed on a new size: open the watch over the families the layer
// took in the two seconds before (the other eye's report of the same change
// joins it).
void openSizeChange(uint32_t fromW, uint32_t fromH, uint32_t toW, uint32_t toH) {
    SizeChange& c = g_sizeChange;
    const uint64_t now = GetTickCount64();
    if (c.open && c.toW == toW && c.toH == toH && now - c.ms < kSizeChangeWatchMs) return;
    uint32_t engaged = 0;
    std::string names;
    for (size_t f = 1; f < static_cast<size_t>(UiLayerFamily::kCount); ++f) {
        const uint64_t last = g_familyLastRedirectMs[f];
        if (!last || now - last > kSizeChangeWatchMs) continue;
        engaged |= 1u << f;
        if (!names.empty()) names += ", ";
        names += uiLayerFamilyName(static_cast<UiLayerFamily>(f));
    }
    c = SizeChange{};
    if (!engaged) return;
    c.open = true;
    c.ms = now;
    c.fromW = fromW;
    c.fromH = fromH;
    c.toW = toW;
    c.toH = toH;
    c.engaged = engaged;
    Log::get().note("ui quality: layer: the door's frame changed size (%ux%u -> %ux%u) with %s in the "
                    "layer -- each is watched for its first draw taken after it.",
                    fromW, fromH, toW, toH, names.c_str());
}

// A draw of `family` was taken: its re-engagement after a size change, once.
void noteTaken(UiLayerFamily family, int eye, uint32_t layerW, uint32_t layerH) {
    const uint64_t now = GetTickCount64();
    const size_t fi = static_cast<size_t>(family);
    if (fi >= static_cast<size_t>(UiLayerFamily::kCount)) return;
    g_familyLastRedirectMs[fi] = now;
    SizeChange& c = g_sizeChange;
    const uint32_t bit = 1u << fi;
    if (!c.open || !(c.engaged & bit) || (c.reengaged & bit)) return;
    c.reengaged |= bit;
    Log::get().note("ui quality: layer: %s re-engaged after the door's size change (%ux%u -> %ux%u) -- "
                    "its first draw taken %llu ms and %llu frames after it, into the %s eye's %ux%u "
                    "layer%s.",
                    uiLayerFamilyName(family), c.fromW, c.fromH, c.toW, c.toH,
                    static_cast<unsigned long long>(now - c.ms), static_cast<unsigned long long>(c.frames),
                    eye == 0 ? "left" : "right", layerW, layerH,
                    c.droppedSaid ? " (it had been named as dropped)" : "");
}

// Once a frame: a family engaged before the change and not taken for two
// seconds after it is named as dropped, with what the family rule made of
// its composite's draws since; the watch closes when every family is back,
// or after thirty seconds.
void sizeChangeTick() {
    SizeChange& c = g_sizeChange;
    if (!c.open) return;
    ++c.frames;
    const uint64_t now = GetTickCount64();
    const uint32_t missing = c.engaged & ~c.reengaged;
    if (!missing) {
        c.open = false;
        return;
    }
    if (!c.droppedSaid && now - c.ms >= kSizeChangeWatchMs) {
        c.droppedSaid = true;
        for (size_t f = 1; f < static_cast<size_t>(UiLayerFamily::kCount); ++f) {
            if (!(missing & (1u << f))) continue;
            const UiLayerFamily fam = static_cast<UiLayerFamily>(f);
            const int slot = fam == UiLayerFamily::kPanel ? 0 : fam == UiLayerFamily::kLoader ? 1 : -1;
            const std::string since = slot >= 0 ? probeText(c.probe[slot], slot) : std::string();
            Log::get().note("ui quality: layer: the door's size change (%ux%u -> %ux%u) DROPPED the %s -- "
                            "none of its draws taken in the %.1f s since%s%s.",
                            c.fromW, c.fromH, c.toW, c.toH, uiLayerFamilyName(fam),
                            static_cast<double>(now - c.ms) / 1000.0,
                            slot >= 0 ? "; its composite's draws since: " : "", since.c_str());
        }
    }
    if (now - c.ms > 30000) c.open = false;
}

// A pixel-shader sampler as one log phrase, for the frosted base's first take: the address mode of the sampler
// its blurred-scene lookup runs through (s1) was on no census line. Whether a lookup past 1.0 folded back
// (mirror), smeared (clamp) or repeated (wrap) is what the artifact looked like; the remap does not depend on
// it, the log records which the game chose.
void describeSampler(ID3D11DeviceContext* ctx, UINT slot, char* out, size_t n) {
    static const char* const kAddress[] = {"?", "wrap", "mirror", "clamp", "border", "mirror-once"};
    D3D11_SAMPLER_DESC sd{};
    ID3D11SamplerState* sampler = nullptr;
    bool read = false;
    guarded("ui.frosted.sampler", [&] {
        ctx->PSGetSamplers(slot, 1, &sampler);
        if (sampler) {
            sampler->GetDesc(&sd);
            read = true;
        }
    });
    ui_holo_remap::release(sampler);
    auto name = [](D3D11_TEXTURE_ADDRESS_MODE m) {
        return m >= 1 && m <= 5 ? kAddress[m] : kAddress[0];
    };
    if (!read) {
        _snprintf_s(out, n, _TRUNCATE, "sampler s%u unreadable", slot);
        return;
    }
    _snprintf_s(out, n, _TRUNCATE, "sampler s%u address %s/%s/%s, filter %u", slot, name(sd.AddressU),
                name(sd.AddressV), name(sd.AddressW), static_cast<unsigned>(sd.Filter));
}

// One issue of a decided draw into the layer (which = 0) or into the
// multiply transmittance (which = 1). the crisp-HUD half of fix.ui_quality: a decided draw with
// g_draw.hdr goes into the eye's HDR HUD layer instead -- same map, jitter
// cancel, seeded depth machinery, write-back; the HDR layer's own seq tracks
// its per-eye-frame clear.
bool beginInner(ID3D11DeviceContext* ctx, int which) {
    Eye& e = g_eye[g_draw.eye];
    ID3D11RenderTargetView* target =
        which ? e.mRtv.Get() : (g_draw.hdr ? e.hdrRtv.Get() : e.rtv.Get());
    if (!target) return false;
    LayerDs& lds = g_draw.hdr ? e.hdrDs : e.ds;
    const uint32_t layerW = g_draw.hdr ? e.hdrW : e.w;
    const uint32_t layerH = g_draw.hdr ? e.hdrH : e.h;
    // The blend at the moment of issue: a verdict's own Begin runs between
    // the decision and here, and the shape must still be the decided one.
    ID3D11BlendState* bs = nullptr;
    FLOAT factor[4] = {};
    UINT sampleMask = 0xFFFFFFFFu;
    ctx->OMGetBlendState(&bs, factor, &sampleMask);
    UiBlendRt game, conv;
    ID3D11BlendState* layerBlend = nullptr;
    const UiBlendShape shape = shapeOf(bs, &game);
    const bool converted = shape == g_draw.shape &&
                           (which ? uiLayerMultiplyBlend(game, &conv) : uiLayerConvertBlend(game, &conv));
    if (!converted || !(layerBlend = cachedBlend(ctx, conv))) {
        if (bs) bs->Release();
        ++g_win.refusedAtIssue;
        if (g_draw.family == UiLayerFamily::kAfterUi) ++g_win.afterRefused;
        return false;
    }
    // A new frame for this eye's layer: clear it, and count a layer the door
    // never composited (the frame took a path without a door, or a withhold).
    // The HDR layer's equivalent: content no tonemap re-issue ever read (the
    // frame took a path without one).
    if (which == 0 && !g_draw.hdr && e.seq != g_draw.seq) {
        if (e.seq && e.draws && e.compositedSeq != e.seq) ++g_win.lostLayers;
        const int timer = routeBegin(ctx, UiRouteStage::kClear, g_draw.eye, g_draw.seq);
        vScreenClearRenderTargetViewRaw(ctx, e.rtv.Get(), kUiLayerClear);
        routeEnd(ctx, timer);
        e.seq = g_draw.seq;
        e.draws = 0;
        e.target = g_draw.targetRes;
        e.chainOpen = false;  // the layer starts with a UI draw: no re-issue's HUD, no chain to follow
        ++g_win.clears;
    }
    if (which == 0 && g_draw.hdr && e.hdrSeq != g_draw.seq) {
        if (e.hdrSeq && e.hdrDraws && e.hdrToneSeq != e.hdrSeq) {
            ++g_win.hdrLost;
            // The missing-consumer deadline (review R1): a content-frame no
            // tonemap re-issue published -- the already-published case does
            // NOT count (hdrToneSeq == hdrSeq). The tonemap runs every frame
            // when the path works; a streak of kCrispHdrMissFrames says the
            // consumer is not coming, so stand down to stock rather than
            // lose the HUD every frame. The streak resets where a
            // publication lands (uiLayerCrispToneEnd). This draw still
            // completes into the layer; the NEXT HUD draw is stock.
            // The flat profile takes only behind a proven tonemap (flat_ui_layer.cpp), so one miss is already a broken
            // proof: it backs off at the first, losing one frame's HUD instead of thirty.
            const uint32_t missLimit = runtimeFlatProfile() ? 1u : kCrispHdrMissFrames;
            if (++e.hdrMissStreak >= missLimit) {
                char why[200];
                _snprintf_s(why, _TRUNCATE,
                            "the HUD layer's content reached no tonemap for %u consecutive frames "
                            "(the cockpit HUD was taken but never came back)",
                            missLimit);
                crispStandDown(why);
            }
        }
        const int timer = routeBegin(ctx, UiRouteStage::kHdrClear, g_draw.eye, g_draw.seq);
        vScreenClearRenderTargetViewRaw(ctx, e.hdrRtv.Get(), kUiLayerClear);
        routeEnd(ctx, timer);
        e.hdrSeq = g_draw.seq;
        e.hdrDraws = 0;
        e.hdrTarget = g_draw.targetRes;
        ++g_win.clears;
    }
    if (which == 1 && e.mSeq != g_draw.seq) {
        const int timer = routeBegin(ctx, UiRouteStage::kMultiply, g_draw.eye, g_draw.seq);
        vScreenClearRenderTargetViewRaw(ctx, e.mRtv.Get(), kUiLayerMultClear);
        routeEnd(ctx, timer);
        e.mSeq = g_draw.seq;
    }
    // Save.
    g_draw.blend = bs;
    std::memcpy(g_draw.factor, factor, sizeof(factor));
    g_draw.sampleMask = sampleMask;
    ctx->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, g_draw.rtv, &g_draw.dsv);
    g_draw.vpCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    ctx->RSGetViewports(&g_draw.vpCount, g_draw.vp);
    g_draw.scissorSet = false;
    g_draw.scCount = 0;
    {
        Ptr<ID3D11RasterizerState> rs;
        ctx->RSGetState(&rs);
        D3D11_RASTERIZER_DESC rd{};
        if (rs) rs->GetDesc(&rd);
        if (rs && rd.ScissorEnable) {
            g_draw.scCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
            ctx->RSGetScissorRects(&g_draw.scCount, g_draw.sc);
            g_draw.scissorSet = g_draw.scCount > 0;
        }
    }
    // Everything the game had is held: from here on any exit, a fault's
    // included, puts it back (uiLayerBegin).
    g_draw.saved = true;
    ui_holo_remap::Params holoParams{};
    if (g_draw.holoPsHash) {
        const auto& v = g_draw.vp[0];
        const int slot = ui_holo_remap::index(g_draw.holoPsHash);
        // Only the hologram programs load the scene depth at t1, and only they need it to be the target's own
        // size; the frosted base looks its blurred scene up by UV, so nothing about its textures constrains
        // the take.
        const bool depthLoad = ui_holo_remap::kindOf(slot) == ui_holo_remap::Kind::kHologramDepth;
        if (depthLoad) ctx->PSGetShaderResources(1, 1, &g_draw.holoDepthCheck);
        const bool valid = g_draw.vpCount == 1 && std::isfinite(v.TopLeftX) &&
            std::isfinite(v.TopLeftY) && std::isfinite(v.Width) && std::isfinite(v.Height) &&
            v.Width > 0 && v.Height > 0 &&
            ui_holo_remap::params(g_draw.targetW, g_draw.targetH, layerW, layerH,
                                  g_draw.jx, g_draw.jy, holoParams) &&
            (!depthLoad ||
             ui_holo_remap::depthSource(g_draw.holoDepthCheck, g_draw.targetW, g_draw.targetH));
        if (!valid) {
            ++g_holoRefused; ++g_holoRefusedBy[slot]; ++g_win.refusedAtIssue;
            if (!g_holoRefusalNoted[slot]) {
                Log::get().note("crisp holo remap: refused PS %016llX -- unsupported %s; stock complete draw retained.",
                    static_cast<unsigned long long>(g_draw.holoPsHash),
                    depthLoad ? "t1 depth, viewport, or map" : "viewport or map");
                g_holoRefusalNoted[slot] = true;
            }
            releaseSaved(); return false;
        }
    }
    // The layer's depth-stencil target: for a draw that tests, seeded from
    // the game's own first (or again, when stale or short of the bits this
    // draw reads); for a draw that only writes, bound once seeded this frame
    // so later tests in the layer see its write.
    ID3D11DepthStencilView* layerDsv = nullptr;
    const void* gameDs = dsvResource(g_draw.dsv);
    // Seeded this frame, from this buffer, AND at the layer's size: a layer
    // re-made mid-frame leaves a depth target of the old size, which D3D11
    // would refuse to bind beside it (review P3-5).
    const bool seededNow = lds.dsv && lds.w == layerW && lds.h == layerH &&
                           lds.seq == g_draw.seq && lds.source && lds.source == gameDs;
    const bool needsDs = g_draw.ds.tests() || (g_draw.ds.writes() && seededNow);
    if (needsDs) {
        const bool stale = !seededNow;
        const uint8_t wantMask = g_draw.ds.stencilTest ? g_draw.stencilRead : 0;
        const bool shortBits = (wantMask & ~lds.seededMask) != 0;
        const bool shortDepth = g_draw.ds.depthTest && !lds.seededDepth;
        if (g_draw.ds.tests() && (stale || shortBits || shortDepth)) {
            const uint8_t mask =
                static_cast<uint8_t>(wantMask | (stale ? 0 : lds.seededMask));
            const bool depth = g_draw.ds.depthTest || (!stale && lds.seededDepth);
            UiSeedPlan seedPlan;
            if (g_seedCensus.enabled) {
                D3D11_DEPTH_STENCIL_VIEW_DESC vd{};
                g_draw.dsv->GetDesc(&vd);
                DXGI_FORMAT tf, df, sf;
                const bool stencil = dsFormats(vd.Format, &tf, &df, &sf) && sf != DXGI_FORMAT_UNKNOWN;
                seedPlan = g_seedCensus.plan(g_draw.eye, g_draw.hdr, g_draw.seq, gameDs,
                    layerW, layerH, stale, shortDepth, shortBits, mask, depth,
                    g_seeder && g_seeder->usesSpecifiedStencilRef(), stencil);
            }
            const bool seeded = seedLayerDepth(ctx, e, lds, g_draw.eye, layerW, layerH, g_draw.dsv, mask, depth,
                                g_draw.hdr ? UiRouteStage::kHdrSeed : UiRouteStage::kSeed,
                                g_draw.hdr ? "a cockpit HUD draw" : "a UI draw", seedPlan);
            if (g_seedCensus.enabled) g_seedCensus.seed(g_draw.eye, g_draw.hdr, g_draw.seq,
                gameDs, layerW, layerH, seedPlan, seeded);
            if (!seeded) {
                ++g_win.seedFailures;
                releaseSaved();
                ++g_win.refusedAtIssue;
                if (g_draw.family == UiLayerFamily::kAfterUi) ++g_win.afterRefused;
                return false;
            }
        }
        layerDsv = lds.dsv.Get();
        if (g_draw.ds.tests() && !g_draw.counted) ++g_win.dsTested;
    }
    // The map (the game's eye target onto the layer) and the jitter cancel.
    const UiLayerMap m = uiLayerMapFromRegion(0.0f, 0.0f, static_cast<float>(g_draw.targetW),
                                              static_cast<float>(g_draw.targetH), layerW, layerH);
    float cx = 0.0f, cy = 0.0f;
    uiLayerJitterCancel(g_draw.jx, g_draw.jy, m, &cx, &cy);
    if (g_draw.holoPsHash && !g_holoBinding.begin(ctx, g_draw.holoPatched, g_holoCache.constants(),
            holoParams, vScreenPSSetShaderRaw, g_draw.holoOriginal)) {
        const int slot = ui_holo_remap::index(g_draw.holoPsHash);
        ++g_holoRefused; ++g_holoRefusedBy[slot]; ++g_win.refusedAtIssue;
        if (!g_holoRefusalNoted[slot]) {
            Log::get().note("crisp holo remap: refused PS %016llX at bind (original shader changed or dynamic classes); stock complete draw retained.",
                static_cast<unsigned long long>(g_draw.holoPsHash));
            g_holoRefusalNoted[slot] = true;
        }
        releaseSaved(); return false;
    }
    D3D11_VIEWPORT vp[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
    for (UINT i = 0; i < g_draw.vpCount; ++i) {
        const D3D11_VIEWPORT& g = g_draw.vp[i];
        UiViewport v;
        v.x = g.TopLeftX;
        v.y = g.TopLeftY;
        v.w = g.Width;
        v.h = g.Height;
        v.minZ = g.MinDepth;
        v.maxZ = g.MaxDepth;
        const UiViewport o = uiLayerMapViewport(m, v, cx, cy);
        vp[i] = {o.x, o.y, o.w, o.h, o.minZ, o.maxZ};
    }
    vScreenRSSetViewportsRaw(ctx, g_draw.vpCount, vp);
    if (g_draw.scissorSet) {
        D3D11_RECT sc[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
        for (UINT i = 0; i < g_draw.scCount; ++i) {
            UiRect r;
            r.l = g_draw.sc[i].left;
            r.t = g_draw.sc[i].top;
            r.r = g_draw.sc[i].right;
            r.b = g_draw.sc[i].bottom;
            const UiRect o = uiLayerMapScissor(m, r, cx, cy, layerW, layerH);
            sc[i] = {o.l, o.t, o.r, o.b};
        }
        ctx->RSSetScissorRects(g_draw.scCount, sc);
    }
    ctx->OMSetBlendState(layerBlend, factor, sampleMask);
    vScreenSetRenderTargetsRaw(ctx, 1, &target, layerDsv);
    // An original read-only view can mask its effect, while this private view
    // is writable. Preserve the raw state's potential without another query.
    g_privateDepthGuard.note(g_draw.seq, g_draw.rawDepthWritePotential);
    if (g_draw.rawDepthWritePotential && !g_draw.counted) ++g_win.privateDepthPotentialBegins;
    g_draw.active = true;
    // A multiply's second issue -- the game's draw again, into the
    // transmittance -- is the layer's own work: timed to uiLayerEnd. The
    // first HDR issue is moved game rendering, separately timed only when
    // diagnostics are armed. Both scopes close before state restoration.
    const UiDrawRouteStart timing = g_draw.drawRoute.begin(
        true, g_draw.hdr, which, g_hdrDrawTimingOn, g_draw.eye, g_draw.seq,
        [&](UiRouteStage stage, int eye, uint64_t seq) { return routeBegin(ctx, stage, eye, seq); },
        [&](int eye, uint64_t seq) { routeMovedMissing(eye, seq); });
    if (g_draw.hdr && which == 0) {
        ++g_win.hdrDrawTimingEligible;
        if (timing == UiDrawRouteStart::Disabled) ++g_win.hdrDrawTimingDisabled;
        if (timing == UiDrawRouteStart::Opened) ++g_win.hdrDrawTimingIssued;
    }

    // Counted once per draw: the curved screen's fall-through re-issues the
    // same draw after a failed substitution, and a multiply's second draw is
    // the same draw too.
    if (!g_draw.counted) {
        g_draw.counted = true;
        ++g_frameTakenDraws;
        if (g_draw.hdr) {
            ++e.hdrDraws;
            ++g_win.hdrRedirected;
        } else {
            ++e.draws;
            e.chainOpen = false;  // the UI has started: a pass over the eye no longer carries its identity
        }
        ++g_win.redirected;
        ++g_sessionRedirected;
        noteTaken(g_draw.family, g_draw.eye, layerW, layerH);
        // The on-foot maps gate gave the layer the 2D screen (a TAKE; the route's re-issue, where the game's own draw lands in
        // its eye, is not): this eye's picture is the layer's, and the door may run layer-only for it (uiLayerDoorLayerOnly).
        if (g_draw.family == UiLayerFamily::kScreen && !g_draw.hdr && !g_draw.reissue && detail::g_uiLayerMapsOn) {
            e.screenTakenSeq = g_draw.seq;
            ++g_maps.win.screenTakes;
        }
        if (g_draw.family == UiLayerFamily::kAfterUi) ++g_win.afterTaken;
        g_win.viewportRemaps += g_draw.vpCount;
        if (g_draw.scissorSet) g_win.scissorRemaps += g_draw.scCount;
        if (g_draw.jx != 0.0f || g_draw.jy != 0.0f) ++g_win.jitterCancels;
    }
    if (which == 1) ++g_win.multiplies;
    g_lastRedirectSeq = g_draw.seq;
    detail::g_uiLayerWatching = true;
    // Once per family: where its first draw landed and the cancel it took,
    // so a family placed in fixed clip space rather than through the eye's
    // projection (which the cancel would jitter) can be told on the flight.
    bool& familyEngaged = g_familyEngaged[static_cast<size_t>(g_draw.family)];
    if (!familyEngaged && g_draw.vpCount && which == 0) {
        familyEngaged = true;
        const D3D11_VIEWPORT& gv = g_draw.vp[0];
        Log::get().note(
            "ui quality: layer: first %s draw in the %s eye's layer -- the game's viewport "
            "(%.1f, %.1f) %.1fx%.1f became (%.3f, %.3f) %.1fx%.1f, a jitter cancel of (%.3f, "
            "%.3f) layer pixels.",
            uiLayerFamilyName(g_draw.family), g_draw.eye == 0 ? "left" : "right",
            static_cast<double>(gv.TopLeftX), static_cast<double>(gv.TopLeftY),
            static_cast<double>(gv.Width), static_cast<double>(gv.Height),
            static_cast<double>(vp[0].TopLeftX), static_cast<double>(vp[0].TopLeftY),
            static_cast<double>(vp[0].Width), static_cast<double>(vp[0].Height),
            static_cast<double>(cx), static_cast<double>(cy));
    }
    if (!g_engageNoted && which == 0) {
        g_engageNoted = true;
        Log::get().note(
            "ui quality: layer: engaged -- the %s went into the %s eye's %slayer (%ux%u) from a "
            "%ux%u %s target: viewport scale %.4f x %.4f, jitter cancel (%.3f, %.3f) layer "
            "pixels, blend %s with transmittance in alpha.",
            uiLayerFamilyName(g_draw.family), g_draw.eye == 0 ? "left" : "right",
            g_draw.hdr ? "HDR HUD " : "", layerW, layerH,
            g_draw.targetW, g_draw.targetH, viewName(g_tc.view), static_cast<double>(m.ax),
            static_cast<double>(m.ay), static_cast<double>(cx), static_cast<double>(cy),
            uiBlendShapeName(shape));
    }
    if (g_draw.holoPsHash) {
        const int slot = ui_holo_remap::index(g_draw.holoPsHash);
        ++g_holoTaken; ++g_holoTakenBy[slot];
        if (!g_holoTakeNoted[slot]) {
            if (ui_holo_remap::kindOf(slot) == ui_holo_remap::Kind::kFrostedBase) {
                char sampler[96];
                describeSampler(ctx, 1, sampler, sizeof(sampler));
                Log::get().note("crisp holo remap: admitted frosted base PS %016llX eye %d input %ux%u layer %ux%u inverse (%g,%g) bias (%g,%g); its blurred-scene lookup reads the game's pixel position, not the layer's (%s).",
                    static_cast<unsigned long long>(g_draw.holoPsHash), g_draw.eye,
                    g_draw.targetW, g_draw.targetH, layerW, layerH,
                    double(holoParams.x), double(holoParams.y), double(holoParams.bx), double(holoParams.by),
                    sampler);
            } else {
                Log::get().note("crisp holo remap: admitted VS %016llX PS %016llX eye %d input %ux%u layer %ux%u inverse (%g,%g) bias (%g,%g); original t1/materials/alpha, seeded stencil and stock write-back retained.",
                    static_cast<unsigned long long>(ui_holo_remap::kVs), static_cast<unsigned long long>(g_draw.holoPsHash),
                    g_draw.eye, g_draw.targetW, g_draw.targetH, layerW, layerH,
                    double(holoParams.x), double(holoParams.y), double(holoParams.bx), double(holoParams.by));
            }
            g_holoTakeNoted[slot] = true;
        }
    }
    return true;
}

bool beginGuarded(ID3D11DeviceContext* ctx, int which) {
    if (!g_draw.decided || g_draw.active || !ctx) return false;
    bool ok = false;
    const bool ran = guardedBudget(g_drawBudget, [&] { ok = beginInner(ctx, which); });
    if (!ran || !ok) {
        // /EHsc does not promise local destructor unwinding for the SEH
        // fault caught by guardedBudget. Explicitly retire its diagnostic pair.
        if (!ran) {
            // A fault inside beginCopy may precede its returned token. Reset
            // every timer, including a partially initialized empty slot.
            const bool reset = guarded("uiLayer.seedProbe.reset", [&] {
                if (!g_hdrSeedGpu.reset(ctx)) g_hdrSeedGpu.reset();
            });
            if (!reset) g_hdrSeedGpu.reset();
            g_hdrSeedActive = 0;
        } else if (g_hdrSeedActive) {
            const bool cancelled = guarded("uiLayer.seedProbe.abort", [&] {
                g_hdrSeedGpu.cancel(ctx, g_hdrSeedActive, UiHdrSeedGpuProbe::Cancel::ExecutionFailed);
            });
            if (!cancelled) g_hdrSeedGpu.reset(); // release-only if the context itself faulted
            g_hdrSeedActive = 0;
        }
        // A timer opened for this issue is discarded: the draw did not issue.
        if (g_draw.hdr && which == 0 && g_draw.drawRoute.slot >= 0) ++g_win.hdrDrawTimingAborted;
        g_draw.drawRoute.cancel([&](int slot) { routeAbort(ctx, slot); });
    }
    if (!ran) {
        // Whatever was changed before the fault, put the game's state back.
        if (g_draw.saved) guarded("uiLayer.restore", [&] { restore(ctx); });
        releaseSaved();
        g_draw.active = false;
        detail::g_uiLayerRedirecting = false;
        standDown("a fault while binding the layer for a draw");
        return false;
    }
    detail::g_uiLayerRedirecting = ok;
    return ok;
}

// ------------------------------------------------------------ the composite

ID3D11ComputeShader* g_cs = nullptr;
bool g_csTried = false;
ID3D11Buffer* g_cb = nullptr;
bool g_fmtChecked[2] = {}, g_fmtOk[2] = {};

void compileOnce(ID3D11DeviceContext* ctx) {
    if (g_cs || g_csTried || !ctx) return;
    g_csTried = true;
    g_cs = shaderSwapCreateCs(ctx,kUiLayerCompositeBytecode,sizeof(kUiLayerCompositeBytecode),"ui_layer_composite_cs","ui quality: layer");
}

bool makeTex(ID3D11Device* dev, uint32_t w, uint32_t h, DXGI_FORMAT texFmt, DXGI_FORMAT viewFmt,
             UINT bind, Ptr<ID3D11Texture2D>* tex, Ptr<ID3D11ShaderResourceView>* srv,
             Ptr<ID3D11UnorderedAccessView>* uav) {
    D3D11_TEXTURE2D_DESC td{};
    td.Width = w;
    td.Height = h;
    td.MipLevels = td.ArraySize = 1;
    td.Format = texFmt;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = bind;
    if (FAILED(dev->CreateTexture2D(&td, nullptr, tex->ReleaseAndGetAddressOf()))) return false;
    if (srv) {
        D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.Format = viewFmt;
        sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        sd.Texture2D.MipLevels = 1;
        if (FAILED(dev->CreateShaderResourceView(tex->Get(), &sd, srv->ReleaseAndGetAddressOf())))
            return false;
    }
    if (uav) {
        D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};
        ud.Format = viewFmt;
        ud.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
        if (FAILED(dev->CreateUnorderedAccessView(tex->Get(), &ud, uav->ReleaseAndGetAddressOf())))
            return false;
    }
    return true;
}

ID3D11Texture2D* compositeInner(Eye& e, uint32_t eye, ID3D11Texture2D* frame,
                                const uint32_t region[4], const float layerUv[4], bool useMult,
                                const char** why) {
    D3D11_TEXTURE2D_DESC fd{};
    frame->GetDesc(&fd);
    const DXGI_FORMAT view = uiLayerFrameView(fd.Format);
    if (view == DXGI_FORMAT_UNKNOWN) {
        *why = "the frame at the door is not an 8-bit UNORM family (the composite blends "
               "in the space the game's UI composites did, and refuses to guess another)";
        return nullptr;
    }
    if (fd.SampleDesc.Count != 1 || fd.ArraySize != 1 || fd.MipLevels != 1) {
        *why = "the frame at the door is multisampled, an array or mipped";
        return nullptr;
    }
    if (region[2] <= region[0] || region[3] <= region[1] || region[2] > fd.Width ||
        region[3] > fd.Height) {
        *why = "the frame's region is empty or off the texture";
        return nullptr;
    }
    const uint32_t rw = region[2] - region[0], rh = region[3] - region[1];
    float uv[4] = {layerUv[0], layerUv[1], layerUv[2], layerUv[3]};
    if (!uiLayerRegionMatches(rw, rh, uv, e.w, e.h)) {
        // The frame this layer was sized for (the door's previous size) has
        // changed aspect this frame -- the per-eye width, an FOV trim: the
        // layer still covers the same frustum and the shader scales each
        // axis on its own, so one frame lands stretched and the next layer
        // is made for the new size. Anything else is a frame that does not
        // hold the layer's eye (half of a double-wide texture): refused.
        const bool resized = e.basisW != e.door.fullW || e.basisH != e.door.fullH;
        if (!resized) {
            *why = "the frame's region does not describe the layer's eye (a half of a "
                   "double-wide texture?)";
            return nullptr;
        }
        static bool stretchNoted = false;
        if (!stretchNoted) {
            stretchNoted = true;
            Log::get().note("ui quality: layer: the door's frame changed shape under a layer made "
                            "for the old one; composited stretched for that frame, the next layer "
                            "is made for the new size.");
        }
    }
    Ptr<ID3D11Device> dev;
    frame->GetDevice(&dev);
    Ptr<ID3D11DeviceContext> ctx;
    if (dev) dev->GetImmediateContext(&ctx);
    if (!dev || !ctx) {
        *why = "no device";
        return nullptr;
    }
    routePoll(ctx.Get());
    g_hdrSeedGpu.poll(ctx.Get());
    compileOnce(ctx.Get());
    if (!g_cs) {
        *why = "the composite shader did not compile";
        return nullptr;
    }
    const int fi = view == DXGI_FORMAT_R8G8B8A8_UNORM ? 0 : 1;
    if (!g_fmtChecked[fi]) {
        g_fmtChecked[fi] = true;
        UINT support = 0;
        g_fmtOk[fi] = SUCCEEDED(dev->CheckFormatSupport(view, &support)) &&
                      (support & D3D11_FORMAT_SUPPORT_TYPED_UNORDERED_ACCESS_VIEW) != 0;
    }
    if (!g_fmtOk[fi]) {
        *why = "no typed unordered-access store for the frame's format on this GPU";
        return nullptr;
    }
    if (!g_cb) {
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = sizeof(UiLayerCompositeParams);
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(dev->CreateBuffer(&bd, nullptr, &g_cb))) {
            g_cb = nullptr;
            *why = "the parameter buffer could not be created";
            return nullptr;
        }
    }
    // The frame's view: over it directly when it allows one, else its region
    // copied out first (the sharpen's rule, for the same reason).
    ID3D11ShaderResourceView* frameSrv = nullptr;
    bool viaCopy = false;
    if (fd.BindFlags & D3D11_BIND_SHADER_RESOURCE) {
        if (e.frameRes != static_cast<void*>(frame) || !e.frameSrv || e.frameView != view) {
            e.frameSrv.Reset();
            e.frameRes = nullptr;
            D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
            sd.Format = view;
            sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            sd.Texture2D.MipLevels = 1;
            if (SUCCEEDED(dev->CreateShaderResourceView(frame, &sd, &e.frameSrv))) {
                e.frameRes = frame;
                e.frameView = view;
            }
        }
        frameSrv = e.frameSrv.Get();
    }
    if (!frameSrv) {
        viaCopy = true;
        if (!e.copy || e.copyW != rw || e.copyH != rh || e.copyFmt != fd.Format) {
            e.copyW = e.copyH = 0;
            if (!makeTex(dev.Get(), rw, rh, fd.Format, view, D3D11_BIND_SHADER_RESOURCE, &e.copy,
                         &e.copySrv, nullptr)) {
                e.copy.Reset();
                e.copySrv.Reset();
                *why = "the frame refuses a shader view and could not be copied";
                return nullptr;
            }
            e.copyW = rw;
            e.copyH = rh;
            e.copyFmt = fd.Format;
        }
        frameSrv = e.copySrv.Get();
    }
    if (!e.out || e.outW != rw || e.outH != rh || e.outFmt != fd.Format) {
        e.outW = e.outH = 0;
        e.out.Reset();
        e.outUav.Reset();
        if (!makeTex(dev.Get(), rw, rh, fd.Format, view,
                     D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS, &e.out, nullptr,
                     &e.outUav)) {
            e.out.Reset();
            e.outUav.Reset();
            *why = "the composite's output texture could not be created";
            return nullptr;
        }
        e.outW = rw;
        e.outH = rh;
        e.outFmt = fd.Format;
        Log::get().note("ui quality: layer: %s eye's composite output %ux%u (%s, %.1f MB).",
                        eye == 0 ? "left" : "right", rw, rh, viewName(view),
                        uiLayerMB(uiLayerBytes(rw, rh)));
    }

    UiLayerCompositeParams p{};
    if (viaCopy) {
        p.region[0] = p.region[1] = 0;
        p.region[2] = static_cast<int32_t>(rw);
        p.region[3] = static_cast<int32_t>(rh);
    } else {
        for (int i = 0; i < 4; ++i) p.region[i] = static_cast<int32_t>(region[i]);
    }
    std::memcpy(p.uv, uv, sizeof(uv));
    p.layerSize[0] = static_cast<float>(e.w);
    p.layerSize[1] = static_cast<float>(e.h);
    p.outSize[0] = rw;
    p.outSize[1] = rh;
    p.mode = 0u;
    p.useMult = useMult ? 1u : 0u;
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(ctx->Map(g_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m)) || !m.pData) {
        *why = "the parameter buffer could not be written";
        return nullptr;
    }
    std::memcpy(m.pData, &p, sizeof(p));
    ctx->Unmap(g_cb, 0);

    // The compute stage is saved and put back around the dispatch (the
    // sharpen's discipline: other passes own slots here).
    ID3D11ComputeShader* savedCs = nullptr;
    ID3D11ShaderResourceView* savedSrv[3] = {};
    ID3D11UnorderedAccessView* savedUav = nullptr;
    ID3D11Buffer* savedCb = nullptr;
    ctx->CSGetShader(&savedCs, nullptr, nullptr);
    ctx->CSGetShaderResources(0, 3, savedSrv);
    ctx->CSGetUnorderedAccessViews(0, 1, &savedUav);
    ctx->CSGetConstantBuffers(0, 1, &savedCb);

    const int qs = routeBegin(ctx.Get(), UiRouteStage::kComposite, static_cast<int>(eye), e.seq);
    gpuCensusBegin(ctx.Get(), GpuCensusSection::DoorUiLayerComposite);
    if (viaCopy) {
        D3D11_BOX box{region[0], region[1], 0, region[2], region[3], 1};
        ctx->CopySubresourceRegion(e.copy.Get(), 0, 0, 0, 0, frame, 0, &box);
    }
    ID3D11ShaderResourceView* nullSrv[3] = {};
    ID3D11UnorderedAccessView* nullUav = nullptr;
    ctx->CSSetShaderResources(0, 3, nullSrv);
    ctx->CSSetUnorderedAccessViews(0, 1, &nullUav, nullptr);
    ctx->CSSetShader(g_cs, nullptr, 0);
    ctx->CSSetConstantBuffers(0, 1, &g_cb);
    ID3D11ShaderResourceView* srvs[3] = {frameSrv, e.srv.Get(), useMult ? e.mSrv.Get() : nullptr};
    ctx->CSSetShaderResources(0, 3, srvs);
    ID3D11UnorderedAccessView* uav = e.outUav.Get();
    ctx->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
    ctx->Dispatch((rw + 7) / 8, (rh + 7) / 8, 1);
    routeEnd(ctx.Get(), qs);
    gpuCensusEnd(ctx.Get(), GpuCensusSection::DoorUiLayerComposite);

    ctx->CSSetShaderResources(0, 3, nullSrv);
    ctx->CSSetUnorderedAccessViews(0, 1, &nullUav, nullptr);
    ctx->CSSetShader(savedCs, nullptr, 0);
    ctx->CSSetShaderResources(0, 3, savedSrv);
    ctx->CSSetUnorderedAccessViews(0, 1, &savedUav, nullptr);
    ctx->CSSetConstantBuffers(0, 1, &savedCb);
    if (savedCs) savedCs->Release();
    for (auto* s : savedSrv)
        if (s) s->Release();
    if (savedUav) savedUav->Release();
    if (savedCb) savedCb->Release();

    ++g_win.composites;
    if (!e.doorFromPass) ++g_win.overGameImage;
    if (!g_compositeNoted) {
        g_compositeNoted = true;
        Log::get().note(
            "ui quality: layer: first composite -- the %s eye's %ux%u layer over a %ux%u %s frame "
            "(layer rectangle u %.3f..%.3f, v %.3f..%.3f), after the upscale and RCAS, before "
            "EDVR's menu%s. The one order that differs from the game's own: whatever the game "
            "drew into an eye AFTER a redirected draw is under the UI now (counted on the gates "
            "line as 'after the UI').",
            eye == 0 ? "left" : "right", e.w, e.h, rw, rh, viewName(view),
            static_cast<double>(uv[0]), static_cast<double>(uv[2]), static_cast<double>(uv[1]),
            static_cast<double>(uv[3]),
            "");
    }
    ID3D11Texture2D* out = e.out.Get();
    out->AddRef();
    return out;
}

// Can a composite run over the frames this door hands on? The format, the
// GPU's typed stores for it and the shader, each refused once with a line
// and cached, so the layer is never armed behind a composite that would
// refuse with UI already in it.
bool doorCanComposite(ID3D11Texture2D* source) {
    D3D11_TEXTURE2D_DESC d{};
    source->GetDesc(&d);
    const DXGI_FORMAT view = uiLayerFrameView(d.Format);
    static bool formatNoted = false, uavNoted = false, shaderNoted = false;
    if (view == DXGI_FORMAT_UNKNOWN || d.SampleDesc.Count != 1 || d.ArraySize != 1 ||
        d.MipLevels != 1) {
        if (!formatNoted) {
            formatNoted = true;
            Log::get().note("ui quality: layer: the pass hands on a %ux%u %s (DXGI_FORMAT %d, %u "
                            "samples, %u mips) -- not an 8-bit UNORM eye; the layer is not armed "
                            "(the composite blends only in the space the game's UI composites "
                            "did).",
                            d.Width, d.Height, viewName(d.Format), static_cast<int>(d.Format),
                            d.SampleDesc.Count, d.MipLevels);
        }
        return false;
    }
    Ptr<ID3D11Device> dev;
    source->GetDevice(&dev);
    Ptr<ID3D11DeviceContext> ctx;
    if (dev) dev->GetImmediateContext(&ctx);
    if (!dev || !ctx) return false;
    const int fi = view == DXGI_FORMAT_R8G8B8A8_UNORM ? 0 : 1;
    if (!g_fmtChecked[fi]) {
        g_fmtChecked[fi] = true;
        UINT support = 0;
        g_fmtOk[fi] = SUCCEEDED(dev->CheckFormatSupport(view, &support)) &&
                      (support & D3D11_FORMAT_SUPPORT_TYPED_UNORDERED_ACCESS_VIEW) != 0;
    }
    if (!g_fmtOk[fi]) {
        if (!uavNoted) {
            uavNoted = true;
            Log::get().note("ui quality: layer: this GPU has no typed unordered-access store for "
                            "%s; the layer is not armed.",
                            viewName(view));
        }
        return false;
    }
    compileOnce(ctx.Get());
    if (!g_cs) {
        if (!shaderNoted) {
            shaderNoted = true;
            Log::get().note("ui quality: layer: the composite shader did not compile (the line "
                            "above says why); the layer is not armed.");
        }
        return false;
    }
    return true;
}

void appendf(std::string& s, const char* fmt, ...) {
    char buf[320];
    va_list ap;
    va_start(ap, fmt);
    const int n = std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n > 0) s.append(buf, static_cast<size_t>(n) < sizeof(buf) ? static_cast<size_t>(n)
                                                                  : sizeof(buf) - 1);
}

// --------------------------------------------------- the world-screen gate

// The most draws a depth target of the 2D screen's own size took in the
// last frame (the depth probe's count; ui_layer_math.h). False while the
// probe is not counting or vScreen does not know the screen.
bool screenDepthDraws(uint32_t* draws, uint32_t* w, uint32_t* h) {
    if (!vScreenPanelSize(w, h)) return false;
    return depthProbeDrawsAtSize(*w, *h, draws);
}

const char* journalReading(bool active, bool known, bool onFoot) {
    return !active ? "off" : !known ? "no Flags2 in Status.json (a menu, or no file yet)"
                   : onFoot ? "on foot"
                            : "aboard";
}

// ---- the on-foot maps gate (ui_maps_math.h): the lines, the 5 s window, the switch in and out
// The key is on and nothing changes: one line per reason (the route's habit), so "on, and silent" is never the answer.
void mapsNoteNotLive(const char* why) {
    Maps& m = g_maps;
    if (m.notLiveWhy && std::strcmp(m.notLiveWhy, why) == 0) return;
    m.notLiveWhy = why;
    char line[640];
    uiMapsFormatNotLive(line, sizeof(line), why);
    Log::get().note("%s", line);
}

// The 5 s window, zeros included while the key is on: an absent line is what "the code never ran" looks like.
void mapsWindowTick(uint64_t now, bool held) {
    Maps& m = g_maps;
    if (!m.windowStartMs) {
        m.windowStartMs = now;
        return;
    }
    if (now - m.windowStartMs < kMapsWindowMs) return;
    char line[760];
    uiMapsFormatWindow(line, sizeof(line), static_cast<double>(now - m.windowStartMs) / 1000.0,
                       m.active ? "naming" : "fallback", held, m.win);
    Log::get().note("%s", line);
    m.win.reset();
    m.windowStartMs = now;
}

// The naming stops deciding the gate (the key went off, screen motion stopped, the layer stopped): one line, the gate is
// today's again from this frame.
void mapsSwitchOut(uint64_t gateFrame, const char* why) {
    Maps& m = g_maps;
    char line[760];
    uiMapsFormatOff(line, sizeof(line), gateFrame, why, !m.gate.world);
    Log::get().note("%s", line);
    m.active = false;
    m.gate = UiMapsGate{};
    m.episodeFrames = 0;
    m.panelLayerOnly = m.panelNotEmpty = 0;
    detail::g_uiLayerMapsOn = false;
}

// The layer is not live: the gate does not run, so neither does the naming. With the key off this is two loads.
void mapsLayerNotLive(uint64_t gateFrame) {
    Maps& m = g_maps;
    if (m.keyCfg != UiMapsKey::On && !m.active) {
        m.windowStartMs = 0;
        return;
    }
    const char* why = uiLayerNotLiveReason();
    if (!why) why = "the UI layer is not live";
    if (m.active) mapsSwitchOut(gateFrame, why);
    if (m.keyCfg != UiMapsKey::On) return;
    ++m.win.notLive;
    mapsNoteNotLive(why);
    mapsWindowTick(GetTickCount64(), g_screenHeld == 1);
}

// One frame of the gate while the layer is live: today's verdict (byJournal || byDepth) is always computed by the caller;
// with the key on and screen motion live the world camera's naming replaces it. Returns the gate for the frame that starts.
bool mapsGate(uint64_t now, uint64_t gateFrame, bool today, bool known, bool onFoot, bool* decided) {
    Maps& m = g_maps;
    const bool keyOn = m.keyCfg == UiMapsKey::On;
    if (!keyOn && !m.active) {
        // The key-off path: nothing else runs, nothing is logged.
        m.windowStartMs = 0;
        detail::g_uiLayerMapsOn = false;
        *decided = false;
        return today;
    }
    const bool smLive = screenMotionLive();
    const bool can = keyOn && smLive;
    char line[900];
    if (can && !m.active) {
        uiMapsCarry(m.gate, today);
        m.active = true;
        m.episodeFrames = 0;
        m.episodeStartMs = now;
        m.panelLayerOnly = m.panelNotEmpty = 0;
        uiMapsFormatOn(line, sizeof(line), gateFrame, today, journalReading(journalWatchActive(), known, onFoot));
        Log::get().note("%s", line);
    } else if (!can && m.active) {
        mapsSwitchOut(gateFrame, keyOn ? "screen motion is not live" : "the key went off");
    }
    if (keyOn && !smLive) {
        ++m.win.notLive;
        mapsNoteNotLive("screen motion is not live (fix.temporal_aa is off, or it stood down)");
    } else if (keyOn) {
        m.notLiveWhy = nullptr;
    }
    bool held = today;
    if (m.active) {
        const bool named = detail::g_uiLayerNamedAt == gateFrame;
        ++m.win.frames;
        if (named) ++m.win.named; else ++m.win.unnamed;
        const UiMapsEdge edge = uiMapsStep(m.gate, named);
        ++m.episodeFrames;
        const double seconds = static_cast<double>(now - m.episodeStartMs) / 1000.0;
        if (edge == UiMapsEdge::Release) {
            ++m.win.releases;
            uiMapsFormatTake(line, sizeof(line), gateFrame, kUiMapsReleaseFrames, m.episodeFrames, seconds,
                             journalReading(journalWatchActive(), known, onFoot));
            Log::get().note("%s", line);
            m.episodeFrames = 0;
            m.episodeStartMs = now;
            m.panelLayerOnly = m.panelNotEmpty = 0;
        } else if (edge == UiMapsEdge::Hold) {
            ++m.win.holds;
            char why[96];
            uiMapsFormatNamedWhy(why, sizeof(why), kUiMapsHoldFrames);
            uiMapsFormatHandBack(line, sizeof(line), gateFrame, m.episodeFrames, seconds, m.panelLayerOnly, m.panelNotEmpty, why);
            Log::get().note("%s", line);
            m.episodeFrames = 0;
            m.episodeStartMs = now;
            m.panelLayerOnly = m.panelNotEmpty = 0;
        }
        held = m.gate.world;
        if (held) ++m.win.worldFrames; else ++m.win.panelFrames;
    }
    *decided = m.active;
    detail::g_uiLayerMapsOn = m.active;
    if (keyOn) mapsWindowTick(now, held); else { m.windowStartMs = 0; m.win.reset(); }
    return held;
}

// Once a frame while the layer is live, on the render thread -- the thread
// that ticks the journal watcher and counts the depth probe's draws. Every
// flip of the combined gate is one line, either way, never rate-limited,
// naming which signal held and the count it judged by.
void onFootGateTick() {
    // The frame that is ending: a draw that names the screen's source attributes itself to this count (ui_layer.h).
    const uint64_t gateFrame = detail::g_uiLayerGateFrame++;
    // A VR question: the flat profile's half takes no 2D screen, so its frames run the gate as a layer that is not live
    // always has (no journal or depth reading, no lines).
    if (runtimeFlatProfile()) {
        mapsLayerNotLive(gateFrame);
        return;
    }
    if (!detail::g_uiLayerLive) {
        mapsLayerNotLive(gateFrame);
        return;
    }
    const bool active = journalWatchActive();
    const bool known = journalOnFootKnown(), onFoot = journalOnFoot();
    const uint64_t now = GetTickCount64();
    const bool byJournal = uiLayerOnFootStep(g_onFoot, known, onFoot, now);
    uint32_t draws = 0, sw = 0, sh = 0;
    const bool counted = screenDepthDraws(&draws, &sw, &sh);
    const bool byDepth = uiLayerWorldScreenStep(g_world, counted, draws);
    ++g_win.gateReads;
    if (byJournal && byDepth) {
        ++g_win.heldBoth;
    } else if (byJournal) {
        ++g_win.heldJournal;
    } else if (byDepth) {
        ++g_win.heldDepth;
    }
    if (counted) {
        ++g_win.depthCounted;
        if (draws > g_win.depthMax) g_win.depthMax = draws;
        uint32_t focus = 0;
        const size_t slot = !journalGuiFocus(&focus)                 ? Window::kFocusSlots - 1
                            : focus < Window::kFocusSlots - 2 ? focus
                                                              : Window::kFocusSlots - 2;
        g_win.focusSeen[slot] = true;
        if (draws > g_win.focusMax[slot]) g_win.focusMax[slot] = draws;
    }
    if (!active && !g_journalOffNoted) {
        g_journalOffNoted = true;
        Log::get().note("ui quality: layer: the journal watcher is not reading the game's Status.json "
                        "(d3d11.journal_watch off, or the journal folder not found) -- %s.",
                        counted ? "the 2D screen is told to be the world by its own depth alone"
                                : "and the depth probe is not counting the screen's depth either: the "
                                  "2D screen is taken on foot too, where it is the world");
    }
    const int8_t before = g_screenHeld;
    // Today's verdict, unless screen motion runs: then the world camera's naming
    // decides and says so in its own lines (mapsGate), and the lines below -- which speak of the journal and the depth -- stay quiet.
    bool decidedByNaming = false;
    const bool held = mapsGate(now, gateFrame, byJournal || byDepth, known, onFoot, &decidedByNaming);
    g_screenHeld = held ? 1 : 0;
    if (g_screenHeld == before || decidedByNaming) return;
    char depth[128];
    if (counted) {
        _snprintf_s(depth, sizeof(depth), _TRUNCATE, "%u draws a frame into its %ux%u depth target",
                    draws, sw, sh);
    } else {
        _snprintf_s(depth, sizeof(depth), _TRUNCATE,
                    "not counted (the depth probe is off, or the screen's size is not known)");
    }
    const char* journal = journalReading(active, known, onFoot);
    if (held) {
        g_heldSinceMs = now;
        Log::get().note(
            "ui quality: layer: the 2D screen shows the world%s -- held by %s (the journal: %s; the "
            "screen's depth: %s, over %u for %u frames holds it) -- it stays in the game's frame for "
            "the temporal pass, the helmet HUD with it; the rest of the UI goes into the layer as before.",
            before < 0 ? ", at the gate's first reading" : "",
            byJournal && byDepth ? "both signals" : byJournal ? "the journal" : "the screen's own depth",
            journal, depth, kUiWorldEnterDraws, kUiWorldEnterFrames);
    } else if (before < 0) {
        Log::get().note("ui quality: layer: the 2D screen is not the world at the gate's first reading "
                        "(the journal: %s; the screen's depth: %s) -- it goes into the layer.",
                        journal, depth);
    } else {
        Log::get().note("ui quality: layer: the 2D screen no longer shows the world after %.1f s (the "
                        "journal: %s; the screen's depth: %s, under %u for %u frames lets go) -- it goes "
                        "into the layer again.",
                        static_cast<double>(now - g_heldSinceMs) / 1000.0, journal, depth,
                        kUiWorldLeaveDraws, kUiWorldLeaveFrames);
    }
}

// The 30 s line of the world-screen gate: its state, both signals, the frames
// each held it, and the screen's depth by GuiFocus -- the counts the
// thresholds were set from, now on the screens no log had caught.
void logWorldScreen() {
    std::string focus;
    for (size_t i = 0; i < Window::kFocusSlots; ++i) {
        if (!g_win.focusSeen[i]) continue;
        char name[48];
        if (i == Window::kFocusSlots - 1) {
            _snprintf_s(name, sizeof(name), _TRUNCATE, "GuiFocus unknown");
        } else if (i == Window::kFocusSlots - 2) {
            _snprintf_s(name, sizeof(name), _TRUNCATE, "GuiFocus 12 or more");
        } else {
            _snprintf_s(name, sizeof(name), _TRUNCATE, "%u %s", static_cast<unsigned>(i),
                        uiGuiFocusName(static_cast<uint32_t>(i)));
        }
        appendf(focus, "%s%s %u", focus.empty() ? "" : ", ", name, g_win.focusMax[i]);
    }
    const bool active = journalWatchActive();
    Log::get().note(
        "ui quality: world screen: the gate %s (the journal: %s; the screen's depth: %u draws a frame "
        "now, %u at most this window, %llu frames counted; over %u for %u frames holds it, under %u for "
        "%u frames lets go); held %llu of %llu frames -- %llu by the journal alone, %llu by the depth "
        "alone, %llu by both; %llu 2D screen draws asked, %llu left in the picture, %llu re-issued into the "
        "layer by the VR world route; the screen's depth at most, by GuiFocus: %s%s.",
        g_screenHeld == 1 ? "holds" : g_screenHeld == 0 ? "is open" : "has never been read",
        journalReading(active, journalOnFootKnown(), journalOnFoot()), g_world.draws, g_win.depthMax,
        static_cast<unsigned long long>(g_win.depthCounted), kUiWorldEnterDraws, kUiWorldEnterFrames,
        kUiWorldLeaveDraws, kUiWorldLeaveFrames,
        static_cast<unsigned long long>(g_win.heldJournal + g_win.heldDepth + g_win.heldBoth),
        static_cast<unsigned long long>(g_win.gateReads),
        static_cast<unsigned long long>(g_win.heldJournal),
        static_cast<unsigned long long>(g_win.heldDepth),
        static_cast<unsigned long long>(g_win.heldBoth),
        static_cast<unsigned long long>(g_win.screenAsked),
        static_cast<unsigned long long>(
            g_win.decided[static_cast<size_t>(UiLayerFamily::kScreen)]
                         [static_cast<size_t>(UiLayerDecision::kWorldScreen)]),
        static_cast<unsigned long long>(g_win.worldReissued),
        focus.empty() ? "nothing counted" : focus.c_str(),
        g_maps.active ? " -- the gate is decided by the world camera's naming, not by these two "
                        "signals; see the \"on foot maps sharp 5s:\" line"
                      : "");
}

// The VR world route's own line: the screen draws the layer re-issued and, for the ones it did not, why -- the
// route's refusals by reason and the decision's own refusals of the screen family (decided[kScreen][...], the
// same numbers the families line carries). Printed while the key is auto, zeros included: an absent line is what
// "the route never reached the layer" looks like.
void logWorldRoute(double seconds) {
    std::string route, decision;
    for (size_t r = 1; r < static_cast<size_t>(UiWorldRefuse::kCount); ++r) {
        const uint64_t n = g_win.worldRefused[r];
        if (!n) continue;
        appendf(route, "%s%s=%llu", route.empty() ? "" : ", ", uiWorldRefuseKey(static_cast<UiWorldRefuse>(r)),
                static_cast<unsigned long long>(n));
    }
    const size_t screen = static_cast<size_t>(UiLayerFamily::kScreen);
    for (size_t d = 1; d < static_cast<size_t>(UiLayerDecision::kCount); ++d) {
        if (d == static_cast<size_t>(UiLayerDecision::kWorldScreen)) continue;  // the eye route's frames
        const uint64_t n = g_win.decided[screen][d];
        if (!n) continue;
        appendf(decision, "%s%s=%llu", decision.empty() ? "" : ", ", uiLayerDecisionKey(static_cast<UiLayerDecision>(d)),
                static_cast<unsigned long long>(n));
    }
    Log::get().note(
        "vr world route layer: %.0f s; %llu screen draws re-issued into the layer (%.2f a frame); refused by the "
        "route's own checks: %s; refused by the decision's tests (every 2D screen draw this window, owned frames or "
        "not): %s; %llu draws into a re-issued eye left in the game's frame (lost while the route owns that eye). "
        "The first eight distinct reasons are named in full, once each, in the lines \"vr world route: layer did not "
        "take the screen draw for eye N\".",
        seconds, static_cast<unsigned long long>(g_win.worldReissued),
        static_cast<double>(g_win.worldReissued) / (g_win.frames ? static_cast<double>(g_win.frames) : 1.0),
        route.empty() ? "none" : route.c_str(), decision.empty() ? "none" : decision.c_str(),
        static_cast<unsigned long long>(g_win.worldLeftDraws));
}

// ------------------------------------------------------------ the totals

// Bytes a pixel, for the formats the layer allocates: its own RGBA8
// targets, the frame's families for the composite output and its copy,
// and the game's depth-stencil families for the seed's target and copy.
uint32_t formatBytes(DXGI_FORMAT f) {
    switch (f) {
        case DXGI_FORMAT_R16G16B16A16_FLOAT:
        case DXGI_FORMAT_R32G8X24_TYPELESS:
        case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
            return 8u;
        case DXGI_FORMAT_R16_TYPELESS:
        case DXGI_FORMAT_D16_UNORM:
            return 2u;
        default:
            return 4u;  // the 8-bit RGBA families, R10G10B10A2, R11G11B10, D24S8, D32
    }
}

void appendResource(std::string& s, const char* name, uint32_t w, uint32_t h, DXGI_FORMAT f,
                    uint64_t* total) {
    if (!w || !h) return;
    const uint64_t bytes = uiLayerBytes(w, h, formatBytes(f));
    *total += bytes;
    appendf(s, "%s%s %ux%u %s %.1f MB", s.empty() ? "" : ", ", name, w, h, viewName(f),
            uiLayerMB(bytes));
}

// Every layer resource allocated right now, by eye, with its size -- not
// the design's estimate: the multiply transmittance and the depth-stencil
// pair exist only once a multiply or a tested draw has been seen, and a
// frame copy only when the frame refuses a shader view.
void logMemory() {
    std::string s;
    uint64_t total = 0;
    for (int i = 0; i < 2; ++i) {
        const Eye& e = g_eye[i];
        std::string one;
        if (e.tex) appendResource(one, "layer colour", e.w, e.h, DXGI_FORMAT_R8G8B8A8_UNORM, &total);
        if (e.out) appendResource(one, "composite output", e.outW, e.outH, e.outFmt, &total);
        if (e.copy) appendResource(one, "frame copy", e.copyW, e.copyH, e.copyFmt, &total);
        if (e.mTex) {
            appendResource(one, "multiply transmittance", e.mW, e.mH, DXGI_FORMAT_R8G8B8A8_UNORM,
                           &total);
        }
        if (e.ds.tex) appendResource(one, "depth-stencil", e.ds.w, e.ds.h, e.ds.viewFmt, &total);
        if (e.hdrTex)
            appendResource(one, "HDR HUD layer", e.hdrW, e.hdrH, DXGI_FORMAT_R16G16B16A16_FLOAT,
                           &total);
        if (e.hdrDs.tex)
            appendResource(one, "HDR HUD depth-stencil", e.hdrDs.w, e.hdrDs.h, e.hdrDs.viewFmt,
                           &total);
        if (e.dsCopy) {
            appendResource(one, "its copy of the game's depth-stencil", e.dsCopyW, e.dsCopyH,
                           e.dsCopyFmt, &total);
        }
        appendf(s, "%s%s eye -- %s", i ? "; " : "", i ? "right" : "left",
                one.empty() ? "nothing" : one.c_str());
    }
    Log::get().note("ui quality: memory: allocated now -- %s; %.1f MB in all.", s.c_str(),
                    uiLayerMB(total));
}

// The layer's half of a scene-line family's own 30 s line (the orbit lines': orbital_width.cpp; the supercruise bars':
// supercruise_bars.cpp), from the decided table the families line reads: how many draws the layer took this window, at what
// rate, and how many it left in the scene by decision key. A window in which the code never ran says "0 in the HDR layer, 0
// left in the scene" -- no draw reached the decision at all -- which is not what a decline looks like.
std::string sceneLineLayerText(UiLayerFamily family, double frames) {
    const size_t fi = static_cast<size_t>(family);
    const uint64_t taken = g_win.decided[fi][static_cast<size_t>(UiLayerDecision::kRedirect)];
    std::string left;
    uint64_t leftTotal = 0;
    for (size_t d = 1; d < static_cast<size_t>(UiLayerDecision::kCount); ++d) {
        const uint64_t n = g_win.decided[fi][d];
        if (!n) continue;
        leftTotal += n;
        appendf(left, "%s%s %llu", left.empty() ? "" : ", ", uiLayerDecisionKey(static_cast<UiLayerDecision>(d)),
                static_cast<unsigned long long>(n));
    }
    std::string text;
    appendf(text, "%llu in the HDR layer (%.2f a frame), %llu left in the scene%s%s%s",
            static_cast<unsigned long long>(taken), static_cast<double>(taken) / frames,
            static_cast<unsigned long long>(leftTotal), left.empty() ? "" : " (", left.c_str(), left.empty() ? "" : ")");
    return text;
}

void logTotals(double seconds) {
    // The frosted base's own count: the flight's answer to "did the station menu's base go through the
    // remapped program" -- admitted must climb with the panels, refused stay 0.
    const size_t frosted = static_cast<size_t>(ui_holo_remap::index(ui_holo_remap::kFrostedPs));
    Log::get().note("crisp holo remap: cumulative capture_calls %llu captured %llu eligible %llu prepared %llu admitted %llu refused %llu; frosted base admitted %llu refused %llu; three exact PS only, zero admitted means no successful route, no extra scene-depth copy.",
        static_cast<unsigned long long>(g_holoCaptureCalls.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(g_holoCaptured.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(g_holoEligible), static_cast<unsigned long long>(g_holoPrepared),
        static_cast<unsigned long long>(g_holoTaken), static_cast<unsigned long long>(g_holoRefused),
        static_cast<unsigned long long>(g_holoTakenBy[frosted]),
        static_cast<unsigned long long>(g_holoRefusedBy[frosted]));
    g_seedCensus.report([](const char* line) { Log::get().note("%s", line); });
    g_hdrSeedGpu.report(g_hdrDrawTimingOn, [](const char* line) { Log::get().note("%s", line); });
    const double frames = g_win.frames ? static_cast<double>(g_win.frames) : 1.0;
    std::string taken, left;
    for (size_t f = 1; f < static_cast<size_t>(UiLayerFamily::kCount); ++f) {
        const uint64_t n = g_win.decided[f][static_cast<size_t>(UiLayerDecision::kRedirect)];
        if (n) {
            appendf(taken, "%s%s %.2f", taken.empty() ? "" : ", ",
                    uiLayerFamilyName(static_cast<UiLayerFamily>(f)),
                    static_cast<double>(n) / frames);
        }
        for (size_t d = 2; d < static_cast<size_t>(UiLayerDecision::kCount); ++d) {
            const uint64_t k = g_win.decided[f][d];
            if (!k) continue;
            appendf(left, "%s%s %.2f a frame (%s)", left.empty() ? "" : "; ",
                    uiLayerFamilyName(static_cast<UiLayerFamily>(f)),
                    static_cast<double>(k) / frames,
                    uiLayerDecisionName(static_cast<UiLayerDecision>(d)));
        }
    }
    // The panels and the layer on lines of their own: Log's line holds 1200
    // characters. The panels' line is the engine-side sizing's own.
    uiPanelScaleLog();     // the engine-side panel sizing: its factor, or why it stands down
    // The two supercruise line draws (2026-10-07): the orbit lines made at the same factor (orbital_width.h), or why they are not,
    // and how many of them the HDR layer took; the supercruise bars' own pass (supercruise_bars.h) and how many it took.
    orbitalWidthLog(sceneLineLayerText(UiLayerFamily::kOrbitLines, frames).c_str());
    supercruiseBarsLog(sceneLineLayerText(UiLayerFamily::kSupercruiseBars, frames).c_str());
    // The space dust has no module of its own (it is taken as the game draws it): its whole account is the decided table's,
    // printed with zeros like the other two so that a log without the line is a build that never ran it, and a window in which no
    // dust reached the decision reads "0 in the HDR layer, 0 left in the scene" -- which is not what a decline looks like.
    Log::get().note("ui quality: space dust: %s.", sceneLineLayerText(UiLayerFamily::kSpaceDust, frames).c_str());
    uiSurfacesLogAtlas();  // the glyph atlas instrument's write counts, when one is watched
    // The price: each stage's GPU time per eye-frame it ran in, and the
    // route's -- every stage of an eye-frame added up -- per eye-frame the
    // layer did anything in.
    std::string price;
    for (size_t si = 0; si < kStages; ++si) {
        if (si == static_cast<size_t>(UiRouteStage::kHdrMovedDraw)) continue;
        appendf(price, "%s%s ", si ? ", " : "", uiRouteStageName(static_cast<UiRouteStage>(si)));
        appendPrice(price, si);
    }
    price += "; machinery route ";
    appendPrice(price, kRouteTotal);
    uint64_t untimed = 0, invalid = 0, lateSamples = 0;
    for (size_t si = 0; si < kStages; ++si) {
        if (si == static_cast<size_t>(UiRouteStage::kHdrMovedDraw)) continue;
        untimed += g_win.routeUntimed[si];
        invalid += g_win.routeInvalid[si];
        lateSamples += g_win.routeLate[si];
    }
    if (untimed || invalid || lateSamples) {
        appendf(price, " (%llu intervals had no free timer, %llu did not measure, %llu arrived after "
                       "their frame closed: their eye-frames are left out)",
                static_cast<unsigned long long>(untimed), static_cast<unsigned long long>(invalid),
                static_cast<unsigned long long>(lateSamples));
    }
    const Eye& l = g_eye[0];
    Log::get().note(
        "ui quality: layer seed preservation: %llu stencil-only game writers kept an unmodified "
        "depth-only seed; matched layer/eye events, not avoided-seed count; %llu first-issue attempts "
        "with raw depth-enabled/write-ALL state activated a conservative whole-frame guard "
        "across both eyes/layers (potential writes, including no private DSV or original read-only view).",
        static_cast<unsigned long long>(g_win.depthOnlySeedPreservedWriters),
        static_cast<unsigned long long>(g_win.privateDepthPotentialBegins));
    Log::get().note(
        "ui quality: layer: %.0f s, %llu frames, %ux%u per eye + composite output %ux%u; %.2f draws "
        "a frame redirected (%s), %.2f multiplies, %.2f depth/stencil write-backs, %.2f tested "
        "against a seeded copy (%llu seeds, %llu failed, %llu stale); per frame %.2f viewport remaps, "
        "%.2f scissor remaps, %.2f jitter cancels; %llu composites (%llu over the game's own image); "
        "GPU ms per eye-frame, median/p95 (eye-frames): %s.",
        seconds, static_cast<unsigned long long>(g_win.frames), l.w, l.h, l.outW, l.outH,
        static_cast<double>(g_win.redirected) / frames, taken.empty() ? "none" : taken.c_str(),
        static_cast<double>(g_win.multiplies) / frames,
        static_cast<double>(g_win.writeBacks) / frames,
        static_cast<double>(g_win.dsTested) / frames,
        static_cast<unsigned long long>(g_win.seeds),
        static_cast<unsigned long long>(g_win.seedFailures),
        static_cast<unsigned long long>(g_win.seedStale),
        static_cast<double>(g_win.viewportRemaps) / frames,
        static_cast<double>(g_win.scissorRemaps) / frames,
        static_cast<double>(g_win.jitterCancels) / frames,
        static_cast<unsigned long long>(g_win.composites),
        static_cast<unsigned long long>(g_win.overGameImage), price.c_str());
    std::string moved, combined;
    const size_t movedStage = static_cast<size_t>(UiRouteStage::kHdrMovedDraw);
    appendPrice(moved, movedStage);
    appendPrice(combined, kRouteWithMoved);
    Log::get().note(
        "ui quality: HDR draw price: diagnostics_current=%u eligible=%llu disabled=%llu issued=%llu "
        "aborted=%llu unavailable=%llu invalid=%llu late=%llu; GPU ms per eye-frame median/p95 (eye-frames): "
        "moved_rendering=%s machinery_plus_moved=%s; moved rendering is game shading at the "
        "layer size, not net overhead; combined samples require every interval armed and valid, "
        "unarmed eye-frames are excluded; retained samples may predate a config toggle. "
        "Stage medians are not additive.",
        g_hdrDrawTimingOn ? 1u : 0u,
        static_cast<unsigned long long>(g_win.hdrDrawTimingEligible),
        static_cast<unsigned long long>(g_win.hdrDrawTimingDisabled),
        static_cast<unsigned long long>(g_win.hdrDrawTimingIssued),
        static_cast<unsigned long long>(g_win.hdrDrawTimingAborted),
        static_cast<unsigned long long>(g_win.routeUntimed[movedStage]),
        static_cast<unsigned long long>(g_win.routeInvalid[movedStage]),
        static_cast<unsigned long long>(g_win.routeLate[movedStage]), moved.c_str(), combined.c_str());
    logMemory();
    if (detail::g_uiLayerCrispOn) {
        // The HDR HUD half's line: the take (the cockpit HUD
        // families, holograms included), the re-issues and
        // coverage passes, what never reached a tonemap, and the declines by
        // reason (each decline is also named once a session where it
        // happened). The taken/left/refused per family WITH the reason is the
        // decided table on the lines around this one.
        std::string declines;
        for (size_t d = 0; d < static_cast<size_t>(CrispToneDecline::kCount); ++d) {
            const uint64_t n = g_win.hdrDeclined[d];
            if (!n) continue;
            appendf(declines, "%s%llu %s", declines.empty() ? "" : "; ",
                    static_cast<unsigned long long>(n),
                    crispToneDeclineName(static_cast<CrispToneDecline>(d)));
        }
        Log::get().note(
            "crisp hud: %.2f HUD draws a frame taken into the HDR layer, %.2f tonemap "
            "re-issues and %.2f coverage passes a frame; %llu HDR layers' content never reached "
            "a tonemap; re-issue declines: %s.",
            static_cast<double>(g_win.hdrRedirected) / frames,
            static_cast<double>(g_win.hdrReissued) / frames,
            static_cast<double>(g_win.hdrCoveragePasses) / frames,
            static_cast<unsigned long long>(g_win.hdrLost),
            declines.empty() ? "none" : declines.c_str());
    }
    Log::get().note("ui quality: left in the game's frame: %s.",
                    left.empty() ? "nothing classified" : left.c_str());
    // The composites no decision above covers (ui_scene_composites.h): every draw into an eye that
    // samples a learned interface surface, taken or left in the game's frame, the left ones named by
    // shaders and family. Printed with zeros, every window the layer prints at all, so a log without
    // this line is a build, or a code path, that never ran it.
    {
        char composites[1100];
        uiSceneCompositeFormat(composites, sizeof(composites), g_win.scene, g_win.frames, uiDepthWantsDraws());
        Log::get().note("%s", composites);
    }
    // The family census: what the family rule made of the two composites'
    // draws -- the ones it turned away never reach a decision above.
    {
        std::string census;
        static const char* const kProbeName[2] = {"menu panel composite (vs A888D51024D9798E)",
                                                  "loading screen composite (vs 4EF6DDB075A927FA)"};
        const UiLayerFamily kProbeFamily[2] = {UiLayerFamily::kPanel, UiLayerFamily::kLoader};
        for (int k = 0; k < 2; ++k) {
            uint64_t total = 0;
            for (uint64_t n : g_win.probe[k]) total += n;
            if (!total) continue;
            const size_t fi = static_cast<size_t>(kProbeFamily[k]);
            uint64_t leftN = 0;
            for (size_t d = 2; d < static_cast<size_t>(UiLayerDecision::kCount); ++d) leftN += g_win.decided[fi][d];
            appendf(census, "%s%s: %llu draws -- %s; decided as the %s: %llu redirected, %llu left",
                    census.empty() ? "" : "; ", kProbeName[k], static_cast<unsigned long long>(total),
                    probeText(g_win.probe[k], k).c_str(), uiLayerFamilyName(kProbeFamily[k]),
                    static_cast<unsigned long long>(
                        g_win.decided[fi][static_cast<size_t>(UiLayerDecision::kRedirect)]),
                    static_cast<unsigned long long>(leftN));
        }
        Log::get().note("ui quality: families: %s.", census.empty() ? "neither composite drawn" : census.c_str());
    }
    uint64_t late = 0;
    for (size_t f = 1; f < static_cast<size_t>(UiLayerFamily::kCount); ++f)
        late += g_win.decided[f][static_cast<size_t>(UiLayerDecision::kLate)];
    Log::get().note(
        "ui quality: gates: G1 -- %llu UI draws arrived after their eye's composite had run "
        "(left in the game's frame); %llu layers never reached the door; the door ran %llu "
        "times, the pass treated %llu eyes; %llu composites refused; %llu redirected draws "
        "refused at issue (a changed blend, or a seed that failed); after the UI the game drew "
        "%llu times into an eye target the UI was taken from: %llu taken into the layer after "
        "the UI (kept over it), %llu left as post passes (eye-sized input), %llu declined by "
        "the layer's rules (the families line's 'after the UI' says which), %llu refused at "
        "issue; %llu times read one (a post pass: the UI misses it), %llu of them carrying the "
        "eye on into the target the interface draws to; eye check against the "
        "game's Submit: %llu matched, %llu SWAPPED, %llu could not be told; %llu redirected "
        "this session%s.",
        static_cast<unsigned long long>(late), static_cast<unsigned long long>(g_win.lostLayers),
        static_cast<unsigned long long>(g_win.doors), static_cast<unsigned long long>(g_win.treated),
        static_cast<unsigned long long>(g_win.compositeRefused),
        static_cast<unsigned long long>(g_win.refusedAtIssue),
        static_cast<unsigned long long>(g_win.afterWrites),
        static_cast<unsigned long long>(g_win.afterTaken),
        static_cast<unsigned long long>(g_win.afterPostPass),
        static_cast<unsigned long long>(g_win.afterDeclined),
        static_cast<unsigned long long>(g_win.afterRefused),
        static_cast<unsigned long long>(g_win.afterReads),
        static_cast<unsigned long long>(g_win.afterFollowed),
        static_cast<unsigned long long>(g_win.eyeMatched),
        static_cast<unsigned long long>(g_win.eyeSwapped),
        static_cast<unsigned long long>(g_win.eyeUntold),
        static_cast<unsigned long long>(g_sessionRedirected),
        g_stoodDown ? " -- the layer STOOD DOWN (the line above says why)" : "");
    logWorldScreen();
    bool worldRefused = false;
    for (const uint64_t n : g_win.worldRefused) worldRefused = worldRefused || n != 0;
    if (vrWorldRouteEnabled() || g_win.worldReissued || g_win.worldLeftDraws || worldRefused) logWorldRoute(seconds);
}

}  // namespace

void uiLayerRememberHoloPs(ID3D11PixelShader* shader, uint64_t hash,
                           const void* bytes, size_t count, bool linked) {
    const int slot = ui_holo_remap::index(hash);
    if (slot < 0) return;
    g_holoCaptureCalls.fetch_add(1, std::memory_order_relaxed);
    bool ok = false;
    const bool ran = guarded("ui.holo.remember", [&] { ok = g_holoCache.remember(shader, hash, bytes, count, linked); });
    if (ran && ok) {
        g_holoCaptured.fetch_add(1, std::memory_order_relaxed);
        if (!g_holoCaptureNoted[slot].exchange(true, std::memory_order_relaxed))
            Log::get().note("crisp holo remap: captured verified PS %016llX %zu bytes (%s), exact SV_Position edit and free shader b13; not yet admitted.",
                static_cast<unsigned long long>(hash), count,
                ui_holo_remap::kindName(ui_holo_remap::kindOf(slot)));
    } else if (!g_holoCaptureRefusalNoted[slot].exchange(true, std::memory_order_relaxed)) {
        Log::get().note("crisp holo remap: PS %016llX capture refused (bytes/linkage/private identity unavailable); original shader untouched.",
            static_cast<unsigned long long>(hash));
    }
}

// The crisp take's whole dependency set, established no later than the first
// HUD take (review R2) -- defined beside the coverage machinery, below.
bool crispTakeReady(ID3D11DeviceContext* ctx, int eye);

// --------------------------------------------------------------- the API

void uiLayerConfigure(Config& cfg) {
    detail::g_uiSeedDiagnostics = g_hdrDrawTimingOn;
    g_seedCensus.configure(g_hdrDrawTimingOn);
    // The fallback, for an ini with no such line (a hand-copied DLL over an old
    // file, a deleted line), is the shipped default: 100 since 2026-09-29. A line
    // that is present and not off/100/125 is refused below and reads as off.
    const std::string text = cfg.getString("fix.ui_quality", "100");
    bool recognized = true;
    const char* newSpelling = nullptr;
    const float target = uiQualityParse(text.c_str(), &recognized, &newSpelling);
    if (newSpelling && !g_aliasNoted) {
        g_aliasNoted = true;
        Log::get().note("ui quality: fix.ui_quality = %s is the first spelling, read as %s (%s); write "
                        "ui_quality = %s -- the old one is read for this release only.",
                        text.c_str(), newSpelling, uiQualityLabel(target), newSpelling);
    }
    const bool temporalVr = temporalModeEnabled(cfg.getString("fix.temporal_aa", "off"));
    // The flat profile reads its mode around the gate (fix.temporal_aa reads off there): its half of the layer -- the cockpit
    // HUD families through the mono adapter, flat_ui_layer.cpp -- and its panel factor arm with the flat anti-aliasing.
    const bool flatAa = runtimeFlatProfile() && temporalModeEnabled(cfg.requestedTemporalMode());
    const bool temporal = runtimeFlatProfile() ? flatAa : temporalVr;
    // The cancel follows the shipped jitter convention (as_is, no lag), always. The flat profile's jitter is the
    // flat phase machine's, read per draw from the camera rows (flat_ui_layer.cpp): as shipped by construction.
    const bool changed = !g_keyNoted || text != g_keyText || target != g_target ||
                         temporal != g_temporal;
    // A live change of the key re-arms a stood-down layer, either half of it
    // (the LDR take's or the HDR HUD path's): the player asked.
    if (g_keyNoted && (text != g_keyText || target != g_target)) {
        g_stoodDown = false;
        g_crispStoodDown = false;
    }
    g_keyText = text;
    g_target = target;
    g_temporal = temporal;
    refreshLive();
    // The panels follow the same key -- one setting, both halves -- and the
    // instruments with them.
    uiSurfacesSetTarget(target);
    uiPanelScaleSetTarget(target);
    // The flat profile's panel factor (ui_panel_scale.cpp) needs its anti-aliasing on too.
    if (runtimeFlatProfile()) uiPanelScaleSetFlatTemporal(flatAa);
    if (!changed) return;
    g_keyNoted = true;
    if (!recognized) {
        Log::get().note("ui quality: fix.ui_quality = '%s' is not off, 100 or 125 -- off.",
                        text.c_str());
        return;
    }
    if (target <= 0.0f) {
        Log::get().note("ui quality: off -- the game's UI surfaces and composites are drawn as "
                        "they always were.");
        return;
    }
    if (runtimeFlatProfile()) {
        Log::get().note(
            "ui quality: %s (flat; the layer is %s) -- %s",
            uiQualityLabel(target),
            detail::g_uiLayerCrispOn ? "live" : uiLayerNotLiveReasonFor(g_target, g_temporal, g_jitterAsShipped, g_stoodDown)
                                              ? uiLayerNotLiveReasonFor(g_target, g_temporal, g_jitterAsShipped, g_stoodDown)
                                              : "not live: the HDR HUD path stood down",
            flatAa ? "panels: the game's own panel formula makes every render-to-texture panel at the display's size "
                     "times the target, whatever the render size, from the next panel init or view change (the \"ui "
                     "quality: panels (flat)\" lines give the factor); cockpit HUD: the holo panels, the flight HUD and the "
                     "target sprite (not the holograms, which stay in the frame) are drawn unjittered into an HDR layer at the display's size times "
                     "the target, tonemapped by the game's own tonemap draw re-issued over it, and composited after the "
                     "resolve and the sharpening, at the game's output copy (the \"flat ui layer\" lines count them)."
                   : "waits: anti-aliasing is off, so the panels stay at the game's own size and the cockpit HUD in the "
                     "game's frame until it is on.");
        return;
    }
    float hmd = 0.0f;
    const bool hmdKnown = deviceHookHmdQuality(&hmd) && hmd > 0.0f;
    char hmdText[64] = "unknown";
    if (hmdKnown) std::snprintf(hmdText, sizeof(hmdText), "%.2f", static_cast<double>(hmd));
    Log::get().note(
        "ui quality: %s (HMD Quality %s) -- panels: the game's own panel formula makes every "
        "render-to-texture panel at its untrimmed size at HMD Quality %.2f; layer: %s",
        uiQualityLabel(target), hmdText, static_cast<double>(target),
        !temporal ? "waits -- fix.temporal_aa is off, and the layer is composited at that "
                    "pass's door."
        : "the game's post-tonemap UI (the 2D screen, the menus, the loading screen) is drawn "
              "by its own shaders into a per-eye layer at that size times the door's output, "
              "unjittered, and composited after the upscale and RCAS, before EDVR's menu -- "
              "except the 2D screen while it shows the world -- on foot, or a 3D map -- where it "
              "stays in the picture for the temporal pass (the game's Status.json says on foot, a "
              "second or so late; the screen's own depth, busy with the world, says so within "
              "two frames); the cockpit's holo panels, the flight HUD, the target sprite and the "
              "holograms (the radar's icons and contacts -- the glass canopy stays) are drawn "
              "into a per-eye HDR layer at the "
              "same size and tonemapped over the finished eye by the game's own tonemap draw "
              "re-issued with the layer as its HDR source (their bloom halo goes with the "
              "upscaled frame; the \"crisp hud\" lines report it). Draws the layer takes get no "
              "UI depth and no "
              "reactive mask.");
}

int uiLayerTargetKind() {
    const uint32_t gen = bindingGeneration(BindSlot::Rtv0);
    if (gen == g_tc.gen) return g_tc.kind;
    g_tc = TargetCache{};
    g_tc.gen = gen;
    void* rtv = bindingGet(BindSlot::Rtv0);
    ResourceInfo info;
    if (!rtv || !bindingResolve(rtv, &info) || !info.isTexture2D ||
        !layerEyeSized(info.a, info.b)) {
        return 0;
    }
    D3D11_RENDER_TARGET_VIEW_DESC d{};
    if (!guarded("uiLayer.rtvDesc",
                 [&] { static_cast<ID3D11RenderTargetView*>(rtv)->GetDesc(&d); })) {
        return 0;
    }
    g_tc.info = info;
    g_tc.view = d.Format;
    g_tc.kind = (d.ViewDimension == D3D11_RTV_DIMENSION_TEXTURE2D && d.Texture2D.MipSlice == 0 &&
                 uiLayerLdrView(d.Format))
                    ? 2
                    : 1;
    return g_tc.kind;
}

// --------------------------------- the VR world route's re-issue (ui_layer.h)
//
// On a frame the route owns the world, the 2D screen's composite is decided here in the route's mode
// (ui_layer_math.h uiLayerWorldRouteMode): the decision's tests run as for any opaque, no-depth eye draw, and the
// draw that passes them is NOT taken -- uiLayerDecide returns false, the game's own draw lands in its eye image as
// it always did (the game's post pass copies that image on, and a refused re-issue leaves the eye whole for the
// eye route), and vscreen.cpp then issues it once more into the eye's layer (worldScreenReissue ->
// uiLayerWorldReissueBegin / End) with the route's mipped copy of the resolved screen at PS slot 0 and a
// trilinear sampler like the game's. Everything the re-issue changes goes back at End; any refusal leaves the
// game's state untouched, is counted by reason and named (the first eight distinct reasons, once each), and the
// eye route serves that eye.

namespace {

// The decided draw, held from its decision until the re-issue right after the game's own issue. One at a time,
// render thread only; a stale one (its draw was swallowed) is overwritten by the next decision or dropped by
// vscreen's scope and never fires.
struct WorldReissue {
    bool pending = false;  // decided: the game's draw runs as it always did, then is issued once more into the layer
    bool active = false;   // Begin bound the layer and the mipped screen; End puts everything back
    bool changed = false;  // Begin took PS slot 0 off the game's draw (the saved pair below is what to put back)
    int eye = -1;
    uint64_t seq = 0;
    const void* targetRes = nullptr;  // the game's eye image the draw writes (identity only)
    uint32_t targetW = 0, targetH = 0;
    float jx = 0.0f, jy = 0.0f;       // this frame's jitter, in the target's pixels (0 while the eye shift is off)
    UiBlendShape shape = UiBlendShape::kOpaque;
    void* screenRes = nullptr;                    // the draw's PS slot 0 texture at the decision (identity only)
    ID3D11ShaderResourceView* mipsSrv = nullptr;  // vrWorldMipsScreen's, valid for this frame (no reference held)
    ID3D11SamplerState* sampler = nullptr;        // vrWorldMipsSampler's, cached by the mips module (none held)
    // What Begin took off the game's PS slot 0 (references held), put back at End.
    ID3D11ShaderResourceView* savedSrv = nullptr;
    ID3D11SamplerState* savedSampler = nullptr;
};
WorldReissue g_reissue;
FaultBudget g_worldBudget("uiLayer.worldReissue", 4);
UiWorldReasonLog g_worldReasons;
bool g_worldReissueNoted[2] = {};

void worldReleaseSaved(WorldReissue& r) {
    if (r.savedSrv) r.savedSrv->Release();
    if (r.savedSampler) r.savedSampler->Release();
    r.savedSrv = nullptr;
    r.savedSampler = nullptr;
}

// Forget a pending or active re-issue: the frame boundary, shutdown.
void worldReissueReset() {
    worldReleaseSaved(g_reissue);
    g_reissue = WorldReissue{};
    detail::g_uiLayerWorldReissue = false;
}

// One refusal: counted by reason (the decision's own refusals are counted in decided[kScreen][...] as for any
// family) and, the first kUiWorldReasonLines distinct ones, named once each.
void worldRefuse(int eye, uint16_t id) {
    if (id < static_cast<uint16_t>(UiWorldRefuse::kCount)) ++g_win.worldRefused[id];
    if (!g_worldReasons.first(id)) return;
    char line[320];
    uiWorldFormatRefusal(line, sizeof(line), eye, id);
    Log::get().note("%s", line);
}

// PS slot 0 of the decided draw -- the screen texture and the game's sampler -> the route's mipped copy of the
// screen and the trilinear sampler like the game's, or why not. The mips module copies and mips the screen once a
// frame (a second eye's call is a cache hit).
UiWorldRefuse worldReissueSources(ID3D11DeviceContext* ctx, uint64_t seq, WorldReissue* plan) {
    Ptr<ID3D11ShaderResourceView> srv;
    ctx->PSGetShaderResources(0, 1, &srv);
    Ptr<ID3D11Resource> res;
    if (srv) srv->GetResource(&res);
    Ptr<ID3D11Texture2D> tex;
    if (res) res.As(&tex);
    if (!tex) return UiWorldRefuse::kNoSource;
    Ptr<ID3D11SamplerState> smp;
    ctx->PSGetSamplers(0, 1, &smp);
    if (!smp) return UiWorldRefuse::kNoSampler;
    ID3D11ShaderResourceView* mips = vrWorldMipsScreen(ctx, tex.Get(), seq);
    if (!mips) return UiWorldRefuse::kMipsNull;
    Ptr<ID3D11Device> dev;
    ctx->GetDevice(&dev);
    D3D11_SAMPLER_DESC sd{};
    smp->GetDesc(&sd);
    ID3D11SamplerState* mip = dev ? vrWorldMipsSampler(dev.Get(), sd) : nullptr;
    if (!mip) return UiWorldRefuse::kSamplerNull;
    plan->screenRes = res.Get();  // identity only: the game holds the texture
    plan->mipsSrv = mips;
    plan->sampler = mip;
    return UiWorldRefuse::kNone;
}

// The route's screen draw passed every test of the decision: check what the re-issue needs from the draw's own
// bindings and, when it has them, hold the draw for the re-issue that follows the game's own issue. Never a take.
// A curved screen (fix.panel_curvature: the game's draw is substituted with the strip, vscreen.cpp) is planned like a flat one:
// the re-issue repeats whatever the game's draw became (a quad, or the strip through panel_curve.h panelCurveReissue), and the
// plan's checks are about bindings the two share (the depth state, the blend, the texture and the sampler at PS slot 0).
void worldReissuePlan(ID3D11DeviceContext* ctx, const UiLayerDrawFacts& f, uint64_t seq,
                      float jx, float jy) {
    UiWorldRefuse why = UiWorldRefuse::kNone;
    WorldReissue plan;
    if (f.ds.tests() || f.ds.writes()) {
        why = UiWorldRefuse::kDepthState;
    } else if (f.blend != UiBlendShape::kOpaque) {
        why = UiWorldRefuse::kNotOpaque;
    } else {
        // The mips' copy and every call below go through the hooked vtable: vscreen's hooks step aside for them.
        // The scope is OUTSIDE the guard -- an SEH fault does not unwind a destructor inside it.
        VrWorldInternalScope internal;
        const bool ran = guardedBudget(g_worldBudget, [&] { why = worldReissueSources(ctx, seq, &plan); });
        if (!ran) why = UiWorldRefuse::kFault;
    }
    if (why != UiWorldRefuse::kNone) {
        worldRefuse(f.eye, uiWorldReasonId(why));
        return;
    }
    plan.eye = f.eye;
    plan.seq = seq;
    plan.targetRes = g_tc.info.resource;
    plan.targetW = g_tc.info.a;
    plan.targetH = g_tc.info.b;
    plan.jx = jx;
    plan.jy = jy;
    plan.shape = f.blend;
    plan.pending = true;
    g_reissue = plan;
    detail::g_uiLayerWorldReissue = true;
}

// The game's PS slot 0, put back (raw: the binding shadow never saw the change).
void worldRestoreSources(ID3D11DeviceContext* ctx, const WorldReissue& plan) {
    ID3D11ShaderResourceView* srv = plan.savedSrv;
    vScreenPSSetShaderResourcesRaw(ctx, 0, 1, &srv);
    ID3D11SamplerState* smp = plan.savedSampler;
    ctx->PSSetSamplers(0, 1, &smp);
}

}  // namespace

bool uiLayerDecide(ID3D11DeviceContext* ctx, int familyInt, bool verdictForwards,
                   bool substituted, int knownEye) {
    g_lastDecision = 0;
    g_draw.decided = false;
    g_draw.counted = false;
    g_draw.hdr = false;
    g_draw.reissue = false;
    g_draw.holoPsHash = 0; g_draw.holoOriginal = nullptr; g_draw.holoPatched = nullptr;
    g_draw.holoRestoreOk = true;
    g_draw.ds = UiDsEffect{};
    g_draw.stencilRead = 0;
    g_draw.shape = UiBlendShape::kRefused;
    if (familyInt != static_cast<int>(UiLayerFamily::kAfterUi)) {
        // A decision of a real family forgets a re-issue still pending from an earlier draw. The after-UI retry's
        // decisions run between a draw's own decision and its re-issue, and leave it alone.
        g_reissue.pending = false;
        detail::g_uiLayerWorldReissue = false;
    }
    if (!ctx || familyInt <= 0 || familyInt >= static_cast<int>(UiLayerFamily::kCount)) return false;
    const UiLayerFamily family = static_cast<UiLayerFamily>(familyInt);
    // The on-foot maps gate's count of the 2D screen composites this decision SEES (ui_maps_math.h UiMapsWindow::screenDraws): every
    // one while the gate is on, whatever the decision comes to (taken, re-issued, left in the game's frame, refused). Counted here and
    // nowhere else, so the reader can tell a window in which nothing was drawn (a cockpit, a load) from one whose composites were not
    // taken. With the key off it is one load of a bool that is false.
    if (family == UiLayerFamily::kScreen && detail::g_uiLayerMapsOn) ++g_maps.win.screenDraws;
    UiLayerDrawFacts f;
    f.family = family;
    f.verdictForwards = verdictForwards;
    f.substituted = substituted;
    // The world-screen gate, as this frame's boundary read it.
    f.worldScreen = g_screenHeld == 1;
    // The VR world route owns this frame's world: the 2D screen composite is re-issued into the layer, not taken
    // (asked for that family alone).
    f.worldRoute = family == UiLayerFamily::kScreen && vrWorldRouteLayerMayTake();
    if (family == UiLayerFamily::kScreen) ++g_win.screenAsked;
    const int kind = uiLayerTargetKind();
    f.eyeTarget = kind != 0;
    f.ldrView = kind == 2;
    // The HDR HUD take (Phases 1-3: the cockpit HUD families -- the holo
    // panels, the flight HUD, the target sprite, and the crisp take's eight
    // hologram families as kHoloGeneric -- and, since 2026-10-07, the two supercruise
    // line draws): a draw of one into the lit HDR pre-tonemap
    // eye target goes to the eye's HDR HUD layer instead of the kHdrTarget
    // refusal. The take arms with the layer (fix.ui_quality), and every
    // other refusal applies to it exactly as to the LDR take. The list is
    // ui_layer_math.h's uiLayerFamilyTakesHdr, one place for this and the rigs' routing model.
    f.crispHdr = detail::g_uiLayerCrispOn && f.eyeTarget && !f.ldrView && uiLayerFamilyTakesHdr(family);
    uint64_t seq = 0;
    float jx = 0.0f, jy = 0.0f;
    uint32_t sw = 0, sh = 0;
    // The scene lines' density fact, for the detail text of its refusal.
    uint32_t sceneLayerW = 0, sceneLayerH = 0;
    if (f.eyeTarget && (f.ldrView || f.crispHdr)) {
        // A caller that already knows the eye (uiLayerNoteOther, for an
        // after-UI write into the exact resource this frame's UI left)
        // passes it directly: uiDepthEyeOfTarget's own table lookup would
        // otherwise re-derive the answer redundantly, with no structural
        // guarantee it agrees with which eye's layer actually holds the UI
        // this resource was taken from.
        f.eye = knownEye >= 0 ? knownEye
                              : uiDepthEyeOfTarget(g_tc.info.resource, g_tc.info.a, g_tc.info.b, g_tc.info.fmt);
        if (f.eye >= 0 &&
            layerDrawJitter(static_cast<uint32_t>(f.eye), &seq, &jx, &jy, &sw, &sh)) {
            // The map sends the whole target onto the layer: only right when
            // the target IS the region the game submits for that eye.
            f.targetMatchesEye = !sw || !sh || (sw == g_tc.info.a && sh == g_tc.info.b);
            f.late = uiLayerLateFor(g_eye[f.eye].door, seq);
            f.armed = uiLayerArmed(g_eye[f.eye].door, seq);
            // The orbit lines and the supercruise bars are scene geometry the layer takes for DENSITY: the layer the door will
            // hand on (its size, from the door's frame and the target) against the render this draw is made at. Not asked of
            // any other family, which keeps its default and so its decision exactly as it was.
            if (uiLayerFamilyIsSceneLines(family)) {
                const UiLayerSize ls = uiLayerSize(g_eye[f.eye].door.fullW, g_eye[f.eye].door.fullH, layerTarget());
                sceneLayerW = ls.w;
                sceneLayerH = ls.h;
                f.layerWider = uiLayerWiderThanRender(ls.w, ls.h, g_tc.info.a, g_tc.info.b);
            }
            // The crisp take's publication deadline (review crisp-hud-phase3-
            // 2026-09-28, the missing ship/target mesh holograms): the game's
            // tonemap ordering varies frame to frame, and content taken after
            // the eye's re-issue ran can never publish -- the layer clears next
            // frame and the counters count the draw, not its pixels. Refuse it
            // to stock instead; the 30 s line names these frames.
            f.lateTone = f.crispHdr && g_eye[f.eye].hdrToneSeq == seq;
        }
    }
    // The cheap facts first; the state reads only when none of them refused.
    f.blend = UiBlendShape::kOpaque;
    UiLayerDecision d = uiLayerDecide(f);
    char detail[320] = "";
    if (d == UiLayerDecision::kNoDensityGain) {
        _snprintf_s(detail, _TRUNCATE, "the layer would be %ux%u and the render is %ux%u: a line drawn there is no denser",
                    sceneLayerW, sceneLayerH, g_tc.info.a, g_tc.info.b);
    }
    UiDsState dsState;
    if (d == UiLayerDecision::kRedirect) {
        // A second render target, or pixel-shader UAVs (which the layer's
        // raw OMSetRenderTargets would not carry across), refuse the draw.
        ID3D11RenderTargetView* rtvs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
        ID3D11DepthStencilView* dsv = nullptr;
        ID3D11UnorderedAccessView* uavs[D3D11_PS_CS_UAV_REGISTER_COUNT] = {};
        ctx->OMGetRenderTargetsAndUnorderedAccessViews(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtvs,
                                                       &dsv, 0, D3D11_PS_CS_UAV_REGISTER_COUNT, uavs);
        bool anyUav = false;
        for (auto* u : uavs) {
            if (u) {
                anyUav = true;
                u->Release();
            }
        }
        f.mrt = boundCount(rtvs) > 1 || anyUav;
        UINT ref = 0;
        DXGI_FORMAT dsFmt = DXGI_FORMAT_UNKNOWN;
        dsState = dsStateOfNoted(ctx, dsv, &ref, &dsFmt);
        f.ds = uiLayerDsEffect(dsState, dsv != nullptr);
        char dsWhy[160] = "";
        if (f.ds.tests()) {
            f.dsReproducible = dsReproducible(ctx, dsv, g_tc.info.a, g_tc.info.b, dsWhy, sizeof(dsWhy));
        }
        ID3D11BlendState* bs = nullptr;
        FLOAT factor[4];
        UINT mask = 0;
        ctx->OMGetBlendState(&bs, factor, &mask);
        UiBlendRt game;
        f.blend = shapeOf(bs, &game);
        if (bs) bs->Release();
        // A multiply is drawn twice (the layer, then its transmittance): a
        // depth or stencil WRITE would then land twice in the layer's copy.
        const bool multiplyWrites = f.blend == UiBlendShape::kMultiply && f.ds.writes();
        if (multiplyWrites) f.blend = UiBlendShape::kRefused;
        // Review R5: the HDR half has no transmittance route, so the pure
        // gate refuses a crisp multiply; the detail line names it below.
        const bool hdrMultiply = f.crispHdr && f.blend == UiBlendShape::kMultiply;
        d = uiLayerDecide(f);
        // The supercruise bars' private pass (a geometry shader, its constants and its rasterizer states) is asked BEFORE the
        // layer is made: a draw the family cannot serve must not be the reason an HDR layer is. Declined, the lines stay in the
        // scene and the module's line says which part was missing.
        const char* familyWhy = "";
        if (d == UiLayerDecision::kRedirect && family == UiLayerFamily::kSupercruiseBars) {
            f.familyReady = supercruiseBarsReady(ctx, &familyWhy);
            d = uiLayerDecide(f);
        }
        if (d == UiLayerDecision::kRedirect) {
            // The crisp take establishes its WHOLE dependency set here, no
            // later than the first take (review R2): a failure refuses the
            // take as kLayerFailed -- the draw stays stock -- and
            // crispTakeReady has already stood the path down with the
            // reason. Never take-then-drop.
            f.layerReady =
                f.crispHdr
                    ? crispTakeReady(ctx, f.eye)
                    : ensureLayerFor(ctx, f.eye) &&
                          (f.blend != UiBlendShape::kMultiply || ensureMult(ctx, g_eye[f.eye], f.eye));
            d = uiLayerDecide(f);
        }
        // The SV_Position remap's draws (ui_holo_remap.h needsRemap): the hologram sphere VS into the HDR HUD
        // layer, and the frosted base wherever it is taken. A program that cannot be prepared is refused to
        // stock: drawn unremapped into the layer, its lookup would run over the layer's pixels.
        if (d == UiLayerDecision::kRedirect &&
            ui_holo_remap::needsRemap(bindingShaderHash(BindSlot::Vs), bindingShaderHash(BindSlot::Ps),
                                      f.crispHdr)) {
            const uint64_t ps = bindingShaderHash(BindSlot::Ps);
            ++g_holoEligible;
            ID3D11PixelShader* original = static_cast<ID3D11PixelShader*>(bindingGet(BindSlot::Ps));
            ID3D11PixelShader* prepared = nullptr;
            const bool ran = guarded("ui.holo.prepare", [&] { prepared = g_holoCache.prepare(ctx, original, ps); });
            if (!ran || !prepared) {
                ++g_holoRefused; d = UiLayerDecision::kLayerFailed;
                const int slot = ui_holo_remap::index(ps);
                if (slot >= 0) ++g_holoRefusedBy[slot];
                _snprintf_s(detail, _TRUNCATE, "%s PS bytecode/device preparation unavailable; stock complete draw retained",
                            slot >= 0 ? ui_holo_remap::kindName(ui_holo_remap::kindOf(slot)) : "hologram");
                if (slot >= 0 && !g_holoRefusalNoted[slot]) {
                    Log::get().note("crisp holo remap: prepare refused PS %016llX before admission (verified bytecode, original identity, device or allocation unavailable); stock complete draw retained.",
                        static_cast<unsigned long long>(ps));
                    g_holoRefusalNoted[slot] = true;
                }
            } else {
                ++g_holoPrepared; g_draw.holoPsHash = ps;
                g_draw.holoOriginal = original; g_draw.holoPatched = prepared;
                const int slot = ui_holo_remap::index(ps);
                if (!g_holoPrepareNoted[slot]) {
                    Log::get().note("crisp holo remap: prepared PS %016llX from verified original DXBC before admission; private b13 float4, no runtime HLSL.",
                        static_cast<unsigned long long>(ps));
                    g_holoPrepareNoted[slot] = true;
                }
            }
        }
        // What decided it, in the census's own numbers, for the first line.
        char bl[64], ds[256];
        describeBlend(game, bl, sizeof(bl));
        describeDs(dsState, ref, dsFmt, ds, sizeof(ds));
        if (d == UiLayerDecision::kBlendRefused) {
            _snprintf_s(detail, _TRUNCATE, "%s%s", bl,
                        multiplyWrites ? " (a multiply that writes depth or stencil)"
                        : hdrMultiply  ? " (a multiply has no transmittance route into the HDR layer)"
                                       : "");
        } else if (d == UiLayerDecision::kDepthStencilTest ||
                   d == UiLayerDecision::kSubstitutedWrite) {
            _snprintf_s(detail, _TRUNCATE, "%s%s%s", ds, *dsWhy ? "; " : "", dsWhy);
        } else if (d == UiLayerDecision::kFamilyNotReady) {
            _snprintf_s(detail, _TRUNCATE, "%s", familyWhy);
        } else if (d == UiLayerDecision::kRedirect) {
            _snprintf_s(detail, _TRUNCATE, "%s%s%s%s", uiBlendShapeName(f.blend),
                        f.ds.tests() ? "; tests against the layer's seeded copy of the game's "
                                       "depth-stencil"
                                     : "",
                        f.ds.writes() ? "; its depth/stencil write kept in the game's buffer by a "
                                        "colourless re-issue"
                                      : "",
                        (f.ds.tests() || f.ds.writes()) ? (std::string(" (") + ds + ")").c_str()
                                                        : "");
        }
        for (auto* r : rtvs)
            if (r) r->Release();
        if (dsv) dsv->Release();
    }
    g_lastDecision = static_cast<int>(d);
    if (uiLayerWorldRouteMode(family, f.worldScreen, f.worldRoute)) {
        // The route's mode: never a take. A draw that passed every test is held for the re-issue after the game's
        // own issue (counted when it lands, as a re-issue -- not as "redirected": the layer took nothing from the
        // game's frame); one the tests refused is counted and named as any refusal, and the eye route serves it.
        if (uiLayerWorldCount(d, true) == UiWorldCount::kReissue) {
            worldReissuePlan(ctx, f, seq, jx, jy);
        } else {
            ++g_win.decided[static_cast<size_t>(family)][static_cast<size_t>(d)];
            noteFamily(family, d, detail);
            worldRefuse(f.eye, uiWorldReasonId(d));
        }
        return false;
    }
    ++g_win.decided[static_cast<size_t>(family)][static_cast<size_t>(d)];
    noteFamily(family, d, detail);
    if (d != UiLayerDecision::kRedirect) return false;
    g_draw.decided = true;
    g_draw.eye = f.eye;
    g_draw.family = family;
    g_draw.hdr = f.crispHdr;
    g_draw.seq = seq;
    g_draw.targetRes = g_tc.info.resource;
    g_draw.targetW = g_tc.info.a;
    g_draw.targetH = g_tc.info.b;
    g_draw.shape = f.blend;
    g_draw.ds = f.ds;
    g_draw.stencilRead = dsState.readMask;
    g_draw.rawDepthWritePotential = dsState.depthEnable && dsState.depthWriteAll;
    // The pass computed the jitter against the eye's submitted region, which
    // is the target (targetMatchesEye above).
    g_draw.jx = jx;
    g_draw.jy = jy;
    return true;
}

void uiLayerNoteFamilyProbe(uint64_t vs, uint64_t ps, int family, int why) {
    (void)family;
    const int slot = probeSlot(vs);
    if (slot < 0 || why < 0 || why >= static_cast<int>(UiFamilyWhy::kCount)) return;
    ++g_win.probe[slot][why];
    if (g_sizeChange.open) ++g_sizeChange.probe[slot][why];
    if (why != static_cast<int>(UiFamilyWhy::kNoSurface) || !ps) return;
    for (uint64_t& known : g_unknownPs[slot]) {
        if (known == ps) return;
        if (!known) {
            known = ps;
            return;
        }
    }
}

void uiLayerNoteCompositeTaken() { g_win.scene.noteTaken(); }

void uiLayerNoteCompositeLeft(uint64_t vs, uint64_t ps, int family) { g_win.scene.noteLeft(vs, ps, family); }

bool uiLayerBegin(ID3D11DeviceContext* ctx) { return beginGuarded(ctx, 0); }

void uiLayerEnd(ID3D11DeviceContext* ctx) {
    if (!g_draw.active) return;
    g_draw.drawRoute.end([&](int slot) { routeEnd(ctx, slot); });
    if (!guarded("uiLayer.end", [&] { restore(ctx); })) {
        standDown("a fault while putting the game's state back after a draw");
    }
    releaseSaved();
    g_draw.active = false;
    detail::g_uiLayerRedirecting = false;
}

bool uiLayerMultiplyBegin(ID3D11DeviceContext* ctx) {
    if (!g_draw.decided || g_draw.shape != UiBlendShape::kMultiply) return false;
    return beginGuarded(ctx, 1);
}

bool uiLayerWriteBackBegin(ID3D11DeviceContext* ctx) {
    if (!g_draw.holoRestoreOk) return false;
    if (!g_draw.decided || !g_draw.counted || !g_draw.ds.writes() || g_draw.active || !ctx)
        return false;
    bool ok = false;
    guarded("uiLayer.writeBack", [&] {
        ctx->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, g_draw.wbRtv, &g_draw.wbDsv);
        if (!g_draw.wbDsv) return;
        g_privateDepthGuard.noteReplay(g_draw.seq, g_lastRedirectSeq, g_draw.rawDepthWritePotential);
        // The game's own depth target and state, no colour target: the draw's
        // depth or stencil write lands in the game's buffer exactly as it
        // always did, and its colour lands nowhere (it is in the layer).
        vScreenSetRenderTargetsRaw(ctx, 0, nullptr, g_draw.wbDsv);
        ok = true;
    });
    if (!ok) {
        for (auto*& r : g_draw.wbRtv) {
            if (r) r->Release();
            r = nullptr;
        }
        if (g_draw.wbDsv) g_draw.wbDsv->Release();
        g_draw.wbDsv = nullptr;
        return false;
    }
    g_draw.wbActive = true;
    ++g_win.writeBacks;
    // The colourless re-issue is the layer's own work: timed to its End.
    g_draw.wbRouteSlot = routeBegin(ctx, UiRouteStage::kWriteBack, g_draw.eye, g_draw.seq);
    return true;
}

void uiLayerWriteBackEnd(ID3D11DeviceContext* ctx) {
    if (!g_draw.wbActive) return;
    routeEnd(ctx, g_draw.wbRouteSlot);
    g_draw.wbRouteSlot = -1;
    guarded("uiLayer.writeBackEnd", [&] {
        vScreenSetRenderTargetsRaw(ctx, boundCount(g_draw.wbRtv), g_draw.wbRtv, g_draw.wbDsv);
    });
    for (auto*& r : g_draw.wbRtv) {
        if (r) r->Release();
        r = nullptr;
    }
    if (g_draw.wbDsv) g_draw.wbDsv->Release();
    g_draw.wbDsv = nullptr;
    g_draw.wbActive = false;
}

// ------------------------------------- the crisp-HUD half of fix.ui_quality: the tonemap re-issue
//
// Phases 1-3 of docs/cockpit-hud-layer-design-2026-09-27.md. The cockpit's
// HDR HUD families (the holo panels, the flight HUD, the target sprite, the
// holograms) are
// taken into the eye's HDR HUD layer by the draw path above
// (g_draw.hdr); they reach the eye HERE, at the game's own tonemap draw --
// the last reader of the lit HDR target, recognised by tonemap_admit.h's
// structural admission shared with the HUD layer census. Right AFTER the
// game's own issue, the admitted draw is issued once more, into the eye's
// 8-bit layer, with the HDR source slot (the admitted ps's table entry)
// bound to the HDR layer's SRV: the layer's premultiplied radiance is
// tonemapped by the game's own shader, with the game's own exposure (VS t0,
// when the variant binds one -- the EDHM swap does not), LUT (PS t0),
// constants (b2), samplers, input layout and VB0, all still bound from the
// game's draw and never touched. Then one EDVR pass (the coverage pass)
// writes the HDR layer's alpha -- the HUD's transmittance, the layer's own
// convention -- into the 8-bit layer's alpha. The 8-bit layer then holds the
// tonemapped HUD exactly as any other taken UI, the post-tonemap menus land
// on top in game order, and the door's composite shows both unchanged.
//
// Only what must differ is bound: the render target (the 8-bit layer, no
// depth view), the blend (disabled, RGB write mask -- the alpha comes from
// the coverage pass), the viewport and scissor (the full layer rect; the two
// layers share a size by construction, so the source sample is 1:1), and the
// HDR source slot. Everything changed goes through the vScreen*Raw entry
// points and is put back at End, so the binding shadow keeps describing the
// game's state, and the game's own draw is always untouched.
//
// Guards (each counted, each named once a session, never fatal): the
// ordering guards -- the 8-bit layer must be empty this frame (menus draw
// after the tonemap) and an eye is re-tonemapped once a frame (a second
// admitted draw is counted and left) -- the admitted ps must name its HDR
// slot and have a 2D source bound there, the two layers must share a size,
// the bindings at the re-issue must be the admitted draw's own, no PS UAV
// may be bound, and both layers must exist. A failure of EDVR's own
// machinery (the coverage shaders, the deferred context) stands the crisp-HUD half of fix.ui_quality
// down alone; the LDR take is unaffected, and the cockpit HUD families go
// back to stock (their kHdrTarget refusal).

// The admitted draw, from its admission in vscreen's eye-draw branch until
// the re-issue right after its own issue. One at a time, render thread only;
// a stale one (its draw was swallowed) is overwritten by the next admission
// and never fires.
struct CrispTonePending {
    bool pending = false;
    int eye = -1;
    uint64_t seq = 0;
    int hdrSlot = -1;
    const void* rtvRes = nullptr;  // the admitted draw's output (the RGBA8 eye)
    const void* hdrRes = nullptr;  // the admitted draw's own HDR source (identity)
    uint64_t vs = 0, ps = 0;
};
CrispTonePending g_crispPending;

// What Begin saved and End puts back (ComPtrs like the census's GdSave: the
// guarded blocks only ever Attach into them).
struct CrispToneSave {
    Ptr<ID3D11RenderTargetView> rtvs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT];
    Ptr<ID3D11DepthStencilView> dsv;
    Ptr<ID3D11BlendState> blend;
    FLOAT factor[4] = {};
    UINT sampleMask = 0xFFFFFFFFu;
    D3D11_VIEWPORT vp[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] = {};
    UINT vpCount = 0;
    D3D11_RECT sc[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] = {};
    UINT scCount = 0;
    bool scissorSet = false;
    Ptr<ID3D11ShaderResourceView> gameHdrSrv;  // the game's own HDR source at the slot
    int routeSlot = -1;
    int eye = -1;
    uint64_t seq = 0;
    int hdrSlot = -1;
    const void* rtvRes = nullptr;
    uint64_t vs = 0, ps = 0;
    bool active = false;
};
CrispToneSave g_crispSave;
FaultBudget g_crispToneBudget("uiLayer.crispTone", 4);

// The coverage pass's own machinery: two shaders (ui_layer_shaders.h) and a
// deferred context, both warmed at the frame boundary (the sharpen's reason:
// no first-use D3DCompile mid-frame).
ID3D11VertexShader* g_covVs = nullptr;
ID3D11PixelShader* g_covPs = nullptr;
bool g_covTried = false;
Ptr<ID3D11DeviceContext> g_crispDeferred;
bool g_crispDeferredTried = false;

void compileCoverageOnce(ID3D11DeviceContext* ctx) {
    if ((g_covVs && g_covPs) || g_covTried || !ctx) return;
    g_covTried = true;
    g_covVs = shaderSwapCreateVs(ctx,kUiLayerCoverageVsBytecode,sizeof(kUiLayerCoverageVsBytecode),"ui_layer_coverage_vs","crisp hud");
    g_covPs = shaderSwapCreatePs(ctx,kUiLayerCoveragePsBytecode,sizeof(kUiLayerCoveragePsBytecode),"ui_layer_coverage_ps","crisp hud");
    if (!g_covVs || !g_covPs) {
        if (g_covVs) g_covVs->Release();
        if (g_covPs) g_covPs->Release();
        g_covVs = nullptr;
        g_covPs = nullptr;
    }
}

// The coverage pass's deferred context, created once a session (the one
// attempt is g_crispDeferredTried). Warmed through crispTakeReady at the
// first HUD take; the pass itself keeps its own late-failure handling.
bool crispCoverageContextReady(ID3D11DeviceContext* ctx) {
    if (g_crispDeferred) return true;
    if (g_crispDeferredTried || !ctx) return false;
    g_crispDeferredTried = true;
    Ptr<ID3D11Device> dev;
    ctx->GetDevice(&dev);
    return dev && SUCCEEDED(dev->CreateDeferredContext(0, &g_crispDeferred)) && g_crispDeferred;
}

// The whole dependency set a taken HUD draw needs at the tonemap,
// established no later than the FIRST HUD take of it (review R2): the eye's
// HDR layer, its 8-bit layer, the RGB-only blend the re-issue draws with,
// and the coverage pass's shaders and deferred context. The HDR take used to
// ensure only the FP16 layer before redirecting; the rest were first ensured
// at the tonemap, after the HUD draws had left stock, and a failure there
// dropped the already-taken HUD every frame. A failure here REFUSES the take
// (the draw runs stock, the decide's kLayerFailed names it on the 30 s line)
// and stands the path down with the reason: never take-then-drop.
bool crispTakeReady(ID3D11DeviceContext* ctx, int eye) {
    if (!ensureHdrLayerFor(ctx, eye)) {
        crispStandDown("the HDR HUD layer could not be created");
        return false;
    }
    if (!ensureLayerFor(ctx, eye)) {
        crispStandDown("the eye's 8-bit layer (the tonemap re-issue's target) could not be created");
        return false;
    }
    UiBlendRt rgbOnly;  // the re-issue's blend: disabled, the colour channels alone written
    rgbOnly.enable = false;
    rgbOnly.mask = uiblend::kWriteRgb;
    if (!cachedBlend(ctx, rgbOnly)) {
        crispStandDown("the tonemap re-issue's blend state could not be created");
        return false;
    }
    compileCoverageOnce(ctx);
    if (!g_covVs || !g_covPs) {
        crispStandDown("the coverage pass's shaders could not be compiled");
        return false;
    }
    if (!crispCoverageContextReady(ctx)) {
        crispStandDown("no deferred context for the coverage pass");
        return false;
    }
    return true;
}

// First-seen lines, deduplicated per (reason, vs, ps) -- and, with reason
// kCount, the first re-issue per variant.
struct CrispToneSeen {
    uint8_t reason;
    uint64_t vs, ps;
};
constexpr uint32_t kMaxCrispToneLines = 24;
CrispToneSeen g_crispToneSeen[kMaxCrispToneLines];
uint32_t g_crispToneSeenCount = 0;

bool crispToneSeenBefore(uint8_t reason, uint64_t vs, uint64_t ps) {
    for (uint32_t i = 0; i < g_crispToneSeenCount; ++i) {
        const CrispToneSeen& s = g_crispToneSeen[i];
        if (s.reason == reason && s.vs == vs && s.ps == ps) return true;
    }
    if (g_crispToneSeenCount >= kMaxCrispToneLines) return true;  // table full: quiet
    g_crispToneSeen[g_crispToneSeenCount++] = {reason, vs, ps};
    return false;
}

void crispToneDecline(CrispToneDecline d, uint64_t vs, uint64_t ps) {
    ++g_win.hdrDeclined[static_cast<size_t>(d)];
    if (crispToneSeenBefore(static_cast<uint8_t>(d), vs, ps)) return;
    Log::get().note("crisp hud: tonemap re-issue declined (vs %016llX ps %016llX): %s. The game's "
                    "own draw ran untouched.",
                    static_cast<unsigned long long>(vs), static_cast<unsigned long long>(ps),
                    crispToneDeclineName(d));
}

const char* crispToneVariantName(uint64_t vs, uint64_t ps) {
    return (vs == kToneVsMeasured && ps == kTonePsMeasured) ? "the measured pair"
           : (vs == kToneVsEdhm && ps == kTonePsMeasured)   ? "the EDHM swap"
                                                            : "a named-by-structure variant";
}

// The coverage pass: the HDR layer's alpha into the 8-bit layer's alpha
// (replace, colour masked off), a full-screen triangle on EDVR's own deferred
// context -- the depth-stencil seed's pattern: recorded off the hooked
// immediate context, executed with the context state restored.
bool crispCoveragePass(ID3D11DeviceContext* ctx, Eye& e, int eye, uint64_t seq) {
    if (!g_covVs || !g_covPs) compileCoverageOnce(ctx);
    if (!g_covVs || !g_covPs) return false;
    if (!g_crispDeferred) {
        // crispTakeReady has run at the first HUD take since R2, so a
        // failure here was almost always said there already; say it once
        // when it is genuinely first seen.
        const bool triedBefore = g_crispDeferredTried;
        if (!crispCoverageContextReady(ctx)) {
            if (triedBefore) return false;
            Log::get().note("crisp hud: no deferred context for the coverage pass; the crisp-HUD half of fix.ui_quality "
                            "stands down (the cockpit HUD is drawn as it always was).");
            crispStandDown("no deferred context for the coverage pass");
            return false;
        }
    }
    UiBlendRt alphaOnly;  // blending off, the alpha channel alone written
    alphaOnly.enable = false;
    alphaOnly.mask = uiblend::kWriteAlpha;
    ID3D11BlendState* blend = cachedBlend(ctx, alphaOnly);
    if (!blend) return false;
    const int timer = routeBegin(ctx, UiRouteStage::kHdrCoverage, eye, seq);
    bool ok = false;
    try {
        const UiCoverageBindings bindings{e.hdrSrv.Get(), e.rtv.Get(), g_covVs, g_covPs,
                                          blend, e.w, e.h};
        ok = e.coverage.execute(ctx, g_crispDeferred.Get(), bindings, vScreenExecuteCommandListRaw);
    } catch (const std::exception&) {
        e.coverage.reset();
        ok = false;
    }
    if (ok) routeEnd(ctx, timer);
    else routeAbort(ctx, timer);
    return ok;
}

// vscreen's eye-draw branch, owner context, while the crisp-HUD half of fix.ui_quality is live: is
// this draw the game's tonemap, admitted for the re-issue? The 3-vertex
// prefilter is register-cheap; the structural admission's state reads run
// for the handful of full-screen triangles a frame that pass it. True =
// admitted: the caller skips its after-UI read check for this draw (it READS
// the HDR target the HUD families were taken from -- that is the point of the
// re-issue, not a post pass to name) and brackets its issue with
// uiLayerCrispToneBegin/End.
namespace {
// An admitted tonemap, pending its re-issue right after the game's own issue: VR's structural admission and the flat
// profile's known-pair one (uiLayerCrispAdmitFlat) both arm it here.
void crispArm(int eye, uint64_t seq, int hdrSlot, const void* rtvRes, const void* hdrRes, uint64_t vs, uint64_t ps) {
    g_crispPending.pending = true;
    g_crispPending.eye = eye;
    g_crispPending.seq = seq;
    g_crispPending.hdrSlot = hdrSlot;
    g_crispPending.rtvRes = rtvRes;
    g_crispPending.hdrRes = hdrRes;
    g_crispPending.vs = vs;
    g_crispPending.ps = ps;
    detail::g_uiLayerCrispPending = true;
}
}  // namespace

bool uiLayerCrispNoteEyeDraw(ID3D11DeviceContext* ctx, char kind, uint32_t count, uint32_t instances,
                             uint32_t startInstance) {
    g_crispPending = CrispTonePending{};
    detail::g_uiLayerCrispPending = false;
    if (!detail::g_uiLayerCrispOn || !ctx) return false;
    if ((kind != 'D' && kind != 'N') || count != 3 || instances != 1 || startInstance != 0)
        return false;
    const uint64_t vs = bindingShaderHash(BindSlot::Vs);
    const uint64_t ps = bindingShaderHash(BindSlot::Ps);
    ToneAdmit ta;
    bool shaped = false;
    guarded("uiLayer.crispAdmit", [&] { shaped = tonemapAdmitStructure(ctx, ps, ta); });
    if (!shaped) return false;
    // The HDR source, by IDENTITY, not by table: the PS slot whose 2D view
    // reads an eye's HDR target this frame (the HUD take recorded which).
    // A table of PS hashes cannot cover the settings tiers -- the 2026-09-28
    // flight at HMD Quality 0.50 flew ps D0A16B9E55BF22CC, the table said
    // kNoHdrSlot, and the taken panels never came back. The identity read
    // covers every variant. It also never touches ui_depth's eye table --
    // registering the tonemap's output there crowded the main menu's
    // composite out of it, the 2026-09-27 regression.
    int eye = -1, hdrSlot = -1;
    const void* hdrRes = nullptr;
    uint64_t seq = 0;
    float jx = 0.0f, jy = 0.0f;
    uint32_t jw = 0, jh = 0;
    // The frame's sequence (the per-eye jitter is not read here).
    if (!layerDrawJitter(0, &seq, &jx, &jy, &jw, &jh)) return false;
    bool content = false;
    for (UINT s = 0; s < 4 && eye < 0; ++s) {
        Ptr<ID3D11ShaderResourceView> srv;
        ctx->PSGetShaderResources(s, 1, &srv);
        if (!srv) continue;
        D3D11_SHADER_RESOURCE_VIEW_DESC vd{};
        srv->GetDesc(&vd);
        if (vd.ViewDimension != D3D11_SRV_DIMENSION_TEXTURE2D) continue;  // the LUT is 3D, never this
        Ptr<ID3D11Resource> r;
        srv->GetResource(&r);
        if (!r) continue;
        for (int cand = 0; cand < 2; ++cand) {
            const Eye& c = g_eye[cand];
            const bool fresh = c.hdrSeq == seq && c.hdrDraws && c.hdrSrv;
            // Outstanding = fresh and NOT yet published: already-consumed
            // content (hdrToneSeq == hdrSeq) must not arm the failed-consumer
            // fallback below (review R1's second half).
            if (fresh && c.hdrToneSeq != c.hdrSeq) content = true;
            if (eye >= 0 || !fresh || r.Get() != c.hdrTarget) continue;
            eye = cand;
            hdrSlot = static_cast<int>(s);
            hdrRes = r.Get();
        }
    }
    if (eye < 0) {
        // Outstanding HUD content this frame, but this draw reads no eye's
        // HUD source. Only the eye's OWN tonemap failing to read it is a
        // failed consumer (review R1): the draw must BE the tonemap -- the
        // full SRV shape, or the EDHM swap's known exposure-less one
        // (tonemapAdmitFull) -- and its output must be the eye whose content
        // is outstanding (the census measures hundreds-thousands of
        // tonemap-SHAPED draws a window that are not it: SMAA and the post
        // passes after it, skipped silently here, as before this branch
        // existed). A consumer that never comes at all is the frame
        // deadline's (beginInner's HDR clear). The eye read is the eye
        // table's READ-ONLY lookup: the registering form crowded the main
        // menu's composite out of it (the 2026-09-27 regression).
        if (content && tonemapAdmitFull(ta, vs)) {
            int outEye = -1;
            if (ta.rtvRes) {
                ResourceInfo ri;
                if (bindingResolveResource(ta.rtvRes, &ri) && ri.isTexture2D)
                    outEye = uiDepthEyeOfTargetReadOnly(ri.resource);
            }
            bool failedConsumer = false;
            if (outEye >= 0 && outEye < 2) {
                const Eye& c = g_eye[outEye];
                failedConsumer =
                    c.hdrSeq == seq && c.hdrDraws && c.hdrSrv && c.hdrToneSeq != c.hdrSeq;
            }
            if (failedConsumer) {
                // Named once, counted -- and the crisp path stands down to
                // stock rather than lose the HUD for the session.
                crispToneDecline(CrispToneDecline::kNoHdrSlot, vs, ps);
                crispStandDown("no PS slot of the tonemap reads an eye's HUD source");
            }
        }
        return false;
    }
    Eye& e = g_eye[eye];
    // The ordering guards.
    if (e.seq == seq && e.draws) {
        crispToneDecline(CrispToneDecline::kLayerBusy, vs, ps);
        return false;
    }
    if (e.hdrToneSeq == seq) {
        crispToneDecline(CrispToneDecline::kSecondTonemap, vs, ps);
        return false;
    }
    crispArm(eye, seq, hdrSlot, ta.rtvRes, hdrRes, vs, ps);
    return true;
}

int uiLayerCrispAdmitFlat(ID3D11DeviceContext* ctx, int hdrSlot, const void* alias, uint64_t vs, uint64_t ps, char* why,
                          size_t whyN) {
    g_crispPending = CrispTonePending{};
    detail::g_uiLayerCrispPending = false;
    const auto say = [&](const char* text) {
        if (why && whyN) _snprintf_s(why, whyN, _TRUNCATE, "%s", text);
        return 0;
    };
    if (!runtimeFlatProfile() || !ctx) return say("not the flat profile");
    if (!detail::g_uiLayerCrispOn) return say("the HDR HUD path is not live");
    if (hdrSlot < 0 || hdrSlot > 3) return say("the tone pair names no HDR slot");
    uint64_t seq = 0;
    if (!layerDrawJitter(0, &seq, nullptr, nullptr, nullptr, nullptr)) return say("no flat frame");
    Eye& e = g_eye[0];
    if (!(e.hdrSeq == seq && e.hdrDraws && e.hdrSrv)) return say("no HUD was taken this frame");
    Ptr<ID3D11ShaderResourceView> srv;
    ctx->PSGetShaderResources(static_cast<UINT>(hdrSlot), 1, &srv);
    Ptr<ID3D11Resource> r;
    if (srv) srv->GetResource(&r);
    if (!r) return say("nothing is bound at the tone's HDR slot");
    if (r.Get() != e.hdrTarget && (!alias || r.Get() != alias))
        return say("the tone reads neither the HUD's HDR target nor its copy");
    Ptr<ID3D11RenderTargetView> rtv;
    ctx->OMGetRenderTargets(1, &rtv, nullptr);
    Ptr<ID3D11Resource> out;
    if (rtv) rtv->GetResource(&out);
    if (!out) return say("the tone has no render target");
    if (e.seq == seq && e.draws) return say("the 8-bit layer is already busy this frame");
    if (e.hdrToneSeq == seq) return say("a second tonemap this frame");
    crispArm(0, seq, hdrSlot, out.Get(), r.Get(), vs, ps);  // the pair the draw scope read before the resolve ran
    return say(r.Get() == e.hdrTarget ? "admitted: reads the HUD's HDR target" : "admitted: reads the HUD target's copy"), 1;
}

// After the admitted draw's own issue (vscreen's forwardWithVerdict, around
// its pureDrawReissue): re-issue it once, tonemapping the HDR layer into the
// 8-bit layer's colour. False -- state untouched, the draw long since run
// stock -- on any decline.
bool uiLayerCrispToneBegin(ID3D11DeviceContext* ctx) {
    detail::g_uiLayerCrispPending = false;
    if (!g_crispPending.pending || !ctx) return false;
    const CrispTonePending p = g_crispPending;
    g_crispPending = CrispTonePending{};
    Eye& e = g_eye[p.eye];
    bool ok = false;
    const bool ran = guardedBudget(g_crispToneBudget, [&] {
        // The bindings must still be the admitted draw's own: nothing ran
        // between its admission and here but its verdict's Begin (kNone for
        // the tonemap) and the draw itself, which changes no state.
        bool drift = false;
        if (runtimeFlatProfile()) {
            // The flat profile reads the context itself: on the HDR route its resolve runs inside the tone draw's own
            // scope (treatHdr, under FlatComputeInternalScope), and the binding shadow no longer names the draw's
            // bindings afterwards -- the 12:31 flight's "bindings ... not the admitted draw's" with vs/ps 0 every time.
            Ptr<ID3D11RenderTargetView> rtvNow;
            ctx->OMGetRenderTargets(1, &rtvNow, nullptr);
            Ptr<ID3D11Resource> outNow;
            if (rtvNow) rtvNow->GetResource(&outNow);
            Ptr<ID3D11ShaderResourceView> srvNow;
            ctx->PSGetShaderResources(static_cast<UINT>(p.hdrSlot), 1, &srvNow);
            Ptr<ID3D11Resource> hdrNow;
            if (srvNow) srvNow->GetResource(&hdrNow);
            drift = !outNow || outNow.Get() != p.rtvRes || !hdrNow || hdrNow.Get() != p.hdrRes;
        } else {
            void* rtv = bindingGet(BindSlot::Rtv0);
            ResourceInfo ri;
            drift = !rtv || !bindingResolve(rtv, &ri) || ri.resource != p.rtvRes;
            if (!drift) {
                void* srv = bindingGet(static_cast<BindSlot>(static_cast<uint32_t>(BindSlot::PsSrv0) +
                                                             static_cast<uint32_t>(p.hdrSlot)));
                ResourceInfo si;
                drift = !srv || !bindingResolve(srv, &si) || si.resource != p.hdrRes;
            }
        }
        if (drift) {
            crispToneDecline(CrispToneDecline::kStateDrift, p.vs, p.ps);
            return;
        }
        // The guards again, at the moment of issue (a frame boundary cannot
        // have intervened; a nested EDVR pass could have).
        if (e.hdrSeq != p.seq || !e.hdrDraws || !e.hdrSrv || !e.hdrRtv) {
            crispToneDecline(CrispToneDecline::kNoContent, p.vs, p.ps);
            return;
        }
        if (e.hdrToneSeq == p.seq) {
            crispToneDecline(CrispToneDecline::kSecondTonemap, p.vs, p.ps);
            return;
        }
        if (e.seq == p.seq && e.draws) {
            crispToneDecline(CrispToneDecline::kLayerBusy, p.vs, p.ps);
            return;
        }
        // The 8-bit layer, at the HDR layer's size by construction.
        if (!ensureLayerFor(ctx, p.eye) || !e.rtv) {
            crispToneDecline(CrispToneDecline::kLayerFailed, p.vs, p.ps);
            return;
        }
        if (e.w != e.hdrW || e.h != e.hdrH) {
            crispToneDecline(CrispToneDecline::kSizeMismatch, p.vs, p.ps);
            return;
        }
        // The blend for the re-issue, before anything is bound: disabled,
        // the colour channels alone written (alpha is the coverage pass's).
        UiBlendRt rgbOnly;
        rgbOnly.enable = false;
        rgbOnly.mask = uiblend::kWriteRgb;
        ID3D11BlendState* rgbBlend = cachedBlend(ctx, rgbOnly);
        if (!rgbBlend) {
            crispToneDecline(CrispToneDecline::kLayerFailed, p.vs, p.ps);
            return;
        }
        // Save. A PS UAV bound at the re-issue would be written twice (the
        // game's draw already wrote it): decline instead.
        ID3D11RenderTargetView* rawRtvs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
        ID3D11DepthStencilView* rawDsv = nullptr;
        ID3D11UnorderedAccessView* uavs[D3D11_PS_CS_UAV_REGISTER_COUNT] = {};
        ctx->OMGetRenderTargetsAndUnorderedAccessViews(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT,
                                                       rawRtvs, &rawDsv, 0,
                                                       D3D11_PS_CS_UAV_REGISTER_COUNT, uavs);
        bool anyUav = false;
        for (auto* u : uavs) {
            if (u) {
                anyUav = true;
                u->Release();
            }
        }
        for (uint32_t i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i)
            g_crispSave.rtvs[i].Attach(rawRtvs[i]);
        g_crispSave.dsv.Attach(rawDsv);
        if (anyUav) {
            crispToneDecline(CrispToneDecline::kUav, p.vs, p.ps);
            for (auto& r : g_crispSave.rtvs) r.Reset();
            g_crispSave.dsv.Reset();
            return;
        }
        ctx->OMGetBlendState(&g_crispSave.blend, g_crispSave.factor, &g_crispSave.sampleMask);
        g_crispSave.vpCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
        ctx->RSGetViewports(&g_crispSave.vpCount, g_crispSave.vp);
        g_crispSave.scissorSet = false;
        g_crispSave.scCount = 0;
        {
            Ptr<ID3D11RasterizerState> rs;
            ctx->RSGetState(&rs);
            D3D11_RASTERIZER_DESC rd{};
            if (rs) rs->GetDesc(&rd);
            if (rs && rd.ScissorEnable) {
                g_crispSave.scCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
                ctx->RSGetScissorRects(&g_crispSave.scCount, g_crispSave.sc);
                g_crispSave.scissorSet = g_crispSave.scCount > 0;
            }
        }
        Ptr<ID3D11ShaderResourceView> gameHdr;
        ctx->PSGetShaderResources(static_cast<UINT>(p.hdrSlot), 1, &gameHdr);
        g_crispSave.gameHdrSrv = gameHdr;
        // Bind: the 8-bit layer, no depth view; the disabled RGB blend; the
        // full-layer viewport (and scissor rect, if the game's rasterizer
        // tests one); the HDR layer at the admitted draw's HDR slot.
        ID3D11RenderTargetView* target = e.rtv.Get();
        vScreenSetRenderTargetsRaw(ctx, 1, &target, nullptr);
        vScreenOMSetBlendStateRaw(ctx, rgbBlend, g_crispSave.factor, g_crispSave.sampleMask);
        D3D11_VIEWPORT vp{0.0f, 0.0f, static_cast<float>(e.w), static_cast<float>(e.h), 0.0f,
                          1.0f};
        vScreenRSSetViewportsRaw(ctx, 1, &vp);
        if (g_crispSave.scissorSet) {
            D3D11_RECT r{0, 0, static_cast<LONG>(e.w), static_cast<LONG>(e.h)};
            ctx->RSSetScissorRects(1, &r);
        }
        ID3D11ShaderResourceView* hdrSrv = e.hdrSrv.Get();
        vScreenPSSetShaderResourcesRaw(ctx, static_cast<uint32_t>(p.hdrSlot), 1, &hdrSrv);
        g_crispSave.eye = p.eye;
        g_crispSave.seq = p.seq;
        g_crispSave.hdrSlot = p.hdrSlot;
        g_crispSave.rtvRes = p.rtvRes;
        g_crispSave.vs = p.vs;
        g_crispSave.ps = p.ps;
        g_crispSave.routeSlot = routeBegin(ctx, UiRouteStage::kHdrTonemap, p.eye, p.seq);
        g_crispSave.active = true;
        ok = true;
    });
    if (!ran) {
        // A fault mid-bind: put back whatever was saved, and stand the HDR
        // path down -- the game's own draw already ran, so the frame is whole.
        if (g_crispSave.active || g_crispSave.rtvs[0]) {
            guarded("uiLayer.crispToneRestore", [&] {
                ID3D11RenderTargetView* rtvs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
                for (uint32_t i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i)
                    rtvs[i] = g_crispSave.rtvs[i].Get();
                UINT n = 0;
                for (uint32_t i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i)
                    if (rtvs[i]) n = i + 1;
                vScreenSetRenderTargetsRaw(ctx, n, rtvs, g_crispSave.dsv.Get());
                vScreenOMSetBlendStateRaw(ctx, g_crispSave.blend.Get(), g_crispSave.factor,
                                          g_crispSave.sampleMask);
                vScreenRSSetViewportsRaw(ctx, g_crispSave.vpCount, g_crispSave.vp);
                if (g_crispSave.scissorSet) ctx->RSSetScissorRects(g_crispSave.scCount, g_crispSave.sc);
                if (g_crispSave.hdrSlot >= 0) {
                    ID3D11ShaderResourceView* gameHdr = g_crispSave.gameHdrSrv.Get();
                    vScreenPSSetShaderResourcesRaw(ctx, static_cast<uint32_t>(g_crispSave.hdrSlot),
                                                   1, &gameHdr);
                }
            });
        }
        g_crispSave = CrispToneSave{};
        crispStandDown("a fault while binding the tonemap re-issue");
        return false;
    }
    return ok;
}

void uiLayerCrispToneEnd(ID3D11DeviceContext* ctx) {
    if (!g_crispSave.active) return;
    const int eye = g_crispSave.eye;
    const uint64_t seq = g_crispSave.seq;
    const int hdrSlot = g_crispSave.hdrSlot;
    const void* rtvRes = g_crispSave.rtvRes;
    const uint64_t vs = g_crispSave.vs, ps = g_crispSave.ps;
    routeEnd(ctx, g_crispSave.routeSlot);
    g_crispSave.routeSlot = -1;
    // Everything the game had, put back through the raw entry points.
    if (!guarded("uiLayer.crispToneEnd", [&] {
            ID3D11RenderTargetView* rtvs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
            UINT n = 0;
            for (uint32_t i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i) {
                rtvs[i] = g_crispSave.rtvs[i].Get();
                if (rtvs[i]) n = i + 1;
            }
            vScreenSetRenderTargetsRaw(ctx, n, rtvs, g_crispSave.dsv.Get());
            vScreenOMSetBlendStateRaw(ctx, g_crispSave.blend.Get(), g_crispSave.factor,
                                      g_crispSave.sampleMask);
            vScreenRSSetViewportsRaw(ctx, g_crispSave.vpCount, g_crispSave.vp);
            if (g_crispSave.scissorSet) ctx->RSSetScissorRects(g_crispSave.scCount, g_crispSave.sc);
            ID3D11ShaderResourceView* gameHdr = g_crispSave.gameHdrSrv.Get();
            vScreenPSSetShaderResourcesRaw(ctx, static_cast<uint32_t>(hdrSlot), 1, &gameHdr);
        })) {
        crispStandDown("a fault while putting the game's state back after the tonemap re-issue");
    }
    g_crispSave = CrispToneSave{};
    // The coverage pass: without it the layer's alpha is stale, so a failure
    // clears the layer (the HUD misses the frame; the picture stays whole).
    Eye& e = g_eye[eye];
    bool covered = false;
    const bool ranCover = guarded("uiLayer.crispCoverage", [&] {
        covered = crispCoveragePass(ctx, e, eye, seq);
    });
    if (ranCover && covered) {
        ++g_win.hdrCoveragePasses;
    } else {
        vScreenClearRenderTargetViewRaw(ctx, e.rtv.Get(), kUiLayerClear);
        if (!g_crispStoodDown) {
            Log::get().note("crisp hud: the coverage pass failed; the HUD misses this frame, and "
                            "the crisp-HUD half of fix.ui_quality stands down (the cockpit HUD is drawn as it always "
                            "was).");
            crispStandDown("the coverage pass failed");
        }
        return;
    }
    // The 8-bit layer now holds this frame's tonemapped HUD: the door's
    // composite shows it, and the post-tonemap menus land on top in game
    // order (their draws see e.seq already current, so no clear).
    //
    // Two things this does not order, both about draws the layer did not take:
    //  - after the UI: the identity below is the tonemap's OUTPUT, the FIRST
    //    link of the eye's chain. Elite's post pass carries the eye on into
    //    another target and every interface draw lands there, so the identity
    //    follows it (uiLayerNoteOther's read case, uiLayerFollowReader): until
    //    it has, and if it never does, the after-the-UI rule sees nothing
    //    (flights 055723, 060935: the frosted base under the station panels
    //    stayed in the frame, under the HUD this layer now holds).
    //  - inside the HDR phase (KNOWN LIMIT, not fixed): a draw the layer does
    //    not take, issued AFTER a taken HUD draw in the HDR target (the
    //    frosted label quads vs 2CECEC30..., the canopy, other families), was
    //    over that HUD draw in stock and is under it now -- the HUD layer is
    //    composited over the whole frame. There is no HDR-phase counterpart of
    //    the after-the-UI take; tools/ui_quality_test pins the inversion.
    e.seq = seq;
    e.draws = 1;
    e.target = rtvRes;  // the RGBA8 eye the game's tonemap wrote: the FIRST link of the identity
    e.chainOpen = true;
    e.hdrToneSeq = seq;
    e.hdrMissStreak = 0;  // a publication landed: the R1 deadline's streak resets
    g_lastRedirectSeq = seq;
    detail::g_uiLayerWatching = true;
    ++g_win.hdrReissued;
    if (!crispToneSeenBefore(static_cast<uint8_t>(CrispToneDecline::kCount), vs, ps)) {
        Log::get().note("crisp hud: tonemap re-issue engaged (%s, vs %016llX ps %016llX) -- the %s "
                        "eye's HDR HUD layer (%ux%u) is tonemapped into its 8-bit layer at ps t%d, "
                        "once an eye a frame, with the game's own exposure and LUT.",
                        crispToneVariantName(vs, ps), static_cast<unsigned long long>(vs),
                        static_cast<unsigned long long>(ps), eye == 0 ? "left" : "right", e.hdrW,
                        e.hdrH, hdrSlot);
    }
}

// ------------------------------- the VR world route's re-issue: Begin, End, the door's preflight (ui_layer.h)

UiLayerWorldStats uiLayerWorldStats() {
    UiLayerWorldStats s;
    s.screenAsked = g_win.screenAsked;
    const size_t screen = static_cast<size_t>(UiLayerFamily::kScreen);
    static_assert(static_cast<size_t>(UiLayerDecision::kCount) <= 24, "UiLayerWorldStats::screenDecided is 24 wide");
    static_assert(static_cast<size_t>(UiWorldRefuse::kCount) <= 16, "UiLayerWorldStats::refused is 16 wide");
    for (size_t d = 0; d < static_cast<size_t>(UiLayerDecision::kCount); ++d) s.screenDecided[d] = g_win.decided[screen][d];
    s.reissued = g_win.worldReissued;
    s.lostDraws = g_win.worldLeftDraws;
    for (size_t r = 0; r < static_cast<size_t>(UiWorldRefuse::kCount); ++r) s.refused[r] = g_win.worldRefused[r];
    return s;
}

bool uiLayerWorldScreenHeld() { return g_screenHeld == 1; }

bool uiLayerLiveForWorldRoute() { return detail::g_uiLayerLive; }

const char* uiLayerNotLiveReason() {
    return uiLayerNotLiveReasonFor(g_target, g_temporal, g_jitterAsShipped, g_stoodDown);
}

void uiLayerSetMapsGateForTest(bool on) { g_maps.keyCfg = on ? UiMapsKey::On : UiMapsKey::Off; }

void uiLayerWorldReissueAbandon() {
    g_reissue.pending = false;
    detail::g_uiLayerWorldReissue = false;
}

// Right after the game's own issue of the decided screen draw (vscreen.cpp, worldScreenReissue): the layer
// bound for one more issue of it. The game's draw is never touched; false leaves the game's state exactly as it
// was and the eye to the eye route.
bool uiLayerWorldReissueBegin(ID3D11DeviceContext* ctx) {
    detail::g_uiLayerWorldReissue = false;
    if (!g_reissue.pending || !ctx) return false;
    WorldReissue plan = g_reissue;
    g_reissue = WorldReissue{};
    if (plan.eye < 0 || plan.eye > 1) return false;
    UiWorldRefuse why = UiWorldRefuse::kNone;
    {
        // Every call below goes through the hooked vtable or a raw entry: vscreen's hooks step aside. The scope is
        // OUTSIDE every guard -- an SEH fault does not unwind a destructor inside one.
        VrWorldInternalScope internal;
        if (!detail::g_uiLayerLive || uiLayerIssueBlocked()) {
            why = UiWorldRefuse::kBeginRefused;
        } else if (uiLayerTargetKind() == 0 || g_tc.info.resource != plan.targetRes) {
            // Nothing ran between the decision and here but the game's own draw and its verdict's state.
            why = UiWorldRefuse::kStateChanged;
        } else {
            bool same = false;
            const bool ran = guardedBudget(g_worldBudget, [&] {
                Ptr<ID3D11ShaderResourceView> srv;
                ctx->PSGetShaderResources(0, 1, &srv);
                Ptr<ID3D11Resource> res;
                if (srv) srv->GetResource(&res);
                same = res.Get() == plan.screenRes;
            });
            if (!ran) why = UiWorldRefuse::kFault;
            else if (!same) why = UiWorldRefuse::kStateChanged;
        }
        if (why == UiWorldRefuse::kNone) {
            // The decided draw, as beginInner reads it: the eye's layer, the map and the jitter cancel, the opaque
            // conversion of the game's own blend, no depth target. beginGuarded saves the game's targets,
            // viewports, scissor and blend and puts them back itself on a refusal or a fault.
            g_draw.decided = true;
            g_draw.counted = false;
            g_draw.hdr = false;
            g_draw.reissue = true;  // the route's: the game's own draw lands in its eye image, this one is not a take
            g_draw.holoPsHash = 0;
            g_draw.holoOriginal = nullptr;
            g_draw.holoPatched = nullptr;
            g_draw.holoRestoreOk = true;
            g_draw.ds = UiDsEffect{};
            g_draw.stencilRead = 0;
            g_draw.rawDepthWritePotential = false;
            g_draw.eye = plan.eye;
            g_draw.family = UiLayerFamily::kScreen;
            g_draw.seq = plan.seq;
            g_draw.targetRes = plan.targetRes;
            g_draw.targetW = plan.targetW;
            g_draw.targetH = plan.targetH;
            g_draw.shape = plan.shape;
            g_draw.jx = plan.jx;
            g_draw.jy = plan.jy;
            if (!beginGuarded(ctx, 0)) {
                g_draw.decided = false;
                why = UiWorldRefuse::kBeginRefused;
            } else {
                // Then the two bindings that make it the route's draw: the mipped screen and the trilinear
                // sampler, both at PS slot 0. The game's own pair is held and put back at End.
                const bool bound = guardedBudget(g_worldBudget, [&] {
                    ctx->PSGetShaderResources(0, 1, &plan.savedSrv);
                    ctx->PSGetSamplers(0, 1, &plan.savedSampler);
                    plan.changed = true;
                    ID3D11ShaderResourceView* mips = plan.mipsSrv;
                    vScreenPSSetShaderResourcesRaw(ctx, 0, 1, &mips);
                    ID3D11SamplerState* sampler = plan.sampler;
                    ctx->PSSetSamplers(0, 1, &sampler);
                });
                if (!bound) {
                    // Put back what was changed, the layer's own state included, and leave the eye alone.
                    if (plan.changed) guarded("uiLayer.worldUnbind", [&] { worldRestoreSources(ctx, plan); });
                    uiLayerEnd(ctx);
                    g_draw.decided = false;
                    why = UiWorldRefuse::kFault;
                } else {
                    plan.active = true;
                }
            }
        }
    }
    if (why != UiWorldRefuse::kNone) {
        worldReleaseSaved(plan);
        worldRefuse(plan.eye, uiWorldReasonId(why));
        return false;
    }
    plan.pending = false;  // decided -> active: a second Begin without a new decision finds nothing to do
    g_reissue = plan;
    return true;
}

// The game's draw has been issued once more: every binding Begin changed goes back, and the route is told which
// eye the layer took. Safe without a Begin.
void uiLayerWorldReissueEnd(ID3D11DeviceContext* ctx, bool landed) {
    if (!g_reissue.active || !ctx) return;
    WorldReissue plan = g_reissue;
    g_reissue = WorldReissue{};
    const bool wasDown = g_stoodDown;
    bool putBack = true;
    {
        VrWorldInternalScope internal;
        putBack = guarded("uiLayer.worldEnd", [&] {
            if (plan.changed) worldRestoreSources(ctx, plan);
        });
        uiLayerEnd(ctx);  // the layer's own: the game's targets, viewports, scissor and blend back, references released
    }
    worldReleaseSaved(plan);
    g_draw.decided = false;
    g_draw.counted = false;
    g_draw.reissue = false;
    if (!putBack) standDown("a fault while putting the game's texture and sampler back after the world route's re-issue");
    if (!putBack || (g_stoodDown && !wasDown)) {
        // The state could not be trusted back: the eye is not the layer's (the eye route serves it, or the layer
        // stood down and the route lets go at the boundary).
        worldRefuse(plan.eye, uiWorldReasonId(UiWorldRefuse::kFault));
        return;
    }
    if (!landed) {
        // The draw between Begin and End did not happen (a fault in the curved screen's strip): nothing is in the layer for this
        // eye, so it is not the route's.
        worldRefuse(plan.eye, uiWorldReasonId(UiWorldRefuse::kFault));
        return;
    }
    ++g_win.worldReissued;
    g_eye[plan.eye].worldSeq = plan.seq;
    vrWorldRouteNoteEyeTaken(static_cast<uint32_t>(plan.eye), plan.seq);
    if (!g_worldReissueNoted[plan.eye]) {
        g_worldReissueNoted[plan.eye] = true;
        Log::get().note(
            "vr world route: the layer re-issued the %s eye's 2D screen draw into its %ux%u layer from the mipped, "
            "resolved screen (sequence %llu) -- the game's own draw still lands in its eye image, and the door runs "
            "layer-only for this eye while the route owns the world.",
            plan.eye == 0 ? "left" : "right", g_eye[plan.eye].w, g_eye[plan.eye].h,
            static_cast<unsigned long long>(plan.seq));
    }
}

// The door's preflight (native_temporal.cpp treat(), before it commits an eye to layer-only): would the composite
// certainly run over `frame` for this eye? 0 when it would, else a UiWorldDoorGap.
int uiLayerWorldDoorGap(uint64_t sequence, uint32_t eye, ID3D11Texture2D* frame) {
    if (eye > 1 || !frame) return static_cast<int>(UiWorldDoorGap::kNoLayer);
    const Eye& e = g_eye[eye];
    UiWorldDoorFacts f;
    f.live = detail::g_uiLayerLive;
    f.layerMade = e.srv && e.rtv && e.w && e.h;
    f.holdsContent = e.seq == sequence && e.draws != 0;
    f.alreadyComposited = e.compositedSeq == sequence;
    f.runtimeDisabled = graphicsRuntimeDisabled();
    f.canComposite = f.live && doorCanComposite(frame);
    D3D11_TEXTURE2D_DESC d{};
    frame->GetDesc(&d);
    const float whole[4] = {0.0f, 0.0f, 1.0f, 1.0f};
    f.aspectMatches = uiLayerRegionMatches(d.Width, d.Height, whole, e.w, e.h);
    return static_cast<int>(uiWorldDoorGap(f));
}

// ---- the on-foot maps gate: what the door and the draw path ask (ui_layer.h; ui_maps_math.h)

void uiLayerMapsNoteRecognised() { ++g_maps.win.recognised; }

// The layer holds the eye's whole picture for this sequence. The route's re-issued world first (unchanged); then, with the key on
// and the naming deciding the gate, a 2D screen the layer TOOK while nothing else went into an eye-sized target. Asked by the
// temporal door and again by the sharpen door, so each eye and sequence is counted once.
bool uiLayerDoorLayerOnly(uint32_t eye, uint64_t sequence) {
    if (vrWorldRouteDoorLayerOnly(eye, sequence)) return true;
    if (!detail::g_uiLayerMapsOn || eye > 1) return false;
    Maps& m = g_maps;
    const uint32_t eyeDraws = vScreenEyeDrawsThisFrame();
    const UiMapsDoor door = uiMapsDoor(g_eye[eye].screenTakenSeq, sequence, eyeDraws, g_frameTakenDraws);
    if (door == UiMapsDoor::No) return false;
    if (door == UiMapsDoor::Yes) {
        if (m.layerOnlySeq[eye] != sequence) {
            m.layerOnlySeq[eye] = sequence;
            ++m.win.doorLayerOnly;
            ++m.panelLayerOnly;
        }
        return true;
    }
    if (m.notEmptySeq[eye] != sequence) {
        m.notEmptySeq[eye] = sequence;
        ++m.win.doorNotEmpty;
        ++m.panelNotEmpty;
        if (m.notEmptyLines < 3) {
            ++m.notEmptyLines;
            char line[480];
            uiMapsFormatNotEmpty(line, sizeof(line), eye, sequence, eyeDraws, g_frameTakenDraws);
            Log::get().note("%s", line);
        }
    }
    return false;
}

namespace {
// A game draw that writes the depth target a seed copied, after the seed: the layer's copy is stale, and the next
// tested draw seeds again. uiLayerNoteOther's first step, and the whole of uiLayerNoteSceneDraw (the flat profile).
void seedWritersCheck(ID3D11DeviceContext* ctx, uint32_t count, uint32_t instances, uint32_t verdict, char drawKind) {
    for (uint32_t eye = 0; eye < 2; ++eye) {
        Eye& e = g_eye[eye];
        for (LayerDs* l : {&e.ds, &e.hdrDs}) {
            const bool hdr = l == &e.hdrDs;
            const bool freshCandidate = l->seq == g_lastRedirectSeq && l->source;
            const bool diagnosticCandidate = g_seedCensus.enabled &&
                g_seedCensus.tracks[eye][hdr ? 1 : 0].known;
            if (!freshCandidate && !diagnosticCandidate) continue;
            void* dsvView = bindingGet(BindSlot::Dsv0);
            ResourceInfo info;
            if (!dsvView || !bindingResolve(dsvView, &info))
                continue;
            const bool matchingFresh = freshCandidate && info.resource == l->source;
            const bool matchingDiagnostic = g_seedCensus.matches(eye, hdr, info.resource);
            if (!matchingFresh && !matchingDiagnostic) continue;
            ID3D11DepthStencilView* dsv = static_cast<ID3D11DepthStencilView*>(dsvView);
            UINT ref = 0;
            DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
            const UiDsState ds = dsStateOfNoted(ctx, dsv,
                matchingDiagnostic ? &ref : nullptr, matchingDiagnostic ? &fmt : nullptr);
            const UiDsEffect writer = uiLayerDsEffect(ds, true);
            const bool invalidated = matchingFresh && uiLayerSeedWriterInvalidates(writer,
                l->seededMask, l->seededDepth, g_privateDepthGuard.active(g_lastRedirectSeq));
            if (matchingFresh && writer.writes() && !invalidated) ++g_win.depthOnlySeedPreservedWriters;
            if (matchingDiagnostic) {
                const UiSeedKey key = uiSeedDrawKey(ds, count, instances, verdict,
                    bindingShaderHash(BindSlot::Vs), bindingShaderHash(BindSlot::Ps), ref,
                    static_cast<uint32_t>(fmt), drawKind);
                g_seedCensus.event(eye, hdr, g_lastRedirectSeq, key, invalidated);
            }
            if (invalidated) {
                l->seq = 0;
                ++g_win.seedStale;
            }
        }
    }
}
}  // namespace

void uiLayerNoteSceneDraw(ID3D11DeviceContext* ctx, uint32_t count, uint32_t instances, char drawKind) {
    if (ctx) seedWritersCheck(ctx, count, instances, 0, drawKind);
}

bool uiLayerNoteOther(ID3D11DeviceContext* ctx, uint32_t count, bool verdictForwards, bool substituted,
                      bool excluded, bool panelSized, uint32_t instances, uint32_t verdict, char drawKind) {
    if (!ctx) return false;
    const void* taken[2] = {nullptr, nullptr};
    for (int e = 0; e < 2; ++e) {
        if (g_eye[e].seq == g_lastRedirectSeq && g_eye[e].draws) taken[e] = g_eye[e].target;
    }
    seedWritersCheck(ctx, count, instances, verdict, drawKind);
    if (!taken[0] && !taken[1]) return false;
    char kind = 0;
    int takenEye = -1;
    // A write: the draw's target is one the UI left this frame -- a compare
    // on the target cache, every draw. A read: a pass over the eye samples it
    // at t0/t1 -- full-screen passes are a handful of vertices, so only those
    // are resolved, and at most kWatchPerFrame a frame. The matching is
    // ui_layer_math.h's (uiLayerAfterWriteEye / uiLayerAfterReadEye), which
    // tools/ui_quality_test drives over the recorded station frames; keep the
    // ORDER of this function's steps (write, read, follow, then the write
    // path's exclusions and decide): the rig scans for it.
    const int writeEye = uiLayerTargetKind() != 0
                             ? uiLayerAfterWriteEye(true, g_tc.info.resource, taken[0], taken[1])
                             : -1;
    if (writeEye >= 0) {
        kind = 'W';
        takenEye = writeEye;
        ++g_win.afterWrites;
    } else if (count <= 6 && g_watchBudget) {
        --g_watchBudget;
        static const BindSlot kSlots[2] = {BindSlot::PsSrv0, BindSlot::PsSrv1};
        for (BindSlot slot : kSlots) {
            void* v = bindingGet(slot);
            ResourceInfo info;
            if (!v || !bindingResolve(v, &info)) continue;
            const int readEye = uiLayerAfterReadEye(info.resource, taken[0], taken[1]);
            if (readEye >= 0) {
                kind = 'R';
                takenEye = readEye;  // the eye whose identity this pass reads
                ++g_win.afterReads;
                break;
            }
        }
    }
    if (!kind) return false;
    const uint64_t vs = bindingShaderHash(BindSlot::Vs), ps = bindingShaderHash(BindSlot::Ps);
    // Exclusion 1 (today's behaviour, unchanged): a draw that READS the
    // target is never taken -- it only stops seeing the UI in what it reads.
    if (kind == 'R') {
        // The follow (2026-09-30): the game's post pass sits between the
        // tonemap and the interface, reading the tonemap's output A (the
        // identity the crisp re-issue set) and writing B, where every
        // interface draw lands. The identity goes with the eye, so those
        // draws are writes into it. BEFORE the once-per-pair note below:
        // that returns on every later frame, and a follow behind it would run
        // once a session. The pass itself is left in the frame either way.
        if (uiLayerFollowReader(g_eye[takenEye].chainOpen, taken[takenEye], taken[1 - takenEye],
                                uiLayerTargetKind() == 2, g_tc.info.resource)) {
            g_eye[takenEye].target = g_tc.info.resource;
            g_eye[takenEye].chainOpen = false;
            ++g_win.afterFollowed;
        }
        for (uint32_t i = 0; i < g_afterSeenCount; ++i) {
            if (g_afterSeen[i].kind == 'R' && g_afterSeen[i].vs == vs && g_afterSeen[i].ps == ps) return false;
        }
        if (g_afterSeenCount < kMaxAfterLines) {
            g_afterSeen[g_afterSeenCount++] = {'R', vs, ps};
            Log::get().note(
                "ui quality: layer: after the UI, the game read an eye target the UI was taken "
                "from: vs %016llX ps %016llX -- with the layer, that no longer sees the UI in what "
                "it reads (a post pass over the eye: the UI misses it).",
                static_cast<unsigned long long>(vs), static_cast<unsigned long long>(ps));
        }
        return false;
    }
    // kind == 'W'. Exclusion 2: a post pass that samples an eye-sized texture
    // at ANY bound PS SRV slot is left in the frame, never taken -- whole-frame
    // input, whatever the resource, is how a post pass differs from an
    // overlay (the field log's named draws sample 512x512 and 614x365, both
    // smaller than the eye). vScreenIsEyeSized is the same predicate
    // uiLayerTargetKind() already used to call this draw's OWN target an eye
    // target, so a match here means literally "as large as the thing being
    // written," not a fresh tolerance.
    bool eyeSizedInput = false;
    uint32_t srvW = 0, srvH = 0;
    static const BindSlot kPsSlots[4] = {BindSlot::PsSrv0, BindSlot::PsSrv1, BindSlot::PsSrv2,
                                         BindSlot::PsSrv3};
    for (BindSlot slot : kPsSlots) {
        void* v = bindingGet(slot);
        if (!v) continue;
        ResourceInfo info;
        if (bindingResolve(v, &info) && info.isTexture2D && vScreenIsEyeSized(info.a, info.b)) {
            eyeSizedInput = true;
            srvW = info.a;
            srvH = info.b;
            break;
        }
    }
    if (kind == 'W') {
        // rc-since-rc2 review F4: the retry preserves the original decision's
        // two exclusions before attempting the take -- the shader exclusion
        // and the held world-screen identity -- which the kAfterUi family
        // alone never saw.
        if (!uiLayerAfterWritePreserved(excluded, g_screenHeld == 1, panelSized)) {
            // Left in the game's frame: lost while the VR world route owns this eye (the layer is the whole eye).
            if (g_eye[takenEye].worldSeq == g_lastRedirectSeq) ++g_win.worldLeftDraws;
            return false;
        }
    }
    if (uiLayerAfterWriteDecide(eyeSizedInput) == UiAfterWriteDecision::kPostPass) {
        ++g_win.afterPostPass;
        if (g_eye[takenEye].worldSeq == g_lastRedirectSeq) ++g_win.worldLeftDraws;
        for (uint32_t i = 0; i < g_afterSeenCount; ++i) {
            if (g_afterSeen[i].kind == 'P' && g_afterSeen[i].vs == vs && g_afterSeen[i].ps == ps) return false;
        }
        if (g_afterSeenCount < kMaxAfterLines) {
            g_afterSeen[g_afterSeenCount++] = {'P', vs, ps};
            Log::get().note(
                "ui quality: layer: after the UI, the game drew into an eye target the UI was "
                "taken from: vs %016llX ps %016llX -- left as a post pass (it samples a %ux%u "
                "input the eye's own size): stays under the UI.",
                static_cast<unsigned long long>(vs), static_cast<unsigned long long>(ps), srvW, srvH);
        }
        return false;
    }
    // Not excluded: attempt the take through the exact machinery a real UI
    // draw uses. A decline here (kMrt, kBlendRefused, kVerdict, a
    // depth-stencil the layer cannot reproduce...) counts in afterDeclined,
    // and the families line says which rule; a decide-time success that
    // still fails at uiLayerBegin (a changed blend, a seed failure) counts
    // in afterRefused there. Either way the draw goes to the game's frame
    // exactly as it did before the layer took after-UI draws.
    const bool took = uiLayerDecide(ctx, static_cast<int>(UiLayerFamily::kAfterUi), verdictForwards,
                                    substituted, takenEye);
    if (!took) {
        ++g_win.afterDeclined;
        if (g_eye[takenEye].worldSeq == g_lastRedirectSeq) ++g_win.worldLeftDraws;  // lost under the route, as above
    }
    return took;
}

void uiLayerSeedDrawOutcome(bool forwarded, bool substituted, bool redirected, bool known) {
    if (g_seedCensus.enabled) g_seedCensus.finishDraw(forwarded, substituted, redirected, known);
}

void uiLayerNoteDepthClear(void* dsv, uint32_t flags, float depth, uint8_t stencil) {
    if (!dsv) return;
    bool any = false;
    for (const Eye& e : g_eye)
        any = any || (e.ds.seq && e.ds.source) || (e.hdrDs.seq && e.hdrDs.source);
    if (!any && !g_seedCensus.enabled) return;
    ResourceInfo info;
    if (!bindingResolve(dsv, &info) || !info.resource) return;
    for (uint32_t eye = 0; eye < 2; ++eye) {
        Eye& e = g_eye[eye];
        for (LayerDs* l : {&e.ds, &e.hdrDs}) {
            const bool hdr = l == &e.hdrDs;
            const bool invalidated = l->seq && l->source == info.resource;
            if (g_seedCensus.matches(eye, hdr, info.resource)) {
                D3D11_DEPTH_STENCIL_VIEW_DESC vd{};
                static_cast<ID3D11DepthStencilView*>(dsv)->GetDesc(&vd);
                DXGI_FORMAT tf, df, sf;
                const bool hasStencil = dsFormats(vd.Format, &tf, &df, &sf) && sf != DXGI_FORMAT_UNKNOWN;
                g_seedCensus.event(eye, hdr, g_lastRedirectSeq,
                    uiSeedClearKey(flags, depth, stencil, vd.Flags, uint32_t(vd.Format), hasStencil),
                    invalidated);
            }
            if (invalidated) {
                l->seq = 0;
                ++g_win.seedStale;
            }
        }
    }
}

void uiLayerNoteTemporal(uint64_t sequence, uint32_t eye, const void* output) {
    if (eye > 1) return;
    Eye& e = g_eye[eye];
    e.door.treatedSeq = sequence;
    e.temporalOut = output;
    e.temporalOutSeq = sequence;
    ++g_win.treated;
}

void uiLayerDoorSeen(uint64_t sequence, uint32_t eye, ID3D11Texture2D* source) {
    if (eye > 1 || !source) return;
    Eye& e = g_eye[eye];
    e.door.doorSeq = sequence;
    e.doorFromPass = e.temporalOutSeq == sequence && e.temporalOut == source;
    ++g_win.doors;
    if (e.doorFromPass && detail::g_uiLayerLive) {
        // Whether a composite can run over this frame at all is settled
        // here, before anything is redirected, not at the composite with a
        // frame's UI already in the layer: an unarmed layer loses nothing.
        if (!doorCanComposite(source)) {
            e.door.fullW = e.door.fullH = 0;
            return;
        }
        D3D11_TEXTURE2D_DESC d{};
        source->GetDesc(&d);
        if (d.Width != e.door.fullW || d.Height != e.door.fullH) {
            if (e.door.fullW && e.door.fullH) openSizeChange(e.door.fullW, e.door.fullH, d.Width, d.Height);
            if (detail::g_uiLayerLive) {
                const UiLayerSize s = uiLayerSize(d.Width, d.Height, layerTarget());
                Log::get().note(
                    "ui quality: layer: the %s eye's door hands on %ux%u; its layer is %ux%u at %s "
                    "(%.1f MB).",
                    eye == 0 ? "left" : "right", d.Width, d.Height, s.w, s.h,
                    uiQualityLabel(layerTarget()),
                    uiLayerMB(uiLayerBytes(s.w, s.h)));
            }
            e.door.fullW = d.Width;
            e.door.fullH = d.Height;
        }
    }
}

void uiLayerNoteCopy(const void* destination, const void* source) {
    if (!destination || !source) return;
    for (Eye& e : g_eye) {
        if (e.draws && e.seq == g_lastRedirectSeq && e.target == source) {
            e.copiedTo = destination;
            e.copiedSeq = e.seq;
        }
    }
}

void uiLayerNoteSubmitted(uint64_t sequence, uint32_t eye, const void* submitted) {
    if (eye > 1) return;
    g_eye[eye].submitted = submitted;
    g_eye[eye].submittedSeq = sequence;
}

ID3D11Texture2D* uiLayerComposite(uint64_t sequence, uint32_t eye, ID3D11Texture2D* frame,
                                  const uint32_t region[4], const float layerUv[4]) {
    if (eye > 1 || !frame || !region || !layerUv) return nullptr;
    g_seedCensus.door(eye, sequence);
    Eye& e = g_eye[eye];
    if (!e.srv || !e.rtv || e.compositedSeq == sequence) return nullptr;
    const bool hasUi = e.seq == sequence && e.draws;
    if (!hasUi) return nullptr;
    e.compositedSeq = sequence;
    {
        // The eye check: did the UI this layer holds leave the target the
        // game submitted (or copied into what it submitted) for THIS eye?
        const void* mine = e.submittedSeq == sequence ? e.submitted : nullptr;
        const void* theirs = g_eye[1 - eye].submitted;  // this frame's or last: stable textures
        const void* copied = e.copiedSeq == sequence ? e.copiedTo : nullptr;
        if (!mine || mine == theirs) {
            ++g_win.eyeUntold;
        } else if (e.target == mine || copied == mine) {
            ++g_win.eyeMatched;
        } else if (theirs && (e.target == theirs || copied == theirs)) {
            // The game's Submit says this UI belongs to the other eye: every
            // frame from here would invert the disparity of every menu. Stand
            // down at once -- the UI goes back into the game's frame, in the
            // right eyes.
            ++g_win.eyeSwapped;
            Log::get().note(
                "ui quality: layer: the %s eye's layer holds UI from the target the game "
                "submitted for the %s eye -- the eye rule (first target of a frame = left) is "
                "backwards on this rig.",
                eye == 0 ? "left" : "right", eye == 0 ? "right" : "left");
            standDown("the eye check found the layer's eyes swapped");
            return nullptr;
        } else {
            ++g_win.eyeUntold;
        }
    }
    if (graphicsRuntimeDisabled()) return nullptr;
    const bool useMult = e.mSrv && e.mSeq == sequence;
    ID3D11Texture2D* result = nullptr;
    const char* why = nullptr;
    const bool ran = guardedBudget(g_compositeBudget, [&] {
        result = compositeInner(e, eye, frame, region, layerUv, useMult, &why);
    });
    if (!result) {
        ++g_win.compositeRefused;
        // The UI of this frame is in the layer and will not reach the eye:
        // once, then the draws go back into the game's frame.
        standDown(!ran ? "a fault in the composite" : (why ? why : "the composite refused"));
    }
    return result;
}

namespace {
// The fence a failed hologram restore raises (detail::g_uiLayerIssueBlocked,
// set in restore()) holds for the rest of the frame it failed in and is
// settled HERE, at the next frame boundary, on the owner context: whichever
// of the hologram PS and b13 is still EDVR's is put back (only where the slot
// still holds EDVR's own object), the fence lifts and the game's draws
// resume. The layer itself stays stood down for the session. If eight
// boundaries in a row cannot do it the fence lifts anyway
// (ui_holo_remap.h settleAtBoundary): a frozen headset is worse than one
// draw on a wrong hologram pixel shader. The rule lives in the header so the
// ui_holo_test rig covers it; this only logs and flips the flag.
unsigned g_holoSettleFailed = 0;
void settleIssueFence(ID3D11DeviceContext* ctx) {
    if (!detail::g_uiLayerIssueBlocked) return;
    const unsigned failedBefore = g_holoSettleFailed;
    switch (ui_holo_remap::settleAtBoundary(g_holoBinding, ctx, vScreenPSSetShaderRaw, g_holoSettleFailed)) {
    case ui_holo_remap::Settle::kHold:
        return;
    case ui_holo_remap::Settle::kSettled:
        Log::get().note("ui quality: the hologram shader/b13 the layer could not put back was restored at the "
                        "frame boundary (after %u failed boundaries) -- game draws resume; the layer stays "
                        "stood down (turning fix.ui_quality off and on re-arms it).", failedBefore);
        break;
    case ui_holo_remap::Settle::kFailOpen:
        Log::get().note("ui quality: the hologram shader/b13 could NOT be put back in %u frame boundaries -- "
                        "the game's draws resume anyway, on possibly wrong hologram pixel shader / b13 state "
                        "until the game rebinds them; a restart clears it. The saved originals stay held, so "
                        "the hologram take stays refused. The layer stays stood down.",
                        ui_holo_remap::kSettleBoundaries);
        break;
    }
    detail::g_uiLayerIssueBlocked = false;
}
}  // namespace

void uiLayerFrameBoundary(ID3D11DeviceContext* ctx) {
    settleIssueFence(ctx);
    // The flat profile's back-off (flatBackOff) lifts itself here.
    if (runtimeFlatProfile() && g_flatBackOffUntilMs && !detail::g_uiLayerIssueBlocked &&
        GetTickCount64() >= g_flatBackOffUntilMs) {
        g_flatBackOffUntilMs = 0;
        g_stoodDown = false;
        g_crispStoodDown = false;
        for (Eye& e : g_eye) e.hdrMissStreak = 0;
        refreshLive();
        Log::get().note("ui quality: flat layer: re-armed after its back-off (%u this session); the cockpit HUD is taken "
                        "again from the next frame behind a proven tonemap.",
                        g_flatBackOffs);
    }
    detail::g_uiLayerWatching = false;
    g_watchBudget = kWatchPerFrame;
    // A tonemap admission whose draw never issued (swallowed) does not
    // survive the frame.
    g_crispPending = CrispTonePending{};
    detail::g_uiLayerCrispPending = false;
    // ...nor does the VR world route's pending re-issue (a screen draw the game's frame never issued).
    worldReissueReset();
    ++g_win.frames;
    if (detail::g_uiLayerLive) ++g_win.scene.framesLive;  // the frames the composite census could run in
    g_frameTakenDraws = 0;  // the door's emptiness test (uiLayerDoorLayerOnly) counts this frame's takes alone
    // The route's timers read back (the door reads them too).
    routePoll(ctx);
    g_hdrSeedGpu.poll(ctx);
    // The next frame's answer to "is the 2D screen the world?".
    onFootGateTick();
    // The flat adapter's per-draw facts belong to the frame that ended.
    g_flatDraw.valid = false;
    // A door size change's watch: the dropped line, two seconds on.
    sizeChangeTick();
    // The engine-side panel sizing's factor, written when its inputs settle.
    uiPanelScaleFrameBoundary();
    // The warm compile, the sharpen's reason: not a first-use D3DCompile at
    // the door.
    if (ctx && detail::g_uiLayerLive) {
        compileOnce(ctx);
        // The depth-stencil seed's three shaders and its deferred context,
        // warmed here too rather than mid-frame at the first tested UI draw
        // (review P3-3).
        if (!g_seeder && !g_seederTried) {
            Ptr<ID3D11Device> dev;
            ctx->GetDevice(&dev);
            ensureSeeder(dev.Get());
        }
        // the crisp-HUD half's of fix.ui_quality coverage shaders, the same.
        if (detail::g_uiLayerCrispOn) compileCoverageOnce(ctx);
    }
    // Not live -- off, no pass, the jitter switches set, stood down: this
    // frame's doors have run, so nothing still needs the layers. Let the
    // memory go; the next live frame makes them again.
    if (!detail::g_uiLayerLive && releaseLayers()) {
        Log::get().note("ui quality: layer: not live -- both eyes' layers and composite outputs "
                        "released.");
    }
    uiSurfacesFrameBoundary();
    const uint64_t now = GetTickCount64();
    if (!g_winStartMs) g_winStartMs = now;
    if (now - g_winStartMs < kTotalsMs) return;
    const bool anything = g_win.redirected || g_win.composites || g_win.compositeRefused;
    if (runtimeFlatProfile()) {
        // The flat profile: this file's per-eye layer is never live there (fix.temporal_aa reads off through the flat
        // gate), so its totals would only say so. The panel half's line is the key's 30 s account in flat.
        uiPanelScaleLog();
    } else if (g_target > 0.0f || anything) {
        uint64_t samples = 0;  // what logTotals is about to sort
        for (const RouteStats& r : g_routeStats) samples += r.n;
        PeriodicWorkScope timing(g_workTotals, samples);
        logTotals(static_cast<double>(now - g_winStartMs) / 1000.0);
    }
    g_win = Window{};
    g_seedCensus.nextWindow();
    for (RouteStats& r : g_routeStats) {
        r.n = 0;
        r.seen = 0;  // the reservoir restarts with the window
    }
    g_winStartMs = now;
}

// ---- the flat profile's mono adapter (ui_layer.h; flat_ui_layer.cpp) ----

void uiLayerFlatSetDraw(uint64_t frame, float jx, float jy, uint32_t renderW, uint32_t renderH) {
    g_flatDraw.valid = frame != 0 && renderW && renderH;
    g_flatDraw.seq = frame;
    g_flatDraw.jx = jx;
    g_flatDraw.jy = jy;
    if (g_flatDraw.renderW != renderW || g_flatDraw.renderH != renderH) g_tc = TargetCache{};  // the eye size moved
    g_flatDraw.renderW = renderW;
    g_flatDraw.renderH = renderH;
}

int uiLayerLastDecision() { return g_lastDecision; }

bool uiLayerFlatRelease() {
    const bool any = releaseLayers();
    for (Eye& e : g_eye) {
        e.door = UiLayerDoorState{};
        e.temporalOut = nullptr;
        e.temporalOutSeq = 0;
        e.doorFromPass = false;
        e.compositedSeq = 0;
    }
    g_tc = TargetCache{};
    g_flatDraw = FlatDraw{};
    g_crispPending = CrispTonePending{};
    detail::g_uiLayerCrispPending = false;
    return any;
}

namespace {
// Every child of the device the shared layer holds -- the layers and their depth targets, the composite's output and
// frame views, the route's timers, the blend cache, the depth seeder and its deferred context, the coverage pass's
// shaders and deferred context, the composite shader and its parameter buffer, the hologram remap's prepared shaders
// -- released, and every creation attempt and format-support answer forgotten, so the next use builds them on the
// device it is handed. uiLayerShutdown's, and uiLayerDeviceReset's (the flat profile's actual device change).
void releaseDeviceObjects() {
    if (g_draw.active) releaseSaved();
    g_holoBinding.clear();
    g_holoCache.reset();
    g_hdrSeedGpu.reset();  // release-only; the old device may already be gone
    g_hdrSeedActive = 0;
    releaseLayers();
    for (RouteSlot& s : g_route) {
        s.timer.reset();
        s.inUse = false;
    }
    g_routeHead = g_routeTail = 0;
    for (uint32_t i = 0; i < g_blendCount; ++i) g_blends[i] = BlendEntry{};
    g_blendCount = 0;
    g_seeder.reset();
    g_deferred.Reset();
    g_seederTried = false;
    g_crispDeferred.Reset();
    g_crispDeferredTried = false;
    if (g_covVs) {
        g_covVs->Release();
        g_covVs = nullptr;
    }
    if (g_covPs) {
        g_covPs->Release();
        g_covPs = nullptr;
    }
    g_covTried = false;
    if (g_cb) {
        g_cb->Release();
        g_cb = nullptr;
    }
    if (g_cs) {
        g_cs->Release();
        g_cs = nullptr;
    }
    g_csTried = false;
    for (int i = 0; i < 2; ++i) g_fmtChecked[i] = g_fmtOk[i] = false;
}
}  // namespace

void uiLayerDeviceReset() {
    releaseDeviceObjects();
    uiLayerFlatRelease();  // the door, the target cache and the per-draw facts, as on a same-device resize
    g_draw = Draw{};
    g_crispSave = CrispToneSave{};
    detail::g_uiLayerRedirecting = false;
    detail::g_uiLayerWatching = false;
}

void uiLayerShutdown() {
    g_holoBinding.clear(); g_holoCache.reset();
    detail::g_uiLayerIssueBlocked = false;
    g_holoSettleFailed = 0;
    g_privateDepthGuard.reset();
    g_hdrSeedGpu.reset(); // release-only shutdown; the owner/device may already be gone
    g_hdrSeedActive = 0;
    g_seedCensus = UiSeedCensus{};
    detail::g_uiSeedDiagnostics = false;
    if (g_draw.active) releaseSaved();
    g_draw = Draw{};
    g_crispPending = CrispTonePending{};
    g_crispSave = CrispToneSave{};
    detail::g_uiLayerCrispPending = false;
    worldReissueReset();
    g_maps = Maps{};
    g_frameTakenDraws = 0;
    detail::g_uiLayerMapsOn = false;
    releaseDeviceObjects();
    for (auto& totals : g_routeTotals) totals = UiRouteFrameTotals{};
    g_routeCoverage = UiRouteCoverage{};
    g_routeCombinedArmedPending = 0;
    g_routeHdrMovedPending = 0;
    g_crispToneSeenCount = 0;
    if (g_sessionRedirected) {
        Log::get().note("ui quality: layer: %llu draws redirected this session.",
                        static_cast<unsigned long long>(g_sessionRedirected));
    }
}

}  // namespace edvr
