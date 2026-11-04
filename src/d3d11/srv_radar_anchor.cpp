// The SRV radar's anchor moved in the game (issue 140; srv_radar_anchor.h says what and why). Two hooks: the placement setter (on and reversed) and the schematic slot's camera
// (camera mode). Exact-build only (tools\hook_signatures.py AUDIT_EXACT_ONLY): the addresses below are build 334925's (analysis\decomp\verify_20261031_srv_radar_anchor.txt,
// verify_20261031_srv_radar_drop.txt and verify_20261101_srv_radar_slot_camera.txt), checked against the running image's PE identity and each function's first bytes before anything
// is patched; any other build stands down.
//
// The setter's callback may do float work: the setter is a leaf that reads only RCX and RDX on entry (it loads XMM0 and XMM1 from the matrix itself).
#include "srv_radar_anchor.h"

#include <windows.h>
#include <intrin.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>

#include "../common/code_hook.h"
#include "../common/game_build.h"
#include "../common/log.h"
#include "avatar_hooks_core.h"
#include "srv_turret_math.h"
#include "vr_camera_census.h"   // nativeTemporalFrameSequence: the host's frame, which pairs the anchor's rotation with the camera sample of the same frame

namespace edvr {
namespace {

namespace st = srv_turret;

constexpr uintptr_t kSetterRva = 0x38563A0;            // the schematic's placement setter: interface slot +0x28 (vtable 0x5511F18, entry 0x5511F40); RCX = obj+0x58, RDX = P
constexpr uintptr_t kObjectVtableRva = 0x5511DE8;      // *(obj + 0x00), written by its constructor 0x383DF40
constexpr uintptr_t kInterfaceVtableRva = 0x5511F18;   // *(obj + 0x58), the interface the setter is called through
constexpr uint32_t kStamp = 0x6ADA8048u, kImageSize = 0x640C000u;   // build 334925's PE TimeDateStamp and SizeOfImage
constexpr uintptr_t kInterfaceAt = 0x58;
// The anchor component at obj+0x140 and its world 4x4 getter, vtable slot +0x20: 0x3843530 (the region box) calls it as `mov rcx,[iface+0E8h]; mov rax,[rcx]; call [rax+20h]` and reads 16
// floats at RAX (rows x, y, z, translation; row-vector convention, so the placement's translation t lands at t * A3x3 + A.t).
constexpr uintptr_t kAnchorAt = 0x140, kAnchorWorldSlot = 0x20;
// `sub rsp,48h; movups xmm0,[rdx]; movups [rcx+1528h],xmm0; movups xmm1,[rdx+10h]`, read from EliteDangerous64-334925.exe through its section table. Boundaries 4, 7, 14, 18:
// CodeHook steals the first two instructions (7 bytes); the first RIP-relative instruction is at +0x2F.
constexpr size_t kSetterPrologueBytes = 18;
constexpr uint8_t kSetterPrologue[kSetterPrologueBytes] = {0x48, 0x83, 0xEC, 0x48, 0x0F, 0x10, 0x02, 0x0F, 0x11, 0x81, 0x28, 0x15, 0x00, 0x00, 0x0F, 0x10, 0x4A, 0x10};
constexpr uint32_t kSlots = 4;   // schematics whose placement may hold an offset of ours at once (one is expected: the Scorpion's)
// THE SLOT CAMERA (camera mode; srv_radar_anchor.h): the schematic slot's update, RCX = the slot, RDX = the camera (its only caller is 0x36BF330), and the slot's vtable (both slot
// constructors; analysis\decomp\verify_20261101_srv_radar_slot_camera.txt).
constexpr uintptr_t kSlotRva = 0x36BF3C0;
constexpr uintptr_t kSlotVtableRva = 0x54FB7F8;
// `push rbp; push r14; push r15; lea rbp,[rsp-6F0h]; sub rsp,7F0h`, read from EliteDangerous64-334925.exe through its section table. Boundaries 2, 4, 6, 14, 21: CodeHook steals the
// three pushes (6 bytes); none of the five is rip-relative, and the first that is lies at +0x15.
constexpr size_t kSlotPrologueBytes = 21;
constexpr uint8_t kSlotPrologue[kSlotPrologueBytes] = {0x40, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8D, 0xAC, 0x24, 0x10, 0xF9, 0xFF, 0xFF, 0x48, 0x81, 0xEC, 0xF0, 0x07, 0x00, 0x00};

using Fn = uintptr_t(__fastcall*)(uintptr_t, uintptr_t, uintptr_t, uintptr_t);
using MatrixFn = const float*(__fastcall*)(uintptr_t);

struct Hook {
    CodeHook hook;
    uint8_t* relay = nullptr;
    alignas(8) std::atomic<uintptr_t> gate{0};   // the relay compares this qword with zero
    std::atomic<uintptr_t> forward{0};
};
// One schematic's st::PlacementMemory, kept by the setter's callback (obj 0 = a free slot).
struct Slot {
    std::atomic<uintptr_t> obj{0};
    std::atomic<bool> have{false};
    std::atomic<uint32_t> in[3]{}, out[3]{}, d[3]{};
};
struct State {
    Hook setter;
    uintptr_t base = 0, objectVtable = 0, interfaceVtable = 0;   // set before either patch
    std::atomic<int> state{int(RadarAnchorHookState::NotTried)};
    char why[480] = {};
    // published by the frame thread
    std::atomic<uint32_t> bits[3]{};
    std::atomic<uint32_t> frame{0};
    Slot slots[kSlots];
    // the setter's record
    std::atomic<uint64_t> moved{0}, ours{0}, restored{0}, faulted{0}, noSlot{0};
    std::atomic<bool> holdsOurs{false}, haveGame{false};
    std::atomic<uint32_t> gameT[3]{}, outT[3]{}, lastD[3]{};
    std::atomic<uint32_t> lastAddStamp{0};
    std::atomic<uintptr_t> caller{0}, lastObj{0};
    // the direction: the camera sample (u in the world, its host frame, the QPC time of the camera write) published by the render thread under a version (odd while written)
    std::atomic<uint32_t> smpVer{0};
    std::atomic<uint32_t> u[3]{};
    std::atomic<uint64_t> smpSeq{0};
    std::atomic<int64_t> smpQpc{0};
    std::atomic<bool> uValid{false};
    // the setter thread's own: the anchor's rotation by host frame
    st::SeqRing<st::Rot9, 8> aRing;
    int64_t qpcFreq = 1;   // set by whichever arm runs first; the slot hook's age of the sample uses it
    // THE SLOT CAMERA: its hook, the slot vtable, its own arm state, the back distance published by the frame thread (float bits; 0: every call gets the game's camera), and its record
    Hook slot;
    uintptr_t slotVtable = 0;
    std::atomic<int> slotState{int(RadarAnchorHookState::NotTried)};
    std::atomic<uint32_t> slotBack{0};
    std::atomic<uint64_t> slotCalls{0}, slotRadar[3][3]{}, slotMoved{0}, slotFromTurret{0}, slotFromCamera{0}, slotFaults{0};
    std::atomic<uint32_t> slotThread{0};
    struct SlotFirst {   // the first move of each kind and list: claimed by the hook, filled, then ready (release) for the frame thread
        std::atomic<bool> claimed{false}, ready{false};
        uintptr_t slot = 0;
        uint32_t index = 0, mode = 0, list = 0;
        bool fromTurret = false;
        float u[3] = {}, back = 0.0f;
    };
    SlotFirst slotFirst[3][3];
};
std::atomic<State*> g_state{nullptr};
std::mutex g_armMutex;

bool insideModule(const State& s, uintptr_t a) noexcept { return a >= s.base && a - s.base < kImageSize; }

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

// The render thread's camera sample, read whole (the version unchanged and even across the read). False when there is none or it kept changing.
struct Sample {
    float u[3] = {};
    uint64_t seq = 0;
    int64_t qpc = 0;
};
bool readSample(State& s, Sample& out) noexcept {
    for (int attempt = 0; attempt < 4; ++attempt) {
        const uint32_t v1 = s.smpVer.load(std::memory_order_acquire);
        if (v1 & 1u) continue;
        if (!s.uValid.load(std::memory_order_acquire)) return false;
        for (int i = 0; i < 3; ++i) out.u[i] = floatOf(s.u[i].load(std::memory_order_relaxed));
        out.seq = s.smpSeq.load(std::memory_order_relaxed);
        out.qpc = s.smpQpc.load(std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_acquire);
        if (s.smpVer.load(std::memory_order_relaxed) == v1) return true;
    }
    return false;
}
int64_t qpcNow() noexcept {
    LARGE_INTEGER t{};
    QueryPerformanceCounter(&t);
    return t.QuadPart;
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

// The anchor's world 4x4 through its vfunc +0x20 (kAnchorWorldSlot): 1 when read; 0 when the anchor is null or its vtable or getter is not inside the game's image (nothing is called);
// -1 when a read or the call faulted.
__declspec(noinline) int readAnchor(State& s, uintptr_t iface, float A[16]) noexcept {
    __try {
        const uintptr_t anchor = *reinterpret_cast<const uintptr_t*>(iface - kInterfaceAt + kAnchorAt);
        if (!anchor) return 0;
        const uintptr_t vtable = *reinterpret_cast<const uintptr_t*>(anchor);
        if (!insideModule(s, vtable)) return 0;
        const uintptr_t getter = *reinterpret_cast<const uintptr_t*>(vtable + kAnchorWorldSlot);
        if (!insideModule(s, getter)) return 0;
        const float* m = reinterpret_cast<MatrixFn>(getter)(anchor);
        if (!m) return 0;
        std::memcpy(A, m, 16 * sizeof(float));
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
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
    const uintptr_t caller = reinterpret_cast<uintptr_t>(_ReturnAddress());
    float P[16];
    if (!iface || !p || readRadarCall(*s, iface, p, P) <= 0) return forward(iface, p, r8, r9);
    s->caller.store(caller, std::memory_order_relaxed);
    s->lastObj.store(iface - kInterfaceAt, std::memory_order_relaxed);
    // The fallback (0, 0, s * F); all zero: no move. With the turret's forward u and the anchor's world rotation A, d is s * F along u in the anchor's axes (st::radarAnchorAlong).
    const uint32_t fb[3] = {s->bits[0].load(std::memory_order_relaxed), s->bits[1].load(std::memory_order_relaxed), s->bits[2].load(std::memory_order_relaxed)};
    const bool dZero = (fb[0] | fb[1] | fb[2]) == 0u;
    uint32_t d[3] = {fb[0], fb[1], fb[2]};
    if (!dZero) {
        float u[3] = {0.0f, 0.0f, 0.0f}, ua[3] = {0.0f, 0.0f, 0.0f};
        float A[16] = {};
        const int anchor = readAnchor(*s, iface, A);   // the game's getter, called only while there is an offset to add
        // The anchor's rotation by host frame, so a camera sample is paired with the anchor of ITS frame (the vehicle turning or driving adds no error).
        const uint64_t nowSeq = nativeTemporalFrameSequence();
        if (anchor > 0) {
            st::Rot9 rot;
            for (int i = 0; i < 3; ++i)
                for (int j = 0; j < 3; ++j) rot.r[i * 3 + j] = A[i * 4 + j];
            s->aRing.put(nowSeq, rot);
        }
        Sample smp;
        const bool haveU = readSample(*s, smp);
        if (haveU) std::memcpy(u, smp.u, sizeof(u));
        float ak[16];
        const st::Rot9* atK = haveU ? s->aRing.find(smp.seq) : nullptr;
        if (anchor > 0) {
            std::memcpy(ak, A, sizeof(ak));
            if (atK)
                for (int i = 0; i < 3; ++i)
                    for (int j = 0; j < 3; ++j) ak[i * 4 + j] = atK->r[i * 3 + j];
        }
        float unitD[3], uaK[3];
        if (haveU && anchor > 0 && st::radarAnchorAlong(u, ak, 1.0f, unitD, uaK)) {
            // u in the anchor's axes at the sample's frame: the turret's aim relative to the vehicle, used as it is, with no lead (an unled aim keeps any error a smooth lag instead
            // of a jitter).
            std::memcpy(ua, uaK, sizeof(ua));
            const float sF = floatOf(fb[2]);
            for (int i = 0; i < 3; ++i) d[i] = bitsOf(sF * ua[i]);
            if ((d[0] | d[1] | d[2]) == 0u) std::memcpy(d, fb, sizeof(d));
        }
    }
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
        for (int i = 0; i < 3; ++i) s->lastD[i].store(d[i], std::memory_order_relaxed);
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

// The only place game memory is read outside the hooks. A fault returns false.
__declspec(noinline) bool guardedRead(uintptr_t address, void* out, size_t bytes) noexcept {
    __try {
        std::memcpy(out, reinterpret_cast<const void*>(address), bytes);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// THE SLOT CAMERA's hook (0x36BF3C0; camera mode). For a radar slot (st::radarSlotTest) while a back distance is published, the original gets a private copy of the camera (0x2F0 bytes on
// this stack, its plane pointers rebased) moved back by d = -D u (st::radarCameraBack: the turret's forward from the CPU camera sample while fresh, else the camera's own forward;
// st::cameraMoveBy: position, view row 3, VP dirty), which the call reads and rebuilds into instead of the game's camera; every other call, and any fault, gets the game's camera.
// Game thread: records only (no logging, no allocation), and every game read is under SEH.
bool sehRead(void*, uintptr_t address, void* out, size_t bytes) noexcept { return guardedRead(address, out, bytes); }
__declspec(noinline) uintptr_t __fastcall slotHooked(uintptr_t slot, uintptr_t cam, uintptr_t r8, uintptr_t r9) noexcept {
    State* const s = g_state.load(std::memory_order_acquire);
    if (!s) return 0;   // cannot happen: the state exists before the relay is built
    const auto forward = reinterpret_cast<Fn>(s->slot.forward.load(std::memory_order_acquire));
    if (!forward) return 0;   // nor this: the gate opens only after the forward is published
    s->slotCalls.fetch_add(1, std::memory_order_relaxed);
    uint32_t thread = 0;
    s->slotThread.compare_exchange_strong(thread, GetCurrentThreadId(), std::memory_order_relaxed);   // the first thread the hook runs on (the 10 s line)
    const float back = floatOf(s->slotBack.load(std::memory_order_relaxed));
    if (!(back > 0.0f) || !slot || !cam) return forward(slot, cam, r8, r9);
    st::RadarSlot hit;
    const int radar = st::radarSlotTest(slot, s->slotVtable, s->interfaceVtable, &sehRead, nullptr, hit);
    if (radar < 0) s->slotFaults.fetch_add(1, std::memory_order_relaxed);
    if (radar <= 0) return forward(slot, cam, r8, r9);
    const int kind = hit.mode == st::kRadarModeTerrain ? 0 : hit.mode == st::kRadarModeIcon ? 1 : 2, list = hit.list == 0 ? 0 : hit.list == 2 ? 1 : 2;
    s->slotRadar[kind][list].fetch_add(1, std::memory_order_relaxed);
    alignas(16) uint8_t copy[st::kCamBytes];
    if (!guardedRead(cam, copy, sizeof(copy))) {
        s->slotFaults.fetch_add(1, std::memory_order_relaxed);
        return forward(slot, cam, r8, r9);
    }
    st::cameraRebase(copy, cam);
    float camForward[3];
    std::memcpy(camForward, copy + st::kCamForwardAt, sizeof(camForward));
    Sample smp;
    const bool haveU = readSample(*s, smp);
    float d[3];
    const st::CameraBackFrom from = st::radarCameraBack(back, haveU, haveU ? qpcNow() - smp.qpc : -1, s->qpcFreq, haveU ? smp.u : nullptr, camForward, d);
    if (from == st::CameraBackFrom::kNone) return forward(slot, cam, r8, r9);
    st::cameraMoveBy(copy, d);
    s->slotMoved.fetch_add(1, std::memory_order_relaxed);
    (from == st::CameraBackFrom::kTurret ? s->slotFromTurret : s->slotFromCamera).fetch_add(1, std::memory_order_relaxed);
    State::SlotFirst& first = s->slotFirst[kind][list];
    bool unclaimed = false;
    if (first.claimed.compare_exchange_strong(unclaimed, true, std::memory_order_relaxed)) {
        first.slot = slot;
        first.index = hit.index;
        first.mode = hit.mode;
        first.list = hit.list;
        first.fromTurret = from == st::CameraBackFrom::kTurret;
        for (int i = 0; i < 3; ++i) first.u[i] = back > 0.0f ? -d[i] / back : 0.0f;
        first.back = back;
        first.ready.store(true, std::memory_order_release);
    }
    return forward(slot, reinterpret_cast<uintptr_t>(copy), r8, r9);   // the copy lives on this frame until the original returns
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
    h.gate.store(1, std::memory_order_seq_cst);   // open: with the offset at zero the callbacks pass the game's own values through
    return "";
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
    {
        LARGE_INTEGER f{};
        QueryPerformanceFrequency(&f);
        s->qpcFreq = f.QuadPart > 0 ? f.QuadPart : 1;
    }
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

void srvRadarAnchorPublishDirection(const float u[3], bool valid, uint64_t seq, bool /*cpu*/, float /*headPitchDeg*/, int64_t qpc) {
    State* s = g_state.load(std::memory_order_acquire);
    if (!s) return;
    if (!u || !valid) {
        s->uValid.store(false, std::memory_order_release);
        return;
    }
    s->smpVer.fetch_add(1, std::memory_order_acq_rel);   // odd: being written (one writer, the render thread)
    for (int i = 0; i < 3; ++i) s->u[i].store(bitsOf(u[i]), std::memory_order_relaxed);
    s->smpSeq.store(seq, std::memory_order_relaxed);
    s->smpQpc.store(qpc, std::memory_order_relaxed);
    s->smpVer.fetch_add(1, std::memory_order_release);   // even: whole
    s->uValid.store(true, std::memory_order_release);
}

RadarAnchorStats srvRadarAnchorStats() {
    RadarAnchorStats r;
    State* s = g_state.load(std::memory_order_acquire);
    if (!s) return r;
    r.state = RadarAnchorHookState(s->state.load());
    r.moved = s->moved.load(std::memory_order_acquire);
    r.ours = s->ours.load(std::memory_order_relaxed);
    r.restored = s->restored.load(std::memory_order_relaxed);
    r.faulted = s->faulted.load(std::memory_order_relaxed);
    r.noSlot = s->noSlot.load(std::memory_order_relaxed);
    r.holdsOurs = s->holdsOurs.load(std::memory_order_relaxed);
    r.haveGame = s->haveGame.load(std::memory_order_relaxed);
    for (int i = 0; i < 3; ++i) {
        r.gameT[i] = floatOf(s->gameT[i].load(std::memory_order_relaxed));
        r.outT[i] = floatOf(s->outT[i].load(std::memory_order_relaxed));
        r.d[i] = floatOf(s->lastD[i].load(std::memory_order_relaxed));
    }
    r.lastAddStamp = s->lastAddStamp.load(std::memory_order_relaxed);
    r.caller = s->caller.load(std::memory_order_relaxed);
    r.callerRva = insideModule(*s, r.caller) ? r.caller - s->base : 0;
    r.obj = s->lastObj.load(std::memory_order_relaxed);
    return r;
}

RadarAnchorHookState srvRadarSlotArm() {
    std::lock_guard<std::mutex> lock(g_armMutex);
    State* s = g_state.load(std::memory_order_acquire);
    if (s && s->slotState.load() != int(RadarAnchorHookState::NotTried)) return RadarAnchorHookState(s->slotState.load());
    if (!s) {
        s = new State;   // process lifetime: a call already inside a relay must be able to finish
        g_state.store(s, std::memory_order_release);
    }
    const auto stood = [&](const char* why) {
        s->slotState.store(int(RadarAnchorHookState::StoodDown));
        Log::get().note("srv turret radar camera: slot hook stood down: %s; nothing was patched", why);
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
    // the same values the setter's arm sets (whichever arms first), before the patch
    s->base = base;
    {
        LARGE_INTEGER f{};
        QueryPerformanceFrequency(&f);
        s->qpcFreq = f.QuadPart > 0 ? f.QuadPart : 1;
    }
    s->objectVtable = base + kObjectVtableRva;
    s->interfaceVtable = base + kInterfaceVtableRva;
    s->slotVtable = base + kSlotVtableRva;
    const uintptr_t target = base + kSlotRva;
    const char* why = installHook(s->slot, target, kSlotPrologue, kSlotPrologueBytes, reinterpret_cast<const void*>(&slotHooked), "srv-radar-slot");
    if (*why) {
        char b[260];
        std::snprintf(b, sizeof(b), "EliteDangerous64.exe+0x36BF3C0 (the schematic slot's update): %s", why);
        return stood(b);
    }
    s->slotState.store(int(RadarAnchorHookState::Armed));
    Log::get().note("srv turret radar camera: slot hook armed: EliteDangerous64.exe+0x%llX (the schematic slot's update, build 334925) at 0x%llX, stolen=%zu bytes, prologue %zu/%zu bytes "
                    "verified, relay at 0x%llX; the radar's slots get a camera moved back by the radar forward key",
                    static_cast<unsigned long long>(kSlotRva), static_cast<unsigned long long>(target), s->slot.hook.stolenBytes(), kSlotPrologueBytes, kSlotPrologueBytes,
                    static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(s->slot.relay)));
    return RadarAnchorHookState::Armed;
}

void srvRadarSlotPublish(float backM) {
    State* s = g_state.load(std::memory_order_acquire);
    if (!s || s->slotState.load() != int(RadarAnchorHookState::Armed)) return;
    s->slotBack.store(bitsOf(backM > 0.0f && std::isfinite(backM) ? backM : 0.0f), std::memory_order_relaxed);
}

RadarSlotStats srvRadarSlotStats() {
    RadarSlotStats r;
    State* s = g_state.load(std::memory_order_acquire);
    if (!s) return r;
    r.state = RadarAnchorHookState(s->slotState.load());
    r.calls = s->slotCalls.load(std::memory_order_relaxed);
    for (int k = 0; k < 3; ++k)
        for (int l = 0; l < 3; ++l) {
            r.radar[k][l] = s->slotRadar[k][l].load(std::memory_order_relaxed);
            const State::SlotFirst& f = s->slotFirst[k][l];
            if (!f.ready.load(std::memory_order_acquire)) continue;
            RadarSlotFirst& o = r.first[k][l];
            o.have = true;
            o.slot = f.slot;
            o.index = f.index;
            o.mode = f.mode;
            o.list = f.list;
            o.fromTurret = f.fromTurret;
            for (int i = 0; i < 3; ++i) o.u[i] = f.u[i];
            o.back = f.back;
        }
    r.moved = s->slotMoved.load(std::memory_order_relaxed);
    r.fromTurret = s->slotFromTurret.load(std::memory_order_relaxed);
    r.fromCamera = s->slotFromCamera.load(std::memory_order_relaxed);
    r.faults = s->slotFaults.load(std::memory_order_relaxed);
    r.thread = s->slotThread.load(std::memory_order_relaxed);
    return r;
}

void srvRadarAnchorShutdown() {
    State* s = g_state.load(std::memory_order_acquire);
    if (!s) return;
    s->setter.gate.store(0, std::memory_order_seq_cst);
    s->slot.gate.store(0, std::memory_order_seq_cst);
}

}  // namespace edvr
