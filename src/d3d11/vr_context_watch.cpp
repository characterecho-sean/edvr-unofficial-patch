// The hardware write watchpoint on ctx+0x3564 (vr_context_watch.h says what and why). Log only.
#include "vr_context_watch.h"

#include "vr_display_observer.h"  // vrDisplayCallerText: "game RVA" or "<module>+0x..." labels
#include "vr_ssaa_gate.h"         // vrSsaaGateFrame

#include "../common/log.h"

#include <windows.h>
#include <tlhelp32.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace edvr {
namespace {

constexpr uint32_t kHitCap = 16;
// The setter's store is at RVA 0x28767EF, inside the setter at 0x28767D0 (build 332841); a write from there is the game's own apply.
constexpr uint32_t kSetterLoRva = 0x28767D0, kSetterHiRva = 0x2876800;

std::atomic<uintptr_t> g_addr{0};
std::atomic<bool> g_tried{false};       // arming was attempted (once per session)
std::atomic<bool> g_armed{false};       // the handler listens and the registers are set (or being set)
std::atomic<uint32_t> g_hits{0};
std::atomic<bool> g_disarmWanted{false};
std::atomic<uint32_t> g_pendingKind{0};  // 1 arm, 2 disarm: read by the worker
char g_disarmWhy[96] = "";
PVOID g_veh = nullptr;

// Sets (arm) or clears (disarm) DR0 and DR7 on every thread of this process except the calling one. Each thread is suspended for
// the change. Returns the number changed; *failed counts the threads that could not be stopped or set.
uint32_t setDebugRegistersAll(bool arm, uintptr_t addr, uint32_t* failed) {
    uint32_t changed = 0;
    *failed = 0;
    const DWORD self = GetCurrentThreadId();
    const DWORD pid = GetCurrentProcessId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) {
        *failed = 1;
        return 0;
    }
    THREADENTRY32 te{};
    te.dwSize = sizeof(te);
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != pid || te.th32ThreadID == self) continue;
        HANDLE th = OpenThread(THREAD_ALL_ACCESS, FALSE, te.th32ThreadID);
        if (!th) {
            ++*failed;
            continue;
        }
        if (SuspendThread(th) == static_cast<DWORD>(-1)) {
            CloseHandle(th);
            ++*failed;
            continue;
        }
        CONTEXT c{};
        c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
        bool done = GetThreadContext(th, &c) != 0;
        if (done) {
            const DWORD64 mask = 0x3ull | (0xFull << 16);  // L0/G0 enable bits and the RW0/LEN0 field
            if (arm) {
                c.Dr0 = addr;
                c.Dr6 = 0;
                c.Dr7 = (c.Dr7 & ~mask) | 0x1ull | (0x1ull << 16) | (0x3ull << 18);  // L0, write, 4 bytes
            } else {
                c.Dr0 = 0;
                c.Dr7 &= ~mask;
            }
            done = SetThreadContext(th, &c) != 0;
        }
        ResumeThread(th);
        CloseHandle(th);
        if (done) ++changed;
        else ++*failed;
    }
    CloseHandle(snap);
    return changed;
}

// The stack's return addresses that lie in the game image: a best-effort caller list (the stack is scanned, not unwound).
void callersText(const CONTEXT* c, char* out, size_t n) {
    out[0] = '\0';
    size_t used = 0;
    int found = 0;
    const uintptr_t sp = static_cast<uintptr_t>(c->Rsp);
    const uintptr_t game = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    for (uint32_t i = 0; i < 64 && found < 6 && used + 2 < n; ++i) {
        const uint64_t v = *reinterpret_cast<const volatile uint64_t*>(sp + 8 * i);
        if (v < game || v - game >= 0x10000000ull) continue;
        char one[96];
        vrDisplayCallerText(reinterpret_cast<void*>(static_cast<uintptr_t>(v)), one, sizeof(one));
        const int m = std::snprintf(out + used, n - used, "%s%s", used ? " <- " : "", one);
        if (m < 0) break;
        used += static_cast<size_t>(m);
        ++found;
    }
}

void logHit(const CONTEXT* c, uint32_t hit) {
    const uintptr_t rip = static_cast<uintptr_t>(c->Rip);
    const uintptr_t game = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    const uint32_t rva = static_cast<uint32_t>(rip - game);
    char writer[96], chain[320];
    vrDisplayCallerText(reinterpret_cast<void*>(rip), writer, sizeof(writer));
    const char* label = "EDVR or another module";
    if (std::strncmp(writer, "game RVA", 8) == 0)
        label = (rva >= kSetterLoRva && rva < kSetterHiRva) ? "the setter's own store" : "game code";
    callersText(c, chain, sizeof(chain));
    const float value = *reinterpret_cast<const volatile float*>(g_addr.load(std::memory_order_acquire));
    Log::get().note("vr ssaa gate: write watch hit %u: writer %s (the instruction after the write); value %.4f; thread %lu; frame %u; "
                    "label %s; callers (stack scan) %s",
                    hit, writer, static_cast<double>(value), static_cast<unsigned long>(GetCurrentThreadId()), vrSsaaGateFrame(),
                    label, chain[0] ? chain : "none in the game image");
}

LONG CALLBACK watchHandler(EXCEPTION_POINTERS* ep) {
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP) return EXCEPTION_CONTINUE_SEARCH;
    CONTEXT* c = ep->ContextRecord;
    if (!g_armed.load(std::memory_order_acquire) || !(c->Dr6 & 0x1ull)) return EXCEPTION_CONTINUE_SEARCH;  // not ours
    c->Dr6 &= ~0xFull;
    const uint32_t hit = g_hits.fetch_add(1, std::memory_order_relaxed) + 1;
    if (hit <= kHitCap) logHit(c, hit);
    if (hit >= kHitCap) g_disarmWanted.store(true, std::memory_order_release);
    return EXCEPTION_CONTINUE_EXECUTION;
}

DWORD WINAPI watchWorker(LPVOID) {
    const uint32_t kind = g_pendingKind.exchange(0, std::memory_order_acq_rel);
    uint32_t failed = 0;
    if (kind == 1) {
        const uintptr_t addr = g_addr.load(std::memory_order_acquire);
        const uint32_t n = setDebugRegistersAll(true, addr, &failed);
        if (n == 0) {
            g_armed.store(false, std::memory_order_release);
            Log::get().note("vr ssaa gate: write watch not armed on ctx+0x3564 0x%llX: no thread took a debug register (%u could "
                            "not be stopped or set)",
                            static_cast<unsigned long long>(addr), failed);
        } else {
            Log::get().note("vr ssaa gate: write watch armed on ctx+0x3564 0x%llX: %u thread(s) set, %u could not be stopped or set; "
                            "16 writes at most, then disarmed; threads created after this have no debug register",
                            static_cast<unsigned long long>(addr), n, failed);
        }
    } else if (kind == 2) {
        const uint32_t hits = g_hits.load(std::memory_order_acquire);
        g_armed.store(false, std::memory_order_release);
        const uint32_t n = setDebugRegistersAll(false, 0, &failed);
        Log::get().note("vr ssaa gate: write watch disarmed (%s): %u write(s) seen; %u thread(s) cleared, %u could not be",
                        g_disarmWhy, hits, n, failed);
    }
    return 0;
}

bool startWorker(uint32_t kind) {
    g_pendingKind.store(kind, std::memory_order_release);
    HANDLE t = CreateThread(nullptr, 0, watchWorker, nullptr, 0, nullptr);
    if (!t) {
        g_pendingKind.store(0, std::memory_order_release);
        Log::get().note("vr ssaa gate: write watch: CreateThread failed (%lu)", GetLastError());
        return false;
    }
    CloseHandle(t);
    return true;
}

}  // namespace

void vrContextWatchArm(uintptr_t fieldAddress) {
    bool expected = false;
    if (!g_tried.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) return;
    if (!fieldAddress || (fieldAddress & 3u)) {
        Log::get().note("vr ssaa gate: write watch not armed: the field 0x%llX is not a 4-byte aligned address",
                        static_cast<unsigned long long>(fieldAddress));
        return;
    }
    g_addr.store(fieldAddress, std::memory_order_release);
    if (!g_veh) g_veh = AddVectoredExceptionHandler(1, watchHandler);
    if (!g_veh) {
        Log::get().note("vr ssaa gate: write watch not armed: AddVectoredExceptionHandler failed (%lu)", GetLastError());
        return;
    }
    g_armed.store(true, std::memory_order_release);  // the handler listens from here; the worker clears it if no thread took a register
    if (!startWorker(1)) g_armed.store(false, std::memory_order_release);
}

void vrContextWatchFrame() {
    if (!g_disarmWanted.load(std::memory_order_acquire)) return;
    if (!g_disarmWanted.exchange(false, std::memory_order_acq_rel)) return;
    std::snprintf(g_disarmWhy, sizeof(g_disarmWhy), "%u writes logged, the cap", kHitCap);
    startWorker(2);
}

void vrContextWatchShutdown() {
    if (!g_armed.exchange(false, std::memory_order_acq_rel)) {
        if (g_veh) {
            RemoveVectoredExceptionHandler(g_veh);
            g_veh = nullptr;
        }
        return;
    }
    std::snprintf(g_disarmWhy, sizeof(g_disarmWhy), "shutdown");
    uint32_t failed = 0;
    const uint32_t n = setDebugRegistersAll(false, 0, &failed);
    Log::get().note("vr ssaa gate: write watch disarmed (shutdown): %u write(s) seen; %u thread(s) cleared, %u could not be",
                    g_hits.load(std::memory_order_acquire), n, failed);
    if (g_veh) {
        RemoveVectoredExceptionHandler(g_veh);
        g_veh = nullptr;
    }
}

}  // namespace edvr
