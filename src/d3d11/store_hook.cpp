// The admission store's detour (store_hook.h). A CodeHook on FUN_144314C60, pinned by its sixteen bytes in build 334925
// (concourse-culling doc, P11); on another build, found by its whole-function signature (src\common\game_build.h, issue
// 124's pilot). The prologue's first five bytes are three pushes and a two-byte push, which the CodeHook
// decoder steals (push rbp, push rbx, push rsi, push rdi: 2+1+1+1). The observer takes the store's five arguments:
// four in registers and the record on the stack, which the relay leaves where the store's caller put it. The observer
// returns whatever the original returns (RAX), so the caller sees the same value with or without the detour.
#include "flat_origin_capture.h"
#include "store_hook.h"

#include <windows.h>

#include <atomic>
#include <cstring>
#include <mutex>

#include "../common/code_hook.h"
#include "../common/game_build.h"
#include "../common/log.h"
#include "flat_world_store.h"
#include "flat_world_witness.h"
#include "optimized_station.h"
#include "optimized_station_core.h"

namespace edvr {
namespace {

constexpr uintptr_t kStoreRva = 0x4314C60u;
constexpr size_t kRelayBytes = 44, kOriginalLiteral = 36;

using StoreFn = uintptr_t(__fastcall*)(uintptr_t bucket, uintptr_t mask, uintptr_t out, uintptr_t lod, uintptr_t record);

__declspec(noinline) uintptr_t __fastcall storeObserved(uintptr_t bucket, uintptr_t mask, uintptr_t out, uintptr_t lod,
                                                        uintptr_t record) noexcept;

alignas(8) std::atomic<uintptr_t> g_storeGate{0};
static_assert(decltype(g_storeGate)::is_always_lock_free, "The relay reads the aligned atomic cell directly.");
std::atomic<bool> g_stationGathering{false}, g_flatWorldActive{false};
std::atomic<uint32_t> g_flatWorldFrame{0};

struct Detour {
    std::atomic<uintptr_t> forward{0};
    CodeHook hook;
    uint8_t* relay = nullptr;
    std::atomic<bool> ready{false};
    std::atomic<bool> refused{false};   // built v14.18: a refusal is final (the build does not change), so it is logged once
    std::atomic<const char*> status{"not wanted yet"};
};

Detour g_store;
std::mutex g_storeInstallMutex; // Present and the station setting can request first installation concurrently.

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
    const bool station = g_stationGathering.load(std::memory_order_acquire);
    return flatWorldStoreStampedPostForward(bucket, mask, out, lod, record, station,
        g_flatWorldActive, g_flatWorldFrame, forward,
        [](uintptr_t b,uintptr_t r) noexcept { optimizedStationAfterStore(b,r); },
        [](uintptr_t b,uintptr_t r,uint32_t frame) noexcept {
            // Select charges and deduplicates before the first extra read.
            if (!flatWorldWitnessSelect(r, frame)) return;
            FlatWorldWitnessInput input{};
            const uint8_t failure = flatWorldStoreRead(b, r, frame, &input);
            if (failure) flatWorldWitnessReadFailure(frame, failure);
            else flatWorldWitnessObserve(input);
        });
}

// The store's sixteen prologue bytes, read under __try: false when the page is unreadable.
bool readPrologue(uintptr_t at, uint8_t* got, size_t n) noexcept {
    __try {
        std::memcpy(got, reinterpret_cast<const void*>(at), n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Installs the detour once. Build 334925 (the resolver's known build): the store is at kStoreRva and its prologue must be the pin, exactly,
// as it always was. An unknown build (issue 124's pilot): the store's address comes from the signature resolver (src\common\game_build.h),
// which found its whole function by masked match or did not; the prologue is then compared with the bytes the signature masks skipped. Then
// the CodeHook must accept it. A refusal is final and logged once; with the detour refused nothing is written to the game (the observer that
// writes is never reached).
void installLocked() noexcept {
    const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    if (!base) {
        g_store.status.store("the module base is unknown", std::memory_order_release);
        return;
    }
    const bool identityRead = gameBuildIdentity(nullptr, nullptr);
    const bool knownBuild = identityRead && gameBuildKnown();
    const uintptr_t storeRva = identityRead ? gameRva(static_cast<uint32_t>(kStoreRva)) : 0;
    uint8_t got[sizeof(sealed::kStorePrologue)] = {};
    const bool readOk = storeRva != 0 && readPrologue(base + storeRva, got, sizeof(got));
    const bool pinOk = readOk && gamePinMatches(static_cast<uint32_t>(kStoreRva), got, sealed::kStorePrologue, sizeof(got));
    const sealed::StoreInstall verdict = sealed::storeInstallDecision(identityRead, storeRva, readOk ? got : nullptr, pinOk);
    if (verdict != sealed::StoreInstall::kInstall) {
        g_store.refused.store(true, std::memory_order_release);
        if (verdict == sealed::StoreInstall::kNotFound) {
            g_store.status.store("an unknown build, and the store was not found by signature", std::memory_order_release);
            Log::get().note("store hook: refused: %s, and the admission store (0x4314C60 in build 334925) was not found by signature; "
                            "optimized station and flat world witness stand down.", gameBuildText());
        } else if (verdict == sealed::StoreInstall::kUnreadable) {
            g_store.status.store("the executable's PE header or the store's prologue is unreadable", std::memory_order_release);
            Log::get().note("store hook: refused: the PE header or the admission store's prologue is unreadable; "
                            "optimized station and flat world witness stand down.");
        } else if (knownBuild) {
            g_store.status.store("the store's prologue does not match build 334925", std::memory_order_release);
            Log::get().note("store hook: refused: the admission store's sixteen prologue bytes are not build 334925's; "
                            "optimized station and flat world witness stand down.");
        } else {
            g_store.status.store("found by signature, but the store's prologue does not match build 334925's", std::memory_order_release);
            Log::get().note("store hook: refused: %s: the admission store was found by signature at 0x%llX, but its sixteen prologue bytes "
                            "are not build 334925's outside the masked fields; optimized station and flat world witness stand down.",
                            gameBuildText(), static_cast<unsigned long long>(storeRva));
        }
        return;
    }
    // The field witnesses (issue 124, docs\design-game-update-signatures-2026-10-25.md, 2026-10-25 round 2): the store's own instructions
    // that carry, unmasked, the record fields optimized station writes back (optimized_station.cpp kRecMask, kRecAdmitted). The record is
    // the store's fifth argument (mov rdi,[rbp+0x1F0], 0x4314C89); it reads the record's node at +0x18 (0x4314CBD) and ends by storing the
    // admitted byte at +0x234 (0x4315227) and the view mask at +0x208 (0x431523D). On an unknown build each must be found, or the store hook
    // stands down with everything that rides it.
    static constexpr uint32_t kRecordWitnessRvas[4] = {0x4314C89, 0x4314CBD, 0x4315227, 0x431523D};
    uint32_t missingWitness = 0;
    if (!gameRvasFound(kRecordWitnessRvas, 4, &missingWitness)) {
        g_store.refused.store(true, std::memory_order_release);
        g_store.status.store("an unknown build, and a record field witness was not found by signature", std::memory_order_release);
        Log::get().note("store hook: refused: %s, and the record field witness 0x%X (in build 334925) was not found by signature, so the "
                        "record fields optimized station writes are not proven on this build; optimized station and flat world witness "
                        "stand down.", gameBuildText(), missingWitness);
        return;
    }
    g_store.relay = allocateRelay(base + storeRva);
    if (!g_store.relay) {
        g_store.status.store("no relay within range", std::memory_order_release);
        return;
    }
    buildRelay(g_store.relay, &g_storeGate, reinterpret_cast<void*>(&storeObserved));
    if (!g_store.hook.install(reinterpret_cast<void*>(base + storeRva), g_store.relay, nullptr,
                              "admission-store", &prepareRelay, &g_store)) {
        VirtualFree(g_store.relay, 0, MEM_RELEASE);
        g_store.relay = nullptr;
        g_store.status.store("CodeHook refused the store (its own line says why)", std::memory_order_release);
        return;
    }
    g_store.ready.store(true, std::memory_order_release);
    g_store.status.store(knownBuild ? "installed" : "installed (found by signature on an unknown build)", std::memory_order_release);
    if (knownBuild)
        Log::get().note("store hook: the admission store (0x4314C60) is detoured; the gate opens for station gathering or the passive flat world witness.");
    else
        Log::get().note("store hook: the admission store (0x4314C60 in build 334925) was found by signature on %s at 0x%llX and is detoured; "
                        "the gate opens for station gathering or the passive flat world witness.",
                        gameBuildText(), static_cast<unsigned long long>(storeRva));
}

void installOnce() noexcept {
    flatWorldStoreInstallGuard(g_storeInstallMutex,g_store.ready,g_store.refused,&installLocked);
}

}  // namespace

void storeHookRecompute() noexcept {
    if (optimizedStationOn() || g_flatWorldActive.load(std::memory_order_acquire)) installOnce();
    storeHookGate();
}

void storeHookGate() noexcept {
    const bool station = optimizedStationGathers();
    g_stationGathering.store(station, std::memory_order_release);
    const bool open = flatWorldStoreGateWanted(station,g_flatWorldActive.load(std::memory_order_acquire),
                                              g_store.ready.load(std::memory_order_acquire));
    g_storeGate.store(open ? 1u : 0u, std::memory_order_release);
}

void storeHookSetFlatWorldProbe(bool active, uint32_t frame) noexcept {
    active = active && flatOriginCaptureEnabled();
    // A transient refusal may be retried by the station's explicit
    // recompute or the next flat activation, never on every Present.
    if (flatWorldStorePublish(active,frame,g_flatWorldActive,g_flatWorldFrame)) {
        installOnce();
        Log::get().note("store hook: flat world witness activation frame=%u ready=%u status=%s",
            frame,g_store.ready.load(std::memory_order_acquire)?1u:0u,g_store.status.load(std::memory_order_acquire));
    }
    storeHookGate();
}

bool storeHookReady() noexcept { return g_store.ready.load(std::memory_order_acquire); }
bool storeHookFlatWorldProbeGateOpen() noexcept {
    return g_flatWorldActive.load(std::memory_order_acquire) && storeHookReady() &&
        g_storeGate.load(std::memory_order_acquire) != 0;
}

const char* storeHookStatus() noexcept { return g_store.status.load(std::memory_order_acquire); }

}  // namespace edvr
