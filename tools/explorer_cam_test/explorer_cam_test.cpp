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
bool hasEv(const ecm::SeqStep& s, SeqEvent e) {
    for (uint8_t i = 0; i < s.nev; ++i)
        if (s.ev[i] == e) return true;
    return false;
}

void testSequencerEnter() {
    std::printf("F5 sequencer: enter\n");
    // From closed: press PhotoCameraToggle, wait for 1 or 2, press ToggleFreeCam, wait for 3, then it is the placement machine's.
    ecm::F5Sequencer q;
    ecm::SeqStep s = q.step(0, F5Req::None, false);
    check(s.press == CtlPress::None && !s.sessionActive && s.nev == 0, "idle: nothing is pressed and no session is on, whatever the mode");
    s = q.step(0, F5Req::Enter, false);
    check(s.press == CtlPress::Photo && s.sessionActive && hasEv(s, SeqEvent::EnterFromClosed), "ENTER from mode 0: PhotoCameraToggle is pressed, the session is on");
    s = q.step(0, F5Req::None, false);
    check(s.press == CtlPress::None && s.sessionActive, "...the next update waits (the mode has not moved yet): no press");
    s = q.step(1, F5Req::None, false);
    check(s.press == CtlPress::Free && hasEv(s, SeqEvent::EnterOpened), "...mode 1: ToggleFreeCam is pressed");
    s = q.step(1, F5Req::None, false);
    check(s.press == CtlPress::None, "...and only once: one press per update, the wait for 3 presses nothing");
    s = q.step(3, F5Req::None, false);
    check(s.press == CtlPress::None && s.sessionActive && hasEv(s, SeqEvent::EnterAttached) && q.stage() == ecm::F5Sequencer::Stage::Active, "...mode 3: the free camera is up, the sequence is Active");
    s = q.step(4, F5Req::None, false);
    check(s.press == CtlPress::None && s.sessionActive && s.nev == 0, "...mode 4 (the lock took): Active, nothing to do");
    // From a preset (mode 1 or 2): press ToggleFreeCam at once.
    for (int modeInt : {1, 2}) {
        const uint8_t mode = static_cast<uint8_t>(modeInt);
        ecm::F5Sequencer p;
        s = p.step(mode, F5Req::Enter, false);
        check(s.press == CtlPress::Free && s.sessionActive && hasEv(s, SeqEvent::EnterFromPreset), mode == 1 ? "ENTER from mode 1: ToggleFreeCam at once" : "ENTER from mode 2: ToggleFreeCam at once");
        s = p.step(3, F5Req::None, false);
        check(s.sessionActive && hasEv(s, SeqEvent::EnterAttached), "...and mode 3 attaches");
    }
    // From the free camera (3, 4): nothing to press.
    for (int modeInt : {3, 4}) {
        const uint8_t mode = static_cast<uint8_t>(modeInt);
        ecm::F5Sequencer p;
        s = p.step(mode, F5Req::Enter, false);
        check(s.press == CtlPress::None && s.sessionActive && hasEv(s, SeqEvent::EnterFromFree), mode == 3 ? "ENTER from mode 3: the session starts, nothing pressed" : "ENTER from mode 4: the session starts, nothing pressed");
    }
    // From a detached camera (5) or a variant (6): refused, no session.
    {
        ecm::F5Sequencer p;
        s = p.step(5, F5Req::Enter, false);
        check(s.press == CtlPress::None && !s.sessionActive && hasEv(s, SeqEvent::EnterRefusedDetached), "ENTER from mode 5 (detached): refused, 'leave it first', no session");
        s = p.step(6, F5Req::Enter, false);
        check(!s.sessionActive && hasEv(s, SeqEvent::EnterRefusedVariant), "ENTER from mode 6 (variant): refused, no session");
    }
    // A second ENTER while a session is on does nothing.
    {
        ecm::F5Sequencer p;
        p.step(3, F5Req::Enter, false);
        s = p.step(3, F5Req::Enter, false);
        check(s.press == CtlPress::None && hasEv(s, SeqEvent::EnterIgnored) && s.sessionActive, "a second ENTER while the session is on is ignored");
    }
}

void testSequencerTimeouts() {
    std::printf("F5 sequencer: timeouts and the player's own presses\n");
    ecm::SeqStep s;
    // The camera never opens.
    {
        ecm::F5Sequencer q;
        q.step(0, F5Req::Enter, false);
        bool aborted = false;
        uint32_t at = 0;
        for (uint32_t i = 1; i <= 100 && !aborted; ++i) {
            s = q.step(0, F5Req::None, false);
            if (hasEv(s, SeqEvent::EnterTimeoutOpen)) { aborted = true; at = i; }
        }
        check(aborted && at == ecm::kSeqWaitUpdates && !s.sessionActive, "the camera never opens: the sequence aborts after 90 updates with a line, and the session is over");
    }
    // The free camera never comes up (preset kind 1: ToggleFreeCam only toggles 1 and 2).
    {
        ecm::F5Sequencer q;
        q.step(1, F5Req::Enter, false);
        bool aborted = false;
        uint32_t at = 0;
        for (uint32_t i = 1; i <= 100 && !aborted; ++i) {
            s = q.step(i % 2 ? 2 : 1, F5Req::None, false);
            if (hasEv(s, SeqEvent::EnterTimeoutFree)) { aborted = true; at = i; }
        }
        check(aborted && at == ecm::kSeqWaitUpdates && !s.sessionActive, "the free camera never comes up: aborts after 90 updates, the session is over");
    }
    // The camera closes by the player's hand while waiting for the free camera: the session ends at once.
    {
        ecm::F5Sequencer q;
        q.step(1, F5Req::Enter, false);
        s = q.step(0, F5Req::None, false);
        check(hasEv(s, SeqEvent::SessionEnded) && !s.sessionActive, "the player closes the camera while the free camera is awaited: the session ends");
    }
    // The player's own camera key and TAB with no F5: nothing is pressed, no session.
    {
        ecm::F5Sequencer q;
        bool any = false;
        const uint8_t modes[] = {0, 0, 1, 1, 3, 3, 4, 4, 3, 5, 3, 1, 0, 0};
        for (uint8_t m : modes) {
            s = q.step(m, F5Req::None, false);
            any = any || s.press != CtlPress::None || s.sessionActive || s.nev != 0;
        }
        check(!any, "the game's own camera key and TAB (modes walking 0,1,3,4,5,3,1,0) with no F5: no press, no session, no event");
    }
    // Detach and re-attach inside a session: the session stays, nothing is pressed.
    {
        ecm::F5Sequencer q;
        q.step(3, F5Req::Enter, false);
        bool pressed = false, ended = false;
        for (int m : {3, 4, 5, 5, 3, 4}) {
            s = q.step(static_cast<uint8_t>(m), F5Req::None, false);
            pressed = pressed || s.press != CtlPress::None;
            ended = ended || !s.sessionActive;
        }
        check(!pressed && !ended, "detach (5) and re-attach (3, 4) inside a session: the session stays and nothing is pressed");
    }
    // Mode 0 on its own ends an Active session.
    {
        ecm::F5Sequencer q;
        q.step(4, F5Req::Enter, false);
        s = q.step(0, F5Req::None, false);
        check(hasEv(s, SeqEvent::SessionEnded) && !s.sessionActive, "mode 0 by any route ends the session");
    }
}

void testSequencerExit() {
    std::printf("F5 sequencer: exit\n");
    ecm::SeqStep s;
    // UI hidden by EDVR: wait for it to be given back, then close with PhotoCameraToggle, then the session ends at mode 0.
    {
        ecm::F5Sequencer q;
        q.step(4, F5Req::Enter, false);
        s = q.step(4, F5Req::Exit, true);
        check(s.press == CtlPress::None && s.exiting && s.sessionActive && hasEv(s, SeqEvent::ExitStart) && hasEv(s, SeqEvent::ExitUnhiding),
              "EXIT with the UI hidden by EDVR: no press yet, 'giving the UI back first', exiting");
        s = q.step(4, F5Req::None, true);
        check(s.press == CtlPress::None && s.exiting, "...it waits while the UI is still held");
        s = q.step(4, F5Req::None, false);
        check(s.press == CtlPress::Photo && hasEv(s, SeqEvent::ExitClosing), "...the UI is back: PhotoCameraToggle is pressed to close");
        s = q.step(4, F5Req::None, false);
        check(s.press == CtlPress::None && s.sessionActive, "...one press only; the close is awaited");
        s = q.step(0, F5Req::None, false);
        check(hasEv(s, SeqEvent::ExitDone) && !s.sessionActive && !s.exiting, "...mode 0: the session is over");
    }
    // UI not hidden: close at once.
    {
        ecm::F5Sequencer q;
        q.step(3, F5Req::Enter, false);
        s = q.step(3, F5Req::Exit, false);
        check(s.press == CtlPress::Photo && hasEv(s, SeqEvent::ExitStart) && hasEv(s, SeqEvent::ExitClosing), "EXIT with the UI showing: PhotoCameraToggle at once");
    }
    // The UI never comes back: close anyway after the wait.
    {
        ecm::F5Sequencer q;
        q.step(3, F5Req::Enter, false);
        q.step(3, F5Req::Exit, true);
        bool closed = false;
        uint32_t at = 0;
        for (uint32_t i = 1; i <= 100 && !closed; ++i) {
            s = q.step(3, F5Req::None, true);
            if (s.press == CtlPress::Photo) { closed = true; at = i; }
        }
        check(closed && at == ecm::kSeqWaitUpdates && hasEv(s, SeqEvent::ExitUnhideTimeout), "the UI never comes back: the camera is closed anyway after 90 updates, with a line");
    }
    // The camera never closes.
    {
        ecm::F5Sequencer q;
        q.step(3, F5Req::Enter, false);
        q.step(3, F5Req::Exit, false);
        bool aborted = false;
        for (uint32_t i = 1; i <= 100 && !aborted; ++i) {
            s = q.step(3, F5Req::None, false);
            aborted = hasEv(s, SeqEvent::ExitTimeout);
        }
        check(aborted && !s.sessionActive, "the camera never closes: aborts after 90 updates, the session is over");
    }
    // EXIT while still opening.
    {
        ecm::F5Sequencer q;
        q.step(0, F5Req::Enter, false);
        s = q.step(1, F5Req::Exit, false);
        check(s.press == CtlPress::Photo && hasEv(s, SeqEvent::ExitStart), "EXIT in the middle of entering: stops entering and closes");
    }
    // EXIT with the camera already closed, and EXIT with no session.
    {
        ecm::F5Sequencer q;
        q.step(3, F5Req::Enter, false);
        s = q.step(0, F5Req::Exit, false);
        check(hasEv(s, SeqEvent::ExitDone) && !s.sessionActive, "EXIT when the camera has already closed: done at once");
        s = q.step(3, F5Req::Exit, false);
        check(hasEv(s, SeqEvent::ExitIgnored) && s.press == CtlPress::None, "EXIT with no session is ignored");
    }
}

void testF5Decision() {
    std::printf("F5's decision table\n");
    using ecm::F5Action;
    ecm::F5Inputs in;
    in.pressed = true; in.gameplay = true; in.active = true; in.controllerAlive = true; in.mode = 0; in.onFootKnown = true; in.onFoot = true;
    check(ecm::decideF5(in) == F5Action::Enter, "on foot, camera closed, controller alive: ENTER");
    in.mode = 1;
    in.onFoot = false;
    check(ecm::decideF5(in) == F5Action::Enter, "in the stock camera on a preset (not on foot): ENTER");
    in.mode = 3;
    check(ecm::decideF5(in) == F5Action::Enter, "in the free camera: ENTER");
    in.mode = 0;
    check(ecm::decideF5(in) == F5Action::RefuseNotOnFoot, "camera closed and not on foot (in a ship): refused, said so");
    in.onFootKnown = false;
    check(ecm::decideF5(in) == F5Action::Enter, "camera closed and on-foot unknown (no Status.json): ENTER");
    in.onFootKnown = true; in.onFoot = true;
    in.sessionActive = true;
    check(ecm::decideF5(in) == F5Action::Exit, "in an Explorer Cam session: EXIT");
    in.sessionActive = false;
    in.controllerAlive = false;
    check(ecm::decideF5(in) == F5Action::RefuseControllerIdle, "the controller is not being called: refused with 'the camera controller is idle: open the camera first'");
    in.controllerAlive = true;
    in.gameplay = false;
    check(ecm::decideF5(in) == F5Action::None, "the journal says not in gameplay (menus): the press is ignored");
    in.gameplay = true;
    in.active = false;
    check(ecm::decideF5(in) == F5Action::RefuseOff, "Explorer Cam off or a hook stood down: refused, said so");
    in.active = true;
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

    ecm::formatF5(line, sizeof(line), ecm::F5Action::RefuseControllerIdle, 0, true, true);
    check(has(line, "the camera controller is idle: open the camera first"), "F5 on an idle controller: 'the camera controller is idle: open the camera first'");
    ecm::formatF5(line, sizeof(line), ecm::F5Action::Enter, 3, true, false);
    check(has(line, "F5 pressed: entering Explorer Cam (camera mode 3") && has(line, "on foot: no"), "F5 ENTER names the camera mode and on-foot");

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
Pages makePages() {
    Pages p;
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
    FnObj freeUpdate = nullptr, ctlUpdate = nullptr, uiUpdate = nullptr;
    bool refuseOpen = false;                      // the game will not open the camera
    bool noHideHandle = false;                    // the camera UI's +0x1D8 is NULL

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
        const uint64_t lock = reinterpret_cast<uint64_t>(lockAction);
        std::memcpy(free + 0x508, &lock, 8);
        std::memcpy(free + 0x4F8, &lock, 8);   // the probe reads these two as well; any readable action object will do
        std::memcpy(free + 0x500, &lock, 8);
        free[0x470] = 1; free[0x471] = 1; free[0x473] = 0; free[0x48C] = 0;
        const uint64_t photo = reinterpret_cast<uint64_t>(photoAction), fr = reinterpret_cast<uint64_t>(freeAction), quit = reinterpret_cast<uint64_t>(quitAction);
        std::memcpy(ctl + 0x310, &photo, 8);
        std::memcpy(ctl + 0x328, &fr, 8);
        std::memcpy(ctl + 0x340, &quit, 8);
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
            if (kind == 0) {
                setMode(3);
                free[0x473] = 1;   // the free camera's first update seeds from the preset
            } else {
                setMode(m2 == 1 ? 2 : 1);
            }
        }
    }
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
    check(rig.cap.count("F5 pressed: entering Explorer Cam (camera mode 0, closed; on foot: yes)") == 1 && t::f5Request() == 1, "F5 PRESSED on foot with the camera closed: 'entering Explorer Cam', the ENTER request is set");
    g.ctlFrame();   // PhotoCameraToggle pressed: the camera opens
    check(g.seenPhoto() == 1 && Game::pressed(g.photoAction) == 0 && g.mode() == 1 && t::sessionActive() && t::f5Request() == 0,
          "the controller's next update: PhotoCameraToggle's int was 1 for that update and is restored to 0; the game opened the camera (mode 1); the session is on");
    g.ctlFrame();   // ToggleFreeCam pressed
    check(g.seenFree() == 1 && Game::pressed(g.freeAction) == 0 && g.seenPhoto() == 0 && g.mode() == 3,
          "the next update: ToggleFreeCam's int was 1 and is restored; PhotoCameraToggle was not pressed again; mode 3");
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
              rig.cap.count("F5 enter: the free camera is up") == 1,
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
    g.ctlFrame();
    check(g.seenPhoto() == 0 && g.seenFree() == 1 && g.mode() == 3 && t::sessionActive(), "F5 from a preset (mode 1): ToggleFreeCam only (the camera is not closed and reopened), mode 3, the session is on");
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
    for (int i = 0; i < 95; ++i) g.ctlFrame();
    rig.boundary();
    check(!t::sessionActive() && rig.cap.count("F5 enter aborted: the free camera did not come up within 90 updates") == 1 && rig.cap.count("preset kind +0x2E8 = 1") >= 1,
          "PRESET KIND 1: the free camera never comes up; aborts after 90 updates naming the preset kind");

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
    check(rig.cap.count("you are not on foot and the camera is closed") == 1 && t::f5Request() == 0, "NOT ON FOOT, camera closed: refused with a line");
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
    check(t::placedActivity() == 0 && rig.cap.count("released:") == 1 && has(rig.cap.nth("released:", 0), "not called for 30 frames") && t::sessionActive(),
          "...the 30th releases it ('not called for 30 frames'); the session is still on");
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
    testF5Decision();
    testUiHider();
    testWatches();
    testRing();
    testText();
    testRelayBytes();
    testGlue();
    if (g_failures) {
        std::printf("explorer cam: FAIL (%d)\n", g_failures);
        return 1;
    }
    std::printf("explorer cam: PASS\n");
    return 0;
}
