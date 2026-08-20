// What the immediate context has bound, as far as we can see -- in one place.
//
// WHY THIS EXISTS
//
// Two fixes need to know what was bound before a draw or a dispatch: vscreen
// asks about the render target and the sampled texture, exposure_fix asks about
// the compute shader and its UAVs. D3D does not tell anyone when a binding ends;
// it can be dropped by ClearState, by replaying a command list, or by a path
// nobody has hooked. So both kept a private shadow, and both had to answer the
// same question: how does a shadow stay truthful?
//
// They answered it OPPOSITELY, and each answer had its own failure:
//
//   vscreen kept the pointers and re-derived its answers each frame. An unbind
//   through an unhooked path therefore left a dangling pointer that was probed
//   again at the first draw of EVERY later frame -- the stale-view crash class,
//   no longer bounded to a single frame.
//
//   exposure_fix nulled its pointers at the frame boundary. If the engine ever
//   filters a redundant re-bind, the shadow is null for the rest of the session,
//   hashOf(nullptr) returns 0 every frame, and the fix goes silently inert while
//   the give-up notice blames the game for being stock.
//
// Two policies in two files guarantees the next fix copies one of them wrongly.
// This is the single policy, and it is:
//
//   1. KEEP the pointers. A game that binds once and draws for many frames is
//      normal, and forgetting is what breaks it.
//   2. Give every slot a GENERATION, bumped whenever the binding could have
//      changed -- on rebind, on forgetAll, and once per frame. A derived answer
//      records the generation it was computed at and is recomputed when they
//      differ, so no answer outlives the thing it was derived from.
//   3. Resolve views through ONE guarded probe with its own fault budget, so a
//      pointer that is no longer a live view degrades to "unknown" instead of
//      crashing -- and so a fault in this probe cannot disable an unrelated fix.
//
// Point 3 is also why the GetResource -> GetType -> GetDesc dance lives here.
// It was hand-written five times across two files, and the one copy that forgot
// GetType wrote a 44-byte texture description into a 20-byte buffer struct.
#pragma once

#include <cstdint>

struct ID3D11DeviceContext;

namespace edvr {

// Which slot a caller is asking about. One generation counter each, so binding a
// compute UAV does not invalidate vscreen's answer about a render target.
//
// PsSrv1..3 and Dsv0 exist for the draw census (draw_census.h), which
// fingerprints a draw by everything it reads and writes -- an overlay is
// often the draw with no depth bound and a mask texture in a later SRV slot.
// No fix derives answers from them. PsSrv1..3 must stay contiguous after
// PsSrv0: the PSSetShaderResources hook records slot i by offsetting from
// PsSrv0.
enum class BindSlot : uint32_t {
    Rtv0 = 0,      // OM render target slot 0
    Dsv0,          // OM depth-stencil view
    PsSrv0,        // pixel shader resource slots 0..3
    PsSrv1,
    PsSrv2,
    PsSrv3,
    VsCb0,         // vertex shader constant buffer slot 0
    Cs,            // the bound compute shader
    CsUav0,        // compute UAV slots 0..3
    CsUav1,
    CsUav2,
    CsUav3,
    Count
};

// What a view's underlying resource turned out to be.
struct ResourceInfo {
    bool     isBuffer = false;
    bool     isTexture2D = false;
    uint32_t a = 0;   // texture width, or buffer byte width
    uint32_t b = 0;   // texture height, or buffer structure stride
    uint32_t fmt = 0; // DXGI_FORMAT for a texture, 0 for a buffer. Size alone
                      // cannot tell two same-shaped textures apart in a census
                      // line, and the format is free: the desc is in hand.
};

// The pointer last seen bound to a slot, or nullptr.
void* bindingGet(BindSlot slot);

// How many times that slot's binding could have changed. A cached answer is
// stale when this differs from the value it was computed at.
uint32_t bindingGeneration(BindSlot slot);

// Record a new binding. Bumps that slot's generation even when the pointer is
// unchanged: an identical address after a rebind is not evidence of an identical
// object, which is the bug that shipped as 0.5.2.
void bindingSet(BindSlot slot, void* ptr);

// Everything is unbound -- ClearState, or ExecuteCommandList without restore.
// The only place forgetting a pointer is the truth rather than a guess.
void bindingForgetAll();

// Once per frame. Bumps every generation, so an answer cannot outlive a frame
// even if the binding ended through a path we do not hook. This is the bound
// that keeps an unhooked unbind cheap instead of permanent.
void bindingFrameBoundary();

// Resolve a view to its resource, under SEH and this module's own budget.
//
// Returns false when the answer is not knowable -- including when the pointer is
// no longer a live view, which is exactly the case the guard exists for. Callers
// must treat false as "do nothing", never as "no".
bool bindingResolve(void* view, ResourceInfo* out);

// Resolve a RESOURCE held directly -- a buffer or a texture that arrived as
// itself rather than through a view.
//
// bindingResolve's first step is ID3D11View::GetResource, which is vtable slot
// 7 on a view and ID3D11Resource::GetType on everything else. Handing it a
// vertex buffer therefore writes a four-byte enum through an eight-byte out
// pointer and then dereferences whatever that left behind -- the same class of
// mistake as the GetDesc one the comment below records, arrived at from the
// other end. The census reads IA state straight off the context, where vertex
// buffers come back as ID3D11Buffer* and never as views, so it needs this
// dance with that first step left out. It lives here for the reason the whole
// file exists: the alternative is a sixth hand-written copy.
//
// Same contract as bindingResolve -- false means "not knowable", never "no" --
// but its OWN fault budget, deliberately. A view pointer reached a hook that
// saw it bound; a resource pointer can be a speculative probe of something
// only ever held as an identity. Five faults in the speculation must not stop
// views resolving, because a view that stops resolving is the panel distance
// fix silently standing down.
bool bindingResolveResource(void* resource, ResourceInfo* out);

}  // namespace edvr
