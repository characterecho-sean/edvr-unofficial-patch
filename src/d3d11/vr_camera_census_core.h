// The VR camera census, the pure half (design doc section 82, "Pre-build findings and the stop").
//
// THE QUESTION. In VR on foot the game draws the world once (a flat "screen" camera) and then one composite draw an
// eye whose VS reads b1 rows 270..273, a per-eye camera matrix in the composer's layout. The VR world route will
// jitter the WORLD camera through the camera injector's detour; that phase must not reach the eye cameras. The
// injector admits every kind-3 camera, and no signal is known that tells an eye camera from the world's:
//   (A) a field signature at the refresh (aspect, near, field of view, the bound pair, the viewport),
//   (B) a join by content between the rows the composer produced for a camera and the eye draws' b1 rows,
//   (C) the call's place in the frame against the tone draw,
//   (D) the caller address,
//   (E) the camera's tangents against the eye frusta EDVR itself advertises,
//   (F) the view (the pass object the refresh is handed with the camera: its second argument).
// One flight with advanced.vr_camera_census = on records enough to decide: this header is what it records and how each
// record is written. `python tools\edvr_log.py --camera-census` reads it back and does the join offline.
//
// FLIGHT 1 ANSWERED IT (4.63 million calls): the eye cameras are kind 5 and the world's on-foot camera is kind 3, read from
// the KIND OF EACH CALL, never from the camera object (one object was the left eye camera in the cockpit and the world
// camera on foot). STAGE 2 then puts a sub-pixel phase into the kind-3 world cameras through the same detour, and the census
// verifies it in the same flight: the eye draws' measured rows must not move while the kind-3 calls' rows carry the phase.
// So each call line also says what the detour decided for it (inj, role), each sample says the phase the route chose for
// its frame, and a frame is sampled only when that phase can show a leak (vrCensusSamplesFrame).
//
// WHAT IS HERE. Everything the census decides and every line it prints, with no D3D, no log, no game and no allocation
// (tools\vr_camera_census_test runs each function, and the DLL compiles the very same text):
//   - the key and the decision to run (VR profile and the key on; anything else is nothing at all);
//   - which frames are sampled (the tone seen, and the journal, when it is read, saying on foot; while the route jitters,
//     a non-zero phase too);
//   - the camera struct's field signature and the rows the composer writes for it, ported from
//     tools\c2_derive_test\c2_derive_model.h (composeSceneCb), so a camera's rows are known without reading the game's
//     own constant buffer and can be compared with an eye draw's;
//   - the bounded tables: one row per distinct camera (64), the call sequence of one frame (160), the lock-free table
//     of calls on other threads (16), the eye-draw allowance (8), and the 5 s window;
//   - the log-line budget per class, so the whole census stays near 400 lines a session whatever the flight does (the episodes
//     below have classes of their own);
//   - the EPISODES (design-world-camera-motion-2026-09-30.md section 6, Phase 0): the trigger machine, the sampled frame's call
//     buffer, the join of the screen's and the eyes' depths to the b1 rows read, the matches of those rows and of the temporal
//     pass's chosen rows to the calls, the call-line selection, the on-foot naming runs and the observer's CPU accumulators, and
//     vrCensusPrintEpisode, which prints one episode (the DLL and the reader's fixture both use it);
//   - the text of every line.
//
// THE ZERO-MUTATION CONTRACT is the detour's (flat_camera_inject.cpp, observe-only mode): nothing here writes the game.
#pragma once
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace edvr {

// ---- the key ----------------------------------------------------------------------------------------------------
// advanced.vr_camera_census: on or off, off by default. A value that is present and is not "on" reads as off, so a typo
// can never install a hook. VR profile only: a flat profile reads the key off whatever the file says (Config refuses
// it, runtimeProfileAllowsKey does not list it), and the wanted decision asks the profile a second time.
enum class VrCameraCensusKey : uint8_t { Off, On };
inline VrCameraCensusKey vrCameraCensusKeyFromText(const char* text) {
    if (!text) return VrCameraCensusKey::Off;
    const char* on = "on";
    for (; *on; ++on, ++text) {
        char c = *text;
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        if (c != *on) return VrCameraCensusKey::Off;
    }
    return *text == 0 ? VrCameraCensusKey::On : VrCameraCensusKey::Off;
}
inline const char* vrCameraCensusKeyName(VrCameraCensusKey key) { return key == VrCameraCensusKey::On ? "on" : "off"; }
constexpr bool vrCameraCensusWantedFor(bool vrProfile, VrCameraCensusKey key) {
    return vrProfile && key == VrCameraCensusKey::On;
}

// ---- the camera struct -------------------------------------------------------------------------------------------
// Camera-relative offsets (the typed table in c2_derive_model.h, which the rig compares these against).
constexpr uint32_t kVrCensusAxes = 0x20;      // source 3x4 view axes, 12 floats (+0x20..+0x4C)
constexpr uint32_t kVrCensusProj = 0x1D0;     // projection 4x4, 16 floats (+0x1D0..+0x20C)
constexpr uint32_t kVrCensusFlags = 0x250;    // dirty flag word: bit 4 = projection dirty
constexpr uint32_t kVrCensusNear = 0x254;
constexpr uint32_t kVrCensusFar = 0x258;
constexpr uint32_t kVrCensusAspect = 0x260;
constexpr uint32_t kVrCensusKind = 0x264;
constexpr uint32_t kVrCensusFov = 0x280;      // the vertical field of view, radians (tan(fov/2) is the half-height)
constexpr uint32_t kVrCensusBoundX = 0x28C;   // the off-centre bound pair (projection[8] = 2 x boundX)
constexpr uint32_t kVrCensusBoundY = 0x290;
constexpr uint32_t kVrCensusViewportW = 0x2A0;
constexpr uint32_t kVrCensusViewportH = 0x2A4;
constexpr uint32_t kVrCensusFlagProj = 4;
// One SEH-guarded copy of camera+0x20 .. +0x2AF is enough for every field above.
constexpr uint32_t kVrCensusSnapFrom = 0x20;
constexpr uint32_t kVrCensusSnapBytes = 0x290;

struct VrCensusSnap {
    uint8_t bytes[kVrCensusSnapBytes];
    float f(uint32_t cameraOffset) const {
        float v;
        std::memcpy(&v, bytes + (cameraOffset - kVrCensusSnapFrom), sizeof(v));
        return v;
    }
    uint32_t u(uint32_t cameraOffset) const {
        uint32_t v;
        std::memcpy(&v, bytes + (cameraOffset - kVrCensusSnapFrom), sizeof(v));
        return v;
    }
};

// What a camera is, by its fields (A). p0, p5, p8, p9 are the derived projection terms: read AFTER the body they are
// the derivation's result (p8 = 2 boundX and p9 = 2 boundY for a perspective camera, p0 = 1/(tan(fov/2) aspect),
// p5 = 1/tan(fov/2)); before it they may be stale.
struct VrCensusSig {
    uint32_t kind = 0;
    float aspect = 0, nearZ = 0, farZ = 0, fov = 0, boundX = 0, boundY = 0, viewportW = 0, viewportH = 0;
    float p0 = 0, p5 = 0, p8 = 0, p9 = 0;
    uint32_t flags = 0;
};
inline void vrCensusSigFromSnap(const VrCensusSnap& s, VrCensusSig* out) {
    out->kind = s.u(kVrCensusKind);
    out->aspect = s.f(kVrCensusAspect);
    out->nearZ = s.f(kVrCensusNear);
    out->farZ = s.f(kVrCensusFar);
    out->fov = s.f(kVrCensusFov);
    out->boundX = s.f(kVrCensusBoundX);
    out->boundY = s.f(kVrCensusBoundY);
    out->viewportW = s.f(kVrCensusViewportW);
    out->viewportH = s.f(kVrCensusViewportH);
    out->p0 = s.f(kVrCensusProj + 0);
    out->p5 = s.f(kVrCensusProj + 4 * 5);
    out->p8 = s.f(kVrCensusProj + 4 * 8);
    out->p9 = s.f(kVrCensusProj + 4 * 9);
    out->flags = s.u(kVrCensusFlags);
}

// The four tangents the camera's frustum spans at unit distance, {left, right, bottom, top}, from the derived terms
// (E). The game builds the projection from a window [L, R] x [B, T] (c2_derive_model.h buildProjection):
// p0 = 2/(R-L) and p8 = -(R+L)/(R-L), so L = -(1+p8)/p0 and R = (1-p8)/p0; likewise p5 = 2/(T-B), p9 = -(T+B)/(T-B),
// B = -(1+p9)/p5 and T = (1-p9)/p5. A symmetric camera has p8 = p9 = 0 and L = -R, B = -T. Perspective kinds only
// (0 and 3); false otherwise or when a term is degenerate.
inline bool vrCensusTangents(const VrCensusSig& sig, float out[4]) {
    if (sig.kind != 0 && sig.kind != 3) return false;
    if (!std::isfinite(sig.p0) || !std::isfinite(sig.p5) || !std::isfinite(sig.p8) || !std::isfinite(sig.p9)) return false;
    if (sig.p0 == 0.0f || sig.p5 == 0.0f) return false;
    out[0] = -(1.0f + sig.p8) / sig.p0;
    out[1] = (1.0f - sig.p8) / sig.p0;
    out[2] = -(1.0f + sig.p9) / sig.p5;
    out[3] = (1.0f - sig.p9) / sig.p5;
    return true;
}

// Rows 270..273 of the scene constant buffer as the composer (FUN_140596830) writes them for this camera (B): the
// source axes rows (lane 3 masked to zero) times the projection, with the projection's own translation row last -- the
// model's composeSceneCb, term for term. It is what the composer WOULD write given the derived blocks, so it is only
// offered when the projection is clean (flag bit 4 clear): after the body ran, a dirty projection means the composer
// never derived it for this call. Sixteen floats, rows 270, 271, 272, 273 in order.
inline bool vrCensusComposeRows(const VrCensusSnap& snap, float out[16]) {
    if (snap.u(kVrCensusFlags) & kVrCensusFlagProj) return false;
    float s[11], p[16];
    for (uint32_t i = 0; i < 11; ++i) s[i] = snap.f(kVrCensusAxes + 4 * i);
    for (uint32_t i = 0; i < 16; ++i) p[i] = snap.f(kVrCensusProj + 4 * i);
    out[0]  = p[8] * s[8]  + p[0] * s[0] + p[4] * s[4];
    out[1]  = p[9] * s[8]  + p[1] * s[0] + p[5] * s[4];
    out[2]  = p[10] * s[8] + p[2] * s[0] + p[6] * s[4];
    out[3]  = p[11] * s[8] + p[3] * s[0] + p[7] * s[4];
    out[4]  = p[8] * s[9]  + p[0] * s[1] + p[4] * s[5];
    out[5]  = p[9] * s[9]  + p[1] * s[1] + p[5] * s[5];
    out[6]  = p[10] * s[9] + p[2] * s[1] + p[6] * s[5];
    out[7]  = p[11] * s[9] + p[3] * s[1] + p[7] * s[5];
    out[8]  = p[8] * s[10] + p[0] * s[2] + p[4] * s[6];
    out[9]  = p[9] * s[10] + p[1] * s[2] + p[5] * s[6];
    out[10] = p[10] * s[10] + p[2] * s[2] + p[6] * s[6];
    out[11] = p[11] * s[10] + p[3] * s[2] + p[7] * s[6];
    out[12] = p[12];
    out[13] = p[13];
    out[14] = p[14];
    out[15] = p[15];
    for (int i = 0; i < 16; ++i) if (!std::isfinite(out[i])) return false;
    return true;
}

// Two sets of rows are the same camera's when every float is within the tolerance (the offline join's rule, 1e-5).
inline bool vrCensusRowsMatch(const float a[16], const float b[16], float tolerance) {
    for (int i = 0; i < 16; ++i) {
        if (!std::isfinite(a[i]) || !std::isfinite(b[i])) return false;
        if (std::fabs(a[i] - b[i]) > tolerance) return false;
    }
    return true;
}

// The off-centre terms a projection built from these four tangents carries: measured from the eye's own rows
// (flatCameraMeasureRowShift) they read (p8, p9) = (-(R+L)/(R-L), -(U+D)/(U-D)). With the shift EDVR advertised for the
// eye added to both edges of an axis (the host moves the frustum by it) this is what the rows should measure; without
// it, the true frustum's own. The leak is the measured value minus the shifted expectation.
inline bool vrCensusExpectedMeasure(const float frustum[4], float shiftX, float shiftY, double* x, double* y) {
    const double l = static_cast<double>(frustum[0]) + shiftX, r = static_cast<double>(frustum[1]) + shiftX;
    const double d = static_cast<double>(frustum[2]) + shiftY, u = static_cast<double>(frustum[3]) + shiftY;
    if (!(r - l != 0.0) || !(u - d != 0.0)) return false;
    *x = -(r + l) / (r - l);
    *y = -(u + d) / (u - d);
    return std::isfinite(*x) && std::isfinite(*y);
}

// The bounds of what one camera may print: its first-sight line and at most this many "changed:" lines.
constexpr uint32_t kVrCensusMaxChangesPerCamera = 4;

// A camera has changed when a field that does not move between frames did, or a bound moved by more than a rounding. An
// eye camera's aspect and field of view are re-derived every frame from the frustum EDVR hands the game and differ in
// their last bits; those are noise, and they must not use up the four lines a camera may print (kVrCensusFieldEpsilon is
// relative, about sixteen units in the last place, and absolute below one). The bound pair IS the signal: it moves with
// the eye shift every frame, so its threshold is absolute and tight.
constexpr float kVrCensusBoundEpsilon = 1.0e-7f;
constexpr float kVrCensusFieldEpsilon = 2.0e-6f;
inline bool vrCensusFieldMoved(float a, float b) {
    if (std::isnan(a) && std::isnan(b)) return false;
    const float m = std::fabs(a) > std::fabs(b) ? std::fabs(a) : std::fabs(b);
    return !(std::fabs(a - b) <= kVrCensusFieldEpsilon * (m > 1.0f ? m : 1.0f));
}
inline bool vrCensusBoundMoved(float a, float b) { return !(std::fabs(a - b) <= kVrCensusBoundEpsilon); }
inline bool vrCensusSigDiffers(const VrCensusSig& a, const VrCensusSig& b) {
    if (a.kind != b.kind) return true;
    if (vrCensusFieldMoved(a.aspect, b.aspect) || vrCensusFieldMoved(a.nearZ, b.nearZ) || vrCensusFieldMoved(a.farZ, b.farZ) ||
        vrCensusFieldMoved(a.fov, b.fov) || vrCensusFieldMoved(a.viewportW, b.viewportW) ||
        vrCensusFieldMoved(a.viewportH, b.viewportH)) return true;
    return vrCensusBoundMoved(a.boundX, b.boundX) || vrCensusBoundMoved(a.boundY, b.boundY);
}

// ---- where in the frame a call falls ---------------------------------------------------------------------------
// From vrWorldRouteDrawProgress: the detector's tone draw (the HDR scene's tone, the on-foot chain's marker). None when
// the route does not report draw progress (the skeleton, or the detector is off).
enum class VrCensusTone : uint8_t { Before, After, None };
inline const char* vrCensusToneName(VrCensusTone t) {
    return t == VrCensusTone::Before ? "before" : t == VrCensusTone::After ? "after" : "none";
}

// ---- which frames are sampled ------------------------------------------------------------------------------------
// The brief's on-foot frame is "the tone was seen". The detector that reports the tone watches draws for the census in
// every frame, and a cockpit, a hangar and a menu draw the same tone: without a second witness the first three call
// sequences and the first four eye frames would be spent before the commander is on foot. The second witness is Elite's
// own journal (Status.json Flags2 bit 0, journal_watch.h): while it is being read, a frame is sampled only when it says on
// foot; with no journal the tone alone decides, as the brief has it. The journal lags the game by about a second. The third
// is the route's phase (below): the first frames on foot are the route's warm-up and carry none.
enum class VrCensusFoot : uint8_t {
    Off,      // the journal is not being read (disabled, no folder, faults): no second witness
    Unknown,  // read, but no Flags2 in the file: a menu, or shutdown
    No,       // Flags2 says not on foot: a ship, a vehicle
    Yes,      // Flags2 says on foot
};
inline const char* vrCensusFootName(VrCensusFoot f) {
    return f == VrCensusFoot::Yes ? "yes" : f == VrCensusFoot::No ? "no" : f == VrCensusFoot::Unknown ? "unknown" : "off";
}
inline VrCensusFoot vrCensusFootFrom(bool journalActive, bool known, bool onFoot) {
    return !journalActive ? VrCensusFoot::Off : !known ? VrCensusFoot::Unknown : onFoot ? VrCensusFoot::Yes : VrCensusFoot::No;
}
// What the world route chose for a frame (vrWorldRouteWorldPhase, the camera injector's stage 2): whether it is jittering the
// world at all this frame and the raster phase it asked the injector for, in render pixels, positive right/down. The phase is
// the frame's CHOICE, so it is zero for the first frames of a warm-up even while the route jitters; those frames cannot show
// a leak (an eye camera with a zero phase in the world cameras has nothing to leak), and flight 1 spent its whole sample on
// them. `jittering` false means the route is not asking for a phase (key off, not warming or owned, no hook): the frame is
// judged exactly as it was before stage 2.
struct VrCensusPhase {
    bool jittering = false;
    float x = 0.0f, y = 0.0f;
};
constexpr float vrCensusAbs(float v) { return v < 0.0f ? -v : v; }
// A phase that can leak: some axis is not zero. NaN is no phase (every comparison with it is false).
constexpr bool vrCensusPhaseNonZero(const VrCensusPhase& p) { return vrCensusAbs(p.x) + vrCensusAbs(p.y) > 0.0f; }
// While the route jitters a frame is worth sampling only with a non-zero phase; with the route not jittering the phase has no say.
constexpr bool vrCensusPhaseAllowsSample(const VrCensusPhase& p) { return !p.jittering || vrCensusPhaseNonZero(p); }
// A frame the census samples (a call sequence, an eye readback). With the world route's draw progress available it is the
// brief's rule plus the journal's: the tone was seen, and the journal, if it is read, says on foot. With no progress (the
// route does not report, or does not watch draws) there is no tone to see: the journal alone decides, and only a journal
// that positively says on foot does (neither witness would sample the first frames of a session, menu frames, for nothing).
// And, while the route jitters, the frame's phase must be non-zero (vrCensusPhaseAllowsSample). The phase is passed
// explicitly, never defaulted: a caller that forgot it would sample the warm-up frames again.
constexpr bool vrCensusSamplesFrame(bool toneSeen, bool progressAvailable, VrCensusFoot foot, const VrCensusPhase& phase) {
    return (progressAvailable ? (toneSeen && (foot == VrCensusFoot::Yes || foot == VrCensusFoot::Off)) : foot == VrCensusFoot::Yes) &&
           vrCensusPhaseAllowsSample(phase);
}
// Whether a call is worth recording at all: a frame the journal says is not on foot, or the route's zero-phase frame, is never
// sampled, so its calls are only counted.
constexpr bool vrCensusMayRecord(VrCensusFoot foot, const VrCensusPhase& phase) {
    return (foot == VrCensusFoot::Yes || foot == VrCensusFoot::Off) && vrCensusPhaseAllowsSample(phase);
}

// ---- the bounded tables -----------------------------------------------------------------------------------------
// What the detour decided for a kind-3 call in the route's injection mode (FlatCameraObserveCall::role): 0 scene, 1 first-person,
// 2 auxiliary, 255 not a role (every other kind, or the detour is not in injection mode). The log says it in words.
constexpr uint8_t kVrCensusRoleNone = 255;
inline const char* vrCensusRoleName(uint8_t role) {
    return role == 0 ? "scene" : role == 1 ? "fp" : role == 2 ? "aux" : "-";
}

struct VrCensusCall {
    uintptr_t camera = 0;
    uintptr_t view = 0;     // the refresh's second argument: the view (pass) object the camera belongs to
    uint32_t kind = 0;
    uint32_t callerRva = 0;
    uint32_t draw = 0;
    uint32_t preFlags = 0, postFlags = 0;
    bool kindReadable = false, drawKnown = false, postSeen = false, rowsValid = false;
    bool willInject = false;                 // the detour will inject this call with a non-zero phase (the route's stage 2)
    uint8_t role = kVrCensusRoleNone;        // FlatCameraObserveCall::role
    VrCensusTone tone = VrCensusTone::None;
    float rows[16] = {};
    // The camera's source 3x4 view axes after the body ran (camera+0x20..+0x4C). Never printed (a call line is full at 400 characters):
    // an episode matches the temporal pass's chosen rows against them in memory and prints which calls they equal.
    float axes[12] = {};
    bool axesValid = false;
};

// One frame's calls, in order. A frame keeps its first kCapacity calls and counts the rest; it is rolled at the Present
// boundary and is printed only when it is one of the first on-foot frames.
struct VrCensusFrame {
    static constexpr size_t kCapacity = 160;
    VrCensusCall call[kCapacity];
    uint32_t calls = 0;     // owner-thread calls this frame, recorded or not
    uint32_t recorded = 0;
    bool toneSeen = false;  // the tone was seen at some call or eye draw: an on-foot frame (see vrCensusSamplesFrame)
    bool progress = false;  // the world route reported its draw progress at some call or eye draw this frame
    void reset() { calls = 0; recorded = 0; toneSeen = false; progress = false; }
    // The next call's record (initialised), or null when the frame is full; the call is counted either way.
    VrCensusCall* add() {
        ++calls;
        if (recorded >= kCapacity) return nullptr;
        VrCensusCall* c = &call[recorded++];
        *c = VrCensusCall{};
        return c;
    }
    uint32_t truncated() const { return calls - recorded; }
};

struct VrCensusCamera {
    uintptr_t camera = 0;
    uintptr_t firstView = 0, firstCtx = 0;   // the first call's second argument (the view) and first (the view-constant context)
    VrCensusSig firstSig, sig, changeFrom;
    float tan[4] = {};
    bool tanValid = false;
    uint32_t callerRva = 0, firstOrdinal = 0, firstDraw = 0;
    bool firstDrawKnown = false, linePending = false, changePending = false;
    VrCensusTone firstTone = VrCensusTone::None;
    uint64_t firstFrame = 0, changeFrame = 0, calls = 0;
    uint32_t changes = 0, changeLines = 0;
};

// One row per distinct camera pointer, the first kCapacity of a session. A full table counts what it cannot hold.
class VrCensusCameraTable {
public:
    static constexpr size_t kCapacity = 64;
    size_t used() const { return used_; }
    uint64_t overflow() const { return overflow_; }
    VrCensusCamera& at(size_t i) { return entry_[i]; }
    const VrCensusCamera& at(size_t i) const { return entry_[i]; }
    size_t find(uintptr_t camera) const {
        for (size_t i = 0; i < used_; ++i) if (entry_[i].camera == camera) return i;
        return kCapacity;
    }
    // The boundary's two questions per row: does its first-sight line still wait, and may its pending change print?
    // A change past kVrCensusMaxChangesPerCamera is counted (changes) and never printed.
    bool takeCameraLine(size_t i) {
        VrCensusCamera& e = entry_[i];
        if (!e.linePending) return false;
        e.linePending = false;
        return true;
    }
    bool takeChangeLine(size_t i) {
        VrCensusCamera& e = entry_[i];
        if (!e.changePending) return false;
        e.changePending = false;
        if (e.changeLines >= kVrCensusMaxChangesPerCamera) return false;
        ++e.changeLines;
        return true;
    }
    enum class Event : uint8_t { Known, New, Changed, Full };
    // What the call that reported a camera knew: the frame in progress and where in it the call fell. A new row keeps
    // these as its "first call"; a known row takes only the frame (when its signature moved).
    struct Call {
        uint64_t frame = 0;
        uint32_t callerRva = 0, ordinal = 0, draw = 0;
        bool drawKnown = false;
        VrCensusTone tone = VrCensusTone::None;
        uintptr_t view = 0, ctx = 0;
    };
    // One post-half report for `camera`: a new row (its line pending), a known one whose signature moved (a pending
    // change), or the same again.
    Event note(uintptr_t camera, const VrCensusSig& sig, const float tan[4], bool tanValid, const Call& call) {
        size_t i = find(camera);
        if (i == kCapacity) {
            if (used_ >= kCapacity) { ++overflow_; return Event::Full; }
            i = used_++;
            VrCensusCamera& e = entry_[i];
            e = VrCensusCamera{};
            e.camera = camera;
            e.firstView = call.view;
            e.firstCtx = call.ctx;
            e.firstSig = e.sig = sig;
            if (tanValid) std::memcpy(e.tan, tan, sizeof(e.tan));
            e.tanValid = tanValid;
            e.callerRva = call.callerRva;
            e.firstOrdinal = call.ordinal;
            e.firstDraw = call.draw;
            e.firstDrawKnown = call.drawKnown;
            e.firstTone = call.tone;
            e.firstFrame = call.frame;
            e.calls = 1;
            e.linePending = true;
            return Event::New;
        }
        VrCensusCamera& e = entry_[i];
        ++e.calls;
        if (!vrCensusSigDiffers(e.sig, sig)) { e.sig.flags = sig.flags; return Event::Known; }
        ++e.changes;
        if (!e.changePending) { e.changePending = true; e.changeFrom = e.sig; e.changeFrame = call.frame; }
        e.sig = sig;
        return Event::Changed;
    }
private:
    VrCensusCamera entry_[kCapacity];
    size_t used_ = 0;
    uint64_t overflow_ = 0;
};

// Calls on a thread other than the owner's: counted by the detour and offered here, from any thread, so the table is
// lock-free. A slot is claimed (0 -> 1), filled, then published (2) with a release store; readers see a slot only at 2.
// Distinct (thread, camera, kind, caller) tuples, the first kCapacity; the rest are counted in overflow().
class VrCensusOffThread {
public:
    static constexpr size_t kCapacity = 16;
    struct Entry {
        uint32_t thread = 0, kind = 0, callerRva = 0;
        bool kindReadable = false;
        uintptr_t camera = 0;
        uint64_t calls = 0;
    };
    void note(uint32_t thread, uintptr_t camera, uint32_t kind, bool kindReadable, uint64_t callerRva) noexcept {
        total_.fetch_add(1, std::memory_order_relaxed);
        const uint32_t caller32 = static_cast<uint32_t>(callerRva);
        const uint32_t kind32 = kindReadable ? kind : 0xffffffffu;
        for (size_t i = 0; i < kCapacity; ++i) {
            Slot& s = slot_[i];
            uint32_t state = s.state.load(std::memory_order_acquire);
            if (state == 2 && s.thread == thread && s.camera == camera && s.kind == kind32 && s.callerRva == caller32) {
                s.calls.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            if (state == 0 && s.state.compare_exchange_strong(state, 1, std::memory_order_acq_rel)) {
                s.thread = thread;
                s.camera = camera;
                s.kind = kind32;
                s.callerRva = caller32;
                s.calls.store(1, std::memory_order_relaxed);
                s.state.store(2, std::memory_order_release);
                return;
            }
        }
        overflow_.fetch_add(1, std::memory_order_relaxed);
    }
    uint64_t total() const noexcept { return total_.load(std::memory_order_relaxed); }
    uint64_t overflow() const noexcept { return overflow_.load(std::memory_order_relaxed); }
    // The owner thread, once a boundary: the next published slot not yet reported (and marks it reported).
    bool takeNew(Entry* out) {
        for (size_t i = 0; i < kCapacity; ++i) {
            Slot& s = slot_[i];
            if (s.state.load(std::memory_order_acquire) != 2 || s.reported) continue;
            s.reported = true;
            out->thread = s.thread;
            out->kindReadable = s.kind != 0xffffffffu;
            out->kind = s.kind;
            out->callerRva = s.callerRva;
            out->camera = s.camera;
            out->calls = s.calls.load(std::memory_order_relaxed);
            return true;
        }
        return false;
    }
    size_t used() const {
        size_t n = 0;
        for (size_t i = 0; i < kCapacity; ++i) if (slot_[i].state.load(std::memory_order_acquire) == 2) ++n;
        return n;
    }
private:
    struct Slot {
        std::atomic<uint32_t> state{0};
        bool reported = false;   // owner thread only
        uint32_t thread = 0, kind = 0, callerRva = 0;
        uintptr_t camera = 0;
        std::atomic<uint64_t> calls{0};
    };
    Slot slot_[kCapacity];
    std::atomic<uint64_t> total_{0}, overflow_{0};
};

// The 5 s window (owner thread). Zeros are printed: an absent line is what "the census never ran" looks like.
struct VrCensusWindow {
    static constexpr size_t kCallers = 16, kCameras = 64;
    struct Caller { uint32_t rva = 0; uint64_t n = 0; };
    uint64_t frames = 0, calls = 0, posts = 0, stale = 0;
    uint64_t injCalls = 0;                  // calls the observer heard with willInject set (the route's stage 2)
    uint64_t kinds[8] = {};                 // 0..5, 6 = other, 7 = unreadable
    Caller callers[kCallers];
    uint32_t callerCount = 0;
    uint64_t callerOverflow = 0;
    uintptr_t cameras[kCameras] = {};
    uint32_t cameraCount = 0;
    uint64_t cameraOverflow = 0;
    uint64_t toneBefore = 0, toneAfter = 0, toneNone = 0;
    uint64_t toneFrames = 0;                // frames in which the tone was seen, sampled or not
    uint64_t onFootFrames = 0;              // ... of which the census samples (the journal, if it is read, says on foot)
    uint64_t eyeDraws = 0, eyeOnFoot = 0;   // eye composite draws reported, and those of a sampled frame
    uint32_t windows = 0;                   // 5 s windows this line covers
    bool progressSeen = false;              // vrWorldRouteDrawProgress answered at least once
    void noteCall(bool kindReadable, uint32_t kind, uint32_t callerRva, uintptr_t camera, VrCensusTone tone, bool lapsed,
                  bool willInject) {
        ++calls;
        if (lapsed) ++stale;
        if (willInject) ++injCalls;
        ++kinds[!kindReadable ? 7 : kind <= 5 ? kind : 6];
        size_t i = 0;
        for (; i < callerCount; ++i) if (callers[i].rva == callerRva) break;
        if (i < callerCount) ++callers[i].n;
        else if (callerCount < kCallers) { callers[callerCount].rva = callerRva; callers[callerCount].n = 1; ++callerCount; }
        else ++callerOverflow;
        size_t c = 0;
        for (; c < cameraCount; ++c) if (cameras[c] == camera) break;
        if (c == cameraCount) { if (cameraCount < kCameras) cameras[cameraCount++] = camera; else ++cameraOverflow; }
        if (tone == VrCensusTone::Before) ++toneBefore; else if (tone == VrCensusTone::After) ++toneAfter; else ++toneNone;
    }
    void reset() { *this = VrCensusWindow{}; }
};
// The first kVrCensusEveryWindow windows (three minutes) print each; after that one line in kVrCensusThinTo covers that
// many windows (the counters keep adding up, so nothing is lost), which keeps a long session's 5 s lines near a hundred.
constexpr uint32_t kVrCensusEveryWindow = 36;
constexpr uint32_t kVrCensusThinTo = 12;
constexpr bool vrCensusWindowPrints(uint32_t tick) {   // tick: 1-based count of 5 s windows since the census started
    return tick <= kVrCensusEveryWindow || tick % kVrCensusThinTo == 0;
}

// ---- the budget ---------------------------------------------------------------------------------------------------
// A line class has a cap; past it a line is counted, not printed. The sum of the caps is the census's own worst case
// (about 700 lines, three call sequences of the full 160 calls). A real session is near the sum of what it saw: one 5 s
// line a window for three minutes and one a minute after (about fifty in ten minutes), a line per camera (a dozen or
// two), three call sequences of a frame's length (about 110 calls each) and eight eye draws: about four hundred. The EPISODES
// (below) have classes of their own, so they can never starve the first three sequences or the 5 s lines, and the other way round:
// an episode prints a header, up to kVrCensusEpisodeLines call lines, the pass's rows and its join (at most kVrCensusJoinRows
// signatures and four rows lines), so ten of them are at most 200 lines of the Episode class and 1,200 of the EpisodeCall class.
// The three lines a 5 s window adds (the episodes' counters, the on-foot naming runs, the detour's CPU) are classes of their own as well.
constexpr uint32_t kVrCensusMaxEpisodes = 10;     // episodes a session samples
constexpr uint32_t kVrCensusEpisodeLines = 120;   // call lines an episode prints at most (a cockpit frame has hundreds of refresh calls)
constexpr uint32_t kVrCensusJoinRows = 12;        // distinct (depth, vertex shader, pixel shader) signatures an episode's join keeps
enum class VrCensusLines : uint8_t { Window, Camera, Changed, Call, Eye, Thread, Info, Episode, EpisodeCall, Runs, Cpu, Counters, kCount };
inline const char* vrCensusLinesName(VrCensusLines c) {
    switch (c) {
        case VrCensusLines::Window: return "5s";
        case VrCensusLines::Camera: return "camera";
        case VrCensusLines::Changed: return "changed";
        case VrCensusLines::Call: return "call";
        case VrCensusLines::Eye: return "eye";
        case VrCensusLines::Thread: return "other-thread";
        case VrCensusLines::Info: return "info";
        case VrCensusLines::Episode: return "episode";
        case VrCensusLines::EpisodeCall: return "episode-call";
        case VrCensusLines::Runs: return "runs";
        case VrCensusLines::Cpu: return "detour";
        case VrCensusLines::Counters: return "episodes";
        case VrCensusLines::kCount: break;
    }
    return "?";
}
struct VrCensusBudget {
    static constexpr uint32_t kCap[static_cast<size_t>(VrCensusLines::kCount)] = {
        100, 64, 24, 480, 16, 16, 24,
        kVrCensusMaxEpisodes * (1 + 1 + 1 + kVrCensusJoinRows + 4 + 1) + 20,   // episodes: header, pass rows, join draws, signatures, their rows lines, the overflow note; 20 spare
        kVrCensusMaxEpisodes * kVrCensusEpisodeLines,                       // episodes' call lines
        100, 100, 100};                                                     // the window's runs, detour and episode-counter lines (as many as its 5 s lines)
    uint32_t used[static_cast<size_t>(VrCensusLines::kCount)] = {};
    uint32_t suppressed[static_cast<size_t>(VrCensusLines::kCount)] = {};
    bool take(VrCensusLines c) {
        const size_t i = static_cast<size_t>(c);
        if (used[i] < kCap[i]) { ++used[i]; return true; }
        ++suppressed[i];
        return false;
    }
    uint32_t total() const {
        uint32_t n = 0;
        for (size_t i = 0; i < static_cast<size_t>(VrCensusLines::kCount); ++i) n += used[i];
        return n;
    }
    static uint32_t capTotal() {
        uint32_t n = 0;
        for (size_t i = 0; i < static_cast<size_t>(VrCensusLines::kCount); ++i) n += kCap[i];
        return n;
    }
};
constexpr uint32_t kVrCensusMaxSequences = 3;   // on-foot frames whose whole call sequence is printed
constexpr uint32_t kVrCensusMaxEyeFrames = 4;   // on-foot frames whose eye draws are read back
constexpr uint32_t kVrCensusMaxEyeDraws = 8;    // and the eye draws in all: each is a staging readback that stalls once
constexpr size_t kVrCensusLineBytes = 400;      // a line is at most this many characters
// The frame that just ended prints its call sequence when the census samples it (vrCensusSamplesFrame: the tone was seen
// and the journal, if it is read, says on foot) and fewer than kVrCensusMaxSequences have printed. The recording of calls
// stops with the last sequence: a call is then only counted.
constexpr bool vrCensusPrintsSequence(bool sampledFrame, uint32_t sequencesLogged) {
    return sampledFrame && sequencesLogged < kVrCensusMaxSequences;
}

// Which eye draws are read back: the first kVrCensusMaxEyeDraws draws of the first kVrCensusMaxEyeFrames on-foot frames.
class VrCensusEyeBudget {
public:
    bool take(uint64_t frame) {
        if (draws_ >= kVrCensusMaxEyeDraws) return false;
        bool known = false;
        for (uint32_t i = 0; i < frames_; ++i) if (frame_[i] == frame) known = true;
        if (!known) {
            if (frames_ >= kVrCensusMaxEyeFrames) return false;
            frame_[frames_++] = frame;
        }
        ++draws_;
        return true;
    }
    uint32_t draws() const { return draws_; }
    uint32_t frames() const { return frames_; }
private:
    uint64_t frame_[kVrCensusMaxEyeFrames] = {};
    uint32_t frames_ = 0, draws_ = 0;
};

// ---- episodes (design-world-camera-motion-2026-09-30.md section 6, Phase 0) --------------------------------------------
// Until now the census sampled the session's first three on-foot frames and never an aboard frame: it could see neither a map nor the cockpit. An
// EPISODE is one frame sampled kVrCensusEpisodeDelay frames after a trigger, whatever that frame is:
//   - every refresh call of it is recorded (aboard ones included) in a buffer of its own, and tallied by kind and caller over ALL of them;
//   - at its first draw into each kind of depth the 2D screen's size, or an eye's (per eye where the depth probe knows it) the vertex and pixel shader,
//     whether the draw writes depth, the size of the VS constant buffer at slot 1 and its rows 270..273, read back, are kept (the JOIN); another
//     vertex and pixel shader pair into the same depth is a row of its own with the same facts and no readback of rows;
//   - at the boundary that ends it the join's rows and the temporal pass's chosen rows are matched, in memory, against every call recorded, and it prints.
// A trigger is: the journal's on-foot reading flips either way; the naming of the 2D screen's source flips and holds for kVrCensusNamingHold frames;
// Status.json's GuiFocus changes between two known values; or the census turns on. A trigger that arrives while an episode is armed, or after
// kVrCensusMaxEpisodes were taken, is counted and never sampled, and the census says so once.
constexpr uint32_t kVrCensusEpisodeDelay = 30;    // frames from a trigger to the frame it samples
constexpr uint32_t kVrCensusNamingHold = 3;       // frames a flip of the naming must hold to be a trigger (the world route's grace)
constexpr float kVrCensusRowsTol = 1.0e-5f;       // two sets of rows 270..273 are one camera's within this (the reader's own join rule)
constexpr float kVrCensusAxesTol = 1.0e-4f;       // the pass's rows and a call's view axes are one pose within this

enum class VrCensusTriggerKind : uint8_t { KeyOn, Foot, Naming, Gui };
struct VrCensusTrigger {
    VrCensusTriggerKind kind = VrCensusTriggerKind::KeyOn;
    uint32_t from = 0, to = 0;   // Foot: 0 not on foot, 1 on foot. Naming: 0 unnamed, 1 named. Gui: the GuiFocus values. KeyOn: unused
};

class VrCensusEpisodes {
public:
    enum class State : uint8_t { Idle, Armed, Live };
    // What the census reads at a boundary about the frame that has just ended.
    struct Inputs {
        VrCensusFoot foot = VrCensusFoot::Off;   // the journal's word
        bool named = false;                      // a draw named the 2D screen's source in the frame (ui_layer.h uiLayerLastFrameNamed)
        bool guiKnown = false;                   // Status.json's GuiFocus is readable (journalGuiFocus)
        uint32_t gui = 0;
    };
    State state() const { return state_; }
    bool armed() const { return state_ == State::Armed; }
    bool live() const { return state_ == State::Live; }
    bool idle() const { return state_ == State::Idle; }
    uint32_t started() const { return started_; }                 // episodes armed this session (the one in progress, or the last, is this one)
    uint32_t triggers() const { return triggers_; }               // triggers seen, sampled or not
    uint32_t skipped() const { return skipped_; }                 // ...of which found an episode armed, or the cap reached
    uint64_t armedFrame() const { return armedFrame_; }
    uint64_t sampleFrame() const { return sampleFrame_; }
    const VrCensusTrigger& trigger() const { return trigger_; }
    const char* stateName() const { return state_ == State::Live ? "live" : state_ == State::Armed ? "armed" : "idle"; }
    // The census turned on (or on again): forget the readings a flip would be judged against, so no flip is read across a gap in which the census was
    // not running. The counters and the session's cap stay.
    void restart() {
        state_ = State::Idle;
        footSeen_ = guiSeen_ = namedSeen_ = false;
        namedRun_ = 0;
    }
    // The key-on trigger, at the boundary that starts frame `frame`.
    void keyOn(uint64_t frame) { fire(VrCensusTrigger{VrCensusTriggerKind::KeyOn, 0, 0}, frame); }
    // One boundary. `frame` is the number of the frame that STARTS here (1-based); `ended` is false at the boundary that turns the census on, when no
    // frame has ended yet. Judges the readings of the frame that ended (a trigger arms an episode for frame + kVrCensusEpisodeDelay) and returns true
    // when the armed frame is the one that starts now (the episode goes live: finish() ends it).
    bool boundary(uint64_t frame, bool ended, const Inputs& in) {
        if (ended) {
            if (in.foot == VrCensusFoot::Yes || in.foot == VrCensusFoot::No) {   // the journal's two known words; unknown and off say nothing
                const bool on = in.foot == VrCensusFoot::Yes;
                if (footSeen_ && on != footOn_) fire(VrCensusTrigger{VrCensusTriggerKind::Foot, footOn_ ? 1u : 0u, on ? 1u : 0u}, frame);
                footSeen_ = true;
                footOn_ = on;
            }
            // The naming: a flip counts once the new value has held for kVrCensusNamingHold frames in a row (a blip shorter than that is nothing).
            if (!namedSeen_) {
                namedSeen_ = true;
                namedHeld_ = namedCand_ = in.named;
                namedRun_ = 0;
            } else if (in.named == namedHeld_) {
                namedRun_ = 0;
            } else {
                if (in.named == namedCand_ && namedRun_ > 0) ++namedRun_; else { namedCand_ = in.named; namedRun_ = 1; }
                if (namedRun_ >= kVrCensusNamingHold) {
                    const bool from = namedHeld_;
                    namedHeld_ = in.named;
                    namedRun_ = 0;
                    fire(VrCensusTrigger{VrCensusTriggerKind::Naming, from ? 1u : 0u, in.named ? 1u : 0u}, frame);
                }
            }
            if (in.guiKnown) {   // a change between two KNOWN values: a missing Status.json (a menu, a shutdown) is no change
                if (guiSeen_ && in.gui != gui_) fire(VrCensusTrigger{VrCensusTriggerKind::Gui, gui_, in.gui}, frame);
                guiSeen_ = true;
                gui_ = in.gui;
            }
        }
        if (state_ == State::Armed && frame >= sampleFrame_) { state_ = State::Live; return true; }
        return false;
    }
    // The boundary that ends the live frame has printed it.
    void finish() { if (state_ == State::Live) state_ = State::Idle; }
    // The census went off: whatever was armed or live is dropped (the count of episodes taken stays).
    void abandon() { state_ = State::Idle; }
    // The first trigger that was skipped, said once: true exactly once, with why ("an episode was armed" or "the session's episodes were taken").
    bool takeSkipNote(const char** why) {
        if (skipNoted_ || !skipped_) return false;
        skipNoted_ = true;
        *why = firstSkipWasCap_ ? "the session's episodes were all taken" : "an episode was armed";
        return true;
    }
private:
    void fire(const VrCensusTrigger& t, uint64_t frame) {
        ++triggers_;
        if (state_ != State::Idle || started_ >= kVrCensusMaxEpisodes) {
            if (!skipped_) firstSkipWasCap_ = state_ == State::Idle;
            ++skipped_;
            return;
        }
        state_ = State::Armed;
        trigger_ = t;
        armedFrame_ = frame;
        sampleFrame_ = frame + kVrCensusEpisodeDelay;
        ++started_;
    }
    State state_ = State::Idle;
    VrCensusTrigger trigger_;
    uint64_t armedFrame_ = 0, sampleFrame_ = 0;
    uint32_t started_ = 0, triggers_ = 0, skipped_ = 0;
    bool skipNoted_ = false, firstSkipWasCap_ = false;
    bool footSeen_ = false, footOn_ = false;
    bool guiSeen_ = false;
    uint32_t gui_ = 0;
    bool namedSeen_ = false, namedHeld_ = false, namedCand_ = false;
    uint32_t namedRun_ = 0;
};

// The calls of an episode's frame: every one is recorded (up to the buffer) and tallied by kind and caller over all of them, recorded or not. A buffer of its
// own so the first-three-frames' sequences stay exactly as they were (160 calls); an episode of a cockpit frame needs hundreds.
struct VrCensusEpisodeFrame {
    static constexpr size_t kCapacity = 640, kCallers = 16;
    struct Caller { uint32_t rva = 0; uint64_t n = 0; };
    VrCensusCall call[kCapacity];
    uint32_t calls = 0, recorded = 0;
    uint32_t kinds[8] = {};                 // 0..5, 6 = other, 7 = unreadable: every call of the frame
    Caller callers[kCallers];
    uint32_t callerCount = 0;
    uint64_t callerOverflow = 0;            // calls whose caller did not fit the table
    void reset() {
        calls = recorded = callerCount = 0;
        callerOverflow = 0;
        std::memset(kinds, 0, sizeof(kinds));
    }
    // The next call's record (initialised), or null when the buffer is full; the call is counted and tallied either way.
    VrCensusCall* add(bool kindReadable, uint32_t kind, uint32_t callerRva) {
        ++calls;
        ++kinds[!kindReadable ? 7 : kind <= 5 ? kind : 6];
        size_t i = 0;
        for (; i < callerCount; ++i) if (callers[i].rva == callerRva) break;
        if (i < callerCount) ++callers[i].n;
        else if (callerCount < kCallers) { callers[callerCount].rva = callerRva; callers[callerCount].n = 1; ++callerCount; }
        else ++callerOverflow;
        if (recorded >= kCapacity) return nullptr;
        VrCensusCall* c = &call[recorded++];
        *c = VrCensusCall{};
        return c;
    }
    uint32_t truncated() const { return calls - recorded; }
};

// Which recorded calls composed exactly these rows (rows 270..273, sixteen floats, within `tol`): the number of them, and the first `max` ordinals (1-based,
// the n= of the call line). A call whose rows are not valid (a dirty projection) matches nothing.
inline uint32_t vrCensusMatchRows(const VrCensusEpisodeFrame& f, const float rows[16], float tol, uint32_t* ordinals, uint32_t max) {
    uint32_t count = 0;
    for (uint32_t i = 0; i < f.recorded; ++i) {
        if (!f.call[i].rowsValid || !vrCensusRowsMatch(f.call[i].rows, rows, tol)) continue;
        if (count < max) ordinals[count] = i + 1;
        ++count;
    }
    return count;
}

// The temporal pass's chosen rows (float 932 of the scene block, three 3x4 rows) against the calls' view axes (camera+0x20, three 3x4 rows). Only the
// rotation (the nine floats of lanes 0..2) is compared, either as it is or transposed: which convention the game's block uses is one of the things the
// flight settles, so the nearer reading of each call decides and is named (how). The translation lane is not compared.
struct VrCensusAxesMatch {
    static constexpr uint32_t kMax = 6;
    uint32_t count = 0;                  // calls whose axes equal the rows within the tolerance
    uint32_t ordinals[kMax] = {};        // the first kMax of them, 1-based
    uint32_t nearest = 0;                // the call whose axes are nearest (0: no call had axes)
    float nearestDiff = 0.0f;            // the largest |difference| of the nine floats, nearer reading
    bool nearestTransposed = false;      // the nearest call matched best with its axes transposed
};
inline float vrCensusAxesDistance(const float chosen[12], const float axes[12], bool transposed) {
    float worst = 0.0f;
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            float d = std::fabs(chosen[r * 4 + c] - axes[transposed ? c * 4 + r : r * 4 + c]);
            if (d != d) d = 3.0e38f;         // a NaN is the farthest there is
            if (d > worst) worst = d;
        }
    }
    return worst;
}
inline VrCensusAxesMatch vrCensusMatchAxes(const VrCensusEpisodeFrame& f, const float chosen[12], float tol) {
    VrCensusAxesMatch m;
    bool haveNearest = false;
    for (uint32_t i = 0; i < f.recorded; ++i) {
        if (!f.call[i].axesValid) continue;
        const float plain = vrCensusAxesDistance(chosen, f.call[i].axes, false);
        const float turned = vrCensusAxesDistance(chosen, f.call[i].axes, true);
        const bool useTurned = turned < plain;
        const float d = useTurned ? turned : plain;
        if (d <= tol) {
            if (m.count < VrCensusAxesMatch::kMax) m.ordinals[m.count] = i + 1;
            ++m.count;
        }
        // The nearest call; of two equally near the first, but one that fits as it is beats one that fits only transposed.
        if (!haveNearest || d < m.nearestDiff || (d == m.nearestDiff && !useTurned && m.nearestTransposed)) {
            haveNearest = true;
            m.nearest = i + 1;
            m.nearestDiff = d;
            m.nearestTransposed = useTurned;
        }
    }
    return m;
}

// Which of an episode's recorded calls print, when there are more than the lines allow (`cap`): the calls that matched something first (the answer to
// "which camera drew this"), then every kind-5 call (the eyes'), then the first call of each run of one (kind, caller, tone), then the rest in order. Selected
// calls print in call order whatever put them in. `matched` is per recorded call. Returns how many were selected.
inline uint32_t vrCensusSelectCalls(const VrCensusEpisodeFrame& f, const bool* matched, uint32_t cap, bool* selected) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < f.recorded; ++i) selected[i] = false;
    auto take = [&](uint32_t i) { if (!selected[i] && n < cap) { selected[i] = true; ++n; } };
    for (uint32_t i = 0; i < f.recorded && n < cap; ++i) if (matched[i]) take(i);
    for (uint32_t i = 0; i < f.recorded && n < cap; ++i) if (f.call[i].kindReadable && f.call[i].kind == 5) take(i);
    for (uint32_t i = 0; i < f.recorded && n < cap; ++i) {
        const VrCensusCall& c = f.call[i];
        const bool runStart = i == 0 || f.call[i - 1].kind != c.kind || f.call[i - 1].kindReadable != c.kindReadable ||
                              f.call[i - 1].callerRva != c.callerRva || f.call[i - 1].tone != c.tone;
        if (runStart) take(i);
    }
    for (uint32_t i = 0; i < f.recorded && n < cap; ++i) take(i);
    return n;
}

// The join: where an episode's draws went, by kind of depth. A DEPTH is the screen's (a draw into a depth of the 2D screen's size) or an eye's (an
// eye-sized depth; the eye where the depth probe's scene pair says it, else unknown). Each distinct (depth, vertex shader, pixel shader) is a row, the first
// kVrCensusJoinRows of an episode: its first draw's ordinal, how many draws it took, and what the first draw bound. The FIRST row of each depth also has the
// b1 rows 270..273 read back (at most four readbacks an episode: each stalls once).
enum class VrCensusJoinDepth : uint8_t { Screen, Eye0, Eye1, EyeUnknown, kCount };
inline const char* vrCensusJoinDepthName(VrCensusJoinDepth d) { return d == VrCensusJoinDepth::Screen ? "screen" : "eye"; }
inline const char* vrCensusJoinEyeName(VrCensusJoinDepth d) {
    return d == VrCensusJoinDepth::Eye0 ? "0" : d == VrCensusJoinDepth::Eye1 ? "1" : "-";
}
struct VrCensusJoinRow {
    VrCensusJoinDepth depth = VrCensusJoinDepth::Screen;
    uint32_t w = 0, h = 0;          // the depth's size
    uint64_t vs = 0, ps = 0;        // the bound shaders' content hashes (the binding shadow's; 0 when the shadow does not know them)
    uint32_t firstDraw = 0;         // the route's draw ordinal of the row's first draw
    uint64_t draws = 0;             // draws of this signature this frame, the first included
    int8_t depthWrite = -1;         // does the draw's depth-stencil state write depth: -1 not read, 0 no, 1 yes
    bool b1Bound = false;           // a constant buffer was bound at VS slot 1
    uint64_t b1 = 0;                // ...its pointer, as an identity
    uint32_t b1First = 0, b1Bytes = 0;   // ...its first constant (16-byte units) and its byte width
    bool rowsAsked = false;         // this row was the first of its depth: its rows 270..273 were read back
    bool rowsRead = false;          // ...and the readback worked
    const char* why = nullptr;      // why the rows are absent: "skip" (not the depth's first row), or the readback's reason
    float rows[16] = {};
    uint32_t matchCount = 0;        // recorded calls whose composed rows equal rows (set at the end of the frame)
    uint32_t matches[6] = {};
};
class VrCensusJoin {
public:
    VrCensusJoinRow row[kVrCensusJoinRows];
    uint32_t used = 0;
    uint64_t overflowDraws = 0;     // draws of a signature past the table
    uint32_t overflowRows = 0;      // distinct signatures past the table
    uint64_t seen = 0;              // every draw the per-draw hook was handed this frame, relevant or not: what tells "the hook never ran" from "no draw joined"
    uint32_t views = 0;             // distinct depth views the hook resolved
    void begin() {
        used = 0;
        overflowDraws = 0;
        overflowRows = 0;
        seen = 0;
        views = 0;
    }
    // Draws into a depth that is the screen's or an eye's: the signatures' draws and those past the table.
    uint64_t relevantDraws() const {
        uint64_t n = overflowDraws;
        for (uint32_t i = 0; i < used; ++i) n += row[i].draws;
        return n;
    }
    VrCensusJoinRow* find(VrCensusJoinDepth d, uint64_t vs, uint64_t ps) {
        for (uint32_t i = 0; i < used; ++i) if (row[i].depth == d && row[i].vs == vs && row[i].ps == ps) return &row[i];
        return nullptr;
    }
    bool hasDepth(VrCensusJoinDepth d) const {
        for (uint32_t i = 0; i < used; ++i) if (row[i].depth == d) return true;
        return false;
    }
    // A new signature's row (initialised), or null when the table is full: the signature is counted in overflowRows.
    VrCensusJoinRow* add(VrCensusJoinDepth d, uint32_t w, uint32_t h, uint64_t vs, uint64_t ps, uint32_t draw) {
        if (used >= kVrCensusJoinRows) { ++overflowRows; ++overflowDraws; return nullptr; }
        VrCensusJoinRow* r = &row[used++];
        *r = VrCensusJoinRow{};
        r->depth = d; r->w = w; r->h = h; r->vs = vs; r->ps = ps; r->firstDraw = draw; r->draws = 1;
        return r;
    }
};

// What the join knows of a depth target the draws bound, kept for the episode's frame so a view is resolved once however many draws it takes. Keyed by the
// view's pointer (identity only, never dereferenced here).
class VrCensusDepthCache {
public:
    static constexpr size_t kSize = 8;
    struct Entry { const void* dsv = nullptr; bool relevant = false; VrCensusJoinDepth depth = VrCensusJoinDepth::Screen; uint32_t w = 0, h = 0; };
    void clear() { used_ = 0; }
    const Entry* find(const void* dsv) const {
        for (uint32_t i = 0; i < used_; ++i) if (e_[i].dsv == dsv) return &e_[i];
        return nullptr;
    }
    // Remembered; a ninth view replaces the last (the table is for the handful of depth targets a frame binds).
    const Entry* put(const void* dsv, bool relevant, VrCensusJoinDepth depth, uint32_t w, uint32_t h) {
        Entry& e = e_[used_ < kSize ? used_++ : kSize - 1];
        e.dsv = dsv; e.relevant = relevant; e.depth = depth; e.w = w; e.h = h;
        return &e;
    }
private:
    Entry e_[kSize];
    uint32_t used_ = 0;
};

// The on-foot NAMING RUNS (H2: in an on-foot world the naming never drops for three frames). Over the frames the journal says on foot, a run is a stretch of
// consecutive frames that all named the 2D screen's source (named) or none did (unnamed). A run is counted when it ENDS, in the bin of its length: 1, 2, 3,
// 4-8, 9-30, 31-89, 90+ frames. The run in progress when a window prints is carried into the next (never counted twice); the longest of each kind includes it.
// A frame the journal does not say on foot ends the run.
struct VrCensusRuns {
    static constexpr int kBins = 7;
    uint64_t named[kBins] = {}, unnamed[kBins] = {};
    uint32_t longestNamed = 0, longestUnnamed = 0;   // since the last reset; the open run counts
    uint64_t frames = 0;                             // on-foot frames since the last reset
    int8_t open = -1;                                // the run in progress: -1 none, 0 unnamed, 1 named
    uint32_t openLength = 0;
    static constexpr int bin(uint32_t length) {
        return length <= 1 ? 0 : length == 2 ? 1 : length == 3 ? 2 : length <= 8 ? 3 : length <= 30 ? 4 : length <= 89 ? 5 : 6;
    }
    static const char* binName(int b) {
        static const char* const names[kBins] = {"1", "2", "3", "4-8", "9-30", "31-89", "90+"};
        return b >= 0 && b < kBins ? names[b] : "?";
    }
    void noteFrame(bool onFoot, bool wasNamed) {
        if (!onFoot) { endRun(); return; }
        ++frames;
        const int8_t kind = wasNamed ? 1 : 0;
        if (open == kind) ++openLength; else { endRun(); open = kind; openLength = 1; }
        uint32_t& longest = wasNamed ? longestNamed : longestUnnamed;
        if (openLength > longest) longest = openLength;
    }
    void endRun() {
        if (open >= 0 && openLength) ++(open ? named : unnamed)[bin(openLength)];
        open = -1;
        openLength = 0;
    }
    // A window printed: its counts start again, the run in progress carries on (and is the longest of its kind so far in the new window).
    void resetWindow() {
        std::memset(named, 0, sizeof(named));
        std::memset(unnamed, 0, sizeof(unnamed));
        longestNamed = longestUnnamed = 0;
        frames = 0;
        if (open == 1) longestNamed = openLength; else if (open == 0) longestUnnamed = openLength;
    }
};

// The refresh detour's own CPU, as the observer's two halves see it (D). One call in kEvery is timed (the clock is read at the start and end of each half
// of that call only), by whether the detour injected for the call (the route's phase) or only observed it. The detour's own prologue and the game's body
// are outside both halves: this is the CPU the census adds to the detour, and what a quiet observer could not undercut by more than the census's own work.
struct VrCensusCpu {
    static constexpr uint32_t kEvery = 16;
    struct Mode {
        uint64_t sampled = 0, preTicks = 0, preMax = 0;          // calls timed, and their pre half's ticks (sum, longest)
        uint64_t postSampled = 0, postTicks = 0, postMax = 0;    // ...and the post halves that ran for them
    };
    Mode mode[2];        // 0 observed only, 1 injected (willInject)
    uint32_t tick = 0;   // calls seen (never reset): the 1-in-kEvery pick
    bool pick() { return (++tick % kEvery) == 0; }
    void notePre(bool injected, uint64_t ticks) {
        Mode& m = mode[injected ? 1 : 0];
        ++m.sampled;
        m.preTicks += ticks;
        if (ticks > m.preMax) m.preMax = ticks;
    }
    void notePost(bool injected, uint64_t ticks) {
        Mode& m = mode[injected ? 1 : 0];
        ++m.postSampled;
        m.postTicks += ticks;
        if (ticks > m.postMax) m.postMax = ticks;
    }
    void resetWindow() { mode[0] = Mode{}; mode[1] = Mode{}; }
};

// ---- the text of every line -----------------------------------------------------------------------------------
// Each returns snprintf's length and is handed at most kVrCensusLineBytes + 1 bytes. No field ever holds a space inside
// a (..) or [..] value, so edvr_log.py can split a line into key=value tokens.
namespace vrcensus_detail {
struct Out {
    char* p; size_t size; size_t n = 0;
    Out(char* out, size_t cap) : p(out), size(cap) { if (cap) out[0] = 0; }
    template <class... A> void put(const char* fmt, A... args) {
        if (n + 1 >= size) return;
        const int w = std::snprintf(p + n, size - n, fmt, args...);
        if (w > 0) n += static_cast<size_t>(w) < size - n ? static_cast<size_t>(w) : size - n - 1;
    }
    // A float as the log prints it: seven significant digits, "nan" for anything not finite.
    void f(float v) { if (std::isfinite(v)) put("%.7g", static_cast<double>(v)); else put("nan"); }
    void d(double v) { if (std::isfinite(v)) put("%.7g", v); else put("nan"); }
    void e(double v) { if (std::isfinite(v)) put("%.3e", v); else put("nan"); }
    void list(const float* v, int count) {
        put("[");
        for (int i = 0; i < count; ++i) { if (i) put(","); f(v[i]); }
        put("]");
    }
    void pair(float a, float b) { put("("); f(a); put(","); f(b); put(")"); }
    void draw(bool known, uint32_t draw) { if (known) put("%u", draw); else put("-"); }
    // One phase axis in render pixels, four decimals ("0.2520"); whatever rounds to zero is "0.0000", never "-0.0000".
    void px(float v) {
        if (!std::isfinite(v)) { put("nan"); return; }
        char text[24];
        std::snprintf(text, sizeof(text), "%.4f", static_cast<double>(v));
        put("%s", std::strcmp(text, "-0.0000") == 0 ? "0.0000" : text);
    }
    // The frame's phase as the reader takes it: "X,Y" in render pixels while the route jitters, "-" when it does not (so a
    // zero phase of a jittering warm-up frame and a route that is not jittering at all read differently).
    void phase(const VrCensusPhase& ph) {
        if (!ph.jittering) { put("-"); return; }
        px(ph.x); put(","); px(ph.y);
    }
    // An episode's trigger as one token: key-on, foot:no>yes, naming:unnamed>named, gui:0>6.
    void trigger(const VrCensusTrigger& t) {
        switch (t.kind) {
            case VrCensusTriggerKind::KeyOn: put("key-on"); break;
            case VrCensusTriggerKind::Foot: put("foot:%s>%s", t.from ? "yes" : "no", t.to ? "yes" : "no"); break;
            case VrCensusTriggerKind::Naming: put("naming:%s>%s", t.from ? "named" : "unnamed", t.to ? "named" : "unnamed"); break;
            case VrCensusTriggerKind::Gui: put("gui:%u>%u", t.from, t.to); break;
        }
    }
    // The calls' kinds as the 5 s line and an episode's header write them: `0:12,3:290,5:10`, `other` and `unreadable` named, `-` for none.
    template <class T> void kinds(const T (&n)[8]) {
        bool any = false;
        for (int k = 0; k < 8; ++k) {
            if (!n[k]) continue;
            if (k < 6) put("%s%d:%llu", any ? "," : "", k, static_cast<unsigned long long>(n[k]));
            else put("%s%s:%llu", any ? "," : "", k == 6 ? "other" : "unreadable", static_cast<unsigned long long>(n[k]));
            any = true;
        }
        if (!any) put("-");
    }
    // Recorded-call ordinals as the join prints them: `98,101` and, when more matched than the list holds, `1,2,3,4,5,6+48`; `-` for none.
    void ordinals(const uint32_t* list, uint32_t listed, uint32_t total) {
        if (!total) { put("-"); return; }
        const uint32_t shown = listed < total ? listed : total;
        for (uint32_t i = 0; i < shown; ++i) put("%s%u", i ? "," : "", list[i]);
        if (total > shown) put("+%u", total - shown);
    }
};
}  // namespace vrcensus_detail

struct VrCensusWindowText {
    const char* hook = "pending";
    VrCensusFoot foot = VrCensusFoot::Off;   // what the journal said when the window closed
    uint64_t offThread = 0, offThreadOverflow = 0;
    uint32_t camerasTotal = 0;
    uint64_t cameraTableOverflow = 0;
};
inline int vrCensusFormatWindow(char* out, size_t size, const VrCensusWindow& w, const VrCensusWindowText& t) {
    vrcensus_detail::Out o(out, size);
    o.put("vr camera census 5s: frames=%llu calls=%llu posts=%llu off-thread=%llu stale=%llu inj-calls=%llu kinds=",
          (unsigned long long)w.frames, (unsigned long long)w.calls, (unsigned long long)w.posts,
          (unsigned long long)t.offThread, (unsigned long long)w.stale, (unsigned long long)w.injCalls);
    bool any = false;
    for (int k = 0; k < 8; ++k) {
        if (!w.kinds[k]) continue;
        if (k < 6) o.put("%s%d:%llu", any ? "," : "", k, (unsigned long long)w.kinds[k]);
        else o.put("%s%s:%llu", any ? "," : "", k == 6 ? "other" : "unreadable", (unsigned long long)w.kinds[k]);
        any = true;
    }
    if (!any) o.put("-");
    o.put(" callers=");
    // The busiest first, at most six: the rest are named in a count.
    bool taken[VrCensusWindow::kCallers] = {};
    uint32_t printed = 0;
    for (; printed < 6 && printed < w.callerCount; ++printed) {
        size_t best = VrCensusWindow::kCallers;
        for (size_t i = 0; i < w.callerCount; ++i)
            if (!taken[i] && (best == VrCensusWindow::kCallers || w.callers[i].n > w.callers[best].n)) best = i;
        if (best == VrCensusWindow::kCallers) break;
        taken[best] = true;
        o.put("%s+0x%X:%llu", printed ? "," : "", w.callers[best].rva, (unsigned long long)w.callers[best].n);
    }
    if (!printed) o.put("-");
    const uint64_t unnamed = static_cast<uint64_t>(w.callerCount - printed) + w.callerOverflow;
    if (unnamed) o.put(",+more:%llu", (unsigned long long)unnamed);
    o.put(" cameras-seen=%u cameras-total=%u tone=%llu/%llu/%llu tone-frames=%llu on-foot-frames=%llu foot=%s "
          "eye-draws=%llu/%llu progress=%s hook=%s windows=%u cam-overflow=%llu thread-overflow=%llu",
          w.cameraCount, t.camerasTotal, (unsigned long long)w.toneBefore, (unsigned long long)w.toneAfter,
          (unsigned long long)w.toneNone, (unsigned long long)w.toneFrames, (unsigned long long)w.onFootFrames,
          vrCensusFootName(t.foot), (unsigned long long)w.eyeDraws, (unsigned long long)w.eyeOnFoot,
          w.progressSeen ? "yes" : "no", t.hook, w.windows,
          (unsigned long long)(w.cameraOverflow + t.cameraTableOverflow), (unsigned long long)t.offThreadOverflow);
    return static_cast<int>(o.n);
}

inline int vrCensusFormatCamera(char* out, size_t size, const VrCensusCamera& c) {
    vrcensus_detail::Out o(out, size);
    const VrCensusSig& s = c.firstSig;
    o.put("vr camera census: camera=0x%llx kind=%u caller=+0x%X thread=owner aspect=", (unsigned long long)c.camera, s.kind,
          c.callerRva);
    o.f(s.aspect); o.put(" near="); o.f(s.nearZ); o.put(" far="); o.f(s.farZ); o.put(" fov="); o.f(s.fov);
    o.put(" bound="); o.pair(s.boundX, s.boundY);
    o.put(" offcentre="); o.pair(s.p8, s.p9);
    o.put(" viewport="); o.pair(s.viewportW, s.viewportH);
    o.put(" tan=");
    if (c.tanValid) { o.put("("); o.f(c.tan[0]); o.put(","); o.f(c.tan[1]); o.put(","); o.f(c.tan[2]); o.put(","); o.f(c.tan[3]); o.put(")"); }
    else o.put("-");
    // The first call's second argument (the view, the pass the camera belongs to) and first (the view-constant context).
    o.put(" view=0x%llx vctx=0x%llx", (unsigned long long)c.firstView, (unsigned long long)c.firstCtx);
    o.put(" first-call=%u draw=", c.firstOrdinal);
    o.draw(c.firstDrawKnown, c.firstDraw);
    o.put(" tone=%s frame=%llu", vrCensusToneName(c.firstTone), (unsigned long long)c.firstFrame);
    return static_cast<int>(o.n);
}

// "changed:" names only the fields that moved, old->new. n is the camera's running count of changed calls.
inline int vrCensusFormatChanged(char* out, size_t size, const VrCensusCamera& c) {
    vrcensus_detail::Out o(out, size);
    const VrCensusSig& a = c.changeFrom;
    const VrCensusSig& b = c.sig;
    o.put("vr camera census: changed: camera=0x%llx frame=%llu n=%u", (unsigned long long)c.camera,
          (unsigned long long)c.changeFrame, c.changes);
    if (a.kind != b.kind) o.put(" kind=%u->%u", a.kind, b.kind);
    auto field = [&](const char* name, float from, float to) {
        if (!vrCensusFieldMoved(from, to)) return;
        o.put(" %s=", name); o.f(from); o.put("->"); o.f(to);
    };
    field("aspect", a.aspect, b.aspect);
    field("near", a.nearZ, b.nearZ);
    field("far", a.farZ, b.farZ);
    field("fov", a.fov, b.fov);
    if (vrCensusBoundMoved(a.boundX, b.boundX) || vrCensusBoundMoved(a.boundY, b.boundY)) {
        o.put(" bound="); o.pair(a.boundX, a.boundY); o.put("->"); o.pair(b.boundX, b.boundY);
    }
    if (vrCensusFieldMoved(a.viewportW, b.viewportW) || vrCensusFieldMoved(a.viewportH, b.viewportH)) {
        o.put(" viewport="); o.pair(a.viewportW, a.viewportH); o.put("->"); o.pair(b.viewportW, b.viewportH);
    }
    return static_cast<int>(o.n);
}

// `phase` is what the route chose for this frame (vrWorldRouteWorldPhase, latched at the boundary that opened it): "X,Y" in
// render pixels, or "-" when the route was not jittering.
inline int vrCensusFormatSequence(char* out, size_t size, uint64_t frame, uint32_t index, VrCensusFoot foot,
                                  const VrCensusPhase& phase, uint32_t calls, uint32_t recorded) {
    vrcensus_detail::Out o(out, size);
    o.put("vr camera census: sequence frame=%llu index=%u/%u foot=%s phase=", (unsigned long long)frame, index,
          kVrCensusMaxSequences, vrCensusFootName(foot));
    o.phase(phase);
    o.put(" calls=%u recorded=%u truncated=%u", calls, recorded, calls - recorded);
    return static_cast<int>(o.n);
}

inline int vrCensusFormatCall(char* out, size_t size, uint64_t frame, uint32_t ordinal, const VrCensusCall& c) {
    vrcensus_detail::Out o(out, size);
    o.put("vr camera census: call frame=%llu n=%u camera=0x%llx kind=", (unsigned long long)frame, ordinal,
          (unsigned long long)c.camera);
    if (c.kindReadable) o.put("%u", c.kind); else o.put("-");
    o.put(" caller=+0x%X draw=", c.callerRva);
    o.draw(c.drawKnown, c.draw);
    o.put(" tone=%s inj=%u role=%s fl=0x%X>", vrCensusToneName(c.tone), c.willInject ? 1u : 0u, vrCensusRoleName(c.role),
          c.preFlags);
    if (c.postSeen) o.put("0x%X", c.postFlags); else o.put("-");
    o.put(" view=0x%llx rows=", (unsigned long long)c.view);
    if (c.rowsValid) o.list(c.rows, 16); else o.put("-");
    return static_cast<int>(o.n);
}

// The eye draw's own rows and what they measure. why: null when the rows were read, else the reason they were not.
inline int vrCensusFormatEye(char* out, size_t size, uint32_t eye, uint64_t frame, VrCensusFoot foot,
                             const VrCensusPhase& phase, bool drawKnown, uint32_t draw, uint64_t b1, uint32_t firstConstant,
                             uint32_t bytes, const float rows[16], bool measured, double measX, double measY,
                             const char* why) {
    vrcensus_detail::Out o(out, size);
    o.put("vr camera census: eye=%u frame=%llu foot=%s phase=", eye, (unsigned long long)frame, vrCensusFootName(foot));
    o.phase(phase);
    o.put(" draw=");
    o.draw(drawKnown, draw);
    o.put(" b1=0x%llx first=%u bytes=%u rows=", (unsigned long long)b1, firstConstant, bytes);
    if (rows) o.list(rows, 16); else o.put("-");
    o.put(" meas=");
    if (measured) { o.put("("); o.d(measX); o.put(","); o.d(measY); o.put(")"); } else o.put("-");
    if (why) o.put(" why=%s", why);
    return static_cast<int>(o.n);
}

// What EDVR advertised for the eye this sequence, and the leak: the measured shift of the eye's rows minus the shift the
// advertised frustum and shift should give. Nothing but the eye shift moves an eye camera, so the leak reads zero (within
// float rounding: 1e-8 in flight 1) unless the world route's phase reaches an eye camera, which would read about 1e-4 (half
// a pixel at 5040 wide is 2e-4). With the route jittering this is stage 2's leak detector, and edvr_log.py's verdict judges it
// against the eye line's phase= (PASS below 1e-6, STOP above 1e-5).
inline int vrCensusFormatEyeGeometry(char* out, size_t size, uint32_t eye, uint64_t frame, bool known, uint64_t sequence,
                                     const float frustum[4], const float shift[2], bool measured, double measX,
                                     double measY) {
    vrcensus_detail::Out o(out, size);
    o.put("vr camera census: eye-geometry eye=%u frame=%llu", eye, (unsigned long long)frame);
    if (!known) { o.put(" geometry=unavailable"); return static_cast<int>(o.n); }
    o.put(" seq=%llu frustum=", (unsigned long long)sequence);
    o.list(frustum, 4);
    o.put(" shift="); o.pair(shift[0], shift[1]);
    double tx = 0, ty = 0, sx = 0, sy = 0;
    const bool haveTrue = vrCensusExpectedMeasure(frustum, 0.0f, 0.0f, &tx, &ty);
    const bool haveShifted = vrCensusExpectedMeasure(frustum, shift[0], shift[1], &sx, &sy);
    o.put(" expect=");
    if (haveTrue) { o.put("("); o.d(tx); o.put(","); o.d(ty); o.put(")"); } else o.put("-");
    o.put(" expect-shifted=");
    if (haveShifted) { o.put("("); o.d(sx); o.put(","); o.d(sy); o.put(")"); } else o.put("-");
    o.put(" leak=");
    if (measured && haveShifted) { o.put("("); o.e(measX - sx); o.put(","); o.e(measY - sy); o.put(")"); } else o.put("-");
    return static_cast<int>(o.n);
}

inline int vrCensusFormatOtherThread(char* out, size_t size, const VrCensusOffThread::Entry& e) {
    vrcensus_detail::Out o(out, size);
    o.put("vr camera census: other-thread tid=%u camera=0x%llx kind=", e.thread, (unsigned long long)e.camera);
    if (e.kindReadable) o.put("%u", e.kind); else o.put("-");
    o.put(" caller=+0x%X calls=%llu", e.callerRva, (unsigned long long)e.calls);
    return static_cast<int>(o.n);
}

// ---- the episodes' lines -----------------------------------------------------------------------------------------------
// An episode prints, in this order: its header, its call lines (the ordinary `call` lines, up to kVrCensusEpisodeLines of them, their n= the call's place among
// the frame's recorded calls), the pass's rows, then its join (a `join` line per signature, and a `join-rows` line for each whose rows were read back). The reader
// attaches every one of them to the episode header before it.
struct VrCensusEpisodeHeader {
    uint32_t n = 0;                          // the episode's ordinal this session
    uint64_t frame = 0, armedFrame = 0;      // the sampled frame, and the frame the trigger armed it at
    VrCensusTrigger trigger;
    VrCensusFoot foot = VrCensusFoot::Off;   // the journal's word at the sampled frame's end
    bool guiKnown = false;
    uint32_t gui = 0;                        // Status.json's GuiFocus then
    bool named = false;                      // the sampled frame named the 2D screen's source
    VrCensusPhase phase;                     // what the route chose for the frame
    uint32_t calls = 0, recorded = 0, printed = 0;
    uint32_t kinds[8] = {};                  // every call of the frame by kind: 0..5, 6 other, 7 unreadable
    const VrCensusEpisodeFrame::Caller* callers = nullptr;
    uint32_t callerCount = 0;
    uint64_t callerOverflow = 0;
};
inline int vrCensusFormatEpisode(char* out, size_t size, const VrCensusEpisodeHeader& h) {
    vrcensus_detail::Out o(out, size);
    o.put("vr camera census: episode frame=%llu n=%u/%u trigger=", (unsigned long long)h.frame, h.n, kVrCensusMaxEpisodes);
    o.trigger(h.trigger);
    o.put(" armed=%llu foot=%s gui=", (unsigned long long)h.armedFrame, vrCensusFootName(h.foot));
    if (h.guiKnown) o.put("%u", h.gui); else o.put("-");
    o.put(" named=%u phase=", h.named ? 1u : 0u);
    o.phase(h.phase);
    o.put(" calls=%u recorded=%u printed=%u kinds=", h.calls, h.recorded, h.printed);
    o.kinds(h.kinds);
    o.put(" callers=");
    // The busiest first, at most four: the rest are named in a count.
    bool taken[VrCensusEpisodeFrame::kCallers] = {};
    uint32_t printed = 0;
    for (; printed < 4 && printed < h.callerCount; ++printed) {
        size_t best = VrCensusEpisodeFrame::kCallers;
        for (size_t i = 0; i < h.callerCount; ++i)
            if (!taken[i] && (best == VrCensusEpisodeFrame::kCallers || h.callers[i].n > h.callers[best].n)) best = i;
        if (best == VrCensusEpisodeFrame::kCallers) break;
        taken[best] = true;
        o.put("%s+0x%X:%llu", printed ? "," : "", h.callers[best].rva, (unsigned long long)h.callers[best].n);
    }
    if (!printed) o.put("-");
    const uint64_t unnamed = static_cast<uint64_t>(h.callerCount - printed) + h.callerOverflow;
    if (unnamed) o.put(",+more:%llu", (unsigned long long)unnamed);
    return static_cast<int>(o.n);
}

// One signature of the join. `episode` and `sig` (1-based) say which; the facts are the first draw's.
inline int vrCensusFormatJoin(char* out, size_t size, uint32_t episode, uint32_t sig, const VrCensusJoinRow& r) {
    vrcensus_detail::Out o(out, size);
    o.put("vr camera census: join ep=%u sig=%u depth=%s eye=%s size=%ux%u draw=%u draws=%llu vs=0x%llX ps=0x%llX dw=%s b1=", episode, sig,
          vrCensusJoinDepthName(r.depth), vrCensusJoinEyeName(r.depth), r.w, r.h, r.firstDraw, (unsigned long long)r.draws,
          (unsigned long long)r.vs, (unsigned long long)r.ps, r.depthWrite < 0 ? "-" : r.depthWrite ? "yes" : "no");
    if (r.b1Bound) o.put("0x%llx first=%u bytes=%u", (unsigned long long)r.b1, r.b1First, r.b1Bytes);
    else o.put("- first=- bytes=-");
    if (r.rowsRead) {
        o.put(" rows=read match=");
        o.ordinals(r.matches, 6, r.matchCount);
    } else {
        o.put(" rows=- why=%s", r.why ? r.why : "-");
    }
    return static_cast<int>(o.n);
}
// The rows 270..273 a signature's first draw read back, in the format of a call line's rows= (a call's composed rows and these are compared as text by the reader).
inline int vrCensusFormatJoinRows(char* out, size_t size, uint32_t episode, uint32_t sig, const float rows[16]) {
    vrcensus_detail::Out o(out, size);
    o.put("vr camera census: join-rows ep=%u sig=%u rows=", episode, sig);
    o.list(rows, 16);
    return static_cast<int>(o.n);
}
// What the join's per-draw hook saw in the sampled frame: every draw it was handed (`seen`: zero means the hook was never reached, which a frame with no draw into the
// screen's or an eye's depth does not), those into a depth of the screen's size or an eye's (`relevant`), the distinct depth views it resolved and the signatures it kept.
inline int vrCensusFormatJoinDraws(char* out, size_t size, uint32_t episode, uint64_t seen, uint64_t relevant, uint32_t views, uint32_t signatures) {
    vrcensus_detail::Out o(out, size);
    o.put("vr camera census: join-draws ep=%u seen=%llu relevant=%llu views=%u signatures=%u", episode, (unsigned long long)seen, (unsigned long long)relevant, views,
          signatures);
    return static_cast<int>(o.n);
}
// The signatures the join's table could not keep (more than kVrCensusJoinRows distinct vertex and pixel shader pairs into the screen's and the eyes' depths in one
// frame), and the draws that fell in them: counted, never lost silently.
inline int vrCensusFormatJoinMore(char* out, size_t size, uint32_t episode, uint32_t signatures, uint64_t draws) {
    vrcensus_detail::Out o(out, size);
    o.put("vr camera census: join-more ep=%u signatures=%u draws=%llu", episode, signatures, (unsigned long long)draws);
    return static_cast<int>(o.n);
}
// What the temporal pass chose as the frame's camera rows (temporal_pass.h temporalPassChosenRows) and which recorded calls' view axes they equal: the calls
// whose axes match within kVrCensusAxesTol, and the nearest call and how near. `valid` false: the pass chose nothing this frame (it is off, or nothing was treated).
inline int vrCensusFormatPassRows(char* out, size_t size, uint32_t episode, uint64_t frame, bool valid, bool bound, const float rows[12],
                                  const VrCensusAxesMatch& m) {
    vrcensus_detail::Out o(out, size);
    o.put("vr camera census: pass-rows ep=%u frame=%llu valid=%u bound=", episode, (unsigned long long)frame, valid ? 1u : 0u);
    if (!valid) { o.put("- rows=- axes-match=- how=- nearest=- diff=-"); return static_cast<int>(o.n); }
    o.put("%u rows=", bound ? 1u : 0u);
    o.list(rows, 12);
    o.put(" axes-match=");
    o.ordinals(m.ordinals, VrCensusAxesMatch::kMax, m.count);
    if (m.nearest) {
        o.put(" how=%s nearest=%u diff=", m.nearestTransposed ? "transpose" : "identity", m.nearest);
        o.e(static_cast<double>(m.nearestDiff));
    } else {
        o.put(" how=- nearest=- diff=-");
    }
    return static_cast<int>(o.n);
}

// The whole of an episode's printing, with the line sink handed in (the DLL's is its budgeted logger, the rig's a list), so what the DLL writes and what the reader's
// fixture holds are one piece of code. Every match is made here, in memory, over ALL the calls the frame recorded -- not only the ones that print -- so a call the line cap
// keeps out of the log is still found, and the calls that matched something print first. Order: the header, the call lines that print, the pass's rows, then each join
// signature with its rows line when it has one, then a note when signatures did not fit. `say(VrCensusLines class, const char* line)`.
struct VrCensusEpisodePrint {
    uint32_t n = 0;                          // the episode's ordinal this session
    uint64_t frame = 0, armedFrame = 0;      // the sampled frame (the census's own frame count), and the frame its trigger armed
    VrCensusTrigger trigger;
    VrCensusFoot foot = VrCensusFoot::Off;   // what the journal said as the sampled frame ended
    bool guiKnown = false;
    uint32_t gui = 0;
    bool named = false;                      // the sampled frame named the 2D screen's source
    VrCensusPhase phase;
    bool haveChosen = false, chosenBound = false;   // the temporal pass chose rows this frame (temporalPassChosenRows), and from the block bound at the first scene draw
    float chosen[12] = {};
};
template <class Say>
inline void vrCensusPrintEpisode(Say&& say, const VrCensusEpisodePrint& in, VrCensusEpisodeFrame& f, VrCensusJoin& join) {
    char line[kVrCensusLineBytes + 16];
    bool matched[VrCensusEpisodeFrame::kCapacity] = {};
    bool selected[VrCensusEpisodeFrame::kCapacity] = {};
    for (uint32_t i = 0; i < join.used; ++i) {
        VrCensusJoinRow& r = join.row[i];
        if (!r.rowsRead) continue;
        r.matchCount = vrCensusMatchRows(f, r.rows, kVrCensusRowsTol, r.matches, 6);
        for (uint32_t k = 0; k < r.matchCount && k < 6; ++k) matched[r.matches[k] - 1] = true;
    }
    VrCensusAxesMatch axes;
    if (in.haveChosen) {
        axes = vrCensusMatchAxes(f, in.chosen, kVrCensusAxesTol);
        for (uint32_t k = 0; k < axes.count && k < VrCensusAxesMatch::kMax; ++k) matched[axes.ordinals[k] - 1] = true;
        if (axes.nearest) matched[axes.nearest - 1] = true;
    }
    const uint32_t printed = vrCensusSelectCalls(f, matched, kVrCensusEpisodeLines, selected);
    VrCensusEpisodeHeader h;
    h.n = in.n;
    h.frame = in.frame;
    h.armedFrame = in.armedFrame;
    h.trigger = in.trigger;
    h.foot = in.foot;
    h.guiKnown = in.guiKnown;
    h.gui = in.gui;
    h.named = in.named;
    h.phase = in.phase;
    h.calls = f.calls;
    h.recorded = f.recorded;
    h.printed = printed;
    std::memcpy(h.kinds, f.kinds, sizeof(h.kinds));
    h.callers = f.callers;
    h.callerCount = f.callerCount;
    h.callerOverflow = f.callerOverflow;
    vrCensusFormatEpisode(line, kVrCensusLineBytes + 1, h);
    say(VrCensusLines::Episode, line);
    for (uint32_t i = 0; i < f.recorded; ++i) {
        if (!selected[i]) continue;
        vrCensusFormatCall(line, kVrCensusLineBytes + 1, in.frame, i + 1, f.call[i]);
        say(VrCensusLines::EpisodeCall, line);
    }
    vrCensusFormatPassRows(line, kVrCensusLineBytes + 1, in.n, in.frame, in.haveChosen, in.chosenBound, in.chosen, axes);
    say(VrCensusLines::Episode, line);
    vrCensusFormatJoinDraws(line, kVrCensusLineBytes + 1, in.n, join.seen, join.relevantDraws(), join.views, join.used);
    say(VrCensusLines::Episode, line);
    for (uint32_t i = 0; i < join.used; ++i) {
        vrCensusFormatJoin(line, kVrCensusLineBytes + 1, in.n, i + 1, join.row[i]);
        say(VrCensusLines::Episode, line);
        if (!join.row[i].rowsRead) continue;
        vrCensusFormatJoinRows(line, kVrCensusLineBytes + 1, in.n, i + 1, join.row[i].rows);
        say(VrCensusLines::Episode, line);
    }
    if (join.overflowRows) {
        vrCensusFormatJoinMore(line, kVrCensusLineBytes + 1, in.n, join.overflowRows, join.overflowDraws);
        say(VrCensusLines::Episode, line);
    }
}

// The window's three companion lines (the 5 s line itself is full at 400 characters, so each window's episode counters, naming runs and detour CPU are lines of their own,
// printed in the window with it, zeros included: an absent line is what "this code never ran" looks like). `windows` is how many 5 s windows the counts cover (the first 36
// print each, then one in twelve covers twelve).
struct VrCensusEpisodeCounters {
    uint32_t taken = 0;                      // episodes armed this session
    uint32_t triggers = 0, skipped = 0;      // triggers seen, and those that found an episode armed or the cap reached
    const char* state = "idle";              // idle, armed or live as the window closed
    VrCensusTrigger trigger;                 // the armed or live episode's trigger,
    uint64_t armedFrame = 0, sampleFrame = 0;   // ...the frame it was armed at and the frame it will sample
};
inline int vrCensusFormatEpisodeCounters(char* out, size_t size, const VrCensusEpisodeCounters& c, uint32_t windows, bool pending) {
    vrcensus_detail::Out o(out, size);
    o.put("vr camera census: episodes windows=%u taken=%u/%u triggers=%u skipped=%u state=%s", windows, c.taken, kVrCensusMaxEpisodes, c.triggers, c.skipped, c.state);
    if (pending) {
        o.put(" trigger=");
        o.trigger(c.trigger);
        o.put(" armed=%llu sample=%llu", (unsigned long long)c.armedFrame, (unsigned long long)c.sampleFrame);
    }
    return static_cast<int>(o.n);
}
inline int vrCensusFormatRuns(char* out, size_t size, const VrCensusRuns& r, uint32_t windows) {
    vrcensus_detail::Out o(out, size);
    o.put("vr camera census: runs windows=%u frames=%llu named=", windows, (unsigned long long)r.frames);
    for (int b = 0; b < VrCensusRuns::kBins; ++b) o.put("%s%s:%llu", b ? "," : "", VrCensusRuns::binName(b), (unsigned long long)r.named[b]);
    o.put(" unnamed=");
    for (int b = 0; b < VrCensusRuns::kBins; ++b) o.put("%s%s:%llu", b ? "," : "", VrCensusRuns::binName(b), (unsigned long long)r.unnamed[b]);
    o.put(" longest=named:%u,unnamed:%u open=", r.longestNamed, r.longestUnnamed);
    if (r.open < 0) o.put("-"); else o.put("%s:%u", r.open ? "named" : "unnamed", r.openLength);
    return static_cast<int>(o.n);
}

// The detour's CPU in the window, from the observer's timed halves. `ticksPerSecond` is the clock's frequency. Means and maxima in microseconds, `-` for what
// no call was timed for; est-ms-frame is the mean cost a frame, calls a frame times what the timed ones cost, over the modes that had calls.
inline int vrCensusFormatCpu(char* out, size_t size, const VrCensusWindow& w, const VrCensusCpu& c, int64_t ticksPerSecond, uint32_t windows) {
    vrcensus_detail::Out o(out, size);
    const double perTickUs = ticksPerSecond > 0 ? 1.0e6 / static_cast<double>(ticksPerSecond) : 0.0;
    const uint64_t obsCalls = w.calls - (w.injCalls < w.calls ? w.injCalls : w.calls), injCalls = w.injCalls < w.calls ? w.injCalls : w.calls;
    const uint64_t calls[2] = {obsCalls, injCalls};
    uint64_t sampled = 0;
    double estUs = 0.0;   // microseconds a window's calls cost, estimated from the timed ones
    bool estimable = ticksPerSecond > 0 && w.frames > 0;
    for (int m = 0; m < 2; ++m) {
        sampled += c.mode[m].sampled;
        if (!calls[m]) continue;
        if (!c.mode[m].sampled) { estimable = false; continue; }
        const double pre = static_cast<double>(c.mode[m].preTicks) / static_cast<double>(c.mode[m].sampled) * perTickUs;
        const double post = c.mode[m].postSampled ? static_cast<double>(c.mode[m].postTicks) / static_cast<double>(c.mode[m].postSampled) * perTickUs : 0.0;
        estUs += static_cast<double>(calls[m]) * (pre + post);
    }
    o.put("vr camera census: detour windows=%u every=%u timed=observer-halves frames=%llu calls=%llu sampled=%llu est-ms-frame=", windows,
          VrCensusCpu::kEvery, (unsigned long long)w.frames, (unsigned long long)w.calls, (unsigned long long)sampled);
    if (estimable && sampled) o.put("%.3f", estUs / static_cast<double>(w.frames) / 1000.0); else o.put("-");
    static const char* const names[2] = {"obs", "inj"};
    for (int m = 0; m < 2; ++m) {
        const VrCensusCpu::Mode& md = c.mode[m];
        o.put(" %s-calls=%llu %s-sampled=%llu %s-pre-us=", names[m], (unsigned long long)calls[m], names[m], (unsigned long long)md.sampled, names[m]);
        if (md.sampled && ticksPerSecond > 0)
            o.put("%.3g/%.3g", static_cast<double>(md.preTicks) / static_cast<double>(md.sampled) * perTickUs, static_cast<double>(md.preMax) * perTickUs);
        else o.put("-");
        o.put(" %s-post-us=", names[m]);
        if (md.postSampled && ticksPerSecond > 0)
            o.put("%.3g/%.3g", static_cast<double>(md.postTicks) / static_cast<double>(md.postSampled) * perTickUs, static_cast<double>(md.postMax) * perTickUs);
        else o.put("-");
    }
    return static_cast<int>(o.n);
}

}  // namespace edvr
