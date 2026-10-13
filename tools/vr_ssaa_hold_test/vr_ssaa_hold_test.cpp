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
    bool kept = true;
    for (int i = 0; i < 4; ++i) kept = kept && edvr::g_block[i].load() == remembered[i];
    check(kept && edvr::g_rememberedKind.load() == 1, "flat: the remembered block is not replaced by a flat request");
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
    installCases();
    if (g_fails) {
        std::printf("vr_ssaa_hold_test: %d of %d checks FAILED\n", g_fails, g_checks);
        return 1;
    }
    std::printf("PASS: %d vr ssaa hold checks (Settings.xml mode parse, held value by profile and mode, the toast, the display config "
                "guard, the window request by profile, the install ordering)\n", g_checks);
    return 0;
}
