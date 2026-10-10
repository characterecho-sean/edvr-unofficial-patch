#include "vertex_resync_hook.h"
#include "vertex_resync_core.h"
#include "../common/code_hook.h"
#include "../common/game_call_probe.h"
#include "../common/log.h"

#include <windows.h>
#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

// The vertex-buffer resync (vertex_resync_core.h says what is wrong and why this is the repair). This file is the glue: find FlushIA, check it is the function
// that was read, patch its entry with the project's CodeHook through a relay, and run the pure repair under SEH before the original.

namespace edvr {
namespace {

// --- Relay machinery, the same copy the other engine hooks carry (grep kRelayBytes): the target is in the game's module and this DLL loads more than two
// gigabytes away, so a five-byte E9 cannot reach a replacement here directly. Kept as a copy rather than a shared unit so none of the flight-proven files is touched.
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
                auto* p = static_cast<uint8_t*>(VirtualAlloc(reinterpret_cast<void*>(candidate), 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
                if (p) return p;
            }
        }
        if (end <= at) break;
        at = end;
    }
    return nullptr;
}

void buildRelay(uint8_t* code, const void* gate, void* callback) noexcept {
    // mov rax,&gate; cmp qword ptr[rax],0; je original; jmp [callback]; original: jmp [trampoline]. RAX and the flags are volatile and FlushIA takes nothing in RAX
    // on entry. No stack adjustment, no nonvolatile register touched.
    const uint8_t body[kRelayBytes] = {
        0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0, 0x48, 0x83, 0x38, 0,
        0x74, 0x0E, 0xFF, 0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        0xFF, 0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    std::memcpy(code, body, sizeof(body));
    const uintptr_t gateAddress = reinterpret_cast<uintptr_t>(gate);
    const uintptr_t callbackAddress = reinterpret_cast<uintptr_t>(callback);
    std::memcpy(code + 2, &gateAddress, 8);
    std::memcpy(code + 22, &callbackAddress, 8);
}

uint8_t* g_relay = nullptr;
std::atomic<uintptr_t> g_forward{0};
std::atomic<uintptr_t> g_relayGate{0};   // 0: the relay forwards straight to the original (never installed, or stood down)
CodeHook g_hook;

bool prepareRelay(void* trampoline, void*) noexcept {
    const uintptr_t address = reinterpret_cast<uintptr_t>(trampoline);
    std::memcpy(g_relay + kOriginalLiteral, &address, 8);
    DWORD oldProtect = 0;
    if (!VirtualProtect(g_relay, 4096, PAGE_EXECUTE_READ, &oldProtect) || !FlushInstructionCache(GetCurrentProcess(), g_relay, kRelayBytes)) return false;
    g_forward.store(address, std::memory_order_release);
    return true;
}

// --- State and instruments -------------------------------------------------------------------------------------------------------------------------------------
std::atomic<bool> g_attempted{false};          // install has been tried (once, ever)
std::atomic<bool> g_installed{false};
std::atomic<uint64_t> g_repairedTotal{0}, g_faults{0};   // rare events: a locked add is fine
// The count of flushes seen: bumped on every flush with a relaxed load and a relaxed store, no lock prefix, on its own cache line (vertex_resync_core.h says why).
vresync::FlushCount g_flushes;
vresync::WindowCount g_window(vresync::kWindowMs);        // repairs in the running 60 s (said while non-zero)
vresync::WindowCount g_beat(vresync::kHeartbeatMs);       // repairs in the running 10 min (said always, zero included)
uint64_t g_flushesAtLastBeat = 0;                         // the poll thread's own
vresync::SightingGate g_sightings;

#ifdef EDVR_VERTEX_RESYNC_TEST
uintptr_t g_testTarget = 0;
std::mutex g_testMutex;
std::vector<std::string> g_testLines;
#endif

// The one place a line is written. The log, and in the rig a copy.
void say(const char* line) {
    Log::get().note("%s", line);
#ifdef EDVR_VERTEX_RESYNC_TEST
    std::lock_guard<std::mutex> lock(g_testMutex);
    g_testLines.emplace_back(line);
#endif
}
void sayf(const char* fmt, ...) {
    char line[900];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);
    say(line);
}

// --- SEH-guarded reads of the game's memory. Each is its own small function (no destructors in a __try body), the discipline CodeHook's neighbours keep. ------------
__declspec(noinline) bool sehCheckBytes(uintptr_t address, const uint8_t* expected, size_t n) noexcept {
    __try { return std::memcmp(reinterpret_cast<const void*>(address), expected, n) == 0; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
__declspec(noinline) bool checkIdentity(uintptr_t base, const char** why) noexcept {
    __try {
        uint32_t peOff = 0;
        std::memcpy(&peOff, reinterpret_cast<const void*>(base + 0x3C), 4);
        if (peOff > 0x1000) { *why = "the PE header offset is implausible"; return false; }
        uint32_t timestamp = 0, imageSize = 0;
        std::memcpy(&timestamp, reinterpret_cast<const void*>(base + peOff + 8), 4);
        std::memcpy(&imageSize, reinterpret_cast<const void*>(base + peOff + 0x50), 4);
        if (!vresync::identityMatches(timestamp, imageSize)) { *why = "not build 332841 (PE timestamp or image size differs)"; return false; }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *why = "a read faulted while checking the PE header";
        return false;
    }
}
// The repair itself, under SEH: the pointers it follows (the layout object, the wrapper) are the game's.
__declspec(noinline) bool guardedResync(uintptr_t pso, uintptr_t desired, uintptr_t applied, vresync::Report* out) noexcept {
    __try {
        *out = vresync::resync(reinterpret_cast<const uint8_t*>(pso), reinterpret_cast<const uint8_t*>(desired), reinterpret_cast<uint8_t*>(applied));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// A fault is counted, said once, and eight stand the hook down: a function that keeps faulting is not the function that was read.
void noteFault() noexcept {
    const uint64_t n = g_faults.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n == 1) say("vertex resync: a read inside the flush faulted and was absorbed; the game's flush ran as it would have. Counting; eight stand the hook down.");
    if (n == 8) {
        g_relayGate.store(0, std::memory_order_release);
        say("vertex resync: stood down after eight faults; the relay now forwards straight to the game's flush.");
    }
}

void noteSighting(const vresync::Report& r, uintptr_t desired) noexcept {
    if (!r.hasFirst || !g_sightings.take()) return;
    const GameCallStack stack = captureGameCallStack();
    char line[900];
    vresync::formatSightingLine(line, sizeof(line), g_sightings.taken(), r.first, desired >= vresync::kListDesired ? desired - vresync::kListDesired : 0, r.layoutCount,
                                GetCurrentThreadId(), stack.gameFrames, stack.rvas);
    say(line);
}

// The process-exit line (Log::setExitLine): the session totals, from atomics, into the caller's stack buffer. Nothing is armed, nothing is said.
int exitLine(char* out, size_t cap) {
    if (!g_installed.load(std::memory_order_acquire) || g_relayGate.load(std::memory_order_acquire) == 0) return 0;
    vresync::formatSessionLine(out, cap, g_repairedTotal.load(std::memory_order_relaxed), g_flushes.read());
    return static_cast<int>(std::strlen(out));
}

using FlushFn = uintptr_t (__fastcall*)(uintptr_t, uintptr_t, uintptr_t, uintptr_t);
// FlushIA(pso = rcx, a command context = rdx, desired = r8, applied = r9) -> al. Four register parameters, nothing on the stack (the dump reads no [rsp+0x70] or above).
__declspec(noinline) uintptr_t __fastcall flushIaObserved(uintptr_t pso, uintptr_t context, uintptr_t desired, uintptr_t applied) noexcept {
    const auto forward = reinterpret_cast<FlushFn>(g_forward.load(std::memory_order_acquire));
    if (!forward) return 0;   // stood down at install; the relay is unreachable then
    g_flushes.bump();
    if (pso && desired && applied) {
        vresync::Report r;
        if (!guardedResync(pso, desired, applied, &r)) {
            noteFault();
        } else if (r.repaired) {
            g_repairedTotal.fetch_add(r.repaired, std::memory_order_relaxed);
            g_window.add(r.repaired);
            g_beat.add(r.repaired);
            noteSighting(r, desired);
        }
    }
    return forward(pso, context, desired, applied);
}

}  // namespace

void vertexResyncInstall() {
    bool expected = false;
    if (!g_attempted.compare_exchange_strong(expected, true)) return;   // once, ever: a refusal is final
    uintptr_t target = 0;
#ifdef EDVR_VERTEX_RESYNC_TEST
    target = g_testTarget;
#endif
    if (!target) {
        const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        if (!base) {
            say("vertex resync: NOT installed -- the game's module could not be resolved. The game's flush runs as it always did.");
            return;
        }
        const char* why = nullptr;
        if (!checkIdentity(base, &why)) {
            sayf("vertex resync: NOT installed -- the game build differs (%s); EliteDangerous64.exe+0x%llX was not touched. The game's flush runs as it always did.",
                 why ? why : "?", static_cast<unsigned long long>(vresync::kFlushIaRva));
            return;
        }
        target = base + vresync::kFlushIaRva;
    }
    if (!sehCheckBytes(target, vresync::kFlushIaPrologue, vresync::kPrologueBytes)) {
        sayf("vertex resync: NOT installed -- the %zu bytes at EliteDangerous64.exe+0x%llX are not build 332841's flush prologue. The game's flush runs as it always did.",
             vresync::kPrologueBytes, static_cast<unsigned long long>(vresync::kFlushIaRva));
        return;
    }
    g_relay = allocateRelay(target);
    if (!g_relay) {
        say("vertex resync: NOT installed -- no executable memory could be placed within two gigabytes of the flush. The game's flush runs as it always did.");
        return;
    }
    buildRelay(g_relay, &g_relayGate, reinterpret_cast<void*>(&flushIaObserved));
    if (!g_hook.install(reinterpret_cast<void*>(target), g_relay, nullptr, "vertex-resync-flush-ia", &prepareRelay, nullptr)) {
        VirtualFree(g_relay, 0, MEM_RELEASE);
        g_relay = nullptr;
        say("vertex resync: NOT installed -- CodeHook refused it (its own line above, tagged vertex-resync-flush-ia, names why). The game's flush runs as it always did.");
        return;
    }
    g_relayGate.store(1, std::memory_order_release);
    g_installed.store(true, std::memory_order_release);
    Log::get().setExitLine(&exitLine);   // the game never unloads this DLL: its process exit says the session line
    sayf("vertex resync: hook armed at EliteDangerous64.exe+0x%llX (Frontier's input-assembler flush, build 332841), stolen=%zu bytes, prologue %zu/%zu bytes verified. Before each "
         "flush the applied vertex-buffer cache is made to agree with the desired state for the slots the draw's layout uses; the first %u sightings are logged, then a count every "
         "60 s while it is non-zero, a heartbeat every 10 min (zero counts included) and a line when the session ends.",
         static_cast<unsigned long long>(vresync::kFlushIaRva), g_hook.stolenBytes(), vresync::kPrologueBytes, vresync::kPrologueBytes, vresync::kMaxSightings);
}

void vertexResyncPoll(uint64_t nowMs) {
    uint32_t n = 0;
    if (g_window.tick(nowMs, &n) && n) {
        char line[200];
        vresync::formatResyncLine(line, sizeof(line), n);
        say(line);
    }
    uint32_t repairedInBeat = 0;
    if (g_beat.tick(nowMs, &repairedInBeat)) {
        const uint64_t flushes = g_flushes.read();
        const uint64_t seen = flushes - g_flushesAtLastBeat;
        g_flushesAtLastBeat = flushes;
        // Said while the hook is armed and not stood down (a stand-down has its own line): the zero-count line is what tells quiet from absent.
        if (g_installed.load(std::memory_order_acquire) && g_relayGate.load(std::memory_order_acquire) != 0) {
            char line[200];
            vresync::formatHeartbeatLine(line, sizeof(line), repairedInBeat, seen);
            say(line);
        }
    }
}

void vertexResyncShutdown() {
    char line[200];
    if (exitLine(line, sizeof(line)) > 0) say(line);
}

#ifdef EDVR_VERTEX_RESYNC_TEST
void vertexResyncTestSetTarget(uintptr_t target) { g_testTarget = target; }
void vertexResyncTestReset() {
    g_attempted.store(false);
    g_repairedTotal = g_faults = 0;
    g_flushes.reset();
    g_flushesAtLastBeat = 0;
    g_window.reset();
    g_beat.reset();
    g_sightings.reset();
    std::lock_guard<std::mutex> lock(g_testMutex);
    g_testLines.clear();
}
VertexResyncTestState vertexResyncTestState() {
    VertexResyncTestState s;
    s.installed = g_installed.load();
    s.flushes = g_flushes.read();
    s.repaired = g_repairedTotal.load();
    s.faults = g_faults.load();
    s.sightings = g_sightings.taken();
    s.attempted = g_attempted.load();
    return s;
}
size_t vertexResyncTestLineCount() {
    std::lock_guard<std::mutex> lock(g_testMutex);
    return g_testLines.size();
}
const char* vertexResyncTestLine(size_t i) {
    std::lock_guard<std::mutex> lock(g_testMutex);
    return i < g_testLines.size() ? g_testLines[i].c_str() : "";
}
int vertexResyncTestExitLine(char* out, size_t cap) { return exitLine(out, cap); }
#endif

}  // namespace edvr
