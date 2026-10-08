// Elite's names for the XInput pad's buttons, in one place.
//
// Two readers need the same table: xinput_watch.cpp, which turns a name into
// the button mask it polls, and hotkey.cpp, which parses a pad hotkey out of
// edvr.ini ("explorer_cam = GamePad_Back") and writes one back after the
// settings menu captured a button press. The names are Elite's own, as its
// .binds files spell them, so a hotkey value and a binding of the player's
// compare as text without a translation table in the middle.
//
// The masks are XInput's XINPUT_GAMEPAD_* values, written out so this header
// needs no <Xinput.h>; xinput_watch.cpp static_asserts every one of them.
//
// The first entry for a mask is the canonical spelling, the one a capture
// writes. The later entries are variants seen in the wild and still parse.
#pragma once

#include <cstdint>
#include <cstring>

namespace edvr {

constexpr uint16_t kPadUp = 0x0001, kPadDown = 0x0002, kPadLeft = 0x0004, kPadRight = 0x0008;
constexpr uint16_t kPadStart = 0x0010, kPadBack = 0x0020;
constexpr uint16_t kPadLeftThumb = 0x0040, kPadRightThumb = 0x0080;
constexpr uint16_t kPadLeftShoulder = 0x0100, kPadRightShoulder = 0x0200;
constexpr uint16_t kPadA = 0x1000, kPadB = 0x2000, kPadX = 0x4000, kPadY = 0x8000;

struct PadName {
    const char* elite;
    uint16_t    buttons;   // XINPUT_GAMEPAD_* mask, or 0 for a trigger
    uint8_t     trigger;   // 1 left, 2 right, 0 for a button
};

inline constexpr PadName kPadNames[] = {
    {"GamePad_FaceDown", kPadA, 0},
    {"GamePad_FaceRight", kPadB, 0},
    {"GamePad_FaceLeft", kPadX, 0},
    {"GamePad_FaceUp", kPadY, 0},
    {"GamePad_A", kPadA, 0},
    {"GamePad_B", kPadB, 0},
    {"GamePad_X", kPadX, 0},
    {"GamePad_Y", kPadY, 0},
    {"GamePad_DPadUp", kPadUp, 0},
    {"GamePad_DPadDown", kPadDown, 0},
    {"GamePad_DPadLeft", kPadLeft, 0},
    {"GamePad_DPadRight", kPadRight, 0},
    {"GamePad_Back", kPadBack, 0},
    {"GamePad_Start", kPadStart, 0},
    {"GamePad_LBumper", kPadLeftShoulder, 0},
    {"GamePad_RBumper", kPadRightShoulder, 0},
    {"GamePad_LShoulder", kPadLeftShoulder, 0},
    {"GamePad_RShoulder", kPadRightShoulder, 0},
    {"GamePad_LThumb", kPadLeftThumb, 0},
    {"GamePad_RThumb", kPadRightThumb, 0},
    {"GamePad_LStick", kPadLeftThumb, 0},
    {"GamePad_RStick", kPadRightThumb, 0},
    {"GamePad_LTrigger", 0, 1},
    {"GamePad_RTrigger", 0, 2},
};

// The entry for an Elite pad key name. An axis binding carries a direction
// prefix ("Pos_GamePad_RTrigger"); the trigger reads the positive direction
// either way, so the prefix is dropped. Null for a name this table lacks.
inline const PadName* padNameFind(const char* eliteKey) {
    if (!eliteKey) return nullptr;
    if (_strnicmp(eliteKey, "Pos_", 4) == 0 || _strnicmp(eliteKey, "Neg_", 4) == 0) eliteKey += 4;
    for (const PadName& m : kPadNames) {
        if (_stricmp(eliteKey, m.elite) == 0) return &m;
    }
    return nullptr;
}

// The canonical name of a (mask, trigger) pair, or null when it is none of the
// table's single buttons. A mask with two bits set (a chord) has no name.
inline const char* padNameOf(uint16_t buttons, uint8_t trigger) {
    for (const PadName& m : kPadNames) {
        if (m.buttons == buttons && m.trigger == trigger) return m.elite;
    }
    return nullptr;
}

}  // namespace edvr
