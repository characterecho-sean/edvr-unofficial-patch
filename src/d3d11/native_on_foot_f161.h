// F161 (native on-foot VR): the left arm's distance to the eye, measured. Header only, no game or D3D11 state, so native_frame_test runs the very functions the Animate post-hook and
// the frame thread use.
//
// The pilot, crouch-walking in ADS with the plasma AR: the left arm clipped in front of the view. The dumps of the flight edvr_gfx_20261028_135901 (140218592001 and 140223977002)
// show the left upper arm and the elbow's sleeve cut by the 2.5 cm near plane in both eyes, following the step bob, the hand on its grip. The eye minus the left upper arm joint read
// (-5.2, -11.8, +20.6) cm, the arm nearly straight, so the upper arm passed 1.3 to 4.7 cm from the eye. F97a's crouched lowering (native_on_foot_f97.h) holds through ADS from F161
// on; this measures what that leaves between the arm and the eye, so the next flight says how close the arm comes and in which context, and not by the pilot's report.
//
// Each left-arm solve, the eye as drawn (the pin's cyclopean eye with all EDVR adds, the one the INSIDE test of F97a uses) against the left upper arm as finally written (the segment from
// the upper arm joint to the forearm joint, the elbow) and against the elbow itself, in the context of the call: the hip or ADS (the body's detach weight from 0.05, as F159), standing or
// crouched (the camera's crouch weight from 0.5, the pin's own test), moving or not. The figures are the nearest distance of the eye to the segment and to the elbow, so an eye over the
// middle of the upper arm reads the arm's distance and not the elbow's.
#pragma once

#include "native_on_foot_f159.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace edvr {
namespace nofoot {
namespace f161 {

constexpr double kNearM = 0.04;   // "near": the eye within this of the arm (the near plane, and 1.5 cm over it: the figure is the cyclopean eye's, each eye is up to 3.2 cm off it)
constexpr double kClipM = 0.025;  // the near plane: an arm closer than this to the eye is cut by it

// ---- the distance ---------------------------------------------------------------------------------------------------------------------------------------------------------

inline double distance(const double a[3], const double b[3]) noexcept {
    const double d[3] = {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
    const double r = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    return std::isfinite(r) ? r : std::nan("");
}
// The nearest distance of the point p to the segment from a to b (both ends included), and where the nearest point is on the segment (0 at a, 1 at b). A segment of no length is its
// point. Not a number when any figure is not finite.
inline double pointSegmentDistance(const double p[3], const double a[3], const double b[3], double* alongSegment = nullptr) noexcept {
    const double ab[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]};
    const double ap[3] = {p[0] - a[0], p[1] - a[1], p[2] - a[2]};
    const double len2 = ab[0] * ab[0] + ab[1] * ab[1] + ab[2] * ab[2];
    double s = 0.0;
    if (len2 > 1e-12) {
        s = (ap[0] * ab[0] + ap[1] * ab[1] + ap[2] * ab[2]) / len2;
        s = s < 0.0 ? 0.0 : s > 1.0 ? 1.0 : s;
    }
    const double nearest[3] = {a[0] + s * ab[0], a[1] + s * ab[1], a[2] + s * ab[2]};
    if (alongSegment) *alongSegment = s;
    return distance(p, nearest);
}

// ---- the contexts ---------------------------------------------------------------------------------------------------------------------------------------------------------

enum Context : int { kHipStanding = 0, kHipCrouched, kAdsStanding, kAdsCrouched, kContextN };
inline int contextOf(bool ads, bool crouched) noexcept { return (ads ? 2 : 0) + (crouched ? 1 : 0); }
inline const char* contextName(int c) noexcept {
    return c == kHipStanding ? "hip standing" : c == kHipCrouched ? "hip crouched" : c == kAdsStanding ? "ADS standing" : c == kAdsCrouched ? "ADS crouched" : "unknown";
}

// One left-arm solve: the eye's nearest distance to the upper arm (shoulder to elbow) and to the elbow (m), and the call's context.
struct Sample {
    double armM = 0.0, elbowM = 0.0;
    bool ads = false, crouched = false, moving = false;
};
// One context's figures: the frames, the arm's and the elbow's smallest and summed distances (cm), the frames with the arm under 4 cm of the eye (and of those, the moving ones) and
// under 2.5 cm.
struct Bin {
    uint32_t n = 0, under4 = 0, under4Moving = 0, under25 = 0;
    double armSum = 0.0, armMin = 1e9, elbowSum = 0.0, elbowMin = 1e9;
};
struct Counters {
    Bin bin[kContextN];
    // The distances in metres, a negative or not-a-number one dropped. The elbow is on the segment, so the arm's distance is never over the elbow's: a figure that says otherwise by
    // rounding is taken as the elbow's.
    void add(const Sample& s) noexcept {
        if (!std::isfinite(s.armM) || !std::isfinite(s.elbowM) || s.armM < 0.0 || s.elbowM < 0.0) return;
        Bin& b = bin[contextOf(s.ads, s.crouched)];
        const double arm = s.armM < s.elbowM ? s.armM : s.elbowM;
        const double armCm = arm * 100.0, elbowCm = s.elbowM * 100.0;
        ++b.n;
        if (arm < kNearM) {
            ++b.under4;
            if (s.moving) ++b.under4Moving;
        }
        if (arm < kClipM) ++b.under25;
        b.armSum += armCm;
        b.armMin = armCm < b.armMin ? armCm : b.armMin;
        b.elbowSum += elbowCm;
        b.elbowMin = elbowCm < b.elbowMin ? elbowCm : b.elbowMin;
    }
    void merge(const Counters& o) noexcept {
        for (int i = 0; i < kContextN; ++i) {
            Bin& d = bin[i];
            const Bin& s = o.bin[i];
            d.n += s.n;
            d.under4 += s.under4;
            d.under4Moving += s.under4Moving;
            d.under25 += s.under25;
            d.armSum += s.armSum;
            d.elbowSum += s.elbowSum;
            d.armMin = s.armMin < d.armMin ? s.armMin : d.armMin;
            d.elbowMin = s.elbowMin < d.elbowMin ? s.elbowMin : d.elbowMin;
        }
    }
    uint32_t frames() const noexcept { return bin[0].n + bin[1].n + bin[2].n + bin[3].n; }
    uint32_t under4() const noexcept { return bin[0].under4 + bin[1].under4 + bin[2].under4 + bin[3].under4; }
    uint32_t under25() const noexcept { return bin[0].under25 + bin[1].under25 + bin[2].under25 + bin[3].under25; }
};

// The seconds of a window in which the arm came within 4 cm of the eye, and within 2.5 cm: a second counts for either when any of its frames did.
struct Seconds {
    uint32_t under4 = 0, under25 = 0;
    void fold(const Counters& second) noexcept {
        if (second.under4()) ++under4;
        if (second.under25()) ++under25;
    }
};

// ---- the line -------------------------------------------------------------------------------------------------------------------------------------------------------------

// The window's line: the item, and for each context its frames, the arm's and the elbow's smallest and mean distance (cm), the frames under 4 cm (the moving ones among them) and under
// 2.5 cm; a context with no frame prints zeros. Then the seconds with the arm under 4 cm, and under 2.5 cm. The log's lines end at 1200 bytes; this one runs to about 700.
inline int formatSummary(char* buf, size_t n, uint32_t seconds, const char* item, const Counters& c, const Seconds& s) noexcept {
    int len = std::snprintf(buf, n, "native on foot left arm to the eye (F161) %u s: item %s; the eye to the left upper arm (shoulder to elbow) and to the elbow, by context: ", seconds,
                            item ? item : "none");
    if (len < 0 || static_cast<size_t>(len) >= n) return len;
    for (int i = 0; i < kContextN; ++i) {
        const Bin& b = c.bin[i];
        const double nn = b.n ? static_cast<double>(b.n) : 1.0;
        const int add = std::snprintf(buf + len, n - static_cast<size_t>(len), "%s %u frames: arm min %.1f mean %.1f cm, elbow min %.1f mean %.1f cm, under 4 cm %u (moving %u), under 2.5 cm %u; ",
                                      contextName(i), b.n, b.n ? b.armMin : 0.0, b.armSum / nn, b.n ? b.elbowMin : 0.0, b.elbowSum / nn, b.under4, b.under4Moving, b.under25);
        if (add < 0) return len;
        len += add;
        if (static_cast<size_t>(len) >= n) return len;
    }
    const int add = std::snprintf(buf + len, n - static_cast<size_t>(len), "seconds with the arm under 4 cm of the eye %u, under 2.5 cm %u", s.under4, s.under25);
    return add < 0 ? len : len + add;
}

}  // namespace f161
}  // namespace nofoot
}  // namespace edvr
