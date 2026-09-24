#include "pose_reader_watch.h"
#include "pose_reader_watch_core.h"
#include "../common/code_hook.h"
#include "../common/config.h"
#include "../common/log.h"
#include "../common/frame_flag.h"

#include <windows.h>
#include <tlhelp32.h>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <memory>
#include <new>

namespace edvr {
// prw:: below resolves to edvr::prw (pose_reader_watch_core.h), visible
// unqualified here the same way tfp:: is inside transition_flash_
// prevent.cpp -- both are nested namespaces of edvr, and this file is
// lexically inside edvr already.
namespace {

// --- Identity: build 332841, transition_flash_prevent.cpp's own pair.
// Kept as this file's own copy rather than a shared constant -- the
// copy-culture rule that file states for its own identity check, so
// neither is touched by a change in the other.
constexpr uint32_t kExpectedTimestamp = 1788384820u;
constexpr uint32_t kExpectedImageSize = 104894464u;

// --- Part B: the two positioner functions (design doc, "Static rounds
// 4-5"). First 16 bytes verified two ways: analysis\decomp\flash\r5's
// ghidra dump, and independently by walking analysis\EliteDangerous64.exe's
// own PE section table (RVA -> file offset via PointerToRawData) and
// reading the raw bytes -- see the task report, not committed here.
constexpr uintptr_t kTickRva = 0x107C760u;
constexpr uintptr_t kSwapSyncRva = 0x1090420u;
constexpr uint8_t kTickBytes[16] = {
    0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x10, 0x48, 0x89, 0x70, 0x18, 0x55, 0x57, 0x41, 0x56, 0x48};
constexpr uint8_t kSwapSyncBytes[16] = {
    0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48};

// --- SEH-guarded reads. Each is its own small function, mixing __try with
// this file's other locals (mutexes, std::string) being what CodeHook's
// own doc calls out as refused by the compiler -- object_record_writer_
// probe.cpp's guardedRead and transition_flash_prevent.cpp's sehReadU64
// are the precedent, kept to one dereference each.
__declspec(noinline) bool sehReadU64(uintptr_t address, uint64_t& out) noexcept {
    __try {
        out = *reinterpret_cast<const uint64_t*>(address);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
__declspec(noinline) bool sehReadBlock16(uintptr_t address, uint8_t out[16]) noexcept {
    __try {
        std::memcpy(out, reinterpret_cast<const void*>(address), 16);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
__declspec(noinline) bool sehCheckBytes(uintptr_t address, const uint8_t* expected, size_t n) noexcept {
    __try {
        return std::memcmp(reinterpret_cast<const void*>(address), expected, n) == 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
// PE TimeDateStamp + SizeOfImage, read the same way transition_flash_
// prevent.cpp's checkIdentity does (base+0x3C -> e_lfanew, +8 TimeDateStamp,
// +0x50 SizeOfImage).
__declspec(noinline) bool checkIdentity(uintptr_t base, const char** why) noexcept {
    __try {
        uint32_t peOff = 0;
        std::memcpy(&peOff, reinterpret_cast<const void*>(base + 0x3C), 4);
        if (peOff > 0x1000) { *why = "PE header offset implausible"; return false; }
        uint32_t timestamp = 0, imageSize = 0;
        std::memcpy(&timestamp, reinterpret_cast<const void*>(base + peOff + 8), 4);
        std::memcpy(&imageSize, reinterpret_cast<const void*>(base + peOff + 0x50), 4);
        if (timestamp != kExpectedTimestamp || imageSize != kExpectedImageSize) {
            *why = "not build 332841 (PE timestamp/size mismatch)";
            return false;
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *why = "a read faulted while checking the PE header";
        return false;
    }
}

// --- Relay machinery, mirrored from transition_flash_prevent.cpp (itself
// mirrored from object_record_writer_hook.cpp / kinematic_eval_hook.cpp).
// Kept as a copy rather than a shared unit so none of the flight-proven
// files are touched; if one changes, change all four. Needed because the
// target is in the GAME's module and this DLL loads more than two
// gigabytes away -- a plain five-byte E9 patch cannot reach a replacement
// here directly.
constexpr size_t kRelayBytes = 44, kOriginalLiteral = 36;

uint8_t* allocateRelay(uintptr_t target) noexcept {
    SYSTEM_INFO info{}; GetSystemInfo(&info);
    const uintptr_t granularity = info.dwAllocationGranularity;
    const uintptr_t floor = reinterpret_cast<uintptr_t>(info.lpMinimumApplicationAddress);
    const uintptr_t ceiling = reinterpret_cast<uintptr_t>(info.lpMaximumApplicationAddress);
    const uintptr_t distance = uintptr_t(INT32_MAX) - 0x10000u;
    uintptr_t at = target > distance ? target - distance : floor;
    if (at < floor) at = floor;
    const uintptr_t limit = target > ceiling - distance ? ceiling : target + distance;
    while (at < limit) {
        MEMORY_BASIC_INFORMATION region{};
        if (!VirtualQuery(reinterpret_cast<void*>(at), &region, sizeof(region))) break;
        const uintptr_t start = reinterpret_cast<uintptr_t>(region.BaseAddress);
        if (region.RegionSize > UINTPTR_MAX - start) break;
        const uintptr_t end = start + region.RegionSize;
        if (region.State == MEM_FREE) {
            uintptr_t candidate = at > start ? at : start;
            if (candidate > UINTPTR_MAX - (granularity - 1)) break;
            candidate = (candidate + granularity - 1) & ~(granularity - 1);
            if (candidate < limit && candidate < end && end - candidate >= 4096) {
                auto* p = static_cast<uint8_t*>(VirtualAlloc(reinterpret_cast<void*>(candidate), 4096,
                                                             MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
                if (p) return p;
            }
        }
        if (end <= at) break; at = end;
    }
    return nullptr;
}

void buildRelay(uint8_t* code, const void* gate, void* callback) noexcept {
    // mov rax,&gate; cmp qword ptr[rax],0; je original; jmp [callback];
    // original: jmp [trampoline]. RAX/flags are volatile and neither
    // hooked target consumes RAX on entry.
    const uint8_t body[kRelayBytes] = {
        0x48,0xB8,0,0,0,0,0,0,0,0, 0x48,0x83,0x38,0,
        0x74,0x0E, 0xFF,0x25,0,0,0,0, 0,0,0,0,0,0,0,0,
        0xFF,0x25,0,0,0,0, 0,0,0,0,0,0,0,0};
    std::memcpy(code, body, sizeof(body));
    const uintptr_t gateAddress = reinterpret_cast<uintptr_t>(gate);
    const uintptr_t callbackAddress = reinterpret_cast<uintptr_t>(callback);
    std::memcpy(code + 2, &gateAddress, 8); std::memcpy(code + 22, &callbackAddress, 8);
}

struct HookEntry {
    const char* name;
    uintptr_t rva;
    void* callback;
    std::atomic<uintptr_t> forward{0};
    CodeHook hook;
    uint8_t* relay = nullptr;
    std::atomic<bool> ready{false};
    const char* failReason = "not attempted";
};

bool prepareRelay(void* trampoline, void* context) noexcept {
    auto& entry = *static_cast<HookEntry*>(context);
    const uintptr_t address = reinterpret_cast<uintptr_t>(trampoline);
    std::memcpy(entry.relay + kOriginalLiteral, &address, 8);
    DWORD oldProtect = 0;
    if (!VirtualProtect(entry.relay, 4096, PAGE_EXECUTE_READ, &oldProtect) ||
        !FlushInstructionCache(GetCurrentProcess(), entry.relay, kRelayBytes)) return false;
    entry.forward.store(address, std::memory_order_release);
    return true;
}

// Held open unconditionally once installation starts, transition_flash_
// prevent.cpp's g_relayGate discipline: off/on is made inside the observed
// callbacks themselves (one atomic load), not by gating the relay.
std::atomic<uintptr_t> g_relayGate{0};

bool installOne(HookEntry& entry, uintptr_t base, const uint8_t* expectedBytes) noexcept {
    if (!sehCheckBytes(base + entry.rva, expectedBytes, 16)) {
        entry.failReason = "prologue mismatch at this RVA (not build 332841's shape)";
        return false;
    }
    entry.relay = allocateRelay(base + entry.rva);
    if (!entry.relay) {
        entry.failReason = "relay allocation failed (no free memory within 2GB)";
        return false;
    }
    buildRelay(entry.relay, &g_relayGate, entry.callback);
    if (!entry.hook.install(reinterpret_cast<void*>(base + entry.rva), entry.relay, nullptr,
                            entry.name, &prepareRelay, &entry)) {
        VirtualFree(entry.relay, 0, MEM_RELEASE);
        entry.relay = nullptr;
        entry.failReason = "CodeHook refused it (see its own line above, tagged with this hook's name)";
        return false;
    }
    entry.ready.store(true, std::memory_order_release);
    return true;
}

// --- Part B: the two detours. Forward-declared so the HookEntry instances
// below can take their address before the bodies (which need
// readPositionerSnapshot/notePositionerCall, defined further down) exist.
using TickFn = uint64_t (__fastcall*)(uintptr_t, float);
using SwapSyncFn = void (__fastcall*)(uintptr_t);
__declspec(noinline) uint64_t __fastcall tickObserved(uintptr_t thisPtr, float dt) noexcept;
__declspec(noinline) void __fastcall swapSyncObserved(uintptr_t thisPtr) noexcept;

HookEntry g_tickEntry{"pose-reader-tick", kTickRva, reinterpret_cast<void*>(&tickObserved)};
HookEntry g_swapSyncEntry{"pose-reader-swapsync", kSwapSyncRva, reinterpret_cast<void*>(&swapSyncObserved)};

// --- Module state --------------------------------------------------------
std::atomic<bool> g_on{false};             // advanced.eye_origin_readers, live
std::atomic<bool> g_offLogged{false};
std::atomic<bool> g_hooksInstalled{false}; // part B install-once latch
std::atomic<uint32_t> g_frame{0};

// Part B's per-frame/session counters. Touched from whichever thread calls
// Tick/swap-sync (the design doc: "it runs as a scheduled job"), read and
// reset only from poseReaderWatchFrameBoundary's thread.
std::atomic<uint16_t> g_tickCallsThisFrame{0};
std::atomic<uint16_t> g_swapSyncCallsThisFrame{0};
std::atomic<bool> g_swapDetectedThisFrame{false};
std::atomic<uint64_t> g_positionerCallsTotal{0};
std::atomic<uint64_t> g_swapsSeenTotal{0};
std::atomic<bool> g_swapTriggerPending{false};
std::atomic<uint32_t> g_swapTriggerFrame{0};

// this+0x2A0/+0x450/+0x2A8/+0x2B0 and the 16-byte blocks at +0x880/+0x890
// (design doc, "Static rounds 4-5"): read before and after calling the
// original, in both detours. A swap is +0x2A0 changing across the call.
struct PositionerSnapshot {
    uint64_t cache = 0;   // this+0x2A0, the positioner cache the Tick checks
    uint64_t target = 0;  // this+0x450, the freshly-resolved positioner
    uint64_t off1 = 0;    // this+0x2A8
    uint64_t off2 = 0;    // this+0x2B0
    uint8_t  block880[16] = {};
    uint8_t  block890[16] = {};
    bool     ok = false;  // every guarded read succeeded
};

PositionerSnapshot readPositionerSnapshot(uintptr_t thisPtr) noexcept {
    PositionerSnapshot s{};
    s.ok = sehReadU64(thisPtr + 0x2A0, s.cache) &&
           sehReadU64(thisPtr + 0x450, s.target) &&
           sehReadU64(thisPtr + 0x2A8, s.off1) &&
           sehReadU64(thisPtr + 0x2B0, s.off2) &&
           sehReadBlock16(thisPtr + 0x880, s.block880) &&
           sehReadBlock16(thisPtr + 0x890, s.block890);
    return s;
}

// Counts the call, cheaply (no logging: B's own rule, "log nothing per
// call"), and -- if +0x2A0 moved across it -- latches a dump trigger for
// glitch_frame.cpp to pick up and counts the swap.
void notePositionerCall(bool isTick, const PositionerSnapshot& before, const PositionerSnapshot& after) noexcept {
    if (isTick) g_tickCallsThisFrame.fetch_add(1, std::memory_order_relaxed);
    else g_swapSyncCallsThisFrame.fetch_add(1, std::memory_order_relaxed);
    g_positionerCallsTotal.fetch_add(1, std::memory_order_relaxed);
    if (before.ok && after.ok && before.cache != after.cache) {
        g_swapDetectedThisFrame.store(true, std::memory_order_relaxed);
        g_swapsSeenTotal.fetch_add(1, std::memory_order_relaxed);
        g_swapTriggerFrame.store(g_frame.load(std::memory_order_relaxed), std::memory_order_relaxed);
        g_swapTriggerPending.store(true, std::memory_order_release);
    }
}

uint64_t __fastcall tickObserved(uintptr_t thisPtr, float dt) noexcept {
    const auto forward = reinterpret_cast<TickFn>(g_tickEntry.forward.load(std::memory_order_acquire));
    if (!forward) return 0;
    if (!g_on.load(std::memory_order_relaxed)) return forward(thisPtr, dt);
    const PositionerSnapshot before = readPositionerSnapshot(thisPtr);
    const uint64_t result = forward(thisPtr, dt);
    const PositionerSnapshot after = readPositionerSnapshot(thisPtr);
    notePositionerCall(/*isTick=*/true, before, after);
    return result;
}

void __fastcall swapSyncObserved(uintptr_t thisPtr) noexcept {
    const auto forward = reinterpret_cast<SwapSyncFn>(g_swapSyncEntry.forward.load(std::memory_order_acquire));
    if (!forward) return;
    if (!g_on.load(std::memory_order_relaxed)) { forward(thisPtr); return; }
    const PositionerSnapshot before = readPositionerSnapshot(thisPtr);
    forward(thisPtr);
    const PositionerSnapshot after = readPositionerSnapshot(thisPtr);
    notePositionerCall(/*isTick=*/false, before, after);
}

// --- Part A2: the hardware-breakpoint reader hunt ---------------------

// The game module's [base, base+size), resolved once at arm time (this
// file's own copy of game_call_probe.h's approach) and cached in atomics
// so the VEH handler -- which may not resolve a module -- only ever reads
// them.
std::atomic<uint64_t> g_gameBase{0};
std::atomic<uint64_t> g_gameSize{0};

bool resolveGameModule() noexcept {
    const auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    if (!base) return false;
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
    g_gameBase.store(static_cast<uint64_t>(base), std::memory_order_relaxed);
    g_gameSize.store(static_cast<uint64_t>(nt->OptionalHeader.SizeOfImage), std::memory_order_release);
    return true;
}

// Every thread of the process this session has set DR0 on, so a periodic
// sweep only touches ones it has not already armed and disarm knows which
// to put back. A small fixed table -- Elite's own thread count is nowhere
// near this in practice, and a game whose thread count outgrows it simply
// leaves the excess unarmed, which is a narrower hunt, not a crash.
constexpr uint32_t kMaxArmedThreads = 512;
struct ArmedThreadSet {
    DWORD ids[kMaxArmedThreads]{};
    uint32_t count = 0;
    bool contains(DWORD tid) const noexcept {
        for (uint32_t i = 0; i < count; ++i) if (ids[i] == tid) return true;
        return false;
    }
    bool add(DWORD tid) noexcept {
        if (contains(tid)) return true;
        if (count >= kMaxArmedThreads) return false;
        ids[count++] = tid;
        return true;
    }
    void clear() noexcept { count = 0; }
};
// Touched only from poseReaderWatchFrameBoundary's own thread (arm, the 2s
// re-arm sweep, and disarm all run from there) -- no lock needed.
ArmedThreadSet g_armedThreads;
uint64_t g_watchAddress = 0;

// DR0/Dr7 on one thread, by handle. `arm` true sets DR0=watchAddress and
// arms slot 0; false clears just L0 (pose_reader_watch_core.h's
// disarmSlot0Dr7). Suspend/GetContext/SetContext/Resume, exactly as the
// design specifies for every thread that is not the caller.
bool setThreadDr(DWORD tid, uint64_t watchAddress, bool arm) noexcept {
    HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT, FALSE, tid);
    if (!h) return false;
    bool ok = false;
    if (SuspendThread(h) != static_cast<DWORD>(-1)) {
        CONTEXT ctx{};
        ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
        if (GetThreadContext(h, &ctx)) {
            const uint32_t lowDr7 = static_cast<uint32_t>(ctx.Dr7 & 0xFFFFFFFFull);
            const uint32_t highDr7 = static_cast<uint32_t>((ctx.Dr7 >> 32) & 0xFFFFFFFFull);
            const uint32_t newLowDr7 = arm ? prw::armSlot0Dr7(lowDr7) : prw::disarmSlot0Dr7(lowDr7);
            ctx.Dr7 = (static_cast<DWORD64>(highDr7) << 32) | newLowDr7;
            if (arm) ctx.Dr0 = static_cast<DWORD64>(watchAddress);
            ok = SetThreadContext(h, &ctx) != FALSE;
        }
        ResumeThread(h);
    }
    CloseHandle(h);
    return ok;
}

// The current (calling) thread cannot usefully SuspendThread itself --
// GetThreadContext right after would not see the suspend take effect. A
// short-lived helper thread does the Suspend/GetContext/SetContext/Resume
// from the outside instead. `args` is heap-owned and freed by the helper,
// not the stack frame here, so a timed-out wait below can never leave it
// reading freed memory.
struct HelperDrArgs { DWORD targetTid; uint64_t watchAddress; bool arm; };

// Not noexcept: LPTHREAD_START_ROUTINE has no exception specification.
DWORD WINAPI helperDrThreadProc(LPVOID param) {
    std::unique_ptr<HelperDrArgs> args(static_cast<HelperDrArgs*>(param));
    setThreadDr(args->targetTid, args->watchAddress, args->arm);
    return 0;
}

bool setCurrentThreadDr(uint64_t watchAddress, bool arm) noexcept {
    auto* args = new (std::nothrow) HelperDrArgs{GetCurrentThreadId(), watchAddress, arm};
    if (!args) return false;
    HANDLE h = CreateThread(nullptr, 0, &helperDrThreadProc, args, 0, nullptr);
    if (!h) { delete args; return false; }
    WaitForSingleObject(h, 2000);
    CloseHandle(h);
    return true;
}

// Toolhelp32 snapshot of every thread in this process; arms any not
// already in g_armedThreads. Called once at arm time and then about every
// 2s while armed, so a thread the game starts after the first sweep still
// gets watched.
void sweepArmThreads() noexcept {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    THREADENTRY32 te{};
    te.dwSize = sizeof(te);
    const DWORD pid = GetCurrentProcessId();
    const DWORD selfTid = GetCurrentThreadId();
    if (Thread32First(snap, &te)) {
        do {
            if (te.th32OwnerProcessID != pid) continue;
            if (g_armedThreads.contains(te.th32ThreadID)) continue;
            const bool ok = (te.th32ThreadID == selfTid)
                ? setCurrentThreadDr(g_watchAddress, true)
                : setThreadDr(te.th32ThreadID, g_watchAddress, true);
            if (ok) g_armedThreads.add(te.th32ThreadID);
        } while (Thread32Next(snap, &te));
    }
    CloseHandle(snap);
}

void disarmAllThreads() noexcept {
    const DWORD selfTid = GetCurrentThreadId();
    for (uint32_t i = 0; i < g_armedThreads.count; ++i) {
        const DWORD tid = g_armedThreads.ids[i];
        if (tid == selfTid) setCurrentThreadDr(g_watchAddress, false);
        else setThreadDr(tid, g_watchAddress, false);
    }
    g_armedThreads.clear();
}

PVOID g_vehHandle = nullptr;

// --- The unique-reader table. Preallocated, guarded by a plain spin (an
// InterlockedCompareExchange lock flag, not a mutex): "the handler may not
// log, take a lock or resolve a module" is vtable_hook.cpp's writeWatch
// rule for its own VEH handler and applies here too. Read from the dump
// path (poseReaderWatchTableCount/Entry) under the same lock -- a short,
// uncontended critical section either way.
struct ReaderEntry {
    prw::ReaderKey key;
    uint32_t count = 0;
    uint32_t firstFrame = 0;
    uint32_t lastFrame = 0;
    uint32_t lastThreadId = 0;
    uint32_t unwindRvas[prw::kMaxUnwindFrames] = {};
    uint32_t unwindCount = 0;
};
ReaderEntry g_readerTable[prw::kMaxReaders];
volatile LONG g_readerTableUsed = 0;
volatile LONG g_readerTableLock = 0;   // 0 free, 1 held
std::atomic<uint32_t> g_frameReaderMask{0};
std::atomic<uint64_t> g_gameHitsTotal{0};
std::atomic<uint64_t> g_edvrHitsIgnored{0};

// RtlLookupFunctionEntry + RtlVirtualUnwind from a COPY of the faulting
// context, exactly crash_context.h's emitUnwind shape (leaf-frame fallback
// via a raw return-address read included): SEH-guarded, POD locals only,
// safe to call from a VEH handler. Walks up to kMaxUnwindFrames total
// steps, keeping only the RVAs that land inside the game module.
__declspec(noinline) uint32_t unwindGameFrames(const CONTEXT& startCtx, uint64_t gameBase, uint64_t gameSize,
                                               uint32_t* outRvas, uint32_t cap) noexcept {
    CONTEXT c = startCtx;
    uint32_t found = 0;
    for (uint32_t step = 0; step < prw::kMaxUnwindFrames && found < cap; ++step) {
        if (!c.Rip) break;
        DWORD64 imageBase = 0;
        PRUNTIME_FUNCTION function = nullptr;
        __try {
            function = RtlLookupFunctionEntry(c.Rip, &imageBase, nullptr);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            break;
        }
        const DWORD64 oldRip = c.Rip, oldRsp = c.Rsp;
        if (function) {
            DWORD64 establisher = 0;
            PVOID handlerData = nullptr;
            __try {
                RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, c.Rip, function, &c, &handlerData,
                                 &establisher, nullptr);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                break;
            }
        } else {
            // Leaf functions have no unwind entry; the return PC is at RSP.
            uintptr_t next = 0;
            bool readOk = false;
            __try {
                if (c.Rsp <= UINTPTR_MAX - sizeof(uintptr_t)) {
                    next = *reinterpret_cast<const uintptr_t*>(c.Rsp);
                    readOk = true;
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                readOk = false;
            }
            if (!readOk) break;
            c.Rip = next;
            c.Rsp += sizeof(uintptr_t);
        }
        if (c.Rsp <= oldRsp || c.Rip == oldRip) break;
        if (c.Rip >= gameBase && c.Rip < gameBase + gameSize) {
            outRvas[found++] = static_cast<uint32_t>(c.Rip - gameBase);
        }
    }
    return found;
}

void recordGameHit(const CONTEXT& originalCtx, uint64_t hitAddress) noexcept {
    const uint64_t base = g_gameBase.load(std::memory_order_relaxed);
    const uint64_t size = g_gameSize.load(std::memory_order_relaxed);
    const uint32_t frameNo = g_frame.load(std::memory_order_relaxed);
    const uint32_t tid = GetCurrentThreadId();
    g_gameHitsTotal.fetch_add(1, std::memory_order_relaxed);

    prw::ReaderKey key{};
    key.rip = hitAddress - base;
    uint32_t rvas[prw::kMaxUnwindFrames] = {};
    const uint32_t n = unwindGameFrames(originalCtx, base, size, rvas, prw::kMaxUnwindFrames);
    key.caller1 = n > 0 ? rvas[0] : 0;
    key.caller2 = n > 1 ? rvas[1] : 0;

    while (InterlockedCompareExchange(&g_readerTableLock, 1, 0) != 0) { /* short spin, no OS call */ }
    const uint32_t used = static_cast<uint32_t>(g_readerTableUsed);
    prw::ReaderKey keys[prw::kMaxReaders];
    for (uint32_t i = 0; i < used; ++i) keys[i] = g_readerTable[i].key;
    uint32_t idx = prw::findReaderSlot(keys, used, key);
    if (idx == prw::kMaxReaders && !prw::readerTableFull(used)) {
        idx = used;
        ReaderEntry& e = g_readerTable[idx];
        e.key = key;
        e.count = 0;
        e.firstFrame = frameNo;
        e.lastFrame = frameNo;
        e.unwindCount = n;
        for (uint32_t i = 0; i < n; ++i) e.unwindRvas[i] = rvas[i];
        g_readerTableUsed = static_cast<LONG>(used + 1);
    }
    if (idx < prw::kMaxReaders) {
        ReaderEntry& e = g_readerTable[idx];
        ++e.count;
        e.lastFrame = frameNo;
        e.lastThreadId = tid;
        g_frameReaderMask.fetch_or(1u << idx, std::memory_order_relaxed);
    }
    InterlockedExchange(&g_readerTableLock, 0);
}

// Not noexcept: AddVectoredExceptionHandler's PVECTORED_EXCEPTION_HANDLER
// has no exception specification, and vtable_hook.cpp's own handler
// (writeWatchHandler) matches its target type the same way.
LONG CALLBACK poseReaderVeh(EXCEPTION_POINTERS* ep) {
    if (!ep || !ep->ExceptionRecord || !ep->ContextRecord) return EXCEPTION_CONTINUE_SEARCH;
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP) return EXCEPTION_CONTINUE_SEARCH;
    const uint32_t dr6 = static_cast<uint32_t>(ep->ContextRecord->Dr6 & 0xFFFFFFFFull);
    if (!prw::dr6HasSlot0Hit(dr6)) return EXCEPTION_CONTINUE_SEARCH;
    // Ours: clear before anything else -- Dr6 is sticky, and a handler that
    // resumes without clearing it re-traps immediately. Every other
    // single-step user (src\common\vtable_hook.cpp's trap-flag diagnostic)
    // never reaches this point, because it never sets Dr6 bit 0.
    ep->ContextRecord->Dr6 = 0;
    const uint64_t base = g_gameBase.load(std::memory_order_relaxed);
    const uint64_t size = g_gameSize.load(std::memory_order_relaxed);
    const auto addr = reinterpret_cast<uint64_t>(ep->ExceptionRecord->ExceptionAddress);
    if (base && addr >= base && addr < base + size) {
        recordGameHit(*ep->ContextRecord, addr);
    } else {
        // EDVR's own copyPose (openvr_compositor.cpp) writes this same
        // address and traps too; expected, and not the reader being hunted.
        g_edvrHitsIgnored.fetch_add(1, std::memory_order_relaxed);
    }
    return EXCEPTION_CONTINUE_EXECUTION;
}

// --- Stability gate and the arm/disarm lifecycle --------------------------
constexpr uint64_t kBoundArmedMs = 30000;      // 30s
constexpr uint64_t kBoundGameHits = 5000;
constexpr uint64_t kRearmSweepMs = 2000;       // ~2s
constexpr uint64_t kNeverStabilizedMs = 30000; // 30s of never reaching 60 consecutive frames

prw::StabilityState g_stability;
bool g_declinedOnStackLogged = false;
bool g_neverStabilizedLogged = false;
uint64_t g_onSinceMs = 0;
bool g_hwArmed = false;
bool g_hwDisarmedForSession = false;
uint64_t g_hwArmedAtMs = 0;
uint64_t g_lastRearmSweepMs = 0;

void installVeh() noexcept {
    if (g_vehHandle) return;
    g_vehHandle = AddVectoredExceptionHandler(1, &poseReaderVeh);
}
void removeVeh() noexcept {
    if (!g_vehHandle) return;
    RemoveVectoredExceptionHandler(g_vehHandle);
    g_vehHandle = nullptr;
}

void armHardwareBreakpoint(uint64_t address) noexcept {
    g_watchAddress = address;
    if (!resolveGameModule()) {
        Log::get().note("pose reader watch: not arming -- could not resolve the game module's base/size.");
        g_hwDisarmedForSession = true;
        return;
    }
    // Handler first, then the threads: a thread armed before the handler
    // exists would hand its first single-step to the OS's default
    // handling, which is an unhandled exception.
    installVeh();
    sweepArmThreads();
    g_hwArmed = true;
    g_hwArmedAtMs = GetTickCount64();
    g_lastRearmSweepMs = g_hwArmedAtMs;
    Log::get().note(
        "pose reader watch: hardware breakpoint armed at 0x%llX (the render-pose buffer's "
        "m[0][3], after %u consecutive stable frames), %u thread(s).",
        static_cast<unsigned long long>(address), prw::kStableFrames, g_armedThreads.count);
}

void disarmHardwareBreakpoint(const char* reason) noexcept {
    // Threads first, then the handler: removing the handler while a thread
    // is still armed risks an unhandled single-step landing with nothing
    // to catch it.
    disarmAllThreads();
    removeVeh();
    g_hwArmed = false;
    g_hwDisarmedForSession = true;
    Log::get().note("pose reader watch: hardware breakpoint disarmed -- %s.", reason);
}

void installPositionerHooks() noexcept {
    const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    const char* why = nullptr;
    if (!base || !checkIdentity(base, &why)) {
        Log::get().note(
            "pose reader watch: armed (advanced.eye_origin_readers = on), but the positioner "
            "hooks (part B) are standing down -- %s. The hardware-breakpoint reader hunt "
            "(part A) still runs; only the positioner swap evidence is unavailable.",
            why ? why : "module base unavailable");
        return;
    }
    g_relayGate.store(1, std::memory_order_relaxed);
    const bool tickOk = installOne(g_tickEntry, base, kTickBytes);
    const bool swapSyncOk = installOne(g_swapSyncEntry, base, kSwapSyncBytes);
    Log::get().note(
        "pose reader watch: armed (advanced.eye_origin_readers = on). identity: build match "
        "(timestamp %u, image %u bytes) -- OK. positioner tick 0x107C760: %s. positioner "
        "swap-sync 0x1090420: %s. Asked the runtime to trace WaitGetPoses/GetLastPoses "
        "callers; a hardware breakpoint arms on the render-pose buffer once its address "
        "holds steady for %u consecutive frames off the calling thread's own stack.",
        kExpectedTimestamp, kExpectedImageSize,
        tickOk ? "installed" : g_tickEntry.failReason,
        swapSyncOk ? "installed" : g_swapSyncEntry.failReason,
        prw::kStableFrames);
}

// --- Periodic (~20s) reporting, only when something moved -----------------
void reportLine(const char* prefix) noexcept {
    Log::get().note(
        "%s readers=%u game_hits=%llu edvr_hits_ignored=%llu threads_armed=%u "
        "positioner_calls=%llu swaps=%llu",
        prefix, poseReaderWatchTableCount(),
        static_cast<unsigned long long>(g_gameHitsTotal.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(g_edvrHitsIgnored.load(std::memory_order_relaxed)),
        g_armedThreads.count,
        static_cast<unsigned long long>(g_positionerCallsTotal.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(g_swapsSeenTotal.load(std::memory_order_relaxed)));
}

uint64_t g_lastReportTickMs = 0;
uint64_t g_lastReportBits[5] = {};

uint64_t reportBit(uint32_t i) noexcept {
    switch (i) {
    case 0: return poseReaderWatchTableCount();
    case 1: return g_gameHitsTotal.load(std::memory_order_relaxed);
    case 2: return g_edvrHitsIgnored.load(std::memory_order_relaxed);
    case 3: return g_positionerCallsTotal.load(std::memory_order_relaxed);
    default: return g_swapsSeenTotal.load(std::memory_order_relaxed);
    }
}

void maybeReportPeriodic() noexcept {
    const uint64_t now = GetTickCount64();
    if (now - g_lastReportTickMs < 20000) return;
    g_lastReportTickMs = now;
    bool moved = false;
    for (uint32_t i = 0; i < 5; ++i) {
        const uint64_t v = reportBit(i);
        if (v != g_lastReportBits[i]) { moved = true; g_lastReportBits[i] = v; }
    }
    if (!moved) return;
    reportLine("pose reader watch:");
}

}  // namespace

// --- Public API ------------------------------------------------------------

void poseReaderWatchConfigure(Config& cfg) {
    const bool on = cfg.getBool("advanced.eye_origin_readers", false);
    g_on.store(on, std::memory_order_relaxed);
    if (!on) {
        if (!g_offLogged.exchange(true, std::memory_order_relaxed)) {
            Log::get().note("pose reader watch: off (advanced.eye_origin_readers = off).");
        }
        requestPoseReaderTrace(false);
        return;
    }
    requestPoseReaderTrace(true);
    if (g_onSinceMs == 0) g_onSinceMs = GetTickCount64();
    if (!g_hooksInstalled.exchange(true, std::memory_order_relaxed)) {
        installPositionerHooks();
    }
}

void poseReaderWatchFrameBoundary(uint32_t frameNo) {
    g_frame.store(frameNo, std::memory_order_relaxed);

    if (!g_on.load(std::memory_order_relaxed)) {
        if (g_hwArmed) disarmHardwareBreakpoint("advanced.eye_origin_readers turned off");
        return;
    }
    if (g_hwDisarmedForSession) { maybeReportPeriodic(); return; }

    const uint64_t now = GetTickCount64();

    if (g_hwArmed) {
        const uint64_t hits = g_gameHitsTotal.load(std::memory_order_relaxed);
        char reason[96];
        if (now - g_hwArmedAtMs >= kBoundArmedMs) {
            std::snprintf(reason, sizeof(reason), "%llus armed (the bound)",
                          static_cast<unsigned long long>((now - g_hwArmedAtMs) / 1000));
            disarmHardwareBreakpoint(reason);
        } else if (hits >= kBoundGameHits) {
            std::snprintf(reason, sizeof(reason), "%llu game hits (the bound)",
                          static_cast<unsigned long long>(hits));
            disarmHardwareBreakpoint(reason);
        } else if (now - g_lastRearmSweepMs >= kRearmSweepMs) {
            g_lastRearmSweepMs = now;
            sweepArmThreads();
        }
        maybeReportPeriodic();
        return;
    }

    // Not yet armed: the 60-frame stability gate against the runtime's
    // published render-pose pointer (frame_flag.h).
    const PoseReaderCall call = poseReaderCall();
    uint64_t address = 0;
    bool onStack = false;
    if (call.seq != 0 && call.renderPtr != 0) {
        address = call.renderPtr + 12;  // render[0].mDeviceToAbsoluteTracking.m[0][3]
        onStack = call.renderOnStack;
    }
    g_stability = prw::observeAddress(g_stability, address);
    if (prw::isStable(g_stability)) {
        if (onStack) {
            if (!g_declinedOnStackLogged) {
                g_declinedOnStackLogged = true;
                Log::get().note(
                    "pose reader watch: not arming -- the stable render-pose address "
                    "(0x%llX) is on the calling thread's own stack, so it cannot be watched "
                    "past this frame. The runtime's own WaitGetPoses call-stack log is the "
                    "anchor instead.",
                    static_cast<unsigned long long>(address));
            }
        } else {
            armHardwareBreakpoint(address);
        }
    } else if (!g_neverStabilizedLogged && now - g_onSinceMs >= kNeverStabilizedMs) {
        g_neverStabilizedLogged = true;
        Log::get().note(
            "pose reader watch: not arming -- no render-pose address has held steady for %u "
            "consecutive frames in the last %llus; it keeps changing. The runtime's own "
            "WaitGetPoses call-stack log is the anchor instead.",
            prw::kStableFrames, static_cast<unsigned long long>(kNeverStabilizedMs / 1000));
    }
    maybeReportPeriodic();
}

bool poseReaderWatchOn() { return g_on.load(std::memory_order_relaxed); }

bool poseReaderTakeSwapTrigger(uint32_t* frameOut) {
    if (!g_swapTriggerPending.exchange(false, std::memory_order_acquire)) return false;
    if (frameOut) *frameOut = g_swapTriggerFrame.load(std::memory_order_relaxed);
    return true;
}

// Read-and-reset, not a peek: called from exactly one of glitch_frame.cpp's
// three (mutually exclusive -- fix-off, disabled-for-session, normal)
// ring-write sites per real frame, ahead of poseReaderWatchFrameBoundary in
// vscreen.cpp's own call order, so this -- not that function -- is where
// "the frame that just ended" gets finalised and the accumulators cleared
// for the next one. transition_flash_prevent.cpp's H3Accum is the same
// finalise-then-reset shape, just triggered from its OWN frame boundary
// instead of a sibling module's, because that one runs first here.
PoseReaderFrameSnapshot poseReaderWatchFrameSnapshot() {
    PoseReaderFrameSnapshot snap;
    snap.readerMask = g_frameReaderMask.exchange(0, std::memory_order_relaxed);
    snap.tickCalls = g_tickCallsThisFrame.exchange(0, std::memory_order_relaxed);
    snap.swapSyncCalls = g_swapSyncCallsThisFrame.exchange(0, std::memory_order_relaxed);
    snap.swapDetected = g_swapDetectedThisFrame.exchange(false, std::memory_order_relaxed);
    return snap;
}

uint32_t poseReaderWatchTableCount() {
    while (InterlockedCompareExchange(&g_readerTableLock, 1, 0) != 0) { /* short spin */ }
    const uint32_t used = static_cast<uint32_t>(g_readerTableUsed);
    InterlockedExchange(&g_readerTableLock, 0);
    return used;
}

PoseReaderTableEntry poseReaderWatchTableEntry(uint32_t index) {
    PoseReaderTableEntry out{};
    while (InterlockedCompareExchange(&g_readerTableLock, 1, 0) != 0) { /* short spin */ }
    const uint32_t used = static_cast<uint32_t>(g_readerTableUsed);
    if (index < used) {
        const ReaderEntry& e = g_readerTable[index];
        out.rip = e.key.rip;
        out.count = e.count;
        out.firstFrame = e.firstFrame;
        out.lastFrame = e.lastFrame;
        out.lastThreadId = e.lastThreadId;
        out.unwindCount = e.unwindCount;
        for (uint32_t i = 0; i < e.unwindCount && i < prw::kMaxUnwindFrames; ++i) out.unwindRvas[i] = e.unwindRvas[i];
    }
    InterlockedExchange(&g_readerTableLock, 0);
    return out;
}

void poseReaderWatchShutdown() {
    if (!g_hooksInstalled.load(std::memory_order_relaxed)) return;
    reportLine("--- pose reader watch, session total:");
}

}  // namespace edvr
