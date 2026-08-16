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
