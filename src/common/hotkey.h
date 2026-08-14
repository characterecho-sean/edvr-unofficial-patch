// GENERATED from src/common/hotkey.h in the private edvr repo -- do not edit here.
// Edit there, then: python tools/sync_common.py --write   [body-sha256 4cb3697e93de8750]
// Edge-triggered hotkey polling.
//
// Polled from the frame loop with GetAsyncKeyState rather than installed as a
// keyboard hook: a low-level hook is a process-wide input tap, which is more
// privilege than a timestamp marker needs and more than this project wants to
// be seen taking. Frame-granularity timing is plenty for annotating a trace.
#pragma once

#include <cstdint>

namespace edvr {

// Modifier flags, ORed. Not Windows' MOD_* values: those are for
// RegisterHotKey, which this deliberately does not use.
enum HotkeyMods : uint32_t {
    kHotkeyCtrl  = 1u << 0,
    kHotkeyAlt   = 1u << 1,
    kHotkeyShift = 1u << 2,
};

class Hotkey {
public:
    Hotkey() = default;
    // vk is a Windows virtual-key code; 0 disables.
    explicit Hotkey(int vk) : m_vk(vk) {}

    void setKey(int vk) { m_vk = vk; m_mods = 0; }
    void setKey(int vk, uint32_t mods) { m_vk = vk; m_mods = mods; }
    // Parse a config string and take BOTH halves of the answer.
    //
    // The two-step form -- setKey(virtualKeyFromName(s)) -- compiles fine and
    // silently discards the modifiers, turning CTRL+ALT+SPACE into a bare
    // SPACE. That is a binding that fires when it should not, which is worse
    // than one that never fires, so there is one call that cannot do it.
    void setBinding(const char* name);
    int  key() const { return m_vk; }
    uint32_t mods() const { return m_mods; }

    // True exactly once per physical press, with the modifiers held.
    bool pressed();

private:
    int      m_vk = 0;
    uint32_t m_mods = 0;
    bool     m_down = false;
};

// Maps a config string to a virtual-key code, with optional modifiers.
//
// COMBINATIONS ARE SUPPORTED, and they have to be: Elite's own default for the
// external camera is CTRL + ALT + SPACE, so a single-key-only parser would have
// left this feature unusable for anybody who had not rebound it -- and would
// have failed by silently watching the wrong key rather than by saying so.
//
//   "F9"                 a plain key
//   "CTRL+ALT+SPACE"     a chord; also CONTROL, ALT/MENU, SHIFT
//   "0x78"               a raw virtual-key code
//
// Separators are '+' or '-', spaces are ignored, and case does not matter.
// Returns 0 if the main key is unrecognised; *mods receives the modifier flags
// and may be null.
int virtualKeyFromName(const char* name, uint32_t* mods);
int virtualKeyFromName(const char* name);

}  // namespace edvr
