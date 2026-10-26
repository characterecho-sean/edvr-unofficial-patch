// F93d checks (native on-foot VR), run93de below (f93_checks.h holds F93b's run93). Header only, run by native_frame_test. F93e's checks went with F93e (F94a retired
// it; f94_checks.h checks the body at its animated height on the same standing figures).
//
// F93d: the published camera (native_on_foot.h: nativeOnFootCameraPublish, nativeOnFootCameraExternal, nativeOnFootBodyCullWanted) and the frame thread's watch
// (native_on_foot_f93de.h, CameraWatch) through the F92 flight's camera-suite stretch: on foot, the suite opened (VanityCamera started, 10:31:17.355), its presets and
// its free camera, closed again (10:31:43.539), the controller going quiet with the suite open, native stereo on foot ending inside the suite.
#pragma once

#include "../../src/d3d11/native_on_foot.h"
#include "../../src/d3d11/native_on_foot_f93de.h"

#include <cmath>
#include <cstdint>

namespace f93detest {

namespace f93 = edvr::nofoot::f93;

template <class Check>
inline void run93de(Check&& check) {
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
}

}  // namespace f93detest
