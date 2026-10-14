// The admission store's detour (store_hook.h). A CodeHook on FUN_144312040, pinned by its sixteen bytes in build 332841
// (concourse-culling doc, P11). The prologue's first five bytes are three pushes and a two-byte push, which the CodeHook
// decoder steals (push rbp, push rbx, push rsi, push rdi: 2+1+1+1). The observer takes the store's five arguments:
// four in registers and the record on the stack, which the relay leaves where the store's caller put it. The observer
// returns whatever the original returns (RAX), so the caller sees the same value with or without the detour.
#include "store_hook.h"

#include <windows.h>

#include <atomic>
#include <cstring>

#include "../common/code_hook.h"
#include "../common/log.h"
#include "explorer_cam_core.h"
#include "optimized_station.h"
#include "optimized_station_core.h"

namespace edvr {
namespace {

constexpr uintptr_t kStoreRva = 0x4312040u;
constexpr size_t kRelayBytes = 44, kOriginalLiteral = 36;

using StoreFn = uintptr_t(__fastcall*)(uintptr_t bucket, uintptr_t mask, uintptr_t out, uintptr_t lod, uintptr_t record);

__declspec(noinline) uintptr_t __fastcall storeObserved(uintptr_t bucket, uintptr_t mask, uintptr_t out, uintptr_t lod,
                                                        uintptr_t record) noexcept;

alignas(8) std::atomic<uintptr_t> g_storeGate{0};
static_assert(decltype(g_storeGate)::is_always_lock_free, "The relay reads the aligned atomic cell directly.");

struct Detour {
    std::atomic<uintptr_t> forward{0};
    CodeHook hook;
    uint8_t* relay = nullptr;
    std::atomic<bool> ready{false};
    std::atomic<bool> refused{false};   // built v14.18: a refusal is final (the build does not change), so it is logged once
    std::atomic<const char*> status{"not wanted yet"};
};

Detour g_store;

uint8_t* allocateRelay(uintptr_t target) noexcept {
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
                auto* p = static_cast<uint8_t*>(VirtualAlloc(reinterpret_cast<void*>(candidate), 4096,
                                                             MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
                if (p) return p;
            }
        }
        if (end <= at) break;
        at = end;
    }
    return nullptr;
}

void buildRelay(uint8_t* code, const void* gate, void* callback) noexcept {
    // mov rax,&gate; cmp qword ptr[rax],0; je original; jmp [callback]; original: jmp [trampoline].
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

bool prepareRelay(void* trampoline, void* context) noexcept {
    auto& d = *static_cast<Detour*>(context);
    const uintptr_t address = reinterpret_cast<uintptr_t>(trampoline);
    std::memcpy(d.relay + kOriginalLiteral, &address, 8);
    DWORD oldProtect = 0;
    if (!VirtualProtect(d.relay, 4096, PAGE_EXECUTE_READ, &oldProtect) ||
        !FlushInstructionCache(GetCurrentProcess(), d.relay, kRelayBytes))
        return false;
    d.forward.store(address, std::memory_order_release);
    return true;
}

__declspec(noinline) uintptr_t __fastcall storeObserved(uintptr_t bucket, uintptr_t mask, uintptr_t out, uintptr_t lod,
                                                        uintptr_t record) noexcept {
    const auto forward = reinterpret_cast<StoreFn>(g_store.forward.load(std::memory_order_acquire));
    if (!forward) return 0;
    const uintptr_t result = forward(bucket, mask, out, lod, record);
    optimizedStationAfterStore(bucket, record);
    return result;
}

// The PE identity the other build-keyed hooks read (kinematic_eval_hook.cpp targetValid): base+0x3C -> e_lfanew, then the TimeDateStamp at +8
// and the SizeOfImage at +0x50. Under __try: an unreadable header is a refusal, not a crash.
bool readIdentity(uintptr_t base, uint32_t* stamp, uint32_t* size) noexcept {
    __try {
        uint32_t peOff = 0;
        std::memcpy(&peOff, reinterpret_cast<const void*>(base + 0x3C), 4);
        if (peOff > 0x1000) return false;
        std::memcpy(stamp, reinterpret_cast<const void*>(base + peOff + 8), 4);
        std::memcpy(size, reinterpret_cast<const void*>(base + peOff + 0x50), 4);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Installs the detour once: the PE identity must be build 332841's, the prologue must match the pin, and the CodeHook must accept it.
// A refusal is final and logged once; with the detour refused nothing is written to the game (the observer that writes is never reached).
void installOnce() noexcept {
    if (g_store.ready.load(std::memory_order_acquire) || g_store.refused.load(std::memory_order_acquire)) return;
    const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    if (!base) {
        g_store.status.store("the module base is unknown", std::memory_order_release);
        return;
    }
    uint32_t stamp = 0;
    uint32_t size = 0;
    const bool identityRead = readIdentity(base, &stamp, &size);
    uint8_t got[sizeof(sealed::kStorePrologue)] = {};
    bool readOk = false;
    __try {
        std::memcpy(got, reinterpret_cast<const void*>(base + kStoreRva), sizeof(got));
        readOk = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        readOk = false;
    }
    const sealed::StoreInstall verdict = sealed::storeInstallDecision(identityRead, stamp, size, ecm::kExpectedTimestamp, ecm::kExpectedImageSize,
                                                                     readOk ? got : nullptr, sealed::kStorePrologue, sizeof(got));
    if (verdict != sealed::StoreInstall::kInstall) {
        g_store.refused.store(true, std::memory_order_release);
        if (verdict == sealed::StoreInstall::kWrongIdentity) {
            g_store.status.store("not build 332841 (PE timestamp or image size)", std::memory_order_release);
            Log::get().note("store hook: refused: not build 332841 (PE timestamp %u, image size %u); the optimized station stands down.",
                            static_cast<unsigned>(stamp), static_cast<unsigned>(size));
        } else if (verdict == sealed::StoreInstall::kUnreadable) {
            g_store.status.store("the executable's PE header or the store's prologue is unreadable", std::memory_order_release);
            Log::get().note("store hook: refused: the PE header or the admission store's prologue is unreadable; "
                            "the optimized station stands down.");
        } else {
            g_store.status.store("the store's prologue does not match build 332841", std::memory_order_release);
            Log::get().note("store hook: refused: the admission store's sixteen prologue bytes are not build 332841's; "
                            "the optimized station stands down.");
        }
        return;
    }
    g_store.relay = allocateRelay(base + kStoreRva);
    if (!g_store.relay) {
        g_store.status.store("no relay within range", std::memory_order_release);
        return;
    }
    buildRelay(g_store.relay, &g_storeGate, reinterpret_cast<void*>(&storeObserved));
    if (!g_store.hook.install(reinterpret_cast<void*>(base + kStoreRva), g_store.relay, nullptr,
                              "admission-store", &prepareRelay, &g_store)) {
        VirtualFree(g_store.relay, 0, MEM_RELEASE);
        g_store.relay = nullptr;
        g_store.status.store("CodeHook refused the store (its own line says why)", std::memory_order_release);
        return;
    }
    g_store.ready.store(true, std::memory_order_release);
    g_store.status.store("installed", std::memory_order_release);
    Log::get().note("store hook: the admission store (0x4312040) is detoured; the gate is closed until the sealed trial or "
                    "the look probe wants it.");
}

}  // namespace

void storeHookRecompute() noexcept {
    if (optimizedStationOn()) installOnce();
    storeHookGate();
}

// Built v14.20 (review 7b): the observer runs only while the station gathers (setting on, docked) and the detour is installed. Closing the gate
// only stops new calls; a call already inside takes the station's state lock, which the toggle and the release take too.
void storeHookGate() noexcept {
    const bool open = optimizedStationGathers() && g_store.ready.load(std::memory_order_acquire);
    g_storeGate.store(open ? 1u : 0u, std::memory_order_release);
}

const char* storeHookStatus() noexcept { return g_store.status.load(std::memory_order_acquire); }

}  // namespace edvr
