// COM vtable interception by patching entries in place.
//
// We make the vtable page writable, exchange the few function pointers we
// care about for our own, and put the protection back. The object's vptr is
// never touched, so the object's identity survives and any mod that wraps
// D3D11/OpenVR objects keeps dispatching through its own tables.
//
// WHY NOT COPY-AND-SWAP-VPTR, WHICH THIS REPLACED
//
// The old mechanism copied the table and pointed the OBJECT at the copy.
// Per-object, elegant, and structurally incompatible with wrapper mods: it
// re-points an object somebody else owns and dispatches through. ReShade
// installed as dxgi.dll wraps the device and context, so EDVR was attaching
// to ReShade's proxies -- visible in every log as `exposure fix installed on
// ... (149 methods)` against 302 without it -- and games crashed on launch
// (issue #6). The crash looked random because the crash sentinel disables
// the fixes on the following launch, so it alternated. Ordering cannot fix
// it; going second is exactly the failure. tools/vtable_test reproduces the
// collision, and those cells failed against the old mechanism first.
//
// WHAT THE CHANGE COSTS THE CALLER: patching a vtable hooks EVERY object of
// that class, not the one you attached to. Each hook body must therefore
// begin by checking that `self` is the object it was installed for and
// forwarding untouched otherwise. That check is not optional -- deferred
// contexts and a wrapper mod's internal objects reach the same table.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace edvr {

// Walks the vtable checking that each slot points into committed executable
// memory, and stops at the first slot that does not. Returns the number of
// plausible entries, capped at maxEntries.
size_t probeVTableLength(void** vtable, size_t maxEntries);

// True if p points into committed, executable memory.
bool isExecutableAddress(const void* p);

class VTableHook {
public:
    VTableHook() = default;
    ~VTableHook() { uninstall(); }

    VTableHook(const VTableHook&) = delete;
    VTableHook& operator=(const VTableHook&) = delete;

    // Reads the object's vtable and sanity-checks it. maxEntries bounds how
    // far the plausibility probe walks. Nothing is written here.
    bool attach(void* object, size_t maxEntries = 512);

    // Stages one slot. origOut receives what the slot holds NOW, which is
    // what the caller must forward to -- if another hook (ours or a foreign
    // one) already patched this slot, that is the entry we chain to, and the
    // chain is preserved in both directions by the polite uninstall below.
    // Must be called before commit(). Refuses indices beyond the executable
    // prefix, since those are not methods we have any reason to believe in.
    bool replace(size_t index, void* replacement, void** origOut);

    // Writes every staged entry into the real vtable. All-or-nothing: a
    // partial failure rolls back the slots already written, the same
    // discipline vscreen_res uses for code patching.
    bool commit();

    // Restores each entry we wrote, but ONLY where it still holds our
    // replacement. An entry someone patched after us belongs to them now;
    // restoring it would clobber their hook, which is the same composition
    // failure this class exists to stop, viewed from the other side.
    void uninstall();

    bool     attached() const { return m_object != nullptr; }
    bool     committed() const { return m_committed; }
    size_t   entryCount() const { return m_execPrefix; }
    // Leading entries that point into committed executable memory. This is
    // what callers should range-check a slot index against.
    size_t   executablePrefix() const { return m_execPrefix; }
    void*    object() const { return m_object; }
    void**   originalVTable() const { return m_vtable; }

private:
    struct Patch {
        size_t slot = 0;
        void*  replacement = nullptr;
        void*  original = nullptr;
    };

    // Writes one entry with the page temporarily writable. Returns false and
    // changes nothing if the protection could not be moved.
    static bool writeEntry(void** vtable, size_t slot, void* value);

    void*              m_object = nullptr;
    void**             m_vtable = nullptr;
    std::vector<Patch> m_patches;
    size_t             m_execPrefix = 0;
    bool               m_committed = false;
};

}  // namespace edvr
