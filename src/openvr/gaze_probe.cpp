#include "gaze_probe.h"

#include <windows.h>

#include <cmath>
#include <cstring>

#include "../common/config.h"
#include "../common/guard.h"
#include "../common/log.h"
#include "launch_centre.h"
#include "system_hook.h"

namespace edvr {
namespace {

// The version this file speaks and the entries it uses, transcribed from
// Valve's openvr_capi.h at SDK 2.15.6 -- the SDK whose IVRSystem_Version IS
// "IVRSystem_026". VR_IVRSystem_FnTable there is 51 function pointers in
// declaration order; the four named below are the only ones called.
//
// The version string and the indices come from ONE header on purpose: the
// 6c rule's incident (system_hook.cpp) indexed an old interface from a new
// header, and the layout had moved between them. A future SDK that
// renumbers IVRSystem gets a new version string with it, and this table
// stays true for the string it names.
constexpr const char* kFnTableVersion = "FnTable:IVRSystem_026";
constexpr size_t kFnRecommendedSize = 0;             // GetRecommendedRenderTargetSize
constexpr size_t kFnFoveationCenter = 35;            // GetEyeTrackedFoveationCenter
constexpr size_t kFnFoveationCenterForProjection = 36;
constexpr size_t kFnRuntimeVersion = 49;             // GetRuntimeVersion
constexpr size_t kFnTableEntries = 51;
static_assert(kFnRuntimeVersion < kFnTableEntries, "index inside the table");

// OPENVR_FNTABLE_CALLTYPE is __stdcall, which x64 accepts and ignores: one
// convention, no hidden `this`, no hidden return slot for any of these.
typedef void (__stdcall* PFN_RecommendedSize)(uint32_t* w, uint32_t* h);
typedef bool (__stdcall* PFN_FoveationCenter)(vr::HmdVector2_t* ndcLeft,
                                              vr::HmdVector2_t* ndcRight);
typedef bool (__stdcall* PFN_FoveationCenterForProjection)(const vr::HmdMatrix44_t* proj,
                                                           vr::HmdVector2_t* ndc);
typedef const char* (__stdcall* PFN_RuntimeVersion)();

PFN_RealGetGenericInterface g_get = nullptr;

bool g_wanted = false;       // advanced.gaze_probe, as read at launch
bool g_configured = false;
bool g_saidChanged = false;

enum class Phase { Waiting, Armed, Off };
Phase g_phase = Phase::Waiting;

// Frames spent waiting for the game to be told its render size, which is
// what the table is validated against. It arrives at the game's first
// IVRSystem call, long before the first frame; the cap exists for a session
// where the observer never saw that call, and the validation then falls
// back to a sanity range and says so.
uint32_t g_waited = 0;
constexpr uint32_t kMaxWaitFrames = 1800;

PFN_FoveationCenter g_center = nullptr;
PFN_FoveationCenterForProjection g_centerProj = nullptr;
Sentinel* g_sentinel = nullptr;

// One eye's centre over a window of frames: range, mean and the mean
// per-frame step. The step is the tell -- a tracker that follows the eyes
// moves every frame by a little; a driver publishing a constant moves by
// exactly nothing.
struct EyeWindow {
    uint32_t n = 0;
    double sumX = 0.0, sumY = 0.0;
    float minX = 0.0f, maxX = 0.0f, minY = 0.0f, maxY = 0.0f;
    double stepSum = 0.0;
    uint32_t stepN = 0;
    float lastX = 0.0f, lastY = 0.0f;
    bool haveLast = false;

    void add(float x, float y) {
        if (n == 0) {
            minX = maxX = x;
            minY = maxY = y;
        } else {
            if (x < minX) minX = x;
            if (x > maxX) maxX = x;
            if (y < minY) minY = y;
            if (y > maxY) maxY = y;
        }
        ++n;
        sumX += x;
        sumY += y;
        if (haveLast) {
            stepSum += sqrt(static_cast<double>((x - lastX) * (x - lastX) +
                                                (y - lastY) * (y - lastY)));
            ++stepN;
        }
        lastX = x;
        lastY = y;
        haveLast = true;
    }
    // A new window: the counts restart, the last sample bridges so the
    // first step of the next window is real.
    void reset() {
        n = 0;
        sumX = sumY = 0.0;
        minX = maxX = minY = maxY = 0.0f;
        stepSum = 0.0;
        stepN = 0;
    }
    double meanX() const { return n ? sumX / n : 0.0; }
    double meanY() const { return n ? sumY / n : 0.0; }
    double meanStep() const { return stepN ? stepSum / stepN : 0.0; }
};

struct Counts {
    uint32_t asked = 0;      // frames the runtime was asked
    uint32_t valid = 0;      // it vouched, and the answer had a shape
    uint32_t declined = 0;   // it returned false: no tracker, or a blink
    uint32_t malformed = 0;  // it vouched for something that is not a point
};

Counts g_win, g_total;
EyeWindow g_eye[2], g_eyeTotal[2];
uint32_t g_frames = 0;          // frames since arming
uint32_t g_summaries = 0;
uint32_t g_nextSummary = 600;   // a quick first verdict, then a minute apart
constexpr uint32_t kSummaryEvery = 5400;
constexpr uint32_t kSummaryCap = 40;
bool g_firstValidSaid = false;

// The projection variant, checked against the plain answer now and then:
// with the runtime's own projection the two must agree, which validates
// the second entry and pins the NDC convention. Under a live guard the
// tangents the game is told are the lied ones, so a difference there is
// the feature, not a fault.
double g_projMaxDiff = 0.0;
uint32_t g_projN = 0;
uint32_t g_projDeclined = 0;

void off(const char* why) {
    g_phase = Phase::Off;
    Log::get().note("gaze probe: OFF for the session -- %s", why);
}

bool wellFormed(const vr::HmdVector2_t& v) {
    return std::isfinite(v.v[0]) && std::isfinite(v.v[1]) && fabsf(v.v[0]) <= 2.0f &&
           fabsf(v.v[1]) <= 2.0f;
}

// Valve's ComposeProjection, from the raw tangents in the order the system
// hook keeps them (l, r, t, b); the near and far rows are filled for form,
// the centre depends only on the first two.
bool composeProjection(const float t4[4], vr::HmdMatrix44_t* out) {
    const float l = t4[0], r = t4[1], t = t4[2], b = t4[3];
    if (!(r > l) || !(b > t) || !std::isfinite(l) || !std::isfinite(r) ||
        !std::isfinite(t) || !std::isfinite(b)) {
        return false;
    }
    const float n = 0.1f, f = 1000.0f;
    const float idx = 1.0f / (r - l), idy = 1.0f / (b - t), idz = 1.0f / (f - n);
    memset(out, 0, sizeof(*out));
    out->m[0][0] = 2.0f * idx;
    out->m[0][2] = (r + l) * idx;
    out->m[1][1] = 2.0f * idy;
    out->m[1][2] = (b + t) * idy;
    out->m[2][2] = -f * idz;
    out->m[2][3] = -f * n * idz;
    out->m[3][2] = -1.0f;
    return true;
}

void record(bool vouched, const vr::HmdVector2_t& L, const vr::HmdVector2_t& R) {
    ++g_win.asked;
    ++g_total.asked;
    if (!vouched) {
        ++g_win.declined;
        ++g_total.declined;
        return;
    }
    if (!wellFormed(L) || !wellFormed(R)) {
        ++g_win.malformed;
        ++g_total.malformed;
        return;
    }
    ++g_win.valid;
    ++g_total.valid;
    g_eye[0].add(L.v[0], L.v[1]);
    g_eye[1].add(R.v[0], R.v[1]);
    g_eyeTotal[0].add(L.v[0], L.v[1]);
    g_eyeTotal[1].add(R.v[0], R.v[1]);
    if (!g_firstValidSaid) {
        g_firstValidSaid = true;
        Log::get().note(
            "gaze probe: FIRST VALID CENTRE, %u frame(s) after arming -- the runtime "
            "vouched for a per-eye point with a shape (finite, within 2 of the "
            "image centre in NDC). Whether it FOLLOWS the eyes is what the "
            "summaries below say: read the per-frame step and the ranges.",
            g_frames);
    }
}

void checkProjectionVariant(bool vouched, const vr::HmdVector2_t& L) {
    if (!vouched || !g_centerProj) return;
    float t4[4];
    if (!systemHookEffectiveTangents(vr::Eye_Left, t4)) return;
    vr::HmdMatrix44_t proj;
    if (!composeProjection(t4, &proj)) return;
    vr::HmdVector2_t v = {{NAN, NAN}};
    bool ok = false;
    const bool survived =
        guarded("gazeProbe/projection", [&] { ok = g_centerProj(&proj, &v); });
    if (!survived) {
        g_centerProj = nullptr;  // never again this session; the plain call carries on
        Log::get().note("gaze probe: the projection variant FAULTED; the plain call "
                        "carries on alone. Please report this log.");
        return;
    }
    if (!ok) {
        ++g_projDeclined;
        return;
    }
    if (!wellFormed(v)) return;
    const double d = sqrt(static_cast<double>((v.v[0] - L.v[0]) * (v.v[0] - L.v[0]) +
                                              (v.v[1] - L.v[1]) * (v.v[1] - L.v[1])));
    if (d > g_projMaxDiff) g_projMaxDiff = d;
    ++g_projN;
}

void summary(bool final) {
    const Counts& c = final ? g_total : g_win;
    const EyeWindow* e = final ? g_eyeTotal : g_eye;
    const double pct = c.asked ? 100.0 * c.valid / c.asked : 0.0;
    Log::get().note(
        "gaze probe %s: asked %u, valid %u (%.1f%%), declined %u, malformed %u; "
        "left x %.3f..%.3f mean %.3f, y %.3f..%.3f mean %.3f, step %.4f/frame; "
        "right x %.3f..%.3f mean %.3f, y %.3f..%.3f mean %.3f, step %.4f/frame; "
        "projection variant vs plain: max diff %.4f over %u checks, %u declined%s",
        final ? "SESSION TOTALS" : "summary", c.asked, c.valid, pct, c.declined,
        c.malformed, static_cast<double>(e[0].minX), static_cast<double>(e[0].maxX),
        e[0].meanX(), static_cast<double>(e[0].minY), static_cast<double>(e[0].maxY),
        e[0].meanY(), e[0].meanStep(), static_cast<double>(e[1].minX),
        static_cast<double>(e[1].maxX), e[1].meanX(), static_cast<double>(e[1].minY),
        static_cast<double>(e[1].maxY), e[1].meanY(), e[1].meanStep(), g_projMaxDiff,
        g_projN, g_projDeclined,
        final ? "." : ". A step of 0.0000 with valid answers is a driver publishing "
                      "a constant, not a tracker.");
    if (!final) {
        g_win = Counts();
        g_eye[0].reset();
        g_eye[1].reset();
        ++g_summaries;
        g_nextSummary = (g_summaries == 1) ? 1800 : g_nextSummary + kSummaryEvery;
    }
}

void tryArm() {
    if (!g_get) {
        off("the proxy never resolved the real VR_GetGenericInterface");
        return;
    }
    if (!launchCentreRuntimeIsValve()) {
        off("the runtime under this proxy is not Valve's own SteamVR. This asks "
            "for an interface OpenComposite does not implement, and OpenComposite "
            "answers such a request with a fatal dialog, so it is never asked.");
        return;
    }
    uint32_t truthW = 0, truthH = 0;
    const bool haveTruth = systemHookRecommendedSize(&truthW, &truthH);
    if (!haveTruth && ++g_waited < kMaxWaitFrames) return;

    Config& cfg = Config::get();
    g_sentinel = new Sentinel(cfg.logDir().c_str(), L"gaze_probe");
    if (g_sentinel->trippedOnStartup() && !cfg.getBool("advanced.ignore_sentinel", false)) {
        g_sentinel->clearTrip();
        off("SENTINEL TRIPPED: the previous run began this probe's first calls and "
            "never finished them, which usually means it crashed there. Skipped "
            "for this session only; it will try again next launch. If this keeps "
            "happening, set advanced.gaze_probe = off and report the log.");
        return;
    }
    if (!g_sentinel->arm()) {
        Log::get().note("gaze probe: the crash sentinel could not be written, so a "
                        "crash in the first calls will not disable this next launch.");
    }

    vr::EVRInitError err = 0;
    void* table = nullptr;
    bool survived = guarded("gazeProbe/request", [&] { table = g_get(kFnTableVersion, &err); });
    if (!survived) {
        g_sentinel->confirm();
        off("the interface request FAULTED (caught). Please report this log.");
        return;
    }
    if (!table) {
        g_sentinel->confirm();
        Log::get().note(
            "gaze probe: VR_GetGenericInterface(\"%s\") answered error %d (105 is "
            "'interface not found': a SteamVR older than SDK 2.15.6, or one that "
            "does not serve the C table for this version). Nothing to ask.",
            kFnTableVersion, static_cast<int>(err));
        off("no interface");
        return;
    }

    void** fn = static_cast<void**>(table);
    void* pSize = nullptr;
    void* pCenter = nullptr;
    void* pCenterProj = nullptr;
    void* pVersion = nullptr;
    survived = guarded("gazeProbe/table", [&] {
        pSize = fn[kFnRecommendedSize];
        pCenter = fn[kFnFoveationCenter];
        pCenterProj = fn[kFnFoveationCenterForProjection];
        pVersion = fn[kFnRuntimeVersion];
    });
    if (!survived || !pSize || !pCenter || !pCenterProj) {
        g_sentinel->confirm();
        off(survived ? "the table has a null entry where this build expects a function; "
                       "it is not the table this build was written against. Please "
                       "report this log."
                     : "reading the table FAULTED (caught). Please report this log.");
        return;
    }

    // Validation before belief: entry 0 must answer the size the game was
    // told. A table that is not what this build thinks it is answers
    // something else, or nothing, and no gaze entry is called.
    uint32_t w = 0, h = 0;
    survived = guarded("gazeProbe/size", [&] {
        reinterpret_cast<PFN_RecommendedSize>(pSize)(&w, &h);
    });
    if (!survived) {
        g_sentinel->confirm();
        off("entry 0 FAULTED (caught): not the table this build expects. Please "
            "report this log.");
        return;
    }
    const bool sizeOk = haveTruth ? (w == truthW && h == truthH)
                                  : (w >= 64 && h >= 64 && w <= 32768 && h <= 32768);
    if (!sizeOk) {
        g_sentinel->confirm();
        Log::get().note(
            "gaze probe: entry 0 answered %ux%u; the game was told %ux%u%s. The "
            "table is NOT what this build was written against, so no gaze entry "
            "is called. Please report this log.",
            w, h, truthW, truthH, haveTruth ? "" : " (unknown: the system observer never saw the game ask; a sanity range was used instead)");
        off("table validation failed");
        return;
    }

    const char* runtimeVersion = nullptr;
    if (pVersion) {
        guarded("gazeProbe/version", [&] {
            runtimeVersion = reinterpret_cast<PFN_RuntimeVersion>(pVersion)();
        });
    }

    g_center = reinterpret_cast<PFN_FoveationCenter>(pCenter);
    g_centerProj = reinterpret_cast<PFN_FoveationCenterForProjection>(pCenterProj);

    vr::HmdVector2_t L = {{NAN, NAN}}, R = {{NAN, NAN}};
    bool vouched = false;
    survived = guarded("gazeProbe/first", [&] { vouched = g_center(&L, &R); });
    if (!survived) {
        g_sentinel->confirm();
        g_center = nullptr;
        off("the first GetEyeTrackedFoveationCenter call FAULTED (caught). Please "
            "report this log.");
        return;
    }
    g_sentinel->confirm();
    g_phase = Phase::Armed;
    Log::get().note(
        "gaze probe: ARMED on %s (runtime reports itself as \"%s\"). Entry 0 "
        "answered %ux%u, matching what the game was told%s. The first "
        "GetEyeTrackedFoveationCenter call %s. Asking every frame from here; "
        "the first summary comes after 600 frames, then a minute apart. "
        "Nothing per frame is written down.",
        kFnTableVersion, runtimeVersion ? runtimeVersion : "(no version string)", w, h,
        haveTruth ? "" : " (by sanity range only)",
        vouched ? "VOUCHED for a centre" : "DECLINED (false): no tracker, no gaze "
                                            "published by this headset's driver, or "
                                            "not yet -- the summaries say which");
    record(vouched, L, R);
    ++g_frames;
}

void ask() {
    vr::HmdVector2_t L = {{NAN, NAN}}, R = {{NAN, NAN}};
    bool vouched = false;
    const bool survived = guarded("gazeProbe/ask", [&] { vouched = g_center(&L, &R); });
    if (!survived) {
        g_center = nullptr;
        off("a per-frame call FAULTED (caught) after the first had succeeded. "
            "Please report this log.");
        return;
    }
    record(vouched, L, R);
    if ((g_frames % 60) == 0) checkProjectionVariant(vouched, L);
    ++g_frames;
    if (g_summaries < kSummaryCap && g_frames >= g_nextSummary) summary(false);
}

}  // namespace

void gazeProbeNoteGetter(PFN_RealGetGenericInterface get) { g_get = get; }

void gazeProbeConfigure() {
    Config& cfg = Config::get();
    const bool want = cfg.getBool("advanced.gaze_probe", false);
    if (!g_configured) {
        g_configured = true;
        g_wanted = want;
        if (want) {
            Log::get().note(
                "gaze probe: ON (advanced.gaze_probe). Asks SteamVR for the "
                "eye-tracked foveation centre its IVRSystem_026 offers, once a "
                "frame, and logs aggregates. A dev instrument for "
                "docs/performance.md feature 3.");
        }
        return;
    }
    if (want != g_wanted && !g_saidChanged) {
        g_saidChanged = true;
        Log::get().note("gaze probe: advanced.gaze_probe changed to %s; it is read at "
                        "launch only, so this session stays %s.",
                        want ? "on" : "off", g_wanted ? "on" : "off");
    }
}

void gazeProbeApply() {
    if (!g_wanted || g_phase == Phase::Off) return;
    if (g_phase == Phase::Waiting) {
        tryArm();
        return;
    }
    ask();
}

void gazeProbeShutdown() {
    if (g_phase != Phase::Armed) return;
    summary(true);
}

}  // namespace edvr
