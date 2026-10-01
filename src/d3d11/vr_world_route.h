// The VR on-foot world route, the runtime surface (docs/design-flat-temporal-aa-2026-09-23.md, section 82).
// The pure half is vr_world_route_math.h; this file is what the rest of the VR pipeline calls.
//
// THE SHAPE OF THE ROUTE. On foot Elite draws the world once, flat, into the 2D screen's target and shows the screen to
// each eye with one composite draw. With experimental.temporal_aa_on_foot_world = auto the route (1) watches the game's
// draws for the flat HDR route's trigger, the tone (vrWorldRouteDraw), and resolves the HDR scene image H there with
// flatMonoResolve, into H itself, on its own upscaler slot; (2) once it has treated kVrWorldWarmFrames frames in a row
// OWNS the world: the eye shift is off (native_temporal begin), and on every frame it treats the UI layer re-issues each
// eye's screen draw with the resolved screen, mipped, into the layer, and the door runs layer-only for that eye (no eye
// upscaler, motion prep or UI resolve); (3) releases the world on any frame it cannot treat for long enough, on the gate,
// the layer, a scene reset or the late-write latch, and the eye route serves the eyes exactly as it does without it.
//
// THREADS. Everything below runs on the game's render thread except vrWorldRouteOwnsNextFrame (the XR owner thread's
// begin(), lock-free) and the frame boundary's publication of it.
//
// KEY OFF. g_vrWorldWants stays false, the hooks do nothing, the state is Off, the eye shift is never suppressed, the layer
// is never told the world is the route's and the door never runs layer-only. tools\vr_world_route_test pins each.
#pragma once
#include "flat_compute_readback.h"
#include <atomic>
#include <cstdint>

struct ID3D11DeviceContext;
struct ID3D11ShaderResourceView;
struct ID3D11Texture2D;

namespace edvr {

// Defined in vr_world_route_math.h (the pure half), declared opaque here so that the modules that only ask the route a
// question (native_temporal, the layer, the hooks) do not pull the detector's headers in.
enum class VrWorldState : uint8_t;

// The upscaler feature slot of the world. The eyes own 0 and 1 (dlaa.cpp, fsr3_engine.cpp); the flat runtime uses 0.
constexpr uint32_t kVrWorldFeatureSlot = 2;

// ---- the per-draw surface (render thread) ------------------------------------------------------------------------
// True while the route has work for the draw hooks: the key is auto, the UI layer is live and the on-foot gate holds. One
// plain load per game draw when it is false, and nothing else the hooks do changes.
extern bool g_vrWorldWants;
inline bool vrWorldRouteWantsDraws() { return g_vrWorldWants; }
// One call per game draw on the owner context while wanted, before the game's draw is issued: the detector, the trigger,
// and, at the tone, the resolve. Never throws; every failure is a counted decline and the eye route serves the frame.
void vrWorldRouteDraw(ID3D11DeviceContext* ctx);

// True from the trigger to the frame's end while the route watches (a treated or refused frame alike): the copy, update,
// clear and dispatch hooks then tell the route what they write, so a write into H after the resolve is counted (the latch).
extern bool g_vrWorldWatchWrites;
void vrWorldRouteNoteWrite(const void* resource);   // a copy or update wrote `resource`
void vrWorldRouteNoteRtvClear(void* rtv);           // ClearRenderTargetView on `rtv`
void vrWorldRouteNoteDispatch();                    // a dispatch ran: its UAVs (the shadow's CsUav0..3) are writes

// The route's own D3D calls (the resolver's dispatches and draw, the upscaler SDK's, the mip copy) go through the hooked
// vtable. Every state hook steps aside for g_flatComputeInternal; the draw and dispatch thunks step aside for this flag
// (set only by the route, so with the key off it is never true), or their verdicts, the exposure fix and the detector
// would see the route's own work as the game's.
extern thread_local bool g_vrWorldInternal;
struct VrWorldInternalScope {
    bool previousWorld = g_vrWorldInternal;
    FlatComputeInternalScope compute;
    VrWorldInternalScope() noexcept { g_vrWorldInternal = true; }
    ~VrWorldInternalScope() { g_vrWorldInternal = previousWorld; }
    VrWorldInternalScope(const VrWorldInternalScope&) = delete;
    VrWorldInternalScope& operator=(const VrWorldInternalScope&) = delete;
};

// ---- frame boundary (render thread) -------------------------------------------------------------------------------
// Once a frame at the Present boundary, AFTER the UI layer's own boundary (it reads the gate that boundary computed):
// accounts the frame that ended, steps the ownership machine, reads the key, prints the 5 s line, publishes the state the
// next frame starts in and arms the detector for it.
void vrWorldRouteFrameBoundary();

// ---- what the rest of the pipeline asks -----------------------------------------------------------------------------
// The key is auto (as read at the last boundary). Render thread.
bool vrWorldRouteEnabled();
// The state the last boundary left. Render thread.
VrWorldState vrWorldRouteState();
// The eye shift stays off for the frame about to begin: the last boundary left the route Owned. LOCK-FREE and callable
// from any thread: native_temporal's begin() runs on the XR owner thread.
bool vrWorldRouteOwnsNextFrame();
// The route resolved THIS frame's world at the tone (its H write-back is in the frame). Render thread.
bool vrWorldRouteTreatedThisFrame();
// The UI layer may take the screen draw: Owned, and this frame treated. Render thread.
bool vrWorldRouteLayerMayTake();
// The layer re-issued eye `eye`'s screen draw into its layer for frame `sequence`. The door reads it back.
void vrWorldRouteNoteEyeTaken(uint32_t eye, uint64_t sequence);
// The door runs layer-only for this eye in this sequence (the layer took its screen draw).
bool vrWorldRouteDoorLayerOnly(uint32_t eye, uint64_t sequence);
// The transition detector reset the eyes' history (boarding, disembarking, a jump, a withheld frame): the route lets go
// and restarts its warm-up, and the resolver's history resets with it.
void vrWorldRouteNoteSceneReset();
// Progress through the current frame's draws, for the camera census (camera_census): the number of game draws the route
// has seen this frame and whether the detector has already seen the tone. False when the route does not watch draws.
bool vrWorldRouteDrawProgress(uint32_t* drawOrdinal, bool* toneSeen, uint64_t* frame);
// STAGE 2 (the camera injector, flat_camera_inject.h "THE VR WORLD ROUTE'S INJECTION MODE"). The raster phase the route
// asked the injector to put into this frame's world cameras, in render pixels (positive right/down), and whether the
// route is jittering this frame at all (injection wanted: the route key is auto, the state is Warming or Owned, the world
// jitter key is on, the hook is live). False, with *x = *y = 0, when it is not. The value is the frame's CHOICE: it is zero
// for the first frames of a warm-up even while jittering, and the census reads it to sample frames whose phase is non-zero,
// the ones that can show a leak into the eyes.
bool vrWorldRouteWorldPhase(float* x, float* y);

}  // namespace edvr
