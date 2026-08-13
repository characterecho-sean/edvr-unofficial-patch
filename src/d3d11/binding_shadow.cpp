#include "binding_shadow.h"

#include <windows.h>

#include <d3d11.h>

#include "../common/guard.h"

namespace edvr {
namespace {

struct Slot {
    void*    ptr = nullptr;
    uint32_t gen = 1;   // starts at 1 so a caller's zero-initialised cache is stale
};

Slot g_slots[static_cast<size_t>(BindSlot::Count)];

// This module's own budget, not shared with any fix.
//
// An exhausted budget stops running its body, so a budget shared across features
// switches all of them off together -- which is how a bad camera offset once
// disabled the black void fix. A fault resolving a view means views cannot be
// resolved; it does not mean the exposure copy should stop.
FaultBudget g_probeBudget("bindingShadow.resolve", 5);

Slot& slotOf(BindSlot s) { return g_slots[static_cast<size_t>(s)]; }

}  // namespace

void* bindingGet(BindSlot slot) { return slotOf(slot).ptr; }

uint32_t bindingGeneration(BindSlot slot) { return slotOf(slot).gen; }

void bindingSet(BindSlot slot, void* ptr) {
    Slot& s = slotOf(slot);
    s.ptr = ptr;
    ++s.gen;
}

void bindingForgetAll() {
    for (Slot& s : g_slots) {
        s.ptr = nullptr;
        ++s.gen;
    }
}

void bindingFrameBoundary() {
    // Generations only. The pointers stay, because a game that binds once and
    // then draws for many frames is ordinary -- nulling them here cost the panel
    // fix everything after the first Present, measured at 1800 overrides before
    // the change and 1 after.
    for (Slot& s : g_slots) ++s.gen;
}

bool bindingResolve(void* view, ResourceInfo* out) {
    if (!view || !out) return false;
    *out = ResourceInfo();

    bool ok = false;
    guardedBudget(g_probeBudget, [&] {
        ID3D11Resource* res = nullptr;
        static_cast<ID3D11View*>(view)->GetResource(&res);
        if (!res) return;

        // GetType FIRST, always.
        //
        // ID3D11Buffer::GetDesc and ID3D11Texture2D::GetDesc occupy the same
        // vtable slot on their respective interfaces, so calling the buffer one
        // on a texture writes a 44-byte description into a 20-byte struct. That
        // is a /GS stack-smash fast-fail, which SEH cannot catch, and it brought
        // the process down on the first frame the one time a copy of this probe
        // was written without it.
        D3D11_RESOURCE_DIMENSION dim = D3D11_RESOURCE_DIMENSION_UNKNOWN;
        res->GetType(&dim);
        if (dim == D3D11_RESOURCE_DIMENSION_BUFFER) {
            D3D11_BUFFER_DESC d{};
            static_cast<ID3D11Buffer*>(res)->GetDesc(&d);
            out->isBuffer = true;
            out->a = d.ByteWidth;
            out->b = d.StructureByteStride;
            ok = true;
        } else if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
            D3D11_TEXTURE2D_DESC d{};
            static_cast<ID3D11Texture2D*>(res)->GetDesc(&d);
            out->isTexture2D = true;
            out->a = d.Width;
            out->b = d.Height;
            ok = true;
        }
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

}  // namespace edvr
