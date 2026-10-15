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
//               pressed-int is restored AFTER it returns.
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

// Once a frame at the Present boundary (device_hook.cpp's own tick, tkExplorerCam: it must run whatever vScreen installed, including transport-only). Reads hotkey.explorer_cam (Explorer Cam is
// armed exactly when it is non-empty), the eye keys, polls F5, installs the hooks the first time they are wanted, publishes the settings to the hook threads,
// drains their events into the log, ends a session whose controller went silent, and writes the 5 s heartbeat while a session is on.
// Render thread. Never call it from inside a game hook.
void explorerCamFrameBoundary(uint32_t frameNo);

// The skeleton interface's FindJoint(name) (EliteDangerous64.exe+0xFDDB10, called by many systems). Explorer Cam hooks it (the original first, its result
// unchanged) to learn which skeleton interfaces the game attaches the local player's two avatars to: FUN 0x19B1240 calls FindJoint("def_c_povCamera_joint")
// from two sites for EVERY humanoid (F6: the last site-1 attach is usually an NPC's). Site 1 (returning to +0x19B12D5) is a humanoid's third-person avatar,
// site 2 its first-person avatar, which only the local player has. So the hook LATCHES a pair only when one thread makes a site-1 attach and then a site-2
// attach at the same stack location (one invocation of 0x19B1240 does both); sites 0 and 1 below are the latched local third-person and first-person
// skeletons. Head hiding and the head-joint eye use the latched pair only. The hook is installed when Explorer Cam is on.
struct ExplorerCamSkeleton {
    uint64_t iface = 0;       // the LATCHED local avatar's skeleton for this site (0 = none latched yet)
    uint32_t index = 0xFFFF;  // the povCamera joint index the original returned for it
    uint32_t captures = 0;    // RAW attaches seen at this site since launch: EVERY humanoid's, not only the local one
    uint32_t latches = 0;     // times a local pair was latched
};
ExplorerCamSkeleton explorerCamSkeleton(int site);
uint64_t explorerCamFindJointSeen();                         // every FindJoint call the hook has seen (proof it is alive)

// Is an Explorer Cam session on? True from F5's request until the session ends (the camera closed, F5 pressed again, or Explorer Cam stood down). An atomic read,
// safe from any thread: the hotkey menu uses it to lock the Explorer Cam key's row, because that key is also the way out.
bool explorerCamSessionActive();

// An unload (FreeLibrary): puts the avatar dither-fade global back to -1 if EDVR still holds it at 0. Frame-thread context; SEH-guarded.
void explorerCamShutdown();

// The game image base, when its PE identity (timestamp and size) is build 332841's; false, with a sentence in `why`, otherwise. The
// answer is cached for the session. The fade global and the follow's head-joint reads resolve their addresses from it.
bool explorerCamBuildKnown(uintptr_t* base, char* why, size_t whyCap);

// Native on-foot F1 census (log only, read-only): the local third-person avatar's parts as the head-hiding capture latched them. `masks`
// holds the part's four view-mask words; `ok` is false when the read faulted. Returns the number of parts written; `*amc` is the latched
// avatar (0 when none is latched yet, then nothing is written).
struct LocalAvatarPart {
    const char* name = nullptr;
    bool head = false;
    bool ok = false;
    uint64_t masks[4] = {};
};
uint32_t localAvatarParts(LocalAvatarPart* out, uint32_t cap, uint64_t* amc);
// The dither-fade mode global (-1 auto, 0 off), read under SEH. False when unreadable.
bool avatarFadeModeRead(int32_t* mode);

// Native on-foot F3 census (log only, read-only): every distinct avatar model component (AMC) the avatar-fade hook saw while native stereo
// was on foot, collected for one window and taken by the frame thread. An AMC whose origin is more than 8 m from the latched local
// avatar's is left out (an NPC far away); the rest are ranked by "has EVASuit_Arms, Hands or Legs" and then by distance to the local one.
struct AvatarCensusPart {
    const char* name = nullptr;
    uint32_t idx = 0;
    uint32_t variants = 0;          // bit k: instance k is set
    uint64_t masks[4] = {};         // the view mask of each instance (raw)
};
struct AvatarCensusRow {
    uint64_t amc = 0;
    int32_t mode = -1;              // AMC creation params +0x14 (3 = third-person)
    uint32_t parts = 0;             // parts with at least one instance
    float dist = 0.0f;
    bool distOk = false;            // both origins were read
    bool evaSet = false;            // instances of EVASuit_Arms, EVASuit_Hands or EVASuit_Legs
    AvatarCensusPart part[54];
};
// Takes the window: writes up to `cap` rows, `*seen` (distinct AMCs collected, before the distance filter) and `*dropped` (AMCs that did not
// fit the eight slots). Clears the window. Returns the number of rows written.
uint32_t avatarCensusTake(AvatarCensusRow* out, uint32_t cap, uint32_t* seen, uint32_t* dropped);

// Native on-foot F2 body (frame thread; native_on_foot.cpp calls it once a frame). `wanted`: the native key is on, so the head-hiding hooks are
// installed from launch (the capture must be in place before the first on-foot avatar attaches), whatever Explorer Cam's own key says. `body`:
// native stereo is on foot now, so the fade global goes to 0 (its own guard; Explorer Cam's claim wins) and the local third-person avatar's
// head parts are zeroed, with Explorer Cam's own code paths. The first-person arms and legs are not touched. `reason` (a literal) says why a
// body ends; it is logged with the restore. The body ends with the native flag, never mid-transition on Explorer Cam's terms.
void explorerCamNativeBody(bool wanted, bool body, const char* reason);

// F15 (native on foot, TEMPORARY, fix.native_on_foot_hide_fp_body; frame thread). `want`: the key is on and native stereo is on foot. While it holds,
// the fade hook's post-call zeroes the four view masks of the three first-person body parts (ecm::kFpBodyPartNames) on the local first-person AMC
// (mode 2), with the head hiding's SEH method. Nothing is restored: the game rewrites the masks every frame. Alpha, the view filter and flag bytes are
// not touched. Logs the transitions once; `reason` (a literal) says why it ends.
void explorerCamFpBody(bool want, const char* reason);
// F19 (TEMPORARY): the body's clip modifiers. unclipWant = write them (fix.native_on_foot_body_unclip on, native on foot); probeWant = the 5 s read.
void explorerCamBodyClipTick(uint64_t nowMs, bool unclipWant, bool probeWant, const char* reason);
// F21 (TEMPORARY, fix.native_on_foot_body_hybrid; frame thread). `want`: the key is on and native stereo is on foot. While it holds, the hook
// (bodyClipStep, fpBodyStep) unclips the third-person torso, zeroes the third-person arms, hands and shoulders' view masks, and zeroes only the
// first-person skeleton's masks on the first-person AMC. Logs the transitions and a 5 s census line.
void explorerCamHybridTick(uint64_t nowMs, bool want, const char* reason);
struct FpBodyReadback {
    uint64_t amc = 0;               // the first-person AMC last zeroed (0 = none yet this session)
    uint32_t parts = 0;             // how many of the three names were found in the table
    const char* name[3] = {};       // their names, in ecm::kFpBodyPartNames order
    uint64_t masks[3][4] = {};      // the four view masks of each, read straight after the zero (the frame thread's own read can see the next frame's)
    uint64_t mode2Seen = 0;         // first-person AMC calls while the body was wanted
};
FpBodyReadback explorerCamFpBodyReadback();

// Native on-foot F1 neck census (log only, read-only; explorer_cam.cpp neckCensusStep): the newest sample. `state` 5 means the head joint
// was read, in avatar-root model space; `amc` is the local AMC's origin in world space (`amcState` 1 when read). Two different spaces:
// the difference between them is not a camera offset. False until a sample exists.
struct NeckCensusReading {
    uint32_t state = 0;
    uint64_t stamp = 0;
    float head[3] = {};
    float amc[3] = {};
    int amcState = 0;
    uint32_t neckState = 0;      // F39: 1 read (neck, avatar-root model space), 2 no candidate name answered, 3 not plausible
    float neck[3] = {};
    const char* neckName = nullptr;   // F39: the candidate name the neck was found under
};
bool neckCensusRead(NeckCensusReading* out);
bool explorerCamRestPovDelta(float out[3]);   // F33: the povCamera joint minus the head joint in model space, once Explorer Cam's walk derived it
uint64_t localAmcPointer();   // F18: the local third-person AMC last matched by the fade hook (0 = none); the latch the census reads

#ifdef EDVR_EXPLORER_CAM_TEST
// The rig's seam (tools\explorer_cam_test): the same boundary with a scripted clock and scripted
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
    bool motionArmed = false;             // the engine's motion for the eye path is live: an entry waits for it (default: nothing to wait for, as the older cells expect)
    uint32_t viewsRun = 0;                // ...consecutive frames both eyes were handed its views
    bool skinJobs = false, skinLive = false;   // ...the frame has skinned jobs, and the second skin's join is live for it
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
uint32_t settingsReads();          // how many times explorerCamFrameBoundary read its settings through Config (once per configuration change, not per frame)
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
