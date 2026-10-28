// Native on foot's avatar hooks rig (src/d3d11/native_on_foot_hooks.cpp, avatar_hooks_core.h, avatar_skeleton_core.h).
//
// Part A drives the pure half the DLL compiles: the build-334925 identity and CodeHook's reading of the three prologues, the avatar fade
// guard, the hook threads' ring, the callback relay's machine code, the head-hiding tables, the local-AMC test, the capture filter and the
// census text.
//
// Part B runs the glue (native_on_foot_hooks.cpp compiled with EDVR_NATIVE_ON_FOOT_HOOKS_TEST) end to end against SYNTHETIC functions that
// begin with the real prologues: the camera controller's update on a page of its own, and the avatar dither fade, FindJoint and the avatar
// attach function at their real RVAs in a synthetic game image. It proves, through the real relays and the real hooks:
//   * nothing is installed while native on foot does not want the hooks; the want published after a boundary acts at the next one; the
//     hooks are installed in order (FindJoint, the fade, the controller), then the part names are checked and the skeleton reads armed;
//   * the capture and the fade hook's gates are open from launch while the key is on, the controller's only on foot;
//   * a wrong prologue (each of the three) stands that hook down with one line and no patch, and the others are armed;
//   * the avatar fade global goes to 0 for native stereo's body, back to -1 when the body ends, in the game's third-person camera and at
//     unload, and someone else's value is left alone;
//   * head hiding zeroes the local third-person avatar's head parts only while the body is on in first person, the capture latches only
//     the local humanoid, and native on foot's camera signal is read after the controller's update and changes nothing.
// This is "what would appear in the log if the new code never ran" made executable.
//
// --self-test runs it and prints "native on foot hooks: PASS" only when every check holds.
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "../../src/common/code_hook.h"
#include "../../src/d3d11/native_on_foot_hooks.h"
#include "../../src/d3d11/avatar_hooks_core.h"
#include "../../src/d3d11/avatar_skeleton_core.h"
#include "../../src/d3d11/native_on_foot.h"   // the native flag and the camera signal the controller hook publishes (F93d)
#include "../../src/d3d11/native_on_foot_logwin.h"

using namespace edvr;

// ---- stubs for what the glue's dependencies call -------------------------------------------------------------------------------
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
    // The index of the first line containing `needle`, or -1.
    int indexOf(const char* needle) const {
        for (size_t i = 0; i < lines.size(); ++i)
            if (has(lines[i], needle)) return static_cast<int>(i);
        return -1;
    }
    size_t prefixed() const {
        size_t n = 0;
        for (const std::string& l : lines) n += l.compare(0, std::strlen(avh::prefix()), avh::prefix()) == 0 ? 1 : 0;
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
    // The bytes spelled out again here (not copied from the header).
    const uint8_t ctl15[15] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18};
    check(sizeof(avh::kControllerPrologue) == 15 && std::memcmp(avh::kControllerPrologue, ctl15, 15) == 0, "the controller prologue is 48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18");
    const uint8_t fade16[16] = {0x4C, 0x8B, 0xDC, 0x53, 0x56, 0x57, 0x48, 0x81, 0xEC, 0x10, 0x01, 0x00, 0x00, 0x48, 0x8B, 0x05};
    check(sizeof(avh::kAvatarFadePrologue) == 16 && std::memcmp(avh::kAvatarFadePrologue, fade16, 16) == 0 && avh::kAvatarFadeRva == 0x3DD8C60 && avh::kAvatarFadeRva % 32 == 0,
          "the avatar dither-fade prologue is 4C 8B DC 53 56 57 48 81 EC 10 01 00 00 48 8B 05 at +0x3DD8C60 (32-byte aligned; the first 5 bytes are whole instructions)");
    const uint8_t find16[16] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0x01, 0x48, 0x8B, 0xFA};
    check(sizeof(avh::kFindJointPrologue) == 16 && std::memcmp(avh::kFindJointPrologue, find16, 16) == 0 && avh::kFindJointRva == 0xFDEBB0 && avh::kFindJointRva % 16 == 0,
          "the skeleton FindJoint prologue is 48 89 5C 24 08 57 48 83 EC 20 48 8B 01 48 8B FA at +0xFDEBB0 (16-byte aligned; the first 5 bytes are one whole instruction)");
    check(avh::kControllerRva == 0x2DF3EA0 && avh::kControllerRva % 8 == 0, "the controller's update is EliteDangerous64.exe+0x2DF3EA0, 8-byte aligned (CodeHook's aligned 8-byte store applies)");
    check(avh::kFadeModeRva == 0x5EA0C78 && avh::kFadeAmountRva == 0x60211F8 && avh::kFadeAuto == -1, "the fade global is +0x5EA0C78 (-1 = auto), its amount float +0x60211F8");
    check(avh::kOffCtlMode == 0x3E0 && avh::kOffCtlPending == 0x3E1, "the controller's mode byte is +0x3E0, its pending byte +0x3E1");
    check(avh::kExpectedTimestamp == 1792704584u && avh::kExpectedImageSize == 104906752u, "the build identity is build 334925's PE timestamp and image size");
    check(std::strcmp(avh::modeText(0), "closed") == 0 && std::strcmp(avh::modeText(1), "suite open on a preset") == 0 && std::strcmp(avh::modeText(3), "free camera") == 0 &&
              std::strcmp(avh::modeText(9), "unknown") == 0,
          "the mode byte's names: 0 closed, 1 and 2 the suite on a preset, 3 the free camera, anything past 6 unknown");
    // CodeHook's decoder against every prologue: the steal lengths the hooks depend on.
    check(stolenOf(ctl15, 15) == 5, "CodeHook steals 5 bytes of the controller prologue (the first mov [rsp+8],rbx)");
    check(stolenOf(fade16, 16) == 5, "...5 of the avatar fade's (mov r11,rsp; push rbx; push rsi)");
    check(stolenOf(find16, 16) == 5, "...5 of FindJoint's (mov [rsp+8],rbx)");
}

void testFadeGuard() {
    std::printf("the avatar fade global: write and restore\n");
    using avh::FadeEvent;
    using Act = avh::FadeStep::Act;
    avh::FadeGuard g;
    avh::FadeIn in;
    in.active = true;
    in.readOk = true;
    in.value = -1;
    avh::FadeStep s = g.step(in);
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
    avh::FadeGuard f;
    avh::FadeIn fi;
    fi.active = true; fi.placed = true; fi.session = true; fi.ctlMode = 3; fi.readOk = true; fi.value = 2;
    s = f.step(fi);
    check(s.act == Act::None && s.ev == FadeEvent::Foreign && !f.ours(), "WRONG INITIAL VALUE (2, not -1): refused, one 'someone else owns it' event, nothing written");
    s = f.step(fi);
    check(s.act == Act::None && s.ev == FadeEvent::None, "...said once, not every frame");
    avh::FadeGuard h;
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
        avh::FadeGuard d;
        avh::FadeIn di;
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
        avh::FadeGuard c;
        avh::FadeIn ci;
        ci.active = true; ci.placed = true; ci.session = true; ci.ctlMode = 4; ci.readOk = true; ci.value = -1;
        c.step(ci);
        ci.value = 7; ci.session = false; ci.placed = false; ci.ctlMode = 0;
        s = c.step(ci);
        check(s.act == Act::None && s.ev == FadeEvent::ChangedUnderUs && !c.ours(), "CHANGED UNDER US (7, not the 0 we wrote): the restore is SKIPPED and said, and it is no longer ours");
    }
    // Unreadable: kept and retried, said once.
    {
        avh::FadeGuard c;
        avh::FadeIn ci;
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
        avh::FadeGuard w;
        avh::FadeIn wi;
        wi.active = true; wi.placed = true; wi.session = true; wi.readOk = true; wi.value = -1;
        w.step(wi);
        w.writeFailed();
        check(!w.ours(), "a write that failed leaves it not ours");
        avh::FadeGuard r;
        r.step(wi);
        avh::FadeIn ri;
        ri.active = true; ri.session = false; ri.placed = false; ri.ctlMode = 0; ri.readOk = true; ri.value = 0;
        r.step(ri);
        r.restoreFailed();
        check(r.ours(), "a restore that failed stays ours, to be tried again");
    }
    // The feature turning off while the camera is closed restores (the glue passes session = session && active = false).
    {
        avh::FadeGuard c;
        avh::FadeIn ci;
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
        avh::FadeGuard u;
        avh::FadeIn ui;
        ui.active = true; ui.placed = true; ui.session = true; ui.ctlMode = 4; ui.readOk = true; ui.value = -1;
        u.step(ui);
        ui.unload = true; ui.value = 0;
        s = u.step(ui);
        check(s.act == Act::Restore && s.ev == FadeEvent::RestoredAtUnload && !u.ours(), "UNLOAD with the global still 0: restored to -1, whatever the camera is doing");
        avh::FadeGuard v;
        avh::FadeIn vi;
        vi.active = true; vi.placed = true; vi.session = true; vi.ctlMode = 4; vi.readOk = true; vi.value = -1;
        v.step(vi);
        vi.unload = true; vi.value = 9;
        s = v.step(vi);
        check(s.act == Act::None && s.ev == FadeEvent::ChangedUnderUs, "UNLOAD with the value changed under us: not touched");
        avh::FadeGuard x;
        vi.value = 0;
        s = x.step(vi);
        check(s.act == Act::None && s.ev == FadeEvent::None, "UNLOAD when it was never ours: nothing");
    }
}

void testRing() {
    std::printf("the hook threads' ring\n");
    struct Item {
        uint64_t v = 0;
    };
    avh::Ring<Item, 4> ring;
    Item e;
    check(!ring.take(&e), "empty: nothing to take");
    for (uint64_t i = 1; i <= 6; ++i) {
        Item p;
        p.v = i;
        ring.push(p);
    }
    check(ring.lost() == 2, "six pushes into four slots: two lost, counted");
    uint64_t sum = 0;
    int n = 0;
    while (ring.take(&e)) { sum += e.v; ++n; }
    check(n == 4 && sum == 1 + 2 + 3 + 4, "the four oldest come out in order");
    Item p;
    p.v = 9;
    ring.push(p);
    check(ring.take(&e) && e.v == 9, "the ring reuses its slots");
}

void testRelayBytes() {
    std::printf("the callback relay's machine code\n");
    uint8_t f[avh::kCallbackRelayBytes] = {};
    int gate = 0, callback = 0;
    avh::buildCallbackRelay(f, &gate, &callback);
    uintptr_t a = 0;
    std::memcpy(&a, f + avh::kCallbackRelayGateAt, 8);
    check(a == reinterpret_cast<uintptr_t>(&gate), "the callback relay's gate literal is the address passed in");
    std::memcpy(&a, f + avh::kCallbackRelayCallbackAt, 8);
    check(a == reinterpret_cast<uintptr_t>(&callback) && 16 + 14 == 30 && f[15] == 0x0E, "...its callback literal too, and `je original` lands on the trampoline jump (offset 30)");
    check(f[30] == 0xFF && f[31] == 0x25 && avh::kCallbackRelayTrampolineAt == 36, "...which is `jmp [rip+0]` with the trampoline literal at 36");
}

// ================================ Phase 2: hiding the head parts ================================
void testHeadHidePure() {
    std::printf("head hiding: the hide list, the local-AMC test, the capture filter, the census text\n");
    static_assert(avh::kPartCount == 54 && avh::kPartStride == 0x680 && avh::kOffAmcInstances == 0x11C0 && avh::kOffAmcMasks == 0x17A0 && avh::kOffAmcPartFlags == 0x1814 &&
                      avh::kOffAmcPartState == 0x1820 && avh::kOffAmcAvatarPoseId == 0x260 && avh::kOffAmcAvatarPose == 0x268 && avh::kOffAmcParams == 0x50 && avh::kOffParamsMode == 0x14 &&
                      avh::kAvatarPoseToSkeleton == 0x30 && avh::kPartNameTableRva == 0x5E9F820,
                  "the AMC layout of NOTES_p2");
    // The masks: AMC + 0x17A0 + idx*0x680 + 8k, which is entry+0x5F0+8k with the entry at AMC+0x11B0.
    check(avh::kOffAmcMasks == 0x11B0 + 0x5F0 && avh::kOffAmcInstances == 0x11B0 + 0x10, "the instance and mask arrays are entry+0x10 and entry+0x5F0 of the part array at AMC+0x11B0");
    const uint32_t expect[12] = {0, 2, 3, 4, 5, 12, 13, 15, 20, 25, 26, 32};
    bool same = avh::kHeadPartCount == 12;
    for (uint32_t i = 0; i < 12; ++i) same = same && avh::kHeadParts[i] == expect[i];
    check(same, "the hide list is exactly parts 0 2 3 4 5 12 13 15 20 25 26 32");
    check(std::strcmp(avh::kPartNames[0], "Head") == 0 && std::strcmp(avh::kPartNames[2], "Eyes") == 0 && std::strcmp(avh::kPartNames[3], "Helmet") == 0 &&
              std::strcmp(avh::kPartNames[4], "SkullCap") == 0 && std::strcmp(avh::kPartNames[5], "Hair") == 0 && std::strcmp(avh::kPartNames[12], "Beard") == 0 &&
              std::strcmp(avh::kPartNames[13], "Teeth") == 0 && std::strcmp(avh::kPartNames[15], "Hat") == 0 && std::strcmp(avh::kPartNames[20], "EyeWear") == 0 &&
              std::strcmp(avh::kPartNames[25], "EVASuit_Helmet") == 0 && std::strcmp(avh::kPartNames[26], "EVASuit_Eyewear") == 0 &&
              std::strcmp(avh::kPartNames[32], "EVASuit_Gear_Head") == 0 && std::strcmp(avh::kPartNames[49], "FirstPersonSkeleton") == 0 && std::strcmp(avh::kPartNames[53], "EVASuit_Gear_Legs") == 0,
          "...and their names are Head Eyes Helmet SkullCap Hair Beard Teeth Hat EyeWear EVASuit_Helmet EVASuit_Eyewear EVASuit_Gear_Head (and 49 is FirstPersonSkeleton, 53 EVASuit_Gear_Legs)");
    bool onlyHead = true;
    for (uint32_t i = 0; i < avh::kPartCount; ++i) {
        bool inList = false;
        for (uint32_t h : expect) inList = inList || h == i;
        onlyHead = onlyHead && avh::isHeadPart(i) == inList;
    }
    check(onlyHead && !avh::isHeadPart(1) && !avh::isHeadPart(24) && !avh::isHeadPart(33) && !avh::isHeadPart(49), "isHeadPart is true for those twelve and for no other of the 54 (not Body, EVASuit, Gear_Neck, FirstPersonSkeleton)");

    // The local AMC test.
    const uint64_t skel = 0x1000000;
    check(avh::isLocalAmc(0xFFFFFFFFu, skel + 0x30, skel, 3), "LOCAL AMC: handle resolved, +0x268 minus 0x30 is the site-1 skeleton, mode 3");
    check(!avh::isLocalAmc(0x5F2867Cu, skel + 0x30, skel, 3), "...not with the handle unresolved (the id dword is not 0xFFFFFFFF)");
    check(!avh::isLocalAmc(0xFFFFFFFFu, skel + 0x38, skel, 3) && !avh::isLocalAmc(0xFFFFFFFFu, skel + 0x28, skel, 3) && !avh::isLocalAmc(0xFFFFFFFFu, skel, skel, 3),
          "...not when +0x268 minus 0x30 is anything but the skeleton (off by 8 either way, or not offset at all)");
    check(!avh::isLocalAmc(0xFFFFFFFFu, skel + 0x30, skel, 1) && !avh::isLocalAmc(0xFFFFFFFFu, skel + 0x30, skel, -1) && !avh::isLocalAmc(0xFFFFFFFFu, skel + 0x30, skel, 4),
          "...not in any mode but 3 (first person is 1, unreadable params -1)");
    check(!avh::isLocalAmc(0xFFFFFFFFu, 0x30, 0, 3) && !avh::isLocalAmc(0xFFFFFFFFu, 0, 0, 3) && !avh::isLocalAmc(0xFFFFFFFFu, 0x10, skel, 3),
          "...and never against a skeleton of 0 (nothing captured), nor a pose pointer below 0x30");

    // The capture filter.
    const uintptr_t s1 = 0x140000000ull + avh::kFindSite1Rva, s2 = 0x140000000ull + avh::kFindSite2Rva, lit = 0x140000000ull + avh::kPovNameRva;
    check(avh::findCaptureSite(s1, lit, s1, s2, lit) == 0 && avh::findCaptureSite(s2, lit, s1, s2, lit) == 1, "CAPTURE FILTER: site 1 is 0, site 2 is 1");
    check(avh::findCaptureSite(s1 + 1, lit, s1, s2, lit) == -1 && avh::findCaptureSite(0x1234, lit, s1, s2, lit) == -1, "...any other caller is -1");
    check(avh::findCaptureSite(s1, lit + 1, s1, s2, lit) == -1 && avh::findCaptureSite(s2, 0, s1, s2, lit) == -1, "...any other name pointer is -1, however right the caller");
    check(avh::findCaptureSite(s1, lit, s1, s2, 0) == -1 && avh::findCaptureSite(0, 0, 0, 0, 0) == -1, "...and nothing matches while the literal's address is unset (0)");

    // The census text.
    avh::AmcCensus c;
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
        avh::PartCensus& p = c.part[c.count++];
        p.idx = i;
        p.variants = i == 2 ? 0x0F : 0x01;
        p.maskBits = i == 3 ? 0 : p.variants;
        p.flags = i == 24 ? 0x14 : 0x05;
        p.state = 1;
        p.pending = -1;
        p.current = 100 + i;
    }
    Capture cap;
    avh::formatAmcCensus(c, avh::Sink{&Capture::add, &cap});
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
    avh::AmcCensus big;
    big.amc = 0x1000;
    for (uint32_t i = 0; i < avh::kPartCount; ++i) {
        avh::PartCensus& p = big.part[big.count++];
        p.idx = static_cast<uint8_t>(i);
        p.variants = 0x0F;
        p.maskBits = 0x0F;
        p.flags = 0x05;
        p.state = 1;
        p.pending = -1;
        p.current = 12345;
    }
    Capture cap2;
    avh::formatAmcCensus(big, avh::Sink{&Capture::add, &cap2});
    size_t longest = 0;
    int partsLines = 0;
    for (const std::string& l : cap2.lines) {
        longest = std::max(longest, l.size());
        partsLines += has(l, "census parts:") ? 1 : 0;
    }
    check(cap2.lines.size() == 1 + static_cast<size_t>(partsLines) && partsLines >= 2 && longest < avh::kLineBytes && cap2.count("[53 EVASuit_Gear_Legs ") == 1 && cap2.count("[0 Head ") == 1,
          "...all 54 parts at four variants split over several 'census parts' lines, none past the line length, every part exactly once");
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

// The controller's update: records the three pressed ints (+0x400, +0x404, +0x408), the mode (+0x40C) and the call count (+0x410).
Bytes buildControllerSynthetic(bool corrupt) {
    Bytes b;
    prologue(b, avh::kControllerPrologue, sizeof(avh::kControllerPrologue));
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

// The controller's synthetic update, with the real prologue and with one byte of it wrong.
struct CtlPages {
    Bytes good, bad;
    uint8_t* g = nullptr;
    uint8_t* b = nullptr;
    bool ok() const { return g && b; }
};
CtlPages makeCtlPages() {
    CtlPages p;
    p.good = buildControllerSynthetic(false);
    p.bad = buildControllerSynthetic(true);
    p.g = makeExecutable(p.good);
    p.b = makeExecutable(p.bad);
    return p;
}

// The camera controller the way the rig plays it: its three action objects wired (the synthetic update reads their pressed ints), its mode byte.
struct CtlGame {
    alignas(16) uint8_t ctl[0x600] = {};
    alignas(16) uint8_t photoAction[0x40] = {}, freeAction[0x40] = {}, quitAction[0x40] = {};
    FnObj ctlUpdate = nullptr;
    template <typename T> static T rd(const uint8_t* base, uint32_t off) { T v; std::memcpy(&v, base + off, sizeof(T)); return v; }
    int32_t seenPhoto() const { return rd<int32_t>(ctl, 0x400); }
    int32_t seenFree() const { return rd<int32_t>(ctl, 0x404); }
    uint32_t ctlCalls() const { return rd<uint32_t>(ctl, 0x410); }
    uint8_t mode() const { return ctl[0x3E0]; }
    void init() {
        std::memset(ctl, 0, sizeof(ctl));
        std::memset(photoAction, 0, sizeof(photoAction));
        std::memset(freeAction, 0, sizeof(freeAction));
        std::memset(quitAction, 0, sizeof(quitAction));
        const uint64_t photo = reinterpret_cast<uint64_t>(photoAction), fr = reinterpret_cast<uint64_t>(freeAction), quit = reinterpret_cast<uint64_t>(quitAction);
        std::memcpy(ctl + 0x310, &photo, 8);
        std::memcpy(ctl + 0x328, &fr, 8);
        std::memcpy(ctl + 0x340, &quit, 8);
    }
    // The suite's mode for the frame: in the game the update writes it; here the rig sets it before the call, and the hook reads it after.
    void setMode(uint8_t m) { ctl[0x3E0] = m; }
    void ctlFrame() { ctlUpdate(ctl); }
};

struct Rig {
    Capture cap;
    void boundary() { nofoothookstest::boundary(&Capture::add, &cap); }
};


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
// FUN 0x19B3F40 for ONE humanoid, in machine code at its real address: block 1 (third-person avatar) calls r8 (FindJoint) at +0x19B4092, returning to +0x19B3FD5
// (site 1); block 2 (first-person avatar) calls it at +0x19B4116, returning to +0x19B4059 (site 2). One frame, no push between. rcx = the third-person interface
// (0 skips block 1), rdx = the name, r8 = FindJoint, r9 = the first-person interface (0 skips block 2: an NPC has none).
// g_cimgBetween, when set, is called between the two attaches of a local invocation (the rig's way to run another thread's attach INSIDE the invocation).
void (*g_cimgBetween)() = nullptr;
void cimgInvoke() {
    cimgCommit(0x19B3F40, 0x200);
    uint8_t* const base = g_cimg + 0x19B3F40;
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
    put({0x41, 0xFF, 0xD0});                           // call r8        (returns to +0x19B3FD5)
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
    put({0x41, 0xFF, 0xD0});                           // call r8        (returns to +0x19B4059)
    put({0x48, 0x83, 0xC4, 0x48, 0xC3});               // end: add rsp, 48h; ret
}
// ---- the synthetic skeleton interfaces (Phase 3: the head-joint eye) -------------------------------------------------------------------------------
// RR's and AO's vtables with the build's slots (+0x18 pose, +0x30 FindJoint, +0x48 world, +0x58 model) pointing at trampolines placed at the EXACT RVAs the build has,
// to native stubs; FindJoint (+0xFDEBB0) is the real hook target. A stub records how it was called; a fault can be injected in each.
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
    if (std::strcmp(name, avh::kHeadName) == 0) {
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
    if (g_cr.swapModelSlot) *reinterpret_cast<uint64_t*>(g_cimg + avh::kRrVtableRva + 8 * 9) = reinterpret_cast<uint64_t>(g_cimg) + 0x3000000;
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
    cimgTramp(0x43FE5E0, reinterpret_cast<const void*>(&crPose));    // RR +0x18
    cimgTramp(0x43F9E60, reinterpret_cast<const void*>(&crWorld));   // RR +0x48
    cimgTramp(0x43F9B90, reinterpret_cast<const void*>(&crModel));   // RR +0x58
    cimgTramp(0xFDF3B0, reinterpret_cast<const void*>(&crPose));     // AO +0x18
    cimgTramp(0xFDEF50, reinterpret_cast<const void*>(&crWorld));    // AO +0x48
    cimgTramp(0xFDEDB0, reinterpret_cast<const void*>(&crModel));    // AO +0x58
    cimgTramp(0x3000100, reinterpret_cast<const void*>(&crWorld));   // a function that is none of the slots'
    const uint64_t b = reinterpret_cast<uint64_t>(g_cimg);
    for (uint32_t i = 0; i < avh::kHeadSlots; ++i) {
        *cimgQ(avh::kRrVtableRva + 8 * avh::kHeadSlotIndex[i]) = b + avh::kRrFunctions[i];
        *cimgQ(avh::kAoVtableRva + 8 * avh::kHeadSlotIndex[i]) = b + avh::kAoFunctions[i];
    }
    cimgCommit(avh::kHeadNameRva, 32);
    std::memcpy(g_cimg + avh::kHeadNameRva, avh::kHeadName, sizeof(avh::kHeadName));
}
void cimgInit(int corruptName = -1) {
    if (!g_cimg) g_cimg = static_cast<uint8_t*>(VirtualAlloc(nullptr, kCImgSize, MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    // The dither fade: the real prologue, a rip-relative displacement of 0, then the matching epilogue. It does nothing: the rig plays the game's masks.
    cimgCommit(avh::kAvatarFadeRva, 64);
    {
        uint8_t* p = g_cimg + avh::kAvatarFadeRva;
        size_t n = 0;
        for (uint8_t b : avh::kAvatarFadePrologue) p[n++] = b;
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
    cimgCommit(avh::kFindJointRva, 64);
    {
        uint8_t* p = g_cimg + avh::kFindJointRva;
        size_t n = 0;
        for (uint8_t b : avh::kFindJointPrologue) p[n++] = b;
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
    cimgCommit(avh::kPovNameRva, 32);
    std::memcpy(g_cimg + avh::kPovNameRva, "def_c_povCamera_joint", 22);
    // The part-name table: 54 absolute pointers at +0x5E9F820, the strings after them.
    cimgCommit(avh::kPartNameTableRva, avh::kPartCount * 8);
    cimgCommit(0x5E9D000, avh::kPartCount * 0x40);
    for (uint32_t i = 0; i < avh::kPartCount; ++i) {
        char* s = reinterpret_cast<char*>(g_cimg + 0x5E9D000 + i * 0x40);
        std::snprintf(s, 0x40, "%s", static_cast<int>(i) == corruptName ? "Xead" : avh::kPartNames[i]);
        const uint64_t abs = reinterpret_cast<uint64_t>(s);
        std::memcpy(g_cimg + avh::kPartNameTableRva + 8 * i, &abs, 8);
    }
    cimgFollowInit();
}
using ImgFade = uint64_t (__fastcall*)(void*);
using ImgFind = uint64_t (__fastcall*)(void* iface, const void* name, void* fn);
using ImgInvoke = uint64_t (__fastcall*)(void* third, const void* name, void* fn, void* first);
// One invocation for a humanoid with BOTH avatars (the local player): third-person at site 1, then first-person at site 2, from one frame.
uint64_t imgInvoke(void* third, void* first, const void* name) { return reinterpret_cast<ImgInvoke>(g_cimg + 0x19B3F40)(third, name, g_cimg + avh::kFindJointRva, first); }
// A LONE attach: site 0 = an invocation with only the third-person avatar (an NPC's), site 1 = only the first-person one, site 2 = a call from somewhere else.
uint64_t imgAttach(int site, void* iface, const void* name) {
    if (site == 0) return imgInvoke(iface, nullptr, name);
    if (site == 1) return imgInvoke(nullptr, iface, name);
    return reinterpret_cast<ImgFind>(g_cimg + 0x1A00000)(iface, name, g_cimg + avh::kFindJointRva);
}
// ...from a deeper stack frame (the same call, but its return-address slot is somewhere else).
__declspec(noinline) uint64_t imgDeep(int depth, int site, void* iface, const void* name) {
    volatile uint8_t pad[128];
    pad[0] = static_cast<uint8_t>(depth);
    pad[1] = 0;
    return (depth > 0 ? imgDeep(depth - 1, site, iface, name) : imgAttach(site, iface, name)) + pad[1];
}
void imgFade(void* amc) { reinterpret_cast<ImgFade>(g_cimg + avh::kAvatarFadeRva)(amc); }

// An AMC the way the rig plays it: the handle slot, the creation params, parts with instances, every view mask non-zero (the game's visibility job just ran).
struct AmcBuf {
    alignas(16) uint8_t b[0x1E000];
    alignas(16) uint8_t params[0x40];
    void init(uint64_t skeleton, int32_t mode, uint32_t id = 0xFFFFFFFFu, uint64_t poseOffset = 0x30) {
        std::memset(b, 0, sizeof(b));
        std::memset(params, 0, sizeof(params));
        const uint64_t pose = skeleton + poseOffset, pp = reinterpret_cast<uint64_t>(params);
        std::memcpy(b + avh::kOffAmcAvatarPoseId, &id, 4);
        std::memcpy(b + avh::kOffAmcAvatarPose, &pose, 8);
        std::memcpy(b + avh::kOffAmcParams, &pp, 8);
        std::memcpy(params + avh::kOffParamsMode, &mode, 4);
        const int32_t viewFilter = 1, context = 7;
        std::memcpy(b + avh::kOffAmcViewFilter, &viewFilter, 4);
        std::memcpy(b + avh::kOffAmcContext, &context, 4);
        const float alpha = 1.0f;
        std::memcpy(b + avh::kOffAmcAlpha, &alpha, 4);
        static const uint32_t withInstance[] = {0, 1, 2, 3, 5, 12, 24, 25, 32, 49};
        for (uint32_t idx : withInstance) {
            const uint32_t at = idx * avh::kPartStride;
            for (uint32_t k = 0; k < (idx == 2 ? 4u : 1u); ++k) {
                const uint64_t inst = 0x2000 + idx * 16 + k;
                std::memcpy(b + avh::kOffAmcInstances + at + 8 * k, &inst, 8);
            }
            b[avh::kOffAmcPartFlags + at] = idx == 24 ? 0x14 : 0x05;
            const int32_t state = 1, pending = -1, current = 100 + static_cast<int32_t>(idx);
            std::memcpy(b + avh::kOffAmcPartState + at, &state, 4);
            std::memcpy(b + avh::kOffAmcPartPending + at, &pending, 4);
            std::memcpy(b + avh::kOffAmcPartCurrent + at, &current, 4);
        }
        refillMasks();
    }
    void setTransform(const void* obj) {
        const uint64_t p = reinterpret_cast<uint64_t>(obj);
        std::memcpy(b + avh::kOffAmcTransform, &p, 8);
    }
    // What the game's visibility job does every frame: every mask non-zero.
    void refillMasks() {
        for (uint32_t idx = 0; idx < avh::kPartCount; ++idx)
            for (uint32_t k = 0; k < avh::kPartVariants; ++k) {
                const uint64_t m = 0x8000000000000001ull + idx;
                std::memcpy(b + avh::kOffAmcMasks + idx * avh::kPartStride + 8 * k, &m, 8);
            }
    }
    uint64_t mask(uint32_t idx, uint32_t k) const {
        uint64_t m = 0;
        std::memcpy(&m, b + avh::kOffAmcMasks + idx * avh::kPartStride + 8 * k, 8);
        return m;
    }
    // Head masks zero / everything else untouched?
    bool onlyHeadZeroed() const {
        for (uint32_t idx = 0; idx < avh::kPartCount; ++idx)
            for (uint32_t k = 0; k < avh::kPartVariants; ++k) {
                const bool zero = mask(idx, k) == 0;
                if (zero != avh::isHeadPart(idx)) return false;
            }
        return true;
    }
    bool allMasksIntact() const {
        for (uint32_t idx = 0; idx < avh::kPartCount; ++idx)
            for (uint32_t k = 0; k < avh::kPartVariants; ++k)
                if (mask(idx, k) != 0x8000000000000001ull + idx) return false;
        return true;
    }
};

// The three targets: the controller's page (good or with a wrong prologue), and the image's fade and FindJoint (or, wrong, a function of 0xCC
// bytes); the image is the skeleton reads' build too.
void installTargets(const CtlPages& p, bool badCtl = false, bool badFade = false, bool badFind = false) {
    NativeOnFootHooksTestTargets tt;
    tt.controller = reinterpret_cast<uintptr_t>(badCtl ? p.b : p.g);
    tt.avatarFade = reinterpret_cast<uintptr_t>(g_cimg) + (badFade ? 0x3000000 : avh::kAvatarFadeRva);
    tt.findJoint = reinterpret_cast<uintptr_t>(g_cimg) + (badFind ? 0x3000000 : avh::kFindJointRva);
    nofoothookstest::setTargets(tt);
    nofoothookstest::setHeadImage(reinterpret_cast<uintptr_t>(g_cimg), kCImgSize);
}

void testInstallAndOrder(const CtlPages& p) {
    std::printf("glue: the hooks are installed when native on foot wants them, in order, and the want acts at the next boundary\n");
    namespace t = edvr::nofoothookstest;
    Rig rig;
    t::reset();
    edvr::g_nativeOnFootActive.store(false);
    cimgInit();
    installTargets(p);
    uint8_t ctlBefore[16];
    std::memcpy(ctlBefore, p.g, 16);
    rig.boundary();
    rig.boundary();
    check(t::hookState(t::kController) == 0 && t::hookState(t::kFade) == 0 && t::hookState(t::kFind) == 0 && std::memcmp(ctlBefore, p.g, 16) == 0 &&
              g_cimg[avh::kAvatarFadeRva] == avh::kAvatarFadePrologue[0] && g_cimg[avh::kFindJointRva] == avh::kFindJointPrologue[0] && rig.cap.lines.empty(),
          "THE NATIVE KEY OFF: nothing is installed, nothing is patched, nothing is said");
    check(!t::gateOpen(t::kController) && !t::gateOpen(t::kFade) && !t::gateOpen(t::kFind) && !t::skeletonArmed(), "...every gate is closed and the skeleton reads are not armed");
    // native_on_foot.cpp publishes the want AFTER the hooks' boundary in the same tick (device_hook.cpp, tkNativeOnFoot), so it acts at the next one.
    nativeOnFootHooksBody(true, false, "rig: the key is on");
    check(t::hookState(t::kFind) == 0 && t::hookState(t::kFade) == 0 && t::hookState(t::kController) == 0,
          "THE WANT IS PUBLISHED: nothing is installed until the next boundary (the one-frame order the tick keeps)");
    rig.boundary();
    check(t::hookState(t::kFind) == 1 && t::hookState(t::kFade) == 1 && t::hookState(t::kController) == 1 && t::stolenBytes(t::kFind) == 5 &&
              t::stolenBytes(t::kFade) == 5 && t::stolenBytes(t::kController) == 5 && p.g[0] == 0xE9 && g_cimg[avh::kAvatarFadeRva] == 0xE9 && g_cimg[avh::kFindJointRva] == 0xE9,
          "...the next boundary installs all three: FindJoint, the avatar fade and the controller each stole 5 bytes");
    const int iFind = rig.cap.indexOf("find-joint hook armed:"), iFade = rig.cap.indexOf("avatar-fade hook armed:"), iCtl = rig.cap.indexOf("controller hook armed:"),
              iNames = rig.cap.indexOf("the part-name table at"), iArm = rig.cap.indexOf("skeleton reads armed (build 334925)");
    check(iFind >= 0 && iFind < iFade && iFade < iCtl && iCtl < iNames && iNames < iArm,
          "THE ORDER: FindJoint first (the capture before the first attach), then the fade, then the controller, then the name check, then the skeleton reads' arming");
    check(rig.cap.count("hook armed:") == 3 && rig.cap.nth("find-joint hook armed:", 0).compare(0, std::strlen(avh::prefix()), avh::prefix()) == 0,
          "...each armed once, the line prefixed");
    check(t::partNamesState() == 1 && t::skeletonArmed() && t::headHideOn(), "...the names match, the skeleton reads are armed, head hiding may write");
    check(t::gateOpen(t::kFind) && t::gateOpen(t::kFade) && !t::gateOpen(t::kController),
          "THE GATES: the capture and the fade hook are open from launch while the key is on; the controller's only on foot");
    edvr::g_nativeOnFootActive.store(true);
    rig.boundary();
    check(t::gateOpen(t::kController), "ON FOOT: the controller gate opens");
    rig.cap.clear();
    rig.boundary();
    rig.boundary();
    check(rig.cap.count("hook armed:") == 0 && rig.cap.count("the part-name table at") == 0 && rig.cap.count("skeleton reads armed") == 0, "...nothing is installed or said twice");
    edvr::g_nativeOnFootActive.store(false);
    nativeOnFootHooksBody(false, false, "rig: the key is off");
    rig.boundary();
    check(!t::gateOpen(t::kFind) && !t::gateOpen(t::kFade) && !t::gateOpen(t::kController) && !t::headHideOn(),
          "THE KEY OFF AGAIN: every gate closes at the next boundary (the hooks stay installed, their callbacks are not reached)");
    t::reset();
}

void testStandDowns(const CtlPages& p) {
    std::printf("glue: each prologue wrong in turn\n");
    namespace t = edvr::nofoothookstest;
    struct Case {
        const char* name;
        int hook;
        const char* line;
        bool badCtl, badFade, badFind;
        bool hideOn;   // head hiding still runs
    };
    const Case cases[] = {
        {"controller", t::kController, "controller hook stood down:", true, false, false, true},
        {"avatar-fade", t::kFade, "avatar-fade hook stood down:", false, true, false, false},
        {"find-joint", t::kFind, "find-joint hook stood down:", false, false, true, false},
    };
    for (const Case& c : cases) {
        char what[200];
        Rig rig;
        t::reset();
        edvr::g_nativeOnFootActive.store(false);
        cimgInit();
        installTargets(p, c.badCtl, c.badFade, c.badFind);
        uint8_t badBefore[16];
        std::memcpy(badBefore, c.badCtl ? p.b : g_cimg + 0x3000000, 16);
        nativeOnFootHooksBody(true, false, "");
        rig.boundary();
        std::snprintf(what, sizeof(what), "WRONG %s PROLOGUE: one '%s' line saying the game build differs and nothing was patched", c.name, c.line);
        check(rig.cap.count(c.line) == 1 && has(rig.cap.nth(c.line, 0), "the game build differs") && has(rig.cap.nth(c.line, 0), "nothing was patched"), what);
        bool othersArmed = true;
        for (int h = 0; h < 3; ++h)
            if (h != c.hook) othersArmed = othersArmed && t::hookState(h) == 1;
        std::snprintf(what, sizeof(what), "...the wrong function was not written, the hook is stood down, the other two are armed, and head hiding %s",
                      c.hideOn ? "still runs" : "does not run");
        check(std::memcmp(badBefore, c.badCtl ? p.b : g_cimg + 0x3000000, 16) == 0 && t::hookState(c.hook) == 2 && t::stolenBytes(c.hook) == 0 && othersArmed &&
                  t::headHideOn() == c.hideOn,
              what);
        t::reset();
    }
}

void testNativeFadeGlue(const CtlPages& p) {
    std::printf("glue: the avatar dither-fade global for native stereo's body\n");
    namespace t = edvr::nofoothookstest;
    static int32_t fadeMode;
    static float fadeAmount;
    Rig rig;
    t::reset();
    edvr::g_nativeOnFootActive.store(false);
    cimgInit();
    installTargets(p);
    fadeMode = -1;
    fadeAmount = 0.0f;
    t::setFadeGlobal(&fadeMode, &fadeAmount);
    nativeOnFootHooksBody(true, false, "rig: not on foot yet");
    rig.boundary();
    rig.boundary();
    check(fadeMode == -1 && !t::fadeOurs() && rig.cap.count("native on foot body:") == 0, "OFF FOOT: the global is the game's (-1), nothing is said");
    edvr::g_nativeOnFootActive.store(true);
    nativeOnFootHooksBody(true, true, "");
    rig.boundary();
    check(fadeMode == 0 && t::fadeOurs() && rig.cap.count("native on foot body: on (fade mode -1 -> 0, head parts hidden)") == 1, "THE BODY ON: -1 -> 0, one line");
    rig.boundary();
    check(fadeMode == 0 && rig.cap.count("native on foot body: on") == 1, "...held, said once");
    // F93d: the game's third-person camera gives the global back to the game; first person takes it again.
    edvr::nativeOnFootCameraPublish(1, 0);
    rig.boundary();
    check(fadeMode == -1 && !t::fadeOurs() && rig.cap.count("native on foot body: off (fade restored -1: the game's third-person camera (F93d))") == 1,
          "THE GAME'S THIRD-PERSON CAMERA: restored to -1, the line names F93d");
    edvr::nativeOnFootCameraPublish(0, 0);
    rig.boundary();
    check(fadeMode == 0 && t::fadeOurs(), "FIRST PERSON AGAIN: 0 again");
    edvr::g_nativeOnFootActive.store(false);
    nativeOnFootHooksBody(true, false, "rig: boarded");
    rig.boundary();
    check(fadeMode == -1 && !t::fadeOurs() && rig.cap.count("native on foot body: off (fade restored -1: rig: boarded)") == 1, "THE BODY ENDS: restored to -1 with the reason native on foot gave");
    fadeMode = 7;
    edvr::g_nativeOnFootActive.store(true);
    nativeOnFootHooksBody(true, true, "");
    rig.boundary();
    rig.boundary();
    check(fadeMode == 7 && !t::fadeOurs() && rig.cap.count("native on foot body: the avatar fade mode reads 7, not -1 (auto), so it is someone else's and is left alone") == 1,
          "SOMEONE ELSE'S VALUE (7): left alone, said once");
    // A fresh session: the body takes the global, and the unload gives it back.
    edvr::g_nativeOnFootActive.store(false);
    t::reset();
    installTargets(p);
    fadeMode = -1;
    t::setFadeGlobal(&fadeMode, &fadeAmount);
    edvr::g_nativeOnFootActive.store(true);
    nativeOnFootHooksBody(true, true, "");
    rig.boundary();
    rig.boundary();
    check(fadeMode == 0 && t::fadeOurs(), "(the body on in a fresh session, the global held)");
    t::shutdown();
    check(fadeMode == -1 && !t::fadeOurs(), "UNLOAD while the body holds it: the global goes back to -1");
    edvr::g_nativeOnFootActive.store(false);
    t::reset();
    edvr::g_nativeOnFootCamExternal.store(false);
}

void testHeadHideGlue(const CtlPages& p) {
    std::printf("glue: head hiding (the avatar-fade hook's post-call, the FindJoint capture, a synthetic game image)\n");
    namespace t = edvr::nofoothookstest;
    static AmcBuf local, npc, firstPerson, wrongPose;
    static alignas(16) uint8_t skelA[0x100], skelB[0x100];   // the interfaces the "game" attaches to (only their addresses matter)
    Rig rig;
    auto begin = [&](int corruptName = -1, bool want = true, bool badFind = false) {
        t::reset();
        edvr::g_nativeOnFootActive.store(false);
        cimgInit(corruptName);
        rig = Rig();
        installTargets(p, false, false, badFind);
        nativeOnFootHooksBody(want, false, "rig: not on foot yet");
        rig.boundary();
        rig.boundary();
    };
    auto fresh = [&]() {
        local.init(reinterpret_cast<uint64_t>(skelA), 3);
        npc.init(reinterpret_cast<uint64_t>(skelB) + 0x400, 3);
        firstPerson.init(reinterpret_cast<uint64_t>(skelA), 1);
        wrongPose.init(reinterpret_cast<uint64_t>(skelA), 3, 0xFFFFFFFFu, 0x38);
    };
    auto bodyOn = [&]() {
        edvr::g_nativeOnFootActive.store(true);
        nativeOnFootHooksBody(true, true, "");
        rig.boundary();
    };

    // ---- the hooks are installed because the native key is on, and the names are checked ------------------------------------------------
    begin();
    fresh();
    check(t::stolenBytes(t::kFade) == 5 && t::stolenBytes(t::kFind) == 5 && g_cimg[avh::kAvatarFadeRva] == 0xE9 && g_cimg[avh::kFindJointRva] == 0xE9,
          "BOTH HOOKS ARE INSTALLED because the native key is on: the avatar fade and FindJoint each stole 5 bytes");
    check(rig.cap.count("avatar-fade hook armed") == 1 && rig.cap.count("find-joint hook armed") == 1, "...each said once");
    check(t::partNamesState() == 1 && rig.cap.count("avatar head hide: the part-name table at EliteDangerous64.exe+0x5E9F820 holds the 54 names") == 1 &&
              has(rig.cap.nth("the part-name table at", 0), "Head Eyes Helmet SkullCap Hair Beard Teeth Hat EyeWear EVASuit_Helmet EVASuit_Eyewear EVASuit_Gear_Head"),
          "THE NAME TABLE IS CHECKED: the exe's 54 names match, one line says what is hidden");
    check(t::headHideOn() && !t::headHideDown(), "...head hiding is on");

    // ---- the capture: through the real FindJoint hook -----------------------------------------------------------------------------------------------
    const void* pov = g_cimg + avh::kPovNameRva;
    imgInvoke(skelA, skelB, pov);
    check(avatarSkeleton(0).iface == reinterpret_cast<uint64_t>(skelA) && avatarSkeleton(0).index == 3 && avatarSkeleton(1).iface == reinterpret_cast<uint64_t>(skelB) &&
              avatarFindJointSeen() == 2,
          "THE CAPTURE: one invocation that attached both avatars latched its third-person and first-person skeletons with their indexes");

    // ---- the body not on: the census and the match line, nothing hidden ----------------------------------------------------------------------------------
    imgFade(local.b);
    check(local.allMasksIntact(), "THE BODY NOT ON: the local AMC's masks are untouched");
    rig.cap.clear();
    rig.boundary();
    check(rig.cap.count("avatar head hide: census of the local third-person avatar AMC 0x") == 1 && has(rig.cap.nth("census of the local", 0), "10 of 54 parts have instances") &&
              rig.cap.count("census parts:") == 1 && has(rig.cap.nth("census parts:", 0), "[2 Eyes v=0123 st=1 fl=0x05 item=-1/102 m=0123 HIDE]") &&
              has(rig.cap.nth("census parts:", 0), "[24 EVASuit v=0 st=1 fl=0x14 item=-1/124 m=0 EXCLUDED]") && has(rig.cap.nth("census parts:", 0), "[49 FirstPersonSkeleton"),
          "THE CENSUS, once, before the body is on: 10 parts have instances, their names, variants, state, flags and which masks are live");
    check(rig.cap.count("the local third-person avatar is AMC 0x") == 1 && has(rig.cap.nth("the local third-person avatar is AMC", 0), "The relation holds"),
          "...and ONE line says how the local AMC was found (the -0x30 relation holds)");
    imgFade(local.b);
    rig.cap.clear();
    rig.boundary();
    check(rig.cap.count("census") == 0 && rig.cap.count("the local third-person avatar is AMC") == 0, "...not repeated for the same AMC");

    // ---- only the local AMC is touched ----------------------------------------------------------------------------------------------------------------
    bodyOn();
    local.refillMasks();
    imgFade(npc.b);
    imgFade(firstPerson.b);
    imgFade(wrongPose.b);
    check(npc.allMasksIntact() && firstPerson.allMasksIntact() && wrongPose.allMasksIntact() && t::hideCalls() == 0,
          "ONLY THE LOCAL AMC: another avatar (a different skeleton), the first-person one (mode 1) and one whose +0x268 is off by 8 keep every mask");
    imgFade(local.b);
    check(local.onlyHeadZeroed(), "THE BODY ON: the local AMC's 12 head parts have all four variants' masks zero, every other part's masks are untouched");
    check(t::hideCalls() == 1 && t::hideZeroed() == 48 && t::amcLocal() == reinterpret_cast<uint64_t>(local.b), "...one hide call that zeroed 48 mask words (12 parts x 4 variants), and it knows the local AMC");
    local.refillMasks();   // the game's visibility job rewrote them
    imgFade(local.b);
    check(local.onlyHeadZeroed() && t::hideCalls() == 2 && t::hideZeroed() == 96, "...every frame: the game rewrites the masks, the post-call zeroes them again");
    imgFade(local.b);
    check(t::hideCalls() == 3 && t::hideZeroed() == 96, "...masks already zero add nothing to the zeroed count");
    // AFTER the original: the synthetic fade update writes part 0's mask during the call (flag set); the post-call must still leave it zero.
    g_cimg[avh::kAvatarFadeRva + 0x40] = 1;
    local.refillMasks();
    imgFade(local.b);
    g_cimg[avh::kAvatarFadeRva + 0x40] = 0;
    check(local.mask(0, 0) == 0 && local.onlyHeadZeroed(), "THE HIDE FOLLOWS THE ORIGINAL: a mask the fade update itself wrote during the call is zero when the hooked call returns");

    // ---- stops with the body ----------------------------------------------------------------------------------------------------------------------------
    edvr::g_nativeOnFootActive.store(false);
    nativeOnFootHooksBody(true, false, "rig: boarded");
    rig.boundary();
    local.refillMasks();
    imgFade(local.b);
    check(local.allMasksIntact() && t::hideCalls() == 4, "THE BODY ENDED: the masks are left alone from the next frame on (nothing to restore)");

    // ---- the native key off ----------------------------------------------------------------------------------------------------------------------------
    begin(-1, false);
    fresh();
    imgInvoke(skelA, skelB, g_cimg + avh::kPovNameRva);
    imgFade(local.b);
    check(!t::headHideOn() && local.allMasksIntact() && t::stolenBytes(t::kFade) == 0 && t::stolenBytes(t::kFind) == 0,
          "THE NATIVE KEY OFF: neither hook is installed, nothing is hidden, nothing is touched");

    // ---- the name table does not match -----------------------------------------------------------------------------------------------------------------
    begin(3);
    fresh();
    imgInvoke(skelA, skelB, g_cimg + avh::kPovNameRva);
    bodyOn();
    imgFade(local.b);
    check(t::partNamesState() == 2 && !t::headHideOn() && local.allMasksIntact() && t::hideCalls() == 0 && rig.cap.count("the part-name table at EliteDangerous64.exe+0x5E9F820 differs") == 1 &&
              has(rig.cap.nth("the part-name table at EliteDangerous64.exe+0x5E9F820 differs", 0), "index 3, which is not \"Helmet\"") &&
              has(rig.cap.nth("the part-name table at EliteDangerous64.exe+0x5E9F820 differs", 0), "nothing is hidden"),
          "A NAME THAT DIFFERS (index 3 reads \"Xead\"): one line names it, head hiding does not run, nothing is hidden even with the body on");

    // ---- the FindJoint hook cannot install: head hiding does not run ----------------------------------------------------------------------------------------
    begin(-1, true, true);
    fresh();
    t::setSkeleton(0, reinterpret_cast<uint64_t>(skelA), 3);   // even with a capture made by hand
    bodyOn();
    imgFade(local.b);
    check(t::stolenBytes(t::kFind) == 0 && !t::headHideOn() && local.allMasksIntact() && t::hideCalls() == 0 && rig.cap.count("find-joint hook stood down") == 1 &&
              t::hookState(t::kFade) == 1 && t::hookState(t::kController) == 1,
          "THE FINDJOINT HOOK STANDS DOWN (a wrong prologue): said once, head hiding does not run, nothing is hidden; the other two hooks are armed");

    // ---- the -0x30 relation never holds -------------------------------------------------------------------------------------------------------------------
    begin();
    fresh();
    imgInvoke(skelA, skelB, g_cimg + avh::kPovNameRva);
    bodyOn();
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
    imgFade(local.b);
    rig.cap.clear();
    rig.boundary();
    check(local.onlyHeadZeroed() && rig.cap.count("the local third-person avatar is AMC 0x") == 1, "...and when an AMC does match later, the match line is said and the hide starts");

    // ---- nothing captured at all ---------------------------------------------------------------------------------------------------------------------------
    begin();
    fresh();
    bodyOn();
    for (int i = 0; i < 100; ++i) imgFade(local.b);
    rig.cap.clear();
    rig.boundary();
    check(rig.cap.count("no AMC can be tested yet") == 0, "...100 fade calls with no capture say nothing yet");
    for (int i = 0; i < 205; ++i) imgFade(local.b);
    rig.cap.clear();
    rig.boundary();
    check(local.allMasksIntact() && rig.cap.count("no AMC can be tested yet: 305 avatar fade calls were seen but no local skeleton pair is latched") == 1,
          "NO CAPTURE AT ALL (the avatars attached before the hooks): ONE line says no AMC can be tested, nothing is hidden");

    // ---- guarded accesses: an AMC whose memory is unmapped -----------------------------------------------------------------------------------------------
    begin();
    fresh();
    imgInvoke(skelA, skelB, g_cimg + avh::kPovNameRva);
    bodyOn();
    {
        // A block shaped like an AMC with its mask pages for the head parts taken away (PAGE_NOACCESS): the header reads, the writes fault.
        uint8_t* block = static_cast<uint8_t*>(VirtualAlloc(nullptr, 0x20000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        check(block != nullptr, "(a block for an AMC with unmapped masks)");
        if (block) {
            std::memcpy(block, local.b, 0x1E000);
            const uint64_t pp = reinterpret_cast<uint64_t>(local.params);
            std::memcpy(block + avh::kOffAmcParams, &pp, 8);
            DWORD old = 0;
            VirtualProtect(block + 0xE000, 0x1000, PAGE_NOACCESS, &old);   // part 32's instances (0xE1C0) and masks (0xE7A0) both live on this page
            const uint64_t before = t::hideCalls();
            for (int i = 0; i < 6; ++i) imgFade(block);
            rig.cap.clear();
            rig.boundary();
            check(t::headHideDown() && t::hideCalls() == before && rig.cap.count("stood down for the session: 3 guarded accesses of the avatar component faulted") == 1,
                  "A FAULT IN THE AMC: counted, never a crash; the third stands head hiding down for the session with ONE line (the six calls made three faults)");
            check(!t::headHideOn(), "...head hiding is off");
            local.refillMasks();
            imgFade(local.b);
            check(local.allMasksIntact(), "...and a good AMC is no longer written to");
            VirtualFree(block, 0, MEM_RELEASE);
        }
    }
    edvr::g_nativeOnFootActive.store(false);
    t::reset();
}

void testFpBodyHideGlue(const CtlPages& p) {
    std::printf("glue: first-person cosmetic bracers follow the body hide\n");
    namespace t = edvr::nofoothookstest;
    static AmcBuf fp, local, other;
    static alignas(16) uint8_t skelA[0x100], skelB[0x100];
    Rig rig;
    t::reset();
    edvr::g_nativeOnFootActive.store(false);
    cimgInit(-1);
    installTargets(p);
    nativeOnFootHooksBody(true, false, "rig: not on foot yet");
    rig.boundary();
    rig.boundary();
    imgInvoke(skelA, skelB, g_cimg + avh::kPovNameRva);
    fp.init(reinterpret_cast<uint64_t>(skelB), 2);
    local.init(reinterpret_cast<uint64_t>(skelA), 3);
    other.init(reinterpret_cast<uint64_t>(skelB), 1);
    edvr::g_nativeOnFootActive.store(true);
    edvr::nativeOnFootCameraPublish(0, 0);
    nativeOnFootHooksBody(true, true, "");
    rig.boundary();
    nativeOnFootFpBody(true, "rig: full third-person body");
    imgFade(fp.b);
    // Spell the expected slots independently: both split bracers and the combined cosmetic asset, plus the original body hide.
    const auto fpMasksMatch = [&](bool hybrid) {
        for (uint32_t part = 0; part < avh::kPartCount; ++part)
            for (uint32_t variant = 0; variant < avh::kPartVariants; ++variant) {
                // F144: the list also covers every other arm, hand and shoulder part (EVASuit_Gear_Arms 52, UpperArms 35, UpperArmL 36, UpperArmR 37, Shoulders 34, Shoulder_Left 50, Shoulder_Right 51,
                // FlightSuit_Arms 21, FlightSuit_Hands 22, OuterClothing_Arms 23); the hybrid body still hides the skeleton (49) only
                const bool extra = part == 52 || part == 35 || part == 36 || part == 37 || part == 34 || part == 50 || part == 51 || part == 21 || part == 22 || part == 23;
                const bool hidden = part == 49 || (!hybrid && (part == 28 || part == 29 || part == 38 || part == 39 || part == 40 || extra));
                const uint64_t expected = hidden ? 0 : 0x8000000000000001ull + part;
                if (fp.mask(part, variant) != expected) return false;
            }
        return true;
    };
    check(fpMasksMatch(false), "FULL 3P: FP arms, hands, skeleton and combined/L/R bracers hidden in all four views; every unrelated mask unchanged");
    const FpBodyReadback rb = nativeOnFootFpBodyReadback();
    bool bracerReadback = rb.parts == 16 && rb.amc == reinterpret_cast<uint64_t>(fp.b);
    for (uint32_t slot = 3; slot < 6; ++slot)
        for (uint32_t variant = 0; variant < 4; ++variant)
            bracerReadback = bracerReadback && rb.name[slot] != nullptr && rb.masks[slot][variant] == 0;
    check(bracerReadback, "...the post-write readback includes all three cosmetic bracer slots, so a hide that never ran is visible");
    imgFade(local.b);
    imgFade(other.b);
    check(local.onlyHeadZeroed() && other.allMasksIntact(), "...the local 3P arm and bracer masks and every non-FP component remain unchanged");

    // The FP hide must not rewrite the instance fields that refer to any renderable.
    const uint64_t renderableInstance = 0x123456789ABCull;
    std::memcpy(fp.b + avh::kOffAmcInstances + 28 * avh::kPartStride, &renderableInstance, sizeof(renderableInstance));
    imgFade(fp.b);
    uint64_t instanceAfter = 0;
    std::memcpy(&instanceAfter, fp.b + avh::kOffAmcInstances + 28 * avh::kPartStride, sizeof(instanceAfter));
    check(instanceAfter == renderableInstance, "...the hide changes visibility masks only, preserving renderable instance fields");

    edvr::nativeOnFootCameraPublish(1, 0);
    fp.refillMasks();
    imgFade(fp.b);
    check(fp.allMasksIntact(), "EXTERNAL CAMERA: the game rewrites every FP body/bracer mask and the hide leaves them intact");
    edvr::nativeOnFootCameraPublish(0, 0);
    imgFade(fp.b);
    check(fpMasksMatch(false), "...returning to first person hides the FP bracers on the next fade call");

    nativeOnFootHybridTick(0, true, "rig: hybrid");
    fp.refillMasks();
    imgFade(fp.b);
    check(fpMasksMatch(true), "HYBRID: only FirstPersonSkeleton is hidden; FP arms, hands and their cosmetic bracers stay visible");
    nativeOnFootHybridTick(0, false, "rig: full third-person body again");
    nativeOnFootFpBody(false, "rig: native on foot ended");
    fp.refillMasks();
    imgFade(fp.b);
    check(fp.allMasksIntact(), "BODY HIDE OFF: the game's next visibility rewrite restores all FP body/bracer masks");
    edvr::g_nativeOnFootActive.store(false);
    t::reset();
}

void testGearClipProbeGlue(const CtlPages& p) {
    std::printf("glue: bounded prewrite gear clip evidence and local third-person gear unclip\n");
    namespace t = edvr::nofoothookstest;
    static AmcBuf local, fp, npc;
    static alignas(16) uint8_t skelA[0x100], skelB[0x100], armMod[32], gearMod[32];
    Rig rig;
    t::reset();
    nofoot::logwin::setDetail(false);
    edvr::g_nativeOnFootActive.store(false);
    cimgInit(-1);
    installTargets(p);
    nativeOnFootHooksBody(true, false, "rig: install");
    rig.boundary(); rig.boundary();
    imgInvoke(skelA, skelB, g_cimg + avh::kPovNameRva);
    local.init(reinterpret_cast<uint64_t>(skelA), 3);
    fp.init(reinterpret_cast<uint64_t>(skelB), 2);
    npc.init(reinterpret_cast<uint64_t>(skelB), 3);
    edvr::g_nativeOnFootActive.store(true);
    edvr::nativeOnFootCameraPublish(0, 0);
    nativeOnFootHooksBody(true, true, ""); rig.boundary();
    t::clipProbeSink(&Capture::add, &rig.cap);
    std::memset(armMod, 0, sizeof(armMod)); std::memset(gearMod, 0, sizeof(gearMod));
    const float amount = .6f;
    std::memcpy(gearMod + avh::kClipModAmount, &amount, 4);
    armMod[avh::kClipModEnable] = gearMod[avh::kClipModEnable] = 1;
    const auto modifier = [&](uint32_t part, uint64_t ptr) {
        std::memcpy(local.b + avh::kOffAmcParts + part * avh::kPartStride + avh::kOffPartClipMod, &ptr, 8);
    };
    modifier(28, reinterpret_cast<uint64_t>(armMod));
    modifier(39, reinterpret_cast<uint64_t>(gearMod));
    const int32_t gearItem = 678, gearState = 1;
    std::memcpy(local.b + avh::kOffAmcPartCurrent + 39 * avh::kPartStride, &gearItem, 4);
    std::memcpy(local.b + avh::kOffAmcPartState + 39 * avh::kPartStride, &gearState, 4);
    local.b[avh::kOffAmcPartFlags + 39 * avh::kPartStride] = 5;
    for (uint32_t k = 0; k < 4; ++k) {
        const uint64_t inst = 0x9000 + k, mask = 0xA000 + k;
        std::memcpy(local.b + avh::kOffAmcInstances + 39 * avh::kPartStride + 8*k, &inst, 8);
        std::memcpy(local.b + avh::kOffAmcMasks + 39 * avh::kPartStride + 8*k, &mask, 8);
    }
    const uint64_t rightInst = 0xB000;
    std::memcpy(local.b + avh::kOffAmcInstances + 40 * avh::kPartStride, &rightInst, 8);
    nativeOnFootBodyClipTick(1000, true, true, "rig");
    t::clipProbeClock(2000); imgFade(fp.b); imgFade(npc.b);
    nativeOnFootBodyClipTick(6000, true, true, "rig");
    check(rig.cap.count("gear clip: armed") == 1 && rig.cap.count("no new local mode-3 fade sample") == 1 && rig.cap.count("prewrite=1") == 0,
          "GEAR PROBE: FP and other-avatar fades cannot supply local 3P evidence; armed and never-ran are explicit");
    t::clipProbeClock(7000); imgFade(local.b);
    nativeOnFootBodyClipTick(7000, true, true, "rig");
    const std::string left = rig.cap.nth("gear clip: EVASuit_Gear_LowerArmL ", 0);
    char owner[128];
    std::snprintf(owner, sizeof(owner), "amc=0x%llX avatarPose=0x%llX skeleton=0x%llX",
                  reinterpret_cast<uint64_t>(local.b), reinterpret_cast<uint64_t>(skelA)+0x30, reinterpret_cast<uint64_t>(skelA));
    check(has(left, "status=available prewrite=1") && has(left, owner) && has(left, "gpuSkinOwner=unavailable") &&
          has(left, "item=678 state=1 flags=0x05") && has(left, "enable=1 dirty=0 amount=0.6000") &&
          has(left, "instances=9000,9001,9002,9003 masks=A000,A001,A002,A003"),
          "...the actual row preserves modifier bytes, all four instance/mask pairs and the validated owner, without guessing a GPU skin job");
    check(rig.cap.count("LowerArms status=no-instance") == 1 && rig.cap.count("LowerArmR status=no-modifier") == 1,
          "...missing combined gear and present gear without a clip modifier are distinct results");
    check(armMod[avh::kClipModEnable] == 0 && armMod[avh::kClipModDirty] == 1 && gearMod[avh::kClipModEnable] == 0 &&
          gearMod[avh::kClipModDirty] == 1 && local.mask(39, 3) == 0xA003 &&
          std::memcmp(gearMod + avh::kClipModAmount, &amount, 4) == 0,
          "...arm and attached gear unclip after the prewrite sample, preserving the amount and gear masks");
    t::clipProbeClock(8000); imgFade(local.b); nativeOnFootBodyClipTick(8000, true, true, "rig");
    check(rig.cap.count("prewrite=1") == 3, "...a second fade inside the five-second interval does not duplicate evidence");
    nativeOnFootBodyClipTick(8000, false, false, "rig: off");
    nativeOnFootBodyClipTick(9000, false, true, "rig: probe only reentry");
    nativeOnFootBodyClipTick(15000, false, true, "rig");
    check(rig.cap.count("no new local mode-3 fade sample") == 2 && rig.cap.count("prewrite=1") == 3,
          "...reentry cannot reuse the previous activation's snapshot as fresh evidence, even with unclip off");
    void* unreadable = VirtualAlloc(nullptr, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_NOACCESS);
    check(unreadable != nullptr, "...a protected page supplies the unreadable modifier fixture");
    modifier(40, reinterpret_cast<uint64_t>(unreadable));
    edvr::nativeOnFootCameraPublish(1, 0);
    t::clipProbeClock(16000); imgFade(local.b); nativeOnFootBodyClipTick(16000, false, true, "rig");
    check(rig.cap.count("LowerArmR status=read-fault") == 1 && rig.cap.count("prewrite=1") == 6,
          "...external-camera probe remains read-only and reports unreadable modifier memory explicitly");
    modifier(40, 1);
    for (uint64_t now : {22000ull, 28000ull}) {
        t::clipProbeClock(now); imgFade(local.b); nativeOnFootBodyClipTick(now, false, true, "rig");
    }
    check(rig.cap.count("prewrite=1") == 9 && rig.cap.count("default snapshot limit reached") == 1 &&
          rig.cap.count("LowerArmR status=modifier-unreadable") == 1,
          "...default logging stops after three snapshots across reentry and reports the limit");
    nofoot::logwin::setDetail(true);
    t::clipProbeClock(34000); imgFade(local.b); nativeOnFootBodyClipTick(34000, false, true, "rig");
    check(rig.cap.count("prewrite=1") == 12, "...detail logging continues the same bounded-cadence probe after the default limit");
    nativeOnFootBodyClipTick(35000, false, false, "rig: off");
    t::clipProbeClock(40000); imgFade(local.b); nativeOnFootBodyClipTick(40000, false, false, "rig");
    check(rig.cap.count("prewrite=1") == 12, "...probe off produces no further gear report");
    // Camera changes and item apply may restore enable=1. Exercise the actual fade hook against those bytes,
    // including the combined-gear suit variant and a replacement modifier rather than a cached pointer.
    alignas(16) uint8_t allGear[3][32] = {}, comparisonMod[32] = {}, replacementMod[32] = {};
    const auto gameApply = [&]() {
        for (auto& m : allGear) {
            std::memcpy(m + avh::kClipModAmount, &amount, 4);
            m[avh::kClipModEnable] = 1; m[avh::kClipModDirty] = 0;
        }
    };
    const auto allGearIs = [&](uint8_t enable, uint8_t dirty) {
        for (const auto& m : allGear)
            if (m[avh::kClipModEnable] != enable || m[avh::kClipModDirty] != dirty ||
                std::memcmp(m + avh::kClipModAmount, &amount, 4)) return false;
        return true;
    };
    for (uint32_t i = 0; i < 3; ++i) {
        const uint64_t ptr = reinterpret_cast<uint64_t>(allGear[i]);
        modifier(38 + i, ptr);
        std::memcpy(fp.b + avh::kOffAmcParts + (38+i)*avh::kPartStride + avh::kOffPartClipMod, &ptr, 8);
        std::memcpy(npc.b + avh::kOffAmcParts + (38+i)*avh::kPartStride + avh::kOffPartClipMod, &ptr, 8);
    }
    comparisonMod[avh::kClipModEnable] = 1;
    modifier(30, reinterpret_cast<uint64_t>(comparisonMod)); // EVASuit_Legs: comparison only
    gameApply();
    nativeOnFootBodyClipTick(41000, true, true, "rig: unclip");
    edvr::nativeOnFootCameraPublish(0, 0);
    imgFade(fp.b); imgFade(npc.b);
    check(allGearIs(1, 0), "GEAR UNCLIP: neither first-person nor another avatar's modifiers are written");
    imgFade(local.b);
    check(allGearIs(0, 1) && comparisonMod[avh::kClipModEnable] == 1 && comparisonMod[avh::kClipModDirty] == 0 &&
          local.mask(39, 3) == 0xA003,
          "...all three named local gear variants unclip, while comparison clipping and gear masks stay intact");
    gameApply();
    edvr::nativeOnFootCameraPublish(1, 0);
    imgFade(local.b);
    check(allGearIs(1, 0), "...external camera retains the game's restored gear clipping");
    edvr::nativeOnFootCameraPublish(0, 0);
    nativeOnFootHybridTick(42000, true, "rig: hybrid");
    imgFade(local.b);
    check(allGearIs(1, 0), "...hybrid torso-only unclip does not alter third-person gear, even with unclip requested");
    nativeOnFootHybridTick(43000, false, "rig: full body");
    nativeOnFootBodyClipTick(43000, false, true, "rig: probe only");
    imgFade(local.b);
    check(allGearIs(1, 0), "...probe-only mode leaves game-restored gear modifier bytes intact");
    nativeOnFootBodyClipTick(44000, true, true, "rig: reentry");
    replacementMod[avh::kClipModEnable] = 1;
    modifier(39, reinterpret_cast<uint64_t>(replacementMod));
    imgFade(local.b);
    check(replacementMod[avh::kClipModEnable] == 0 && replacementMod[avh::kClipModDirty] == 1 &&
          allGear[1][avh::kClipModEnable] == 1 && allGear[1][avh::kClipModDirty] == 0,
          "...item replacement reads the current modifier and never rewrites the retired modifier");
    gameApply();
    replacementMod[avh::kClipModEnable] = 1; replacementMod[avh::kClipModDirty] = 0;
    nativeOnFootBodyClipTick(45000, false, false, "rig: off");
    imgFade(local.b);
    check(allGearIs(1, 0) && replacementMod[avh::kClipModEnable] == 1 && replacementMod[avh::kClipModDirty] == 0,
          "...off leaves the game's next item-apply values intact");
    if (unreadable) VirtualFree(unreadable, 0, MEM_RELEASE);
    nofoot::logwin::setDetail(false);
    edvr::g_nativeOnFootActive.store(false);
    t::reset();
}

// F93d: native on foot's camera signal. The native key on: the controller hook is installed for the signal, its gate opens only on foot, its post-call reads
// the mode byte the update left and publishes it (native_on_foot.h), presses nothing and changes nothing; native stereo's head hiding zeroes the local head parts
// in the game's first-person camera only (mode 0), and leaves them to the game in its third-person camera suite (1..6).
void testNativeCameraGlue(const CtlPages& p) {
    std::printf("glue: native on foot's camera signal (F93d): the controller's mode byte after its update gates native stereo's head hiding\n");
    namespace t = edvr::nofoothookstest;
    static CtlGame g;
    static AmcBuf local;
    static alignas(16) uint8_t skelA[0x100], skelB[0x100];
    Rig rig;
    t::reset();
    edvr::g_nativeOnFootActive.store(false);
    g.init();
    cimgInit(-1);
    installTargets(p);
    g.ctlUpdate = reinterpret_cast<FnObj>(p.g);
    nativeOnFootHooksBody(true, false, "rig: not on foot yet");
    rig.boundary();
    rig.boundary();
    check(t::stolenBytes(t::kController) == 5 && p.g[0] == 0xE9 && rig.cap.count("controller hook armed:") == 1, "THE NATIVE KEY INSTALLS THE CONTROLLER HOOK: armed once");
    check(!t::gateOpen(t::kController), "...its gate stays closed off foot");
    g.setMode(1);
    g.ctlFrame();
    check(edvr::g_nativeOnFootCamReads.load() == 0 && !edvr::nativeOnFootCameraExternal(), "OFF FOOT: the suite open is not read (the relay runs the original alone)");
    g.setMode(0);

    // ---- on foot ------------------------------------------------------------------------------------------------------------------------------------------------
    edvr::g_nativeOnFootActive.store(true);
    nativeOnFootHooksBody(true, true, "");
    rig.boundary();
    check(t::gateOpen(t::kController), "ON FOOT: the controller gate opens for the camera signal");
    local.init(reinterpret_cast<uint64_t>(skelA), 3);
    imgInvoke(skelA, skelB, g_cimg + avh::kPovNameRva);
    g.ctlFrame();
    check(edvr::g_nativeOnFootCamReads.load() == 1 && edvr::g_nativeOnFootCamMode.load() == 0x100u && !edvr::nativeOnFootCameraExternal(),
          "FIRST PERSON: the controller's update read mode 0 after the call: one read, the game's first-person camera");
    check(g.seenPhoto() == 0 && g.seenFree() == 0 && g.mode() == 0 && t::controllerCalls() >= 1, "...it pressed nothing and changed nothing: the read is all it does");
    imgFade(local.b);
    check(local.onlyHeadZeroed(), "FIRST PERSON: native stereo's head hiding zeroes the local AMC's head parts");

    g.setMode(1);
    g.ctlFrame();
    check(edvr::nativeOnFootCameraExternal() && edvr::g_nativeOnFootCamFlips.load() == 1 && edvr::g_nativeOnFootCamMode.load() == 0x101u,
          "THE SUITE OPENS (mode 1 after the update): the game's third-person camera, one change");
    local.refillMasks();
    const uint64_t calls = t::hideCalls();
    imgFade(local.b);
    imgFade(local.b);
    check(local.allMasksIntact() && t::hideCalls() == calls && edvr::nativeOnFootCameraStats().headHeld == 2,
          "THIRD PERSON: the head is the game's: every mask intact over two fade calls, both counted as held back");
    g.setMode(3);
    g.ctlFrame();
    imgFade(local.b);
    check(edvr::nativeOnFootCameraExternal() && edvr::g_nativeOnFootCamFlips.load() == 1 && local.allMasksIntact(), "THE FREE CAMERA (mode 3): still third person, still the game's head");
    g.setMode(0);
    g.ctlFrame();
    check(!edvr::nativeOnFootCameraExternal() && edvr::g_nativeOnFootCamFlips.load() == 2, "THE SUITE CLOSES (mode 0): first person again, the second change");
    imgFade(local.b);
    check(local.onlyHeadZeroed(), "...and the head hiding is back on the very next fade call");
    const edvr::NativeOnFootCameraStats st = edvr::nativeOnFootCameraStats();
    check(st.controllerHook == 1 && st.controller == reinterpret_cast<uint64_t>(g.ctl) && st.switches == 0 && st.faults == 0,
          "THE STATS: the controller hook armed, the controller the rig's, no other controller, no fault");

    // Native stereo on foot ends inside the suite: nothing is read and the flag does not count.
    g.setMode(1);
    g.ctlFrame();
    const bool openOnFoot = edvr::nativeOnFootCameraExternal();
    edvr::g_nativeOnFootActive.store(false);
    nativeOnFootHooksBody(true, false, "rig: boarded");
    rig.boundary();
    const uint64_t readsOff = edvr::g_nativeOnFootCamReads.load();
    g.ctlFrame();
    check(openOnFoot && !edvr::nativeOnFootCameraExternal() && !t::gateOpen(t::kController) && edvr::g_nativeOnFootCamReads.load() == readsOff,
          "OFF FOOT AGAIN: the flag does not count, the gate closes, nothing more is read");
    t::reset();
    edvr::g_nativeOnFootActive.store(false);
}

void testSkeletonLatch(const CtlPages& p) {
    std::printf("glue: the FindJoint capture latches only the local humanoid (a site-1 then a site-2 attach in ONE invocation of 0x19B3F40)\n");
    namespace t = edvr::nofoothookstest;
    static AmcBuf localAmc, npcAmc, localAmc2;
    static alignas(16) uint8_t npcA[0x100], npcB[0x100], npcC[0x100], skelL[0x100], skelF[0x100], skelL2[0x100], skelF2[0x100], skelQ[0x100];
    Rig rig;
    auto begin = [&]() {
        t::reset();
        edvr::g_nativeOnFootActive.store(false);
        cimgInit();
        rig = Rig();
        installTargets(p);
        nativeOnFootHooksBody(true, false, "rig: not on foot yet");
        rig.boundary();
        rig.boundary();
    };
    auto bodyOn = [&]() {
        edvr::g_nativeOnFootActive.store(true);
        nativeOnFootHooksBody(true, true, "");
        rig.boundary();
    };
    begin();
    const void* pov = g_cimg + avh::kPovNameRva;
    auto third = [&]() { return avatarSkeleton(0).iface; };
    auto first = [&]() { return avatarSkeleton(1).iface; };
    const uint64_t L = reinterpret_cast<uint64_t>(skelL), F = reinterpret_cast<uint64_t>(skelF);

    // ---- an NPC (no first-person avatar) never latches -------------------------------------------------------------------------------------------------
    imgAttach(0, npcA, pov);
    imgAttach(0, npcB, pov);
    imgAttach(0, npcC, pov);
    check(third() == 0 && first() == 0 && avatarSkeleton(0).captures == 3 && avatarSkeleton(0).latches == 0 && avatarFindJointSeen() == 3,
          "AN NPC-ONLY INVOCATION (site 1 alone, three humanoids in a row) never latches: three raw attaches, nothing latched");
    begin();   // a fresh session for the counts below

    // ---- a local invocation (site 1 then site 2, one frame) latches ----------------------------------------------------------------------------------
    imgAttach(0, npcA, pov);
    imgAttach(0, npcB, pov);
    imgInvoke(skelL, skelF, pov);
    check(third() == L && first() == F && avatarSkeleton(0).index == 3 && avatarSkeleton(1).index == 3 && avatarSkeleton(0).latches == 1 && avatarSkeleton(0).captures == 3 &&
              avatarSkeleton(1).captures == 1,
          "A LOCAL INVOCATION (site 1 then site 2 in one frame) latches: third-person = skelL, first-person = skelF, with their indexes; three raw site-1 attaches, one site-2");
    rig.cap.clear();
    rig.boundary();
    check(rig.cap.count("the local avatar's skeleton pair changed: third-person 0x") == 1 && has(rig.cap.nth("skeleton pair changed", 0), "(was 0x0)") &&
              has(rig.cap.nth("skeleton pair changed", 0), "latches 1, raw attaches seen: site 1 3, site 2 1"),
          "LOG: one line when the latched third-person skeleton changes (from 0), with the latch count and the raw attach counts");
    // A lone site-2 attach right after the pair: no site-1 attach is pending, so nothing latches.
    imgAttach(1, skelQ, pov);
    check(third() == L && first() == F && avatarSkeleton(0).latches == 1, "A SITE-2 ATTACH WITH NO SITE 1 PENDING never latches (the pair cleared the pending site 1)");

    // ---- the latch survives later NPC captures ----------------------------------------------------------------------------------------------------------
    imgAttach(0, npcA, pov);
    imgAttach(0, npcB, pov);
    imgAttach(0, npcC, pov);
    rig.cap.clear();
    rig.boundary();
    check(third() == L && first() == F && avatarSkeleton(0).latches == 1 && avatarSkeleton(0).captures == 6 && rig.cap.count("skeleton pair changed") == 0,
          "THE LATCH SURVIVES LATER NPC SITE-1 CAPTURES: three more raw attaches, the latched pair unchanged, no new line");

    // ---- one interface at both sites is not a pair of avatars ---------------------------------------------------------------------------------------------
    imgInvoke(skelQ, skelQ, pov);
    check(third() == L && first() == F && avatarSkeleton(0).latches == 1, "THE SAME INTERFACE AT BOTH SITES does not latch (a pair is two different skeletons)");

    // ---- site 1 from one humanoid, site 2 from a DIFFERENT FRAME: no latch ----------------------------------------------------------------------------------
    imgAttach(0, npcA, pov);
    imgDeep(3, 1, skelQ, pov);
    check(third() == L && first() == F && avatarSkeleton(0).latches == 1 && avatarSkeleton(1).captures == 4,
          "A SITE-2 ATTACH FROM A DIFFERENT STACK FRAME than the pending site 1 does not latch (raw count still goes up)");

    // ---- the AMC match uses the latch, never a raw capture ---------------------------------------------------------------------------------------------------
    bodyOn();
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
        const uint32_t latchesBefore = avatarSkeleton(0).latches;
        std::thread npcThread([&] {
            imgAttach(0, npcA, pov);   // a site-1 attach pending on THIS thread
            stage = 1;
            while (stage < 2) std::this_thread::yield();
            imgAttach(0, npcB, pov);
            stage = 3;
        });
        while (stage < 1) std::this_thread::yield();
        imgAttach(1, skelQ, pov);      // a lone site 2 on the main thread while ANOTHER thread has a site 1 pending: nothing pairs across threads
        check(third() == L && avatarSkeleton(0).latches == latchesBefore, "TWO THREADS: a site-2 attach on one thread never pairs with a site-1 attach pending on another");
        imgInvoke(skelL2, skelF2, pov);   // the local player's invocation on the main thread, between the NPC thread's two attaches
        stage = 2;
        while (stage < 3) std::this_thread::yield();
        npcThread.join();
        check(third() == reinterpret_cast<uint64_t>(skelL2) && first() == reinterpret_cast<uint64_t>(skelF2) && avatarSkeleton(0).latches == latchesBefore + 1,
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
        const uint32_t latchesBefore = avatarSkeleton(0).latches, rawBefore = avatarSkeleton(0).captures;
        imgInvoke(skelL4, skelF4, pov);
        g_cimgBetween = nullptr;
        check(third() == reinterpret_cast<uint64_t>(skelL4) && first() == reinterpret_cast<uint64_t>(skelF4) && avatarSkeleton(0).latches == latchesBefore + 1 &&
                  avatarSkeleton(0).captures == rawBefore + 2,
              "AN NPC ATTACH ON ANOTHER THREAD INSIDE THE LOCAL INVOCATION (between its site 1 and its site 2) does not break the latch: the pending site 1 is per thread");
    }
    edvr::g_nativeOnFootActive.store(false);
    t::reset();
}

void testGlue() {
    std::printf("glue, end to end (synthetic functions with the real prologues)\n");
    namespace t = edvr::nofoothookstest;
    CtlPages p = makeCtlPages();
    check(p.ok(), "two executable pages for the synthetic controller update");
    if (!p.ok()) return;
    static CtlGame g;
    g.init();
    check(reinterpret_cast<FnObj>(p.g)(g.ctl) == 0x2233445566778899ull && g.ctlCalls() == 1, "baseline: the synthetic controller update works unhooked");
    testInstallAndOrder(p);
    testStandDowns(p);
    testNativeFadeGlue(p);
    testHeadHideGlue(p);
    testFpBodyHideGlue(p);
    testGearClipProbeGlue(p);
    testNativeCameraGlue(p);
    testSkeletonLatch(p);
    t::reset();
    check(std::memcmp(p.g, p.good.data(), 5) == 0 && std::memcmp(g_cimg + avh::kAvatarFadeRva, avh::kAvatarFadePrologue, 5) == 0 &&
              std::memcmp(g_cimg + avh::kFindJointRva, avh::kFindJointPrologue, 5) == 0,
          "RESET: uninstall put every function's original bytes back");
}

}  // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);   // a crash in a cell must not eat the lines before it
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--dry-run") == 0) {
            std::printf("native on foot hooks test: dry run, nothing to do\n");
            return 0;
        }
    }
    testIdentity();
    testFadeGuard();
    testRing();
    testRelayBytes();
    testHeadHidePure();
    testGlue();
    if (g_failures) {
        std::printf("native on foot hooks: FAIL (%d)\n", g_failures);
        return 1;
    }
    std::printf("native on foot hooks: PASS\n");
    return 0;
}
