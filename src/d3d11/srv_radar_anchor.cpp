// The SRV radar's anchor moved in the game (issue 140; srv_radar_anchor.h says what and why). Exact-build only (tools\hook_signatures.py AUDIT_EXACT_ONLY): the addresses below are
// build 334925's (analysis\decomp\verify_20261031_srv_radar_anchor.txt), checked against the running image's PE identity and the function's first sixteen bytes before anything is
// patched; any other build stands down.
//
// THE CALLBACK MUST NOT TOUCH XMM1. The update takes the object in RCX and dt in XMM1, and the callback is entered by a jump from the relay, so the game's XMM1 is still live when it
// calls the original. It is declared with four INTEGER parameters, does integer work only before that call (no float arithmetic, no memcpy, no call into anything, no logging: the
// offset arrives as precomputed bit patterns and is written with volatile 32-bit stores), and the call itself passes the four integer registers on untouched.
#include "srv_radar_anchor.h"

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>

#include "../common/code_hook.h"
#include "../common/game_build.h"
#include "../common/log.h"
#include "avatar_hooks_core.h"

namespace edvr {
namespace {

constexpr uintptr_t kUpdateRva = 0x384A590;            // the radar terrain schematic's per-frame update; reached only through the thunk 0x381EF00 (rcx = rdx - 0xB0, xmm1 = xmm2)
constexpr uintptr_t kObjectVtableRva = 0x5511DE8;      // *(obj + 0x00), written by its constructor 0x383DF40
constexpr uintptr_t kInterfaceVtableRva = 0x5511F18;   // *(obj + 0x58), the interface the Scorpion icon's schematic reads the offset through
constexpr uint32_t kStamp = 0x6ADA8048u, kImageSize = 0x640C000u;   // build 334925's PE TimeDateStamp and SizeOfImage
constexpr uintptr_t kInterfaceAt = 0x58, kTranslationA = 0x15B0, kTranslationB = 0x15F0;   // the two local translation rows (x, y, z, w), lerped by 0x385DC20
constexpr size_t kPrologueBytes = 16;
// `push rbx; push rsi; sub rsp,168h; movaps [rsp+130h],xmm7` (the last cut at 16 bytes), read from EliteDangerous64-334925.exe through its section table. Boundaries 2, 3, 10, 18:
// CodeHook steals the first three instructions (10 bytes); the instruction at +0x12 is RIP-relative and is not reached.
constexpr uint8_t kPrologue[kPrologueBytes] = {0x40, 0x53, 0x56, 0x48, 0x81, 0xEC, 0x68, 0x01, 0x00, 0x00, 0x0F, 0x29, 0xBC, 0x24, 0x30, 0x01};
constexpr uint32_t kSlots = 4;   // schematics that may hold an offset of ours at once (one is expected: the Scorpion's)

using UpdateFn = uintptr_t(__fastcall*)(uintptr_t, uintptr_t, uintptr_t, uintptr_t);

struct State {
    CodeHook hook;
    uint8_t* relay = nullptr;
    alignas(8) std::atomic<uintptr_t> gate{0};   // the relay compares this qword with zero
    std::atomic<uintptr_t> forward{0};
    uintptr_t objectVtable = 0, interfaceVtable = 0;   // module base + the two RVAs, set before the patch
    std::atomic<int> state{int(RadarAnchorHookState::NotTried)};
    char why[480] = {};
    // published by the frame thread
    std::atomic<uint32_t> bits[3]{};
    std::atomic<uint32_t> frame{0};
    // the hook's own record: the schematics that hold an offset of ours (0 = a free slot)
    std::atomic<uintptr_t> holding[kSlots]{};
    std::atomic<uint64_t> calls{0}, matched{0}, applied{0}, appliedFrames{0}, restored{0}, faulted{0}, noSlot{0};
    std::atomic<uint32_t> lastStamp{0};
    std::atomic<uintptr_t> lastObj{0};
};
std::atomic<State*> g_state{nullptr};
std::mutex g_armMutex;

// The entry hook. Integer work only before the original runs (the header comment above): XMM1 is the game's dt.
__declspec(noinline) uintptr_t __fastcall radarUpdateHooked(uintptr_t obj, uintptr_t b, uintptr_t c, uintptr_t d) noexcept {
    State* const s = g_state.load(std::memory_order_acquire);
    if (!s) return 0;   // cannot happen: the state exists before the relay is built
    const auto forward = reinterpret_cast<UpdateFn>(s->forward.load(std::memory_order_acquire));
    if (!forward) return 0;   // nor this: the gate opens only after the forward is published
    s->calls.fetch_add(1, std::memory_order_relaxed);
    const uint32_t x = s->bits[0].load(std::memory_order_relaxed), y = s->bits[1].load(std::memory_order_relaxed), z = s->bits[2].load(std::memory_order_relaxed);
    const bool zero = (x | y | z) == 0;
    uint32_t slot = kSlots;
    for (uint32_t i = 0; i < kSlots; ++i)
        if (s->holding[i].load(std::memory_order_relaxed) == obj) slot = i;
    if (obj && (!zero || slot < kSlots)) {
        bool radar = false;
        __try {
            radar = *reinterpret_cast<volatile const uintptr_t*>(obj) == s->objectVtable &&
                    *reinterpret_cast<volatile const uintptr_t*>(obj + kInterfaceAt) == s->interfaceVtable;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            radar = false;
            s->faulted.fetch_add(1, std::memory_order_relaxed);
        }
        if (!radar) {
            if (slot < kSlots) s->holding[slot].store(0, std::memory_order_relaxed);   // the schematic we wrote is gone (freed, or the address reused): nothing to put back
        } else {
            s->matched.fetch_add(1, std::memory_order_relaxed);
            bool write = true;
            if (!zero && slot == kSlots) {   // a schematic new to us: claim a slot first, so a restore can always find it
                write = false;
                for (uint32_t i = 0; i < kSlots && !write; ++i) {
                    uintptr_t empty = 0;
                    write = s->holding[i].compare_exchange_strong(empty, obj, std::memory_order_relaxed);
                }
                if (!write) s->noSlot.fetch_add(1, std::memory_order_relaxed);
            }
            if (write) {
                volatile uint32_t* const a = reinterpret_cast<volatile uint32_t*>(obj + kTranslationA);
                volatile uint32_t* const t = reinterpret_cast<volatile uint32_t*>(obj + kTranslationB);
                a[0] = x;
                a[1] = y;
                a[2] = z;
                a[3] = 0;
                t[0] = x;
                t[1] = y;
                t[2] = z;
                t[3] = 0;
                if (zero) {
                    s->holding[slot].store(0, std::memory_order_relaxed);
                    s->restored.fetch_add(1, std::memory_order_relaxed);
                } else {
                    s->applied.fetch_add(1, std::memory_order_relaxed);
                    s->lastObj.store(obj, std::memory_order_relaxed);
                    const uint32_t stamp = s->frame.load(std::memory_order_relaxed) + 1u;
                    if (s->lastStamp.exchange(stamp, std::memory_order_relaxed) != stamp) s->appliedFrames.fetch_add(1, std::memory_order_relaxed);
                }
            }
        }
    }
    return forward(obj, b, c, d);
}

// The only place game memory is read outside the hook. A fault returns false.
__declspec(noinline) bool guardedRead(uintptr_t address, void* out, size_t bytes) noexcept {
    __try {
        std::memcpy(out, reinterpret_cast<const void*>(address), bytes);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

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
                auto* p = static_cast<uint8_t*>(VirtualAlloc(reinterpret_cast<void*>(candidate), 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
                if (p) return p;
            }
        }
        if (end <= at) break;
        at = end;
    }
    return nullptr;
}

bool prepareRelay(void* trampoline, void* context) noexcept {
    auto& s = *static_cast<State*>(context);
    const uintptr_t address = reinterpret_cast<uintptr_t>(trampoline);
    std::memcpy(s.relay + avh::kCallbackRelayTrampolineAt, &address, 8);
    DWORD oldProtect = 0;
    if (!VirtualProtect(s.relay, 4096, PAGE_EXECUTE_READ, &oldProtect) || !FlushInstructionCache(GetCurrentProcess(), s.relay, avh::kCallbackRelayBytes)) return false;
    s.forward.store(address, std::memory_order_release);
    return true;
}

bool holdingAny(const State& s) noexcept {
    for (const auto& h : s.holding)
        if (h.load(std::memory_order_relaxed)) return true;
    return false;
}

}  // namespace

RadarAnchorHookState srvRadarAnchorArm(char* whyOut, size_t cap) {
    std::lock_guard<std::mutex> lock(g_armMutex);
    State* s = g_state.load(std::memory_order_acquire);
    if (s && s->state.load() != int(RadarAnchorHookState::NotTried)) {
        if (whyOut && cap) std::snprintf(whyOut, cap, "%s", s->why);
        return RadarAnchorHookState(s->state.load());
    }
    if (!s) {
        s = new State;   // process lifetime: a call already inside the relay must be able to finish
        g_state.store(s, std::memory_order_release);
    }
    const auto stood = [&](const char* why) {
        s->state.store(int(RadarAnchorHookState::StoodDown));
        std::snprintf(s->why, sizeof(s->why), "%s", why);
        Log::get().note("srv turret radar anchor: hook stood down: %s; nothing was patched", why);
        if (whyOut && cap) std::snprintf(whyOut, cap, "%s", s->why);
        return RadarAnchorHookState::StoodDown;
    };
    const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    if (!base) return stood("the game module could not be resolved");
    uint32_t stamp = 0, size = 0;
    if (!gameBuildIdentity(&stamp, &size)) return stood("the game's PE header is unreadable");
    if (stamp != kStamp || size != kImageSize) {
        char b[200];
        std::snprintf(b, sizeof(b), "the game is %s, not build 334925 (PE stamp 0x%08X, image size 0x%X); this hook knows build 334925 only", gameBuildText(), stamp, size);
        return stood(b);
    }
    const uintptr_t target = base + kUpdateRva;
    uint8_t got[kPrologueBytes] = {};
    if (!guardedRead(target, got, kPrologueBytes)) return stood("EliteDangerous64.exe+0x384A590 could not be read");
    if (std::memcmp(got, kPrologue, kPrologueBytes) != 0) {
        char b[200];
        std::snprintf(b, sizeof(b), "the %zu bytes at EliteDangerous64.exe+0x384A590 are not build 334925's radar schematic update prologue (something else has patched it, or the build differs)", kPrologueBytes);
        return stood(b);
    }
    s->objectVtable = base + kObjectVtableRva;
    s->interfaceVtable = base + kInterfaceVtableRva;
    s->relay = allocateRelay(target);
    if (!s->relay) return stood("no executable memory could be placed within two gigabytes of the target");
    avh::buildCallbackRelay(s->relay, &s->gate, reinterpret_cast<const void*>(&radarUpdateHooked));
    // the gate stays shut (the relay forwards straight to the game's function) until the frame thread publishes an offset
    if (!s->hook.install(reinterpret_cast<void*>(target), s->relay, nullptr, "srv-radar-anchor", &prepareRelay, s)) {
        VirtualFree(s->relay, 0, MEM_RELEASE);
        s->relay = nullptr;
        return stood("CodeHook refused it (its own line above, tagged srv-radar-anchor, names why)");
    }
    s->state.store(int(RadarAnchorHookState::Armed));
    std::snprintf(s->why, sizeof(s->why),
                  "hook armed: EliteDangerous64.exe+0x%llX (the radar terrain schematic's update, build 334925) at 0x%llX, stolen=%zu bytes, prologue %zu/%zu bytes verified, relay at 0x%llX",
                  static_cast<unsigned long long>(kUpdateRva), static_cast<unsigned long long>(target), s->hook.stolenBytes(), kPrologueBytes, kPrologueBytes,
                  static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(s->relay)));
    Log::get().note("srv turret radar anchor: %s", s->why);
    if (whyOut && cap) std::snprintf(whyOut, cap, "%s", s->why);
    return RadarAnchorHookState::Armed;
}

RadarAnchorHookState srvRadarAnchorState() {
    State* s = g_state.load(std::memory_order_acquire);
    return s ? RadarAnchorHookState(s->state.load()) : RadarAnchorHookState::NotTried;
}

const char* srvRadarAnchorWhy() {
    State* s = g_state.load(std::memory_order_acquire);
    return s ? s->why : "";
}

void srvRadarAnchorPublish(const uint32_t bits[3], uint32_t frameNo) {
    State* s = g_state.load(std::memory_order_acquire);
    if (!s || s->state.load() != int(RadarAnchorHookState::Armed)) return;
    s->frame.store(frameNo, std::memory_order_relaxed);
    for (int i = 0; i < 3; ++i) s->bits[i].store(bits[i], std::memory_order_relaxed);
    const bool move = (bits[0] | bits[1] | bits[2]) != 0;
    // Shut only when there is nothing to write and nothing to put back. A call that read the old offset just before this may still claim a slot after it: the next frame's publish
    // sees the slot and opens the gate again, so the restore happens a frame later, never not at all.
    s->gate.store(move || holdingAny(*s) ? 1 : 0, std::memory_order_seq_cst);
}

RadarAnchorStats srvRadarAnchorStats() {
    RadarAnchorStats r;
    State* s = g_state.load(std::memory_order_acquire);
    if (!s) return r;
    r.state = RadarAnchorHookState(s->state.load());
    r.calls = s->calls.load(std::memory_order_relaxed);
    r.matched = s->matched.load(std::memory_order_relaxed);
    r.applied = s->applied.load(std::memory_order_relaxed);
    r.appliedFrames = s->appliedFrames.load(std::memory_order_relaxed);
    r.restored = s->restored.load(std::memory_order_relaxed);
    r.faulted = s->faulted.load(std::memory_order_relaxed);
    r.noSlot = s->noSlot.load(std::memory_order_relaxed);
    r.lastStamp = s->lastStamp.load(std::memory_order_relaxed);
    r.lastObj = s->lastObj.load(std::memory_order_relaxed);
    r.holding = holdingAny(*s);
    return r;
}

void srvRadarAnchorShutdown() {
    State* s = g_state.load(std::memory_order_acquire);
    if (s) s->gate.store(0, std::memory_order_seq_cst);
}

}  // namespace edvr
