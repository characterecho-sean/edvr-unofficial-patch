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
    // How many reclaim() passes found something to re-patch. Drives the log
    // cadence: the first explains, later ones report at doublings, so a tool
    // that re-hooks every second cannot fill the log while still being visible
    // AS a tool that re-hooks every second.
    uint32_t           m_reclaimEvents = 0;
};

}  // namespace edvr
