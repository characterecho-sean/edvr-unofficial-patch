// F149/F150/F152 (native on-foot VR): the ADS eye depth, one per weapon. Header only, no game or D3D11 state, so native_frame_test runs the very functions the ADS sight lock uses.
//
// The ADS eye sits at the sight origin (the FP animation's aiming joint P14, or F79's measured optic origin) + k f, f the FP root's forward. Until F148 k was one setting for every
// weapon (the pilot's 21 cm), and for the laser SMG that put the eye inside the gun. E's forward from P14 (F141's eye matrix E, def_c_povCamera_joint through the HumanoidWeaponAiming
// bend) is the game's own ADS camera depth for the weapon: laser SMG +15.0, kinetic AR +25.0, kinetic SMG +20.0, rocket +22.0, plasma sniper +16.0 unscoped, laser pistol -2.5, plasma
// pistol -7.5.
//
// F152: the depth is base + offset + trim, then the 0..30 cm clamp (adsEyeDistanceClampCm), then the clearance cap against the FP weapon's points (adsEyeDistanceCap, unchanged). The
// old setting, a cap at 21 cm, is gone: it let the pilot move a weapon back but never forward past the weapon's own depth (the laser SMG would not leave 10 cm; the rocket launcher's
// 21.98 was cut to 21).
//   base   the game's unscoped ADS camera depth for the item (kAdsCameraDepths): a measurement, one value per item whatever the scope state. The game backs the camera off with a
//          scope because it zooms at the same time (the scoped plasma sniper's is 5.0, the unscoped one's 16.0); VR cannot zoom, so following the scoped camera would make the
//          scope view smaller than the unscoped one, and the unscoped camera is the base for both.
//   offset the baked back-off from that camera (kAdsBackOffs): 0 for an item without a row. Only the two weapons whose camera puts the eye inside the gun have one. A flown value
//          changes this table and no other.
//   trim   the live setting in the experimental menu (-30..+30 cm, 0 adds nothing), added to the held weapon's depth, whatever the weapon.
// An item without a row in kAdsCameraDepths (the TK Aphelion has no measurement) takes its settled line's E forward as the base when the held copy is known unscoped (the settled line
// feeds the depth of no item that has a row; it still feeds the sight line's shift), else kAdsFallbackBaseCm, the depth every weapon had before F149 (the old setting's 21 cm).
//
// The one value asked feeds the eye point (P14 + k f), the clearance cap, the error to the target eye point and the sight line's shift (F143's Q on the line at the lock's eye depth); a
// change of k between weapons or of the trim reaches the eye through the lock's follower (adsEyeStep).
#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>

#include "journal_loadout.h"
#include "native_on_foot_core.h"
#include "native_on_foot_f139.h"
#include "native_on_foot_f144.h"

namespace edvr {
namespace nofoot {
namespace f149 {

enum Source : int { kSrcNone = 0, kSrcTable = 1, kSrcSettled = 2, kSrcCount = 3 };
inline const char* sourceText(int s) noexcept { return s == kSrcSettled ? "the settled line" : s == kSrcTable ? "the table" : "the fallback base (no measurement)"; }
// The trim's range (cm): the experimental setting's, the same as the readout trims.
constexpr double kTrimMinCm = -30.0, kTrimMaxCm = 30.0;
// The base for an item with no table row and no usable settled line: the depth every weapon had before F149 (the retired setting's shipping 21 cm).
constexpr double kAdsFallbackBaseCm = 21.0;
inline double clampTrimCm(double cm) noexcept {
    if (!std::isfinite(cm)) return 0.0;
    return cm < kTrimMinCm ? kTrimMinCm : cm > kTrimMaxCm ? kTrimMaxCm : cm;
}

struct ItemCm {
    const char* item;   // the exact item name (Status.json SelectedWeapon)
    double cm;
};
// The base: E's forward from P14 for the item's unscoped ADS camera, cm. From the settled sight lines (F144: the "ADS sight line (F143)" lines' E) of the flights
// edvr_gfx_20261027_192734, _201427, edvr_gfx_20261028_050843 and _060723, and for the flight edvr_gfx_20261027_184022 (before F144 kept a line) the "ADS sight point (F141)" lines'
// "E - P14 forward" in the seconds the game's ADS fraction stood at 1.0 on every sample. Every value was the same, to the two decimals the lines print, in every flight and every
// second it was read (the animation's own constant):
//   laser SMG 15.00, kinetic AR 25.00, kinetic SMG 20.00, rocket launcher 21.98, plasma AR 12.99, plasma shotgun 0.01, kinetic pistol -2.50, laser pistol -2.47, plasma pistol -7.51,
//   plasma sniper 16.00 unscoped (the flight 060723, the copy without a scope; the scoped copy's camera is 5.00 and is not used).
constexpr ItemCm kAdsCameraDepths[] = {
    {"$wpn_m_assaultrifle_kinetic_fauto_name;", 25.0},
    {"$wpn_m_assaultrifle_plasma_fauto_name;", 12.99},
    {"$wpn_m_launcher_rocket_sauto_name;", 21.98},
    {"$wpn_m_shotgun_plasma_doublebarrel_name;", 0.01},
    {"$wpn_m_sniper_plasma_charged_name;", 16.0},
    {"$wpn_m_submachinegun_kinetic_fauto_name;", 20.0},
    {"$wpn_m_submachinegun_laser_fauto_name;", 15.0},
    {"$wpn_s_pistol_kinetic_sauto_name;", -2.5},
    {"$wpn_s_pistol_laser_sauto_name;", -2.47},
    {"$wpn_s_pistol_plasma_charged_name;", -7.51},
};
constexpr int kAdsCameraDepthCount = static_cast<int>(sizeof(kAdsCameraDepths) / sizeof(kAdsCameraDepths[0]));
// The offset: how far back of the base the eye sits, cm (negative is back). The kinetic AR's is the pilot's: the camera (25.00) put the eye 21 cm inside the gun (flight
// edvr_gfx_20261028_050843) and he dialled 6 cm with the trim and held it 55 s (flight edvr_gfx_20261028_073013, from 07:33:22): -19.0 from the camera. The laser SMG's -5.0 (10 cm)
// is a first guess, UNCONFIRMED: the pilot could not move it forward of 10 cm (the old cap) and no flight has held another depth for it. An item without a row has offset 0.
constexpr ItemCm kAdsBackOffs[] = {
    {"$wpn_m_assaultrifle_kinetic_fauto_name;", -19.0},
    {"$wpn_m_submachinegun_laser_fauto_name;", -5.0},
};
constexpr int kAdsBackOffCount = static_cast<int>(sizeof(kAdsBackOffs) / sizeof(kAdsBackOffs[0]));

inline bool rowCm(const ItemCm* rows, int count, const char* name, double* cm) noexcept {
    if (cm) *cm = 0.0;
    if (!name) return false;
    for (int i = 0; i < count; ++i) {
        if (std::strcmp(name, rows[i].item) != 0) continue;
        if (cm) *cm = rows[i].cm;
        return true;
    }
    return false;
}
// The item's base (cm) and its offset (cm, 0 where it has no row). False (both 0) for an item with no base row: the offset table is not asked for it.
inline bool tableDepthCm(const char* name, double* baseCm, double* offsetCm) noexcept {
    if (offsetCm) *offsetCm = 0.0;
    if (!rowCm(kAdsCameraDepths, kAdsCameraDepthCount, name, baseCm)) return false;
    rowCm(kAdsBackOffs, kAdsBackOffCount, name, offsetCm);
    return true;
}

struct EyeDepth {
    double trimCm = 0.0;      // the live trim after its -30..30 cm clamp
    double baseCm = 0.0;      // the table's base (the game's unscoped ADS camera depth), the settled line's E forward (cm from P14), or the fallback base
    double offsetCm = 0.0;    // the table's baked back-off (0 for the settled line, the fallback and an item without one)
    double depthCm = 0.0;     // base + offset + trim, before the 0..30 cm clamp (so a clamp shows)
    int source = kSrcNone;    // where the base came from (Source)
    double askedCm = 0.0;     // depthCm after the 0..30 cm clamp: the k asked of the clearance cap
};
// The depth asked for the held item. trimCm: the live setting (clamped here; a NaN reads 0). itemOk: the item snapshot is coherent (F139::itemCoherent). scopeState: the held copy's
// scope state (loadout::kScope*). settled: the held item's F144 entry (null for none), used only for an item without a table row, and only when the copy is known unscoped (scope state
// no); its line is used once it has one (line.have), E's forward in metres in the root's axes.
inline EyeDepth eyeDepth(double trimCm, bool itemOk, const char* name, int scopeState, const f144::Entry* settled) noexcept {
    EyeDepth d;
    d.trimCm = clampTrimCm(trimCm);
    double base = kAdsFallbackBaseCm, offset = 0.0;
    if (itemOk) {
        double tableBase = 0.0, tableOffset = 0.0;
        if (tableDepthCm(name, &tableBase, &tableOffset)) {
            d.source = kSrcTable;
            base = tableBase;
            offset = tableOffset;
        } else if (scopeState == loadout::kScopeNo && settled && settled->line.have && std::isfinite(settled->line.e[2])) {
            d.source = kSrcSettled;
            base = 100.0 * settled->line.e[2];
        }
    }
    d.baseCm = base;
    d.offsetCm = offset;
    d.depthCm = base + offset + d.trimCm;
    d.askedCm = adsEyeDistanceClampCm(d.depthCm);
    return d;
}

}  // namespace f149
}  // namespace nofoot
}  // namespace edvr
