// Explorer Cam: the glue (explorer_cam.h says what it is; explorer_cam_core.h holds every decision, every byte of the relays and the
// text of every line, driven by tools\explorer_cam_test).
//
// WHAT RUNS WHERE.
//   free camera   freeCameraHooked, on whichever thread the game's job system calls the free camera's update from:
//                   pre   (preFree)  decide from the activity's flag bytes, write the pose, set the lock's pressed-int
//                   the original update
//                   post  (postFor)  restore the pressed-int, then the observers
//   camera UI     cameraUiHooked: pre presses FreeCamToggleHUD once per placement (and again to give the UI back), post restores.
//   controller    controllerHooked: pre runs F5's sequence (PhotoCameraToggle, ToggleFreeCam, again PhotoCameraToggle to leave), post
//                 restores the press. The mode byte is read on every call.
//   collision     the relay in machine code (explorer_cam_core.h): no C++ between the game and the original.
//   box push      the same relay, for the commander's box push.
//   frame thread  explorerCamFrameBoundary, the Present boundary: the only code that reads Config, the F5 key and the Elite bindings,
//                 and the only code that logs.
// Every hook thread takes no lock (a try-flag keeps a second concurrent caller out of that hook's pre..post), allocates nothing,
// writes no log line and calls nothing of the game's. Every access to the game's memory is under SEH.
//
// THE FLAGS THEY SHARE are atomics. Each hook thread owns its state machine (explorer_cam_core.h); the frame thread asks them to
// start over by bumping g_resetRequest, which each acts on at its next call, and clears the published state (g_placedActivity,
// g_phase, g_sessionActive) itself whenever placement is not active, every frame, so an end never depends on a hook being called
// again.
#include "explorer_cam.h"
#include "explorer_cam_core.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>

#include "../common/code_hook.h"
#include "../common/config.h"
#include "../common/hotkey.h"
#include "../common/log.h"
#include "elite_binds.h"
#include "journal_watch.h"

namespace edvr {
namespace {

// ---- the five hooks ------------------------------------------------------------------------------------------------------------
enum Hk : int { kHkFree = 0, kHkCollision, kHkBox, kHkUi, kHkCtl, kHkCount };
struct HookSpec {
    const char* label;      // the log's name for it
    const char* what;       // what the prologue belongs to
    const char* tag;        // CodeHook's name for it
    uintptr_t rva;
    const uint8_t* prologue;
    size_t prologueBytes;
    bool bypass;            // a bypass relay (return 0 for the placed activity) instead of a gate and a callback
};
const HookSpec kSpec[kHkCount] = {
    {"free-camera", "FreeCameraActivity update", "explorer-cam-free-camera-update", ecm::kFreeCameraRva, ecm::kFreeCameraPrologue,
     ecm::kFreeCameraPrologueBytes, false},
    {"collision", "collision step", "explorer-cam-collision", ecm::kCollisionRva, ecm::kCollisionPrologue, ecm::kCollisionPrologueBytes, true},
    {"box-push", "commander box push", "explorer-cam-box-push", ecm::kBoxPushRva, ecm::kBoxPushPrologue, ecm::kBoxPushPrologueBytes, true},
    {"camera-UI", "VanityCameraUIActivity update", "explorer-cam-camera-ui", ecm::kCameraUiRva, ecm::kCameraUiPrologue,
     ecm::kCameraUiPrologueBytes, false},
    {"controller", "VesselCameraMountControl update", "explorer-cam-controller", ecm::kControllerRva, ecm::kControllerPrologue,
     ecm::kControllerPrologueBytes, false},
};

// ---- state shared between the threads --------------------------------------------------------------------------------------
// The bypass relays read g_placedActivity and bump their counters from machine code, so those are plain 8-byte atomics.
alignas(8) std::atomic<uint64_t> g_placedActivity{0};   // the activity while a pose is being written, else 0
alignas(8) std::atomic<uint64_t> g_bypassed[2];         // [0] collision, [1] box push: calls answered "no edit" by the relay
alignas(8) std::atomic<uint64_t> g_forwarded[2];        // ...and calls passed to the original
alignas(8) std::atomic<uintptr_t> g_gate[kHkCount];     // a callback relay's gate: open = the callback runs, closed = straight on
std::atomic<uintptr_t> g_forward[kHkCount];             // a callback relay's trampoline
std::atomic<ExplorerCamActivityObserver> g_observers[2][kExplorerCamMaxObservers];

std::atomic<bool> g_placeActive{false};                 // on, every required hook armed, not stood down by faults (frame thread)
std::atomic<uint32_t> g_resetRequest{0};
std::atomic<float> g_eyeUp{ecm::kEyeUpDefault}, g_eyeForward{ecm::kEyeForwardDefault}, g_eyeRight{ecm::kEyeRightDefault};

std::atomic<bool> g_busy[3];                            // free camera, camera UI, controller: a call is inside that hook's pre..post
std::atomic<uint32_t> g_phase{0};                       // 0 idle, 1 waiting (tracked, not yet placeable), 2 placed
std::atomic<uint64_t> g_trackedActivity{0};
std::atomic<uint64_t> g_trackedCalls{0};                // calls from the tracked activity
std::atomic<uint64_t> g_updatesPlaced{0};
std::atomic<uint64_t> g_waitingUpdates{0};
std::atomic<uint64_t> g_hookCalls{0};                   // the free-camera hook
std::atomic<uint64_t> g_contended{0};
std::atomic<uint64_t> g_foreign{0};
std::atomic<uint32_t> g_faults{0};
std::atomic<uint32_t> g_lastState{0};

std::atomic<bool> g_sessionActive{false};               // an F5 session is on (the controller's sequencer writes, the frame thread clears)
std::atomic<bool> g_exiting{false};                     // the EXIT half of the sequence is running
std::atomic<uint32_t> g_f5Request{0};                   // ecm::F5Req, set by the frame thread, taken by the controller hook
std::atomic<uint64_t> g_ctlCalls{0};
std::atomic<uint32_t> g_ctlMode{0};
std::atomic<uint64_t> g_uiCalls{0};
std::atomic<bool> g_uiHeld{false};                      // EDVR hid the UI (or a hide is in flight): the sequence waits for it to clear
std::atomic<bool> g_uiHiddenByUs{false};

std::atomic<uint64_t> g_eventSeq{0};
enum Ring : int { kRingFree = 0, kRingUi = 1, kRingCtl = 2 };
ecm::EventRing<64> g_rings[3];

// ---- hook-thread state (touched only while that hook's g_busy is held) ------------------------------------------------------
ecm::Machine g_machine;
uint32_t g_freeResetSeen = 0;
bool g_firstCallNoted = false;
ecm::F5Sequencer g_seq;
uint32_t g_ctlResetSeen = 0;
bool g_ctlFirstNoted = false;
ecm::UiHider g_uiHider;

// ---- frame-thread state ------------------------------------------------------------------------------------------------------
struct FrameState {
    bool announced = false;
    bool lastOn = false;
    bool haveEye = false;
    ecm::Eye lastEye;
    bool skippedNoted = false;
    ecm::StaleWatch stale;
    ecm::StaleWatch ctlStale;
    ecm::LiveWatch ctlLive;
    uint32_t lastPhase = 0;
    bool lastSession = false;
    uint32_t f5SetFrame = 0;
    uint64_t nextBeatMs = 0;
    uint64_t lastBeatMs = 0;
    uint64_t beatUpdates = 0, beatBypassed = 0, beatForwarded = 0, beatBoxBypassed = 0, beatBoxForwarded = 0, beatHookCalls = 0, beatCtlCalls = 0;
    // The hotkey against the player's live Elite bindings.
    std::string checkedHotkey;
    bool clashChecked = false;
    bool unreadNoted = false;
    uint64_t nextBindsMs = 0;
    uint64_t bindsFp = 0, bindsPending = 0;
    bool ctlAnnounced = false;
};
FrameState g_frame;

#ifdef EDVR_EXPLORER_CAM_TEST
uintptr_t g_testTargets[kHkCount] = {};   // the rig's synthetic functions, installed in place of the game's
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
__declspec(noinline) bool sehReadByte(const uint8_t* a, uint32_t offset, uint8_t* out) noexcept {
    __try {
        *out = a[offset];
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
__declspec(noinline) bool sehReadInt(const uint8_t* a, uint32_t offset, int32_t* out) noexcept {
    __try {
        std::memcpy(out, a + offset, 4);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
// The camera UI's two fields: is the hide-UI handle there, and is the UI hidden.
__declspec(noinline) bool sehReadUi(const uint8_t* a, ecm::UiObserved* o) noexcept {
    __try {
        uint64_t handle = 0;
        std::memcpy(&handle, a + ecm::kOffUiHideAction, 8);
        o->handle = handle != 0;
        o->hidden = a[ecm::kOffUiHidden];
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
// A press: read the action object's pointer at object+handleOffset, remember the pressed-int's value, set it to 1.
__declspec(noinline) bool sehSetPressed(const uint8_t* object, uint32_t handleOffset, uint64_t* action, int32_t* previous) noexcept {
    __try {
        uint64_t p = 0;
        std::memcpy(&p, object + handleOffset, 8);
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
__declspec(noinline) bool sehRestorePressed(uint64_t action, int32_t previous) noexcept {
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

// ---- events --------------------------------------------------------------------------------------------------------------------
void publishIdle() {
    g_placedActivity.store(0, std::memory_order_release);
    g_phase.store(0, std::memory_order_release);
    g_trackedActivity.store(0, std::memory_order_relaxed);
}

void pushEvent(Ring ring, ecm::EvKind kind, uint64_t object, uint32_t why, uint32_t before, uint32_t after, uint32_t flags,
               const ecm::Eye* eye, uint32_t count) {
    ecm::Event e;
    e.seq = g_eventSeq.fetch_add(1, std::memory_order_relaxed);
    e.kind = static_cast<uint32_t>(kind);
    e.activity = object;
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
    g_rings[ring].push(e);
}

// A guarded access faulted: count it, say where, end what that hook was doing. The 8th fault ends the feature for the session.
void onFault(Ring ring, ecm::FaultSite site, uint64_t object) {
    const uint32_t n = g_faults.fetch_add(1, std::memory_order_relaxed) + 1;
    pushEvent(ring, ecm::EvKind::Fault, object, static_cast<uint32_t>(site), 0, 0, 0, nullptr, n);
    if (ring == kRingFree) {
        g_machine.abort(ecm::Why::Fault);
        publishIdle();
    } else if (ring == kRingCtl) {
        g_seq.reset();
        g_sessionActive.store(false, std::memory_order_release);
        g_exiting.store(false, std::memory_order_release);
    }
    if (n == ecm::kMaxFaults) pushEvent(ring, ecm::EvKind::FaultLimit, object, 0, 0, 0, 0, nullptr, n);
}

// What a hook's pre half hands to its post half.
struct PreState {
    int hook = -1;          // 0 free camera, 1 camera UI, 2 controller
    bool holdsBusy = false;
    bool pressed = false;
    uint64_t object = 0;
    uint64_t action = 0;
    int32_t previous = 0;
};
constexpr int kHookFree = 0, kHookUi = 1, kHookCtl = 2;

bool enter(PreState& ps, int hook, void* object) {
    ps.hook = hook;
    if (!object) return false;
    if (g_busy[hook].exchange(true, std::memory_order_acquire)) {
        g_contended.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    ps.holdsBusy = true;
    ps.object = reinterpret_cast<uint64_t>(object);
    return true;
}

void postFor(PreState& ps) noexcept {
    if (!ps.holdsBusy) return;
    if (ps.pressed && !sehRestorePressed(ps.action, ps.previous)) {
        switch (ps.hook) {
            case kHookFree: onFault(kRingFree, ecm::FaultSite::RestoreLock, ps.object); break;
            case kHookUi: onFault(kRingUi, ecm::FaultSite::RestoreUiPress, ps.object); break;
            default: onFault(kRingCtl, ecm::FaultSite::RestoreControllerPress, ps.object); break;
        }
    }
    ps.holdsBusy = false;
    g_busy[ps.hook].store(false, std::memory_order_release);
}

void runObservers(int which, void* object) noexcept {
    for (int i = 0; i < kExplorerCamMaxObservers; ++i) {
        const auto fn = g_observers[which][i].load(std::memory_order_acquire);
        if (fn) fn(object);
    }
}

// ---- the free-camera hook: pre, then the original, then post ----------------------------------------------------------------
void runFreePre(void* activityPtr, PreState& ps) {
    const uint32_t request = g_resetRequest.load(std::memory_order_acquire);
    if (request != g_freeResetSeen) {
        g_freeResetSeen = request;
        g_machine.reset();
    }
    if (!g_placeActive.load(std::memory_order_acquire) || g_faults.load(std::memory_order_relaxed) >= ecm::kMaxFaults) return;

    const uint64_t activity = reinterpret_cast<uint64_t>(activityPtr);
    const uint8_t* const bytes = static_cast<const uint8_t*>(activityPtr);
    ecm::Observed o;
    if (!sehReadObserved(bytes, &o)) {
        onFault(kRingFree, ecm::FaultSite::ReadActivity, activity);
        return;
    }
    g_lastState.store(o.state, std::memory_order_relaxed);
    if (!g_firstCallNoted) {
        g_firstCallNoted = true;
        pushEvent(kRingFree, ecm::EvKind::FirstCall, activity, 0, 0, o.state, ecm::packObserved(o), nullptr, 0);
    }

    // Only an F5 session places: the game's own camera key and TAB bring the activity up, and nothing here touches it.
    const ecm::Step s = g_machine.step(activity, o, g_sessionActive.load(std::memory_order_acquire));
    if (s.foreign) {
        g_foreign.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (s.pressResult) pushEvent(kRingFree, ecm::EvKind::LockResult, activity, 0, s.pressBefore, s.pressAfter, ecm::packObserved(o), nullptr, 0);
    if (s.released) {
        publishIdle();
        pushEvent(kRingFree, ecm::EvKind::Released, activity, static_cast<uint32_t>(s.why), 0, o.state, ecm::packObserved(o), nullptr, 0);
        if (!s.entered) return;
    }
    if (s.entered) {
        if (!writableRange(bytes, ecm::kActivityBytes)) {
            onFault(kRingFree, ecm::FaultSite::Validate, activity);
            return;
        }
        g_trackedActivity.store(activity, std::memory_order_relaxed);
        g_phase.store(1, std::memory_order_release);
        pushEvent(kRingFree, ecm::EvKind::Entered, activity, 0, 0, o.state, ecm::packObserved(o), nullptr, 0);
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
        onFault(kRingFree, ecm::FaultSite::ReadPose, activity);
        return;
    }
    ecm::buildLocalPose(current, eye, out);
    // Published BEFORE the write and the original run: this update's two position edits must already see it.
    if (s.placeNow) g_placedActivity.store(activity, std::memory_order_release);
    if (!sehWritePose(const_cast<uint8_t*>(bytes), out)) {
        onFault(kRingFree, ecm::FaultSite::WritePose, activity);
        return;
    }
    g_updatesPlaced.fetch_add(1, std::memory_order_relaxed);
    if (s.placeNow) {
        g_phase.store(2, std::memory_order_release);
        pushEvent(kRingFree, ecm::EvKind::Placed, activity, s.alreadyLocked ? 1u : 0u, 0, o.state, ecm::packObserved(o), &eye, 0);
    }
    if (s.press) {
        if (!sehSetPressed(bytes, ecm::kOffLockAction, &ps.action, &ps.previous)) {
            onFault(kRingFree, ecm::FaultSite::SetLock, activity);
            return;
        }
        ps.pressed = true;
    }
}

__declspec(noinline) PreState preFree(void* activity) noexcept {
    PreState ps;
    g_hookCalls.fetch_add(1, std::memory_order_relaxed);
    if (!enter(ps, kHookFree, activity)) return ps;
    runFreePre(activity, ps);
    return ps;
}

// Ghidra shows one parameter (rcx = the object) for all three callback hooks, a return value in rax and no float-register parameter
// read, so four integer registers are forwarded and nothing else is declared: declaring a float parameter would read an xmm register
// the caller never set.
using ForwardFn = uint64_t (__fastcall*)(void*, void*, void*, void*);

__declspec(noinline) uint64_t __fastcall freeCameraHooked(void* a, void* b, void* c, void* d) noexcept {
    const auto forward = reinterpret_cast<ForwardFn>(g_forward[kHkFree].load(std::memory_order_acquire));
    if (!forward) return 0;   // unreachable: the relay exists only after the install published a trampoline
    PreState ps = preFree(a);
    const uint64_t result = forward(a, b, c, d);
    postFor(ps);
    runObservers(0, a);
    return result;
}

// ---- the camera UI hook ---------------------------------------------------------------------------------------------------------
void runUiPre(void* objectPtr, PreState& ps) {
    const bool active = g_placeActive.load(std::memory_order_acquire) && g_faults.load(std::memory_order_relaxed) < ecm::kMaxFaults;
    // Nothing to do, and nothing of the game's touched, unless EDVR holds the UI or a hide is in flight: those are given back / read
    // even after Explorer Cam has gone off, because a UI that EDVR hid and nobody gives back stays hidden.
    if (!active && !g_uiHider.hiddenByUs() && !g_uiHider.pending()) return;
    const uint64_t object = reinterpret_cast<uint64_t>(objectPtr);
    const uint8_t* const bytes = static_cast<const uint8_t*>(objectPtr);
    ecm::UiObserved o;
    if (!sehReadUi(bytes, &o)) {
        onFault(kRingUi, ecm::FaultSite::ReadUi, object);
        return;
    }
    // Hidden while a placement stands and the session is not leaving; given back the moment it is not so.
    const bool wantHidden = active && g_sessionActive.load(std::memory_order_acquire) &&
                            g_placedActivity.load(std::memory_order_acquire) != 0 && !g_exiting.load(std::memory_order_acquire);
    const ecm::UiStep s = g_uiHider.step(object, o, wantHidden);
    g_uiHiddenByUs.store(g_uiHider.hiddenByUs(), std::memory_order_release);
    g_uiHeld.store(g_uiHider.hiddenByUs() || g_uiHider.pending(), std::memory_order_release);
    if (s.ev != ecm::UiEvent::None) pushEvent(kRingUi, ecm::EvKind::Ui, object, static_cast<uint32_t>(s.ev), 0, s.hidden, 0, nullptr, 0);
    if (s.press) {
        if (!sehSetPressed(bytes, ecm::kOffUiHideAction, &ps.action, &ps.previous)) {
            onFault(kRingUi, ecm::FaultSite::SetUiPress, object);
            return;
        }
        ps.pressed = true;
    }
}

__declspec(noinline) PreState preUi(void* object) noexcept {
    PreState ps;
    g_uiCalls.fetch_add(1, std::memory_order_relaxed);
    if (!enter(ps, kHookUi, object)) return ps;
    runUiPre(object, ps);
    return ps;
}

__declspec(noinline) uint64_t __fastcall cameraUiHooked(void* a, void* b, void* c, void* d) noexcept {
    const auto forward = reinterpret_cast<ForwardFn>(g_forward[kHkUi].load(std::memory_order_acquire));
    if (!forward) return 0;
    PreState ps = preUi(a);
    const uint64_t result = forward(a, b, c, d);
    postFor(ps);
    return result;
}

// ---- the controller hook --------------------------------------------------------------------------------------------------------
void runCtlPre(void* controllerPtr, PreState& ps) {
    const uint32_t request = g_resetRequest.load(std::memory_order_acquire);
    if (request != g_ctlResetSeen) {
        g_ctlResetSeen = request;
        g_seq.reset();
    }
    if (!g_placeActive.load(std::memory_order_acquire) || g_faults.load(std::memory_order_relaxed) >= ecm::kMaxFaults) return;
    const uint64_t controller = reinterpret_cast<uint64_t>(controllerPtr);
    const uint8_t* const bytes = static_cast<const uint8_t*>(controllerPtr);
    uint8_t mode = 0;
    if (!sehReadByte(bytes, ecm::kOffCtlMode, &mode)) {
        onFault(kRingCtl, ecm::FaultSite::ReadController, controller);
        return;
    }
    g_ctlMode.store(mode, std::memory_order_relaxed);
    if (!g_ctlFirstNoted) {
        g_ctlFirstNoted = true;
        pushEvent(kRingCtl, ecm::EvKind::ControllerFirstCall, controller, 0, 0, mode, 0, nullptr, 0);
    }
    const ecm::F5Req req = static_cast<ecm::F5Req>(g_f5Request.exchange(0, std::memory_order_acq_rel));
    const ecm::SeqStep s = g_seq.step(mode, req, g_uiHeld.load(std::memory_order_acquire));
    g_sessionActive.store(s.sessionActive, std::memory_order_release);
    g_exiting.store(s.exiting, std::memory_order_release);
    if (s.nev) {
        int32_t kind = 0;
        sehReadInt(bytes, ecm::kOffCtlPresetKind, &kind);   // for the line only; a failed read leaves 0
        for (uint8_t i = 0; i < s.nev; ++i)
            pushEvent(kRingCtl, ecm::EvKind::Seq, controller, static_cast<uint32_t>(s.ev[i]), 0, mode, 0, nullptr, static_cast<uint32_t>(kind));
    }
    if (s.press != ecm::CtlPress::None) {
        const uint32_t handle = s.press == ecm::CtlPress::Photo ? ecm::kOffCtlPhotoAction : ecm::kOffCtlFreeAction;
        if (!sehSetPressed(bytes, handle, &ps.action, &ps.previous)) {
            onFault(kRingCtl, ecm::FaultSite::SetControllerPress, controller);
            return;
        }
        ps.pressed = true;
    }
}

__declspec(noinline) PreState preCtl(void* controller) noexcept {
    PreState ps;
    g_ctlCalls.fetch_add(1, std::memory_order_relaxed);
    if (!enter(ps, kHookCtl, controller)) return ps;
    runCtlPre(controller, ps);
    return ps;
}

__declspec(noinline) uint64_t __fastcall controllerHooked(void* a, void* b, void* c, void* d) noexcept {
    const auto forward = reinterpret_cast<ForwardFn>(g_forward[kHkCtl].load(std::memory_order_acquire));
    if (!forward) return 0;
    PreState ps = preCtl(a);
    const uint64_t result = forward(a, b, c, d);
    postFor(ps);
    runObservers(1, a);
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
    int id = 0;
    int state = ExplorerCamHookStatus::NotTried;
    size_t stolen = 0;
    char why[400] = {};
};
HookEntry g_hooks[kHkCount];

// Seal a relay page and publish the trampoline into its literal slot before CodeHook makes the entry patch live.
bool prepareRelay(void* trampoline, void* context) noexcept {
    auto& entry = *static_cast<HookEntry*>(context);
    const HookSpec& spec = kSpec[entry.id];
    const size_t literalAt = spec.bypass ? ecm::kBypassRelayTrampolineAt : ecm::kCallbackRelayTrampolineAt;
    const size_t relayBytes = spec.bypass ? ecm::kBypassRelayBytes : ecm::kCallbackRelayBytes;
    const uintptr_t address = reinterpret_cast<uintptr_t>(trampoline);
    std::memcpy(entry.relay + literalAt, &address, 8);
    DWORD oldProtect = 0;
    if (!VirtualProtect(entry.relay, 4096, PAGE_EXECUTE_READ, &oldProtect) ||
        !FlushInstructionCache(GetCurrentProcess(), entry.relay, relayBytes))
        return false;
    if (!spec.bypass) g_forward[entry.id].store(address, std::memory_order_release);
    return true;
}

// The PE identity is checked once per session; every hook needs it.
int g_identity = 0;   // 0 not checked, 1 ok, 2 differs
char g_identityWhy[200] = {};

// Resolve the target, verify the build and the prologue. On failure `why` says why in one sentence and nothing was patched.
bool resolveAndVerify(int id, uintptr_t* target, char* why, size_t whyCap) {
    const HookSpec& spec = kSpec[id];
    uintptr_t t = 0;
#ifdef EDVR_EXPLORER_CAM_TEST
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
                      "the game build differs (the %zu bytes at EliteDangerous64.exe+0x%llX are not build 332841's %s prologue); nothing was patched",
                      spec.prologueBytes, static_cast<unsigned long long>(spec.rva), spec.what);
        return false;
    }
    *target = t;
    return true;
}

const void* callbackFor(int id) {
    switch (id) {
        case kHkFree: return reinterpret_cast<const void*>(&freeCameraHooked);
        case kHkUi: return reinterpret_cast<const void*>(&cameraUiHooked);
        case kHkCtl: return reinterpret_cast<const void*>(&controllerHooked);
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
    if (spec.bypass) ecm::buildBypassRelay(entry.relay, &g_placedActivity, &g_bypassed[id == kHkBox ? 1 : 0], &g_forwarded[id == kHkBox ? 1 : 0]);
    else ecm::buildCallbackRelay(entry.relay, &g_gate[id], callbackFor(id));
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

void say(const ecm::Sink& sink, const char* fmt, ...) {
    char line[ecm::kLineBytes];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    sink(line);
}
void logSink(void*, const char* line) { Log::get().note("%s", line); }

const char* armedRole(int id) {
    switch (id) {
        case kHkFree:
            return "Explorer Cam and advanced.explorer_cam_probe share this hook: the placement runs before the original, the lock press is "
                   "restored after it, then the observers";
        case kHkCollision:
            return "it answers 0 (no collision edit) for the placed activity only and hands every other call, all five arguments intact, to the "
                   "original";
        case kHkBox:
            return "it answers 0 (no box push: the point is left alone) for the placed activity only and hands every other call to the "
                   "original";
        case kHkUi:
            return "FreeCamToggleHUD is pressed for one update when a placement begins, and again to give the UI back; the original runs "
                   "between, and the press is restored after it";
        default:
            return "F5's presses (PhotoCameraToggle, ToggleFreeCam) are set for one update before the original and restored after it; the "
                   "mode byte is read on every call";
    }
}
const char* downConsequence(int id) {
    switch (id) {
        case kHkFree: return "Explorer Cam placement and advanced.explorer_cam_probe's I3 do not run.";
        case kHkCollision: return "Explorer Cam placement does not run (without it the free camera would stop 0.70 m from the face).";
        case kHkBox: return "Explorer Cam placement does not run (without it the free camera would be lifted onto the helmet).";
        case kHkCtl: return "Explorer Cam does not run: F5 has nothing to press.";
        default: return "the camera UI stays up while placed; everything else runs.";
    }
}

void tryHook(int id, const ecm::Sink& sink) {
    const HookSpec& spec = kSpec[id];
    HookEntry& entry = g_hooks[id];
    char why[400] = {};
    if (installHook(id, why, sizeof(why))) {
        entry.state = ExplorerCamHookStatus::Armed;
        say(sink, "%s %s hook armed: EliteDangerous64.exe+0x%llX (%s, build 332841) at 0x%llX, stolen=%zu bytes, prologue %zu/%zu bytes verified, "
                  "relay at 0x%llX; %s",
            ecm::prefix(), spec.label, static_cast<unsigned long long>(spec.rva), spec.what, static_cast<unsigned long long>(entry.target),
            entry.stolen, spec.prologueBytes, spec.prologueBytes, static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(entry.relay)),
            armedRole(id));
    } else {
        entry.state = ExplorerCamHookStatus::StoodDown;
        std::snprintf(entry.why, sizeof(entry.why), "%s", why);
        say(sink, "%s %s hook stood down: %s. %s", ecm::prefix(), spec.label, why, downConsequence(id));
    }
}

bool armed(int id) { return g_hooks[id].state == ExplorerCamHookStatus::Armed; }

int observerCount(int which) {
    int n = 0;
    for (int i = 0; i < kExplorerCamMaxObservers; ++i) n += g_observers[which][i].load(std::memory_order_relaxed) != nullptr ? 1 : 0;
    return n;
}

void updateGates() {
    const bool active = g_placeActive.load(std::memory_order_relaxed);
    g_gate[kHkFree].store(armed(kHkFree) && (active || observerCount(0) > 0) ? 1u : 0u, std::memory_order_release);
    g_gate[kHkCtl].store(armed(kHkCtl) && (active || observerCount(1) > 0) ? 1u : 0u, std::memory_order_release);
    g_gate[kHkUi].store(armed(kHkUi) && (active || g_uiHeld.load(std::memory_order_relaxed)) ? 1u : 0u, std::memory_order_release);
}

// ---- the frame thread ----------------------------------------------------------------------------------------------------------
// Everything the frame thread reads from the world in one frame, so the rig can script it. The production wrapper fills it from
// Config, the Hotkey, the journal and the Elite bindings; the rig fills it by hand.
struct FrameInput {
    bool on = true;
    float up = ecm::kEyeUpDefault, forward = ecm::kEyeForwardDefault, right = ecm::kEyeRightDefault;
    const char* hotkey = "";
    bool f5Pressed = false;
    bool gameplay = true;
    bool onFootKnown = false, onFoot = false;
    bool readBindings = true;
    const wchar_t* bindsDir = nullptr;   // null: the live Elite bindings directory
};

uint64_t fingerprintOf(const wchar_t* dir) { return dir ? eliteBindsFingerprintDir(dir) : eliteBindsFingerprint(); }

// hotkey.explorer_cam against the player's own Elite bindings: a key the game also acts on is named. At launch, when the key
// changes, and when Elite rewrites its bindings (a change that holds across two checks, as device_hook's does).
void checkHotkeyClash(FrameState& fs, const FrameInput& in, uint64_t nowMs, const ecm::Sink& sink) {
    const std::string name = in.hotkey ? in.hotkey : "";
    bool recheck = false;
    if (name != fs.checkedHotkey) {
        fs.checkedHotkey = name;
        fs.clashChecked = false;
        fs.unreadNoted = false;
    }
    if (name.empty()) return;
    if (!in.readBindings) {
        if (!fs.unreadNoted) {
            fs.unreadNoted = true;
            say(sink, "%s hotkey %s is not checked against your Elite bindings (hotkey.read_game_bindings is off): if the game also acts on it, "
                      "rebind one of them",
                ecm::prefix(), name.c_str());
        }
        return;
    }
    if (!fs.clashChecked) {
        recheck = true;
        fs.bindsFp = fingerprintOf(in.bindsDir);
        fs.bindsPending = 0;
        fs.nextBindsMs = nowMs + 2000;
    } else if (nowMs >= fs.nextBindsMs) {
        fs.nextBindsMs = nowMs + 2000;
        const uint64_t fp = fingerprintOf(in.bindsDir);
        if (fp == fs.bindsFp) {
            fs.bindsPending = 0;
        } else if (fp == fs.bindsPending) {
            fs.bindsFp = fp;
            fs.bindsPending = 0;
            recheck = true;
        } else {
            fs.bindsPending = fp;
        }
    }
    if (!recheck) return;
    fs.clashChecked = true;

    uint32_t mods = 0;
    const int vk = virtualKeyFromName(name.c_str(), &mods);
    if (!vk) return;   // the Hotkey's own line says the name is not a key EDVR knows
    static EliteKeyboardUse uses[512];
    char file[64] = {};
    const int n = in.bindsDir ? eliteBindsKeyboardUsesDir(in.bindsDir, uses, 512, file, sizeof(file))
                              : eliteBindsKeyboardUses(uses, 512, file, sizeof(file));
    if (n < 0) {
        say(sink, "%s hotkey %s could not be checked against your Elite bindings (no preset or no readable .binds file under Options\\Bindings)",
            ecm::prefix(), name.c_str());
        return;
    }
    std::string clashes;
    int clashCount = 0;
    for (int i = 0; i < n; ++i) {
        uint32_t m2 = 0;
        const int vk2 = virtualKeyFromName(uses[i].binding, &m2);
        // The same key with either side's modifiers held inside the other's fires both: a bare F5 binding also fires on CTRL+F5.
        if (vk2 != vk || ((m2 & ~mods) != 0 && (mods & ~m2) != 0)) continue;
        if (clashCount++ < 4) {
            if (!clashes.empty()) clashes += ", ";
            clashes += uses[i].element;
            clashes += " (";
            clashes += uses[i].binding;
            clashes += ")";
        }
    }
    if (clashCount) {
        say(sink, "%s hotkey %s CLASHES with your Elite bindings in %s: %s%s. The game acts on that key too, so one press does both; rebind one "
                  "of them (hotkey.explorer_cam here, or the action in Elite)",
            ecm::prefix(), name.c_str(), file, clashes.c_str(), clashCount > 4 ? ", ..." : "");
    } else {
        say(sink, "%s hotkey %s is free in your Elite bindings (%s: %d keyboard bindings checked)", ecm::prefix(), name.c_str(), file, n);
    }
}

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

void endSessionFromFrame(const char* why, const ecm::Sink& sink) {
    g_sessionActive.store(false, std::memory_order_release);
    g_exiting.store(false, std::memory_order_release);
    g_f5Request.store(0, std::memory_order_release);
    g_resetRequest.fetch_add(1, std::memory_order_release);
    say(sink, "%s the Explorer Cam session ended: %s", ecm::prefix(), why);
}

void boundaryAt(uint32_t frame, uint64_t nowMs, const FrameInput& in, const ecm::Sink& sink) {
    FrameState& fs = g_frame;
    char line[ecm::kLineBytes];
    const bool on = in.on;

    // 1. The settings, clamped and published. The eye is live: the next update writes the new one.
    const ecm::Eye eye = ecm::clampEye(in.up, in.forward, in.right);
    g_eyeUp.store(eye.up, std::memory_order_relaxed);
    g_eyeForward.store(eye.forward, std::memory_order_relaxed);
    g_eyeRight.store(eye.right, std::memory_order_relaxed);
    if (!fs.announced) {
        fs.announced = true;
        fs.lastOn = on;
        fs.lastEye = eye;
        fs.haveEye = true;
        if (on)
            say(sink, "%s on (fix.explorer_cam = on): press %s on foot or in the camera to put the view at your commander's head and lock it "
                      "to them (press it again to leave); eye up=%.3f forward=%.3f right=%.3f m in the commander's frame from their feet "
                      "(fix.explorer_cam_eye_up, _eye_forward, _eye_right; live). The game's own camera key and TAB place nothing.",
                ecm::prefix(), in.hotkey && in.hotkey[0] ? in.hotkey : "(hotkey.explorer_cam is empty: no way in)", eye.up, eye.forward,
                eye.right);
        else
            say(sink, "%s off (fix.explorer_cam = off): nothing is installed for it and nothing is written to the game", ecm::prefix());
    } else {
        if (on != fs.lastOn) {
            fs.lastOn = on;
            if (on)
                say(sink, "%s on again (fix.explorer_cam = on): eye up=%.3f forward=%.3f right=%.3f", ecm::prefix(), eye.up, eye.forward, eye.right);
            else
                say(sink, "%s off (fix.explorer_cam turned off while running): the session, if any, is ended and nothing more is written",
                    ecm::prefix());
        }
        if (!ecm::sameEye(eye, fs.lastEye)) {
            say(sink, "%s eye changed: up=%.3f forward=%.3f right=%.3f (was up=%.3f forward=%.3f right=%.3f)", ecm::prefix(), eye.up, eye.forward,
                eye.right, fs.lastEye.up, fs.lastEye.forward, fs.lastEye.right);
            fs.lastEye = eye;
        }
    }

    // 2. The hooks, the first time they are wanted. The free camera first; the rest only once it is armed, and each required one only
    // once the one before it is, so a stand-down leaves nothing half installed that did not have to be.
    if (on && g_hooks[kHkFree].state == ExplorerCamHookStatus::NotTried) tryHook(kHkFree, sink);
    if (on) {
        static const int kOrder[] = {kHkCollision, kHkBox, kHkCtl};
        bool allArmed = armed(kHkFree);
        if (!allArmed && g_hooks[kHkFree].state == ExplorerCamHookStatus::StoodDown && !fs.skippedNoted) {
            fs.skippedNoted = true;
            say(sink, "%s the collision, box-push, camera-UI and controller hooks are not installed: the free-camera hook stood down, so "
                      "Explorer Cam does not run.",
                ecm::prefix());
        }
        for (int id : kOrder) {
            if (!allArmed) break;
            if (g_hooks[id].state == ExplorerCamHookStatus::NotTried) tryHook(id, sink);
            allArmed = armed(id);
            if (!allArmed && !fs.skippedNoted) {
                fs.skippedNoted = true;
                say(sink, "%s the remaining hooks are not installed: the %s hook stood down, so Explorer Cam does not run.", ecm::prefix(),
                    kSpec[id].label);
            }
        }
        if (allArmed && g_hooks[kHkUi].state == ExplorerCamHookStatus::NotTried) tryHook(kHkUi, sink);
    }

    // 3. Active = on, every required hook armed, and the fault budget not spent. A change starts the hook threads' machines over.
    const bool active = on && armed(kHkFree) && armed(kHkCollision) && armed(kHkBox) && armed(kHkCtl) &&
                        g_faults.load(std::memory_order_relaxed) < ecm::kMaxFaults;
    if (active != g_placeActive.load(std::memory_order_relaxed)) {
        g_placeActive.store(active, std::memory_order_release);
        g_resetRequest.fetch_add(1, std::memory_order_release);
    }
    updateGates();

    // 4. The hotkey against the player's Elite bindings.
    if (on) checkHotkeyClash(fs, in, nowMs, sink);

    // 5. F5.
    fs.ctlLive.tick(g_ctlCalls.load(std::memory_order_relaxed));
    {
        ecm::F5Inputs fi;
        fi.pressed = in.f5Pressed;
        fi.gameplay = in.gameplay;
        fi.active = active;
        fi.sessionActive = g_sessionActive.load(std::memory_order_acquire);
        fi.controllerAlive = fs.ctlLive.alive();
        fi.mode = static_cast<uint8_t>(g_ctlMode.load(std::memory_order_relaxed));
        fi.onFootKnown = in.onFootKnown;
        fi.onFoot = in.onFoot;
        const ecm::F5Action action = ecm::decideF5(fi);
        if (action != ecm::F5Action::None) {
            ecm::formatF5(line, sizeof(line), action, fi.mode, in.onFootKnown, in.onFoot);
            sink(line);
            if (action == ecm::F5Action::Enter || action == ecm::F5Action::Exit) {
                g_f5Request.store(static_cast<uint32_t>(action == ecm::F5Action::Enter ? ecm::F5Req::Enter : ecm::F5Req::Exit),
                                  std::memory_order_release);
                fs.f5SetFrame = frame;
            }
        } else if (g_f5Request.load(std::memory_order_acquire) != 0 && frame - fs.f5SetFrame >= ecm::StaleWatch::kStaleFrames) {
            // Asked, and the controller never took it: the controller is not running, and the player is told so.
            g_f5Request.store(0, std::memory_order_release);
            say(sink, "%s the F5 request was dropped: the camera controller was not called within %u frames (controller calls so far: %llu)",
                ecm::prefix(), ecm::StaleWatch::kStaleFrames, static_cast<unsigned long long>(g_ctlCalls.load(std::memory_order_relaxed)));
        }
    }

    // 6. What the hook threads said, in the order they said it.
    {
        ecm::Event events[3 * 64];
        size_t count = 0;
        for (auto& ring : g_rings)
            while (count < sizeof(events) / sizeof(events[0]) && ring.take(&events[count])) ++count;
        std::sort(events, events + count, [](const ecm::Event& a, const ecm::Event& b) { return a.seq < b.seq; });
        for (size_t i = 0; i < count; ++i) {
            ecm::formatEvent(line, sizeof(line), events[i], frame);
            sink(line);
        }
    }

    // 7. Not active: whatever the hook threads left published is withdrawn, every frame, until it is gone.
    uint32_t phase = g_phase.load(std::memory_order_acquire);
    if (!active) {
        if (phase != 0 || g_placedActivity.load(std::memory_order_acquire) != 0) {
            if (phase != 0) releaseFromFrame(ecm::Why::KeyOff, frame, sink);
            else publishIdle();
        }
        if (g_sessionActive.load(std::memory_order_acquire) || g_exiting.load(std::memory_order_acquire))
            endSessionFromFrame("Explorer Cam is off or stood down", sink);
        fs.stale.tick(false, 0);
        fs.ctlStale.tick(false, 0);
        fs.lastPhase = 0;
        fs.lastSession = false;
        return;
    }

    // 8. A session whose camera controller went silent ends; a placement whose activity went silent is released; a placement
    // whose session ended is released at once (the camera may have closed without the activity being called again).
    const bool session = g_sessionActive.load(std::memory_order_acquire);
    if (fs.ctlStale.tick(session, g_ctlCalls.load(std::memory_order_relaxed)))
        endSessionFromFrame("the camera controller was not called for 30 frames", sink);
    phase = g_phase.load(std::memory_order_acquire);
    if (phase != 0 && !g_sessionActive.load(std::memory_order_acquire)) {
        releaseFromFrame(ecm::Why::KeyOff, frame, sink);
        phase = 0;
    }
    if (fs.stale.tick(phase != 0, g_trackedCalls.load(std::memory_order_relaxed))) {
        releaseFromFrame(ecm::Why::Stale, frame, sink);
        phase = 0;
    }

    // 9. The 5 s heartbeat while a session is on.
    const bool sessionNow = g_sessionActive.load(std::memory_order_acquire);
    if (phase == 0 && !sessionNow) {
        fs.lastPhase = 0;
        fs.lastSession = false;
        return;
    }
    if (fs.lastPhase == 0 && !fs.lastSession) {
        fs.nextBeatMs = nowMs + 5000;
        fs.lastBeatMs = nowMs;
        fs.beatUpdates = g_updatesPlaced.load(std::memory_order_relaxed);
        fs.beatBypassed = g_bypassed[0].load(std::memory_order_relaxed);
        fs.beatForwarded = g_forwarded[0].load(std::memory_order_relaxed);
        fs.beatBoxBypassed = g_bypassed[1].load(std::memory_order_relaxed);
        fs.beatBoxForwarded = g_forwarded[1].load(std::memory_order_relaxed);
        fs.beatHookCalls = g_hookCalls.load(std::memory_order_relaxed);
        fs.beatCtlCalls = g_ctlCalls.load(std::memory_order_relaxed);
    }
    fs.lastPhase = phase;
    fs.lastSession = sessionNow;
    if (nowMs >= fs.nextBeatMs) {
        ecm::HeartbeatIn h;
        h.windowSeconds = static_cast<double>(nowMs - fs.lastBeatMs) / 1000.0;
        h.phase = phase == 2 ? "placed" : phase == 1 ? "waiting" : "session";
        h.session = sessionNow;
        h.activity = g_trackedActivity.load(std::memory_order_relaxed);
        h.state = g_lastState.load(std::memory_order_relaxed);
        h.updates = g_updatesPlaced.load(std::memory_order_relaxed);
        h.bypassed = g_bypassed[0].load(std::memory_order_relaxed);
        h.forwarded = g_forwarded[0].load(std::memory_order_relaxed);
        h.boxBypassed = g_bypassed[1].load(std::memory_order_relaxed);
        h.boxForwarded = g_forwarded[1].load(std::memory_order_relaxed);
        h.hookCalls = g_hookCalls.load(std::memory_order_relaxed);
        h.ctlCalls = g_ctlCalls.load(std::memory_order_relaxed);
        h.ctlMode = g_ctlMode.load(std::memory_order_relaxed);
        h.uiHiddenByUs = g_uiHiddenByUs.load(std::memory_order_relaxed);
        h.uiCalls = g_uiCalls.load(std::memory_order_relaxed);
        h.updatesWindow = h.updates - fs.beatUpdates;
        h.bypassedWindow = h.bypassed - fs.beatBypassed;
        h.forwardedWindow = h.forwarded - fs.beatForwarded;
        h.boxBypassedWindow = h.boxBypassed - fs.beatBoxBypassed;
        h.boxForwardedWindow = h.boxForwarded - fs.beatBoxForwarded;
        h.hookCallsWindow = h.hookCalls - fs.beatHookCalls;
        h.ctlCallsWindow = h.ctlCalls - fs.beatCtlCalls;
        h.waiting = g_waitingUpdates.load(std::memory_order_relaxed);
        h.contended = g_contended.load(std::memory_order_relaxed);
        h.foreign = g_foreign.load(std::memory_order_relaxed);
        h.lost = g_rings[0].lost() + g_rings[1].lost() + g_rings[2].lost();
        h.faults = g_faults.load(std::memory_order_relaxed);
        h.eye = eye;
        ecm::formatHeartbeat(line, sizeof(line), h);
        sink(line);
        fs.beatUpdates = h.updates;
        fs.beatBypassed = h.bypassed;
        fs.beatForwarded = h.forwarded;
        fs.beatBoxBypassed = h.boxBypassed;
        fs.beatBoxForwarded = h.boxForwarded;
        fs.beatHookCalls = h.hookCalls;
        fs.beatCtlCalls = h.ctlCalls;
        fs.lastBeatMs = nowMs;
        fs.nextBeatMs = nowMs + 5000;
    }
}

// ---- the F5 key (frame thread) ---------------------------------------------------------------------------------------------------
// The hotkey.h pattern: polled with GetAsyncKeyState, EDVR's own key, so it keeps the foreground rule (a press typed in a browser is
// ignored, and the game takes it as the game's) and is never captured (the game sees the press too; the clash check names any
// action of the player's that shares the key).
Hotkey g_f5;
std::string g_f5Name;
bool g_f5Configured = false;
int g_f5Missed = 0;

void applyHotkey(const std::string& name, const ecm::Sink& sink) {
    if (g_f5Configured && name == g_f5Name) return;
    g_f5Configured = true;
    g_f5Name = name;
    g_f5.setBinding(name.c_str());
    if (name.empty())
        say(sink, "%s hotkey.explorer_cam is empty: there is no way into Explorer Cam this session", ecm::prefix());
    else if (g_f5.key() == 0)
        say(sink, "%s hotkey.explorer_cam = \"%s\" bound nothing (the line above says why), so there is no way into Explorer Cam this session",
            ecm::prefix(), name.c_str());
    else
        say(sink, "%s hotkey bound: %s (vk 0x%02X, mods 0x%X); watched, never captured, and only while the game has the focus and the journal "
                  "says you are playing",
            ecm::prefix(), name.c_str(), g_f5.key(), g_f5.mods());
}

}  // namespace

void explorerCamFrameBoundary(uint32_t frameNo) {
    const ecm::Sink sink{&logSink, nullptr};
    const Config& cfg = Config::get();
    FrameInput in;
    in.on = cfg.getBool("fix.explorer_cam", true);
    in.up = cfg.getFloat("fix.explorer_cam_eye_up", ecm::kEyeUpDefault);
    in.forward = cfg.getFloat("fix.explorer_cam_eye_forward", ecm::kEyeForwardDefault);
    in.right = cfg.getFloat("fix.explorer_cam_eye_right", ecm::kEyeRightDefault);
    static std::string hotkeyName;
    hotkeyName = cfg.getString("hotkey.explorer_cam", "F5");
    in.hotkey = hotkeyName.c_str();
    applyHotkey(hotkeyName, sink);
    in.f5Pressed = g_f5.pressed();
    if (g_f5.takeMissedWhileUnfocused() && g_f5Missed < 3) {
        ++g_f5Missed;
        say(sink, "%s the Explorer Cam key was pressed, but another window had the focus, so nothing happened. Click on the game window and "
                  "press it again. Said at most 3 times a session.",
            ecm::prefix());
    }
    // The journal states what the key means: before LoadGame every press is menu navigation. With no journal to ask, every press counts.
    in.gameplay = !journalWatchActive() || journalGameplay();
    in.onFootKnown = journalOnFootKnown();
    in.onFoot = journalOnFoot();
    in.readBindings = cfg.getBool("hotkey.read_game_bindings", true);
    boundaryAt(frameNo, GetTickCount64(), in, sink);
}

bool explorerCamBuildKnown(uintptr_t* baseOut, char* why, size_t whyCap) {
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

ExplorerCamHookStatus explorerCamObserve(ExplorerCamHook hook, ExplorerCamActivityObserver observer, bool attach) {
    const int which = hook == ExplorerCamHook::Controller ? 1 : 0;
    const int id = which == 1 ? kHkCtl : kHkFree;
    if (attach && g_hooks[id].state == ExplorerCamHookStatus::NotTried) tryHook(id, ecm::Sink{&logSink, nullptr});
    if (attach) {
        if (armed(id) && observer) {
            bool present = false;
            for (int i = 0; i < kExplorerCamMaxObservers; ++i) present = present || g_observers[which][i].load() == observer;
            for (int i = 0; i < kExplorerCamMaxObservers && !present; ++i) {
                ExplorerCamActivityObserver empty = nullptr;
                if (g_observers[which][i].compare_exchange_strong(empty, observer)) present = true;
            }
        }
    } else {
        for (int i = 0; i < kExplorerCamMaxObservers; ++i) {
            ExplorerCamActivityObserver current = g_observers[which][i].load();
            if (current && (!observer || current == observer)) g_observers[which][i].store(nullptr);
        }
    }
    updateGates();
    ExplorerCamHookStatus status;
    status.state = g_hooks[id].state;
    status.stolen = g_hooks[id].stolen;
    status.target = g_hooks[id].target;
    status.relay = reinterpret_cast<uintptr_t>(g_hooks[id].relay);
    std::snprintf(status.why, sizeof(status.why), "%s", g_hooks[id].why);
    return status;
}

#ifdef EDVR_EXPLORER_CAM_TEST
namespace explorercamtest {
void setTargets(const ExplorerCamTestTargets& t) {
    g_testTargets[kHkFree] = t.freeCamera;
    g_testTargets[kHkCollision] = t.collision;
    g_testTargets[kHkBox] = t.boxPush;
    g_testTargets[kHkUi] = t.cameraUi;
    g_testTargets[kHkCtl] = t.controller;
}
void boundary(uint32_t frame, uint64_t nowMs, const ExplorerCamTestFrame& t, ExplorerCamSinkFn fn, void* ctx) {
    FrameInput in;
    in.on = t.on;
    in.up = t.up;
    in.forward = t.forward;
    in.right = t.right;
    in.hotkey = t.hotkey;
    in.f5Pressed = t.f5Pressed;
    in.gameplay = t.gameplay;
    in.onFootKnown = t.onFootKnown;
    in.onFoot = t.onFoot;
    in.readBindings = t.readBindings;
    // Hermetic: a rig with no fixture directory must not read the real player's Elite bindings.
    in.bindsDir = t.bindsDir ? t.bindsDir : L"C:\\edvr_explorer_cam_test_no_such_bindings_dir";
    boundaryAt(frame, nowMs, in, ecm::Sink{fn, ctx});
}
bool gateOpen() { return g_gate[kHkFree].load() != 0; }
bool uiGateOpen() { return g_gate[kHkUi].load() != 0; }
bool controllerGateOpen() { return g_gate[kHkCtl].load() != 0; }
size_t stolenBytes(int hook) { return hook >= 0 && hook < kHkCount ? g_hooks[hook].hook.stolenBytes() : 0; }
uint64_t placedActivity() { return g_placedActivity.load(); }
uint64_t bypassed() { return g_bypassed[0].load(); }
uint64_t forwarded() { return g_forwarded[0].load(); }
uint64_t boxBypassed() { return g_bypassed[1].load(); }
uint64_t boxForwarded() { return g_forwarded[1].load(); }
uint64_t updatesPlaced() { return g_updatesPlaced.load(); }
uint64_t hookCalls() { return g_hookCalls.load(); }
uint64_t controllerCalls() { return g_ctlCalls.load(); }
uint32_t controllerMode() { return g_ctlMode.load(); }
uint64_t uiCalls() { return g_uiCalls.load(); }
bool uiHiddenByEdvr() { return g_uiHiddenByUs.load(); }
uint32_t faults() { return g_faults.load(); }
uint32_t phase() { return g_phase.load(); }
bool placeActive() { return g_placeActive.load(); }
bool sessionActive() { return g_sessionActive.load(); }
bool exiting() { return g_exiting.load(); }
uint32_t f5Request() { return g_f5Request.load(); }
int hotkeyVk() { return g_f5.key(); }
void preThenPost(void* activity) {
    PreState ps = preFree(activity);
    postFor(ps);
}
void forceSession(bool on) { g_sessionActive.store(on); }
void controllerPreThenPost(void* controller) {
    PreState ps = preCtl(controller);
    postFor(ps);
}
void uiPreThenPost(void* object) {
    PreState ps = preUi(object);
    postFor(ps);
}
// Back to a session that has not tried any hook: every CodeHook is uninstalled (the original bytes return), every latch and counter
// clears. The relay pages are process-lifetime storage by design: the rig leaks them.
void reset() {
    for (HookEntry& entry : g_hooks) {
        entry.hook.uninstall();
        entry.relay = nullptr;
        entry.target = 0;
        entry.state = ExplorerCamHookStatus::NotTried;
        entry.stolen = 0;
        entry.why[0] = 0;
    }
    for (int i = 0; i < kHkCount; ++i) {
        g_forward[i].store(0);
        g_gate[i].store(0);
        g_testTargets[i] = 0;
    }
    for (int w = 0; w < 2; ++w)
        for (int i = 0; i < kExplorerCamMaxObservers; ++i) g_observers[w][i].store(nullptr);
    g_placedActivity.store(0);
    for (int i = 0; i < 2; ++i) {
        g_bypassed[i].store(0);
        g_forwarded[i].store(0);
    }
    g_placeActive.store(false);
    g_resetRequest.store(0);
    g_eyeUp.store(ecm::kEyeUpDefault);
    g_eyeForward.store(ecm::kEyeForwardDefault);
    g_eyeRight.store(ecm::kEyeRightDefault);
    for (auto& b : g_busy) b.store(false);
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
    g_sessionActive.store(false);
    g_exiting.store(false);
    g_f5Request.store(0);
    g_ctlCalls.store(0);
    g_ctlMode.store(0);
    g_uiCalls.store(0);
    g_uiHeld.store(false);
    g_uiHiddenByUs.store(false);
    g_eventSeq.store(0);
    ecm::Event drop;
    for (auto& ring : g_rings)
        while (ring.take(&drop)) {}
    g_machine = ecm::Machine();
    g_freeResetSeen = 0;
    g_firstCallNoted = false;
    g_seq = ecm::F5Sequencer();
    g_ctlResetSeen = 0;
    g_ctlFirstNoted = false;
    g_uiHider = ecm::UiHider();
    g_frame = FrameState();
    g_identity = 0;
    g_f5Configured = false;
    g_f5Name.clear();
    g_f5 = Hotkey();
    g_f5Missed = 0;
}
}  // namespace explorercamtest
#endif

}  // namespace edvr
