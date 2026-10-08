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
// start over by bumping g_resetRequest (the placement machine) or g_resetCtlRequest (the F5 sequencer), which each acts on at its next call,
// and clears the published state (g_placedActivity,
// g_phase, g_sessionActive) itself whenever placement is not active, every frame, so an end never depends on a hook being called
// again.
#include "explorer_cam.h"
#include "explorer_cam_core.h"
#include "explorer_cam_fade_core.h"
#include "explorer_cam_follow_core.h"

#include <windows.h>
#include <intrin.h>

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>

#include "../common/code_hook.h"
#include "../common/comfort_fade.h"
#include "../common/config.h"
#include "../common/hotkey.h"
#include "../common/log.h"
#include "elite_binds.h"
#include "journal_watch.h"

namespace edvr {
namespace {

// ---- the seven hooks ------------------------------------------------------------------------------------------------------------
enum Hk : int { kHkFree = 0, kHkCollision, kHkBox, kHkUi, kHkCtl, kHkFade, kHkFind, kHkZoom, kHkCount };
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
    {"avatar-fade", "AvatarModelComponent dither-fade update", "explorer-cam-avatar-fade", ecm::kAvatarFadeRva, ecm::kAvatarFadePrologue,
     ecm::kAvatarFadePrologueBytes, false},
    {"find-joint", "skeleton interface FindJoint(name)", "explorer-cam-find-joint", ecm::kFindJointRva, ecm::kFindJointPrologue,
     ecm::kFindJointPrologueBytes, false},
    {"zoom/DOF", "VanityCameraDofAndZoomControls update", "explorer-cam-zoom-dof", ecm::kZoomDofRva, ecm::kZoomDofPrologue, ecm::kZoomDofPrologueBytes, false},
};

// ---- state shared between the threads --------------------------------------------------------------------------------------
// The bypass relays read g_placedActivity and bump their counters from machine code, so those are plain 8-byte atomics.
alignas(8) std::atomic<uint64_t> g_placedActivity{0};   // the activity while a pose is being written, else 0
alignas(8) std::atomic<uint64_t> g_bypassed[2];         // [0] collision, [1] box push: calls answered "no edit" by the relay
alignas(8) std::atomic<uint64_t> g_forwarded[2];        // ...and calls passed to the original
alignas(8) std::atomic<uintptr_t> g_gate[kHkCount];     // a callback relay's gate: open = the callback runs, closed = straight on
std::atomic<uintptr_t> g_forward[kHkCount];             // a callback relay's trampoline
std::atomic<ExplorerCamActivityObserver> g_observers[3][kExplorerCamMaxObservers];   // free camera, controller, avatar fade
// The FindJoint hook's capture. 0x19B1240 attaches EVERY humanoid's avatars (F6: site 1 alone is whichever humanoid ran last, usually an NPC). Only the
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
std::atomic<uint32_t> g_findWant{0};                   // bit 0 the probe, bit 1 Explorer Cam (placement active)

// Phase 2: head hiding (hook threads write; the frame thread reads and logs).
std::atomic<bool> g_hideOn{false};                     // the fade hook's post-call hides: active, both hooks armed, the part names verified
std::atomic<bool> g_hideDown{false};                   // stood down for the session: guarded accesses of the AMC faulted
std::atomic<uint32_t> g_hideFaults{0};
std::atomic<int> g_partNames{0};                       // 0 not checked, 1 the exe's name table matches kPartNames, 2 it does not
std::atomic<uint64_t> g_amcCalls{0}, g_amcLocalMatches{0}, g_amcIdOk{0}, g_amcMode3{0}, g_hideCalls{0}, g_hideZeroed{0};
std::atomic<uint64_t> g_amcLocal{0}, g_amcLocalPose{0}, g_amcCand{0}, g_amcCandPose{0};
std::atomic<bool> g_busyAmc{false};
uint64_t g_amcCensused = 0;                            // g_busyAmc
ecm::Ring<ecm::AmcCensus, 2> g_censusRing;

std::atomic<bool> g_placeActive{false};                 // on, every required hook armed, not stood down by faults (frame thread)
std::atomic<uint32_t> g_resetRequest{0};                // the free-camera hook's placement machine starts over
std::atomic<uint32_t> g_resetCtlRequest{0};             // the controller hook's F5 sequencer starts over (a session ended, or placement toggled)
std::atomic<float> g_eyeUp{ecm::kEyeUpDefault}, g_eyeForward{ecm::kEyeForwardDefault}, g_eyeRight{ecm::kEyeRightDefault};

// Phase 3: the head-joint eye source (docs\design-explorer-cam-free-camera-2026-10-07.md, "Phase 3: the camera follows the head joint"). The frame thread publishes
// the temporary keys and arms the source; the free-camera hook (the camera-job thread) reads the latched local skeleton's head joint with +0x58 before each placing
// update, and falls back to the fixed eye keys when it cannot.
struct TimerStat {
    std::atomic<uint32_t> n{0}, minUs{0xFFFFFFFFu}, maxUs{0}, allMaxUs{0};
};
std::atomic<float> g_trimRight{0.0f}, g_trimUp{0.0f}, g_trimForward{0.0f}, g_followSmoothMs{0.0f};
std::atomic<bool> g_followReady{false};          // g_followT is written and the joint-name literals matched (released by the frame thread)
std::atomic<bool> g_followDown{false};           // the head source stood down for the session (8 faults, or a vtable slot differs); the placement goes on
std::atomic<uint32_t> g_followFaults{0};
std::atomic<uint64_t> g_headUpdates{0}, g_fixedUpdates{0};
std::atomic<uint32_t> g_followWhy{0};            // ecm::FixedWhy of the latest fixed update
std::atomic<uint32_t> g_followSource{0};         // 1 = the latest placing update took its eye from the head joint
std::atomic<float> g_lastEye[3];                 // the latest eye written: up, forward, right
// The comfort fade's facts from the hook threads (explorer_cam_fade_core.h): how many placing updates in a row the eye has moved under 2 cm, and whether the camera
// UI's hide has run its course for this placement.
std::atomic<uint32_t> g_steadyUpdates{0};
std::atomic<bool> g_prevEyeValid{false};
std::atomic<bool> g_uiSettled{false};
std::atomic<uint32_t> g_fadeAlphaBits{0};        // the level the frame thread last published (the rig reads it back)
TimerStat g_t58;                                 // the +0x58 call
ecm::Ring<ecm::FollowNote, 8> g_followNotes;
ecm::HeadTargets g_followT;

// Phase 3: isolating the camera suite (explorer_cam_follow_core.h, section C).
std::atomic<bool> g_isoDown{false};              // stood down for the session (guarded accesses of the action objects faulted); the placement goes on
std::atomic<uint32_t> g_isoFaults{0};
std::atomic<uint64_t> g_isoBlocked[ecm::kIsoHolderCount];   // presses swallowed per holder (fields that were non-zero when zeroed)
std::atomic<uint64_t> g_zoomCalls{0};            // the zoom/DOF activity's update reached its hook

std::atomic<bool> g_busy[4];                            // free camera, camera UI, controller, zoom/DOF: a call is inside that hook's pre..post
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
std::atomic<bool> g_fadeOurs{false};                    // EDVR holds the dither-fade global at 0 (the frame thread writes, the controller gate reads)

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
bool g_ctlSharedNoted = false;

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
    bool amcMatchSaid = false, amcNoMatchSaid = false, amcDownSaid = false;
    uint64_t latchedThirdSeen = 0;
    uint64_t beatHideCalls = 0, beatZeroed = 0;
    // Phase 3.
    bool followArmTried = false, followCfgSaid = false, isoSaid = false, isoDownSaid = false;
    ecm::ComfortTimeline fade;
    ecm::Trim lastTrim;
    float lastSmooth = 0.0f;
    uint64_t beatHeadUpdates = 0, beatFixedUpdates = 0, beatZoomCalls = 0;
    uint32_t fadeAlphaBits = 0;
    uint64_t beatBlocked[ecm::kIsoHolderCount] = {};
};
FrameState g_frame;

// The avatar fade global (frame thread only): the guard, and where the global and its amount float live.
ecm::FadeGuard g_fade;
int32_t* g_fadeMode = nullptr;
float* g_fadeAmount = nullptr;


#ifdef EDVR_EXPLORER_CAM_TEST
uintptr_t g_testTargets[kHkCount] = {};   // the rig's synthetic functions, installed in place of the game's
int32_t* g_testFadeMode = nullptr;        // ...and its synthetic fade global
float* g_testFadeAmount = nullptr;
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
// The controller's mode byte and its pending-retry byte (+0x3E0, +0x3E1).
__declspec(noinline) bool sehReadCtlView(const uint8_t* c, uint8_t* mode, uint8_t* pending) noexcept {
    __try {
        *mode = c[ecm::kOffCtlMode];
        *pending = c[ecm::kOffCtlPending];
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
// The controller's shared record is NOT embedded in it (see explorer_cam_core.h, kOffCtlInterface): the controller reaches it by a virtual
// call, slot +0x20, on the interface cached at +0x108. If that accessor is a plain `lea rax,[rcx+disp]; ret` the record is interface+disp and
// its +0x1D can be read without calling anything. 0 = read (flag set), 1 = no interface cached or implausible pointers, 2 = the accessor is
// not a plain lea (its first 8 bytes are returned), 3 = a fault.
bool plausibleUserPointer(uint64_t p) { return p >= 0x10000u && p < 0x00007FFF00000000ull; }
__declspec(noinline) int sehReadSharedRecord(const uint8_t* ctl, int32_t* flag, uint64_t* accessor) noexcept {
    __try {
        uint64_t iface = 0;
        std::memcpy(&iface, ctl + ecm::kOffCtlInterface, 8);
        if (!plausibleUserPointer(iface) || (iface & 7u) != 0) return 1;
        uint64_t vptr = 0;
        std::memcpy(&vptr, reinterpret_cast<const void*>(iface), 8);
        if (!plausibleUserPointer(vptr) || (vptr & 7u) != 0) return 1;
        uint64_t fn = 0;
        std::memcpy(&fn, reinterpret_cast<const void*>(vptr + 8u * (ecm::kSharedAccessorSlot / 8u)), 8);
        if (!plausibleUserPointer(fn)) return 1;
        uint8_t code[8] = {};
        std::memcpy(code, reinterpret_cast<const void*>(fn), 8);
        std::memcpy(accessor, code, 8);
        int64_t disp = -1;
        if (code[0] == 0x48 && code[1] == 0x8D && code[2] == 0x81 && code[7] == 0xC3) {          // lea rax,[rcx+disp32]; ret
            int32_t d = 0;
            std::memcpy(&d, code + 3, 4);
            disp = d;
        } else if (code[0] == 0x48 && code[1] == 0x8D && code[2] == 0x41 && code[4] == 0xC3) {   // lea rax,[rcx+disp8]; ret
            disp = static_cast<int8_t>(code[3]);
        }
        if (disp < 0 || disp > 0x4000) return 2;
        *flag = *reinterpret_cast<const uint8_t*>(iface + static_cast<uint64_t>(disp) + ecm::kOffSharedFlag);
        return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 3;
    }
}
// The fade global and its amount float: plain data in the exe's .data, read and written under SEH.
__declspec(noinline) bool sehReadInt32(const int32_t* p, int32_t* out) noexcept {
    __try {
        *out = *reinterpret_cast<const volatile int32_t*>(p);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
__declspec(noinline) bool sehReadFloat(const float* p, float* out) noexcept {
    __try {
        *out = *reinterpret_cast<const volatile float*>(p);
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
    g_prevEyeValid.store(false, std::memory_order_relaxed);   // the next placement's eye starts a new steady count
    g_steadyUpdates.store(0, std::memory_order_relaxed);
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

// A Seq event carries its detail in the spare fields (explorer_cam_core.h, formatEvent): before = the readiness wait, updates = the wait for
// mode 3, flags = the unmet conditions, the queued bit and the press count (ecm::packSeqFlags).
void pushSeqEvent(ecm::SeqEvent ev, uint64_t controller, uint32_t mode, uint32_t presetKind, uint32_t readyAfter, uint32_t toMode3, uint32_t flags) {
    ecm::Event e;
    e.seq = g_eventSeq.fetch_add(1, std::memory_order_relaxed);
    e.kind = static_cast<uint32_t>(ecm::EvKind::Seq);
    e.activity = controller;
    e.threadId = GetCurrentThreadId();
    e.why = static_cast<uint32_t>(ev);
    e.before = readyAfter;
    e.after = mode;
    e.flags = flags;
    e.count = presetKind;
    e.updates = toMode3;
    g_rings[kRingCtl].push(e);
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

// ---- Phase 3: isolating the camera suite ---------------------------------------------------------------------------------------------
// While a session has placed the view, each camera object's action objects are cleared before its update reads them and put back after it: save the field,
// write zero, run the original, write the saved value back. Only fields that were non-zero are touched, and the handles are read again on every call (the
// binder re-resolves them on a context change). EDVR's own presses are set AFTER the clear, so they survive it. A fault stands the isolation down after
// ecm::kMaxIsoFaults, never the placement.
__declspec(noinline) int sehIsoClear(const uint8_t* holder, const ecm::IsoField* fields, size_t n, ecm::IsoSaved* saved) noexcept {
    __try {
        for (size_t i = 0; i < n; ++i) {
            uint64_t p = 0;
            std::memcpy(&p, holder + fields[i].handle, 8);
            if (!plausibleUserPointer(p) || (p & 7u) != 0) continue;   // no action object there (any handle may be null)
            const uint64_t at = p + ecm::isoFieldOffset(fields[i].kind);
            uint32_t v = 0;
            if (fields[i].kind == ecm::IsoKind::Held) v = *reinterpret_cast<volatile const uint8_t*>(at);
            else v = *reinterpret_cast<volatile const uint32_t*>(at);
            if (v == 0) continue;   // nothing pressed: nothing to clear, nothing to put back
            const uint32_t k = saved->count;
            saved->addr[k] = at;
            saved->value[k] = v;
            saved->width[k] = static_cast<uint8_t>(ecm::isoFieldWidth(fields[i].kind));
            saved->count = k + 1;   // recorded BEFORE the write, so a fault in the write is still put back
            if (fields[i].kind == ecm::IsoKind::Held) *reinterpret_cast<volatile uint8_t*>(at) = 0;
            else *reinterpret_cast<volatile uint32_t*>(at) = 0;
        }
        return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}
__declspec(noinline) bool sehPokeIso(uint64_t at, uint32_t value, uint8_t width) noexcept {
    __try {
        if (width == 1) *reinterpret_cast<volatile uint8_t*>(at) = static_cast<uint8_t>(value);
        else *reinterpret_cast<volatile uint32_t*>(at) = value;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
// Every saved value back, bit for bit (the last saved first, so an object reached through two handles ends at its first value). False if any write faulted.
bool isoRestore(const ecm::IsoSaved& s) noexcept {
    bool ok = true;
    for (uint32_t i = s.count; i-- > 0;) ok = sehPokeIso(s.addr[i], s.value[i], s.width[i]) && ok;
    return ok;
}
void isoFault() noexcept {
    if (g_isoFaults.fetch_add(1, std::memory_order_relaxed) + 1 >= ecm::kMaxIsoFaults) g_isoDown.store(true, std::memory_order_release);
}
// The isolation is in force from the placement on, for the whole session (the exit's own presses still go in), and never when Explorer Cam is off, stood down
// or has had its fault budget spent.
bool isolating() noexcept {
    return g_placeActive.load(std::memory_order_acquire) && g_faults.load(std::memory_order_relaxed) < ecm::kMaxFaults && g_sessionActive.load(std::memory_order_acquire) &&
           g_placedActivity.load(std::memory_order_acquire) != 0 && !g_isoDown.load(std::memory_order_acquire);
}

// What a hook's pre half hands to its post half.
struct PreState {
    int hook = -1;          // 0 free camera, 1 camera UI, 2 controller, 3 zoom/DOF
    bool holdsBusy = false;
    bool pressed = false;
    uint64_t object = 0;
    uint64_t action = 0;
    int32_t previous = 0;
    ecm::IsoSaved iso;      // the camera suite's action objects the isolation cleared: put back after the original
};
constexpr int kHookFree = 0, kHookUi = 1, kHookCtl = 2, kHookZoom = 3;

// Clear the holder's action objects (when the isolation is in force) before its update runs; postFor puts them back. Called AFTER the hook's own decisions
// and BEFORE its own press, so EDVR's presses survive the clear.
void isoClear(PreState& ps, int holder, const void* objectPtr) noexcept {
    if (!isolating()) return;
    const int r = sehIsoClear(static_cast<const uint8_t*>(objectPtr), ecm::isoFields(holder), ecm::kIsoCounts[holder], &ps.iso);
    if (r < 0) isoFault();   // whatever was saved before the fault is put back by postFor
    if (ps.iso.count) g_isoBlocked[holder].fetch_add(ps.iso.count, std::memory_order_relaxed);
}


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
    // The isolation's saved values go back AFTER the own press is restored (that restore wrote the value read after the clear), bit for bit.
    if (ps.iso.count) {
        if (!isoRestore(ps.iso)) isoFault();
        ps.iso.count = 0;
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

// ---- Phase 3: the eye follows the head joint -------------------------------------------------------------------------------------------
// Read on the camera-job thread, inside the free-camera hook's pre-call (H proved the calls safe there and under 1 us). Before EVERY call into the game the
// interface's vtable is RR's or AO's and every slot used holds exactly the build's function; an interface that no longer has such a vtable is stale (the avatar
// was destroyed: the fixed keys serve until the next latch), a slot that differs is another build (the head source stands down), a fault is counted and the
// fixed keys serve that update.
uint64_t qpcNow() noexcept {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return static_cast<uint64_t>(t.QuadPart);
}
uint64_t realNowUs() {
    static const uint64_t freq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return static_cast<uint64_t>(f.QuadPart > 0 ? f.QuadPart : 1);
    }();
    return qpcNow() * 1000000ull / freq;
}
uint64_t (*g_followNowUs)() = &realNowUs;   // the rig scripts the clock the smoothing reads
void atomicMaxU32(std::atomic<uint32_t>& a, uint32_t v) noexcept {
    uint32_t c = a.load(std::memory_order_relaxed);
    while (v > c && !a.compare_exchange_weak(c, v, std::memory_order_relaxed)) {}
}
void atomicMinU32(std::atomic<uint32_t>& a, uint32_t v) noexcept {
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
    atomicMinU32(t.minUs, us);
    atomicMaxU32(t.maxUs, us);
    atomicMaxU32(t.allMaxUs, us);
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
__declspec(noinline) bool sehReadSlots(uintptr_t vptr, uint64_t fn[ecm::kHeadSlots]) noexcept {
    __try {
        for (uint32_t i = 0; i < ecm::kHeadSlots; ++i) std::memcpy(&fn[i], reinterpret_cast<const void*>(vptr + 8u * ecm::kHeadSlotIndex[i]), 8);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
// The pose's local transforms (32 bytes a joint) and u16 parents, copied for the walk.
__declspec(noinline) bool sehReadPoseArrays(uintptr_t pose, uint32_t joints, float* locals, uint16_t* parents) noexcept {
    __try {
        uint64_t lp = 0, pp = 0;
        std::memcpy(&lp, reinterpret_cast<const void*>(pose + ecm::kPoseLocalsOff), 8);
        std::memcpy(&pp, reinterpret_cast<const void*>(pose + ecm::kPoseParentsOff), 8);
        if (!plausibleUserPointer(lp) || !plausibleUserPointer(pp)) return false;
        std::memcpy(locals, reinterpret_cast<const void*>(lp), static_cast<size_t>(joints) * ecm::kJointBytes);
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
    uint64_t fn[ecm::kHeadSlots] = {};
    ecm::HeadKind kind = ecm::HeadKind::None;
    uint32_t badSlot = 0;
    uint64_t found = 0, expected = 0;
};
SkVerify skVerify(uintptr_t iface, ecm::HeadKind expect, SkSlots* s) noexcept {
    uint64_t vptr = 0;
    if (!plausibleUserPointer(iface) || !sehReadU64At(iface, &vptr)) return SkVerify::Stale;
    const ecm::HeadKind kind = ecm::headKindOfVtable(vptr, g_followT);
    if (kind == ecm::HeadKind::None || (expect != ecm::HeadKind::None && kind != expect)) return SkVerify::Stale;
    if (!sehReadSlots(static_cast<uintptr_t>(vptr), s->fn)) return SkVerify::Fault;
    uint32_t bad = 0;
    uint64_t found = 0;
    if (ecm::headVerifySlots(vptr, s->fn, kind, g_followT, &bad, &found) != 0) {
        s->badSlot = bad;
        s->found = found;
        s->expected = ecm::headExpectedFunction(kind, bad, g_followT);
        return SkVerify::Mismatch;
    }
    s->kind = kind;
    return SkVerify::Ok;
}

struct FollowCache {                  // hook thread only (the free-camera hook's busy flag)
    uint64_t iface = 0;               // the skeleton the rest offset below belongs to
    bool failed = false;              // its rest offset could not be derived: the fixed keys serve it until the latch moves (or, for a pose not built yet, until a retry works)
    bool retryable = false;           // the failure is a pose that is not there yet (no pose, no joints): tried again every kFollowRetryUpdates updates
    bool failNoted = false;           // the failure was said once; the retries are silent
    uint32_t retryIn = 0;
    bool firstLive = false;
    ecm::HeadKind kind = ecm::HeadKind::None;
    uint32_t headIdx = ecm::kNoJoint, povIdx = ecm::kNoJoint, joints = 0;
    ecm::RestOffset rest;
};
FollowCache g_fc;
ecm::EyeSmoother g_smoother;
uint64_t g_smoothLastUs = 0;
int g_followHeadLast = -1;            // -1 nothing said yet, 0 fixed, 1 head joint: the source the last switch note named
uint32_t g_followWhyLast = 0;
uint32_t g_followSwitchNotes = 0;
float g_walkLocals[ecm::kMaxWalkJoints * ecm::kJointFloats];
uint16_t g_walkParents[ecm::kMaxWalkJoints];

void followFault() noexcept {
    const uint32_t n = g_followFaults.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n >= ecm::kMaxFollowFaults && !g_followDown.exchange(true, std::memory_order_acq_rel)) {
        ecm::FollowNote note;
        note.kind = static_cast<uint32_t>(ecm::FollowNoteKind::StoodDown);
        note.why = 0;
        note.faults = n;
        note.iface = g_fc.iface;
        g_followNotes.push(note);
    }
}
bool followFixed(ecm::FixedWhy why) noexcept {
    g_followWhy.store(static_cast<uint32_t>(why), std::memory_order_relaxed);
    return false;
}
// A verification that did not pass: stale (try again when the latch moves), a slot that differs (stand the head source down), or a fault (counted).
ecm::FixedWhy followVerifyFailed(SkVerify v, uint64_t iface, const SkSlots& sl) noexcept {
    if (v == SkVerify::Stale) return ecm::FixedWhy::Stale;
    if (v == SkVerify::Mismatch) {
        if (!g_followDown.exchange(true, std::memory_order_acq_rel)) {
            ecm::FollowNote note;
            note.kind = static_cast<uint32_t>(ecm::FollowNoteKind::StoodDown);
            note.why = 1;
            note.iface = iface;
            note.a = 8u * ecm::kHeadSlotIndex[sl.badSlot];
            note.b = sl.found;
            note.c = sl.expected;
            g_followNotes.push(note);
        }
        return ecm::FixedWhy::StoodDown;
    }
    followFault();
    return ecm::FixedWhy::Fault;
}

// A new skeleton: verify it, find the head joint, walk the animated pose once for the rest offset. FixedWhy::None = the cache is ready for this skeleton.
ecm::FixedWhy followDerive(uint64_t iface, FollowCache& c) noexcept {
    using ecm::FixedWhy;
    SkSlots sl;
    SkVerify v = skVerify(static_cast<uintptr_t>(iface), ecm::HeadKind::None, &sl);
    if (v != SkVerify::Ok) return followVerifyFailed(v, iface, sl);
    ecm::FollowNote note;
    note.iface = iface;
    note.hkind = static_cast<uint32_t>(sl.kind);
    note.povIdx = g_skelIndex[0].load(std::memory_order_relaxed) & 0xFFFFu;
    auto cacheFailure = [&](ecm::RestWhy why) {
        c.iface = iface;   // do not try again until the latch moves, unless the pose is simply not built yet
        c.failed = true;
        c.retryable = why == ecm::RestWhy::NoPose || why == ecm::RestWhy::NoJoints;
        c.retryIn = ecm::kFollowRetryUpdates;
        if (!c.failNoted) {   // said once per skeleton; a retry that fails again is silent
            c.failNoted = true;
            note.kind = static_cast<uint32_t>(ecm::FollowNoteKind::RestFailed);
            note.why = static_cast<uint32_t>(why);
            g_followNotes.push(note);
        }
        return FixedWhy::Unverified;
    };
    uintptr_t pose = 0;
    if (!sehCallPose(sl.fn[0], static_cast<uintptr_t>(iface), &pose)) {
        followFault();
        return FixedWhy::Fault;
    }
    if (pose == 0) return cacheFailure(ecm::RestWhy::NoPose);
    uint16_t joints = 0;
    if (!sehReadU16At(pose, &joints)) {
        followFault();
        return FixedWhy::Fault;
    }
    note.joints = joints;
    if (joints == 0) return cacheFailure(ecm::RestWhy::NoJoints);
    if (joints > ecm::kMaxWalkJoints) return cacheFailure(ecm::RestWhy::TooManyJoints);
    v = skVerify(static_cast<uintptr_t>(iface), sl.kind, &sl);
    if (v != SkVerify::Ok) return followVerifyFailed(v, iface, sl);
    uint32_t headIdx = ecm::kNoJoint;
    if (!sehCallFind(sl.fn[1], static_cast<uintptr_t>(iface), ecm::kHeadName, &headIdx)) {
        followFault();
        return FixedWhy::Fault;
    }
    headIdx &= 0xFFFFu;
    note.headIdx = headIdx;
    if (!sehReadPoseArrays(pose, joints, g_walkLocals, g_walkParents)) {
        followFault();
        return FixedWhy::Fault;
    }
    ecm::RestOffset rest;
    const ecm::RestWhy w = ecm::deriveRestOffset(g_walkLocals, g_walkParents, joints, headIdx, note.povIdx, &rest);
    if (w != ecm::RestWhy::Ok) return cacheFailure(w);
    c.iface = iface;
    c.failed = false;
    c.retryable = false;
    c.retryIn = 0;
    c.firstLive = false;
    c.kind = sl.kind;
    c.headIdx = headIdx;
    c.povIdx = note.povIdx;
    c.joints = joints;
    c.rest = rest;
    note.kind = static_cast<uint32_t>(ecm::FollowNoteKind::Rest);
    note.headDepth = rest.headDepth;
    note.povDepth = rest.povDepth;
    std::memcpy(note.headRest, rest.headRest, sizeof(note.headRest));
    std::memcpy(note.povRest, rest.povRest, sizeof(note.povRest));
    std::memcpy(note.delta, rest.delta, sizeof(note.delta));
    std::memcpy(note.local, rest.local, sizeof(note.local));
    g_followNotes.push(note);
    return FixedWhy::None;
}

// The eye from the head joint, or false (the reason is in g_followWhy and `eye` is untouched).
bool followEye(ecm::Eye* eye, const ecm::Trim& trim) noexcept {
    using ecm::FixedWhy;
    if (g_followDown.load(std::memory_order_acquire)) return followFixed(FixedWhy::StoodDown);
    if (!g_followReady.load(std::memory_order_acquire)) return followFixed(FixedWhy::NotArmed);
    const uint64_t iface = g_skelIface[0].load(std::memory_order_acquire);
    if (iface == 0) return followFixed(FixedWhy::NothingLatched);
    FollowCache& c = g_fc;
    if (c.iface != iface) {
        c = FollowCache();
        const FixedWhy w = followDerive(iface, c);
        if (w != FixedWhy::None) return followFixed(w);
    } else if (c.failed) {
        // A pose that is not there yet (no pose, no joints) is tried again every kFollowRetryUpdates updates, silently, until it reads; any other failure stands.
        if (!c.retryable || --c.retryIn != 0) return followFixed(FixedWhy::Unverified);
        c.retryIn = ecm::kFollowRetryUpdates;   // a retry that faults or goes stale is tried again a window later, not never
        const FixedWhy w = followDerive(iface, c);
        if (w != FixedWhy::None) return followFixed(w);
    }
    SkSlots sl;
    const SkVerify v = skVerify(static_cast<uintptr_t>(iface), c.kind, &sl);
    if (v != SkVerify::Ok) {
        if (v == SkVerify::Stale) c.iface = 0;   // the interface is gone: derive again when the latch names one
        return followFixed(followVerifyFailed(v, iface, sl));
    }
    alignas(16) float m[16] = {};
    const uint64_t t0 = qpcNow();
    const bool ok = sehCallModel(sl.fn[3], static_cast<uintptr_t>(iface), c.headIdx, m);
    noteTimer(g_t58, t0, qpcNow());
    if (!ok) {
        followFault();
        return followFixed(FixedWhy::Fault);
    }
    if (!ecm::headMatrixPlausible(m)) return followFixed(FixedWhy::Implausible);
    float p[3];
    ecm::headEyeModel(m, c.rest.local, p);
    const ecm::Eye e = ecm::eyeFromModelPoint(p, trim);
    if (!c.firstLive) {
        c.firstLive = true;
        ecm::FollowNote note;
        note.kind = static_cast<uint32_t>(ecm::FollowNoteKind::FirstLive);
        note.iface = iface;
        std::memcpy(note.live, m + 12, sizeof(note.live));
        note.rotDiff = ecm::rotationDiff(m, c.rest.headRot);
        note.eye[0] = e.up;
        note.eye[1] = e.forward;
        note.eye[2] = e.right;
        g_followNotes.push(note);
    }
    *eye = e;
    g_followWhy.store(static_cast<uint32_t>(FixedWhy::None), std::memory_order_relaxed);
    return true;
}

// After the source is chosen: the optional smoothing (head-joint eyes only; 0 returns the eye unchanged, bit for bit), the counters, the latest eye, and a note
// when the source changed (the first few only).
void followAfter(bool fromHead, ecm::Eye* eye) noexcept {
    if (fromHead) {
        const float tau = g_followSmoothMs.load(std::memory_order_relaxed);
        double dtMs = 0.0;
        if (tau > 0.0f) {
            const uint64_t nowUs = g_followNowUs();
            if (g_smoother.have && nowUs >= g_smoothLastUs) dtMs = static_cast<double>(nowUs - g_smoothLastUs) / 1000.0;
            g_smoothLastUs = nowUs;
        }
        *eye = ecm::smoothEye(g_smoother, *eye, dtMs, tau);
        g_headUpdates.fetch_add(1, std::memory_order_relaxed);
    } else {
        g_smoother.have = false;   // a switch back to the head joint starts from the head joint, not from a stale smoothed eye
        g_fixedUpdates.fetch_add(1, std::memory_order_relaxed);
    }
    g_followSource.store(fromHead ? 1u : 0u, std::memory_order_relaxed);
    {
        // Steady: this eye is within 2 cm of the last one written (either source). The first eye of a placement has nothing to compare with and starts the count.
        if (g_prevEyeValid.load(std::memory_order_relaxed)) {
            ecm::Eye prev;
            prev.up = g_lastEye[0].load(std::memory_order_relaxed);
            prev.forward = g_lastEye[1].load(std::memory_order_relaxed);
            prev.right = g_lastEye[2].load(std::memory_order_relaxed);
            const bool still = ecm::eyeStepMetres(*eye, prev) < ecm::kFadeSteadyMetres;
            g_steadyUpdates.store(still ? g_steadyUpdates.load(std::memory_order_relaxed) + 1 : 0u, std::memory_order_relaxed);
        } else {
            g_steadyUpdates.store(0, std::memory_order_relaxed);
            g_prevEyeValid.store(true, std::memory_order_relaxed);
        }
    }
    g_lastEye[0].store(eye->up, std::memory_order_relaxed);
    g_lastEye[1].store(eye->forward, std::memory_order_relaxed);
    g_lastEye[2].store(eye->right, std::memory_order_relaxed);
    const uint32_t why = g_followWhy.load(std::memory_order_relaxed);
    if (static_cast<int>(fromHead) != g_followHeadLast) {
        g_followHeadLast = fromHead ? 1 : 0;
        g_followWhyLast = why;
        if (g_followSwitchNotes++ < 10) {
            ecm::FollowNote note;
            note.kind = static_cast<uint32_t>(ecm::FollowNoteKind::Switched);
            note.why = fromHead ? static_cast<uint32_t>(ecm::FixedWhy::None) : why;
            note.iface = g_skelIface[0].load(std::memory_order_relaxed);
            g_followNotes.push(note);
        }
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
        g_smoother = ecm::EyeSmoother();   // a new placement starts from the head joint, not from a smoothed eye of the last one
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

    // The eye: the head joint of the latched local skeleton (+ the trims), or, when none can be read, the fixed keys (the fallback).
    ecm::Eye eye;
    eye.up = g_eyeUp.load(std::memory_order_relaxed);
    eye.forward = g_eyeForward.load(std::memory_order_relaxed);
    eye.right = g_eyeRight.load(std::memory_order_relaxed);
    ecm::Trim trim;
    trim.right = g_trimRight.load(std::memory_order_relaxed);
    trim.up = g_trimUp.load(std::memory_order_relaxed);
    trim.forward = g_trimForward.load(std::memory_order_relaxed);
    followAfter(followEye(&eye, trim), &eye);
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
    // The isolation: the camera's own keys are cleared now, so EDVR's lock press below goes in after the clear and survives it.
    isoClear(ps, ecm::kIsoFreeCamera, bytes);
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
    g_uiSettled.store(wantHidden && g_uiHider.settledForPlacement(), std::memory_order_release);
    g_uiHiddenByUs.store(g_uiHider.hiddenByUs(), std::memory_order_release);
    g_uiHeld.store(g_uiHider.hiddenByUs() || g_uiHider.pending(), std::memory_order_release);
    if (s.ev != ecm::UiEvent::None) pushEvent(kRingUi, ecm::EvKind::Ui, object, static_cast<uint32_t>(s.ev), 0, s.hidden, 0, nullptr, 0);
    isoClear(ps, ecm::kIsoCameraUi, bytes);   // before EDVR's own hide press below
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
    const uint32_t request = g_resetCtlRequest.load(std::memory_order_acquire);
    if (request != g_ctlResetSeen) {
        g_ctlResetSeen = request;
        g_seq.reset();
        g_ctlSharedNoted = false;
    }
    const bool active = g_placeActive.load(std::memory_order_acquire) && g_faults.load(std::memory_order_relaxed) < ecm::kMaxFaults;
    // The mode byte is published for the avatar-fade restore as long as EDVR holds the fade global, even with the feature off.
    if (!active && !g_fadeOurs.load(std::memory_order_acquire)) return;
    const uint64_t controller = reinterpret_cast<uint64_t>(controllerPtr);
    const uint8_t* const bytes = static_cast<const uint8_t*>(controllerPtr);
    uint8_t mode = 0, pending = 0;
    if (!sehReadCtlView(bytes, &mode, &pending)) {
        onFault(kRingCtl, ecm::FaultSite::ReadController, controller);
        return;
    }
    g_ctlMode.store(mode, std::memory_order_relaxed);
    if (!active) return;
    if (!g_ctlFirstNoted) {
        g_ctlFirstNoted = true;
        pushEvent(kRingCtl, ecm::EvKind::ControllerFirstCall, controller, 0, 0, mode, 0, nullptr, 0);
    }
    const ecm::F5Req req = static_cast<ecm::F5Req>(g_f5Request.exchange(0, std::memory_order_acq_rel));
    ecm::CtlView view;
    view.mode = mode;
    view.pending = pending;
    view.sharedFlag = -1;
    // The TAB wait looks at the shared record's +0x1D when it can be read: only on a preset, only while a session wants the free camera.
    if ((mode == 1 || mode == 2) && (g_seq.sessionActive() || req == ecm::F5Req::Enter)) {
        int32_t flag = 0;
        uint64_t accessor = 0;
        const int how = sehReadSharedRecord(bytes, &flag, &accessor);
        if (how == 0) view.sharedFlag = static_cast<int16_t>(flag);
        if (!g_ctlSharedNoted && how != 3) {
            g_ctlSharedNoted = true;
            if (how == 0)
                pushSeqEvent(ecm::SeqEvent::SharedRecordFound, controller, mode, 0, 0, 0, static_cast<uint32_t>(flag));
            else
                pushSeqEvent(ecm::SeqEvent::SharedRecordUnreadable, controller, mode, 0, 0, static_cast<uint32_t>(accessor >> 32),
                             static_cast<uint32_t>(accessor & 0xFFFFFFFFu));
        }
    }
    const ecm::SeqStep s = g_seq.step(view, req, g_uiHeld.load(std::memory_order_acquire));
    g_sessionActive.store(s.sessionActive, std::memory_order_release);
    g_exiting.store(s.exiting, std::memory_order_release);
    if (!s.sessionActive) g_ctlSharedNoted = false;
    if (s.nev) {
        int32_t kind = 0;
        sehReadInt(bytes, ecm::kOffCtlPresetKind, &kind);   // for the line only; a failed read leaves 0
        for (uint8_t i = 0; i < s.nev; ++i) pushSeqEvent(s.ev[i], controller, mode, static_cast<uint32_t>(kind), s.readyAfter, s.toMode3,
                                                          ecm::packSeqFlags(s.unmet, s.queued, s.presses));
    }
    isoClear(ps, ecm::kIsoController, bytes);   // before F5's own presses below
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

// ---- the zoom/DOF hook (isolation only: the zoom, aperture and focus keys) -----------------------------------------------------
__declspec(noinline) PreState preZoom(void* object) noexcept {
    PreState ps;
    g_zoomCalls.fetch_add(1, std::memory_order_relaxed);
    if (!enter(ps, kHookZoom, object)) return ps;
    isoClear(ps, ecm::kIsoZoomDof, object);
    return ps;
}

__declspec(noinline) uint64_t __fastcall zoomDofHooked(void* a, void* b, void* c, void* d) noexcept {
    const auto forward = reinterpret_cast<ForwardFn>(g_forward[kHkZoom].load(std::memory_order_acquire));
    if (!forward) return 0;
    PreState ps = preZoom(a);
    const uint64_t result = forward(a, b, c, d);
    postFor(ps);
    return result;
}

// ---- the FindJoint hook (the probe's H rides it; nothing here writes) -------------------------------------------------------------
// FindJoint is called by many systems, so this stays trivial: the original FIRST (its result is what the observer is told, and it is returned
// unchanged), then one observer call carrying rcx, rdx, the result and the caller's return address. The callback relay JUMPS here, so the
// return address on entry is the caller's own.
__declspec(noinline) uint64_t __fastcall findJointHooked(void* a, void* b, void* c, void* d) noexcept {
    const uintptr_t returnAddress = reinterpret_cast<uintptr_t>(_ReturnAddress());
    const uintptr_t where = reinterpret_cast<uintptr_t>(_AddressOfReturnAddress());   // the relay JUMPS here, so this is the caller's own stack slot
    const auto forward = reinterpret_cast<ForwardFn>(g_forward[kHkFind].load(std::memory_order_acquire));
    if (!forward) return 0;
    const uint64_t result = forward(a, b, c, d);
    g_findSeen.fetch_add(1, std::memory_order_relaxed);
    const int site = ecm::findCaptureSite(returnAddress, reinterpret_cast<uintptr_t>(b), g_findSite[0].load(std::memory_order_relaxed),
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
        if (ecm::isLocalPair(third.valid, third.where, where, third.iface, reinterpret_cast<uint64_t>(a))) {
            g_skelIndex[0].store(third.index, std::memory_order_relaxed);
            g_skelIndex[1].store(static_cast<uint32_t>(result & 0xFFFFu), std::memory_order_relaxed);
            g_skelIface[1].store(reinterpret_cast<uint64_t>(a), std::memory_order_release);
            g_skelIface[0].store(third.iface, std::memory_order_release);
            g_skelLatches.fetch_add(1, std::memory_order_relaxed);
        }
    }
    return result;
}

// ---- Phase 2: hiding the head parts (the avatar-fade hook's post-call) -------------------------------------------------------------
// Runs inside the game's submit job, right after the dither fade and before the loop that reads the view masks, on the same thread: a zero written here is
// seen by that loop and by nothing racing it. Local third-person AMC only; masks of the head set only; only while a placement stands. Every access is
// under SEH; three faults stand head hiding down for the session (it never counts against Explorer Cam's own fault budget).
struct AmcIdent {
    uint32_t id = 0;
    uint64_t pose = 0;
    int32_t mode = -1;
};
__declspec(noinline) bool sehReadAmcIdent(const uint8_t* amc, AmcIdent* o) noexcept {
    __try {
        std::memcpy(&o->id, amc + ecm::kOffAmcAvatarPoseId, 4);
        std::memcpy(&o->pose, amc + ecm::kOffAmcAvatarPose, 8);
        uint64_t params = 0;
        std::memcpy(&params, amc + ecm::kOffAmcParams, 8);
        o->mode = -1;
        if (params >= 0x10000u && params < 0x00007FFF00000000ull) std::memcpy(&o->mode, reinterpret_cast<const uint8_t*>(params) + ecm::kOffParamsMode, 4);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
// Zero the four view masks of every head part. `zeroed` counts the words that were not already zero.
__declspec(noinline) bool sehZeroHeadMasks(uint8_t* amc, uint32_t* zeroed) noexcept {
    __try {
        uint32_t n = 0;
        for (uint32_t i = 0; i < ecm::kHeadPartCount; ++i) {
            volatile uint64_t* masks = reinterpret_cast<volatile uint64_t*>(amc + ecm::kOffAmcMasks + ecm::kHeadParts[i] * ecm::kPartStride);
            for (uint32_t k = 0; k < ecm::kPartVariants; ++k) {
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
__declspec(noinline) bool sehReadCensus(const uint8_t* amc, ecm::AmcCensus* c) noexcept {
    __try {
        std::memcpy(&c->viewFilter, amc + ecm::kOffAmcViewFilter, 4);
        std::memcpy(&c->alpha, amc + ecm::kOffAmcAlpha, 4);
        std::memcpy(&c->context, amc + ecm::kOffAmcContext, 4);
        std::memcpy(c->flagBytes, amc + ecm::kOffAmcFlagBytes, 4);
        uint32_t n = 0;
        for (uint32_t idx = 0; idx < ecm::kPartCount; ++idx) {
            const uint32_t at = idx * ecm::kPartStride;
            uint8_t variants = 0, maskBits = 0;
            for (uint32_t k = 0; k < ecm::kPartVariants; ++k) {
                uint64_t inst = 0, mask = 0;
                std::memcpy(&inst, amc + ecm::kOffAmcInstances + at + 8 * k, 8);
                std::memcpy(&mask, amc + ecm::kOffAmcMasks + at + 8 * k, 8);
                if (inst != 0) variants = static_cast<uint8_t>(variants | (1u << k));
                if (mask != 0 && inst != 0) maskBits = static_cast<uint8_t>(maskBits | (1u << k));
            }
            if (variants == 0) continue;
            ecm::PartCensus& p = c->part[n++];
            p.idx = static_cast<uint8_t>(idx);
            p.variants = variants;
            p.maskBits = maskBits;
            p.flags = amc[ecm::kOffAmcPartFlags + at];
            std::memcpy(&p.state, amc + ecm::kOffAmcPartState + at, 4);
            std::memcpy(&p.pending, amc + ecm::kOffAmcPartPending + at, 4);
            std::memcpy(&p.current, amc + ecm::kOffAmcPartCurrent + at, 4);
        }
        c->count = n;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
// The second witness (log only): the AMC's world origin the way the fade code reads it, against the commander root of the free camera's two poses.
std::atomic<uint64_t> g_witnessNextMs{0};
std::atomic<uint32_t> g_witnessIntervalMs{1000}, g_witnessUnrooted{0}, g_witnessFaults{0};
std::atomic<bool> g_busyWitness{false};
ecm::Ring<ecm::AmcWitness, 4> g_witnessRing;
bool plausiblePointer(uint64_t p) noexcept { return p >= 0x10000u && p < 0x00007FFF00000000ull; }
// rcx = *(AMC+0x208); rdx = *rcx; call [rdx+0x20] returns a matrix pointer whose origin is at +0x30 (FUN 0x3DD6040 at 0x3DD61A2..0x3DD61AF, same call).
// 1 = read, 0 = not available (a pointer along the chain is null or implausible: nothing was called), -1 = a fault.
__declspec(noinline) int sehAmcOrigin(const uint8_t* amc, float* out3) noexcept {
    __try {
        uint64_t obj = 0, vt = 0, fn = 0;
        std::memcpy(&obj, amc + ecm::kOffAmcTransform, 8);
        if (!plausiblePointer(obj)) return 0;
        std::memcpy(&vt, reinterpret_cast<const void*>(obj), 8);
        if (!plausiblePointer(vt)) return 0;
        std::memcpy(&fn, reinterpret_cast<const void*>(vt + ecm::kTransformMatrixSlot), 8);
        if (!plausiblePointer(fn)) return 0;
        const uint64_t m = reinterpret_cast<uint64_t(__fastcall*)(uint64_t)>(fn)(obj);
        if (!plausiblePointer(m)) return 0;
        std::memcpy(out3, reinterpret_cast<const void*>(m + ecm::kMatrixOriginOff), 12);
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}
__declspec(noinline) bool sehReadPoses(const uint8_t* activity, float* local, float* world) noexcept {
    __try {
        std::memcpy(local, activity + ecm::kOffLocalPose, 64);
        std::memcpy(world, activity + 0x70, 64);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
void witnessStep(uint8_t* amc) noexcept {
    if (g_witnessFaults.load(std::memory_order_relaxed) >= 3) return;   // three faults in the transform call: the witness stops (head hiding does not)
    ecm::AmcWitness w;
    w.amc = reinterpret_cast<uint64_t>(amc);
    {
        const int got = sehAmcOrigin(amc, w.origin);
        if (got > 0) w.flags |= 1u;
        else if (got < 0) g_witnessFaults.fetch_add(1, std::memory_order_relaxed);   // three real faults stop the witness; "not available" does not count
    }
    const uint64_t activity = g_trackedActivity.load(std::memory_order_relaxed);
    float local[16], world[16];
    if (activity != 0 && sehReadPoses(reinterpret_cast<const uint8_t*>(activity), local, world)) {
        const ecm::CommanderFrame cf = ecm::commanderFrame(local, world);
        if (cf.valid) {
            w.flags |= 2u;
            std::memcpy(w.root, cf.root, sizeof(w.root));
        }
        std::memcpy(w.camera, world + 12, sizeof(w.camera));
        w.flags |= 4u;
    }
    // On foot, outside the free camera, there is no root to compare with: say so a couple of times, then only the rooted ones.
    if ((w.flags & 2u) == 0 && g_witnessUnrooted.fetch_add(1, std::memory_order_relaxed) >= 2) return;
    g_witnessRing.push(w);
}
void hideFault() noexcept {
    if (g_hideFaults.fetch_add(1, std::memory_order_relaxed) + 1 >= ecm::kMaxHideFaults) g_hideDown.store(true, std::memory_order_release);
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
    if (!ecm::isLocalAmc(id.id, id.pose, skeleton, id.mode)) return;
    g_amcLocalMatches.fetch_add(1, std::memory_order_relaxed);
    g_amcLocal.store(reinterpret_cast<uint64_t>(amcPtr), std::memory_order_relaxed);
    g_amcLocalPose.store(id.pose, std::memory_order_relaxed);
    // The census, once per local AMC (a new AMC, a new census). One thread at a time takes it.
    if (reinterpret_cast<uint64_t>(amcPtr) != g_amcCensused && !g_busyAmc.exchange(true, std::memory_order_acquire)) {
        if (reinterpret_cast<uint64_t>(amcPtr) != g_amcCensused) {
            ecm::AmcCensus census;
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
    if (g_hideDown.load(std::memory_order_acquire)) return;
    {
        const uint64_t now = GetTickCount64();
        if (now >= g_witnessNextMs.load(std::memory_order_relaxed) && !g_busyWitness.exchange(true, std::memory_order_acquire)) {
            g_witnessNextMs.store(now + g_witnessIntervalMs.load(std::memory_order_relaxed), std::memory_order_relaxed);
            witnessStep(amc);
            g_busyWitness.store(false, std::memory_order_release);
        }
    }
    if (g_placedActivity.load(std::memory_order_acquire) == 0) return;
    uint32_t zeroed = 0;
    if (!sehZeroHeadMasks(amc, &zeroed)) {
        hideFault();
        return;
    }
    g_hideCalls.fetch_add(1, std::memory_order_relaxed);
    g_hideZeroed.fetch_add(zeroed, std::memory_order_relaxed);
}

// ---- the avatar-fade hook (the probe's fade counter rides it; nothing here writes) ---------------------------------------------
__declspec(noinline) uint64_t __fastcall avatarFadeHooked(void* a, void* b, void* c, void* d) noexcept {
    const auto forward = reinterpret_cast<ForwardFn>(g_forward[kHkFade].load(std::memory_order_acquire));
    if (!forward) return 0;
    const uint64_t result = forward(a, b, c, d);   // the original FIRST: the observers read what it left, and the hide follows it
    if (g_hideOn.load(std::memory_order_acquire)) headHideStep(a);
    runObservers(2, a);
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
        case kHkFade: return reinterpret_cast<const void*>(&avatarFadeHooked);
        case kHkFind: return reinterpret_cast<const void*>(&findJointHooked);
        case kHkZoom: return reinterpret_cast<const void*>(&zoomDofHooked);
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
        case kHkFade:
            return "LOG ONLY, for advanced.explorer_cam_probe's fade counter: the original runs first, then the component's dither block is read";
        case kHkFind:
            return "LOG ONLY: the original runs first and its result is returned unchanged; a call from one of the two avatar-attach sites with the "
                   "povCamera literal stores the skeleton interface (rcx) and the index, for head hiding and the probe's H";
        case kHkZoom:
            return "ISOLATION only: while a session has placed the view, the zoom, aperture and focus action objects (+0x250..+0x280) are cleared before the "
                   "original and put back after it";
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
        case kHkFade: return "head hiding does not run, and advanced.explorer_cam_probe's fade counter does not either.";
        case kHkFind: return "head hiding and the head-joint eye do not run (the fixed eye keys place the view), and advanced.explorer_cam_probe's H does not either.";
        case kHkZoom: return "the zoom, aperture and focus keys are not blocked while the view is placed; everything else runs.";
        default: return "the camera UI stays up while placed; everything else runs.";
    }
}

void tryHook(int id, const ecm::Sink& sink) {
    const HookSpec& spec = kSpec[id];
    HookEntry& entry = g_hooks[id];
    char why[400] = {};
    if (installHook(id, why, sizeof(why))) {
        entry.state = ExplorerCamHookStatus::Armed;
        if (id == kHkFind) {
            // The image base is the target minus its RVA (production: target = base + RVA; the rigs place their synthetic functions the same way).
            const uintptr_t base = entry.target - ecm::kFindJointRva;
            g_findSite[0].store(base + ecm::kFindSite1Rva, std::memory_order_relaxed);
            g_findSite[1].store(base + ecm::kFindSite2Rva, std::memory_order_relaxed);
            g_findPov.store(base + ecm::kPovNameRva, std::memory_order_release);
        }
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

int observerCount(int which) {   // 0 free camera, 1 controller, 2 avatar fade
    int n = 0;
    for (int i = 0; i < kExplorerCamMaxObservers; ++i) n += g_observers[which][i].load(std::memory_order_relaxed) != nullptr ? 1 : 0;
    return n;
}

void updateGates() {
    const bool active = g_placeActive.load(std::memory_order_relaxed);
    g_gate[kHkFree].store(armed(kHkFree) && (active || observerCount(0) > 0) ? 1u : 0u, std::memory_order_release);
    g_gate[kHkCtl].store(armed(kHkCtl) && (active || observerCount(1) > 0 || g_fadeOurs.load(std::memory_order_relaxed)) ? 1u : 0u,
                         std::memory_order_release);
    // Head hiding needs both optional hooks, the part names verified, and a placement-capable Explorer Cam; it also needs the FindJoint capture from
    // launch (the attach runs every frame, but the hook must be in place), so the capture's share of the gate is open whenever Explorer Cam is active.
    const bool hideOn = active && armed(kHkFade) && armed(kHkFind) && g_partNames.load(std::memory_order_relaxed) == 1 && !g_hideDown.load(std::memory_order_relaxed);
    g_hideOn.store(hideOn, std::memory_order_release);
    if (active) g_findWant.fetch_or(2u, std::memory_order_relaxed);
    else g_findWant.fetch_and(~2u, std::memory_order_relaxed);
    g_gate[kHkFade].store(armed(kHkFade) && (observerCount(2) > 0 || hideOn) ? 1u : 0u, std::memory_order_release);
    g_gate[kHkFind].store(armed(kHkFind) && g_findWant.load(std::memory_order_relaxed) != 0 ? 1u : 0u, std::memory_order_release);
    g_gate[kHkUi].store(armed(kHkUi) && (active || g_uiHeld.load(std::memory_order_relaxed)) ? 1u : 0u, std::memory_order_release);
    g_gate[kHkZoom].store(armed(kHkZoom) && active ? 1u : 0u, std::memory_order_release);
}

// ---- the frame thread ----------------------------------------------------------------------------------------------------------
// Everything the frame thread reads from the world in one frame, so the rig can script it. The production wrapper fills it from
// Config, the Hotkey, the journal and the Elite bindings; the rig fills it by hand.
struct FrameInput {
    bool on = true;
    float up = ecm::kEyeUpDefault, forward = ecm::kEyeForwardDefault, right = ecm::kEyeRightDefault;
    float trimRight = 0.0f, trimUp = 0.0f, trimForward = 0.0f, smoothingMs = 0.0f;   // the temporary follow keys
    const char* hotkey = "";
    bool f5Pressed = false;
    bool gameplay = true;
    bool onFootKnown = false, onFoot = false;
    bool focusKnown = false;             // Status.json GuiFocus
    uint32_t focus = 0;
    bool readBindings = true;
    const wchar_t* bindsDir = nullptr;   // null: the live Elite bindings directory
    bool comfortFade = true;             // production: always (no key). The legacy rig cells run without it, so F5's request goes out at once
    uint64_t nowUs = 0;                  // a finer clock for the fade's ramps (0: nowMs)
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

// ---- the avatar fade global (frame thread) -------------------------------------------------------------------------------------------
bool fadeResolve() {
    if (g_fadeMode) return true;
#ifdef EDVR_EXPLORER_CAM_TEST
    if (g_testFadeMode) {
        g_fadeMode = g_testFadeMode;
        g_fadeAmount = g_testFadeAmount;
        return true;
    }
#endif
    uintptr_t base = 0;
    char why[160] = {};
    if (!explorerCamBuildKnown(&base, why, sizeof(why))) return false;   // an unknown build: the global is not touched
    g_fadeMode = reinterpret_cast<int32_t*>(base + ecm::kFadeModeRva);
    g_fadeAmount = reinterpret_cast<float*>(base + ecm::kFadeAmountRva);
    return true;
}
bool fadeStore(int32_t value) {
    if (!writableRange(g_fadeMode, 4)) return false;
    return sehWriteInt32(g_fadeMode, value);
}

// One evaluation: write 0 while a placement stands (only if the global reads -1), put -1 back once the session is over or the placement was
// released for a detach AND the camera is closed or detached (never while it is still inside the body), and at unload.
void fadeTick(bool active, bool placed, bool session, uint32_t frame, bool unload, const ecm::Sink& sink) {
    if (!fadeResolve()) return;
    ecm::FadeIn in;
    in.active = active;
    in.placed = placed;
    in.session = session && active;
    in.ctlMode = static_cast<uint8_t>(g_ctlMode.load(std::memory_order_relaxed));
    in.unload = unload;
    if (g_fade.ours() || (active && placed) || unload) in.readOk = sehReadInt32(g_fadeMode, &in.value);
    const ecm::FadeStep st = g_fade.step(in);
    ecm::FadeEvent ev = st.ev;
    float amount = 0.0f;
    if (st.act == ecm::FadeStep::Act::Write) {
        sehReadFloat(g_fadeAmount, &amount);
        if (!fadeStore(0)) {
            g_fade.writeFailed();
            g_faults.fetch_add(1, std::memory_order_relaxed);
            ev = ecm::FadeEvent::WriteFailed;
        }
    } else if (st.act == ecm::FadeStep::Act::Restore) {
        if (!fadeStore(ecm::kFadeAuto)) {
            g_fade.restoreFailed();
            g_faults.fetch_add(1, std::memory_order_relaxed);
            ev = ecm::FadeEvent::RestoreFailed;
        }
    }
    g_fadeOurs.store(g_fade.ours(), std::memory_order_release);
    if (ev != ecm::FadeEvent::None) {
        char line[ecm::kLineBytes];
        ecm::formatFade(line, sizeof(line), ev, st.seen, in.ctlMode, amount, frame);
        sink(line);
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
void checkPartNames(const ecm::Sink& sink) {
    if (g_partNames.load(std::memory_order_relaxed) != 0 || !armed(kHkFade)) return;
    const uintptr_t base = g_hooks[kHkFade].target - ecm::kAvatarFadeRva;
    uint32_t bad = 0, first = 0;
    for (uint32_t i = 0; i < ecm::kPartCount; ++i) {
        if (sehNameIs(base + ecm::kPartNameTableRva + 8u * i, ecm::kPartNames[i])) continue;
        if (bad++ == 0) first = i;
    }
    if (bad == 0) {
        g_partNames.store(1, std::memory_order_release);
        std::string heads;
        for (uint32_t h : ecm::kHeadParts) {
            if (!heads.empty()) heads += " ";
            heads += ecm::kPartNames[h];
        }
        say(sink, "%s the part-name table at EliteDangerous64.exe+0x%llX holds the %u names the hide list was written against; while a placement stands the view "
                  "masks of these %u head parts of the LOCAL third-person avatar are zeroed after the dither fade (nothing is restored: the game rewrites the masks "
                  "every frame): %s",
            ecm::prefixHide(), static_cast<unsigned long long>(ecm::kPartNameTableRva), ecm::kPartCount, ecm::kHeadPartCount, heads.c_str());
    } else {
        g_partNames.store(2, std::memory_order_release);
        say(sink, "%s the part-name table at EliteDangerous64.exe+0x%llX differs from the one the hide list was written against (%u of %u names; the first is index %u, "
                  "which is not \"%s\"): head hiding does not run and nothing is hidden",
            ecm::prefixHide(), static_cast<unsigned long long>(ecm::kPartNameTableRva), bad, ecm::kPartCount, first, ecm::kPartNames[first]);
    }
}
// The census (once per local AMC, from the hook thread), and the verdict on the -0x30 relation that picks the local AMC out.
void reportAmc(FrameState& fs, const ecm::Sink& sink) {
    ecm::AmcCensus census;
    while (g_censusRing.take(&census)) ecm::formatAmcCensus(census, sink);
    ecm::AmcWitness witness;
    while (g_witnessRing.take(&witness)) {
        char wline[ecm::kLineBytes];
        ecm::formatWitness(wline, sizeof(wline), witness);
        sink(wline);
    }
    // The latched local skeleton pair: one line whenever the third-person one changes (it should be stable for an on-foot session).
    const uint64_t third = g_skelIface[0].load(std::memory_order_acquire);
    if (third != fs.latchedThirdSeen) {
        say(sink, "%s the local avatar's skeleton pair %s: third-person 0x%llX (was 0x%llX), first-person 0x%llX; latched from a humanoid that attached BOTH avatars on one "
                  "thread (a site-1 attach then a site-2 attach at one stack location; NPCs have no first-person avatar); latches %u, raw attaches seen: site 1 %u, "
                  "site 2 %u (every humanoid's)",
            ecm::prefixHide(), third ? "changed" : "was dropped", static_cast<unsigned long long>(third), static_cast<unsigned long long>(fs.latchedThirdSeen),
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
            ecm::prefixHide(), static_cast<unsigned long long>(g_amcLocal.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(g_amcLocalPose.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(g_amcLocalPose.load(std::memory_order_relaxed) - ecm::kAvatarPoseToSkeleton));
    } else if (matches == 0 && !fs.amcNoMatchSaid && g_amcCalls.load(std::memory_order_relaxed) >= ecm::kAmcNoMatchCalls) {
        fs.amcNoMatchSaid = true;
        if (skeleton == 0)
            say(sink, "%s no AMC can be tested yet: %llu avatar fade calls were seen but no local skeleton pair is latched (no humanoid attached BOTH a third-person and a "
                      "first-person avatar on one thread since launch), so the local skeleton is unknown; nothing is hidden. (Explorer Cam must be on at launch, and the "
                      "avatars attached after it.)",
                ecm::prefixHide(), static_cast<unsigned long long>(g_amcCalls.load(std::memory_order_relaxed)));
        else
            say(sink, "%s NO AMC matched the local skeleton in %llu avatar fade calls (%llu with a resolved AvatarPose handle, %llu of those in mode 3; the last such "
                      "AMC 0x%llX has +0x268 = 0x%llX, minus 0x30 = 0x%llX; the latched local skeleton is 0x%llX): the relation +0x268 - 0x30 == skeleton does not hold "
                      "here, so nothing is hidden",
                ecm::prefixHide(), static_cast<unsigned long long>(g_amcCalls.load(std::memory_order_relaxed)),
                static_cast<unsigned long long>(g_amcIdOk.load(std::memory_order_relaxed)), static_cast<unsigned long long>(g_amcMode3.load(std::memory_order_relaxed)),
                static_cast<unsigned long long>(g_amcCand.load(std::memory_order_relaxed)), static_cast<unsigned long long>(g_amcCandPose.load(std::memory_order_relaxed)),
                static_cast<unsigned long long>(g_amcCandPose.load(std::memory_order_relaxed) - ecm::kAvatarPoseToSkeleton), static_cast<unsigned long long>(skeleton));
    }
    if (g_hideDown.load(std::memory_order_relaxed) && !fs.amcDownSaid) {
        fs.amcDownSaid = true;
        say(sink, "%s stood down for the session: %u guarded accesses of the avatar component faulted; the head parts are no longer touched", ecm::prefixHide(),
            g_hideFaults.load(std::memory_order_relaxed));
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
    g_resetRequest.fetch_add(1, std::memory_order_release);   // the placement machine only: the F5 session carries on
}

void endSessionFromFrame(const char* why, const ecm::Sink& sink) {
    g_sessionActive.store(false, std::memory_order_release);
    g_exiting.store(false, std::memory_order_release);
    g_f5Request.store(0, std::memory_order_release);
    g_resetRequest.fetch_add(1, std::memory_order_release);
    g_resetCtlRequest.fetch_add(1, std::memory_order_release);
    say(sink, "%s the Explorer Cam session ended: %s", ecm::prefix(), why);
}

// ---- Phase 3, the frame thread's half: arming the head source, and what the hook threads said about it -----------------------------------------------
#ifdef EDVR_EXPLORER_CAM_TEST
uintptr_t g_testHeadBase = 0;   // the rig's synthetic game image (0: the real module)
size_t g_testHeadSize = 0;
#endif
// Once per session, when Explorer Cam first becomes active: establish the build's addresses, check the two joint-name literals, and open the hook's side.
void followArm(FrameState& fs, const ecm::Sink& sink) {
    if (fs.followArmTried) return;
    fs.followArmTried = true;
    uintptr_t base = 0;
    size_t size = 0;
    char why[300] = {};
    bool known = false;
#ifdef EDVR_EXPLORER_CAM_TEST
    if (g_testHeadBase) {
        base = g_testHeadBase;
        size = g_testHeadSize;
        known = true;
    }
#endif
    if (!known) {
        known = explorerCamBuildKnown(&base, why, sizeof(why));
        size = known ? static_cast<size_t>(ecm::kExpectedImageSize) : 0;   // the identity check just matched the image size
    }
    if (!known) {
        say(sink, "%s the head-joint source is not armed: %s; the fixed eye keys (fix.explorer_cam_eye_up, _eye_forward, _eye_right) place the view",
            ecm::prefixFollow(), why[0] ? why : "the game build is unknown");
        return;
    }
    const ecm::HeadTargets t = ecm::headTargetsFromBase(base, size);
    if (!sehCheckBytes(t.headName, reinterpret_cast<const uint8_t*>(ecm::kHeadName), sizeof(ecm::kHeadName)) ||
        !sehCheckBytes(t.povName, reinterpret_cast<const uint8_t*>(ecm::kPovName), sizeof(ecm::kPovName))) {
        say(sink, "%s the head-joint source is not armed: the joint-name literals at EliteDangerous64.exe+0x%llX / +0x%llX are not \"%s\" / \"%s\" (a different build); "
                  "the fixed eye keys place the view",
            ecm::prefixFollow(), static_cast<unsigned long long>(ecm::kHeadNameRva), static_cast<unsigned long long>(ecm::kPovNameRva), ecm::kHeadName, ecm::kPovName);
        return;
    }
    g_followT = t;
    g_followReady.store(true, std::memory_order_release);
    say(sink, "%s armed (build 332841): while a session has placed the view, each placing update reads the head joint of the LOCAL third-person skeleton (the FindJoint "
              "capture's latched pair) through the skeleton interface's +0x58 slot on the camera-job thread, after checking the vtable (RR +0x%llX or AO +0x%llX) and the "
              "slots +0x18 +0x30 +0x58 before every call; nothing is written to the skeleton. The eye is the head joint + the head's rotation x the rest-pose offset to the "
              "povCamera joint + the trims, with no smoothing unless fix.explorer_cam_follow_smoothing_ms is set. The fixed eye keys serve only when no head joint is "
              "readable (nothing latched, a fault, an implausible matrix; %u faults stand the head source down, never the placement)",
        ecm::prefixFollow(), static_cast<unsigned long long>(ecm::kRrVtableRva), static_cast<unsigned long long>(ecm::kAoVtableRva), ecm::kMaxFollowFaults);
}
// What the hook threads said about the head source and the isolation, in order.
void reportFollow(FrameState& fs, const ecm::Sink& sink) {
    ecm::FollowNote n;
    char line[ecm::kLineBytes];
    while (g_followNotes.take(&n)) {
        ecm::formatFollowNote(line, sizeof(line), n);
        sink(line);
    }
    if (g_isoDown.load(std::memory_order_relaxed) && !fs.isoDownSaid) {
        fs.isoDownSaid = true;
        say(sink, "%s isolation stood down for the session: %u guarded accesses of the camera suite's action objects faulted; the suite's own keys work again and the "
                  "placement goes on",
            ecm::prefix(), g_isoFaults.load(std::memory_order_relaxed));
    }
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
    {
        // The temporary follow keys: the trims added after the joint, and the comfort smoothing (0 = exact follow).
        ecm::Trim trim;
        trim.right = ecm::clampTrim(in.trimRight);
        trim.up = ecm::clampTrim(in.trimUp);
        trim.forward = ecm::clampTrim(in.trimForward);
        const float smooth = ecm::clampSmoothingMs(in.smoothingMs);
        g_trimRight.store(trim.right, std::memory_order_relaxed);
        g_trimUp.store(trim.up, std::memory_order_relaxed);
        g_trimForward.store(trim.forward, std::memory_order_relaxed);
        g_followSmoothMs.store(smooth, std::memory_order_relaxed);
        const bool changed = trim.right != fs.lastTrim.right || trim.up != fs.lastTrim.up || trim.forward != fs.lastTrim.forward || smooth != fs.lastSmooth;
        const bool nonDefault = trim.right != 0.0f || trim.up != 0.0f || trim.forward != 0.0f || smooth > 0.0f;
        if (on && changed && (nonDefault || fs.followCfgSaid)) {
            fs.followCfgSaid = true;
            say(sink, "%s trims right=%.3f up=%.3f forward=%.3f m (fix.explorer_cam_eye_trim_right, _up, _forward; live, +-%.1f, in the commander's axes, added after the head "
                      "joint), follow smoothing %.0f ms (fix.explorer_cam_follow_smoothing_ms; live, 0 = exact follow); all temporary",
                ecm::prefixFollow(), trim.right, trim.up, trim.forward, ecm::kTrimLimit, smooth);
        }
        fs.lastTrim = trim;
        fs.lastSmooth = smooth;
    }
    if (!fs.announced) {
        fs.announced = true;
        fs.lastOn = on;
        fs.lastEye = eye;
        fs.haveEye = true;
        if (on)
            say(sink, "%s Explorer Cam armed: %s enters it, on foot in first person or in the camera, and puts the view at your commander's head and locks it to them "
                      "(press it again to leave). The view follows the head joint when it can be read; the FALLBACK eye is up=%.3f forward=%.3f right=%.3f m in the "
                      "commander's frame from their feet (fix.explorer_cam_eye_up, _eye_forward, _eye_right; live). The game's own camera key and TAB place nothing, "
                      "and while the view is placed the camera's own keys are blocked.",
                ecm::prefix(), in.hotkey, eye.up, eye.forward, eye.right);
        else
            say(sink, "%s hotkey.explorer_cam is empty: Explorer Cam is off: nothing is installed for it and nothing is written to the game", ecm::prefix());
    } else {
        if (on != fs.lastOn) {
            fs.lastOn = on;
            if (on)
                say(sink, "%s Explorer Cam armed again (hotkey.explorer_cam = %s): fallback eye up=%.3f forward=%.3f right=%.3f", ecm::prefix(), in.hotkey, eye.up,
                    eye.forward, eye.right);
            else
                say(sink, "%s hotkey.explorer_cam was cleared while running: Explorer Cam is off, the session, if any, is ended, the camera's UI and the avatar fade are given "
                          "back and nothing more is written",
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
        // Head hiding's two hooks, at launch like the rest: the FindJoint capture must be in place before the on-foot avatars attach. Optional: a stand-down
        // here costs the head hiding (said once, with its consequence) and never placement.
        if (allArmed && g_hooks[kHkFind].state == ExplorerCamHookStatus::NotTried) tryHook(kHkFind, sink);
        if (allArmed && g_hooks[kHkFade].state == ExplorerCamHookStatus::NotTried) tryHook(kHkFade, sink);
        // The zoom/DOF update, for the isolation of the zoom, aperture and focus keys. Optional: a stand-down leaves those keys unblocked, never the placement.
        if (allArmed && g_hooks[kHkZoom].state == ExplorerCamHookStatus::NotTried) tryHook(kHkZoom, sink);
        checkPartNames(sink);
    }

    // 3. Active = on, every required hook armed, and the fault budget not spent. A change starts the hook threads' machines over.
    const bool active = on && armed(kHkFree) && armed(kHkCollision) && armed(kHkBox) && armed(kHkCtl) &&
                        g_faults.load(std::memory_order_relaxed) < ecm::kMaxFaults;
    if (active != g_placeActive.load(std::memory_order_relaxed)) {
        g_placeActive.store(active, std::memory_order_release);
        g_resetRequest.fetch_add(1, std::memory_order_release);
        g_resetCtlRequest.fetch_add(1, std::memory_order_release);
    }
    updateGates();
    if (active) followArm(fs, sink);

    // 4. The hotkey against the player's Elite bindings.
    if (on) checkHotkeyClash(fs, in, nowMs, sink);

    // 5. F5.
    fs.ctlLive.tick(g_ctlCalls.load(std::memory_order_relaxed));
    bool fadePressEnter = false, fadePressExit = false;
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
        fi.focusKnown = in.focusKnown;
        fi.focus = in.focus;
        const ecm::F5Action action = ecm::decideF5(fi);
        if (action != ecm::F5Action::None) {
            ecm::formatF5(line, sizeof(line), action, fi);
            sink(line);
            if (action == ecm::F5Action::Enter || action == ecm::F5Action::Exit) {
                if (in.comfortFade) {
                    // The comfort fade holds the request until the view is black (step 6c), so the first press is never seen.
                    fadePressEnter = action == ecm::F5Action::Enter;
                    fadePressExit = action == ecm::F5Action::Exit;
                } else {
                    g_f5Request.store(static_cast<uint32_t>(action == ecm::F5Action::Enter ? ecm::F5Req::Enter : ecm::F5Req::Exit),
                                      std::memory_order_release);
                    fs.f5SetFrame = frame;
                }
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

    // 6a. Head hiding: the census of the local AMC and the verdict on how it was found.
    reportAmc(fs, sink);
    reportFollow(fs, sink);

    // 6b. The avatar fade global. Before the not-active clean-up below, so a feature turned off while the camera is closed puts it back.
    fadeTick(active, g_placedActivity.load(std::memory_order_acquire) != 0, g_sessionActive.load(std::memory_order_acquire), frame, false, sink);
    updateGates();

    // 6c. The comfort fade: how black the view is this frame, and whether F5's request may go (not before the view is dark). Before the not-active return below, so
    // a stand-down or a cleared hotkey clears the view at once.
    if (in.comfortFade) {
        ecm::ComfortInputs fin;
        fin.nowUs = in.nowUs ? in.nowUs : nowMs * 1000ull;
        fin.active = active;
        fin.sessionActive = g_sessionActive.load(std::memory_order_acquire);
        fin.requestPending = g_f5Request.load(std::memory_order_acquire) != 0;
        fin.mode = static_cast<uint8_t>(g_ctlMode.load(std::memory_order_relaxed));
        fin.placed = g_placedActivity.load(std::memory_order_acquire) != 0;
        fin.state = static_cast<uint8_t>(g_lastState.load(std::memory_order_relaxed));
        fin.steady = g_steadyUpdates.load(std::memory_order_relaxed);
        fin.uiSettled = g_uiSettled.load(std::memory_order_acquire);
        fin.ctlCalls = g_ctlCalls.load(std::memory_order_relaxed);
        fin.pressEnter = fadePressEnter;
        fin.pressExit = fadePressExit;
        const ecm::ComfortStep st = fs.fade.step(fin);
        if (st.releaseEnter || st.releaseExit) {
            g_f5Request.store(static_cast<uint32_t>(st.releaseEnter ? ecm::F5Req::Enter : ecm::F5Req::Exit), std::memory_order_release);
            fs.f5SetFrame = frame;
        }
        for (uint8_t i = 0; i < st.nev; ++i) {
            ecm::formatComfort(line, sizeof(line), st.ev[i]);
            sink(line);
        }
        std::memcpy(&fs.fadeAlphaBits, &st.alpha, sizeof(fs.fadeAlphaBits));
        g_fadeAlphaBits.store(fs.fadeAlphaBits, std::memory_order_relaxed);
        comfort::publish(st.alpha, nowMs);
    } else {
        fs.fade.reset();
        g_fadeAlphaBits.store(0, std::memory_order_relaxed);
        comfort::publish(0.0f, nowMs);
    }
    if (!g_sessionActive.load(std::memory_order_acquire)) g_uiSettled.store(false, std::memory_order_release);

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
    // The isolation, said once per session.
    if (g_sessionActive.load(std::memory_order_acquire)) {
        if (!fs.isoSaid) {
            fs.isoSaid = true;
            ecm::formatIsolation(line, sizeof(line), armed(kHkZoom), in.hotkey);
            sink(line);
        }
    } else {
        fs.isoSaid = false;
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
        fs.beatHideCalls = g_hideCalls.load(std::memory_order_relaxed);
        fs.beatZeroed = g_hideZeroed.load(std::memory_order_relaxed);
        fs.beatHeadUpdates = g_headUpdates.load(std::memory_order_relaxed);
        fs.beatFixedUpdates = g_fixedUpdates.load(std::memory_order_relaxed);
        fs.beatZoomCalls = g_zoomCalls.load(std::memory_order_relaxed);
        for (int i = 0; i < ecm::kIsoHolderCount; ++i) fs.beatBlocked[i] = g_isoBlocked[i].load(std::memory_order_relaxed);
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
        h.fadeOurs = g_fadeOurs.load(std::memory_order_relaxed);
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
        h.latches = g_skelLatches.load(std::memory_order_relaxed);
        h.hide = g_hideDown.load(std::memory_order_relaxed) ? "stood down" : g_hideOn.load(std::memory_order_relaxed) ? "on" : "off";
        h.localAmc = g_amcLocal.load(std::memory_order_relaxed);
        h.amcCalls = g_amcCalls.load(std::memory_order_relaxed);
        h.amcMatches = g_amcLocalMatches.load(std::memory_order_relaxed);
        h.hideCalls = g_hideCalls.load(std::memory_order_relaxed);
        h.hideCallsWindow = h.hideCalls - fs.beatHideCalls;
        h.zeroed = g_hideZeroed.load(std::memory_order_relaxed);
        h.zeroedWindow = h.zeroed - fs.beatZeroed;
        h.hideFaults = g_hideFaults.load(std::memory_order_relaxed);
        ecm::formatHeartbeat(line, sizeof(line), h);
        sink(line);
        {
            // The second line: where the eye comes from, what the +0x58 call costs, and what the isolation swallowed.
            ecm::FollowBeatIn fb;
            fb.windowSeconds = h.windowSeconds;
            fb.source = g_followSource.load(std::memory_order_relaxed) ? "head-joint" : "fixed";
            fb.why = static_cast<ecm::FixedWhy>(g_followWhy.load(std::memory_order_relaxed));
            fb.lastEye.up = g_lastEye[0].load(std::memory_order_relaxed);
            fb.lastEye.forward = g_lastEye[1].load(std::memory_order_relaxed);
            fb.lastEye.right = g_lastEye[2].load(std::memory_order_relaxed);
            fb.headUpdates = g_headUpdates.load(std::memory_order_relaxed);
            fb.headWindow = fb.headUpdates - fs.beatHeadUpdates;
            fb.fixedUpdates = g_fixedUpdates.load(std::memory_order_relaxed);
            fb.fixedWindow = fb.fixedUpdates - fs.beatFixedUpdates;
            fb.headFaults = g_followFaults.load(std::memory_order_relaxed);
            fb.headDown = g_followDown.load(std::memory_order_relaxed);
            {
                const uint32_t n = g_t58.n.exchange(0, std::memory_order_relaxed);
                const uint32_t mn = g_t58.minUs.exchange(0xFFFFFFFFu, std::memory_order_relaxed);
                fb.t58n = n;
                fb.t58min = n ? mn : 0;
                fb.t58max = g_t58.maxUs.exchange(0, std::memory_order_relaxed);
                fb.t58allMax = g_t58.allMaxUs.load(std::memory_order_relaxed);
            }
            fb.trim.right = g_trimRight.load(std::memory_order_relaxed);
            fb.trim.up = g_trimUp.load(std::memory_order_relaxed);
            fb.trim.forward = g_trimForward.load(std::memory_order_relaxed);
            fb.smoothingMs = g_followSmoothMs.load(std::memory_order_relaxed);
            fb.isoDown = g_isoDown.load(std::memory_order_relaxed);
            fb.isoFaults = g_isoFaults.load(std::memory_order_relaxed);
            for (int i = 0; i < ecm::kIsoHolderCount; ++i) {
                fb.blocked[i] = g_isoBlocked[i].load(std::memory_order_relaxed);
                fb.blockedWindow[i] = fb.blocked[i] - fs.beatBlocked[i];
                fs.beatBlocked[i] = fb.blocked[i];
            }
            fb.zoomCalls = g_zoomCalls.load(std::memory_order_relaxed);
            fb.zoomCallsWindow = fb.zoomCalls - fs.beatZoomCalls;
            fb.zoomArmed = armed(kHkZoom);
            fb.fadePhase = ecm::comfortPhaseText(fs.fade.phase());
            fb.fadeKind = ecm::comfortKindText(fs.fade.kind());
            fb.fadeAlpha = fs.fade.alpha();
            ecm::formatFollowBeat(line, sizeof(line), fb);
            sink(line);
            fs.beatHeadUpdates = fb.headUpdates;
            fs.beatFixedUpdates = fb.fixedUpdates;
            fs.beatZoomCalls = fb.zoomCalls;
        }
        fs.beatUpdates = h.updates;
        fs.beatBypassed = h.bypassed;
        fs.beatForwarded = h.forwarded;
        fs.beatBoxBypassed = h.boxBypassed;
        fs.beatBoxForwarded = h.boxForwarded;
        fs.beatHookCalls = h.hookCalls;
        fs.beatCtlCalls = h.ctlCalls;
        fs.beatHideCalls = h.hideCalls;
        fs.beatZeroed = h.zeroed;
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
// The key actually watched: hotkey.explorer_cam, except that a change or a clearing made while a session is on waits for the session to end (the key is the exit).
ecm::HotkeyKeeper g_hotkeyKeeper;
ecm::Sink g_frameSink{&logSink, nullptr};   // the production wrapper's lines (the rig points it at a capture)

void applyHotkey(const std::string& name, const ecm::Sink& sink) {
    if (g_f5Configured && name == g_f5Name) return;
    g_f5Configured = true;
    g_f5Name = name;
    g_f5.setBinding(name.c_str());
    if (name.empty()) {
        // Nothing to say here: an empty hotkey IS "Explorer Cam is off", and the frame's own line says so.
    } else if (!g_f5.bound())   // a pad button or a HOTAS button is bound too, with key() == 0 (hotkey.h)
        say(sink, "%s hotkey.explorer_cam = \"%s\" bound nothing (the line above says why), so there is no way into Explorer Cam this session",
            ecm::prefix(), name.c_str());
    else
        say(sink, "%s hotkey bound: %s (vk 0x%02X, mods 0x%X); watched, never captured, and only while the game has the focus and the journal "
                  "says you are playing",
            ecm::prefix(), name.c_str(), g_f5.key(), g_f5.mods());
}

}  // namespace

void explorerCamFrameBoundary(uint32_t frameNo) {
    const ecm::Sink sink = g_frameSink;
    const Config& cfg = Config::get();
    FrameInput in;
    in.up = cfg.getFloat("fix.explorer_cam_eye_up", ecm::kEyeUpDefault);
    in.forward = cfg.getFloat("fix.explorer_cam_eye_forward", ecm::kEyeForwardDefault);
    in.right = cfg.getFloat("fix.explorer_cam_eye_right", ecm::kEyeRightDefault);
    in.trimRight = cfg.getFloat("fix.explorer_cam_eye_trim_right", 0.0f);
    in.trimUp = cfg.getFloat("fix.explorer_cam_eye_trim_up", 0.0f);
    in.trimForward = cfg.getFloat("fix.explorer_cam_eye_trim_forward", 0.0f);
    in.smoothingMs = cfg.getFloat("fix.explorer_cam_follow_smoothing_ms", 0.0f);
    static std::string hotkeyName;
    {
        bool deferred = false;
        const std::string desired = cfg.getString("hotkey.explorer_cam", "F5");
        hotkeyName = g_hotkeyKeeper.step(desired, g_sessionActive.load(std::memory_order_acquire), &deferred);
        if (deferred)
            say(sink, "%s hotkey.explorer_cam changed during Explorer Cam: %s stays the exit until you leave; the new value (%s) applies when the session ends", ecm::prefix(),
                hotkeyName.c_str(), g_hotkeyKeeper.pending().empty() ? "empty: Explorer Cam turns off" : g_hotkeyKeeper.pending().c_str());
    }
    in.hotkey = hotkeyName.c_str();
    in.on = !hotkeyName.empty();   // Explorer Cam is armed exactly when its hotkey is set
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
    {
        uint32_t focus = 0;
        in.focusKnown = journalGuiFocus(&focus);
        in.focus = focus;
    }
    in.readBindings = cfg.getBool("hotkey.read_game_bindings", true);
    {
        // The fade's ramps want a finer clock than GetTickCount64's 15 ms.
        static const uint64_t freq = [] {
            LARGE_INTEGER f;
            QueryPerformanceFrequency(&f);
            return static_cast<uint64_t>(f.QuadPart > 0 ? f.QuadPart : 1);
        }();
        LARGE_INTEGER t;
        QueryPerformanceCounter(&t);
        in.nowUs = static_cast<uint64_t>(t.QuadPart) * 1000000ull / freq;
    }
    boundaryAt(frameNo, GetTickCount64(), in, sink);
}

void explorerCamShutdown() {
    comfort::clear();   // an unload: the runtime reads no fade from here on
    // An unload (FreeLibrary): the dither-fade global goes back to -1 if EDVR still holds it at 0. A process exit does not come here, and
    // needs nothing: the global dies with the process.
    fadeTick(false, false, false, 0, true, ecm::Sink{&logSink, nullptr});
    g_fadeOurs.store(g_fade.ours(), std::memory_order_release);
}

const int32_t* explorerCamFadeGlobalAddress() { return fadeResolve() ? g_fadeMode : nullptr; }

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
    const int which = hook == ExplorerCamHook::AvatarFade ? 2 : hook == ExplorerCamHook::Controller ? 1 : 0;
    const int id = which == 2 ? kHkFade : which == 1 ? kHkCtl : kHkFree;
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

ExplorerCamHookStatus explorerCamWantFindJoint(bool want) {
    if (want && g_hooks[kHkFind].state == ExplorerCamHookStatus::NotTried) tryHook(kHkFind, ecm::Sink{&logSink, nullptr});
    if (want) g_findWant.fetch_or(1u, std::memory_order_relaxed);
    else g_findWant.fetch_and(~1u, std::memory_order_relaxed);
    updateGates();
    ExplorerCamHookStatus status;
    status.state = g_hooks[kHkFind].state;
    status.stolen = g_hooks[kHkFind].stolen;
    status.target = g_hooks[kHkFind].target;
    status.relay = reinterpret_cast<uintptr_t>(g_hooks[kHkFind].relay);
    std::snprintf(status.why, sizeof(status.why), "%s", g_hooks[kHkFind].why);
    return status;
}

ExplorerCamSkeleton explorerCamSkeleton(int site) {
    ExplorerCamSkeleton s;
    if (site < 0 || site > 1) return s;
    s.iface = g_skelIface[site].load(std::memory_order_acquire);
    s.index = g_skelIndex[site].load(std::memory_order_relaxed);
    s.captures = g_skelCaptures[site].load(std::memory_order_relaxed);
    s.latches = g_skelLatches.load(std::memory_order_relaxed);
    return s;
}
void explorerCamSkeletonDrop(int site, uint64_t iface) {
    if (site < 0 || site > 1) return;
    uint64_t expected = iface;
    g_skelIface[site].compare_exchange_strong(expected, 0, std::memory_order_acq_rel);
}
uint64_t explorerCamFindJointSeen() { return g_findSeen.load(std::memory_order_relaxed); }
bool explorerCamSessionActive() { return g_sessionActive.load(std::memory_order_acquire); }

#ifdef EDVR_EXPLORER_CAM_TEST
namespace explorercamtest {
void setSkeleton(int site, uint64_t iface, uint32_t index) {   // a latched pair member set by hand
    g_skelIndex[site].store(index);
    g_skelIface[site].store(iface);
    g_skelLatches.fetch_add(1);
}
void setWitnessInterval(uint32_t ms) { g_witnessIntervalMs.store(ms); g_witnessNextMs.store(0); }
bool headHideOn() { return g_hideOn.load(); }
bool headHideDown() { return g_hideDown.load(); }
int partNamesState() { return g_partNames.load(); }
uint64_t hideCalls() { return g_hideCalls.load(); }
uint64_t hideZeroed() { return g_hideZeroed.load(); }
uint64_t amcCalls() { return g_amcCalls.load(); }
uint64_t amcLocalMatches() { return g_amcLocalMatches.load(); }
uint64_t amcLocal() { return g_amcLocal.load(); }
void setTargets(const ExplorerCamTestTargets& t) {
    g_testTargets[kHkFree] = t.freeCamera;
    g_testTargets[kHkCollision] = t.collision;
    g_testTargets[kHkBox] = t.boxPush;
    g_testTargets[kHkUi] = t.cameraUi;
    g_testTargets[kHkCtl] = t.controller;
    g_testTargets[kHkFade] = t.avatarFade;
    g_testTargets[kHkFind] = t.findJoint;
    g_testTargets[kHkZoom] = t.zoomDof;
}
void setFadeGlobal(int32_t* mode, float* amount) {
    g_testFadeMode = mode;
    g_testFadeAmount = amount;
}
bool fadeOurs() { return g_fadeOurs.load(); }
void shutdown() { explorerCamShutdown(); }
void boundary(uint32_t frame, uint64_t nowMs, const ExplorerCamTestFrame& t, ExplorerCamSinkFn fn, void* ctx) {
    FrameInput in;
    in.on = t.hotkey && t.hotkey[0];   // armed exactly when the hotkey is set
    in.up = t.up;
    in.forward = t.forward;
    in.right = t.right;
    in.trimRight = t.trimRight;
    in.trimUp = t.trimUp;
    in.trimForward = t.trimForward;
    in.smoothingMs = t.smoothingMs;
    in.hotkey = t.hotkey;
    in.f5Pressed = t.f5Pressed;
    in.gameplay = t.gameplay;
    in.onFootKnown = t.onFootKnown;
    in.onFoot = t.onFoot;
    in.focusKnown = t.focusKnown;
    in.focus = t.focus;
    in.readBindings = t.readBindings;
    in.comfortFade = t.comfortFade;
    in.nowUs = t.nowUs;
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
void setHeadImage(uintptr_t base, size_t size) {
    g_testHeadBase = base;
    g_testHeadSize = size;
}
void forcePlaceActive(bool on) { g_placeActive.store(on); }
float fadeAlpha() {
    const uint32_t bits = g_fadeAlphaBits.load();
    float v = 0.0f;
    std::memcpy(&v, &bits, 4);
    return v;
}
uint32_t steadyUpdates() { return g_steadyUpdates.load(); }
bool uiSettled() { return g_uiSettled.load(); }
float comfortRead(uint64_t nowMs) { return comfort::read(nowMs); }
bool comfortDefaultOn() { return FrameInput().comfortFade; }
void setNowUs(uint64_t (*fn)()) { g_followNowUs = fn ? fn : &realNowUs; }
uint64_t followHeadUpdates() { return g_headUpdates.load(); }
uint64_t followFixedUpdates() { return g_fixedUpdates.load(); }
uint32_t followSource() { return g_followSource.load(); }
uint32_t followWhy() { return g_followWhy.load(); }
bool followReady() { return g_followReady.load(); }
bool followDown() { return g_followDown.load(); }
uint32_t followFaults() { return g_followFaults.load(); }
uint64_t isoBlocked(int holder) { return holder >= 0 && holder < ecm::kIsoHolderCount ? g_isoBlocked[holder].load() : 0; }
bool isoDown() { return g_isoDown.load(); }
uint32_t isoFaults() { return g_isoFaults.load(); }
uint64_t zoomCalls() { return g_zoomCalls.load(); }
bool zoomGateOpen() { return g_gate[kHkZoom].load() != 0; }
void setFrameSink(ExplorerCamSinkFn fn, void* ctx) { g_frameSink = fn ? ecm::Sink{fn, ctx} : ecm::Sink{&logSink, nullptr}; }
void zoomPreThenPost(void* object) {
    PreState ps = preZoom(object);
    postFor(ps);
}
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
    for (int w = 0; w < 3; ++w)
        for (int i = 0; i < kExplorerCamMaxObservers; ++i) g_observers[w][i].store(nullptr);
    for (int i = 0; i < 2; ++i) {
        g_skelIface[i].store(0);
        g_skelIndex[i].store(0xFFFF);
        g_skelCaptures[i].store(0);
        g_findSite[i].store(0);
    }
    g_skelLatches.store(0);
    t_site1 = Site1Seen();
    g_witnessNextMs.store(0);
    g_witnessIntervalMs.store(1000);
    g_witnessUnrooted.store(0);
    g_witnessFaults.store(0);
    g_busyWitness.store(false);
    {
        ecm::AmcWitness drop;
        while (g_witnessRing.take(&drop)) {}
    }
    g_findPov.store(0);
    g_findSeen.store(0);
    g_findWant.store(0);
    g_hideOn.store(false);
    g_hideDown.store(false);
    g_hideFaults.store(0);
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
        ecm::AmcCensus drop;
        while (g_censusRing.take(&drop)) {}
    }
    g_placedActivity.store(0);
    for (int i = 0; i < 2; ++i) {
        g_bypassed[i].store(0);
        g_forwarded[i].store(0);
    }
    g_placeActive.store(false);
    g_resetRequest.store(0);
    g_resetCtlRequest.store(0);
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
    g_fadeOurs.store(false);
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
    g_ctlSharedNoted = false;
    g_fade = ecm::FadeGuard();
    g_fadeMode = nullptr;
    g_fadeAmount = nullptr;
    g_testFadeMode = nullptr;
    g_testFadeAmount = nullptr;
    g_steadyUpdates.store(0);
    g_prevEyeValid.store(false);
    g_uiSettled.store(false);
    g_fadeAlphaBits.store(0);
    comfort::clear();
    g_trimRight.store(0.0f);
    g_trimUp.store(0.0f);
    g_trimForward.store(0.0f);
    g_followSmoothMs.store(0.0f);
    g_followReady.store(false);
    g_followDown.store(false);
    g_followFaults.store(0);
    g_headUpdates.store(0);
    g_fixedUpdates.store(0);
    g_followWhy.store(0);
    g_followSource.store(0);
    for (auto& e : g_lastEye) e.store(0.0f);
    g_t58.n.store(0);
    g_t58.minUs.store(0xFFFFFFFFu);
    g_t58.maxUs.store(0);
    g_t58.allMaxUs.store(0);
    {
        ecm::FollowNote dropNote;
        while (g_followNotes.take(&dropNote)) {}
    }
    g_followT = ecm::HeadTargets();
    g_fc = FollowCache();
    g_smoother = ecm::EyeSmoother();
    g_smoothLastUs = 0;
    g_followHeadLast = -1;
    g_followWhyLast = 0;
    g_followSwitchNotes = 0;
    g_testHeadBase = 0;
    g_testHeadSize = 0;
    g_followNowUs = &realNowUs;
    g_isoDown.store(false);
    g_isoFaults.store(0);
    for (auto& b : g_isoBlocked) b.store(0);
    g_zoomCalls.store(0);
    g_frame = FrameState();
    g_identity = 0;
    g_f5Configured = false;
    g_f5Name.clear();
    g_f5 = Hotkey();
    g_f5Missed = 0;
    g_hotkeyKeeper = ecm::HotkeyKeeper();
    g_frameSink = ecm::Sink{&logSink, nullptr};
}
}  // namespace explorercamtest
#endif

}  // namespace edvr
