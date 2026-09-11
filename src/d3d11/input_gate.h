// The keyboard gate: the game sees no keyboard while the settings menu is
// open (docs/settings-menu.md, "The keyboard gate").
//
// MEASURED 2026-09-07 from EliteDangerous64.exe's import table: the game
// reads the keyboard through exactly three doors, all in this process --
// DirectInput8's keyboard device (the bindings path), the user32 key-state
// trio (GetAsyncKeyState / GetKeyState / GetKeyboardState), and its
// PeekMessageA message pump (WM_KEYDOWN and the WM_CHAR TranslateMessage
// makes from it). It imports no Raw Input function at all. So one atomic
// flag and three thin hooks make the keyboard private:
//
//   Door 1  The EXE's DirectInput8Create import is captured during loader
//           attach. Returned factories' CreateDevice methods capture the
//           game's actual keyboard, including private Steam-overlay tables,
//           and hook GetDeviceState / GetDeviceData without changing identity.
//           A fallback uses the shared vtable of a dummy keyboard device EDVR
//           creates itself (dinput8.dll dispatches every device object of
//           the class through one static table, which is the mechanism; a
//           CopyVptr hook on our dummy would hook our dummy and nothing
//           else). InPlace, deliberately, with vtable_hook.h's costs
//           accepted: reclaim() from the frame path, the `this` check in
//           every thunk, a foreign entry chained through. Only KEYBOARD
//           devices are filtered -- a HOTAS, a throttle and pedals share
//           the table and are never touched: the ship keeps flying.
//   Door 2  the user32 trio, patched in the EXECUTABLE's import table only
//           (iat_hook.h). EDVR's own modules keep reading the real keys.
//   Door 3  PeekMessageA, the same way: a keyboard message becomes WM_NULL
//           in place, so no WM_CHAR is ever made from it.
//
// THE POLICY while private: release everything, admit nothing. State reads
// return every key up; buffered reads keep the key-UPS and drop the DOWNS,
// so a key the game saw go down before the menu opened still gets its
// release and no toggle is left half-pressed; the trio answers up;
// keyboard messages are nulled.
// After closing, keys held at that instant remain private until released,
// so the Escape that closes EDVR cannot also open Elite's pause menu.
//
// THE SUMMON KEY IS PRIVATE TOO, with the menu closed: whenever its chord
// is held, the key itself is swallowed at every door, so the game never
// sees the press that opens the menu. One byte comparison per DirectInput
// state read for the session.
//
// FAIL-OPEN: every thunk forwards untouched on any doubt -- an unknown
// device, a buffer of the wrong size, a fault (budgeted per door: a door
// that faults retires to pass-through for the session and says so). The
// flag is set by the menu, which sets it only while the panel is actually
// being drawn (frame_flag.h's menuDrawn), never on its own belief.
//
// WHAT IS NOT CAPTURED: the mouse, DirectInput joysticks, XInput pads (a
// phase-B door of the same shape). Nothing here reads a key EDVR does not
// already read through hotkey.cpp; nothing here injects anything.
#pragma once

#include <cstdint>

namespace edvr {

class Config;

// Capture DirectInput factories before the game creates its keyboard.
// Loader-safe: patches only the executable's existing import slot.
void inputGateInstallEarly();

// Reads hotkey.menu (for the swallow), menu.keyboard (private | shared)
// and advanced.input_probe. Install AND reload.
void inputGateConfigure(Config& cfg);

// Install the doors. Idempotent; each door reports its own outcome once.
// Called lazily by the menu on its first use, from the frame thread. The
// early factory capture above is already installed before game startup.
void inputGateInstall();

// The flag. True while the menu is drawn AND menu.keyboard = private. The
// menu calls this every frame with its current answer, the heartbeat
// discipline: a caller that stops calling leaves the flag wherever it was,
// which is why the menu also clears it on every path that closes.
void inputGateSetPrivate(bool priv);
bool inputGatePrivate();

// The stricter question the menu's adopted Elite keys ask (menu_keys.h):
// is the flag set AND a DirectInput door installed and not retired AND the
// game's keyboard seen reaching one -- the same two facts the Status
// page's doors line prints. inputGatePrivate() reports the flag alone: a
// door that faulted and retired does not clear it, and on a rig whose game
// device dispatches through a table the door is not on ("Tab boosts") the
// flag is set while every key still reaches the ship. An Elite panel key
// acting on the menu there would drive the ship and the menu at once.
// MEASURED 2026-09-11 on the maintainer's rig: "reached" arrives in the
// same millisecond as the first open.
bool inputGateHoldsGameKeyboard();

// The door evidence alone -- a live DirectInput door the game has been
// seen reaching -- without the flag. For the Status page's "Elite keys"
// row: that page is never private by design, so the question there is
// whether the keys WOULD act on a settings page, not whether they do now.
bool inputGateGameKeyboardSeen();

// Once per frame, from the frame boundary: reclaim on the dinput table,
// the probe's periodic line.
void inputGateTick();

// For the Status page: which doors are installed, which the game has been
// seen reaching, and whether keys are private right now. Each is a short
// phrase; buf receives a single line.
void inputGateStatusLine(char* buf, size_t bufLen);

// The DirectInput scan code (DIK_*) for a Windows virtual key, or 0 when
// the mapping is not known. Exposed for the test.
uint8_t inputGateDikOf(int vk);

// The filter policies, pure, for the test (docs/settings-menu.md's
// "release everything, admit nothing"):
//   * inputGateFilterState zeroes a 256-byte keyboard state (private), or
//     clears one key when its chord's modifiers are held in the buffer
//     (the summon swallow; mods as hotkey.h's kHotkey* bits).
//   * inputGateFilterData compacts a DIDEVICEOBJECTDATA array in place:
//     private keeps the ups and drops the downs; the swallow drops the
//     summon key's events when `modsHeld`. Returns the kept count.
struct DiObjectData {   // DIDEVICEOBJECTDATA's layout, dinput.h's order
    uint32_t dwOfs;
    uint32_t dwData;
    uint32_t dwTimeStamp;
    uint32_t dwSequence;
    uint64_t uAppData;
};
void     inputGateFilterState(uint8_t* state256, bool priv, uint8_t summonDik,
                              uint32_t summonMods);
uint32_t inputGateFilterData(DiObjectData* data, uint32_t count, bool priv,
                             uint8_t summonDik, bool summonModsHeld);

void inputGateShutdown();

}  // namespace edvr
