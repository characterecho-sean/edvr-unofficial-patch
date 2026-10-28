// F143 (native on-foot VR): the pure parts of two changes. Header only, no game or D3D11 state, so native_frame_test runs the very functions the frame thread, the avatar
// hooks and the ADS sight lock use. Journal: the arc doc, 2026-10-27, "the F142 flight" and F143.
//
// 1. The ADS eye on the game's own sight line. F141's hook reads, on the game thread, the eye matrix E (position in row 3) and the sight point S the game's ADS bend aims
//    the shots through; the F142 flight showed the bend takes effect in VR (the shots go along the line E -> S) and that this line, taken at EDVR's eye depth, matches the
//    pilot's hand-trimmed totals within 0.32 cm on nine weapons. So the sight lock's shift (F139's, from a table and two live trims) is taken from that line:
//      - each usable sample paired with the FP snapshot of its own render frame gives E and S relative to that frame's P14w, in that frame's FP root axes (right, up, forward,
//        each made unit); a filter per activation (an exponential, tau 0.15 s, reset when the item, its scope or F141's activation changes) smooths the pair;
//      - the filtered pair is published with its time, the item and the scope;
//      - the sight lock, when the pair is for the held item and scope and under 0.25 s old, rebuilds E and S in the world from this frame's P14w and root, takes Q, the
//        point on the line E -> S at the lock's eye depth k along the root's forward from the origin, and shifts the eye from (origin + k f) to Q, less the part along f; the
//        two live trims (up along u, right along r) are added, under F139's 8 cm bound. A line it cannot use falls back to F139's table, and the reason is counted.
//
// 2. The coat's clip modifiers. The fade hook's step reads, for every part of the local third-person avatar with an instance, the part's vertex clip modifier ("pVertexClipAmount",
//    part entry + 0x610: amount +8, enable +0xC, dirty +0xD) and view masks, every 5 s and when EDVR's armed state changes (the measure). With fix.native_on_foot_body_full_outfit
//    on, F19's unclip (enable 0, dirty 1) also covers every non-head part its list does not (the A/B).
#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>
#include <type_traits>

#include "native_on_foot_f139.h"

namespace edvr {
namespace nofoot {
namespace f143 {

// ---- 1. the game's sight line ---------------------------------------------------------------------------------------------------------------------------------------
constexpr double kTauS = 0.15;           // the pair's filter: an exponential, time constant in seconds
constexpr uint64_t kStaleMs = 250;       // a published pair older than this is not used
constexpr double kMinAlongM = 0.01;      // |dot(S - E, f)| under 1 cm: the line has no depth to take Q on

// The vectors the filter and the choice work in: root axes are (right, up, forward) in metres from P14w.
namespace detail {
inline double dot3(const double a[3], const double b[3]) noexcept { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
inline bool unit3(const double a[3], double out[3]) noexcept {
    const double l = std::sqrt(dot3(a, a));
    if (!std::isfinite(l) || !(l > 1e-9)) return false;
    for (int i = 0; i < 3; ++i) out[i] = a[i] / l;
    return true;
}
inline bool finite3(const double a[3]) noexcept { return std::isfinite(a[0]) && std::isfinite(a[1]) && std::isfinite(a[2]); }
}  // namespace detail

// The root's three unit axes from a row-vector matrix (rows 0, 1, 2 = right, up, forward); false when a row has no length.
inline bool rootAxes(const float froot[16], double r[3], double u[3], double f[3]) noexcept {
    const double a[3] = {froot[0], froot[1], froot[2]}, b[3] = {froot[4], froot[5], froot[6]}, c[3] = {froot[8], froot[9], froot[10]};
    return detail::unit3(a, r) && detail::unit3(b, u) && detail::unit3(c, f);
}
// A world point (three floats) as (right, up, forward) from P14w in the root's unit axes; false when an axis is unusable or a coordinate is not a number.
inline bool toRoot(const float froot[16], const double p14w[3], const float pos[3], double out[3]) noexcept {
    double r[3], u[3], f[3];
    if (!rootAxes(froot, r, u, f)) return false;
    const double d[3] = {pos[0] - p14w[0], pos[1] - p14w[1], pos[2] - p14w[2]};
    if (!detail::finite3(d)) return false;
    out[0] = detail::dot3(d, r);
    out[1] = detail::dot3(d, u);
    out[2] = detail::dot3(d, f);
    return detail::finite3(out);
}

// What a pair belongs to: the filter restarts when any of these changes.
struct LineKey {
    uint32_t held = 0;          // the held item's serial (g_heldItemSerial)
    bool coherent = false;      // the item snapshot was usable
    int scope = 0;              // the held weapon's scope state (loadout::Scope)
    uint32_t activation = 0;    // F141's activation count (the act or its sight handle changed)
    bool operator==(const LineKey& o) const noexcept { return held == o.held && coherent == o.coherent && scope == o.scope && activation == o.activation; }
    bool operator!=(const LineKey& o) const noexcept { return !(*this == o); }
};
// The filter. note() collects a tick's samples (the mean of a tick is one step: samples drained together carry one time); flush() applies that mean at the tick's time.
struct LineFilter {
    bool have = false;
    LineKey key;
    uint64_t ms = 0;
    double e[3] = {}, s[3] = {};
    uint32_t samples = 0;                  // samples folded in since the last reset
    double pe[3] = {}, ps[3] = {};
    uint32_t pn = 0;                       // pending samples of the tick
    void reset() noexcept { *this = LineFilter{}; }
    bool started = false;                  // a key was noted (the filter's own, set by the first note after a reset)
    void note(const LineKey& k, const double eRoot[3], const double sRoot[3]) noexcept {
        if (!started || k != key) {        // another item, scope or activation: the filter starts again from this sample
            reset();
            key = k;
            started = true;
        }
        for (int i = 0; i < 3; ++i) {
            pe[i] += eRoot[i];
            ps[i] += sRoot[i];
        }
        ++pn;
    }
    // Apply the tick's mean at nowMs. True when the state changed (the caller publishes).
    bool flush(uint64_t nowMs, double tauS = kTauS) noexcept {
        if (!pn) return false;
        double me[3], ms3[3];
        for (int i = 0; i < 3; ++i) {
            me[i] = pe[i] / pn;
            ms3[i] = ps[i] / pn;
        }
        if (!have) {
            for (int i = 0; i < 3; ++i) {
                e[i] = me[i];
                s[i] = ms3[i];
            }
            have = true;
        } else {
            const double dt = nowMs > ms ? static_cast<double>(nowMs - ms) / 1000.0 : 0.0;
            const double a = tauS > 0.0 ? 1.0 - std::exp(-dt / tauS) : 1.0;
            for (int i = 0; i < 3; ++i) {
                e[i] += a * (me[i] - e[i]);
                s[i] += a * (ms3[i] - s[i]);
            }
        }
        ms = nowMs;
        samples += pn;
        pn = 0;
        for (int i = 0; i < 3; ++i) pe[i] = ps[i] = 0.0;
        return true;
    }
};
// The published pair.
struct Pub {
    bool valid = false;
    uint64_t ms = 0;
    LineKey key;
    double e[3] = {}, s[3] = {};
};
inline void publish(Pub& p, const LineFilter& f) noexcept {
    p.valid = f.have;
    p.ms = f.ms;
    p.key = f.key;
    for (int i = 0; i < 3; ++i) {
        p.e[i] = f.e[i];
        p.s[i] = f.s[i];
    }
}

// Why a frame's shift came from the table and not the game's line (kGame: it came from the line). Order of the tests is the order here.
enum Why : uint8_t { kGame = 0, kNoPair, kStale, kOtherItem, kNoItem, kDegenerate, kBound, kKeyOff, kWhyCount };
inline const char* whyText(unsigned w) noexcept {
    static const char* const kText[kWhyCount] = {"the game's line", "no pair published", "pair stale (over 250 ms)", "another item or scope (or not a held weapon)", "item not read",
                                                 "degenerate line (under 1 cm of depth)", "past the 8 cm bound", "key off"};
    return w < kWhyCount ? kText[w] : "?";
}
struct Choice {
    uint8_t why = kKeyOff;       // kGame when the game's line is the source
    bool game = false;
    double shift[3] = {};        // world, metres: to be added to the origin (the game's shift and the two trims, perpendicular to f, within the bound)
    double gameShift[3] = {};    // the game's alone (world, perpendicular to f)
    double gameUpCm = 0.0, gameRightCm = 0.0;   // the game's alone along the root's up and right
    double shiftM = 0.0;         // |shift|
    double ageMs = 0.0;          // the pair's age
    double lineDeg = 0.0;        // the angle of S - E from the root's forward
    double q[3] = {};            // Q in the world
};
// The choice for one frame of the lock. keyOn: fix.native_on_foot_ads_game_sight. itemOk: the coherent item snapshot (F139's); weapon: a $wpn_ item; heldSerial and scopeState are
// the held item's now. p14w, up, right and fwd are this frame's P14 and FP root (axes of any length: unitised); origin is the lock's origin before any shift (P14w, or F79's
// measured origin) and k the lock's eye depth along fwd; trimUpCm and trimRightCm are the live keys' values (clamped here, 0 for a non-weapon).
inline Choice choose(bool keyOn, bool itemOk, bool weapon, uint32_t heldSerial, int scopeState, const Pub& pub, uint64_t nowMs, const double p14w[3], const double up[3],
                     const double right[3], const double fwd[3], const double origin[3], double k, double trimUpCm, double trimRightCm) noexcept {
    Choice c;
    if (!keyOn) { c.why = kKeyOff; return c; }
    if (!itemOk) { c.why = kNoItem; return c; }
    if (!pub.valid) { c.why = kNoPair; return c; }
    if (!weapon || !pub.key.coherent || pub.key.held != heldSerial || pub.key.scope != scopeState) { c.why = kOtherItem; return c; }
    c.ageMs = nowMs > pub.ms ? static_cast<double>(nowMs - pub.ms) : 0.0;
    if (c.ageMs > static_cast<double>(kStaleMs)) { c.why = kStale; return c; }
    double r[3], u[3], f[3];
    if (!detail::unit3(right, r) || !detail::unit3(up, u) || !detail::unit3(fwd, f) || !std::isfinite(k) || !detail::finite3(p14w) || !detail::finite3(origin) ||
        !detail::finite3(pub.e) || !detail::finite3(pub.s)) {
        c.why = kDegenerate;
        return c;
    }
    double ew[3], sw[3], d[3];
    for (int i = 0; i < 3; ++i) {
        ew[i] = p14w[i] + pub.e[0] * r[i] + pub.e[1] * u[i] + pub.e[2] * f[i];
        sw[i] = p14w[i] + pub.s[0] * r[i] + pub.s[1] * u[i] + pub.s[2] * f[i];
        d[i] = sw[i] - ew[i];
    }
    const double along = detail::dot3(d, f);
    const double len = std::sqrt(detail::dot3(d, d));
    if (!(std::fabs(along) >= kMinAlongM) || !(len > 0.0)) { c.why = kDegenerate; return c; }
    const double oe[3] = {origin[0] - ew[0], origin[1] - ew[1], origin[2] - ew[2]};
    const double t = (k + detail::dot3(oe, f)) / along;   // Q = E + t (S - E) has dot(Q - origin, f) = k
    double g[3];
    for (int i = 0; i < 3; ++i) {
        c.q[i] = ew[i] + t * d[i];
        g[i] = c.q[i] - (origin[i] + k * f[i]);
    }
    const double gAlong = detail::dot3(g, f);
    for (int i = 0; i < 3; ++i) c.gameShift[i] = g[i] - gAlong * f[i];
    const double tu = f139::clampTrimCm(trimUpCm), tr = f139::clampTrimCm(trimRightCm);
    double raw[3], s[3];
    for (int i = 0; i < 3; ++i) raw[i] = c.gameShift[i] + 0.01 * (tu * u[i] + tr * r[i]);
    const double rAlong = detail::dot3(raw, f);
    for (int i = 0; i < 3; ++i) s[i] = raw[i] - rAlong * f[i];
    const double l = std::sqrt(detail::dot3(s, s));
    if (!detail::finite3(s) || !std::isfinite(l) || l > f139::kBoundM + 1e-9) { c.why = kBound; return c; }
    for (int i = 0; i < 3; ++i) c.shift[i] = s[i];
    c.shiftM = l;
    c.gameUpCm = 100.0 * detail::dot3(c.gameShift, u);
    c.gameRightCm = 100.0 * detail::dot3(c.gameShift, r);
    c.lineDeg = std::acos(std::fmax(-1.0, std::fmin(1.0, along / len))) * 180.0 / 3.14159265358979323846;
    c.game = true;
    c.why = kGame;
    return c;
}
// The distance between two shifts (metres): the frame-to-frame step.
inline double stepM(const double a[3], const double b[3]) noexcept {
    const double d[3] = {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
    return std::sqrt(detail::dot3(d, d));
}

// ---- 2. the coat's clip modifiers ----------------------------------------------------------------------------------------------------------------------------------
constexpr uint32_t kPartCount = 54;   // avatar_hooks_core.h kPartCount (checked where both are included)
// The parts the unclip A/B adds: every part but the head parts and those F19's unclip list already names (its bits in listMask: the torso, arms, hands, shoulders, lower-arm gear,
// and the comparison parts the list names but leaves clipped: legs, feet, the first-person skeleton). Bit i = part index i.
inline uint64_t extraUnclipMask(uint64_t listMask, uint64_t headMask) noexcept {
    const uint64_t all = (1ull << kPartCount) - 1ull;
    return all & ~listMask & ~headMask;
}
// EDVR's armed state as the measure names it: 0 unarmed, 1 a weapon ($wpn_ item) in hand, 2 a tool.
enum Armed : uint8_t { kUnarmed = 0, kWeapon = 1, kTool = 2 };
inline uint8_t armedState(bool armed, const char* heldName) noexcept {
    if (!armed) return kUnarmed;
    return f139::isWeapon(heldName) ? kWeapon : kTool;
}
inline const char* armedText(unsigned a) noexcept {
    return a == kUnarmed ? "unarmed" : a == kWeapon ? "weapon" : a == kTool ? "tool" : "?";
}
// When the measure reads: at the first call, every 5 s, and when the armed state differs from the last read's. 0 not now, 1 the 5 s clock, 2 the armed state changed (or the
// first read). The clock's reads stop after capClock of them a session (the armed-state reads do not).
enum ReadWhy : int { kReadNo = 0, kReadClock = 1, kReadState = 2 };
inline int readWhy(int lastArmed, uint8_t armed, uint64_t nowMs, uint64_t nextMs, uint32_t clockReads, uint32_t capClock) noexcept {
    if (lastArmed < 0 || lastArmed != static_cast<int>(armed)) return kReadState;
    if (nowMs >= nextMs && clockReads < capClock) return kReadClock;
    return kReadNo;
}
struct ClipRow {
    uint8_t idx = 0;
    uint8_t inst = 0;               // bit k: instance k exists
    bool modSet = false;            // the modifier pointer at part entry + 0x610 is plausible
    bool readOk = false;            // ...and its bytes were read
    uint8_t enable = 0, dirty = 0;
    float amount = 0.0f;
    uint64_t masks[4] = {};
};
struct ClipSnap {
    uint64_t amc = 0;
    uint8_t armed = 0;              // Armed
    uint8_t why = kReadClock;       // ReadWhy
    bool flagsOk = false;
    uint8_t flag321 = 0, flag322 = 0;
    uint32_t n = 0;                 // rows used: the parts with an instance
    ClipRow row[kPartCount];
};
static_assert(std::is_trivially_copyable<ClipSnap>::value, "the clip snapshot is copied through a ring");

}  // namespace f143
}  // namespace nofoot
}  // namespace edvr
