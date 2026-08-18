#include "system_hook.h"

#include <windows.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>

#include "../common/config.h"
#include "../common/guard.h"
#include "../common/log.h"
#include "../common/timing.h"
#include "../common/vtable_hook.h"
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
    bool      receiverInstalled = false;  // slot 1 = member receiver, not asm
    bool      restartNoted = false;       // "enable needs a restart", said once
    bool      guardInert = false;         // formula or shape failed; truth only
    bool      lieLive = false;            // flips ONLY at frame boundaries
    bool      liePending = false;         // prerequisites met, awaiting boundary
    uint64_t  liePendingSinceMs = 0;
    uint64_t  lastBoundaryMs = 0;         // 0 = no boundary driver seen
    uint32_t  cropsApplied = 0;           // eye-submits cropped
    bool      matrixFormulaOk[2] = {};
    bool      matrixChecked[2] = {};
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
bool computeLied(GuardMode mode, float pct, const float t[4], float out[4]) {
    if (mode == GuardMode::Symmetric) {
        const float h = fabsf(t[0]) > fabsf(t[1]) ? fabsf(t[0]) : fabsf(t[1]);
        const float v = fabsf(t[2]) > fabsf(t[3]) ? fabsf(t[2]) : fabsf(t[3]);
        out[0] = -h;
        out[1] = h;
        out[2] = -v;
        out[3] = v;
    } else {
        const float f = 1.0f + pct / 100.0f;
        for (int i = 0; i < 4; ++i) out[i] = t[i] * f;
    }
    for (int i = 0; i < 4; ++i) {
        if (!std::isfinite(out[i]) || fabsf(out[i]) > 20.0f) return false;
    }
    return out[0] < out[1] && out[2] < out[3];
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
        s->sizeW = *w;
        s->sizeH = *h;
        s->sizeDirty = true;
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
            if (!computeLied(s->modeRequested, s->percent, s->trueRaw[eye], lie)) {
                s->guardInert = true;
                Log::get().note(
                    "cull guard INERT: the %s-mode frustum derived from tangents "
                    "l=%g r=%g t=%g b=%g is not sane. Truth only from here; "
                    "report this log.",
                    modeName(s->modeRequested), lv, rv, tv, bv);
                return;
            }
            memcpy(s->lied[eye], lie, sizeof(lie));
            if (!s->lieLive && !s->liePending && s->trueSeen[0] && s->trueSeen[1]) {
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

    if (lieActiveFor(s, eye) && s->matrixFormulaOk[eye]) {
        const float* lie = s->lied[eye];
        const float du = lie[1] - lie[0], dv = lie[3] - lie[2];
        if (du > 1e-4f && dv > 1e-4f) {
            m.m[0][0] = 2.0f / du;
            m.m[0][2] = (lie[1] + lie[0]) / du;
            m.m[1][1] = 2.0f / dv;
            m.m[1][2] = (lie[3] + lie[2]) / dv;
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
bool cropFractions(const State* s, int eye, float out[4]) {
    const float* t = s->trueRaw[eye];
    const float* lie = s->lied[eye];
    const float du = lie[1] - lie[0], dv = lie[3] - lie[2];
    if (du < 1e-4f || dv < 1e-4f) return false;
    out[0] = (t[0] - lie[0]) / du;   // left
    out[1] = (t[2] - lie[2]) / dv;   // top
    out[2] = (t[1] - lie[0]) / du;   // right
    out[3] = (t[3] - lie[2]) / dv;   // bottom
    return true;
}

// The lie goes live (or dies) HERE and only here, so one frame's raw
// answers, matrix answers and submit crop agree. Callers: the frame
// boundary, and periodic's fallback for boundary-less processes.
void promoteOrDemote(State* s) {
    if (s->lieLive &&
        (s->guardInert || s->modeRequested == GuardMode::Off)) {
        s->lieLive = false;
        s->liePending = false;
        Log::get().note(
            "cull guard OFF%s: the game sees true projections again from this "
            "frame.",
            s->guardInert ? " (inert)" : "");
        return;
    }
    if (!s->lieLive && s->liePending && !s->guardInert &&
        s->modeRequested != GuardMode::Off) {
        s->liePending = false;
        s->lieLive = true;
        for (int eye = 0; eye < 2; ++eye) {
            const float* t = s->trueRaw[eye];
            const float* lie = s->lied[eye];
            float f[4] = {};
            cropFractions(s, eye, f);
            Log::get().note(
                "cull guard LIVE (%s): %s eye true l=%+.4f r=%+.4f t=%+.4f "
                "b=%+.4f -> reported l=%+.4f r=%+.4f t=%+.4f b=%+.4f (FOV "
                "%.1fx%.1f -> %.1fx%.1f deg); submit keeps u %.3f..%.3f, "
                "v %.3f..%.3f of the rendered image.",
                modeName(s->modeRequested), eyeName(eye), t[0], t[1], t[2], t[3],
                lie[0], lie[1], lie[2], lie[3],
                degrees(t[0]) + degrees(t[1]), degrees(t[2]) + degrees(t[3]),
                degrees(lie[0]) + degrees(lie[1]),
                degrees(lie[2]) + degrees(lie[3]),
                f[0], f[2], f[1], f[3]);
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
            emitValues(s);
            if (!s->receiverInstalled) {
                logCaptureIfReady(s, kSlotMatrix, "GetProjectionMatrix");
            }
            logCaptureIfReady(s, kSlotEyeToHead, "GetEyeToHeadTransform");
            emitSummary(s);
            // The boundary-less fallback: a process with no compositor hook
            // (the test harness) has no frame boundary, so after two quiet
            // seconds the promotion happens here instead. In the game the
            // boundary fires every frame and this never runs.
            if (s->liePending && !s->lieLive && s->lastBoundaryMs == 0 &&
                elapsedMs(s->liePendingSinceMs, kBoundaryFallbackMs)) {
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

    const GuardMode before = s->modeRequested;
    const float pctBefore = s->percent;
    s->modeRequested = mode;
    s->percent = pct;

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
    if (before != mode || (mode == GuardMode::Percent &&
                           fabsf(pctBefore - pct) > 0.01f)) {
        if (s->receiverInstalled || mode == GuardMode::Off) {
            Log::get().note(
                "cull guard config: mode %s%s (was %s). Changes take effect at "
                "the next frame boundary.",
                modeName(mode),
                mode == GuardMode::Percent ? " (see cull_guard_percent)" : "",
                modeName(before));
        }
    }
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
    const bool wantReceiver = s.modeRequested != GuardMode::Off;

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
            ? ", and the CULL GUARD is armed -- once both eyes' true "
              "projections are read and checked, the game will be told a "
              "wider frustum and the image cropped back at submit"
            : "");
}

void shutdownSystemHook() {
    State* s = g_state;
    if (!s) return;
    if (s->installed) {
        Log::get().note(
            "IVRSystem totals: size %u, projMatrix %u, projRaw %u, eyeToHead "
            "%u calls%s; cull guard cropped %u eye-submits%s.",
            edvr_sysCounts[kSlotSize], edvr_sysCounts[kSlotMatrix],
            edvr_sysCounts[kSlotRaw], edvr_sysCounts[kSlotEyeToHead],
            s->inert ? " (observation went inert; counts kept running)" : "",
            s->cropsApplied,
            s->guardInert ? " (guard went INERT -- see the line above where)"
                          : "");
    }
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
