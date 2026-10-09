// fix.ui_quality's flat half: the mono adapter over the VR UI layer (ui_layer.h), docs/design-flat-ui-quality-2026-10-05.md.
//
// WHAT. The flat 09:36 census (2026-10-09) found the smear: the cockpit HUD families -- the holo panels, the flight HUD,
// the target sprite, the holograms -- are drawn into the scene's HDR target H at the render size R, jittered, before
// EDVR's resolve, so they go through DLSS / TAA / FSR and their history. With fix.ui_quality on and the flat
// anti-aliasing on, each such draw is taken out of H by the VR layer's own take path, as eye 0: drawn by the game's own
// shaders into an HDR layer at the display's size D times the target (100: D, 125: 1.25 D, smoothed down by the
// composite), unjittered (the raster phase its camera rows carry is cancelled through the viewport), tested against a
// seeded copy of the game's depth; the game's tonemap draw is re-issued with that layer as its HDR source into an 8-bit
// layer (the game's own exposure and LUT); and at the game's output copy, after the resolve and the sharpening, the
// layer is composited over the picture the copy reads. VR's choice of size is followed: the layer is the size the door
// hands on (here the resolved picture, D) times the target, so 100 lands texel for texel and 125 is box-filtered down.
//
// THE DOOR. The layer arms frame N+1 only when frame N was resolved by EDVR and the copy read a D-sized picture
// (flatUiLayerDoorArms): a frame after a refusal leaves the HUD in H. A frame armed and then refused at the copy still
// composites its HUD, over the picture the game's copy reads (the game's own, at R): the HUD is never dropped and never
// drawn twice. The HDR route (treatHdr) needs nothing of its own: it resolves H before the tonemap, the HUD is out of H
// either way, and the composite runs at the same copy.
//
// FAILURE. Every refusal leaves the draw in the game's frame exactly as today, counted by reason on the 30 s "flat ui
// layer" lines; a failure of the layer's own machinery stands it down with the VR layer's own line ("ui quality: the
// layer stands down ..." / "the HDR HUD path stands down ..."). Resize and device loss release every layer reference
// (flatUiLayerRelease, from flatRuntimeResize): none of them is the back buffer, and the next door is a fresh one.
//
// THREADS. The flat runtime's owner thread only (its draw scope and its frame boundary). Every call into the shared layer
// runs under FlatComputeInternalScope, so the layer's own D3D work (the seed's and the coverage pass's deferred-context
// draws, its timer queries) is never observed by the flat runtime's hooks as the game's.
#pragma once

#include <cstdint>

struct ID3D11DeviceContext;
struct ID3D11ShaderResourceView;

namespace edvr {

// fix.ui_quality on, the flat anti-aliasing on, the flat profile, and the HDR HUD path not stood down. One load or two.
bool flatUiLayerOn();

// One draw the flat scope watched, as the adapter needs it.
struct FlatUiLayerDraw {
    uint64_t frame = 0;                // the flat frame being drawn (the layer's sequence)
    uint64_t vs = 0, ps = 0;
    const void* color = nullptr;       // its colour target (identity only)
    uint32_t width = 0, height = 0;    // its colour target
    uint32_t format = 0;
    bool hdrTarget = false;            // that target is the scene's HDR target (flat_ui_census.h's kHdr class)
    bool otherWork = false;            // the scope does something else with the draw (a capture, an overlay, a substitution)
    bool upstream = false;             // the camera injector owns the jitter
    bool haveRows = false;             // `rows` are the draw's camera rows
    float rows[6][4] = {};
    float phaseX = 0.0f, phaseY = 0.0f;  // the frame's raster phase, render pixels
};
enum class FlatUiLayerAsk : uint8_t { kNotAsked = 0, kDecided, kRefused };
// The draw's family is a cockpit HUD family: ask the shared decision (eye 0) with this draw's jitter. kDecided: bracket
// the game's issue with flatUiLayerBegin / flatUiLayerEnd, then call flatUiLayerReissue's write-back half.
FlatUiLayerAsk flatUiLayerDecide(ID3D11DeviceContext* ctx, const FlatUiLayerDraw& draw);
bool flatUiLayerBegin(ID3D11DeviceContext* ctx);   // false: the draw goes to the game's frame as always
void flatUiLayerEnd(ID3D11DeviceContext* ctx);
// Begin's answer for a decided draw, into the window's per-family counts (taken, or refused at issue).
void flatUiLayerNoteIssue(uint64_t vs, uint64_t ps, bool taken);
// A decided draw that writes depth or stencil: its colourless re-issue into the game's buffer (uiLayerWriteBackBegin/End).
bool flatUiLayerWriteBackBegin(ID3D11DeviceContext* ctx);
void flatUiLayerWriteBackEnd(ID3D11DeviceContext* ctx);

// The game's tone pass, by its known pair (flat_mono_frame.h toneHdrSlot), whatever its vertex count: `input` is what
// it reads at its HDR slot. Records the proof the next frame's takes need (flat_ui_layer_math.h FlatUiToneProof) and,
// when this frame's HUD is in the HDR layer, admits it for the re-issue (uiLayerCrispAdmitFlat). True: admitted.
// The first eight candidates of a session are each logged with their verdict.
bool flatUiLayerToneCandidate(ID3D11DeviceContext* ctx, uint64_t frame, uint32_t renderW, uint32_t renderH, int hdrSlot,
                              uint64_t vs, uint64_t ps, const void* input, uint32_t outW, uint32_t outH, bool hdrRoute);
// A plain copy (the game's copy pixel shader) from `source` into `output` this frame: when the source is the HUD's HDR
// target, the output is the copy the tone may read instead (the HDR route's post chain does).
void flatUiLayerNoteCopy(uint64_t frame, const void* source, const void* output);

// A full-screen triangle (3 vertices, 1 instance) that is no known tone pair: the game's tonemap, admitted for the re-issue when this frame's HUD
// is in the HDR layer (uiLayerCrispNoteEyeDraw). True: after the game's own issue, re-issue it between
// flatUiLayerToneBegin / flatUiLayerToneEnd.
bool flatUiLayerToneAdmit(ID3D11DeviceContext* ctx, uint64_t frame, uint32_t renderW, uint32_t renderH, char kind,
                          uint32_t count, uint32_t instances, uint32_t startInstance);
bool flatUiLayerToneBegin(ID3D11DeviceContext* ctx);
void flatUiLayerToneEnd(ID3D11DeviceContext* ctx);

// A game draw the layer did not take, while the layer watches this frame: a write of a seeded depth buffer.
void flatUiLayerNoteSceneDraw(ID3D11DeviceContext* ctx, uint32_t count, uint32_t instances, char kind);

// The game's output copy, right before its issue, after the resolve and the sharpening bound what it reads at t0. The
// door (arming the next frame) and the composite: this frame's HUD over that picture, bound at t0 in its place.
// `original` and `replaced` are the draw scope's own (the game's t0, put back after the draw).
void flatUiLayerAtCopy(ID3D11DeviceContext* ctx, uint64_t frame, bool treated, uint32_t outW, uint32_t outH,
                       ID3D11ShaderResourceView** original, bool* replaced);

// ResizeBuffers or a device change: every reference the layer holds, released.
void flatUiLayerRelease();

// The 30 s lines (from the census's window): "flat ui layer: ...", zeros included, every window of the flat profile.
void flatUiLayerReport(uint64_t windowSeconds);
// The layer's state for the census header: "live", or "off (why)".
const char* flatUiLayerState();

}  // namespace edvr
