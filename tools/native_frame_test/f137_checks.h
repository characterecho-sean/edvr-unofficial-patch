// F137 checks (native on-foot VR): a virtual neck under mouse look. Header only, run by native_frame_test (run137).
// Convention: v is the neck-to-eye rest vector (forward kPinRestBackM 9 cm, up kPinRestUpM 15.5 cm), R(p) turns it nose up by p in the (forward, up) plane, h is the head's
// own pitch and a the game's aim pitch (nose up +): d = R(h + a) v - R(h) v, body axes (forward +, up +), published as the seated vector (0, -up, +forward).
#pragma once

#include "../../src/d3d11/native_on_foot_f119.h"
#include "../../src/d3d11/native_on_foot_f137.h"

#include <cmath>
#include <cstdint>

namespace f137test {

namespace nf = edvr::nofoot;
namespace f137 = edvr::nofoot::f137;

inline f137::In inputs(double headDeg, double aimDeg, double dt = 0.0) {
    f137::In in;
    in.scope = true;
    in.aimFresh = true;
    in.headValid = true;
    in.headPitchRad = headDeg * nf::kAdsDeg;
    in.aimPitchRad = aimDeg * nf::kAdsDeg;
    in.dt = dt;
    return in;
}
// A neck that has reached its full weight on these inputs.
inline f137::Out settle(f137::VirtualNeck& v, const f137::In& base) {
    f137::In in = base;
    in.dt = 0.01;
    f137::Out o;
    for (int i = 0; i < 40; ++i) o = v.step(in);
    return o;
}

template <class Check>
inline void run137(Check&& check) {
    {   // the offset: 0 at a = 0, +9 cm forward and -15 cm (down) at h = 0, a = -60 deg, the caps
        const f137::Offset level = f137::offset(0.0, 0.0);
        const f137::Offset levelHeadDown = f137::offset(-40.0 * nf::kAdsDeg, 0.0);
        check(level.ok && level.forward == 0.0 && level.up == 0.0 && levelHeadDown.ok && std::fabs(levelHeadDown.forward) < 1e-15 && std::fabs(levelHeadDown.up) < 1e-15,
              "F137: a level aim adds nothing, with the head level or pitched (a head pitched by the player already carries its own neck)");
        const f137::Offset down60 = f137::offset(0.0, -60.0 * nf::kAdsDeg);
        check(down60.ok && std::fabs(down60.forward - 0.0892) < 0.0005 && std::fabs(down60.up + 0.1554) < 0.0005,
              "F137: the aim 60 deg down with the head level: the eye 8.9 cm forward and 15.5 cm down (v = up 15.5, forward 9 cm)");
        // the rest vector turned by hand, 60 deg nose down: forward 9 cos60 + 15.5 sin60, up -9 sin60 + 15.5 cos60
        const double f = 0.09 * 0.5 + 0.155 * std::sqrt(3.0) / 2.0, u = -0.09 * std::sqrt(3.0) / 2.0 + 0.155 * 0.5;
        check(std::fabs(down60.forward - (f - 0.09)) < 1e-12 && std::fabs(down60.up - (u - 0.155)) < 1e-12, "F137: d is R(h + a) v - R(h) v, worked by hand");
        const f137::Offset up60 = f137::offset(0.0, 60.0 * nf::kAdsDeg);
        check(up60.ok && up60.forward < -0.15 && std::fabs(up60.up) < 0.001, "F137: the aim 60 deg up moves the eye back, as a head tilted back does");
        // the head's own pitch: the camera's pitch is h + a, the head carries R(h) v already
        const f137::Offset mixed = f137::offset(-20.0 * nf::kAdsDeg, -40.0 * nf::kAdsDeg);
        double f1, u1, f0, u0;
        f137::rotateRest(-60.0 * nf::kAdsDeg, &f1, &u1);
        f137::rotateRest(-20.0 * nf::kAdsDeg, &f0, &u0);
        check(std::fabs(mixed.forward - (f1 - f0)) < 1e-12 && std::fabs(mixed.up - (u1 - u0)) < 1e-12, "F137: with the head pitched 20 deg down and the aim 40 deg down the camera is at 60 deg");
        const f137::Offset steep = f137::offset(0.0, -90.0 * nf::kAdsDeg);
        const f137::Offset steeper = f137::offset(0.0, -120.0 * nf::kAdsDeg);
        check(steep.ok && steep.length() <= f137::kCapM + 1e-12 && std::fabs(steep.length() - f137::kCapM) < 1e-9 && std::fabs(steeper.length() - f137::kCapM) < 1e-9,
              "F137: the offset is capped at 25 cm (the rest vector turned 90 deg is 25.3 cm)");
        const f137::Offset ratio = f137::offset(0.0, -120.0 * nf::kAdsDeg);
        double rf, ru;
        f137::rotateRest(-120.0 * nf::kAdsDeg, &rf, &ru);
        check(std::fabs(ratio.forward / ratio.up - (rf - 0.09) / (ru - 0.155)) < 1e-9, "F137: the cap keeps the direction");
        check(!f137::offset(std::nan(""), 0.0).ok && !f137::offset(0.0, std::nan("")).ok, "F137: no offset from a pitch that is not a number");
    }
    {   // the seated vector: seated = D body, D = diag(-1, -1, +1): the view moves forward and down
        double seated[3];
        const f137::Offset d = f137::offset(0.0, -60.0 * nf::kAdsDeg);
        f137::seatedOf(d.forward, d.up, seated);
        double body[3] = {0.0, d.up, d.forward}, expect[3];
        nf::seatedFromBody(body, 0.0, expect);
        check(seated[0] == 0.0 && !std::signbit(seated[0]) && seated[1] > 0.15 && std::fabs(seated[1] + d.up) < 1e-15 && std::fabs(seated[2] - d.forward) < 1e-15 &&
                  std::fabs(seated[1] - expect[1]) < 1e-15 && std::fabs(seated[2] - expect[2]) < 1e-15,
              "F137: the seated vector is (0, -up, +forward): up 15.5 cm and forward 8.9 cm for an eye 15.5 cm down and 8.9 cm forward");
        double zeros[3];
        f137::seatedOf(0.0, 0.0, zeros);
        check(!std::signbit(zeros[0]) && !std::signbit(zeros[1]) && !std::signbit(zeros[2]) && zeros[0] == 0.0 && zeros[1] == 0.0 && zeros[2] == 0.0, "F137: a zero offset is exact positive zeros");
        // the pin's side: the camera the pose carried, less the seated vector through the ADS shift's map (adsUnlockedCam), is the game's own camera
        const float rows[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
        const float game[3] = {0.0f, 1.6f, 0.0f};
        const double bodyMove[3] = {0.0, d.up, d.forward};   // the camera moves by this in the body's axes
        const float drawn[3] = {game[0], static_cast<float>(game[1] + bodyMove[1]), static_cast<float>(game[2] + bodyMove[2])};
        float back[3];
        nf::adsUnlockedCam(drawn, seated, rows, back);
        check(std::fabs(back[0] - game[0]) < 1e-6f && std::fabs(back[1] - game[1]) < 1e-6f && std::fabs(back[2] - game[2]) < 1e-6f,
              "F137: the pin takes the whole vector out of the camera (right, up, forward): the body is placed from the game's own camera");
    }
    {   // the weight: 0.15 s in and out, 0 under look to aim, the ADS weight, the scope
        f137::VirtualNeck v;
        f137::In in = inputs(0.0, -60.0, 0.015);
        f137::Out o = v.step(in);
        check(o.reason == f137::kOn && std::fabs(o.w - 0.1) < 1e-12, "F137: the weight ramps in 0.15 s (15 ms is 0.1)");
        for (int i = 0; i < 4; ++i) o = v.step(in);
        check(std::fabs(o.w - 0.5) < 1e-9 && std::fabs(o.upM - 0.5 * f137::offset(0.0, -60.0 * nf::kAdsDeg).up) < 1e-12, "F137: half way at 75 ms, half the offset");
        for (int i = 0; i < 5; ++i) o = v.step(in);
        check(o.w > 1.0 - 1e-9, "F137: the full weight at 150 ms");
        in.adsWeight = 0.5;
        o = v.step(in);
        check(std::fabs(o.w - 0.5) < 1e-12, "F137: the ADS lock's weight takes the same share off (times 1 - w)");
        in.adsWeight = 1.0;
        o = v.step(in);
        check(o.w == 0.0 && o.seated[1] == 0.0 && o.seated[2] == 0.0 && o.forwardM == 0.0 && o.upM == 0.0, "F137: no offset in ADS (the sight lock owns the eye)");
        in.adsWeight = 0.0;
        o = v.step(in);
        check(o.w > 1.0 - 1e-9, "F137: and it is back when the press ends");
        // look to aim drives the aim: the weight leaves in 0.15 s with the last offset, and stays out
        in.lookToAim = true;
        in.dt = 0.075;
        o = v.step(in);
        check(o.reason == f137::kLookToAim && std::fabs(o.w - 0.5) < 1e-9 && std::fabs(o.upM - 0.5 * f137::offset(0.0, -60.0 * nf::kAdsDeg).up) < 1e-12,
              "F137: look to aim engaged ramps the weight out with the last offset");
        o = v.step(in);
        o = v.step(in);
        check(o.w == 0.0 && o.seated[0] == 0.0 && o.seated[1] == 0.0 && o.seated[2] == 0.0, "F137: under look to aim the weight is 0 and the vector is exact zeros");
        in.aimPitchRad = -10.0 * nf::kAdsDeg;   // the aim has moved meanwhile
        in.lookToAim = false;
        in.dt = 0.075;
        o = v.step(in);
        check(std::fabs(o.upM - 0.5 * f137::offset(0.0, -10.0 * nf::kAdsDeg).up) < 1e-12, "F137: back on mouse look the offset follows the inputs again");
    }
    {   // each reason, in the order the tick asks, and a ramp that leaves with the inputs gone
        f137::In in = inputs(0.0, -60.0, 0.01);
        in.scope = false;
        check(f137::reasonOf(in) == f137::kOutOfScope, "F137: outside the pin's scope");
        in.scope = true;
        in.lookToAim = true;
        check(f137::reasonOf(in) == f137::kLookToAim, "F137: look to aim drives the aim");
        in.lookToAim = false;
        in.aimFresh = false;
        check(f137::reasonOf(in) == f137::kNoAim, "F137: no fresh aim pitch");
        in.aimFresh = true;
        in.headValid = false;
        check(f137::reasonOf(in) == f137::kNoHead, "F137: no head pose");
        in.headValid = true;
        in.aimPitchRad = std::nan("");
        check(f137::reasonOf(in) == f137::kNoAim, "F137: an aim pitch that is not a number is no aim pitch");
        // a stale aim ramps the weight out on the offset it had
        f137::VirtualNeck v;
        const f137::Out full = settle(v, inputs(0.0, -60.0));
        f137::In stale = inputs(0.0, -60.0, 0.075);
        stale.aimFresh = false;
        const f137::Out half = v.step(stale);
        check(full.w == 1.0 && half.reason == f137::kNoAim && std::fabs(half.w - 0.5) < 1e-9 && std::fabs(half.upM - 0.5 * full.upM) < 1e-12 && std::fabs(half.forwardM - 0.5 * full.forwardM) < 1e-12,
              "F137: a stale aim pitch ramps the weight out on the last offset");
        // a long frame is capped, a zero or negative dt moves nothing
        f137::VirtualNeck w;
        f137::In long1 = inputs(0.0, -60.0, 5.0);
        check(w.step(long1).w == 1.0, "F137: a long frame (capped at 250 ms) reaches the full weight and does not overshoot it");
        f137::VirtualNeck z;
        f137::In zero = inputs(0.0, -60.0, 0.0);
        check(z.step(zero).w == 0.0, "F137: no time passes, no weight");
    }
    {   // the pose's history: the pin finds exactly the vector the drawn pose carried, however many newer ones were published
        f137::SeatedHistory hist;
        hist.record(5, 0.155, 0.089);
        hist.record(6, 0.100, 0.060);
        hist.record(7, 0.0, 0.0);
        f137::SeatedHistory::Found a = hist.find(5 + 1);
        double ny, nz;
        hist.newest(&ny, &nz);
        check(a.reported && a.matched && std::fabs(a.y - 0.155) < 1e-6 && std::fabs(a.z - 0.089) < 1e-6 && ny == 0.0 && nz == 0.0,
              "F137: the pose's reported serial finds its own vector (0.155, 0.089) and not the newest (0, 0)");
        check(!hist.find(0).reported && !hist.find(0).matched, "F137: a runtime that reports no serial is not matched");
        check(hist.find(1).matched && hist.find(1).y == 0.0 && hist.find(1).z == 0.0, "F137: serial 0 is the zero alignment before any publication");
        const f137::SeatedHistory::Found lost = hist.find(5 + 256 + 1);
        check(lost.reported && !lost.matched, "F137: a serial the history has not got is reported and not matched");
        hist.record(5 + 256, 0.02, 0.01);
        check(!hist.find(5 + 1).matched && hist.find(5 + 256 + 1).matched, "F137: a slot overwritten by a later serial is a miss, not a mixed value");
    }
    {   // the head containment: the physical head is the pose's render plus the alignment the pose carried, so the virtual neck cancels
        double d[3];
        f137::seatedOf(0.089, -0.155, d);   // body (forward 8.9, up -15.5) -> seated (0, +15.5, +8.9)
        float renderWith[12] = {}, renderWithout[12] = {};
        const double none[3] = {};
        const double physical[3] = {0.02, 1.62, -0.30};   // right, up, z (forward is -z)
        renderWithout[3] = static_cast<float>(physical[0]);
        renderWithout[7] = static_cast<float>(physical[1]);
        renderWithout[11] = static_cast<float>(physical[2]);
        // the runtime moved the seated origin by the alignment: the pose the game gets is the physical head less it
        renderWith[3] = static_cast<float>(physical[0] - d[0]);
        renderWith[7] = static_cast<float>(physical[1] - d[1]);
        renderWith[11] = static_cast<float>(physical[2] - d[2]);
        const edvr::nofoot::f119::Offset withOffset = edvr::nofoot::f119::physicalOffset(renderWith, d);
        const edvr::nofoot::f119::Offset withoutOffset = edvr::nofoot::f119::physicalOffset(renderWithout, none);
        check(std::fabs(withOffset.right - withoutOffset.right) < 1e-6 && std::fabs(withOffset.up - withoutOffset.up) < 1e-6 && std::fabs(withOffset.forward - withoutOffset.forward) < 1e-6,
              "F137: the physical head offset (render plus the alignment the pose carried) is the same with and without the virtual neck: the leash cannot trip on it");
    }
    {   // a second of ticks
        f137::VirtualNeck v;
        f137::Second s;
        f137::Out o = v.step(inputs(0.0, -60.0, 0.0));
        s.add(o, -60.0 * nf::kAdsDeg, 0.0);
        check(!s.any(), "F137: a tick at weight 0 is not counted");
        for (int i = 0; i < 20; ++i) {
            o = v.step(inputs(0.0, -60.0, 0.01));
            s.add(o, -60.0 * nf::kAdsDeg, 0.0);
        }
        check(s.any() && s.ticks == 20 && s.wMax == 1.0 && s.maxCm > 17.0 && s.maxCm < 18.0 && std::fabs(s.mean(s.aimDegSum) + 60.0) < 1e-9 && s.mean(s.upCmSum) < -5.0 &&
                  s.mean(s.forwardCmSum) > 2.0,
              "F137: a second counts the ticks with weight, the largest offset (17.9 cm for 60 deg), the means");
        s.clear();
        check(!s.any(), "F137: the second is cleared after its line");
    }
}

}  // namespace f137test
