// The F2 instruments: the glue (explorer_cam_f2.h says what they are; explorer_cam_f2_core.h holds the decisions, the commander-frame
// math and the text of every line, driven by tools\explorer_cam_probe_test).
//
// THREADS. The free-camera and controller observers run on whichever thread the game's job system calls those updates from, after
// the original has returned and every press is restored: each takes a try-flag (a second concurrent caller skips), allocates nothing,
// writes no log line and calls nothing of the game's, and publishes through a small ring or a seqlock. The neck's replacement getter
// runs on ANY thread that reads the eye matrix, for every humanoid: it takes nothing, allocates nothing, logs nothing, and returns
// exactly what the original returns. The tick (explorerCamF2Tick) is the probe's frame boundary and is the only code that logs.
#include "explorer_cam_f2.h"
#include "explorer_cam_f2_core.h"

#include <windows.h>
#include <intrin.h>

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "explorer_cam.h"

namespace edvr {
namespace {

// ---- shared state ----------------------------------------------------------------------------------------------------------
std::atomic<bool> g_armed{false};

// I3 pressed (free-camera observer)
std::atomic<bool> g_busy3{false};
f2::Pressed3 g_last3;
bool g_have3 = false;
std::atomic<uint64_t> g_freeCalls{0};
ecm::Ring<f2::F2Event, 64> g_ringFree;

// I4 (controller observer)
std::atomic<bool> g_busyCtl{false};
f2::ControllerSnap g_lastCtl;
bool g_haveCtl = false;
std::atomic<uint64_t> g_ctlCalls{0};
std::atomic<uint32_t> g_ctlMode{0};
std::atomic<uint64_t> g_ctlLast{0};
ecm::Ring<f2::F2Event, 64> g_ringCtl;

std::atomic<uint64_t> g_f2Seq{0};

// The neck.
f2::NeckTargets g_neck;                          // written before the swap, read-only afterwards (the replacement reads it)
std::atomic<uint64_t> g_neckCalls{0}, g_neckLocalCalls{0};
std::atomic<uintptr_t> g_localEye{0};            // the local commander's eye interface, or 0
ecp::KeyTable<32> g_neckSeen;                    // the distinct interface pointers (capped)
std::atomic<uint32_t> g_neckSeenOverflow{0};
std::atomic<uint32_t> g_staleCount{0};
std::atomic<uintptr_t> g_staleVptr{0};
ecp::SeqSlot<f2::NeckSample> g_sample;
bool g_neckTried = false;
bool g_neckInstalled = false;
uintptr_t g_neckOriginal = 0;                    // what the slot held

// The avatar fade counter.
std::atomic<uint64_t> g_fadeCalls{0}, g_fadeEnabledCalls{0}, g_fadeEnabledWindow{0}, g_fadeNoBlock{0}, g_fadeReadFaults{0};
std::atomic<uint64_t> g_fadeEnabledWhileZero{0}, g_fadeEnabledWhileZeroWindow{0};
std::atomic<uint32_t> g_fadeMinBits{0x7F800000u};   // +infinity: the least amount among the enabled ones in the window
ecp::KeyTable<64> g_fadeSeen;                       // the distinct components (capped)
std::atomic<uint32_t> g_fadeSeenOverflow{0};
ecp::DistinctSet<64> g_fadeEnabledSet;              // the components that read enabled = 1 since the last heartbeat
const int32_t* g_fadeGlobal = nullptr;              // Explorer Cam's dither-fade mode global, for the verdict
const char* g_fadeStatus = "not tried";

// H: the local commander's skeleton, route B (explorer_cam_f2_core.h). The FindJoint hook stores the interfaces the game attaches the local player's
// avatars to; the free-camera hook's post-call reads them at most once a second.
struct TimerStat {
    std::atomic<uint32_t> n{0}, minUs{0xFFFFFFFFu}, maxUs{0}, allMaxUs{0};
};
struct HeadCache {
    uint64_t iface = 0;
    uint16_t headIdx = f2::kNoJoint;
    uint16_t footL = f2::kNoJoint, footR = f2::kNoJoint;   // H2's two feet (looked up once per interface)
};
std::atomic<bool> g_busyH{false};
f2::HeadTargets g_head;                          // set at arm before the FindJoint observer is attached; read-only after
std::atomic<uint32_t> g_headState{0};            // 0 not tried, 1 armed, 2 stood down
std::atomic<uint64_t> g_headSteps{0}, g_headCalls{0}, g_headFaults{0};
std::atomic<bool> g_h2On{false};                 // H2 walks the animated pose too (the foot literals were verified); a part of H, it stands down with it
std::atomic<uint64_t> g_headNextMs{0};           // GetTickCount64 at which the next evaluation may run
std::atomic<const char*> g_headLast{"none"};
f2::HeadDown g_headDown;                         // written once, before g_headDownSet is released
std::atomic<uint32_t> g_headDownSet{0};
std::atomic<uint32_t> g_headDownClaim{0};
ecp::SeqSlot<f2::HeadSample> g_headSample;
HeadCache g_headCache[2];                        // hook thread only (g_busyH)
TimerStat g_t58, g_t48, g_tFind, g_tWalk;
float g_walkLocals[f2::kMaxWalkJoints * f2::kJointFloats];   // hook thread only (g_busyH): the pose arrays, copied
uint16_t g_walkParents[f2::kMaxWalkJoints];
uint32_t g_headIntervalMs = f2::kHeadIntervalMs;

// The arm lines say what happened once per session.
const char* g_i4Status = "not tried";
const char* g_neckStatus = "not tried";
bool g_announced = false;

#ifdef EDVR_EXPLORER_CAM_TEST
explorercamf2test::NeckSeam g_testNeck;
explorercamf2test::HeadSeam g_testHead;
#endif

// ---- guarded access --------------------------------------------------------------------------------------------------------
// The pressed int of the action object at object+handleOffset, or kUnreadable (NULL, not an object pointer, or a fault).
__declspec(noinline) int32_t sehPressedAt(const uint8_t* object, uint32_t handleOffset) noexcept {
    __try {
        uint64_t p = 0;
        std::memcpy(&p, object + handleOffset, 8);
        if (p < 0x10000u || p >= 0x00007FFF00000000ull || (p & 7u) != 0) return f2::kUnreadable;
        int32_t v = 0;
        std::memcpy(&v, reinterpret_cast<const void*>(p + ecm::kOffActionPressed), 4);
        return v;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return f2::kUnreadable;
    }
}
__declspec(noinline) bool sehReadController(const uint8_t* c, f2::ControllerSnap* s) noexcept {
    __try {
        s->mode = c[ecm::kOffCtlMode];
        std::memcpy(&s->presetKind, c + ecm::kOffCtlPresetKind, 4);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
// The paired sample: the eye interface's matrix and the activity's two poses, in one go. `vptrBad` says the interface is stale.
__declspec(noinline) bool sehReadNeckSample(const uint8_t* activity, uintptr_t iface, const f2::NeckTargets& t, f2::NeckSample* s, bool* vptrBad,
                                            uint64_t* badVptr) noexcept {
    __try {
        uint64_t vptr = 0;
        std::memcpy(&vptr, reinterpret_cast<const void*>(iface), 8);
        if (!f2::neckInterfaceOk(vptr, t)) {
            *vptrBad = true;
            *badVptr = vptr;
            return false;
        }
        std::memcpy(s->eye, reinterpret_cast<const void*>(iface + f2::kOffEyeMatrix), 64);
        std::memcpy(s->world, activity + 0x70, 64);
        std::memcpy(s->local, activity + ecm::kOffLocalPose, 64);
        s->state = activity[ecm::kOffState];
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
struct EyeRead {
    float a[16], b[16], c[16], d[16], v[4];
};
__declspec(noinline) bool sehReadEyeInterface(uintptr_t iface, const f2::NeckTargets& t, EyeRead* r, bool* vptrBad, uint64_t* badVptr) noexcept {
    __try {
        uint64_t vptr = 0;
        std::memcpy(&vptr, reinterpret_cast<const void*>(iface), 8);
        if (!f2::neckInterfaceOk(vptr, t)) {
            *vptrBad = true;
            *badVptr = vptr;
            return false;
        }
        std::memcpy(r->a, reinterpret_cast<const void*>(iface + f2::kOffEyeMatrix), 64);
        std::memcpy(r->b, reinterpret_cast<const void*>(iface + f2::kOffEyeB), 64);
        std::memcpy(r->c, reinterpret_cast<const void*>(iface + f2::kOffEyeC), 64);
        std::memcpy(r->d, reinterpret_cast<const void*>(iface + f2::kOffEyeD), 64);
        std::memcpy(r->v, reinterpret_cast<const void*>(iface + f2::kOffEyeVec), 16);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
// One component's dither block, read after the update: comp+0x378 -> block (+0x90 enabled, +0x120 amount), comp+0x380 eased. 0 = read, 1 = no
// block, 2 = a fault.
__declspec(noinline) int sehReadFade(const uint8_t* comp, uint8_t* enabled, float* amount, float* eased) noexcept {
    __try {
        uint64_t block = 0;
        std::memcpy(&block, comp + ecm::kOffAvatarFadeBlock, 8);
        std::memcpy(eased, comp + ecm::kOffAvatarFadeEased, 4);
        if (block < 0x10000u || block >= 0x00007FFF00000000ull) return 1;
        *enabled = *reinterpret_cast<const uint8_t*>(block + ecm::kOffFadeBlockEnabled);
        std::memcpy(amount, reinterpret_cast<const void*>(block + ecm::kOffFadeBlockAmount), 4);
        return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 2;
    }
}
__declspec(noinline) bool sehReadGlobal(const int32_t* p, int32_t* out) noexcept {
    __try {
        *out = *reinterpret_cast<const volatile int32_t*>(p);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
__declspec(noinline) bool sehReadQword(uintptr_t address, uint64_t* out) noexcept {
    __try {
        std::memcpy(out, reinterpret_cast<const void*>(address), 8);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
__declspec(noinline) bool sehBytesEqual(uintptr_t address, const uint8_t* expected, size_t n) noexcept {
    __try {
        return std::memcmp(reinterpret_cast<const void*>(address), expected, n) == 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
__declspec(noinline) bool sehStoreQword(uintptr_t address, uintptr_t value) noexcept {
    __try {
        *reinterpret_cast<volatile uintptr_t*>(address) = value;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// ---- H: the local commander's skeleton ---------------------------------------------------------------------------------------------
// The FindJoint observer runs on whichever thread the game calls FindJoint from (many systems, many calls): it counts, compares the return address
// with the two attach sites and the name with the literal, and stores. Everything else here runs on the game's camera-job thread, inside the
// free-camera hook's post-call: the update has returned, every press is restored and nothing of ours is held. Every read and every call into the
// game is under SEH, in a function of its own with no destructors.
__declspec(noinline) bool sehReadU32(uintptr_t address, uint32_t* out) noexcept {
    __try {
        std::memcpy(out, reinterpret_cast<const void*>(address), 4);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
__declspec(noinline) bool sehReadU16(uintptr_t address, uint16_t* out) noexcept {
    __try {
        std::memcpy(out, reinterpret_cast<const void*>(address), 2);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
__declspec(noinline) bool sehReadU8(uintptr_t address, uint8_t* out) noexcept {
    __try {
        *out = *reinterpret_cast<const volatile uint8_t*>(address);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
__declspec(noinline) bool sehReadFrame(const uint8_t* activity, float* local, float* world) noexcept {
    __try {
        std::memcpy(local, activity + ecm::kOffLocalPose, 64);
        std::memcpy(world, activity + 0x70, 64);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
// The four vtable slots H uses (+0x18, +0x30, +0x48, +0x58).
__declspec(noinline) bool sehReadSlots(uintptr_t vtable, uint64_t* out) noexcept {
    __try {
        for (uint32_t i = 0; i < f2::kHeadSlots; ++i) std::memcpy(&out[i], reinterpret_cast<const void*>(vtable + 8u * f2::kHeadSlotIndex[i]), 8);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
// H2: copy the pose's local transforms and parents (P+0x48 and P+0x50 point at them) into our buffers.
__declspec(noinline) bool sehReadPoseArrays(uintptr_t pose, uint32_t joints, float* locals, uint16_t* parents) noexcept {
    __try {
        uint64_t lp = 0, pp = 0;
        std::memcpy(&lp, reinterpret_cast<const void*>(pose + f2::kPoseLocalsOff), 8);
        std::memcpy(&pp, reinterpret_cast<const void*>(pose + f2::kPoseParentsOff), 8);
        if (lp < 0x10000u || lp >= 0x00007FFF00000000ull || pp < 0x10000u || pp >= 0x00007FFF00000000ull) return false;
        std::memcpy(locals, reinterpret_cast<const void*>(lp), static_cast<size_t>(joints) * f2::kJointBytes);
        std::memcpy(parents, reinterpret_cast<const void*>(pp), static_cast<size_t>(joints) * 2u);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
using HeadPoseFn = void* (__fastcall*)(void* iface);
using HeadFindFn = uint32_t (__fastcall*)(void* iface, const char* name);
using HeadMatrixFn = void (__fastcall*)(void* iface, uint32_t index, float* out);
__declspec(noinline) bool sehCallPose(uintptr_t fn, uintptr_t iface, uintptr_t* out) noexcept {
    __try {
        *out = reinterpret_cast<uintptr_t>(reinterpret_cast<HeadPoseFn>(fn)(reinterpret_cast<void*>(iface)));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
__declspec(noinline) bool sehCallFind(uintptr_t fn, uintptr_t iface, const char* name, uint32_t* out) noexcept {
    __try {
        *out = reinterpret_cast<HeadFindFn>(fn)(reinterpret_cast<void*>(iface), name);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
__declspec(noinline) bool sehCallMatrix(uintptr_t fn, uintptr_t iface, uint32_t index, float* out) noexcept {
    __try {
        reinterpret_cast<HeadMatrixFn>(fn)(reinterpret_cast<void*>(iface), index, out);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

uint64_t qpcNow() noexcept {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return static_cast<uint64_t>(t.QuadPart);
}
void atomicMax(std::atomic<uint32_t>& a, uint32_t v) noexcept {
    uint32_t c = a.load(std::memory_order_relaxed);
    while (v > c && !a.compare_exchange_weak(c, v, std::memory_order_relaxed)) {}
}
void atomicMin(std::atomic<uint32_t>& a, uint32_t v) noexcept {
    uint32_t c = a.load(std::memory_order_relaxed);
    while (v < c && !a.compare_exchange_weak(c, v, std::memory_order_relaxed)) {}
}
void noteTimer(TimerStat& t, uint64_t t0, uint64_t t1) noexcept {
    static const uint64_t freq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return static_cast<uint64_t>(f.QuadPart > 0 ? f.QuadPart : 1);
    }();
    const uint64_t us64 = (t1 - t0) * 1000000ull / freq;
    const uint32_t us = us64 > 0xFFFFFFFEull ? 0xFFFFFFFEu : static_cast<uint32_t>(us64);
    t.n.fetch_add(1, std::memory_order_relaxed);
    atomicMin(t.minUs, us);
    atomicMax(t.maxUs, us);
    atomicMax(t.allMaxUs, us);
}
f2::HeadTiming drainTimer(TimerStat& t) noexcept {
    f2::HeadTiming r;
    r.n = t.n.exchange(0, std::memory_order_relaxed);
    const uint32_t mn = t.minUs.exchange(0xFFFFFFFFu, std::memory_order_relaxed);
    r.maxUs = t.maxUs.exchange(0, std::memory_order_relaxed);
    r.minUs = r.n ? mn : 0;
    r.allMaxUs = t.allMaxUs.load(std::memory_order_relaxed);
    return r;
}

// H stops for the session: the first reason wins, and the tick says it once.
void headStandDown(f2::HeadWhy why, uint32_t slot, uint64_t a, uint64_t b, uint64_t c, bool fault, const char* text = nullptr) noexcept {
    if (fault) g_headFaults.fetch_add(1, std::memory_order_relaxed);
    if (g_headDownClaim.exchange(1, std::memory_order_acq_rel) != 0) return;
    g_headDown.why = why;
    g_headDown.slot = slot;
    g_headDown.a = a;
    g_headDown.b = b;
    g_headDown.c = c;
    if (text) std::snprintf(g_headDown.text, sizeof(g_headDown.text), "%s", text);
    g_headLast.store("stood down", std::memory_order_relaxed);
    g_headState.store(2, std::memory_order_release);
    g_headDownSet.store(1, std::memory_order_release);
}
bool headFault(f2::HeadStage stage, uint32_t slot) noexcept {
    headStandDown(f2::HeadWhy::Fault, slot, static_cast<uint64_t>(stage), 0, 0, true);
    return false;
}

// BEFORE every call into the game: the interface's vtable pointer is a skeleton interface's (RR or AO, and the same kind as before) and every slot H
// uses holds exactly the expected function. A captured pointer may go stale when the game destroys the avatar: an unreadable interface, or one whose
// first qword is no longer RR's or AO's vtable, is STALE (dropped, a later capture replaces it); a slot that differs in a vtable that IS RR's or AO's
// is a different build, and stands H down.
enum class Verify { Ok, Stale, Down };
Verify headVerify(uint32_t site, uintptr_t iface, f2::HeadKind expect, uint64_t* fn, f2::HeadKind* kindOut) noexcept {
    const f2::HeadTargets& T = g_head;
    uint64_t vptr = 0;
    if (!sehReadQword(iface, &vptr)) return Verify::Stale;
    const f2::HeadKind kind = f2::headKindOfVtable(vptr, T);
    if (kind == f2::HeadKind::None || (expect != f2::HeadKind::None && kind != expect)) return Verify::Stale;
    if (!sehReadSlots(static_cast<uintptr_t>(vptr), fn)) {
        headFault(f2::HeadStage::ReadSlots, site);
        return Verify::Down;
    }
    uint32_t bad = 0;
    uint64_t found = 0;
    if (f2::headVerifySlots(vptr, fn, kind, T, &bad, &found) != 0) {
        headStandDown(f2::HeadWhy::SlotMismatch, site, 8u * f2::kHeadSlotIndex[bad], found, f2::headExpectedFunction(kind, bad, T), false);
        return Verify::Down;
    }
    *kindOut = kind;
    return Verify::Ok;
}
bool headStale(uint32_t site, uint64_t iface, f2::HeadSlot& S) noexcept {
    explorerCamSkeletonDrop(static_cast<int>(site), iface);
    g_headCache[site] = HeadCache();
    S.state = static_cast<uint8_t>(f2::HeadSlotState::Stale);
    return true;
}

// H2: the four joints (head, pov, feet) composed from the animated pose. No game call: the arrays are copied under SEH and walked here.
void h2Walk(f2::HeadSlot& S, uintptr_t pose, uint16_t joints, const HeadCache& C) noexcept {
    S.footL = C.footL;
    S.footR = C.footR;
    if (joints > f2::kMaxWalkJoints) {
        S.h2State = 3;
        return;
    }
    const uint64_t t0 = qpcNow();
    if (!sehReadPoseArrays(pose, joints, g_walkLocals, g_walkParents)) {
        S.h2State = 2;
        return;
    }
    const uint16_t idx[4] = {C.headIdx, S.povIdx, C.footL, C.footR};
    for (uint32_t j = 0; j < 4; ++j) {
        uint32_t depth = 0;
        if (idx[j] == f2::kNoJoint || !f2::walkJointToModel(g_walkLocals, g_walkParents, joints, idx[j], S.walked[j], &depth)) continue;
        S.walkOk = static_cast<uint8_t>(S.walkOk | (1u << j));
        S.depth[j] = static_cast<uint16_t>(depth);
    }
    S.h2State = 1;
    noteTimer(g_tWalk, t0, qpcNow());
}

// One attach site: its capture, the interface's identity, the joint indexes and the four matrices. True while H goes on (a site with nothing
// captured yet, or a stale capture, is not a failure).
bool headSite(uint32_t site, f2::HeadSlot& S) noexcept {
    HeadCache& C = g_headCache[site];
    const ExplorerCamSkeleton cap = explorerCamSkeleton(static_cast<int>(site));
    S.captures = cap.captures;
    const uint64_t iface = cap.iface;
    S.povIdx = static_cast<uint16_t>(cap.index & 0xFFFFu);
    if (iface == 0) {
        S.state = static_cast<uint8_t>(f2::HeadSlotState::NotCaptured);
        C = HeadCache();
        return true;
    }
    S.iface = iface;
    uint64_t fn[f2::kHeadSlots] = {};
    f2::HeadKind kind = f2::HeadKind::None;
    Verify v = headVerify(site, static_cast<uintptr_t>(iface), f2::HeadKind::None, fn, &kind);
    if (v == Verify::Down) return false;
    if (v == Verify::Stale) return headStale(site, iface, S);
    S.kind = static_cast<uint8_t>(kind);
    uint8_t flag = 0;
    if (!sehReadU8(static_cast<uintptr_t>(iface) + f2::headCachedFlagOffset(kind), &flag)) return headStale(site, iface, S);
    S.cached = flag;

    uintptr_t pose = 0;
    g_headCalls.fetch_add(1, std::memory_order_relaxed);
    if (!sehCallPose(static_cast<uintptr_t>(fn[0]), static_cast<uintptr_t>(iface), &pose)) return headFault(f2::HeadStage::PoseCall, site);
    uint16_t joints = 0;
    if (pose != 0 && !sehReadU16(pose, &joints)) return headFault(f2::HeadStage::PoseCall, site);
    S.joints = joints;
    S.state = static_cast<uint8_t>(f2::HeadSlotState::Resolved);
    if (pose == 0 || joints == 0) return true;   // the game's own matrix functions do nothing without a pose object: neither does H

    if (C.iface != iface) {   // a new interface: look the head joint up once (the povCamera index is the capture's)
        uint32_t r = f2::kNoJoint;
        v = headVerify(site, static_cast<uintptr_t>(iface), kind, fn, &kind);
        if (v == Verify::Down) return false;
        if (v == Verify::Stale) return headStale(site, iface, S);
        const uint64_t f0 = qpcNow();
        g_headCalls.fetch_add(1, std::memory_order_relaxed);
        const bool ok = sehCallFind(static_cast<uintptr_t>(fn[1]), static_cast<uintptr_t>(iface), f2::kHeadName, &r);
        noteTimer(g_tFind, f0, qpcNow());
        if (!ok) return headFault(f2::HeadStage::FindCall, site);
        C.iface = iface;
        C.headIdx = static_cast<uint16_t>(r & 0xFFFFu);
        C.footL = C.footR = f2::kNoJoint;
        if (g_h2On.load(std::memory_order_relaxed)) {
            for (int foot = 0; foot < 2; ++foot) {
                v = headVerify(site, static_cast<uintptr_t>(iface), kind, fn, &kind);
                if (v == Verify::Down) return false;
                if (v == Verify::Stale) return headStale(site, iface, S);
                const uint64_t f1 = qpcNow();
                g_headCalls.fetch_add(1, std::memory_order_relaxed);
                uint32_t fr = f2::kNoJoint;
                const bool fok = sehCallFind(static_cast<uintptr_t>(fn[1]), static_cast<uintptr_t>(iface), foot == 0 ? f2::kFootLName : f2::kFootRName, &fr);
                noteTimer(g_tFind, f1, qpcNow());
                if (!fok) return headFault(f2::HeadStage::FindCall, site);
                (foot == 0 ? C.footL : C.footR) = static_cast<uint16_t>(fr & 0xFFFFu);
            }
        }
    }
    S.headIdx = C.headIdx;
    const uint16_t index[2] = {C.headIdx, S.povIdx};
    for (uint32_t j = 0; j < 2; ++j) {
        if (index[j] == f2::kNoJoint || index[j] >= joints) continue;   // absent, or out of range: never handed to the game
        alignas(16) float buf[16] = {};
        v = headVerify(site, static_cast<uintptr_t>(iface), kind, fn, &kind);
        if (v == Verify::Down) return false;
        if (v == Verify::Stale) return headStale(site, iface, S);
        uint64_t t0 = qpcNow();
        g_headCalls.fetch_add(1, std::memory_order_relaxed);
        bool ok = sehCallMatrix(static_cast<uintptr_t>(fn[3]), static_cast<uintptr_t>(iface), index[j], buf);
        noteTimer(g_t58, t0, qpcNow());
        if (!ok) return headFault(f2::HeadStage::ModelCall, site);
        std::memcpy(S.model[j], buf, 64);
        S.have = static_cast<uint8_t>(S.have | (1u << j));
        std::memset(buf, 0, sizeof(buf));
        v = headVerify(site, static_cast<uintptr_t>(iface), kind, fn, &kind);
        if (v == Verify::Down) return false;
        if (v == Verify::Stale) return headStale(site, iface, S);
        t0 = qpcNow();
        g_headCalls.fetch_add(1, std::memory_order_relaxed);
        ok = sehCallMatrix(static_cast<uintptr_t>(fn[2]), static_cast<uintptr_t>(iface), index[j], buf);
        noteTimer(g_t48, t0, qpcNow());
        if (!ok) return headFault(f2::HeadStage::WorldCall, site);
        std::memcpy(S.world[j], buf, 64);
        S.have = static_cast<uint8_t>(S.have | (4u << j));
    }
    if (g_h2On.load(std::memory_order_relaxed)) h2Walk(S, pose, joints, C);
    return true;
}

// One evaluation, at most once a second, on the hook thread (g_busyH held).
void headEvaluate(const uint8_t* a, void* activity) noexcept {
    f2::HeadSample smp;
    smp.activity = reinterpret_cast<uint64_t>(activity);
    smp.steps = g_headSteps.fetch_add(1, std::memory_order_relaxed) + 1;
    if (!sehReadFrame(a, smp.local, smp.actWorld)) {
        headFault(f2::HeadStage::ReadFrame, 0);
        return;
    }
    // The dead route's diagnostic, non-fatal: what *(activity+0x368) really points at, for a later static pass.
    uint64_t h = 0;
    if (!sehReadQword(reinterpret_cast<uintptr_t>(a) + f2::kOffActivityHum, &h)) {
        headFault(f2::HeadStage::ReadActivity, 0);
        return;
    }
    smp.humH = h;
    if (h != 0) {
        smp.humFlags |= 1u;
        uint64_t v = 0;
        if (sehReadQword(static_cast<uintptr_t>(h), &v)) {
            smp.humAtH = v;
            smp.humFlags |= 2u;
        }
        if (h > f2::kHumFromInterface && sehReadQword(static_cast<uintptr_t>(h) - f2::kHumFromInterface, &v)) {
            smp.humBelow = v;
            smp.humFlags |= 4u;
        }
    }
    bool resolved = false, stale = false, captured = false;
    for (uint32_t s = 0; s < 2; ++s) {
        if (!headSite(s, smp.slot[s])) return;
        resolved = resolved || smp.slot[s].state == static_cast<uint8_t>(f2::HeadSlotState::Resolved);
        stale = stale || smp.slot[s].state == static_cast<uint8_t>(f2::HeadSlotState::Stale);
        captured = captured || smp.slot[s].captures != 0;
    }
    g_headLast.store(resolved ? "ok" : stale ? "waiting: a captured interface is stale" : captured ? "waiting: nothing resolved" : "waiting: no avatar attach seen yet",
                     std::memory_order_relaxed);
    g_headSample.tryPublish(smp);
}

void headMaybeStep(const uint8_t* a, void* activity) noexcept {
    const uint64_t now = GetTickCount64();
    if (now < g_headNextMs.load(std::memory_order_relaxed)) return;
    if (g_busyH.exchange(true, std::memory_order_acquire)) return;
    g_headNextMs.store(now + g_headIntervalMs, std::memory_order_relaxed);
    if (g_headState.load(std::memory_order_acquire) == 1) headEvaluate(a, activity);
    g_busyH.store(false, std::memory_order_release);
}

// The exe's SizeOfImage, from its own headers (0 when they do not read as a PE image).
size_t imageSizeOf(uintptr_t base) noexcept {
    uint32_t lfanew = 0, size = 0;
    uint16_t mz = 0;
    uint32_t pe = 0;
    if (!sehReadU16(base, &mz) || mz != 0x5A4D) return 0;
    if (!sehReadU32(base + 0x3C, &lfanew) || lfanew > 0x1000) return 0;
    if (!sehReadU32(base + lfanew, &pe) || pe != 0x4550) return 0;
    if (!sehReadU32(base + lfanew + 24 + 56, &size)) return 0;   // OptionalHeader.SizeOfImage (PE32+)
    return size;
}

// ---- the neck's replacement getter ---------------------------------------------------------------------------------------------
// Slot +0x20 of the eye interface's vtable: `lea rax,[rcx+268h]; ret`. This does what it does and a little more: it counts, notes the
// interface pointer, and keeps the pointer when the caller is the first-person camera activity's own read, which only ever reads
// the LOCAL commander's eye. No lock, no log, no allocation; it can be called from several threads and for every humanoid.
__declspec(noinline) void* __fastcall eyeGetterObserved(void* self) noexcept {
    const uintptr_t ret = reinterpret_cast<uintptr_t>(_ReturnAddress());
    g_neckCalls.fetch_add(1, std::memory_order_relaxed);
    if (g_neckSeen.findOrInsert(reinterpret_cast<uintptr_t>(self)) < 0) g_neckSeenOverflow.fetch_add(1, std::memory_order_relaxed);
    if (f2::isLocalEyeSite(ret, g_neck)) {
        g_neckLocalCalls.fetch_add(1, std::memory_order_relaxed);
        g_localEye.store(reinterpret_cast<uintptr_t>(self), std::memory_order_release);
    }
    return static_cast<uint8_t*>(self) + f2::kOffEyeMatrix;
}

// Replace one 8-byte slot: only those 8 bytes change protection, one aligned store, the protection back.
bool storeSlot(uintptr_t slot, uintptr_t value) {
    DWORD old = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(slot), 8, PAGE_READWRITE, &old)) return false;
    const bool ok = sehStoreQword(slot, value);
    DWORD ignored = 0;
    VirtualProtect(reinterpret_cast<void*>(slot), 8, old, &ignored);
    return ok;
}

bool installNeck(const f2::NeckTargets& t, char* why, size_t cap) {
    uint64_t current = 0;
    if (!sehReadQword(t.slot, &current)) {
        std::snprintf(why, cap, "the read of the eye vtable's slot at 0x%llX faulted; nothing was patched", static_cast<unsigned long long>(t.slot));
        return false;
    }
    if (!f2::neckSlotOk(current, t)) {
        std::snprintf(why, cap,
                      "the game build differs (slot +0x%X of the eye interface's vtable at 0x%llX holds 0x%llX, not build 332841's accessor at 0x%llX); "
                      "nothing was patched",
                      8u * f2::kEyeGetterSlot, static_cast<unsigned long long>(t.vtable), static_cast<unsigned long long>(current),
                      static_cast<unsigned long long>(t.getter));
        return false;
    }
    if (!sehBytesEqual(t.getter, f2::kEyeGetterCode, f2::kEyeGetterBytes)) {
        std::snprintf(why, cap, "the game build differs (the 8 bytes at 0x%llX are not `lea rax,[rcx+268h]; ret`); nothing was patched",
                      static_cast<unsigned long long>(t.getter));
        return false;
    }
    g_neck = t;
    if (!storeSlot(t.slot, reinterpret_cast<uintptr_t>(&eyeGetterObserved))) {
        g_neck = f2::NeckTargets();
        std::snprintf(why, cap, "the slot could not be written; nothing was patched");
        return false;
    }
    g_neckOriginal = static_cast<uintptr_t>(current);
    g_neckInstalled = true;
    return true;
}

// Put the original back, but only if the slot is still ours: another tool that replaced it after us owns it now.
void uninstallNeck() {
    if (!g_neckInstalled) return;
    uint64_t now = 0;
    if (sehReadQword(g_neck.slot, &now) && now == reinterpret_cast<uintptr_t>(&eyeGetterObserved)) storeSlot(g_neck.slot, g_neckOriginal);
    g_neckInstalled = false;
}
struct NeckGuard {
    ~NeckGuard() { uninstallNeck(); }
} g_neckGuard;

// ---- text and ticks --------------------------------------------------------------------------------------------------------------
void say(const ecm::Sink& sink, const char* fmt, ...) {
    char line[ecm::kLineBytes];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    sink(line);
}

struct F2Frame {
    bool started = false;
    ecp::Cadence sec, beat;
    uint64_t lastBeatMs = 0;
    uint64_t beatCtlCalls = 0, beatNeckCalls = 0, beatLocalCalls = 0, beatFadeCalls = 0;
    uint64_t seenLocalCalls = 0;
    int64_t lastLocalMs = -1;
    uint32_t seenPublishes = 0;
    int64_t lastSampleMs = -1;
    uint32_t staleSaid = 0;
    bool noPairNoted = false;
    // H
    bool headDownSaid = false;
    uint32_t seenHeadPublishes = 0;
    int64_t lastHeadMs = -1;
    bool slotSaid[2] = {false, false};
    uint64_t h2Iface[2] = {0, 0};              // H2's REST POSE line is said once per skeleton (and again when its state or what it walked changes)
    uint8_t h2State[2] = {0, 0}, h2WalkOk[2] = {0, 0};
    f2::HeadSlot slotKey[2];                   // what the last slot line said (compared on a few fields)
    bool humSaid = false;
    uint64_t humH = 0, humAtH = 0, humBelow = 0;
    uint32_t humFlags = 0;
    uint64_t beatHeadSteps = 0, beatHeadCalls = 0;
};
F2Frame g_f2;

// A slot line is due when its identity changes: state, kind, interface, joint count, cache flag, the two indexes (not the capture count, which may
// climb every second if the game attaches every frame).
bool slotChanged(const f2::HeadSlot& a, const f2::HeadSlot& b) {
    return a.state != b.state || a.kind != b.kind || a.iface != b.iface || a.joints != b.joints || a.cached != b.cached || a.headIdx != b.headIdx ||
           a.povIdx != b.povIdx;
}

}  // namespace

void explorerCamF2Arm(const ecm::Sink& sink) {
    if (g_armed.load(std::memory_order_acquire)) return;
    g_armed.store(true, std::memory_order_release);
    const bool first = !g_announced;
    g_announced = true;

    // I4: the controller observer rides explorer_cam.cpp's hook.
    const ExplorerCamHookStatus ctl = explorerCamObserve(ExplorerCamHook::Controller, &explorerCamF2Controller, true);
    if (ctl.state == ExplorerCamHookStatus::Armed) {
        g_i4Status = "armed";
        if (first)
            say(sink, "%s EliteDangerous64.exe+0x%llX (VesselCameraMountControl update, build 332841) hooked at 0x%llX, stolen=%zu bytes, relay at 0x%llX; "
                      "observed after the original and after every press is restored: mode +0x3E0 on every transition, pressed ints +0x310 +0x328 "
                      "+0x340 and preset kind +0x2E8 on change, and a 5 s heartbeat of the call count; read only",
                f2::prefixI4Armed(), static_cast<unsigned long long>(ecm::kControllerRva), static_cast<unsigned long long>(ctl.target), ctl.stolen,
                static_cast<unsigned long long>(ctl.relay));
    } else {
        g_i4Status = "stood down";
        if (first) say(sink, "%s %s. I4 will not run.", f2::prefixI4Down(), ctl.why);
    }

    // F: the avatar fade counter rides the dither-fade update's hook.
    {
        const ExplorerCamHookStatus fade = explorerCamObserve(ExplorerCamHook::AvatarFade, &explorerCamF2Fade, true);
        g_fadeGlobal = explorerCamFadeGlobalAddress();
        if (fade.state == ExplorerCamHookStatus::Armed) {
            g_fadeStatus = "armed";
            if (first)
                say(sink, "%s EliteDangerous64.exe+0x%llX (AvatarModelComponent's dither-fade update, build 332841) hooked at 0x%llX, stolen=%zu bytes, relay at "
                          "0x%llX; the original runs FIRST, then the component's dither block is read (comp+0x378 -> +0x90 enabled, +0x120 amount; comp+0x380 "
                          "eased); a 5 s heartbeat counts calls, distinct components, how many had enabled = 1 and the least amount among them. With Explorer "
                          "Cam holding the fade global at 0, enabled = 1 must read 0; read only",
                    f2::prefixFArmed(), static_cast<unsigned long long>(ecm::kAvatarFadeRva), static_cast<unsigned long long>(fade.target), fade.stolen,
                    static_cast<unsigned long long>(fade.relay));
        } else {
            g_fadeStatus = "stood down";
            if (first) say(sink, "%s %s. F will not run.", f2::prefixFDown(), fade.why);
        }
    }

    // H (route B): a FindJoint hook learns the interfaces the game attaches the local avatars to; the reads ride the free-camera observer. The build's
    // addresses are established once per session.
    {
        const uint32_t headState = g_headState.load(std::memory_order_acquire);
        if (headState == 0) {
            uintptr_t base = 0;
            size_t size = 0;
            char why[300] = {};
            bool known = false;
#ifdef EDVR_EXPLORER_CAM_TEST
            if (g_testHead.base) {
                base = g_testHead.base;
                size = g_testHead.imageSize;
                known = true;
            }
#endif
            if (!known) {
                known = explorerCamBuildKnown(&base, why, sizeof(why));
                if (known) {
                    size = imageSizeOf(base);
                    known = size != 0;
                }
            }
            if (!known) {
                headStandDown(f2::HeadWhy::BuildUnknown, 0, 0, 0, 0, false);
            } else {
                g_head = f2::headTargetsFromBase(base, size);
                const bool headOk = sehBytesEqual(g_head.headName, reinterpret_cast<const uint8_t*>(f2::kHeadName), sizeof(f2::kHeadName));
                const bool povOk = sehBytesEqual(g_head.povName, reinterpret_cast<const uint8_t*>(f2::kPovName), sizeof(f2::kPovName));
                const bool footOk = sehBytesEqual(g_head.footLName, reinterpret_cast<const uint8_t*>(f2::kFootLName), sizeof(f2::kFootLName)) &&
                                    sehBytesEqual(g_head.footRName, reinterpret_cast<const uint8_t*>(f2::kFootRName), sizeof(f2::kFootRName));
                g_h2On.store(footOk, std::memory_order_relaxed);
                if (!footOk) say(sink, "%s not run: the foot joint-name literals at EliteDangerous64.exe+0x%llX / +0x%llX are not \"%s\" / \"%s\"; H goes on without H2",
                                 f2::prefixH2Note(), static_cast<unsigned long long>(f2::kFootLNameRva), static_cast<unsigned long long>(f2::kFootRNameRva),
                                 f2::kFootLName, f2::kFootRName);
                if (!headOk || !povOk) {
                    headStandDown(f2::HeadWhy::LiteralMismatch, 0, headOk ? 2 : 1, 0, 0, false);
                } else {
                    const ExplorerCamHookStatus fj = explorerCamWantFindJoint(true);
                    if (fj.state != ExplorerCamHookStatus::Armed) {
                        headStandDown(f2::HeadWhy::FindHookDown, 0, 0, 0, 0, false, fj.why);
                    } else {
                        g_headNextMs.store(0, std::memory_order_relaxed);
                        g_headState.store(1, std::memory_order_release);
                        say(sink, "%s route B: FindJoint (EliteDangerous64.exe+0x%llX, many callers) hooked at 0x%llX, stolen=%zu bytes, relay at 0x%llX, installed NOW "
                                  "(an avatar attach before this is not seen); the original runs FIRST; a call returning to +0x%llX or +0x%llX (FUN 0x19B1240's two avatar "
                                  "attaches) with \"%s\" stores the interface (rcx) and the index. H reads those at most once a second on the camera-job thread; before "
                                  "EVERY call the vtable (RR +0x%llX or AO +0x%llX) and slots +0x18 +0x30 +0x48 +0x58 are checked; GetPoseData, FindJoint(\"%s\") once per "
                                  "interface, +0x58 and +0x48 for both joints of BOTH sites; H2 also walks the animated pose (P+0x48/+0x50) for head, pov and both feet; SEH throughout; "
                                  "nothing is written",
                            f2::prefixHArmed(), static_cast<unsigned long long>(ecm::kFindJointRva), static_cast<unsigned long long>(fj.target), fj.stolen,
                            static_cast<unsigned long long>(fj.relay), static_cast<unsigned long long>(f2::kFindSite1Rva),
                            static_cast<unsigned long long>(f2::kFindSite2Rva), f2::kPovName, static_cast<unsigned long long>(f2::kRrVtableRva),
                            static_cast<unsigned long long>(f2::kAoVtableRva), f2::kHeadName);
                    }
                }
            }
        } else if (headState == 1) {
            explorerCamWantFindJoint(true);   // the key was off and is on again: the hook is still in place, its probe share of the gate was closed
        }
    }

    // N: the neck's slot swap, once per session.
    if (!g_neckTried) {
        g_neckTried = true;
        char why[400] = {};
        f2::NeckTargets t;
        bool known = false;
#ifdef EDVR_EXPLORER_CAM_TEST
        if (g_testNeck.vtable) {
            t.vtable = g_testNeck.vtable;
            t.slot = g_testNeck.slot;
            t.getter = g_testNeck.getter;
            t.localSite = g_testNeck.localSite;
            known = true;
        }
#endif
        if (!known) {
            uintptr_t base = 0;
            if (explorerCamBuildKnown(&base, why, sizeof(why))) {
                t = f2::neckTargetsFromBase(base);
                known = true;
            }
        }
        if (known && installNeck(t, why, sizeof(why))) {
            g_neckStatus = "armed";
            say(sink, "%s the eye interface's vtable at EliteDangerous64.exe+0x%llX (build 332841): slot +0x%X (0x%llX, `lea rax,[rcx+268h]; ret`, shared by six "
                      "vtables, so the SLOT is replaced and the code is not hooked) now holds a getter that counts, notes the interface pointer and returns "
                      "exactly what the original returns. A call returning to 0x%llX is the first-person camera activity's own read, which only ever reads "
                      "the LOCAL commander's eye. Read from the free-camera hook's post-call and at 1 Hz; nothing is written to the game but that one slot",
                f2::prefixNArmed(), static_cast<unsigned long long>(f2::kEyeVtableRva), 8u * f2::kEyeGetterSlot, static_cast<unsigned long long>(t.getter),
                static_cast<unsigned long long>(t.localSite));
        } else {
            g_neckStatus = "stood down";
            say(sink, "%s %s. N will not run.", f2::prefixNDown(), why[0] ? why : "the build is unknown");
        }
    }
    g_f2.started = false;
}

void explorerCamF2Disarm() {
    if (!g_armed.exchange(false, std::memory_order_acq_rel)) return;
    explorerCamObserve(ExplorerCamHook::Controller, &explorerCamF2Controller, false);
    explorerCamObserve(ExplorerCamHook::AvatarFade, &explorerCamF2Fade, false);
    explorerCamWantFindJoint(false);
    g_f2.started = false;
}

void explorerCamF2FreeCamera(void* activity) noexcept {
    if (!activity || !g_armed.load(std::memory_order_acquire)) return;
    if (g_busy3.exchange(true, std::memory_order_acquire)) return;
    const uint8_t* const a = static_cast<const uint8_t*>(activity);
    const uint64_t call = g_freeCalls.fetch_add(1, std::memory_order_relaxed) + 1;

    f2::Pressed3 p;
    p.a = sehPressedAt(a, ecm::kOffToggleRotationAction);
    p.b = sehPressedAt(a, ecm::kOffWorldFixAction);
    p.c = sehPressedAt(a, ecm::kOffLockAction);
    if (!g_have3 || !(p == g_last3)) {
        f2::F2Event e;
        e.seq = g_f2Seq.fetch_add(1, std::memory_order_relaxed);
        e.kind = static_cast<uint32_t>(f2::F2Kind::Pressed3);
        e.object = reinterpret_cast<uint64_t>(activity);
        e.call = call;
        e.threadId = GetCurrentThreadId();
        e.a = p.a; e.b = p.b; e.c = p.c;
        e.pa = g_last3.a; e.pb = g_last3.b; e.pc = g_last3.c;
        e.first = g_have3 ? 0u : 1u;
        g_ringFree.push(e);
        g_last3 = p;
        g_have3 = true;
    }

    // The neck, paired with this update's poses: only once the local eye interface is known.
    const uintptr_t iface = g_localEye.load(std::memory_order_acquire);
    if (iface) {
        f2::NeckSample s;
        s.activity = reinterpret_cast<uint64_t>(activity);
        s.iface = iface;
        s.localSiteCalls = g_neckLocalCalls.load(std::memory_order_relaxed);
        bool bad = false;
        uint64_t badVptr = 0;
        if (sehReadNeckSample(a, iface, g_neck, &s, &bad, &badVptr)) {
            g_sample.tryPublish(s);
        } else if (bad) {
            uintptr_t expected = iface;
            if (g_localEye.compare_exchange_strong(expected, 0, std::memory_order_acq_rel)) {
                g_staleVptr.store(badVptr, std::memory_order_relaxed);
                g_staleCount.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }
    g_busy3.store(false, std::memory_order_release);
    // H: after the try-flag is given back, so the lock-taking calls never keep the other instruments out.
    headMaybeStep(a, activity);
}

void explorerCamF2Fade(void* component) noexcept {
    if (!component || !g_armed.load(std::memory_order_acquire)) return;
    g_fadeCalls.fetch_add(1, std::memory_order_relaxed);
    if (g_fadeSeen.findOrInsert(reinterpret_cast<uintptr_t>(component)) < 0) g_fadeSeenOverflow.fetch_add(1, std::memory_order_relaxed);
    uint8_t enabled = 0;
    float amount = 0, eased = 0;
    const int how = sehReadFade(static_cast<const uint8_t*>(component), &enabled, &amount, &eased);
    if (how == 2) {
        g_fadeReadFaults.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (how == 1) {
        g_fadeNoBlock.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (enabled == 0) return;
    g_fadeEnabledCalls.fetch_add(1, std::memory_order_relaxed);
    g_fadeEnabledWindow.fetch_add(1, std::memory_order_relaxed);
    g_fadeEnabledSet.note(reinterpret_cast<uintptr_t>(component));
    uint32_t bits = 0;
    std::memcpy(&bits, &amount, 4);
    uint32_t cur = g_fadeMinBits.load(std::memory_order_relaxed);
    for (;;) {
        float curValue = 0;
        std::memcpy(&curValue, &cur, 4);
        if (!(amount < curValue)) break;
        if (g_fadeMinBits.compare_exchange_weak(cur, bits, std::memory_order_relaxed)) break;
    }
    int32_t global = 0;
    if (g_fadeGlobal && sehReadGlobal(g_fadeGlobal, &global) && global == 0) {
        g_fadeEnabledWhileZero.fetch_add(1, std::memory_order_relaxed);
        g_fadeEnabledWhileZeroWindow.fetch_add(1, std::memory_order_relaxed);
    }
}

void explorerCamF2Controller(void* controller) noexcept {
    if (!controller || !g_armed.load(std::memory_order_acquire)) return;
    const uint64_t call = g_ctlCalls.fetch_add(1, std::memory_order_relaxed) + 1;
    g_ctlLast.store(reinterpret_cast<uint64_t>(controller), std::memory_order_relaxed);
    if (g_busyCtl.exchange(true, std::memory_order_acquire)) return;
    const uint8_t* const c = static_cast<const uint8_t*>(controller);
    f2::ControllerSnap s;
    s.controller = reinterpret_cast<uint64_t>(controller);
    s.call = call;
    if (sehReadController(c, &s)) {
        s.photo = sehPressedAt(c, ecm::kOffCtlPhotoAction);
        s.free = sehPressedAt(c, ecm::kOffCtlFreeAction);
        s.quit = sehPressedAt(c, ecm::kOffCtlQuitAction);
        g_ctlMode.store(s.mode, std::memory_order_relaxed);
        if (!g_haveCtl || !s.sameAs(g_lastCtl)) {
            f2::F2Event e;
            e.seq = g_f2Seq.fetch_add(1, std::memory_order_relaxed);
            e.kind = static_cast<uint32_t>(f2::F2Kind::Controller);
            e.object = s.controller;
            e.call = call;
            e.threadId = GetCurrentThreadId();
            e.a = s.photo; e.b = s.free; e.c = s.quit;
            e.mode = s.mode;
            e.prevMode = g_haveCtl ? g_lastCtl.mode : s.mode;
            e.presetKind = s.presetKind;
            e.first = g_haveCtl ? 0u : 1u;
            g_ringCtl.push(e);
            g_lastCtl = s;
            g_haveCtl = true;
        }
    }
    g_busyCtl.store(false, std::memory_order_release);
}

void explorerCamF2Tick(uint32_t frame, uint64_t nowMs, const ecm::Sink& sink) {
    if (!g_armed.load(std::memory_order_acquire)) return;
    F2Frame& fs = g_f2;
    char line[ecm::kLineBytes];
    if (!fs.started) {
        fs.started = true;
        fs.sec.periodMs = 1000;
        fs.sec.reset(nowMs);
        fs.beat.periodMs = 5000;
        fs.beat.reset(nowMs);
        fs.lastBeatMs = nowMs;
        fs.beatCtlCalls = g_ctlCalls.load(std::memory_order_relaxed);
        fs.beatNeckCalls = g_neckCalls.load(std::memory_order_relaxed);
        fs.beatLocalCalls = g_neckLocalCalls.load(std::memory_order_relaxed);
        fs.seenLocalCalls = fs.beatLocalCalls;
        fs.seenPublishes = g_sample.publishes();
    }

    // 1. What the observers saw change, in order.
    {
        f2::F2Event events[2 * 64];
        size_t count = 0;
        for (auto* ring : {&g_ringFree, &g_ringCtl})
            while (count < sizeof(events) / sizeof(events[0]) && ring->take(&events[count])) ++count;
        std::sort(events, events + count, [](const f2::F2Event& x, const f2::F2Event& y) { return x.seq < y.seq; });
        for (size_t i = 0; i < count; ++i) {
            f2::formatF2Event(line, sizeof(line), events[i], frame);
            sink(line);
        }
    }

    // 2. A stale interface pointer, said once each time it happens.
    const uint32_t stale = g_staleCount.load(std::memory_order_relaxed);
    if (stale != fs.staleSaid) {
        fs.staleSaid = stale;
        say(sink, "%s the local eye interface pointer's vtable is 0x%llX, not the eye vtable's 0x%llX: it was stale, so it is cleared and re-learned the "
                  "next time the first-person camera activity reads the eye (stale count %u)",
            f2::prefixNStale(), static_cast<unsigned long long>(g_staleVptr.load(std::memory_order_relaxed)), static_cast<unsigned long long>(g_neck.vtable),
            stale);
    }

    // 3. When the local site was last called, and when the paired sample last moved.
    const uint64_t localCalls = g_neckLocalCalls.load(std::memory_order_relaxed);
    if (localCalls != fs.seenLocalCalls) {
        fs.seenLocalCalls = localCalls;
        fs.lastLocalMs = static_cast<int64_t>(nowMs);
    }
    const uint32_t publishes = g_sample.publishes();
    if (publishes != fs.seenPublishes) {
        fs.seenPublishes = publishes;
        fs.lastSampleMs = static_cast<int64_t>(nowMs);
    }

    // 4. 1 Hz, while the free camera activity exists or the local site was called in the last second.
    if (fs.sec.due(nowMs) && std::strcmp(g_neckStatus, "armed") == 0) {
        const bool activity = fs.lastSampleMs >= 0 && static_cast<int64_t>(nowMs) - fs.lastSampleMs <= 1500;
        const bool local = fs.lastLocalMs >= 0 && static_cast<int64_t>(nowMs) - fs.lastLocalMs <= 1000;
        const uintptr_t iface = g_localEye.load(std::memory_order_acquire);
        if ((activity || local) && iface) {
            const char* phase = activity ? "free-camera" : "local-site";
            EyeRead r;
            bool bad = false;
            uint64_t badVptr = 0;
            if (sehReadEyeInterface(iface, g_neck, &r, &bad, &badVptr)) {
                f2::formatNeckMatrix(line, sizeof(line), "+0x268(eye)", iface, r.a, phase);
                sink(line);
                f2::formatNeckMatrix(line, sizeof(line), "+0x1A8", iface, r.b, phase);
                sink(line);
                f2::formatNeckMatrix(line, sizeof(line), "+0x1E8", iface, r.c, phase);
                sink(line);
                f2::formatNeckMatrix(line, sizeof(line), "+0x228", iface, r.d, phase);
                sink(line);
                f2::formatNeckVec(line, sizeof(line), iface, r.v, phase);
                sink(line);
            } else if (bad) {
                uintptr_t expected = iface;
                if (g_localEye.compare_exchange_strong(expected, 0, std::memory_order_acq_rel)) {
                    g_staleVptr.store(badVptr, std::memory_order_relaxed);
                    g_staleCount.fetch_add(1, std::memory_order_relaxed);
                }
            }
        }
        if (activity) {
            f2::NeckSample s;
            if (g_sample.read(s)) {
                const f2::CommanderFrame cf = f2::commanderFrame(s.local, s.world);
                float eyeLocal[3] = {0, 0, 0};
                if (cf.valid) f2::worldToCommanderLocal(cf, s.eye + 12, eyeLocal);
                f2::formatNeckLocal(line, sizeof(line), s, cf, eyeLocal, static_cast<int64_t>(nowMs) - fs.lastSampleMs);
                sink(line);
            }
        } else if (local && fs.lastSampleMs < 0 && !fs.noPairNoted) {
            fs.noPairNoted = true;
            say(sink, "%s the first-person camera activity reads the eye, but no free-camera update has paired a sample with it yet, so the "
                      "commander-local figure waits for the free camera (TAB, or F5)",
                f2::prefixNLocal());
        }
    }

    // 4b. H: the stand-down, said once; the hum line when what *(activity+0x368) points at changes; the slot lines when a site's identity changes; the
    // joint lines each time the hook thread published (at most 1 Hz).
    if (g_headDownSet.load(std::memory_order_acquire) != 0 && !fs.headDownSaid) {
        fs.headDownSaid = true;
        f2::formatHeadDown(line, sizeof(line), g_headDown, g_head);
        sink(line);
    }
    {
        const uint32_t headPublishes = g_headSample.publishes();
        if (headPublishes != fs.seenHeadPublishes) {
            fs.seenHeadPublishes = headPublishes;
            fs.lastHeadMs = static_cast<int64_t>(nowMs);
            f2::HeadSample hs;
            if (g_headSample.read(hs)) {
                if ((hs.humFlags & 1u) != 0 &&
                    (!fs.humSaid || hs.humH != fs.humH || hs.humAtH != fs.humAtH || hs.humBelow != fs.humBelow || hs.humFlags != fs.humFlags)) {
                    fs.humSaid = true;
                    fs.humH = hs.humH;
                    fs.humAtH = hs.humAtH;
                    fs.humBelow = hs.humBelow;
                    fs.humFlags = hs.humFlags;
                    f2::formatHeadHum(line, sizeof(line), hs, g_head);
                    sink(line);
                }
                const f2::CommanderFrame cf = f2::commanderFrame(hs.local, hs.actWorld);
                for (int i = 0; i < 2; ++i) {
                    if (!fs.slotSaid[i] || slotChanged(hs.slot[i], fs.slotKey[i])) {
                        fs.slotSaid[i] = true;
                        fs.slotKey[i] = hs.slot[i];
                        f2::formatHeadSlot(line, sizeof(line), hs.slot[i], i);
                        sink(line);
                    }
                    if (hs.slot[i].state == static_cast<uint8_t>(f2::HeadSlotState::Resolved) && hs.slot[i].have != 0) {
                        f2::formatHeadJoints(line, sizeof(line), hs, i, cf);
                        sink(line);
                    }
                    // H2 walks the BIND pose (F7), which never moves: one line per skeleton, and another only when its state or what it could walk changes.
                    if (hs.slot[i].state == static_cast<uint8_t>(f2::HeadSlotState::Resolved) && hs.slot[i].h2State != 0 &&
                        (hs.slot[i].iface != fs.h2Iface[i] || hs.slot[i].h2State != fs.h2State[i] || hs.slot[i].walkOk != fs.h2WalkOk[i])) {
                        fs.h2Iface[i] = hs.slot[i].iface;
                        fs.h2State[i] = hs.slot[i].h2State;
                        fs.h2WalkOk[i] = hs.slot[i].walkOk;
                        f2::formatH2Joints(line, sizeof(line), hs, i);
                        sink(line);
                    }
                }
            }
        }
    }

    // 5. The 5 s heartbeats: one per instrument, always, so a zero is distinguishable from not running.
    if (fs.beat.due(nowMs)) {
        const double seconds = static_cast<double>(nowMs - fs.lastBeatMs) / 1000.0;
        f2::I4HeartbeatIn h4;
        h4.windowSeconds = seconds;
        h4.hook = g_i4Status;
        h4.callsTotal = g_ctlCalls.load(std::memory_order_relaxed);
        h4.callsWindow = h4.callsTotal - fs.beatCtlCalls;
        h4.mode = g_ctlMode.load(std::memory_order_relaxed);
        h4.lastController = g_ctlLast.load(std::memory_order_relaxed);
        h4.lost = g_ringCtl.lost();
        f2::formatI4Heartbeat(line, sizeof(line), h4);
        sink(line);
        fs.beatCtlCalls = h4.callsTotal;

        f2::NeckHeartbeatIn hn;
        hn.windowSeconds = seconds;
        hn.hook = g_neckStatus;
        hn.calls = g_neckCalls.load(std::memory_order_relaxed);
        hn.callsWindow = hn.calls - fs.beatNeckCalls;
        hn.localSiteCalls = g_neckLocalCalls.load(std::memory_order_relaxed);
        hn.localSiteWindow = hn.localSiteCalls - fs.beatLocalCalls;
        size_t distinct = 0;
        for (size_t i = 0; i < 32; ++i) distinct += g_neckSeen.key(i) != 0 ? 1 : 0;
        hn.distinctInterfaces = distinct;
        hn.distinctOverflow = g_neckSeenOverflow.load(std::memory_order_relaxed);
        hn.localEye = g_localEye.load(std::memory_order_relaxed);
        hn.localEyeSet = hn.localEye != 0;
        hn.localSiteAgeMs = fs.lastLocalMs < 0 ? -1 : static_cast<int64_t>(nowMs) - fs.lastLocalMs;
        f2::formatNeckHeartbeat(line, sizeof(line), hn);
        sink(line);

        f2::FadeHeartbeatIn hf;
        hf.windowSeconds = seconds;
        hf.hook = g_fadeStatus;
        hf.calls = g_fadeCalls.load(std::memory_order_relaxed);
        hf.callsWindow = hf.calls - fs.beatFadeCalls;
        fs.beatFadeCalls = hf.calls;
        size_t distinctFade = 0;
        for (size_t i = 0; i < 64; ++i) distinctFade += g_fadeSeen.key(i) != 0 ? 1 : 0;
        hf.distinctComponents = distinctFade;
        hf.distinctOverflow = g_fadeSeenOverflow.load(std::memory_order_relaxed);
        hf.enabledCalls = g_fadeEnabledCalls.load(std::memory_order_relaxed);
        hf.enabledWindow = g_fadeEnabledWindow.exchange(0, std::memory_order_relaxed);
        uint64_t enabledComponents[64];
        hf.componentsEnabledWindow = g_fadeEnabledSet.drain(enabledComponents, 64);
        const uint32_t minBits = g_fadeMinBits.exchange(0x7F800000u, std::memory_order_relaxed);
        std::memcpy(&hf.minAmount, &minBits, 4);
        hf.haveMin = minBits != 0x7F800000u;
        int32_t global = 0;
        hf.globalKnown = g_fadeGlobal && sehReadGlobal(g_fadeGlobal, &global);
        hf.global = global;
        hf.enabledWhileZero = g_fadeEnabledWhileZero.load(std::memory_order_relaxed);
        hf.enabledWhileZeroWindow = g_fadeEnabledWhileZeroWindow.exchange(0, std::memory_order_relaxed);
        hf.noBlock = g_fadeNoBlock.load(std::memory_order_relaxed);
        hf.readFaults = g_fadeReadFaults.load(std::memory_order_relaxed);
        f2::formatFadeHeartbeat(line, sizeof(line), hf);
        sink(line);

        f2::HeadHeartbeatIn hh;
        hh.windowSeconds = seconds;
        const uint32_t headState = g_headState.load(std::memory_order_acquire);
        hh.state = headState == 1 ? "armed" : headState == 2 ? "stood down" : "not tried";
        hh.steps = g_headSteps.load(std::memory_order_relaxed);
        hh.stepsWindow = hh.steps - fs.beatHeadSteps;
        fs.beatHeadSteps = hh.steps;
        hh.calls = g_headCalls.load(std::memory_order_relaxed);
        hh.callsWindow = hh.calls - fs.beatHeadCalls;
        fs.beatHeadCalls = hh.calls;
        hh.faults = g_headFaults.load(std::memory_order_relaxed);
        hh.last = g_headLast.load(std::memory_order_relaxed);
        hh.captures[0] = explorerCamSkeleton(0).captures;
        hh.captures[1] = explorerCamSkeleton(1).captures;
        hh.latches = explorerCamSkeleton(0).latches;
        hh.findSeen = explorerCamFindJointSeen();
        hh.m58 = drainTimer(g_t58);
        hh.m48 = drainTimer(g_t48);
        hh.find = drainTimer(g_tFind);
        hh.walk = drainTimer(g_tWalk);
        f2::formatHeadHeartbeat(line, sizeof(line), hh);
        sink(line);
        fs.beatNeckCalls = hn.calls;
        fs.beatLocalCalls = hn.localSiteCalls;
        fs.lastBeatMs = nowMs;
    }
}

#ifdef EDVR_EXPLORER_CAM_TEST
namespace explorercamf2test {
void setNeckTargets(const NeckSeam& t) { g_testNeck = t; }
void setHeadTargets(const HeadSeam& t) { g_testHead = t; }
void setHeadInterval(uint32_t ms) { g_headIntervalMs = ms; }
uint64_t headSteps() { return g_headSteps.load(); }
uint64_t headCalls() { return g_headCalls.load(); }
uint64_t headFaults() { return g_headFaults.load(); }
uint32_t headState() { return g_headState.load(); }
uint64_t findSeen() { return explorerCamFindJointSeen(); }
uint32_t captureCount(int site) { return explorerCamSkeleton(site).captures; }
uint64_t capturedInterface(int site) { return explorerCamSkeleton(site).iface; }
uint32_t capturedIndex(int site) { return explorerCamSkeleton(site).index; }
uint64_t fadeCalls() { return g_fadeCalls.load(); }
uint64_t fadeEnabledCalls() { return g_fadeEnabledCalls.load(); }
uint64_t fadeEnabledWhileZero() { return g_fadeEnabledWhileZero.load(); }
size_t fadeDistinct() {
    size_t n = 0;
    for (size_t i = 0; i < 64; ++i) n += g_fadeSeen.key(i) != 0 ? 1 : 0;
    return n;
}
uint64_t neckCalls() { return g_neckCalls.load(); }
uint64_t neckLocalSiteCalls() { return g_neckLocalCalls.load(); }
uintptr_t neckLocalEye() { return g_localEye.load(); }
size_t neckDistinct() {
    size_t n = 0;
    for (size_t i = 0; i < 32; ++i) n += g_neckSeen.key(i) != 0 ? 1 : 0;
    return n;
}
bool neckInstalled() { return g_neckInstalled; }
uintptr_t neckReplacement() { return reinterpret_cast<uintptr_t>(&eyeGetterObserved); }
void reset() {
    uninstallNeck();
    g_armed.store(false);
    g_busy3.store(false);
    g_last3 = f2::Pressed3();
    g_have3 = false;
    g_freeCalls.store(0);
    g_busyCtl.store(false);
    g_lastCtl = f2::ControllerSnap();
    g_haveCtl = false;
    g_ctlCalls.store(0);
    g_ctlMode.store(0);
    g_ctlLast.store(0);
    g_f2Seq.store(0);
    g_neck = f2::NeckTargets();
    g_neckCalls.store(0);
    g_neckLocalCalls.store(0);
    g_localEye.store(0);
    g_neckSeenOverflow.store(0);
    g_staleCount.store(0);
    g_staleVptr.store(0);
    g_neckTried = false;
    g_neckOriginal = 0;
    g_i4Status = "not tried";
    g_neckStatus = "not tried";
    g_announced = false;
    g_testNeck = NeckSeam();
    g_f2 = F2Frame();
    g_fadeCalls.store(0);
    g_fadeEnabledCalls.store(0);
    g_fadeEnabledWindow.store(0);
    g_fadeNoBlock.store(0);
    g_fadeReadFaults.store(0);
    g_fadeEnabledWhileZero.store(0);
    g_fadeEnabledWhileZeroWindow.store(0);
    g_fadeMinBits.store(0x7F800000u);
    g_fadeSeen.clear();
    g_fadeSeenOverflow.store(0);
    {
        uint64_t drop[64];
        g_fadeEnabledSet.drain(drop, 64);
    }
    g_fadeGlobal = nullptr;
    g_fadeStatus = "not tried";
    g_neckSeen.clear();
    g_sample.clear();
    g_busyH.store(false);
    g_head = f2::HeadTargets();
    g_headState.store(0);
    g_headSteps.store(0);
    g_headCalls.store(0);
    g_headFaults.store(0);
    g_headNextMs.store(0);
    g_headLast.store("none");
    g_headDown = f2::HeadDown();
    g_headDownSet.store(0);
    g_headDownClaim.store(0);
    g_headSample.clear();
    g_headCache[0] = HeadCache();
    g_headCache[1] = HeadCache();
    g_h2On.store(false);
    for (TimerStat* t : {&g_t58, &g_t48, &g_tFind, &g_tWalk}) {
        t->n.store(0);
        t->minUs.store(0xFFFFFFFFu);
        t->maxUs.store(0);
        t->allMaxUs.store(0);
    }
    g_headIntervalMs = f2::kHeadIntervalMs;
    g_testHead = HeadSeam();
    f2::F2Event drop;
    while (g_ringFree.take(&drop)) {}
    while (g_ringCtl.take(&drop)) {}
}
}  // namespace explorercamf2test
#endif

}  // namespace edvr
