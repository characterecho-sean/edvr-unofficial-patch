#include "hotkey.h"

#include <atomic>
#include <cctype>
#include <string>

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "log.h"
#include "pad_names.h"

namespace edvr {

// Does the foreground window belong to this process?
//
// GetAsyncKeyState is global: it reports the key whoever is typing, in
// whatever application. Without this check, Scroll Lock pressed in a browser
// toggled the brightness fix and Pause wrote a camera dump.
//
// THE CHECK IS NO LONGER APPLIED TO EVERY BINDING, and the line it is drawn
// along is whose key it is (2026-08-16):
//
//   * EDVR's OWN controls -- the exposure toggle, the history dump -- keep
//     it. The game does nothing with those keys, so there is no game state to
//     stay in step with and a press typed elsewhere is pure noise.
//
//   * Bindings ADOPTED FROM ELITE'S OWN CONFIGURATION do not. There EDVR is
//     mirroring the game's reading of a key, and Elite takes its input
//     through DirectInput, which keeps delivering while its window is not
//     foreground. Requiring focus made EDVR disagree with the game about
//     which mode the player was in: measured 2026-08-16, a session where the
//     camera key was pressed, the flat panel stopped, the stereo scene came
//     up, and the gate sat at `intent=CLEAR, pressed not yet this session`
//     while it watched the player cycle presets. Worse than losing one entry
//     -- the camera key is a TOGGLE, so a swallowed press inverts every press
//     after it.
//
// The stray-key exposure that reopens is bounded ON ENTRY by the gate rather
// than by this check: intent alone arms nothing, because arming also needs
// the flat panel to have stopped and a stereo scene to be drawing. A camera
// key pressed in a browser while the on-foot screen is up changes nothing
// visible.
//
// EXIT IS NOT BOUNDED THE SAME WAY, and the first version of this note got
// that wrong by claiming a stray press was "recoverable by pressing again".
// It is not, within one camera stay: the exit path needs no render evidence
// at all, and gatePanelRun is zeroed after 90 panel-less frames, so pressing
// again sets intent but cannot re-enter -- entry wants gatePanelRun > 30 and
// only the flat panel returning rebuilds it. A genuine exit self-heals
// because the panel does come back; a stray press while still in the camera
// costs the offset for the rest of that stay. That is the real price of this
// decision, it is larger than the price of the bug it replaces, and it is
// worth fixing in the gate rather than papering over here.
//
// Controllers inherit the right answer for free: an adopted binding is
// game-mirrored by definition, and neither XInput nor DirectInput has a
// concept of focus to consult.
static bool gameHasFocus() {
    HWND fg = GetForegroundWindow();
    if (!fg) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId();
}

// The keyboard. GetAsyncKeyState, unless a rig has put a reader of its own in (hotkey.h).
static HotkeyKeyDownFn g_keyReader = nullptr;
void hotkeySetKeyboardReaderForTest(HotkeyKeyDownFn fn) { g_keyReader = fn; }
static bool rawKeyDown(int vk) {
    return g_keyReader ? g_keyReader(vk) : (GetAsyncKeyState(vk) & 0x8000) != 0;
}

// Which modifiers are physically down right now.
static uint32_t heldMods() {
    uint32_t m = 0;
    if (rawKeyDown(VK_CONTROL)) m |= kHotkeyCtrl;
    if (rawKeyDown(VK_MENU)) m |= kHotkeyAlt;
    if (rawKeyDown(VK_SHIFT)) m |= kHotkeyShift;
    return m;
}

// SUBSET, not equality: the binding's modifiers must be held, and extra ones
// are allowed.
//
// Equality was the first attempt and it is wrong for the job. These are the
// player's OWN Elite bindings, and EDVR is trying to see the same press the
// game sees -- while the player is holding Shift to sprint, or Ctrl for
// something else. Under equality, CTRL+ALT+SPACE with Shift also down does not
// match, EDVR misses the press, and the intent toggle desynchronises for the
// session. Missing a press is the expensive failure here; that is the whole
// class of bug this gate has been chasing.
//
// It also silently changed two settings that predate all of this. Scroll Lock
// and Pause used to fire whatever was held, because nothing looked at
// modifiers at all. Equality quietly made them fire only when bare -- an
// unannounced change to behaviour people already had muscle memory for.
// Subset restores it.
static bool modsSatisfied(uint32_t want, uint32_t held) {
    return (want & ~held) == 0;
}

// Bindings that exist, so a plain one can tell when a combo on the same key is
// the better match.
//
// Subset matching alone lets ONE physical press fire TWO bindings: with SHIFT+F
// and bare F both bound, pressing SHIFT+F satisfies both. For a toggle that
// is not cosmetic: a double fire sets the intent and immediately clears it.
//
// A registry rather than an ordering rule, because the bindings are independent
// objects polled in whatever order the frame loop happens to use, and a rule
// that depends on the combo being polled first would be right only by accident.
//
// COUNTED, since 2026-10-08: an entry is one live Hotkey's binding (two Hotkeys
// on the same key share it), and it goes when the last of them rebinds or is
// destroyed. The registry was append-only while a binding was set once at launch;
// the settings menu's Hotkeys page rebinds them while the game runs, so a stale
// entry would keep suppressing a bare key that a long-gone chord once shadowed,
// and sixteen of them would have starved every later binding.
struct Registered { int vk; uint32_t mods; int refs; };
static Registered g_bindings[16];
static unsigned   g_bindingCount = 0;

static void registerBinding(int vk, uint32_t mods) {
    if (!vk) return;
    for (unsigned i = 0; i < g_bindingCount; ++i) {
        if (g_bindings[i].vk == vk && g_bindings[i].mods == mods) {
            ++g_bindings[i].refs;
            return;
        }
    }
    if (g_bindingCount < 16) g_bindings[g_bindingCount++] = {vk, mods, 1};
}

static void unregisterBinding(int vk, uint32_t mods) {
    for (unsigned i = 0; i < g_bindingCount; ++i) {
        if (g_bindings[i].vk != vk || g_bindings[i].mods != mods) continue;
        if (--g_bindings[i].refs <= 0) {
            g_bindings[i] = g_bindings[--g_bindingCount];
        }
        return;
    }
}

// Is some OTHER binding on this key a strictly better match right now?
static bool betterMatchExists(int vk, uint32_t mine, uint32_t held) {
    for (unsigned i = 0; i < g_bindingCount; ++i) {
        const Registered& b = g_bindings[i];
        if (b.vk != vk || b.mods == mine) continue;
        // More modifiers, all of them held: that binding is what the player
        // pressed, and this one is the accidental subset.
        if (modsSatisfied(b.mods, held) && (b.mods & ~mine) != 0) return true;
    }
    return false;
}

// The pad / joystick state reader (hotkey.h). One pointer, set once by the
// d3d11 half before any hotkey is polled.
static HotkeyHeldFn g_nonKeyboardReader = nullptr;

void hotkeySetNonKeyboardReader(HotkeyHeldFn fn) { g_nonKeyboardReader = fn; }

// Every live Hotkey, so a suspension that lifts can prime them all (hotkey.h). A fixed array of
// pointers and a lock, both constant-initialised: a Hotkey in another file's static storage
// (Explorer Cam's F5) registers itself from its constructor whatever the order the files
// initialise in, and the registry needs nothing from the CRT to be ready.
constexpr int kLiveMax = 128;
static Hotkey*  g_live[kLiveMax];
static SRWLOCK  g_liveLock = SRWLOCK_INIT;

static void liveAdd(Hotkey* h) {
    AcquireSRWLockExclusive(&g_liveLock);
    for (Hotkey*& slot : g_live) {
        if (!slot) {
            slot = h;
            break;
        }
    }
    ReleaseSRWLockExclusive(&g_liveLock);
}

static void liveRemove(Hotkey* h) {
    AcquireSRWLockExclusive(&g_liveLock);
    for (Hotkey*& slot : g_live) {
        if (slot == h) slot = nullptr;
    }
    ReleaseSRWLockExclusive(&g_liveLock);
}

void hotkeysPrimeAll() {
    AcquireSRWLockExclusive(&g_liveLock);
    for (Hotkey* h : g_live) {
        if (h) h->primeToHeldNow();
    }
    ReleaseSRWLockExclusive(&g_liveLock);
}

int hotkeysLiveCount() {
    int n = 0;
    AcquireSRWLockExclusive(&g_liveLock);
    for (const Hotkey* h : g_live) n += h != nullptr;
    ReleaseSRWLockExclusive(&g_liveLock);
    return n;
}

static std::atomic<bool> g_suspended{false};
// Lifting primes first, while still suspended, so no poll can fall between the two.
void hotkeysSuspend(bool on) {
    if (!on && g_suspended.load(std::memory_order_relaxed)) hotkeysPrimeAll();
    g_suspended.store(on, std::memory_order_relaxed);
}
bool hotkeysSuspended() { return g_suspended.load(std::memory_order_relaxed); }

bool hotkeyBindingsEqual(const HotkeyBinding& a, const HotkeyBinding& b) {
    if (a.kind != b.kind) return false;
    switch (a.kind) {
        case HotkeyKind::None: return true;
        case HotkeyKind::Key: return a.vk == b.vk && a.mods == b.mods;
        case HotkeyKind::Pad: return a.padButtons == b.padButtons && a.padTrigger == b.padTrigger;
        case HotkeyKind::Joy: return a.joyDevice == b.joyDevice && a.joyInput == b.joyInput;
    }
    return false;
}

// Is this binding held at this moment? The keyboard answer is the raw key (the
// modifiers are not part of "held": m_down latches the key alone, see
// pressedWith); the others come from the reader.
bool Hotkey::readDownNow() const {
    switch (m_bind.kind) {
        case HotkeyKind::Key: return rawKeyDown(m_bind.vk);
        case HotkeyKind::Pad:
        case HotkeyKind::Joy: return g_nonKeyboardReader && g_nonKeyboardReader(m_bind);
        default: return false;
    }
}

void Hotkey::takeRegistryEntry() {
    if (m_registered || m_bind.kind != HotkeyKind::Key) return;
    registerBinding(m_bind.vk, m_bind.mods);
    m_registered = true;
}

void Hotkey::giveRegistryEntry() {
    if (!m_registered) return;
    unregisterBinding(m_bind.vk, m_bind.mods);
    m_registered = false;
}

Hotkey::Hotkey() { liveAdd(this); }

Hotkey::Hotkey(int vk) {
    liveAdd(this);
    setKey(vk);
}

Hotkey::Hotkey(const Hotkey& o)
    : m_bind(o.m_bind), m_down(o.m_down), m_missedUnfocused(o.m_missedUnfocused),
      m_gameMirrored(o.m_gameMirrored) {
    liveAdd(this);
    if (o.m_registered) takeRegistryEntry();
}

Hotkey& Hotkey::operator=(const Hotkey& o) {
    if (this == &o) return *this;
    giveRegistryEntry();
    m_bind = o.m_bind;
    m_down = o.m_down;
    m_missedUnfocused = o.m_missedUnfocused;
    m_gameMirrored = o.m_gameMirrored;
    if (o.m_registered) takeRegistryEntry();
    return *this;
}

Hotkey::~Hotkey() {
    liveRemove(this);
    giveRegistryEntry();
}

// setKey does not register (it never did): a key set by number is not a binding
// the better-match rule or the menu's adoption check should hear about.
void Hotkey::setKey(int vk, uint32_t mods) {
    HotkeyBinding b;
    if (vk) {
        b.kind = HotkeyKind::Key;
        b.vk = vk;
        b.mods = mods;
    }
    const bool changed = !hotkeyBindingsEqual(b, m_bind);
    giveRegistryEntry();
    m_bind = b;
    if (changed) m_down = readDownNow();
}

void Hotkey::setBinding(const char* name) {
    HotkeyBinding b;
    hotkeyParseBinding(name, &b);
    const bool changed = !hotkeyBindingsEqual(b, m_bind);
    giveRegistryEntry();
    m_bind = b;
    takeRegistryEntry();
    // A new binding starts with its latch at the key's real state. The menu
    // writes the ini while the key it just captured is still down; without
    // this the reload's first poll saw a press that began before the binding
    // existed, and Explorer Cam would have been entered by binding its key.
    if (changed) m_down = readDownNow();
}

bool hotkeyWouldFire(int vk, uint32_t mods, uint32_t held) {
    if (!vk) return false;
    return modsSatisfied(mods, held) && !betterMatchExists(vk, mods, held);
}

int hotkeyRegisteredKeys(int* vks, int max) {
    if (!vks || max <= 0) return 0;
    int n = 0;
    for (unsigned i = 0; i < g_bindingCount && n < max; ++i) vks[n++] = g_bindings[i].vk;
    return n;
}

bool Hotkey::pressed() {
    if (m_bind.kind == HotkeyKind::None) return false;
    // A game-mirrored binding is never filtered by focus; see the note above.
    // A pad or joystick binding takes the same rule: neither device has a
    // window, so "focused" is whether the game has it, as for a key.
    return pressedWith(readDownNow(), m_bind.kind == HotkeyKind::Key ? heldMods() : 0,
                       m_gameMirrored || gameHasFocus());
}

bool Hotkey::pressedWith(bool keyDown, uint32_t held, bool focused) {
    if (m_bind.kind == HotkeyKind::None) return false;
    const bool matches =
        keyDown && (m_bind.kind != HotkeyKind::Key || hotkeyWouldFire(m_bind.vk, m_bind.mods, held));
    const bool fire = matches && focused;
    const bool edge = fire && !m_down && !hotkeysSuspended();

    // A press that matched the binding and was thrown away only because
    // another window had focus is recorded so somebody can be told. Only
    // EDVR's own keys can reach this now -- a game-mirrored binding passes
    // focused=true always -- and it uses !m_down for the same reason the real
    // edge does: one physical press, one report.
    if (matches && !focused && !m_down) m_missedUnfocused = true;

    // m_down latches the RAW key, not whether this binding fired.
    //
    // It used to latch the composite, and that let one physical press produce
    // an edge from a binding that had already been suppressed. Press
    // CTRL+ALT+SPACE with a bare SPACE binding also configured: SPACE is
    // suppressed, so its m_down stays false. Release CTRL and ALT while SPACE
    // is still held and the suppression lifts -- keyDown is still true, m_down
    // is still false, and a fresh edge is minted from a press the player made
    // once and finished with.
    //
    // On a toggle that is not a stray event, it is an inversion: the combo set
    // the intent, and letting go of the modifiers clears it again.
    //
    // Latching the raw key makes the suppression unable to create edges at all,
    // because an edge now requires the key to have been physically UP. It also
    // fixes the same shape for focus: a press made while another window has
    // focus no longer fires the moment focus returns.
    m_down = keyDown;
    return edge;
}

// ---------------------------------------------------------------------------
// Key names. Hoisted out of virtualKeyFromName (2026-10-08) so the settings
// menu can write a captured key back as a name this parser reads to the same
// key: one table, both directions.

namespace {

struct KeyNameEntry { const char* name; int vk; };
// The FIRST name listed for a key is the one a capture writes.
const KeyNameEntry kNamedKeys[] = {
    {"F1", VK_F1},   {"F2", VK_F2},   {"F3", VK_F3},   {"F4", VK_F4},
    {"F5", VK_F5},   {"F6", VK_F6},   {"F7", VK_F7},   {"F8", VK_F8},
    {"F9", VK_F9},   {"F10", VK_F10}, {"F11", VK_F11}, {"F12", VK_F12},
    {"F13", VK_F13}, {"F14", VK_F14}, {"F15", VK_F15}, {"F16", VK_F16},
    {"F17", VK_F17}, {"F18", VK_F18}, {"F19", VK_F19}, {"F20", VK_F20},
    {"F21", VK_F21}, {"F22", VK_F22}, {"F23", VK_F23}, {"F24", VK_F24},
    {"SCROLLLOCK", VK_SCROLL}, {"SCROLL", VK_SCROLL},
    {"PAUSE", VK_PAUSE},       {"NUMLOCK", VK_NUMLOCK},
    {"INSERT", VK_INSERT},     {"HOME", VK_HOME},
    {"END", VK_END},           {"DELETE", VK_DELETE},
    {"PAGEUP", VK_PRIOR},      {"PAGEDOWN", VK_NEXT},
    {"NUMPAD0", VK_NUMPAD0},   {"NUMPAD1", VK_NUMPAD1},
    {"NUMPAD2", VK_NUMPAD2},   {"NUMPAD3", VK_NUMPAD3},
    {"NUMPAD4", VK_NUMPAD4},   {"NUMPAD5", VK_NUMPAD5},
    {"NUMPAD6", VK_NUMPAD6},   {"NUMPAD7", VK_NUMPAD7},
    {"NUMPAD8", VK_NUMPAD8},   {"NUMPAD9", VK_NUMPAD9},
    {"MULTIPLY", VK_MULTIPLY}, {"DIVIDE", VK_DIVIDE},
    {"ADD", VK_ADD},           {"SUBTRACT", VK_SUBTRACT},
    // The arrow keys were missing, and Elite binds the camera-view cycle to
    // one of them by default. Asking for RIGHT fell through to the
    // unrecognised path below, which returned "no key" in silence -- so the
    // feature simply never fired and nothing said why.
    {"RIGHT", VK_RIGHT},       {"LEFT", VK_LEFT},
    {"UP", VK_UP},             {"DOWN", VK_DOWN},
    {"RIGHTARROW", VK_RIGHT},  {"LEFTARROW", VK_LEFT},
    {"UPARROW", VK_UP},        {"DOWNARROW", VK_DOWN},
    {"SPACE", VK_SPACE},       {"TAB", VK_TAB},
    {"ENTER", VK_RETURN},      {"RETURN", VK_RETURN},
    {"BACKSPACE", VK_BACK},    {"ESCAPE", VK_ESCAPE},
    {"ESC", VK_ESCAPE},        {"CAPSLOCK", VK_CAPITAL},
    {"PRINTSCREEN", VK_SNAPSHOT}, {"APPS", VK_APPS},
    {"MENU_KEY", VK_APPS},     {"DECIMAL", VK_DECIMAL},
    {"NUMPADDOT", VK_DECIMAL},
};

// The punctuation row, by NAME. Resolved through the character rather than
// through a hard-coded VK_OEM_* code, because the OEM codes are positions
// on a US keyboard and these names describe CHARACTERS -- on another
// layout the character lives on a different physical key, and the one the
// player actually presses is the one that types it.
struct CharName { const char* name; wchar_t ch; };
const CharName kCharNames[] = {
    {"BACKSLASH", L'\\'},   {"SLASH", L'/'},
    {"LEFTBRACKET", L'['},  {"RIGHTBRACKET", L']'},
    {"SEMICOLON", L';'},    {"APOSTROPHE", L'\''},
    {"QUOTE", L'\''},       {"COMMA", L','},
    {"PERIOD", L'.'},       {"DOT", L'.'},
    {"GRAVE", L'`'},        {"BACKTICK", L'`'},
    {"TILDE", L'`'},        {"MINUS", L'-'},
    {"DASH", L'-'},         {"EQUALS", L'='},
    {"PLUS", L'='},
    // '#' by name, because bare ';' and '#' after "= " are eaten by the
    // ini's own trailing-comment rule -- SEMICOLON and HASH are the
    // reliable spellings in a config value, and the ini says so. (On a
    // UK layout '#' is its own physical key, so it is a real binding.)
    {"HASH", L'#'},         {"POUND", L'#'},
};

// Set while the formatter checks its own output, and by a quiet parse: the
// "not a key name" line is for a person's typo, not for a probe.
thread_local int t_quietKeyNames = 0;
struct QuietScope {
    QuietScope() { ++t_quietKeyNames; }
    ~QuietScope() { --t_quietKeyNames; }
};

bool isVkOem(int vk) {
    return (vk >= 0xBA && vk <= 0xC0) || (vk >= 0xDB && vk <= 0xDF) || vk == 0xE2;
}

}  // namespace

size_t hotkeyKeyName(int vk, char* out, size_t cap) {
    if (!out || cap == 0) return 0;
    char name[24];
    name[0] = 0;
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) {
        snprintf(name, sizeof(name), "%c", vk);
    } else {
        for (const KeyNameEntry& e : kNamedKeys) {
            if (e.vk == vk) {
                snprintf(name, sizeof(name), "%s", e.name);
                break;
            }
        }
    }
    if (!name[0] && isVkOem(vk)) {
        // The character this key types on the active layout, and the first
        // name for it. The round trip below is the check that the layout
        // agrees; a key it cannot name falls through to the raw code.
        const UINT ch = MapVirtualKeyW(static_cast<UINT>(vk), MAPVK_VK_TO_CHAR) & 0x7FFFFFFFu;
        for (const CharName& e : kCharNames) {
            if (static_cast<UINT>(e.ch) == ch) {
                snprintf(name, sizeof(name), "%s", e.name);
                break;
            }
        }
    }
    if (name[0]) {
        QuietScope quiet;
        uint32_t m = 0;
        if (virtualKeyFromName(name, &m) != vk || m != 0) name[0] = 0;
    }
    if (!name[0]) snprintf(name, sizeof(name), "0x%02X", vk & 0xFF);
    snprintf(out, cap, "%s", name);
    return strlen(out);
}

int virtualKeyFromName(const char* name) { return virtualKeyFromName(name, nullptr); }

int virtualKeyFromName(const char* name, uint32_t* mods) {
    if (mods) *mods = 0;
    if (!name || !*name) return 0;
    // A pad or joystick spelling is a hotkey value too, just not a keyboard
    // one: say nothing, the caller that asked for a keyboard key gets "none".
    if (hotkeyLooksNonKeyboard(name)) return 0;

    // Modifiers are PEELED FROM THE FRONT, not split out of the whole string.
    //
    // The old parser split on every '+' and '-' and called the last piece the
    // key -- which made '-', '+' and '=' unbindable as keys, because the
    // characters themselves were separators. A field user bound '\\' and '['
    // (6az), and the punctuation row is exactly where people put camera keys.
    // Peeling instead means a separator only counts when it FOLLOWS a
    // modifier word, so CTRL+ALT+SPACE and CONTROL-MENU-SPACE parse exactly
    // as before while SHIFT+- means shift and the minus key, and a lone '-'
    // is just the minus key.
    //
    // A leading token that is not a modifier still rejects the WHOLE binding
    // (CTRL+WOMBAT+SPACE binds nothing and says so) -- taking the last part
    // and ignoring the rest would turn a typo into a different, live binding.
    {
        std::string s(name);
        uint32_t m = 0;
        size_t pos = 0;
        while (true) {
            // Skip leading spaces, read a word, upper-case it.
            while (pos < s.size() && (s[pos] == ' ' || s[pos] == '\t')) ++pos;
            size_t w = pos;
            while (w < s.size() && (isalpha(static_cast<unsigned char>(s[w])))) ++w;
            std::string word = s.substr(pos, w - pos);
            for (char& c : word) c = static_cast<char>(toupper(c));
            uint32_t bit = 0;
            if (word == "CTRL" || word == "CONTROL") bit = kHotkeyCtrl;
            else if (word == "ALT" || word == "MENU") bit = kHotkeyAlt;
            else if (word == "SHIFT") bit = kHotkeyShift;
            if (!bit) break;
            // A modifier word must be FOLLOWED by a separator to be one --
            // otherwise it is (an attempt at) the key itself.
            size_t after = w;
            while (after < s.size() && (s[after] == ' ' || s[after] == '\t')) ++after;
            if (after >= s.size() || (s[after] != '+' && s[after] != '-')) break;
            m |= bit;
            pos = after + 1;
        }
        if (pos > 0) {
            // Something was peeled: the remainder, trimmed, is the key.
            size_t b = s.find_first_not_of(" \t", pos);
            size_t e = s.find_last_not_of(" \t");
            std::string last = (b == std::string::npos)
                                   ? std::string()
                                   : s.substr(b, e - b + 1);
            if (last.empty()) {
                if (!t_quietKeyNames)
                    Log::get().note("hotkey: \"%s\" has modifiers but no key. "
                                    "Modifiers are CTRL, ALT and SHIFT, joined with "
                                    "'+', and the key comes last -- CTRL+ALT+SPACE.",
                                    name);
                return 0;
            }
            const int vk = virtualKeyFromName(last.c_str(), nullptr);
            if (vk && mods) *mods = m;
            return vk;
        }
    }

    if (name[0] == '0' && (name[1] == 'x' || name[1] == 'X')) {
        return static_cast<int>(strtol(name, nullptr, 16));
    }

    for (const KeyNameEntry& e : kNamedKeys) {
        if (_stricmp(name, e.name) == 0) return e.vk;
    }

    wchar_t toScan = 0;
    for (const CharName& e : kCharNames) {
        if (_stricmp(name, e.name) == 0) { toScan = e.ch; break; }
    }

    // Single printable character: letters and digits map directly; anything
    // else -- '\\', '[', ';', '-', whatever the player's camera key types --
    // goes through the keyboard layout.
    if (!toScan && name[1] == '\0') {
        const char c = name[0];
        if (c >= 'a' && c <= 'z') return c - 'a' + 'A';
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) return c;
        if (c > 0x20 && c < 0x7F) toScan = static_cast<wchar_t>(c);
    }
    if (toScan) {
        // VkKeyScanW asks the ACTIVE layout which key types this character.
        // The low byte is the physical key; the high byte says which
        // modifiers the character itself needs, and that half is deliberately
        // ignored -- a binding names a KEY, so '|' and '\\' are the same
        // binding, and whether to require Shift is what the CTRL/ALT/SHIFT
        // prefix is for. -1 means no key on this layout types it, which
        // falls through to the say-so below rather than binding nothing
        // silently.
        const SHORT scan = VkKeyScanW(toScan);
        if (scan != -1) return scan & 0xFF;
    }
    // A name nobody recognises is NOT the same as no key, and returning 0 for
    // both is what made this invisible: a typo, or a key this table has never
    // heard of, produced a feature that silently never fired. An empty setting
    // is a choice and returns 0 above, without comment; getting here means
    // somebody asked for something specific and did not get it.
    if (!t_quietKeyNames)
        Log::get().note("hotkey \"%s\" is not a key name EDVR knows, so nothing is "
                        "bound. Try F1-F24, SCROLLLOCK, PAUSE, NUMLOCK, CAPSLOCK, "
                        "PRINTSCREEN, INSERT, HOME, END, DELETE, PAGEUP, PAGEDOWN, "
                        "LEFT, RIGHT, UP, DOWN, SPACE, TAB, ENTER, ESCAPE, "
                        "NUMPAD0-9, DECIMAL, any single character your keyboard "
                        "types (letters, digits, punctuation like \\ [ ] ; ' , . "
                        "/ ` - =), names for those (BACKSLASH, LEFTBRACKET, "
                        "SEMICOLON, ...), or 0x## for a raw virtual-key code. A "
                        "gamepad button is GamePad_Back and the like; a HOTAS or "
                        "joystick button is the device and the button, "
                        "231D0200:Joy_12 (the settings menu's Hotkeys page "
                        "writes these for you).",
                        name);
    return 0;
}

// ---------------------------------------------------------------------------
// Pad and joystick spellings, and the parse/format of a whole hotkey value.

namespace {

bool isHexDigit(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

const char* skipSpaces(const char* s) {
    while (*s == ' ' || *s == '\t') ++s;
    return s;
}

const char* const kPovDirName[4] = {"Up", "Right", "Down", "Left"};

}  // namespace

bool hotkeyJoyInputFromEliteKey(const char* key, uint16_t* input) {
    if (!key || _strnicmp(key, "Joy_", 4) != 0) return false;
    const char* r = key + 4;
    if (_strnicmp(r, "POV", 3) == 0) {
        r += 3;
        if (*r < '1' || *r > '0' + kJoyHatCount) return false;
        const int hat = *r - '1';
        ++r;
        for (int dir = 0; dir < 4; ++dir) {
            if (_stricmp(r, kPovDirName[dir]) == 0) {
                if (input) *input = static_cast<uint16_t>(kJoyPovBase + hat * 4 + dir);
                return true;
            }
        }
        return false;
    }
    // Joy_12: a plain decimal 1..128, no sign, no leading zero, nothing after.
    if (*r < '1' || *r > '9') return false;
    int n = 0;
    for (; *r; ++r) {
        if (*r < '0' || *r > '9') return false;
        n = n * 10 + (*r - '0');
        if (n > kJoyButtonCount) return false;
    }
    if (input) *input = static_cast<uint16_t>(n - 1);
    return true;
}

size_t hotkeyJoyInputName(uint16_t input, char* out, size_t cap) {
    if (!out || cap == 0) return 0;
    out[0] = 0;
    if (input < kJoyButtonCount) {
        snprintf(out, cap, "Joy_%d", input + 1);
    } else if (input < kJoyInputCount) {
        const int k = input - kJoyPovBase;
        snprintf(out, cap, "Joy_POV%d%s", k / 4 + 1, kPovDirName[k % 4]);
    }
    return strlen(out);
}

bool hotkeyJoyDeviceFromText(const char* text, uint32_t* device) {
    if (!text) return false;
    uint32_t v = 0;
    for (int i = 0; i < 8; ++i) {
        if (!isHexDigit(text[i])) return false;
        const char c = text[i];
        v = (v << 4) | static_cast<uint32_t>(c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10);
    }
    if (text[8] != 0 && text[8] != ':') return false;
    if (device) *device = v;
    return true;
}

void hotkeyJoyDeviceText(uint32_t device, char out[9]) {
    snprintf(out, 9, "%08X", device);
}

bool hotkeyLooksNonKeyboard(const char* text) {
    if (!text) return false;
    text = skipSpaces(text);
    if (_strnicmp(text, "GamePad_", 8) == 0) return true;
    for (int i = 0; i < 8; ++i) {
        if (!isHexDigit(text[i])) return false;
    }
    return text[8] == ':';
}

bool hotkeyParseBinding(const char* text, HotkeyBinding* out, bool quiet) {
    HotkeyBinding b;
    bool ok = true;
    if (text) text = skipSpaces(text);
    if (!text || !*text) {
        // Empty: an unbound hotkey, which is a choice and says nothing.
    } else if (_strnicmp(text, "GamePad_", 8) == 0) {
        std::string name(text);
        while (!name.empty() && (name.back() == ' ' || name.back() == '\t')) name.pop_back();
        const PadName* p = padNameFind(name.c_str());
        if (p) {
            b.kind = HotkeyKind::Pad;
            b.padButtons = p->buttons;
            b.padTrigger = p->trigger;
        } else {
            ok = false;
            if (!quiet)
                Log::get().note("hotkey \"%s\" is not a gamepad button EDVR knows, so nothing is "
                                "bound. Try GamePad_FaceDown, _FaceRight, _FaceLeft, _FaceUp, "
                                "_DPadUp, _DPadDown, _DPadLeft, _DPadRight, _Back, _Start, "
                                "_LBumper, _RBumper, _LThumb, _RThumb, _LTrigger or _RTrigger.",
                                name.c_str());
        }
    } else if (hotkeyLooksNonKeyboard(text)) {
        uint32_t device = 0;
        uint16_t input = 0;
        std::string rest(text + 9);
        const size_t first = rest.find_first_not_of(" \t");
        rest = first == std::string::npos ? std::string() : rest.substr(first);
        while (!rest.empty() && (rest.back() == ' ' || rest.back() == '\t')) rest.pop_back();
        if (hotkeyJoyDeviceFromText(text, &device) && hotkeyJoyInputFromEliteKey(rest.c_str(), &input)) {
            b.kind = HotkeyKind::Joy;
            b.joyDevice = device;
            b.joyInput = input;
        } else {
            ok = false;
            if (!quiet)
                Log::get().note("hotkey \"%s\" is not a joystick button EDVR knows, so nothing is "
                                "bound. It is the device Elite writes in your bindings (eight hex "
                                "digits: vendor then product) and a button, 231D0200:Joy_12, or a "
                                "hat, 231D0200:Joy_POV1Up (Right, Down, Left).",
                                text);
        }
    } else {
        uint32_t mods = 0;
        int vk = 0;
        if (quiet) {
            QuietScope scope;
            vk = virtualKeyFromName(text, &mods);
        } else {
            vk = virtualKeyFromName(text, &mods);
        }
        if (vk) {
            b.kind = HotkeyKind::Key;
            b.vk = vk;
            b.mods = mods;
        } else {
            ok = false;
        }
    }
    if (out) *out = b;
    return ok;
}

size_t hotkeyFormatBinding(const HotkeyBinding& b, char* out, size_t cap) {
    if (!out || cap == 0) return 0;
    out[0] = 0;
    switch (b.kind) {
        case HotkeyKind::Key: {
            std::string s;
            if (b.mods & kHotkeyCtrl) s += "CTRL+";
            if (b.mods & kHotkeyAlt) s += "ALT+";
            if (b.mods & kHotkeyShift) s += "SHIFT+";
            char name[24];
            hotkeyKeyName(b.vk, name, sizeof(name));
            s += name;
            snprintf(out, cap, "%s", s.c_str());
            break;
        }
        case HotkeyKind::Pad: {
            const char* name = padNameOf(b.padButtons, b.padTrigger);
            if (name) snprintf(out, cap, "%s", name);
            break;
        }
        case HotkeyKind::Joy: {
            char dev[9];
            char key[24];
            hotkeyJoyDeviceText(b.joyDevice, dev);
            if (hotkeyJoyInputName(b.joyInput, key, sizeof(key))) snprintf(out, cap, "%s:%s", dev, key);
            break;
        }
        default:
            break;
    }
    return strlen(out);
}

}  // namespace edvr
