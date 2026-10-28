// F144 checks (native on-foot VR), run144 below. Header only, run by native_frame_test.
//
// F144 (native_on_foot_f144.h, native_on_foot_f90.h): (1) the settled sight line per (item, scope): the settle gate (the ADS fraction 1.0, 0.6 s after it first reached 1.0 in the
// press, E and S moved under 2 mm), the average, the cache across presses, an item or scope change, the blend from the table, the choice and its reasons, the entry path from the
// F89 trace's eye samples; (2) the coat's Gear_Back decision and the first-person census; (3) the readout placed per role: the table's seeds are today's values, the rifle
// class's hip readout is placed with all four keys 0 where the game draws it, the trims are added to the role drawn now and no other, the pistol's ADS role takes its own keys
// from the hip's content centre, and the proof of what reaches the draw.
#pragma once

#include "../../src/d3d11/avatar_hooks_core.h"
#include "../../src/d3d11/native_on_foot_f144.h"
#include "f90_checks.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace f144test {

namespace f144 = edvr::nofoot::f144;
namespace f143 = edvr::nofoot::f143;
namespace f140 = edvr::nofoot::f140;
namespace f142 = edvr::nofoot::f142;
namespace f90 = edvr::nofoot::f90;
namespace ls = edvr::loadout;

inline bool near144(double a, double b, double tol) { return std::fabs(a - b) <= tol; }
inline bool sameMap(const f90::Map& a, const f90::Map& b) {
    return a.scale == b.scale && a.move == b.move && a.k == b.k && std::memcmp(a.c, b.c, sizeof(a.c)) == 0 && std::memcmp(a.t, b.t, sizeof(a.t)) == 0;
}

template <class Check>
inline void run144(Check&& check) {
    // ---- 1. the settle gate ------------------------------------------------------------------------------------------------------------------------------------------
    {
        f144::SettleGate g;
        const double e0[3] = {0.001, -0.040, 0.150}, s0[3] = {0.001, -0.040, 0.350};
        double e1[3] = {0.0015, -0.040, 0.150};   // 0.5 mm from e0
        double e2[3] = {0.0045, -0.040, 0.150};   // 3 mm from e1
        check(!g.settled(true, 0.5f, 1000, e0, s0), "F144: the first sample of a press is never settled (nothing to compare it with)");
        check(!g.settled(true, 1.0f, 1100, e1, s0) && g.fullSeen && g.fullMs == 1100, "F144: the ADS fraction first at 1.0 starts the 0.6 s");
        check(!g.settled(true, 1.0f, 1699, e1, s0), "F144: 599 ms after the fraction first reached 1.0 is not yet");
        check(g.settled(true, 1.0f, 1700, e1, s0), "F144: 600 ms after, with E and S still (0 mm from the previous sample): settled");
        check(!g.settled(true, 1.0f, 1800, e2, s0), "F144: E moved 3 mm from the previous usable sample: not settled");
        check(g.settled(true, 1.0f, 1900, e2, s0), "F144: ... and the next sample, 0 mm on, is");
        check(!g.settled(true, 0.99f, 2000, e2, s0) && g.settled(true, 1.0f, 2100, e2, s0), "F144: a fraction under 1.0 is not settled; this press's first 1.0 stands");
        check(!g.settled(false, 1.0f, 2200, e2, s0), "F144: a fraction that was not read is not settled");
        double s2[3] = {0.001, -0.040, 0.3519};   // S 1.9 mm
        check(g.settled(true, 1.0f, 2300, e2, s0) && g.settled(true, 1.0f, 2400, e2, s2) && g.settled(true, 1.0f, 2500, e2, s0),
              "F144: S moving 1.9 mm (under 2 mm) keeps it settled, and moving back 1.9 mm too");
        double s3[3] = {0.001, -0.040, 0.3521};
        check(!g.settled(true, 1.0f, 2600, e2, s3), "F144: S moving 2.1 mm: not settled");
        g.reset();
        check(!g.settled(true, 1.0f, 3000, e0, s0) && g.fullSeen && g.havePrev, "F144: a new press starts again (reset)");
        check(!g.settled(true, 1.0f, 3300, e0, s0) && !g.settled(true, 1.0f, 3599, e0, s0) && g.settled(true, 1.0f, 3600, e0, s0), "F144: ... and waits its own 0.6 s");
    }
    // ---- the average ---------------------------------------------------------------------------------------------------------------------------------------------------
    {
        f144::Line l;
        const double e0[3] = {0.0, 0.0, 0.15}, s0[3] = {0.0, 0.0, 0.35}, e1[3] = {0.01, -0.02, 0.15}, s1[3] = {0.01, -0.02, 0.35};
        check(!l.flush(100) && !l.have, "F144: nothing noted, nothing to flush");
        l.note(e0, s0);
        check(l.flush(1000) && l.have && l.samples == 1 && near144(l.e[2], 0.15, 1e-12), "F144: the first settled sample is the line");
        l.note(e1, s1);
        check(l.flush(2000) && near144(l.e[0], 0.01 * (1.0 - std::exp(-1.0)), 1e-12) && near144(l.s[1], -0.02 * (1.0 - std::exp(-1.0)), 1e-12),
              "F144: 1 s later the line has moved 1 - 1/e of the way to the new sample (tau 1 s)");
        f144::Line m;
        const double ea[3] = {0.02, 0.0, 0.1}, eb[3] = {0.04, 0.0, 0.2};
        m.note(e0, s0);
        m.flush(0);
        m.note(ea, s0);
        m.note(eb, s0);
        m.flush(100000);
        check(near144(m.e[0], 0.03, 1e-9) && m.samples == 3, "F144: the settled samples of a tick are one step (their mean)");
    }
    // ---- the cache: across presses, per item, per scope ---------------------------------------------------------------------------------------------------------------
    {
        f144::Cache c;
        const char* laser = "$wpn_m_submachinegun_laser_fauto_name;";
        const char* pistol = "$wpn_s_pistol_plasma_charged_name;";
        check(c.find(laser, ls::kScopeNo) == nullptr && c.get(nullptr, 1, 0) == nullptr && c.get("", 1, 0) == nullptr, "F144: no line yet; no name, no entry");
        f144::Entry* a = c.get(laser, ls::kScopeNo, 100);
        const double e[3] = {0.001, -0.046, 0.149}, s[3] = {0.002, -0.046, 0.359};
        a->line.note(e, s);
        a->line.flush(200);
        f144::Entry* again = c.get(laser, ls::kScopeNo, 90000);   // the second press, a minute later
        check(again == a && again->line.have && again->lastUse == 90000 && near144(again->line.e[1], -0.046, 1e-12), "F144: a second press finds the line (a cache hit), from its first frame");
        check(c.find(laser, ls::kScopeYes) == nullptr && c.find(pistol, ls::kScopeNo) == nullptr, "F144: another scope or another item has no line");
        f144::Entry* b = c.get(laser, ls::kScopeYes, 91000);
        check(b != a && !b->line.have && c.find(laser, ls::kScopeYes) == b && c.find(laser, ls::kScopeNo)->line.have, "F144: the scoped laser SMG is its own entry; the unscoped keeps its line");
        // fill and replace the least recently used
        for (int i = 0; i < f144::Cache::kN; ++i) {
            char n[64];
            std::snprintf(n, sizeof(n), "$wpn_test_%d;", i);
            c.get(n, ls::kScopeNo, 100000 + i);
        }
        check(c.find(laser, ls::kScopeNo) == nullptr && c.find("$wpn_test_15;", ls::kScopeNo) != nullptr, "F144: past 16 entries the least recently used is replaced");
    }
    // ---- the choice ------------------------------------------------------------------------------------------------------------------------------------------------------
    {
        const double p14w[3] = {0, 0, 0}, up[3] = {0, 1, 0}, right[3] = {1, 0, 0}, fwd[3] = {0, 0, 1};
        f144::Entry ent;
        ent.used = true;
        const double e[3] = {-0.005, -0.0461, 0.1493}, s[3] = {-0.0046, -0.0463, 0.359};
        const auto run = [&](bool key, bool itemOk, bool weapon, const f144::Entry* en) {
            return f144::choose(key, itemOk, weapon, en, p14w, up, right, fwd, p14w, 0.21, 0.0, 0.0);
        };
        check(run(false, true, true, &ent).why == f143::kKeyOff, "F144: the key off: the table (key off)");
        check(run(true, false, true, &ent).why == f143::kNoItem, "F144: an item snapshot that was not read: item not read");
        check(run(true, true, false, &ent).why == f143::kOtherItem, "F144: a held item that is not a weapon takes no line");
        check(run(true, true, true, nullptr).why == f143::kNoLine && run(true, true, true, &ent).why == f143::kNoLine, "F144: no entry, or an entry with no settled line yet: no settled line yet");
        std::memcpy(ent.line.e, e, sizeof(e));
        std::memcpy(ent.line.s, s, sizeof(s));
        ent.line.have = true;
        const f143::Choice c = run(true, true, true, &ent);
        check(c.game && near144(c.gameUpCm, -4.62, 0.01) && near144(c.gameRightCm, -0.49, 0.01), "F144: a settled line gives the game's shift from the press's first frame (the flight's laser SMG: up -4.62, right -0.49)");
        const f143::Choice t = f144::choose(true, true, true, &ent, p14w, up, right, fwd, p14w, 0.21, 0.5, 0.0);
        check(t.game && near144(t.shift[1], c.shift[1] + 0.005, 1e-12), "F144: the live trims are added to it");
        const f143::Choice bound = f144::choose(true, true, true, &ent, p14w, up, right, fwd, p14w, 0.21, -4.0, 0.0);
        check(!bound.game && bound.why == f143::kBound, "F144: past the 8 cm bound: the table stands in");
    }
    // ---- the blend ---------------------------------------------------------------------------------------------------------------------------------------------------------
    {
        f144::Blend b;
        check(near144(b.step(true, 1000), 1.0, 1e-12) && near144(b.step(true, 1011), 1.0, 1e-12), "F144: a press whose first frame has a line uses it at once (no blend)");
        b.reset();
        check(near144(b.step(false, 1000), 0.0, 1e-12) && near144(b.step(false, 1011), 0.0, 1e-12), "F144: without a line the table alone");
        check(near144(b.step(true, 1022), 0.0, 1e-12), "F144: the frame the line first becomes usable mid-press starts the blend at the table's shift");
        check(near144(b.step(true, 1022 + 125), 0.5, 1e-9) && near144(b.step(true, 1022 + 200), 0.8, 1e-9), "F144: ... and takes 0.25 s to the line (half way at 125 ms)");
        check(near144(b.step(true, 1022 + 250), 1.0, 1e-12) && near144(b.step(true, 1022 + 261), 1.0, 1e-12), "F144: ... and then it is the line alone");
        check(near144(b.step(false, 2000), 0.0, 1e-12) && near144(b.step(true, 2011), 0.0, 1e-12) && near144(b.step(true, 2011 + 250), 1.0, 1e-12),
              "F144: a line lost and back blends in again");
        const double table[3] = {0.0, 0.01, 0.0}, line[3] = {0.0, -0.04, 0.02};
        double out[3];
        f144::blendShift(0.25, table, line, out);
        check(near144(out[1], 0.01 * 0.75 - 0.04 * 0.25, 1e-12) && near144(out[2], 0.005, 1e-12), "F144: the blended shift is the weighted mix of the two");
    }
    // ---- the entry path --------------------------------------------------------------------------------------------------------------------------------------------------
    {
        f144::EntryPath p;
        for (int i = 0; i < 360; ++i) {   // the kinetic rifle press at 18:44:49: right +8.1 to -1.3 by 0.68 s, near -1.3 to 1.0 s, -0.3 by 1.2 s; up 0 to -1.4 then -2.1
            const double t = i / 90.0;
            double r, u;
            if (t <= 0.68) { r = 8.1 + (-1.3 - 8.1) * (t / 0.68); u = -1.4 * (t / 0.68); }
            else if (t <= 1.0) { r = -1.3; u = -1.4; }
            else if (t <= 1.2) { r = -1.3 + 1.0 * ((t - 1.0) / 0.2); u = -1.4 - 0.7 * ((t - 1.0) / 0.2); }
            else { r = -0.3; u = -2.1; }
            p.add(t, r, u);
        }
        const f144::PathResult res = p.result();
        check(res.ok && near144(res.lateral.settled, -0.3, 1e-3) && near144(res.vertical.settled, -2.1, 1e-3), "F144: the settled eye is the mean of the last second of the press");
        check(near144(res.lateral.over, 1.0, 0.02) && near144(res.lateral.dev, -1.0, 0.02) && res.lateral.overT >= 0.67 && res.lateral.overT <= 1.0,
              "F144: the lateral excursion beyond the settled value is 1.0 cm left, reached by 0.68 s");
        check(res.vertical.over == 0.0 && res.vertical.dev == 0.0, "F144: an eye that never passes its settled value (up) has no excursion");
        f144::EntryPath shortP;
        for (int i = 0; i < 180; ++i) shortP.add(i / 90.0, 1.0, 1.0);   // 2 s only
        check(!shortP.result().ok && std::strlen(shortP.result().why) > 0, "F144: a press that ended before 2.5 s has no settled value");
        f144::EntryPath few;
        for (int i = 0; i < 10; ++i) few.add(i * 0.4, 1.0, 1.0);
        check(!few.result().ok, "F144: too few eye samples");
        f144::EntryPath still;
        for (int i = 0; i < 360; ++i) still.add(i / 90.0, i >= 45 && i < 90 ? 0.2 : 0.0, 0.0);
        check(still.result().ok && still.result().lateral.over > 0.19 && still.result().vertical.over == 0.0, "F144: an eye that starts at its settled value and strays reports its deviation");
        f144::EntryPath cap;
        for (int i = 0; i < f144::EntryPath::kMax + 5; ++i) cap.add(i * 0.001, 0.0, 0.0);
        check(cap.n == f144::EntryPath::kMax && cap.dropped == 5, "F144: the path holds at most kMax samples and counts the rest");
        cap.reset();
        check(cap.n == 0 && cap.dropped == 0, "F144: reset");
    }

    // ---- 2. the coat: Gear_Back only for an outer garment ---------------------------------------------------------------------------------------------------------
    {
        check(f144::gearBackDecision(false, true) == f144::kGearOuter && f144::gearBackPromoted(f144::gearBackDecision(false, true)), "F144: no EVASuit_Arms and an EVASuit_Gear_Arms: the coat, promoted");
        check(f144::gearBackDecision(true, false) == f144::kGearSuitArms && !f144::gearBackPromoted(f144::gearBackDecision(true, false)), "F144: EVASuit_Arms present (the other suit): kept shadows only");
        check(f144::gearBackDecision(true, true) == f144::kGearSuitArms && !f144::gearBackPromoted(f144::kGearSuitArms), "F144: both present: the suit's arms win, kept shadows only");
        check(f144::gearBackDecision(false, false) == f144::kGearNoArms && !f144::gearBackPromoted(f144::kGearNoArms) && !f144::gearBackPromoted(f144::kGearUnknown),
              "F144: neither: kept shadows only");
        check(std::strcmp(f144::gearBackText(f144::kGearOuter), "Gear_Back promoted: outer garment") == 0 &&
                  std::strstr(f144::gearBackText(f144::kGearSuitArms), "kept shadows only: suit arms present") != nullptr,
              "F144: the decision's text");
        const int suitArms = edvr::avh::partIndexByName("EVASuit_Arms"), gearArms = edvr::avh::partIndexByName("EVASuit_Gear_Arms"), gearBack = edvr::avh::partIndexByName("EVASuit_Gear_Back");
        check(suitArms >= 0 && gearArms >= 0 && gearBack >= 0 && gearBack == 48, "F144: the three parts are in the part table (Gear_Back 48)");
        // the promotion with Gear_Back held back
        int32_t cls[f142::kPartCount];
        for (uint32_t i = 0; i < f142::kPartCount; ++i) cls[i] = 1;
        cls[gearBack] = 2;
        cls[edvr::avh::partIndexByName("EVASuit_Gear_LowerArmR")] = 2;
        f142::OutfitSwap sw;
        f142::promoteClasses(sw, 49, &edvr::avh::isHeadPart, [&](uint32_t i) { return cls[i]; },
                                   [&](uint32_t i, int32_t d, int32_t e) { if (cls[i] != e) return false; cls[i] = d; return true; }, 1ull << gearBack);
        check(sw.n == 1 && cls[gearBack] == 2 && cls[edvr::avh::partIndexByName("EVASuit_Gear_LowerArmR")] == 1, "F144: a skipped Gear_Back stays the game's class 2; the other parts keep F142's rule");
        // the first-person census
        f144::FpCensus fc;
        fc.c.amc = 0x1000;
        fc.c.n = 2;
        fc.c.row[0].idx = 52; fc.c.row[0].inst = 1; fc.c.row[0].masks[0] = 0xF;
        fc.c.row[1].idx = 29; fc.c.row[1].inst = 1;
        fc.listMask = (1ull << 52) | (1ull << 29);
        fc.zeroedMask = f144::zeroedOf(fc.c, fc.listMask);
        check(fc.zeroedMask == (1ull << 52), "F144: of the list, the part with a mask bit set is the one the zero took away");
        f144::FpCensus fd = fc;
        fd.c.row[0].masks[0] = 0x7;
        check(f144::fpSig(fc) == f144::fpSig(fd), "F144: the first-person census's signature ignores the masks' bits");
        fd.zeroedMask = 0;
        check(f144::fpSig(fc) != f144::fpSig(fd), "F144: ... but not the set of parts the zero changed");
        fd = fc;
        fd.listMask &= ~(1ull << 29);
        check(f144::fpSig(fc) != f144::fpSig(fd), "F144: ... nor the list");
    }

    // ---- 3. the readout placed per role ---------------------------------------------------------------------------------------------------------------------------------
    {
        double none[4] = {};
        // F145: the seeds are the pilot's dialled totals for the scope-no rows of eight weapons; everything else is as F144 left it
        struct Seed { const char* item; int role; double v[4]; };
        const Seed baked[] = {
            {"$wpn_m_assaultrifle_kinetic_fauto_name;", f144::kHip, {5.0, 4.5, 0.0, 0.0}},   {"$wpn_m_assaultrifle_kinetic_fauto_name;", f144::kAds, {3.5, 5.5, 0.0, 0.0}},
            {"$wpn_m_assaultrifle_plasma_fauto_name;", f144::kHip, {7.0, 2.5, 0.0, 0.0}},    {"$wpn_m_assaultrifle_plasma_fauto_name;", f144::kAds, {7.0, 6.0, 0.0, 0.0}},
            {"$wpn_m_launcher_rocket_sauto_name;", f144::kHip, {5.5, 0.0, 0.0, 0.0}},        {"$wpn_m_launcher_rocket_sauto_name;", f144::kAds, {5.5, 3.5, 0.0, 0.0}},
            {"$wpn_m_shotgun_plasma_doublebarrel_name;", f144::kHip, {10.0, -0.5, 1.0, 0.0}}, {"$wpn_m_shotgun_plasma_doublebarrel_name;", f144::kAds, {10.0, 3.0, 1.0, 0.0}},
            {"$wpn_m_submachinegun_kinetic_fauto_name;", f144::kHip, {6.5, 0.0, 0.0, 0.0}},  {"$wpn_m_submachinegun_kinetic_fauto_name;", f144::kAds, {6.5, 3.5, 0.0, 0.0}},
            {"$wpn_m_submachinegun_laser_fauto_name;", f144::kHip, {7.0, 1.5, 0.5, 0.0}},    {"$wpn_m_submachinegun_laser_fauto_name;", f144::kAds, {10.0, 4.5, 1.0, 0.0}},
            {"$wpn_s_pistol_kinetic_sauto_name;", f144::kHip, {13.0, 3.5, -3.0, 3.0}},       {"$wpn_s_pistol_kinetic_sauto_name;", f144::kAds, {16.0, 5.0, -3.0, 3.0}},
            {"$wpn_s_pistol_laser_sauto_name;", f144::kHip, {13.0, 3.5, -3.0, 3.0}},         {"$wpn_s_pistol_laser_sauto_name;", f144::kAds, {18.0, 6.5, -3.0, 3.0}},
        };
        bool seeds = f144::kTableCount == 44;
        for (const Seed& sd : baked) {
            const f144::Row* r = f144::find(sd.item, ls::kScopeNo, sd.role);
            seeds = seeds && r;
            for (int i = 0; seeds && i < 4; ++i) seeds = seeds && r->v[i] == sd.v[i];
        }
        check(seeds, "F145: the scope-no rows of the eight weapons the pilot dialled hold his totals, role by role (the kinetic pistol's hip and the laser pistol's hip unchanged)");
        // not baked: the scope-yes rows (F144's seeds: the F140 table for ADS and the pistols, 0 for a rifle's hip), the plasma pistol and the plasma sniper, whatever the scope
        bool kept = true;
        for (const f140::Entry& e : f140::kTable) {
            bool baked8 = false;
            for (const Seed& sd : baked) baked8 = baked8 || (std::strcmp(sd.item, e.item) == 0);
            if (baked8 && e.scope == ls::kScopeNo) continue;
            const f144::Row* ads = f144::find(e.item, e.scope, f144::kAds);
            const f144::Row* hip = f144::find(e.item, e.scope, f144::kHip);
            const bool pistol = e.v[0] == 13.0;
            kept = kept && ads && hip;
            for (int i = 0; kept && i < 4; ++i) {
                kept = kept && ads->v[i] == e.v[i];
                kept = kept && hip->v[i] == (pistol ? e.v[i] : 0.0);
            }
        }
        check(kept, "F145: the scope-yes rows, the plasma pistol and the plasma sniper keep F144's seeds (the F140 table for the ADS roles and the pistols, 0 for a rifle's hip role)");
        const char* kinRifle = "$wpn_m_assaultrifle_kinetic_fauto_name;";
        const char* plasmaPistol = "$wpn_s_pistol_plasma_charged_name;";
        const double trim[4] = {1.0, 2.0, -1.5, 0.5};
        // trims added to the role drawn now only
        const f144::RolePlacement hipDrawn = f144::place(true, 7, 7, kinRifle, ls::kScopeNo, f140::kClassRifle, f144::kHip, true, trim);
        const f144::RolePlacement adsIdle = f144::place(true, 7, 7, kinRifle, ls::kScopeNo, f140::kClassRifle, f144::kAds, false, trim);
        check(hipDrawn.coherent && hipDrawn.weapon && hipDrawn.table && near144(hipDrawn.total[0], 6.0, 1e-12) && near144(hipDrawn.total[1], 6.5, 1e-12) && near144(hipDrawn.total[2], -1.5, 1e-12) &&
                  near144(hipDrawn.total[3], 0.5, 1e-12) && hipDrawn.seed[0] == 5.0 && hipDrawn.seed[1] == 4.5,
              "F144: at the hip the trims move the rifle's hip role: total = its seed (5, 4.5, 0, 0) + the trims");
        check(near144(adsIdle.total[1], 5.5, 1e-12) && adsIdle.trim[1] == 0.0 && adsIdle.total[0] == 3.5, "F144: ... and not its ADS role, which stays at its seed (3.5, 5.5, 0, 0)");
        const f144::RolePlacement adsDrawn = f144::place(true, 7, 7, kinRifle, ls::kScopeNo, f140::kClassRifle, f144::kAds, true, trim);
        const f144::RolePlacement hipIdle = f144::place(true, 7, 7, kinRifle, ls::kScopeNo, f140::kClassRifle, f144::kHip, false, trim);
        check(near144(adsDrawn.total[1], 7.5, 1e-12) && near144(adsDrawn.total[0], 4.5, 1e-12) && hipIdle.total[0] == 5.0 && hipIdle.total[1] == 4.5 && hipIdle.trim[0] == 0.0,
              "F144: in ADS the trims move the ADS role (3.5 + 1.0 forward, 5.5 + 2.0 left) and not the hip role");
        const f144::RolePlacement pHip = f144::place(true, 7, 7, plasmaPistol, ls::kScopeNo, f140::kClassPistol, f144::kHip, true, trim);
        const f144::RolePlacement pAds = f144::place(true, 7, 7, plasmaPistol, ls::kScopeNo, f140::kClassPistol, f144::kAds, false, trim);
        check(near144(pHip.total[0], 14.0, 1e-12) && near144(pHip.total[1], 5.5, 1e-12) && near144(pAds.total[0], 13.0, 1e-12) && near144(pAds.total[1], 3.5, 1e-12) && near144(pAds.total[2], -3.0, 1e-12),
              "F144: a pistol's hip and ADS roles are dialled apart (13 + 1 forward at the hip, 13 for the idle ADS role)");
        // never carried to another weapon or scope: each (item, scope, role) is its own row, the trim a live key of the moment
        const f144::RolePlacement other = f144::place(true, 7, 7, "$wpn_m_assaultrifle_laser_fauto_name;", ls::kScopeYes, f140::kClassRifle, f144::kHip, false, trim);
        check(other.table && other.total[0] == 0.0 && other.total[1] == 0.0 && other.entryScope == ls::kScopeYes, "F144: another weapon and scope: its own row, the trims at rest");
        const f144::RolePlacement sameWeaponScopeYes = f144::place(true, 7, 7, kinRifle, ls::kScopeYes, f140::kClassRifle, f144::kHip, false, trim);
        check(sameWeaponScopeYes.table && sameWeaponScopeYes.total[0] == 0.0 && sameWeaponScopeYes.total[1] == 0.0 && sameWeaponScopeYes.entryScope == ls::kScopeYes,
              "F145: the kinetic rifle with a scope is its own row: the baked no-scope totals are not carried to it");
        // not a weapon, not coherent, a miss, unknown scope, clamps
        const f144::RolePlacement tool = f144::place(true, 7, 7, "$humanoid_companalyser_name;", ls::kScopeNo, f140::kClassRifle, f144::kAds, true, trim);
        check(tool.coherent && !tool.weapon && !tool.table && tool.trim[0] == 0.0 && tool.total[1] == 3.5, "F144: a tool takes no trim and falls back to the class's values");
        const f144::RolePlacement stale = f144::place(true, 7, 8, kinRifle, ls::kScopeNo, f140::kClassRifle, f144::kHip, true, trim);
        check(!stale.coherent && !stale.table && stale.trim[0] == 0.0 && stale.total[1] == 0.0, "F144: an item whose snapshot is not the held one: no row, no trim, the class's values (the rifle's hip 0)");
        const f144::RolePlacement missPistol = f144::place(true, 7, 7, "$wpn_s_pistol_new_name;", ls::kScopeNo, f140::kClassPistol, f144::kAds, false, trim);
        const f144::RolePlacement missRifleHip = f144::place(true, 7, 7, "$wpn_m_new_name;", ls::kScopeNo, f140::kClassRifle, f144::kHip, false, trim);
        check(!missPistol.table && missPistol.total[0] == 13.0 && !missRifleHip.table && missRifleHip.total[0] == 0.0 && missRifleHip.total[1] == 0.0, "F144: a miss takes the class's values (the pistol's, or 0 for the rifle class's hip)");
        const f144::RolePlacement unk = f144::place(true, 7, 7, kinRifle, ls::kScopeUnknown, f140::kClassRifle, f144::kAds, false, none);
        check(unk.table && unk.scopeUnknown && unk.entryScope == ls::kScopeNo, "F144: an unknown scope state uses the no-scope row and says so");
        const double big[4] = {50.0, -50.0, 50.0, -50.0};
        const f144::RolePlacement clamped = f144::place(true, 7, 7, kinRifle, ls::kScopeNo, f140::kClassRifle, f144::kHip, true, big);
        check(near144(clamped.trim[0], 30.0, 1e-12) && near144(clamped.trim[1], -30.0, 1e-12) && near144(clamped.trim[3], -30.0, 1e-12) && near144(clamped.total[0], 30.0, 1e-12) &&
                  near144(clamped.total[1], -25.5, 1e-12) && near144(clamped.total[3], -30.0, 1e-12),
              "F145: the trims are clamped to -30..30 cm before they are added (5 + 50 reads 5 + 30, clamped again to 30; 4.5 - 50 reads 4.5 - 30)");
        check(f144::drawnRole(0.0) == f144::kHip && f144::drawnRole(0.49) == f144::kHip && f144::drawnRole(0.5) == f144::kAds && f144::drawnRole(1.0) == f144::kAds,
              "F144: the ADS role is the one drawn from an ADS weight of 0.5");
    }
    {   // F90's decision with role keys, on the F85 two-eye rig
        namespace f90t = f90test;
        const f90t::Rig g = f90t::makeRig();
        const auto rot = edvr::drawtag::rotateByPackedQuat;
        const f90t::Record hip{f90t::kN89Hip, {0.10, -0.12, 0.36}, 0.07207, {2.0, 1.0}};
        const f90t::Record pads{f90t::kN89PistolAds, {0.07, -0.05, 0.42}, 0.1668, {2.0, 1.0}};
        const f90t::Weapon wp = f90t::weaponOf(g, 7.0);
        const auto frameFn = [&] { return f90::weaponFrame(true, wp.attach, true, wp.froot); };
        const edvr::nofoot::f85::UFrame g0 = f90t::uOf(g, 0), g1 = f90t::uOf(g, 1);
        // the rifle class's hip readout: not placed before (matched only); with role keys all 0 placed where the game draws it
        {
            f90::Latch latch;
            f90::In in = f90t::inOf(g, 0, 100, hip, f90::kItemPrimary, 7, f90::Keys{});
            const f90::Out before = f90::decide(in, g0, latch, rot, frameFn);
            check(before.role == f90::kRoleHip && before.b == f90::kBNone && before.cls == f90::kDrawnNone && before.map.identity(), "F144: before, the rifle class's hip readout is matched only (no placement)");
            in.roleKeys = true;
            const f90::Out o = f90::decide(in, g0, latch, rot, frameFn);
            edvr::nofoot::f85::UFrame uf = g0;
            f90::applyMap(uf, o.map);
            check(o.b == f90::kBMoved && o.cls == f90::kDrawnPrimary && !o.map.move && o.map.identity() && std::memcmp(&uf, &g0, sizeof(uf)) == 0,
                  "F144: with role keys, the rifle class's hip readout with all four keys 0 is placed and the frame it is drawn from is the game's, bit for bit");
            char text[200];
            f90::roleText(o, text, sizeof(text));
            check(std::strstr(text, "rifle placement") != nullptr, "F144: the role line says a rifle placement for it");
            // keys: 2 cm forward, 1 cm left at the hip
            in.rifleHipKeys.forwardCm = 2.0;
            in.rifleHipKeys.leftCm = 1.0;
            f90::Latch l2;
            const f90::Out m = f90::decide(in, g0, l2, rot, frameFn);
            double expect[3];
            for (int i = 0; i < 3; ++i) expect[i] = 0.01 * (2.0 * m.wf.b[i] + 1.0 * m.wf.l[i]);
            check(m.b == f90::kBMoved && m.map.move && m.haveWf && std::fabs(m.map.t[0] - expect[0]) < 1e-12 && std::fabs(m.map.t[1] - expect[1]) < 1e-12 && std::fabs(m.map.t[2] - expect[2]) < 1e-12,
                  "F144: the hip trims move the rifle class's hip readout 2 cm along the barrel and 1 cm left (in the weapon's frame)");
            const f90::Out m1 = f90::decide(f90t::inOf(g, 1, 100, hip, f90::kItemPrimary, 7, f90::Keys{}), g1, l2, rot, frameFn);
            check(m1.b == f90::kBNone, "F144: (roleKeys off) the other eye's decision is the old one");
            f90::In in1 = f90t::inOf(g, 1, 100, hip, f90::kItemPrimary, 7, f90::Keys{});
            in1.roleKeys = true;
            in1.rifleHipKeys = in.rifleHipKeys;
            const f90::Out m2 = f90::decide(in1, g1, l2, rot, frameFn);
            check(m2.b == f90::kBMoved && !m2.latched && std::memcmp(m2.T, m.T, sizeof(m.T)) == 0, "F144: the other eye reuses the latched target");
            // Status.json names the pistol long after: refused
            f90::In stale = in;
            stale.item = f90::kItemPistol;
            stale.drawnClassAgeS = 10.0;
            f90::Latch l3;
            check(f90::decide(stale, g0, l3, rot, frameFn).b == f90::kBRefItem, "F144: the rifle class's hip readout is refused when Status.json names a pistol long after a switch");
            f90::In mixed = in;
            mixed.drawnClass = f90::kDrawnMixed;
            check(f90::decide(mixed, g0, l3, rot, frameFn).b == f90::kBRefClass, "F144: ... and in a switch");
        }
        // the pistol class: ADS role keys equal to the hip's are the hip's target itself (bit for bit); different keys take their own target from the hip's centre
        {
            f90::Keys keys;
            keys.forwardCm = 13.0; keys.leftCm = 3.5; keys.upCm = -3.0; keys.towardCm = 3.0;
            f90::Latch la, lb;
            f90::In h = f90t::inOf(g, 0, 100, hip, f90::kItemPistol, 7, keys);
            f90::In a = f90t::inOf(g, 0, 100, pads, f90::kItemPistol, 7, keys);
            f90::decide(h, g0, la, rot, frameFn);
            const f90::Out oldAds = f90::decide(a, g0, la, rot, frameFn);
            h.roleKeys = a.roleKeys = true;
            h.adsKeys = a.adsKeys = keys;
            f90::decide(h, g0, lb, rot, frameFn);
            const f90::Out newAds = f90::decide(a, g0, lb, rot, frameFn);
            check(std::memcmp(oldAds.T, newAds.T, sizeof(oldAds.T)) == 0 && sameMap(oldAds.map, newAds.map) && newAds.b == f90::kBMoved && newAds.cls == f90::kDrawnPistol,
                  "F144: the pistol's ADS role with the hip's keys is today's placement, bit for bit");
            f90::Keys ads = keys;
            ads.forwardCm += 2.0;
            ads.upCm += 1.0;
            f90::Latch lc;
            f90::In h2 = h, a2 = a;
            h2.adsKeys = a2.adsKeys = ads;
            const f90::Out hipOut = f90::decide(h2, g0, lc, rot, frameFn);
            const f90::Out adsOut = f90::decide(a2, g0, lc, rot, frameFn);
            double texp[3];
            f90::target(hipOut.C, hipOut.wf, ads, h2.eyeOk ? h2.eye : nullptr, texp);
            double dd = 0.0;
            for (int i = 0; i < 3; ++i) dd += (adsOut.T[i] - texp[i]) * (adsOut.T[i] - texp[i]);
            double dh = 0.0;
            for (int i = 0; i < 3; ++i) dh += (adsOut.T[i] - hipOut.T[i]) * (adsOut.T[i] - hipOut.T[i]);
            check(adsOut.b == f90::kBMoved && std::sqrt(dd) < 1e-12 && std::sqrt(dh) > 0.02 && std::sqrt(dh) < 0.04 && std::memcmp(hipOut.T, newAds.T, sizeof(hipOut.T)) == 0,
                  "F144: the pistol's ADS role with its own keys takes its target from the hip part's centre (2 cm forward and 1 cm up more), the hip's target unchanged");
            f90::In a3 = f90t::inOf(g, 1, 100, pads, f90::kItemPistol, 7, keys);
            a3.roleKeys = true;
            a3.adsKeys = ads;
            const f90::Out adsE1 = f90::decide(a3, g1, lc, rot, frameFn);
            check(std::memcmp(adsE1.T, adsOut.T, sizeof(adsOut.T)) == 0 && adsE1.b == f90::kBMoved, "F144: both eyes share the pistol ADS role's target of a frame");
            // the previous frame's target still serves the ADS part in the frame after
            f90::In a4 = f90t::inOf(g, 0, 101, pads, f90::kItemPistol, 7, keys);
            a4.roleKeys = true;
            a4.adsKeys = ads;
            const f90::Out prev = f90::decide(a4, g0, lc, rot, frameFn);
            check(prev.b == f90::kBMovedPrevious && std::memcmp(prev.T, adsOut.T, sizeof(adsOut.T)) == 0, "F144: ... and the next frame, before the hip part is decided, takes the previous frame's (counted)");
        }
        // the rifle ADS readout is as it was
        {
            const f90t::Record rads{f90t::kN89RifleAds, {0.07, -0.05, 0.42}, 0.0500976555, {3.0, 1.0}};
            f90::Keys rk;
            rk.leftCm = 3.5;
            f90::Latch l0, l1;
            f90::In in = f90t::inOf(g, 0, 100, rads, f90::kItemPrimary, 7, f90::Keys{});
            in.rifleKeys = rk;
            const f90::Out before = f90::decide(in, g0, l0, rot, frameFn);
            in.roleKeys = true;
            const f90::Out after = f90::decide(in, g0, l1, rot, frameFn);
            check(std::memcmp(before.T, after.T, sizeof(before.T)) == 0 && sameMap(before.map, after.map) && after.cls == f90::kDrawnPrimary && after.b == f90::kBMoved,
                  "F144: the rifle's ADS readout is placed as before");
        }
    }
    {   // the proof: the drawn move against the reference (F145: the same frame at the seed alone; F144: the unplaced draw)
        const double game[3] = {1.0, 2.0, 3.0};
        const double b[3] = {0, 0, 1}, l[3] = {1, 0, 0}, u[3] = {0, 1, 0};
        const double expected[3] = {0.01, 0.002, 0.03};   // 1 cm left-axis, 0.2 up, 3 forward
        const double drawn[3] = {1.01, 2.002, 3.03};
        const f144::Move m = f144::moveOf(drawn, game, expected, b, l, u);
        check(near144(m.cm[0], 3.0, 1e-9) && near144(m.cm[1], 1.0, 1e-9) && near144(m.cm[2], 0.2, 1e-9) && m.diffMm < 1e-6 && near144(m.absCm, m.expAbsCm, 1e-9),
              "F144: the drawn move along the weapon's barrel, left and up (3.0, 1.0, 0.2 cm) equals the placement's");
        const double drawn2[3] = {1.0, 2.0, 3.0};
        const f144::Move none = f144::moveOf(drawn2, game, expected, b, l, u);
        check(none.absCm == 0.0 && near144(none.diffMm, 1000.0 * std::sqrt(0.01 * 0.01 + 0.002 * 0.002 + 0.03 * 0.03), 1e-6), "F144: a draw that did not move reads a move of 0 against the total's");
        f144::RoleAcc a;
        check(f144::verdictOf(a) == f144::kVNotMeasured, "F144: nothing measured");
        a.placed = 10;
        a.measured = 10;
        check(f144::verdictOf(a) == f144::kVNotReaching && std::strstr(f144::verdictText(f144::verdictOf(a)), "NOT REACHING THE DRAW") != nullptr,
              "F144: measured but never bound at the issue: NOT REACHING THE DRAW");
        for (int i = 0; i < 10; ++i) a.fold(m, m);
        check(f144::verdictOf(a) == f144::kVReaches && a.bound == 10 && near144(a.moveSum[0], 30.0, 1e-9), "F144: bound, the drawn move equal to the total within 1 mm: reaches the draw");
        f144::RoleAcc z = a;
        z.diffMaxMm = 1.5;
        check(f144::verdictOf(z) == f144::kVDiffers, "F144: 1.5 mm off the total: the drawn move differs");
        f144::RoleAcc d;
        d.measured = 5;
        for (int i = 0; i < 5; ++i) d.fold(none, none);
        check(f144::verdictOf(d) == f144::kVNotReaching, "F144: bound, the total's move 3 cm, the drawn move 0: NOT REACHING THE DRAW");
        f144::RoleAcc zero;
        zero.measured = 5;
        f144::Move zm;
        for (int i = 0; i < 5; ++i) zero.fold(zm, zm);
        check(f144::verdictOf(zero) == f144::kVNoMoveAsked && std::strstr(f144::verdictText(f144::kVNoMoveAsked), "no trim asked") != nullptr,
              "F145: no trim asked and a drawn move of 0 against the seed: no trim asked, drawn at its seed");
        // the move against the game's draw is information: it may be tens of cm while the proof against the seed reads the trims
        f144::RoleAcc info;
        info.measured = 4;
        const double gameFar[3] = {1.0, 2.0, 3.0}, drawnFar[3] = {1.0 + 0.16, 2.0 - 0.09, 3.0 + 0.04};   // a seed-and-size artefact of 18.8 cm
        const double noExpect[3] = {};
        const f144::Move vsGame = f144::moveOf(drawnFar, gameFar, noExpect, b, l, u);
        for (int i = 0; i < 4; ++i) info.fold(m, vsGame);
        check(f144::verdictOf(info) == f144::kVReaches && near144(info.gameAbsSum / 4.0, vsGame.absCm, 1e-9) && info.gameAbsSum / 4.0 > 10.0 && info.gameDiffMaxMm > 100.0,
              "F145: the move against the game's draw (tens of cm here) is kept apart and does not decide the verdict against the seed");
    }
}

}  // namespace f144test
