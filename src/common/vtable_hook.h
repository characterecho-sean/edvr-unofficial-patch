// COM vtable interception, by whichever of three mechanisms fits the table.
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
// (the d3d11.dll we forward to) is the runtime's own -- the live table is
// what auto chooses (LiveCopy), wrapper-safe because there is no wrapper and
// following the runtime as it re-points its own table rather than freezing a
// snapshot the way CopyVptr did. A table living anywhere else is
// somebody's proxy class -- InPlace, because re-pointing their object is
// issue #6. vtableInsideModule() is the probe; the POLICY stays with callers,
// who know which module implements what they hooked. tools/vtable_test holds
// both mechanisms' cells, each written to fail against the wrong one first.
//
// AND THEN A THIRD, which exists because those two do not span the space and
// issue #21 is the rig standing in the gap between them.
//
//   LiveCopy -- the object's vptr moves to a private table exactly as CopyVptr
//   moves it, and EVERY entry of that private table is a twelve-byte jump stub
//   that reads the RUNTIME'S OWN entry for that slot and jumps through it, at
//   the moment of the call. Nothing is frozen. The only thing that differs
//   from stock is WHERE the vptr points.
//
//   It is built to answer one question that nothing else can. On two users'
//   rigs CopyVptr dies 1.7 s after install with the GPU hung, and the
//   once-a-second walk saw 24 work-emitting slots switch to a second variant at
//   the frame that hung (slot 12 DrawIndexed from ..._DrawIndexed_<1> to
//   ..._DrawIndexed_Amortized<1>). Two things changed at once in that mode and
//   only one of them can be the cause: the vptr was RELOCATED, and the table was
//   FROZEN. commitUnpatched() (the swap-only probe) removes the thunks and keeps
//   both; this removes the freeze and keeps the relocation. Run both and the
//   answer is arithmetic rather than inference -- if swap dies and live lives,
//   staleness is fatal and the frozen copy is the bug; if both die, relocating
//   the vptr is fatal on its own and no amount of following the runtime helps.
//
//   And if live LIVES, it is not merely a probe: it is a mechanism with
//   CopyVptr's immunity (a tool writing the shared table cannot reach a slot
//   the object no longer dispatches through) and none of its staleness, for two
//   extra jumps per call. What it costs is what CopyVptr costs -- issue #6, a
//   wrapper's object re-pointed -- so the pick between InPlace and this is the
//   same question, asked the same way.
//
//   WHAT "ORIGINAL" MEANS IN THIS MODE, because a caller's whole forwarding
//   contract rests on it: the pointer replace() hands back for a patched slot is
//   that slot's LIVE STUB, so calling it means "call whatever the runtime's own
//   table holds for this slot right now, read at this call". It is never a
//   snapshot, and a thunk body needs no change at all to get that.
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
// d3d11.dll mean the runtime owns this object and swapping its vptr breaks no
// wrapper -- but relocating the vptr is NOT immune to the runtime re-pointing
// its own table; the live-forwarding table (LiveCopy) is what follows the
// runtime's re-lay (2026-08-18); entries in a
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
//
// `timeline` turns the watch from a writer-naming probe into the FLIP TIMELINE
// below, which is a different instrument answering a different question off the
// same mechanism. See the struct.
//
// THE WHOLE TABLE IS COVERED, not the slot's page. `slot` only says where to
// anchor the log lines; the protected region is [vtable, vtable+slotCount)
// rounded out to page boundaries, which for a ~300-entry table is one or two
// pages and one VirtualProtect call. It was one page anchored on the slot,
// which on a 2.4 KB table left roughly a quarter of the placements reporting
// nothing at all about the slots that fell off the end -- silently, because
// "no write was seen" and "that slot was never watched" are the same line.
bool vtableWatchSlot(void** vtable, size_t slot, size_t slotCount,
                     const char* who, bool timeline = false);

// Who armed the one watch, or null when nothing is armed. For a caller that
// has just been refused and has to say WHICH setting is holding it.
const char* vtableWatchArmedBy();

// ONE VALUE-CHANGING WRITE INTO THE TABLE -- WHO, WHEN, AND FROM WHAT TO WHAT.
//
// THE QUESTION IT EXISTS FOR. On two rigs the private-copy mode dies 1.7 s
// after install with the GPU hung, and the once-a-second walk saw 24
// work-emitting slots holding a second variant AT THE FRAME THAT HUNG (slot 12
// DrawIndexed on ..._DrawIndexed_Amortized<1>). Nobody can say whether that
// switch happened BEFORE the hang -- a live method dispatched against state
// prepared for its sibling, which is a cause -- or AFTER it, the runtime
// reacting to a device that was already gone, which is a consequence. A
// once-a-second sample cannot tell those apart, and they are opposite
// conclusions.
//
// So the watch already in this file is extended rather than duplicated: the
// page is read-only, every write faults, and the handler now reads the slot
// before the store and again after the single step. A write that puts back the
// SAME value is counted and dropped -- on the maintainer's rig there are
// hundreds of those per frame, and they are the reason a frozen copy has never
// hurt him. A write that CHANGES the value is recorded with a timestamp, the
// frame number, the slot, both values, the faulting instruction, the thread and
// eight frames of the writer's stack.
//
// Nothing is resolved, logged or locked inside the handler. Module names cost a
// VirtualQuery each and the events are drained on the frame path, which is the
// only thread that may spend a syscall. The first few also go to the breadcrumb
// file, which is unbuffered and survives the process being killed by a TDR --
// which is how these sessions end, and the reason a log-only instrument would
// answer nothing on the one rig it was built for.
struct VTableFlip {
    uint64_t qpc = 0;            // QueryPerformanceCounter at the fault
    uint64_t frame = 0;          // the render frame in progress
    size_t   slot = 0;
    void*    before = nullptr;
    void*    after = nullptr;
    void*    writer = nullptr;   // the faulting instruction
    uint32_t thread = 0;
    uint8_t  stackCount = 0;
    void*    stack[8] = {};
};

// Once per frame, on the render thread. Publishes the frame number the handler
// stamps events with, records which thread the render thread IS (so an event
// from anywhere else can be called out as such), then drains the ring and
// prints what arrived. Free when no watch is armed.
//
// THE ONE FRAME NUMBER, and it is published whether or not anything is armed,
// because two counters that mean "the frame" are how an ordering question gets
// answered wrongly. device_hook owns the count; this publishes it; the handler
// stamps flips with it; perf_monitor prints it beside a long frame. Frame N is
// everything between Present N-1 returning and Present N returning, so the
// number to pass here is the frame the work about to be done BELONGS to --
// which, at the top of a post-Present block, is the frame that is only now
// beginning. See the call site.
void vtableWatchFrameTick(uint64_t frameNo);

// The frame number last published by vtableWatchFrameTick, and how long the
// watch has been armed. Zero before the first tick; the seconds are zero when
// nothing is armed. For any other instrument that has to say WHEN it is
// speaking in terms a flip can be compared against.
uint64_t vtableWatchFrame();
double   vtableWatchSecondsSinceArm();

// Write the last few recorded flips to the log AND the breadcrumb file, with a
// reason. For the paths that fire when something has just gone wrong -- a long
// or dropped frame, the copy-mode census reporting the table has moved -- so
// that a rig which dies leaves behind the answer to "did the flip precede the
// hang, and who did it". Safe to call when nothing is armed; bounded per
// session so it cannot fill the breadcrumb file.
//
// `subjectFrame` IS THE FRAME THE LINE IS ABOUT, and it is a parameter because
// the two callers mean different frames. The monitor's long-frame path runs in
// the block for frame N about the frame that just ENDED, N-1; the census runs
// about the frame it is in. Printing "the frame in progress" for both made the
// dump's own instruction -- if a change lands in the same frame as this, it
// PRECEDED what this line is about -- give the opposite of the right answer for
// the monitor: a flip inside the hung frame carries N-1 against a header saying
// N, and a reader following the sentence concludes it did not precede the hang.
// On the one question this instrument exists for.
void vtableWatchDumpRecent(const char* why, uint64_t subjectFrame);

// What the last dump was ABOUT, and how many of the flips it listed fell inside
// that frame. For the cell that holds the paragraph above to its word.
uint64_t vtableWatchLastDumpFrame();
uint32_t vtableWatchLastDumpInside();

// Value-changing writes recorded, and in-table writes that put the same value
// back. For the unit cells, and for anyone asking whether an armed timeline
// ever saw anything.
uint32_t vtableWatchFlips();
uint32_t vtableWatchIdempotentWrites();

// Distinct (slot, old, new, writer) tuples seen. The aggregate the timeline
// collapses to once the per-event lines stop: a runtime alternating one slot
// between two entries is two rows with big counts, not ten thousand lines.
uint32_t vtableWatchFlipShapes();

// Read back one recorded flip by index, oldest first, for the unit cells.
// False when the index is past what has been recorded or has been overwritten.
bool vtableWatchFlipAt(uint32_t index, VTableFlip* out);
#ifdef EDVR_VTABLE_TEST
// Pauses publication after the first field is written, for a deterministic
// reader/writer interleaving in the test executable only.
void vtableWatchSetPublishObserverForTest(void (*observer)(uint32_t));
#endif

// Has the timeline printed its closing report -- the cost per frame and the
// table of who wrote what? The twin of vtableWatchSummarised, and it exists for
// the same reason: the field found the WRITE watch's summary never printing at
// all, because the flag guarding it against printing twice had been set by the
// disarm path first. A summary nothing asserts is a summary that can vanish.
bool vtableWatchFlipSummarised();

// Put the watch back after a catch let a write through. Cheap and safe to call
// every frame; does nothing unless a catch is pending. Re-arming here rather
// than inside the handler avoids the single-step dance: the faulting
// instruction gets a writable page, completes, and the next frame closes it
// again.
void vtableWatchRearm();

// How many writes the watch has caught. For the unit cell, and for anyone
// asking whether an armed watch ever fired.
uint32_t vtableWatchCatches();

// ...of which, how many landed INSIDE the watched table rather than merely
// sharing its pages. The cell that proves the whole table is covered writes to
// the first and last slot of a table that straddles a page boundary and
// requires both to be counted here: a slot on an unprotected page does not
// fault at all, so this number is the coverage.
uint32_t vtableWatchInTableWrites();

// Single steps that arrived for a catch the frame path had already given up on.
//
// The re-arm waits two seconds for a step that is merely late and then takes
// the pages back anyway, because a step that never comes would otherwise leave
// them open for the session. If that thread then turns up after all, its step
// no longer owns anything -- and counting it as a chimera would put a number
// the summary calls "must be zero" next to an event the file deliberately
// caused. It is counted here instead, and swallowed exactly the same way.
uint32_t vtableWatchEscapedSteps();

// Catches the frame path gave up on: the other end of the same escape. It is in
// the closing summary as "never had their second half", and it is here because
// a cell has to be able to say that a HEALTHY catch was never given up on.
uint32_t vtableWatchUnfinishedCatches();

// Single-step exceptions that arrived while a watch existed and did NOT match
// the fault/step pair the handler owed itself.
//
// It must read zero. The pair is claimed with one atomic exchange, so exactly
// one thread can own a catch and only that thread's step can complete it --
// and a step arriving on any other thread means two threads believed they were
// mid-catch, which is the state that produced a chimera event (one thread's
// slot with another's value) before the claim was atomic. Such a step is still
// SWALLOWED rather than passed on: the trap flag was ours, and handing an
// orphaned single step back to a process that has no handler for it kills the
// process outright, which is how this was found.
uint32_t vtableWatchChimeras();

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
    LiveCopy,      // private table of jump stubs, each reading the live entry
};

// How many bytes one live-mode stub occupies.
//
// Twelve are used -- `mov rax, imm64` (10) then `jmp qword ptr [rax]` (2) --
// and sixteen are spent, so every stub starts on a sixteen-byte boundary and
// the slot index is a shift rather than a multiply for anyone reading a
// disassembly. The four spare bytes are int3, so a jump into the gap stops
// there instead of running into the next stub.
//
// rax is the choice because the x64 calling convention makes it neither an
// argument register nor callee-saved, and no D3D11 method is variadic (where
// rax carries the vector-register count). The stub therefore clobbers nothing
// any callee may read.
constexpr size_t kLiveStubBytes = 16;

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
    // replace(): the three mechanisms stage differently, and switching after
    // staging would mean patches recorded against a table that is no longer
    // the one being modified. Defaults to InPlace, which is the mode that
    // never breaks somebody else's object.
    //
    // LiveCopy also allocates and writes the stub page here, and can fail for
    // that reason alone -- a caller that ignores the return value gets a hook
    // still in InPlace mode rather than a half-built live one.
    bool setMode(HookMode mode);
    HookMode mode() const { return m_mode; }

    // Stages one slot. origOut receives what the slot holds NOW, which is
    // what the caller must forward to -- if another hook (ours or a foreign
    // one) already patched this slot, that is the entry we chain to, and the
    // chain is preserved in both directions by the polite uninstall below.
    // In CopyVptr mode "what the slot holds now" reads through the object's
    // CURRENT vptr, so stacking two copy-mode hooks on one object chains
    // exactly like stacking two in-place hooks on one table. In LiveCopy mode
    // it is that slot's live stub, which is the same statement with the
    // staleness taken out: forwarding to it reaches whatever the runtime's own
    // table holds at the moment of each call. Must be called before commit().
    // Refuses indices beyond the executable prefix, since those are not
    // methods we have any reason to believe in.
    bool replace(size_t index, void* replacement, void** origOut);

    // Applies the staged patches. InPlace: writes every staged entry into the
    // shared table, all-or-nothing -- a partial failure rolls back the slots
    // already written, the same discipline vscreen_res uses for code
    // patching. CopyVptr and LiveCopy: writes the patches into the private
    // table and swaps the object's vptr -- one aligned pointer store, which
    // cannot be partial.
    bool commit();

    // THE SWAP WITH NOTHING IN IT. CopyVptr only: point the object at a
    // byte-identical private copy of its table, with no slot patched and no
    // thunk anywhere in the path. Every call the object receives then runs
    // exactly the code it would have run before -- from a different address.
    //
    // A diagnostic, and the sharpest one this mechanism admits. Two users'
    // machines die 1.7 seconds after the private-copy mode installs, and the
    // one fact that separates their rigs from a working one is that EDVR's
    // thunks RUN there (the in-place mode, where the runtime overwrites the
    // thunks before the first frame, is stable on both). That leaves two
    // suspects with nothing between them: the swap itself, or what the thunks
    // do once they are running. This removes the second entirely. If the game
    // still dies, the swap is fatal on its own and no hook body was ever the
    // problem; if it lives, the mechanism is innocent and the bug is somewhere
    // in twenty-nine functions that can be bisected.
    bool commitUnpatched();

    // THE SWAP THAT FREEZES NOTHING. LiveCopy only, and the other half of the
    // experiment commitUnpatched() opens.
    //
    // The object is pointed at a private table in which every entry is a stub
    // that jumps through the runtime's own entry for that slot, read at the
    // call. No slot is patched, no thunk exists anywhere, and no call can ever
    // reach an implementation the runtime has moved on from. The only thing
    // that differs from stock is the ADDRESS of the table the vptr names.
    //
    // So the pair separates the two things CopyVptr does at once. If
    // context_hook_probe = swap dies and this lives, the fatal half is the
    // FREEZE -- the game was calling last second's implementation with this
    // second's state -- and the mechanism to build is this one. If both die,
    // relocating the vptr is fatal by itself on that rig, following the runtime
    // perfectly does not help, and InPlace is the only mechanism left. Either
    // answer is worth one session; neither is obtainable any other way.
    //
    // Refuses a non-empty patch list on purpose: commit() is how a patched live
    // table is reached, and one way to each state is what keeps a log line
    // meaning what it says.
    bool commitLive();

    // InPlace: restores each entry we wrote, but ONLY where it still holds
    // our replacement. An entry someone patched after us belongs to them now;
    // restoring it would clobber their hook, which is the same composition
    // failure this class exists to stop, viewed from the other side.
    // CopyVptr and LiveCopy: restores the vptr this hook found at attach --
    // which, for stacked private hooks, is the table underneath, so unwinding
    // in reverse install order peels the stack exactly as it was built. The
    // live mode's stub page is NEVER freed; see the definition.
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
    // The private modes re-patch nothing and always return 0: the private table
    // has no co-owners to misread and no shared table for the runtime or a
    // clean-resolving tool to rewrite -- immunity is their whole reason to
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
    //
    // In LiveCopy the same walk runs and is not an alarm at all: nothing can be
    // stale there, so it is a CENSUS of how often the runtime re-selects its own
    // variants and which slots it moves -- the measurement the copy-mode logs
    // from issue #21 produced, taken on a rig that is not being harmed by it.
    size_t reclaim(const char* name, const size_t* quietSlots = nullptr,
                   size_t quietCount = 0);

    // THE CENSUS ON ITS OWN, for a hook nobody calls reclaim() on.
    //
    // The drift walk and the flip dump ride inside reclaim's private branch,
    // and the two context probes (context_hook_probe = swap and live) install
    // no fix, so nothing on the frame path ever calls reclaim -- which means
    // those sessions, the ones that exist to ask what the runtime is doing to
    // the table, reported nothing about it at all. Call this about once a
    // second from a frame path instead. No-op unless the hook is committed in a
    // private mode.
    void censusTick(const char* name);

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
    // Zero in the private modes, where the question does not arise: the object
    // dispatches through a table only we can write.
    size_t   lastPassDisplaced() const { return m_lastDisplaced; }

    // Slots this pass found foreign and will never contest again. Steady for
    // the session once the concessions have happened, so a caller reports the
    // number rather than a rate.
    size_t   lastPassConceded() const { return m_lastConceded; }

    // Did the last reclaim() actually inspect the table? False when the hook is
    // uncommitted, in a private mode, or when the slot registry overflowed and
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

    // Does this mode dispatch the object through a table of ours? True for
    // CopyVptr and LiveCopy, which differ in what the table HOLDS and agree
    // about everything else: where patches are staged, how the vptr is swapped,
    // how uninstall peels a stack, and that there is no shared slot to patrol.
    bool usesPrivateTable() const { return m_mode != HookMode::InPlace; }

    // The table the object dispatches through in a private mode, and how many
    // entries it has. CopyVptr's lives in m_copy; LiveCopy's lives at the front
    // of the never-freed stub allocation, because a table whose cells an upper
    // hook's stubs dereference must outlive this hook exactly as the stubs do.
    // Null and 0 in place, where the object dispatches through the shared table.
    void** dispatchTable() const {
        if (m_mode == HookMode::LiveCopy) return m_liveTable;
        if (m_mode == HookMode::CopyVptr) return const_cast<void**>(m_copy.data());
        return nullptr;
    }
    size_t dispatchCount() const {
        if (m_mode == HookMode::LiveCopy) return m_stubCount;
        if (m_mode == HookMode::CopyVptr) return m_copy.size();
        return 0;
    }

    // LiveCopy only: allocate the block that holds BOTH the private table and
    // its stubs, and fill both in. Called from setMode, once, before anything
    // is staged. See m_liveBlock for why they share one allocation.
    bool buildLiveStubs();

    // LiveCopy only: make the whole block execute-read, once, after commit has
    // written its patches into the table and before the vptr moves onto it.
    // Never PAGE_EXECUTE_READWRITE -- a writable-executable page is the single
    // strongest heuristic every antivirus scanner looks for, and EDVR already
    // carries a Defender false positive without handing it one.
    bool sealLiveBlock();

    // The one aligned pointer store that moves the object onto m_copy, shared
    // by commit(), commitUnpatched() and commitLive() so that three entry
    // points cannot drift apart on the one operation that actually changes the
    // object. `why` labels the guard site.
    bool installVptr(const char* why);

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

    // Private modes only, read-only, from reclaim(): compare what the runtime's
    // table said when we attached against what it says now, and report where
    // they no longer agree. In CopyVptr that is the measurement of the premise
    // the mode rests on; in LiveCopy it is a census of the runtime's own
    // variant selection, which the live table follows by construction. See the
    // .cpp for both wordings and why they are different sentences.
    void noteCopyDrift(const char* who);

    void*              m_object = nullptr;
    void**             m_vtable = nullptr;
    std::vector<Patch> m_patches;
    size_t             m_execPrefix = 0;
    bool               m_committed = false;
    HookMode           m_mode = HookMode::InPlace;
    // Private-table state: the table the object dispatches through, and the
    // vptr found at attach (what uninstall puts back). The vector must never
    // reallocate after commit -- the object's vptr points at its data -- so it
    // is sized at attach and never touched again except by uninstall's clear.
    // In CopyVptr it holds copied entries; in LiveCopy it holds stub addresses.
    std::vector<void*> m_copy;
    // LiveCopy only: what the runtime's table held when this hook attached.
    //
    // The census below asks "has the runtime changed its own entries since we
    // installed", and in copy mode m_copy IS that baseline. In live mode m_copy
    // holds stubs, so the baseline has to be kept separately or the walk would
    // report all 302 entries as changed on the first pass, every pass, forever.
    // It is read-only after setMode and is never dispatched through.
    std::vector<void*> m_frozen;
    // LiveCopy only: one allocation holding the private TABLE and then one stub
    // per entry -- m_liveTable points at its front, the stubs follow.
    //
    // DELIBERATELY LEAKED at uninstall, and this is not an oversight to tidy up
    // later. A thread can be executing inside a stub at the instant the vptr is
    // restored -- the call was dispatched before the store and returns after it
    // -- and freeing the page under it is an access violation in somebody
    // else's code with EDVR nowhere on the stack.
    //
    // THE TABLE IS IN THE SAME ALLOCATION FOR THE SAME REASON, and it was in a
    // std::vector that uninstall cleared. An upper live hook's stubs hold the
    // addresses of THIS hook's table cells as immediates and dereference them at
    // every call, so freeing the table while such a hook is still installed is a
    // read of freed memory on the next draw -- reached by unwinding the two
    // hooks in the wrong order, which the shipped teardown does not do and which
    // nothing enforced. Now there is nothing to free: one VirtualAlloc, never
    // released, a few kilobytes once per hook per session, on a teardown that
    // only runs under FreeLibrary (the game exits by TerminateProcess).
    // Forgetting the pointer IS the leak.
    uint8_t*           m_liveBlock = nullptr;
    void**             m_liveTable = nullptr;
    size_t             m_stubCount = 0;
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
