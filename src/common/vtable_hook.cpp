// GENERATED from src/common/vtable_hook.cpp in the private edvr repo -- do not edit here.
// Edit there, then: python tools/sync_common.py --write   [body-sha256 6fa1bdd8b1f26230]
#include "vtable_hook.h"

#include <windows.h>

#include "guard.h"
#include "log.h"

namespace edvr {

bool isExecutableAddress(const void* p) {
    if (!p) return false;
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(p, &mbi, sizeof(mbi)) != sizeof(mbi)) return false;
    if (mbi.State != MEM_COMMIT) return false;
    const DWORD exec = PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
                       PAGE_EXECUTE_WRITECOPY;
    if ((mbi.Protect & exec) == 0) return false;
    if (mbi.Protect & PAGE_GUARD) return false;
    return true;
}

size_t probeVTableLength(void** vtable, size_t maxEntries) {
    if (!vtable) return 0;
    size_t count = 0;
    for (size_t i = 0; i < maxEntries; ++i) {
        void* entry = nullptr;
        // Reading past the end of a vtable can land on an unmapped page, so the
        // read itself is guarded, not just the validity check.
        const bool ok = guarded("probeVTableLength", [&] { entry = vtable[i]; });
        if (!ok || !isExecutableAddress(entry)) break;
        ++count;
    }
    return count;
}

bool VTableHook::attach(void* object, size_t maxEntries) {
    if (m_object) return false;
    if (!object) return false;

    void** vt = nullptr;
    if (!guarded("VTableHook::attach/read-vptr",
                 [&] { vt = *reinterpret_cast<void***>(object); })) {
        return false;
    }
    if (!vt) return false;

    // The executable prefix is a sanity check only -- it says "this really is a
    // vtable" and bounds which slots we are willing to patch.
    m_execPrefix = probeVTableLength(vt, maxEntries);
    if (m_execPrefix < 4) {
        Log::get().note("VTableHook: implausible vtable at %p (%zu executable entries)",
                        object, m_execPrefix);
        return false;
    }

    // Copy as wide a window as is readable, NOT just the executable prefix.
    //
    // Stopping the copy at the first slot that does not look like code is how
    // you build a vtable that works right up until the host calls a method past
    // the cut and reads off the end of our buffer. That is a use of
    // uninitialised heap, and it will not reproduce until something unusual
    // happens -- teardown, an interface the game rarely uses, a runtime update
    // that adds a method. Copy generously and let the tail be whatever the
    // original held.
    m_copy.clear();
    m_copy.reserve(maxEntries);
    for (size_t i = 0; i < maxEntries; ++i) {
        void* entry = nullptr;
        if (!guarded("VTableHook::attach/copy", [&] { entry = vt[i]; })) break;
        m_copy.push_back(entry);
    }
    if (m_copy.size() < m_execPrefix) {
        m_copy.clear();
        return false;
    }

    m_object   = object;
    m_original = vt;
    return true;
}

bool VTableHook::replace(size_t index, void* replacement, void** origOut) {
    if (!m_object || m_committed) return false;
    if (index >= m_execPrefix) return false;
    if (origOut) *origOut = m_copy[index];
    m_copy[index] = replacement;
    return true;
}

bool VTableHook::commit() {
    if (!m_object || m_committed) return false;

    void** target = reinterpret_cast<void**>(m_object);
    DWORD oldProtect = 0;
    if (!VirtualProtect(target, sizeof(void*), PAGE_READWRITE, &oldProtect)) {
        Log::get().note("VTableHook: VirtualProtect failed on %p (err %lu)",
                        m_object, GetLastError());
        return false;
    }
    const bool ok = guarded("VTableHook::commit",
                            [&] { *target = reinterpret_cast<void*>(m_copy.data()); });
    DWORD ignored = 0;
    VirtualProtect(target, sizeof(void*), oldProtect, &ignored);

    if (!ok) return false;
    m_committed = true;
    return true;
}

void VTableHook::uninstall() {
    if (!m_object) return;
    if (m_committed) {
        void** target = reinterpret_cast<void**>(m_object);
        DWORD oldProtect = 0;
        if (VirtualProtect(target, sizeof(void*), PAGE_READWRITE, &oldProtect)) {
            guarded("VTableHook::uninstall",
                    [&] { *target = reinterpret_cast<void*>(m_original); });
            DWORD ignored = 0;
            VirtualProtect(target, sizeof(void*), oldProtect, &ignored);
        }
        m_committed = false;
    }
    m_object = nullptr;
    m_original = nullptr;
    m_execPrefix = 0;
    m_copy.clear();
}

}  // namespace edvr
