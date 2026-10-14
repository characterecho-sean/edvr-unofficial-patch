#pragma once
// The blend probe (TEMPORARY; docs/dlss-blur-elements-2026-10-14.md). LOG ONLY: it changes no draw, no state that outlives a call and
// no pixel. A press of hotkey.blend_probe (unbound by default) captures 3 consecutive frames and reports, in the log:
//   (1) a ROW for every eye draw that writes colour and is blend-enabled or writes no depth AND whose samples reach a 96x96 window at
//       the view centre: shader hashes, draw kind and counts, the blend and depth state, the render target, the covered samples,
//       EDVR's verdict for it, the UI layer's decision, the hologram and coverage routes it matched, the reactive-mask strength a
//       coverage family writes, and whether the pair is keyed for engine-record motion (blend_probe_core.h has the text);
//   (2) per captured frame an 8x8 GRID over the same window, read after the temporal pass's motion dispatch: depth, motion, the
//       decision path, the engine-record kind, the coverage mask and the bias mask, with the window's median motion by path and the
//       pass's camera term beside it.
// Coverage is measured by a scissored duplicate of each candidate draw that writes nothing, counted by an occlusion query
// (the look probe's method, claude/hangar-test-4 look_probe.cpp, widened from one pixel to the window). Draws are offered by
// vscreen.cpp's forwardWithVerdict, after the routing is decided and before the game's own issue; the grid is offered by the
// temporal pass (temporal_pass.cpp) after eye 0's motion dispatch. Unarmed, each entry is one relaxed atomic load.
#include <cstdint>

struct ID3D11DeviceContext;
struct ID3D11Texture2D;

namespace edvr {

// Once, at install: one line saying whether hotkey.blend_probe is bound (key is the ini text it was bound from).
void blendProbeInstall(bool flatProfile, bool keyBound, const char* key);   // the settings file's name comes from Config::iniName()
// A press of hotkey.blend_probe: arms a capture at the next frame boundary. Refused (and said) while one runs, or in the flat profile.
void blendProbeRequest(const char* trigger);
// vScreenFrameBoundary: starts, advances and finishes the capture, and reads the answers back.
void blendProbeFrameBoundary(uint32_t gameFrame) noexcept;

// The draw path's gate, one relaxed load each. Wants: a capture is collecting draws (also true of the draw gate, so the verdict ladder
// runs and the AA-off fast path is not taken). Collecting is the same fact for the thread that owns the draws.
bool blendProbeWantsDraws() noexcept;

// What the draw path knows about one eye draw by the time its routing is decided (vscreen.cpp fills it).
struct BlendDrawInfo {
    char kind = '?';               // D, I, N or X
    uint32_t count = 0, instances = 1;
    uint32_t ordinal = 0;          // the draw's place among the frame's eye draws
    uint32_t gameFrame = 0;
    const char* verdict = "";      // EDVR's DrawVerdict name
    int uiFamily = 0;              // UiLayerFamily as an int, 0 none
    bool uiTaken = false;          // the layer took the draw
    bool uiDecided = false;        // a uiLayerDecide ran for this draw
    int uiDecision = 0;            // its UiLayerDecision as an int (meaningful when uiDecided)
    bool udCoverage = false;       // ui_depth chose a coverage shader for the draw
    bool udWantMask = false;       // ...and the reissue marks the reactive mask
    int udMaskSlot = 0;
    bool holoMatched = false;      // the generic hologram pass classified the draw
    bool holoWorld = false;        // ...from its world-marker list (else the cockpit list)
    bool engineSubstituted = false;  // engine-record velocity's pool pixel shaders are bound for this draw
};
// Re-issues the draw through the real entry point, below every EDVR hook (vscreen.cpp's pureDrawReissue).
using BlendReplayFn = void (*)(void* arg);
// Offer one eye draw of the collecting frame. Safe to call for any draw: it returns at once for a draw that is not a candidate.
void blendProbeDraw(ID3D11DeviceContext* ctx, const BlendDrawInfo& info, BlendReplayFn replay, void* arg) noexcept;
// An indirect draw (not replayable): counted, never tested.
void blendProbeNoteIndirect() noexcept;

// The temporal pass's side. Wanted: a capture is collecting and the current frame has no grid yet. The pass asks per eye per frame.
bool blendProbeGridWanted() noexcept;
struct BlendGridIn {
    ID3D11Texture2D* decision = nullptr;   // R32G32B32A32_FLOAT: motion xy, predicted depth, flags (capture-only trace variant)
    ID3D11Texture2D* depth = nullptr;      // R32_FLOAT: the pass's depth copy
    ID3D11Texture2D* coverage = nullptr;   // R8_UNORM: the UI coverage mask, null when nothing was marked
    ID3D11Texture2D* bias = nullptr;       // R8_UNORM: the bias mask, null when not built
    uint32_t w = 0, h = 0;                 // the textures' size (the pass's input)
    uint32_t sceneFrame = 0;               // the pass's own frame number
    float rotTrace = 3.0f;                 // dR0[0] + dR1[1] + dR2[2]: the pass's delta rotation since last frame
    float transLen = 0.0f;                 // |tvCam|: the camera rows' translation term
    bool transValid = false;               // tvCam.w: the world path is on
};
// Stages the window out of the pass's textures into staging copies (read back at the following frame boundaries). Call after the motion
// dispatch with the trace UAV unbound. Marks the frame's grid done whether or not it staged.
void blendProbeGridOffer(ID3D11DeviceContext* ctx, const BlendGridIn& in) noexcept;
// The pass could not produce a grid this frame (said once per capture, with the reason).
void blendProbeGridRefuse(const char* why) noexcept;

}  // namespace edvr
