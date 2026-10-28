// F149/F150/F152 checks (native on-foot VR), run149 below. Header only, run by native_frame_test.
//
// F149/F150/F152 (native_on_foot_f149.h, native_on_foot_core.h, native_on_foot_f144.h): the ADS eye depth is the weapon's depth plus the live trim (experimental, -30..+30 cm,
// 0 by default), clamped to 0..30 cm, then the clearance cap. The weapon's depth is one value per item whatever the scope state: the table's base (the game's unscoped ADS camera
// depth) plus the item's baked offset (0 except for the kinetic assault rifle, -19, and the laser SMG, -3). An item with no table row takes its settled line's forward when the
// copy is known unscoped, else the fallback base (21 cm). There is no cap at 21 any more. The settled line feeds the depth of no table item. The F144 cache is told by copy while
// the scope state is unknown (two copies of one item). The checks: the two tables; the rule on every row and for every trim; the source order; the clearance cap after the rule;
// the sight line's shift taken at the same depth; the lock's follower carrying a depth change; the copy rule.
#pragma once

#include "../../src/d3d11/journal_loadout.h"
#include "../../src/d3d11/native_on_foot_core.h"
#include "../../src/d3d11/native_on_foot_f143.h"
#include "../../src/d3d11/native_on_foot_f144.h"
#include "../../src/d3d11/native_on_foot_f149.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>

namespace f149test {

namespace nf = edvr::nofoot;
namespace f149 = edvr::nofoot::f149;
namespace f144 = edvr::nofoot::f144;
namespace f143 = edvr::nofoot::f143;
namespace ls = edvr::loadout;

constexpr const char* kLaserSmg = "$wpn_m_submachinegun_laser_fauto_name;";
constexpr const char* kKineticRifle = "$wpn_m_assaultrifle_kinetic_fauto_name;";
constexpr const char* kKineticSmg = "$wpn_m_submachinegun_kinetic_fauto_name;";
constexpr const char* kRocket = "$wpn_m_launcher_rocket_sauto_name;";
constexpr const char* kPlasmaRifle = "$wpn_m_assaultrifle_plasma_fauto_name;";
constexpr const char* kShotgun = "$wpn_m_shotgun_plasma_doublebarrel_name;";
constexpr const char* kSniper = "$wpn_m_sniper_plasma_charged_name;";
constexpr const char* kKineticPistol = "$wpn_s_pistol_kinetic_sauto_name;";
constexpr const char* kLaserPistol = "$wpn_s_pistol_laser_sauto_name;";
constexpr const char* kPlasmaPistol = "$wpn_s_pistol_plasma_charged_name;";
constexpr const char* kLaserRifle = "$wpn_m_assaultrifle_laser_fauto_name;";   // F79's rifle (the TK Aphelion): no row

inline bool near149(double a, double b, double tol = 1e-9) { return std::fabs(a - b) <= tol; }

// A settled entry for (name, scope) whose line has E's forward at eForwardCm from P14 (S 20 cm further on, level).
inline f144::Entry settledEntry(const char* name, int scope, double eForwardCm, bool have = true) {
    f144::Entry e;
    e.used = true;
    std::snprintf(e.name, sizeof(e.name), "%s", name);
    e.scope = scope;
    e.line.have = have;
    e.line.e[2] = 0.01 * eForwardCm;
    e.line.s[2] = 0.01 * (eForwardCm + 20.0);
    return e;
}

template <class Check>
inline void run149(Check&& check) {
    // ---- 1. the two tables ------------------------------------------------------------------------------------------------------------------------------------------
    {
        double base = 9.0, off = 9.0;
        check(f149::tableDepthCm(kLaserSmg, &base, &off) && base == 15.0 && off == -3.0, "F153: the laser SMG's base (the game's unscoped ADS camera) is 15 cm and its baked offset -3 (the pilot's 12 cm, held 38 s in flight edvr_gfx_20261028_090045)");
        check(f149::tableDepthCm(kKineticRifle, &base, &off) && base == 25.0 && off == -19.0, "F152: the kinetic assault rifle's base is 25 cm and its baked offset -19 (the pilot's 6 cm, held 55 s in flight edvr_gfx_20261028_073013 and settled on after a trim sweep in flight edvr_gfx_20261028_090045)");
        check(f149::tableDepthCm(kKineticSmg, &base, &off) && base == 20.0 && off == 0.0, "F150: the kinetic SMG's base is 20 cm, no offset");
        check(f149::tableDepthCm(kRocket, &base, &off) && base == 21.98 && off == 0.0, "F150: the rocket launcher's base is 21.98 cm, no offset");
        check(f149::tableDepthCm(kPlasmaRifle, &base, &off) && base == 12.99 && off == 0.0, "F150: the plasma assault rifle's base is 12.99 cm, no offset");
        check(f149::tableDepthCm(kShotgun, &base, &off) && base == 0.01 && off == 0.0, "F150: the plasma shotgun's base is 0.01 cm, no offset");
        check(f149::tableDepthCm(kSniper, &base, &off) && base == 16.0 && off == 0.0, "F150: the plasma sniper's base is its unscoped camera, 16 cm (the scoped 5 cm is not in the table), no offset");
        check(f149::tableDepthCm(kKineticPistol, &base, &off) && base == -2.5 && off == 0.0 && f149::tableDepthCm(kLaserPistol, &base, &off) && base == -2.47 && off == 0.0 &&
                  f149::tableDepthCm(kPlasmaPistol, &base, &off) && base == -7.51 && off == 0.0,
              "F150: the pistols' cameras are behind P14 (-2.5, -2.47 and -7.51 cm), no offset");
        check(!f149::tableDepthCm(kLaserRifle, &base, &off) && base == 0.0 && off == 0.0 && !f149::tableDepthCm("", &base, &off) && !f149::tableDepthCm(nullptr, &base, &off) &&
                  !f149::tableDepthCm("$wpn_m_submachinegun_laser_fauto_name", &base, &off) && !f149::tableDepthCm("$WPN_M_SUBMACHINEGUN_LASER_FAUTO_NAME;", &base, &off),
              "F150: the laser assault rifle (F79's, the TK Aphelion), the empty name, no name and a name not exact are misses, both figures 0");
        check(f149::tableDepthCm(kLaserSmg, nullptr, nullptr) && !f149::tableDepthCm(kLaserRifle, nullptr, nullptr), "F150: the table can be asked for the hit alone");
        // every row names an item the readout's table (F144) knows, is a weapon, and appears once; every offset names a row of the base table
        bool known = true, weapons = true, unique = true, finite = true, offsetsOk = true;
        for (int i = 0; i < f149::kAdsCameraDepthCount; ++i) {
            const f149::ItemCm& r = f149::kAdsCameraDepths[i];
            known = known && f144::find(r.item, f144::kHip) != nullptr;
            weapons = weapons && nf::f139::isWeapon(r.item);
            finite = finite && std::isfinite(r.cm) && r.cm > -50.0 && r.cm < 50.0;
            for (int j = i + 1; j < f149::kAdsCameraDepthCount; ++j) unique = unique && std::strcmp(r.item, f149::kAdsCameraDepths[j].item) != 0;
        }
        for (int i = 0; i < f149::kAdsBackOffCount; ++i) {
            const f149::ItemCm& o = f149::kAdsBackOffs[i];
            double b = 0.0;
            offsetsOk = offsetsOk && f149::rowCm(f149::kAdsCameraDepths, f149::kAdsCameraDepthCount, o.item, &b) && std::isfinite(o.cm) && o.cm <= 0.0 && o.cm > -30.0;
            for (int j = i + 1; j < f149::kAdsBackOffCount; ++j) offsetsOk = offsetsOk && std::strcmp(o.item, f149::kAdsBackOffs[j].item) != 0;
        }
        check(f149::kAdsCameraDepthCount == 10 && known && weapons && unique && finite,
              "F150: the ten base rows each name a weapon the readout table knows (a typo would be a miss), once, with a plausible depth");
        check(f149::kAdsBackOffCount == 2 && offsetsOk, "F150: two baked offsets, each on a base row, once, backwards (never above 0) and under 30 cm");
        check(f149::kTrimMinCm == -30.0 && f149::kTrimMaxCm == 30.0 && f149::kAdsFallbackBaseCm == 21.0 && f149::clampTrimCm(45.0) == 30.0 && f149::clampTrimCm(-45.0) == -30.0 &&
                  f149::clampTrimCm(7.5) == 7.5 && f149::clampTrimCm(std::numeric_limits<double>::quiet_NaN()) == 0.0 && f149::clampTrimCm(std::numeric_limits<double>::infinity()) == 0.0,
              "F152: the trim is -30..30 cm; one that is not a number reads 0; the fallback base is the old setting's 21 cm");
    }
    // ---- 2. the rule --------------------------------------------------------------------------------------------------------------------------------------------
    {
        const f149::EyeDepth smg = f149::eyeDepth(0.0, true, kLaserSmg, ls::kScopeNo, nullptr);
        check(smg.source == f149::kSrcTable && smg.baseCm == 15.0 && smg.offsetCm == -3.0 && smg.trimCm == 0.0 && smg.depthCm == 12.0 && smg.askedCm == 12.0,
              "F153: the laser SMG with no trim: base 15 + offset -3 + trim 0 = 12 cm");
        const f149::EyeDepth ar = f149::eyeDepth(0.0, true, kKineticRifle, ls::kScopeNo, nullptr);
        check(ar.source == f149::kSrcTable && ar.baseCm == 25.0 && ar.offsetCm == -19.0 && ar.depthCm == 6.0 && ar.askedCm == 6.0,
              "F152: the kinetic assault rifle with no trim: base 25 + offset -19 = 6 cm (the pilot's)");
        const f149::EyeDepth rocket = f149::eyeDepth(0.0, true, kRocket, ls::kScopeNo, nullptr);
        check(rocket.askedCm == 21.98 && rocket.depthCm == 21.98 && rocket.offsetCm == 0.0, "F152: the rocket launcher goes to its 21.98 cm: there is no cap at 21 any more");
        const f149::EyeDepth ksmg = f149::eyeDepth(0.0, true, kKineticSmg, ls::kScopeNo, nullptr);
        const f149::EyeDepth plasmaRifle = f149::eyeDepth(0.0, true, kPlasmaRifle, ls::kScopeNo, nullptr);
        const f149::EyeDepth shotgun = f149::eyeDepth(0.0, true, kShotgun, ls::kScopeNo, nullptr);
        check(ksmg.askedCm == 20.0 && plasmaRifle.askedCm == 12.99 && shotgun.askedCm == 0.01,
              "F150: the kinetic SMG goes to 20 cm, the plasma assault rifle to 12.99 and the plasma shotgun to 0.01");
        // the trim: added to any weapon, moves it both ways, the result clamped to 0..30
        const f149::EyeDepth smgForward = f149::eyeDepth(5.0, true, kLaserSmg, ls::kScopeNo, nullptr);
        const f149::EyeDepth smgForward20 = f149::eyeDepth(20.0, true, kLaserSmg, ls::kScopeNo, nullptr);
        check(smgForward.askedCm == 17.0 && smgForward.trimCm == 5.0 && smgForward.depthCm == 17.0 && smgForward20.askedCm == 30.0 && smgForward20.depthCm == 32.0,
              "F153: a trim of +5 on the laser SMG gives 17 cm (past the old cap's 10), +20 gives 30 (its sum 32)");
        const f149::EyeDepth arBack = f149::eyeDepth(-10.0, true, kKineticRifle, ls::kScopeNo, nullptr);
        const f149::EyeDepth smgBack = f149::eyeDepth(-30.0, true, kLaserSmg, ls::kScopeNo, nullptr);
        check(arBack.depthCm == -4.0 && arBack.askedCm == 0.0 && smgBack.depthCm == -18.0 && smgBack.askedCm == 0.0 && smgBack.trimCm == -30.0,
              "F153: a trim that takes the depth below 0 clamps at 0 (the kinetic AR -10: -4 -> 0; the laser SMG -30: -18 -> 0), the unclamped sum kept in depthCm");
        const f149::EyeDepth rocketPlus = f149::eyeDepth(30.0, true, kRocket, ls::kScopeNo, nullptr);
        const f149::EyeDepth rocketSmall = f149::eyeDepth(2.0, true, kRocket, ls::kScopeNo, nullptr);
        check(rocketPlus.askedCm == 30.0 && near149(rocketPlus.depthCm, 51.98) && near149(rocketSmall.askedCm, 23.98),
              "F152: the rocket launcher at +2 goes to 23.98 cm (no cap at 21), at +30 clamps at 30 (its sum 51.98)");
        const f149::EyeDepth big = f149::eyeDepth(45.0, true, kKineticRifle, ls::kScopeNo, nullptr);
        const f149::EyeDepth neg = f149::eyeDepth(-45.0, true, kLaserSmg, ls::kScopeNo, nullptr);
        const f149::EyeDepth nan = f149::eyeDepth(std::numeric_limits<double>::quiet_NaN(), true, kLaserSmg, ls::kScopeNo, nullptr);
        check(big.trimCm == 30.0 && big.depthCm == 36.0 && big.askedCm == 30.0 && neg.trimCm == -30.0 && neg.askedCm == 0.0 && nan.trimCm == 0.0 && nan.askedCm == 12.0,
              "F152: a trim of 45 reads 30 (the kinetic AR: 36, asked 30), -45 reads -30, and one that is not a number reads 0 before the rule is applied");
        // the plasma sniper: 16 cm whatever the scope state, and never the settled line's
        const f149::EyeDepth sniperYes = f149::eyeDepth(0.0, true, kSniper, ls::kScopeYes, nullptr);
        const f149::EyeDepth sniperNo = f149::eyeDepth(0.0, true, kSniper, ls::kScopeNo, nullptr);
        const f149::EyeDepth sniperUnknown = f149::eyeDepth(0.0, true, kSniper, ls::kScopeUnknown, nullptr);
        check(sniperYes.askedCm == 16.0 && sniperNo.askedCm == 16.0 && sniperUnknown.askedCm == 16.0 && sniperYes.source == f149::kSrcTable && sniperNo.source == f149::kSrcTable &&
                  sniperUnknown.source == f149::kSrcTable && sniperYes.baseCm == 16.0 && sniperYes.offsetCm == 0.0,
              "F150: the plasma sniper sits at 16 cm with a scope, without one and with the scope state unknown (the unscoped camera; the scoped copy's 5 cm is not used)");
        const f149::EyeDepth pk = f149::eyeDepth(0.0, true, kKineticPistol, ls::kScopeNo, nullptr), pl = f149::eyeDepth(0.0, true, kLaserPistol, ls::kScopeNo, nullptr),
                             pp = f149::eyeDepth(0.0, true, kPlasmaPistol, ls::kScopeNo, nullptr);
        check(pk.askedCm == 0.0 && pl.askedCm == 0.0 && pp.askedCm == 0.0 && pp.depthCm == -7.51 && pp.source == f149::kSrcTable,
              "F150: the pistols' cameras are behind P14: the depth asked is 0 after the clamp (the plasma pistol's sum is kept as read, -7.51 cm)");
        const f149::EyeDepth ppPlus = f149::eyeDepth(10.0, true, kPlasmaPistol, ls::kScopeNo, nullptr);
        check(near149(ppPlus.depthCm, 2.49) && near149(ppPlus.askedCm, 2.49), "F152: a trim of +10 on the plasma pistol brings its eye 2.49 cm forward of the aiming point");
        // an item with no offset sits at its base with no trim
        bool atBase = true;
        for (const f149::ItemCm& r : f149::kAdsCameraDepths) {
            double off = 0.0;
            f149::rowCm(f149::kAdsBackOffs, f149::kAdsBackOffCount, r.item, &off);
            const f149::EyeDepth d = f149::eyeDepth(0.0, true, r.item, ls::kScopeNo, nullptr);
            if (off == 0.0) atBase = atBase && d.offsetCm == 0.0 && d.depthCm == r.cm && d.baseCm == r.cm;
            else atBase = atBase && d.offsetCm == off && d.depthCm == r.cm + off;
        }
        check(atBase, "F152: with no trim an item with no offset has its depth at its base, to the digit; one with an offset has base + offset");
        // an item with no row: the settled line when the copy is known unscoped, else the fallback base
        f144::Entry aphelion = settledEntry(kLaserRifle, ls::kScopeNo, 10.0);
        const f149::EyeDepth unlistedNo = f149::eyeDepth(0.0, true, kLaserRifle, ls::kScopeNo, &aphelion);
        const f149::EyeDepth unlistedNoTrim = f149::eyeDepth(5.0, true, kLaserRifle, ls::kScopeNo, &aphelion);
        check(unlistedNo.source == f149::kSrcSettled && near149(unlistedNo.baseCm, 10.0) && unlistedNo.offsetCm == 0.0 && near149(unlistedNo.depthCm, 10.0) && near149(unlistedNo.askedCm, 10.0) &&
                  near149(unlistedNoTrim.askedCm, 15.0),
              "F152: an item with no row (the TK Aphelion), known unscoped, takes its settled line's forward (10 cm) as its base, no offset; a trim of +5 makes it 15");
        f144::Entry aphelionYes = settledEntry(kLaserRifle, ls::kScopeYes, 10.0), aphelionUnknown = settledEntry(kLaserRifle, ls::kScopeUnknown, 10.0);
        const f149::EyeDepth unlistedYes = f149::eyeDepth(0.0, true, kLaserRifle, ls::kScopeYes, &aphelionYes);
        const f149::EyeDepth unlistedUnknown = f149::eyeDepth(0.0, true, kLaserRifle, ls::kScopeUnknown, &aphelionUnknown);
        const f149::EyeDepth unlistedNoLine = f149::eyeDepth(0.0, true, kLaserRifle, ls::kScopeNo, nullptr);
        const f149::EyeDepth unlistedName = f149::eyeDepth(0.0, true, "$wpn_m_unlisted_name;", ls::kScopeNo, nullptr);
        const f149::EyeDepth unlistedTrimmed = f149::eyeDepth(-3.0, true, "$wpn_m_unlisted_name;", ls::kScopeNo, nullptr);
        check(unlistedYes.source == f149::kSrcNone && unlistedYes.askedCm == 21.0 && unlistedYes.baseCm == 21.0 && unlistedYes.depthCm == 21.0 && unlistedUnknown.source == f149::kSrcNone &&
                  unlistedUnknown.askedCm == 21.0 && unlistedNoLine.source == f149::kSrcNone && unlistedNoLine.askedCm == 21.0 && unlistedName.source == f149::kSrcNone &&
                  unlistedName.askedCm == 21.0 && unlistedTrimmed.askedCm == 18.0,
              "F152: an item with no row takes the fallback base (21 cm, source none) when the copy has a scope or its scope is unknown even with a line, and when it has no line; a trim of -3 makes it 18");
        const f149::EyeDepth notCoherent = f149::eyeDepth(0.0, false, kLaserSmg, ls::kScopeNo, nullptr);
        const f149::EyeDepth notCoherentLine = f149::eyeDepth(0.0, false, kLaserRifle, ls::kScopeNo, &aphelion);
        check(notCoherent.source == f149::kSrcNone && notCoherent.askedCm == 21.0 && notCoherentLine.source == f149::kSrcNone && notCoherentLine.askedCm == 21.0,
              "F150: an item snapshot that is not coherent is no item: the fallback base");
        // exactly the clamp of base + offset + trim, on every row, every scope state and a range of trims from -30 to 30
        bool exact = true, inRange = true;
        const double trims[] = {-30.0, -20.0, -10.0, -5.0, -0.5, 0.0, 0.5, 1.0, 5.0, 10.0, 15.5, 21.0, 25.0, 30.0};
        for (const double t : trims) {
            for (const f149::ItemCm& r : f149::kAdsCameraDepths) {
                double off = 0.0;
                f149::rowCm(f149::kAdsBackOffs, f149::kAdsBackOffCount, r.item, &off);
                for (const int scope : {ls::kScopeUnknown, ls::kScopeNo, ls::kScopeYes}) {
                    const f149::EyeDepth d = f149::eyeDepth(t, true, r.item, scope, nullptr);
                    inRange = inRange && d.askedCm >= 0.0 && d.askedCm <= nf::kAdsEyeDistMaxCm;
                    exact = exact && d.askedCm == nf::adsEyeDistanceClampCm(r.cm + off + t) && d.trimCm == t && d.depthCm == r.cm + off + t;
                }
            }
        }
        check(inRange && exact, "F152: for every row, every scope state and every trim from -30 to 30 cm the depth asked is the clamp (0..30) of base + offset + trim, exactly");
    }
    // ---- 3. the source order --------------------------------------------------------------------------------------------------------------------------------------
    {
        // the settled line no longer feeds the depth of an item that has a row: not below the table, not above it, whatever the scope state
        f144::Entry lower = settledEntry(kLaserSmg, ls::kScopeNo, 12.0);
        const f149::EyeDepth a = f149::eyeDepth(0.0, true, kLaserSmg, ls::kScopeNo, &lower);
        check(a.source == f149::kSrcTable && a.askedCm == 12.0 && a.baseCm == 15.0, "F150/F153: a settled line (E 12 cm forward) does not move the laser SMG's depth: the table's 12");
        f144::Entry deeper = settledEntry(kKineticRifle, ls::kScopeNo, 8.0);
        const f149::EyeDepth b = f149::eyeDepth(0.0, true, kKineticRifle, ls::kScopeNo, &deeper);
        check(b.source == f149::kSrcTable && b.askedCm == 6.0, "F150: ... nor the kinetic assault rifle's (a line at 8 cm leaves its 6)");
        f144::Entry scoped = settledEntry(kSniper, ls::kScopeYes, 5.0);
        const f149::EyeDepth c = f149::eyeDepth(0.0, true, kSniper, ls::kScopeYes, &scoped);
        f144::Entry unscoped = settledEntry(kSniper, ls::kScopeNo, 16.0);
        const f149::EyeDepth d = f149::eyeDepth(0.0, true, kSniper, ls::kScopeNo, &unscoped);
        check(c.source == f149::kSrcTable && c.askedCm == 16.0 && d.source == f149::kSrcTable && d.askedCm == 16.0,
              "F150: the scoped plasma sniper's settled line (5 cm) does not move it off the unscoped camera's 16");
        // an item with no row: a line that has not settled, a non-finite one
        f144::Entry notYet = settledEntry(kLaserRifle, ls::kScopeNo, 10.0, false);
        const f149::EyeDepth e = f149::eyeDepth(0.0, true, kLaserRifle, ls::kScopeNo, &notYet);
        f144::Entry nanLine = settledEntry(kLaserRifle, ls::kScopeNo, 10.0);
        nanLine.line.e[2] = std::numeric_limits<double>::quiet_NaN();
        const f149::EyeDepth f = f149::eyeDepth(0.0, true, kLaserRifle, ls::kScopeNo, &nanLine);
        check(e.source == f149::kSrcNone && e.askedCm == 21.0 && f.source == f149::kSrcNone && f.askedCm == 21.0,
              "F150: an unlisted item's entry whose line has not settled, or whose E is not a number, is no line: the fallback base");
        f144::Entry beyond = settledEntry(kLaserRifle, ls::kScopeNo, 35.0);
        const f149::EyeDepth g = f149::eyeDepth(0.0, true, kLaserRifle, ls::kScopeNo, &beyond);
        f144::Entry behind = settledEntry(kLaserRifle, ls::kScopeNo, -4.0);
        const f149::EyeDepth h = f149::eyeDepth(0.0, true, kLaserRifle, ls::kScopeNo, &behind);
        check(g.source == f149::kSrcSettled && near149(g.depthCm, 35.0) && g.askedCm == 30.0 && h.source == f149::kSrcSettled && near149(h.depthCm, -4.0) && h.askedCm == 0.0,
              "F152: an unlisted item's line further forward than 30 cm (35) is clamped to 30, and one behind P14 (-4 cm) reads 0 after the clamp");
        // the cache finds the line by exact name, scope and copy: a line settled with the scope answers the scoped copy only
        f144::Cache cache;
        f144::Entry* sc = cache.get(kLaserRifle, ls::kScopeYes, 100);
        sc->line.have = true;
        sc->line.e[2] = 0.04;
        const f149::EyeDepth withScope = f149::eyeDepth(0.0, true, kLaserRifle, ls::kScopeYes, cache.find(kLaserRifle, ls::kScopeYes));
        const f149::EyeDepth noScope = f149::eyeDepth(0.0, true, kLaserRifle, ls::kScopeNo, cache.find(kLaserRifle, ls::kScopeNo));
        check(withScope.source == f149::kSrcNone && withScope.askedCm == 21.0 && noScope.source == f149::kSrcNone && noScope.askedCm == 21.0,
              "F150: an unlisted item's line settled with a scope never gives its depth (scope yes is the fallback base), and the unscoped copy has no line of its own yet");
        check(std::strcmp(f149::sourceText(f149::kSrcSettled), "the settled line") == 0 && std::strcmp(f149::sourceText(f149::kSrcTable), "the table") == 0 &&
                  std::strcmp(f149::sourceText(f149::kSrcNone), "the fallback base (no measurement)") == 0 && std::strcmp(f149::sourceText(99), "the fallback base (no measurement)") == 0,
              "F152: the log names the source of the base: the settled line, the table, the fallback base");
    }
    // ---- 4. the clearance cap after the rule, the sight line at the same depth, the follower ------------------------------------------------------------------------------------------
    {
        const double p14[3] = {1.0, 1.5, -2.0}, f[3] = {0.0, 0.0, 1.0};
        const f149::EyeDepth smg = f149::eyeDepth(0.0, true, kLaserSmg, ls::kScopeNo, nullptr);
        const f149::EyeDepth smgForward = f149::eyeDepth(9.0, true, kLaserSmg, ls::kScopeNo, nullptr);
        double cutPoint[1][3] = {{p14[0], p14[1], p14[2] + 0.14}}, farPoint[1][3] = {{p14[0], p14[1], p14[2] + 0.30}};
        const nf::AdsEyeCap cut = nf::adsEyeDistanceCap(smg.askedCm / 100.0, p14, f, cutPoint, 1), kept = nf::adsEyeDistanceCap(smg.askedCm / 100.0, p14, f, farPoint, 1);
        const nf::AdsEyeCap cutForward = nf::adsEyeDistanceCap(smgForward.askedCm / 100.0, p14, f, cutPoint, 1);
        check(cut.capped && near149(cut.kM, 0.08) && !kept.capped && near149(kept.kM, 0.12) && cutForward.capped && near149(smgForward.askedCm, 21.0) && near149(cutForward.kM, 0.08),
              "F153: the clearance cap takes the depth asked: a weapon point 14 cm ahead on the line cuts the laser SMG's 12 cm to 8, one 30 cm ahead leaves 12, and a trim that asks 21 is cut to 8 as well");
        // the sight line's shift is taken at the same k: a line that leaves the root's forward by 2 cm over 20 cm, E 15 cm forward of P14, is -0.3 cm up at the depth asked (12 cm) and 0.6 cm up at 21 cm
        f144::Entry ent = settledEntry(kLaserSmg, ls::kScopeNo, 15.0);
        ent.line.s[1] = 0.02;
        const double up[3] = {0, 1, 0}, right[3] = {1, 0, 0}, origin[3] = {p14[0], p14[1], p14[2]};
        const f143::Choice atAsked = f144::choose(true, true, &ent, p14, up, right, f, origin, smg.askedCm / 100.0);
        const f143::Choice atTrimmed = f144::choose(true, true, &ent, p14, up, right, f, origin, smgForward.askedCm / 100.0);
        check(atAsked.game && near149(atAsked.gameUpCm, -0.3, 1e-9) && atTrimmed.game && near149(atTrimmed.gameUpCm, 0.6, 1e-9) && near149(atAsked.q[2] - origin[2], 0.12, 1e-12) &&
                  near149(atTrimmed.q[2] - origin[2], 0.21, 1e-12),
              "F153: the line's shift is Q on the line at the depth asked: -0.3 cm up at 12 cm asked, 0.6 cm up at 21 (a trim of +9), the same k as the eye point");
        // the settled line of a table item still gives the shift at the depth the table asked
        const f149::EyeDepth withLine = f149::eyeDepth(0.0, true, kLaserSmg, ls::kScopeNo, &ent);
        check(withLine.source == f149::kSrcTable && withLine.askedCm == smg.askedCm, "F150: the same line leaves the depth where the table put it (the line feeds the shift only)");
        // the lock's follower carries the change of k: the target moves 6 cm back along the sight, no overshoot, 1 mm off in 0.3 s
        double pos[3] = {0.0, 0.0, -0.234}, vel[3] = {}, target[3] = {};
        const double goal = -0.294;
        double maxStep = 0.0, overshoot = 0.0;
        for (int i = 0; i < 27; ++i) {
            const double e[3] = {0.0, 0.0, goal - pos[2]};
            const double before = pos[2];
            nf::adsEyeStep(pos, vel, target, e, true, true, false, 1.0 / 90.0);
            maxStep = std::fmax(maxStep, std::fabs(pos[2] - before));
            overshoot = std::fmax(overshoot, goal - pos[2]);
        }
        check(maxStep <= nf::kAdsEyeCapPerFrameM && overshoot <= 1e-9 && std::fabs(pos[2] - goal) < 0.003 && std::fabs(target[2] - goal) < 1e-12,
              "F150: a 6 cm change of k (between weapons, or of the live trim) reaches the eye through the existing follower: no frame over the per-frame cap, no overshoot, within 3 mm in 0.3 s");
    }
    // ---- 5. the F144 cache told by copy while the scope state is unknown --------------------------------------------------------------------------------------------------
    {
        // the copy: 0 whenever the scope tells the copies apart; otherwise 1 + the game's selected slot id; none when the selection cannot be read
        check(f144::copyKey(ls::kScopeYes, true, 3) == 0 && f144::copyKey(ls::kScopeNo, true, 2) == 0 && f144::copyKey(ls::kScopeNo, false, 0) == 0 &&
                  f144::copyKey(ls::kScopeYes, false, 0xFFFFFFFFu) == 0,
              "F150: a scope state that is known (yes or no) is the copy: the key is 0 whatever the selection says");
        const uint32_t slot1 = f144::copyKey(ls::kScopeUnknown, true, 2), slot2 = f144::copyKey(ls::kScopeUnknown, true, 3), pistol = f144::copyKey(ls::kScopeUnknown, true, 4);
        check(slot1 == 3 && slot2 == 4 && pistol == 5 && slot1 != slot2 && slot1 != 0 && slot2 != 0,
              "F150: while the scope is unknown the copy is 1 + the selected slot (the flight's sniper copies were slots 2 and 3): two copies differ");
        check(f144::copyKey(ls::kScopeUnknown, false, 2) == f144::kNoCopy && f144::copyKey(ls::kScopeUnknown, true, 0xFFFFFFFFu) == f144::kNoCopy &&
                  f144::copyKey(ls::kScopeUnknown, true, 0xFFFFFFFEu) == f144::kNoCopy && f144::copyKey(ls::kScopeUnknown, true, 0) == 1,
              "F150: scope unknown and no item selected (the holster id, or the selection unreadable): the copy is unknown; slot 0 is a copy like any other");
        // two sniper copies, scope unknown for both: each press finds the line of its own copy, the other copy's line is never handed over
        f144::Cache cache;
        f144::Entry* first = cache.get(kSniper, ls::kScopeUnknown, 100, slot1);
        first->line.have = true;
        first->line.e[2] = 0.05;
        check(cache.find(kSniper, ls::kScopeUnknown, slot1) == first && cache.find(kSniper, ls::kScopeUnknown, slot2) == nullptr && cache.find(kSniper, ls::kScopeUnknown) == nullptr,
              "F150: the line learned on the first copy answers that copy alone: the second copy, and a lookup without a copy, find nothing");
        f144::Entry* second = cache.get(kSniper, ls::kScopeUnknown, 200, slot2);
        check(second != first && !second->line.have && second->copy == slot2 && first->copy == slot1 && first->line.have && near149(first->line.e[2], 0.05, 1e-12),
              "F150: the second copy is its own entry, with no line to start on, and the first keeps its line");
        second->line.have = true;
        second->line.e[2] = 0.16;
        // swapping back and forth: every lookup returns its own copy's line
        bool own = true;
        for (int i = 0; i < 6; ++i) {
            const uint32_t c = (i & 1) ? slot2 : slot1;
            const f144::Entry* e = cache.find(kSniper, ls::kScopeUnknown, c);
            own = own && e && e->copy == c && near149(e->line.e[2], (i & 1) ? 0.16 : 0.05, 1e-12);
            cache.get(kSniper, ls::kScopeUnknown, 300 + i, c);
        }
        check(own && cache.get(kSniper, ls::kScopeUnknown, 400, slot1) == first && cache.get(kSniper, ls::kScopeUnknown, 400, slot2) == second,
              "F150: six swaps between the two copies: each lookup is the line of the copy in hand (5 cm, 16 cm), neither slides to the other");
        // an unidentified copy keeps no line and uses none
        check(cache.get(kSniper, ls::kScopeUnknown, 500, f144::kNoCopy) == nullptr && cache.find(kSniper, ls::kScopeUnknown, f144::kNoCopy) == nullptr,
              "F150: a copy that cannot be told has no entry made and none found");
        // a known scope keeps its one entry whatever the selected slot was: the copy is 0 there, and an entry with the scope does not meet the unknown-scope copies
        f144::Entry* scoped = cache.get(kSniper, ls::kScopeYes, 600, f144::copyKey(ls::kScopeYes, true, 3));
        f144::Entry* scopedAgain = cache.get(kSniper, ls::kScopeYes, 700, f144::copyKey(ls::kScopeYes, true, 2));
        f144::Entry* unscoped = cache.get(kSniper, ls::kScopeNo, 800, f144::copyKey(ls::kScopeNo, false, 0));
        check(scoped == scopedAgain && scoped != first && scoped != second && unscoped != scoped && scoped->copy == 0 && unscoped->copy == 0 && cache.find(kSniper, ls::kScopeYes) == scoped,
              "F150: with the scope known the entry is per (item, scope) as before, the same from either slot, and apart from the unknown-scope copies' entries");
        // the depth never reads a copy's line: both copies sit at the table's 16 cm
        const f149::EyeDepth d1 = f149::eyeDepth(0.0, true, kSniper, ls::kScopeUnknown, cache.find(kSniper, ls::kScopeUnknown, slot1));
        const f149::EyeDepth d2 = f149::eyeDepth(0.0, true, kSniper, ls::kScopeUnknown, cache.find(kSniper, ls::kScopeUnknown, slot2));
        check(d1.askedCm == 16.0 && d2.askedCm == 16.0 && d1.source == f149::kSrcTable && d2.source == f149::kSrcTable,
              "F150: with a line learned on each copy (5 cm and 16 cm) the eye depth is the table's 16 cm for both: a press starts at the same depth on either copy");
        // the shift: each copy's line gives the shift at the same depth, from its own line
        const double p14[3] = {0, 0, 0}, up[3] = {0, 1, 0}, right[3] = {1, 0, 0}, fwd[3] = {0, 0, 1};
        first->line.s[2] = 0.25;
        first->line.s[1] = 0.01;
        second->line.s[2] = 0.36;
        second->line.s[1] = -0.01;
        const f143::Choice c1 = f144::choose(true, true, cache.find(kSniper, ls::kScopeUnknown, slot1), p14, up, right, fwd, p14, 0.20);
        const f143::Choice c2 = f144::choose(true, true, cache.find(kSniper, ls::kScopeUnknown, slot2), p14, up, right, fwd, p14, 0.20);
        const f143::Choice none = f144::choose(true, true, cache.find(kSniper, ls::kScopeUnknown, f144::kNoCopy), p14, up, right, fwd, p14, 0.20);
        check(c1.game && c2.game && c1.gameUpCm > 0.0 && c2.gameUpCm < 0.0 && !none.game && none.why == f143::kNoLine,
              "F150: the sight line's shift comes from the copy in hand's own line (up for the first, down for the second); a copy that cannot be told has no line: the table shifts the eye");
    }
}

}  // namespace f149test
