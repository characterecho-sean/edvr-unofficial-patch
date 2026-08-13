#include "hotkey.h"

#include <windows.h>

#include <cstdlib>
#include <cstring>

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

bool Hotkey::pressed() {
    if (m_vk == 0) return false;
    const bool down = (GetAsyncKeyState(m_vk) & 0x8000) != 0 && gameHasFocus();
    const bool edge = down && !m_down;
    m_down = down;
    return edge;
}

int virtualKeyFromName(const char* name) {
    if (!name || !*name) return 0;

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
    return 0;
}

}  // namespace edvr
