// F145 checks (native on-foot VR), run145 below. Header only, run by native_frame_test.
//
// F145 (native_on_foot_f145.h, native_on_foot_f144.h): (1) the weapon UI's tag slots are given back: the pick (hit, free, the one asked for longest ago once idle long enough,
// none while every slot is in use), the F144 flight's session replayed (eleven space-1 part names over four loadouts into eight slots: the plasma pistol's ADS readout and the
// scoped sniper's parts get slots), the event ring; (2) the proof against the seed: the seed-only map, and on the F85 two-eye rig the drawn move of a trim read against the same
// frame at the seed alone equals the trim within 1 mm, in a scaled (ADS) role and in the pistol's ADS role, while the move against the game's own draw does not.
#pragma once

#include "../../src/d3d11/native_on_foot_f145.h"
#include "f144_checks.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace f145test {

namespace f145 = edvr::nofoot::f145;
namespace f144 = edvr::nofoot::f144;
namespace f90 = edvr::nofoot::f90;
namespace f85 = edvr::nofoot::f85;

// A small copy of the submit hook's slot table, driving pickSlot the way f85bSlotOf does.
struct Table {
    static constexpr uint32_t kN = 8;
    uint32_t names[kN] = {};
    uint64_t seen[kN] = {};
    uint32_t recycles = 0, noSlot = 0;
    int ask(uint16_t nameId, uint64_t nowMs) {
        const uint32_t want = nameId + 1u;
        const f145::SlotPick p = f145::pickSlot(names, seen, kN, want, nowMs);
        switch (p.how) {
            case f145::kSlotHit:
            case f145::kSlotFree:
                names[p.slot] = want;
                seen[p.slot] = nowMs;
                return p.slot;
            case f145::kSlotRecycled:
                names[p.slot] = want;
                seen[p.slot] = nowMs;
                ++recycles;
                return p.slot;
            default:
                ++noSlot;
                return -1;
        }
    }
};

inline bool closeTo(double a, double b, double tol) { return std::fabs(a - b) <= tol; }

template <class Check>
inline void run145(Check&& check) {
    // ---- 1. the slot pick ------------------------------------------------------------------------------------------------------------------------------------------
    {
        uint32_t names[4] = {11, 22, 33, 44};
        uint64_t seen[4] = {1000, 2000, 3000, 4000};
        f145::SlotPick p = f145::pickSlot(names, seen, 4, 33, 4500);
        check(p.how == f145::kSlotHit && p.slot == 2, "F145: a name that holds a slot finds it (a hit)");
        p = f145::pickSlot(names, seen, 4, 55, 4500);
        check(p.how == f145::kSlotNone && p.slot == -1, "F145: every slot asked for within 5 s (the oldest 3.5 s ago): a new name has none");
        p = f145::pickSlot(names, seen, 4, 55, 6000);
        check(p.how == f145::kSlotRecycled && p.slot == 0 && p.oldName == 11 && p.idleMs == 5000, "F145: the oldest slot unasked for exactly 5 s goes to the new name (the one asked for longest ago)");
        p = f145::pickSlot(names, seen, 4, 55, 5999);
        check(p.how == f145::kSlotNone, "F145: ... not at 4.999 s");
        seen[0] = 5500;
        seen[2] = 100;
        p = f145::pickSlot(names, seen, 4, 55, 20000);
        check(p.how == f145::kSlotRecycled && p.slot == 2 && p.oldName == 33, "F145: the slot asked for longest ago is the one given up, whatever its index");
        names[1] = 0;
        p = f145::pickSlot(names, seen, 4, 55, 4500);
        check(p.how == f145::kSlotFree && p.slot == 1, "F145: a free slot is taken before any is recycled");
        p = f145::pickSlot(names, seen, 4, 0, 4500);
        check(p.how == f145::kSlotNone, "F145: no name asked for, no slot");
        p = f145::pickSlot(nullptr, seen, 4, 5, 4500);
        check(p.how == f145::kSlotNone, "F145: no table, no slot");
        uint32_t clockBack[2] = {7, 8};
        uint64_t seenAhead[2] = {9000, 9000};
        check(f145::pickSlot(clockBack, seenAhead, 2, 9, 1000).how == f145::kSlotNone, "F145: a tick before the last one reads no idle time");
    }
    // ---- the F144 flight's session: names in the order the submit hook saw them, one slot each for the whole session before ------------------------------------------------
    {
        // {time (ms), space-1 part names submitted at 90 fps from then until the next row}
        struct Step {
            uint64_t t;
            uint16_t names[3];
        };
        const Step flight[] = {
            {53000, {30903, 30915, 0}},        // 19:29:53 "Rocket man": fists, rocket launcher
            {135000, {30903, 30901, 0}},       // 19:31:15 the laser pistol
            {248000, {30040, 32689, 0}},       // 19:33:08 "Combat": the kinetic rifle
            {418000, {30040, 29134, 0}},       // 19:34:58 the kinetic pistol
            {464000, {31800, 29237, 0}},       // 19:35:44 "Combat Plasma": the shotgun
            {464000 + 120000, {29237, 31837, 0}},   // 19:37:44 the plasma pistol (its ADS readout is a new name)
            {464000 + 181000, {32832, 29157, 0}},   // 19:38:46 "Nightvision": the scoped sniper (two new names)
        };
        Table t;
        bool allSlots = true;
        int pistolAds = -2, sniper0 = -2, sniper1 = -2;
        for (const Step& st : flight)
            for (uint64_t ms = st.t; ms < st.t + 4000; ms += 11) {   // 90 fps for 4 s
                for (uint16_t n : st.names) {
                    if (!n) continue;
                    const int slot = t.ask(n, ms);
                    allSlots = allSlots && slot >= 0;
                    if (n == 31837) pistolAds = slot;
                    if (n == 32832) sniper0 = slot;
                    if (n == 29157) sniper1 = slot;
                }
            }
        check(allSlots && t.noSlot == 0 && t.recycles == 3, "F145: the F144 flight replayed: every part gets a slot (eleven names, eight slots, three recycles)");
        check(pistolAds >= 0 && sniper0 >= 0 && sniper1 >= 0 && sniper0 != sniper1, "F145: the plasma pistol's ADS readout (31837) and the sniper's two parts (32832, 29157) are tagged");
        // before F145 a name had its slot for good: the same replay finds the ninth name through the eleventh without one
        uint32_t oldNames[8] = {};
        int unslotted = 0;
        for (const Step& st : flight)
            for (uint16_t n : st.names) {
                if (!n) continue;
                bool have = false;
                for (uint32_t& x : oldNames) have = have || x == n + 1u;
                for (uint32_t& x : oldNames)
                    if (!have && x == 0) {
                        x = n + 1u;
                        have = true;
                    }
                if (!have) ++unslotted;
            }
        check(unslotted == 3, "F145: ... the old table, replayed, left 31837, 32832 and 29157 without a slot (what the F144 log showed)");
        // a held weapon's parts keep their slots while they are submitted
        Table u;
        for (uint16_t n = 100; n < 108; ++n) u.ask(n, 0);
        for (uint64_t ms = 1000; ms < 40000; ms += 1000) {
            u.ask(100, ms);
            u.ask(101, ms);
        }
        const int s100 = u.ask(100, 40000), s101 = u.ask(101, 40000);
        const int fresh = u.ask(200, 41000);
        check(s100 == 0 && s101 == 1 && fresh >= 2 && u.recycles == 1, "F145: two parts submitted every second keep their slots; a new name takes an idle one");
        Table full;
        for (uint16_t n = 100; n < 108; ++n) full.ask(n, 1000);
        check(full.ask(300, 3000) == -1 && full.noSlot == 1, "F145: every slot in use within 5 s: the ninth name finds none (said once by the caller)");
    }
    // ---- the event ring --------------------------------------------------------------------------------------------------------------------------------------------
    {
        f145::EventRing<4> ring;
        f145::SlotEvent ev;
        check(!ring.take(&ev), "F145: an empty ring has nothing");
        bool pushed = true;
        for (uint16_t n = 1; n <= 4; ++n) {
            ev = f145::SlotEvent{};
            ev.kind = f145::kEvRecycled;
            ev.name = n;
            ev.slot = static_cast<uint8_t>(n);
            ev.oldName = static_cast<uint16_t>(n + 10);
            ev.idleMs = 7000u + n;
            pushed = pushed && ring.push(ev);
        }
        check(pushed && !ring.push(ev), "F145: a full ring refuses a fifth event");
        bool order = true;
        for (uint16_t n = 1; n <= 4; ++n) order = order && ring.take(&ev) && ev.name == n && ev.slot == n && ev.oldName == n + 10 && ev.idleMs == 7000u + n && ev.kind == f145::kEvRecycled;
        check(order && !ring.take(&ev) && ring.push(ev), "F145: events come out in the order they went in, and the freed room is usable again");
    }
    // ---- 2. the proof against the seed -----------------------------------------------------------------------------------------------------------------------------
    {
        namespace f90t = f90test;
        const f90t::Rig g = f90t::makeRig();
        const auto rot = edvr::drawtag::rotateByPackedQuat;
        const f90t::Weapon wp = f90t::weaponOf(g, 7.0);
        const auto frameFn = [&] { return f90::weaponFrame(true, wp.attach, true, wp.froot); };
        // Where the patched rows draw the part's content centre: the proof's "drawn" (what f85bDecide computes from the rows it writes).
        const auto drawnOf = [&](int e, const f90::Map& m, const f90t::Record& rec, double out[3]) {
            f85::UFrame uf = f90t::uOf(g, e);
            f90::applyMap(uf, m);
            float r[12];
            if (!f90t::rowsOf(g, e, uf, r)) return false;
            double cv[3];
            f90t::contentHud(g, e, rec, cv);
            return f145::drawnPoint(g.K[e], g.B[e], f85::rowsOfComposite(r, r + 4, r + 8), cv, out);
        };
        // F144's reading: the camera solved from the patched rows and the centre through it (kept here to show what it got wrong in a scaled role)
        const auto solvedOf = [&](int e, const f90::Map& m, const f90t::Record& rec, double out[3]) {
            f85::UFrame uf = f90t::uOf(g, e);
            f90::applyMap(uf, m);
            float r[12];
            if (!f90t::rowsOf(g, e, uf, r)) return false;
            const f85::CamK k = f85::solveK(f85::rowsOfComposite(r, r + 4, r + 8), g.W[e], g.aff);
            if (!k.ok) return false;
            double cv[3];
            f90t::contentHud(g, e, rec, cv);
            f85::mulVec3(k.K3, cv, out);
            for (int i = 0; i < 3; ++i) out[i] += k.k4[i];
            return true;
        };
        // 2a. the rifle's ADS readout: a size factor about the content centre and a move; seed (3.5, 5.5, 0, 0), total = seed + 4 cm forward + 1.5 cm left
        {
            const f90t::Record ads{f90t::kN89RifleAds, {-0.34, -0.05, 0.52}, 0.0501, {3.0, 1.0}};
            f90::Keys seed, total;
            seed.forwardCm = 3.5;
            seed.leftCm = 5.5;
            total = seed;
            total.forwardCm += 4.0;
            total.leftCm += 1.5;
            bool ok = true;
            double worstSeed = 0.0, worstGame = 0.0, fwd = 0.0, left = 0.0, gameAbs = 0.0, expTrim = 0.0, solvedOff = 0.0, unscaledOff = 0.0;
            for (int e = 0; e < 2; ++e) {
                f90::Latch latch;
                f90::In in = f90t::inOf(g, e, 100, ads, f90::kItemPrimary, 7, f90::Keys{});
                in.roleKeys = true;
                in.rifleKeys = total;
                const f90::Out o = f90::decide(in, f90t::uOf(g, e), latch, rot, frameFn);
                ok = ok && o.b == f90::kBMoved && o.map.scale && o.map.move && o.haveWf;
                f90::Map seedMap;
                ok = ok && f145::seedOnlyMap(o, seed, in.eyeOk ? in.eye : nullptr, &seedMap) && seedMap.scale && seedMap.move && seedMap.k == o.map.k;
                double drawn[3], drawnSeed[3], expTrimV[3], expGame[3] = {};
                ok = ok && drawnOf(e, o.map, ads, drawn) && drawnOf(e, seedMap, ads, drawnSeed);
                if (!ok) break;
                f145::trimMove(o.map, seedMap, expTrimV);
                std::memcpy(expGame, o.map.t, sizeof(expGame));
                double game[3];
                for (int i = 0; i < 3; ++i) game[i] = o.C[i] - in.ow[i];
                const f144::Move vsSeed = f144::moveOf(drawn, drawnSeed, expTrimV, o.wfUsed.b, o.wfUsed.l, o.wfUsed.u);
                const f144::Move vsGame = f144::moveOf(drawn, game, expGame, o.wfUsed.b, o.wfUsed.l, o.wfUsed.u);
                worstSeed = std::max(worstSeed, vsSeed.diffMm);
                worstGame = std::max(worstGame, vsGame.diffMm);
                fwd = vsSeed.cm[0];
                left = vsSeed.cm[1];
                gameAbs = vsGame.absCm;
                expTrim = vsSeed.expAbsCm;
                // F144's reading of the same rows: the solved camera's centre, against the true one
                double solved[3];
                ok = ok && solvedOf(e, o.map, ads, solved);
                double d[3] = {solved[0] - drawn[0], solved[1] - drawn[1], solved[2] - drawn[2]};
                solvedOff = std::max(solvedOff, 100.0 * std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]));
                // and a plain scale with no move (the ADS role at the hip with a zero seed would read the same)
                f90::Map scaleOnly = o.map;
                scaleOnly.move = false;
                double sc[3], scs[3];
                ok = ok && drawnOf(e, scaleOnly, ads, sc) && solvedOf(e, scaleOnly, ads, scs);
                d[0] = scs[0] - sc[0]; d[1] = scs[1] - sc[1]; d[2] = scs[2] - sc[2];
                unscaledOff = std::max(unscaledOff, 100.0 * std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]));
            }
            std::printf("native_frame_test: F145 rifle ADS rig: vs the seed alone forward %.3f left %.3f cm (trims ask 4.000 / 1.500; worst %.4f mm); vs the game's draw %.2f cm, worst %.4f mm; "
                        "F144's solved-camera reading was %.2f cm off (a scale alone: %.2f cm)\n", fwd, left, worstSeed, gameAbs, worstGame, solvedOff, unscaledOff);
            check(ok && worstSeed < 1.0 && closeTo(fwd, 4.0, 0.02) && closeTo(left, 1.5, 0.02) && closeTo(expTrim, std::sqrt(4.0 * 4.0 + 1.5 * 1.5), 0.02),
                  "F145: in the rifle's ADS role (a size factor and a move) the drawn move against the same frame at the seed alone is the trims' 4 cm forward and 1.5 cm left, within 1 mm, in both eyes");
            check(ok && worstGame < 1.0 && gameAbs > 5.0,
                  "F145: ... and the move against the game's own draw is the whole total (seed and trims, over 5 cm here) within 1 mm: the size factor does not move the content centre");
            check(ok && solvedOff > 5.0 && unscaledOff > 5.0,
                  "F145: the camera solved from the patched rows (F144's reading) is over 5 cm off in the scaled role, with or without the move: it divides by the w row's scale, so the size factor read as a distance");
        }
        // 2b. the pistol's ADS role: its target is taken from the hip part's centre, so the seed-only map must take it from there too
        {
            const f90t::Record hip{f90t::kN89Hip, {0.10, -0.12, 0.36}, 0.07207, {2.0, 1.0}};
            const f90t::Record pads{f90t::kN89PistolAds, {0.07, -0.05, 0.42}, 0.1668, {2.0, 1.0}};
            f90::Keys hipKeys, seed, total;
            hipKeys.forwardCm = 13.0;
            hipKeys.leftCm = 3.5;
            hipKeys.upCm = -3.0;
            hipKeys.towardCm = 3.0;
            seed.forwardCm = 16.0;
            seed.leftCm = 5.0;
            seed.upCm = -3.0;
            seed.towardCm = 3.0;
            total = seed;
            total.forwardCm += 6.0;
            total.upCm += 2.0;
            bool ok = true;
            double worst = 0.0, fwd = 0.0, up = 0.0;
            bool crefUsed = false;
            for (int e = 0; e < 2; ++e) {
                f90::Latch latch;
                f90::In h = f90t::inOf(g, e, 100, hip, f90::kItemPistol, 7, hipKeys);
                f90::In a = f90t::inOf(g, e, 100, pads, f90::kItemPistol, 7, hipKeys);
                h.roleKeys = a.roleKeys = true;
                h.adsKeys = a.adsKeys = total;
                f90::decide(h, f90t::uOf(g, e), latch, rot, frameFn);
                const f90::Out o = f90::decide(a, f90t::uOf(g, e), latch, rot, frameFn);
                ok = ok && o.b == f90::kBMoved && o.map.scale && o.map.move && o.haveWf;
                crefUsed = o.haveCref;
                f90::Map seedMap;
                ok = ok && f145::seedOnlyMap(o, seed, a.eyeOk ? a.eye : nullptr, &seedMap);
                double drawn[3], drawnSeed[3], expTrimV[3];
                ok = ok && drawnOf(e, o.map, pads, drawn) && drawnOf(e, seedMap, pads, drawnSeed);
                if (!ok) break;
                f145::trimMove(o.map, seedMap, expTrimV);
                const f144::Move vsSeed = f144::moveOf(drawn, drawnSeed, expTrimV, o.wfUsed.b, o.wfUsed.l, o.wfUsed.u);
                worst = std::max(worst, vsSeed.diffMm);
                fwd = vsSeed.cm[0];
                up = vsSeed.cm[2];
            }
            std::printf("native_frame_test: F145 pistol ADS rig: vs the seed alone forward %.3f up %.3f cm (trims ask 6.000 / 2.000; worst %.4f mm)\n", fwd, up, worst);
            // the toward-eye move follows the line of sight from the moved point, so a trim in the weapon's axes shifts it a little: the move is the map's difference to the millimetre,
            // and within a few mm of the trims asked
            check(ok && crefUsed && worst < 1.0 && closeTo(fwd, 6.0, 0.15) && closeTo(up, 2.0, 0.3),
                  "F145: in the pistol's ADS role (a target taken from the hip part's centre) the seed-only map takes its target from there too: the drawn move is the map's difference within 1 mm, about the trims' 6 cm forward and 2 cm up");
        }
        // 2c. the hip role of a rifle-class weapon: a move only; seed 0 is the game's own draw, so both references agree
        {
            const f90t::Record hip{f90t::kN89Hip, {0.10, -0.12, 0.36}, 0.07207, {2.0, 1.0}};
            f90::Keys seed, total;
            total.forwardCm = 2.0;
            total.leftCm = -1.0;
            f90::Latch latch;
            f90::In in = f90t::inOf(g, 0, 100, hip, f90::kItemPrimary, 7, f90::Keys{});
            in.roleKeys = true;
            in.rifleHipKeys = total;
            const f90::Out o = f90::decide(in, f90t::uOf(g, 0), latch, rot, frameFn);
            f90::Map seedMap;
            const bool ok = f145::seedOnlyMap(o, seed, in.eyeOk ? in.eye : nullptr, &seedMap);
            double drawn[3], drawnSeed[3], expTrimV[3], game[3];
            const bool dr = drawnOf(0, o.map, hip, drawn) && drawnOf(0, seedMap, hip, drawnSeed);
            for (int i = 0; i < 3; ++i) game[i] = o.C[i] - in.ow[i];
            f145::trimMove(o.map, seedMap, expTrimV);
            const f144::Move vsSeed = f144::moveOf(drawn, drawnSeed, expTrimV, o.wfUsed.b, o.wfUsed.l, o.wfUsed.u);
            const f144::Move vsGame = f144::moveOf(drawn, game, o.map.t, o.wfUsed.b, o.wfUsed.l, o.wfUsed.u);
            check(ok && dr && !seedMap.move && !seedMap.scale && vsSeed.diffMm < 1.0 && vsGame.diffMm < 1.0 && closeTo(vsSeed.cm[0], 2.0, 0.02) && closeTo(vsSeed.cm[1], -1.0, 0.02) &&
                      closeTo(vsSeed.absCm, vsGame.absCm, 1e-3),
                  "F145: at the hip with seed 0 the seed-only frame is the game's own draw: both references give the same 2 cm forward, 1 cm right");
        }
        // 2d. the seed-only map: what it needs
        {
            f90::Out o;
            o.haveC = true;
            o.C[0] = 1.0;
            o.map.scale = true;
            o.map.k = 1.7778;
            o.map.move = true;
            o.map.t[0] = 0.5;
            f90::Map m;
            f90::Keys seed;
            check(f145::seedOnlyMap(o, seed, nullptr, &m) && m.scale && !m.move && m.t[0] == 0.0 && m.k == 1.7778, "F145: a seed of 0 is the scale alone (no move)");
            seed.forwardCm = 1.0;
            check(!f145::seedOnlyMap(o, seed, nullptr, &m), "F145: a seed that needs a weapon frame the decision did not have: no seed-only map (not measured)");
            seed = f90::Keys{};
            seed.towardCm = 2.0;
            check(!f145::seedOnlyMap(o, seed, nullptr, &m), "F145: ... or an eye it did not have");
            check(!f145::seedOnlyMap(o, seed, nullptr, nullptr), "F145: no output, no map");
            f90::Map total, none;
            total.move = true;
            total.t[0] = 0.3;
            total.t[1] = -0.1;
            none.move = true;
            none.t[0] = 0.25;
            double d[3];
            f145::trimMove(total, none, d);
            check(closeTo(d[0], 0.05, 1e-12) && closeTo(d[1], -0.1, 1e-12) && closeTo(d[2], 0.0, 1e-12), "F145: the move the trims ask is the total's move less the seed's");
            f90::Map nomove;
            f145::trimMove(nomove, nomove, d);
            check(d[0] == 0.0 && d[1] == 0.0 && d[2] == 0.0, "F145: neither moving: the trims ask nothing");
        }
    }
}

}  // namespace f145test
