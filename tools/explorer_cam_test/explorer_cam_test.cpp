// Explorer Cam's rig (src/d3d11/explorer_cam_core.h, explorer_cam.cpp; fix.explorer_cam).
//
// Part A drives the pure half the DLL compiles: the build-332841 identity and CodeHook's reading of both prologues, the eye keys'
// clamp, the sixteen floats written into the free camera's commander-local pose, the per-activity state machine on scripted state
// sequences (entry, the pending first update, the lock pressed once, the user's own unlock, the world lock and the variant state,
// a fresh session, a second activity, the key turning off, a fault), the stale watch, the event ring, every log line's text, and
// the two relays' machine code.
//
// Part B runs the glue (explorer_cam.cpp compiled with EDVR_EXPLORER_CAM_TEST) end to end against SYNTHETIC functions that begin
// with the real prologues: a free-camera update whose body records what it saw (the lock's pressed-int, the pose's origin, the
// state byte) and a collision step that reads its FIFTH argument from the stack. The rig plays the game between calls: it
// advances +0x48C the way the update does, re-derives the pose the way the update does, and presses nothing itself. It proves that
// the pose is written before the original and visible to it, the lock's int is 1 for exactly one call and restored after it, the
// collision relay answers 0 without running the function for the placed activity only and hands every other call (all five
// arguments) to the original, a wrong prologue stands down with one line and no patch, the live eye keys reach the next update,
// a silent activity is released after 30 frames, the key turning off releases, faults are counted and end the feature at eight,
// and the keys are read from the config under their real names. This is "what would appear in the log if the new code never ran"
// made executable.
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
#include "../../src/d3d11/explorer_cam.h"
#include "../../src/d3d11/explorer_cam_core.h"

using namespace edvr;

// ---- stubs for what the glue calls -------------------------------------------------------------------------------------
namespace edvr {
void breadcrumb(const char*) {}   // production guard.cpp's crash-channel dependency (proxy.cpp), as the other rigs stub it
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
    size_t prefixed() const {
        size_t n = 0;
        for (const std::string& l : lines) n += l.compare(0, std::strlen(ecm::prefix()), ecm::prefix()) == 0 ? 1 : 0;
        return n;
    }
    void clear() { lines.clear(); }
};

// ================================ Part A: the pure half ================================

void testIdentity() {
    std::printf("identity\n");
    // The bytes exactly as Phase 0a / 0a-2 printed them, spelled out again here (not copied from the header).
    const uint8_t free28[28] = {0x48, 0x89, 0x5C, 0x24, 0x20, 0x55, 0x57, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48,
                                0x8D, 0xAC, 0x24, 0x40, 0xFD, 0xFF, 0xFF, 0x48, 0x81, 0xEC, 0xC0, 0x03, 0x00, 0x00};
    const uint8_t col26[26] = {0x40, 0x55, 0x53, 0x56, 0x57, 0x41, 0x54, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8D,
                               0xAC, 0x24, 0xB0, 0xFE, 0xFF, 0xFF, 0x48, 0x81, 0xEC, 0x50, 0x02, 0x00, 0x00};
    check(sizeof(ecm::kFreeCameraPrologue) == 28 && std::memcmp(ecm::kFreeCameraPrologue, free28, 28) == 0,
          "the free-camera prologue is 48 89 5C 24 20 55 57 41 55 41 56 41 57 48 8D AC 24 40 FD FF FF 48 81 EC C0 03 00 00");
    check(sizeof(ecm::kCollisionPrologue) == 26 && std::memcmp(ecm::kCollisionPrologue, col26, 26) == 0,
          "the collision prologue is 40 55 53 56 57 41 54 41 56 41 57 48 8D AC 24 B0 FE FF FF 48 81 EC 50 02 00 00");
    check(ecm::kFreeCameraRva == 0x1071980 && ecm::kCollisionRva == 0x1091140, "the targets are EliteDangerous64.exe+0x1071980 and +0x1091140");
    check(ecm::kCollisionRva % 64 == 0, "0x1091140 is 64-byte aligned, so CodeHook's aligned 8-byte store applies");
    check(ecm::kOffLocalPose == 0x3B0 && ecm::kOffRelative == 0x470 && ecm::kOffRotationLock == 0x471 && ecm::kOffPresetPending == 0x473 &&
              ecm::kOffState == 0x48C && ecm::kOffLockAction == 0x508 && ecm::kOffActionPressed == 0x1C,
          "the activity's offsets are +0x3B0 +0x470 +0x471 +0x473 +0x48C +0x508 and the pressed-int at +0x1C");
    // CodeHook's decoder: the collision prologue's first instruction is a REX-prefixed push (40 55) and the first four instructions
    // cover exactly five bytes, with no rip-relative displacement.
    size_t disp = 99;
    size_t at = 0, stolen = 0;
    size_t lens[4] = {};
    for (int i = 0; i < 4 && stolen < kCodeHookPatchBytes; ++i) {
        const size_t len = codeInstructionLength(col26 + at, sizeof(col26) - at, &disp);
        lens[i] = len;
        if (len == 0 || disp != 0) break;
        at += len;
        stolen += len;
    }
    check(lens[0] == 2 && lens[1] == 1 && lens[2] == 1 && lens[3] == 1 && stolen == 5,
          "CodeHook's decoder accepts `40 55` (REX push rbp) and measures the collision prologue as 2+1+1+1 = 5 stolen bytes, no displacement");
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

// ---- the state machine --------------------------------------------------------------------------------------------------------
ecm::Observed obs(uint8_t relative, uint8_t rotLock, uint8_t pending, uint8_t state) {
    ecm::Observed o;
    o.relative = relative; o.rotationLock = rotLock; o.presetPending = pending; o.state = state;
    return o;
}
constexpr uint64_t kA = 0xA000, kB = 0xB000;

void testMachineSession() {
    std::printf("machine: a normal session\n");
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
    // A new session on the same activity.
    s = m.step(kA, obs(1, 1, 1, 0), true);
    check(!s.entered && !s.write, "a new session's first call (+0x48C=0, +0x473=1): idle again");
    s = m.step(kA, obs(1, 1, 0, 3), true);
    check(s.entered && s.write && s.press, "...and its second call enters, places and presses again: ONE press per entry");
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
    // Locked by the user before the first write: placing with no press.
    ecm::Machine n;
    n.step(kA, obs(1, 1, 1, 3), true);
    s = n.step(kA, obs(1, 1, 0, 4), true);
    check(s.write && s.placeNow && s.alreadyLocked && !s.press && n.pressed(), "already locked (+0x48C=4) at the first write: placed, no press, now or later");
    s = n.step(kA, obs(1, 1, 0, 3), true);
    check(s.write && !s.press, "...and an unlock after that is not pressed again either");
    // A stale byte on the first call of a new session must not block the entry that follows.
    ecm::Machine t;
    t.step(kA, obs(1, 1, 0, 4), true);   // the byte seen at 4 without +0x473: a session that began without us
    s = t.step(kA, obs(1, 1, 0, 3), true);
    check(!s.entered && !s.write, "a session that began locked (+0x48C=4, no +0x473) is not taken for an entry when it unlocks");
    s = t.step(kA, obs(1, 1, 1, 4), true);   // +0x473 = 1: the first update of a brand-new session, whatever stale state the byte held
    check(!s.entered, "a new session's first call with a stale +0x48C=4 is idle...");
    s = t.step(kA, obs(1, 1, 0, 3), true);
    check(s.entered && s.write && s.press, "...and its second call enters (+0x473=1 made the session fresh)");
}

void testMachineReleases() {
    std::printf("machine: what releases, and what it takes to enter again\n");
    ecm::Step s;
    // The world lock.
    ecm::Machine m;
    m.step(kA, obs(1, 1, 0, 3), true);
    s = m.step(kA, obs(1, 1, 0, 5), true);
    check(s.released && s.why == ecm::Why::WorldLock && !s.write && !s.press && m.phase() == ecm::Machine::Phase::Idle, "+0x48C=5 (world lock) RELEASES, writes nothing");
    s = m.step(kA, obs(1, 1, 0, 3), true);
    check(!s.entered && !s.write, "...and a return to 3 does not re-enter (the user took the camera)");
    s = m.step(kA, obs(1, 1, 0, 0), true);
    s = m.step(kA, obs(1, 1, 1, 0), true);
    s = m.step(kA, obs(1, 1, 0, 3), true);
    check(s.entered && s.write && s.press, "...until +0x48C has been 0: then the next session enters");
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
    // The key turning off.
    ecm::Machine k;
    k.step(kA, obs(1, 1, 0, 3), true);
    s = k.step(kA, obs(1, 1, 0, 4), false);
    check(s.released && s.why == ecm::Why::KeyOff && !s.write && !s.press, "the key off RELEASES on the next call, writes nothing");
    s = k.step(kA, obs(1, 1, 0, 3), false);
    check(!s.entered && !s.write, "...and with the key still off an idle machine does nothing, even at +0x48C=3");
    s = k.step(kA, obs(1, 1, 0, 3), true);
    check(s.entered && s.write && s.press, "...key back on at +0x48C=3: enters again");
    // The fault path: the hook thread aborts.
    ecm::Machine f;
    f.step(kA, obs(1, 1, 0, 3), true);
    f.abort(ecm::Why::Fault);
    check(f.phase() == ecm::Machine::Phase::Idle && !f.placed(), "a fault aborts the session");
    s = f.step(kA, obs(1, 1, 0, 3), true);
    check(!s.entered && !s.write, "...and does not re-enter at the same state (no fault loop)");
    // A reset (the frame thread's request) starts over, fresh.
    f.reset();
    s = f.step(kA, obs(1, 1, 0, 3), true);
    check(s.entered && s.write, "a reset makes the next +0x48C=3 an entry");
    // +0x473 going back to 1 on a placed activity: a new session without a 0 in between.
    ecm::Machine r;
    r.step(kA, obs(1, 1, 0, 3), true);
    r.step(kA, obs(1, 1, 0, 4), true);
    s = r.step(kA, obs(1, 1, 1, 4), true);
    check(s.released && s.why == ecm::Why::NewSession && !s.write && !s.entered, "+0x473=1 again on a placed activity ends the session (a new one began) and does not write");
    s = r.step(kA, obs(1, 1, 0, 3), true);
    check(s.entered && s.write && s.press, "...and the update after it enters the new session");
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

void testStaleWatch() {
    std::printf("the stale watch\n");
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
    check(std::strncmp(line, "explorer cam: placed:", 21) == 0 && has(line, "act=0x1234ABCD") && has(line, "eye(up=1.680 forward=0.100 right=0.000)"),
          "Placed: 'explorer cam: placed:' with the activity and the eye values to 3 decimals");
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
    check(std::strncmp(line, "explorer cam: released:", 23) == 0 && has(line, "world lock") && has(line, "pose writes so far=321"), "Released: 'explorer cam: released:' with the reason and the write count");
    e.kind = static_cast<uint32_t>(ecm::EvKind::Fault);
    e.why = static_cast<uint32_t>(ecm::FaultSite::SetLock);
    e.count = 3;
    ecm::formatEvent(line, sizeof(line), e, 10);
    check(std::strncmp(line, "explorer cam: fault 3 of 8:", 27) == 0 && has(line, "lock press"), "Fault: 'explorer cam: fault 3 of 8:' naming where");
    e.kind = static_cast<uint32_t>(ecm::EvKind::FaultLimit);
    e.count = 8;
    ecm::formatEvent(line, sizeof(line), e, 11);
    check(std::strncmp(line, "explorer cam: stood down for the session:", 41) == 0 && has(line, "8 guarded accesses"), "FaultLimit: 'explorer cam: stood down for the session:'");

    ecm::HeartbeatIn h;
    h.windowSeconds = 5.0; h.phase = "placed"; h.activity = 0xABC; h.state = 4;
    h.updates = 450; h.updatesWindow = 450; h.bypassed = 450; h.bypassedWindow = 450; h.forwarded = 3; h.forwardedWindow = 3;
    h.hookCalls = 460; h.hookCallsWindow = 460; h.faults = 1;
    h.eye = ecm::clampEye(1.68f, 0.10f, 0.0f);
    ecm::formatHeartbeat(line, sizeof(line), h);
    check(std::strncmp(line, "explorer cam: heartbeat: phase=placed", 37) == 0 && has(line, "updates_placed=450(+450)") && has(line, "collision_bypassed=450(+450)") &&
              has(line, "collision_forwarded=3(+3)") && has(line, "faults=1") && has(line, "hook_calls=460(+460)"),
          "the heartbeat names updates placed, collision calls bypassed and forwarded, hook calls and faults");
    Capture cap;
    cap.lines.push_back("explorer cam probe I3 heartbeat: x");
    cap.lines.push_back("explorer cam probe: on");
    check(cap.prefixed() == 0, "the probe's lines do not match this feature's 'explorer cam:' prefix");
}

void testRelayBytes() {
    std::printf("the relays' machine code\n");
    uint8_t c[ecm::kCollisionRelayBytes] = {};
    uint64_t placed = 0, bypassed = 0, forwarded = 0;
    ecm::buildCollisionRelay(c, &placed, &bypassed, &forwarded);
    uintptr_t a = 0;
    std::memcpy(&a, c + ecm::kCollisionRelayPlacedAt, 8);
    check(a == reinterpret_cast<uintptr_t>(&placed), "the placed-activity literal is the address passed in");
    std::memcpy(&a, c + ecm::kCollisionRelayBypassedAt, 8);
    check(a == reinterpret_cast<uintptr_t>(&bypassed), "the bypassed-counter literal is the address passed in");
    std::memcpy(&a, c + ecm::kCollisionRelayForwardedAt, 8);
    check(a == reinterpret_cast<uintptr_t>(&forwarded), "the forwarded-counter literal is the address passed in");
    // The two short jumps land on `forward:` (offset 40), and the xor/ret sits before it.
    check(c[16] == 0x74 && 18 + c[17] == 40 && c[21] == 0x75 && 23 + c[22] == 40, "both short jumps land on the forward block (offset 40)");
    check(c[37] == 0x31 && c[38] == 0xC0 && c[39] == 0xC3, "the bypass block ends `xor eax,eax; ret`");
    check(c[54] == 0xFF && c[55] == 0x25 && ecm::kCollisionRelayTrampolineAt == 60, "the forward block ends `jmp [rip+0]` with the trampoline literal at 60");
    uint8_t f[ecm::kFreeCameraRelayBytes] = {};
    int gate = 0, callback = 0;
    ecm::buildFreeCameraRelay(f, &gate, &callback);
    std::memcpy(&a, f + ecm::kFreeCameraRelayGateAt, 8);
    check(a == reinterpret_cast<uintptr_t>(&gate), "the free-camera relay's gate literal is the address passed in");
    std::memcpy(&a, f + ecm::kFreeCameraRelayCallbackAt, 8);
    check(a == reinterpret_cast<uintptr_t>(&callback) && 16 + 14 == 30 && f[15] == 0x0E, "...its callback literal too, and `je original` lands on the trampoline jump (offset 30)");
}

// ================================ Part B: the glue, end to end ================================
// Machine code for the synthetic functions. Verified against capstone when written (the rig's comment on each line is the assembly).
using Bytes = std::vector<uint8_t>;
void emit(Bytes& b, std::initializer_list<uint8_t> bytes) { for (uint8_t x : bytes) b.push_back(x); }

// FreeCameraActivity's update, as far as this feature can tell: the real 28-byte prologue, then a body that records what it saw.
//   activity+0x540 = the lock action's pressed-int; +0x544 += 1 (calls); +0x548/54C/550 = the pose's origin (+0x3E0/3E4/3E8);
//   +0x554 = the state byte; returns 0x1122334455667788.
constexpr uint32_t kSeenInt = 0x540, kSeenCalls = 0x544, kSeenX = 0x548, kSeenY = 0x54C, kSeenZ = 0x550, kSeenState = 0x554, kRan = 0x558;
Bytes buildFreeSynthetic(bool corrupt) {
    Bytes b;
    for (uint8_t x : ecm::kFreeCameraPrologue) b.push_back(x);
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
    if (corrupt) b[14] ^= 0x01;   // the byte of `lea rbp,[rsp-2C0h]`: no longer build 332841's prologue
    return b;
}
// The collision step: the real 26-byte prologue, then a body that counts its run in the rcx object (+0x558) and returns
// rdx + r8 + r9 + (the byte the caller pushed as the fifth argument << 32). After the seven pushes and `sub rsp,250h` the fifth
// argument is at [rsp+2B0h] (entry [rsp+28h] + 7*8 + 250h).
Bytes buildCollisionSynthetic(bool corrupt) {
    Bytes b;
    for (uint8_t x : ecm::kCollisionPrologue) b.push_back(x);
    emit(b, {0xFF, 0x81, 0x58, 0x05, 0x00, 0x00});                // inc dword ptr [rcx+558h]
    emit(b, {0x48, 0x89, 0xD0});                                  // mov rax, rdx
    emit(b, {0x4C, 0x01, 0xC0});                                  // add rax, r8
    emit(b, {0x4C, 0x01, 0xC8});                                  // add rax, r9
    emit(b, {0x44, 0x0F, 0xB6, 0x94, 0x24, 0xB0, 0x02, 0x00, 0x00});   // movzx r10d, byte ptr [rsp+2B0h]
    emit(b, {0x49, 0xC1, 0xE2, 0x20});                            // shl r10, 32
    emit(b, {0x4C, 0x09, 0xD0});                                  // or rax, r10
    emit(b, {0x48, 0x81, 0xC4, 0x50, 0x02, 0x00, 0x00});          // add rsp, 250h
    emit(b, {0x41, 0x5F, 0x41, 0x5E, 0x41, 0x5C, 0x5F, 0x5E, 0x5B, 0x5D});   // pop r15; pop r14; pop r12; pop rdi; pop rsi; pop rbx; pop rbp
    emit(b, {0xC3});                                              // ret
    if (corrupt) b[15] ^= 0x01;   // the `lea rbp,[rsp-1B0h]` displacement
    return b;
}
// A page whose function entry is on a 64-byte boundary (as 0x1091140 is).
uint8_t* makeExecutable(const Bytes& code) {
    auto* page = static_cast<uint8_t*>(VirtualAlloc(nullptr, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!page) return nullptr;
    std::memcpy(page, code.data(), code.size());
    DWORD old = 0;
    VirtualProtect(page, 4096, PAGE_EXECUTE_READ, &old);
    FlushInstructionCache(GetCurrentProcess(), page, code.size());
    return page;
}
using FreeFn = uint64_t (__fastcall*)(void*);
using CollisionFn = uint64_t (__fastcall*)(void*, uint64_t, uint64_t, uint64_t, uint64_t);

// The rig is the game between calls. An activity and the action object it points at.
struct World {
    alignas(16) uint8_t activity[0x600] = {};
    alignas(16) uint8_t action[0x40] = {};
    FreeFn update = nullptr;

    uint8_t* a() { return activity; }
    int32_t& pressedInt() { return *reinterpret_cast<int32_t*>(action + 0x1C); }
    float& pose(int i) { return *reinterpret_cast<float*>(activity + 0x3B0 + 4 * i); }
    float seen(uint32_t offset) const { float f; std::memcpy(&f, activity + offset, 4); return f; }
    int32_t seenInt() const { int32_t v; std::memcpy(&v, activity + kSeenInt, 4); return v; }
    uint32_t seenCalls() const { uint32_t v; std::memcpy(&v, activity + kSeenCalls, 4); return v; }
    uint32_t seenState() const { uint32_t v; std::memcpy(&v, activity + kSeenState, 4); return v; }
    uint32_t ran() const { uint32_t v; std::memcpy(&v, activity + kRan, 4); return v; }
    uint8_t& state() { return activity[0x48C]; }

    void init() {
        std::memset(activity, 0, sizeof(activity));
        std::memset(action, 0, sizeof(action));
        const uint64_t p = reinterpret_cast<uint64_t>(action);
        std::memcpy(activity + 0x508, &p, 8);
        activity[0x470] = 1; activity[0x471] = 1; activity[0x473] = 1; activity[0x48C] = 0;
        // The selfie preset: facing back, origin (0, 1.5, 1.9), a distinctive fourth float per row.
        const float preset[16] = {-1, 0, 0, 0.5f, 0, 1, 0, 0.25f, 0, 0, -1, 0.125f, 0, 1.5f, 1.9f, 1.0f};
        std::memcpy(activity + 0x3B0, preset, 64);
    }
    // One game frame: the hooked update runs, then the game does what its update did to the state.
    void frame() {
        update(activity);
        if (activity[0x473] == 1) {            // the first update seeds the pose from the preset, clears +0x473 and stores state 3
            activity[0x473] = 0;
            activity[0x48C] = 3;
        } else if (seenInt() != 0) {           // the update saw the relative-lock action pressed (decomp lines 459-480)
            if (activity[0x48C] == 4) activity[0x48C] = 3;
            else if (activity[0x48C] == 3) { activity[0x470] = 1; activity[0x48C] = 4; }
        }
        // The update re-derives the commander-local pose from its collided result: here, where the face stopped it (0.7 m).
        pose(12) = 0.02f; pose(13) = 1.70f; pose(14) = 0.70f;
    }
};

void testGlue() {
    std::printf("glue, end to end (synthetic FreeCameraActivity update and collision step)\n");
    namespace t = edvr::explorercamtest;
    const Bytes goodFreeCode = buildFreeSynthetic(false), badFreeCode = buildFreeSynthetic(true);
    const Bytes goodColCode = buildCollisionSynthetic(false), badColCode = buildCollisionSynthetic(true);
    uint8_t* goodFree = makeExecutable(goodFreeCode);
    uint8_t* badFree = makeExecutable(badFreeCode);
    uint8_t* goodCol = makeExecutable(goodColCode);
    uint8_t* badCol = makeExecutable(badColCode);
    check(goodFree && badFree && goodCol && badCol, "four executable pages for the synthetic functions");
    if (!goodFree || !badFree || !goodCol || !badCol) return;

    static World w, other;
    w.init();
    other.init();
    w.update = reinterpret_cast<FreeFn>(goodFree);
    other.update = reinterpret_cast<FreeFn>(goodFree);
    auto collision = reinterpret_cast<CollisionFn>(goodCol);
    const FreeFn bareFree = reinterpret_cast<FreeFn>(goodFree);

    // Baselines, unhooked.
    check(bareFree(w.a()) == 0x1122334455667788ull && w.seenCalls() == 1, "baseline: the synthetic free-camera update works unhooked");
    check(collision(w.a(), 1000, 20, 3, 0xA5) == (1000ull + 20 + 3 + (0xA5ull << 32)) && w.ran() == 1,
          "baseline: the synthetic collision step reads its fifth stack argument and returns rdx+r8+r9+(fifth<<32)");
    w.init();
    other.init();

    uint8_t freeBefore[40], colBefore[40], badFreeBefore[40], badColBefore[40];
    std::memcpy(freeBefore, goodFree, 40); std::memcpy(colBefore, goodCol, 40);
    std::memcpy(badFreeBefore, badFree, 40); std::memcpy(badColBefore, badCol, 40);

    Capture cap;
    // ---- key off from the start: nothing is installed --------------------------------------------------------------------------
    t::setTargets(reinterpret_cast<uintptr_t>(goodFree), reinterpret_cast<uintptr_t>(goodCol));
    t::boundary(1, 1000, false, 1.68f, 0.10f, 0.0f, &Capture::add, &cap);
    t::boundary(2, 1016, false, 1.68f, 0.10f, 0.0f, &Capture::add, &cap);
    t::boundary(3, 7000, false, 1.68f, 0.10f, 0.0f, &Capture::add, &cap);
    check(cap.lines.size() == 1 && std::strncmp(cap.lines[0].c_str(), "explorer cam: off (fix.explorer_cam = off)", 42) == 0,
          "KEY OFF: three boundaries print ONE 'explorer cam: off' line");
    check(std::memcmp(freeBefore, goodFree, 40) == 0 && std::memcmp(colBefore, goodCol, 40) == 0 && !t::gateOpen() && !t::placeActive() &&
              t::freeStolen() == 0 && t::collisionStolen() == 0,
          "KEY OFF: neither function was touched, the gate is closed, placement is not active");
    t::reset();

    // ---- a wrong free-camera prologue: one stand-down line, no patch, the collision hook never tried -----------------------------
    cap.clear();
    t::setTargets(reinterpret_cast<uintptr_t>(badFree), reinterpret_cast<uintptr_t>(goodCol));
    t::boundary(4, 8000, true, 1.68f, 0.10f, 0.0f, &Capture::add, &cap);
    t::boundary(5, 8016, true, 1.68f, 0.10f, 0.0f, &Capture::add, &cap);
    check(cap.count("free-camera hook stood down:") == 1 && has(cap.nth("free-camera hook stood down:", 0), "the game build differs") &&
              has(cap.nth("free-camera hook stood down:", 0), "nothing was patched"),
          "WRONG FREE-CAMERA PROLOGUE: one 'free-camera hook stood down' line saying the game build differs");
    check(cap.count("collision hook not installed:") == 1 && cap.count("hook armed:") == 0, "...one line says the collision hook was not installed, and no hook armed");
    check(std::memcmp(badFreeBefore, badFree, 40) == 0 && std::memcmp(colBefore, goodCol, 40) == 0 && !t::placeActive() && !t::gateOpen(),
          "...neither function was written, placement is not active, the gate is closed");
    t::reset();

    // ---- a wrong collision prologue: the collision hook stands down and placement does not run ----------------------------------
    cap.clear();
    w.init();
    t::setTargets(reinterpret_cast<uintptr_t>(goodFree), reinterpret_cast<uintptr_t>(badCol));
    t::boundary(6, 9000, true, 1.68f, 0.10f, 0.0f, &Capture::add, &cap);
    check(cap.count("collision hook stood down:") == 1 && has(cap.nth("collision hook stood down:", 0), "collision step prologue") &&
              has(cap.nth("collision hook stood down:", 0), "nothing was patched") && cap.count("collision hook armed:") == 0,
          "WRONG COLLISION PROLOGUE: one 'collision hook stood down' line");
    check(std::memcmp(badColBefore, badCol, 40) == 0 && !t::placeActive(), "...the collision function was not written and placement is not active");
    w.frame(); w.frame(); w.frame();
    check(w.seenInt() == 0 && w.pose(12) == 0.02f && t::placedActivity() == 0, "...a free-camera update through its (armed, idle) hook writes nothing: no pose, no press, nothing placed");
    t::reset();

    // ---- the real path ----------------------------------------------------------------------------------------------------------
    cap.clear();
    w.init();
    other.init();
    t::setTargets(reinterpret_cast<uintptr_t>(goodFree), reinterpret_cast<uintptr_t>(goodCol));
    t::boundary(10, 20000, true, 1.68f, 0.10f, 0.0f, &Capture::add, &cap);
    check(cap.count("explorer cam: on (fix.explorer_cam = on)") == 1 && cap.count("free-camera hook armed:") == 1 && cap.count("collision hook armed:") == 1,
          "ARMED: one 'on' line, one 'free-camera hook armed' line, one 'collision hook armed' line");
    check(has(cap.nth("free-camera hook armed:", 0), "stolen=5 bytes") && has(cap.nth("free-camera hook armed:", 0), "28/28 bytes verified") &&
              has(cap.nth("collision hook armed:", 0), "stolen=5 bytes") && has(cap.nth("collision hook armed:", 0), "26/26 bytes verified"),
          "...both stole 5 bytes after verifying 28/28 and 26/26 prologue bytes");
    check(t::freeStolen() == 5 && t::collisionStolen() == 5 && goodFree[0] == 0xE9 && goodCol[0] == 0xE9 && t::gateOpen() && t::placeActive(),
          "both functions now begin with E9 (the relay jump); the gate is open and placement is active");
    check(cap.prefixed() == cap.lines.size(), "every line starts 'explorer cam:'");

    // The first update: +0x48C = 0, +0x473 = 1. Nothing is placed; the hook was reached and says so.
    cap.clear();
    w.frame();
    check(w.seenCalls() == 1 && w.seenInt() == 0 && t::placedActivity() == 0 && t::phase() == 0 && w.pose(12) == 0.02f,
          "FIRST UPDATE (+0x48C=0, +0x473=1): the original ran, no pose written, the lock not pressed, nothing placed");
    t::boundary(11, 20100, true, 1.68f, 0.10f, 0.0f, &Capture::add, &cap);
    check(cap.count("first free-camera update reached the hook") == 1, "...and 'first free-camera update reached the hook' is logged: reached is distinguishable from never reached");

    // The second update: +0x48C = 3, +0x473 = 0. Entry, placement, the press.
    cap.clear();
    w.pressedInt() = 0;
    const uint64_t bypassedBefore = t::bypassed();
    w.frame();
    check(w.seenCalls() == 2, "SECOND UPDATE: the original ran");
    check(closeTo(w.seen(kSeenX), 0.0f) && closeTo(w.seen(kSeenY), 1.68f) && closeTo(w.seen(kSeenZ), 0.10f),
          "...it SAW the placed pose: origin (right 0.00, up 1.68, forward 0.10) written BEFORE the original");
    check(w.seenInt() == 1, "...it SAW the lock action's pressed-int = 1 (the press, set before the original)");
    check(w.pressedInt() == 0, "...and the pressed-int is RESTORED to its previous value (0) after the original returned");
    check(t::placedActivity() == reinterpret_cast<uint64_t>(w.a()) && t::phase() == 2, "the activity is published as placed");
    // The rows the original saw were the identity (the rig re-derived the origin after the call, but the rotation is ours).
    check(w.pose(0) == 1.0f && w.pose(5) == 1.0f && w.pose(10) == 1.0f && w.pose(1) == 0.0f && w.pose(2) == 0.0f && w.pose(4) == 0.0f && w.pose(6) == 0.0f &&
              w.pose(8) == 0.0f && w.pose(9) == 0.0f && w.pose(3) == 0.5f && w.pose(7) == 0.25f && w.pose(11) == 0.125f && w.pose(15) == 1.0f,
          "...rows right/up/forward are the identity and each row's 4th float is still the game's (0.5, 0.25, 0.125, 1.0)");
    check(w.state() == 4, "(the rig's game moved +0x48C 3 -> 4 because the update saw the press)");

    // The collision relay, for the placed activity and for another.
    const uint32_t ranBefore = w.ran(), otherRanBefore = other.ran();
    const uint64_t viaPlaced = collision(w.a(), 1000, 20, 3, 0xA5);
    check(viaPlaced == 0 && w.ran() == ranBefore && t::bypassed() == bypassedBefore + 1,
          "COLLISION, placed activity (rcx == placed): returns 0 WITHOUT running the function; the bypass is counted");
    const uint64_t viaOther = collision(other.a(), 1000, 20, 3, 0xA5);
    check(viaOther == (1000ull + 20 + 3 + (0xA5ull << 32)) && other.ran() == otherRanBefore + 1,
          "COLLISION, any other rcx: runs the function and returns its value, the FIFTH stack argument (0xA5) intact");
    check(t::forwarded() >= 1, "...and the forward is counted");
    const uint64_t viaOther2 = collision(other.a(), 7, 8, 9, 0x3C);
    check(viaOther2 == (7ull + 8 + 9 + (0x3Cull << 32)), "...with another set of arguments: rdx, r8, r9 and the stack byte all arrive");

    cap.clear();
    t::boundary(12, 20200, true, 1.68f, 0.10f, 0.0f, &Capture::add, &cap);
    check(cap.count("explorer cam: entered the free camera") == 1 && cap.count("explorer cam: placed:") == 1 &&
              has(cap.nth("explorer cam: placed:", 0), "eye(up=1.680 forward=0.100 right=0.000)") && has(cap.nth("explorer cam: placed:", 0), "act=0x"),
          "LOG: 'entered the free camera' and 'placed:' with the activity and the eye");
    check(cap.count("lock pressed:") == 0, "...the lock result is NOT logged yet (it is read on the next call)");

    // The third update: +0x48C = 4. The pose is written again (the rig re-derived 0.70 after the last call), no press.
    cap.clear();
    w.frame();
    check(closeTo(w.seen(kSeenZ), 0.10f) && closeTo(w.seen(kSeenY), 1.68f) && w.seenInt() == 0 && w.seenState() == 4,
          "THIRD UPDATE (+0x48C=4): the original saw the eye pose again (the game had pushed it to 0.70), the pressed-int 0: NO second press");
    t::boundary(13, 20300, true, 1.68f, 0.10f, 0.0f, &Capture::add, &cap);
    check(cap.count("lock pressed:") == 1 && has(cap.nth("lock pressed:", 0), "before=3 after=4") && has(cap.nth("lock pressed:", 0), "relative lock is on"),
          "LOG: 'lock pressed: ... before=3 after=4' (read on this call)");

    // The eye keys are live.
    cap.clear();
    t::boundary(14, 20400, true, 1.60f, 0.20f, 0.05f, &Capture::add, &cap);
    w.frame();
    check(cap.count("eye changed:") == 1 && has(cap.nth("eye changed:", 0), "up=1.600 forward=0.200 right=0.050"), "LIVE EYE: a changed key logs one 'eye changed' line");
    check(closeTo(w.seen(kSeenX), 0.05f) && closeTo(w.seen(kSeenY), 1.60f) && closeTo(w.seen(kSeenZ), 0.20f), "...and the very next update saw the new origin (right 0.05, up 1.60, forward 0.20)");
    t::boundary(15, 20500, true, 9.0f, 9.0f, -9.0f, &Capture::add, &cap);
    w.frame();
    check(closeTo(w.seen(kSeenX), -0.5f) && closeTo(w.seen(kSeenY), 2.5f) && closeTo(w.seen(kSeenZ), 0.5f), "...and out-of-range keys are clamped (up 2.5, forward 0.5, right -0.5)");
    t::boundary(16, 20600, true, 1.68f, 0.10f, 0.0f, &Capture::add, &cap);

    // The user unlocks (4 -> 3): placing continues and the lock is not pressed again.
    w.state() = 3;
    w.frame();
    check(w.seenInt() == 0 && closeTo(w.seen(kSeenY), 1.68f) && w.seenState() == 3 && t::phase() == 2,
          "USER UNLOCK (+0x48C=3): still placing, the pressed-int stays 0 (not pressed again)");
    w.state() = 4;

    // The heartbeat while placed, then the numbers in it.
    cap.clear();
    t::boundary(17, 25500, true, 1.68f, 0.10f, 0.0f, &Capture::add, &cap);
    check(cap.count("explorer cam: heartbeat:") == 1, "HEARTBEAT: one line after 5 s while placed");
    const std::string beat = cap.nth("explorer cam: heartbeat:", 0);
    check(has(beat, "phase=placed") && has(beat, "updates_placed=") && has(beat, "collision_bypassed=1(") && has(beat, "collision_forwarded=2(") && has(beat, "faults=0"),
          "...naming updates placed, collision calls bypassed (1) and forwarded (2), faults (0)");
    cap.clear();
    t::boundary(18, 26000, true, 1.68f, 0.10f, 0.0f, &Capture::add, &cap);
    check(cap.count("explorer cam: heartbeat:") == 0, "...and not on the next frame");

    // A silent activity: released after 30 frames, with the collision step the game's again. One more call and a boundary first, so
    // the count of silent frames starts at zero.
    w.frame();
    t::boundary(99, 26050, true, 1.68f, 0.10f, 0.0f, &Capture::add, &cap);
    cap.clear();
    const uint64_t ranA = w.ran();
    for (int i = 0; i < 29; ++i) t::boundary(100 + i, 26100 + i * 11, true, 1.68f, 0.10f, 0.0f, &Capture::add, &cap);
    check(t::placedActivity() != 0 && cap.count("released:") == 0, "STALE: 29 silent frames: still placed");
    t::boundary(130, 26500, true, 1.68f, 0.10f, 0.0f, &Capture::add, &cap);
    check(t::placedActivity() == 0 && t::phase() == 0 && cap.count("released:") == 1 && has(cap.nth("released:", 0), "not called for 30 frames"),
          "...the 30th releases it: one 'released: ... not called for 30 frames' line, nothing placed");
    const uint64_t viaStale = collision(w.a(), 5, 6, 7, 0x11);
    check(viaStale == (5ull + 6 + 7 + (0x11ull << 32)) && w.ran() == ranA + 1, "...and the collision step runs for that activity again");

    // The activity is called again (state 4, user-locked state it ended in): idle, no re-entry at 4.
    w.frame();
    check(t::placedActivity() == 0 && closeTo(w.seen(kSeenZ), 0.70f), "after the release a call at +0x48C=4 does not place again (the update saw the game's own 0.70, the session began without us)");

    // The world lock: a new session, then 5.
    w.init();
    cap.clear();
    w.frame(); w.frame();   // pending first update, then state 3: entry + press (the machine was reset when the stale release ran)
    t::boundary(131, 27000, true, 1.68f, 0.10f, 0.0f, &Capture::add, &cap);
    check(t::placedActivity() == reinterpret_cast<uint64_t>(w.a()) && cap.count("explorer cam: placed:") == 1, "a NEW session after a release places again");
    w.frame();
    w.state() = 5;
    w.pose(12) = 0.31f; w.pose(13) = 0.32f; w.pose(14) = 0.33f;
    cap.clear();
    w.frame();
    t::boundary(132, 27100, true, 1.68f, 0.10f, 0.0f, &Capture::add, &cap);
    check(t::placedActivity() == 0 && closeTo(w.seen(kSeenX), 0.31f) && closeTo(w.seen(kSeenY), 0.32f) && cap.count("released:") == 1 &&
              has(cap.nth("released:", 0), "world lock"),
          "WORLD LOCK (+0x48C=5): released at once, the pose is NOT written (the original saw the game's 0.31/0.32/0.33), one 'world lock' line");
    w.state() = 3;
    w.frame();
    check(t::placedActivity() == 0 && closeTo(w.seen(kSeenY), 1.70f), "...and +0x48C back at 3 does not place again");

    // +0x48C = 0, then a new session with a NON-ZERO previous pressed-int: restored after the press.
    w.state() = 0;
    w.frame();
    w.activity[0x473] = 1;
    w.frame();   // pending first update of the new session
    w.pressedInt() = 7;
    cap.clear();
    w.frame();   // +0x48C = 3: entry, place, press
    check(w.seenInt() == 1 && w.pressedInt() == 7 && t::placedActivity() == reinterpret_cast<uint64_t>(w.a()),
          "NEW SESSION: the original saw the pressed-int = 1 and it was restored to its previous value 7 (not zeroed)");
    w.pressedInt() = 0;

    // The key turns off while placed.
    cap.clear();
    t::boundary(133, 28000, false, 1.68f, 0.10f, 0.0f, &Capture::add, &cap);
    check(t::placedActivity() == 0 && t::phase() == 0 && !t::placeActive() && cap.count("released:") == 1 && has(cap.nth("released:", 0), "turned off"),
          "KEY OFF while placed: released at once ('fix.explorer_cam turned off'), nothing placed, placement inactive");
    check(cap.count("explorer cam: off (fix.explorer_cam turned off while running)") == 1, "...and one 'off (turned off while running)' line");
    const uint64_t callsOff = t::hookCalls();
    w.pose(12) = 0.4f; w.pose(13) = 0.5f; w.pose(14) = 0.6f;
    w.frame();
    check(closeTo(w.seen(kSeenY), 0.5f) && t::hookCalls() == callsOff, "...a call now runs straight through to the original: no pose written, the hook body not reached (gate closed)");
    check(!t::gateOpen(), "...the relay's gate is closed");

    // Key back on: the machine starts over and the next +0x48C = 3 enters.
    cap.clear();
    w.state() = 3;
    t::boundary(134, 29000, true, 1.68f, 0.10f, 0.0f, &Capture::add, &cap);
    w.frame();
    check(t::placedActivity() == reinterpret_cast<uint64_t>(w.a()) && closeTo(w.seen(kSeenY), 1.68f) && cap.count("explorer cam: on again") == 1,
          "KEY BACK ON at +0x48C=3: 'on again', and the next update places");

    // The probe's observer rides the same hook, after the restore.
    static std::atomic<int> observed;
    static std::atomic<int32_t> pressedAtObserver;
    static World* observedWorld;
    observed = 0;
    observedWorld = &w;
    auto observer = [](void* a) noexcept { observed.fetch_add(1); pressedAtObserver = observedWorld->pressedInt(); (void)a; };
    const ExplorerCamHookStatus status = explorerCamProbeAttach(true, observer);
    check(status.state == ExplorerCamHookStatus::Armed && status.stolen == 5 && status.target == reinterpret_cast<uintptr_t>(goodFree), "PROBE ATTACH: reports the armed hook (5 stolen bytes)");
    w.init();
    w.frame();
    w.state() = 3; w.activity[0x473] = 0;
    w.pressedInt() = 9;
    w.frame();
    check(observed.load() == 2 && pressedAtObserver.load() == 9, "...the observer ran after each original, and the lock's int was already restored (9) when it ran");
    explorerCamProbeAttach(false, nullptr);

    // Faults: guarded, counted, released; the eighth ends the feature. These cells drive the hook's pre and post halves directly
    // (preThenPost), because the synthetic update itself would fault on the same bad pointer.
    cap.clear();
    t::boundary(135, 30000, true, 1.68f, 0.10f, 0.0f, &Capture::add, &cap);
    w.init();
    t::preThenPost(w.a());   // +0x48C = 0, +0x473 = 1: the first update of a new session; the old one ends, nothing is placed
    w.state() = 3; w.activity[0x473] = 0;
    const uint64_t notAnObject = 0x30;
    std::memcpy(w.a() + 0x508, &notAnObject, 8);   // a lock action pointer that is not an object
    const uint32_t faultsBefore = t::faults();
    t::preThenPost(w.a());
    t::boundary(136, 30100, true, 1.68f, 0.10f, 0.0f, &Capture::add, &cap);
    check(t::faults() == faultsBefore + 1 && cap.count("explorer cam: fault 1 of 8:") == 1 && has(cap.nth("explorer cam: fault 1 of 8:", 0), "lock press") && t::placedActivity() == 0 && t::phase() == 0,
          "FAULT: a lock action pointer that is not an object is a counted fault ('fault 1 of 8: ... lock press'), the session released, nothing placed");
    check(std::memcmp(w.a() + 0x508, &notAnObject, 8) == 0 && w.pressedInt() == 0, "...and nothing was written through that pointer");
    cap.clear();
    for (int i = 0; i < 6; ++i) {
        t::preThenPost(reinterpret_cast<void*>(static_cast<uintptr_t>(0x10 + 0x10 * i)));
        t::boundary(140 + i, 31000 + i * 16, true, 1.68f, 0.10f, 0.0f, &Capture::add, &cap);
    }
    check(t::faults() == 7 && cap.count("explorer cam: fault ") == 6 && cap.count("stood down for the session") == 0, "SIX more bad activity pointers: faults 7, six more fault lines, the budget not yet spent");
    t::preThenPost(reinterpret_cast<void*>(static_cast<uintptr_t>(0x500)));
    t::boundary(150, 32000, true, 1.68f, 0.10f, 0.0f, &Capture::add, &cap);
    check(t::faults() == 8 && cap.count("explorer cam: fault 8 of 8:") == 1 && cap.count("stood down for the session") == 1 && !t::placeActive(),
          "THE EIGHTH FAULT: one 'stood down for the session' line, placement inactive");
    t::preThenPost(reinterpret_cast<void*>(static_cast<uintptr_t>(0x600)));
    t::boundary(151, 32100, true, 1.68f, 0.10f, 0.0f, &Capture::add, &cap);
    check(t::faults() == 8 && cap.count("stood down for the session") == 1, "...and nothing more is counted or said");
    w.init();
    w.frame(); w.state() = 3; w.activity[0x473] = 0; w.frame();
    check(t::placedActivity() == 0 && w.seenInt() == 0, "...a good activity is no longer placed or pressed after the stand-down");

    t::reset();
    check(std::memcmp(goodFree, goodFreeCode.data(), 5) == 0 && std::memcmp(goodCol, goodColCode.data(), 5) == 0,
          "RESET: uninstall put both functions' original five bytes back");
}

// The config path: the keys under their real names.
void testConfigPath() {
    std::printf("the config keys, read the way the frame boundary reads them\n");
    namespace t = edvr::explorercamtest;
    const Bytes freeCode = buildFreeSynthetic(false), colCode = buildCollisionSynthetic(false);
    uint8_t* freePage = makeExecutable(freeCode);
    uint8_t* colPage = makeExecutable(colCode);
    check(freePage && colPage, "two executable pages for the synthetic functions");
    if (!freePage || !colPage) return;
    static World w;
    w.init();
    w.update = reinterpret_cast<FreeFn>(freePage);
    Config& cfg = Config::get();
    // Defaults: nothing set. The ini ships `explorer_cam = on` and these three numbers, and the code defaults agree.
    t::setTargets(reinterpret_cast<uintptr_t>(freePage), reinterpret_cast<uintptr_t>(colPage));
    explorerCamFrameBoundary(1);
    check(t::placeActive(), "no keys set: Explorer Cam is ON by default");
    w.frame(); w.frame();
    check(closeTo(w.seen(kSeenX), 0.0f) && closeTo(w.seen(kSeenY), 1.68f) && closeTo(w.seen(kSeenZ), 0.10f), "...with the eye at up 1.68, forward 0.10, right 0.0");
    cfg.set("fix.explorer_cam_eye_up", "1.55");
    cfg.set("fix.explorer_cam_eye_forward", "0.25");
    cfg.set("fix.explorer_cam_eye_right", "-0.04");
    explorerCamFrameBoundary(2);
    w.frame();
    check(closeTo(w.seen(kSeenX), -0.04f) && closeTo(w.seen(kSeenY), 1.55f) && closeTo(w.seen(kSeenZ), 0.25f),
          "fix.explorer_cam_eye_up / _eye_forward / _eye_right reach the hook (1.55, 0.25, -0.04)");
    cfg.set("fix.explorer_cam", "off");
    explorerCamFrameBoundary(3);
    check(!t::placeActive() && t::placedActivity() == 0, "fix.explorer_cam = off: placement inactive, nothing placed");
    cfg.set("fix.explorer_cam", "on");
    explorerCamFrameBoundary(4);
    check(t::placeActive(), "fix.explorer_cam = on: active again");
    t::reset();
    cfg.set("fix.explorer_cam", "");
    cfg.set("fix.explorer_cam_eye_up", "");
    cfg.set("fix.explorer_cam_eye_forward", "");
    cfg.set("fix.explorer_cam_eye_right", "");
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
    testMachinePending();
    testMachineReleases();
    testMachineForeign();
    testStaleWatch();
    testRing();
    testText();
    testRelayBytes();
    testGlue();
    testConfigPath();
    if (g_failures) {
        std::printf("explorer cam: FAIL (%d)\n", g_failures);
        return 1;
    }
    std::printf("explorer cam: PASS\n");
    return 0;
}
