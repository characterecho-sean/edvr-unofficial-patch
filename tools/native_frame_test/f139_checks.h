// F139 checks (native on-foot VR): each weapon's sight on the ADS line. Header only, run by native_frame_test (run139).
// Convention: the shift s is the eye's transverse offset in the FP root's axes, up cm times the root's up row plus right cm times its right row, less its part along the root's
// forward (the sight line); |s| is bounded to 8 cm (past it nothing is moved and the refusal is counted); the up and the right are the held item's table values (an exact item
// name, from a coherent item snapshot). F146 removed the two live trims that tuned the table: the geometry and the bound are checked through shiftFor, the shift of any asked
// up and right, which decide() uses for the table's values.
#pragma once

#include "../../src/d3d11/native_on_foot_core.h"
#include "../../src/d3d11/journal_loadout.h"
#include "../../src/d3d11/native_on_foot_f139.h"

#include <cmath>
#include <cstdint>
#include <cstring>

namespace f139test {

namespace nf = edvr::nofoot;
namespace f139 = edvr::nofoot::f139;
namespace ls = edvr::loadout;

constexpr const char* kPlasma = "$wpn_m_assaultrifle_plasma_fauto_name;";
constexpr const char* kRocket = "$wpn_m_launcher_rocket_sauto_name;";
constexpr const char* kKineticSmg = "$wpn_m_submachinegun_kinetic_fauto_name;";
constexpr const char* kLaserSmg = "$wpn_m_submachinegun_laser_fauto_name;";
constexpr const char* kLaserRifle = "$wpn_m_assaultrifle_laser_fauto_name;";   // F79's rifle: not in the table
constexpr const char* kPistol = "$wpn_s_pistol_kinetic_sauto_name;";
constexpr const char* kPlasmaShotgun = "$wpn_m_shotgun_plasma_doublebarrel_name;";   // F142: the pilot's heights
constexpr const char* kPlasmaPistol = "$wpn_s_pistol_plasma_charged_name;";
constexpr const char* kKineticRifle = "$wpn_m_assaultrifle_kinetic_fauto_name;";

inline double dot3(const double a[3], const double b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
inline double len3(const double a[3]) { return std::sqrt(dot3(a, a)); }
inline bool approx(double a, double b, double tol = 1e-9) { return std::fabs(a - b) <= tol; }

// A decision for the held item, coherent unless said otherwise, on a level root (right +x, up +y, forward +z).
inline f139::Decision level(const char* name, bool readOk = true, uint32_t snapshot = 7, uint32_t current = 7, int scope = ls::kScopeNo) {
    const double up[3] = {0, 1, 0}, right[3] = {1, 0, 0}, fwd[3] = {0, 0, 1};
    return f139::decide(readOk, snapshot, current, name, scope, up, right, fwd);
}
// The shift of an asked up and right (cm) on a level root.
inline f139::Decision asked(double upCm, double rightCm = 0.0) {
    const double up[3] = {0, 1, 0}, right[3] = {1, 0, 0}, fwd[3] = {0, 0, 1};
    f139::Decision d;
    f139::shiftFor(upCm, rightCm, up, right, fwd, &d);
    return d;
}

template <class Check>
inline void run139(Check&& check) {
    {   // the table: exact item names, the pilot's heights (F142), every other item 0
        double up = 9.0, right = 9.0;
        check(f139::tableLookup(kPlasma, ls::kScopeNo, &up, &right) && up == 0.8 && right == 0.0, "F139/F142: the plasma assault rifle's sight is 0.8 cm over the line (the pilot's height; the seed was 1.1)");
        check(f139::tableLookup(kRocket, ls::kScopeNo, &up, &right) && up == 1.7 && right == 0.0, "F139/F142: the rocket launcher's is 1.7 cm over (the pilot's height; the seed was 1.2)");
        check(f139::tableLookup(kKineticSmg, ls::kScopeNo, &up, &right) && up == -2.0 && right == 0.0, "F139: the kinetic SMG's is 2.0 cm under");
        check(f139::tableLookup(kLaserSmg, ls::kScopeNo, &up, &right) && up == -3.0 && right == 0.0, "F139: the laser SMG's is 3.0 cm under (F140: the holographic sight panel above the emitter, not the emitter's -5.8)");
        check(!f139::tableLookup(kLaserRifle, ls::kScopeNo, &up, &right) && up == 0.0 && right == 0.0, "F139: a miss (the laser assault rifle, F79's) gives 0 and 0");
        check(!f139::tableLookup(kPistol, ls::kScopeNo, &up, &right) && up == 0.0 && !f139::tableLookup("", ls::kScopeNo, &up, &right) && !f139::tableLookup(nullptr, ls::kScopeNo, &up, &right), "F139: every other item, the empty name and no name: a miss");
        check(!f139::tableLookup("$wpn_m_assaultrifle_plasma_fauto_name", ls::kScopeNo, &up, &right) && !f139::tableLookup("$WPN_M_ASSAULTRIFLE_PLASMA_FAUTO_NAME;", ls::kScopeNo, &up, &right) &&
                  !f139::tableLookup("$wpn_m_assaultrifle_plasma_fauto_name; ", ls::kScopeNo, &up, &right) && !f139::tableLookup("$wpn_m_assaultrifle_plasma_fauto", ls::kScopeNo, &up, &right),
              "F139: the key is the exact item name: no prefix, no other case, no trailing text");
        check(f139::tableLookup(kPlasmaShotgun, ls::kScopeNo, &up, &right) && up == 1.4 && right == 0.0, "F142: the plasma shotgun's sight is 1.4 cm over the line");
        check(f139::tableLookup(kPlasmaPistol, ls::kScopeNo, &up, &right) && up == 1.0 && right == 0.0, "F142: the plasma pistol's is 1.0 cm over");
        check(f139::tableLookup(kKineticRifle, ls::kScopeNo, &up, &right) && up == -0.6 && right == 0.0, "F142: the kinetic assault rifle's is 0.6 cm under");
        check(!f139::tableLookup("$wpn_s_pistol_laser_sauto_name;", ls::kScopeNo, &up, &right) && !f139::tableLookup(kPistol, ls::kScopeNo, &up, &right),
              "F142: the kinetic and laser pistols stay out of the table (0, the pilot trimmed them and set them back)");
        check(f139::kTableCount == 14, "F142: seven weapons, each under both scope states");
    }
    {   // the shift: the table value along the root's up row, perpendicular to the sight line
        const f139::Decision d = level(kPlasma);
        check(d.coherent && d.weapon && d.table && d.applied && !d.refused && approx(d.tableUpCm, 0.8) && approx(d.askedUpCm, 0.8) && approx(d.appliedUpCm, 0.8) && d.appliedRightCm == 0.0,
              "F139: the plasma rifle on a level root asks and applies 0.8 cm up, no right");
        check(approx(d.shift[0], 0.0) && approx(d.shift[1], 0.008) && approx(d.shift[2], 0.0) && approx(d.shiftM, 0.008), "F139: the shift is 0.8 cm along the root's up row, metres");
        const f139::Decision under = level(kKineticSmg);
        check(under.applied && approx(under.shift[1], -0.020) && approx(under.appliedUpCm, -2.0), "F139: a sight under the line moves the eye down: -2.0 cm");
        const f139::Decision laser = level(kLaserSmg);
        check(laser.applied && approx(laser.shift[1], -0.030) && approx(laser.shiftM, 0.030), "F139: the laser SMG: 3.0 cm down");
        const f139::Decision rocket = level(kRocket);
        check(rocket.applied && approx(rocket.shift[1], 0.017), "F139: the rocket launcher: 1.7 cm up");
        const f139::Decision shotgun = level(kPlasmaShotgun), pistolP = level(kPlasmaPistol), kinRifle = level(kKineticRifle);
        check(shotgun.applied && approx(shotgun.shift[1], 0.014) && pistolP.applied && approx(pistolP.shift[1], 0.010) && kinRifle.applied && approx(kinRifle.shift[1], -0.006),
              "F142: the plasma shotgun 1.4 cm up, the plasma pistol 1.0 up, the kinetic assault rifle 0.6 down");
        // the right part rides the root's right row (the table's right is 0; shiftFor takes both)
        check(approx(level(kPlasma).shift[0], 0.0), "F139: no right in the table: nothing along the right row");
    }
    {   // perpendicular to the sight line, whatever the axes: a tilted root, and an up row with a part along the forward
        // a root yawed 35 deg and pitched 25 deg up (rows right, up, forward), orthonormal
        const double yaw = 35.0 * nf::kAdsDeg, pitch = 25.0 * nf::kAdsDeg;
        const double fwd[3] = {std::sin(yaw) * std::cos(pitch), std::sin(pitch), std::cos(yaw) * std::cos(pitch)};
        const double right[3] = {std::cos(yaw), 0.0, -std::sin(yaw)};
        const double up[3] = {fwd[1] * right[2] - fwd[2] * right[1], fwd[2] * right[0] - fwd[0] * right[2], fwd[0] * right[1] - fwd[1] * right[0]};
        const double upSign = up[1] < 0.0 ? -1.0 : 1.0;
        const double upRow[3] = {upSign * up[0], upSign * up[1], upSign * up[2]};
        f139::Decision d;
        f139::shiftFor(-4.8, 0.0, upRow, right, fwd, &d);
        check(d.applied && approx(d.askedUpCm, -4.8) && approx(d.shiftM, 0.048, 1e-9), "F139: on a tilted orthonormal root the shift is exactly the asked 4.8 cm");
        check(std::fabs(dot3(d.shift, fwd)) < 1e-12, "F139: the shift is perpendicular to the sight line (forward)");
        check(approx(dot3(d.shift, upRow), -0.048, 1e-9) && std::fabs(dot3(d.shift, right)) < 1e-12, "F139: and lies along the up row");
        // an up row that leans into the forward: its part along the sight line is removed
        const double lean[3] = {0.0, 1.0, 0.5};
        const double f0[3] = {0, 0, 1}, r0[3] = {1, 0, 0};
        const f139::Decision l = f139::decide(true, 3, 3, kPlasma, ls::kScopeNo, lean, r0, f0);
        const double leanLen = std::sqrt(1.0 + 0.25);
        check(l.applied && std::fabs(l.shift[2]) < 1e-12 && approx(l.shift[1], 0.008 * (1.0 / leanLen), 1e-12) && approx(l.shiftM, 0.008 / leanLen, 1e-12),
              "F139: an up row with a part along the forward gives only its transverse part (perpendicular to f)");
        // axes of any length: the cm stay cm
        const double big[3] = {0.0, 3.0, 0.0};
        check(approx(f139::decide(true, 3, 3, kPlasma, ls::kScopeNo, big, r0, f0).shift[1], 0.008), "F139: the up row is unitised: 0.8 cm is 0.8 cm");
    }
    {   // a miss gives 0
        const f139::Decision d = level(kPistol);
        check(d.coherent && d.weapon && !d.table && !d.applied && !d.refused && d.appliedUpCm == 0.0 && d.shiftM == 0.0 && d.shift[0] == 0.0 && d.shift[1] == 0.0 && d.shift[2] == 0.0,
              "F139: an item the table does not hold: no shift, not a refusal");
        const f139::Decision laser = level(kLaserRifle);
        check(laser.coherent && !laser.table && !laser.applied && laser.shiftM == 0.0, "F139: the laser assault rifle is not in the table: F79 alone");
        const f139::Decision tool = level("$humanoid_companalyser_name;");
        check(tool.coherent && !tool.weapon && !tool.table && !tool.applied && !tool.refused, "F139: a tool is not a weapon and has no table row: nothing");
    }
    {   // any asked shift (what the table's values are)
        const f139::Decision d = asked(1.3);
        check(d.applied && approx(d.askedUpCm, 1.3) && approx(d.shift[1], 0.013), "F139: an asked 1.3 cm up is a 1.3 cm shift");
        const f139::Decision none = asked(0.0, 0.0);
        check(!none.applied && !none.refused && none.askedUpCm == 0.0 && none.shiftM == 0.0, "F139: nothing asked leaves nothing to move (not a refusal)");
        const f139::Decision down = asked(-1.5);
        check(down.applied && approx(down.shift[1], -0.015), "F139: an asked 1.5 cm down moves the eye down");
    }
    {   // the bound refuses
        const f139::Decision edge = asked(8.0);
        check(edge.applied && !edge.refused && approx(edge.shiftM, 0.08), "F139: 8.0 cm exactly is inside the bound");
        const f139::Decision both = asked(-9.0);
        check(both.refused && !both.applied && both.shiftM == 0.0 && both.shift[1] == 0.0 && both.appliedUpCm == 0.0 && approx(both.askedUpCm, -9.0),
              "F139: 9.0 cm is refused: no shift, counted (refused)");
        check(asked(-8.0).applied && asked(-8.1).refused, "F139: -8.0 applies; -8.1 is refused");
        check(asked(8.8).refused && asked(8.1).refused && asked(8.0).applied, "F139: 8.8 and 8.1 cm are refused; 8.0 applies");
        check(f139::kBoundM == 0.08, "F139: the bound is 8 cm");
    }
    {   // the laser assault rifle: F79's origin stands exactly as it was; an asked shift goes on top of it
        const float rifle51[16] = {1.000000119f, -0.000128559768f, 0.000163342338f, 0, 0.000128652901f, 0.999999821f, 0.000350989401f, 0,
                                   -0.000163567252f, -0.000351212918f, 1.000000238f, 0, -0.000151023269f, -0.134648457f, 0.260953605f, 1};
        const float rifleRoot[16] = {-0.508981764f, 0.487074763f, 0.709715366f, 0, 0.571072876f, -0.425843567f, 0.701807201f, 0,
                                     0.644060254f, 0.762506127f, -0.0614085943f, 0, -18.481029388f, 48.807460657f, 45.268551693f, 1};
        const double rifleP14World[3] = {-18.4810581, 48.8074646, 45.268589};
        float invRoot[16], p14[16], rifle52[16];
        float worldPoint[3] = {static_cast<float>(rifleP14World[0]), static_cast<float>(rifleP14World[1]), static_cast<float>(rifleP14World[2])};
        const bool inv = nf::pfzAffineInverse(rifleRoot, invRoot);
        double local[3] = {};
        if (inv) nf::ikXform(worldPoint, invRoot, local);
        nf::pfzIdentity(p14);
        for (int i = 0; i < 3; ++i) p14[12 + i] = static_cast<float>(local[i]);
        std::memcpy(rifle52, rifle51, sizeof(rifle52));
        rifle52[12] += 0.345f;
        const nf::AdsRifleAnchorResult f79 = nf::adsRifleOpticOrigin(true, true, p14, rifle51, rifle52, rifleRoot);
        check(inv && f79.applied, "F139 fixture: F79's measured origin applies on the captured laser rifle frame");
        const double upRow[3] = {rifleRoot[4], rifleRoot[5], rifleRoot[6]}, rightRow[3] = {rifleRoot[0], rifleRoot[1], rifleRoot[2]}, fwd[3] = {rifleRoot[8], rifleRoot[9], rifleRoot[10]};
        // the loop's order: F79's origin first, then F139's shift added to it
        const f139::Decision untrimmed = f139::decide(true, 5, 5, kLaserRifle, ls::kScopeNo, upRow, rightRow, fwd);
        double originNoShift[3], originShift[3];
        f139::Decision shifted;
        f139::shiftFor(1.5, 0.0, upRow, rightRow, fwd, &shifted);
        for (int i = 0; i < 3; ++i) {
            originNoShift[i] = f79.origin[i] + untrimmed.shift[i];
            originShift[i] = f79.origin[i] + shifted.shift[i];
        }
        check(!untrimmed.applied && originNoShift[0] == f79.origin[0] && originNoShift[1] == f79.origin[1] && originNoShift[2] == f79.origin[2],
              "F139: the laser assault rifle (not in the table): F79's origin exactly (nothing added)");
        const double step[3] = {originShift[0] - f79.origin[0], originShift[1] - f79.origin[1], originShift[2] - f79.origin[2]};
        check(shifted.applied && approx(len3(step), 0.015, 1e-12) && std::fabs(dot3(step, fwd)) < 1e-9 && approx(dot3(step, upRow), 0.015, 1e-6),
              "F139: an asked +1.5 cm moves F79's origin by 1.5 cm along the root's up row, perpendicular to the sight line");
        // F79's own shift (anchor less P14, transverse) is unchanged by it, and the total transverse offset from raw P14 is the sum of the two
        double fromRaw[3] = {originShift[0] - f79.rawOrigin[0], originShift[1] - f79.rawOrigin[1], originShift[2] - f79.rawOrigin[2]};
        check(std::fabs(dot3(fromRaw, fwd)) < 1e-5 && approx(len3(f79.shift), f79.transverseResidualM, 1e-12), "F139: the total offset from raw P14 stays transverse and F79's own shift is untouched");
        // the same ask off the rifle is the same vector: F79 and F139 are independent
        f139::Decision other;
        f139::shiftFor(1.5, 0.0, upRow, rightRow, fwd, &other);
        check(approx(other.shift[0], shifted.shift[0], 1e-15) && approx(other.shift[1], shifted.shift[1], 1e-15) && approx(other.shift[2], shifted.shift[2], 1e-15),
              "F139: the shift does not depend on whether F79 applied");
    }
    {   // a stale or incoherent item snapshot gives no table value
        check(f139::itemCoherent(true, 42, 42, kPlasma) && !f139::itemCoherent(false, 42, 42, kPlasma) && !f139::itemCoherent(true, 42, 43, kPlasma) && !f139::itemCoherent(true, 42, 42, "") &&
                  !f139::itemCoherent(true, 42, 42, nullptr),
              "F139: the snapshot is coherent when it was read, its serial is the held item's, and it names an item");
        const f139::Decision unread = level(kPlasma, false);
        check(!unread.coherent && !unread.table && unread.tableUpCm == 0.0 && !unread.applied && !unread.refused && unread.shiftM == 0.0,
              "F139: an item that was not read: no table value, no shift");
        const f139::Decision stale = level(kPlasma, true, 7, 8);
        check(!stale.coherent && !stale.table && stale.tableUpCm == 0.0 && !stale.applied && stale.shift[1] == 0.0,
              "F139: a snapshot of another serial (the item changed between the reads): no table value");
        const f139::Decision nameless = level("");
        check(!nameless.coherent && !nameless.applied, "F139: no item name: nothing");
        const double up[3] = {0, 1, 0}, right[3] = {1, 0, 0}, fwd[3] = {0, 0, 1};
        check(!f139::decide(true, 1, 1, nullptr, ls::kScopeNo, up, right, fwd).coherent, "F139: a null name is not coherent");
    }
    {   // an axis that cannot be used refuses what was asked, and only that
        const double zero[3] = {0, 0, 0}, right[3] = {1, 0, 0}, fwd[3] = {0, 0, 1}, up[3] = {0, 1, 0}, bad[3] = {std::nan(""), 1, 0};
        const f139::Decision d = f139::decide(true, 1, 1, kPlasma, ls::kScopeNo, zero, right, fwd);
        check(d.refused && !d.applied && d.shiftM == 0.0, "F139: an up row with no length refuses the shift");
        check(f139::decide(true, 1, 1, kPlasma, ls::kScopeNo, up, right, zero).refused && f139::decide(true, 1, 1, kPlasma, ls::kScopeNo, up, right, bad).refused && f139::decide(true, 1, 1, kPlasma, ls::kScopeNo, bad, right, fwd).refused,
              "F139: a forward with no length, a row that is not a number: refused");
        check(!f139::decide(true, 1, 1, kPistol, ls::kScopeNo, zero, zero, zero).refused, "F139: with nothing asked the axes are not looked at");
    }
    {   // F140: the table is keyed by (item, scope fitted)
        double up = 9.0, right = 9.0;
        for (const f139::Entry& e : f139::kTable) {
            const bool hit = f139::tableLookup(e.item, e.scope, &up, &right);
            check(hit && up == e.upCm && right == e.rightCm, "F140: every row of the sight table is found under its own scope state");
        }
        check(f139::tableLookup(kPlasma, ls::kScopeNo, &up, &right) && up == 0.8 && f139::tableLookup(kPlasma, ls::kScopeYes, &up, &right) && up == 0.0,
              "F140: a table hit for each scope state: the plasma rifle's 0.8 cm is the no-scope row, the scoped row starts at 0");
        for (const char* w : {kPlasma, kRocket, kKineticSmg, kLaserSmg, kPlasmaShotgun, kPlasmaPistol, kKineticRifle}) {
            check(f139::tableLookup(w, ls::kScopeYes, &up, &right) && up == 0.0 && right == 0.0, "F140: every weapon's scoped sight row exists and starts at 0 until tuned");
        }
        check(f139::tableLookup(kPlasma, ls::kScopeUnknown, &up, &right) && up == 0.8 && f139::tableLookup(kLaserSmg, ls::kScopeUnknown, &up, &right) && up == -3.0,
              "F140: an unknown scope state uses the no-scope row");
        const f139::Decision no = level(kPlasma, true, 7, 7, ls::kScopeNo), yes = level(kPlasma, true, 7, 7, ls::kScopeYes), unk = level(kPlasma, true, 7, 7, ls::kScopeUnknown);
        check(no.table && no.applied && approx(no.askedUpCm, 0.8) && !no.scopeUnknown && no.entryScope == ls::kScopeNo, "F140: a no-scope plasma rifle: the 0.8 cm row, state known");
        check(yes.table && !yes.applied && yes.askedUpCm == 0.0 && !yes.scopeUnknown && yes.entryScope == ls::kScopeYes && yes.scope == ls::kScopeYes,
              "F140: a scoped plasma rifle: its own row (0 until tuned), nothing moved");
        check(unk.table && unk.applied && approx(unk.askedUpCm, 0.8) && unk.scopeUnknown && unk.entryScope == ls::kScopeNo && unk.scope == ls::kScopeUnknown,
              "F140: an unknown state takes the no-scope row and says it was unknown (counted by the caller)");
        check(!level(kPlasma, false, 7, 7, ls::kScopeYes).coherent && !level(kPlasma, true, 7, 8, ls::kScopeYes).coherent,
              "F140: an incoherent snapshot gives nothing under any scope state");
    }
    {   // F142: the right column rides the root's right row (the table's right is 0 for every weapon, so shiftFor takes it directly)
        const double up[3] = {0, 1, 0}, right[3] = {1, 0, 0}, fwd[3] = {0, 0, 1};
        const f139::Decision d = asked(1.4, 1.5);
        check(d.applied && approx(d.askedUpCm, 1.4) && approx(d.askedRightCm, 1.5) && approx(d.appliedUpCm, 1.4) && approx(d.appliedRightCm, 1.5),
              "F142: an asked 1.4 cm up and 1.5 cm right are both applied");
        check(approx(d.shift[0], 0.015) && approx(d.shift[1], 0.014) && approx(d.shift[2], 0.0) && approx(d.shiftM, std::sqrt(0.015 * 0.015 + 0.014 * 0.014), 1e-12),
              "F142: the shift is 1.4 cm up and 1.5 cm along the root's right row, metres");
        const f139::Decision both = asked(-4.0, -2.0);
        check(both.applied && approx(both.askedUpCm, -4.0) && approx(both.askedRightCm, -2.0) && approx(both.shift[0], -0.02) && approx(both.shift[1], -0.04),
              "F142: -4.0 up and -2.0 right (left) at once");
        const f139::Decision rightOnly = asked(0.0, 2.5);
        check(rightOnly.applied && rightOnly.askedUpCm == 0.0 && approx(rightOnly.askedRightCm, 2.5) && approx(rightOnly.shift[0], 0.025) && rightOnly.shift[1] == 0.0,
              "F142: a right shift alone");
        // the bound is the whole shift's: 6 up and 6 right is 8.49 cm, refused; 4.8 and 6.4 is exactly 8.0
        check(asked(6.0, 6.0).refused && asked(4.8, 6.4).applied, "F142: the 8 cm bound is on the whole shift (up and right together)");
        // the right row is unitised and loses its part along the sight line, as the up row does
        const double lean[3] = {1.0, 0.0, 0.5};
        f139::Decision l;
        f139::shiftFor(0.0, 1.0, up, lean, fwd, &l);
        const double leanLen = std::sqrt(1.25);
        check(l.applied && std::fabs(l.shift[2]) < 1e-12 && approx(l.shift[0], 0.01 / leanLen, 1e-12), "F142: a right row with a part along the forward gives only its transverse part");
        // the table asks no right for any weapon
        check(approx(level(kPlasma).askedRightCm, 0.0) && approx(level(kPlasmaShotgun).askedRightCm, 0.0), "F142: the table asks no right");
    }
}

}  // namespace f139test
