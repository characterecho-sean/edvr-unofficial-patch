// F93d and F93e checks (native on-foot VR), run93 below. Header only, run by native_frame_test.
//
// F93d: the published camera (native_on_foot.h: nativeOnFootCameraPublish, nativeOnFootCameraExternal, nativeOnFootBodyCullWanted) and the frame thread's watch
// (native_on_foot_f93.h, CameraWatch) through the F92 flight's camera-suite stretch: on foot, the suite opened (VanityCamera started, 10:31:17.355), its presets and
// its free camera, closed again (10:31:43.539), the controller going quiet with the suite open, native stereo on foot ending inside the suite.
// F93e: the stance's height taken by the hidden head (neckTakesHeight) on the F92 flight's standing figures: the Energylink looking level and with the aim pitched
// down, fists, and the rifle stance (a raise, untouched).
#pragma once

#include "../../src/d3d11/native_on_foot.h"
#include "../../src/d3d11/native_on_foot_f93.h"

#include <cmath>
#include <cstdint>

namespace f93test {

namespace f93 = edvr::nofoot::f93;

inline bool within(double a, double b, double tol) { return std::fabs(a - b) <= tol; }

// The pin's eye, its rest target and the animated neck as the F92 flight's standing seconds give them (the model space, metres; the heading +z).
struct Stance {
    double eye[3], target[3], n0y;
};
inline Stance stance(double eyeY, double upM, double fwdM, double n0y) {
    Stance s;
    s.eye[0] = 0.02;
    s.eye[1] = eyeY;
    s.eye[2] = 0.40;
    s.target[0] = s.eye[0];
    s.target[1] = eyeY - upM;
    s.target[2] = s.eye[2] - fwdM;
    s.n0y = n0y;
    return s;
}

template <class Check>
inline void run93(Check&& check) {
    using edvr::g_nativeOnFootActive;
    // ---- F93d: the published camera --------------------------------------------------------------------------------------------------------------------------------
    edvr::g_nativeOnFootCamExternal.store(false);
    edvr::g_nativeOnFootCamFlips.store(0);
    edvr::g_nativeOnFootCamReads.store(0);
    g_nativeOnFootActive.store(true);
    check(!edvr::nativeOnFootCameraExternal(), "F93d: on foot with no read yet the camera is first person (the body changes run as before F93d)");
    bool flipped = edvr::nativeOnFootCameraPublish(0, 1000);
    check(!flipped && !edvr::nativeOnFootCameraExternal() && edvr::g_nativeOnFootCamReads.load() == 1 && edvr::g_nativeOnFootCamMode.load() == 0x100u,
          "F93d: mode 0 after the controller's update: first person, one read, no change");
    flipped = edvr::nativeOnFootCameraPublish(1, 2000);   // the suite opens on its first preset (10:31:17.355)
    check(flipped && edvr::nativeOnFootCameraExternal() && edvr::g_nativeOnFootCamFlips.load() == 1 && edvr::g_nativeOnFootCamFlipQpc.load() == 2000,
          "F93d: mode 1 (the suite on a preset): external from that read, the change counted and stamped with the read's time");
    edvr::g_nativeOnFootBodyCull.store(1);
    check(!edvr::nativeOnFootBodyCullWanted(), "F93d: the torso cull (F18, key none) is not wanted in the third-person camera");
    flipped = edvr::nativeOnFootCameraPublish(2, 3000);
    const bool still2 = !flipped && edvr::nativeOnFootCameraExternal();
    flipped = edvr::nativeOnFootCameraPublish(3, 4000);   // the free camera
    const bool still3 = !flipped && edvr::nativeOnFootCameraExternal();
    flipped = edvr::nativeOnFootCameraPublish(5, 5000);   // detached (world lock)
    check(still2 && still3 && !flipped && edvr::nativeOnFootCameraExternal() && edvr::g_nativeOnFootCamFlips.load() == 1 && edvr::g_nativeOnFootCamFlipQpc.load() == 2000,
          "F93d: the second preset, the free camera and its world lock (modes 2, 3, 5) are all the third-person camera: no further change");
    g_nativeOnFootActive.store(false);
    check(!edvr::nativeOnFootCameraExternal(), "F93d: off foot the published flag does not count (nativeOnFootCameraExternal is on-foot only)");
    g_nativeOnFootActive.store(true);
    flipped = edvr::nativeOnFootCameraPublish(0, 6000);   // closed (10:31:43.539)
    check(flipped && !edvr::nativeOnFootCameraExternal() && edvr::g_nativeOnFootCamFlips.load() == 2 && edvr::nativeOnFootBodyCullWanted(),
          "F93d: mode 0 again: first person from that read, the second change counted, the torso cull wanted again");
    edvr::g_nativeOnFootBodyCull.store(0);
    g_nativeOnFootActive.store(false);
    edvr::g_nativeOnFootCamExternal.store(false);

    // ---- F93d: the frame thread's watch ------------------------------------------------------------------------------------------------------------------------------
    {
        f93::CameraWatch w;
        f93::WatchIn in;
        in.active = true;
        in.nowMs = 100000;
        in.reads = 0;
        f93::WatchOut o = w.step(in);
        check(!o.watching && !o.missing && !o.changed && !o.external && !o.clearFlag, "F93d watch: the first tick on foot with no read says nothing yet");
        in.nowMs += 4900;
        o = w.step(in);
        const bool quiet = !o.missing;
        in.nowMs += 100;
        o = w.step(in);
        check(quiet && o.missing && !o.changed, "F93d watch: no read for 5 s on foot: ONE missing line (the body changes stay on in every camera)");
        in.nowMs += 1000;
        o = w.step(in);
        check(!o.missing, "F93d watch: ...said once a stretch");
        in.reads = 90;
        in.nowMs += 11;
        o = w.step(in);
        check(o.watching && !o.changed && !o.external, "F93d watch: the first read: ONE watching line, first person");
        in.reads = 91;
        in.nowMs += 11;
        o = w.step(in);
        check(!o.watching && !o.changed, "F93d watch: ...said once");
        in.reads = 92;
        in.flag = true;   // the suite opened at this read
        in.nowMs += 11;
        o = w.step(in);
        check(o.changed && o.external && !o.clearFlag, "F93d watch: the suite opens: a change to the third-person camera");
        for (int i = 0; i < 2000; ++i) {   // 26 s in the suite at 90 updates a second (the F92 flight's 10:31:17-10:31:43)
            in.reads += 1;
            in.nowMs += 11;
            o = w.step(in);
            if (o.changed || !o.external || o.silent) break;
        }
        check(!o.changed && o.external && !o.silent, "F93d watch: 2000 updates in the suite: no further change, no silence (the controller keeps reading)");
        in.reads += 1;
        in.flag = false;   // closed
        in.nowMs += 11;
        o = w.step(in);
        check(o.changed && !o.external, "F93d watch: the suite closes: a change back to first person on that tick");
        // the controller goes quiet with the suite open: after 1 s the flag is no longer evidence
        in.reads += 1;
        in.flag = true;
        in.nowMs += 11;
        o = w.step(in);
        const bool opened = o.changed && o.external;
        in.nowMs += 1000;
        o = w.step(in);
        const bool waited = !o.silent && o.external;
        in.nowMs += 12;
        o = w.step(in);
        check(opened && waited && o.silent && o.clearFlag && o.changed && !o.external && o.quietMs == 1012,
              "F93d watch: the controller quiet for over 1 s with the suite open: ONE silent line, the flag cleared, first person (the body changes on)");
        in.flag = false;   // the frame thread cleared it
        in.nowMs += 500;
        o = w.step(in);
        check(!o.silent && !o.changed, "F93d watch: ...said once");
        in.reads += 1;
        in.flag = true;   // it reads again, the suite still open
        in.nowMs += 11;
        o = w.step(in);
        check(o.changed && o.external && !o.watching, "F93d watch: the controller reads again with the suite open: the third-person camera again (no second watching line)");
        // native stereo on foot ends inside the suite
        in.active = false;
        in.nowMs += 11;
        o = w.step(in);
        check(o.ended && o.clearFlag && !o.external && !o.changed, "F93d watch: native stereo on foot ends with the suite open: the closing line and the flag cleared");
        in.nowMs += 11;
        o = w.step(in);
        check(!o.ended && o.clearFlag, "F93d watch: ...said once; off foot the flag is cleared every tick");
        // the next stretch on foot starts first person and watches again
        in.active = true;
        in.flag = false;
        in.reads += 1;
        in.nowMs += 11;
        o = w.step(in);
        check(o.watching && !o.changed && !o.external, "F93d watch: the next stretch on foot: a new watching line, first person");
    }

    // ---- F93e: the stance's height taken by the hidden head ---------------------------------------------------------------------------------------------------------
    {
        // The Energylink standing, the aim level (10:31:49-50): eye 1.587 m, the rest vector 14.1 up and 10.9 forward (LOCAL pitch applied), animated neck 1.540 m:
        // the rest target 1.446 m, 9.4 cm under the neck: the pin lowered the hips by it (the knees bent).
        Stance s = stance(1.587, 0.141, 0.109, 1.540);
        const double rad = std::sqrt(0.141 * 0.141 + 0.109 * 0.109);
        f93::NeckTake t = f93::neckTakesHeight(s.eye, s.target, 0.0, 1.0, s.n0y);
        const double v[3] = {s.eye[0] - s.target[0], s.eye[1] - s.target[1], s.eye[2] - s.target[2]};
        check(t.applied && within(t.gapM, 0.094, 1e-9) && within(t.takenM, 0.094, 1e-9) && within(t.hipsM, 0.0, 1e-9) && within(s.target[1], 1.540, 1e-9),
              "F93e: the Energylink, aim level: the 9.4 cm the rest target would lower the neck is taken whole by the head: the neck keeps its animated 1.540 m (hips 0)");
        check(within(std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]), rad, 1e-9) && v[1] > 0.0 && v[2] > 0.0 && within(v[0], 0.0, 1e-12),
              "F93e: ...the eye keeps its distance from the neck, ahead of it and above it, and the lateral part (0) is kept");
        check(within(t.backM, std::sqrt(rad * rad - 0.047 * 0.047) - 0.109, 1e-9) && t.backM > 0.05 && t.backM < 0.07 && t.pitchDeg > 30.0 && t.pitchDeg < 45.0,
              "F93e: ...the neck-to-eye vector pitched forward by 30-45 deg, the body 5-7 cm further back");

        // The Energylink with the aim pitched down 21 deg (10:31:56-58): eye 1.529 m, vector 15.5 up 8.8 forward, neck 1.542: the gap 16.8 cm, more than the
        // 15.5 cm the head can take (the eye level with the neck): the hips keep 1.3 cm.
        s = stance(1.529, 0.155, 0.088, 1.542);
        t = f93::neckTakesHeight(s.eye, s.target, 0.0, 1.0, s.n0y);
        check(t.applied && within(t.gapM, 0.168, 1e-9) && within(t.takenM, 0.155, 1e-9) && within(t.hipsM, 0.013, 1e-9) && within(s.target[1], 1.529, 1e-9) &&
                  within(s.eye[2] - s.target[2], std::sqrt(0.155 * 0.155 + 0.088 * 0.088), 1e-9),
              "F93e: the Energylink, aim down 21 deg: the head takes 15.5 cm (the eye level with the neck, the whole vector forward), the hips keep 1.3 cm of 16.8");

        // Fists standing (10:28:28-30): eye 1.612 m, the rest vector 15.5 up and 9 forward, neck 1.543: 8.6 cm, taken whole.
        s = stance(1.612, 0.155, 0.090, 1.543);
        t = f93::neckTakesHeight(s.eye, s.target, 0.0, 1.0, s.n0y);
        check(t.applied && within(t.hipsM, 0.0, 1e-9) && within(s.target[1], 1.543, 1e-9), "F93e: fists standing: the 8.6 cm is taken by the head, the hips keep nothing");

        // The rifle hip stance (10:29:00): eye 1.573 m, neck 1.301 m: the rest target is 11.7 cm ABOVE the neck: a raise, untouched.
        s = stance(1.573, 0.155, 0.090, 1.301);
        const double before[3] = {s.target[0], s.target[1], s.target[2]};
        t = f93::neckTakesHeight(s.eye, s.target, 0.0, 1.0, s.n0y);
        check(!t.applied && t.gapM < 0.0 && within(t.takenM, 0.0, 0.0) && s.target[0] == before[0] && s.target[1] == before[1] && s.target[2] == before[2],
              "F93e: the rifle stance (the neck 11.7 cm under the rest target): a raise, the target untouched bit for bit");

        // A turned heading (+x): the move stays in the heading's vertical plane.
        Stance r;
        r.eye[0] = 1.0; r.eye[1] = 1.587; r.eye[2] = -2.0;
        r.target[0] = 1.0 - 0.109; r.target[1] = 1.587 - 0.141; r.target[2] = -2.0 + 0.01;   // 1 cm lateral (the heading is +x: lateral is z)
        r.n0y = 1.540;
        t = f93::neckTakesHeight(r.eye, r.target, 1.0, 0.0, r.n0y);
        check(t.applied && within(r.target[1], 1.540, 1e-9) && within(r.target[2], -2.0 + 0.01, 1e-12) && r.target[0] < 1.0 - 0.109,
              "F93e: a heading along +x: the neck moves back along x and up; the lateral centimetre (z) is kept");

        // Nothing to take: the eye not above the neck, the eye behind it, non-finite input.
        Stance z = stance(1.540, 0.0, 0.10, 1.600);
        t = f93::neckTakesHeight(z.eye, z.target, 0.0, 1.0, z.n0y);
        check(!t.applied && within(t.hipsM, 0.06, 1e-9) && z.target[1] == 1.540, "F93e: the eye level with the neck already: nothing turns, the hips keep the gap");
        Stance bk = stance(1.587, 0.141, -0.05, 1.540);
        t = f93::neckTakesHeight(bk.eye, bk.target, 0.0, 1.0, bk.n0y);
        check(!t.applied && bk.target[1] == 1.587 - 0.141, "F93e: the eye behind the neck (a backward vector): left alone");
        Stance nf = stance(1.587, 0.141, 0.109, std::nan(""));
        t = f93::neckTakesHeight(nf.eye, nf.target, 0.0, 1.0, nf.n0y);
        check(!t.applied && t.gapM == 0.0 && nf.target[1] == 1.587 - 0.141, "F93e: a NaN neck: left alone, no figure");
    }
}

}  // namespace f93test
