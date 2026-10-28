// F161 checks (native on-foot VR), run161 below. Header only, run by native_frame_test.
//
// F161 (native_on_foot_f161.h): the left arm's distance to the eye, measured. The eye as drawn against the left upper arm (the segment from the upper arm joint to the forearm joint)
// and the elbow, per context (the hip or ADS, standing or crouched, moving), the frames within 4 cm and within 2.5 cm of the eye, the seconds that had any, and the window's line.
// The figures are the flight edvr_gfx_20261028_135901's crouch-walk in ADS: the left shoulder 20.6 cm behind the eye, 11.8 cm above it and 5.2 cm to its right, the upper arm 30.4 cm
// long passing 1.3 to 4.7 cm from it. (The body's lowering through ADS is F97a's, checked with it, run97.)
#pragma once

#include "../../src/d3d11/native_on_foot_f161.h"

#include <cmath>
#include <cstdint>
#include <cstring>

namespace f161test {

namespace f161 = edvr::nofoot::f161;

template <class Check>
inline void run161(Check&& check) {
    const auto eq = [](double a, double b, double tol) { return std::fabs(a - b) <= tol; };
    // ---- 1. the distance ----------------------------------------------------------------------------------------------------------------------------------------------------------
    {
        const double a[3] = {0.0, 0.0, 0.0}, b[3] = {0.0, 0.0, 0.30};
        double s = -1.0;
        const double mid[3] = {0.03, 0.0, 0.15};
        check(eq(f161::pointSegmentDistance(mid, a, b, &s), 0.03, 1e-12) && eq(s, 0.5, 1e-12), "F161: beside the middle of the segment the distance is the perpendicular, 3 cm, at half its length");
        const double beyond[3] = {0.0, 0.04, 0.40}, behind[3] = {0.0, 0.0, -0.05};
        check(eq(f161::pointSegmentDistance(beyond, a, b, &s), std::sqrt(0.04 * 0.04 + 0.10 * 0.10), 1e-12) && s == 1.0 && eq(f161::pointSegmentDistance(behind, a, b, &s), 0.05, 1e-12) && s == 0.0,
              "F161: past the elbow the nearest point is the elbow, behind the shoulder the shoulder (the segment ends, not the line)");
        check(eq(f161::pointSegmentDistance(mid, a, a, &s), std::sqrt(0.03 * 0.03 + 0.15 * 0.15), 1e-12) && s == 0.0, "F161: a segment of no length is its point");
        const double bad[3] = {std::nan(""), 0.0, 0.0};
        check(std::isnan(f161::pointSegmentDistance(bad, a, b)) && std::isnan(f161::pointSegmentDistance(mid, bad, b)) && std::isnan(f161::distance(bad, a)),
              "F161: a figure that is not a number gives no distance");
        // the flight's geometry in the eye's axes (right, up, forward): the shoulder 5.2 right, 11.8 up and 20.6 behind, the elbow 30 cm along the arm to a grip ahead of the eye
        const double shoulder[3] = {0.052, 0.118, -0.206}, elbow[3] = {0.010, -0.020, 0.085}, eye[3] = {0.0, 0.0, 0.0};
        double fit = -1.0;
        const double d = f161::pointSegmentDistance(eye, shoulder, elbow, &fit);
        // the brute force over the segment: ten thousand points
        double best = 1e9;
        for (int i = 0; i <= 10000; ++i) {
            const double t = i / 10000.0, p[3] = {shoulder[0] + t * (elbow[0] - shoulder[0]), shoulder[1] + t * (elbow[1] - shoulder[1]), shoulder[2] + t * (elbow[2] - shoulder[2])};
            best = std::fmin(best, std::sqrt(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]));
        }
        check(eq(d, best, 1e-6) && eq(d, 0.0279, 5e-4) && fit > 0.7 && fit < 0.8 && f161::distance(eye, elbow) > 0.08,
              "F161: the flight's crouch-walk geometry: the arm passes within 4 cm of the eye in the middle of the upper arm, equal to a brute force over the segment, and the elbow is further off");
    }
    // ---- 2. the counters -----------------------------------------------------------------------------------------------------------------------------------------------------------
    {
        check(f161::contextOf(false, false) == f161::kHipStanding && f161::contextOf(false, true) == f161::kHipCrouched && f161::contextOf(true, false) == f161::kAdsStanding &&
                  f161::contextOf(true, true) == f161::kAdsCrouched && std::strcmp(f161::contextName(f161::kAdsCrouched), "ADS crouched") == 0 &&
                  std::strcmp(f161::contextName(f161::kHipStanding), "hip standing") == 0,
              "F161: the contexts: the hip or ADS, standing or crouched");
        f161::Counters c;
        const auto add = [&](double armCm, double elbowCm, bool ads, bool crouched, bool moving) {
            f161::Sample s;
            s.armM = armCm * 0.01;
            s.elbowM = elbowCm * 0.01;
            s.ads = ads;
            s.crouched = crouched;
            s.moving = moving;
            c.add(s);
        };
        add(18.0, 25.0, false, false, false);   // the hip standing: far
        add(12.0, 20.0, false, false, true);
        add(3.9, 11.0, true, true, true);       // ADS crouched, near and moving
        add(2.4, 9.0, true, true, true);        // within the near plane
        add(3.0, 6.0, true, true, false);       // near, still
        add(4.0, 8.0, true, true, true);        // 4.0 cm exactly is not under 4 cm
        add(30.0, 40.0, true, false, true);     // ADS standing
        add(std::nan(""), 8.0, true, true, true);   // not counted
        add(-1.0, 8.0, true, true, true);           // not counted
        add(7.0, 5.0, false, true, false);          // the arm over the elbow by rounding: taken as the elbow's, 5 cm
        const f161::Bin& hs = c.bin[f161::kHipStanding];
        const f161::Bin& hc = c.bin[f161::kHipCrouched];
        const f161::Bin& ac = c.bin[f161::kAdsCrouched];
        const f161::Bin& as = c.bin[f161::kAdsStanding];
        check(hs.n == 2 && hs.under4 == 0 && eq(hs.armMin, 12.0, 1e-9) && eq(hs.armSum, 30.0, 1e-9) && eq(hs.elbowMin, 20.0, 1e-9) && eq(hs.elbowSum, 45.0, 1e-9),
              "F161: the hip standing: the frames, the arm's and the elbow's smallest and summed distances (cm)");
        check(ac.n == 4 && ac.under4 == 3 && ac.under4Moving == 2 && ac.under25 == 1 && eq(ac.armMin, 2.4, 1e-9) && eq(ac.armSum, 3.9 + 2.4 + 3.0 + 4.0, 1e-9) && eq(ac.elbowMin, 6.0, 1e-9),
              "F161: ADS crouched: 4 frames, 3 within 4 cm (2 of them moving), 1 within 2.5 cm; 4.0 cm is not within 4 cm");
        check(as.n == 1 && as.under4 == 0 && hc.n == 1 && hc.under4 == 0 && eq(hc.armMin, 5.0, 1e-9) && c.frames() == 8 && c.under4() == 3 && c.under25() == 1,
              "F161: ADS standing and the hip crouched are kept apart; a distance that is not a number or negative is left out; the arm over the elbow is taken as the elbow's; the totals");
        f161::Counters d;
        d.merge(c);
        d.merge(c);
        check(d.frames() == 16 && d.under4() == 6 && d.under25() == 2 && eq(d.bin[f161::kAdsCrouched].armMin, 2.4, 1e-9) && eq(d.bin[f161::kAdsCrouched].armSum, 2 * 13.3, 1e-9) &&
                  d.bin[f161::kAdsCrouched].under4Moving == 4,
              "F161: two seconds merge by sums and minima");
        // the seconds of a window: any frame within 4 cm makes the second one, any within 2.5 cm a clip second
        f161::Seconds sec;
        f161::Counters calm, closeOnly, clipping;
        {
            f161::Sample s;
            s.armM = 0.20;
            s.elbowM = 0.25;
            calm.add(s);
            s.armM = 0.035;
            closeOnly.add(s);
            s.armM = 0.02;
            clipping.add(s);
        }
        sec.fold(calm);
        sec.fold(closeOnly);
        sec.fold(clipping);
        sec.fold(f161::Counters{});
        check(sec.under4 == 2 && sec.under25 == 1, "F161: a second counts when any frame of it was within 4 cm, and as a clip second when any was within 2.5 cm (3 seconds in, 2 and 1)");
    }
    // ---- 3. the line ----------------------------------------------------------------------------------------------------------------------------------------------------------------
    {
        f161::Counters c;
        // F163: the crouched weight and the animated neck's drop (m) each frame was filed by
        const auto add = [&](double armCm, bool ads, bool crouched, bool moving, double postureW = 0.0, double neckDropM = 0.05) {
            f161::Sample s;
            s.armM = armCm * 0.01;
            s.elbowM = armCm * 0.01 + 0.03;
            s.ads = ads;
            s.crouched = crouched;
            s.moving = moving;
            s.postureW = postureW;
            s.neckDropM = neckDropM;
            c.add(s);
        };
        add(3.2, true, true, true, 1.0, 0.50);
        add(2.0, true, true, true, 0.94, 0.60);
        add(15.0, false, false, false);
        f161::Seconds sec;
        sec.under4 = 1;
        sec.under25 = 1;
        char buf[1300];
        const int n = f161::formatSummary(buf, sizeof(buf), 10, "$wpn_m_assaultrifle_plasma_fauto_name;", c, sec);
        check(n > 0 && n < 1000 && std::strstr(buf, "(F161) 10 s: item $wpn_m_assaultrifle_plasma_fauto_name;; the eye to the left upper arm (shoulder to elbow) and to the elbow, by context: hip standing 1 frames: "
                                                  "arm min 15.0 mean 15.0 cm, elbow min 18.0 mean 18.0 cm, under 4 cm 0 (moving 0), under 2.5 cm 0, crouched weight mean 0.00, neck drop mean 5.0 cm; "
                                                  "hip crouched 0 frames: arm min 0.0 mean 0.0 cm, elbow min 0.0 mean 0.0 cm, under 4 cm 0 (moving 0), under 2.5 cm 0, crouched weight mean 0.00, neck drop mean 0.0 cm; "
                                                  "ADS standing 0 frames:") != nullptr &&
                  std::strstr(buf, "ADS crouched 2 frames: arm min 2.0 mean 2.6 cm, elbow min 5.0 mean 5.6 cm, under 4 cm 2 (moving 2), under 2.5 cm 1, crouched weight mean 0.97, neck drop mean 55.0 cm; "
                                   "seconds with the arm under 4 cm of the eye 1, under 2.5 cm 1") != nullptr,
              "F161: the window's line names the item, the four contexts (zeros for one with no frame), each one's crouched weight and neck drop (F163), and the seconds; under 1000 bytes");
        // at its widest: every figure at its widest (the neck's drop at three digits and a sign)
        f161::Counters w;
        for (int i = 0; i < f161::kContextN; ++i) {
            f161::Bin& b = w.bin[i];
            b.n = b.under4 = b.under4Moving = b.under25 = b.postureN = 4000000000u;
            b.armSum = b.elbowSum = 1e9;
            b.armMin = b.elbowMin = 999.9;
            b.postureSum = 0.99 * 4000000000.0;
            b.dropSumCm = -199.9 * 4000000000.0;
        }
        f161::Seconds ws;
        ws.under4 = ws.under25 = 4000000000u;
        const int wn = f161::formatSummary(buf, sizeof(buf), 4000000000u, "$wpn_m_assaultrifle_kinetic_fauto_suppressed_name;", w, ws);
        check(wn > 0 && wn < 1200 && std::strlen(buf) == static_cast<size_t>(wn), "F161: the line at its widest (every count at ten digits) stays under the log's 1200 bytes, and the buffer holds it");
    }
    // ---- 4. the crouched weight and the neck's drop per context (F163) ----------------------------------------------------------------------------------------------------
    {
        f161::Counters c;
        const auto add = [&](bool ads, bool crouched, double postureW, double neckDropM) {
            f161::Sample s;
            s.armM = 0.20;
            s.elbowM = 0.25;
            s.ads = ads;
            s.crouched = crouched;
            s.postureW = postureW;
            s.neckDropM = neckDropM;
            c.add(s);
        };
        add(true, true, 1.0, 0.50);
        add(true, true, 0.5, 0.38);
        add(true, false, 0.0, 0.10);
        add(true, false, 0.0, 0.20);
        add(true, false, std::nan(""), 0.20);   // a weight that is not a number: the frame counts, its weight and drop do not
        add(true, false, 0.0, std::nan(""));    // a drop that is not a number: the same
        const f161::Bin& ac = c.bin[f161::kAdsCrouched];
        const f161::Bin& as = c.bin[f161::kAdsStanding];
        check(ac.n == 2 && ac.postureN == 2 && eq(ac.postureSum, 1.5, 1e-12) && eq(ac.dropSumCm, 88.0, 1e-9) && as.n == 4 && as.postureN == 2 && eq(as.postureSum, 0.0, 1e-12) &&
                  eq(as.dropSumCm, 30.0, 1e-9),
              "F161: each context sums the crouched weight and the neck's drop (cm) of the frames that had them; a frame without either still counts as a frame");
        f161::Counters d;
        d.merge(c);
        d.merge(c);
        check(d.bin[f161::kAdsCrouched].postureN == 4 && eq(d.bin[f161::kAdsCrouched].postureSum, 3.0, 1e-12) && eq(d.bin[f161::kAdsCrouched].dropSumCm, 176.0, 1e-9),
              "F161: two seconds merge the weight and the drop by sums");
        char buf[1300];
        f161::Seconds none;
        f161::formatSummary(buf, sizeof(buf), 5, "$wpn_m_shotgun_plasma_doublebarrel_name;", c, none);
        check(std::strstr(buf, "ADS standing 4 frames: arm min 20.0 mean 20.0 cm, elbow min 25.0 mean 25.0 cm, under 4 cm 0 (moving 0), under 2.5 cm 0, crouched weight mean 0.00, neck drop mean 15.0 cm; "
                               "ADS crouched 2 frames:") != nullptr &&
                  std::strstr(buf, "ADS crouched 2 frames: arm min 20.0 mean 20.0 cm, elbow min 25.0 mean 25.0 cm, under 4 cm 0 (moving 0), under 2.5 cm 0, crouched weight mean 0.75, neck drop mean 44.0 cm; ") != nullptr,
              "F161: the line says the mean weight and the mean neck's drop of each context, over the frames that had them");
    }
}

}  // namespace f161test
