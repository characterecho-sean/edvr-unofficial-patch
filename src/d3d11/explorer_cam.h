#pragma once
// Explorer Cam (hotkey.explorer_cam; docs\design-explorer-cam-free-camera-2026-10-07.md, "Phase 1a" and "Phase 1c").
//
// F5 is the only way in. Pressed on foot in first person, in the stock camera on any preset or in the free camera, it puts the
// view in the commander's head, facing their way, locked to them, with the camera's own UI hidden; pressed again it gives the UI
// back and closes the camera. The game's own camera key and TAB place nothing: placement needs a session F5 started. All of it
// goes through the game's own camera objects, and all of it is writes into game memory that Sean approved (D1, 2026-10-07):
//   the free camera's commander-local pose, and its two position edits (the collision sweep and the commander's box push) made
//   to return "no edit" for that one camera; and the game's own actions pressed for ONE update each -- the relative lock, the
//   camera UI's FreeCamToggleHUD, and the camera controller's PhotoCameraToggle and ToggleFreeCam.
// explorer_cam_core.h holds the decisions; this file's .cpp holds the five hooks, the config, the F5 key and the log.
//
// THE HOOKS (build 332841 only; any PE or prologue mismatch stands the feature down with one line and patches nothing):
//   +0x1071980  the free camera's update. Placement runs BEFORE the original (the pose write and the lock press), the lock's
//               pressed-int is restored AFTER it returns, then the observers.
//   +0x1091140  the camera's collision sweep, only ever called by that update.     } relays in machine code: return 0
//   +0x108F1B0  the commander's box push, only ever called by that update.        } when rcx is the placed activity
//   +0x47C7640  the camera UI's update: FreeCamToggleHUD pressed once per placement, and again to give the UI back.
//   +0x2DF14C0  the camera controller's update (vtable only): F5's sequence of presses, and the mode byte.
//   +0x3DD6040  the avatar's dither fade (rcx = the AvatarModelComponent), inside the submit job: HEAD HIDING runs in its post-call, zeroing the view masks
//               of the local third-person avatar's head parts while a placement stands. Optional: a stand-down costs the hiding, never placement.
//   +0xFDDB10  the skeleton interface's FindJoint (called by many systems): the capture of the skeletons the game attaches the local avatars to, which is
//               how the local third-person avatar is told from the others. Optional likewise.
//
// THREADS. The hooks run on whichever thread the game's job system calls the functions from. Each takes no lock (a try-flag keeps a
// second concurrent caller out), allocates nothing, writes no log line and calls nothing of the game's: it talks to the frame thread
// through atomics and a small event ring of its own. Config, the F5 key and the Elite bindings are read on the frame thread only,
// in explorerCamFrameBoundary.
#include <cstddef>
#include <cstdint>

namespace edvr {

// Once a frame at the Present boundary (device_hook.cpp's own tick, tkExplorerCam: it must run whatever vScreen installed, including transport-only), before explorerCamProbeFrameBoundary. Reads hotkey.explorer_cam (Explorer Cam is
// armed exactly when it is non-empty), the eye keys, polls F5, installs the hooks the first time they are wanted, publishes the settings to the hook threads,
// drains their events into the log, ends a session whose controller went silent, and writes the 5 s heartbeat while a session is on.
// Render thread. Never call it from inside a game hook.
void explorerCamFrameBoundary(uint32_t frameNo);

// ---- diagnostic observers (advanced.explorer_cam_probe, temporary) ----------------------------------------------------------------
// One target gets one CodeHook, so a diagnostic's observation of the same function rides this file's hook instead of installing its
// own. A hook can carry several observers (kMaxObservers); the next instrument -- the neck -- adds its own the same way.
enum class ExplorerCamHook : int { FreeCamera = 0, Controller = 1, AvatarFade = 2 };
struct ExplorerCamHookStatus {
    enum State : int { NotTried = 0, Armed = 1, StoodDown = 2 };
    int state = NotTried;
    size_t stolen = 0;       // Armed: the bytes of the prologue CodeHook moved
    uintptr_t target = 0;    // Armed: the hooked address
    uintptr_t relay = 0;     // Armed: the relay's address
    char why[400] = {};      // StoodDown: one sentence
};
// Called by the probe's own boundary (the frame thread). The observer runs after the original returns and after every press has
// been restored, with the object (rcx). `attach` false removes it. Installs the hook the first time anyone wants it. Returns the
// hook's status.
using ExplorerCamActivityObserver = void (*)(void* object) noexcept;
constexpr int kExplorerCamMaxObservers = 4;
ExplorerCamHookStatus explorerCamObserve(ExplorerCamHook hook, ExplorerCamActivityObserver observer, bool attach);

// The skeleton interface's FindJoint(name) (EliteDangerous64.exe+0xFDDB10, called by many systems). Explorer Cam hooks it (the original first, its result
// unchanged) to learn which skeleton interfaces the game attaches the local player's two avatars to: FUN 0x19B1240 calls FindJoint("def_c_povCamera_joint")
// from two sites for EVERY humanoid (F6: the last site-1 attach is usually an NPC's). Site 1 (returning to +0x19B12D5) is a humanoid's third-person avatar,
// site 2 its first-person avatar, which only the local player has. So the hook LATCHES a pair only when one thread makes a site-1 attach and then a site-2
// attach at the same stack location (one invocation of 0x19B1240 does both); sites 0 and 1 below are the latched local third-person and first-person
// skeletons. Head hiding and the probe's H use the latched pair only. The hook is installed when Explorer Cam is on or the probe asks.
struct ExplorerCamSkeleton {
    uint64_t iface = 0;       // the LATCHED local avatar's skeleton for this site (0 = none latched yet, or dropped as stale)
    uint32_t index = 0xFFFF;  // the povCamera joint index the original returned for it
    uint32_t captures = 0;    // RAW attaches seen at this site since launch: EVERY humanoid's, not only the local one
    uint32_t latches = 0;     // times a local pair was latched
};
ExplorerCamHookStatus explorerCamWantFindJoint(bool want);   // the probe's request: installs the hook the first time, opens or closes its share of the gate
ExplorerCamSkeleton explorerCamSkeleton(int site);
void explorerCamSkeletonDrop(int site, uint64_t iface);      // a captured interface that went stale: forgotten unless a newer capture replaced it
uint64_t explorerCamFindJointSeen();                         // every FindJoint call the hook has seen (proof it is alive)

// Is an Explorer Cam session on? True from F5's request until the session ends (the camera closed, F5 pressed again, or Explorer Cam stood down). An atomic read,
// safe from any thread: the hotkey menu uses it to lock the Explorer Cam key's row, because that key is also the way out.
bool explorerCamSessionActive();

// An unload (FreeLibrary): puts the avatar dither-fade global back to -1 if EDVR still holds it at 0. Frame-thread context; SEH-guarded.
void explorerCamShutdown();

// Where the avatar dither-fade mode global is (null when the build is not known): for the probe's fade counter, which reads it.
const int32_t* explorerCamFadeGlobalAddress();

// The game image base, when its PE identity (timestamp and size) is build 332841's; false, with a sentence in `why`, otherwise. The
// answer is cached for the session. For the F2 instruments, which patch a vtable slot instead of a function.
bool explorerCamBuildKnown(uintptr_t* base, char* why, size_t whyCap);

#ifdef EDVR_EXPLORER_CAM_TEST
// The rigs' seam (tools\explorer_cam_test, tools\explorer_cam_probe_test): the same boundary with a scripted clock and scripted
// inputs, synthetic functions in place of the game's, and the shared state to look at.
using ExplorerCamSinkFn = void (*)(void* ctx, const char* line);
struct ExplorerCamTestTargets {
    uintptr_t freeCamera = 0, collision = 0, boxPush = 0, cameraUi = 0, controller = 0, avatarFade = 0, findJoint = 0, zoomDof = 0;
};
struct ExplorerCamTestFrame {
    float up = 1.68f, forward = 0.10f, right = 0.0f;   // the FALLBACK eye (fix.explorer_cam_eye_up/_forward/_right)
    float trimRight = 0.0f, trimUp = 0.0f, trimForward = 0.0f;   // fix.explorer_cam_eye_trim_right/_up/_forward (user settings; 0 here is the rig's neutral, not the shipped value)
    float smoothingMs = 0.0f;             // fix.explorer_cam_follow_smoothing_ms (a user setting)
    const char* hotkey = "F5";            // hotkey.explorer_cam: Explorer Cam is armed exactly when this is non-empty
    bool f5Pressed = false;               // the key's edge this frame
    bool gameplay = true;
    bool onFootKnown = true, onFoot = true;
    bool focusKnown = true;               // Status.json GuiFocus is in the file
    uint32_t focus = 0;                   // ...and its value (0 = no panel)
    bool readBindings = true;             // hotkey.read_game_bindings
    const wchar_t* bindsDir = nullptr;    // an Elite bindings directory for the clash check (null: none, unchecked)
    bool comfortFade = false;             // the comfort fade (production: always on). Off, F5's request goes out the frame it is pressed, as the older cells expect
    uint64_t nowUs = 0;                   // the fade's clock in microseconds (0: nowMs * 1000)
};
namespace explorercamtest {
void setTargets(const ExplorerCamTestTargets& targets);
void setFadeGlobal(int32_t* mode, float* amount);   // the synthetic dither-fade global and its amount float (null: the build's real addresses)
bool fadeOurs();                                    // EDVR holds the global at 0
void shutdown();                                    // explorerCamShutdown()
void boundary(uint32_t frame, uint64_t nowMs, const ExplorerCamTestFrame& in, ExplorerCamSinkFn fn, void* ctx);
bool gateOpen();                   // the free-camera relay's gate
bool uiGateOpen();
bool controllerGateOpen();
size_t stolenBytes(int hook);      // 0 free, 1 collision, 2 box push, 3 camera UI, 4 controller, 5 avatar fade, 6 FindJoint, 7 zoom/DOF
uint64_t placedActivity();
uint64_t bypassed();               // the collision sweep
uint64_t forwarded();
uint64_t boxBypassed();
uint64_t boxForwarded();
uint64_t updatesPlaced();
uint64_t hookCalls();
uint64_t controllerCalls();
uint32_t controllerMode();
uint64_t uiCalls();
bool uiHiddenByEdvr();
uint32_t faults();
void setSkeleton(int site, uint64_t iface, uint32_t index);   // a latched pair member set by hand (the cells that need no FindJoint hook)
void setWitnessInterval(uint32_t ms);                          // the witness measures every N ms (the rigs: 0)
bool headHideOn();                 // the fade hook's post-call hides the head parts of the local AMC (Explorer Cam on, hooks armed, names verified)
bool headHideDown();               // stood down for the session (guarded accesses faulted)
int partNamesState();              // 0 not checked, 1 the table matches, 2 it does not
uint64_t hideCalls();              // post-calls that zeroed the masks (a local AMC, placed)
uint64_t hideZeroed();             // mask words that were non-zero and were zeroed
uint64_t amcCalls();               // fade calls seen with the hide on
uint64_t amcLocalMatches();        // ...that were the local third-person AMC
uint64_t amcLocal();               // the local AMC pointer last matched
uint32_t phase();                  // 0 idle, 1 waiting, 2 placed
bool placeActive();
bool sessionActive();
bool exiting();
uint32_t f5Request();              // 0 none, 1 enter, 2 exit
int hotkeyVk();                    // the F5 key's virtual key after explorerCamFrameBoundary read the config
void preThenPost(void* activity);  // the free-camera hook's pre half and post half with no original between them (the fault cells)
void controllerPreThenPost(void* controller);
void forceSession(bool on);         // an F5 session switched on or off by hand (the config cells cannot press the real key)
// Phase 3: the head-joint eye source and the camera-suite isolation.
void setHeadImage(uintptr_t base, size_t size);   // the rig's synthetic game image for the head source (base 0: the real module)
void forcePlaceActive(bool on);                    // g_placeActive set by hand: Explorer Cam stood down under the hook threads, before the frame thread withdrew the placement
void setNowUs(uint64_t (*fn)());
float fadeAlpha();                 // the comfort fade's level as the frame thread last published it
uint32_t steadyUpdates();          // consecutive placing updates whose eye moved under 2 cm
bool uiSettled();                  // the camera UI's hide has run its course for this placement
float comfortRead(uint64_t nowMs); // what the runtime's provider would read from the signal at nowMs (comfort_fade.h)
bool comfortDefaultOn();           // the production wrapper's FrameInput runs the comfort fade (there is no key for it)                   // the clock the follow smoothing reads, in microseconds (null: the real one)
float followTrimRight();           // the trims and the smoothing as the frame thread last published them (clamped), for the config cells
float followTrimUp();
float followTrimForward();
float followSmoothingMs();
uint64_t followHeadUpdates();      // placing updates whose eye came from the head joint
uint64_t followFixedUpdates();     // ...and from the fixed keys
uint32_t followSource();           // 1 = the latest update used the head joint
uint32_t followWhy();              // ecm::FixedWhy of the latest fixed update
bool followReady();                // armed: the build and the joint-name literals checked
bool followDown();                 // the head source stood down for the session
uint32_t followFaults();           // faults of the head read
uint64_t isoBlocked(int holder);   // presses swallowed: 0 free camera, 1 controller, 2 camera UI, 3 zoom/DOF
bool isoDown();                    // the isolation stood down for the session
uint32_t isoFaults();
uint64_t zoomCalls();              // the zoom/DOF update reached its hook
bool zoomGateOpen();
void setFrameSink(ExplorerCamSinkFn fn, void* ctx);   // where explorerCamFrameBoundary's own lines go (null: the log)
void zoomPreThenPost(void* object);   // the zoom/DOF hook's pre half and post half with no original between them
void uiPreThenPost(void* object);
void reset();                      // every hook uninstalled, every latch and counter cleared
}  // namespace explorercamtest
#endif

}  // namespace edvr
