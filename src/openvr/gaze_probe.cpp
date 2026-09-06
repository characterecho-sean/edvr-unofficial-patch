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
constexpr size_t kFnSeatedToStanding = 13;           // GetSeatedZeroPoseToStandingAbsoluteTrackingPose
constexpr size_t kFnRawToStanding = 14;              // GetRawZeroPoseToStandingAbsoluteTrackingPose
constexpr size_t kFnFoveationCenter = 35;            // GetEyeTrackedFoveationCenter
constexpr size_t kFnFoveationCenterForProjection = 36;
constexpr size_t kFnRuntimeVersion = 49;             // GetRuntimeVersion
constexpr size_t kFnDeviceToAbsolutePose = 12;       // GetDeviceToAbsoluteTrackingPose (Phase 1: t)
constexpr size_t kFnStringProperty = 28;             // GetStringTrackedDeviceProperty (Phase 1: the driver's name)
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
// A 3x4 returned by value through a plain C function: the compiler supplies
// the hidden return pointer the C way, which is the whole reason this file
// speaks to the table and not the vtable.
typedef vr::HmdMatrix34_t (__stdcall* PFN_SeatedToStanding)();
typedef void (__stdcall* PFN_DeviceToAbsolutePose)(vr::ETrackingUniverseOrigin origin,
                                                   float predictedSeconds,
                                                   vr::TrackedDevicePose_t* poses, uint32_t count);
typedef uint32_t (__stdcall* PFN_StringProperty)(uint32_t deviceIndex, int32_t prop, char* value,
                                                 uint32_t bufferSize, int32_t* error);

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
PFN_SeatedToStanding g_seatedToStanding = nullptr;
PFN_SeatedToStanding g_rawToStanding = nullptr;   // same shape, entry 14
Sentinel* g_sentinel = nullptr;

// p' = M p for a row-major 3x4 (rotation | translation).
void apply34(const vr::HmdMatrix34_t& m, const double p[3], double out[3]) {
    for (int r = 0; r < 3; ++r) {
        out[r] = m.m[r][0] * p[0] + m.m[r][1] * p[1] + m.m[r][2] * p[2] + m.m[r][3];
    }
}
// p' = M^-1 p for a rigid 3x4: R^T (p - t).
void applyInverse34(const vr::HmdMatrix34_t& m, const double p[3], double out[3]) {
    const double d[3] = {p[0] - m.m[0][3], p[1] - m.m[1][3], p[2] - m.m[2][3]};
    for (int c = 0; c < 3; ++c) {
        out[c] = m.m[0][c] * d[0] + m.m[1][c] * d[1] + m.m[2][c] * d[2];
    }
}
// The yaw a rigid transform turns +z by, degrees, for saying whether two
// universes are rotated against each other.
double yawOf34(const vr::HmdMatrix34_t& m) {
    return atan2(static_cast<double>(m.m[0][2]), static_cast<double>(m.m[2][2])) * 57.2957795;
}
// Where a point would land if it were read as a direction in the head's
// frame: yaw/pitch through the projection, or "behind" when z >= 0.
bool pointAngles(const double p[3], double* yawDeg, double* pitchDeg) {
    if (!(p[2] < -0.05)) return false;
    *yawDeg = atan(p[0] / -p[2]) * 57.2957795;
    *pitchDeg = atan(p[1] / -p[2]) * 57.2957795;
    return true;
}

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

// The head over the same window: where it was and which way it faced, so
// the reported centre can be read against it. Position in the runtime's
// tracking space, metres; facing as the device's -z in that space.
struct HeadWindow {
    uint32_t n = 0;
    double sum[3] = {0.0, 0.0, 0.0};
    float  lo[3] = {0.0f, 0.0f, 0.0f}, hi[3] = {0.0f, 0.0f, 0.0f};
    double fwd[3] = {0.0, 0.0, 0.0};   // summed forward vectors
    void add(const vr::HmdMatrix34_t& m) {
        const float p[3] = {m.m[0][3], m.m[1][3], m.m[2][3]};
        for (int i = 0; i < 3; ++i) {
            if (n == 0) lo[i] = hi[i] = p[i];
            if (p[i] < lo[i]) lo[i] = p[i];
            if (p[i] > hi[i]) hi[i] = p[i];
            sum[i] += p[i];
            fwd[i] += -m.m[i][2];   // the device's -z axis, in tracking space
        }
        ++n;
    }
    void reset() {
        n = 0;
        for (int i = 0; i < 3; ++i) { sum[i] = 0.0; fwd[i] = 0.0; lo[i] = hi[i] = 0.0f; }
    }
    double mean(int i) const { return n ? sum[i] / n : 0.0; }
    double span() const {
        double s = 0.0;
        for (int i = 0; i < 3; ++i) if (hi[i] - lo[i] > s) s = hi[i] - lo[i];
        return s;
    }
    // Facing, degrees: yaw + = right (+x), pitch + = up (+y).
    void facing(double* yawDeg, double* pitchDeg) const {
        if (!n) { *yawDeg = *pitchDeg = 0.0; return; }
        const double x = fwd[0] / n, y = fwd[1] / n, z = fwd[2] / n;
        *yawDeg = atan2(x, -z) * 57.2957795;
        *pitchDeg = atan2(y, sqrt(x * x + z * z)) * 57.2957795;
    }
};

Counts g_win, g_total;
EyeWindow g_eye[2], g_eyeTotal[2];
HeadWindow g_head, g_headTotal;
uint32_t g_frames = 0;          // frames since arming
uint32_t g_summaries = 0;
uint32_t g_nextSummary = 600;   // a quick first verdict, then on the cadence
uint32_t g_cadence = 5400;      // advanced.gaze_probe_summary_frames (live)
constexpr uint32_t kSummaryCap = 400;
bool g_firstValidSaid = false;

// The reported centre read back as a direction through the eye's own
// projection (Valve's ComposeProjection, tangents l r t b): NDC x =
// m00 tx - m02, NDC y = m11 ty - m12 for a direction (tx, ty, -1). yaw +
// = right, pitch + = up in OpenVR's NDC. False without the tangents.
bool ndcToDegrees(int eye, double ndcX, double ndcY, double* yawDeg, double* pitchDeg) {
    float t4[4];
    if (!systemHookEffectiveTangents(eye == 0 ? vr::Eye_Left : vr::Eye_Right, t4)) return false;
    const double l = t4[0], r = t4[1], t = t4[2], b = t4[3];
    if (!(r > l) || !(b > t)) return false;
    const double m00 = 2.0 / (r - l), m02 = (r + l) / (r - l);
    const double m11 = 2.0 / (b - t), m12 = (b + t) / (b - t);
    const double tx = (ndcX + m02) / m00, ty = (ndcY + m12) / m11;
    *yawDeg = atan(tx) * 57.2957795;
    *pitchDeg = atan(ty) * 57.2957795;
    return true;
}

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

// The runtime's centre on this rig is d - t, a unit gaze minus the head's
// position in the raw universe, and the raw origin sits metres away (5 m
// one session, 15.6 the next), so the centre projects far outside the image
// -- the flight of 2026-09-05 12:31 read it at 2.9 NDC and a bound of 2
// called every frame malformed. Finite is the shape; 64 fences off garbage.
bool wellFormed(const vr::HmdVector2_t& v) {
    return std::isfinite(v.v[0]) && std::isfinite(v.v[1]) && fabsf(v.v[0]) <= 64.0f &&
           fabsf(v.v[1]) <= 64.0f;
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

bool wellFormedTan(const vr::HmdVector2_t& v) {
    return std::isfinite(v.v[0]) && std::isfinite(v.v[1]) && fabsf(v.v[0]) <= 64.0f &&
           fabsf(v.v[1]) <= 64.0f;
}

// ---------------------------------------------------------------------------
// Phase 1 of docs/eye-tracking.md (2026-09-05): two routes to a usable gaze,
// probed in one flight. Route A repairs the frame SteamVR hands out on this
// rig -- the reported direction is d - t, a unit gaze d minus the headset's
// position t in Pimax's raw universe -- by reading t every frame and
// solving |lambda n + t| = 1 for d. Route B reads the tracker from Pimax's
// own runtime through the client library its SteamVR driver uses. Both
// are aggregates per window, never a sample; both sit behind the guard,
// and Route B behind its own crash sentinel.
// ---------------------------------------------------------------------------

// Entry 36 with a hand-made matrix. SteamVR applies the 4x4 to the gaze and
// divides by w: rows (1,0,0,0) (0,1,0,0) (0,0,1,0) (0,0,-1,0) return the
// direction's tangents (x/-z, y/-z) whatever projection it would otherwise
// pick; with (0,0,0,1) as the last row it returns x and y undivided IF the
// gaze is carried as a point with w = 1, and a third call with (0,0,1,0)
// as the first row then returns z -- the scale, which says whether SteamVR
// normalises. A fault retires the entry for the session, as the periodic
// projection check does.
const float kDirRows[4][4]   = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, -1, 0}};
const float kPointRows[4][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}};
const float kZRows[4][4]     = {{0, 0, 1, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}};

PFN_DeviceToAbsolutePose g_devicePose = nullptr;
PFN_StringProperty g_stringProp = nullptr;
bool g_pointFormTried = false;

bool callProj(const float rows[4][4], vr::HmdVector2_t* out, bool* declined) {
    *declined = false;
    if (!g_centerProj) return false;
    vr::HmdMatrix44_t m{};
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) m.m[r][c] = rows[r][c];
    }
    vr::HmdVector2_t v = {{NAN, NAN}};
    bool ok = false;
    const bool survived = guarded("gazeProbe/projForm", [&] { ok = g_centerProj(&m, &v); });
    if (!survived) {
        g_centerProj = nullptr;
        Log::get().note("gaze probe: entry 36 FAULTED with a hand-made matrix; the direction-form "
                        "read is off for the session. Please report this log.");
        return false;
    }
    *declined = !ok;
    *out = v;
    return ok;
}

// The HMD's pose in the raw universe (entry 12), once a frame: t.
bool readRawHead(double t[3]) {
    if (!g_devicePose) return false;
    vr::TrackedDevicePose_t pose{};
    const bool survived = guarded("gazeProbe/rawPose", [&] {
        g_devicePose(vr::TrackingUniverseRawAndUncalibrated, 0.0f, &pose, 1);
    });
    if (!survived) {
        g_devicePose = nullptr;
        Log::get().note("gaze probe: entry 12 FAULTED; the raw-universe pose is off for the "
                        "session. Please report this log.");
        return false;
    }
    if (!pose.bPoseIsValid) return false;
    for (int i = 0; i < 3; ++i) {
        t[i] = pose.mDeviceToAbsoluteTracking.m[i][3];
        if (!std::isfinite(t[i]) || fabs(t[i]) > 100.0) return false;
    }
    return true;
}

void anglesOf(const double d[3], double* yawDeg, double* pitchDeg) {
    *yawDeg = atan2(d[0], -d[2]) * 57.2957795;
    *pitchDeg = atan2(d[1], sqrt(d[0] * d[0] + d[2] * d[2])) * 57.2957795;
}

// The repair's window: t, the direction-form tangents, the inversion for
// both roots and four sign variants (x mirrored or not, y mirrored or not),
// the far root's frustum share, and the agreement with Route B's tangents.
struct RepairWindow {
    uint32_t n = 0;              // frames with both n and t
    uint32_t noT = 0;            // frames without a raw pose
    uint32_t noN = 0;            // frames the direction-form read declined
    uint32_t neg = 0;            // negative discriminant
    double   discMin = 1e9;
    double   tSum[3] = {0.0, 0.0, 0.0};
    double   tAbsSum = 0.0;
    double   nTanSum[2] = {0.0, 0.0};
    double   dSum[2][4][3] = {};   // [root: 0 far, 1 near][variant][xyz], unit vectors summed
    uint32_t inFrustum[2][4] = {};
    double   pvrDiffSq[2][4] = {}; // squared angular difference to Route B, degrees^2
    uint32_t pvrN = 0;
    // The subtraction (primary since the 12:31 flight): p from the point-form
    // reads carries its scale, so d = p + t.
    uint32_t subN = 0;             // frames with p and t, |d| plausible
    uint32_t noP = 0;              // the point-form reads declined or were not finite
    uint32_t dRejected = 0;        // |d| outside 0.5..2: the model did not hold that frame
    double   pAbsSum = 0.0;
    double   dAbsSum = 0.0, dAbsMin = 0.0, dAbsMax = 0.0;
    double   dSubSum[4][3] = {};
    uint32_t inFrustumSub[4] = {};
    double   pvrDiffSqSub[4] = {};
    uint32_t pvrSubN = 0;
    // The point-form vector's magnitudes over EVERY frame with p and t, so
    // the frame and sign it is handed out in can be read off: which of
    // |p + t| and |p - t| is 1, whether p is parallel to the direction-form
    // n (cos +1) or opposed to it (-1), and the mean vectors themselves.
    uint32_t magN = 0;
    double   pAbs = 0.0, tAbs = 0.0, plusAbs = 0.0, minusAbs = 0.0;
    double   cosPN = 0.0;
    uint32_t cosN = 0;
    double   pMean[3] = {}, tMean[3] = {};
    void reset() { *this = RepairWindow(); }
};
RepairWindow g_repair, g_repairTotal;

// Route B: Pimax's client library, bound by position from PVR_Interface.h
// (interface 1.32; "Copyright 2017 Pimax, Inc. All Rights reserved." -- the
// positions and the one struct are transcribed here with that attribution,
// the header is not vendored). getPvrInterface(1, 32) returns a table of
// function pointers; the seven used: 0 initialise, 1 shutdown, 2 createHmd,
// 3 destroyHmd, 4 getVersionString, 5 getTimeSeconds, 65 getEyeTrackingInfo.
// pvrEyeTrackingInfo is 8-byte aligned: GazeTan[2] (two float pairs, the
// tangents of each eye's gaze), TimeInSeconds (0 = no tracking),
// ConvergenceDistance, blink[2].
struct PvrEyeTrackingInfo {
    float  gazeTan[2][2];
    double timeInSeconds;
    float  convergenceDistance;
    float  blink[2];
};
static_assert(sizeof(PvrEyeTrackingInfo) == 40, "pvrEyeTrackingInfo is 40 bytes at 8-byte alignment");
typedef void* PvrHmd;
typedef void** (*PFN_GetPvrInterface)(uint32_t major, uint32_t minor);
typedef int32_t (*PFN_PvrInitialise)();
typedef void (*PFN_PvrShutdown)();
typedef int32_t (*PFN_PvrCreateHmd)(PvrHmd* out);
typedef void (*PFN_PvrDestroyHmd)(PvrHmd hmd);
typedef const char* (*PFN_PvrVersionString)();
typedef double (*PFN_PvrTimeSeconds)();
typedef int32_t (*PFN_PvrEyeTrackingInfo)(PvrHmd hmd, double absTime, PvrEyeTrackingInfo* out);
constexpr size_t kPvrInitialise = 0, kPvrShutdown = 1, kPvrCreateHmd = 2, kPvrDestroyHmd = 3,
                 kPvrVersionString = 4, kPvrTimeSeconds = 5, kPvrEyeTrackingInfo = 65;
constexpr size_t kPvrTableMin = 66;

struct PvrWindow {
    uint32_t n = 0;            // samples with tracking
    uint32_t noTracking = 0;   // TimeInSeconds == 0
    double   tanSum[2][2] = {};
    float    tanMin[2][2] = {}, tanMax[2][2] = {};
    double   blinkSum[2] = {0.0, 0.0};
    double   convSum = 0.0;
    double   firstTime = 0.0, lastTime = 0.0;
    uint32_t advances = 0;     // samples whose time moved on from the previous
    void reset() { *this = PvrWindow(); }
};
struct PvrState {
    bool      wanted = false;
    Phase     phase = Phase::Waiting;
    HMODULE   lib = nullptr;
    void**    table = nullptr;
    PvrHmd    hmd = nullptr;
    Sentinel* sentinel = nullptr;
    bool      fresh = false;      // this frame's sample, for the cross-check
    float     tanX = 0.0f, tanY = 0.0f;
    double    lastSampleTime = 0.0;
    PvrWindow win, total;
};
PvrState g_pvr;

void pvrOff(const char* why) {
    g_pvr.phase = Phase::Off;
    Log::get().note("gaze probe pvr: OFF for the session -- %s", why);
}

void pvrArm() {
    if (!g_pvr.wanted || g_pvr.phase != Phase::Waiting) return;
    Config& cfg = Config::get();
    g_pvr.sentinel = new Sentinel(cfg.logDir().c_str(), L"gaze_probe_pvr");
    if (g_pvr.sentinel->trippedOnStartup() && !cfg.getBool("advanced.ignore_sentinel", false)) {
        g_pvr.sentinel->clearTrip();
        pvrOff("SENTINEL TRIPPED: the previous run began the Pimax-runtime calls and never "
               "finished them. Skipped this session; it tries again next launch. If this "
               "keeps happening, set advanced.gaze_probe_pvr = off and report the log.");
        return;
    }
    if (!g_pvr.sentinel->arm()) {
        Log::get().note("gaze probe pvr: the crash sentinel could not be written.");
    }
    g_pvr.lib = LoadLibraryW(L"LibPVRClient64.dll");   // Pimax installs it in System32
    if (!g_pvr.lib) {
        g_pvr.sentinel->confirm();
        pvrOff("LibPVRClient64.dll is not on this machine (Pimax's runtime is not installed, "
               "or not where the loader looks)");
        return;
    }
    PFN_GetPvrInterface getIface =
        reinterpret_cast<PFN_GetPvrInterface>(GetProcAddress(g_pvr.lib, "getPvrInterface"));
    if (!getIface) {
        g_pvr.sentinel->confirm();
        pvrOff("the library exports no getPvrInterface");
        return;
    }
    void** table = nullptr;
    bool survived = guarded("gazeProbePvr/interface", [&] { table = getIface(1, 32); });
    if (!survived) {
        g_pvr.sentinel->confirm();
        pvrOff("getPvrInterface FAULTED (caught). Please report this log.");
        return;
    }
    if (!table) {
        g_pvr.sentinel->confirm();
        pvrOff("the runtime does not serve interface 1.32 (a newer or older Pimax runtime); "
               "nothing is bound");
        return;
    }
    bool nulls = false;
    survived = guarded("gazeProbePvr/table", [&] {
        const size_t idx[7] = {kPvrInitialise, kPvrShutdown, kPvrCreateHmd, kPvrDestroyHmd,
                               kPvrVersionString, kPvrTimeSeconds, kPvrEyeTrackingInfo};
        for (size_t i : idx) if (!table[i]) nulls = true;
    });
    if (!survived || nulls) {
        g_pvr.sentinel->confirm();
        pvrOff(survived ? "the interface table has a null where this build expects a function"
                        : "reading the interface table FAULTED (caught)");
        return;
    }
    g_pvr.table = table;
    // initialise first: the clock and the version come from the connection
    // it opens (the 12:31 flight asked before it and the answer was nothing).
    int32_t rc = -1;
    survived = guarded("gazeProbePvr/initialise", [&] {
        rc = reinterpret_cast<PFN_PvrInitialise>(table[kPvrInitialise])();
    });
    if (!survived || rc != 0) {
        g_pvr.sentinel->confirm();
        char why[160];
        snprintf(why, sizeof(why), survived ? "initialise answered pvrResult %d (0 is success)"
                                            : "initialise FAULTED (caught)", rc);
        pvrOff(why);
        return;
    }
    // Plausibility before the tracker: the version string readable, the
    // clock a finite positive number; what was read is said either way.
    const char* ver = nullptr;
    double now = 0.0;
    survived = guarded("gazeProbePvr/version", [&] {
        ver = reinterpret_cast<PFN_PvrVersionString>(table[kPvrVersionString])();
        now = reinterpret_cast<PFN_PvrTimeSeconds>(table[kPvrTimeSeconds])();
    });
    if (!survived || !ver || !std::isfinite(now) || now <= 0.0) {
        g_pvr.sentinel->confirm();
        char why[220];
        if (survived) {
            snprintf(why, sizeof(why),
                     "after initialise the version string is %s and the clock reads %.3f (not the "
                     "table this build was written against, or a runtime answering nothing to a "
                     "second client)",
                     ver ? "readable" : "null", now);
        } else {
            snprintf(why, sizeof(why), "the version or clock call FAULTED (caught)");
        }
        pvrOff(why);
        guarded("gazeProbePvr/shutdown", [&] {
            reinterpret_cast<PFN_PvrShutdown>(table[kPvrShutdown])();
        });
        return;
    }
    PvrHmd hmd = nullptr;
    rc = -1;
    survived = guarded("gazeProbePvr/createHmd", [&] {
        rc = reinterpret_cast<PFN_PvrCreateHmd>(table[kPvrCreateHmd])(&hmd);
    });
    if (!survived || rc != 0 || !hmd) {
        g_pvr.sentinel->confirm();
        char why[160];
        snprintf(why, sizeof(why), survived ? "createHmd answered pvrResult %d with %s handle"
                                            : "createHmd FAULTED (caught)",
                 rc, hmd ? "a" : "a null");
        pvrOff(why);
        guarded("gazeProbePvr/shutdown", [&] {
            reinterpret_cast<PFN_PvrShutdown>(table[kPvrShutdown])();
        });
        return;
    }
    g_pvr.hmd = hmd;
    g_pvr.sentinel->confirm();
    g_pvr.phase = Phase::Armed;
    Log::get().note(
        "gaze probe pvr: ARMED -- LibPVRClient64.dll serves interface 1.32, reports itself "
        "as \"%s\", its clock reads %.1f s; initialise and createHmd succeeded. Asking "
        "getEyeTrackingInfo once a frame from here; per-window aggregates only.",
        ver, now);
}

void pvrAsk() {
    g_pvr.fresh = false;
    if (g_pvr.phase != Phase::Armed) return;
    PvrEyeTrackingInfo info{};
    int32_t rc = -1;
    double now = 0.0;
    const bool survived = guarded("gazeProbePvr/ask", [&] {
        now = reinterpret_cast<PFN_PvrTimeSeconds>(g_pvr.table[kPvrTimeSeconds])();
        rc = reinterpret_cast<PFN_PvrEyeTrackingInfo>(g_pvr.table[kPvrEyeTrackingInfo])(
            g_pvr.hmd, now, &info);
    });
    if (!survived) {
        pvrOff("a per-frame call FAULTED (caught). Please report this log.");
        return;
    }
    if (rc != 0) {
        char why[120];
        snprintf(why, sizeof(why), "getEyeTrackingInfo answered pvrResult %d (0 is success)", rc);
        pvrOff(why);
        return;
    }
    if (info.timeInSeconds == 0.0 || !std::isfinite(info.timeInSeconds)) {
        ++g_pvr.win.noTracking;
        ++g_pvr.total.noTracking;
        return;
    }
    bool sane = true;
    for (int e = 0; e < 2; ++e) {
        for (int a = 0; a < 2; ++a) {
            if (!std::isfinite(info.gazeTan[e][a]) || fabsf(info.gazeTan[e][a]) > 4.0f) sane = false;
        }
    }
    if (!sane) return;
    PvrWindow* w[2] = {&g_pvr.win, &g_pvr.total};
    for (PvrWindow* pw : w) {
        if (pw->n == 0) {
            pw->firstTime = info.timeInSeconds;
            for (int e = 0; e < 2; ++e) for (int a = 0; a < 2; ++a) pw->tanMin[e][a] = pw->tanMax[e][a] = info.gazeTan[e][a];
        }
        for (int e = 0; e < 2; ++e) {
            for (int a = 0; a < 2; ++a) {
                pw->tanSum[e][a] += info.gazeTan[e][a];
                if (info.gazeTan[e][a] < pw->tanMin[e][a]) pw->tanMin[e][a] = info.gazeTan[e][a];
                if (info.gazeTan[e][a] > pw->tanMax[e][a]) pw->tanMax[e][a] = info.gazeTan[e][a];
            }
            pw->blinkSum[e] += std::isfinite(info.blink[e]) ? info.blink[e] : 0.0;
        }
        pw->convSum += std::isfinite(info.convergenceDistance) ? info.convergenceDistance : 0.0;
        if (info.timeInSeconds > g_pvr.lastSampleTime) ++pw->advances;
        pw->lastTime = info.timeInSeconds;
        ++pw->n;
    }
    g_pvr.lastSampleTime = info.timeInSeconds;
    g_pvr.fresh = true;
    g_pvr.tanX = 0.5f * (info.gazeTan[0][0] + info.gazeTan[1][0]);
    g_pvr.tanY = 0.5f * (info.gazeTan[0][1] + info.gazeTan[1][1]);
}

void pvrSummary(bool final) {
    if (g_pvr.phase != Phase::Armed) return;
    const PvrWindow& w = final ? g_pvr.total : g_pvr.win;
    const double span = w.lastTime - w.firstTime;
    const double hz = (w.advances > 1 && span > 0.0) ? (w.advances - 1) / span : 0.0;
    auto mean = [&](int e, int a) { return w.n ? w.tanSum[e][a] / w.n : 0.0; };
    Log::get().note(
        "gaze probe pvr %s: %u samples with tracking, %u frames without (time 0); the sample "
        "time advanced on %u frames at %.1f Hz; left GazeTan x %.3f..%.3f mean %.3f, y %.3f..%.3f "
        "mean %.3f; right x %.3f..%.3f mean %.3f, y %.3f..%.3f mean %.3f; blink mean %.2f / %.2f; "
        "convergence mean %.2f m. Eyes far left/right should swing x by about 0.6, eyes shut "
        "should raise blink toward 1.",
        final ? "SESSION TOTALS" : "summary", w.n, w.noTracking, w.advances, hz,
        static_cast<double>(w.tanMin[0][0]), static_cast<double>(w.tanMax[0][0]), mean(0, 0),
        static_cast<double>(w.tanMin[0][1]), static_cast<double>(w.tanMax[0][1]), mean(0, 1),
        static_cast<double>(w.tanMin[1][0]), static_cast<double>(w.tanMax[1][0]), mean(1, 0),
        static_cast<double>(w.tanMin[1][1]), static_cast<double>(w.tanMax[1][1]), mean(1, 1),
        w.n ? w.blinkSum[0] / w.n : 0.0, w.n ? w.blinkSum[1] / w.n : 0.0,
        w.n ? w.convSum / w.n : 0.0);
}

void pvrShutdown() {
    if (g_pvr.phase != Phase::Armed || !g_pvr.table) return;
    void** table = g_pvr.table;
    PvrHmd hmd = g_pvr.hmd;
    g_pvr.phase = Phase::Off;
    guarded("gazeProbePvr/destroy", [&] {
        if (hmd) reinterpret_cast<PFN_PvrDestroyHmd>(table[kPvrDestroyHmd])(hmd);
        reinterpret_cast<PFN_PvrShutdown>(table[kPvrShutdown])();
    });
    // The library stays loaded: freeing a runtime's client at exit is how
    // exits crash, and the process is leaving anyway.
}

// The repair, once a frame: n from the direction-form read, t from entry
// 12, the two roots for four sign variants, each judged against the left
// eye's frustum and, when Route B has a sample this frame, against its
// averaged tangents.
void repairAccumulate() {
    RepairWindow* w[2] = {&g_repair, &g_repairTotal};
    double t[3];
    const bool haveT = readRawHead(t);
    if (!haveT) { for (RepairWindow* rw : w) ++rw->noT; }
    float t4[4];
    const bool haveFrustum = systemHookEffectiveTangents(vr::Eye_Left, t4) && t4[1] > t4[0] && t4[3] > t4[2];
    double pvrDir[3] = {0.0, 0.0, 0.0};
    if (g_pvr.fresh) {
        pvrDir[0] = g_pvr.tanX;
        pvrDir[1] = g_pvr.tanY;
        pvrDir[2] = -1.0;
        const double pl = sqrt(pvrDir[0] * pvrDir[0] + pvrDir[1] * pvrDir[1] + 1.0);
        for (double& c : pvrDir) c /= pl;
    }
    // The direction-form read first: its n serves the quadratic and the
    // point-form's parallelism check alike.
    vr::HmdVector2_t tan = {{NAN, NAN}};
    bool declined = false;
    const bool haveN = callProj(kDirRows, &tan, &declined) && wellFormedTan(tan);
    if (!haveN) { for (RepairWindow* rw : w) ++rw->noN; }
    double n[3] = {0.0, 0.0, -1.0};
    if (haveN) {
        n[0] = tan.v[0];
        n[1] = tan.v[1];
        const double nl = sqrt(n[0] * n[0] + n[1] * n[1] + 1.0);
        for (double& c : n) c /= nl;
    }
    // The point-form repair: p with its scale from two point-form reads (x
    // and y with the identity-with-w=1 matrix, z with the first row swapped
    // in). The 18:15 flight read |p - t| = 1.00 in every window and
    // cos(p, n) = -1.000: the point form is handed out as t - d, the head's
    // raw-universe position minus the unit gaze, so the gaze is d = t - p,
    // one subtraction per frame. The magnitudes stay in the summary as the
    // standing check of that model.
    vr::HmdVector2_t xy = {{NAN, NAN}}, zy = {{NAN, NAN}};
    bool dec1 = false, dec2 = false;
    const bool haveP = callProj(kPointRows, &xy, &dec1) && callProj(kZRows, &zy, &dec2) &&
                       std::isfinite(xy.v[0]) && std::isfinite(xy.v[1]) && std::isfinite(zy.v[0]) &&
                       fabsf(xy.v[0]) < 1000.0f && fabsf(xy.v[1]) < 1000.0f && fabsf(zy.v[0]) < 1000.0f;
    if (!haveP) { for (RepairWindow* rw : w) ++rw->noP; }
    if (haveP && haveT) {
        const double p[3] = {xy.v[0], xy.v[1], zy.v[0]};
        const double d0[3] = {t[0] - p[0], t[1] - p[1], t[2] - p[2]};   // the gaze, d = t - p
        const double s0[3] = {p[0] + t[0], p[1] + t[1], p[2] + t[2]};   // the other sign, for the record
        const double dl = sqrt(d0[0] * d0[0] + d0[1] * d0[1] + d0[2] * d0[2]);
        const double ml = sqrt(s0[0] * s0[0] + s0[1] * s0[1] + s0[2] * s0[2]);
        const double pl = sqrt(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]);
        const double tl = sqrt(t[0] * t[0] + t[1] * t[1] + t[2] * t[2]);
        for (RepairWindow* rw : w) {
            ++rw->magN;
            rw->pAbs += pl;
            rw->tAbs += tl;
            rw->plusAbs += ml;    // |p + t|
            rw->minusAbs += dl;   // |t - p|, the gaze's length: 1.000 when the model holds
            for (int i = 0; i < 3; ++i) {
                rw->pMean[i] += p[i];
                rw->tMean[i] += t[i];
            }
            if (haveN && pl > 0.0) {
                rw->cosPN += (p[0] * n[0] + p[1] * n[1] + p[2] * n[2]) / pl;
                ++rw->cosN;
            }
            if (!(dl > 0.5 && dl < 2.0)) {
                ++rw->dRejected;
                continue;
            }
            if (rw->subN == 0) rw->dAbsMin = rw->dAbsMax = dl;
            if (dl < rw->dAbsMin) rw->dAbsMin = dl;
            if (dl > rw->dAbsMax) rw->dAbsMax = dl;
            ++rw->subN;
            rw->pAbsSum += pl;
            rw->dAbsSum += dl;
            for (int v = 0; v < 4; ++v) {
                const double d[3] = {((v & 1) ? -d0[0] : d0[0]) / dl, ((v & 2) ? -d0[1] : d0[1]) / dl,
                                     d0[2] / dl};
                for (int i = 0; i < 3; ++i) rw->dSubSum[v][i] += d[i];
                if (haveFrustum && d[2] < 0.0) {
                    const double tx = d[0] / -d[2], ty = d[1] / -d[2];
                    if (tx >= t4[0] && tx <= t4[1] && ty >= t4[2] && ty <= t4[3]) ++rw->inFrustumSub[v];
                }
                if (g_pvr.fresh) {
                    double dot = d[0] * pvrDir[0] + d[1] * pvrDir[1] + d[2] * pvrDir[2];
                    if (dot > 1.0) dot = 1.0;
                    if (dot < -1.0) dot = -1.0;
                    const double deg = acos(dot) * 57.2957795;
                    rw->pvrDiffSqSub[v] += deg * deg;
                }
            }
            if (g_pvr.fresh) ++rw->pvrSubN;
        }
    }
    // The quadratic -- Route A as it stands: the 14:54 flight's far root,
    // both axes mirrored, agreed with Route B to 0.2 degrees RMS.
    if (!haveN || !haveT) return;
    const double nt = n[0] * t[0] + n[1] * t[1] + n[2] * t[2];
    const double tt = t[0] * t[0] + t[1] * t[1] + t[2] * t[2];
    const double disc = nt * nt - tt + 1.0;
    for (RepairWindow* rw : w) {
        ++rw->n;
        for (int i = 0; i < 3; ++i) rw->tSum[i] += t[i];
        rw->tAbsSum += sqrt(tt);
        rw->nTanSum[0] += tan.v[0];
        rw->nTanSum[1] += tan.v[1];
        if (disc < rw->discMin) rw->discMin = disc;
        if (disc < 0.0) {
            ++rw->neg;
            continue;
        }
        const double s = sqrt(disc);
        const double lam[2] = {-nt + s, -nt - s};   // far, near
        for (int root = 0; root < 2; ++root) {
            const double d0[3] = {lam[root] * n[0] + t[0], lam[root] * n[1] + t[1], lam[root] * n[2] + t[2]};
            for (int v = 0; v < 4; ++v) {
                const double d[3] = {(v & 1) ? -d0[0] : d0[0], (v & 2) ? -d0[1] : d0[1], d0[2]};
                for (int i = 0; i < 3; ++i) rw->dSum[root][v][i] += d[i];
                if (haveFrustum && d[2] < 0.0) {
                    const double tx = d[0] / -d[2], ty = d[1] / -d[2];
                    if (tx >= t4[0] && tx <= t4[1] && ty >= t4[2] && ty <= t4[3]) ++rw->inFrustum[root][v];
                }
                if (g_pvr.fresh) {
                    double dot = d[0] * pvrDir[0] + d[1] * pvrDir[1] + d[2] * pvrDir[2];
                    if (dot > 1.0) dot = 1.0;
                    if (dot < -1.0) dot = -1.0;
                    const double deg = acos(dot) * 57.2957795;
                    rw->pvrDiffSq[root][v] += deg * deg;
                }
            }
        }
        if (g_pvr.fresh) ++rw->pvrN;
    }
}

void repairSummary(bool final) {
    const RepairWindow& w = final ? g_repairTotal : g_repair;
    if (w.n == 0 && w.noN == 0 && w.noT == 0 && w.subN == 0 && w.noP == 0) return;
    const char* vnames[4] = {"as-is", "x-mirrored", "y-mirrored", "both"};
    {
        char dirs[400];
        int len = 0;
        for (int v = 0; v < 4; ++v) {
            const double* s = w.dSubSum[v];
            const double l = sqrt(s[0] * s[0] + s[1] * s[1] + s[2] * s[2]);
            double yaw = 0.0, pitch = 0.0;
            if (l > 0.0) {
                const double d[3] = {s[0] / l, s[1] / l, s[2] / l};
                anglesOf(d, &yaw, &pitch);
            }
            len += snprintf(dirs + len, sizeof(dirs) - len, "%s%s %+.1f/%+.1f (%.0f%% in frustum)",
                            v ? ", " : "", vnames[v], yaw, pitch,
                            w.subN ? 100.0 * w.inFrustumSub[v] / w.subN : 0.0);
        }
        char agree[200] = "no Route B samples this window";
        if (w.pvrSubN) {
            int alen = snprintf(agree, sizeof(agree), "RMS to Route B: ");
            for (int v = 0; v < 4; ++v) {
                alen += snprintf(agree + alen, sizeof(agree) - alen, "%s%s %.1f deg", v ? ", " : "",
                                 vnames[v], sqrt(w.pvrDiffSqSub[v] / w.pvrSubN));
            }
        }
        const double mn = w.magN ? static_cast<double>(w.magN) : 1.0;
        Log::get().note(
            "gaze probe %s, the frame repair by SUBTRACTION (Route A, d = t - p): over %u frames with p and "
            "t |p| %.2f, |t| %.2f, |p + t| %.2f, |t - p| %.2f (1.000 says the model holds), cos(p, n) "
            "%+.3f over %u (-1 says the point form is handed out opposed to the direction form, as "
            "measured); p mean (%.2f, %.2f, %.2f), t mean (%.2f, %.2f, %.2f). Frames with |d| within "
            "0.5..2: %u (%u the read declined, %u without a raw pose, %u set aside), |d| mean %.3f, "
            "%.3f..%.3f; d's mean direction as yaw/pitch, degrees, + = right/up, in four sign "
            "conventions: %s. The convention whose yaw goes left when the eyes go left and whose pitch "
            "goes up when they go up is the gaze in the head's frame. %s.",
            final ? "SESSION TOTALS" : "summary", w.magN, w.pAbs / mn, w.tAbs / mn, w.plusAbs / mn,
            w.minusAbs / mn, w.cosN ? w.cosPN / w.cosN : 0.0, w.cosN, w.pMean[0] / mn, w.pMean[1] / mn,
            w.pMean[2] / mn, w.tMean[0] / mn, w.tMean[1] / mn, w.tMean[2] / mn, w.subN, w.noP, w.noT,
            w.dRejected, w.subN ? w.dAbsSum / w.subN : 0.0, w.dAbsMin, w.dAbsMax, dirs, agree);
    }
    char roots[2][320];
    for (int root = 0; root < 2; ++root) {
        int len = 0;
        for (int v = 0; v < 4; ++v) {
            const double* s = w.dSum[root][v];
            const double l = sqrt(s[0] * s[0] + s[1] * s[1] + s[2] * s[2]);
            double yaw = 0.0, pitch = 0.0;
            if (l > 0.0) {
                const double d[3] = {s[0] / l, s[1] / l, s[2] / l};
                anglesOf(d, &yaw, &pitch);
            }
            const uint32_t solved = w.n - w.neg;
            const double inPct = solved ? 100.0 * w.inFrustum[root][v] / solved : 0.0;
            len += snprintf(roots[root] + len, sizeof(roots[root]) - len, "%s%s %+.1f/%+.1f (%.0f%% in frustum)",
                            v ? ", " : "", vnames[v], yaw, pitch, inPct);
        }
    }
    char agree[200] = "no Route B samples this window";
    if (w.pvrN) {
        int len = snprintf(agree, sizeof(agree), "RMS to Route B, far root: ");
        for (int v = 0; v < 4; ++v) {
            len += snprintf(agree + len, sizeof(agree) - len, "%s%s %.1f deg", v ? ", " : "", vnames[v],
                            sqrt(w.pvrDiffSq[0][v] / w.pvrN));
        }
    }
    Log::get().note(
        "gaze probe %s, the frame repair (Route A): over %u frames (%u without a raw pose, %u the "
        "direction-form read declined) t = the head in the raw universe, mean (%.3f, %.3f, %.3f), "
        "|t| %.2f m (compare the room test's raw-universe head above); the direction-form read's "
        "mean tangents (%.3f, %.3f); the inversion's discriminant min %.3f, negative on %u frames. "
        "The FAR root's mean direction as yaw/pitch, degrees, + = right/up: %s. The NEAR root: %s. "
        "The variant and root that read straight ahead at rest and swing with the eye sweeps are "
        "the gaze. %s.",
        final ? "SESSION TOTALS" : "summary", w.n, w.noT, w.noN,
        w.n ? w.tSum[0] / w.n : 0.0, w.n ? w.tSum[1] / w.n : 0.0, w.n ? w.tSum[2] / w.n : 0.0,
        w.n ? w.tAbsSum / w.n : 0.0, w.n ? w.nTanSum[0] / w.n : 0.0, w.n ? w.nTanSum[1] / w.n : 0.0,
        w.discMin < 1e9 ? w.discMin : 0.0, w.neg, roots[0], roots[1], agree);
}

// At arming: is the gaze carried as a point (x, y, z readable, |p| the
// scale) or as a direction (the point-form call declines or answers
// nothing finite)? Once; the magnitude only is logged.
void pointFormRead() {
    if (g_pointFormTried) return;
    g_pointFormTried = true;
    vr::HmdVector2_t xy = {{NAN, NAN}}, zy = {{NAN, NAN}};
    bool dec1 = false, dec2 = false;
    const bool ok1 = callProj(kPointRows, &xy, &dec1);
    const bool ok2 = ok1 && callProj(kZRows, &zy, &dec2);
    if (ok1 && ok2 && std::isfinite(xy.v[0]) && std::isfinite(xy.v[1]) && std::isfinite(zy.v[0])) {
        const double p = sqrt(static_cast<double>(xy.v[0]) * xy.v[0] + static_cast<double>(xy.v[1]) * xy.v[1] +
                              static_cast<double>(zy.v[0]) * zy.v[0]);
        Log::get().note(
            "gaze probe: the point-form read of entry 36 answered finite values with |p| = %.3f "
            "-- %s. The direction-form read carries the repair either way.",
            p, fabs(p - 1.0) < 0.02 ? "SteamVR hands out a NORMALISED direction (the scale is gone; "
                                      "the repair's constraint supplies it)"
                                    : "NOT unit length: SteamVR hands out the raw d - t with its scale, "
                                      "which would make the repair a subtraction instead of a quadratic");
    } else {
        Log::get().note(
            "gaze probe: the point-form read of entry 36 %s -- SteamVR treats the gaze as a "
            "DIRECTION (w = 0), so only the direction-form read applies.",
            (dec1 || dec2) ? "declined" : "answered nothing finite");
    }
}

// The HMD's tracking system and driver version (entry 28, properties 1000
// and 1031), for the ARMED line and for the report to the vendor.
void driverIdentity(char* system, size_t systemCap, char* version, size_t versionCap) {
    snprintf(system, systemCap, "?");
    snprintf(version, versionCap, "?");
    if (!g_stringProp) return;
    guarded("gazeProbe/identity", [&] {
        int32_t err = 0;
        char buf[128] = "";
        uint32_t n = g_stringProp(vr::k_unTrackedDeviceIndex_Hmd, 1000, buf, sizeof(buf), &err);
        if (n > 0 && err == 0) snprintf(system, systemCap, "%s", buf);
        buf[0] = 0;
        n = g_stringProp(vr::k_unTrackedDeviceIndex_Hmd, 1031, buf, sizeof(buf), &err);
        if (n > 0 && err == 0) snprintf(version, versionCap, "%s", buf);
        else snprintf(version, versionCap, "? (property error %d)", err);
    });
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
            "vouched for a per-eye point with a shape (finite). Whether it FOLLOWS "
            "the eyes is what the summaries below say: read the per-frame step and "
            "the ranges.",
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
    const HeadWindow& h = final ? g_headTotal : g_head;
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
    // The same window read as directions, and the head beside them.
    double ly = 0.0, lp = 0.0, ry = 0.0, rp = 0.0;
    const bool haveL = e[0].n && ndcToDegrees(0, e[0].meanX(), e[0].meanY(), &ly, &lp);
    const bool haveR = e[1].n && ndcToDegrees(1, e[1].meanX(), e[1].meanY(), &ry, &rp);
    double hy = 0.0, hp = 0.0;
    h.facing(&hy, &hp);
    Log::get().note(
        "gaze probe %s, read as directions: the mean centre is %s%.1f deg yaw, %+.1f deg "
        "pitch (left eye) and %s%.1f deg yaw, %+.1f deg pitch (right eye), through each "
        "eye's own projection, + = right / up; the head over the same %u frames sat at "
        "(%.3f, %.3f, %.3f) m in tracking space, moved at most %.3f m, and faced %+.1f deg "
        "yaw, %+.1f deg pitch. A centre that swings with the head's facing is in the "
        "wrong frame; one that holds through a head turn is head-relative.",
        final ? "totals" : "summary", haveL ? (ly < 0 ? "" : "+") : "?", haveL ? ly : 0.0,
        haveL ? lp : 0.0, haveR ? (ry < 0 ? "" : "+") : "?", haveR ? ry : 0.0,
        haveR ? rp : 0.0, h.n, h.mean(0), h.mean(1), h.mean(2), h.span(), hy, hp);
    // The room test. Flight 2 (2026-09-05) read a constant 37 deg right,
    // 54 deg up that eye sweeps moved by a few degrees and a 50 deg head
    // turn dragged the same way -- the signature of a POINT in the room's
    // standing space being projected as if it were head-relative. If so,
    // the head's own position in standing space, read as such a point,
    // lands on the same angles. The runtime's seated-to-standing transform
    // is entry 13 of the same table; the game's poses are seated.
    // Flight 3 (2026-09-05) put the head at (-2.3, -4.5, +2.4) m in the
    // STANDING universe -- nowhere near the predicted point and behind the
    // origin -- so if the constant is a room-space origin, it is not
    // SteamVR's standing space. The driver's own RAW universe (the frame an
    // inside-out headset starts in) is the other candidate: entry 14 maps
    // raw to standing, so head_raw = inverse(raw->standing) * head_standing.
    if (g_seatedToStanding && h.n) {
        vr::HmdMatrix34_t ss{}, rs{};
        bool gotSs = false, gotRs = false;
        const bool survived = guarded("gazeProbe/universes", [&] {
            ss = g_seatedToStanding();
            gotSs = true;
            if (g_rawToStanding) {
                rs = g_rawToStanding();
                gotRs = true;
            }
        });
        if (!survived) {
            g_seatedToStanding = nullptr;
            g_rawToStanding = nullptr;
            Log::get().note("gaze probe: a universe-transform call FAULTED; the room test "
                            "is off for the session. Please report this log.");
        } else if (gotSs) {
            const double p[3] = {h.mean(0), h.mean(1), h.mean(2)};
            double s[3], rw[3] = {0.0, 0.0, 0.0};
            apply34(ss, p, s);
            if (gotRs) applyInverse34(rs, s, rw);
            double ys = 0.0, ps = 0.0, yr = 0.0, pr = 0.0;
            const bool aheadS = pointAngles(s, &ys, &ps);
            const bool aheadR = gotRs && pointAngles(rw, &yr, &pr);
            char angS[64], angR[64];
            if (aheadS) snprintf(angS, sizeof(angS), "%+.1f deg yaw, %+.1f deg pitch", ys, ps);
            else snprintf(angS, sizeof(angS), "behind or beside the origin");
            if (!gotRs) snprintf(angR, sizeof(angR), "(entry 14 not available)");
            else if (aheadR) snprintf(angR, sizeof(angR), "%+.1f deg yaw, %+.1f deg pitch", yr, pr);
            else snprintf(angR, sizeof(angR), "behind or beside the origin");
            Log::get().note(
                "gaze probe %s, the room test: the head sat at (%.3f, %.3f, %.3f) m in the "
                "STANDING universe (seated-to-standing translation (%.3f, %.3f, %.3f), yaw "
                "%+.1f deg) -- read as head-relative that projects to %s; and at (%.3f, %.3f, "
                "%.3f) m in the RAW universe (raw-to-standing translation (%.3f, %.3f, %.3f), "
                "yaw %+.1f deg) -- read as head-relative that projects to %s. Whichever lands "
                "on the centre's angles above is the frame the runtime is mixing in.",
                final ? "totals" : "summary", s[0], s[1], s[2],
                static_cast<double>(ss.m[0][3]), static_cast<double>(ss.m[1][3]),
                static_cast<double>(ss.m[2][3]), yawOf34(ss), angS, rw[0], rw[1], rw[2],
                gotRs ? static_cast<double>(rs.m[0][3]) : 0.0,
                gotRs ? static_cast<double>(rs.m[1][3]) : 0.0,
                gotRs ? static_cast<double>(rs.m[2][3]) : 0.0, gotRs ? yawOf34(rs) : 0.0, angR);
        }
    }
    repairSummary(final);
    pvrSummary(final);
    if (!final) {
        g_win = Counts();
        g_eye[0].reset();
        g_eye[1].reset();
        g_head.reset();
        g_repair.reset();
        g_pvr.win.reset();
        ++g_summaries;
        g_nextSummary = g_frames + g_cadence;
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
    void* pSeated = nullptr;
    void* pRaw = nullptr;
    void* pPose = nullptr;
    void* pProp = nullptr;
    survived = guarded("gazeProbe/table", [&] {
        pSize = fn[kFnRecommendedSize];
        pCenter = fn[kFnFoveationCenter];
        pCenterProj = fn[kFnFoveationCenterForProjection];
        pVersion = fn[kFnRuntimeVersion];
        pSeated = fn[kFnSeatedToStanding];
        pRaw = fn[kFnRawToStanding];
        pPose = fn[kFnDeviceToAbsolutePose];
        pProp = fn[kFnStringProperty];
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
    g_seatedToStanding = reinterpret_cast<PFN_SeatedToStanding>(pSeated);
    g_rawToStanding = reinterpret_cast<PFN_SeatedToStanding>(pRaw);
    g_devicePose = reinterpret_cast<PFN_DeviceToAbsolutePose>(pPose);
    g_stringProp = reinterpret_cast<PFN_StringProperty>(pProp);

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
    // Where straight ahead lands in each eye's NDC, for reading the
    // summaries: an asymmetric frustum puts it off the image centre.
    double ax[2] = {0.0, 0.0}, ay[2] = {0.0, 0.0};
    bool haveAhead = true;
    for (int e = 0; e < 2 && haveAhead; ++e) {
        float t4[4];
        haveAhead = systemHookEffectiveTangents(e == 0 ? vr::Eye_Left : vr::Eye_Right, t4) &&
                    t4[1] > t4[0] && t4[3] > t4[2];
        if (haveAhead) {
            ax[e] = -(t4[1] + t4[0]) / (t4[1] - t4[0]);
            ay[e] = -(t4[3] + t4[2]) / (t4[3] - t4[2]);
        }
    }
    Log::get().note(
        "gaze probe: ARMED on %s (runtime reports itself as \"%s\"). Entry 0 "
        "answered %ux%u, matching what the game was told%s. The first "
        "GetEyeTrackedFoveationCenter call %s. Straight ahead is NDC (%+.3f, %+.3f) in "
        "the left eye and (%+.3f, %+.3f) in the right%s. Asking every frame from here; "
        "the first summary comes after 600 frames, then every %u "
        "(advanced.gaze_probe_summary_frames). Nothing per frame is written down.",
        kFnTableVersion, runtimeVersion ? runtimeVersion : "(no version string)", w, h,
        haveTruth ? "" : " (by sanity range only)",
        vouched ? "VOUCHED for a centre" : "DECLINED (false): no tracker, no gaze "
                                            "published by this headset's driver, or "
                                            "not yet -- the summaries say which",
        ax[0], ay[0], ax[1], ay[1],
        haveAhead ? "" : " (tangents not yet seen: zeros printed)", g_cadence);
    {
        // Phase 1 (docs/eye-tracking.md): who made this driver, for the
        // report; whether the gaze is carried as a point or a direction;
        // and Route B, when asked for.
        char sys[128], drv[128];
        driverIdentity(sys, sizeof(sys), drv, sizeof(drv));
        Log::get().note(
            "gaze probe: the HMD's tracking system is \"%s\", driver version \"%s\" (entry 28, "
            "properties 1000 and 1031).",
            sys, drv);
    }
    pointFormRead();
    pvrArm();
    record(vouched, L, R);
    ++g_frames;
}

void ask(const vr::TrackedDevicePose_t* hmd) {
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
    pvrAsk();            // Route B's sample first: the repair compares against it
    repairAccumulate();  // Route A
    if (hmd && hmd->bPoseIsValid) {
        g_head.add(hmd->mDeviceToAbsoluteTracking);
        g_headTotal.add(hmd->mDeviceToAbsoluteTracking);
    }
    if ((g_frames % 60) == 0) checkProjectionVariant(vouched, L);
    ++g_frames;
    if (g_summaries < kSummaryCap && g_frames >= g_nextSummary) summary(false);
}

}  // namespace

void gazeProbeNoteGetter(PFN_RealGetGenericInterface get) { g_get = get; }

void gazeProbeConfigure() {
    Config& cfg = Config::get();
    const bool want = cfg.getBool("advanced.gaze_probe", false);
    // The summary cadence is live: a look-sequence flight wants a few
    // seconds a window, a soak wants a minute.
    int cadence = cfg.getInt("advanced.gaze_probe_summary_frames", 5400);
    if (cadence < 60) cadence = 60;
    if (cadence > 54000) cadence = 54000;
    if (static_cast<uint32_t>(cadence) != g_cadence) {
        g_cadence = static_cast<uint32_t>(cadence);
        if (g_phase == Phase::Armed && g_nextSummary > g_frames + g_cadence) {
            g_nextSummary = g_frames + g_cadence;
        }
    }
    if (!g_configured) {
        g_configured = true;
        g_wanted = want;
        // Route B rides on the probe: the Pimax-runtime read, off by default.
        g_pvr.wanted = want && cfg.getBool("advanced.gaze_probe_pvr", false);
        if (g_pvr.wanted) {
            Log::get().note(
                "gaze probe pvr: ON (advanced.gaze_probe_pvr). Loads Pimax's client library "
                "and asks its eye tracker once a frame, beside SteamVR's answer; aggregates "
                "only. A dev instrument for docs/eye-tracking.md.");
        }
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

void gazeProbeApply(vr::EVRCompositorError err, const vr::TrackedDevicePose_t* renderPoses,
                    uint32_t renderCount) {
    if (!g_wanted || g_phase == Phase::Off) return;
    if (g_phase == Phase::Waiting) {
        tryArm();
        return;
    }
    const vr::TrackedDevicePose_t* hmd =
        (err == 0 && renderPoses && renderCount > vr::k_unTrackedDeviceIndex_Hmd)
            ? &renderPoses[vr::k_unTrackedDeviceIndex_Hmd]
            : nullptr;
    ask(hmd);
}

void gazeProbeShutdown() {
    if (g_phase != Phase::Armed) return;
    summary(true);
    pvrShutdown();
}

}  // namespace edvr
