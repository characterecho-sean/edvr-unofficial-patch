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

// ---- F160: the holster bridge (HolsterBridge) -------------------------------------------------------------------------------------------------------------------------------

inline f99::HolsterIn hin(uint32_t id, bool status, bool ok = true, bool active = true) {
    f99::HolsterIn in;
    in.active = active;
    in.nativeOk = ok;
    in.nativeId = id;
    in.nativeHeld = id != f99::kNoItem;
    in.statusHeld = status;
    in.dt = kDt;
    return in;
}

// A holster at 90 Hz: the game selects an item at the first read, reads none at t = 0, Status.json lets the item go at lead s. Returns when the armed flag's ramp (F66a's 0.3 s)
// is empty after the none read, s; the bridge on or off (off: the flag from Status.json alone, as until F160).
inline double emptyAt(double lead, bool bridgeOn) {
    f99::HolsterBridge b;
    nf::IkBlend ramp{1.0f, nf::kArmedRampSec};
    b.step(hin(5u, true));
    for (int i = 0; i < 600; ++i) {
        const double t = i * kDt;
        const bool status = t < lead - 1e-9;
        const f99::HolsterOut o = b.step(hin(f99::kNoItem, status));
        ramp.step(status && !(bridgeOn && o.running), kDt);
        if (ramp.w <= 0.0f) return t;
    }
    return 99.0;
}

template <class Check>
inline void run160Holster(Check&& check) {
    {   // the start: a none id read after a held one while Status.json still names an item; the bridge holds while it runs
        f99::HolsterBridge b;
        const f99::HolsterOut first = b.step(hin(5u, true));
        const f99::HolsterOut edge = b.step(hin(f99::kNoItem, true));
        f99::HolsterOut later = edge;
        for (int i = 0; i < 90; ++i) later = b.step(hin(f99::kNoItem, true));
        check(!first.started && !first.running && edge.started && edge.running && later.running && !later.started && later.ended == f99::kHolsterNotEnded,
              "F160 holster: a none selection read after a held one while Status.json still names an item starts the bridge, which holds the flag false while it runs");
    }
    {   // the three ends: Status.json lets go (the flight's leads), the game selects an item again, 4 s without either
        const double leads[3] = {1.35, 0.97, 3.08};
        bool status = true;
        for (double lead : leads) {
            f99::HolsterBridge b;
            b.step(hin(5u, true));
            f99::HolsterOut o = b.step(hin(f99::kNoItem, true));
            f99::HolsterOut end;
            for (int i = 1; i < 600 && end.ended == f99::kHolsterNotEnded; ++i) {
                o = b.step(hin(f99::kNoItem, i * kDt < lead - 1e-9));
                if (o.ended != f99::kHolsterNotEnded) end = o;
            }
            status = status && end.ended == f99::kHolsterStatus && !end.running && std::fabs(end.ranS - lead) < 2.0 * kDt && end.fromId == 5u;
        }
        check(status, "F160 holster: the bridge ends when Status.json no longer names an item (1.35, 0.97 and 3.08 s here) and says how long it ran, to a frame");
        f99::HolsterBridge d;
        d.step(hin(5u, true));
        d.step(hin(f99::kNoItem, true));
        for (int i = 0; i < 30; ++i) d.step(hin(f99::kNoItem, true));
        const f99::HolsterOut drawn = d.step(hin(6u, true));
        f99::HolsterBridge s;
        s.step(hin(4u, true));
        s.step(hin(f99::kNoItem, true));
        const f99::HolsterOut same = s.step(hin(4u, true));
        check(drawn.ended == f99::kHolsterDrawn && !drawn.running && std::fabs(drawn.ranS - 31.0 * kDt) < 1e-9 && same.ended == f99::kHolsterDrawn && !same.running,
              "F160 holster: the game selecting an item again (a draw of another item or of the same one) ends the bridge at once: the flag is Status.json's");
        f99::HolsterBridge t;
        t.step(hin(5u, true));
        t.step(hin(f99::kNoItem, true));
        f99::HolsterOut at3, end;
        for (int i = 1; i < 600 && end.ended == f99::kHolsterNotEnded; ++i) {
            const f99::HolsterOut o = t.step(hin(f99::kNoItem, true));
            if (i == 330) at3 = o;
            if (o.ended != f99::kHolsterNotEnded) end = o;
        }
        check(at3.running && end.ended == f99::kHolsterTimeout && !end.running && end.ranS > f99::kHolsterMaxS && end.ranS < f99::kHolsterMaxS + 2.0 * kDt,
              "F160 holster: 4 s without Status.json letting go or a new selection ends the bridge (a timeout: the flag is Status.json's again)");
        const f99::HolsterOut after = t.step(hin(f99::kNoItem, true));
        check(!after.started && !after.running, "F160 holster: a bridge that timed out does not start again on the same none selection");
    }
    {   // no start on a swap (one held id straight to another), without a held edge, or without Status.json naming an item
        f99::HolsterBridge s;
        s.step(hin(5u, true));
        const f99::HolsterOut swap = s.step(hin(4u, true));
        const f99::HolsterOut swapped = s.step(hin(4u, true));
        f99::HolsterBridge n;
        const f99::HolsterOut firstNone = n.step(hin(f99::kNoItem, true));   // the first read after the scope opens is none: no held edge
        const f99::HolsterOut againNone = n.step(hin(f99::kNoItem, true));
        f99::HolsterBridge u;
        u.step(hin(5u, false));
        const f99::HolsterOut unnamed = u.step(hin(f99::kNoItem, false));      // Status.json names no item: the flag is already false
        f99::HolsterBridge d;
        d.step(hin(f99::kNoItem, true));
        const f99::HolsterOut drawFromNone = d.step(hin(5u, true));            // a draw is not a holster
        check(!swap.started && !swap.running && !swapped.running && !firstNone.started && !againNone.started && !unnamed.started && !unnamed.running && !drawFromNone.started,
              "F160 holster: no bridge on a swap, on a none read with no held read before it, when Status.json names no item, or on a draw");
    }
    {   // a stale or unreadable selection read is ignored: it neither starts nor ends a bridge, and the held edge survives it
        f99::HolsterBridge b;
        b.step(hin(5u, true));
        const f99::HolsterOut stale = b.step(hin(f99::kNoItem, true, false));   // the read was stale, unreadable or suspended: the id is not evidence
        const f99::HolsterOut staleHeld = b.step(hin(6u, true, false));
        const f99::HolsterOut edge = b.step(hin(f99::kNoItem, true));            // the next good read: the held edge from before the bad reads
        f99::HolsterOut during = edge;
        for (int i = 0; i < 20; ++i) during = b.step(hin(7u, true, false));     // a held id read badly while it runs does not end it
        for (int i = 0; i < 20; ++i) during = b.step(hin(f99::kNoItem, true, false));
        check(!stale.started && !stale.running && !staleHeld.started && edge.started && during.running && during.ended == f99::kHolsterNotEnded,
              "F160 holster: a read that is not fresh, readable and unsuspended starts nothing and ends nothing; the held edge before it still counts");
        f99::HolsterBridge x;
        x.step(hin(5u, true));
        x.step(hin(f99::kNoItem, true));
        f99::HolsterOut y;
        for (int i = 0; i < 380; ++i) {
            y = x.step(hin(f99::kNoItem, true, false));
            if (y.ended != f99::kHolsterNotEnded) break;
        }
        check(y.ended == f99::kHolsterTimeout, "F160 holster: with the selection unreadable the 4 s limit still returns the flag to Status.json");
    }
    {   // the scope closing resets the bridge; the first read after it is not an edge
        f99::HolsterBridge b;
        b.step(hin(5u, true));
        b.step(hin(f99::kNoItem, true));
        const f99::HolsterOut off = b.step(hin(f99::kNoItem, true, true, false));
        const f99::HolsterOut back = b.step(hin(f99::kNoItem, true));
        check(!off.running && off.ended == f99::kHolsterNotEnded && !back.started && !back.running,
              "F160 holster: leaving the on-foot scope drops a running bridge without an end, and the first read after it starts nothing");
    }
    {   // the armed flag's ramp: empty 0.3 s after the game's none selection, against Status.json's lead plus 0.3 s (the flight's three holsters)
        const double leads[3] = {1.35, 0.97, 3.08};
        bool with = true, without = true;
        for (double lead : leads) {
            with = with && emptyAt(lead, true) <= 0.31;
            without = without && emptyAt(lead, false) >= lead + 0.28;
        }
        check(with && without, "F160 holster: the flight's holsters (Status.json 0.97-3.08 s late): the armed ramp empty 0.30 s after the game's none selection, not 1.3-3.4 s");
    }
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
    run160Holster(check);
}

}  // namespace f99test
