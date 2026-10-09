// Explorer Cam's rig (src/d3d11/explorer_cam_core.h, explorer_cam.cpp, elite_binds.cpp; hotkey.explorer_cam).
//
// Part A drives the pure half the DLL compiles: the build-332841 identity and CodeHook's reading of all five prologues, the eye
// keys' clamp, the sixteen floats written into the free camera's commander-local pose, the placement machine on scripted state
// sequences (entry, the pending first update, the lock pressed once, the user's own unlock, a detach and the return, the variant
// state, a second activity, the session ending, a fault), the F5 sequencer on scripted mode sequences (enter from every mode,
// timeouts, the player's own TAB engaging nothing, detach and re-attach, exit with the UI given back first), F5's decision table,
// the camera-UI hider, the stale and liveness watches, the event ring, every log line's text, and the two relays' machine code.
//
// Part B runs the glue (explorer_cam.cpp compiled with EDVR_EXPLORER_CAM_TEST) end to end against SYNTHETIC functions that begin
// with the real prologues: the free-camera update, the collision sweep (which reads a fifth stack argument), the commander's box
// push, the camera UI's update and the camera controller's update. The rig is the GAME between calls: it moves the mode byte the
// way the controller does when it sees a pressed int, toggles the UI byte the way the UI does, advances +0x48C the way the update
// does, and presses nothing itself unless a cell plays the player. It proves, through the real relays and the real hooks:
//   * the pose is written before the original and visible to it, every pressed int is 1 for exactly one call and restored after;
//   * both bypass relays answer 0 without running the function for the placed activity only, and hand every other call, with all
//     its arguments, to the original;
//   * a wrong prologue (each of the five) stands down with one line and no patch, and placement never runs half installed;
//   * F5 opens the camera, switches to the free camera, places, locks and hides the UI; F5 again gives the UI back, closes the
//     camera and ends the session; entering from a preset and from the free camera; a detached camera refuses; timeouts abort;
//     the player's own camera key and TAB engage nothing; a detach keeps the session and the return places again;
//   * a controller that is not called says "the camera controller is idle: open the camera first";
//   * the hotkey is checked against a fixture of the player's Elite bindings at launch and on a rebind, and a clash is named;
//   * faults are counted and the eighth ends the feature; the keys are read from the config under their real names.
// This is "what would appear in the log if the new code never ran" made executable.
//
// --self-test runs it and prints "explorer cam: PASS" only when every check holds.
#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "../../src/common/code_hook.h"
#include "../../src/common/config.h"
#include "../../src/common/hotkey.h"
#include "../../src/d3d11/elite_binds.h"
#include "../../src/d3d11/engine_motion_ready.h"
#include "../../src/d3d11/explorer_cam.h"
#include "../../src/d3d11/explorer_cam_core.h"
#include "../../src/d3d11/explorer_cam_follow_core.h"
#include "../../src/d3d11/explorer_cam_fade_core.h"
#include "../../src/common/comfort_fade.h"

using namespace edvr;

namespace {
bool g_journalActive = true, g_journalGameplay = true, g_journalOnFootKnown = true, g_journalOnFoot = true;
}

// ---- stubs for what the glue calls -------------------------------------------------------------------------------------
namespace edvr {
void breadcrumb(const char*) {}   // production guard.cpp's crash-channel dependency (proxy.cpp), as the other rigs stub it
bool journalWatchActive() { return g_journalActive; }
bool journalGameplay() { return g_journalGameplay; }
bool journalOnFootKnown() { return g_journalOnFootKnown; }
bool journalOnFoot() { return g_journalOnFoot; }
bool journalGuiFocus(uint32_t* focus) {
    if (focus) *focus = 0;
    return false;   // the rig's boundary gets its focus from ExplorerCamTestFrame, not from here
}
// explorerCamFrameBoundary asks the engine for its motion's readiness; the rig's boundary scripts it through ExplorerCamTestFrame instead.
EngineMotionReady engineMotionReady() { return EngineMotionReady{}; }
}  // namespace edvr

namespace {

int g_failures = 0;
void check(bool ok, const char* what) {
    if (!ok) { ++g_failures; std::printf("  FAIL  %s\n", what); }
    else std::printf("  ok    %s\n", what);
}
bool has(const std::string& text, const char* needle) { return text.find(needle) != std::string::npos; }
bool closeTo(float a, float b, float tol = 1e-5f) { return std::fabs(a - b) <= tol; }

struct Capture {
    std::vector<std::string> lines;
    static void add(void* ctx, const char* line) { static_cast<Capture*>(ctx)->lines.emplace_back(line); }
    // Lines that contain `needle`.
    size_t count(const char* needle) const {
        size_t n = 0;
        for (const std::string& l : lines) n += has(l, needle) ? 1 : 0;
        return n;
    }
    // The nth (0-based) line containing `needle`, or an empty string.
    std::string nth(const char* needle, size_t n) const {
        for (const std::string& l : lines)
            if (has(l, needle) && n-- == 0) return l;
        return std::string();
    }
    // The index of the first line containing `needle`, or -1.
    int indexOf(const char* needle) const {
        for (size_t i = 0; i < lines.size(); ++i)
            if (has(lines[i], needle)) return static_cast<int>(i);
        return -1;
    }
    size_t prefixed() const {
        size_t n = 0;
        for (const std::string& l : lines) n += l.compare(0, std::strlen(ecm::prefix()), ecm::prefix()) == 0 ? 1 : 0;
        return n;
    }
    void clear() { lines.clear(); }
};

// How many bytes CodeHook would steal from a prologue: whole instructions until at least five are covered, no displacement.
size_t stolenOf(const uint8_t* code, size_t n) {
    size_t at = 0;
    while (at < kCodeHookPatchBytes) {
        size_t disp = 99;
        const size_t len = codeInstructionLength(code + at, n - at, &disp);
        if (len == 0 || disp != 0) return 0;
        at += len;
    }
    return at;
}

// ================================ Part A: the pure half ================================

void testIdentity() {
    std::printf("identity\n");
    // The bytes exactly as the Phase 0a notes printed them, spelled out again here (not copied from the header).
    const uint8_t free28[28] = {0x48, 0x89, 0x5C, 0x24, 0x20, 0x55, 0x57, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48,
                                0x8D, 0xAC, 0x24, 0x40, 0xFD, 0xFF, 0xFF, 0x48, 0x81, 0xEC, 0xC0, 0x03, 0x00, 0x00};
    const uint8_t col26[26] = {0x40, 0x55, 0x53, 0x56, 0x57, 0x41, 0x54, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8D,
                               0xAC, 0x24, 0xB0, 0xFE, 0xFF, 0xFF, 0x48, 0x81, 0xEC, 0x50, 0x02, 0x00, 0x00};
    const uint8_t box20[20] = {0x48, 0x8B, 0xC4, 0x55, 0x53, 0x56, 0x41, 0x56, 0x41, 0x57,
                               0x48, 0x8B, 0xEC, 0x48, 0x81, 0xEC, 0x80, 0x00, 0x00, 0x00};
    const uint8_t ui19[19] = {0x40, 0x55, 0x41, 0x56, 0x48, 0x8D, 0xAC, 0x24, 0x48, 0xFF, 0xFF, 0xFF, 0x48, 0x81, 0xEC, 0xB8, 0x01, 0x00, 0x00};
    const uint8_t ctl15[15] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18};
    check(sizeof(ecm::kFreeCameraPrologue) == 28 && std::memcmp(ecm::kFreeCameraPrologue, free28, 28) == 0, "the free-camera prologue is 48 89 5C 24 20 55 57 41 55 ...");
    check(sizeof(ecm::kCollisionPrologue) == 26 && std::memcmp(ecm::kCollisionPrologue, col26, 26) == 0, "the collision prologue is 40 55 53 56 57 41 54 41 56 41 57 48 8D AC 24 B0 FE ...");
    check(sizeof(ecm::kBoxPushPrologue) == 20 && std::memcmp(ecm::kBoxPushPrologue, box20, 20) == 0, "the box-push prologue is 48 8B C4 55 53 56 41 56 41 57 48 8B EC 48 81 EC 80 00 00 00");
    check(sizeof(ecm::kCameraUiPrologue) == 19 && std::memcmp(ecm::kCameraUiPrologue, ui19, 19) == 0, "the camera-UI prologue is 40 55 41 56 48 8D AC 24 48 FF FF FF 48 81 EC B8 01 00 00");
    check(sizeof(ecm::kControllerPrologue) == 15 && std::memcmp(ecm::kControllerPrologue, ctl15, 15) == 0, "the controller prologue is 48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18");
    const uint8_t fade16[16] = {0x4C, 0x8B, 0xDC, 0x53, 0x56, 0x57, 0x48, 0x81, 0xEC, 0x10, 0x01, 0x00, 0x00, 0x48, 0x8B, 0x05};
    check(sizeof(ecm::kAvatarFadePrologue) == 16 && std::memcmp(ecm::kAvatarFadePrologue, fade16, 16) == 0 && ecm::kAvatarFadeRva == 0x3DD6040 && ecm::kAvatarFadeRva % 64 == 0,
          "the avatar dither-fade prologue is 4C 8B DC 53 56 57 48 81 EC 10 01 00 00 48 8B 05 at +0x3DD6040 (64-byte aligned; the first 5 bytes are whole instructions)");
    const uint8_t find16[16] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0x01, 0x48, 0x8B, 0xFA};
    check(sizeof(ecm::kFindJointPrologue) == 16 && std::memcmp(ecm::kFindJointPrologue, find16, 16) == 0 && ecm::kFindJointRva == 0xFDDB10 && ecm::kFindJointRva % 16 == 0,
          "the skeleton FindJoint prologue is 48 89 5C 24 08 57 48 83 EC 20 48 8B 01 48 8B FA at +0xFDDB10 (16-byte aligned; the first 5 bytes are one whole instruction)");
    check(ecm::kFadeModeRva == 0x5E9DC28 && ecm::kFadeAmountRva == 0x601E088 && ecm::kFadeAuto == -1 && ecm::kOffAvatarFadeBlock == 0x378 && ecm::kOffAvatarFadeEased == 0x380 &&
              ecm::kOffFadeBlockEnabled == 0x90 && ecm::kOffFadeBlockAmount == 0x120,
          "the fade global is +0x5E9DC28 (-1 = auto), the amount float +0x601E088, the component's block pointer +0x378 and eased level +0x380, the block's enabled +0x90 and amount +0x120");
    check(ecm::kFreeCameraRva == 0x1071980 && ecm::kCollisionRva == 0x1091140 && ecm::kBoxPushRva == 0x108F1B0 && ecm::kCameraUiRva == 0x47C7640 &&
              ecm::kControllerRva == 0x2DF14C0,
          "the targets are EliteDangerous64.exe+0x1071980, +0x1091140, +0x108F1B0, +0x47C7640 and +0x2DF14C0");
    check(ecm::kFreeCameraRva % 8 == 0 && ecm::kCollisionRva % 8 == 0 && ecm::kBoxPushRva % 8 == 0 && ecm::kCameraUiRva % 8 == 0 && ecm::kControllerRva % 8 == 0,
          "every target is 8-byte aligned, so CodeHook's aligned 8-byte store applies");
    check(ecm::kOffLocalPose == 0x3B0 && ecm::kOffRelative == 0x470 && ecm::kOffRotationLock == 0x471 && ecm::kOffPresetPending == 0x473 &&
              ecm::kOffState == 0x48C && ecm::kOffLockAction == 0x508 && ecm::kOffActionPressed == 0x1C && ecm::kOffToggleRotationAction == 0x4F8 &&
              ecm::kOffWorldFixAction == 0x500,
          "the free-camera activity's offsets are +0x3B0 +0x470 +0x471 +0x473 +0x48C, the actions at +0x4F8 +0x500 +0x508, the pressed-int at +0x1C");
    check(ecm::kOffUiHidden == 0x1A0 && ecm::kOffUiHideAction == 0x1D8 && ecm::kOffCtlMode == 0x3E0 && ecm::kOffCtlPhotoAction == 0x310 &&
              ecm::kOffCtlFreeAction == 0x328 && ecm::kOffCtlQuitAction == 0x340 && ecm::kOffCtlPresetKind == 0x2E8,
          "the camera UI's +0x1A0 +0x1D8 and the controller's +0x3E0 +0x310 +0x328 +0x340 +0x2E8");
    // CodeHook's decoder against every prologue: the steal lengths the hooks depend on.
    check(stolenOf(free28, 28) == 5, "CodeHook steals 5 bytes of the free-camera prologue");
    check(stolenOf(col26, 26) == 5, "...5 of the collision prologue (`40 55` is a REX push, a 2-byte instruction)");
    check(stolenOf(box20, 20) == 5, "...5 of the box-push prologue (`48 8B C4` mov rax,rsp; push rbp; push rbx)");
    check(stolenOf(ui19, 19) == 12, "...12 of the camera-UI prologue (push rbp; push r14; lea rbp,[rsp-0B8h] with SIB and disp32, no displacement to rewrite)");
    check(stolenOf(ctl15, 15) == 5, "...5 of the controller prologue (the first mov [rsp+8],rbx)");
}

void testEye() {
    std::printf("eye keys\n");
    ecm::Eye e = ecm::clampEye(1.68f, 0.10f, 0.0f);
    check(e.up == 1.68f && e.forward == 0.10f && e.right == 0.0f, "the defaults pass through unchanged");
    e = ecm::clampEye(0.1f, -9.0f, 9.0f);
    check(e.up == 0.5f && e.forward == -0.5f && e.right == 0.5f, "out of range: up clamps to 0.5, forward to -0.5, right to 0.5");
    e = ecm::clampEye(9.0f, 9.0f, -9.0f);
    check(e.up == 2.5f && e.forward == 0.5f && e.right == -0.5f, "out of range the other way: up 2.5, forward 0.5, right -0.5");
    e = ecm::clampEye(std::nanf(""), std::nanf(""), std::nanf(""));
    check(e.up == ecm::kEyeUpDefault && e.forward == ecm::kEyeForwardDefault && e.right == ecm::kEyeRightDefault, "NaN reads as the default");
    e = ecm::clampEye(INFINITY, -INFINITY, 0.25f);
    check(e.up == 2.5f && e.forward == -0.5f && e.right == 0.25f, "infinity clamps to the bound");
}

void testPoseWriter() {
    std::printf("the local-pose writer\n");
    // The game's pose as the selfie preset leaves it: facing back, origin (0, 1.5, 1.9), and a distinctive fourth float per row.
    const float game[16] = {-1, 0, 0, 0.5f, 0, 1, 0, 0.25f, 0, 0, -1, 0.125f, 0, 1.5f, 1.9f, 1.0f};
    ecm::Eye eye;
    eye.up = 1.68f; eye.forward = 0.10f; eye.right = -0.03f;
    float out[16];
    ecm::buildLocalPose(game, eye, out);
    const float want[16] = {1, 0, 0, 0.5f, 0, 1, 0, 0.25f, 0, 0, 1, 0.125f, -0.03f, 1.68f, 0.10f, 1.0f};
    bool exact = true;
    for (int i = 0; i < 16; ++i) exact = exact && std::memcmp(&out[i], &want[i], 4) == 0;
    check(exact, "exactly 16 floats: rows right (1,0,0), up (0,1,0), forward (0,0,1), origin (right, up, forward), each row's 4th float the game's");
    check(out[12] == -0.03f && out[13] == 1.68f && out[14] == 0.10f, "the origin is x = right, y = up, z = forward");
}

// ---- the placement machine ------------------------------------------------------------------------------------------------------
ecm::Observed obs(uint8_t relative, uint8_t rotLock, uint8_t pending, uint8_t state) {
    ecm::Observed o;
    o.relative = relative; o.rotationLock = rotLock; o.presetPending = pending; o.state = state;
    return o;
}
constexpr uint64_t kA = 0xA000, kB = 0xB000;

void testMachineSession() {
    std::printf("machine: a normal placement (a session is on)\n");
    ecm::Machine m;
    // The activity's first call: the byte still holds its constructor value (0), +0x473 = 1. Nothing happens.
    ecm::Step s = m.step(kA, obs(1, 1, 1, 0), true);
    check(!s.entered && !s.write && !s.press && !s.released && m.phase() == ecm::Machine::Phase::Idle, "first call (+0x48C=0, +0x473=1): idle, no write, no press");
    // The second call: the first update stored 3 and cleared +0x473. Entry, first write and the press, all on this call.
    s = m.step(kA, obs(1, 1, 0, 3), true);
    check(s.entered && s.write && s.placeNow && s.press && !s.waiting && !s.alreadyLocked, "second call (+0x48C=3, +0x473=0, +0x470=1): ENTRY, first write, lock PRESSED");
    check(m.phase() == ecm::Machine::Phase::Placing && m.activity() == kA && m.placed() && m.pressed(), "...the machine now tracks the activity as placed and pressed");
    // The third call: the press worked (3 -> 4). The result is reported once; the write continues; no second press.
    s = m.step(kA, obs(1, 1, 0, 4), true);
    check(s.write && !s.placeNow && !s.press && !s.entered && s.pressResult && s.pressBefore == 3 && s.pressAfter == 4,
          "third call (+0x48C=4): writes again, no press, and reports the press once: state 3 -> 4");
    s = m.step(kA, obs(1, 1, 0, 4), true);
    check(s.write && !s.press && !s.pressResult, "fourth call: writes, no press, no second report");
    // The user unlocks (4 -> 3): placing continues, the lock is NOT pressed again.
    s = m.step(kA, obs(1, 1, 0, 3), true);
    check(s.write && !s.press && !s.released && !s.entered, "the user unlocks (+0x48C=3): keeps placing, does not press again");
    s = m.step(kA, obs(1, 1, 0, 3), true);
    check(s.write && !s.press, "...and again on the next update");
    // Leaving the free camera.
    s = m.step(kA, obs(1, 1, 0, 0), true);
    check(s.released && s.why == ecm::Why::LeftFreeCamera && !s.write && m.phase() == ecm::Machine::Phase::Idle, "+0x48C=0 releases (left the free camera), no write");
    // A new camera visit with the session still on.
    s = m.step(kA, obs(1, 1, 1, 0), true);
    check(!s.entered && !s.write, "a new visit's first call (+0x48C=0, +0x473=1): idle again");
    s = m.step(kA, obs(1, 1, 0, 3), true);
    check(s.entered && s.write && s.press, "...and its second call enters, places and presses again: ONE press per placement");
}

void testMachineNoSession() {
    std::printf("machine: no session, nothing is placed\n");
    ecm::Machine m;
    // The player's own camera key and TAB: the activity runs, the states change, and EDVR does nothing.
    const uint8_t states[] = {0, 3, 3, 4, 4, 3, 5, 3, 0};
    bool any = false;
    for (uint8_t st : states) {
        const ecm::Step s = m.step(kA, obs(1, 1, st == 0 ? 1 : 0, st), false);
        any = any || s.entered || s.write || s.press || s.released || s.waiting;
    }
    check(!any && m.phase() == ecm::Machine::Phase::Idle, "without a session the machine never enters, writes or presses, whatever the activity does");
}

void testMachinePending() {
    std::printf("machine: the preset still pending, and the relative flag\n");
    ecm::Machine m;
    // State already 3 on the very first call with +0x473 = 1 (a pooled activity that kept its byte): entry, but nothing is written yet.
    ecm::Step s = m.step(kA, obs(1, 1, 1, 3), true);
    check(s.entered && !s.write && !s.press && s.waiting && !s.placeNow, "+0x48C=3 with +0x473=1: entered and WAITING, no write, no press (the first update ignores +0x3B0)");
    s = m.step(kA, obs(1, 1, 1, 3), true);
    check(!s.entered && !s.write && s.waiting, "still pending: still waiting");
    s = m.step(kA, obs(0, 1, 0, 3), true);
    check(!s.write && !s.press && s.waiting, "+0x473=0 but +0x470=0 (a world pose): still waiting, nothing written, nothing pressed");
    s = m.step(kA, obs(1, 1, 0, 3), true);
    check(s.write && s.placeNow && s.press, "+0x470=1 and +0x473=0: the first write, and the press on it");
    // Locked before the first write: placing with no press.
    ecm::Machine n;
    n.step(kA, obs(1, 1, 1, 3), true);
    s = n.step(kA, obs(1, 1, 0, 4), true);
    check(s.write && s.placeNow && s.alreadyLocked && !s.press && n.pressed(), "already locked (+0x48C=4) at the first write: placed, no press, now or later");
    s = n.step(kA, obs(1, 1, 0, 3), true);
    check(s.write && !s.press, "...and an unlock after that is not pressed again either");
    // A session that finds the camera already locked (F5 from the locked free camera): enters at 4 and places, no press.
    ecm::Machine t;
    s = t.step(kA, obs(1, 1, 0, 4), true);
    check(s.entered && s.write && s.placeNow && s.alreadyLocked && !s.press, "F5 from the locked free camera (+0x48C=4): enters, places, does not press");
}

void testMachineReleases() {
    std::printf("machine: what ends a placement, and what places again\n");
    ecm::Step s;
    // A detach (the world lock) mid-session: released, the session untouched, and the return to 3 places AND locks again.
    ecm::Machine m;
    m.step(kA, obs(1, 1, 0, 3), true);
    s = m.step(kA, obs(1, 1, 0, 5), true);
    check(s.released && s.why == ecm::Why::WorldLock && !s.write && !s.press && m.phase() == ecm::Machine::Phase::Idle, "+0x48C=5 (detached) RELEASES, writes nothing");
    s = m.step(kA, obs(1, 1, 0, 5), true);
    check(!s.entered && !s.write, "...and stays idle while detached");
    s = m.step(kA, obs(1, 1, 0, 3), true);
    check(s.entered && s.write && s.placeNow && s.press, "...the return to 3 (session still on) enters, places and presses the lock again");
    s = m.step(kA, obs(1, 1, 0, 4), true);
    check(s.write && s.pressResult && !s.press, "...and the placement carries on as before");
    // The variant state.
    ecm::Machine v;
    v.step(kA, obs(1, 1, 0, 3), true);
    s = v.step(kA, obs(1, 1, 0, 6), true);
    check(s.released && s.why == ecm::Why::Variant && !s.write, "+0x48C=6 (variant) RELEASES");
    // An unknown state.
    ecm::Machine u;
    u.step(kA, obs(1, 1, 0, 3), true);
    s = u.step(kA, obs(1, 1, 0, 7), true);
    check(s.released && s.why == ecm::Why::UnexpectedState && !s.write, "an unknown state (7) releases rather than writes");
    s = u.step(kA, obs(1, 1, 0, 7), true);
    check(!s.entered && !s.write, "...and an idle machine does not enter at 7");
    // The session ending.
    ecm::Machine k;
    k.step(kA, obs(1, 1, 0, 3), true);
    s = k.step(kA, obs(1, 1, 0, 4), false);
    check(s.released && s.why == ecm::Why::KeyOff && !s.write && !s.press, "the session ending RELEASES on the next call, writes nothing");
    s = k.step(kA, obs(1, 1, 0, 3), false);
    check(!s.entered && !s.write, "...and with no session an idle machine does nothing, even at +0x48C=3");
    s = k.step(kA, obs(1, 1, 0, 3), true);
    check(s.entered && s.write && s.press, "...a session again at +0x48C=3: enters again");
    // The fault path: the hook thread aborts.
    ecm::Machine f;
    f.step(kA, obs(1, 1, 0, 3), true);
    f.abort(ecm::Why::Fault);
    check(f.phase() == ecm::Machine::Phase::Idle && !f.placed(), "a fault aborts the placement");
    s = f.step(kA, obs(1, 1, 0, 3), true);
    check(!s.entered && !s.write, "...and does not re-enter at the same state (no fault loop)");
    s = f.step(kA, obs(1, 1, 0, 0), true);
    s = f.step(kA, obs(1, 1, 0, 3), true);
    check(s.entered && s.write, "...until the state has changed: then it places again");
    // A reset (the frame thread's request) starts over.
    f.reset();
    s = f.step(kA, obs(1, 1, 0, 3), true);
    check(s.entered && s.write, "a reset makes the next +0x48C=3 an entry");
    // +0x473 going back to 1 on a placed activity: a new visit without a 0 in between.
    ecm::Machine r;
    r.step(kA, obs(1, 1, 0, 3), true);
    r.step(kA, obs(1, 1, 0, 4), true);
    s = r.step(kA, obs(1, 1, 1, 4), true);
    check(s.released && s.why == ecm::Why::NewSession && !s.write && s.entered && s.waiting,
          "+0x473=1 again on a placed activity ends the placement (a new visit began), does not write, and enters the new visit WAITING");
    s = r.step(kA, obs(1, 1, 0, 3), true);
    check(s.write && s.placeNow && s.press && !s.entered, "...and the update after it places and locks again");
}

void testMachineForeign() {
    std::printf("machine: a second activity\n");
    ecm::Machine m;
    m.step(kA, obs(1, 1, 0, 3), true);
    ecm::Step s = m.step(kB, obs(1, 1, 0, 3), true);
    check(s.foreign && !s.write && !s.press && !s.entered && m.activity() == kA, "while A is placed, B's calls are ignored (nothing written, nothing pressed, A still tracked)");
    s = m.step(kA, obs(1, 1, 0, 4), true);
    check(s.write && s.pressResult, "...A carries on");
}

// ---- F5's sequencer -------------------------------------------------------------------------------------------------------------
using ecm::CtlPress;
using ecm::F5Req;
using ecm::SeqEvent;
ecm::CtlView V(int mode, int pending = 0, int sharedFlag = 0) {
    ecm::CtlView v;
    v.mode = static_cast<uint8_t>(mode);
    v.pending = static_cast<uint8_t>(pending);
    v.sharedFlag = static_cast<int16_t>(sharedFlag);
    return v;
}
bool hasEv(const ecm::SeqStep& s, SeqEvent e) {
    for (uint8_t i = 0; i < s.nev; ++i)
        if (s.ev[i] == e) return true;
    return false;
}
// Step the sequencer `n` times on the same view with no request; returns the last step, and the step on which a press happened (0 = none).
ecm::SeqStep run(ecm::F5Sequencer& q, const ecm::CtlView& v, int n, int* pressedAt = nullptr, bool ui = false) {
    ecm::SeqStep s;
    if (pressedAt) *pressedAt = 0;
    for (int i = 1; i <= n; ++i) {
        s = q.step(v, F5Req::None, ui);
        if (s.press != CtlPress::None && pressedAt && *pressedAt == 0) *pressedAt = i;
    }
    return s;
}

void testSequencerEnter() {
    std::printf("F5 sequencer: enter\n");
    // From closed: press PhotoCameraToggle, wait for 1 or 2, wait until the suite is READY, press ToggleFreeCam once, wait for 3.
    ecm::F5Sequencer q;
    ecm::SeqStep s = q.step(V(0), F5Req::None, false);
    check(s.press == CtlPress::None && !s.sessionActive && s.nev == 0, "idle: nothing is pressed and no session is on, whatever the mode");
    s = q.step(V(0), F5Req::Enter, false);
    check(s.press == CtlPress::Photo && s.sessionActive && hasEv(s, SeqEvent::EnterFromClosed), "ENTER from mode 0: PhotoCameraToggle is pressed, the session is on");
    s = q.step(V(0), F5Req::None, false);
    check(s.press == CtlPress::None && s.sessionActive, "...the next update waits (the mode has not moved yet): no press");
    s = q.step(V(1), F5Req::None, false);
    check(s.press == CtlPress::None && hasEv(s, SeqEvent::EnterOpened), "...mode 1 (the suite just opened): NOT pressed yet -- the F2 failure was a ToggleFreeCam pressed the update after opening");
    int at = 0;
    s = run(q, V(1), 10, &at);
    check(at == 4 && hasEv(s, SeqEvent::EnterReady) == false, "...it is pressed on the 5th update that looks ready (the opening update was the 1st), not before");
    // Re-run to look at the ready step itself.
    ecm::F5Sequencer r;
    r.step(V(0), F5Req::Enter, false);
    r.step(V(1), F5Req::None, false);
    s = run(r, V(1), 3);
    check(s.press == CtlPress::None, "...the 4th ready update: still no press");
    s = r.step(V(1), F5Req::None, false);
    check(s.press == CtlPress::Free && hasEv(s, SeqEvent::EnterReady) && s.readyAfter == 5, "...the 5th: ToggleFreeCam, once, 'ready after 5 updates'");
    s = run(r, V(1), 9, &at);
    check(at == 0, "...nothing in the 9 updates after it (the suite may be about to take it)");
    s = r.step(V(1), F5Req::None, false);
    check(s.press == CtlPress::Free && s.nev == 0, "...the 10th update after it, the suite still on a preset with +0x3E1 clear: ToggleFreeCam AGAIN (a repeat is not logged on its own)");
    s = run(r, V(1), 20, &at);
    s = r.step(V(3), F5Req::None, false);
    check(s.press == CtlPress::None && s.sessionActive && hasEv(s, SeqEvent::EnterAttached) && s.readyAfter == 5 && s.toMode3 == 31 && s.presses == 4 && !s.queued &&
              r.stage() == ecm::F5Sequencer::Stage::Active,
          "...mode 3 after 31 updates and 4 presses (at 0, 10, 20, 30): the free camera is up, the line carries 'ready after 5, mode 3 after 31, 4 presses, accepted'");
    s = r.step(V(4), F5Req::None, false);
    check(s.press == CtlPress::None && s.sessionActive && s.nev == 0, "...mode 4 (the lock took): Active, nothing to do");
    // From a preset (mode 1 or 2): the same readiness wait, no Photo press.
    for (int mode : {1, 2}) {
        ecm::F5Sequencer p;
        s = p.step(V(mode), F5Req::Enter, false);
        check(s.press == CtlPress::None && s.sessionActive && hasEv(s, SeqEvent::EnterFromPreset), mode == 1 ? "ENTER from mode 1: no press yet (the suite must look ready first)" : "ENTER from mode 2: no press yet");
        s = run(p, V(mode), 4, &at);
        check(at == 4, "...ToggleFreeCam on the 5th ready update, the ENTER update counting as the first");
        s = p.step(V(3), F5Req::None, false);
        check(s.sessionActive && hasEv(s, SeqEvent::EnterAttached), "...and mode 3 attaches");
    }
    // From the free camera (3, 4): nothing to press.
    for (int mode : {3, 4}) {
        ecm::F5Sequencer p;
        s = p.step(V(mode), F5Req::Enter, false);
        check(s.press == CtlPress::None && s.sessionActive && hasEv(s, SeqEvent::EnterFromFree), mode == 3 ? "ENTER from mode 3: the session starts, nothing pressed" : "ENTER from mode 4: the session starts, nothing pressed");
    }
    // From a detached camera (5) or a variant (6): refused, no session.
    {
        ecm::F5Sequencer p;
        s = p.step(V(5), F5Req::Enter, false);
        check(s.press == CtlPress::None && !s.sessionActive && hasEv(s, SeqEvent::EnterRefusedDetached), "ENTER from mode 5 (detached): refused, 'leave it first', no session");
        s = p.step(V(6), F5Req::Enter, false);
        check(!s.sessionActive && hasEv(s, SeqEvent::EnterRefusedVariant), "ENTER from mode 6 (variant): refused, no session");
    }
    // A second ENTER while a session is on does nothing.
    {
        ecm::F5Sequencer p;
        p.step(V(3), F5Req::Enter, false);
        s = p.step(V(3), F5Req::Enter, false);
        check(s.press == CtlPress::None && hasEv(s, SeqEvent::EnterIgnored) && s.sessionActive, "a second ENTER while the session is on is ignored");
    }
}


// F3: a single press is dropped during the suite's opening transition. ToggleFreeCam is pressed again every kSeqRepressUpdates updates.
void testRepress() {
    std::printf("F5 sequencer: re-pressing ToggleFreeCam (dropped press, accepted on press N, pending after press N, first press, timeout, never outside mode 1/2)\n");
    ecm::SeqStep s;
    // Helper: open the session on a preset and step to the first press; returns the sequencer ready to count updates since it.
    auto ready = [&](ecm::F5Sequencer& q) {
        q.step(V(1), F5Req::Enter, false);
        run(q, V(1), 3);
        s = q.step(V(1), F5Req::None, false);
        return s.press == CtlPress::Free;
    };
    // DROP THEN ACCEPT: presses 1 and 2 are dropped (the suite still shows mode 1), press 3 is taken: mode 3 on the next update.
    {
        ecm::F5Sequencer q;
        check(ready(q), "(the first press, at the 5th ready update)");
        int updates = 0, pressesSeen = 1;
        std::vector<int> at = {0};
        while (pressesSeen < 3 && updates < 100) {
            ++updates;
            s = q.step(V(1), F5Req::None, false);
            if (s.press == CtlPress::Free) { ++pressesSeen; at.push_back(updates); }
        }
        check(pressesSeen == 3 && at.size() == 3 && at[1] == 10 && at[2] == 20, "DROP THEN ACCEPT: presses go out 10 updates apart (updates 0, 10, 20 after the first)");
        s = q.step(V(3), F5Req::None, false);
        check(s.press == CtlPress::None && hasEv(s, SeqEvent::EnterAttached) && s.presses == 3 && !s.queued && s.toMode3 == 21 && s.readyAfter == 5 && s.sessionActive,
              "...press 3 is taken: mode 3 on the next update -> attached, presses=3, accepted, 21 updates after the first press (26 from the suite opening)");
        s = q.step(V(3), F5Req::None, false);
        check(s.press == CtlPress::None && s.nev == 0, "...and nothing is pressed once the free camera is up");
    }
    // ACCEPT ON THE VERY FIRST PRESS.
    {
        ecm::F5Sequencer q;
        ready(q);
        s = q.step(V(3), F5Req::None, false);
        check(hasEv(s, SeqEvent::EnterAttached) && s.presses == 1 && s.toMode3 == 1 && !s.queued, "ACCEPT ON THE FIRST PRESS: attached on the very next update, presses=1, no repeat");
    }
    // PENDING AFTER PRESS N: the second press makes the game queue the entry; no third press, however long it takes.
    {
        ecm::F5Sequencer q;
        ready(q);
        run(q, V(1), 9);
        s = q.step(V(1), F5Req::None, false);
        check(s.press == CtlPress::Free, "(press 2 at 10)");
        s = q.step(V(1, 1, 0), F5Req::None, false);
        check(s.press == CtlPress::None && hasEv(s, SeqEvent::EnterQueued), "PENDING AFTER PRESS 2: +0x3E1 = 1 -> 'the game queued the entry itself', not pressed");
        bool again = false;
        for (int i = 0; i < 300; ++i) {
            s = q.step(V(1, i % 2, 0), F5Req::None, false);   // +0x3E1 even flickers back to 0: the pressing stays stopped
            again = again || s.press != CtlPress::None;
        }
        check(!again && s.sessionActive, "...300 updates later (including updates where +0x3E1 reads 0 again) nothing is re-pressed, the session is alive");
        s = q.step(V(3), F5Req::None, false);
        check(hasEv(s, SeqEvent::EnterAttached) && s.presses == 2 && s.queued, "...mode 3: attached, presses=2, pending then accepted");
    }
    // PENDING AT THE DUE UPDATE: the press that would be due is not made while +0x3E1 is 1.
    {
        ecm::F5Sequencer q;
        ready(q);
        run(q, V(1), 9);
        s = q.step(V(1, 1, 0), F5Req::None, false);
        check(s.press == CtlPress::None && hasEv(s, SeqEvent::EnterQueued), "PRESSING WHILE PENDING: at the update where press 2 is due, +0x3E1 = 1 -> no press");
    }
    // TIMEOUT: every press dropped; 90 presses in about 10 s and then the abort.
    {
        ecm::F5Sequencer q;
        ready(q);
        int n = 1;
        uint32_t when = 0;
        for (uint32_t i = 1; i <= 1000 && when == 0; ++i) {
            s = q.step(V(1), F5Req::None, false);
            if (s.press == CtlPress::Free) ++n;
            if (hasEv(s, SeqEvent::EnterTimeoutFree)) when = i;
        }
        check(when == ecm::kSeqTabWaitUpdates && n == 90 && s.presses == 90 && !s.sessionActive, "TIMEOUT: 90 presses, then aborted at the 900th update; the due update at 900 presses nothing");
    }
    // NEVER OUTSIDE MODE 1/2, and either of 1 and 2 will do.
    for (int m : {0, 3, 4, 5, 6, 7, 255}) {
        ecm::F5Sequencer q;
        ready(q);
        run(q, V(1), 9);
        s = q.step(V(m), F5Req::None, false);
        char what[160];
        std::snprintf(what, sizeof(what), "NEVER OUTSIDE MODE 1/2: at the due update with mode %d, ToggleFreeCam is not pressed", m);
        check(s.press == CtlPress::None, what);
    }
    {
        ecm::F5Sequencer q;
        ready(q);
        run(q, V(1), 9);
        s = q.step(V(2), F5Req::None, false);
        check(s.press == CtlPress::Free, "...mode 2 at the due update is a preset too: pressed");
    }
    // The due update that the player's own TAB beat: mode 3 at the due update attaches and presses nothing.
    {
        ecm::F5Sequencer q;
        ready(q);
        run(q, V(1), 9);
        s = q.step(V(3), F5Req::None, false);
        check(s.press == CtlPress::None && hasEv(s, SeqEvent::EnterAttached) && s.presses == 1, "...and mode 3 at the due update attaches, presses=1, nothing pressed");
    }
}

void testTabWait() {
    std::printf("F5 sequencer: the TAB wait (not ready, ready, pending, late completion, timeout)\n");
    ecm::SeqStep s;
    int at = 0;
    // NOT READY: each condition, held off, keeps ToggleFreeCam from being pressed -- and one ready update in between restarts the count.
    {
        ecm::F5Sequencer q;
        q.step(V(1), F5Req::Enter, false);
        run(q, V(1), 3);
        s = q.step(V(1, 1, 0), F5Req::None, false);
        check(s.press == CtlPress::None, "NOT READY: +0x3E1 = 1 (the game's own retry is running) at the 4th update: no press");
        run(q, V(1), 3);
        s = q.step(V(1), F5Req::None, false);
        check(s.press == CtlPress::None, "...and the count restarted: the 4th ready update since it is not the 5th");
        s = run(q, V(1), 2, &at);
        check(at == 1, "...five in a row since it: pressed");
    }
    {
        ecm::F5Sequencer q;
        q.step(V(1), F5Req::Enter, false);
        s = run(q, V(1, 0, 1), 100, &at);
        check(at == 0, "NOT READY: the shared record's +0x1D = 1: nothing pressed in 100 updates");
        s = run(q, V(1, 0, 0), 5, &at);
        check(at == 5, "...cleared: pressed on the 5th ready update after it");
    }
    {
        ecm::F5Sequencer q;
        q.step(V(1, 0, -1), F5Req::Enter, false);
        s = run(q, V(1, 0, -1), 5, &at);
        check(at == 4, "+0x1D not observable (-1): it does not hold the press back (the wait goes on mode and +0x3E1 alone)");
    }
    {
        ecm::F5Sequencer q;
        q.step(V(0), F5Req::Enter, false);
        run(q, V(0), 3);
        s = run(q, V(0), 1);
        check(s.press == CtlPress::None && !s.nev, "NOT READY: mode 0 is not ready (the suite has not opened)");
    }
    // PENDING / LATE COMPLETION: pressed once; the game queues the entry itself and completes it 6 s later; the session is still there.
    {
        ecm::F5Sequencer q;
        q.step(V(1), F5Req::Enter, false);
        s = run(q, V(1), 4, &at);
        check(at == 4, "(pressed at the 5th ready update)");
        s = q.step(V(1, 1, 0), F5Req::None, false);
        check(s.press == CtlPress::None && hasEv(s, SeqEvent::EnterQueued), "PENDING: +0x3E1 = 1 after the press: 'the game queued the entry itself', said once, not re-pressed");
        bool again = false, queuedAgain = false;
        for (int i = 0; i < 540; ++i) {
            s = q.step(V(1, 1, 0), F5Req::None, false);
            again = again || s.press != CtlPress::None;
            queuedAgain = queuedAgain || hasEv(s, SeqEvent::EnterQueued);
        }
        check(!again && !queuedAgain && s.sessionActive, "...540 updates later (about 6 s) still not re-pressed, still said once, the session still alive");
        s = q.step(V(3), F5Req::None, false);
        check(s.sessionActive && hasEv(s, SeqEvent::EnterAttached) && s.readyAfter == 5 && s.toMode3 == 542 && s.presses == 1 && s.queued,
              "LATE COMPLETION: mode 3 arrives 542 updates after the press and the session places it: 'ready after 5, mode 3 after 542', one press, pending then accepted");
    }
    // TIMEOUT after the press: 900 updates, the line names what was still unmet.
    {
        ecm::F5Sequencer q;
        q.step(V(1), F5Req::Enter, false);
        run(q, V(1), 4);
        bool aborted = false;
        uint32_t when = 0;
        for (uint32_t i = 1; i <= 1000 && !aborted; ++i) {
            s = q.step(V(1, 0, 0), F5Req::None, false);
            if (hasEv(s, SeqEvent::EnterTimeoutFree)) { aborted = true; when = i; }
        }
        check(aborted && when == ecm::kSeqTabWaitUpdates && !s.sessionActive && (s.unmet & ecm::kUnmetMode) && !(s.unmet & ecm::kUnmetPending) && s.presses == 90 && !s.queued,
              "TIMEOUT after the press: aborts at the 900th update (about 10 s), the session over, unmet = the mode never reached 3, 90 presses (every 10 updates)");
        ecm::F5Sequencer w;
        w.step(V(1), F5Req::Enter, false);
        run(w, V(1), 4);
        for (uint32_t i = 1; i <= 1000; ++i) {
            s = w.step(V(1, 1, 0), F5Req::None, false);
            if (hasEv(s, SeqEvent::EnterTimeoutFree)) break;
        }
        check((s.unmet & ecm::kUnmetMode) && (s.unmet & ecm::kUnmetPending) && s.presses == 1 && s.queued,
              "...with +0x3E1 still 1 the line says the game's own retry was still running, after ONE press (pending stops the pressing)");
    }
    // TIMEOUT of the readiness wait: never ready.
    {
        ecm::F5Sequencer q;
        q.step(V(1), F5Req::Enter, false);
        bool aborted = false;
        uint32_t when = 0;
        for (uint32_t i = 1; i <= 1000 && !aborted; ++i) {
            s = q.step(V(1, 0, 1), F5Req::None, false);
            if (hasEv(s, SeqEvent::EnterTimeoutReady)) { aborted = true; when = i; }
        }
        check(aborted && when == ecm::kSeqTabWaitUpdates - 1 && !s.sessionActive && (s.unmet & ecm::kUnmetShared) && !(s.unmet & ecm::kUnmetMode),
              "TIMEOUT of the readiness wait: the shared +0x1D never clears -> aborts at 900 updates from the ENTER, naming the shared record");
        ecm::F5Sequencer w;
        w.step(V(1), F5Req::Enter, false);
        for (uint32_t i = 1; i <= 1000; ++i) {
            s = w.step(V(1, 1, -1), F5Req::None, false);
            if (hasEv(s, SeqEvent::EnterTimeoutReady)) break;
        }
        check((s.unmet & ecm::kUnmetPending) && (s.unmet & ecm::kSharedUnobserved), "...and with +0x1D unreadable the line says it could not be checked");
    }
    // The player's own TAB, or the camera closing, during the readiness wait.
    {
        ecm::F5Sequencer q;
        q.step(V(1), F5Req::Enter, false);
        run(q, V(1), 2);
        s = q.step(V(3), F5Req::None, false);
        check(s.press == CtlPress::None && s.sessionActive && hasEv(s, SeqEvent::EnterAttached), "the player's own TAB during the wait attaches (nothing pressed)");
        ecm::F5Sequencer w;
        w.step(V(1), F5Req::Enter, false);
        run(w, V(1), 2);
        s = w.step(V(0), F5Req::None, false);
        check(!s.sessionActive && hasEv(s, SeqEvent::SessionEnded), "the camera closing during the wait ends the session");
    }
}

void testFadeGuard() {
    std::printf("the avatar fade global: write and restore\n");
    using ecm::FadeEvent;
    using Act = ecm::FadeStep::Act;
    ecm::FadeGuard g;
    ecm::FadeIn in;
    in.active = true;
    in.readOk = true;
    in.value = -1;
    ecm::FadeStep s = g.step(in);
    check(s.act == Act::None && !g.ours(), "no placement: nothing is written");
    in.placed = true;
    in.session = true;
    in.ctlMode = 4;
    s = g.step(in);
    check(s.act == Act::Write && s.ev == FadeEvent::Written && g.ours(), "a placement and the global reads -1 (auto): WRITE 0");
    in.value = 0;
    s = g.step(in);
    check(s.act == Act::None && s.ev == FadeEvent::None && g.ours(), "...it holds while the placement stands (mode 4, the camera inside the body)");
    // Never restore while the camera is still inside the body.
    in.session = false;
    s = g.step(in);
    check(s.act == Act::None && g.ours(), "NEVER RESTORED while mode is 4 even though the session is over (the camera is still in the body)");
    in.placed = false;
    in.ctlMode = 3;
    s = g.step(in);
    check(s.act == Act::None && g.ours(), "...nor at mode 3 with the placement gone (a stale release): the camera is still in the body");
    in.ctlMode = 0;
    s = g.step(in);
    check(s.act == Act::Restore && s.ev == FadeEvent::Restored && !g.ours(), "RESTORED to -1 once the session is over and the camera is closed (mode 0)");
    in.value = -1;
    s = g.step(in);
    check(s.act == Act::None && s.ev == FadeEvent::None, "...and not again");

    // A wrong initial value is refused, said once, and the next placement may try again.
    ecm::FadeGuard f;
    ecm::FadeIn fi;
    fi.active = true; fi.placed = true; fi.session = true; fi.ctlMode = 3; fi.readOk = true; fi.value = 2;
    s = f.step(fi);
    check(s.act == Act::None && s.ev == FadeEvent::Foreign && !f.ours(), "WRONG INITIAL VALUE (2, not -1): refused, one 'someone else owns it' event, nothing written");
    s = f.step(fi);
    check(s.act == Act::None && s.ev == FadeEvent::None, "...said once, not every frame");
    ecm::FadeGuard h;
    fi.value = 0;
    s = h.step(fi);
    check(s.act == Act::None && s.ev == FadeEvent::Foreign && !h.ours(), "...and a 0 we did not write is someone else's too: refused");
    fi.placed = false; fi.session = false;
    f.step(fi);
    fi.placed = true; fi.session = true; fi.value = -1;
    s = f.step(fi);
    check(s.act == Act::Write && f.ours(), "...the next placement, with the global back at -1: written");

    // A detach (5, 6) with the placement released restores; the return writes again.
    for (int mode : {5, 6}) {
        ecm::FadeGuard d;
        ecm::FadeIn di;
        di.active = true; di.placed = true; di.session = true; di.ctlMode = 4; di.readOk = true; di.value = -1;
        d.step(di);
        di.value = 0;
        di.ctlMode = static_cast<uint8_t>(mode);   // the controller says detached, the placement not yet released
        s = d.step(di);
        check(s.act == Act::None && d.ours(), mode == 5 ? "DETACH (5): not restored while the placement is still in force" : "VARIANT (6): not restored while the placement is still in force");
        di.placed = false;
        s = d.step(di);
        check(s.act == Act::Restore && !d.ours(), "...restored the moment the placement is released with the camera detached");
        di.ctlMode = 3; di.placed = true; di.value = -1;
        s = d.step(di);
        check(s.act == Act::Write && d.ours(), "...and written again when the camera is back and placed");
    }

    // Restore skipped if the value changed under us.
    {
        ecm::FadeGuard c;
        ecm::FadeIn ci;
        ci.active = true; ci.placed = true; ci.session = true; ci.ctlMode = 4; ci.readOk = true; ci.value = -1;
        c.step(ci);
        ci.value = 7; ci.session = false; ci.placed = false; ci.ctlMode = 0;
        s = c.step(ci);
        check(s.act == Act::None && s.ev == FadeEvent::ChangedUnderUs && !c.ours(), "CHANGED UNDER US (7, not the 0 we wrote): the restore is SKIPPED and said, and it is no longer ours");
    }
    // Unreadable: kept and retried, said once.
    {
        ecm::FadeGuard c;
        ecm::FadeIn ci;
        ci.active = true; ci.placed = true; ci.session = true; ci.ctlMode = 4; ci.readOk = true; ci.value = -1;
        c.step(ci);
        ci.session = false; ci.placed = false; ci.ctlMode = 0; ci.readOk = false;
        s = c.step(ci);
        check(s.ev == FadeEvent::Unreadable && c.ours(), "UNREADABLE when it is time to restore: said once, still ours");
        s = c.step(ci);
        check(s.ev == FadeEvent::None && c.ours(), "...not said again");
        ci.readOk = true; ci.value = 0;
        s = c.step(ci);
        check(s.act == Act::Restore && !c.ours(), "...and restored the moment it can be read");
        ecm::FadeGuard w;
        ecm::FadeIn wi;
        wi.active = true; wi.placed = true; wi.session = true; wi.readOk = true; wi.value = -1;
        w.step(wi);
        w.writeFailed();
        check(!w.ours(), "a write that failed leaves it not ours");
        ecm::FadeGuard r;
        r.step(wi);
        ecm::FadeIn ri;
        ri.active = true; ri.session = false; ri.placed = false; ri.ctlMode = 0; ri.readOk = true; ri.value = 0;
        r.step(ri);
        r.restoreFailed();
        check(r.ours(), "a restore that failed stays ours, to be tried again");
    }
    // The feature turning off while the camera is closed restores (the glue passes session = session && active = false).
    {
        ecm::FadeGuard c;
        ecm::FadeIn ci;
        ci.active = true; ci.placed = true; ci.session = true; ci.ctlMode = 4; ci.readOk = true; ci.value = -1;
        c.step(ci);
        ci.active = false; ci.session = false; ci.placed = false; ci.ctlMode = 4; ci.value = 0;
        s = c.step(ci);
        check(s.act == Act::None && c.ours(), "FEATURE OFF while the camera is still open inside the body: held (the camera must close first)");
        ci.ctlMode = 0;
        s = c.step(ci);
        check(s.act == Act::Restore && !c.ours(), "...restored when the camera has closed, though the feature is off");
    }
    // UNLOAD.
    {
        ecm::FadeGuard u;
        ecm::FadeIn ui;
        ui.active = true; ui.placed = true; ui.session = true; ui.ctlMode = 4; ui.readOk = true; ui.value = -1;
        u.step(ui);
        ui.unload = true; ui.value = 0;
        s = u.step(ui);
        check(s.act == Act::Restore && s.ev == FadeEvent::RestoredAtUnload && !u.ours(), "UNLOAD with the global still 0: restored to -1, whatever the camera is doing");
        ecm::FadeGuard v;
        ecm::FadeIn vi;
        vi.active = true; vi.placed = true; vi.session = true; vi.ctlMode = 4; vi.readOk = true; vi.value = -1;
        v.step(vi);
        vi.unload = true; vi.value = 9;
        s = v.step(vi);
        check(s.act == Act::None && s.ev == FadeEvent::ChangedUnderUs, "UNLOAD with the value changed under us: not touched");
        ecm::FadeGuard x;
        vi.value = 0;
        s = x.step(vi);
        check(s.act == Act::None && s.ev == FadeEvent::None, "UNLOAD when it was never ours: nothing");
    }
}

void testSequencerTimeouts() {
    std::printf("F5 sequencer: timeouts and the player's own presses\n");
    ecm::SeqStep s;
    // The camera never opens.
    {
        ecm::F5Sequencer q;
        q.step(V(0), F5Req::Enter, false);
        bool aborted = false;
        uint32_t at = 0;
        for (uint32_t i = 1; i <= 100 && !aborted; ++i) {
            s = q.step(V(0), F5Req::None, false);
            if (hasEv(s, SeqEvent::EnterTimeoutOpen)) { aborted = true; at = i; }
        }
        check(aborted && at == ecm::kSeqWaitUpdates && !s.sessionActive, "the camera never opens: the sequence aborts after 90 updates with a line, and the session is over");
    }
    // The camera closes by the player's hand while waiting for the free camera: the session ends at once.
    {
        ecm::F5Sequencer q;
        q.step(V(1), F5Req::Enter, false);
        s = q.step(V(0), F5Req::None, false);
        check(hasEv(s, SeqEvent::SessionEnded) && !s.sessionActive, "the player closes the camera while the free camera is awaited: the session ends");
    }
    // The player's own camera key and TAB with no F5: nothing is pressed, no session.
    {
        ecm::F5Sequencer q;
        bool any = false;
        const int modes[] = {0, 0, 1, 1, 3, 3, 4, 4, 3, 5, 3, 1, 0, 0};
        for (int m : modes) {
            s = q.step(V(m), F5Req::None, false);
            any = any || s.press != CtlPress::None || s.sessionActive || s.nev != 0;
        }
        check(!any, "the game's own camera key and TAB (modes walking 0,1,3,4,5,3,1,0) with no F5: no press, no session, no event");
    }
    // Detach and re-attach inside a session: the session stays, nothing is pressed.
    {
        ecm::F5Sequencer q;
        q.step(V(3), F5Req::Enter, false);
        bool pressed = false, ended = false;
        for (int m : {3, 4, 5, 5, 3, 4}) {
            s = q.step(V(m), F5Req::None, false);
            pressed = pressed || s.press != CtlPress::None;
            ended = ended || !s.sessionActive;
        }
        check(!pressed && !ended, "detach (5) and re-attach (3, 4) inside a session: the session stays and nothing is pressed");
    }
    // Mode 0 on its own ends an Active session.
    {
        ecm::F5Sequencer q;
        q.step(V(4), F5Req::Enter, false);
        s = q.step(V(0), F5Req::None, false);
        check(hasEv(s, SeqEvent::SessionEnded) && !s.sessionActive, "mode 0 by any route ends the session");
    }
}

void testSequencerExit() {
    std::printf("F5 sequencer: exit\n");
    ecm::SeqStep s;
    // UI hidden by EDVR: wait for it to be given back, then close with PhotoCameraToggle, then the session ends at mode 0.
    {
        ecm::F5Sequencer q;
        q.step(V(4), F5Req::Enter, false);
        s = q.step(V(4), F5Req::Exit, true);
        check(s.press == CtlPress::None && s.exiting && s.sessionActive && hasEv(s, SeqEvent::ExitStart) && hasEv(s, SeqEvent::ExitUnhiding),
              "EXIT with the UI hidden by EDVR: no press yet, 'giving the UI back first', exiting");
        s = q.step(V(4), F5Req::None, true);
        check(s.press == CtlPress::None && s.exiting, "...it waits while the UI is still held");
        s = q.step(V(4), F5Req::None, false);
        check(s.press == CtlPress::Photo && hasEv(s, SeqEvent::ExitClosing), "...the UI is back: PhotoCameraToggle is pressed to close");
        s = q.step(V(4), F5Req::None, false);
        check(s.press == CtlPress::None && s.sessionActive, "...one press only; the close is awaited");
        s = q.step(V(0), F5Req::None, false);
        check(hasEv(s, SeqEvent::ExitDone) && !s.sessionActive && !s.exiting, "...mode 0: the session is over");
    }
    // UI not hidden: close at once.
    {
        ecm::F5Sequencer q;
        q.step(V(3), F5Req::Enter, false);
        s = q.step(V(3), F5Req::Exit, false);
        check(s.press == CtlPress::Photo && hasEv(s, SeqEvent::ExitStart) && hasEv(s, SeqEvent::ExitClosing), "EXIT with the UI showing: PhotoCameraToggle at once");
    }
    // The UI never comes back: close anyway after the wait.
    {
        ecm::F5Sequencer q;
        q.step(V(3), F5Req::Enter, false);
        q.step(V(3), F5Req::Exit, true);
        bool closed = false;
        uint32_t at = 0;
        for (uint32_t i = 1; i <= 100 && !closed; ++i) {
            s = q.step(V(3), F5Req::None, true);
            if (s.press == CtlPress::Photo) { closed = true; at = i; }
        }
        check(closed && at == ecm::kSeqWaitUpdates && hasEv(s, SeqEvent::ExitUnhideTimeout), "the UI never comes back: the camera is closed anyway after 90 updates, with a line");
    }
    // The camera never closes.
    {
        ecm::F5Sequencer q;
        q.step(V(3), F5Req::Enter, false);
        q.step(V(3), F5Req::Exit, false);
        bool aborted = false;
        for (uint32_t i = 1; i <= 100 && !aborted; ++i) {
            s = q.step(V(3), F5Req::None, false);
            aborted = hasEv(s, SeqEvent::ExitTimeout);
        }
        check(aborted && !s.sessionActive, "the camera never closes: aborts after 90 updates, the session is over");
    }
    // EXIT while still opening, and while waiting for readiness.
    {
        ecm::F5Sequencer q;
        q.step(V(0), F5Req::Enter, false);
        s = q.step(V(1), F5Req::Exit, false);
        check(s.press == CtlPress::Photo && hasEv(s, SeqEvent::ExitStart), "EXIT in the middle of entering: stops entering and closes");
        ecm::F5Sequencer w;
        w.step(V(1), F5Req::Enter, false);
        run(w, V(1), 2);
        s = w.step(V(1), F5Req::Exit, false);
        check(s.press == CtlPress::Photo && hasEv(s, SeqEvent::ExitStart), "EXIT during the readiness wait: stops waiting and closes");
    }
    // EXIT with the camera already closed, and EXIT with no session.
    {
        ecm::F5Sequencer q;
        q.step(V(3), F5Req::Enter, false);
        s = q.step(V(0), F5Req::Exit, false);
        check(hasEv(s, SeqEvent::ExitDone) && !s.sessionActive, "EXIT when the camera has already closed: done at once");
        s = q.step(V(3), F5Req::Exit, false);
        check(hasEv(s, SeqEvent::ExitIgnored) && s.press == CtlPress::None, "EXIT with no session is ignored");
    }
}

void testF5Decision() {
    std::printf("F5's decision table (ENTER only on foot, in first person or the on-foot camera, with no panel open)\n");
    using ecm::F5Action;
    auto base = []() {
        ecm::F5Inputs in;
        in.pressed = true; in.gameplay = true; in.active = true; in.controllerAlive = true; in.mode = 0;
        in.onFootKnown = true; in.onFoot = true; in.focusKnown = true; in.focus = 0;
        return in;
    };
    ecm::F5Inputs in = base();
    check(ecm::decideF5(in) == F5Action::Enter, "on foot, first person (camera closed), no panel: ENTER");
    // The camera suite and the free camera, on foot with no panel.
    bool allEnter = true;
    for (int m : {1, 2, 3, 4, 5, 6}) {
        in = base();
        in.mode = static_cast<uint8_t>(m);
        allEnter = allEnter && ecm::decideF5(in) == F5Action::Enter;
    }
    check(allEnter, "...and in every camera mode 1-6 on foot with no panel: ENTER (the sequence itself refuses a detached camera in its own words)");
    // ON FOOT: unknown and false refuse in EVERY mode (the camera suite open in a ship or an SRV never enters).
    bool unknownRefused = true, falseRefused = true;
    for (int m : {0, 1, 2, 3, 4}) {
        in = base();
        in.mode = static_cast<uint8_t>(m);
        in.onFootKnown = false;
        unknownRefused = unknownRefused && ecm::decideF5(in) == F5Action::RefuseNotOnFoot;
        in = base();
        in.mode = static_cast<uint8_t>(m);
        in.onFoot = false;
        falseRefused = falseRefused && ecm::decideF5(in) == F5Action::RefuseNotOnFoot;
    }
    check(unknownRefused, "ON FOOT UNKNOWN (no Status.json Flags2): refused in modes 0, 1, 2, 3 and 4");
    check(falseRefused, "ON FOOT FALSE (in a ship, an SRV): refused in modes 0, 1, 2, 3 and 4 -- the camera suite open in a ship never enters");
    in = base();
    in.onFootKnown = true; in.onFoot = false; in.focusKnown = false; in.controllerAlive = false;
    check(ecm::decideF5(in) == F5Action::RefuseNotOnFoot, "...on foot is checked before the panel and before the idle controller");
    // THE PANEL: a known non-zero focus refuses in every mode. An ABSENT GuiFocus (F7: on foot, Status.json has no GuiFocus field at all when no panel has the
    // focus) with on foot known true reads as 0 and enters in every mode.
    bool focusRefused = true;
    for (uint32_t f : {1u, 5u, 6u, 7u, 9u, 11u}) {
        for (int m : {0, 1, 2, 3, 4}) {
            in = base();
            in.mode = static_cast<uint8_t>(m);
            in.focus = f;
            focusRefused = focusRefused && ecm::decideF5(in) == F5Action::RefuseFocus;
        }
    }
    check(focusRefused, "GUI FOCUS KNOWN AND NON-ZERO (a panel, the maps, station services, the FSS...): refused in modes 0, 1, 2, 3 and 4");
    bool absentEnters = true;
    for (int m : {0, 1, 2, 3, 4, 5, 6}) {
        in = base();
        in.mode = static_cast<uint8_t>(m);
        in.focusKnown = false;
        absentEnters = absentEnters && ecm::decideF5(in) == F5Action::Enter;
    }
    check(absentEnters, "GUI FOCUS ABSENT (no GuiFocus field, on foot known true), F7's case: ENTER in every mode, first person included -- the field is left out when nothing has the focus");
    in = base();
    in.focusKnown = false;
    in.focus = 9;   // a stale value next to an absent field is not read
    check(ecm::decideF5(in) == F5Action::Enter, "...an absent field is 0 whatever the value next to it holds");
    in = base();
    in.focusKnown = false;
    in.onFootKnown = false;
    in.mode = 2;
    check(ecm::decideF5(in) == F5Action::RefuseNotOnFoot, "...but never with on foot unknown as well (a fully absent Status.json has no Flags2 either)");
    in = base();
    in.focusKnown = true;
    in.focus = 0;
    check(ecm::decideF5(in) == F5Action::Enter, "GUI FOCUS KNOWN AND 0: ENTER");
    // EXIT is always allowed while a session is on.
    bool exitAlways = true;
    for (int m : {0, 1, 2, 3, 4, 5, 6}) {
        for (int k = 0; k < 4; ++k) {
            in = base();
            in.sessionActive = true;
            in.mode = static_cast<uint8_t>(m);
            in.onFootKnown = k != 0; in.onFoot = k == 2;
            in.focusKnown = k != 1; in.focus = k == 3 ? 5 : 0;
            exitAlways = exitAlways && ecm::decideF5(in) == F5Action::Exit;
        }
    }
    check(exitAlways, "EXIT in an Explorer Cam session: allowed in every mode with on foot unknown, false, true and any focus, known, unknown or a panel");
    in = base();
    in.controllerAlive = false;
    check(ecm::decideF5(in) == F5Action::RefuseControllerIdle, "the controller is not being called (on foot, no panel): refused with 'the camera controller is idle: open the camera first'");
    in = base();
    in.gameplay = false;
    check(ecm::decideF5(in) == F5Action::None, "the journal says not in gameplay (menus): the press is ignored");
    in = base();
    in.active = false;
    check(ecm::decideF5(in) == F5Action::RefuseOff, "Explorer Cam off or a hook stood down: refused, said so");
    in = base();
    in.pressed = false;
    check(ecm::decideF5(in) == F5Action::None, "no press, no action");
}

// ---- the camera UI hider ------------------------------------------------------------------------------------------------------
void testUiHider() {
    std::printf("the camera UI hider\n");
    using ecm::UiEvent;
    constexpr uint64_t kUi = 0x7100, kUi2 = 0x7200;
    ecm::UiObserved shown, hiddenUi, noHandle;
    shown.handle = true; shown.hidden = 0;
    hiddenUi.handle = true; hiddenUi.hidden = 1;
    noHandle.handle = false; noHandle.hidden = 0;
    ecm::UiHider h;
    ecm::UiStep s = h.step(kUi, shown, false);
    check(!s.press && !h.hiddenByUs(), "not placed: the UI is left alone");
    s = h.step(kUi, shown, true);
    check(s.press && h.pending() && !h.hiddenByUs(), "placed and the UI showing: FreeCamToggleHUD is pressed (once)");
    s = h.step(kUi, hiddenUi, true);
    check(!s.press && s.ev == UiEvent::Hidden && h.hiddenByUs() && !h.pending(), "...the next update reads +0x1A0 = 1: 'hidden', EDVR remembers it hid it");
    s = h.step(kUi, hiddenUi, true);
    check(!s.press && s.ev == UiEvent::None, "...and does not press again while placed");
    s = h.step(kUi, hiddenUi, false);
    check(s.press && h.pending(), "the placement ends and +0x1A0 is still 1: pressed again to give it back");
    s = h.step(kUi, shown, false);
    check(!s.press && s.ev == UiEvent::Unhidden && !h.hiddenByUs() && !h.pending(), "...+0x1A0 is 0 again: 'unhidden', EDVR no longer holds it");
    // A new placement hides again.
    s = h.step(kUi, shown, true);
    s = h.step(kUi, hiddenUi, true);
    check(h.hiddenByUs(), "a new placement (detach and return) hides it again");
    // The UI hidden by the player already: no press, nothing to give back.
    ecm::UiHider p;
    s = p.step(kUi, hiddenUi, true);
    check(!s.press && !p.hiddenByUs(), "the player already hid the UI: no press");
    s = p.step(kUi, hiddenUi, false);
    check(!s.press, "...and nothing is given back that EDVR did not take");
    // The player shows the UI again while placed: EDVR leaves it.
    ecm::UiHider q;
    q.step(kUi, shown, true);
    q.step(kUi, hiddenUi, true);
    s = q.step(kUi, shown, true);
    check(!s.press && s.ev == UiEvent::UserShowed && !q.hiddenByUs(), "the player shows the UI again while placed: EDVR leaves it (no fight)");
    // The press did nothing.
    ecm::UiHider r;
    r.step(kUi, shown, true);
    s = r.step(kUi, shown, true);
    check(s.ev == UiEvent::HideNoEffect && !r.hiddenByUs() && !s.press, "the hide press did not change +0x1A0: said so, not repeated");
    // The handle is NULL: said once, never a fault.
    ecm::UiHider n;
    s = n.step(kUi, noHandle, false);
    check(s.ev == UiEvent::None && !s.press, "no handle and not placed: nothing to say");
    s = n.step(kUi, noHandle, true);
    check(s.ev == UiEvent::NoHandle && !s.press, "no handle while placed: 'the game offers no hide-UI here', no press");
    s = n.step(kUi, noHandle, true);
    check(s.ev == UiEvent::None, "...said once");
    // A different UI object (a new visit) forgets the old one.
    ecm::UiHider o;
    o.step(kUi, shown, true);
    o.step(kUi, hiddenUi, true);
    s = o.step(kUi2, shown, true);
    check(s.press && !o.hiddenByUs(), "a different camera UI object starts clean: the old hide is forgotten and the new UI is hidden");
}

void testUiSettled() {
    std::printf("the camera UI's hide: when it has run its course (pure)\n");
    constexpr uint64_t kUi = 0x7100;
    const ecm::UiObserved shown{true, 0}, hidden{true, 1}, noHandle{false, 0};
    ecm::UiHider h;
    check(!h.settledForPlacement(), "A FRESH HIDER is not settled");
    h.step(kUi, shown, false);
    check(!h.settledForPlacement(), "...nor when the hide is not wanted");
    ecm::UiStep st = h.step(kUi, shown, true);
    check(st.press && !h.settledForPlacement(), "THE PRESS IS OUT but its result is not read: not settled");
    h.step(kUi, hidden, true);
    check(h.settledForPlacement(), "...the next update reads the UI hidden: settled");
    h.step(kUi, hidden, false);
    check(!h.settledForPlacement(), "...and when the hide is no longer wanted (the exit) it is not");
    ecm::UiHider again;
    again.step(kUi, shown, true);
    again.step(kUi, shown, true);   // the press did nothing
    check(again.settledForPlacement(), "A PRESS THAT HAD NO EFFECT is settled too (the fade does not wait for what will not happen)");
    ecm::UiHider mine;
    mine.step(kUi, hidden, true);
    check(mine.settledForPlacement(), "A UI THE PLAYER ALREADY HID is settled");
    ecm::UiHider none;
    none.step(kUi, noHandle, true);
    check(none.settledForPlacement(), "NO HANDLE TO PRESS is settled (nothing will ever hide)");
    ecm::UiHider fresh2;
    fresh2.step(kUi, hidden, true);
    fresh2.step(kUi + 0x100, shown, false);
    check(!fresh2.settledForPlacement(), "A DIFFERENT UI OBJECT forgets the old one's state");
    ecm::UiHider unwanted;
    unwanted.step(kUi, hidden, false);
    check(!unwanted.settledForPlacement(), "A UI THE PLAYER HID with the hide not wanted (no placement) is not settled: it counts only for the placement that wants it");
}

void testWatches() {
    std::printf("the stale and liveness watches\n");
    ecm::StaleWatch w;
    bool released = false;
    for (int i = 0; i < 5; ++i) released = released || w.tick(true, 100 + i);
    check(!released, "an activity called every frame is never released");
    int frames = 0;
    bool hit = false;
    for (; frames < 40 && !hit; ++frames) hit = w.tick(true, 104);
    check(hit && frames == 30, "an activity not called for 30 frames is released on the 30th");
    check(!w.tick(true, 104), "...once: the watch starts over");
    ecm::StaleWatch v;
    for (int i = 0; i < 29; ++i) v.tick(true, 7);
    check(!v.tick(false, 7), "an idle machine is never stale");
    for (int i = 0; i < 29; ++i) v.tick(true, 7);
    check(!v.tick(true, 8), "a call at the 30th frame resets the count");

    ecm::LiveWatch l;
    check(!l.alive(), "a counter that has never moved is not alive");
    l.tick(0);
    l.tick(0);
    check(!l.alive(), "...even after ticks");
    l.tick(5);
    check(l.alive(), "a counter that moved is alive");
    for (int i = 0; i < 29; ++i) l.tick(5);
    check(l.alive(), "...for 29 silent ticks");
    l.tick(5);
    check(!l.alive(), "...and not at the 30th");
    l.tick(6);
    check(l.alive(), "...and alive again when it moves");
}

void testRing() {
    std::printf("the event ring\n");
    ecm::EventRing<4> ring;
    ecm::Event e;
    check(!ring.take(&e), "empty: nothing to take");
    for (uint64_t i = 1; i <= 6; ++i) {
        ecm::Event p;
        p.activity = i;
        ring.push(p);
    }
    check(ring.lost() == 2, "six pushes into four slots: two lost, counted");
    uint64_t sum = 0;
    int n = 0;
    while (ring.take(&e)) { sum += e.activity; ++n; }
    check(n == 4 && sum == 1 + 2 + 3 + 4, "the four oldest come out in order");
    ecm::Event p;
    p.activity = 9;
    ring.push(p);
    check(ring.take(&e) && e.activity == 9, "the ring reuses its slots");
}

void testText() {
    std::printf("log lines\n");
    char line[ecm::kLineBytes];
    ecm::Event e;
    e.activity = 0x1234ABCD;
    e.threadId = 77;
    e.flags = ecm::packObserved(obs(1, 1, 0, 3));
    e.after = 3;

    e.kind = static_cast<uint32_t>(ecm::EvKind::FirstCall);
    ecm::formatEvent(line, sizeof(line), e, 5);
    check(std::strncmp(line, "explorer cam: first free-camera update reached the hook", 55) == 0 && has(line, "act=0x1234ABCD") && has(line, "thread=77") && has(line, "frame=5"),
          "FirstCall: 'explorer cam: first free-camera update reached the hook' with the activity, thread and frame");
    e.kind = static_cast<uint32_t>(ecm::EvKind::Entered);
    ecm::formatEvent(line, sizeof(line), e, 6);
    check(std::strncmp(line, "explorer cam: entered the free camera", 37) == 0 && has(line, "+0x48C=3"), "Entered: 'explorer cam: entered the free camera' with +0x48C");
    e.kind = static_cast<uint32_t>(ecm::EvKind::Placed);
    e.eye[0] = 1.68f; e.eye[1] = 0.10f; e.eye[2] = 0.0f;
    ecm::formatEvent(line, sizeof(line), e, 7);
    check(std::strncmp(line, "explorer cam: placed:", 21) == 0 && has(line, "act=0x1234ABCD") && has(line, "eye(up=1.680 forward=0.100 right=0.000)") && has(line, "box push"),
          "Placed: 'explorer cam: placed:' with the activity and the eye values to 3 decimals, naming the collision step and the box push");
    e.kind = static_cast<uint32_t>(ecm::EvKind::LockResult);
    e.before = 3; e.after = 4;
    ecm::formatEvent(line, sizeof(line), e, 8);
    check(std::strncmp(line, "explorer cam: lock pressed:", 27) == 0 && has(line, "before=3 after=4") && has(line, "relative lock is on"),
          "LockResult: 'explorer cam: lock pressed:' with the state before and after");
    e.after = 3;
    ecm::formatEvent(line, sizeof(line), e, 8);
    check(has(line, "before=3 after=3") && has(line, "did not move the state to 4"), "...and a press that did nothing says so");
    e.kind = static_cast<uint32_t>(ecm::EvKind::Released);
    e.why = static_cast<uint32_t>(ecm::Why::WorldLock);
    e.updates = 321;
    ecm::formatEvent(line, sizeof(line), e, 9);
    check(std::strncmp(line, "explorer cam: released:", 23) == 0 && has(line, "detached") && has(line, "pose writes so far=321"), "Released: 'explorer cam: released:' with the reason and the write count");
    e.kind = static_cast<uint32_t>(ecm::EvKind::Fault);
    e.why = static_cast<uint32_t>(ecm::FaultSite::SetUiPress);
    e.count = 3;
    ecm::formatEvent(line, sizeof(line), e, 10);
    check(std::strncmp(line, "explorer cam: fault 3 of 8:", 27) == 0 && has(line, "hide press"), "Fault: 'explorer cam: fault 3 of 8:' naming where");
    e.kind = static_cast<uint32_t>(ecm::EvKind::FaultLimit);
    e.count = 8;
    ecm::formatEvent(line, sizeof(line), e, 11);
    check(std::strncmp(line, "explorer cam: stood down for the session:", 41) == 0 && has(line, "8 guarded accesses"), "FaultLimit: 'explorer cam: stood down for the session:'");

    ecm::Event q;
    q.kind = static_cast<uint32_t>(ecm::EvKind::Seq);
    q.activity = 0xC0DE;
    q.why = static_cast<uint32_t>(SeqEvent::EnterTimeoutFree);
    q.after = 2;
    q.count = 1;
    ecm::formatEvent(line, sizeof(line), q, 12);
    check(std::strncmp(line, "explorer cam: F5 enter aborted:", 31) == 0 && has(line, "preset kind +0x2E8 = 1") && has(line, "ctl=0xC0DE"), "Seq: an aborted enter names the preset kind (+0x2E8)");
    q.why = static_cast<uint32_t>(SeqEvent::EnterRefusedDetached);
    ecm::formatEvent(line, sizeof(line), q, 12);
    check(has(line, "detached") && has(line, "leave it first"), "Seq: a detached camera 'must be left first'");
    q.why = static_cast<uint32_t>(SeqEvent::ExitDone);
    ecm::formatEvent(line, sizeof(line), q, 12);
    check(has(line, "F5 exit: the camera is closed") && has(line, "session is over"), "Seq: the exit says the camera is closed and the session over");
    q.kind = static_cast<uint32_t>(ecm::EvKind::Ui);
    q.why = static_cast<uint32_t>(ecm::UiEvent::NoHandle);
    ecm::formatEvent(line, sizeof(line), q, 12);
    check(std::strncmp(line, "explorer cam: the game offers no hide-UI here", 45) == 0, "Ui: a NULL handle says 'the game offers no hide-UI here'");
    q.kind = static_cast<uint32_t>(ecm::EvKind::ControllerFirstCall);
    q.after = 0;
    ecm::formatEvent(line, sizeof(line), q, 12);
    check(has(line, "camera controller update reached the hook") && has(line, "mode=0") && has(line, "runs with the camera closed"), "ControllerFirstCall at mode 0 says it runs with the camera closed");
    q.after = 1;
    ecm::formatEvent(line, sizeof(line), q, 12);
    check(!has(line, "runs with the camera closed"), "...and says nothing of the sort at mode 1");

    ecm::F5Inputs f5in;
    f5in.pressed = true; f5in.gameplay = true; f5in.active = true; f5in.controllerAlive = true; f5in.mode = 0;
    f5in.onFootKnown = true; f5in.onFoot = true; f5in.focusKnown = true; f5in.focus = 0;
    ecm::formatF5(line, sizeof(line), ecm::F5Action::RefuseControllerIdle, f5in);
    check(has(line, "the camera controller is idle: open the camera first"), "F5 on an idle controller: 'the camera controller is idle: open the camera first'");
    f5in.mode = 3;
    ecm::formatF5(line, sizeof(line), ecm::F5Action::Enter, f5in);
    check(has(line, "F5 pressed: entering Explorer Cam (camera mode 3") && has(line, "on foot: yes") && has(line, "GuiFocus: 0 (none)"), "F5 ENTER names the camera mode, on-foot and the GuiFocus");
    f5in.mode = 0; f5in.onFoot = false;
    f5in.focusKnown = true; f5in.focus = 5;
    ecm::formatF5(line, sizeof(line), ecm::F5Action::RefuseNotOnFoot, f5in);
    check(has(line, "only starts on foot") && has(line, "on foot = no") && has(line, "GuiFocus = 5 (station services)") && has(line, "lags about 6 s after you step out") && has(line, "may need a moment"),
          "THE NOT-ON-FOOT REFUSAL names the on-foot value (no) and the GuiFocus (5, station services) and says Status.json's OnFoot lags about 6 s after a disembark");
    f5in.onFootKnown = false; f5in.focusKnown = false;
    ecm::formatF5(line, sizeof(line), ecm::F5Action::RefuseNotOnFoot, f5in);
    check(has(line, "on foot = unknown (Status.json has no Flags2") && has(line, "GuiFocus = absent") && has(line, "taken as 0") && has(line, "lags about 6 s"),
          "...and for an unknown on-foot state (and an absent GuiFocus) says unknown and absent (taken as 0), and the same lag");
    f5in.onFootKnown = true; f5in.onFoot = true; f5in.focusKnown = true; f5in.focus = 9;
    ecm::formatF5(line, sizeof(line), ecm::F5Action::RefuseFocus, f5in);
    check(has(line, "a panel has the focus") && has(line, "GuiFocus = 9 (the FSS)") && has(line, "on foot = yes") && !has(line, "while its own camera suite is open"),
          "THE PANEL REFUSAL names the focus (9, the FSS) and on foot = yes");
    f5in.mode = 2;
    ecm::formatF5(line, sizeof(line), ecm::F5Action::RefuseFocus, f5in);
    check(has(line, "The game reports a non-zero GuiFocus while its own camera suite is open"), "...inside the camera suite it adds that the game reports a non-zero GuiFocus while its own camera suite is open");
    f5in.mode = 0; f5in.focusKnown = false;
    ecm::formatF5(line, sizeof(line), ecm::F5Action::Enter, f5in);
    check(has(line, "entering Explorer Cam") && has(line, "GuiFocus: absent") && has(line, "taken as 0"), "THE ENTER LINE for an absent GuiFocus says absent, taken as 0");

    ecm::HeartbeatIn h;
    h.windowSeconds = 5.0; h.phase = "placed"; h.activity = 0xABC; h.state = 4; h.session = true;
    h.updates = 450; h.updatesWindow = 450; h.bypassed = 450; h.bypassedWindow = 450; h.forwarded = 3; h.forwardedWindow = 3;
    h.boxBypassed = 449; h.boxBypassedWindow = 449; h.boxForwarded = 2; h.boxForwardedWindow = 2;
    h.hookCalls = 460; h.hookCallsWindow = 460; h.ctlCalls = 900; h.ctlCallsWindow = 450; h.ctlMode = 4; h.uiHiddenByUs = true; h.uiCalls = 12; h.faults = 1;
    h.eye = ecm::clampEye(1.68f, 0.10f, 0.0f);
    ecm::formatHeartbeat(line, sizeof(line), h);
    check(std::strncmp(line, "explorer cam: heartbeat: phase=placed session=on", 48) == 0 && has(line, "updates_placed=450(+450)") && has(line, "collision_bypassed=450(+450)") &&
              has(line, "collision_forwarded=3(+3)") && has(line, "box_bypassed=449(+449)") && has(line, "box_forwarded=2(+2)") && has(line, "hook_calls=460(+460)") &&
              has(line, "controller_calls=900(+450)") && has(line, "controller_mode=4") && has(line, "ui_hidden_by_edvr=yes") && has(line, "faults=1"),
          "the heartbeat names updates placed, collision and box-push calls bypassed and forwarded, hook calls, the controller's calls and mode, the UI, faults");
    Capture cap;
    cap.lines.push_back("explorer cam probe I3 heartbeat: x");
    cap.lines.push_back("explorer cam probe: on");
    check(cap.prefixed() == 0, "the probe's lines do not match this feature's 'explorer cam:' prefix");
}

void testRelayBytes() {
    std::printf("the relays' machine code\n");
    uint8_t c[ecm::kBypassRelayBytes] = {};
    uint64_t placed = 0, bypassed = 0, forwarded = 0;
    ecm::buildBypassRelay(c, &placed, &bypassed, &forwarded);
    uintptr_t a = 0;
    std::memcpy(&a, c + ecm::kBypassRelayPlacedAt, 8);
    check(a == reinterpret_cast<uintptr_t>(&placed), "the placed-activity literal is the address passed in");
    std::memcpy(&a, c + ecm::kBypassRelayBypassedAt, 8);
    check(a == reinterpret_cast<uintptr_t>(&bypassed), "the bypassed-counter literal is the address passed in");
    std::memcpy(&a, c + ecm::kBypassRelayForwardedAt, 8);
    check(a == reinterpret_cast<uintptr_t>(&forwarded), "the forwarded-counter literal is the address passed in");
    // The two short jumps land on `forward:` (offset 40), and the xor/ret sits before it.
    check(c[16] == 0x74 && 18 + c[17] == 40 && c[21] == 0x75 && 23 + c[22] == 40, "both short jumps land on the forward block (offset 40)");
    check(c[37] == 0x31 && c[38] == 0xC0 && c[39] == 0xC3, "the bypass block ends `xor eax,eax; ret`");
    check(c[54] == 0xFF && c[55] == 0x25 && ecm::kBypassRelayTrampolineAt == 60, "the forward block ends `jmp [rip+0]` with the trampoline literal at 60");
    uint8_t f[ecm::kCallbackRelayBytes] = {};
    int gate = 0, callback = 0;
    ecm::buildCallbackRelay(f, &gate, &callback);
    std::memcpy(&a, f + ecm::kCallbackRelayGateAt, 8);
    check(a == reinterpret_cast<uintptr_t>(&gate), "the callback relay's gate literal is the address passed in");
    std::memcpy(&a, f + ecm::kCallbackRelayCallbackAt, 8);
    check(a == reinterpret_cast<uintptr_t>(&callback) && 16 + 14 == 30 && f[15] == 0x0E, "...its callback literal too, and `je original` lands on the trampoline jump (offset 30)");
}

// ================================ Part B: the glue, end to end ================================
// Machine code for the synthetic functions (each body disassembled against capstone when written).
using Bytes = std::vector<uint8_t>;
void emit(Bytes& b, std::initializer_list<uint8_t> bytes) { for (uint8_t x : bytes) b.push_back(x); }
void prologue(Bytes& b, const uint8_t* p, size_t n) { for (size_t i = 0; i < n; ++i) b.push_back(p[i]); }

// A snippet every synthetic update carries right after its prologue: it calls a C++ probe (when one is set) with rcx = the object, WHILE the original runs, so a
// cell sees what the game's own update would read. `saveReg` 0 = rbx, 1 = r14 (a register the prologue saves), -1 = rbx already holds rcx; `align` adds the
// 28h of stack a frameless function needs for the call.
using ProbeFn = void (__fastcall*)(void*);
ProbeFn g_probeFree = nullptr, g_probeCtl = nullptr, g_probeUi = nullptr, g_probeZoom = nullptr;
void emitProbe(Bytes& b, ProbeFn* slot, int saveReg, bool align) {
    if (saveReg == 0) emit(b, {0x48, 0x89, 0xCB});          // mov rbx, rcx
    else if (saveReg == 1) emit(b, {0x49, 0x89, 0xCE});     // mov r14, rcx
    emit(b, {0x48, 0xB8});
    const uint64_t at = reinterpret_cast<uint64_t>(slot);
    for (int i = 0; i < 8; ++i) b.push_back(static_cast<uint8_t>(at >> (8 * i)));   // mov rax, imm64 (&slot)
    emit(b, {0x48, 0x8B, 0x00});                            // mov rax, [rax]
    emit(b, {0x48, 0x85, 0xC0});                            // test rax, rax
    const size_t jeAt = b.size();
    emit(b, {0x74, 0x00});                                  // je skip (patched)
    if (align) emit(b, {0x48, 0x83, 0xEC, 0x28});           // sub rsp, 28h
    emit(b, {0xFF, 0xD0});                                  // call rax
    if (align) emit(b, {0x48, 0x83, 0xC4, 0x28});           // add rsp, 28h
    b[jeAt + 1] = static_cast<uint8_t>(b.size() - (jeAt + 2));
    if (saveReg == 1) emit(b, {0x4C, 0x89, 0xF1});          // mov rcx, r14
    else emit(b, {0x48, 0x89, 0xD9});                       // mov rcx, rbx
}

// The free-camera update: the real 28-byte prologue, then a body that records what it saw.
//   activity+0x540 = the lock action's pressed-int; +0x544 += 1 (calls); +0x548/54C/550 = the pose's origin; +0x554 = the state byte.
constexpr uint32_t kSeenInt = 0x540, kSeenCalls = 0x544, kSeenX = 0x548, kSeenY = 0x54C, kSeenZ = 0x550, kSeenState = 0x554, kRan = 0x558;
Bytes buildFreeSynthetic(bool corrupt) {
    Bytes b;
    prologue(b, ecm::kFreeCameraPrologue, sizeof(ecm::kFreeCameraPrologue));
    emitProbe(b, &g_probeFree, 0, false);
    emit(b, {0x48, 0x8B, 0x81, 0x08, 0x05, 0x00, 0x00});          // mov rax, [rcx+508h]
    emit(b, {0x8B, 0x40, 0x1C});                                  // mov eax, [rax+1Ch]
    emit(b, {0x89, 0x81, 0x40, 0x05, 0x00, 0x00});                // mov [rcx+540h], eax
    emit(b, {0xFF, 0x81, 0x44, 0x05, 0x00, 0x00});                // inc dword ptr [rcx+544h]
    emit(b, {0x8B, 0x81, 0xE0, 0x03, 0x00, 0x00});                // mov eax, [rcx+3E0h]
    emit(b, {0x89, 0x81, 0x48, 0x05, 0x00, 0x00});                // mov [rcx+548h], eax
    emit(b, {0x8B, 0x81, 0xE4, 0x03, 0x00, 0x00});                // mov eax, [rcx+3E4h]
    emit(b, {0x89, 0x81, 0x4C, 0x05, 0x00, 0x00});                // mov [rcx+54Ch], eax
    emit(b, {0x8B, 0x81, 0xE8, 0x03, 0x00, 0x00});                // mov eax, [rcx+3E8h]
    emit(b, {0x89, 0x81, 0x50, 0x05, 0x00, 0x00});                // mov [rcx+550h], eax
    emit(b, {0x0F, 0xB6, 0x81, 0x8C, 0x04, 0x00, 0x00});          // movzx eax, byte ptr [rcx+48Ch]
    emit(b, {0x89, 0x81, 0x54, 0x05, 0x00, 0x00});                // mov [rcx+554h], eax
    emit(b, {0x48, 0xB8, 0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11});   // mov rax, 1122334455667788h
    emit(b, {0x48, 0x81, 0xC4, 0xC0, 0x03, 0x00, 0x00});          // add rsp, 3C0h
    emit(b, {0x41, 0x5F, 0x41, 0x5E, 0x41, 0x5D, 0x5F, 0x5D});    // pop r15; pop r14; pop r13; pop rdi; pop rbp
    emit(b, {0x48, 0x8B, 0x5C, 0x24, 0x20});                      // mov rbx, [rsp+20h]
    emit(b, {0xC3});                                              // ret
    if (corrupt) b[14] ^= 0x01;
    return b;
}
// The collision sweep: the real 26-byte prologue, then a body that counts its run in the rcx object (+0x558) and returns
// rdx + r8 + r9 + (the byte pushed as the fifth argument << 32). The fifth argument is at [rsp+2B0h] after the prologue.
Bytes buildCollisionSynthetic(bool corrupt) {
    Bytes b;
    prologue(b, ecm::kCollisionPrologue, sizeof(ecm::kCollisionPrologue));
    emit(b, {0xFF, 0x81, 0x58, 0x05, 0x00, 0x00});                // inc dword ptr [rcx+558h]
    emit(b, {0x48, 0x89, 0xD0});                                  // mov rax, rdx
    emit(b, {0x4C, 0x01, 0xC0});                                  // add rax, r8
    emit(b, {0x4C, 0x01, 0xC8});                                  // add rax, r9
    emit(b, {0x44, 0x0F, 0xB6, 0x94, 0x24, 0xB0, 0x02, 0x00, 0x00});   // movzx r10d, byte ptr [rsp+2B0h]
    emit(b, {0x49, 0xC1, 0xE2, 0x20});                            // shl r10, 32
    emit(b, {0x4C, 0x09, 0xD0});                                  // or rax, r10
    emit(b, {0x48, 0x81, 0xC4, 0x50, 0x02, 0x00, 0x00});          // add rsp, 250h
    emit(b, {0x41, 0x5F, 0x41, 0x5E, 0x41, 0x5C, 0x5F, 0x5E, 0x5B, 0x5D});   // pop r15; pop r14; pop r12; pop rdi; pop rsi; pop rbx; pop rbp
    emit(b, {0xC3});
    if (corrupt) b[15] ^= 0x01;
    return b;
}
// The box push: int f(activity, point*). Counts its run (+0x558), reads the point's first dword, rewrites it to 0x11111111 and
// returns that dword + 3.
Bytes buildBoxSynthetic(bool corrupt) {
    Bytes b;
    prologue(b, ecm::kBoxPushPrologue, sizeof(ecm::kBoxPushPrologue));
    emit(b, {0xFF, 0x81, 0x58, 0x05, 0x00, 0x00});                // inc dword ptr [rcx+558h]
    emit(b, {0x8B, 0x02});                                        // mov eax, [rdx]
    emit(b, {0x83, 0xC0, 0x03});                                  // add eax, 3
    emit(b, {0xC7, 0x02, 0x11, 0x11, 0x11, 0x11});                // mov dword ptr [rdx], 11111111h
    emit(b, {0x48, 0x81, 0xC4, 0x80, 0x00, 0x00, 0x00});          // add rsp, 80h
    emit(b, {0x41, 0x5F, 0x41, 0x5E, 0x5E, 0x5B, 0x5D});          // pop r15; pop r14; pop rsi; pop rbx; pop rbp
    emit(b, {0xC3});
    if (corrupt) b[16] ^= 0x01;
    return b;
}
// The camera UI's update: records the hide action's pressed-int (+0x300), the hidden byte (+0x304) and the call count (+0x308).
Bytes buildUiSynthetic(bool corrupt) {
    Bytes b;
    prologue(b, ecm::kCameraUiPrologue, sizeof(ecm::kCameraUiPrologue));
    emitProbe(b, &g_probeUi, 1, false);
    emit(b, {0x48, 0x8B, 0x81, 0xD8, 0x01, 0x00, 0x00});          // mov rax, [rcx+1D8h]
    emit(b, {0x48, 0x85, 0xC0});                                  // test rax, rax
    emit(b, {0x74, 0x09});                                        // je skip
    emit(b, {0x8B, 0x40, 0x1C});                                  // mov eax, [rax+1Ch]
    emit(b, {0x89, 0x81, 0x00, 0x03, 0x00, 0x00});                // mov [rcx+300h], eax
    emit(b, {0x0F, 0xB6, 0x81, 0xA0, 0x01, 0x00, 0x00});          // skip: movzx eax, byte ptr [rcx+1A0h]
    emit(b, {0x89, 0x81, 0x04, 0x03, 0x00, 0x00});                // mov [rcx+304h], eax
    emit(b, {0xFF, 0x81, 0x08, 0x03, 0x00, 0x00});                // inc dword ptr [rcx+308h]
    emit(b, {0x48, 0xB8, 0xDD, 0xCC, 0xBB, 0xAA, 0x99, 0x88, 0x77, 0x66});   // mov rax, 66778899AABBCCDDh
    emit(b, {0x48, 0x81, 0xC4, 0xB8, 0x01, 0x00, 0x00});          // add rsp, 1B8h
    emit(b, {0x41, 0x5E, 0x5D, 0xC3});                            // pop r14; pop rbp; ret
    if (corrupt) b[9] ^= 0x01;
    return b;
}
// The controller's update: records the three pressed ints (+0x400, +0x404, +0x408), the mode (+0x40C) and the call count (+0x410).
Bytes buildControllerSynthetic(bool corrupt) {
    Bytes b;
    prologue(b, ecm::kControllerPrologue, sizeof(ecm::kControllerPrologue));
    emitProbe(b, &g_probeCtl, 0, true);
    emit(b, {0x48, 0x8B, 0x81, 0x10, 0x03, 0x00, 0x00, 0x8B, 0x40, 0x1C, 0x89, 0x81, 0x00, 0x04, 0x00, 0x00});   // photo
    emit(b, {0x48, 0x8B, 0x81, 0x28, 0x03, 0x00, 0x00, 0x8B, 0x40, 0x1C, 0x89, 0x81, 0x04, 0x04, 0x00, 0x00});   // free
    emit(b, {0x48, 0x8B, 0x81, 0x40, 0x03, 0x00, 0x00, 0x8B, 0x40, 0x1C, 0x89, 0x81, 0x08, 0x04, 0x00, 0x00});   // quit
    emit(b, {0x0F, 0xB6, 0x81, 0xE0, 0x03, 0x00, 0x00, 0x89, 0x81, 0x0C, 0x04, 0x00, 0x00});                     // mode
    emit(b, {0xFF, 0x81, 0x10, 0x04, 0x00, 0x00});                // inc dword ptr [rcx+410h]
    emit(b, {0x48, 0xB8, 0x99, 0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22});   // mov rax, 2233445566778899h
    emit(b, {0x48, 0x8B, 0x5C, 0x24, 0x08, 0x48, 0x8B, 0x6C, 0x24, 0x10, 0x48, 0x8B, 0x74, 0x24, 0x18, 0xC3});   // restore rbx, rbp, rsi; ret
    if (corrupt) b[13] ^= 0x01;
    return b;
}
// The zoom/DOF update: the real 16-byte prologue (it leaves rbx = rcx), the probe, then a count of its runs in the activity (+0x300).
Bytes buildZoomSynthetic(bool corrupt) {
    Bytes b;
    prologue(b, ecm::kZoomDofPrologue, sizeof(ecm::kZoomDofPrologue));
    emitProbe(b, &g_probeZoom, -1, false);
    emit(b, {0xFF, 0x83, 0x00, 0x03, 0x00, 0x00});                // inc dword ptr [rbx+300h]
    emit(b, {0x48, 0xB8, 0x55, 0x44, 0x33, 0x22, 0x11, 0xEE, 0xDD, 0xCC});   // mov rax, 0CCDDEE1122334455h
    emit(b, {0x48, 0x83, 0xC4, 0x60, 0x5B, 0xC3});                // add rsp, 60h; pop rbx; ret
    if (corrupt) b[7] ^= 0x01;
    return b;
}
uint8_t* makeExecutable(const Bytes& code) {
    auto* page = static_cast<uint8_t*>(VirtualAlloc(nullptr, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!page) return nullptr;
    std::memcpy(page, code.data(), code.size());
    DWORD old = 0;
    VirtualProtect(page, 4096, PAGE_EXECUTE_READ, &old);
    FlushInstructionCache(GetCurrentProcess(), page, code.size());
    return page;
}
using FnObj = uint64_t (__fastcall*)(void*);
using CollisionFn = uint64_t (__fastcall*)(void*, uint64_t, uint64_t, uint64_t, uint64_t);
using BoxFn = int (__fastcall*)(void*, uint32_t*);

struct Pages {
    Bytes freeGood, freeBad, colGood, colBad, boxGood, boxBad, uiGood, uiBad, ctlGood, ctlBad, zoomGood, zoomBad;
    uint8_t *freeG = nullptr, *freeB = nullptr, *colG = nullptr, *colB = nullptr, *boxG = nullptr, *boxB = nullptr, *uiG = nullptr, *uiB = nullptr,
            *ctlG = nullptr, *ctlB = nullptr, *zoomG = nullptr, *zoomB = nullptr;
    bool ok() const { return freeG && freeB && colG && colB && boxG && boxB && uiG && uiB && ctlG && ctlB; }
};
uint8_t* g_leaPage = nullptr;     // `lea rax,[rcx+40h]; ret`: the shared record's accessor as a plain lea (the record at interface+0x40)
uint8_t* g_otherPage = nullptr;   // `mov rax,[rcx+8]; ret`: an accessor that is not a plain lea
Pages makePages() {
    Pages p;
    g_leaPage = makeExecutable(Bytes{0x48, 0x8D, 0x81, 0x40, 0x00, 0x00, 0x00, 0xC3});
    g_otherPage = makeExecutable(Bytes{0x48, 0x8B, 0x41, 0x08, 0xC3});
    p.freeGood = buildFreeSynthetic(false); p.freeBad = buildFreeSynthetic(true);
    p.colGood = buildCollisionSynthetic(false); p.colBad = buildCollisionSynthetic(true);
    p.boxGood = buildBoxSynthetic(false); p.boxBad = buildBoxSynthetic(true);
    p.uiGood = buildUiSynthetic(false); p.uiBad = buildUiSynthetic(true);
    p.ctlGood = buildControllerSynthetic(false); p.ctlBad = buildControllerSynthetic(true);
    p.zoomGood = buildZoomSynthetic(false); p.zoomBad = buildZoomSynthetic(true);
    p.freeG = makeExecutable(p.freeGood); p.freeB = makeExecutable(p.freeBad);
    p.colG = makeExecutable(p.colGood); p.colB = makeExecutable(p.colBad);
    p.boxG = makeExecutable(p.boxGood); p.boxB = makeExecutable(p.boxBad);
    p.uiG = makeExecutable(p.uiGood); p.uiB = makeExecutable(p.uiBad);
    p.ctlG = makeExecutable(p.ctlGood); p.ctlB = makeExecutable(p.ctlBad);
    p.zoomG = makeExecutable(p.zoomGood); p.zoomB = makeExecutable(p.zoomBad);
    return p;
}

// The camera suite's action handles as the static notes (analysis\decomp\explorer_cam\NOTES_actions.txt) give them, written out AGAIN here and not taken from
// the production tables, so a wrong offset or a wrong field in those is a failing cell. kind: 'A' float axis at +0x18, 'P' int pressed at +0x1C, 'H' byte held at +0x24.
struct ExpField {
    uint16_t handle;
    char kind;
};
const ExpField kExpFree[] = {{0x4B8, 'H'}, {0x4C0, 'H'}, {0x4C8, 'A'}, {0x4D0, 'A'}, {0x4F0, 'A'}, {0x4D8, 'A'}, {0x4E0, 'A'}, {0x4E8, 'A'}, {0x4F8, 'P'}, {0x500, 'P'}, {0x508, 'P'}};
const ExpField kExpCtl[] = {{0x310, 'P'}, {0x318, 'P'}, {0x320, 'P'}, {0x328, 'P'}, {0x340, 'P'}, {0x350, 'P'}, {0x358, 'P'}, {0x360, 'P'},
                            {0x368, 'P'}, {0x370, 'P'}, {0x378, 'P'}, {0x380, 'P'}, {0x388, 'P'}, {0x390, 'P'}, {0x398, 'P'}};
const ExpField kExpUi[] = {{0x1D8, 'P'}};
const ExpField kExpZoom[] = {{0x250, 'H'}, {0x258, 'H'}, {0x260, 'P'}, {0x268, 'H'}, {0x270, 'H'}, {0x278, 'H'}, {0x280, 'H'}};
constexpr int kExpCount[4] = {11, 15, 1, 7};
const ExpField* const kExpTable[4] = {kExpFree, kExpCtl, kExpUi, kExpZoom};
// What a cell sets in a field: a value no other field shares, so a clear of the wrong field or an object restored from another's value shows.
uint32_t isoPatternAxis(uint32_t h) { return 0x3F000000u | h; }
uint32_t isoPatternPressed(uint32_t h) { return 0x1000u + h; }
uint8_t isoPatternHeld(uint32_t h) { return static_cast<uint8_t>(0x80u | (h & 0x3Fu)); }
// The value of the spec'd field of `kind` in an action object.
uint32_t actionField(const uint8_t* object, char kind) {
    uint32_t v = 0;
    if (kind == 'H') v = object[0x24];
    else std::memcpy(&v, object + (kind == 'A' ? 0x18 : 0x1C), 4);
    return v;
}
// What the original saw, per holder: the spec'd field of every handle (0xFFFFFFFF = no object behind the handle).
struct SeenIso {
    uint32_t v[16];
    int calls = 0;
    uint32_t canary = 0xFFFFFFFFu;   // the pressed int of a canary action object behind an unlisted slot, as the original saw it
    uint32_t nb[16];                 // a held field's three neighbour bytes (+0x25..+0x27), as the original saw them
};
SeenIso g_seenIso[4];
const uint8_t* g_canaryObj[4] = {};
void snapIso(void* holder, int which) {
    SeenIso& out = g_seenIso[which];
    ++out.calls;
    if (g_canaryObj[which]) out.canary = actionField(g_canaryObj[which], 'P');
    for (int i = 0; i < kExpCount[which]; ++i) {
        uint64_t ptr = 0;
        std::memcpy(&ptr, static_cast<const uint8_t*>(holder) + kExpTable[which][i].handle, 8);
        const bool real = ptr >= 0x10000 && (ptr & 7) == 0 && ptr < 0x00007FFF00000000ull;
        out.v[i] = real ? actionField(reinterpret_cast<const uint8_t*>(ptr), kExpTable[which][i].kind) : 0xFFFFFFFFu;
        out.nb[i] = 0xFFFFFFFFu;
        if (real && kExpTable[which][i].kind == 'H') {
            uint32_t w = 0;
            std::memcpy(&w, reinterpret_cast<const uint8_t*>(ptr) + 0x24, 4);
            out.nb[i] = w >> 8;
        }
    }
}
void __fastcall probeFreeCb(void* o) { snapIso(o, 0); }
void __fastcall probeCtlCb(void* o) { snapIso(o, 1); }
void __fastcall probeUiCb(void* o) { snapIso(o, 2); }
void __fastcall probeZoomCb(void* o) { snapIso(o, 3); }

// The rig is the game between calls. Three objects and the action objects they point at.
struct Game {
    alignas(16) uint8_t free[0x600] = {};         // the free-camera activity
    alignas(16) uint8_t lockAction[0x40] = {};
    alignas(16) uint8_t ctl[0x600] = {};          // the camera controller
    alignas(16) uint8_t photoAction[0x40] = {}, freeAction[0x40] = {}, quitAction[0x40] = {};
    alignas(16) uint8_t ui[0x600] = {};           // the camera UI
    alignas(16) uint8_t hideAction[0x40] = {};
    alignas(16) uint8_t zoom[0x600] = {};         // the zoom/DOF activity
    alignas(16) uint8_t iso[40][0x40] = {};       // action objects for the isolation cells (every handle not already wired to an action above)
    alignas(16) uint8_t iface[0x100] = {};        // the interface cached at controller+0x108; the shared record is at iface+0x40
    uintptr_t vtable[8] = {};                     // its vtable: slot 4 (+0x20) is the record's accessor
    FnObj freeUpdate = nullptr, ctlUpdate = nullptr, uiUpdate = nullptr, zoomUpdate = nullptr;
    bool refuseOpen = false;                      // the game will not open the camera
    bool noHideHandle = false;                    // the camera UI's +0x1D8 is NULL
    bool blockedByOther = false;                  // the object at controller+0x80 says no: ToggleFreeCam is simply lost
    int dropPresses = 0;                          // the suite is still opening: this many ToggleFreeCam presses are dropped (F3)
    int pressesTaken = 0;                         // ToggleFreeCam presses the game saw in modes 1/2, dropped or not
    int queueDelay = 0;                           // > 0: ToggleFreeCam is QUEUED (+0x3E1 = 1) and the entry completes this many updates later
    bool queued = false;
    int queueLeft = 0;

    static int32_t& pressed(uint8_t* action) { return *reinterpret_cast<int32_t*>(action + 0x1C); }
    float& pose(int i) { return *reinterpret_cast<float*>(free + 0x3B0 + 4 * i); }
    template <typename T> static T rd(const uint8_t* base, uint32_t off) { T v; std::memcpy(&v, base + off, sizeof(T)); return v; }
    float seen(uint32_t off) const { return rd<float>(free, off); }
    int32_t seenLock() const { return rd<int32_t>(free, kSeenInt); }
    uint32_t seenState() const { return rd<uint32_t>(free, kSeenState); }
    uint32_t freeCalls() const { return rd<uint32_t>(free, kSeenCalls); }
    uint32_t ran() const { return rd<uint32_t>(free, kRan); }
    int32_t seenPhoto() const { return rd<int32_t>(ctl, 0x400); }
    int32_t seenFree() const { return rd<int32_t>(ctl, 0x404); }
    int32_t seenQuit() const { return rd<int32_t>(ctl, 0x408); }
    uint32_t ctlCalls() const { return rd<uint32_t>(ctl, 0x410); }
    int32_t seenHide() const { return rd<int32_t>(ui, 0x300); }
    uint32_t uiCallsSeen() const { return rd<uint32_t>(ui, 0x308); }
    uint8_t mode() const { return ctl[0x3E0]; }
    uint8_t& hidden() { return ui[0x1A0]; }
    bool sharedFlag() const { return iface[0x40 + 0x1D] != 0; }
    void setShared(bool on) { iface[0x40 + 0x1D] = on ? 1 : 0; }
    void useLeaAccessor(bool lea) { vtable[4] = reinterpret_cast<uintptr_t>(lea ? g_leaPage : g_otherPage); }

    void init() {
        std::memset(free, 0, sizeof(free));
        std::memset(ctl, 0, sizeof(ctl));
        std::memset(ui, 0, sizeof(ui));
        std::memset(lockAction, 0, sizeof(lockAction));
        std::memset(photoAction, 0, sizeof(photoAction));
        std::memset(freeAction, 0, sizeof(freeAction));
        std::memset(quitAction, 0, sizeof(quitAction));
        std::memset(hideAction, 0, sizeof(hideAction));
        std::memset(zoom, 0, sizeof(zoom));
        std::memset(iso, 0, sizeof(iso));
        refuseOpen = false;
        noHideHandle = false;
        blockedByOther = false;
        dropPresses = 0;
        pressesTaken = 0;
        queueDelay = 0;
        queued = false;
        queueLeft = 0;
        std::memset(iface, 0, sizeof(iface));
        for (uintptr_t& v : vtable) v = 0;
        useLeaAccessor(true);
        const uint64_t ifaceVptr = reinterpret_cast<uint64_t>(vtable), ifacePtr = reinterpret_cast<uint64_t>(iface);
        std::memcpy(iface, &ifaceVptr, 8);
        const uint64_t lock = reinterpret_cast<uint64_t>(lockAction);
        std::memcpy(free + 0x508, &lock, 8);
        std::memcpy(free + 0x4F8, &lock, 8);   // the probe reads these two as well; any readable action object will do
        std::memcpy(free + 0x500, &lock, 8);
        free[0x470] = 1; free[0x471] = 1; free[0x473] = 0; free[0x48C] = 0;
        const uint64_t photo = reinterpret_cast<uint64_t>(photoAction), fr = reinterpret_cast<uint64_t>(freeAction), quit = reinterpret_cast<uint64_t>(quitAction);
        std::memcpy(ctl + 0x310, &photo, 8);
        std::memcpy(ctl + 0x328, &fr, 8);
        std::memcpy(ctl + 0x340, &quit, 8);
        std::memcpy(ctl + 0x108, &ifacePtr, 8);
        const uint64_t hide = reinterpret_cast<uint64_t>(hideAction);
        std::memcpy(ui + 0x1D8, &hide, 8);
        // The selfie preset: facing back, origin (0, 1.5, 1.9), a distinctive fourth float per row.
        const float preset[16] = {-1, 0, 0, 0.5f, 0, 1, 0, 0.25f, 0, 0, -1, 0.125f, 0, 1.5f, 1.9f, 1.0f};
        std::memcpy(free + 0x3B0, preset, 64);
    }
    // Every handle of the four holders pointed at an action object of its own (the lock, photo, free, quit and hide actions keep their places), all three fields of
    // every object cleared. Returns the object behind (holder, index).
    uint8_t* isoObject(int which, int i) {
        uint8_t* holder = which == 0 ? free : which == 1 ? ctl : which == 2 ? ui : zoom;
        uint64_t ptr = 0;
        std::memcpy(&ptr, holder + kExpTable[which][i].handle, 8);
        return reinterpret_cast<uint8_t*>(ptr);
    }
    void isoWire() {
        std::memset(iso, 0, sizeof(iso));
        int k = 0;
        auto wire = [](uint8_t* holder, uint32_t handle, uint8_t* object) {
            const uint64_t ptr = reinterpret_cast<uint64_t>(object);
            std::memcpy(holder + handle, &ptr, 8);
        };
        for (const ExpField& f : kExpFree) wire(free, f.handle, f.handle == 0x508 ? lockAction : iso[k++]);
        for (const ExpField& f : kExpCtl) wire(ctl, f.handle, f.handle == 0x310 ? photoAction : f.handle == 0x328 ? freeAction : f.handle == 0x340 ? quitAction : iso[k++]);
        wire(ui, 0x1D8, hideAction);
        for (const ExpField& f : kExpZoom) wire(zoom, f.handle, iso[k++]);
    }
    // All three fields of every object a holder points at, set to that handle's own patterns (the game "holds every key down").
    void isoFill(int which) {
        for (int i = 0; i < kExpCount[which]; ++i) {
            uint8_t* o = isoObject(which, i);
            const uint32_t h = kExpTable[which][i].handle;
            const uint32_t axis = isoPatternAxis(h), pressed = isoPatternPressed(h);
            std::memcpy(o + 0x18, &axis, 4);
            std::memcpy(o + 0x1C, &pressed, 4);
            o[0x24] = isoPatternHeld(h);
        }
    }
    // Is every field of every object of a holder exactly what isoFill left?
    bool isoIntact(int which) {
        for (int i = 0; i < kExpCount[which]; ++i) {
            const uint8_t* o = isoObject(which, i);
            const uint32_t h = kExpTable[which][i].handle;
            uint32_t axis = 0, pressed = 0;
            std::memcpy(&axis, o + 0x18, 4);
            std::memcpy(&pressed, o + 0x1C, 4);
            if (axis != isoPatternAxis(h) || pressed != isoPatternPressed(h) || o[0x24] != isoPatternHeld(h)) return false;
        }
        return true;
    }
    // The spec'd field of handle i as the original saw it, against what isoFill set: 0 = zero (blocked), 1 = the pattern, 2 = something else.
    int isoSeen(int which, int i) const {
        const uint32_t v = g_seenIso[which].v[i], h = kExpTable[which][i].handle;
        const char kind = kExpTable[which][i].kind;
        const uint32_t pat = kind == 'A' ? isoPatternAxis(h) : kind == 'P' ? isoPatternPressed(h) : isoPatternHeld(h);
        return v == 0 ? 0 : v == pat ? 1 : 2;
    }
    int isoSeenZeroCount(int which) const {
        int n = 0;
        for (int i = 0; i < kExpCount[which]; ++i) n += isoSeen(which, i) == 0 ? 1 : 0;
        return n;
    }
    int isoSeenPatternCount(int which) const {
        int n = 0;
        for (int i = 0; i < kExpCount[which]; ++i) n += isoSeen(which, i) == 1 ? 1 : 0;
        return n;
    }
    void zoomFrame() { zoomUpdate(zoom); }
    uint32_t zoomCallsSeen() const { return rd<uint32_t>(zoom, 0x300); }
    // The shared mode record: the controller's byte and the free camera's state byte are the same number.
    void setMode(uint8_t m) {
        ctl[0x3E0] = m;
        free[0x48C] = m;
    }
    void setNoHandle(bool no) {
        noHideHandle = no;
        const uint64_t hide = no ? 0 : reinterpret_cast<uint64_t>(hideAction);
        std::memcpy(ui + 0x1D8, &hide, 8);
    }
    // One game frame of each object: the hooked update runs, then the game does what its update did to the state.
    void ctlFrame() {
        ctlUpdate(ctl);
        const uint8_t m = mode();
        if (seenPhoto() != 0) {
            if (m == 0) {
                if (!refuseOpen) setMode(1);
            } else {
                setMode(0);
            }
        }
        const uint8_t m2 = mode();
        if ((m2 == 1 || m2 == 2) && seenFree() != 0) {
            const int32_t kind = rd<int32_t>(ctl, 0x2E8);
            ++pressesTaken;
            if (dropPresses > 0) {
                --dropPresses;   // the suite's opening transition ignores the press; nothing in +0x3E1 or the shared record shows it (F3)
            } else if (blockedByOther || sharedFlag()) {
                // SetMode(3) returns without latching: the press is simply lost (F2).
            } else if (kind == 0 && queueDelay > 0) {
                if (!queued) {
                    queued = true;
                    queueLeft = queueDelay;
                    ctl[0x3E1] = 1;   // the game's own retry
                }
            } else if (kind == 0) {
                setMode(3);
                free[0x473] = 1;   // the free camera's first update seeds from the preset
            } else {
                setMode(m2 == 1 ? 2 : 1);
            }
        }
        if (queued && (mode() == 1 || mode() == 2) && --queueLeft <= 0) {
            queued = false;
            ctl[0x3E1] = 0;
            setMode(3);
            free[0x473] = 1;
        }
    }
    void ctlFrames(int n) { for (int i = 0; i < n; ++i) ctlFrame(); }
    void freeFrame() {
        freeUpdate(free);
        if (free[0x473] == 1) {
            free[0x473] = 0;
        } else if (seenLock() != 0) {          // the update saw the relative-lock action pressed
            if (mode() == 4) setMode(3);
            else if (mode() == 3) { free[0x470] = 1; setMode(4); }
        }
        // The update re-derives the commander-local pose from its collided result: here, where the face stopped it.
        pose(12) = 0.02f; pose(13) = 1.70f; pose(14) = 0.70f;
    }
    void uiFrame() {
        std::memset(ui + 0x300, 0, 8);
        uiUpdate(ui);
        if (seenHide() != 0) hidden() ^= 1;
    }
    // The player presses an action for one frame.
    void userPresses(uint8_t* action, void (Game::*frame)()) {
        pressed(action) = 1;
        (this->*frame)();
        pressed(action) = 0;
    }
};

struct Rig {
    Capture cap;
    uint32_t frame = 100;
    uint64_t ms = 100000;
    ExplorerCamTestFrame f;
    void boundary(bool f5 = false) {
        f.f5Pressed = f5;
        explorercamtest::boundary(frame++, ms += 16, f, &Capture::add, &cap);
    }
    void advance(uint64_t byMs) { ms += byMs; }
};

void installAll(const Pages& p, Rig& rig, Game& g) {
    ExplorerCamTestTargets t;
    t.freeCamera = reinterpret_cast<uintptr_t>(p.freeG);
    t.collision = reinterpret_cast<uintptr_t>(p.colG);
    t.boxPush = reinterpret_cast<uintptr_t>(p.boxG);
    t.cameraUi = reinterpret_cast<uintptr_t>(p.uiG);
    t.controller = reinterpret_cast<uintptr_t>(p.ctlG);
    explorercamtest::setTargets(t);
    g.freeUpdate = reinterpret_cast<FnObj>(p.freeG);
    g.ctlUpdate = reinterpret_cast<FnObj>(p.ctlG);
    g.uiUpdate = reinterpret_cast<FnObj>(p.uiG);
    rig.boundary();
}

void testStandDowns(const Pages& p) {
    std::printf("glue: the key off, and each prologue wrong in turn\n");
    namespace t = edvr::explorercamtest;
    Rig rig;
    ExplorerCamTestTargets good;
    good.freeCamera = reinterpret_cast<uintptr_t>(p.freeG);
    good.collision = reinterpret_cast<uintptr_t>(p.colG);
    good.boxPush = reinterpret_cast<uintptr_t>(p.boxG);
    good.cameraUi = reinterpret_cast<uintptr_t>(p.uiG);
    good.controller = reinterpret_cast<uintptr_t>(p.ctlG);

    uint8_t before[5][40];
    uint8_t* const goodPages[5] = {p.freeG, p.colG, p.boxG, p.uiG, p.ctlG};
    for (int i = 0; i < 5; ++i) std::memcpy(before[i], goodPages[i], 40);

    // ---- key off from the start: nothing is installed -----------------------------------------------------------------------
    t::setTargets(good);
    rig.f.hotkey = "";
    rig.boundary();
    rig.boundary();
    rig.advance(6000);
    rig.boundary();
    check(rig.cap.lines.size() >= 1 && std::strncmp(rig.cap.lines[0].c_str(), "explorer cam: hotkey.explorer_cam is empty: Explorer Cam is off", 63) == 0 &&
              rig.cap.count("hotkey.explorer_cam is empty: Explorer Cam is off") == 1,
          "HOTKEY EMPTY: three boundaries print ONE 'hotkey.explorer_cam is empty: Explorer Cam is off' line");
    bool untouched = true;
    for (int i = 0; i < 5; ++i) untouched = untouched && std::memcmp(before[i], goodPages[i], 40) == 0;
    check(untouched && !t::gateOpen() && !t::uiGateOpen() && !t::controllerGateOpen() && !t::placeActive(), "KEY OFF: no function was touched, every gate is closed, placement is not active");
    t::reset();

    // ---- each prologue wrong in turn --------------------------------------------------------------------------------------------
    struct Case {
        const char* name;
        int hook;                 // 0 free, 1 collision, 2 box, 3 ui, 4 controller
        const char* line;         // the stand-down line
        bool placementRuns;
    };
    const Case cases[] = {
        {"free-camera", 0, "free-camera hook stood down:", false}, {"collision", 1, "collision hook stood down:", false},
        {"box-push", 2, "box-push hook stood down:", false},       {"controller", 4, "controller hook stood down:", false},
        {"camera-UI", 3, "camera-UI hook stood down:", true},
    };
    uint8_t* const badPages[5] = {p.freeB, p.colB, p.boxB, p.uiB, p.ctlB};
    for (const Case& c : cases) {
        char what[160];
        rig.cap.clear();
        ExplorerCamTestTargets targets = good;
        uintptr_t* slots[5] = {&targets.freeCamera, &targets.collision, &targets.boxPush, &targets.cameraUi, &targets.controller};
        *slots[c.hook] = reinterpret_cast<uintptr_t>(badPages[c.hook]);
        uint8_t badBefore[40];
        std::memcpy(badBefore, badPages[c.hook], 40);
        t::setTargets(targets);
        rig.f.hotkey = "F5";
        rig.boundary();
        rig.boundary();
        std::snprintf(what, sizeof(what), "WRONG %s PROLOGUE: one '%s' line saying the game build differs and nothing was patched", c.name, c.line);
        check(rig.cap.count(c.line) == 1 && has(rig.cap.nth(c.line, 0), "the game build differs") && has(rig.cap.nth(c.line, 0), "nothing was patched"), what);
        std::snprintf(what, sizeof(what), "...the wrong function was not written, and placement %s", c.placementRuns ? "still runs (the camera UI is optional)" : "does not run");
        check(std::memcmp(badBefore, badPages[c.hook], 40) == 0 && t::placeActive() == c.placementRuns, what);
        if (c.hook == 0) {
            check(rig.cap.count("hooks are not installed") == 1 && rig.cap.count("hook armed:") == 0, "...the others are not installed, with one line saying so");
        } else if (c.hook == 1 || c.hook == 2) {
            check(rig.cap.count("the remaining hooks are not installed") == 1, "...the hooks after it are not installed, with one line saying so");
            bool laterUntouched = true;
            for (int i = c.hook + 1; i < 5; ++i) laterUntouched = laterUntouched && std::memcmp(before[i], goodPages[i], 40) == 0;
            check(laterUntouched, "...and the later functions were not patched");
        } else if (c.hook == 4) {
            check(rig.cap.count("the remaining hooks are not installed") == 1 && std::memcmp(before[3], goodPages[3], 40) == 0, "...the camera UI hook after it is not installed");
        }
        t::reset();
    }
}

// The full F5 flow and what it does to the game, from a closed camera.
void testF5FromClosed(const Pages& p) {
    std::printf("glue: F5 from the closed camera, placement, the hidden UI, F5 again\n");
    namespace t = edvr::explorercamtest;
    static Game g;
    static Game other;
    g.init();
    other.init();
    Rig rig;
    installAll(p, rig, g);
    auto collision = reinterpret_cast<CollisionFn>(p.colG);
    auto box = reinterpret_cast<BoxFn>(p.boxG);
    const FnObj bareFree = reinterpret_cast<FnObj>(p.freeG);
    (void)bareFree;

    check(rig.cap.count("explorer cam: Explorer Cam armed: F5 enters it") == 1 && rig.cap.count("hook armed:") == 5,
          "ARMED: one 'Explorer Cam armed: F5 enters it' line and five 'hook armed' lines (free-camera, collision, box-push, controller, camera-UI)");
    check(has(rig.cap.nth("free-camera hook armed:", 0), "stolen=5 bytes") && has(rig.cap.nth("free-camera hook armed:", 0), "28/28 bytes verified") &&
              has(rig.cap.nth("collision hook armed:", 0), "26/26 bytes verified") && has(rig.cap.nth("box-push hook armed:", 0), "20/20 bytes verified") &&
              has(rig.cap.nth("camera-UI hook armed:", 0), "stolen=12 bytes") && has(rig.cap.nth("camera-UI hook armed:", 0), "19/19 bytes verified") &&
              has(rig.cap.nth("controller hook armed:", 0), "15/15 bytes verified"),
          "...each verified its whole prologue; the camera UI's stole 12 bytes");
    check(t::stolenBytes(0) == 5 && t::stolenBytes(1) == 5 && t::stolenBytes(2) == 5 && t::stolenBytes(3) == 12 && t::stolenBytes(4) == 5 &&
              p.freeG[0] == 0xE9 && p.colG[0] == 0xE9 && p.boxG[0] == 0xE9 && p.uiG[0] == 0xE9 && p.ctlG[0] == 0xE9,
          "all five functions now begin with E9 (the relay jump); the steals are 5, 5, 5, 12 and 5 bytes");
    check(t::placeActive() && t::gateOpen() && t::uiGateOpen() && t::controllerGateOpen() && !t::sessionActive(), "placement is active, the three gates are open, and no session is on");
    check(rig.cap.prefixed() == rig.cap.lines.size(), "every line starts 'explorer cam:'");

    // The camera is closed. The controller is being called every frame (the question F2 asks, answered here as yes).
    rig.cap.clear();
    g.ctlFrame();
    rig.boundary();
    g.ctlFrame();
    rig.boundary();
    check(rig.cap.count("the camera controller update reached the hook") == 1 && has(rig.cap.nth("the camera controller update reached the hook", 0), "mode=0") &&
              has(rig.cap.nth("the camera controller update reached the hook", 0), "runs with the camera closed"),
          "the controller's first call is logged with mode 0: it runs with the camera CLOSED");
    check(t::controllerMode() == 0 && g.seenPhoto() == 0 && g.seenFree() == 0, "...and nothing was pressed: with no session the controller sees only the player's presses");

    // The player's own camera key and TAB do NOT engage Explorer Cam.
    rig.cap.clear();
    g.userPresses(g.photoAction, &Game::ctlFrame);      // the player opens the camera: mode 1
    g.userPresses(g.freeAction, &Game::ctlFrame);       // the player presses TAB: mode 3
    check(g.mode() == 3 && !t::sessionActive(), "THE PLAYER'S OWN camera key and TAB: the game goes 0 -> 1 -> 3 and no session is started");
    g.freeFrame(); g.freeFrame(); g.freeFrame();
    rig.boundary();
    check(g.seenLock() == 0 && t::placedActivity() == 0 && t::phase() == 0 && g.pose(13) == 1.70f && g.seen(kSeenY) != 1.68f,
          "...the free camera runs stock: no pose written, no lock pressed, nothing placed");
    check(collision(g.free, 5, 6, 7, 0x11) == (5ull + 6 + 7 + (0x11ull << 32)) && t::bypassed() == 0, "...the collision sweep runs for it (not bypassed)");
    uint32_t pt = 0x5555;
    check(box(g.free, &pt) == 0x5558 && pt == 0x11111111 && t::boxBypassed() == 0, "...and so does the box push (not bypassed)");
    // The player closes the camera again by hand.
    g.userPresses(g.photoAction, &Game::ctlFrame);
    check(g.mode() == 0, "(the player closes the camera)");
    g.init();
    rig.boundary();

    // ---- F5 -----------------------------------------------------------------------------------------------------------------------------
    rig.cap.clear();
    g.ctlFrame();
    rig.boundary();
    rig.boundary(true);
    check(rig.cap.count("F5 pressed: entering Explorer Cam (camera mode 0, closed; on foot: yes; GuiFocus: 0 (none))") == 1 && t::f5Request() == 1, "F5 PRESSED on foot with the camera closed: 'entering Explorer Cam', the ENTER request is set");
    g.ctlFrame();   // PhotoCameraToggle pressed: the camera opens
    check(g.seenPhoto() == 1 && Game::pressed(g.photoAction) == 0 && g.mode() == 1 && t::sessionActive() && t::f5Request() == 0,
          "the controller's next update: PhotoCameraToggle's int was 1 for that update and is restored to 0; the game opened the camera (mode 1); the session is on");
    g.ctlFrame();   // the suite has just opened: NOT pressed yet (the F2 failure pressed it here)
    check(g.seenFree() == 0 && g.mode() == 1, "THE TAB WAIT: the update after the suite opened presses NOTHING (F2's ToggleFreeCam pressed here was lost)");
    g.ctlFrames(3);
    check(g.seenFree() == 0 && g.mode() == 1, "...nor the next three: the suite must look ready for 5 updates in a row");
    g.ctlFrame();   // ToggleFreeCam pressed
    check(g.seenFree() == 1 && Game::pressed(g.freeAction) == 0 && g.seenPhoto() == 0 && g.mode() == 3,
          "the 5th ready update: ToggleFreeCam's int was 1 and is restored; PhotoCameraToggle was not pressed again; mode 3");
    g.freeFrame();  // the free camera's first update: pending preset
    check(g.seenLock() == 0 && t::placedActivity() == 0 && t::phase() == 1, "the free camera's first update (+0x473 = 1): entered and WAITING; no pose, no press, not yet placed");
    g.ctlFrame();
    g.freeFrame();  // the second update: place, press the lock
    check(closeTo(g.seen(kSeenX), 0.0f) && closeTo(g.seen(kSeenY), 1.68f) && closeTo(g.seen(kSeenZ), 0.10f), "the second update: the original SAW the placed pose (right 0.00, up 1.68, forward 0.10)");
    check(g.seenLock() == 1 && Game::pressed(g.lockAction) == 0 && g.mode() == 4, "...saw the relative-lock int = 1, which is restored to 0; the game locked (mode 4)");
    check(t::placedActivity() == reinterpret_cast<uint64_t>(g.free) && t::phase() == 2, "...and the activity is published as placed");

    // Both relays, for the placed activity and for another.
    const uint64_t colBypassed0 = t::bypassed(), boxBypassed0 = t::boxBypassed(), colForwarded0 = t::forwarded(), boxForwarded0 = t::boxForwarded();
    const uint32_t ranBefore = g.ran(), otherRanBefore = other.ran();
    check(collision(g.free, 1000, 20, 3, 0xA5) == 0 && g.ran() == ranBefore && t::bypassed() == colBypassed0 + 1,
          "COLLISION, placed activity: returns 0 WITHOUT running the function; counted");
    check(collision(other.free, 1000, 20, 3, 0xA5) == (1000ull + 20 + 3 + (0xA5ull << 32)) && other.ran() == otherRanBefore + 1 && t::forwarded() == colForwarded0 + 1,
          "COLLISION, any other rcx: runs the function, rdx r8 r9 and the FIFTH stack argument (0xA5) intact; counted");
    pt = 0x5555;
    const uint32_t boxRan = g.ran();
    check(box(g.free, &pt) == 0 && pt == 0x5555 && g.ran() == boxRan && t::boxBypassed() == boxBypassed0 + 1,
          "BOX PUSH, placed activity: returns 0 WITHOUT running the function and leaves the point ALONE; counted");
    pt = 0x5555;
    check(box(other.free, &pt) == 0x5558 && pt == 0x11111111 && other.ran() == otherRanBefore + 2 && t::boxForwarded() == boxForwarded0 + 1,
          "BOX PUSH, any other rcx: runs the function (point rewritten, count returned); counted");

    // The camera UI is hidden once.
    g.uiFrame();
    check(g.seenHide() == 1 && Game::pressed(g.hideAction) == 0 && g.hidden() == 1, "THE CAMERA UI: FreeCamToggleHUD's int was 1 for one update and is restored; the game hid the UI (+0x1A0 = 1)");
    g.uiFrame();
    check(g.seenHide() == 0 && g.hidden() == 1 && t::uiHiddenByEdvr(), "...the next update does not press again; EDVR remembers it hid it");
    g.freeFrame();   // the lock result is read here
    g.ctlFrame();
    rig.boundary();
    check(rig.cap.indexOf("F5 pressed: entering") >= 0 && rig.cap.indexOf("F5 enter: the camera is closed (mode 0)") > rig.cap.indexOf("F5 pressed: entering") &&
              rig.cap.indexOf("entered the free camera") > rig.cap.indexOf("F5 enter: the camera opened on a preset") && rig.cap.indexOf("explorer cam: placed:") > rig.cap.indexOf("entered the free camera") &&
              rig.cap.count("lock pressed:") == 1 && has(rig.cap.nth("lock pressed:", 0), "before=3 after=4") && rig.cap.count("the camera UI is hidden") == 1 &&
              rig.cap.count("F5 enter: the free camera is up") == 1 && rig.cap.count("the suite was ready after 5 updates") == 1,
          "LOG, in the order it happened: F5 pressed, the camera opened, the free camera up, entered, placed, lock pressed (3 -> 4), the camera UI hidden");
    check(has(rig.cap.nth("explorer cam: placed:", 0), "eye(up=1.680 forward=0.100 right=0.000)"), "...'placed:' carries the eye");

    // The heartbeat while the session is on.
    rig.advance(5200);
    g.freeFrame(); g.ctlFrame();
    rig.cap.clear();
    rig.boundary();
    const std::string beat = rig.cap.nth("explorer cam: heartbeat:", 0);
    check(!beat.empty() && has(beat, "phase=placed") && has(beat, "session=on") && has(beat, "box_bypassed=1(") && has(beat, "box_forwarded=2(") && has(beat, "collision_bypassed=1(") &&
              has(beat, "collision_forwarded=2(") && has(beat, "controller_mode=4") && has(beat, "ui_hidden_by_edvr=yes") && has(beat, "faults=0"),
          "HEARTBEAT: phase placed, session on, collision and box bypassed 1 and forwarded 2 each, the controller's mode 4, the UI hidden by EDVR");

    // The eye keys are live.
    rig.cap.clear();
    rig.f.up = 1.60f; rig.f.forward = 0.20f; rig.f.right = 0.05f;
    rig.boundary();
    g.freeFrame();
    check(rig.cap.count("eye changed:") == 1 && has(rig.cap.nth("eye changed:", 0), "up=1.600 forward=0.200 right=0.050") && closeTo(g.seen(kSeenX), 0.05f) &&
              closeTo(g.seen(kSeenY), 1.60f) && closeTo(g.seen(kSeenZ), 0.20f),
          "LIVE EYE: a changed key logs one 'eye changed' line and the very next update saw the new origin (right 0.05, up 1.60, forward 0.20)");
    rig.f.up = 9.0f; rig.f.forward = 9.0f; rig.f.right = -9.0f;
    rig.boundary();
    g.freeFrame();
    check(closeTo(g.seen(kSeenX), -0.5f) && closeTo(g.seen(kSeenY), 2.5f) && closeTo(g.seen(kSeenZ), 0.5f), "...out-of-range keys are clamped (up 2.5, forward 0.5, right -0.5)");
    rig.f.up = 1.68f; rig.f.forward = 0.10f; rig.f.right = 0.0f;
    rig.boundary();
    g.freeFrame();

    // ---- F5 again: leave ---------------------------------------------------------------------------------------------------------
    rig.cap.clear();
    rig.boundary(true);
    check(rig.cap.count("F5 pressed: leaving Explorer Cam (camera mode 4") == 1 && t::f5Request() == 2, "F5 PRESSED again in the session: 'leaving Explorer Cam', the EXIT request is set");
    g.ctlFrame();   // the exit begins: the UI is held, so nothing is pressed
    check(g.seenPhoto() == 0 && g.mode() == 4 && t::exiting(), "the controller's update: EXIT begins, the UI is held by EDVR so the camera is NOT closed yet");
    g.ctlFrame();
    check(g.seenPhoto() == 0 && g.mode() == 4, "...it waits");
    g.freeFrame();
    g.uiFrame();    // wantHidden is now false: unhide
    check(g.seenHide() == 1 && g.hidden() == 0 && Game::pressed(g.hideAction) == 0, "the camera UI's update: FreeCamToggleHUD pressed again (int 1 for one update, restored); the UI is back (+0x1A0 = 0)");
    g.uiFrame();
    check(!t::uiHiddenByEdvr(), "...the next update confirms and EDVR no longer holds it");
    g.ctlFrame();   // the UI is back: close
    check(g.seenPhoto() == 1 && Game::pressed(g.photoAction) == 0 && g.mode() == 0, "the controller's next update: PhotoCameraToggle pressed (restored after); the camera closed (mode 0)");
    g.ctlFrame();
    check(!t::sessionActive() && !t::exiting(), "...and the update after it sees mode 0: the session is over");
    rig.boundary();
    check(t::placedActivity() == 0 && t::phase() == 0, "the frame thread releases the placement at once (the free camera is not called again)");
    check(collision(g.free, 1, 2, 3, 0x22) == (1ull + 2 + 3 + (0x22ull << 32)), "...the collision sweep is the game's again for that activity");
    pt = 0x5555;
    check(box(g.free, &pt) == 0x5558, "...and so is the box push");
    check(rig.cap.indexOf("F5 exit: giving the camera UI back first") >= 0 && rig.cap.indexOf("camera UI is back") > rig.cap.indexOf("giving the camera UI back first") &&
              rig.cap.indexOf("F5 exit: closing the camera") > rig.cap.indexOf("camera UI is back") && rig.cap.indexOf("F5 exit: the camera is closed") > rig.cap.indexOf("F5 exit: closing the camera") &&
              rig.cap.count("released:") == 1,
          "LOG: leaving, the UI given back first, then the close, then the camera closed, then the release");
    t::reset();
}

void testF5FromOtherModes(const Pages& p) {
    std::printf("glue: F5 from a preset, from the free camera, from a detached camera\n");
    namespace t = edvr::explorercamtest;
    static Game g;
    g.init();
    Rig rig;
    installAll(p, rig, g);

    // From a preset (the camera is open on mode 1, kind 0).
    g.setMode(1);
    g.ctlFrame(); rig.boundary(); g.ctlFrame(); rig.boundary();
    rig.cap.clear();
    rig.boundary(true);
    g.ctlFrames(4);
    check(g.seenFree() == 0 && g.mode() == 1 && t::sessionActive(), "F5 from a preset (mode 1): not pressed for the first four updates (the suite must look ready for five)");
    g.ctlFrame();
    check(g.seenPhoto() == 0 && g.seenFree() == 1 && g.mode() == 3 && t::sessionActive(), "...then ToggleFreeCam only (the camera is not closed and reopened), mode 3, the session is on");
    g.freeFrame(); g.freeFrame(); g.ctlFrame();
    check(t::placedActivity() == reinterpret_cast<uint64_t>(g.free) && g.mode() == 4, "...placed and locked");
    rig.boundary();
    check(rig.cap.count("F5 pressed: entering Explorer Cam (camera mode 1, suite open on a preset") == 1 && rig.cap.count("F5 enter: the camera is open on a preset (mode 1)") == 1, "LOG: the preset mode is named");
    // The player's own camera key does nothing now (the isolation swallows it while the view is placed); mode 0 by ANY OTHER route (the game closing the camera
    // itself) still ends the session.
    rig.cap.clear();
    g.userPresses(g.photoAction, &Game::ctlFrame);
    g.ctlFrame();
    rig.boundary();
    check(g.mode() == 4 && t::sessionActive() && t::placedActivity() == reinterpret_cast<uint64_t>(g.free) && g.seenPhoto() == 0,
          "THE PLAYER'S OWN CAMERA KEY, placed: swallowed (the controller reads 0), the camera stays open, the session and the placement stand");
    g.setMode(0);
    g.ctlFrame();
    rig.boundary();
    check(g.mode() == 0 && !t::sessionActive() && t::placedActivity() == 0 && rig.cap.count("the camera closed (mode 0)") == 1, "MODE 0 BY THE GAME'S OWN HAND ends the session and releases the placement, with a line");

    // From the free camera (mode 4: locked).
    g.init();
    rig.boundary();
    g.setMode(4);
    g.free[0x473] = 0;
    g.ctlFrame(); rig.boundary(); g.ctlFrame(); rig.boundary();
    rig.cap.clear();
    rig.boundary(true);
    g.ctlFrame();
    g.freeFrame();
    check(g.seenPhoto() == 0 && g.seenFree() == 0 && t::sessionActive() && t::placedActivity() == reinterpret_cast<uint64_t>(g.free) && g.seenLock() == 0 && closeTo(g.seen(kSeenY), 1.68f),
          "F5 from the locked free camera (mode 4): nothing pressed, placed on the next update, the lock NOT pressed again");
    rig.boundary();
    check(rig.cap.count("F5 enter: already in the free camera (mode 4)") == 1, "LOG: 'already in the free camera'");

    // From a detached camera (mode 5): refused.
    g.init();
    t::reset();
    installAll(p, rig, g);
    g.setMode(5);
    g.ctlFrame(); rig.boundary(); g.ctlFrame(); rig.boundary();
    rig.cap.clear();
    rig.boundary(true);
    g.ctlFrame();
    rig.boundary();
    check(!t::sessionActive() && g.seenPhoto() == 0 && g.seenFree() == 0 && rig.cap.count("detached (mode 5, the world lock); leave it first") == 1, "F5 from a detached camera (mode 5): refused with 'leave it first', no session, nothing pressed");
    t::reset();
}

void testDetachAndReattach(const Pages& p) {
    std::printf("glue: a detach mid-session releases the placement, the return places, locks and hides again\n");
    namespace t = edvr::explorercamtest;
    static Game g;
    g.init();
    Rig rig;
    installAll(p, rig, g);
    g.setMode(3);
    g.free[0x473] = 1;
    g.ctlFrame(); rig.boundary(); g.ctlFrame(); rig.boundary();
    rig.boundary(true);
    g.ctlFrame();
    g.freeFrame(); g.freeFrame(); g.freeFrame(); g.uiFrame(); g.uiFrame(); g.ctlFrame();
    check(t::placedActivity() != 0 && g.mode() == 4 && g.hidden() == 1, "(placed, locked and hidden)");
    rig.cap.clear();
    // The player takes the camera to the world lock.
    g.setMode(5);
    g.freeFrame();
    g.ctlFrame();
    rig.boundary();
    check(t::placedActivity() == 0 && t::sessionActive() && closeTo(g.seen(kSeenZ), 0.70f), "DETACHED (mode 5): the placement is released at once (the original saw the game's own pose), the SESSION stays");
    g.uiFrame(); g.uiFrame();
    check(g.hidden() == 0 && !t::uiHiddenByEdvr(), "...and the camera UI is given back");
    rig.boundary();
    check(rig.cap.count("released:") == 1 && has(rig.cap.nth("released:", 0), "detached"), "LOG: released, 'detached'");
    // Back to the free camera: place, lock, hide again.
    g.setMode(3);
    g.freeFrame(); g.ctlFrame();
    check(t::placedActivity() == reinterpret_cast<uint64_t>(g.free) && g.seenLock() == 1 && closeTo(g.seen(kSeenY), 1.68f), "BACK TO MODE 3: placed again, and the lock pressed again");
    g.uiFrame(); g.uiFrame();
    check(g.hidden() == 1 && t::uiHiddenByEdvr(), "...and the camera UI hidden again");
    t::reset();
}

void testSessionTimeoutsAndIdle(const Pages& p) {
    std::printf("glue: timeouts, an idle controller, a dropped request\n");
    namespace t = edvr::explorercamtest;
    static Game g;
    g.init();
    Rig rig;
    installAll(p, rig, g);

    // The game will not open the camera.
    g.refuseOpen = true;
    g.ctlFrame(); rig.boundary(); g.ctlFrame(); rig.boundary();
    rig.cap.clear();
    rig.boundary(true);
    for (int i = 0; i < 95; ++i) g.ctlFrame();
    rig.boundary();
    check(!t::sessionActive() && rig.cap.count("F5 enter aborted: the camera did not open within 90 updates") == 1, "THE GAME WON'T OPEN THE CAMERA: the sequence aborts after 90 updates with one line, the session is over");
    check(g.mode() == 0 && Game::pressed(g.photoAction) == 0, "...the press was restored every time");

    // Preset kind 1: ToggleFreeCam only toggles presets 1 and 2.
    g.refuseOpen = false;
    g.setMode(1);
    std::memset(g.ctl + 0x2E8, 0, 4);
    const int32_t kind1 = 1;
    std::memcpy(g.ctl + 0x2E8, &kind1, 4);
    g.ctlFrame(); rig.boundary(); g.ctlFrame(); rig.boundary();
    rig.cap.clear();
    rig.boundary(true);
    g.ctlFrames(500);
    check(t::sessionActive(), "PRESET KIND 1: 500 updates in, the sequence is still waiting (it waits about 10 s, 900 updates, after the press)");
    g.ctlFrames(420);
    rig.boundary();
    check(!t::sessionActive() && rig.cap.count("F5 enter aborted: the free camera did not come up within 900 updates") == 1 && rig.cap.count("preset kind +0x2E8 = 1") >= 1,
          "...and aborts after 900: the free camera never comes up; the line names the preset kind");

    // The controller is idle (not called): F5 says so.
    t::reset();
    g.init();
    installAll(p, rig, g);
    rig.cap.clear();
    for (int i = 0; i < 40; ++i) rig.boundary();   // the controller is never called
    rig.boundary(true);
    check(rig.cap.count("the camera controller is idle: open the camera first") == 1 && t::f5Request() == 0 && !t::sessionActive(),
          "THE CONTROLLER IS IDLE: F5 says 'the camera controller is idle: open the camera first' and requests nothing");

    // A request that the controller never takes is dropped, with a line.
    g.ctlFrame(); rig.boundary(); g.ctlFrame(); rig.boundary();
    rig.cap.clear();
    rig.boundary(true);
    check(t::f5Request() == 1, "(a request is pending)");
    for (int i = 0; i < 35; ++i) rig.boundary();
    check(t::f5Request() == 0 && rig.cap.count("the F5 request was dropped: the camera controller was not called within 30 frames") == 1, "A REQUEST THE CONTROLLER NEVER TAKES is dropped after 30 frames, with a line");

    // Not on foot with the camera closed (in a ship), and not in gameplay.
    t::reset();
    g.init();
    installAll(p, rig, g);
    g.ctlFrame(); rig.boundary(); g.ctlFrame(); rig.boundary();
    rig.cap.clear();
    rig.f.onFoot = false;
    rig.boundary(true);
    check(rig.cap.count("F5 pressed, but Explorer Cam only starts on foot") == 1 && has(rig.cap.nth("only starts on foot", 0), "on foot = no") && has(rig.cap.nth("only starts on foot", 0), "GuiFocus = 0 (none)") &&
              has(rig.cap.nth("only starts on foot", 0), "lags about 6 s") && t::f5Request() == 0 && !t::sessionActive(),
          "NOT ON FOOT, camera closed: refused with ONE line naming on foot = no and the focus, and the 6 s lag");
    rig.f.onFootKnown = false;
    rig.cap.clear();
    rig.boundary(true);
    check(rig.cap.count("only starts on foot") == 1 && has(rig.cap.nth("only starts on foot", 0), "on foot = unknown") && t::f5Request() == 0, "ON FOOT UNKNOWN, camera closed: refused with one line saying unknown");
    rig.f.onFootKnown = true; rig.f.onFoot = true;
    rig.f.focus = 5;   // station services
    rig.cap.clear();
    rig.boundary(true);
    check(rig.cap.count("a panel has the focus") == 1 && has(rig.cap.nth("a panel has the focus", 0), "GuiFocus = 5 (station services)") && t::f5Request() == 0,
          "A PANEL OPEN (GuiFocus 5), camera closed: refused with one line naming the focus");
    rig.f.focusKnown = false;
    rig.cap.clear();
    rig.boundary(true);
    check(t::f5Request() == 1 && rig.cap.count("a panel has the focus") == 0 && rig.cap.count("F5 pressed: entering Explorer Cam") == 1 &&
              has(rig.cap.nth("entering Explorer Cam", 0), "GuiFocus: absent"),
          "GUI FOCUS ABSENT from Status.json (F7: on foot, no panel), camera closed, on foot known true: the press ENTERS (the request is set), and the line says absent");
    t::reset();
    g.init();
    installAll(p, rig, g);
    g.ctlFrame(); rig.boundary(); g.ctlFrame(); rig.boundary();
    rig.f.focusKnown = true; rig.f.focus = 0;
    rig.cap.clear();
    rig.boundary(true);
    check(t::f5Request() == 1 && rig.cap.count("F5 pressed: entering Explorer Cam") == 1 && has(rig.cap.nth("entering Explorer Cam", 0), "GuiFocus: 0 (none)"),
          "ON FOOT, focus 0: the same press ENTERS (the request is set)");
    // The camera suite open on a preset: in a ship it never enters, on foot with an unknown focus it does, with a panel it does not.
    t::reset();
    g.init();
    installAll(p, rig, g);
    g.ctlFrame(); rig.boundary(); g.ctlFrame(); rig.boundary();
    g.userPresses(g.photoAction, &Game::ctlFrame);   // the game's own camera opens: mode 1
    g.ctlFrame();
    rig.boundary();
    check(g.mode() == 1, "(the game's own camera suite is open: mode 1)");
    rig.f.onFoot = false;   // a ship
    rig.cap.clear();
    rig.boundary(true);
    check(rig.cap.count("only starts on foot") == 1 && has(rig.cap.nth("only starts on foot", 0), "camera mode 1") && t::f5Request() == 0, "THE CAMERA SUITE OPEN IN A SHIP (mode 1, not on foot): refused");
    rig.f.onFoot = true;
    rig.f.focusKnown = false;   // unknown focus, on foot known true
    rig.cap.clear();
    rig.boundary(true);
    check(t::f5Request() == 1 && rig.cap.count("a panel has the focus") == 0, "...on foot with an ABSENT focus inside the camera suite (mode 1): ENTER");
    t::reset();
    g.init();
    installAll(p, rig, g);
    g.ctlFrame(); rig.boundary(); g.ctlFrame(); rig.boundary();
    g.userPresses(g.photoAction, &Game::ctlFrame);
    g.ctlFrame();
    rig.boundary();
    rig.f.focusKnown = true; rig.f.focus = 6;   // the galaxy map
    rig.cap.clear();
    rig.boundary(true);
    check(rig.cap.count("a panel has the focus") == 1 && has(rig.cap.nth("a panel has the focus", 0), "GuiFocus = 6 (the galaxy map)") &&
              has(rig.cap.nth("a panel has the focus", 0), "while its own camera suite is open") && t::f5Request() == 0,
          "A NON-ZERO GUI FOCUS while the camera suite is open (mode 1): refused, and the line says the game reports it while its own suite is open");
    rig.f.focus = 0;
    // EXIT is never refused: a session on, then the journal says everything wrong.
    rig.boundary(true);
    g.ctlFrame();
    g.ctlFrame();
    rig.boundary();
    check(t::sessionActive(), "(an Explorer Cam session is on)");
    rig.f.onFootKnown = false; rig.f.focusKnown = true; rig.f.focus = 9;
    rig.cap.clear();
    rig.boundary(true);
    check(rig.cap.count("F5 pressed: leaving Explorer Cam") == 1 && rig.cap.count("only starts on foot") == 0 && rig.cap.count("a panel has the focus") == 0,
          "EXIT with on foot unknown and a panel open is not refused: 'leaving Explorer Cam'");
    rig.f.onFootKnown = true; rig.f.onFoot = true; rig.f.focus = 0;
    t::reset();
    g.init();
    installAll(p, rig, g);
    g.ctlFrame(); rig.boundary(); g.ctlFrame(); rig.boundary();
    rig.f.onFoot = true;
    rig.f.gameplay = false;
    rig.cap.clear();
    rig.boundary(true);
    check(rig.cap.lines.empty() && t::f5Request() == 0, "NOT IN GAMEPLAY (the journal): the press is ignored without a word");
    rig.f.gameplay = true;
    // F5 with the feature stood down says so.
    t::reset();
}

void testSessionEnds(const Pages& p) {
    std::printf("glue: the key turning off, stale activity, a stopped controller\n");
    namespace t = edvr::explorercamtest;
    static Game g;
    g.init();
    Rig rig;
    installAll(p, rig, g);
    auto collision = reinterpret_cast<CollisionFn>(p.colG);
    auto goToPlaced = [&]() {
        g.setMode(3);
        g.free[0x473] = 1;
        g.ctlFrame(); rig.boundary(); g.ctlFrame(); rig.boundary();
        rig.boundary(true);
        g.ctlFrame();
        g.freeFrame(); g.freeFrame(); g.ctlFrame(); g.uiFrame(); g.uiFrame();
    };
    goToPlaced();
    check(t::placedActivity() != 0 && t::sessionActive() && g.hidden() == 1, "(placed and hidden)");

    // The hotkey cleared: the placement is released, the session ended, and the UI is given back through the open gate.
    rig.cap.clear();
    rig.f.hotkey = "";
    rig.boundary();
    check(t::placedActivity() == 0 && !t::placeActive() && !t::sessionActive() && rig.cap.count("released:") == 1 && rig.cap.count("the Explorer Cam session ended: Explorer Cam is off") == 1,
          "KEY OFF while placed: released at once, the session ended, one line each");
    check(t::uiGateOpen() && !t::gateOpen() && !t::controllerGateOpen(), "(the camera UI hook's gate stays open while EDVR holds the UI; the other two close)");
    g.uiFrame(); g.uiFrame();
    check(g.hidden() == 0 && !t::uiHiddenByEdvr(), "...the camera UI hook gives the UI back even with the feature off");
    rig.boundary();
    check(!t::uiGateOpen(), "...and then closes its gate");
    check(collision(g.free, 1, 2, 3, 0x22) == (1ull + 2 + 3 + (0x22ull << 32)), "the collision sweep is the game's again");
    t::reset();

    // A silent activity: released after 30 frames.
    g.init();
    rig.f.hotkey = "F5";
    installAll(p, rig, g);
    goToPlaced();
    g.freeFrame();
    g.ctlFrame();
    rig.boundary();
    rig.cap.clear();
    for (int i = 0; i < 29; ++i) { g.ctlFrame(); rig.boundary(); }
    check(t::placedActivity() != 0 && rig.cap.count("released:") == 0, "STALE: 29 frames without a free-camera update: still placed");
    g.ctlFrame();
    rig.boundary();
    g.ctlFrame();   // a controller update after the release: the F5 session must survive it (the sequencer is not reset by a placement's release)
    check(t::placedActivity() == 0 && rig.cap.count("released:") == 1 && has(rig.cap.nth("released:", 0), "not called for 30 frames") && t::sessionActive(),
          "...the 30th releases it ('not called for 30 frames'); the session is still on, even after the next controller update");
    t::reset();

    // A controller that stops being called ends the session.
    g.init();
    rig.f.hotkey = "F5";
    installAll(p, rig, g);
    goToPlaced();
    g.ctlFrame();
    g.freeFrame();
    rig.boundary();
    rig.cap.clear();
    for (int i = 0; i < 29; ++i) { g.freeFrame(); rig.boundary(); }
    check(t::sessionActive(), "THE CONTROLLER STOPS: 29 frames: the session is still on");
    g.freeFrame();
    rig.boundary();
    check(!t::sessionActive() && rig.cap.count("the camera controller was not called for 30 frames") == 1, "...the 30th ends the session, with a line");
    t::reset();

    // The key goes off while the hide press is in flight: the UI still comes back.
    g.init();
    rig.f.hotkey = "F5";
    installAll(p, rig, g);
    g.setMode(3);
    g.free[0x473] = 1;
    g.ctlFrame(); rig.boundary(); g.ctlFrame(); rig.boundary();
    rig.boundary(true);
    g.ctlFrame(); g.freeFrame(); g.freeFrame(); g.ctlFrame();
    g.uiFrame();   // FreeCamToggleHUD pressed: the game hid the UI, EDVR has not yet read the result
    check(g.hidden() == 1 && !t::uiHiddenByEdvr(), "(the hide press is in flight: the UI is hidden, EDVR has not read the result)");
    rig.f.hotkey = "";
    rig.boundary();
    check(t::uiGateOpen(), "KEY OFF with a hide in flight: the camera UI hook's gate stays open");
    g.uiFrame();   // reads the result, then gives it back in the same update
    g.uiFrame();
    check(g.hidden() == 0 && !t::uiHiddenByEdvr(), "...and the UI still comes back (a UI that EDVR hid and nobody gives back would stay hidden)");
    t::reset();
}

void testFaults(const Pages& p) {
    std::printf("glue: faults are counted, and the eighth ends the feature\n");
    namespace t = edvr::explorercamtest;
    static Game g;
    g.init();
    Rig rig;
    installAll(p, rig, g);
    const uint64_t notAnObject = 0x30;

    // A bad lock-action pointer on the free camera's first press.
    g.setMode(3);
    g.ctlFrame(); rig.boundary(); g.ctlFrame(); rig.boundary();
    rig.boundary(true);
    g.ctlFrame();   // session on (mode 3: nothing pressed)
    std::memcpy(g.free + 0x508, &notAnObject, 8);
    t::preThenPost(g.free);   // +0x473 = 0 and state 3: enters, writes, and the press faults
    rig.cap.clear();
    rig.boundary();
    check(t::faults() == 1 && rig.cap.count("explorer cam: fault 1 of 8:") == 1 && has(rig.cap.nth("explorer cam: fault 1 of 8:", 0), "lock press") && t::placedActivity() == 0,
          "FAULT: a lock-action pointer that is not an object is a counted fault (lock press), the placement released, nothing written through it");
    // A bad PhotoCameraToggle pointer on the controller.
    t::reset();
    g.init();
    installAll(p, rig, g);
    g.ctlFrame(); rig.boundary(); g.ctlFrame(); rig.boundary();
    std::memcpy(g.ctl + 0x310, &notAnObject, 8);
    rig.boundary(true);
    t::controllerPreThenPost(g.ctl);   // ENTER from mode 0: the Photo press faults
    rig.cap.clear();
    rig.boundary();
    check(t::faults() == 1 && !t::sessionActive() && rig.cap.count("the camera controller's press") == 1, "FAULT: the controller's press through a bad pointer is counted, the sequence ended, no session");
    // A bad hide pointer on the camera UI.
    t::reset();
    g.init();
    installAll(p, rig, g);
    g.setMode(3);
    g.ctlFrame(); rig.boundary(); g.ctlFrame(); rig.boundary();
    rig.boundary(true);
    g.ctlFrame();
    g.freeFrame(); g.freeFrame();
    std::memcpy(g.ui + 0x1D8, &notAnObject, 8);
    t::uiPreThenPost(g.ui);
    rig.cap.clear();
    rig.boundary();
    check(t::faults() == 1 && rig.cap.count("hide press") == 1, "FAULT: the camera UI's hide press through a bad pointer is counted (a non-NULL handle that is not an object)");

    // Eight faults: the feature stands down for the session with one line.
    t::reset();
    g.init();
    installAll(p, rig, g);
    rig.cap.clear();
    for (int i = 0; i < 8; ++i) {
        t::preThenPost(reinterpret_cast<void*>(static_cast<uintptr_t>(0x10 + 0x10 * i)));
        rig.boundary();
    }
    check(t::faults() == 8 && rig.cap.count("explorer cam: fault ") == 8 && rig.cap.count("stood down for the session") == 1 && !t::placeActive(),
          "EIGHT FAULTS: eight 'fault N of 8' lines, ONE 'stood down for the session' line, placement inactive");
    t::preThenPost(reinterpret_cast<void*>(static_cast<uintptr_t>(0x600)));
    t::controllerPreThenPost(reinterpret_cast<void*>(static_cast<uintptr_t>(0x600)));
    rig.boundary();
    check(t::faults() == 8 && rig.cap.count("stood down for the session") == 1, "...and nothing more is counted or said");
    t::reset();
}

void testObservers(const Pages& p) {
    std::printf("glue: observers ride the hooks after the restore\n");
    namespace t = edvr::explorercamtest;
    static Game g;
    g.init();
    Rig rig;
    installAll(p, rig, g);
    static std::atomic<int> freeSeen, ctlSeen;
    static std::atomic<int32_t> lockAtObserver, photoAtObserver;
    static Game* world;
    world = &g;
    freeSeen = 0; ctlSeen = 0;
    auto freeObs = [](void*) noexcept { freeSeen.fetch_add(1); lockAtObserver = Game::pressed(world->lockAction); };
    auto ctlObs = [](void*) noexcept { ctlSeen.fetch_add(1); photoAtObserver = Game::pressed(world->photoAction); };
    const ExplorerCamHookStatus fs = explorerCamObserve(ExplorerCamHook::FreeCamera, freeObs, true);
    const ExplorerCamHookStatus cs = explorerCamObserve(ExplorerCamHook::Controller, ctlObs, true);
    check(fs.state == ExplorerCamHookStatus::Armed && fs.stolen == 5 && cs.state == ExplorerCamHookStatus::Armed && cs.stolen == 5, "OBSERVE: both hooks report armed, 5 stolen bytes");
    g.ctlFrame();
    check(ctlSeen.load() == 1, "the controller observer ran after the original");
    g.setMode(3);
    g.free[0x473] = 0;
    rig.boundary(true);
    g.ctlFrame();   // session on
    g.freeFrame();  // places and presses the lock
    check(freeSeen.load() == 1 && lockAtObserver.load() == 0, "the free-camera observer ran after the original, and the lock's int was already restored (0) when it ran");
    // A second observer on the same hook: the slot list takes up to four.
    static std::atomic<int> second;
    second = 0;
    auto secondObs = [](void*) noexcept { second.fetch_add(1); };
    explorerCamObserve(ExplorerCamHook::FreeCamera, secondObs, true);
    g.freeFrame();
    check(second.load() == 1 && freeSeen.load() == 2, "A SECOND OBSERVER attaches to the same hook (the neck's place): both run");
    explorerCamObserve(ExplorerCamHook::FreeCamera, secondObs, false);
    g.freeFrame();
    check(second.load() == 1 && freeSeen.load() == 3, "...and detaches without touching the first");
    t::reset();
}

// ---- the hotkey against the player's Elite bindings ---------------------------------------------------------------------------
bool writeTextFile(const std::wstring& path, const char* text) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD wrote = 0;
    const BOOL ok = WriteFile(f, text, static_cast<DWORD>(std::strlen(text)), &wrote, nullptr);
    CloseHandle(f);
    return ok && wrote == std::strlen(text);
}

const char kBindsWithF5[] =
    "<?xml version=\"1.0\" encoding=\"UTF-8\" ?>\r\n"
    "<Root PresetName=\"Custom\" MajorVersion=\"4\" MinorVersion=\"2\">\r\n"
    "  <KeyboardLayout>en-US</KeyboardLayout>\r\n"
    "  <MouseXMode Value=\"Bindings_MouseRoll\" />\r\n"
    "  <PhotoCameraToggle_Humanoid>\r\n"
    "    <Primary Device=\"Keyboard\" Key=\"Key_F5\">\r\n"
    "      <Modifier Device=\"Keyboard\" Key=\"Key_LeftShift\" />\r\n"
    "    </Primary>\r\n"
    "    <Secondary Device=\"{NoDevice}\" Key=\"\" />\r\n"
    "  </PhotoCameraToggle_Humanoid>\r\n"
    "  <ExplorationFSSEnter>\r\n"
    "    <Primary Device=\"Keyboard\" Key=\"Key_F5\" />\r\n"
    "    <Secondary Device=\"GamePad\" Key=\"GamePad_Back\" />\r\n"
    "  </ExplorationFSSEnter>\r\n"
    "  <ToggleFreeCam>\r\n"
    "    <Primary Device=\"Keyboard\" Key=\"Key_Tab\" />\r\n"
    "    <Secondary Device=\"{NoDevice}\" Key=\"\" />\r\n"
    "  </ToggleFreeCam>\r\n"
    "  <HeadlookToggle>\r\n"
    "    <Primary Device=\"{NoDevice}\" Key=\"\" />\r\n"
    "    <Secondary Device=\"Keyboard\" Key=\"Key_F6\" />\r\n"
    "  </HeadlookToggle>\r\n"
    "</Root>\r\n";
const char kBindsNoF5[] =
    "<?xml version=\"1.0\" encoding=\"UTF-8\" ?>\r\n"
    "<Root PresetName=\"Custom\" MajorVersion=\"4\" MinorVersion=\"2\">\r\n"
    "  <ExplorationFSSEnter>\r\n"
    "    <Primary Device=\"Keyboard\" Key=\"Key_F9\" />\r\n"
    "    <Secondary Device=\"GamePad\" Key=\"GamePad_Back\" />\r\n"
    "  </ExplorationFSSEnter>\r\n"
    "  <ToggleFreeCam>\r\n"
    "    <Primary Device=\"Keyboard\" Key=\"Key_Tab\" />\r\n"
    "    <Secondary Device=\"{NoDevice}\" Key=\"\" />\r\n"
    "  </ToggleFreeCam>\r\n"
    "</Root>\r\n";

void testHotkeyClash(const Pages& p) {
    std::printf("glue: the hotkey checked against the player's Elite bindings\n");
    namespace t = edvr::explorercamtest;
    wchar_t temp[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, temp);
    const std::wstring dir = std::wstring(temp) + L"edvr_explorer_cam_binds_" + std::to_wstring(GetCurrentProcessId());
    CreateDirectoryW(dir.c_str(), nullptr);
    const std::wstring preset = dir + L"\\StartPreset.4.start", binds = dir + L"\\Custom.4.2.binds";
    check(writeTextFile(preset, "Custom\r\n") && writeTextFile(binds, kBindsWithF5), "(a fixture of the player's Elite bindings is written)");

    // The scan itself.
    static EliteKeyboardUse uses[512];
    char file[64] = {};
    const int n = eliteBindsKeyboardUsesDir(dir.c_str(), uses, 512, file, sizeof(file));
    bool sawPhoto = false, sawFss = false, sawTab = false, sawF6 = false, sawPad = false;
    for (int i = 0; i < n; ++i) {
        sawPhoto = sawPhoto || (std::strcmp(uses[i].element, "PhotoCameraToggle_Humanoid") == 0 && std::strcmp(uses[i].binding, "SHIFT+F5") == 0);
        sawFss = sawFss || (std::strcmp(uses[i].element, "ExplorationFSSEnter") == 0 && std::strcmp(uses[i].binding, "F5") == 0);
        sawTab = sawTab || (std::strcmp(uses[i].element, "ToggleFreeCam") == 0 && std::strcmp(uses[i].binding, "TAB") == 0);
        sawF6 = sawF6 || (std::strcmp(uses[i].element, "HeadlookToggle") == 0 && std::strcmp(uses[i].binding, "F6") == 0);
        sawPad = sawPad || std::strstr(uses[i].binding, "Back") != nullptr;
    }
    check(n == 4 && sawPhoto && sawFss && sawTab && sawF6 && !sawPad && std::strcmp(file, "Custom.4.2.binds") == 0,
          "the scan finds the four KEYBOARD bindings (SHIFT+F5, F5, TAB, F6) with the element each belongs to, in Custom.4.2.binds, and skips the gamepad slot and the empty ones");
    check(eliteBindsKeyboardUsesDir((dir + L"\\nowhere").c_str(), uses, 512, file, sizeof(file)) == -1, "...and a directory with no preset is -1");

    static Game g;
    g.init();
    Rig rig;
    ExplorerCamTestTargets targets;
    targets.freeCamera = reinterpret_cast<uintptr_t>(p.freeG);
    targets.collision = reinterpret_cast<uintptr_t>(p.colG);
    targets.boxPush = reinterpret_cast<uintptr_t>(p.boxG);
    targets.cameraUi = reinterpret_cast<uintptr_t>(p.uiG);
    targets.controller = reinterpret_cast<uintptr_t>(p.ctlG);
    t::setTargets(targets);
    rig.f.bindsDir = dir.c_str();
    rig.f.hotkey = "F5";
    rig.boundary();
    check(rig.cap.count("hotkey F5 CLASHES with your Elite bindings in Custom.4.2.binds") == 1 && has(rig.cap.nth("CLASHES", 0), "ExplorationFSSEnter (F5)") &&
              has(rig.cap.nth("CLASHES", 0), "PhotoCameraToggle_Humanoid (SHIFT+F5)"),
          "AT LAUNCH: F5 CLASHES with ExplorationFSSEnter (F5) and PhotoCameraToggle_Humanoid (SHIFT+F5), named in one line");
    rig.cap.clear();
    for (int i = 0; i < 5; ++i) rig.boundary();
    check(rig.cap.count("hotkey") == 0, "...said once, not every frame");

    // The player rebinds in Elite: the files change, the change HOLDS across two checks, and the check runs again.
    check(writeTextFile(binds, kBindsNoF5), "(the player rebinds F5 away in Elite)");
    rig.advance(2100);
    rig.boundary();
    check(rig.cap.count("hotkey F5") == 0, "ON A REBIND: the first sight of the change says nothing (half a save is not a configuration)");
    rig.advance(2100);
    rig.boundary();
    check(rig.cap.count("hotkey F5 is free in your Elite bindings (Custom.4.2.binds") == 1, "...it held across the second check: 'hotkey F5 is free in your Elite bindings'");
    // A different key: re-checked at once.
    rig.cap.clear();
    rig.f.hotkey = "F9";
    rig.boundary();
    check(rig.cap.count("hotkey F9 CLASHES with your Elite bindings") == 1 && has(rig.cap.nth("CLASHES", 0), "ExplorationFSSEnter (F9)"), "A CHANGED hotkey is checked at once: F9 clashes with ExplorationFSSEnter");
    // A chord that the player's bare binding also fires on.
    rig.cap.clear();
    rig.f.hotkey = "CTRL+F9";
    rig.boundary();
    check(rig.cap.count("hotkey CTRL+F9 CLASHES") == 1, "CTRL+F9 clashes with a bare F9 (one press fires both)");
    rig.cap.clear();
    rig.f.hotkey = "F7";
    rig.boundary();
    check(rig.cap.count("hotkey F7 is free") == 1, "F7 is free");
    // Bindings that cannot be read.
    rig.cap.clear();
    const std::wstring missing = dir + L"\\nowhere";
    rig.f.bindsDir = missing.c_str();
    rig.f.hotkey = "F8";
    rig.boundary();
    check(rig.cap.count("hotkey F8 could not be checked against your Elite bindings") == 1, "NO PRESET: 'could not be checked', said");
    rig.cap.clear();
    rig.f.readBindings = false;
    rig.f.hotkey = "F4";
    rig.boundary();
    check(rig.cap.count("hotkey F4 is not checked against your Elite bindings (hotkey.read_game_bindings is off)") == 1, "hotkey.read_game_bindings off: 'not checked', said once");
    rig.boundary();
    check(rig.cap.count("not checked") == 1, "...once");
    t::reset();
    DeleteFileW(preset.c_str());
    DeleteFileW(binds.c_str());
    RemoveDirectoryW(dir.c_str());
}

// The config path: the keys under their real names.
void testConfigPath(const Pages& p) {
    std::printf("the config keys, read the way the frame boundary reads them\n");
    namespace t = edvr::explorercamtest;
    static Game g;
    g.init();
    Config& cfg = Config::get();
    ExplorerCamTestTargets targets;
    targets.freeCamera = reinterpret_cast<uintptr_t>(p.freeG);
    targets.collision = reinterpret_cast<uintptr_t>(p.colG);
    targets.boxPush = reinterpret_cast<uintptr_t>(p.boxG);
    targets.cameraUi = reinterpret_cast<uintptr_t>(p.uiG);
    targets.controller = reinterpret_cast<uintptr_t>(p.ctlG);
    t::setTargets(targets);
    g.freeUpdate = reinterpret_cast<FnObj>(p.freeG);
    g.ctlUpdate = reinterpret_cast<FnObj>(p.ctlG);
    g.uiUpdate = reinterpret_cast<FnObj>(p.uiG);
    // Defaults: nothing set. The ini ships `hotkey.explorer_cam = F5`, which arms Explorer Cam; the code default agrees.
    explorerCamFrameBoundary(1);
    check(t::placeActive(), "no keys set: Explorer Cam is ON by default");
    check(t::hotkeyVk() == VK_F5, "...and hotkey.explorer_cam defaults to F5");
    // The eye keys reach the hook. The wrapper's F5 is the real keyboard, so the session is switched on through the seam.
    t::forceSession(true);
    g.setMode(3);
    g.freeFrame();
    check(closeTo(g.seen(kSeenX), 0.0f) && closeTo(g.seen(kSeenY), 1.68f) && closeTo(g.seen(kSeenZ), 0.10f), "a placement with no keys set puts the eye at up 1.68, forward 0.10, right 0.0");
    // The follow settings, read from Config under their real names, with the code's own defaults when nothing is set (the shipped edvr.ini holds the same
    // numbers: tools/config_test), live, and held to their clamps. The frame thread publishes them; the hook thread reads what is published.
    check(closeTo(t::followTrimUp(), 0.15f) && closeTo(t::followTrimForward(), -0.08f) && closeTo(t::followTrimRight(), 0.0f) && t::followSmoothingMs() == 0.0f,
          "NOTHING SET: the trims are Sean's defaults (up 0.15, forward -0.08, right 0) and the smoothing is 0");
    cfg.set("fix.explorer_cam_eye_trim_up", "0.22");
    cfg.set("fix.explorer_cam_eye_trim_forward", "-0.01");
    cfg.set("fix.explorer_cam_eye_trim_right", "0.04");
    cfg.set("fix.explorer_cam_follow_smoothing_ms", "40");
    explorerCamFrameBoundary(2);
    check(closeTo(t::followTrimUp(), 0.22f) && closeTo(t::followTrimForward(), -0.01f) && closeTo(t::followTrimRight(), 0.04f) && t::followSmoothingMs() == 40.0f,
          "fix.explorer_cam_eye_trim_up / _forward / _right and fix.explorer_cam_follow_smoothing_ms are read from the ini and applied on the next frame (the menu page's rows)");
    cfg.set("fix.explorer_cam_eye_trim_up", "0.9");
    cfg.set("fix.explorer_cam_eye_trim_forward", "-0.9");
    cfg.set("fix.explorer_cam_follow_smoothing_ms", "5000");
    explorerCamFrameBoundary(3);
    check(closeTo(t::followTrimUp(), 0.5f) && closeTo(t::followTrimForward(), -0.5f) && t::followSmoothingMs() == 1000.0f,
          "...held to +-0.5 m and to 1000 ms, whatever the file says");
    cfg.set("fix.explorer_cam_follow_smoothing_ms", "-20");
    explorerCamFrameBoundary(3);
    check(t::followSmoothingMs() == 0.0f, "...a negative smoothing is 0");
    for (const char* key : {"fix.explorer_cam_eye_trim_up", "fix.explorer_cam_eye_trim_forward", "fix.explorer_cam_eye_trim_right", "fix.explorer_cam_follow_smoothing_ms"}) cfg.set(key, "");
    explorerCamFrameBoundary(3);
    check(closeTo(t::followTrimUp(), 0.15f) && closeTo(t::followTrimForward(), -0.08f) && t::followSmoothingMs() == 0.0f,
          "...and keys removed again fall back to the defaults, not to zero");
    cfg.set("fix.explorer_cam_eye_up", "1.55");
    cfg.set("fix.explorer_cam_eye_forward", "0.25");
    cfg.set("fix.explorer_cam_eye_right", "-0.04");
    explorerCamFrameBoundary(4);
    g.freeFrame();
    check(closeTo(g.seen(kSeenX), -0.04f) && closeTo(g.seen(kSeenY), 1.55f) && closeTo(g.seen(kSeenZ), 0.25f),
          "fix.explorer_cam_eye_up / _eye_forward / _eye_right reach the hook (1.55, 0.25, -0.04)");
    t::forceSession(false);
    cfg.set("hotkey.explorer_cam", "");
    explorerCamFrameBoundary(5);
    check(t::hotkeyVk() == 0, "hotkey.explorer_cam = (empty): no key bound");
    check(!t::placeActive(), "...and Explorer Cam is OFF: placement inactive (the hotkey is what arms it; there is no other switch)");
    cfg.set("hotkey.explorer_cam", "F6");
    explorerCamFrameBoundary(6);
    check(t::hotkeyVk() == VK_F6, "hotkey.explorer_cam = F6: bound to F6");
    check(t::placeActive(), "...and Explorer Cam is armed again");

    // ---- the key is also the way out: a change or a clearing made while a session is on waits for the session to end ------------------------------------------------
    Capture cap;
    t::setFrameSink(&Capture::add, &cap);
    check(!explorerCamSessionActive(), "explorerCamSessionActive(): false with no session");
    t::forceSession(true);
    check(explorerCamSessionActive(), "...true while a session is on (the hotkey menu locks the key's row on it)");
    cfg.set("hotkey.explorer_cam", "");
    cap.clear();
    explorerCamFrameBoundary(10);
    check(t::hotkeyVk() == VK_F6 && t::placeActive() && cap.count("hotkey.explorer_cam changed during Explorer Cam: F6 stays the exit until you leave") == 1 &&
              has(cap.nth("changed during Explorer Cam", 0), "empty: Explorer Cam turns off"),
          "HOTKEY CLEARED MID-SESSION: F6 stays bound and Explorer Cam stays armed (the old key still exits), one line says so");
    cap.clear();
    explorerCamFrameBoundary(11);
    explorerCamFrameBoundary(12);
    check(t::hotkeyVk() == VK_F6 && t::placeActive() && cap.count("changed during Explorer Cam") == 0, "...said ONCE, and the old key stays bound while the session lasts");
    t::forceSession(false);
    explorerCamFrameBoundary(13);
    check(t::hotkeyVk() == 0 && !t::placeActive(), "...the session over: the cleared key applies, Explorer Cam is disarmed");
    cfg.set("hotkey.explorer_cam", "F6");
    explorerCamFrameBoundary(14);
    check(t::hotkeyVk() == VK_F6 && t::placeActive(), "(F6 armed again)");
    t::forceSession(true);
    cfg.set("hotkey.explorer_cam", "F7");
    cap.clear();
    explorerCamFrameBoundary(15);
    check(t::hotkeyVk() == VK_F6 && t::placeActive() && cap.count("hotkey.explorer_cam changed during Explorer Cam: F6 stays the exit until you leave") == 1 && has(cap.nth("changed during Explorer Cam", 0), "the new value (F7)"),
          "HOTKEY CHANGED MID-SESSION (F6 to F7): F6 stays bound and exits, one line says so and names the new value");
    explorerCamFrameBoundary(16);
    check(t::hotkeyVk() == VK_F6, "...still F6 the next frame");
    t::forceSession(false);
    explorerCamFrameBoundary(17);
    check(t::hotkeyVk() == VK_F7 && t::placeActive(), "...the session over: F7 is bound and live");
    t::forceSession(true);
    cfg.set("hotkey.explorer_cam", "F8");
    cap.clear();
    explorerCamFrameBoundary(18);
    cfg.set("hotkey.explorer_cam", "F7");
    cap.clear();
    explorerCamFrameBoundary(19);
    check(t::hotkeyVk() == VK_F7 && cap.count("changed during Explorer Cam") == 0, "A CHANGE REVERTED BEFORE THE SESSION ENDS says nothing more and changes nothing");
    cfg.set("hotkey.explorer_cam", "F8");
    cap.clear();
    explorerCamFrameBoundary(20);
    check(cap.count("changed during Explorer Cam") == 1, "...and a new change is held back and said again");
    t::forceSession(false);
    t::setFrameSink(nullptr, nullptr);
    t::reset();
    for (const char* key : {"hotkey.explorer_cam", "fix.explorer_cam_eye_up", "fix.explorer_cam_eye_forward", "fix.explorer_cam_eye_right"}) cfg.set(key, "");
}

// ToggleFreeCam waits for the suite to be ready (F2: pressed the update after opening, it was lost and the sequence aborted).
void testTabWaitGlue(const Pages& p) {
    std::printf("glue: the TAB wait against a controller that loses an early press\n");
    namespace t = edvr::explorercamtest;
    static Game g;
    g.init();
    Rig rig;
    installAll(p, rig, g);
    g.ctlFrame(); rig.boundary(); g.ctlFrame(); rig.boundary();

    // The shared record's +0x1D is set (the game would lose a ToggleFreeCam now): nothing is pressed; cleared, it is pressed after 5 ready updates.
    g.setShared(true);
    rig.cap.clear();
    rig.boundary(true);
    g.ctlFrame();   // PhotoCameraToggle: the suite opens
    int presses = 0;
    for (int i = 0; i < 60; ++i) {
        g.ctlFrame();
        presses += g.seenFree() != 0 ? 1 : 0;
    }
    check(presses == 0 && g.mode() == 1 && t::sessionActive(), "SHARED +0x1D SET: 60 updates in the opened suite, ToggleFreeCam is never pressed, and the session is waiting");
    g.setShared(false);
    int frames = 0;
    while (g.mode() == 1 && frames < 20) {
        g.ctlFrame();
        presses += g.seenFree() != 0 ? 1 : 0;
        ++frames;
    }
    check(frames == 5 && presses == 1 && g.mode() == 3, "...cleared: ToggleFreeCam is pressed on the 5th ready update, and the game takes it (mode 3)");
    rig.boundary();
    check(rig.cap.count("the controller's shared record was reached through the interface cached at +0x108") == 1 && rig.cap.count("the suite was ready after") == 1 &&
              rig.cap.count("pressing ToggleFreeCam, and again every 10 updates") == 1,
          "LOG: the shared record is found and named once, the wait is logged ('the suite was ready after ...')");
    t::reset();

    // LATE COMPLETION: the game queues the entry itself (+0x3E1 = 1) and finishes it about 6 s later; pressed once, the session alive to place it.
    g.init();
    g.queueDelay = 540;
    installAll(p, rig, g);
    g.ctlFrame(); rig.boundary(); g.ctlFrame(); rig.boundary();
    rig.cap.clear();
    rig.boundary(true);
    g.ctlFrame();
    presses = 0;
    frames = 0;
    while (g.mode() != 3 && frames < 700) {
        g.ctlFrame();
        presses += g.seenFree() != 0 ? 1 : 0;
        ++frames;
        if (frames == 300) {
            check(t::sessionActive() && g.ctl[0x3E1] == 1 && presses == 1, "LATE COMPLETION: 300 updates after the press the game's own retry is running (+0x3E1 = 1), the session is alive, nothing re-pressed");
        }
    }
    check(presses == 1 && g.mode() == 3 && frames > 500 && t::sessionActive(), "...mode 3 arrives about 6 s late, ToggleFreeCam was pressed exactly once (the game queued it), and the session is still on");
    g.freeFrame(); g.ctlFrame(); g.freeFrame();
    rig.boundary();
    check(t::placedActivity() == reinterpret_cast<uint64_t>(g.free) && g.mode() == 4, "...the late entry is placed and locked");
    check(rig.cap.count("the game queued the entry itself (+0x3E1 = 1") == 1 && rig.cap.count("result=pending then accepted presses=1: ready after 5 updates, mode 3 ") == 1,
          "LOG: 'the game queued the entry itself' once, and 'result=pending then accepted presses=1: ready after 5 updates, mode 3 N updates after the first press'");
    t::reset();

    // TIMEOUT after the press: the press is lost (the object the controller cannot show us said no); aborts at 900 updates and says what was unmet.
    g.init();
    g.blockedByOther = true;
    installAll(p, rig, g);
    g.ctlFrame(); rig.boundary(); g.ctlFrame(); rig.boundary();
    rig.cap.clear();
    rig.boundary(true);
    g.ctlFrames(1 + 5 + 899);
    check(t::sessionActive(), "TIMEOUT: 899 updates after the press, still waiting");
    g.ctlFrames(2);
    rig.boundary();
    check(!t::sessionActive() && rig.cap.count("F5 enter aborted: the free camera did not come up within 900 updates of the first ToggleFreeCam press") == 1 &&
              rig.cap.count("result=timeout presses=90") == 1 && rig.cap.count("still unmet: mode is not what is wanted") == 1 && g.pressesTaken == 90,
          "...900: aborts with 'the free camera did not come up ... result=timeout presses=90 ... still unmet: mode is not what is wanted', the session over; the game saw 90 presses");
    t::reset();

    // F3: THE SUITE DROPS THE FIRST TWO PRESSES. ToggleFreeCam is pressed again every 10 updates; press 3 is taken; one summary line at the end.
    g.init();
    g.dropPresses = 2;
    installAll(p, rig, g);
    g.ctlFrame(); rig.boundary(); g.ctlFrame(); rig.boundary();
    rig.cap.clear();
    rig.boundary(true);
    g.ctlFrame();   // PhotoCameraToggle: the suite opens
    std::vector<int> pressFrames;
    frames = 0;
    while (g.mode() == 1 && frames < 200) {
        g.ctlFrame();
        ++frames;
        if (g.seenFree() != 0) pressFrames.push_back(frames);
    }
    check(g.mode() == 3 && g.pressesTaken == 3 && pressFrames.size() == 3 && pressFrames[0] == 5 && pressFrames[1] == 15 && pressFrames[2] == 25 && frames == 25 && t::sessionActive(),
          "DROPPED, THEN ACCEPTED: the suite drops presses 1 and 2; ToggleFreeCam goes out at updates 5, 15 and 25 (10 apart) and press 3 gives mode 3");
    g.freeFrame(); g.ctlFrame(); g.freeFrame();
    rig.boundary();
    check(t::placedActivity() == reinterpret_cast<uint64_t>(g.free) && g.mode() == 4, "...the free camera is placed and locked");
    check(rig.cap.count("result=accepted presses=3") == 1 && rig.cap.count("the suite was ready after 5 updates") == 1 && rig.cap.count("the game queued the entry itself") == 0 &&
              rig.cap.count("F5 enter aborted") == 0,
          "LOG: ONE summary line at the end ('result=accepted presses=3'), the ready line once, no line per repeat");
    t::reset();

    // ACCEPTED ON THE VERY FIRST PRESS (no repeat, presses=1) is the cell above; PENDING AFTER PRESS 2: the second press makes the game queue the entry.
    g.init();
    g.dropPresses = 1;
    g.queueDelay = 300;
    installAll(p, rig, g);
    g.ctlFrame(); rig.boundary(); g.ctlFrame(); rig.boundary();
    rig.cap.clear();
    rig.boundary(true);
    g.ctlFrame();
    frames = 0;
    while (g.mode() != 3 && frames < 700) {
        g.ctlFrame();
        ++frames;
    }
    check(g.mode() == 3 && g.pressesTaken == 2 && frames > 300 && t::sessionActive(), "PENDING AFTER PRESS 2: press 1 dropped, press 2 queued by the game (+0x3E1 = 1), nothing pressed again, mode 3 about 300 updates later");
    g.ctlFrame();   // the controller update that sees mode 3
    rig.boundary();
    check(rig.cap.count("result=pending then accepted presses=2") == 1 && rig.cap.count("the game queued the entry itself (+0x3E1 = 1") == 1, "LOG: 'result=pending then accepted presses=2', 'queued' once");
    t::reset();

    // TIMEOUT of the readiness wait: +0x1D never clears.
    g.init();
    g.setShared(true);
    installAll(p, rig, g);
    g.ctlFrame(); rig.boundary(); g.ctlFrame(); rig.boundary();
    rig.cap.clear();
    rig.boundary(true);
    g.ctlFrames(1 + 899);
    check(t::sessionActive(), "TIMEOUT OF THE READINESS WAIT: 899 updates in, still waiting");
    g.ctlFrames(3);
    rig.boundary();
    check(!t::sessionActive() && rig.cap.count("F5 enter aborted: the suite was not ready within 900 updates") == 1 && rig.cap.count("still unmet: the shared record's +0x1D = 1") == 1,
          "...900: aborts with 'the suite was not ready ... still unmet: the shared record's +0x1D = 1'");
    t::reset();

    // THE SHARED RECORD CANNOT BE READ (its accessor is not a plain lea): said once with the accessor's bytes, and the wait goes on without it.
    g.init();
    g.useLeaAccessor(false);
    installAll(p, rig, g);
    g.ctlFrame(); rig.boundary(); g.ctlFrame(); rig.boundary();
    rig.cap.clear();
    rig.boundary(true);
    g.ctlFrame();
    presses = 0;
    for (int i = 0; i < 8; ++i) {
        g.ctlFrame();
        presses += g.seenFree() != 0 ? 1 : 0;
    }
    rig.boundary();
    check(presses == 1 && rig.cap.count("could not be read as a plain `lea rax,[rcx+disp]; ret`") == 1 && rig.cap.count("48 8B 41 08 C3 00 00 00") == 1 &&
              rig.cap.count("the controller's shared record was reached through") == 0,
          "ACCESSOR NOT A PLAIN LEA: the press is not held back, and ONE line says so with the accessor's first 8 bytes (48 8B 41 08 C3 00 00 00)");
    t::reset();
}

// The dither-fade global: written while a placement stands, put back when the camera is gone.
void testFadeGlue(const Pages& p) {
    std::printf("glue: the avatar dither-fade global\n");
    namespace t = edvr::explorercamtest;
    static Game g;
    static int32_t fadeMode;
    static float fadeAmount;
    Rig rig;
    auto begin = [&](int32_t initial) {
        t::reset();
        g.init();
        fadeMode = initial;
        fadeAmount = 0.0f;
        rig = Rig();
        installAll(p, rig, g);
        t::setFadeGlobal(&fadeMode, &fadeAmount);
    };
    // Into the free camera by F5 from mode 3, placed, locked, UI hidden.
    auto place = [&]() {
        g.setMode(3);
        g.free[0x473] = 1;
        g.ctlFrame(); rig.boundary(); g.ctlFrame(); rig.boundary();
        rig.boundary(true);
        g.ctlFrame();
        g.freeFrame(); g.freeFrame(); g.ctlFrame(); g.uiFrame(); g.uiFrame();
    };

    // ---- the whole F5 round trip ------------------------------------------------------------------------------------------------------
    begin(-1);
    place();
    check(fadeMode == -1, "(placed, locked, hidden; the frame thread has not looked yet: the global still reads -1)");
    rig.cap.clear();
    rig.boundary();
    check(fadeMode == 0 && t::fadeOurs() && rig.cap.count("avatar fade: wrote 0 to the dither-fade mode global") == 1 && has(rig.cap.nth("avatar fade: wrote 0", 0), "was -1 = auto") &&
              has(rig.cap.nth("avatar fade: wrote 0", 0), "amount float at +0x601E088 reads 0"),
          "WRITTEN: a placement stands and the global read -1: it is 0 now, one line says so (and what the amount float reads)");
    rig.boundary();
    rig.boundary();
    check(fadeMode == 0 && rig.cap.count("avatar fade:") == 1, "...once, not every frame");
    rig.advance(5200);
    g.freeFrame();
    rig.cap.clear();
    rig.boundary();
    check(has(rig.cap.nth("explorer cam: heartbeat:", 0), "fade_global_held_by_edvr=yes"), "the heartbeat says EDVR holds the fade global");
    // Leaving: the global stays 0 until the camera has CLOSED.
    rig.cap.clear();
    rig.boundary(true);
    g.ctlFrame();   // the exit begins (the UI is held)
    g.freeFrame();
    g.uiFrame(); g.uiFrame();
    g.ctlFrame();   // the close is pressed: mode 0 in the game, the sequencer has not seen it yet
    rig.boundary();
    check(fadeMode == 0 && g.mode() == 0, "F5 EXIT: the close was pressed but the controller has not yet read mode 0: the global is still 0 (never restored while the camera could be inside the body)");
    g.ctlFrame();   // mode 0 read: the session is over
    rig.boundary();
    check(fadeMode == -1 && !t::fadeOurs() && rig.cap.count("avatar fade: put the dither-fade mode global back to -1 (auto)") == 1 && has(rig.cap.nth("avatar fade: put", 0), "closed (controller mode 0)"),
          "...mode 0 read, the session over: RESTORED to -1, one line");
    check(rig.cap.indexOf("F5 exit: closing the camera") < rig.cap.indexOf("avatar fade: put the dither-fade") && rig.cap.indexOf("F5 exit: giving the camera UI back first") < rig.cap.indexOf("F5 exit: closing the camera"),
          "LOG ORDER of the exit: the UI given back, then the close, then the fade restored");
    rig.boundary();
    check(fadeMode == -1 && rig.cap.count("avatar fade:") == 1, "...and not touched again");

    // ---- someone else owns it -------------------------------------------------------------------------------------------------------------
    begin(7);
    place();
    rig.cap.clear();
    rig.boundary();
    rig.boundary();
    check(fadeMode == 7 && !t::fadeOurs() && rig.cap.count("avatar fade: the dither-fade mode global reads 7, not -1 (auto), so someone else owns it") == 1,
          "FOREIGN VALUE (7): left alone, one line says someone else owns it");
    g.userPresses(g.photoAction, &Game::ctlFrame);
    g.ctlFrame();
    rig.boundary();
    check(fadeMode == 7, "...and still 7 after the camera closes (never restored, never ours)");

    // ---- a detach releases the placement: restored; the return writes again -----------------------------------------------------------------
    begin(-1);
    place();
    rig.boundary();
    check(fadeMode == 0, "(placed: 0)");
    g.setMode(5);
    g.freeFrame();
    g.ctlFrame();
    rig.cap.clear();
    rig.boundary();
    check(fadeMode == -1 && t::sessionActive() && rig.cap.count("put the dither-fade mode global back to -1 (auto)") == 1 && has(rig.cap.nth("put the dither-fade", 0), "detached (controller mode 5)"),
          "DETACH (mode 5): the placement released, the camera detached: restored to -1, the session still on");
    g.setMode(3);
    g.freeFrame(); g.freeFrame(); g.ctlFrame();
    rig.boundary();
    check(fadeMode == 0 && t::fadeOurs(), "...the return to 3: placed again, 0 again");

    // ---- the feature turned off while the camera is open inside the body: held until it closes ------------------------------------------------
    rig.cap.clear();
    rig.f.hotkey = "";
    rig.boundary();
    check(fadeMode == 0 && t::fadeOurs() && !t::placeActive(), "FEATURE OFF with the camera still open (mode 4): the global is HELD at 0 (the camera is inside the body)");
    g.ctlFrame();
    g.userPresses(g.photoAction, &Game::ctlFrame);   // the player closes the camera by hand
    g.ctlFrame();
    check(g.mode() == 0, "(the player closes the camera)");
    rig.boundary();
    check(fadeMode == -1 && !t::fadeOurs() && rig.cap.count("avatar fade: put the dither-fade mode global back to -1 (auto)") == 1,
          "...the camera closed (the controller hook still publishes the mode while EDVR holds the global): restored to -1 though the feature is off");
    rig.f.hotkey = "F5";

    // ---- changed under us ---------------------------------------------------------------------------------------------------------------------
    begin(-1);
    place();
    rig.boundary();
    fadeMode = 5;   // someone else sets it
    rig.boundary(true);
    g.ctlFrame(); g.freeFrame(); g.uiFrame(); g.uiFrame(); g.ctlFrame(); g.ctlFrame();
    rig.cap.clear();
    rig.boundary();
    check(fadeMode == 5 && !t::fadeOurs() && rig.cap.count("now reads 5, not the 0 Explorer Cam wrote, so it is left alone") == 1,
          "CHANGED UNDER US (5): the restore is skipped and said, the value is left as it was");

    // ---- a stale release with the camera still inside: held -------------------------------------------------------------------------------------
    begin(-1);
    place();
    rig.boundary();
    g.freeFrame();
    g.ctlFrame();
    rig.boundary();
    for (int i = 0; i < 31; ++i) { g.ctlFrame(); rig.boundary(); }
    check(t::phase() == 0 && t::sessionActive() && fadeMode == 0 && t::fadeOurs(),
          "STALE RELEASE (the activity went silent) with the camera still in the body (mode 4): the placement is released but the global stays 0");

    // ---- unload ----------------------------------------------------------------------------------------------------------------------------------
    t::shutdown();
    check(fadeMode == -1 && !t::fadeOurs(), "UNLOAD: the global goes back to -1");
    begin(-1);
    place();
    rig.boundary();
    fadeMode = 9;
    t::shutdown();
    check(fadeMode == 9, "UNLOAD with the value changed under us: not touched");
    begin(-1);
    t::shutdown();
    check(fadeMode == -1, "UNLOAD when it was never ours: nothing written");
    t::reset();
}

// ================================ Phase 2: hiding the head parts ================================
void testHeadHidePure() {
    std::printf("head hiding: the hide list, the local-AMC test, the capture filter, the census text\n");
    static_assert(ecm::kPartCount == 54 && ecm::kPartStride == 0x680 && ecm::kOffAmcInstances == 0x11C0 && ecm::kOffAmcMasks == 0x17A0 && ecm::kOffAmcPartFlags == 0x1814 &&
                      ecm::kOffAmcPartState == 0x1820 && ecm::kOffAmcAvatarPoseId == 0x260 && ecm::kOffAmcAvatarPose == 0x268 && ecm::kOffAmcParams == 0x50 && ecm::kOffParamsMode == 0x14 &&
                      ecm::kAvatarPoseToSkeleton == 0x30 && ecm::kPartNameTableRva == 0x5E9C7D0,
                  "the AMC layout of NOTES_p2");
    // The masks: AMC + 0x17A0 + idx*0x680 + 8k, which is entry+0x5F0+8k with the entry at AMC+0x11B0.
    check(ecm::kOffAmcMasks == 0x11B0 + 0x5F0 && ecm::kOffAmcInstances == 0x11B0 + 0x10, "the instance and mask arrays are entry+0x10 and entry+0x5F0 of the part array at AMC+0x11B0");
    const uint32_t expect[12] = {0, 2, 3, 4, 5, 12, 13, 15, 20, 25, 26, 32};
    bool same = ecm::kHeadPartCount == 12;
    for (uint32_t i = 0; i < 12; ++i) same = same && ecm::kHeadParts[i] == expect[i];
    check(same, "the hide list is exactly parts 0 2 3 4 5 12 13 15 20 25 26 32");
    check(std::strcmp(ecm::kPartNames[0], "Head") == 0 && std::strcmp(ecm::kPartNames[2], "Eyes") == 0 && std::strcmp(ecm::kPartNames[3], "Helmet") == 0 &&
              std::strcmp(ecm::kPartNames[4], "SkullCap") == 0 && std::strcmp(ecm::kPartNames[5], "Hair") == 0 && std::strcmp(ecm::kPartNames[12], "Beard") == 0 &&
              std::strcmp(ecm::kPartNames[13], "Teeth") == 0 && std::strcmp(ecm::kPartNames[15], "Hat") == 0 && std::strcmp(ecm::kPartNames[20], "EyeWear") == 0 &&
              std::strcmp(ecm::kPartNames[25], "EVASuit_Helmet") == 0 && std::strcmp(ecm::kPartNames[26], "EVASuit_Eyewear") == 0 &&
              std::strcmp(ecm::kPartNames[32], "EVASuit_Gear_Head") == 0 && std::strcmp(ecm::kPartNames[49], "FirstPersonSkeleton") == 0 && std::strcmp(ecm::kPartNames[53], "EVASuit_Gear_Legs") == 0,
          "...and their names are Head Eyes Helmet SkullCap Hair Beard Teeth Hat EyeWear EVASuit_Helmet EVASuit_Eyewear EVASuit_Gear_Head (and 49 is FirstPersonSkeleton, 53 EVASuit_Gear_Legs)");
    bool onlyHead = true;
    for (uint32_t i = 0; i < ecm::kPartCount; ++i) {
        bool inList = false;
        for (uint32_t h : expect) inList = inList || h == i;
        onlyHead = onlyHead && ecm::isHeadPart(i) == inList;
    }
    check(onlyHead && !ecm::isHeadPart(1) && !ecm::isHeadPart(24) && !ecm::isHeadPart(33) && !ecm::isHeadPart(49), "isHeadPart is true for those twelve and for no other of the 54 (not Body, EVASuit, Gear_Neck, FirstPersonSkeleton)");

    // The local AMC test.
    const uint64_t skel = 0x1000000;
    check(ecm::isLocalAmc(0xFFFFFFFFu, skel + 0x30, skel, 3), "LOCAL AMC: handle resolved, +0x268 minus 0x30 is the site-1 skeleton, mode 3");
    check(!ecm::isLocalAmc(0x5F2867Cu, skel + 0x30, skel, 3), "...not with the handle unresolved (the id dword is not 0xFFFFFFFF)");
    check(!ecm::isLocalAmc(0xFFFFFFFFu, skel + 0x38, skel, 3) && !ecm::isLocalAmc(0xFFFFFFFFu, skel + 0x28, skel, 3) && !ecm::isLocalAmc(0xFFFFFFFFu, skel, skel, 3),
          "...not when +0x268 minus 0x30 is anything but the skeleton (off by 8 either way, or not offset at all)");
    check(!ecm::isLocalAmc(0xFFFFFFFFu, skel + 0x30, skel, 1) && !ecm::isLocalAmc(0xFFFFFFFFu, skel + 0x30, skel, -1) && !ecm::isLocalAmc(0xFFFFFFFFu, skel + 0x30, skel, 4),
          "...not in any mode but 3 (first person is 1, unreadable params -1)");
    check(!ecm::isLocalAmc(0xFFFFFFFFu, 0x30, 0, 3) && !ecm::isLocalAmc(0xFFFFFFFFu, 0, 0, 3) && !ecm::isLocalAmc(0xFFFFFFFFu, 0x10, skel, 3),
          "...and never against a skeleton of 0 (nothing captured), nor a pose pointer below 0x30");

    // The capture filter.
    const uintptr_t s1 = 0x140000000ull + ecm::kFindSite1Rva, s2 = 0x140000000ull + ecm::kFindSite2Rva, lit = 0x140000000ull + ecm::kPovNameRva;
    check(ecm::findCaptureSite(s1, lit, s1, s2, lit) == 0 && ecm::findCaptureSite(s2, lit, s1, s2, lit) == 1, "CAPTURE FILTER: site 1 is 0, site 2 is 1");
    check(ecm::findCaptureSite(s1 + 1, lit, s1, s2, lit) == -1 && ecm::findCaptureSite(0x1234, lit, s1, s2, lit) == -1, "...any other caller is -1");
    check(ecm::findCaptureSite(s1, lit + 1, s1, s2, lit) == -1 && ecm::findCaptureSite(s2, 0, s1, s2, lit) == -1, "...any other name pointer is -1, however right the caller");
    check(ecm::findCaptureSite(s1, lit, s1, s2, 0) == -1 && ecm::findCaptureSite(0, 0, 0, 0, 0) == -1, "...and nothing matches while the literal's address is unset (0)");

    // The census text.
    ecm::AmcCensus c;
    c.amc = 0x7FF612340000ull;
    c.avatarPose = skel + 0x30;
    c.skeleton = skel;
    c.mode = 3;
    c.viewFilter = 1;
    c.alpha = 1.0f;
    c.context = 7;
    c.flagBytes[3] = 0x14;
    const uint8_t idxs[] = {0, 1, 2, 3, 24, 25, 32, 49};
    for (uint8_t i : idxs) {
        ecm::PartCensus& p = c.part[c.count++];
        p.idx = i;
        p.variants = i == 2 ? 0x0F : 0x01;
        p.maskBits = i == 3 ? 0 : p.variants;
        p.flags = i == 24 ? 0x14 : 0x05;
        p.state = 1;
        p.pending = -1;
        p.current = 100 + i;
    }
    Capture cap;
    ecm::formatAmcCensus(c, ecm::Sink{&Capture::add, &cap});
    const std::string head = cap.nth("census of the local third-person avatar", 0), parts = cap.nth("census parts:", 0);
    check(cap.lines.size() == 2 && has(head, "AMC 0x7FF612340000") && has(head, "mode 3") && has(head, "8 of 54 parts have instances") && has(head, "view_filter(+0x2A8)=1") &&
              has(head, "context(+0x334)=7") && has(head, "flag_bytes(+0x320..323)=00 00 00 14"),
          "CENSUS: a header line names the AMC, the mode, how many of the 54 parts have instances, the view filter, the context and the flag bytes");
    check(has(parts, "[0 Head v=0 st=1 fl=0x05 item=-1/100 m=0 HIDE]") && has(parts, "[1 Body v=0 ") && !has(parts, "[1 Body v=0 st=1 fl=0x05 item=-1/101 m=0 HIDE]") &&
              has(parts, "[2 Eyes v=0123 st=1 fl=0x05 item=-1/102 m=0123 HIDE]") && has(parts, "[3 Helmet v=0 st=1 fl=0x05 item=-1/103 m= HIDE]") &&
              has(parts, "[24 EVASuit v=0 st=1 fl=0x14 item=-1/124 m=0 EXCLUDED]") && has(parts, "[25 EVASuit_Helmet v=0") && has(parts, "[32 EVASuit_Gear_Head v=0") &&
              has(parts, "[49 FirstPersonSkeleton v=0"),
          "...each part with an instance: index, name, which variants exist, state, flags, items, which masks are non-zero, HIDE for the head set, EXCLUDED for flag 0x10");
    // Many parts split across lines under the log's line length.
    ecm::AmcCensus big;
    big.amc = 0x1000;
    for (uint32_t i = 0; i < ecm::kPartCount; ++i) {
        ecm::PartCensus& p = big.part[big.count++];
        p.idx = static_cast<uint8_t>(i);
        p.variants = 0x0F;
        p.maskBits = 0x0F;
        p.flags = 0x05;
        p.state = 1;
        p.pending = -1;
        p.current = 12345;
    }
    Capture cap2;
    ecm::formatAmcCensus(big, ecm::Sink{&Capture::add, &cap2});
    size_t longest = 0;
    int partsLines = 0;
    for (const std::string& l : cap2.lines) {
        longest = std::max(longest, l.size());
        partsLines += has(l, "census parts:") ? 1 : 0;
    }
    check(cap2.lines.size() == 1 + static_cast<size_t>(partsLines) && partsLines >= 2 && longest < ecm::kLineBytes && cap2.count("[53 EVASuit_Gear_Legs ") == 1 && cap2.count("[0 Head ") == 1,
          "...all 54 parts at four variants split over several 'census parts' lines, none past the line length, every part exactly once");
    // The heartbeat carries the hide counters.
    ecm::HeartbeatIn hb;
    hb.hide = "on";
    hb.localAmc = 0x7FF612340000ull;
    hb.amcCalls = 500;
    hb.amcMatches = 120;
    hb.hideCalls = 100;
    hb.hideCallsWindow = 40;
    hb.zeroed = 4800;
    hb.zeroedWindow = 1920;
    hb.hideFaults = 1;
    char line[ecm::kLineBytes];
    ecm::formatHeartbeat(line, sizeof(line), hb);
    const std::string beat(line);
    check(has(beat, "head_hide=on local_amc=0x7FF612340000 amc_calls=500 local_matches=120 hide_calls=100(+40) masks_zeroed=4800(+1920) hide_faults=1"),
          "the 5 s heartbeat carries head_hide, the local AMC, the calls, the matches, the hide calls and zeroed masks per window, and the faults");
}

// A synthetic game image for head hiding: the avatar fade update at its RVA (the real 16-byte prologue), a FindJoint at its RVA, the two attach thunks, the
// povCamera literal and the 54-name part table. Offsets are the build's RVAs, so the hooks' derived addresses (site returns, literal, table) are the real ones.
constexpr size_t kCImgSize = 0x5F30000;
uint8_t* g_cimg = nullptr;
void cimgCommit(size_t rva, size_t len) {
    const size_t lo = rva & ~static_cast<size_t>(0xFFF), hi = (rva + len + 0xFFF) & ~static_cast<size_t>(0xFFF);
    VirtualAlloc(g_cimg + lo, hi - lo, MEM_COMMIT, PAGE_EXECUTE_READWRITE);
}
void cimgThunk(size_t entryRva, size_t callRva) {   // `call r8` at callRva (returns to callRva+3), rcx and rdx as given
    cimgCommit(entryRva, 32);
    uint8_t* p = g_cimg + entryRva;
    size_t n = 0;
    p[n++] = 0x48; p[n++] = 0x83; p[n++] = 0xEC; p[n++] = 0x28;
    while (entryRva + n < callRva) p[n++] = 0x90;
    p[n++] = 0x41; p[n++] = 0xFF; p[n++] = 0xD0;
    p[n++] = 0x48; p[n++] = 0x83; p[n++] = 0xC4; p[n++] = 0x28; p[n++] = 0xC3;
}
// FUN 0x19B1240 for ONE humanoid, in machine code at its real address: block 1 (third-person avatar) calls r8 (FindJoint) at +0x19B12D2, returning to +0x19B12D5
// (site 1); block 2 (first-person avatar) calls it at +0x19B1356, returning to +0x19B1359 (site 2). One frame, no push between. rcx = the third-person interface
// (0 skips block 1), rdx = the name, r8 = FindJoint, r9 = the first-person interface (0 skips block 2: an NPC has none).
// g_cimgBetween, when set, is called between the two attaches of a local invocation (the rig's way to run another thread's attach INSIDE the invocation).
void (*g_cimgBetween)() = nullptr;
void cimgInvoke() {
    cimgCommit(0x19B1240, 0x200);
    uint8_t* const base = g_cimg + 0x19B1240;
    std::memset(base, 0x90, 0x200);
    size_t n = 0;
    auto put = [&](std::initializer_list<uint8_t> b) { for (uint8_t x : b) base[n++] = x; };
    put({0x48, 0x83, 0xEC, 0x48});                     // sub rsp, 48h
    put({0x4C, 0x89, 0x4C, 0x24, 0x30});               // mov [rsp+30h], r9
    put({0x4C, 0x89, 0x44, 0x24, 0x28});               // mov [rsp+28h], r8
    put({0x48, 0x89, 0x54, 0x24, 0x20});               // mov [rsp+20h], rdx
    put({0x48, 0x85, 0xC9});                           // test rcx, rcx
    put({0x74, static_cast<uint8_t>(0x95 - (n + 2))}); // je block2
    n = 0x92;
    put({0x41, 0xFF, 0xD0});                           // call r8        (returns to +0x19B12D5)
    put({0x4C, 0x8B, 0x4C, 0x24, 0x30});               // block2: mov r9, [rsp+30h]
    put({0x4D, 0x85, 0xC9});                           // test r9, r9
    const size_t jeAt = n;
    put({0x74, 0x00});                                 // je end (patched)
    {   // between the two attaches the rig can run code (another thread's attach): `mov rax, [&g_cimgBetween]; test rax, rax; je +2; call rax`, then r9 is reloaded
        put({0x48, 0xB8});
        const uint64_t at = reinterpret_cast<uint64_t>(&g_cimgBetween);
        for (int i = 0; i < 8; ++i) base[n++] = static_cast<uint8_t>(at >> (8 * i));
        put({0x48, 0x8B, 0x00});                       // mov rax, [rax]
        put({0x48, 0x85, 0xC0});                       // test rax, rax
        put({0x74, 0x02});                             // je +2
        put({0xFF, 0xD0});                             // call rax
        put({0x4C, 0x8B, 0x4C, 0x24, 0x30});           // mov r9, [rsp+30h]
    }
    put({0x4C, 0x89, 0xC9});                           // mov rcx, r9
    put({0x48, 0x8B, 0x54, 0x24, 0x20});               // mov rdx, [rsp+20h]
    put({0x4C, 0x8B, 0x44, 0x24, 0x28});               // mov r8, [rsp+28h]
    base[jeAt + 1] = static_cast<uint8_t>(0x119 - (jeAt + 2));
    n = 0x116;
    put({0x41, 0xFF, 0xD0});                           // call r8        (returns to +0x19B1359)
    put({0x48, 0x83, 0xC4, 0x48, 0xC3});               // end: add rsp, 48h; ret
}
// ---- the synthetic skeleton interfaces (Phase 3: the head-joint eye) -------------------------------------------------------------------------------
// RR's and AO's vtables with the build's slots (+0x18 pose, +0x30 FindJoint, +0x48 world, +0x58 model) pointing at trampolines placed at the EXACT RVAs the build has,
// to native stubs; FindJoint (+0xFDDB10) is the real hook target. A stub records how it was called; a fault can be injected in each.
struct CamRec {
    uint32_t headIdx = 12;
    uint32_t headJunk = 0;           // OR-ed into FindJoint(head)'s result: the part of eax above the u16 the game returns
    int poseCalls = 0, modelCalls = 0, findHeadCalls = 0, worldCalls = 0;
    uintptr_t lastModelIface = 0, lastModelOut = 0;
    uint32_t lastModelIdx = 0xFFFFFFFFu, modelThread = 0;
    int faultAt = 0;                 // 1 GetPoseData, 2 FindJoint(head), 3 the model matrix (+0x58)
    int faultCount = 0;              // > 0: only that many calls fault (then they work again); 0 with faultAt set: every call
    float headM[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0.0f, 1.66f, 0.03f, 1};   // the live +0x58 head matrix: identity rotation, standing
    float otherM[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0.0f, 1.0f, 0.0f, 1};
    bool swapModelSlot = false;      // the +0x58 stub changes the RR vtable's +0x48 slot behind the hook's back
};
CamRec g_cr;
void crFault() {
    if (g_cr.faultCount > 0 && --g_cr.faultCount == 0) g_cr.faultAt = 0;
    volatile int* bad = reinterpret_cast<volatile int*>(static_cast<uintptr_t>(0x8));
    *bad = 1;
}
void* __fastcall crPose(void* iface) {
    ++g_cr.poseCalls;
    if (g_cr.faultAt == 1) crFault();
    return *reinterpret_cast<void**>(static_cast<uint8_t*>(iface) + 0x20);
}
uint32_t __fastcall crFind(void*, const char* name) {
    if (std::strcmp(name, ecm::kHeadName) == 0) {
        ++g_cr.findHeadCalls;
        if (g_cr.faultAt == 2) crFault();
        return g_cr.headIdx | g_cr.headJunk;
    }
    return 3;   // the povCamera joint (and, as before, every other name)
}
void __fastcall crModel(void* iface, uint32_t idx, float* out) {
    ++g_cr.modelCalls;
    g_cr.modelThread = GetCurrentThreadId();
    g_cr.lastModelIface = reinterpret_cast<uintptr_t>(iface);
    g_cr.lastModelIdx = idx;
    g_cr.lastModelOut = reinterpret_cast<uintptr_t>(out);
    if (g_cr.faultAt == 3) crFault();
    std::memcpy(out, idx == g_cr.headIdx ? g_cr.headM : g_cr.otherM, 64);
    if (g_cr.swapModelSlot) *reinterpret_cast<uint64_t*>(g_cimg + ecm::kRrVtableRva + 8 * 9) = reinterpret_cast<uint64_t>(g_cimg) + 0x3000000;
}
void __fastcall crWorld(void*, uint32_t, float* out) {
    ++g_cr.worldCalls;
    std::memset(out, 0, 64);
}
uint64_t* cimgQ(size_t rva) {
    cimgCommit(rva, 8);
    return reinterpret_cast<uint64_t*>(g_cimg + rva);
}
void cimgTramp(size_t rva, const void* target) {
    cimgCommit(rva, 16);
    uint8_t* q = g_cimg + rva;
    q[0] = 0x48;
    q[1] = 0xB8;   // mov rax, imm64
    std::memcpy(q + 2, &target, 8);
    q[10] = 0xFF;
    q[11] = 0xE0;  // jmp rax
}
void cimgFollowInit() {
    g_cr = CamRec();
    cimgTramp(0x43FB900, reinterpret_cast<const void*>(&crPose));    // RR +0x18
    cimgTramp(0x43F7180, reinterpret_cast<const void*>(&crWorld));   // RR +0x48
    cimgTramp(0x43F6EB0, reinterpret_cast<const void*>(&crModel));   // RR +0x58
    cimgTramp(0xFDE310, reinterpret_cast<const void*>(&crPose));     // AO +0x18
    cimgTramp(0xFDDEB0, reinterpret_cast<const void*>(&crWorld));    // AO +0x48
    cimgTramp(0xFDDD10, reinterpret_cast<const void*>(&crModel));    // AO +0x58
    cimgTramp(0x3000100, reinterpret_cast<const void*>(&crWorld));   // a function that is none of the slots'
    const uint64_t b = reinterpret_cast<uint64_t>(g_cimg);
    for (uint32_t i = 0; i < ecm::kHeadSlots; ++i) {
        *cimgQ(ecm::kRrVtableRva + 8 * ecm::kHeadSlotIndex[i]) = b + ecm::kRrFunctions[i];
        *cimgQ(ecm::kAoVtableRva + 8 * ecm::kHeadSlotIndex[i]) = b + ecm::kAoFunctions[i];
    }
    cimgCommit(ecm::kHeadNameRva, 32);
    std::memcpy(g_cimg + ecm::kHeadNameRva, ecm::kHeadName, sizeof(ecm::kHeadName));
}
void cimgInit(int corruptName = -1) {
    if (!g_cimg) g_cimg = static_cast<uint8_t*>(VirtualAlloc(nullptr, kCImgSize, MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    // The dither fade: the real prologue, a rip-relative displacement of 0, then the matching epilogue. It does nothing: the rig plays the game's masks.
    cimgCommit(ecm::kAvatarFadeRva, 64);
    {
        uint8_t* p = g_cimg + ecm::kAvatarFadeRva;
        size_t n = 0;
        for (uint8_t b : ecm::kAvatarFadePrologue) p[n++] = b;
        p[n++] = 0; p[n++] = 0; p[n++] = 0; p[n++] = 0;
        // The body: when the flag byte at +0x40 is set, write 0xFF into part 0 variant 0's view mask (as if the game's data changed during the call), so a test
        // can tell whether the hide ran AFTER the original: cmp byte ptr [rip+37], 0; je +11; mov qword ptr [rcx+17A0h], 0FFh.
        p[n++] = 0x80; p[n++] = 0x3D; p[n++] = 37; p[n++] = 0; p[n++] = 0; p[n++] = 0; p[n++] = 0x00;
        p[n++] = 0x74; p[n++] = 0x0B;
        p[n++] = 0x48; p[n++] = 0xC7; p[n++] = 0x81; p[n++] = 0xA0; p[n++] = 0x17; p[n++] = 0x00; p[n++] = 0x00; p[n++] = 0xFF; p[n++] = 0x00; p[n++] = 0x00; p[n++] = 0x00;
        p[n++] = 0x48; p[n++] = 0x81; p[n++] = 0xC4; p[n++] = 0x10; p[n++] = 0x01; p[n++] = 0x00; p[n++] = 0x00;   // add rsp, 110h
        p[n++] = 0x5F; p[n++] = 0x5E; p[n++] = 0x5B; p[n++] = 0xC3;                                                // pop rdi; pop rsi; pop rbx; ret
        p[0x40] = 0;   // the flag
    }
    // FindJoint: the real prologue, undone, and a TAIL jump to a stub with rcx and rdx as they came in (3 for every name but the head's, which the follow cells set).
    cimgCommit(ecm::kFindJointRva, 64);
    {
        uint8_t* p = g_cimg + ecm::kFindJointRva;
        size_t n = 0;
        for (uint8_t b : ecm::kFindJointPrologue) p[n++] = b;
        p[n++] = 0x48; p[n++] = 0x83; p[n++] = 0xC4; p[n++] = 0x20; p[n++] = 0x5F;                                 // add rsp,20h; pop rdi
        p[n++] = 0x48; p[n++] = 0x8B; p[n++] = 0x5C; p[n++] = 0x24; p[n++] = 0x08;                                 // mov rbx,[rsp+8]
        const uint64_t stub = reinterpret_cast<uint64_t>(&crFind);
        p[n++] = 0x48; p[n++] = 0xB8;                                                                              // mov rax, imm64
        std::memcpy(p + n, &stub, 8);
        n += 8;
        p[n++] = 0xFF; p[n++] = 0xE0;                                                                              // jmp rax
    }
    cimgCommit(0x3000000, 64);         // a function that is not FindJoint (a prologue of 0xCC)
    std::memset(g_cimg + 0x3000000, 0xCC, 64);
    cimgInvoke();                      // sites 1 and 2
    cimgThunk(0x1A00000, 0x1A00008);   // neither
    cimgCommit(ecm::kPovNameRva, 32);
    std::memcpy(g_cimg + ecm::kPovNameRva, "def_c_povCamera_joint", 22);
    // The part-name table: 54 absolute pointers at +0x5E9C7D0, the strings after them.
    cimgCommit(ecm::kPartNameTableRva, ecm::kPartCount * 8);
    cimgCommit(0x5E9D000, ecm::kPartCount * 0x40);
    for (uint32_t i = 0; i < ecm::kPartCount; ++i) {
        char* s = reinterpret_cast<char*>(g_cimg + 0x5E9D000 + i * 0x40);
        std::snprintf(s, 0x40, "%s", static_cast<int>(i) == corruptName ? "Xead" : ecm::kPartNames[i]);
        const uint64_t abs = reinterpret_cast<uint64_t>(s);
        std::memcpy(g_cimg + ecm::kPartNameTableRva + 8 * i, &abs, 8);
    }
    cimgFollowInit();
}
using ImgFade = uint64_t (__fastcall*)(void*);
using ImgFind = uint64_t (__fastcall*)(void* iface, const void* name, void* fn);
using ImgInvoke = uint64_t (__fastcall*)(void* third, const void* name, void* fn, void* first);
// One invocation for a humanoid with BOTH avatars (the local player): third-person at site 1, then first-person at site 2, from one frame.
uint64_t imgInvoke(void* third, void* first, const void* name) { return reinterpret_cast<ImgInvoke>(g_cimg + 0x19B1240)(third, name, g_cimg + ecm::kFindJointRva, first); }
// A LONE attach: site 0 = an invocation with only the third-person avatar (an NPC's), site 1 = only the first-person one, site 2 = a call from somewhere else.
uint64_t imgAttach(int site, void* iface, const void* name) {
    if (site == 0) return imgInvoke(iface, nullptr, name);
    if (site == 1) return imgInvoke(nullptr, iface, name);
    return reinterpret_cast<ImgFind>(g_cimg + 0x1A00000)(iface, name, g_cimg + ecm::kFindJointRva);
}
// ...from a deeper stack frame (the same call, but its return-address slot is somewhere else).
__declspec(noinline) uint64_t imgDeep(int depth, int site, void* iface, const void* name) {
    volatile uint8_t pad[128];
    pad[0] = static_cast<uint8_t>(depth);
    pad[1] = 0;
    return (depth > 0 ? imgDeep(depth - 1, site, iface, name) : imgAttach(site, iface, name)) + pad[1];
}
void imgFade(void* amc) { reinterpret_cast<ImgFade>(g_cimg + ecm::kAvatarFadeRva)(amc); }

// An AMC the way the rig plays it: the handle slot, the creation params, parts with instances, every view mask non-zero (the game's visibility job just ran).
struct AmcBuf {
    alignas(16) uint8_t b[0x1E000];
    alignas(16) uint8_t params[0x40];
    void init(uint64_t skeleton, int32_t mode, uint32_t id = 0xFFFFFFFFu, uint64_t poseOffset = 0x30) {
        std::memset(b, 0, sizeof(b));
        std::memset(params, 0, sizeof(params));
        const uint64_t pose = skeleton + poseOffset, pp = reinterpret_cast<uint64_t>(params);
        std::memcpy(b + ecm::kOffAmcAvatarPoseId, &id, 4);
        std::memcpy(b + ecm::kOffAmcAvatarPose, &pose, 8);
        std::memcpy(b + ecm::kOffAmcParams, &pp, 8);
        std::memcpy(params + ecm::kOffParamsMode, &mode, 4);
        const int32_t viewFilter = 1, context = 7;
        std::memcpy(b + ecm::kOffAmcViewFilter, &viewFilter, 4);
        std::memcpy(b + ecm::kOffAmcContext, &context, 4);
        const float alpha = 1.0f;
        std::memcpy(b + ecm::kOffAmcAlpha, &alpha, 4);
        static const uint32_t withInstance[] = {0, 1, 2, 3, 5, 12, 24, 25, 32, 49};
        for (uint32_t idx : withInstance) {
            const uint32_t at = idx * ecm::kPartStride;
            for (uint32_t k = 0; k < (idx == 2 ? 4u : 1u); ++k) {
                const uint64_t inst = 0x2000 + idx * 16 + k;
                std::memcpy(b + ecm::kOffAmcInstances + at + 8 * k, &inst, 8);
            }
            b[ecm::kOffAmcPartFlags + at] = idx == 24 ? 0x14 : 0x05;
            const int32_t state = 1, pending = -1, current = 100 + static_cast<int32_t>(idx);
            std::memcpy(b + ecm::kOffAmcPartState + at, &state, 4);
            std::memcpy(b + ecm::kOffAmcPartPending + at, &pending, 4);
            std::memcpy(b + ecm::kOffAmcPartCurrent + at, &current, 4);
        }
        refillMasks();
    }
    void setTransform(const void* obj) {
        const uint64_t p = reinterpret_cast<uint64_t>(obj);
        std::memcpy(b + ecm::kOffAmcTransform, &p, 8);
    }
    // What the game's visibility job does every frame: every mask non-zero.
    void refillMasks() {
        for (uint32_t idx = 0; idx < ecm::kPartCount; ++idx)
            for (uint32_t k = 0; k < ecm::kPartVariants; ++k) {
                const uint64_t m = 0x8000000000000001ull + idx;
                std::memcpy(b + ecm::kOffAmcMasks + idx * ecm::kPartStride + 8 * k, &m, 8);
            }
    }
    uint64_t mask(uint32_t idx, uint32_t k) const {
        uint64_t m = 0;
        std::memcpy(&m, b + ecm::kOffAmcMasks + idx * ecm::kPartStride + 8 * k, 8);
        return m;
    }
    // Head masks zero / everything else untouched?
    bool onlyHeadZeroed() const {
        for (uint32_t idx = 0; idx < ecm::kPartCount; ++idx)
            for (uint32_t k = 0; k < ecm::kPartVariants; ++k) {
                const bool zero = mask(idx, k) == 0;
                if (zero != ecm::isHeadPart(idx)) return false;
            }
        return true;
    }
    bool allMasksIntact() const {
        for (uint32_t idx = 0; idx < ecm::kPartCount; ++idx)
            for (uint32_t k = 0; k < ecm::kPartVariants; ++k)
                if (mask(idx, k) != 0x8000000000000001ull + idx) return false;
        return true;
    }
};

void testHeadHideGlue(const Pages& p) {
    std::printf("glue: head hiding (the avatar-fade hook's post-call, the FindJoint capture, a synthetic game image)\n");
    namespace t = edvr::explorercamtest;
    static Game g;
    static AmcBuf local, npc, firstPerson, wrongPose;
    static alignas(16) uint8_t skelA[0x100], skelB[0x100];   // the interfaces the "game" attaches to (only their addresses matter)
    Rig rig;
    auto begin = [&](int corruptName = -1, bool on = true, bool badFind = false) {
        t::reset();
        g.init();
        cimgInit(corruptName);
        rig = Rig();
        rig.f.hotkey = on ? "F5" : "";
        ExplorerCamTestTargets tt;
        tt.freeCamera = reinterpret_cast<uintptr_t>(p.freeG);
        tt.collision = reinterpret_cast<uintptr_t>(p.colG);
        tt.boxPush = reinterpret_cast<uintptr_t>(p.boxG);
        tt.cameraUi = reinterpret_cast<uintptr_t>(p.uiG);
        tt.controller = reinterpret_cast<uintptr_t>(p.ctlG);
        tt.avatarFade = reinterpret_cast<uintptr_t>(g_cimg) + ecm::kAvatarFadeRva;
        tt.findJoint = reinterpret_cast<uintptr_t>(g_cimg) + (badFind ? 0x3000000 : ecm::kFindJointRva);
        t::setTargets(tt);
        g.freeUpdate = reinterpret_cast<FnObj>(p.freeG);
        g.ctlUpdate = reinterpret_cast<FnObj>(p.ctlG);
        g.uiUpdate = reinterpret_cast<FnObj>(p.uiG);
        rig.boundary();
        rig.boundary();
    };
    auto fresh = [&]() {
        local.init(reinterpret_cast<uint64_t>(skelA), 3);
        npc.init(reinterpret_cast<uint64_t>(skelB) + 0x400, 3);
        firstPerson.init(reinterpret_cast<uint64_t>(skelA), 1);
        wrongPose.init(reinterpret_cast<uint64_t>(skelA), 3, 0xFFFFFFFFu, 0x38);
    };
    auto place = [&]() {
        g.setMode(3);
        g.free[0x473] = 1;
        g.ctlFrame(); rig.boundary(); g.ctlFrame(); rig.boundary();
        rig.boundary(true);
        g.ctlFrame();
        g.freeFrame(); g.freeFrame(); g.ctlFrame(); g.uiFrame(); g.uiFrame();
        rig.boundary();
    };

    // ---- the hooks are installed because Explorer Cam is on (no probe), and the names are checked ------------------------------------------------
    begin();
    fresh();
    check(t::stolenBytes(5) == 5 && t::stolenBytes(6) == 5 && g_cimg[ecm::kAvatarFadeRva] == 0xE9 && g_cimg[ecm::kFindJointRva] == 0xE9,
          "BOTH HOOKS ARE INSTALLED because Explorer Cam is on, with no probe: the avatar fade and FindJoint each stole 5 bytes");
    check(rig.cap.count("avatar-fade hook armed") == 1 && rig.cap.count("find-joint hook armed") == 1, "...each said once");
    check(t::partNamesState() == 1 && rig.cap.count("explorer cam head hide: the part-name table at EliteDangerous64.exe+0x5E9C7D0 holds the 54 names") == 1 &&
              has(rig.cap.nth("the part-name table at", 0), "Head Eyes Helmet SkullCap Hair Beard Teeth Hat EyeWear EVASuit_Helmet EVASuit_Eyewear EVASuit_Gear_Head"),
          "THE NAME TABLE IS CHECKED: the exe's 54 names match, one line says what is hidden");
    check(t::headHideOn() && !t::headHideDown(), "...head hiding is on");

    // ---- the capture: through the real FindJoint hook -----------------------------------------------------------------------------------------------
    const void* pov = g_cimg + ecm::kPovNameRva;
    imgInvoke(skelA, skelB, pov);
    check(explorerCamSkeleton(0).iface == reinterpret_cast<uint64_t>(skelA) && explorerCamSkeleton(0).index == 3 && explorerCamSkeleton(1).iface == reinterpret_cast<uint64_t>(skelB) &&
              explorerCamFindJointSeen() == 2,
          "THE CAPTURE (explorer_cam.cpp's own, no probe): one invocation that attached both avatars latched its third-person and first-person skeletons with their indexes");

    // ---- not placed: the census and the match line, nothing hidden ------------------------------------------------------------------------------------
    imgFade(local.b);
    check(local.allMasksIntact(), "NOT PLACED: the local AMC's masks are untouched (nothing is hidden outside a placement)");
    rig.cap.clear();
    rig.boundary();
    check(rig.cap.count("explorer cam head hide: census of the local third-person avatar AMC 0x") == 1 && has(rig.cap.nth("census of the local", 0), "10 of 54 parts have instances") &&
              rig.cap.count("census parts:") == 1 && has(rig.cap.nth("census parts:", 0), "[2 Eyes v=0123 st=1 fl=0x05 item=-1/102 m=0123 HIDE]") &&
              has(rig.cap.nth("census parts:", 0), "[24 EVASuit v=0 st=1 fl=0x14 item=-1/124 m=0 EXCLUDED]") && has(rig.cap.nth("census parts:", 0), "[49 FirstPersonSkeleton"),
          "THE CENSUS, once, before any placement: 10 parts have instances, their names, variants, state, flags and which masks are live");
    check(rig.cap.count("the local third-person avatar is AMC 0x") == 1 && has(rig.cap.nth("the local third-person avatar is AMC", 0), "The relation holds"),
          "...and ONE line says how the local AMC was found (the -0x30 relation holds)");
    imgFade(local.b);
    rig.cap.clear();
    rig.boundary();
    check(rig.cap.count("census") == 0 && rig.cap.count("the local third-person avatar is AMC") == 0, "...not repeated for the same AMC");

    // ---- only the local AMC is touched ----------------------------------------------------------------------------------------------------------------
    place();
    check(t::placedActivity() == reinterpret_cast<uint64_t>(g.free), "(placed)");
    local.refillMasks();
    imgFade(npc.b);
    imgFade(firstPerson.b);
    imgFade(wrongPose.b);
    check(npc.allMasksIntact() && firstPerson.allMasksIntact() && wrongPose.allMasksIntact() && t::hideCalls() == 0,
          "ONLY THE LOCAL AMC: another avatar (a different skeleton), the first-person one (mode 1) and one whose +0x268 is off by 8 keep every mask");
    imgFade(local.b);
    check(local.onlyHeadZeroed(), "PLACED: the local AMC's 12 head parts have all four variants' masks zero, every other part's masks are untouched");
    check(t::hideCalls() == 1 && t::hideZeroed() == 48 && t::amcLocal() == reinterpret_cast<uint64_t>(local.b), "...one hide call that zeroed 48 mask words (12 parts x 4 variants), and it knows the local AMC");
    local.refillMasks();   // the game's visibility job rewrote them
    imgFade(local.b);
    check(local.onlyHeadZeroed() && t::hideCalls() == 2 && t::hideZeroed() == 96, "...every frame: the game rewrites the masks, the post-call zeroes them again");
    // A mask that is already zero is not a write and not counted.
    imgFade(local.b);
    check(t::hideCalls() == 3 && t::hideZeroed() == 96, "...masks already zero add nothing to the zeroed count");
    // AFTER the original: the synthetic fade update writes part 0's mask during the call (flag set); the post-call must still leave it zero.
    g_cimg[ecm::kAvatarFadeRva + 0x40] = 1;
    local.refillMasks();
    imgFade(local.b);
    g_cimg[ecm::kAvatarFadeRva + 0x40] = 0;
    check(local.mask(0, 0) == 0 && local.onlyHeadZeroed(), "THE HIDE FOLLOWS THE ORIGINAL: a mask the fade update itself wrote during the call is zero when the hooked call returns");
    local.refillMasks();
    imgFade(local.b);
    check(local.onlyHeadZeroed(), "(back to the plain case)");

    // ---- the heartbeat ----------------------------------------------------------------------------------------------------------------------------------
    rig.advance(5200);
    g.freeFrame();
    rig.cap.clear();
    rig.boundary();
    const std::string beat = rig.cap.nth("explorer cam: heartbeat:", 0);
    check(has(beat, "head_hide=on") && has(beat, "hide_calls=5(+5)") && has(beat, "masks_zeroed=192(+192)") && has(beat, "local_matches=") && has(beat, "hide_faults=0"),
          "THE HEARTBEAT counts the hide calls and the zeroed masks per window");

    // ---- stops with the placement -----------------------------------------------------------------------------------------------------------------------
    rig.boundary(true);   // F5 leaves
    g.ctlFrame(); g.freeFrame(); g.uiFrame(); g.uiFrame(); g.ctlFrame(); g.ctlFrame();
    rig.boundary();
    rig.boundary();
    check(t::placedActivity() == 0, "(the placement ended)");
    local.refillMasks();
    imgFade(local.b);
    check(local.allMasksIntact() && t::hideCalls() == 5, "THE PLACEMENT ENDED: the masks are left alone from the next frame on (nothing to restore)");

    // ---- feature off ------------------------------------------------------------------------------------------------------------------------------------
    begin(-1, false);
    fresh();
    imgInvoke(skelA, skelB, g_cimg + ecm::kPovNameRva);
    imgFade(local.b);
    check(!t::headHideOn() && local.allMasksIntact() && t::stolenBytes(5) == 0 && t::stolenBytes(6) == 0, "FEATURE OFF: neither hook is installed, nothing is hidden, nothing is touched");

    // ---- the name table does not match -----------------------------------------------------------------------------------------------------------------
    begin(3);
    fresh();
    imgInvoke(skelA, skelB, g_cimg + ecm::kPovNameRva);
    place();
    imgFade(local.b);
    check(t::partNamesState() == 2 && !t::headHideOn() && local.allMasksIntact() && t::hideCalls() == 0 && rig.cap.count("the part-name table at EliteDangerous64.exe+0x5E9C7D0 differs") == 1 &&
              has(rig.cap.nth("the part-name table at EliteDangerous64.exe+0x5E9C7D0 differs", 0), "index 3, which is not \"Helmet\"") && has(rig.cap.nth("the part-name table at EliteDangerous64.exe+0x5E9C7D0 differs", 0), "nothing is hidden"),
          "A NAME THAT DIFFERS (index 3 reads \"Xead\"): one line names it, head hiding does not run, nothing is hidden even when placed");

    // ---- the FindJoint hook cannot install: head hiding does not run (and placement does) ----------------------------------------------------------------------
    begin(-1, true, true);
    fresh();
    t::setSkeleton(0, reinterpret_cast<uint64_t>(skelA), 3);   // even with a capture made by hand
    place();
    imgFade(local.b);
    check(t::stolenBytes(6) == 0 && !t::headHideOn() && local.allMasksIntact() && t::hideCalls() == 0 && t::placeActive() && rig.cap.count("find-joint hook stood down") == 1,
          "THE FINDJOINT HOOK STANDS DOWN (a wrong prologue): said once, head hiding does not run, nothing is hidden, and Explorer Cam's placement is unaffected");

    // ---- the -0x30 relation never holds -------------------------------------------------------------------------------------------------------------------
    begin();
    fresh();
    imgInvoke(skelA, skelB, g_cimg + ecm::kPovNameRva);
    place();
    for (int i = 0; i < 100; ++i) imgFade(wrongPose.b);
    rig.cap.clear();
    rig.boundary();
    check(rig.cap.count("NO AMC matched") == 0 && rig.cap.count("no AMC can be tested yet") == 0, "...100 fade calls without a match say nothing yet (the verdict waits for 300)");
    for (int i = 0; i < 205; ++i) imgFade(wrongPose.b);
    rig.cap.clear();
    rig.boundary();
    check(wrongPose.allMasksIntact() && t::hideCalls() == 0 && rig.cap.count("NO AMC matched the local skeleton in 305 avatar fade calls") == 1 &&
              has(rig.cap.nth("NO AMC matched", 0), "does not hold here, so nothing is hidden"),
          "THE RELATION NEVER MATCHES: after 300+ fade calls ONE line says so (with the numbers it saw) and nothing is hidden");
    rig.cap.clear();
    rig.boundary();
    check(rig.cap.count("NO AMC matched") == 0, "...said once");
    // Later it does match (the avatar is recreated): the match line, and hiding starts.
    imgFade(local.b);
    rig.cap.clear();
    rig.boundary();
    check(local.onlyHeadZeroed() && rig.cap.count("the local third-person avatar is AMC 0x") == 1, "...and when an AMC does match later, the match line is said and the hide starts");

    // ---- nothing captured at all ---------------------------------------------------------------------------------------------------------------------------
    begin();
    fresh();
    place();
    for (int i = 0; i < 100; ++i) imgFade(local.b);
    rig.cap.clear();
    rig.boundary();
    check(rig.cap.count("no AMC can be tested yet") == 0, "...100 fade calls with no capture say nothing yet");
    for (int i = 0; i < 205; ++i) imgFade(local.b);
    rig.cap.clear();
    rig.boundary();
    check(local.allMasksIntact() && rig.cap.count("no AMC can be tested yet: 305 avatar fade calls were seen but no local skeleton pair is latched") == 1,
          "NO CAPTURE AT ALL (Explorer Cam turned on after the avatars attached): ONE line says no AMC can be tested, nothing is hidden");

    // ---- guarded accesses: an AMC whose memory is unmapped -----------------------------------------------------------------------------------------------
    begin();
    fresh();
    imgInvoke(skelA, skelB, g_cimg + ecm::kPovNameRva);
    place();
    {
        // A block shaped like an AMC with its mask pages for the head parts taken away (PAGE_NOACCESS): the header reads, the writes fault.
        uint8_t* block = static_cast<uint8_t*>(VirtualAlloc(nullptr, 0x20000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        check(block != nullptr, "(a block for an AMC with unmapped masks)");
        if (block) {
            std::memcpy(block, local.b, 0x1E000);
            const uint64_t pp = reinterpret_cast<uint64_t>(local.params);
            std::memcpy(block + ecm::kOffAmcParams, &pp, 8);
            DWORD old = 0;
            VirtualProtect(block + 0xE000, 0x1000, PAGE_NOACCESS, &old);   // part 32's instances (0xE1C0) and masks (0xE7A0) both live on this page
            const uint64_t before = t::hideCalls();
            for (int i = 0; i < 6; ++i) imgFade(block);
            rig.cap.clear();
            rig.boundary();
            check(t::headHideDown() && t::hideCalls() == before && rig.cap.count("stood down for the session: 3 guarded accesses of the avatar component faulted") == 1,
                  "A FAULT IN THE AMC: counted, never a crash; the third stands head hiding down for the session with ONE line (the six calls made three faults)");
            check(!t::headHideOn() && t::faults() == 0, "...head hiding is off, and Explorer Cam's own fault budget is untouched (placement goes on)");
            local.refillMasks();
            imgFade(local.b);
            check(local.allMasksIntact(), "...and a good AMC is no longer written to");
            VirtualFree(block, 0, MEM_RELEASE);
        }
    }
    t::reset();
}

// What the fade code reads for an avatar's world origin: rcx = *(AMC+0x208) (an object whose first qword is a vtable), call [vtable+0x20] -> a matrix, origin at +0x30.
float g_xform[16];
int g_xformCalls = 0;
uint64_t __fastcall xformMatrix(uint64_t) {
    ++g_xformCalls;
    return reinterpret_cast<uint64_t>(g_xform);
}

void testSkeletonLatch(const Pages& p) {
    std::printf("glue: the FindJoint capture latches only the local humanoid (a site-1 then a site-2 attach in ONE invocation of 0x19B1240)\n");
    namespace t = edvr::explorercamtest;
    static Game g;
    static AmcBuf localAmc, npcAmc, localAmc2;
    static alignas(16) uint8_t npcA[0x100], npcB[0x100], npcC[0x100], skelL[0x100], skelF[0x100], skelL2[0x100], skelF2[0x100], skelQ[0x100];
    static alignas(16) uint64_t xformVtable[8], xformObj[2];
    Rig rig;
    auto begin = [&]() {
        t::reset();
        g.init();
        cimgInit();
        rig = Rig();
        ExplorerCamTestTargets tt;
        tt.freeCamera = reinterpret_cast<uintptr_t>(p.freeG);
        tt.collision = reinterpret_cast<uintptr_t>(p.colG);
        tt.boxPush = reinterpret_cast<uintptr_t>(p.boxG);
        tt.cameraUi = reinterpret_cast<uintptr_t>(p.uiG);
        tt.controller = reinterpret_cast<uintptr_t>(p.ctlG);
        tt.avatarFade = reinterpret_cast<uintptr_t>(g_cimg) + ecm::kAvatarFadeRva;
        tt.findJoint = reinterpret_cast<uintptr_t>(g_cimg) + ecm::kFindJointRva;
        t::setTargets(tt);
        g.freeUpdate = reinterpret_cast<FnObj>(p.freeG);
        g.ctlUpdate = reinterpret_cast<FnObj>(p.ctlG);
        g.uiUpdate = reinterpret_cast<FnObj>(p.uiG);
        rig.boundary();
        rig.boundary();
    };
    auto place = [&]() {
        g.setMode(3);
        g.free[0x473] = 1;
        g.ctlFrame(); rig.boundary(); g.ctlFrame(); rig.boundary();
        rig.boundary(true);
        g.ctlFrame();
        g.freeFrame(); g.freeFrame(); g.ctlFrame(); g.uiFrame(); g.uiFrame();
        rig.boundary();
    };
    begin();
    const void* pov = g_cimg + ecm::kPovNameRva;
    auto third = [&]() { return explorerCamSkeleton(0).iface; };
    auto first = [&]() { return explorerCamSkeleton(1).iface; };
    const uint64_t L = reinterpret_cast<uint64_t>(skelL), F = reinterpret_cast<uint64_t>(skelF);

    // ---- an NPC (no first-person avatar) never latches -------------------------------------------------------------------------------------------------
    imgAttach(0, npcA, pov);
    imgAttach(0, npcB, pov);
    imgAttach(0, npcC, pov);
    check(third() == 0 && first() == 0 && explorerCamSkeleton(0).captures == 3 && explorerCamSkeleton(0).latches == 0 && explorerCamFindJointSeen() == 3,
          "AN NPC-ONLY INVOCATION (site 1 alone, three humanoids in a row) never latches: three raw attaches, nothing latched");
    begin();   // a fresh session for the counts below

    // ---- a local invocation (site 1 then site 2, one frame) latches ----------------------------------------------------------------------------------
    imgAttach(0, npcA, pov);
    imgAttach(0, npcB, pov);
    imgInvoke(skelL, skelF, pov);
    check(third() == L && first() == F && explorerCamSkeleton(0).index == 3 && explorerCamSkeleton(1).index == 3 && explorerCamSkeleton(0).latches == 1 && explorerCamSkeleton(0).captures == 3 &&
              explorerCamSkeleton(1).captures == 1,
          "A LOCAL INVOCATION (site 1 then site 2 in one frame) latches: third-person = skelL, first-person = skelF, with their indexes; three raw site-1 attaches, one site-2");
    rig.cap.clear();
    rig.boundary();
    check(rig.cap.count("the local avatar's skeleton pair changed: third-person 0x") == 1 && has(rig.cap.nth("skeleton pair changed", 0), "(was 0x0)") &&
              has(rig.cap.nth("skeleton pair changed", 0), "latches 1, raw attaches seen: site 1 3, site 2 1"),
          "LOG: one line when the latched third-person skeleton changes (from 0), with the latch count and the raw attach counts");
    // A lone site-2 attach right after the pair: no site-1 attach is pending, so nothing latches.
    imgAttach(1, skelQ, pov);
    check(third() == L && first() == F && explorerCamSkeleton(0).latches == 1, "A SITE-2 ATTACH WITH NO SITE 1 PENDING never latches (the pair cleared the pending site 1)");

    // ---- the latch survives later NPC captures ----------------------------------------------------------------------------------------------------------
    imgAttach(0, npcA, pov);
    imgAttach(0, npcB, pov);
    imgAttach(0, npcC, pov);
    rig.cap.clear();
    rig.boundary();
    check(third() == L && first() == F && explorerCamSkeleton(0).latches == 1 && explorerCamSkeleton(0).captures == 6 && rig.cap.count("skeleton pair changed") == 0,
          "THE LATCH SURVIVES LATER NPC SITE-1 CAPTURES: three more raw attaches, the latched pair unchanged, no new line");

    // ---- one interface at both sites is not a pair of avatars ---------------------------------------------------------------------------------------------
    imgInvoke(skelQ, skelQ, pov);
    check(third() == L && first() == F && explorerCamSkeleton(0).latches == 1, "THE SAME INTERFACE AT BOTH SITES does not latch (a pair is two different skeletons)");

    // ---- site 1 from one humanoid, site 2 from a DIFFERENT FRAME: no latch ----------------------------------------------------------------------------------
    imgAttach(0, npcA, pov);
    imgDeep(3, 1, skelQ, pov);
    check(third() == L && first() == F && explorerCamSkeleton(0).latches == 1 && explorerCamSkeleton(1).captures == 4,
          "A SITE-2 ATTACH FROM A DIFFERENT STACK FRAME than the pending site 1 does not latch (raw count still goes up)");

    // ---- the AMC match uses the latch, never a raw capture ---------------------------------------------------------------------------------------------------
    place();
    npcAmc.init(reinterpret_cast<uint64_t>(npcC), 3);       // the LAST raw site-1 capture: what F6 mistook for the local avatar
    localAmc.init(L, 3);
    imgFade(npcAmc.b);
    check(npcAmc.allMasksIntact() && t::hideCalls() == 0, "THE F6 BUG: an AMC whose skeleton is the last RAW site-1 capture (an NPC) is NOT hidden");
    imgFade(localAmc.b);
    check(localAmc.onlyHeadZeroed() && t::hideCalls() == 1, "...the AMC of the latched local skeleton is");
    rig.cap.clear();
    rig.boundary();
    check(rig.cap.count("census of the local third-person avatar AMC 0x") == 1, "THE CENSUS runs once, for the local AMC only (the NPC's AMC was never matched)");
    localAmc.refillMasks();
    imgFade(localAmc.b);
    imgFade(npcAmc.b);
    rig.cap.clear();
    rig.boundary();
    check(rig.cap.count("census of the local third-person avatar") == 0, "...and not again while the local AMC stays the same");

    // ---- interleaving on two threads ------------------------------------------------------------------------------------------------------------------------
    {
        std::atomic<int> stage{0};
        const uint32_t latchesBefore = explorerCamSkeleton(0).latches;
        std::thread npcThread([&] {
            imgAttach(0, npcA, pov);   // a site-1 attach pending on THIS thread
            stage = 1;
            while (stage < 2) std::this_thread::yield();
            imgAttach(0, npcB, pov);
            stage = 3;
        });
        while (stage < 1) std::this_thread::yield();
        imgAttach(1, skelQ, pov);      // a lone site 2 on the main thread while ANOTHER thread has a site 1 pending: nothing pairs across threads
        check(third() == L && explorerCamSkeleton(0).latches == latchesBefore, "TWO THREADS: a site-2 attach on one thread never pairs with a site-1 attach pending on another");
        imgInvoke(skelL2, skelF2, pov);   // the local player's invocation on the main thread, between the NPC thread's two attaches
        stage = 2;
        while (stage < 3) std::this_thread::yield();
        npcThread.join();
        check(third() == reinterpret_cast<uint64_t>(skelL2) && first() == reinterpret_cast<uint64_t>(skelF2) && explorerCamSkeleton(0).latches == latchesBefore + 1,
              "...the local pair on the main thread latches while the other thread's NPC attaches go on around it, and the NPC thread's later site-1 attach does not disturb it");
    }
    rig.cap.clear();
    rig.boundary();
    check(rig.cap.count("the local avatar's skeleton pair changed: third-person 0x") == 1 && has(rig.cap.nth("skeleton pair changed", 0), "was 0x") && !has(rig.cap.nth("skeleton pair changed", 0), "(was 0x0)"),
          "LOG: a second line when the third-person skeleton changes again (a new local avatar), naming the one it replaced");
    // The old AMC no longer matches; the new local AMC gets its own census.
    localAmc2.init(reinterpret_cast<uint64_t>(skelL2), 3);
    localAmc.refillMasks();
    imgFade(localAmc.b);
    check(localAmc.allMasksIntact(), "AFTER THE LATCH MOVED: the previous local AMC is no longer matched");
    imgFade(localAmc2.b);
    rig.cap.clear();
    rig.boundary();
    check(localAmc2.onlyHeadZeroed() && rig.cap.count("census of the local third-person avatar AMC 0x") == 1, "...the new one is hidden and gets its own census (one census per LOCAL AMC change)");

    // ---- another thread's attach INSIDE the local invocation, between its site 1 and its site 2 -------------------------------------------------------------
    {
        static alignas(16) uint8_t skelL4[0x100], skelF4[0x100];
        static const void* s_pov = nullptr;
        s_pov = pov;
        g_cimgBetween = [] { std::thread worker([] { imgAttach(0, npcC, s_pov); }); worker.join(); };   // an NPC's site 1 on another thread, mid-invocation
        const uint32_t latchesBefore = explorerCamSkeleton(0).latches, rawBefore = explorerCamSkeleton(0).captures;
        imgInvoke(skelL4, skelF4, pov);
        g_cimgBetween = nullptr;
        check(third() == reinterpret_cast<uint64_t>(skelL4) && first() == reinterpret_cast<uint64_t>(skelF4) && explorerCamSkeleton(0).latches == latchesBefore + 1 &&
                  explorerCamSkeleton(0).captures == rawBefore + 2,
              "AN NPC ATTACH ON ANOTHER THREAD INSIDE THE LOCAL INVOCATION (between its site 1 and its site 2) does not break the latch: the pending site 1 is per thread");
    }

    // ---- the second witness: the AMC's world origin against the commander root ---------------------------------------------------------------------------
    begin();
    imgInvoke(skelL, skelF, pov);
    localAmc.init(L, 3);
    xformVtable[4] = reinterpret_cast<uint64_t>(&xformMatrix);
    xformObj[0] = reinterpret_cast<uint64_t>(xformVtable);
    localAmc.setTransform(xformObj);
    for (int i = 0; i < 16; ++i) g_xform[i] = (i % 5 == 0) ? 1.0f : 0.0f;
    t::setWitnessInterval(0);
    g_xformCalls = 0;
    for (int i = 0; i < 6; ++i) imgFade(localAmc.b);
    rig.cap.clear();
    rig.boundary();
    check(rig.cap.count("witness (log only, not a gate): local AMC 0x") == 2 && has(rig.cap.nth("witness (log only", 0), "no commander root to compare with") && g_xformCalls == 6,
          "THE WITNESS, on foot (no free camera): the AMC's origin is read through its transform handle (6 calls), and only the first TWO 'no commander root' lines are logged");
    place();
    // A valid frame in the activity's world pose; the placement has written the local pose.
    {
        float* world = reinterpret_cast<float*>(g.free + 0x70);
        for (int i = 0; i < 16; ++i) world[i] = (i % 5 == 0) ? 1.0f : 0.0f;
        world[12] = 10.0f; world[13] = 0.0f; world[14] = 20.0f;
        float localPose[16], worldPose[16];
        std::memcpy(localPose, g.free + ecm::kOffLocalPose, 64);
        std::memcpy(worldPose, g.free + 0x70, 64);
        const ecm::CommanderFrame cf = ecm::commanderFrame(localPose, worldPose);
        check(cf.valid, "(the activity's two poses make a commander frame)");
        g_xform[12] = cf.root[0] + 3.0f; g_xform[13] = cf.root[1]; g_xform[14] = cf.root[2] + 4.0f;
        t::setWitnessInterval(0);
        rig.cap.clear();
        imgFade(localAmc.b);
        rig.boundary();
        const std::string w = rig.cap.nth("witness (log only", 0);
        check(has(w, "is 5.000 m from the commander root") && has(w, "about 0 means the frames agree") && has(w, "the free camera is at (10.000,0.000,20.000)") && has(w, "m from the AMC"),
              "...in the free camera it prints the distance from the commander root (an AMC 3 m right and 4 m forward of the root is 5.000 m away) and the camera's own origin");
        g_xform[12] = cf.root[0]; g_xform[13] = cf.root[1]; g_xform[14] = cf.root[2];
        rig.cap.clear();
        imgFade(localAmc.b);
        rig.boundary();
        check(has(rig.cap.nth("witness (log only", 0), "is 0.000 m from the commander root"), "...and 0.000 m when the AMC is at the root (the frames agree)");
    }
    // An AMC whose transform handle is not readable: the line says so, nothing faults.
    localAmc.setTransform(nullptr);
    const uint64_t callsBefore = static_cast<uint64_t>(g_xformCalls);
    rig.cap.clear();
    imgFade(localAmc.b);
    rig.boundary();
    check(has(rig.cap.nth("witness (log only", 0), "world origin could not be read") && static_cast<uint64_t>(g_xformCalls) == callsBefore && !t::headHideDown(),
          "AN UNREADABLE TRANSFORM HANDLE (+0x208 is 0): 'world origin could not be read', no call is made, head hiding is unaffected");
    // A transform handle whose vtable slot faults: counted as a fault of the witness only; three of them stop the witness, head hiding is untouched.
    {
        static alignas(16) uint64_t badVtable[8], badObj[2];
        badVtable[4] = 0x10;   // a "function" that is not mapped: the call faults under SEH (implausible pointers are refused before the call, so use a plausible unmapped one)
        badVtable[4] = reinterpret_cast<uint64_t>(g_cimg) + 0x2000000;   // inside the image reservation but never committed: executing it faults
        badObj[0] = reinterpret_cast<uint64_t>(badVtable);
        localAmc.setTransform(badObj);
        rig.cap.clear();
        for (int i = 0; i < 6; ++i) imgFade(localAmc.b);
        rig.boundary();
        check(rig.cap.count("witness (log only") == 3 && has(rig.cap.nth("witness (log only", 0), "world origin could not be read") && !t::headHideDown() && t::faults() == 0,
              "A FAULT IN THE TRANSFORM CALL: each is caught (SEH), the line says the origin could not be read, no crash; head hiding and Explorer Cam's own fault budget are untouched");
        const std::string after = rig.cap.nth("witness (log only", 2);
        rig.cap.clear();
        imgFade(localAmc.b);
        rig.boundary();
        check(!after.empty() && rig.cap.count("witness (log only") == 0, "...and after three faults the witness stops calling");
    }
    t::reset();
}

// ================================ Phase 3: the head-joint eye, the trims, the smoothing, the isolation tables ================================

// A bind pose with the local transforms and parents the pose walk reads: every joint under the root at the origin with an identity rotation (x,y,z,w = 0,0,0,1).
struct FlatPose {
    float locals[64 * 8];
    uint16_t parents[64];
    FlatPose() {
        std::memset(locals, 0, sizeof(locals));
        for (int j = 0; j < 64; ++j) {
            parents[j] = 0;
            locals[j * 8 + 7] = 1.0f;
        }
        parents[0] = 0xFFFF;
    }
    void pos(int j, float x, float y, float z) {
        locals[j * 8 + 0] = x;
        locals[j * 8 + 1] = y;
        locals[j * 8 + 2] = z;
    }
};
// The head matrix +0x58 hands back: rows 0-2 the joint's axes in the avatar's model space, row 3 its position.
void headMatrix(float m[16], const float rows[9], float x, float y, float z) {
    std::memset(m, 0, 64);
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) m[i * 4 + j] = rows[i * 3 + j];
    m[12] = x;
    m[13] = y;
    m[14] = z;
    m[15] = 1.0f;
}
const float kIdentityRows[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};

void testFollowPure() {
    std::printf("the eye from the head joint: the rest offset, the live eye, the trims, the smoothing, the isolation tables (pure)\n");
    static_assert(ecm::kZoomDofRva == 0x1078990 && ecm::kZoomDofPrologueBytes == 16 && ecm::kZoomDofPrologue[0] == 0x40 && ecm::kZoomDofPrologue[1] == 0x53,
                  "the zoom/DOF update is +0x1078990 and begins `push rbx` (REX 40 53)");
    check(stolenOf(ecm::kZoomDofPrologue, ecm::kZoomDofPrologueBytes) == 6, "THE ZOOM/DOF PROLOGUE: CodeHook steals 6 bytes (push rbx; sub rsp,60h), no rip-relative displacement among them");

    // ---- the rest offset on a skeleton with a turned joint (hand-worked) ---------------------------------------------------------------------------------------
    // j0 root at (1,2,3); j1 under it at (0,1,0), turned +90 degrees about Y (rows (0,0,-1) (0,1,0) (1,0,0)); the povCamera joint j3 at (0.1,0,0) and the head j12 at
    // (0,0.05,0.05), both under j1. Model space (row vectors): pov (1,3,2.9), head (1.05,3.05,3). The head's rest rotation is j1's.
    FlatPose sk;
    sk.pos(0, 1.0f, 2.0f, 3.0f);
    sk.pos(1, 0.0f, 1.0f, 0.0f);
    sk.locals[1 * 8 + 5] = 0.70710678f;
    sk.locals[1 * 8 + 7] = 0.70710678f;
    sk.parents[1] = 0;
    sk.parents[3] = 1;
    sk.parents[12] = 1;
    sk.pos(3, 0.1f, 0.0f, 0.0f);
    sk.pos(12, 0.0f, 0.05f, 0.05f);
    ecm::RestOffset r;
    check(ecm::deriveRestOffset(sk.locals, sk.parents, 64, 12, 3, &r) == ecm::RestWhy::Ok, "THE REST OFFSET derives on a skeleton with a turned joint");
    check(closeTo(r.headRest[0], 1.05f) && closeTo(r.headRest[1], 3.05f) && closeTo(r.headRest[2], 3.0f) && closeTo(r.povRest[0], 1.0f) && closeTo(r.povRest[1], 3.0f) &&
              closeTo(r.povRest[2], 2.9f) && r.headDepth == 2 && r.povDepth == 2,
          "...the rest head is at (1.05,3.05,3) and the rest povCamera at (1,3,2.9) in model space, two ancestors each");
    check(closeTo(r.delta[0], -0.05f) && closeTo(r.delta[1], -0.05f) && closeTo(r.delta[2], -0.1f), "...the offset in model space is pov - head = (-0.05,-0.05,-0.1)");
    const float restRows[9] = {0, 0, -1, 0, 1, 0, 1, 0, 0};
    bool rowsOk = true;
    for (int i = 0; i < 9; ++i) rowsOk = rowsOk && closeTo(r.headRot[i], restRows[i], 1e-5f);
    check(rowsOk, "...the head's rest rotation rows are j1's turn: (0,0,-1) (0,1,0) (1,0,0)");
    check(closeTo(r.local[0], 0.1f) && closeTo(r.local[1], -0.05f) && closeTo(r.local[2], -0.05f),
          "...and the offset in the head joint's OWN axes is the dot of the model offset with each row: (0.1,-0.05,-0.05)");
    float m[16], e[3];
    headMatrix(m, kIdentityRows, 0.2f, 1.5f, 0.1f);
    ecm::headEyeModel(m, r.local, e);
    check(closeTo(e[0], 0.3f) && closeTo(e[1], 1.45f) && closeTo(e[2], 0.05f), "A LIVE HEAD WITH AN IDENTITY ROTATION: eye = position + (0.1,-0.05,-0.05) = (0.3,1.45,0.05)");
    headMatrix(m, restRows, 0.2f, 1.5f, 0.1f);
    ecm::headEyeModel(m, r.local, e);
    check(closeTo(e[0], 0.15f) && closeTo(e[1], 1.45f) && closeTo(e[2], 0.0f), "...a head still in its rest rotation: eye = position + the model offset (-0.05,-0.05,-0.1) = (0.15,1.45,0.0)");
    check(closeTo(ecm::rotationDiff(m, r.headRot), 0.0f), "...and its rotation differs from the rest rotation by 0");
    const float turned[9] = {-1, 0, 0, 0, 1, 0, 0, 0, -1};   // yawed 180 degrees about Y
    headMatrix(m, turned, 0.2f, 1.5f, 0.1f);
    ecm::headEyeModel(m, r.local, e);
    check(closeTo(e[0], 0.1f) && closeTo(e[1], 1.45f) && closeTo(e[2], 0.15f), "...a head turned 180 degrees: the offset turns with it, (-0.1,-0.05,+0.05) from the position = (0.1,1.45,0.15)");
    check(closeTo(ecm::rotationDiff(m, r.headRot), 1.0f), "...and its rotation differs from the rest rotation by 1.0 at most (the convention self-check the first live line prints)");

    // ---- two turns that do not commute: the order of the composition and the convention agree with the position walk ---------------------------------------------------------
    {
        // j5 turned 90 degrees about Y under j6 (a root) turned 90 degrees about X; j7 under j5 at (1,0,0).
        FlatPose two;
        two.parents[5] = 6;
        two.parents[6] = 0xFFFF;
        two.parents[7] = 5;
        two.locals[5 * 8 + 5] = 0.70710678f;
        two.locals[5 * 8 + 7] = 0.70710678f;
        two.locals[6 * 8 + 4] = 0.70710678f;
        two.locals[6 * 8 + 7] = 0.70710678f;
        two.pos(7, 1.0f, 0.0f, 0.0f);
        float rot[9], pos[3];
        uint32_t depth = 0;
        const float want[9] = {0, 1, 0, 0, 0, 1, 1, 0, 0};   // R(j5) x R(j6), own turn first
        bool same = ecm::walkJointRotationToModel(two.locals, two.parents, 64, 5, rot);
        for (int i = 0; i < 9; ++i) same = same && closeTo(rot[i], want[i], 1e-5f);
        check(same, "THE ROTATION COMPOSES OWN TURN FIRST, THEN THE PARENT'S: j5 (Y 90) under j6 (X 90) gives rows (0,1,0) (0,0,1) (1,0,0) -- not the reverse order");
        check(ecm::walkJointToModel(two.locals, two.parents, 64, 7, pos, &depth) && closeTo(pos[0], 0.0f) && closeTo(pos[1], 1.0f) && closeTo(pos[2], 0.0f) && depth == 2 &&
                  closeTo(pos[0], 1.0f * rot[0], 1e-5f) && closeTo(pos[1], 1.0f * rot[1], 1e-5f) && closeTo(pos[2], 1.0f * rot[2], 1e-5f),
              "...and it is the same convention as the position walk: a child at (1,0,0) lands on the first row of that rotation, (0,1,0)");
        const float zeros[16] = {};
        check(closeTo(ecm::rotationDiff(zeros, kIdentityRows), 1.0f), "...rotationDiff is absolute: a rotation every element of which is BELOW the rest's still differs by 1.0");
    }

    // ---- the F7 skeleton: head (0,1.675,0.003) and povCamera (0,1.713,0.115) under the root, identity rotations --------------------------------------------------
    FlatPose real;
    real.pos(12, 0.0f, 1.675f, 0.003f);
    real.pos(3, 0.0f, 1.713f, 0.115f);
    ecm::RestOffset f7;
    check(ecm::deriveRestOffset(real.locals, real.parents, 64, 12, 3, &f7) == ecm::RestWhy::Ok && closeTo(f7.local[0], 0.0f) && closeTo(f7.local[1], 0.038f, 1e-5f) &&
              closeTo(f7.local[2], 0.112f, 1e-5f),
          "THE F7 SKELETON: the eye sits (0, 0.038, 0.112) from the head in its own axes");
    struct Stance {
        const char* name;
        float head[3];
        float want[3];   // right, up, forward
    };
    const Stance stances[] = {{"standing", {0.00f, 1.66f, 0.03f}, {0.00f, 1.698f, 0.142f}},
                              {"crouched", {0.09f, 0.94f, 0.19f}, {0.09f, 0.978f, 0.302f}},
                              {"weapon out", {0.10f, 1.38f, 0.19f}, {0.10f, 1.418f, 0.302f}}};
    for (const Stance& st : stances) {
        headMatrix(m, kIdentityRows, st.head[0], st.head[1], st.head[2]);
        ecm::headEyeModel(m, f7.local, e);
        const ecm::Eye eye = ecm::eyeFromModelPoint(e, ecm::Trim());
        char what[200];
        std::snprintf(what, sizeof(what), "F7's %s head (%.2f,%.2f,%.2f) gives the eye right %.3f, up %.3f, forward %.3f", st.name, st.head[0], st.head[1], st.head[2], st.want[0],
                      st.want[1], st.want[2]);
        check(closeTo(eye.right, st.want[0], 2e-5f) && closeTo(eye.up, st.want[1], 2e-5f) && closeTo(eye.forward, st.want[2], 2e-5f), what);
    }
    ecm::Trim trim;
    trim.right = 0.05f;
    trim.up = -0.02f;
    trim.forward = 0.03f;
    headMatrix(m, kIdentityRows, 0.0f, 1.66f, 0.03f);
    ecm::headEyeModel(m, f7.local, e);
    const ecm::Eye trimmed = ecm::eyeFromModelPoint(e, trim);
    check(closeTo(trimmed.right, 0.05f, 2e-5f) && closeTo(trimmed.up, 1.678f, 2e-5f) && closeTo(trimmed.forward, 0.172f, 2e-5f), "THE TRIMS are added after the joint, in the commander's right, up, forward");
    check(ecm::kTrimLimit == 0.5f && ecm::clampTrim(0.9f) == 0.5f && ecm::clampTrim(-0.9f) == -0.5f && ecm::clampTrim(0.5f) == 0.5f && ecm::clampTrim(-0.5f) == -0.5f &&
              ecm::clampTrim(0.1f) == 0.1f && ecm::clampTrim(0.3f) == 0.3f && ecm::clampTrim(std::nanf("")) == 0.0f,
          "...held to +-0.5 m (widened from 0.3 when they became user settings), and a NaN reads as 0");
    check(ecm::kTrimUpDefault == 0.15f && ecm::kTrimForwardDefault == -0.08f && ecm::kTrimRightDefault == 0.0f && ecm::kSmoothingMsDefault == 0,
          "THE SHIPPED DEFAULTS are Sean's own tuning: up 0.15, forward -0.08, right 0.0, smoothing 0 (exact follow)");
    check(ecm::clampTrim(ecm::kTrimUpDefault) == ecm::kTrimUpDefault && ecm::clampTrim(ecm::kTrimForwardDefault) == ecm::kTrimForwardDefault &&
              ecm::clampTrim(ecm::kTrimRightDefault) == ecm::kTrimRightDefault && ecm::clampSmoothingMs(static_cast<float>(ecm::kSmoothingMsDefault)) == 0.0f,
          "...and every one of them is inside its clamp (a default the clamp changed would not be the default)");

    // ---- the rest offset refuses what it cannot trust ------------------------------------------------------------------------------------------------------------
    ecm::RestOffset junk;
    check(ecm::deriveRestOffset(sk.locals, sk.parents, 0, 12, 3, &junk) == ecm::RestWhy::NoJoints, "NO JOINTS: refused");
    check(ecm::deriveRestOffset(sk.locals, sk.parents, 513, 12, 3, &junk) == ecm::RestWhy::TooManyJoints, "513 JOINTS (the walk's buffer holds 512): refused");
    check(ecm::deriveRestOffset(sk.locals, sk.parents, 64, 0xFFFF, 3, &junk) == ecm::RestWhy::HeadMissing && ecm::deriveRestOffset(sk.locals, sk.parents, 64, 64, 3, &junk) == ecm::RestWhy::HeadMissing,
          "THE HEAD NOT FOUND (0xFFFF) or past the joint count: refused");
    check(ecm::deriveRestOffset(sk.locals, sk.parents, 64, 12, 0xFFFF, &junk) == ecm::RestWhy::PovMissing && ecm::deriveRestOffset(sk.locals, sk.parents, 64, 12, 64, &junk) == ecm::RestWhy::PovMissing,
          "THE POVCAMERA INDEX missing or past the joint count: refused");
    {
        FlatPose c = sk;
        c.parents[12] = 1;
        c.parents[1] = 12;   // 12 -> 1 -> 12
        check(ecm::deriveRestOffset(c.locals, c.parents, 64, 12, 3, &junk) == ecm::RestWhy::HeadWalk, "A PARENT CYCLE above the head: refused, no hang");
        FlatPose d = sk;
        d.parents[3] = 200;
        check(ecm::deriveRestOffset(d.locals, d.parents, 64, 12, 3, &junk) == ecm::RestWhy::PovWalk, "A PARENT OUT OF RANGE above the povCamera joint: refused");
        FlatPose n = sk;
        n.locals[1 * 8 + 7] = 2.0f;   // a quaternion that is not a rotation
        check(ecm::deriveRestOffset(n.locals, n.parents, 64, 12, 3, &junk) == ecm::RestWhy::NotRotation, "A QUATERNION THAT IS NOT UNIT LENGTH: the rest rotation is refused");
        FlatPose nanPose = sk;
        nanPose.locals[12 * 8 + 4] = std::nanf("");
        check(ecm::deriveRestOffset(nanPose.locals, nanPose.parents, 64, 12, 3, &junk) == ecm::RestWhy::NotRotation, "A NaN IN THE HEAD'S ROTATION: refused");
        FlatPose distant = real;
        distant.pos(3, 5.0f, 1.713f, 0.115f);
        check(ecm::deriveRestOffset(distant.locals, distant.parents, 64, 12, 3, &junk) == ecm::RestWhy::ImplausibleOffset, "A POVCAMERA JOINT 5 m FROM THE HEAD: refused as implausible");
    }

    // ---- the live matrix must look like a head ------------------------------------------------------------------------------------------------------------------
    headMatrix(m, kIdentityRows, 0.0f, 1.66f, 0.03f);
    check(ecm::headMatrixPlausible(m), "A PLAUSIBLE HEAD MATRIX: finite, unit rows, inside the avatar's surroundings");
    {
        float b[16];
        std::memcpy(b, m, 64);
        b[13] = 5.0f;
        check(!ecm::headMatrixPlausible(b), "...a head 5 m up is not");
        std::memcpy(b, m, 64);
        b[13] = 0.05f;
        check(!ecm::headMatrixPlausible(b), "...nor one 5 cm off the ground");
        std::memcpy(b, m, 64);
        b[12] = 3.0f;
        check(!ecm::headMatrixPlausible(b), "...nor one 3 m to the side");
        std::memcpy(b, m, 64);
        b[14] = std::nanf("");
        check(!ecm::headMatrixPlausible(b), "...nor a NaN");
        std::memcpy(b, m, 64);
        b[3] = std::nanf("");
        check(!ecm::headMatrixPlausible(b), "...nor a NaN in the fourth column, which nothing else reads");
        std::memcpy(b, m, 64);
        b[5] = 0.0f;
        b[0] = 0.0f;
        check(!ecm::headMatrixPlausible(b), "...nor rows that are not near unit length");
        std::memcpy(b, m, 64);
        b[3] = 1.0e9f;
        check(!ecm::headMatrixPlausible(b), "...nor a huge number anywhere in the sixteen");
    }

    // ---- the smoothing: 0 is exact ----------------------------------------------------------------------------------------------------------------------------
    ecm::Eye a;
    a.up = 1.698f;
    a.forward = 0.142f;
    a.right = 0.0f;
    ecm::Eye b2;
    b2.up = 0.978f;
    b2.forward = 0.302f;
    b2.right = 0.09f;
    ecm::EyeSmoother sm;
    ecm::Eye out = ecm::smoothEye(sm, a, 16.0, 0.0f);
    check(std::memcmp(&out, &a, sizeof(ecm::Eye)) == 0 && !sm.have, "SMOOTHING 0: the target comes back BIT FOR BIT and no state is kept");
    out = ecm::smoothEye(sm, b2, 16.0, 0.0f);
    check(std::memcmp(&out, &b2, sizeof(ecm::Eye)) == 0, "...also after another target: the unsmoothed eye");
    out = ecm::smoothEye(sm, a, 0.0, 100.0f);
    check(std::memcmp(&out, &a, sizeof(ecm::Eye)) == 0 && sm.have, "SMOOTHING 100 ms: the first eye snaps to the target");
    out = ecm::smoothEye(sm, b2, 0.0, 100.0f);
    check(std::memcmp(&out, &a, sizeof(ecm::Eye)) == 0, "...no time passed: the eye stays");
    out = ecm::smoothEye(sm, b2, 100.0, 100.0f);
    {
        const float k = static_cast<float>(1.0 - std::exp(-1.0));
        check(closeTo(out.up, 1.698f + (0.978f - 1.698f) * k, 1e-5f) && closeTo(out.right, 0.09f * k, 1e-5f) && closeTo(out.forward, 0.142f + (0.302f - 0.142f) * k, 1e-5f),
              "...one time constant later it has covered 1 - 1/e (63.2%) of the way");
    }
    out = ecm::smoothEye(sm, b2, 60000.0, 100.0f);
    check(closeTo(out.up, 0.978f, 1e-3f), "...a long pause lands on the target");
    ecm::smoothEye(sm, a, 16.0, 0.0f);          // forget the history
    out = ecm::smoothEye(sm, a, 0.0, 1000.0f);   // ...and start again from  with a 1 s time constant
    out = ecm::smoothEye(sm, b2, 60000.0, 1000.0f);
    check(closeTo(out.up, 0.978f, 1e-3f), "...also with a long time constant (1 s) and a 60 s pause: no cap on the step");
    out = ecm::smoothEye(sm, a, 16.0, 0.0f);
    check(std::memcmp(&out, &a, sizeof(ecm::Eye)) == 0 && !sm.have, "...and turning the smoothing back to 0 gives the unsmoothed eye at once, bit for bit");
    check(ecm::clampSmoothingMs(-5.0f) == 0.0f && ecm::clampSmoothingMs(std::nanf("")) == 0.0f && ecm::clampSmoothingMs(250.0f) == 250.0f && ecm::clampSmoothingMs(5000.0f) == 1000.0f,
          "THE SMOOTHING KEY is held to 0..1000 ms and a NaN or a negative reads as 0");

    // ---- the isolation tables, against the static notes -------------------------------------------------------------------------------------------------------
    bool tablesOk = true;
    for (int h = 0; h < ecm::kIsoHolderCount; ++h) {
        tablesOk = tablesOk && static_cast<int>(ecm::kIsoCounts[h]) == kExpCount[h];
        for (int i = 0; i < kExpCount[h] && tablesOk; ++i) {
            const ecm::IsoField& f = ecm::isoFields(h)[i];
            const char kind = f.kind == ecm::IsoKind::Axis ? 'A' : f.kind == ecm::IsoKind::Pressed ? 'P' : 'H';
            tablesOk = f.handle == kExpTable[h][i].handle && kind == kExpTable[h][i].kind;
        }
    }
    check(tablesOk, "THE ISOLATION TABLES match the static notes handle by handle and field by field (free camera 11, controller 15, camera UI 1, zoom/DOF 7)");
    check(ecm::isoFieldOffset(ecm::IsoKind::Axis) == 0x18 && ecm::isoFieldOffset(ecm::IsoKind::Pressed) == 0x1C && ecm::isoFieldOffset(ecm::IsoKind::Held) == 0x24 &&
              ecm::isoFieldWidth(ecm::IsoKind::Axis) == 4 && ecm::isoFieldWidth(ecm::IsoKind::Pressed) == 4 && ecm::isoFieldWidth(ecm::IsoKind::Held) == 1,
          "...an axis is the float at +0x18, a press the int at +0x1C, a hold the byte at +0x24");

    // ---- the lines ---------------------------------------------------------------------------------------------------------------------------------------------
    char line[ecm::kLineBytes];
    ecm::formatIsolation(line, sizeof(line), true, "F5");
    check(has(line, "explorer cam: camera suite isolated: 34 actions blocked across 4 holders (free camera 11, camera controller 15, camera UI 1, zoom/DOF 7)") && has(line, "until you press F5 again") &&
              has(line, "EDVR's own presses") && has(line, "Walking and turning are not touched") && !has(line, "not in place") && std::strlen(line) < ecm::kLineBytes - 1,
          "THE SESSION LINE: 'camera suite isolated: 34 actions blocked across 4 holders', the own presses, walking untouched");
    ecm::formatIsolation(line, sizeof(line), false, "F6");
    check(has(line, "27 actions blocked across 3 holders (free camera 11, camera controller 15, camera UI 1)") && has(line, "until you press F6 again") && has(line, "zoom/DOF hook is not in place"),
          "...with the zoom/DOF hook down: 27 actions across 3 holders, and it says why the zoom keys are not blocked");

    ecm::FollowNote n;
    n.kind = static_cast<uint32_t>(ecm::FollowNoteKind::Rest);
    n.iface = 0x23A812307C8ull;
    n.hkind = static_cast<uint32_t>(ecm::HeadKind::Runtime);
    n.joints = 185;
    n.headIdx = 44;
    n.povIdx = 15;
    n.headDepth = 9;
    n.povDepth = 10;
    std::memcpy(n.headRest, f7.headRest, 12);
    std::memcpy(n.povRest, f7.povRest, 12);
    std::memcpy(n.delta, f7.delta, 12);
    std::memcpy(n.local, f7.local, 12);
    ecm::formatFollowNote(line, sizeof(line), n);
    check(has(line, "explorer cam: head follow: head joint of skeleton 0x23A812307C8 (RR (RuntimeRigComponent), 185 joints) verified: head idx 44 rest (0.000,1.675,0.003) depth 9") &&
              has(line, "povCamera idx 15 rest (0.000,1.713,0.115) depth 10") && has(line, "(0.000,0.038,0.112) from the head in model space, which is (0.000,0.038,0.112) in the head joint's own axes") &&
              has(line, "+ the trims"),
          "THE REST LINE names the skeleton, the joints, both rest positions and the offset in model space and in the head's own axes");
    n.kind = static_cast<uint32_t>(ecm::FollowNoteKind::RestFailed);
    n.why = static_cast<uint32_t>(ecm::RestWhy::HeadMissing);
    ecm::formatFollowNote(line, sizeof(line), n);
    check(has(line, "cannot be used: FindJoint(\"def_c_head_joint\") found no head joint") && has(line, "the fixed eye keys place the view for this skeleton"), "THE FAILURE LINE says why the head joint cannot be used and that the fixed keys serve");
    n.kind = static_cast<uint32_t>(ecm::FollowNoteKind::FirstLive);
    n.live[0] = 0.0f;
    n.live[1] = 1.66f;
    n.live[2] = 0.03f;
    n.eye[0] = 1.698f;
    n.eye[1] = 0.142f;
    n.eye[2] = 0.0f;
    n.rotDiff = 0.012f;
    ecm::formatFollowNote(line, sizeof(line), n);
    check(has(line, "first live head joint (+0x58)") && has(line, "head (0.000,1.660,0.030)") && has(line, "at most 0.012 per element") && has(line, "up=1.698 forward=0.142 right=0.000"),
          "THE FIRST LIVE LINE gives the head, the rotation difference from rest (the convention check) and the eye");
    n.kind = static_cast<uint32_t>(ecm::FollowNoteKind::Switched);
    n.why = static_cast<uint32_t>(ecm::FixedWhy::None);
    ecm::formatFollowNote(line, sizeof(line), n);
    check(has(line, "the eye now comes from the head joint of skeleton 0x23A812307C8"), "A SWITCH to the head joint says so");
    n.why = static_cast<uint32_t>(ecm::FixedWhy::NothingLatched);
    ecm::formatFollowNote(line, sizeof(line), n);
    check(has(line, "the eye now comes from the FIXED keys") && has(line, "no local skeleton pair is latched yet"), "...a switch to the fixed keys says why");
    n.kind = static_cast<uint32_t>(ecm::FollowNoteKind::StoodDown);
    n.why = 0;
    n.faults = 8;
    ecm::formatFollowNote(line, sizeof(line), n);
    check(has(line, "stood down for the session: 8 reads or calls of the head joint faulted") && has(line, "the placement goes on"), "THE FAULT STAND-DOWN names the 8 faults and says the placement goes on");
    n.why = 1;
    n.a = 0x48;
    n.b = 0x3000000;
    n.c = 0x43F7180;
    ecm::formatFollowNote(line, sizeof(line), n);
    check(has(line, "vtable slot +0x48 of skeleton 0x23A812307C8 holds 0x3000000, not the build-332841 function 0x43F7180"), "...a vtable slot that differs names the slot, what it holds and what it should");

    ecm::FollowBeatIn fb;
    fb.windowSeconds = 5.0;
    fb.source = "head-joint";
    fb.lastEye.up = 1.698f;
    fb.lastEye.forward = 0.142f;
    fb.headUpdates = 450;
    fb.headWindow = 450;
    fb.t58n = 450;
    fb.t58min = 1;
    fb.t58max = 3;
    fb.t58allMax = 9;
    fb.blocked[0] = 11;
    fb.blockedWindow[0] = 11;
    fb.blocked[3] = 7;
    fb.blockedWindow[3] = 7;
    fb.zoomArmed = true;
    fb.zoomCalls = 30;
    fb.zoomCallsWindow = 30;
    fb.fadePhase = "black";
    fb.fadeKind = "entering";
    fb.fadeAlpha = 1.0f;
    ecm::formatFollowBeat(line, sizeof(line), fb);
    check(has(line, "comfort_fade(phase=black kind=entering alpha=1.000)"), "THE SECOND HEARTBEAT LINE ends with the comfort fade's phase, kind and level");
    check(has(line, "explorer cam: heartbeat (follow, isolation):") && !has(line, "explorer cam: heartbeat:") && has(line, "eye_source=head-joint") && has(line, "last_eye(up=1.698 forward=0.142 right=0.000)") &&
              has(line, "head_joint_updates=450(+450)") && has(line, "read58_us(n/min/max/session_max)=450/1/3/9") && has(line, "free_camera=11(+11)") && has(line, "zoom=7(+7)") &&
              has(line, "zoom_hook=armed zoom_hook_calls=30(+30)") && has(line, "isolation=on"),
          "THE SECOND HEARTBEAT LINE names the eye source, the last eye, the +0x58 call's n/min/max/session max in microseconds, the blocked presses per holder and the zoom hook's calls");
}

// ================================ Phase 3, glue: the head-joint eye end to end ================================

// A synthetic skeleton interface in the synthetic game image: the RR (or AO) vtable at the build's address, a pose with 64 joints, the F7 bind pose (every joint under
// the root; the head j12 at (0,1.675,0.003), the povCamera joint j3 at (0,1.713,0.115)).
struct CamSkel {
    alignas(16) uint8_t iface[0x400];
    alignas(16) uint8_t pose[0x80];
    alignas(16) float locals[64 * 8];
    alignas(16) uint16_t parents[64];
    void init(int kind = 0, uint16_t joints = 64) {
        std::memset(this, 0, sizeof(*this));
        const uint64_t vt = reinterpret_cast<uint64_t>(g_cimg) + (kind == 0 ? ecm::kRrVtableRva : ecm::kAoVtableRva);
        std::memcpy(iface, &vt, 8);
        const uint64_t pp = reinterpret_cast<uint64_t>(pose), lp = reinterpret_cast<uint64_t>(locals), pa = reinterpret_cast<uint64_t>(parents);
        std::memcpy(iface + 0x20, &pp, 8);
        std::memcpy(pose, &joints, 2);
        std::memcpy(pose + ecm::kPoseLocalsOff, &lp, 8);
        std::memcpy(pose + ecm::kPoseParentsOff, &pa, 8);
        for (int j = 0; j < 64; ++j) {
            parents[j] = 0;
            locals[j * 8 + 7] = 1.0f;
        }
        parents[0] = 0xFFFF;
        pos(12, 0.0f, 1.675f, 0.003f);
        pos(3, 0.0f, 1.713f, 0.115f);
    }
    void pos(int j, float x, float y, float z) {
        locals[j * 8 + 0] = x;
        locals[j * 8 + 1] = y;
        locals[j * 8 + 2] = z;
    }
};
uint64_t g_fakeUs = 0;
uint64_t fakeNowUs() { return g_fakeUs; }
void setHead(float x, float y, float z, const float rows[9] = kIdentityRows) { headMatrix(g_cr.headM, rows, x, y, z); }

void testFollowGlue(const Pages& p) {
    std::printf("glue: the eye follows the head joint (a synthetic skeleton in a synthetic game image, the real FindJoint hook)\n");
    namespace t = edvr::explorercamtest;
    static Game g;
    static CamSkel skel, skel2, first;
    Rig rig;
    const void* pov = nullptr;
    auto begin = [&](bool image = true, bool corruptLiteral = false) {
        t::reset();
        g.init();
        cimgInit();
        if (corruptLiteral) g_cimg[ecm::kHeadNameRva] ^= 1;   // the exe's "def_c_head_joint" is not what it should be
        rig = Rig();
        ExplorerCamTestTargets tt;
        tt.freeCamera = reinterpret_cast<uintptr_t>(p.freeG);
        tt.collision = reinterpret_cast<uintptr_t>(p.colG);
        tt.boxPush = reinterpret_cast<uintptr_t>(p.boxG);
        tt.cameraUi = reinterpret_cast<uintptr_t>(p.uiG);
        tt.controller = reinterpret_cast<uintptr_t>(p.ctlG);
        tt.avatarFade = reinterpret_cast<uintptr_t>(g_cimg) + ecm::kAvatarFadeRva;
        tt.findJoint = reinterpret_cast<uintptr_t>(g_cimg) + ecm::kFindJointRva;
        t::setTargets(tt);
        t::setHeadImage(image ? reinterpret_cast<uintptr_t>(g_cimg) : 0, kCImgSize);
        g.freeUpdate = reinterpret_cast<FnObj>(p.freeG);
        g.ctlUpdate = reinterpret_cast<FnObj>(p.ctlG);
        g.uiUpdate = reinterpret_cast<FnObj>(p.uiG);
        pov = g_cimg + ecm::kPovNameRva;
        skel.init();
        skel2.init();
        first.init(1);
        setHead(0.00f, 1.66f, 0.03f);
        rig.boundary();
        rig.boundary();
    };
    auto place = [&]() {
        g.setMode(3);
        g.free[0x473] = 1;
        g.ctlFrame(); rig.boundary(); g.ctlFrame(); rig.boundary();
        rig.boundary(true);
        g.ctlFrame();
        g.freeFrame(); g.freeFrame(); g.ctlFrame(); g.uiFrame(); g.uiFrame();
        rig.boundary();
    };
    auto latch = [&](CamSkel& s) { imgInvoke(s.iface, first.iface, pov); };
    auto eyeIs = [&](float right, float up, float forward, float tol = 3e-5f) {
        return closeTo(g.seen(kSeenX), right, tol) && closeTo(g.seen(kSeenY), up, tol) && closeTo(g.seen(kSeenZ), forward, tol);
    };

    // ---- arming ----------------------------------------------------------------------------------------------------------------------------------------
    begin(false);
    check(!t::followReady() && rig.cap.count("explorer cam: head follow: the head-joint source is not armed") == 1,
          "THE BUILD IS NOT KNOWN: one line says the head-joint source is not armed and the fixed keys place the view");
    latch(skel);
    place();
    g.freeFrame();
    check(t::followSource() == 0 && t::followWhy() == static_cast<uint32_t>(ecm::FixedWhy::NotArmed) && eyeIs(0.0f, 1.68f, 0.10f) && g_cr.modelCalls == 0,
          "...a latched skeleton changes nothing: the eye is the fixed one (up 1.68, forward 0.10), no game call was made, the reason is 'not armed'");

    begin(true, true);
    check(!t::followReady() && rig.cap.count("the head-joint source is not armed: the joint-name literals at EliteDangerous64.exe+0x554EE10 / +0x51F9930 are not") == 1,
          "THE HEAD LITERAL DIFFERS (\"def_c_head_joint\" is not what the exe holds): not armed, one line names the two literals, the fixed keys place the view");
    begin(true);
    check(t::followReady() && rig.cap.count("explorer cam: head follow: armed (build 332841)") == 1, "ARMED on the synthetic image: one line, once");
    check(has(rig.cap.nth("head follow: armed", 0), "+0x559CF90") && has(rig.cap.nth("head follow: armed", 0), "+0x517DC20") && has(rig.cap.nth("head follow: armed", 0), "8 faults stand the head source down"),
          "...it names the two vtables, the checks before every call and the 8-fault stand-down that never touches the placement");

    // ---- nothing latched: the fixed keys ------------------------------------------------------------------------------------------------------------------
    place();
    g.freeFrame();
    check(t::followSource() == 0 && t::followWhy() == static_cast<uint32_t>(ecm::FixedWhy::NothingLatched) && eyeIs(0.0f, 1.68f, 0.10f) && g_cr.modelCalls == 0 && g_cr.poseCalls == 0,
          "NOTHING LATCHED: the fixed eye (the absolute keys are the fallback), and the skeleton is not called");
    check(rig.cap.count("the eye now comes from the FIXED keys") == 1 && has(rig.cap.nth("the eye now comes from the FIXED keys", 0), "no local skeleton pair is latched yet"),
          "...one line says the eye comes from the fixed keys and why");

    // ---- a latched RR skeleton: verified once, then the head joint ---------------------------------------------------------------------------------------------
    latch(skel);
    check(explorerCamSkeleton(0).iface == reinterpret_cast<uint64_t>(skel.iface), "(the local pair is latched by the real FindJoint hook)");
    const int updates0 = static_cast<int>(t::updatesPlaced());
    g.freeFrame();
    check(t::followSource() == 1 && eyeIs(0.00f, 1.698f, 0.142f), "THE STANDING HEAD (0.00,1.66,0.03): the eye is up 1.698, forward 0.142 (the F7 rest offset 0.038 up, 0.112 forward)");
    check(g_cr.poseCalls == 1 && g_cr.findHeadCalls == 1 && g_cr.modelCalls == 1 && g_cr.lastModelIdx == 12 && g_cr.lastModelIface == reinterpret_cast<uintptr_t>(skel.iface) &&
              (g_cr.lastModelOut & 15) == 0 && g_cr.modelThread == GetCurrentThreadId(),
          "...one GetPoseData and one FindJoint(head) to derive it, then +0x58 for joint 12 with a 16-byte aligned buffer, on the calling (camera-job) thread");
    rig.cap.clear();
    rig.boundary();
    check(rig.cap.count("explorer cam: head follow: head joint of skeleton 0x") == 1 && has(rig.cap.nth("head follow: head joint of skeleton", 0), "(RR (RuntimeRigComponent), 64 joints) verified: head idx 12 rest (0.000,1.675,0.003)") &&
              has(rig.cap.nth("head follow: head joint of skeleton", 0), "povCamera idx 3 rest (0.000,1.713,0.115)") && has(rig.cap.nth("head follow: head joint of skeleton", 0), "(0.000,0.038,0.112) in the head joint's own axes"),
          "THE REST OFFSET IS LOGGED ONCE: head idx 12, povCamera idx 3, the offset (0, 0.038, 0.112) in the head's own axes");
    check(rig.cap.count("first live head joint (+0x58)") == 1 && has(rig.cap.nth("first live head joint", 0), "at most 0.000 per element") && has(rig.cap.nth("first live head joint", 0), "up=1.698 forward=0.142 right=0.000") &&
              rig.cap.count("the eye now comes from the head joint of skeleton") == 1,
          "...and the first live read once (the rotation matches the rest pose), and the switch to the head joint once");
    setHead(0.09f, 0.94f, 0.19f);
    g.freeFrame();
    check(eyeIs(0.09f, 0.978f, 0.302f), "CROUCHED (0.09,0.94,0.19): up 0.978, forward 0.302, right 0.09 -- the camera follows the body down");
    setHead(0.10f, 1.38f, 0.19f);
    g.freeFrame();
    check(eyeIs(0.10f, 1.418f, 0.302f), "WEAPON OUT (0.10,1.38,0.19): up 1.418, forward 0.302, right 0.10");
    setHead(0.00f, 1.66f, 0.03f);
    g.freeFrame();
    check(eyeIs(0.00f, 1.698f, 0.142f) && g_cr.poseCalls == 1 && g_cr.findHeadCalls == 1 && g_cr.modelCalls == 4,
          "STANDING AGAIN: back to up 1.698; the rest offset was derived once (still one GetPoseData and one FindJoint), +0x58 once per update (4)");
    const int modelsBefore = g_cr.modelCalls;
    rig.boundary();
    rig.boundary();
    rig.boundary();
    check(g_cr.modelCalls == modelsBefore && g_cr.poseCalls == 1, "NEVER FROM THE FRAME THREAD: three boundaries made no skeleton call");
    check(static_cast<int>(t::updatesPlaced()) - updates0 == 4 && t::followHeadUpdates() >= 4, "(every placing update used the head joint)");

    // ---- the pre-call: the original saw THIS update's eye ----------------------------------------------------------------------------------------------------
    setHead(0.30f, 1.20f, 0.10f);
    g.freeUpdate(g.free);
    check(eyeIs(0.30f, 1.238f, 0.212f), "THE READ IS IN THE PRE-CALL: the game's own update saw the eye of the head as it was just before it ran");

    // ---- a head turned 90 degrees: the rest offset turns with it -----------------------------------------------------------------------------------------------------
    {
        const float yaw90[9] = {0, 0, -1, 0, 1, 0, 1, 0, 0};
        setHead(0.00f, 1.66f, 0.03f, yaw90);
        g.freeUpdate(g.free);
        // eye = pos + 0*row0 + 0.038*row1 + 0.112*row2 = (0.112, 1.698, 0.03)
        check(eyeIs(0.112f, 1.698f, 0.030f), "A HEAD TURNED 90 DEGREES: the offset (0,0.038,0.112) is carried by the head's own axes: right 0.112, up 1.698, forward 0.03");
        rig.cap.clear();
        rig.boundary();
        setHead(0.00f, 1.66f, 0.03f);
    }

    // ---- the trims, live, held to +-0.5, added after the joint -------------------------------------------------------------------------------------------------
    rig.f.trimRight = 0.05f;
    rig.f.trimUp = -0.02f;
    rig.f.trimForward = 0.03f;
    rig.cap.clear();
    rig.boundary();
    g.freeUpdate(g.free);
    check(eyeIs(0.05f, 1.678f, 0.172f) && rig.cap.count("explorer cam: head follow: trims right=0.050 up=-0.020 forward=0.030 m") == 1,
          "THE TRIMS (right 0.05, up -0.02, forward 0.03) are added after the joint, live, and said once");
    rig.f.trimUp = 0.9f;
    rig.boundary();
    g.freeUpdate(g.free);
    check(eyeIs(0.05f, 2.198f, 0.172f), "...held to +-0.5: a trim of 0.9 is 0.5");
    rig.f.trimRight = rig.f.trimUp = rig.f.trimForward = 0.0f;
    rig.boundary();
    g.freeUpdate(g.free);
    check(eyeIs(0.0f, 1.698f, 0.142f), "...and back to zero");
    // The SHIPPED trims (0.15 up, -0.08 forward) with the eye already placed: the next update sits at the head + the rest offset + them, and ONE trim changed
    // live (the menu's Explorer Cam page does exactly this, a hundredth at a time, in the headset) moves the eye on the very next update with the session going.
    rig.f.trimRight = ecm::kTrimRightDefault;
    rig.f.trimUp = ecm::kTrimUpDefault;
    rig.f.trimForward = ecm::kTrimForwardDefault;
    rig.boundary();
    g.freeUpdate(g.free);
    check(eyeIs(0.0f, 1.848f, 0.062f), "THE SHIPPED TRIMS (up 0.15, forward -0.08, right 0) lift the placed eye by 0.15 and bring it back 0.08 from the head joint's own");
    const uint64_t placedBefore = t::updatesPlaced();
    rig.f.trimUp = 0.16f;   // one step of the menu row
    rig.boundary();
    g.freeUpdate(g.free);
    check(eyeIs(0.0f, 1.858f, 0.062f) && t::updatesPlaced() == placedBefore + 1 && t::phase() == 2,
          "A LIVE CHANGE WHILE PLACED (eye height +0.01) takes effect on the next update, forward and right untouched, the session undisturbed");
    rig.f.trimUp = 0.14f;
    rig.f.trimRight = -0.03f;
    rig.boundary();
    g.freeUpdate(g.free);
    check(eyeIs(-0.03f, 1.838f, 0.062f), "...and two at once, a step down and a step sideways, on the next one");
    rig.f.trimRight = rig.f.trimUp = rig.f.trimForward = 0.0f;
    rig.boundary();
    g.freeUpdate(g.free);

    // ---- the absolute keys are only the fallback -------------------------------------------------------------------------------------------------------------
    rig.f.up = 1.50f;
    rig.f.forward = 0.20f;
    rig.f.right = 0.05f;
    rig.boundary();
    g.freeUpdate(g.free);
    check(eyeIs(0.0f, 1.698f, 0.142f), "THE ABSOLUTE EYE KEYS DO NOTHING while the head joint serves (up 1.50 changes nothing)");

    // ---- stale: the avatar was destroyed ---------------------------------------------------------------------------------------------------------------------
    const int modelsStale = g_cr.modelCalls;
    uint64_t vtSave = 0;
    std::memcpy(&vtSave, skel.iface, 8);
    const uint64_t garbage = 0x1234;
    std::memcpy(skel.iface, &garbage, 8);
    rig.cap.clear();
    g.freeUpdate(g.free);
    rig.boundary();
    check(t::followSource() == 0 && t::followWhy() == static_cast<uint32_t>(ecm::FixedWhy::Stale) && eyeIs(0.05f, 1.50f, 0.20f) && g_cr.modelCalls == modelsStale && t::followFaults() == 0,
          "A STALE INTERFACE (its vtable is gone): the FALLBACK keys serve (right 0.05, up 1.50, forward 0.20), +0x58 is not called, no fault is counted");
    check(rig.cap.count("the eye now comes from the FIXED keys") == 1 && has(rig.cap.nth("the eye now comes from the FIXED keys", 0), "no longer a skeleton interface"), "...one line says why");
    std::memcpy(skel.iface, &vtSave, 8);
    rig.cap.clear();
    g.freeUpdate(g.free);
    rig.boundary();
    check(t::followSource() == 1 && eyeIs(0.0f, 1.698f, 0.142f) && rig.cap.count("head follow: head joint of skeleton") == 1 && rig.cap.count("the eye now comes from the head joint") == 1,
          "...the interface valid again: the rest offset is derived again and the head joint serves");
    rig.f.up = ecm::kEyeUpDefault;
    rig.f.forward = ecm::kEyeForwardDefault;
    rig.f.right = ecm::kEyeRightDefault;
    rig.boundary();

    // ---- an implausible matrix: the fixed eye for that update, no fault -------------------------------------------------------------------------------------------
    setHead(0.0f, 5.0f, 0.03f);
    g.freeUpdate(g.free);
    check(t::followSource() == 0 && t::followWhy() == static_cast<uint32_t>(ecm::FixedWhy::Implausible) && eyeIs(0.0f, 1.68f, 0.10f) && t::followFaults() == 0,
          "AN IMPLAUSIBLE HEAD (5 m up): the fixed eye for that update, counted as no fault");
    setHead(std::nanf(""), 1.66f, 0.03f);
    g.freeUpdate(g.free);
    check(t::followSource() == 0 && t::followWhy() == static_cast<uint32_t>(ecm::FixedWhy::Implausible), "...also a NaN");
    setHead(0.0f, 1.66f, 0.03f);
    g.freeUpdate(g.free);
    check(t::followSource() == 1 && eyeIs(0.0f, 1.698f, 0.142f), "...and the next good matrix is followed again");

    // ---- a fault in +0x58: the fixed eye for that update, counted, the placement untouched ------------------------------------------------------------------------
    g_cr.faultAt = 3;
    g_cr.faultCount = 1;
    const uint64_t placedUpdates = t::updatesPlaced();
    g.freeUpdate(g.free);
    check(t::followSource() == 0 && t::followWhy() == static_cast<uint32_t>(ecm::FixedWhy::Fault) && t::followFaults() == 1 && eyeIs(0.0f, 1.68f, 0.10f) && t::faults() == 0 &&
              t::updatesPlaced() == placedUpdates + 1,
          "A FAULT IN THE +0x58 CALL: caught, the fixed eye for that update, one head fault counted, the placement's own fault budget untouched and the pose still written");
    g.freeUpdate(g.free);
    check(t::followSource() == 1 && eyeIs(0.0f, 1.698f, 0.142f) && !t::followDown(), "...the next update reads the head again");

    // ---- eight faults stand the head source down, not the placement -------------------------------------------------------------------------------------------------
    g_cr.faultAt = 3;
    g_cr.faultCount = 0;
    rig.cap.clear();
    for (int i = 0; i < 8; ++i) g.freeUpdate(g.free);
    rig.boundary();
    check(t::followDown() && t::followFaults() == 8 && rig.cap.count("the head-joint source stood down for the session: 8 reads or calls") == 1 && t::faults() == 0 && t::placeActive() &&
              t::phase() == 2,
          "THE EIGHTH FAULT stands the head source down for the session (said once); the placement's budget is untouched and the camera is still placed");
    g_cr.faultAt = 0;
    const int modelsDown = g_cr.modelCalls;
    g.freeUpdate(g.free);
    check(t::followSource() == 0 && t::followWhy() == static_cast<uint32_t>(ecm::FixedWhy::StoodDown) && eyeIs(0.0f, 1.68f, 0.10f) && g_cr.modelCalls == modelsDown,
          "...from then on the fixed keys serve and the skeleton is not called again");

    // ---- a vtable slot that differs: another build ----------------------------------------------------------------------------------------------------------------
    begin(true);
    latch(skel);
    place();
    g_cr.swapModelSlot = true;
    g.freeFrame();   // derives, reads the head; the +0x58 stub swaps the RR vtable's +0x48 slot behind our back
    rig.cap.clear();
    g.freeFrame();   // the check before the next call sees it
    rig.boundary();
    check(t::followDown() && t::followWhy() == static_cast<uint32_t>(ecm::FixedWhy::StoodDown) && rig.cap.count("vtable slot +0x48 of skeleton 0x") == 1 &&
              has(rig.cap.nth("vtable slot +0x48", 0), "not the build-332841 function") && t::followFaults() == 0 && eyeIs(0.0f, 1.68f, 0.10f),
          "A VTABLE SLOT THAT DIFFERS (+0x48 swapped): the head source stands down before the next call, naming the slot, and the fixed keys serve");
    g_cr.swapModelSlot = false;

    // ---- the head joint is not found -----------------------------------------------------------------------------------------------------------------------------
    begin(true);
    g_cr.headIdx = 0xFFFF;
    latch(skel);
    place();
    g.freeFrame();
    g.freeFrame();
    rig.boundary();
    check(t::followSource() == 0 && t::followWhy() == static_cast<uint32_t>(ecm::FixedWhy::Unverified) && eyeIs(0.0f, 1.68f, 0.10f) && g_cr.findHeadCalls == 1 && g_cr.poseCalls == 1 &&
              g_cr.modelCalls == 0 && t::followFaults() == 0,
          "NO HEAD JOINT FOUND (0xFFFF): the fixed keys serve this skeleton, tried ONCE (one FindJoint, one GetPoseData across several updates), +0x58 never called, not a fault");
    check(rig.cap.count("cannot be used: FindJoint(\"def_c_head_joint\") found no head joint") == 1, "...one line says why");
    g_cr.headIdx = 12;
    skel2.init();
    latch(skel2);
    g.freeFrame();
    check(t::followSource() == 1 && eyeIs(0.0f, 1.698f, 0.142f), "...a NEW skeleton is tried afresh and works");

    // ---- no joints, an unreadable pose, an AO skeleton ----------------------------------------------------------------------------------------------------------
    begin(true);
    skel.init(0, 0);
    latch(skel);
    place();
    g.freeFrame();
    g.freeFrame();
    check(t::followSource() == 0 && t::followWhy() == static_cast<uint32_t>(ecm::FixedWhy::Unverified) && g_cr.findHeadCalls == 0 && g_cr.modelCalls == 0,
          "A POSE WITH NO JOINTS: refused once, no FindJoint, no +0x58");
    begin(true);
    skel.init();
    const uint64_t none = 0;
    std::memcpy(skel.pose + ecm::kPoseLocalsOff, &none, 8);
    latch(skel);
    place();
    g.freeFrame();
    check(t::followSource() == 0 && t::followWhy() == static_cast<uint32_t>(ecm::FixedWhy::Fault) && t::followFaults() >= 1 && g_cr.modelCalls == 0,
          "UNREADABLE POSE ARRAYS: counted as a fault of the head read (never as the placement's), the fixed keys serve");
    begin(true);
    skel.init(1);
    latch(skel);
    place();
    g.freeFrame();
    setHead(0.00f, 1.50f, 0.10f);
    g.freeFrame();
    rig.boundary();
    check(t::followSource() == 1 && eyeIs(0.0f, 1.538f, 0.212f) && g_cr.lastModelIface == reinterpret_cast<uintptr_t>(skel.iface),
          "AN AO (AnimatedObject) SKELETON works the same through its own slots");
    check(rig.cap.count("(AO (AnimatedObject), 64 joints) verified") == 1, "...and the line names the kind");
    begin(true);
    g_cr.headJunk = 0x00DE0000u;   // FindJoint returns a u16 in ax: the high half of eax is whatever it was
    latch(skel);
    place();
    g.freeFrame();
    g.freeFrame();
    check(t::followSource() == 1 && g_cr.lastModelIdx == 12 && eyeIs(0.0f, 1.698f, 0.142f), "FINDJOINT RETURNS A u16: garbage in the high half of eax is not part of the joint index");

    // ---- a pose that is not built yet is tried again (review 2026-10-08, conditional concern 1) ---------------------------------------------------------------------
    // A null pose or a zero joint count is transient: the first derive fails, said once, and the fixed keys serve until a retry (every kFollowRetryUpdates updates,
    // silently) finds the pose built. Every other refusal (no head joint, too many joints, a fault) is for good, or counted, as before.
    {
        // Updates until the next GetPoseData call (a derive attempt); -1 when none came within `maxUpdates`.
        auto windowOf = [&](int maxUpdates) {
            const int before = g_cr.poseCalls;
            int n = 0;
            while (g_cr.poseCalls == before && n < maxUpdates) {
                g.freeFrame();
                ++n;
            }
            return g_cr.poseCalls == before ? -1 : n;
        };
        static_assert(ecm::kFollowRetryUpdates == 60, "the retry window is one second of updates at 60 Hz");
        const uint64_t noPose = 0;

        begin(true);
        skel.init();
        uint64_t realPose = 0;
        std::memcpy(&realPose, skel.iface + 0x20, 8);
        std::memcpy(skel.iface + 0x20, &noPose, 8);   // GetPoseData returns null: the pose is not built yet
        latch(skel);
        place();
        g.freeFrame();
        const int firstTry = windowOf(200);
        rig.boundary();
        check(firstTry >= 0 && t::followSource() == 0 && t::followWhy() == static_cast<uint32_t>(ecm::FixedWhy::Unverified) && eyeIs(0.0f, 1.68f, 0.10f) && g_cr.modelCalls == 0 && t::followFaults() == 0,
              "A NULL POSE: the fixed keys serve (up 1.68), +0x58 never called, not a fault");
        check(windowOf(200) == static_cast<int>(ecm::kFollowRetryUpdates), "...tried AGAIN exactly kFollowRetryUpdates (60) updates after the last try, not every update and not never");
        check(windowOf(200) == static_cast<int>(ecm::kFollowRetryUpdates) && g_cr.findHeadCalls == 0 && g_cr.modelCalls == 0, "...and again 60 updates later, still no head read");
        rig.boundary();
        check(rig.cap.count("cannot be used: GetPoseData returned no pose") == 1 && rig.cap.count("head follow: head joint of skeleton") == 0,
              "...the failure is said ONCE across three tries (a retry that fails again is silent)");
        std::memcpy(skel.iface + 0x20, &realPose, 8);   // the pose is built now
        const int ok = windowOf(200);
        rig.boundary();
        check(ok >= 1 && ok <= static_cast<int>(ecm::kFollowRetryUpdates) && t::followSource() == 1 && eyeIs(0.0f, 1.698f, 0.142f) && g_cr.findHeadCalls == 1 && g_cr.modelCalls >= 1 &&
                  rig.cap.count("head follow: head joint of skeleton") == 1 && rig.cap.count("the eye now comes from the head joint") == 1,
              "THE POSE APPEARS: within one window the retry derives the rest offset, the head joint serves (up 1.698, forward 0.142), the rest line and the switch are said once");
        const int posesAfter = g_cr.poseCalls;
        for (int i = 0; i < 150; ++i) g.freeFrame();
        check(g_cr.poseCalls == posesAfter && t::followSource() == 1, "...once derived the pose is not called again (a retry is only for a failure)");

        // The same with a pose whose joint count is still 0.
        begin(true);
        skel.init(0, 0);
        latch(skel);
        place();
        g.freeFrame();
        windowOf(200);
        check(t::followSource() == 0 && windowOf(200) == static_cast<int>(ecm::kFollowRetryUpdates) && g_cr.findHeadCalls == 0, "A POSE WITH NO JOINTS YET: tried again every 60 updates, no FindJoint while it has none");
        const uint16_t sixtyFour = 64;
        std::memcpy(skel.pose, &sixtyFour, 2);   // the pose is filled in
        const int ok2 = windowOf(200);
        check(ok2 >= 1 && ok2 <= static_cast<int>(ecm::kFollowRetryUpdates) && t::followSource() == 1 && eyeIs(0.0f, 1.698f, 0.142f), "...and when the joints exist the next retry follows the head joint");

        // A retry that faults is counted and is tried again a window later (not never).
        begin(true);
        skel.init();
        std::memcpy(&realPose, skel.iface + 0x20, 8);
        std::memcpy(skel.iface + 0x20, &noPose, 8);
        latch(skel);
        place();
        g.freeFrame();
        windowOf(200);
        g_cr.faultAt = 1;
        g_cr.faultCount = 1;
        const uint32_t faultsBefore = t::followFaults();
        const int faultTry = windowOf(200);
        check(faultTry >= 1 && t::followFaults() == faultsBefore + 1 && t::followSource() == 0, "A RETRY THAT FAULTS is counted as one head fault and the fixed keys keep serving");
        std::memcpy(skel.iface + 0x20, &realPose, 8);
        check(windowOf(200) == static_cast<int>(ecm::kFollowRetryUpdates) && t::followSource() == 1 && eyeIs(0.0f, 1.698f, 0.142f),
              "...and the retry after a faulted one comes a window later and works (the countdown is re-armed before the try)");

        // The refusals that are NOT transient stand: no head joint is tried once, however long the session runs.
        begin(true);
        g_cr.headIdx = 0xFFFF;
        latch(skel);
        place();
        for (int i = 0; i < 200; ++i) g.freeFrame();
        rig.boundary();
        check(g_cr.poseCalls == 1 && g_cr.findHeadCalls == 1 && t::followSource() == 0 && rig.cap.count("cannot be used: FindJoint(\"def_c_head_joint\") found no head joint") == 1,
              "NO HEAD JOINT IS FOR GOOD: 200 updates later still one GetPoseData and one FindJoint (only a null pose and a zero joint count retry)");
        g_cr.headIdx = 12;
        skel2.init();
        latch(skel2);
        g.freeFrame();
        check(t::followSource() == 1 && eyeIs(0.0f, 1.698f, 0.142f), "...and a new skeleton is still tried afresh");
    }

    // ---- the smoothing: 0 is exact; a time constant eases; the comfort key is live ----------------------------------------------------------------------------------
    begin(true);
    latch(skel);
    place();
    t::setNowUs(&fakeNowUs);
    g_fakeUs = 1000000;
    setHead(0.00f, 1.66f, 0.03f);
    g.freeUpdate(g.free);
    float expectUp = 0, expectFwd = 0;
    {
        FlatPose bind;
        bind.pos(12, 0.0f, 1.675f, 0.003f);
        bind.pos(3, 0.0f, 1.713f, 0.115f);
        ecm::RestOffset ro;
        ecm::deriveRestOffset(bind.locals, bind.parents, 64, 12, 3, &ro);
        float c2[16], p2[3];
        headMatrix(c2, kIdentityRows, 0.09f, 0.94f, 0.19f);
        ecm::headEyeModel(c2, ro.local, p2);
        const ecm::Eye want = ecm::eyeFromModelPoint(p2, ecm::Trim());
        setHead(0.09f, 0.94f, 0.19f);
        g.freeUpdate(g.free);
        const float gotX = g.seen(kSeenX), gotY = g.seen(kSeenY), gotZ = g.seen(kSeenZ);
        check(std::memcmp(&gotX, &want.right, 4) == 0 && std::memcmp(&gotY, &want.up, 4) == 0 && std::memcmp(&gotZ, &want.forward, 4) == 0,
              "SMOOTHING 0 (the default): the pose origin the game's update saw is BIT-IDENTICAL to the unsmoothed eye computed from the same arithmetic");
        expectUp = want.up;
        expectFwd = want.forward;
    }
    rig.f.smoothingMs = 100.0f;
    rig.cap.clear();
    rig.boundary();
    check(rig.cap.count("follow smoothing 100 ms") == 1, "(the smoothing key is live and said once)");
    setHead(0.00f, 1.66f, 0.03f);
    g.freeUpdate(g.free);   // the first smoothed eye snaps (no history yet)
    check(eyeIs(0.0f, 1.698f, 0.142f), "SMOOTHING 100 ms: the first eye snaps to the head");
    setHead(0.09f, 0.94f, 0.19f);
    g_fakeUs += 100000;   // one time constant
    g.freeUpdate(g.free);
    {
        const float k = static_cast<float>(1.0 - std::exp(-1.0));
        check(eyeIs(0.09f * k, 1.698f + (expectUp - 1.698f) * k, 0.142f + (expectFwd - 0.142f) * k, 5e-5f), "...a crouch 100 ms later has covered 1 - 1/e of the way: the camera trails the head");
    }
    g.freeUpdate(g.free);
    check(eyeIs(0.09f * static_cast<float>(1.0 - std::exp(-1.0)), 1.698f + (expectUp - 1.698f) * static_cast<float>(1.0 - std::exp(-1.0)), 0.142f + (expectFwd - 0.142f) * static_cast<float>(1.0 - std::exp(-1.0)), 5e-5f),
          "...no time passed: it stays where it was");
    {
        // A trip through the fixed keys forgets the smoothing's history: coming back, the eye starts from the head.
        uint64_t vt = 0;
        std::memcpy(&vt, skel.iface, 8);
        const uint64_t bad = 0x1234;
        std::memcpy(skel.iface, &bad, 8);
        g_fakeUs += 1000;
        g.freeUpdate(g.free);
        std::memcpy(skel.iface, &vt, 8);
        g_fakeUs += 1000;
        g.freeUpdate(g.free);
        check(eyeIs(0.09f, expectUp, expectFwd, 2e-5f), "SWITCHING BACK to the head joint after the fixed keys starts from the head, not from a stale smoothed eye");
    }
    rig.f.smoothingMs = 0.0f;
    rig.boundary();
    g.freeUpdate(g.free);
    {
        const float gotX = g.seen(kSeenX), gotY = g.seen(kSeenY);
        check(std::memcmp(&gotY, &expectUp, 4) == 0 && closeTo(gotX, 0.09f, 1e-6f), "...and the key back at 0 is the exact head eye on the very next update, bit for bit");
    }
    t::setNowUs(nullptr);

    // ---- the heartbeat names the source and the +0x58 cost -----------------------------------------------------------------------------------------------------
    begin(true);
    latch(skel);
    place();
    for (int i = 0; i < 5; ++i) g.freeUpdate(g.free);
    rig.advance(5200);
    rig.cap.clear();
    rig.boundary();
    const std::string beat2 = rig.cap.nth("explorer cam: heartbeat (follow, isolation):", 0);
    check(has(beat2, "eye_source=head-joint") && has(beat2, "head_joint_updates=") && has(beat2, "head_source=on") && has(beat2, "head_faults=0") && has(beat2, "read58_us(n/min/max/session_max)=") &&
              !has(beat2, "read58_us(n/min/max/session_max)=0/") && has(beat2, "last_eye(up=1.698 forward=0.142 right=0.000)"),
          "THE HEARTBEAT says the eye comes from the head joint, the last eye, the counts and the +0x58 call's n/min/max/session max in microseconds");
    check(rig.cap.count("explorer cam: heartbeat:") == 1, "...beside the main heartbeat, which keeps its own line");
    t::reset();
}

// ================================ Phase 3, glue: the camera suite is isolated while the view is placed ================================

// A probe that also makes the page its action object lives on read-only while the original runs, so the restore after it faults.
uint8_t* g_protectPage = nullptr;
void __fastcall probeFreeProtect(void* o) {
    snapIso(o, 0);
    DWORD old = 0;
    VirtualProtect(g_protectPage, 4096, PAGE_READONLY, &old);
}

void testIsolationGlue(const Pages& p) {
    std::printf("glue: the camera suite is isolated while the view is placed (save, zero, original, restore; EDVR's own presses survive)\n");
    namespace t = edvr::explorercamtest;
    static Game g;
    Rig rig;
    auto begin = [&](bool zoomHook = true, bool zoomBadPrologue = false) {
        t::reset();
        g.init();
        g.isoWire();
        rig = Rig();
        ExplorerCamTestTargets tt;
        tt.freeCamera = reinterpret_cast<uintptr_t>(p.freeG);
        tt.collision = reinterpret_cast<uintptr_t>(p.colG);
        tt.boxPush = reinterpret_cast<uintptr_t>(p.boxG);
        tt.cameraUi = reinterpret_cast<uintptr_t>(p.uiG);
        tt.controller = reinterpret_cast<uintptr_t>(p.ctlG);
        tt.zoomDof = zoomHook ? reinterpret_cast<uintptr_t>(zoomBadPrologue ? p.zoomB : p.zoomG) : 0;
        t::setTargets(tt);
        g.freeUpdate = reinterpret_cast<FnObj>(p.freeG);
        g.ctlUpdate = reinterpret_cast<FnObj>(p.ctlG);
        g.uiUpdate = reinterpret_cast<FnObj>(p.uiG);
        g.zoomUpdate = reinterpret_cast<FnObj>(p.zoomG);
        g_probeFree = &probeFreeCb;
        g_probeCtl = &probeCtlCb;
        g_probeUi = &probeUiCb;
        g_probeZoom = &probeZoomCb;
        std::memset(g_seenIso, 0, sizeof(g_seenIso));
        for (const uint8_t*& c : g_canaryObj) c = nullptr;
        rig.boundary();
        rig.boundary();
    };
    auto place = [&]() {
        g.setMode(3);
        g.free[0x473] = 1;
        g.ctlFrame(); rig.boundary(); g.ctlFrame(); rig.boundary();
        rig.boundary(true);
        g.ctlFrame();
        g.freeFrame(); g.freeFrame(); g.ctlFrame(); g.uiFrame(); g.uiFrame();
        rig.boundary();
    };
    auto fillAll = [&]() { for (int h = 0; h < 4; ++h) g.isoFill(h); };
    auto runAll = [&]() {
        g.freeUpdate(g.free);
        g.ctlUpdate(g.ctl);
        g.uiUpdate(g.ui);
        g.zoomUpdate(g.zoom);
    };
    auto exitSession = [&]() {
        rig.boundary(true);
        for (int i = 0; i < 24 && g.mode() != 0; ++i) {
            g.ctlFrame();
            g.uiFrame();
            rig.boundary();
        }
        g.ctlUpdate(g.ctl);   // the update after it sees mode 0: the session is over
        rig.boundary();
    };

    // ---- the hook is installed like the others, and its prologue is checked -----------------------------------------------------------------------------------
    begin();
    check(t::stolenBytes(7) == 6 && p.zoomG[0] == 0xE9 && rig.cap.count("zoom/DOF hook armed:") == 1 && has(rig.cap.nth("zoom/DOF hook armed:", 0), "stolen=6 bytes") &&
              has(rig.cap.nth("zoom/DOF hook armed:", 0), "16/16 bytes verified") && has(rig.cap.nth("zoom/DOF hook armed:", 0), "ISOLATION only"),
          "THE ZOOM/DOF HOOK: installed (6 bytes stolen, 16/16 prologue bytes verified), said once, and described as isolation only");
    check(t::zoomGateOpen(), "...its gate is open while Explorer Cam is active");

    // ---- nothing is blocked outside a session ------------------------------------------------------------------------------------------------------------
    fillAll();
    runAll();
    bool outside = true;
    for (int h = 0; h < 4; ++h) outside = outside && g.isoSeenPatternCount(h) == kExpCount[h] && g.isoIntact(h) && t::isoBlocked(h) == 0;
    check(outside, "NO SESSION: every key of all four holders (11, 15, 1 and 7 actions) reaches the game's own update untouched, and nothing is counted as blocked");
    check(t::zoomCalls() == 1 && g.zoomCallsSeen() == 1, "...the zoom/DOF update was called through the hook and ran (1 call, 1 run)");

    // ---- a session whose view is not placed yet (F5's own entry sequence): nothing is blocked --------------------------------------------------------------------
    begin();
    g.ctlUpdate(g.ctl);
    rig.boundary();
    g.ctlUpdate(g.ctl);
    rig.boundary();
    g.isoFill(1);
    rig.boundary(true);
    g.ctlUpdate(g.ctl);   // the sequencer's first update: EDVR presses PhotoCameraToggle (+0x310)
    check(t::sessionActive() && t::placedActivity() == 0, "(a session is on, the view is not placed)");
    bool entryOk = g.isoSeen(1, 0) == 2 && g_seenIso[1].v[0] == 1;   // EDVR's own press replaces the user's value for that one update
    for (int i = 1; i < 15; ++i) entryOk = entryOk && g.isoSeen(1, i) == 1;
    check(entryOk && t::isoBlocked(1) == 0 && g.isoIntact(1), "F5's ENTRY SEQUENCE blocks nothing: the other fourteen controller keys are the player's, EDVR's own press is the one that changed, and every value is back after");

    // ---- placed: every holder, every field --------------------------------------------------------------------------------------------------------------------
    begin();
    place();
    check(t::placedActivity() == reinterpret_cast<uint64_t>(g.free) && t::sessionActive(), "(placed)");
    fillAll();
    runAll();
    check(g.isoSeenZeroCount(0) == 11 && g.isoIntact(0) && t::isoBlocked(0) == 11, "PLACED, THE FREE CAMERA: all 11 actions (speed inc/dec held, the six move/look axes, the three lock toggles) read 0 inside the original, and are back exactly after; 11 blocked");
    check(g.isoSeenZeroCount(1) == 15 && g.isoIntact(1) && t::isoBlocked(1) == 15, "PLACED, THE CONTROLLER: all 15 (camera toggle, two scrolls, free-cam toggle, quit, presets one to ten) read 0 and are back exactly; 15 blocked");
    check(g.isoSeenZeroCount(2) == 1 && g.isoIntact(2) && t::isoBlocked(2) == 1, "PLACED, THE CAMERA UI: FreeCamToggleHud reads 0 and is back exactly; 1 blocked");
    check(g.isoSeenZeroCount(3) == 7 && g.isoIntact(3) && t::isoBlocked(3) == 7 && t::zoomCalls() == 1 && g.zoomCallsSeen() == 1,
          "PLACED, ZOOM/DOF: all 7 (zoom, aperture, focus holds; the advance-mode toggle) read 0 and are back exactly; 7 blocked; the update ran");
    g.freeUpdate(g.free);
    check(t::isoBlocked(0) == 22 && g.isoSeenZeroCount(0) == 11 && g.isoIntact(0), "...every update does it again (22 blocked after the second)");
    {
        // The held byte and the axis float: bit for bit, including a negative axis and a held byte with the high bit set.
        uint8_t* axisObj = g.isoObject(0, 2);
        uint8_t* heldObj = g.isoObject(0, 0);   // +0x4B8, a held byte
        const uint32_t negative = 0xBF4CCCCDu;   // -0.8
        std::memcpy(axisObj + 0x18, &negative, 4);
        axisObj[0x24] = 0xFF;
        axisObj[0x1C] = 0x7F;
        heldObj[0x25] = 0xAA;
        heldObj[0x26] = 0xBB;
        g.freeUpdate(g.free);
        uint32_t back = 0;
        std::memcpy(&back, axisObj + 0x18, 4);
        check(g_seenIso[0].v[2] == 0 && back == negative && axisObj[0x24] == 0xFF && axisObj[0x1C] == 0x7F, "...a negative axis (-0.8), a held byte of 0xFF and a pressed int are restored BIT FOR BIT");
        check(g_seenIso[0].v[0] == 0 && g_seenIso[0].nb[0] == 0x00BBAAu && heldObj[0x25] == 0xAA && heldObj[0x26] == 0xBB && heldObj[0x24] == isoPatternHeld(0x4B8),
              "...a held field is ONE BYTE: the bytes beside it (+0x25, +0x26) are untouched inside the original and after");
    }

    // ---- nothing but the listed handles is touched ----------------------------------------------------------------------------------------------------------
    {
        struct Canary {
            uint8_t* holder;
            uint32_t offset;
        };
        // Unlisted slots next to the listed ones: the game keeps other pointers there, and an object behind them must come through the update unchanged.
        const Canary canaries[4] = {{g.free, 0x4A8}, {g.ctl, 0x330}, {g.ui, 0x1E0}, {g.zoom, 0x288}};
        bool all = true;
        for (int h = 0; h < 4; ++h) {
            uint8_t* obj = g.iso[36 + h];
            const uint32_t v = 0x4141u + h, held = 0x70u + h;
            std::memcpy(obj + 0x18, &v, 4);
            std::memcpy(obj + 0x1C, &v, 4);
            obj[0x24] = static_cast<uint8_t>(held);
            const uint64_t ptr = reinterpret_cast<uint64_t>(obj);
            std::memcpy(canaries[h].holder + canaries[h].offset, &ptr, 8);
            g_canaryObj[h] = obj;
        }
        runAll();
        for (int h = 0; h < 4; ++h) {
            const uint8_t* obj = g.iso[36 + h];
            uint32_t a = 0, b = 0;
            std::memcpy(&a, obj + 0x18, 4);
            std::memcpy(&b, obj + 0x1C, 4);
            all = all && a == 0x4141u + h && b == 0x4141u + h && obj[0x24] == 0x70u + h && g_seenIso[h].canary == 0x4141u + h;
        }
        check(all, "NO COLLATERAL: an action object behind an UNLISTED slot (free +0x4A8, controller +0x330, UI +0x1E0, zoom +0x288) keeps all three fields, inside the original too");
        for (const uint8_t*& c : g_canaryObj) c = nullptr;
    }

    // ---- EDVR's own presses go in AFTER the clear ------------------------------------------------------------------------------------------------------------
    begin();
    g.setMode(3);
    g.free[0x473] = 1;
    g.ctlFrame(); rig.boundary(); g.ctlFrame(); rig.boundary();
    rig.boundary(true);
    g.ctlFrame();
    g.freeFrame();   // seeds from the preset
    Game::pressed(g.lockAction) = 7;   // the player presses "lock relative" at the very update EDVR places and presses it too
    g.freeFrame();
    check(g.seenLock() == 1 && Game::pressed(g.lockAction) == 7 && g.mode() == 4 && t::placedActivity() == reinterpret_cast<uint64_t>(g.free),
          "THE LOCK PRESS SURVIVES THE CLEAR: with the player's own value (7) in the lock action, the update saw EDVR's press (1), the player's value is back after, the camera locked (mode 4)");
    Game::pressed(g.lockAction) = 0;
    g.ctlFrame();
    Game::pressed(g.hideAction) = 4;   // the player presses "hide the camera UI" when EDVR does
    g.uiFrame();
    check(g.seenHide() == 1 && Game::pressed(g.hideAction) == 4 && g.hidden() == 1, "THE UI HIDE PRESS SURVIVES THE CLEAR: the update saw EDVR's press (1), the player's value (4) is back after, the UI is hidden");
    g.uiFrame();
    check(g.seenHide() == 0 && Game::pressed(g.hideAction) == 4 && g.hidden() == 1, "...and the player's own press alone does nothing: the UI stays hidden (the update read 0), the player's value is back after");
    // The controller: the player holds the camera key; it does nothing until EDVR's own exit.
    g.ctlFrame();
    check(g.mode() == 4, "(the free camera is locked: mode 4)");
    Game::pressed(g.photoAction) = 9;
    g.ctlFrame(); g.ctlFrame(); g.ctlFrame();
    check(g.mode() == 4 && t::sessionActive() && g.seenPhoto() == 0, "THE PLAYER'S CAMERA KEY, held for three updates: nothing happens, the camera stays open in mode 4");
    exitSession();
    check(g.mode() == 0 && !t::sessionActive() && Game::pressed(g.photoAction) == 9,
          "EDVR'S OWN EXIT PRESS SURVIVES THE CLEAR: with the player holding the camera key (9), F5 still closes the camera, the session ends, and the player's value is back after");

    // ---- after the session, and with the hotkey cleared: nothing is blocked ----------------------------------------------------------------------------------------
    {
        const uint64_t before = t::isoBlocked(0) + t::isoBlocked(1) + t::isoBlocked(2) + t::isoBlocked(3);
        fillAll();
        runAll();
        bool after = true;
        for (int h = 0; h < 4; ++h) after = after && g.isoSeenPatternCount(h) == kExpCount[h] && g.isoIntact(h);
        check(after && t::isoBlocked(0) + t::isoBlocked(1) + t::isoBlocked(2) + t::isoBlocked(3) == before, "AFTER THE SESSION: every key reaches the game again, nothing more is counted");
    }
    begin();
    place();
    rig.f.hotkey = "";
    rig.boundary();
    check(!t::placeActive() && !t::zoomGateOpen(), "(the hotkey is cleared: Explorer Cam is off, the zoom hook's gate is closed)");
    g.uiFrame();   // EDVR gives the camera UI back through its open gate (its own press), as it did with the old key
    g.uiFrame();
    rig.boundary();
    check(g.hidden() == 0 && !t::uiHiddenByEdvr(), "(EDVR gave the camera UI back)");
    fillAll();
    runAll();
    bool offOk = true;
    for (int h = 0; h < 4; ++h) offOk = offOk && g.isoSeenPatternCount(h) == kExpCount[h] && g.isoIntact(h) && t::isoBlocked(h) == 0;
    check(offOk, "THE HOTKEY CLEARED mid-session: nothing is blocked from the next update (the camera suite is the game's again)");

    // ---- the session ends (the game closes the camera) before the frame thread has withdrawn the placement: nothing is blocked from that update on ----------------------
    begin();
    place();
    fillAll();
    g.setMode(0);
    g.ctlUpdate(g.ctl);   // the controller sees mode 0: the session is over
    check(!t::sessionActive() && t::placedActivity() != 0 && g.isoSeenPatternCount(1) == 15, "THE SESSION ENDS under a standing placement: the very update that ended it already sees all 15 controller keys");
    g.zoomUpdate(g.zoom);
    check(g.isoSeenPatternCount(3) == 7 && t::isoBlocked(3) == 0, "...and the zoom/DOF update that follows, before any frame boundary, sees its 7 keys: blocking is a property of the SESSION");
    begin();
    place();
    fillAll();
    t::forcePlaceActive(false);   // Explorer Cam stood down under the hook threads' feet; the frame thread has not withdrawn the placement yet
    g.zoomUpdate(g.zoom);
    check(g.isoSeenPatternCount(3) == 7 && t::isoBlocked(3) == 0, "EXPLORER CAM STOOD DOWN under a standing session: the zoom/DOF update sees its 7 keys (nothing is blocked when the feature is not active)");

    // ---- the zoom/DOF handles: any may be null; a bad pointer is not an object; handles are read again on every call ------------------------------------
    begin();
    place();
    g.isoFill(3);
    uint8_t* const o1 = g.isoObject(3, 1);   // +0x258
    uint8_t* const o3 = g.isoObject(3, 3);   // +0x268
    uint8_t* const o4 = g.isoObject(3, 4);   // +0x270
    uint8_t* const o5 = g.isoObject(3, 5);   // +0x278
    const uint64_t zero = 0, tiny = 0x1234, odd = reinterpret_cast<uint64_t>(o5) + 1;
    std::memcpy(g.zoom + 0x258, &zero, 8);
    std::memcpy(g.zoom + 0x270, &zero, 8);
    std::memcpy(g.zoom + 0x268, &tiny, 8);
    std::memcpy(g.zoom + 0x278, &odd, 8);
    g.zoomUpdate(g.zoom);
    check(g_seenIso[3].v[0] == 0 && g_seenIso[3].v[2] == 0 && g_seenIso[3].v[6] == 0 && g_seenIso[3].v[1] == 0xFFFFFFFFu && g_seenIso[3].v[4] == 0xFFFFFFFFu && t::isoBlocked(3) == 3 &&
              g.zoomCallsSeen() == 1,
          "ZOOM/DOF WITH NULL HANDLES (+0x258, +0x270), an implausible one (+0x268 = 0x1234) and a misaligned one (+0x278): no crash, only the three real objects are cleared (3 blocked)");
    check(o1[0x24] == isoPatternHeld(0x258) && o3[0x24] == isoPatternHeld(0x268) && o4[0x24] == isoPatternHeld(0x270) && o5[0x24] == isoPatternHeld(0x278),
          "...the objects behind the null, implausible and misaligned handles were not touched");
    std::memcpy(g.zoom + 0x278, &o5, 8);
    uint8_t* const fresh = g.iso[39];
    std::memset(fresh, 0, 0x40);
    fresh[0x24] = 0x55;
    uint8_t* const old0 = g.isoObject(3, 0);
    const uint64_t freshPtr = reinterpret_cast<uint64_t>(fresh);
    std::memcpy(g.zoom + 0x250, &freshPtr, 8);   // the binder re-resolved the handle to another object
    g.zoomUpdate(g.zoom);
    check(g_seenIso[3].v[0] == 0 && fresh[0x24] == 0x55 && old0[0x24] == isoPatternHeld(0x250), "HANDLES ARE READ AGAIN ON EVERY CALL: a handle re-pointed to another object clears THAT object (and puts its 0x55 back), and the old one is left alone");

    // ---- a clear that faults stands the isolation down after three, never the placement -----------------------------------------------------------------------
    begin();
    place();
    fillAll();
    uint8_t* const f0 = g.isoObject(0, 0);   // +0x4B8, cleared before the fault
    uint8_t* const f1 = g.isoObject(0, 1);   // +0x4C0
    const uint64_t unmapped = 0x00006FFF00000000ull;
    const uint64_t goodPtr = reinterpret_cast<uint64_t>(g.isoObject(0, 2));
    std::memcpy(g.free + 0x4C8, &unmapped, 8);
    g_probeFree = nullptr;
    const uint64_t updatesBefore = t::updatesPlaced();
    g.freeUpdate(g.free);
    check(t::isoFaults() == 1 && !t::isoDown() && f0[0x24] == isoPatternHeld(0x4B8) && f1[0x24] == isoPatternHeld(0x4C0) && t::faults() == 0,
          "A FAULT IN THE CLEAR (a handle to unmapped memory): counted as an isolation fault, what was cleared before it is PUT BACK, the placement's own fault budget untouched");
    g.freeUpdate(g.free);
    g.freeUpdate(g.free);
    rig.boundary();
    check(t::isoFaults() == 3 && t::isoDown() && rig.cap.count("isolation stood down for the session: 3 guarded accesses") == 1 && t::placeActive() && t::phase() == 2 && t::faults() == 0 &&
              t::updatesPlaced() >= updatesBefore + 3,
          "THREE FAULTS stand the isolation down (said once); the placement goes on, still placed, its budget untouched, the pose still written");
    std::memcpy(g.free + 0x4C8, &goodPtr, 8);
    g_probeFree = &probeFreeCb;
    fillAll();
    g.freeUpdate(g.free);
    check(g.isoSeenPatternCount(0) == 11 && g.isoIntact(0), "...from then on the camera's own keys reach the game's update again");

    // ---- a restore that faults is counted, not fatal ---------------------------------------------------------------------------------------------------------
    begin();
    place();
    fillAll();
    g_protectPage = static_cast<uint8_t*>(VirtualAlloc(nullptr, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (g_protectPage) {
        uint8_t* obj = g_protectPage + 0x100;
        const uint32_t pat = 0x2468;
        std::memcpy(obj + 0x18, &pat, 4);
        const uint64_t ptr = reinterpret_cast<uint64_t>(obj);
        std::memcpy(g.free + 0x4C8, &ptr, 8);
        g_probeFree = &probeFreeProtect;
        g.freeUpdate(g.free);
        DWORD old = 0;
        VirtualProtect(g_protectPage, 4096, PAGE_READWRITE, &old);
        check(t::isoFaults() == 1 && !t::isoDown() && t::faults() == 0 && t::placeActive(), "A RESTORE THAT FAULTS (the object's page went read-only inside the update): counted as one isolation fault, no crash, the placement untouched");
        g_probeFree = &probeFreeCb;
        VirtualFree(g_protectPage, 0, MEM_RELEASE);
        g_protectPage = nullptr;
    } else {
        check(false, "(a page for the restore-fault cell)");
    }

    // ---- the lines: once per session start, and the heartbeat ----------------------------------------------------------------------------------------------------
    begin();
    check(rig.cap.count("camera suite isolated") == 0, "(no session: no isolation line)");
    place();
    check(rig.cap.count("explorer cam: camera suite isolated: 34 actions blocked across 4 holders (free camera 11, camera controller 15, camera UI 1, zoom/DOF 7)") == 1 &&
              has(rig.cap.nth("camera suite isolated", 0), "until you press F5 again"),
          "THE SESSION LINE, once at the session start: 'camera suite isolated: 34 actions blocked across 4 holders'");
    fillAll();
    runAll();
    rig.advance(5200);
    rig.cap.clear();
    rig.boundary();
    {
        const std::string beat = rig.cap.nth("explorer cam: heartbeat (follow, isolation):", 0);
        check(has(beat, "blocked_presses(free_camera=11(+11) controller=15(+15) camera_ui=1(+1) zoom=7(+7))") && has(beat, "zoom_hook=armed zoom_hook_calls=") && has(beat, "isolation=on") &&
                  has(beat, "isolation_faults=0"),
              "THE HEARTBEAT counts the blocked presses per holder in the window (11, 15, 1, 7: only the fields that were non-zero when zeroed), the zoom hook's calls and the isolation's state");
    }
    runAll();
    rig.advance(5200);
    rig.cap.clear();
    rig.boundary();
    check(has(rig.cap.nth("explorer cam: heartbeat (follow, isolation):", 0), "blocked_presses(free_camera=22(+11) controller=30(+15) camera_ui=2(+1) zoom=14(+7))"),
          "...and the next window counts its own (+11, +15, +1, +7 on top of the totals)");
    exitSession();
    rig.cap.clear();
    Game::pressed(g.photoAction) = 0;   // the player let go of the camera key
    place();
    check(rig.cap.count("explorer cam: camera suite isolated: 34 actions blocked across 4 holders") == 1, "A SECOND SESSION says it again");

    // ---- the zoom/DOF hook cannot install: the rest is isolated, and the line says so -----------------------------------------------------------------------------
    begin(true, true);
    check(t::stolenBytes(7) == 0 && rig.cap.count("zoom/DOF hook stood down:") == 1 && has(rig.cap.nth("zoom/DOF hook stood down:", 0), "the game build differs") &&
              has(rig.cap.nth("zoom/DOF hook stood down:", 0), "nothing was patched") && has(rig.cap.nth("zoom/DOF hook stood down:", 0), "zoom, aperture and focus keys are not blocked") && t::placeActive(),
          "A WRONG ZOOM/DOF PROLOGUE: one stand-down line saying the build differs and nothing was patched, and Explorer Cam still runs");
    place();
    fillAll();
    runAll();
    check(g.isoSeenZeroCount(0) == 11 && g.isoSeenZeroCount(1) == 15 && g.isoSeenZeroCount(2) == 1 && g.isoSeenPatternCount(3) == 7 && t::zoomCalls() == 0,
          "...the free camera, the controller and the camera UI are still isolated; the zoom keys are not, and the hook saw no call");
    check(rig.cap.count("explorer cam: camera suite isolated: 27 actions blocked across 3 holders") == 1 && has(rig.cap.nth("camera suite isolated", 0), "zoom/DOF hook is not in place"),
          "...and the session line says 27 actions across 3 holders and why the zoom keys are not blocked");
    begin(false);   // no target for it at all: the real module, which is not Elite
    place();
    fillAll();
    runAll();
    check(g.isoSeenZeroCount(0) == 11 && g.isoSeenPatternCount(3) == 7, "(an unknown build for the zoom hook is the same)");

    g_probeFree = g_probeCtl = g_probeUi = g_probeZoom = nullptr;
    t::reset();
    check(std::memcmp(p.zoomG, p.zoomGood.data(), 6) == 0, "RESET put the zoom/DOF function's original bytes back");
}

// The pure rule behind it.
void testHotkeyKeeper() {
    std::printf("the hotkey that is also the exit: a change or a clearing waits for the session to end (pure)\n");
    ecm::HotkeyKeeper k;
    bool deferred = true;
    check(k.step("F5", false, &deferred) == "F5" && !deferred, "the first value is taken as it is");
    check(k.step("F5", true, &deferred) == "F5" && !deferred, "UNCHANGED during a session: nothing to say");
    check(k.step("", true, &deferred) == "F5" && deferred && k.pending().empty(), "CLEARED during a session: the old key is kept, deferred once");
    check(k.step("", true, &deferred) == "F5" && !deferred, "...and not said again");
    check(k.step("", false, &deferred) == "" && !deferred, "...the session over: the cleared value applies (Explorer Cam disarms)");
    check(k.step("F6", false, &deferred) == "F6" && !deferred, "a change with no session applies at once");
    check(k.step("F7", true, &deferred) == "F6" && deferred && k.pending() == "F7", "CHANGED during a session: the old key is kept and the new value is named");
    check(k.step("F8", true, &deferred) == "F6" && deferred && k.pending() == "F8", "...another change is a new deferral");
    check(k.step("F6", true, &deferred) == "F6" && !deferred && k.pending().empty(), "...a change back to the key in force clears it");
    check(k.step("F7", true, &deferred) == "F6" && deferred, "...a new one is held back again");
    check(k.step("F7", false, &deferred) == "F7" && !deferred && k.pending().empty(), "...the session over: F7 applies");
}


// ================================ Phase 4: the comfort fade (the timeline, the signal) ================================

// The timeline driven by hand: a clock in 10 ms steps, the facts as plain fields.
struct TlRig {
    ecm::ComfortTimeline tl;
    ecm::ComfortInputs in;
    uint64_t us = 5000000;
    std::vector<ecm::ComfortLine> events;
    uint32_t releasedEnter = 0, releasedExit = 0;
    float alpha = 0.0f;
    uint32_t ticks = 0, firstBlackTick = 0, releaseTick = 0;
    TlRig() {
        in.active = true;
        in.ctlCalls = 100;
    }
    ecm::ComfortStep tick(uint32_t ms = 10) {
        us += static_cast<uint64_t>(ms) * 1000;
        in.nowUs = us;
        const ecm::ComfortStep s = tl.step(in);
        in.pressEnter = in.pressExit = false;
        for (uint8_t i = 0; i < s.nev; ++i) events.push_back(s.ev[i]);
        releasedEnter += s.releaseEnter ? 1u : 0u;
        releasedExit += s.releaseExit ? 1u : 0u;
        alpha = s.alpha;
        ++ticks;
        if (alpha == 1.0f && !firstBlackTick) firstBlackTick = ticks;
        if ((s.releaseEnter || s.releaseExit) && !releaseTick) releaseTick = ticks;
        return s;
    }
    void run(uint32_t ms) { for (uint32_t t = 0; t < ms; t += 10) tick(10); }
    size_t count(ecm::ComfortEv ev) const {
        size_t n = 0;
        for (const auto& e : events) n += e.ev == ev ? 1 : 0;
        return n;
    }
    const ecm::ComfortLine* find(ecm::ComfortEv ev, size_t nth = 0) const {
        for (const auto& e : events)
            if (e.ev == ev && nth-- == 0) return &e;
        return nullptr;
    }
    // F5 on foot with the camera closed, up to and including the moment the request goes out. Returns the ms it took.
    uint32_t pressEnterUntilReleased() {
        in.pressEnter = true;
        uint32_t ms = 0;
        tick(10);
        while (!releasedEnter && ms < 1000) { tick(10); ms += 10; }
        return ms;
    }
    // The controller took the request, the session is on, and the placement came: everything the fade-in waits for.
    void goodEntry() {
        in.sessionActive = true;
        in.requestPending = false;
        in.mode = 4;
        in.placed = true;
        in.state = ecm::kStateRelativeLock;
        in.steady = ecm::kFadeSteadyUpdates;
        in.uiSettled = true;
    }
};

void testComfortTimeline() {
    std::printf("the comfort fade's timeline: enter, exit, re-attach, each timeout, the aborts (pure)\n");
    static_assert(ecm::kFadeOutMs == 200 && ecm::kFadeInMs == 300 && ecm::kFadeReattachOutMs == 100 && ecm::kFadeReattachInMs == 200 && ecm::kFadeMaxBlackMs == 3000 &&
                      ecm::kFadeSteadyUpdates == 10 && ecm::kFadeClosedUpdates == 10,
                  "the durations Sean's brief names: 200 out, 300 in, 100/200 for a re-attach, 3 s at most, 10 updates");
    check(ecm::kFadeSteadyMetres == 0.02f, "...and the eye is steady under 2 cm an update");

    // ---- idle ---------------------------------------------------------------------------------------------------------------------------------------------------
    {
        TlRig r;
        r.run(500);
        check(r.alpha == 0.0f && r.events.empty() && !r.tl.busy() && r.releasedEnter == 0, "IDLE: alpha is exactly 0, no event, no request released");
    }
    // ---- enter: out, release, hold, in ----------------------------------------------------------------------------------------------------------------------------
    {
        TlRig r;
        r.run(50);
        r.in.pressEnter = true;
        ecm::ComfortStep s = r.tick();
        check(r.count(ecm::ComfortEv::Start) == 1 && r.find(ecm::ComfortEv::Start)->kind == ecm::ComfortKind::Enter && r.tl.phase() == ecm::ComfortPhase::Out && s.alpha == 0.0f &&
                  !s.releaseEnter,
              "ENTER, F5: one Start event, the fade out begins from clear, and the request is NOT released with the press");
        std::string startLine;
        {
            char buf[ecm::kLineBytes];
            ecm::formatComfort(buf, sizeof(buf), *r.find(ecm::ComfortEv::Start));
            startLine = buf;
        }
        check(has(startLine.c_str(), "explorer cam: comfort fade: F5: fading to black over 200 ms BEFORE the camera opens") && has(startLine.c_str(), "3 s at most"),
              "...the line says it fades BEFORE the camera opens, over 200 ms, for 3 s at most");
        for (int i = 0; i < 9; ++i) s = r.tick();   // 90 ms in
        check(std::fabs(s.alpha - 0.425f) < 0.01f && r.releasedEnter == 0, "...90 ms in, the view is part dark (the smoothstep of 0.45 is 0.425, not the linear 0.45) and no press has been released");
        uint32_t ms = 90;
        while (!r.releasedEnter && ms < 600) { r.tick(); ms += 10; }
        check(r.releasedEnter == 1 && r.alpha == 1.0f && ms >= 200 && ms <= 230, "THE REQUEST IS RELEASED once the view is fully black, 200 ms after the press (a frame later at most), and never before");
        check(r.firstBlackTick != 0 && r.releaseTick > r.firstBlackTick, "...on a frame AFTER the first one that was fully black, so the black level has been published once before the first press can happen");
        check(r.tl.phase() == ecm::ComfortPhase::Black, "...and the view holds black");
        // ---- the hold: nothing placed yet -----------------------------------------------------------------------------------------------------------------
        r.in.sessionActive = true;
        r.in.mode = 1;
        r.run(500);
        check(r.alpha == 1.0f && r.tl.phase() == ecm::ComfortPhase::Black && r.releasedEnter == 1, "BLACK HOLDS through the open and the wait for TAB (500 ms, session on, nothing placed)");
        // ---- each condition alone keeps it black -------------------------------------------------------------------------------------------------------------------
        r.goodEntry();
        r.in.placed = false;
        r.run(200);
        check(r.alpha == 1.0f, "...NOT PLACED keeps it black even with the lock, the eye and the UI all fine");
        r.goodEntry();
        r.in.state = 3;
        r.run(200);
        check(r.alpha == 1.0f, "...THE LOCK NOT CONFIRMED (+0x48C = 3) keeps it black");
        r.goodEntry();
        r.in.steady = 9;
        r.run(200);
        check(r.alpha == 1.0f, "...AN EYE STEADY FOR ONLY 9 UPDATES keeps it black");
        r.goodEntry();
        r.in.uiSettled = false;
        r.run(200);
        check(r.alpha == 1.0f, "...THE CAMERA UI'S HIDE NOT SETTLED keeps it black");
        // ---- all met: fade in ----------------------------------------------------------------------------------------------------------------------------------
        r.goodEntry();
        const size_t before = r.events.size();
        s = r.tick();
        check(r.count(ecm::ComfortEv::FadeIn) == 1 && r.tl.phase() == ecm::ComfortPhase::In && r.events.size() == before + 1,
              "ALL FOUR MET (placed, lock 4, steady 10, UI settled): the fade in begins on that frame, one FadeIn event");
        char buf[ecm::kLineBytes];
        ecm::formatComfort(buf, sizeof(buf), *r.find(ecm::ComfortEv::FadeIn));
        check(has(buf, "entering: placed, locked, the camera UI hidden and the eye steady for 10 updates") && has(buf, "black lasted 1.") && has(buf, "fading in over 300 ms"),
              "...its line says what was met, how long black lasted, and the 300 ms");
        float prev = s.alpha;
        bool monotone = true;
        uint32_t inMs = 0;
        while (r.tl.phase() == ecm::ComfortPhase::In && inMs < 1000) {
            s = r.tick();
            monotone = monotone && s.alpha <= prev + 1e-6f;
            prev = s.alpha;
            inMs += 10;
        }
        check(monotone && inMs >= 290 && inMs <= 330 && s.alpha == 0.0f && r.count(ecm::ComfortEv::Cleared) == 1 && !r.tl.busy(),
              "THE FADE IN takes 300 ms, only ever lightens, ends at EXACTLY 0, says it is clear once, and the timeline is idle again");
        ecm::formatComfort(buf, sizeof(buf), *r.find(ecm::ComfortEv::Cleared));
        check(has(buf, "clear again; the view was dark for ") && has(buf, "s in all (entering)"), "...with a line giving the total dark time");
        r.run(300);
        check(r.alpha == 0.0f && r.events.size() == before + 2 && r.releasedEnter == 1, "...and nothing more happens (no second release, no stray event)");
    }
    // ---- a second F5 press while the entry is under way ----------------------------------------------------------------------------------------------------------
    {
        TlRig r;
        r.in.pressEnter = true;
        r.tick();
        r.run(80);
        const float mid = r.alpha;
        r.in.pressEnter = true;
        r.tick();
        check(r.count(ecm::ComfortEv::Cancelled) == 1 && r.tl.phase() == ecm::ComfortPhase::In && r.alpha <= mid + 0.1f, "A SECOND F5 BEFORE THE REQUEST WENT: the entry is cancelled and the view fades back in from where it stands");
        r.run(600);
        check(r.releasedEnter == 0 && r.alpha == 0.0f && !r.tl.busy() && r.count(ecm::ComfortEv::Cleared) == 1, "...the request is never released and the view ends clear");
        char buf[ecm::kLineBytes];
        ecm::formatComfort(buf, sizeof(buf), *r.find(ecm::ComfortEv::Cancelled));
        check(has(buf, "pressed again before the camera opened") && has(buf, "cancelled"), "...with a line");
        r.in.pressEnter = true;
        r.tick();
        check(r.count(ecm::ComfortEv::Start) == 2, "(and a new F5 starts a new entry)");
    }
    {
        TlRig r;   // the controller has not taken the request yet: no session, the request still pending, nothing else true
        r.pressEnterUntilReleased();
        r.goodEntry();
        r.in.sessionActive = false;
        r.in.requestPending = true;
        r.run(300);
        check(r.alpha == 1.0f && r.count(ecm::ComfortEv::Aborted) == 0 && r.count(ecm::ComfortEv::FadeIn) == 0, "NO SESSION YET with the request still pending: black holds (every other fact true is not enough)");
        r.in.pressEnter = true;
        r.tick();
        check(r.count(ecm::ComfortEv::Start) == 1 && r.count(ecm::ComfortEv::Cancelled) == 0 && r.tl.phase() == ecm::ComfortPhase::Black, "A SECOND F5 AFTER THE REQUEST WENT is ignored (the entry is under way)");
    }
    // ---- timeouts -------------------------------------------------------------------------------------------------------------------------------------------------
    {
        TlRig r;
        const uint32_t releasedAfter = r.pressEnterUntilReleased();
        (void)releasedAfter;
        r.in.sessionActive = true;
        r.in.requestPending = false;
        r.in.mode = 3;
        r.in.placed = true;
        r.in.state = 3;     // never locks
        r.in.steady = 4;    // never steady
        r.in.uiSettled = false;
        uint32_t ms = 0;
        while (r.count(ecm::ComfortEv::TimedOut) == 0 && ms < 6000) { r.tick(); ms += 10; }
        const ecm::ComfortLine* t = r.find(ecm::ComfortEv::TimedOut);
        check(t && t->heldMs >= 3000 && t->heldMs <= 3030 && r.tl.phase() == ecm::ComfortPhase::In,
              "THE 3 s CAP: nothing is ever met, and at 3.0 s from the press the view fades in regardless (one TimedOut event)");
        check(t && (t->unmet & ecm::kComfortUnmetLock) && (t->unmet & ecm::kComfortUnmetSteady) && (t->unmet & ecm::kComfortUnmetUi) && !(t->unmet & ecm::kComfortUnmetPlaced) &&
                  !(t->unmet & ecm::kComfortUnmetSession),
              "...the event names exactly what was unmet: the lock, the steady eye, the UI hide (not the placement, not the session)");
        char buf[ecm::kLineBytes];
        if (t) ecm::formatComfort(buf, sizeof(buf), *t);
        check(t && has(buf, "black reached the 3 s cap, so the view fades in anyway") && has(buf, "the lock is not confirmed (+0x48C = 3, want 4)") && has(buf, "the eye is not steady (4 of 10 updates)") &&
                  has(buf, "the camera UI's hide has not settled") && !has(buf, "the view is not placed"),
              "...and the line says why in words, with the numbers");
        r.run(400);
        check(r.alpha == 0.0f && !r.tl.busy(), "...and the view is clear again 300 ms later: black never outlasts 3.3 s");
    }
    {
        TlRig r;
        r.in.sessionActive = true;   // a session whose request never came out of the dark: still bounded
        r.in.pressEnter = true;
        r.tick();
        uint32_t ms = 0;
        while (r.count(ecm::ComfortEv::TimedOut) == 0 && ms < 6000) { r.tick(); ms += 10; }
        check(r.find(ecm::ComfortEv::TimedOut) && r.find(ecm::ComfortEv::TimedOut)->heldMs <= 3030, "THE CAP HOLDS with the session on and nothing else true");
    }
    // ---- aborts: the request is not taken; the session ends under it -----------------------------------------------------------------------------------------------
    {
        TlRig r;
        r.pressEnterUntilReleased();
        r.in.requestPending = true;
        r.run(100);
        check(r.count(ecm::ComfortEv::Aborted) == 0 && r.alpha == 1.0f, "(the request is pending: the controller may still take it)");
        r.in.requestPending = false;   // dropped by the watchdog, or refused: no session ever came
        r.run(40);
        check(r.count(ecm::ComfortEv::Aborted) == 0, "(a few frames with neither a request nor a session are not yet a refusal)");
        r.run(60);
        const ecm::ComfortLine* a = r.find(ecm::ComfortEv::Aborted);
        check(a && a->why == ecm::ComfortWhy::NotTaken && r.tl.phase() == ecm::ComfortPhase::In, "A REQUEST NOBODY TOOK (no session ever, none pending): the view fades back in at once, well inside the cap");
        char buf[ecm::kLineBytes];
        if (a) ecm::formatComfort(buf, sizeof(buf), *a);
        check(a && has(buf, "F5's request was not taken or was refused") && has(buf, "fades back in at once"), "...with a line");
        r.run(400);
        check(r.alpha == 0.0f && !r.tl.busy(), "...and the view is clear");
    }
    {
        TlRig r;
        r.pressEnterUntilReleased();
        r.in.sessionActive = true;
        r.in.requestPending = false;
        r.in.mode = 1;
        r.run(200);
        r.in.sessionActive = false;   // the sequencer gave up (a timeout, a refusal) or the controller went silent
        r.run(10);
        check(r.count(ecm::ComfortEv::Aborted) == 0, "(one frame without the session is not yet an abort)");
        r.run(10);
        const ecm::ComfortLine* a = r.find(ecm::ComfortEv::Aborted);
        check(a && a->why == ecm::ComfortWhy::SessionEnded && r.tl.phase() == ecm::ComfortPhase::In, "THE SESSION ENDS UNDER THE ENTRY: the view fades back in at once, on the second frame without it");
        char buf[ecm::kLineBytes];
        if (a) ecm::formatComfort(buf, sizeof(buf), *a);
        check(a && has(buf, "the session ended under it") && has(buf, "Unmet: "), "...the line says so and names what was unmet");
    }
    // ---- Explorer Cam stands down, or the hotkey is cleared ----------------------------------------------------------------------------------------------------------
    {
        TlRig r;
        r.pressEnterUntilReleased();
        r.goodEntry();
        r.in.steady = 3;
        r.run(100);
        check(r.alpha == 1.0f, "(black, entering)");
        r.in.active = false;
        const ecm::ComfortStep s = r.tick();
        check(s.alpha == 0.0f && r.count(ecm::ComfortEv::Dropped) == 1 && !r.tl.busy(), "EXPLORER CAM STOOD DOWN (or the hotkey cleared) while black: the view is clear AT ONCE, one Dropped event");
        char buf[ecm::kLineBytes];
        ecm::formatComfort(buf, sizeof(buf), *r.find(ecm::ComfortEv::Dropped));
        check(has(buf, "stood down or its hotkey was cleared while the view was dark") && has(buf, "the view is clear at once"), "...with a line");
        r.run(100);
        check(r.alpha == 0.0f && r.count(ecm::ComfortEv::Dropped) == 1, "...and stays clear, with no second line");
        r.in.active = true;
        r.run(100);
        check(r.alpha == 0.0f && !r.tl.busy(), "...also when it comes back: nothing is dark until F5 asks");
    }
    // ---- exit -----------------------------------------------------------------------------------------------------------------------------------------------------
    {
        TlRig r;
        r.goodEntry();
        r.run(100);
        check(r.alpha == 0.0f && !r.tl.busy(), "(a session is on, placed, locked and the view is clear)");
        r.in.pressExit = true;
        ecm::ComfortStep s = r.tick();
        check(r.count(ecm::ComfortEv::Start) == 1 && r.find(ecm::ComfortEv::Start)->kind == ecm::ComfortKind::Exit && r.releasedExit == 0 && s.alpha == 0.0f,
              "EXIT, F5 in a session: one Start event, the fade out begins, the exit request is NOT released with the press");
        char buf[ecm::kLineBytes];
        ecm::formatComfort(buf, sizeof(buf), *r.find(ecm::ComfortEv::Start));
        check(has(buf, "BEFORE the camera's controls come back and the camera closes"), "...the line says the controls come back only after the view is dark");
        uint32_t ms = 0;
        while (!r.releasedExit && ms < 600) { r.tick(); ms += 10; }
        check(r.releasedExit == 1 && r.alpha == 1.0f && ms >= 190 && ms <= 230, "...the exit request goes out once the view is black, 200 ms after the press");
        r.in.pressExit = true;
        r.tick();
        check(r.count(ecm::ComfortEv::Start) == 1 && r.releasedExit == 1, "a second F5 while leaving is the same press (no second start, no second release)");
        r.run(500);
        check(r.alpha == 1.0f, "BLACK HOLDS while the camera is still open (mode 4)");
        r.in.mode = 0;
        r.in.sessionActive = false;
        r.in.placed = false;
        r.in.ctlCalls = 1000;
        r.tick();
        for (int i = 0; i < 9; ++i) { ++r.in.ctlCalls; r.tick(); }
        check(r.alpha == 1.0f && r.count(ecm::ComfortEv::FadeIn) == 0, "...it reads closed but only 9 controller updates have passed: still black");
        ++r.in.ctlCalls;
        r.tick();
        check(r.count(ecm::ComfortEv::FadeIn) == 1 && r.tl.phase() == ecm::ComfortPhase::In, "...the 10th update: the fade in begins");
        ecm::formatComfort(buf, sizeof(buf), *r.find(ecm::ComfortEv::FadeIn));
        check(has(buf, "leaving: the camera reads closed and 10 controller updates have passed") && has(buf, "fading in over 300 ms"), "...with its line");
        r.run(400);
        check(r.alpha == 0.0f && !r.tl.busy() && r.count(ecm::ComfortEv::Cleared) == 1, "...and the view is clear again");
    }
    {
        TlRig r;
        r.goodEntry();
        r.in.pressExit = true;
        r.tick();
        while (!r.releasedExit) r.tick();
        r.in.mode = 0;
        r.in.sessionActive = false;
        r.run(100);   // mode 0 but the controller is never called again (ctlCalls frozen)
        check(r.alpha == 1.0f, "EXIT: mode 0 with the controller silent does not count as closed");
        uint32_t ms = 100;
        while (r.count(ecm::ComfortEv::TimedOut) == 0 && ms < 6000) { r.tick(); ms += 10; }
        const ecm::ComfortLine* t = r.find(ecm::ComfortEv::TimedOut);
        char buf[ecm::kLineBytes];
        if (t) ecm::formatComfort(buf, sizeof(buf), *t);
        check(t && t->heldMs >= 3000 && t->heldMs <= 3030 && has(buf, "leaving: black reached the 3 s cap") && has(buf, "only 0 of 10 controller updates since it closed"),
              "EXIT TIMES OUT at 3 s too, and says how far the closed-camera count got");
    }
    {
        TlRig r;
        r.goodEntry();
        r.in.pressExit = true;
        r.tick();
        while (!r.releasedExit) r.tick();
        r.in.sessionActive = false;   // the exit sequence gave up with the camera still open
        r.in.mode = 4;
        r.run(30);
        check(r.count(ecm::ComfortEv::Aborted) == 0, "(three frames without the session, the camera still open: not yet an abort)");
        r.run(30);
        const ecm::ComfortLine* a = r.find(ecm::ComfortEv::Aborted);
        check(a && a->why == ecm::ComfortWhy::SessionEnded && r.tl.phase() == ecm::ComfortPhase::In, "EXIT ABORTED (the session is over and the camera is still open): the view fades in rather than wait out the cap");
    }
    {
        TlRig r;   // an exit pressed with no session to leave (it ended while the view faded): nothing is released
        r.goodEntry();
        r.in.pressExit = true;
        r.tick();
        r.in.sessionActive = false;
        r.in.mode = 0;
        r.run(400);
        check(r.releasedExit == 0, "EXIT when the session ended during the fade out: no request is released (there is nothing to leave)");
        r.in.ctlCalls += 20;
        r.run(30);
        check(r.count(ecm::ComfortEv::FadeIn) == 1, "...and the fade in follows the closed camera as before");
    }
    // ---- exit pressed while the entry is black: no dip --------------------------------------------------------------------------------------------------------------------
    {
        TlRig r;
        r.pressEnterUntilReleased();
        r.in.sessionActive = true;
        r.in.requestPending = false;
        r.in.mode = 1;
        r.run(100);
        r.in.pressExit = true;
        float lowest = 1.0f;
        r.tick();
        for (int i = 0; i < 30; ++i) {
            lowest = (std::min)(lowest, r.alpha);
            r.tick();
        }
        check(lowest == 1.0f && r.releasedExit == 1 && r.find(ecm::ComfortEv::Start, 1) && r.find(ecm::ComfortEv::Start, 1)->kind == ecm::ComfortKind::Exit,
              "EXIT PRESSED WHILE THE ENTRY IS STILL BLACK: the view never lightens, the exit request goes out on the next frame, a second Start (Exit) is logged");
    }
    // ---- re-attach ------------------------------------------------------------------------------------------------------------------------------------------------
    {
        TlRig r;
        r.goodEntry();
        r.run(100);
        check(!r.tl.busy(), "(placed and clear)");
        r.in.placed = false;       // a detach: the placement is released, the session goes on
        r.in.mode = 5;
        r.in.state = 5;
        r.tick();
        check(r.count(ecm::ComfortEv::Start) == 1 && r.find(ecm::ComfortEv::Start)->kind == ecm::ComfortKind::Reattach, "A DETACH with the session on: a Reattach starts the frame the placement goes");
        char buf[ecm::kLineBytes];
        ecm::formatComfort(buf, sizeof(buf), *r.find(ecm::ComfortEv::Start));
        check(has(buf, "the placement was released with the session still on (a detach): fading to black over 100 ms"), "...the line says 100 ms");
        r.run(40);
        check(r.alpha > 0.1f && r.alpha < 0.9f, "...40 ms in, the view is part dark (not instantly black)");
        r.run(70);
        check(r.alpha == 1.0f, "...fully black 110 ms after it began: the out is 100 ms, not the entry's 200");
        check(r.releasedEnter == 0 && r.releasedExit == 0, "...no F5 request is released for a re-attach");
        r.run(500);
        check(r.alpha == 1.0f, "BLACK HOLDS until the view is placed again");
        r.goodEntry();
        r.tick();
        check(r.count(ecm::ComfortEv::FadeIn) == 1, "...placed, locked, steady and the UI settled: the fade in begins");
        float seen = 1.0f;
        uint32_t inMs = 0;
        while (r.tl.phase() == ecm::ComfortPhase::In && inMs < 1000) {
            r.tick();
            if (inMs == 90) seen = r.alpha;
            inMs += 10;
        }
        check(inMs >= 190 && inMs <= 230 && seen > 0.3f && seen < 0.7f && r.alpha == 0.0f, "...over 200 ms (about half way at 100 ms), ending at exactly 0");
    }
    {
        TlRig r;
        r.goodEntry();
        r.run(100);
        r.in.placed = false;
        r.in.state = 5;
        r.tick();
        uint32_t ms = 0;
        while (r.count(ecm::ComfortEv::TimedOut) == 0 && ms < 6000) { r.tick(); ms += 10; }
        check(r.find(ecm::ComfortEv::TimedOut) && r.find(ecm::ComfortEv::TimedOut)->heldMs <= 3030 && (r.find(ecm::ComfortEv::TimedOut)->unmet & ecm::kComfortUnmetPlaced),
              "A RE-ATTACH that never places again: the 3 s cap, naming 'the view is not placed'");
    }
    {
        TlRig r;
        r.goodEntry();
        r.run(100);
        r.in.sessionActive = false;   // the session ended: the placement goes with it. That is no detach.
        r.in.placed = false;
        r.run(100);
        check(r.count(ecm::ComfortEv::Start) == 0 && r.alpha == 0.0f, "A PLACEMENT LOST WITH THE SESSION is not a detach: no fade starts");
    }
    {
        TlRig r;
        r.goodEntry();
        r.in.pressExit = true;
        r.tick();
        while (!r.releasedExit) r.tick();
        r.in.placed = false;
        r.run(100);
        check(r.count(ecm::ComfortEv::Start) == 1, "...nor during an exit (the placement goes when the session does)");
    }
    {
        TlRig r;   // the exit has faded back IN while the placement still stands; it goes a frame later with the session flag still up. That is the exit finishing.
        r.goodEntry();
        r.in.pressExit = true;
        r.tick();
        while (!r.releasedExit) r.tick();
        r.in.mode = 0;
        r.in.ctlCalls = 1000;
        r.tick();
        for (int i = 0; i < 10; ++i) {
            ++r.in.ctlCalls;
            r.tick();
        }
        check(r.count(ecm::ComfortEv::FadeIn) == 1 && r.tl.phase() == ecm::ComfortPhase::In && r.in.placed && r.in.sessionActive, "(the exit fades in with the placement and the session flag still up)");
        const float before = r.alpha;
        r.in.placed = false;
        r.tick();
        check(r.count(ecm::ComfortEv::Start) == 1 && r.tl.kind() == ecm::ComfortKind::Exit && r.tl.phase() == ecm::ComfortPhase::In && r.alpha <= before,
              "...the placement released now is the exit finishing, NOT a detach: no Reattach starts and the view keeps fading in");
        r.run(400);
        check(r.alpha == 0.0f && r.count(ecm::ComfortEv::Start) == 1 && r.count(ecm::ComfortEv::Cleared) == 1, "...and ends clear with the one Start");
    }
    // ---- an interrupted ramp turns round where it stands ----------------------------------------------------------------------------------------------------------------
    {
        TlRig r;
        r.pressEnterUntilReleased();
        r.goodEntry();
        r.tick();
        r.run(100);   // a third of the way through the fade in
        const float mid = r.alpha;
        check(mid > 0.1f && mid < 0.9f && r.tl.phase() == ecm::ComfortPhase::In, "(half way through a fade in)");
        r.in.pressExit = true;
        const ecm::ComfortStep s = r.tick();
        check(r.tl.phase() == ecm::ComfortPhase::Out && std::fabs(s.alpha - mid) < 0.2f && s.alpha >= mid - 0.01f, "F5 DURING A FADE IN: the ramp turns round from where it stands (no jump to clear or to black)");
    }
    // ---- the steady test --------------------------------------------------------------------------------------------------------------------------------------------------
    {
        ecm::Eye a, b;
        b.up = a.up + 0.019f;
        check(ecm::eyeStepMetres(a, b) < ecm::kFadeSteadyMetres, "THE STEP between two eyes: 1.9 cm is steady");
        b.up = a.up + 0.021f;
        check(ecm::eyeStepMetres(a, b) >= ecm::kFadeSteadyMetres, "...2.1 cm is not");
        b = a;
        b.up += 0.015f;
        b.forward += 0.015f;
        check(ecm::eyeStepMetres(a, b) > ecm::kFadeSteadyMetres - 0.001f, "...and the three axes combine (1.5 cm in two of them is 2.12 cm)");
    }
}

void testComfortSignal() {
    std::printf("the comfort fade's signal: fresh, stale, never published, garbage (pure)\n");
    namespace cf = edvr::comfort;
    const uint64_t t0 = 1000000;
    check(cf::effective(0, t0) == 0.0f, "NEVER PUBLISHED reads 0");
    uint64_t w = cf::pack(1.0f, t0);
    check(cf::effective(w, t0) == 1.0f && cf::effective(w, t0 + 100) == 1.0f && cf::effective(w, t0 + 200) == 1.0f, "A FRESH LEVEL reads as published, up to 200 ms old");
    check(std::fabs(cf::effective(w, t0 + 250) - 0.5f) < 1e-5f, "...a stale one decays: 250 ms old reads half");
    check(cf::effective(w, t0 + 300) == 0.0f && cf::effective(w, t0 + 10000) == 0.0f, "...and is GONE at 300 ms: the user is never left in black by a silent publisher");
    w = cf::pack(0.6f, t0);
    check(std::fabs(cf::effective(w, t0) - 0.6f) < 1e-6f && std::fabs(cf::effective(w, t0 + 275) - 0.15f) < 1e-5f, "A PARTIAL LEVEL decays proportionally (0.6 at 275 ms reads 0.15)");
    check(cf::effective(cf::pack(0.0f, t0), t0) == 0.0f, "a published 0 reads 0");
    check(cf::effective(cf::pack(std::nanf(""), t0), t0) == 0.0f && cf::effective(cf::pack(-1.0f, t0), t0) == 0.0f, "NaN and a negative level read 0, never black");
    check(cf::effective(cf::pack(7.0f, t0), t0) == 1.0f && cf::effective(cf::pack(std::numeric_limits<float>::infinity(), t0), t0) == 1.0f, "a level above 1 reads 1 (clamped)");
    check(cf::effective(cf::pack(1.0f, t0 + 5), t0) == 1.0f, "a level published a moment AFTER the reader took its time reads fresh");
    const uint64_t wrap = 0x7FFFFFFFull - 50;
    check(cf::effective(cf::pack(1.0f, wrap), wrap + 100) == 1.0f && cf::effective(cf::pack(1.0f, wrap), wrap + 400) == 0.0f, "...also across the 31-bit millisecond wrap");
    check(cf::sanitize(0.5f) == 0.5f && cf::sanitize(-0.0f) == 0.0f && cf::sanitize(1.0f) == 1.0f && cf::sanitize(std::nanf("")) == 0.0f, "sanitize: in range is itself, NaN is 0");
    cf::clear();
    check(cf::read(t0) == 0.0f, "the shared word reads 0 once cleared");
    cf::publish(1.0f, t0);
    check(cf::read(t0 + 50) == 1.0f && cf::read(t0 + 400) == 0.0f, "...publish then read: fresh 1, stale 0");
    cf::clear();
}

// ================================ Phase 4, glue: the comfort fade around F5, end to end ================================

void testComfortGlue(const Pages& p) {
    std::printf("glue: the comfort fade around F5 (the request held until black, black held through placement, the signal, the aborts)\n");
    namespace t = edvr::explorercamtest;
    static Game g;
    Rig rig;
    auto begin = [&]() {
        t::reset();
        g.init();
        rig = Rig();
        installAll(p, rig, g);
        rig.f.comfortFade = true;
        g.ctlFrame();
        rig.boundary();
        g.ctlFrame();
        rig.boundary();
    };
    // Boundaries until `cond`, at 16 ms each; the most it will wait is `limit`. Returns how many it took.
    auto until = [&](auto cond, int limit) {
        int n = 0;
        while (!cond() && n < limit) {
            rig.boundary();
            ++n;
        }
        return n;
    };
    // The controller and the camera through F5's whole entry (as testF5FromClosed plays it), with the request already out.
    auto openAndPlace = [&]() {
        g.ctlFrame();                  // PhotoCameraToggle: the camera opens
        g.ctlFrame();
        g.ctlFrames(3);
        g.ctlFrame();                  // ToggleFreeCam
        g.freeFrame();                 // the free camera's first update
        g.ctlFrame();
        g.freeFrame();                 // placed, the lock pressed
    };
    auto linesWith = [&](const char* needle) { return rig.cap.count(needle); };

    // ---- the request waits for the dark --------------------------------------------------------------------------------------------------------------------------
    begin();
    rig.cap.clear();
    rig.boundary(true);
    check(t::f5Request() == 0 && rig.cap.count("F5 pressed: entering Explorer Cam") == 1 && linesWith("comfort fade: F5: fading to black over 200 ms BEFORE the camera opens") == 1,
          "F5 WITH THE FADE: pressed, the entry is announced and so is the fade out -- and the ENTER request is NOT set");
    g.ctlFrame();
    check(g.seenPhoto() == 0 && g.mode() == 0 && !t::sessionActive(), "...the controller's next update presses NOTHING: the camera does not open while the view is still visible");
    float previous = t::fadeAlpha();
    bool rising = true;
    int n = 0;
    while (t::f5Request() == 0 && n < 40) {
        rig.boundary();
        rising = rising && t::fadeAlpha() >= previous;
        previous = t::fadeAlpha();
        ++n;
        if (t::f5Request() == 0) {
            g.ctlFrame();
            if (g.seenPhoto() != 0) rising = false;
        }
    }
    check(rising && n >= 13 && n <= 15 && t::f5Request() == 1 && t::fadeAlpha() == 1.0f, "...the view darkens frame by frame and the request goes out after about 200 ms (13 to 15 boundaries of 16 ms), when the view is fully black");
    check(t::comfortRead(rig.ms) == 1.0f, "THE SIGNAL the runtime reads is that level: 1.0, fresh");
    check(t::comfortRead(rig.ms + 250) > 0.4f && t::comfortRead(rig.ms + 250) < 0.6f && t::comfortRead(rig.ms + 350) == 0.0f,
          "...and if nobody publishes again it decays: about half at 250 ms and exactly 0 at 350 ms (a silent d3d11 half never leaves the user in black)");
    g.ctlFrame();
    check(g.seenPhoto() == 1 && g.mode() == 1 && t::sessionActive(), "THE FIRST PRESS (PhotoCameraToggle) happens only now, in the dark");

    // ---- black holds through the open, TAB, placement, lock and the UI hide -------------------------------------------------------------------------------------------
    g.ctlFrame();
    rig.boundary();
    check(t::fadeAlpha() == 1.0f, "BLACK HOLDS while the camera is open on its preset");
    g.ctlFrames(3);
    g.ctlFrame();
    g.freeFrame();
    g.ctlFrame();
    rig.boundary();
    check(t::fadeAlpha() == 1.0f && t::phase() == 1, "...through TAB and the free camera's first update (waiting, nothing placed)");
    g.freeFrame();   // placed, the lock pressed
    for (int i = 0; i < 4; ++i) g.freeFrame();
    rig.boundary();
    check(t::phase() == 2 && t::steadyUpdates() >= 4 && t::steadyUpdates() < 10 && t::fadeAlpha() == 1.0f, "...through the placement: placed, the eye steady for fewer than 10 updates: still black");
    check(!t::uiSettled(), "(the camera UI's hide has not run yet)");
    for (int i = 0; i < 12; ++i) g.freeFrame();
    rig.boundary();
    check(t::steadyUpdates() >= 10 && g.mode() == 4 && t::fadeAlpha() == 1.0f, "...the eye steady for 10 updates and the camera locked (mode 4), but the UI's hide has not run: STILL BLACK");
    g.uiFrame();
    rig.boundary();
    check(t::fadeAlpha() == 1.0f && !t::uiSettled(), "...the hide press is out but its result is not read yet: black");
    g.uiFrame();
    check(t::uiSettled(), "(the next UI update reads it: settled)");
    rig.cap.clear();
    rig.boundary();
    check(linesWith("comfort fade: entering: placed, locked, the camera UI hidden and the eye steady for 10 updates") == 1, "ONLY NOW the fade in begins: one line says what was met");
    check(t::fadeAlpha() == 1.0f, "(that frame still shows black: the ramp's first step comes with the next)");
    rig.boundary();
    const float first = t::fadeAlpha();
    rig.boundary();
    check(first < 1.0f && first > 0.9f && t::fadeAlpha() < first, "...then the view lightens a little each frame");
    check(until([&] { return t::fadeAlpha() == 0.0f; }, 60) >= 15, "...and takes about 300 ms (18 to 20 boundaries) to be clear");
    check(t::fadeAlpha() == 0.0f && linesWith("comfort fade: clear again; the view was dark for ") == 1 && t::comfortRead(rig.ms) == 0.0f, "...ends at exactly 0, with a line giving the total dark time, and the signal reads 0");
    check(t::phase() == 2 && t::sessionActive(), "(the session is on and the view is placed)");

    // ---- eye steadiness: a 5 cm step resets the count, a 1.9 cm step does not ----------------------------------------------------------------------------------------
    g.freeFrame();
    g.freeFrame();
    const uint32_t steadyBefore = t::steadyUpdates();
    rig.f.up = 1.73f;
    rig.boundary();
    g.freeFrame();
    check(t::steadyUpdates() == 0, "THE EYE MOVES 5 cm in one update (the fixed eye key 1.68 to 1.73): the steady count starts over");
    g.freeFrame();
    g.freeFrame();
    check(t::steadyUpdates() == 2 && steadyBefore >= 10, "...and counts again from there");
    rig.f.up = 1.749f;
    rig.boundary();
    g.freeFrame();
    check(t::steadyUpdates() == 3, "A 1.9 cm step is still steady (the count goes on)");
    rig.f.up = 1.68f;
    rig.boundary();
    g.freeFrame();

    // ---- exit: dark first, then the controls come back and the camera closes ---------------------------------------------------------------------------------------------
    rig.cap.clear();
    rig.boundary(true);
    check(t::f5Request() == 0 && linesWith("F5 pressed: leaving Explorer Cam") == 1 && linesWith("comfort fade: F5: fading to black over 200 ms BEFORE the camera's controls come back") == 1,
          "EXIT, F5: announced, and the EXIT request is NOT set while the view is still visible");
    g.ctlFrame();
    g.uiFrame();
    check(g.hidden() == 1 && g.mode() == 4 && g.seenPhoto() == 0, "...nothing happens to the camera or its UI yet");
    until([&] { return t::f5Request() == 2; }, 40);
    check(t::f5Request() == 2 && t::fadeAlpha() == 1.0f, "...the exit request goes out when the view is black");
    g.ctlFrame();   // the exit begins: the UI is held
    g.freeFrame();
    g.uiFrame();    // the UI comes back, in the dark
    rig.boundary();
    check(g.hidden() == 0 && t::fadeAlpha() == 1.0f, "THE UI COMES BACK IN THE DARK, and the view stays black");
    g.uiFrame();
    g.ctlFrame();   // PhotoCameraToggle: the camera closes
    check(g.mode() == 0, "(the camera closes)");
    g.ctlFrame();
    rig.boundary();
    check(!t::sessionActive() && t::fadeAlpha() == 1.0f, "...the session ends; the view is still black (10 updates at mode 0 have not passed)");
    for (int i = 0; i < 9; ++i) g.ctlFrame();
    rig.boundary();
    check(t::fadeAlpha() == 1.0f, "...nor have 9");
    rig.cap.clear();
    g.ctlFrame();
    rig.boundary();
    check(linesWith("comfort fade: leaving: the camera reads closed and 10 controller updates have passed") == 1, "...the 10th: the fade in begins, with its line");
    until([&] { return t::fadeAlpha() == 0.0f; }, 60);
    check(t::fadeAlpha() == 0.0f && linesWith("comfort fade: clear again") == 1, "...and the view is clear again");
    check(t::steadyUpdates() == 0 && !t::uiSettled(), "THE FACTS RESET with the session: the next entry starts its steady count from 0 and finds the UI's hide unsettled");

    // ---- a second F5 before the request went: a change of mind ---------------------------------------------------------------------------------------------------------
    begin();
    rig.cap.clear();
    rig.boundary(true);
    for (int i = 0; i < 6; ++i) rig.boundary();
    check(t::fadeAlpha() > 0.0f && t::fadeAlpha() < 1.0f && t::f5Request() == 0, "(F5, and part way to black)");
    rig.boundary(true);
    check(linesWith("comfort fade: F5 pressed again before the camera opened") == 1, "F5 AGAIN before the request went: the entry is cancelled, with a line");
    until([&] { return t::fadeAlpha() == 0.0f; }, 60);
    g.ctlFrame();
    check(t::fadeAlpha() == 0.0f && t::f5Request() == 0 && g.seenPhoto() == 0 && g.mode() == 0 && !t::sessionActive(), "...the view is clear again and the camera never opened");

    // ---- a request nobody takes -------------------------------------------------------------------------------------------------------------------------------------------
    begin();
    rig.cap.clear();
    rig.boundary(true);
    until([&] { return t::f5Request() == 1; }, 40);
    check(t::f5Request() == 1 && t::fadeAlpha() == 1.0f, "(the request is out and the controller is not called)");
    int waited = until([&] { return t::fadeAlpha() < 1.0f; }, 120);
    check(linesWith("the F5 request was dropped: the camera controller was not called within 30 frames") == 1 && linesWith("F5's request was not taken or was refused") == 1 && waited >= 30 && waited <= 45,
          "A REQUEST THE CONTROLLER NEVER TAKES: dropped after 30 frames (said), and the view fades back in a few frames later (said) -- not left black for 3 s");
    until([&] { return t::fadeAlpha() == 0.0f; }, 60);
    check(t::fadeAlpha() == 0.0f, "...and is clear");

    // ---- the 3 s cap, with the glue's own facts --------------------------------------------------------------------------------------------------------------------------
    begin();
    rig.cap.clear();
    rig.boundary(true);
    const uint64_t pressedAt = rig.ms;
    until([&] { return t::f5Request() == 1; }, 40);
    openAndPlace();
    int boundaries = 0;
    while (t::fadeAlpha() >= 1.0f && boundaries < 300) {   // the game keeps updating the controller and the free camera; the camera UI is never updated, so its hide never settles
        g.ctlFrame();
        g.freeFrame();
        rig.boundary();
        ++boundaries;
    }
    check(t::fadeAlpha() < 1.0f && t::phase() == 2 && t::sessionActive() && boundaries < 300, "THE CAP: placed, locked and steady, but the camera UI never reports: the fade in begins anyway");
    const uint64_t blackMs = rig.ms - pressedAt;
    check(blackMs >= 2990 && blackMs <= 3100, "...3.0 s after the F5 press (not before, not much after)");
    check(linesWith("black reached the 3 s cap, so the view fades in anyway") == 1 && has(rig.cap.nth("black reached the 3 s cap", 0), "the camera UI's hide has not settled") &&
              !has(rig.cap.nth("black reached the 3 s cap", 0), "the view is not placed") && !has(rig.cap.nth("black reached the 3 s cap", 0), "the lock is not confirmed"),
          "...and its line names exactly the unmet condition: the camera UI's hide");

    // ---- each fact alone, in the glue: the other three met, this one not -> still black -----------------------------------------------------------------------------------
    // (the pure timeline pins each bit; these pin that the glue feeds it the real facts: the steady count, +0x48C and the camera UI's settle flag)
    begin();
    rig.boundary(true);
    until([&] { return t::f5Request() == 1; }, 40);
    openAndPlace();
    for (int i = 0; i < 3; ++i) g.freeFrame();
    g.uiFrame();
    g.uiFrame();
    for (int i = 0; i < 6; ++i) rig.boundary();
    check(t::phase() == 2 && g.mode() == 4 && t::uiSettled() && t::steadyUpdates() >= 2 && t::steadyUpdates() < ecm::kFadeSteadyUpdates && t::fadeAlpha() == 1.0f,
          "THE EYE ALONE: placed, locked (+0x48C = 4), the camera UI's hide settled, the eye steady for only a few updates: STILL BLACK");
    for (int i = 0; i < 12; ++i) g.freeFrame();
    until([&] { return t::fadeAlpha() < 1.0f; }, 5);
    check(t::steadyUpdates() >= ecm::kFadeSteadyUpdates && t::fadeAlpha() < 1.0f, "...the tenth steady update lets the fade in begin");

    begin();
    rig.boundary(true);
    until([&] { return t::f5Request() == 1; }, 40);
    openAndPlace();
    g.setMode(3);   // the lock did not hold (the player unlocked it): +0x48C = 3, the placement goes on and is not pressed again
    for (int i = 0; i < 14; ++i) g.freeFrame();
    g.uiFrame();
    g.uiFrame();
    for (int i = 0; i < 6; ++i) rig.boundary();
    check(t::phase() == 2 && t::steadyUpdates() >= ecm::kFadeSteadyUpdates && t::uiSettled() && g.mode() == 3 && t::fadeAlpha() == 1.0f,
          "THE LOCK ALONE: placed, steady for 10 updates, the UI's hide settled, but +0x48C = 3: STILL BLACK");
    g.setMode(4);
    g.freeFrame();
    until([&] { return t::fadeAlpha() < 1.0f; }, 5);
    check(t::fadeAlpha() < 1.0f, "...locked (+0x48C = 4): the fade in begins");

    // ---- a detach after the entry, in the glue: 100 ms out, black until placed again, 200 ms in; then the game closes the camera ---------------------------------------------
    begin();
    rig.boundary(true);
    until([&] { return t::f5Request() == 1; }, 40);
    openAndPlace();
    for (int i = 0; i < 12; ++i) g.freeFrame();
    g.uiFrame();
    g.uiFrame();
    until([&] { return t::fadeAlpha() < 1.0f; }, 10);
    until([&] { return t::fadeAlpha() == 0.0f; }, 60);
    check(t::fadeAlpha() == 0.0f && t::phase() == 2 && t::uiSettled() && t::sessionActive(), "(entered: placed, clear, the camera UI's hide settled)");
    rig.cap.clear();
    g.setMode(5);   // the player takes the camera to the world lock: the placement is released, the session goes on
    g.freeFrame();
    g.ctlFrame();
    rig.boundary();
    check(t::placedActivity() == 0 && t::sessionActive() && linesWith("comfort fade: the placement was released with the session still on (a detach): fading to black over 100 ms") == 1,
          "A DETACH with the fade on: the placement is released, the session stays, and the re-attach fade begins (one line, 100 ms)");
    g.uiFrame();
    g.uiFrame();
    const int outBoundaries = until([&] { return t::fadeAlpha() == 1.0f; }, 20);
    check(t::fadeAlpha() == 1.0f && outBoundaries >= 4 && outBoundaries <= 9, "...the view is black within about 100 ms (4 to 9 boundaries of 16 ms)");
    for (int i = 0; i < 12; ++i) rig.boundary();
    check(t::fadeAlpha() == 1.0f && t::placedActivity() == 0, "...and holds black while the camera is not placed");
    g.setMode(3);
    g.freeFrame();   // placed again, the lock pressed again
    for (int i = 0; i < 12; ++i) g.freeFrame();
    g.uiFrame();
    g.uiFrame();
    until([&] { return t::fadeAlpha() < 1.0f; }, 10);
    check(t::placedActivity() != 0 && g.mode() == 4 && t::fadeAlpha() < 1.0f && linesWith("comfort fade: re-attaching: placed, locked, the camera UI hidden and the eye steady for 10 updates") == 1 &&
              linesWith("fading in over 200 ms") == 1,
          "...placed, locked, the UI hidden and the eye steady again: the fade in begins (200 ms, one line)");
    until([&] { return t::fadeAlpha() == 0.0f; }, 60);
    check(t::fadeAlpha() == 0.0f && linesWith("comfort fade: clear again") == 1, "...and the view is clear again");
    g.setMode(0);   // the game closes the camera by its own hand: no further camera-UI update is called
    g.ctlFrame();
    rig.boundary();
    rig.boundary();
    check(!t::sessionActive() && !t::uiSettled() && t::steadyUpdates() == 0 && t::fadeAlpha() == 0.0f,
          "THE FACTS RESET WITH THE SESSION even when the camera UI is not updated again: the settle flag is false and the steady count 0, and the view stays clear");

    // ---- the hotkey cleared (or a hook stood down) while black: clear at once ------------------------------------------------------------------------------------------
    begin();
    rig.cap.clear();
    rig.boundary(true);
    until([&] { return t::f5Request() == 1; }, 40);
    g.ctlFrame();
    check(t::fadeAlpha() == 1.0f && t::sessionActive(), "(black, entering)");
    rig.f.hotkey = "";   // hotkey.explorer_cam is emptied by hand -- the session's old key is kept (the keeper is the production wrapper's, not the rig's), so Explorer Cam is off
    rig.boundary();
    check(t::fadeAlpha() == 0.0f && t::comfortRead(rig.ms) == 0.0f && linesWith("stood down or its hotkey was cleared while the view was dark") == 1,
          "THE HOTKEY CLEARED while black: the view is clear AT ONCE (published 0, the runtime reads 0), with a line");
    rig.f.hotkey = "F5";
    rig.boundary();
    check(t::fadeAlpha() == 0.0f, "...and stays clear when it comes back");

    // ---- an unload while black: the signal is withdrawn ----------------------------------------------------------------------------------------------------------------
    begin();
    rig.boundary(true);
    until([&] { return t::f5Request() == 1; }, 40);
    check(t::comfortRead(rig.ms) == 1.0f, "(black, and the runtime reads 1)");
    t::shutdown();
    check(t::comfortRead(rig.ms) == 0.0f, "AN UNLOAD (explorerCamShutdown) while black: the signal is withdrawn at once and the runtime reads 0, not a level that decays");

    // ---- the heartbeat names the fade -------------------------------------------------------------------------------------------------------------------------------
    begin();
    rig.boundary(true);
    until([&] { return t::f5Request() == 1; }, 40);
    openAndPlace();
    for (int i = 0; i < 12; ++i) g.freeFrame();
    g.uiFrame();
    g.uiFrame();
    until([&] { return t::fadeAlpha() < 1.0f; }, 10);
    until([&] { return t::fadeAlpha() == 0.0f; }, 60);
    rig.advance(5200);
    g.freeFrame();
    g.ctlFrame();
    rig.cap.clear();
    rig.boundary();
    check(has(rig.cap.nth("explorer cam: heartbeat (follow, isolation):", 0), "comfort_fade(phase=clear kind=idle alpha=0.000)"),
          "THE HEARTBEAT names the fade (here clear, idle, 0.000: the cap is shorter than the heartbeat, so a black beat is checked on the line itself)");
    check(edvr::explorercamtest::comfortDefaultOn(), "(and the production wrapper runs with the fade on: there is no key)");
    t::reset();
}

void testGlue() {
    std::printf("glue, end to end (synthetic functions with the real prologues)\n");
    Pages p = makePages();
    check(p.ok(), "ten executable pages for the synthetic functions");
    if (!p.ok()) return;
    // Baselines, unhooked.
    static Game g;
    g.init();
    const FnObj freeFn = reinterpret_cast<FnObj>(p.freeG), ctlFn = reinterpret_cast<FnObj>(p.ctlG), uiFn = reinterpret_cast<FnObj>(p.uiG);
    check(freeFn(g.free) == 0x1122334455667788ull && g.freeCalls() == 1, "baseline: the synthetic free-camera update works unhooked");
    check(ctlFn(g.ctl) == 0x2233445566778899ull && g.ctlCalls() == 1, "baseline: the synthetic controller update works unhooked");
    check(uiFn(g.ui) == 0x66778899AABBCCDDull && g.uiCallsSeen() == 1, "baseline: the synthetic camera-UI update works unhooked");
    uint32_t pt = 0x5555;
    check(reinterpret_cast<BoxFn>(p.boxG)(g.free, &pt) == 0x5558 && pt == 0x11111111, "baseline: the synthetic box push works unhooked");
    check(reinterpret_cast<CollisionFn>(p.colG)(g.free, 1000, 20, 3, 0xA5) == (1000ull + 20 + 3 + (0xA5ull << 32)), "baseline: the synthetic collision sweep reads its fifth stack argument");
    g.init();

    testStandDowns(p);
    testF5FromClosed(p);
    testF5FromOtherModes(p);
    testDetachAndReattach(p);
    testSessionTimeoutsAndIdle(p);
    testTabWaitGlue(p);
    testFadeGlue(p);
    testHeadHideGlue(p);
    testSkeletonLatch(p);
    testFollowGlue(p);
    testIsolationGlue(p);
    testComfortGlue(p);
    testSessionEnds(p);
    testFaults(p);
    testObservers(p);
    testHotkeyClash(p);
    testConfigPath(p);
    namespace t = edvr::explorercamtest;
    t::reset();
    bool restored = std::memcmp(p.freeG, p.freeGood.data(), 5) == 0 && std::memcmp(p.colG, p.colGood.data(), 5) == 0 && std::memcmp(p.boxG, p.boxGood.data(), 5) == 0 &&
                    std::memcmp(p.uiG, p.uiGood.data(), 8) == 0 && std::memcmp(p.ctlG, p.ctlGood.data(), 5) == 0;
    check(restored, "RESET: uninstall put every function's original bytes back");
}

}  // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);   // a crash in a cell must not eat the lines before it
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--dry-run") == 0) {
            std::printf("explorer cam test: dry run, nothing to do\n");
            return 0;
        }
    }
    testIdentity();
    testEye();
    testPoseWriter();
    testMachineSession();
    testMachineNoSession();
    testMachinePending();
    testMachineReleases();
    testMachineForeign();
    testSequencerEnter();
    testSequencerTimeouts();
    testSequencerExit();
    testTabWait();
    testRepress();
    testFadeGuard();
    testF5Decision();
    testUiHider();
    testUiSettled();
    testWatches();
    testRing();
    testText();
    testRelayBytes();
    testHeadHidePure();
    testFollowPure();
    testHotkeyKeeper();
    testComfortTimeline();
    testComfortSignal();
    testGlue();
    if (g_failures) {
        std::printf("explorer cam: FAIL (%d)\n", g_failures);
        return 1;
    }
    std::printf("explorer cam: PASS\n");
    return 0;
}
