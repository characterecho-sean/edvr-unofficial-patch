// The in-headset settings menu (docs/settings-menu.md).
//
// Every knob EDVR has is a taste-and-cost trade that can only be judged with
// the headset on, over the scene that is actually struggling. This is the
// panel that lets it be judged there: summoned by hotkey.menu, anchored in
// the world where you were looking, driven by the arrow keys or by where
// your head points, and written straight into edvr.ini so the game's own
// reload applies it -- the file stays the single source of truth, and the
// installer's window and this panel can never disagree.
//
// While a settings page is drawn the keyboard is the menu's (input_gate.h): the
// game sees every key released, so Up, Down, Enter and Escape are free to
// mean what they say. The gate follows the DRAW, not the menu's belief: a
// panel that is not reaching the headset never takes the keyboard.
// The read-only Status and Monitor pages deliberately pass keys to the game.
//
// Elite's own panel keys -- UI_Up .. CyclePreviousPage, the ones that walk
// the cockpit panels -- are read from the player's bindings and become
// ALIASES of the menu's own actions: the same dispatcher, never a second
// one, and never registered as Hotkeys. They are inert unless the gate
// provably holds the game's keyboard (elsewhere an adopted key IS a game
// key), inert while a value is typed, and a key held across a state change
// is swallowed until released -- except the next/previous tab pair, which
// follows Tab and changes the page on the shared pages too. menu_keys.h
// holds the rules; the panel's bottom line names the live keys.
//
// This half owns the model, the keys, the aim, the ini write and the restart
// bookkeeping. menu_panel.h owns the pixels: the GDI rasterisation and the
// compute composite the openvr half calls at the door.
#pragma once

#include <cstdint>

struct ID3D11Device;

namespace edvr {

class Config;

// Reads hotkey.menu and every [menu] key. Install AND reload.
void menuConfigure(Config& cfg);

// Read (or, with enabled=false, drop) Elite's panel keys from the player's
// bindings and resolve them into the menu's aliases. Frame thread only;
// after menuConfigure, which places the summon key the rules check
// against. `why` is null on the first read and names the reason on a
// re-read ("your Elite bindings changed"). Exactly one "menu keys:" line
// follows every call, so a session log with none means it never ran.
void menuAdoptGameBindings(bool enabled, const char* why);

// Once per frame, from the frame boundary. dev may be null before the game
// has a device; nothing is drawn or uploaded until it has one.
void menuTick(ID3D11Device* dev);

// The reload poll re-read edvr.ini: refresh every row's value, diff the
// restart snapshot, and toast what changed from outside the menu.
void menuNoteConfigReloaded();

// Does the menu want the config poll to run NOW rather than at its cadence?
// True once after each write the menu made, so the change lands this frame
// through the same configure path a hand edit takes.
bool menuTakeConfigPollRequest();

// An action row for the Instruments page: `fn(user)` runs on the frame
// thread when the row is activated. Registered by device_hook for the
// things that otherwise need a hotkey bound.
typedef void (*MenuActionFn)(void* user);
void menuRegisterAction(const char* label, const char* hint, MenuActionFn fn, void* user);

// Is the panel up (fading in, showing, or fading out)?
bool menuOpen();

void menuShutdown();

}  // namespace edvr
