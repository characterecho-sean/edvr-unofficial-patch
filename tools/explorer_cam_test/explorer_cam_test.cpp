// Explorer Cam's rig (src/d3d11/explorer_cam_core.h, explorer_cam.cpp, elite_binds.cpp; fix.explorer_cam, hotkey.explorer_cam).
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
#include "../../src/d3d11/explorer_cam.h"
#include "../../src/d3d11/explorer_cam_core.h"

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
    // THE PANEL: a known non-zero focus refuses in every mode; an unknown focus refuses with the camera closed and passes inside the camera suite.
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
    in = base();
    in.focusKnown = false;
    check(ecm::decideF5(in) == F5Action::RefuseFocus, "GUI FOCUS UNKNOWN, camera closed (mode 0): refused -- first person cannot be told from a panel");
    bool unknownInSuite = true;
    for (int m : {1, 2, 3, 4}) {
        in = base();
        in.mode = static_cast<uint8_t>(m);
        in.focusKnown = false;
        unknownInSuite = unknownInSuite && ecm::decideF5(in) == F5Action::Enter;
    }
    check(unknownInSuite, "GUI FOCUS UNKNOWN inside the camera suite (modes 1-4, on foot known true): ENTER");
    in = base();
    in.focusKnown = false;
    in.onFootKnown = false;
    in.mode = 2;
    check(ecm::decideF5(in) == F5Action::RefuseNotOnFoot, "...but never with on foot unknown as well");
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
    check(has(line, "on foot = unknown (Status.json has no Flags2") && has(line, "GuiFocus = unknown") && has(line, "lags about 6 s"), "...and for an unknown on-foot state (and an unknown focus) says unknown, and the same lag");
    f5in.onFootKnown = true; f5in.onFoot = true; f5in.focusKnown = true; f5in.focus = 9;
    ecm::formatF5(line, sizeof(line), ecm::F5Action::RefuseFocus, f5in);
    check(has(line, "a panel may have the focus") && has(line, "GuiFocus = 9 (the FSS)") && has(line, "on foot = yes") && !has(line, "while its own camera suite is open"),
          "THE PANEL REFUSAL names the focus (9, the FSS) and on foot = yes");
    f5in.mode = 2;
    ecm::formatF5(line, sizeof(line), ecm::F5Action::RefuseFocus, f5in);
    check(has(line, "The game reports a non-zero GuiFocus while its own camera suite is open"), "...inside the camera suite it adds that the game reports a non-zero GuiFocus while its own camera suite is open");
    f5in.mode = 0; f5in.focusKnown = false;
    ecm::formatF5(line, sizeof(line), ecm::F5Action::RefuseFocus, f5in);
    check(has(line, "GuiFocus = unknown") && has(line, "first person cannot be told from an open panel"), "...and for an unknown focus with the camera closed says why it cannot allow it");

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

// The free-camera update: the real 28-byte prologue, then a body that records what it saw.
//   activity+0x540 = the lock action's pressed-int; +0x544 += 1 (calls); +0x548/54C/550 = the pose's origin; +0x554 = the state byte.
constexpr uint32_t kSeenInt = 0x540, kSeenCalls = 0x544, kSeenX = 0x548, kSeenY = 0x54C, kSeenZ = 0x550, kSeenState = 0x554, kRan = 0x558;
Bytes buildFreeSynthetic(bool corrupt) {
    Bytes b;
    prologue(b, ecm::kFreeCameraPrologue, sizeof(ecm::kFreeCameraPrologue));
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
    Bytes freeGood, freeBad, colGood, colBad, boxGood, boxBad, uiGood, uiBad, ctlGood, ctlBad;
    uint8_t *freeG = nullptr, *freeB = nullptr, *colG = nullptr, *colB = nullptr, *boxG = nullptr, *boxB = nullptr, *uiG = nullptr, *uiB = nullptr,
            *ctlG = nullptr, *ctlB = nullptr;
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
    p.freeG = makeExecutable(p.freeGood); p.freeB = makeExecutable(p.freeBad);
    p.colG = makeExecutable(p.colGood); p.colB = makeExecutable(p.colBad);
    p.boxG = makeExecutable(p.boxGood); p.boxB = makeExecutable(p.boxBad);
    p.uiG = makeExecutable(p.uiGood); p.uiB = makeExecutable(p.uiBad);
    p.ctlG = makeExecutable(p.ctlGood); p.ctlB = makeExecutable(p.ctlBad);
    return p;
}

// The rig is the game between calls. Three objects and the action objects they point at.
struct Game {
    alignas(16) uint8_t free[0x600] = {};         // the free-camera activity
    alignas(16) uint8_t lockAction[0x40] = {};
    alignas(16) uint8_t ctl[0x600] = {};          // the camera controller
    alignas(16) uint8_t photoAction[0x40] = {}, freeAction[0x40] = {}, quitAction[0x40] = {};
    alignas(16) uint8_t ui[0x600] = {};           // the camera UI
    alignas(16) uint8_t hideAction[0x40] = {};
    alignas(16) uint8_t iface[0x100] = {};        // the interface cached at controller+0x108; the shared record is at iface+0x40
    uintptr_t vtable[8] = {};                     // its vtable: slot 4 (+0x20) is the record's accessor
    FnObj freeUpdate = nullptr, ctlUpdate = nullptr, uiUpdate = nullptr;
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
    rig.f.on = false;
    rig.boundary();
    rig.boundary();
    rig.advance(6000);
    rig.boundary();
    check(rig.cap.lines.size() >= 1 && std::strncmp(rig.cap.lines[0].c_str(), "explorer cam: off (fix.explorer_cam = off)", 42) == 0 && rig.cap.count("explorer cam: off") == 1,
          "KEY OFF: three boundaries print ONE 'explorer cam: off' line");
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
        rig.f.on = true;
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

    check(rig.cap.count("explorer cam: on (fix.explorer_cam = on)") == 1 && rig.cap.count("hook armed:") == 5, "ARMED: one 'on' line and five 'hook armed' lines (free-camera, collision, box-push, controller, camera-UI)");
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
    // End the session by the player closing the camera by hand: mode 0 by any route ends it.
    rig.cap.clear();
    g.userPresses(g.photoAction, &Game::ctlFrame);
    g.ctlFrame();
    rig.boundary();
    check(g.mode() == 0 && !t::sessionActive() && t::placedActivity() == 0 && rig.cap.count("the camera closed (mode 0)") == 1, "MODE 0 BY THE PLAYER'S OWN HAND ends the session and releases the placement, with a line");

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
    check(rig.cap.count("a panel may have the focus") == 1 && has(rig.cap.nth("a panel may have the focus", 0), "GuiFocus = 5 (station services)") && t::f5Request() == 0,
          "A PANEL OPEN (GuiFocus 5), camera closed: refused with one line naming the focus");
    rig.f.focusKnown = false;
    rig.cap.clear();
    rig.boundary(true);
    check(rig.cap.count("a panel may have the focus") == 1 && has(rig.cap.nth("a panel may have the focus", 0), "GuiFocus = unknown") && t::f5Request() == 0, "GUI FOCUS UNKNOWN, camera closed: refused with one line");
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
    check(t::f5Request() == 1 && rig.cap.count("a panel may have the focus") == 0, "...on foot with an UNKNOWN focus inside the camera suite (mode 1): ENTER");
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
    check(rig.cap.count("a panel may have the focus") == 1 && has(rig.cap.nth("a panel may have the focus", 0), "GuiFocus = 6 (the galaxy map)") &&
              has(rig.cap.nth("a panel may have the focus", 0), "while its own camera suite is open") && t::f5Request() == 0,
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
    check(rig.cap.count("F5 pressed: leaving Explorer Cam") == 1 && rig.cap.count("only starts on foot") == 0 && rig.cap.count("a panel may have the focus") == 0,
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

    // fix.explorer_cam turning off: the placement is released, the session ended, and the UI is given back through the open gate.
    rig.cap.clear();
    rig.f.on = false;
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
    rig.f.on = true;
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
    rig.f.on = true;
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
    rig.f.on = true;
    installAll(p, rig, g);
    g.setMode(3);
    g.free[0x473] = 1;
    g.ctlFrame(); rig.boundary(); g.ctlFrame(); rig.boundary();
    rig.boundary(true);
    g.ctlFrame(); g.freeFrame(); g.freeFrame(); g.ctlFrame();
    g.uiFrame();   // FreeCamToggleHUD pressed: the game hid the UI, EDVR has not yet read the result
    check(g.hidden() == 1 && !t::uiHiddenByEdvr(), "(the hide press is in flight: the UI is hidden, EDVR has not read the result)");
    rig.f.on = false;
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
    // Defaults: nothing set. The ini ships `explorer_cam = on` and `hotkey.explorer_cam = F5`; the code defaults agree.
    explorerCamFrameBoundary(1);
    check(t::placeActive(), "no keys set: Explorer Cam is ON by default");
    check(t::hotkeyVk() == VK_F5, "...and hotkey.explorer_cam defaults to F5");
    // The eye keys reach the hook. The wrapper's F5 is the real keyboard, so the session is switched on through the seam.
    t::forceSession(true);
    g.setMode(3);
    g.freeFrame();
    check(closeTo(g.seen(kSeenX), 0.0f) && closeTo(g.seen(kSeenY), 1.68f) && closeTo(g.seen(kSeenZ), 0.10f), "a placement with no keys set puts the eye at up 1.68, forward 0.10, right 0.0");
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
    cfg.set("hotkey.explorer_cam", "F6");
    explorerCamFrameBoundary(6);
    check(t::hotkeyVk() == VK_F6, "hotkey.explorer_cam = F6: bound to F6");
    cfg.set("fix.explorer_cam", "off");
    explorerCamFrameBoundary(7);
    check(!t::placeActive(), "fix.explorer_cam = off: placement inactive");
    cfg.set("fix.explorer_cam", "on");
    explorerCamFrameBoundary(8);
    check(t::placeActive(), "fix.explorer_cam = on: active again");
    t::reset();
    for (const char* key : {"hotkey.explorer_cam", "fix.explorer_cam", "fix.explorer_cam_eye_up", "fix.explorer_cam_eye_forward", "fix.explorer_cam_eye_right"}) cfg.set(key, "");
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
    rig.f.on = false;
    rig.boundary();
    check(fadeMode == 0 && t::fadeOurs() && !t::placeActive(), "FEATURE OFF with the camera still open (mode 4): the global is HELD at 0 (the camera is inside the body)");
    g.ctlFrame();
    g.userPresses(g.photoAction, &Game::ctlFrame);   // the player closes the camera by hand
    g.ctlFrame();
    check(g.mode() == 0, "(the player closes the camera)");
    rig.boundary();
    check(fadeMode == -1 && !t::fadeOurs() && rig.cap.count("avatar fade: put the dither-fade mode global back to -1 (auto)") == 1,
          "...the camera closed (the controller hook still publishes the mode while EDVR holds the global): restored to -1 though the feature is off");
    rig.f.on = true;

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
    // FindJoint: the real prologue, then eax = 3 (the index every name gets) and the matching epilogue.
    cimgCommit(ecm::kFindJointRva, 64);
    {
        uint8_t* p = g_cimg + ecm::kFindJointRva;
        size_t n = 0;
        for (uint8_t b : ecm::kFindJointPrologue) p[n++] = b;
        p[n++] = 0xB8; p[n++] = 0x03; p[n++] = 0; p[n++] = 0; p[n++] = 0;                                          // mov eax, 3
        p[n++] = 0x48; p[n++] = 0x83; p[n++] = 0xC4; p[n++] = 0x20; p[n++] = 0x5F;                                 // add rsp,20h; pop rdi
        p[n++] = 0x48; p[n++] = 0x8B; p[n++] = 0x5C; p[n++] = 0x24; p[n++] = 0x08; p[n++] = 0xC3;                  // mov rbx,[rsp+8]; ret
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
        rig.f.on = on;
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
              has(rig.cap.nth("differs", 0), "index 3, which is not \"Helmet\"") && has(rig.cap.nth("differs", 0), "nothing is hidden"),
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
    testWatches();
    testRing();
    testText();
    testRelayBytes();
    testHeadHidePure();
    testGlue();
    if (g_failures) {
        std::printf("explorer cam: FAIL (%d)\n", g_failures);
        return 1;
    }
    std::printf("explorer cam: PASS\n");
    return 0;
}
