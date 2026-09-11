#include "vtable_hook.h"

#include <windows.h>

#include <cstdio>   // _snprintf_s, for the reclaimed-slot list
#include <cstring>  // strncat_s

#include "guard.h"
#include "log.h"
#include "proxy.h"  // breadcrumb(), which outlives the process a TDR kills

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
    uintptr_t tableLo = 0;           // the vtable array, so a write can be told
    uintptr_t tableHi = 0;           // from a write merely sharing its page
    void*     pageBase = nullptr;
    SIZE_T    pageSize = 0;
    DWORD     writableProtect = 0;   // what the page was, and is put back to
    DWORD     readOnlyProtect = 0;   // the same without write permission
    volatile LONG armed = 0;
    volatile LONG rearmWanted = 0;   // the frame-path fallback, see vtableWatchRearm
    volatile LONG stepPending = 0;   // a single step is owed to us
    volatile LONG stepThread = 0;    // ...by this thread, and no other
    bool      finished = false;
    uint32_t  catches = 0;           // writes seen anywhere on the page
    uint32_t  inTable = 0;           // ...of which, inside the vtable
    uint32_t  reported = 0;          // in-table lines printed
    // Writes that faulted while the watch was mid-catch on another thread: let
    // through, not recorded, counted. A gap in an instrument's coverage has to
    // be a number rather than a silence. See the handler.
    uint32_t  concurrent = 0;
    uintptr_t lo = 0;                // span of addresses written
    uintptr_t hi = 0;
    void*     sites[8] = {};         // distinct writing instructions
    uint32_t  siteCount = 0;
    // WHICH SLOTS, as a bitmap over the 512 the probe will address. The scope
    // question the first proof left open: four draw entries were caught in one
    // burst, and whether the same sweep restores the other nineteen slots on
    // EDVR's list or only the draws decides how much of the permanent fix has
    // to exist. A count cannot answer that; the set can.
    uint64_t  slotsWritten[8] = {};
    char      who[48] = {};
    // THE FLIP TIMELINE, armed instead of the writer probe. See VTableFlip.
    bool      timeline = false;
    // What the fault handler owes the single-step handler: the cell that is
    // about to be written and what it held before. The NEW value is read after
    // the step rather than decoded out of the instruction, and that choice is
    // not laziness -- decoding would have to cover every store form the runtime
    // and the game might use, and a decoder that is wrong once names the wrong
    // value with total confidence. Reading the cell back cannot be wrong about
    // what the cell now holds. Its one gap is a second thread writing the same
    // cell inside the window the store runs in, which would credit this thread
    // with that value; the window is one instruction and the thread id recorded
    // is the one that faulted.
    volatile LONG pendingLive = 0;
    uintptr_t pendingCell = 0;
    size_t    pendingSlot = 0;
    void*     pendingOld = nullptr;
    void*     pendingRip = nullptr;
    uint32_t  pendingThread = 0;
    uint64_t  pendingQpc = 0;
    uint8_t   pendingStackCount = 0;
    void*     pendingStack[8] = {};
};
WriteWatch g_watch;
PVOID      g_watchHandler = nullptr;

// How many events the ring holds between two frame-path drains.
//
// A value-CHANGING write is rare by construction -- the same-value restores
// that dominate a healthy rig never reach here -- so 128 is many frames of
// headroom in the private modes, where EDVR writes the shared table never. In
// the shared mode EDVR's own per-frame re-patching lands here too (about two
// dozen a frame, and honestly so: they really are changes, made by EDVR, and
// the writer name says as much), which is the one case that can overrun. An
// overrun is COUNTED and reported rather than papered over.
constexpr uint32_t kFlipRing = 128;

// The per-session budget for value-changing events, separate from
// kMaxWatchCatches and much larger.
//
// The catches cap bounds the cost of an armed probe on a page nobody stops
// writing to, and with the timeline on, that page is written hundreds of times
// a frame with the same value every time -- so charging those against a 2000
// cap would end the instrument in four frames, having recorded nothing. Same
// -value writes are counted and never charged; only changes are, and 4000 of
// those is far past anything a session should produce in the modes this exists
// for.
constexpr uint32_t kMaxFlipEvents = 4000;

// How many distinct (slot, old, new, writer) tuples the aggregate holds. A
// runtime alternating one slot between two implementations is two rows; the 24
// slots issue #21 reports moving at once are 24 to 48. Sized past that, and an
// overflow is stated rather than silently truncating the table.
constexpr uint32_t kFlipShapes = 64;

// Events that get a line of their own as they arrive, before the aggregate
// takes over. Sixteen covers the whole of one re-lay of the work-emitting
// family with room over, which is the event this exists to catch in full.
constexpr uint32_t kFlipFullLines = 16;

// ...and how many of those also go to the breadcrumb file. Eight, because a
// breadcrumb is an open/append/close and the file has to stay readable: these
// are the lines that survive a TDR, not a second copy of the log.
constexpr uint32_t kFlipCrumbs = 8;

// How many times a session vtableWatchDumpRecent may fire. The callers are
// already rate-limited (the monitor's long-frame line has its own cap, the
// census reports at doublings), but they are rate-limited for the LOG, and this
// also writes the breadcrumb file, which is opened and closed per line and is
// meant to stay short enough to read.
constexpr uint32_t kFlipDumps = 16;

struct FlipShape {
    size_t   slot = 0;
    void*    before = nullptr;
    void*    after = nullptr;
    void*    writer = nullptr;
    uint32_t count = 0;
};

struct FlipTimeline {
    VTableFlip      ring[kFlipRing];
    // index+1 of the entry in that slot, stored LAST. A reader checks it before
    // and after copying: equal both times means the entry was complete and
    // stayed complete for the length of the copy. Anything else is an event the
    // handler overwrote while the frame path was reading it, which is counted
    // as lost rather than reported as something it is not.
    volatile LONG64 ready[kFlipRing] = {};
    volatile LONG   count = 0;      // value-changing events recorded, ever
    uint32_t        reported = 0;   // ...of which the frame path has drained
    uint32_t        lost = 0;       // overwritten before it could be read
    uint32_t        unfinished = 0; // faults whose single step never completed
    uint32_t        idempotent = 0; // in-table writes that changed nothing
    FlipShape       shapes[kFlipShapes];
    uint32_t        shapeCount = 0;
    bool            shapeOverflow = false;
    uint32_t        nextShapeReport = kFlipFullLines;
    volatile LONG64 frameNo = 0;
    volatile LONG   frameThread = 0;
    uint64_t        frames = 0;     // frames the timeline has been armed over
    uint64_t        armedQpc = 0;
    uint32_t        dumps = 0;
    bool            summarised = false;
};
FlipTimeline g_flip;

// Back to nothing, field by field rather than by assigning a fresh instance:
// the volatile members make a member-wise copy assignment a construct nobody
// should have to reason about, and "the counters are zero" is the whole
// contract here.
void resetFlipTimeline() {
    for (uint32_t i = 0; i < kFlipRing; ++i) {
        g_flip.ring[i] = VTableFlip();
        InterlockedExchange64(&g_flip.ready[i], 0);
    }
    InterlockedExchange(&g_flip.count, 0);
    g_flip.reported = 0;
    g_flip.lost = 0;
    g_flip.unfinished = 0;
    g_flip.idempotent = 0;
    for (uint32_t i = 0; i < kFlipShapes; ++i) g_flip.shapes[i] = FlipShape();
    g_flip.shapeCount = 0;
    g_flip.shapeOverflow = false;
    g_flip.nextShapeReport = kFlipFullLines;
    InterlockedExchange64(&g_flip.frameNo, 0);
    InterlockedExchange(&g_flip.frameThread, 0);
    g_flip.frames = 0;
    g_flip.armedQpc = static_cast<uint64_t>(qpcNow());
    g_flip.dumps = 0;
    g_flip.summarised = false;
}

// Seconds since the watch armed, for a log line. QPC because a timeline whose
// resolution is the 15 ms system tick cannot say whether a flip preceded a hang
// that happened in the same frame, which is the entire question.
double flipSecondsSinceArm(uint64_t qpc) {
    const int64_t freq = qpcFrequency();
    if (freq <= 0 || qpc <= g_flip.armedQpc) return 0.0;
    return static_cast<double>(qpc - g_flip.armedQpc) / static_cast<double>(freq);
}

// Up to `max` return addresses above the faulting instruction, unwound from the
// exception's own CONTEXT.
//
// RtlCaptureStackBackTrace is the obvious call and the wrong one here: it walks
// from ITS OWN frame, which inside a vectored handler is ntdll's exception
// dispatcher, so the frames that matter -- the runtime routine doing the store
// and whoever called it -- start several entries in and may be past the eight
// we keep. Unwinding the CONTEXT we were handed starts exactly at the store.
//
// A FRAME WITH NO UNWIND DATA IS A LEAF, and it is handled once, at the
// bottom, and never again.
//
// The store this handler catches can perfectly well be the whole body of a
// small routine, and a routine that touches no stack and calls nothing has no
// RUNTIME_FUNCTION entry at all -- so the obvious "stop when the lookup fails"
// produces an EMPTY stack for exactly that shape of writer, which is the shape
// worth naming. The x64 convention says such a frame's return address sits at
// [rsp], so that is read, once, for the first frame only. Deeper frames that
// have no unwind data still stop the walk: by then rsp has been moved by an
// unwinder rather than by the CPU, and guessing twice compounds.
//
// The read is bounded against the thread's OWN STACK, taken from the TEB --
// which is a plain memory read, no syscall and no lock, and is the only thing
// standing between this and a wild dereference inside a fault handler. A value
// that is on the stack but is not a return address renders as a strange
// address in one report; a read outside the stack would be a second fault
// inside the handler for the first.
//
// Modules are deliberately NOT resolved here -- that is a VirtualQuery and a
// GetModuleFileName per frame, in a handler that can run hundreds of times a
// frame. The addresses are raw and the frame path names them.
uint8_t captureWriterStack(const CONTEXT* from, void** out, uint8_t max) {
    if (!from || !out) return 0;
    // A copy, because unwinding mutates the context it walks and the one we
    // were handed is what the CPU resumes into.
    CONTEXT ctx = *from;
    uint8_t n = 0;
    while (n < max) {
        DWORD64 imageBase = 0;
        PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(ctx.Rip, &imageBase, nullptr);
        if (!fn) {
            if (n) break;   // deeper than the leaf: stop rather than guess twice
            const NT_TIB* tib = reinterpret_cast<const NT_TIB*>(NtCurrentTeb());
            const DWORD64 lo = reinterpret_cast<DWORD64>(tib->StackLimit);
            const DWORD64 hi = reinterpret_cast<DWORD64>(tib->StackBase);
            if (!lo || !hi || ctx.Rsp < lo || ctx.Rsp + sizeof(DWORD64) > hi) break;
            const DWORD64 ret = *reinterpret_cast<const DWORD64*>(ctx.Rsp);
            if (!ret) break;
            ctx.Rsp += sizeof(DWORD64);
            ctx.Rip = ret;
            out[n++] = reinterpret_cast<void*>(ret);
            continue;
        }
        PVOID   handlerData = nullptr;
        DWORD64 establisher = 0;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, ctx.Rip, fn, &ctx,
                         &handlerData, &establisher, nullptr);
        if (!ctx.Rip) break;
        out[n++] = reinterpret_cast<void*>(ctx.Rip);
    }
    return n;
}

// Publish one completed event. Runs in the single-step handler, so it allocates
// nothing, takes no lock and says nothing.
void recordFlip(size_t slot, void* before, void* after, void* rip,
                uint32_t thread, uint64_t qpc, void* const* stack,
                uint8_t stackCount) {
    const uint32_t index =
        static_cast<uint32_t>(InterlockedIncrement(&g_flip.count)) - 1;
    const uint32_t at = index % kFlipRing;
    VTableFlip& e = g_flip.ring[at];
    e.qpc = qpc;
    e.frame = static_cast<uint64_t>(
        InterlockedCompareExchange64(&g_flip.frameNo, 0, 0));
    e.slot = slot;
    e.before = before;
    e.after = after;
    e.writer = rip;
    e.thread = thread;
    e.stackCount = stackCount;
    for (uint8_t i = 0; i < 8; ++i) e.stack[i] = i < stackCount ? stack[i] : nullptr;
    // LAST, and with a full barrier, so a reader that sees this value is
    // guaranteed to see every field above it.
    InterlockedExchange64(&g_flip.ready[at], static_cast<LONG64>(index) + 1);
}

// How many writes INSIDE the vtable earn a line.
//
// Four, at first, which was enough to prove the writer: four consecutive draw
// slots restored by four stores fourteen bytes apart inside system32\d3d11.dll.
// It was not enough to answer what came next -- whether that sweep restores all
// twenty-three slots EDVR patches or only the draw block -- because it stopped
// the watch four writes in, before the rest of the sweep could show itself.
// Thirty-two covers the whole of EDVR's list with room over, and the summary
// prints the SET rather than the count, which is the actual question.
constexpr uint32_t kMaxWatchReports = 32;

// The total budget, because the page is shared with whatever else the runtime
// put on it and that turned out to be written constantly. Every caught write
// costs two exceptions -- the fault and the single step that re-arms behind it
// -- so this bounds the cost of an armed probe at a few tens of milliseconds
// even on a page nobody stops writing to.
constexpr uint32_t kMaxWatchCatches = 2000;

// x86 EFlags trap flag: set it in the context we resume into and the CPU
// raises a single-step exception after exactly one instruction.
constexpr DWORD kTrapFlag = 0x100;

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

// Everything the watch saw, once, when it stops. The SPAN is the point: the
// first cut of this probe reported four catches at one address 264 bytes below
// the vtable and the field read it, reasonably, as the leading edge of a sweep
// that then ran up through the table -- but the probe could not have seen such
// a sweep, because it surrendered the page after the first store and did not
// take it back until the next frame. A range that reaches the table proves the
// sweep; one that stays pinned below it disproves it.
void reportWatchSummary() {
    if (g_watch.finished) return;
    g_watch.finished = true;
    char siteBuf[MAX_PATH];
    char joined[4 * MAX_PATH];
    joined[0] = '\0';
    for (uint32_t i = 0; i < g_watch.siteCount; ++i) {
        char one[MAX_PATH + 4];
        _snprintf_s(one, sizeof(one), _TRUNCATE, "%s%s", i ? "; " : "",
                    ownerModuleName(g_watch.sites[i], siteBuf, sizeof(siteBuf)));
        strncat_s(joined, sizeof(joined), one, _TRUNCATE);
    }
    // THE SET, which is the answer the count could never give.
    char slots[512];
    slots[0] = '\0';
    for (size_t i = 0; i < 512; ++i) {
        if (g_watch.slotsWritten[i >> 6] & (1ull << (i & 63))) {
            appendSlot(slots, sizeof(slots), i);
        }
    }
    Log::get().note(
        "VTableHook %s: write watch DISARMED. It saw %u write(s) to the page, "
        "%u of them INSIDE the vtable itself, spanning %p to %p; the vtable "
        "occupies %p to %p. The slots written were: %s. The instruction(s) "
        "responsible: %s. If that slot list covers everything EDVR patches, one "
        "sweep restores the whole table; if it is only part of it, the rest are "
        "being lost some other way and that is a different question. The page is "
        "back to normal and nothing further is intercepted.",
        g_watch.who, static_cast<unsigned>(g_watch.catches),
        static_cast<unsigned>(g_watch.inTable),
        reinterpret_cast<void*>(g_watch.lo), reinterpret_cast<void*>(g_watch.hi),
        reinterpret_cast<void*>(g_watch.tableLo),
        reinterpret_cast<void*>(g_watch.tableHi),
        slots[0] ? slots : "none -- no write landed inside the table at all",
        joined[0] ? joined : "none recorded");
    if (g_watch.timeline) {
        // Counts only, and no module lookups: this can be reached from inside
        // the exception handler when the event budget runs out, and
        // GetModuleFileName takes the loader lock. The table of who wrote what
        // is printed by reportFlipSummary on the frame path.
        Log::get().note(
            "VTableHook %s: flip timeline -- %u write(s) into the table CHANGED "
            "a value, %u put the same value back and cost nothing but an "
            "exception. The change budget is %u. The per-writer table and the "
            "cost per frame follow on the next frame.",
            g_watch.who,
            static_cast<unsigned>(InterlockedCompareExchange(&g_flip.count, 0, 0)),
            static_cast<unsigned>(g_flip.idempotent),
            static_cast<unsigned>(kMaxFlipEvents));
    }
}

LONG CALLBACK writeWatchHandler(EXCEPTION_POINTERS* ep) {
    const EXCEPTION_RECORD* er = ep ? ep->ExceptionRecord : nullptr;
    if (!er) return EXCEPTION_CONTINUE_SEARCH;

    // THE SECOND HALF OF EVERY CATCH. The write we let through has now
    // happened, so take the page back before anything else touches it.
    //
    // This is what the first cut of the probe lacked, and it is why that cut
    // could only ever see one write per frame: it opened the page and waited
    // for the frame path to close it, so an entire sweep of stores went by
    // unseen behind the first one. Re-protecting here instead means every
    // write is caught, which is the difference between "somebody wrote near our
    // table" and "somebody wrote OUR TABLE".
    if (er->ExceptionCode == EXCEPTION_SINGLE_STEP &&
        InterlockedCompareExchange(&g_watch.stepPending, 0, 0) &&
        g_watch.stepThread == static_cast<LONG>(GetCurrentThreadId())) {
        InterlockedExchange(&g_watch.stepPending, 0);
        if (ep->ContextRecord) ep->ContextRecord->EFlags &= ~kTrapFlag;
        // THE SECOND READ. The store has retired, so the cell now holds the
        // value the writer meant to put there. Same value as before: a restore,
        // which is what a healthy rig does hundreds of times a frame and which
        // the timeline must not spend a line or a budget slot on. Different
        // value: a FLIP, and the whole point of the instrument.
        if (InterlockedCompareExchange(&g_watch.pendingLive, 0, 0)) {
            InterlockedExchange(&g_watch.pendingLive, 0);
            // Readable without a guard: the instruction that just faulted wrote
            // to this exact address and completed, so the page is mapped.
            void* const now = *reinterpret_cast<void* const*>(g_watch.pendingCell);
            if (now == g_watch.pendingOld) {
                ++g_flip.idempotent;
            } else {
                recordFlip(g_watch.pendingSlot, g_watch.pendingOld, now,
                           g_watch.pendingRip, g_watch.pendingThread,
                           g_watch.pendingQpc, g_watch.pendingStack,
                           g_watch.pendingStackCount);
            }
        }
        if (!g_watch.finished) {
            DWORD previous = 0;
            if (VirtualProtect(g_watch.pageBase, g_watch.pageSize,
                               g_watch.readOnlyProtect, &previous)) {
                InterlockedExchange(&g_watch.armed, 1);
            }
        }
        return EXCEPTION_CONTINUE_EXECUTION;
    }

    if (er->ExceptionCode != EXCEPTION_ACCESS_VIOLATION) {
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
    if (!g_watch.pageBase) return EXCEPTION_CONTINUE_SEARCH;
    const uintptr_t at = static_cast<uintptr_t>(er->ExceptionInformation[1]);
    const uintptr_t lo = reinterpret_cast<uintptr_t>(g_watch.pageBase);
    if (at < lo || at >= lo + g_watch.pageSize) return EXCEPTION_CONTINUE_SEARCH;

    // A SECOND THREAD FAULTED INSIDE OUR OWN CATCH WINDOW, and the answer is
    // NOT to walk away from it.
    //
    // The armed test used to be the first thing this handler did, so an
    // exception arriving while armed was 0 went straight to
    // EXCEPTION_CONTINUE_SEARCH -- and for a write to OUR page, disarmed by US
    // one instruction ago, there is nobody else to handle it. Nothing else in
    // the process knows why that page is read-only. The exception goes
    // unhandled and the game dies, blamed on EDVR, correctly.
    //
    // The window is two instructions wide, so this needs the D3D11 context
    // being driven from two threads at once, which is exactly what multithread
    // protection permits and what issue #21's rig may well be doing. It was
    // survivable while the probe armed for a burst of 32 writes in one mode;
    // the flip timeline arms for a whole session on a page the runtime writes
    // hundreds of times a frame, which turns a race nobody had hit into one
    // nobody should be running.
    //
    // The page is writable at this moment -- that is what being unarmed means
    // here -- so re-asserting that and resuming lets the store complete. The
    // write is LOST to the instrument rather than to the process, and the count
    // says how many times that happened rather than leaving the gap silent. A
    // VirtualProtect that refuses falls through to the old behaviour, so a
    // genuinely unwritable page cannot spin here forever.
    if (!InterlockedCompareExchange(&g_watch.armed, 0, 0)) {
        DWORD previous = 0;
        if (!VirtualProtect(g_watch.pageBase, g_watch.pageSize,
                            g_watch.writableProtect, &previous)) {
            return EXCEPTION_CONTINUE_SEARCH;
        }
        ++g_watch.concurrent;
        return EXCEPTION_CONTINUE_EXECUTION;
    }

    // Let the write through, then owe ourselves one instruction: set the trap
    // flag and the page comes back the moment the store has retired.
    InterlockedExchange(&g_watch.armed, 0);
    DWORD ignored = 0;
    VirtualProtect(g_watch.pageBase, g_watch.pageSize, g_watch.writableProtect,
                   &ignored);
    if (ep->ContextRecord) {
        ep->ContextRecord->EFlags |= kTrapFlag;
        g_watch.stepThread = static_cast<LONG>(GetCurrentThreadId());
        InterlockedExchange(&g_watch.stepPending, 1);
    } else {
        // No context to step with. Fall back to the frame path, which is a
        // whole frame late and will miss the rest of any sweep -- said nowhere
        // because it has never been observed; the fallback exists so that a
        // missing context cannot silently end the watch.
        InterlockedExchange(&g_watch.rearmWanted, 1);
    }

    ++g_watch.catches;
    if (!g_watch.lo || at < g_watch.lo) g_watch.lo = at;
    if (at > g_watch.hi) g_watch.hi = at;
    bool knownSite = false;
    for (uint32_t i = 0; i < g_watch.siteCount; ++i) {
        if (g_watch.sites[i] == er->ExceptionAddress) { knownSite = true; break; }
    }
    if (!knownSite && g_watch.siteCount < 8) {
        g_watch.sites[g_watch.siteCount++] = er->ExceptionAddress;
    }

    const bool inTable = at >= g_watch.tableLo && at < g_watch.tableHi;
    if (inTable) {
        ++g_watch.inTable;
        const size_t which = (at - g_watch.tableLo) / sizeof(void*);
        if (which < 512) g_watch.slotsWritten[which >> 6] |= 1ull << (which & 63);

        // THE FIRST READ, and everything the frame path will need about this
        // write except what it is about to become. Owed to the single-step
        // handler, which reads the cell again and decides whether anything
        // actually changed. A pending event still outstanding here -- the
        // previous fault's step never arrived -- is counted and replaced: the
        // pair is per-thread by construction (stepThread gates the completion),
        // and an event with no second half is not one to report.
        if (g_watch.timeline) {
            if (InterlockedCompareExchange(&g_watch.pendingLive, 0, 0)) {
                ++g_flip.unfinished;
            }
            const uintptr_t cell = g_watch.tableLo + which * sizeof(void*);
            g_watch.pendingCell = cell;
            g_watch.pendingSlot = which;
            g_watch.pendingOld = *reinterpret_cast<void* const*>(cell);
            g_watch.pendingRip = er->ExceptionAddress;
            g_watch.pendingThread = GetCurrentThreadId();
            g_watch.pendingQpc = static_cast<uint64_t>(qpcNow());
            g_watch.pendingStackCount = captureWriterStack(
                ep->ContextRecord, g_watch.pendingStack, 8);
            InterlockedExchange(&g_watch.pendingLive, 1);
        }
    }

    // Only writes INSIDE the table get a line. A write that merely shares the
    // page is what the last build reported four times and it answered nothing;
    // it is counted, and the summary says how many there were.
    //
    // NEVER with the timeline armed. This is a Log::get().note() from inside a
    // vectored handler, which is survivable at the 32 lines the writer probe
    // takes and is not survivable at the rate a page holding a live dispatch
    // table is written. With the timeline on, the frame path does all the
    // talking.
    if (inTable && !g_watch.timeline && g_watch.reported < kMaxWatchReports) {
        ++g_watch.reported;
        char modBuf[MAX_PATH];
        Log::get().note(
            "VTableHook %s: CAUGHT A WRITE TO THE VTABLE ITSELF. %p was written "
            "-- %s -- by the instruction at %s. This is the author of the value "
            "that keeps reappearing, named directly: every other module name in "
            "this log is the value found in a slot, not whoever stored it. The "
            "write was allowed through and the page taken straight back.",
            g_watch.who, reinterpret_cast<void*>(at),
            at == reinterpret_cast<uintptr_t>(g_watch.slotAddress)
                ? "exactly the watched slot"
                : "another slot of the same table",
            ownerModuleName(er->ExceptionAddress, modBuf, sizeof(modBuf)));
    }

    // THE TIMELINE HAS ITS OWN BUDGET, AND ITS OWN REASONS.
    //
    // Neither cap above applies to it. kMaxWatchCatches bounds an armed probe's
    // cost on a busy page, and with the timeline on that page is the runtime's
    // live dispatch table being re-laid hundreds of times a frame with the same
    // values -- 2000 of those is four frames, and the instrument would stop
    // before the event it exists for. kMaxWatchReports stops the writer probe
    // once it has named enough writers, which is not this instrument's job at
    // all. What is charged instead is the thing the reader is reading: writes
    // that CHANGED something.
    if (g_watch.timeline) {
        if (static_cast<uint32_t>(
                InterlockedCompareExchange(&g_flip.count, 0, 0)) >=
            kMaxFlipEvents) {
            InterlockedExchange(&g_watch.rearmWanted, 0);
            reportWatchSummary();
        }
        return EXCEPTION_CONTINUE_EXECUTION;
    }

    if (g_watch.catches >= kMaxWatchCatches ||
        g_watch.inTable >= kMaxWatchReports) {
        // NOT `finished = true` here. reportWatchSummary sets it itself, and
        // its first line is a guard against printing twice -- so setting it
        // first made the guard fire on the only call there was, and the summary
        // never printed at all. Reported from the field on the run that most
        // needed it: thirty-two catches, then silence where the line that
        // explains them should have been. The flag also stops the single step
        // re-arming, and the summary sets it before it returns, so the ordering
        // is unchanged.
        InterlockedExchange(&g_watch.rearmWanted, 0);
        reportWatchSummary();
    }
    return EXCEPTION_CONTINUE_EXECUTION;
}

// ---- the flip timeline's frame-path half -----------------------------------
//
// Everything below runs on the render thread, never in the handler. That is
// where a VirtualQuery, a GetModuleFileName, a log line and a file append are
// all affordable, and none of them are affordable inside a vectored handler on
// a thread that is mid-store.

// "the render thread" or "OTHER thread N", because which one it was is the
// difference between the runtime doing this inside the game's own draw
// submission and something doing it from the side.
const char* flipThreadName(uint32_t tid, char* buf, size_t bufLen) {
    const uint32_t render =
        static_cast<uint32_t>(InterlockedCompareExchange(&g_flip.frameThread, 0, 0));
    if (render && tid == render) return "the render thread";
    _snprintf_s(buf, bufLen, _TRUNCATE, "OTHER thread %lu",
                static_cast<unsigned long>(tid));
    return buf;
}

// Fold one event into the (slot, old, new, writer) table. Four facts, because
// three of them repeat: a runtime alternating one slot between two entries is
// two rows however many thousand times it does it, and the writer is what
// separates "the runtime restored it" from "EDVR patched it back".
void foldFlipShape(const VTableFlip& e) {
    for (uint32_t i = 0; i < g_flip.shapeCount; ++i) {
        FlipShape& s = g_flip.shapes[i];
        if (s.slot == e.slot && s.before == e.before && s.after == e.after &&
            s.writer == e.writer) {
            ++s.count;
            return;
        }
    }
    if (g_flip.shapeCount >= kFlipShapes) {
        g_flip.shapeOverflow = true;
        return;
    }
    FlipShape& s = g_flip.shapes[g_flip.shapeCount++];
    s.slot = e.slot;
    s.before = e.before;
    s.after = e.after;
    s.writer = e.writer;
    s.count = 1;
}

// The tuple table, across as many lines as it takes. Each row is three module
// names, so four of them is already most of Log::note's budget -- and a table
// that silently truncates at the line length is a table whose last rows, which
// are the newest shapes, are the ones nobody sees.
void printFlipShapes(const char* why) {
    if (!g_flip.shapeCount) return;
    Log::get().note(
        "VTableHook %s: flip timeline, %s -- %u distinct (slot, from, to, "
        "writer) shape(s) over %u value-changing write(s).%s",
        g_watch.who, why, static_cast<unsigned>(g_flip.shapeCount),
        static_cast<unsigned>(InterlockedCompareExchange(&g_flip.count, 0, 0)),
        g_flip.shapeOverflow
            ? " The table is FULL, so later shapes are not counted: whatever is "
              "writing this table is not choosing between a handful of values."
            : "");
    char line[1000];
    line[0] = '\0';
    for (uint32_t i = 0; i < g_flip.shapeCount; ++i) {
        const FlipShape& s = g_flip.shapes[i];
        char a[MAX_PATH], b[MAX_PATH], c[MAX_PATH];
        char one[3 * MAX_PATH];
        _snprintf_s(one, sizeof(one), _TRUNCATE,
                    "  slot %zu: %s -> %s, written by %s, x%u\n", s.slot,
                    ownerModuleName(s.before, a, sizeof(a)),
                    ownerModuleName(s.after, b, sizeof(b)),
                    ownerModuleName(s.writer, c, sizeof(c)), s.count);
        if (strlen(line) + strlen(one) >= sizeof(line)) {
            Log::get().note("%s", line);
            line[0] = '\0';
        }
        strncat_s(line, sizeof(line), one, _TRUNCATE);
    }
    if (line[0]) Log::get().note("%s", line);
}

// What the instrument cost and what it found, once, on the frame path.
void reportFlipSummary(const char* why) {
    if (g_flip.summarised) return;
    if (!g_flip.frames && !InterlockedCompareExchange(&g_flip.count, 0, 0)) return;
    g_flip.summarised = true;
    const uint32_t changes =
        static_cast<uint32_t>(InterlockedCompareExchange(&g_flip.count, 0, 0));
    const double perFrame =
        g_flip.frames ? static_cast<double>(g_watch.catches) /
                            static_cast<double>(g_flip.frames)
                      : 0.0;
    Log::get().note(
        "VTableHook %s: flip timeline %s. %u write(s) to the page were caught "
        "over %llu frame(s), a mean of %.1f per frame -- that is what this probe "
        "costs, in exceptions. Of the writes INSIDE the table, %u changed a "
        "value and %u put the same value back; %u event(s) were overwritten "
        "before the frame path could read them, %u never had their second half, "
        "and %u faulted on another thread while this one was mid-catch and were "
        "let through unrecorded.%s",
        g_watch.who, why, static_cast<unsigned>(g_watch.catches),
        static_cast<unsigned long long>(g_flip.frames), perFrame,
        static_cast<unsigned>(changes),
        static_cast<unsigned>(g_flip.idempotent),
        static_cast<unsigned>(g_flip.lost),
        static_cast<unsigned>(g_flip.unfinished),
        static_cast<unsigned>(g_watch.concurrent),
        changes >= kMaxFlipEvents
            ? " THE CHANGE BUDGET IS SPENT, so the timeline stopped here and "
              "later changes are not recorded."
            : "");
    printFlipShapes("summary");
}

}  // namespace

bool vtableWatchSlot(void** vtable, size_t slot, size_t slotCount,
                     const char* who, bool timeline) {
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
    // The table's own extent, so a write to it can be told from a write that
    // merely shares its page -- the distinction the first cut of this probe
    // could not make, and the one the whole question turns on.
    g_watch.tableLo = reinterpret_cast<uintptr_t>(vtable);
    g_watch.tableHi = g_watch.tableLo + slotCount * sizeof(void*);
    g_watch.pageBase = pageBase;
    g_watch.pageSize = pageSize;
    g_watch.writableProtect = mbi.Protect;
    g_watch.readOnlyProtect = readOnly;
    g_watch.catches = 0;
    g_watch.inTable = 0;
    g_watch.reported = 0;
    g_watch.lo = 0;
    g_watch.hi = 0;
    g_watch.concurrent = 0;
    g_watch.siteCount = 0;
    for (uint32_t i = 0; i < 8; ++i) g_watch.slotsWritten[i] = 0;
    g_watch.finished = false;
    g_watch.timeline = timeline;
    InterlockedExchange(&g_watch.pendingLive, 0);
    strncpy_s(g_watch.who, who ? who : "?", _TRUNCATE);

    resetFlipTimeline();

    // WHICH SLOTS THIS PAGE ACTUALLY COVERS, stated rather than assumed.
    //
    // One page is protected, not the whole table: a table of ~300 entries is
    // 2.4 KB and can straddle a page boundary wherever the heap put it, so a
    // watch anchored on one slot may not see a write to another. A reader who
    // assumes the table is covered and finds a slot missing from the report has
    // been told something false by omission; the range says exactly what was
    // watched.
    const uintptr_t pageLo = reinterpret_cast<uintptr_t>(pageBase);
    const uintptr_t pageHi = pageLo + pageSize;
    const size_t firstCovered =
        pageLo > g_watch.tableLo ? (pageLo - g_watch.tableLo) / sizeof(void*) : 0;
    size_t lastCovered = slotCount ? slotCount - 1 : 0;
    if (pageHi < g_watch.tableHi) {
        lastCovered = (pageHi - g_watch.tableLo) / sizeof(void*) - 1;
    }

    DWORD previous = 0;
    if (!VirtualProtect(pageBase, pageSize, readOnly, &previous)) {
        Log::get().note("VTableHook: could not make the page at %p read-only "
                        "(err %lu), so the write watch is off.",
                        pageBase, GetLastError());
        g_watch.slotAddress = nullptr;
        g_watch.timeline = false;
        return false;
    }
    InterlockedExchange(&g_watch.armed, 1);
    Log::get().note(
        "VTableHook %s: %s ARMED on slot %zu at %p. The table runs %p to %p and "
        "its page is %p (%llu bytes, %s), which covers slots %zu to %zu -- a "
        "write to any slot outside that range is NOT seen. The page is "
        "read-only, every write to it is caught, and the page is taken straight "
        "back after each one, so a whole sweep of stores is seen and not just "
        "the first. Diagnostic only: every write anywhere on this page takes two "
        "exceptions while this is armed.",
        g_watch.who, timeline ? "FLIP TIMELINE" : "WRITE WATCH", slot, addr,
        reinterpret_cast<void*>(g_watch.tableLo),
        reinterpret_cast<void*>(g_watch.tableHi), pageBase,
        static_cast<unsigned long long>(pageSize), protectName(mbi.Protect),
        firstCovered, lastCovered);
    if (timeline) {
        Log::get().note(
            "VTableHook %s: what the timeline records -- a write that puts the "
            "SAME value back is counted and dropped (on a healthy rig there are "
            "hundreds a frame, and they are the reason a frozen copy has never "
            "hurt anyone here); a write that CHANGES a value is stamped with the "
            "frame, the time, the slot, both values, the instruction, the thread "
            "and eight frames of the writer's stack. The first %u get a line "
            "each and the first %u also go to edvr_breadcrumbs.txt, which "
            "survives the process being killed. In the shared hook mode EDVR's "
            "own re-patching is a change like any other and appears here under "
            "EDVR's own module name, which is the honest answer rather than a "
            "filtered one. Set advanced.vtable_flip_timeline back to 0 "
            "afterwards.",
            g_watch.who, static_cast<unsigned>(kFlipFullLines),
            static_cast<unsigned>(kFlipCrumbs));
    } else {
        Log::get().note(
            "VTableHook %s: only writes INSIDE the table are reported; the rest "
            "are counted and summarised when it stops. Set "
            "advanced.vtable_writer_probe back to 0 afterwards.",
            g_watch.who);
    }
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

void vtableWatchFrameTick(uint64_t frameNo) {
    if (!g_watch.timeline) return;
    // The frame the handler stamps events with, and which thread the render
    // thread is. Published before the drain, so an event arriving during this
    // frame is stamped with this frame's number.
    InterlockedExchange64(&g_flip.frameNo, static_cast<LONG64>(frameNo));
    InterlockedExchange(&g_flip.frameThread,
                        static_cast<LONG>(GetCurrentThreadId()));
    ++g_flip.frames;

    const uint32_t total =
        static_cast<uint32_t>(InterlockedCompareExchange(&g_flip.count, 0, 0));
    while (g_flip.reported < total) {
        const uint32_t index = g_flip.reported;
        if (total - index > kFlipRing) {
            // The handler lapped the ring between two frames. Skip to what is
            // still there and COUNT what was missed: a timeline with a silent
            // hole in it is worse than one that says where the hole is.
            g_flip.lost += (total - index) - kFlipRing;
            g_flip.reported = total - kFlipRing;
            continue;
        }
        const uint32_t at = index % kFlipRing;
        const LONG64 want = static_cast<LONG64>(index) + 1;
        VTableFlip e;
        if (InterlockedCompareExchange64(&g_flip.ready[at], 0, 0) != want) {
            // Allocated but not yet published, or already overwritten.
            ++g_flip.lost;
            ++g_flip.reported;
            continue;
        }
        e = g_flip.ring[at];
        if (InterlockedCompareExchange64(&g_flip.ready[at], 0, 0) != want) {
            // Overwritten DURING the copy, so the fields may be a mix of two
            // events. Checked on both sides for exactly that reason.
            ++g_flip.lost;
            ++g_flip.reported;
            continue;
        }
        ++g_flip.reported;
        foldFlipShape(e);

        if (index < kFlipFullLines) {
            char from[MAX_PATH], to[MAX_PATH], by[MAX_PATH], tid[64];
            Log::get().note(
                "VTableHook %s: FLIP #%u -- frame %llu, %.4f s after the "
                "timeline armed, slot %zu went from %s to %s. Written by %s, on "
                "%s. This is a write that CHANGED the entry; same-value writes "
                "are counted and not reported.",
                g_watch.who, static_cast<unsigned>(index + 1),
                static_cast<unsigned long long>(e.frame),
                flipSecondsSinceArm(e.qpc), e.slot,
                ownerModuleName(e.before, from, sizeof(from)),
                ownerModuleName(e.after, to, sizeof(to)),
                ownerModuleName(e.writer, by, sizeof(by)),
                flipThreadName(e.thread, tid, sizeof(tid)));
            // The stack on its own line: eight module+offset frames do not fit
            // beside the sentence above, and a line that truncates loses the
            // OUTERMOST frames, which are the ones that say who asked.
            char stack[1000];
            stack[0] = '\0';
            for (uint8_t i = 0; i < e.stackCount; ++i) {
                char f[MAX_PATH], one[MAX_PATH + 8];
                _snprintf_s(one, sizeof(one), _TRUNCATE, "%s%s", i ? " <- " : "",
                            ownerModuleName(e.stack[i], f, sizeof(f)));
                if (strlen(stack) + strlen(one) >= sizeof(stack)) break;
                strncat_s(stack, sizeof(stack), one, _TRUNCATE);
            }
            Log::get().note(
                "VTableHook %s: FLIP #%u called from: %s",
                g_watch.who, static_cast<unsigned>(index + 1),
                stack[0] ? stack : "nothing with unwind data above the store");
        }
        if (index < kFlipCrumbs) {
            // The breadcrumb file is unbuffered and outlives a TDR, which is
            // how the sessions this exists for end. Terse on purpose: the line
            // buffer is 256 bytes.
            char from[MAX_PATH], to[MAX_PATH], by[MAX_PATH], crumb[224];
            _snprintf_s(crumb, sizeof(crumb), _TRUNCATE,
                        "gfx: FLIP f=%llu t=%.4f slot=%zu %s -> %s by %s tid=%lu",
                        static_cast<unsigned long long>(e.frame),
                        flipSecondsSinceArm(e.qpc), e.slot,
                        ownerModuleName(e.before, from, sizeof(from)),
                        ownerModuleName(e.after, to, sizeof(to)),
                        ownerModuleName(e.writer, by, sizeof(by)),
                        static_cast<unsigned long>(e.thread));
            breadcrumb(crumb);
        }
    }

    // The tuple table at doublings once the per-event lines stop. A runtime
    // that re-selects every frame must stay visible without filling the log
    // with the fact -- the same cadence every other repeating report here uses.
    const uint32_t now =
        static_cast<uint32_t>(InterlockedCompareExchange(&g_flip.count, 0, 0));
    if (now >= g_flip.nextShapeReport) {
        char why[48];
        _snprintf_s(why, sizeof(why), _TRUNCATE, "%u events in",
                    static_cast<unsigned>(now));
        printFlipShapes(why);
        g_flip.nextShapeReport *= 2;
    }

    // The budget ran out in the handler, which printed the counts it could
    // print from in there. The table and the cost belong here.
    if (g_watch.finished) reportFlipSummary("stopped at the budget");
}

void vtableWatchDumpRecent(const char* why) {
    if (!g_watch.timeline) return;
    if (g_flip.dumps >= kFlipDumps) return;
    const uint32_t total =
        static_cast<uint32_t>(InterlockedCompareExchange(&g_flip.count, 0, 0));
    if (!total) return;
    ++g_flip.dumps;

    const uint32_t want = total < 8 ? total : 8;
    Log::get().note(
        "VTableHook %s: %s -- the last %u of %u value-changing write(s) to the "
        "context's table follow, newest last. If one of them lands in the same "
        "frame as this, the change PRECEDED whatever this line is about; if the "
        "newest is frames old, it did not.",
        g_watch.who, why ? why : "?", static_cast<unsigned>(want),
        static_cast<unsigned>(total));
    for (uint32_t k = want; k > 0; --k) {
        const uint32_t index = total - k;
        const uint32_t at = index % kFlipRing;
        const LONG64 ready = InterlockedCompareExchange64(&g_flip.ready[at], 0, 0);
        if (ready != static_cast<LONG64>(index) + 1) continue;
        const VTableFlip e = g_flip.ring[at];
        char from[MAX_PATH], to[MAX_PATH], by[MAX_PATH], tid[64];
        Log::get().note(
            "VTableHook %s:   flip #%u, frame %llu (%.4f s), slot %zu: %s -> %s, "
            "by %s on %s",
            g_watch.who, static_cast<unsigned>(index + 1),
            static_cast<unsigned long long>(e.frame), flipSecondsSinceArm(e.qpc),
            e.slot, ownerModuleName(e.before, from, sizeof(from)),
            ownerModuleName(e.after, to, sizeof(to)),
            ownerModuleName(e.writer, by, sizeof(by)),
            flipThreadName(e.thread, tid, sizeof(tid)));
        char crumb[224];
        _snprintf_s(crumb, sizeof(crumb), _TRUNCATE,
                    "gfx: %s / flip #%u f=%llu t=%.4f slot=%zu %s -> %s",
                    why ? why : "?", static_cast<unsigned>(index + 1),
                    static_cast<unsigned long long>(e.frame),
                    flipSecondsSinceArm(e.qpc), e.slot,
                    ownerModuleName(e.before, from, sizeof(from)),
                    ownerModuleName(e.after, to, sizeof(to)));
        breadcrumb(crumb);
    }
}

uint32_t vtableWatchFlips() {
    return static_cast<uint32_t>(InterlockedCompareExchange(&g_flip.count, 0, 0));
}

uint32_t vtableWatchIdempotentWrites() { return g_flip.idempotent; }

uint32_t vtableWatchFlipShapes() { return g_flip.shapeCount; }

bool vtableWatchFlipSummarised() { return g_flip.summarised; }

bool vtableWatchFlipAt(uint32_t index, VTableFlip* out) {
    if (!out) return false;
    const uint32_t total =
        static_cast<uint32_t>(InterlockedCompareExchange(&g_flip.count, 0, 0));
    if (index >= total) return false;
    const uint32_t at = index % kFlipRing;
    if (InterlockedCompareExchange64(&g_flip.ready[at], 0, 0) !=
        static_cast<LONG64>(index) + 1) {
        return false;
    }
    *out = g_flip.ring[at];
    return true;
}

uint32_t vtableWatchCatches() { return g_watch.catches; }

bool vtableWatchSummarised() { return g_watch.finished; }

void vtableWatchStop() {
    if (!g_watch.slotAddress) return;
    // A session that ends before the budget still deserves its summary -- the
    // span and the slot set are the whole product, and a watch that caught
    // twenty things and then had the game closed on it should not take them
    // with it. No-op when the budget already printed one.
    if (g_watch.catches) reportWatchSummary();
    InterlockedExchange(&g_watch.armed, 0);
    InterlockedExchange(&g_watch.rearmWanted, 0);
    InterlockedExchange(&g_watch.pendingLive, 0);
    DWORD previous = 0;
    VirtualProtect(g_watch.pageBase, g_watch.pageSize, g_watch.writableProtect,
                   &previous);
    // The timeline's own closing report -- the tuple table and the cost per
    // frame -- on this thread rather than the handler's, because it resolves a
    // module per row. Before the flag goes, since it is what gates the report.
    reportFlipSummary("disarmed");
    g_watch.timeline = false;
    // The handler stays registered. Removing it would race any thread already
    // inside it, and an unarmed handler is a compare and a return.
    g_watch.slotAddress = nullptr;
    g_watch.pageBase = nullptr;
    g_watch.pageSize = 0;
    g_watch.catches = 0;
}

const char* vtableOwnerModuleName(void* p, char* buf, size_t bufLen) {
    return ownerModuleName(p, buf, bufLen);
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

// THE LIVE STUB PAGE. One twelve-byte trampoline per entry, each reading the
// runtime's OWN slot and jumping through it at the moment of the call.
//
//     48 B8 <8 bytes>   mov rax, &m_vtable[i]   -- the slot's ADDRESS, not its
//                                                  contents; the contents are
//                                                  read afresh by the next
//                                                  instruction, every call
//     FF 20             jmp qword ptr [rax]
//     CC CC CC CC       padding to sixteen
//
// The address is an immediate rather than rip-relative because the stub has to
// reach a heap cell arbitrarily far from wherever VirtualAlloc put the page, and
// a 32-bit displacement cannot promise that. Ten bytes buys unconditional
// reach.
//
// Written READ-WRITE and then flipped to EXECUTE-READ, never allocated RWX: a
// writable-executable page is the single strongest heuristic every antivirus
// scanner looks for, and EDVR already carries a Defender false positive without
// handing it one. FlushInstructionCache afterwards because these bytes were
// written as data and are about to be fetched as code.
bool VTableHook::buildLiveStubs() {
    const size_t count = m_copy.size();
    if (!count) return false;
    const size_t bytes = count * kLiveStubBytes;
    uint8_t* page = static_cast<uint8_t*>(
        VirtualAlloc(nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!page) {
        Log::get().note("VTableHook: could not allocate %zu bytes for the live "
                        "vtable's jump stubs (err %lu), so the live mode is off "
                        "and the caller keeps the mode it had.",
                        bytes, GetLastError());
        return false;
    }
    for (size_t i = 0; i < count; ++i) {
        uint8_t* s = page + i * kLiveStubBytes;
        void* const cell = static_cast<void*>(&m_vtable[i]);
        s[0] = 0x48;
        s[1] = 0xB8;
        memcpy(s + 2, &cell, sizeof(cell));
        s[10] = 0xFF;
        s[11] = 0x20;
        for (size_t k = 12; k < kLiveStubBytes; ++k) s[k] = 0xCC;
    }
    DWORD previous = 0;
    if (!VirtualProtect(page, bytes, PAGE_EXECUTE_READ, &previous)) {
        Log::get().note("VTableHook: the live vtable's stub page at %p could not "
                        "be made executable (err %lu), so the live mode is off. "
                        "Nothing was installed.",
                        static_cast<void*>(page), GetLastError());
        // Safe to release: no vptr points at it and no thread can be inside it.
        VirtualFree(page, 0, MEM_RELEASE);
        return false;
    }
    FlushInstructionCache(GetCurrentProcess(), page, bytes);
    m_stubs = page;
    m_stubCount = count;
    for (size_t i = 0; i < count; ++i) {
        m_copy[i] = static_cast<void*>(page + i * kLiveStubBytes);
    }
    return true;
}

bool VTableHook::setMode(HookMode mode) {
    // Before any staging: the three mechanisms record patches against different
    // tables (the shared one vs the private one), so a switch after the first
    // replace() would leave patches describing a table we are no longer using.
    if (!m_object || m_committed || !m_patches.empty()) return false;
    if (mode == m_mode) return true;

    if (mode == HookMode::CopyVptr || mode == HookMode::LiveCopy) {
        // Copy as wide a window as is readable, NOT just the executable
        // prefix. Stopping at the first non-code slot builds a table that
        // works until the host calls a method past the cut and reads off the
        // end of our buffer -- uninitialised heap, reproducing only on
        // teardown or a rare interface. Copy generously; let the tail be
        // whatever the original held. reserve() to the same width first, so
        // the vector never reallocates after commit points the vptr at it.
        m_copy.clear();
        m_frozen.clear();
        m_copy.reserve(512);
        for (size_t i = 0; i < 512; ++i) {
            void* entry = nullptr;
            if (!guarded("VTableHook::setMode/copy", [&] { entry = m_vtable[i]; })) break;
            m_copy.push_back(entry);
        }
        if (m_copy.size() < m_execPrefix) {
            m_copy.clear();
            return false;
        }
        if (mode == HookMode::LiveCopy) {
            // The census baseline, taken BEFORE the stubs overwrite m_copy --
            // see m_frozen. The stub build is the only part of this that can
            // fail on its own, and a failure leaves the hook exactly as it was
            // rather than half converted.
            m_frozen = m_copy;
            if (!buildLiveStubs()) {
                m_copy.clear();
                m_frozen.clear();
                return false;
            }
        }
    } else {
        m_copy.clear();
        m_frozen.clear();
    }
    m_mode = mode;
    return true;
}

bool VTableHook::replace(size_t index, void* replacement, void** origOut) {
    if (!m_object || m_committed) return false;
    if (index >= m_execPrefix) return false;

    // The current entry is read through whichever table this mode dispatches
    // by: the shared vtable in place, our private one once CopyVptr or LiveCopy
    // has taken it. All three answer "what a call on this object runs right
    // now", which is what the caller must forward to -- and in LiveCopy that
    // answer is the slot's stub, so the caller's forward resolves the runtime's
    // current entry at every call instead of freezing this one.
    void** table = usesPrivateTable() ? m_copy.data() : m_vtable;
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

// The store that moves the object onto our table, and the only place it
// happens.
//
// One aligned pointer write, which cannot be partial, so there is no rollback
// path to write. It was copied out three times once commitLive() joined
// commit() and commitUnpatched(); three copies of the operation that actually
// changes somebody else's object is three places for the protection dance to
// drift, so there is one.
bool VTableHook::installVptr(const char* why) {
    void** target = reinterpret_cast<void**>(m_object);
    DWORD oldProtect = 0;
    if (!VirtualProtect(target, sizeof(void*), PAGE_READWRITE, &oldProtect)) {
        Log::get().note("VTableHook: VirtualProtect failed on object %p (err %lu)",
                        m_object, GetLastError());
        return false;
    }
    const bool ok = guarded(why, [&] {
        *target = reinterpret_cast<void*>(m_copy.data());
    });
    DWORD ignored = 0;
    VirtualProtect(target, sizeof(void*), oldProtect, &ignored);
    if (!ok) return false;
    m_committed = true;
    return true;
}

bool VTableHook::commitUnpatched() {
    if (!m_object || m_committed) return false;
    if (m_mode != HookMode::CopyVptr) return false;
    if (!m_patches.empty()) return false;   // that is what commit() is for
    if (m_copy.empty()) return false;

    // Into a copy that differs from the original in nothing but its address.
    // See the header for why anyone would want that.
    return installVptr("VTableHook::commitUnpatched/vptr");
}

bool VTableHook::commitLive() {
    if (!m_object || m_committed) return false;
    if (m_mode != HookMode::LiveCopy) return false;
    if (!m_patches.empty()) return false;   // that is what commit() is for
    if (m_copy.empty()) return false;

    // Into a table of stubs that differs from the original in nothing but its
    // address and two jumps. See the header for the experiment this is half of.
    return installVptr("VTableHook::commitLive/vptr");
}

bool VTableHook::commit() {
    if (!m_object || m_committed) return false;
    if (m_patches.empty()) return false;

    if (usesPrivateTable()) {
        // Patch the private table, then point the object at it. No registry
        // entry and no reclaim: the table is unreachable by the table owners
        // this whole registry exists to arbitrate. See the header.
        //
        // In LiveCopy the entry being overwritten is that slot's stub, and the
        // caller already holds it as its forward -- so the patched slot runs our
        // thunk, and our thunk's "original" is the stub that reads the runtime's
        // current entry. Nothing about a thunk body changes between the modes.
        for (const Patch& p : m_patches) {
            if (p.slot < m_copy.size()) m_copy[p.slot] = p.replacement;
        }
        return installVptr("VTableHook::commit/vptr");
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

    if (usesPrivateTable()) {
        if (m_committed) {
            // Restore the vptr this hook found at attach -- but ONLY if the
            // object still dispatches through OUR copy, mirroring the polite
            // in-place uninstall below. For stacked copy hooks unwound in
            // reverse install order (the shipped order), the object does still
            // point at our copy, so this restores the copy underneath and the
            // stack peels as it was built.
            //
            // THAT TEST CARRIES MORE WEIGHT IN LIVE MODE, and it already
            // carries it correctly. A live hook stacked ON TOP of this one has
            // baked the addresses of OUR m_copy cells into its own stubs and
            // dereferences them at every call -- so freeing our table while it
            // is still installed would dangle on the next draw. The test below
            // sees that case as "somebody swapped on top of us", takes the leak
            // path, and never frees. Unwinding in the shipped upper-first order
            // never reaches it.
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
                // it can go. The live mode's STUB PAGE still cannot -- a thread
                // dispatched before the store above is still inside a stub and
                // returns after it -- so that is leaked, always. See m_stubs.
                m_committed = false;
                forgetObject();
                m_copy.clear();
                m_frozen.clear();
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
    // The stub page is NOT freed here, and forgetting the pointer is exactly
    // what makes it a leak rather than a use-after-free. See m_stubs: a thread
    // can be executing inside a stub at this instant, and the page must outlive
    // it. A re-attach builds a fresh one.
    m_stubs = nullptr;
    m_stubCount = 0;
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
    // THE SAME WALK, TWO DIFFERENT SENTENCES, and the difference is not
    // cosmetic. In CopyVptr a changed entry is a hazard: the object dispatches
    // through a table that no longer agrees with the runtime's. In LiveCopy the
    // very same change is the instrument WORKING -- the stub reads the new entry
    // at the next call, and the count is a census of how often the runtime
    // re-selects rather than a report of anything going wrong. Printing the
    // copy-mode wording in live mode would send a reader chasing a fault that
    // cannot exist there, which is the sort of false alarm this file has spent
    // three releases learning to avoid.
    const bool live = (m_mode == HookMode::LiveCopy);
    // What the runtime's table said when we attached. m_copy is that in copy
    // mode; in live mode m_copy holds stubs and m_frozen is the baseline.
    const std::vector<void*>& base = live ? m_frozen : m_copy;
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

    const size_t span = m_execPrefix < base.size() ? m_execPrefix : base.size();
    for (size_t i = 0; i < span; ++i) {
        // What this slot held when we copied it. In copy mode our patches
        // overwrote the copy, so for a patched slot that value lives in the
        // Patch and not in m_copy -- comparing the live table against our own
        // thunk would report every hooked slot as drift, every pass, forever.
        // In live mode m_frozen was taken before anything was staged, so it
        // already holds the runtime's entry for every slot, patched or not
        // (and a live Patch::original is a STUB, which would be wrong here).
        void* copied = base[i];
        if (!live) {
            for (const Patch& p : m_patches) {
                if (p.slot == i) {
                    copied = p.original;
                    break;
                }
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
    if (!drifted) {
        // SAY THE NEGATIVE, once, on the first pass.
        //
        // This returned silently when nothing had drifted, so "no drift" and
        // "the walk never ran" were the same log -- and the walk HAS been dead
        // code once already in this file's history. Worse, the conclusion drawn
        // from that silence ("the runtime re-lays the same values, so a frozen
        // copy is safe") is load-bearing, and it was resting on an absence.
        // First pass, always, whatever the answer: a rig that dies 1.7 seconds
        // in gets exactly one of these and it should still say something.
        if (!m_copyCleanNoted) {
            m_copyCleanNoted = true;
            Log::get().note(
                live
                    ? "VTableHook %s: the context's own table still holds every "
                      "entry it held when EDVR attached -- all %zu of them, "
                      "checked once a second from here on. The live private "
                      "table would have followed a change at the next call; so "
                      "far there has been nothing to follow."
                    : "VTableHook %s: the vtable EDVR copied still matches the "
                      "table it was copied from -- all %zu entries, checked once "
                      "a second from here on. Nothing has drifted, which is what "
                      "the private-copy mode needs to be true and is reported "
                      "here so that its being true is visible rather than merely "
                      "unmentioned.",
                who, span);
        }
        return;
    }

    ++m_copyDriftEvents;
    char modBuf[MAX_PATH];
    if (m_copyDriftEvents == 1) {
        if (live) {
            Log::get().note(
                "VTableHook %s: the runtime has CHANGED %zu of the first %zu "
                "entries of the context's own table (%s), and the first of them "
                "now points into %s. Nothing is stale: this object dispatches "
                "through a private table of jump stubs that read the runtime's "
                "own slot at every call, so the very next call through each of "
                "those went to the NEW entry. This line is a census of how often "
                "the runtime re-selects its own variants -- the same measurement "
                "the private-copy mode reports as DRIFT, taken where it cannot "
                "do any harm. It repeats about once a second and reports again "
                "at doublings.",
                who, drifted, span, slots,
                ownerModuleName(firstNow, modBuf, sizeof(modBuf)));
        } else {
            Log::get().note(
                "VTableHook %s: the vtable EDVR copied has DRIFTED from the table "
                "it was copied FROM -- %zu of the first %zu entries (%s) now hold "
                "something else, and the first of them points into %s. EDVR's "
                "copy still holds what they held at install, and this object "
                "dispatches through the copy, so for any of those that is a "
                "method of this interface, the game is calling the older entry. "
                "That is what the private-copy mode is FOR when the two are "
                "interchangeable, and a genuine hazard when they are not. Nothing "
                "was changed. This check repeats about once a second and reports "
                "again at doublings; if this log ends in a crash, this line is "
                "the one to report.",
                who, drifted, span, slots,
                ownerModuleName(firstNow, modBuf, sizeof(modBuf)));
        }
    } else if ((m_copyDriftEvents & (m_copyDriftEvents - 1)) == 0) {
        Log::get().note(
            live ? "VTableHook %s: runtime re-point #%u (%zu slot(s): %s; first "
                   "points into %s). Followed by the live table at the next call."
                 : "VTableHook %s: copy drift #%u (%zu slot(s): %s; first points "
                   "into %s).",
            who, m_copyDriftEvents, drifted, slots,
            ownerModuleName(firstNow, modBuf, sizeof(modBuf)));
    } else {
        return;
    }
    // THE TWO INSTRUMENTS, JOINED. This walk knows THAT the table moved and
    // samples once a second; the flip timeline knows WHO moved it and WHEN, to
    // the microsecond. Neither answers issue #21 alone -- the question is
    // whether the switch preceded the hang or followed it -- and reading them
    // side by side in one log is what makes the ordering legible. No-op unless
    // advanced.vtable_flip_timeline armed one.
    vtableWatchDumpRecent("the census found the runtime's table has moved");
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
    if (usesPrivateTable()) {
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
            // WHICH slot, and WHAT the entries are -- not just how many.
            //
            // This printed the count alone, and two independent rigs reported
            // it going from 1 to 2 about a second in. That second value is the
            // difference between "the runtime re-lays the same pointers
            // forever" and "the runtime genuinely re-selects", which is the
            // whole question, and the line that saw it could not say what it
            // was. A count is not evidence; a module and an offset are.
            const Patch* busiest = nullptr;
            bool          overflowed = false;
            for (const Patch& q : m_patches) {
                if (!busiest || q.seenCount > busiest->seenCount) busiest = &q;
                if (q.seenOverflow) overflowed = true;
            }
            char entries[3 * MAX_PATH];
            entries[0] = '\0';
            if (busiest) {
                for (uint8_t k = 0; k < busiest->seenCount; ++k) {
                    char modBuf[MAX_PATH];
                    char one[MAX_PATH + 4];
                    _snprintf_s(one, sizeof(one), _TRUNCATE, "%s%s", k ? "; " : "",
                                ownerModuleName(busiest->seen[k], modBuf,
                                                sizeof(modBuf)));
                    strncat_s(entries, sizeof(entries), one, _TRUNCATE);
                }
            }
            Log::get().note(
                "VTableHook %s: reclaim #%u (slot(s) %s, taken by %s). The "
                "busiest is slot %zu, which has been through %u distinct "
                "entr%s: %s.%s",
                who, m_reclaimEvents, slots, adoptedMod ? adoptedMod : "?",
                busiest ? busiest->slot : 0,
                busiest ? busiest->seenCount : 0,
                (busiest && busiest->seenCount == 1) ? "y" : "ies",
                entries[0] ? entries : "none recorded",
                overflowed ? " At least one slot has had more than four, so the "
                             "set is not small."
                           : " A small set means every one of them could be "
                             "hooked directly, which would end this exchange "
                             "for good.");
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
