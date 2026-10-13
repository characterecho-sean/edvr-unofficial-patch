#include "terrain_cull_cover_hook.h"
#include "terrain_cull_cover_core.h"
#include "../common/code_hook.h"
#include "../common/log.h"
#include "../common/runtime_profile.h"  // runtimeVrProfile: the per-call gate in coverDetour
#include "cross_profile_cost.h"         // coverDetour's cost while the flat profile runs (log only)

#include <windows.h>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

// The terrain cull cover (terrain_cull_cover_core.h says what is wrong and why this is the repair). This file is the glue: find the cull-plane builder, patch its entry with the
// project's CodeHook through a relay, and in the detour widen arguments 5 and 6 (vFOV and aspect, the two floats on the stack) before the original runs.

namespace edvr {
namespace {

// --- Relay machinery, the same copy the other engine hooks carry (grep kRelayBytes): the target is in the game's module and this DLL loads more than two gigabytes away, so a
// five-byte E9 cannot reach a replacement here directly. Kept as a copy rather than a shared unit so none of the flight-proven files is touched.
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
    // mov rax,&gate; cmp qword ptr[rax],0; je original; jmp [callback]; original: jmp [trampoline]. The builder's first instruction is mov rax,rsp, which overwrites RAX, and
    // it takes nothing in RAX or the flags; no stack adjustment, no nonvolatile register touched, so the detour sees the arguments exactly as the caller left them.
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
std::atomic<uintptr_t> g_relayGate{0};   // 0: the relay forwards straight to the original (never installed)
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
std::atomic<bool> g_attempted{false};   // install has been tried (once, ever)
std::atomic<bool> g_installed{false};
std::atomic<bool> g_byStamp{false};
std::atomic<uint64_t> g_tangents{0};    // tcover::packTangents(T, H), 0 until a frame says
std::atomic<uint64_t> g_calls{0}, g_widened{0};   // the running heartbeat window's
std::atomic<bool> g_firstCallSaid{false};
std::atomic<uint32_t> g_last[4]{};      // the last call's in aspect, in vfov, out aspect, out vfov (float bits)
std::atomic<bool> g_haveLast{false};

// The poll thread's own.
bool g_pollStarted = false;
uint64_t g_windowStartMs = 0;

#ifdef EDVR_TERRAIN_COVER_TEST
const uint8_t* g_testBase = nullptr;
std::mutex g_testMutex;
std::vector<std::string> g_testLines;
#endif

// The one place a line is written. The log, and in the rig a copy.
void say(const char* line) {
    Log::get().note("%s", line);
#ifdef EDVR_TERRAIN_COVER_TEST
    std::lock_guard<std::mutex> lock(g_testMutex);
    g_testLines.emplace_back(line);
#endif
}

float bitsToFloat(uint32_t bits) { float f; std::memcpy(&f, &bits, 4); return f; }
uint32_t floatToBits(float f) { uint32_t b; std::memcpy(&b, &f, 4); return b; }

// The cull-plane builder: (a, b, c, d) = rcx, rdx, r8, r9 (four pointers: the planes out, the eye, the scratch), then the two floats on the stack. Only the last two are touched.
using BuilderFn = uintptr_t (__fastcall*)(uintptr_t, uintptr_t, uintptr_t, uintptr_t, float, float);
__declspec(noinline) uintptr_t __fastcall coverDetour(uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d, float vfov, float aspect) noexcept {
    const auto forward = reinterpret_cast<BuilderFn>(g_forward.load(std::memory_order_acquire));
    if (!forward) return 0;   // never installed; the relay is unreachable then
    // The flat profile has no stereo frustum: it gets the builder's own call, with the arguments as the game made them.
    if (!runtimeVrProfile()) {
        // Inactive: timed from entry to the builder's own call (cross_profile_cost.h).
        cross_profile::enterSite(cross_profile::Site::VrCoverDetour, true).stop();
        return forward(a, b, c, d, vfov, aspect);
    }
    float tanV = 0.0f, tanH = 0.0f;
    tcover::unpackTangents(g_tangents.load(std::memory_order_relaxed), &tanV, &tanH);
    const tcover::Cover r = tcover::widen(vfov, aspect, tanV, tanH);
    g_calls.fetch_add(1, std::memory_order_relaxed);
    if (r.widened) g_widened.fetch_add(1, std::memory_order_relaxed);
    g_last[0].store(floatToBits(aspect), std::memory_order_relaxed);
    g_last[1].store(floatToBits(vfov), std::memory_order_relaxed);
    g_last[2].store(floatToBits(r.aspect), std::memory_order_relaxed);
    g_last[3].store(floatToBits(r.vfov), std::memory_order_relaxed);
    g_haveLast.store(true, std::memory_order_release);
    if (tanV > 0.0f && !g_firstCallSaid.load(std::memory_order_relaxed) && !g_firstCallSaid.exchange(true, std::memory_order_relaxed)) {
        char line[360];
        tcover::formatFirstCall(line, sizeof(line), aspect, vfov, r.aspect, r.vfov, r.widened, tanV, tanH);
        say(line);
    }
    return forward(a, b, c, d, r.vfov, r.aspect);
}

// --- SEH-guarded read of the game's module ------------------------------------------------------------------------------------------------------------------------
__declspec(noinline) bool guardedLocate(const uint8_t* base, tcover::Located* out) noexcept {
    __try {
        return tcover::locate(base, 0, out);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *out = tcover::Located{};
        std::snprintf(out->why, sizeof(out->why), "a read of the game's module faulted while looking for the cull-plane builder");
        return false;
    }
}

void sayInert(const char* why) {
    char line[520];
    tcover::formatInert(line, sizeof(line), why);
    say(line);
}

}  // namespace

void terrainCullCoverInstall(bool noVrScope) {
    bool expected = false;
    if (!g_attempted.compare_exchange_strong(expected, true)) return;   // once, ever: a refusal is final
    if (noVrScope) {
        sayInert("the install scope has no VR, so there is no stereo frustum to cover; nothing was patched");
        return;
    }
    const uint8_t* base = nullptr;
#ifdef EDVR_TERRAIN_COVER_TEST
    base = g_testBase;
#endif
    if (!base) base = reinterpret_cast<const uint8_t*>(GetModuleHandleW(nullptr));
    tcover::Located found;
    if (!guardedLocate(base, &found)) {
        sayInert(found.why[0] ? found.why : "the cull-plane builder was not found");
        return;
    }
    const uintptr_t target = reinterpret_cast<uintptr_t>(base) + found.targetRva;
    g_relay = allocateRelay(target);
    if (!g_relay) {
        sayInert("no executable memory could be placed within two gigabytes of the cull-plane builder");
        return;
    }
    buildRelay(g_relay, &g_relayGate, reinterpret_cast<void*>(&coverDetour));
    if (!g_hook.install(reinterpret_cast<void*>(target), g_relay, nullptr, "terrain-cull-cover", &prepareRelay, nullptr)) {
        VirtualFree(g_relay, 0, MEM_RELEASE);
        g_relay = nullptr;
        sayInert("CodeHook refused the cull-plane builder (its own line above, tagged terrain-cull-cover, names why)");
        return;
    }
    g_byStamp.store(found.byStamp, std::memory_order_relaxed);
    g_relayGate.store(1, std::memory_order_release);
    g_installed.store(true, std::memory_order_release);
    char line[600];
    tcover::formatLive(line, sizeof(line), found.targetRva, found.how);
    say(line);
}

void terrainCullCoverNoteFrusta(const float frusta[2][4]) {
    float tanV = 0.0f, tanH = 0.0f;
    if (!frusta || !tcover::frustaTangents(frusta, &tanV, &tanH)) return;
    g_tangents.store(tcover::packTangents(tanV, tanH), std::memory_order_relaxed);
}

void terrainCullCoverPoll(uint64_t nowMs) {
    if (!g_installed.load(std::memory_order_acquire)) return;
    if (!g_pollStarted) {   // the first tick starts the window
        g_pollStarted = true;
        g_windowStartMs = nowMs;
        return;
    }
    if (nowMs - g_windowStartMs < tcover::kHeartbeatMs) return;
    g_windowStartMs = nowMs;
    const uint64_t calls = g_calls.exchange(0, std::memory_order_relaxed);
    const uint64_t widened = g_widened.exchange(0, std::memory_order_relaxed);
    tcover::Last last;
    if (g_haveLast.load(std::memory_order_acquire)) {
        last.have = true;
        last.inAspect = bitsToFloat(g_last[0].load(std::memory_order_relaxed));
        last.inVfov = bitsToFloat(g_last[1].load(std::memory_order_relaxed));
        last.outAspect = bitsToFloat(g_last[2].load(std::memory_order_relaxed));
        last.outVfov = bitsToFloat(g_last[3].load(std::memory_order_relaxed));
    }
    char line[360];
    tcover::formatHeartbeat(line, sizeof(line), widened, calls, last);
    say(line);
}

#ifdef EDVR_TERRAIN_COVER_TEST
void terrainCullCoverTestSetImage(const uint8_t* base) { g_testBase = base; }
void terrainCullCoverTestReset() {
    g_hook.uninstall();
    if (g_relay) { VirtualFree(g_relay, 0, MEM_RELEASE); g_relay = nullptr; }
    g_relayGate.store(0);
    g_forward.store(0);
    g_installed.store(false);
    g_attempted.store(false);
    g_byStamp.store(false);
    g_tangents.store(0);
    g_calls.store(0);
    g_widened.store(0);
    g_firstCallSaid.store(false);
    for (auto& v : g_last) v.store(0);
    g_haveLast.store(false);
    g_pollStarted = false;
    g_windowStartMs = 0;
    std::lock_guard<std::mutex> lock(g_testMutex);
    g_testLines.clear();
}
TerrainCoverTestState terrainCullCoverTestState() {
    TerrainCoverTestState s;
    s.attempted = g_attempted.load();
    s.installed = g_installed.load();
    s.byStamp = g_byStamp.load();
    s.firstCallSaid = g_firstCallSaid.load();
    s.calls = g_calls.load();
    s.widened = g_widened.load();
    return s;
}
size_t terrainCullCoverTestLineCount() {
    std::lock_guard<std::mutex> lock(g_testMutex);
    return g_testLines.size();
}
const char* terrainCullCoverTestLine(size_t i) {
    std::lock_guard<std::mutex> lock(g_testMutex);
    return i < g_testLines.size() ? g_testLines[i].c_str() : "";
}
#endif

}  // namespace edvr
