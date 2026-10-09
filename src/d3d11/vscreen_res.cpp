#include "vscreen_res.h"

#include <windows.h>

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <string>

#include "../common/config.h"
#include "../common/log.h"
#include "../common/proxy.h"
#include "../common/runtime_profile.h"
#include "../common/temporal_mode.h"
#include "../common/vscreen_auto_state.h"
#include "../common/vscreen_fit.h"
#include "ui_layer_math.h"
#include "vr_runtime.h"

namespace edvr {
namespace {

// The panel resolution is an OVERRIDE on a view mode, not a size with an owner.
//
// 6m closed this on "29 call sites, the resolution is threaded through the
// renderer, not owned anywhere". Reading the executable says otherwise: the
// exact instruction pair 6m patched -- `mov r8d, 0x780` / `mov r9d, 0x438` --
// occurs ONCE in the whole 81 MB of .text. Two view modes get a forced size and
// every other mode reads a real one from a struct. The 29 call sites 6m counted
// are downstream: they receive a size, they do not choose one. That is why
// patching one allocation changed 8 of 61 targets.
//
// What this touches: the width and height immediates. Not the comparison, not
// the branch, not the mode value, not the struct read.
//
// Nothing here is pinned to a version, deliberately.
//
// The obvious design pins to build 330683 and refuses on anything else. That is
// safe and it is also broken by every game update, which means the fix is off
// more often than on. Measurement says it is unnecessary: the SHAPE below --
// with no knowledge of 1920 or 1080 at all -- matches exactly six places in
// 81 MB of code, which are the six we want.
//
//     cmp  r32, 1
//     ja   .other
//     mov  r32, <width>        ; a 16:9 pair, forced for this view mode
//     mov  r32, <height>
//     jmp  .done
//   .other:
//     mov  r32, [reg + disp]   ; the size the object really has
//
// Recognising the thing being edited is stronger verification than trusting a
// version string, because a version string says nothing about whether the code
// still means what it meant.
//
// It is also safe in a way worth being explicit about: this replaces a 4-byte
// immediate operand with another 4-byte immediate. Instruction lengths do not
// change, control flow does not change, and the instruction stream cannot
// desynchronise. If the signature ever matched something unintended, the result
// is a surface rendering at an odd size -- visible, harmless, and gone on
// restart. That is not the risk profile of a patch that rewrites instructions.
constexpr uint32_t kMinWidth = 640;      // below this it is not a render size

// What Elite forces today. Used only to recognise "the user asked for the stock
// resolution", which means do nothing -- NOT to find the sites, which is done
// by shape so that a build forcing some other size still works.
constexpr uint32_t kStockWidth = 1920;
constexpr uint32_t kStockHeight = 1080;

// A band, not an exact count, so a build that adds or drops a call site still
// works. Zero means the shape is gone entirely; a large number means the shape
// stopped being distinctive. Either is a refusal.
constexpr size_t kMinSites = 3;
constexpr size_t kMaxExpected = 12;
constexpr size_t kMaxSites = 32;
constexpr size_t kWindow = 26;   // bytes after the cmp that may hold the movs

struct Site {
    uint8_t* width = nullptr;
    uint8_t* height = nullptr;
};
Site g_sites[kMaxSites];
size_t g_count = 0;
bool g_applied = false;
// The width written, for the panel patch's size budget (vscreenModeAppliedWidth): 0 while nothing is.
std::atomic<uint32_t> g_appliedW{0};
// What the game had before we touched it, so revert restores what was actually
// there rather than a number this file assumed.
uint32_t g_origW = 0, g_origH = 0;

bool textSection(const uint8_t** begin, size_t* size) {
    const uint8_t* base = reinterpret_cast<const uint8_t*>(GetModuleHandleW(nullptr));
    if (!base) return false;
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;

    const auto* sec = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
        if (memcmp(sec->Name, ".text", 5) == 0) {
            *begin = base + sec->VirtualAddress;
            *size = sec->Misc.VirtualSize;
            return true;
        }
    }
    return false;
}

// Every `mov r32, imm32` in a window: the immediate's address and its value.
size_t collectMovs(const uint8_t* p, size_t n, const uint8_t** at, uint32_t* val,
                   size_t cap) {
    size_t k = 0;
    for (size_t i = 1; i + 4 <= n && k < cap; ++i) {
        const uint8_t op = p[i - 1];
        if (op < 0xB8 || op > 0xBF) continue;
        uint32_t v = 0;
        memcpy(&v, p + i, 4);
        at[k] = p + i;
        val[k] = v;
        ++k;
    }
    return k;
}

// No destructors here: __try is illegal in a function needing unwinding, and
// reading another module's memory is exactly where a guard belongs.
size_t scanSites(const uint8_t* begin, size_t size, Site* out, size_t cap,
                 uint32_t* foundW, uint32_t* foundH) {
    size_t n = 0;
    *foundW = 0;
    *foundH = 0;
    __try {
        for (size_t i = 0; i + 3 + kWindow <= size; ++i) {
            // cmp r32, 1  ->  83 /7 ib with modrm F8..FF
            if (begin[i] != 0x83 || begin[i + 1] < 0xF8 || begin[i + 2] != 0x01) continue;

            const uint8_t* movAt[12];
            uint32_t movVal[12];
            const size_t m = collectMovs(begin + i + 3, kWindow, movAt, movVal, 12);

            // A 16:9 pair among them, wide enough to be a render size. The
            // aspect test is what makes this a resolution rather than any two
            // constants that happen to sit near a comparison.
            for (size_t a = 0; a < m; ++a) {
                for (size_t c = 0; c < m; ++c) {
                    if (a == c) continue;
                    const uint32_t w = movVal[a], h = movVal[c];
                    if (w < kMinWidth || h == 0) continue;
                    if (w * 9 != h * 16) continue;

                    // Every site must agree on the same forced size. Sites that
                    // disagree would mean the shape is catching more than one
                    // thing, and patching all of them would be a guess.
                    if (*foundW == 0) {
                        *foundW = w;
                        *foundH = h;
                    } else if (*foundW != w || *foundH != h) {
                        return static_cast<size_t>(-2);
                    }
                    if (n < cap) {
                        out[n].width = const_cast<uint8_t*>(movAt[a]);
                        out[n].height = const_cast<uint8_t*>(movAt[c]);
                    }
                    ++n;
                    a = m;   // one pair per site
                    break;
                }
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return static_cast<size_t>(-1);
    }
    return n;
}

bool writeImm(uint8_t* at, uint32_t value) {
    DWORD prot = 0;
    if (!VirtualProtect(at, 4, PAGE_EXECUTE_READWRITE, &prot)) return false;
    memcpy(at, &value, 4);
    DWORD ignored = 0;
    VirtualProtect(at, 4, prot, &ignored);
    FlushInstructionCache(GetCurrentProcess(), at, 4);
    return true;
}

}  // namespace

bool applyVScreenModeResolution(uint32_t width, uint32_t height) {
    if (g_applied) return true;

    // Asking for the stock resolution is asking for nothing, so do nothing --
    // and do it BEFORE the scan, not after.
    //
    // This is what lets the shipped ini default to 1920x1080 rather than to 0.
    // A default of 0 means "off" and reads as a missing value; 1920x1080 says
    // what the game already does, so the setting documents itself and the
    // numbers to change are right there. Either way nothing is written and the
    // 81 MB scan never runs.
    //
    // The trade: on some future build that forces a different size, asking for
    // exactly 1920x1080 will be treated as "leave it alone" rather than as an
    // instruction to force it down to that. Worth it for a default that
    // explains itself, and the log says which resolution the game is really
    // forcing whenever the fix does run.
    if (width == kStockWidth && height == kStockHeight) {
        Log::get().note("vScreen resolution: off (set to %ux%u, which is the game's own "
                        "resolution). Nothing was scanned and nothing was written.",
                        width, height);
        return false;
    }

    // The game allocates render targets from these numbers, so absurd values
    // cost VRAM at best and fail allocation at worst.
    if (width < kMinWidth || height < 360 || width > 8192 || height > 8192) {
        Log::get().note("vScreen resolution REFUSED: %ux%u is outside the sane range "
                        "(640x360 to 8192x8192). Nothing was written.", width, height);
        return false;
    }

    const uint8_t* begin = nullptr;
    size_t size = 0;
    if (!textSection(&begin, &size)) {
        Log::get().note("vScreen resolution: could not locate the executable section; "
                        "nothing was written");
        return false;
    }

    uint32_t srcW = 0, srcH = 0;
    const size_t found = scanSites(begin, size, g_sites, kMaxSites, &srcW, &srcH);

    if (found == static_cast<size_t>(-1)) {
        Log::get().note("vScreen resolution: faulted while scanning; nothing was written");
        return false;
    }
    if (found == static_cast<size_t>(-2)) {
        Log::get().note("vScreen resolution REFUSED: the sites disagree about which "
                        "resolution is being forced, so this shape is matching more than "
                        "one thing. Nothing was written.");
        return false;
    }
    if (found < kMinSites || found > kMaxExpected) {
        Log::get().note("vScreen resolution REFUSED: found %zu site(s), expected between "
                        "%zu and %zu. Nothing was written. Either this game version no "
                        "longer forces the resolution this way, or the pattern has stopped "
                        "being specific enough to trust -- both are reasons to do nothing.",
                        found, kMinSites, kMaxExpected);
        return false;
    }
    if (srcW == width && srcH == height) {
        Log::get().note("vScreen resolution: the game already renders that screen at "
                        "%ux%u; nothing to do.", srcW, srcH);
        return false;
    }

    // Report what was found BEFORE changing it. If a game update moves this to
    // some other resolution, the log records the real one instead of leaving
    // anyone to assume it was still 1920x1080.
    const std::string build = gameBuildVersion();
    Log::get().note("vScreen resolution: %zu site(s) forcing %ux%u, on game build %s%s",
                    found, srcW, srcH,
                    build.empty() ? "(unreadable)" : build.c_str(),
                    gameBuildIsVerified()
                        ? " -- the build this was developed against."
                        : " -- NOT the build this was developed against. Proceeding anyway: "
                          "the code being changed was identified by its shape, which says "
                          "more about whether it is the right code than a version string "
                          "does. If the picture looks wrong, set both values to 0.");

    // Warn, do not refuse. A different aspect stretches rather than widens,
    // because the screen's shape is set elsewhere -- but "we think this looks
    // wrong" is not grounds to override a deliberate choice.
    if (width * 9 != height * 16) {
        Log::get().note("vScreen resolution: %ux%u is not 16:9, while the game forces "
                        "%ux%u which is. Expect a stretched picture. Applying anyway.",
                        width, height, srcW, srcH);
    }

    // All or nothing. A half-applied resolution renders worse than none, as an
    // earlier version of this established the hard way by shipping one.
    size_t done = 0;
    for (; done < found; ++done) {
        if (!writeImm(g_sites[done].width, width)) break;
        if (!writeImm(g_sites[done].height, height)) {
            writeImm(g_sites[done].width, srcW);
            break;
        }
    }
    if (done != found) {
        for (size_t i = 0; i < done; ++i) {
            writeImm(g_sites[i].width, srcW);
            writeImm(g_sites[i].height, srcH);
        }
        Log::get().note("vScreen resolution: a write failed at site %zu of %zu; every site "
                        "restored, nothing left changed.", done + 1, found);
        return false;
    }

    g_count = found;
    g_origW = srcW;
    g_origH = srcH;
    g_applied = true;
    g_appliedW.store(width, std::memory_order_release);
    Log::get().note("vScreen resolution: %ux%u -> %ux%u at %zu site(s). This writes to game "
                    "CODE -- to %zu pairs of numbers and nothing else. It reverts when the "
                    "game closes.",
                    srcW, srcH, width, height, found, found);
    return true;
}

void revertVScreenModeResolution() {
    if (!g_applied) return;
    for (size_t i = 0; i < g_count; ++i) {
        writeImm(g_sites[i].width, g_origW);
        writeImm(g_sites[i].height, g_origH);
    }
    Log::get().note("vScreen resolution reverted to %ux%u at %zu site(s)",
                    g_origW, g_origH, g_count);
    g_applied = false;
    g_appliedW.store(0, std::memory_order_release);
    g_count = 0;
}

uint32_t vscreenModeAppliedWidth() { return g_appliedW.load(std::memory_order_acquire); }

// --- "auto": what each eye actually shows, from what the runtime rendered last time ---
//
// TWO RULES, one reason to choose (src/common/vscreen_fit.h says all of it; docs/design-
// flat-temporal-aa-2026-09-23.md, section 82, the "vscreen auto-fit" entry): when the VR
// world route will run, auto is 70% (vscreenfit::kMultiplier) of the on-foot screen's
// head-on footprint in eye pixels (the instrument in vscreen_footprint.cpp measures it,
// the state file keeps its 10th percentile, the pure half does the arithmetic); otherwise
// it is today's 125% of the eye width, unchanged.
// The decision is made from the configuration this launch runs with plus two files of
// the last session's, and every caller (the panel patch, the intro movie's target, the
// menu's hint) asks this one function.
//
// WHY LAST SESSION'S NUMBER, NOT THIS ONE'S. The panel patch above has to run
// at device creation, before the game builds its render chain -- that is the
// whole reason this file exists as a code patch rather than a config value.
// But the OpenXR host that resolves fix.openxr_resolution into a real per-eye
// width is not even constructed until the game's device has Presented
// (docs/openxr-resolution-per-headset-2026-09-14.md), which is well after
// device creation. So at the moment this session's own panel size has to be
// decided, this session's own headset is not identified yet, let alone its
// resolved width. Persisting the LAST session's answer and reading it back
// here is the only way "auto" can track the runtime resolution at all without
// moving the patch later than "the device exists" -- which is not a change to
// make on a hypothesis, given what a wrong guess there costs.
//
// The trade this accepts: a resolution change tracks one restart late, which
// is no worse than this setting already requires for a manual change to take
// effect, and a fresh install runs "auto" as "off" (the same stock 1920x1080
// as before) until one session with VR running has completed.
namespace {

// The route's conditions as the configuration states them -- the same three the world
// route needs at run time, read from the ini the way each owner reads it, because at
// launch (and in the menu, for the next one) no owner has run yet:
//   * the on-foot world route: always on (vr_world_route.cpp's boundary)
//   * (the curved screen is not a condition: the route re-issues a curved screen through
//     the same strip the game's draw is substituted with, panel_curve.h panelCurveReissue)
//   * the UI layer: ui_layer.cpp's uiLayerConfigure -- fix.ui_quality (default 100), a
//     temporal mode on (fix.temporal_aa, default off) and the jitter switches as shipped;
//     ui_layer_math.h's uiLayerNotLiveReasonFor words the failure ("stood down" is a fact
//     of a running session, not of a launch, so it is false here)
//   * the runtime: the module list, which at device creation usually says "none loaded
//     yet" (the device is made about a second before openvr_api.dll is called). That is
//     undecided, not a failure: the eye width this rule starts from is only ever written
//     by EDVR's own OpenXR runtime (native_render_settings.cpp), so a launch that has one
//     on record has run on it. Elite's native Oculus back end loads LibOVRRT first and
//     is refused here.
// tools\vscreen_fit_test pins every literal below against its owner's source.
vscreenfit::RouteFacts routeFactsFromConfig(Config& cfg) {
    vscreenfit::RouteFacts f;
    f.flatProfile = runtimeFlatProfile();
    f.keyAuto = true;

    const std::string quality = cfg.getString("fix.ui_quality", "100");
    bool recognized = true;
    const float target = uiQualityParse(quality.c_str(), &recognized, nullptr);
    const bool temporal = temporalModeEnabled(cfg.getString("fix.temporal_aa", "off"));
    const bool jitterAsShipped = true;
    f.layerWhy = uiLayerNotLiveReasonFor(target, temporal, jitterAsShipped, /*stoodDown=*/false);

    switch (vrRuntime()) {
        case VrRuntime::NativeOpenXR:   f.runtime = vscreenfit::RuntimeKind::EdvrOpenXr; break;
        case VrRuntime::OculusNative:   f.runtime = vscreenfit::RuntimeKind::OculusNative; break;
        case VrRuntime::ForeignOpenvr:  f.runtime = vscreenfit::RuntimeKind::ForeignOpenvr; break;
        case VrRuntime::NoneLoaded:
        default:                        f.runtime = vscreenfit::RuntimeKind::NotLoadedYet; break;
    }
    return f;
}

}  // namespace

void resolveVScreenTargetResolution(Config& cfg, uint32_t* outWidth, uint32_t* outHeight,
                                    bool announce, vscreenfit::Decision* outDecision) {
    if (outWidth) *outWidth = 0;
    if (outHeight) *outHeight = 0;
    if (outDecision) *outDecision = vscreenfit::Decision{};
    const std::string widthCfg = cfg.getString("fix.vscreen_res_width", "auto");
    uint32_t w = 0;
    if (_stricmp(widthCfg.c_str(), "auto") == 0) {
        uint32_t eyeWidth = 0;
        if (!lastKnownEyeWidth(cfg.logDir(), &eyeWidth)) {
            if (announce) {
                Log::get().note(
                    "vScreen resolution: auto, but no per-eye render width is on record "
                    "yet for this install -- the on-foot screen stays at the game's own "
                    "1920x1080 until a session with VR running has completed once. Set "
                    "fix.vscreen_res_width to a width in pixels instead if you don't want "
                    "to wait.");
            }
            return;
        }
        vscreenfit::Inputs in;
        in.eyeWidth = eyeWidth;
        in.distance = cfg.getFloat("fix.panel_distance", 1.0f);
        vscreenfit::Record stored;
        if (lastKnownPanelFootprint(cfg.logDir(), &stored)) {
            in.haveFootprint = true;
            in.fractionAtUnit = stored.fractionAtUnit;
            in.footprintSamples = stored.samples;
        }
        in.route = routeFactsFromConfig(cfg);
        const vscreenfit::Decision d = vscreenfit::decide(in);
        if (outDecision) *outDecision = d;
        w = d.width;
        if (announce) {
            char line[1100];
            vscreenfit::formatRuleLine(line, sizeof(line), d, eyeWidth);
            Log::get().note("%s", line);
        }
    } else {
        w = static_cast<uint32_t>(atoi(widthCfg.c_str()));
        if (!w) return;  // "0", empty or unparsable: the same "leave it alone" as before
    }
    if (w < kMinWidth) w = kMinWidth;
    if (w > 8192) w = 8192;
    if (announce && _stricmp(widthCfg.c_str(), "auto") != 0) {
        Log::get().note(
            "vScreen resolution: explicit %u wide (fix.vscreen_res_width), %ux%u at 16:9, "
            "used exactly: auto's rule is not in play.",
            w, w, (w * 9 + 8) / 16);
    }
    if (outWidth) *outWidth = w;
    if (outHeight) *outHeight = (w * 9 + 8) / 16;
}

}  // namespace edvr

