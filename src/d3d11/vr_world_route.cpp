// The VR on-foot world route, the runtime (vr_world_route.h; the pure half is vr_world_route_math.h; design doc section 82).
//
// One call per game draw while the route is wanted (vrWorldRouteDraw) finds the flat HDR route's trigger, the tone, from
// the draw hooks' binding shadow; at the trigger it resolves the world's HDR scene image with flatMonoResolve, into H
// itself, on its own upscaler slot; the frame boundary accounts the frame and steps the ownership machine. What follows from
// an owned route (the eye shift off, the layer re-issuing each eye's screen draw, the door layer-only) lives in
// native_temporal.cpp, ui_layer.cpp and native_sharpen.cpp and reads this module through vr_world_route.h.
//
// COST. A draw with no colour target (16.8k shadow draws a frame on foot) dies at one shadow load. A coloured draw reads two
// view pointers from the shadow and looks them up in a per-frame table: a view's resource, size and format are resolved once
// a frame (four COM calls under the shadow's own guard and budget), never per draw. No allocation, no lock and no D3D call
// per draw. FlatContractObservation is ~360 bytes: one static instance, eleven fields written.
//
// THE JITTER (stage 2). The route jitters the world's cameras through the camera injector while it is Warming or Owned, with no
// key of its own; only the global experimental.temporal_aa_jitter off stops it, and the route then resolves an unjittered
// world, which is what flight 1 measured.
#include "vr_world_route.h"
#include "vr_world_route_math.h"
#include "binding_shadow.h"
#include "engine_velocity.h"
#include "flat_camera_inject.h"    // stage 2: the camera injector's VR mode
#include "flat_camera_phase.h"     // flatCameraCheckRowPair: the rows carry the phases they are claimed to
#include "flat_live_phase.h"       // FlatLivePhase: the flat profile's phase machine, reused
#include "dlaa.h"
#include "flat_mono_resolve.h"
#include "gpu_census.h"
#include "panel_curve.h"    // panelCurveInfo: the curve in use, named in the route's 5 s line and its OWNS line
#include "ui_layer.h"
#include "vr_camera_census.h"
#include "vr_world_mips.h"
#include "weapon_motion.h"
#include "vscreen.h"
#include "../common/config.h"
#include "../common/log.h"
#include "../common/temporal_mode.h"
#include <d3d11.h>
#include <wrl/client.h>
#include <windows.h>
#include <cstring>
#include <string>

namespace edvr {

static_assert(kVrWorldFeatureSlot < kUpscalerSlots && kVrWorldFeatureSlot >= kUpscalerEyeSlots,
              "the world's upscaler slot is one the backends have, and not one of the eyes' (dlaa.h)");

bool g_vrWorldWants = false;
bool g_vrWorldWatchWrites = false;
thread_local bool g_vrWorldInternal = false;

namespace {
using Microsoft::WRL::ComPtr;

// ---- the state of the route -------------------------------------------------------------------------------------
VrWorldMachine g_machine;
VrWorldKey g_key = VrWorldKey::Off;
VrWorldWindow g_win;
bool g_census = false;
const char* g_notLiveNoted = nullptr;        // the reason the route last said it stays off (one line per reason)
uint64_t g_frameNo = 0;                      // present frames seen by the route, consecutive; the resolver's gap test reads it
uint64_t g_windowStartMs = 0;
uint64_t g_ownedFramesEpisode = 0;           // frames this ownership episode (for the release line)
bool g_sceneResetEvent = false;              // the transition detector reset the eyes' history: the machine lets go at the boundary
bool g_sceneResetPending = false;            // ... and the resolver's history resets at the next treat
bool g_tookNoted = false;                    // the first layer take of this ownership episode was logged
bool g_firstTriggerLogged = false, g_latchLogged = false, g_lateWriteLogged = false;
std::atomic<bool> g_ownsNext{false};
uint64_t g_takenSequence[2] = {0, 0};
uint64_t g_countedLayerOnly[2] = {0, 0};     // the sequence each eye's layer-only door was last counted for (it is asked twice an eye)
bool g_startedOwned = false;                 // the frame now running started owned (the eye shift was off)
bool g_resourcesLive = false;                // the resolver (and the mipped screen) made resources: release them when the key goes off
bool g_gatePrev = false, g_gateSeen = false; // the on-foot gate at the last boundary, for the window's flip count

// ---- stage 2: the world jitter (design doc section 82) ---------------------------------------------------------------------
// The route puts the flat profile's phase into the game's own kind-3 world cameras through the camera injector, only while it
// is Warming or Owned and the global experimental.temporal_aa_jitter is on. The injector is driven from the frame boundary
// (driveInjector) and told at the trigger that the window is over; the route reads what it did at the trigger and at the next
// boundary. With the route key off from the start nothing below is ever called.
FlatLivePhase g_phase;                          // picks the frame's phase after two warm zero-phase frames
VrWorldJitter g_jitter = VrWorldJitter::Idle;   // this frame's decision (the frame that is running)
bool g_injectorEngaged = false;                 // flatCameraVrFrame was called with inject = true and the injector is not yet quiet again
bool g_injectFault = false;                     // a STOP line switched the injector off for the session
bool g_jitterLiveLogged = false;                // the JITTERED line was logged in this jitter period
bool g_hookUnavailableLogged = false;
float g_framePhaseX = 0.0f, g_framePhaseY = 0.0f; // the phase chosen for the running frame (what the injector was given)
bool g_frameCoverageOk = true;                  // every scene injection of the running frame landed
bool g_frameNamed = false;                      // the running frame's trigger named a source for the screen (the selector's depthNamed)
bool g_namedLast = false;                       // the frame that ended named one: the only thing that opens the next frame's window
VrWorldAppliedPhase g_frameWorldApplied;        // what the running frame's world rows carry (set at the trigger)
uint32_t g_noSceneLines = 0, g_pairMismatchLines = 0, g_excludedLogged = 0, g_unnamedLines = 0;
const char* g_declineWhy = nullptr;             // the selector's reason for the running frame's decline
const char* g_missWhy = nullptr;                // the reason of the last untreated frame (a decline or "no-trigger")
uint32_t g_missRun = 0;                         // consecutive untreated frames (for the RELEASED line)
uint32_t g_declineRunLines = 0, g_declineLinesSession = 0;   // decline lines logged in this run of declines / this session

// ---- the stage 2 experiment build (design doc section 82) -----------------------------------------------------------------------
// Read at the frame boundary, only while the route key is auto (readExperimentKeys), and used by the resolve at the trigger. The depth-checked
// steady detail (stale slots take the camera term where last frame's depth confirms it) has no key: treatWorld always hands the resolver
// steadyDetail = true (design doc section 82, flight 4). The debug key's motion_source and advanced.vr_camera_census default to off, and with
// both off treatWorld hands the resolver exactly what it handed it in flight 4.
bool g_viewOn = false, g_viewReported = false;          // advanced.temporal_aa_debug = motion_source: the resolver paints the prep's classes into H

// ---- the per-frame detector state ------------------------------------------------------------------------------------
struct ViewEntry {
    const void* view = nullptr;      // the shadow's view pointer this entry answers for
    VrWorldView v{};
};
constexpr unsigned kViewCache = 96;
struct Frame {
    FlatHdrFrame hdr;
    ViewEntry views[kViewCache];
    unsigned viewCount = 0;
    uint32_t viewOverflow = 0;
    uint32_t seq = 0;                        // coloured draws seen (the detector's q)
    uint32_t draws = 0;                      // every draw seen (the census's ordinal)
    const void* candDepth[kFlatHdrCandidates] = {};
    // The last coloured draw's target pair and whether it showed the route nothing to watch (vrWorldRouteDraw): the next draw
    // into the same pair skips at two compares. 16k draws into one shadow atlas are one lookup and 16k compares.
    const void* runRtv = nullptr;
    const void* runDsv = nullptr;
    bool runSkippable = false;
    bool depthMixed = false;
    bool treated = false;
    bool triggered = false;
};
Frame g_f;
FlatContractObservation g_k;                 // one instance; eleven fields written per coloured draw

// What the resolver kept from the last treated frame, for the next one's history decisions.
struct Previous {
    bool valid = false;
    uint64_t frame = 0;
    float rows[6][4] = {};
    const void* depth = nullptr;
    const void* color = nullptr;
    uint32_t w = 0, h = 0;
    VrWorldAppliedPhase worldApplied;   // the phase the stored rows carry (the injection landed), render pixels
    VrWorldAppliedPhase fpApplied;      // the phase the first-person camera carried that frame (the fold-in's mode reads both)
};
Previous g_prev;
uint64_t g_lastTreatMs = 0;

// The depth texture's shader view, made once per texture (the resolver wants an SRV over the world's depth).
struct DepthViews {
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11ShaderResourceView> depth;
    ComPtr<ID3D11ShaderResourceView> stencil;   // the stencil plane (R32G8X24 only): the weapon fold-in's second input; null otherwise
};
DepthViews g_depthViews;

FlatMonoResolveMode resolveModeFromConfig(bool* known) {
    const std::string m = Config::get().getString("fix.temporal_aa", "off");
    if (known) *known = temporalModeEnabled(m);
    if (_stricmp(m.c_str(), "fsr") == 0) return FlatMonoResolveMode::Fsr;
    if (_stricmp(m.c_str(), "dlss") == 0) return FlatMonoResolveMode::Dlss;
    if (_stricmp(m.c_str(), "dlaa") == 0) return FlatMonoResolveMode::Dlaa;
    return FlatMonoResolveMode::Taa;
}

const ViewEntry* lookup(Frame& f, void* view) {
    for (unsigned i = 0; i < f.viewCount; ++i)
        if (f.views[i].view == view) return &f.views[i];
    if (f.viewCount == kViewCache) { ++f.viewOverflow; return nullptr; }
    ViewEntry& e = f.views[f.viewCount++];
    e.view = view;
    ResourceInfo info{};
    e.v = VrWorldView{};
    if (bindingResolveProbe(view, &info)) {
        e.v.known = true; e.v.texture2d = info.isTexture2D; e.v.resource = info.resource;
        e.v.width = info.a; e.v.height = info.b; e.v.format = info.fmt;
    }
    return &e;
}

void declineLine(const char* why) {
    g_win.hdr.lastVerdict = why;
    ++g_win.hdr.declined;
    g_declineWhy = why;
    // The log caps per RUN of declines, not per session (flight 1 spent its twelve lines on the entry and the first seconds of
    // one gap): three lines for each run, 64 a session. The 5 s tallies carry the rest.
    if (vrWorldDeclineLogAllowed(g_declineRunLines, g_declineLinesSession)) {
        ++g_declineRunLines; ++g_declineLinesSession;
        Log::get().note("vr world route: declined at frame=%llu seq=%u: %s (the eye route serves this frame)",
                        static_cast<unsigned long long>(g_frameNo), g_f.hdr.trigger.sequence, why);
    }
}

// The injector's counters for the running frame, as the route reads them (a no-op copy of zeros when it is not engaged).
VrWorldInjectFrame readInjectFrame() {
    VrWorldInjectFrame f{};
    if (!g_injectorEngaged) return f;
    const FlatCameraVrCounters c = flatCameraVrCounters();
    f.sceneInjected = c.sceneInjected; f.sceneRefused = c.sceneRefused;
    f.firstPersonInjected = c.firstPersonInjected; f.firstPersonRefused = c.firstPersonRefused;
    f.warming = c.warming; f.auxiliary = c.auxiliary; f.afterTrigger = c.afterTrigger; f.unsupported = c.unsupported;
    f.otherKind = c.otherKind; f.unreadable = c.unreadable; f.writeFailures = c.writeFailures; f.offThread = c.offThread;
    for (int i = 0; i < 8; ++i) f.injectedKind[i] = c.injectedKind[i];
    f.fovNarrowest = c.fovNarrowest; f.fovWidest = c.fovWidest;
    return f;
}

bool makeDepthViews(ID3D11Device* device, ID3D11Texture2D* depth) {
    if (g_depthViews.texture.Get() == depth && g_depthViews.depth) return true;
    g_depthViews = DepthViews{};
    if (!depth) return false;
    D3D11_TEXTURE2D_DESC d{};
    depth->GetDesc(&d);
    if (d.SampleDesc.Count != 1 || d.ArraySize != 1 || !(d.BindFlags & D3D11_BIND_SHADER_RESOURCE)) return false;
    DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
    switch (d.Format) {
        case DXGI_FORMAT_R32G8X24_TYPELESS: fmt = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS; break;
        case DXGI_FORMAT_R32_TYPELESS: fmt = DXGI_FORMAT_R32_FLOAT; break;
        case DXGI_FORMAT_R24G8_TYPELESS: fmt = DXGI_FORMAT_R24_UNORM_X8_TYPELESS; break;
        case DXGI_FORMAT_R16_TYPELESS: fmt = DXGI_FORMAT_R16_UNORM; break;
        default: return false;
    }
    D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.Format = fmt;
    sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    sd.Texture2D.MipLevels = 1;
    ComPtr<ID3D11ShaderResourceView> srv;
    if (FAILED(device->CreateShaderResourceView(depth, &sd, srv.GetAddressOf()))) return false;
    g_depthViews.texture = depth;
    g_depthViews.depth = srv;
    if (d.Format == DXGI_FORMAT_R32G8X24_TYPELESS) {
        // The same texture's stencil plane, for the first-person fold-in. Optional: without it the route runs without the
        // weapon inputs (the flat path's weapon handling).
        sd.Format = DXGI_FORMAT_X32_TYPELESS_G8X24_UINT;
        ComPtr<ID3D11ShaderResourceView> stencil;
        if (SUCCEEDED(device->CreateShaderResourceView(depth, &sd, stencil.GetAddressOf()))) g_depthViews.stencil = stencil;
    }
    return true;
}

// THE WEAPON SEAM (docs section 82, item 3: Sean, 2026-09-30, builds the fold-in). The first-person motion map (weapon_motion.cpp:
// the weapon's own animated vertices, rebuilt into a source-sized map each frame a weapon draws) and the stencil plane of the
// world's depth (bit 0x10, written by the game's first-person draws) are two optional inputs of the resolver's prep: attached
// pixels take the map's motion, or reject their history, and never the world's camera term at the weapon's depth and field of
// view. BOTH OR NEITHER. Neither (no weapon drew this frame, fix.weapon_stability off, the depth without a stencil plane, a
// map the resolver refuses) is the flat path's weapon handling: today's arithmetic, bit for bit. What the fold-in needs at this
// seam, for whoever changes it: a map of FlatMonoResolveFrame::firstPersonMotion's contract and the depth's stencil view.
struct WorldFirstPerson {
    ID3D11ShaderResourceView* motion = nullptr;
    ID3D11ShaderResourceView* stencil = nullptr;
};
WorldFirstPerson worldFirstPerson() {
    WorldFirstPerson fp;
    ID3D11ShaderResourceView* map = weaponMotionView();   // null unless a weapon draw made the map THIS frame (and it is not ambiguous)
    if (!map || !g_depthViews.stencil) return fp;
    fp.motion = map;
    fp.stencil = g_depthViews.stencil.Get();
    return fp;
}

// THE JITTER (stage 2; the seam of the first build is gone). The phase is chosen at the frame boundary by the flat profile's own
// phase machine (FlatLivePhase), handed to the camera injector (flat_camera_inject.h), and read back at the trigger: the world
// resolves with the phase its scene camera call took. See driveInjector below and treatWorld.

void treatWorld(ID3D11DeviceContext* ctx) {
    Frame& fr = g_f;
    const FlatHdrTrigger& t = fr.hdr.trigger;
    if (g_machine.state == VrWorldState::Latched) { declineLine("latched-off"); return; }
    if (g_key != VrWorldKey::Auto) return;   // the detector runs for the census alone; nothing to treat

    bool engineKnown = false;
    const FlatMonoResolveMode mode = resolveModeFromConfig(&engineKnown);
    uint32_t screenW = 0, screenH = 0;
    vScreenPanelSize(&screenW, &screenH);

    // Which depth did H's draws use? The candidate that matched the consumer.
    const FlatHdrCandidate* cand = flatHdrFindCandidate(fr.hdr, t.hdr);
    const void* depthRes = nullptr;
    if (cand) depthRes = fr.candDepth[cand - fr.hdr.candidates];

    ID3D11Texture2D* depthTex = static_cast<ID3D11Texture2D*>(const_cast<void*>(depthRes));
    EngineVelocityViews ev{};
    VrWorldSelectFacts facts;
    facts.triggered = fr.hdr.triggered;
    facts.ambiguous = t.ambiguous;
    facts.engineKnown = engineKnown;
    facts.extentOk = t.hdrWidth == screenW && t.hdrHeight == screenH && screenW && screenH;
    facts.depthKnown = depthRes != nullptr && !fr.depthMixed;
    facts.depthNamed = facts.depthKnown && engineVelocitySourceIsNamed(depthTex);
    g_frameNamed = facts.depthNamed;   // the camera window's rule: it opens only after a frame that named its source
    float rows[6][4] = {};
    bool rowsOk = false;
    if (facts.depthNamed) {
        facts.engineViews = engineVelocitySourceViews(depthTex, &ev);
        float shaped[6][4];
        rowsOk = engineVelocitySourceCameraRows(rows) &&
                 flat_mono_detail::cameraShape(reinterpret_cast<const unsigned char*>(rows), shaped);
    }
    facts.cameraRows = rowsOk;
    const VrWorldSelect sel = vrWorldSelect(facts);
    g_win.hdr.noteSelection(vrWorldSelectName(sel));
    struct Release {
        EngineVelocityViews& v;
        ~Release() { if (v.slots) v.slots->Release(); if (v.pool) v.pool->Release(); if (v.sceneNow) v.sceneNow->Release();
                     if (v.scenePrev) v.scenePrev->Release(); if (v.gameMark) v.gameMark->Release(); }
    } release{ev};
    if (sel != VrWorldSelect::Selected) { declineLine(vrWorldSelectName(sel)); return; }

    ComPtr<ID3D11Device> device;
    ctx->GetDevice(device.GetAddressOf());
    if (!device) { declineLine("no-device"); return; }
    if (!flatHdrRouteEvaluatesAtRender(mode, t.hdrWidth, t.hdrHeight, screenW, screenH)) {
        declineLine("route-does-not-evaluate-at-render-size");
        return;
    }
    // Verify the actual binding once: the shadow only nominated this draw.
    if (!t.srvKnown || t.srvSlot > 3) { declineLine("trigger-without-a-shader-resource-slot"); return; }
    ComPtr<ID3D11ShaderResourceView> hdrView;
    ctx->PSGetShaderResources(t.srvSlot, 1, hdrView.GetAddressOf());
    ComPtr<ID3D11Resource> hdrResource;
    if (hdrView) hdrView->GetResource(hdrResource.GetAddressOf());
    if (!hdrView || hdrResource.Get() != t.hdr || !makeDepthViews(device.Get(), depthTex)) {
        declineLine("actual-hdr-binding-or-depth-view-refused");
        return;
    }

    FlatMonoResolveFrame f{};
    f.color = hdrView.Get();
    f.depth = g_depthViews.depth.Get();
    f.hdr = true;
    f.renderWidth = t.hdrWidth; f.renderHeight = t.hdrHeight;
    f.outputWidth = screenW; f.outputHeight = screenH;
    f.frame = g_frameNo;
    f.mode = mode;
    f.slot = kVrWorldFeatureSlot;
    std::memcpy(f.camera, rows, sizeof(f.camera));
    const bool resetMissing = !g_prev.valid;
    const bool resetGap = g_prev.valid && g_prev.frame + 1 != g_frameNo;
    const bool resetDepth = g_prev.valid && g_prev.depth != depthRes;
    const bool resetColor = g_prev.valid && g_prev.color != t.hdr;
    const bool resetExtent = g_prev.valid && (g_prev.w != t.hdrWidth || g_prev.h != t.hdrHeight);
    const bool resetScene = g_sceneResetPending;
    f.reset = resetMissing || resetGap || resetDepth || resetColor || resetExtent || resetScene;
    // THE EXPERIMENT BUILD. The steady detail, always on, sends a pixel whose engine slot a later draw overdrew to the camera term instead of
    // refusing its history, but only where last frame's depth confirms the camera term (FlatMonoResolveFrame::steadyDetail, the
    // depth-validated form; the blanket form, staticScene, is the flat 3D menu's alone and this route never sets it); the census key
    // counts the refused pixels by cause and the debug key's motion_source paints them. Each of those two is the resolver's own
    // contract and off by default, so a frame with both off is the frame flight 4 resolved.
    f.steadyDetail = true;
    f.refusalCensus = g_census;
    f.refusalView = g_viewOn ? 1u : 0u;
    // THE PHASE (stage 2). What the injector did to the world's cameras BEFORE the trigger is final: the route closed its window
    // at the trigger. The phase the frame APPLIED is the one it was given when a scene camera call took it, zero otherwise (a
    // frame the route meant to jitter and could not is resolved unjittered, and says so); the backend gets that phase as its
    // jitter and the resolver gets it as the phase the rows carry, this frame and last. The first-person camera takes the same
    // phase (the injector writes the same NDC shift into both), so the weapon fold-in adds the phase term to its map.
    const WorldFirstPerson fp = worldFirstPerson();
    const VrWorldInjectFrame inj = readInjectFrame();
    VrWorldAppliedPhase worldApplied, fpApplied;
    g_frameCoverageOk = true;
    if (g_jitter == VrWorldJitter::On) {
        const bool phaseNonzero = g_framePhaseX != 0.0f || g_framePhaseY != 0.0f;
        if (phaseNonzero && inj.sceneInjected > 0) worldApplied = {g_framePhaseX, g_framePhaseY};
        if (phaseNonzero && (inj.sceneRefused || inj.firstPersonRefused || inj.writeFailures)) {
            // Some of the frame's world cameras took the phase and some did not: the frame is part jittered and cannot be
            // resolved with one phase. The eye route serves it; the phase machine starts its warm-up again.
            g_frameCoverageOk = false;
            declineLine("camera-injection-incomplete");
            return;
        }
        if (phaseNonzero && inj.sceneInjected == 0 && g_noSceneLines < 8) {
            ++g_noSceneLines;
            char line[512];
            vrWorldFormatNoSceneInjection(line, sizeof(line), g_frameNo, inj);
            Log::get().note("%s", line);
        }
        // A frame with no weapon drawn has no attached pixels: the world's phase stands in for the first-person camera's.
        // A weapon drawn with no first-person call injected carried no phase at all.
        fpApplied = !fp.motion ? worldApplied
                               : ((inj.firstPersonInjected > 0 && inj.firstPersonRefused == 0) ? worldApplied : VrWorldAppliedPhase{});
    }
    const VrWorldAppliedPhase prevWorld = f.reset ? worldApplied : g_prev.worldApplied;
    const VrWorldAppliedPhase prevFp = f.reset ? fpApplied : g_prev.fpApplied;
    f.jitterX = worldApplied.x; f.jitterY = worldApplied.y;
    f.previousJitterX = prevWorld.x; f.previousJitterY = prevWorld.y;
    f.rowsJitterX = worldApplied.x; f.rowsJitterY = worldApplied.y;
    f.previousRowsJitterX = prevWorld.x; f.previousRowsJitterY = prevWorld.y;
    std::memcpy(f.previousCamera, f.reset ? rows : g_prev.rows, sizeof(f.previousCamera));
    f.engine = ev;
    const uint64_t nowMs = GetTickCount64();
    f.deltaMs = g_lastTreatMs ? static_cast<float>(nowMs - g_lastTreatMs) : 11.111f;
    f.firstPersonMotion = fp.motion;
    f.firstPersonStencil = fp.stencil;
    f.firstPersonPhaseMode = (fp.motion && fp.stencil) ? vrWorldFirstPersonMode(worldApplied, prevWorld, fpApplied, prevFp) : 0u;
    if (!f.reset) {
        // The live proof that the rows carry the phases they are claimed to (the flat runtime's check): two consecutive
        // frames' rows differ by exactly the difference of the two phases. Evidence, not a gate: the frame is still resolved.
        const FlatCameraPairResult pair = flatCameraCheckRowPair(rows, worldApplied.x, worldApplied.y, g_prev.rows, prevWorld.x,
                                                                prevWorld.y, t.hdrWidth, t.hdrHeight);
        if (pair.verdict != FlatCameraPairVerdict::Skipped) {
            ++g_win.inject.pairChecked;
            if (pair.verdict == FlatCameraPairVerdict::Inconsistent) {
                ++g_win.inject.pairBad;
                if (g_pairMismatchLines < 8) {
                    ++g_pairMismatchLines;
                    char line[384];
                    vrWorldFormatRowsMismatch(line, sizeof(line), g_frameNo, worldApplied.x, worldApplied.y, prevWorld.x,
                                              prevWorld.y, pair.maxError);
                    Log::get().note("%s", line);
                }
            }
        }
    }

    ComPtr<ID3D11ShaderResourceView> none;
    const char* why = nullptr;
    bool ok = false;
    try {
        GpuCensusScope census(ctx, GpuCensusSection::FrameWorldResolve);
        VrWorldInternalScope internal;
        ok = flatMonoResolve(device.Get(), ctx, f, none.GetAddressOf(), &why);
    } catch (...) {
        ok = false;
        why = "exception-in-resolve";
    }
    if (!ok) {
        // The backend or the route's own guard refused before H was written (the resolver writes H last): H is still the
        // game's own, and the world is unjittered, so there is nothing to recover. The eye route serves the frame.
        g_prev.valid = false;
        declineLine(why ? why : "resolve-refused");
        return;
    }
    fr.treated = true;
    g_resourcesLive = true;
    g_sceneResetPending = false;
    g_lastTreatMs = nowMs;
    ++g_win.hdr.treated;
    g_win.hdr.lastVerdict = f.reset ? "treated-reset" : "treated";
    g_prev.valid = true;
    g_prev.frame = g_frameNo;
    std::memcpy(g_prev.rows, rows, sizeof(g_prev.rows));
    g_prev.depth = depthRes;
    g_prev.color = t.hdr;
    g_prev.w = t.hdrWidth; g_prev.h = t.hdrHeight;
    g_prev.worldApplied = worldApplied;
    g_prev.fpApplied = fpApplied;
    g_frameWorldApplied = worldApplied;
    if (f.firstPersonMotion && f.firstPersonStencil) ++g_win.foldMode[f.firstPersonPhaseMode > 2 ? 2 : f.firstPersonPhaseMode];
    if (!worldApplied.zero() && !g_jitterLiveLogged) {
        g_jitterLiveLogged = true;
        char line[512];
        vrWorldFormatJitterLive(line, sizeof(line), g_frameNo, worldApplied.x, worldApplied.y, t.hdrWidth, t.hdrHeight, inj);
        Log::get().note("%s", line);
    }
}

void onTrigger(ID3D11DeviceContext* ctx) {
    Frame& fr = g_f;
    fr.triggered = true;
    g_vrWorldWatchWrites = true;
    // The world's camera calls all precede the trigger: the injection window is over (a screen-view call after it is counted
    // and never written). Only while the injector is engaged; with the route key off it never is.
    if (g_injectorEngaged) flatCameraVrCloseWindow();
    if (!g_firstTriggerLogged) {
        g_firstTriggerLogged = true;
        char line[512];
        vrWorldFormatFirstTrigger(line, sizeof(line), g_frameNo, fr.hdr);
        Log::get().note("%s", line);
    }
    treatWorld(ctx);
}

}  // namespace

// ---- the published state (final: other modules link against these) --------------------------------------------------------
bool vrWorldRouteEnabled() { return g_key == VrWorldKey::Auto; }
VrWorldState vrWorldRouteState() { return g_machine.state; }
bool vrWorldRouteOwnsNextFrame() { return g_ownsNext.load(std::memory_order_acquire); }
bool vrWorldRouteTreatedThisFrame() { return g_f.treated; }
bool vrWorldRouteLayerMayTake() { return vrWorldLayerMayTake(g_machine.state, g_f.treated); }
void vrWorldRouteNoteEyeTaken(uint32_t eye, uint64_t sequence) {
    if (eye < 2) g_takenSequence[eye] = sequence;
    ++g_win.takes;
    if (!g_tookNoted) {
        g_tookNoted = true;
        Log::get().note("vr world route: the layer took the screen for eye %u at frame=%llu (sequence %llu): the eye upscaler, "
                        "its motion prep and UI resolve stand aside for this eye while the route holds the world",
                        eye, static_cast<unsigned long long>(g_frameNo), static_cast<unsigned long long>(sequence));
    }
}
bool vrWorldRouteDoorLayerOnly(uint32_t eye, uint64_t sequence) {
    if (eye >= 2 || !vrWorldDoorLayerOnly(g_takenSequence[eye], sequence)) return false;
    // Asked twice an eye and sequence (the temporal door and then the sharpen pass): counted once.
    if (g_countedLayerOnly[eye] != sequence) { g_countedLayerOnly[eye] = sequence; ++g_win.layerOnly; }
    return true;
}
void vrWorldRouteNoteSceneReset() {
    if (g_key != VrWorldKey::Auto) return;
    g_sceneResetEvent = true;
    g_sceneResetPending = true;
    ++g_win.resets;
}
bool vrWorldRouteDrawProgress(uint32_t* drawOrdinal, bool* toneSeen, uint64_t* frame) {
    if (!g_vrWorldWants) return false;
    if (drawOrdinal) *drawOrdinal = g_f.draws;
    if (toneSeen) *toneSeen = g_f.triggered;
    if (frame) *frame = g_frameNo;
    return true;
}

bool vrWorldRouteWorldPhase(float* x, float* y) {
    const bool on = g_jitter == VrWorldJitter::On;
    if (x) *x = on ? g_framePhaseX : 0.0f;
    if (y) *y = on ? g_framePhaseY : 0.0f;
    return on;
}

// ---- writes after the trigger (the late-write latch's inputs) ---------------------------------------------------------------
void vrWorldRouteNoteWrite(const void* resource) {
    if (!g_f.hdr.triggered) return;
    flatHdrObserveExplicitWrite(g_f.hdr, resource);
}
void vrWorldRouteNoteDispatch() {
    if (!g_f.hdr.triggered) return;
    for (uint32_t i = 0; i < 4; ++i) {
        void* uav = bindingGet(static_cast<BindSlot>(static_cast<uint32_t>(BindSlot::CsUav0) + i));
        if (!uav) continue;
        const ViewEntry* e = lookup(g_f, uav);
        if (e && e->v.known && e->v.resource) flatHdrObserveDispatchWrite(g_f.hdr, e->v.resource);
    }
}
void vrWorldRouteNoteRtvClear(void* rtv) {
    if (!g_f.hdr.triggered || !rtv) return;
    const ViewEntry* e = lookup(g_f, rtv);
    if (e && e->v.known && e->v.resource) flatHdrObserveExplicitWrite(g_f.hdr, e->v.resource);
}

// ---- the per-draw hook ---------------------------------------------------------------------------------------------------------
void vrWorldRouteDraw(ID3D11DeviceContext* ctx) {
    Frame& f = g_f;
    ++f.draws;
    // The census's episodes (vr_camera_census.h): the pointer is set only while an episode's sampled frame runs, so any other draw, and every draw with the
    // census off, costs this one load. Before the colour-target test below: the join wants depth-only draws too.
    if (detail::g_vrCensusJoinDraw) detail::g_vrCensusJoinDraw(ctx, f.draws);
    void* rtv = bindingGet(BindSlot::Rtv0);
    if (!rtv) return;                                   // depth-only: no colour target, so no candidate, consumer or H write
    const uint32_t q = ++f.seq;
    void* dsv = bindingGet(BindSlot::Dsv0);
    // A run of draws into one target pair that the last of them showed to be nothing the route watches for (not a candidate,
    // not a consumer, not a write into H) is the same answer again: the shadow atlas takes thousands in a row.
    if (f.runSkippable && rtv == f.runRtv && dsv == f.runDsv) return;
    f.runRtv = rtv; f.runDsv = dsv; f.runSkippable = false;
    const ViewEntry* c = lookup(f, rtv);
    const ViewEntry* d = dsv ? lookup(f, dsv) : nullptr;
    FlatContractObservation& k = g_k;
    if (!c || !vrWorldFillObservation(k, &c->v, d ? &d->v : nullptr, rtv, dsv, bindingShaderHash(BindSlot::Vs),
                                      bindingShaderHash(BindSlot::Ps))) {
        f.runSkippable = true;                          // a colour target the shadow cannot read is ignored, run or not
        return;
    }
    if (!f.hdr.outputWidth) {   // before the first boundary armed the detector
        uint32_t outW = 0, outH = 0;
        vScreenPanelSize(&outW, &outH);
        flatHdrBeginFrame(f.hdr, outW, outH);
    }

    const void* srv[4] = {};
    bool srvKnown = false;
    const bool consumer = !f.hdr.triggered && flatHdrCouldConsume(f.hdr, k);
    if (consumer) {
        for (uint32_t i = 0; i < 4; ++i) {
            void* v = bindingGet(static_cast<BindSlot>(static_cast<uint32_t>(BindSlot::PsSrv0) + i));
            if (!v) continue;
            const ViewEntry* e = lookup(f, v);
            srv[i] = e && e->v.known ? e->v.resource : nullptr;
        }
        srvKnown = true;
    }
    const bool candidate = !f.hdr.triggered && flatHdrCandidateDraw(k, f.hdr.outputWidth, f.hdr.outputHeight);
    const bool trigger = flatHdrObserveDraw(f.hdr, k, q, srv, srvKnown);
    if (candidate) {
        // One depth target for all of H's draws: the route resolves against it.
        if (const FlatHdrCandidate* hc = flatHdrFindCandidate(f.hdr, k.color)) {
            const void*& slot = f.candDepth[hc - f.hdr.candidates];
            if (!slot) slot = k.depth;
            else if (slot != k.depth) f.depthMixed = true;
        }
    }
    f.runSkippable = !trigger && !candidate && !consumer && !(f.hdr.triggered && flatHdrFindCandidate(f.hdr, k.color));
    if (trigger) onTrigger(ctx);
}

// ---- stage 2: driving the camera injector -----------------------------------------------------------------------------------------
namespace {
// The kind-3 calls the role test excluded, once per distinct signature (the first eight the injector keeps): no I/O on the
// game's call path, so the route says it here, at the boundary.
void logNewExcluded(float screenAspect) {
    FlatCameraVrExcluded ex[8];
    const size_t n = flatCameraVrExcluded(ex, 8);
    for (size_t i = g_excludedLogged; i < n; ++i) {
        char line[384];
        vrWorldFormatExcluded(line, sizeof(line), ex[i].aspect, ex[i].fov, ex[i].nearZ, ex[i].farZ,
                              static_cast<unsigned long long>(ex[i].callerRva), screenAspect,
                              static_cast<unsigned long long>(ex[i].calls));
        Log::get().note("%s", line);
    }
    if (n > g_excludedLogged) g_excludedLogged = static_cast<uint32_t>(n);
}

// The frame boundary's injector step for a route that is on (key auto): decide the phase of the frame that starts, hand it to
// the injector, or wind the injector down. wantsInjection: the route is Warming or Owned, the gate holds and the layer is live.
void driveInjector(bool wantsInjection) {
    const bool globalJitter = Config::get().getBool("experimental.temporal_aa_jitter", true);
    uint32_t w = 0, h = 0;
    vScreenPanelSize(&w, &h);
    VrWorldJitter want = vrWorldJitterDecide(true, globalJitter, wantsInjection && w && h, g_namedLast, true, g_injectFault);
    g_framePhaseX = g_framePhaseY = 0.0f;
    g_frameCoverageOk = true;
    g_frameWorldApplied = VrWorldAppliedPhase{};
    if (want == VrWorldJitter::On) {
        // The phase machine yields a zero phase for its first two frames (warm-up), then the flat profile's Halton sequence.
        g_phase.beginFrame(true, g_prev.valid && g_f.treated, w, h);
        g_framePhaseX = g_phase.currentX;
        g_framePhaseY = g_phase.currentY;
        FlatCameraVrFrame vf;
        vf.inject = true;
        vf.observe = g_census;
        vf.phaseX = g_framePhaseX;
        vf.phaseY = g_framePhaseY;
        vf.renderW = w;
        vf.renderH = h;
        vf.screenAspect = static_cast<float>(w) / static_cast<float>(h);
        const bool live = flatCameraVrFrame(vf);
        g_injectorEngaged = true;
        if (!live) {
            want = VrWorldJitter::NoHook;
            g_framePhaseX = g_framePhaseY = 0.0f;
            g_phase.resetHistory();
            if (!g_hookUnavailableLogged) {
                g_hookUnavailableLogged = true;
                Log::get().note("vr world route: the camera hook is not available (%s): the world stays unjittered",
                                flatCameraVrStatus());
            }
        }
    } else {
        g_phase.beginFrame(false, false, w, h);
        if (g_injectorEngaged) {
            // Not injecting this frame: the injector goes pass-through (the census, if it is on, re-asserts observation in its
            // own boundary step right after this one) and, once no camera it wrote is waiting for its flush, is left alone.
            FlatCameraVrFrame vf;
            flatCameraVrFrame(vf);
            if (flatCameraVrQuiet()) g_injectorEngaged = false;
        }
    }
    if (want != VrWorldJitter::On) g_jitterLiveLogged = false;
    g_jitter = want;
}
// The screen's curve as the route's lines say it (vr_world_route_math.h vrWorldFormatCurve), from panel_curve.h's state at this
// boundary: "off" while the game's own quad is drawn, "pending" while the substitution is still learning the panel's SIZE, "stood-down",
// else curvature/columns/gain -- the numbers of the strip the layer's re-issue draws. `reissues` (when asked) is panel_curve's
// cumulative count of strips the route's layer drew; the window line prints its delta.
uint64_t g_curveReissuesSeen = 0;
void curveTextNow(char* out, size_t size, uint64_t* reissues) {
    const PanelCurveInfo pc = panelCurveInfo();
    const bool configured = pc.curvature > 0.0f || pc.segments != detail::kDefaultSegments;
    vrWorldFormatCurve(out, size, configured, pc.standDown, pc.ready, pc.curvature, pc.segments, pc.gain);
    if (reissues) *reissues = pc.reissues;
}

// The stage 2 experiment build's keys, read at the boundary while the route key is auto (design doc section 82): the debug key's
// motion_source (the refusal view) and, beside it, what the census key said at the top of the boundary (g_census). A change of the view
// is said once, in the log, with what the state does. The steady detail is not read here: it has no key, treatWorld always asks for it.
void readExperimentKeys() {
    g_viewOn = _stricmp(Config::get().getString("advanced.temporal_aa_debug", "off").c_str(), "motion_source") == 0;
    if (g_viewOn != g_viewReported) {
        g_viewReported = g_viewOn;
        char line[768];
        vrWorldFormatViewChanged(line, sizeof(line), g_frameNo, g_viewOn);
        Log::get().note("%s", line);
    }
}
// The route's key is off (live): nothing is injected; an engaged injector is wound down once it has nothing left to flush.
void windDownInjector() {
    g_injectFault = false;           // flipping the key off is what clears a STOP
    g_namedLast = g_frameNamed = false;
    g_jitter = VrWorldJitter::Idle;
    g_jitterLiveLogged = false;
    g_framePhaseX = g_framePhaseY = 0.0f;
    g_frameWorldApplied = VrWorldAppliedPhase{};
    g_phase.resetHistory();
    if (g_injectorEngaged) {
        FlatCameraVrFrame vf;
        flatCameraVrFrame(vf);
        if (flatCameraVrQuiet()) g_injectorEngaged = false;
    }
}
}  // namespace

// ---- the frame boundary ------------------------------------------------------------------------------------------------------------
void vrWorldRouteFrameBoundary() {
    // The key, live: read here, so the frame that starts now is the first to see a change. Auto for an ini with no line (the shipped
    // edvr.ini says auto too, and tools\config_test holds the two to one answer); any other word reads as off.
    const VrWorldKey key = vrWorldKeyFromText(Config::get().getString("experimental.temporal_aa_on_foot_world", "auto").c_str());
    g_key = key;
    g_census = vrCameraCensusWanted();
    if (key == VrWorldKey::Off && g_machine.state == VrWorldState::Off && !g_census && !g_injectorEngaged) {
        // Nothing to account and nothing to arm: the key-off path is this early return.
        g_vrWorldWants = false; g_vrWorldWatchWrites = false;
        g_ownsNext.store(false, std::memory_order_release);
        return;
    }
    ++g_frameNo;

    // The frame that ended. A curved screen (fix.panel_curvature above 0) does not hold the route off: the layer re-issues it through
    // the very strip the game's own draw is substituted with (panel_curve.h panelCurveReissue, vscreen.cpp curvedScreenSwallowed), so
    // the route needs the layer live and nothing of the screen's geometry. The curve in use is named in the 5 s line (curve=) and the
    // OWNS line.
    const bool layerLive = uiLayerLiveForWorldRoute();
    const bool gate = uiLayerWorldScreenHeld();
    if (key == VrWorldKey::Auto) {
        readExperimentKeys();
        if (g_win.hdr.frames == 0 && g_windowStartMs == 0) g_windowStartMs = GetTickCount64();
        if (g_vrWorldWants) {
            g_win.hdr.noteFrame(g_f.hdr);
            ++g_win.gateFrames;
        }
        if (g_gateSeen && gate != g_gatePrev) ++g_win.gateFlips;
        g_gatePrev = gate; g_gateSeen = true;
        if (g_startedOwned) { ++g_win.ownedFrames; ++g_ownedFramesEpisode; }
        // Stage 2: what the injector did in the frame that ended (its counters, read before the next VrFrame resets them), the
        // phase machine's end of frame, and the run of untreated frames the RELEASED line names.
        if (g_injectorEngaged) {
            const VrWorldInjectFrame inj = readInjectFrame();
            vrWorldAddInjectFrame(g_win.inject, inj);
            if (vrWorldInjectedWrongKind(inj) && !g_injectFault) {
                g_injectFault = true;
                char line[512];
                vrWorldFormatStopWrongKind(line, sizeof(line), g_frameNo, inj);
                Log::get().note("%s", line);
            }
            uint32_t pw = 0, ph = 0;
            vScreenPanelSize(&pw, &ph);
            logNewExcluded(ph ? static_cast<float>(pw) / static_cast<float>(ph) : 0.0f);
            // The naming rule. A write on a frame the route had shut (a shut window is never written through) is a STOP. A frame
            // whose window was open and that named nothing is the expected first frame of a map or menu: counted, said once per
            // change (eight lines a session), the window shuts for the next frame by itself.
            const bool injected = (inj.sceneInjected + inj.firstPersonInjected) > 0;
            if (injected && g_jitter != VrWorldJitter::On) {
                g_win.inject.shutInjected += inj.sceneInjected + inj.firstPersonInjected;
                if (!g_injectFault) {
                    g_injectFault = true;
                    char line[640];
                    vrWorldFormatStopShut(line, sizeof(line), g_frameNo, vrWorldJitterName(g_jitter), inj);
                    Log::get().note("%s", line);
                }
            } else if (injected && !g_frameNamed) {
                ++g_win.inject.unnamedFrames;
                if (g_unnamedLines < 8) {
                    ++g_unnamedLines;
                    char line[512];
                    vrWorldFormatUnnamedOpen(line, sizeof(line), g_frameNo, inj);
                    Log::get().note("%s", line);
                }
            }
        }
        g_namedLast = g_frameNamed;
        g_win.phaseX = g_framePhaseX; g_win.phaseY = g_framePhaseY;
        g_win.rowsX = g_f.treated ? g_frameWorldApplied.x : 0.0f;
        g_win.rowsY = g_f.treated ? g_frameWorldApplied.y : 0.0f;
        if (g_jitter == VrWorldJitter::On) g_phase.finish(g_f.treated, g_frameCoverageOk);
        if (g_f.treated) { g_missRun = 0; g_declineRunLines = 0; }
        else { ++g_missRun; g_missWhy = g_f.triggered && g_declineWhy ? g_declineWhy : "no-trigger"; }
        g_declineWhy = nullptr;
        VrWorldFrameEnd e;
        e.keyOn = true;
        e.layerLive = layerLive;
        e.gate = gate;
        e.treated = g_f.treated;
        e.lateWrites = g_f.treated && flatHdrLateWrites(g_f.hdr) != 0;
        e.sceneReset = g_sceneResetEvent;              // the machine lets go; the resolver resets at its next treat
        g_sceneResetEvent = false;
        if (e.lateWrites) {   // (the window's own late-write counters are noted by noteFrame above)
            if (!g_lateWriteLogged) {
                g_lateWriteLogged = true;
                Log::get().note("vr world route: frame=%llu wrote the scene HDR after the trigger: %u draw(s), %u dispatch(es), "
                                "%u explicit write(s); the first is seq %u VS=%016llX PS=%016llX; a treated frame with such "
                                "writes counts toward the latch (%u turns the route off)",
                                static_cast<unsigned long long>(g_frameNo), g_f.hdr.lateDraws, g_f.hdr.lateDispatches,
                                g_f.hdr.lateExplicit, g_f.hdr.lateSequence,
                                static_cast<unsigned long long>(g_f.hdr.lateVs), static_cast<unsigned long long>(g_f.hdr.latePs),
                                kFlatHdrLatchFrames);
            }
        }
        const VrWorldStep step = g_machine.frameEnd(e);
        if (step.entered) {
            ++g_win.enters;
            g_tookNoted = false;
            g_ownedFramesEpisode = 0;
            char line[384];
            char curveText[sizeof(g_win.curve)];
            curveTextNow(curveText, sizeof(curveText), nullptr);
            vrWorldFormatEntered(line, sizeof(line), g_frameNo, kVrWorldWarmFrames, curveText);
            Log::get().note("%s", line);
        }
        if (step.released != VrWorldRelease::None) {
            ++g_win.releases;
            g_win.lastRelease = vrWorldReleaseName(step.released);
            char line[512];
            const bool declined = step.released == VrWorldRelease::Declined;
            vrWorldFormatReleased(line, sizeof(line), g_frameNo, step.released, g_ownedFramesEpisode,
                                  declined ? g_missWhy : nullptr, declined ? g_missRun : 0u);
            Log::get().note("%s", line);
            g_ownedFramesEpisode = 0;
        }
        if (g_machine.state == VrWorldState::Latched && !g_latchLogged) {
            g_latchLogged = true;
            Log::get().note("vr world route: turned off at frame=%llu after %u treated frame(s) wrote the scene HDR after the "
                            "resolve; the eye route serves the eyes until experimental.temporal_aa_on_foot_world is set off and "
                            "auto again",
                            static_cast<unsigned long long>(g_frameNo), kFlatHdrLatchFrames);
        }
        if (g_machine.state != VrWorldState::Latched) g_latchLogged = false;
        // The key is auto but the layer is not live: one line per reason, saying the route stays off and why.
        if (!layerLive) {
            const char* why = uiLayerNotLiveReason();
            if (!why) why = "the UI layer is not live";
            if (g_notLiveNoted != why && (!g_notLiveNoted || std::strcmp(g_notLiveNoted, why) != 0)) {
                g_notLiveNoted = why;
                Log::get().note("vr world route: experimental.temporal_aa_on_foot_world is auto but the route stays off, and "
                                "on-foot VR keeps today's two-eye route: %s (the route hands the eyes the resolved screen through "
                                "the UI layer, so keep fix.ui_quality on)", why);
            }
        } else {
            g_notLiveNoted = nullptr;
        }
        // The 5 s window: zeros included while the key is auto.
        const uint64_t now = GetTickCount64();
        if (g_windowStartMs && now - g_windowStartMs >= 5000) {
            char line[1200];
            g_win.jitter = vrWorldJitterName(g_jitter);
            uint64_t curveReissues = 0;
            curveTextNow(g_win.curve, sizeof(g_win.curve), &curveReissues);
            g_win.curveReissues = curveReissues - g_curveReissuesSeen;
            g_curveReissuesSeen = curveReissues;
            vrWorldFormatWindow(line, sizeof(line), key, g_machine.state, layerLive, gate, g_win);
            Log::get().note("%s", line);
            char injectLine[512];
            vrWorldFormatInjectWindow(injectLine, sizeof(injectLine), g_win.inject);
            Log::get().note("%s", injectLine);
            // The refusal census's line (the census key on, samples still in flight from before it went off, or the steady detail's depth
            // check, which counts its own frames whatever the census asks): the resolver's sums since the last window beside the route's
            // own treated count. Absent when the census was not wanted and the resolver counted no frame. Its steady-detail= token is
            // the window's default, "on": the route has nothing to tell it.
            const FlatMonoRefusalCensus rc = flatMonoResolveTakeRefusalCensus();
            if (g_census || rc.asked || rc.sampled || rc.frames || rc.checked || rc.skipped) {
                VrWorldRefusalWindow rw;
                rw.census = g_census;
                rw.treated = g_win.hdr.treated;
                rw.asked = rc.asked; rw.sampled = rc.sampled; rw.dropped = rc.dropped; rw.read = rc.frames;
                rw.every = rc.every;
                rw.width = rc.width; rw.height = rc.height; rw.pixels = rc.pixels;
                for (uint32_t i = 0; i < kFlatMonoRefusalSlots; ++i) rw.counts[i] = rc.counts[i];
                rw.checked = rc.checked; rw.skipped = rc.skipped;
                rw.view = g_viewOn ? "on" : "off";
                char refusalLine[1024];
                vrWorldFormatRefusalWindow(refusalLine, sizeof(refusalLine), rw);
                Log::get().note("%s", refusalLine);
            }
            g_win.reset();
            g_windowStartMs = now;
        }
    } else {
        // The key went off (or never was on): the machine lets go at once and the route's state is cleaned up.
        VrWorldFrameEnd e;
        const VrWorldStep step = g_machine.frameEnd(e);
        if (step.released != VrWorldRelease::None) {
            char line[384];
            vrWorldFormatReleased(line, sizeof(line), g_frameNo, step.released, g_ownedFramesEpisode);
            Log::get().note("%s", line);
        }
        g_ownedFramesEpisode = 0; g_tookNoted = false; g_win.reset(); g_windowStartMs = 0;
        g_sceneResetPending = g_sceneResetEvent = false; g_prev.valid = false; g_declineRunLines = g_declineLinesSession = 0;
        g_viewOn = g_viewReported = false;   // the experiment build's view key is only read while the key is auto
        g_missRun = 0; g_missWhy = nullptr; g_declineWhy = nullptr;
        windDownInjector();
        if (g_resourcesLive) {
            // The key went off after the route had made its resolver resources (about 300 MB at 5040x2835) and the mipped
            // screen: let them go once, on the render thread, inside the route's own scope.
            g_resourcesLive = false;
            g_depthViews = DepthViews{};
            VrWorldInternalScope internal;
            flatMonoResolveReset();
            vrWorldMipsReset();
        }
    }

    // The state the next frame starts in.
    g_ownsNext.store(g_machine.owned(), std::memory_order_release);
    g_startedOwned = g_machine.owned();
    g_vrWorldWants = g_census || (key == VrWorldKey::Auto && g_machine.wantsDraws() && gate && layerLive);
    g_vrWorldWatchWrites = false;
    // Stage 2: the phase of the frame that starts. The route must be Warming or Owned (a frame the route may not resolve is not
    // jittered at the source) and every condition that makes it watch draws must hold.
    if (key == VrWorldKey::Auto) {
        const bool routeWorks = g_machine.state == VrWorldState::Warming || g_machine.state == VrWorldState::Owned;
        driveInjector(routeWorks && g_vrWorldWants && gate && layerLive);
    }

    // Arm the detector for the frame that starts now.
    uint32_t outW = 0, outH = 0;
    vScreenPanelSize(&outW, &outH);
    flatHdrBeginFrame(g_f.hdr, outW, outH);
    g_f.viewCount = 0; g_f.viewOverflow = 0; g_f.seq = 0; g_f.draws = 0;
    g_f.runRtv = g_f.runDsv = nullptr; g_f.runSkippable = false;
    std::memset(g_f.candDepth, 0, sizeof(g_f.candDepth));
    g_f.depthMixed = false; g_f.treated = false; g_f.triggered = false;
    g_frameNamed = false;
    g_takenSequence[0] = g_takenSequence[1] = 0;
    g_countedLayerOnly[0] = g_countedLayerOnly[1] = 0;
}

}  // namespace edvr
