// F131 checks (native on-foot VR): look to aim holds while a panel or a menu is open, and while the game runs no look. Header only, run by native_frame_test (run131).
// The flight (edvr_gfx_20261027_064839, 06:59:40 to 07:01): a full-screen menu (gate=panel) juddered under 0.01-0.09 deg asks, on-foot panels followed 45 deg head turns,
// and from 07:00:55 the game ran no look processor while the follower asked +117 deg in a second.
#pragma once

#include "../../src/d3d11/native_on_foot_f109.h"
#include "../../src/d3d11/native_on_foot_f131.h"

#include <cmath>
#include <cstdint>
#include <string>

namespace f131test {

namespace f109 = edvr::nofoot::f109;
namespace f131 = edvr::nofoot::f131;

// A frame loop at 90 Hz: the counters move as given.
struct Rig {
    f131::Hold hold;
    uint64_t now = 100000;
    uint32_t panelSeq = 0, procCalls = 0;
    bool menu = false, procRuns = true, panelUp = false, procKnown = true, active = true;
    bool hookUp = false;      // F131c: the cursor window / owner transform hook runs every frame
    uint64_t hookMs = 0;      // ... and when it last did
    f131::Out last;
    int begins = 0, ends = 0;
    double lastHeldSec = 0.0;
    void frame(uint64_t dtMs = 11) {
        now += dtMs;
        if (panelUp) ++panelSeq;
        if (procRuns) ++procCalls;
        f131::Sample s;
        s.nowMs = now;
        s.active = active;
        s.panelSeq = panelSeq;
        s.menu = menu;
        s.procKnown = procKnown;
        s.procCalls = procCalls;
        if (hookUp) hookMs = now;
        s.menuHookMs = hookMs;
        last = hold.step(s);
        if (last.edge > 0) ++begins;
        if (last.edge < 0) {
            ++ends;
            lastHeldSec = last.heldSec;
        }
    }
    void run(uint64_t ms) {
        for (uint64_t t = 0; t < ms; t += 11) frame();
    }
};

template <class Check>
inline void run131(Check&& check) {
    {   // nothing holds: following, no hold from a counter that was already high or a baseline
        Rig r;
        r.panelSeq = 5000;   // panels reached long ago
        r.procCalls = 7000;
        r.run(2000);
        check(!r.last.held && r.begins == 0, "F131 hold: with no panel, no menu and the processor running there is no hold, whatever the counters stood at when look to aim engaged");
    }
    {   // a panel: held at once, and for 250 ms after the last placement; left 200 ms after that
        Rig r;
        r.run(300);
        r.panelUp = true;
        r.frame();
        check(r.last.held && r.last.edge == +1 && (r.last.reasons & f131::kPanelOpen) && r.begins == 1, "F131 hold: a panel placement holds look to aim at once (the same sample)");
        r.run(1000);
        check(r.last.held && r.begins == 1 && r.ends == 0, "F131 hold: a panel up every frame keeps the hold");
        r.panelUp = false;
        r.run(240);
        check(r.last.held, "F131 hold: 240 ms after the last placement a panel still counts as open");
        r.run(11 * 3);   // past 250 ms; the 200 ms leave begins
        check(r.last.held && r.ends == 0, "F131 hold: after 250 ms the panel is gone but the hold waits out the 200 ms");
        r.run(150);
        check(r.last.held && r.ends == 0, "F131 hold: 176 ms clear is not enough");
        r.run(60);
        check(!r.last.held && r.ends == 1 && r.lastHeldSec > 1.0 && (r.last.seen & f131::kPanelOpen) && r.last.reasons == 0,
              "F131 hold: 200 ms clear ends it, with the seconds held and the reasons it had");
    }
    {   // F131c: the menu hooks themselves (the F111 cursor window, the F114b owner transform): held at once while they run, 250 ms after the last call, then the 200 ms leave
        Rig r;
        r.run(300);
        r.hookUp = true;
        r.frame();
        check(r.last.held && r.last.edge == +1 && (r.last.reasons & f131::kPanelOpen) && r.begins == 1, "F131c: a menu hook running holds look to aim at once, with no placement count moving");
        r.run(2000);
        check(r.last.held && r.begins == 1 && r.ends == 0, "F131c: still held while the hook keeps running (the menu up, the mouse still)");
        r.hookUp = false;
        r.run(11 * 20);   // 220 ms since the hook last ran: a hook seen 100 to 250 ms ago still counts
        check(r.last.held, "F131c: a hook last seen 220 ms ago still holds");
        r.run(11 * 4);   // past 250 ms; the 200 ms leave starts
        check(r.last.held && r.ends == 0, "F131c: past 250 ms the reason is gone but the hold waits out the 200 ms");
        r.run(11 * 25);
        check(!r.last.held && r.ends == 1 && (r.last.seen & f131::kPanelOpen), "F131c: and ends 200 ms after that");
        Rig q;   // a hook last seen 100 ms ago holds; 300 ms ago with nothing else does not
        q.run(300);
        q.hookMs = q.now - 100;
        q.frame();
        const bool nearHeld = q.last.held;
        Rig z;
        z.run(300);
        z.hookMs = z.now - 300;
        z.frame();
        check(nearHeld && !z.last.held && z.begins == 0, "F131c: a hook seen 100 ms ago holds; seen 300 ms ago, with nothing else, does not");
        Rig w;   // the placement count alone still holds (kept), and the hook never having run is no reason
        w.run(300);
        w.panelUp = true;
        w.frame();
        Rig n;
        n.run(1000);
        check(w.last.held && !n.last.held && n.hookMs == 0, "F131c: the placement count still holds on its own, and a hook that never ran is no reason");
    }
    {   // a menu or map: held while the maps gate holds the 2D screen as a panel, 200 ms after it lets go
        Rig r;
        r.run(200);
        r.menu = true;
        r.frame();
        check(r.last.held && r.last.edge == +1 && (r.last.reasons & f131::kMenuOrMap), "F131 hold: the maps gate holding the 2D screen holds look to aim at once");
        r.run(5000);
        r.menu = false;
        r.run(11 * 17);
        check(r.last.held && r.ends == 0, "F131 hold: 187 ms after the menu left it is still held");
        r.run(40);
        check(!r.last.held && r.ends == 1 && (r.last.seen & f131::kMenuOrMap), "F131 hold: and 200 ms after it, not");
    }
    {   // the processor stalls: held after 150 ms with no call of the player's own, only while the look hooks are in
        Rig r;
        r.run(500);
        r.procRuns = false;
        r.run(11 * 13);   // 143 ms without a call
        check(!r.last.held, "F131 hold: 143 ms without a processor call is not yet a stall");
        r.run(11 * 2);
        check(r.last.held && (r.last.reasons & f131::kNoLook) && r.begins == 1, "F131 hold: 150 ms without a processor call holds (the 07:00:55 flight: comps now 0, +117 deg asked, 0 applied)");
        r.run(3000);
        check(r.last.held && r.begins == 1, "F131 hold: it stays held while the processor stays silent");
        r.procRuns = true;
        r.frame();
        check(r.last.held && r.last.reasons == 0, "F131 hold: the first call back clears the reason, the hold waits out the 200 ms");
        r.run(220);
        check(!r.last.held && r.ends == 1, "F131 hold: and ends 200 ms after the processor came back");
        Rig q;
        q.procKnown = false;   // the look hooks are not in: no processor call can be judged
        q.procRuns = false;
        q.run(2000);
        check(!q.last.held && q.begins == 0, "F131 hold: with the look hooks not in, a silent processor is not a reason");
    }
    {   // no flapping: a reason back inside the 200 ms keeps the same hold; two reasons at once, one after the other
        Rig r;
        r.run(100);
        r.menu = true;
        r.frame();
        r.menu = false;
        r.run(11 * 10);   // 110 ms clear
        r.menu = true;
        r.frame();
        r.menu = false;
        r.run(11 * 10);
        r.run(11 * 10);
        check(r.begins == 1 && r.ends == 1, "F131 hold: a reason that returns inside the 200 ms is the same hold (one begin, one end)");
        Rig q;
        q.run(100);
        q.menu = true;
        q.panelUp = true;
        q.frame();
        q.menu = false;
        q.run(2000);
        q.panelUp = false;
        q.run(1000);
        check(q.begins == 1 && q.ends == 1 && (q.last.seen & (f131::kMenuOrMap | f131::kPanelOpen)) == (f131::kMenuOrMap | f131::kPanelOpen),
              "F131 hold: a menu and a panel one after the other are one hold with both reasons");
    }
    {   // look to aim stopping forgets the hold; engaging again takes new baselines (no hold from counters that moved meanwhile)
        Rig r;
        r.run(100);
        r.menu = true;
        r.run(100);
        r.active = false;
        r.frame();
        check(!r.last.held && r.ends == 0 && !r.hold.held, "F131 hold: look to aim not engaged: no hold, nothing ended (the caller's engage state is its own)");
        r.menu = false;
        r.panelSeq += 900;
        r.active = true;
        r.run(300);
        check(!r.last.held && r.begins == 1, "F131 hold: engaging again takes the counters as they stand: no hold from panels reached while off");
    }
    {   // the follower asks nothing while held: no ask, no probe, the probe's phase not advanced
        f109::Follower f;
        f.target[0] = 40.0 * f109::kDeg;   // a 40 deg head turn waiting
        f.target[1] = -10.0 * f109::kDeg;
        f109::ProbePair probe;
        double ay = 9.0, ap = 9.0;
        f131::followAsk(f131::followMode(true, false), f, probe, true, 0.011, 0.0, 0.0, 0.0, &ay, &ap);
        const bool heldNothing = ay == 0.0 && ap == 0.0 && !probe.undo;
        f131::followAsk(f131::followMode(true, true), f, probe, true, 0.011, 0.0, 0.0, 0.0, &ay, &ap);
        const bool heldBlockedNothing = ay == 0.0 && ap == 0.0 && !probe.undo;
        double fy = 0.0, fp = 0.0;
        f131::followAsk(f131::followMode(false, false), f, probe, true, 0.011, 0.0, 0.0, 0.0, &fy, &fp);
        double by = 0.0, bp = 0.0, cy = 0.0, cp = 0.0;
        f131::followAsk(f131::followMode(false, true), f, probe, true, 0.011, 0.0, 0.0, 0.0, &by, &bp);
        f131::followAsk(f131::followMode(false, true), f, probe, false, 0.011, 0.0, 0.0, 0.0, &cy, &cp);   // between probes: nothing
        check(heldNothing && heldBlockedNothing, "F131 follower: held asks nothing at all, no ask and no probe, even blocked, with 40 deg of head turn waiting; the probe's phase stays");
        check(fy > 0.0 && fp < 0.0 && std::fabs(by - f109::kProbeRad) < 1e-12 && bp < 0.0 && probe.undo && cy == 0.0 && cp == 0.0,
              "F131 follower: not held it asks (rate-capped) when following, and probes (0.25 deg, zero-sum phase) when blocked");
        char buf[200], shortBuf[60];
        f131::reasonText(f131::kPanelOpen | f131::kNoLook, buf, sizeof(buf));
        f131::reasonShort(f131::kMenuOrMap | f131::kPanelOpen, shortBuf, sizeof(shortBuf));
        const std::string none = f131::reasonShort(0, shortBuf + 30, 20);
        check(std::string(buf) == "an on-foot panel or menu is open + the game's look processor has not run for 150 ms" && std::string(shortBuf) == "panel+menu" && none == "none",
              "F131 lines: the reasons read as text, short for the 1 s line");
    }
}

}  // namespace f131test
