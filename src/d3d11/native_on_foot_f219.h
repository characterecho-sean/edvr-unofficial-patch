// F219 (native on-foot VR): at the hip the elbow's swing about the shoulder-to-hand axis is limited per frame. Header only, no game or D3D11 state, so native_frame_test runs the very functions the
// arm solve uses.
//
// What the flight established. Recording 20261106_090823, the arc cutter and the laser rifle: the hip swivel cap (F181a) made one-frame hops of -60.4, -75.7 and -60.8 deg, and with the arm folded to
// its 40 deg limit its search landed 60 deg away for three frames (-82, -141, -143, -82 deg) while the elbow moved 25 cm and back. The flown medians of the swivel's step a frame are 0.7 to 1.5 deg;
// the hops were 15 to 60.
//
// The rule. For each arm at the hip the solved elbow, as a vector from the shoulder, is remembered. On the next call its swing about the shoulder-to-hand axis, from the remembered vector to
// the solved one, is held to kMaxRateDegS times the time between the calls (4 deg at 90 Hz). The swivel is turned back by what is over, so only the written elbow moves: the hand does not (the
// swivel turns the elbow about the axis through the hand), and the pole's seed and tracking are not touched. A memory older than kFreshMs, from another item or another plan, or from a call
// that did not write, is not used. In the sights and anywhere the arm solve is not at the hip nothing here limits.
//
// One writer: the arm solve (under g_pfzBusy). No key: it always helps.
#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace edvr {
namespace nofoot {
namespace f219 {

constexpr double kMaxRateDegS = 360.0;   // the swing's limit: 4 deg a frame at 90 Hz
constexpr double kMinDtS = 1.0 / 240.0;  // the time between calls is read at least this long (a repeat call of the same game frame must not shrink the limit to nothing)
constexpr double kMaxDtS = 0.05;         // ... and at most this long
constexpr uint64_t kFreshMs = 200;       // a remembered elbow is used for this long
constexpr double kMinRadiusM = 0.005;    // the elbow's distance from the axis, under which its angle about it means nothing
constexpr double kPi = 3.14159265358979323846;

// The most the elbow may swing in one call (rad), for the time since the last call (s).
inline double maxStepRad(double dtS) noexcept {
    if (!(dtS > kMinDtS)) dtS = kMinDtS;
    if (dtS > kMaxDtS) dtS = kMaxDtS;
    return kMaxRateDegS * kPi / 180.0 * dtS;
}

// The signed angle (rad) from prev's part perpendicular to the axis to now's, about the unit axis (right-handed). False when either perpendicular part is under kMinRadiusM.
inline bool signedAngleAbout(const double prev[3], const double now[3], const double axis[3], double* out) noexcept {
    double p[3], n[3];
    const double dp = prev[0] * axis[0] + prev[1] * axis[1] + prev[2] * axis[2], dn = now[0] * axis[0] + now[1] * axis[1] + now[2] * axis[2];
    for (int i = 0; i < 3; ++i) {
        p[i] = prev[i] - dp * axis[i];
        n[i] = now[i] - dn * axis[i];
    }
    const double lp = std::sqrt(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]), ln = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
    if (!(lp >= kMinRadiusM) || !(ln >= kMinRadiusM) || !std::isfinite(lp) || !std::isfinite(ln)) return false;
    const double c[3] = {p[1] * n[2] - p[2] * n[1], p[2] * n[0] - p[0] * n[2], p[0] * n[1] - p[1] * n[0]};
    const double s = c[0] * axis[0] + c[1] * axis[1] + c[2] * axis[2], d = p[0] * n[0] + p[1] * n[1] + p[2] * n[2];
    *out = std::atan2(s, d);
    return std::isfinite(*out);
}

// phi held to +-maxStep.
inline double limitAngle(double phi, double maxStep) noexcept { return phi > maxStep ? maxStep : phi < -maxStep ? -maxStep : phi; }

// The elbow of one side as the last write left it: against the shoulder, in the model's axes, with when, for which item and which plan.
struct ElbowMemory {
    double rel[3] = {};
    uint64_t ms = 0;
    bool have = false;
    uint32_t item = 0, generation = 0;
    void remember(const double relNow[3], uint64_t nowMs, uint32_t itemSerial, uint32_t gen) noexcept {
        for (int i = 0; i < 3; ++i) rel[i] = relNow[i];
        ms = nowMs;
        have = true;
        item = itemSerial;
        generation = gen;
    }
    void reset() noexcept { *this = ElbowMemory{}; }
    bool fresh(uint64_t nowMs, uint32_t itemSerial, uint32_t gen) const noexcept { return have && item == itemSerial && generation == gen && nowMs >= ms && nowMs - ms <= kFreshMs; }
};

struct Result {
    bool measured = false;   // the swing was read (the elbow was far enough from the axis, both times)
    bool limited = false;    // the swivel was turned back
    double swingDeg = 0.0;   // the swing before the limit
    double cutDeg = 0.0;     // what was taken off it (positive)
};

// The limit on a solved arm. tb is the two-bone frame (S the shoulder, hand the solve's hand, elbowAt(theta) the elbow); mem the side's memory, which the caller has found fresh; dtS the time since
// the memory was made. theta (the swivel) is turned back by what the swing is over the limit; a swivel whose effect cannot be measured is left as it is.
template <class Frame>
inline Result limitSwivel(const Frame& tb, const ElbowMemory& mem, double dtS, double* theta) noexcept {
    Result r;
    double axis[3] = {tb.hand[0] - tb.S[0], tb.hand[1] - tb.S[1], tb.hand[2] - tb.S[2]};
    const double len = std::sqrt(axis[0] * axis[0] + axis[1] * axis[1] + axis[2] * axis[2]);
    if (!(len > 1e-9)) return r;
    for (double& a : axis) a /= len;
    double E[3], rel[3];
    tb.elbowAt(*theta, E);
    for (int i = 0; i < 3; ++i) rel[i] = E[i] - tb.S[i];
    double phi = 0.0;
    if (!signedAngleAbout(mem.rel, rel, axis, &phi)) return r;
    r.measured = true;
    r.swingDeg = phi * 180.0 / kPi;
    const double maxStep = maxStepRad(dtS);
    if (!(std::fabs(phi) > maxStep)) return r;
    double E1[3], rel1[3], eff = 0.0;
    tb.elbowAt(*theta + 0.01, E1);
    for (int i = 0; i < 3; ++i) rel1[i] = E1[i] - tb.S[i];
    if (!signedAngleAbout(rel, rel1, axis, &eff) || eff == 0.0) return r;   // the swivel cannot be measured: the solve's own stands
    const double cut = phi - limitAngle(phi, maxStep);
    *theta -= (eff > 0.0 ? 1.0 : -1.0) * cut;
    r.limited = true;
    r.cutDeg = std::fabs(cut) * 180.0 / kPi;
    return r;
}

// ---- the log window ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

struct Stats {
    uint32_t solves[2] = {}, limited[2] = {};
    double cutMaxDeg[2] = {};
    // One arm solve at the hip (side 0 left, 1 right): whether the swing was limited and the cut (deg).
    void add(int side, bool wasLimited, double cutDeg) noexcept {
        if (side < 0 || side > 1) return;
        ++solves[side];
        if (wasLimited) {
            ++limited[side];
            if (cutDeg > cutMaxDeg[side]) cutMaxDeg[side] = cutDeg;
        }
    }
    bool any() const noexcept { return solves[0] + solves[1] > 0; }
    void clear() noexcept { *this = Stats{}; }
};

// The window's line. tools\edvr_log.py reads it.
inline int formatLine(char* buf, size_t n, uint32_t seconds, const Stats& s) noexcept {
    return std::snprintf(buf, n, "native on foot elbow step (F219) %u s: left solves %u limited %u largest cut %.1f deg; right solves %u limited %u largest cut %.1f deg", seconds, s.solves[0], s.limited[0],
                         s.cutMaxDeg[0], s.solves[1], s.limited[1], s.cutMaxDeg[1]);
}

}  // namespace f219
}  // namespace nofoot
}  // namespace edvr
