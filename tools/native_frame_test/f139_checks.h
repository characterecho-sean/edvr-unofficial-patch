// F139 checks (native on-foot VR): each weapon's sight on the ADS line. Header only, run by native_frame_test (run139).
// Convention: the shift s is the eye's transverse offset in the FP root's axes, up cm times the root's up row plus right cm times its right row, less its part along the root's
// forward (the sight line); |s| is bounded to 8 cm (past it nothing is moved and the refusal is counted); the up is the held item's table value (an exact item name, from a
// coherent item snapshot) plus the live trim, which any held $wpn_ item takes.
#pragma once

#include "../../src/d3d11/native_on_foot_core.h"
#include "../../src/d3d11/native_on_foot_f139.h"

#include <cmath>
#include <cstdint>
#include <cstring>

namespace f139test {

namespace nf = edvr::nofoot;
namespace f139 = edvr::nofoot::f139;

constexpr const char* kPlasma = "$wpn_m_assaultrifle_plasma_fauto_name;";
constexpr const char* kRocket = "$wpn_m_launcher_rocket_sauto_name;";
constexpr const char* kKineticSmg = "$wpn_m_submachinegun_kinetic_fauto_name;";
constexpr const char* kLaserSmg = "$wpn_m_submachinegun_laser_fauto_name;";
constexpr const char* kLaserRifle = "$wpn_m_assaultrifle_laser_fauto_name;";   // F79's rifle: not in the table
constexpr const char* kPistol = "$wpn_s_pistol_kinetic_sauto_name;";

inline double dot3(const double a[3], const double b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
inline double len3(const double a[3]) { return std::sqrt(dot3(a, a)); }
inline bool approx(double a, double b, double tol = 1e-9) { return std::fabs(a - b) <= tol; }

// A decision for the held item, coherent unless said otherwise, on a level root (right +x, up +y, forward +z).
inline f139::Decision level(const char* name, double trimCm = 0.0, bool readOk = true, uint32_t snapshot = 7, uint32_t current = 7) {
    const double up[3] = {0, 1, 0}, right[3] = {1, 0, 0}, fwd[3] = {0, 0, 1};
    return f139::decide(readOk, snapshot, current, name, trimCm, up, right, fwd);
}

template <class Check>
inline void run139(Check&& check) {
    {   // the table: exact item names, the four seeds, every other item 0
        double up = 9.0, right = 9.0;
        check(f139::tableLookup(kPlasma, &up, &right) && up == 1.1 && right == 0.0, "F139: the plasma assault rifle's sight is 1.1 cm over the line");
        check(f139::tableLookup(kRocket, &up, &right) && up == 1.2 && right == 0.0, "F139: the rocket launcher's is 1.2 cm over");
        check(f139::tableLookup(kKineticSmg, &up, &right) && up == -2.0 && right == 0.0, "F139: the kinetic SMG's is 2.0 cm under");
        check(f139::tableLookup(kLaserSmg, &up, &right) && up == -5.8 && right == 0.0, "F139: the laser SMG's is 5.8 cm under");
        check(!f139::tableLookup(kLaserRifle, &up, &right) && up == 0.0 && right == 0.0, "F139: a miss (the laser assault rifle, F79's) gives 0 and 0");
        check(!f139::tableLookup(kPistol, &up, &right) && up == 0.0 && !f139::tableLookup("", &up, &right) && !f139::tableLookup(nullptr, &up, &right), "F139: every other item, the empty name and no name: a miss");
        check(!f139::tableLookup("$wpn_m_assaultrifle_plasma_fauto_name", &up, &right) && !f139::tableLookup("$WPN_M_ASSAULTRIFLE_PLASMA_FAUTO_NAME;", &up, &right) &&
                  !f139::tableLookup("$wpn_m_assaultrifle_plasma_fauto_name; ", &up, &right) && !f139::tableLookup("$wpn_m_assaultrifle_plasma_fauto", &up, &right),
              "F139: the key is the exact item name: no prefix, no other case, no trailing text");
        check(f139::kTableCount == 4, "F139: four seeds");
    }
    {   // the shift: the table value along the root's up row, perpendicular to the sight line
        const f139::Decision d = level(kPlasma);
        check(d.coherent && d.weapon && d.table && d.applied && !d.refused && approx(d.tableUpCm, 1.1) && d.trimUpCm == 0.0 && approx(d.askedUpCm, 1.1) && approx(d.appliedUpCm, 1.1) &&
                  d.appliedRightCm == 0.0,
              "F139: the plasma rifle on a level root asks and applies 1.1 cm up, no right, no trim");
        check(approx(d.shift[0], 0.0) && approx(d.shift[1], 0.011) && approx(d.shift[2], 0.0) && approx(d.shiftM, 0.011), "F139: the shift is 1.1 cm along the root's up row, metres");
        const f139::Decision under = level(kKineticSmg);
        check(under.applied && approx(under.shift[1], -0.020) && approx(under.appliedUpCm, -2.0), "F139: a sight under the line moves the eye down: -2.0 cm");
        const f139::Decision laser = level(kLaserSmg);
        check(laser.applied && approx(laser.shift[1], -0.058) && approx(laser.shiftM, 0.058), "F139: the laser SMG: 5.8 cm down");
        const f139::Decision rocket = level(kRocket);
        check(rocket.applied && approx(rocket.shift[1], 0.012), "F139: the rocket launcher: 1.2 cm up");
        // the right part rides the root's right row (the table's right is 0 for now; the shift function takes both)
        const double up[3] = {0, 1, 0}, right[3] = {1, 0, 0}, fwd[3] = {0, 0, 1};
        check(approx(f139::decide(true, 1, 1, kPlasma, 0.0, up, right, fwd).shift[0], 0.0), "F139: no right in the table: nothing along the right row");
    }
    {   // perpendicular to the sight line, whatever the axes: a tilted root, and an up row with a part along the forward
        // a root yawed 35 deg and pitched 25 deg up (rows right, up, forward), orthonormal
        const double yaw = 35.0 * nf::kAdsDeg, pitch = 25.0 * nf::kAdsDeg;
        const double fwd[3] = {std::sin(yaw) * std::cos(pitch), std::sin(pitch), std::cos(yaw) * std::cos(pitch)};
        const double right[3] = {std::cos(yaw), 0.0, -std::sin(yaw)};
        const double up[3] = {fwd[1] * right[2] - fwd[2] * right[1], fwd[2] * right[0] - fwd[0] * right[2], fwd[0] * right[1] - fwd[1] * right[0]};
        const double upSign = up[1] < 0.0 ? -1.0 : 1.0;
        const double upRow[3] = {upSign * up[0], upSign * up[1], upSign * up[2]};
        const f139::Decision d = f139::decide(true, 3, 3, kLaserSmg, 1.0, upRow, right, fwd);   // -5.8 + 1.0 = -4.8 cm
        check(d.applied && approx(d.askedUpCm, -4.8) && approx(d.shiftM, 0.048, 1e-9), "F139: on a tilted orthonormal root the shift is exactly the asked 4.8 cm");
        check(std::fabs(dot3(d.shift, fwd)) < 1e-12, "F139: the shift is perpendicular to the sight line (forward)");
        check(approx(dot3(d.shift, upRow), -0.048, 1e-9) && std::fabs(dot3(d.shift, right)) < 1e-12, "F139: and lies along the up row");
        // an up row that leans into the forward: its part along the sight line is removed
        const double lean[3] = {0.0, 1.0, 0.5};
        const double f0[3] = {0, 0, 1}, r0[3] = {1, 0, 0};
        const f139::Decision l = f139::decide(true, 3, 3, kPlasma, 0.0, lean, r0, f0);
        const double leanLen = std::sqrt(1.0 + 0.25);
        check(l.applied && std::fabs(l.shift[2]) < 1e-12 && approx(l.shift[1], 0.011 * (1.0 / leanLen), 1e-12) && approx(l.shiftM, 0.011 / leanLen, 1e-12),
              "F139: an up row with a part along the forward gives only its transverse part (perpendicular to f)");
        // axes of any length: the cm stay cm
        const double big[3] = {0.0, 3.0, 0.0};
        check(approx(f139::decide(true, 3, 3, kPlasma, 0.0, big, r0, f0).shift[1], 0.011), "F139: the up row is unitised: 1.1 cm is 1.1 cm");
    }
    {   // a miss gives 0
        const f139::Decision d = level(kPistol);
        check(d.coherent && d.weapon && !d.table && !d.applied && !d.refused && d.appliedUpCm == 0.0 && d.shiftM == 0.0 && d.shift[0] == 0.0 && d.shift[1] == 0.0 && d.shift[2] == 0.0,
              "F139: an item the table does not hold, with no trim: no shift, not a refusal");
        const f139::Decision laser = level(kLaserRifle);
        check(laser.coherent && !laser.table && !laser.applied && laser.shiftM == 0.0, "F139: the laser assault rifle is not in the table: F79 alone");
        const f139::Decision tool = level("$humanoid_companalyser_name;", 3.0);
        check(tool.coherent && !tool.weapon && tool.trimUpCm == 0.0 && !tool.applied && !tool.refused, "F139: the trim is for a held weapon ($wpn_ item): a tool takes none");
    }
    {   // the trim adds
        const f139::Decision d = level(kPlasma, 0.5);
        check(d.applied && approx(d.tableUpCm, 1.1) && approx(d.trimUpCm, 0.5) && approx(d.askedUpCm, 1.6) && approx(d.shift[1], 0.016), "F139: the trim adds to the table: 1.1 + 0.5 = 1.6 cm");
        const f139::Decision neg = level(kPlasma, -1.1);
        check(!neg.applied && !neg.refused && neg.askedUpCm < 1e-12 && neg.askedUpCm > -1e-12, "F139: a trim that cancels the table leaves nothing to move (not a refusal)");
        const f139::Decision miss = level(kPistol, 2.5);
        check(miss.applied && miss.table == false && approx(miss.askedUpCm, 2.5) && approx(miss.shift[1], 0.025), "F139: any held weapon takes the trim: the pistol, not in the table, 2.5 cm");
        const f139::Decision rifle = level(kLaserRifle, -1.5);
        check(rifle.applied && approx(rifle.shift[1], -0.015), "F139: the laser assault rifle takes the trim too (on top of F79)");
        check(approx(level(kPistol, 20.0).askedUpCm, 8.0) && approx(level(kPistol, -20.0).askedUpCm, -8.0) && level(kPistol, std::nan("")).trimUpCm == 0.0 &&
                  level(kPistol, INFINITY).trimUpCm == 0.0 && approx(f139::clampTrimCm(7.9), 7.9) && approx(f139::clampTrimCm(-8.0), -8.0),
              "F139: the trim is clamped to -8..8 cm and a value that is not a number is 0");
    }
    {   // the bound refuses
        const f139::Decision edge = level(kPistol, 8.0);
        check(edge.applied && !edge.refused && approx(edge.shiftM, 0.08), "F139: 8.0 cm exactly is inside the bound");
        const f139::Decision both = level(kLaserSmg, -4.0);   // -5.8 - 4.0 = -9.8
        check(both.refused && !both.applied && both.shiftM == 0.0 && both.shift[1] == 0.0 && both.appliedUpCm == 0.0 && approx(both.askedUpCm, -9.8),
              "F139: the laser SMG's -5.8 with a trim of -4.0 is 9.8 cm: refused, no shift, counted (refused)");
        check(level(kLaserSmg, -2.2).applied && level(kLaserSmg, -2.3).refused, "F139: -5.8 - 2.2 = 8.0 applies; -5.8 - 2.3 = 8.1 is refused");
        check(level(kPlasma, 8.0).refused && level(kPlasma, 7.0).refused && level(kPlasma, 6.9).applied, "F139: 1.1 + 8.0 = 9.1 and 1.1 + 7.0 = 8.1 cm are refused; 1.1 + 6.9 = 8.0 applies");
        check(f139::kBoundM == 0.08, "F139: the bound is 8 cm");
    }
    {   // the laser assault rifle: F79's origin stands exactly as it was, the trim goes on top of it
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
        const f139::Decision untrimmed = f139::decide(true, 5, 5, kLaserRifle, 0.0, upRow, rightRow, fwd);
        double originNoTrim[3], originTrim[3];
        const f139::Decision trimmed = f139::decide(true, 5, 5, kLaserRifle, 1.5, upRow, rightRow, fwd);
        for (int i = 0; i < 3; ++i) {
            originNoTrim[i] = f79.origin[i] + untrimmed.shift[i];
            originTrim[i] = f79.origin[i] + trimmed.shift[i];
        }
        check(!untrimmed.applied && originNoTrim[0] == f79.origin[0] && originNoTrim[1] == f79.origin[1] && originNoTrim[2] == f79.origin[2],
              "F139: the laser assault rifle with no trim: F79's origin exactly (nothing added)");
        const double step[3] = {originTrim[0] - f79.origin[0], originTrim[1] - f79.origin[1], originTrim[2] - f79.origin[2]};
        check(trimmed.applied && approx(len3(step), 0.015, 1e-12) && std::fabs(dot3(step, fwd)) < 1e-9 && approx(dot3(step, upRow), 0.015, 1e-6),
              "F139: a trim of +1.5 cm moves F79's origin by 1.5 cm along the root's up row, perpendicular to the sight line");
        // F79's own shift (anchor less P14, transverse) is unchanged by the trim, and the total transverse offset from raw P14 is the sum of the two
        double fromRaw[3] = {originTrim[0] - f79.rawOrigin[0], originTrim[1] - f79.rawOrigin[1], originTrim[2] - f79.rawOrigin[2]};
        check(std::fabs(dot3(fromRaw, fwd)) < 1e-5 && approx(len3(f79.shift), f79.transverseResidualM, 1e-12), "F139: the total offset from raw P14 stays transverse and F79's own shift is untouched");
        // the same trim off the rifle is the same vector: F79 and F139 are independent
        const f139::Decision pistol = f139::decide(true, 5, 5, kPistol, 1.5, upRow, rightRow, fwd);
        check(approx(pistol.shift[0], trimmed.shift[0], 1e-15) && approx(pistol.shift[1], trimmed.shift[1], 1e-15) && approx(pistol.shift[2], trimmed.shift[2], 1e-15),
              "F139: the shift does not depend on whether F79 applied");
    }
    {   // a stale or incoherent item snapshot gives no table value and no trim
        check(f139::itemCoherent(true, 42, 42, kPlasma) && !f139::itemCoherent(false, 42, 42, kPlasma) && !f139::itemCoherent(true, 42, 43, kPlasma) && !f139::itemCoherent(true, 42, 42, "") &&
                  !f139::itemCoherent(true, 42, 42, nullptr),
              "F139: the snapshot is coherent when it was read, its serial is the held item's, and it names an item");
        const f139::Decision unread = level(kPlasma, 3.0, false);
        check(!unread.coherent && !unread.table && unread.tableUpCm == 0.0 && unread.trimUpCm == 0.0 && !unread.applied && !unread.refused && unread.shiftM == 0.0,
              "F139: an item that was not read: no table value, no trim, no shift");
        const f139::Decision stale = level(kPlasma, 3.0, true, 7, 8);
        check(!stale.coherent && !stale.table && stale.tableUpCm == 0.0 && stale.trimUpCm == 0.0 && !stale.applied && stale.shift[1] == 0.0,
              "F139: a snapshot of another serial (the item changed between the reads): no table value, no trim");
        const f139::Decision nameless = level("", 3.0);
        check(!nameless.coherent && !nameless.applied, "F139: no item name: nothing");
        const double up[3] = {0, 1, 0}, right[3] = {1, 0, 0}, fwd[3] = {0, 0, 1};
        check(!f139::decide(true, 1, 1, nullptr, 3.0, up, right, fwd).coherent, "F139: a null name is not coherent");
    }
    {   // an axis that cannot be used refuses what was asked, and only that
        const double zero[3] = {0, 0, 0}, right[3] = {1, 0, 0}, fwd[3] = {0, 0, 1}, up[3] = {0, 1, 0}, bad[3] = {std::nan(""), 1, 0};
        const f139::Decision d = f139::decide(true, 1, 1, kPlasma, 0.0, zero, right, fwd);
        check(d.refused && !d.applied && d.shiftM == 0.0, "F139: an up row with no length refuses the shift");
        check(f139::decide(true, 1, 1, kPlasma, 0.0, up, right, zero).refused && f139::decide(true, 1, 1, kPlasma, 0.0, up, right, bad).refused && f139::decide(true, 1, 1, kPlasma, 0.0, bad, right, fwd).refused,
              "F139: a forward with no length, a row that is not a number: refused");
        check(!f139::decide(true, 1, 1, kPistol, 0.0, zero, zero, zero).refused, "F139: with nothing asked the axes are not looked at");
    }
}

}  // namespace f139test
