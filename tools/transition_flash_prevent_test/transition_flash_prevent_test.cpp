// Build gate for the transition-flash engine fix's pure logic
// (transition_flash_eye_base_core.h): the bit-exact reset-mailbox match, the
// ENTRY/EXIT classifier (a low wake's 5,041-consume idle stretch must name its
// edges once, not every consume), the event window and the two-frame act
// window, and the head-only eye test. Also the Dr7 slot-0 composition
// (debug_register_core.h) the flat camera probe arms its write watch with, and
// glitch_scene.h's print text. No game, no Windows hook -- this drives the
// headers directly, the kinematic_probe_test pattern (single-TU, production
// source compiled into the rig). The file keeps its old name: build.bat and
// the self-test gate know it by it.
//
// What used to be here and is gone with the code it tested (Build 2d,
// 2026-10-09): the pose-tracking classifier and guard table, the writer
// watch, the render-time patch maths, the view locator and the dump
// triggers -- see the design doc.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include "../../src/d3d11/debug_register_core.h"
#include "../../src/d3d11/transition_flash_eye_base_core.h"
#include "../../src/d3d11/glitch_scene.h"

namespace dr7 = edvr::dr7;
namespace tfeb = edvr::tfeb;

namespace {
unsigned checks = 0, failures = 0;
void check(bool ok, const char* name) {
    ++checks;
    if (!ok) { ++failures; std::fprintf(stderr, "FAIL: %s\n", name); }
}

void caseDr7ArmLeavesOtherSlotsAlone() {
    check(dr7::armSlot0Dr7(0) == 0x000F0001u, "Dr7 arm: from a clear register, exactly L0/RW0/LEN0 come on");
    // Slot 1 already armed (L1=bit2, RW1=bits20-21, LEN1=bits22-23) plus a
    // reserved bit (10) some other debugger set: none of that is this
    // module's to touch.
    const uint32_t otherSlotBits = (1u << 2) | (0x3u << 20) | (0x3u << 22) | (1u << 10);
    const uint32_t armed = dr7::armSlot0Dr7(otherSlotBits);
    check((armed & ~dr7::kDr7Slot0Mask) == otherSlotBits,
          "Dr7 arm: every bit outside slot 0's mask survives untouched");
    check((armed & dr7::kDr7Slot0Mask) == dr7::kDr7Slot0ArmedBits,
          "Dr7 arm: slot 0's own bits read exactly L0=1, RW0=11b, LEN0=11b");
}

void caseDr7DisarmClearsOnlyL0() {
    const uint32_t armed = dr7::armSlot0Dr7(0);
    const uint32_t disarmed = dr7::disarmSlot0Dr7(armed);
    check((disarmed & dr7::kDr7L0Bit) == 0, "Dr7 disarm: L0 comes off");
    check((disarmed & ~dr7::kDr7L0Bit) == (armed & ~dr7::kDr7L0Bit),
          "Dr7 disarm: RW0/LEN0 and every other bit are left exactly as they were");
    // Idempotent, and never disturbs a DIFFERENT slot's enable bit.
    const uint32_t withSlot2 = armed | (1u << 4);  // L2
    check((dr7::disarmSlot0Dr7(withSlot2) & (1u << 4)) != 0,
          "Dr7 disarm: another slot's own enable bit is not this call's to clear");
}

void caseDr7ArmWriteModeSlot0() {
    const uint32_t armed = dr7::armSlot0Dr7(0, dr7::kDr7RwWrite);
    check((armed & dr7::kDr7L0Bit) != 0, "Dr7 write-mode arm: L0 comes on");
    check(((armed >> 16) & 0x3u) == dr7::kDr7RwWrite, "Dr7 write-mode arm: RW0 reads back 01b, not 11b");
    check(((armed >> 18) & 0x3u) == dr7::kDr7Len4Bytes, "Dr7 write-mode arm: LEN0 is still 4 bytes");

    // Slot 1 already armed (L1=bit2, RW1=bits20-21, LEN1=bits22-23) plus a
    // reserved bit (bit10): every one of those must survive untouched, the
    // same property caseDr7ArmLeavesOtherSlotsAlone asserts for the
    // default read-or-write mode.
    const uint32_t otherSlotBits = (1u << 2) | (0x3u << 20) | (0x3u << 22) | (1u << 10);
    const uint32_t armedWithOthers = dr7::armSlot0Dr7(otherSlotBits, dr7::kDr7RwWrite);
    check((armedWithOthers & ~dr7::kDr7Slot0Mask) == otherSlotBits,
          "Dr7 write-mode arm: every bit outside slot 0's mask survives untouched");
    check(((armedWithOthers >> 16) & 0x3u) == dr7::kDr7RwWrite,
          "Dr7 write-mode arm: RW0 stays 01b even with other slots live");

    // The default argument reproduces the pose path's own read-or-write
    // shape exactly -- the "keep the pose path's behaviour identical" half
    // of the refactor.
    check(dr7::armSlot0Dr7(0) == dr7::armSlot0Dr7(0, dr7::kDr7RwReadWrite),
          "Dr7 arm: the default RW mode is still read-or-write");
}

void eyeBaseIdentityMailbox(float m[16]) {
    static constexpr float kIdentity[16] = {
        1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
    std::memcpy(m, kIdentity, sizeof(kIdentity));
}

float bitsToFloat(uint32_t bits) {
    float f;
    std::memcpy(&f, &bits, sizeof(f));
    return f;
}

void caseResetMailboxBitExact() {
    float m[16];
    eyeBaseIdentityMailbox(m);
    check(tfeb::isResetMailbox(m), "resetMailbox: the exact constant matches");

    // -0.0f vs +0.0f: IEEE == calls these equal, but their bits differ
    // (0x80000000 vs 0x00000000) -- a writer that stored -0.0 where the
    // constant holds +0.0 has still WRITTEN, and this must see that.
    eyeBaseIdentityMailbox(m);
    m[3] = bitsToFloat(0x80000000u);  // a padding lane, -0.0 instead of +0.0
    check(!tfeb::isResetMailbox(m), "resetMailbox: -0.0 where the constant holds +0.0 does not match");
    check(bitsToFloat(0x80000000u) == 0.0f, "resetMailbox: precondition -- IEEE == would have called this a match");

    // A NaN lane must not match either -- bit-different from any finite
    // constant value regardless of which lane it lands in.
    eyeBaseIdentityMailbox(m);
    m[12] = bitsToFloat(0x7FC00000u);  // quiet NaN in the translation row
    check(!tfeb::isResetMailbox(m), "resetMailbox: a NaN lane does not match");

    // Not tolerances: one ULP off a real value must not match either.
    eyeBaseIdentityMailbox(m);
    m[0] = bitsToFloat(0x3F800001u);  // 1.0f plus one ULP
    check(!tfeb::isResetMailbox(m), "resetMailbox: compares bits, not a tolerance");

    eyeBaseIdentityMailbox(m);
    check(tfeb::isResetMailbox(m), "resetMailbox: unchanged again after the mutating cases above");
}

void caseModeSwitchEdgeEntryFiresOnce() {
    uint32_t c = 0;
    // A refilled call from a zero streak is not an edge -- nothing to enter.
    check(tfeb::classifyModeSwitchEdge(/*gameMode=*/2, /*unrefilled=*/false, c) == tfeb::ModeSwitchEdge::None,
          "modeSwitchEdge: a refilled call from a zero streak is not an edge");

    // The first un-refilled call after a refilled one: ENTRY.
    check(tfeb::classifyModeSwitchEdge(2, true, c) == tfeb::ModeSwitchEdge::Entry,
          "modeSwitchEdge: the first un-refilled call after a refilled one is ENTRY");
    c = tfeb::updateConsecutiveUnrefilled(c, 2, true);
    check(c == 1, "modeSwitchEdge: the streak counter reaches 1 after that call");

    // The second consecutive un-refilled call is not another entry -- entry
    // fires once per run, not on every call inside it.
    check(tfeb::classifyModeSwitchEdge(2, true, c) == tfeb::ModeSwitchEdge::None,
          "modeSwitchEdge: entry fires once -- the second un-refilled call in the run is not an edge");
}

void caseModeSwitchEdgeNoneInsideRun() {
    uint32_t c = 1;  // the entry has already fired; this run is under way
    for (int i = 0; i < 28; ++i) {
        check(tfeb::classifyModeSwitchEdge(2, true, c) == tfeb::ModeSwitchEdge::None,
              "modeSwitchEdge: no trigger inside an un-refilled run");
        c = tfeb::updateConsecutiveUnrefilled(c, 2, true);
    }
    check(c == 29, "modeSwitchEdge: 29 consecutive un-refilled calls (1 entry + 28 more, still no exit)");
}

void caseModeSwitchEdgeExitAfterThirty() {
    // One short of the exit run length: a refilled call here is not an exit.
    uint32_t c29 = 0;
    for (int i = 0; i < 29; ++i) c29 = tfeb::updateConsecutiveUnrefilled(c29, 2, true);
    check(c29 == 29, "modeSwitchEdge: 29 consecutive un-refilled calls, one short of the exit run");
    check(tfeb::classifyModeSwitchEdge(2, false, c29) == tfeb::ModeSwitchEdge::None,
          "modeSwitchEdge: exit fires only after >= 30 -- a refilled call after 29 is not one");

    // Exactly 30: the next refilled call IS the exit.
    uint32_t c30 = c29;
    c30 = tfeb::updateConsecutiveUnrefilled(c30, 2, true);
    check(c30 == 30, "modeSwitchEdge: 30 consecutive un-refilled calls reaches the exit run length");
    check(tfeb::classifyModeSwitchEdge(2, false, c30) == tfeb::ModeSwitchEdge::Exit,
          "modeSwitchEdge: a refilled call after 30 consecutive un-refilled ones is EXIT");

    // A mode==1 call neither breaks nor extends the run, and is never itself
    // an edge -- the same exclusion updateConsecutiveRefilled already
    // applies to the refilled-streak counter.
    const uint32_t afterMode1 = tfeb::updateConsecutiveUnrefilled(c30, /*gameMode=*/1, /*unrefilled=*/true);
    check(afterMode1 == c30, "modeSwitchEdge: a mode==1 call leaves the streak untouched");
    check(tfeb::classifyModeSwitchEdge(1, true, c30) == tfeb::ModeSwitchEdge::None,
          "modeSwitchEdge: a mode==1 call is never itself an edge");
}

void caseModeSwitchEdgeSingleFrameSkip() {
    uint32_t c = 0;
    // Refilled, then a single un-refilled frame: ENTRY.
    check(tfeb::classifyModeSwitchEdge(2, true, c) == tfeb::ModeSwitchEdge::Entry,
          "modeSwitchEdge: a single-frame skip's own un-refilled call is ENTRY");
    c = tfeb::updateConsecutiveUnrefilled(c, 2, true);
    check(c == 1, "modeSwitchEdge: the skip's streak reaches 1");

    // Refilled again immediately: not an exit -- the run never reached 30.
    check(tfeb::classifyModeSwitchEdge(2, false, c) == tfeb::ModeSwitchEdge::None,
          "modeSwitchEdge: a single-frame skip's recovery fires no exit (run length 1 < 30)");
}

void caseSceneGeometryFreshAndDecisionText() {
    check(std::strcmp(edvr::glitchSceneGeometryFreshText(true), "") == 0,
          "sceneGeometryFreshText: a fresh record prints no prefix");
    check(std::strcmp(edvr::glitchSceneGeometryFreshText(false), "STALE-") == 0,
          "sceneGeometryFreshText: a stale-geometry record prints as not fresh");
    check(std::strcmp(edvr::glitchSceneDecisionText(edvr::GlitchSceneDecision::Unknown), "unknown") == 0,
          "sceneDecisionText: Unknown prints as unknown");
    check(std::strcmp(edvr::glitchSceneDecisionText(edvr::GlitchSceneDecision::Coherent), "coherent") == 0,
          "sceneDecisionText: Coherent prints as coherent");
    check(std::strcmp(edvr::glitchSceneDecisionText(edvr::GlitchSceneDecision::CameraReset), "cameraReset") == 0,
          "sceneDecisionText: CameraReset prints as cameraReset");
}

void caseEngineActFrame() {
    // A one-frame skip at S=100: the consume of 100 was the only gap consume.
    check(tfeb::engineActFrame(101, 100, 100), "engineActFrame: skip+1 acts");
    check(!tfeb::engineActFrame(100, 100, 100), "engineActFrame: the skip frame's own tap is not a bad render");
    check(!tfeb::engineActFrame(102, 100, 100), "engineActFrame: skip+2 does not act when the gap closed");
    // A two-frame gap (flight 050558's 15117): consume 101 was a gap consume too.
    check(tfeb::engineActFrame(101, 100, 101) && tfeb::engineActFrame(102, 100, 101),
          "engineActFrame: a still-open gap acts on skip+1 and skip+2");
    // A long gap (supercruise): never more than two frames.
    check(!tfeb::engineActFrame(103, 100, 5000), "engineActFrame: at most two frames per event");
    check(tfeb::engineActFrame(102, 100, 5000), "engineActFrame: a long gap still acts on skip+2");
    check(!tfeb::engineActFrame(5, 100, 100), "engineActFrame: a frame before the skip never acts");
    // A stale gapLastConsume from an earlier event cannot open this one.
    check(!tfeb::engineActFrame(101, 100, 90), "engineActFrame: a gap marker older than the skip does not act");
    check(tfeb::kEngineMaxFrames == 2, "engineActFrame: the spec's cap is 2");
    // Wraparound-safe at the top of the range.
    check(!tfeb::engineActFrame(0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu), "engineActFrame: skip+1 overflow does not act");
}

void caseEventWindow() {
    // Armed at the skip frame N, the event covers exactly the taps N+1..N+3
    // (the bad render is N+1; one frame of skew slack either side).
    check(!tfeb::eventWindowCovers(13547, 13547), "eventWindow: the skip frame itself is not covered");
    check(tfeb::eventWindowCovers(13548, 13547), "eventWindow: N+1 is covered");
    check(tfeb::eventWindowCovers(13549, 13547), "eventWindow: N+2 is covered");
    check(tfeb::eventWindowCovers(13550, 13547), "eventWindow: N+3 is covered");
    check(!tfeb::eventWindowCovers(13551, 13547), "eventWindow: N+4 is past the window");
    check(!tfeb::eventWindowCovers(5, 13547), "eventWindow: a frame before the skip is not covered");
}

void caseHeadOnlyEye() {
    const float head[3] = {0.05f, -0.016f, 0.016f};          // the bad frame's row 275 (flight 060955)
    const float edge[3] = {0.99f, 0.0f, 0.0f};
    const float out[3] = {1.0f, 0.0f, 0.0f};                  // exactly 1 m is not under it
    const float world[3] = {6.119f, -3.432f, 11.511f};        // an ordinary eye origin
    const float nan3[3] = {NAN, 0.0f, 0.0f};
    check(tfeb::isHeadOnlyEye(head), "headOnly: the head pose alone is head-only");
    check(tfeb::isHeadOnlyEye(edge), "headOnly: 0.99 m is still under a metre");
    check(!tfeb::isHeadOnlyEye(out), "headOnly: exactly 1 m is not");
    check(!tfeb::isHeadOnlyEye(world), "headOnly: a world-space eye origin is not");
    check(!tfeb::isHeadOnlyEye(nan3), "headOnly: NaN is never head-only");
    check(tfeb::kSceneCBBytes == 5376 && tfeb::kSceneCBOriginFloat == 1100, "scene CB: 5376 bytes, cb1[275] at float 1100");
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc != 2) return 2;
    if (!std::wcscmp(argv[1], L"--dry-run")) {
        std::puts("transition_flash_prevent_test: dry-run (no runtime, device or files)");
        return 0;
    }
    if (std::wcscmp(argv[1], L"--self-test")) return 2;
    caseDr7ArmLeavesOtherSlotsAlone();
    caseDr7DisarmClearsOnlyL0();
    caseDr7ArmWriteModeSlot0();
    caseResetMailboxBitExact();
    caseModeSwitchEdgeEntryFiresOnce();
    caseModeSwitchEdgeNoneInsideRun();
    caseModeSwitchEdgeExitAfterThirty();
    caseModeSwitchEdgeSingleFrameSkip();
    caseSceneGeometryFreshAndDecisionText();
    caseEngineActFrame();
    caseEventWindow();
    caseHeadOnlyEye();
    std::printf("transition_flash_prevent_test: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
