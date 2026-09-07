#include "vtable_hook.h"

#include <windows.h>

#include <cstdio>   // _snprintf_s, for the reclaimed-slot list
#include <cstring>  // strncat_s

#include "guard.h"
#include "log.h"

namespace edvr {

namespace {

// Which (vtable, slot) pairs are EDVR's, and whose replacement sits where.
//
// reclaim() needs one distinction it cannot make from a single hook's own
// records: "this slot holds a stranger" versus "this slot holds ANOTHER EDVR
// hook, stacked on mine". The two context hooks really do share ClearState
// (slot 110, exposure first, vscreen on top), so without this the lower hook
// reads the upper one as a clobber, re-patches over it, the upper does the
// same back a second later -- and after one round of that the two forwards
// point at each other, which is an infinite call loop discovered by the next
// ClearState. The registry is written at commit and erased at uninstall, so a
// scan answers for the hooks that exist NOW.
struct SlotOwner {
    void**            vtable = nullptr;
    size_t            slot = 0;
    void*             replacement = nullptr;
    const VTableHook* owner = nullptr;
};

// More than the two DLLs together ever commit (about two dozen). If it fills,
// commit() refuses reclaim for the overflowing hook rather than letting it
// run half-informed: a reclaim that cannot see an owner is a reclaim that can
// build the loop above.
constexpr size_t kMaxSlotOwners = 64;
SlotOwner g_slotOwners[kMaxSlotOwners];
size_t    g_slotOwnerCount = 0;

// Registration happens at install time and reclaim on the frame path, which
// in practice never overlap -- the swapchain hook that drives frames is
// itself one of these registrations. The lock costs nothing and removes the
// "in practice" from that sentence.
SRWLOCK g_slotOwnersLock = SRWLOCK_INIT;

// Set when the registry overflows. Reclaim then stops FOR EVERY HOOK, not just
// the overflowing one: a hook that is patched but invisible to the registry is
// read by its shared-slot co-owner as an intruder, and "re-claiming" a
// co-owner is the call loop the registry exists to prevent. All-or-nothing is
// the only shape of this that stays safe without being re-derived at every
// future call site.
bool g_slotOwnersPoisoned = false;

// How many times one slot may be re-taken before it is conceded. A tool that
// re-checks its hooks like we do would otherwise trade the slot back and
// forth for the whole session; 64 exchanges is far past any one-time
// installer. Each exchange now costs the re-checker's write plus our
// three-quiet-passes vouch, so a full tug-of-war runs four to five minutes
// before the concession -- still bounded, still loud, and slow enough that
// the log shows the rhythm of the fight rather than a blur.
constexpr uint32_t kMaxRepatchesPerSlot = 64;

// Which DLL owns this pointer, for the reclaim log lines.
//
// A detection line that says "another tool" made every report a fingerprinting
// exercise: the 2026-08-18 field case took a day of timing analysis to
// attribute, and the answer was one VirtualQuery away the whole time -- the
// foreign pointer's allocation base IS the module handle of whoever owns it.
// Resolved only on the logging paths, never per pass.
const char* ownerModuleName(void* p, char* buf, size_t bufLen) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(p, &mbi, sizeof(mbi)) != sizeof(mbi) || !mbi.AllocationBase) {
        return "no loaded module (freed or generated code)";
    }
    char path[MAX_PATH] = {};
    if (!GetModuleFileNameA(static_cast<HMODULE>(mbi.AllocationBase), path,
                            sizeof(path))) {
        return "no loaded module (freed or generated code)";
    }
    // The FULL path plus the offset from the module base, not the basename.
    // The basename identified the first field thief as "d3d11.dll" -- which
    // names two different modules in this process: the system runtime AND
    // EDVR's own proxy, which the game loads under exactly that name. A line
    // that cannot tell the runtime from ourselves is a line that cannot close
    // the question it exists to answer; the path can, and the offset lets a
    // debugger name the exact function without a live process.
    _snprintf_s(buf, bufLen, _TRUNCATE, "%s+0x%llX", path,
                static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(p) -
                                                reinterpret_cast<uintptr_t>(
                                                    mbi.AllocationBase)));
    return buf;
}

// Which mapped image a pointer belongs to, or null for none. The same
// VirtualQuery ownerModuleName uses, without the formatting -- this one is a
// comparison, not a message, and it runs on the reclaim path rather than only
// the logging one.
void* owningModule(void* p) {
    if (!p) return nullptr;
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(p, &mbi, sizeof(mbi)) != sizeof(mbi)) return nullptr;
    return mbi.AllocationBase;
}

// A page protection as its name, for the write autopsy. Hex here would be one
// more lookup between a reader and the answer, and the copy-on-write values are
// the whole reason the line exists.
const char* protectName(DWORD p) {
    switch (p & 0xFF) {
        case PAGE_NOACCESS:               return "PAGE_NOACCESS";
        case PAGE_READONLY:               return "PAGE_READONLY";
        case PAGE_READWRITE:              return "PAGE_READWRITE";
        case PAGE_WRITECOPY:              return "PAGE_WRITECOPY (copy-on-write)";
        case PAGE_EXECUTE:                return "PAGE_EXECUTE";
        case PAGE_EXECUTE_READ:           return "PAGE_EXECUTE_READ";
        case PAGE_EXECUTE_READWRITE:      return "PAGE_EXECUTE_READWRITE";
        case PAGE_EXECUTE_WRITECOPY:      return "PAGE_EXECUTE_WRITECOPY (copy-on-write)";
        default:                          return "an unnamed protection";
    }
}

// Append a slot number to a comma-separated list, or close the list with an
// ellipsis if it will not fit.
//
// strncat_s with _TRUNCATE does not stop at a boundary that means anything: it
// cuts mid-number, so ", 19" becomes ", 1" and the line names a slot that was
// never touched. The counts printed beside these lists stay honest either way,
// but a reader chasing a slot number cannot tell a real one from half of one.
// Both list-building sites in this file use this.
void appendSlot(char* list, size_t listSize, size_t slot) {
    if (!list || listSize < 8) return;
    const size_t used = strlen(list);
    // Already closed with an ellipsis by an earlier call.
    if (used >= 3 && list[used - 1] == '.') return;
    char one[24];
    _snprintf_s(one, sizeof(one), _TRUNCATE, "%s%zu", used ? ", " : "", slot);
    // Room for this entry AND a later ellipsis, or the list ends here instead.
    if (used + strlen(one) + 5 >= listSize) {
        strncat_s(list, listSize, ", ...", _TRUNCATE);
        return;
    }
    strncat_s(list, listSize, one, _TRUNCATE);
}

// THE WRITE WATCH. One page, made read-only, so the next write to it faults and
// the handler can read the faulting instruction's address out of the exception.
// See the header for why naming the writer needed its own mechanism.
struct WriteWatch {
    void*     slotAddress = nullptr;
    void*     pageBase = nullptr;
    SIZE_T    pageSize = 0;
    DWORD     writableProtect = 0;   // what the page was, and is put back to
    DWORD     readOnlyProtect = 0;   // the same without write permission
    volatile LONG armed = 0;
    volatile LONG rearmWanted = 0;
    uint32_t  catches = 0;
    char      who[48] = {};
};
WriteWatch g_watch;
PVOID      g_watchHandler = nullptr;

// Four is enough to tell one writer from several and to show whether the same
// instruction does it every time; more than that is a log nobody reads.
constexpr uint32_t kMaxWatchCatches = 4;

// The same protection without write permission, or 0 if there is nothing to
// take away (already read-only, or no access at all).
DWORD withoutWrite(DWORD p) {
    switch (p & 0xFF) {
        case PAGE_READWRITE:         return PAGE_READONLY;
        case PAGE_WRITECOPY:         return PAGE_READONLY;
        case PAGE_EXECUTE_READWRITE: return PAGE_EXECUTE_READ;
        case PAGE_EXECUTE_WRITECOPY: return PAGE_EXECUTE_READ;
        default:                     return 0;
    }
}

LONG CALLBACK writeWatchHandler(EXCEPTION_POINTERS* ep) {
    if (!InterlockedCompareExchange(&g_watch.armed, 0, 0)) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    const EXCEPTION_RECORD* er = ep ? ep->ExceptionRecord : nullptr;
    if (!er || er->ExceptionCode != EXCEPTION_ACCESS_VIOLATION) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    // [0] is 0 for a read, 1 for a write, 8 for a DEP violation; [1] is the
    // address touched. A read of a read-only page does not fault, so anything
    // arriving here for our page is the write we are hunting -- but the test is
    // explicit, because passing somebody else's access violation off as our
    // answer would be worse than no answer.
    if (er->NumberParameters < 2 || er->ExceptionInformation[0] != 1) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    const uintptr_t at = static_cast<uintptr_t>(er->ExceptionInformation[1]);
    const uintptr_t lo = reinterpret_cast<uintptr_t>(g_watch.pageBase);
    if (at < lo || at >= lo + g_watch.pageSize) return EXCEPTION_CONTINUE_SEARCH;

    // Let the write through: restore write permission and disarm. The
    // instruction re-executes when we continue, and the frame path puts the
    // watch back.
    InterlockedExchange(&g_watch.armed, 0);
    DWORD ignored = 0;
    VirtualProtect(g_watch.pageBase, g_watch.pageSize, g_watch.writableProtect,
                   &ignored);

    if (g_watch.catches < kMaxWatchCatches) {
        ++g_watch.catches;
        char modBuf[MAX_PATH];
        Log::get().note(
            "VTableHook %s: CAUGHT THE WRITER. Something wrote to %p, which is "
            "%s, and the instruction that did it is at %s. The write has been "
            "allowed through and the watch re-arms next frame; at most %u of "
            "these. THIS is the author of the value that keeps reappearing in "
            "the slot -- every other module name in this log is the value "
            "itself, not whoever stored it.",
            g_watch.who, reinterpret_cast<void*>(at),
            at == reinterpret_cast<uintptr_t>(g_watch.slotAddress)
                ? "exactly the watched slot"
                : "elsewhere on the same page as the watched slot",
            ownerModuleName(er->ExceptionAddress, modBuf, sizeof(modBuf)),
            static_cast<unsigned>(kMaxWatchCatches));
    }
    if (g_watch.catches < kMaxWatchCatches) {
        InterlockedExchange(&g_watch.rearmWanted, 1);
    } else {
        Log::get().note(
            "VTableHook %s: write watch DISARMED after %u catches. The page is "
            "back to normal and nothing further is being intercepted.",
            g_watch.who, static_cast<unsigned>(g_watch.catches));
    }
    return EXCEPTION_CONTINUE_EXECUTION;
}

}  // namespace

bool vtableWatchSlot(void** vtable, size_t slot, const char* who) {
    if (!vtable) return false;
    if (g_watch.slotAddress) {
        Log::get().note("VTableHook: a write watch is already armed on %p; "
                        "only one at a time, so this request was ignored.",
                        g_watch.slotAddress);
        return false;
    }

    void* const addr = static_cast<void*>(&vtable[slot]);
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(addr, &mbi, sizeof(mbi)) != sizeof(mbi)) return false;

    const DWORD readOnly = withoutWrite(mbi.Protect);
    if (!readOnly) {
        Log::get().note(
            "VTableHook: cannot watch slot %zu at %p -- the page is %s, which is "
            "not writable to begin with, so a write to it already faults and "
            "whoever writes must be un-protecting it first. This probe cannot "
            "see that.",
            slot, addr, protectName(mbi.Protect));
        return false;
    }

    // ONE page, not the region. VirtualQuery reports the whole run of pages
    // sharing a protection, which on a heap can be megabytes; making all of
    // that read-only would fault on every unrelated write in it and bring the
    // game to a halt.
    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    const SIZE_T pageSize = si.dwPageSize ? si.dwPageSize : 4096;
    void* const pageBase = reinterpret_cast<void*>(
        reinterpret_cast<uintptr_t>(addr) & ~static_cast<uintptr_t>(pageSize - 1));

    if (!g_watchHandler) {
        // First in the chain: a handler that runs after somebody else's would
        // never see an exception they continued from.
        g_watchHandler = AddVectoredExceptionHandler(1, writeWatchHandler);
        if (!g_watchHandler) return false;
    }

    g_watch.slotAddress = addr;
    g_watch.pageBase = pageBase;
    g_watch.pageSize = pageSize;
    g_watch.writableProtect = mbi.Protect;
    g_watch.readOnlyProtect = readOnly;
    g_watch.catches = 0;
    strncpy_s(g_watch.who, who ? who : "?", _TRUNCATE);

    DWORD previous = 0;
    if (!VirtualProtect(pageBase, pageSize, readOnly, &previous)) {
        Log::get().note("VTableHook: could not make the page at %p read-only "
                        "(err %lu), so the write watch is off.",
                        pageBase, GetLastError());
        g_watch.slotAddress = nullptr;
        return false;
    }
    InterlockedExchange(&g_watch.armed, 1);
    Log::get().note(
        "VTableHook %s: WRITE WATCH ARMED on slot %zu at %p. Its page (%p, %llu "
        "bytes, %s) is read-only until something writes to it, and the next "
        "write is caught and its instruction named. Diagnostic only -- every "
        "write anywhere on this page takes an exception while it is armed, so "
        "this is for one session with a purpose, not for playing. Set "
        "advanced.vtable_writer_probe back to 0 afterwards.",
        g_watch.who, slot, addr, pageBase,
        static_cast<unsigned long long>(pageSize), protectName(mbi.Protect));
    return true;
}

void vtableWatchRearm() {
    if (!InterlockedCompareExchange(&g_watch.rearmWanted, 0, 0)) return;
    InterlockedExchange(&g_watch.rearmWanted, 0);
    DWORD previous = 0;
    if (VirtualProtect(g_watch.pageBase, g_watch.pageSize,
                       g_watch.readOnlyProtect, &previous)) {
        InterlockedExchange(&g_watch.armed, 1);
    }
}

uint32_t vtableWatchCatches() { return g_watch.catches; }

void vtableWatchStop() {
    if (!g_watch.slotAddress) return;
    InterlockedExchange(&g_watch.armed, 0);
    InterlockedExchange(&g_watch.rearmWanted, 0);
    DWORD previous = 0;
    VirtualProtect(g_watch.pageBase, g_watch.pageSize, g_watch.writableProtect,
                   &previous);
    // The handler stays registered. Removing it would race any thread already
    // inside it, and an unarmed handler is a compare and a return.
    g_watch.slotAddress = nullptr;
    g_watch.pageBase = nullptr;
    g_watch.pageSize = 0;
    g_watch.catches = 0;
}

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

// [base, base+SizeOfImage) for a loaded module, read from its own PE headers
// (the walk GetModuleInformation does, without pulling in psapi). Returns
// false and a zero range on anything that is not a valid image.
static bool moduleRange(void* moduleBase, uintptr_t* lo, uintptr_t* hi) {
    if (!moduleBase) return false;
    const auto* dos = static_cast<const IMAGE_DOS_HEADER*>(moduleBase);
    bool ok = false;
    guarded("moduleRange", [&] {
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) return;
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(
            static_cast<const uint8_t*>(moduleBase) + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE) return;
        *lo = reinterpret_cast<uintptr_t>(moduleBase);
        *hi = *lo + nt->OptionalHeader.SizeOfImage;
        ok = true;
    });
    return ok;
}

bool vtableInsideModule(void** vtable, void* moduleBase) {
    if (!vtable) return false;
    uintptr_t lo = 0, hi = 0;
    if (!moduleRange(moduleBase, &lo, &hi)) return false;
    const uintptr_t at = reinterpret_cast<uintptr_t>(vtable);
    return at >= lo && at < hi;
}

size_t vtableEntriesInModule(void** vtable, size_t count, void* moduleBase) {
    if (!vtable) return 0;
    uintptr_t lo = 0, hi = 0;
    if (!moduleRange(moduleBase, &lo, &hi)) return 0;
    size_t hits = 0;
    for (size_t i = 0; i < count; ++i) {
        void* entry = nullptr;
        if (!guarded("vtableEntriesInModule", [&] { entry = vtable[i]; })) break;
        const uintptr_t at = reinterpret_cast<uintptr_t>(entry);
        if (at >= lo && at < hi) ++hits;
    }
    return hits;
}

bool VTableHook::writeEntry(void** vtable, size_t slot, void* value,
                            const char* why) {
    DWORD oldProtect = 0;
    if (!VirtualProtect(&vtable[slot], sizeof(void*), PAGE_READWRITE, &oldProtect)) {
        Log::get().note("VTableHook: could not make vtable slot %zu at %p "
                        "writable (err %lu)", slot, (void*)&vtable[slot],
                        GetLastError());
        return false;
    }
    const bool ok =
        guarded("VTableHook::writeEntry", [&] { vtable[slot] = value; });

    // THE WRITE AUTOPSY. Five years of this function assuming its own writes
    // land, and issue #21 is the rig that made the assumption worth testing:
    // there the slot is foreign again on the very next frame, every frame,
    // always holding the SAME original pointer -- one distinct entry through
    // eight thousand exchanges. That is not somebody selecting between
    // implementations. That is a restore, and there are two families of cause
    // this function has never been able to tell apart:
    //
    //   * somebody writes the original back after we write ours, or
    //   * our write does not survive, and nobody else is involved at all.
    //
    // Nothing here ever read the slot back, so the second family has never once
    // been looked at. Two reads settle it, and their PLACEMENT is the point:
    // one straight after the store while the page is still PAGE_READWRITE, one
    // after the protection is put back. If the value is ours at the first and
    // gone at the second, restoring the protection is what loses it -- a
    // one-line bug in here, not a war with the operating system.
    void* afterWrite = nullptr;
    guarded("VTableHook::writeEntry/read-back",
            [&] { afterWrite = vtable[slot]; });

    // Restore -- but never to a copy-on-write protection. Putting
    // PAGE_WRITECOPY or PAGE_EXECUTE_WRITECOPY back onto a page that has just
    // been made private by writing to it is the classic way to have the private
    // copy dropped and the image's original contents returned, which would look
    // exactly like the field report above. The non-copy equivalents leave the
    // page as readable and as executable as it was, and are what every other
    // hooking library restores. Only reached when a table lives in a mapped
    // image; the D3D11 context's table is heap-resident on the reporting rig,
    // where this changes nothing.
    DWORD restore = oldProtect;
    if (restore == PAGE_WRITECOPY)                 restore = PAGE_READONLY;
    else if (restore == PAGE_EXECUTE_WRITECOPY)    restore = PAGE_EXECUTE_READ;
    DWORD ignored = 0;
    VirtualProtect(&vtable[slot], sizeof(void*), restore, &ignored);

    void* afterRestore = nullptr;
    guarded("VTableHook::writeEntry/read-back-2",
            [&] { afterRestore = vtable[slot]; });

    if (ok && m_writeAutopsies < kWriteAutopsies &&
        (afterWrite != value || afterRestore != value)) {
        // Reported only when a write did NOT survive its own function. A write
        // that lands and stays needs no line, and the ordinary case must not
        // fill the log to prove it is ordinary.
        ++m_writeAutopsies;
        MEMORY_BASIC_INFORMATION mbi{};
        const bool haveMbi =
            VirtualQuery(&vtable[slot], &mbi, sizeof(mbi)) == sizeof(mbi);
        Log::get().note(
            "VTableHook: the %s write to slot %zu at %p DID NOT SURVIVE this "
            "function. Straight after the store the slot held %s; after the page "
            "protection was restored it held %s. Page: %s, %s, allocation base "
            "%p, protection was %s and was put back as %s. If the value was ours "
            "before the restore and not after, the restore is the bug; if it was "
            "already not ours before it, the store itself did not take. Said at "
            "most %u times.",
            why ? why : "?", slot, (void*)&vtable[slot],
            afterWrite == value ? "OURS" : "the ORIGINAL (not ours)",
            afterRestore == value ? "OURS" : "the ORIGINAL (not ours)",
            haveMbi ? (mbi.Type == MEM_IMAGE     ? "a mapped image"
                       : mbi.Type == MEM_MAPPED  ? "a mapped file or section"
                       : mbi.Type == MEM_PRIVATE ? "private memory (heap)"
                                                 : "an unnamed region")
                    : "unqueryable",
            haveMbi && mbi.State == MEM_COMMIT ? "committed" : "not committed",
            haveMbi ? mbi.AllocationBase : nullptr,
            protectName(oldProtect), protectName(restore),
            static_cast<unsigned>(kWriteAutopsies));
    }
    return ok;
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

    m_object = object;
    m_vtable = vt;
    m_patches.clear();
    return true;
}

bool VTableHook::setMode(HookMode mode) {
    // Before any staging: the two mechanisms record patches against different
    // tables (the shared one vs the private copy), so a switch after the first
    // replace() would leave patches describing a table we are no longer using.
    if (!m_object || m_committed || !m_patches.empty()) return false;
    if (mode == m_mode) return true;

    if (mode == HookMode::CopyVptr) {
        // Copy as wide a window as is readable, NOT just the executable
        // prefix. Stopping at the first non-code slot builds a table that
        // works until the host calls a method past the cut and reads off the
        // end of our buffer -- uninitialised heap, reproducing only on
        // teardown or a rare interface. Copy generously; let the tail be
        // whatever the original held. reserve() to the same width first, so
        // the vector never reallocates after commit points the vptr at it.
        m_copy.clear();
        m_copy.reserve(512);
        bool ok = true;
        for (size_t i = 0; i < 512; ++i) {
            void* entry = nullptr;
            if (!guarded("VTableHook::setMode/copy", [&] { entry = m_vtable[i]; })) break;
            m_copy.push_back(entry);
        }
        if (m_copy.size() < m_execPrefix) {
            m_copy.clear();
            ok = false;
        }
        if (!ok) return false;
    } else {
        m_copy.clear();
    }
    m_mode = mode;
    return true;
}

bool VTableHook::replace(size_t index, void* replacement, void** origOut) {
    if (!m_object || m_committed) return false;
    if (index >= m_execPrefix) return false;

    // The current entry is read through whichever table this mode dispatches
    // by: the shared vtable in place, our copy once CopyVptr has taken it.
    // Both answer "what a call on this object runs right now", which is what
    // the caller must forward to.
    void** table = (m_mode == HookMode::CopyVptr) ? m_copy.data() : m_vtable;
    void* original = nullptr;
    if (!guarded("VTableHook::replace/read-slot",
                 [&] { original = table[index]; })) {
        return false;
    }
    if (origOut) *origOut = original;
    Patch p;
    p.slot = index;
    p.replacement = replacement;
    p.original = original;
    p.origOut = origOut;
    m_patches.push_back(p);
    return true;
}

bool VTableHook::commit() {
    if (!m_object || m_committed) return false;
    if (m_patches.empty()) return false;

    if (m_mode == HookMode::CopyVptr) {
        // Patch the private copy, then point the object at it -- one aligned
        // pointer store, which cannot be partial, so there is no rollback
        // path to write. No registry entry and no reclaim: the copy is
        // unreachable by the table owners this whole registry exists to
        // arbitrate. See the header.
        for (const Patch& p : m_patches) {
            if (p.slot < m_copy.size()) m_copy[p.slot] = p.replacement;
        }
        void** target = reinterpret_cast<void**>(m_object);
        DWORD oldProtect = 0;
        if (!VirtualProtect(target, sizeof(void*), PAGE_READWRITE, &oldProtect)) {
            Log::get().note("VTableHook: VirtualProtect failed on object %p (err %lu)",
                            m_object, GetLastError());
            return false;
        }
        const bool ok = guarded("VTableHook::commit/vptr", [&] {
            *target = reinterpret_cast<void*>(m_copy.data());
        });
        DWORD ignored = 0;
        VirtualProtect(target, sizeof(void*), oldProtect, &ignored);
        if (!ok) return false;
        m_committed = true;
        return true;
    }

    size_t written = 0;
    for (; written < m_patches.size(); ++written) {
        const Patch& p = m_patches[written];
        if (!writeEntry(m_vtable, p.slot, p.replacement, "install")) break;
    }
    if (written < m_patches.size()) {
        // Partial patch is worse than none: a half-installed fix is a fix
        // whose invariants nobody has reasoned about. Put back what went in.
        for (size_t i = 0; i < written; ++i) {
            const Patch& p = m_patches[i];
            writeEntry(m_vtable, p.slot, p.original, "roll back");
        }
        Log::get().note("VTableHook: %zu of %zu entries could not be patched at "
                        "%p, so all of them were rolled back and the fix is off.",
                        m_patches.size() - written, m_patches.size(),
                        (void*)m_vtable);
        m_patches.clear();
        return false;
    }
    m_committed = true;

    // Register what was committed, so reclaim() -- on ANY hook sharing this
    // table -- can tell one of ours from an intruder. Registration failing is
    // survivable (the hooks work; only reclaim is refused for them), but it
    // must not be silent, because "reclaim never fires" and "nothing ever
    // clobbered us" read identically in a log.
    {
        AcquireSRWLockExclusive(&g_slotOwnersLock);
        bool full = false;
        for (const Patch& p : m_patches) {
            if (g_slotOwnerCount >= kMaxSlotOwners) {
                full = true;
                break;
            }
            SlotOwner& o = g_slotOwners[g_slotOwnerCount++];
            o.vtable = m_vtable;
            o.slot = p.slot;
            o.replacement = p.replacement;
            o.owner = this;
        }
        if (full) {
            // Poison reclaim OUTRIGHT, for everyone. Retiring only this hook
            // was tried on paper and refuted: its patches stay in the table,
            // its shared-slot co-owner cannot see them in the registry, reads
            // them as an intruder, adopts one as a forward -- and that is the
            // call loop the registry exists to prevent, built by the fallback.
            // A session without reclaim is the pre-reclaim status quo; a
            // session with a loop is a crash.
            g_slotOwnersPoisoned = true;
            Log::get().note(
                "VTableHook: the slot registry is full, so re-claiming is OFF "
                "for every hook this session -- a hook the registry cannot see "
                "must not be re-claimed around, so none may be. The hooks "
                "themselves still work. This means more hooks exist than the "
                "codebase has ever had; raise kMaxSlotOwners in vtable_hook.cpp.",
                (void*)m_vtable);
        }
        ReleaseSRWLockExclusive(&g_slotOwnersLock);
    }
    return true;
}

void VTableHook::uninstall() {
    if (!m_object) return;

    if (m_mode == HookMode::CopyVptr) {
        if (m_committed) {
            // Restore the vptr this hook found at attach -- but ONLY if the
            // object still dispatches through OUR copy, mirroring the polite
            // in-place uninstall below. For stacked copy hooks unwound in
            // reverse install order (the shipped order), the object does still
            // point at our copy, so this restores the copy underneath and the
            // stack peels as it was built.
            //
            // If the object points SOMEWHERE ELSE, a later tool swapped the
            // vptr on top of us and it belongs to them now: writing our stale
            // m_vtable would cut them out, and worse, they may hold OUR copy as
            // their restore target -- which m_copy.clear() below is about to
            // free. Leaving their vptr alone keeps their chain (which still
            // runs through our copy) intact and order-independent, instead of
            // correct only because shutdown happens to run upper-first.
            void** target = reinterpret_cast<void**>(m_object);
            void*  live = nullptr;
            guarded("VTableHook::uninstall/vptr-read",
                    [&] { live = *target; });
            if (live == static_cast<void*>(m_copy.data())) {
                DWORD oldProtect = 0;
                if (VirtualProtect(target, sizeof(void*), PAGE_READWRITE, &oldProtect)) {
                    guarded("VTableHook::uninstall/vptr", [&] {
                        *target = reinterpret_cast<void*>(m_vtable);
                    });
                    DWORD ignored = 0;
                    VirtualProtect(target, sizeof(void*), oldProtect, &ignored);
                }
                // Restored: nothing dispatches through our copy any more, so
                // it can go.
                m_committed = false;
                forgetObject();
                m_copy.clear();
                return;
            }
            // Swapped away by a later tool: leave THEIR vptr, and DELIBERATELY
            // LEAK our copy rather than free it. Something is still dispatching
            // through it -- their chain forwards into it, or they hold its
            // address as their own restore target -- and freeing it here is a
            // dangling-vptr crash on the next call. A one-time leak of a few KB
            // on a teardown that only ever runs under FreeLibrary (the game
            // exits by TerminateProcess) is the right trade. m_copy is left
            // intact and this object is abandoned in place.
            Log::get().note(
                "VTableHook: a copy-mode object was swapped away from EDVR's "
                "vtable copy by another tool before uninstall. The vptr is left "
                "with them and EDVR's copy is intentionally leaked rather than "
                "freed, because their chain still runs through it -- freeing it "
                "would dangle. One-time, teardown only.");
            m_committed = false;
        }
        // NOT m_copy.clear() on the leak path -- see the note above. Reached
        // only when the committed branch fell through the swapped-away case,
        // or when the hook was never committed (m_copy already empty).
        forgetObject();
        return;
    }

    if (m_committed) {
        // Reverse order, so that where we patched one slot twice the chain
        // unwinds the way it was built.
        for (size_t i = m_patches.size(); i-- > 0;) {
            const Patch& p = m_patches[i];
            void* now = nullptr;
            if (!guarded("VTableHook::uninstall/read-slot",
                         [&] { now = m_vtable[p.slot]; })) {
                continue;
            }
            if (now != p.replacement) {
                // Somebody hooked this slot after us. Their entry is the live
                // one and restoring ours would delete their hook; our thunk
                // stays reachable through whatever they forward to.
                Log::get().note("VTableHook: slot %zu at %p was hooked after us, "
                                "so it is being left alone rather than restored.",
                                p.slot, (void*)m_vtable);
                continue;
            }
            writeEntry(m_vtable, p.slot, p.original, "uninstall");
        }
        m_committed = false;
    }
    // Out of the registry AFTER the slots are restored, and the order is
    // load-bearing. Walked both ways for the shared slot (ClearState, two
    // hooks stacked): with restore-first, a co-owner's reclaim pass that lands
    // mid-uninstall sees either the full stack (both registered, our thunk on
    // top -- healthy) or the restored slot (its own thunk on top -- healthy).
    // Deregister-first opens a window where the slot still holds OUR thunk but
    // the registry no longer names us: the co-owner reads it as an intruder,
    // adopts our thunk -- whose forward already points at the co-owner -- and
    // that is the call loop, built by the teardown path. (This whole race
    // needs FreeLibrary during live rendering, which a closing game never
    // does; the order is right anyway, because the comment claiming safety is
    // what the next reader will trust.)
    {
        AcquireSRWLockExclusive(&g_slotOwnersLock);
        size_t kept = 0;
        for (size_t i = 0; i < g_slotOwnerCount; ++i) {
            if (g_slotOwners[i].owner != this) g_slotOwners[kept++] = g_slotOwners[i];
        }
        g_slotOwnerCount = kept;
        ReleaseSRWLockExclusive(&g_slotOwnersLock);
    }
    forgetObject();
}

// Everything this hook believed about the object it has just let go.
//
// One function because uninstall() has THREE exits -- copy-mode restored,
// copy-mode swapped-away-and-leaked, and in-place -- and the first cut of the
// new state cleared it on only the last of them, so a copy-mode uninstall left
// m_implModule set and a later attach() to a different object inherited an
// assertion nobody had made about it. That assertion waives the chainer gate,
// so inheriting it is the difference between a refusal and an adoption.
// Neither m_committed nor m_copy is touched here: the three exits disagree
// about both, deliberately, and that disagreement is the whole reason they are
// three exits.
void VTableHook::forgetObject() {
    m_object = nullptr;
    m_vtable = nullptr;
    m_execPrefix = 0;
    m_patches.clear();
    m_reclaimEvents = 0;
    m_implModule = nullptr;
    m_copyDriftEvents = 0;
    m_ownerRepointNoted = false;
    m_copyBreachNoted = false;
    m_lastDisplaced = 0;
    m_lastConceded = 0;
    m_lastPassRan = false;
}

// The counterpart to reclaim() for the mode that has no war to fight: does the
// table we COPIED still say what the live one says?
//
// CopyVptr's entire claim is immunity -- the runtime may re-point its shared
// table as often as it likes, because our object no longer dispatches through
// it. That claim rests on a premise this file states nowhere and has never
// measured: that the entries a runtime swaps BETWEEN are interchangeable, so a
// frozen snapshot stays as good as the live table for the whole session. If
// they are not -- if a swap is the runtime moving its object onto a different
// implementation because its own state now requires that one -- then a private
// copy is not immunity. It is a snapshot of a table that has since moved on,
// and the game is calling last second's implementation with this second's
// state, which is the kind of mismatch that ends in a dead GPU rather than a
// wrong pixel.
//
// Issue #21 is what an unmeasured premise looks like from the field: a rig
// where the context table is re-pointed about once a second, where every
// in-place release survives and every copy release dies a second and a half
// after the hooks arm -- and where the only way anyone could find that out was
// to install five years of releases in order, because this class reported
// nothing at all about the mode whose immunity it was asserting.
//
// So: read-only, once per reclaim pass. How far our copy has drifted from the
// table it was copied from, and whose code that table points at now. It heals
// nothing. What to DO about drift depends entirely on who is causing it, and
// that name is the fact this line exists to supply.
//
// TWO LIMITS, both deliberate, both worth knowing before reading a report:
//
//   * STACKED COPY HOOKS ONLY MEASURE THE BOTTOM ONE. When two hooks take the
//     same object in copy mode, the second attaches to an object whose vptr the
//     first already moved, so its m_vtable IS the first hook's private buffer.
//     Nothing else writes that buffer, so the upper hook's drift is structurally
//     zero for the session. In the d3d11 half that is the exposure hook
//     underneath and vScreen on top, so the exposure hook's line is the real
//     measurement and vScreen's silence says nothing at all. Do not read it as
//     "vScreen's slots were stable".
//   * THE SPAN IS THE PROBE PREFIX, NOT THE INTERFACE. m_execPrefix walks until
//     an entry stops looking like code, which on a D3D11 context runs to about
//     300 -- past the ~150 methods the interface has, into whatever the module
//     put next in .rdata. Entries beyond the interface are reported honestly as
//     entries, and the message says "for any of those that is a method of this
//     interface", because a slot the object has no method at cannot be called
//     through and is not evidence of anything.
void VTableHook::noteCopyDrift(const char* who) {
    size_t drifted = 0;
    void*  firstNow = nullptr;
    // Wider than reclaim's list, because this walks the whole probe prefix
    // rather than the two dozen slots a caller patched, and a slot list that
    // truncates does not merely stop early: strncat_s cuts mid-number, so
    // ", 19" becomes ", 1" and the line names a slot that never drifted. The
    // count above it is the honest total either way, and appending stops
    // cleanly at the ellipsis rather than trailing off.
    char   slots[512];
    slots[0] = '\0';

    const size_t span = m_execPrefix < m_copy.size() ? m_execPrefix : m_copy.size();
    for (size_t i = 0; i < span; ++i) {
        // What this slot held when we copied it. Our patches overwrote the
        // copy, so for a patched slot that value lives in the Patch and not in
        // m_copy -- comparing the live table against our own thunk would report
        // every hooked slot as drift, every pass, forever.
        void* copied = m_copy[i];
        for (const Patch& p : m_patches) {
            if (p.slot == i) {
                copied = p.original;
                break;
            }
        }
        void* now = nullptr;
        if (!guarded("VTableHook::noteCopyDrift/read-slot",
                     [&] { now = m_vtable[i]; })) {
            break;
        }
        if (now == copied) continue;
        if (!drifted) firstNow = now;
        ++drifted;
        appendSlot(slots, sizeof(slots), i);
    }
    if (!drifted) return;

    ++m_copyDriftEvents;
    char modBuf[MAX_PATH];
    if (m_copyDriftEvents == 1) {
        Log::get().note(
            "VTableHook %s: the vtable EDVR copied has DRIFTED from the table it "
            "was copied FROM -- %zu of the first %zu entries (%s) now hold "
            "something else, and the first of them points into %s. EDVR's copy "
            "still holds what they held at install, and this object dispatches "
            "through the copy, so for any of those that is a method of this "
            "interface, the game is calling the older entry. That is what the "
            "private-copy mode is FOR when the two are interchangeable, and a "
            "genuine hazard when they are not. Nothing was changed. This check "
            "repeats about once a second and reports again at doublings; if this "
            "log ends in a crash, this line is the one to report.",
            who, drifted, span, slots,
            ownerModuleName(firstNow, modBuf, sizeof(modBuf)));
    } else if ((m_copyDriftEvents & (m_copyDriftEvents - 1)) == 0) {
        Log::get().note(
            "VTableHook %s: copy drift #%u (%zu slot(s): %s; first points into "
            "%s).",
            who, m_copyDriftEvents, drifted, slots,
            ownerModuleName(firstNow, modBuf, sizeof(modBuf)));
    }
}

size_t VTableHook::reclaim(const char* name, const size_t* quietSlots,
                           size_t quietCount) {
    // Answered fresh by every pass, including the passes that re-patch nothing:
    // a caller sampling it once a frame is asking "were the hooks in the table
    // just now", and a stale answer from the last pass that DID something would
    // report the opposite of the truth. Zeroed before the early returns too --
    // an uncommitted or copy-mode hook has no displacement to report, and
    // leaving yesterday's number there would be worse than saying nothing.
    m_lastDisplaced = 0;
    m_lastConceded = 0;
    m_lastPassRan = false;
    if (!m_committed) return 0;
    if (m_mode == HookMode::CopyVptr) {
        // No war to fight -- but VERIFY the immunity rather than assume it,
        // because a silent grey void is the exact failure this project exists
        // to end. Two things must still hold: the object dispatches through
        // OUR copy, and our copy still holds our thunks. If the object's vptr
        // was swapped away, a later tool copy-hooked on top (fine, it chains
        // through us). If our copy's slots were overwritten, something
        // re-derived the vtable from the live object and wrote through it --
        // which would mean CopyVptr does NOT dodge this re-pointer, and the
        // reader needs to know that in words, once, instead of inferring it
        // from a grey void.
        const char* who = name ? name : "?";
        void** live = nullptr;
        guarded("VTableHook::reclaim/copy-vptr-read", [&] {
            live = *reinterpret_cast<void***>(m_object);
        });
        if (live == m_copy.data()) {
            for (const Patch& p : m_patches) {
                if (p.slot < m_copy.size() && m_copy[p.slot] != p.replacement &&
                    !m_copyBreachNoted) {
                    m_copyBreachNoted = true;
                    char modBuf[MAX_PATH];
                    // Deliberately NOT "the fix is bypassed": from in here we
                    // cannot tell the two tools apart. A later hooker that
                    // CHAINED through us captured our thunk as its forward, so
                    // slot != replacement yet our thunk still runs and both
                    // compose. One that resolved a CLEAN original does bypass
                    // us. Only the fixes' own output says which, so the line
                    // points there instead of asserting breakage and
                    // manufacturing a false report.
                    Log::get().note(
                        "VTableHook %s: slot %zu in EDVR's private vtable copy "
                        "was overwritten by another tool (now %s). If that tool "
                        "chained through EDVR the fixes still run and this is "
                        "harmless; if it resolved a clean original they are "
                        "bypassed. The totals lines say which -- report this "
                        "log only if the fixes reading this call have actually "
                        "gone quiet. Said once.",
                        who, p.slot,
                        ownerModuleName(m_copy[p.slot], modBuf, sizeof(modBuf)));
                }
            }
        }
        // OUTSIDE the "are we still the object's vptr" test, and that placement
        // is the whole difference between this instrument working and not.
        //
        // It was inside, and in the shipped d3d11 stack that made it dead code.
        // Two copy hooks stack on one context: exposure commits first, vScreen
        // on top. For the LOWER hook the object's vptr is now the UPPER hook's
        // copy, so `live == m_copy.data()` is false and the walk never ran --
        // and the lower hook is the only one whose m_vtable is the runtime's
        // real shared table, so it is the only one that could have measured
        // anything. The upper hook, whose test did pass, was comparing against
        // the lower hook's private buffer, which nothing ever writes. Between
        // them the instrument reported "no drift" on exactly the rig it was
        // written to diagnose, which is the failure mode this file keeps
        // promising not to produce.
        noteCopyDrift(who);
        return 0;
    }
    // Not measured, so not counted. lastPassDisplaced() would otherwise read
    // zero here and a caller sampling it per frame would score a hook nobody
    // patrolled as perfectly held.
    if (g_slotOwnersPoisoned) return 0;   // see the flag's comment
    m_lastPassRan = true;
    const char* who = name ? name : "?";

    size_t reclaimed = 0;
    size_t ownerReclaimed = 0;   // of those, taken back from the module's own re-point
    // Sized past the two dozen slots the d3d11 half patches, because at 96 a
    // full pass rendered 94 of the 95 usable bytes and one more hooked slot
    // would have started truncating. appendSlot cannot cut mid-number, but a
    // list that ends in an ellipsis on an ordinary pass is still a worse report
    // than one that fits.
    char slots[512];
    slots[0] = '\0';
    // Who we chained to, for the report below. One name is enough: multiple
    // intruders on one hook's slots in one pass has never been seen, and the
    // per-slot detection lines carry their own names if it ever is.
    char        adoptedModBuf[MAX_PATH];
    const char* adoptedMod = nullptr;
    // The implementation module's own name, kept apart from adoptedMod so the
    // owner-repoint line can never be handed a rival tool's path.
    char        ownerModBuf[MAX_PATH];
    const char* ownerMod = nullptr;

    for (Patch& p : m_patches) {
        void* now = nullptr;
        if (!guarded("VTableHook::reclaim/read-slot", [&] { now = m_vtable[p.slot]; })) {
            continue;
        }
        if (now == p.replacement) continue;   // healthy: we are on top

        // Is the current entry another EDVR hook's replacement, and how many
        // of ours share this slot? Both answers come from the registry; see
        // its comment for the loop that reading them wrongly builds.
        bool   oursOnTop = false;
        size_t owners = 0;
        AcquireSRWLockShared(&g_slotOwnersLock);
        for (size_t i = 0; i < g_slotOwnerCount; ++i) {
            const SlotOwner& o = g_slotOwners[i];
            if (o.vtable != m_vtable || o.slot != p.slot) continue;
            ++owners;
            if (o.replacement == now) oursOnTop = true;
        }
        ReleaseSRWLockShared(&g_slotOwnersLock);

        if (oursOnTop) continue;   // healthy: a co-owner is on top, we are in its chain

        // CONCEDED versus CONTESTED, and they are counted apart because the
        // duty-cycle figure is worthless if they are not.
        //
        // A retired slot and a shared slot are permanent, known, already-logged
        // losses. Counting them as displacement means the figure reads zero on
        // every frame for the rest of the session, and on issue #21's rig that
        // is exactly what happens: ClearState is hooked by both context hooks,
        // it is conceded on the first pass, and a "held 0% of frames" line would
        // then hide the 23 slots that ARE being healed within a frame -- the
        // measurement reporting total failure while the fix works. Contested is
        // the number the caller wants: slots we are still fighting for and did
        // not have this instant.
        if (p.retired || owners > 1) ++m_lastConceded;
        else                         ++m_lastDisplaced;

        if (p.retired) continue;   // conceded earlier; the intruder keeps it

        // Whether the writer is the image the caller named as implementing
        // these methods.
        //
        // Independent of the vouch, not gated behind it: a slot can be both
        // vouched and an owner re-point, and computing this as `!vouched && ...`
        // meant that when the once-a-second pass happened to reach a slot first,
        // the runtime's own re-point was booked against the tug-of-war cap and
        // retired after 64 exchanges -- the exact concession the exemption
        // exists to prevent, arriving by the back door on whichever slots the
        // vouch got to first.
        //
        // Cached per slot, because this is a VirtualQuery and the pass now runs
        // every frame. A foreign entry that nobody is going to heal (an
        // unvouched chainer, a slot conceded upstream) would otherwise buy a
        // syscall per frame for the session to re-derive an answer that has not
        // changed. The entry is the cache key: a new pointer is re-classified.
        if (now != p.lastForeign) {
            p.lastForeign = now;
            p.lastForeignIsOwner = m_implModule && owningModule(now) == m_implModule;
            // The variant census, recorded on the same rare path -- see the
            // Patch field. Only entries we have not seen in this slot before.
            bool known = false;
            for (uint8_t i = 0; i < p.seenCount; ++i) {
                if (p.seen[i] == now) { known = true; break; }
            }
            if (!known) {
                if (p.seenCount < 4) p.seen[p.seenCount++] = now;
                else                 p.seenOverflow = true;
            }
        }
        const bool ownerRepoint = p.lastForeignIsOwner;

        bool vouched = ownerRepoint;
        for (size_t i = 0; i < quietCount && !vouched; ++i) {
            if (quietSlots[i] == p.slot) vouched = true;
        }

        if (owners > 1) {
            // Two of ours underneath, a stranger on top. Whichever of us
            // re-patched alone would splice the other out of the chain, so
            // neither does. Said once per owner, because the names differ and
            // both features are the ones going quiet.
            //
            // THE EXEMPTION DOES NOT REACH HERE, and that is a known gap rather
            // than an oversight. Restoring a shared slot means rebuilding a
            // STACK -- slot -> upper thunk -> lower thunk -> the new entry --
            // and no single hook holds both halves: the lower owner's forward
            // must become the runtime's new entry while only the upper owner
            // may write the slot. One hook acting alone gets it wrong in either
            // direction. So on issue #21's rig the 28 unshared slots heal and
            // ClearState (110, hooked by both the exposure and vScreen
            // contexts) does not, which costs the binding shadow its
            // ClearState notifications. The repair needs the registry to drive
            // both owners in one pass; the line below at least stops calling
            // the operating system "another tool" while it waits.
            if (!p.sharedNoted) {
                p.sharedNoted = true;
                char modBuf[MAX_PATH];
                Log::get().note(
                    "VTableHook %s: slot %zu was re-pointed by %s (%s), and it "
                    "is a slot TWO EDVR hooks share -- re-patching it from "
                    "either one would cut the other out of the chain, so it is "
                    "left as it is and whatever reads this call stays bypassed. "
                    "Report this log.",
                    who, p.slot,
                    ownerRepoint ? "the module that implements it"
                                 : "another tool",
                    ownerModuleName(now, modBuf, sizeof(modBuf)));
            }
            continue;
        }

        // THE CHAINER GATE. "Not our pointer in the slot" describes two
        // opposite tools: a bypasser (resolved a clean original; our thunk is
        // starved -- the thing to heal) and a chainer (captured our thunk as
        // its forward; our thunk still runs -- the thing to leave alone,
        // because adopting a chainer's entry points the two hooks at each
        // other and the next call is a stack overflow). The slot's own call
        // traffic is USUALLY the only fact that tells them apart, the caller is
        // the only one who has it, and quietSlots is how it vouches. No vouch,
        // no re-patch -- the note below is what a starved-but-unvouched slot
        // leaves behind instead of silence.
        //
        // The exception is setImplementationModule: see its comment. When the
        // caller has named the image that implements these methods, an entry
        // arriving FROM that image is that image re-selecting its own internals,
        // not a rival hook, and it is adopted with no traffic evidence at all --
        // which is the whole point, because issue #21's rig is one where traffic
        // evidence is unobtainable by construction. Both `ownerRepoint` and
        // `vouched` were settled above, before the shared-slot branch that also
        // reports which of the two it is conceding to.
        if (!vouched) {
            if (!p.foreignNoted) {
                p.foreignNoted = true;
                char modBuf[MAX_PATH];
                Log::get().note(
                    "VTableHook %s: slot %zu no longer holds EDVR's hook -- it "
                    "now points into %s. It is NOT being taken back: EDVR only "
                    "re-patches slots whose own calls have measurably gone "
                    "quiet, and this one has no such evidence -- a tool that "
                    "CHAINS through EDVR still runs us, and re-patching over a "
                    "chainer builds a call loop. If the fixes reading this call "
                    "have gone quiet, report this log. Said once.",
                    who, p.slot, ownerModuleName(now, modBuf, sizeof(modBuf)));
            }
            continue;
        }

        if (!p.origOut) {
            // No forward pointer was registered, so our thunk cannot be told
            // to chain to the intruder -- re-patching would silently drop
            // their hook, which is what was just done to us.
            p.retired = true;
            Log::get().note(
                "VTableHook %s: slot %zu was re-pointed and cannot be "
                "re-claimed -- replace() was given no forward pointer for it, "
                "so chaining in front of the new entry is impossible. Left "
                "alone.",
                who, p.slot);
            continue;
        }

        // Chaining to something that is not code would turn the next call into
        // a jump into data. Expected only during teardown, but "expected" has
        // been wrong in this file before -- so it is said once rather than
        // never, and skipped without conceding, so a transient bad read does
        // not retire the slot for the session.
        if (!isExecutableAddress(now)) {
            if (!p.oddNoted) {
                p.oddNoted = true;
                Log::get().note(
                    "VTableHook %s: slot %zu holds a value that is not "
                    "executable code, which nothing sane writes into a live "
                    "vtable. EDVR will not chain to it and leaves the slot "
                    "alone. Said once.",
                    who, p.slot);
            }
            continue;
        }

        // Adopt the intruder as the new forward target, THEN take the slot
        // back. Between the two writes every call still takes the intruder's
        // path exactly as it did before this pass; after both, calls run our
        // thunk, then the intruder's, then whatever it captured. A call
        // in-flight through our thunk from before the clobber can read the
        // old forward and skip the intruder once -- one call, theirs, and
        // nothing dangling.
        *p.origOut = now;
        p.original = now;   // uninstall now politely restores THEIR entry

        // The attempt counts toward the cap whether or not the write lands.
        // writeEntry failing repeatably (protection interference) used to mean
        // an unbounded retry -- and an unbounded copy of writeEntry's own
        // failure line, once a second, for the session. Bounded by the same
        // cap as success: 64 lines at the very worst, then concession.
        //
        // EXCEPT when the re-pointer is the module that owns the entry AND the
        // write lands. The cap exists to end a tug-of-war with a TOOL: two
        // hookers trading one slot every second serve nobody, and somebody has
        // to stop. An image maintaining its own vtable is not that. It will not
        // tire, it will not negotiate, and conceding to it does not restore any
        // other tool's function -- it just switches EDVR off after a minute on a
        // rig where everything was working. A successful exchange costs one
        // aligned pointer write per pass, which is nothing, so it is allowed to
        // run for the session with the log's doubling cadence keeping it
        // visible.
        //
        // A FAILED write is a different animal and stays capped, owner or not.
        // writeEntry logs every time VirtualProtect refuses it, so exempting
        // failures would restore exactly the unbounded-log regression the cap
        // was added for -- one line per contested slot per second, forever, and
        // on issue #21's rig that is 29 of them. Failing repeatably also means
        // the forward has been moved while our thunk is NOT in the slot, which
        // is a state to escape rather than re-enter for the session.
        const bool wrote = writeEntry(m_vtable, p.slot, p.replacement, "re-claim");
        if (!ownerRepoint || !wrote) ++p.repatches;

        if (p.repatches >= kMaxRepatchesPerSlot) {
            p.retired = true;
            char modBuf[MAX_PATH];
            Log::get().note(
                "VTableHook %s: slot %zu has been fought over %u times -- the "
                "re-taker is %s. Either it re-checks its hooks the way EDVR "
                "does, or EDVR's own write into the slot keeps failing; the "
                "lines above say which. Trading one slot back and forth serves "
                "nobody, so EDVR will not contest the NEXT re-point: when it "
                "comes, whatever reads this call goes quiet for good. Report "
                "this log.",
                who, p.slot, p.repatches,
                ownerModuleName(now, modBuf, sizeof(modBuf)));
        }
        if (!wrote) continue;

        ++reclaimed;
        adoptedMod = ownerModuleName(now, adoptedModBuf, sizeof(adoptedModBuf));
        if (ownerRepoint) {
            // Named from THIS slot, not from whatever the pass happened to
            // adopt last. A pass can mix the two kinds -- the runtime taking
            // one slot while a rival tool takes another -- and sharing one name
            // between the two reports would put a rival tool's path into the
            // sentence "the module that implements them", which is a false
            // statement about somebody else's software.
            ++ownerReclaimed;
            ownerMod = ownerModuleName(now, ownerModBuf, sizeof(ownerModBuf));
        }
        appendSlot(slots, sizeof(slots), p.slot);
    }

    if (reclaimed) {
        ++m_reclaimEvents;
        if (m_reclaimEvents == 1) {
            // Two writers, two different sentences. Every slot coming from the
            // implementation module is that module re-selecting its own
            // internals; calling that "another tool" which "resolved its own
            // original pointers" describes an intent the operating system does
            // not have, and issue #21's report quotes exactly this line with
            // system32\d3d11.dll in it.
            const bool allOwner = ownerReclaimed == reclaimed;
            Log::get().note(
                "VTableHook %s: %zu slot(s) (%s) had been re-pointed by %s -- %s "
                "-- after EDVR installed, so EDVR's hooks there had stopped "
                "running. EDVR re-patched on top and now forwards to the new "
                "entries, so BOTH run. This check repeats every frame and "
                "reports again at doublings.",
                who, reclaimed, slots,
                allOwner ? "the module that implements them" : "another tool",
                allOwner ? (ownerMod ? ownerMod : "?")
                         : (adoptedMod ? adoptedMod : "?"));
        } else if ((m_reclaimEvents & (m_reclaimEvents - 1)) == 0) {
            // The variant census rides the doubling line, because it is the
            // number that decides whether patrolling this table is the best
            // EDVR can do or merely the cheapest. See Patch::seen.
            uint8_t widest = 0;
            bool    overflowed = false;
            for (const Patch& q : m_patches) {
                if (q.seenCount > widest) widest = q.seenCount;
                if (q.seenOverflow) overflowed = true;
            }
            Log::get().note(
                "VTableHook %s: reclaim #%u (slot(s) %s, taken by %s). The "
                "busiest slot has been through %u distinct entr%s so far%s.",
                who, m_reclaimEvents, slots, adoptedMod ? adoptedMod : "?",
                widest, widest == 1 ? "y" : "ies",
                overflowed ? " and at least one slot has had more than four, so "
                             "the set is not small"
                           : " -- a small set means every one of them could be "
                             "hooked directly, which would end this exchange "
                             "for good");
        }
        // Said separately the first time it happens, because it is a different
        // event with a different meaning: not a rival tool bypassing us, but the
        // module that implements these methods swapping its own table under
        // everybody. It is the one kind of re-point EDVR adopts without any
        // traffic evidence, and the one it will keep adopting for the whole
        // session rather than conceding after 64 rounds -- so a reader who ever
        // wonders why this hook never gives up should find the answer here.
        if (ownerReclaimed && !m_ownerRepointNoted) {
            m_ownerRepointNoted = true;
            Log::get().note(
                "VTableHook %s: %zu of those were re-pointed BY THE MODULE THAT "
                "IMPLEMENTS THEM (%s), which is that module re-selecting its own "
                "internal variants and not another tool at all. EDVR was told which "
                "image implements this object, and an entry coming FROM it cannot "
                "be a hook chaining through EDVR -- another tool's hook is that "
                "tool's own code -- so "
                "no quiet-slot evidence is required to take it back, and EDVR will "
                "keep taking it back for as long as it keeps happening. Issue #21: "
                "on a rig where this happens to EVERY patched slot before the "
                "first frame, waiting for that evidence meant waiting forever, and "
                "every fix in this DLL sat inert with the log dutifully explaining "
                "why.",
                who, ownerReclaimed, ownerMod ? ownerMod : "?");
        }
    }
    return reclaimed;
}

}  // namespace edvr
