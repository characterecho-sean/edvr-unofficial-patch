// F94b checks (native on-foot VR), run94b below: the interaction label moved by its own anchor (native_on_foot_f94b.h) through the real row patch
// (draw_record_tag.h: markerAffine, markerAffineU, affineOffsetRows, displayOfUi), and the measure of F93c's rule beside it. Header only, run by native_frame_test.
//
// The camera: a pinhole yawed about y; the scene rows (b1 270..273, row-vector form) at this frame's yaw, the marker rows (b0 rows 4, 5, 7) at a yaw some frames
// older, with the marker projection's own focal scale (sigma about 1.0094, the F65 measurement) and the scene rows carrying a TAA jitter. 2016 x 2017 target.
#pragma once

#include "../../src/d3d11/draw_record_tag.h"
#include "../../src/d3d11/native_on_foot_f87.h"
#include "../../src/d3d11/native_on_foot_f94b.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace f94btest {

namespace dt = edvr::drawtag;
namespace f94b = edvr::nofoot::f94b;
namespace f87 = edvr::nofoot::f87;

constexpr double kPi = 3.14159265358979323846;
constexpr double kW = 2016.0, kH = 2017.0;
constexpr double kFx = 0.94, kFy = 0.934, kCx = 0.16, kCy = 0.09, kSigma = 1.0094, kJx = 0.0004, kJy = -0.0003;

// The scene rows at yaw deg: clip x = fx xv + cx zv, clip y = fy yv + cy zv, w = zv; xv = c x - s z, zv = s x + c z (cx, cy carry the jitter).
inline void sceneRows(double yawDeg, float rows[16]) {
    const double s = std::sin(yawDeg * kPi / 180.0), c = std::cos(yawDeg * kPi / 180.0), cx = kCx + kJx, cy = kCy + kJy;
    for (int i = 0; i < 16; ++i) rows[i] = 0.0f;
    rows[0] = static_cast<float>(kFx * c + cx * s);
    rows[8] = static_cast<float>(-kFx * s + cx * c);
    rows[1] = static_cast<float>(cy * s);
    rows[5] = static_cast<float>(kFy);
    rows[9] = static_cast<float>(cy * c);
    rows[3] = static_cast<float>(s);
    rows[11] = static_cast<float>(c);
    rows[10] = 0.5f;
    rows[14] = 0.01f;
}
// The marker rows at yaw deg, the marker projection's focal scale sigma times the scene's, no jitter.
inline void markerRows(double yawDeg, float r4[4], float r5[4], float r7[4]) {
    const double s = std::sin(yawDeg * kPi / 180.0), c = std::cos(yawDeg * kPi / 180.0), fx = kFx * kSigma, fy = kFy * kSigma;
    r4[0] = static_cast<float>(fx * c + kCx * s), r4[1] = 0.0f, r4[2] = static_cast<float>(-fx * s + kCx * c), r4[3] = 0.0f;
    r5[0] = static_cast<float>(kCy * s), r5[1] = static_cast<float>(fy), r5[2] = static_cast<float>(kCy * c), r5[3] = 0.0f;
    r7[0] = static_cast<float>(s), r7[1] = 0.0f, r7[2] = static_cast<float>(c), r7[3] = 0.0f;
}
// One eye-frame as fixDecide sees a marker: the affine, P through the scene rows unjittered, a point through the unpatched marker rows.
struct View {
    float rows[16], r4[4], r5[4], r7[4];
    dt::MarkerAffine aff;
    double o[3] = {0.0, 0.0, 0.0};   // the eye's position (both projections' origin)
    View(double sceneYaw, double markerYaw, double eyeX = 0.0) {
        o[0] = eyeX;
        sceneRows(sceneYaw, rows);
        markerRows(markerYaw, r4, r5, r7);
        dt::ProjDecomp dM, dW;
        const double axis[2] = {kCx, kCy};
        if (dt::projDecomposeMarker(r4, r5, r7, dM) && dt::projDecomposeWorld(rows, dW)) aff = dt::markerAffine(dM, dW, axis);
    }
    void scene(const double p[3], double out[2]) const {
        double wc[4];
        dt::worldClip(rows, p, o, wc);
        const double n[2] = {wc[0] / wc[3], wc[1] / wc[3]};
        dt::unjitter(aff, n, out);
    }
    void marker(const double p[3], double out[2]) const {
        double mc[3];
        dt::markerClip(r4, r5, r7, p, o, mc);
        out[0] = mc[0] / mc[2];
        out[1] = mc[1] / mc[2];
    }
    // Where a point is shown when the draw's rows are patched for target T of the record at nM0 (the patch fixDecide binds).
    void shownPatched(const double T[2], const double nM0[2], const double p[3], double out[2]) const {
        float q4[4], q5[4];
        for (int i = 0; i < 4; ++i) q4[i] = r4[i], q5[i] = r5[i];
        double u[2];
        dt::markerAffineU(aff, T, nM0, u);
        dt::affineOffsetRows(q4, q5, r7, aff.sigma, u);
        double mc[3];
        dt::markerClip(q4, q5, r7, p, o, mc);
        const double n[2] = {mc[0] / mc[2], mc[1] / mc[2]};
        dt::displayOfUi(aff, n, out);
    }
};
inline double px(const double a[2], const double b[2]) { return std::sqrt(std::pow((a[0] - b[0]) * kW * 0.5, 2) + std::pow((a[1] - b[1]) * kH * 0.5, 2)); }

// The F92 flight's charge port, roughly: the label's anchor P 1.9 m ahead, its record laid out right of it and below in the plane of the camera the game laid it out
// with (t = P + s (off.x X - off.y Y): the marker camera's right and up axes), a dot B beside it at another depth whose record sits 36 px (at 2016) above its
// anchor, like key 46's.
const double kP[3] = {0.10, 0.05, 1.90};
const double kPB[3] = {-0.05, 0.13, 2.05};
const double kX0B[3] = {-0.05, 0.13 + 0.073, 2.05};
inline void layout(double markerYawDeg, double X0[3]) {
    const double s = std::sin(markerYawDeg * kPi / 180.0), c = std::cos(markerYawDeg * kPi / 180.0);
    X0[0] = kP[0] + 0.33 * c;
    X0[1] = kP[1] - 0.23;
    X0[2] = kP[2] - 0.33 * s;
}

template <class Check>
inline void run94b(Check&& check) {
    // ---- the row patch with T from the label's own anchor: the anchor is shown where the scene shows it, the record at T ---------------------------------------------
    {
        const View v(0.0, 2.0);   // the marker rows 2 deg of yaw behind the scene's
        double X0[3], nWu[2], nM0[2], nMP[2], T[2], shownP[2], shownX0[2], gameP[2];
        layout(2.0, X0);
        v.scene(kP, nWu);
        v.marker(X0, nM0);
        v.marker(kP, nMP);
        f94b::ownAnchorTarget(nWu, nM0, nMP, T);
        v.shownPatched(T, nM0, kP, shownP);
        v.shownPatched(T, nM0, X0, shownX0);
        dt::displayOfUi(v.aff, nMP, gameP);
        check(v.aff.ok && std::fabs(v.aff.sigma[0] - kSigma) < 1e-6 && std::fabs(v.aff.jitter[0] - kJx) < 1e-6 && px(shownP, nWu) < 1e-3 && px(shownX0, T) < 1e-3 &&
                  px(gameP, nWu) > 20.0,
              "F94b: with T from the label's own anchor the patched rows show the anchor exactly where the scene rows show it (the dot's place, sigma 1.0094, jitter "
              "removed) and the record at T; unpatched, the game's anchor was over 20 px off in a 2 deg lag");
        // the layout kept: the record's offset from the anchor is the game's (through the same marker rows), about (+173, -120) px at 2016 like the flight's key 1
        const double off[2] = {(T[0] - nWu[0]) * kW * 0.5, (T[1] - nWu[1]) * kH * 0.5}, game[2] = {(nM0[0] - nMP[0]) * kW * 0.5, (nM0[1] - nMP[1]) * kH * 0.5};
        check(std::fabs(off[0] - game[0]) < 1e-9 && std::fabs(off[1] - game[1]) < 1e-9 && off[0] > 120.0 && off[1] < -80.0,
              "F94b: the label from its anchor is the game's own layout offset as the marker rows draw it (up-right of the anchor in the camera's plane)");
        // F93c's rule with the label's own anchor as its reference in the same frame is the same T
        const double d[2] = {nWu[0] - nMP[0], nWu[1] - nMP[1]}, T93[2] = {nM0[0] + d[0], nM0[1] + d[1]};
        check(px(T93, T) < 1e-9, "F94b: F93c's rule gives the same target when its reference is the label's own anchor in the same frame");
    }
    // ---- a turn with the marker rows' lag alternating 1 and 2 frames (the flight's 1.75 frames): the label stays on its anchor, F93c's previous-frame rule does not ----
    {
        f94b::Tracker tr;
        f94b::Sec sec;
        double prevD[2] = {};
        bool havePrev = false;
        double worstLock = 0.0;
        const double rate = 0.6;   // deg a frame: 54 deg/s at 90 Hz
        for (uint32_t n = 2; n < 14; ++n) {
            const double yaw = rate * n, lagFrames = (n % 2) ? 1.0 : 2.0;
            const View v(yaw, yaw - rate * lagFrames);
            double X0[3], nWu[2], nM0[2], nMP[2], T[2], shownP[2];
            layout(yaw - rate * lagFrames, X0);
            v.scene(kP, nWu);
            v.marker(X0, nM0);
            v.marker(kP, nMP);
            f94b::ownAnchorTarget(nWu, nM0, nMP, T);
            v.shownPatched(T, nM0, kP, shownP);
            worstLock = std::max(worstLock, px(shownP, nWu));
            // F93c's rule: the dot at the anchor (a plain marker, record at P) as the reference, its correction from the frame before
            double T93[2];
            if (havePrev) {
                T93[0] = nM0[0] + prevD[0];
                T93[1] = nM0[1] + prevD[1];
            } else {
                dt::displayOfUi(v.aff, nM0, T93);
            }
            prevD[0] = nWu[0] - nMP[0];
            prevD[1] = nWu[1] - nMP[1];
            f94b::Sample x;
            x.frame = n;
            x.key = 1;
            x.off[0] = (T[0] - nWu[0]) * kW * 0.5;
            x.off[1] = (T[1] - nWu[1]) * kH * 0.5;
            x.off93[0] = (T93[0] - nWu[0]) * kW * 0.5;
            x.off93[1] = (T93[1] - nWu[1]) * kH * 0.5;
            x.ref93 = havePrev;
            x.refKey = 45;
            x.headDps = rate * 90.0;
            for (int k = 0; k < 3; ++k) x.p[k] = kP[k];
            tr.add(x, sec);
            havePrev = true;
        }
        check(worstLock < 1e-3 && sec.steps == 11 && sec.stepMax < 0.5 && sec.step93Max > 5.0 && sec.err93Max > 3.0 && sec.fastN == 12 && sec.switches == 1 &&
                  sec.pStepMax == 0.0,
              "F94b: in a 54 deg/s turn with the marker rows 1 and 2 frames late in turn, the label's anchor stays on the scene's (under 0.001 px) and the label moves "
              "under 0.5 px a frame against it; F93c's rule (the frame before's correction) steps over 5 px a frame against its anchor");
        // with a steady lag and a steady turn F93c's frame-old correction is right: its error is the change of the lag between frames, not the lag
        f94b::Tracker tr2;
        f94b::Sec sec2;
        havePrev = false;
        for (uint32_t n = 2; n < 10; ++n) {
            const double yaw = rate * n;
            const View v(yaw, yaw - rate * 2.0);
            double X0[3], nWu[2], nM0[2], nMP[2], T[2];
            layout(yaw - rate * 2.0, X0);
            v.scene(kP, nWu);
            v.marker(X0, nM0);
            v.marker(kP, nMP);
            f94b::ownAnchorTarget(nWu, nM0, nMP, T);
            const double T93[2] = {nM0[0] + prevD[0], nM0[1] + prevD[1]};
            prevD[0] = nWu[0] - nMP[0];
            prevD[1] = nWu[1] - nMP[1];
            if (!havePrev) {
                havePrev = true;
                continue;
            }
            f94b::Sample x;
            x.frame = n;
            x.key = 1;
            x.off[0] = (T[0] - nWu[0]) * kW * 0.5;
            x.off[1] = (T[1] - nWu[1]) * kH * 0.5;
            x.off93[0] = (T93[0] - nWu[0]) * kW * 0.5;
            x.off93[1] = (T93[1] - nWu[1]) * kH * 0.5;
            x.ref93 = true;
            x.refKey = 45;
            tr2.add(x, sec2);
        }
        check(sec2.steps == 6 && sec2.step93Max < 0.5 && sec2.err93Max < 1.0 && sec2.switches == 0 && sec2.fastN == 0,
              "F94b: with a steady 2-frame lag and a steady turn F93c's frame-old correction is within 1 px (the measure is not biased against it); no head speed: no fast draw");
    }
    // ---- two dots nearly equidistant from the label: F93c's nearest flips between them, the label jumps by the difference of their corrections ----------------------
    {
        const View v(0.0, 0.0);   // the head still: every correction is the dot's own layout offset
        double X0[3], nWu[2], nM0[2], nMP[2], T[2];
        layout(0.0, X0);
        v.scene(kP, nWu);
        v.marker(X0, nM0);
        v.marker(kP, nMP);
        f94b::ownAnchorTarget(nWu, nM0, nMP, T);
        double aA[2], dA[2], aB[2], dB[2], nwB[2];
        v.marker(kP, aA);   // dot A: a plain marker at the label's anchor
        dA[0] = nWu[0] - aA[0];
        dA[1] = nWu[1] - aA[1];
        v.marker(kX0B, aB);   // dot B: its record 36 px above its anchor (the icon offset F93c counts as a correction)
        v.scene(kPB, nwB);
        dB[0] = nwB[0] - aB[0];
        dB[1] = nwB[1] - aB[1];
        const double diff = std::sqrt(std::pow((dA[0] - dB[0]) * kW * 0.5, 2) + std::pow((dA[1] - dB[1]) * kH * 0.5, 2));
        f94b::Tracker tr;
        f94b::Sec sec;
        for (uint32_t n = 100; n < 110; ++n) {
            f87::RefSet rs;
            // the label's distance to the two decides; the head's sway moves it across the tie every frame
            const double nudge = (n % 2) ? 1e-4 : -1e-4;
            const double atA[2] = {nM0[0] - 0.2 + nudge, nM0[1]}, atB[2] = {nM0[0] - 0.2 - nudge, nM0[1]};
            rs.add(n - 1, atA, dA, 45);
            rs.add(n - 1, atB, dB, 46);
            double d[2] = {}, dist = 0.0;
            uint32_t age = 0, key = 0;
            const bool got = rs.nearest(n, nM0, 1, d, &dist, &age, &key);
            const double T93[2] = {nM0[0] + d[0], nM0[1] + d[1]};
            f94b::Sample x;
            x.frame = n;
            x.key = 1;
            x.off[0] = (T[0] - nWu[0]) * kW * 0.5;
            x.off[1] = (T[1] - nWu[1]) * kH * 0.5;
            x.off93[0] = (T93[0] - nWu[0]) * kW * 0.5;
            x.off93[1] = (T93[1] - nWu[1]) * kH * 0.5;
            x.ref93 = got;
            x.refKey = key;
            x.headDps = 2.0;
            tr.add(x, sec);
        }
        check(diff > 30.0 && sec.steps == 9 && sec.switches == 9 && std::fabs(sec.step93Max - diff) < 1e-6 && sec.stepMax < 1e-9 && sec.fastN == 0,
              "F94b: with the head still and two dots near the tie (one whose record sits 36 px off its anchor), F93c's rule jumps the label by their corrections' "
              "difference at every switch; the label on its own anchor does not move");
    }
    // ---- the tracker: steps only between consecutive frames of one key; a second draw in a frame is not a step; keys kept apart; the oldest slot reused ----------------
    {
        f94b::Tracker tr;
        f94b::Sec sec;
        auto add = [&](uint32_t frame, uint32_t key, double off, bool ref, uint32_t refKey) {
            f94b::Sample x;
            x.frame = frame;
            x.key = key;
            x.off[0] = off;
            x.off93[0] = off;
            x.ref93 = ref;
            x.refKey = refKey;
            tr.add(x, sec);
        };
        add(10, 1, 0.0, true, 45);
        add(10, 1, 9.0, true, 46);    // the same frame: no step, no switch, not kept
        add(11, 1, 1.0, true, 45);    // step 1
        add(11, 3, 50.0, false, 0);   // another key: its first frame
        add(13, 1, 4.0, true, 45);    // a gap: no step
        add(12, 3, 52.0, true, 47);   // key 3 again: step 2, a switch (none -> a reference)
        const bool basic = sec.n == 6 && sec.steps == 2 && std::fabs(sec.stepSum - 3.0) < 1e-12 && sec.stepMax == 2.0 && sec.switches == 1;
        f94b::Tracker full;
        f94b::Sec s2;
        for (uint32_t k = 0; k < f94b::Tracker::kSlots + 1; ++k) {
            f94b::Sample x;
            x.frame = 20 + k;
            x.key = 100 + k;
            full.add(x, s2);
        }
        bool oldestGone = true, newestKept = false;
        for (const auto& q : full.s) {
            if (q.used && q.key == 100) oldestGone = false;
            if (q.used && q.key == 100 + f94b::Tracker::kSlots) newestKept = true;
        }
        check(basic && oldestGone && newestKept && s2.steps == 0,
              "F94b: a step is a key's change between consecutive frames (a second draw in a frame and a gap are none); a switch is another reference or none; the oldest key's "
              "slot goes to a new key");
    }
    // ---- the anchor's own move in the world is measured over the same steps ------------------------------------------------------------------------------------------
    {
        f94b::Tracker tr;
        f94b::Sec sec;
        f94b::Sample x;
        x.key = 1;
        for (uint32_t n = 0; n < 4; ++n) {
            x.frame = 30 + n;
            x.p[0] = 0.1 + 0.002 * n;   // 2 mm a frame
            x.p[1] = 0.05;
            x.p[2] = 1.9;
            tr.add(x, sec);
        }
        check(sec.steps == 3 && std::fabs(sec.pStepMax - 2.0) < 1e-9 && std::fabs(sec.pStepSum - 6.0) < 1e-9,
              "F94b: an anchor that moves 2 mm a frame in the world reads 2 mm a step (a label whose anchor moves is told apart from one placed wrong)");
    }
    // ---- the two eyes: a label laid out in its anchor's plane has its anchor's depth, one 20 cm nearer does not --------------------------------------------------------
    {
        const double ipd = 0.064;
        const View l(0.0, 0.0, -ipd / 2), r(0.0, 0.0, ipd / 2);
        double X0[3];
        layout(0.0, X0);
        const double Xnear[3] = {X0[0], X0[1], X0[2] - 0.20};
        double off[2][2], offNear[2][2];
        const View* eyes[2] = {&l, &r};
        for (int e = 0; e < 2; ++e) {
            double nWu[2], nM0[2], nMP[2], nN[2], T[2], Tn[2];
            eyes[e]->scene(kP, nWu);
            eyes[e]->marker(X0, nM0);
            eyes[e]->marker(Xnear, nN);
            eyes[e]->marker(kP, nMP);
            f94b::ownAnchorTarget(nWu, nM0, nMP, T);
            f94b::ownAnchorTarget(nWu, nN, nMP, Tn);
            off[e][0] = (T[0] - nWu[0]) * kW * 0.5;
            off[e][1] = (T[1] - nWu[1]) * kH * 0.5;
            offNear[e][0] = (Tn[0] - nWu[0]) * kW * 0.5;
            offNear[e][1] = (Tn[1] - nWu[1]) * kH * 0.5;
        }
        f94b::EyeJoin join;
        f94b::Sec s0, s1;
        double diff[2] = {};
        const bool first = join.post(0, 1, 50, off[0], diff);          // eye 0 first: nothing to join yet
        f94b::eyeJoinTally(first, diff, s0);
        const bool second = join.post(1, 1, 50, off[1], diff);         // eye 1 joins eye 0's frame 50
        f94b::eyeJoinTally(second, diff, s1);
        const bool other = join.post(1, 3, 50, off[1], diff);          // another key: no join
        const bool late = join.post(1, 1, 51, off[1], diff);           // eye 0 has not drawn frame 51
        const bool back = join.post(0, 1, 51, offNear[0], diff);       // eye 0 joins eye 1's frame 51 (whichever eye comes first)
        f94b::Sec sn;
        double dn[2] = {};
        f94b::EyeJoin jn;
        jn.post(0, 1, 60, offNear[0], dn);
        f94b::eyeJoinTally(jn.post(1, 1, 60, offNear[1], dn), dn, sn);
        check(!first && second && !other && !late && back && s0.eyeN == 0 && s1.eyeN == 1 && s1.eyeMax < 0.5 && sn.eyeN == 1 && sn.eyeMax > 3.0,
              "F94b: the eyes' difference of the label's offset from its anchor is under 0.5 px for a record in its anchor's plane and over 3 px for one 20 cm nearer "
              "(64 mm apart, 1.9 m); the join takes the same key in the same frame, whichever eye drew first");
    }
    // ---- F93c's references carry their key (the switch count reads it) ---------------------------------------------------------------------------------------------
    {
        f87::RefSet rs;
        const double a[2] = {0.1, 0.1}, d[2] = {0.01, 0.02}, b[2] = {0.5, 0.5}, e[2] = {0.03, 0.04};
        rs.add(7, a, d, 45);
        rs.add(7, b, e, 46);
        double got[2] = {}, dist = 0.0;
        uint32_t age = 9, key = 0;
        const double at[2] = {0.45, 0.5};
        const bool ok = rs.nearest(7, at, 1, got, &dist, &age, &key) && key == 46 && age == 0 && got[0] == e[0];
        got[0] = 0.0;
        const bool keyless = rs.nearest(7, at, 1, got, &dist, &age) && got[0] == e[0];
        check(ok && keyless, "F94b: F93c's nearest reference names its key; a caller that asks for no key gets the correction as before");
    }
}

}  // namespace f94btest
