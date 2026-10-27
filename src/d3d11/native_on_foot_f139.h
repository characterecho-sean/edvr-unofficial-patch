// F139 (native on-foot VR): each weapon's sight on the ADS line. Header only, no game or D3D11 state, so native_frame_test runs the very functions the ADS sight lock uses.
// Journal: the arc doc, 2026-10-27, "four weapons' sights off the ADS line".
//
// The ADS eye target is adsOrigin + k f: adsOrigin is the FP animation's aiming joint (P14) in the world, or F79's measured optic origin for the laser assault rifle only, and f
// is the FP root's forward. The shots fly along P14's line, but a weapon's sight does not always sit on it (the F138 flight edvr_gfx_20261027_130251, measured in the right
// eye): the plasma assault rifle's sight about 1.1 cm over the line, the rocket launcher's 0.8 to 1.7 cm over, the kinetic SMG's about 2.0 cm under, the laser SMG's under it
// (see the table). So the eye moves onto each weapon's own sight line: up for a sight over the line, down for one under it.
//
// The shift s is the eye's transverse offset in the FP root's axes, up cm times the root's up row plus right cm times its right row, less its part along f (the sight line is
// along f, so only the transverse part moves the eye off it). Its size is bounded: past kBoundM no shift is made and the refusal is counted. The up is the held item's table
// value plus the live trim fix.native_on_foot_ads_sight_up_cm, and (F142) the right is the table's plus the live trim fix.native_on_foot_ads_sight_right_cm; both trims apply to
// any held weapon ($wpn_ item): the pilot tunes each weapon in one flight and the tuned totals go into the table. The item must be the coherent snapshot F79 uses (read, and its
// serial the held item's); a stale or incoherent one gives nothing.
//
// F140: the table is keyed by (item name, scope fitted): a scope modification moves a weapon's aiming point. The scope state comes from the journal's loadout events
// (journal_loadout.h). A state that is not known (no loadout read, the name in no slot, two slots with different mods) uses the no-scope entry and is counted as unknown.
// Every entry for a scope is 0 until tuned; the live trim applies whatever the state is.
#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>

#include "journal_loadout.h"

namespace edvr {
namespace nofoot {
namespace f139 {

constexpr double kBoundM = 0.08;                             // |s| past this is refused: no shift
constexpr double kTrimMinCm = -8.0, kTrimMaxCm = 8.0;        // the live trim's bounds
constexpr char kWeaponPrefix[] = "$wpn_";

struct Entry {
    const char* item;   // the exact item name (Status.json SelectedWeapon)
    int scope;          // loadout::kScopeNo or kScopeYes: the scope state the figures were measured under
    double upCm;        // + up, in the FP root's axes
    double rightCm;     // + right (0 for every entry until the lateral trim, F142, has measured one)
};
// Seeds from the 2026-10-27 dumps of edvr_gfx_20261027_130251 (13:02 to 13:11 local, 19:02 to 19:11 UTC); the journal has every weapon of that flight without a scope mod
// (the plasma assault rifle at 19:05:53 had weapon_stability and weapon_handling, the rocket launcher, the kinetic SMG and the pistol none, the laser SMG a suppression mod
// only), so the seeds are the no-scope entries. The scoped entries start at 0 until the pilot's tuning flight.
//
// F142: the pilot's own heights from the 16:03 flight (edvr_gfx_20261027_160358, the live trim's totals, no scope): the rocket launcher +1.7 (seed 1.2), the plasma assault
// rifle +0.8 (seed 1.1), the plasma shotgun +1.4, the plasma pistol +1.0 and the kinetic assault rifle -0.6 (these three were 0). The kinetic SMG (-2.0) and the laser SMG
// (-3.0: no height aligned it in that flight, see below) are as before; the kinetic and laser pistols stay out of the table (0).
//
// The laser SMG's -3.0 (F140): the VR dump measured -5.8 cm to the weapon's blue emitter, where no reticle was visible. The pilot's flat-screen scope chart (every weapon in
// ADS, without and with the scope mod) shows this SMG's unscoped sight is a translucent holographic panel floating above that emitter, the target centred in the panel; in
// flat the emitter's top sits about 3 cm under the sight line. -5.8 would put the eye at the emitter, under the sight.
constexpr Entry kTable[] = {
    {"$wpn_m_assaultrifle_plasma_fauto_name;", loadout::kScopeNo, +0.8, 0.0},
    {"$wpn_m_assaultrifle_plasma_fauto_name;", loadout::kScopeYes, 0.0, 0.0},
    {"$wpn_m_launcher_rocket_sauto_name;", loadout::kScopeNo, +1.7, 0.0},
    {"$wpn_m_launcher_rocket_sauto_name;", loadout::kScopeYes, 0.0, 0.0},
    {"$wpn_m_submachinegun_kinetic_fauto_name;", loadout::kScopeNo, -2.0, 0.0},
    {"$wpn_m_submachinegun_kinetic_fauto_name;", loadout::kScopeYes, 0.0, 0.0},
    {"$wpn_m_submachinegun_laser_fauto_name;", loadout::kScopeNo, -3.0, 0.0},
    {"$wpn_m_submachinegun_laser_fauto_name;", loadout::kScopeYes, 0.0, 0.0},
    {"$wpn_m_shotgun_plasma_doublebarrel_name;", loadout::kScopeNo, +1.4, 0.0},
    {"$wpn_m_shotgun_plasma_doublebarrel_name;", loadout::kScopeYes, 0.0, 0.0},
    {"$wpn_s_pistol_plasma_charged_name;", loadout::kScopeNo, +1.0, 0.0},
    {"$wpn_s_pistol_plasma_charged_name;", loadout::kScopeYes, 0.0, 0.0},
    {"$wpn_m_assaultrifle_kinetic_fauto_name;", loadout::kScopeNo, -0.6, 0.0},
    {"$wpn_m_assaultrifle_kinetic_fauto_name;", loadout::kScopeYes, 0.0, 0.0},
};
constexpr int kTableCount = static_cast<int>(sizeof(kTable) / sizeof(kTable[0]));

// The item snapshot is usable: it was read, its serial is the held item's now (the item did not change between the two reads), and it names an item.
inline bool itemCoherent(bool readOk, uint32_t snapshotSerial, uint32_t currentSerial, const char* name) noexcept {
    return readOk && snapshotSerial == currentSerial && name && name[0];
}
inline bool isWeapon(const char* name) noexcept { return name && std::strncmp(name, kWeaponPrefix, sizeof(kWeaponPrefix) - 1) == 0; }
// The scope state's table row: a state that is not known uses the no-scope row.
inline int entryScopeOf(int scopeState) noexcept { return scopeState == loadout::kScopeYes ? loadout::kScopeYes : loadout::kScopeNo; }
// The table's up and right for an exact item name under a scope state; false (both 0) for a miss.
inline bool tableLookup(const char* name, int scopeState, double* upCm, double* rightCm) noexcept {
    if (upCm) *upCm = 0.0;
    if (rightCm) *rightCm = 0.0;
    if (!name) return false;
    const int scope = entryScopeOf(scopeState);
    for (int i = 0; i < kTableCount; ++i) {
        if (kTable[i].scope != scope || std::strcmp(name, kTable[i].item) != 0) continue;
        if (upCm) *upCm = kTable[i].upCm;
        if (rightCm) *rightCm = kTable[i].rightCm;
        return true;
    }
    return false;
}
// The live trim: its bounds, 0 for a value that is not a number.
inline double clampTrimCm(double cm) noexcept {
    if (!std::isfinite(cm)) return 0.0;
    return cm < kTrimMinCm ? kTrimMinCm : cm > kTrimMaxCm ? kTrimMaxCm : cm;
}

struct Decision {
    bool coherent = false;      // the item snapshot was usable
    bool weapon = false;        // a $wpn_ item: the trim applies
    bool table = false;         // the item is in the table (under the scope row used)
    int scope = loadout::kScopeUnknown;   // the held weapon's scope state as given
    int entryScope = loadout::kScopeNo;   // the table row used for it (an unknown state uses the no-scope row)
    bool scopeUnknown = false;  // the state was not known
    double tableUpCm = 0.0, tableRightCm = 0.0, trimUpCm = 0.0, trimRightCm = 0.0;
    double askedUpCm = 0.0, askedRightCm = 0.0;           // table plus trim, each way
    bool refused = false;       // something was asked, but |s| is past the bound (or an axis is unusable): no shift
    bool applied = false;       // a shift was made
    double shift[3] = {};       // world axes, metres: to be added to the sight origin
    double shiftM = 0.0;        // |shift|
    double appliedUpCm = 0.0, appliedRightCm = 0.0;       // the asked values when applied, else 0
};

namespace detail {
inline bool unit3(const double a[3], double out[3]) noexcept {
    const double l = std::sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
    if (!std::isfinite(l) || !(l > 1e-9)) return false;
    for (int i = 0; i < 3; ++i) out[i] = a[i] / l;
    return true;
}
}  // namespace detail

// The decision for one frame of the lock. up, right and forward are the FP root's up row, right row and forward row in the world (any length: they are unitised); trimUpCm and
// trimRightCm are the live keys' values (clamped here); scopeState the held weapon's (loadout::Scope).
inline Decision decide(bool readOk, uint32_t snapshotSerial, uint32_t currentSerial, const char* name, int scopeState, double trimUpCm, double trimRightCm, const double up[3],
                       const double right[3], const double forward[3]) noexcept {
    Decision d;
    d.scope = scopeState;
    d.entryScope = entryScopeOf(scopeState);
    d.scopeUnknown = scopeState != loadout::kScopeNo && scopeState != loadout::kScopeYes;
    if (!itemCoherent(readOk, snapshotSerial, currentSerial, name)) return d;   // no item known: no table value, no trim
    d.coherent = true;
    d.weapon = isWeapon(name);
    d.table = tableLookup(name, scopeState, &d.tableUpCm, &d.tableRightCm);
    d.trimUpCm = d.weapon ? clampTrimCm(trimUpCm) : 0.0;
    d.trimRightCm = d.weapon ? clampTrimCm(trimRightCm) : 0.0;
    d.askedUpCm = d.tableUpCm + d.trimUpCm;
    d.askedRightCm = d.tableRightCm + d.trimRightCm;
    if (d.askedUpCm == 0.0 && d.askedRightCm == 0.0) return d;                 // nothing asked
    double u[3], r[3], f[3];
    if (!detail::unit3(up, u) || !detail::unit3(right, r) || !detail::unit3(forward, f)) {
        d.refused = true;
        return d;
    }
    double raw[3], s[3];
    for (int i = 0; i < 3; ++i) raw[i] = 0.01 * (d.askedUpCm * u[i] + d.askedRightCm * r[i]);
    const double along = raw[0] * f[0] + raw[1] * f[1] + raw[2] * f[2];
    for (int i = 0; i < 3; ++i) s[i] = raw[i] - along * f[i];
    const double len = std::sqrt(s[0] * s[0] + s[1] * s[1] + s[2] * s[2]);
    if (!std::isfinite(len) || len > kBoundM + 1e-9) {
        d.refused = true;
        return d;
    }
    for (int i = 0; i < 3; ++i) d.shift[i] = s[i];
    d.shiftM = len;
    d.applied = true;
    d.appliedUpCm = d.askedUpCm;
    d.appliedRightCm = d.askedRightCm;
    return d;
}
// The height-only form (F139's), for callers and checks that have no lateral trim: the right trim is 0.
inline Decision decide(bool readOk, uint32_t snapshotSerial, uint32_t currentSerial, const char* name, int scopeState, double trimUpCm, const double up[3], const double right[3],
                       const double forward[3]) noexcept {
    return decide(readOk, snapshotSerial, currentSerial, name, scopeState, trimUpCm, 0.0, up, right, forward);
}

}  // namespace f139
}  // namespace nofoot
}  // namespace edvr
