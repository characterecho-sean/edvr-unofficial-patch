#pragma once
// Engine-record velocity, phase 1 (part of fix.temporal_aa; docs/kinematic-motion-
// injection-2026-09-19.md, 2026-09-23 "Phase 1 built" and "Fix round" entries).
//
// Exact per-pixel motion for the game's rigid movers from the engine's own
// kinematic records, with no estimation anywhere:
//
//  1. EMIT (job threads). FUN_144312E00 builds each kinematic rig record's
//     0x150-byte pool record -- byte-for-byte the t33 record the vertex shader
//     reads -- and appends it to the owner's node lists, from which the
//     engine's copier fills the mapped pool. A bracket on that call (the
//     direct-producer relay kinematic_eval_hook.cpp already installs) writes,
//     into each record the call just appended, the PREVIOUS frame's engine
//     pose of that same engine record (record+0x170 position, +0x17C packed
//     quaternion, as the previous frame's call copied them) into the record's
//     unread second pose block (t33 bytes 292-303 and 312-319), plus a marker
//     word at byte 288 -- joined only when that previous frame holds one
//     validated pose, masked otherwise (engine_velocity_emit.h has the rules).
//     The engine's own copy then carries it to the GPU: no slot map, no
//     matching, no readback of write-combined memory.
//  2. DRAW (render thread). The pool families' pixel shaders are substituted
//     (hash-keyed, dxbc_engine_velocity.h) so the game's own draws write, per
//     pixel, the t33 slot they drew (odd-coded) and the depth they wrote to
//     MRT6, under a derived blend state that never blends MRT6
//     (engine_velocity_state.h); the UV-only family's vertex shader also
//     exports its slot. Each eye-frame's pool and scene constants are copied
//     at its first substituted draw, and every later one is checked against
//     that copy: the same pool view and constant buffer bound, the pool not
//     rewritten, registers 270..275 of the constants unchanged. A mismatch
//     drops the eye-frame's engine data (counted); nothing stale is used.
//  3. COMPOSE (temporal pass, temporal_shader_source.h enginePixel). For a
//     pixel whose MRT6 depth equals the scene depth: the record's marker says
//     joined (exact motion from the two pose blocks and the game's own clip
//     rows, this frame's and last), masked (a rig record EDVR cannot follow:
//     no history), or neither (not a rig record: the camera term). A slot code
//     that is not odd and whole is declined.
//
// While the emit hook is not installed (or the build check fails) the whole
// feature STANDS DOWN: no substitution, every view request refused, and the
// log says so at configure and in every 30 s block.
#include <atomic>
#include <cstddef>
#include <cstdint>

#include "binding_shadow.h"

struct ID3D11Buffer;
struct ID3D11DeviceContext;
struct ID3D11PixelShader;
struct ID3D11Resource;
struct ID3D11ShaderResourceView;
struct ID3D11Texture2D;
struct ID3D11VertexShader;

namespace edvr {

// The pool families' bindings the snapshot comes from (VS t33, VS b1), and
// the compose's slots for this feature's inputs (temporal_shader_source.h:
// ES t21, EP t22, EN b1, EB b2; cs_stage_save.h covers all four).
constexpr unsigned kEngineVelocityPoolSlot = 33;
constexpr unsigned kEngineVelocitySceneSlot = 1;
constexpr unsigned kEngineVelocitySlotsSrv = 21;
constexpr unsigned kEngineVelocityPoolSrv = 22;
constexpr unsigned kEngineVelocitySceneNowCb = 1;
constexpr unsigned kEngineVelocityScenePrevCb = 2;

// The record, item and marker layout lives in engine_velocity_emit.h (the
// emit half) and the compose's ENGINE_MOTION_HLSL block; nothing here repeats it.

// On with the temporal pass live (fix.temporal_aa, every mode). Idempotent
// across the once-per-second config re-poll.
void engineVelocityConfigure(bool on);
void engineVelocityShutdown();
bool engineVelocityActive() noexcept;
// Engine motion's diagnostics wanted (temporal_pass: advanced.temporal_aa_
// diagnostics or an eye run): the emit's census runs only
// then (the 2026-09-23 performance review).
void engineVelocityDiagnostics(bool on);

// Shader creation (device_hook): the keyed pool families' bytecode is kept
// whether or not the feature is on (ten small shaders), so enabling it live
// needs no restart.
void engineVelocityRememberVs(ID3D11VertexShader*, uint64_t hash, const void* bytecode, size_t bytes,
                              bool linked);
void engineVelocityRememberPs(ID3D11PixelShader*, uint64_t hash, const void* bytecode, size_t bytes,
                              bool linked);

// The per-draw hook (vscreen's draw hooks, owner context, before the real
// draw). The inline half is a handful of compares: the slow half runs only
// when the game has rebound a shader, a render target, the depth target or
// the blend state, bound another pool view or scene constant buffer, or
// written a watched source, since the last look.
namespace engine_velocity_detail {
struct DrawCache {
    uint32_t vs = 0, ps = 0, rtv = 0, dsv = 0, blend = 0;
    const void* pool = nullptr;    // VS t33's view as the shadow last saw it
    const void* scene = nullptr;   // VS b1's buffer, likewise
    bool eye = false;
    int family = -1;   // the family whose substituted shaders are bound, -1 none
};
constexpr int kMaxFamilies = 10;
extern std::atomic<bool> live;
extern DrawCache cache;                        // owner thread only
extern uint64_t familyDraws[kMaxFamilies];     // owner thread only: draws that ran substituted
// The resources an open eye-frame's snapshot came from (eye0 pool, eye0
// scene, eye1 pool, eye1 scene, then the on-foot source's pool and scene).
// Identities only, never dereferenced here.
constexpr unsigned kWatchSlots = 6;
extern std::atomic<const ID3D11Resource*> watch[kWatchSlots];
void beforeDrawSlow(ID3D11DeviceContext*, bool rtv0Eye);
void noteResourceMapped(const ID3D11Resource*, void* data, int mapType) noexcept;
void noteResourceWrite(const ID3D11Resource*) noexcept;
inline bool watchesResource(const ID3D11Resource* resource) noexcept {
    if (!resource) return false;
    for (const auto& w : watch) if (w.load(std::memory_order_relaxed) == resource) return true;
    return false;
}
}  // namespace engine_velocity_detail

inline void engineVelocityBeforeDraw(ID3D11DeviceContext* ctx, bool rtv0Eye) {
    using namespace engine_velocity_detail;
    if (!live.load(std::memory_order_relaxed)) return;
    if (cache.vs != bindingGeneration(BindSlot::Vs) || cache.ps != bindingGeneration(BindSlot::Ps) ||
        cache.rtv != bindingGeneration(BindSlot::Rtv0) || cache.dsv != bindingGeneration(BindSlot::Dsv0) ||
        cache.blend != bindingGeneration(BindSlot::Blend) || cache.pool != bindingGet(BindSlot::VsSrv33) ||
        cache.scene != bindingGet(BindSlot::VsCb1) || cache.eye != rtv0Eye)
        beforeDrawSlow(ctx, rtv0Eye);
    if (cache.family >= 0) ++familyDraws[cache.family];
}

// The Map tee (vscreen's hookedMap, owner context, after the real Map): the
// mapped pointer and the map type of a watched source, for the write tee.
inline void engineVelocityResourceMapped(const ID3D11Resource* resource, void* data, int mapType) {
    using namespace engine_velocity_detail;
    if (live.load(std::memory_order_relaxed) && watchesResource(resource)) noteResourceMapped(resource, data, mapType);
}

// The Unmap/Copy/Update tees (vscreen, owner context; an Unmap before the
// real one): a write to a watched source. The scene constants' registers
// 270..275 are read back from the mapped memory and compared; a pool write
// is an append (NO_OVERWRITE) or a replacement. The next pool draw then takes
// the slow half, which keeps, refreshes or drops the eye-frame's snapshot.
inline void engineVelocityResourceWritten(const ID3D11Resource* resource) {
    using namespace engine_velocity_detail;
    if (live.load(std::memory_order_relaxed) && watchesResource(resource)) noteResourceWrite(resource);
}

// The present-frame clock (device_hook, once per owned Present).
void engineVelocityNotePresentFrame(uint32_t presentFrame) noexcept;
// The owner thread's frame boundary (vscreen): rotation, the periodic lines.
void engineVelocityFrameBoundary(ID3D11DeviceContext*);
// Flat draw bracket: restore substituted shaders/blend; caller restores MRTs.
void engineVelocityAfterFlatDraw(ID3D11DeviceContext*);
// Owner-thread diagnostic, sampled after BeforeDraw and before restoring the
// draw bracket. A source candidate alone does not prove substitution succeeded.
inline bool engineVelocityDrawSubstituted() noexcept {
    return engine_velocity_detail::cache.family >= 0;
}

// The temporal pass's inputs for one eye this frame, AddRef'd: the slot
// target (MRT6, the scene depth's size), the pool snapshot the eye's draws
// read, and the game's scene constants for this frame and the previous one
// (registers 270..275 are read). False, with every pointer null, when the eye
// has no complete engine data this frame -- the compose then keeps the camera
// term everywhere.
struct EngineVelocityViews {
    ID3D11ShaderResourceView* slots = nullptr;
    ID3D11ShaderResourceView* pool = nullptr;
    ID3D11Buffer* sceneNow = nullptr;
    ID3D11Buffer* scenePrev = nullptr;
};
bool engineVelocityViews(ID3D11DeviceContext*, int eye, ID3D11Texture2D* sceneDepth, EngineVelocityViews* out);
// The eye-pass capture's GPU time since the last take (the performance
// review, item 5): the slot target clear, the snapshots at preparation and
// the pool refreshes, each a GpuTimer interval polled without waiting. Per
// event median/p95 ms; untimed (no timer free or no lease) and invalid
// (disjoint, expired) counted. The temporal pass prints it beside prep's
// parts and takes it once per price window. False: nothing this window.
struct EngineVelocityCaptureGpu {
    uint32_t events[3] = {};            // clear, snapshots, refreshes
    double medianMs[3] = {}, p95Ms[3] = {};
    uint64_t untimed = 0, invalid = 0;
};
bool engineVelocityTakeCaptureGpu(EngineVelocityCaptureGpu* out);
// One eye's compose pixel counts, read back by the temporal pass (Stats
// 50..54): engine-joined, masked, pool-but-not-a-rig-record, stale slot,
// corrupt slot code.
void engineVelocityNotePixels(uint32_t joined, uint32_t masked, uint32_t camera, uint32_t stale, uint32_t corrupt);
// On foot (docs/kinematic-motion-injection-2026-09-19.md, 2026-09-23 "On
// foot"): the world is drawn into a flat SOURCE image that the 2D screen
// shows in each eye, and no pool draw targets an eye. screen_motion names the
// source's depth once per source frame; a pool family draw into that depth is
// the source pass, which gets MRT6 like an eye pass, into a slot target of the
// source's own size -- made only while the source is drawn, released when it
// has not been for kSourceIdleFrames present frames. The views and their
// refusals follow engineVelocityViews; the screen shader (screen_motion.h)
// consumes them and hands back its per-eye-draw pixel counts.
//
// The naming also says which camera the source's engine data is for: the
// scene constants the naming draw reads (screen_motion's terrain/scene draw,
// whose camera its camera term uses). The source is drawn by more than one
// camera (flight 5, 2026-09-23 140351: a few pool draws under rows 270..275
// that equal the world's standing still and differ walking dropped every
// walking frame under the eyes' first-draw rule), so each source pool draw
// is held to the naming's camera and a draw that is not -- before this
// frame's naming, other scene constants, rows unseen, other rows -- is
// declined, not substituted, without dropping the frame.
constexpr int kEngineVelocitySourceEye = 2;
constexpr uint32_t kSourceIdleFrames = 120;
// Which signal named the source (flight 6, the hangar): a terrain or scene
// draw (settlements), or -- where there is neither -- the pool family draw
// into the screen-sized depth that took the most of them last frame.
enum class EngineVelocitySourceSignal { Terrain = 0, ScreenDepth = 1 };
void engineVelocityNoteSource(ID3D11Texture2D* sourceDepth, ID3D11Buffer* sceneConstants,
                              EngineVelocitySourceSignal signal = EngineVelocitySourceSignal::Terrain);
// Is this vertex shader one of the pool families (the naming without
// terrain counts their draws)? Pure: the family table, no state.
bool engineVelocityPoolFamilyVs(uint64_t vsHash) noexcept;
// Pure declaration lookup only; does not configure or run the producer and
// does not assert that the runtime shader patch or motion views are ready.
bool engineVelocityPoolFamilyPair(uint64_t vsHash, uint64_t psHash) noexcept;
bool engineVelocitySourceViews(ID3D11Texture2D* sourceDepth, EngineVelocityViews* out);
// The screen shader's panel counts without diagnostics: one present frame in
// kPanelSampleFrames, one eye pixel in kPanelSampleStride squared (a grid on
// the eye pixel), raw; pixelStride 1 = every pixel (diagnostics, motion_source).
constexpr uint32_t kPanelSampleFrames = 300, kPanelSampleStride = 4;
void engineVelocityNotePanelPixels(uint32_t joined, uint32_t masked, uint32_t camera, uint32_t stale, uint32_t corrupt,
                                   uint32_t eyeDraws, uint32_t pixelStride);

}  // namespace edvr
