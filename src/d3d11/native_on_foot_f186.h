// F186 (native on foot VR): the ADS lock spiral at a steep aim. The ADS terms of the eye alignment pre-turned by the aim pitch under mouse look, the sight lock's response measured,
// and a stability stop. Header only, no game or D3D11 state, so native_frame_test runs the very functions the frame thread uses. Journal: the arc doc, F186.
//
// The flight edvr_gfx_20261030_144707 (build c1f91eba; the pilot: "still enters a feedback loop with ADS"): F184 (the guard measuring only, the raise asked of the legs) did not stop it. Engaging
// ADS at the +85 aim clamp from the hip ran away (14:50:00-10 and 14:50:28-30 at +81.9): the sight lock's e after the lock 20-26 cm, before it 47-60 cm, the applied lock at its 40 cm clamp, the view
// 18-40 mm rms with the head still. The applied lock's (up, forward) part, per second: (12.5, 9.3), (34.5, -6.4), (27.8, -22.3), (11.5, -34.1), (-7.5, -35.4), (26.6, -26.8), (-13.4, -35.4),
// (28.5, -22.9), (-11.5, -34.5) cm, the right part steady at 7 cm: a correction of near-constant size that ROTATES. Engaged level and then raised to the same +85.2 it held (lock 8-9 cm, e after
// 0.3-0.9 cm).
//
// The plant. F172 proved (the carriage line of the flight edvr_gfx_20261029_054640) that under mouse look the game applies the eye alignment, a seated vector, in the aim-pitched frame: a body
// offset (forward f, up u) goes out as R(a) (f, u) = (f cos a - u sin a, f sin a + u cos a), a the aim's pitch, nose up positive. F172 pre-turned the neck raise only. The sight lock's solve
// (adsSightTick: the error in the body's axes, seatedFromBody at psi 0, F93b's target = the shift the measured eye was rendered with + the error measured on it) knows nothing of a: it assumes the
// shift lands where it was meant (the plant at psi 0 is diag(-1, -1, +1)). The loop's response to a commanded change d is then r = -R(a) d in the seated (y, z) plane (y = -up, z = forward;
// R(phi) w = w e^{i phi} for w = y + i z), and the controller adds the error each frame: v' = v + e0 - R(a) v, the factor 1 - e^{ia}, of magnitude 2 sin(a/2): over 1 from a = 60 deg, and
// rotating. At a = 85 deg the lock spirals out to its clamp. The lock's own solve does NOT account for the turn (read: adsSightTick, adsSightErrorEyeK, F93b's sightTarget: no pitch in any of
// them), so turning the terms the game turns does not double a compensation.
//
// ---- a. the lock's response (always on, measure only)
// Each rendered frame m the lock reports the shift the measured eye was rendered with (v_m, the meant shift under the serial the runtime reported with the rows: the F93b history) and the error
// measured on that eye (e_m, seated metres). Consecutive frames give the pair (d, r) = (v_m - v_{m-1}, e_m - e_{m-1}): the change commanded and the response at the serial lag. Per aim-pitch bin
// (|a| under 30, 30 to 60, 60 to 75, over 75 deg) the map r = c d with the complex c = sum(r conj d) / sum(|d|^2) (w = y + i z): gain |c|, angle arg(-c). A correct loop reads gain 1, angle 0
// (c = -1); the game's turn reads the aim pitch. corr = |sum(r conj d)| / sqrt(sum|d|^2 sum|r|^2) says how much of r the map explains.
//
// ---- b. the pre-turn (behaviour since F187: the F186 flight with it fixed the runaway, 0 stops; the A/B key experimental.native_on_foot_ads_preturn is retired)
// Under mouse look, with F172's conditions (look to aim not driving the aim, the aim pitch fresh, the on-foot camera), the sum of the ADS terms of the eye alignment, the position lock
// (g_adsLock), the sight lock (g_adsSight.applied) and the weapon eye offset (F88: weaponEye.out.seated, which carries the ADS eye depth), goes out turned by -a in the (forward, up) plane, as
// F172 turns the raise: y' = y cos a + z sin a, z' = z cos a - y sin a. The game's R(a) of it lands the vector where it was meant. The F70 shift (what the pin takes the camera less) and F119's
// sums stay the unturned vectors, as F172 kept the neck record unturned. weaponView.seated (the manual weapon view bias) is a seated offset in the same sense but a right-only vector (only its
// seat-yaw z share would turn), not an ADS term and not in the loop: left as it was; so is F87h's height hold.
//
// ---- c. the stability stop (always on, counted)
// If the lock's e after the lock stays over 10 cm, or its applied vector sits at its clamp, for 0.3 s, the lock is released (the applied vector ramped to zero over 0.15 s) and re-acquired from
// zero. A loop nobody has proven closed: it must be rare, and it is counted.
#pragma once

#include "native_on_foot_core.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace edvr {
namespace nofoot {
namespace f186 {

constexpr double kPi = 3.14159265358979323846;

// ---- b. the pre-turn ----------------------------------------------------------------------------------------------------------------------------------------------------------------

struct Turned {
    bool turned = false;
    double dy = 0.0, dz = 0.0;   // what the turn adds to the published y and z (the unturned vector stays what the other readers take)
    double pitchRad = 0.0;       // the aim pitch used (0 when unturned)
};
// The ADS terms' seated sum t (x, y, z) turned for an aim pitch of aimPitchRad: unturned (turned false, dy = dz = 0) unless the camera is the commander's first-person one, look to
// aim is not driving the aim, the aim pitch is fresh and a number and not 0, and the sum is not exactly zero.
inline Turned turnAds(const double t[3], bool lookToAim, bool aimFresh, double aimPitchRad, bool firstPerson) noexcept {
    Turned r;
    if (!firstPerson || lookToAim || !aimFresh || !std::isfinite(aimPitchRad) || aimPitchRad == 0.0) return r;
    if (!std::isfinite(t[1]) || !std::isfinite(t[2]) || (t[1] == 0.0 && t[2] == 0.0)) return r;
    const double c = std::cos(aimPitchRad), s = std::sin(aimPitchRad);
    r.turned = true;
    r.pitchRad = aimPitchRad;
    r.dy = t[1] * c + t[2] * s - t[1];
    r.dz = t[2] * c - t[1] * s - t[2];
    return r;
}
// The game's turn of a seated vector's (y, z) share, as the rig models it: the body offset (forward, up) = (z, -y) turned nose up by the aim pitch, back to (y, z).
inline void gameTurnSeated(double aimPitchRad, double y, double z, double* yOut, double* zOut) noexcept {
    const double c = std::cos(aimPitchRad), s = std::sin(aimPitchRad), f = z, u = -y;
    *zOut = f * c - u * s;
    *yOut = -(f * s + u * c);
}

// ---- a. the lock's response --------------------------------------------------------------------------------------------------------------------------------------------------------------

constexpr int kBins = 4;
inline int binOf(double aimDeg) noexcept {
    const double a = std::fabs(aimDeg);
    return a < 30.0 ? 0 : a < 60.0 ? 1 : a < 75.0 ? 2 : 3;
}
inline const char* binName(int b) noexcept { return b == 0 ? "under 30" : b == 1 ? "30-60" : b == 2 ? "60-75" : "over 75"; }
constexpr double kMinStepM = 0.001;    // a commanded change under this (in y and z) is noise: no pair
constexpr double kMaxStepM = 0.10;     // and one over this is a jump (a clamp, a release), not a response
constexpr uint32_t kMinPairs = 20;     // the health rule: fewer pairs than this in a bin are too few to judge
constexpr double kAngleFlagDeg = 30.0; // the health rule: a bin's angle this far from a correct loop's 0

struct Bin {
    uint32_t n = 0;
    double dd = 0.0, rdRe = 0.0, rdIm = 0.0, rr = 0.0, aimSum = 0.0;
    double gain() const noexcept { return dd > 0.0 ? std::sqrt(rdRe * rdRe + rdIm * rdIm) / dd : 0.0; }
    // arg(-c): the response r = -g R(phi) d reads phi (deg); 0 for a correct loop, the aim pitch for the game's turn
    double angleDeg() const noexcept {
        if (!(dd > 0.0)) return 0.0;
        const double deg = std::atan2(-rdIm, -rdRe) * 180.0 / kPi;
        return deg == 0.0 ? 0.0 : deg;   // never a printed "-0.0"
    }
    double corr() const noexcept { return dd > 0.0 && rr > 0.0 ? std::sqrt(rdRe * rdRe + rdIm * rdIm) / std::sqrt(dd * rr) : 0.0; }
    double aimMean() const noexcept { return n ? aimSum / static_cast<double>(n) : 0.0; }
};
struct Response {
    Bin bin[kBins];
    uint32_t lockFrames = 0, turnedFrames = 0, stops = 0, pairs = 0;
    // One pair: the change commanded (dv) and the response (de) in the seated (y, z) plane, at the aim pitch aimDeg. False (nothing added) for a step outside [kMinStepM, kMaxStepM].
    bool addPair(double aimDeg, double dvy, double dvz, double dey, double dez) noexcept {
        if (!std::isfinite(aimDeg + dvy + dvz + dey + dez)) return false;
        const double m = std::sqrt(dvy * dvy + dvz * dvz);
        if (!(m >= kMinStepM) || !(m <= kMaxStepM)) return false;
        Bin& b = bin[binOf(aimDeg)];
        ++b.n;
        ++pairs;
        b.dd += dvy * dvy + dvz * dvz;
        b.rdRe += dey * dvy + dez * dvz;   // r conj(d) = (ry + i rz)(dy - i dz)
        b.rdIm += dez * dvy - dey * dvz;
        b.rr += dey * dey + dez * dez;
        b.aimSum += aimDeg;
        return true;
    }
    void merge(const Response& o) noexcept {
        for (int i = 0; i < kBins; ++i) {
            bin[i].n += o.bin[i].n; bin[i].dd += o.bin[i].dd; bin[i].rdRe += o.bin[i].rdRe; bin[i].rdIm += o.bin[i].rdIm; bin[i].rr += o.bin[i].rr; bin[i].aimSum += o.bin[i].aimSum;
        }
        lockFrames += o.lockFrames; turnedFrames += o.turnedFrames; stops += o.stops; pairs += o.pairs;
    }
};
// Consecutive rendered frames' (shift, error) into (d, r): a pair only from frame m-1 to m.
struct Pairer {
    bool have = false;
    uint32_t frame = 0;
    double v[3] = {}, e[3] = {};
    void reset() noexcept { have = false; }
    bool step(uint32_t f, const double vNow[3], const double eNow[3], double dv[3], double de[3]) noexcept {
        const bool pair = have && f == frame + 1u;
        if (pair)
            for (int i = 0; i < 3; ++i) {
                dv[i] = vNow[i] - v[i];
                de[i] = eNow[i] - e[i];
            }
        have = true;
        frame = f;
        for (int i = 0; i < 3; ++i) {
            v[i] = vNow[i];
            e[i] = eNow[i];
        }
        return pair;
    }
};

// ---- c. the stability stop ---------------------------------------------------------------------------------------------------------------------------------------------------------------

constexpr double kStopPerpM = 0.10, kStopSec = 0.3, kRampSec = 0.15;
enum StopReason : int { kReasonNone = 0, kReasonError = 1, kReasonClamp = 2, kReasonBoth = 3 };
inline const char* reasonName(int r) noexcept { return r == kReasonError ? "e after the lock over 10 cm" : r == kReasonClamp ? "the applied lock at its clamp" : r == kReasonBoth ? "e over 10 cm and the lock at its clamp" : "none"; }
struct StopGuard {
    bool bad = false, releasing = false;
    int badReason = 0;
    uint64_t badSinceMs = 0, releaseSinceMs = 0;
    double start[3] = {};
    uint32_t stops = 0;
    struct Out {
        bool stopped = false;       // the stop began this call
        bool ramping = false;       // the applied vector is being ramped to zero: scale it by `scale` from the start vector
        bool zeroNow = false;       // the ramp is done: zero the lock whole (re-acquired from zero from the next call)
        double scale = 1.0;
        int reason = 0;
    };
    void reset() noexcept { *this = StopGuard{}; }
    // active: the sight lock is running (ADS active, the guard F63 not tripped). measured: this call has a measurement (lockOk). perpM: |e perpendicular| after the lock; appliedM: |applied|
    // after this call's step; clampM: the lock's clamp. pos: the applied vector (the ramp's start when a stop begins).
    Out step(uint64_t nowMs, bool active, bool measured, double perpM, double appliedM, double clampM, const double pos[3]) noexcept {
        Out o;
        if (!active) {
            reset();
            return o;
        }
        if (releasing) {
            const double t = nowMs >= releaseSinceMs ? static_cast<double>(nowMs - releaseSinceMs) * 1e-3 : 0.0;
            if (t >= kRampSec - 1e-9) {
                releasing = false;
                bad = false;
                o.zeroNow = true;
                o.scale = 0.0;
                return o;
            }
            o.ramping = true;
            o.scale = 1.0 - t / kRampSec;
            return o;
        }
        if (!measured) return o;   // nothing measured: the condition stands as it was
        const bool overE = std::isfinite(perpM) && perpM > kStopPerpM, atClamp = std::isfinite(appliedM) && appliedM >= clampM - 1e-6;
        if (!overE && !atClamp) {
            bad = false;
            return o;
        }
        const int reason = (overE ? kReasonError : 0) | (atClamp ? kReasonClamp : 0);
        if (!bad) {
            bad = true;
            badSinceMs = nowMs;
            badReason = reason;
        } else {
            badReason |= reason;
        }
        if (nowMs >= badSinceMs && static_cast<double>(nowMs - badSinceMs) * 1e-3 >= kStopSec - 1e-9) {
            releasing = true;
            releaseSinceMs = nowMs;
            for (int i = 0; i < 3; ++i) start[i] = pos[i];
            ++stops;
            o.stopped = true;
            o.ramping = true;
            o.scale = 1.0;
            o.reason = badReason;
            bad = false;
        }
        return o;
    }
};

// ---- the lines ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------

// The window's line, printed whenever the lock ran (a lock frame): how many lock frames turned the ADS terms, the stops, and each aim-pitch bin that had pairs.
inline int formatResponse(char* buf, size_t n, uint32_t seconds, const Response& r) noexcept {
    int len = std::snprintf(buf, n,
                            "native on foot ADS lock response (F186) %u s: the ADS terms turned in %u of %u lock frames; lock stops %u; response of the measured error to the lock's change "
                            "(a correct loop reads gain 1.00 angle +0.0 deg, the game's turn the aim pitch): ",
                            seconds, r.turnedFrames, r.lockFrames, r.stops);
    if (len < 0 || static_cast<size_t>(len) >= n) return len;
    bool any = false;
    for (int b = 0; b < kBins; ++b) {
        const Bin& bin = r.bin[b];
        if (!bin.n) continue;
        const int w = std::snprintf(buf + len, n - static_cast<size_t>(len), "%saim %s deg: %u pairs, gain %.2f, angle %+.1f deg, corr %.2f, mean aim %+.1f deg", any ? "; " : "", binName(b), bin.n, bin.gain(),
                                    bin.angleDeg(), bin.corr(), bin.aimMean());
        if (w < 0 || static_cast<size_t>(w) >= n - static_cast<size_t>(len)) break;
        len += w;
        any = true;
    }
    if (!any) {
        const int w = std::snprintf(buf + len, n - static_cast<size_t>(len), "no pairs");
        if (w > 0) len += w;
    }
    return len;
}
// One stop's line: why, the figures and the aim pitch.
inline int formatStop(char* buf, size_t n, int reason, double perpCm, double appliedCm, double clampCm, bool aimKnown, double aimDeg, uint32_t stopsTotal) noexcept {
    return std::snprintf(buf, n,
                         "native on foot ADS lock stop (F186): the sight lock held over its limit for %.1f s (%s; e after the lock %.1f cm, applied %.1f cm of %.0f) and is released over %.2f s and acquired again from zero; "
                         "aim pitch %+.1f deg%s; stops this session %u",
                         kStopSec, reasonName(reason), perpCm, appliedCm, clampCm, kRampSec, aimKnown ? aimDeg : 0.0, aimKnown ? "" : " (not fresh)", stopsTotal);
}

}  // namespace f186
}  // namespace nofoot
}  // namespace edvr
