#pragma once
// Explorer Cam (fix.explorer_cam; docs\design-explorer-cam-free-camera-2026-10-07.md, "Phase 1a: placement built").
//
// In Elite's on-foot free camera (TAB), put the view in the commander's head, facing their way, and lock it to them. All of it goes
// through the game's own free-camera object, FreeCameraActivity, and is exactly three writes into game memory (Sean's decision D1,
// 2026-10-07): its commander-local pose, its collision step made to return "no edit" for that one activity, and one press of its
// relative lock. explorer_cam_core.h holds the decisions; this file's .cpp holds the two hooks, the config and the log.
//
// THE HOOKS (build 332841 only; any PE or prologue mismatch stands the feature down with one line and patches nothing):
//   EliteDangerous64.exe+0x1071980  the free camera's update. Placement runs BEFORE the original (the pose write and the lock press),
//                                   the lock's pressed-int is restored AFTER it returns, then the probe's snapshot, if it is on.
//   EliteDangerous64.exe+0x1091140  the camera's collision step, only ever called by that update. A relay in machine code returns 0
//                                   (no collision edit) when rcx is the placed activity and jumps to the original otherwise.
//
// THREADS. The free-camera hook runs on whichever thread the game's job system calls the update from. It takes no lock (a try-flag
// serialises a second caller out), allocates nothing, writes no log line and calls nothing of the game's: it talks to the frame
// thread through atomics and a small event ring. Config is read on the frame thread only, in explorerCamFrameBoundary.
#include <cstddef>
#include <cstdint>

namespace edvr {

// Once a frame at the Present boundary (vscreen.cpp), before explorerCamProbeFrameBoundary. Reads fix.explorer_cam and the three eye
// keys, installs the hooks the first time they are wanted, publishes the settings to the hook thread, drains the hook thread's events
// into the log, releases a session whose activity went silent, and writes the 5 s heartbeat while a session is on. Render thread.
// Never call it from inside a game hook.
void explorerCamFrameBoundary(uint32_t frameNo);

// ---- the probe's seam (advanced.explorer_cam_probe, temporary) -----------------------------------------------------------------
// One target gets one CodeHook, so the probe's observation of the same update rides this file's hook instead of installing its own.
struct ExplorerCamHookStatus {
    enum State : int { NotTried = 0, Armed = 1, StoodDown = 2 };
    int state = NotTried;
    size_t stolen = 0;       // Armed: the bytes of the prologue CodeHook moved
    uintptr_t target = 0;    // Armed: the hooked address
    uintptr_t relay = 0;     // Armed: the relay's address
    char why[400] = {};      // StoodDown: one sentence
};
// Called by the probe's own boundary (the frame thread). The observer, if any, runs after the original returns and after the lock
// press has been restored, with the activity (rcx). `want` false detaches it. Installs the free-camera hook the first time anyone
// wants it. Returns the hook's status.
using ExplorerCamActivityObserver = void (*)(void* activity) noexcept;
ExplorerCamHookStatus explorerCamProbeAttach(bool want, ExplorerCamActivityObserver observer);

#ifdef EDVR_EXPLORER_CAM_TEST
// The rigs' seam (tools\explorer_cam_test, tools\explorer_cam_probe_test): the same boundary with a scripted clock and scripted
// settings, synthetic functions in place of the game's, and the shared state to look at.
using ExplorerCamSinkFn = void (*)(void* ctx, const char* line);
namespace explorercamtest {
void setTargets(uintptr_t freeCamera, uintptr_t collision);
void boundary(uint32_t frame, uint64_t nowMs, bool on, float up, float forward, float right, ExplorerCamSinkFn fn, void* ctx);
bool gateOpen();
size_t freeStolen();
size_t collisionStolen();
uint64_t placedActivity();
uint64_t bypassed();
uint64_t forwarded();
uint64_t updatesPlaced();
uint64_t hookCalls();
uint32_t faults();
uint32_t phase();                  // 0 idle, 1 waiting, 2 placed
bool placeActive();
void preThenPost(void* activity);  // the hook's pre half and post half with no original between them (the fault cells)
void reset();                      // both hooks uninstalled, every latch and counter cleared
}  // namespace explorercamtest
#endif

}  // namespace edvr
