// Explorer Cam: the glue (explorer_cam.h says what it is; explorer_cam_core.h holds every decision, every byte of the relays and the
// text of every line, driven by tools\explorer_cam_test).
//
// WHAT RUNS WHERE.
//   hook thread   freeCameraHooked, on whichever thread the game's job system calls the free camera's update from:
//                   pre   (placementPre)   decide from the activity's flag bytes, write the pose, set the lock's pressed-int
//                   the original update
//                   post  (placementPost)  restore the lock's pressed-int, then the probe's observer
//                 It takes no lock (a try-flag keeps a second concurrent caller out), allocates nothing, writes no log line and
//                 calls nothing of the game's. Every access to the game's memory is under SEH.
//   collision     the relay in machine code (explorer_cam_core.h): no C++ between the game and the original.
//   frame thread  explorerCamFrameBoundary, the Present boundary: the only code that reads Config and the only code that logs.
//
// THE FLAGS THEY SHARE are atomics. The hook thread owns the Machine (explorer_cam_core.h); the frame thread asks it to start over
// by bumping g_resetRequest, which the hook thread acts on at its next call, and clears the published state (g_placedActivity,
// g_phase) itself whenever placement is not active, every frame, so a release never depends on the activity being called again.
#include "explorer_cam.h"
#include "explorer_cam_core.h"

#include <windows.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "../common/code_hook.h"
#include "../common/config.h"
#include "../common/log.h"

namespace edvr {
namespace {

// ---- state shared between the threads --------------------------------------------------------------------------------------
// The collision relay reads g_placedActivity and bumps the two counters from machine code, so they are plain 8-byte atomics.
alignas(8) std::atomic<uint64_t> g_placedActivity{0};   // the activity while a pose is being written, else 0
alignas(8) std::atomic<uint64_t> g_bypassed{0};         // collision calls answered "no edit" by the relay
alignas(8) std::atomic<uint64_t> g_forwarded{0};        // collision calls passed to the original
alignas(8) std::atomic<uintptr_t> g_relayGate{0};       // the free-camera relay: open = the callback runs, closed = straight to the original
std::atomic<uintptr_t> g_freeForward{0};                // the free-camera trampoline

std::atomic<bool> g_placeActive{false};                 // armed, on, and not stood down by faults (set by the frame thread)
std::atomic<bool> g_probeWants{false};
std::atomic<ExplorerCamActivityObserver> g_observer{nullptr};
std::atomic<uint32_t> g_resetRequest{0};
std::atomic<float> g_eyeUp{ecm::kEyeUpDefault}, g_eyeForward{ecm::kEyeForwardDefault}, g_eyeRight{ecm::kEyeRightDefault};

std::atomic<bool> g_busy{false};                        // a call is inside pre..post; a second concurrent caller skips placement
std::atomic<uint32_t> g_phase{0};                       // 0 idle, 1 waiting (tracked, not yet placeable), 2 placed
std::atomic<uint64_t> g_trackedActivity{0};
std::atomic<uint64_t> g_trackedCalls{0};                // calls from the tracked activity
std::atomic<uint64_t> g_updatesPlaced{0};
std::atomic<uint64_t> g_waitingUpdates{0};
std::atomic<uint64_t> g_hookCalls{0};
std::atomic<uint64_t> g_contended{0};
std::atomic<uint64_t> g_foreign{0};
std::atomic<uint32_t> g_faults{0};
std::atomic<uint32_t> g_lastState{0};

ecm::EventRing<64> g_events;

// ---- hook-thread state (touched only while g_busy is held) ----------------------------------------------------------------
ecm::Machine g_machine;
uint32_t g_resetSeen = 0;
bool g_firstCallNoted = false;

// ---- frame-thread state ------------------------------------------------------------------------------------------------------
struct FrameState {
    bool announced = false;
    bool lastOn = false;
    bool haveEye = false;
    ecm::Eye lastEye;
    bool skippedCollisionNoted = false;
    ecm::StaleWatch stale;
    uint32_t lastPhase = 0;
    uint64_t nextBeatMs = 0;
    uint64_t lastBeatMs = 0;
    uint64_t beatUpdates = 0, beatBypassed = 0, beatForwarded = 0, beatHookCalls = 0;
};
FrameState g_frame;

#ifdef EDVR_EXPLORER_CAM_TEST
uintptr_t g_testFreeTarget = 0;   // the rig's synthetic functions, installed in place of the game's
uintptr_t g_testCollisionTarget = 0;
#endif

// ---- guarded access to the game's memory (nothing with a destructor lives in a function that has a __try) ------------------
__declspec(noinline) bool sehReadObserved(const uint8_t* a, ecm::Observed* o) noexcept {
    __try {
        o->relative = a[ecm::kOffRelative];
        o->rotationLock = a[ecm::kOffRotationLock];
        o->presetPending = a[ecm::kOffPresetPending];
        o->state = a[ecm::kOffState];
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
__declspec(noinline) bool sehReadPose(const uint8_t* a, float out[16]) noexcept {
    __try {
        std::memcpy(out, a + ecm::kOffLocalPose, 64);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
__declspec(noinline) bool sehWritePose(uint8_t* a, const float in[16]) noexcept {
    __try {
        std::memcpy(a + ecm::kOffLocalPose, in, 64);
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
// The lock press: read the action object's pointer at activity+0x508, remember the pressed-int's value, set it to 1.
__declspec(noinline) bool sehSetLock(const uint8_t* a, uint64_t* action, int32_t* previous) noexcept {
    __try {
        uint64_t p = 0;
        std::memcpy(&p, a + ecm::kOffLockAction, 8);
        if (p < 0x10000u || p >= 0x00007FFF00000000ull || (p & 7u) != 0) return false;   // not an object pointer
        if (!writableRange(reinterpret_cast<const void*>(p), ecm::kOffActionPressed + 4)) return false;
        volatile int32_t* pressed = reinterpret_cast<volatile int32_t*>(p + ecm::kOffActionPressed);
        *previous = *pressed;
        *pressed = 1;
        *action = p;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
// The restore: the same action object, the value it held before. Writes whatever the game left there, one way or the other.
__declspec(noinline) bool sehRestoreLock(uint64_t action, int32_t previous) noexcept {
    __try {
        *reinterpret_cast<volatile int32_t*>(action + ecm::kOffActionPressed) = previous;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
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
        if (timestamp != ecm::kExpectedTimestamp || imageSize != ecm::kExpectedImageSize) {
            *why = "the PE timestamp or image size is not build 332841's";
            return false;
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *why = "a read faulted while checking the PE header";
        return false;
    }
}

// ---- the hook thread: pre, then the original, then post ----------------------------------------------------------------------
struct PreState {
    bool holdsBusy = false;
    bool pressed = false;
    uint64_t activity = 0;
    uint64_t action = 0;
    int32_t previous = 0;
};

void publishIdle() {
    g_placedActivity.store(0, std::memory_order_release);
    g_phase.store(0, std::memory_order_release);
    g_trackedActivity.store(0, std::memory_order_relaxed);
}

void pushEvent(ecm::EvKind kind, uint64_t activity, uint32_t why, uint32_t before, uint32_t after, uint32_t flags, const ecm::Eye* eye,
               uint32_t count) {
    ecm::Event e;
    e.kind = static_cast<uint32_t>(kind);
    e.activity = activity;
    e.threadId = GetCurrentThreadId();
    e.why = why;
    e.before = before;
    e.after = after;
    e.flags = flags;
    e.count = count;
    e.updates = g_updatesPlaced.load(std::memory_order_relaxed);
    if (eye) {
        e.eye[0] = eye->up;
        e.eye[1] = eye->forward;
        e.eye[2] = eye->right;
    }
    g_events.push(e);
}

// A guarded access faulted: count it, say where, end the session. The 8th fault ends the feature for the session.
void onFault(ecm::FaultSite site, uint64_t activity) {
    const uint32_t n = g_faults.fetch_add(1, std::memory_order_relaxed) + 1;
    pushEvent(ecm::EvKind::Fault, activity, static_cast<uint32_t>(site), 0, 0, 0, nullptr, n);
    g_machine.abort(ecm::Why::Fault);
    publishIdle();
    if (n == ecm::kMaxFaults) pushEvent(ecm::EvKind::FaultLimit, activity, 0, 0, 0, 0, nullptr, n);
}

void runPre(void* activityPtr, PreState& ps) {
    const uint32_t request = g_resetRequest.load(std::memory_order_acquire);
    if (request != g_resetSeen) {
        g_resetSeen = request;
        g_machine.reset();
    }
    if (!g_placeActive.load(std::memory_order_acquire) || g_faults.load(std::memory_order_relaxed) >= ecm::kMaxFaults) return;

    const uint64_t activity = reinterpret_cast<uint64_t>(activityPtr);
    const uint8_t* const bytes = static_cast<const uint8_t*>(activityPtr);
    ecm::Observed o;
    if (!sehReadObserved(bytes, &o)) {
        onFault(ecm::FaultSite::ReadActivity, activity);
        return;
    }
    g_lastState.store(o.state, std::memory_order_relaxed);
    if (!g_firstCallNoted) {
        g_firstCallNoted = true;
        pushEvent(ecm::EvKind::FirstCall, activity, 0, 0, o.state, ecm::packObserved(o), nullptr, 0);
    }

    const ecm::Step s = g_machine.step(activity, o, true);
    if (s.foreign) {
        g_foreign.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (s.pressResult) pushEvent(ecm::EvKind::LockResult, activity, 0, s.pressBefore, s.pressAfter, ecm::packObserved(o), nullptr, 0);
    if (s.released) {
        publishIdle();
        pushEvent(ecm::EvKind::Released, activity, static_cast<uint32_t>(s.why), 0, o.state, ecm::packObserved(o), nullptr, 0);
        if (!s.entered) return;
    }
    if (s.entered) {
        if (!writableRange(bytes, ecm::kActivityBytes)) {
            onFault(ecm::FaultSite::Validate, activity);
            return;
        }
        g_trackedActivity.store(activity, std::memory_order_relaxed);
        g_phase.store(1, std::memory_order_release);
        pushEvent(ecm::EvKind::Entered, activity, 0, 0, o.state, ecm::packObserved(o), nullptr, 0);
    }
    if (g_machine.phase() != ecm::Machine::Phase::Placing) return;   // an idle call: nothing is tracked
    g_trackedCalls.fetch_add(1, std::memory_order_relaxed);
    if (!s.write) {
        if (s.waiting) g_waitingUpdates.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    ecm::Eye eye;
    eye.up = g_eyeUp.load(std::memory_order_relaxed);
    eye.forward = g_eyeForward.load(std::memory_order_relaxed);
    eye.right = g_eyeRight.load(std::memory_order_relaxed);
    float current[16], out[16];
    if (!sehReadPose(bytes, current)) {
        onFault(ecm::FaultSite::ReadPose, activity);
        return;
    }
    ecm::buildLocalPose(current, eye, out);
    // Published BEFORE the write and the original run: this update's collision call must already see it.
    if (s.placeNow) g_placedActivity.store(activity, std::memory_order_release);
    if (!sehWritePose(const_cast<uint8_t*>(bytes), out)) {
        onFault(ecm::FaultSite::WritePose, activity);
        return;
    }
    g_updatesPlaced.fetch_add(1, std::memory_order_relaxed);
    if (s.placeNow) {
        g_phase.store(2, std::memory_order_release);
        pushEvent(ecm::EvKind::Placed, activity, s.alreadyLocked ? 1u : 0u, 0, o.state, ecm::packObserved(o), &eye, 0);
    }
    if (s.press) {
        if (!sehSetLock(bytes, &ps.action, &ps.previous)) {
            onFault(ecm::FaultSite::SetLock, activity);
            return;
        }
        ps.pressed = true;
    }
}

__declspec(noinline) PreState placementPre(void* activity) noexcept {
    PreState ps;
    g_hookCalls.fetch_add(1, std::memory_order_relaxed);
    if (!activity) return ps;
    if (g_busy.exchange(true, std::memory_order_acquire)) {
        g_contended.fetch_add(1, std::memory_order_relaxed);
        return ps;
    }
    ps.holdsBusy = true;
    ps.activity = reinterpret_cast<uint64_t>(activity);
    runPre(activity, ps);
    return ps;
}

__declspec(noinline) void placementPost(PreState& ps) noexcept {
    if (!ps.holdsBusy) return;
    if (ps.pressed && !sehRestoreLock(ps.action, ps.previous)) onFault(ecm::FaultSite::RestoreLock, ps.activity);
    ps.holdsBusy = false;
    g_busy.store(false, std::memory_order_release);
}

// Ghidra shows one parameter (rcx = the activity), a return value in rax and no float-register parameter read, so four integer
// registers are forwarded and nothing else is declared: declaring a float parameter would read an xmm register the caller never set.
using ForwardFn = uint64_t (__fastcall*)(void*, void*, void*, void*);

__declspec(noinline) uint64_t __fastcall freeCameraHooked(void* a, void* b, void* c, void* d) noexcept {
    const auto forward = reinterpret_cast<ForwardFn>(g_freeForward.load(std::memory_order_acquire));
    if (!forward) return 0;   // unreachable: the relay exists only after the install published a trampoline
    PreState ps = placementPre(a);
    const uint64_t result = forward(a, b, c, d);
    placementPost(ps);
    const auto observer = g_observer.load(std::memory_order_acquire);
    if (observer) observer(a);
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

struct HookEntry {
    CodeHook hook;
    uint8_t* relay = nullptr;
    uintptr_t target = 0;
    int state = ExplorerCamHookStatus::NotTried;
    size_t stolen = 0;
    char why[400] = {};
};
HookEntry g_free;        // 0x1071980
HookEntry g_collision;   // 0x1091140

// Seal a relay page and publish the trampoline into its literal slot before CodeHook makes the entry patch live.
bool sealRelay(HookEntry& entry, void* trampoline, size_t literalAt, size_t relayBytes) noexcept {
    const uintptr_t address = reinterpret_cast<uintptr_t>(trampoline);
    std::memcpy(entry.relay + literalAt, &address, 8);
    DWORD oldProtect = 0;
    return VirtualProtect(entry.relay, 4096, PAGE_EXECUTE_READ, &oldProtect) &&
           FlushInstructionCache(GetCurrentProcess(), entry.relay, relayBytes);
}
bool prepareFreeRelay(void* trampoline, void* context) noexcept {
    auto& entry = *static_cast<HookEntry*>(context);
    if (!sealRelay(entry, trampoline, ecm::kFreeCameraRelayTrampolineAt, ecm::kFreeCameraRelayBytes)) return false;
    g_freeForward.store(reinterpret_cast<uintptr_t>(trampoline), std::memory_order_release);
    return true;
}
bool prepareCollisionRelay(void* trampoline, void* context) noexcept {
    return sealRelay(*static_cast<HookEntry*>(context), trampoline, ecm::kCollisionRelayTrampolineAt, ecm::kCollisionRelayBytes);
}

// The PE identity is checked once per session; both hooks need it.
int g_identity = 0;   // 0 not checked, 1 ok, 2 differs
char g_identityWhy[200] = {};

// Resolve the target, verify the build and the prologue. On failure `why` says why in one sentence and nothing was patched.
bool resolveAndVerify(uintptr_t rva, uintptr_t testTarget, const uint8_t* prologue, size_t prologueBytes, const char* what, uintptr_t* target,
                      char* why, size_t whyCap) {
    uintptr_t t = testTarget;
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
                          static_cast<unsigned long long>(rva));
            return false;
        }
        t = base + rva;
    }
    if (!sehCheckBytes(t, prologue, prologueBytes)) {
        std::snprintf(why, whyCap,
                      "the game build differs (the %zu bytes at EliteDangerous64.exe+0x%llX are not build 332841's %s prologue); nothing was patched",
                      prologueBytes, static_cast<unsigned long long>(rva), what);
        return false;
    }
    *target = t;
    return true;
}

bool installFree(char* why, size_t whyCap) {
    uintptr_t target = 0;
    uintptr_t testTarget = 0;
#ifdef EDVR_EXPLORER_CAM_TEST
    testTarget = g_testFreeTarget;
#endif
    if (!resolveAndVerify(ecm::kFreeCameraRva, testTarget, ecm::kFreeCameraPrologue, ecm::kFreeCameraPrologueBytes,
                          "FreeCameraActivity update", &target, why, whyCap))
        return false;
    g_free.target = target;
    g_free.relay = allocateRelay(target);
    if (!g_free.relay) {
        std::snprintf(why, whyCap, "no executable memory could be placed within two gigabytes of the target; nothing was patched");
        return false;
    }
    ecm::buildFreeCameraRelay(g_free.relay, &g_relayGate, reinterpret_cast<const void*>(&freeCameraHooked));
    if (!g_free.hook.install(reinterpret_cast<void*>(target), g_free.relay, nullptr, "explorer-cam-free-camera-update", &prepareFreeRelay,
                             &g_free)) {
        VirtualFree(g_free.relay, 0, MEM_RELEASE);
        g_free.relay = nullptr;
        std::snprintf(why, whyCap, "CodeHook refused it (its own line above, tagged explorer-cam-free-camera-update, names why); nothing was patched");
        return false;
    }
    g_free.stolen = g_free.hook.stolenBytes();
    return true;
    // Process-lifetime storage: the relay and trampoline are never freed, so a call already inside them can finish.
}

bool installCollision(char* why, size_t whyCap) {
    uintptr_t target = 0;
    uintptr_t testTarget = 0;
#ifdef EDVR_EXPLORER_CAM_TEST
    testTarget = g_testCollisionTarget;
#endif
    if (!resolveAndVerify(ecm::kCollisionRva, testTarget, ecm::kCollisionPrologue, ecm::kCollisionPrologueBytes, "collision step", &target, why,
                          whyCap))
        return false;
    g_collision.target = target;
    g_collision.relay = allocateRelay(target);
    if (!g_collision.relay) {
        std::snprintf(why, whyCap, "no executable memory could be placed within two gigabytes of the target; nothing was patched");
        return false;
    }
    ecm::buildCollisionRelay(g_collision.relay, &g_placedActivity, &g_bypassed, &g_forwarded);
    if (!g_collision.hook.install(reinterpret_cast<void*>(target), g_collision.relay, nullptr, "explorer-cam-collision", &prepareCollisionRelay,
                                  &g_collision)) {
        VirtualFree(g_collision.relay, 0, MEM_RELEASE);
        g_collision.relay = nullptr;
        std::snprintf(why, whyCap, "CodeHook refused it (its own line above, tagged explorer-cam-collision, names why); nothing was patched");
        return false;
    }
    g_collision.stolen = g_collision.hook.stolenBytes();
    return true;
}

void say(const ecm::Sink& sink, const char* fmt, ...) {
    char line[ecm::kLineBytes];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    sink(line);
}
void logSink(void*, const char* line) { Log::get().note("%s", line); }

void tryFree(const ecm::Sink& sink) {
    char why[400] = {};
    if (installFree(why, sizeof(why))) {
        g_free.state = ExplorerCamHookStatus::Armed;
        say(sink, "%s free-camera hook armed: EliteDangerous64.exe+0x%llX (FreeCameraActivity update, build 332841) at 0x%llX, stolen=%zu bytes, "
                  "prologue %zu/%zu bytes verified, relay at 0x%llX; Explorer Cam and advanced.explorer_cam_probe share this hook: the "
                  "placement runs before the original, the lock press is restored after it, then the probe's snapshot",
            ecm::prefix(), static_cast<unsigned long long>(ecm::kFreeCameraRva), static_cast<unsigned long long>(g_free.target), g_free.stolen,
            ecm::kFreeCameraPrologueBytes, ecm::kFreeCameraPrologueBytes, static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(g_free.relay)));
    } else {
        g_free.state = ExplorerCamHookStatus::StoodDown;
        std::snprintf(g_free.why, sizeof(g_free.why), "%s", why);
        say(sink, "%s free-camera hook stood down: %s. Explorer Cam placement and advanced.explorer_cam_probe's I3 do not run.", ecm::prefix(), why);
    }
}
void tryCollision(const ecm::Sink& sink) {
    char why[400] = {};
    if (installCollision(why, sizeof(why))) {
        g_collision.state = ExplorerCamHookStatus::Armed;
        say(sink, "%s collision hook armed: EliteDangerous64.exe+0x%llX (the free camera's collision step, build 332841) at 0x%llX, stolen=%zu "
                  "bytes, prologue %zu/%zu bytes verified, relay at 0x%llX; it answers 0 (no collision edit) for the placed activity only and "
                  "hands every other call, all five arguments intact, to the original",
            ecm::prefix(), static_cast<unsigned long long>(ecm::kCollisionRva), static_cast<unsigned long long>(g_collision.target), g_collision.stolen,
            ecm::kCollisionPrologueBytes, ecm::kCollisionPrologueBytes, static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(g_collision.relay)));
    } else {
        g_collision.state = ExplorerCamHookStatus::StoodDown;
        std::snprintf(g_collision.why, sizeof(g_collision.why), "%s", why);
        say(sink, "%s collision hook stood down: %s. Explorer Cam placement does not run (without it the free camera would stop 0.70 m from the face).",
            ecm::prefix(), why);
    }
}

void updateGate() {
    const bool open = g_free.state == ExplorerCamHookStatus::Armed &&
                      (g_placeActive.load(std::memory_order_relaxed) || g_observer.load(std::memory_order_relaxed) != nullptr);
    g_relayGate.store(open ? 1u : 0u, std::memory_order_release);
}

// ---- the frame thread ----------------------------------------------------------------------------------------------------------
void releaseFromFrame(ecm::Why why, uint32_t frame, const ecm::Sink& sink) {
    ecm::Event e;
    e.kind = static_cast<uint32_t>(ecm::EvKind::Released);
    e.activity = g_trackedActivity.load(std::memory_order_relaxed);
    e.why = static_cast<uint32_t>(why);
    e.after = g_lastState.load(std::memory_order_relaxed);
    e.updates = g_updatesPlaced.load(std::memory_order_relaxed);
    char line[ecm::kLineBytes];
    ecm::formatEvent(line, sizeof(line), e, frame);
    sink(line);
    publishIdle();
    g_resetRequest.fetch_add(1, std::memory_order_release);
}

void boundaryAt(uint32_t frame, uint64_t nowMs, bool on, float up, float forward, float right, const ecm::Sink& sink) {
    FrameState& fs = g_frame;
    char line[ecm::kLineBytes];

    // 1. The settings, clamped and published. The eye is live: the next update writes the new one.
    const ecm::Eye eye = ecm::clampEye(up, forward, right);
    g_eyeUp.store(eye.up, std::memory_order_relaxed);
    g_eyeForward.store(eye.forward, std::memory_order_relaxed);
    g_eyeRight.store(eye.right, std::memory_order_relaxed);
    if (!fs.announced) {
        fs.announced = true;
        fs.lastOn = on;
        fs.lastEye = eye;
        fs.haveEye = true;
        if (on)
            say(sink, "%s on (fix.explorer_cam = on): in the on-foot free camera the view is placed at your commander's head and locked to "
                      "them; eye up=%.3f forward=%.3f right=%.3f m in the commander's frame from their feet (fix.explorer_cam_eye_up, "
                      "_eye_forward, _eye_right; live)",
                ecm::prefix(), eye.up, eye.forward, eye.right);
        else
            say(sink, "%s off (fix.explorer_cam = off): nothing is installed for it and nothing is written to the game", ecm::prefix());
    } else {
        if (on != fs.lastOn) {
            fs.lastOn = on;
            if (on)
                say(sink, "%s on again (fix.explorer_cam = on): eye up=%.3f forward=%.3f right=%.3f", ecm::prefix(), eye.up, eye.forward, eye.right);
            else
                say(sink, "%s off (fix.explorer_cam turned off while running): the session, if any, is released and nothing more is written",
                    ecm::prefix());
        }
        if (!ecm::sameEye(eye, fs.lastEye)) {
            say(sink, "%s eye changed: up=%.3f forward=%.3f right=%.3f (was up=%.3f forward=%.3f right=%.3f)", ecm::prefix(), eye.up, eye.forward,
                eye.right, fs.lastEye.up, fs.lastEye.forward, fs.lastEye.right);
            fs.lastEye = eye;
        }
    }

    // 2. The hooks, the first time they are wanted. The collision hook only after the free-camera hook is armed.
    if ((on || g_probeWants.load(std::memory_order_relaxed)) && g_free.state == ExplorerCamHookStatus::NotTried) tryFree(sink);
    if (on && g_collision.state == ExplorerCamHookStatus::NotTried) {
        if (g_free.state == ExplorerCamHookStatus::Armed) {
            tryCollision(sink);
        } else if (g_free.state == ExplorerCamHookStatus::StoodDown && !fs.skippedCollisionNoted) {
            fs.skippedCollisionNoted = true;
            say(sink, "%s collision hook not installed: the free-camera hook stood down, so Explorer Cam placement does not run.", ecm::prefix());
        }
    }

    // 3. Active = on, both hooks armed, and the fault budget not spent. A change starts the hook thread's machine over.
    const bool active = on && g_free.state == ExplorerCamHookStatus::Armed && g_collision.state == ExplorerCamHookStatus::Armed &&
                        g_faults.load(std::memory_order_relaxed) < ecm::kMaxFaults;
    if (active != g_placeActive.load(std::memory_order_relaxed)) {
        g_placeActive.store(active, std::memory_order_release);
        g_resetRequest.fetch_add(1, std::memory_order_release);
    }
    updateGate();

    // 4. What the hook thread said.
    ecm::Event ev;
    while (g_events.take(&ev)) {
        ecm::formatEvent(line, sizeof(line), ev, frame);
        sink(line);
    }

    // 5. Not active: whatever the hook thread left published is withdrawn, every frame, until it is gone.
    uint32_t phase = g_phase.load(std::memory_order_acquire);
    if (!active) {
        if (phase != 0 || g_placedActivity.load(std::memory_order_acquire) != 0) {
            if (phase != 0) releaseFromFrame(ecm::Why::KeyOff, frame, sink);
            else publishIdle();
        }
        fs.stale.tick(false, 0);
        fs.lastPhase = 0;
        return;
    }

    // 6. A session whose activity went silent is released.
    phase = g_phase.load(std::memory_order_acquire);
    if (fs.stale.tick(phase != 0, g_trackedCalls.load(std::memory_order_relaxed))) {
        releaseFromFrame(ecm::Why::Stale, frame, sink);
        phase = 0;
    }

    // 7. The 5 s heartbeat while a session is on.
    if (phase == 0) {
        fs.lastPhase = 0;
        return;
    }
    if (fs.lastPhase == 0) {
        fs.nextBeatMs = nowMs + 5000;
        fs.lastBeatMs = nowMs;
        fs.beatUpdates = g_updatesPlaced.load(std::memory_order_relaxed);
        fs.beatBypassed = g_bypassed.load(std::memory_order_relaxed);
        fs.beatForwarded = g_forwarded.load(std::memory_order_relaxed);
        fs.beatHookCalls = g_hookCalls.load(std::memory_order_relaxed);
    }
    fs.lastPhase = phase;
    if (nowMs >= fs.nextBeatMs) {
        ecm::HeartbeatIn h;
        h.windowSeconds = static_cast<double>(nowMs - fs.lastBeatMs) / 1000.0;
        h.phase = phase == 2 ? "placed" : "waiting";
        h.activity = g_trackedActivity.load(std::memory_order_relaxed);
        h.state = g_lastState.load(std::memory_order_relaxed);
        h.updates = g_updatesPlaced.load(std::memory_order_relaxed);
        h.bypassed = g_bypassed.load(std::memory_order_relaxed);
        h.forwarded = g_forwarded.load(std::memory_order_relaxed);
        h.hookCalls = g_hookCalls.load(std::memory_order_relaxed);
        h.updatesWindow = h.updates - fs.beatUpdates;
        h.bypassedWindow = h.bypassed - fs.beatBypassed;
        h.forwardedWindow = h.forwarded - fs.beatForwarded;
        h.hookCallsWindow = h.hookCalls - fs.beatHookCalls;
        h.waiting = g_waitingUpdates.load(std::memory_order_relaxed);
        h.contended = g_contended.load(std::memory_order_relaxed);
        h.foreign = g_foreign.load(std::memory_order_relaxed);
        h.lost = g_events.lost();
        h.faults = g_faults.load(std::memory_order_relaxed);
        h.eye = eye;
        ecm::formatHeartbeat(line, sizeof(line), h);
        sink(line);
        fs.beatUpdates = h.updates;
        fs.beatBypassed = h.bypassed;
        fs.beatForwarded = h.forwarded;
        fs.beatHookCalls = h.hookCalls;
        fs.lastBeatMs = nowMs;
        fs.nextBeatMs = nowMs + 5000;
    }
}

}  // namespace

void explorerCamFrameBoundary(uint32_t frameNo) {
    const Config& cfg = Config::get();
    const bool on = cfg.getBool("fix.explorer_cam", true);
    const float up = cfg.getFloat("fix.explorer_cam_eye_up", ecm::kEyeUpDefault);
    const float forward = cfg.getFloat("fix.explorer_cam_eye_forward", ecm::kEyeForwardDefault);
    const float right = cfg.getFloat("fix.explorer_cam_eye_right", ecm::kEyeRightDefault);
    boundaryAt(frameNo, GetTickCount64(), on, up, forward, right, ecm::Sink{&logSink, nullptr});
}

ExplorerCamHookStatus explorerCamProbeAttach(bool want, ExplorerCamActivityObserver observer) {
    g_probeWants.store(want, std::memory_order_relaxed);
    if (want && g_free.state == ExplorerCamHookStatus::NotTried) tryFree(ecm::Sink{&logSink, nullptr});
    g_observer.store(want && g_free.state == ExplorerCamHookStatus::Armed ? observer : nullptr, std::memory_order_release);
    updateGate();
    ExplorerCamHookStatus status;
    status.state = g_free.state;
    status.stolen = g_free.stolen;
    status.target = g_free.target;
    status.relay = reinterpret_cast<uintptr_t>(g_free.relay);
    std::snprintf(status.why, sizeof(status.why), "%s", g_free.why);
    return status;
}

#ifdef EDVR_EXPLORER_CAM_TEST
namespace explorercamtest {
void setTargets(uintptr_t freeCamera, uintptr_t collision) {
    g_testFreeTarget = freeCamera;
    g_testCollisionTarget = collision;
}
void boundary(uint32_t frame, uint64_t nowMs, bool on, float up, float forward, float right, ExplorerCamSinkFn fn, void* ctx) {
    boundaryAt(frame, nowMs, on, up, forward, right, ecm::Sink{fn, ctx});
}
bool gateOpen() { return g_relayGate.load() != 0; }
size_t freeStolen() { return g_free.hook.stolenBytes(); }
size_t collisionStolen() { return g_collision.hook.stolenBytes(); }
uint64_t placedActivity() { return g_placedActivity.load(); }
uint64_t bypassed() { return g_bypassed.load(); }
uint64_t forwarded() { return g_forwarded.load(); }
uint64_t updatesPlaced() { return g_updatesPlaced.load(); }
uint64_t hookCalls() { return g_hookCalls.load(); }
uint32_t faults() { return g_faults.load(); }
uint32_t phase() { return g_phase.load(); }
bool placeActive() { return g_placeActive.load(); }
void preThenPost(void* activity) {
    PreState ps = placementPre(activity);
    placementPost(ps);
}
// Back to a session that has not tried either hook: both CodeHooks are uninstalled (the original bytes return), every latch and
// counter clears. The relay pages are process-lifetime storage by design: the rig leaks them.
void reset() {
    for (HookEntry* entry : {&g_free, &g_collision}) {
        entry->hook.uninstall();
        entry->relay = nullptr;
        entry->target = 0;
        entry->state = ExplorerCamHookStatus::NotTried;
        entry->stolen = 0;
        entry->why[0] = 0;
    }
    g_freeForward.store(0);
    g_relayGate.store(0);
    g_placedActivity.store(0);
    g_bypassed.store(0);
    g_forwarded.store(0);
    g_placeActive.store(false);
    g_probeWants.store(false);
    g_observer.store(nullptr);
    g_resetRequest.store(0);
    g_eyeUp.store(ecm::kEyeUpDefault);
    g_eyeForward.store(ecm::kEyeForwardDefault);
    g_eyeRight.store(ecm::kEyeRightDefault);
    g_busy.store(false);
    g_phase.store(0);
    g_trackedActivity.store(0);
    g_trackedCalls.store(0);
    g_updatesPlaced.store(0);
    g_waitingUpdates.store(0);
    g_hookCalls.store(0);
    g_contended.store(0);
    g_foreign.store(0);
    g_faults.store(0);
    g_lastState.store(0);
    ecm::Event drop;
    while (g_events.take(&drop)) {}
    g_machine = ecm::Machine();
    g_resetSeen = 0;
    g_firstCallNoted = false;
    g_frame = FrameState();
    g_identity = 0;
    g_testFreeTarget = 0;
    g_testCollisionTarget = 0;
}
}  // namespace explorercamtest
#endif

}  // namespace edvr
