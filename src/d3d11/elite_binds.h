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
//
// A third consumer since 2026-09-11: the settings menu reads Elite's ten
// panel-navigation elements (UI_Up/Down/Left/Right/Select/Back and the four
// Cycle*Panel/Page keys) so the panel answers to the keys the player already
// uses in the cockpit. That reader wants every slot of an element, not the
// first watchable one, and it is the only caller allowed to see a modifier
// key (Key_LeftControl...) as a MAIN key: under kEliteKeyAllowModifierMain
// the translation answers "0xA2" where the camera path refuses. The camera
// path never passes the flag -- a bare Ctrl as a camera watch would fire on
// every chord the player types. The slot parser also bounds each slot's
// <Modifier> by the NEXT slot's tag, while parseElementIn (the camera's)
// deliberately scans to the element's end: changing the camera parser would
// change its answer for a bare-Primary/chorded-Secondary element, and that
// answer is pinned by the smoke fixtures.
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
// "Key_SemiColon" -> "SEMICOLON". Returns false for names it cannot map,
// and that includes the six modifier keys as main keys.
bool eliteBindsTranslateKey(const char* eliteKey, char* out, size_t outLen);

// The multi-slot form, for the settings menu.
//
// eliteBindsLookup answers "which ONE key can EDVR watch for this element"
// and stops at the first slot it can name. The menu wants every slot of an
// element as Elite wrote it -- a gamepad Primary beside a keyboard Secondary
// is the shape of every UI_* element in a pad-and-keyboard file -- so it can
// adopt the keyboard ones and say in the log why the others were not.
enum EliteKeyFlags : unsigned {
    kEliteKeyPlain = 0,
    // Admit the six modifier keys as MAIN keys, translated to the raw
    // "0x.." form virtualKeyFromName already parses. Menu only.
    kEliteKeyAllowModifierMain = 1u << 0,
};

struct EliteKeySlot {
    bool keyboard;        // Device="Keyboard"
    bool chorded;         // a keyboard <Modifier> sits between this slot's
                          // tag and the next slot's tag / the element's close
    bool modifierMain;    // the main key is itself a modifier (LeftControl
                          // ...), translated only under the flag
    char binding[32];     // EDVR binding string: "W", "UP", "SHIFT+E",
                          // "0xA2"; empty when not keyboard or unnamed
    char eliteName[32];   // the raw Key_/GamePad_/Mouse_ name as written
};
struct EliteKeySlots {
    bool present;         // the element tag exists in the answering file
    int  count;           // slots found in file order (Primary, Secondary):
                          // 0..2
    EliteKeySlot slot[2];
    int  filesSeen;       // candidate .binds files the walk opened (0 = no
                          // preset or no .binds at all)
    char file[64];        // UTF-8 basename of the answering file, "" when
                          // none
};

// eliteBindsTranslateKey's answer, and under kEliteKeyAllowModifierMain ALSO
// the six modifier keys as MAIN keys, in the raw "0x.." form
// virtualKeyFromName already parses: LeftShift 0xA0, RightShift 0xA1,
// LeftControl 0xA2, RightControl 0xA3, LeftAlt 0xA4, RightAlt 0xA5.
// `modifierMain` (optional) says which answer it was.
bool eliteBindsTranslateKeyEx(const char* eliteKey, unsigned flags, char* out,
                              size_t outLen, bool* modifierMain);

// Every slot of one element from the active preset's files. The first file
// that CONTAINS the element answers alone, as for the camera lookup; there
// is no fallback element. Logs nothing -- the menu prints one summary line
// over all ten elements. Returns `present`; `out` is fully written either
// way (filesSeen tells "no files at all" from "the element is not there").
bool eliteBindsLookupSlots(const char* element, unsigned flags,
                           EliteKeySlots* out);
bool eliteBindsLookupSlotsDir(const wchar_t* dir, const char* element,
                              unsigned flags, EliteKeySlots* out);

// A cheap stamp over the bindings directory: names, sizes and write times of
// its files, folded together. It changes when the player applies a rebind or
// switches preset in-game, which is when Elite rewrites the files. 0 means
// the directory could not be enumerated. Only comparison against a previous
// value is meaningful.
unsigned long long eliteBindsFingerprint();

// The same walk over an explicit directory, exposed for the smoke test.
unsigned long long eliteBindsFingerprintDir(const wchar_t* dir);

}  // namespace edvr
