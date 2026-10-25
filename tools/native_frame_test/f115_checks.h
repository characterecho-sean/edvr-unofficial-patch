// F115 checks (native on-foot VR): boarding undoes every on-foot camera change. Header only, run by native_frame_test (run115).
// The figures are the boarding in log edvr_gfx_20261025_071049 (v0.21.1-236-g33e05706, Pimax Crystal Super on Pimax OpenXR, Epic): the eye alignment's y -0.346 m
// (the standing-height hold's raise +16.9 cm with the head 16.9 cm under the seated origin, and the neck reference's raise), look to aim's seated yaw -5.84 deg and
// pitch +4.86 deg, and the head 17.8 cm under the seated origin 2 s after the edge.
#pragma once

#include "../../src/common/native_frame.h"
#include "../../src/d3d11/native_on_foot_core.h"
#include "../../src/d3d11/native_on_foot_f94.h"
#include "../../src/d3d11/native_on_foot_f115.h"
#include "../../src/d3d11/holo_material.h"

#include <cmath>
#include <limits>

namespace f115test {

namespace f87 = edvr::nofoot::f87;
namespace f94 = edvr::nofoot::f94;
namespace f115 = edvr::nofoot::f115;

template <class Check>
inline void run115(Check&& check) {
    {   // the edge: a stretch on foot, the cockpit, on foot again, the cockpit
        f115::ExitEdge e;
        const bool flag[9] = {false, false, true, true, true, false, false, true, false};
        f115::ExitEdge::Edge got[9];
        for (int i = 0; i < 9; ++i) got[i] = e.step(flag[i]);
        check(got[0] == f115::ExitEdge::kNone && got[1] == f115::ExitEdge::kNone && got[2] == f115::ExitEdge::kEntry && got[3] == f115::ExitEdge::kNone &&
                  got[4] == f115::ExitEdge::kNone && got[5] == f115::ExitEdge::kExit && got[6] == f115::ExitEdge::kNone && got[7] == f115::ExitEdge::kEntry &&
                  got[8] == f115::ExitEdge::kExit && e.exits == 2 && e.entries == 2,
              "F115: one exit edge per fall of native stereo on foot's flag and one entry per rise; a process that starts elsewhere has nothing to undo");
        f115::ExitEdge s;
        const f115::ExitEdge::Edge a = s.step(true), b = s.step(true), c = s.step(false);
        check(a == f115::ExitEdge::kEntry && b == f115::ExitEdge::kNone && c == f115::ExitEdge::kExit && s.exits == 1,
              "F115: a process that starts on foot has entered, and its first boarding is an exit edge");
    }
    {   // the cockpit's own panels as the early edge, 11 ms frames: the camera's state at each, through CameraOnFoot as f115Tick steps it
        struct Run { int offAt = -1, onAgainAt = -1, falseStarts = 0; };
        // flagUp/flagDown: the flag's span (ms); panels drawn in [p0, p1) (p0 < 0: never)
        const auto run = [](int flagUp, int flagDown, int p0, int p1, int endMs) {
            Run r;
            f115::CameraOnFoot c;
            bool last = false;
            for (int t = 0; t < endMs; t += 11) {
                const bool flag = t >= flagUp && t < flagDown, panels = p0 >= 0 && t >= p0 && t < p1;
                const f115::CameraOnFoot::Out o = c.step(flag, panels, static_cast<uint64_t>(t) + 1000);
                if (o.falseStart) ++r.falseStarts;
                if (last && !o.on && r.offAt < 0) r.offAt = t;
                if (!last && o.on && r.offAt >= 0 && r.onAgainAt < 0) r.onAgainAt = t;
                last = o.on;
            }
            return r;
        };
        // the 07:27 boarding: on foot 15 s, the panels from 0.98 s before the flag falls, and on in the cockpit after it
        const Run board = run(0, 15000, 14020, 30000, 30000);
        // the flag alone (GUI effects on, or an SRV): no panel
        const Run flagOnly = run(0, 15000, -1, 0, 30000);
        // a disembark: the flag rises with the cockpit's panels still drawing for 0.6 s, then on foot for 20 s
        const Run disembark = run(0, 30000, 0, 600, 20000);
        // a false start: armed, the panels for 0.3 s at 10 s, then none with the flag still up
        const Run falseStart = run(0, 30000, 10000, 10300, 20000);
        check(board.offAt >= 14020 && board.offAt < 14031 && board.onAgainAt < 0 && board.falseStarts == 0 && flagOnly.offAt >= 15000 && flagOnly.offAt < 15011,
              "F115: the boarding ends the on-foot camera at the cockpit panels' first frame, 0.98 s before the flag (which stays the edge when no panel draws)");
        check(disembark.offAt < 0 && disembark.falseStarts == 0,
              "F115: at a disembark the cockpit's last panel frames after the flag's rise end nothing: the panels' edge arms 2 s after the last of them");
        check(falseStart.offAt >= 10000 && falseStart.offAt < 10011 && falseStart.falseStarts == 1 && falseStart.onAgainAt >= 10289 + 3000 &&
                  falseStart.onAgainAt <= 10300 + 3000 + 11,
              "F115: panels that draw on foot and stop with the flag still up are a false start: the camera changes resume 3 s after the last one");
        check(f115::kCockpitPanelVs == edvr::kHoloGuiFxOffVs, "F115: the edge's vertex shader is the cockpit holo panels' with Disable GUI effects on (holo_material.h)");
    }
    {   // the flight's eye alignment through the edge, the hold and the neck reference stepped as alignTick steps them
        const double dt = 1.0 / 90.0;
        struct Ys { double before = 1.0, edge = 1.0, quarter = 1.0, second = 1.0, back = 1.0; };
        const auto run = [&](bool f115On) {
            Ys ys;
            f87::HeightHold hold;
            f94::NeckRaise neck;
            for (int i = 0; i < 360; ++i) {   // 4 s at 90 Hz: on foot to 2 s (frame 180, the edge), the cockpit to 3.9 s, on foot again from frame 351
                const bool onFoot = i < 180 || i >= 351;
                if (f115On && i == 180) {   // f115Exit, before alignTick in the edge's frame
                    hold.reset();
                    neck.reset();
                }
                f87::HoldIn hi;
                hi.scope = onFoot;
                hi.devValid = true;
                hi.devM = -0.169;
                hi.dt = dt;
                hi.settleS = 0.1;
                const f87::HoldOut ho = hold.step(hi);
                f94::In ni;
                ni.scope = onFoot;
                ni.targetFresh = onFoot;
                ni.targetM = 0.177;
                ni.dt = dt;
                ni.settleS = 0.1;
                const f94::Out no = neck.step(ni);
                double y = f87::eyeAlignY(0.0, 0.0, 0.0, 0.0, ho.eyeY) + no.seatedY;
                if (f115On) y = f115::gate(onFoot, y);
                if (i == 179) ys.before = y;
                if (i == 180) ys.edge = y;
                if (i == 202) ys.quarter = y;
                if (i == 270) ys.second = y;
                if (i == 351) ys.back = y;
            }
            return ys;
        };
        const Ys now = run(true), old = run(false);
        check(std::fabs(now.before + 0.346) < 0.002 && now.edge == 0.0 && now.quarter == 0.0 && now.second == 0.0 && std::fabs(now.back) < 0.01,
              "F115: the flight's 34.6 cm raise (the hold and the neck reference) is an exact zero from the edge's frame, a quarter second and a second later, and "
              "back on foot it ramps in from zero as at any entry");
        check(old.edge < -0.33 && old.quarter < -0.15 && old.second == 0.0,
              "F115 (control): the 33e05706 build ramped the 34.6 cm out over 0.5 s with the cockpit drawn (still over 15 cm a quarter second after the edge)");
    }
    {   // the snapshot's zero: every field exactly, a negative zero included
        f115::Published z;
        z.eyeM[1] = -0.0;
        f115::Published share = z, look = z, shift = z;
        share.share = 0.01;
        look.lookDeg = -5.84;
        shift.shiftM[2] = 1e-9;
        check(z.zero() && !share.zero() && !look.zero() && !shift.zero() && std::fabs(shift.shiftCm() - 1e-7) < 1e-12,
              "F115: the read-back is zero only when every value is exactly zero (the share, the look, a nanometre of ADS shift each count)");
    }
    {   // the head against the seated origin a second after the edge: the flight's -17.8 cm is a recentre's worth, a sitting head's 2 cm is not
        const float flight[12] = {1, 0, 0, 0.005f, 0, 1, 0, -0.178f, 0, 0, 1, -0.072f};
        const float seated[12] = {1, 0, 0, 0.01f, 0, 1, 0, -0.02f, 0, 0, 1, 0.0f};
        const double h = f115::headHeightM(flight), off = f115::headOffsetM(flight);
        check(std::fabs(h + 0.178) < 1e-6 && std::fabs(off - std::sqrt(0.005 * 0.005 + 0.072 * 0.072)) < 1e-6 && f115::seatOff(h) &&
                  !f115::seatOff(f115::headHeightM(seated)) && f115::seatOff(std::numeric_limits<double>::quiet_NaN()),
              "F115: the head 17.8 cm under the seated origin a second after boarding (the flight) is a recentre's worth; 2 cm is not; not a number is");
    }
    {   // the provider's words read back: the flight's values, then the edge's zeros
        const float eye[3] = {0.0f, -0.346f, 0.0f};
        edvr::nativeFrameSetEyeAlign(eye, 8576);
        edvr::nativeFrameSetLookYaw(-0.40065f, 130217, 9);
        edvr::nativeFrameSetAdsPitch(0.0848f);
        edvr::nativeFrameSetRotCalibrate(0.0f, 0.0f);
        edvr::EdvrNativeOnFootWords w{};
        edvr::nativeFrameOnFootWords(w);
        const bool flightOk = w.eyeAlign[1] == -0.346f && w.eyeAlignSerial == 8576 && w.lookYawTotal == -0.40065f && w.lookYawReset == 9 && w.adsPitch == 0.0848f;
        const float zero[3] = {};
        edvr::nativeFrameSetEyeAlign(zero, 8577);
        edvr::nativeFrameSetLookYaw(-0.40065f, 130218, 10);
        edvr::nativeFrameSetAdsPitch(0.0f);
        edvr::nativeFrameOnFootWords(w);
        const bool edgeOk = w.eyeAlign[0] == 0.0f && w.eyeAlign[1] == 0.0f && w.eyeAlign[2] == 0.0f && w.eyeAlignSerial == 8577 && w.lookYawReset == 10 &&
                            w.adsPitch == 0.0f && w.rotCal[0] == 0.0f && w.rotCal[1] == 0.0f;
        check(flightOk && edgeOk, "F115: the provider hands back the words the frame thread set: the flight's alignment, look and pitch, then the edge's zeros and reset serial");
        edvr::nativeFrameSetEyeAlign(zero, 0);
        edvr::nativeFrameSetLookYaw(0.0f, 0, 0);
    }
}

}  // namespace f115test
