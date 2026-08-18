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

}  // namespace

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

bool VTableHook::writeEntry(void** vtable, size_t slot, void* value) {
    DWORD oldProtect = 0;
    if (!VirtualProtect(&vtable[slot], sizeof(void*), PAGE_READWRITE, &oldProtect)) {
        Log::get().note("VTableHook: could not make vtable slot %zu at %p "
                        "writable (err %lu)", slot, (void*)&vtable[slot],
                        GetLastError());
        return false;
    }
    const bool ok =
        guarded("VTableHook::writeEntry", [&] { vtable[slot] = value; });
    DWORD ignored = 0;
    VirtualProtect(&vtable[slot], sizeof(void*), oldProtect, &ignored);
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
        if (!writeEntry(m_vtable, p.slot, p.replacement)) break;
    }
    if (written < m_patches.size()) {
        // Partial patch is worse than none: a half-installed fix is a fix
        // whose invariants nobody has reasoned about. Put back what went in.
        for (size_t i = 0; i < written; ++i) {
            const Patch& p = m_patches[i];
            writeEntry(m_vtable, p.slot, p.original);
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
                m_object = nullptr;
                m_vtable = nullptr;
                m_execPrefix = 0;
                m_patches.clear();
                m_copy.clear();
                m_reclaimEvents = 0;
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
        m_object = nullptr;
        m_vtable = nullptr;
        m_execPrefix = 0;
        m_patches.clear();
        // NOT m_copy.clear() on the leak path -- see the note above. Reached
        // only when the committed branch fell through the swapped-away case,
        // or when the hook was never committed (m_copy already empty).
        m_reclaimEvents = 0;
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
            writeEntry(m_vtable, p.slot, p.original);
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
    m_object = nullptr;
    m_vtable = nullptr;
    m_execPrefix = 0;
    m_patches.clear();
    m_reclaimEvents = 0;
}

size_t VTableHook::reclaim(const char* name, const size_t* quietSlots,
                           size_t quietCount) {
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
        return 0;
    }
    if (g_slotOwnersPoisoned) return 0;   // see the flag's comment
    const char* who = name ? name : "?";

    size_t reclaimed = 0;
    char slots[96];
    slots[0] = '\0';
    // Who we chained to, for the report below. One name is enough: multiple
    // intruders on one hook's slots in one pass has never been seen, and the
    // per-slot detection lines carry their own names if it ever is.
    char        adoptedModBuf[MAX_PATH];
    const char* adoptedMod = nullptr;

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
        if (p.retired) continue;   // conceded earlier; the intruder keeps it

        if (owners > 1) {
            // Two of ours underneath, a stranger on top. Whichever of us
            // re-patched alone would splice the other out of the chain, so
            // neither does. Said once per owner, because the names differ and
            // both features are the ones going quiet.
            if (!p.sharedNoted) {
                p.sharedNoted = true;
                char modBuf[MAX_PATH];
                Log::get().note(
                    "VTableHook %s: slot %zu was re-pointed by another tool "
                    "(%s), and it is a slot TWO EDVR hooks share -- re-patching "
                    "it from either one would cut the other out of the chain, "
                    "so it is left with the other tool and whatever reads this "
                    "call stays bypassed. Report this log.",
                    who, p.slot, ownerModuleName(now, modBuf, sizeof(modBuf)));
            }
            continue;
        }

        // THE CHAINER GATE. "Not our pointer in the slot" describes two
        // opposite tools: a bypasser (resolved a clean original; our thunk is
        // starved -- the thing to heal) and a chainer (captured our thunk as
        // its forward; our thunk still runs -- the thing to leave alone,
        // because adopting a chainer's entry points the two hooks at each
        // other and the next call is a stack overflow). The slot's own call
        // traffic is the only fact that tells them apart, the caller is the
        // only one who has it, and quietSlots is how it vouches. No vouch, no
        // re-patch -- the note below is what a starved-but-unvouched slot
        // leaves behind instead of silence.
        bool vouched = false;
        for (size_t i = 0; i < quietCount; ++i) {
            if (quietSlots[i] == p.slot) { vouched = true; break; }
        }
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
        ++p.repatches;
        const bool wrote = writeEntry(m_vtable, p.slot, p.replacement);

        if (p.repatches >= kMaxRepatchesPerSlot) {
            p.retired = true;
            char modBuf[MAX_PATH];
            Log::get().note(
                "VTableHook %s: slot %zu has been fought over %u times -- the "
                "re-taker is %s, and it re-checks its hooks the way EDVR does. "
                "A tug-of-war every second serves nobody, so EDVR will not "
                "contest the NEXT re-point: when it comes, whatever reads this "
                "call goes quiet for good. Report this log.",
                who, p.slot, p.repatches,
                ownerModuleName(now, modBuf, sizeof(modBuf)));
        }
        if (!wrote) continue;

        ++reclaimed;
        adoptedMod = ownerModuleName(now, adoptedModBuf, sizeof(adoptedModBuf));
        char one[16];
        _snprintf_s(one, sizeof(one), _TRUNCATE, "%s%zu", slots[0] ? ", " : "",
                    p.slot);
        strncat_s(slots, sizeof(slots), one, _TRUNCATE);
    }

    if (reclaimed) {
        ++m_reclaimEvents;
        if (m_reclaimEvents == 1) {
            Log::get().note(
                "VTableHook %s: %zu slot(s) (%s) had been re-pointed by another "
                "tool -- %s -- after EDVR installed. It resolved its own "
                "\"original\" pointers instead of chaining through the slot, so "
                "EDVR's hooks there were silently bypassed. EDVR re-patched on "
                "top and now forwards to the other tool's entries, so BOTH run. "
                "This check repeats about once a second and reports again at "
                "doublings.",
                who, reclaimed, slots, adoptedMod ? adoptedMod : "?");
        } else if ((m_reclaimEvents & (m_reclaimEvents - 1)) == 0) {
            Log::get().note("VTableHook %s: reclaim #%u (slot(s) %s, taken by %s).",
                            who, m_reclaimEvents, slots,
                            adoptedMod ? adoptedMod : "?");
        }
    }
    return reclaimed;
}

}  // namespace edvr
