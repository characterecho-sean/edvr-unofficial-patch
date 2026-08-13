// GENERATED from src/common/hotkey.h in the private edvr repo -- do not edit here.
// Edit there, then: python tools/sync_common.py --write   [body-sha256 2b277cb32edf3c77]
// Edge-triggered hotkey polling.
//
// Polled from the frame loop with GetAsyncKeyState rather than installed as a
// keyboard hook: a low-level hook is a process-wide input tap, which is more
// privilege than a timestamp marker needs and more than this project wants to
// be seen taking. Frame-granularity timing is plenty for annotating a trace.
#pragma once

#include <cstdint>

namespace edvr {

class Hotkey {
public:
    Hotkey() = default;
    // vk is a Windows virtual-key code; 0 disables.
    explicit Hotkey(int vk) : m_vk(vk) {}

    void setKey(int vk) { m_vk = vk; }
    int  key() const { return m_vk; }

    // True exactly once per physical press.
    bool pressed();

private:
    int  m_vk = 0;
    bool m_down = false;
};

// Maps a config string ("F9", "SCROLLLOCK", "0x78") to a virtual-key code.
// Returns 0 if unrecognised.
int virtualKeyFromName(const char* name);

}  // namespace edvr
