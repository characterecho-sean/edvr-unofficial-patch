#include "supersample_resolve.h"

#include <windows.h>
#include <d3d11.h>   // GetDesc on the texture about to go out, for its size only

#include <cmath>
#include <cstring>
#include <string>

#include "../common/config.h"
#include "../common/guard.h"
#include "../common/log.h"
#include "../common/supersample_math.h"
#include "../common/timing.h"
#include "system_hook.h"

namespace edvr {
namespace {

enum class Mode : uint8_t { Off, Auto, On };

typedef void* (*PFN_EdvrSupersampleResolve)(void*, int, const float*, unsigned,
                                            unsigned, int, float, int);

// Same budget and same reasoning as the guard's crop: every treat
// dereferences a handle the game owns, and a read that faults every frame
// must retire itself rather than bleed the log.
constexpr uint32_t kMaxFaults = 8;

// How long the `on` mode waits before saying it has nothing to do. Long
// enough for the intro and the first scene to settle; the line is said once.
constexpr uint64_t kIdleNoteMs = 15000;

struct State {
    Mode  mode = Mode::Off;
    int   filter = kSupersampleCalm;
    float width = 1.0f;
    bool  configured = false;

    bool  standDown = false;          // inert for the session; said once
    SupersampleArmer armer;

    PFN_EdvrSupersampleResolve fn = nullptr;
    bool  fnTried = false;

    uint32_t faults = 0;
    bool     faultsNoted = false;
    bool     regionNoted = false;
    bool     noRecNoted = false;
    bool     idleNoted = false;
    uint64_t firstReportMs = 0;
    uint32_t lastSubW = 0, lastSubH = 0;   // the latest agreed per-eye size
    uint32_t treats = 0;
};
State g_s;

const char* modeName(Mode m) {
    return m == Mode::Auto ? "auto" : m == Mode::On ? "on" : "off";
}

const char* filterName(int f) {
    return f == kSupersampleCrisp ? "crisp (mitchell)" : "calm (gaussian)";
}

// OpenVR's colour-space rule, applied where the format is known only by
// its name: Auto means gamma (sRGB-encoded) for 8-bit formats and linear
// for everything else; Gamma and Linear say so outright. The d3d11 half
// ignores the hint for float formats, which are linear whatever is said.
int gammaHint(vr::EColorSpace space, DXGI_FORMAT f) {
    if (space == vr::ColorSpace_Gamma) return 1;
    if (space == vr::ColorSpace_Linear) return 0;
    switch (f) {
        case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        case DXGI_FORMAT_B8G8R8A8_TYPELESS:
        case DXGI_FORMAT_B8G8R8A8_UNORM:
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
            return 1;
        default:
            return 0;
    }
}

}  // namespace

void supersampleResolveConfigure() {
    State& s = g_s;
    Config& cfg = Config::get();

    const std::string raw = cfg.getString("experimental.supersample_resolve", "auto");
    Mode mode = Mode::Off;
    if (_stricmp(raw.c_str(), "auto") == 0) mode = Mode::Auto;
    else if (_stricmp(raw.c_str(), "on") == 0) mode = Mode::On;
    else if (_stricmp(raw.c_str(), "off") != 0 && !raw.empty()) {
        Log::get().note(
            "supersample_resolve = \"%s\" is not a mode this build knows "
            "(off, auto, on). Treating it as off.",
            raw.c_str());
    }

    const std::string rawFilter = cfg.getString("fix.supersample_filter", "calm");
    int filter = kSupersampleCalm;
    if (_stricmp(rawFilter.c_str(), "crisp") == 0) filter = kSupersampleCrisp;
    else if (_stricmp(rawFilter.c_str(), "calm") != 0 && !rawFilter.empty()) {
        Log::get().note(
            "supersample_filter = \"%s\" is not a kernel this build knows "
            "(calm, crisp). Using calm.",
            rawFilter.c_str());
    }

    float width = cfg.getFloat("experimental.supersample_width", 1.0f);
    if (!std::isfinite(width)) width = 1.0f;
    if (width < kSupersampleWidthMin) width = kSupersampleWidthMin;
    if (width > kSupersampleWidthMax) width = kSupersampleWidthMax;

    const bool first = !s.configured;
    const bool changed = first || mode != s.mode || filter != s.filter ||
                         fabsf(width - s.width) > 1e-4f;
    s.configured = true;
    s.mode = mode;
    s.filter = filter;
    s.width = width;
    if (!changed) return;
    if (mode == Mode::Off) {
        if (!first) {
            Log::get().note("supersample resolve: off. Every frame forwards as "
                            "the game submitted it from the next boundary.");
        }
        return;
    }
    Log::get().note(
        "supersample resolve: %s -- %s kernel, radius %.2f output pixels%s. "
        "It engages once both eyes submit larger than the runtime's "
        "recommended eye size (Elite's HMD Quality above 1.0 is the usual "
        "source of the extra pixels) and says so; until then every frame "
        "forwards as the game submitted it. Both keys are live.",
        modeName(mode), filterName(filter), static_cast<double>(width),
        filter == kSupersampleCrisp && width < 1.0f
            ? " (crisp runs no narrower than 1.0)"
            : "");
}

bool supersampleResolveWanted() {
    const State& s = g_s;
    return s.mode != Mode::Off && !s.standDown;
}

void supersampleResolveStandDown(const char* why) {
    State& s = g_s;
    if (s.standDown) return;
    s.standDown = true;
    Log::get().note(
        "supersample resolve STANDING DOWN: %s. Every frame forwards as the "
        "game submitted it for the rest of this session. Please report this "
        "log.",
        why ? why : "a submit-side failure");
}

void supersampleResolveFrameBoundary() {
    State& s = g_s;
    if (!supersampleResolveWanted()) return;

    uint32_t recW = 0, recH = 0;
    if (!systemHookRecommendedSize(&recW, &recH)) {
        // Nothing to compare against: the IVRSystem observation is off or
        // did not install, so the recommendation was never seen. Said
        // once, and the reports keep arriving in case it does.
        if (!s.noRecNoted && (s.armer.eyeSeen[0] || s.armer.eyeSeen[1])) {
            s.noRecNoted = true;
            Log::get().note(
                "supersample resolve: the runtime's recommended eye size is "
                "not known (the IVRSystem observation is off or did not "
                "install), so there is nothing to compare the submitted size "
                "against. Waiting.");
        }
        s.armer.boundary(0, 0);
        return;
    }

    // What both eyes said this frame, kept for the lines below before the
    // boundary clears it.
    const bool bothSeen = s.armer.eyeSeen[0] && s.armer.eyeSeen[1];
    const bool agree = bothSeen && s.armer.eyeW[0] == s.armer.eyeW[1] &&
                       s.armer.eyeH[0] == s.armer.eyeH[1];
    const uint32_t subW = agree ? s.armer.eyeW[0] : 0;
    const uint32_t subH = agree ? s.armer.eyeH[0] : 0;
    if (bothSeen && s.firstReportMs == 0) s.firstReportMs = stampMs();

    const SupersampleArmer::Event ev = s.armer.boundary(recW, recH);
    if (ev == SupersampleArmer::kArmed || ev == SupersampleArmer::kReadopted) {
        const double rx = static_cast<double>(s.armer.inW) / static_cast<double>(recW);
        const double ry = static_cast<double>(s.armer.inH) / static_cast<double>(recH);
        Log::get().note(
            "supersample resolve: %s -- each eye arrives at %ux%u against the "
            "runtime's recommended %ux%u (%.2fx horizontal, %.2fx vertical: "
            "about %.0f%% of the native pixel count rendered) and leaves at "
            "the recommended size, filtered down at submit with the %s "
            "kernel at radius %.2f px instead of by the compositor's own "
            "sampler, full bounds, both eyes. The extra pixels are the game's "
            "(Elite's HMD Quality above 1.0 is the usual source); only who "
            "filters them changed. The graphics log prints the pass's price "
            "and the submitted format once it has run a while.",
            ev == SupersampleArmer::kArmed ? "engaged" : "re-adopted",
            s.armer.inW, s.armer.inH, recW, recH, rx, ry, rx * ry * 100.0,
            filterName(s.filter), static_cast<double>(s.width));
        s.lastSubW = s.armer.inW;
        s.lastSubH = s.armer.inH;
        s.idleNoted = true;   // nothing idle about it any more
    } else if (ev == SupersampleArmer::kDisarmed) {
        Log::get().note(
            "supersample resolve: disengaged -- the submitted size moved from "
            "%ux%u to %ux%u against a recommended %ux%u. Frames forward as the "
            "game submits them until both eyes settle at a size larger than "
            "the recommendation again.",
            s.lastSubW, s.lastSubH, subW, subH, recW, recH);
    } else if (!s.armer.armed && s.mode == Mode::On && !s.idleNoted && agree &&
               !supersampleExceeds(recW, recH, subW, subH) &&
               elapsedMs(s.firstReportMs, kIdleNoteMs)) {
        // `on` promised to say so when it finds nothing to do; `auto`
        // stays quiet, which is the whole difference between them.
        s.idleNoted = true;
        Log::get().note(
            "supersample resolve: on, but each eye arrives at %ux%u, no "
            "larger than the runtime's recommended %ux%u -- nothing to "
            "resolve, so nothing runs. Elite's HMD Quality above 1.0 (its "
            "graphics options, in VR; not the Supersampling slider, which "
            "shrinks the image before the game's own post-processing) is "
            "what gives it something to do. Waiting.",
            subW, subH, recW, recH);
    }
}

void* supersampleResolveTreat(vr::EVREye eye, void* handle,
                              vr::EColorSpace colorSpace,
                              const vr::VRTextureBounds_t* bounds,
                              vr::VRTextureBounds_t* outBounds) {
    State& s = g_s;
    if (!supersampleResolveWanted() || !handle || !outBounds) return nullptr;
    const int e = eye == vr::Eye_Left ? 0 : 1;

    if (s.faults > kMaxFaults) {
        if (!s.faultsNoted) {
            s.faultsNoted = true;
            supersampleResolveStandDown(
                "reading the submitted texture handles faulted repeatedly");
        }
        return nullptr;
    }

    // The size this eye arrives at, from the texture about to go out and
    // the bounds naming its region -- the post-crop size when the guard
    // is live, the game's own otherwise: exactly what the pass would be
    // handed. One guarded GetDesc per submit while the mode is on; the
    // mode off costs the flag test above and nothing else.
    D3D11_TEXTURE2D_DESC desc{};
    bool ok = false;
    guarded("supersample/desc", [&] {
        static_cast<ID3D11Texture2D*>(handle)->GetDesc(&desc);
        ok = true;
    });
    if (!ok) {
        ++s.faults;
        return nullptr;
    }
    float b4[4] = {0.0f, 0.0f, 1.0f, 1.0f};
    bool haveBounds = false;
    if (bounds) {
        haveBounds = guarded("supersample/bounds", [&] {
            b4[0] = bounds->uMin;
            b4[1] = bounds->vMin;
            b4[2] = bounds->uMax;
            b4[3] = bounds->vMax;
        });
        if (!haveBounds) {
            ++s.faults;
            return nullptr;
        }
    }
    uint32_t region[4] = {};
    bool flipU = false, flipV = false;
    if (!supersampleRegionFromBounds(desc.Width, desc.Height,
                                     haveBounds ? b4 : nullptr, region, &flipU,
                                     &flipV)) {
        if (!s.regionNoted) {
            s.regionNoted = true;
            Log::get().note(
                "supersample resolve: the Submit bounds name no usable eye "
                "region of the %ux%u texture (u %.3f..%.3f, v %.3f..%.3f); "
                "this eye forwards untouched.",
                desc.Width, desc.Height, b4[0], b4[2], b4[1], b4[3]);
        }
        return nullptr;
    }
    const uint32_t regionW = region[2] - region[0];
    const uint32_t regionH = region[3] - region[1];
    // NOT WHILE THE CULL GUARD IS STAGING. Stage 1 asks the game for wider
    // targets and does not crop them yet, so for a couple of seconds every
    // submit arrives supersampled by the guard's own margin -- 5792x5356
    // against a recommended 5424x5356 on the first flight (Pimax, 7%
    // horizontal, 2026-09-02) -- and the resolve armed on exactly that.
    // The next frame the crop went live, delivered the recommended size,
    // and the pass refused a 1:1 resolve; the resolve stood down for the
    // session before a real supersampled frame ever reached it. Reports
    // made while the guard is staging are not evidence about the game's
    // own supersampling, so they are not fed to the armer at all. A
    // resolve already armed keeps treating through the stage: the game
    // renders TRUE projections into the wider stage-1 targets, so the
    // whole region is the true view at a higher density, and shrinking it
    // to the recommended size is exactly right.
    if (!systemHookSizeProbeWanted()) s.armer.note(e, regionW, regionH);

    if (!s.armer.armed) return nullptr;
    // A region that no longer exceeds the target by more than rounding --
    // the guard's crop landing at the recommended size, the runtime's
    // slider moving up, the game's quality moving down -- forwards
    // untouched for this frame, both eyes alike since both carry the same
    // size, and the boundary disarms. It is NOT a refusal: the same-size
    // case is exactly what stood the first flight down, and nothing about
    // it is wrong with the pass. The pass takes anything that exceeds.
    if (!supersampleExceeds(s.armer.outW, s.armer.outH, regionW, regionH)) {
        return nullptr;
    }

    if (!s.fn && !s.fnTried) {
        s.fnTried = true;
        HMODULE m = GetModuleHandleW(L"d3d11.dll");
        if (m) {
            s.fn = reinterpret_cast<PFN_EdvrSupersampleResolve>(
                GetProcAddress(m, "edvrSupersampleResolve"));
        }
        if (s.fn) {
            Log::get().note("supersample resolve: the d3d11 half is linked.");
        } else {
            supersampleResolveStandDown(
                "d3d11.dll exports no resolver (mismatched pair?)");
        }
    }
    if (!s.fn) return nullptr;

    void* out = s.fn(handle, e, haveBounds ? b4 : nullptr, s.armer.outW,
                     s.armer.outH, s.filter, s.width,
                     gammaHint(colorSpace, desc.Format));
    if (!out) {
        supersampleResolveStandDown(
            "the resolve pass refused (its own line in the graphics log "
            "says why)");
        return nullptr;
    }

    // Full-span bounds, preserving only the direction the game's own
    // bounds ran in -- guardCropCopy's rule: the pixels are the pixels, and
    // a flipped-origin submission stays a flipped-origin submission.
    outBounds->uMin = flipU ? 1.0f : 0.0f;
    outBounds->uMax = flipU ? 0.0f : 1.0f;
    outBounds->vMin = flipV ? 1.0f : 0.0f;
    outBounds->vMax = flipV ? 0.0f : 1.0f;
    ++s.treats;
    return out;
}

void supersampleResolveShutdown() {
    const State& s = g_s;
    if (s.treats == 0) return;
    Log::get().note(
        "supersample resolve: %u eye-submits resolved this session%s.",
        s.treats, s.standDown ? " (then stood down -- see the line above)" : "");
}

}  // namespace edvr
