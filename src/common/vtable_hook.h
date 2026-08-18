// COM vtable interception, by whichever of two mechanisms fits the table.
//
// TWO MECHANISMS, ONE QUESTION. Both existed as sole mechanisms first, each
// shipped, and each was refuted in the field by a rig the other would have
// survived. The question that picks between them is: WHOSE MODULE OWNS THE
// VTABLE THIS OBJECT DISPATCHES THROUGH?
//
//   CopyVptr -- copy the table into memory we own, patch the copy, point the
//   OBJECT's vptr at it. Shipped alone through 0.7.1. Its virtue took three
//   releases to even name: the D3D11 RUNTIME RE-POINTS ITS OWN STATIC TABLE
//   ENTRIES between internal per-mode variants (measured 2026-08-18:
//   draw/clear/dispatch slots re-pointed to system32\d3d11.dll internals when
//   an OpenXR Toolkit install flipped the runtime's mode; the war for those
//   slots was unwinnable because the opponent was the OS maintaining its own
//   state). A private copy never notices any of that -- which is why 0.7.0
//   "just worked" on the stack that broke 0.7.2+. Its vice was issue #6: on a
//   rig where the object is a WRAPPER's proxy (ReShade as dxgi.dll), the
//   table belongs to the wrapper, re-pointing the wrapper's object breaks the
//   wrapper's own dispatch assumptions, and the game crashes at launch.
//
//   InPlace -- make the table page writable and exchange the entries. Shipped
//   as the sole mechanism in 0.7.2+ because it composes with wrappers (their
//   object, their table, untouched identity). Its vice is the mirror image:
//   the table is shared property, so anything else that writes it -- a later
//   tool with clean-resolved forwards, or the RUNTIME ITSELF re-selecting
//   variants -- silently bypasses us, which is what reclaim() below detects
//   and, where evidence permits, heals.
//
// The pick: a table living inside the REAL implementation module's image
// (the d3d11.dll we forward to) is the runtime's own -- CopyVptr, immune and
// wrapper-safe because there is no wrapper. A table living anywhere else is
// somebody's proxy class -- InPlace, because re-pointing their object is
// issue #6. vtableInsideModule() is the probe; the POLICY stays with callers,
// who know which module implements what they hooked. tools/vtable_test holds
// both mechanisms' cells, each written to fail against the wrong one first.
//
// WHAT THE CHANGE COSTS THE CALLER: patching a vtable hooks EVERY object of
// that class, not the one you attached to. Each hook body must therefore
// begin by checking that `self` is the object it was installed for and
// forwarding untouched otherwise. That check is not optional -- deferred
// contexts and a wrapper mod's internal objects reach the same table.
//
// AND ONE MORE COST, PAID IN THE FIELD BEFORE IT WAS UNDERSTOOD: the table is
// shared property, so a tool that installs AFTER us can write the same slots.
// If it chains -- captures what the slot holds and forwards to it -- both run.
// The one that broke a user's session does not: OpenXR Toolkit (under
// OpenComposite) resolves its "original" pointers from a clean vtable of its
// own and then writes its hooks over whatever is in the live table. Measured
// 2026-08-18: it re-pointed the draw, render-target-bind and dispatch slots a
// few seconds after EDVR installed (its XR session init), every fix reading
// those calls starved, and nothing anywhere said so -- while Map, Unmap and
// ClearState, slots it does not touch, kept arriving and made the log look
// half-alive. The copy-and-swap mechanism was immune to this by accident: its
// private copy WAS the dispatch table, so a later hooker patched ours and
// chained through us.
//
// reclaim() is the answer: re-read the patched slots, and where somebody has
// re-pointed one, adopt their entry as the new forward target and re-patch on
// top -- both tools run, in the order last-installed-first, which is the order
// in-place patching always produces. Call it from a frame path, about once a
// second -- and vouch only for slots whose thunks you have measured silent,
// because a tool that CHAINS through us is also "not ours" in the slot, and
// re-patching over a chainer builds a call loop. See the method comment.
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

// Does this vtable ARRAY live inside this module's mapped image? True where
// the table is static read-only data in the module. NOT the mechanism probe
// on its own -- D3D11 hands out per-object heap vtables whose array is on the
// heap while its entries point into d3d11.dll, and that case must still take
// CopyVptr. Use vtableEntriesInModule for the mechanism decision; this stays
// for the unit test that documents the array-location fact.
bool vtableInsideModule(void** vtable, void* moduleBase);

// How many of the first `count` vtable entries point into this module's image.
//
// THE mechanism probe. It asks whose CODE implements the object's methods,
// which is the fact that actually decides safety: entries in the runtime's
// d3d11.dll mean the runtime owns this object and a vptr swap is safe and
// immune to the runtime re-pointing its own table (2026-08-18); entries in a
// wrapper's module (ReShade) mean swapping the object's vptr breaks the
// wrapper, which is issue #6. Robust to where the vtable ARRAY happens to
// live, which vtableInsideModule was not.
size_t vtableEntriesInModule(void** vtable, size_t count, void* moduleBase);

// Which mechanism a hook uses. Decided by the CALLER before replace(), from
// vtableInsideModule() and knowledge of which module implements the object.
enum class HookMode : uint32_t {
    InPlace = 0,   // patch the shared table; reclaim() watches it
    CopyVptr,      // private table copy; immune to table owners, no reclaim
};

class VTableHook {
public:
    VTableHook() = default;
    ~VTableHook() { uninstall(); }

    VTableHook(const VTableHook&) = delete;
    VTableHook& operator=(const VTableHook&) = delete;

    // Reads the object's vtable and sanity-checks it. maxEntries bounds how
    // far the plausibility probe walks; in CopyVptr mode it is also the copy
    // window, deliberately over-wide (a copy truncated to the apparent method
    // count breaks the moment the host calls a slot beyond it). Nothing is
    // written here.
    bool attach(void* object, size_t maxEntries = 512);

    // Selects the mechanism. Callable only between attach() and the first
    // replace(): the two mechanisms stage differently, and switching after
    // staging would mean patches recorded against a table that is no longer
    // the one being modified. Defaults to InPlace, which is the mode that
    // never breaks somebody else's object.
    bool setMode(HookMode mode);
    HookMode mode() const { return m_mode; }

    // Stages one slot. origOut receives what the slot holds NOW, which is
    // what the caller must forward to -- if another hook (ours or a foreign
    // one) already patched this slot, that is the entry we chain to, and the
    // chain is preserved in both directions by the polite uninstall below.
    // In CopyVptr mode "what the slot holds now" reads through the object's
    // CURRENT vptr, so stacking two copy-mode hooks on one object chains
    // exactly like stacking two in-place hooks on one table. Must be called
    // before commit(). Refuses indices beyond the executable prefix, since
    // those are not methods we have any reason to believe in.
    bool replace(size_t index, void* replacement, void** origOut);

    // Applies the staged patches. InPlace: writes every staged entry into the
    // shared table, all-or-nothing -- a partial failure rolls back the slots
    // already written, the same discipline vscreen_res uses for code
    // patching. CopyVptr: writes the patches into the private copy and swaps
    // the object's vptr -- one aligned pointer store, which cannot be
    // partial.
    bool commit();

    // InPlace: restores each entry we wrote, but ONLY where it still holds
    // our replacement. An entry someone patched after us belongs to them now;
    // restoring it would clobber their hook, which is the same composition
    // failure this class exists to stop, viewed from the other side.
    // CopyVptr: restores the vptr this hook found at attach -- which, for
    // stacked copy hooks, is the copy underneath, so unwinding in reverse
    // install order peels the stack exactly as it was built.
    void uninstall();

    // Re-read every committed slot; re-patch the ones somebody re-pointed --
    // but ONLY where the caller can vouch the slot's own thunk has gone quiet.
    //
    // Two kinds of tool write over an in-place patch, and they must not be
    // treated alike. A CHAINER captured our thunk from the slot and forwards
    // to it: our hook still runs, and adopting their entry as our forward
    // would point the two hooks at each other -- an infinite call loop, found
    // by the next Draw as a stack overflow. A BYPASSER resolved its forward
    // from a clean table: our thunk stops running entirely, which is the
    // OpenXR Toolkit failure this exists to heal. The two are told apart by
    // the one fact the caller owns and this class cannot see: whether the
    // slot's thunk is still being CALLED. A chainer keeps it firing; a
    // bypasser starves it.
    //
    // So `quietSlots` lists the slots whose thunks the caller has measured
    // silent long enough to rule idleness out (vscreen counts per-thunk calls
    // and requires several consecutive quiet seconds while frames flow). Only
    // those are eligible for adoption: the current entry becomes the new
    // forward target (written through the SAME origOut the caller gave
    // replace(), forward first, slot second -- a call mid-pass takes either
    // the old path or the whole new chain, never half of one), and our thunk
    // goes back on top. Both tools then run. A re-pointed slot NOT vouched
    // for is reported once and left alone: chainers keep working, and the
    // report is the evidence a starved-but-unvouched slot leaves behind.
    //
    // Also never touched, each for a reason it must keep:
    //   - a slot whose current entry is another EDVR hook's replacement (the
    //     two context hooks share ClearState) -- a healthy stack, not a
    //     clobber; ownership is tracked at commit() in a registry local to
    //     this file.
    //   - a shared slot a third party re-pointed: either owner re-patching
    //     alone would splice out the other. Reported and conceded.
    //   - a slot re-pointed more than kMaxRepatchesPerSlot times: a re-checking
    //     intruder would otherwise trade it back every second forever.
    //     Conceded, loudly.
    //
    // The residual this buys instead of the loop: a chainer that installs at
    // the START of a genuine multi-second lull in a slot with prior traffic
    // can be mistaken for a bypasser and adopted. For the slots callers vouch
    // for -- draw, bind and clear paths that fire every rendered frame -- a
    // multi-second lull while frames present does not happen in this game.
    //
    // Returns how many slots were re-patched this pass. `name` labels the log
    // lines; the first reclaim explains itself, later ones report at doublings.
    //
    // CopyVptr mode returns 0 without looking: the private copy has no
    // co-owners to misread and no shared table for the runtime or a
    // clean-resolving tool to rewrite -- immunity is the mode's whole reason
    // to exist, and a reclaim over it would be patrolling a wall nobody can
    // reach. (A later tool that vtable-patches finds the copy through the
    // object's vptr and chains through our thunks; one that swaps the vptr
    // again stacks on top the way we stacked. Both compose without help --
    // 0.7.0 and 0.7.1 shipped exactly this and the field never contradicted
    // it.)
    size_t reclaim(const char* name, const size_t* quietSlots = nullptr,
                   size_t quietCount = 0);

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
        // Where the caller keeps its forward pointer -- the address replace()
        // was given, kept so reclaim() can re-point the forward when it adopts
        // an intruder's entry. Null means replace() was called without one, and
        // such a slot can never be reclaimed: re-patching it would drop the
        // intruder from the chain instead of running in front of it.
        void** origOut = nullptr;
        // reclaim() bookkeeping. Counted per slot because the fight is per
        // slot: one contested entry must not retire the others with it.
        uint32_t repatches = 0;
        bool     retired = false;      // conceded -- cap hit, or unreclaimable
        bool     sharedNoted = false;  // the shared-slot report, said once
        bool     foreignNoted = false; // the unvouched-foreign report, said once
        bool     oddNoted = false;     // the non-executable-entry report, said once
    };

    // Writes one entry with the page temporarily writable. Returns false and
    // changes nothing if the protection could not be moved.
    static bool writeEntry(void** vtable, size_t slot, void* value);

    void*              m_object = nullptr;
    void**             m_vtable = nullptr;
    std::vector<Patch> m_patches;
    size_t             m_execPrefix = 0;
    bool               m_committed = false;
    HookMode           m_mode = HookMode::InPlace;
    // CopyVptr state: the private table, and the vptr found at attach (what
    // uninstall puts back). The vector must never reallocate after commit --
    // the object's vptr points at its data -- so it is sized at attach and
    // never touched again except by uninstall's clear.
    std::vector<void*> m_copy;
    bool               m_copyBreachNoted = false;  // the copy-mode breach line, said once
    // How many reclaim() passes found something to re-patch. Drives the log
    // cadence: the first explains, later ones report at doublings, so a tool
    // that re-hooks every second cannot fill the log while still being visible
    // AS a tool that re-hooks every second.
    uint32_t           m_reclaimEvents = 0;
};

}  // namespace edvr
