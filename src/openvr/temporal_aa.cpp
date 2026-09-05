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
                                    const float*, float, float, const float*,
                                    const float*, const float*, float, float,
                                    float, int, float, float, unsigned, unsigned,
                                    unsigned);

constexpr uint32_t kMaxFaults = 8;

// 2 was "camera", the rows alone read without the z flip: wrong since the
// flip was measured, retired 2026-09-04.
enum class Motion : uint8_t { None = 0, Head = 1, Depth = 3 };

typedef void (*PFN_EdvrTemporalAaNoteHead)(int, const float*, const float*, const float*);

struct State {
    bool   on = false;
    bool   dlaa = false;   // experimental.temporal_aa = dlaa | dlss: NVIDIA's history instead of the pass's own
    bool   upscale = false; // ...and dlss: a frame smaller than the unit-quality size comes back at it
    bool   jitterWanted = true;
    float  blend = 0.90f;
    float  clamp = 1.0f;
    Motion motion = Motion::Depth;
    bool   configured = false;
    bool   standDown = false;

    PFN_EdvrTemporalAa fn = nullptr;
    PFN_EdvrTemporalAaNoteHead fnHead = nullptr;   // the head note, for the world path
    bool cantNoted = false;                          // the eyes' cant, logged once
    bool fnTried = false;

    // The jitter: the frame counter it is drawn from, this frame's offset
    // in render pixels, and whether the system hook could apply it.
    uint32_t frame = 0;
    bool     jitterLiveNoted = false;
    float    jx = 0.0f, jy = 0.0f;   // this frame's offset in render pixels, 0 when not live
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
    float prev2Pose[12] = {};   // the frame before that: the lagged candidate
    bool  prev2Valid = false;
    // The game-pose array's HMD pose, the same three deep: the candidate
    // for a renderer that draws with those (the instrument only).
    float pendingGame[12] = {};
    bool  pendingGameValid = false;
    float curGame[12] = {};
    bool  curGameValid = false;
    float prevGame[12] = {};
    bool  prevGameValid = false;
    bool  noGamePosesNoted = false;

    // Per eye: what the history holds (its frustum), and whether the next
    // treat must start afresh.
    float    tanPrev[2][4] = {};
    bool     havePrev[2] = {};
    bool     resetNext[2] = {true, true};

    uint32_t faults = 0;
    bool     faultsNoted = false;
    bool     engagedNoted = false;
    uint32_t treats = 0;

    // The projection-read order (the review of 2026-09-04, F4). The jitter
    // set at the boundary is the jitter the game rendered with only if the
    // game reads its projection AFTER the boundary and before it submits;
    // the cull guard's constant lie could never test that, a per-frame
    // offset depends on it. Counted over the first frames and said once.
    uint32_t readsAtBoundary = 0;
    uint32_t readsAtLastTreat = 0;
    bool     readsAtLastTreatValid = false;
    bool     firstTreatOfFrame = false;
    uint64_t readsBeforeSum = 0;   // boundary -> the frame's first treat
    uint64_t readsAfterSum = 0;    // the frame's last treat -> the next boundary
    uint32_t readOrderFrames = 0;
    bool     readOrderNoted = false;

    // Two A/B instruments for the jitter handed to the consumers (the
    // pass's filter and NGX), never to the projection the game renders
    // through: a sign flip per axis, and a one-frame lag. They exist
    // because the review of 2026-09-04 left the game's displacement
    // direction (F5) and the read phase (F4) unmeasured in the field;
    // the shipped answers are no flip and no lag.
    int   jitterSignX = 1;
    int   jitterSignY = 1;
    bool  jitterLag = false;
    float jxPrev = 0.0f, jyPrev = 0.0f;   // last frame's offset, for the lag
};
State g_s;
constexpr uint32_t kReadOrderFrames = 600;

const char* motionName(Motion m) {
    return m == Motion::Depth ? "head with depth" : m == Motion::Head ? "head" : "none";
}

// The mode as the log should say it. The reload line used to say "on" for
// every mode that was not off, so a log could not tell whose history had
// run -- and a session spent comparing two headsets read as the pass's own
// when NVIDIA's was doing the work (2026-09-04).
const char* modeName(bool dlaa, bool upscale) {
    if (!dlaa) return "on (the pass's own history)";
    return upscale ? "dlss (NVIDIA's history, upscaling to the unit-quality size)"
                   : "dlaa (NVIDIA's history at the frame's own size)";
}

// Pixels per degree, across and down, from the frustum the game renders
// with and the size it submits. This is the number a comparison between
// two headsets has to hold level: aliasing is set by how densely the
// frame samples the world, not by which headset is on the head, and a
// Quest 3 at three quarters quality samples about half as densely as a
// Pimax Crystal Super at the same nominal settings.
void samplingDensity(const float tan[4], uint32_t w, uint32_t h,
                     float* degH, float* degV, float* pxH, float* pxV) {
    const float k = 180.0f / 3.14159265f;
    *degH = (atanf(-tan[0]) + atanf(tan[1])) * k;
    *degV = (atanf(-tan[2]) + atanf(tan[3])) * k;
    *pxH = *degH > 0.0f ? static_cast<float>(w) / *degH : 0.0f;
    *pxV = *degV > 0.0f ? static_cast<float>(h) / *degV : 0.0f;
}

}  // namespace

void temporalAaConfigure() {
    State& s = g_s;
    Config& cfg = Config::get();

    const std::string raw = cfg.getString("experimental.temporal_aa", "off");
    bool on = false;
    bool dlaa = false;
    bool upscale = false;
    if (_stricmp(raw.c_str(), "on") == 0) on = true;
    else if (_stricmp(raw.c_str(), "dlaa") == 0) { on = true; dlaa = true; }
    else if (_stricmp(raw.c_str(), "dlss") == 0) { on = true; dlaa = true; upscale = true; }
    else if (_stricmp(raw.c_str(), "off") != 0 && !raw.empty()) {
        Log::get().note("temporal_aa = \"%s\" is not a mode this build knows "
                        "(off, on, dlaa, dlss). Treating it as off.", raw.c_str());
    }
    const std::string rawJit = cfg.getString("experimental.temporal_aa_jitter", "on");
    const bool jitter = _stricmp(rawJit.c_str(), "off") != 0;
    float blend = cfg.getFloat("experimental.temporal_aa_blend", 0.90f);
    if (!std::isfinite(blend)) blend = 0.90f;
    if (blend < 0.5f) blend = 0.5f;
    if (blend > 0.95f) blend = 0.95f;
    float clampSig = cfg.getFloat("experimental.temporal_aa_clamp", 1.0f);
    if (!std::isfinite(clampSig)) clampSig = 1.0f;
    if (clampSig < 0.5f) clampSig = 0.5f;
    if (clampSig > 3.0f) clampSig = 3.0f;
    const std::string rawMotion = cfg.getString("advanced.temporal_aa_motion", "depth");
    Motion motion = Motion::Depth;
    // "head" was never assigned here while depth was the default, so the
    // key silently ran depth (found 2026-09-04 in the cleanup pass).
    if (_stricmp(rawMotion.c_str(), "none") == 0) motion = Motion::None;
    else if (_stricmp(rawMotion.c_str(), "depth") == 0) motion = Motion::Depth;
    else if (_stricmp(rawMotion.c_str(), "head") == 0) motion = Motion::Head;
    else if (!rawMotion.empty()) {
        Log::get().note("temporal_aa_motion = \"%s\" is not a source this build "
                        "knows (depth, head, none; \"camera\" was retired 2026-09-04). "
                        "Using depth.",
                        rawMotion.c_str());
    }
    const std::string rawSign = cfg.getString("advanced.temporal_aa_jitter_sign", "as_is");
    int signX = 1, signY = 1;
    if (_stricmp(rawSign.c_str(), "flip_x") == 0) signX = -1;
    else if (_stricmp(rawSign.c_str(), "flip_y") == 0) signY = -1;
    else if (_stricmp(rawSign.c_str(), "flip_both") == 0) { signX = -1; signY = -1; }
    const bool lag = cfg.getFloat("advanced.temporal_aa_jitter_lag", 0.0f) >= 0.5f;
    const bool instrumentsOn = signX < 0 || signY < 0 || lag;
    if ((s.configured && (signX != s.jitterSignX || signY != s.jitterSignY || lag != s.jitterLag)) ||
        (!s.configured && instrumentsOn)) {
        Log::get().note(
            "temporal aa: the jitter handed to the pass and to NVIDIA is now %s%s "
            "(an A/B instrument; the projection the game renders through is unchanged).",
            signX < 0 && signY < 0 ? "flipped on both axes"
            : signX < 0 ? "flipped on x" : signY < 0 ? "flipped on y" : "as computed",
            lag ? ", one frame late" : "");
    }
    s.jitterSignX = signX;
    s.jitterSignY = signY;
    s.jitterLag = lag;

    const bool first = !s.configured;
    const bool changed = first || on != s.on || dlaa != s.dlaa || upscale != s.upscale ||
                         jitter != s.jitterWanted ||
                         fabsf(blend - s.blend) > 1e-4f ||
                         fabsf(clampSig - s.clamp) > 1e-4f || motion != s.motion;
    s.configured = true;
    s.on = on;
    s.dlaa = dlaa;
    s.upscale = upscale;
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
        "temporal aa: %s -- motion from the %s, jitter %s, history weight "
        "%.2f, clip %.2f sigma. Engages at the first forwarded frame and "
        "says so; every key is live.",
        modeName(dlaa, upscale), motionName(motion), jitter ? "on" : "off",
        static_cast<double>(blend), static_cast<double>(clampSig));
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

void temporalAaNoteGamePose(const vr::HmdMatrix34_t& pose, bool valid) {
    State& s = g_s;
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 4; ++c) s.pendingGame[r * 4 + c] = pose.m[r][c];
    }
    s.pendingGameValid = valid;
}

void temporalAaFrameBoundary() {
    State& s = g_s;
    // The pose pair rolls whether or not the pass is on, so a live enable
    // has a delta on its first frame.
    memcpy(s.prev2Pose, s.prevPose, sizeof(s.prev2Pose));
    s.prev2Valid = s.prevValid;
    memcpy(s.prevPose, s.curPose, sizeof(s.prevPose));
    s.prevValid = s.curValid;
    memcpy(s.curPose, s.pendingPose, sizeof(s.curPose));
    s.curValid = s.pendingValid;
    memcpy(s.prevGame, s.curGame, sizeof(s.prevGame));
    s.prevGameValid = s.curGameValid;
    memcpy(s.curGame, s.pendingGame, sizeof(s.curGame));
    s.curGameValid = s.pendingGameValid;
    if (s.on && s.curValid && !s.curGameValid && !s.noGamePosesNoted) {
        s.noGamePosesNoted = true;
        Log::get().note(
            "temporal aa: the game asks WaitGetPoses for no game poses -- "
            "the render pose is the only pose it has, so the registration "
            "instrument's game-pose candidate stays empty and the head's "
            "delta is the one to build on (measured 2026-09-03).");
    }

    if (!temporalAaWanted()) {
        if (s.jitterLive) {
            s.jitterLive = false;
            systemHookSetJitter(0.0f, 0.0f, false);
        }
        return;
    }
    ++s.frame;
    s.jxPrev = s.jx;
    s.jyPrev = s.jy;

    // The projection-read order: the reads that landed between the last
    // treat and this boundary, and the count armed for the coming frame.
    {
        const uint32_t reads = systemHookProjectionReads();
        if (s.readsAtLastTreatValid && s.readOrderFrames < kReadOrderFrames) {
            s.readsAfterSum += reads - s.readsAtLastTreat;
        }
        s.readsAtBoundary = reads;
        s.firstTreatOfFrame = true;
        if (!s.readOrderNoted && s.readOrderFrames >= kReadOrderFrames) {
            s.readOrderNoted = true;
            Log::get().note(
                "temporal aa: over %u frames the game asked for its projection %.1f "
                "times between the boundary and its first submit, and %.1f times "
                "between its last submit and the next boundary. The jitter is set at "
                "the boundary, so the first number should be the larger; if it is "
                "not, the frame was rendered with the previous frame's offset and the "
                "pass's jitter is a frame late (the review of 2026-09-04, F4).",
                kReadOrderFrames,
                static_cast<double>(s.readsBeforeSum) / kReadOrderFrames,
                static_cast<double>(s.readsAfterSum) / kReadOrderFrames);
        }
    }

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
            s.jx = jx;
            s.jy = jy;
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
    if (!live) {
        s.jx = s.jy = 0.0f;
        if (s.jitterLive) systemHookSetJitter(0.0f, 0.0f, false);
    }
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
            s.fnHead = reinterpret_cast<PFN_EdvrTemporalAaNoteHead>(
                GetProcAddress(m, "edvrTemporalAaNoteHead"));
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
    // one, computed whenever a pair exists (the instrument wants it under
    // every motion source); the pass uses it only when the head is the
    // source, and without a valid pair is told so and restarts. The
    // lagged twin -- the same delta one frame earlier -- and the turn's
    // size go with it, for the registration instrument alone.
    float delta[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    float deltaLag[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    const bool haveHeadDelta = s.curValid && s.prevValid;
    float headDeg = 0.0f;
    if (haveHeadDelta) {
        temporalHeadDelta(s.prevPose, s.curPose, delta);
        headDeg = temporalRotationAngleDeg(delta);
    }
    // The translation term per eye, for the depth reprojection and for
    // the instrument's check of the eye assignment (the other eye's
    // offset): the runtime's eye-to-head offsets, asked once, and the
    // game's planes, seen at its first matrix call.
    float tv[3] = {0, 0, 0}, tvSwapped[3] = {0, 0, 0};
    bool haveTv = false;
    float nearZ = 0.0f, farZ = 0.0f;
    float eyeOffOut[3] = {0.0f, 0.0f, 0.0f};
    bool haveEyeOff = false;
    if (haveHeadDelta) {
        float e2h[2][12];
        const bool okL = systemHookEyeToHead(vr::Eye_Left, e2h[0]);
        const bool okR = systemHookEyeToHead(vr::Eye_Right, e2h[1]);
        if (okL && okR) {
            const float offThis[3] = {e2h[e][3], e2h[e][7], e2h[e][11]};
            memcpy(eyeOffOut, offThis, sizeof(eyeOffOut));
            haveEyeOff = true;
            const float offOther[3] = {e2h[1 - e][3], e2h[1 - e][7], e2h[1 - e][11]};
            temporalHeadTranslation(s.prevPose, s.curPose, offThis, tv);
            temporalHeadTranslation(s.prevPose, s.curPose, offOther, tvSwapped);
            haveTv = true;
            // The eye's cant. The shader applies the delta to directions in
            // the EYE's frame, which on canted panels is the head's frame
            // turned by GetEyeToHeadTransform's rotation Re: the delta there
            // is Re^T delta Re and the translation term Re^T tv (a pitch of
            // the head is not a pitch of a canted eye). Parallel panels make
            // Re the identity and this a no-op; the angles are logged once
            // so the log says which headset this is (2026-09-04).
            float Re[9], ReT[9], tmp[9], t2[3];
            temporalRot3Of34(e2h[e], Re);
            temporalTranspose3(Re, ReT);
            temporalMul3(ReT, delta, tmp);
            temporalMul3(tmp, Re, delta);
            temporalApply3(ReT, tv, t2);
            memcpy(tv, t2, sizeof(tv));
            temporalApply3(ReT, tvSwapped, t2);
            memcpy(tvSwapped, t2, sizeof(tvSwapped));
            if (!s.cantNoted) {
                s.cantNoted = true;
                float RL[9], RR[9];
                temporalRot3Of34(e2h[0], RL);
                temporalRot3Of34(e2h[1], RR);
                Log::get().note("temporal aa: the eyes sit %.2f (left) and %.2f (right) degrees from "
                                "the head's frame -- the cant, zero on parallel panels; the head's delta "
                                "is composed into each eye's frame.",
                                static_cast<double>(temporalRotationAngleDeg(RL)),
                                static_cast<double>(temporalRotationAngleDeg(RR)));
            }
        }
        if (!systemHookNearFar(&nearZ, &farZ)) nearZ = farZ = 0.0f;
    }
    (void)deltaLag;
    const bool haveDelta =
        (s.motion == Motion::Head || s.motion == Motion::Depth) && haveHeadDelta;
    unsigned flags = 0;
    if (s.resetNext[e] || !s.havePrev[e] ||
        ((s.motion == Motion::Head || s.motion == Motion::Depth) && !haveDelta)) {
        flags |= 1u;
    }
    // Bit 1: NVIDIA's history instead of the pass's own, when the d3d11
    // half has the runtime; it says so once either way and falls back.
    if (s.dlaa) flags |= 2u;
    // The size to come back at: under dlss, the unit-quality size when the
    // frame is smaller than it on both axes (Elite's HMD Quality below
    // 1.0 -- the frame is a fraction of that size, and NVIDIA's upscaler
    // brings it back, which is the game's render cost falling with the
    // square of the quality). Zero means the frame's own size.
    unsigned outW = 0, outH = 0;
    if (s.upscale) {
        uint32_t uw = 0, uh = 0;
        if (systemHookUnitQualitySize(&uw, &uh) && uw && uh &&
            (region[2] - region[0]) * 50 < uw * 49 && (region[3] - region[1]) * 50 < uh * 49) {
            outW = uw;
            outH = uh;
        }
    }
    // The jitter handed to the consumers: this frame's, or last frame's
    // under the lag instrument, with the sign instrument's flips.
    const float jxOut = static_cast<float>(s.jitterSignX) * (s.jitterLag ? s.jxPrev : s.jx);
    const float jyOut = static_cast<float>(s.jitterSignY) * (s.jitterLag ? s.jyPrev : s.jy);
    // The headset's two poses (the ones the game rendered from, held or
    // not) and this eye's offset, for the world path's composition with
    // the ship's camera rows (temporal_pass.h). A d3d11 half without the
    // entry simply never gets them and keeps the world path off.
    if (s.fnHead) {
        if (haveHeadDelta && haveEyeOff) s.fnHead(e, s.prevPose, s.curPose, eyeOffOut);
        else s.fnHead(e, nullptr, nullptr, nullptr);
    }
    void* out = s.fn(handle, e, haveBounds ? b4 : nullptr, tanNow,
                     s.havePrev[e] ? s.tanPrev[e] : tanNow, jxOut, jyOut,
                     haveHeadDelta ? delta : nullptr, haveTv ? tv : nullptr,
                     haveTv ? tvSwapped : nullptr, nearZ, farZ, headDeg,
                     static_cast<int>(s.motion), s.blend, s.clamp, outW, outH,
                     flags);
    if (!out) {
        temporalAaStandDown("the temporal pass refused (its own line in the "
                            "graphics log says why)");
        return nullptr;
    }
    memcpy(s.tanPrev[e], tanNow, sizeof(tanNow));
    s.havePrev[e] = true;
    s.resetNext[e] = false;
    ++s.treats;
    {
        const uint32_t reads = systemHookProjectionReads();
        if (s.firstTreatOfFrame) {
            s.firstTreatOfFrame = false;
            if (s.readOrderFrames < kReadOrderFrames) {
                s.readsBeforeSum += reads - s.readsAtBoundary;
                ++s.readOrderFrames;
            }
        }
        s.readsAtLastTreat = reads;
        s.readsAtLastTreatValid = true;
    }
    if (!s.engagedNoted) {
        s.engagedNoted = true;
        float degH = 0.0f, degV = 0.0f, pxH = 0.0f, pxV = 0.0f;
        samplingDensity(tanNow, region[2] - region[0], region[3] - region[1],
                        &degH, &degV, &pxH, &pxV);
        Log::get().note(
            "temporal aa: the frame samples %.1f pixels per degree across and "
            "%.1f down (%ux%u over %.0f x %.0f degrees). Aliasing follows this "
            "number, so two headsets are only comparable when it matches: a "
            "headset at half the density shimmers more with every setting "
            "identical.",
            static_cast<double>(pxH), static_cast<double>(pxV),
            region[2] - region[0], region[3] - region[1],
            static_cast<double>(degH), static_cast<double>(degV));
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
