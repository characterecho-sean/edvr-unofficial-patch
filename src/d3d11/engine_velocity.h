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
//     Primary rigid 42B4130 emissions use the same canonical history through
//     an emission sink: native records stay unchanged. Verified CPU list
//     relocation/upload joins certify only EDVR's private clone for patching.
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
#include <type_traits>

#include "binding_shadow.h"
#include "engine_velocity_families.h"
#include "../common/plugin_cost.h"

struct ID3D11Buffer;
struct ID3D11DeviceContext;
struct ID3D11PixelShader;
struct ID3D11Resource;
struct ID3D11ShaderResourceView;
struct ID3D11Texture2D;
struct ID3D11VertexShader;
struct D3D11_BUFFER_DESC;

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
// The flat census of unkeyed pairs (engine_velocity_unkeyed.h): writes the 5 s
// window's line into `out` and starts the next window. The line is written even
// when the window held nothing or the feature is off (live=0): its absence in a
// log means the flat 5 s block never ran, not that nothing was unkeyed. Returns
// the length written (snprintf's convention).
int engineVelocityFormatUnkeyed(char* out, size_t size);

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
extern uint64_t g_stateCalls;                  // owner thread only: the draw wrapper's D3D context calls (see below)
// The resources an open eye-frame's snapshot came from (eye0 pool, eye0
// scene, eye1 pool, eye1 scene, then the on-foot source's pool and scene).
// Identities only, never dereferenced here.
constexpr unsigned kWatchSlots = 6;
extern std::atomic<const ID3D11Resource*> watch[kWatchSlots];
void beforeDrawSlow(ID3D11DeviceContext*, bool rtv0Eye);
void beforeDrawSlowSampledApi(ID3D11DeviceContext*, bool rtv0Eye);
extern uint32_t g_psShadowProbeN;   // owner thread; the header's sampling counter
// Sampled backstop for a genuinely unobserved game bind. The seam arc's
// original zero-bind/live-census evidence was our own generated shaders
// (exact production-patcher hash proof, 2026-09-28; see the family header).
// The probe must exclude the installed EDVR patch before considering a live
// shader absent from the shadow; otherwise it adopts our substitution as
// game state and destroys restoration ownership.
void psShadowProbe(ID3D11DeviceContext*);
void psShadowProbeSampledApi(ID3D11DeviceContext*);
// NoApi's original probe gate keeps these tiny pure lookups at the callsite.
// The sampled/shared family lookups retain their existing implementation.
__forceinline int familyOfVsNoApiInline(uint64_t hash) noexcept {
    using namespace engine_velocity_family;
    for (int i = 0; i < kFamilyCount; ++i) if (kFamilies[i].vs == hash) return i;
    return -1;
}
__forceinline bool anyFamilyPsNoApiInline(uint64_t ps) noexcept {
    using namespace engine_velocity_family;
    for (const Family& f : kFamilies)
        for (uint64_t h : f.ps)
            if (h == ps) return true;
    for (const SelfMarking& p : kSelfMarking)
        if (p.ps == ps) return true;
    return false;
}
void noteResourceMapped(const ID3D11Resource*, void* data, int mapType) noexcept;
void noteResourceWrite(const ID3D11Resource*) noexcept;
constexpr unsigned kPrimaryPoolResources=16;
extern std::atomic<const ID3D11Resource*> primaryPoolResources[kPrimaryPoolResources];
void notePrimaryBufferCreated(ID3D11Buffer*,const D3D11_BUFFER_DESC&) noexcept;
void notePrimaryResourceMapped(const ID3D11Resource*,void*,int) noexcept;
void notePrimaryResourceWritten(const ID3D11Resource*) noexcept;
void notePrimaryResourceUnknown(const ID3D11Resource*) noexcept;
inline bool watchesPrimaryResource(const ID3D11Resource* resource) noexcept {
    if(!resource)return false;
    for(const auto& slot:primaryPoolResources)if(slot.load(std::memory_order_relaxed)==resource)return true;
    return false;
}
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
        cache.scene != bindingGet(BindSlot::VsCb1) || cache.eye != rtv0Eye) {
        beforeDrawSlow(ctx, rtv0Eye);
    } else if (((++g_psShadowProbeN) & 63u) == 0u &&
               (familyOfVsNoApiInline(bindingShaderHash(BindSlot::Vs)) >= 0 ||
                anyFamilyPsNoApiInline(bindingShaderHash(BindSlot::Ps)))) {
        // One pool-context draw in 64 checks for an unobserved game bind.
        // A live shader different from the shadow is normally our installed
        // substitution, so the probe first excludes its owned identity. The
        // 162120 capture's live/shadow differences all reproduce EDVR patch
        // hashes; they are not evidence of a real hook bypass. A different
        // registered game shader heals the shadow and takes the slow half.
        psShadowProbe(ctx);
    }
    if (cache.family >= 0) ++familyDraws[cache.family];
}

template <class ApiPolicy>
inline void engineVelocityBeforeDrawWithApi(ID3D11DeviceContext* ctx, bool rtv0Eye) {
    using namespace engine_velocity_detail;
    if (!live.load(std::memory_order_relaxed)) return;
    if (cache.vs != bindingGeneration(BindSlot::Vs) || cache.ps != bindingGeneration(BindSlot::Ps) ||
        cache.rtv != bindingGeneration(BindSlot::Rtv0) || cache.dsv != bindingGeneration(BindSlot::Dsv0) ||
        cache.blend != bindingGeneration(BindSlot::Blend) || cache.pool != bindingGet(BindSlot::VsSrv33) ||
        cache.scene != bindingGet(BindSlot::VsCb1) || cache.eye != rtv0Eye) {
        beforeDrawSlowSampledApi(ctx, rtv0Eye);
    } else if (((++g_psShadowProbeN) & 63u) == 0u &&
               (engine_velocity_family::familyOfVs(bindingShaderHash(BindSlot::Vs)) >= 0 ||
                engine_velocity_family::anyFamilyPs(bindingShaderHash(BindSlot::Ps)))) {
        psShadowProbeSampledApi(ctx);
    }
    if (cache.family >= 0) ++familyDraws[cache.family];
}

// Preserve the original unsampled gate and call target. Only this tiny
// compile-time adapter is forced inline; the sampled gate remains separate.
template <>
__forceinline void engineVelocityBeforeDrawWithApi<plugin_cost::NoApi>(
    ID3D11DeviceContext* ctx, bool rtv0Eye) {
    engineVelocityBeforeDraw(ctx, rtv0Eye);
}

// Auto and indirect hooks do not enter the common draw chooser. Their API
// policy is selected only after the same cheap live/cache/probe gates show
// that EngineVelocity will query or change target/blend state.
inline void engineVelocityBeforeDrawSampledBoundary(ID3D11DeviceContext* ctx,
                                                     bool rtv0Eye) {
    using namespace engine_velocity_detail;
    if (!live.load(std::memory_order_relaxed)) return;
    const bool stale = cache.vs != bindingGeneration(BindSlot::Vs) ||
        cache.ps != bindingGeneration(BindSlot::Ps) ||
        cache.rtv != bindingGeneration(BindSlot::Rtv0) ||
        cache.dsv != bindingGeneration(BindSlot::Dsv0) ||
        cache.blend != bindingGeneration(BindSlot::Blend) ||
        cache.pool != bindingGet(BindSlot::VsSrv33) ||
        cache.scene != bindingGet(BindSlot::VsCb1) || cache.eye != rtv0Eye;
    if (stale) {
        if (plugin_cost::apiSampleHint() &&
            edvrPluginCostApiSampleContext(ctx) != 0)
            beforeDrawSlowSampledApi(ctx, rtv0Eye);
        else
            beforeDrawSlow(ctx, rtv0Eye);
    } else if (((++g_psShadowProbeN) & 63u) == 0u &&
               (engine_velocity_family::familyOfVs(bindingShaderHash(BindSlot::Vs)) >= 0 ||
                engine_velocity_family::anyFamilyPs(bindingShaderHash(BindSlot::Ps)))) {
        if (plugin_cost::apiSampleHint() &&
            edvrPluginCostApiSampleContext(ctx) != 0)
            psShadowProbeSampledApi(ctx);
        else
            psShadowProbe(ctx);
    }
    if (cache.family >= 0) ++familyDraws[cache.family];
}

// The Map tee (vscreen's hookedMap, owner context, after the real Map): the
// mapped pointer and the map type of a watched source, for the write tee.
inline void engineVelocityResourceMapped(const ID3D11Resource* resource, void* data, int mapType) {
    using namespace engine_velocity_detail;
    if (!live.load(std::memory_order_relaxed))return;
    if(watchesPrimaryResource(resource))notePrimaryResourceMapped(resource,data,mapType);
    if(watchesResource(resource))noteResourceMapped(resource,data,mapType);
}

// The Unmap/Copy/Update tees (vscreen, owner context; an Unmap before the
// real one): a write to a watched source. The scene constants' registers
// 270..275 are read back from the mapped memory and compared; a pool write
// is an append (NO_OVERWRITE) or a replacement. The next pool draw then takes
// the slow half, which keeps, refreshes or drops the eye-frame's snapshot.
inline void engineVelocityResourceWritten(const ID3D11Resource* resource) {
    using namespace engine_velocity_detail;
    if (!live.load(std::memory_order_relaxed))return;
    if(!resource || watchesPrimaryResource(resource))notePrimaryResourceWritten(resource);
    if(watchesResource(resource))noteResourceWrite(resource);
}

// A buffer the game created (device_hook, every successful CreateBuffer). Only
// while the feature is live: a registered pool is held by reference for the
// private-copy cache, so with the feature off -- the default -- nothing is
// registered and no game buffer is kept alive. A pool created before the
// feature went live is nominated from the t33 binding itself at the first
// snapshot (engine_velocity.cpp, "existing buffer on mid-session activation").
inline void engineVelocityBufferCreated(ID3D11Buffer* buffer,const D3D11_BUFFER_DESC* desc) {
    if(!engine_velocity_detail::live.load(std::memory_order_relaxed))return;
    if(buffer && desc)
        engine_velocity_detail::notePrimaryBufferCreated(buffer,*desc);
}
inline void engineVelocityResourceUnknown(const ID3D11Resource* resource) {
    using namespace engine_velocity_detail;
    if(live.load(std::memory_order_relaxed) && (!resource || watchesPrimaryResource(resource)))notePrimaryResourceUnknown(resource);
}

// The present-frame clock (device_hook, once per owned Present).
void engineVelocityNotePresentFrame(uint32_t presentFrame) noexcept;
// The owner thread's frame boundary (vscreen): rotation, the periodic lines.
void engineVelocityFrameBoundary(ID3D11DeviceContext*);
// The flat runtime's per-draw restore before the lazy form below: substituted shaders/blend back, the draw cache reset,
// and the caller put the game's render targets back. The runtime no longer calls it (engineVelocityFlatEndDraw and
// engineVelocityFlatFlush do this, and the targets too); the lifecycle rig drives it as its model of a draw that restores
// after itself.
void engineVelocityAfterFlatDraw(ID3D11DeviceContext*);

// ---- The flat draw bracket, lazy form (2026-09-30) ----------------------------------------------------------
// The flat runtime put the game's state back after EVERY substituted producer draw, and bound EDVR's again for the
// next: the game's eight render targets read, MRT6 added (read again, set, read to verify), the blend state read and
// set, the patched pixel shader set, then the shader, the blend and the targets restored -- eleven immediate-context
// calls a draw at least, 13,549 a frame over 1,130 draws on foot (flight 053745), each of them a driver round trip,
// and, under a graphics wrapper (ReShade), each one dearer. It also reset the draw cache, so every next draw took the
// slow half and its two mutex acquisitions.
//
// Now EDVR's state (MRT6, the derived blend, the patched shaders) stays bound across consecutive substituted producer
// draws, as VR's always has, and the game's is put back ONCE, before anything that could observe or depend on it.
// What that is, is flat_substitution.h's policy, called from the hooks: every hooked draw, dispatch, clear, copy,
// resolve and command-list execution that is not a substituted producer draw, and the Present. A game setter of the
// same state simply replaces EDVR's (the binding shadow's generations say so, as they always did: restore puts back
// only what is still ours). A game Get* of that state is NOT seen by the hook layer -- no Get is hooked -- so it would
// read EDVR's, exactly as it can under VR, where the state has always stayed bound between draws; that exposure is
// the whole of what the lazy form adds, and it is why every hooked call above restores instead of guessing.
//
//   engineVelocityFlatBeginDraw: before a candidate producer draw. The game's render targets are saved once per
//     binding (a read only when the game has rebound them), BeforeDraw runs, and a draw it declines puts the game's
//     state fully back at once. True: EDVR's state is bound for this draw. *gameHadTarget6: the game's own slot 6
//     was occupied.
//   engineVelocityFlatEndDraw: after the draw. Lazy: nothing is restored. Eager (a diagnostic capture is armed, or the
//     overlay guard's private t3 is bound): the game's state goes back now, as it always did.
//   engineVelocityFlatFlush: the game's state back where EDVR's is still bound, and the bookkeeping reset. A no-op
//     when nothing is bound.
//   engineVelocityFlatAbandon: the context lost its state (ClearState, a resize): forget, without touching it.
//   engineVelocityFlatLazy: the runtime's switch, owner thread, per draw.
enum class EngineVelocityFlushCause : unsigned {
    kDeclined = 0,   // a candidate producer draw the slow half declined
    kOtherDraw,      // a draw that is not a substituted producer draw
    kDispatch,
    kClear,
    kCopy,
    kResolve,
    kKeepTargets,    // the game keeps its render targets and sets UAVs beside them
    kCommandList,
    kPresent,
    kOverlay,        // an overlay-guard draw's base snapshot needs the game's state bound
    kEager,          // not lazy: a diagnostic capture is armed, or the overlay guard's t3 is bound
    kCount
};
namespace engine_velocity_detail {
extern std::atomic<bool> g_flatPending;   // EDVR's state may be bound over the game's
}
inline bool engineVelocityFlatPending() noexcept {
    return engine_velocity_detail::g_flatPending.load(std::memory_order_relaxed);
}
// Flat-only explicit-domain marker bracket. It never renames the primary
// world camera or snapshots foreign scene/pool buffers as world inputs.
// The caller certifies camera domain and final-writer provenance separately.
enum class FlatEngineDomain { ForeignPool, WorldPool, World };
bool engineVelocityFlatDomainBeginDraw(ID3D11DeviceContext*, ID3D11Texture2D* depth,
    const void* vsBytes, size_t vsSize, const void* psBytes, size_t psSize,
    FlatEngineDomain domain, const char** reason, bool coverage=false,
    unsigned writerToken=0,unsigned primitiveCount=0);
void engineVelocityFlatDomainEndDraw(ID3D11DeviceContext*);
bool engineVelocityFlatDomainSlots(ID3D11Texture2D* depth, ID3D11ShaderResourceView** out);

bool engineVelocityFlatBeginDraw(ID3D11DeviceContext* ctx, bool* gameHadTarget6);
void engineVelocityFlatEndDraw(ID3D11DeviceContext* ctx);
void engineVelocityFlatFlush(ID3D11DeviceContext* ctx, EngineVelocityFlushCause cause);
// The flat runtime calls this only for a pending kOtherDraw restore after its
// owner-thread and exact owner-context gates. Other flush causes keep the
// original NoApi entry above.
void engineVelocityFlatFlushOtherDrawSampledBoundary(ID3D11DeviceContext* ctx);
void engineVelocityFlatAbandon() noexcept;
// The frame ends, after the Present's flush: what the bracket kept for it (the game's render-target set, its blend state, the
// accepted binding; flat_query_cut.h) is released. A no-op while EDVR's state is still bound: the flush is owed first.
void engineVelocityFlatFrameEnd() noexcept;
void engineVelocityFlatLazy(bool on) noexcept;
// The flat CPU census (flat_cpu.h): what engine motion's draw wrapper asks of the D3D
// immediate context. Every Get and Set on it, and the clears and copies an eye-frame's
// preparation makes, are counted at their call sites, on the owner thread (the caller of
// the flat draw scope); the flat Present drains the count once a frame with the draws the
// wrapper substituted. A count, not a clock: it prices nothing by itself, it says how many
// driver round trips the wrapper is.
inline void engineVelocityNoteStateCalls(unsigned n) noexcept { engine_velocity_detail::g_stateCalls += n; }
struct EngineVelocityWrapperCounts {
    uint64_t stateCalls = 0;
    uint64_t substitutedDraws = 0;
};
EngineVelocityWrapperCounts engineVelocityTakeWrapperCounts() noexcept;
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
// term everywhere. gameMark, when set, is the game's OWN target-6 texture,
// captured this eye-frame at a self-marking pair's draw (kSelfMarking in
// engine_velocity_families.h): detail shaders that natively write the marker
// encoding there. The compose reads it as a fallback beside the slot target.
struct EngineVelocityViews {
    ID3D11ShaderResourceView* slots = nullptr;
    ID3D11ShaderResourceView* pool = nullptr;
    ID3D11Buffer* sceneNow = nullptr;
    ID3D11Buffer* scenePrev = nullptr;
    ID3D11ShaderResourceView* gameMark = nullptr;
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
// 50..55): engine-joined, masked, pool-but-not-a-rig-record, stale slot,
// corrupt slot code, stale stamp (a joined marker from an older frame: the
// camera term).
void engineVelocityNotePixels(uint32_t joined, uint32_t masked, uint32_t camera, uint32_t stale, uint32_t corrupt,
                              uint32_t stamped);
// vscreen's PS hook calls this with the bound shader's content hash: counts
// binds of the self-marking detail shaders, so the draw-path census can tell
// "never bound through the hook" from "bound but never drawn through it".
void engineVelocityNoteSelfMarkingPs(uint64_t psHash) noexcept;
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
// The VR world route's two reads of the source's naming (vr_world_route.cpp; docs section 82): is `depth` the source depth
// screen_motion named in THIS present frame, and the source camera's rows 270..275 (the resolver's camera[6][4]: b1's first
// 96 bytes from row 270) as the watch last saw them written for that naming. False when nothing was named this frame, the
// depth is another, or the rows were not seen. Pure reads under the module's own lock: no GPU work, no allocation.
bool engineVelocitySourceIsNamed(const ID3D11Texture2D* depth);
bool engineVelocitySourceCameraRows(float (&rows)[6][4]);
// The screen shader's panel counts without diagnostics: one present frame in
// kPanelSampleFrames, one eye pixel in kPanelSampleStride squared (a grid on
// the eye pixel), raw; pixelStride 1 = every pixel (diagnostics, motion_source).
constexpr uint32_t kPanelSampleFrames = 300, kPanelSampleStride = 4;
void engineVelocityNotePanelPixels(uint32_t joined, uint32_t masked, uint32_t camera, uint32_t stale, uint32_t corrupt,
                                   uint32_t stamped, uint32_t eyeDraws, uint32_t pixelStride);

}  // namespace edvr
