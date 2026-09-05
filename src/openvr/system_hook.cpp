#include "system_hook.h"

#include <windows.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>

#include "../common/config.h"
#include "../common/frame_flag.h"
#include "../common/guard.h"
#include "../common/log.h"
#include "../common/timing.h"
#include "../common/vtable_hook.h"
#include "guard_crop.h"
#include "openvr_min.h"

// ---------------------------------------------------------------------------
// What this file is.
//
// Frontier issue 72609: planet terrain tiles cull too early at the FOV edges
// in VR. Phase 0 (observation, 2026-08-18) established from both field rigs:
// Elite queries GetRecommendedRenderTargetSize, GetProjectionMatrix,
// GetProjectionRaw and GetEyeToHeadTransform EVERY FRAME (~1080/s each),
// through ordinary member-convention call sites (register capture: rcx is
// the interface pointer), rendering near=1 far=50000, and both rigs receive
// exact-visible tangents -- no overscan anywhere. The Q3's frustum is
// strongly asymmetric (54/40 horizontal, 55/44 vertical); a culler that
// assumes a CENTERED frustum would drop exactly the visible outer edges,
// which is where every report puts the missing quads.
//
// Phase 1 is the CULL GUARD: when armed (fix.cull_guard in edvr.ini), the
// projection answers handed to the game describe a WIDER frustum than the
// headset shows, and the bounds handed to the compositor at Submit are
// narrowed so it samples exactly the true-frustum region back out of the
// wider-rendered image. The compositor is consistent throughout: it never
// sees the lie, only a texture whose named region covers the eye's real
// FOV. Two modes:
//
//   symmetric -- each axis reported as +/-max of its two tangents. If the
//   centered-frustum hypothesis is right, this fits the culler exactly.
//   percent   -- every tangent inflated by N%: a mechanism-agnostic margin.
//
// CONSISTENCY IS THE WHOLE GAME. The game must never see mixed answers
// within one frame, so the lie switches on and off only at the frame
// boundary (systemHookFrameBoundary, called from the compositor's
// WaitGetPoses hook after the game is released and before it queries), and
// only after the TRUE tangents of both eyes have been captured and the
// runtime's matrix has been checked against the tangent formula. The submit
// crop is keyed on the same switch.
//
// Two kinds of slot, two mechanisms (EVIDENCE 6bo), and phase 1 keeps the
// split where it can:
//
//   Slots 0 and 2 return nothing by value; C thunks with an explicit `self`
//   receive them correctly under every convention. The raw thunk is where
//   the tangent lie is told.
//
//   Slot 1 (GetProjectionMatrix) returns a struct, where the member and
//   free conventions disagree about the hidden return pointer. Phase 0
//   observed it convention-blind by asm thunk; phase 1 must EDIT it, and
//   the field capture licensed the receiver: every observed caller uses the
//   member convention, so when the guard is armed at startup the slot gets
//   a MEMBER-SHAPED C++ receiver (`this` arrives as the interface pointer;
//   the forward is a member-shaped call through the original entry). With
//   the guard off at startup the phase-0 asm thunk stays, and slot 4
//   (GetEyeToHeadTransform) keeps it always -- IPD and cant are never lied
//   about.
//
// The receiver never trusts the formula it edits by: on each eye's first
// matrix call it predicts the four tangent-carrying elements from the TRUE
// raw tangents (m00=2/(r-l), m02=(r+l)/(r-l), m11=2/(b-t), m12=(b+t)/(b-t)
// -- cross-checked against EVIDENCE 4.2's live values, which match to four
// decimals) and compares against what the runtime actually returned. A
// runtime that builds its matrix differently makes the whole guard inert,
// loudly, and everything forwards the truth.
//
// IVRSystem is still never CALLED from inside the game process (the 6c
// rule). Receiving calls the game makes is not calling; the ban stands.
// ---------------------------------------------------------------------------

// The asm contract. Everything the thunks touch is defined here, C-named,
// and the offsets they hardcode are asserted below.
extern "C" {
uint32_t edvr_sysCounts[8] = {};       // per-slot call counters
uint32_t edvr_sysCapClaim[8] = {};     // bit 0: a thread won the first-call capture
uint32_t edvr_sysCapDone[8] = {};      // 1: capture words are all written
uint64_t edvr_sysCapture[8][7] = {};   // rcx rdx r8 r9 xmm3 [rsp+28h] [rsp+30h]
void*    edvr_sysOrig[8] = {};         // forward targets, written before commit
void edvr_sysThunkMatrix();            // slot 1, system_thunks.asm
void edvr_sysThunkEyeToHead();         // slot 4, system_thunks.asm
}
static_assert(sizeof(edvr_sysCounts) == 32, "asm indexes dword[8]");
static_assert(sizeof(edvr_sysCapture) == 448 && sizeof(edvr_sysCapture[0]) == 56,
              "asm hardcodes a 56-byte capture stride");

namespace edvr {
namespace {

// Slot layout for IVRSystem_012 and no other version. 0, 1 and 4 are
// probe-verified against the real runtime (EVIDENCE 4.1-4.3); 2 sits between
// two verified anchors in the declaration order of the era's header and is
// additionally shape-validated on its first call below. Later generations
// dropped GetProjectionMatrix's fourth parameter (EVIDENCE 4.9), so this
// table must never be forward-listed the way the compositor's is -- a
// version bump changes the ABI, not just the indices.
constexpr size_t kSlotSize = 0;
constexpr size_t kSlotMatrix = 1;
constexpr size_t kSlotRaw = 2;
constexpr size_t kSlotEyeToHead = 4;

typedef void (*PFN_Size)(void* self, uint32_t* w, uint32_t* h);
typedef void (*PFN_Raw)(void* self, int32_t eye, float* l, float* r, float* t,
                        float* b);

enum class GuardMode : uint8_t { Off, Symmetric, Percent };

// How long periodic() waits for a frame boundary that never comes before
// promoting the lie itself. In the game, WaitGetPoses fires every ~11 ms
// and this path is unreachable; in a boundary-less process (the smoke
// harness) it is the only promoter.
constexpr uint64_t kBoundaryFallbackMs = 2000;

struct State {
    VTableHook hook;
    // Identity token only, like the compositor's: patching entries in place
    // hooks every object sharing the table, so values are recorded -- and
    // lies are told -- only for the interface WE were handed; everything
    // else forwards the truth untouched.
    void*     ownerIface = nullptr;
    Sentinel* sentinel = nullptr;
    bool      refusedThisSession = false;

    bool installed = false;
    bool inert = false;        // observation off; forwarding continues
    bool validated = false;    // slot 2 has produced one sane reading
    bool confirmed = false;    // sentinel released after surviving a frame

    // --- the guard ---
    // modeRequested/percent are written by systemHookConfigure (install
    // thread and the reload poll on the frame thread) and read everywhere;
    // single-word writes, and a torn read costs one oddly-margined frame in
    // an opt-in experiment.
    GuardMode modeRequested = GuardMode::Off;
    float     percent = 8.0f;
    // How much of each axis's deficit symmetric mode actually covers: the
    // short side is extended by fraction x (larger tangent - short tangent).
    // 1.0 is the full symmetrization the verdict flight proved sufficient;
    // the point of the knob is walking DOWN from sufficiency to the
    // cheapest value that still keeps the edges clean, live, one flight.
    float     fracH = 1.0f, fracV = 1.0f;
    // Which headsets the guard arms for, as rounded-degree FOV signatures
    // ("94x99"); empty = all. One shared ini serves rigs that swap headsets:
    // the rig that shows the bug pays for the fix, the one that never did
    // pays nothing, and nobody edits config at swap time.
    uint32_t  gateSig[8][2] = {};
    uint32_t  gateCount = 0;
    bool      sigLogged = false;
    bool      gateNoted = false;
    bool      restage = false;            // margin changed mid-flight: redo 1->2
    bool      guardHeldForIntroNoted = false;  // said once, while the intro is up
    bool      submitCopy = true;          // copy mechanism vs narrowed bounds
    bool      receiverInstalled = false;  // slot 1 = member receiver, not asm
    bool      restartNoted = false;       // "enable needs a restart", said once
    bool      guardInert = false;         // formula or shape failed; truth only

    // The temporal pass's jitter for the current frame (system_hook.h):
    // written at the frame boundary, read by the raw thunk and the matrix
    // receiver on the game's thread -- the same plain-store discipline as
    // the lie, for the same reason.
    bool      jitLive = false;
    float     jitDx = 0.0f, jitDy = 0.0f;
    bool      receiverForTemporal = false;  // the receiver was installed for it

    // THE TWO-STAGE GO-LIVE. Both field failures of this guard were the
    // transport (OpenComposite over VDXR) mishandling a submission shape the
    // session had not served before -- narrowed bounds first, then a
    // cropped texture whose aspect differed from the canonical size. So the
    // guard never changes what the transport sees: stage 1 lies about
    // GetRecommendedRenderTargetSize so the game rebuilds its targets
    // BIGGER (to the transport that is an ordinary supersampling change,
    // proven handled in the field), and only when both eyes' submissions
    // arrive at the inflated size does stage 2 start the projection lie --
    // whose crop then lands, snapped, at EXACTLY the canonical size the
    // session established. Submissions never change shape at the moment the
    // lie begins, and the resolution cost of the first design disappears:
    // the margin is rendered in NEW pixels, not carved out of the old ones.
    uint8_t   stage = 0;                  // 0 off, 1 size-only, 2 full
    bool      lieLive = false;            // stage 2, kept as the hot-path flag
    bool      liePending = false;         // prerequisites met, awaiting boundary
    uint64_t  liePendingSinceMs = 0;
    uint64_t  stage1SinceMs = 0;
    // Submitted sizes as they stood when stage 1 began. Adoption requires
    // the submissions to CHANGE from these, not merely to exceed the
    // threshold: a re-stage to a SMALLER margin starts while the old,
    // bigger targets are still flowing, and those satisfied any >= test
    // instantly -- field log 2026-08-18 21:35:01, an 8 ms "adoption" that
    // froze the canonical against targets the game was about to shrink,
    // which the snap would later refuse as a mid-session stand-down.
    //
    // SEEDED BY THE FIRST SUBMISSIONS AFTER STAGE 1 BEGINS, not read at
    // the boundary: the probe only runs during stage 1, so at a session's
    // first staging these were zero, and zero had "moved" to whatever the
    // game was already submitting. At HMD Quality 1.0 that was harmless --
    // the native size fell short of the threshold and adoption waited for
    // the rebuild -- but at 1.25 the un-rebuilt 6780-wide targets cleared
    // 0.97 x 5792 on the very first probe, the guard adopted a canonical
    // of 6349 the game was about to abandon, and stood down when the real
    // 7240 arrived (Pimax, 2026-09-02, both flights). The game takes about
    // two seconds to rebuild (measured, the first of those flights), so
    // the first probe after the boundary is the size to move FROM.
    uint32_t  preStageW[2] = {}, preStageH[2] = {};
    bool      preStageSeeded[2] = {};
    bool      marginNoop = false;  // factors ~1.0: staging would never finish
    bool      stage1WaitNoted = false;
    float     sizeFactorH = 1.0f, sizeFactorV = 1.0f;
    uint32_t  trueSizeW = 0, trueSizeH = 0;    // last truth from slot 0
    uint32_t  submittedW[2] = {}, submittedH[2] = {};
    uint32_t  canonicalW[2] = {}, canonicalH[2] = {};  // crop snap target
    uint64_t  lastBoundaryMs = 0;         // 0 = no boundary driver seen
    uint32_t  cropsApplied = 0;           // eye-submits cropped (bounds mode)
    bool      matrixFormulaOk[2] = {};
    bool      matrixChecked[2] = {};
    float     nearZ = 0.0f, farZ = 0.0f;   // the SCENE's planes: the smallest near the game asks with
    float     planePairs[4][2] = {};       // the distinct pairs seen, for the log
    int       planePairCount = 0;
    float     eyeToHead[2][12] = {};
    bool      eyeToHeadValid[2] = {};
    bool      eyeToHeadTried[2] = {};
    bool      receiverFirstLogged = false;

    // True tangents per eye as the runtime reports them (l r t b), and the
    // lie derived from them. Written by the raw thunk on the game's thread,
    // read by the matrix receiver, the crop, and periodic() -- plain stores
    // on purpose: the values are quasi-static (optics), a torn float during
    // the one write that ever changes them costs one odd frame, and locking
    // a per-frame hot path for that is the wrong trade.
    float    trueRaw[2][4] = {};
    bool     trueSeen[2] = {};
    float    lied[2][4] = {};

    // --- observation state (phase 0) ---
    uint32_t sizeW = 0, sizeH = 0;
    bool     sizeDirty = false;
    bool     rawDirty[2] = {};

    // What periodic() last reported, for change detection.
    uint32_t lastSizeW = 0, lastSizeH = 0;
    float    lastRaw[2][4] = {};
    bool     lastRawSeen[2] = {};
    uint32_t lastCounts[8] = {};
    bool     capLogged[8] = {};

    uint64_t summaryStampMs = 0;
    uint32_t valueLinesLeft = 48;  // cap on value lines; summaries continue
};

State* g_state = nullptr;

// periodic() can be entered from the compositor's frame thread and from the
// observed calls' threads at once; one at a time is enough and blocking a
// render path on a log line would be backwards.
volatile LONG g_periodicBusy = 0;

const char* eyeName(int32_t eye) {
    return eye == 0 ? "left" : eye == 1 ? "right" : "?";
}

float degrees(float tangent) {
    return atanf(fabsf(tangent)) * 57.29578f;
}

const char* modeName(GuardMode m) {
    return m == GuardMode::Symmetric ? "symmetric"
         : m == GuardMode::Percent   ? "percent"
                                     : "off";
}

// The lie, derived fresh from the true tangents on every call so a live
// mode or margin change flows without any cached state to invalidate.
// Returns false (and the guard should stand down) if the result is not a
// sane frustum.
//
// Symmetric mode extends only the SHORT side of each axis, by fraction x
// its deficit against the larger tangent: fraction 1 reports a fully
// symmetric frustum (the shape the verdict flight proved sufficient), 0
// reports the truth for that axis, and anything between trades coverage of
// the extreme outer degrees for rendered pixels. An axis that is already
// symmetric has no deficit and costs nothing at any fraction -- which is
// why the Pimax's vertical rode along free.
bool computeLied(GuardMode mode, float pct, float fh, float fv,
                 const float t[4], float out[4]) {
    if (mode == GuardMode::Symmetric) {
        auto extend = [](float neg, float pos, float frac, float* outNeg,
                         float* outPos) {
            const float m = fabsf(neg) > fabsf(pos) ? fabsf(neg) : fabsf(pos);
            *outNeg = neg - (fabsf(neg) < m ? frac * (m - fabsf(neg)) : 0.0f);
            *outPos = pos + (fabsf(pos) < m ? frac * (m - fabsf(pos)) : 0.0f);
        };
        extend(t[0], t[1], fh, &out[0], &out[1]);
        extend(t[2], t[3], fv, &out[2], &out[3]);
    } else {
        const float f = 1.0f + pct / 100.0f;
        for (int i = 0; i < 4; ++i) out[i] = t[i] * f;
    }
    for (int i = 0; i < 4; ++i) {
        if (!std::isfinite(out[i]) || fabsf(out[i]) > 20.0f) return false;
    }
    return out[0] < out[1] && out[2] < out[3] && out[0] <= t[0] &&
           out[1] >= t[1] && out[2] <= t[2] && out[3] >= t[3];
}

// The rounded-degree FOV signature this headset reports, from eye 0's true
// tangents -- "94x99" on the Quest 3 rig, "103x103" on the Pimax. Stable
// per headset and runtime because it is a property of the optics.
void fovSignature(const float t[4], uint32_t* w, uint32_t* h) {
    *w = static_cast<uint32_t>(lroundf(degrees(t[0]) + degrees(t[1])));
    *h = static_cast<uint32_t>(lroundf(degrees(t[2]) + degrees(t[3])));
}

bool gatePasses(const State* s) {
    if (s->gateCount == 0) return true;
    if (!s->trueSeen[0]) return false;
    uint32_t w = 0, h = 0;
    fovSignature(s->trueRaw[0], &w, &h);
    for (uint32_t i = 0; i < s->gateCount; ++i) {
        if (s->gateSig[i][0] == w && s->gateSig[i][1] == h) return true;
    }
    return false;
}

bool lieActiveFor(const State* s, int32_t eye) {
    return s->lieLive && !s->guardInert && s->modeRequested != GuardMode::Off &&
           eye >= 0 && eye < 2 && s->trueSeen[eye];
}

// ---------------------------------------------------------------------------
// The C thunks. Shape proven by EVIDENCE 6bo cell 1: no struct return, so a
// member-convention caller lands here with self/args in exactly these
// positions. Forward first, observe (and, for raw, answer) after.
// ---------------------------------------------------------------------------

void hookedGetRecommendedRenderTargetSize(void* self, uint32_t* w, uint32_t* h) {
    InterlockedIncrement(reinterpret_cast<volatile LONG*>(&edvr_sysCounts[kSlotSize]));
    reinterpret_cast<PFN_Size>(edvr_sysOrig[kSlotSize])(self, w, h);

    State* s = g_state;
    if (!s || s->inert || self != s->ownerIface || !w || !h) return;
    guarded("sysHook/size", [&] {
        s->trueSizeW = *w;
        s->trueSizeH = *h;
        s->sizeW = *w;
        s->sizeH = *h;
        s->sizeDirty = true;
        // Stage 1 and up: the game is asked for targets big enough that the
        // wide render keeps the session's pixels-per-degree. The runtime
        // itself is never told anything -- only the game's answer changes,
        // and the game treats it exactly like a supersampling change.
        if (s->stage >= 1 && !s->guardInert) {
            *w = static_cast<uint32_t>(
                lroundf(static_cast<float>(*w) * s->sizeFactorH));
            *h = static_cast<uint32_t>(
                lroundf(static_cast<float>(*h) * s->sizeFactorV));
        }
    });
    systemHookPeriodic();
}

void hookedGetProjectionRaw(void* self, int32_t eye, float* l, float* r,
                            float* t, float* b) {
    InterlockedIncrement(reinterpret_cast<volatile LONG*>(&edvr_sysCounts[kSlotRaw]));

    State* s = g_state;
    // Argument shape, checked BEFORE the forward on the calls we would
    // observe: if this slot is not GetProjectionRaw, these registers belong
    // to some other method and the values will not look like an eye and four
    // pointers. Going inert keeps the observation honest; the forward still
    // happens, because refusing to forward is not this hook's to decide.
    if (s && !s->inert && self == s->ownerIface) {
        if ((eye != 0 && eye != 1) || !l || !r || !t || !b) {
            s->inert = true;
            s->guardInert = true;
            Log::get().note(
                "IVRSystem observation going INERT: GetProjectionRaw's arguments "
                "do not look like an eye and four tangent pointers (eye=%d l=%p "
                "r=%p t=%p b=%p). The slot table is wrong for this runtime's "
                "IVRSystem_012; calls keep forwarding untouched, nothing more is "
                "recorded, and the cull guard stays off. Please report this log.",
                eye, static_cast<void*>(l), static_cast<void*>(r),
                static_cast<void*>(t), static_cast<void*>(b));
        }
    }

    reinterpret_cast<PFN_Raw>(edvr_sysOrig[kSlotRaw])(self, eye, l, r, t, b);

    s = g_state;
    if (!s || s->inert || self != s->ownerIface) return;
    if (eye != 0 && eye != 1) return;
    guarded("sysHook/raw", [&] {
        const float lv = *l, rv = *r, tv = *t, bv = *b;
        // Value shape, checked AFTER the forward, once: tangents are finite,
        // left of right, and within anything optics can produce. Loose on
        // purpose -- Pimax parallel-projection tangents run large, and an
        // instrument that rejects the unusual values is rejecting exactly the
        // rig the investigation is about.
        if (!s->validated) {
            const bool sane = std::isfinite(lv) && std::isfinite(rv) &&
                              std::isfinite(tv) && std::isfinite(bv) &&
                              lv < rv && fabsf(lv) < 20.0f && fabsf(rv) < 20.0f &&
                              fabsf(tv) < 20.0f && fabsf(bv) < 20.0f;
            if (!sane) {
                s->inert = true;
                s->guardInert = true;
                Log::get().note(
                    "IVRSystem observation going INERT: GetProjectionRaw returned "
                    "l=%g r=%g t=%g b=%g, which is not a projection. The slot "
                    "table is wrong for this runtime; calls keep forwarding "
                    "untouched and the cull guard stays off. Please report this "
                    "log.",
                    lv, rv, tv, bv);
                return;
            }
            s->validated = true;
        }
        s->trueRaw[eye][0] = lv;
        s->trueRaw[eye][1] = rv;
        s->trueRaw[eye][2] = tv;
        s->trueRaw[eye][3] = bv;
        s->trueSeen[eye] = true;
        s->rawDirty[eye] = true;

        // The lie, recomputed from truth on every call. When it is live for
        // this eye, THIS is where the game learns the wider frustum.
        if (s->modeRequested != GuardMode::Off && s->receiverInstalled &&
            !s->guardInert) {
            float lie[4];
            if (!computeLied(s->modeRequested, s->percent, s->fracH, s->fracV,
                             s->trueRaw[eye], lie)) {
                s->guardInert = true;
                Log::get().note(
                    "cull guard INERT: the %s-mode frustum derived from tangents "
                    "l=%g r=%g t=%g b=%g is not sane. Truth only from here; "
                    "report this log.",
                    modeName(s->modeRequested), lv, rv, tv, bv);
                return;
            }
            memcpy(s->lied[eye], lie, sizeof(lie));
            // Armed only from a standing start, and only for a headset the
            // gate names (an empty gate names them all): a rig that swaps
            // headsets pays for the fix exactly where its owner said to.
            if (s->stage == 0 && !s->lieLive && !s->liePending &&
                !s->marginNoop && s->trueSeen[0] && s->trueSeen[1] &&
                gatePasses(s)) {
                s->liePending = true;
                s->liePendingSinceMs = stampMs();
            }
            if (lieActiveFor(s, eye)) {
                *l = lie[0];
                *r = lie[1];
                *t = lie[2];
                *b = lie[3];
            }
        }
        // The temporal pass's jitter, after the lie: a sub-pixel shift of
        // the whole frustum, told to the game only while the receiver can
        // tell it the same thing (systemHookJitterAvailable), so the raw
        // and matrix answers never disagree about where the frustum sits.
        if (s->jitLive && s->receiverInstalled && s->matrixFormulaOk[eye]) {
            *l += s->jitDx;
            *r += s->jitDx;
            *t += s->jitDy;
            *b += s->jitDy;
        }
    });
    systemHookPeriodic();
}

// ---------------------------------------------------------------------------
// The member-shaped GetProjectionMatrix receiver (guard-armed sessions only).
//
// `this` arrives holding the interface pointer -- the receiver has no
// members and never dereferences it as itself. The forward is a
// member-shaped call through the original entry, so a member-convention
// caller (every caller phase 0 observed, on both rigs) sees a perfectly
// ordinary call whichever path it takes.
// ---------------------------------------------------------------------------

struct SysIfaceTag { };  // typed handle for member-shaped calls; never real
typedef vr::HmdMatrix44_t (SysIfaceTag::*PMF_Matrix)(int32_t, float, float,
                                                     int32_t);
static_assert(sizeof(PMF_Matrix) == sizeof(void*),
              "a single-inheritance member pointer must be one code pointer");

vr::HmdMatrix44_t forwardMatrix(void* self, int32_t eye, float nearZ,
                                float farZ, int32_t projType) {
    PMF_Matrix f;
    memcpy(&f, &edvr_sysOrig[kSlotMatrix], sizeof(f));
    return (reinterpret_cast<SysIfaceTag*>(self)->*f)(eye, nearZ, farZ, projType);
}

struct MatrixReceiver {
    vr::HmdMatrix44_t GetProjectionMatrix(int32_t eye, float nearZ, float farZ,
                                          int32_t projType);
};

vr::HmdMatrix44_t MatrixReceiver::GetProjectionMatrix(int32_t eye, float nearZ,
                                                      float farZ,
                                                      int32_t projType) {
    void* self = this;  // the interface pointer, an identity token only
    InterlockedIncrement(reinterpret_cast<volatile LONG*>(&edvr_sysCounts[kSlotMatrix]));

    vr::HmdMatrix44_t m = forwardMatrix(self, eye, nearZ, farZ, projType);

    State* s = g_state;
    if (!s || self != s->ownerIface || (eye != 0 && eye != 1)) return m;
    // The game asks for its projection with more than one pair of planes
    // -- 0.025..50000 for the scene and 0.1..1000 for something else, on
    // the depth flight of 2026-09-03 -- and the depth reprojection wants
    // the scene's. The smallest near names it: the last pair seen did
    // not, and read the cockpit four times too far. Each new pair is
    // logged once.
    if (nearZ > 0.0f && farZ > nearZ) {
        bool known = false;
        for (int i = 0; i < s->planePairCount; ++i) {
            if (fabsf(s->planePairs[i][0] - nearZ) < 1e-6f &&
                fabsf(s->planePairs[i][1] - farZ) < 1e-3f) {
                known = true;
                break;
            }
        }
        if (!known && s->planePairCount < 4) {
            s->planePairs[s->planePairCount][0] = nearZ;
            s->planePairs[s->planePairCount][1] = farZ;
            ++s->planePairCount;
            Log::get().note(
                "IVRSystem: the game asks for its projection with planes "
                "%g..%g m (pair %d seen this session). The depth reprojection "
                "reads the scene's depth with the smallest near of them.",
                static_cast<double>(nearZ), static_cast<double>(farZ),
                s->planePairCount);
        }
        if (!(s->nearZ > 0.0f) || nearZ < s->nearZ) {
            s->nearZ = nearZ;
            s->farZ = farZ;
        }
    }

    if (!s->receiverFirstLogged) {
        s->receiverFirstLogged = true;
        Log::get().note(
            "IVRSystem: first GetProjectionMatrix through the guard's receiver: "
            "eye=%d nearZ=%g farZ=%g projType=%d. Member-convention receipt, as "
            "the phase-0 capture said every caller uses.",
            eye, nearZ, farZ, projType);
    }

    // The formula check, once per eye, before any edit is ever applied: the
    // four tangent-carrying elements the runtime returned must match what
    // the TRUE tangents predict, or this runtime builds its matrix some
    // other way and the guard has no business editing it.
    if (!s->matrixChecked[eye] && s->trueSeen[eye]) {
        s->matrixChecked[eye] = true;
        const float* t = s->trueRaw[eye];
        const float du = t[1] - t[0], dv = t[3] - t[2];
        if (du > 1e-4f && dv > 1e-4f) {
            const float p00 = 2.0f / du, p02 = (t[1] + t[0]) / du;
            const float p11 = 2.0f / dv, p12 = (t[3] + t[2]) / dv;
            const float e00 = fabsf(m.m[0][0] - p00);
            const float e02 = fabsf(m.m[0][2] - p02);
            const float e11 = fabsf(m.m[1][1] - p11);
            const float e12 = fabsf(m.m[1][2] - p12);
            const float tol = 2e-3f;
            if (e00 > tol * fabsf(p00) + tol || e02 > tol ||
                e11 > tol * fabsf(p11) + tol || e12 > tol) {
                s->guardInert = true;
                Log::get().note(
                    "cull guard INERT: this runtime's GetProjectionMatrix does "
                    "not match the tangent formula for the %s eye (got "
                    "m00=%g m02=%g m11=%g m12=%g, predicted %g/%g/%g/%g from "
                    "tangents l=%g r=%g t=%g b=%g). Editing it would hand the "
                    "game an inconsistent projection, so nothing is edited and "
                    "nothing is lied about. Please report this log.",
                    eyeName(eye), m.m[0][0], m.m[0][2], m.m[1][1], m.m[1][2],
                    p00, p02, p11, p12, t[0], t[1], t[2], t[3]);
            } else {
                s->matrixFormulaOk[eye] = true;
                Log::get().note(
                    "cull guard: GetProjectionMatrix matches the tangent formula "
                    "for the %s eye (m00/m02/m11/m12 within tolerance) -- the "
                    "guard may edit it.",
                    eyeName(eye));
            }
        }
    }

    // The edit: the lie's tangents when the guard is live, the truth's
    // otherwise, either shifted by the temporal pass's jitter when one is
    // live -- the same numbers the raw thunk answers, so the two calls tell
    // one story about where the frustum sits this frame.
    const bool lie = lieActiveFor(s, eye);
    const bool jit = s->jitLive && s->matrixFormulaOk[eye];
    if ((lie || jit) && s->matrixFormulaOk[eye]) {
        float tt[4];
        memcpy(tt, lie ? s->lied[eye] : s->trueRaw[eye], sizeof(tt));
        if (jit) {
            tt[0] += s->jitDx;
            tt[1] += s->jitDx;
            tt[2] += s->jitDy;
            tt[3] += s->jitDy;
        }
        const float du = tt[1] - tt[0], dv = tt[3] - tt[2];
        if (du > 1e-4f && dv > 1e-4f) {
            m.m[0][0] = 2.0f / du;
            m.m[0][2] = (tt[1] + tt[0]) / du;
            m.m[1][1] = 2.0f / dv;
            m.m[1][2] = (tt[3] + tt[2]) / dv;
        }
    }
    return m;
}

// ---------------------------------------------------------------------------
// Deferred reporting.
// ---------------------------------------------------------------------------

void logCaptureIfReady(State* s, size_t slot, const char* name) {
    if (s->capLogged[slot] || !edvr_sysCapDone[slot]) return;
    s->capLogged[slot] = true;

    const uint64_t* c = edvr_sysCapture[slot];
    const void* rcx = reinterpret_cast<const void*>(c[0]);
    const void* rdx = reinterpret_cast<const void*>(c[1]);
    float xmm3f = 0.0f;
    memcpy(&xmm3f, &c[4], sizeof(xmm3f));

    // The convention verdict, which is the fact the guard's matrix receiver
    // stands on. A C++ member call carries `this` first and the return slot
    // second; the free-style shape carries the return slot first.
    const char* verdict =
        rcx == s->ownerIface
            ? "rcx IS the interface pointer: the caller uses the C++ member "
              "convention (this first, return slot second), as a compiler-"
              "generated call site would"
        : rdx == s->ownerIface
            ? "rdx is the interface pointer: the caller passes the return "
              "slot FIRST -- the free-function shape, like the old probe"
            : "neither rcx nor rdx is the interface this hook was installed "
              "on -- another consumer of the same class reached the thunk "
              "first";

    Log::get().note(
        "IVRSystem: first %s call captured: rcx=%p rdx=%p r8=0x%llx r9=0x%llx "
        "xmm3=%g stack4=0x%llx stack5=0x%llx. %s.",
        name, rcx, rdx, static_cast<unsigned long long>(c[2]),
        static_cast<unsigned long long>(c[3]), xmm3f,
        static_cast<unsigned long long>(c[5]),
        static_cast<unsigned long long>(c[6]), verdict);
}

void emitValues(State* s) {
    if (s->sizeDirty) {
        s->sizeDirty = false;
        if ((s->sizeW != s->lastSizeW || s->sizeH != s->lastSizeH) &&
            s->valueLinesLeft > 0) {
            --s->valueLinesLeft;
            Log::get().note(
                "IVRSystem: GetRecommendedRenderTargetSize -> %ux%u per eye%s",
                s->sizeW, s->sizeH,
                s->lastSizeW ? " -- CHANGED, the runtime moved the render "
                               "resolution under the game"
                             : "");
            s->lastSizeW = s->sizeW;
            s->lastSizeH = s->sizeH;
        }
    }

    for (int eye = 0; eye < 2; ++eye) {
        if (!s->rawDirty[eye]) continue;
        s->rawDirty[eye] = false;
        float v[4];
        memcpy(v, s->trueRaw[eye], sizeof(v));
        // Publish the horizontal frustum for the RemLok angle derivation on
        // every flush, not only the logged ones -- valueLinesLeft caps the
        // LOG, and a reader downstream must not starve because the log went
        // quiet. The eyes mirror, so whichever eye flushed carries the pair.
        {
            const float lm = fabsf(v[0]), rm = fabsf(v[1]);
            announceEyeTangents(lm > rm ? lm : rm, lm > rm ? rm : lm);
            // And the VERTICAL pair, which used to be thrown away here.
            // The intro panel derived it from the horizontal span and the
            // eye's shape, which assumes the vertical frustum is
            // symmetric. A Pimax's is (t=-1.2648 b=+1.2648) and a Quest 3's
            // is not (t=-1.4281 b=+0.9657), so on the Quest the panel was
            // built through a projection that did not match the runtime's
            // and sheared as the head moved. v[2] is top, v[3] is bottom;
            // both eyes report the same pair.
            announceEyeTangentsVertical(fabsf(v[2]), fabsf(v[3]));
        }
        bool moved = !s->lastRawSeen[eye];
        for (int i = 0; i < 4 && !moved; ++i) {
            if (fabsf(v[i] - s->lastRaw[eye][i]) > 1e-4f) moved = true;
        }
        if (!moved || s->valueLinesLeft == 0) continue;
        --s->valueLinesLeft;
        // The tangents are the payload of this whole instrument: they are the
        // FOV the game believes it is rendering. Degrees inline, so a log
        // read on a phone in a headset queue answers the question directly.
        Log::get().note(
            "IVRSystem: GetProjectionRaw(%s eye) TRUE tangents l=%+.4f r=%+.4f "
            "t=%+.4f b=%+.4f -> %.1f+%.1f = %.1f deg horizontal, "
            "%.1f+%.1f = %.1f deg vertical%s%s",
            eyeName(eye), v[0], v[1], v[2], v[3],
            degrees(v[0]), degrees(v[1]), degrees(v[0]) + degrees(v[1]),
            degrees(v[2]), degrees(v[3]), degrees(v[2]) + degrees(v[3]),
            s->lastRawSeen[eye] ? " -- CHANGED" : "",
            lieActiveFor(s, eye) ? " [guard live: the game is being told the "
                                   "reported values in the go-live line]"
                                 : "");
        memcpy(s->lastRaw[eye], v, sizeof(v));
        s->lastRawSeen[eye] = true;
    }
}

void emitSummary(State* s) {
    if (!dueMs(s->summaryStampMs, 10000)) return;
    s->summaryStampMs = stampMs();

    uint32_t now[8];
    memcpy(now, edvr_sysCounts, sizeof(now));
    bool anyNew = false;
    for (int i = 0; i < 8; ++i) {
        if (now[i] != s->lastCounts[i]) anyNew = true;
    }
    if (!anyNew) return;  // a quiet interface earns a quiet log

    Log::get().note(
        "IVRSystem calls, last 10s (session total): size %u (%u), "
        "projMatrix %u (%u), projRaw %u (%u), eyeToHead %u (%u)",
        now[kSlotSize] - s->lastCounts[kSlotSize], now[kSlotSize],
        now[kSlotMatrix] - s->lastCounts[kSlotMatrix], now[kSlotMatrix],
        now[kSlotRaw] - s->lastCounts[kSlotRaw], now[kSlotRaw],
        now[kSlotEyeToHead] - s->lastCounts[kSlotEyeToHead],
        now[kSlotEyeToHead]);
    memcpy(s->lastCounts, now, sizeof(now));
}

// The crop fractions of the true frustum within the lied one, per axis.
// Shared by the submit crop and the go-live report so what is logged is
// what is done.
//
// THE AXIS DIRECTIONS ARE DERIVED, NOT ASSUMED, because one of them shipped
// wrong. From the runtime's own matrix formula (field-verified against
// EVIDENCE 4.2 to four decimals): the u=1 edge satisfies x/-z = (1+m02)/m00
// = r, so u runs l -> r and the horizontal fractions measure from l. The
// v=0 edge satisfies y/-z = (1+m12)/m11 = b -- NDC y=+1, which D3D11's
// viewport transform puts at the TOP of the target, texture row 0 -- so v
// runs b -> t and the vertical fractions measure from B, the POSITIVE
// tangent. The first version measured them from t; every column kept the
// wrong end of itself, the content sat ~11 degrees off the declared
// vertical centre, and the compositor's positional reprojection turned
// every forward lean into a vertical stretch (field, 2026-08-18). The
// smoke fixture's vertical is asymmetric on purpose so that inversion can
// never come back quietly.
bool cropFractions(const State* s, int eye, float out[4]) {
    const float* t = s->trueRaw[eye];
    const float* lie = s->lied[eye];
    const float du = lie[1] - lie[0], dv = lie[3] - lie[2];
    if (du < 1e-4f || dv < 1e-4f) return false;
    out[0] = (t[0] - lie[0]) / du;   // left:   u=0 is the l' edge
    out[1] = (lie[3] - t[3]) / dv;   // top:    v=0 is the b' edge
    out[2] = (t[1] - lie[0]) / du;   // right
    out[3] = (lie[3] - t[2]) / dv;   // bottom
    return true;
}

// Stage transitions happen HERE and only here, at the frame boundary (or
// periodic's fallback for boundary-less processes), so one frame's raw
// answers, matrix answers, target-size answers and submit handling always
// agree.
void promoteOrDemote(State* s) {
    // Demotion wins, from any stage. The game re-reads the true target size
    // within a frame and rebuilds its targets back; to the transport that is
    // one ordinary full-texture resize, the event the field has proven it
    // handles. A margin or gate change demotes the same way and then re-arms
    // organically -- the per-call arming logic re-runs the moment the raw
    // thunk sees the new configuration, so a re-stage is a demotion plus
    // nothing.
    if (s->stage > 0 &&
        (s->guardInert || s->modeRequested == GuardMode::Off || s->restage)) {
        const bool restaging =
            s->restage && !s->guardInert && s->modeRequested != GuardMode::Off;
        const bool wasLive = s->lieLive;
        s->restage = false;
        s->stage = 0;
        s->lieLive = false;
        s->liePending = false;
        s->stage1WaitNoted = false;
        // Published at every stage transition, HERE and in the two promotions
        // below, so the d3d11 half's churn attribution (frame_flag.h, spec
        // §1g) always names the stage that governed the frame: transitions
        // happen only at this boundary, before the game queries.
        announceCullGuardState(0, 1.0f, 1.0f);
        if (restaging) {
            Log::get().note(
                "cull guard re-staging: the margin or headset gate changed. "
                "Truth for a moment, then the two stages run again with the "
                "new numbers.");
        } else {
            Log::get().note(
                "cull guard OFF%s: the game sees true projections%s and its "
                "normal target size again from this frame.",
                s->guardInert ? " (inert)" : "",
                wasLive ? ", full submissions" : "");
        }
        return;
    }

    // Stage 0 -> 1: start asking for bigger render targets. Projections stay
    // TRUE; the game simply supersamples while it adopts.
    // NOT WHILE THE INTRO IS UP.
    //
    // Going live widens the frustum the game renders with and crops it
    // back at submit. The intro panel, meanwhile, is placed from the TRUE
    // tangents -- so during the movie and the menu the two disagree by the
    // whole margin, and the panel comes out oversized and displaced.
    // Measured on paper at cull_guard = symmetric with the default
    // fraction: 24% oversize on a Quest 3.
    //
    // Terrain culling is a flight concern. There is nothing for it to do
    // over a movie, so waiting for a scene costs the guard nothing and
    // removes the interaction entirely.
    //
    // gameDevice() is the presence test: without the d3d11 half nobody
    // will ever announce a scene, and an openvr-only install must not
    // have its guard held back forever by a message that cannot come.
    if (s->stage == 0 && s->liePending && !s->guardInert &&
        s->modeRequested != GuardMode::Off && gameDevice() &&
        !sceneArrived()) {
        if (!s->guardHeldForIntroNoted) {
            s->guardHeldForIntroNoted = true;
            Log::get().note(
                "cull guard: holding off until a rendered scene arrives. "
                "Widening the frustum while the intro movie and the menu "
                "are up would put the movie's panel out of step with what "
                "the game renders, and there is no terrain to cull yet.");
        }
        return;
    }

    if (s->stage == 0 && s->liePending && !s->guardInert &&
        s->modeRequested != GuardMode::Off) {
        s->liePending = false;
        const float duT = s->trueRaw[0][1] - s->trueRaw[0][0];
        const float dvT = s->trueRaw[0][3] - s->trueRaw[0][2];
        const float duL = s->lied[0][1] - s->lied[0][0];
        const float dvL = s->lied[0][3] - s->lied[0][2];
        if (duT < 1e-4f || dvT < 1e-4f || duL < duT || dvL < dvT) {
            s->guardInert = true;
            Log::get().note(
                "cull guard INERT: the widened spans are degenerate "
                "(u %g->%g, v %g->%g). Report this log.",
                duT, duL, dvT, dvL);
            return;
        }
        s->sizeFactorH = duL / duT;
        s->sizeFactorV = dvL / dvT;
        // A margin under about a percent asks the game to rebuild targets it
        // will rebuild to the same size, so adoption's changed-size test
        // could never pass and stage 1 would sit forever. It is also not a
        // guard anyone can see. Idle instead, until the config changes.
        if (s->sizeFactorH < 1.01f && s->sizeFactorV < 1.01f) {
            s->marginNoop = true;
            Log::get().note(
                "cull guard idle: the configured margin widens the frustum "
                "under 1%%, which protects nothing. Raise cull_guard_fraction_"
                "h/_v (or cull_guard_percent) to arm it.");
            return;
        }
        for (int e = 0; e < 2; ++e) {
            s->preStageW[e] = 0;
            s->preStageH[e] = 0;
            s->preStageSeeded[e] = false;   // the first probe fills it in
        }
        s->stage = 1;
        s->stage1SinceMs = stampMs();
        announceCullGuardState(1, s->sizeFactorH, s->sizeFactorV);
        Log::get().note(
            "cull guard stage 1 (%s): asking the game for %.0f%% x %.0f%% "
            "larger render targets, so the wider frustum keeps this "
            "session's pixels-per-degree -- about %.0f%% more rendered "
            "pixels while the guard is live (cull_guard_fraction_h/_v tune "
            "this, live). Projections stay true until both eyes submit at "
            "the new size -- the runtime never sees a submission shape this "
            "session has not already served.",
            modeName(s->modeRequested), (s->sizeFactorH - 1.0f) * 100.0f,
            (s->sizeFactorV - 1.0f) * 100.0f,
            (s->sizeFactorH * s->sizeFactorV - 1.0f) * 100.0f);
        return;
    }

    // Stage 1 -> 2: the game has rebuilt its targets, so the lie can start
    // and every crop lands at the canonical size the session established.
    if (s->stage == 1) {
        bool adopted = s->trueSizeW != 0;
        for (int e = 0; e < 2 && adopted; ++e) {
            const float needW =
                static_cast<float>(s->trueSizeW) * s->sizeFactorH * 0.97f;
            const float needH =
                static_cast<float>(s->trueSizeH) * s->sizeFactorV * 0.97f;
            if (static_cast<float>(s->submittedW[e]) < needW ||
                static_cast<float>(s->submittedH[e]) < needH) {
                adopted = false;
            }
            // The size must have MOVED since stage 1 began -- see preStageW.
            // The threshold alone reads leftover bigger-than-needed targets
            // (a re-stage down in margin, or HMD Quality above 1.0) as
            // instant adoption and freezes the canonical against a size the
            // game is about to abandon. Unseeded means no probe has run yet.
            if (!s->preStageSeeded[e] ||
                (s->submittedW[e] == s->preStageW[e] &&
                 s->submittedH[e] == s->preStageH[e])) {
                adopted = false;
            }
        }
        // Boundary-less processes (the test harness) have no submissions and
        // no transport to protect; they go live with free-size crops.
        const bool fallback = s->lastBoundaryMs == 0;
        if (!adopted && !fallback) {
            if (!s->stage1WaitNoted && elapsedMs(s->stage1SinceMs, 10000)) {
                s->stage1WaitNoted = true;
                Log::get().note(
                    "cull guard: still at stage 1 after 10 s -- the game has "
                    "not rebuilt its render targets at the larger size "
                    "(submitting %ux%u / %ux%u, want about %ux%u). Everything "
                    "runs normally meanwhile; if this is the guard's last "
                    "line, report the log.",
                    s->submittedW[0], s->submittedH[0], s->submittedW[1],
                    s->submittedH[1],
                    static_cast<unsigned>(
                        lroundf(s->trueSizeW * s->sizeFactorH)),
                    static_cast<unsigned>(
                        lroundf(s->trueSizeH * s->sizeFactorV)));
            }
            return;
        }
        for (int e = 0; e < 2; ++e) {
            if (adopted) {
                s->canonicalW[e] = static_cast<uint32_t>(lroundf(
                    static_cast<float>(s->submittedW[e]) / s->sizeFactorH));
                s->canonicalH[e] = static_cast<uint32_t>(lroundf(
                    static_cast<float>(s->submittedH[e]) / s->sizeFactorV));
            } else {
                s->canonicalW[e] = 0;
                s->canonicalH[e] = 0;
            }
        }
        s->stage = 2;
        s->lieLive = true;
        announceCullGuardState(2, s->sizeFactorH, s->sizeFactorV);
        for (int eye = 0; eye < 2; ++eye) {
            const float* t = s->trueRaw[eye];
            const float* lie = s->lied[eye];
            float f[4] = {};
            cropFractions(s, eye, f);
            Log::get().note(
                "cull guard LIVE (%s): %s eye true l=%+.4f r=%+.4f t=%+.4f "
                "b=%+.4f -> reported l=%+.4f r=%+.4f t=%+.4f b=%+.4f (FOV "
                "%.1fx%.1f -> %.1fx%.1f deg); submit keeps u %.3f..%.3f, "
                "v %.3f..%.3f of the rendered image, by %s%s.",
                modeName(s->modeRequested), eyeName(eye), t[0], t[1], t[2], t[3],
                lie[0], lie[1], lie[2], lie[3],
                degrees(t[0]) + degrees(t[1]), degrees(t[2]) + degrees(t[3]),
                degrees(lie[0]) + degrees(lie[1]),
                degrees(lie[2]) + degrees(lie[3]),
                f[0], f[2], f[1], f[3],
                s->submitCopy ? "copying that region into an EDVR texture"
                              : "narrowing the submitted bounds",
                s->canonicalW[eye]
                    ? " -- submissions stay at the session's own size"
                    : " (free-size crop, no transport to protect)");
        }
        // The tuning readout: what the current margin leaves uncovered under
        // the centred-culler model the verdict flight supports. This is the
        // number the fraction staircase walks against -- lower the fractions
        // until the quads reappear, and the margin printed on the last clean
        // step is what the culler actually needed.
        {
            const float* t = s->trueRaw[0];
            const float* lie = s->lied[0];
            const float mH = fabsf(t[0]) > fabsf(t[1]) ? fabsf(t[0]) : fabsf(t[1]);
            const float mV = fabsf(t[2]) > fabsf(t[3]) ? fabsf(t[2]) : fabsf(t[3]);
            const float coveredH = atanf((lie[1] - lie[0]) * 0.5f) * 57.29578f;
            const float coveredV = atanf((lie[3] - lie[2]) * 0.5f) * 57.29578f;
            Log::get().note(
                "cull guard margins: if the culler centres the frustum, the "
                "uncovered outer margin is about %.1f deg horizontal, %.1f "
                "vertical (0.0 = fully covered; tune with "
                "cull_guard_fraction_h/_v, live).",
                degrees(mH) - coveredH, degrees(mV) - coveredV);
        }
    }
}

}  // namespace

void systemHookPeriodic() {
    State* s = g_state;
    if (!s || !s->installed) return;
    if (InterlockedExchange(&g_periodicBusy, 1) != 0) return;

    // The sentinel is released on the first pass through here after commit:
    // the process took the patch and lived to run a frame (or answer the next
    // system call). The thunk shapes themselves are covered by test cells, so
    // what the sentinel really guards is the install writing somewhere it
    // should not -- which kills the process long before this line.
    if (!s->confirmed) {
        s->confirmed = true;
        if (s->sentinel) s->sentinel->confirm();
    }

    if (!s->inert) {
        guarded("sysHook/periodic", [&] {
            // The signature line, once, whatever the gate says: it is what a
            // user copies INTO cull_guard_headsets, so it must appear on the
            // headset that is not yet listed.
            if (!s->sigLogged && s->trueSeen[0] && s->trueSeen[1]) {
                s->sigLogged = true;
                uint32_t w = 0, h = 0;
                fovSignature(s->trueRaw[0], &w, &h);
                Log::get().note(
                    "cull guard: this headset's signature is %ux%u (the value "
                    "cull_guard_headsets matches on).",
                    w, h);
            }
            if (!s->gateNoted && s->gateCount > 0 && s->trueSeen[0] &&
                s->receiverInstalled && s->modeRequested != GuardMode::Off &&
                !gatePasses(s)) {
                s->gateNoted = true;
                Log::get().note(
                    "cull guard: armed, but this headset's signature is not in "
                    "cull_guard_headsets -- observation only here, and the "
                    "GPU pays nothing. Add the signature above to the list to "
                    "enable it on this headset too.");
            }
            emitValues(s);
            if (!s->receiverInstalled) {
                logCaptureIfReady(s, kSlotMatrix, "GetProjectionMatrix");
            }
            logCaptureIfReady(s, kSlotEyeToHead, "GetEyeToHeadTransform");
            emitSummary(s);
            // The boundary-less fallback: a process with no compositor hook
            // (the test harness) has no frame boundary, so after two quiet
            // seconds the staging advances here instead -- stage 1 on the
            // first pass, stage 2 on the next (its no-transport fallback
            // needs no further wait). In the game the boundary fires every
            // frame and this never runs.
            if (s->lastBoundaryMs == 0 && !s->lieLive &&
                ((s->liePending &&
                  elapsedMs(s->liePendingSinceMs, kBoundaryFallbackMs)) ||
                 s->stage == 1)) {
                promoteOrDemote(s);
            }
        });
    }

    InterlockedExchange(&g_periodicBusy, 0);
}

void systemHookFrameBoundary() {
    State* s = g_state;
    if (!s || !s->installed) return;
    s->lastBoundaryMs = stampMs();
    guarded("sysHook/boundary", [&] { promoteOrDemote(s); });
    systemHookPeriodic();
}

void systemHookConfigure() {
    State* s = g_state;
    if (!s) return;
    Config& cfg = Config::get();

    const std::string raw = cfg.getString("fix.cull_guard", "off");
    GuardMode mode = GuardMode::Off;
    if (_stricmp(raw.c_str(), "symmetric") == 0) mode = GuardMode::Symmetric;
    else if (_stricmp(raw.c_str(), "percent") == 0) mode = GuardMode::Percent;
    else if (_stricmp(raw.c_str(), "off") != 0 && !raw.empty()) {
        Log::get().note(
            "cull_guard = \"%s\" is not a mode this build knows (off, "
            "symmetric, percent). Treating it as off.",
            raw.c_str());
    }

    float pct = cfg.getFloat("fix.cull_guard_percent", 8.0f);
    if (!std::isfinite(pct) || pct < 0.0f) pct = 0.0f;
    if (pct > 50.0f) pct = 50.0f;

    // Written as two literal reads, not a helper taking the key: the config
    // contract check finds keys by the literal string inside the read call,
    // and a key passed through a parameter is invisible to it -- which it
    // treats, correctly, as unread.
    auto clampFrac = [](float f) {
        if (!std::isfinite(f)) return 1.0f;
        if (f < 0.0f) return 0.0f;
        if (f > 1.0f) return 1.0f;
        return f;
    };
    const float fh = clampFrac(cfg.getFloat("fix.cull_guard_fraction_h", 1.0f));
    const float fv = clampFrac(cfg.getFloat("fix.cull_guard_fraction_v", 1.0f));

    // The headset gate: comma-separated rounded-degree signatures ("94x99").
    // Parsed only when the string changes; a token that does not parse is
    // named once and skipped, never silently matched.
    const std::string gates = cfg.getString("fix.cull_guard_headsets", "");
    uint32_t gateSig[8][2] = {};
    uint32_t gateCount = 0;
    bool gateChanged = false;
    {
        static std::string lastGates;  // configure runs on one thread at a time
        if (gates != lastGates) {
            gateChanged = true;
            lastGates = gates;
        }
        size_t pos = 0;
        while (pos < gates.size() && gateCount < 8) {
            size_t end = gates.find(',', pos);
            if (end == std::string::npos) end = gates.size();
            unsigned w = 0, h = 0;
            const std::string tok = gates.substr(pos, end - pos);
            if (sscanf_s(tok.c_str(), " %ux%u", &w, &h) == 2 && w > 10 &&
                w < 360 && h > 10 && h < 360) {
                gateSig[gateCount][0] = w;
                gateSig[gateCount][1] = h;
                ++gateCount;
            } else if (gateChanged && !tok.empty() &&
                       tok.find_first_not_of(' ') != std::string::npos) {
                Log::get().note(
                    "cull_guard_headsets: \"%s\" is not a WxH signature (the "
                    "log prints each headset's, like 94x99). Skipped.",
                    tok.c_str());
            }
            pos = end + 1;
        }
    }

    // "copy" unless the ini says bounds. Copy is the default because the
    // bounds mechanism was refuted in the field the day it flew: correct by
    // the OpenVR contract, ignored by OpenComposite over VDXR, experienced
    // as the world distorting with every head turn. See guard_crop.h.
    const std::string sub = cfg.getString("advanced.cull_guard_submit", "copy");
    const bool copyMode = _stricmp(sub.c_str(), "bounds") != 0;

    const GuardMode before = s->modeRequested;
    const bool lieParamsChanged =
        before != mode ||
        (mode == GuardMode::Percent && fabsf(s->percent - pct) > 0.01f) ||
        (mode == GuardMode::Symmetric && (fabsf(s->fracH - fh) > 0.001f ||
                                          fabsf(s->fracV - fv) > 0.001f)) ||
        gateChanged;
    s->modeRequested = mode;
    s->percent = pct;
    s->fracH = fh;
    s->fracV = fv;
    memcpy(s->gateSig, gateSig, sizeof(gateSig));
    s->gateCount = gateCount;
    if (gateChanged) s->gateNoted = false;  // the new list earns a new verdict
    if (lieParamsChanged) s->marginNoop = false;  // a new margin earns a retry
    s->submitCopy = copyMode;

    // A live change of anything the lie is built from cannot be mutated into
    // a running stage -- the target-size factors and the crop snap were
    // computed from the OLD margin. Demote and let the stages re-run; to the
    // transport that is one ordinary resize down and, seconds later, one up.
    if (lieParamsChanged && s->stage > 0) s->restage = true;

    if (s->installed && mode != GuardMode::Off && !s->receiverInstalled &&
        !s->restartNoted) {
        s->restartNoted = true;
        Log::get().note(
            "cull_guard = %s was set AFTER startup, but the matrix half of the "
            "lie installs only at launch (the GetProjectionMatrix receiver "
            "replaced nothing this session). A projection lied about in one "
            "call and true in another would be worse than no guard, so nothing "
            "changes until the game is restarted with this setting on.",
            modeName(mode));
    }
    if (lieParamsChanged) {
        if (s->receiverInstalled || mode == GuardMode::Off) {
            Log::get().note(
                "cull guard config: mode %s%s (was %s), margins h=%.2f v=%.2f, "
                "%u headset signature(s) gated. Changes take effect at the "
                "next frame boundary.",
                modeName(mode),
                mode == GuardMode::Percent ? " (see cull_guard_percent)" : "",
                modeName(before), fh, fv, gateCount);
        }
    }
}

bool systemHookCropFractions(vr::EVREye eye, float out[4]) {
    State* s = g_state;
    if (!s || !out) return false;
    const int e = (eye == vr::Eye_Left) ? 0 : 1;
    if (!lieActiveFor(s, e)) return false;
    return cropFractions(s, e, out);
}

bool systemHookSubmitCopyMode() {
    State* s = g_state;
    return s ? s->submitCopy : true;
}

void systemHookGuardStandDown(const char* why) {
    State* s = g_state;
    if (!s || s->guardInert) return;
    s->guardInert = true;
    Log::get().note(
        "cull guard STANDING DOWN: %s. The lie ends at the next frame "
        "boundary and the game sees true projections for the rest of the "
        "session. Please report this log.",
        why ? why : "a submit-side failure");
}

bool systemHookSizeProbeWanted() {
    State* s = g_state;
    return s && s->stage == 1 && !s->guardInert;
}

void systemHookNoteSubmittedSize(vr::EVREye eye, uint32_t w, uint32_t h) {
    State* s = g_state;
    if (!s || !w || !h) return;
    const int e = (eye == vr::Eye_Left) ? 0 : 1;
    // The first report after stage 1 began is the size the game was
    // submitting BEFORE it rebuilt -- the size adoption must move from.
    if (!s->preStageSeeded[e]) {
        s->preStageSeeded[e] = true;
        s->preStageW[e] = w;
        s->preStageH[e] = h;
    }
    s->submittedW[e] = w;
    s->submittedH[e] = h;
}

bool systemHookCropTarget(vr::EVREye eye, uint32_t* w, uint32_t* h) {
    State* s = g_state;
    if (!s || !w || !h) return false;
    const int e = (eye == vr::Eye_Left) ? 0 : 1;
    if (!s->canonicalW[e] || !s->canonicalH[e]) return false;
    *w = s->canonicalW[e];
    *h = s->canonicalH[e];
    return true;
}

bool systemHookRecommendedSize(uint32_t* w, uint32_t* h) {
    State* s = g_state;
    if (!s || !w || !h || !s->trueSizeW || !s->trueSizeH) return false;
    *w = s->trueSizeW;
    *h = s->trueSizeH;
    return true;
}

bool systemHookUnitQualitySize(uint32_t* w, uint32_t* h) {
    State* s = g_state;
    if (!s || !w || !h || !s->trueSizeW || !s->trueSizeH) return false;
    *w = s->trueSizeW;
    *h = s->trueSizeH;
    if (s->stage >= 1 && !s->guardInert) {
        *w = static_cast<uint32_t>(lroundf(static_cast<float>(s->trueSizeW) * s->sizeFactorH));
        *h = static_cast<uint32_t>(lroundf(static_cast<float>(s->trueSizeH) * s->sizeFactorV));
    }
    return true;
}

bool systemHookCropBounds(vr::EVREye eye, const vr::VRTextureBounds_t* in,
                          vr::VRTextureBounds_t* out) {
    State* s = g_state;
    if (!s || !out) return false;
    const int e = (eye == vr::Eye_Left) ? 0 : 1;
    if (!lieActiveFor(s, e)) return false;

    float f[4];
    if (!cropFractions(s, e, f)) return false;

    // Compose within the caller's own span, whichever direction it runs:
    // OpenVR permits flipped bounds, and a fraction applied inside the span
    // respects the flip without knowing about it. Null means the whole
    // texture. v=0 is the TOP of a DirectX texture, which is the t (negative
    // tangent) edge -- the same end the top fraction is measured from.
    vr::VRTextureBounds_t base = {0.0f, 0.0f, 1.0f, 1.0f};
    if (in) base = *in;
    const float du = base.uMax - base.uMin;
    const float dv = base.vMax - base.vMin;
    out->uMin = base.uMin + f[0] * du;
    out->uMax = base.uMin + f[2] * du;
    out->vMin = base.vMin + f[1] * dv;
    out->vMax = base.vMin + f[3] * dv;
    ++s->cropsApplied;
    return true;
}

void maybeObserveSystemInterface(void* iface, const char* interfaceVersion) {
    if (!iface || !interfaceVersion) return;

    if (!g_state) g_state = new State();
    State& s = *g_state;
    if (s.hook.attached()) return;  // first interface wins, like the compositor
    if (s.refusedThisSession) return;

    Config& cfg = Config::get();
    // Install-time read on purpose, with no reload twin: whether observation
    // runs is not something to tune from inside a headset, and a half-session
    // of data is worse than none or all.
    if (!cfg.getBool("advanced.observe_projection", true)) return;

    if (strcmp(interfaceVersion, "IVRSystem_012") != 0) {
        // Exactly _012, and the refusal is the safety feature: the slot
        // indices AND the slot ABI are that generation's. A game update that
        // moves forward gets observed only after someone re-verifies both.
        Log::get().note(
            "IVRSystem observation NOT installed: the game asked for %s and "
            "this build only knows the _012 layout (slots and ABI both). "
            "Please report the version string.",
            interfaceVersion);
        s.refusedThisSession = true;
        return;
    }

    if (!s.sentinel) s.sentinel = new Sentinel(cfg.logDir().c_str(), L"system_hook");
    if (s.sentinel->trippedOnStartup() &&
        !cfg.getBool("advanced.ignore_sentinel", false)) {
        s.refusedThisSession = true;
        s.sentinel->clearTrip();
        Log::get().note(
            "SENTINEL TRIPPED: the previous run armed the IVRSystem hook and "
            "never confirmed it. Skipping it for THIS session only; it will "
            "try again next launch. The cull guard needs it, so that is off "
            "too for this session.");
        return;
    }

    if (!s.hook.attach(iface)) {
        Log::get().note("IVRSystem vtable attach failed; observation off");
        return;
    }
    const size_t prefix = s.hook.executablePrefix();
    if (prefix <= kSlotEyeToHead) {
        Log::get().note(
            "IVRSystem vtable has %zu plausible methods, need >%zu; observation "
            "off",
            prefix, kSlotEyeToHead);
        s.hook.uninstall();
        return;
    }

    if (!s.sentinel->arm()) {
        Log::get().note("NOTE: the crash sentinel could not be written, so a "
                        "crash in this hook will not disable it next launch.");
    }

    // The guard's mode, read NOW because it decides slot 1's mechanism: a
    // guard armed at startup needs the member-shaped receiver there (the
    // matrix must be EDITABLE, and the phase-0 field capture verified every
    // caller is member-convention on both rigs); a session with the guard
    // off keeps the phase-0 asm thunk, which has no opinion about
    // conventions at all. Decided once -- swapping mechanisms on a live
    // slot mid-session buys nothing and risks a torn frame.
    systemHookConfigure();
    // The temporal pass's jitter needs the same receiver: a shift told
    // through the raw tangents alone would leave the matrix the game
    // renders through un-shifted. Read here, once, like the guard's mode.
    {
        const std::string taa = cfg.getString("fix.temporal_aa", "off");
        s.receiverForTemporal = !taa.empty() && _stricmp(taa.c_str(), "off") != 0;
    }
    // The receiver is in from EVERY launch since 2026-09-04, whatever the
    // settings say. Idle, it forwards the matrix unchanged, notes the
    // planes and runs its formula check once, and that is all. Until then
    // it installed only when the guard or the temporal pass was on at
    // launch, so a key turned on later ran without its half of the edit --
    // the pass as a smoother with no jitter, the guard not at all -- and
    // the settings window had to call both "restart". The convention it
    // relies on is the game's own, verified by the phase-0 capture, and
    // the guard and the pass have trusted it on every rig that ran them.
    // advanced.projection_edit = off keeps the observation thunk in the
    // slot instead, for a rig where the receiver misbehaves; a feature on
    // at launch still gets the receiver, as before.
    const std::string editKey = cfg.getString("advanced.projection_edit", "on");
    const bool editWanted = editKey.empty() || _stricmp(editKey.c_str(), "off") != 0;
    const bool wantReceiver =
        editWanted || s.modeRequested != GuardMode::Off || s.receiverForTemporal;

    void* matrixEntry;
    if (wantReceiver) {
        auto pmf = &MatrixReceiver::GetProjectionMatrix;
        static_assert(sizeof(pmf) == sizeof(void*),
                      "MatrixReceiver must stay single-inheritance");
        memcpy(&matrixEntry, &pmf, sizeof(matrixEntry));
    } else {
        matrixEntry = reinterpret_cast<void*>(&edvr_sysThunkMatrix);
    }

    // InPlace, never CopyVptr: under OpenComposite this object is another
    // mod's own (re-pointing its vptr is the issue-#6 failure), and behind
    // real SteamVR nothing has ever been seen contesting IVRSystem's table.
    // No reclaim pass either -- a stolen slot here loses log lines (or, with
    // the guard live, quietly returns the culling bug), and reclaim exists
    // for slots whose loss the caller can measure; nothing here counts
    // silence the way Submit does.
    bool ok = s.hook.replace(kSlotSize,
                             reinterpret_cast<void*>(&hookedGetRecommendedRenderTargetSize),
                             &edvr_sysOrig[kSlotSize]);
    ok = ok && s.hook.replace(kSlotMatrix, matrixEntry,
                              &edvr_sysOrig[kSlotMatrix]);
    ok = ok && s.hook.replace(kSlotRaw,
                              reinterpret_cast<void*>(&hookedGetProjectionRaw),
                              &edvr_sysOrig[kSlotRaw]);
    ok = ok && s.hook.replace(kSlotEyeToHead,
                              reinterpret_cast<void*>(&edvr_sysThunkEyeToHead),
                              &edvr_sysOrig[kSlotEyeToHead]);
    if (!ok || !s.hook.commit()) {
        Log::get().note("IVRSystem vtable stage/commit failed; observation off");
        s.hook.uninstall();
        s.sentinel->confirm();
        return;
    }

    s.ownerIface = iface;
    s.receiverInstalled = wantReceiver;
    s.installed = true;
    if (wantReceiver) {
        // The asm capture path is not running for slot 1 this session; its
        // report would wait forever, so retire it.
        s.capLogged[kSlotMatrix] = true;
    }
    Log::get().note(
        "IVRSystem_012 hook installed (slots 0/1/2/4): observation for the "
        "terrain culling investigation (frontier issue 72609)%s. Every call "
        "is forwarded; nothing changes until a go-live line says so.",
        wantReceiver
            ? (s.modeRequested != GuardMode::Off
                   ? ", and the CULL GUARD is armed -- once both eyes' true "
                     "projections are read and checked, the game will be told "
                     "a wider frustum and the image cropped back at submit"
                   : ", with the matrix receiver in place (the cull guard and "
                     "the temporal pass's jitter edit the projection through "
                     "it, and it is idle until one of them is on)")
            : "");
}

void systemHookSetJitter(float dx, float dy, bool live) {
    State* s = g_state;
    if (!s) return;
    s->jitDx = live ? dx : 0.0f;
    s->jitDy = live ? dy : 0.0f;
    s->jitLive = live && s->receiverInstalled;
}

uint32_t systemHookProjectionReads() {
    return edvr_sysCounts[kSlotMatrix] + edvr_sysCounts[kSlotRaw];
}

int systemHookJitterVerdict() {
    State* s = g_state;
    if (!s || !s->installed || s->inert || !s->receiverInstalled) return -1;
    if (!s->matrixChecked[0] || !s->matrixChecked[1]) return 0;
    return (s->matrixFormulaOk[0] && s->matrixFormulaOk[1]) ? 1 : -1;
}

bool systemHookJitterAvailable() {
    return systemHookJitterVerdict() > 0;
}

bool systemHookEffectiveTangents(vr::EVREye eye, float out[4]) {
    State* s = g_state;
    if (!s || !out) return false;
    const int e = (eye == vr::Eye_Left) ? 0 : 1;
    if (!s->trueSeen[e]) return false;
    memcpy(out, lieActiveFor(s, e) ? s->lied[e] : s->trueRaw[e], sizeof(float) * 4);
    return true;
}

// GetEyeToHeadTransform through the ORIGINAL entry (slot 4, which the hook
// counts and otherwise leaves alone), member-shaped like the matrix
// forward: the runtime returns a 3x4 by value, which on x64 travels
// through a hidden pointer a member-function call supplies.
typedef vr::HmdMatrix34_t (SysIfaceTag::*PMF_EyeToHead)(int32_t);
static_assert(sizeof(PMF_EyeToHead) == sizeof(void*),
              "a single-inheritance member pointer must be one code pointer");

bool systemHookEyeToHead(vr::EVREye eye, float out[12]) {
    State* s = g_state;
    const int e = (eye == vr::Eye_Left) ? 0 : 1;
    if (!s || !s->installed || s->inert || !s->ownerIface || !out) return false;
    if (!s->eyeToHeadTried[e]) {
        s->eyeToHeadTried[e] = true;
        if (!edvr_sysOrig[kSlotEyeToHead]) return false;
        vr::HmdMatrix34_t m{};
        bool got = false;
        guarded("sysHook/eyeToHead", [&] {
            PMF_EyeToHead f;
            memcpy(&f, &edvr_sysOrig[kSlotEyeToHead], sizeof(f));
            m = (reinterpret_cast<SysIfaceTag*>(s->ownerIface)->*f)(static_cast<int32_t>(e));
            got = true;
        });
        if (!got) return false;
        float rows[12];
        for (int r = 0; r < 3; ++r) {
            for (int c = 0; c < 4; ++c) rows[r * 4 + c] = m.m[r][c];
        }
        // A rigid transform with a small offset, or nothing: the runtime's
        // eye-to-head is a cant and half an IPD, never more than a few cm.
        float rot[9];
        for (int r = 0; r < 3; ++r) {
            for (int c = 0; c < 3; ++c) rot[r * 3 + c] = rows[r * 4 + c];
        }
        bool sane = true;
        for (int r = 0; r < 3; ++r) {
            const float n = rot[r * 3] * rot[r * 3] + rot[r * 3 + 1] * rot[r * 3 + 1] +
                            rot[r * 3 + 2] * rot[r * 3 + 2];
            if (fabsf(n - 1.0f) > 1e-3f) sane = false;
        }
        const float tx = rows[3], ty = rows[7], tz = rows[11];
        if (fabsf(tx) > 0.1f || fabsf(ty) > 0.1f || fabsf(tz) > 0.1f) sane = false;
        if (!sane) {
            Log::get().note(
                "IVRSystem: GetEyeToHeadTransform for the %s eye is not a rigid "
                "transform with a small offset (row norms off or a translation "
                "over 10 cm); the depth reprojection will run without an eye "
                "offset. Please report this log.",
                e == 0 ? "left" : "right");
            return false;
        }
        memcpy(s->eyeToHead[e], rows, sizeof(rows));
        s->eyeToHeadValid[e] = true;
        Log::get().note(
            "IVRSystem: the %s eye sits at (%.4f, %.4f, %.4f) m from the head "
            "(GetEyeToHeadTransform, asked once through the original entry) -- "
            "the offset the depth reprojection carries.",
            e == 0 ? "left" : "right", static_cast<double>(tx),
            static_cast<double>(ty), static_cast<double>(tz));
    }
    if (!s->eyeToHeadValid[e]) return false;
    memcpy(out, s->eyeToHead[e], sizeof(float) * 12);
    return true;
}

bool systemHookNearFar(float* nearZ, float* farZ) {
    State* s = g_state;
    if (!s || !(s->nearZ > 0.0f) || !(s->farZ > s->nearZ)) return false;
    if (nearZ) *nearZ = s->nearZ;
    if (farZ) *farZ = s->farZ;
    return true;
}

void* systemInterfaceV012() {
    State* s = g_state;
    if (!s || s->inert) return nullptr;
    return s->ownerIface;
}

size_t systemInterfacePrefixV012() {
    State* s = g_state;
    if (!s || s->inert || !s->ownerIface) return 0;
    return s->hook.executablePrefix();
}

void shutdownSystemHook() {
    State* s = g_state;
    if (!s) return;
    if (s->installed) {
        Log::get().note(
            "IVRSystem totals: size %u, projMatrix %u, projRaw %u, eyeToHead "
            "%u calls%s; cull guard cropped %u eye-submits by copy, %u by "
            "narrowed bounds%s.",
            edvr_sysCounts[kSlotSize], edvr_sysCounts[kSlotMatrix],
            edvr_sysCounts[kSlotRaw], edvr_sysCounts[kSlotEyeToHead],
            s->inert ? " (observation went inert; counts kept running)" : "",
            guardCropCopies(), s->cropsApplied,
            s->guardInert ? " (guard went INERT -- see the line above where)"
                          : "");
    }
    guardCropShutdown();
    s->hook.uninstall();
    if (s->sentinel) s->sentinel->confirm();
}

}  // namespace edvr

extern "C" unsigned int edvr_selftest_system_hook(void) {
    edvr::State* s = edvr::g_state;
    unsigned int v = 0;
    if (s && s->installed) v |= 1u;
    if (s && s->validated) v |= 2u;
    if (s && s->inert) v |= 4u;
    auto sat = [](uint32_t c) -> unsigned int { return c > 255 ? 255u : c; };
    v |= sat(edvr_sysCounts[1]) << 8;
    v |= sat(edvr_sysCounts[4]) << 16;
    v |= sat(edvr_sysCounts[2]) << 24;
    return v;
}

extern "C" unsigned int edvr_selftest_cull_guard(int eye, float out[4]) {
    edvr::State* s = edvr::g_state;
    if (!s || !out || eye < 0 || eye > 1) return 0;
    if (!edvr::lieActiveFor(s, eye)) return 0;
    return edvr::cropFractions(s, eye, out) ? 1u : 0u;
}
