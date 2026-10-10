// The window-mode trampolines (vr_window_trampolines.h says what and why). Observe only.
#include "vr_window_trampolines.h"

#include "vr_display_observer.h"  // vrDisplayGameWindow: the game's window, to compare with the foreground
#include "vr_ssaa_gate.h"         // vrSsaaGateFrame: the frame number
#include "vr_ssaa_hold.h"         // vrSsaaHoldLastMode: the StereoscopicMode the hold last read

#include "../common/code_hook.h"
#include "../common/log.h"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace edvr {
namespace {

constexpr uint32_t kRequestRva = 0x7E9D50;  // the mode request
constexpr uint32_t kApplyRva = 0x5589B0;    // the window apply
// The first eleven bytes of each, read from build 332841 (the build gate is the PE stamp; these pin the function itself).
constexpr uint8_t kRequestBytes[11] = {0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x10, 0x48, 0x89, 0x70, 0x18};
constexpr uint8_t kApplyBytes[11] = {0x40, 0x53, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x57};
constexpr uint32_t kCap = 64;

using Fn8 = uint64_t (*)(void*, void*, void*, void*, void*, void*, void*, void*);
Fn8 g_requestOrig = nullptr;
Fn8 g_applyOrig = nullptr;
CodeHook g_requestHook;
CodeHook g_applyHook;
std::atomic<uint32_t> g_requestLines{0};
std::atomic<uint32_t> g_applyLines{0};

// Guarded reads: a fault in the game's memory must not become the game's crash. No objects inside the guards.
bool readU32(uintptr_t at, uint32_t* out) {
    __try {
        *out = *reinterpret_cast<const volatile uint32_t*>(at);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool readU64(uintptr_t at, uint64_t* out) {
    __try {
        *out = *reinterpret_cast<const volatile uint64_t*>(at);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool readGuardedBytes(const uint8_t* at, uint8_t* out, size_t n) {
    __try {
        for (size_t i = 0; i < n; ++i) out[i] = at[i];
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void modeText(char* out, size_t n) {
    bool known = false;
    const int mode = vrSsaaHoldLastMode(&known);
    if (known) std::snprintf(out, n, "%d", mode);
    else std::snprintf(out, n, "unknown");
}

const char* foregroundText() {
    const HWND game = static_cast<HWND>(vrDisplayGameWindow());
    if (!game) return "no game window yet";
    return GetForegroundWindow() == game ? "yes" : "no";
}

void observeRequest(void* window, void* request) {
    if (g_requestLines.fetch_add(1, std::memory_order_relaxed) >= kCap) return;
    uint32_t mode = 0, kind = 0;
    const bool haveMode = readU32(reinterpret_cast<uintptr_t>(request), &mode);
    const bool haveKind = readU32(reinterpret_cast<uintptr_t>(request) + 0x20, &kind);
    char stereo[16];
    modeText(stereo, sizeof(stereo));
    Log::get().note("vr window: mode request (0x7E9D50): window %p, mode index %s, kind %s; StereoscopicMode %s; game window "
                    "foreground %s; frame %u",
                    window, haveMode ? std::to_string(mode).c_str() : "unreadable",
                    haveKind ? std::to_string(kind).c_str() : "unreadable", stereo, foregroundText(), vrSsaaGateFrame());
}

void observeApply(void* window, void* state) {
    if (g_applyLines.fetch_add(1, std::memory_order_relaxed) >= kCap) return;
    const uintptr_t s = reinterpret_cast<uintptr_t>(state);
    uint32_t kind = 0, w = 0, h = 0;
    uint64_t monitor = 0;
    const bool haveKind = readU32(s + 0x20, &kind);
    const bool haveW = readU32(s + 0x18, &w);
    const bool haveH = readU32(s + 0x1C, &h);
    const bool haveMonitor = readU64(s + 0x28, &monitor);
    char stereo[16];
    modeText(stereo, sizeof(stereo));
    Log::get().note("vr window: apply (0x5589B0): window %p, kind %s, client %s x %s, monitor entry %s; StereoscopicMode %s; game "
                    "window foreground %s; frame %u",
                    window, haveKind ? std::to_string(kind).c_str() : "unreadable", haveW ? std::to_string(w).c_str() : "unreadable",
                    haveH ? std::to_string(h).c_str() : "unreadable",
                    haveMonitor ? (std::string("0x") + std::to_string(monitor)).c_str() : "unreadable", stereo, foregroundText(),
                    vrSsaaGateFrame());
}

// The replacements take all eight integer argument slots, so the original receives exactly what the caller passed: the first four in
// registers and the rest from the caller's stack, as the calling convention lays them out. Nothing is changed.
uint64_t requestReplacement(void* a, void* b, void* c, void* d, void* e, void* f, void* g, void* h) {
    observeRequest(a, b);
    return g_requestOrig(a, b, c, d, e, f, g, h);
}

uint64_t applyReplacement(void* a, void* b, void* c, void* d, void* e, void* f, void* g, void* h) {
    observeApply(a, b);
    return g_applyOrig(a, b, c, d, e, f, g, h);
}

// A relay within +/-2 GB of the target: the five-byte jump CodeHook writes can reach it, and the relay jumps to the replacement, which
// may be anywhere (kinematic_eval_hook.cpp's allocateRelay, copied rather than shared, as that file does with its writer hook).
// Layout: jmp qword ptr [rip+0] (FF 25 00 00 00 00), then the replacement's address. Registers and the stack pass through unchanged.
constexpr size_t kRelayBytes = 14;
uint8_t* allocateRelayNear(uintptr_t target) {
    SYSTEM_INFO info{};
    GetSystemInfo(&info);
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
                auto* p = static_cast<uint8_t*>(VirtualAlloc(reinterpret_cast<void*>(candidate), 4096, MEM_RESERVE | MEM_COMMIT,
                                                             PAGE_READWRITE));
                if (p) return p;
            }
        }
        if (end <= at) break;
        at = end;
    }
    return nullptr;
}

uint8_t* buildRelayNear(uintptr_t target, void* replacement) {
    uint8_t* relay = allocateRelayNear(target);
    if (!relay) return nullptr;
    const uint8_t body[kRelayBytes] = {0xFF, 0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    std::memcpy(relay, body, sizeof(body));
    const uintptr_t to = reinterpret_cast<uintptr_t>(replacement);
    std::memcpy(relay + 6, &to, 8);
    DWORD old = 0;
    if (!VirtualProtect(relay, 4096, PAGE_EXECUTE_READ, &old) || !FlushInstructionCache(GetCurrentProcess(), relay, kRelayBytes)) {
        VirtualFree(relay, 0, MEM_RELEASE);
        return nullptr;
    }
    return relay;  // kept for the process lifetime: the installed patch jumps here
}

// Pins the function's first bytes, then installs the entry through a near relay. False (and no patch) when the bytes differ, no
// relay can be placed, or CodeHook refuses.
bool installOne(const uint8_t* base, uint32_t rva, const uint8_t* want, size_t len, void* replacement, void** origOut,
                CodeHook* hook, const char* name) {
    uint8_t got[11] = {};
    if (!readGuardedBytes(base + rva, got, len) || std::memcmp(got, want, len) != 0) {
        Log::get().note("vr window: trampoline %s refused: the bytes at 0x%X are not build 332841's; nothing patched", name, rva);
        return false;
    }
    const uintptr_t target = reinterpret_cast<uintptr_t>(base) + rva;
    uint8_t* relay = buildRelayNear(target, replacement);
    if (!relay) {
        Log::get().note("vr window: trampoline %s refused: no page within +/-2 GB of the target for its relay; nothing patched", name);
        return false;
    }
    if (!hook->install(reinterpret_cast<void*>(target), relay, origOut, name)) {
        Log::get().note("vr window: trampoline %s refused by CodeHook (the prologue cannot be relocated; the reason is in the "
                        "line above); nothing patched", name);
        return false;
    }
    return true;
}

}  // namespace

void vrWindowTrampolinesInstall() {
    static bool done = false;
    if (done) return;
    done = true;
    const uint8_t* base = reinterpret_cast<const uint8_t*>(GetModuleHandleW(nullptr));
    if (!base) {
        Log::get().note("vr window: trampolines off: the game module could not be found");
        return;
    }
    const bool req = installOne(base, kRequestRva, kRequestBytes, sizeof(kRequestBytes), reinterpret_cast<void*>(&requestReplacement),
                                reinterpret_cast<void**>(&g_requestOrig), &g_requestHook, "0x7E9D50 (mode request)");
    const bool app = installOne(base, kApplyRva, kApplyBytes, sizeof(kApplyBytes), reinterpret_cast<void*>(&applyReplacement),
                                reinterpret_cast<void**>(&g_applyOrig), &g_applyHook, "0x5589B0 (window apply)");
    Log::get().note("vr window: trampolines: 0x7E9D50 (mode request) %s; 0x5589B0 (window apply) %s; observe only",
                    req ? "installed" : "not installed", app ? "installed" : "not installed");
}

}  // namespace edvr
