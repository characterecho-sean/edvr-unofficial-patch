#include "temporal_aa.h"

#include <windows.h>
#include <d3d11.h>   // GetDesc on the texture about to go out, for its size only

#include <cmath>
#include <cstring>
#include <string>

#include "../common/config.h"
#include "../common/guard.h"
#include "../common/log.h"
#include "../common/supersample_math.h"   // the eye region from Submit bounds
#include "../common/temporal_math.h"
#include "system_hook.h"

namespace edvr {
namespace {

typedef void* (*PFN_EdvrTemporalAa)(void*, int, const float*, const float*,
                                    const float*, const float*, int, float,
                                    float, unsigned);

constexpr uint32_t kMaxFaults = 8;

enum class Motion : uint8_t { None = 0, Head = 1, Camera = 2 };

struct State {
    bool   on = false;
    bool   jitterWanted = true;
    float  blend = 0.90f;
    float  clamp = 1.0f;
    Motion motion = Motion::Head;
    bool   configured = false;
    bool   standDown = false;

    PFN_EdvrTemporalAa fn = nullptr;
    bool fnTried = false;

    // The jitter: the frame counter it is drawn from, this frame's offset
    // in render pixels, and whether the system hook could apply it.
    uint32_t frame = 0;
    bool     jitterLiveNoted = false;
    bool     jitterLive = false;
    bool     jitterUnavailableNoted = false;
    uint32_t renderW = 0, renderH = 0;   // the last treated region, for the pixel size

    // The head pose: the one noted since the last boundary (the frame
    // about to render), this frame's, last frame's.
    float pendingPose[12] = {};
    bool  pendingValid = false;
    float curPose[12] = {};
    bool  curValid = false;
    float prevPose[12] = {};
    bool  prevValid = false;

    // Per eye: what the history holds (its frustum), and whether the next
    // treat must start afresh.
    float    tanPrev[2][4] = {};
    bool     havePrev[2] = {};
    bool     resetNext[2] = {true, true};

    uint32_t faults = 0;
    bool     faultsNoted = false;
    bool     engagedNoted = false;
    uint32_t treats = 0;
};
State g_s;

const char* motionName(Motion m) {
    return m == Motion::Camera ? "camera" : m == Motion::Head ? "head" : "none";
}

}  // namespace

void temporalAaConfigure() {
    State& s = g_s;
    Config& cfg = Config::get();

    const std::string raw = cfg.getString("fix.temporal_aa", "off");
    bool on = false;
    if (_stricmp(raw.c_str(), "on") == 0) on = true;
    else if (_stricmp(raw.c_str(), "off") != 0 && !raw.empty()) {
        Log::get().note("temporal_aa = \"%s\" is not a mode this build knows "
                        "(off, on). Treating it as off.", raw.c_str());
    }
    const std::string rawJit = cfg.getString("fix.temporal_aa_jitter", "on");
    const bool jitter = _stricmp(rawJit.c_str(), "off") != 0;
    float blend = cfg.getFloat("fix.temporal_aa_blend", 0.90f);
    if (!std::isfinite(blend)) blend = 0.90f;
    if (blend < 0.5f) blend = 0.5f;
    if (blend > 0.95f) blend = 0.95f;
    float clampSig = cfg.getFloat("fix.temporal_aa_clamp", 1.0f);
    if (!std::isfinite(clampSig)) clampSig = 1.0f;
    if (clampSig < 0.5f) clampSig = 0.5f;
    if (clampSig > 3.0f) clampSig = 3.0f;
    const std::string rawMotion = cfg.getString("advanced.temporal_aa_motion", "head");
    Motion motion = Motion::Head;
    if (_stricmp(rawMotion.c_str(), "camera") == 0) motion = Motion::Camera;
    else if (_stricmp(rawMotion.c_str(), "none") == 0) motion = Motion::None;
    else if (_stricmp(rawMotion.c_str(), "head") != 0 && !rawMotion.empty()) {
        Log::get().note("temporal_aa_motion = \"%s\" is not a source this build "
                        "knows (head, camera, none). Using head.",
                        rawMotion.c_str());
    }

    const bool first = !s.configured;
    const bool changed = first || on != s.on || jitter != s.jitterWanted ||
                         fabsf(blend - s.blend) > 1e-4f ||
                         fabsf(clampSig - s.clamp) > 1e-4f || motion != s.motion;
    s.configured = true;
    s.on = on;
    s.jitterWanted = jitter;
    s.blend = blend;
    s.clamp = clampSig;
    s.motion = motion;
    if (!changed) return;
    if (!on) {
        if (!first) {
            Log::get().note("temporal aa: off. Every frame forwards as the game "
                            "submitted it from the next boundary, and the "
                            "projection is no longer jittered.");
        }
        s.resetNext[0] = s.resetNext[1] = true;
        return;
    }
    // A motion or blend change mid-flight need not reset the history; a
    // freshly-on pass starts from nothing anyway.
    Log::get().note(
        "temporal aa: on -- motion from the %s, jitter %s, history weight "
        "%.2f, clip %.2f sigma. Engages at the first forwarded frame and "
        "says so; every key is live.",
        motionName(motion), jitter ? "on" : "off", static_cast<double>(blend),
        static_cast<double>(clampSig));
}

bool temporalAaWanted() {
    const State& s = g_s;
    return s.on && !s.standDown;
}

void temporalAaStandDown(const char* why) {
    State& s = g_s;
    if (s.standDown) return;
    s.standDown = true;
    systemHookSetJitter(0.0f, 0.0f, false);
    Log::get().note(
        "temporal aa STANDING DOWN: %s. Every frame forwards as the game "
        "submitted it for the rest of this session, and the projection is "
        "no longer jittered. Please report this log.",
        why ? why : "a submit-side failure");
}

void temporalAaNotePose(const vr::HmdMatrix34_t& pose, bool valid) {
    State& s = g_s;
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 4; ++c) s.pendingPose[r * 4 + c] = pose.m[r][c];
    }
    s.pendingValid = valid;
}

void temporalAaFrameBoundary() {
    State& s = g_s;
    // The pose pair rolls whether or not the pass is on, so a live enable
    // has a delta on its first frame.
    memcpy(s.prevPose, s.curPose, sizeof(s.prevPose));
    s.prevValid = s.curValid;
    memcpy(s.curPose, s.pendingPose, sizeof(s.curPose));
    s.curValid = s.pendingValid;

    if (!temporalAaWanted()) {
        if (s.jitterLive) {
            s.jitterLive = false;
            systemHookSetJitter(0.0f, 0.0f, false);
        }
        return;
    }
    ++s.frame;

    // The jitter for the coming frame, as a shift of the tangents the
    // game will be told. It needs the matrix half of the projection edit
    // (the guard's receiver, installed at launch when either feature was
    // on) or the raw tangents and the matrix would disagree by the shift
    // -- worse than no jitter.
    bool live = false;
    if (s.jitterWanted) {
        // Asked every boundary, because the answer is not known at the
        // first one: the receiver checks the runtime's matrix against the
        // tangent formula on the game's FIRST GetProjectionMatrix per eye,
        // which lands a frame or two after this pass configures. The
        // first flight asked once, was told "not yet", printed that the
        // session could not be jittered, and then jittered from the next
        // frame on (Quest 3, 2026-09-03) -- so the line waits for a
        // verdict, and says which one it got.
        const int verdict = systemHookJitterVerdict();
        if (verdict > 0) {
            float jx = 0.0f, jy = 0.0f;
            temporalJitter(s.frame, &jx, &jy);
            float tan[4];
            uint32_t w = s.renderW, h = s.renderH;
            if (!w || !h) systemHookRecommendedSize(&w, &h);
            if (systemHookEffectiveTangents(vr::Eye_Left, tan) && w && h) {
                float dx = 0.0f, dy = 0.0f;
                temporalJitterToTangents(jx, jy, tan, w, h, &dx, &dy);
                systemHookSetJitter(dx, dy, true);
                live = true;
                if (!s.jitterLiveNoted) {
                    s.jitterLiveNoted = true;
                    Log::get().note(
                        "temporal aa: the jitter is live -- from this frame "
                        "on, the projection the game is told moves by a "
                        "sub-pixel Halton (2,3) offset every frame (one of "
                        "eight, %ux%u render pixels), in the raw tangents "
                        "and the matrix alike. temporal_aa_jitter = off "
                        "holds it still, live.",
                        w, h);
                }
            }
        } else if (verdict < 0 && !s.jitterUnavailableNoted) {
            s.jitterUnavailableNoted = true;
            Log::get().note(
                "temporal aa: the projection cannot be jittered this session "
                "-- the matrix half of the projection edit is not in place "
                "(temporal_aa was off at launch and the terrain fix is off, "
                "or the runtime's matrix failed the tangent-formula check; "
                "the cull guard's lines above say which). The pass runs as a "
                "temporal smoother: real integration while the head moves, "
                "nothing new while it is held still. Restart with "
                "temporal_aa = on for the full effect.");
        }
        // verdict 0: the receiver is in and the game has not asked for both
        // eyes' matrices yet. Quiet; next boundary.
    }
    if (!live && s.jitterLive) systemHookSetJitter(0.0f, 0.0f, false);
    s.jitterLive = live;
}

void* temporalAaTreat(vr::EVREye eye, void* handle,
                      const vr::VRTextureBounds_t* bounds,
                      vr::VRTextureBounds_t* outBounds) {
    State& s = g_s;
    if (!temporalAaWanted() || !handle || !outBounds) return nullptr;
    const int e = eye == vr::Eye_Left ? 0 : 1;

    if (s.faults > kMaxFaults) {
        if (!s.faultsNoted) {
            s.faultsNoted = true;
            temporalAaStandDown("reading the submitted texture handles faulted "
                                "repeatedly");
        }
        return nullptr;
    }

    D3D11_TEXTURE2D_DESC desc{};
    bool ok = false;
    guarded("temporal/desc", [&] {
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
        haveBounds = guarded("temporal/bounds", [&] {
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
        return nullptr;
    }
    if (e == 0) {
        s.renderW = region[2] - region[0];
        s.renderH = region[3] - region[1];
    }

    // The frustum this frame rendered through, jitter excluded: the lie
    // under the guard, the truth otherwise.
    float tanNow[4];
    if (!systemHookEffectiveTangents(eye, tanNow)) return nullptr;

    if (!s.fn && !s.fnTried) {
        s.fnTried = true;
        HMODULE m = GetModuleHandleW(L"d3d11.dll");
        if (m) {
            s.fn = reinterpret_cast<PFN_EdvrTemporalAa>(
                GetProcAddress(m, "edvrTemporalAa"));
        }
        if (s.fn) {
            Log::get().note("temporal aa: the d3d11 half is linked.");
        } else {
            temporalAaStandDown("d3d11.dll exports no temporal pass (mismatched "
                                "pair?)");
        }
    }
    if (!s.fn) return nullptr;

    // The head's rotation between the frame the history holds and this
    // one. Without a valid pair the pass is told so and restarts.
    float delta[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    bool haveDelta = false;
    if (s.motion == Motion::Head && s.curValid && s.prevValid) {
        temporalHeadDelta(s.prevPose, s.curPose, delta);
        haveDelta = true;
    }
    unsigned flags = 0;
    if (s.resetNext[e] || !s.havePrev[e] ||
        (s.motion == Motion::Head && !haveDelta)) {
        flags |= 1u;
    }
    void* out = s.fn(handle, e, haveBounds ? b4 : nullptr, tanNow,
                     s.havePrev[e] ? s.tanPrev[e] : tanNow,
                     haveDelta ? delta : nullptr, static_cast<int>(s.motion),
                     s.blend, s.clamp, flags);
    if (!out) {
        temporalAaStandDown("the temporal pass refused (its own line in the "
                            "graphics log says why)");
        return nullptr;
    }
    memcpy(s.tanPrev[e], tanNow, sizeof(tanNow));
    s.havePrev[e] = true;
    s.resetNext[e] = false;
    ++s.treats;
    if (!s.engagedNoted) {
        s.engagedNoted = true;
        Log::get().note(
            "temporal aa: engaged -- each eye's %ux%u frame is blended with "
            "its reprojected history at submit, before the guard's crop and "
            "the supersample resolve; motion from the %s, jitter %s, history "
            "weight %.2f, clip %.2f sigma. The graphics log prints the "
            "price, the format and the history's acceptance once it has run "
            "a while.",
            region[2] - region[0], region[3] - region[1], motionName(s.motion),
            s.jitterWanted ? (s.jitterLive ? "on (Halton 2,3 over 8 frames)"
                                           : "on, waiting for the projection "
                                             "edit's verdict")
                           : "off",
            static_cast<double>(s.blend), static_cast<double>(s.clamp));
    }
    outBounds->uMin = flipU ? 1.0f : 0.0f;
    outBounds->uMax = flipU ? 0.0f : 1.0f;
    outBounds->vMin = flipV ? 1.0f : 0.0f;
    outBounds->vMax = flipV ? 0.0f : 1.0f;
    return out;
}

void temporalAaNoteWithheld(vr::EVREye eye) {
    State& s = g_s;
    s.resetNext[eye == vr::Eye_Left ? 0 : 1] = true;
}

void temporalAaShutdown() {
    const State& s = g_s;
    if (s.treats == 0) return;
    Log::get().note("temporal aa: %u eye-submits treated this session%s.",
                    s.treats,
                    s.standDown ? " (then stood down -- see the line above)" : "");
}

}  // namespace edvr
