#pragma once

#include <cstdint>

#include "menu_keys.h"

namespace edvr {

// Only controls consumed by the flat settings panel. Sticks, triggers,
// Start/Back, thumb clicks and X/Y always remain the game's.
constexpr uint16_t kMenuPadMask = 0x000F | 0x0300 | 0x3000;
constexpr int kMenuPadCount = 8;
// Present may block for seconds while a temporal backend creates its device.
// Keep the request bounded, and revoke immediately when focus is lost.
constexpr uint64_t kMenuPadOwnershipMs = 5000;
constexpr uint64_t kMenuPadDoorFreshMs = 500;
inline bool menuPadOwnsButton(uint16_t buttons, uint8_t trigger) {
    return trigger == 0 && (buttons & kMenuPadMask) != 0;
}
inline bool menuPadMayOwn(int index, int requested, uint64_t requestMs, uint64_t now,
                          bool installed, bool retired, bool focused) {
    return installed && !retired && focused && index >= 0 && index < 4 && requested == index &&
           requestMs != 0 && now >= requestMs && now - requestMs <= kMenuPadOwnershipMs;
}
constexpr uint16_t kMenuPadButtons[kMenuPadCount] = {
    0x0001, 0x0002, 0x0004, 0x0008, 0x1000, 0x2000, 0x0100, 0x0200};
constexpr MenuNav kMenuPadNavs[kMenuPadCount] = {
    kNavUp, kNavDown, kNavLeft, kNavRight, kNavSelect, kNavBack, kNavPagePrev, kNavPageNext};

struct MenuPadSample {
    uint32_t packet = 0;
    uint16_t buttons = 0;
    uint8_t leftTrigger = 0, rightTrigger = 0;
    int16_t leftX = 0, leftY = 0, rightX = 0, rightY = 0;
};

inline bool menuPadSameOutput(const MenuPadSample& a, const MenuPadSample& b) {
    return a.buttons == b.buttons && a.leftTrigger == b.leftTrigger &&
           a.rightTrigger == b.rightTrigger && a.leftX == b.leftX &&
           a.leftY == b.leftY && a.rightX == b.rightX && a.rightY == b.rightY;
}

// One instance per XInput index, under the door's lock. No struct padding
// comparison: the packet changes exactly when visible controller data changes.
struct MenuPadGate {
    bool owned = false;
    bool initialized = false;
    bool transformed = false;
    uint16_t tail = 0;
    MenuPadSample visible;

    MenuPadSample filter(MenuPadSample raw, bool own) {
        // Before any ownership, preserve the game's actual packet number.
        // Remember it so the very first masking transition can bump it even
        // if the hardware packet has not changed.
        if (!own && !transformed) {
            visible = raw;
            initialized = true;
            return raw;
        }
        if (own) transformed = true;
        if (own) tail = 0;
        else {
            if (owned) tail |= raw.buttons & kMenuPadMask;
            tail &= raw.buttons;
        }
        owned = own;
        MenuPadSample out = raw;
        out.buttons &= static_cast<uint16_t>(~(own ? kMenuPadMask : tail));
        if (initialized) out.packet = visible.packet + (menuPadSameOutput(out, visible) ? 0u : 1u);
        visible = out;
        initialized = true;
        return out;
    }

    void disconnect() { *this = MenuPadGate{}; }
};

struct MenuPadNavigation {
    KeyRepeat key[kMenuPadCount];

    void prime(uint16_t buttons) {
        for (int i = 0; i < kMenuPadCount; ++i)
            keyRepeatPrime(key[i], (buttons & kMenuPadButtons[i]) != 0);
    }

    int step(int i, uint16_t buttons, uint64_t now, bool act) {
        const bool wasDown = key[i].down;
        const int n = keyRepeatStep(key[i], (buttons & kMenuPadButtons[i]) != 0, now, act);
        // Directional controls repeat; action/back/page controls are edges.
        return i >= 4 && wasDown ? 0 : n;
    }

    bool stepHeld() const { return key[2].down || key[3].down; }
};

}  // namespace edvr
