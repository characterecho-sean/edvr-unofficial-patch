#include "binding_shadow.h"

#include <windows.h>

#include <d3d11.h>

#include "../common/guard.h"

namespace edvr {

// The shadow. Declared in the header so the three per-draw readers are loads
// rather than calls (binding_shadow.h says why); defined here, once, and still
// written only through the setters below.
namespace detail {
BindingSlot g_bindingSlots[static_cast<size_t>(BindSlot::Count)];
uint32_t g_bindingEyeKeyGen = 0;
// Build 5, item 4: engine velocity's aggregate over its five slots. Starts at 1 and never takes 0 (the increment skips it): engine_velocity.h's DrawCache holds
// 0 when fresh or reset and has no valid flag, so 0 must never equal a live value. The per-slot generations start at 1 for the same reason.
uint32_t g_bindingEvKeyGen = 1;
}  // namespace detail

namespace {

using Slot = detail::BindingSlot;

// The eight slots the eye draws' aggregate generation follows (bindingEyeKeyGeneration, binding_shadow.h): the vertex and pixel
// shaders, pixel SRV 0..3, render target 0 and the depth view. The set is the one the binding-repeat flight measured. A mask over
// the slot's index, so a setter's test is one AND; every other slot (the constant buffers, the compute slots, the pool, the
// blend) leaves the aggregate alone.
constexpr uint32_t slotBit(BindSlot s) { return 1u << static_cast<uint32_t>(s); }
constexpr uint32_t kEyeKeySlots = slotBit(BindSlot::Vs) | slotBit(BindSlot::Ps) | slotBit(BindSlot::PsSrv0) |
                                  slotBit(BindSlot::PsSrv1) | slotBit(BindSlot::PsSrv2) | slotBit(BindSlot::PsSrv3) |
                                  slotBit(BindSlot::Rtv0) | slotBit(BindSlot::Dsv0);
static_assert(static_cast<uint32_t>(BindSlot::Count) <= 32, "the eye key's slot mask is one word");

// The five slots engine velocity's draw cache follows (bindingEvKeyGeneration): the vertex and pixel shaders, render target 0, the depth view and the blend.
constexpr uint32_t kEvKeySlots = slotBit(BindSlot::Vs) | slotBit(BindSlot::Ps) | slotBit(BindSlot::Rtv0) | slotBit(BindSlot::Dsv0) | slotBit(BindSlot::Blend);

void bumpEvKey() {
    if (++detail::g_bindingEvKeyGen == 0) detail::g_bindingEvKeyGen = 1;
}

// The one helper bindingSet, bindingInvalidate and bindingSetShader call after bumping the slot's own generation. `ptrMoved` is false only for a PS SRV 0..3
// set of the pointer already bound (build 6, S2): the eye key stays, the slot's own generation moved. The engine velocity key follows every set, as before.
void noteEyeKeySlot(BindSlot s, bool eyeKeyMoves = true) {
    const uint32_t bit = slotBit(s);
    if (eyeKeyMoves && (kEyeKeySlots & bit)) ++detail::g_bindingEyeKeyGen;
    if (kEvKeySlots & bit) bumpEvKey();
}

// This module's own budget, not shared with any fix.
//
// An exhausted budget stops running its body, so a budget shared across features
// switches all of them off together -- which is how a bad camera offset once
// disabled the black void fix. A fault resolving a view means views cannot be
// resolved; it does not mean the exposure copy should stop.
FaultBudget g_viewBudget("bindingShadow.resolve", 5);

// And its own budget for the resource path, which is NOT the same question.
//
// The rule this file states about features applies within it too. A view
// pointer comes from a hook that saw it bound; a resource pointer can come
// from a caller probing speculatively -- the census resolves the constant
// buffer it has only ever held as an identity, and reads vertex buffers
// straight off the context. Sharing one budget would let five faults in that
// speculation stop bindingResolve, and bindingResolve returning false forever
// is srv0IsPanelSized answering false forever, which is the panel distance
// fix switching itself off during a census with nothing saying why. Two
// budgets, because they are two failures.
FaultBudget g_resourceBudget("bindingShadow.resolveResource", 5);

// And the same two again for the instruments (binding_shadow.h, "THE SAME TWO
// RESOLVERS FOR AN INSTRUMENT"). The two budgets above are the fixes'; a census
// or a probe that faults five times used to spend them, and every fix's resolver
// went quiet with it. The instruments' own faults are charged here, so a probe
// can run out alone, and the note says it was a probe.
FaultBudget g_viewProbeBudget("bindingShadow.resolveProbe", 5);
FaultBudget g_resourceProbeBudget("bindingShadow.resolveResourceProbe", 5);

Slot& slotOf(BindSlot s) { return detail::g_bindingSlots[static_cast<size_t>(s)]; }

// The GetType-then-GetDesc half, shared by both resolvers so there is one copy
// of it and not two. Called from inside a guard; holds nothing that unwinds.
//
// GetType FIRST, always.
//
// ID3D11Buffer::GetDesc and ID3D11Texture2D::GetDesc occupy the same vtable
// slot on their respective interfaces, so calling the buffer one on a texture
// writes a 44-byte description into a 20-byte struct. That is a /GS
// stack-smash fast-fail, which SEH cannot catch, and it brought the process
// down on the first frame the one time a copy of this probe was written
// without it.
bool describeResource(ID3D11Resource* res, ResourceInfo* out) {
    D3D11_RESOURCE_DIMENSION dim = D3D11_RESOURCE_DIMENSION_UNKNOWN;
    res->GetType(&dim);
    if (dim == D3D11_RESOURCE_DIMENSION_BUFFER) {
        D3D11_BUFFER_DESC d{};
        static_cast<ID3D11Buffer*>(res)->GetDesc(&d);
        out->isBuffer = true;
        out->a = d.ByteWidth;
        out->b = d.StructureByteStride;
        return true;
    }
    if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
        D3D11_TEXTURE2D_DESC d{};
        static_cast<ID3D11Texture2D*>(res)->GetDesc(&d);
        out->isTexture2D = true;
        out->a = d.Width;
        out->b = d.Height;
        out->fmt = static_cast<uint32_t>(d.Format);
        return true;
    }
    return false;
}

}  // namespace

void bindingSet(BindSlot slot, void* ptr) {
    Slot& s = slotOf(slot);
    const bool srv = slot >= BindSlot::PsSrv0 && slot <= BindSlot::PsSrv3;
    // Build 6, S2 (maintainer-approved behaviour change, 2026-11-04): a PS SRV 0..3 set of the pointer already bound moves the slot's own generation (the scrim
    // cache, engine velocity and srv0IsPanelSized compare it) but not the eye key. Compared before the pointer is overwritten.
    const bool eyeKeyMoves = !(srv && s.ptr == ptr);
    s.ptr = ptr;
    ++s.gen;
    noteEyeKeySlot(slot, eyeKeyMoves);
}

void bindingInvalidate(BindSlot slot) {
    Slot& s = slotOf(slot);
    s.ptr = nullptr;
    s.mayWant = 0xFF;   // a cleared shader slot must not keep a stale may-want byte (vs_may_want.h)
    ++s.gen;
    noteEyeKeySlot(slot);
}

void bindingSetShader(BindSlot slot, void* ptr, uint64_t hash) {
    Slot& s = slotOf(slot);
    s.ptr = ptr;
    s.hash = hash;
    s.mayWant = 0xFF;   // unknown until bindingSetVsMayWant
    ++s.gen;
    noteEyeKeySlot(slot);
}

void bindingSetVsMayWant(uint8_t mayWant) {
    slotOf(BindSlot::Vs).mayWant = mayWant;
}

void bindingForgetAll() {
    for (Slot& s : detail::g_bindingSlots) {
        s.ptr = nullptr;
        s.hash = 0;
        s.mayWant = 0xFF;
        ++s.gen;
    }
    ++detail::g_bindingEyeKeyGen;   // every slot moved, so the aggregate does, once
    bumpEvKey();
}

void bindingFrameBoundary() {
    // Generations only. The pointers stay, because a game that binds once and
    // then draws for many frames is ordinary -- nulling them here cost the panel
    // fix everything after the first Present, measured at 1800 overrides before
    // the change and 1 after.
    for (Slot& s : detail::g_bindingSlots) ++s.gen;
    ++detail::g_bindingEyeKeyGen;   // ...and the aggregate, so a memo keyed on it cannot outlive a frame either
    bumpEvKey();
}

namespace {

// The one copy of each resolver. The public entry points below differ only in
// whose budget a fault is charged to.
bool resolveView(FaultBudget& budget, void* view, ResourceInfo* out) {
    if (!view || !out) return false;
    *out = ResourceInfo();

    bool ok = false;
    guardedBudget(budget, [&] {
        ID3D11Resource* res = nullptr;
        static_cast<ID3D11View*>(view)->GetResource(&res);
        if (!res) return;
        ok = describeResource(res, out);
        // Identity only, recorded before the Release on purpose: the pointer
        // is what connects a view to the resource behind it, and the context
        // still holds its own reference to anything reachable through a live
        // binding. Nothing dereferences this.
        out->resource = res;
        res->Release();
    });
    // A fault between GetResource and this Release leaks one reference. That is
    // deliberate rather than overlooked: SEH and C++ objects with destructors
    // cannot share a frame (C2712), so the alternative is no guard at all. The
    // budget caps it at five for the life of the process, and a leaked reference
    // on a resource the game is about to lose anyway is a far better outcome
    // than the crash it replaces.
    return ok;
}

bool resolveResource(FaultBudget& budget, void* resource, ResourceInfo* out) {
    if (!resource || !out) return false;
    *out = ResourceInfo();

    bool ok = false;
    guardedBudget(budget, [&] {
        // No GetResource step and so no reference taken: the caller's pointer
        // is the resource, and the caller owns whatever reference it arrived
        // with. That also means this one has no leak to document -- the
        // bargain the view resolver strikes above does not arise here.
        ok = describeResource(static_cast<ID3D11Resource*>(resource), out);
        out->resource = resource;
    });
    return ok;
}

}  // namespace

bool bindingResolve(void* view, ResourceInfo* out) {
    return resolveView(g_viewBudget, view, out);
}

bool bindingResolveResource(void* resource, ResourceInfo* out) {
    return resolveResource(g_resourceBudget, resource, out);
}

bool bindingResolveProbe(void* view, ResourceInfo* out) {
    return resolveView(g_viewProbeBudget, view, out);
}

bool bindingResolveResourceProbe(void* resource, ResourceInfo* out) {
    return resolveResource(g_resourceProbeBudget, resource, out);
}

}  // namespace edvr
