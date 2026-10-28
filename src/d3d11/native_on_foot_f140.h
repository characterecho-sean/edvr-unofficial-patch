// F140 (native on-foot VR): what the weapon readout's placement shares. Header only, no game or D3D11 state, so native_frame_test runs the very functions the weapon UI decision uses.
// Journal: the arc doc, 2026-10-27, F140.
//
// The weapon readout (the ammunition counter the game draws on the weapon) is placed per weapon, by the exact item name and the scope state the journal's loadout gives
// (journal_loadout.h), in two roles (the hip and the ADS readout). The table of those placements, the role chosen and the trims added to it are native_on_foot_f144.h's (F140 had
// a table of its own, one row per weapon and scope; F144's replaced it and F147 removed it, so there is one table). What is here is what both halves use: the two classes the game's
// readout parts come in (the pistol's and the rifle's) with the built-in values a weapon the table does not hold falls back to, the ranges the four values and the four live trims
// are clamped to, whether an item is a weapon, and whether the item snapshot can be trusted.
//
// The values are forward, left, up and toward eye, in cm, in the weapon frame with the same target() as before (native_on_foot_f90.h). Four live trims,
// experimental.native_on_foot_weapon_ui_forward_cm, _left_cm, _up_cm and _toward_eye_cm (default 0; fix.* until F146, which moved them to [experimental] for adjusting a weapon's
// readout from feedback), are added to the held weapon's values and the totals clamped (F145: -30..30 cm). An item that is not in the table, or a name that cannot be trusted (its
// snapshot was not read, or its serial is not the held item's), takes the class's built-in values (an untrusted name takes no trim).
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

// The totals' ranges and the trims' own. F145: the trims run -30..+30 cm on all four axes (the pilot ran the shotgun's and the laser SMG's forward trim into its +10 stop in ADS),
// and the totals' ranges are the same -30..+30 so that a trim is never silently cut by a narrower total (the retired class keys' ranges were forward -10..30, left -15..15, up
// -10..10, toward eye -5..10).
constexpr double kMinCm[4] = {-30.0, -30.0, -30.0, -30.0};
constexpr double kMaxCm[4] = {30.0, 30.0, 30.0, 30.0};
constexpr double kTrimMinCm[4] = {-30.0, -30.0, -30.0, -30.0};
constexpr double kTrimMaxCm[4] = {30.0, 30.0, 30.0, 30.0};
inline double clampTotal(int i, double cm) noexcept { return f88::clampCm(cm, kMinCm[i], kMaxCm[i]); }
inline double clampTrim(int i, double cm) noexcept { return f88::clampCm(cm, kTrimMinCm[i], kTrimMaxCm[i]); }

inline bool isWeapon(const char* name) noexcept { return name && std::strncmp(name, "$wpn_", 5) == 0; }
inline bool itemCoherent(bool readOk, uint32_t snapshotSerial, uint32_t currentSerial, const char* name) noexcept {
    return readOk && snapshotSerial == currentSerial && name && name[0];
}

}  // namespace f140
}  // namespace nofoot
}  // namespace edvr
