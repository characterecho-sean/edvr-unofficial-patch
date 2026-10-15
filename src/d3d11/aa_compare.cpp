#include "aa_compare.h"

#include <cstdio>
#include <string>

#include "../common/aa_compare.h"
#include "../common/config.h"
#include "../common/hotkey.h"
#include "../common/log.h"
#include "../common/timing.h"
#include "elite_binds.h"
#include "menu.h"

namespace edvr {
namespace {

constexpr const char* kKey = "fix.temporal_aa";

aacmp::Machine g_machine;
Hotkey         g_key;
std::string    g_keyText;           // the value last applied; "" is unbound
uint32_t       g_generation = ~0u;  // Config::generation() the binding was last read under
bool           g_keyApplied = false;
unsigned       g_missedNotes = 0;
constexpr unsigned kMissedNotesMax = 3;

// The player's own bindings, once per bind: a key Elite also acts on does both on one press. Same rule as Explorer Cam's check.
void checkAgainstEliteBindings(const std::string& text) {
    uint32_t mods = 0;
    const int vk = virtualKeyFromName(text.c_str(), &mods);
    if (!vk) return;   // a pad or joystick value, or a name the hotkey line already said it does not know
    static EliteKeyboardUse uses[512];
    char file[64] = {};
    const int n = eliteBindsKeyboardUses(uses, 512, file, sizeof(file));
    if (n < 0) {
        Log::get().note("hotkey: AA comparison key %s could not be checked against your Elite bindings (no preset or no readable "
                        ".binds file under Options\\Bindings).", text.c_str());
        return;
    }
    std::string clashes;
    int clashCount = 0;
    for (int i = 0; i < n; ++i) {
        uint32_t m2 = 0;
        const int vk2 = virtualKeyFromName(uses[i].binding, &m2);
        if (!aacmp::keysClash(vk, mods, vk2, m2)) continue;
        if (clashCount++ < 4) {
            if (!clashes.empty()) clashes += ", ";
            clashes += uses[i].element;
            clashes += " (";
            clashes += uses[i].binding;
            clashes += ")";
        }
    }
    if (clashCount) {
        Log::get().note("WARNING hotkey: AA comparison key %s CLASHES with your Elite bindings in %s: %s%s. The game acts on that key "
                        "too, so one press does both; rebind one of them (hotkey.compare_aa here, or the action in Elite).",
                        text.c_str(), file, clashes.c_str(), clashCount > 4 ? ", ..." : "");
    } else {
        Log::get().note("hotkey: AA comparison key %s is free in your Elite bindings (%s: %d keyboard bindings checked).",
                        text.c_str(), file, n);
    }
}

// Re-reads the key when the config moved. Binding it is the whole of "live": the F8 Hotkeys page and a hand edit both land here.
void rebindIfChanged(Config& cfg) {
    const uint32_t gen = cfg.generation();
    if (g_keyApplied && gen == g_generation) return;
    g_generation = gen;
    const std::string value = cfg.getString("hotkey.compare_aa", aacmp::kDefaultKey);
    if (g_keyApplied && value == g_keyText) return;
    const bool first = !g_keyApplied;
    g_keyApplied = true;
    g_keyText = value;
    g_key.setBinding(value.c_str());
    if (g_key.bound()) {
        char text[64] = "";
        hotkeyFormatBinding(g_key.binding(), text, sizeof(text));
        Log::get().note("hotkey: AA comparison key %s: %s (vk 0x%02X, mods 0x%X). It turns EDVR's temporal AA off and back on for "
                        "a side-by-side look, at runtime only; no ini is written.",
                        first ? "bound" : "changed", text, g_key.key(), g_key.mods());
        checkAgainstEliteBindings(text);
    } else if (!value.empty()) {
        Log::get().note("hotkey: hotkey.compare_aa = \"%s\" bound nothing (the line above says why), so the AA comparison key is "
                        "off this session.", value.c_str());
    } else {
        Log::get().note("hotkey: AA comparison key %s (hotkey.compare_aa is empty).", first ? "off" : "cleared");
    }
}

// The change is made; ask for the configure sweep now rather than at the poll's cadence, and keep the menu from toasting it as an
// outside edit.
void applied() {
    menuNoteOwnChange(kKey);
    menuRequestConfigPoll();
}

void onPress(Config& cfg) {
    const std::string configured = cfg.configuredTemporalMode();
    const aacmp::Press p = g_machine.press(configured);
    switch (p) {
        case aacmp::Press::WentOff:
            cfg.setRuntimeOverride(kKey, "off");
            applied();
            Log::get().note("aa compare: AA OFF for comparison (configured %s). Runtime only, no ini written; %s restores it. The "
                            "configure sweep that follows is the same one choosing off in the F8 menu runs, so fix.ui_quality "
                            "follows it (live only with AA on).",
                            configured.c_str(), g_keyText.c_str());
            break;
        case aacmp::Press::Restored:
            cfg.clearRuntimeOverride(kKey);
            applied();
            Log::get().note("aa compare: AA RESTORED to %s, what the settings say. Runtime only, no ini written.",
                            configured.c_str());
            break;
        case aacmp::Press::NothingToCompare:
            Log::get().note("aa compare: %s pressed with AA off in the settings (fix.temporal_aa = %s): nothing to compare, nothing "
                            "changed.", g_keyText.c_str(), configured.empty() ? "(unset)" : configured.c_str());
            break;
    }
    menuShowNotice(aacmp::noticeText(p, configured, g_keyText).c_str());
}

}  // namespace

void aaCompareTick() {
    Config& cfg = Config::get();
    rebindIfChanged(cfg);

    if (g_machine.off) {
        const aacmp::Seen seen = g_machine.observe(cfg.configuredTemporalMode(), cfg.hasRuntimeOverride(kKey), nowMs());
        switch (seen) {
            case aacmp::Seen::Nothing: break;
            case aacmp::Seen::EndedByChange:
                cfg.clearRuntimeOverride(kKey);
                applied();
                Log::get().note("aa compare: ended -- fix.temporal_aa changed under the comparison (now %s); your choice applies.",
                                cfg.configuredTemporalMode().c_str());
                break;
            case aacmp::Seen::EndedBySameValue:
                cfg.clearRuntimeOverride(kKey);
                applied();
                Log::get().note("aa compare: ended -- an F8 pick left AA at %s; the comparison is over and AA is that mode.",
                                cfg.configuredTemporalMode().c_str());
                break;
            case aacmp::Seen::EndedByProfile:
                Log::get().note("aa compare: ended -- the profile switched, which drops the runtime override; AA is %s from the "
                                "new profile's settings.", cfg.configuredTemporalMode().c_str());
                break;
        }
    }

    if (g_key.pressed()) onPress(cfg);
    if (g_missedNotes < kMissedNotesMax && g_key.takeMissedWhileUnfocused()) {
        ++g_missedNotes;
        Log::get().note("aa compare: %s was pressed, but another window had focus, so nothing changed. EDVR only acts on its hotkeys "
                        "while Elite is the active window.", g_keyText.c_str());
    }
}

void aaCompareNoteMenuEdit() { g_machine.noteMenuEdit(nowMs()); }

const char* aaCompareKeyText() { return g_keyText.c_str(); }

}  // namespace edvr
