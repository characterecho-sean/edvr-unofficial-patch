// vr_ssaa_hold_test -- the Supersampling hold's pure decision (src\d3d11\vr_ssaa_hold_math.h): the Settings.xml mode parse, the
// held value for each (profile, mode known, mode, requested), and the toast text. Drives the very functions the DLL calls.
// Built from the repo root by build.bat's :rig_vr_ssaa_hold_test. --dry-run writes nothing; --self-test runs the checks.
#include "../../src/d3d11/vr_ssaa_hold_math.h"
#include "../../src/d3d11/vr_display_config_guard.h"
#include "../../src/d3d11/vr_window_trampolines.cpp"  // the production window request path (its hooks are not installed in this rig)

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

// The DLL state the window request path reads, stubbed for this rig: the live VR decision, the saved mode, the panel size, CodeHook.
namespace edvr {

bool g_stubLive = false;        // vrSsaaHoldVrLive: the runtime's live VR session
int g_stubSavedMode = 3;        // the Settings.xml StereoscopicMode the stub reports
int g_readSettingsCalls = 0;    // how often the saved mode was consulted

bool deviceHookPanelSettings(float*, float*, uint32_t*, uint32_t*) { return false; }
uint32_t vrSsaaGateFrame() { return 0; }
bool vrSsaaHoldVrLive() { return g_stubLive; }
bool vrSsaaHoldReadSettings(int* mode, char* raw, size_t rawLen) {
    ++g_readSettingsCalls;
    *mode = g_stubSavedMode;
    std::snprintf(raw, rawLen, "%d", g_stubSavedMode);
    return true;
}
void vrSsaaHoldAfterCopy(uintptr_t) {}
bool g_stubWindow = true;       // vrDisplayReadGameWindow: the swap chain's window is hooked
bool vrDisplayReadGameWindow(unsigned long long* style, long* clientWidth, long* clientHeight) {
    *style = 0x16CF0000ull;
    *clientWidth = 3840;
    *clientHeight = 2160;
    return g_stubWindow;
}

// CodeHook::install: Prepare runs first; the entry is then "published", and the forward must already be set at that point. origOut is
// assigned after the publish, as the real CodeHook does (code_hook.cpp).
bool g_installRefuse = false;
bool g_prepareGiven = false;
void* g_forwardAtPublish = nullptr;
uint8_t g_fakeTrampoline[16] = {};
bool CodeHook::install(void*, void*, void** origOut, const char*, Prepare prepare, void* prepareContext) {
    if (g_installRefuse) return false;
    g_prepareGiven = prepare != nullptr;
    if (prepare && !prepare(g_fakeTrampoline, prepareContext)) return false;
    g_forwardAtPublish = origOut ? *origOut : nullptr;
    if (origOut) *origOut = g_fakeTrampoline;
    return true;
}
void CodeHook::uninstall() {}

}  // namespace edvr

namespace {

int g_checks = 0;
int g_fails = 0;

void check(bool ok, const char* what) {
    ++g_checks;
    if (!ok) {
        ++g_fails;
        std::printf("FAIL: %s\n", what);
    }
}

bool same(float a, float b) { return std::fabs(a - b) < 1e-6f; }

void parseCases() {
    using edvr::ssaahold::parseStereoscopicMode;
    int m = -1;
    check(parseStereoscopicMode("<StereoscopicMode>3</StereoscopicMode>", &m) && m == 3, "mode 3 parses");
    check(parseStereoscopicMode("<A/>\r\n  <StereoscopicMode>\t5 \r\n</StereoscopicMode>", &m) && m == 5, "mode 5 with whitespace parses");
    check(parseStereoscopicMode("<StereoscopicMode>0</StereoscopicMode>", &m) && m == 0, "mode 0 parses (off)");
    m = -7;
    check(!parseStereoscopicMode("<StereoscopicMode>0<", &m) && m == -7, "a lone '<' after the number is refused (no closing tag)");
    check(!parseStereoscopicMode("<StereoscopicMode>0</Stereo", &m) && m == -7, "a partial closing tag is refused");
    check(parseStereoscopicMode("<StereoscopicMode>6</StereoscopicMode>", &m) && m == 6, "mode 6 (the top of the game's clamp) parses");
    m = -7;
    check(!parseStereoscopicMode("<StereoscopicMode>7</StereoscopicMode>", &m) && m == -7, "mode 7 is refused and *mode is untouched");
    check(!parseStereoscopicMode("<StereoscopicMode>12</StereoscopicMode>", &m) && m == -7, "two digits past 6 are refused");
    check(!parseStereoscopicMode("<StereoscopicMode>x</StereoscopicMode>", &m), "a non-number is refused");
    check(!parseStereoscopicMode("<StereoscopicMode></StereoscopicMode>", &m), "an empty tag is refused");
    check(!parseStereoscopicMode("<StereoscopicMode>3", &m), "a tag with no closing '<' after the number is refused");
    check(!parseStereoscopicMode("<PresetName>Custom</PresetName>", &m), "a file with no StereoscopicMode tag is refused");
    check(!parseStereoscopicMode(nullptr, &m), "a null text is refused");
}

void decideCases() {
    using edvr::ssaahold::decide;
    using edvr::ssaahold::kHeldValue;
    // VR profile, mode known, 3D on: held at 1.0 whatever was asked.
    auto d = decide(true, true, 3, 0.5f);
    check(d.hold && same(d.value, kHeldValue), "VR, mode 3: held at 1.0 for a requested 0.5");
    d = decide(true, true, 5, 1.25f);
    check(d.hold && same(d.value, kHeldValue), "VR, mode 5: held at 1.0 for a requested 1.25");
    d = decide(true, true, 3, 1.0f);
    check(d.hold && same(d.value, 1.0f), "VR, mode 3: a requested 1.0 is held at 1.0 too");
    // 3D off: exactly today's behaviour, the game's own value passes.
    d = decide(true, true, 0, 0.5f);
    check(!d.hold && same(d.value, 0.5f), "VR, mode 0: not held, the requested 0.5 passes");
    // Flat profile: never held, whatever the mode.
    d = decide(false, true, 3, 0.85f);
    check(!d.hold && same(d.value, 0.85f), "flat profile, mode 3: not held, the requested 0.85 passes");
    // Mode unknown (no Settings.xml, or no tag): not held.
    d = decide(true, false, -1, 0.5f);
    check(!d.hold && same(d.value, 0.5f), "VR, mode unknown: not held, the requested 0.5 passes");
    d = decide(true, false, 3, 0.5f);
    check(!d.hold && same(d.value, 0.5f), "VR, mode unknown even with a stale 3: not held");
    // A profile change: to flat while held releases, to VR while live holds, and nothing else acts.
    using edvr::ssaahold::ProfileAction;
    using edvr::ssaahold::profileChangeAction;
    check(profileChangeAction(false, true, true) == ProfileAction::Release, "held, profile to flat (not allowed), VR live: released");
    check(profileChangeAction(true, false, true) == ProfileAction::Release, "held, VR not live: released");
    check(profileChangeAction(true, true, false) == ProfileAction::Engage, "not held, profile to VR while VR live: held");
    check(profileChangeAction(false, true, false) == ProfileAction::None, "not held, flat profile: nothing to do");
    check(profileChangeAction(true, true, true) == ProfileAction::None, "held and allowed while live: nothing to do");
    check(profileChangeAction(false, false, false) == ProfileAction::None, "flat and not live, not held: nothing to do");

    // The 3D-on apply: the game sizes its targets before the profile switch commits, so the hold must arm when VR goes live under a
    // flat profile (flight 2026-10-14: the on-foot screen stayed at the flat 0.65).
    using edvr::ssaahold::arrivingAtRise;
    using edvr::ssaahold::holdAllowedFor;
    // Flat profile, VR in scope, VR just went live (an apply saying 3D on): arms, and the hold is allowed.
    const bool arrivingFlat = arrivingAtRise(/*profileAllows*/ false, /*scopeIncludesVr*/ true, /*featuresAllowed*/ true);
    check(arrivingFlat, "flat profile, VR in scope, VR live: arriving (3D turning on)");
    check(holdAllowedFor(false, arrivingFlat), "flat profile plus an arriving 3D-on: the hold is allowed");
    // Flat profile, 3D off: VR never goes live, so nothing arms and the hold stays off (the copy reads the game's value).
    check(!holdAllowedFor(false, false), "flat profile, 3D off (nothing arriving): no hold");
    // A flat-only install never holds, even if something reported a live session.
    check(!arrivingAtRise(false, false, true), "flat-only scope: never arriving");
    // Passthrough (features off) never holds.
    check(!arrivingAtRise(false, true, false), "features off (passthrough): never arriving");
    // The VR profile is as before: allowed by the profile, and a rise there is not an arrival.
    check(!arrivingAtRise(true, true, true), "VR profile: a rise is not an arrival (the profile already allows)");
    check(holdAllowedFor(true, false), "VR profile: allowed without any latch");
    // End to end through the decisions the DLL takes: arming ahead of the profile switch, then the switch commits to VR.
    check(profileChangeAction(holdAllowedFor(false, arrivingFlat), true, false) == ProfileAction::Engage,
          "arriving, VR live, field not yet held: engage");
    check(profileChangeAction(holdAllowedFor(true, false), true, true) == ProfileAction::None,
          "the switch to the VR profile after an arming: already held, nothing to do");
    // A VR-to-flat switch while the session is still tearing down is not an arrival: flat profile, latch clear, so it releases.
    check(profileChangeAction(holdAllowedFor(false, false), true, true) == ProfileAction::Release,
          "VR to flat with the session still live: released (no arrival was armed)");

    // The settings copy reads the game's own 3D mode from its block (flight 2026-10-14 11:58: o+0x30 0 -> 4 at the apply, 0.7 s before the
    // profile switch, VR live 1 throughout flat, no rising edge). The arguments: modeKnown, mode, modeRose, scopeIncludesVr,
    // featuresAllowed, profileAllows, vrLive, arriving.
    using edvr::ssaahold::blockModeKnown;
    using edvr::ssaahold::copyHolds;
    check(blockModeKnown(0) && blockModeKnown(4) && blockModeKnown(6), "block mode 0, 4 and 6 are Settings.xml values");
    check(!blockModeKnown(7) && !blockModeKnown(4294967295u), "block mode 7 and a stray dword are not read as a mode");
    // The shakedown: flat profile, the session still live, the block says 3D on (mode 4, just risen from 0): hold.
    check(copyHolds(true, 4, true, true, true, false, true, false), "flat profile, VR live, block mode 4 (rose): hold");
    check(copyHolds(true, 4, true, true, true, false, false, false), "flat profile, no session yet, block mode 4 just rose: hold");
    check(copyHolds(true, 3, false, true, true, false, true, false), "flat profile, VR live, block mode 3: hold (any nonzero mode)");
    // 3D off in the block: never a hold, whatever the profile and the session.
    check(!copyHolds(true, 0, false, true, true, false, true, false), "flat profile, VR live, block mode 0: no hold (the user's 0.65 sizes flat)");
    check(!copyHolds(true, 0, false, true, true, true, true, true), "VR profile, live, arriving, block mode 0: no hold");
    // Not a VR install, or the profile's features are off (passthrough): never.
    check(!copyHolds(true, 4, true, false, true, false, true, false), "flat-only scope: no hold even for mode 4");
    check(!copyHolds(true, 4, true, true, false, false, true, false), "features off (passthrough): no hold");
    // Mode on, flat profile, no session and no rise (a stale mode-4 file with no VR runtime): the flat launch stays unheld.
    check(!copyHolds(true, 4, false, true, true, false, false, false), "flat profile, no session, mode 4 but not risen: no hold");
    // VR profile with the mode on holds at once, live or not (the startup copy).
    check(copyHolds(true, 4, false, true, true, true, false, false), "VR profile, block mode 4, session not yet live: hold");
    check(copyHolds(true, 4, false, true, true, true, true, false), "VR profile, block mode 4, live: hold (as before)");
    // The block did not read: the earlier rule (VR profile or arriving, and live).
    check(copyHolds(false, 0, false, true, true, true, true, false), "block unreadable, VR profile, live: hold (as before)");
    check(!copyHolds(false, 0, false, true, true, true, false, false), "block unreadable, VR profile, not live: no hold (as before)");
    check(copyHolds(false, 0, false, true, true, false, true, true), "block unreadable, flat, arriving, live: hold (the fallback latch)");
    check(!copyHolds(false, 0, false, true, true, false, true, false), "block unreadable, flat, live, not arriving: no hold");
}

void toastCases() {
    char buf[160] = {};
    const int n = edvr::ssaahold::formatHoldToast(buf, sizeof(buf), 0.85f);
    check(n > 0 && std::strstr(buf, "Supersampling 0.85 is held at 1.0 in VR: use HMD Image Quality to set resolution") != nullptr,
          "the toast names the requested value and says where to set resolution");
    check(std::strlen(buf) < 120, "the toast fits its buffer");
}

// The DisplayConfig plausibility guard (vr_display_config_guard.h): the values of a real object pass, the layout that corrupted the save
// (3840 read as FullScreen, 2160 as VSync, 2 as PresentInterval) is refused.
void displayConfigCases() {
    using namespace edvr::displaycfg;
    check(plausibleObject(3840, 2160, 2, 0, 1), "a 4K borderless object (FullScreen 2, VSync 0, PresentInterval 1) is plausible");
    check(plausibleObject(1280, 768, 0, 1, 1), "a windowed 1280x768 object is plausible");
    check(!plausibleFullScreen(3840), "FullScreen 3840 (the corrupting write) is refused");
    check(!plausibleVSync(2160), "VSync 2160 (the corrupting write) is refused");
    check(!plausiblePresentInterval(2160), "PresentInterval 2160 is refused");
    check(!plausibleSide(0) && !plausibleSide(100) && !plausibleSide(20000), "a side of 0, 100 or 20000 is refused");
    check(!plausibleObject(3840, 2160, 2, 1064042496u, 1), "a float-bit VSync (1064042496) is refused: the object is not what the guard names");
}

// The game's mode request, through the production requestReplacement. The original is a capture: it records what it was handed.
uint64_t g_origKind = 99;
int g_origCalls = 0;
uint8_t g_origBlock[0x28] = {};

uint64_t captureOriginal(void*, void* request, void*, void*, void*, void*, void*, void*) {
    ++g_origCalls;
    std::memcpy(g_origBlock, request, sizeof(g_origBlock));
    g_origKind = *reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(request) + 0x20);
    return 0;
}

void resetWindowState() {
    for (auto& q : edvr::g_block) q.store(0);
    edvr::g_rememberedKind.store(0);
    edvr::g_remembered.store(false);
    edvr::g_rewrites.store(0);
    edvr::g_noneLogged.store(false);
    edvr::g_modeRequests.store(0);
    edvr::g_lastLoggedAction.store(0xFF);
    edvr::g_stubLive = false;
    edvr::g_stubSavedMode = 3;
    edvr::g_readSettingsCalls = 0;
    edvr::g_requestOrig = &captureOriginal;
    g_origCalls = 0;
    g_origKind = 99;
    std::memset(g_origBlock, 0, sizeof(g_origBlock));
}

// The remembered block, as a non-windowed request left it: four qwords at request +0x08; the kind is the low dword of the last.
void rememberBlock(const uint64_t (&block)[4]) {
    for (int i = 0; i < 4; ++i) edvr::g_block[i].store(block[i]);
    edvr::g_rememberedKind.store(static_cast<uint32_t>(block[3]));
    edvr::g_remembered.store(true);
}

void makeRequest(uint8_t (&buf)[0x28], const uint64_t (&block)[4]) {
    std::memset(buf, 0, sizeof(buf));
    std::memcpy(buf + 0x08, block, 0x20);
}

void callRequest(uint8_t (&buf)[0x28]) {
    edvr::requestReplacement(nullptr, buf, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
}

bool forwardedBlockIs(const uint64_t (&block)[4]) {
    return std::memcmp(g_origBlock + 0x08, block, 0x20) == 0;
}

void reportGroup(const char* name, int checks0, int fails0) {
    std::printf("  window request, %s: %d checks, %d failed\n", name, g_checks - checks0, g_fails - fails0);
}

// Flat: the request passes byte for byte, the saved mode is not read, and nothing is remembered or rewritten.
void windowFlatCases() {
    const int c0 = g_checks, f0 = g_fails;
    resetWindowState();
    edvr::g_runtimeProfile = edvr::RuntimeProfile::Flat;
    const uint64_t remembered[4] = {0x1111, 0x2222, 0x3333, 1};   // a non-windowed kind 1 remembered earlier
    rememberBlock(remembered);
    edvr::g_stubSavedMode = 3;                                    // Settings.xml says 3D on, the case that rewrote in flat before the gate
    const uint64_t windowed[4] = {0x4444, 0x5555, 0x6666, 0};     // kind 0
    uint8_t buf[0x28], before[0x28];
    makeRequest(buf, windowed);
    std::memcpy(before, buf, sizeof(buf));
    callRequest(buf);
    check(g_origCalls == 1 && g_origKind == 0, "flat, saved mode 3, remembered kind 1: the windowed request is forwarded as kind 0");
    check(std::memcmp(g_origBlock, before, sizeof(before)) == 0, "flat: the forwarded request block is unchanged byte for byte");
    check(edvr::g_readSettingsCalls == 0, "flat: the saved mode is not read for the request");
    check(edvr::g_rewrites.load() == 0, "flat: nothing rewritten");

    const uint64_t nonWindowed[4] = {0xAAAA, 0xBBBB, 0xCCCC, 2};  // kind 2, not windowed
    makeRequest(buf, nonWindowed);
    std::memcpy(before, buf, sizeof(buf));
    callRequest(buf);
    check(g_origKind == 2 && std::memcmp(g_origBlock, before, sizeof(before)) == 0,
          "flat: a non-windowed request passes unchanged");
    bool taken = true;
    for (int i = 0; i < 4; ++i) taken = taken && edvr::g_block[i].load() == nonWindowed[i];
    check(taken && edvr::g_rememberedKind.load() == 2,
          "flat, VR not live: a non-windowed request is remembered (the launch's own mode), while it still passes unchanged");
    reportGroup("flat profile", c0, f0);
}

// VR: the rewrite happens before the live session (on the saved mode) and while live; with 3D off or nothing remembered, nothing changes.
void windowVrCases() {
    const int c0 = g_checks, f0 = g_fails;
    const uint64_t remembered[4] = {0x1111, 0x2222, 0x3333, 1};
    const uint64_t windowed[4] = {0x4444, 0x5555, 0x6666, 0};
    uint8_t buf[0x28], before[0x28];

    // (b) VR profile before the live session exists, saved mode 3: the existing VR rewrite.
    resetWindowState();
    edvr::g_runtimeProfile = edvr::RuntimeProfile::Vr;
    rememberBlock(remembered);
    edvr::g_stubSavedMode = 3;
    makeRequest(buf, windowed);
    callRequest(buf);
    check(g_origCalls == 1 && g_origKind == 1, "VR, before the live session, saved mode 3: the windowed request is rewritten to kind 1");
    check(forwardedBlockIs(remembered), "VR, before the live session: the forwarded block is the remembered block");
    check(edvr::g_readSettingsCalls == 1 && edvr::g_rewrites.load() == 1,
          "VR, before the live session: the saved mode is consulted and one rewrite is counted");

    // VR, saved mode 0 and not live: 3D off, the request passes unchanged.
    resetWindowState();
    edvr::g_runtimeProfile = edvr::RuntimeProfile::Vr;
    rememberBlock(remembered);
    edvr::g_stubSavedMode = 0;
    makeRequest(buf, windowed);
    std::memcpy(before, buf, sizeof(buf));
    callRequest(buf);
    check(g_origKind == 0 && std::memcmp(g_origBlock, before, sizeof(before)) == 0,
          "VR, before the live session, saved mode 0: the windowed request passes unchanged");

    // (c) VR live: rewritten on the live signal alone (the saved mode is not read).
    resetWindowState();
    edvr::g_runtimeProfile = edvr::RuntimeProfile::Vr;
    rememberBlock(remembered);
    edvr::g_stubLive = true;
    edvr::g_stubSavedMode = 0;
    makeRequest(buf, windowed);
    callRequest(buf);
    check(g_origCalls == 1 && g_origKind == 1 && forwardedBlockIs(remembered),
          "VR live: the windowed request is rewritten to the remembered block");
    check(edvr::g_readSettingsCalls == 0, "VR live: the saved mode is not read");

    // VR live with nothing remembered: left as the game made it.
    resetWindowState();
    edvr::g_runtimeProfile = edvr::RuntimeProfile::Vr;
    edvr::g_stubLive = true;
    makeRequest(buf, windowed);
    std::memcpy(before, buf, sizeof(buf));
    callRequest(buf);
    check(g_origKind == 0 && std::memcmp(g_origBlock, before, sizeof(before)) == 0,
          "VR live, nothing remembered: the windowed request passes unchanged");
    reportGroup("VR profile", c0, f0);
}

// The mode request trace: the action each decision is logged as, the cap, and the two line formats. Logging changes no decision (the groups
// above pin the forwarded block); here the action tag is pinned to the branch taken.
const char* lastAction() {
    const uint8_t a = edvr::g_lastLoggedAction.load();
    return a == 0xFF ? "none" : edvr::modeActionName(static_cast<edvr::ModeAction>(a));
}
void modeRequestTraceCases() {
    const int c0 = g_checks, f0 = g_fails;
    const uint64_t remembered[4] = {0x1111, 0x2222, 0x3333, 1};
    const uint64_t windowed[4] = {0x4444, 0x5555, 0x6666, 0};
    const uint64_t nonWindowed[4] = {0xAAAA, 0xBBBB, 0xCCCC, 2};
    uint8_t buf[0x28];

    resetWindowState();
    edvr::g_runtimeProfile = edvr::RuntimeProfile::Flat;
    makeRequest(buf, windowed);
    callRequest(buf);
    check(std::strcmp(lastAction(), "passthrough_not_allowed") == 0 && edvr::g_modeRequests.load() == 1 && edvr::g_readSettingsCalls == 0,
          "trace: a flat request is logged as passthrough_not_allowed, counted, and the saved mode is still not read");

    resetWindowState();
    edvr::g_runtimeProfile = edvr::RuntimeProfile::Vr;
    edvr::g_stubSavedMode = 0;
    makeRequest(buf, nonWindowed);
    callRequest(buf);
    check(std::strcmp(lastAction(), "remembered") == 0 && edvr::g_remembered.load(), "trace: VR inactive, a non-windowed request: remembered");
    makeRequest(buf, windowed);
    callRequest(buf);
    check(std::strcmp(lastAction(), "left_as_is_vr_inactive") == 0 && g_origKind == 0, "trace: VR inactive, a windowed request: left as is");

    resetWindowState();
    edvr::g_runtimeProfile = edvr::RuntimeProfile::Vr;
    edvr::g_stubLive = true;
    makeRequest(buf, windowed);
    callRequest(buf);
    check(std::strcmp(lastAction(), "left_as_is_nothing_remembered") == 0, "trace: VR on, windowed, nothing remembered: left as is");
    makeRequest(buf, nonWindowed);
    callRequest(buf);
    check(std::strcmp(lastAction(), "left_as_is_vr_on_non_windowed") == 0, "trace: VR on, a non-windowed request: left as is");
    rememberBlock(remembered);
    makeRequest(buf, windowed);
    callRequest(buf);
    check(std::strcmp(lastAction(), "rewritten") == 0 && g_origKind == 1 && forwardedBlockIs(remembered),
          "trace: VR on, windowed, remembered: rewritten, and the forwarded block is the remembered one");

    // The cap: calls past the 32nd are counted and not logged.
    resetWindowState();
    edvr::g_runtimeProfile = edvr::RuntimeProfile::Flat;
    makeRequest(buf, windowed);
    for (int i = 0; i < 32; ++i) callRequest(buf);
    edvr::g_lastLoggedAction.store(0xFF);
    callRequest(buf);
    check(edvr::g_modeRequests.load() == 33 && std::strcmp(lastAction(), "none") == 0, "trace: the 33rd request is counted and not logged");

    // The formats.
    edvr::ModeRequestRecord r;
    r.n = 3; r.frame = 1200; r.thread = 4242; r.sinceInstallMs = 15000;
    r.haveKind = r.haveFlags = r.haveBlock = true;
    r.kind = 0; r.flagsDword = 0x0201; r.q[0] = 0x4444; r.q[1] = 0x5555; r.q[2] = 0x6666; r.q[3] = 0x0000000200000000ull;
    r.profile = "flat"; r.live = false; r.rememberedBefore = true; r.action = edvr::ModeAction::PassNotAllowed;
    char line[640];
    const int n = edvr::formatModeRequestLine(r, line, sizeof(line));
    check(n > 0 && n < 640 && std::strstr(line, "mode request #3: frame 1200, +15000 ms since hook install, thread 4242, kind 0, flags +0x24 01 +0x25 02, "
                                                "block +0x08 4444 +0x10 5555 +0x18 6666 +0x20 200000000, profile flat, vr_live 0, remembered_before 1, "
                                                "action passthrough_not_allowed") != nullptr,
          "trace: the request line carries frame, time, thread, kind, flags, the block, profile, live, remembered, action");
    r.haveKind = r.haveFlags = r.haveBlock = false;
    edvr::formatModeRequestLine(r, line, sizeof(line));
    check(std::strstr(line, "kind ?, flags +0x24 ? +0x25 ?, block +0x08 ? +0x10 ? +0x18 ? +0x20 ?") != nullptr, "trace: a field that could not be read prints ?");
    char tiny[32];
    check(edvr::formatModeRequestLine(r, tiny, sizeof(tiny)) == 0, "trace: a buffer too small reports 0");
    char ret[320];
    check(edvr::formatModeReturnLine(r, true, 0x16CF0000ull, 3840, 2160, 1201, 15003, ret, sizeof(ret)) > 0 &&
          std::strcmp(ret, "vr window: mode request #3 returned: game window style 0x16CF0000 client 3840x2160, frame 1201, +15003 ms since hook install") == 0,
          "trace: the return line carries the window's style and client size");
    check(edvr::formatModeReturnLine(r, false, 0, 0, 0, 0, 0, ret, sizeof(ret)) > 0 && std::strstr(ret, "game window not hooked yet") != nullptr,
          "trace: the return line says when the window is not hooked yet");
    reportGroup("mode request trace", c0, f0);
}

// The launch's own mode request is remembered whatever the profile, so the first live switch keeps it. The gate decides the rewrite only.
void rememberAtLaunchCases() {
    const int c0 = g_checks, f0 = g_fails;
    const uint64_t launch[4] = {0, 0x19, 0, 0x1010000000002ull};   // flight 2026-10-23: kind 2, borderless 3840x2160
    const uint64_t windowed[4] = {0x4444, 0x5555, 0x6666, 0};
    const uint64_t vrChoice[4] = {0xAAAA, 0xBBBB, 0xCCCC, 3};
    uint8_t buf[0x28], before[0x28];

    // Flat launch: kind 2 at frame 1, VR not live, the gate closed. Remembered, logged as such, forwarded byte for byte.
    resetWindowState();
    edvr::g_runtimeProfile = edvr::RuntimeProfile::Flat;
    makeRequest(buf, launch);
    std::memcpy(before, buf, sizeof(buf));
    callRequest(buf);
    check(std::strcmp(lastAction(), "remembered_gate_closed") == 0 && edvr::g_remembered.load() && edvr::g_rememberedKind.load() == 2 &&
          g_origKind == 2 && std::memcmp(g_origBlock, before, sizeof(before)) == 0 && edvr::g_readSettingsCalls == 0,
          "flat launch: the kind 2 request is remembered with the gate closed, logged remembered_gate_closed, and forwarded unchanged");

    // 3D on: the windowed request, VR live, the gate open: rewritten to the launch's block.
    edvr::g_stubLive = true;
    makeRequest(buf, windowed);
    callRequest(buf);
    check(std::strcmp(lastAction(), "rewritten") == 0 && g_origKind == 2 && forwardedBlockIs(launch),
          "3D on after a flat launch: the windowed request is rewritten to the remembered launch block");

    // A request made while VR is live is not remembered, in any profile.
    makeRequest(buf, vrChoice);
    callRequest(buf);
    check(std::strcmp(lastAction(), "left_as_is_vr_on_non_windowed") == 0 && edvr::g_rememberedKind.load() == 2 && edvr::g_block[1].load() == 0x19,
          "a kind 3 request made while VR is live is not remembered; the launch block stays");
    for (auto profile : {edvr::RuntimeProfile::Flat, edvr::RuntimeProfile::Vr, edvr::RuntimeProfile::Passthrough}) {
        resetWindowState();
        edvr::g_runtimeProfile = profile;
        edvr::g_stubLive = true;
        makeRequest(buf, vrChoice);
        callRequest(buf);
        check(!edvr::g_remembered.load(), "VR live: a non-windowed request is not remembered in this profile");
    }

    // A windowed request with nothing remembered is left as it was.
    resetWindowState();
    edvr::g_runtimeProfile = edvr::RuntimeProfile::Flat;
    edvr::g_stubLive = true;
    makeRequest(buf, windowed);
    std::memcpy(before, buf, sizeof(buf));
    callRequest(buf);
    check(std::strcmp(lastAction(), "left_as_is_nothing_remembered") == 0 && g_origKind == 0 && std::memcmp(g_origBlock, before, sizeof(before)) == 0,
          "3D on with nothing remembered: the windowed request is left as the game made it");

    // The VR profile remembers as it did: VR not live, kind 2, action remembered.
    resetWindowState();
    edvr::g_runtimeProfile = edvr::RuntimeProfile::Vr;
    edvr::g_stubSavedMode = 0;
    makeRequest(buf, launch);
    callRequest(buf);
    check(std::strcmp(lastAction(), "remembered") == 0 && edvr::g_rememberedKind.load() == 2, "VR profile, VR not live: remembered, as before");
    reportGroup("remember at launch", c0, f0);
}

// The install: the forward is published through Prepare before the entry is published, and cleared when CodeHook refuses.
void installCases() {
    const int c0 = g_checks, f0 = g_fails;
    resetWindowState();
    edvr::g_runtimeProfile = edvr::RuntimeProfile::Vr;
    std::vector<uint8_t> image(edvr::kRequestRva + 16u, 0);   // a stand-in for the game module: only the entry's bytes matter here
    std::memcpy(image.data() + edvr::kRequestRva, edvr::kRequestBytes, sizeof(edvr::kRequestBytes));
    auto install = [&](const char* name) {
        return edvr::installOne(image.data(), edvr::kRequestRva, edvr::kRequestBytes, sizeof(edvr::kRequestBytes),
                                reinterpret_cast<void*>(&edvr::requestReplacement), reinterpret_cast<void**>(&edvr::g_requestOrig),
                                &edvr::g_requestHook, name);
    };
    edvr::g_installRefuse = true;
    const bool refused = install("test: refused");
    check(!refused && edvr::g_requestOrig == nullptr, "CodeHook refusal: nothing patched and the forward is cleared");
    edvr::g_installRefuse = false;
    const bool ok = install("test: installed");
    check(ok, "installOne installs the entry through the production path");
    check(edvr::g_prepareGiven, "installOne passes CodeHook a Prepare callback");
    check(edvr::g_forwardAtPublish == static_cast<void*>(edvr::g_fakeTrampoline),
          "the forward is set before the entry is published");
    check(reinterpret_cast<void*>(edvr::g_requestOrig) == static_cast<void*>(edvr::g_fakeTrampoline),
          "the forward is the trampoline after the install");
    reportGroup("install ordering", c0, f0);
}

}  // namespace

int main(int argc, char** argv) {
    bool dryRun = false, selfTest = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--dry-run") == 0) dryRun = true;
        if (std::strcmp(argv[i], "--self-test") == 0) selfTest = true;
    }
    if (dryRun && !selfTest) {
        std::printf("vr_ssaa_hold_test: dry run, nothing written\n");
        return 0;
    }
    parseCases();
    decideCases();
    toastCases();
    displayConfigCases();
    windowFlatCases();
    windowVrCases();
    modeRequestTraceCases();
    rememberAtLaunchCases();
    installCases();
    if (g_fails) {
        std::printf("vr_ssaa_hold_test: %d of %d checks FAILED\n", g_fails, g_checks);
        return 1;
    }
    std::printf("PASS: %d vr ssaa hold checks (Settings.xml mode parse, held value by profile and mode, the toast, the display config "
                "guard, the window request by profile, the install ordering)\n", g_checks);
    return 0;
}
