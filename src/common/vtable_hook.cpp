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

bool VTableHook::replace(size_t index, void* replacement, void** origOut) {
    if (!m_object || m_committed) return false;
    if (index >= m_execPrefix) return false;

    void* original = nullptr;
    if (!guarded("VTableHook::replace/read-slot",
                 [&] { original = m_vtable[index]; })) {
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
    if (g_slotOwnersPoisoned) return 0;   // see the flag's comment
    const char* who = name ? name : "?";

    size_t reclaimed = 0;
    char slots[96];
    slots[0] = '\0';

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
                Log::get().note(
                    "VTableHook %s: slot %zu was re-pointed by another tool, and "
                    "it is a slot TWO EDVR hooks share -- re-patching it from "
                    "either one would cut the other out of the chain, so it is "
                    "left with the other tool and whatever reads this call stays "
                    "bypassed. Report this log.",
                    who, p.slot);
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
                Log::get().note(
                    "VTableHook %s: slot %zu no longer holds EDVR's hook. It is "
                    "NOT being taken back: EDVR only re-patches slots whose own "
                    "calls have measurably gone quiet, and this one has no such "
                    "evidence -- a tool that CHAINS through EDVR still runs us, "
                    "and re-patching over a chainer builds a call loop. If the "
                    "fixes reading this call have gone quiet, report this log. "
                    "Said once.",
                    who, p.slot);
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
            Log::get().note(
                "VTableHook %s: slot %zu has been fought over %u times -- "
                "whatever keeps taking it re-checks its hooks the way EDVR "
                "does, and a tug-of-war every second serves nobody. EDVR will "
                "not contest the NEXT re-point: when it comes, whatever reads "
                "this call goes quiet for good. Report this log.",
                who, p.slot, p.repatches);
        }
        if (!wrote) continue;

        ++reclaimed;
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
                "tool after EDVR installed -- it resolved its own \"original\" "
                "pointers instead of chaining through the slot, so EDVR's hooks "
                "there were silently bypassed. OpenXR Toolkit under "
                "OpenComposite is a known one. EDVR re-patched on top and now "
                "forwards to the other tool's entries, so BOTH run. This check "
                "repeats about once a second and reports again at doublings.",
                who, reclaimed, slots);
        } else if ((m_reclaimEvents & (m_reclaimEvents - 1)) == 0) {
            Log::get().note("VTableHook %s: reclaim #%u (slot(s) %s).", who,
                            m_reclaimEvents, slots);
        }
    }
    return reclaimed;
}

}  // namespace edvr
