// GENERATED from src/common/vtable_hook.h in the private edvr repo -- do not edit here.
// Edit there, then: python tools/sync_common.py --write   [body-sha256 74ab56c465c403a2]
// COM vtable interception by vtable copy and vptr swap.
//
// We copy the object's vtable into memory we own, patch the entries we care
// about in the copy, and point the object instance at it. That keeps the real
// vtable inside the host DLL untouched, forwards every method we did not patch
// automatically, and needs only the index of the methods we want -- not the
// full interface declaration.
//
// The one write we make is the 8-byte vptr in the object instance. uninstall()
// puts it back.
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

    // Copies the object's vtable. maxEntries bounds how wide a window is
    // copied, which should be generous: a copy truncated to the apparent method
    // count breaks the moment the host calls a slot beyond it. Returns false if
    // the object or its vtable does not look sane.
    bool attach(void* object, size_t maxEntries = 512);

    // Patches one slot in our copy. Fills origOut with the original entry.
    // Must be called before commit(). Refuses indices beyond the executable
    // prefix, since those are not methods we have any reason to believe in.
    bool replace(size_t index, void* replacement, void** origOut);

    // Points the object at the patched copy.
    bool commit();

    // Restores the object's original vptr.
    void uninstall();

    bool     attached() const { return m_object != nullptr; }
    bool     committed() const { return m_committed; }
    size_t   entryCount() const { return m_copy.size(); }
    // Leading entries that point into committed executable memory. This, not
    // entryCount(), is the count callers should range-check a slot index
    // against -- entryCount() is a deliberately over-wide copy window.
    size_t   executablePrefix() const { return m_execPrefix; }
    void*    object() const { return m_object; }
    void**   originalVTable() const { return m_original; }

private:
    void*              m_object = nullptr;
    void**             m_original = nullptr;
    std::vector<void*> m_copy;
    size_t             m_execPrefix = 0;
    bool               m_committed = false;
};

}  // namespace edvr
