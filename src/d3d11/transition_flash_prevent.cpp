#include "transition_flash_prevent.h"
#include "transition_flash_prevent_core.h"
#include "../common/code_hook.h"
#include "../common/config.h"
#include "../common/log.h"
#include "../common/game_call_probe.h"

#include <windows.h>
#include <intrin.h>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>
#include <string>

namespace edvr {
namespace {

// --- Identity: build 332841, the same pair object_record_writer_probe.h
// checks. Kept as this file's own copy rather than a shared constant --
// the "copy-culture is deliberate" rule kinematic_eval_hook.cpp states for
// its own identity check, so that file is never touched by a change here.
constexpr uint32_t kExpectedTimestamp = 1788384820u;
constexpr uint32_t kExpectedImageSize = 104894464u;

// --- The five functions (design doc's engine chain table). Compose, decode,
// push and latch are hooked; recompute is verified and CALLED, never
// patched; the twin compose's extent is used only to classify the decode
// hook's caller.
constexpr uintptr_t kComposeRva = 0x23BC8A0u, kComposeEnd = 0x23BC9B1u;
constexpr uintptr_t kTwinComposeRva = 0x23859A0u, kTwinComposeEnd = 0x23860EFu;
constexpr uintptr_t kDecodeRva = 0x3CEE4C0u;
constexpr uintptr_t kPushRva = 0x3D0CF10u;
constexpr uintptr_t kLatchRva = 0x23AE880u;
constexpr uintptr_t kRecomputeRva = 0x3CEE650u;

// First 16 bytes of each, read from analysis\EliteDangerous64.exe (build
// 332841) via its PE section table -- not the loaded, possibly-patched
// process image -- and cross-checked against
// analysis\decomp\flash\r2\prologue_bytes.txt for compose/decode/push.
constexpr uint8_t kComposeBytes[16] = {
    0x48,0x89,0x5C,0x24,0x08,0x48,0x89,0x7C,0x24,0x10,0x55,0x48,0x8D,0x6C,0x24,0xD0};
constexpr uint8_t kDecodeBytes[16] = {
    0x8B,0x81,0x84,0x03,0x00,0x00,0x4C,0x8D,0x04,0x40,0x48,0x8B,0x81,0x78,0x03,0x00};
constexpr uint8_t kPushBytes[16] = {
    0x48,0x8B,0xC4,0x48,0x89,0x58,0x10,0x48,0x89,0x70,0x18,0x57,0x48,0x81,0xEC,0x90};
constexpr uint8_t kLatchBytes[16] = {
    0x48,0x89,0x5C,0x24,0x08,0x48,0x89,0x74,0x24,0x18,0x57,0x48,0x83,0xEC,0x20,0x48};
constexpr uint8_t kRecomputeBytes[16] = {
    0x48,0x89,0x5C,0x24,0x08,0x48,0x89,0x74,0x24,0x10,0x57,0x48,0x81,0xEC,0xD0,0x00};

constexpr uint32_t kRecomputeBudgetPerFrame = 512;
constexpr uint32_t kSessionActCap = 200;
constexpr uint32_t kOwnEventDumpDeferFrames = 12;  // so N+1.. are in, per the design
constexpr uint32_t kRingCapacity = 8192;
constexpr uint32_t kMaxDumpsPerSession = 16;

// --- Game-side signatures (signed off against the decompiles in
// analysis\decomp\flash\, \r2\ and \r3\). Every argument and the return
// value pass through unchanged.
using ComposeFn   = void    (__fastcall*)(uintptr_t, uintptr_t, uintptr_t, uintptr_t, uint32_t*);
using DecodeFn    = double* (__fastcall*)(uintptr_t, double*);
using PushFn      = void    (__fastcall*)(uintptr_t, double*);
using LatchFn     = uintptr_t (__fastcall*)(uintptr_t, uintptr_t);
using RecomputeFn = double* (__fastcall*)(uintptr_t, double*);

__declspec(noinline) void    __fastcall composeObserved(uintptr_t,uintptr_t,uintptr_t,uintptr_t,uint32_t*) noexcept;
__declspec(noinline) double* __fastcall decodeObserved(uintptr_t,double*) noexcept;
__declspec(noinline) void    __fastcall pushObserved(uintptr_t,double*) noexcept;
__declspec(noinline) uintptr_t __fastcall latchObserved(uintptr_t,uintptr_t) noexcept;

// Defined with the other SEH-guarded readers, below; forward-declared so
// installOne can check a target's prologue before ever patching it.
bool sehCheckBytes(uintptr_t address, const uint8_t* expected, size_t n) noexcept;

// --- Relay machinery, mirrored from object_record_writer_hook.cpp /
// kinematic_eval_hook.cpp. Kept as a copy rather than a shared unit so
// neither flight-proven file is touched; if one changes, change all three.
// Needed because the target is in the GAME's module and this DLL loads more
// than two gigabytes away -- a plain five-byte E9 patch cannot reach a
// replacement here directly (kinematic_eval_hook.cpp's 2026-09-19 flight).
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
    // original: jmp [trampoline]. RAX/flags are volatile and none of the
    // four hooked targets consume RAX on entry.
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

// Held open unconditionally once installation starts: the off/watch/on/
// alternate distinction is made inside the observed callbacks themselves
// (one atomic load), not by gating the relay -- there is exactly one
// consumer of these four hooks, unlike the multi-consumer kinematic set.
std::atomic<uintptr_t> g_relayGate{0};

bool installOne(HookEntry& entry, uintptr_t base, const uint8_t* expectedBytes) noexcept {
    // The prologue check comes first and separately from CodeHook's own
    // decode: CodeHook only asks whether the bytes there are SOME shape it
    // knows how to relocate, and would patch a same-shaped-but-different
    // function without a word of complaint. This is the build-332841-
    // specific signature: a mismatch here means the function moved or
    // changed, and stands the hook down before any write happens.
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
    // Process-lifetime storage, same rule as every CodeHook user here: do not
    // free a relay or trampoline an in-flight call may already be executing.
}

HookEntry g_composeEntry{"transition-flash-compose", kComposeRva, reinterpret_cast<void*>(&composeObserved)};
HookEntry g_decodeEntry {"transition-flash-decode",  kDecodeRva,  reinterpret_cast<void*>(&decodeObserved)};
HookEntry g_pushEntry   {"transition-flash-push",    kPushRva,    reinterpret_cast<void*>(&pushObserved)};
HookEntry g_latchEntry  {"transition-flash-latch",   kLatchRva,   reinterpret_cast<void*>(&latchObserved)};

// --- Module state ------------------------------------------------------
std::atomic<uint8_t> g_mode{uint8_t(tfp::Mode::Off)};
std::atomic<bool> g_armed{false};     // install has been attempted (once)
std::atomic<bool> g_offLogged{false};
std::atomic<uintptr_t> g_base{0};
std::atomic<uint32_t> g_frame{0};

std::atomic<uintptr_t> g_recomputeAddr{0};
std::atomic<bool> g_recomputeUsable{false};

// Validation / per-parent / event state share one mutex: every decode-from-
// compose call touches at least the first two together.
std::mutex g_guardMutex;
tfp::ValidationCounts g_validation;
tfp::ParentGuardTable g_parentTable;
tfp::EventTracker g_eventTracker;

// The push hook's last recorded pose, for H3 and for the call-stack capture
// on event frames.
struct LastPushed {
    bool valid = false;
    uint32_t frame = 0;
    uintptr_t target = 0;
    double t[3] = {0, 0, 0};
    bool hasStack = false;
    GameCallStack stack{};
};
std::mutex g_pushMutex;
LastPushed g_lastPushed;

// So the push hook can tell "is this frame part of an active event" without
// taking g_guardMutex on every call: set by the decode hook on every
// disagreement, read by the push hook.
std::atomic<uint32_t> g_lastEventFrame{0};
std::atomic<bool> g_hasEvent{false};

// Session counters. Monotonic; the periodic line and the shutdown summary
// both read a snapshot rather than resetting anything.
std::atomic<uint64_t> g_composeCalls{0};
std::atomic<uint64_t> g_decodeAgree{0}, g_decodeNear{0}, g_decodeDisagree{0};
std::atomic<uint64_t> g_recomputeFaults{0}, g_budgetSkips{0};
std::atomic<uint64_t> g_eventsWatched{0}, g_eventsActed{0}, g_eventsNotValidated{0};
std::atomic<uint64_t> g_h3Match{0}, g_h3Mismatch{0};
std::atomic<uint64_t> g_pushes{0}, g_latchCalls{0}, g_actedFrames{0};

// The recompute's own per-frame call budget: 512, then skips are counted
// instead of spending more CPU walking the ancestor chain. Touched only from
// decode-from-compose calls, which are rare enough (a handful a frame) that
// a mutex is simplest and costs nothing measurable.
std::mutex g_budgetMutex;
uint32_t g_budgetFrame = 0, g_budgetUsed = 0;

bool takeRecomputeBudget(uint32_t frame) noexcept {
    std::lock_guard<std::mutex> lock(g_budgetMutex);
    if (g_budgetFrame != frame) { g_budgetFrame = frame; g_budgetUsed = 0; }
    if (g_budgetUsed >= kRecomputeBudgetPerFrame) return false;
    ++g_budgetUsed;
    return true;
}

// --- The ring: fixed capacity, one atomic index, four kinds of entry. A
// concurrent dump can read a slot mid-write (this file's hot-path writers
// are not serialised against the dump), which shows as one stale-looking
// line in an at-most-16-a-session diagnostic dump -- accepted the same way
// glitch_frame.cpp's own ring accepts it, for the same reason: a lock here
// would put file-adjacent latency on the game's own camera compose.
enum class RingKind : uint8_t { Decode = 0, Push = 1, Latch = 2, H3 = 3 };

struct RingEntry {
    uint32_t frame = 0;
    uint32_t threadId = 0;
    RingKind kind = RingKind::Decode;
    uint8_t site = 0;       // decode: 0 primary compose, 1 twin compose
    uint8_t cls = 0;        // decode: tfp::PoseClass
    uint8_t treatment = 0;  // decode: 1 if this call acted
    uint64_t retAddr = 0;   // decode/push: _ReturnAddress()
    uint64_t parent = 0;    // decode: parent. push: target. latch: item.
    uint32_t idx = 0;       // decode: *(parent+0x384)
    uint32_t eventNumber = 0;
    uint64_t count = 0;     // decode: *(container+0x380). latch: mgr (this)
    uint64_t chain = 0;     // decode: *(parent+0x350). latch: *(item+0x18)
    double a[3] = {0, 0, 0};  // decode: cached t. push: pushed t. H3: cb1 t
    double b[3] = {0, 0, 0};  // decode: recompute t. H3: last-pushed t
    double dt = 0, dr = 0;
};

RingEntry g_ring[kRingCapacity];
std::atomic<uint64_t> g_ringHead{0};

void pushRing(const RingEntry& e) noexcept {
    const uint64_t slot = g_ringHead.fetch_add(1, std::memory_order_relaxed);
    g_ring[slot % kRingCapacity] = e;
}

// --- Dumps: one pending slot, serviced from the frame boundary (never from
// inside a game hook -- a dump can be dozens of Log::note calls, and the
// hooks can run on a scheduler job thread where that latency is the game's).
struct PendingDump {
    bool active = false;
    uint32_t triggerFrame = 0;
    uint32_t dueFrame = 0;
    const char* reason = "";
};
std::mutex g_dumpMutex;
PendingDump g_pendingDump;
std::atomic<uint32_t> g_dumpsThisSession{0};

void requestDump(const char* reason, uint32_t triggerFrame, uint32_t deferFrames) noexcept {
    if (g_dumpsThisSession.load(std::memory_order_relaxed) >= kMaxDumpsPerSession) return;
    std::lock_guard<std::mutex> lock(g_dumpMutex);
    if (g_pendingDump.active) return;  // one at a time; the next trigger tries again later
    g_pendingDump = PendingDump{true, triggerFrame, triggerFrame + deferFrames, reason};
}

const char* classText(uint8_t cls) noexcept {
    return cls == uint8_t(tfp::PoseClass::Agree) ? "agree"
         : cls == uint8_t(tfp::PoseClass::Near)  ? "near" : "disagree";
}

void performDump(const char* reason, uint32_t triggerFrame) noexcept {
    g_dumpsThisSession.fetch_add(1, std::memory_order_relaxed);
    const uint64_t head = g_ringHead.load(std::memory_order_relaxed);
    const uint64_t have = head < kRingCapacity ? head : kRingCapacity;
    const uint64_t first = head - have;
    const uint32_t lo = triggerFrame > 30 ? triggerFrame - 30 : 0;
    Log::get().note("--- transition flash prevent: ring dump, trigger=\"%s\" frame=%u, window "
                    "[%u,%u] ---", reason, triggerFrame, lo, triggerFrame + 30);
    uint32_t printed = 0;
    for (uint64_t i = first; i < head; ++i) {
        const RingEntry e = g_ring[i % kRingCapacity];
        if (!tfp::ringFrameInWindow(e.frame, triggerFrame)) continue;
        ++printed;
        switch (e.kind) {
        case RingKind::Decode:
            Log::get().note(
                "  f%-7u t%-6u DECODE site=%u parent=0x%llX idx=%u/%llu chain=0x%llX class=%s "
                "cached=(%+.3f %+.3f %+.3f) recompute=(%+.3f %+.3f %+.3f) dt=%.4f dr=%.6f "
                "event=%u treat=%s",
                e.frame, e.threadId, e.site, (unsigned long long)e.parent, e.idx,
                (unsigned long long)e.count, (unsigned long long)e.chain, classText(e.cls),
                e.a[0], e.a[1], e.a[2], e.b[0], e.b[1], e.b[2], e.dt, e.dr,
                e.eventNumber, e.treatment ? "ACT" : "watch");
            break;
        case RingKind::Push:
            Log::get().note("  f%-7u t%-6u PUSH target=0x%llX t=(%+.3f %+.3f %+.3f) ret=0x%llX",
                            e.frame, e.threadId, (unsigned long long)e.parent,
                            e.a[0], e.a[1], e.a[2], (unsigned long long)e.retAddr);
            break;
        case RingKind::Latch:
            Log::get().note("  f%-7u t%-6u LATCH mgr=0x%llX item=0x%llX key=0x%llX",
                            e.frame, e.threadId, (unsigned long long)e.count,
                            (unsigned long long)e.parent, (unsigned long long)e.chain);
            break;
        case RingKind::H3:
            Log::get().note(
                "  f%-7u t%-6u H3 cb1=(%+.3f %+.3f %+.3f) pushed=(%+.3f %+.3f %+.3f) dt=%.4f %s",
                e.frame, e.threadId, e.a[0], e.a[1], e.a[2], e.b[0], e.b[1], e.b[2], e.dt,
                e.dt < 0.01 ? "match" : "mismatch");
            break;
        }
    }
    Log::get().note("--- transition flash prevent: ring dump done, %u entries in the window ---",
                    printed);
}

void serviceDump(uint32_t nowFrame) noexcept {
    PendingDump due;
    {
        std::lock_guard<std::mutex> lock(g_dumpMutex);
        if (!g_pendingDump.active || nowFrame < g_pendingDump.dueFrame) return;
        due = g_pendingDump;
        g_pendingDump = PendingDump{};
    }
    performDump(due.reason, due.triggerFrame);
}

// --- SEH-guarded reads. Each is its own small function: mixing __try with
// this file's other locals (mutexes, std::string) in one function is what
// CodeHook's own doc calls out as refused by the compiler, and object_
// record_writer_probe.cpp's guardedRead is the precedent for keeping the
// protected region to a single dereference.
__declspec(noinline) bool sehReadU64(uintptr_t address, uint64_t& out) noexcept {
    __try { out = *reinterpret_cast<const uint64_t*>(address); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
__declspec(noinline) bool sehReadU32(uintptr_t address, uint32_t& out) noexcept {
    __try { out = *reinterpret_cast<const uint32_t*>(address); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
// The recompute call itself: a fault here disables it for the rest of the
// session (the caller sets g_recomputeUsable=false and logs once).
__declspec(noinline) bool sehCallRecompute(RecomputeFn fn, uintptr_t node, double* out) noexcept {
    __try { fn(node, out); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
__declspec(noinline) bool sehCheckBytes(uintptr_t address, const uint8_t* expected, size_t n) noexcept {
    __try { return std::memcmp(reinterpret_cast<const void*>(address), expected, n) == 0; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// PE TimeDateStamp + SizeOfImage, read the same way object_record_writer_
// probe.cpp / kinematic_eval_hook.cpp do (base+0x3C -> e_lfanew, +8
// TimeDateStamp, +0x50 SizeOfImage).
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

const char* modeName(tfp::Mode m) noexcept {
    switch (m) {
    case tfp::Mode::Off: return "off";
    case tfp::Mode::Watch: return "watch";
    case tfp::Mode::On: return "on";
    case tfp::Mode::Alternate: return "alternate";
    }
    return "?";
}

bool currentlyValidated() noexcept {
    std::lock_guard<std::mutex> lock(g_guardMutex);
    return tfp::isValidated(g_validation);
}

// --- Periodic (~20s) and shutdown reporting share this line, so a build
// where the hooks never ran reads differently from one where nothing
// happened: the "off" line above never gets here at all, and an armed
// build with zero calls still prints zeros here rather than silence.
void reportLine(const char* prefix) noexcept {
    const tfp::Mode mode = static_cast<tfp::Mode>(g_mode.load(std::memory_order_relaxed));
    Log::get().note(
        "%s mode=%s compose=%llu decode(agree/near/disagree)=%llu/%llu/%llu "
        "recompute(faults/skips)=%llu/%llu events(watched/acted/not-validated)=%llu/%llu/%llu "
        "validated=%s h3(match/mismatch)=%llu/%llu pushes=%llu latch=%llu acted-frames=%llu",
        prefix, modeName(mode),
        (unsigned long long)g_composeCalls.load(std::memory_order_relaxed),
        (unsigned long long)g_decodeAgree.load(std::memory_order_relaxed),
        (unsigned long long)g_decodeNear.load(std::memory_order_relaxed),
        (unsigned long long)g_decodeDisagree.load(std::memory_order_relaxed),
        (unsigned long long)g_recomputeFaults.load(std::memory_order_relaxed),
        (unsigned long long)g_budgetSkips.load(std::memory_order_relaxed),
        (unsigned long long)g_eventsWatched.load(std::memory_order_relaxed),
        (unsigned long long)g_eventsActed.load(std::memory_order_relaxed),
        (unsigned long long)g_eventsNotValidated.load(std::memory_order_relaxed),
        currentlyValidated() ? "yes" : "no",
        (unsigned long long)g_h3Match.load(std::memory_order_relaxed),
        (unsigned long long)g_h3Mismatch.load(std::memory_order_relaxed),
        (unsigned long long)g_pushes.load(std::memory_order_relaxed),
        (unsigned long long)g_latchCalls.load(std::memory_order_relaxed),
        (unsigned long long)g_actedFrames.load(std::memory_order_relaxed));
}

// Touched only from the frame boundary (vscreen.cpp, one thread), so a plain
// snapshot-and-compare needs no lock of its own.
uint64_t g_lastReportTickMs = 0;
uint64_t g_lastReportBits[14] = {};

uint64_t reportBit(uint32_t i) noexcept {
    switch (i) {
    case 0: return g_composeCalls.load(std::memory_order_relaxed);
    case 1: return g_decodeAgree.load(std::memory_order_relaxed);
    case 2: return g_decodeNear.load(std::memory_order_relaxed);
    case 3: return g_decodeDisagree.load(std::memory_order_relaxed);
    case 4: return g_recomputeFaults.load(std::memory_order_relaxed);
    case 5: return g_budgetSkips.load(std::memory_order_relaxed);
    case 6: return g_eventsWatched.load(std::memory_order_relaxed);
    case 7: return g_eventsActed.load(std::memory_order_relaxed);
    case 8: return g_eventsNotValidated.load(std::memory_order_relaxed);
    case 9: return g_h3Match.load(std::memory_order_relaxed);
    case 10: return g_h3Mismatch.load(std::memory_order_relaxed);
    case 11: return g_pushes.load(std::memory_order_relaxed);
    case 12: return g_latchCalls.load(std::memory_order_relaxed);
    default: return g_actedFrames.load(std::memory_order_relaxed);
    }
}

void maybeReportPeriodic() noexcept {
    const uint64_t now = GetTickCount64();
    if (now - g_lastReportTickMs < 20000) return;
    g_lastReportTickMs = now;
    bool moved = false;
    for (uint32_t i = 0; i < 14; ++i) {
        const uint64_t v = reportBit(i);
        if (v != g_lastReportBits[i]) { moved = true; g_lastReportBits[i] = v; }
    }
    if (!moved) return;
    reportLine("transition flash prevent:");
}

// --- The four observed callbacks --------------------------------------

// Compose: wraps the original call in a thread_local context (mgr, key,
// target, site=primary) and counts calls per frame. The context is not
// consulted by the other three hooks in this build -- decode and push key
// off _ReturnAddress()/their own arguments instead, which is robust to the
// scheduler-driven compose loop calling this from any thread -- but it is
// here because the design calls for it and a later widening (the decode's
// "other callers" residual risk) can read it without another flight.
struct ComposeContext { uintptr_t mgr = 0, key = 0, target = 0; bool active = false; };
thread_local ComposeContext t_composeCtx;

void __fastcall composeObserved(uintptr_t mgr, uintptr_t key, uintptr_t target, uintptr_t param4,
                                uint32_t* param5) noexcept {
    const auto forward = reinterpret_cast<ComposeFn>(g_composeEntry.forward.load(std::memory_order_acquire));
    if (!forward) return;  // stood down at install; the relay is unreachable then
    g_composeCalls.fetch_add(1, std::memory_order_relaxed);
    const ComposeContext saved = t_composeCtx;
    t_composeCtx = ComposeContext{mgr, key, target, true};
    forward(mgr, key, target, param4, param5);
    t_composeCtx = saved;
}

// Decode: calls the original first (it fills `out` with the cached 128-byte
// world block, no validity check, and returns `out`), then -- only for a
// call whose return address lands inside the primary or twin compose extent
// -- runs the anatomy instrument and, if every guard allows it, hands the
// compose the recompute instead.
double* __fastcall decodeObserved(uintptr_t parent, double* out) noexcept {
    const auto forward = reinterpret_cast<DecodeFn>(g_decodeEntry.forward.load(std::memory_order_acquire));
    if (!forward) return out;  // stood down at install; the relay is unreachable then
    double* const result = forward(parent, out);
    if (g_mode.load(std::memory_order_relaxed) == uint8_t(tfp::Mode::Off)) return result;

    const uintptr_t base = g_base.load(std::memory_order_relaxed);
    const uintptr_t ret = reinterpret_cast<uintptr_t>(_ReturnAddress());
    uint8_t site;
    if (base && ret >= base + kComposeRva && ret < base + kComposeEnd) site = 0;
    else if (base && ret >= base + kTwinComposeRva && ret < base + kTwinComposeEnd) site = 1;
    else return result;  // not a compose-site call: nothing further to do here

    const uint32_t frame = g_frame.load(std::memory_order_relaxed);

    uint64_t container = 0, chain = 0, count = 0;
    uint32_t idx = 0;
    const bool okContainer = sehReadU64(parent + 0x378, container);
    sehReadU32(parent + 0x384, idx);
    sehReadU64(parent + 0x350, chain);
    if (okContainer && container) sehReadU64(container + 0x380, count);

    if (!takeRecomputeBudget(frame)) {
        g_budgetSkips.fetch_add(1, std::memory_order_relaxed);
        return result;
    }
    if (!g_recomputeUsable.load(std::memory_order_relaxed)) return result;

    const auto recompute = reinterpret_cast<RecomputeFn>(g_recomputeAddr.load(std::memory_order_relaxed));
    double rc[16];
    if (!sehCallRecompute(recompute, parent, rc)) {
        g_recomputeUsable.store(false, std::memory_order_relaxed);
        g_recomputeFaults.fetch_add(1, std::memory_order_relaxed);
        static std::atomic<bool> faultLogged{false};
        if (!faultLogged.exchange(true, std::memory_order_relaxed)) {
            Log::get().note(
                "transition flash prevent: the from-root recompute (0x3CEE650) faulted at "
                "parent 0x%llX, frame %u -- disabled for the rest of this session. Every decode "
                "from a compose site is watch-only from here on.",
                (unsigned long long)parent, frame);
        }
        return result;
    }

    const double cachedT[3] = {out[12], out[13], out[14]};
    const double recomputeT[3] = {rc[12], rc[13], rc[14]};
    const tfp::PoseDelta delta = tfp::comparePose(out, rc);
    const tfp::PoseClass cls = tfp::classify(delta);
    if (cls == tfp::PoseClass::Agree) g_decodeAgree.fetch_add(1, std::memory_order_relaxed);
    else if (cls == tfp::PoseClass::Near) g_decodeNear.fetch_add(1, std::memory_order_relaxed);
    else g_decodeDisagree.fetch_add(1, std::memory_order_relaxed);

    bool actedThisCall = false;
    uint32_t eventNumber = 0;
    {
        std::lock_guard<std::mutex> lock(g_guardMutex);
        if (cls == tfp::PoseClass::Agree) ++g_validation.agree;
        else if (cls == tfp::PoseClass::Disagree) ++g_validation.disagree;
        const bool validatedNow = tfp::isValidated(g_validation);
        const tfp::ParentGuardTable::Verdict guardVerdict = g_parentTable.onClassified(parent, frame, cls);

        if (guardVerdict.justCrossedPersistent) {
            Log::get().note(
                "transition flash prevent: parent 0x%llX has disagreed for more than %u frames "
                "running (frame %u) -- logged once, never acted on while this streak holds.",
                (unsigned long long)parent, tfp::ParentGuardTable::kMaxStreak, frame);
        }

        if (cls == tfp::PoseClass::Disagree) {
            const tfp::EventTracker::Note note = g_eventTracker.note(frame);
            eventNumber = note.eventNumber;
            g_lastEventFrame.store(frame, std::memory_order_relaxed);
            g_hasEvent.store(true, std::memory_order_relaxed);
            if (note.isNewEvent) {
                const tfp::Mode mode = static_cast<tfp::Mode>(g_mode.load(std::memory_order_relaxed));
                const bool byMode = tfp::modeAllowsActing(mode, note.eventNumber);
                bool eligible = byMode && validatedNow && guardVerdict.eligible;
                const char* reason;
                if (eligible && tfp::sessionCapReached(g_actedFrames.load(std::memory_order_relaxed), kSessionActCap)) {
                    eligible = false;
                    reason = "session cap of 200 acted frames reached";
                    static std::atomic<bool> capLogged{false};
                    if (!capLogged.exchange(true, std::memory_order_relaxed)) {
                        Log::get().note(
                            "transition flash prevent: session cap of %u acted frames reached at "
                            "frame %u -- the act path stands down for the rest of this session. "
                            "Still watching and logging.", kSessionActCap, frame);
                    }
                } else if (!byMode) {
                    reason = mode == tfp::Mode::Alternate ? "alternate: this event's slot is watched"
                                                          : "mode is watch";
                } else if (!validatedNow) {
                    reason = "not validated yet (need 60 agreements this session, disagree under 5%)";
                } else if (!guardVerdict.eligible) {
                    reason = "this parent is not in good standing (not new, not recently agreeing, "
                             "or its disagree streak is over 3)";
                } else {
                    reason = "mode, validation and this parent's history all allow it";
                }
                g_eventTracker.latch(eligible ? tfp::Treatment::Act : tfp::Treatment::Watch);
                if (eligible) g_eventsActed.fetch_add(1, std::memory_order_relaxed);
                else g_eventsWatched.fetch_add(1, std::memory_order_relaxed);
                if (!validatedNow) g_eventsNotValidated.fetch_add(1, std::memory_order_relaxed);

                Log::get().note(
                    "transition flash prevent: event %u frame %u %s (%s) parent=0x%llX idx=%u/"
                    "%llu cached=(%+.3f %+.3f %+.3f) recompute=(%+.3f %+.3f %+.3f) dt=%.4f dr=%.6f",
                    note.eventNumber, frame, eligible ? "ACTED" : "WATCHED", reason,
                    (unsigned long long)parent, idx, (unsigned long long)count,
                    cachedT[0], cachedT[1], cachedT[2], recomputeT[0], recomputeT[1], recomputeT[2],
                    delta.dt, delta.dr);
                requestDump("our own event", frame, kOwnEventDumpDeferFrames);
            }
            actedThisCall = g_eventTracker.treatment() == tfp::Treatment::Act;
        }
    }

    if (actedThisCall) {
        std::memcpy(out, rc, 16 * sizeof(double));
        g_actedFrames.fetch_add(1, std::memory_order_relaxed);
    }

    RingEntry e{};
    e.frame = frame; e.threadId = GetCurrentThreadId(); e.kind = RingKind::Decode; e.site = site;
    e.cls = uint8_t(cls); e.treatment = actedThisCall ? 1 : 0;
    e.retAddr = ret; e.parent = parent; e.idx = idx; e.eventNumber = eventNumber;
    e.count = count; e.chain = chain;
    e.a[0] = cachedT[0]; e.a[1] = cachedT[1]; e.a[2] = cachedT[2];
    e.b[0] = recomputeT[0]; e.b[1] = recomputeT[1]; e.b[2] = recomputeT[2];
    e.dt = delta.dt; e.dr = delta.dr;
    pushRing(e);

    return result;
}

// Push: records the pose translation and the compose site that pushed it as
// the "last pushed" snapshot (for H3), and on event frames also captures a
// call stack -- read-only, the pose is passed through untouched.
void __fastcall pushObserved(uintptr_t target, double* pose) noexcept {
    const auto forward = reinterpret_cast<PushFn>(g_pushEntry.forward.load(std::memory_order_acquire));
    if (!forward) return;  // stood down at install; the relay is unreachable then
    forward(target, pose);
    if (g_mode.load(std::memory_order_relaxed) == uint8_t(tfp::Mode::Off)) return;

    g_pushes.fetch_add(1, std::memory_order_relaxed);
    const uintptr_t ret = reinterpret_cast<uintptr_t>(_ReturnAddress());
    const uint32_t frame = g_frame.load(std::memory_order_relaxed);

    // On event frames only: captureGameCallStack costs a real unwind, and is
    // only useful while a disagreement is being chased.
    const bool eventFrame = g_hasEvent.load(std::memory_order_relaxed) &&
        (frame - g_lastEventFrame.load(std::memory_order_relaxed)) <= 2u;
    const GameCallStack stack = eventFrame ? captureGameCallStack() : GameCallStack{};

    {
        std::lock_guard<std::mutex> lock(g_pushMutex);
        g_lastPushed.valid = true;
        g_lastPushed.frame = frame;
        g_lastPushed.target = target;
        g_lastPushed.t[0] = pose[12]; g_lastPushed.t[1] = pose[13]; g_lastPushed.t[2] = pose[14];
        if (eventFrame) { g_lastPushed.stack = stack; g_lastPushed.hasStack = true; }
    }
    if (eventFrame && stack.gameFrames > 0) {
        Log::get().note("transition flash prevent: push at frame %u came through %s",
                        frame, stack.rvas);
    }

    RingEntry e{};
    e.frame = frame; e.threadId = GetCurrentThreadId(); e.kind = RingKind::Push;
    e.retAddr = ret; e.parent = target;
    e.a[0] = pose[12]; e.a[1] = pose[13]; e.a[2] = pose[14];
    pushRing(e);
}

// Latch: logs (into the ring) every call to the re-target latch's only
// setter, to learn whether it is part of a transition. Pure pass-through.
uintptr_t __fastcall latchObserved(uintptr_t mgr, uintptr_t item) noexcept {
    const auto forward = reinterpret_cast<LatchFn>(g_latchEntry.forward.load(std::memory_order_acquire));
    if (!forward) return 0;  // stood down at install; the relay is unreachable then
    const uintptr_t result = forward(mgr, item);
    if (g_mode.load(std::memory_order_relaxed) == uint8_t(tfp::Mode::Off)) return result;

    g_latchCalls.fetch_add(1, std::memory_order_relaxed);
    uint64_t key = 0;
    if (item) sehReadU64(item + 0x18, key);

    RingEntry e{};
    e.frame = g_frame.load(std::memory_order_relaxed); e.threadId = GetCurrentThreadId();
    e.kind = RingKind::Latch; e.parent = item; e.count = mgr; e.chain = key;
    pushRing(e);
    return result;
}

// --- Install -------------------------------------------------------------
void doInstall(tfp::Mode mode) noexcept {
    const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    const char* why = nullptr;
    if (!base || !checkIdentity(base, &why)) {
        Log::get().note(
            "transition flash prevent: standing down entirely -- %s. Nothing installed, "
            "nothing changed.", why ? why : "module base unavailable");
        g_armed.store(true, std::memory_order_release);  // identity cannot change at runtime; stop retrying
        return;
    }
    g_base.store(base, std::memory_order_relaxed);
    g_relayGate.store(1, std::memory_order_relaxed);

    const bool composeOk = installOne(g_composeEntry, base, kComposeBytes);
    const bool decodeOk  = installOne(g_decodeEntry, base, kDecodeBytes);
    const bool pushOk    = installOne(g_pushEntry, base, kPushBytes);
    const bool latchOk   = installOne(g_latchEntry, base, kLatchBytes);

    const bool recomputeOk = sehCheckBytes(base + kRecomputeRva, kRecomputeBytes, sizeof(kRecomputeBytes));
    g_recomputeAddr.store(base + kRecomputeRva, std::memory_order_relaxed);
    g_recomputeUsable.store(recomputeOk, std::memory_order_relaxed);

    g_armed.store(true, std::memory_order_release);

    Log::get().note(
        "transition flash prevent: armed, mode=%s. identity: build match (timestamp %u, image "
        "%u bytes) -- OK. compose 0x23BC8A0: %s. decode 0x3CEE4C0: %s. push 0x3D0CF10: %s. "
        "latch 0x23AE880: %s. recompute 0x3CEE650: %s. Ring %u entries, session cap %u acted "
        "frames, %u recompute/frame budget.",
        modeName(mode), kExpectedTimestamp, kExpectedImageSize,
        composeOk ? "installed" : g_composeEntry.failReason,
        decodeOk ? "installed" : g_decodeEntry.failReason,
        pushOk ? "installed" : g_pushEntry.failReason,
        latchOk ? "installed" : g_latchEntry.failReason,
        recomputeOk ? "verified" : "prologue mismatch -- disabled for this session",
        kRingCapacity, kSessionActCap, kRecomputeBudgetPerFrame);
}

}  // namespace

// --- Public API ------------------------------------------------------------

void transitionFlashPreventConfigure(Config& cfg) {
    bool recognized = true;
    const std::string text = cfg.getString("advanced.transition_flash_prevent", "off");
    const tfp::Mode mode = tfp::parseMode(text.c_str(), &recognized);
    if (!recognized) {
        static std::atomic<bool> warned{false};
        if (!warned.exchange(true, std::memory_order_relaxed)) {
            Log::get().note(
                "transition flash prevent: advanced.transition_flash_prevent = \"%s\" is not "
                "off, watch, on or alternate -- treated as off.", text.c_str());
        }
    }

    if (mode == tfp::Mode::Off) {
        g_mode.store(uint8_t(tfp::Mode::Off), std::memory_order_relaxed);
        if (!g_offLogged.exchange(true, std::memory_order_relaxed)) {
            Log::get().note("transition flash prevent: off (advanced.transition_flash_prevent = off).");
        }
        return;
    }

    g_mode.store(uint8_t(mode), std::memory_order_relaxed);
    if (!g_armed.load(std::memory_order_acquire)) doInstall(mode);
}

void transitionFlashPreventFrameBoundary(uint32_t frameNo) {
    g_frame.store(frameNo, std::memory_order_relaxed);
    if (!g_armed.load(std::memory_order_relaxed)) return;
    serviceDump(frameNo);
    maybeReportPeriodic();
}

void transitionFlashPreventNoteH3(uint32_t frame, const float pos[3]) {
    if (!g_armed.load(std::memory_order_relaxed)) return;
    if (g_mode.load(std::memory_order_relaxed) == uint8_t(tfp::Mode::Off)) return;
    LastPushed snap;
    {
        std::lock_guard<std::mutex> lock(g_pushMutex);
        snap = g_lastPushed;
    }
    if (!snap.valid) return;
    const double dx = double(pos[0]) - snap.t[0];
    const double dy = double(pos[1]) - snap.t[1];
    const double dz = double(pos[2]) - snap.t[2];
    const double dist = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (dist < 0.01) g_h3Match.fetch_add(1, std::memory_order_relaxed);
    else g_h3Mismatch.fetch_add(1, std::memory_order_relaxed);

    RingEntry e{};
    e.frame = frame; e.threadId = GetCurrentThreadId(); e.kind = RingKind::H3;
    e.a[0] = pos[0]; e.a[1] = pos[1]; e.a[2] = pos[2];
    e.b[0] = snap.t[0]; e.b[1] = snap.t[1]; e.b[2] = snap.t[2];
    e.dt = dist;
    pushRing(e);
}

void transitionFlashPreventNoteDetectorVerdict(uint32_t frame, uint8_t verdict, bool withheldClass) {
    (void)verdict;
    if (!g_armed.load(std::memory_order_relaxed)) return;
    if (!withheldClass) return;
    requestDump("the transition-flash detector's verdict", frame, 0);
}

void transitionFlashPreventDumpRing(const char* trigger) {
    if (!g_armed.load(std::memory_order_relaxed)) {
        Log::get().note(
            "transition flash prevent: dump requested (%s), but nothing is armed -- "
            "advanced.transition_flash_prevent is off, or this is not build 332841.",
            trigger ? trigger : "?");
        return;
    }
    requestDump(trigger ? trigger : "a key you pressed", g_frame.load(std::memory_order_relaxed), 0);
}

void transitionFlashPreventShutdown() {
    if (!g_armed.load(std::memory_order_relaxed)) return;
    reportLine("--- transition flash prevent, session total:");
}

}  // namespace edvr
