// GENERATED from tools/openvr_smoke/openvr_smoke.cpp in the private edvr repo -- do not edit here.
// Edit there, then: python tools/sync_common.py --write   [body-sha256 bf7c8642fc261434]
// openvr_smoke -- checks the openvr proxy's startup path without the game.
//
// The thing under test is that the proxy does NOT load the real openvr_api.dll
// from DllMain. It used to. LoadLibrary of a module nothing else has mapped runs
// that module's DllMain under the loader lock, re-entrantly, which Windows does
// not support -- the same pattern crashed the game for a user running ReShade
// with EDHM on the d3d11 side, and that side was rebuilt to defer.
//
// Three things are asserted, in order:
//
//   1. Loading the proxy returns promptly. On a worker thread with a timeout, so
//      a hang is reported rather than hanging the test.
//
//   2. fakevr is not mapped yet. THIS is the assertion that catches the bug:
//      if the real module is already loaded when LoadLibrary returns, the proxy
//      loaded it from DllMain, which is the thing being fixed. It fails against
//      the old build and passes against this one.
//
//   3. A thunked export forwards, and its arguments arrive intact. The lazy path
//      runs a C initialiser in the middle of a call whose arguments are still
//      live in rcx/rdx/r8/r9 and xmm0-xmm3; the probe checks all of those plus
//      two stack arguments.
//
// Usage: openvr_smoke.exe <dir containing openvr_api.dll and openvr_api_orig.dll>
#include <windows.h>
#include <d3d11.h>

#include <cstdio>
#include <cstring>
#include <vector>

#include "../../src/common/frame_flag.h"
#include "../../src/common/hotkey.h"
#include "../../src/common/guard.h"
#include "../../src/openvr/resubmit_shadow.h"
#include "../../src/d3d11/elite_binds.h"

namespace {

wchar_t g_proxyPath[MAX_PATH];
HMODULE g_loaded = nullptr;

int fail(const char* what) {
    printf("  FAIL  %s\n", what);
    return 1;
}

DWORD WINAPI loadProxy(LPVOID) {
    g_loaded = LoadLibraryW(g_proxyPath);
    return 0;
}

typedef unsigned long long(*PFN_Probe)(unsigned long long, double, unsigned long long,
                                       double, unsigned long long, unsigned long long);

}  // namespace

// A real module whose DllMain faults must degrade, not kill the process.
//
// This started life as a test that the fault stays CATCHABLE, to lock in the
// unwind info the assembly shim was missing. It does not test that: Windows
// catches a faulting DllMain inside LoadLibraryW and returns failure rather than
// propagating an exception, so nothing ever reaches the handler -- measured, the
// child exits with "no exception seen" either way. The unwind info is still
// required, for faults raised in our own initialiser rather than in someone
// else's DllMain, and build.bat asserts it is present instead.
//
// What this DOES pin down is worth keeping: the export still returns, the
// process survives, and resolveProcs has filled the table with the do-nothing
// stub, so the game loses VR instead of dying at startup.
//
// A child process because the fault is arranged by an environment variable read
// in the stand-in's DllMain, and the table resolves once per process.
int faultChild(const char* dir) {
    wchar_t proxy[MAX_PATH];
    _snwprintf_s(proxy, _TRUNCATE, L"%hs\\openvr_api.dll", dir);
    HMODULE m = LoadLibraryW(proxy);
    if (!m) return 3;
    FARPROC p = GetProcAddress(m, "VR_GetInitToken");
    if (!p) return 4;

    unsigned long long v = 12345;
    __try {
        v = reinterpret_cast<unsigned long long(*)()>(p)();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;   // also a pass: it returned control to us either way
    }
    // The stub returns 0. Anything else means we forwarded into a module whose
    // DllMain faulted. Not returning at all -- the process being killed -- is
    // the failure, and shows up as an exit code that is neither 0 nor 6.
    return v == 0 ? 0 : 6;
}

// The crash sentinel's lifecycle, which is shared code with none of its own.
//
// Both of its bugs were lifecycle rather than logic, and both were invisible:
//
//   arm() set its flag whether or not the file was written. The file lives in
//   the log directory, which only Log::open() creates -- and that returns early
//   when log.enabled = 0. So with logging off nothing was ever written, no
//   launch saw a trip, and a hook that really was crashing re-armed every start:
//   the protection was absent in exactly the configuration with no log to
//   diagnose it from.
//
//   A trip was permanent. confirm() runs on validation, on commit failure, and
//   from a shutdown needing FreeLibrary that a closing game never does -- so any
//   session ending early left the file behind and every later launch refused,
//   announcing a crash that never happened.
//
// Asserted here rather than trusted, because neither failure shows up as a
// crash or a wrong pixel; they show up as a fix that quietly is not running.
// The channel the head-offset gate runs on.
//
// d3d11.dll decides which mode the player is in and openvr_api.dll acts on it,
// so the answer crosses a module boundary through a named mapping. A mistake in
// that struct is invisible at compile time in BOTH modules and shows up only as
// a viewpoint that moves in the cockpit, so the round trip is asserted here --
// where the other cross-module channel already is.
int frameFlagChecks() {
    int bad = 0;

    edvr::setExternalCameraOnFoot(false);
    if (edvr::externalCameraOnFoot()) {
        printf("  FAIL  the mode flag reads true after being cleared\n");
        ++bad;
    }
    edvr::setExternalCameraOnFoot(true);
    if (!edvr::externalCameraOnFoot()) {
        printf("  FAIL  the mode flag did not survive a set\n");
        ++bad;
    }

    // It must NOT behave like the glitch mark. That one is cleared every frame
    // by design; this one is a STATE, and clearing it at the frame boundary
    // would drop the player out of the offset one frame after entering it.
    edvr::clearGlitchFrame();
    if (!edvr::externalCameraOnFoot()) {
        printf("  FAIL  the frame boundary cleared the mode state\n");
        ++bad;
    }

    // ...and the two must not share storage.
    edvr::markGlitchFrame();
    edvr::setExternalCameraOnFoot(false);
    if (!edvr::glitchFrameMarked()) {
        printf("  FAIL  clearing the mode flag also cleared the glitch mark\n");
        ++bad;
    }
    edvr::clearGlitchFrame();

    if (bad == 0) printf("  ok    the mode flag round-trips and is independent\n");
    return bad;
}

// Both eyes of a frame get the same verdict, whenever the flag moves.
//
// The mark is read once per eye, at each Submit, and it legitimately changes
// mid-frame -- the detector re-decides on every new furthest camera. A change
// landing between the two Submits would show one eye this frame and the other a
// reprojection of the last one: a one-frame binocular mismatch, which is what a
// flash feels like. The fix for flashes, producing one.
//
// The channel carries no frame identity (EDVR-31), so this is asserted on the
// latch rather than on frame numbers.
int submitPairChecks() {
    int bad = 0;

    // The flag comes up between the two eyes. The first eye decided "show it",
    // so the second must show it too.
    {
        edvr::SubmitPairLatch latch;
        const bool left = latch.verdict(false);
        const bool right = latch.verdict(true);
        if (left || right) {
            printf("  FAIL  the flag rising between eyes split the pair "
                   "(left %s, right %s)\n", left ? "withheld" : "shown",
                   right ? "withheld" : "shown");
            ++bad;
        }
    }

    // And the other direction: the first eye decided "withhold", so the second
    // is withheld even though the mark has since been withdrawn. Consistent-late
    // rather than one eye ahead of the other.
    {
        edvr::SubmitPairLatch latch;
        const bool left = latch.verdict(true);
        const bool right = latch.verdict(false);
        if (!left || !right) {
            printf("  FAIL  the flag falling between eyes split the pair "
                   "(left %s, right %s)\n", left ? "withheld" : "shown",
                   right ? "withheld" : "shown");
            ++bad;
        }
    }

    // A third read -- a runtime that submits more than twice, or a retry --
    // still follows the frame's verdict rather than re-deciding.
    {
        edvr::SubmitPairLatch latch;
        latch.verdict(true);
        latch.verdict(false);
        if (!latch.verdict(false)) {
            printf("  FAIL  a third Submit in one frame re-decided\n");
            ++bad;
        }
    }

    // The boundary releases it, so the next frame is judged on its own merits.
    // Without this one detection would withhold every frame that followed, which
    // is the headset freezing rather than skipping a frame.
    {
        edvr::SubmitPairLatch latch;
        latch.verdict(true);
        latch.reset();
        if (latch.latched() || latch.verdict(false)) {
            printf("  FAIL  the frame boundary did not release the pair verdict\n");
            ++bad;
        }
    }

    if (bad == 0)
        printf("  ok    both eyes of a frame get the same verdict\n");
    return bad;
}

// The resubmit shadow: a withhold hands SteamVR the game's own PREVIOUS
// frame, never the live one and never EDVR-authored pixels (1f).
//
// Driven against a real D3D11 device (WARP, so no GPU and no headset is
// needed) with textures whose bytes are known, because the property under
// test is about CONTENTS: the substitute must hold what was last forwarded,
// a withheld frame's content must never reach it, and a shape change must
// fall back to classic withholding rather than submit a stale-shaped copy.
int resubmitChecks() {
    int bad = 0;

    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0,
                                 nullptr, 0, D3D11_SDK_VERSION, &dev, nullptr,
                                 &ctx)) ||
        !dev || !ctx) {
        // WARP ships with Windows 8+; failing to create it is a broken machine,
        // not an acceptable skip -- a skipped cell would report a fix as
        // covered that no test had touched.
        printf("  FAIL  could not create a WARP device to test the resubmit "
               "shadow against\n");
        return 1;
    }

    auto makeTex = [&](uint32_t side, uint8_t fill) {
        D3D11_TEXTURE2D_DESC d{};
        d.Width = side;
        d.Height = side;
        d.MipLevels = 1;
        d.ArraySize = 1;
        d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        std::vector<uint8_t> bytes(static_cast<size_t>(side) * side * 4, fill);
        D3D11_SUBRESOURCE_DATA init{};
        init.pSysMem = bytes.data();
        init.SysMemPitch = side * 4;
        ID3D11Texture2D* t = nullptr;
        dev->CreateTexture2D(&d, &init, &t);
        return t;
    };
    auto firstByte = [&](void* tex, uint32_t side, uint8_t* out) {
        D3D11_TEXTURE2D_DESC d{};
        d.Width = side;
        d.Height = side;
        d.MipLevels = 1;
        d.ArraySize = 1;
        d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_STAGING;
        d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ID3D11Texture2D* staging = nullptr;
        if (FAILED(dev->CreateTexture2D(&d, nullptr, &staging)) || !staging)
            return false;
        ctx->CopyResource(staging, static_cast<ID3D11Texture2D*>(tex));
        D3D11_MAPPED_SUBRESOURCE map{};
        bool ok = false;
        if (SUCCEEDED(ctx->Map(staging, 0, D3D11_MAP_READ, 0, &map))) {
            *out = static_cast<const uint8_t*>(map.pData)[0];
            ctx->Unmap(staging, 0);
            ok = true;
        }
        staging->Release();
        return ok;
    };

    ID3D11Texture2D* texA = makeTex(64, 0xAA);   // the frame that gets forwarded
    ID3D11Texture2D* texB = makeTex(64, 0xBB);   // the live (withheld) frame
    ID3D11Texture2D* texC = makeTex(128, 0xCC);  // a shape change
    if (!texA || !texB || !texC) {
        printf("  FAIL  could not create the resubmit test textures\n");
        if (texA) texA->Release();
        if (texB) texB->Release();
        if (texC) texC->Release();
        ctx->Release();
        dev->Release();
        return 1;
    }

    edvr::resubmitShadowConfigure();

    // Before anything was forwarded, a withhold has nothing to hand over --
    // classic withholding, which is the never-worse-than-before floor.
    if (edvr::resubmitShadowForWithhold(0, texB) != nullptr) {
        printf("  FAIL  a substitute was offered before any frame had been "
               "forwarded\n");
        ++bad;
    }

    // Pattern A is forwarded; the next frame is withheld while the live
    // texture holds pattern B. The substitute must be a texture that is
    // neither the live one nor the forwarded original, holding A's bytes.
    edvr::resubmitShadowNoteForwarded(0, texA);
    void* sub = edvr::resubmitShadowForWithhold(0, texB);
    if (!sub || sub == texB || sub == texA) {
        printf("  FAIL  the withheld frame was not offered an EDVR-owned copy "
               "(got %p, live %p, forwarded %p)\n", sub,
               static_cast<void*>(texB), static_cast<void*>(texA));
        ++bad;
    } else {
        uint8_t v = 0;
        if (!firstByte(sub, 64, &v) || v != 0xAA) {
            printf("  FAIL  the substitute holds 0x%02X, not the forwarded "
                   "frame's 0xAA\n", v);
            ++bad;
        }
    }

    // A second consecutive withhold forwards the SAME content again: nothing
    // a withheld frame carries may reach the shadow.
    void* sub2 = edvr::resubmitShadowForWithhold(0, texB);
    if (sub2 != sub) {
        printf("  FAIL  consecutive withholds got different substitutes\n");
        ++bad;
    } else if (sub2) {
        uint8_t v = 0;
        if (!firstByte(sub2, 64, &v) || v != 0xAA) {
            printf("  FAIL  the second consecutive withhold's content changed "
                   "to 0x%02X -- a withheld frame reached the shadow\n", v);
            ++bad;
        }
    }

    // The eye texture changes shape mid-flight: this withhold must fall back
    // to classic (nullptr) rather than submit a 64x64 copy where a 128x128
    // frame belongs.
    if (edvr::resubmitShadowForWithhold(0, texC) != nullptr) {
        printf("  FAIL  a shape change mid-flight was handed the stale-shaped "
               "copy\n");
        ++bad;
    }

    // ...and the next FORWARDED frame at the new shape rebuilds the copy.
    edvr::resubmitShadowNoteForwarded(0, texC);
    void* sub3 = edvr::resubmitShadowForWithhold(0, texC);
    if (!sub3 || sub3 == texC) {
        printf("  FAIL  the copy did not rebuild after a shape change\n");
        ++bad;
    } else {
        uint8_t v = 0;
        if (!firstByte(sub3, 128, &v) || v != 0xCC) {
            printf("  FAIL  the rebuilt copy holds 0x%02X, not the newly "
                   "forwarded 0xCC\n", v);
            ++bad;
        }
    }

    // The eyes do not share a shadow: eye 1 never forwarded, so it still
    // withholds classically whatever eye 0 has.
    if (edvr::resubmitShadowForWithhold(1, texB) != nullptr) {
        printf("  FAIL  eye 1 was offered eye 0's frame\n");
        ++bad;
    }

    edvr::resubmitShadowShutdown();
    texA->Release();
    texB->Release();
    texC->Release();
    ctx->Release();
    dev->Release();

    if (bad == 0)
        printf("  ok    a withheld frame resubmits the game's previous frame, "
               "and only that\n");
    return bad;
}

// Which binding fires, given what is physically held.
//
// This is the rule that decides whether EDVR sees the same camera keypress the
// game sees, and a miss is expensive: external_camera is a TOGGLE, so one
// dropped press inverts the intent for the rest of the session and the offset
// arms in the wrong place or never arms at all. Asserted rather than reasoned
// about, because the failure is invisible -- a press that did nothing.
int hotkeyMatchChecks() {
    int bad = 0;
    const uint32_t C = edvr::kHotkeyCtrl, A = edvr::kHotkeyAlt, S = edvr::kHotkeyShift;

    edvr::hotkeyResetBindings();
    edvr::Hotkey combo, plainOther;
    combo.setBinding("CTRL+ALT+SPACE");        // Elite's default camera bind
    plainOther.setBinding("F9");               // an unrelated single key

    // THE CASE THAT MOTIVATED THIS. Sprinting holds Shift; the player presses
    // their camera key. Under equality this missed, and the intent desynced.
    if (!edvr::hotkeyWouldFire(VK_SPACE, C | A, C | A | S)) {
        printf("  FAIL  CTRL+ALT+SPACE did not fire with Shift also held\n");
        ++bad;
    }
    if (!edvr::hotkeyWouldFire(VK_SPACE, C | A, C | A)) {
        printf("  FAIL  CTRL+ALT+SPACE did not fire with exactly its modifiers\n");
        ++bad;
    }
    // ...but a missing required modifier still must not fire.
    if (edvr::hotkeyWouldFire(VK_SPACE, C | A, C)) {
        printf("  FAIL  CTRL+ALT+SPACE fired with Alt not held\n");
        ++bad;
    }
    if (edvr::hotkeyWouldFire(VK_SPACE, C | A, 0)) {
        printf("  FAIL  CTRL+ALT+SPACE fired on a bare press\n");
        ++bad;
    }

    // LEGACY SEMANTICS. A plain binding with no combo on the same key fires
    // whatever else is held -- which is how Scroll Lock and Pause behaved before
    // modifiers existed here at all. Equality silently changed that.
    if (!edvr::hotkeyWouldFire(VK_F9, 0, S)) {
        printf("  FAIL  a plain binding stopped firing while Shift was held -- "
               "an unannounced change to keys that predate combos\n");
        ++bad;
    }

    // ONE PRESS, ONE BINDING. With both bound on the same key, CTRL+ALT+SPACE
    // must fire the combo and NOT the bare one: firing both would set the
    // camera intent and clear it again in the same frame.
    edvr::Hotkey plainSame;
    plainSame.setBinding("SPACE");
    if (!edvr::hotkeyWouldFire(VK_SPACE, C | A, C | A)) {
        printf("  FAIL  registering a bare SPACE binding broke the combo\n");
        ++bad;
    }
    if (edvr::hotkeyWouldFire(VK_SPACE, 0, C | A)) {
        printf("  FAIL  one press fired BOTH the combo and the bare binding on "
               "the same key\n");
        ++bad;
    }
    // ...and the bare one still fires on its own press.
    if (!edvr::hotkeyWouldFire(VK_SPACE, 0, 0)) {
        printf("  FAIL  the bare binding stopped firing on a bare press\n");
        ++bad;
    }
    // The suppression must be specific to the key, not global.
    if (!edvr::hotkeyWouldFire(VK_F9, 0, C | A)) {
        printf("  FAIL  a combo on SPACE suppressed an unrelated binding on F9\n");
        ++bad;
    }

    edvr::hotkeyResetBindings();
    if (bad == 0)
        printf("  ok    modifier matching: extras allowed, one press one binding\n");
    return bad;
}

// One physical press must produce at most one edge, per binding.
//
// The parsing and matching tests above cover which binding SHOULD fire. This
// covers when the edge happens, which is a separate thing and is where the
// harder bug lived: a binding that was correctly suppressed could still mint an
// edge later, from a press that was already over.
int hotkeyEdgeChecks() {
    int bad = 0;
    const uint32_t C = edvr::kHotkeyCtrl, A = edvr::kHotkeyAlt, S = edvr::kHotkeyShift;

    edvr::hotkeyResetBindings();
    edvr::Hotkey combo, bare;
    combo.setBinding("CTRL+ALT+SPACE");
    bare.setBinding("SPACE");

    // THE BUG. The player presses CTRL+ALT+SPACE and lets go of the modifiers
    // slightly before the spacebar, which is how anybody releases a chord.
    //
    //   frame 1  all three down   -> the combo fires, the bare one is suppressed
    //   frame 2  modifiers up, SPACE still down
    //            -> suppression lifts. The bare binding used to fire HERE,
    //               from a press the player made once and had finished with.
    int comboFires = 0, bareFires = 0;
    if (combo.pressedWith(true, C | A, true)) ++comboFires;
    if (bare.pressedWith(true, C | A, true)) ++bareFires;
    if (combo.pressedWith(true, 0, true)) ++comboFires;
    if (bare.pressedWith(true, 0, true)) ++bareFires;      // <- the old edge
    // ...and the key finally comes up.
    combo.pressedWith(false, 0, true);
    bare.pressedWith(false, 0, true);

    if (comboFires != 1) {
        printf("  FAIL  the combo fired %d time(s) for one press, expected 1\n",
               comboFires);
        ++bad;
    }
    if (bareFires != 0) {
        printf("  FAIL  releasing the modifiers minted %d edge(s) on the bare "
               "binding from a press that was already over\n", bareFires);
        ++bad;
    }

    // Holding a key does not repeat.
    edvr::hotkeyResetBindings();
    edvr::Hotkey solo;
    solo.setBinding("F9");
    int fires = 0;
    for (int i = 0; i < 20; ++i) {
        if (solo.pressedWith(true, 0, true)) ++fires;
    }
    if (fires != 1) {
        printf("  FAIL  holding a key fired %d time(s), expected 1\n", fires);
        ++bad;
    }
    // ...and releasing then pressing again does.
    solo.pressedWith(false, 0, true);
    if (!solo.pressedWith(true, 0, true)) {
        printf("  FAIL  a second press after releasing did not fire\n");
        ++bad;
    }

    // A press made while the game does NOT have focus must not fire when focus
    // comes back with the key still held. Same shape as the chord case: the
    // edge would come from a press aimed at another application.
    edvr::hotkeyResetBindings();
    edvr::Hotkey bg;
    bg.setBinding("F10");
    bg.pressedWith(true, 0, false);          // pressed elsewhere
    if (bg.pressedWith(true, 0, true)) {
        printf("  FAIL  a key held from before the game regained focus fired\n");
        ++bad;
    }
    // A fresh press once focused does fire.
    bg.pressedWith(false, 0, true);
    if (!bg.pressedWith(true, 0, true)) {
        printf("  FAIL  a fresh press after focus returned did not fire\n");
        ++bad;
    }

    edvr::hotkeyResetBindings();
    if (bad == 0)
        printf("  ok    one physical press produces at most one edge\n");
    return bad;
}

// Hotkey bindings, including combinations.
//
// Elite's own default for the external camera is CTRL + ALT + SPACE, so chords
// are the normal case rather than an extra. A parser that dropped the modifiers
// would leave a binding watching bare SPACE -- firing constantly, in a build
// whose whole job is to know which mode the player asked for. Silent, and
// exactly backwards, so it is asserted.
int hotkeyChecks() {
    int bad = 0;
    uint32_t m = 0xFFFFFFFFu;

    if (edvr::virtualKeyFromName("F9", &m) != VK_F9 || m != 0) {
        printf("  FAIL  a plain key did not parse, or invented modifiers\n");
        ++bad;
    }
    const int space = edvr::virtualKeyFromName("CTRL+ALT+SPACE", &m);
    if (space != VK_SPACE || m != (edvr::kHotkeyCtrl | edvr::kHotkeyAlt)) {
        printf("  FAIL  CTRL+ALT+SPACE parsed as vk=%d mods=%u\n", space, m);
        ++bad;
    }
    // Elite's default written the other ways people write it.
    if (edvr::virtualKeyFromName("ctrl + alt + space", &m) != VK_SPACE ||
        m != (edvr::kHotkeyCtrl | edvr::kHotkeyAlt)) {
        printf("  FAIL  lower case and spaces did not parse the same\n");
        ++bad;
    }
    if (edvr::virtualKeyFromName("CONTROL-MENU-SPACE", &m) != VK_SPACE ||
        m != (edvr::kHotkeyCtrl | edvr::kHotkeyAlt)) {
        printf("  FAIL  the CONTROL/MENU spellings or '-' did not parse\n");
        ++bad;
    }
    if (edvr::virtualKeyFromName("SHIFT+F11", &m) != VK_F11 ||
        m != edvr::kHotkeyShift) {
        printf("  FAIL  SHIFT+F11 did not parse\n");
        ++bad;
    }
    // A component that is not a modifier must make the WHOLE binding
    // unrecognised. Taking the last part and ignoring the rest would turn a
    // typo into a different, live binding.
    if (edvr::virtualKeyFromName("CTRL+WOMBAT+SPACE", &m) != 0) {
        printf("  FAIL  an unknown modifier did not reject the binding\n");
        ++bad;
    }
    if (edvr::virtualKeyFromName("CTRL+", &m) != 0) {
        printf("  FAIL  a binding with no key was accepted\n");
        ++bad;
    }

    // The punctuation row (6az: a field user bound '\\' and '[' and got
    // "not a key name EDVR knows"). Characters resolve through the keyboard
    // layout, names resolve to the same key as their character, and '-' is a
    // KEY unless it follows a modifier word -- the old parser split on it
    // anywhere, which made the minus key unbindable.
    const int backslash = edvr::virtualKeyFromName("\\", &m);
    if (backslash == 0 || m != 0) {
        printf("  FAIL  '\\' did not bind (vk=%d mods=%u)\n", backslash, m);
        ++bad;
    }
    if (edvr::virtualKeyFromName("BACKSLASH", &m) != backslash) {
        printf("  FAIL  BACKSLASH and '\\' bound different keys\n");
        ++bad;
    }
    const int lbracket = edvr::virtualKeyFromName("[", &m);
    if (lbracket == 0) {
        printf("  FAIL  '[' did not bind\n");
        ++bad;
    }
    if (edvr::virtualKeyFromName("LEFTBRACKET", &m) != lbracket) {
        printf("  FAIL  LEFTBRACKET and '[' bound different keys\n");
        ++bad;
    }
    const int minus = edvr::virtualKeyFromName("-", &m);
    if (minus == 0 || m != 0) {
        printf("  FAIL  '-' alone did not bind as the minus key\n");
        ++bad;
    }
    if (edvr::virtualKeyFromName("SHIFT+-", &m) != minus ||
        m != edvr::kHotkeyShift) {
        printf("  FAIL  SHIFT+- did not parse as shift and the minus key\n");
        ++bad;
    }
    if (edvr::virtualKeyFromName(";", &m) == 0) {
        printf("  FAIL  ';' did not bind\n");
        ++bad;
    }
    // A shifted character names the same physical key as its base: the high
    // half of the layout lookup is deliberately ignored.
    if (edvr::virtualKeyFromName("|", &m) != backslash) {
        printf("  FAIL  '|' and '\\' are the same physical key and did not "
               "bind alike\n");
        ++bad;
    }
    // HASH exists because a bare '#' after "= " is eaten by the ini's own
    // trailing-comment rule -- the name is the reliable spelling there.
    const int hash = edvr::virtualKeyFromName("HASH", &m);
    if (hash == 0 || edvr::virtualKeyFromName("#", &m) != hash) {
        printf("  FAIL  HASH and '#' did not bind the same key\n");
        ++bad;
    }

    // Elite's Key_ names translate to bindings this parser accepts -- the
    // bridge that lets bindings be adopted from the game's own files.
    char t[32];
    if (!edvr::eliteBindsTranslateKey("Key_F11", t, sizeof(t)) ||
        edvr::virtualKeyFromName(t, &m) != VK_F11) {
        printf("  FAIL  Key_F11 did not translate to F11\n");
        ++bad;
    }
    if (!edvr::eliteBindsTranslateKey("Key_RightArrow", t, sizeof(t)) ||
        edvr::virtualKeyFromName(t, &m) != VK_RIGHT) {
        printf("  FAIL  Key_RightArrow did not translate to RIGHT\n");
        ++bad;
    }
    if (!edvr::eliteBindsTranslateKey("Key_LeftBracket", t, sizeof(t)) ||
        edvr::virtualKeyFromName(t, &m) == 0) {
        printf("  FAIL  Key_LeftBracket did not translate to a bindable key\n");
        ++bad;
    }
    if (!edvr::eliteBindsTranslateKey("Key_SemiColon", t, sizeof(t)) ||
        edvr::virtualKeyFromName(t, &m) == 0) {
        printf("  FAIL  Key_SemiColon did not translate to a bindable key\n");
        ++bad;
    }
    if (edvr::eliteBindsTranslateKey("Key_LeftShift", t, sizeof(t))) {
        printf("  FAIL  a bare modifier translated as a main key\n");
        ++bad;
    }

    // setBinding must carry BOTH halves. This is the regression that matters:
    // setKey(virtualKeyFromName(s)) compiles and drops the modifiers.
    edvr::Hotkey k;
    k.setBinding("CTRL+ALT+SPACE");
    if (k.key() != VK_SPACE || k.mods() != (edvr::kHotkeyCtrl | edvr::kHotkeyAlt)) {
        printf("  FAIL  setBinding dropped the modifiers\n");
        ++bad;
    }
    // ...and setting a plain key afterwards must clear them, or the chord's
    // modifiers would linger on a binding that no longer wants any.
    k.setKey(VK_F9);
    if (k.mods() != 0) {
        printf("  FAIL  setKey left stale modifiers behind\n");
        ++bad;
    }

    if (bad == 0) printf("  ok    hotkey bindings parse, including combinations\n");
    return bad;
}

// The heartbeat, which is the part that decides whether a player's viewpoint
// stays moved after the gate stops running.
//
// Written as a test because the failure is invisible in every log: a frozen
// gate publishes nothing, so nothing says the flag went stale, and the symptom
// is "the offset is applied in the cockpit" several minutes later.
int liveFlagChecks() {
    int bad = 0;
    const uint32_t kMaxAge = 5;

    // FIRST, before anything in this process has published: the d3d11-absent
    // case, where openvr_api.dll is installed and its partner is not. There is
    // nobody to ask, which is not the same as being told no, and guessing yes
    // would apply the offset in every mode with no gate at all.
    //
    // This assertion is why liveFlagChecks runs before frameFlagChecks: once
    // anything has called setExternalCameraOnFoot, the "never published" state
    // is unreachable for the rest of the process and the case goes untested.
    if (edvr::externalCameraOnFootLive(kMaxAge)) {
        printf("  FAIL  the live flag read true with nothing ever published\n");
        ++bad;
    }

    // A writer that keeps publishing keeps it live, including across a run of
    // frames longer than the staleness window -- the heartbeat is the WRITES,
    // not the changes.
    bool heldLive = true;
    for (uint32_t i = 0; i < kMaxAge * 4; ++i) {
        edvr::setExternalCameraOnFoot(true);
        if (!edvr::externalCameraOnFootLive(kMaxAge)) heldLive = false;
    }
    if (!heldLive) {
        printf("  FAIL  an actively refreshed flag went stale while being written\n");
        ++bad;
    }

    // Now the writer stops. The raw flag still says yes -- that is the whole
    // hazard -- and the live one must give up within the window.
    uint32_t agedOutAt = 0;
    for (uint32_t i = 1; i <= kMaxAge * 3; ++i) {
        if (!edvr::externalCameraOnFootLive(kMaxAge)) { agedOutAt = i; break; }
    }
    if (!edvr::externalCameraOnFoot()) {
        printf("  FAIL  the raw flag did not stay set -- the test is not testing "
               "the stuck case\n");
        ++bad;
    }
    if (agedOutAt == 0) {
        printf("  FAIL  a frozen gate never aged out; the offset would stay "
               "applied for the session\n");
        ++bad;
    } else if (agedOutAt <= kMaxAge) {
        printf("  FAIL  aged out after %u frames, inside the %u-frame window\n",
               agedOutAt, kMaxAge);
        ++bad;
    }

    // And it recovers: a gate that starts publishing again is believed again,
    // so a single fault-and-recover does not disable the feature for good.
    edvr::setExternalCameraOnFoot(true);
    if (!edvr::externalCameraOnFootLive(kMaxAge)) {
        printf("  FAIL  a gate that resumed publishing was not believed again\n");
        ++bad;
    }

    // A live NO is still a no. The stamp moving must not be mistaken for the
    // answer being yes.
    edvr::setExternalCameraOnFoot(false);
    if (edvr::externalCameraOnFootLive(kMaxAge)) {
        printf("  FAIL  a refreshed 'off' read as on\n");
        ++bad;
    }
    edvr::setExternalCameraOnFoot(false);

    if (bad == 0)
        printf("  ok    the mode flag ages out when the gate stops publishing\n");
    return bad;
}

int sentinelChecks() {
    wchar_t base[MAX_PATH];
    GetTempPathW(MAX_PATH, base);
    wchar_t dir[MAX_PATH];
    _snwprintf_s(dir, _TRUNCATE, L"%sedvr_sentinel_test_%lu", base, GetCurrentProcessId());
    wchar_t file[MAX_PATH];
    _snwprintf_s(file, _TRUNCATE, L"%s\\probe.armed", dir);

    RemoveDirectoryW(dir);   // from a previous run, if any
    int rc = 0;

    {
        // The directory does NOT exist yet. This is the log.enabled = 0 case.
        edvr::Sentinel s(dir, L"probe");
        if (s.trippedOnStartup()) {
            printf("  FAIL  a fresh sentinel reports a trip with no file present\n");
            rc = 1;
        }
        if (!s.arm()) {
            printf("  FAIL  arm() failed with no log directory -- it must create it\n");
            rc = 1;
        } else if (GetFileAttributesW(file) == INVALID_FILE_ATTRIBUTES) {
            printf("  FAIL  arm() reported success but wrote no file\n");
            rc = 1;
        }
    }
    {
        // A new sentinel over the same path is the next launch.
        edvr::Sentinel s(dir, L"probe");
        if (!s.trippedOnStartup()) {
            printf("  FAIL  the armed file did not trip the next sentinel\n");
            rc = 1;
        }
        s.clearTrip();
        if (GetFileAttributesW(file) != INVALID_FILE_ATTRIBUTES) {
            printf("  FAIL  clearTrip() left the file in place -- a false trip would\n");
            printf("        then refuse every launch forever\n");
            rc = 1;
        }
        if (s.trippedOnStartup()) {
            printf("  FAIL  clearTrip() left the in-memory trip set, so a second request\n");
            printf("        this session would install after being refused\n");
            rc = 1;
        }
    }
    {
        // The normal success path: armed, then confirmed.
        edvr::Sentinel s(dir, L"probe");
        s.arm();
        s.confirm();
        if (GetFileAttributesW(file) != INVALID_FILE_ATTRIBUTES) {
            printf("  FAIL  confirm() did not remove the armed file\n");
            rc = 1;
        }
    }
    {
        // Somewhere it cannot possibly write. arm() must say so rather than
        // claiming protection it does not have.
        edvr::Sentinel s(L"\\\\?\\Z:\\nonexistent-volume\\edvr", L"probe");
        if (s.arm()) {
            printf("  FAIL  arm() returned true for a path it cannot write\n");
            rc = 1;
        }
    }

    RemoveDirectoryW(dir);
    if (rc == 0) printf("  ok    crash sentinel arms, trips, clears and confirms\n");
    return rc;
}

int main(int argc, char** argv) {
    if (argc >= 3 && strcmp(argv[2], "--fault-child") == 0) return faultChild(argv[1]);

    printf("edvr openvr smoke\n");
    if (argc < 2) {
        printf("usage: openvr_smoke.exe <dir with openvr_api.dll + openvr_api_orig.dll>\n");
        return 2;
    }
    _snwprintf_s(g_proxyPath, _TRUNCATE, L"%hs\\openvr_api.dll", argv[1]);
    printf("proxy: %ls\n\n", g_proxyPath);

    // 1. The load must not deadlock.
    HANDLE t = CreateThread(nullptr, 0, loadProxy, nullptr, 0, nullptr);
    if (!t) return fail("could not create the loader thread");
    const DWORD waited = WaitForSingleObject(t, 15000);
    if (waited == WAIT_TIMEOUT) {
        printf("  FAIL  loading the proxy did not finish in 15 seconds\n");
        printf("        Something in the load path is blocking under the loader lock.\n");
        printf("\nOPENVR SMOKE FAILED\n");
        return 1;   // the thread is stuck; process exit takes it with us
    }
    CloseHandle(t);
    if (!g_loaded) return fail("the proxy did not load at all");
    printf("  ok    proxy loaded without deadlocking\n");

    // 2. ...and it did so without pulling in the real module.
    if (GetModuleHandleW(L"openvr_api_orig.dll")) {
        printf("  FAIL  openvr_api_orig.dll was mapped by DllMain\n");
        printf("        Deferral is not happening; assertion 1 passed by luck.\n");
        printf("\nOPENVR SMOKE FAILED\n");
        return 1;
    }
    printf("  ok    real module not loaded yet -- deferred as intended\n");

    // 3. The first export call resolves it and forwards correctly.
    PFN_Probe probe =
        reinterpret_cast<PFN_Probe>(GetProcAddress(g_loaded, "VR_IsHmdPresent"));
    if (!probe) return fail("VR_IsHmdPresent is not exported by the proxy");

    const unsigned long long got = probe(7ull, 6.0, 5ull, 4.0, 3ull, 2ull);
    const unsigned long long want = 7ull + 60ull + 500ull + 4000ull + 30000ull + 200000ull;
    if (got != want) {
        printf("  FAIL  forwarded call returned %llu, expected %llu\n", got, want);
        printf("        An argument was corrupted across the lazy-load path.\n");
        printf("\nOPENVR SMOKE FAILED\n");
        return 1;
    }
    printf("  ok    first call resolved the table and passed all six arguments\n");

    if (!GetModuleHandleW(L"openvr_api_orig.dll")) {
        return fail("the real module still is not loaded after a forwarded call");
    }
    printf("  ok    real module loaded on demand\n");

    // 5. A real module whose DllMain faults degrades instead of killing us.
    {
        wchar_t self[MAX_PATH]{};
        GetModuleFileNameW(nullptr, self, MAX_PATH);
        wchar_t cmd[MAX_PATH * 2];
        _snwprintf_s(cmd, _TRUNCATE, L"\"%s\" \"%hs\" --fault-child", self, argv[1]);

        SetEnvironmentVariableW(L"EDVR_FAKEVR_FAULT", L"1");
        STARTUPINFOW si{};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};
        const BOOL ok = CreateProcessW(nullptr, cmd, nullptr, nullptr, FALSE, 0, nullptr,
                                       nullptr, &si, &pi);
        SetEnvironmentVariableW(L"EDVR_FAKEVR_FAULT", nullptr);
        if (!ok) return fail("could not start the fault child");

        WaitForSingleObject(pi.hProcess, 20000);
        DWORD code = 0xFFFFFFFF;
        GetExitCodeProcess(pi.hProcess, &code);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);

        if (code != 0) {
            printf("  FAIL  a faulting real-module DllMain did not degrade cleanly "
                   "(child exit 0x%08lX)\n", code);
            printf("        Expected the export to return the stub's zero and the\n");
            printf("        process to survive. It did not return at all.\n");
            printf("\nOPENVR SMOKE FAILED\n");
            return 1;
        }
        printf("  ok    a faulting real module degrades to stubs, process survives\n");
    }

    // Shared code with no other coverage. Runs last because it touches nothing
    // the assertions above depend on.
    //
    // liveFlagChecks BEFORE frameFlagChecks, and the order is load-bearing: its
    // first assertion is the "d3d11 never published" case, which stops existing
    // the moment anything calls setExternalCameraOnFoot.
    if (hotkeyMatchChecks() != 0) {
        printf("\nOPENVR SMOKE FAILED\n");
        return 1;
    }
    if (hotkeyEdgeChecks() != 0) {
        printf("\nOPENVR SMOKE FAILED\n");
        return 1;
    }
    if (hotkeyChecks() != 0) {
        printf("\nOPENVR SMOKE FAILED\n");
        return 1;
    }
    if (liveFlagChecks() != 0) {
        printf("\nOPENVR SMOKE FAILED\n");
        return 1;
    }
    if (frameFlagChecks() != 0) {
        printf("\nOPENVR SMOKE FAILED\n");
        return 1;
    }
    if (submitPairChecks() != 0) {
        printf("\nOPENVR SMOKE FAILED\n");
        return 1;
    }
    if (resubmitChecks() != 0) {
        printf("\nOPENVR SMOKE FAILED\n");
        return 1;
    }
    if (sentinelChecks() != 0) {
        printf("\nOPENVR SMOKE FAILED\n");
        return 1;
    }

    printf("\nOPENVR SMOKE PASSED\n");
    return 0;
}
