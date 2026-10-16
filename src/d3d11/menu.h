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

#include "../../include/edvr_plugin_api.h"
#include <cstdint>

struct ID3D11Device;
struct IDXGISwapChain;

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

// The flat panel is composited onto the owned desktop swapchain immediately
// before Present. The model/raster still advances from menuTick after Present.
void menuFlatBeforePresent(IDXGISwapChain* swap, unsigned flags);
void menuFlatResize();
// Called at a profile switch, render thread, frame boundary: closes the menu, releases the flat panel's raster, and makes the
// next menuConfigure run the first-time block of the profile now active.
void menuProfileSwitch();
// Whether the menu has run its configure block for the profile now active (tools\profile_switch_test reads it).
bool menuIsConfigured();

// Raw pad hotkeys are read independently of Elite's XInput door. While the
// drawn flat panel owns navigation, suppress conflicting existing ini bindings.
bool menuFlatPadOwnsHotkey(uint16_t buttons, uint8_t trigger);

// The reload poll re-read edvr.ini: refresh every row's value, diff the
// restart snapshot, and toast what changed from outside the menu.
void menuNoteConfigReloaded();

// A short notice from outside the menu (the AA comparison key's "AA off (comparison)..."). VR: a toast, in place of any queued AA toast
// and gated on menu.toasts like every toast. Flat, which has no toasts: a small card in the screen's upper left for about 2.5 s, drawn
// from the menu's own raster, and dropped if the menu is open. Logged either way, so a notice that did not show says why.
void menuShowNotice(const char* text);

// The caller changed `dotted` itself (a runtime override, not an ini write), so the reload that follows must not toast it as an edit
// made from outside. One-shot, like the menu's own write mark.
void menuNoteOwnChange(const char* dotted);

// Does the menu want the config poll to run NOW rather than at its cadence?
// True once after each write the menu made, so the change lands this frame
// through the same configure path a hand edit takes.
bool menuTakeConfigPollRequest();

// Ask for that poll now without the menu having written anything: for a change made in memory (Config's runtime override, the AA
// comparison key), whose reload reports it and runs the configure sweep every module re-reads its setting in.
void menuRequestConfigPoll();

// An action row for the Instruments page: `fn(user)` runs on the frame
// thread when the row is activated. Registered by device_hook for the
// things that otherwise need a hotkey bound.
typedef void (*MenuActionFn)(void* user);
void menuRegisterAction(const char* label, const char* hint, MenuActionFn fn, void* user);

void menuShutdown();

// Addon settings on the Plugins page belong to the addon that registered them (its stem). A setting
// holds the addon's getter, setter and user-data pointers, so they must go before its DLL does.
int menuRegisterPluginSetting(const char* ownerId, const EdvrPluginSettingDef* setting);
// Retires the addon's rows at once (no callback into it again) and queues their removal for the menu thread, so this is
// safe from any thread; it does not rebuild the pages.
void menuRemovePluginSettings(const char* ownerId);
// Hides every addon row and the approval list (a flat session: the addons are dormant), or shows them again. Nothing is
// deleted. Queued for the menu thread like a removal.
void menuSetPluginRowsHidden(bool hidden);

}  // namespace edvr

