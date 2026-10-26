// F99a checks (native on-foot VR): a drawn item's hands from the game's own selection, not from Status.json. Header only, run by native_frame_test (run99).
// The F98 flight (edvr_gfx_20261024_192353): Status.json named a drawn tool 288-686 ms after the game's own selection; the hands waited for it, then ramped 0.3 s.
#pragma once

#include "../../src/d3d11/native_on_foot_core.h"
#include "../../src/d3d11/native_on_foot_f99.h"

#include <cmath>

namespace f99test {

namespace f99 = edvr::nofoot::f99;
namespace nf = edvr::nofoot;

constexpr double kDt = 1.0 / 90.0;

inline f99::BridgeIn bin(uint32_t id, bool status, bool active = true, bool ok = true) {
    f99::BridgeIn in;
    in.active = active;
    in.nativeOk = ok;
    in.nativeId = id;
    in.nativeHeld = id != f99::kNoItem;
    in.statusHeld = status;
    in.dt = kDt;
    return in;
}

// One draw at 90 Hz: the game selects the item at t = 0, Status.json names it at lead s. Returns when the armed ramp (F66a's 0.3 s) is full, s; the bridge on
// or off (off: the flag from Status.json alone, as until F99a).
inline double fullAt(double lead, bool bridgeOn) {
    f99::Bridge b;
    nf::IkBlend ramp{0.0f, nf::kArmedRampSec};
    b.step(bin(f99::kNoItem, false));
    for (int i = 0; i < 400; ++i) {
        const double t = i * kDt;
        const bool status = t >= lead - 1e-9;
        const f99::BridgeOut o = b.step(bin(5u, status));
        ramp.step(status || (bridgeOn && o.armed), kDt);
        if (ramp.w >= 1.0f) return t;
    }
    return 99.0;
}

template <class Check>
inline void run99(Check&& check) {
    {   // the first selection read is not a draw; a new item selected with Status.json naming none arms at once
        f99::Bridge b;
        const f99::BridgeOut first = b.step(bin(4u, false));
        f99::Bridge c;
        c.step(bin(f99::kNoItem, false));
        const f99::BridgeOut draw = c.step(bin(5u, false));
        check(!first.armed && !first.started && draw.started && draw.armed,
              "F99a: the first selection read after the scope opens is not a draw; a new item selected while Status.json names none arms the hands at once");
        f99::BridgeOut held = draw;
        for (int i = 0; i < 45; ++i) held = c.step(bin(5u, false));
        const f99::BridgeOut named = c.step(bin(5u, true));
        check(held.armed && !named.armed && named.confirmed, "F99a: the bridge holds until Status.json names the item (0.5 s here), then the flag is Status.json's");
    }
    {   // Status.json never names it: released after kBridgeMaxS; the game puts it away: released at once
        f99::Bridge b;
        b.step(bin(f99::kNoItem, false));
        b.step(bin(6u, false));
        bool armedAt19 = false, expired = false;
        for (int i = 1; i <= 200 && !expired; ++i) {
            const f99::BridgeOut o = b.step(bin(6u, false));
            if (i == 171) armedAt19 = o.armed;
            expired = o.expired;
        }
        f99::Bridge p;
        p.step(bin(f99::kNoItem, false));
        p.step(bin(4u, false));
        const f99::BridgeOut away = p.step(bin(f99::kNoItem, false));
        check(armedAt19 && expired && away.putAway && !away.armed,
              "F99a: not named by Status.json within 2 s the bridge releases; the game putting the item away releases it at once");
    }
    {   // a swap (Status.json names an item already), the same selection read again, the scope closing: no bridge
        f99::Bridge s;
        s.step(bin(5u, true));
        const f99::BridgeOut swap = s.step(bin(4u, true));
        f99::Bridge r;
        r.step(bin(f99::kNoItem, false));
        r.step(bin(5u, false));
        r.step(bin(5u, true));   // named: the bridge is done
        const f99::BridgeOut again = r.step(bin(5u, false));   // the same item read again (Status.json lagging the holster)
        f99::Bridge x;
        x.step(bin(f99::kNoItem, false));
        x.step(bin(5u, false));
        const f99::BridgeOut off = x.step(bin(5u, false, false));
        const f99::BridgeOut back = x.step(bin(5u, false));
        check(!swap.started && !swap.armed && !again.started && !again.armed && !off.armed && !back.armed && !back.started,
              "F99a: no bridge on a swap, on the same selection read again, or for the first selection after the scope opens again");
    }
    {   // the F98 flight's four draws from the fists: the armed ramp full 0.30 s after the game's selection, against Status.json's lead plus 0.30 s
        const double leads[4] = {0.686, 0.432, 0.352, 0.288};
        bool with = true, without = true;
        for (double lead : leads) {
            with = with && fullAt(lead, true) <= 0.31;
            without = without && fullAt(lead, false) >= lead + 0.28;
        }
        check(with && without, "F99a: the F98 flight's draws (Status.json 288-686 ms late): the hands at full weight 0.30 s after the game's selection, not 0.58-0.99 s");
    }
    {   // the instrument: a draw's record from the game's selection, the authority's source and times; a build whose flag waits for Status.json reads as such
        const auto run = [](bool bridgeOn, f99::OnsetRecord* rec) {
            f99::Bridge b;
            f99::Onset on;
            nf::IkBlend ramp{0.0f, nf::kArmedRampSec};
            bool closed = false;
            for (int i = 0; i < 300 && !closed; ++i) {
                const double t = i * kDt;
                const uint32_t id = i == 0 ? f99::kNoItem : 5u;
                const bool status = i > 0 && t >= 0.5;
                const f99::BridgeOut o = b.step(bin(id, status));
                const bool armed = status || (bridgeOn && o.armed);
                ramp.step(armed, kDt);
                f99::OnsetIn oi;
                oi.active = oi.nativeOk = true;
                oi.nativeId = id;
                oi.nativeHeld = id != f99::kNoItem;
                oi.statusHeld = status;
                oi.armed = armed;
                oi.source = bridgeOn && o.armed ? f99::kFromSelection : f99::kFromStatus;
                oi.w = ramp.w;
                oi.moving = true;
                oi.dt = kDt;
                closed = on.step(oi, rec);
            }
            return closed;
        };
        f99::OnsetRecord fixed, late;
        const bool a = run(true, &fixed), c = run(false, &late);
        check(a && fixed.native && fixed.moving && fixed.source == f99::kFromSelection && fixed.tOnset < 0.02 && std::fabs(fixed.tStatus - 0.5) < 0.02 &&
                  fixed.tFull < 0.32,
              "F99a instrument: the draw's record opens at the game's selection, the hands' authority from it at once, Status.json at +0.5 s, full at +0.3 s");
        check(c && late.native && late.source == f99::kFromStatus && std::fabs(late.tOnset - 0.5) < 0.02 && late.tFull > 0.75,
              "F99a instrument (control): with the flag from Status.json alone the record shows the authority from it at the game's lead (never applied)");
    }
}

}  // namespace f99test
