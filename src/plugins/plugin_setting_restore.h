#pragma once
// What an addon's setting reads as when it is registered: the value saved in the ini under
// addon.<stem>.<header>.<key>, else the addon's own default. Kept out of menu.cpp so the rig that drives
// the plugin manager can check it.

#include "../../include/edvr_plugin_api.h"
#include "../common/config.h"

#include <cctype>
#include <cstdint>
#include <cstring>
#include <string>

namespace edvr::plugins {

// The ini key for a setting, as the edit path (stepPluginSetting) writes it. The addon's identity (its DLL file stem, the same
// one that tags its rows and keys its approval in [addons]) is part of the key, so two addons that both register
// Tools / enabled keep separate saved values. A setting registered with no owner is saved under "unowned".
//
// The whole key is lowercased, as Config::parse lowercases every stored name (section + "." + key) and Config::set does: a key the
// SDK lets an addon spell "Enabled" is saved, found and restored as "enabled". Config's getters look a name up exactly, so write and
// restore must both go through here. The addon sees and displays its own spelling; only the persisted name is folded. (Two keys of one
// header that differ only in case therefore share a saved value.)
inline std::string pluginSettingIniKey(const std::string& owner, const std::string& header, const std::string& key) {
    std::string safeHeader = header;
    for (char& c : safeHeader) {
        if (c == ' ' || c == '-' || c == '.') c = '_';
    }
    std::string name = "addon." + (owner.empty() ? std::string("unowned") : owner) + "." + safeHeader + "." + key;
    for (char& c : name) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    return name;
}

inline int64_t restoredPluginSettingValue(const EdvrPluginSettingDef& setting, const std::string& iniKey) {
    int64_t initialVal = setting.defaultValue;
    if (setting.type == EDVR_PLUGIN_SETTING_BOOL) {
        initialVal = Config::get().getBool(iniKey.c_str(), setting.defaultValue != 0) ? 1 : 0;
    } else if (setting.type == EDVR_PLUGIN_SETTING_INT) {
        initialVal = Config::get().getInt(iniKey.c_str(), static_cast<int>(setting.defaultValue));
    } else if (setting.type == EDVR_PLUGIN_SETTING_FLOAT) {
        // A float travels in the low four bytes of the int64, as stepPluginSetting reads and writes it.
        float def = 0.0f;
        memcpy(&def, &setting.defaultValue, sizeof(float));
        const float saved = Config::get().getFloat(iniKey.c_str(), def);
        initialVal = 0;
        memcpy(&initialVal, &saved, sizeof(float));
    } else if (setting.type == EDVR_PLUGIN_SETTING_CHOICE) {
        std::string chosen = Config::get().getString(iniKey.c_str(), "");
        if (!chosen.empty() && setting.choiceOptions && setting.choiceCount > 0) {
            for (uint32_t i = 0; i < setting.choiceCount; ++i) {
                if (setting.choiceOptions[i] && _stricmp(chosen.c_str(), setting.choiceOptions[i]) == 0) {
                    initialVal = i;
                    break;
                }
            }
        }
    }
    return initialVal;
}

} // namespace edvr::plugins
