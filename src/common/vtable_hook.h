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
//   "just worked" on the stack that broke 0.7.2+. Never noticing is the virtue
//   and, on a rig where the swapped-to entry is not interchangeable with the
//   swapped-from one, it is also the vice: the object then dispatches through a
//   snapshot of a table that has moved on. reclaim() measures that as DRIFT and
//   reports it; issue #21 is why it does. Its OTHER vice was issue #6: on a
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

// Which loaded module a code pointer belongs to, as "<full path>+0x<offset>".
//
// The full path and not the basename, because "d3d11.dll" names two different
// modules in this process -- Windows' runtime and EDVR's own proxy, which the
// game loads under exactly that name -- and a line that cannot tell them apart
// cannot answer the question it was printed for. Returns a description of why
// not when the pointer belongs to no module. `buf` should be MAX_PATH.
const char* vtableOwnerModuleName(void* p, char* buf, size_t bufLen);

// WATCH ONE VTABLE SLOT AND NAME WHOEVER WRITES TO IT.
//
// Every attribution this file makes is of a POINTER FOUND IN A SLOT, not of the
// code that put it there -- ownerModuleName resolves the value, and the reclaim
// lines that say "re-pointed by <module>" have always been naming the thing now
// in the table rather than its author. On issue #21's rig that distinction
// became the whole question: something restores one canonical value into
// twenty-three slots every frame, EDVR's own writes were proven to land and
// survive, EDHM and every overlay were taken out of the process and it carried
// on, and the only code left in there is the D3D11 runtime and the game itself.
// Which of those two, nobody can say, because nothing has ever looked at the
// writer.
//
// So: make the page read-only, catch the access violation the next write
// raises, and log the FAULTING INSTRUCTION's module and offset. That is the
// author, directly, with no inference in it. The write is then let through --
// protection restored, the trap flag set, EXCEPTION_CONTINUE_EXECUTION -- and
// the single-step exception that arrives one instruction later takes the page
// straight back.
//
// THAT SINGLE STEP IS THE WHOLE DESIGN, and the first cut of this probe did not
// have it: it opened the page and waited for the frame path to close it, so it
// saw one write per frame and an entire sweep of stores went by behind the
// first. Its four catches all landed 264 bytes BELOW the table -- traffic
// sharing the page -- and not one write to a slot was ever observed, while the
// log's wording invited exactly the reading that a sweep had been seen. Taking
// the page back after each store is what turns "somebody wrote near our table"
// into "somebody wrote OUR TABLE".
//
// `slotCount` gives the table's extent so a write to it can be told from a
// write that merely shares its page: only the former is reported, the latter is
// counted, and the summary at the end prints the SPAN of everything written --
// a range that reaches the table proves a sweep, one that stays outside it
// disproves the sweep and says the writer is still unfound.
//
// DIAGNOSTIC, OFF BY DEFAULT, and it must stay that way. While armed, every
// write anywhere on that page takes an exception, and a page holds far more
// than one vtable. It is for a rig with a known writer to point at it for one
// session, not for anybody's daily flying.
//
// `slot` is an index into `vtable`; `who` labels the log lines. Returns false
// if the page cannot be made read-only, if it is read-only already (in which
// case the writer is un-protecting it and this cannot see that), or if a watch
// is already armed -- one at a time, because one answer is what is wanted.
bool vtableWatchSlot(void** vtable, size_t slot, size_t slotCount,
                     const char* who);

// Put the watch back after a catch let a write through. Cheap and safe to call
// every frame; does nothing unless a catch is pending. Re-arming here rather
// than inside the handler avoids the single-step dance: the faulting
// instruction gets a writable page, completes, and the next frame closes it
// again.
void vtableWatchRearm();

// How many writes the watch has caught. For the unit cell, and for anyone
// asking whether an armed watch ever fired.
uint32_t vtableWatchCatches();

// Has the watch printed its closing summary? For the unit cell, which exists
// because the field found that summary never printing: the disarm path set the
// flag the summary uses to avoid printing twice, so its one call was guarded
// out and thirty-two catches were followed by silence.
bool vtableWatchSummarised();

// Disarm: give the page its write permission back and forget the watch. Safe
// to call when nothing is armed. Called at teardown so a session never ends
// leaving somebody else's page read-only, and by the unit cell between runs.
void vtableWatchStop();

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

    // Name the module that IMPLEMENTS these methods, if the caller knows it.
    //
    // This is the one fact that lets reclaim adopt a re-pointed slot with no
    // traffic evidence, and it stays with the caller for the same reason the
    // mechanism choice does: only the caller knows which image implements what
    // it hooked. Optional -- unset means every re-point goes through the quiet
    // vouch, which is the behaviour every release before this one had.
    //
    // WHAT IT ASSERTS, and it is a promise the caller must be able to keep:
    // this module implements the methods, and it is not a tool that hooks. The
    // D3D11 runtime qualifies. A wrapper mod does not, and neither does EDVR's
    // own DLL -- pass a module that might CHAIN through us and reclaim will
    // adopt a chainer's thunk as its forward, which points the two hooks at
    // each other and overflows the stack on the next call. tools/vtable_test
    // has that cell, and it failed the first cut of this feature, which
    // inferred the module from the entries instead of being told: the test's
    // chainer and the entry it replaced live in the same image, so an inferred
    // rule adopted it. Being TOLD cannot make that mistake.
    //
    // Why it is needed at all: the vouch is measured silence on a slot while
    // OTHER thunks on the object still fire, and issue #21 found the rig where
    // that is unobtainable -- Windows' d3d11.dll re-pointed all 29 patched
    // slots 57 ms after install, before the first frame, so no thunk of ours
    // ever ran to be measured. Every slot was correctly detected, correctly
    // reported, and correctly left alone, and the DLL sat inert for fourteen
    // minutes explaining itself. An entry arriving from the implementation
    // module cannot be a chainer -- a chainer's thunk is the chainer's own
    // code -- so that case never needed the proxy in the first place.
    void setImplementationModule(void* moduleBase) { m_implModule = moduleBase; }

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
    // CopyVptr mode re-patches nothing and always returns 0: the private copy
    // has no co-owners to misread and no shared table for the runtime or a
    // clean-resolving tool to rewrite -- immunity is the mode's whole reason to
    // exist, and re-patching over it would be patrolling a wall nobody can
    // reach. (A later tool that vtable-patches finds the copy through the
    // object's vptr and chains through our thunks; one that swaps the vptr
    // again stacks on top the way we stacked. Both compose without help --
    // 0.7.0 and 0.7.1 shipped exactly this and the field never contradicted
    // it.)
    //
    // It does still LOOK, at two different things, because "immune" was an
    // assertion this class made about itself and never checked. It reports a
    // tool that wrote through the object into our copy (m_copyBreachNoted), and
    // it reports DRIFT: slots where the live shared table no longer says what
    // we copied. Drift is the mode working as designed -- and, if the entries
    // the runtime swaps between are not interchangeable, it is also the mode's
    // failure mode, with the game calling an implementation the runtime has
    // moved on from. Issue #21 is the field case that turned that from an
    // unstated premise into a question; see noteCopyDrift in the .cpp.
    size_t reclaim(const char* name, const size_t* quietSlots = nullptr,
                   size_t quietCount = 0);

    // How many committed slots the last reclaim() pass found holding something
    // that is neither our replacement nor a co-owner's -- whether or not it
    // re-patched any of them.
    //
    // THE DUTY-CYCLE FACT. On issue #21's rig the D3D11 runtime rewrites the
    // context's table about once a second and reclaim writes it back, so the
    // hooks are not installed CONTINUOUSLY -- they are installed between
    // exchanges. Nothing measured what fraction of frames that was, and "the
    // fixes are running" and "the fixes are running on some frames" look
    // identical in every totals line this project has. A caller polling this
    // once a frame can say which.
    //
    // CONTESTED slots only. A slot already conceded -- retired after a
    // tug-of-war, or shared by two EDVR hooks and therefore unrepairable by
    // either alone -- is a permanent, already-logged loss, and counting it here
    // would peg the figure at "never held" for the rest of the session and hide
    // the slots that ARE being healed within a frame. Those are counted by
    // lastPassConceded instead, so the caller can report both.
    //
    // Zero in CopyVptr mode, where the question does not arise: the object
    // dispatches through a table only we can write.
    size_t   lastPassDisplaced() const { return m_lastDisplaced; }

    // Slots this pass found foreign and will never contest again. Steady for
    // the session once the concessions have happened, so a caller reports the
    // number rather than a rate.
    size_t   lastPassConceded() const { return m_lastConceded; }

    // Did the last reclaim() actually inspect the table? False when the hook is
    // uncommitted, in copy mode, or when the slot registry overflowed and
    // re-claiming is off for the session. A caller sampling lastPassDisplaced()
    // must not score an unpatrolled hook as perfectly held.
    bool     lastPassRan() const { return m_lastPassRan; }

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
        // "Is this foreign entry the implementation module's own?", cached
        // against the entry that answered it. reclaim now runs every frame, and
        // the question costs a VirtualQuery -- which a permanently foreign slot
        // nobody will heal would otherwise pay every frame for the session to
        // re-derive an answer that cannot have changed. A different pointer is
        // a different question and is classified again.
        void*    lastForeign = nullptr;
        bool     lastForeignIsOwner = false;
        // THE VARIANT CENSUS, and it exists to answer a design question rather
        // than a diagnostic one.
        //
        // Patrolling the table is a treaty, not a peace: the runtime rewrites
        // the slot, we write it back, forever, and between the two writes our
        // thunk is not in the dispatch path. The only design with no window at
        // all is to stop owning the SLOT and own the CODE -- hook the entry
        // point of every implementation the runtime selects, so it can rewrite
        // its table as often as it likes and whichever variant it picks is one
        // we are already inside. That is only tractable if the set of variants
        // per slot is SMALL AND CLOSED. Nothing has ever measured whether it is.
        // Four is enough to answer the question: a slot that alternates between
        // two entries says the set is tiny and the design is easy; one that
        // overflows says the opposite, loudly, and saves the attempt.
        void*    seen[4] = {};
        uint8_t  seenCount = 0;
        bool     seenOverflow = false;
    };

    // Writes one entry with the page temporarily writable. Returns false and
    // changes nothing if the protection could not be moved.
    //
    // Reads the slot back twice -- once before the protection is restored, once
    // after -- and reports when the write did not survive its own function.
    // `why` labels the write in that report ("install", "re-claim", "undo").
    // See the definition: five years of assuming these writes land, and a rig
    // where the slot is the original again on the next frame, every frame.
    bool writeEntry(void** vtable, size_t slot, void* value, const char* why);

    // How many non-surviving writes get an autopsy line. Three is enough to
    // show install and re-claim behaving the same way without a line per frame.
    static constexpr uint32_t kWriteAutopsies = 3;

    // Clear everything this hook believed about the object it just released.
    // Called from every one of uninstall()'s three exits; see the definition.
    void forgetObject();

    // CopyVptr only, read-only, from reclaim(): compare the copy we dispatch
    // through against the live shared table and report where they no longer
    // agree. The measurement of the premise the mode rests on -- see the .cpp.
    void noteCopyDrift(const char* who);

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
    // How many passes found the live shared table saying something our copy
    // does not. Same cadence as m_reclaimEvents, and for the same reason: a
    // runtime that re-points every second must stay visible without filling
    // the log with the fact.
    uint32_t           m_copyDriftEvents = 0;
    // The "nothing has drifted" line, said once. A negative that is never
    // stated is a negative nobody can rely on.
    bool               m_copyCleanNoted = false;
    // The "the module re-pointed its own table" explanation, said once. It is
    // a different event from a rival tool's clobber and needs its own line.
    bool               m_ownerRepointNoted = false;
    // The image that implements these methods, per setImplementationModule.
    // Null until a caller names one, and then reclaim may adopt from it
    // without a vouch.
    void*              m_implModule = nullptr;
    // Slots found not ours on the last pass -- see lastPassDisplaced().
    size_t             m_lastDisplaced = 0;
    size_t             m_lastConceded = 0;
    bool               m_lastPassRan = false;
    uint32_t           m_writeAutopsies = 0;   // see kWriteAutopsies
    // How many reclaim() passes found something to re-patch. Drives the log
    // cadence: the first explains, later ones report at doublings, so a tool
    // that re-hooks every second cannot fill the log while still being visible
    // AS a tool that re-hooks every second.
    uint32_t           m_reclaimEvents = 0;
};

}  // namespace edvr
