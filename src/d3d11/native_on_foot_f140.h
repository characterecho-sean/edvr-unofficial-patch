// F140 (native on-foot VR): the weapon readout placed per weapon. Header only, no game or D3D11 state, so native_frame_test runs the very functions the weapon UI decision uses.
// Journal: the arc doc, 2026-10-27, F140.
//
// The weapon readout (the ammunition counter the game draws on the weapon) was placed by the CLASS of the readout part the game draws (F90 pistol, F92 rifle ADS; F93a's drawn
// class), each with four TEMPORARY keys (fix.native_on_foot_pistol_readout_* and fix.native_on_foot_rifle_ads_readout_*, ini-only since F127). They are retired: the same
// values are now a table of each weapon's own placement, keyed by (exact item name, scope fitted). When a placement applies is unchanged (F93a's drawn class, the latch, the
// refusals); only which values it uses changes. A pistol-class part drawn by an item whose entry says otherwise uses the entry.
//
// The values are forward, left, up and toward eye, in cm, in the weapon frame with the same target() as before (native_on_foot_f90.h). Four TEMPORARY live trims,
// fix.native_on_foot_weapon_ui_forward_cm, _left_cm, _up_cm and _toward_eye_cm (default 0), are added to the held weapon's values and the totals clamped to the ranges the
// class keys had; the pilot tunes each weapon in one flight and the tuned totals go into the table. An item that is not in the table, or a name that cannot be trusted (its
// snapshot was not read, or its serial is not the held item's), keeps the old behaviour: the drawn class's built-in values (an untrusted name takes no trim).
//
// The scope state comes from the journal's loadout (journal_loadout.h); a state that is not known uses the no-scope row and is counted. Both rows of a weapon start at the same
// values, the class built-ins, until the pilot tunes them.
#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>

#include "journal_loadout.h"
#include "native_on_foot_f90.h"

namespace edvr {
namespace nofoot {
namespace f140 {

enum Class : int { kClassPistol = 0, kClassRifle = 1, kClasses = 2 };
inline const char* className(int c) noexcept { return c == kClassPistol ? "pistol" : "rifle"; }
// The class built-in values (the pilot's from the F91 and F92 flights): forward, left, up, toward eye, cm.
constexpr double kPistolClass[4] = {f90::kForwardDefaultCm, f90::kLeftDefaultCm, f90::kUpDefaultCm, f90::kTowardDefaultCm};
constexpr double kRifleClass[4] = {f90::kRifleForwardDefaultCm, f90::kRifleLeftDefaultCm, f90::kRifleUpDefaultCm, f90::kRifleTowardDefaultCm};
inline const double* classValues(int cls) noexcept { return cls == kClassPistol ? kPistolClass : kRifleClass; }

// The totals' ranges and the trims' own. F145: the trims run -30..+30 cm on all four axes (the pilot ran the shotgun's and the laser SMG's forward trim into its +10 stop in ADS),
// and the totals' ranges are the same -30..+30 so that a trim is never silently cut by a narrower total (the retired class keys' ranges were forward -10..30, left -15..15, up
// -10..10, toward eye -5..10).
constexpr double kMinCm[4] = {-30.0, -30.0, -30.0, -30.0};
constexpr double kMaxCm[4] = {30.0, 30.0, 30.0, 30.0};
constexpr double kTrimMinCm[4] = {-30.0, -30.0, -30.0, -30.0};
constexpr double kTrimMaxCm[4] = {30.0, 30.0, 30.0, 30.0};
inline double clampTotal(int i, double cm) noexcept { return f88::clampCm(cm, kMinCm[i], kMaxCm[i]); }
inline double clampTrim(int i, double cm) noexcept { return f88::clampCm(cm, kTrimMinCm[i], kTrimMaxCm[i]); }

struct Entry {
    const char* item;   // the exact item name (Status.json SelectedWeapon)
    int scope;          // loadout::kScopeNo or kScopeYes
    double v[4];        // forward, left, up, toward eye (cm)
};
// The nine weapons seen in the pilot's logs, each under both scope states. Seeds: the values each gets today, which are its drawn class's. The class of the part an item draws
// is read from the Frontier logs' `rifle ADS readout placement (F92)` and `pistol readout (F90)` lines, frames moved by each over the flights the item was held in
// (2026-10-26/27, 25 logs): the three pistols place by the pistol class (the pistol line, 69,580 + 2,446 + 5,404 hip-readout frames for the kinetic, laser and plasma
// pistols); every `$wpn_m_` weapon places by the rifle class (the rifle line: laser assault rifle 65,094, plasma sniper 20,074, kinetic SMG 7,113, laser SMG 7,335, plasma
// assault rifle 4,588, rocket launcher 5,964 frames), the few pistol-class frames at a switch being the previous item's part drawn while Status.json still named the new one.
// The rocket launcher is rifle class by those lines (13:08:55 to 13:09:29 in edvr_gfx_20261027_130251: 180 rifle-placed frames a second, no pistol-placed frame until the
// switch to the pistol at 13:09:29.6). None of the nine had a scope entry measured: the scoped rows start at the class values.
constexpr Entry kTable[] = {
    {"$wpn_m_assaultrifle_laser_fauto_name;", loadout::kScopeNo, {0.0, 3.5, 0.0, 0.0}},
    {"$wpn_m_assaultrifle_laser_fauto_name;", loadout::kScopeYes, {0.0, 3.5, 0.0, 0.0}},
    {"$wpn_m_assaultrifle_plasma_fauto_name;", loadout::kScopeNo, {0.0, 3.5, 0.0, 0.0}},
    {"$wpn_m_assaultrifle_plasma_fauto_name;", loadout::kScopeYes, {0.0, 3.5, 0.0, 0.0}},
    {"$wpn_m_launcher_rocket_sauto_name;", loadout::kScopeNo, {0.0, 3.5, 0.0, 0.0}},
    {"$wpn_m_launcher_rocket_sauto_name;", loadout::kScopeYes, {0.0, 3.5, 0.0, 0.0}},
    {"$wpn_m_sniper_plasma_charged_name;", loadout::kScopeNo, {0.0, 3.5, 0.0, 0.0}},
    {"$wpn_m_sniper_plasma_charged_name;", loadout::kScopeYes, {0.0, 3.5, 0.0, 0.0}},
    {"$wpn_m_submachinegun_kinetic_fauto_name;", loadout::kScopeNo, {0.0, 3.5, 0.0, 0.0}},
    {"$wpn_m_submachinegun_kinetic_fauto_name;", loadout::kScopeYes, {0.0, 3.5, 0.0, 0.0}},
    {"$wpn_m_submachinegun_laser_fauto_name;", loadout::kScopeNo, {0.0, 3.5, 0.0, 0.0}},
    {"$wpn_m_submachinegun_laser_fauto_name;", loadout::kScopeYes, {0.0, 3.5, 0.0, 0.0}},
    {"$wpn_s_pistol_kinetic_sauto_name;", loadout::kScopeNo, {13.0, 3.5, -3.0, 3.0}},
    {"$wpn_s_pistol_kinetic_sauto_name;", loadout::kScopeYes, {13.0, 3.5, -3.0, 3.0}},
    {"$wpn_s_pistol_laser_sauto_name;", loadout::kScopeNo, {13.0, 3.5, -3.0, 3.0}},
    {"$wpn_s_pistol_laser_sauto_name;", loadout::kScopeYes, {13.0, 3.5, -3.0, 3.0}},
    {"$wpn_s_pistol_plasma_charged_name;", loadout::kScopeNo, {13.0, 3.5, -3.0, 3.0}},
    {"$wpn_s_pistol_plasma_charged_name;", loadout::kScopeYes, {13.0, 3.5, -3.0, 3.0}},
};
constexpr int kTableCount = static_cast<int>(sizeof(kTable) / sizeof(kTable[0]));
inline int entryScopeOf(int scopeState) noexcept { return scopeState == loadout::kScopeYes ? loadout::kScopeYes : loadout::kScopeNo; }
inline const Entry* find(const char* name, int scopeState) noexcept {
    if (!name) return nullptr;
    const int scope = entryScopeOf(scopeState);
    for (const Entry& e : kTable)
        if (e.scope == scope && std::strcmp(name, e.item) == 0) return &e;
    return nullptr;
}
inline bool isWeapon(const char* name) noexcept { return name && std::strncmp(name, "$wpn_", 5) == 0; }
inline bool itemCoherent(bool readOk, uint32_t snapshotSerial, uint32_t currentSerial, const char* name) noexcept {
    return readOk && snapshotSerial == currentSerial && name && name[0];
}

struct Placement {
    bool coherent = false;       // the item snapshot was usable
    bool weapon = false;         // a $wpn_ item: the trim applies
    bool table = false;          // the item has an entry (under the row used)
    int cls = kClassRifle;       // the class whose built-in values stood in for a miss
    int scope = loadout::kScopeUnknown;
    int entryScope = loadout::kScopeNo;
    bool scopeUnknown = false;
    double seed[4] = {};         // the entry's values, else the class built-ins
    double trim[4] = {};         // added (0 unless a coherent $wpn_ item)
    double total[4] = {};        // seed + trim, clamped to the ranges
    f90::Keys keys() const noexcept {
        f90::Keys k;
        k.forwardCm = total[0];
        k.leftCm = total[1];
        k.upCm = total[2];
        k.towardCm = total[3];
        return k;
    }
};
// The placement for one class of part (the pistol's hip and ADS readouts, or the rifle's ADS readout) of the held item. cls picks the built-in values a miss or an untrusted
// name falls back to. trim: the four live keys (clamped here).
inline Placement place(bool readOk, uint32_t snapshotSerial, uint32_t currentSerial, const char* name, int scopeState, int cls, const double trim[4]) noexcept {
    Placement p;
    p.cls = cls;
    p.scope = scopeState;
    p.entryScope = entryScopeOf(scopeState);
    p.scopeUnknown = scopeState != loadout::kScopeNo && scopeState != loadout::kScopeYes;
    const double* base = classValues(cls);
    for (int i = 0; i < 4; ++i) p.seed[i] = base[i];
    p.coherent = itemCoherent(readOk, snapshotSerial, currentSerial, name);
    if (p.coherent) {
        p.weapon = isWeapon(name);
        if (const Entry* e = find(name, scopeState)) {
            p.table = true;
            for (int i = 0; i < 4; ++i) p.seed[i] = e->v[i];
        }
        if (p.weapon && trim)
            for (int i = 0; i < 4; ++i) p.trim[i] = clampTrim(i, trim[i]);
    }
    for (int i = 0; i < 4; ++i) p.total[i] = clampTotal(i, p.seed[i] + p.trim[i]);
    return p;
}

}  // namespace f140
}  // namespace nofoot
}  // namespace edvr
