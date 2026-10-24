// F87 (native on-foot VR): the pure parts of the standing-height hold (h), the stream windows' history and escalation (c), the scene rows' fallback (f), the
// prompt probes' pins, roles and box geometry (p) and the hand residual (a). Design: reviews\native-onfoot-f87-review-2026-10-24.md; journal: the arc doc, F87.
//
// Header only, no game or D3D11 state, so native_frame_test runs the very functions the hooks use. Conventions as native_on_foot_f86.h: 4x4 float matrices are
// row-vector affines (world = p R + t, the rows R[0..2] the axes, the translation at 12..14).
#pragma once

#include "native_on_foot_f86.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>

namespace edvr {
namespace nofoot {
namespace f87 {

// ---- a lock-free ring by game frame (F85a's, shared with the rig) --------------------------------------------------------------------------------
// post() never waits (a writer in the slot skips); get() answers only for the frame asked, from a slot no writer touched while it was read.
template <class T, uint32_t N>
struct Ring {
    struct Slot {
        std::atomic<uint32_t> seq{0};
        uint32_t frame = ~0u;
        T v{};
    };
    Slot s[N];
    void post(uint32_t frame, const T& v) noexcept {
        Slot& d = s[frame % N];
        uint32_t q = d.seq.load(std::memory_order_relaxed);
        if ((q & 1u) || !d.seq.compare_exchange_strong(q, q + 1, std::memory_order_acq_rel)) return;
        d.frame = frame;
        d.v = v;
        d.seq.store(q + 2, std::memory_order_release);
    }
    bool get(uint32_t frame, T* out) const noexcept {
        const Slot& d = s[frame % N];
        for (int tries = 0; tries < 2; ++tries) {
            const uint32_t s1 = d.seq.load(std::memory_order_acquire);
            if (!s1 || (s1 & 1u)) continue;
            if (d.frame != frame) return false;
            *out = d.v;
            std::atomic_thread_fence(std::memory_order_acquire);
            if (d.seq.load(std::memory_order_relaxed) == s1) return true;
        }
        return false;
    }
};

// ---- H: the standing-height hold -----------------------------------------------------------------------------------------------------------------
// Elite's camera is the character's point of view plus the headset's pose in the seated space, so a head 8-10 cm under the seat (F86 flight, R1) is a view 8-10 cm
// under the character's eye and the right eye sits 8-10 cm lower over the weapon's root. The hold moves the view up by the SUSTAINED part of that deviation, and
// leaves the quick part (nods, bobs, breathing) as it is. dev is measured about the neck pivot:
//     dev = (the head's height in the seated space, the eye alignment taken out) - (rest eye height over the neck at this pitch - at level) - 0
// (0 is the seat: the seated space's origin is where the head was at the recentre). The sustained part is the output of a critically damped low pass that settles to
// 95% in `settleS` seconds, its rate capped at kHoldSlewMps; with settleS 0 the low pass is dev itself (a hard lock about the neck pivot).
//     camera raise = - sustained,   published as the eye alignment's y = - raise (the runtime moves the seated origin: its +y lowers the view, as weaponViewOffsetSeated)
//     transient = dev + raise       what the view still carries over the character's eye height
// The body never follows the posture: the pin takes the camera less the transient (pinEyeYAdjust).
constexpr double kSettleDefaultS = 0.1, kSettleMaxS = 10.0;   // F89: the pilot's value (was 3; the rate cap still applies)
constexpr double kHoldSlewMps = 0.04, kHoldRampS = 0.5, kHoldStepM = 0.001, kHoldMaxM = 0.5;
constexpr double kHoldCritical95 = 4.743898;   // omega * t for 1 - (1 + wt) exp(-wt) = 0.95
constexpr double kPinCalStillM = 0.01, kPinCalStillS = 1.0;
inline double settleClamp(double s) noexcept { return !(s == s) ? kSettleDefaultS : s < 0.0 ? 0.0 : s > kSettleMaxS ? kSettleMaxS : s; }
struct HoldIn {
    bool scope = false;      // the body pin's scope (native on foot, full 3P body)
    bool devValid = false;   // the render head and the LOCAL pitch were read
    bool frozen = false;     // ADS: the ADS locks own the eye
    double devM = 0.0, dt = 0.0, settleS = kSettleDefaultS;
};
struct HoldOut {
    double raiseM = 0.0;        // the view's raise, quantised to a millimetre, ramped; + up
    double eyeY = 0.0;          // the eye alignment's y term (= -raiseM)
    double sustainedM = 0.0;    // the low pass of dev (what the hold is absorbing)
    double transientM = 0.0;    // dev + raise: what the view carries over the character's eye height
    bool transientValid = false;
    double ramp = 0.0;
    bool slewed = false;        // the rate cap was in force on this step
};
struct HeightHold {
    bool have = false;
    double x = 0.0, v = 0.0, ramp = 0.0;
    void reset() noexcept { *this = HeightHold{}; }
    HoldOut step(const HoldIn& in) noexcept {
        HoldOut o;
        double dt = in.dt;
        dt = !(dt > 0.0) ? 0.0 : dt > 0.25 ? 0.25 : dt;
        ramp = in.scope ? std::min(1.0, ramp + dt / kHoldRampS) : std::max(0.0, ramp - dt / kHoldRampS);
        if (!in.scope && ramp <= 0.0) {
            have = false;
            x = v = 0.0;
        }
        if (in.scope && in.devValid && std::isfinite(in.devM)) {
            const double dev = std::max(-kHoldMaxM, std::min(kHoldMaxM, in.devM));
            const double settle = settleClamp(in.settleS);
            if (!have) {
                x = dev;
                v = 0.0;
                have = true;
            } else if (!in.frozen && dt > 0.0) {
                if (settle < 1e-3) {
                    x = dev;
                    v = 0.0;
                } else {
                    const double w = kHoldCritical95 / settle, e = x - dev, c = v + w * e, ex = std::exp(-w * dt);
                    double xn = dev + (e + c * dt) * ex, vn = (v - w * c * dt) * ex;
                    const double maxStep = kHoldSlewMps * dt, dx = xn - x;
                    if (std::fabs(dx) > maxStep) {
                        xn = x + (dx < 0.0 ? -maxStep : maxStep);
                        o.slewed = true;
                    }
                    vn = std::max(-kHoldSlewMps, std::min(kHoldSlewMps, vn));
                    x = xn;
                    v = vn;
                }
            }
        }
        o.ramp = ramp;
        o.sustainedM = have ? x : 0.0;
        double raise = ramp * (-o.sustainedM);
        raise = std::max(-kHoldMaxM, std::min(kHoldMaxM, raise));
        raise = std::round(raise / kHoldStepM) * kHoldStepM;
        o.raiseM = raise == 0.0 ? 0.0 : raise;
        o.eyeY = raise == 0.0 ? 0.0 : -raise;
        o.transientValid = in.scope && in.devValid && have && std::isfinite(in.devM);
        o.transientM = o.transientValid ? in.devM + o.raiseM : 0.0;
        return o;
    }
};
// dev from the pieces: the render head's height in the seated space (the eye alignment is part of that space: the head reads lower by what it moved the origin up),
// the eye alignment published for the frame, and the neck pivot's drop at this pitch.
inline double devOf(double renderY, double eyeAlignY, double neckDropM) noexcept { return renderY + eyeAlignY - neckDropM; }
// The eye alignment's y: the view run, the ADS locks, the weapon view offset and the hold (the hold is not an ADS lock: it freezes in ADS, it does not stop).
inline double eyeAlignY(double alignApplied, double adsLock, double adsSight, double weaponSeated, double holdEyeY) noexcept {
    return alignApplied + adsLock + adsSight + weaponSeated + holdEyeY;
}
// The body pin's eye: the camera in the model less the transient along the model's up (the body keeps the character's height, the pin and its standing
// calibration see no posture).
inline double pinEyeYAdjust(double eyeModelY, double transientM, bool transientValid) noexcept { return transientValid ? eyeModelY - transientM : eyeModelY; }
// The pin's standing calibration waits until the view has been still: |transient| under kPinCalStillM for kPinCalStillS.
struct StillGate {
    double stillS = 0.0;
    bool ok = false;
    void step(bool valid, double transientM, double dt) noexcept {
        if (!valid || std::fabs(transientM) >= kPinCalStillM) stillS = 0.0;
        else stillS += dt > 0.0 && dt < 0.25 ? dt : 0.0;
        ok = valid && stillS >= kPinCalStillS;
    }
};
// The hold's second (frame thread): the line's figures.
struct HoldSecond {
    uint32_t n = 0, jumps = 0;
    double devMin = 1e9, devMax = -1e9, maxTransient = 0.0, maxStep = 0.0, lastY = 0.0;
    bool haveY = false;
    void add(bool devValid, double devM, double transientM, double localY, bool localYValid) noexcept {
        if (devValid) {
            ++n;
            devMin = std::min(devMin, devM);
            devMax = std::max(devMax, devM);
            maxTransient = std::max(maxTransient, std::fabs(transientM));
        }
        if (localYValid) {
            if (haveY) {
                const double d = std::fabs(localY - lastY);
                maxStep = std::max(maxStep, d);
                if (d > 0.02) ++jumps;
            }
            lastY = localY;
            haveY = true;
        } else {
            haveY = false;
        }
    }
    void reset() noexcept {
        n = jumps = 0;
        devMin = 1e9;
        devMax = -1e9;
        maxTransient = maxStep = 0.0;
    }
};
// The eye over the weapon's root, in the character's axes (the right eye less F_root; right, up, forward), the second's up range.
struct EyeRootSecond {
    uint32_t n = 0;
    double last[3] = {}, upMin = 1e9, upMax = -1e9;
    void add(const double eye[3], const double froot[3], const double rows[3][3]) noexcept {
        const double d[3] = {eye[0] - froot[0], eye[1] - froot[1], eye[2] - froot[2]};
        for (int a = 0; a < 3; ++a) last[a] = d[0] * rows[a][0] + d[1] * rows[a][1] + d[2] * rows[a][2];
        upMin = std::min(upMin, last[1]);
        upMax = std::max(upMax, last[1]);
        ++n;
    }
    void reset() noexcept {
        n = 0;
        upMin = 1e9;
        upMax = -1e9;
    }
};

// ---- A: the previous copy of the body records ----------------------------------------------------------------------------------------------------
// The 2026-10-24 flight (F86 dump 003): the body records' previous position (+0x124) is, at the submit's start, bit-equal to the record's own position of THIS frame
// (the game writes both from the same value), so moving the position alone made position - previous the carry itself (5-30 cm in a jump): a motion vector for a
// body that did not move. The previous copy moves by the same carry whenever it equals the position.
inline bool prevWithPos(const float prev[3], const float pos[3]) noexcept { return std::memcmp(prev, pos, 12) == 0; }
// The 12 entries of a row-vector affine (the 3x3 and the translation). The padding words (3, 7, 11, 15) carry NaN in this build, so they are not part of a matrix's validity.
inline bool affineFinite(const float m[16]) noexcept {
    for (int i = 0; i < 16; ++i) {
        if ((i & 3) == 3) continue;
        if (!std::isfinite(m[i])) return false;
    }
    return true;
}
// The hand's residual after the carry: frame i's target (3P model, from its own Animate) against the wrist frame i-1's Animate wrote, turned by M3P_i, less the carry written.
inline double handResidualCm(const double target[3], const double wristPrev[3], const float m3p[16], const double delta[3]) noexcept {
    const double d[3] = {target[0] - wristPrev[0], target[1] - wristPrev[1], target[2] - wristPrev[2]};
    double w[3];
    for (int j = 0; j < 3; ++j) w[j] = d[0] * static_cast<double>(m3p[j]) + d[1] * static_cast<double>(m3p[4 + j]) + d[2] * static_cast<double>(m3p[8 + j]) - delta[j];
    return f85::len3(w) * 100.0;
}
// FP frame - submit frame at a submit, as the bucket the second counts it in: -1, 0, +1, other.
inline int fpFrameBucket(int32_t diff) noexcept { return diff == -1 ? 0 : diff == 0 ? 1 : diff == 1 ? 2 : 3; }
struct HandSecond {
    uint64_t n[2] = {}, binN[f85::kStepBins][2] = {};
    double sum[2] = {}, mx[2] = {}, binSum[f85::kStepBins][2] = {};
    void add(int side, double cm, int bin) noexcept {
        if (side < 0 || side > 1 || !(cm >= 0.0) || !std::isfinite(cm)) return;
        ++n[side];
        sum[side] += cm;
        mx[side] = std::max(mx[side], cm);
        if (bin >= 0 && bin < f85::kStepBins) {
            ++binN[bin][side];
            binSum[bin][side] += cm;
        }
    }
    void reset() noexcept { *this = HandSecond{}; }
};

// ---- C: the stream windows' history and escalation -----------------------------------------------------------------------------------------------
// The CPU identification names a draw from a copy of the instance stream made at the Unmap before it. F86 (the entries of the last two frames) left the 2026-10-24 flicker
// where it was: an extra pass moves eye 1's entries 6-14k between layouts, so entries drawn up to 64 frames ago are drawn again. Per eye the copy is
//     F85's window: 8192 entries about the newest entry (always), plus
//     the history: a window of 4096 entries about each cluster of the entries drawn in the last kKeepFrames frames, at most kHistWindows,
//     and the whole stream for kEscalateFrames frames after a draw that no window held.
constexpr uint32_t kWinEntries = 4096, kF85Entries = 8192, kHistWindows = 6, kWinMax = 1 + kHistWindows, kWinGap = 2048;
constexpr uint32_t kKeepFrames = 64, kEscalateFrames = 8, kHistMax = 16;
constexpr uint32_t kWinCapEntries = kF85Entries + kHistWindows * kWinEntries;   // the copy buffer of one eye
struct ClusterHist {
    struct Iv {
        bool used = false;
        uint32_t lo = 0, hi = 0, last = 0;
    };
    Iv iv[kHistMax];
    void reset() noexcept { *this = ClusterHist{}; }
    // One entry drawn at `frame`: joins the interval it touches (a gap of at most kWinGap), else opens one (evicting the least recently seen when all are used).
    void add(uint32_t entry, uint32_t frame) noexcept {
        int hit = -1;
        for (uint32_t i = 0; i < kHistMax; ++i) {
            Iv& v = iv[i];
            if (!v.used) continue;
            if (entry + kWinGap >= v.lo && entry <= v.hi + kWinGap) {
                if (hit < 0) {
                    hit = static_cast<int>(i);
                    v.lo = std::min(v.lo, entry);
                    v.hi = std::max(v.hi, entry);
                    v.last = frame;
                } else {   // the entry bridges two intervals: they become one
                    Iv& h = iv[hit];
                    h.lo = std::min(h.lo, v.lo);
                    h.hi = std::max(h.hi, v.hi);
                    h.last = std::max(h.last, v.last);
                    v = Iv{};
                }
            }
        }
        if (hit >= 0) return;
        int slot = -1;
        for (uint32_t i = 0; i < kHistMax && slot < 0; ++i)
            if (!iv[i].used) slot = static_cast<int>(i);
        if (slot < 0) {
            uint32_t oldest = 0;
            for (uint32_t i = 1; i < kHistMax; ++i)
                if (frame - iv[i].last > frame - iv[oldest].last) oldest = i;
            slot = static_cast<int>(oldest);
        }
        iv[slot] = Iv{true, entry, entry, frame};
    }
    void expire(uint32_t frame) noexcept {
        for (Iv& v : iv)
            if (v.used && frame - v.last > kKeepFrames) v = Iv{};
    }
    uint32_t count() const noexcept {
        uint32_t c = 0;
        for (const Iv& v : iv) c += v.used ? 1u : 0u;
        return c;
    }
};
struct WinPlan {
    uint32_t lo[kWinMax] = {}, n[kWinMax] = {}, off[kWinMax] = {};   // off: where the window's copy starts in the eye's buffer (entries)
    uint32_t windows = 0, f85 = 0, history = 0, clusters = 0, dropped = 0, used = 0;
    bool full = false;   // the whole stream is held (escalation)
    bool holds(uint32_t entry) const noexcept { return where(entry) >= 0; }
    int where(uint32_t entry) const noexcept {
        for (uint32_t w = 0; w < windows; ++w)
            if (n[w] && entry >= lo[w] && entry - lo[w] < n[w]) return static_cast<int>(w);
        return -1;
    }
    // Entries between `entry` and the nearest window (0 inside one); ~0u with no window.
    uint32_t distance(uint32_t entry) const noexcept {
        uint32_t best = ~0u;
        for (uint32_t w = 0; w < windows; ++w) {
            if (!n[w]) continue;
            const uint32_t end = lo[w] + n[w];
            const uint32_t d = entry < lo[w] ? lo[w] - entry : entry >= end ? entry - end + 1u : 0u;
            best = std::min(best, d);
        }
        return best;
    }
};
// A window of `want` entries (even) about `centre`, kept inside the stream; returns its length (even).
inline uint32_t winPlace(uint32_t centre, uint32_t total, uint32_t want, uint32_t* lo) noexcept {
    want &= ~1u;
    uint32_t a = centre > want / 2 ? centre - want / 2 : 0u;
    a &= ~1u;
    if (total < want) a = 0;
    else if (a > total - want) a = (total - want) & ~1u;
    const uint32_t left = total > a ? total - a : 0u;
    *lo = a;
    return (left < want ? left : want) & ~1u;
}
inline void planAdd(WinPlan* p, uint32_t lo, uint32_t n) noexcept {
    p->lo[p->windows] = lo;
    p->n[p->windows] = n;
    p->off[p->windows] = p->used;
    p->used += n;
    ++p->windows;
}
// F85's window first (when the eye has a newest entry), then the history's clusters, the most recently seen first.
inline void planWindows(const ClusterHist& h, bool haveCentre, uint32_t centre, uint32_t total, WinPlan* out) noexcept {
    *out = WinPlan{};
    if (!total) return;
    if (haveCentre) {
        uint32_t lo = 0;
        const uint32_t n = winPlace(centre, total, kF85Entries, &lo);
        if (n) {
            planAdd(out, lo, n);
            out->f85 = 1;
        }
    }
    bool taken[kHistMax] = {};
    for (uint32_t pick = 0; pick < kHistMax; ++pick) {
        int best = -1;
        for (uint32_t i = 0; i < kHistMax; ++i) {
            if (!h.iv[i].used || taken[i]) continue;
            if (best < 0 || h.iv[i].last > h.iv[best].last) best = static_cast<int>(i);
        }
        if (best < 0) break;
        taken[best] = true;
        ++out->clusters;
        const uint32_t a = h.iv[best].lo, b = std::min(h.iv[best].hi, total - 1u);
        bool inside = false;
        for (uint32_t w = 0; w < out->windows && !inside; ++w) inside = out->n[w] && a >= out->lo[w] && b - out->lo[w] < out->n[w];
        if (inside) continue;
        if (b - a + 1u + 2u <= kWinEntries) {
            if (out->windows >= kWinMax) {
                ++out->dropped;
                continue;
            }
            uint32_t lo = 0;
            const uint32_t n = winPlace(a + (b - a) / 2u, total, kWinEntries, &lo);
            planAdd(out, lo, n);
            ++out->history;
        } else {   // a cluster wider than a window: windows end to end from its first entry
            uint32_t at = a & ~1u;
            while (at <= b) {
                if (out->windows >= kWinMax) {
                    ++out->dropped;
                    break;
                }
                planAdd(out, at, std::min(kWinEntries, total > at ? total - at : 0u) & ~1u);
                ++out->history;
                at += kWinEntries;
            }
        }
    }
}
// The escalation: a draw that no window held starts kEscalateFrames frames of whole-stream copies.
struct Escalation {
    uint32_t until = 0;
    bool on = false;
    void outside(uint32_t frame) noexcept {
        until = frame + kEscalateFrames;
        on = true;
    }
    bool active(uint32_t frame) noexcept {
        if (on && static_cast<int32_t>(frame - until) > 0) on = false;
        return on;
    }
};

// ---- F: the scene rows' fallback -----------------------------------------------------------------------------------------------------------------
// An eye-frame whose world draws include no scene-geometry vertex shader (bare ground, the head down) has no scene rows, and every consumer of them refuses the frame.
// The depth prepass (VS F516BF0201303B87, PS B40B0462256E31C2: the main content of 12,858 of 12,858 scene frames of the F86 flight and present in all 1,089 frames
// without one) carries the same constants. Its rows stand in only for such a frame, and a frame that has both compares them: one difference over kFbTolerance
// latches the fallback off.
constexpr uint64_t kFbVs = 0xF516BF0201303B87ull, kFbPs = 0xB40B0462256E31C2ull;
constexpr double kFbTolerance = 1e-5;
inline bool fallbackPair(uint64_t vs, uint64_t ps) noexcept { return vs == kFbVs && ps == kFbPs; }
// `a` and `b`: b1 rows 270..273 (16 floats) and the origin row 275 (4); the largest absolute difference (NaN counts as infinite).
inline double rowsMaxDiff(const float a[20], const float b[20]) noexcept {
    double m = 0.0;
    for (int i = 0; i < 20; ++i) {
        const double d = std::fabs(static_cast<double>(a[i]) - static_cast<double>(b[i]));
        if (!(d == d)) return 1e30;
        m = std::max(m, d);
    }
    return m;
}
struct FbGuard {
    bool latchedOff = false;
    uint64_t both = 0, mismatches = 0;
    double maxDiff = 0.0;
    // The comparison of an eye-frame that has both; true while the fallback may still be used.
    bool compare(double diff) noexcept {
        ++both;
        maxDiff = std::max(maxDiff, diff);
        if (diff > kFbTolerance) {
            ++mismatches;
            latchedOff = true;
        }
        return !latchedOff;
    }
};

// ---- P: the prompt probes (read only) ------------------------------------------------------------------------------------------------------------
// The interaction prompt's point is the interactable model's `Root!Ui_interact` node, else the centre of the model's bounding box (analysis\onfoot_prompt_anchor_334925.txt).
// Build 334925, RVAs. The marker interface's vtable (comp+0x158) is at 0x520B2E8; the probes swap three of its slots and three call stubs.
constexpr uint32_t kSlotUpdateIndicator = 0x520B330, kSlotSetMarker = 0x520B338, kSlotUpdateMarker = 0x520B348;   // +0x48, +0x50, +0x60
constexpr uint32_t kFnUpdateIndicator = 0x1AB15D0, kFnSetMarker = 0x1AB1560, kFnUpdateMarker = 0x1AA53D0;
constexpr uint32_t kGetterFocus = 0x191F140, kGetterNearby = 0x1A43230;
constexpr uint32_t kTypeIdRva = 0x5F05D38, kNodeNameRva = 0x51F3500, kHalfRva = 0x4E328B0;
struct Pin {
    const char* name;
    uint32_t rva;
    uint8_t len;
    uint8_t bytes[16];
};
constexpr Pin kPins[] = {
    {"UpdateIndicator entry", kFnUpdateIndicator, 15, {0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57, 0x48, 0x83, 0xEC, 0x20}},
    {"SetMarker entry", kFnSetMarker, 13, {0x4C, 0x8D, 0x91, 0xE0, 0x01, 0x00, 0x00, 0x49, 0x8B, 0x02, 0x48, 0x85, 0xC0}},
    {"UpdateMarker entry", kFnUpdateMarker, 15, {0x48, 0x89, 0x5C, 0x24, 0x18, 0x55, 0x56, 0x57, 0x48, 0x81, 0xEC, 0xA0, 0x00, 0x00, 0x00}},
    {"focus SetMarker call", 0x1AA9DE1, 3, {0xFF, 0x50, 0x50}},
    {"nearby SetMarker call", 0x1A9E27A, 3, {0xFF, 0x50, 0x50}},
    {"target SetMarker call", 0x1AA4D63, 3, {0xFF, 0x50, 0x50}},
    {"focus key load", 0x1AA9DC9, 6, {0x8B, 0x93, 0xD8, 0x02, 0x00, 0x00}},
    {"focus UpdateIndicator call", 0x1AA9DA7, 3, {0x41, 0xFF, 0xD0}},
    {"focus getter call", 0x1AA9A8F, 5, {0xE8, 0xAC, 0x56, 0xE7, 0xFF}},
    {"nearby getter call", 0x1A9E13D, 5, {0xE8, 0xEE, 0x50, 0xFA, 0xFF}},
    {"target getter call", 0x1AA4AD8, 5, {0xE8, 0x53, 0xE7, 0xF9, 0xFF}},
    {"model type id load", 0x191F190, 6, {0x8B, 0x15, 0xA2, 0x6B, 0x5E, 0x04}},
    {"FindNode call", 0x191F1C7, 7, {0x4C, 0x8B, 0x81, 0x28, 0x01, 0x00, 0x00}},
    {"bounds call", 0x191F382, 6, {0xFF, 0x90, 0x10, 0x01, 0x00, 0x00}},
    {"world matrix call", 0x191F3A3, 6, {0xFF, 0x90, 0xA8, 0x00, 0x00, 0x00}},
    // F92c: the indicator owner 0x1AA9830 and its three callers (the type each passes)
    {"indicator owner entry", 0x1AA9830, 7, {0x48, 0x8B, 0xC4, 0x48, 0x83, 0xEC, 0x68}},
    {"indicator UpdateIndicator call", 0x1AA98EA, 3, {0x41, 0xFF, 0xD0}},
    {"indicator SetMarker call", 0x1AA9928, 3, {0xFF, 0x50, 0x50}},
    {"focus indicator call", 0x1AA9778, 5, {0xE8, 0xB3, 0x00, 0x00, 0x00}},
    {"focus indicator type", 0x1AA976B, 5, {0xBA, 0xE8, 0x03, 0x00, 0x00}},
    {"interactable indicator call A", 0x1AAE644, 5, {0xE8, 0xE7, 0xB1, 0xFF, 0xFF}},
    {"interactable indicator type A", 0x1AAE635, 5, {0xBA, 0xE9, 0x03, 0x00, 0x00}},
    {"interactable indicator call B", 0x1AB67CB, 5, {0xE8, 0x60, 0x30, 0xFF, 0xFF}},
    {"interactable indicator type B", 0x1AB67BC, 5, {0xBA, 0xE9, 0x03, 0x00, 0x00}},
};
constexpr uint32_t kPinIndicatorEntry = 15;   // F92c: the indicator owner's prologue (its `sub rsp, 0x68` immediate is byte 6)
constexpr uint32_t kPinCount = sizeof(kPins) / sizeof(kPins[0]);
constexpr uint32_t kPinFocusGetter = 8, kPinNearbyGetter = 9, kPinTargetGetter = 10;
// The machine code of the two probe shapes (x64, the Microsoft calling convention), emitted byte for byte so the rig can run the very code the hooks install.
struct Emit {
    uint8_t* p;
    size_t i = 0;
    void b(std::initializer_list<uint8_t> v) {
        for (uint8_t x : v) p[i++] = x;
    }
    void q(uint64_t v) {
        std::memcpy(p + i, &v, 8);
        i += 8;
    }
};
// A getter stub, entered as the game's getter is (a `call` whose rel32 was swapped): push rcx, rdx, r8; sub rsp,0x30; pre(rcx, rdx, r8, site) with the registers as
// the game left them; the game's getter `real` with the same three registers; its result kept; post(rcx, rdx, r8, site); the result back in rax; ret. Entered at
// rsp = 8 mod 16, the three pushes and the 0x30 leave rsp 16-aligned at each call: 0x20 of shadow space, the result at +0x20, then r8, rdx, rcx at +0x30, +0x38, +0x40.
inline size_t emitGetterStub(uint8_t* at, uint64_t real, uint64_t site, uint64_t pre, uint64_t post) {
    Emit m{at};
    m.b({0x51, 0x52, 0x41, 0x50, 0x48, 0x83, 0xEC, 0x30});
    m.b({0x49, 0xB9});
    m.q(site);
    m.b({0x48, 0xB8});
    m.q(pre);
    m.b({0xFF, 0xD0});
    const std::initializer_list<uint8_t> restore = {0x48, 0x8B, 0x4C, 0x24, 0x40, 0x48, 0x8B, 0x54, 0x24, 0x38, 0x4C, 0x8B, 0x44, 0x24, 0x30};
    m.b(restore);
    m.b({0x48, 0xB8});
    m.q(real);
    m.b({0xFF, 0xD0});
    m.b({0x48, 0x89, 0x44, 0x24, 0x20});
    m.b(restore);
    m.b({0x49, 0xB9});
    m.q(site);
    m.b({0x48, 0xB8});
    m.q(post);
    m.b({0xFF, 0xD0});
    m.b({0x48, 0x8B, 0x44, 0x24, 0x20, 0x48, 0x83, 0xC4, 0x30, 0x41, 0x58, 0x5A, 0x59, 0xC3});
    return m.i;
}
// A vtable trampoline: rcx, rdx, r8, r9 and xmm0..3 saved; rec(slot, the return address, a pointer to the saved r9, r8, rdx, rcx, the caller's shadow space) called
// with 16-byte alignment; everything restored; then a jump to the game's function `orig`, so its stack, registers and return address are exactly the game's.
inline size_t emitTrampoline(uint8_t* at, uint64_t orig, uint64_t slot, uint64_t rec) {
    Emit m{at};
    m.b({0x51, 0x52, 0x41, 0x50, 0x41, 0x51, 0x48, 0x83, 0xEC, 0x68});
    m.b({0x0F, 0x11, 0x44, 0x24, 0x20, 0x0F, 0x11, 0x4C, 0x24, 0x30, 0x0F, 0x11, 0x54, 0x24, 0x40, 0x0F, 0x11, 0x5C, 0x24, 0x50});
    m.b({0x48, 0xB9});
    m.q(slot);
    m.b({0x48, 0x8B, 0x94, 0x24, 0x88, 0x00, 0x00, 0x00, 0x4C, 0x8D, 0x44, 0x24, 0x68, 0x4C, 0x8D, 0x8C, 0x24, 0x90, 0x00, 0x00, 0x00});
    m.b({0x48, 0xB8});
    m.q(rec);
    m.b({0xFF, 0xD0});
    m.b({0x0F, 0x10, 0x44, 0x24, 0x20, 0x0F, 0x10, 0x4C, 0x24, 0x30, 0x0F, 0x10, 0x54, 0x24, 0x40, 0x0F, 0x10, 0x5C, 0x24, 0x50});
    m.b({0x48, 0x83, 0xC4, 0x68, 0x41, 0x59, 0x41, 0x58, 0x5A, 0x59});
    m.b({0x48, 0xB8});
    m.q(orig);
    m.b({0xFF, 0xE0});
    return m.i;
}

// The target of an E8 call at `rva` whose 5 bytes are `b`.
inline uint32_t callTarget(uint32_t rva, const uint8_t b[5]) noexcept {
    int32_t rel = 0;
    std::memcpy(&rel, b + 1, 4);
    return static_cast<uint32_t>(static_cast<int64_t>(rva) + 5 + rel);
}
// Which SetMarker / UpdateMarker call a return address (RVA) belongs to.
enum Role : int { kRoleFocus = 0, kRoleFocusHidden, kRoleNearby, kRoleTarget, kRoleList, kRoleIndicator, kRoleOther, kRoleCount };
inline const char* roleText(int r) noexcept {
    static const char* const n[kRoleCount] = {"focus", "focus hidden", "nearby", "target", "list", "indicator", "other"};
    return n[r < 0 || r >= kRoleCount ? kRoleOther : r];
}
inline int roleOfReturn(uint32_t rva) noexcept {
    switch (rva) {
        case 0x1AA9DE4: return kRoleFocus;
        case 0x1AA9F67: return kRoleFocusHidden;
        case 0x1A9E27D: return kRoleNearby;
        case 0x1AA4D66: return kRoleTarget;
        case 0x1A9E74D: return kRoleList;
        case 0x1AA992B: return kRoleIndicator;   // F92c: the indicator owner's SetMarker (0x1AA9830)
        default: return kRoleOther;
    }
}
// ---- F92c: the label's owner ------------------------------------------------------------------------------------------------------------------------
// The F91 flight (log 092147): the focus prompt's producer 0x1AA99C0 took its hidden path every frame (SetMarker visible 0 returning to 0x1AA9F67, 83 a second),
// so no focus key was ever set although UpdateIndicator fired 45,410 times. The labels come from the indicator owner 0x1AA9830 (this = rcx; [this+0x108] the
// marker type, [this+0x10C] its key, [this+0xF8] the marker interface): UpdateIndicator (+0x48, at 0x1AA98EA) and then SetMarker (+0x50, at 0x1AA9928, returning
// to 0x1AA992B) with that key, the caller's point (r8), visible 1, offset 0, scale 1. It has three callers, each passing the type in edx: 0x1AA9778 (type 1000,
// the focused interactable) and 0x1AAE644 and 0x1AB67CB (type 1001, the other interactables). 0x1AA9830 saves nothing with a push and takes 0x68 of stack, so at
// its SetMarker call its own return address (into the caller) is 0x68 above the stack pointer: the trampoline's caller-shadow pointer + 0x68.
constexpr uint32_t kIndicatorOuterReturnOffset = 0x68;   // the immediate of the pinned `sub rsp, 0x68` (the rig checks the two agree)
constexpr uint32_t kIndicatorTypeFocus = 1000, kIndicatorTypeInteractable = 1001;
inline uint32_t indicatorTypeOfCaller(uint32_t rva) noexcept {
    switch (rva) {
        case 0x1AA977D: return kIndicatorTypeFocus;
        case 0x1AAE649:
        case 0x1AB67D0: return kIndicatorTypeInteractable;
        default: return 0u;
    }
}
// The focused interactable's key and where it came from (0 none, 1 the focus prompt 0x1AA99C0, 2 the focus indicator 0x1AA9778), packed so a reader sees both
// from one store. The focus prompt's hidden path clears only a key it set itself; a focus indicator key is cleared by the render thread after a second in which no
// focus indicator was posted.
enum FocusSrc : int { kFocusSrcNone = 0, kFocusSrcPrompt = 1, kFocusSrcIndicator = 2 };
enum FocusEvent : int { kFocusEvPrompt = 0, kFocusEvPromptHidden, kFocusEvIndicator, kFocusEvQuietSecond };
inline uint64_t focusPack(int32_t key, int src) noexcept { return static_cast<uint64_t>(static_cast<uint32_t>(key)) | (static_cast<uint64_t>(static_cast<uint32_t>(src)) << 32); }
inline int32_t focusKeyOf(uint64_t s) noexcept { return static_cast<int32_t>(static_cast<uint32_t>(s & 0xFFFFFFFFull)); }
inline int focusSrcOf(uint64_t s) noexcept { return static_cast<int>(s >> 32); }
inline uint64_t focusStep(uint64_t s, int ev, int32_t key) noexcept {
    switch (ev) {
        case kFocusEvPrompt: return focusPack(key, kFocusSrcPrompt);
        case kFocusEvPromptHidden: return focusSrcOf(s) == kFocusSrcPrompt ? focusPack(-1, kFocusSrcNone) : s;
        case kFocusEvIndicator: return focusPack(key, kFocusSrcIndicator);
        case kFocusEvQuietSecond: return focusSrcOf(s) == kFocusSrcIndicator ? focusPack(-1, kFocusSrcNone) : s;
        default: return s;
    }
}
inline const char* focusSrcText(int src) noexcept {
    return src == kFocusSrcPrompt ? "focus prompt" : src == kFocusSrcIndicator ? "focus indicator" : "none";
}
// The getter stubs' site ids and the calls of the pins' stub: the site of a getter's call, by its pin.
enum Site : int { kSiteFocus = 0, kSiteNearby, kSiteTarget, kSiteCount };
inline const char* siteText(int s) noexcept {
    static const char* const n[kSiteCount] = {"focus", "nearby", "target"};
    return n[s < 0 || s >= kSiteCount ? 0 : s];
}
// The label: an untagged marker composite (VS B10B032BDFD46700, PS DB899F4BD577F2E5) whose record is camera-local, (0, 0, 0.01).
constexpr uint64_t kLabelVs = 0xB10B032BDFD46700ull, kLabelPs = 0xDB899F4BD577F2E5ull;
inline bool cameraLocal(const float p[3]) noexcept { return std::fabs(p[0]) <= 1e-6f && std::fabs(p[1]) <= 1e-6f && std::fabs(p[2] - 0.01f) <= 1e-6f; }
struct LabelCensus {
    uint64_t composites = 0, eyeFrames = 0, candidates = 0, read = 0;
    uint32_t idx[4] = {}, idxN[4] = {};
    uint64_t idxOther = 0;
    void index(uint32_t count) noexcept {
        for (int i = 0; i < 4; ++i) {
            if (idxN[i] && idx[i] == count) {
                ++idxN[i];
                return;
            }
        }
        for (int i = 0; i < 4; ++i)
            if (!idxN[i]) {
                idx[i] = count;
                idxN[i] = 1;
                return;
            }
        ++idxOther;
    }
    // "3600:12 624:12", else "none"
    void text(char* buf, size_t n) const noexcept {
        size_t at = 0;
        buf[0] = 0;
        for (int i = 0; i < 4; ++i)
            if (idxN[i] && at < n)
                at += static_cast<size_t>(std::snprintf(buf + at, n - at, "%s%u:%u", at ? " " : "", idx[i], idxN[i]));
        if (idxOther && at < n) at += static_cast<size_t>(std::snprintf(buf + at, n - at, "%sother:%llu", at ? " " : "", static_cast<unsigned long long>(idxOther)));
        if (!at) std::snprintf(buf, n, "none");
    }
};
// The model's bounding box as the getter reads it: the local min and max, and the world matrix (row-vector rows; the translation at 12..14).
struct Box {
    double mn[3] = {}, mx[3] = {};
    float world[16] = {};
};
inline void boxPoint(const Box& b, const double p[3], double out[3]) noexcept {
    for (int j = 0; j < 3; ++j)
        out[j] = p[0] * static_cast<double>(b.world[j]) + p[1] * static_cast<double>(b.world[4 + j]) + p[2] * static_cast<double>(b.world[8 + j]) + static_cast<double>(b.world[12 + j]);
}
// The game's centre: min + 0.5 (max - min) through the world matrix.
inline void boxCentre(const Box& b, double out[3]) noexcept {
    const double c[3] = {b.mn[0] + 0.5 * (b.mx[0] - b.mn[0]), b.mn[1] + 0.5 * (b.mx[1] - b.mn[1]), b.mn[2] + 0.5 * (b.mx[2] - b.mn[2])};
    boxPoint(b, c, out);
}
struct BoxFace {
    bool ok = false;
    int axis = 0;
    double sign = 1.0, n[3] = {}, h = 0.0;   // n: the face's outward normal in world; h: the half extent along it (world metres)
};
// The face of the box most facing `cam` (the largest n . (cam - C)).
inline BoxFace boxFaceFacing(const Box& b, const double cam[3]) noexcept {
    BoxFace best;
    double C[3];
    boxCentre(b, C);
    double bestScore = -1e300;
    for (int a = 0; a < 3; ++a) {
        const double r[3] = {static_cast<double>(b.world[4 * a]), static_cast<double>(b.world[4 * a + 1]), static_cast<double>(b.world[4 * a + 2])};
        const double len = f85::len3(r);
        if (!(len > 1e-9) || !std::isfinite(len)) continue;
        for (int s = 0; s < 2; ++s) {
            const double sg = s == 0 ? 1.0 : -1.0;
            const double n[3] = {sg * r[0] / len, sg * r[1] / len, sg * r[2] / len};
            const double score = n[0] * (cam[0] - C[0]) + n[1] * (cam[1] - C[1]) + n[2] * (cam[2] - C[2]);
            if (score > bestScore) {
                bestScore = score;
                best.ok = true;
                best.axis = a;
                best.sign = sg;
                for (int i = 0; i < 3; ++i) best.n[i] = n[i];
                best.h = 0.5 * (b.mx[a] - b.mn[a]) * len;
            }
        }
    }
    return best;
}
// P' = P + n ((C + h n - P) . n): P carried along n onto the face plane.
inline bool boxFrontPoint(const Box& b, const BoxFace& f, const double P[3], double out[3]) noexcept {
    if (!f.ok) return false;
    double C[3];
    boxCentre(b, C);
    const double q[3] = {C[0] + f.h * f.n[0] - P[0], C[1] + f.h * f.n[1] - P[1], C[2] + f.h * f.n[2] - P[2]};
    const double k = q[0] * f.n[0] + q[1] * f.n[1] + q[2] * f.n[2];
    for (int i = 0; i < 3; ++i) out[i] = P[i] + f.n[i] * k;
    return std::isfinite(out[0]) && std::isfinite(out[1]) && std::isfinite(out[2]);
}

}  // namespace f87
}  // namespace nofoot
}  // namespace edvr
