// S0 checks (scope zoom, measure only), runS0 below. Header only, run by native_frame_test.
//
// S0 (src\d3d11\scope_zoom_s0.h): the pure parts of the first scope zoom flight. clipZoom reads the projection scale of an eye's scene rows; PressTracker finds the hip, the settled value
// and the ramp of one scalar across a press of the sights (a camera's FOV in flat, the readout's two FOVs and the eyes' p11 in VR); CameraTable holds the cameras the view-constant
// refresh hook saw; probePoints and lensEye put the lens centre in an eye's pixels and the pixel probe's four points about it; the line formats are the DLL's own, which
// tools\edvr_log.py renders its --health fixtures from.
#pragma once

#include "../../src/d3d11/scope_zoom_s0.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>

namespace s0test {

namespace sz = edvr::scopezoom;

// The rows of a camera at c with axes r, u, f (unit, orthogonal, world) and the projection p00, p11, offX, offY, in worldClip's layout (clip_j = x rows[j] + y rows[4 + j] + z rows[8 + j]
// + rows[12 + j]): clip x = p00 (v . r) + offX (v . f), clip y = p11 (v . u) + offY (v . f), clip z = a (v . f), clip w = v . f, v = p - c. scale multiplies all sixteen rows.
inline void makeRows(const double c[3], const double r[3], const double u[3], const double f[3], double p00, double p11, double offX, double offY, double scale, float rows[16]) {
    for (int i = 0; i < 3; ++i) {
        rows[4 * i + 0] = static_cast<float>(scale * (p00 * r[i] + offX * f[i]));
        rows[4 * i + 1] = static_cast<float>(scale * (p11 * u[i] + offY * f[i]));
        rows[4 * i + 2] = static_cast<float>(scale * 1.001 * f[i]);
        rows[4 * i + 3] = static_cast<float>(scale * f[i]);
    }
    for (int j = 0; j < 4; ++j) {
        double t = 0.0;
        for (int i = 0; i < 3; ++i) t -= c[i] * rows[4 * i + j];
        rows[12 + j] = static_cast<float>(t);
    }
}
// Axes from yaw (about up), pitch (about right) and roll (about forward), radians: forward starts at +z, right at +x, up at +y.
inline void makeAxes(double yaw, double pitch, double roll, double r[3], double u[3], double f[3]) {
    const double cy = std::cos(yaw), sy = std::sin(yaw), cp = std::cos(pitch), sp = std::sin(pitch), cr = std::cos(roll), sr = std::sin(roll);
    // forward then right then up, composed as yaw * pitch * roll on the basis
    const double f0[3] = {0, 0, 1}, r0[3] = {1, 0, 0}, u0[3] = {0, 1, 0};
    const auto rotate = [&](const double v[3], double out[3]) {
        // roll about z
        const double a[3] = {v[0] * cr - v[1] * sr, v[0] * sr + v[1] * cr, v[2]};
        // pitch about x
        const double b[3] = {a[0], a[1] * cp - a[2] * sp, a[1] * sp + a[2] * cp};
        // yaw about y
        out[0] = b[0] * cy + b[2] * sy;
        out[1] = b[1];
        out[2] = -b[0] * sy + b[2] * cy;
    };
    rotate(r0, r);
    rotate(u0, u);
    rotate(f0, f);
}

// One sample of a source at 90 Hz: value(t) and the flag(t).
template <class V, class A>
inline unsigned driveTracker(sz::PressTracker& tr, double t0, double t1, V value, A flag, sz::PressResult* last) {
    unsigned ends = 0;
    const long k0 = std::lround(t0 * 90.0), k1 = std::lround(t1 * 90.0);
    for (long k = k0; k < k1; ++k) {
        const double t = static_cast<double>(k) / 90.0;
        if (tr.feed(t, flag(t), value(t))) {
            ++ends;
            if (last) *last = tr.result();
        }
    }
    return ends;
}
// A value at hip, ramping linearly to ads over rampS from t0 and back from t1 over the same time: hip before, ads between, hip after.
struct Zoom {
    double hip, ads, t0, t1, rampS;
    double operator()(double t) const {
        if (t < t0) return hip;
        if (t < t0 + rampS) return hip + (ads - hip) * (t - t0) / rampS;
        if (t < t1) return ads;
        if (t < t1 + rampS) return ads + (hip - ads) * (t - t1) / rampS;
        return hip;
    }
};
struct Flag {
    double on, off;
    bool operator()(double t) const { return t >= on && t < off; }
};

inline bool near3(double a, double b, double tol) { return std::fabs(a - b) <= tol; }

template <class Check>
inline void runS0(Check&& check) {
    // ---- clipZoom ----
    {
        bool allOk = true;
        double worst = 0.0;
        const double fovs[] = {30.0, 60.0, 90.0, 104.0, 20.0};
        const double angles[][3] = {{0, 0, 0}, {0.7, 0.2, 0.1}, {-1.9, -0.4, 0.8}, {3.0, 1.2, -0.6}, {0.1, -1.4, 2.5}};
        const double cams[][3] = {{0, 0, 0}, {12.5, -3.0, 700.25}, {-1500.0, 30.0, 22.0}};
        for (double fov : fovs) {
            for (const auto& a : angles) {
                for (const auto& c : cams) {
                    for (double scale : {1.0, 3.7, 0.02}) {
                        const double p11 = 1.0 / std::tan(0.5 * fov * sz::kPi / 180.0), p00 = p11 / 1.1;
                        const double offX = 0.07, offY = -0.04;
                        double r[3], u[3], f[3];
                        makeAxes(a[0], a[1], a[2], r, u, f);
                        float rows[16];
                        makeRows(c, r, u, f, p00, p11, offX, offY, scale, rows);
                        const sz::ClipZoom z = sz::clipZoom(rows);
                        const double e = std::fmax(std::fmax(std::fabs(z.p00 - p00), std::fabs(z.p11 - p11)), std::fmax(std::fabs(z.offX - offX), std::fabs(z.offY - offY)));
                        worst = std::fmax(worst, e);
                        if (!z.ok || e > 1e-5 * std::fmax(1.0, p11)) allOk = false;
                    }
                }
            }
        }
        check(allOk, "S0 clipZoom: p00, p11 and the principal offset recovered to 1e-5 from five FOVs, five orientations (yaw, pitch, roll), three places and three scales of all sixteen rows");
        // a centred frustum: the half angle from p11
        double r[3], u[3], f[3];
        const double c0[3] = {0, 0, 0};
        makeAxes(0, 0, 0, r, u, f);
        float rows[16];
        makeRows(c0, r, u, f, 0.5, 1.0, 0.0, 0.0, 1.0, rows);
        const sz::ClipZoom z = sz::clipZoom(rows);
        check(z.ok && near3(sz::fovFromP11(z.p11), 2.0 * std::atan(1.0), 1e-6) && near3(z.offX, 0.0, 1e-9), "S0 clipZoom: a centred p11 of 1 is a 90 degree vertical FOV");
        float tg[4];
        check(sz::tangentsOf(z, tg) && near3(tg[0], -2.0, 1e-5) && near3(tg[1], 2.0, 1e-5) && near3(tg[2], -1.0, 1e-5) && near3(tg[3], 1.0, 1e-5),
              "S0 clipZoom: the tangents {L, R, D, U} of p00 0.5, p11 1 are -2, 2, -1, 1");
        makeRows(c0, r, u, f, 0.5, 1.0, 0.2, -0.1, 1.0, rows);
        const sz::ClipZoom zo = sz::clipZoom(rows);
        check(sz::tangentsOf(zo, tg) && near3(tg[0], -2.4, 1e-5) && near3(tg[1], 1.6, 1e-5) && near3(tg[2], -0.9, 1e-5) && near3(tg[3], 1.1, 1e-5),
              "S0 clipZoom: an off-centre projection moves the tangents (offX 0.2, offY -0.1)");
        float bad[16] = {};
        check(!sz::clipZoom(bad).ok && !sz::clipZoom(nullptr).ok, "S0 clipZoom: a zero matrix and a null pointer are not usable");
        float nan16[16];
        std::memcpy(nan16, rows, sizeof(nan16));
        nan16[3] = std::nanf("");
        check(!sz::clipZoom(nan16).ok, "S0 clipZoom: a matrix with a lane that is not a number is not usable");
        float flat[16];
        std::memcpy(flat, rows, sizeof(flat));
        flat[0] = flat[4] = flat[8] = 0.0f;   // no x scale
        check(!sz::clipZoom(flat).ok, "S0 clipZoom: no horizontal scale is not usable");
        check(near3(sz::zoomFromP11(1.0, 1.8), 1.8, 1e-12) && near3(sz::zoomFromFov(1.0, 2.0 * std::atan(std::tan(0.5) / 1.8)), 1.8, 1e-9) &&
                  std::isnan(sz::zoomFromFov(sz::kNaN, 1.0)) && std::isnan(sz::zoomFromFov(1.0, 0.0)) && std::isnan(sz::zoomFromFov(1.0, 3.5)) && std::isnan(sz::zoomFromP11(0.0, 1.0)),
              "S0 zoom: p11_ads / p11_hip and tan(hip / 2) / tan(ads / 2) agree; a FOV that is not in (0, pi) has no zoom");
    }

    // ---- PressTracker ----
    {
        // (a) the spec's scoped press: hip 1.0 rad stable, a ramp to 0.55 over 0.3 s from t = 10.0, the flag late at 10.2, held to 12.0, released
        sz::PressTracker tr;
        sz::PressResult got;
        const Zoom zoom{1.0, 0.55, 10.0, 12.0, 0.3};
        const unsigned ends = driveTracker(tr, 0.0, 16.0, zoom, Flag{10.2, 12.0}, &got);
        check(ends == 1 && got.valid && near3(got.hip, 1.0, 1e-9) && near3(got.ads, 0.55, 1e-9) && got.settled && near3(got.rampS, 0.3, 0.02) && near3(got.holdS, 1.8, 0.02),
              "S0 press (a): a scoped press with the flag late: hip 1.0, settled 0.55, ramp about 0.3 s, held 1.8 s");
        check(near3(sz::zoomFromFov(got.hip, got.ads), std::tan(0.5) / std::tan(0.275), 1e-9) && got.minV <= 0.5501 && got.maxV <= 0.71 && got.maxV > 0.55 && got.samples > 100 && got.missing == 0 &&
                  !got.hipFromBefore,
              "S0 press (a): the zoom is tan(hip / 2) / tan(ads / 2); the press saw the ramp's tail, no sample was missing");
        check(near3(tr.hip(), 1.0, 1e-9), "S0 press (a): after the release the hip is the hip again");

        // (b) an unscoped press: nothing changes
        sz::PressTracker tb;
        sz::PressResult gb;
        const unsigned eb = driveTracker(tb, 0.0, 14.0, Zoom{1.0, 1.0, 10.0, 12.0, 0.3}, Flag{10.0, 12.0}, &gb);
        check(eb == 1 && gb.valid && near3(gb.hip, 1.0, 1e-9) && near3(gb.ads, 1.0, 1e-9) && gb.settled && gb.rampS == 0.0 && near3(gb.holdS, 2.0, 0.02) && near3(sz::zoomFromFov(gb.hip, gb.ads), 1.0, 1e-12),
              "S0 press (b): an unscoped press settles 0.6 s after its flag with M = 1 and no ramp");

        // (c) a value that never settles: 0.2 % a frame down for the whole press
        sz::PressTracker tc;
        sz::PressResult gc;
        const auto drift = [](double t) { return t < 10.0 ? 1.0 : 1.0 * std::pow(0.998, (t - 10.0) * 90.0); };
        const unsigned ec = driveTracker(tc, 0.0, 14.0, drift, Flag{10.1, 11.5}, &gc);
        check(ec == 1 && gc.valid && !gc.settled && near3(gc.hip, 1.0, 1e-9) && gc.ads == gc.minV && gc.ads < 0.9 && gc.rampS == 0.0,
              "S0 press (c): a value that never holds within 0.1 % is not settled; its value is the last one, which is its minimum");

        // (d) the sights up from the first frame: no hip
        sz::PressTracker td;
        sz::PressResult gd;
        const unsigned ed = driveTracker(td, 0.0, 3.0, [](double) { return 0.55; }, Flag{0.0, 2.0}, &gd);
        check(ed == 1 && gd.valid && std::isnan(gd.hip) && gd.settled && near3(gd.ads, 0.55, 1e-12) && std::isnan(sz::zoomFromFov(gd.hip, gd.ads)),
              "S0 press (d): the sights up from the first frame: no hip, settled, and no zoom");

        // (e) the flag arrives after the ramp has finished (flat: Status.json about once a second)
        sz::PressTracker te(sz::kFlatLagGuardS), te0;
        sz::PressResult ge, ge0;
        const Zoom lateZoom{1.0, 0.55, 10.0, 14.0, 0.3};
        driveTracker(te, 0.0, 20.0, lateZoom, Flag{11.3, 14.1}, &ge);
        driveTracker(te0, 0.0, 20.0, lateZoom, Flag{11.3, 14.1}, &ge0);
        check(ge.valid && near3(ge.hip, 1.0, 1e-9) && ge.hipFromBefore && near3(ge.ads, 0.55, 1e-9) && ge.settled && near3(ge.rampS, 0.3, 0.02),
              "S0 press (e): the flag 1.0 s after the ramp ended: the lag guard keeps the hip from before the ramp, and the ramp is the camera's own");
        check(ge0.valid && near3(ge0.hip, 0.55, 1e-9) && !ge0.hipFromBefore && near3(sz::zoomFromFov(ge0.hip, ge0.ads), 1.0, 1e-9),
              "S0 press (e): without the guard the same flag reads the ramp's plateau as the hip (M = 1); this is why the flat trackers carry it");

        // (f) a quick second press: the hip is the hip again, not the first press's plateau
        sz::PressTracker tf(sz::kFlatLagGuardS);
        sz::PressResult g1, g2;
        const auto twoZooms = [](double t) {
            const Zoom a{1.0, 0.55, 10.0, 12.0, 0.3};
            const Zoom b{1.0, 0.55, 14.0, 16.0, 0.3};
            return t < 13.0 ? a(t) : b(t);
        };
        const unsigned e1 = driveTracker(tf, 0.0, 13.0, twoZooms, Flag{10.2, 12.0}, &g1);
        const unsigned e2 = driveTracker(tf, 13.0, 20.0, twoZooms, Flag{14.0, 16.0}, &g2);
        check(e1 == 1 && e2 == 1 && near3(g1.hip, 1.0, 1e-9) && near3(g2.hip, 1.0, 1e-9) && !g2.hipFromBefore && near3(g2.ads, 0.55, 1e-9) && near3(g2.rampS, 0.3, 0.02),
              "S0 press (f): a second press 2 s after the release, its flag on time: hip 1.0 (the first press's plateau never became the hip), ramp 0.3 s");

        // (g) flush ends an open press once; a tracker with no press has nothing to flush
        sz::PressTracker tg;
        driveTracker(tg, 0.0, 10.6, Zoom{1.0, 0.55, 10.0, 99.0, 0.3}, Flag{10.0, 99.0}, nullptr);
        const bool open = tg.inPress();
        const bool flushed = tg.flush(10.6);
        check(open && flushed && tg.result().valid && tg.result().settled == false && !tg.inPress() && !tg.flush(10.7) && near3(tg.result().holdS, 0.6, 0.02),
              "S0 press (g): flush() ends an open press (here before it settled) once and says so");

        // (h) frames with no value count as missing and do not end or break the press
        sz::PressTracker th;
        sz::PressResult gh;
        const auto holes = [](double t) { return (t > 10.5 && t < 10.7) ? sz::kNaN : 1.0; };
        driveTracker(th, 0.0, 13.0, holes, Flag{10.0, 12.0}, &gh);
        check(gh.valid && gh.missing >= 15 && gh.missing <= 20 && gh.settled && near3(gh.ads, 1.0, 1e-12), "S0 press (h): frames with no value are counted as missing; the press still settles");

        // (i) a source that never has a value: a press with no hip and no ads
        sz::PressTracker ti;
        sz::PressResult gi;
        driveTracker(ti, 0.0, 5.0, [](double) { return sz::kNaN; }, Flag{1.0, 3.0}, &gi);
        check(gi.valid && std::isnan(gi.hip) && std::isnan(gi.ads) && !gi.settled && gi.samples == 0 && gi.missing > 100, "S0 press (i): a source with no value at all gives a press of nothing, counted");
    }

    // ---- CameraTable ----
    {
        sz::CameraTable t;
        sz::CameraTable::Entry e[sz::CameraTable::kSlots];
        t.note(0, 3, 1.0f, 1.7f, 100);
        check(t.snapshot(e, 16) == 0, "S0 cameras: a null camera is not noted");
        t.note(0x1000, 3, 1.047f, 1.778f, 100);
        t.note(0x1000, 3, 0.6f, 1.778f, 110);
        t.note(0x2000, 4, 1.2f, 1.0f, 110);
        size_t n = t.snapshot(e, 16);
        const sz::CameraTable::Entry* a = nullptr;
        for (size_t i = 0; i < n; ++i) if (e[i].camera == 0x1000) a = &e[i];
        check(n == 2 && a && a->kind == 3 && a->fov == 0.6f && a->aspect == 1.778f && a->calls == 2 && a->ms == 110 && a->id != 0, "S0 cameras: a camera keeps one slot; its FOV, call count and time are the latest");
        // fill every slot at times 1000.., then one more with all of them fresh
        sz::CameraTable full;
        for (uintptr_t i = 0; i < sz::CameraTable::kSlots; ++i) full.note(0x10000 + i * 0x100, 3, 1.0f, 1.7f, 1000 + i * 10);
        full.note(0x90000, 3, 1.0f, 1.7f, 1500);
        check(full.drops() == 1 && full.snapshot(e, 16) == sz::CameraTable::kSlots, "S0 cameras: a camera no slot can take while every slot is fresh is counted, not stored");
        // the least recently noted slot (noted at 1000) is older than 2000 ms at 3100: replaced; the next at 3200 finds the oldest (1010) also stale
        full.note(0x90000, 3, 0.9f, 1.7f, 3100);
        n = full.snapshot(e, 16);
        bool oldGone = true, newIn = false;
        for (size_t i = 0; i < n; ++i) {
            if (e[i].camera == 0x10000) oldGone = false;
            if (e[i].camera == 0x90000) newIn = true;
        }
        check(n == sz::CameraTable::kSlots && oldGone && newIn && full.drops() == 1, "S0 cameras: a slot not noted for 2 s is replaced, the least recently noted first");
        full.note(0xA0000, 3, 0.9f, 1.7f, 3105);
        full.note(0xB0000, 3, 0.9f, 1.7f, 3106);
        // slots noted at 1010.. are stale by 3105 (> 2000 ms): 0xA0000 replaces the oldest, which is 1010; 3106: the next oldest is 1020; both stale, so both go in
        n = full.snapshot(e, 16);
        bool a0 = false, b0 = false;
        for (size_t i = 0; i < n; ++i) { if (e[i].camera == 0xA0000) a0 = true; if (e[i].camera == 0xB0000) b0 = true; }
        check(a0 && b0 && full.drops() == 1, "S0 cameras: stale slots are replaced one camera at a time, oldest first");
        // a slot noted just now is not stale
        sz::CameraTable fresh;
        for (uintptr_t i = 0; i < sz::CameraTable::kSlots; ++i) fresh.note(0x20000 + i * 0x100, 3, 1.0f, 1.7f, 5000);
        fresh.note(0x80000, 3, 1.0f, 1.7f, 6999);
        check(fresh.drops() == 1, "S0 cameras: slots 1999 ms old are not replaced");
        fresh.note(0x80000, 3, 1.0f, 1.7f, 7001);
        check(fresh.drops() == 1 && fresh.snapshot(e, 16) == sz::CameraTable::kSlots, "S0 cameras: slots 2001 ms old are");
        fresh.clear();
        check(fresh.snapshot(e, 16) == 0 && fresh.drops() == 0, "S0 cameras: clear empties the table");
        sz::g_cameraTable.clear();
        sz::g_cameraTable.note(0x4000, 3, 1.1f, 1.5f, 1);
        check(sz::g_cameraTable.snapshot(e, 16) == 1 && e[0].camera == 0x4000, "S0 cameras: the process-wide table is one inline object");
        sz::g_cameraTable.clear();
    }

    // ---- probePoints, lensEye ----
    {
        const float sym[4] = {-1.0f, 1.0f, -0.8f, 0.8f};
        const double c0[2] = {0.0, 0.0};
        sz::ProbeSet s;
        const bool ok = sz::probePoints(c0, sym, &s);
        const double a09 = std::tan(0.9 * sz::kPi / 180.0), a15 = std::tan(1.5 * sz::kPi / 180.0), a4 = std::tan(4.0 * sz::kPi / 180.0);
        check(ok && s.n == 4 && s.clamped == 0 && near3(s.pt[0].x, 0.5, 1e-6) && near3(s.pt[0].y, 0.5, 1e-6), "S0 probe points: a centred c is the middle of the target (0.5, 0.5)");
        check(near3(s.pt[1].x, 0.5, 1e-6) && near3(s.pt[1].y, 0.5 + 0.5 * a09 / 0.8, 1e-6) && s.pt[1].y > 0.5,
              "S0 probe points: 0.9 degrees straight below c (where the chevron sits) moves y toward the bottom of the target (y down) by tan(0.9 deg) and not x");
        check(near3(s.pt[2].x, 0.5, 1e-6) && near3(s.pt[2].y, 0.5 - 0.5 * a15 / 0.8, 1e-6) && s.pt[2].y < 0.5, "S0 probe points: 1.5 degrees above c moves y toward the top of the target");
        check(near3(s.pt[3].x, 0.5 + 0.5 * a4 / 1.0, 1e-6) && near3(s.pt[3].y, 0.5, 1e-6) && s.pt[3].x > 0.5, "S0 probe points: 4 degrees beside c moves x to the right by tan(4 deg) / (R - L) * 2 / 2 and not y");
        // off-centre c, asymmetric tangents: the plane angle from c's ray is exact
        const float asym[4] = {-1.1f, 0.9f, -0.7f, 0.9f};
        const double c1[2] = {0.3, -0.2};
        sz::ProbeSet o;
        double tc[2];
        sz::tangentOfNdc(c1, asym, tc);
        const bool ok1 = sz::probePoints(c1, asym, &o);
        double tp[3][2];
        for (int i = 1; i < 4; ++i) sz::tangentOfNdc(o.ndc[i], asym, tp[i - 1]);
        const double dx1 = (std::atan(tp[0][0]) - std::atan(tc[0])) * sz::kDeg, dy1 = (std::atan(tp[0][1]) - std::atan(tc[1])) * sz::kDeg;
        const double dx2 = (std::atan(tp[1][0]) - std::atan(tc[0])) * sz::kDeg, dy2 = (std::atan(tp[1][1]) - std::atan(tc[1])) * sz::kDeg;
        const double dx3 = (std::atan(tp[2][0]) - std::atan(tc[0])) * sz::kDeg, dy3 = (std::atan(tp[2][1]) - std::atan(tc[1])) * sz::kDeg;
        check(ok1 && near3(tc[0], 0.2, 1e-6) && near3(tc[1], -0.06, 1e-6) && near3(dx1, 0.0, 1e-4) && near3(dy1, -0.9, 1e-4) && near3(dx2, 0.0, 1e-4) && near3(dy2, 1.5, 1e-4) &&
                  near3(dx3, 4.0, 1e-4) && near3(dy3, 0.0, 1e-4) && o.clamped == 0,
              "S0 probe points: an off-centre c in an off-axis frustum: each point is the asked plane angle from c's ray, 0.9 below, 1.5 above and 4 beside");
        check(near3(o.pt[0].x, (1.0 + 0.3) * 0.5, 1e-6) && near3(o.pt[0].y, (1.0 + 0.2) * 0.5, 1e-6), "S0 probe points: c itself is (ndc + 1) / 2 in x and (1 - ndc) / 2 in y");
        // a clamp: c near the right edge, the 4 degree point beside it falls off the target
        const double edge[2] = {0.995, 0.0};
        sz::ProbeSet cl;
        const bool okc = sz::probePoints(edge, sym, &cl);
        check(okc && cl.clamped == 8u && cl.pt[3].x == 1.0f && cl.pt[0].x < 1.0f && cl.pt[3].y >= 0.0f && cl.pt[3].y <= 1.0f && cl.pt[2].x < 1.0f,
              "S0 probe points: a point past the target's edge is moved to it and named in the clamp mask (bit 3 here)");
        const double corner[2] = {-0.99, 0.99};
        sz::ProbeSet cc;
        check(sz::probePoints(corner, sym, &cc) && cc.clamped == 4u && cc.pt[2].y == 0.0f && cc.pt[3].x > 0.0f && cc.pt[1].y > 0.0f,
              "S0 probe points: the up-left corner clamps the point 1.5 degrees above c (y to 0) and no other: the one beside c moves away from the edge");
        const double nanC[2] = {sz::kNaN, 0.0};
        const float badTan[4] = {1.0f, -1.0f, 0.0f, 0.0f};
        check(!sz::probePoints(nanC, sym, &s) && !sz::probePoints(c0, badTan, &s) && !sz::probePoints(c0, nullptr, &s) && !sz::probePoints(c0, sym, nullptr),
              "S0 probe points: a c that is not a number, tangents that are not a frustum, a missing pointer: refused");
        // lensEye
        const sz::LensEye l = sz::lensEye(c0, sym, 2000, 1000);
        check(l.ok && near3(l.px[0], 1000.0, 1e-9) && near3(l.px[1], 500.0, 1e-9) && near3(l.offAxisDeg, 0.0, 1e-9), "S0 lens: the middle of a symmetric eye is its pixel middle, on the axis");
        const double tr[2] = {1.0, 1.0};
        const sz::LensEye lt = sz::lensEye(tr, sym, 2000, 1000);
        check(near3(lt.px[0], 2000.0, 1e-9) && near3(lt.px[1], 0.0, 1e-9) && near3(lt.offAxisDeg, std::atan(std::sqrt(1.0 + 0.64)) * sz::kDeg, 1e-6),
              "S0 lens: ndc (1, 1) is the top right pixel corner (y down), off the axis by atan of its tangent");
        const sz::LensEye la = sz::lensEye(c0, asym, 0, 0);
        check(la.ok && std::isnan(la.px[0]) && near3(la.offAxisDeg, std::atan(std::sqrt(0.01 + 0.01)) * sz::kDeg, 1e-6),
              "S0 lens: with no target size the pixels are unknown; an off-axis frustum's middle is off the axis by its tangents (-0.1, 0.1)");
        check(!sz::lensEye(nanC, sym, 100, 100).ok && std::isnan(sz::lensEye(c0, nullptr, 100, 100).offAxisDeg), "S0 lens: a c that is not a number is refused; no tangents, no angle");
    }

    // ---- the lines ----
    {
        char buf[1100];
        const auto has = [&](const char* needle) { return std::strstr(buf, needle) != nullptr; };
        const auto starts = [&](const char* prefix) { return std::strncmp(buf, prefix, std::strlen(prefix)) == 0; };
        sz::formatFlatWatching(buf, sizeof(buf), "running");
        check(starts("scope zoom (S0) flat: watching (measure only, no key)") && has("refresh hook (running).") && !has("nan"), "S0 lines: the flat announce");
        sz::formatFlatFlags(buf, sizeof(buf), true, false);
        check(starts("scope zoom (S0) flat: the journal's flags are readable from Status.json (on foot 1, ADS 0)"), "S0 lines: the flat flags line");
        sz::formatFlatPress(buf, sizeof(buf), 3, "$wpn_m_sniper_plasma_charged_name;", 4.2, nullptr, 0);
        check(starts("scope zoom (S0) flat: press 3 item $wpn_m_sniper_plasma_charged_name; hold 4.20 s; cameras none"), "S0 lines: a flat press with no camera says cameras none");
        sz::CamPress scoped, still, noHip;
        scoped.id = 2; scoped.camera = 0x1F2E3D4C; scoped.kind = 3; scoped.aspect = 1.7778;
        scoped.r.valid = true; scoped.r.hip = 1.047; scoped.r.ads = 0.5; scoped.r.settled = true; scoped.r.rampS = 0.31; scoped.r.minV = 0.5; scoped.r.maxV = 1.047;
        still = scoped; still.id = 1; still.r.ads = 1.047; still.r.minV = 1.047;
        noHip = scoped; noHip.id = 5; noHip.r.hip = sz::kNaN; noHip.r.minV = 0.5; noHip.r.maxV = 0.5; noHip.r.ads = 0.5;
        const sz::CamPress cams[3] = {still, scoped, noHip};
        sz::formatFlatPress(buf, sizeof(buf), 1, nullptr, 2.0, cams, 3);
        const char* scopedAt = std::strstr(buf, "cam 2 at 0x1F2E3D4C kind 3 aspect 1.7778 hip 59.989 deg ads 28.648 deg M ");
        const char* stillAt = std::strstr(buf, "cam 1 at ");
        check(scopedAt && stillAt && scopedAt < stillAt && has("item none") && has("settled 1 (min 28.648 max 59.989 deg)") && has("ramp 0.31 s") && !has("nan") && has("cam 5 at") && has("hip none"),
              "S0 lines: a flat press lists the cameras whose FOV moved first; a missing hip is none, never nan");
        sz::CamPress many[16];
        for (unsigned i = 0; i < 16; ++i) { many[i] = scoped; many[i].id = i + 1; many[i].camera = 0x7FF000000000ull + i; }
        const int len = sz::formatFlatPress(buf, sizeof(buf), 9, "$x;", 1.0, many, 16);
        check(len > 0 && len < 1100 && has("more camera(s) not listed") && std::strstr(buf, "cam 1 at ") != nullptr, "S0 lines: a flat press with sixteen moving cameras fits the line and counts the ones it leaves out");
        sz::formatVrWatching(buf, sizeof(buf));
        check(starts("scope zoom (S0) vr: watching (measure only, no key): each press of the sights in native on foot") && has("a pixel probe census.") && std::strlen(buf) < 1100 && !has("nan"),
              "S0 lines: the VR announce");
        // the VR line
        sz::VrPress v;
        v.press = 2; v.item = "$wpn_m_sniper_plasma_charged_name;"; v.scope = "yes"; v.copy = 0; v.holdS = 5.1; v.wMax = 1.0; v.wFullS = 4.3; v.settled = true;
        v.rc16C.hip = 1.571; v.rc16C.ads = 1.571;
        v.eyeL.hip = 1.0; v.eyeL.ads = 1.0;
        sz::formatVrPress(buf, sizeof(buf), v, nullptr, 0);
        check(starts("scope zoom (S0) vr: press 2 item $wpn_m_sniper_plasma_charged_name; scope yes copy 0 hold 5.10 s w_max 1.000 w_full 4.30 s settled 1") &&
                  has("; rc16C hip 1.5710 ads 1.5710; rc17C none; eyeL p11 hip 1.0000 ads 1.0000; eyeR p11 none; cameras none") && !has("nan"),
              "S0 lines: the VR press line: a source with no value is none; no camera says cameras none");
        v.wMax = sz::kNaN;
        sz::CamPress vc = scoped;
        sz::formatVrPress(buf, sizeof(buf), v, &vc, 1);
        check(has("w_max none") && has("; cam 2 at 0x1F2E3D4C kind 3 hip 59.989 ads 28.648 deg"), "S0 lines: the VR press line lists the cameras the VR hook saw; no w is none");
        // geometry
        const double cl[2] = {0.0123, -0.0456};
        const float tl[4] = {-1.f, 1.f, -1.f, 1.f};
        sz::LensEye eyeL = sz::lensEye(cl, tl, 2468, 2288);
        sz::LensEye none;
        sz::formatGeometry(buf, sizeof(buf), 4, "$x;", "yes", eyeL, nullptr, none, "no rows for this eye", 12.34, sz::kNaN);
        check(starts("scope lens geometry (S0): press 4 item $x; scope yes; eye L c ndc (0.0123, -0.0456) px (1249.2, 1196.2) of 2468x2288 off-axis ") &&
                  has("; eye R c none (no rows for this eye); game |E-S| 12.34 cm; eye R to S none cm") && !has("nan"),
              "S0 lines: the geometry line: c in the eye's ndc and pixels, none with its reason");
        // census
        sz::ProbeSet ps;
        const double cc[2] = {0.2, 0.1};
        const float tg[4] = {-1.0f, 1.0f, -1.0f, 1.0f};
        sz::probePoints(cc, tg, &ps);
        sz::formatCensusRun(buf, sizeof(buf), 1, 4, "$x;", "yes", ps, true);
        check(starts("scope lens census (S0): run 1 press 4 item $x; scope yes eye L points 0.600,0.450;") && has("at 0, 0.9 below, 1.5 above, 4 beside deg about c") && has("clamped to the target: none") &&
                  has("probe eye 0 is physical eye L;") && has("the probe keeps these points afterwards"),
              "S0 lines: the census run line says the left eye, names the four points, the clamp, the probe's eye beside the physical eye, and (the first time) that the probe keeps them");
        sz::formatCensusRun(buf, sizeof(buf), 3, 6, "$x;", "yes", ps, false, 1, 0);
        check(starts("scope lens census (S0): run 3 press 6 item $x; scope yes eye R points ") && has("probe eye 0 is physical eye L;"),
              "S0 lines: points from the right eye read against a probe on the left eye say so");
        sz::formatCensusRun(buf, sizeof(buf), 2, 5, "$x;", "yes", ps, false);
        check(!has("the probe keeps these points"), "S0 lines: only the first census run line says the probe keeps its points");
        sz::formatCensusSkip(buf, sizeof(buf), "c missing");
        check(std::strcmp(buf, "scope lens census (S0): run skipped (c missing)") == 0, "S0 lines: the census skip line");
        sz::formatCensusNotOptical(buf, sizeof(buf), 7, "$wpn_m_assaultrifle_kinetic_fauto_name;", "yes");
        check(starts("scope lens census (S0): run skipped (press 7 is not an optical sight: item $wpn_m_assaultrifle_kinetic_fauto_name; scope yes, and the S1 table holds optical sights only") &&
                  has(", so the pixel probe stays off for it)") && std::strlen(buf) < sizeof(buf) - 1,
              "S0 lines: a press of a sight that is not an optical sight is skipped with a line that names the press (so --health can tell it from a settled press the census never reached)");
        sz::formatCensusNotOptical(buf, sizeof(buf), 1, nullptr, nullptr);
        check(starts("scope lens census (S0): run skipped (press 1 is not an optical sight: item none scope unknown,"), "S0 lines: the not-optical skip line with no item or scope says none and unknown");
        // the skip line is written once for each (item, scope) a session
        sz::CensusSkipNoted said;
        const char* rifle = "$wpn_m_assaultrifle_kinetic_fauto_name;";
        check(said.first(rifle, 1) && !said.first(rifle, 1) && !said.first(rifle, 1), "S0 census skip lines: a sight's first press is told and its later presses are not");
        check(said.first(rifle, 0) && !said.first(rifle, 0), "S0 census skip lines: the same item with another scope state is another sight");
        check(said.first("$wpn_m_shotgun_plasma_doublebarrel_name;", 1) && !said.first("$wpn_m_shotgun_plasma_doublebarrel_name;", 1) && !said.first(rifle, 1),
              "S0 census skip lines: another item is another sight, and the first is still known");
        check(said.first(nullptr, 1) && !said.first("", 1) && !said.first("none", 1), "S0 census skip lines: no item and an empty one are the same sight, named none (the line says none)");
        const std::string a(sz::CensusSkipNoted::kItemChars, 'a'), b = a.substr(0, a.size() - 1) + "b", longer = a + "zzzz";
        sz::CensusSkipNoted names;
        check(names.first(a.c_str(), 1) && names.first(b.c_str(), 1) && !names.first(a.c_str(), 1) && !names.first(longer.c_str(), 1),
              "S0 census skip lines: names differing in the buffer's last character are two sights, and a name past the buffer is the sight whose first characters it shares");
        sz::CensusSkipNoted full;
        bool allFirst = true;
        for (uint32_t i = 0; i < sz::CensusSkipNoted::kSlots; ++i) allFirst = allFirst && full.first(("$item" + std::to_string(i)).c_str(), 1);
        check(allFirst && full.first("$item_beyond", 1) && full.first("$item_beyond", 1) && !full.first("$item0", 1) && !full.first(("$item" + std::to_string(sz::CensusSkipNoted::kSlots - 1)).c_str(), 1),
              "S0 census skip lines: past the table's size a new sight is still told (to the press line cap), and the sights it holds stay known");
    }
}

}  // namespace s0test
