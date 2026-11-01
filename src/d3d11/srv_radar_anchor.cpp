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
constexpr uintptr_t kUpdateRva = 0x384A590;            // the schematic's per-frame update; reached only through the thunk 0x381EF00 (rcx = rdx - 0xB0, xmm1 = xmm2)
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
// `push rbx; push rsi; sub rsp,168h; movaps [rsp+130h],xmm7` (the last cut at 16 bytes). Boundaries 2, 3, 10, 18: CodeHook steals the first three (10 bytes).
constexpr size_t kUpdatePrologueBytes = 16;
constexpr uint8_t kUpdatePrologue[kUpdatePrologueBytes] = {0x40, 0x53, 0x56, 0x48, 0x81, 0xEC, 0x68, 0x01, 0x00, 0x00, 0x0F, 0x29, 0xBC, 0x24, 0x30, 0x01};
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
    // the direction: the camera sample (u in the world, its host frame, its source, the head's pitch) published by the render thread under a version (odd while written)
    std::atomic<uint32_t> smpVer{0};
    std::atomic<uint32_t> u[3]{};
    std::atomic<uint64_t> smpSeq{0};
    std::atomic<bool> smpCpu{false};
    std::atomic<uint32_t> smpHeadPitch{0};
    std::atomic<int64_t> smpQpc{0};
    std::atomic<bool> uValid{false};
    std::atomic<uint64_t> directed{0}, fallback{0}, fallbackNoU{0}, fallbackNoAnchor{0}, anchorFaults{0};
    std::atomic<bool> lastDirected{false}, haveAnchor{false};
    std::atomic<uint32_t> recA[9]{}, recP[9]{}, recU[3]{}, recUA[3]{}, recD[3]{};
    // the pairing's record (the last directed add, and sums for the 10 s line)
    std::atomic<uint64_t> recNowSeq{0}, recSmpSeq{0}, recAge{0};
    std::atomic<uint32_t> recHeadPitch{0}, recAimPitch{0}, recAimYaw{0}, recIntervalMs{0};
    std::atomic<bool> recCpu{false}, recPaired{false};
    std::atomic<uint64_t> ageSum{0}, ageMax{0}, paired{0}, unpaired{0}, ageHist[3]{};
    // the residual at each camera write (srv_turret_math.h ResidualStats, kept as sums for the 10 s line), the measured display horizon, and the gap from the shown call to the write
    std::atomic<uint64_t> resN{0}, resMaxMilli{0}, resLagging{0}, resLeading{0}, latN{0}, horizonN{0}, horizonSumUs{0}, gapN{0}, gapSumUs{0}, gapMinUs{UINT64_MAX}, gapMaxUs{0};
    std::atomic<int64_t> resSumMicro{0}, latSumMilli{0};
    std::atomic<uint32_t> setterThread{0};
    // THE CALL RING: each radar setter call, written by the setter thread and read by the render thread's draw-time correction (srvRadarAnchorShownCall) and the residual, under a
    // version per entry (odd while written): the call's QPC time, the offset it left in force (0: none), the anchor's world rotation it read (valid flag), and s * F.
    struct CallSlot {
        std::atomic<uint32_t> ver{0};
        std::atomic<uint64_t> serial{0};
        std::atomic<int64_t> t{0}, baseT{0};
        std::atomic<uint32_t> d[3]{}, a[9]{}, sF{0};
        std::atomic<bool> aValid{false};
    };
    CallSlot calls[8];
    uint32_t callNext = 0;   // the setter thread's
    uint64_t callSerial = 0;   // the setter thread's: the calls put in the ring so far
    // the setter thread's own: the anchor's rotation by host frame; the last sample's aim (the residual's rate) and the sample interval
    st::SeqRing<st::Rot9, 8> aRing;
    int64_t lastSmpQpc = 0, qpcFreq = 1;
    st::Aim prevAimK;
    bool havePrevAim = false;
    double intervalS = 1.0 / 90.0;
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
    bool cpu = false;
    float headPitch = 0.0f;
    int64_t qpc = 0;
};
bool readSample(State& s, Sample& out) noexcept {
    for (int attempt = 0; attempt < 4; ++attempt) {
        const uint32_t v1 = s.smpVer.load(std::memory_order_acquire);
        if (v1 & 1u) continue;
        if (!s.uValid.load(std::memory_order_acquire)) return false;
        for (int i = 0; i < 3; ++i) out.u[i] = floatOf(s.u[i].load(std::memory_order_relaxed));
        out.seq = s.smpSeq.load(std::memory_order_relaxed);
        out.cpu = s.smpCpu.load(std::memory_order_relaxed);
        out.headPitch = floatOf(s.smpHeadPitch.load(std::memory_order_relaxed));
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
void maxInto(std::atomic<uint64_t>& m, uint64_t v) noexcept {
    uint64_t cur = m.load(std::memory_order_relaxed);
    while (v > cur && !m.compare_exchange_weak(cur, v, std::memory_order_relaxed)) {
    }
}
void minInto(std::atomic<uint64_t>& m, uint64_t v) noexcept {
    uint64_t cur = m.load(std::memory_order_relaxed);
    while (v < cur && !m.compare_exchange_weak(cur, v, std::memory_order_relaxed)) {
    }
}

// One call into the ring (the setter thread, the only writer): the version odd while the fields are written, even again after.
void putCall(State& s, int64_t t, int64_t baseT, const uint32_t d[3], const float* A, bool aValid, float sF) noexcept {
    State::CallSlot& c = s.calls[s.callNext];
    s.callNext = (s.callNext + 1) % (sizeof(s.calls) / sizeof(s.calls[0]));
    const uint32_t v = c.ver.load(std::memory_order_relaxed);
    c.ver.store(v + 1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    c.serial.store(++s.callSerial, std::memory_order_relaxed);
    c.t.store(t, std::memory_order_relaxed);
    c.baseT.store(baseT, std::memory_order_relaxed);
    for (int i = 0; i < 3; ++i) c.d[i].store(d[i], std::memory_order_relaxed);
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) c.a[i * 3 + j].store(aValid ? bitsOf(A[i * 4 + j]) : 0u, std::memory_order_relaxed);
    c.aValid.store(aValid, std::memory_order_relaxed);
    c.sF.store(bitsOf(sF), std::memory_order_relaxed);
    c.ver.store(v + 2, std::memory_order_release);
}
// One entry read whole (any thread): false while it is being written (after a few tries) or never written.
bool readCall(const State::CallSlot& c, RadarAnchorShownCall& out) noexcept {
    for (int attempt = 0; attempt < 4; ++attempt) {
        const uint32_t v1 = c.ver.load(std::memory_order_acquire);
        if (v1 & 1u) continue;
        if (v1 == 0) return false;
        out.serial = c.serial.load(std::memory_order_relaxed);
        out.t = c.t.load(std::memory_order_relaxed);
        out.baseT = c.baseT.load(std::memory_order_relaxed);
        for (int i = 0; i < 3; ++i) out.d[i] = floatOf(c.d[i].load(std::memory_order_relaxed));
        for (int i = 0; i < 16; ++i) out.A[i] = 0.0f;
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j) out.A[i * 4 + j] = floatOf(c.a[i * 3 + j].load(std::memory_order_relaxed));
        out.A[15] = 1.0f;
        out.aValid = c.aValid.load(std::memory_order_relaxed);
        out.sF = floatOf(c.sF.load(std::memory_order_relaxed));
        std::atomic_thread_fence(std::memory_order_acquire);
        if (c.ver.load(std::memory_order_relaxed) == v1) return true;
    }
    return false;
}
// The call a camera write at QPC time `before` shows: the most recent call made before it. False when none is kept.
bool shownCall(const State& s, int64_t before, RadarAnchorShownCall& out) noexcept {
    bool found = false;
    for (const State::CallSlot& c : s.calls) {
        RadarAnchorShownCall one;
        if (!readCall(c, one) || one.t <= 0 || one.t >= before) continue;
        if (!found || one.t > out.t) {
            out = one;
            found = true;
        }
    }
    return found;
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
        s.anchorFaults.fetch_add(1, std::memory_order_relaxed);
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
    // The fallback (0, 0, s * F); all zero: no move. With the turret's forward u and the anchor's world rotation A, d is s * F along u in the anchor's axes (st::radarAnchorAlong).
    const uint32_t fb[3] = {s->bits[0].load(std::memory_order_relaxed), s->bits[1].load(std::memory_order_relaxed), s->bits[2].load(std::memory_order_relaxed)};
    const bool dZero = (fb[0] | fb[1] | fb[2]) == 0u;
    uint32_t d[3] = {fb[0], fb[1], fb[2]};
    bool directed = false, haveU = false;
    float u[3] = {0.0f, 0.0f, 0.0f}, ua[3] = {0.0f, 0.0f, 0.0f};
    float A[16] = {};
    int anchor = 0;
    int64_t baseT = 0;   // the write time of the camera sample this call used
    const int64_t q = qpcNow();   // this call's time: the call ring's key (a camera write shows the most recent call before it)
    {
        uint32_t none = 0;
        if (s->setterThread.compare_exchange_strong(none, GetCurrentThreadId(), std::memory_order_relaxed)) {
        }
    }
    if (!dZero) {
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j) s->recP[i * 3 + j].store(bitsOf(P[i * 4 + j]), std::memory_order_relaxed);
        anchor = readAnchor(*s, iface, A);   // the game's getter, called only while there is an offset to add
        s->haveAnchor.store(anchor > 0, std::memory_order_relaxed);
        if (anchor > 0)
            for (int i = 0; i < 3; ++i)
                for (int j = 0; j < 3; ++j) s->recA[i * 3 + j].store(bitsOf(A[i * 4 + j]), std::memory_order_relaxed);
        // The anchor's rotation by host frame, so a camera sample is paired with the anchor of ITS frame (the vehicle turning or driving adds no error).
        const uint64_t nowSeq = nativeTemporalFrameSequence();
        if (anchor > 0) {
            st::Rot9 rot;
            for (int i = 0; i < 3; ++i)
                for (int j = 0; j < 3; ++j) rot.r[i * 3 + j] = A[i * 4 + j];
            s->aRing.put(nowSeq, rot);
        }
        Sample smp;
        haveU = readSample(*s, smp);
        if (haveU) {
            std::memcpy(u, smp.u, sizeof(u));
            baseT = smp.qpc;
        }
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
            // u in the anchor's axes at the sample's frame: the turret's aim relative to the vehicle, used as it is, with no lead. What the turret does after the sample is taken up when
            // the frame is drawn (srv_turret.cpp, the draw-time correction), and an unled aim keeps any error a smooth lag instead of a jitter (srv_turret_math.h THE SHAKE).
            const st::Aim aimK = st::aimOf(uaK);
            if (smp.qpc > 0 && smp.qpc != s->lastSmpQpc) {
                // A new camera write: the truth for it is aimK. It shows the most recent call before it (the call ring), so the residual is aimK minus that call's aim, the display
                // horizon is the time from the sample that call used to this write, and the gap is how long before the write that call was.
                const double freq = double(s->qpcFreq);
                if (s->lastSmpQpc > 0 && smp.qpc > s->lastSmpQpc) {
                    const double gap = double(smp.qpc - s->lastSmpQpc) / freq;
                    if (gap > 0.0 && gap < 0.1) s->intervalS += (gap - s->intervalS) / 32.0;
                }
                RadarAnchorShownCall shown;
                const float shownSF = shownCall(*s, smp.qpc, shown) ? shown.sF : 0.0f;
                if (shownSF != 0.0f && (shown.d[0] != 0.0f || shown.d[1] != 0.0f || shown.d[2] != 0.0f)) {
                    const float usedDir[3] = {shown.d[0] / shownSF, shown.d[1] / shownSF, shown.d[2] / shownSF};
                    const double residual = st::wrapDeg(aimK.yaw - st::aimOf(usedDir).yaw);
                    const double perSample = s->havePrevAim ? st::wrapDeg(aimK.yaw - s->prevAimK.yaw) : 0.0;   // the plain change between the last two samples (instrument only)
                    s->resN.fetch_add(1, std::memory_order_relaxed);
                    s->resSumMicro.fetch_add(static_cast<int64_t>(residual * 1.0e6), std::memory_order_relaxed);
                    maxInto(s->resMaxMilli, static_cast<uint64_t>(std::fabs(residual) * 1000.0 + 0.5));
                    const int side = st::residualSide(residual, perSample);
                    if (side > 0) s->resLagging.fetch_add(1, std::memory_order_relaxed);
                    if (side < 0) s->resLeading.fetch_add(1, std::memory_order_relaxed);
                    if (side != 0) {
                        s->latN.fetch_add(1, std::memory_order_relaxed);
                        s->latSumMilli.fetch_add(static_cast<int64_t>(residual / perSample * 1000.0), std::memory_order_relaxed);
                    }
                    const uint64_t gapUs = static_cast<uint64_t>(double(smp.qpc - shown.t) / freq * 1.0e6);
                    s->gapN.fetch_add(1, std::memory_order_relaxed);
                    s->gapSumUs.fetch_add(gapUs, std::memory_order_relaxed);
                    minInto(s->gapMinUs, gapUs);
                    maxInto(s->gapMaxUs, gapUs);
                    if (shown.baseT > 0 && smp.qpc > shown.baseT) {   // from the write of the sample that call used to this write
                        const double horizon = double(smp.qpc - shown.baseT) / freq;
                        if (horizon > 0.0 && horizon < 0.1) {
                            s->horizonN.fetch_add(1, std::memory_order_relaxed);
                            s->horizonSumUs.fetch_add(static_cast<uint64_t>(horizon * 1.0e6), std::memory_order_relaxed);
                        }
                    }
                }
                s->prevAimK = aimK;
                s->havePrevAim = true;
                s->lastSmpQpc = smp.qpc;
            }
            std::memcpy(ua, uaK, sizeof(ua));
            const float sF = floatOf(fb[2]);
            for (int i = 0; i < 3; ++i) d[i] = bitsOf(sF * ua[i]);
            directed = (d[0] | d[1] | d[2]) != 0u;
            if (!directed) std::memcpy(d, fb, sizeof(d));
            const uint64_t age = nowSeq > smp.seq ? nowSeq - smp.seq : 0;
            s->recNowSeq.store(nowSeq, std::memory_order_relaxed);
            s->recSmpSeq.store(smp.seq, std::memory_order_relaxed);
            s->recAge.store(age, std::memory_order_relaxed);
            s->recIntervalMs.store(bitsOf(float(s->intervalS * 1000.0)), std::memory_order_relaxed);
            s->recHeadPitch.store(bitsOf(smp.headPitch), std::memory_order_relaxed);
            s->recAimPitch.store(bitsOf(float(aimK.pitch)), std::memory_order_relaxed);
            s->recAimYaw.store(bitsOf(float(aimK.yaw)), std::memory_order_relaxed);
            s->recCpu.store(smp.cpu, std::memory_order_relaxed);
            s->recPaired.store(atK != nullptr, std::memory_order_relaxed);
            if (directed) {
                s->ageSum.fetch_add(age, std::memory_order_relaxed);
                maxInto(s->ageMax, age);
                s->ageHist[age < 2 ? age : 2].fetch_add(1, std::memory_order_relaxed);
                (atK ? s->paired : s->unpaired).fetch_add(1, std::memory_order_relaxed);
            }
        }
    }
    Slot* slot = slotOf(*s, iface);
    if (!slot && !dZero) {
        slot = claimSlot(*s, iface);
        if (!slot) {   // no room to remember the add: the game's own placement, so a fed-back value can never be added to twice
            s->noSlot.fetch_add(1, std::memory_order_relaxed);
            s->holdsOurs.store(false, std::memory_order_relaxed);
            const uint32_t none[3] = {0u, 0u, 0u};
            putCall(*s, q, baseT, none, A, anchor > 0, floatOf(fb[2]));   // the game's own placement is in force
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
    {   // the call ring: the offset this call leaves in force (added, or ours fed back: the remembered one; else none) and the anchor it read
        const bool inForce = r == st::PlacementMove::kAdded || r == st::PlacementMove::kOurs;
        const uint32_t none[3] = {0u, 0u, 0u};
        putCall(*s, q, baseT, inForce ? m.d : none, A, anchor > 0, floatOf(fb[2]));
    }
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
        if (directed) {
            s->directed.fetch_add(1, std::memory_order_relaxed);
        } else {
            s->fallback.fetch_add(1, std::memory_order_relaxed);
            (haveU ? s->fallbackNoAnchor : s->fallbackNoU).fetch_add(1, std::memory_order_relaxed);
        }
        s->lastDirected.store(directed, std::memory_order_relaxed);
        for (int i = 0; i < 3; ++i) {
            s->recU[i].store(bitsOf(u[i]), std::memory_order_relaxed);
            s->recUA[i].store(bitsOf(directed ? ua[i] : 0.0f), std::memory_order_relaxed);
            s->recD[i].store(d[i], std::memory_order_relaxed);
        }
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
    {
        uint32_t none = 0;
        if (s->slotThread.compare_exchange_strong(none, GetCurrentThreadId(), std::memory_order_relaxed)) {
        }
    }
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
    h.armed = true;
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

void srvRadarAnchorPublishDirection(const float u[3], bool valid, uint64_t seq, bool cpu, float headPitchDeg, int64_t qpc) {
    State* s = g_state.load(std::memory_order_acquire);
    if (!s) return;
    if (!u || !valid) {
        s->uValid.store(false, std::memory_order_release);
        return;
    }
    s->smpVer.fetch_add(1, std::memory_order_acq_rel);   // odd: being written (one writer, the render thread)
    for (int i = 0; i < 3; ++i) s->u[i].store(bitsOf(u[i]), std::memory_order_relaxed);
    s->smpSeq.store(seq, std::memory_order_relaxed);
    s->smpCpu.store(cpu, std::memory_order_relaxed);
    s->smpHeadPitch.store(bitsOf(headPitchDeg), std::memory_order_relaxed);
    s->smpQpc.store(qpc, std::memory_order_relaxed);
    s->smpVer.fetch_add(1, std::memory_order_release);   // even: whole
    s->uValid.store(true, std::memory_order_release);
}

void srvRadarAnchorTakeWindowMax(uint64_t* ageMax, double* residualMaxDeg, double* gapMinMs, double* gapMaxMs) {
    State* s = g_state.load(std::memory_order_acquire);
    const uint64_t a = s ? s->ageMax.exchange(0, std::memory_order_relaxed) : 0, r = s ? s->resMaxMilli.exchange(0, std::memory_order_relaxed) : 0,
                   gmin = s ? s->gapMinUs.exchange(UINT64_MAX, std::memory_order_relaxed) : UINT64_MAX, gmax = s ? s->gapMaxUs.exchange(0, std::memory_order_relaxed) : 0;
    if (ageMax) *ageMax = a;
    if (residualMaxDeg) *residualMaxDeg = double(r) / 1000.0;
    if (gapMinMs) *gapMinMs = gmin == UINT64_MAX ? 0.0 : double(gmin) / 1000.0;
    if (gapMaxMs) *gapMaxMs = double(gmax) / 1000.0;
}

bool srvRadarAnchorShownCall(int64_t before, RadarAnchorShownCall& out) {
    State* s = g_state.load(std::memory_order_acquire);
    if (!s || s->state.load(std::memory_order_relaxed) != int(RadarAnchorHookState::Armed)) return false;
    return shownCall(*s, before, out);
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
    r.directed = s->directed.load(std::memory_order_relaxed);
    r.fallback = s->fallback.load(std::memory_order_relaxed);
    r.fallbackNoU = s->fallbackNoU.load(std::memory_order_relaxed);
    r.fallbackNoAnchor = s->fallbackNoAnchor.load(std::memory_order_relaxed);
    r.anchorFaults = s->anchorFaults.load(std::memory_order_relaxed);
    r.lastDirected = s->lastDirected.load(std::memory_order_relaxed);
    r.haveAnchor = s->haveAnchor.load(std::memory_order_relaxed);
    for (int i = 0; i < 9; ++i) {
        r.anchorRows[i] = floatOf(s->recA[i].load(std::memory_order_relaxed));
        r.pRows[i] = floatOf(s->recP[i].load(std::memory_order_relaxed));
    }
    for (int i = 0; i < 3; ++i) {
        r.u[i] = floatOf(s->recU[i].load(std::memory_order_relaxed));
        r.uAnchor[i] = floatOf(s->recUA[i].load(std::memory_order_relaxed));
        r.d[i] = floatOf(s->recD[i].load(std::memory_order_relaxed));
    }
    r.nowSeq = s->recNowSeq.load(std::memory_order_relaxed);
    r.sampleSeq = s->recSmpSeq.load(std::memory_order_relaxed);
    r.age = s->recAge.load(std::memory_order_relaxed);
    r.headPitch = floatOf(s->recHeadPitch.load(std::memory_order_relaxed));
    r.aimPitch = floatOf(s->recAimPitch.load(std::memory_order_relaxed));
    r.aimYaw = floatOf(s->recAimYaw.load(std::memory_order_relaxed));
    r.sampleCpu = s->recCpu.load(std::memory_order_relaxed);
    r.paired = s->recPaired.load(std::memory_order_relaxed);
    r.ageSum = s->ageSum.load(std::memory_order_relaxed);
    r.pairedCount = s->paired.load(std::memory_order_relaxed);
    r.unpairedCount = s->unpaired.load(std::memory_order_relaxed);
    r.intervalMs = floatOf(s->recIntervalMs.load(std::memory_order_relaxed));
    for (int i = 0; i < 3; ++i) r.ageHist[i] = s->ageHist[i].load(std::memory_order_relaxed);
    r.resN = s->resN.load(std::memory_order_relaxed);
    r.resSumMicro = s->resSumMicro.load(std::memory_order_relaxed);
    r.resLagging = s->resLagging.load(std::memory_order_relaxed);
    r.resLeading = s->resLeading.load(std::memory_order_relaxed);
    r.latN = s->latN.load(std::memory_order_relaxed);
    r.latSumMilli = s->latSumMilli.load(std::memory_order_relaxed);
    r.horizonN = s->horizonN.load(std::memory_order_relaxed);
    r.horizonSumUs = s->horizonSumUs.load(std::memory_order_relaxed);
    r.gapN = s->gapN.load(std::memory_order_relaxed);
    r.gapSumUs = s->gapSumUs.load(std::memory_order_relaxed);
    r.setterThread = s->setterThread.load(std::memory_order_relaxed);
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
    s->update.gate.store(0, std::memory_order_seq_cst);
    s->slot.gate.store(0, std::memory_order_seq_cst);
}

}  // namespace edvr
