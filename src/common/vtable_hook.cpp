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

// WHICH TABLES ARE EDVR'S OWN, and how long each of them really is.
//
// Two hooks stack on the one context: exposure commits first, vScreen attaches
// afterwards and therefore reads the vptr exposure already moved. So in every
// private mode the UPPER hook's "runtime table" is the LOWER hook's private
// buffer, and two of its reports are then false by construction:
//
//   * the census walks that buffer against its own copy of it, finds zero
//     drift every pass forever, and prints "the context's own table still
//     holds every entry it held when EDVR attached" -- a confident sentence
//     about a table nothing outside EDVR can write. On the rig whose runtime
//     re-lays the real table every frame, that line is the opposite of true.
//   * the method count is the BUFFER's length, not the interface's. A live
//     lower hook's table is 512 executable stub addresses, so the upper hook
//     announces 512 methods where the exposure hook says 302 -- and "(N
//     methods)" is the number the field has been reading since issue #6 to tell
//     a real context from a wrapper's.
//
// The registry answers both: a hook can ask whether its table belongs to
// another EDVR hook, and if so, how many entries that hook believes in.
struct PrivateTable {
    void**            table = nullptr;
    size_t            logical = 0;   // the OWNER's method count, not the buffer's
    const VTableHook* owner = nullptr;
};
constexpr size_t kMaxPrivateTables = 8;
PrivateTable g_privateTables[kMaxPrivateTables];
size_t       g_privateTableCount = 0;   // under g_slotOwnersLock

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

// Is this table another EDVR hook's private one? Copies out what the caller
// needs rather than handing back a pointer into a table the lock protects.
bool findPrivateTable(void** table, const VTableHook* notThis, size_t* logicalOut) {
    if (!table) return false;
    bool found = false;
    AcquireSRWLockShared(&g_slotOwnersLock);
    for (size_t i = 0; i < g_privateTableCount; ++i) {
        if (g_privateTables[i].table != table) continue;
        if (g_privateTables[i].owner == notThis) continue;
        if (logicalOut) *logicalOut = g_privateTables[i].logical;
        found = true;
        break;
    }
    ReleaseSRWLockShared(&g_slotOwnersLock);
    return found;
}

void registerPrivateTable(void** table, size_t logical, const VTableHook* owner) {
    if (!table) return;
    AcquireSRWLockExclusive(&g_slotOwnersLock);
    if (g_privateTableCount < kMaxPrivateTables) {
        PrivateTable& p = g_privateTables[g_privateTableCount++];
        p.table = table;
        p.logical = logical;
        p.owner = owner;
    }
    ReleaseSRWLockExclusive(&g_slotOwnersLock);
}

void unregisterPrivateTable(const VTableHook* owner) {
    AcquireSRWLockExclusive(&g_slotOwnersLock);
    size_t kept = 0;
    for (size_t i = 0; i < g_privateTableCount; ++i) {
        if (g_privateTables[i].owner != owner) g_privateTables[kept++] = g_privateTables[i];
    }
    g_privateTableCount = kept;
    ReleaseSRWLockExclusive(&g_slotOwnersLock);
}

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

// THE SAME ANSWER, SHORT ENOUGH TO PUT EIGHT OF THEM ON ONE LINE.
//
// ownerModuleName above spends a full path per name on purpose, and for the
// reclaim and census lines -- one name, once a second -- that is right. The
// timeline's lines are different arithmetic: a FLIP line carries three names, a
// stack line carries eight, and Log::note stops at about 1167 bytes. At
// 60-character paths the stack line truncated after four frames, and the frames
// it dropped were the OUTERMOST ones -- the entries that say who CALLED the
// writer, which is the question the stack was captured to answer. A breadcrumb
// is worse: 224 bytes, so three paths did not fit and the writer, printed last,
// was the field that fell off.
//
// So these lines get "d3d11.dll+0x44A14". The one thing a basename cannot say
// on its own is which d3d11.dll -- the game loads EDVR's proxy under exactly
// that name -- so EDVR's own image is prefixed rather than left ambiguous. The
// module handle is this DLL's, taken from the address of this function, once.
const char* ownerModuleBrief(void* p, char* buf, size_t bufLen) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(p, &mbi, sizeof(mbi)) != sizeof(mbi) || !mbi.AllocationBase) {
        return "no module";
    }
    char path[MAX_PATH] = {};
    if (!GetModuleFileNameA(static_cast<HMODULE>(mbi.AllocationBase), path,
                            sizeof(path))) {
        return "no module";
    }
    const char* leaf = path;
    for (const char* c = path; *c; ++c) {
        if (*c == '\\' || *c == '/') leaf = c + 1;
    }
    static HMODULE self = nullptr;
    if (!self) {
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(&ownerModuleBrief), &self);
    }
    _snprintf_s(buf, bufLen, _TRUNCATE, "%s%s+0x%llX",
                (self && mbi.AllocationBase == self) ? "EDVR's own " : "", leaf,
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

// THE WRITE WATCH. The table's pages, made read-only, so the next write to them
// faults and the handler can read the faulting instruction's address out of the
// exception. See the header for why naming the writer needed its own mechanism.
//
// EVERY COUNTER HERE IS ATOMIC, and that is not defensive tidying. The page the
// D3D11 context's table lives on is written from whatever threads the runtime
// drives the context from, this handler runs on the writing thread, and a
// plain `++` is a read-modify-write with no atomicity at all -- so two writers
// produce a catch count that is simply wrong, and `sites[siteCount++]` produces
// two threads claiming the same index or one claiming index 8 of an array of
// eight. The numbers this instrument prints are the whole product; a number
// nobody can trust is worse than no number.
struct WriteWatch {
    void*     slotAddress = nullptr;
    uintptr_t tableLo = 0;           // the vtable array, so a write can be told
    uintptr_t tableHi = 0;           // from a write merely sharing its pages
    void*     pageBase = nullptr;    // the protected region: the table's whole
    SIZE_T    pageSize = 0;          // span, rounded out to page boundaries
    DWORD     writableProtect = 0;   // what the pages were, and are put back to
    DWORD     readOnlyProtect = 0;   // the same without write permission
    // ONE WORD FOR THE WHOLE STATE MACHINE: whether the pages are closed, and
    // who is mid-catch. kCatchArmed, kCatchIdle, or a thread id.
    //
    // It was two -- an `armed` flag and an owner -- and no ordering of two words
    // is safe, because a claim is "take armed" THEN "publish the owner" with a
    // VirtualProtect syscall in between. The frame path's re-arm, checking the
    // owner in that gap, saw nobody, armed the pages, and a SECOND thread then
    // claimed while the first was still mid-catch: two owners, and the first
    // one's single step arrived as an orphan. Measured at roughly one run in
    // three of the four-writer cell with the words separate, and never once with
    // them merged. A claim is now a compare-exchange from ARMED to this thread,
    // a release is a compare-exchange from this thread back to IDLE, and neither
    // can be observed half done.
    volatile LONG catchState = 0;    // set to kCatchIdle at arming, see below
    volatile LONG rearmWanted = 0;   // the frame-path fallback, see vtableWatchRearm
    // When the frame-path re-arm first held off for the catch it is holding
    // for, as a QPC stamp, and whose catch that was; 0 when it is not holding.
    // Frame path only, so no atomics. See vtableWatchRearm.
    uint64_t  rearmHeldSince = 0;
    LONG      rearmHeldOwner = 0;
    // A budget ran out and the frame path must stop the watch. Set in the
    // handler, acted on from vtableWatchFrameTick -- see reportWatchSummary for
    // why the handler may not do the stopping itself.
    volatile LONG stopWanted = 0;
    volatile LONG stopReason = 0;    // one of kStop* below
    bool      finished = false;
    volatile LONG catches = 0;       // writes seen anywhere on the pages
    volatile LONG inTable = 0;       // ...of which, inside the vtable
    volatile LONG reported = 0;      // in-table lines printed
    // Writes that faulted while the watch was mid-catch on another thread: let
    // through, not recorded, counted. A gap in an instrument's coverage has to
    // be a number rather than a silence. See the handler.
    volatile LONG concurrent = 0;
    // Single steps that arrived without matching the pair we owed ourselves.
    // Must be zero; see vtableWatchChimeras.
    volatile LONG chimera = 0;
    // The thread whose catch the two-second escape gave up on, and how many
    // late steps have arrived for one. See vtableWatchEscapedSteps: the escape
    // is something this file CHOSE to do, so the step it orphans must not be
    // counted beside the one the summary calls impossible.
    volatile LONG escapedOwner = 0;
    volatile LONG escaped = 0;
    volatile LONG64 lo = 0;          // span of addresses written
    volatile LONG64 hi = 0;
    void*     sites[8] = {};         // distinct writing instructions
    volatile LONG siteCount = 0;
    // WHICH SLOTS, as a bitmap over the 512 the probe will address. The scope
    // question the first proof left open: four draw entries were caught in one
    // burst, and whether the same sweep restores the other nineteen slots on
    // EDVR's list or only the draws decides how much of the permanent fix has
    // to exist. A count cannot answer that; the set can.
    volatile LONG64 slotsWritten[8] = {};
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

// WHERE THE PER-EVENT NARRATION STOPS. A SOFT cap, and the word is the whole
// point.
//
// It was a hard one: at 4000 value-changing writes the timeline disarmed
// itself. In the shared hook mode EDVR's own per-frame re-patching is a change
// like any other -- about 29 of them a frame on issue #21's rig -- so 4000 is
// roughly a second and a half, and the instrument would switch off before the
// event it exists for on precisely the rig it was built for. Now nothing stops
// here: the per-event lines and the breadcrumbs stop (they have their own much
// smaller caps anyway), the (slot, from, to, writer) table goes on folding
// every event, the ring goes on holding the last 128 for the crash dump, and
// the summary says the cap was hit and at which frame. What bounds the COST is
// the catch budget and the cost ceiling below, which are measurements of what
// the probe is doing to the frame rather than of how interesting it is.
constexpr uint32_t kMaxFlipEvents = 4000;

// THE SESSION BUDGET THAT DOES STOP IT, in caught page writes.
//
// kMaxWatchCatches (2000) bounds the writer probe, and the timeline used to
// bypass it entirely -- there was no ceiling of any kind on a session-long
// watch over a page the runtime writes hundreds of times a frame. At the
// measured 3.65 us a catch this is about seven seconds of pure exception
// handling spread over a session, which no session this exists for will reach
// (the rig it was built for dies at 1.7 s), and it means a rig that DOES write
// this page without pause stops the instrument loudly instead of running it
// forever.
constexpr uint32_t kMaxTimelineCatches = 2000000;

// What one caught write costs, measured: the fault, the protection change, the
// single step, and the protection change back. 3.65 us on the maintainer's rig,
// timed over the writer probe's own catches. Used only to turn a catches-per-
// frame rate into a milliseconds-per-frame one for the ceiling below; being
// wrong by a factor of two moves the ceiling, it does not break anything.
constexpr double kCatchCostUs = 3.65;

// The cost ceiling, in milliseconds of exception handling per frame, and the
// grace period before it may fire.
//
// A probe that costs 4 ms a frame has taken a third of a 90 Hz frame, which is
// the point at which it is no longer observing the session but changing it. The
// GRACE is the part that matters: the crash this instrument exists for lands
// 1.7 seconds after the hooks arm, so a ceiling that could disarm inside those
// ten seconds could remove the instrument from the one frame it was installed
// to record. It cannot fire until the ten seconds are up, whatever the cost.
constexpr double   kMaxCatchMsPerFrame = 4.0;
constexpr double   kCostCeilingGraceS = 10.0;

// Why a watch stopped, for the frame path's line. The handler cannot log.
constexpr LONG kStopNone = 0;
constexpr LONG kStopCatchBudget = 1;   // the timeline's session catch budget
constexpr LONG kStopWriterProbe = 2;   // the writer probe's own small caps
constexpr LONG kStopCostCeiling = 3;   // milliseconds per frame, over the ceiling

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
    volatile LONG   unfinished = 0; // faults whose single step never completed
    volatile LONG   idempotent = 0; // in-table writes that changed nothing
    FlipShape       shapes[kFlipShapes];
    uint32_t        shapeCount = 0;
    bool            shapeOverflow = false;
    uint32_t        nextShapeReport = kFlipFullLines;
    volatile LONG64 frameNo = 0;
    volatile LONG   frameThread = 0;
    uint64_t        frames = 0;     // frames the timeline has been armed over
    // What the instrument COSTS, per frame, which is the number that decides
    // whether it may go on running. Sampled on the frame path: catches is
    // atomic and monotonic, so the difference between two ticks is this
    // frame's, and no counter of its own is needed in the handler.
    uint32_t        catchesAtLastTick = 0;
    uint32_t        lastFrameCatches = 0;
    uint64_t        armedQpc = 0;
    // When the soft cap was passed, so the summary can say WHEN the per-event
    // narration stopped rather than only that it did. Zero until it is.
    uint64_t        softCapFrame = 0;
    double          softCapSeconds = 0.0;
    uint32_t        dumps = 0;
    // What the last dump was about and how many of its rows fell inside that
    // frame -- for the cell that holds the dump's own sentence to its word.
    uint64_t        lastDumpFrame = 0;
    uint32_t        lastDumpInside = 0;
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
    InterlockedExchange(&g_flip.unfinished, 0);
    InterlockedExchange(&g_flip.idempotent, 0);
    for (uint32_t i = 0; i < kFlipShapes; ++i) g_flip.shapes[i] = FlipShape();
    g_flip.shapeCount = 0;
    g_flip.shapeOverflow = false;
    g_flip.nextShapeReport = kFlipFullLines;
    // NOT frameNo or frameThread: those are the frame path's publication of
    // where the session is, they are published whether or not anything is
    // armed, and zeroing them here would make the first flip after an arming
    // read as frame 0 -- the one number that means "before the first Present".
    g_flip.frames = 0;
    g_flip.catchesAtLastTick = 0;
    g_flip.lastFrameCatches = 0;
    g_flip.armedQpc = static_cast<uint64_t>(qpcNow());
    g_flip.softCapFrame = 0;
    g_flip.softCapSeconds = 0.0;
    g_flip.dumps = 0;
    g_flip.lastDumpFrame = 0;
    g_flip.lastDumpInside = 0;
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
// AND THE UNWINDER ITSELF IS GUARDED, which is not belt-and-braces.
// RtlLookupFunctionEntry and RtlVirtualUnwind read the unwind tables of
// whatever module the faulting instruction is in, and RtlVirtualUnwind reads
// the stack the codes describe -- so a corrupt table, a JIT-generated frame
// with a bad entry, or a stack that has already been smashed faults INSIDE the
// unwind, on a thread that is already inside a fault handler. That is a double
// fault with EDVR at the top of it. Guarded, the walk simply stops and the
// event carries the frames it had.
uint8_t captureWriterStack(const CONTEXT* from, void** out, uint8_t max) {
    if (!from || !out) return 0;
    // A copy, because unwinding mutates the context it walks and the one we
    // were handed is what the CPU resumes into.
    CONTEXT ctx = *from;
    uint8_t n = 0;
    while (n < max) {
        DWORD64 imageBase = 0;
        PRUNTIME_FUNCTION fn = nullptr;
        if (!guarded("captureWriterStack/lookup", [&] {
                fn = RtlLookupFunctionEntry(ctx.Rip, &imageBase, nullptr);
            })) {
            break;
        }
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
        if (!guarded("captureWriterStack/unwind", [&] {
                RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, ctx.Rip, fn, &ctx,
                                 &handlerData, &establisher, nullptr);
            })) {
            break;
        }
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

// The two values of WriteWatch::catchState that are not a thread id. Zero is
// not used for either, so a zero-initialised watch is neither armed nor idle
// and the handler's compare-exchanges all fail against it -- which is the right
// behaviour for a watch nobody has armed.
constexpr LONG kCatchArmed = -1;   // pages read-only, nobody mid-catch
constexpr LONG kCatchIdle = -2;    // pages writable, nobody mid-catch

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
//
// FRAME PATH ONLY, and that is a rule the file's own header states and this
// function used to break: it was called from inside the vectored handler when a
// budget ran out, where it takes the log's lock, formats, writes a file and
// calls GetModuleFileNameA -- which takes the loader lock -- on a thread that
// is mid-store inside the D3D11 runtime. The handler now sets stopWanted and
// vtableWatchFrameTick does the talking.
void reportWatchSummary() {
    if (g_watch.finished) return;
    g_watch.finished = true;
    char siteBuf[MAX_PATH];
    char joined[4 * MAX_PATH];
    joined[0] = '\0';
    const uint32_t siteCount =
        static_cast<uint32_t>(InterlockedCompareExchange(&g_watch.siteCount, 0, 0));
    for (uint32_t i = 0; i < siteCount && i < 8; ++i) {
        char one[MAX_PATH + 4];
        _snprintf_s(one, sizeof(one), _TRUNCATE, "%s%s", i ? "; " : "",
                    ownerModuleName(g_watch.sites[i], siteBuf, sizeof(siteBuf)));
        strncat_s(joined, sizeof(joined), one, _TRUNCATE);
    }
    // THE SET, which is the answer the count could never give.
    char slots[512];
    slots[0] = '\0';
    for (size_t i = 0; i < 512; ++i) {
        const LONG64 word = InterlockedCompareExchange64(
            &g_watch.slotsWritten[i >> 6], 0, 0);
        if (static_cast<uint64_t>(word) & (1ull << (i & 63))) {
            appendSlot(slots, sizeof(slots), i);
        }
    }
    Log::get().note(
        "VTableHook %s: write watch DISARMED. It saw %u write(s) to the watched "
        "pages, %u of them INSIDE the vtable itself, spanning %p to %p; the "
        "vtable occupies %p to %p. The slots written were: %s. The "
        "instruction(s) responsible: %s. If that slot list covers everything "
        "EDVR patches, one sweep restores the whole table; if it is only part of "
        "it, the rest are being lost some other way and that is a different "
        "question. The pages are back to normal and nothing further is "
        "intercepted.",
        g_watch.who,
        static_cast<unsigned>(InterlockedCompareExchange(&g_watch.catches, 0, 0)),
        static_cast<unsigned>(InterlockedCompareExchange(&g_watch.inTable, 0, 0)),
        reinterpret_cast<void*>(InterlockedCompareExchange64(&g_watch.lo, 0, 0)),
        reinterpret_cast<void*>(InterlockedCompareExchange64(&g_watch.hi, 0, 0)),
        reinterpret_cast<void*>(g_watch.tableLo),
        reinterpret_cast<void*>(g_watch.tableHi),
        slots[0] ? slots : "none -- no write landed inside the table at all",
        joined[0] ? joined : "none recorded");
    if (g_watch.timeline) {
        // The cost, in the same line as the finding, because a probe whose cost
        // nobody measured is a probe nobody can advise a user to run -- and the
        // per-frame figure is the one that says whether the instrument was
        // observing the session or changing it.
        const uint32_t catches =
            static_cast<uint32_t>(InterlockedCompareExchange(&g_watch.catches, 0, 0));
        const double perFrame =
            g_flip.frames ? static_cast<double>(catches) /
                                static_cast<double>(g_flip.frames)
                          : 0.0;
        Log::get().note(
            "VTableHook %s: flip timeline -- %u write(s) into the table CHANGED "
            "a value, %u put the same value back and cost nothing but an "
            "exception. Catches cost a mean of %.1f per frame over %llu frame(s) "
            "(~%.2f ms/frame at %.2f us each), and the last frame took %u. The "
            "per-writer table follows on the next frame.",
            g_watch.who,
            static_cast<unsigned>(InterlockedCompareExchange(&g_flip.count, 0, 0)),
            static_cast<unsigned>(InterlockedCompareExchange(&g_flip.idempotent, 0, 0)),
            perFrame, static_cast<unsigned long long>(g_flip.frames),
            perFrame * kCatchCostUs / 1000.0, kCatchCostUs,
            static_cast<unsigned>(g_flip.lastFrameCatches));
    }
}

// Does the watch still want the pages read-only? False once a budget has asked
// for the stop, or once the summary has printed -- the two states in which the
// next write must go through without an exception.
bool watchKeepsArming() {
    return !g_watch.finished &&
           !InterlockedCompareExchange(&g_watch.stopWanted, 0, 0);
}

// A budget is spent. The handler may not log, take a lock or resolve a module,
// so all it does is say so and stop re-arming; vtableWatchFrameTick prints the
// reason and does the disarming a few milliseconds later, on the render thread.
void requestWatchStop(LONG reason) {
    InterlockedCompareExchange(&g_watch.stopReason, reason, kStopNone);
    InterlockedExchange(&g_watch.stopWanted, 1);
    InterlockedExchange(&g_watch.rearmWanted, 0);
}

LONG CALLBACK writeWatchHandler(EXCEPTION_POINTERS* ep) {
    const EXCEPTION_RECORD* er = ep ? ep->ExceptionRecord : nullptr;
    if (!er) return EXCEPTION_CONTINUE_SEARCH;

    // THE SECOND HALF OF EVERY CATCH. The write we let through has now
    // happened, so take the pages back before anything else touches them.
    //
    // This is what the first cut of the probe lacked, and it is why that cut
    // could only ever see one write per frame: it opened the page and waited
    // for the frame path to close it, so an entire sweep of stores went by
    // unseen behind the first one. Re-protecting here instead means every
    // write is caught, which is the difference between "somebody wrote near our
    // table" and "somebody wrote OUR TABLE".
    // ANY SINGLE STEP, WHILE THIS HANDLER HAS EVER ARMED A WATCH -- not only
    // while one is armed NOW. The trap flag is ours and nothing else in this
    // process sets one, so a step arriving after the watch has been stopped is
    // still a trap we set, and handing it back kills the process just as surely.
    // g_watchHandler is registered on the first arming and never removed, which
    // makes it exactly the test "EDVR has used the trap flag in this process".
    if (er->ExceptionCode == EXCEPTION_SINGLE_STEP && g_watchHandler) {
        // Claim the release, do not merely test it: the state word says who owns
        // the catch, and letting go of it has to be one operation for the same
        // reason taking it does. IDLE, not ARMED -- the pages are still writable
        // at this instant and saying otherwise is the blindness below.
        const LONG me = static_cast<LONG>(GetCurrentThreadId());
        const bool mine =
            InterlockedCompareExchange(&g_watch.catchState, kCatchIdle, me) == me;
        if (!mine) {
            // NEVER HAND BACK A TRAP WE SET.
            //
            // A single step arriving here while a watch exists and not owned by
            // this thread is, one way or another, ours: the trap flag is not
            // something anything else in this process sets. Passing it on means
            // EXCEPTION_CONTINUE_SEARCH, and nobody downstream handles
            // STATUS_SINGLE_STEP -- the process dies, EDVR is blamed, and
            // correctly. Measured before the claim below became atomic: four
            // writer threads, all dead inside 50 ms, three runs out of three.
            // Ownership is one word now, so this must never fire; the count is
            // what says so rather than an assumption that it does not.
            //
            // EXCEPT for the one case this file causes on purpose: the frame
            // path's re-arm gives up on a catch after two seconds and takes the
            // pages back, and if that thread then turns up after all its step
            // owns nothing through no fault of its own. Counting that beside a
            // number the summary calls "must be zero" would make the summary
            // lie about its own escape hatch. One late step per escape.
            if (InterlockedCompareExchange(&g_watch.escapedOwner, 0, me) == me) {
                InterlockedIncrement(&g_watch.escaped);
            } else {
                InterlockedIncrement(&g_watch.chimera);
            }
            if (ep->ContextRecord) ep->ContextRecord->EFlags &= ~kTrapFlag;
            return EXCEPTION_CONTINUE_EXECUTION;
        }
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
                InterlockedIncrement(&g_flip.idempotent);
            } else {
                recordFlip(g_watch.pendingSlot, g_watch.pendingOld, now,
                           g_watch.pendingRip, g_watch.pendingThread,
                           g_watch.pendingQpc, g_watch.pendingStack,
                           g_watch.pendingStackCount);
            }
        }
        // ARMED FIRST, PROTECTED SECOND, and the order is the difference
        // between a watch that loses a few catches and one that goes
        // permanently blind.
        //
        // It was the other way round: re-protect, then set armed. A write
        // arriving in that window takes the concurrent branch below, which makes
        // the pages WRITABLE and returns -- and then this thread sets armed with
        // the pages writable and nothing pending. No write faults ever again,
        // vtableWatchRearm has nothing to put back because rearmWanted is 0, and
        // the instrument reports its findings up to that instant and silence
        // after it. Measured with two writer threads: blind after about 250 ms,
        // 282 catches, then nothing for the rest of the run.
        //
        // This way the window says "armed but writable", which loses the writes
        // that land inside it -- a few microseconds of them -- and cannot lose
        // the watch. A VirtualProtect that refuses hands the job to the frame
        // path rather than leaving the state lying about the pages. And the arm
        // is a compare-exchange FROM idle, so if the frame path or another
        // thread has taken the state in the meantime this leaves it alone rather
        // than stamping ARMED over somebody's catch.
        if (g_watch.pageBase && watchKeepsArming()) {
            const LONG prev = InterlockedCompareExchange(
                &g_watch.catchState, kCatchArmed, kCatchIdle);
            if (prev == kCatchIdle || prev == kCatchArmed) {
                DWORD previous = 0;
                if (!VirtualProtect(g_watch.pageBase, g_watch.pageSize,
                                    g_watch.readOnlyProtect, &previous)) {
                    InterlockedExchange(&g_watch.rearmWanted, 1);
                }
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

    // CLAIM THE CATCH, ATOMICALLY, AND THE WHOLE PROCESS DEPENDS ON IT.
    //
    // This was a read of an `armed` flag with a no-op compare-exchange, a
    // VirtualProtect syscall, and only then a clear -- so two threads faulting
    // on the same page could both read it, both come through, both set the trap
    // flag and both write the one global that says whose step is owed. The
    // loser's single step then failed the ownership test above, fell through to
    // EXCEPTION_CONTINUE_SEARCH, and nothing in the process handles
    // STATUS_SINGLE_STEP: the game died. Measured with four writer threads --
    // all four dead within 50 ms, three runs out of three. ONE compare-exchange
    // takes the state from ARMED to this thread, so exactly one thread owns a
    // catch at a time and every other faulting thread takes the branch below.
    //
    // THE LOSER'S WRITE IS NOT LOST TO THE PROCESS, only to the instrument. The
    // pages are writable at this moment -- that is what being unarmed means
    // here -- so re-asserting that and resuming lets the store complete, the
    // count says how many times that happened rather than leaving the gap
    // silent, and rearmWanted asks the frame path to close the pages again in
    // case this thread's catch was the one that was meant to. A VirtualProtect
    // that refuses falls through to the old behaviour, so a genuinely
    // unwritable page cannot spin here forever.
    const LONG me = static_cast<LONG>(GetCurrentThreadId());
    if (InterlockedCompareExchange(&g_watch.catchState, me, kCatchArmed) !=
        kCatchArmed) {
        DWORD previous = 0;
        if (!VirtualProtect(g_watch.pageBase, g_watch.pageSize,
                            g_watch.writableProtect, &previous)) {
            return EXCEPTION_CONTINUE_SEARCH;
        }
        // THE CATCH'S OWN THREAD CAN LAND HERE, and it is not a lost write when
        // it does. A thread that has already claimed and had its pages closed
        // again underneath it -- by the frame path, or by another thread's step
        // -- faults a second time on the same store, finds the catch taken (by
        // itself), opens the pages and carries on with its trap flag and its
        // ownership intact. Its step still completes normally. Measured at one
        // or two occurrences per 300 ms of four-thread hammering.
        InterlockedIncrement(&g_watch.concurrent);
        InterlockedExchange(&g_watch.rearmWanted, 1);
        return EXCEPTION_CONTINUE_EXECUTION;
    }

    // Let the write through, then owe ourselves one instruction: set the trap
    // flag and the pages come back the moment the store has retired.
    DWORD ignored = 0;
    VirtualProtect(g_watch.pageBase, g_watch.pageSize, g_watch.writableProtect,
                   &ignored);
    if (ep->ContextRecord) {
        ep->ContextRecord->EFlags |= kTrapFlag;
        // Ownership is already ours -- the compare-exchange above wrote this
        // thread's id into the state word. There is nothing further to publish,
        // which is the point: it cannot be half claimed, and nothing else here
        // has to be reached for the frame path to know a catch is outstanding.
        // See vtableWatchRearm: it reads the STATE, not a flag this handler
        // might be interrupted before setting.
    } else {
        // No context to step with, so no step will ever come: hand the state
        // back and let the frame path close the pages. A whole frame late, and
        // it will miss the rest of any sweep -- said nowhere because it has
        // never been observed; the fallback exists so that a missing context
        // cannot silently end the watch.
        InterlockedCompareExchange(&g_watch.catchState, kCatchIdle, me);
        InterlockedExchange(&g_watch.rearmWanted, 1);
    }

    const uint32_t catches =
        static_cast<uint32_t>(InterlockedIncrement(&g_watch.catches));
    for (;;) {
        const LONG64 seen = InterlockedCompareExchange64(&g_watch.lo, 0, 0);
        if (seen && seen <= static_cast<LONG64>(at)) break;
        if (InterlockedCompareExchange64(&g_watch.lo, static_cast<LONG64>(at),
                                         seen) == seen) {
            break;
        }
    }
    for (;;) {
        const LONG64 seen = InterlockedCompareExchange64(&g_watch.hi, 0, 0);
        if (seen >= static_cast<LONG64>(at)) break;
        if (InterlockedCompareExchange64(&g_watch.hi, static_cast<LONG64>(at),
                                         seen) == seen) {
            break;
        }
    }
    // The site table, with the index CLAIMED before it is written. `sites[n++]`
    // from two threads writes one entry twice and, at the boundary, writes
    // sites[8] of an array of eight -- which is the next member of this struct.
    const LONG siteCount = InterlockedCompareExchange(&g_watch.siteCount, 0, 0);
    bool knownSite = false;
    for (LONG i = 0; i < siteCount && i < 8; ++i) {
        if (g_watch.sites[i] == er->ExceptionAddress) { knownSite = true; break; }
    }
    if (!knownSite) {
        for (;;) {
            const LONG n = InterlockedCompareExchange(&g_watch.siteCount, 0, 0);
            if (n >= 8) break;
            if (InterlockedCompareExchange(&g_watch.siteCount, n + 1, n) == n) {
                g_watch.sites[n] = er->ExceptionAddress;
                break;
            }
        }
    }

    const bool inTable = at >= g_watch.tableLo && at < g_watch.tableHi;
    if (inTable) {
        InterlockedIncrement(&g_watch.inTable);
        const size_t which = (at - g_watch.tableLo) / sizeof(void*);
        if (which < 512) {
            InterlockedOr64(&g_watch.slotsWritten[which >> 6],
                            static_cast<LONG64>(1ull << (which & 63)));
        }

        // THE FIRST READ, and everything the frame path will need about this
        // write except what it is about to become. Owed to the single-step
        // handler, which reads the cell again and decides whether anything
        // actually changed.
        //
        // ONE OWNER, so these seven fields cannot be a chimera -- one thread's
        // slot carrying another thread's value and a third's stack. The catch
        // above is claimed with a single atomic exchange, so no second thread
        // reaches here until this one's step has completed; before that claim
        // was atomic, two threads could be in this block at once and the fields
        // are one global set. A pending event still outstanding here -- the
        // previous fault's step never arrived, or the frame path re-armed the
        // pages under a store that had not run yet -- is counted and replaced:
        // an event with no second half is not one to report.
        if (g_watch.timeline) {
            if (InterlockedCompareExchange(&g_watch.pendingLive, 0, 0)) {
                InterlockedIncrement(&g_flip.unfinished);
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
    if (inTable && !g_watch.timeline &&
        static_cast<uint32_t>(InterlockedIncrement(&g_watch.reported)) <=
            kMaxWatchReports) {
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
    // kMaxWatchReports stops the writer probe once it has named enough writers,
    // which is not this instrument's job at all. kMaxWatchCatches (2000) is four
    // frames on a page the runtime re-lays hundreds of times a frame, so the
    // timeline would stop before the event it exists for. What bounds this one
    // is a catch budget three orders of magnitude larger, and the cost ceiling
    // the frame path applies -- and BOTH of them stop it loudly, which the
    // bypass that used to live here did not: with the caps skipped and the
    // change budget hit, the timeline simply left the pages open and said so in
    // a summary printed from inside the handler.
    //
    // The change budget is now soft: it ends the narration, not the instrument.
    // Only the FIRST crossing is stamped, and racily -- two threads can both see
    // the crossing and write the same pair of fields, which costs a frame number
    // that may be one frame out and is not worth a lock in here.
    if (g_watch.timeline) {
        const uint32_t changes =
            static_cast<uint32_t>(InterlockedCompareExchange(&g_flip.count, 0, 0));
        if (changes >= kMaxFlipEvents && !g_flip.softCapFrame) {
            g_flip.softCapFrame = static_cast<uint64_t>(
                InterlockedCompareExchange64(&g_flip.frameNo, 0, 0));
            g_flip.softCapSeconds =
                flipSecondsSinceArm(static_cast<uint64_t>(qpcNow()));
        }
        if (catches >= kMaxTimelineCatches) requestWatchStop(kStopCatchBudget);
        return EXCEPTION_CONTINUE_EXECUTION;
    }

    if (catches >= kMaxWatchCatches ||
        static_cast<uint32_t>(InterlockedCompareExchange(&g_watch.inTable, 0, 0)) >=
            kMaxWatchReports) {
        // NOT reportWatchSummary() from in here, and not `finished = true`
        // either. The summary takes the log's lock and resolves modules, which
        // is a loader-lock call on a thread that is mid-store; and setting the
        // flag first made the guard inside the summary fire on the only call
        // there was, so thirty-two catches were followed by silence in the field.
        // The frame path prints it, a few milliseconds later, off the flag.
        requestWatchStop(kStopWriterProbe);
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

// The writer's stack, on its own line, innermost first -- AND, IF IT WILL NOT
// FIT, THE INNERMOST FRAMES ARE THE ONES THAT GO.
//
// The line used to be built front to back and stopped when the next name would
// not fit, so a deep stack lost its OUTERMOST frames: the entries that name who
// CALLED the writer, which is the only reason the stack is captured at all. The
// innermost frame is already printed in the FLIP line above as the writer. So
// the names are measured first and dropped from the near end until the rest
// fits, with an ellipsis where they were, and what survives is always the part
// that says who asked.
void printWriterStack(uint32_t number, const VTableFlip& e) {
    char names[8][MAX_PATH];
    size_t lengths[8] = {};
    const uint8_t count = e.stackCount < 8 ? e.stackCount : 8;
    for (uint8_t i = 0; i < count; ++i) {
        char one[MAX_PATH];
        strncpy_s(names[i], ownerModuleBrief(e.stack[i], one, sizeof(one)),
                  _TRUNCATE);
        lengths[i] = strlen(names[i]) + 4;   // " <- "
    }
    // Log::note stops at about 1167 bytes and the prefix below takes some of
    // it; 900 leaves room for the sentence and the ellipsis either way.
    constexpr size_t kBudget = 900;
    uint8_t first = 0;
    for (;;) {
        size_t total = 0;
        for (uint8_t i = first; i < count; ++i) total += lengths[i];
        if (total <= kBudget || first + 1 >= count) break;
        ++first;
    }
    char stack[1000];
    stack[0] = '\0';
    if (first) strncat_s(stack, sizeof(stack), "... <- ", _TRUNCATE);
    for (uint8_t i = first; i < count; ++i) {
        if (i > first) strncat_s(stack, sizeof(stack), " <- ", _TRUNCATE);
        strncat_s(stack, sizeof(stack), names[i], _TRUNCATE);
    }
    Log::get().note(
        "VTableHook %s: FLIP #%u called from: %s%s", g_watch.who,
        static_cast<unsigned>(number),
        stack[0] ? stack : "nothing with unwind data above the store",
        first ? "  (the innermost frames were dropped to fit; the outermost, "
                "which say who asked, are kept)"
              : "");
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
                    ownerModuleBrief(s.before, a, sizeof(a)),
                    ownerModuleBrief(s.after, b, sizeof(b)),
                    ownerModuleBrief(s.writer, c, sizeof(c)), s.count);
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
    const uint32_t catches =
        static_cast<uint32_t>(InterlockedCompareExchange(&g_watch.catches, 0, 0));
    const double perFrame =
        g_flip.frames ? static_cast<double>(catches) /
                            static_cast<double>(g_flip.frames)
                      : 0.0;
    char softCap[240];
    softCap[0] = '\0';
    if (g_flip.softCapFrame || changes >= kMaxFlipEvents) {
        _snprintf_s(softCap, sizeof(softCap), _TRUNCATE,
                    " The per-event narration budget of %u changes ran out at "
                    "frame %llu (%.4f s), so the shapes above cover the whole "
                    "session while the individual lines stop there.",
                    static_cast<unsigned>(kMaxFlipEvents),
                    static_cast<unsigned long long>(g_flip.softCapFrame),
                    g_flip.softCapSeconds);
    }
    Log::get().note(
        "VTableHook %s: flip timeline %s. %u write(s) to the watched pages were "
        "caught over %llu frame(s) -- a mean of %.1f per frame, about %.2f ms of "
        "exception handling per frame at the measured %.2f us each, and %u in "
        "the last frame. That is what this probe costs. Of the writes INSIDE the "
        "table, %u changed a value and %u put the same value back; %u event(s) "
        "were overwritten before the frame path could read them, %u never had "
        "their second half, %u faulted while a catch was already in progress and "
        "were let through unrecorded (that includes a catching thread's own "
        "store faulting a second time after its pages were closed under it, "
        "which loses nothing), %u step(s) arrived for a catch the frame path had "
        "already given up on after two seconds, and %u single step(s) arrived on "
        "a thread that did not own the catch (that last one must be zero).%s",
        g_watch.who, why, static_cast<unsigned>(catches),
        static_cast<unsigned long long>(g_flip.frames), perFrame,
        perFrame * kCatchCostUs / 1000.0, kCatchCostUs,
        static_cast<unsigned>(g_flip.lastFrameCatches),
        static_cast<unsigned>(changes),
        static_cast<unsigned>(InterlockedCompareExchange(&g_flip.idempotent, 0, 0)),
        static_cast<unsigned>(g_flip.lost),
        static_cast<unsigned>(InterlockedCompareExchange(&g_flip.unfinished, 0, 0)),
        static_cast<unsigned>(InterlockedCompareExchange(&g_watch.concurrent, 0, 0)),
        static_cast<unsigned>(InterlockedCompareExchange(&g_watch.escaped, 0, 0)),
        static_cast<unsigned>(InterlockedCompareExchange(&g_watch.chimera, 0, 0)),
        softCap);
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

    // THE WHOLE TABLE, and NOT the whole region. VirtualQuery reports the entire
    // run of pages sharing a protection, which on a heap can be megabytes;
    // making all of that read-only would fault on every unrelated write in it
    // and bring the game to a halt. So the protected range is exactly the
    // table's own span rounded out to page boundaries -- one page for a small
    // table, two for a ~300-entry one that straddles, and one VirtualProtect
    // call either way.
    //
    // It was ONE page, anchored on the watched slot. A 302-entry table is 2416
    // bytes, so wherever the heap puts it there is about a one-in-four chance
    // that the anchor's page does not reach the far end of the table -- and on
    // the reporter's rig the far end is slot 137, twenty-four of whose
    // neighbours are the whole question. A slot outside the protected page
    // produces no fault, no line and no mention, which reads exactly like a slot
    // nobody wrote.
    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    const SIZE_T pageSize = si.dwPageSize ? si.dwPageSize : 4096;
    const uintptr_t tableLo = reinterpret_cast<uintptr_t>(vtable);
    const uintptr_t tableHi = tableLo + slotCount * sizeof(void*);
    uintptr_t spanLo = tableLo & ~static_cast<uintptr_t>(pageSize - 1);
    uintptr_t spanHi = (tableHi + pageSize - 1) & ~static_cast<uintptr_t>(pageSize - 1);
    // A span that reaches into a DIFFERENT allocation, or into pages with a
    // different protection, is not one region to protect and restore: putting
    // the anchor page's protection back onto somebody else's page afterwards
    // would be a change EDVR never announced.
    //
    // BOTH ENDS ARE CHECKED, and only the far one used to be. The anchor slot
    // is 12, so the query above lands on the page holding slot 12 -- and when
    // the table starts late enough in its own first page (about one placement in
    // fifty, with a 4 KB page and a 2.4 KB table) slot 12 is already on the
    // SECOND page and the first one was never queried at all. Its protection
    // would still have been restored as the anchor page's, which is a change to
    // a page EDVR never looked at. Either end disagreeing shrinks the span to
    // the anchor's own page, and the covered-slot line below then says so.
    // (`farEnd`, because `far` is a windows.h macro and this file has been
    // bitten by `near`.)
    if (spanHi - spanLo > pageSize) {
        MEMORY_BASIC_INFORMATION farEnd{};
        MEMORY_BASIC_INFORMATION nearEnd{};
        const void* lastByte = reinterpret_cast<const void*>(spanHi - 1);
        const void* firstByte = reinterpret_cast<const void*>(spanLo);
        const bool farOk =
            VirtualQuery(lastByte, &farEnd, sizeof(farEnd)) == sizeof(farEnd) &&
            farEnd.AllocationBase == mbi.AllocationBase &&
            farEnd.Protect == mbi.Protect;
        const bool nearOk =
            VirtualQuery(firstByte, &nearEnd, sizeof(nearEnd)) == sizeof(nearEnd) &&
            nearEnd.AllocationBase == mbi.AllocationBase &&
            nearEnd.Protect == mbi.Protect;
        if (!farOk || !nearOk) {
            Log::get().note(
                "VTableHook: the vtable at %p spans more than one page and its "
                "%s is not the same allocation and protection as the page "
                "holding slot %zu, so only that page is watched. Slots on the "
                "other page(s) are NOT seen.",
                static_cast<void*>(vtable),
                !nearOk ? (farOk ? "first page" : "first and last page")
                        : "last page",
                slot);
            spanLo = reinterpret_cast<uintptr_t>(addr) &
                     ~static_cast<uintptr_t>(pageSize - 1);
            spanHi = spanLo + pageSize;
        }
    }
    void* const  pageBase = reinterpret_cast<void*>(spanLo);
    const SIZE_T protectBytes = static_cast<SIZE_T>(spanHi - spanLo);

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
    g_watch.pageSize = protectBytes;
    g_watch.writableProtect = mbi.Protect;
    g_watch.readOnlyProtect = readOnly;
    InterlockedExchange(&g_watch.catches, 0);
    InterlockedExchange(&g_watch.inTable, 0);
    InterlockedExchange(&g_watch.reported, 0);
    InterlockedExchange64(&g_watch.lo, 0);
    InterlockedExchange64(&g_watch.hi, 0);
    InterlockedExchange(&g_watch.concurrent, 0);
    InterlockedExchange(&g_watch.chimera, 0);
    InterlockedExchange(&g_watch.siteCount, 0);
    InterlockedExchange(&g_watch.stopWanted, 0);
    InterlockedExchange(&g_watch.stopReason, kStopNone);
    InterlockedExchange(&g_watch.escaped, 0);
    InterlockedExchange(&g_watch.escapedOwner, 0);
    g_watch.rearmHeldSince = 0;
    g_watch.rearmHeldOwner = 0;
    for (uint32_t i = 0; i < 8; ++i) InterlockedExchange64(&g_watch.slotsWritten[i], 0);
    g_watch.finished = false;
    g_watch.timeline = timeline;
    InterlockedExchange(&g_watch.pendingLive, 0);
    strncpy_s(g_watch.who, who ? who : "?", _TRUNCATE);

    resetFlipTimeline();

    // WHICH SLOTS THE PROTECTED RANGE ACTUALLY COVERS, stated rather than
    // assumed.
    //
    // With the whole span protected this should now always be the whole table,
    // and the line is kept for exactly that reason: it is the assertion a reader
    // can check. If it ever says anything narrower, the far-end refusal above
    // fired and the report that follows has a hole in it with a known shape.
    const uintptr_t pageLo = reinterpret_cast<uintptr_t>(pageBase);
    const uintptr_t pageHi = pageLo + protectBytes;
    const size_t firstCovered =
        pageLo > g_watch.tableLo ? (pageLo - g_watch.tableLo) / sizeof(void*) : 0;
    size_t lastCovered = slotCount ? slotCount - 1 : 0;
    if (pageHi < g_watch.tableHi) {
        lastCovered = (pageHi - g_watch.tableLo) / sizeof(void*) - 1;
    }

    // ARMED BEFORE PROTECTED, the same order the step handler uses and for the
    // same reason: a write faulting between the two would find a state that is
    // not ARMED, take the concurrent branch, open the pages -- and then this
    // line would declare the watch armed over pages anybody can write.
    InterlockedExchange(&g_watch.catchState, kCatchArmed);
    DWORD previous = 0;
    if (!VirtualProtect(pageBase, protectBytes, readOnly, &previous)) {
        Log::get().note("VTableHook: could not make the %llu bytes at %p "
                        "read-only (err %lu), so the write watch is off.",
                        static_cast<unsigned long long>(protectBytes), pageBase,
                        GetLastError());
        InterlockedExchange(&g_watch.catchState, kCatchIdle);
        g_watch.slotAddress = nullptr;
        g_watch.pageBase = nullptr;
        g_watch.pageSize = 0;
        g_watch.timeline = false;
        return false;
    }
    Log::get().note(
        "VTableHook %s: %s ARMED on slot %zu at %p. The table runs %p to %p and "
        "the %llu bytes protected for it start at %p (%s) and cover slots %zu to "
        "%zu -- a write to any slot outside that range is NOT seen. Those pages "
        "are read-only, every write to them is caught, and they are taken "
        "straight back after each one, so a whole sweep of stores is seen and "
        "not just the first. Diagnostic only: every write anywhere in that range "
        "takes two exceptions while this is armed.",
        g_watch.who, timeline ? "FLIP TIMELINE" : "WRITE WATCH", slot, addr,
        reinterpret_cast<void*>(g_watch.tableLo),
        reinterpret_cast<void*>(g_watch.tableHi),
        static_cast<unsigned long long>(protectBytes), pageBase,
        protectName(mbi.Protect), firstCovered, lastCovered);
    if (timeline) {
        Log::get().note(
            "VTableHook %s: what the timeline records -- a write that puts the "
            "SAME value back is counted and dropped (on a healthy rig there are "
            "hundreds a frame, and they are the reason a frozen copy has never "
            "hurt anyone here); a write that CHANGES a value is stamped with the "
            "frame, the time, the slot, both values, the instruction, the thread "
            "and eight frames of the writer's stack. The first %u get a line "
            "each and the first %u also go to edvr_breadcrumbs.txt, which "
            "survives the process being killed; after %u changes the per-event "
            "lines stop and the shape table carries on. EDVR'S OWN WRITES ARE "
            "NOT HERE: every re-patch this DLL makes goes through a function "
            "that makes the page writable first, so it never faults and the "
            "watch never sees it -- and while that page is open, a write by "
            "anybody else lands unseen too. Set advanced.vtable_flip_timeline "
            "back to 0 afterwards.",
            g_watch.who, static_cast<unsigned>(kFlipFullLines),
            static_cast<unsigned>(kFlipCrumbs),
            static_cast<unsigned>(kMaxFlipEvents));
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
    // THE STATE FIRST, AND THE FLAG SECOND, because a flag is something the
    // handler has to REACH and the state is something it has already written.
    //
    // This began with `if (!rearmWanted) return;`, and an ordinary claim set
    // rearmWanted nowhere -- it was written only by the concurrent branch and by
    // a failed VirtualProtect. So a catch whose single step never arrived left
    // the state owned by a thread that would never release it, the pages
    // PAGE_READWRITE, and the two-second escape below unreachable, for the rest
    // of the session, with no counter moving. Measured: suspend a writer
    // mid-catch, tick frames for four seconds, and the pages are still writable
    // and a fresh store is not caught, 5 runs out of 5.
    //
    // Setting the flag at the claim was tried first and is not enough: the claim
    // is a compare-exchange, then a VirtualProtect syscall, then the flag, and a
    // thread stopped anywhere in that window leaves exactly the same unsupervised
    // state -- which is how the cell below failed 5 runs out of 6 against that
    // version. Reading the state costs one compare-exchange per frame and cannot
    // be outrun.
    if (!g_watch.pageBase || !watchKeepsArming()) {
        InterlockedExchange(&g_watch.rearmWanted, 0);
        return;
    }
    // NOT WHILE A STORE IS STILL IN FLIGHT BEHIND AN OPEN PAGE.
    //
    // A thread that has taken a catch is between its fault and its single step
    // with the trap flag set and its store not yet retired. Closing the pages
    // under it faults that store a SECOND time, and the second fault hands the
    // catch to whichever thread claims it -- overwriting the one global that
    // says whose step is owed, so the first thread's step then arrives as an
    // orphan. That is the chimera counter's whole population, and this is the
    // only path in the file that can produce one now.
    //
    // Held, not abandoned: rearmWanted stays set and the next frame tries
    // again. The owner's own step re-protects the pages a microsecond later
    // anyway, so holding costs nothing at all in the ordinary case.
    //
    // The escape exists because a step that never arrives -- a debugger
    // swallowing it, a thread killed mid-catch -- would otherwise leave the
    // pages open for the session, which is the blindness this whole ordering was
    // rewritten to prevent. TWO SECONDS, and not the eight frames it was first
    // written as: eight frames is eight milliseconds, a thread under contention
    // is off the CPU for longer than that routinely, and clearing the flag under
    // a step that is merely LATE manufactures exactly the orphaned step the
    // handler counts as a chimera. Two seconds is past any scheduling delay and
    // still bounded.
    constexpr double kRearmHoldLimitS = 2.0;
    LONG prev = InterlockedCompareExchange(&g_watch.catchState, kCatchArmed,
                                           kCatchIdle);
    if (prev != kCatchIdle && prev != kCatchArmed) {
        // A thread is mid-catch. Hold.
        //
        // The clock is per CATCH, not per hold: now that every claim asks the
        // frame path to look, a busy page has some thread mid-catch on a good
        // many of the frames the frame path samples, and a timer that only
        // restarted on a SUCCESSFUL re-arm would have been measuring "how long
        // since the last frame that happened to land between catches" rather
        // than "how long this catch has been outstanding". Two seconds of that
        // would give up on a catch that was a microsecond old.
        const uint64_t now = static_cast<uint64_t>(qpcNow());
        if (!g_watch.rearmHeldSince || g_watch.rearmHeldOwner != prev) {
            g_watch.rearmHeldSince = now;
            g_watch.rearmHeldOwner = prev;
        }
        const int64_t freq = qpcFrequency();
        const double held =
            freq > 0 ? static_cast<double>(now - g_watch.rearmHeldSince) /
                           static_cast<double>(freq)
                     : 0.0;
        if (held < kRearmHoldLimitS) return;
        // Remember WHOSE catch is being given up on. If that thread turns up
        // afterwards its step owns nothing, and the reason it owns nothing is
        // this line rather than anything the handler got wrong -- so it is
        // counted apart from the chimeras the summary calls impossible.
        InterlockedExchange(&g_watch.escapedOwner, prev);
        InterlockedExchange(&g_watch.catchState, kCatchArmed);
        InterlockedIncrement(&g_flip.unfinished);
    } else if (prev == kCatchArmed &&
               !InterlockedCompareExchange(&g_watch.rearmWanted, 0, 0)) {
        // Armed, nobody mid-catch, and nobody has asked for anything: the pages
        // are as the owner's own step left them, which is closed. This is the
        // ordinary frame and it costs one compare-exchange. (kCatchIdle is NOT
        // this case: the state says nobody owns the catch but nothing has
        // necessarily closed the pages, so that one falls through and protects.)
        g_watch.rearmHeldSince = 0;
        g_watch.rearmHeldOwner = 0;
        return;
    }
    g_watch.rearmHeldSince = 0;
    g_watch.rearmHeldOwner = 0;
    InterlockedExchange(&g_watch.rearmWanted, 0);
    // The state says ARMED from here whether it was idle or already armed; the
    // pages may still be open either way (a concurrent catch opens them without
    // touching the state), so protect unconditionally.
    DWORD previous = 0;
    VirtualProtect(g_watch.pageBase, g_watch.pageSize, g_watch.readOnlyProtect,
                   &previous);
}

// What a stopped watch says on its way out, in the words of whichever budget
// stopped it. From the frame path, because the handler that decided may not
// log; see requestWatchStop.
static void reportWatchStop() {
    const LONG reason = InterlockedCompareExchange(&g_watch.stopReason, 0, 0);
    const uint32_t catches =
        static_cast<uint32_t>(InterlockedCompareExchange(&g_watch.catches, 0, 0));
    const double perFrame =
        g_flip.frames ? static_cast<double>(catches) /
                            static_cast<double>(g_flip.frames)
                      : 0.0;
    if (reason == kStopCatchBudget) {
        Log::get().note(
            "VTableHook %s: THE FLIP TIMELINE HAS SWITCHED ITSELF OFF -- it has "
            "caught %u page writes, which is its whole-session budget of %u. At "
            "the measured %.2f us a catch that is about %.1f seconds of pure "
            "exception handling, and a page this busy is one the instrument is "
            "changing rather than observing. Everything recorded up to here "
            "stands; nothing after it is seen.",
            g_watch.who, static_cast<unsigned>(catches),
            static_cast<unsigned>(kMaxTimelineCatches), kCatchCostUs,
            static_cast<double>(catches) * kCatchCostUs / 1000000.0);
    } else if (reason == kStopCostCeiling) {
        Log::get().note(
            "VTableHook %s: THE FLIP TIMELINE HAS SWITCHED ITSELF OFF BECAUSE OF "
            "WHAT IT COSTS. It is catching a mean of %.1f page writes a frame "
            "(%u in the last frame), which at the measured %.2f us each is about "
            "%.2f ms of exception handling per frame -- over the %.1f ms ceiling, "
            "and enough to change the frame times it was installed to observe. "
            "It ran for the first %.0f seconds whatever the cost, because the "
            "crash this exists for lands 1.7 s in and an instrument that is not "
            "there for it answers nothing. Everything recorded up to here stands.",
            g_watch.who, perFrame,
            static_cast<unsigned>(g_flip.lastFrameCatches), kCatchCostUs,
            perFrame * kCatchCostUs / 1000.0, kMaxCatchMsPerFrame,
            kCostCeilingGraceS);
    } else {
        Log::get().note(
            "VTableHook %s: the write watch has seen enough (%u write(s) caught, "
            "%u of them inside the table) and is disarming itself. The summary "
            "follows.",
            g_watch.who, static_cast<unsigned>(catches),
            static_cast<unsigned>(InterlockedCompareExchange(&g_watch.inTable, 0, 0)));
    }
}

void vtableWatchFrameTick(uint64_t frameNo) {
    // THE FRAME NUMBER IS PUBLISHED WHETHER OR NOT ANYTHING IS ARMED, because it
    // is not the timeline's number: it is the session's, and perf_monitor prints
    // it beside a long frame so that a flip and a hang can be ordered against
    // each other. Two counters that both mean "the frame" is how that ordering
    // gets answered wrongly, which is the whole of finding F10.
    InterlockedExchange64(&g_flip.frameNo, static_cast<LONG64>(frameNo));
    InterlockedExchange(&g_flip.frameThread,
                        static_cast<LONG>(GetCurrentThreadId()));
    if (!g_watch.slotAddress) return;

    // What this frame cost, and the last frame's number kept for the lines that
    // print both. catches is atomic and monotonic, so the difference between two
    // ticks is this frame's and the handler needs no counter of its own.
    const uint32_t catchesNow =
        static_cast<uint32_t>(InterlockedCompareExchange(&g_watch.catches, 0, 0));
    g_flip.lastFrameCatches = catchesNow >= g_flip.catchesAtLastTick
                                  ? catchesNow - g_flip.catchesAtLastTick
                                  : 0;
    g_flip.catchesAtLastTick = catchesNow;
    ++g_flip.frames;

    // A budget spent in the handler, which could not say so. The line, then the
    // disarm, then out: everything below wants a watch that is still running.
    if (InterlockedCompareExchange(&g_watch.stopWanted, 0, 0)) {
        InterlockedExchange(&g_watch.stopWanted, 0);
        reportWatchStop();
        vtableWatchStop();
        return;
    }

    // THE COST CEILING, and the grace period that keeps it from removing the
    // instrument from the frame it exists for.
    //
    // Nothing here may disarm during the first ten seconds: the crash this was
    // built for lands 1.7 s after the hooks arm. After that, a probe costing
    // more than a few milliseconds a frame is changing the session rather than
    // recording it, and it says so and stops. Measured per-catch cost, measured
    // catch rate; no guesswork in the number that appears in the line.
    if (g_watch.timeline && g_flip.frames > 60 &&
        flipSecondsSinceArm(static_cast<uint64_t>(qpcNow())) > kCostCeilingGraceS) {
        const double perFrame =
            static_cast<double>(catchesNow) / static_cast<double>(g_flip.frames);
        if (perFrame * kCatchCostUs / 1000.0 > kMaxCatchMsPerFrame) {
            InterlockedExchange(&g_watch.stopReason, kStopCostCeiling);
            reportWatchStop();
            vtableWatchStop();
            return;
        }
    }

    if (!g_watch.timeline) return;

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
                "are counted and not reported. The watch has caught %u write(s) "
                "this frame, %.1f a frame on average.",
                g_watch.who, static_cast<unsigned>(index + 1),
                static_cast<unsigned long long>(e.frame),
                flipSecondsSinceArm(e.qpc), e.slot,
                ownerModuleBrief(e.before, from, sizeof(from)),
                ownerModuleBrief(e.after, to, sizeof(to)),
                ownerModuleBrief(e.writer, by, sizeof(by)),
                flipThreadName(e.thread, tid, sizeof(tid)),
                static_cast<unsigned>(g_flip.lastFrameCatches),
                g_flip.frames ? static_cast<double>(catchesNow) /
                                    static_cast<double>(g_flip.frames)
                              : 0.0);
            printWriterStack(index + 1, e);
        }
        if (index < kFlipCrumbs) {
            // The breadcrumb file is unbuffered and outlives a TDR, which is
            // how the sessions this exists for end. Terse on purpose: the line
            // buffer is 224 bytes, which is why these names are basenames -- at
            // full paths the writer, printed last, was the field that fell off.
            char from[MAX_PATH], to[MAX_PATH], by[MAX_PATH], crumb[224];
            _snprintf_s(crumb, sizeof(crumb), _TRUNCATE,
                        "gfx: FLIP f=%llu t=%.4f slot=%zu by %s tid=%lu %s -> %s",
                        static_cast<unsigned long long>(e.frame),
                        flipSecondsSinceArm(e.qpc), e.slot,
                        ownerModuleBrief(e.writer, by, sizeof(by)),
                        static_cast<unsigned long>(e.thread),
                        ownerModuleBrief(e.before, from, sizeof(from)),
                        ownerModuleBrief(e.after, to, sizeof(to)));
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

void vtableWatchDumpRecent(const char* why, uint64_t subjectFrame) {
    if (!g_watch.timeline) return;
    if (g_flip.dumps >= kFlipDumps) return;
    const uint32_t total =
        static_cast<uint32_t>(InterlockedCompareExchange(&g_flip.count, 0, 0));
    if (!total) return;
    ++g_flip.dumps;
    g_flip.lastDumpFrame = subjectFrame;
    g_flip.lastDumpInside = 0;

    const uint32_t want = total < 8 ? total : 8;
    // THE HEADER NAMES THE FRAME THE LINE IS ABOUT, which is not always the
    // frame the process is in.
    //
    // It printed the frame in PROGRESS for both callers, and for the monitor
    // those differ by one: the long-frame path runs in the block for frame N
    // about the frame that just ended, N-1. So a flip inside the frame that hung
    // carried N-1, the header said N, and the sentence underneath -- if one of
    // them lands in the same frame as this, the change PRECEDED what this line
    // is about -- told the reader it did NOT precede the hang. The opposite of
    // the truth, on the one question the instrument exists to answer, in the one
    // dump that survives the crash.
    //
    // So the subject is the caller's to state, and the sentence says what each
    // number means rather than leaving the arithmetic to be done under a
    // deadline.
    Log::get().note(
        "VTableHook %s: %s -- this line is about FRAME %llu (the frame in "
        "progress is %llu), %.4f s after the timeline armed, and the watch "
        "caught %u write(s) in the last frame (%.1f a frame on average). The "
        "last %u of %u value-changing write(s) to the context's table follow, "
        "newest last. A change stamped %llu happened INSIDE the frame this line "
        "is about; a smaller number happened BEFORE it; a larger one AFTER it.",
        g_watch.who, why ? why : "?",
        static_cast<unsigned long long>(subjectFrame),
        static_cast<unsigned long long>(
            InterlockedCompareExchange64(&g_flip.frameNo, 0, 0)),
        flipSecondsSinceArm(static_cast<uint64_t>(qpcNow())),
        static_cast<unsigned>(g_flip.lastFrameCatches),
        g_flip.frames
            ? static_cast<double>(InterlockedCompareExchange(&g_watch.catches, 0, 0)) /
                  static_cast<double>(g_flip.frames)
            : 0.0,
        static_cast<unsigned>(want), static_cast<unsigned>(total),
        static_cast<unsigned long long>(subjectFrame));
    for (uint32_t k = want; k > 0; --k) {
        const uint32_t index = total - k;
        const uint32_t at = index % kFlipRing;
        const LONG64 ready = InterlockedCompareExchange64(&g_flip.ready[at], 0, 0);
        if (ready != static_cast<LONG64>(index) + 1) continue;
        const VTableFlip e = g_flip.ring[at];
        // Said per row as well as in the header, because a reader chasing this
        // is reading a crash log under time pressure and one subtraction done
        // wrongly is the whole answer done wrongly.
        const char* when = e.frame == subjectFrame  ? "INSIDE this frame"
                           : e.frame < subjectFrame ? "before it"
                                                    : "after it";
        if (e.frame == subjectFrame) ++g_flip.lastDumpInside;
        char from[MAX_PATH], to[MAX_PATH], by[MAX_PATH], tid[64];
        Log::get().note(
            "VTableHook %s:   flip #%u, frame %llu (%s, %.4f s), slot %zu: %s -> "
            "%s, by %s on %s",
            g_watch.who, static_cast<unsigned>(index + 1),
            static_cast<unsigned long long>(e.frame), when,
            flipSecondsSinceArm(e.qpc),
            e.slot, ownerModuleBrief(e.before, from, sizeof(from)),
            ownerModuleBrief(e.after, to, sizeof(to)),
            ownerModuleBrief(e.writer, by, sizeof(by)),
            flipThreadName(e.thread, tid, sizeof(tid)));
        char crumb[224];
        _snprintf_s(crumb, sizeof(crumb), _TRUNCATE,
                    "gfx: %s / flip #%u f=%llu (%s f=%llu) slot=%zu %s -> %s",
                    why ? why : "?", static_cast<unsigned>(index + 1),
                    static_cast<unsigned long long>(e.frame), when,
                    static_cast<unsigned long long>(subjectFrame), e.slot,
                    ownerModuleBrief(e.before, from, sizeof(from)),
                    ownerModuleBrief(e.after, to, sizeof(to)));
        breadcrumb(crumb);
    }
}

uint64_t vtableWatchLastDumpFrame() { return g_flip.lastDumpFrame; }

uint32_t vtableWatchLastDumpInside() { return g_flip.lastDumpInside; }

uint32_t vtableWatchEscapedSteps() {
    return static_cast<uint32_t>(InterlockedCompareExchange(&g_watch.escaped, 0, 0));
}


uint32_t vtableWatchFlips() {
    return static_cast<uint32_t>(InterlockedCompareExchange(&g_flip.count, 0, 0));
}

uint32_t vtableWatchIdempotentWrites() {
    return static_cast<uint32_t>(InterlockedCompareExchange(&g_flip.idempotent, 0, 0));
}

uint32_t vtableWatchFlipShapes() { return g_flip.shapeCount; }

bool vtableWatchFlipSummarised() { return g_flip.summarised; }

uint64_t vtableWatchFrame() {
    return static_cast<uint64_t>(InterlockedCompareExchange64(&g_flip.frameNo, 0, 0));
}

double vtableWatchSecondsSinceArm() {
    if (!g_watch.slotAddress) return 0.0;
    return flipSecondsSinceArm(static_cast<uint64_t>(qpcNow()));
}

const char* vtableWatchArmedBy() {
    return g_watch.slotAddress ? g_watch.who : nullptr;
}

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

uint32_t vtableWatchCatches() {
    return static_cast<uint32_t>(InterlockedCompareExchange(&g_watch.catches, 0, 0));
}

uint32_t vtableWatchInTableWrites() {
    return static_cast<uint32_t>(InterlockedCompareExchange(&g_watch.inTable, 0, 0));
}

uint32_t vtableWatchChimeras() {
    return static_cast<uint32_t>(InterlockedCompareExchange(&g_watch.chimera, 0, 0));
}

bool vtableWatchSummarised() { return g_watch.finished; }

void vtableWatchStop() {
    if (!g_watch.slotAddress) return;
    // A session that ends before the budget still deserves its summary -- the
    // span and the slot set are the whole product, and a watch that caught
    // twenty things and then had the game closed on it should not take them
    // with it. No-op when the budget already printed one.
    if (InterlockedCompareExchange(&g_watch.catches, 0, 0)) reportWatchSummary();
    // ARMED to IDLE, and only from ARMED: a thread may be between its fault and
    // its step at this instant, and taking its ownership away would make its
    // step an orphan -- which is the fault that kills the process. It releases
    // the state itself, finds the watch finished, and re-protects nothing.
    InterlockedCompareExchange(&g_watch.catchState, kCatchIdle, kCatchArmed);
    InterlockedExchange(&g_watch.rearmWanted, 0);
    InterlockedExchange(&g_watch.pendingLive, 0);
    InterlockedExchange(&g_watch.stopWanted, 0);
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
    InterlockedExchange(&g_watch.catches, 0);
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
    // EXCEPT WHEN THE TABLE IS ANOTHER EDVR HOOK'S, where the probe measures the
    // BUFFER and not the interface. A live lower hook's table is 512 executable
    // stub addresses, so this walks all 512 and the upper hook announces "512
    // methods" where the hook underneath says 302 -- and that number is what the
    // field has been reading since issue #6 to tell a real context from a
    // wrapper's proxy. The registry knows what the lower hook believes; take it.
    size_t lowerLogical = 0;
    if (findPrivateTable(vt, this, &lowerLogical) && lowerLogical &&
        lowerLogical < m_execPrefix) {
        m_execPrefix = lowerLogical;
    }
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
// Written READ-WRITE and then flipped to EXECUTE-READ by sealLiveBlock once the
// patches are in, never allocated RWX: a writable-executable page is the single
// strongest heuristic every antivirus scanner looks for, and EDVR already
// carries a Defender false positive without handing it one.
//
// THE TABLE LIVES AT THE FRONT OF THE SAME ALLOCATION, and that is not tidiness.
// See m_liveBlock: an upper live hook bakes the ADDRESSES of this table's cells
// into its own stubs, so the table has to outlive this hook exactly as the stubs
// do, and a std::vector that uninstall clears does not.
bool VTableHook::buildLiveStubs() {
    const size_t count = m_frozen.size();
    if (!count) return false;
    // The table rounded up to the stub alignment, so every stub still starts on
    // a sixteen-byte boundary and the slot index stays a shift in a disassembly.
    const size_t tableBytes =
        ((count * sizeof(void*)) + kLiveStubBytes - 1) & ~(kLiveStubBytes - 1);
    const size_t bytes = tableBytes + count * kLiveStubBytes;
    uint8_t* block = static_cast<uint8_t*>(
        VirtualAlloc(nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!block) {
        Log::get().note("VTableHook: could not allocate %zu bytes for the live "
                        "vtable and its jump stubs (err %lu), so the live mode "
                        "is off and the caller keeps the mode it had.",
                        bytes, GetLastError());
        return false;
    }
    uint8_t* stubs = block + tableBytes;
    void**   table = reinterpret_cast<void**>(block);
    for (size_t i = 0; i < count; ++i) {
        uint8_t* s = stubs + i * kLiveStubBytes;
        void* const cell = static_cast<void*>(&m_vtable[i]);
        s[0] = 0x48;
        s[1] = 0xB8;
        memcpy(s + 2, &cell, sizeof(cell));
        s[10] = 0xFF;
        s[11] = 0x20;
        for (size_t k = 12; k < kLiveStubBytes; ++k) s[k] = 0xCC;
        table[i] = static_cast<void*>(s);
    }
    m_liveBlock = block;
    m_liveTable = table;
    m_stubCount = count;
    return true;
}

bool VTableHook::sealLiveBlock() {
    if (m_mode != HookMode::LiveCopy) return true;
    if (!m_liveBlock || !m_stubCount) return false;
    const size_t tableBytes =
        ((m_stubCount * sizeof(void*)) + kLiveStubBytes - 1) & ~(kLiveStubBytes - 1);
    const size_t bytes = tableBytes + m_stubCount * kLiveStubBytes;
    DWORD previous = 0;
    if (!VirtualProtect(m_liveBlock, bytes, PAGE_EXECUTE_READ, &previous)) {
        Log::get().note("VTableHook: the live vtable's page at %p could not be "
                        "made executable (err %lu), so the live mode is off. "
                        "Nothing was installed.",
                        static_cast<void*>(m_liveBlock), GetLastError());
        return false;
    }
    // These bytes were written as data and are about to be fetched as code.
    FlushInstructionCache(GetCurrentProcess(), m_liveBlock, bytes);
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
            // The census baseline -- see m_frozen -- and the count the stub
            // build works from. The stub build is the only part of this that
            // can fail on its own, and a failure leaves the hook exactly as it
            // was rather than half converted.
            m_frozen = m_copy;
            if (!buildLiveStubs()) {
                m_copy.clear();
                m_frozen.clear();
                return false;
            }
            // The live table is the block's, not this vector's, and nothing may
            // be left believing otherwise: dispatchTable() answers for both
            // modes and m_copy holds only CopyVptr's.
            m_copy.clear();
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
    void** table = usesPrivateTable() ? dispatchTable() : m_vtable;
    if (!table) return false;
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
    void** const table = dispatchTable();
    if (!table) return false;
    // Sealed here, not in setMode: the live block stays writable until the last
    // patch has gone into its table, and one flip covers both halves of it.
    if (!sealLiveBlock()) return false;
    void** target = reinterpret_cast<void**>(m_object);
    DWORD oldProtect = 0;
    if (!VirtualProtect(target, sizeof(void*), PAGE_READWRITE, &oldProtect)) {
        Log::get().note("VTableHook: VirtualProtect failed on object %p (err %lu)",
                        m_object, GetLastError());
        return false;
    }
    const bool ok = guarded(why, [&] {
        *target = reinterpret_cast<void*>(table);
    });
    DWORD ignored = 0;
    VirtualProtect(target, sizeof(void*), oldProtect, &ignored);
    if (!ok) return false;
    m_committed = true;
    // Now that the object dispatches through it, say so: a hook that later
    // attaches to this same object reads THIS table as "the runtime's", and the
    // registry is the only thing that can tell it otherwise. See PrivateTable.
    registerPrivateTable(table, m_execPrefix, this);
    return true;
}

bool VTableHook::commitUnpatched() {
    if (!m_object || m_committed) return false;
    if (m_mode != HookMode::CopyVptr) return false;
    if (!m_patches.empty()) return false;   // that is what commit() is for
    if (!dispatchCount()) return false;

    // Into a copy that differs from the original in nothing but its address.
    // See the header for why anyone would want that.
    return installVptr("VTableHook::commitUnpatched/vptr");
}

bool VTableHook::commitLive() {
    if (!m_object || m_committed) return false;
    if (m_mode != HookMode::LiveCopy) return false;
    if (!m_patches.empty()) return false;   // that is what commit() is for
    if (!dispatchCount()) return false;

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
        void** const table = dispatchTable();
        const size_t count = dispatchCount();
        if (!table || !count) return false;
        for (const Patch& p : m_patches) {
            if (p.slot < count) table[p.slot] = p.replacement;
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
            // IN LIVE MODE THE TABLE IS NOT FREED AT ALL, whichever branch is
            // taken, and this is where that stopped being a question of order.
            // A live hook stacked ON TOP of this one bakes the ADDRESSES of our
            // table's cells into its own stubs and dereferences them at every
            // call -- so the cells have to outlive us, exactly as our stubs do.
            // They now share one never-freed allocation, so there is nothing
            // here that could dangle no matter which way the stack is unwound.
            // The test below still matters for CopyVptr, whose table IS a
            // vector this function clears.
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
            if (live == static_cast<void*>(dispatchTable())) {
                DWORD oldProtect = 0;
                if (VirtualProtect(target, sizeof(void*), PAGE_READWRITE, &oldProtect)) {
                    guarded("VTableHook::uninstall/vptr", [&] {
                        *target = reinterpret_cast<void*>(m_vtable);
                    });
                    DWORD ignored = 0;
                    VirtualProtect(target, sizeof(void*), oldProtect, &ignored);
                }
                // Restored: nothing dispatches through our copy any more, so
                // it can go. The live mode's BLOCK still cannot -- a thread
                // dispatched before the store above is still inside a stub and
                // returns after it -- so that is leaked, always, table and all.
                // See m_liveBlock.
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
    // The live block is NOT freed here, and forgetting the pointer is exactly
    // what makes it a leak rather than a use-after-free. See m_liveBlock: a
    // thread can be executing inside a stub at this instant, and an upper live
    // hook can still be dereferencing the table's cells, so both must outlive
    // this. A re-attach builds a fresh one.
    m_liveBlock = nullptr;
    m_liveTable = nullptr;
    m_stubCount = 0;
    // Out of the private-table registry, so a hook attaching after this one
    // does not read a table nobody owns as somebody's. Safe on the leak path
    // too: the memory stays mapped, but this object no longer speaks for it.
    unregisterPrivateTable(this);
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
    // A HOOK SITTING ON ANOTHER EDVR HOOK'S TABLE HAS NOTHING TO CENSUS, and
    // saying so is the only honest thing it can do.
    //
    // In every private mode the upper of two stacked hooks attached to an object
    // whose vptr the lower one had already moved, so its "runtime table" is the
    // lower hook's private buffer. Nothing outside EDVR writes that, so the walk
    // below finds zero drift on every pass forever and prints a confident
    // sentence -- "the context's own table still holds every entry it held when
    // EDVR attached" -- about a table that is not the context's. On the rig
    // whose runtime re-lays the real table every frame, that is the opposite of
    // what the log then says. The lower hook's census is the real one and it
    // prints under its own name.
    size_t lowerLogical = 0;
    if (findPrivateTable(m_vtable, this, &lowerLogical)) {
        if (!m_copyCleanNoted) {
            m_copyCleanNoted = true;
            Log::get().note(
                "VTableHook %s: no census from this hook -- it sits on ANOTHER "
                "EDVR hook's private table (%p, %zu entries), not on the one the "
                "context itself holds, and nothing outside EDVR can write that. "
                "The hook underneath is the one whose table the runtime writes "
                "and its census is the measurement; a walk here would report "
                "\"nothing has changed\" every second for the session and mean "
                "nothing by it.",
                who, static_cast<void*>(m_vtable), lowerLogical);
        }
        return;
    }

    const bool live = (m_mode == HookMode::LiveCopy);
    // What the runtime's table said when we attached. m_copy is that in copy
    // mode; in live mode the private table holds stubs and m_frozen is the
    // baseline.
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
    // The census speaks about the frame it is IN -- it has just read the table
    // and found it changed -- so that is the frame it hands the dump.
    vtableWatchDumpRecent("the census found the runtime's table has moved",
                          static_cast<uint64_t>(InterlockedCompareExchange64(
                              &g_flip.frameNo, 0, 0)));
}

void VTableHook::censusTick(const char* name) {
    // The same walk reclaim's private branch runs, reachable without it. The two
    // context probes install no fix, so nothing on the frame path calls reclaim
    // in exactly the sessions that exist to ask what the runtime is doing to the
    // table -- and those sessions reported nothing about it at all.
    if (!m_committed || !usesPrivateTable()) return;
    noteCopyDrift(name ? name : "?");
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
        void** const table = dispatchTable();
        const size_t count = dispatchCount();
        if (live == table && table) {
            for (const Patch& p : m_patches) {
                if (p.slot < count && table[p.slot] != p.replacement &&
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
                        ownerModuleName(table[p.slot], modBuf, sizeof(modBuf)));
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
