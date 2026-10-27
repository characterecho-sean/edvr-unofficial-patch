// F120 checks (native on-foot VR): the panel cursor stays on the panel when the head turns (native_on_foot_f120.h). Header only, run by native_frame_test
// (run120). A head (the eye's view rows E) and a HUD frame (the HRC matrix's rows H, scaled as the game scales them) as orientations, a sprite 0.65 m
// ahead in the pick camera's frame, and the tracker the placement hook uses: the reference, the correction M = E Ht H0 E0t, the refusals, the mouse-still
// drift and the words about the HUD frame.
#pragma once

#include "../../src/d3d11/native_on_foot_f120.h"
#include "f117_checks.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace f120test {

namespace w = edvr::nofoot::f120;
namespace z = edvr::nofoot::f117;

constexpr double kPi = 3.14159265358979323846;

inline w::Mat rotX(double deg) {
    const double c = std::cos(deg * kPi / 180.0), s = std::sin(deg * kPi / 180.0);
    w::Mat r = w::identity();
    r.m[4] = c;
    r.m[5] = -s;
    r.m[7] = s;
    r.m[8] = c;
    return r;
}
inline w::Mat rotY(double deg) {
    const double c = std::cos(deg * kPi / 180.0), s = std::sin(deg * kPi / 180.0);
    w::Mat r = w::identity();
    r.m[0] = c;
    r.m[2] = s;
    r.m[6] = -s;
    r.m[8] = c;
    return r;
}
inline w::Mat rotZ(double deg) {
    const double c = std::cos(deg * kPi / 180.0), s = std::sin(deg * kPi / 180.0);
    w::Mat r = w::identity();
    r.m[0] = c;
    r.m[1] = -s;
    r.m[3] = s;
    r.m[4] = c;
    return r;
}

// A frame whose axes in the world are the columns of q: its rows (axes) are the rows of qt, scaled (the HRC matrix carries a scale of 0.052).
inline void rows(const w::Mat& q, double scale, double out[9]) {
    const w::Mat t = w::transpose(q);
    for (int i = 0; i < 9; ++i) out[i] = t.m[i] * scale;
}

inline w::Input mk(uint32_t frame, const w::Mat& qe, const w::Mat& qh, const double p[3], double u = 0.4, double v = 0.6) {
    w::Input in;
    in.frame = frame;
    in.pinned = in.screen = in.haveEye = in.haveHrc = true;
    in.follow = true;
    rows(qe, 1.0, in.E);
    rows(qh, 0.052, in.H);
    in.u = u;
    in.v = v;
    in.key = 0x1000;
    for (int k = 0; k < 3; ++k) in.p[k] = p[k];
    return in;
}

inline double dist3(const double a[3], const double b[3]) {
    return std::sqrt((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2]));
}
// The written point as a world direction: the eye's frame to the world, Et p' = Q p'.
inline void world(const w::Mat& qe, const double p[3], double out[3]) { w::apply(qe, p, out); }

template <class Check>
inline void run120(Check&& check) {
    const double p[3] = {-0.02, 0.05, 0.65};
    const w::Mat qh0 = w::mul(rotY(15.0), rotX(-5.0));   // a HUD frame turned a little off the head's, and not the identity
    const w::Mat qe0 = w::identity();
    {   // nothing moved: M = I, every placement leaves the point alone, one reference
        w::Tracker t;
        bool same = true, noChange = true;
        for (uint32_t f = 100; f < 111; ++f) {
            const w::Out o = t.place(mk(f, qe0, qh0, p));
            same = same && o.why == w::Why::kApplied && dist3(o.target, p) < 1e-12;
            noChange = noChange && !o.changes;
        }
        check(same && noChange && t.sec.refs == 1 && t.sec.corrected == 11 && t.sec.screen == 11,
              "F120: nothing moved gives M = I: the point is left alone on all 11 placements, one reference taken, the write skipped");
    }
    {   // a head turn with the HUD fixed: the written sprite keeps its world point (the HUD's), whatever the HUD's own orientation
        w::Tracker t;
        t.place(mk(100, qe0, qh0, p));
        double worst = 0.0, moved = 1e9;
        for (int i = 1; i <= 8; ++i) {
            const w::Mat qe = w::mul(rotY(5.0 * i), w::mul(rotX(-2.0 * i), qe0));
            const w::Out o = t.place(mk(100 + static_cast<uint32_t>(i), qe, qh0, p));
            double w0[3], w1[3];
            world(qe0, p, w0);
            world(qe, o.target, w1);
            const double d = dist3(w0, w1);
            if (d > worst) worst = d;
            const double m = dist3(o.target, p);
            if (m < moved) moved = m;
        }
        check(worst < 1e-9 && moved > 0.04,
              "F120: a head turn of 5 to 40 deg with the HUD fixed keeps the written sprite's world point (within 1e-9) and moves it in the eye's frame by more than 4 cm");
    }
    {   // the head and the HUD turned together: M = I
        w::Tracker t;
        t.place(mk(100, qe0, qh0, p));
        bool same = true;
        for (int i = 1; i <= 8; ++i) {
            const w::Mat q = w::mul(rotY(7.0 * i), rotZ(3.0 * i));
            const w::Out o = t.place(mk(100 + static_cast<uint32_t>(i), w::mul(q, qe0), w::mul(q, qh0), p));
            same = same && o.why == w::Why::kApplied && dist3(o.target, p) < 1e-9 && !o.changes;
        }
        check(same, "F120: the head and the HUD turning together give M = I (the point is left alone within 1e-9)");
    }
    {   // it composes with F117: the panel point first, then M
        const std::vector<z::Tri> turned = f117test::mesh(true);
        float s[3];
        f117test::spriteAt(turned, 0.7, 0.2, s);
        const z::Decision d = z::decide(turned.data(), turned.size(), 0.7, 0.2, s, z::Mode::kPanel);
        double sd[3] = {s[0], s[1], s[2]};
        w::Tracker t;
        t.place(mk(100, qe0, qh0, d.target));
        const w::Mat qe = w::mul(rotY(20.0), qe0);
        const w::Out o = t.place(mk(101, qe, qh0, d.target));
        w::Mat e{}, h{}, e0{};
        double raw[9];
        rows(qe, 1.0, raw);
        w::unitRows(raw, e, nullptr);
        rows(qh0, 1.0, raw);
        w::unitRows(raw, h, nullptr);
        rows(qe0, 1.0, raw);
        w::unitRows(raw, e0, nullptr);
        const w::Mat m = w::mul(w::mul(e, w::transpose(h)), w::mul(h, w::transpose(e0)));   // E Ht H0 E0t
        double want[3], wrongOrder[3], fin[3];
        w::apply(m, d.target, want);
        w::apply(m, sd, wrongOrder);
        const bool wrote = w::finalTarget(d.why == z::Why::kMoved, d.target, o, fin);
        w::Tracker head;
        head.place(mk(100, qe0, qh0, d.target));
        w::Input hin = mk(101, qe, qh0, d.target);
        hin.follow = false;
        const w::Out oh = head.place(hin);
        double fin2[3];
        const bool wroteHead = w::finalTarget(true, d.target, oh, fin2);
        check(d.why == z::Why::kMoved && o.why == w::Why::kApplied && dist3(o.target, want) < 1e-9 && dist3(o.target, wrongOrder) > 0.001 && wrote &&
                  dist3(fin, want) < 1e-9 && oh.why == w::Why::kHead && wroteHead && dist3(fin2, d.target) < 1e-12,
              "F120: it composes with F117: M is applied to F117's panel point (not to the game's sprite), one final write holds both; with the setting head the "
              "final write is F117's point alone");
    }
    {   // the reference drops after 30 placement-free frames, not before
        w::Tracker kept, dropped;
        const w::Mat qe = w::mul(rotY(20.0), qe0);
        kept.place(mk(100, qe0, qh0, p));
        kept.place(mk(105, qe0, qh0, p));
        dropped.place(mk(100, qe0, qh0, p));
        dropped.place(mk(105, qe0, qh0, p));
        const w::Out ok = kept.place(mk(135, qe, qh0, p));      // 29 placement-free frames: the reference stays
        const w::Out od = dropped.place(mk(136, qe, qh0, p));   // 30 placement-free frames: a new reference, at the turned head
        const w::Out o2 = dropped.place(mk(137, w::mul(rotY(30.0), qe0), qh0, p));
        check(kept.sec.refs == 1 && ok.changes && dist3(ok.target, p) > 0.1 && dropped.sec.refs == 2 && !od.changes && dist3(od.target, p) < 1e-12 &&
                  dist3(o2.target, p) > 0.05,
              "F120: the reference holds across 29 placement-free frames and drops after 30: the placement after them takes a new reference (M = I there), "
              "and the next turn is judged from it");
    }
    {   // the frame counter starting over drops the reference; a second panel in the same frame keeps it
        w::Tracker t;
        const w::Mat qe = w::mul(rotY(20.0), qe0);
        t.place(mk(500, qe0, qh0, p));
        w::Input other = mk(500, qe, qh0, p);
        other.key = 0x2000;
        const w::Out same = t.place(other);
        const w::Out back = t.place(mk(10, qe, qh0, p));
        check(t.sec.refs == 2 && same.changes && !back.changes && dist3(back.target, p) < 1e-12,
              "F120: a second panel in the same frame keeps the reference; a frame counter that went backwards drops it");
    }
    {   // the screen-space test and the pin
        uint8_t bytes[16];
        std::memcpy(bytes, w::kOwnerPrologue, 16);
        const bool pinned = w::ownerPinned(bytes);
        bytes[9] ^= 0x01;
        const bool differs = !w::ownerPinned(bytes);
        check(pinned && differs && w::screenSpace(w::kOwnerRva, 2, 1) && !w::screenSpace(0x4526E40, 2, 1) && !w::screenSpace(w::kOwnerRva, 0, 1) &&
                  !w::screenSpace(w::kOwnerRva, 2, 0) && w::kOwnerRva == 0x47299F0 && w::kOffOwnerCount == 0x1B0 && w::kOffOwnerArray == 0x1B8 &&
                  w::kOffScreenFlag == 0x1C0,
              "F120: the screen-space test wants the pinned owner function, a count above 0 and the flag set; the pin is the first 16 bytes and one differing byte "
              "fails it");
    }
    {   // each refusal is its own count, none writes, and only a screen-space placement counts as one
        w::Tracker t;
        auto refused = [&](const w::Input& in, w::Why why) {
            const w::Out o = t.place(in);
            return o.why == why && !o.changes && t.sec.refused[static_cast<int>(why)] >= 1;
        };
        w::Input base = mk(100, qe0, qh0, p);
        w::Input flat = base, unpinned = base, notScreen = base, noEye = base, noHrc = base, degenerate = base, nonOrtho = base;
        flat.flat = true;
        unpinned.pinned = false;
        notScreen.screen = false;
        noEye.haveEye = false;
        noHrc.haveHrc = false;
        for (int k = 0; k < 3; ++k) degenerate.H[3 + k] = 0.0;   // a zero row
        nonOrtho.H[3] = 0.3 * 0.052;                              // the up row leaning 17 deg into the right row's side
        nonOrtho.H[0] = 0.052;
        nonOrtho.H[1] = 0.0;
        nonOrtho.H[2] = 0.0;
        nonOrtho.H[4] = 0.052;
        nonOrtho.H[5] = 0.0;
        const bool a = refused(flat, w::Why::kFlat), b = refused(unpinned, w::Why::kNotPinned), c = refused(notScreen, w::Why::kNotScreen);
        const uint32_t screenBefore = t.sec.screen;
        const bool d = refused(noEye, w::Why::kNoEye), e = refused(noHrc, w::Why::kNoHrc), f = refused(degenerate, w::Why::kDegenerate),
                   g = refused(nonOrtho, w::Why::kDegenerate);
        w::Input head = base;
        head.follow = false;
        const w::Out oh = t.place(head);
        check(a && b && c && screenBefore == 0 && d && e && f && g && t.sec.screen == 5 && oh.why == w::Why::kHead && t.sec.refused[static_cast<int>(w::Why::kHead)] == 1 &&
                  t.sec.corrected == 0 && t.sec.placements == 8 && t.sec.refs == 1,
              "F120: flat, unpinned, not screen-space, no eye view, no HRC matrix, a degenerate row (zero, or leaning) and the setting head each refuse and are "
              "counted apart; only the last five are screen-space placements; none corrected");
    }
    {   // a failed write moves the count from corrected to refused
        w::Tracker t;
        t.place(mk(100, qe0, qh0, p));
        const w::Out o = t.place(mk(101, w::mul(rotY(20.0), qe0), qh0, p));
        const uint32_t before = t.sec.corrected;
        t.writeFailed();
        check(o.changes && t.sec.corrected == before - 1 && t.sec.refused[static_cast<int>(w::Why::kWriteFailed)] == 1,
              "F120: a write that failed is counted as a refusal, not a correction");
    }
    {   // the drift: panel holds the cursor on the panel while the head turns with the mouse still, head lets it ride (about 11 cm per 10 deg at 0.65 m)
        const double ahead[3] = {0.0, 0.0, 0.65};
        auto run = [&](bool follow, bool hudWithHead, bool mouseMoves) {
            w::Tracker t;
            for (int i = 0; i <= 10; ++i) {
                const w::Mat q = rotY(static_cast<double>(i));
                w::Input in = mk(200 + static_cast<uint32_t>(i), w::mul(q, qe0), hudWithHead ? w::mul(q, qh0) : qh0, ahead, 0.4 + (mouseMoves ? 0.01 * i : 0.0), 0.6);
                in.follow = follow;
                t.place(in);
            }
            return t.sec;
        };
        const w::Sec onPanel = run(true, false, false), onHead = run(false, false, false), moving = run(true, false, true), together = run(true, true, false),
                     headTogether = run(false, true, false);
        check(onPanel.stillN == 10 && std::fabs(onPanel.stillTurn - 10.0) < 0.01 && onPanel.stillDrift < 1e-6 && onHead.stillN == 10 &&
                  std::fabs(onHead.stillDrift - 2.0 * 650.0 * std::sin(5.0 * kPi / 180.0)) < 0.5 && onHead.stillDrift > 100.0 && moving.stillN == 0 &&
                  moving.stillDrift == 0.0 && together.stillN == 10 && together.stillTurn < 1e-4 && together.stillDrift < 1e-6 && headTogether.stillDrift < 1e-6,
              "F120: with the mouse still and the head turned 10 deg against the HUD, panel's drift is under 1e-6 mm and head's is 113 mm (2 x 650 mm x sin 5 deg); "
              "a moving mouse starts no run; the head turning with the HUD drifts nothing under either setting");
        // the words about the HUD frame
        const std::string still = w::frameText(onPanel), with = w::frameText(together), none = w::frameText(w::Sec{});
        w::Tracker rest;
        for (uint32_t f = 300; f < 305; ++f) rest.place(mk(f, qe0, qh0, ahead));
        check(still.find("holds still") != std::string::npos && with.find("not in the HRC frame") != std::string::npos && with.find("near zero") != std::string::npos &&
                  std::string(w::frameText(rest.sec)) == "the head did not turn" && none == "no screen-space panel" &&
                  std::fabs(onPanel.turnE - 10.0) < 0.01 && onPanel.turnH < 1e-4 && std::fabs(onPanel.turnEH - 10.0) < 0.01 && together.turnEH < 1e-4 &&
                  std::fabs(together.turnE - 10.0) < 0.01 && std::fabs(together.turnH - 10.0) < 0.01,
              "F120: the line says whether the HUD frame held still (the correction follows the head) or turned with the head (E Ht constant: the drawn panel is not "
              "in the HRC frame, the correction near zero), or the head did not turn");
    }
    {   // E Ht's yaw, pitch and roll recover the angles it was built from
        const w::Mat q = w::mul(rotY(23.0), w::mul(rotX(-11.0), rotZ(7.0)));
        double yaw = 0.0, pitch = 0.0, roll = 0.0;
        w::ypr(q, &yaw, &pitch, &roll);
        check(std::fabs(yaw - 23.0) < 1e-9 && std::fabs(pitch + 11.0) < 1e-9 && std::fabs(roll - 7.0) < 1e-9 && std::fabs(w::det(q) - 1.0) < 1e-12 &&
                  std::fabs(w::angleDeg(q, w::identity()) - w::angleDeg(w::identity(), q)) < 1e-9,
              "F120: yaw, pitch and roll read back from E Ht (23, -11, 7 deg), the determinant 1");
    }
    {   // the merged totals keep the largest readings
        w::Sec a, b;
        a.placements = 3;
        a.stillDrift = 2.0;
        a.turnEH = 4.0;
        b.placements = 5;
        b.stillDrift = 7.0;
        b.turnEH = 1.0;
        b.corrected = 5;
        a.merge(b);
        check(a.placements == 8 && a.corrected == 5 && a.stillDrift == 7.0 && a.turnEH == 4.0, "F120: merged seconds add their counts and keep the largest readings");
    }
}

}  // namespace f120test
