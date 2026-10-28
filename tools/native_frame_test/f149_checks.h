// F149 checks (native on-foot VR), run149 below. Header only, run by native_frame_test.
//
// F149 (native_on_foot_f149.h, native_on_foot_core.h): the ADS eye depth is min(the key, the game's ADS camera depth for the held (item, scope)), then the key's 0..30 cm clamp, then
// the clearance cap. The camera depth comes from the settled line when one has settled this session, else from the baked table, else there is none and the key stands alone. The
// checks: the table's values and names; the rule on every table row (never further forward than the key; the laser SMG 15, the kinetic AR 21, the pistols 0, an unknown item 21); the
// source order (a settled line over the table, a line that has not settled, a non-finite one, an unknown scope); the clearance cap after the rule; the sight line's shift taken at the
// same depth; the lock's follower carrying a depth change.
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
constexpr const char* kLaserRifle = "$wpn_m_assaultrifle_laser_fauto_name;";   // F79's rifle: no row

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
    // ---- 1. the table ------------------------------------------------------------------------------------------------------------------------------------------
    {
        double cm = 9.0;
        check(f149::tableDepthCm(kLaserSmg, ls::kScopeNo, &cm) && cm == 15.0, "F149: the laser SMG's ADS camera is 15 cm forward of P14");
        check(f149::tableDepthCm(kKineticRifle, ls::kScopeNo, &cm) && cm == 25.0, "F149: the kinetic assault rifle's is 25 cm");
        check(f149::tableDepthCm(kKineticSmg, ls::kScopeNo, &cm) && cm == 20.0, "F149: the kinetic SMG's is 20 cm");
        check(f149::tableDepthCm(kRocket, ls::kScopeNo, &cm) && cm == 21.98, "F149: the rocket launcher's is 21.98 cm");
        check(f149::tableDepthCm(kPlasmaRifle, ls::kScopeNo, &cm) && cm == 12.99, "F149: the plasma assault rifle's is 12.99 cm");
        check(f149::tableDepthCm(kShotgun, ls::kScopeNo, &cm) && cm == 0.01, "F149: the plasma shotgun's is 0.01 cm");
        check(f149::tableDepthCm(kSniper, ls::kScopeYes, &cm) && cm == 5.0, "F149: the scoped plasma sniper's is 5 cm");
        check(f149::tableDepthCm(kKineticPistol, ls::kScopeNo, &cm) && cm == -2.5 && f149::tableDepthCm(kLaserPistol, ls::kScopeNo, &cm) && cm == -2.47 &&
                  f149::tableDepthCm(kPlasmaPistol, ls::kScopeNo, &cm) && cm == -7.51,
              "F149: the pistols' cameras are behind P14 (-2.5, -2.47 and -7.51 cm)");
        check(!f149::tableDepthCm(kSniper, ls::kScopeNo, &cm) && cm == 0.0, "F149: the sniper without its scope has no row (a scope moves the camera): a miss gives 0");
        check(!f149::tableDepthCm(kLaserRifle, ls::kScopeNo, &cm) && !f149::tableDepthCm("", ls::kScopeNo, &cm) && !f149::tableDepthCm(nullptr, ls::kScopeNo, &cm) &&
                  !f149::tableDepthCm("$wpn_m_submachinegun_laser_fauto_name", ls::kScopeNo, &cm) && !f149::tableDepthCm("$WPN_M_SUBMACHINEGUN_LASER_FAUTO_NAME;", ls::kScopeNo, &cm),
              "F149: the laser assault rifle (F79's), the empty name, no name and a name not exact are misses");
        check(f149::tableDepthCm(kLaserSmg, ls::kScopeUnknown, &cm) && cm == 15.0 && !f149::tableDepthCm(kSniper, ls::kScopeUnknown, &cm),
              "F149: an unknown scope state reads the no-scope row, as F139's table does");
        // every row names an item the readout's table (F144) knows, is a weapon, and appears once
        bool known = true, weapons = true, unique = true, finite = true;
        for (int i = 0; i < f149::kAdsCameraDepthCount; ++i) {
            const f149::CameraDepth& r = f149::kAdsCameraDepths[i];
            known = known && f144::find(r.item, r.scope, f144::kHip) != nullptr;
            weapons = weapons && nf::f139::isWeapon(r.item);
            finite = finite && std::isfinite(r.cm) && r.cm > -50.0 && r.cm < 50.0;
            for (int j = i + 1; j < f149::kAdsCameraDepthCount; ++j)
                unique = unique && !(r.scope == f149::kAdsCameraDepths[j].scope && std::strcmp(r.item, f149::kAdsCameraDepths[j].item) == 0);
        }
        check(f149::kAdsCameraDepthCount == 10 && known && weapons && unique && finite,
              "F149: the ten rows each name a weapon the readout table knows (a typo would be a miss), once, with a plausible depth");
    }
    // ---- 2. the rule --------------------------------------------------------------------------------------------------------------------------------------------
    {
        const double key = 21.0;
        const f149::EyeDepth smg = f149::eyeDepth(key, true, kLaserSmg, ls::kScopeNo, nullptr);
        check(smg.source == f149::kSrcTable && smg.cameraCm == 15.0 && smg.keyCm == 21.0 && smg.askedCm == 15.0,
              "F149: the laser SMG at the pilot's 21 cm key: the eye goes back to the game's camera, 15 cm (it was inside the gun at 21)");
        const f149::EyeDepth ar = f149::eyeDepth(key, true, kKineticRifle, ls::kScopeNo, nullptr);
        check(ar.source == f149::kSrcTable && ar.cameraCm == 25.0 && ar.askedCm == 21.0, "F149: the kinetic assault rifle: its camera is further forward than the key, so the key stands, 21 cm");
        const f149::EyeDepth rocket = f149::eyeDepth(key, true, kRocket, ls::kScopeNo, nullptr);
        check(rocket.askedCm == 21.0 && rocket.cameraCm == 21.98, "F149: the rocket launcher (21.98 cm) keeps the 21 cm key");
        const f149::EyeDepth ksmg = f149::eyeDepth(key, true, kKineticSmg, ls::kScopeNo, nullptr);
        const f149::EyeDepth sniper = f149::eyeDepth(key, true, kSniper, ls::kScopeYes, nullptr);
        const f149::EyeDepth plasmaRifle = f149::eyeDepth(key, true, kPlasmaRifle, ls::kScopeNo, nullptr);
        const f149::EyeDepth shotgun = f149::eyeDepth(key, true, kShotgun, ls::kScopeNo, nullptr);
        check(ksmg.askedCm == 20.0 && sniper.askedCm == 5.0 && plasmaRifle.askedCm == 12.99 && shotgun.askedCm == 0.01,
              "F149: the kinetic SMG goes to 20 cm, the scoped sniper to 5, the plasma assault rifle to 12.99 and the plasma shotgun to 0.01");
        const f149::EyeDepth pk = f149::eyeDepth(key, true, kKineticPistol, ls::kScopeNo, nullptr), pl = f149::eyeDepth(key, true, kLaserPistol, ls::kScopeNo, nullptr),
                             pp = f149::eyeDepth(key, true, kPlasmaPistol, ls::kScopeNo, nullptr);
        check(pk.askedCm == 0.0 && pl.askedCm == 0.0 && pp.askedCm == 0.0 && pp.cameraCm == -7.51 && pp.source == f149::kSrcTable,
              "F149: the pistols' cameras are behind P14: the depth asked is 0 after the clamp (the plasma pistol's camera is kept as read, -7.51 cm)");
        const f149::EyeDepth unknown = f149::eyeDepth(key, true, "$wpn_m_unlisted_name;", ls::kScopeNo, nullptr);
        const f149::EyeDepth laserRifle = f149::eyeDepth(key, true, kLaserRifle, ls::kScopeNo, nullptr);
        const f149::EyeDepth sniperNoScope = f149::eyeDepth(key, true, kSniper, ls::kScopeNo, nullptr);
        check(unknown.source == f149::kSrcNone && unknown.askedCm == 21.0 && unknown.cameraCm == 0.0 && laserRifle.source == f149::kSrcNone && laserRifle.askedCm == 21.0 &&
                  sniperNoScope.source == f149::kSrcNone && sniperNoScope.askedCm == 21.0,
              "F149: an item with neither a line nor a row (unlisted, the laser assault rifle, the sniper without its scope) has the key alone, 21 cm, source none");
        const f149::EyeDepth notCoherent = f149::eyeDepth(key, false, kLaserSmg, ls::kScopeNo, nullptr);
        check(notCoherent.source == f149::kSrcNone && notCoherent.askedCm == 21.0, "F149: an item snapshot that is not coherent is no item: the key alone");
        // never further forward than the key, on every row and for keys from 0 to 30
        bool never = true, exact = true;
        const double keys[] = {0.0, 1.0, 5.0, 10.0, 15.0, 15.5, 21.0, 25.0, 30.0};
        for (const double k : keys) {
            for (const f149::CameraDepth& r : f149::kAdsCameraDepths) {
                const f149::EyeDepth d = f149::eyeDepth(k, true, r.item, r.scope, nullptr);
                never = never && d.askedCm <= k + 1e-12 && d.askedCm >= 0.0 && d.askedCm <= nf::kAdsEyeDistMaxCm;
                const double want = nf::adsEyeDistanceClampCm(r.cm < k ? r.cm : k);
                exact = exact && d.askedCm == want && d.keyCm == k;
            }
        }
        check(never && exact, "F149: for every row and every key from 0 to 30 cm the depth asked is the lesser of the key and the camera, never above the key, and clamped to 0..30");
        // the key's own clamp and a NaN
        const f149::EyeDepth big = f149::eyeDepth(45.0, true, kKineticRifle, ls::kScopeNo, nullptr);
        const f149::EyeDepth neg = f149::eyeDepth(-3.0, true, kLaserSmg, ls::kScopeNo, nullptr);
        const f149::EyeDepth nan = f149::eyeDepth(std::numeric_limits<double>::quiet_NaN(), true, kLaserSmg, ls::kScopeNo, nullptr);
        const f149::EyeDepth nanUnlisted = f149::eyeDepth(std::numeric_limits<double>::quiet_NaN(), true, "$wpn_m_unlisted_name;", ls::kScopeNo, nullptr);
        check(big.keyCm == 30.0 && big.askedCm == 25.0 && neg.keyCm == 0.0 && neg.askedCm == 0.0 && nan.keyCm == 21.0 && nan.askedCm == 15.0 && nanUnlisted.askedCm == 21.0,
              "F149: a key of 45 reads 30 (the kinetic assault rifle 25), -3 reads 0, and a key that is not a number reads the default 21 before the rule is applied");
    }
    // ---- 3. the source order --------------------------------------------------------------------------------------------------------------------------------------
    {
        const double key = 21.0;
        // a settled line over the table, below it, above it, and for an item the table does not have
        f144::Entry lower = settledEntry(kLaserSmg, ls::kScopeNo, 12.0);
        const f149::EyeDepth a = f149::eyeDepth(key, true, kLaserSmg, ls::kScopeNo, &lower);
        check(a.source == f149::kSrcSettled && near149(a.cameraCm, 12.0) && near149(a.askedCm, 12.0), "F149: a settled line (E 12 cm forward) overrides the table's 15 for the laser SMG: 12 cm");
        f144::Entry deeper = settledEntry(kKineticRifle, ls::kScopeNo, 18.0);
        const f149::EyeDepth b = f149::eyeDepth(key, true, kKineticRifle, ls::kScopeNo, &deeper);
        check(b.source == f149::kSrcSettled && near149(b.askedCm, 18.0), "F149: ... and the kinetic assault rifle's line at 18 cm (the table says 25) takes it back from 21 to 18");
        f144::Entry forward = settledEntry(kLaserSmg, ls::kScopeNo, 30.0);
        const f149::EyeDepth c = f149::eyeDepth(key, true, kLaserSmg, ls::kScopeNo, &forward);
        check(c.source == f149::kSrcSettled && near149(c.cameraCm, 30.0) && c.askedCm == 21.0, "F149: a settled line further forward than the key (30 cm): it is the source, and the key stands");
        f144::Entry unlisted = settledEntry(kLaserRifle, ls::kScopeNo, 10.0);
        const f149::EyeDepth d = f149::eyeDepth(key, true, kLaserRifle, ls::kScopeNo, &unlisted);
        check(d.source == f149::kSrcSettled && near149(d.askedCm, 10.0), "F149: an item with no table row takes the depth from its own settled line once it has one (10 cm)");
        // a line that has not settled, a non-finite one, no entry
        f144::Entry notYet = settledEntry(kLaserSmg, ls::kScopeNo, 12.0, false);
        const f149::EyeDepth e = f149::eyeDepth(key, true, kLaserSmg, ls::kScopeNo, &notYet);
        check(e.source == f149::kSrcTable && e.askedCm == 15.0, "F149: an entry whose line has not settled (no flush yet) is no line: the table's 15");
        f144::Entry nanLine = settledEntry(kLaserSmg, ls::kScopeNo, 12.0);
        nanLine.line.e[2] = std::numeric_limits<double>::quiet_NaN();
        const f149::EyeDepth f = f149::eyeDepth(key, true, kLaserSmg, ls::kScopeNo, &nanLine);
        check(f.source == f149::kSrcTable && f.askedCm == 15.0, "F149: a line whose E is not a number is not used: the table");
        const f149::EyeDepth g = f149::eyeDepth(key, true, kLaserRifle, ls::kScopeNo, &notYet);
        check(g.source == f149::kSrcNone && g.askedCm == 21.0, "F149: ... and with no row either, the key alone");
        // the line is the held (item, scope)'s: the cache finds it by exact name and scope, so another scope's line is never handed over
        f144::Cache cache;
        f144::Entry* sc = cache.get(kSniper, ls::kScopeYes, 100);
        sc->line.have = true;
        sc->line.e[2] = 0.04;
        const f149::EyeDepth withScope = f149::eyeDepth(key, true, kSniper, ls::kScopeYes, cache.find(kSniper, ls::kScopeYes));
        const f149::EyeDepth noScope = f149::eyeDepth(key, true, kSniper, ls::kScopeNo, cache.find(kSniper, ls::kScopeNo));
        check(withScope.source == f149::kSrcSettled && near149(withScope.askedCm, 4.0) && noScope.source == f149::kSrcNone && noScope.askedCm == 21.0,
              "F149: a line settled with the scope (4 cm) answers the scoped sniper only; without the scope there is neither a line nor a row: 21 cm");
        check(std::strcmp(f149::sourceText(f149::kSrcSettled), "the settled line") == 0 && std::strcmp(f149::sourceText(f149::kSrcTable), "the table") == 0 &&
                  std::strcmp(f149::sourceText(f149::kSrcNone), "none") == 0 && std::strcmp(f149::sourceText(99), "none") == 0,
              "F149: the log names the source: the settled line, the table, none");
    }
    // ---- 4. the clearance cap after the rule, the sight line at the same depth, the follower ------------------------------------------------------------------------------------------
    {
        const double p14[3] = {1.0, 1.5, -2.0}, f[3] = {0.0, 0.0, 1.0};
        const f149::EyeDepth smg = f149::eyeDepth(21.0, true, kLaserSmg, ls::kScopeNo, nullptr);
        double cutPoint[1][3] = {{p14[0], p14[1], p14[2] + 0.18}}, farPoint[1][3] = {{p14[0], p14[1], p14[2] + 0.30}};
        const nf::AdsEyeCap cut = nf::adsEyeDistanceCap(smg.askedCm / 100.0, p14, f, cutPoint, 1), kept = nf::adsEyeDistanceCap(smg.askedCm / 100.0, p14, f, farPoint, 1);
        const nf::AdsEyeCap oldKey = nf::adsEyeDistanceCap(0.21, p14, f, cutPoint, 1);
        check(cut.capped && near149(cut.kM, 0.12) && !kept.capped && near149(kept.kM, 0.15) && oldKey.capped && near149(oldKey.kM, 0.12),
              "F149: the clearance cap takes the depth asked: a weapon point 18 cm ahead on the line cuts the laser SMG's 15 cm to 12, one 30 cm ahead leaves 15 (the key's 21 was cut to 12 as well)");
        // the sight line's shift is taken at the same k: a line that leaves the root's forward by 2 cm over 20 cm has 0 cm up at the camera (15 cm) and 0.6 cm up at 21 cm
        f144::Entry ent = settledEntry(kLaserSmg, ls::kScopeNo, 15.0);
        ent.line.s[1] = 0.02;
        const double up[3] = {0, 1, 0}, right[3] = {1, 0, 0}, origin[3] = {p14[0], p14[1], p14[2]};
        const f143::Choice atAsked = f144::choose(true, true, &ent, p14, up, right, f, origin, smg.askedCm / 100.0);
        const f143::Choice atKey = f144::choose(true, true, &ent, p14, up, right, f, origin, 0.21);
        check(atAsked.game && near149(atAsked.gameUpCm, 0.0, 1e-9) && atKey.game && near149(atKey.gameUpCm, 0.6, 1e-9) && near149(atAsked.q[2] - origin[2], 0.15, 1e-12) &&
                  near149(atKey.q[2] - origin[2], 0.21, 1e-12),
              "F149: the line's shift is Q on the line at the depth asked: 0 cm up at the camera's 15 cm, 0.6 cm up if it were taken at 21 (the same k as the eye point)");
        // the lock's follower carries the change of k between weapons: the target moves 6 cm back along the sight (the laser SMG after a weapon at 21), no overshoot, 1 mm off in 0.3 s
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
              "F149: a 6 cm change of k reaches the eye through the existing follower: no frame over the per-frame cap, no overshoot, within 3 mm in 0.3 s");
    }
}

}  // namespace f149test
