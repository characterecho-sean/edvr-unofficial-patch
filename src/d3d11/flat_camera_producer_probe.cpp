#include "flat_camera_producer_probe.h"

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <memory>

#include "../common/code_hook.h"
#include "../common/config.h"
#include "../common/log.h"
#include "../common/module_name.h"
#include "../common/runtime_profile.h"
#include "../common/vtable_hook.h" // isExecutableAddress
#include "debug_register_core.h"

namespace edvr {
namespace {

// The camera producer probe (docs/design-flat-camera-integration.md C1->C2).
//
// Step 1 hooks the Ghidra-validated single-site upload helper
// (EliteDangerous64.exe+0x51B640, the f3dx Map/memcpy/Unmap path) to learn
// the staging pool block of the 5376-byte scene CB. Step 2 arms one hardware
// write breakpoint per uploading thread on the block's camera-row span (rows
// 270..271, the 0x10E0..0x1100 window); each hit names the writer's RIP with
// a bounded stack. Budgets: 8 distinct blocks, 8 watches, 8 writer hits, 30
// seconds armed. A refusal stands down and says why; it never guesses.
//
// Safety invariants, learned the expensive way (two crashed flights and the
// 2026-09-28 review's R1-R4):
// - Every hardware watch ever armed is tracked in the table below until its
//   hardware is verifiably cleared -- an external Suspend/Get/Set/Resume
//   disarm, or the VEH clearing the faulting thread's own exception context.
//   Nothing else touches Dr0/Dr7: SetThreadContext from inside the handler is
//   overwritten by the kernel's context restore, and Get/SetThreadContext on
//   the CURRENT thread is not a supported way to program debug registers.
// - The VEH claims any single-step whose Dr0 matches ANY tracked watch on the
//   current thread, armed or stale. An orphaned watch that raises a
//   single-step nobody claims is an unhandled exception, and it has taken
//   this game down once already.
// - Once installed, the upload hook is never uninstalled and the relay and
//   trampoline are never freed. Closing the gate does not drain a callback
//   already inside the relay; freeing code it can still call is a use-after-
//   free in someone else's render thread. Off is the gate, not removal --
//   the pose_reader_watch discipline.

constexpr uintptr_t kUploadRva = 0x51B640;
constexpr uint8_t kUploadPrologue[16] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74,
                                         0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x50, 0x41};
constexpr uint32_t kSceneBytes = 5376;
constexpr uint32_t kCameraRowOffset = 270u * 16u; // 0x10E0
constexpr size_t kRelayBytes = 44;
constexpr uint32_t kOriginalLiteral = 36;
constexpr uint32_t kMaxBlocks = 8, kMaxWatches = 8, kMaxHits = 8, kArmMs = 30000;

struct Watch {
    std::atomic<DWORD> tid{0};           // owning thread; 0 = free slot
    std::atomic<uintptr_t> address{0};   // watched address; 0 = none
    std::atomic<uint32_t> generation{0}; // bumped on every claim; frees are CAS
    std::atomic<bool> armRequested{false};
    std::atomic<bool> armed{false};
    std::atomic<bool> stale{false};      // logically done; hardware still tracked
    std::atomic<uint64_t> armedAtMs{0};
    std::atomic<uint32_t> hits{0};
};

struct ProbeState {
    std::atomic<bool> installed{false};
    CodeHook hook;
    uint8_t* relay = nullptr;
    const char* failReason = "not attempted";
    // The staging blocks seen from scene-sized uploads (dedup by pointer).
    // Claimed by fetch_add: more than one thread can upload through the
    // helper, and a check-then-append race would write past the array.
    const void* blocks[kMaxBlocks]{};
    std::atomic<uint32_t> blockCount{0};
    bool baseNoted = false;
};
ProbeState g_probe;
Watch g_watches[kMaxWatches];
std::atomic<uint32_t> g_totalHits{0};
std::atomic<uint64_t> g_frame{0};
std::atomic<uintptr_t> g_gate{0};
// The trampoline to the real upload helper, stored by prepareRelay; the
// relay callback calls it so the scene CB upload is never swallowed.
std::atomic<uintptr_t> g_uploadForward{0};
void* g_veh = nullptr;
bool g_holdNoted = false; // frame thread only: the held-open note, said once

bool sehCheck(uintptr_t at, const uint8_t* expected, size_t bytes) noexcept {
    __try { return std::memcmp(reinterpret_cast<const void*>(at), expected, bytes) == 0; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

uint8_t* allocateRelay(uintptr_t target) noexcept {
    const uintptr_t granularity = 64 * 1024, span = 0x7FFF0000ull;
    const uintptr_t begin = (target - span) & ~(granularity - 1);
    for (uintptr_t candidate = target; candidate >= begin; candidate -= granularity) {
        auto* p = static_cast<uint8_t*>(VirtualAlloc(reinterpret_cast<void*>(candidate), 4096,
                                                     MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        if (p) return p;
    }
    return nullptr;
}

void buildRelay(uint8_t* code, const void* gate, void* callback) noexcept {
    // mov rax,&gate; cmp qword ptr[rax],0; je original; jmp [callback];
    // original: jmp [trampoline]. RAX/flags are volatile on entry here.
    const uint8_t body[kRelayBytes] = {
        0x48,0xB8,0,0,0,0,0,0,0,0, 0x48,0x83,0x38,0,
        0x74,0x0E, 0xFF,0x25,0,0,0,0, 0,0,0,0,0,0,0,0,
        0xFF,0x25,0,0,0,0, 0,0,0,0,0,0,0,0};
    std::memcpy(code, body, sizeof(body));
    const uintptr_t gateAddress = reinterpret_cast<uintptr_t>(gate);
    const uintptr_t callbackAddress = reinterpret_cast<uintptr_t>(callback);
    std::memcpy(code + 2, &gateAddress, 8); std::memcpy(code + 22, &callbackAddress, 8);
}

bool prepareRelay(void* trampoline, void*) noexcept {
    const uintptr_t address = reinterpret_cast<uintptr_t>(trampoline);
    std::memcpy(g_probe.relay + kOriginalLiteral, &address, 8);
    DWORD oldProtect = 0;
    if (!VirtualProtect(g_probe.relay, 4096, PAGE_EXECUTE_READ, &oldProtect) ||
        !FlushInstructionCache(GetCurrentProcess(), g_probe.relay, kRelayBytes)) return false;
    g_uploadForward.store(address, std::memory_order_release);
    return true;
}

// DR0/Dr7 slot 0 on one thread, by handle, from OUTSIDE that thread:
// Suspend/GetContext/SetContext/Resume, the supported mechanism
// (the Dr7 slot-0 pattern (debug_register_core.h), with this probe's write-mode bits).
bool setThreadDr(DWORD tid, uint64_t watchAddress, bool arm) noexcept {
    HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT, FALSE, tid);
    if (!h) return false;
    bool ok = false;
    if (SuspendThread(h) != static_cast<DWORD>(-1)) {
        CONTEXT ctx{};
        ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
        if (GetThreadContext(h, &ctx)) {
            const uint32_t low = static_cast<uint32_t>(ctx.Dr7 & 0xFFFFFFFFull);
            const uint32_t high = static_cast<uint32_t>((ctx.Dr7 >> 32) & 0xFFFFFFFFull);
            ctx.Dr7 = (static_cast<DWORD64>(high) << 32) |
                      (arm ? dr7::armSlot0Dr7(low, dr7::kDr7RwWrite) : dr7::disarmSlot0Dr7(low));
            if (arm) ctx.Dr0 = static_cast<DWORD64>(watchAddress);
            ok = SetThreadContext(h, &ctx) != FALSE;
        }
        ResumeThread(h);
    }
    CloseHandle(h);
    return ok;
}

// The uploading thread and the frame thread are the SAME thread here (the
// uploads ride the render thread, and processWatches runs on its boundary):
// suspending it from itself is an instant deadlock, which is exactly what
// the 16:32 crash was. For the self case a short-lived helper does the
// suspend/arm/resume from outside, pose_reader_watch's own pattern.
struct HelperDrArgs { DWORD tid; uint64_t address; bool arm; };

DWORD WINAPI helperDrProc(LPVOID param) {
    std::unique_ptr<HelperDrArgs> args(static_cast<HelperDrArgs*>(param));
    setThreadDr(args->tid, args->address, args->arm);
    return 0;
}

bool setThreadDrSafe(DWORD tid, uint64_t address, bool arm) noexcept {
    if (tid != GetCurrentThreadId()) return setThreadDr(tid, address, arm);
    auto* args = new (std::nothrow) HelperDrArgs{tid, address, arm};
    if (!args) return false;
    HANDLE h = CreateThread(nullptr, 0, &helperDrProc, args, 0, nullptr);
    if (!h) { delete args; return false; }
    WaitForSingleObject(h, 2000);
    CloseHandle(h);
    return true;
}

void freeWatch(Watch& w) {
    w.generation.fetch_add(1, std::memory_order_acq_rel);
    w.armed.store(false, std::memory_order_release);
    w.armRequested.store(false, std::memory_order_release);
    w.stale.store(false, std::memory_order_release);
    w.address.store(0, std::memory_order_release);
    w.tid.store(0, std::memory_order_release);
}

// The uploading thread asks for a watch on this block; the frame thread arms
// it from outside (there is no supported way to program a running thread's
// own debug registers). One live watch per thread: DR0 holds one address.
void requestWatch(const void* block) {
    const DWORD tid = GetCurrentThreadId();
    const uintptr_t address = reinterpret_cast<uintptr_t>(block) + kCameraRowOffset;
    for (auto& w : g_watches) {
        if (w.tid.load(std::memory_order_acquire) == tid) {
            Log::get().note("flat camera producer: thread %lu already holds a camera-row watch; "
                            "the request for %p is refused, not queued",
                            static_cast<unsigned long>(tid), reinterpret_cast<void*>(address));
            return;
        }
    }
    for (auto& w : g_watches) {
        DWORD expected = 0;
        if (!w.tid.compare_exchange_strong(expected, tid, std::memory_order_acq_rel)) continue;
        w.generation.fetch_add(1, std::memory_order_acq_rel);
        w.address.store(address, std::memory_order_release);
        w.armRequested.store(true, std::memory_order_release);
        Log::get().note("flat camera producer: camera-row write watch requested at %p on thread %lu (frame=%llu)",
                        reinterpret_cast<void*>(address), static_cast<unsigned long>(tid),
                        (unsigned long long)g_frame.load(std::memory_order_acquire));
        return;
    }
    Log::get().note("flat camera producer: no free watch slot for %p (%u already tracked); refused, not queued",
                    reinterpret_cast<void*>(address), kMaxWatches);
}

LONG CALLBACK producerWatchVeh(EXCEPTION_POINTERS* ep) {
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP) return EXCEPTION_CONTINUE_SEARCH;
    if (!(ep->ContextRecord->Dr6 & 1)) return EXCEPTION_CONTINUE_SEARCH;
    const DWORD tid = GetCurrentThreadId();
    Watch* hit = nullptr;
    for (auto& w : g_watches) {
        if (w.tid.load(std::memory_order_acquire) == tid &&
            w.address.load(std::memory_order_acquire) ==
                static_cast<uintptr_t>(ep->ContextRecord->Dr0)) {
            hit = &w;
            break;
        }
    }
    if (!hit) return EXCEPTION_CONTINUE_SEARCH; // genuinely somebody else's
    const uint32_t generation = hit->generation.load(std::memory_order_acquire);
    // Dr6 is sticky: clear it first, as pose_reader_watch documents, or the
    // resumed thread re-traps immediately.
    ep->ContextRecord->Dr6 = 0;
    hit->hits.fetch_add(1, std::memory_order_acq_rel);
    const uint32_t total = g_totalHits.fetch_add(1, std::memory_order_acq_rel) + 1;
    if (!hit->stale.load(std::memory_order_acquire)) {
        if (total <= kMaxHits) {
            char ripBuf[96];
            const char* rip = moduleBrief(reinterpret_cast<void*>(ep->ContextRecord->Rip), ripBuf, sizeof(ripBuf));
            void* frames[8] = {};
            const USHORT n = CaptureStackBackTrace(0, 8, frames, nullptr);
            char line[768]{}; size_t used = std::snprintf(line, sizeof(line), "%s", rip);
            for (USHORT i = 0; i < n && used < sizeof(line) - 96; ++i) {
                if (!isExecutableAddress(frames[i])) continue;
                char briefBuf[96];
                const char* brief = moduleBrief(frames[i], briefBuf, sizeof(briefBuf));
                used += static_cast<size_t>(std::snprintf(line + used, sizeof(line) - used, " <- %s", brief));
            }
            Log::get().note("flat camera producer: camera-row writer #%u (thread %lu): %s",
                            total, static_cast<unsigned long>(tid), line);
        }
        if (total >= kMaxHits) {
            // The budget is global: every live watch stands down. Each clears
            // on its owning thread's next hit or an external disarm.
            for (auto& w : g_watches)
                if (w.armed.load(std::memory_order_acquire)) w.stale.store(true, std::memory_order_release);
            Log::get().note("flat camera producer: writer budget full (%u hit(s)); all watches stand down",
                            total);
        }
    }
    if (hit->stale.load(std::memory_order_acquire)) {
        // Clear the hardware slot right here, in the context record the
        // kernel restores on continue -- the one place that cannot be lost.
        const uint32_t low = static_cast<uint32_t>(ep->ContextRecord->Dr7 & 0xFFFFFFFFull);
        const uint32_t high = static_cast<uint32_t>((ep->ContextRecord->Dr7 >> 32) & 0xFFFFFFFFull);
        ep->ContextRecord->Dr7 = (static_cast<DWORD64>(high) << 32) | dr7::disarmSlot0Dr7(low);
        ep->ContextRecord->Dr0 = 0;
        // Free the slot only if nobody re-claimed it under us; a re-claim
        // bumps the generation, and wiping a fresh watch's fields would
        // orphan its hardware exactly the way this table exists to prevent.
        uint32_t expected = generation;
        if (hit->generation.compare_exchange_strong(expected, generation + 1, std::memory_order_acq_rel)) {
            hit->armed.store(false, std::memory_order_release);
            hit->armRequested.store(false, std::memory_order_release);
            hit->stale.store(false, std::memory_order_release);
            hit->address.store(0, std::memory_order_release);
            hit->tid.store(0, std::memory_order_release);
        }
    }
    ep->ContextRecord->EFlags |= 0x10000; // RF: do not re-trigger on this instruction
    return EXCEPTION_CONTINUE_EXECUTION;
}

// Frame-thread watch maintenance: arm what was requested, time out what
// overstayed, and give stale watches an external disarm so their hardware
// does not wait for the next watched write.
void processWatches() {
    const uint64_t now = GetTickCount64();
    for (auto& w : g_watches) {
        const DWORD tid = w.tid.load(std::memory_order_acquire);
        if (tid == 0) continue;
        if (w.armRequested.load(std::memory_order_acquire) &&
            !w.armed.load(std::memory_order_acquire)) {
            const uintptr_t address = w.address.load(std::memory_order_acquire);
            if (setThreadDrSafe(tid, address, true)) {
                w.armedAtMs.store(now, std::memory_order_release);
                w.armed.store(true, std::memory_order_release);
                Log::get().note("flat camera producer: camera-row write watch armed at %p on thread %lu (frame=%llu)",
                                reinterpret_cast<void*>(address), static_cast<unsigned long>(tid),
                                (unsigned long long)g_frame.load(std::memory_order_acquire));
            } else {
                Log::get().note("flat camera producer: could not arm the camera-row watch on thread %lu "
                                "(thread gone or its registers refused); the request is dropped, not queued",
                                static_cast<unsigned long>(tid));
                freeWatch(w);
            }
            continue;
        }
        if (!w.armed.load(std::memory_order_acquire)) continue;
        if (!w.stale.load(std::memory_order_acquire) &&
            now - w.armedAtMs.load(std::memory_order_acquire) > kArmMs) {
            w.stale.store(true, std::memory_order_release);
            Log::get().note("flat camera producer: camera-row write watch on thread %lu disarmed "
                            "(30 s armed without a full budget; %u hit(s) recorded)",
                            static_cast<unsigned long>(tid), w.hits.load(std::memory_order_acquire));
        }
        if (w.stale.load(std::memory_order_acquire) && setThreadDrSafe(tid, 0, false)) {
            Log::get().note("flat camera producer: camera-row write watch on thread %lu cleared from outside",
                            static_cast<unsigned long>(tid));
            freeWatch(w);
        }
        // A failed external disarm leaves the slot tracked; the VEH clears
        // the hardware on the owning thread's next watched write.
    }
}

void observeSceneUpload(const void* src, int sizeA, int sizeB) {
    if (static_cast<int64_t>(sizeA) * sizeB != kSceneBytes || !src) return;
    const uint32_t known = g_probe.blockCount.load(std::memory_order_acquire);
    for (uint32_t i = 0; i < known && i < kMaxBlocks; ++i)
        if (g_probe.blocks[i] == src) return;
    const uint32_t slot = g_probe.blockCount.fetch_add(1, std::memory_order_acq_rel);
    if (slot >= kMaxBlocks) return; // the block budget is full; count only
    g_probe.blocks[slot] = src;
    Log::get().note("flat camera producer: scene-sized staging block %p (%u/%u)",
                    src, slot + 1, kMaxBlocks);
    if (g_totalHits.load(std::memory_order_acquire) < kMaxHits) requestWatch(src);
}

using UploadFn = int (__fastcall*)(uintptr_t, uintptr_t, const void*, int, int);
int __fastcall uploadRelay(uintptr_t a, uintptr_t b, const void* src, int sizeA, int sizeB) noexcept {
    const auto forward = reinterpret_cast<UploadFn>(g_uploadForward.load(std::memory_order_acquire));
    if (!forward) return 0;
    observeSceneUpload(src, sizeA, sizeB);
    return forward(a, b, src, sizeA, sizeB);
}

void standDown(const char* why) {
    const bool gateWas = g_gate.exchange(0, std::memory_order_acq_rel) != 0;
    bool tracked = false;
    for (auto& w : g_watches) {
        if (w.tid.load(std::memory_order_acquire) == 0) continue;
        tracked = true;
        if (!w.armed.load(std::memory_order_acquire)) {
            freeWatch(w); // requested but never armed: nothing to clear
            continue;
        }
        w.stale.store(true, std::memory_order_release);
        if (setThreadDrSafe(w.tid.load(std::memory_order_acquire), 0, false)) freeWatch(w);
    }
    if (g_probe.installed.load(std::memory_order_acquire) && (gateWas || tracked) && !g_holdNoted) {
        g_holdNoted = true;
        Log::get().note("flat camera producer: %s; the upload hook stays in place as an inert "
                        "pass-through for process lifetime -- closing the gate does not drain a "
                        "callback already inside, and uninstalling would free code it can still call",
                        why);
    }
}

} // namespace

void flatCameraProducerProbeFrame(uint64_t frame) {
    g_frame.store(frame, std::memory_order_release);
    if (!runtimeFlatProfile()) { standDown("the flat profile is off"); return; }
    const bool wanted = Config::get().getBool("advanced.flat_camera_producer_probe", false); // "0" and "false" are off; this arms game-code patching
    if (!wanted) { standDown("advanced.flat_camera_producer_probe is off"); return; }
    if (g_probe.installed.load(std::memory_order_acquire)) { processWatches(); return; }
    if (g_probe.relay) return; // a failed install is final for the session
    const HMODULE game = GetModuleHandleW(L"EliteDangerous64.exe");
    if (!game) {
        if (!g_probe.baseNoted) {
            g_probe.baseNoted = true;
            g_probe.failReason = "EliteDangerous64.exe is not loaded in this process";
            Log::get().note("flat camera producer: wanted but %s; standing down", g_probe.failReason);
        }
        return;
    }
    const uintptr_t base = reinterpret_cast<uintptr_t>(game);
    if (!sehCheck(base + kUploadRva, kUploadPrologue, sizeof(kUploadPrologue))) {
        g_probe.failReason = "upload helper prologue mismatch at this build (not the Ghidra-verified shape)";
        Log::get().note("flat camera producer: %s; the probe stands down", g_probe.failReason);
        g_probe.relay = reinterpret_cast<uint8_t*>(1); // do not retry
        return;
    }
    // The handler goes in BEFORE anything that can arm a watch: a hardware
    // breakpoint with nobody to claim its single-step is an unhandled
    // exception. A failed registration refuses the whole probe, by name.
    if (!g_veh) {
        g_veh = AddVectoredExceptionHandler(1, &producerWatchVeh);
        if (!g_veh) {
            g_probe.failReason = "AddVectoredExceptionHandler failed; a watch without its handler would "
                                 "raise single-steps nobody claims";
            Log::get().note("flat camera producer: %s; the probe stands down", g_probe.failReason);
            g_probe.relay = reinterpret_cast<uint8_t*>(1); // do not retry
            return;
        }
    }
    g_probe.relay = allocateRelay(base + kUploadRva);
    if (!g_probe.relay) { g_probe.failReason = "relay allocation failed (no free memory within 2 GB)"; return; }
    buildRelay(g_probe.relay, &g_gate, &uploadRelay);
    if (!g_probe.hook.install(reinterpret_cast<void*>(base + kUploadRva), g_probe.relay, nullptr,
                              "camera-producer-upload", &prepareRelay, &g_probe)) {
        VirtualFree(g_probe.relay, 0, MEM_RELEASE); g_probe.relay = reinterpret_cast<uint8_t*>(1);
        g_probe.failReason = "CodeHook refused it (its own line above names why)";
        Log::get().note("flat camera producer: %s; the probe stands down", g_probe.failReason);
        return;
    }
    g_probe.installed.store(true, std::memory_order_release);
    g_gate.store(1, std::memory_order_release);
    Log::get().note("flat camera producer: upload hook installed at EliteDangerous64.exe+0x%llX; "
                    "scene-sized uploads now name their staging block, first one requests a camera-row write watch",
                    static_cast<unsigned long long>(kUploadRva));
}

} // namespace edvr
