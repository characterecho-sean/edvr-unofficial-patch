// F140 checks (native on-foot VR): the journal's loadout that says which scope is fitted, and what the weapon readout's placement shares.
// Header only, run by native_frame_test (run140). Two halves: the journal parser (journal_loadout.h: the name mapping, the three events and the removal, the unknown and
// ambiguous cases, the chunked reading) and what the placement shares (native_on_foot_f140.h: the class built-ins, the ranges, a weapon, a trusted item; F147: the table is
// F144's alone, so its rows are checked here for the nine weapons F140 held, and the rocket launcher's row while the pistol part is drawn).
#pragma once

#include "../../src/d3d11/journal_loadout.h"
#include "../../src/d3d11/native_on_foot_f140.h"
#include "../../src/d3d11/native_on_foot_f144.h"   // F147: the one table (F140's own was removed)
#include "f90_checks.h"   // the F85 two-eye rig and the part records, for the placement through f90::decide

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>

namespace f140test {

namespace ls = edvr::loadout;
namespace f140 = edvr::nofoot::f140;
namespace f90 = edvr::nofoot::f90;

// Lines of the pilot's own journal (2026-10-27, the flight edvr_gfx_20261027_130251), word for word.
constexpr const char* kSwitch = R"J({ "timestamp":"2026-10-27T19:05:24Z", "event":"SwitchSuitLoadout", "SuitID":1700603230961705, "SuitName":"tacticalsuit_class3", "SuitName_Localised":"$TacticalSuit_Class1_Name;", "SuitMods":[ "suit_improvedjumpassist" ], "LoadoutID":4293000002, "LoadoutName":"Combat", "Modules":[ { "SlotName":"PrimaryWeapon1", "SuitModuleID":1700600873492864, "ModuleName":"wpn_m_assaultrifle_kinetic_fauto", "ModuleName_Localised":"Karma AR-50", "Class":5, "WeaponMods":[ "weapon_handling" ] }, { "SlotName":"PrimaryWeapon2", "SuitModuleID":1700652859875426, "ModuleName":"wpn_m_assaultrifle_laser_fauto", "ModuleName_Localised":"TK Aphelion", "Class":5, "WeaponMods":[ "weapon_stability" ] }, { "SlotName":"SecondaryWeapon", "SuitModuleID":1700597869676361, "ModuleName":"wpn_s_pistol_laser_sauto", "ModuleName_Localised":"TK Zenith", "Class":3, "WeaponMods":[  ] } ] })J";
constexpr const char* kEquipPlasma = R"J({ "timestamp":"2026-10-27T19:05:53Z", "event":"LoadoutEquipModule", "LoadoutName":"Combat", "SuitID":1700603230961705, "SuitName":"tacticalsuit_class3", "SuitName_Localised":"$TacticalSuit_Class1_Name;", "LoadoutID":4293000002, "SlotName":"PrimaryWeapon1", "ModuleName":"wpn_m_assaultrifle_plasma_fauto", "ModuleName_Localised":"Manticore Oppressor", "Class":5, "WeaponMods":[ "weapon_stability", "weapon_handling" ], "SuitModuleID":1700228271976649 })J";
constexpr const char* kEquipSmg = R"J({ "timestamp":"2026-10-27T19:05:59Z", "event":"LoadoutEquipModule", "LoadoutName":"Combat", "SuitID":1700603230961705, "SuitName":"tacticalsuit_class3", "SuitName_Localised":"$TacticalSuit_Class1_Name;", "LoadoutID":4293000002, "SlotName":"PrimaryWeapon2", "ModuleName":"wpn_m_submachinegun_kinetic_fauto", "ModuleName_Localised":"Karma C-44", "Class":4, "WeaponMods":[  ], "SuitModuleID":1723790001509894 })J";
constexpr const char* kSuit = R"J({ "timestamp":"2026-10-27T19:06:18Z", "event":"SuitLoadout", "SuitID":1700603230961705, "SuitName":"tacticalsuit_class3", "SuitName_Localised":"$TacticalSuit_Class1_Name;", "SuitMods":[ "suit_improvedjumpassist" ], "LoadoutID":4293000002, "LoadoutName":"Combat", "Modules":[ { "SlotName":"PrimaryWeapon1", "SuitModuleID":1700228271976649, "ModuleName":"wpn_m_assaultrifle_plasma_fauto", "ModuleName_Localised":"Manticore Oppressor", "Class":5, "WeaponMods":[ "weapon_stability", "weapon_handling" ] }, { "SlotName":"PrimaryWeapon2", "SuitModuleID":1700901316509316, "ModuleName":"wpn_m_submachinegun_laser_fauto", "ModuleName_Localised":"TK Eclipse", "Class":2, "WeaponMods":[ "weapon_suppression_unpressurised" ] }, { "SlotName":"SecondaryWeapon", "SuitModuleID":1700597869676361, "ModuleName":"wpn_s_pistol_laser_sauto", "ModuleName_Localised":"TK Zenith", "Class":3, "WeaponMods":[  ] } ] })J";
// The sniper with weapon_scope (2026-10-27T18:25:22Z), PrimaryWeapon2 empty.
constexpr const char* kSniper = R"J({ "timestamp":"2026-10-27T18:25:22Z", "event":"SuitLoadout", "SuitID":1700597851712697, "SuitName":"utilitysuit_class5", "SuitName_Localised":"$UtilitySuit_Class1_Name;", "SuitMods":[ "suit_nightvision", "suit_improvedjumpassist", "suit_increasedsprintduration", "suit_improvedradar" ], "LoadoutID":4293000007, "LoadoutName":"Nightvision", "Modules":[ { "SlotName":"PrimaryWeapon1", "SuitModuleID":1700228265276809, "ModuleName":"wpn_m_sniper_plasma_charged", "ModuleName_Localised":"Manticore Executioner", "Class":5, "WeaponMods":[ "weapon_suppression_pressurised", "weapon_suppression_unpressurised", "weapon_range", "weapon_scope" ] }, { "SlotName":"SecondaryWeapon", "SuitModuleID":1700228275508053, "ModuleName":"wpn_s_pistol_plasma_charged", "ModuleName_Localised":"Manticore Tormentor", "Class":5, "WeaponMods":[ "weapon_suppression_pressurised", "weapon_accuracy" ] } ] })J";
constexpr const char* kNoise = R"J({ "timestamp":"2026-10-27T19:05:30Z", "event":"FSSSignalDiscovered", "SystemAddress":3932277478106, "SignalName":"SuitLoadout", "SignalType":"Generic" })J";

inline std::string equipLine(const char* slot, const char* module, const char* mods, unsigned long long loadoutId = 4293000002ull) {
    char b[700];
    std::snprintf(b, sizeof(b),
                  "{ \"timestamp\":\"2026-10-27T19:10:00Z\", \"event\":\"LoadoutEquipModule\", \"LoadoutName\":\"Combat\", \"SuitID\":1, \"LoadoutID\":%llu, \"SlotName\":\"%s\", "
                  "\"ModuleName\":\"%s\", \"Class\":5, \"WeaponMods\":[ %s ], \"SuitModuleID\":2 }",
                  loadoutId, slot, module, mods);
    return b;
}
inline std::string removeLine(const char* slot, const char* module, unsigned long long loadoutId = 4293000002ull) {
    char b[600];
    std::snprintf(b, sizeof(b),
                  "{ \"timestamp\":\"2026-10-27T19:10:30Z\", \"event\":\"LoadoutRemoveModule\", \"LoadoutName\":\"Combat\", \"SuitID\":1, \"LoadoutID\":%llu, \"SlotName\":\"%s\", "
                  "\"ModuleName\":\"%s\", \"SuitModuleID\":2 }",
                  loadoutId, slot, module);
    return b;
}
inline ls::LineResult feedLine(ls::Loadout& l, const std::string& s) { return ls::applyLine(l, s.data(), s.size()); }

constexpr const char* kPlasmaAr = "$wpn_m_assaultrifle_plasma_fauto_name;";
constexpr const char* kLaserRifle = "$wpn_m_assaultrifle_laser_fauto_name;";
constexpr const char* kKineticSmg = "$wpn_m_submachinegun_kinetic_fauto_name;";
constexpr const char* kLaserSmg = "$wpn_m_submachinegun_laser_fauto_name;";
constexpr const char* kRocket = "$wpn_m_launcher_rocket_sauto_name;";
constexpr const char* kSniper2 = "$wpn_m_sniper_plasma_charged_name;";
constexpr const char* kPistolLaser = "$wpn_s_pistol_laser_sauto_name;";
constexpr const char* kPistolKinetic = "$wpn_s_pistol_kinetic_sauto_name;";

template <class Check>
inline void runLoadout(Check&& check) {
    {   // the name mapping
        check(ls::statusNameMatches("$wpn_m_sniper_plasma_charged_name;", "wpn_m_sniper_plasma_charged"), "F140: Status.json's $wpn_m_sniper_plasma_charged_name; is the journal's wpn_m_sniper_plasma_charged");
        check(ls::statusNameMatches("$WPN_M_Sniper_Plasma_CHARGED_name;", "wpn_m_sniper_plasma_charged") && ls::statusNameMatches("$wpn_m_sniper_plasma_charged_NAME;", "WPN_M_SNIPER_plasma_charged"),
              "F140: the comparison is without case, on either side");
        check(!ls::statusNameMatches("wpn_m_sniper_plasma_charged_name;", "wpn_m_sniper_plasma_charged") && !ls::statusNameMatches("$wpn_m_sniper_plasma_charged", "wpn_m_sniper_plasma_charged") &&
                  !ls::statusNameMatches("$wpn_m_sniper_plasma_charged_name", "wpn_m_sniper_plasma_charged") && !ls::statusNameMatches("$wpn_m_sniper_plasma_charged_name;", "wpn_m_sniper_plasma") &&
                  !ls::statusNameMatches("$wpn_m_sniper_plasma_name;", "wpn_m_sniper_plasma_charged") && !ls::statusNameMatches("$wpn_m_sniper_plasma_charged_name;", "") &&
                  !ls::statusNameMatches(nullptr, "x") && !ls::statusNameMatches("$_name;", "") && !ls::statusNameMatches("$wpn_m_sniper_plasma_charged_name;", nullptr),
              "F140: the $ and the _name; are both needed, and nothing else may differ (no prefix, no suffix, no empty name)");
        check(!ls::statusNameMatches("$humanoid_fists_name;", "wpn_m_sniper_plasma_charged"), "F140: the fists match no weapon");
    }
    {   // SwitchSuitLoadout carries all three slots
        ls::Loadout l;
        const ls::LineResult r = feedLine(l, kSwitch);
        check(r.event == ls::kEvSwitch && r.changed && l.known && l.haveId && l.id == 4293000002ull && std::strcmp(l.name, "Combat") == 0 && l.generation == 1, "F140: SwitchSuitLoadout: the loadout, its id and name, generation 1");
        check(l.slot[0].have && std::strcmp(l.slot[0].module, "wpn_m_assaultrifle_kinetic_fauto") == 0 && !l.slot[0].scope && std::strcmp(l.slot[0].mods, "weapon_handling") == 0 &&
                  l.slot[1].have && std::strcmp(l.slot[1].module, "wpn_m_assaultrifle_laser_fauto") == 0 && std::strcmp(l.slot[1].mods, "weapon_stability") == 0 &&
                  l.slot[2].have && std::strcmp(l.slot[2].module, "wpn_s_pistol_laser_sauto") == 0 && l.slot[2].mods[0] == '\0' && !l.slot[2].scope,
              "F140: all three slots, by name, mods and scope (the 2026-10-27 19:05:24 line)");
        const ls::HeldScope laser = ls::scopeOfHeld(l, kLaserRifle), pistol = ls::scopeOfHeld(l, kPistolLaser);
        check(laser.state == ls::kScopeNo && laser.why == ls::kWhyOk && laser.slots == 1 && pistol.state == ls::kScopeNo, "F140: the held weapon's name finds its slot: scope no");
        const ls::LineResult again = feedLine(l, kSwitch);
        check(again.event == ls::kEvSwitch && !again.changed && l.generation == 1, "F140: the same loadout again changes nothing (no new generation, no log line)");
        // F147: the log line names the loadout by its id and the weapons with their scope state; the commander's name for it ("Combat") is parsed (a rename is a change) but not written
        char text[640];
        ls::describe(l, text, sizeof(text));
        check(std::strstr(text, "SwitchSuitLoadout, loadout id 4293000002: PrimaryWeapon1 wpn_m_assaultrifle_kinetic_fauto scope no [weapon_handling]; PrimaryWeapon2 wpn_m_assaultrifle_laser_fauto scope no "
                                "[weapon_stability]; SecondaryWeapon wpn_s_pistol_laser_sauto scope no []") == text && !std::strstr(text, "Combat") && !std::strstr(text, "\""),
              "F147: the loadout line carries the id, the weapons and their scope state, and not the commander's name for the loadout");
        std::string renamedLine = kSwitch;
        renamedLine.replace(renamedLine.find("Combat"), 6, "Renamed");
        const ls::LineResult renamed = feedLine(l, renamedLine.c_str());
        check(renamed.event == ls::kEvSwitch && renamed.changed && l.generation == 2 && std::strcmp(l.name, "Renamed") == 0, "F147: ... a loadout the commander renamed is still read as a change (the name is parsed, not written)");
    }
    {   // LoadoutEquipModule: one slot, the other two stand; then SuitLoadout replaces all three
        ls::Loadout l;
        feedLine(l, kSwitch);
        const ls::LineResult r = feedLine(l, kEquipPlasma);
        check(r.event == ls::kEvEquip && r.changed && l.generation == 2 && std::strcmp(l.slot[0].module, "wpn_m_assaultrifle_plasma_fauto") == 0 && !l.slot[0].scope &&
                  std::strcmp(l.slot[0].mods, "weapon_stability,weapon_handling") == 0 && std::strcmp(l.slot[1].module, "wpn_m_assaultrifle_laser_fauto") == 0,
              "F140: LoadoutEquipModule changes its own slot (the plasma assault rifle, 19:05:53: stability and handling, no scope) and leaves the others");
        check(ls::scopeOfHeld(l, kPlasmaAr).state == ls::kScopeNo && ls::scopeOfHeld(l, kPlasmaAr).why == ls::kWhyOk, "F140: the plasma assault rifle: scope no");
        const ls::HeldScope gone = ls::scopeOfHeld(l, "$wpn_m_assaultrifle_kinetic_fauto_name;");
        check(gone.state == ls::kScopeUnknown && gone.why == ls::kWhyNotInSlots && gone.slots == 0, "F140: the weapon it replaced is in no slot now: unknown, 'the name is in no slot'");
        feedLine(l, kEquipSmg);
        feedLine(l, equipLine("PrimaryWeapon2", "wpn_m_submachinegun_laser_fauto", "\"weapon_suppression_unpressurised\""));
        check(l.generation == 4 && std::strcmp(l.slot[1].module, "wpn_m_submachinegun_laser_fauto") == 0, "F140: three equips, three generations");
        const ls::LineResult suit = feedLine(l, kSuit);
        check(suit.event == ls::kEvSuitLoadout && !suit.changed && l.generation == 4,
              "F140: the SuitLoadout the game writes after those equips (19:06:18) is the loadout already held: unchanged");
        ls::Loadout fromSuit;
        feedLine(fromSuit, kSuit);
        check(ls::scopeOfHeld(fromSuit, kLaserSmg).state == ls::kScopeNo && ls::scopeOfHeld(fromSuit, kPlasmaAr).state == ls::kScopeNo && sameSlots(l, fromSuit),
              "F140: and starting from that SuitLoadout alone gives the same three slots");
    }
    {   // the scope mod
        ls::Loadout l;
        const ls::LineResult r = feedLine(l, kSniper);
        check(r.changed && l.slot[0].scope && std::strstr(l.slot[0].mods, "weapon_scope") != nullptr && !l.slot[1].have && l.slot[2].have && !l.slot[2].scope,
              "F140: the sniper's weapon_scope is read from its WeaponMods; PrimaryWeapon2 is empty; the pistol has none");
        check(ls::scopeOfHeld(l, kSniper2).state == ls::kScopeYes && ls::scopeOfHeld(l, "$wpn_s_pistol_plasma_charged_name;").state == ls::kScopeNo, "F140: the sniper's held scope state is yes, the pistol's no");
        feedLine(l, equipLine("PrimaryWeapon2", "wpn_m_launcher_rocket_sauto", "\"weapon_scope\", \"weapon_range\"", 4293000007ull));
        check(ls::scopeOfHeld(l, kRocket).state == ls::kScopeYes && l.slot[1].scope, "F140: a LoadoutEquipModule with weapon_scope fits a scope in its slot");
        feedLine(l, equipLine("PrimaryWeapon2", "wpn_m_launcher_rocket_sauto", "\"weapon_range\"", 4293000007ull));
        check(ls::scopeOfHeld(l, kRocket).state == ls::kScopeNo, "F140: the same module equipped again without the scope mod: scope no");
        check(l.slot[0].scope && ls::scopeOfHeld(l, kSniper2).state == ls::kScopeYes, "F140: the sniper's slot was not touched by the other slot's equips");
        const std::string noise = std::string("{ \"timestamp\":\"x\", \"event\":\"Music\", \"MusicTrack\":\"weapon_scope\" }");
        check(!feedLine(l, noise).changed && feedLine(l, noise).event == ls::kEvNone, "F140: a line of another event is ignored");
        check(feedLine(l, kNoise).event == ls::kEvNone && !feedLine(l, kNoise).changed, "F140: a line whose string value reads like a loadout event is not one");
        const ls::LineResult tricky = feedLine(l, "{ \"timestamp\":\"x\", \"Note\":\"event\", \"event\":\"LoadoutEquipModule\", \"LoadoutID\":4293000007, \"SlotName\":\"SecondaryWeapon\", "
                                               "\"ModuleName\":\"wpn_s_pistol_x\", \"WeaponMods\":[ ] }");
        check(tricky.event == ls::kEvEquip && tricky.changed && std::strcmp(l.slot[2].module, "wpn_s_pistol_x") == 0,
              "F140: the key `event` is the one with a colon after it: a string value that reads \"event\" before it does not hide it");
    }
    {   // LoadoutRemoveModule, if it is ever written, empties its slot; a stale or other-loadout event does not
        ls::Loadout l;
        feedLine(l, kSwitch);
        check(!feedLine(l, removeLine("PrimaryWeapon1", "wpn_s_pistol_kinetic_sauto")).changed && l.slot[0].have, "F140: a removal naming another module than the slot's is stale: ignored");
        const ls::LineResult r = feedLine(l, removeLine("PrimaryWeapon1", "wpn_m_assaultrifle_kinetic_fauto"));
        check(r.event == ls::kEvRemove && r.changed && !l.slot[0].have && l.slot[0].module[0] == '\0' && l.generation == 2, "F140: LoadoutRemoveModule empties the slot");
        check(ls::scopeOfHeld(l, "$wpn_m_assaultrifle_kinetic_fauto_name;").state == ls::kScopeUnknown && ls::scopeOfHeld(l, "$wpn_m_assaultrifle_kinetic_fauto_name;").why == ls::kWhyNotInSlots,
              "F140: the removed weapon is in no slot: unknown");
        const ls::LineResult other = feedLine(l, equipLine("PrimaryWeapon1", "wpn_m_launcher_rocket_sauto", "", 4293000009ull));
        check(other.event == ls::kEvEquip && other.otherLoadout && !other.changed && !l.slot[0].have, "F140: an equip into another loadout (another LoadoutID) is not the current one: ignored");
        check(!feedLine(l, removeLine("PrimaryWeapon9", "x")).changed && !feedLine(l, equipLine("Tool1", "wpn_m_x", "")).changed, "F140: a slot that is not a weapon slot is ignored");
    }
    {   // the unknown cases
        ls::Loadout none;
        check(ls::scopeOfHeld(none, kPlasmaAr).state == ls::kScopeUnknown && ls::scopeOfHeld(none, kPlasmaAr).why == ls::kWhyNoLoadout, "F140: no loadout read yet: unknown");
        ls::Loadout l;
        feedLine(l, kSuit);
        check(ls::scopeOfHeld(l, kRocket).state == ls::kScopeUnknown && ls::scopeOfHeld(l, kRocket).why == ls::kWhyNotInSlots, "F140: a name in no slot: unknown");
        check(ls::scopeOfHeld(l, "").why == ls::kWhyNoName && ls::scopeOfHeld(l, nullptr).state == ls::kScopeUnknown && ls::scopeOfHeld(l, "$humanoid_fists_name;").state == ls::kScopeUnknown,
              "F140: no name, the fists: unknown");
        // two slots holding the same module: different mods are ambiguous, equal scope state is not
        ls::Loadout amb;
        feedLine(amb, equipLine("PrimaryWeapon1", "wpn_m_assaultrifle_plasma_fauto", "\"weapon_scope\""));
        feedLine(amb, equipLine("PrimaryWeapon2", "wpn_m_assaultrifle_plasma_fauto", "\"weapon_stability\""));
        const ls::HeldScope a = ls::scopeOfHeld(amb, kPlasmaAr);
        check(a.state == ls::kScopeUnknown && a.why == ls::kWhyAmbiguous && a.slots == 2, "F140: two slots hold the plasma rifle, one scoped: unknown, ambiguous");
        feedLine(amb, equipLine("PrimaryWeapon2", "wpn_m_assaultrifle_plasma_fauto", "\"weapon_scope\", \"weapon_stability\""));
        const ls::HeldScope same = ls::scopeOfHeld(amb, kPlasmaAr);
        check(same.state == ls::kScopeYes && same.why == ls::kWhyOk && same.slots == 2, "F140: both slots scoped: yes (the two agree)");
        feedLine(amb, equipLine("PrimaryWeapon1", "wpn_m_assaultrifle_plasma_fauto", "\"weapon_handling\""));
        feedLine(amb, equipLine("PrimaryWeapon2", "wpn_m_assaultrifle_plasma_fauto", "\"weapon_stability\""));
        check(ls::scopeOfHeld(amb, kPlasmaAr).state == ls::kScopeNo && ls::scopeOfHeld(amb, kPlasmaAr).slots == 2, "F140: both slots unscoped: no");
    }
    {   // reading in chunks: a line may be split anywhere, the result is the whole file's
        const std::string file = std::string(kNoise) + "\n" + kSwitch + "\r\n" + kNoise + "\n" + kEquipPlasma + "\n" + kEquipSmg + "\n" + kSuit + "\n" + kSniper + "\n";
        ls::Loadout whole;
        {
            ls::LineFeeder f;
            f.feed(file.data(), file.size(), [&](const char* line, size_t n) { ls::applyLine(whole, line, n); });
        }
        check(whole.known && std::strcmp(whole.name, "Nightvision") == 0 && whole.slot[0].scope && !whole.slot[1].have, "F140: the whole file read at once ends on its last SuitLoadout (the sniper's)");
        bool allSame = true;
        for (size_t chunk : {1u, 2u, 7u, 64u, 333u, 1000u, 4096u}) {
            ls::Loadout l;
            ls::LineFeeder f;
            for (size_t at = 0; at < file.size(); at += chunk) f.feed(file.data() + at, std::min(chunk, file.size() - at), [&](const char* line, size_t n) { ls::applyLine(l, line, n); });
            allSame = allSame && l.generation == whole.generation && sameSlots(l, whole) && std::strcmp(l.name, whole.name) == 0;
        }
        check(allSame, "F140: the same loadout and the same generations whatever the chunk size (1 byte to 4 KB)");
        // an unfinished last line is not applied until its newline arrives
        ls::Loadout partial;
        ls::LineFeeder f;
        const std::string line = kSwitch;
        f.feed(line.data(), line.size(), [&](const char* l, size_t n) { ls::applyLine(partial, l, n); });
        check(!partial.known, "F140: a line the game is still writing (no newline yet) is not applied");
        f.feed("\n", 1, [&](const char* l, size_t n) { ls::applyLine(partial, l, n); });
        check(partial.known, "F140: and is applied when its newline comes");
        // a line longer than the cap is dropped and the next one is read
        std::string big(ls::kMaxLine + 100, 'x');
        ls::Loadout l2;
        ls::LineFeeder g;
        const std::string tail = std::string(kSwitch) + "\n";
        g.feed(big.data(), big.size(), [&](const char* l, size_t n) { ls::applyLine(l2, l, n); });
        g.feed("\n", 1, [&](const char* l, size_t n) { ls::applyLine(l2, l, n); });
        g.feed(tail.data(), tail.size(), [&](const char* l, size_t n) { ls::applyLine(l2, l, n); });
        check(l2.known && l2.generation == 1, "F140: a line past the 16 KB cap is dropped to its newline and the next line is read");
    }
    {   // the publication
        ls::Loadout l, out;
        feedLine(l, kSwitch);
        ls::Publication pub;
        check(!pub.read(&out), "F140: nothing published yet: not read");
        pub.publish(l);
        check(pub.read(&out) && out.generation == l.generation && sameSlots(out, l), "F140: a published loadout is read whole");
        ls::g_published.publish(l);
        uint32_t gen = 0;
        const ls::HeldScope h = ls::heldScope(kLaserRifle, &gen);
        check(h.state == ls::kScopeNo && gen == l.generation && ls::publishedGeneration() == l.generation, "F140: heldScope reads the published loadout and its generation");
    }
}

template <class Check>
inline void runPlacement(Check&& check) {
    namespace f144 = edvr::nofoot::f144;
    const double noTrim[4] = {0, 0, 0, 0};
    const double* pistol = f140::kPistolClass;
    const double* rifle = f140::kRifleClass;
    check(pistol[0] == 13.0 && pistol[1] == 3.5 && pistol[2] == -3.0 && pistol[3] == 3.0 && rifle[0] == 0.0 && rifle[1] == 3.5 && rifle[2] == 0.0 && rifle[3] == 0.0,
          "F140: the class built-ins are the retired keys': pistol 13, 3.5, -3, 3 and rifle 0, 3.5, 0, 0 cm");
    {   // F147: one table. F140 had its own (a row per weapon and scope), dead since F144's replaced it; the nine weapons it held are F144's rows now, under both roles (F151: one row per role, whatever the scope)
        const char* const nine[9] = {kPistolKinetic, kPistolLaser, "$wpn_s_pistol_plasma_charged_name;", kLaserRifle, kPlasmaAr, kRocket, kSniper2, kKineticSmg, kLaserSmg};
        bool ok = true;
        for (const char* w : nine)
            for (int role : {f144::kHip, f144::kAds}) ok = ok && f144::find(w, role);
        check(ok, "F147: each of the nine weapons F140 listed has a row in the one table (F144's) under each role");
        check(!f144::find("$wpn_m_unknown_name;", f144::kAds) && !f144::find(nullptr, f144::kAds) && !f144::find("$wpn_s_pistol_laser_sauto_name", f144::kAds),
              "F147: an item not in the table is a miss (the key is the exact name)");
    }
    {   // the ranges (F145): the trim to its own range, -30..30 cm on all four axes, the total to the same
        check(f140::clampTrim(0, 10.5) == 10.5 && f140::clampTrim(3, -12.0) == -12.0 && f140::clampTrim(1, 31.0) == 30.0 && f140::clampTrim(2, -31.0) == -30.0,
              "F145: a trim past the old stop (10 cm; toward you 5) is kept up to 30");
        check(f140::clampTotal(0, 35.0) == 30.0 && f140::clampTotal(0, -20.0) == -20.0 && f140::clampTotal(1, 20.0) == 20.0 && f140::clampTotal(3, 12.0) == 12.0 && f140::clampTotal(3, -9.0) == -9.0 &&
                  f140::clampTotal(2, -40.0) == -30.0 && f140::clampTotal(2, std::nan("")) == 0.0,
              "F145: the totals' ranges are -30..30 on all four axes (a value that is not a number reads 0)");
        check(f140::clampTrim(0, std::nan("")) == 0.0 && f140::clampTrim(1, INFINITY) == 0.0, "F140: a trim that is not a number is 0");
    }
    {   // a weapon, and an item snapshot that can be trusted
        check(f140::isWeapon(kRocket) && f140::isWeapon(kPistolLaser) && !f140::isWeapon("$humanoid_companalyser_name;") && !f140::isWeapon(nullptr) && !f140::isWeapon(""), "F140: only a $wpn_ item is a weapon (a tool takes no trim)");
        check(f140::itemCoherent(true, 5, 5, kRocket) && !f140::itemCoherent(false, 5, 5, kRocket) && !f140::itemCoherent(true, 5, 6, kRocket) && !f140::itemCoherent(true, 5, 5, "") &&
                  !f140::itemCoherent(true, 5, 5, nullptr),
              "F140: an item that was not read, a snapshot of another serial (the item changed between the reads) or no name is not trusted");
    }
    {   // the placement is F144's: an untrusted name takes the class's values and no trim
        const double trim[4] = {2.0, 2.0, 2.0, 2.0};
        const f144::RolePlacement unread = f144::place(false, 5, 5, kRocket, ls::kScopeNo, f140::kClassPistol, f144::kHip, true, trim);
        const f144::RolePlacement stale = f144::place(true, 5, 6, kRocket, ls::kScopeNo, f140::kClassPistol, f144::kHip, true, trim);
        const f144::RolePlacement nullName = f144::place(true, 5, 5, nullptr, ls::kScopeNo, f140::kClassRifle, f144::kAds, true, trim);
        check(!unread.coherent && !unread.table && std::memcmp(unread.total, pistol, sizeof(unread.total)) == 0 && unread.trim[0] == 0.0, "F140: an item that was not read: the class's values, no trim");
        check(!stale.coherent && !stale.table && std::memcmp(stale.total, pistol, sizeof(stale.total)) == 0 && stale.trim[1] == 0.0,
              "F140: a snapshot of another serial (the item changed between the reads): the class's values, no trim, not the rocket launcher's row");
        check(!nullName.coherent && std::memcmp(nullName.total, rifle, sizeof(nullName.total)) == 0, "F140: no name: the class's values");
    }
    {   // an item's own keys are used while it draws the other class's part: through f90::decide on the F85 rig
        namespace t90 = f90test;
        const t90::Rig g = t90::makeRig();
        const t90::Record hip{t90::kN89Hip, {0.10, -0.12, 0.36}, 0.07207, {2.0, 1.0}};
        const t90::Weapon wp = t90::weaponOf(g, 7.0);
        const auto rot = edvr::drawtag::rotateByPackedQuat;
        const auto frameFn = [&] { return f90::weaponFrame(true, wp.attach, true, wp.froot); };
        const f144::RolePlacement rocket = f144::place(true, 5, 5, kRocket, ls::kScopeNo, f140::kClassPistol, f144::kHip, false, noTrim);
        const f144::RolePlacement pistolItem = f144::place(true, 5, 5, kPistolLaser, ls::kScopeNo, f140::kClassPistol, f144::kHip, false, noTrim);
        // the game is drawing the pistol part (a switch in flight: Status.json already names the rocket launcher, a primary), within the grace
        f90::In in = t90::inOf(g, 0, 300, hip, f90::kItemPrimary, 7, rocket.keys());
        in.drawnClass = f90::kDrawnPistol;
        in.drawnClassAgeS = 0.5;
        in.rifleKeys = f144::place(true, 5, 5, kRocket, ls::kScopeNo, f140::kClassRifle, f144::kAds, false, noTrim).keys();
        f90::Latch latch;
        const f90::Out o = f90::decide(in, t90::uOf(g, 0), latch, rot, frameFn);
        f90::In inP = t90::inOf(g, 0, 300, hip, f90::kItemPistol, 7, pistolItem.keys());
        f90::Latch latchP;
        const f90::Out op = f90::decide(inP, t90::uOf(g, 0), latchP, rot, frameFn);
        double wantT[3];
        const bool targeted = f90::target(o.C, o.wf, rocket.keys(), in.eyeOk ? in.eye : nullptr, wantT);
        double pistolT[3];
        f90::target(op.C, op.wf, pistolItem.keys(), inP.eyeOk ? inP.eye : nullptr, pistolT);
        check(o.b == f90::kBMoved && targeted && t90::dist3(o.T, wantT) < 1e-12, "F140: the rocket launcher drawing the pistol's part: placed at its own row's target (5.5 forward at the hip)");
        check(op.b == f90::kBMoved && t90::dist3(op.T, pistolT) < 1e-12 && t90::dist3(o.T, op.T) > 1e-3,
              "F140: and not at the pistol's (13, 3.5, -3, 3): the same part, the same frame, another place");
        // the same part drawn by a pistol that is not in the table takes the pistol class's values
        const f144::RolePlacement newPistol = f144::place(true, 5, 5, "$wpn_s_pistol_new_sauto_name;", ls::kScopeNo, f140::kClassPistol, f144::kHip, false, noTrim);
        f90::In inN = t90::inOf(g, 0, 300, hip, f90::kItemPistol, 7, newPistol.keys());
        f90::Latch latchN;
        const f90::Out on = f90::decide(inN, t90::uOf(g, 0), latchN, rot, frameFn);
        check(on.b == f90::kBMoved && t90::dist3(on.T, op.T) < 1e-12, "F140: an unlisted pistol's hip readout lands where the pistol class put every pistol's before");
    }
}

template <class Check>
inline void run140(Check&& check) {
    runLoadout(check);
    runPlacement(check);
}

}  // namespace f140test
