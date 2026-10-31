// Native on foot's avatar hooks: the glue (native_on_foot_hooks.h says what they are; avatar_hooks_core.h and avatar_skeleton_core.h hold the
// decisions, the tables and the bytes of the relay, driven by tools\native_on_foot_hooks_test).
//
// WHAT RUNS WHERE.
//   controller    controllerHooked: the game's update, then the mode byte is read and published for native on foot's camera signal (F93d).
//   avatar fade   avatarFadeHooked: the game's dither fade, then the head hide, the neck census, the first-person body and the clip modifiers, and
//                 the F3 avatar census, on the game's submit job thread.
//   FindJoint     findJointHooked: the game's FindJoint, then the capture of the local avatar's skeleton pair. Nothing is written.
//   frame thread  nativeOnFootHooksFrameBoundary, the Present boundary: installs the hooks when native on foot wants them, opens their gates, holds
//                 the avatar dither-fade global for native stereo's body, and is the only code here that logs.
// Every hook thread takes no lock it can wait on (a try-flag keeps a second concurrent caller out where one matters), allocates nothing and
// writes no log line. Every access to the game's memory is under SEH.
#include <algorithm>
#include <array>
#include <limits>
#include <vector>
#include <mutex>

#include "native_on_foot_hooks.h"
#include "avatar_hooks_core.h"
#include "avatar_skeleton_core.h"
#include "native_on_foot.h"   // nativeOnFootActive, nativeOnFootCameraExternal and the pose freeze's hand-off (pfz)
#include "native_on_foot_f95c.h"   // F95c: the head's shadow-only class through the visibility call, the fade hook's action
#include "native_on_foot_f180.h"   // F180b: the spine straighten's plan (the spine and the neck as held joints)
#include "native_on_foot_f142.h"   // F142: the full outfit's promotion through the same call, and the outfit census
#include "native_on_foot_f143.h"   // F143: the clip measure's schedule, the clip snapshot
#include "native_on_foot_f144.h"   // F144: Gear_Back only for an outer garment, the first-person census
#include "native_on_foot_logwin.h"   // F125: the detail key's cached value (the 5 s probe lines here are the detail log's)

#include <windows.h>
#include <intrin.h>

#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>

#include "../common/code_hook.h"
#include "../common/log.h"

namespace edvr {
namespace {

// ---- the three hooks ------------------------------------------------------------------------------------------------------------
enum Hk : int { kHkCtl = 0, kHkFade, kHkFind, kHkCount };
struct HookSpec {
    const char* label;      // the log's name for it
    const char* what;       // what the prologue belongs to
    const char* tag;        // CodeHook's name for it
    uintptr_t rva;
    const uint8_t* prologue;
    size_t prologueBytes;
};
const HookSpec kSpec[kHkCount] = {
    {"controller", "VesselCameraMountControl update", "avatar-hooks-controller", avh::kControllerRva, avh::kControllerPrologue,
     avh::kControllerPrologueBytes},
    {"avatar-fade", "AvatarModelComponent dither-fade update", "avatar-hooks-avatar-fade", avh::kAvatarFadeRva, avh::kAvatarFadePrologue,
     avh::kAvatarFadePrologueBytes},
    {"find-joint", "skeleton interface FindJoint(name)", "avatar-hooks-find-joint", avh::kFindJointRva, avh::kFindJointPrologue,
     avh::kFindJointPrologueBytes},
};

// ---- state shared between the threads --------------------------------------------------------------------------------------
alignas(8) std::atomic<uintptr_t> g_gate[kHkCount];     // a callback relay's gate: open = the callback runs, closed = straight on
std::atomic<uintptr_t> g_forward[kHkCount];             // a callback relay's trampoline
// The FindJoint hook's capture. 0x19B3F40 attaches EVERY humanoid's avatars (F6: site 1 alone is whichever humanoid ran last, usually an NPC). Only the
// local player has a first-person avatar, so a pair is latched as THE local avatar when one thread makes a site-1 attach and then a site-2 attach at the same
// stack location (one invocation does both, from one frame, with no other site-1 attach on that thread between). Everything that wants "the local
// third-person skeleton" reads the latch and never a raw capture.
alignas(8) std::atomic<uint64_t> g_skelIface[2];     // LATCHED: [0] the local third-person skeleton, [1] the local first-person one (0 = none yet / dropped)
std::atomic<uint32_t> g_skelIndex[2];                // the povCamera index the original returned for each, at the latch
std::atomic<uint32_t> g_skelCaptures[2];             // RAW attaches seen at site 1 / site 2 (every humanoid's), for the log
std::atomic<uint32_t> g_skelLatches{0};              // times a pair was latched
std::atomic<uint64_t> g_findSeen{0};
struct Site1Seen {                                   // this thread's last site-1 attach, waiting for its site 2
    uint64_t iface = 0;
    uint32_t index = 0;
    uintptr_t where = 0;                             // the stack slot of the return address, identical for the two calls of one invocation
    bool valid = false;
};
thread_local Site1Seen t_site1;
std::atomic<uintptr_t> g_findSite[2], g_findPov{0};   // the two return addresses and the literal's address; set before the gate can open

// Phase 2: head hiding (hook threads write; the frame thread reads and logs).
std::atomic<bool> g_hideOn{false};                     // the fade hook's post-call hides: active, both hooks armed, the part names verified
std::atomic<bool> g_hideDown{false};                   // stood down for the session: guarded accesses of the AMC faulted
std::atomic<uint32_t> g_hideFaults{0};
std::atomic<int> g_partNames{0};                       // 0 not checked, 1 the exe's name table matches kPartNames, 2 it does not
std::atomic<uint64_t> g_amcCalls{0}, g_amcLocalMatches{0}, g_amcIdOk{0}, g_amcMode3{0}, g_hideCalls{0}, g_hideZeroed{0};
std::atomic<uint64_t> g_amcLocal{0}, g_amcLocalPose{0}, g_amcCand{0}, g_amcCandPose{0};
std::atomic<bool> g_busyAmc{false};
uint64_t g_amcCensused = 0;                            // g_busyAmc
avh::Ring<avh::AmcCensus, 2> g_censusRing;
// F95c: the head kept in the shadows (native_on_foot_f95c.h). The visibility job's hook writes the ticket after the game's call (End) and withdraws it before the
// AMC's next call (Begin); the fade hook's post-call takes it. The counters are cumulative; the frame thread logs their change once a second.
std::atomic<uint64_t> g_f95cTicket{0};   // the AMC whose latest visibility call ran with the head as shadow-only
std::atomic<uint64_t> g_f95cDeny{0};     // ...that call's main and other view bits
std::atomic<uint64_t> g_f95cCalls{0}, g_f95cSwapped{0}, g_f95cParts{0}, g_f95cNoViews{0}, g_f95cLost{0}, g_f95cShadowPairs{0}, g_f95cEyePairs{0}, g_f95cKept{0},
    g_f95cKeptWords{0}, g_f95cRemoved{0}, g_f95cZeroed{0};
std::atomic<uint32_t> g_f95cFaults{0};
std::atomic<bool> g_f95cDown{false};
std::mutex g_f95cViewMutex;              // guards g_f95cViews (the hook only try-locks it)
NativeOnFootHeadShadowViews g_f95cViews;
// F142 (always on since F146, while the head hide writes): the outfit's non-head shadow-only parts as every-view for the visibility call (native_on_foot_f142.h). The hook
// counts; the frame thread logs the change. The census (taken in the fade hook's step, the same thread as the head hide) goes to the frame thread by a ring.
std::atomic<uint64_t> g_f142Calls{0}, g_f142Swapped{0}, g_f142Promoted{0}, g_f142Refused{0}, g_f142Lost{0};
std::atomic<uint64_t> g_f142Part[avh::kPartCount];   // static storage: zero
std::atomic<uint32_t> g_f142Faults{0};
std::atomic<bool> g_f142Down{false};
std::atomic<bool> g_f142CensusDown{false};            // three faults reading the census: it stops (the full outfit is unaffected)
std::atomic<uint32_t> g_f142CensusFaults{0};
std::atomic<bool> g_f142Busy{false};                  // one thread at a time takes the census
std::atomic<uint64_t> g_f142CensusMs{0};
uint64_t g_f142Sig = 0;                               // g_f142Busy: the signature of the last census queued
bool g_f142HaveSig = false;                           // g_f142Busy: a census was queued
avh::Ring<nofoot::f142::Census, 4> g_f142CensusRing;
// F143: the clip measure (F147: at the first call, when the outfit changes, and when the armed state first changes to a state in this outfit; capped) and the extra unclip's writes.
// The frame thread sets the armed state; the fade hook counts and queues.
std::atomic<uint8_t> g_f143Armed{0};
std::atomic<int> g_f143SeenArmed{-1};                 // the armed state at the last snapshot (-1: none yet)
std::atomic<uint32_t> g_f143SeenMask{0};              // the armed states read in this outfit (bit 0 unarmed, 1 weapon, 2 tool)
std::atomic<uint32_t> g_f142OutfitGen{0};             // bumped each time the census queues a change: a new outfit as far as the clip measure is concerned
std::atomic<uint32_t> g_f143SeenGen{0};               // g_f142OutfitGen at the last snapshot
std::atomic<uint32_t> g_f143Reads{0};                 // snapshots taken this session
std::atomic<uint32_t> g_f143Faults{0};
std::atomic<bool> g_f143Down{false};
std::atomic<bool> g_f143Busy{false};
std::atomic<uint64_t> g_f143ClipPart[avh::kPartCount];   // static storage: zero
std::atomic<uint64_t> g_f143ClipWrites{0}, g_f143ClipFaults{0};
avh::Ring<nofoot::f143::ClipSnap, 4> g_f143Ring;
// F144: Gear_Back's decision (the last one made, for the frame thread's line) and the first-person census.
std::atomic<int> g_f144Gear{0};
std::atomic<bool> g_f144FpBusy{false};
std::atomic<uint64_t> g_f144FpMs{0};
std::atomic<uint32_t> g_f144FpFaults{0};
uint64_t g_f144FpSig = 0;                             // g_f144FpBusy: the signature of the last first-person census queued
bool g_f144FpHaveSig = false;
avh::Ring<nofoot::f144::FpCensus, 4> g_f144FpRing;
constexpr uint32_t kF143ReadCap = 24;                 // snapshots a session (about 3 KB of log text each): three armed states in eight outfits
// F15 (native on foot E1, TEMPORARY): the first-person body hide. The frame thread writes g_fpHideWant and the names (g_fpIdx); the hook thread writes the
// readback and counts the first-person AMC calls. g_fpBodyMutex guards g_fpIdx and g_fpReadback.
std::atomic<bool> g_fpHideWant{false};
std::atomic<bool> g_hybridWant{false};        // F21: the hybrid body is wanted now (frame thread writes; the hook reads)
std::atomic<uint64_t> g_hybridMaskWrites{0};  // F21: third-person mask words zeroed since the frame thread last logged (hook thread)
std::atomic<uint64_t> g_fpMode2Seen{0};
std::mutex g_fpBodyMutex;
std::array<int, avh::kFpBodyPartCount> g_fpIdx = [] {
    std::array<int, avh::kFpBodyPartCount> idx;
    idx.fill(-1);
    return idx;
}();   // guarded by g_fpBodyMutex: the part index of each kFpBodyPartNames entry, -1 when absent
FpBodyReadback g_fpReadback;                           // guarded by g_fpBodyMutex (its parts, names and mode2Seen fields are filled on read)
std::atomic<uint64_t> g_ctlCalls{0};                   // the controller hook's calls (the gate open)

// The skeleton interface's build addresses (avatar_skeleton_core.h): established once, when the hooks are first wanted, before the census
// verifies a skeleton through them. Written by the frame thread before g_skelArmed is released; read by the hook threads after it.
avh::HeadTargets g_skelTargets;
std::atomic<bool> g_skelArmed{false};

// The frame thread's own state: what it has already said.
struct FrameState {
    bool armTried = false;
    bool amcMatchSaid = false, amcNoMatchSaid = false, amcDownSaid = false;
    uint64_t latchedThirdSeen = 0;
};
FrameState g_frame;

// The avatar fade global (frame thread only): where the global and its amount float live.
int32_t* g_fadeMode = nullptr;
float* g_fadeAmount = nullptr;

// Native on-foot body (F2). g_nativeWant: the native key is on, so the hooks are installed from launch (frame thread writes).
// g_nativeBody: native stereo is on foot now, so headHideStep may zero the head parts (frame thread writes, the hook threads read).
// g_nativeReason and g_nativeFade are frame thread only: why the body last ended, and the native claim on the fade global.
std::atomic<bool> g_nativeWant{false};
std::atomic<bool> g_nativeBody{false};
std::atomic<uint64_t> g_nativeHideHeld{0};   // F93d, hook threads: head-hide calls that wrote nothing because the game's camera was third-person
const char* g_nativeReason = "";
avh::FadeGuard g_nativeFade;

#ifdef EDVR_NATIVE_ON_FOOT_HOOKS_TEST
uintptr_t g_testTargets[kHkCount] = {};   // the rig's synthetic functions, installed in place of the game's
int32_t* g_testFadeMode = nullptr;        // ...and its synthetic fade global
float* g_testFadeAmount = nullptr;
#endif

// ---- guarded access to the game's memory (nothing with a destructor lives in a function that has a __try) ------------------
// The controller's mode byte and its pending-retry byte (+0x3E0, +0x3E1).
__declspec(noinline) bool sehReadCtlView(const uint8_t* c, uint8_t* mode, uint8_t* pending) noexcept {
    __try {
        *mode = c[avh::kOffCtlMode];
        *pending = c[avh::kOffCtlPending];
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool plausibleUserPointer(uint64_t p) { return p >= 0x10000u && p < 0x00007FFF00000000ull; }
// The fade global and its amount float: plain data in the exe's .data, read and written under SEH.
__declspec(noinline) bool sehReadInt32(const int32_t* p, int32_t* out) noexcept {
    __try {
        *out = *reinterpret_cast<const volatile int32_t*>(p);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
__declspec(noinline) bool sehWriteInt32(int32_t* p, int32_t value) noexcept {
    __try {
        *reinterpret_cast<volatile int32_t*>(p) = value;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
// Is [p, p+bytes) committed, readable and writable? One VirtualQuery per region; no SEH needed.
bool writableRange(const void* p, size_t bytes) noexcept {
    uintptr_t at = reinterpret_cast<uintptr_t>(p);
    const uintptr_t end = at + bytes;
    if (end < at) return false;
    while (at < end) {
        MEMORY_BASIC_INFORMATION region{};
        if (!VirtualQuery(reinterpret_cast<void*>(at), &region, sizeof(region))) return false;
        if (region.State != MEM_COMMIT) return false;
        if (region.Protect & (PAGE_GUARD | PAGE_NOACCESS)) return false;
        if (!(region.Protect & (PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY))) return false;
        const uintptr_t regionEnd = reinterpret_cast<uintptr_t>(region.BaseAddress) + region.RegionSize;
        if (regionEnd <= at) return false;
        at = regionEnd;
    }
    return true;
}
// PE TimeDateStamp + SizeOfImage (base+0x3C -> e_lfanew, +8 and +0x50), the pair every build-keyed hook here checks.
__declspec(noinline) bool sehCheckBytes(uintptr_t address, const uint8_t* expected, size_t n) noexcept {
    __try {
        return std::memcmp(reinterpret_cast<const void*>(address), expected, n) == 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
__declspec(noinline) bool checkIdentity(uintptr_t base, const char** why) noexcept {
    __try {
        uint32_t peOff = 0;
        std::memcpy(&peOff, reinterpret_cast<const void*>(base + 0x3C), 4);
        if (peOff > 0x1000) { *why = "the PE header offset is implausible"; return false; }
        uint32_t timestamp = 0, imageSize = 0;
        std::memcpy(&timestamp, reinterpret_cast<const void*>(base + peOff + 8), 4);
        std::memcpy(&imageSize, reinterpret_cast<const void*>(base + peOff + 0x50), 4);
        if (timestamp != avh::kExpectedTimestamp || imageSize != avh::kExpectedImageSize) {
            *why = "the PE timestamp or image size is not build 334925's";
            return false;
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *why = "a read faulted while checking the PE header";
        return false;
    }
}


__declspec(noinline) bool sehReadU64At(uintptr_t at, uint64_t* out) noexcept {
    __try {
        std::memcpy(out, reinterpret_cast<const void*>(at), 8);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
__declspec(noinline) bool sehReadU16At(uintptr_t at, uint16_t* out) noexcept {
    __try {
        std::memcpy(out, reinterpret_cast<const void*>(at), 2);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
__declspec(noinline) bool sehReadSlots(uintptr_t vptr, uint64_t fn[avh::kHeadSlots]) noexcept {
    __try {
        for (uint32_t i = 0; i < avh::kHeadSlots; ++i) std::memcpy(&fn[i], reinterpret_cast<const void*>(vptr + 8u * avh::kHeadSlotIndex[i]), 8);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
// The pose's local transforms (32 bytes a joint) and u16 parents, copied for the walk.
__declspec(noinline) bool sehReadPoseArrays(uintptr_t pose, uint32_t joints, float* locals, uint16_t* parents) noexcept {
    __try {
        uint64_t lp = 0, pp = 0;
        std::memcpy(&lp, reinterpret_cast<const void*>(pose + avh::kPoseLocalsOff), 8);
        std::memcpy(&pp, reinterpret_cast<const void*>(pose + avh::kPoseParentsOff), 8);
        if (!plausibleUserPointer(lp) || !plausibleUserPointer(pp)) return false;
        std::memcpy(locals, reinterpret_cast<const void*>(lp), static_cast<size_t>(joints) * avh::kJointBytes);
        std::memcpy(parents, reinterpret_cast<const void*>(pp), static_cast<size_t>(joints) * 2u);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
using SkPoseFn = void* (__fastcall*)(void* iface);
using SkFindFn = uint32_t (__fastcall*)(void* iface, const char* name);
using SkMatrixFn = void (__fastcall*)(void* iface, uint32_t index, float* out);
__declspec(noinline) bool sehCallPose(uint64_t fn, uintptr_t iface, uintptr_t* out) noexcept {
    __try {
        *out = reinterpret_cast<uintptr_t>(reinterpret_cast<SkPoseFn>(fn)(reinterpret_cast<void*>(iface)));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
__declspec(noinline) bool sehCallFind(uint64_t fn, uintptr_t iface, const char* name, uint32_t* out) noexcept {
    __try {
        *out = reinterpret_cast<SkFindFn>(fn)(reinterpret_cast<void*>(iface), name);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
__declspec(noinline) bool sehCallModel(uint64_t fn, uintptr_t iface, uint32_t index, float* out) noexcept {
    __try {
        reinterpret_cast<SkMatrixFn>(fn)(reinterpret_cast<void*>(iface), index, out);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

enum class SkVerify { Ok, Stale, Mismatch, Fault };
struct SkSlots {
    uint64_t fn[avh::kHeadSlots] = {};
    avh::HeadKind kind = avh::HeadKind::None;
    uint32_t badSlot = 0;
    uint64_t found = 0, expected = 0;
};
SkVerify skVerify(uintptr_t iface, avh::HeadKind expect, SkSlots* s) noexcept {
    uint64_t vptr = 0;
    if (!plausibleUserPointer(iface) || !sehReadU64At(iface, &vptr)) return SkVerify::Stale;
    const avh::HeadKind kind = avh::headKindOfVtable(vptr, g_skelTargets);
    if (kind == avh::HeadKind::None || (expect != avh::HeadKind::None && kind != expect)) return SkVerify::Stale;
    if (!sehReadSlots(static_cast<uintptr_t>(vptr), s->fn)) return SkVerify::Fault;
    uint32_t bad = 0;
    uint64_t found = 0;
    if (avh::headVerifySlots(vptr, s->fn, kind, g_skelTargets, &bad, &found) != 0) {
        s->badSlot = bad;
        s->found = found;
        s->expected = avh::headExpectedFunction(kind, bad, g_skelTargets);
        return SkVerify::Mismatch;
    }
    s->kind = kind;
    return SkVerify::Ok;
}

// Ghidra shows one parameter (rcx = the object) for all three callback hooks, a return value in rax and no float-register parameter
// read, so four integer registers are forwarded and nothing else is declared: declaring a float parameter would read an xmm register
// the caller never set.
using ForwardFn = uint64_t (__fastcall*)(void*, void*, void*, void*);
// F93d: the controller last read for native on foot's camera signal, and how often a different controller answered (the frame thread logs both).
std::atomic<uint64_t> g_nativeCamCtl{0};
std::atomic<uint64_t> g_nativeCamCtlSwitches{0};
std::atomic<uint64_t> g_nativeCamReadFaults{0};

// F93d: after the game's own update the mode byte is the suite's state for this frame (the update copies the shared record's +0x30 in at its start and
// writes its own result back at its end), so it is read here, not in the pre-call. Read only; native stereo on foot only.
__declspec(noinline) void nativeCamAfterUpdate(void* controller) noexcept {
    if (!controller || !nativeOnFootActive()) return;
    uint8_t mode = 0, pending = 0;
    if (!sehReadCtlView(static_cast<const uint8_t*>(controller), &mode, &pending)) {
        g_nativeCamReadFaults.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const uint64_t c = reinterpret_cast<uint64_t>(controller);
    const uint64_t was = g_nativeCamCtl.exchange(c, std::memory_order_relaxed);
    if (was != 0 && was != c) g_nativeCamCtlSwitches.fetch_add(1, std::memory_order_relaxed);
    LARGE_INTEGER q{};
    QueryPerformanceCounter(&q);
    nativeOnFootCameraPublish(mode, static_cast<uint64_t>(q.QuadPart));
}

// ---- the controller hook --------------------------------------------------------------------------------------------------------
// The game's update runs untouched; the mode byte is read after it. Nothing is written to the controller.
__declspec(noinline) uint64_t __fastcall controllerHooked(void* a, void* b, void* c, void* d) noexcept {
    const auto forward = reinterpret_cast<ForwardFn>(g_forward[kHkCtl].load(std::memory_order_acquire));
    if (!forward) return 0;
    g_ctlCalls.fetch_add(1, std::memory_order_relaxed);
    const uint64_t result = forward(a, b, c, d);
    nativeCamAfterUpdate(a);   // F93d: the game's camera for native on foot's body changes
    return result;
}

// ---- the FindJoint hook (nothing here writes) --------------------------------------------------------------------------------------
// FindJoint is called by many systems, so this stays trivial: the original FIRST (its result is returned unchanged), then the capture, which
// looks at rcx, rdx, the result and the caller's return address. The callback relay JUMPS here, so the return address on entry is the caller's own.
__declspec(noinline) uint64_t __fastcall findJointHooked(void* a, void* b, void* c, void* d) noexcept {
    const uintptr_t returnAddress = reinterpret_cast<uintptr_t>(_ReturnAddress());
    const uintptr_t where = reinterpret_cast<uintptr_t>(_AddressOfReturnAddress());   // the relay JUMPS here, so this is the caller's own stack slot
    const auto forward = reinterpret_cast<ForwardFn>(g_forward[kHkFind].load(std::memory_order_acquire));
    if (!forward) return 0;
    const uint64_t result = forward(a, b, c, d);
    g_findSeen.fetch_add(1, std::memory_order_relaxed);
    const int site = avh::findCaptureSite(returnAddress, reinterpret_cast<uintptr_t>(b), g_findSite[0].load(std::memory_order_relaxed),
                                          g_findSite[1].load(std::memory_order_relaxed), g_findPov.load(std::memory_order_relaxed));
    if (site == 0) {
        g_skelCaptures[0].fetch_add(1, std::memory_order_relaxed);
        t_site1.iface = reinterpret_cast<uint64_t>(a);
        t_site1.index = static_cast<uint32_t>(result & 0xFFFFu);
        t_site1.where = where;
        t_site1.valid = true;
    } else if (site == 1) {
        g_skelCaptures[1].fetch_add(1, std::memory_order_relaxed);
        const Site1Seen third = t_site1;
        t_site1.valid = false;
        if (avh::isLocalPair(third.valid, third.where, where, third.iface, reinterpret_cast<uint64_t>(a))) {
            g_skelIndex[0].store(third.index, std::memory_order_relaxed);
            g_skelIndex[1].store(static_cast<uint32_t>(result & 0xFFFFu), std::memory_order_relaxed);
            g_skelIface[1].store(reinterpret_cast<uint64_t>(a), std::memory_order_release);
            g_skelIface[0].store(third.iface, std::memory_order_release);
            g_skelLatches.fetch_add(1, std::memory_order_relaxed);
        }
    }
    return result;
}

// ---- hiding the head parts (the avatar-fade hook's post-call) -------------------------------------------------------------------
// Runs inside the game's submit job, right after the dither fade and before the loop that reads the view masks, on the same thread: a zero written here is
// seen by that loop and by nothing racing it. Local third-person AMC only; masks of the head set only; only while native stereo's body is on foot in the
// game's first-person camera. Every access is under SEH; three faults stand head hiding down for the session.
struct AmcIdent {
    uint32_t id = 0;
    uint64_t pose = 0;
    int32_t mode = -1;
};
__declspec(noinline) bool sehReadAmcIdent(const uint8_t* amc, AmcIdent* o) noexcept {
    __try {
        std::memcpy(&o->id, amc + avh::kOffAmcAvatarPoseId, 4);
        std::memcpy(&o->pose, amc + avh::kOffAmcAvatarPose, 8);
        uint64_t params = 0;
        std::memcpy(&params, amc + avh::kOffAmcParams, 8);
        o->mode = -1;
        if (params >= 0x10000u && params < 0x00007FFF00000000ull) std::memcpy(&o->mode, reinterpret_cast<const uint8_t*>(params) + avh::kOffParamsMode, 4);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
// Zero the four view masks of every head part. `zeroed` counts the words that were not already zero.
__declspec(noinline) bool sehZeroHeadMasks(uint8_t* amc, uint32_t* zeroed) noexcept {
    __try {
        uint32_t n = 0;
        for (uint32_t i = 0; i < avh::kHeadPartCount; ++i) {
            volatile uint64_t* masks = reinterpret_cast<volatile uint64_t*>(amc + avh::kOffAmcMasks + avh::kHeadParts[i] * avh::kPartStride);
            for (uint32_t k = 0; k < avh::kPartVariants; ++k) {
                if (masks[k] != 0) {
                    masks[k] = 0;
                    ++n;
                }
            }
        }
        *zeroed = n;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
// F21: zero every view mask word of the parts idx[0..n) (a negative index is skipped). `zeroed` counts the words that were not already zero.
__declspec(noinline) bool sehZeroPartMasks(uint8_t* amc, const int* idx, size_t n, uint32_t* zeroed) noexcept {
    __try {
        uint32_t z = 0;
        for (size_t i = 0; i < n; ++i) {
            if (idx[i] < 0) continue;
            volatile uint64_t* masks = reinterpret_cast<volatile uint64_t*>(amc + avh::kOffAmcMasks + static_cast<uint32_t>(idx[i]) * avh::kPartStride);
            for (uint32_t k = 0; k < avh::kPartVariants; ++k) {
                if (masks[k] != 0) {
                    masks[k] = 0;
                    ++z;
                }
            }
        }
        *zeroed = z;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
// ---- F95c: the head kept in the shadows (native_on_foot_f95c.h) ----------------------------------------------------------------------------------------------------
namespace f95c = nofoot::f95c;
static_assert(avh::kHeadPartCount <= f95c::kMaxParts, "F95c: the swap record holds every head part");
static_assert(f95c::kOffPartClass == avh::kOffAmcPartState, "F95c: the part class is the census's state");
// The views of a visibility job context: each view's flags and bit. 1 read, 0 not a context this reads (more than 64 views), -1 a fault.
__declspec(noinline) int sehReadViews(const uint8_t* job, uint32_t* flags, uint64_t* bits, uint32_t* n) noexcept {
    __try {
        uint64_t count = 0;
        std::memcpy(&count, job + f95c::kViewCountOff, 8);
        if (count > f95c::kMaxViews) return 0;
        for (uint32_t i = 0; i < static_cast<uint32_t>(count); ++i) {
            const uint8_t* v = job + f95c::kViewsOff + static_cast<size_t>(i) * f95c::kViewStride;
            std::memcpy(&flags[i], v + f95c::kViewFlagsOff, 4);
            std::memcpy(&bits[i], v + f95c::kViewBitOff, 8);
        }
        *n = static_cast<uint32_t>(count);
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}
// The head parts' classes for the call: each one shadowClass changes is swapped by compare-exchange (a class the game changed between the read and the write is left
// alone and not recorded). False on a fault; the parts recorded before it are in s.
__declspec(noinline) bool sehSwapHeadClasses(uint8_t* amc, f95c::Swap* s) noexcept {
    __try {
        for (uint32_t i = 0; i < avh::kHeadPartCount; ++i) {
            volatile LONG* p = reinterpret_cast<volatile LONG*>(amc + f95c::kOffPartClass + avh::kHeadParts[i] * avh::kPartStride);
            const int32_t was = static_cast<int32_t>(*p);
            const int32_t set = f95c::shadowClass(was);
            if (set == was) continue;
            if (InterlockedCompareExchange(p, set, was) != was) continue;
            s->part[s->n] = avh::kHeadParts[i];
            s->was[s->n] = was;
            s->set[s->n] = set;
            ++s->n;
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
// The game's classes put back: a class still at the value written gets the game's value; one the game changed during the call stands (f95c::restoredClass).
// `lost` counts those. False on a fault.
__declspec(noinline) bool sehRestoreHeadClasses(uint8_t* amc, const f95c::Swap* s, uint32_t* lost) noexcept {
    __try {
        uint32_t l = 0;
        for (uint32_t j = 0; j < s->n; ++j) {
            volatile LONG* p = reinterpret_cast<volatile LONG*>(amc + f95c::kOffPartClass + s->part[j] * avh::kPartStride);
            if (InterlockedCompareExchange(p, s->was[j], s->set[j]) != s->set[j]) ++l;
        }
        *lost = l;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
// The head masks after the call: (instance, view) pairs in the shadow views, and in the main and other views (the bits the fade hook removes).
__declspec(noinline) bool sehCountHeadPairs(const uint8_t* amc, uint64_t shadowBits, uint64_t deny, uint64_t* shadowPairs, uint64_t* eyePairs) noexcept {
    __try {
        uint64_t sp = 0, ep = 0;
        for (uint32_t i = 0; i < avh::kHeadPartCount; ++i) {
            for (uint32_t k = 0; k < avh::kPartVariants; ++k) {
                uint64_t m = 0;
                std::memcpy(&m, amc + avh::kOffAmcMasks + avh::kHeadParts[i] * avh::kPartStride + 8 * k, 8);
                sp += f95c::popcount64(m & shadowBits & ~deny);
                ep += f95c::popcount64(m & deny);
            }
        }
        *shadowPairs = sp;
        *eyePairs = ep;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
// The fade hook's keep: every head mask less deny (f95c::guarded). `removed` counts the words that lost a bit, `kept` the words left non-zero.
__declspec(noinline) bool sehGuardHeadMasks(uint8_t* amc, uint64_t deny, uint32_t* removed, uint32_t* kept) noexcept {
    __try {
        uint32_t r = 0, kp = 0;
        for (uint32_t i = 0; i < avh::kHeadPartCount; ++i) {
            volatile uint64_t* masks = reinterpret_cast<volatile uint64_t*>(amc + avh::kOffAmcMasks + avh::kHeadParts[i] * avh::kPartStride);
            for (uint32_t k = 0; k < avh::kPartVariants; ++k) {
                const uint64_t m = masks[k];
                const uint64_t g = f95c::guarded(m, deny);
                if (g != m) {
                    masks[k] = g;
                    ++r;
                }
                if (g != 0) ++kp;
            }
        }
        *removed = r;
        *kept = kp;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
void f95cFault() noexcept {
    if (g_f95cFaults.fetch_add(1, std::memory_order_relaxed) + 1 >= avh::kMaxHideFaults) g_f95cDown.store(true, std::memory_order_release);
}
// ---- F142 (the full outfit, always on since F146): the outfit through the visibility call, and the outfit census (native_on_foot_f142.h) -------------------------------------
namespace f142 = nofoot::f142;
static_assert(f142::kPartCount == avh::kPartCount, "F142: the swap record and the census hold every part of the avatar's table");
static_assert(nofoot::f143::kPartCount == avh::kPartCount, "F143: the clip snapshot holds every part of the avatar's table");
// The classes of every part for the call: f142::promoteClasses with the game's own volatile reads and compare-exchanges (F95c's swap, the other way). False on a fault; the
// parts promoted before it are in s.
__declspec(noinline) bool sehPromoteOutfit(uint8_t* amc, f142::OutfitSwap* s, uint32_t skeletonIdx, uint64_t skipMask) noexcept {
    __try {
        f142::promoteClasses(
            *s, skeletonIdx, &avh::isHeadPart,
            [amc](uint32_t i) { return static_cast<int32_t>(*reinterpret_cast<volatile LONG*>(amc + f95c::kOffPartClass + i * avh::kPartStride)); },
            [amc](uint32_t i, int32_t desired, int32_t expected) {
                volatile LONG* p = reinterpret_cast<volatile LONG*>(amc + f95c::kOffPartClass + i * avh::kPartStride);
                return InterlockedCompareExchange(p, desired, expected) == expected;
            },
            skipMask);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
// F144: whether a part has any instance (the four instance pointers). 1 yes, 0 no, -1 a fault.
__declspec(noinline) int sehHasInstance(const uint8_t* amc, uint32_t idx) noexcept {
    __try {
        for (uint32_t k = 0; k < avh::kPartVariants; ++k) {
            uint64_t inst = 0;
            std::memcpy(&inst, amc + avh::kOffAmcInstances + idx * avh::kPartStride + 8 * k, 8);
            if (inst != 0) return 1;
        }
        return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}
// F144: EVASuit_Gear_Back is the coat only for an outer garment (no EVASuit_Arms instance, an EVASuit_Gear_Arms instance); the other suit's is its backpack. Read where the promotion
// is, on the same thread, from the avatar's instances. The decision is kept for the frame thread's line. kGearUnknown on a fault (nothing is promoted).
int gearBackNow(const uint8_t* amc) noexcept {
    static const int suitArms = avh::partIndexByName("EVASuit_Arms"), gearArms = avh::partIndexByName("EVASuit_Gear_Arms");
    if (suitArms < 0 || gearArms < 0) return nofoot::f144::kGearUnknown;
    const int suit = sehHasInstance(amc, static_cast<uint32_t>(suitArms)), gear = sehHasInstance(amc, static_cast<uint32_t>(gearArms));
    if (suit < 0 || gear < 0) return nofoot::f144::kGearUnknown;
    const int d = nofoot::f144::gearBackDecision(suit != 0, gear != 0);
    g_f144Gear.store(d, std::memory_order_relaxed);
    return d;
}
// F147: what the full outfit may touch, resolved once by name (nofoot::f144::kPromoteNames and kUnclipNames): the promotion is EVASuit_Gear_Back alone and the extra unclip five
// parts, both only under the outer-garment decision above. Anything else keeps the game's class and clip.
uint64_t promoteAllowedMask() noexcept {
    static const uint64_t m = [] {
        int idx[sizeof(nofoot::f144::kPromoteNames) / sizeof(nofoot::f144::kPromoteNames[0])];
        for (size_t i = 0; i < sizeof(idx) / sizeof(idx[0]); ++i) idx[i] = avh::partIndexByName(nofoot::f144::kPromoteNames[i]);
        return nofoot::f144::maskOfIdx(idx, sizeof(idx) / sizeof(idx[0]));
    }();
    return m;
}
uint64_t unclipAllowedMask() noexcept {
    static const uint64_t m = [] {
        int idx[sizeof(nofoot::f144::kUnclipNames) / sizeof(nofoot::f144::kUnclipNames[0])];
        for (size_t i = 0; i < sizeof(idx) / sizeof(idx[0]); ++i) idx[i] = avh::partIndexByName(nofoot::f144::kUnclipNames[i]);
        return nofoot::f144::maskOfIdx(idx, sizeof(idx) / sizeof(idx[0]));
    }();
    return m;
}
// The game's classes put back (f142::restoreClasses): `lost` counts the classes the game changed during the call, which stand. False on a fault.
__declspec(noinline) bool sehRestoreOutfit(uint8_t* amc, const f142::OutfitSwap* s, uint32_t* lost) noexcept {
    __try {
        *lost = f142::restoreClasses(*s, [amc](uint32_t i, int32_t desired, int32_t expected) {
            volatile LONG* p = reinterpret_cast<volatile LONG*>(amc + f95c::kOffPartClass + i * avh::kPartStride);
            return InterlockedCompareExchange(p, desired, expected) == expected;
        });
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
void f142Fault() noexcept {
    if (g_f142Faults.fetch_add(1, std::memory_order_relaxed) + 1 >= avh::kMaxHideFaults) g_f142Down.store(true, std::memory_order_release);
}
// The census of every part with an instance: the instances present, the class, the flags, the current and pending item, the four view masks. Read-only, under SEH.
__declspec(noinline) bool sehReadOutfitCensus(const uint8_t* amc, f142::Census* c) noexcept {
    __try {
        uint32_t n = 0;
        for (uint32_t idx = 0; idx < avh::kPartCount; ++idx) {
            const uint32_t at = idx * avh::kPartStride;
            uint8_t inst = 0;
            uint64_t masks[avh::kPartVariants] = {};
            for (uint32_t k = 0; k < avh::kPartVariants; ++k) {
                uint64_t ptr = 0;
                std::memcpy(&ptr, amc + avh::kOffAmcInstances + at + 8 * k, 8);
                std::memcpy(&masks[k], amc + avh::kOffAmcMasks + at + 8 * k, 8);
                if (ptr != 0) inst = static_cast<uint8_t>(inst | (1u << k));
            }
            if (!inst) continue;
            f142::PartRow& r = c->row[n++];
            r.idx = static_cast<uint8_t>(idx);
            r.inst = inst;
            r.flags = amc[avh::kOffAmcPartFlags + at];
            std::memcpy(&r.state, amc + avh::kOffAmcPartState + at, 4);
            std::memcpy(&r.pending, amc + avh::kOffAmcPartPending + at, 4);
            std::memcpy(&r.current, amc + avh::kOffAmcPartCurrent + at, 4);
            std::memcpy(r.masks, masks, sizeof(masks));
        }
        c->n = n;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
void outfitClipStep(uint8_t* amc) noexcept;   // F143 (defined with the clip probe below)
// The fade hook's step for the local third-person AMC (its caller has identified it): about every 100 ms, the census; queued for the frame thread when it differs from the last
// one queued (any part's instances, class, flags or items; never the masks), and the first. A full ring leaves the signature, so the next read tries again. Three faults stop it.
void outfitCensusStep(uint8_t* amc) noexcept {
    if (g_f142CensusDown.load(std::memory_order_acquire)) return;
    const uint64_t now = GetTickCount64();
    if (now - g_f142CensusMs.load(std::memory_order_relaxed) < 100) return;
    if (g_f142Busy.exchange(true, std::memory_order_acquire)) return;
    g_f142CensusMs.store(now, std::memory_order_relaxed);
    f142::Census c;
    c.amc = reinterpret_cast<uint64_t>(amc);
    if (!sehReadOutfitCensus(amc, &c)) {
        if (g_f142CensusFaults.fetch_add(1, std::memory_order_relaxed) + 1 >= avh::kMaxHideFaults) g_f142CensusDown.store(true, std::memory_order_release);
    } else {
        const uint64_t sig = f142::censusSig(c);
        if (!g_f142HaveSig || sig != g_f142Sig) {
            if (g_f142CensusRing.push(c)) {
                g_f142Sig = sig;
                g_f142HaveSig = true;
                g_f142OutfitGen.fetch_add(1, std::memory_order_relaxed);   // F147: the clip measure reads again for this outfit
            }
        }
    }
    g_f142Busy.store(false, std::memory_order_release);
}
__declspec(noinline) bool sehReadCensus(const uint8_t* amc, avh::AmcCensus* c) noexcept {
    __try {
        std::memcpy(&c->viewFilter, amc + avh::kOffAmcViewFilter, 4);
        std::memcpy(&c->alpha, amc + avh::kOffAmcAlpha, 4);
        std::memcpy(&c->context, amc + avh::kOffAmcContext, 4);
        std::memcpy(c->flagBytes, amc + avh::kOffAmcFlagBytes, 4);
        uint32_t n = 0;
        for (uint32_t idx = 0; idx < avh::kPartCount; ++idx) {
            const uint32_t at = idx * avh::kPartStride;
            uint8_t variants = 0, maskBits = 0;
            for (uint32_t k = 0; k < avh::kPartVariants; ++k) {
                uint64_t inst = 0, mask = 0;
                std::memcpy(&inst, amc + avh::kOffAmcInstances + at + 8 * k, 8);
                std::memcpy(&mask, amc + avh::kOffAmcMasks + at + 8 * k, 8);
                if (inst != 0) variants = static_cast<uint8_t>(variants | (1u << k));
                if (mask != 0 && inst != 0) maskBits = static_cast<uint8_t>(maskBits | (1u << k));
            }
            if (variants == 0) continue;
            avh::PartCensus& p = c->part[n++];
            p.idx = static_cast<uint8_t>(idx);
            p.variants = variants;
            p.maskBits = maskBits;
            p.flags = amc[avh::kOffAmcPartFlags + at];
            std::memcpy(&p.state, amc + avh::kOffAmcPartState + at, 4);
            std::memcpy(&p.pending, amc + avh::kOffAmcPartPending + at, 4);
            std::memcpy(&p.current, amc + avh::kOffAmcPartCurrent + at, 4);
        }
        c->count = n;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool plausiblePointer(uint64_t p) noexcept { return p >= 0x10000u && p < 0x00007FFF00000000ull; }
// rcx = *(AMC+0x208); rdx = *rcx; call [rdx+0x20] returns a matrix pointer whose origin is at +0x30 (FUN 0x3DD8C60 at 0x3DD8DC2..0x3DD8DCF, same call).
// 1 = read, 0 = not available (a pointer along the chain is null or implausible: nothing was called), -1 = a fault.
__declspec(noinline) int sehAmcOrigin(const uint8_t* amc, float* out3) noexcept {
    __try {
        uint64_t obj = 0, vt = 0, fn = 0;
        std::memcpy(&obj, amc + avh::kOffAmcTransform, 8);
        if (!plausiblePointer(obj)) return 0;
        std::memcpy(&vt, reinterpret_cast<const void*>(obj), 8);
        if (!plausiblePointer(vt)) return 0;
        std::memcpy(&fn, reinterpret_cast<const void*>(vt + avh::kTransformMatrixSlot), 8);
        if (!plausiblePointer(fn)) return 0;
        const uint64_t m = reinterpret_cast<uint64_t(__fastcall*)(uint64_t)>(fn)(obj);
        if (!plausiblePointer(m)) return 0;
        std::memcpy(out3, reinterpret_cast<const void*>(m + avh::kMatrixOriginOff), 12);
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}
void hideFault() noexcept {
    if (g_hideFaults.fetch_add(1, std::memory_order_relaxed) + 1 >= avh::kMaxHideFaults) g_hideDown.store(true, std::memory_order_release);
}
// Whether the head hide writes now: native stereo's body (F2) in the game's first-person camera (F93d).
bool headHideWritesNow() noexcept {
    return g_nativeBody.load(std::memory_order_acquire) && !nativeOnFootCameraExternal();
}
void headHideStep(void* amcPtr) noexcept {
    if (!amcPtr || g_hideDown.load(std::memory_order_acquire)) return;
    g_amcCalls.fetch_add(1, std::memory_order_relaxed);
    uint8_t* const amc = static_cast<uint8_t*>(amcPtr);
    AmcIdent id;
    if (!sehReadAmcIdent(amc, &id)) {
        hideFault();
        return;
    }
    const bool idOk = id.id == 0xFFFFFFFFu;
    if (idOk) g_amcIdOk.fetch_add(1, std::memory_order_relaxed);
    if (idOk && id.mode == 3) {
        g_amcMode3.fetch_add(1, std::memory_order_relaxed);
        g_amcCand.store(reinterpret_cast<uint64_t>(amcPtr), std::memory_order_relaxed);
        g_amcCandPose.store(id.pose, std::memory_order_relaxed);
    }
    const uint64_t skeleton = g_skelIface[0].load(std::memory_order_acquire);
    if (!avh::isLocalAmc(id.id, id.pose, skeleton, id.mode)) return;
    g_amcLocalMatches.fetch_add(1, std::memory_order_relaxed);
    g_amcLocal.store(reinterpret_cast<uint64_t>(amcPtr), std::memory_order_relaxed);
    g_amcLocalPose.store(id.pose, std::memory_order_relaxed);
    // The census, once per local AMC (a new AMC, a new census). One thread at a time takes it.
    if (reinterpret_cast<uint64_t>(amcPtr) != g_amcCensused && !g_busyAmc.exchange(true, std::memory_order_acquire)) {
        if (reinterpret_cast<uint64_t>(amcPtr) != g_amcCensused) {
            avh::AmcCensus census;
            census.amc = reinterpret_cast<uint64_t>(amcPtr);
            census.avatarPose = id.pose;
            census.skeleton = skeleton;
            census.mode = id.mode;
            if (sehReadCensus(amc, &census)) {
                g_censusRing.push(census);
                g_amcCensused = census.amc;
            } else {
                hideFault();
            }
        }
        g_busyAmc.store(false, std::memory_order_release);
    }
    outfitCensusStep(amc);   // F142: the per-part census on a change (read only; about every 100 ms)
    outfitClipStep(amc);     // F143: every part's clip modifier and view masks, every 5 s and when the armed state changes (read only; before this call's unclip writes)
    if (g_hideDown.load(std::memory_order_acquire)) return;
    // The writes need native stereo's body (F2, nativeOnFootHooksBody). Without it this function only reads (the census above).
    // F93d: native stereo's head hiding is first-person only: in the game's third-person camera (the camera suite) the head is the game's.
    if (!headHideWritesNow()) {
        if (g_nativeBody.load(std::memory_order_acquire)) g_nativeHideHeld.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    // F95c: the AMC's latest visibility call ran with the head as shadow-only (the ticket): keep its masks, less every main and other view's bit. Else zero.
    uint64_t mine = reinterpret_cast<uint64_t>(amcPtr);
    const bool ticket = g_f95cTicket.compare_exchange_strong(mine, 0, std::memory_order_acq_rel);
    if (f95c::fadeAction(true, ticket) == f95c::Action::Keep) {
        uint32_t removed = 0, kept = 0;
        if (!sehGuardHeadMasks(amc, g_f95cDeny.load(std::memory_order_relaxed), &removed, &kept)) {
            hideFault();
            return;
        }
        g_hideCalls.fetch_add(1, std::memory_order_relaxed);
        g_f95cKept.fetch_add(1, std::memory_order_relaxed);
        g_f95cKeptWords.fetch_add(kept, std::memory_order_relaxed);
        g_f95cRemoved.fetch_add(removed, std::memory_order_relaxed);
        return;
    }
    g_f95cZeroed.fetch_add(1, std::memory_order_relaxed);
    uint32_t zeroed = 0;
    if (!sehZeroHeadMasks(amc, &zeroed)) {
        hideFault();
        return;
    }
    g_hideCalls.fetch_add(1, std::memory_order_relaxed);
    g_hideZeroed.fetch_add(zeroed, std::memory_order_relaxed);
}

// ---- F15: the first-person body (E1, TEMPORARY) --------------------------------------------------------------------------------------
// The local first-person AMC is the one at mode 2 (its creation parameters say first person): only the local player has one (isLocalPair, above).
// Its arms, hands, skeleton and lower-arm cosmetics get the same zeroing headHideStep gives the head parts, after the fade, on the same thread.
// The cosmetics have their own visibility masks: keeping them would draw FP bracers beside the solved 3P arms. Nothing is restored.
__declspec(noinline) bool sehZeroFpMasks(uint8_t* amc, const int* idx, uint32_t* zeroed, uint64_t (*readback)[avh::kPartVariants]) noexcept {
    __try {
        uint32_t n = 0;
        for (uint32_t i = 0; i < avh::kFpBodyPartCount; ++i) {
            if (idx[i] < 0) continue;
            volatile uint64_t* masks = reinterpret_cast<volatile uint64_t*>(amc + avh::kOffAmcMasks + static_cast<uint32_t>(idx[i]) * avh::kPartStride);
            for (uint32_t k = 0; k < avh::kPartVariants; ++k) {
                if (masks[k] != 0) {
                    masks[k] = 0;
                    ++n;
                }
                readback[i][k] = masks[k];
            }
        }
        *zeroed = n;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
// F144: the local first-person AMC's census, before EDVR's zero: about every 100 ms, the parts with an instance (instances, class, flags, items, view masks as the game left them),
// the parts EDVR's list zeroes, and those of them with a mask bit set (what the zero changes). Queued when the parts, their classes, flags or items, or the zeroed set differ from the
// last queued (never the masks: they come and go with the scene's views). SEH-guarded; three faults stop it.
void fpCensusStep(uint8_t* amc, const int* idx) noexcept {
    if (g_f144FpFaults.load(std::memory_order_acquire) >= avh::kMaxHideFaults) return;
    const uint64_t now = GetTickCount64();
    if (now - g_f144FpMs.load(std::memory_order_relaxed) < 100) return;
    if (g_f144FpBusy.exchange(true, std::memory_order_acquire)) return;
    g_f144FpMs.store(now, std::memory_order_relaxed);
    static nofoot::f144::FpCensus fc;   // (3 KB; only this thread, under g_f144FpBusy)
    fc = nofoot::f144::FpCensus{};
    fc.c.amc = reinterpret_cast<uint64_t>(amc);
    if (!sehReadOutfitCensus(amc, &fc.c)) {
        g_f144FpFaults.fetch_add(1, std::memory_order_relaxed);
    } else {
        for (uint32_t i = 0; i < avh::kFpBodyPartCount; ++i)
            if (idx[i] >= 0) fc.listMask |= 1ull << idx[i];
        fc.zeroedMask = nofoot::f144::zeroedOf(fc.c, fc.listMask);
        const uint64_t sig = nofoot::f144::fpSig(fc);
        if ((!g_f144FpHaveSig || sig != g_f144FpSig) && g_f144FpRing.push(fc)) {
            g_f144FpSig = sig;
            g_f144FpHaveSig = true;
        }
    }
    g_f144FpBusy.store(false, std::memory_order_release);
}
void fpBodyStep(void* amcPtr) noexcept {
    if (!amcPtr || !g_fpHideWant.load(std::memory_order_acquire) || g_hideDown.load(std::memory_order_acquire)) return;
    if (nativeOnFootCameraExternal()) return;   // F93d: the first-person avatar is the game's own business in its third-person camera
    uint8_t* const amc = static_cast<uint8_t*>(amcPtr);
    AmcIdent id;
    if (!sehReadAmcIdent(amc, &id)) {
        hideFault();
        return;
    }
    if (id.mode != 2) return;
    g_fpMode2Seen.fetch_add(1, std::memory_order_relaxed);
    int idx[avh::kFpBodyPartCount];
    {
        std::lock_guard<std::mutex> lock(g_fpBodyMutex);
        for (uint32_t i = 0; i < avh::kFpBodyPartCount; ++i) idx[i] = g_fpIdx[i];
    }
    if (g_hybridWant.load(std::memory_order_acquire)) {   // F21: the hybrid body hides the first-person skeleton only; its arms and hands hold the weapon
        for (uint32_t i = 0; i < avh::kFpBodyPartCount; ++i)
            if (i != avh::kFpSkeletonPartSlot) idx[i] = -1;
    }
    bool any = false;
    for (int i : idx) any = any || i >= 0;
    if (!any) return;   // no name resolved: nothing to zero (the frame thread logs why)
    fpCensusStep(amc, idx);   // F144: the first-person avatar's parts and masks as the game left them, before this call's zero
    uint64_t masks[avh::kFpBodyPartCount][avh::kPartVariants] = {};
    uint32_t zeroed = 0;
    if (!sehZeroFpMasks(amc, idx, &zeroed, masks)) {
        hideFault();
        return;
    }
    std::lock_guard<std::mutex> lock(g_fpBodyMutex);
    g_fpReadback.amc = reinterpret_cast<uint64_t>(amcPtr);
    std::memcpy(g_fpReadback.masks, masks, sizeof(masks));
}

// ---- F19 (TEMPORARY): the body's clip modifiers (fix.native_on_foot_body_unclip) and their 5 s probe ----------------------------------
// The item-apply function (EliteDangerous64.exe+0x3DDCAC0) gives eligible parts of a mode-3 avatar a "pVertexClipAmount" modifier at part+0x610:
// +8 the float amount, +0xC the enable byte, +0xD the dirty byte. The enable byte is [AMC+0x321] bit 3 for EVASuit_Torso, _Arms, _Hands and
// Shoulder_Left/_Right and Gear_LowerArms/LowerArmL/LowerArmR (the bit-3 switch case, 0x3DDCAC0+0x3B1), and 1 for EVASuit_Legs,
// EVASuit_Feet and FirstPersonSkeleton (the default case).
// The body shader discards a pixel whose clip value exceeds cb2[5].w, so an enable of 0 keeps those pixels. The unclip writes enable 0 and dirty 1
// after the fade returns, on the same thread, for the local third-person AMC only (isLocalAmc, as headHideStep identifies it). Nothing is restored.
// The probe reads the same bytes every 5 s (before that call's write), with the flags [AMC+0x321], [AMC+0x322], [AMC+0x378] once per AMC.
// F164: the legacy flight suit's torso, arms, hands and shoulder pads carry the same modifier (flight edvr_gfx_20261028_153030, the outfit census and F143's clip rows: all four enable 1,
// amounts 0.0, 0.6, 0.6 and 0.6), and the game's first-person clip discarded them in the main view: only FlightSuit_Legs, which has no modifier, was drawn. They are written like the EVA suit's.
// The order is the roles: the EVA suit's five (the hybrid body uses index 0 as its torso and 1 to 4 as the arms, hands and shoulders it masks), the flight suit's four, the three
// comparison parts (never written), then the three lower-arm gear parts. kClipUnclipParts and kClipGearFirst bound the roles.
constexpr size_t kClipParts = 15;
constexpr size_t kClipUnclipParts = 9;       // indexes 0 to 8 are written
constexpr size_t kClipGearFirst = 12;        // indexes 9 to 11 are the comparisons; 12 to 14 are the lower-arm gear, written and reported by the gear probe
inline constexpr const char* kClipPartNames[kClipParts] = {"EVASuit_Torso", "EVASuit_Arms", "EVASuit_Hands", "Shoulder_Left", "Shoulder_Right",
    "FlightSuit_Torso", "FlightSuit_Arms", "FlightSuit_Hands", "ShoulderPads",
    "EVASuit_Legs", "EVASuit_Feet", "FirstPersonSkeleton", "EVASuit_Gear_LowerArms", "EVASuit_Gear_LowerArmL", "EVASuit_Gear_LowerArmR"};
static_assert(kClipUnclipParts <= kClipGearFirst && kClipGearFirst < kClipParts, "the clip roles: unclip parts, then comparisons, then gear");
struct ClipPart {
    bool named = false;         // the name is in the part-name table (index resolved)
    bool present = false;       // any instance of the part is on the AMC
    bool readOk = false;        // the modifier's bytes were read
    bool readFault = false;
    uint64_t mod = 0;           // the modifier pointer at part+0x610 (0 = none)
    uint8_t enable = 0, dirty = 0;
    float amount = 0.0f;
    uint64_t masks[avh::kPartVariants] = {};   // F21: the view masks, as the probe read them (before that call's hybrid zero)
    uint64_t instances[avh::kPartVariants] = {};
    int32_t currentItem = -1, state = -1;
    uint8_t flags = 0;
};
struct BodyClipSnap {
    uint64_t stamp = 0;         // GetTickCount64 of the read (0 = none yet)
    uint64_t amc = 0;
    uint64_t avatarPose = 0, skeleton = 0; // validated local AMC owner, not a GPU skin-job identity
    bool flagsOk = false;
    uint8_t flags[3] = {};      // [AMC+0x321], [AMC+0x322], [AMC+0x378]
    ClipPart part[kClipParts];
};
std::atomic<bool> g_unclipWant{false};       // frame thread: fix.native_on_foot_body_unclip is on and native stereo is on foot
std::atomic<bool> g_clipProbeWant{false};    // frame thread: native stereo is on foot (the 5 s read runs)
std::atomic<uint64_t> g_clipWrites{0};       // hook thread: modifier writes since the frame thread last logged
std::atomic<uint64_t> g_clipWriteFaults{0};  // hook thread: guarded writes that faulted
std::atomic<uint64_t> g_clipExternalSkips{0};  // F93d, hook thread: calls that wrote nothing because the game's camera was third-person
std::atomic<uint64_t> g_clipNextMs{0};       // hook thread: the next probe read
BodyClipSnap g_clipSnap;                     // guarded by g_fpBodyMutex
#ifdef EDVR_NATIVE_ON_FOOT_HOOKS_TEST
uint64_t g_clipTestNow = 0;
#endif
std::array<int, kClipParts> g_clipIdx = [] { std::array<int, kClipParts> a; a.fill(-1); return a; }(); // guarded by g_fpBodyMutex

__declspec(noinline) bool sehReadClipFlags(const uint8_t* amc, uint8_t* out) noexcept {
    __try {
        out[0] = amc[0x321];
        out[1] = amc[0x322];
        out[2] = amc[0x378];
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
// The part's instances (present) and its modifier (pointer and bytes). False only on a fault.
__declspec(noinline) bool sehReadClipPart(const uint8_t* amc, uint32_t idx, ClipPart* p) noexcept {
    __try {
        const uint32_t at = avh::kOffAmcParts + idx * avh::kPartStride;
        uint64_t any = 0;
        for (uint32_t k = 0; k < avh::kPartVariants; ++k) {
            uint64_t inst = 0;
            std::memcpy(&inst, amc + avh::kOffAmcInstances + idx * avh::kPartStride + 8 * k, 8);
            p->instances[k] = inst;
            any |= inst;
        }
        p->present = any != 0;
        std::memcpy(p->masks, amc + avh::kOffAmcMasks + idx * avh::kPartStride, 8 * avh::kPartVariants);
        std::memcpy(&p->currentItem, amc + avh::kOffAmcPartCurrent + idx * avh::kPartStride, 4);
        std::memcpy(&p->state, amc + avh::kOffAmcPartState + idx * avh::kPartStride, 4);
        p->flags = amc[avh::kOffAmcPartFlags + idx * avh::kPartStride];
        std::memcpy(&p->mod, amc + at + avh::kOffPartClipMod, 8);
        p->readOk = false;
        if (plausiblePointer(p->mod)) {
            const uint8_t* m = reinterpret_cast<const uint8_t*>(p->mod);
            std::memcpy(&p->enable, m + avh::kClipModEnable, 1);
            std::memcpy(&p->dirty, m + avh::kClipModDirty, 1);
            std::memcpy(&p->amount, m + avh::kClipModAmount, 4);
            p->readOk = true;
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
// The unclip write: enable 0, dirty 1, every call, also when the game already has enable 0. 1 = written, 0 = no modifier (nothing written), -1 = a fault. F144 to F146 wrote it
// every call and F148 restored that after F147 skipped a modifier whose enable was already 0. The write is every call, as flown. It is not what keeps the backpack out of the
// view: a flight that wrote it every call still showed the pack with the head turned from the body (F151's flight, edvr_gfx_20261028_072553). The pack's own draw is left out of both
// eyes instead (F157, native_on_foot.cpp).
__declspec(noinline) int sehWriteClipPart(uint8_t* amc, uint32_t idx) noexcept {
    __try {
        uint64_t mod = 0;
        std::memcpy(&mod, amc + avh::kOffAmcParts + idx * avh::kPartStride + avh::kOffPartClipMod, 8);
        if (!plausiblePointer(mod)) return 0;
        volatile uint8_t* m = reinterpret_cast<volatile uint8_t*>(mod);
        m[avh::kClipModEnable] = 0;
        m[avh::kClipModDirty] = 1;
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}
// ---- F143: the coat's clip modifiers, measured (every part of the local third-person AMC with an instance) -------------------------------------------------------------
// Called from headHideStep for the local third-person AMC (before bodyClipStep's writes for that call): when native_on_foot_f143.h readWhy says so (the first call, a changed
// outfit, or the armed state's first change to a state in this outfit; at most kF143ReadCap a session), every part's instances, modifier pointer (set or not) and, where set, amount, enable and dirty, with the view masks, are queued for the frame
// thread, with [AMC+0x321] and [AMC+0x322]. Reads only, under SEH; three faults stop it.
void f143Fault() noexcept {
    if (g_f143Faults.fetch_add(1, std::memory_order_relaxed) + 1 >= avh::kMaxHideFaults) g_f143Down.store(true, std::memory_order_release);
}
void outfitClipStep(uint8_t* amc) noexcept {
    if (g_f143Down.load(std::memory_order_acquire)) return;
    const uint8_t armed = g_f143Armed.load(std::memory_order_relaxed);
    const uint32_t gen = g_f142OutfitGen.load(std::memory_order_relaxed);
    const bool outfitChanged = gen != g_f143SeenGen.load(std::memory_order_relaxed);
    const int why = nofoot::f143::readWhy(g_f143SeenArmed.load(std::memory_order_relaxed), armed, g_f143SeenMask.load(std::memory_order_relaxed), outfitChanged,
                                          g_f143Reads.load(std::memory_order_relaxed), kF143ReadCap);
    if (why == nofoot::f143::kReadNo) return;
    if (g_f143Busy.exchange(true, std::memory_order_acquire)) return;
    static nofoot::f143::ClipSnap snap;   // (3 KB; only this thread, under g_f143Busy)
    snap = nofoot::f143::ClipSnap{};
    snap.amc = reinterpret_cast<uint64_t>(amc);
    snap.armed = armed;
    snap.why = static_cast<uint8_t>(why);
    uint8_t flags[3] = {};
    snap.flagsOk = sehReadClipFlags(amc, flags);
    snap.flag321 = flags[0];
    snap.flag322 = flags[1];
    bool fault = false;
    for (uint32_t idx = 0; idx < avh::kPartCount && !fault; ++idx) {
        ClipPart cp;
        if (!sehReadClipPart(amc, idx, &cp)) {
            fault = true;
            break;
        }
        if (!cp.present) continue;
        nofoot::f143::ClipRow& r = snap.row[snap.n++];
        r.idx = static_cast<uint8_t>(idx);
        for (uint32_t k = 0; k < avh::kPartVariants; ++k) {
            if (cp.instances[k]) r.inst = static_cast<uint8_t>(r.inst | (1u << k));
            r.masks[k] = cp.masks[k];
        }
        r.modSet = plausiblePointer(cp.mod);
        r.readOk = cp.readOk;
        r.enable = cp.enable;
        r.dirty = cp.dirty;
        r.amount = cp.amount;
    }
    if (fault) {
        f143Fault();
    } else if (g_f143Ring.push(snap)) {   // a full ring leaves the schedule alone: the next call tries again
        g_f143SeenArmed.store(armed, std::memory_order_relaxed);
        g_f143SeenGen.store(gen, std::memory_order_relaxed);
        // a new outfit starts its armed states again; the same outfit adds this one
        const uint32_t bit = 1u << (armed < 8 ? armed : 0);
        g_f143SeenMask.store(why == nofoot::f143::kReadFirst || why == nofoot::f143::kReadOutfit ? bit : (g_f143SeenMask.load(std::memory_order_relaxed) | bit),
                             std::memory_order_relaxed);
        g_f143Reads.fetch_add(1, std::memory_order_relaxed);
    }
    g_f143Busy.store(false, std::memory_order_release);
}
// The part mask of F19's unclip list (every part kClipPartNames names, the comparison parts too: they stay clipped; F164: and the flight suit's four) and of the head parts, once.
uint64_t clipListMask(const int* idx) noexcept {
    uint64_t m = 0;
    for (size_t i = 0; i < kClipParts; ++i)
        if (idx[i] >= 0) m |= 1ull << idx[i];
    return m;
}
uint64_t headPartMask() noexcept {
    uint64_t m = 0;
    for (uint32_t h : avh::kHeadParts) m |= 1ull << h;
    return m;
}

// The hook (after the fade, beside fpBodyStep): the local third-person AMC only, identified as headHideStep does it.
void bodyClipStep(void* amcPtr) noexcept {
    if (!amcPtr) return;
    const bool unclip = g_unclipWant.load(std::memory_order_acquire);
    const bool probe = g_clipProbeWant.load(std::memory_order_acquire);
    const bool hybrid = g_hybridWant.load(std::memory_order_acquire);   // F21: the third-person side of the hybrid body
    if (!unclip && !probe && !hybrid) return;
    uint8_t* const amc = static_cast<uint8_t*>(amcPtr);
    AmcIdent id;
    if (!sehReadAmcIdent(amc, &id)) return;
    const uint64_t skeleton = g_skelIface[0].load(std::memory_order_acquire);
    if (!avh::isLocalAmc(id.id, id.pose, skeleton, id.mode)) return;
    int idx[kClipParts];
    {
        std::lock_guard<std::mutex> lock(g_fpBodyMutex);
        for (size_t i = 0; i < kClipParts; ++i) idx[i] = g_clipIdx[i];
    }
    if (probe) {   // read first: what the game left since our last write (the probe's enable reads 1 if the game re-applied)
        const uint64_t now =
#ifdef EDVR_NATIVE_ON_FOOT_HOOKS_TEST
            g_clipTestNow ? g_clipTestNow :
#endif
            GetTickCount64();
        if (now >= g_clipNextMs.load(std::memory_order_relaxed)) {
            g_clipNextMs.store(now + 5000, std::memory_order_relaxed);
            BodyClipSnap s;
            s.stamp = now;
            s.amc = reinterpret_cast<uint64_t>(amcPtr);
            s.avatarPose = id.pose;
            s.skeleton = skeleton;
            s.flagsOk = sehReadClipFlags(amc, s.flags);
            for (size_t i = 0; i < kClipParts; ++i) {
                s.part[i].named = idx[i] >= 0;
                if (idx[i] >= 0 && !sehReadClipPart(amc, static_cast<uint32_t>(idx[i]), &s.part[i])) s.part[i].readFault = true;
            }
            std::lock_guard<std::mutex> lock(g_fpBodyMutex);
            g_clipSnap = s;
        }
    }
    if (nativeOnFootCameraExternal()) {   // F93d: no mask or clip write in the game's third-person camera (the probe above still reads)
        if (unclip || hybrid) g_clipExternalSkips.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (hybrid) {   // F21: the third-person arms, hands and both shoulders are hidden by their view masks (the torso is unclipped below)
        const int maskIdx[4] = {idx[1], idx[2], idx[3], idx[4]};
        uint32_t zeroed = 0;
        if (sehZeroPartMasks(amc, maskIdx, 4, &zeroed)) {
            if (zeroed) g_hybridMaskWrites.fetch_add(zeroed, std::memory_order_relaxed);
        } else {
            g_clipWriteFaults.fetch_add(1, std::memory_order_relaxed);
        }
    }
    if (unclip || hybrid) {
        const size_t n = hybrid ? 1 : kClipParts;   // F21: the hybrid body unclips the EVASuit torso only (index 0)
        for (size_t i = 0; i < n; ++i) {
            if (i >= kClipUnclipParts && i < kClipGearFirst) continue; // comparison parts retain the game's clipping
            if (idx[i] < 0) continue;
            const int r = sehWriteClipPart(amc, static_cast<uint32_t>(idx[i]));
            if (r == 1) g_clipWrites.fetch_add(1, std::memory_order_relaxed);
            else if (r < 0) g_clipWriteFaults.fetch_add(1, std::memory_order_relaxed);
        }
    }
    // F143 (always on since F146; F147: five parts, in the coat only): the same unclip, same thread and gate (the unclip is wanted, the game's camera is first person), for the
    // coat outfit's EVASuit_Gear_UpperArmL, _UpperArmR, _Chest, _Back and _Arms (the parts the flights showed it writing), and only under F144's outer-garment decision (no
    // EVASuit_Arms instance, an EVASuit_Gear_Arms instance). Every other part keeps the game's clip. A part without a modifier is not written (0).
    if (unclip && !g_f143Down.load(std::memory_order_acquire)) {
        const uint64_t extra = nofoot::f144::unclipMask(unclipAllowedMask(), clipListMask(idx), headPartMask(), gearBackNow(amc));
        for (uint32_t i = 0; i < avh::kPartCount; ++i) {
            if (!((extra >> i) & 1ull)) continue;
            const int r = sehWriteClipPart(amc, i);
            if (r == 1) {
                g_f143ClipWrites.fetch_add(1, std::memory_order_relaxed);
                g_f143ClipPart[i].fetch_add(1, std::memory_order_relaxed);
            } else if (r < 0) {
                g_f143ClipFaults.fetch_add(1, std::memory_order_relaxed);
                f143Fault();
            }
        }
    }
}

// ---- the neck census (native on foot, F1: log only, read-only) ------------------------------------------------------------------
// Every 100 ms while native stereo is on foot, on the game's job thread: the local third-person head joint in avatar-root model space
// (skVerify, FindJoint "def_c_head_joint", the +0x58 model call), and the local AMC's origin in world space (sehAmcOrigin). Both are reads
// the game already makes. The frame thread reads the newest sample under the mutex.
struct NeckSnapshot {
    uint32_t state = 0;          // 0 not tried, 1 no skeleton latched, 2 vtable not verified, 3 no head joint, 4 head matrix not plausible, 5 head read
    uint64_t stamp = 0;          // GetTickCount64 of the sample
    float head[3] = {};          // avatar-root model space (row vectors; translation in the fourth row)
    float amc[3] = {};           // world space: the AMC's origin
    int amcState = 0;            // sehAmcOrigin: 1 read, 0 not available, -1 faulted
    uint32_t neckState = 0;      // F39: 1 read, 2 no candidate name answered, 3 matrix not plausible, 0 not tried (head not read)
    float neck[3] = {};          // F39: the neck joint, avatar-root model space (as the head)
    const char* neckName = nullptr;
    uint32_t shState = 0;        // F43: bit 0 the 3P shoulders (L then R) read, bit 1 the FP shoulders read
    float sh3P[6] = {};          // F43: the 3P shoulders, avatar-root model space (L, R)
    float shFp[6] = {};          // F43: the FP shoulders, FP avatar-root model space (L, R)
    float ext3P[12] = {};        // F45: the 3P hands (L, R) and attach points (L, R), model space
    float extFp[12] = {};        // F45: the same on the FP skeleton
    float pov3P[3] = {};         // F48: the 3P def_c_povCamera_joint, model space (shState bit 0x10)
    uint32_t povIndex = 0xFFFFu; // F48: its index (FindJoint)
};
std::mutex g_neckMutex;
NeckSnapshot g_neck;                                  // guarded by g_neckMutex
std::atomic<uint64_t> g_neckNextMs{0};
std::atomic<bool> g_busyNeck{false};

// F39: the neck joint by name on the local third-person skeleton, found the way the head is (FindJoint, then the +0x58 model matrix). The exe
// holds no neck joint name, so these are our own strings. Each is asked once per skeleton: the first that answers with a plausible matrix is
// kept; the upper spine is used only when no neck name answers. Census thread only (neckCensusStep holds g_busyNeck).
struct NeckJointCache {
    uint64_t iface = 0;
    bool tried = false;
    uint32_t index = 0xFFFFu;
    const char* name = nullptr;
};
NeckJointCache g_neckJoint;
constexpr const char* kNeckJointNames[] = {"def_c_neck_joint", "def_c_neck", "def_c_spine2_joint", "def_c_spine1_joint", "def_c_spine_joint"};
// 1 read (out = the translation of the matrix, model space), 2 no candidate answered, 3 the matrix is not plausible.
// F40: the head joint's parent in the skeleton's pose arrays (the animated pose walk, avatar_skeleton_core.h), once per skeleton (the caller caches
// the answer). The walk enumerates joint indices and parents, not names: there is no name list to log from it.
float g_neckWalkLocals[avh::kMaxWalkJoints * avh::kJointFloats];
uint16_t g_neckWalkParents[avh::kMaxWalkJoints];
bool neckWalkIndex(uintptr_t iface, const SkSlots& sl, uint32_t* out) noexcept {
    uintptr_t pose = 0;
    if (!sehCallPose(sl.fn[0], iface, &pose) || pose == 0) return false;
    uint16_t joints = 0;
    if (!sehReadU16At(pose, &joints) || joints == 0 || joints > avh::kMaxWalkJoints) return false;
    uint32_t headIdx = avh::kNoJoint;
    if (!sehCallFind(sl.fn[1], iface, avh::kHeadName, &headIdx)) return false;
    headIdx &= 0xFFFFu;
    if (headIdx >= joints) return false;
    if (!sehReadPoseArrays(pose, joints, g_neckWalkLocals, g_neckWalkParents)) return false;
    const uint16_t parent = g_neckWalkParents[headIdx];
    Log::get().note("native on foot neck census: the walk gives the head joint index %u and its parent %u (%u joints; the walk enumerates "
                    "indices and parents, not names)", static_cast<unsigned>(headIdx), static_cast<unsigned>(parent), static_cast<unsigned>(joints));
    if (parent == 0xFFFFu || parent >= joints) return false;
    *out = parent;
    return true;
}
uint32_t neckJointRead(uintptr_t iface, const SkSlots& sl, float out[3]) noexcept {
    if (!g_neckJoint.tried || g_neckJoint.iface != static_cast<uint64_t>(iface)) {
        g_neckJoint = NeckJointCache{};
        g_neckJoint.iface = static_cast<uint64_t>(iface);
        g_neckJoint.tried = true;
        for (const char* name : kNeckJointNames) {
            uint32_t idx = avh::kNoJoint;
            if (!sehCallFind(sl.fn[1], iface, name, &idx)) continue;
            idx &= 0xFFFFu;
            if (idx == 0xFFFFu) continue;
            alignas(16) float m[16] = {};
            if (!sehCallModel(sl.fn[3], iface, idx, m) || !avh::headMatrixPlausible(m)) continue;
            g_neckJoint.index = idx;
            g_neckJoint.name = name;
            break;
        }
        if (!g_neckJoint.name) {
            // F40: no candidate name answered: the head's parent in the walk.
            uint32_t walkIdx = 0xFFFFu;
            if (neckWalkIndex(iface, sl, &walkIdx)) {
                g_neckJoint.index = walkIdx;
                g_neckJoint.name = "the walk's parent of the head joint";
            }
        }
        Log::get().note("native on foot neck census: neck joint on the local third-person skeleton: %s",
                        g_neckJoint.name ? g_neckJoint.name : "none of the candidate names answered (the pov falls back to the low-passed head)");
    }
    if (g_neckJoint.index == 0xFFFFu) return 2;
    alignas(16) float m[16] = {};
    if (!sehCallModel(sl.fn[3], iface, g_neckJoint.index, m) || !avh::headMatrixPlausible(m)) return 3;
    out[0] = m[12];
    out[1] = m[13];
    out[2] = m[14];
    return 1;
}

// F43: the body anchor's joints, by name, on each latched skeleton (the 3P one and the FP one). Probed once per skeleton on the census thread:
// the probe logs every name's index (0xFFFF: none). The walk dump runs only when a side's shoulder does not answer.
// F65b: plus the feet (the legs' chains for stand armed; the R leg's indices are inferred by mirror and confirmed by this name).
constexpr size_t kJointProbeCount = 18;
constexpr const char* kJointProbeNames[kJointProbeCount] = {"def_l_shoulder_joint", "def_r_shoulder_joint", "def_l_clavicle_joint", "def_r_clavicle_joint",
    "def_l_upperArm_joint", "def_r_upperArm_joint", "def_l_arm_joint", "def_r_arm_joint", "def_c_spine03_joint", "def_l_handAttach_joint",
    "def_r_handAttach_joint", "def_l_hand_joint", "def_r_hand_joint", "def_c_neck_joint", "def_c_head_joint", "def_c_povCamera_joint",
    "def_l_foot_joint", "def_r_foot_joint"};
constexpr size_t kJointNameNeck = 13, kJointNamePov = 15, kJointNameFootL = 16, kJointNameFootR = 17;
struct ShoulderProbe {
    uint64_t iface = 0;
    bool done = false;
    uint32_t l = 0xFFFFu, r = 0xFFFFu;   // the first answered L and R shoulder: shoulder, then clavicle, then upper arm
    uint32_t neck = 0xFFFFu;
    uint32_t hl = 0xFFFFu, hr = 0xFFFFu, al = 0xFFFFu, ar = 0xFFFFu;   // F45: the hands (L, R) and the attach points (L, R)
    uint32_t pov = 0xFFFFu;                                            // F48: def_c_povCamera_joint
    uint32_t fl = 0xFFFFu, fr = 0xFFFFu;                               // F65b: the feet (L, R)
};

// F48 (census, read only): the skeleton's parent array, once per skeleton (the probe's cache). The FP skeleton: the joint count (the u16 at
// pose + kPoseJointCountOff) and every joint's parent (*(pose + 0x50), a u16 per joint), 24 joints a line. The 3P skeleton: the parents of
// joints 15 (povCamera), 34, 35, 36 and 44 (the head), then (F56) the whole array as the FP one.
void skeletonParentsDump(uint64_t iface, const SkSlots& sl, bool fp, const char* label) noexcept {
    uintptr_t pose = 0;
    uint16_t joints = 0;
    if (!sehCallPose(sl.fn[0], static_cast<uintptr_t>(iface), &pose) || pose == 0 || !sehReadU16At(pose + avh::kPoseJointCountOff, &joints)) {
        Log::get().note("native on foot skeleton parents (%s): the pose or its joint count could not be read", label);
        return;
    }
    if (joints == 0 || joints > avh::kMaxWalkJoints || !sehReadPoseArrays(pose, joints, g_neckWalkLocals, g_neckWalkParents)) {
        Log::get().note("native on foot skeleton parents (%s): %u joints; the parent array could not be read", label, static_cast<unsigned>(joints));
        return;
    }
    if (!fp) {
        const uint32_t ids[5] = {15, 34, 35, 36, 44};
        char buf[256] = {};
        size_t at = 0;
        for (uint32_t id : ids) {
            const int w = id < joints ? std::snprintf(buf + at, sizeof(buf) - at, " %u<-%u", static_cast<unsigned>(id), static_cast<unsigned>(g_neckWalkParents[id]))
                                      : std::snprintf(buf + at, sizeof(buf) - at, " %u<-(out of range)", static_cast<unsigned>(id));
            if (w > 0 && static_cast<size_t>(w) < sizeof(buf) - at) at += static_cast<size_t>(w);
        }
        Log::get().note("native on foot skeleton parents (%s): %u joints; joint<-parent:%s", label, static_cast<unsigned>(joints), buf);
        // F56: the 3P skeleton's whole parent array follows too (the arm IK's chains are derived from it; the indices are known offline).
    }
    Log::get().note("native on foot skeleton parents (%s): %u joints (pose + 0x%X); the parent array follows, joint<-parent, 0xFFFF the root",
                    label, static_cast<unsigned>(joints), static_cast<unsigned>(avh::kPoseJointCountOff));
    for (uint32_t first = 0; first < joints; first += 24) {
        char buf[512] = {};
        size_t at = 0;
        for (uint32_t i = first; i < joints && i < first + 24; ++i) {
            const int w = std::snprintf(buf + at, sizeof(buf) - at, " %u<-%u", static_cast<unsigned>(i), static_cast<unsigned>(g_neckWalkParents[i]));
            if (w > 0 && static_cast<size_t>(w) < sizeof(buf) - at) at += static_cast<size_t>(w);
        }
        Log::get().note("native on foot skeleton parents (%s): joints %u..%u:%s", label, static_cast<unsigned>(first),
                        static_cast<unsigned>(std::min<uint32_t>(joints, first + 24) - 1), buf);
    }
}
ShoulderProbe g_shProbe3P, g_shProbeFP;   // census thread only

// The walk dump (no shoulder answered): the 30 joints nearest the neck by index, with name hash, depth and parent, once per skeleton.
void shoulderWalkDump(uint64_t iface, const SkSlots& sl, uint32_t neck, const char* label) noexcept {
    uintptr_t pose = 0;
    if (!sehCallPose(sl.fn[0], static_cast<uintptr_t>(iface), &pose) || pose == 0) return;
    uint16_t joints = 0;
    if (!sehReadU16At(pose, &joints) || joints == 0 || joints > avh::kMaxWalkJoints) return;
    if (!sehReadPoseArrays(pose, joints, g_neckWalkLocals, g_neckWalkParents)) return;
    const uint32_t centre = neck < joints ? neck : static_cast<uint32_t>(joints / 2);
    std::vector<uint32_t> order;
    for (uint32_t i = 0; i < joints; ++i) order.push_back(i);
    std::stable_sort(order.begin(), order.end(), [centre](uint32_t a, uint32_t b) {
        const uint32_t da = a > centre ? a - centre : centre - a, db = b > centre ? b - centre : centre - b;
        return da < db;
    });
    const size_t shown = std::min<size_t>(30, order.size());
    for (size_t k = 0; k < shown; ++k) {
        const uint32_t i = order[k];
        uint16_t hash = 0;
        sehReadU16At(pose + 0x18 + 2u * i, &hash);
        uint32_t depth = 0;
        uint16_t p = g_neckWalkParents[i];
        while (p != 0xFFFFu && p < joints && depth < joints) {
            ++depth;
            p = g_neckWalkParents[p];
        }
        Log::get().note("native on foot joint walk (%s): joint %u name hash 0x%04X depth %u parent %u%s", label, static_cast<unsigned>(i),
                        static_cast<unsigned>(hash), static_cast<unsigned>(depth), static_cast<unsigned>(g_neckWalkParents[i]),
                        i == centre ? " (the neck's index)" : "");
    }
}

// F43: the probe for one skeleton, once per skeleton (the iface is the cache key).
void shoulderProbe(uint64_t iface, const SkSlots& sl, ShoulderProbe& pr, const char* label) noexcept {
    if (pr.done && pr.iface == iface) return;
    pr = ShoulderProbe{};
    pr.iface = iface;
    pr.done = true;
    uint32_t found[kJointProbeCount] = {};
    for (size_t i = 0; i < kJointProbeCount; ++i) {
        uint32_t idx = avh::kNoJoint;
        found[i] = sehCallFind(sl.fn[1], static_cast<uintptr_t>(iface), kJointProbeNames[i], &idx) ? (idx & 0xFFFFu) : 0xFFFFu;
        if (found[i] == 0xFFFFu)
            Log::get().note("native on foot joint probe (%s): %s -> none", label, kJointProbeNames[i]);
        else
            Log::get().note("native on foot joint probe (%s): %s -> index %u", label, kJointProbeNames[i], static_cast<unsigned>(found[i]));
    }
    const size_t lc[3] = {0, 2, 4}, rc[3] = {1, 3, 5};
    for (size_t c : lc)
        if (pr.l == 0xFFFFu && found[c] != 0xFFFFu) pr.l = found[c];
    for (size_t c : rc)
        if (pr.r == 0xFFFFu && found[c] != 0xFFFFu) pr.r = found[c];
    pr.neck = found[kJointNameNeck];
    pr.hl = found[11];
    pr.hr = found[12];
    pr.al = found[9];
    pr.ar = found[10];
    pr.pov = found[kJointNamePov];
    pr.fl = found[kJointNameFootL];
    pr.fr = found[kJointNameFootR];
    if (pr.l == 0xFFFFu || pr.r == 0xFFFFu) shoulderWalkDump(iface, sl, pr.neck, label);
    skeletonParentsDump(iface, sl, &pr == &g_shProbeFP, label);   // F48 (census): once per skeleton
}

// F45: a joint matrix is usable when every element is finite and of ordinary size (the head's placement test rejected the joints).
bool jointMatrixSane(const float m[16]) noexcept {
    for (int i = 0; i < 16; ++i)
        if (!std::isfinite(m[i]) || std::fabs(m[i]) > 100.0f) return false;
    return true;
}
// F43: one joint's model-space translation (the +0x58 model call, as the head and the neck), or false.
bool jointModelAt(uint64_t iface, const SkSlots& sl, uint32_t idx, float out[3]) noexcept {
    alignas(16) float m[16] = {};
    if (!sehCallModel(sl.fn[3], static_cast<uintptr_t>(iface), idx, m) || !jointMatrixSane(m)) return false;
    out[0] = m[12];
    out[1] = m[13];
    out[2] = m[14];
    return true;
}

// ---- F49 (TEMPORARY): the pose freeze's census side ----------------------------------------------------------------------------
// Census thread only (neckCensusStep holds g_busyNeck). Once per evaluator X (from the local third-person skeleton interface: RR *(iface+0x2B8),
// AO *(iface+0x120), the readers' own choice, notes 0x43FE420 / 0xFDEDB0): P = [X+8], its joint count, rest locals and parents; FindJoint
// ("def_l_foot_joint"); the frozen set (nofoot::pfzFrozenSet); the plan published under pfz::g_planSeq; the rest chain checked against the game's
// own 0xFDF170 (a leaf: it reads P's locals and parents and writes 64 bytes to its second argument). Every 5 s: GetModelMatrix(34) for the frame
// thread's cross-check against the hook's span. Reads and the game's own read-only calls only: nothing here writes game memory.
constexpr uintptr_t kPfzRestModelRva = 0xFDF170;
const uint8_t kPfzRestModelPrologue[11] = {0x48, 0x83, 0xEC, 0x48, 0x48, 0x8B, 0x41, 0x48, 0x4D, 0x8B, 0xC8};
constexpr uint32_t kPfzNeck = 34, kPfzHead = 44, kPfzClavL = 35, kPfzClavR = 36;
using PfzRestModelFn = void(__fastcall*)(void* pose, float* out, uint64_t idx);
__declspec(noinline) bool sehCallRestModel(uintptr_t fn, uintptr_t pose, uint32_t idx, float* out) noexcept {
    __try {
        reinterpret_cast<PfzRestModelFn>(fn)(reinterpret_cast<void*>(pose), out, idx);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
__declspec(noinline) bool sehReadBytesAt(uintptr_t at, void* out, size_t n) noexcept {
    __try {
        std::memcpy(out, reinterpret_cast<const void*>(at), n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
struct PfzCensus {
    uint64_t iface = 0, x = 0;
    bool xNoted = false;
    uint64_t modelNextMs = 0;
    bool modelNoted = false;
    bool planOk = false;   // the plan for this X was built and published (a failed build is refused for this X, said once)
    bool armOk = false;    // F56: the arm IK's plan for this X has at least one side
};
PfzCensus g_pfzCensus;
float g_pfzLocals[nofoot::kPfzMaxJoints * avh::kJointFloats];
uint16_t g_pfzParents[nofoot::kPfzMaxJoints];

// The largest element difference of two 4x4 matrices, and the position difference in mm.
void pfzMatrixDiff(const float a[16], const float b[16], double* maxElem, double* posMm) noexcept {
    double m = 0.0;
    for (int i = 0; i < 16; ++i) m = std::max(m, std::fabs(static_cast<double>(a[i]) - b[i]));
    const double dx = static_cast<double>(a[12]) - b[12], dy = static_cast<double>(a[13]) - b[13], dz = static_cast<double>(a[14]) - b[14];
    *maxElem = m;
    *posMm = std::sqrt(dx * dx + dy * dy + dz * dz) * 1000.0;
}

void pfzBuildPlan(uint64_t iface, uint64_t x, const SkSlots& sl) noexcept {
    pfz::g_localX.store(0, std::memory_order_release);
    uint64_t pose = 0;
    if (!sehReadU64At(static_cast<uintptr_t>(x) + 8u, &pose) || !plausibleUserPointer(pose)) {
        Log::get().note("native on foot pose freeze: X=%016llX: P = [X+8] is not readable; refused for this X (nothing is frozen or probed)",
                        static_cast<unsigned long long>(x));
        return;
    }
    uintptr_t gp = 0;
    const bool gpOk = sehCallPose(sl.fn[0], static_cast<uintptr_t>(iface), &gp);
    uint16_t joints = 0;
    if (!sehReadU16At(static_cast<uintptr_t>(pose), &joints) || joints == 0 || joints > nofoot::kPfzMaxJoints ||
        !sehReadPoseArrays(static_cast<uintptr_t>(pose), joints, g_pfzLocals, g_pfzParents)) {
        Log::get().note("native on foot pose freeze: X=%016llX P=%016llX: %u joints (cap 0xC0) or the locals and parents unreadable; refused for this X",
                        static_cast<unsigned long long>(x), static_cast<unsigned long long>(pose), static_cast<unsigned>(joints));
        return;
    }
    uint32_t foot = avh::kNoJoint;
    const bool findOk = sehCallFind(sl.fn[1], static_cast<uintptr_t>(iface), avh::kFootLName, &foot);
    foot &= 0xFFFFu;
    if (!findOk || foot >= joints) {
        Log::get().note("native on foot pose freeze: FindJoint(\"%s\") on the 3P skeleton %s; the anchor is the root's child on the neck's path",
                        avh::kFootLName, findOk ? "found no joint (or an index past the joint count)" : "faulted");
        foot = avh::kNoJoint;
    }
    const nofoot::PfzFrozenSet fs = nofoot::pfzFrozenSet(g_pfzParents, joints, kPfzNeck, foot);
    if (fs.why != 0) {
        Log::get().note("native on foot pose freeze: X=%016llX: no frozen set (%s; %u joints, neck %u, foot %u); refused for this X",
                        static_cast<unsigned long long>(x), nofoot::pfzWhyText(fs.why), static_cast<unsigned>(joints), kPfzNeck, foot);
        return;
    }
    char list[1024] = {};
    size_t at = 0;
    for (uint32_t k = 0; k < fs.count; ++k) {
        const int w = std::snprintf(list + at, sizeof(list) - at, "%s%u", k ? " " : "", static_cast<unsigned>(fs.member[k]));
        if (w <= 0 || static_cast<size_t>(w) >= sizeof(list) - at) break;
        at += static_cast<size_t>(w);
    }
    Log::get().note("native on foot pose freeze: plan for X=%016llX (iface %016llX, %s): P=[X+8]=%016llX (GetPoseData %s), %u joints; neck %u (the "
                    "joint probe's neck: %u), foot %s %u; anchor a=%u (the lowest common ancestor%s), torso child %u; F has %u members; clavicle 35 %s F, "
                    "clavicle 36 %s F, neck 34 %s F, head 44 %s F; members parent first: %s",
                    static_cast<unsigned long long>(x), static_cast<unsigned long long>(iface), avh::hkindText(static_cast<uint32_t>(sl.kind)),
                    static_cast<unsigned long long>(pose),
                    !gpOk ? "faulted" : gp == pose ? "the same" : "DIFFERS", static_cast<unsigned>(joints), kPfzNeck,
                    static_cast<unsigned>(g_shProbe3P.neck), avh::kFootLName, foot, fs.anchor,
                    fs.footResolved ? "" : "; the foot did not resolve: the root's child on the neck's path", fs.child, fs.count,
                    fs.slotOf(kPfzClavL) >= 0 ? "in" : "NOT in", fs.slotOf(kPfzClavR) >= 0 ? "in" : "NOT in", fs.slotOf(kPfzNeck) >= 0 ? "in" : "NOT in",
                    fs.slotOf(kPfzHead) >= 0 ? "in" : "NOT in", list);
    // The cross-check: the rest chain composed here against the game's own uncached path, for the neck and the head (read only).
    {
        const uintptr_t fn = g_skelTargets.base ? g_skelTargets.base + kPfzRestModelRva : 0;
        uint8_t bytes[sizeof(kPfzRestModelPrologue)] = {};
        if (!fn || !sehReadBytesAt(fn, bytes, sizeof(bytes)) || std::memcmp(bytes, kPfzRestModelPrologue, sizeof(bytes)) != 0) {
            Log::get().note("native on foot pose freeze: rest chain cross-check: not available (0xFDF170's entry bytes are not build 334925's)");
        } else {
            const uint32_t ids[2] = {kPfzNeck, kPfzHead};
            for (uint32_t id : ids) {
                alignas(16) float game[16] = {}, ours[16] = {};
                const bool okGame = id < joints && sehCallRestModel(fn, static_cast<uintptr_t>(pose), id, game);
                const bool okOurs = nofoot::pfzRestModel(g_pfzLocals, g_pfzParents, joints, id, ours);
                if (!okGame || !okOurs) {
                    Log::get().note("native on foot pose freeze: rest chain cross-check joint %u: %s", id, !okGame ? "the game's call faulted or the index is out of range"
                                                                                                                 : "our walk failed");
                    continue;
                }
                double maxElem = 0.0, posMm = 0.0;
                pfzMatrixDiff(game, ours, &maxElem, &posMm);
                Log::get().note("native on foot pose freeze: rest chain cross-check joint %u: 0xFDF170 pos (%.4f,%.4f,%.4f), ours (%.4f,%.4f,%.4f); "
                                "max element difference %.6f, position %.3f mm (%s)", id, static_cast<double>(game[12]), static_cast<double>(game[13]),
                                static_cast<double>(game[14]), static_cast<double>(ours[12]), static_cast<double>(ours[13]), static_cast<double>(ours[14]),
                                maxElem, posMm, maxElem < 1e-3 ? "agree" : "DIFFER");
            }
        }
    }
    pfz::g_planSeq.fetch_add(1, std::memory_order_acq_rel);   // odd: the plan is being written
    pfz::g_plan.x = x;
    pfz::g_plan.joints = joints;
    pfz::g_plan.anchor = fs.anchor;
    pfz::g_plan.count = fs.count;
    pfz::g_plan.slot34 = fs.slotOf(kPfzNeck);
    pfz::g_plan.slot44 = fs.slotOf(kPfzHead);
    for (uint32_t k = 0; k < fs.count; ++k) {
        pfz::g_plan.member[k] = fs.member[k];
        pfz::g_plan.parentSlot[k] = fs.parentSlot[k];
        std::memcpy(pfz::g_plan.rest[k], g_pfzLocals + fs.member[k] * avh::kJointFloats, sizeof(pfz::g_plan.rest[k]));
    }
    pfz::g_planSeq.fetch_add(1, std::memory_order_release);   // even again
    g_pfzCensus.planOk = true;
    pfz::g_localX.store(x, std::memory_order_release);
}

// F51: one latched skeleton's evaluator candidates, published for the frame thread's 5 s line (slot 0 the local 3P skeleton, 1 the FP one): X =
// *(iface+0x2B8) (the RR rule) and *(iface+0x120) (the AO rule), each with P = [X+8], against the iface's GetPoseData; [iface+0x2C0] and
// [iface+0xF8] (the readers' gates). The X the pose freeze takes: the kind's own X when the Animate hook has counted it in the last 5 to 10 s;
// else the one X the hook counted whose [X+8] is GetPoseData's pointer; else (no match, or several X share P: the skeleton asset may be shared
// by every humanoid) the kind's own X, the F49 behaviour. Read only; every read under SEH.
uint64_t pfzCandidates(uint32_t slot, uint64_t iface, const SkSlots& sl) noexcept {
    pfz::Candidates& c = pfz::g_cand[slot & 1u];
    const uintptr_t ifc = static_cast<uintptr_t>(iface);
    uint64_t x2B8 = 0, x120 = 0, p2B8 = 0, p120 = 0, f2C0 = 0, fF8 = 0;
    const bool ok2B8 = sehReadU64At(ifc + 0x2B8, &x2B8) && plausibleUserPointer(x2B8);
    const bool ok120 = sehReadU64At(ifc + 0x120, &x120) && plausibleUserPointer(x120);
    if (ok2B8 && !sehReadU64At(static_cast<uintptr_t>(x2B8) + 8u, &p2B8)) p2B8 = 0;
    if (ok120 && !sehReadU64At(static_cast<uintptr_t>(x120) + 8u, &p120)) p120 = 0;
    if (!sehReadU64At(ifc + 0x2C0, &f2C0)) f2C0 = ~0ull;
    if (!sehReadU64At(ifc + 0xF8, &fF8)) fF8 = ~0ull;
    uintptr_t gp = 0;
    if (!sehCallPose(sl.fn[0], ifc, &gp)) gp = 0;
    const uint64_t direct = sl.kind == avh::HeadKind::Runtime ? (ok2B8 ? x2B8 : 0) : (ok120 ? x120 : 0);
    uint64_t chosen = 0, how = 0, matches = 0;
    if (direct && pfz::xCount(direct) > 0) {
        chosen = direct;
        how = 1;
    } else {
        uint64_t found[4] = {};
        if (gp) {
            for (uint32_t b = 0; b < 2; ++b)
                for (uint32_t i = 0; i < pfz::kXSlots; ++i) {
                    const uint64_t x = pfz::g_xTable[b][i].x.load(std::memory_order_relaxed);
                    if (!plausibleUserPointer(x)) continue;
                    bool dup = false;
                    for (uint64_t k = 0; k < matches && k < 4; ++k) dup = dup || found[k] == x;
                    if (dup) continue;
                    uint64_t p = 0;
                    if (!sehReadU64At(static_cast<uintptr_t>(x) + 8u, &p) || p != gp) continue;
                    if (matches < 4) found[matches] = x;
                    ++matches;
                }
        }
        if (matches == 1) {
            chosen = found[0];
            how = 2;
        } else {
            chosen = direct;
            how = matches > 1 ? 4 : direct ? 3 : 0;
        }
    }
    c.iface.store(iface, std::memory_order_relaxed);
    c.kind.store(static_cast<uint64_t>(sl.kind), std::memory_order_relaxed);
    c.x2B8.store(x2B8, std::memory_order_relaxed);
    c.x120.store(x120, std::memory_order_relaxed);
    c.f2C0.store(f2C0, std::memory_order_relaxed);
    c.fF8.store(fF8, std::memory_order_relaxed);
    c.gp.store(gp, std::memory_order_relaxed);
    c.p2B8.store(p2B8, std::memory_order_relaxed);
    c.p120.store(p120, std::memory_order_relaxed);
    c.chosen.store(chosen, std::memory_order_relaxed);
    c.pMatches.store(matches, std::memory_order_relaxed);
    c.how.store(how, std::memory_order_relaxed);
    c.stamp.store(GetTickCount64(), std::memory_order_relaxed);
    return chosen;
}

// F55 (TEMPORARY): the FP shoulder drop's plan, once per (FP iface, FP X): P = [X+8], its joint count and parents, the subtree of the clavicles
// 19 and 20 (nofoot::dropSubtree), published under pfz::g_fdSeq. Refused (the plan's X stays 0) when X is not plausible, is the 3P local X, or
// the pose cannot be read. Census thread only; reads only.
struct FdCensus {
    uint64_t iface = 0, x = 0;
};
FdCensus g_fdCensus;
uint16_t g_fdMembers[nofoot::kPfzMaxF];
bool deriveHandRig(uint32_t joints, uint32_t hand, uint32_t attach, int side, const float* locals, const uint16_t* parents, const uint16_t* names,
                   pfz::HandRig* out) noexcept;
const char* handRigWhy(uint8_t why) noexcept;
void fdPublish(uint64_t x, uint32_t joints, uint32_t count, const pfz::HandRig* hands = nullptr, uint8_t handCount = 0) noexcept {
    pfz::g_fdX.store(0, std::memory_order_release);
    const uint32_t generation = pfz::g_fdSeq.load(std::memory_order_relaxed) + 2u;
    pfz::g_fdSeq.fetch_add(1, std::memory_order_acq_rel);
    pfz::g_fdPlan.x = x;
    pfz::g_fdPlan.generation = generation;
    pfz::g_fdPlan.joints = joints;
    pfz::g_fdPlan.count = count;
    pfz::g_fdPlan.handCount = handCount;
    if (hands && handCount == 2) std::memcpy(pfz::g_fdPlan.hand, hands, sizeof(pfz::g_fdPlan.hand));
    else std::memset(pfz::g_fdPlan.hand, 0, sizeof(pfz::g_fdPlan.hand));
    for (uint32_t k = 0; k < count && k < nofoot::kPfzMaxF; ++k) pfz::g_fdPlan.member[k] = g_fdMembers[k];
    pfz::g_fdSeq.fetch_add(1, std::memory_order_release);
    pfz::g_fdX.store(count > 0 ? x : 0, std::memory_order_release);
}
void fdCensusStep(uint64_t iface, uint64_t x) noexcept {
    if (iface == g_fdCensus.iface && x == g_fdCensus.x) return;
    g_fdCensus.iface = iface;
    g_fdCensus.x = x;
    fdPublish(0, 0, 0);
    static uint32_t notes = 0;   // the plan lines, at most 24 a session (a flapping candidate must not flood the log)
    if (++notes > 24) {
        if (notes == 25) Log::get().note("native on foot shoulder drop (F55): the FP X changed again; further plan lines are not printed");
        if (notes > 25) notes = 25;
    }
    const bool say = notes <= 24;
    if (!plausibleUserPointer(x) || x == pfz::g_localX.load(std::memory_order_relaxed)) {
        if (say)
            Log::get().note("native on foot shoulder drop (F55): FP X %016llX (iface %016llX) is %s; no plan (nothing is dropped or snapshotted)",
                        static_cast<unsigned long long>(x), static_cast<unsigned long long>(iface),
                        plausibleUserPointer(x) ? "the 3P local X" : "not a plausible pointer");
        return;
    }
    uint64_t pose = 0;
    uint16_t joints = 0;
    if (!sehReadU64At(static_cast<uintptr_t>(x) + 8u, &pose) || !plausibleUserPointer(pose) || !sehReadU16At(static_cast<uintptr_t>(pose), &joints) ||
        joints <= nofoot::kDropHandR || joints > nofoot::kPfzMaxJoints ||
        !sehReadPoseArrays(static_cast<uintptr_t>(pose), joints, g_pfzLocals, g_pfzParents)) {
        if (say)
            Log::get().note("native on foot shoulder drop (F55): FP X %016llX: P = [X+8] %016llX, %u joints (need 45..192) or the parents unreadable; no plan",
                        static_cast<unsigned long long>(x), static_cast<unsigned long long>(pose), static_cast<unsigned>(joints));
        return;
    }
    const uint32_t count = nofoot::dropSubtree(g_pfzParents, joints, nofoot::kDropClavL, nofoot::kDropClavR, g_fdMembers, nofoot::kPfzMaxF);
    uint64_t nameTable = 0;
    uint16_t names[nofoot::kPfzMaxJoints] = {};
    bool namesOk = sehReadU64At(static_cast<uintptr_t>(pose) + 0x18u, &nameTable) && plausibleUserPointer(nameTable);
    for (uint32_t j = 0; j < joints && namesOk; ++j) namesOk = sehReadU16At(static_cast<uintptr_t>(nameTable) + 2u * j, &names[j]);
    pfz::HandRig hands[2]{};
    bool handL = false, handR = false;
    if (namesOk) {
        handL = deriveHandRig(joints, nofoot::kDropHandL, nofoot::kIkFpJoints[1], 0, g_pfzLocals, g_pfzParents, names, &hands[0]);
        handR = deriveHandRig(joints, nofoot::kDropHandR, nofoot::kIkFpJoints[0], 1, g_pfzLocals, g_pfzParents, names, &hands[1]);
    } else { hands[0].why = hands[1].why = 3; }
    bool has37 = false, has44 = false;
    char list[1024] = {};
    size_t at = 0;
    for (uint32_t k = 0; k < count; ++k) {
        has37 = has37 || g_fdMembers[k] == nofoot::kDropHandL;
        has44 = has44 || g_fdMembers[k] == nofoot::kDropHandR;
        const int w = std::snprintf(list + at, sizeof(list) - at, "%s%u", k ? " " : "", static_cast<unsigned>(g_fdMembers[k]));
        if (w > 0 && static_cast<size_t>(w) < sizeof(list) - at) at += static_cast<size_t>(w);
    }
    if (say)
        Log::get().note("native on foot shoulder drop (F55): plan for the FP X %016llX (iface %016llX): P=[X+8]=%016llX, %u joints; 19<-%u, 20<-%u; "
                    "the subtree of 19 and 20 has %u joints (hand 37 %s, hand 44 %s); the FP joint probe's L/R shoulder: %u/%u; members: %s",
                    static_cast<unsigned long long>(x), static_cast<unsigned long long>(iface), static_cast<unsigned long long>(pose),
                    static_cast<unsigned>(joints), static_cast<unsigned>(g_pfzParents[nofoot::kDropClavL]),
                    static_cast<unsigned>(g_pfzParents[nofoot::kDropClavR]), count, has37 ? "in" : "NOT in", has44 ? "in" : "NOT in",
                    static_cast<unsigned>(g_shProbeFP.l), static_cast<unsigned>(g_shProbeFP.r), at ? list : "none");
    if (count > 0) fdPublish(x, joints, count, hands, 2);
    if (say) Log::get().note("native on foot hand anatomy (F70b): FP X %016llX: name IDs %s; left %s (%s, %u joints, %.1f x %.1f cm, radius %.1f mm), right %s (%s, %u joints, %.1f x %.1f cm, radius %.1f mm); optional plan, legacy 11 matrices remain available",
                             static_cast<unsigned long long>(x), namesOk ? "read" : "unreadable",
                             handL ? "ok" : "refused", handRigWhy(hands[0].why), hands[0].count, 100.0 * hands[0].width, 100.0 * hands[0].length, 1000.0 * hands[0].skinRadius,
                             handR ? "ok" : "refused", handRigWhy(hands[1].why), hands[1].count, 100.0 * hands[1].width, 100.0 * hands[1].length, 1000.0 * hands[1].skinRadius);
}

// F56 (TEMPORARY): the arm IK's plan, once per local 3P X: P = [X+8], its parents; per side the chain C (the joint probe's clavicle), U, F, H
// (the hand), A (the hand attach) and the groups gU, gF, gH (nofoot::ikArmChain), published under pfz::g_armSeq. The plan line prints, per side,
// the chain, its depths, the group sizes and members, and the children of U and F with their projected t along U->F and F->H at the rest pose.
// Census thread only; reads only.
void armAppend(char* buf, size_t cap, size_t* at, const char* fmt, ...) noexcept {
    if (*at >= cap) return;
    va_list ap;
    va_start(ap, fmt);
    const int w = std::vsnprintf(buf + *at, cap - *at, fmt, ap);
    va_end(ap);
    if (w > 0) *at = std::min(cap - 1, *at + static_cast<size_t>(w));
}
bool handAncestor(const uint16_t* parents, uint32_t n, uint32_t j, uint32_t ancestor) noexcept {
    for (uint32_t k = 0; k < n && j < n; ++k) {
        if (j == ancestor) return true;
        const uint16_t p = parents[j];
        if (p == 0xFFFFu) return false;
        j = p;
    }
    return false;
}
const char* handRigWhy(uint8_t why) noexcept {
    switch (why) {
        case 0: return "ok";
        case 1: return "bad plan or unreadable rest transform";
        case 2: return "missing, extra, forked, or inconsistent finger branch";
        case 3: return "missing interned joint name";
        case 4: return "degenerate palm geometry, dimensions, or inverse";
        default: return "unknown refusal";
    }
}
bool deriveHandRig(uint32_t joints, uint32_t hand, uint32_t attach, int side, const float* locals, const uint16_t* parents, const uint16_t* names,
                   pfz::HandRig* out) noexcept {
    if (!out) return false;
    out->why = 1;
    // Census-worker only: fdCensusStep and armBuildPlan reach this routine
    // serially from the avatar census callback. Keep failure and success records
    // separate so a transient refusal cannot hide later usable landmarks.
    static uint64_t f74Failed[8] = {}, f74Succeeded[8] = {};
    static uint32_t f74FailedCount = 0, f74SucceededCount = 0;
    const bool f74IdentityReadable = locals && parents && names && hand < joints && attach < joints && joints <= nofoot::kPfzMaxJoints;
    const uint64_t f74Key = (static_cast<uint64_t>(joints) << 32) ^ (static_cast<uint64_t>(hand) << 16) ^
                            (static_cast<uint64_t>(attach) << 1) ^ static_cast<uint64_t>(side & 1) ^
                            (static_cast<uint64_t>(f74IdentityReadable ? names[hand] : 0) << 48);
    const auto f74First = [&](bool success) noexcept {
        uint64_t* seen = success ? f74Succeeded : f74Failed;
        uint32_t& count = success ? f74SucceededCount : f74FailedCount;
        for (uint32_t i = 0; i < count; ++i) if (seen[i] == f74Key) return false;
        if (count >= 8) return false;
        seen[count++] = f74Key;
        return true;
    };
    if (!f74IdentityReadable) {
        if (f74First(false)) Log::get().note("native on foot hand landmarks (F74): rig hand %u side %s: data unavailable (invalid arrays or indices)",
                                       static_cast<unsigned>(hand), side ? "R" : "L");
        return false;
    }
    pfz::HandRig r{};
    const auto refuse = [&](uint8_t why) noexcept {
        r.why = why; *out = r;
        if (f74First(false)) Log::get().note("native on foot hand landmarks (F74): rig hand %u side %s joints %u: data unavailable (%s)",
                                       static_cast<unsigned>(hand), side ? "R" : "L", static_cast<unsigned>(joints), handRigWhy(why));
        return false;
    };
    r.hand = static_cast<uint16_t>(hand); r.attach = static_cast<uint16_t>(attach);
    r.joint[0] = static_cast<uint16_t>(hand); r.name[0] = names[hand]; r.parentSlot[0] = 0xFFFFu; r.count = 1;
    if (!names[hand]) return refuse(3);
    alignas(16) float M[nofoot::kPfzMaxJoints][16] = {};
    for (uint32_t j = 0; j < joints; ++j)
        if (!nofoot::pfzRestModel(locals, parents, joints, j, M[j])) return refuse(1);
    uint16_t branches[5][4] = {};
    uint8_t lengths[5] = {};
    uint32_t branchN = 0;
    for (uint32_t root = 0; root < joints; ++root) {
        if (parents[root] != hand || root == attach || handAncestor(parents, joints, root, attach)) continue;
        uint16_t candidate[4] = {};
        uint32_t cur = root, len = 0;
        bool chainOk = true;
        while (len < 4) {
            candidate[len++] = static_cast<uint16_t>(cur);
            uint32_t child = joints, children = 0;
            for (uint32_t q = 0; q < joints; ++q)
                if (parents[q] == cur && q != attach && !handAncestor(parents, joints, q, attach)) { child = q; ++children; }
            if (!children) break;
            if (children != 1) { chainOk = false; break; }
            cur = child;
        }
        if (chainOk && len == 4) {
            for (uint32_t q = 0; q < joints; ++q)
                if (parents[q] == cur && q != attach && !handAncestor(parents, joints, q, attach)) chainOk = false;
        }
        if (!chainOk || (len != 3 && len != 4)) continue;
        if (branchN >= 5) return refuse(2);
        std::memcpy(branches[branchN], candidate, sizeof(candidate));
        lengths[branchN++] = static_cast<uint8_t>(len);
    }
    if (branchN != 5) return refuse(2);
    uint32_t thumb = 5, longN = 0;
    for (uint32_t b = 0; b < 5; ++b) {
        if (lengths[b] == 3) { if (thumb != 5) return refuse(2); thumb = b; }
        else if (lengths[b] == 4) ++longN;
    }
    if (thumb == 5 || longN != 4) return refuse(2);
    double W[3] = {M[hand][12], M[hand][13], M[hand][14]}, T[3] = {}, K[3] = {}, K2[3] = {};
    for (int c = 0; c < 3; ++c) T[c] = M[branches[thumb][0]][12 + c];
    for (uint32_t b = 0; b < 5; ++b) if (b != thumb)
        for (int c = 0; c < 3; ++c) { K[c] += M[branches[b][0]][12 + c] * 0.25; K2[c] += M[branches[b][1]][12 + c] * 0.25; }
    double distal[3] = {K[0] - W[0], K[1] - W[1], K[2] - W[2]}, radial[3] = {T[0] - K[0], T[1] - K[1], T[2] - K[2]};
    if (!nofoot::ikUnit(distal, distal)) return refuse(4);
    const double along = nofoot::ikDot(radial, distal);
    for (int c = 0; c < 3; ++c) radial[c] -= along * distal[c];
    if (!nofoot::ikUnit(radial, radial)) return refuse(4);
    double normal[3]; nofoot::ikCross(distal, radial, normal);
    if (!nofoot::ikUnit(normal, normal)) return refuse(4);
    r.palmarSign = pfz::handPalmarSign(side);   // physical palm-facing side; the stored basis remains proper in both rigs
    float palmModel[16], invHand[16];
    nofoot::pfzIdentity(palmModel);
    for (int c = 0; c < 3; ++c) { palmModel[c] = static_cast<float>(distal[c]); palmModel[4 + c] = static_cast<float>(radial[c]); palmModel[8 + c] = static_cast<float>(normal[c]);
                                  palmModel[12 + c] = static_cast<float>((W[c] + K[c]) * 0.5); }
    double lo = std::numeric_limits<double>::infinity(), hi = -lo, rootCoord[4] = {};
    uint32_t branchOrder[5] = {thumb, 0, 0, 0, 0};
    uint32_t bi = 0;
    for (uint32_t b = 0; b < 5; ++b) if (b != thumb) {
        const double p[3] = {M[branches[b][0]][12] - K[0], M[branches[b][0]][13] - K[1], M[branches[b][0]][14] - K[2]};
        rootCoord[bi] = nofoot::ikDot(p, radial); branchOrder[bi + 1] = b; lo = std::min(lo, rootCoord[bi]); hi = std::max(hi, rootCoord[bi]); ++bi;
    }
    for (uint32_t i = 1; i < 5; ++i)
        for (uint32_t j = i + 1; j < 5; ++j) {
            const double a[3] = {M[branches[branchOrder[i]][0]][12] - K[0], M[branches[branchOrder[i]][0]][13] - K[1], M[branches[branchOrder[i]][0]][14] - K[2]};
            const double b[3] = {M[branches[branchOrder[j]][0]][12] - K[0], M[branches[branchOrder[j]][0]][13] - K[1], M[branches[branchOrder[j]][0]][14] - K[2]};
            if (nofoot::ikDot(a, radial) > nofoot::ikDot(b, radial)) std::swap(branchOrder[i], branchOrder[j]);
        }
    std::sort(rootCoord, rootCoord + 4);
    double spacing[3] = {rootCoord[1]-rootCoord[0], rootCoord[2]-rootCoord[1], rootCoord[3]-rootCoord[2]};
    std::sort(spacing, spacing + 3);
    r.width = static_cast<float>(hi - lo);
    const double palmLength[3] = {K[0]-W[0], K[1]-W[1], K[2]-W[2]};
    r.length = static_cast<float>(nofoot::ikLen(palmLength));
    const double secondLengthVec[3] = {K2[0]-W[0], K2[1]-W[1], K2[2]-W[2]};
    const double secondLength = nofoot::ikLen(secondLengthVec);
    double secondLo = std::numeric_limits<double>::infinity(), secondHi = -secondLo;
    for (uint32_t b = 0; b < 5; ++b) if (b != thumb) {
        const double p[3] = {M[branches[b][1]][12] - K[0], M[branches[b][1]][13] - K[1], M[branches[b][1]][14] - K[2]};
        const double coord = nofoot::ikDot(p, radial);
        secondLo = std::min(secondLo, coord); secondHi = std::max(secondHi, coord);
    }
    const double secondWidth = secondHi - secondLo;
    r.skinRadius = static_cast<float>(spacing[1] * 0.5);
    if (!(r.width > 0.015f && r.width < 0.20f && r.length > 0.015f && r.length < 0.25f && r.skinRadius > 0.001f && r.skinRadius < 0.04f)) return refuse(4);
    float invPalmLocal[16];
    if (!nofoot::pfzAffineInverse(M[hand], invHand)) return refuse(4);
    float palmLocal[16];
    nofoot::pfzMul(palmModel, invHand, palmLocal);
    std::memcpy(r.palm, palmLocal, sizeof(r.palm));
    // F75: keep the retargeting palm above intact; the support cup gets a distinct contact frame at K2.
    {
        double k2Points[4][3] = {};
        uint32_t k2At = 0;
        for (uint32_t b = 0; b < 5 && k2At < 4; ++b) if (b != thumb) {
            for (int c = 0; c < 3; ++c) k2Points[k2At][c] = M[branches[b][1]][12 + c];
            ++k2At;
        }
        // The four non-thumb branches are all four joints long by the topology check above.
        if (k2At == 4) {
            const double wrist[3] = {W[0], W[1], W[2]}, thumbRoot[3] = {T[0], T[1], T[2]};
            const nofoot::SupportContactGeometry contact = nofoot::supportContactFromK2(wrist, thumbRoot, k2Points);
            if (contact.ok) {
                float contactModel[16], contactLocal[16], inverseContact[16];
                nofoot::pfzIdentity(contactModel);
                for (int c = 0; c < 3; ++c) {
                    contactModel[c] = static_cast<float>(contact.distal[c]);
                    contactModel[4 + c] = static_cast<float>(contact.radial[c]);
                    contactModel[8 + c] = static_cast<float>(contact.normal[c]);
                    contactModel[12 + c] = static_cast<float>(contact.centre[c]);
                }
                nofoot::pfzMul(contactModel, invHand, contactLocal);
                bool finite = true;
                for (float v : contactLocal) finite = finite && std::isfinite(v);
                if (finite && nofoot::pfzAffineInverse(contactLocal, inverseContact)) {
                    std::memcpy(r.supportContactLocal, contactLocal, sizeof(r.supportContactLocal));
                    r.supportWidth = static_cast<float>(contact.width);
                    r.supportLength = static_cast<float>(contact.length);
                    r.supportRadius = static_cast<float>(contact.radius);
                    r.supportContactValid = true;
                }
            }
        }
    }
    if (!nofoot::pfzAffineInverse(r.palm, invPalmLocal)) return refuse(4);
    uint8_t slotOf[nofoot::kPfzMaxJoints]; std::memset(slotOf, 0xFF, sizeof(slotOf));
    nofoot::pfzIdentity(r.restLocal[0]);
    std::memcpy(r.boneToPalm[0], invPalmLocal, sizeof(invPalmLocal));
    r.boneToPalm[0][12] = r.boneToPalm[0][13] = r.boneToPalm[0][14] = 0.0f; r.boneToPalm[0][15] = 1.0f;
    uint32_t at = 1;
    for (uint32_t oi = 0; oi < 5; ++oi) { const uint32_t b = branchOrder[oi]; for (uint32_t d = 0; d < lengths[b]; ++d) {
        const uint32_t j = branches[b][d];
        if (!names[j]) return refuse(3);
        if (at >= 24) return refuse(2);
        r.joint[at] = static_cast<uint16_t>(j); r.name[at] = names[j];
        const uint16_t p = parents[j];
        if (p == hand) r.parentSlot[at] = 0;
        else {
            if (p >= joints || slotOf[p] == 0xFFu) return refuse(2);
            r.parentSlot[at] = slotOf[p];
        }
        slotOf[j] = static_cast<uint8_t>(at);
        nofoot::pfzLocalMatrix(locals + j * avh::kJointFloats, r.restLocal[at]);
        float localHand[16], boneToPalm[16];
        nofoot::pfzMul(M[j], invHand, localHand);
        nofoot::pfzMul(localHand, invPalmLocal, boneToPalm);
        boneToPalm[12] = boneToPalm[13] = boneToPalm[14] = 0.0f; boneToPalm[15] = 1.0f;
        std::memcpy(r.boneToPalm[at], boneToPalm, sizeof(boneToPalm));
        ++at;
        r.count = static_cast<uint8_t>(at);
    }}
    r.count = static_cast<uint8_t>(at);
    r.why = 0;
    *out = r;
    if (f74First(true)) {
        const auto wristLocal = [&](const double p[3], double q[3]) noexcept {
            for (int c = 0; c < 3; ++c) q[c] = invHand[12 + c] + invHand[c] * p[0] + invHand[4 + c] * p[1] + invHand[8 + c] * p[2];
        };
        double kLocal[3], k2Local[3]; wristLocal(K, kLocal); wristLocal(K2, k2Local);
        Log::get().note("native on foot hand landmarks (F74): rig hand %u/%u side %s; wrist %u/%u@0,0,0; wrist-local K1=%.1f,%.1f,%.1f K2=%.1f,%.1f,%.1f cm; candidates K1 %.1fx%.1f K2 %.1fx%.1f cm",
                        static_cast<unsigned>(hand), static_cast<unsigned>(names[hand]), side ? "R" : "L", static_cast<unsigned>(hand), static_cast<unsigned>(names[hand]),
                        100.0 * kLocal[0], 100.0 * kLocal[1], 100.0 * kLocal[2], 100.0 * k2Local[0], 100.0 * k2Local[1], 100.0 * k2Local[2],
                        100.0 * r.width, 100.0 * r.length, 100.0 * secondWidth, 100.0 * secondLength);
        for (uint32_t start = 0; start < 5; start += 3) {
            char line[320] = {};
            size_t at = 0;
            for (uint32_t b = start; b < std::min(start + 3, 5u); ++b) {
                double first[3], second[3];
                const double firstModel[3] = {M[branches[b][0]][12], M[branches[b][0]][13], M[branches[b][0]][14]};
                const double secondModel[3] = {M[branches[b][1]][12], M[branches[b][1]][13], M[branches[b][1]][14]};
                wristLocal(firstModel, first); wristLocal(secondModel, second);
                const int n = std::snprintf(line + at, sizeof(line) - at, "%sb%u%c %u/%u@%.1f,%.1f,%.1f>%u/%u@%.1f,%.1f,%.1f",
                                            at ? " | " : "", static_cast<unsigned>(b), b == thumb ? 'T' : '-', static_cast<unsigned>(branches[b][0]), static_cast<unsigned>(names[branches[b][0]]),
                                            100.0 * first[0], 100.0 * first[1], 100.0 * first[2], static_cast<unsigned>(branches[b][1]), static_cast<unsigned>(names[branches[b][1]]),
                                            100.0 * second[0], 100.0 * second[1], 100.0 * second[2]);
                if (n <= 0 || static_cast<size_t>(n) >= sizeof(line) - at) break;
                at += static_cast<size_t>(n);
            }
            Log::get().note("native on foot hand landmarks (F74): rig hand %u/%u side %s thumb b%u; %s",
                            static_cast<unsigned>(hand), static_cast<unsigned>(names[hand]), side ? "R" : "L", static_cast<unsigned>(thumb), line);
        }
    }
    return true;
}
bool armBuildPlan(uint64_t x) noexcept {
    pfz::g_armX.store(0, std::memory_order_release);
    uint64_t pose = 0;
    uint16_t joints = 0;
    if (!sehReadU64At(static_cast<uintptr_t>(x) + 8u, &pose) || !plausibleUserPointer(pose) || !sehReadU16At(static_cast<uintptr_t>(pose), &joints) ||
        joints == 0 || joints > nofoot::kPfzMaxJoints || !sehReadPoseArrays(static_cast<uintptr_t>(pose), joints, g_pfzLocals, g_pfzParents)) {
        Log::get().note("native on foot arm IK (F56): X=%016llX: P = [X+8] %016llX, %u joints (cap 0xC0) or the parents unreadable; no arm plan",
                        static_cast<unsigned long long>(x), static_cast<unsigned long long>(pose), static_cast<unsigned>(joints));
        return false;
    }
    pfz::ArmPlan plan;
    plan.x = x;
    plan.joints = joints;
    const uint32_t cs[2] = {g_shProbe3P.l, g_shProbe3P.r}, hs[2] = {g_shProbe3P.hl, g_shProbe3P.hr}, as[2] = {g_shProbe3P.al, g_shProbe3P.ar};
    uint64_t nameTable = 0;
    uint16_t names[nofoot::kPfzMaxJoints] = {};
    bool namesOk = sehReadU64At(static_cast<uintptr_t>(pose) + 0x18u, &nameTable) && plausibleUserPointer(nameTable);
    for (uint32_t j = 0; j < joints && namesOk; ++j) namesOk = sehReadU16At(static_cast<uintptr_t>(nameTable) + 2u * j, &names[j]);
    bool handL = false, handR = false;
    if (namesOk) {
        handL = deriveHandRig(joints, hs[0], as[0], 0, g_pfzLocals, g_pfzParents, names, &plan.hand[0]);
        handR = deriveHandRig(joints, hs[1], as[1], 1, g_pfzLocals, g_pfzParents, names, &plan.hand[1]);
    } else { plan.hand[0].why = plan.hand[1].why = 3; }
    plan.handCount = 2;  // optional per-side anatomy failures retain their reason and do not suppress the other side
    for (int side = 0; side < 2; ++side) {
        const char* name = side ? "R" : "L";
        const nofoot::IkArm arm = nofoot::ikArmChain(g_pfzParents, joints, cs[side], hs[side], as[side]);
        plan.side[side] = arm;
        plan.ok[side] = arm.why == 0;
        if (arm.why != 0) {
            Log::get().note("native on foot arm IK (F56): X=%016llX side %s: clavicle %u hand %u attach %u: REFUSED (%s); this arm is not solved",
                            static_cast<unsigned long long>(x), name, cs[side], hs[side], as[side], nofoot::ikArmWhyText(arm.why));
            continue;
        }
        char groups[1400] = {};
        size_t at = 0;
        const uint16_t* lists[3] = {arm.gU, arm.gF, arm.gH};
        const uint16_t counts[3] = {arm.nU, arm.nF, arm.nH};
        const char* gname[3] = {"gU", "gF", "gH"};
        for (int g = 0; g < 3; ++g) {
            armAppend(groups, sizeof(groups), &at, "%s%s(%u):", g ? "; " : "", gname[g], static_cast<unsigned>(counts[g]));
            for (uint32_t k = 0; k < counts[g]; ++k)
                armAppend(groups, sizeof(groups), &at, " %u%s", static_cast<unsigned>(lists[g][k]), g == 0 && arm.chainU[k] ? "*" : "");
        }
        // The children of U and F with their projected t at the rest pose (U->F for U's, F->H for F's).
        char kids[700] = {};
        size_t kat = 0;
        alignas(16) float mU[16] = {}, mF[16] = {}, mH[16] = {};
        const bool restOk = nofoot::pfzRestModel(g_pfzLocals, g_pfzParents, joints, arm.U, mU) &&
                            nofoot::pfzRestModel(g_pfzLocals, g_pfzParents, joints, arm.F, mF) &&
                            nofoot::pfzRestModel(g_pfzLocals, g_pfzParents, joints, arm.H, mH);
        const uint32_t heads[2] = {arm.U, arm.F};
        for (int q = 0; q < 2; ++q) {
            const float* a = q ? mF : mU;
            const float* b = q ? mH : mF;
            armAppend(kids, sizeof(kids), &kat, "%schildren of %s %u:", q ? "; " : "", q ? "F" : "U", heads[q]);
            for (uint32_t j = 0; j < joints; ++j) {
                if (g_pfzParents[j] != heads[q]) continue;
                alignas(16) float mj[16] = {};
                double t = -1.0;
                if (restOk && nofoot::pfzRestModel(g_pfzLocals, g_pfzParents, joints, j, mj)) {
                    const double ax[3] = {static_cast<double>(b[12]) - a[12], static_cast<double>(b[13]) - a[13], static_cast<double>(b[14]) - a[14]};
                    const double pj[3] = {static_cast<double>(mj[12]) - a[12], static_cast<double>(mj[13]) - a[13], static_cast<double>(mj[14]) - a[14]};
                    const double l2 = nofoot::ikDot(ax, ax);
                    if (l2 > 1e-12) t = nofoot::ikDot(pj, ax) / l2;
                }
                armAppend(kids, sizeof(kids), &kat, " %u(t %.2f)", j, t);
            }
        }
        double l1 = -1.0, l2 = -1.0;
        if (restOk) {
            const float u2f[3] = {mF[12] - mU[12], mF[13] - mU[13], mF[14] - mU[14]}, f2h[3] = {mH[12] - mF[12], mH[13] - mF[13], mH[14] - mF[14]};
            l1 = std::sqrt(static_cast<double>(u2f[0]) * u2f[0] + static_cast<double>(u2f[1]) * u2f[1] + static_cast<double>(u2f[2]) * u2f[2]);
            l2 = std::sqrt(static_cast<double>(f2h[0]) * f2h[0] + static_cast<double>(f2h[1]) * f2h[1] + static_cast<double>(f2h[2]) * f2h[2]);
        }
        Log::get().note("native on foot arm IK (F56): X=%016llX side %s: C %u U %u F %u H %u A %u (%s; the attach's parent is %s); depth C %u, H %u "
                        "(H - C = %u, FP has 3); rest l1 %.1f cm l2 %.1f cm (-100 not known); %s; %s",
                        static_cast<unsigned long long>(x), name, static_cast<unsigned>(arm.C), static_cast<unsigned>(arm.U),
                        static_cast<unsigned>(arm.F), static_cast<unsigned>(arm.H), static_cast<unsigned>(arm.A),
                        arm.direct ? "U = parents[F], parents[U] = C" : "U found by the upward search", arm.attachDirect ? "H" : "NOT H (under H)",
                        arm.depthC, arm.depthH, arm.depthH - arm.depthC, l1 * 100.0, l2 * 100.0, groups, kids);
    }
    if (!plan.ok[0] && !plan.ok[1]) return false;
    Log::get().note("native on foot hand anatomy (F70b): 3P X %016llX: name IDs %s; left %s (%s, %u joints, %.1f x %.1f cm, radius %.1f mm), right %s (%s, %u joints, %.1f x %.1f cm, radius %.1f mm); optional plan, legacy arm plan remains available",
                    static_cast<unsigned long long>(x), namesOk ? "read" : "unreadable", handL ? "ok" : "refused", handRigWhy(plan.hand[0].why), plan.hand[0].count,
                    100.0 * plan.hand[0].width, 100.0 * plan.hand[0].length, 1000.0 * plan.hand[0].skinRadius,
                    handR ? "ok" : "refused", handRigWhy(plan.hand[1].why), plan.hand[1].count,
                    100.0 * plan.hand[1].width, 100.0 * plan.hand[1].length, 1000.0 * plan.hand[1].skinRadius);
    // F57: the torso square-up's plan: the chest root (the clavicles' parent) and the spine 16 subtree by level (16, 21, 24 exclusive).
    plan.sq = nofoot::ikSquarePlan(g_pfzParents, joints);
    plan.sqOk = plan.sq.why == 0;
    {
        uint32_t byLevel[3] = {};
        for (uint32_t k = 0; k < plan.sq.n; ++k) ++byLevel[plan.sq.level[k] < 3 ? plan.sq.level[k] : 2];
        Log::get().note("native on foot arm IK (F57): X=%016llX square-up plan: %s; chest root %u (parent of 35 %u, of 36 %u); spine 16/21/24 "
                        "exclusive subtrees %u/%u/%u joints (%u in all), weights 1/3, 2/3, 1; pivot joint 34",
                        static_cast<unsigned long long>(x), plan.sqOk ? "ok" : nofoot::ikSquareWhyText(plan.sq.why),
                        static_cast<unsigned>(plan.sq.chestRoot), joints > nofoot::kIkClavL ? static_cast<unsigned>(g_pfzParents[nofoot::kIkClavL]) : 0xFFFFu,
                        joints > nofoot::kIkClavR ? static_cast<unsigned>(g_pfzParents[nofoot::kIkClavR]) : 0xFFFFu, byLevel[0], byLevel[1], byLevel[2],
                        static_cast<unsigned>(plan.sq.n));
    }
    // F166: the upper body's held joints (the spine, both clavicles and upper arms) and the joints that come with them. A refusal leaves the hold off; nothing else depends on it.
    {
        const uint32_t clav[2] = {plan.ok[0] ? plan.side[0].C : 0xFFFFu, plan.ok[1] ? plan.side[1].C : 0xFFFFu};
        const uint32_t upper[2] = {plan.ok[0] ? plan.side[0].U : 0xFFFFu, plan.ok[1] ? plan.side[1].U : 0xFFFFu};
        plan.f166 = nofoot::f166::buildPlan(g_pfzParents, joints, clav, upper);
        char held[96] = {};
        size_t at = 0;
        for (uint32_t k = 0; k < plan.f166.n; ++k)
            armAppend(held, sizeof(held), &at, "%s%u", k ? " " : "", static_cast<unsigned>(plan.f166.joint[k]));
        Log::get().note("native on foot upper body hold plan (F166): X=%016llX %s (%s); held joints %s (arms %u); %u joints come with them",
                        static_cast<unsigned long long>(x), plan.f166.ok() ? "ok" : "REFUSED", nofoot::f166::whyText(plan.f166.why), held[0] ? held : "none",
                        static_cast<unsigned>(plan.f166.arms), static_cast<unsigned>(plan.f166.carriedN));
    }
    // F180b: the spine 16, 21, 24 and the neck 34 (a child of the chest) and the joints that come with them, for the standing ADS spine straighten dial. A refusal leaves the dial doing nothing.
    {
        plan.f180 = nofoot::f180::buildPlan(g_pfzParents, joints);
        char held[96] = {};
        size_t at = 0;
        for (uint32_t k = 0; k < plan.f180.n; ++k)
            armAppend(held, sizeof(held), &at, "%s%u", k ? " " : "", static_cast<unsigned>(plan.f180.joint[k]));
        Log::get().note("native on foot spine straighten plan (F180b plan): X=%016llX %s (%s); held joints %s (the neck %s); %u joints come with them",
                        static_cast<unsigned long long>(x), plan.f180.ok() ? "ok" : "REFUSED", nofoot::f166::whyText(plan.f180.why), held[0] ? held : "none",
                        nofoot::f180::planHasNeck(plan.f180) ? "held" : "not held: it is not a direct child of the chest 24", static_cast<unsigned>(plan.f180.carriedN));
    }
    // F169: the left arm's chain (clavicle, upper arm, forearm, hand) and the joints that come with it, for the pistol sprint's delayed swing. A refusal leaves the swing dial doing nothing.
    {
        if (plan.ok[0]) plan.f169 = nofoot::f169::buildPlan(g_pfzParents, joints, plan.side[0].C, plan.side[0].U, plan.side[0].F, plan.side[0].H);
        Log::get().note("native on foot pistol sprint swing plan (F169): X=%016llX %s (%s); left arm chain %u %u %u %u; %u joints come with it",
                        static_cast<unsigned long long>(x), plan.f169.ok() ? "ok" : "REFUSED", plan.ok[0] ? nofoot::f169::whyText(plan.f169.why) : "the left arm's plan was refused",
                        plan.ok[0] ? static_cast<unsigned>(plan.side[0].C) : 0xFFFFu, plan.ok[0] ? static_cast<unsigned>(plan.side[0].U) : 0xFFFFu,
                        plan.ok[0] ? static_cast<unsigned>(plan.side[0].F) : 0xFFFFu, plan.ok[0] ? static_cast<unsigned>(plan.side[0].H) : 0xFFFFu,
                        static_cast<unsigned>(plan.f169.carriedN));
    }
    // F65b: the legs for stand armed: each chain from the probe's foot (nofoot::ikLegChain), the anchor (the freeze's: the lowest common
    // ancestor of the neck 34 and the left foot), the pair check against the RE's indices (the R foot confirmed by FindJoint), and the upper
    // body UB. A refusal leaves the legs and the raise off; the arms are unaffected.
    {
        const uint32_t probeFoot[2] = {g_shProbe3P.fl, g_shProbe3P.fr};
        for (int k = 0; k < 2; ++k) plan.leg[k] = nofoot::ikLegChain(g_pfzParents, joints, probeFoot[k]);
        const nofoot::PfzFrozenSet fs = nofoot::pfzFrozenSet(g_pfzParents, joints, kPfzNeck, probeFoot[0]);
        plan.legAnchor = fs.why == 0 ? fs.anchor : 0xFFFFu;
        plan.legWhy = fs.why != 0 ? 7u : nofoot::ikLegPairWhy(g_pfzParents, joints, plan.leg[0], plan.leg[1], plan.legAnchor, probeFoot);
        if (plan.legWhy == 0) {
            plan.ubN = static_cast<uint16_t>(nofoot::ikUpperBody(g_pfzParents, joints, plan.legAnchor, plan.leg[0].U, plan.leg[1].U, plan.ub,
                                                                 nofoot::kPfzMaxJoints));
            if (plan.ubN == 0) plan.legWhy = 1;
        }
        plan.legOk = plan.legWhy == 0;
        char legs[2][200] = {};
        for (int k = 0; k < 2; ++k) {
            const nofoot::IkArm& g = plan.leg[k];
            std::snprintf(legs[k], sizeof(legs[k]), "%s foot (FindJoint %s) %u%s, calf %u, thigh %u, pelvis %u; gU %u gF %u gH %u", k ? "R" : "L",
                          k ? "def_r_foot_joint" : "def_l_foot_joint", probeFoot[k],
                          probeFoot[k] == nofoot::kLegFoot[k] ? (k ? " = the inferred 100: CONFIRMED" : " = 103") : " (NOT the RE's)",
                          static_cast<unsigned>(g.F), static_cast<unsigned>(g.U), static_cast<unsigned>(g.C), static_cast<unsigned>(g.nU),
                          static_cast<unsigned>(g.nF), static_cast<unsigned>(g.nH));
        }
        Log::get().note("native on foot stand armed (F65b): X=%016llX legs %s (%s); %s; %s; anchor %u (pelvis parent %u); upper body %u joints",
                        static_cast<unsigned long long>(x), plan.legOk ? "ok" : "REFUSED", nofoot::ikLegWhyText(plan.legWhy), legs[0], legs[1],
                        plan.legAnchor, plan.leg[0].C < joints ? static_cast<unsigned>(g_pfzParents[plan.leg[0].C]) : 0xFFFFu,
                        static_cast<unsigned>(plan.ubN));
    }
    pfz::g_armSeq.fetch_add(1, std::memory_order_acq_rel);   // odd: the plan is being written
    plan.generation = pfz::g_armSeq.load(std::memory_order_relaxed) + 1u;
    pfz::g_armPlan = plan;
    pfz::g_armSeq.fetch_add(1, std::memory_order_release);   // even again
    return true;
}

void pfzCensusStep(uint64_t iface, const SkSlots& sl) noexcept {
    if (pfz::g_mode.load(std::memory_order_relaxed) == 0 && !pfz::g_ikWanted.load(std::memory_order_relaxed)) {
        pfz::g_localX.store(0, std::memory_order_relaxed);
        pfz::g_armX.store(0, std::memory_order_relaxed);
        return;
    }
    const uint32_t off = sl.kind == avh::HeadKind::Runtime ? 0x2B8u : 0x120u;
    uint64_t x = pfzCandidates(0, iface, sl);   // F51: the kind's X when Animate sees it, else the unique P match, else the kind's X
    if (!plausibleUserPointer(x)) {
        if (!g_pfzCensus.xNoted) {
            g_pfzCensus.xNoted = true;
            Log::get().note("native on foot pose freeze: the evaluator X = *(iface+0x%X) on the local 3P skeleton %016llX is not readable (%016llX); waiting",
                            off, static_cast<unsigned long long>(iface), static_cast<unsigned long long>(x));
        }
        pfz::g_localX.store(0, std::memory_order_relaxed);
        pfz::g_armX.store(0, std::memory_order_relaxed);
        return;
    }
    if (iface != g_pfzCensus.iface || x != g_pfzCensus.x) {
        g_pfzCensus = PfzCensus{};
        g_pfzCensus.iface = iface;
        g_pfzCensus.x = x;
        pfzBuildPlan(iface, x, sl);
        g_pfzCensus.armOk = armBuildPlan(x);   // F56: the arm IK's chains and groups, once per X
    }
    pfz::g_localX.store(g_pfzCensus.planOk ? x : 0, std::memory_order_release);   // again after a key change from off
    pfz::g_armX.store(g_pfzCensus.armOk ? x : 0, std::memory_order_release);     // F56: the same, for the arm IK's plan

    // Every 5 s: the iface's GetModelMatrix(34) beside the hook's newest span matrix of joint 34 (read back to back on this thread).
    const uint64_t now = GetTickCount64();
    if (now < g_pfzCensus.modelNextMs) return;
    g_pfzCensus.modelNextMs = now + 5000;
    alignas(16) float m[16] = {};
    if (!sehCallModel(sl.fn[3], static_cast<uintptr_t>(iface), kPfzNeck, m)) {
        if (!g_pfzCensus.modelNoted) {
            g_pfzCensus.modelNoted = true;
            Log::get().note("native on foot pose freeze: GetModelMatrix(34) on the local 3P skeleton faulted; the span cross-check has no iface side");
        }
        return;
    }
    if (!nofoot::logwin::detail()) return;   // F125: the cross-check's lines are the detail log's (GetModelMatrix and its fault note above run as before)
    float span[16] = {}, written[16] = {};
    bool wasWritten = false, got = false;
    uint64_t spanMs = 0;
    uint32_t buffer = 0;
    for (int tries = 0; tries < 4 && !got; ++tries) {
        const uint32_t s1 = pfz::g_spanSeq.load(std::memory_order_acquire);
        if (s1 & 1u) continue;
        std::memcpy(span, pfz::g_span34, sizeof(span));
        std::memcpy(written, pfz::g_span34Written, sizeof(written));
        wasWritten = pfz::g_span34WasWritten;
        spanMs = pfz::g_span34Ms;
        buffer = pfz::g_span34Buffer;
        std::atomic_thread_fence(std::memory_order_acquire);
        got = pfz::g_spanSeq.load(std::memory_order_relaxed) == s1 && spanMs != 0;
    }
    if (!got) {
        Log::get().note("native on foot pose freeze: span cross-check joint 34: GetModelMatrix pos (%.4f,%.4f,%.4f); the hook has no span sample for "
                        "the local X yet", static_cast<double>(m[12]), static_cast<double>(m[13]), static_cast<double>(m[14]));
        return;
    }
    double maxElem = 0.0, posMm = 0.0, maxW = 0.0, posW = 0.0;
    pfzMatrixDiff(m, span, &maxElem, &posMm);
    if (wasWritten) pfzMatrixDiff(m, written, &maxW, &posW);
    char wtext[160] = "the freeze did not write it";
    if (wasWritten)
        std::snprintf(wtext, sizeof(wtext), "against the frozen (written) matrix: max element %.5f, position %.2f mm", maxW, posW);
    Log::get().note("native on foot pose freeze: span cross-check joint 34: GetModelMatrix pos (%.4f,%.4f,%.4f), the span (%s) as animated pos "
                    "(%.4f,%.4f,%.4f), %llu ms older; max element difference %.5f, position %.2f mm; %s",
                    static_cast<double>(m[12]), static_cast<double>(m[13]), static_cast<double>(m[14]), buffer ? "[[X+0x1F0]+0xA0]" : "[job+0x28]",
                    static_cast<double>(span[12]), static_cast<double>(span[13]), static_cast<double>(span[14]),
                    static_cast<unsigned long long>(now >= spanMs ? now - spanMs : 0), maxElem, posMm, wtext);
}

void neckCensusStep() noexcept {
    if (!nativeOnFootActive()) return;
    const uint64_t now = GetTickCount64();
    if (now < g_neckNextMs.load(std::memory_order_relaxed)) return;
    if (g_busyNeck.exchange(true, std::memory_order_acquire)) return;
    g_neckNextMs.store(now + 100, std::memory_order_relaxed);
    NeckSnapshot s;
    s.stamp = now;
    const uint64_t iface = g_skelIface[0].load(std::memory_order_acquire);
    if (!iface) {
        s.state = 1;
    } else {
        SkSlots sl;
        if (skVerify(static_cast<uintptr_t>(iface), avh::HeadKind::None, &sl) != SkVerify::Ok) {
            s.state = 2;
        } else {
            uint32_t headIdx = avh::kNoJoint;
            if (!sehCallFind(sl.fn[1], static_cast<uintptr_t>(iface), avh::kHeadName, &headIdx)) {
                s.state = 3;
            } else {
                headIdx &= 0xFFFFu;
                alignas(16) float m[16] = {};
                if (!sehCallModel(sl.fn[3], static_cast<uintptr_t>(iface), headIdx, m) || !avh::headMatrixPlausible(m)) {
                    s.state = 4;
                } else {
                    s.state = 5;
                    s.head[0] = m[12];
                    s.head[1] = m[13];
                    s.head[2] = m[14];
                    // F39: the neck joint, by name, on the same skeleton (cached per skeleton).
                    s.neckState = neckJointRead(static_cast<uintptr_t>(iface), sl, s.neck);
                    if (s.neckState == 1) s.neckName = g_neckJoint.name;
                    // F43: the 3P shoulders, by name, on this skeleton.
                    shoulderProbe(iface, sl, g_shProbe3P, "3P");
                    if (g_shProbe3P.l != 0xFFFFu && g_shProbe3P.r != 0xFFFFu && jointModelAt(iface, sl, g_shProbe3P.l, &s.sh3P[0]) &&
                        jointModelAt(iface, sl, g_shProbe3P.r, &s.sh3P[3]))
                        s.shState |= 1u;
                    if (g_shProbe3P.hl != 0xFFFFu && g_shProbe3P.hr != 0xFFFFu && g_shProbe3P.al != 0xFFFFu && g_shProbe3P.ar != 0xFFFFu) {
                        const uint32_t ids[4] = {g_shProbe3P.hl, g_shProbe3P.hr, g_shProbe3P.al, g_shProbe3P.ar};
                        bool allRead = true;
                        for (size_t k = 0; k < 4; ++k) allRead = jointModelAt(iface, sl, ids[k], &s.ext3P[3 * k]) && allRead;
                        if (allRead) s.shState |= 4u;
                    }
                    // F48: the 3P povCamera joint (the attach joint 0x19B3F40 sets), the body anchor's 3P side.
                    if (g_shProbe3P.pov != 0xFFFFu && jointModelAt(iface, sl, g_shProbe3P.pov, s.pov3P)) {
                        s.shState |= 0x10u;
                        s.povIndex = g_shProbe3P.pov;
                    }
                    pfzCensusStep(iface, sl);   // F49 (TEMPORARY): the pose freeze's evaluator, plan and iface cross-check
                }
            }
        }
    }
    // F43: the FP shoulders, by name, on the second latched skeleton (once per skeleton).
    const uint64_t fpIface = g_skelIface[1].load(std::memory_order_acquire);
    if (fpIface) {
        SkSlots slf;
        if (skVerify(static_cast<uintptr_t>(fpIface), avh::HeadKind::None, &slf) == SkVerify::Ok) {
            // F51: the FP skeleton's X, for the line; F55: and the shoulder drop's plan (the FP X, with the 3P one's fallbacks)
            // F56: and the arm IK's FP snapshot (joints 51, 52, 37, 44), so the IK key keeps the FP plan too
            if (pfz::g_mode.load(std::memory_order_relaxed) != 0 || pfz::g_ikWanted.load(std::memory_order_relaxed))
                fdCensusStep(fpIface, pfzCandidates(1, fpIface, slf));
            shoulderProbe(fpIface, slf, g_shProbeFP, "FP");
            if (g_shProbeFP.l != 0xFFFFu && g_shProbeFP.r != 0xFFFFu && jointModelAt(fpIface, slf, g_shProbeFP.l, &s.shFp[0]) &&
                jointModelAt(fpIface, slf, g_shProbeFP.r, &s.shFp[3]))
                s.shState |= 2u;
            if (g_shProbeFP.hl != 0xFFFFu && g_shProbeFP.hr != 0xFFFFu && g_shProbeFP.al != 0xFFFFu && g_shProbeFP.ar != 0xFFFFu) {
                const uint32_t ids[4] = {g_shProbeFP.hl, g_shProbeFP.hr, g_shProbeFP.al, g_shProbeFP.ar};
                bool allRead = true;
                for (size_t k = 0; k < 4; ++k) allRead = jointModelAt(fpIface, slf, ids[k], &s.extFp[3 * k]) && allRead;
                if (allRead) s.shState |= 8u;
            }
        }
    }
    const uint64_t amc = g_amcLocal.load(std::memory_order_acquire);
    if (amc) s.amcState = sehAmcOrigin(reinterpret_cast<const uint8_t*>(amc), s.amc);
    {
        std::lock_guard<std::mutex> lock(g_neckMutex);
        g_neck = s;
    }
    g_busyNeck.store(false, std::memory_order_release);
}

bool neckCensusCopy(NeckCensusReading* out) {
    if (!out) return false;
    std::lock_guard<std::mutex> lock(g_neckMutex);
    out->state = g_neck.state;
    out->stamp = g_neck.stamp;
    std::memcpy(out->head, g_neck.head, sizeof(out->head));
    std::memcpy(out->amc, g_neck.amc, sizeof(out->amc));
    out->amcState = g_neck.amcState;
    out->neckState = g_neck.neckState;
    std::memcpy(out->neck, g_neck.neck, sizeof(out->neck));
    out->neckName = g_neck.neckName;
    out->shState = g_neck.shState;
    std::memcpy(out->sh3P, g_neck.sh3P, sizeof(out->sh3P));
    std::memcpy(out->shFp, g_neck.shFp, sizeof(out->shFp));
    std::memcpy(out->ext3P, g_neck.ext3P, sizeof(out->ext3P));
    std::memcpy(out->extFp, g_neck.extFp, sizeof(out->extFp));
    std::memcpy(out->pov3P, g_neck.pov3P, sizeof(out->pov3P));
    out->povIndex = g_neck.povIndex;
    return g_neck.stamp != 0;
}

// ---- Native on-foot F3 census (log only): the AMCs the fade hook sees while native stereo is on foot, one window at a time ----------
// The hook (game's submit job) notes each distinct AMC once per window and reads its origin there, while the object is live. The frame
// thread takes the window (avatarCensusTake) and reads the parts from its own side under SEH; a freed AMC faults inside the guard.
constexpr size_t kFpSlots = 8;
struct FpSlot {
    uintptr_t amc = 0;               // zero: free
    float origin[3] = {};
    bool originOk = false;           // false while the origin is pending or was not readable
};
std::mutex g_fpMutex;                // guards the slots and the drop count; never held across a game call
FpSlot g_fpSlots[kFpSlots];
uint32_t g_fpDropped = 0;

__declspec(noinline) void avatarCensusNote(void* amcPtr) noexcept {
    const uintptr_t a = reinterpret_cast<uintptr_t>(amcPtr);
    {
        std::lock_guard<std::mutex> lock(g_fpMutex);
        size_t freeSlot = kFpSlots;
        for (size_t i = 0; i < kFpSlots; ++i) {
            if (g_fpSlots[i].amc == a) return;   // already in this window
            if (g_fpSlots[i].amc == 0 && freeSlot == kFpSlots) freeSlot = i;
        }
        if (freeSlot == kFpSlots) {
            ++g_fpDropped;
            return;
        }
        g_fpSlots[freeSlot].amc = a;             // reserved; the origin follows below
        g_fpSlots[freeSlot].originOk = false;
    }
    float origin[3] = {};
    const int got = sehAmcOrigin(static_cast<const uint8_t*>(amcPtr), origin);   // the game's transform call: the object is live here
    std::lock_guard<std::mutex> lock(g_fpMutex);
    for (size_t i = 0; i < kFpSlots; ++i) {
        if (g_fpSlots[i].amc != a) continue;
        g_fpSlots[i].originOk = got > 0;
        std::memcpy(g_fpSlots[i].origin, origin, sizeof(origin));
        break;
    }
}

__declspec(noinline) uint64_t __fastcall avatarFadeHooked(void* a, void* b, void* c, void* d) noexcept {
    const auto forward = reinterpret_cast<ForwardFn>(g_forward[kHkFade].load(std::memory_order_acquire));
    if (!forward) return 0;
    const uint64_t result = forward(a, b, c, d);   // the original FIRST: the hide follows what it left
    if (g_hideOn.load(std::memory_order_acquire)) {
        headHideStep(a);
        neckCensusStep();
        fpBodyStep(a);   // F15 (E1): the first-person body, after the head hiding; a no-op unless the frame thread wants it
        bodyClipStep(a); // F19 (TEMPORARY): the body's clip modifiers (unclip) and their 5 s probe; the local third-person AMC only
    }
    if (a && nativeOnFootActive()) avatarCensusNote(a);
    return result;
}

// ---- installing the hooks -----------------------------------------------------------------------------------------------------
// The relay must sit within two gigabytes of the target (this DLL loads further than that from the game).
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

enum HookState : int { kHookStateNotTried = 0, kHookStateArmed = 1, kHookStateStoodDown = 2 };
struct HookEntry {
    CodeHook hook;
    uint8_t* relay = nullptr;
    uintptr_t target = 0;
    int id = 0;
    int state = kHookStateNotTried;
    size_t stolen = 0;
    char why[400] = {};
};
HookEntry g_hooks[kHkCount];

// Seal a relay page and publish the trampoline into its literal slot before CodeHook makes the entry patch live.
bool prepareRelay(void* trampoline, void* context) noexcept {
    auto& entry = *static_cast<HookEntry*>(context);
    const uintptr_t address = reinterpret_cast<uintptr_t>(trampoline);
    std::memcpy(entry.relay + avh::kCallbackRelayTrampolineAt, &address, 8);
    DWORD oldProtect = 0;
    if (!VirtualProtect(entry.relay, 4096, PAGE_EXECUTE_READ, &oldProtect) ||
        !FlushInstructionCache(GetCurrentProcess(), entry.relay, avh::kCallbackRelayBytes))
        return false;
    g_forward[entry.id].store(address, std::memory_order_release);
    return true;
}

// The PE identity is checked once per session; every hook needs it.
int g_identity = 0;   // 0 not checked, 1 ok, 2 differs
char g_identityWhy[200] = {};

// Resolve the target, verify the build and the prologue. On failure `why` says why in one sentence and nothing was patched.
bool resolveAndVerify(int id, uintptr_t* target, char* why, size_t whyCap) {
    const HookSpec& spec = kSpec[id];
    uintptr_t t = 0;
#ifdef EDVR_NATIVE_ON_FOOT_HOOKS_TEST
    t = g_testTargets[id];
#endif
    if (!t) {
        const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        if (!base) {
            std::snprintf(why, whyCap, "the game module could not be resolved");
            return false;
        }
        if (g_identity == 0) {
            const char* peWhy = nullptr;
            g_identity = checkIdentity(base, &peWhy) ? 1 : 2;
            if (g_identity == 2) std::snprintf(g_identityWhy, sizeof(g_identityWhy), "%s", peWhy ? peWhy : "the build differs");
        }
        if (g_identity == 2) {
            std::snprintf(why, whyCap, "the game build differs (%s); EliteDangerous64.exe+0x%llX was not touched", g_identityWhy,
                          static_cast<unsigned long long>(spec.rva));
            return false;
        }
        t = base + spec.rva;
    }
    if (!sehCheckBytes(t, spec.prologue, spec.prologueBytes)) {
        std::snprintf(why, whyCap,
                      "the game build differs (the %zu bytes at EliteDangerous64.exe+0x%llX are not build 334925's %s prologue); nothing was patched",
                      spec.prologueBytes, static_cast<unsigned long long>(spec.rva), spec.what);
        return false;
    }
    *target = t;
    return true;
}

const void* callbackFor(int id) {
    switch (id) {
        case kHkCtl: return reinterpret_cast<const void*>(&controllerHooked);
        case kHkFade: return reinterpret_cast<const void*>(&avatarFadeHooked);
        case kHkFind: return reinterpret_cast<const void*>(&findJointHooked);
        default: return nullptr;
    }
}

bool installHook(int id, char* why, size_t whyCap) {
    const HookSpec& spec = kSpec[id];
    HookEntry& entry = g_hooks[id];
    uintptr_t target = 0;
    if (!resolveAndVerify(id, &target, why, whyCap)) return false;
    entry.id = id;
    entry.target = target;
    entry.relay = allocateRelay(target);
    if (!entry.relay) {
        std::snprintf(why, whyCap, "no executable memory could be placed within two gigabytes of the target; nothing was patched");
        return false;
    }
    avh::buildCallbackRelay(entry.relay, &g_gate[id], callbackFor(id));
    if (!entry.hook.install(reinterpret_cast<void*>(target), entry.relay, nullptr, spec.tag, &prepareRelay, &entry)) {
        VirtualFree(entry.relay, 0, MEM_RELEASE);
        entry.relay = nullptr;
        std::snprintf(why, whyCap, "CodeHook refused it (its own line above, tagged %s, names why); nothing was patched", spec.tag);
        return false;
    }
    entry.stolen = entry.hook.stolenBytes();
    return true;
    // Process-lifetime storage: the relay and trampoline are never freed, so a call already inside them can finish.
}

void say(const avh::Sink& sink, const char* fmt, ...) {
    char line[avh::kLineBytes];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    sink(line);
}
void logSink(void*, const char* line) { Log::get().note("%s", line); }

const char* armedRole(int id) {
    switch (id) {
        case kHkFade:
            return "HEAD HIDING: the original runs first, then, while native stereo's body is on foot in the game's first-person camera, the view masks of "
                   "the local third-person avatar's head parts are zeroed; the neck census and the avatar census read here too";
        case kHkFind:
            return "CAPTURE only: the original runs first and its result is returned unchanged; a call from one of the two avatar-attach sites with the "
                   "povCamera literal stores the skeleton interface (rcx) and the index, for head hiding and the census";
        default:
            return "the original runs untouched and the mode byte is read after it, for native on foot's camera signal; nothing is written";
    }
}
const char* downConsequence(int id) {
    switch (id) {
        case kHkFade: return "head hiding and the census do not run.";
        case kHkFind: return "head hiding and the census do not run (no local skeleton can be latched).";
        default: return "native on foot's body changes stay on in every camera (the game's third-person camera is not told apart).";
    }
}

void tryHook(int id, const avh::Sink& sink) {
    const HookSpec& spec = kSpec[id];
    HookEntry& entry = g_hooks[id];
    char why[400] = {};
    if (installHook(id, why, sizeof(why))) {
        entry.state = kHookStateArmed;
        if (id == kHkFind) {
            // The image base is the target minus its RVA (production: target = base + RVA; the rigs place their synthetic functions the same way).
            const uintptr_t base = entry.target - avh::kFindJointRva;
            g_findSite[0].store(base + avh::kFindSite1Rva, std::memory_order_relaxed);
            g_findSite[1].store(base + avh::kFindSite2Rva, std::memory_order_relaxed);
            g_findPov.store(base + avh::kPovNameRva, std::memory_order_release);
        }
        say(sink, "%s %s hook armed: EliteDangerous64.exe+0x%llX (%s, build 334925) at 0x%llX, stolen=%zu bytes, prologue %zu/%zu bytes verified, "
                  "relay at 0x%llX; %s",
            avh::prefix(), spec.label, static_cast<unsigned long long>(spec.rva), spec.what, static_cast<unsigned long long>(entry.target),
            entry.stolen, spec.prologueBytes, spec.prologueBytes, static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(entry.relay)),
            armedRole(id));
    } else {
        entry.state = kHookStateStoodDown;
        std::snprintf(entry.why, sizeof(entry.why), "%s", why);
        say(sink, "%s %s hook stood down: %s. %s", avh::prefix(), spec.label, why, downConsequence(id));
    }
}

bool armed(int id) { return g_hooks[id].state == kHookStateArmed; }

void updateGates() {
    // F93d: the controller's post-call reads the mode byte while native stereo is on foot (the camera signal).
    g_gate[kHkCtl].store(armed(kHkCtl) && nativeOnFootActive() ? 1u : 0u, std::memory_order_release);
    // The FindJoint capture and the fade hook's post-call are open from launch while the native key is on (the attach runs every frame, but the
    // capture must be in place), so the local skeleton pair and the local AMC are known before native stereo first reaches the commander's feet.
    // Head hiding also needs both hooks armed and the part names verified; headHideStep writes the view masks only while native stereo's body is
    // on in the game's first-person camera (checked there, not just here).
    const bool capture = g_nativeWant.load(std::memory_order_relaxed);
    const bool hideOn = capture && armed(kHkFade) && armed(kHkFind) && g_partNames.load(std::memory_order_relaxed) == 1 && !g_hideDown.load(std::memory_order_relaxed);
    g_hideOn.store(hideOn, std::memory_order_release);
    g_gate[kHkFade].store(armed(kHkFade) && hideOn ? 1u : 0u, std::memory_order_release);
    g_gate[kHkFind].store(armed(kHkFind) && capture ? 1u : 0u, std::memory_order_release);
}

// ---- the frame thread ----------------------------------------------------------------------------------------------------------
// The avatar fade global (frame thread).
bool fadeResolve() {
    if (g_fadeMode) return true;
#ifdef EDVR_NATIVE_ON_FOOT_HOOKS_TEST
    if (g_testFadeMode) {
        g_fadeMode = g_testFadeMode;
        g_fadeAmount = g_testFadeAmount;
        return true;
    }
#endif
    uintptr_t base = 0;
    char why[160] = {};
    if (!avatarHooksBuildKnown(&base, why, sizeof(why))) return false;   // an unknown build: the global is not touched
    g_fadeMode = reinterpret_cast<int32_t*>(base + avh::kFadeModeRva);
    g_fadeAmount = reinterpret_cast<float*>(base + avh::kFadeAmountRva);
    return true;
}
bool fadeStore(int32_t value) {
    if (!writableRange(g_fadeMode, 4)) return false;
    return sehWriteInt32(g_fadeMode, value);
}

// ---- the native body's fade claim (native on foot, F2; frame thread) ---------------------------------------------------------------
// While native stereo is on foot the guard (avatar_hooks_core.h) sees the claim on and wanted (active, placed, session = body) and the camera
// as closed (ctlMode 0), so the release waits for the body alone: it ends when the body does. A Foreign value is left alone.
void nativeFadeTick(bool body, const char* reason, const avh::Sink& sink) {
    if (!body && !g_nativeFade.ours()) return;     // nothing of ours to put back
    if (!fadeResolve()) return;
    avh::FadeIn in;
    in.active = body;
    in.placed = body;
    in.session = body;
    in.ctlMode = 0;
    if (g_nativeFade.ours() || body) in.readOk = sehReadInt32(g_fadeMode, &in.value);
    const avh::FadeStep st = g_nativeFade.step(in);
    if (st.act == avh::FadeStep::Act::Write) {
        if (fadeStore(0)) {
            say(sink, "native on foot body: on (fade mode -1 -> 0, head parts hidden)");
        } else {
            g_nativeFade.writeFailed();
            say(sink, "native on foot body: the avatar fade mode could not be written; the body is left as the game draws it");
        }
    } else if (st.act == avh::FadeStep::Act::Restore) {
        if (fadeStore(avh::kFadeAuto)) {
            say(sink, "native on foot body: off (fade restored -1: %s)", reason);
        } else {
            g_nativeFade.restoreFailed();   // stays ours: the next frame tries again
        }
    } else if (st.ev == avh::FadeEvent::Foreign) {
        say(sink, "native on foot body: the avatar fade mode reads %d, not -1 (auto), so it is someone else's and is left alone", st.seen);
    } else if (st.ev == avh::FadeEvent::ChangedUnderUs) {
        say(sink, "native on foot body: the avatar fade mode changed while EDVR held it (now %d); it is not put back", st.seen);
    } else if (st.ev == avh::FadeEvent::Unreadable) {
        say(sink, "native on foot body: the avatar fade mode could not be read; the body is left as the game draws it");
    }
}

// ---- head hiding, frame-thread half ------------------------------------------------------------------------------------------------
__declspec(noinline) bool sehNameIs(uintptr_t tableEntry, const char* expected) noexcept {
    __try {
        uint64_t p = 0;
        std::memcpy(&p, reinterpret_cast<const void*>(tableEntry), 8);
        if (p < 0x10000u || p >= 0x00007FFF00000000ull) return false;
        return std::strcmp(reinterpret_cast<const char*>(p), expected) == 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
// Once the fade hook is armed: does the exe's part-name table hold the 54 names the head set's indexes were read against? If not, nothing is hidden.
void checkPartNames(const avh::Sink& sink) {
    if (g_partNames.load(std::memory_order_relaxed) != 0 || !armed(kHkFade)) return;
    const uintptr_t base = g_hooks[kHkFade].target - avh::kAvatarFadeRva;
    uint32_t bad = 0, first = 0;
    for (uint32_t i = 0; i < avh::kPartCount; ++i) {
        if (sehNameIs(base + avh::kPartNameTableRva + 8u * i, avh::kPartNames[i])) continue;
        if (bad++ == 0) first = i;
    }
    if (bad == 0) {
        g_partNames.store(1, std::memory_order_release);
        std::string heads;
        for (uint32_t h : avh::kHeadParts) {
            if (!heads.empty()) heads += " ";
            heads += avh::kPartNames[h];
        }
        say(sink, "%s the part-name table at EliteDangerous64.exe+0x%llX holds the %u names the hide list was written against; while native stereo's body is on the view "
                  "masks of these %u head parts of the LOCAL third-person avatar are zeroed after the dither fade (nothing is restored: the game rewrites the masks "
                  "every frame): %s",
            avh::prefixHide(), static_cast<unsigned long long>(avh::kPartNameTableRva), avh::kPartCount, avh::kHeadPartCount, heads.c_str());
    } else {
        g_partNames.store(2, std::memory_order_release);
        say(sink, "%s the part-name table at EliteDangerous64.exe+0x%llX differs from the one the hide list was written against (%u of %u names; the first is index %u, "
                  "which is not \"%s\"): head hiding does not run and nothing is hidden",
            avh::prefixHide(), static_cast<unsigned long long>(avh::kPartNameTableRva), bad, avh::kPartCount, first, avh::kPartNames[first]);
    }
}
// The census (once per local AMC, from the hook thread), and the verdict on the -0x30 relation that picks the local AMC out.
void reportAmc(FrameState& fs, const avh::Sink& sink) {
    avh::AmcCensus census;
    while (g_censusRing.take(&census)) avh::formatAmcCensus(census, sink);
    // The latched local skeleton pair: one line whenever the third-person one changes (it should be stable for an on-foot session).
    const uint64_t third = g_skelIface[0].load(std::memory_order_acquire);
    if (third != fs.latchedThirdSeen) {
        say(sink, "%s the local avatar's skeleton pair %s: third-person 0x%llX (was 0x%llX), first-person 0x%llX; latched from a humanoid that attached BOTH avatars on one "
                  "thread (a site-1 attach then a site-2 attach at one stack location; NPCs have no first-person avatar); latches %u, raw attaches seen: site 1 %u, "
                  "site 2 %u (every humanoid's)",
            avh::prefixHide(), third ? "changed" : "was dropped", static_cast<unsigned long long>(third), static_cast<unsigned long long>(fs.latchedThirdSeen),
            static_cast<unsigned long long>(g_skelIface[1].load(std::memory_order_relaxed)), g_skelLatches.load(std::memory_order_relaxed),
            g_skelCaptures[0].load(std::memory_order_relaxed), g_skelCaptures[1].load(std::memory_order_relaxed));
        fs.latchedThirdSeen = third;
    }
    const uint64_t matches = g_amcLocalMatches.load(std::memory_order_relaxed);
    const uint64_t skeleton = g_skelIface[0].load(std::memory_order_relaxed);
    if (matches > 0 && !fs.amcMatchSaid) {
        fs.amcMatchSaid = true;
        say(sink, "%s the local third-person avatar is AMC 0x%llX: its AvatarPose handle is resolved (+0x260 reads 0xFFFFFFFF), +0x268 = 0x%llX minus 0x30 = 0x%llX is "
                  "the local avatar's third-person skeleton (latched from the attach that also attached a first-person avatar), and its creation mode is 3. The relation holds.",
            avh::prefixHide(), static_cast<unsigned long long>(g_amcLocal.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(g_amcLocalPose.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(g_amcLocalPose.load(std::memory_order_relaxed) - avh::kAvatarPoseToSkeleton));
    } else if (matches == 0 && !fs.amcNoMatchSaid && g_amcCalls.load(std::memory_order_relaxed) >= avh::kAmcNoMatchCalls) {
        fs.amcNoMatchSaid = true;
        if (skeleton == 0)
            say(sink, "%s no AMC can be tested yet: %llu avatar fade calls were seen but no local skeleton pair is latched (no humanoid attached BOTH a third-person and a "
                      "first-person avatar on one thread since launch), so the local skeleton is unknown; nothing is hidden. (fix.native_on_foot must be on at launch, and the "
                      "avatars attached after it.)",
                avh::prefixHide(), static_cast<unsigned long long>(g_amcCalls.load(std::memory_order_relaxed)));
        else
            say(sink, "%s NO AMC matched the local skeleton in %llu avatar fade calls (%llu with a resolved AvatarPose handle, %llu of those in mode 3; the last such "
                      "AMC 0x%llX has +0x268 = 0x%llX, minus 0x30 = 0x%llX; the latched local skeleton is 0x%llX): the relation +0x268 - 0x30 == skeleton does not hold "
                      "here, so nothing is hidden",
                avh::prefixHide(), static_cast<unsigned long long>(g_amcCalls.load(std::memory_order_relaxed)),
                static_cast<unsigned long long>(g_amcIdOk.load(std::memory_order_relaxed)), static_cast<unsigned long long>(g_amcMode3.load(std::memory_order_relaxed)),
                static_cast<unsigned long long>(g_amcCand.load(std::memory_order_relaxed)), static_cast<unsigned long long>(g_amcCandPose.load(std::memory_order_relaxed)),
                static_cast<unsigned long long>(g_amcCandPose.load(std::memory_order_relaxed) - avh::kAvatarPoseToSkeleton), static_cast<unsigned long long>(skeleton));
    }
    if (g_hideDown.load(std::memory_order_relaxed) && !fs.amcDownSaid) {
        fs.amcDownSaid = true;
        say(sink, "%s stood down for the session: %u guarded accesses of the avatar component faulted; the head parts are no longer touched", avh::prefixHide(),
            g_hideFaults.load(std::memory_order_relaxed));
    }
}

// ---- the skeleton reads' arming (frame thread) ------------------------------------------------------------------------------------------------
#ifdef EDVR_NATIVE_ON_FOOT_HOOKS_TEST
uintptr_t g_testHeadBase = 0;   // the rig's synthetic game image (0: the real module)
size_t g_testHeadSize = 0;
#endif
// Once per session, when the hooks are first wanted: establish the build's addresses of the skeleton interface (the census verifies every skeleton
// against them before it calls into one) and check the two joint-name literals. Done before the gates first open, so the hook threads never see
// the targets half written.
void skeletonArm(FrameState& fs, const avh::Sink& sink) {
    if (fs.armTried) return;
    fs.armTried = true;
    uintptr_t base = 0;
    size_t size = 0;
    char why[300] = {};
    bool known = false;
#ifdef EDVR_NATIVE_ON_FOOT_HOOKS_TEST
    if (g_testHeadBase) {
        base = g_testHeadBase;
        size = g_testHeadSize;
        known = true;
    }
#endif
    if (!known) {
        known = avatarHooksBuildKnown(&base, why, sizeof(why));
        size = known ? static_cast<size_t>(avh::kExpectedImageSize) : 0;   // the identity check just matched the image size
    }
    if (!known) {
        say(sink, "%s the skeleton reads are not armed: %s; the neck census and the pose census read nothing", avh::prefix(),
            why[0] ? why : "the game build is unknown");
        return;
    }
    const avh::HeadTargets t = avh::headTargetsFromBase(base, size);
    if (!sehCheckBytes(t.headName, reinterpret_cast<const uint8_t*>(avh::kHeadName), sizeof(avh::kHeadName)) ||
        !sehCheckBytes(t.povName, reinterpret_cast<const uint8_t*>(avh::kPovName), sizeof(avh::kPovName))) {
        say(sink, "%s the skeleton reads are not armed: the joint-name literals at EliteDangerous64.exe+0x%llX / +0x%llX are not \"%s\" / \"%s\" (a different "
                  "build); the neck census and the pose census read nothing",
            avh::prefix(), static_cast<unsigned long long>(avh::kHeadNameRva), static_cast<unsigned long long>(avh::kPovNameRva), avh::kHeadName, avh::kPovName);
        return;
    }
    g_skelTargets = t;
    g_skelArmed.store(true, std::memory_order_release);
    say(sink, "%s skeleton reads armed (build 334925): the census reads the LOCAL avatar's skeletons (the FindJoint capture's latched pair) through the skeleton "
              "interface's slots, after checking the vtable (RR +0x%llX or AO +0x%llX) and the slots +0x18 +0x30 +0x48 +0x58 before every call; nothing is "
              "written to a skeleton",
        avh::prefix(), static_cast<unsigned long long>(avh::kRrVtableRva), static_cast<unsigned long long>(avh::kAoVtableRva));
}

// One frame boundary. In order: the hooks, the first time they are wanted (while the native key is on, from launch, so the FindJoint capture is in
// place before the first on-foot avatar attaches; each is optional, and a stand-down costs what its line says); the skeleton reads' arming; the
// census and the latch lines; the avatar fade global for native stereo's body; the gates. nativeOnFootHooksBody publishes the want and the body
// after this runs (native_on_foot.cpp's own tick follows this one), so a change acts at the next boundary.
void boundaryAt(const avh::Sink& sink) {
    FrameState& fs = g_frame;
    if (g_nativeWant.load(std::memory_order_relaxed)) {
        if (g_hooks[kHkFind].state == kHookStateNotTried) tryHook(kHkFind, sink);
        if (g_hooks[kHkFade].state == kHookStateNotTried) tryHook(kHkFade, sink);
        // F93d: the camera controller's update, for the game's camera (the suite's mode byte): native on foot's body changes are first-person only.
        // Optional: a stand-down leaves the body changes on in every camera (native_on_foot.cpp says so once).
        if (g_hooks[kHkCtl].state == kHookStateNotTried) tryHook(kHkCtl, sink);
        checkPartNames(sink);
        skeletonArm(fs, sink);
    }
    reportAmc(fs, sink);
    {   // F93d: the fade claim is first-person behaviour: in the game's third-person camera the global is the game's (auto)
        const bool nativeBody = g_nativeBody.load(std::memory_order_acquire), external = nativeBody && nativeOnFootCameraExternal();
        nativeFadeTick(nativeBody && !external, external ? "the game's third-person camera (F93d)" : g_nativeReason, sink);
    }
    updateGates();
}
avh::Sink g_frameSink{&logSink, nullptr};   // the production wrapper's lines (the rig points it at a capture)
}  // namespace

void nativeOnFootHooksFrameBoundary() { boundaryAt(g_frameSink); }

void nativeOnFootHooksShutdown() {
    // An unload (FreeLibrary): the dither-fade global goes back to -1 if EDVR still holds it at 0. A process exit does not come here, and
    // needs nothing: the global dies with the process.
    nativeFadeTick(false, "the game is shutting down", avh::Sink{&logSink, nullptr});
}

bool avatarHooksBuildKnown(uintptr_t* baseOut, char* why, size_t whyCap) {
    const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    if (!base) {
        std::snprintf(why, whyCap, "the game module could not be resolved");
        return false;
    }
    if (g_identity == 0) {
        const char* peWhy = nullptr;
        g_identity = checkIdentity(base, &peWhy) ? 1 : 2;
        if (g_identity == 2) std::snprintf(g_identityWhy, sizeof(g_identityWhy), "%s", peWhy ? peWhy : "the build differs");
    }
    if (g_identity == 2) {
        std::snprintf(why, whyCap, "the game build differs (%s)", g_identityWhy);
        return false;
    }
    *baseOut = base;
    return true;
}

AvatarSkeleton avatarSkeleton(int site) {
    AvatarSkeleton s;
    if (site < 0 || site > 1) return s;
    s.iface = g_skelIface[site].load(std::memory_order_acquire);
    s.index = g_skelIndex[site].load(std::memory_order_relaxed);
    s.captures = g_skelCaptures[site].load(std::memory_order_relaxed);
    s.latches = g_skelLatches.load(std::memory_order_relaxed);
    return s;
}
uint64_t avatarFindJointSeen() { return g_findSeen.load(std::memory_order_relaxed); }

// Native on-foot F1 census (log only, read-only): the latched local third-person avatar's parts and their view masks, read under SEH.
// Writes nothing. Production code, not the rig's seam.
__declspec(noinline) bool sehReadPartMasks(const uint8_t* amc, uint32_t idx, uint64_t* masks) noexcept {
    __try {
        for (uint32_t k = 0; k < avh::kPartVariants; ++k) std::memcpy(&masks[k], amc + avh::kOffAmcMasks + idx * avh::kPartStride + 8 * k, 8);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
__declspec(noinline) bool sehReadFadeMode(uintptr_t address, int32_t* mode) noexcept {
    __try {
        std::memcpy(mode, reinterpret_cast<const void*>(address), sizeof(*mode));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
uint32_t localAvatarParts(LocalAvatarPart* out, uint32_t cap, uint64_t* amcOut) {
    const uint64_t amc = g_amcLocal.load(std::memory_order_acquire);
    if (amcOut) *amcOut = amc;
    if (!amc || !out) return 0;
    uint32_t n = 0;
    for (uint32_t i = 0; i < avh::kPartCount && n < cap; ++i, ++n) {
        LocalAvatarPart& p = out[n];
        p.name = avh::kPartNames[i];
        p.head = avh::isHeadPart(i);
        p.ok = sehReadPartMasks(reinterpret_cast<const uint8_t*>(amc), i, p.masks);
        if (!p.ok) std::memset(p.masks, 0, sizeof(p.masks));
    }
    return n;
}
bool avatarFadeModeRead(int32_t* mode) {
    const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    if (!base || !mode) return false;
    return sehReadFadeMode(base + avh::kFadeModeRva, mode);
}
// The F3 census row, read under SEH: the mode, the instanced parts with their four view masks, and whether an EVASuit arms, hands or legs
// part is instanced (parts 28, 29 and 30 of kPartNames). Read-only.
__declspec(noinline) bool sehReadAvatarRow(const uint8_t* amc, AvatarCensusRow* row) noexcept {
    __try {
        uint64_t params = 0;
        std::memcpy(&params, amc + avh::kOffAmcParams, 8);
        row->mode = -1;
        if (plausiblePointer(params)) std::memcpy(&row->mode, reinterpret_cast<const uint8_t*>(params) + avh::kOffParamsMode, 4);
        uint32_t n = 0;
        bool eva = false;
        for (uint32_t idx = 0; idx < avh::kPartCount; ++idx) {
            const uint32_t at = idx * avh::kPartStride;
            uint64_t inst[avh::kPartVariants] = {}, mask[avh::kPartVariants] = {};
            uint32_t bits = 0;
            for (uint32_t k = 0; k < avh::kPartVariants; ++k) {
                std::memcpy(&inst[k], amc + avh::kOffAmcInstances + at + 8 * k, 8);
                std::memcpy(&mask[k], amc + avh::kOffAmcMasks + at + 8 * k, 8);
                if (inst[k] != 0) bits |= 1u << k;
            }
            if (bits == 0) continue;
            AvatarCensusPart& p = row->part[n++];
            p.name = avh::kPartNames[idx];
            p.idx = idx;
            p.variants = bits;
            std::memcpy(p.masks, mask, sizeof(p.masks));
            if (idx == 28 || idx == 29 || idx == 30) eva = true;
        }
        row->parts = n;
        row->evaSet = eva;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

uint32_t avatarCensusTake(AvatarCensusRow* out, uint32_t cap, uint32_t* seen, uint32_t* dropped) {
    FpSlot snap[kFpSlots];
    uint32_t drop = 0;
    {
        std::lock_guard<std::mutex> lock(g_fpMutex);
        std::memcpy(snap, g_fpSlots, sizeof(snap));
        for (FpSlot& s : g_fpSlots) s = FpSlot{};
        drop = g_fpDropped;
        g_fpDropped = 0;
    }
    uint32_t distinct = 0;
    for (const FpSlot& s : snap)
        if (s.amc) ++distinct;
    if (seen) *seen = distinct;
    if (dropped) *dropped = drop;
    if (!out || cap == 0) return 0;
    // The local avatar's origin, when the latched third-person AMC was in this window and its origin was read.
    const uint64_t local = g_amcLocal.load(std::memory_order_acquire);
    bool haveLocal = false;
    float lo[3] = {};
    for (const FpSlot& s : snap) {
        if (s.amc && s.amc == local && s.originOk) {
            haveLocal = true;
            std::memcpy(lo, s.origin, sizeof(lo));
        }
    }
    AvatarCensusRow cand[kFpSlots];
    uint32_t n = 0;
    for (const FpSlot& s : snap) {
        if (!s.amc) continue;
        AvatarCensusRow& r = cand[n];
        r = AvatarCensusRow{};
        r.amc = s.amc;
        if (haveLocal && s.originOk) {
            const float dx = s.origin[0] - lo[0], dy = s.origin[1] - lo[1], dz = s.origin[2] - lo[2];
            r.dist = std::sqrt(dx * dx + dy * dy + dz * dz);
            r.distOk = true;
            if (r.dist > 8.0f) continue;   // an NPC far away
        }
        if (!sehReadAvatarRow(reinterpret_cast<const uint8_t*>(s.amc), &r)) r.parts = 0;   // a faulting read keeps its row, with no parts
        ++n;
    }
    // Insertion sort, at most eight rows: the first-person AMC (mode 2, F15: the body hide's target) first, then the EVASuit arms/hands/legs, then the
    // nearest to the local avatar, then the rest.
    for (uint32_t i = 1; i < n; ++i) {
        for (uint32_t j = i; j > 0; --j) {
            const AvatarCensusRow& a = cand[j - 1];
            const AvatarCensusRow& b = cand[j];
            bool bFirst = false;
            if ((a.mode == 2) != (b.mode == 2)) bFirst = b.mode == 2;
            else if (a.evaSet != b.evaSet) bFirst = b.evaSet;
            else if (a.distOk != b.distOk) bFirst = b.distOk;
            else if (a.distOk && a.dist != b.dist) bFirst = b.dist < a.dist;
            else bFirst = b.amc < a.amc;
            if (!bFirst) break;
            const AvatarCensusRow tmp = cand[j - 1];
            cand[j - 1] = cand[j];
            cand[j] = tmp;
        }
    }
    const uint32_t m = n < cap ? n : cap;
    for (uint32_t i = 0; i < m; ++i) out[i] = cand[i];
    return m;
}

void nativeOnFootHooksBody(bool wanted, bool body, const char* reason) {
    g_nativeWant.store(wanted, std::memory_order_release);
    g_nativeBody.store(body, std::memory_order_release);
    g_nativeReason = reason ? reason : "";
}

// F15 (E1, TEMPORARY): the first-person body hide, frame thread. Logs the transitions and resolves the names once the exe's part-name table
// is verified (g_partNames == 1, checkPartNames). The hook (fpBodyStep) does the zeroing.
namespace {
bool g_fpWantLast = false;           // the body was wanted on the last frame
uint64_t g_fpWantSinceMs = 0;        // when it was last turned on
uint64_t g_fpMode2AtStart = 0;       // g_fpMode2Seen when it was last turned on
uint64_t g_fpLoggedAmc = 0;          // the first-person AMC last logged as hidden (0 = none)
bool g_fpResolved = false;           // the names were looked up
bool g_fpNoMode2Noted = false;       // the no-first-person-AMC line, once per turn-on
std::string g_fpNamesText;           // the names found, as the hidden line spells them
}  // namespace

void nativeOnFootFpBody(bool want, const char* reason) {
    const char* why = reason ? reason : "";
    if (!want) {
        g_fpHideWant.store(false, std::memory_order_release);
        if (g_fpWantLast) {
            {
                std::lock_guard<std::mutex> lock(g_fpBodyMutex);
                g_fpReadback.amc = 0;
            }
            if (g_fpLoggedAmc != 0) Log::get().note("native on foot body: first-person body shown again (%s)", why);
        }
        g_fpLoggedAmc = 0;
        g_fpWantLast = false;
        return;
    }
    const uint64_t now = GetTickCount64();
    if (!g_fpWantLast) {
        g_fpWantSinceMs = now;
        g_fpMode2AtStart = g_fpMode2Seen.load(std::memory_order_relaxed);
        g_fpNoMode2Noted = false;
    }
    if (!g_fpResolved && g_partNames.load(std::memory_order_relaxed) == 1) {
        int found[avh::kFpBodyPartCount];
        std::string names, missing;
        for (uint32_t i = 0; i < avh::kFpBodyPartCount; ++i) {
            found[i] = avh::partIndexByName(avh::kFpBodyPartNames[i]);
            std::string& list = found[i] >= 0 ? names : missing;
            if (!list.empty()) list += ", ";
            list += avh::kFpBodyPartNames[i];
        }
        {
            std::lock_guard<std::mutex> lock(g_fpBodyMutex);
            for (uint32_t i = 0; i < avh::kFpBodyPartCount; ++i) g_fpIdx[i] = found[i];
        }
        g_fpNamesText = names;
        g_fpResolved = true;
        if (!missing.empty()) Log::get().note("native on foot body: first-person part(s) not in the part-name table, left alone: %s", missing.c_str());
    }
    g_fpHideWant.store(true, std::memory_order_release);
    g_fpWantLast = true;
    uint64_t amc = 0;
    {
        std::lock_guard<std::mutex> lock(g_fpBodyMutex);
        amc = g_fpReadback.amc;
    }
    if (amc != 0 && amc != g_fpLoggedAmc) {
        g_fpLoggedAmc = amc;
        Log::get().note("native on foot body: first-person body hidden (%s on AMC 0x%llX)",
                        g_hybridWant.load(std::memory_order_acquire) ? "FirstPersonSkeleton only, the hybrid body" : g_fpNamesText.c_str(),
                        static_cast<unsigned long long>(amc));
    }
    if (!g_fpNoMode2Noted && now - g_fpWantSinceMs >= 5000 && g_fpMode2Seen.load(std::memory_order_relaxed) == g_fpMode2AtStart) {
        g_fpNoMode2Noted = true;
        Log::get().note("native on foot body: no first-person AMC (mode 2) reached the fade hook in the first 5 s with the key on; nothing is hidden");
    }
}

// F19 (TEMPORARY), frame thread, every frame: the unclip's on/off (its transitions are logged), the probe's and the unclip's wants for the hook, and
// the 5 s lines from the hook's last snapshot. Names are resolved once the part-name table is verified (g_partNames == 1).
namespace {
bool g_clipWantLast = false;         // the unclip was wanted on the last frame
uint64_t g_clipOnMs = 0;             // when the unclip was last turned on
bool g_clipUnseenNoted = false;      // the no-AMC line was written for this turn-on
uint64_t g_clipLogStamp = 0;         // the snapshot stamp last logged
uint64_t g_clipLogMs = 0;            // when a snapshot was last logged
uint64_t g_clipFlagsAmc = 0;         // the AMC whose flags were logged (0 = none)
bool g_clipResolved = false;         // the names were looked up
bool g_clipProbeLast = false, g_clipGearUnseen = false;
uint64_t g_clipProbeOnMs = 0, g_clipProbeStartStamp = 0, g_clipGearLogStamp = 0;
uint32_t g_clipGearReports = 0;       // at most three default snapshots per process; detail retains the existing 5 s cadence

void reportClipGear(const BodyClipSnap& s) {
    for (size_t i = kClipGearFirst; i < kClipParts; ++i) {
        const ClipPart& p = s.part[i];
        const char* status = !p.named ? "name-unavailable" : p.readFault ? "read-fault" : !p.present ? "no-instance" :
            p.mod == 0 ? "no-modifier" : !p.readOk ? "modifier-unreadable" : "available";
        say(g_frameSink, "native on foot gear clip: %s status=%s prewrite=1 amc=0x%llX avatarPose=0x%llX skeleton=0x%llX gpuSkinOwner=unavailable "
            "sampleMs=%llu item=%d state=%d flags=0x%02X mod=0x%llX enable=%u dirty=%u amount=%.4f "
            "instances=%llX,%llX,%llX,%llX masks=%llX,%llX,%llX,%llX",
            kClipPartNames[i], status, s.amc, s.avatarPose, s.skeleton, s.stamp, p.currentItem, p.state, p.flags, p.mod,
            p.enable, p.dirty, static_cast<double>(p.amount), p.instances[0], p.instances[1], p.instances[2], p.instances[3],
            p.masks[0], p.masks[1], p.masks[2], p.masks[3]);
    }
}
}  // namespace

void nativeOnFootBodyClipTick(uint64_t now, bool unclipWant, bool probeWant, const char* reason) {
    const char* why = reason ? reason : "";
    if (!g_clipResolved && g_partNames.load(std::memory_order_relaxed) == 1) {
        int found[kClipParts];
        std::string missing;
        {
            for (size_t i = 0; i < kClipParts; ++i) {
                found[i] = avh::partIndexByName(kClipPartNames[i]);
                if (found[i] < 0) {
                    if (!missing.empty()) missing += ", ";
                    missing += kClipPartNames[i];
                }
            }
            std::lock_guard<std::mutex> lock(g_fpBodyMutex);
            for (size_t i = 0; i < kClipParts; ++i) g_clipIdx[i] = found[i];
        }
        g_clipResolved = true;
        if (!missing.empty()) Log::get().note("native on foot body clip: part(s) not in the part-name table, left alone: %s", missing.c_str());
    }
    if (probeWant && !g_clipProbeLast) {
        g_clipProbeOnMs = now;
        g_clipGearUnseen = false;
        {
            std::lock_guard<std::mutex> lock(g_fpBodyMutex);
            g_clipProbeStartStamp = g_clipSnap.stamp;
        }
        if (g_clipGearReports < 3)
            say(g_frameSink, "native on foot gear clip: armed; prewrite probe of local mode-3 lower-arm gear, at most three default snapshots per process; detail continues every 5 s");
    }
    g_clipProbeLast = probeWant;
    g_unclipWant.store(unclipWant, std::memory_order_release);
    g_clipProbeWant.store(probeWant, std::memory_order_release);
    if (unclipWant && !g_clipWantLast) {
        g_clipOnMs = now;
        g_clipUnseenNoted = false;
        Log::get().note("native on foot body unclip: on (EVASuit torso, arms and hands, both shoulders, the flight suit's torso, arms, hands and shoulder pads, and lower-arm gear: enable 0, dirty 1, local third-person AMC)");
    }
    if (!unclipWant && g_clipWantLast)
        Log::get().note("native on foot body unclip: off (%s); nothing is restored: the game writes its own enable on its next item apply, and the value it had was not kept", why);
    g_clipWantLast = unclipWant;
    if (!unclipWant && !probeWant) return;
    BodyClipSnap s;
    {
        std::lock_guard<std::mutex> lock(g_fpBodyMutex);
        s = g_clipSnap;
    }
    const bool freshProbe = s.stamp != 0 && s.stamp >= g_clipProbeOnMs && s.stamp != g_clipProbeStartStamp;
    if (probeWant && !freshProbe && !g_clipGearUnseen && now - g_clipProbeOnMs >= 5000) {
        g_clipGearUnseen = true;
        if (g_clipGearReports < 3)
            say(g_frameSink, "native on foot gear clip: unavailable; no new local mode-3 fade sample in the first 5 s (names=%d); modifier state unknown",
                g_partNames.load(std::memory_order_relaxed));
    }
    if (probeWant && freshProbe && s.stamp != g_clipGearLogStamp) {
        g_clipGearLogStamp = s.stamp;
        if (g_clipGearReports < 3 || nofoot::logwin::detail()) {
            reportClipGear(s);
            if (g_clipGearReports < 3 && ++g_clipGearReports == 3)
                say(g_frameSink, "native on foot gear clip: default snapshot limit reached; further samples require detail logging");
        }
    }
    // The hook never saw the local third-person AMC: say so, so "no lines" cannot be read as "all written".
    if (unclipWant && s.stamp == 0 && !g_clipUnseenNoted && now - g_clipOnMs >= 5000) {
        g_clipUnseenNoted = true;
        Log::get().note("native on foot body unclip: the local third-person AMC has not reached the fade hook in the first 5 s with the key on; nothing is written");
    }
    if (now - g_clipLogMs < 5000) return;
    if (s.stamp == 0 || s.stamp == g_clipLogStamp) return;
    g_clipLogStamp = s.stamp;
    g_clipLogMs = now;
    // F125: the 5 s writes line and the per-part lines are the detail log's; the window is taken as before, and the flags line (once per AMC) and the
    // on / off / not-reached lines above are written either way.
    const bool say = nofoot::logwin::detail();
    if (unclipWant) {
        const uint64_t writes = g_clipWrites.exchange(0, std::memory_order_relaxed);
        const uint64_t faults = g_clipWriteFaults.load(std::memory_order_relaxed);
        if (say)
            Log::get().note("native on foot body unclip: %llu modifier writes in the last 5 s (AMC 0x%llX, faults so far %llu)",
                            static_cast<unsigned long long>(writes), static_cast<unsigned long long>(s.amc),
                            static_cast<unsigned long long>(faults));
    } else {
        g_clipWrites.store(0, std::memory_order_relaxed);
    }
    if (s.amc != g_clipFlagsAmc) {
        g_clipFlagsAmc = s.amc;
        if (s.flagsOk)
            Log::get().note("native on foot body clip flags: [AMC+0x321]=0x%02X [AMC+0x322]=0x%02X [AMC+0x378]=0x%02X", s.flags[0], s.flags[1], s.flags[2]);
        else
            Log::get().note("native on foot body clip flags: not readable");
    }
    for (size_t i = 0; i < kClipGearFirst && say; ++i) {
        if (!s.part[i].named) continue;
        const ClipPart& p = s.part[i];
        if (i >= kClipUnclipParts && !p.present) continue;   // the comparison parts only where the AMC has them
        Log::get().note("native on foot body clip: %s mod=0x%llX enable=%u dirty=%u amount=%.4f%s", kClipPartNames[i],
                        static_cast<unsigned long long>(p.mod), static_cast<unsigned>(p.enable), static_cast<unsigned>(p.dirty),
                        static_cast<double>(p.amount),
                        p.readFault ? " (part read fault)" : !p.present ? " (no instance on this AMC)" : (p.mod != 0 && !p.readOk) ? " (modifier not readable)" : "");
    }
}

// ---- F21 (TEMPORARY): the hybrid first-person body (fix.native_on_foot_body_hybrid) ------------------------------------------------------
// Frame thread, every frame. The hook does the writes: bodyClipStep (the third-person torso's clip; the arms, hands and shoulders' view masks) and
// fpBodyStep (the first-person skeleton's masks only). The 5 s census shows what the hook last read, before its write in that call.
namespace {
bool g_hybridWantLast = false;      // the hybrid was wanted on the last frame
uint64_t g_hybridLogMs = 0;         // when the census was last logged
uint64_t g_hybridLogStamp = 0;      // the clip probe's snapshot stamp last logged
}  // namespace

void nativeOnFootHybridTick(uint64_t now, bool want, const char* reason) {
    const char* why = reason ? reason : "";
    g_hybridWant.store(want, std::memory_order_release);
    if (want && !g_hybridWantLast) {
        g_hybridLogMs = now;
        g_hybridLogStamp = 0;
        Log::get().note("native on foot body hybrid: on (first-person AMC: first-person skeleton hidden, its arms and hands kept; "
                        "third-person AMC: EVASuit torso unclipped, its arms, hands and shoulders hidden)");
    } else if (!want && g_hybridWantLast) {
        Log::get().note("native on foot body hybrid: off (%s); nothing is restored: the game writes its own clip on its next item apply", why);
    }
    g_hybridWantLast = want;
    if (!want || now - g_hybridLogMs < 5000) return;
    g_hybridLogMs = now;
    BodyClipSnap s;
    {
        std::lock_guard<std::mutex> lock(g_fpBodyMutex);
        s = g_clipSnap;
    }
    const FpBodyReadback fp = nativeOnFootFpBodyReadback();
    const uint64_t maskWrites = g_hybridMaskWrites.exchange(0, std::memory_order_relaxed);
    const uint64_t torsoWrites = g_clipWrites.exchange(0, std::memory_order_relaxed);
    if (s.stamp == 0) {
        Log::get().note("native on foot body hybrid: the local third-person AMC has not reached the hook yet (no torso or mask writes on it)");
    } else if (s.stamp != g_hybridLogStamp) {
        g_hybridLogStamp = s.stamp;
        const auto orMasks = [](const uint64_t* m) { return m[0] | m[1] | m[2] | m[3]; };   // nonzero = visible
        const ClipPart& t = s.part[0];
        Log::get().note("native on foot body hybrid: third-person AMC 0x%llX: torso %s enable=%u dirty=%u | view masks before this call's zero: "
                        "arms 0x%llX hands 0x%llX shoulder L 0x%llX shoulder R 0x%llX (0 = hidden) | last 5 s: torso writes %llu, mask words zeroed %llu",
                        static_cast<unsigned long long>(s.amc), t.named ? "named" : "NOT in the part table", static_cast<unsigned>(t.enable),
                        static_cast<unsigned>(t.dirty), static_cast<unsigned long long>(orMasks(s.part[1].masks)),
                        static_cast<unsigned long long>(orMasks(s.part[2].masks)), static_cast<unsigned long long>(orMasks(s.part[3].masks)),
                        static_cast<unsigned long long>(orMasks(s.part[4].masks)), static_cast<unsigned long long>(torsoWrites),
                        static_cast<unsigned long long>(maskWrites));
    }
    if (fp.amc == 0) {
        Log::get().note("native on foot body hybrid: no first-person AMC has reached the hook yet (nothing hidden on it)");
    } else {
        Log::get().note("native on foot body hybrid: first-person AMC 0x%llX: first-person skeleton view masks after the zero 0x%llX (arms 0x%llX, "
                        "hands 0x%llX kept; 0 = hidden)",
                        static_cast<unsigned long long>(fp.amc), static_cast<unsigned long long>(fp.masks[2][0] | fp.masks[2][1] | fp.masks[2][2] | fp.masks[2][3]),
                        static_cast<unsigned long long>(fp.masks[0][0] | fp.masks[0][1] | fp.masks[0][2] | fp.masks[0][3]),
                        static_cast<unsigned long long>(fp.masks[1][0] | fp.masks[1][1] | fp.masks[1][2] | fp.masks[1][3]));
    }
}

FpBodyReadback nativeOnFootFpBodyReadback() {
    FpBodyReadback out;
    {
        std::lock_guard<std::mutex> lock(g_fpBodyMutex);
        out.amc = g_fpReadback.amc;
        std::memcpy(out.masks, g_fpReadback.masks, sizeof(out.masks));
        for (uint32_t i = 0; i < avh::kFpBodyPartCount; ++i) {
            if (g_fpIdx[i] < 0) continue;
            out.name[i] = avh::kFpBodyPartNames[i];
            ++out.parts;
        }
    }
    out.mode2Seen = g_fpMode2Seen.load(std::memory_order_relaxed);
    return out;
}
bool neckCensusRead(NeckCensusReading* out) { return neckCensusCopy(out); }
uint64_t localAmcPointer() { return g_amcLocal.load(); }   // F18
NativeOnFootCameraStats nativeOnFootCameraStats() {   // F93d
    NativeOnFootCameraStats s;
    const HookEntry& h = g_hooks[kHkCtl];
    s.controllerHook = h.state == kHookStateArmed ? 1 : h.state == kHookStateStoodDown ? 2 : 0;
    s.why = h.state == kHookStateStoodDown ? h.why : "";
    s.controller = g_nativeCamCtl.load(std::memory_order_relaxed);
    s.switches = g_nativeCamCtlSwitches.load(std::memory_order_relaxed);
    s.faults = g_nativeCamReadFaults.load(std::memory_order_relaxed);
    s.headHeld = g_nativeHideHeld.load(std::memory_order_relaxed);
    s.clipHeld = g_clipExternalSkips.load(std::memory_order_relaxed);
    return s;
}
// F95c: the head through the visibility call as shadow-only (native_on_foot_hooks.h). The hook thread; the views are read before anything is written, and a call that
// cannot be covered leaves no ticket (the fade hook then zeroes, as before F95c).
bool nativeOnFootHeadShadowBegin(void* amcPtr, void* job, nofoot::f95c::Swap* s, bool allow) noexcept {
    s->armed = false;
    s->n = 0;
    if (!amcPtr) return false;
    uint64_t mine = reinterpret_cast<uint64_t>(amcPtr);
    if (g_f95cTicket.load(std::memory_order_relaxed) == mine)   // this AMC's previous call no longer stands for its masks (every avatar's call reads; one locks)
        g_f95cTicket.compare_exchange_strong(mine, 0, std::memory_order_acq_rel);
    if (!allow || !job || !g_hideOn.load(std::memory_order_acquire) || g_hideDown.load(std::memory_order_acquire) || g_f95cDown.load(std::memory_order_acquire))
        return false;
    if (reinterpret_cast<uint64_t>(amcPtr) != g_amcLocal.load(std::memory_order_relaxed) || !headHideWritesNow()) return false;
    uint8_t* const amc = static_cast<uint8_t*>(amcPtr);
    AmcIdent id;
    if (!sehReadAmcIdent(amc, &id) || !avh::isLocalAmc(id.id, id.pose, g_skelIface[0].load(std::memory_order_acquire), id.mode)) return false;
    g_f95cCalls.fetch_add(1, std::memory_order_relaxed);
    uint32_t flags[f95c::kMaxViews];
    uint64_t bits[f95c::kMaxViews];
    uint32_t n = 0;
    const int got = sehReadViews(static_cast<const uint8_t*>(job), flags, bits, &n);
    if (got <= 0) {
        g_f95cNoViews.fetch_add(1, std::memory_order_relaxed);
        if (got < 0) f95cFault();
        return false;
    }
    s->views = f95c::splitViews(flags, bits, n);
    if (g_f95cViewMutex.try_lock()) {   // the census of this call's views, for the frame thread's line (skipped, never waited for, while it reads)
        NativeOnFootHeadShadowViews& c = g_f95cViews;
        c = NativeOnFootHeadShadowViews{};
        c.n = s->views.n;
        c.main = s->views.main;
        c.shadow = s->views.shadow;
        c.other = s->views.other;
        c.shared = s->views.shared;
        for (uint32_t i = 0; i < n; ++i) {
            uint32_t j = 0;
            while (j < c.distinct && c.flags[j] != flags[i]) ++j;
            if (j < c.distinct) {
                ++c.count[j];
            } else if (c.distinct < 8) {
                c.flags[c.distinct] = flags[i];
                c.count[c.distinct++] = 1;
            } else {
                ++c.more;
            }
        }
        g_f95cViewMutex.unlock();
    }
    s->amc = amc;
    if (!sehSwapHeadClasses(amc, s)) {
        uint32_t lost = 0;
        sehRestoreHeadClasses(amc, s, &lost);   // the parts swapped before the fault go back
        f95cFault();
        return false;
    }
    s->armed = true;
    g_f95cSwapped.fetch_add(1, std::memory_order_relaxed);
    g_f95cParts.fetch_add(s->n, std::memory_order_relaxed);
    return true;
}
void nativeOnFootHeadShadowEnd(nofoot::f95c::Swap* s) noexcept {
    if (!s->armed) return;
    s->armed = false;
    uint32_t lost = 0;
    if (!sehRestoreHeadClasses(s->amc, s, &lost)) {
        f95cFault();
        return;
    }
    g_f95cLost.fetch_add(lost, std::memory_order_relaxed);
    uint64_t sp = 0, ep = 0;
    if (!sehCountHeadPairs(s->amc, s->views.shadowBits, s->views.deny, &sp, &ep)) {
        f95cFault();
        return;
    }
    g_f95cShadowPairs.fetch_add(sp, std::memory_order_relaxed);
    g_f95cEyePairs.fetch_add(ep, std::memory_order_relaxed);
    g_f95cDeny.store(s->views.deny, std::memory_order_relaxed);
    g_f95cTicket.store(reinterpret_cast<uint64_t>(s->amc), std::memory_order_release);
}
NativeOnFootHeadShadowStats nativeOnFootHeadShadowStats() {
    NativeOnFootHeadShadowStats s;
    s.calls = g_f95cCalls.load(std::memory_order_relaxed);
    s.swapped = g_f95cSwapped.load(std::memory_order_relaxed);
    s.parts = g_f95cParts.load(std::memory_order_relaxed);
    s.noViews = g_f95cNoViews.load(std::memory_order_relaxed);
    s.lost = g_f95cLost.load(std::memory_order_relaxed);
    s.shadowPairs = g_f95cShadowPairs.load(std::memory_order_relaxed);
    s.eyePairs = g_f95cEyePairs.load(std::memory_order_relaxed);
    s.kept = g_f95cKept.load(std::memory_order_relaxed);
    s.keptWords = g_f95cKeptWords.load(std::memory_order_relaxed);
    s.removed = g_f95cRemoved.load(std::memory_order_relaxed);
    s.zeroed = g_f95cZeroed.load(std::memory_order_relaxed);
    s.held = g_nativeHideHeld.load(std::memory_order_relaxed);
    s.faults = g_f95cFaults.load(std::memory_order_relaxed);
    s.down = g_f95cDown.load(std::memory_order_acquire);
    {
        std::lock_guard<std::mutex> lock(g_f95cViewMutex);
        s.views = g_f95cViews;
    }
    return s;
}

// F142 (always on since F146): native_on_foot_hooks.h says what these do. The hook thread: the same gate as F95c's swap (the head hide capable, not stood down, the local
// third-person AMC, the head hide writing now); one compare-exchange per part the game has in the shadow-views-only class. A call nothing was promoted in leaves nothing to
// restore (Begin returns false and End is not called).
bool nativeOnFootOutfitBegin(void* amcPtr, nofoot::f142::OutfitSwap* s, bool allow) noexcept {
    s->armed = false;
    s->n = 0;
    s->refused = 0;
    if (!amcPtr || !allow) return false;
    if (!g_hideOn.load(std::memory_order_acquire) || g_hideDown.load(std::memory_order_acquire) || g_f142Down.load(std::memory_order_acquire)) return false;
    if (reinterpret_cast<uint64_t>(amcPtr) != g_amcLocal.load(std::memory_order_relaxed) || !headHideWritesNow()) return false;
    uint8_t* const amc = static_cast<uint8_t*>(amcPtr);
    AmcIdent id;
    if (!sehReadAmcIdent(amc, &id) || !avh::isLocalAmc(id.id, id.pose, g_skelIface[0].load(std::memory_order_acquire), id.mode)) return false;
    g_f142Calls.fetch_add(1, std::memory_order_relaxed);
    s->amc = amc;
    static const int skeleton = avh::partIndexByName("FirstPersonSkeleton");
    // F147: EVASuit_Gear_Back alone, and only for an outer garment (F144's decision); every other part, and the other suit's backpack, keeps the game's class
    const uint64_t promote = nofoot::f144::promoteMask(promoteAllowedMask(), gearBackNow(amc));
    if (!promote) return false;
    if (!sehPromoteOutfit(amc, s, static_cast<uint32_t>(skeleton), ~promote)) {
        uint32_t lost = 0;
        sehRestoreOutfit(amc, s, &lost);   // the parts promoted before the fault go back
        f142Fault();
        return false;
    }
    if (s->refused) g_f142Refused.fetch_add(s->refused, std::memory_order_relaxed);
    if (s->n == 0) return false;
    s->armed = true;
    g_f142Swapped.fetch_add(1, std::memory_order_relaxed);
    g_f142Promoted.fetch_add(s->n, std::memory_order_relaxed);
    for (uint32_t j = 0; j < s->n; ++j) g_f142Part[s->part[j]].fetch_add(1, std::memory_order_relaxed);
    return true;
}
void nativeOnFootOutfitEnd(nofoot::f142::OutfitSwap* s) noexcept {
    if (!s->armed) return;
    s->armed = false;
    uint32_t lost = 0;
    if (!sehRestoreOutfit(s->amc, s, &lost)) {
        f142Fault();
        return;
    }
    if (lost) g_f142Lost.fetch_add(lost, std::memory_order_relaxed);
}
NativeOnFootOutfitStats nativeOnFootOutfitStats() {
    NativeOnFootOutfitStats s;
    s.calls = g_f142Calls.load(std::memory_order_relaxed);
    s.swapped = g_f142Swapped.load(std::memory_order_relaxed);
    s.promoted = g_f142Promoted.load(std::memory_order_relaxed);
    s.refused = g_f142Refused.load(std::memory_order_relaxed);
    s.lost = g_f142Lost.load(std::memory_order_relaxed);
    for (uint32_t i = 0; i < avh::kPartCount; ++i) s.part[i] = g_f142Part[i].load(std::memory_order_relaxed);
    s.faults = g_f142Faults.load(std::memory_order_relaxed);
    s.down = g_f142Down.load(std::memory_order_acquire);
    for (uint32_t i = 0; i < avh::kPartCount; ++i) s.clip[i] = g_f143ClipPart[i].load(std::memory_order_relaxed);   // F143
    s.clipWrites = g_f143ClipWrites.load(std::memory_order_relaxed);
    s.clipFaults = g_f143ClipFaults.load(std::memory_order_relaxed);
    return s;
}
void nativeOnFootArmedStateSet(uint8_t state) noexcept {
    g_f143Armed.store(state, std::memory_order_relaxed);
}
bool nativeOnFootOutfitClipTake(nofoot::f143::ClipSnap* out) noexcept {
    return out && g_f143Ring.take(out);
}
int nativeOnFootGearBack() noexcept {
    return g_f144Gear.load(std::memory_order_relaxed);
}
bool nativeOnFootFpCensusTake(nofoot::f144::FpCensus* out) noexcept {
    return out && g_f144FpRing.take(out);
}
bool nativeOnFootOutfitCensusTake(nofoot::f142::Census* out) noexcept {
    return out && g_f142CensusRing.take(out);
}

#ifdef EDVR_NATIVE_ON_FOOT_HOOKS_TEST
namespace nofoothookstest {
void setTargets(const NativeOnFootHooksTestTargets& t) {
    g_testTargets[kHkCtl] = t.controller;
    g_testTargets[kHkFade] = t.avatarFade;
    g_testTargets[kHkFind] = t.findJoint;
}
void setFadeGlobal(int32_t* mode, float* amount) {
    g_testFadeMode = mode;
    g_testFadeAmount = amount;
}
void setHeadImage(uintptr_t base, size_t size) {
    g_testHeadBase = base;
    g_testHeadSize = size;
}
void boundary(NativeOnFootHooksSinkFn fn, void* ctx) { boundaryAt(avh::Sink{fn, ctx}); }
void clipProbeClock(uint64_t now) { g_clipTestNow = now; }
void clipProbeSink(NativeOnFootHooksSinkFn fn, void* ctx) { g_frameSink = avh::Sink{fn, ctx}; }
void shutdown() { nativeOnFootHooksShutdown(); }
bool fadeOurs() { return g_nativeFade.ours(); }
size_t stolenBytes(int hook) { return hook >= 0 && hook < kHkCount ? g_hooks[hook].hook.stolenBytes() : 0; }
int hookState(int hook) { return hook >= 0 && hook < kHkCount ? g_hooks[hook].state : -1; }
bool gateOpen(int hook) { return hook >= 0 && hook < kHkCount && g_gate[hook].load() != 0; }
uint64_t controllerCalls() { return g_ctlCalls.load(); }
bool skeletonArmed() { return g_skelArmed.load(); }
void setSkeleton(int site, uint64_t iface, uint32_t index) {   // a latched pair member set by hand
    g_skelIndex[site].store(index);
    g_skelIface[site].store(iface);
    g_skelLatches.fetch_add(1);
}
bool headHideOn() { return g_hideOn.load(); }
bool headHideDown() { return g_hideDown.load(); }
int partNamesState() { return g_partNames.load(); }
uint64_t hideCalls() { return g_hideCalls.load(); }
uint64_t hideZeroed() { return g_hideZeroed.load(); }
uint64_t amcCalls() { return g_amcCalls.load(); }
uint64_t amcLocalMatches() { return g_amcLocalMatches.load(); }
uint64_t amcLocal() { return g_amcLocal.load(); }
void reset() {
    for (HookEntry& entry : g_hooks) {
        entry.hook.uninstall();
        entry.relay = nullptr;
        entry.target = 0;
        entry.state = kHookStateNotTried;
        entry.stolen = 0;
        entry.why[0] = 0;
    }
    for (int i = 0; i < kHkCount; ++i) {
        g_forward[i].store(0);
        g_gate[i].store(0);
        g_testTargets[i] = 0;
    }
    for (int i = 0; i < 2; ++i) {
        g_skelIface[i].store(0);
        g_skelIndex[i].store(0xFFFF);
        g_skelCaptures[i].store(0);
        g_findSite[i].store(0);
    }
    g_skelLatches.store(0);
    t_site1 = Site1Seen();
    g_findPov.store(0);
    g_findSeen.store(0);
    g_hideOn.store(false);
    g_hideDown.store(false);
    g_hideFaults.store(0);
    g_f95cTicket.store(0);   // F95c
    g_f95cDown.store(false);
    g_f95cFaults.store(0);
    g_partNames.store(0);
    g_amcCalls.store(0);
    g_amcLocalMatches.store(0);
    g_amcIdOk.store(0);
    g_amcMode3.store(0);
    g_hideCalls.store(0);
    g_hideZeroed.store(0);
    g_amcLocal.store(0);
    g_amcLocalPose.store(0);
    g_amcCand.store(0);
    g_amcCandPose.store(0);
    g_busyAmc.store(false);
    g_amcCensused = 0;
    {
        avh::AmcCensus drop;
        while (g_censusRing.take(&drop)) {}
    }
    {   // F142: the full outfit and its census start from nothing at the next launch of the hooks
        g_f142Busy.store(false);
        g_f142Down.store(false);
        g_f142Faults.store(0);
        g_f142CensusDown.store(false);
        g_f142CensusFaults.store(0);
        g_f143Down.store(false);
        g_f143Faults.store(0);
        g_f143Reads.store(0);
        g_f143SeenMask.store(0);
        g_f143SeenGen.store(0);
        g_f142OutfitGen.store(0);
        g_f144FpFaults.store(0);
        g_f142HaveSig = false;
        g_f142Sig = 0;
        g_f142CensusMs.store(0);
        static nofoot::f142::Census dropped;   // (3 KB: not on this stack)
        while (g_f142CensusRing.take(&dropped)) {}
        g_f144Gear.store(0);         // F144
        g_f144FpBusy.store(false);
        g_f144FpMs.store(0);
        g_f144FpHaveSig = false;
        g_f144FpSig = 0;
        static nofoot::f144::FpCensus droppedFp;
        while (g_f144FpRing.take(&droppedFp)) {}
        g_f143SeenArmed.store(-1);   // F143: the next launch of the hooks reads at once
        g_f143Busy.store(false);
        g_f143Armed.store(0);
        static nofoot::f143::ClipSnap droppedClip;
        while (g_f143Ring.take(&droppedClip)) {}
    }
    g_ctlCalls.store(0);
    g_skelTargets = avh::HeadTargets();
    g_skelArmed.store(false);
    g_testHeadBase = 0;
    g_testHeadSize = 0;
    g_fadeMode = nullptr;
    g_fadeAmount = nullptr;
    g_testFadeMode = nullptr;
    g_testFadeAmount = nullptr;
    g_frame = FrameState();
    g_identity = 0;
    g_frameSink = avh::Sink{&logSink, nullptr};
    // Native on foot's body state and the camera signal (F93d).
    g_nativeWant.store(false);
    g_nativeBody.store(false);
    g_nativeReason = "";
    g_nativeFade = avh::FadeGuard();
    g_nativeHideHeld.store(0);
    g_clipExternalSkips.store(0);
    g_unclipWant.store(false);
    g_clipProbeWant.store(false);
    g_clipWrites.store(0);
    g_clipWriteFaults.store(0);
    g_clipNextMs.store(0);
    g_clipSnap = BodyClipSnap();
    g_clipIdx.fill(-1);
    g_clipResolved = g_clipWantLast = g_clipUnseenNoted = false;
    g_clipProbeLast = g_clipGearUnseen = false;
    g_clipOnMs = g_clipLogStamp = g_clipLogMs = g_clipFlagsAmc = 0;
    g_clipProbeOnMs = g_clipProbeStartStamp = g_clipGearLogStamp = g_clipTestNow = 0;
    g_clipGearReports = 0;
    g_nativeCamCtl.store(0);
    g_nativeCamCtlSwitches.store(0);
    g_nativeCamReadFaults.store(0);
    g_nativeOnFootCamExternal.store(false);
    g_nativeOnFootCamMode.store(0);
    g_nativeOnFootCamReads.store(0);
    g_nativeOnFootCamFlips.store(0);
    g_nativeOnFootCamFlipQpc.store(0);
}
}  // namespace nofoothookstest
#endif
}  // namespace edvr
