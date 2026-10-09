// Edge-triggered hotkey polling.
//
// Polled from the frame loop with GetAsyncKeyState rather than installed as a
// keyboard hook: a low-level hook is a process-wide input tap, which is more
// privilege than a timestamp marker needs and more than this project wants to
// be seen taking. Frame-granularity timing is plenty for annotating a trace.
//
// A hotkey is a KEYBOARD key with optional Ctrl/Shift/Alt, an XInput PAD
// button, or a HOTAS/joystick BUTTON or POV direction (2026-10-08; the in-
// headset menu writes all three, docs/settings-menu.md "Hotkeys page"). The
// keyboard is read here; the other two are read through a reader the d3d11
// half installs (hotkeySetNonKeyboardReader), because their state lives
// there: the pad is polled by xinput_watch, the joystick table is filled from
// the game's own DirectInput reads (joy_watch.h) and never by EDVR's devices.
#pragma once

#include <cstddef>
#include <cstdint>

namespace edvr {

// Modifier flags, ORed. Not Windows' MOD_* values: those are for
// RegisterHotKey, which this deliberately does not use.
enum HotkeyMods : uint32_t {
    kHotkeyCtrl  = 1u << 0,
    kHotkeyAlt   = 1u << 1,
    kHotkeyShift = 1u << 2,
};

enum class HotkeyKind : uint8_t { None = 0, Key, Pad, Joy };

// Joystick inputs are numbered once, for the ini grammar, Elite's .binds and
// the observation table alike: 0..127 are Joy_1..Joy_128, and a POV hat is
// kJoyPovBase + hat * 4 + direction (Up, Right, Down, Left) -- Joy_POV1Up is
// 128. A pad has no such number: it is a button mask or a trigger.
constexpr int      kJoyButtonCount = 128;
constexpr int      kJoyHatCount = 4;
constexpr uint16_t kJoyPovBase = 128;
constexpr uint16_t kJoyInputCount = kJoyPovBase + kJoyHatCount * 4;

// What one hotkey value in edvr.ini says, parsed.
//
//   Key  "F5", "CTRL+ALT+SPACE", "0x78"           vk, mods
//   Pad  "GamePad_Back"                            padButtons / padTrigger
//   Joy  "231D0200:Joy_12", "231D0200:Joy_POV1Up"  joyDevice, joyInput
//
// A joystick's device is written the way Elite writes it in its .binds:
// eight hex digits, the vendor id then the product id ("231D0200" is vendor
// 231D, product 0200), so a value can be compared with a binding of the
// player's as text.
struct HotkeyBinding {
    HotkeyKind kind = HotkeyKind::None;
    int        vk = 0;
    uint32_t   mods = 0;
    uint16_t   padButtons = 0;
    uint8_t    padTrigger = 0;
    uint32_t   joyDevice = 0;   // vendor << 16 | product
    uint16_t   joyInput = 0;
};

bool hotkeyBindingsEqual(const HotkeyBinding& a, const HotkeyBinding& b);

// Parse any hotkey value. True for an empty value (kind None: an empty
// setting is a choice) and for one that names a binding; false, with *out
// None, for text that names nothing. A keyboard name that is not one is said
// in the log (virtualKeyFromName's line) unless `quiet`; so is an unknown pad
// or joystick spelling.
bool hotkeyParseBinding(const char* text, HotkeyBinding* out, bool quiet = false);

// The ini text of a binding, canonical: parse(format(b)) is b for every
// binding the parser can produce. Returns the length written, 0 for None.
size_t hotkeyFormatBinding(const HotkeyBinding& b, char* out, size_t cap);

// A keyboard key's canonical name ("F5", "SPACE", "A", "0x91" for one with no
// name); never fails, and virtualKeyFromName reads the answer back to the same
// key. The punctuation row is written as its NAME (SEMICOLON, HASH...) because
// ';' and '#' after "= " are eaten by the ini's trailing-comment rule.
size_t hotkeyKeyName(int vk, char* out, size_t cap);

// Is this text a pad or joystick spelling, not a keyboard name? The keyboard
// parser answers 0 for these WITHOUT the "not a key name" line.
bool hotkeyLooksNonKeyboard(const char* text);

// Joystick pieces, shared with elite_binds.cpp (the .binds spell the same
// things). "Joy_12" / "Joy_POV1Up" <-> the input number above; false for
// anything else (an axis, "Joy_RZAxis").
bool   hotkeyJoyInputFromEliteKey(const char* key, uint16_t* input);
size_t hotkeyJoyInputName(uint16_t input, char* out, size_t cap);
// "231D0200" <-> vendor << 16 | product. The text is exactly eight hex digits.
bool   hotkeyJoyDeviceFromText(const char* text, uint32_t* device);
void   hotkeyJoyDeviceText(uint32_t device, char out[9]);

// While the settings menu waits for a key, pad button or HOTAS button to bind, every
// hotkey is held still: its latch follows the key as always, but it reports no press.
// Otherwise the very key being chosen would also do its job -- Pause writing a camera
// dump, F5 entering Explorer Cam, the exposure key toggling -- on the way to being
// bound. A press that began while suspended is not a press afterwards either (the
// latch saw it go down). The menu clears it on every path that ends a capture.
//
// LIFTING IT PRIMES EVERY LIVE HOTKEY (the review of 2026-10-08, finding 1). The latches
// are polled at different places in the frame -- the diagnostic keys before the menu,
// Explorer Cam's F5 after it -- so a latch polled after the capture ended saw the
// capture-ending press for the first time with suspension already lifted, and made an
// edge of it: capturing F5 on another row (refused as a duplicate) entered Explorer
// Cam. Priming sets every latch to what is held at that moment (a key, a pad button, a
// HOTAS button alike), so the press that ended the capture is consumed wherever the
// latch sits in the frame, and the next fresh press after a release still fires.
void hotkeysSuspend(bool on);
bool hotkeysSuspended();
// Every live Hotkey takes the held state of its own binding as its latch. hotkeysSuspend(false)
// calls it; it is public so a caller that rebinds many at once can too.
void hotkeysPrimeAll();
// How many Hotkey objects are alive (the rig counts them; the registry holds 128).
int  hotkeysLiveCount();
// The keyboard, read through GetAsyncKeyState unless a reader is set. Only a rig sets one: it lets
// the real Hotkey run in the frame's real poll order against a keyboard it controls.
typedef bool (*HotkeyKeyDownFn)(int vk);
void hotkeySetKeyboardReaderForTest(HotkeyKeyDownFn fn);

// The held-state reader for pad and joystick bindings. Installed once by the
// d3d11 half; until then (and in a rig that does not install one) a pad or
// joystick binding never fires. It answers "is this binding held right now",
// and Hotkey makes the edge, with the same latch and focus rules a key has.
typedef bool (*HotkeyHeldFn)(const HotkeyBinding& b);
void hotkeySetNonKeyboardReader(HotkeyHeldFn fn);

class Hotkey {
public:
    Hotkey();
    // vk is a Windows virtual-key code; 0 disables.
    explicit Hotkey(int vk);
    // The registry of keyboard bindings (hotkeyRegisteredKeys, and the better-
    // match rule) counts LIVE hotkeys: a binding that changes, or a Hotkey that
    // goes away, gives its entry back. It used to be append-only, which was
    // right while a binding was set once at launch; the settings menu rebinds
    // them while the game runs, and sixteen stale entries would have starved it.
    Hotkey(const Hotkey& o);
    Hotkey& operator=(const Hotkey& o);
    ~Hotkey();

    void setKey(int vk) { setKey(vk, 0); }
    void setKey(int vk, uint32_t mods);
    // Parse a config string and take ALL of the answer: the key AND its
    // modifiers, or the pad button, or the joystick input.
    //
    // The two-step form -- setKey(virtualKeyFromName(s)) -- compiles fine and
    // silently discards the modifiers, turning CTRL+ALT+SPACE into a bare
    // SPACE. That is a binding that fires when it should not, which is worse
    // than one that never fires, so there is one call that cannot do it.
    //
    // A binding that CHANGES starts with its edge latch set to whatever is
    // held now: the key pressed to bind it in the settings menu is still down
    // when the ini reloads, and a press that finished before the binding
    // existed must not fire it.
    void setBinding(const char* name);

    // Does this binding MIRROR A GAME ACTION, or is it EDVR's own control?
    //
    // EDVR's own keys (the exposure toggle, the history dump) are only
    // meaningful while the player is in the game, and the game does nothing
    // with them -- so they keep the foreground check, and a press typed in a
    // browser is correctly ignored.
    //
    // A binding read from Elite's own configuration is the opposite case.
    // EDVR is mirroring the game's reading of that key, and Elite takes input
    // through DirectInput while unfocused, so the foreground check made the
    // two disagree about which mode the player was in -- and on a toggle,
    // one swallowed press inverts every press after it (measured 2026-08-16).
    // Mirrored bindings therefore fire regardless of focus; the gate's own
    // render-state conditions are what bound a stray press.
    //
    // Default false: a binding is EDVR's own until something says otherwise,
    // so a new call site cannot pick up the permissive behaviour by accident.
    void setGameMirrored(bool mirrored) { m_gameMirrored = mirrored; }

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

    // The keyboard half of the answer: 0 and 0 for a pad or joystick binding.
    int  key() const { return m_bind.kind == HotkeyKind::Key ? m_bind.vk : 0; }
    uint32_t mods() const { return m_bind.kind == HotkeyKind::Key ? m_bind.mods : 0; }
    // Is anything bound -- a key, a pad button or a joystick input? key() == 0
    // does NOT mean unbound any more.
    bool bound() const { return m_bind.kind != HotkeyKind::None; }
    HotkeyKind kind() const { return m_bind.kind; }
    const HotkeyBinding& binding() const { return m_bind; }

    // The latch takes whatever its binding holds right now (hotkeysPrimeAll, for every live one).
    void primeToHeldNow() { m_down = readDownNow(); }

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
    // them. For a pad or joystick binding `keyDown` is the held state and
    // `held` (the keyboard's modifiers) is ignored.
    bool pressedWith(bool keyDown, uint32_t held, bool focused);

private:
    bool readDownNow() const;
    void takeRegistryEntry();
    void giveRegistryEntry();

    HotkeyBinding m_bind;
    bool          m_down = false;
    bool          m_missedUnfocused = false;
    bool          m_gameMirrored = false;
    bool          m_registered = false;   // holds one entry of the registry
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
// and may be null. A pad or joystick spelling is not a keyboard name and
// answers 0 silently.
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

// The virtual keys every registered binding sits on, at most `max` of them;
// returns how many were copied. The settings menu asks before adopting one
// of Elite's panel keys as its own (menu_keys.h, rule R5): a registered
// binding is polled every frame whether or not the menu is open, so a key
// shared with one would fire in two places.
int hotkeyRegisteredKeys(int* vks, int max);

}  // namespace edvr
