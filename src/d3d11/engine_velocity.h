#pragma once
// Engine-record velocity, phase 1 (fix.engine_motion=on; docs/kinematic-motion-
// injection-2026-09-19.md, 2026-09-23 "Phase 1 built" entry).
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
//     word at byte 288. The engine's own copy then carries it to the GPU:
//     no slot map, no matching, no readback of write-combined memory.
//  2. DRAW (render thread). The pool families' pixel shaders are substituted
//     (hash-keyed, dxbc_engine_velocity.h) so the game's own draws write, per
//     pixel, the t33 slot they drew and the depth they wrote to MRT6; the
//     UV-only family's vertex shader also exports its slot.
//  3. COMPOSE (temporal pass, temporal_shader_source.h enginePixel). For a
//     pixel whose MRT6 depth equals the scene depth: the record's marker says
//     joined (exact motion from the two pose blocks and the game's own clip
//     rows, this frame's and last), masked (a rig record EDVR cannot follow:
//     no history), or neither (not a rig record: the camera term).
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

// The record, item and marker layout lives in engine_velocity_emit.h (the
// emit half) and the compose's ENGINE_MOTION_HLSL block; nothing here repeats it.

// fix.engine_motion=on with the temporal pass live. Idempotent across the
// once-per-second config re-poll.
void engineVelocityConfigure(bool on);
void engineVelocityShutdown();
bool engineVelocityActive() noexcept;

// Shader creation (device_hook): the keyed pool families' bytecode is kept
// whether or not the feature is on (ten small shaders), so enabling it live
// needs no restart.
void engineVelocityRememberVs(ID3D11VertexShader*, uint64_t hash, const void* bytecode, size_t bytes,
                              bool linked);
void engineVelocityRememberPs(ID3D11PixelShader*, uint64_t hash, const void* bytecode, size_t bytes,
                              bool linked);

// The per-draw hook (vscreen's DrawIndexedInstanced, owner context, before
// the real draw). The inline half is four generation compares: the slow half
// runs only when the game has rebound a shader, a render target or the
// depth target since the last look.
namespace engine_velocity_detail {
struct DrawCache {
    uint32_t vs = 0, ps = 0, rtv = 0, dsv = 0;
    bool eye = false;
    int family = -1;   // the family whose substituted shaders are bound, -1 none
};
constexpr int kMaxFamilies = 8;
extern std::atomic<bool> live;
extern DrawCache cache;                        // owner thread only
extern uint64_t familyDraws[kMaxFamilies];     // owner thread only: draws that ran substituted
void beforeDrawSlow(ID3D11DeviceContext*, bool rtv0Eye);
bool watchesResource(const ID3D11Resource*) noexcept;
void noteResourceWrite(const ID3D11Resource*) noexcept;
}  // namespace engine_velocity_detail

inline void engineVelocityBeforeDraw(ID3D11DeviceContext* ctx, bool rtv0Eye) {
    using namespace engine_velocity_detail;
    if (!live.load(std::memory_order_relaxed)) return;
    if (cache.vs != bindingGeneration(BindSlot::Vs) || cache.ps != bindingGeneration(BindSlot::Ps) ||
        cache.rtv != bindingGeneration(BindSlot::Rtv0) || cache.dsv != bindingGeneration(BindSlot::Dsv0) ||
        cache.eye != rtv0Eye)
        beforeDrawSlow(ctx, rtv0Eye);
    if (cache.family >= 0) ++familyDraws[cache.family];
}

// The Map/Unmap/Copy/Update tees (vscreen): a write to the pool or the scene
// constants this eye-frame's snapshot came from, while its pass may still
// draw, invalidates that eye-frame's engine data (no stale snapshot is ever
// used; the count says how often).
inline void engineVelocityResourceWritten(const ID3D11Resource* resource) {
    using namespace engine_velocity_detail;
    if (live.load(std::memory_order_relaxed) && watchesResource(resource)) noteResourceWrite(resource);
}

// The present-frame clock (device_hook, once per owned Present).
void engineVelocityNotePresentFrame(uint32_t presentFrame) noexcept;
// The owner thread's frame boundary (vscreen): rotation, the periodic lines.
void engineVelocityFrameBoundary(ID3D11DeviceContext*);

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
// One eye's compose pixel counts, read back by the temporal pass (Stats
// 50..53): engine-joined, masked, pool-but-not-a-rig-record, stale slot.
void engineVelocityNotePixels(uint32_t joined, uint32_t masked, uint32_t camera, uint32_t stale);

}  // namespace edvr
