// F143 (native on-foot VR): the pure parts of two changes. Header only, no game or D3D11 state, so native_frame_test runs the very functions the frame thread, the avatar
// hooks and the ADS sight lock use. Journal: the arc doc, 2026-10-27, "the F142 flight" and F143.
//
// 1. The ADS eye on the game's own sight line. F141's hook reads, on the game thread, the eye matrix E (position in row 3) and the sight point S the game's ADS bend aims the
//    shots through; the F142 flight showed the bend takes effect in VR (the shots go along the line E -> S) and that this line, taken at EDVR's eye depth, matches the pilot's
//    hand-trimmed totals within 0.32 cm on nine weapons. So the sight lock's shift (F139's table) is taken from that line, relative to P14w in the FP root's axes (F144 keeps a
//    settled line per (item, scope): native_on_foot_f144.h). lineShift takes Q, the point on the line at the lock's eye depth k along the root's forward from the origin, and
//    shifts the eye from (origin + k f) to Q, less the part along f, under F139's 8 cm bound. A line it cannot use falls back to F139's table, and the reason is counted. F146
//    removed the switch ("ADS sight from the game") and the two live trims: the line is the only source, and the table is its fallback.
//
// 2. The coat's clip modifiers. The fade hook's step reads, for every part of the local third-person avatar with an instance, the part's vertex clip modifier ("pVertexClipAmount",
//    part entry + 0x610: amount +8, enable +0xC, dirty +0xD) and view masks, every 5 s and when EDVR's armed state changes (the measure). F19's unclip (enable 0, dirty 1) also
//    covers every non-head part its list does not (the full outfit; the switch for it was removed in F146, so it is always on).
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

// Why a frame's shift came from the table and not the game's line (kGame: it came from the line). F144 replaced F143's live pair (no pair, stale) with a settled line kept per
// (item, scope) for the session (native_on_foot_f144.h): "no settled line yet" is the reason a weapon's first presses have no line. (F146: the "key off" reason went with the key.)
enum Why : uint8_t { kGame = 0, kNoLine, kOtherItem, kNoItem, kDegenerate, kBound, kWhyCount };
inline const char* whyText(unsigned w) noexcept {
    static const char* const kText[kWhyCount] = {"the game's line", "no settled line yet", "another item or scope (or not a held weapon)", "item not read",
                                                 "degenerate line (under 1 cm of depth)", "past the 8 cm bound"};
    return w < kWhyCount ? kText[w] : "?";
}
struct Choice {
    uint8_t why = kNoLine;       // kGame when the game's line is the source
    bool game = false;
    double shift[3] = {};        // world, metres: to be added to the origin (the game's shift, perpendicular to f, within the bound)
    double gameShift[3] = {};    // the game's shift (world, perpendicular to f)
    double gameUpCm = 0.0, gameRightCm = 0.0;   // the game's alone along the root's up and right
    double shiftM = 0.0;         // |shift|
    double lineDeg = 0.0;        // the angle of S - E from the root's forward
    double q[3] = {};            // Q in the world
};
// The shift of one line (E and S in the root's axes, metres from P14w) for one frame of the lock. p14w, up, right and fwd are this frame's P14 and FP root (axes of any
// length: unitised); origin is the lock's origin before any shift (P14w, or F79's measured origin) and k the lock's eye depth along fwd.
// kDegenerate: an axis with no length, a value that is not a number, or a line under 1 cm deep; kBound: past F139's 8 cm.
inline Choice lineShift(const double eRoot[3], const double sRoot[3], const double p14w[3], const double up[3], const double right[3], const double fwd[3], const double origin[3],
                        double k) noexcept {
    Choice c;
    double r[3], u[3], f[3];
    if (!detail::unit3(right, r) || !detail::unit3(up, u) || !detail::unit3(fwd, f) || !std::isfinite(k) || !detail::finite3(p14w) || !detail::finite3(origin) ||
        !detail::finite3(eRoot) || !detail::finite3(sRoot)) {
        c.why = kDegenerate;
        return c;
    }
    double ew[3], sw[3], d[3];
    for (int i = 0; i < 3; ++i) {
        ew[i] = p14w[i] + eRoot[0] * r[i] + eRoot[1] * u[i] + eRoot[2] * f[i];
        sw[i] = p14w[i] + sRoot[0] * r[i] + sRoot[1] * u[i] + sRoot[2] * f[i];
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
    const double l = std::sqrt(detail::dot3(c.gameShift, c.gameShift));
    if (!detail::finite3(c.gameShift) || !std::isfinite(l) || l > f139::kBoundM + 1e-9) { c.why = kBound; return c; }
    for (int i = 0; i < 3; ++i) c.shift[i] = c.gameShift[i];
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
constexpr uint32_t kPartCount = 54;   // avatar_hooks_core.h kPartCount (a static_assert in native_on_foot_hooks.cpp holds the two together)
static_assert(kPartCount < 64, "the part masks are 64-bit words");
// EDVR's armed state as the measure names it: 0 unarmed, 1 a weapon ($wpn_ item) in hand, 2 a tool.
enum Armed : uint8_t { kUnarmed = 0, kWeapon = 1, kTool = 2 };
inline uint8_t armedState(bool armed, const char* heldName) noexcept {
    if (!armed) return kUnarmed;
    return f139::isWeapon(heldName) ? kWeapon : kTool;
}
inline const char* armedText(unsigned a) noexcept {
    return a == kUnarmed ? "unarmed" : a == kWeapon ? "weapon" : a == kTool ? "tool" : "?";
}
// When the measure reads (F147): at the first call, when the outfit changed (the F142 census queued a change), and when EDVR's armed state changes to a state not yet read in this
// outfit. Not on a clock, and not at every draw and holster: a snapshot is about 3 KB of log text. At most capReads a session. 0 not now; 1 the 5 s clock (the F143 to F146
// builds' schedule, which logs from before still carry); 2 the armed state first changed to this state in this outfit; 3 the first call; 4 the outfit changed.
enum ReadWhy : int { kReadNo = 0, kReadClock = 1, kReadState = 2, kReadFirst = 3, kReadOutfit = 4 };
inline int readWhy(int lastArmed, uint8_t armed, unsigned seenMask, bool outfitChanged, uint32_t reads, uint32_t capReads) noexcept {
    if (reads >= capReads) return kReadNo;
    if (lastArmed < 0) return kReadFirst;
    if (outfitChanged) return kReadOutfit;
    if (lastArmed != static_cast<int>(armed) && armed < 8 && !((seenMask >> armed) & 1u)) return kReadState;
    return kReadNo;
}
inline const char* readWhyText(unsigned w) noexcept {
    return w == kReadClock ? "on the 5 s clock" : w == kReadState ? "when the armed state first changed to this state in this outfit" : w == kReadFirst ? "at the first call"
         : w == kReadOutfit ? "when the outfit changed" : "?";
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
    uint8_t why = kReadFirst;       // ReadWhy
    bool flagsOk = false;
    uint8_t flag321 = 0, flag322 = 0;
    uint32_t n = 0;                 // rows used: the parts with an instance
    ClipRow row[kPartCount];
};
static_assert(std::is_trivially_copyable<ClipSnap>::value, "the clip snapshot is copied through a ring");

}  // namespace f143
}  // namespace nofoot
}  // namespace edvr
