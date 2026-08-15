// GENERATED from src/common/hotkey.cpp in the private edvr repo -- do not edit here.
// Edit there, then: python tools/sync_common.py --write   [body-sha256 36fd7f3b33246134]
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

// Which modifiers are physically down right now.
static uint32_t heldMods() {
    uint32_t m = 0;
    if (GetAsyncKeyState(VK_CONTROL) & 0x8000) m |= kHotkeyCtrl;
    if (GetAsyncKeyState(VK_MENU) & 0x8000) m |= kHotkeyAlt;
    if (GetAsyncKeyState(VK_SHIFT) & 0x8000) m |= kHotkeyShift;
    return m;
}

// SUBSET, not equality: the binding's modifiers must be held, and extra ones
// are allowed.
//
// Equality was the first attempt and it is wrong for the job. These are the
// player's OWN Elite bindings, and EDVR is trying to see the same press the
// game sees -- while the player is holding Shift to sprint, or Ctrl for
// something else. Under equality, CTRL+ALT+SPACE with Shift also down does not
// match, EDVR misses the press, and the intent toggle desynchronises for the
// session. Missing a press is the expensive failure here; that is the whole
// class of bug this gate has been chasing.
//
// It also silently changed two settings that predate all of this. Scroll Lock
// and Pause used to fire whatever was held, because nothing looked at
// modifiers at all. Equality quietly made them fire only when bare -- an
// unannounced change to behaviour people already had muscle memory for.
// Subset restores it.
static bool modsSatisfied(uint32_t want, uint32_t held) {
    return (want & ~held) == 0;
}

// Bindings that exist, so a plain one can tell when a combo on the same key is
// the better match.
//
// Subset matching alone lets ONE physical press fire TWO bindings: with SHIFT+F
// and bare F both bound, pressing SHIFT+F satisfies both. For this feature that
// is not cosmetic -- external_camera is a toggle, so a double fire sets the
// intent and immediately clears it, and the offset never arms.
//
// A registry rather than an ordering rule, because the bindings are independent
// objects polled in whatever order the frame loop happens to use, and a rule
// that depends on the combo being polled first would be right only by accident.
struct Registered { int vk; uint32_t mods; };
static Registered g_bindings[16];
static unsigned   g_bindingCount = 0;

static void registerBinding(int vk, uint32_t mods) {
    if (!vk) return;
    for (unsigned i = 0; i < g_bindingCount; ++i) {
        if (g_bindings[i].vk == vk && g_bindings[i].mods == mods) return;
    }
    if (g_bindingCount < 16) g_bindings[g_bindingCount++] = {vk, mods};
}

// Is some OTHER binding on this key a strictly better match right now?
static bool betterMatchExists(int vk, uint32_t mine, uint32_t held) {
    for (unsigned i = 0; i < g_bindingCount; ++i) {
        const Registered& b = g_bindings[i];
        if (b.vk != vk || b.mods == mine) continue;
        // More modifiers, all of them held: that binding is what the player
        // pressed, and this one is the accidental subset.
        if (modsSatisfied(b.mods, held) && (b.mods & ~mine) != 0) return true;
    }
    return false;
}

void Hotkey::setBinding(const char* name) {
    uint32_t m = 0;
    m_vk = virtualKeyFromName(name, &m);
    m_mods = m_vk ? m : 0;
    registerBinding(m_vk, m_mods);
}

bool hotkeyWouldFire(int vk, uint32_t mods, uint32_t held) {
    if (!vk) return false;
    return modsSatisfied(mods, held) && !betterMatchExists(vk, mods, held);
}

void hotkeyResetBindings() { g_bindingCount = 0; }

bool Hotkey::pressed() {
    if (m_vk == 0) return false;
    return pressedWith((GetAsyncKeyState(m_vk) & 0x8000) != 0, heldMods(),
                       gameHasFocus());
}

bool Hotkey::pressedWith(bool keyDown, uint32_t held, bool focused) {
    if (m_vk == 0) return false;
    const bool matches = keyDown && hotkeyWouldFire(m_vk, m_mods, held);
    const bool fire = matches && focused;
    const bool edge = fire && !m_down;

    // The same edge test, on the far side of the focus rule.
    //
    // A press that matched the binding and was thrown away only because another
    // window had focus is recorded here so somebody can be told. It uses !m_down
    // for the same reason the real edge does: one physical press, one report,
    // not one a frame for as long as the key is held.
    if (matches && !focused && !m_down) m_missedUnfocused = true;

    // m_down latches the RAW key, not whether this binding fired.
    //
    // It used to latch the composite, and that let one physical press produce
    // an edge from a binding that had already been suppressed. Press
    // CTRL+ALT+SPACE with a bare SPACE binding also configured: SPACE is
    // suppressed, so its m_down stays false. Release CTRL and ALT while SPACE
    // is still held and the suppression lifts -- keyDown is still true, m_down
    // is still false, and a fresh edge is minted from a press the player made
    // once and finished with.
    //
    // On a toggle that is not a stray event, it is an inversion: the combo set
    // the intent, and letting go of the modifiers clears it again.
    //
    // Latching the raw key makes the suppression unable to create edges at all,
    // because an edge now requires the key to have been physically UP. It also
    // fixes the same shape for focus: a press made while another window has
    // focus no longer fires the moment focus returns.
    m_down = keyDown;
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
