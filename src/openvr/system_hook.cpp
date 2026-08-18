#include "system_hook.h"

#include <windows.h>

#include <cmath>
#include <cstdint>
#include <cstring>

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
// in VR. The working hypothesis is that the game's cull frustum is narrower
// than its render frustum, and that rigs whose projection path overscans
// (parallel projections, OpenComposite translation) hide the shortfall while
// exact-FOV SteamVR rigs show it. Deciding that needs facts only the live
// game can give: WHICH projection calls Elite makes, HOW OFTEN, and with WHAT
// values, on both kinds of rig.
//
// So this hooks IVRSystem_012 -- the generation this game links (established
// by scanning EliteDangerous64.exe for IVR*_nnn literals) -- and OBSERVES.
// Nothing is modified; every call is forwarded untouched.
//
// Two kinds of slot, two mechanisms, and the split is load-bearing:
//
//   Slots 0 (GetRecommendedRenderTargetSize) and 2 (GetProjectionRaw) return
//   nothing by value. For such methods the member and free-function calling
//   conventions agree position-for-position, so a C thunk with an explicit
//   `self` first parameter receives them correctly -- proven by a compiled
//   experiment (EVIDENCE 6bo), not assumed. These are hooked in C and their
//   values decoded in full: slot 2 is the per-eye tangent data this
//   investigation exists to collect.
//
//   Slots 1 (GetProjectionMatrix) and 4 (GetEyeToHeadTransform) return
//   structs by value, and there the conventions DISAGREE about where the
//   hidden return pointer rides. The same experiment showed the failure is
//   the quiet kind: values still round-trip (callers read the result back
//   through RAX), while the struct is written over whatever the first
//   pointer names -- the interface object itself, whose vptr dies. That is
//   also the corrected reading of EVIDENCE 4.2/4.3: the archived probe's
//   free-style calls read true matrices out of vrclient while silently
//   clobbering vrclient's IVRSystem object. A C thunk of either shape would
//   do the same to whichever caller convention it does not match, so these
//   two slots are observed by register-preserving asm tail-jump thunks
//   (system_thunks.asm) that count the call and spill the argument registers
//   once, decoding nothing in-line. Which register holds the interface
//   pointer tells us the caller's convention from the field -- the fact any
//   phase-1 value hook on these slots would have to be built on.
//
// IVRSystem is still never CALLED from inside the game process (the 6c rule).
// Receiving calls the game makes is not calling; the ban stands.
// ---------------------------------------------------------------------------

// The asm contract. Everything the thunks touch is defined here, C-named, and
// the offsets they hardcode are asserted below.
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

struct State {
    VTableHook hook;
    // Identity token only, like the compositor's: patching entries in place
    // hooks every object sharing the table, so values are recorded only for
    // the interface WE were handed and everything else forwards untouched.
    void*     ownerIface = nullptr;
    Sentinel* sentinel = nullptr;
    bool      refusedThisSession = false;

    bool installed = false;
    bool inert = false;        // observation off; forwarding continues
    bool validated = false;    // slot 2 has produced one sane reading
    bool confirmed = false;    // sentinel released after surviving a frame

    // Written by the C thunks (whatever thread the game calls from), read by
    // periodic() on the compositor's frame thread. Plain stores on purpose: a
    // torn float costs one odd log line in an instrument, and any locking
    // here would be more machinery than the data. The `dirty` flags are the
    // handshake; x86 store ordering makes them arrive after the values.
    uint32_t sizeW = 0, sizeH = 0;
    bool     sizeDirty = false;
    float    raw[2][4] = {};       // per eye: l r t b, as last observed
    bool     rawSeen[2] = {};
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

// ---------------------------------------------------------------------------
// The C thunks. Shape proven by EVIDENCE 6bo cell 1: no struct return, so a
// member-convention caller lands here with self/args in exactly these
// positions. Forward first, observe after -- the observation must never be
// able to delay or damage the answer the game gets.
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
            Log::get().note(
                "IVRSystem observation going INERT: GetProjectionRaw's arguments "
                "do not look like an eye and four tangent pointers (eye=%d l=%p "
                "r=%p t=%p b=%p). The slot table is wrong for this runtime's "
                "IVRSystem_012; calls keep forwarding untouched, nothing more is "
                "recorded. Please report this log.",
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
                Log::get().note(
                    "IVRSystem observation going INERT: GetProjectionRaw returned "
                    "l=%g r=%g t=%g b=%g, which is not a projection. The slot "
                    "table is wrong for this runtime; calls keep forwarding "
                    "untouched. Please report this log.",
                    lv, rv, tv, bv);
                return;
            }
            s->validated = true;
        }
        s->raw[eye][0] = lv;
        s->raw[eye][1] = rv;
        s->raw[eye][2] = tv;
        s->raw[eye][3] = bv;
        s->rawSeen[eye] = true;
        s->rawDirty[eye] = true;
    });
    systemHookPeriodic();
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

    // The convention verdict, which is the fact a phase-1 value hook on the
    // struct-returning slots would stand on. A C++ member call carries `this`
    // first and the return slot second; the free-style shape carries the
    // return slot first. Whichever of the two pointer registers equals the
    // interface is the position `this` was passed in.
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
        memcpy(v, s->raw[eye], sizeof(v));
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
            "IVRSystem: GetProjectionRaw(%s eye) tangents l=%+.4f r=%+.4f "
            "t=%+.4f b=%+.4f -> rendered FOV %.1f+%.1f = %.1f deg horizontal, "
            "%.1f+%.1f = %.1f deg vertical%s",
            eyeName(eye), v[0], v[1], v[2], v[3],
            degrees(v[0]), degrees(v[1]), degrees(v[0]) + degrees(v[1]),
            degrees(v[2]), degrees(v[3]), degrees(v[2]) + degrees(v[3]),
            s->lastRawSeen[eye] ? " -- CHANGED" : "");
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
            logCaptureIfReady(s, kSlotMatrix, "GetProjectionMatrix");
            logCaptureIfReady(s, kSlotEyeToHead, "GetEyeToHeadTransform");
            emitSummary(s);
        });
    }

    InterlockedExchange(&g_periodicBusy, 0);
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
            "SENTINEL TRIPPED: the previous run armed the IVRSystem observation "
            "and never confirmed it. Skipping it for THIS session only; it will "
            "try again next launch. Everything else runs normally -- this hook "
            "only records, so nothing visible is lost with it off.");
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
                        "crash in this observation will not disable it next "
                        "launch.");
    }

    // InPlace, never CopyVptr: under OpenComposite this object is another
    // mod's own (re-pointing its vptr is the issue-#6 failure), and behind
    // real SteamVR nothing has ever been seen contesting IVRSystem's table.
    // No reclaim pass either -- a stolen slot here loses log lines, not
    // correctness, and reclaim exists for slots whose LOSS changes what the
    // player sees.
    bool ok = s.hook.replace(kSlotSize,
                             reinterpret_cast<void*>(&hookedGetRecommendedRenderTargetSize),
                             &edvr_sysOrig[kSlotSize]);
    ok = ok && s.hook.replace(kSlotMatrix,
                              reinterpret_cast<void*>(&edvr_sysThunkMatrix),
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
    s.installed = true;
    Log::get().note(
        "IVRSystem_012 observation installed (slots 0/1/2/4): recording what "
        "the game asks about your headset's projection, for the terrain "
        "culling investigation (frontier issue 72609). Render-target sizes "
        "and lens tangents are logged in full; the two struct-returning calls "
        "are counted, with one register snapshot each. Nothing is changed and "
        "every call is forwarded untouched.");
}

void shutdownSystemHook() {
    State* s = g_state;
    if (!s) return;
    if (s->installed) {
        Log::get().note(
            "IVRSystem observation totals: size %u, projMatrix %u, projRaw %u, "
            "eyeToHead %u calls%s",
            edvr_sysCounts[kSlotSize], edvr_sysCounts[kSlotMatrix],
            edvr_sysCounts[kSlotRaw], edvr_sysCounts[kSlotEyeToHead],
            s->inert ? " (observation went inert; counts kept running)" : "");
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
