// The player's own Elite keybindings, read from where the game keeps them.
//
// Elite stores bindings as XML in Options\Bindings\*.binds under its local
// appdata, with StartPreset.start naming the active preset. The two bindings
// EDVR watches for -- the external-camera toggle and the next-vanity-view
// cycle -- are in there, already answered, for every player who has ever
// bound them in the game. Reading them removes the last piece of manual
// setup: no ini editing at all for keyboard players.
//
// Read at startup and RE-READ when the files change: Elite rewrites this
// directory the moment a rebind or preset switch is applied, so a slow stat
// (eliteBindsFingerprint below) notices within seconds and the adopted keys
// follow without a restart. Keyboard entries only. A binding on a controller
// or HOTAS is reported and skipped -- EDVR watches the keyboard. An explicit
// hotkey.* value in edvr.ini always wins over what is read here, so nothing
// changes for anyone who has already set up.
#pragma once

#include <cstddef>

namespace edvr {

// Look up an Elite binding element (e.g. "PhotoCameraToggle_Humanoid",
// "VanityCameraScrollRight") in the active preset's files and translate it
// to an EDVR binding string ("F11", "SHIFT+RIGHT", "["). Returns true and
// fills `out` when a keyboard binding was found; false when the element is
// unbound, bound to a non-keyboard device, or the files cannot be read.
//
// `fallbackElement`, when given, is consulted ONLY where the primary
// element is entirely ABSENT from the chosen file. On foot the game acts
// on PhotoCameraToggle_Humanoid exclusively -- a Humanoid entry bound to a
// controller must NOT fall through to the ship element's keyboard key,
// because that key does nothing on foot and watching it is the
// missed-press desync class.
bool eliteBindsLookup(const char* element, char* out, size_t outLen,
                      const char* fallbackElement = nullptr);

// The GamePad form: returns the raw Elite key name (e.g. "GamePad_Back")
// of a Primary or Secondary slot bound to the XInput pad, for the
// xinput watcher to translate. A slot carrying a Modifier chord is
// skipped -- watching half a chord would fire on a bare button the game
// ignores. Same preset/file selection rules as the keyboard lookup.
bool eliteBindsLookupPad(const char* element, char* out, size_t outLen);

// The MODIFIER of an element's gamepad slot, when it has one -- the half
// eliteBindsLookupPad throws away by skipping chorded slots.
//
// Wanted by a binding that shares a button with a chord and has to know when
// to stand aside: watching DPad-Right for the view cycle means nothing unless
// you can also tell that this particular DPad-Right came with Face-Right and
// belongs to the camera toggle.
bool eliteBindsLookupPadMod(const char* element, char* out, size_t outLen);
bool eliteBindsLookupPadDir(const wchar_t* dir, const char* element,
                            char* out, size_t outLen);

// The same lookup against an explicit bindings directory, exposed for the
// smoke test -- the selection rules (newest maintained file wins; stale
// previous-format presets lose) earned a harness the day a January relic
// answered a live rebind.
bool eliteBindsLookupDir(const wchar_t* dir, const char* element,
                         char* out, size_t outLen,
                         const char* fallbackElement = nullptr);

// The Elite-name to EDVR-name translation, exposed for the smoke test:
// "Key_F11" -> "F11", "Key_RightArrow" -> "RIGHT", "Key_BackSlash" -> "\\",
// "Key_SemiColon" -> "SEMICOLON". Returns false for names it cannot map.
bool eliteBindsTranslateKey(const char* eliteKey, char* out, size_t outLen);

// A cheap stamp over the bindings directory: names, sizes and write times of
// its files, folded together. It changes when the player applies a rebind or
// switches preset in-game, which is when Elite rewrites the files. 0 means
// the directory could not be enumerated. Only comparison against a previous
// value is meaningful.
unsigned long long eliteBindsFingerprint();

// The same walk over an explicit directory, exposed for the smoke test.
unsigned long long eliteBindsFingerprintDir(const wchar_t* dir);

}  // namespace edvr
