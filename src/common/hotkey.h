// GENERATED from src/common/hotkey.h in the private edvr repo -- do not edit here.
// Edit there, then: python tools/sync_common.py --write   [body-sha256 745d9f82c1d57fb2]
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
    // Did a press get thrown away because another window had focus?
    //
    // True once per such press, cleared by reading. The focus rule is right --
    // GetAsyncKeyState is global and Scroll Lock typed in a browser used to
    // toggle the brightness fix -- but its failure is SILENT, and for a
    // diagnostic key that is the worst possible shape: the player presses the
    // key that is supposed to write a log, nothing is written, and the log they
    // would consult to find out why is the one that was not written. Measured:
    // a session where the external-camera key registered twice and Pause never
    // did, leaving a reported flash with no capture. In VR another window
    // holding focus is the ordinary case, not an edge one.
    bool takeMissedWhileUnfocused() {
        const bool m = m_missedUnfocused;
        m_missedUnfocused = false;
        return m;
    }

    int  key() const { return m_vk; }
    uint32_t mods() const { return m_mods; }

    // True exactly once per physical press, with the modifiers held.
    bool pressed();

    // pressed(), with the world supplied instead of read.
    //
    // pressed() asks Windows for the key state and the foreground window, so
    // nothing can assert its edge behaviour -- and the edge behaviour is where
    // the bug was: a suppressed binding could mint an edge when the modifiers
    // were released, inverting a toggle from a press the player had finished
    // with. Threading the inputs through one function makes that testable
    // without a keyboard, and leaves pressed() as the thin part that reads
    // them.
    bool pressedWith(bool keyDown, uint32_t held, bool focused);

private:
    int      m_vk = 0;
    uint32_t m_mods = 0;
    bool     m_down = false;
    bool     m_missedUnfocused = false;
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

// Would a binding of (vk, mods) fire with `held` modifiers down?
//
// Pure, and separated from pressed() so it can be asserted without a keyboard.
// The rule has two halves and both were wrong at some point: the binding's
// modifiers must be HELD (extras allowed, so sprinting with Shift does not make
// EDVR miss the player's camera key), and no OTHER binding on the same key may
// be a strictly better match (so one press cannot fire two bindings and toggle
// an intent straight back off).
bool hotkeyWouldFire(int vk, uint32_t mods, uint32_t held);

// Forget every registered binding. For tests; bindings re-register on setBinding.
void hotkeyResetBindings();

}  // namespace edvr
