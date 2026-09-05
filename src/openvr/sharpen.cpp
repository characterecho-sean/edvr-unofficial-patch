#include "sharpen.h"

#include <windows.h>

#include <cmath>
#include <cstdint>
#include <cstring>

#include "../common/config.h"
#include "../common/guard.h"
#include "../common/log.h"

namespace edvr {
namespace {

typedef void* (*PFN_EdvrSharpen)(void*, int, const float*, float);

// Same budget and same reasoning as the resolve's treat: the bounds are
// the game's memory, and a read that faults every frame must retire itself
// rather than bleed the log.
constexpr uint32_t kMaxFaults = 8;

struct State {
    float strength = 0.0f;
    bool  configured = false;

    bool  standDown = false;          // inert for the session; said once

    PFN_EdvrSharpen fn = nullptr;
    bool  fnTried = false;

    uint32_t faults = 0;
    bool     faultsNoted = false;
    bool     engagedNoted = false;
    uint32_t treats = 0;
};
State g_s;

double stopsOf(float strength) {
    return 2.0 * (1.0 - static_cast<double>(strength));
}

}  // namespace

void sharpenConfigure() {
    State& s = g_s;
    Config& cfg = Config::get();

    float v = cfg.getFloat("fix.render_sharpness", 0.0f);
    if (!std::isfinite(v) || v < 0.0f) v = 0.0f;
    if (v > 1.0f) v = 1.0f;

    const bool first = !s.configured;
    const bool changed = first || fabsf(v - s.strength) > 1e-4f;
    s.configured = true;
    s.strength = v;
    if (!changed) return;
    if (v <= 0.0f) {
        if (!first) {
            Log::get().note("render sharpening: off. Frames leave as the passes "
                            "before it made them from the next submit.");
        }
        return;
    }
    Log::get().note(
        "render sharpening: %.2f -- AMD's RCAS at %.2f stops on every "
        "outgoing eye frame, the last pass before it leaves for the "
        "compositor (after the terrain fix's crop and the supersample "
        "resolve, on whatever they produced). It engages at the next submit "
        "and says so once. Live.",
        static_cast<double>(v), stopsOf(v));
}

bool sharpenWanted() {
    const State& s = g_s;
    return s.strength > 0.0f && !s.standDown;
}

void sharpenStandDown(const char* why) {
    State& s = g_s;
    if (s.standDown) return;
    s.standDown = true;
    Log::get().note(
        "render sharpening STANDING DOWN: %s. Frames leave unsharpened for "
        "the rest of this session. Please report this log.",
        why ? why : "a submit-side failure");
}

void* sharpenTreat(vr::EVREye eye, void* handle,
                   const vr::VRTextureBounds_t* bounds,
                   vr::VRTextureBounds_t* outBounds) {
    State& s = g_s;
    if (!sharpenWanted() || !handle || !outBounds) return nullptr;
    const int e = eye == vr::Eye_Left ? 0 : 1;

    if (s.faults > kMaxFaults) {
        if (!s.faultsNoted) {
            s.faultsNoted = true;
            sharpenStandDown("reading the submitted bounds faulted repeatedly");
        }
        return nullptr;
    }

    // Only the bounds are read here; the pass reads the texture's own
    // description itself, so a size never has to be known on this side.
    float b4[4] = {0.0f, 0.0f, 1.0f, 1.0f};
    bool haveBounds = false;
    if (bounds) {
        haveBounds = guarded("sharpen/bounds", [&] {
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

    if (!s.fn && !s.fnTried) {
        s.fnTried = true;
        HMODULE m = GetModuleHandleW(L"d3d11.dll");
        if (m) {
            s.fn = reinterpret_cast<PFN_EdvrSharpen>(
                GetProcAddress(m, "edvrSharpen"));
        }
        if (s.fn) {
            Log::get().note("render sharpening: the d3d11 half is linked.");
        } else {
            sharpenStandDown("d3d11.dll exports no sharpener (mismatched pair?)");
        }
    }
    if (!s.fn) return nullptr;

    void* out = s.fn(handle, e, haveBounds ? b4 : nullptr, s.strength);
    if (!out) {
        sharpenStandDown("the sharpen pass refused (its own line in the "
                         "graphics log says why)");
        return nullptr;
    }

    // Full-span bounds, preserving only the direction the incoming bounds
    // ran in -- guardCropCopy's rule: the pixels are the pixels, and a
    // flipped-origin submission stays a flipped-origin submission.
    const bool flipU = haveBounds && b4[0] > b4[2];
    const bool flipV = haveBounds && b4[1] > b4[3];
    outBounds->uMin = flipU ? 1.0f : 0.0f;
    outBounds->uMax = flipU ? 0.0f : 1.0f;
    outBounds->vMin = flipV ? 1.0f : 0.0f;
    outBounds->vMax = flipV ? 0.0f : 1.0f;
    ++s.treats;
    if (!s.engagedNoted) {
        s.engagedNoted = true;
        Log::get().note(
            "render sharpening: engaged -- every outgoing eye frame is "
            "sharpened at %.2f (AMD's RCAS, %.2f stops) as the last pass "
            "before it leaves. The graphics log prints the price and the "
            "format once it has run a while.",
            static_cast<double>(s.strength), stopsOf(s.strength));
    }
    return out;
}

void sharpenShutdown() {
    const State& s = g_s;
    if (s.treats == 0) return;
    Log::get().note(
        "render sharpening: %u eye-submits sharpened this session%s.",
        s.treats, s.standDown ? " (then stood down -- see the line above)" : "");
}

}  // namespace edvr
