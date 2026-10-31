// The SRV radar's anchor moved in the game (issue 140; srv_radar_anchor.h says what and why). Exact-build only (tools\hook_signatures.py AUDIT_EXACT_ONLY): the addresses below are
// build 334925's (analysis\decomp\verify_20261031_srv_radar_anchor.txt and verify_20261031_srv_radar_drop.txt), checked against the running image's PE identity and each
// function's first bytes before anything is patched; any other build stands down.
//
// THE UPDATE COUNTER MUST NOT TOUCH XMM1. The update takes the object in RCX and dt in XMM1, and its callback is entered by a jump from the relay, so the game's XMM1 is still live
// when it calls the original. It is declared with four INTEGER parameters and does integer work only before that call (no float arithmetic, no memcpy, no call into anything, no
// logging). The setter's callback may do float work: the setter is a leaf that reads only RCX and RDX on entry (it loads XMM0 and XMM1 from the matrix itself).
#include "srv_radar_anchor.h"

#include <windows.h>
#include <intrin.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>

#include "../common/code_hook.h"
#include "../common/game_build.h"
#include "../common/log.h"
#include "avatar_hooks_core.h"
#include "srv_turret_math.h"

namespace edvr {
namespace {

namespace st = srv_turret;

constexpr uintptr_t kSetterRva = 0x38563A0;            // the schematic's placement setter: interface slot +0x28 (vtable 0x5511F18, entry 0x5511F40); RCX = obj+0x58, RDX = P
constexpr uintptr_t kUpdateRva = 0x384A590;            // the schematic's per-frame update; reached only through the thunk 0x381EF00 (rcx = rdx - 0xB0, xmm1 = xmm2)
constexpr uintptr_t kObjectVtableRva = 0x5511DE8;      // *(obj + 0x00), written by its constructor 0x383DF40
constexpr uintptr_t kInterfaceVtableRva = 0x5511F18;   // *(obj + 0x58), the interface the setter is called through
constexpr uint32_t kStamp = 0x6ADA8048u, kImageSize = 0x640C000u;   // build 334925's PE TimeDateStamp and SizeOfImage
constexpr uintptr_t kInterfaceAt = 0x58;
// `sub rsp,48h; movups xmm0,[rdx]; movups [rcx+1528h],xmm0; movups xmm1,[rdx+10h]`, read from EliteDangerous64-334925.exe through its section table. Boundaries 4, 7, 14, 18:
// CodeHook steals the first two instructions (7 bytes); the first RIP-relative instruction is at +0x2F.
constexpr size_t kSetterPrologueBytes = 18;
constexpr uint8_t kSetterPrologue[kSetterPrologueBytes] = {0x48, 0x83, 0xEC, 0x48, 0x0F, 0x10, 0x02, 0x0F, 0x11, 0x81, 0x28, 0x15, 0x00, 0x00, 0x0F, 0x10, 0x4A, 0x10};
// `push rbx; push rsi; sub rsp,168h; movaps [rsp+130h],xmm7` (the last cut at 16 bytes). Boundaries 2, 3, 10, 18: CodeHook steals the first three (10 bytes).
constexpr size_t kUpdatePrologueBytes = 16;
constexpr uint8_t kUpdatePrologue[kUpdatePrologueBytes] = {0x40, 0x53, 0x56, 0x48, 0x81, 0xEC, 0x68, 0x01, 0x00, 0x00, 0x0F, 0x29, 0xBC, 0x24, 0x30, 0x01};
constexpr uint32_t kSlots = 4;   // schematics whose placement may hold an offset of ours at once (one is expected: the Scorpion's)

using Fn = uintptr_t(__fastcall*)(uintptr_t, uintptr_t, uintptr_t, uintptr_t);

struct Hook {
    CodeHook hook;
    uint8_t* relay = nullptr;
    alignas(8) std::atomic<uintptr_t> gate{0};   // the relay compares this qword with zero
    std::atomic<uintptr_t> forward{0};
    bool armed = false;
};
// One schematic's st::PlacementMemory, kept by the setter's callback (obj 0 = a free slot).
struct Slot {
    std::atomic<uintptr_t> obj{0};
    std::atomic<bool> have{false};
    std::atomic<uint32_t> in[3]{}, out[3]{}, d[3]{};
};
struct State {
    Hook setter, update;
    uintptr_t base = 0, objectVtable = 0, interfaceVtable = 0;   // set before either patch
    std::atomic<int> state{int(RadarAnchorHookState::NotTried)};
    char why[480] = {};
    // published by the frame thread
    std::atomic<uint32_t> bits[3]{};
    std::atomic<uint32_t> frame{0};
    Slot slots[kSlots];
    // the hooks' record
    std::atomic<uint64_t> setterCalls{0}, setterRadar{0}, moved{0}, ours{0}, restored{0}, faulted{0}, noSlot{0}, gameChanges{0}, updates{0};
    std::atomic<bool> holdsOurs{false}, haveGame{false}, suspect{false};
    std::atomic<uint32_t> gameT[3]{}, outT[3]{}, suspectStep[3]{};
    std::atomic<uint32_t> lastAddStamp{0};
    std::atomic<uintptr_t> caller{0}, lastObj{0};
};
std::atomic<State*> g_state{nullptr};
std::mutex g_armMutex;

uint32_t bitsOf(float f) noexcept {
    uint32_t b;
    std::memcpy(&b, &f, sizeof(b));
    return b;
}
float floatOf(uint32_t b) noexcept {
    float f;
    std::memcpy(&f, &b, sizeof(f));
    return f;
}

// The setter's arguments, read under SEH: 1 with P copied when the interface is the radar schematic's, 0 when it is another object's, -1 when a read faulted.
__declspec(noinline) int readRadarCall(State& s, uintptr_t iface, uintptr_t p, float P[16]) noexcept {
    __try {
        if (*reinterpret_cast<const uintptr_t*>(iface) != s.interfaceVtable || *reinterpret_cast<const uintptr_t*>(iface - kInterfaceAt) != s.objectVtable) return 0;
        std::memcpy(P, reinterpret_cast<const void*>(p), 16 * sizeof(float));
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        s.faulted.fetch_add(1, std::memory_order_relaxed);
        return -1;
    }
}

Slot* slotOf(State& s, uintptr_t iface) noexcept {
    for (Slot& slot : s.slots)
        if (slot.obj.load(std::memory_order_relaxed) == iface) return &slot;
    return nullptr;
}
Slot* claimSlot(State& s, uintptr_t iface) noexcept {
    for (Slot& slot : s.slots) {
        uintptr_t empty = 0;
        if (slot.obj.compare_exchange_strong(empty, iface, std::memory_order_relaxed)) {
            slot.have.store(false, std::memory_order_relaxed);
            return &slot;
        }
    }
    return nullptr;
}

// The setter hook: the game's P, with the published offset ADDED to its translation (st::radarPlacementMove), goes to the original. Records for the frame thread; never logs.
__declspec(noinline) uintptr_t __fastcall setterHooked(uintptr_t iface, uintptr_t p, uintptr_t r8, uintptr_t r9) noexcept {
    State* const s = g_state.load(std::memory_order_acquire);
    if (!s) return 0;   // cannot happen: the state exists before the relay is built
    const auto forward = reinterpret_cast<Fn>(s->setter.forward.load(std::memory_order_acquire));
    if (!forward) return 0;   // nor this: the gate opens only after the forward is published
    s->setterCalls.fetch_add(1, std::memory_order_relaxed);
    const uintptr_t caller = reinterpret_cast<uintptr_t>(_ReturnAddress());
    float P[16];
    if (!iface || !p || readRadarCall(*s, iface, p, P) <= 0) return forward(iface, p, r8, r9);
    s->setterRadar.fetch_add(1, std::memory_order_relaxed);
    s->caller.store(caller, std::memory_order_relaxed);
    s->lastObj.store(iface - kInterfaceAt, std::memory_order_relaxed);
    const uint32_t d[3] = {s->bits[0].load(std::memory_order_relaxed), s->bits[1].load(std::memory_order_relaxed), s->bits[2].load(std::memory_order_relaxed)};
    const bool dZero = (d[0] | d[1] | d[2]) == 0u;
    Slot* slot = slotOf(*s, iface);
    if (!slot && !dZero) {
        slot = claimSlot(*s, iface);
        if (!slot) {   // no room to remember the add: the game's own placement, so a fed-back value can never be added to twice
            s->noSlot.fetch_add(1, std::memory_order_relaxed);
            s->holdsOurs.store(false, std::memory_order_relaxed);
            return forward(iface, p, r8, r9);
        }
    }
    st::PlacementMemory m;
    if (slot) {
        m.have = slot->have.load(std::memory_order_relaxed);
        for (int i = 0; i < 3; ++i) {
            m.in[i] = slot->in[i].load(std::memory_order_relaxed);
            m.out[i] = slot->out[i].load(std::memory_order_relaxed);
            m.d[i] = slot->d[i].load(std::memory_order_relaxed);
        }
    }
    // The game's own translation: P's, or for our value fed back, the one it came from.
    uint32_t t[3];
    std::memcpy(t, P + 12, sizeof(t));
    const bool fedBack = m.have && t[0] == m.out[0] && t[1] == m.out[1] && t[2] == m.out[2];
    const uint32_t game[3] = {fedBack ? m.in[0] : t[0], fedBack ? m.in[1] : t[1], fedBack ? m.in[2] : t[2]};
    if (s->haveGame.load(std::memory_order_relaxed)) {
        const uint32_t before[3] = {s->gameT[0].load(std::memory_order_relaxed), s->gameT[1].load(std::memory_order_relaxed), s->gameT[2].load(std::memory_order_relaxed)};
        if (before[0] != game[0] || before[1] != game[1] || before[2] != game[2]) {
            s->gameChanges.fetch_add(1, std::memory_order_relaxed);
            const float b[3] = {floatOf(before[0]), floatOf(before[1]), floatOf(before[2])}, a[3] = {floatOf(game[0]), floatOf(game[1]), floatOf(game[2])};
            const float df[3] = {floatOf(d[0]), floatOf(d[1]), floatOf(d[2])};
            if (!s->suspect.load(std::memory_order_relaxed) && st::placementStepLooksLikeOffset(b, a, df)) {
                for (int i = 0; i < 3; ++i) s->suspectStep[i].store(bitsOf(a[i] - b[i]), std::memory_order_relaxed);
                s->suspect.store(true, std::memory_order_release);
            }
        }
    }
    for (int i = 0; i < 3; ++i) s->gameT[i].store(game[i], std::memory_order_relaxed);
    s->haveGame.store(true, std::memory_order_relaxed);
    float Q[16];
    const st::PlacementMove r = st::radarPlacementMove(P, d, m, Q);
    if (slot) {
        for (int i = 0; i < 3; ++i) {
            slot->in[i].store(m.in[i], std::memory_order_relaxed);
            slot->out[i].store(m.out[i], std::memory_order_relaxed);
            slot->d[i].store(m.d[i], std::memory_order_relaxed);
        }
        slot->have.store(m.have, std::memory_order_relaxed);
        if (!m.have) slot->obj.store(0, std::memory_order_relaxed);   // nothing of ours left in this placement: the slot is free
    }
    s->holdsOurs.store(r == st::PlacementMove::kAdded || r == st::PlacementMove::kOurs, std::memory_order_relaxed);
    if (r == st::PlacementMove::kAdded) {
        for (int i = 0; i < 3; ++i) s->outT[i].store(bitsOf(Q[12 + i]), std::memory_order_relaxed);
        s->lastAddStamp.store(s->frame.load(std::memory_order_relaxed) + 1u, std::memory_order_relaxed);
        s->moved.fetch_add(1, std::memory_order_release);
        return forward(iface, reinterpret_cast<uintptr_t>(Q), r8, r9);
    }
    if (r == st::PlacementMove::kRestored) {
        s->restored.fetch_add(1, std::memory_order_relaxed);
        return forward(iface, reinterpret_cast<uintptr_t>(Q), r8, r9);
    }
    if (r == st::PlacementMove::kOurs) s->ours.fetch_add(1, std::memory_order_relaxed);
    return forward(iface, p, r8, r9);
}

// The update counter. Integer work only before the original runs (the file's comment above): XMM1 is the game's dt. It writes nothing in game memory.
__declspec(noinline) uintptr_t __fastcall updateHooked(uintptr_t obj, uintptr_t b, uintptr_t c, uintptr_t d) noexcept {
    State* const s = g_state.load(std::memory_order_acquire);
    if (!s) return 0;
    const auto forward = reinterpret_cast<Fn>(s->update.forward.load(std::memory_order_acquire));
    if (!forward) return 0;
    if (obj) {
        bool radar = false;
        __try {
            radar = *reinterpret_cast<volatile const uintptr_t*>(obj) == s->objectVtable &&
                    *reinterpret_cast<volatile const uintptr_t*>(obj + kInterfaceAt) == s->interfaceVtable;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            radar = false;
        }
        if (radar) s->updates.fetch_add(1, std::memory_order_relaxed);
    }
    return forward(obj, b, c, d);
}

// The only place game memory is read outside the hooks. A fault returns false.
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
    auto& h = *static_cast<Hook*>(context);
    const uintptr_t address = reinterpret_cast<uintptr_t>(trampoline);
    std::memcpy(h.relay + avh::kCallbackRelayTrampolineAt, &address, 8);
    DWORD oldProtect = 0;
    if (!VirtualProtect(h.relay, 4096, PAGE_EXECUTE_READ, &oldProtect) || !FlushInstructionCache(GetCurrentProcess(), h.relay, avh::kCallbackRelayBytes)) return false;
    h.forward.store(address, std::memory_order_release);
    return true;
}

// One hook: the prologue compared, a relay placed, the patch installed and the gate opened. "" on success, else why not (nothing patched).
const char* installHook(Hook& h, uintptr_t target, const uint8_t* prologue, size_t n, const void* callback, const char* who) {
    uint8_t got[32] = {};
    if (n > sizeof(got) || !guardedRead(target, got, n)) return "the function's first bytes could not be read";
    if (std::memcmp(got, prologue, n) != 0) return "its first bytes are not build 334925's (something else has patched it, or the build differs)";
    h.relay = allocateRelay(target);
    if (!h.relay) return "no executable memory could be placed within two gigabytes of the target";
    avh::buildCallbackRelay(h.relay, &h.gate, callback);
    if (!h.hook.install(reinterpret_cast<void*>(target), h.relay, nullptr, who, &prepareRelay, &h)) {
        VirtualFree(h.relay, 0, MEM_RELEASE);
        h.relay = nullptr;
        return "CodeHook refused it (its own line above names why)";
    }
    h.armed = true;
    h.gate.store(1, std::memory_order_seq_cst);   // open: with the offset at zero the callbacks pass the game's own values through
    return "";
}

bool insideModule(const State& s, uintptr_t a) noexcept { return a >= s.base && a - s.base < kImageSize; }

}  // namespace

RadarAnchorHookState srvRadarAnchorArm(char* whyOut, size_t cap) {
    std::lock_guard<std::mutex> lock(g_armMutex);
    State* s = g_state.load(std::memory_order_acquire);
    if (s && s->state.load() != int(RadarAnchorHookState::NotTried)) {
        if (whyOut && cap) std::snprintf(whyOut, cap, "%s", s->why);
        return RadarAnchorHookState(s->state.load());
    }
    if (!s) {
        s = new State;   // process lifetime: a call already inside a relay must be able to finish
        g_state.store(s, std::memory_order_release);
    }
    const auto stood = [&](const char* why) {
        s->state.store(int(RadarAnchorHookState::StoodDown));
        std::snprintf(s->why, sizeof(s->why), "%s", why);
        Log::get().note("srv turret radar anchor: setter hook stood down: %s; nothing was patched", why);
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
    s->base = base;
    s->objectVtable = base + kObjectVtableRva;
    s->interfaceVtable = base + kInterfaceVtableRva;
    const uintptr_t setter = base + kSetterRva;
    const char* setterWhy = installHook(s->setter, setter, kSetterPrologue, kSetterPrologueBytes, reinterpret_cast<const void*>(&setterHooked), "srv-radar-setter");
    if (*setterWhy) {
        char b[260];
        std::snprintf(b, sizeof(b), "EliteDangerous64.exe+0x38563A0 (the radar schematic's placement setter): %s", setterWhy);
        return stood(b);
    }
    s->state.store(int(RadarAnchorHookState::Armed));
    std::snprintf(s->why, sizeof(s->why),
                  "setter hook armed: EliteDangerous64.exe+0x%llX (the radar schematic's placement setter, build 334925) at 0x%llX, stolen=%zu bytes, prologue %zu/%zu bytes verified, "
                  "relay at 0x%llX",
                  static_cast<unsigned long long>(kSetterRva), static_cast<unsigned long long>(setter), s->setter.hook.stolenBytes(), kSetterPrologueBytes, kSetterPrologueBytes,
                  static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(s->setter.relay)));
    Log::get().note("srv turret radar anchor: %s", s->why);
    // The update counter: only says the schematic is alive (the 10 s line's updates). Its standing down leaves the setter hook in.
    const uintptr_t update = base + kUpdateRva;
    const char* updateWhy = installHook(s->update, update, kUpdatePrologue, kUpdatePrologueBytes, reinterpret_cast<const void*>(&updateHooked), "srv-radar-update");
    if (*updateWhy)
        Log::get().note("srv turret radar anchor: update counter stood down: EliteDangerous64.exe+0x384A590 (the radar schematic's update): %s; nothing was patched there", updateWhy);
    else
        Log::get().note("srv turret radar anchor: update counter armed: EliteDangerous64.exe+0x%llX (the radar schematic's update, counted only, nothing written) at 0x%llX, stolen=%zu bytes, "
                        "prologue %zu/%zu bytes verified, relay at 0x%llX",
                        static_cast<unsigned long long>(kUpdateRva), static_cast<unsigned long long>(update), s->update.hook.stolenBytes(), kUpdatePrologueBytes, kUpdatePrologueBytes,
                        static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(s->update.relay)));
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
}

RadarAnchorStats srvRadarAnchorStats() {
    RadarAnchorStats r;
    State* s = g_state.load(std::memory_order_acquire);
    if (!s) return r;
    r.state = RadarAnchorHookState(s->state.load());
    r.updateArmed = s->update.armed;
    r.setterCalls = s->setterCalls.load(std::memory_order_relaxed);
    r.setterRadar = s->setterRadar.load(std::memory_order_relaxed);
    r.moved = s->moved.load(std::memory_order_acquire);
    r.ours = s->ours.load(std::memory_order_relaxed);
    r.restored = s->restored.load(std::memory_order_relaxed);
    r.faulted = s->faulted.load(std::memory_order_relaxed);
    r.noSlot = s->noSlot.load(std::memory_order_relaxed);
    r.gameChanges = s->gameChanges.load(std::memory_order_relaxed);
    r.updates = s->updates.load(std::memory_order_relaxed);
    r.holdsOurs = s->holdsOurs.load(std::memory_order_relaxed);
    r.haveGame = s->haveGame.load(std::memory_order_relaxed);
    for (int i = 0; i < 3; ++i) {
        r.gameT[i] = floatOf(s->gameT[i].load(std::memory_order_relaxed));
        r.outT[i] = floatOf(s->outT[i].load(std::memory_order_relaxed));
    }
    r.lastAddStamp = s->lastAddStamp.load(std::memory_order_relaxed);
    r.caller = s->caller.load(std::memory_order_relaxed);
    r.callerRva = insideModule(*s, r.caller) ? r.caller - s->base : 0;
    r.obj = s->lastObj.load(std::memory_order_relaxed);
    r.suspect = s->suspect.load(std::memory_order_acquire);
    if (r.suspect)
        for (int i = 0; i < 3; ++i) r.suspectStep[i] = floatOf(s->suspectStep[i].load(std::memory_order_relaxed));
    return r;
}

void srvRadarAnchorShutdown() {
    State* s = g_state.load(std::memory_order_acquire);
    if (!s) return;
    s->setter.gate.store(0, std::memory_order_seq_cst);
    s->update.gate.store(0, std::memory_order_seq_cst);
}

}  // namespace edvr
