// F91 (native on-foot VR), parts B and C: the per-frame ADS vibration instrument and its fit (B), and the ADS item gate (C). Design: reviews\native-onfoot-f91-review-2026-10-24.md
// sections 2 and 3; journal: the arc doc, F91.
//
// Header only, no game or D3D11 state, so native_frame_test runs the very functions the hooks use.
//
// B. "The rifle itself seems to be vibrating" in ADS. The leading candidate (H1): the runtime applies the ADS seated cancel of the game's turn 1-2 frames after the game turns the
// camera, so every turn the head aim asks for, tremor included, shows in the image for a frame or two as a turn the head did not make. The instrument extends the F89 transition
// trace (no new line family): each press is traced for 4 s, and every traced frame carries the residual r (the right eye's view less the head less the player's own aim, the F68a
// guard's), the game's applied turn, the head's step, the view's step, the cancel (this frame's, the total, and the total the pose carried, found by the alignment serial the
// runtime reported with the pose: the same answer carries the seated yaw and pitch), the sight lock's error and the rifle's sight line in the right eye's view. Every 0.5 s of a
// press the fit (fitWindow) says which of H1, the sight lock and H3 (the readout pin lagging a frame) the frames show, on still windows (net head turn under 2 deg/s, the head's
// tremor) as on moving ones.
//
// C. Our ADS features engage only for a weapon: F83's native primary, or a held item whose Status.json name starts "$wpn_". A tool (the Profile Analyser, the Arc Cutter, the
// Energylink, the Genetic Sampler) or the fists never engage them, whatever HumanoidZoom is bound to. One gate on the resolved request (adsAimTick) covers every consumer.
#pragma once

#include "journal_watch.h"   // JournalHeldItem

#include <cmath>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>

namespace edvr {
namespace nofoot {
namespace f91 {

// ---- C: the ADS item gate ----------------------------------------------------------------------------------------------------------------------------------
// True when the name starts "$wpn_" (any case): the game's weapons ($wpn_m_assaultrifle_laser_fauto_name;, $wpn_s_pistol_kinetic_sauto_name;); tools are $humanoid_*.
inline bool weaponName(const char* name) noexcept {
    static const char kPrefix[] = "$wpn_";
    if (!name) return false;
    for (size_t i = 0; i + 1 < sizeof(kPrefix); ++i) {
        char c = name[i];
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        if (c != kPrefix[i]) return false;   // the terminating NUL of a short name fails here too
    }
    return true;
}
struct ItemGate {
    bool allow = false;
    const char* reason = "unknown";   // allowed: "weapon" or "native primary"; refused: "tool", "fists" or "unknown"
};
// nativePrimary: F83's fresh, readable primary selection (the native weapon selection, which needs no Status.json); kind and name: Status.json's SelectedWeapon. A held
// weapon is allowed by its name; a primary is allowed whatever Status.json says, which lags a switch by up to 100 ms (a pistol waits for it).
inline ItemGate adsItemGate(bool nativePrimary, JournalHeldItem kind, const char* name) noexcept {
    ItemGate g;
    if (kind == kHeldItem && weaponName(name)) {
        g.allow = true;
        g.reason = "weapon";
    } else if (nativePrimary) {
        g.allow = true;
        g.reason = "native primary";
    } else {
        g.reason = kind == kHeldNone ? "fists" : (kind == kHeldItem && name && name[0]) ? "tool" : "unknown";
    }
    return g;
}
enum GateEvent : int { kGateNone = 0, kGateRefused = 1, kGateStopped = 2, kGateAllowed = 3 };
// The gate's edges, once per tick: asked is the resolved request (the bound HumanoidZoom, else the journal), allow the gate's answer. ads = asked && allow is what every
// consumer reads. Refused: a rising edge of asked while refused. Stopped: a held, allowed press whose item stopped being a weapon (the press releases through the normal
// unwind: ads falls). Allowed: a held press the gate refused becomes allowed (a pistol's name arriving in Status.json).
struct GateEdges {
    bool askedLast = false, allowLast = true;
    struct Out {
        bool ads = false;
        GateEvent event = kGateNone;
    };
    Out step(bool asked, bool allow) noexcept {
        Out o;
        o.ads = asked && allow;
        if (asked && !allow) o.event = !askedLast ? kGateRefused : allowLast ? kGateStopped : kGateNone;
        else if (asked && askedLast && !allowLast) o.event = kGateAllowed;
        askedLast = asked;
        allowLast = allow;
        return o;
    }
    void reset() noexcept {
        askedLast = false;
        allowLast = true;
    }
};
// The ADS head aim's state edges (adsAimTick: engage on ADS, release when it ends, re-engage from the release). The release's end (the weight at 0 and the unwind done) is the
// caller's. The values are adsAimTick's kAdsIdle, kAdsActive and kAdsReleasing (static_assert there).
constexpr int kStateIdle = 0, kStateEngaged = 1, kStateReleasing = 2;
inline int adsStateAfter(int state, bool ads) noexcept {
    if (state == kStateIdle) return ads ? kStateEngaged : kStateIdle;
    if (state == kStateEngaged || state == kStateReleasing) return ads ? kStateEngaged : kStateReleasing;
    return state;
}

// ---- B: the trace's per-frame fields -----------------------------------------------------------------------------------------------------------------------
constexpr double kPi = 3.14159265358979323846;
inline double dot3(const double a[3], const double b[3]) noexcept { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
inline double wrapDeg(double a) noexcept {
    if (!std::isfinite(a)) return 0.0;
    while (a > 180.0) a -= 360.0;
    while (a <= -180.0) a += 360.0;
    return a;
}
// The angle (degrees) the view turned between two sets of axes (right, up, forward; unit): 2 asin(|R1 - R0| / sqrt 8), which keeps its precision for the hundredths of a degree a
// frame this instrument is after (acos of the trace does not, with axes from float rows).
inline double viewStepDeg(const double r0[3], const double u0[3], const double f0[3], const double r1[3], const double u1[3], const double f1[3]) noexcept {
    double d = 0.0;
    for (int i = 0; i < 3; ++i) d += (r1[i] - r0[i]) * (r1[i] - r0[i]) + (u1[i] - u0[i]) * (u1[i] - u0[i]) + (f1[i] - f0[i]) * (f1[i] - f0[i]);
    double s = std::sqrt(d / 8.0);
    s = s > 1.0 ? 1.0 : s;
    return 2.0 * std::asin(s) * 180.0 / kPi;
}
// The rifle's sight line (dir, world) in the right eye's view (its axes right, up, forward, world): yaw = atan2(dir.r, dir.f), pitch = atan2(dir.u, dir.f), degrees. Fixed in
// the view while the rifle turns with the view; its frame-to-frame change is the rifle's motion in the image.
inline bool rifleInView(const double dir[3], const double vr[3], const double vu[3], const double vf[3], double* yawDeg, double* pitchDeg) noexcept {
    const double x = dot3(dir, vr), y = dot3(dir, vu), z = dot3(dir, vf);
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z) || (x * x + y * y + z * z) < 1e-12) return false;
    *yawDeg = std::atan2(x, z) * 180.0 / kPi;
    *pitchDeg = std::atan2(y, z) * 180.0 / kPi;
    return true;
}
// The cancel the pose carried. The runtime reads the seated yaw (the look total), the ADS pitch and the eye alignment from the one answer and reports, with each pose, the serial of
// the alignment its seated origin carried (F89). The frame thread keeps, for every alignment serial it publishes, its tick and the cancel total at that tick: the pose's serial
// then names the cancel it carried and how many ticks old it was. Exact while the alignment is republished every tick (in ADS the position lock follows the head's own motion);
// an alignment that did not change for some ticks makes the age an upper bound.
constexpr uint32_t kCancelHistN = 256;
struct CancelHistory {
    struct Slot {
        uint32_t serial = ~0u, tick = 0;
        double yaw = 0.0, pitch = 0.0;   // degrees, the seated cancel totals
    };
    Slot s[kCancelHistN];
    uint32_t newest = 0;
    void record(uint32_t serial, uint32_t tick, double yawDeg, double pitchDeg) noexcept {
        Slot& d = s[serial % kCancelHistN];
        d.serial = serial;
        d.tick = tick;
        d.yaw = yawDeg;
        d.pitch = pitchDeg;
        newest = serial;
    }
    struct Found {
        bool ok = false;
        uint32_t age = 0, serialLag = 0;   // ticks since that serial was published; publications since
        double yaw = 0.0, pitch = 0.0;
    };
    // word: the frame input's applied-alignment word (0: not reported, else the serial plus 1).
    Found find(uint32_t word, uint32_t tickNow) const noexcept {
        Found f;
        if (word == 0) return f;
        const uint32_t serial = word - 1u;
        const Slot& d = s[serial % kCancelHistN];
        if (d.serial != serial) return f;
        f.ok = true;
        f.age = tickNow - d.tick;
        f.serialLag = newest - serial;
        f.yaw = d.yaw;
        f.pitch = d.pitch;
        return f;
    }
};

inline void appendf(char* buf, size_t cap, size_t& at, const char* fmt, ...) noexcept {
    if (!buf || at + 1 >= cap) return;
    va_list ap;
    va_start(ap, fmt);
    const int n = std::vsnprintf(buf + at, cap - at, fmt, ap);
    va_end(ap);
    if (n > 0) at = (at + static_cast<size_t>(n) < cap) ? at + static_cast<size_t>(n) : cap - 1;
}

// One traced frame's F91 fields (degrees, seat convention for the head aim's figures; millimetres for the sight). Values are carried across ticks, stamped with the rows'
// frame and the head's frameSequence, so a stale one shows its age.
struct ViewFields {
    bool rShown = false;               // false: r - (the head aim is not engaged, or the residual has no measurement since its base)
    double r[2] = {};
    bool rowsKnown = false;
    uint32_t rowsFrame = 0;            // the right eye's rows r was measured on
    uint64_t headSeq = 0;              // the head sample of the applied, head and cancel figures
    double applied[2] = {}, head[2] = {}, cancelTotal[2] = {};
    bool viewOk = false;
    double viewStep = 0.0;             // the angle between the last two consecutive new right-eye rows
    bool sightOk = false;
    double along = 0.0, perp = 0.0;    // this frame's sight lock error (the ADS eye point line's figures), mm
    double asked[2] = {}, cancelNow[2] = {};   // the look input asked of the game this frame (seat), the cancel taken this frame
    bool poseOk = false;
    double cancelPose[2] = {};         // the cancel total the pose of this frame carried
    uint32_t poseAge = 0, serialLag = 0;
    bool rifleOk = false;
    double rifle[2] = {};              // the rifle's sight line in the right eye's view
    bool rifleStepOk = false;
    double rifleStep[2] = {};
};
// The suffix every `ads transition (F89) ... frame` line carries from F91 on; a line without "; view (F91)" means the fields never ran.
inline size_t formatView(char* buf, size_t cap, const ViewFields& v) noexcept {
    size_t at = 0;
    if (buf && cap) buf[0] = '\0';
    appendf(buf, cap, at, "; view (F91): ");
    if (v.rShown) appendf(buf, cap, at, "r yaw %+.3f pitch %+.3f deg", v.r[0], v.r[1]);
    else appendf(buf, cap, at, "r -");
    if (v.rowsKnown) appendf(buf, cap, at, " (rows frame %u, head seq %llu)", static_cast<unsigned>(v.rowsFrame), static_cast<unsigned long long>(v.headSeq));
    else appendf(buf, cap, at, " (rows frame none, head seq %llu)", static_cast<unsigned long long>(v.headSeq));
    appendf(buf, cap, at, ", applied yaw %+.3f pitch %+.3f, head step yaw %+.3f pitch %+.3f, ", v.applied[0], v.applied[1], v.head[0], v.head[1]);
    if (v.viewOk) appendf(buf, cap, at, "view step %.3f deg", v.viewStep);
    else appendf(buf, cap, at, "view step -");
    appendf(buf, cap, at, ", cancel total yaw %+.3f pitch %+.3f deg; ", v.cancelTotal[0], v.cancelTotal[1]);
    if (v.sightOk) appendf(buf, cap, at, "sight along %+.2f perp %.2f mm", v.along, v.perp);
    else appendf(buf, cap, at, "sight -");
    appendf(buf, cap, at, "; asked yaw %+.3f pitch %+.3f, cancel this frame yaw %+.3f pitch %+.3f, ", v.asked[0], v.asked[1], v.cancelNow[0], v.cancelNow[1]);
    if (v.poseOk)
        appendf(buf, cap, at, "cancel in the pose yaw %+.3f pitch %+.3f deg (%u frames old, alignment serial lag %u); ", v.cancelPose[0], v.cancelPose[1],
                static_cast<unsigned>(v.poseAge), static_cast<unsigned>(v.serialLag));
    else
        appendf(buf, cap, at, "cancel in the pose - (the pose's alignment serial is not reported or not in the history); ");
    if (v.rifleOk) {
        appendf(buf, cap, at, "rifle in view yaw %+.3f pitch %+.3f deg", v.rifle[0], v.rifle[1]);
        if (v.rifleStepOk) appendf(buf, cap, at, ", step yaw %+.3f pitch %+.3f", v.rifleStep[0], v.rifleStep[1]);
        else appendf(buf, cap, at, ", step -");
    } else {
        appendf(buf, cap, at, "rifle in view -");
    }
    return at;
}

// ---- B: the fit ----------------------------------------------------------------------------------------------------------------------------------------------
constexpr double kFitWindowS = 0.5;      // a press's trace is fitted in windows of this long
constexpr uint32_t kFitMaxFrames = 160;  // ... or of this many frames (a window closes at either)
constexpr uint32_t kFitMinFrames = 8;    // fewer r frames than this: no verdict
constexpr double kStillDegS = 2.0;       // a window whose net head turn is under this is still (the head's tremor), else moving
constexpr double kH1FitDeg = 0.01;       // H1: r equals the last d applied steps to this rms (degrees)
constexpr double kH1StepDeg = 0.02;      // ... and r moves frame to frame by at least this rms (0.66 px)
constexpr double kLockMm = 0.2;          // the lock: the sight error's frame-to-frame step reverses at this size or more, or overshoots this much after the head stops
constexpr double kH3FlatDeg = 0.005;     // H3: r's step rms under this,
constexpr double kH3ViewDeg = 0.01;      // the view's step equal to the head's step to this rms (not in the design: H1's tolerance),
constexpr double kH3SightMm = 0.1;       // and the sight error's step rms under this
struct FitFrame {
    double dt = 0.0;                     // seconds since the last traced frame
    double t = 0.0;                      // the trace's time
    bool headOk = false;
    double head[2] = {};                 // the head's step this frame (deg, seat)
    double applied[2] = {};              // the game's turn for our input this frame (deg, seat; 0 when the map did not run)
    double sum[3][2] = {};               // the last 1, 2 and 3 applied steps summed, this frame's included (FitWindow::add fills it)
    bool rOk = false;
    double r[2] = {};                    // the residual this frame (deg)
    uint32_t rBase = 0;                  // the residual's base (a new base moves r's offset)
    bool missingOk = false;
    double missing[2] = {};              // the cancel total less the cancel the pose carried (deg)
    bool viewOk = false;
    double viewStep = 0.0;               // deg
    bool sightOk = false;
    double along = 0.0, perp = 0.0;      // mm
    bool rifleOk = false;
    double rifle[2] = {};                // deg
};
struct FitResult {
    uint32_t index = 0, frames = 0;
    double t0 = 0.0, t1 = 0.0, dur = 0.0;
    bool still = false;
    double netRate = 0.0;                // the head's net turn over the window, deg/s
    uint32_t headN = 0;
    double headRms[2] = {};              // the head's step about its mean (the tremor), deg a frame
    uint32_t rN = 0, rStepN = 0;
    double rMean[2] = {}, rStepRms[2] = {}, rStepRms2 = 0.0;
    int bestD = 0;                       // 1..3, 0 when no fit
    int signY = 1, signP = 1;
    double rmsD[3] = {}, bestRms = 0.0;
    uint32_t missN = 0;
    double missRms = 0.0;
    uint32_t viewN = 0;
    double viewHeadRms = 0.0;
    uint32_t rifleStepN = 0;
    double rifleRms[2] = {};             // the rifle's step in the view about its mean, deg a frame
    double xr[2][3] = {}, xf[2][3] = {}; // Pearson correlation of the head's step with r's step (xr) and the rifle's step (xf) k frames later, k 0..2, yaw and pitch
    uint32_t xrN[3] = {}, xfN[3] = {};
    uint32_t sightN = 0, sightStepN = 0, revAlong = 0, revPerp = 0;
    double stepRmsAlong = 0.0, stepRmsPerp = 0.0, netAlong = 0.0, netPerp = 0.0;
    bool overshoot = false;
    bool h1 = false, lock = false, h3 = false, enough = false;
};
// A press's windows: frames go in as they are traced; a full window is fitted and starts the next. The applied history and the last window's motion carry across windows.
struct FitWindow {
    FitFrame f[kFitMaxFrames];
    uint32_t n = 0, index = 0;
    double dur = 0.0;
    double hist[3][2] = {};              // the last three applied steps, newest first
    bool prevValid = false, prevMoving = false;
    double prevNet[2] = {};              // the last window's sight change (along, perp; mm), for the overshoot
    void beginPress() noexcept {
        n = 0;
        index = 0;
        dur = 0.0;
        for (auto& h : hist) h[0] = h[1] = 0.0;
        prevValid = prevMoving = false;
        prevNet[0] = prevNet[1] = 0.0;
    }
    // True when the window is full (kFitWindowS of frames, or kFitMaxFrames).
    bool add(const FitFrame& in) noexcept {
        if (n >= kFitMaxFrames) return true;
        for (int k = 2; k > 0; --k) hist[k][0] = hist[k - 1][0], hist[k][1] = hist[k - 1][1];
        hist[0][0] = std::isfinite(in.applied[0]) ? in.applied[0] : 0.0;
        hist[0][1] = std::isfinite(in.applied[1]) ? in.applied[1] : 0.0;
        FitFrame& d = f[n++];
        d = in;
        for (int k = 0; k < 3; ++k) {
            d.sum[k][0] = d.sum[k][1] = 0.0;
            for (int j = 0; j <= k; ++j) d.sum[k][0] += hist[j][0], d.sum[k][1] += hist[j][1];
        }
        dur += in.dt > 0.0 ? in.dt : 0.0;
        return dur >= kFitWindowS || n >= kFitMaxFrames;
    }
    void clear() noexcept {
        n = 0;
        dur = 0.0;
    }
};
namespace detail {
inline double rmsAbout(const double* x, uint32_t n) noexcept {
    if (!n) return 0.0;
    double m = 0.0;
    for (uint32_t i = 0; i < n; ++i) m += x[i];
    m /= n;
    double s = 0.0;
    for (uint32_t i = 0; i < n; ++i) s += (x[i] - m) * (x[i] - m);
    return std::sqrt(s / n);
}
inline double pearson(const double* x, const double* y, uint32_t n) noexcept {
    if (n < 4) return std::numeric_limits<double>::quiet_NaN();
    double mx = 0.0, my = 0.0;
    for (uint32_t i = 0; i < n; ++i) mx += x[i], my += y[i];
    mx /= n;
    my /= n;
    double sxy = 0.0, sxx = 0.0, syy = 0.0;
    for (uint32_t i = 0; i < n; ++i) {
        sxy += (x[i] - mx) * (y[i] - my);
        sxx += (x[i] - mx) * (x[i] - mx);
        syy += (y[i] - my) * (y[i] - my);
    }
    return (sxx > 1e-18 && syy > 1e-18) ? sxy / std::sqrt(sxx * syy) : std::numeric_limits<double>::quiet_NaN();
}
// The sum of squares of e about its mean within each run of one residual base (a new base moves r by a constant).
inline double ssPerBase(const double* e, const uint32_t* base, uint32_t n) noexcept {
    double ss = 0.0;
    uint32_t i = 0;
    while (i < n) {
        uint32_t j = i;
        double m = 0.0;
        while (j < n && base[j] == base[i]) m += e[j++];
        m /= (j - i);
        for (uint32_t k = i; k < j; ++k) ss += (e[k] - m) * (e[k] - m);
        i = j;
    }
    return ss;
}
}  // namespace detail
// The deciding rule (the design's section 2), on one window:
//  H1   for one d in 1..3, r = (the last d applied steps summed) to kH1FitDeg rms, and r's frame-to-frame step at least kH1StepDeg rms. The sign of each axis is fitted (r is
//       the guard's convention, applied the seat's) and r's offset is taken out per residual base (r is measured since a base).
//  lock the sight error's frame-to-frame step (along or perp) reverses at kLockMm or more, or it overshoots: a still window after a moving one moves the error back against the
//       moving window's change by kLockMm or more.
//  H3   r flat (step rms under kH3FlatDeg), the view's step equal to the head's (rms under kH3ViewDeg) and the sight error's step under kH3SightMm rms: the view follows the head
//       and the suspect is the readout pin's one-frame lag (measured in the dump).
// Evaluated on still windows (the head's tremor: the 1 s lines average it out) as on moving ones; the window says which it is.
inline FitResult fitWindow(const FitWindow& w) noexcept {
    FitResult R;
    const uint32_t n = w.n;
    R.index = w.index;
    R.frames = n;
    if (!n) return R;
    R.t0 = w.f[0].t;
    R.t1 = w.f[n - 1].t;
    R.dur = w.dur;
    double hy[kFitMaxFrames], hp[kFitMaxFrames];
    double netY = 0.0, netP = 0.0;
    for (uint32_t i = 0; i < n; ++i) {
        if (!w.f[i].headOk) continue;
        hy[R.headN] = w.f[i].head[0];
        hp[R.headN] = w.f[i].head[1];
        netY += w.f[i].head[0];
        netP += w.f[i].head[1];
        ++R.headN;
    }
    R.netRate = R.dur > 0.0 ? std::sqrt(netY * netY + netP * netP) / R.dur : 0.0;
    R.still = R.netRate < kStillDegS;
    R.headRms[0] = detail::rmsAbout(hy, R.headN);
    R.headRms[1] = detail::rmsAbout(hp, R.headN);
    // r, its steps (a step only between consecutive frames on one base), and the fit
    double sy[kFitMaxFrames], sp[kFitMaxFrames];
    uint32_t rb[kFitMaxFrames];
    bool stepOk[kFitMaxFrames] = {};
    double dRy[kFitMaxFrames] = {}, dRp[kFitMaxFrames] = {};
    double ssStep = 0.0;
    for (uint32_t i = 0; i < n; ++i) {
        const FitFrame& a = w.f[i];
        if (!a.rOk) continue;
        rb[R.rN] = a.rBase;
        R.rMean[0] += a.r[0];
        R.rMean[1] += a.r[1];
        ++R.rN;
        if (i > 0 && w.f[i - 1].rOk && w.f[i - 1].rBase == a.rBase) {
            stepOk[i] = true;
            dRy[i] = a.r[0] - w.f[i - 1].r[0];
            dRp[i] = a.r[1] - w.f[i - 1].r[1];
            sy[R.rStepN] = dRy[i];
            sp[R.rStepN] = dRp[i];
            ssStep += dRy[i] * dRy[i] + dRp[i] * dRp[i];
            ++R.rStepN;
        }
    }
    if (R.rN) R.rMean[0] /= R.rN, R.rMean[1] /= R.rN;
    R.rStepRms[0] = detail::rmsAbout(sy, R.rStepN);
    R.rStepRms[1] = detail::rmsAbout(sp, R.rStepN);
    R.rStepRms2 = R.rStepN ? std::sqrt(ssStep / R.rStepN) : 0.0;
    R.enough = R.rN >= kFitMinFrames;
    if (R.enough) {
        double e[kFitMaxFrames];
        R.bestRms = std::numeric_limits<double>::infinity();
        for (int d = 0; d < 3; ++d) {
            double best[2] = {};
            int sgn[2] = {1, 1};
            for (int axis = 0; axis < 2; ++axis) {
                double ssBest = std::numeric_limits<double>::infinity();
                for (int s = 1; s >= -1; s -= 2) {
                    uint32_t m = 0;
                    for (uint32_t i = 0; i < n; ++i)
                        if (w.f[i].rOk) e[m++] = w.f[i].r[axis] - s * w.f[i].sum[d][axis];
                    const double ss = detail::ssPerBase(e, rb, m);
                    if (ss < ssBest) {
                        ssBest = ss;
                        sgn[axis] = s;
                    }
                }
                best[axis] = ssBest;
            }
            R.rmsD[d] = std::sqrt((best[0] + best[1]) / R.rN);
            if (R.rmsD[d] < R.bestRms) {
                R.bestRms = R.rmsD[d];
                R.bestD = d + 1;
                R.signY = sgn[0];
                R.signP = sgn[1];
            }
        }
        R.h1 = R.bestRms <= kH1FitDeg && R.rStepRms2 >= kH1StepDeg;
        // r against the cancel missing from the pose (the same sign and offset freedom)
        double ss[2] = {};
        uint32_t mb[kFitMaxFrames];
        for (int axis = 0; axis < 2; ++axis) {
            double ssBest = std::numeric_limits<double>::infinity();
            for (int s = 1; s >= -1; s -= 2) {
                uint32_t m = 0;
                for (uint32_t i = 0; i < n; ++i)
                    if (w.f[i].rOk && w.f[i].missingOk) {
                        mb[m] = w.f[i].rBase;
                        e[m++] = w.f[i].r[axis] - s * w.f[i].missing[axis];
                    }
                R.missN = m;
                const double v = detail::ssPerBase(e, mb, m);
                if (v < ssBest) ssBest = v;
            }
            ss[axis] = ssBest;
        }
        R.missRms = R.missN ? std::sqrt((ss[0] + ss[1]) / R.missN) : 0.0;
    }
    // the view's step against the head's
    {
        double s = 0.0;
        for (uint32_t i = 0; i < n; ++i) {
            const FitFrame& a = w.f[i];
            if (!a.viewOk || !a.headOk) continue;
            const double dv = a.viewStep - std::sqrt(a.head[0] * a.head[0] + a.head[1] * a.head[1]);
            s += dv * dv;
            ++R.viewN;
        }
        R.viewHeadRms = R.viewN ? std::sqrt(s / R.viewN) : 0.0;
    }
    // the rifle in the view: its steps, and the correlations with the head's step
    bool fOk[kFitMaxFrames] = {};
    double dFy[kFitMaxFrames] = {}, dFp[kFitMaxFrames] = {};
    {
        double fy[kFitMaxFrames], fp[kFitMaxFrames];
        for (uint32_t i = 1; i < n; ++i) {
            if (!w.f[i].rifleOk || !w.f[i - 1].rifleOk) continue;
            fOk[i] = true;
            dFy[i] = wrapDeg(w.f[i].rifle[0] - w.f[i - 1].rifle[0]);
            dFp[i] = wrapDeg(w.f[i].rifle[1] - w.f[i - 1].rifle[1]);
            fy[R.rifleStepN] = dFy[i];
            fp[R.rifleStepN] = dFp[i];
            ++R.rifleStepN;
        }
        R.rifleRms[0] = detail::rmsAbout(fy, R.rifleStepN);
        R.rifleRms[1] = detail::rmsAbout(fp, R.rifleStepN);
    }
    for (int k = 0; k < 3; ++k) {
        double hx[2][kFitMaxFrames], jr[2][kFitMaxFrames], hf[2][kFitMaxFrames], jf[2][kFitMaxFrames];
        uint32_t mr = 0, mf = 0;
        for (uint32_t i = 0; i + static_cast<uint32_t>(k) < n; ++i) {
            if (!w.f[i].headOk) continue;
            const uint32_t j = i + static_cast<uint32_t>(k);
            if (stepOk[j]) {
                hx[0][mr] = w.f[i].head[0];
                hx[1][mr] = w.f[i].head[1];
                jr[0][mr] = dRy[j];
                jr[1][mr] = dRp[j];
                ++mr;
            }
            if (fOk[j]) {
                hf[0][mf] = w.f[i].head[0];
                hf[1][mf] = w.f[i].head[1];
                jf[0][mf] = dFy[j];
                jf[1][mf] = dFp[j];
                ++mf;
            }
        }
        R.xrN[k] = mr;
        R.xfN[k] = mf;
        for (int axis = 0; axis < 2; ++axis) {
            R.xr[axis][k] = detail::pearson(hx[axis], jr[axis], mr);
            R.xf[axis][k] = detail::pearson(hf[axis], jf[axis], mf);
        }
    }
    // the sight lock
    {
        double sa[kFitMaxFrames], spp[kFitMaxFrames];
        double lastA = 0.0, lastP = 0.0, firstA = 0.0, firstP = 0.0;
        bool haveFirst = false, havePrevStep = false;
        double prevDa = 0.0, prevDp = 0.0;
        for (uint32_t i = 0; i < n; ++i) {
            const FitFrame& a = w.f[i];
            if (!a.sightOk) {
                havePrevStep = false;
                continue;
            }
            ++R.sightN;
            if (!haveFirst) {
                haveFirst = true;
                firstA = a.along;
                firstP = a.perp;
            }
            lastA = a.along;
            lastP = a.perp;
            if (i > 0 && w.f[i - 1].sightOk) {
                const double da = a.along - w.f[i - 1].along, dp = a.perp - w.f[i - 1].perp;
                sa[R.sightStepN] = da;
                spp[R.sightStepN] = dp;
                ++R.sightStepN;
                if (havePrevStep) {
                    if (da * prevDa < 0.0 && std::fabs(da) >= kLockMm && std::fabs(prevDa) >= kLockMm) ++R.revAlong;
                    if (dp * prevDp < 0.0 && std::fabs(dp) >= kLockMm && std::fabs(prevDp) >= kLockMm) ++R.revPerp;
                }
                prevDa = da;
                prevDp = dp;
                havePrevStep = true;
            } else {
                havePrevStep = false;
            }
        }
        double ssA = 0.0, ssP = 0.0;
        for (uint32_t i = 0; i < R.sightStepN; ++i) ssA += sa[i] * sa[i], ssP += spp[i] * spp[i];
        R.stepRmsAlong = R.sightStepN ? std::sqrt(ssA / R.sightStepN) : 0.0;
        R.stepRmsPerp = R.sightStepN ? std::sqrt(ssP / R.sightStepN) : 0.0;
        R.netAlong = haveFirst ? lastA - firstA : 0.0;
        R.netPerp = haveFirst ? lastP - firstP : 0.0;
        if (haveFirst && R.still && w.prevValid && w.prevMoving)
            R.overshoot = (R.netAlong * w.prevNet[0] < 0.0 && std::fabs(R.netAlong) >= kLockMm) || (R.netPerp * w.prevNet[1] < 0.0 && std::fabs(R.netPerp) >= kLockMm);
    }
    R.lock = R.revAlong > 0 || R.revPerp > 0 || R.overshoot;
    R.h3 = R.enough && R.rStepN > 0 && R.rStepRms2 < kH3FlatDeg && R.viewN > 0 && R.viewHeadRms < kH3ViewDeg && R.sightStepN > 0 && R.stepRmsAlong < kH3SightMm &&
           R.stepRmsPerp < kH3SightMm;
    return R;
}
// After a window is fitted: the next window's overshoot test needs this one's motion; the window starts again.
inline void fitCarry(FitWindow& w, const FitResult& R) noexcept {
    w.prevValid = R.sightN > 0;
    w.prevMoving = !R.still;
    w.prevNet[0] = R.netAlong;
    w.prevNet[1] = R.netPerp;
    ++w.index;
    w.clear();
}
inline void appendCorr(char* buf, size_t cap, size_t& at, const double c[3]) noexcept {
    for (int k = 0; k < 3; ++k) {
        if (k) appendf(buf, cap, at, "/");
        if (std::isfinite(c[k])) appendf(buf, cap, at, "%+.2f", c[k]);
        else appendf(buf, cap, at, "-");
    }
}
// The fit line's text (after "ads transition (F89) press fit (F91) ").
inline size_t formatFit(char* buf, size_t cap, const FitResult& R) noexcept {
    size_t at = 0;
    if (buf && cap) buf[0] = '\0';
    appendf(buf, cap, at, "window %u: t %+.2f to %+.2f s, %u frames, head %s (net %.1f deg/s; tremor rms yaw %.3f pitch %.3f deg a frame over %u head steps)", R.index, R.t0, R.t1,
            R.frames, R.still ? "STILL" : "MOVING", R.netRate, R.headRms[0], R.headRms[1], R.headN);
    appendf(buf, cap, at, "; r: %u frames, mean yaw %+.3f pitch %+.3f deg, step rms %.4f deg (yaw %.4f pitch %.4f about the mean, %u steps)", R.rN, R.rMean[0], R.rMean[1],
            R.rStepRms2, R.rStepRms[0], R.rStepRms[1], R.rStepN);
    if (R.enough)
        appendf(buf, cap, at, "; H1 fit r = the last d applied steps: rms d1 %.4f d2 %.4f d3 %.4f deg, best d %d (signs yaw %+d pitch %+d) %.4f; r against the cancel missing from the pose "
                "%.4f deg rms over %u frames", R.rmsD[0], R.rmsD[1], R.rmsD[2], R.bestD, R.signY, R.signP, R.bestRms, R.missRms, R.missN);
    else
        appendf(buf, cap, at, "; H1 fit: too few r frames (%u, need %u)", R.rN, kFitMinFrames);
    appendf(buf, cap, at, "; rifle in view: step rms yaw %.4f pitch %.4f deg (%u steps); view step - head step rms %.4f deg (%u frames)", R.rifleRms[0], R.rifleRms[1],
            R.rifleStepN, R.viewHeadRms, R.viewN);
    appendf(buf, cap, at, "; head step against, k = 0/1/2 frames later: r step yaw ");
    appendCorr(buf, cap, at, R.xr[0]);
    appendf(buf, cap, at, " pitch ");
    appendCorr(buf, cap, at, R.xr[1]);
    appendf(buf, cap, at, " (%u/%u/%u pairs), rifle step yaw ", R.xrN[0], R.xrN[1], R.xrN[2]);
    appendCorr(buf, cap, at, R.xf[0]);
    appendf(buf, cap, at, " pitch ");
    appendCorr(buf, cap, at, R.xf[1]);
    appendf(buf, cap, at, " (%u/%u/%u pairs)", R.xfN[0], R.xfN[1], R.xfN[2]);
    appendf(buf, cap, at, "; sight: %u frames, step rms along %.3f perp %.3f mm, reversals of %.1f mm or more along %u perp %u, net along %+.3f perp %+.3f mm, overshoot after the head "
            "stopped %s", R.sightN, R.stepRmsAlong, R.stepRmsPerp, kLockMm, R.revAlong, R.revPerp, R.netAlong, R.netPerp, R.overshoot ? "YES" : "no");
    appendf(buf, cap, at, "; verdict: ");
    if (!R.enough) {
        appendf(buf, cap, at, "none (too few r frames)");
        return at;
    }
    bool any = false;
    if (R.h1) {
        appendf(buf, cap, at, "H1 (the cancel lands %d frame%s late)", R.bestD, R.bestD == 1 ? "" : "s");
        any = true;
    }
    if (R.lock) {
        appendf(buf, cap, at, "%sLOCK (the sight lock moves the eye back and forth)", any ? " + " : "");
        any = true;
    }
    if (R.h3) {
        appendf(buf, cap, at, "%sH3 (the view follows the head: the readout pin's one-frame lag is the suspect)", any ? " + " : "");
        any = true;
    }
    if (!any) appendf(buf, cap, at, "none of H1, LOCK or H3");
    return at;
}

}  // namespace f91
}  // namespace nofoot
}  // namespace edvr
