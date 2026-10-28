// F149 (native on-foot VR): the ADS eye depth from the game's own ADS camera, never further forward than the key. Header only, no game or D3D11 state, so native_frame_test runs
// the very functions the ADS sight lock uses.
//
// The ADS eye sits at the sight origin (the FP animation's aiming joint P14, or F79's measured optic origin) + k f, f the FP root's forward. Until F148 k was the key
// (fix.native_on_foot_ads_eye_distance_cm, the pilot's 21 cm) for every weapon, and for the laser SMG that put the eye inside the gun: the game's own ADS camera (F141's eye matrix E,
// def_c_povCamera_joint through the HumanoidWeaponAiming bend) sits 15.0 cm forward of P14 for it, and the eye was 6 cm ahead of that camera, toward the gun. E's forward from P14
// is the game's ADS camera depth for the weapon: laser SMG +15.0, kinetic AR +25.0, kinetic SMG +20.0, rocket +22.0, scoped plasma sniper +5.0, laser pistol -2.5, plasma pistol -7.5.
//
// The rule: the depth asked for the held (item, scope) is min(the key, the game's ADS camera depth), then the key's own 0..30 cm clamp (adsEyeDistanceClampCm), then the clearance
// cap against the FP weapon's points (adsEyeDistanceCap, unchanged). The key therefore reads "the furthest forward the ADS eye sits"; where the game's camera is further back than
// the key the eye goes back to the camera (the pistols' camera is behind P14, so 0 after the clamp), and where it is further forward (the kinetic AR, the rocket launcher) the key stands.
//
// The camera depth, in this order: the held (item, scope)'s settled line when one has settled this session (E's forward, the F144 line), else kAdsCameraDepths (baked from the
// flights below), else none (the key alone). The one value asked feeds the eye point (P14 + k f), the clearance cap, the error to the target eye point and the sight line's shift
// (F143's Q on the line at the lock's eye depth); a change of k between weapons, or when a line first settles, reaches the eye through the lock's follower (adsEyeStep).
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
inline const char* sourceText(int s) noexcept { return s == kSrcSettled ? "the settled line" : s == kSrcTable ? "the table" : "none"; }

struct CameraDepth {
    const char* item;   // the exact item name (Status.json SelectedWeapon)
    int scope;          // loadout::kScopeNo or kScopeYes: the scope state the depth was read under
    double cm;          // E's forward from P14, cm: the game's ADS camera depth
};
// E's forward from P14 per (item, scope), from the settled sight lines (F144: the "ADS sight line (F143)" lines' E) of the flights edvr_gfx_20261027_192734, _201427 and
// edvr_gfx_20261028_050843, and for the flight edvr_gfx_20261027_184022 (before F144 kept a line) the "ADS sight point (F141)" lines' "E - P14 forward" in the seconds the
// game's ADS fraction stood at 1.0 on every sample. Every value was the same, to the two decimals the lines print, in every flight and every second it was read (the animation's
// own constant):
//   laser SMG 15.00 (all four flights), kinetic AR 25.00 (all four), kinetic SMG 20.00 (184022, 192734, 201427), rocket launcher 21.98 (184022, 192734, 201427),
//   plasma AR 12.99 (184022, 192734, 201427), plasma shotgun 0.01 (184022, 192734, 201427), scoped plasma sniper 5.00 (184022, 201427), kinetic pistol -2.50 (184022, 192734, 201427),
//   laser pistol -2.47 (184022, 192734, 201427), plasma pistol -7.51 (184022, 192734, 201427).
// A weapon or scope state not listed has no depth until its own line settles in the session: the key alone.
constexpr CameraDepth kAdsCameraDepths[] = {
    {"$wpn_m_assaultrifle_kinetic_fauto_name;", loadout::kScopeNo, 25.0},
    {"$wpn_m_assaultrifle_plasma_fauto_name;", loadout::kScopeNo, 12.99},
    {"$wpn_m_launcher_rocket_sauto_name;", loadout::kScopeNo, 21.98},
    {"$wpn_m_shotgun_plasma_doublebarrel_name;", loadout::kScopeNo, 0.01},
    {"$wpn_m_sniper_plasma_charged_name;", loadout::kScopeYes, 5.0},
    {"$wpn_m_submachinegun_kinetic_fauto_name;", loadout::kScopeNo, 20.0},
    {"$wpn_m_submachinegun_laser_fauto_name;", loadout::kScopeNo, 15.0},
    {"$wpn_s_pistol_kinetic_sauto_name;", loadout::kScopeNo, -2.5},
    {"$wpn_s_pistol_laser_sauto_name;", loadout::kScopeNo, -2.47},
    {"$wpn_s_pistol_plasma_charged_name;", loadout::kScopeNo, -7.51},
};
constexpr int kAdsCameraDepthCount = static_cast<int>(sizeof(kAdsCameraDepths) / sizeof(kAdsCameraDepths[0]));

// The table's depth (cm) for an exact item name under a scope state; a state that is not known reads the no-scope row (as F139's table does). False (cm 0) for a miss.
inline bool tableDepthCm(const char* name, int scopeState, double* cm) noexcept {
    if (cm) *cm = 0.0;
    if (!name) return false;
    const int scope = f139::entryScopeOf(scopeState);
    for (const CameraDepth& r : kAdsCameraDepths) {
        if (r.scope != scope || std::strcmp(name, r.item) != 0) continue;
        if (cm) *cm = r.cm;
        return true;
    }
    return false;
}

struct EyeDepth {
    double keyCm = 0.0;       // the key after its 0..30 cm clamp
    double cameraCm = 0.0;    // the game's ADS camera depth (cm from P14); 0 with source kSrcNone
    int source = kSrcNone;    // where cameraCm came from (Source)
    double askedCm = 0.0;     // min(key, camera) after the clamp: the k asked of the clearance cap
};
// The depth asked for the held item. keyCm: the setting (clamped here; a NaN reads the default). itemOk: the item snapshot is coherent (F139::itemCoherent). settled: the held
// (item, scope)'s F144 entry (null for none); its line is used once it has one (line.have), E's forward in metres in the root's axes.
inline EyeDepth eyeDepth(double keyCm, bool itemOk, const char* name, int scopeState, const f144::Entry* settled) noexcept {
    EyeDepth d;
    d.keyCm = adsEyeDistanceClampCm(keyCm);
    d.askedCm = d.keyCm;
    if (!itemOk) return d;
    double cam = 0.0;
    if (settled && settled->line.have && std::isfinite(settled->line.e[2])) {
        cam = 100.0 * settled->line.e[2];
        d.source = kSrcSettled;
    } else if (tableDepthCm(name, scopeState, &cam)) {
        d.source = kSrcTable;
    } else {
        return d;
    }
    d.cameraCm = cam;
    d.askedCm = adsEyeDistanceClampCm(cam < d.keyCm ? cam : d.keyCm);
    return d;
}

}  // namespace f149
}  // namespace nofoot
}  // namespace edvr
