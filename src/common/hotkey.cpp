// GENERATED from src/common/hotkey.cpp in the private edvr repo -- do not edit here.
// Edit there, then: python tools/sync_common.py --write   [body-sha256 d839d33abd006208]
#include "hotkey.h"

#include <cctype>
#include <string>

#include <windows.h>

#include <cstdlib>
#include <cstring>

#include "log.h"

namespace edvr {

// Does the foreground window belong to this process?
//
// GetAsyncKeyState is global: it reports the key whoever is typing, in whatever
// application. Without this check, Scroll Lock pressed in a browser toggled the
// brightness fix and Pause wrote a camera dump -- and VR users routinely have
// another window focused while the headset keeps rendering, so this is the
// normal case rather than an edge one.
//
// Observation only, as before. Nothing is intercepted or withheld from the game.
static bool gameHasFocus() {
    HWND fg = GetForegroundWindow();
    if (!fg) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId();
}

// Are the required modifiers held, and no extra ones?
//
// BOTH halves matter. Requiring them stops a bare SPACE firing a binding that
// is CTRL+ALT+SPACE. Rejecting extras stops CTRL+ALT+SPACE ALSO firing a
// binding that is plain SPACE -- which is the same key, and in Elite those can
// be two different commands.
static bool modsHeld(uint32_t want) {
    const bool ctrl  = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
    const bool alt   = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
    const bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    return ctrl  == ((want & kHotkeyCtrl) != 0) &&
           alt   == ((want & kHotkeyAlt) != 0) &&
           shift == ((want & kHotkeyShift) != 0);
}

void Hotkey::setBinding(const char* name) {
    uint32_t m = 0;
    m_vk = virtualKeyFromName(name, &m);
    m_mods = m_vk ? m : 0;
}

bool Hotkey::pressed() {
    if (m_vk == 0) return false;
    const bool keyDown = (GetAsyncKeyState(m_vk) & 0x8000) != 0;
    const bool down = keyDown && modsHeld(m_mods) && gameHasFocus();
    const bool edge = down && !m_down;
    m_down = down;
    return edge;
}

int virtualKeyFromName(const char* name) { return virtualKeyFromName(name, nullptr); }

int virtualKeyFromName(const char* name, uint32_t* mods) {
    if (mods) *mods = 0;
    if (!name || !*name) return 0;

    // Split on '+' or '-' and take the modifiers off the front. The LAST
    // component is the key; everything before it must be a modifier, and a
    // component that is neither makes the whole binding unrecognised rather
    // than quietly becoming a different one.
    {
        std::string s(name);
        size_t cut = s.find_first_of("+-");
        if (cut != std::string::npos) {
            uint32_t m = 0;
            size_t start = 0;
            bool ok = true;
            std::string last;
            while (true) {
                const size_t sep = s.find_first_of("+-", start);
                std::string part = s.substr(start, sep == std::string::npos
                                                       ? std::string::npos
                                                       : sep - start);
                // Trim spaces and upper-case, so "ctrl + alt + space" works.
                size_t b = part.find_first_not_of(" 	");
                size_t e = part.find_last_not_of(" 	");
                part = (b == std::string::npos) ? std::string()
                                                : part.substr(b, e - b + 1);
                for (char& c : part) c = static_cast<char>(toupper(c));
                if (sep == std::string::npos) { last = part; break; }
                if (part == "CTRL" || part == "CONTROL") m |= kHotkeyCtrl;
                else if (part == "ALT" || part == "MENU") m |= kHotkeyAlt;
                else if (part == "SHIFT") m |= kHotkeyShift;
                else { ok = false; break; }
                start = sep + 1;
            }
            if (!ok || last.empty()) {
                Log::get().note("hotkey: \"%s\" is not a binding this understands. "
                                "Modifiers are CTRL, ALT and SHIFT, joined with '+', "
                                "and the key comes last -- CTRL+ALT+SPACE.", name);
                return 0;
            }
            const int vk = virtualKeyFromName(last.c_str(), nullptr);
            if (vk && mods) *mods = m;
            return vk;
        }
    }

    if (name[0] == '0' && (name[1] == 'x' || name[1] == 'X')) {
        return static_cast<int>(strtol(name, nullptr, 16));
    }

    struct Entry { const char* name; int vk; };
    static const Entry kTable[] = {
        {"F1", VK_F1},   {"F2", VK_F2},   {"F3", VK_F3},   {"F4", VK_F4},
        {"F5", VK_F5},   {"F6", VK_F6},   {"F7", VK_F7},   {"F8", VK_F8},
        {"F9", VK_F9},   {"F10", VK_F10}, {"F11", VK_F11}, {"F12", VK_F12},
        {"F13", VK_F13}, {"F14", VK_F14}, {"F15", VK_F15}, {"F16", VK_F16},
        {"F17", VK_F17}, {"F18", VK_F18}, {"F19", VK_F19}, {"F20", VK_F20},
        {"F21", VK_F21}, {"F22", VK_F22}, {"F23", VK_F23}, {"F24", VK_F24},
        {"SCROLLLOCK", VK_SCROLL}, {"SCROLL", VK_SCROLL},
        {"PAUSE", VK_PAUSE},       {"NUMLOCK", VK_NUMLOCK},
        {"INSERT", VK_INSERT},     {"HOME", VK_HOME},
        {"END", VK_END},           {"DELETE", VK_DELETE},
        {"PAGEUP", VK_PRIOR},      {"PAGEDOWN", VK_NEXT},
        {"NUMPAD0", VK_NUMPAD0},   {"NUMPAD1", VK_NUMPAD1},
        {"NUMPAD2", VK_NUMPAD2},   {"NUMPAD3", VK_NUMPAD3},
        {"NUMPAD4", VK_NUMPAD4},   {"NUMPAD5", VK_NUMPAD5},
        {"NUMPAD6", VK_NUMPAD6},   {"NUMPAD7", VK_NUMPAD7},
        {"NUMPAD8", VK_NUMPAD8},   {"NUMPAD9", VK_NUMPAD9},
        {"MULTIPLY", VK_MULTIPLY}, {"DIVIDE", VK_DIVIDE},
        {"ADD", VK_ADD},           {"SUBTRACT", VK_SUBTRACT},
        // The arrow keys were missing, and Elite binds the camera-view cycle to
        // one of them by default. Asking for RIGHT fell through to the
        // unrecognised path below, which returned "no key" in silence -- so the
        // feature simply never fired and nothing said why.
        {"RIGHT", VK_RIGHT},       {"LEFT", VK_LEFT},
        {"UP", VK_UP},             {"DOWN", VK_DOWN},
        {"RIGHTARROW", VK_RIGHT},  {"LEFTARROW", VK_LEFT},
        {"UPARROW", VK_UP},        {"DOWNARROW", VK_DOWN},
        {"SPACE", VK_SPACE},       {"TAB", VK_TAB},
        {"ENTER", VK_RETURN},      {"RETURN", VK_RETURN},
        {"BACKSPACE", VK_BACK},    {"ESCAPE", VK_ESCAPE},
        {"ESC", VK_ESCAPE},
    };

    for (const Entry& e : kTable) {
        if (_stricmp(name, e.name) == 0) return e.vk;
    }
    // Single printable character: use its uppercase VK.
    if (name[1] == '\0') {
        const char c = name[0];
        if (c >= 'a' && c <= 'z') return c - 'a' + 'A';
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) return c;
    }
    // A name nobody recognises is NOT the same as no key, and returning 0 for
    // both is what made this invisible: a typo, or a key this table has never
    // heard of, produced a feature that silently never fired. An empty setting
    // is a choice and returns 0 above, without comment; getting here means
    // somebody asked for something specific and did not get it.
    Log::get().note("hotkey \"%s\" is not a key name EDVR knows, so nothing is "
                    "bound. Try F1-F24, SCROLLLOCK, PAUSE, NUMLOCK, INSERT, HOME, "
                    "END, DELETE, PAGEUP, PAGEDOWN, LEFT, RIGHT, UP, DOWN, SPACE, "
                    "TAB, ENTER, ESCAPE, NUMPAD0-9, a single letter or digit, or "
                    "0x## for a virtual-key code.", name);
    return 0;
}

}  // namespace edvr
