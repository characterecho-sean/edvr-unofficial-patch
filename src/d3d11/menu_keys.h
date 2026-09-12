// The settings menu's keys, as rules: the edge-and-repeat tracker every key
// goes through, which of Elite's own panel keys the menu adopts as aliases
// of its actions, and what the panel's bottom line says about them.
//
// WHY A MODULE OF ITS OWN. Everything here is pure -- no key-state read, no
// file, no log, no Hotkey registration -- because each rule is a place a
// headset would otherwise have found the bug, and a flight is the most
// expensive test this project has: an alias polled on a key the menu already
// polls fires two actions; a key held across a page change fires on the page
// it was never pressed on; a footer that does not fit is cut in silence
// (MEASURED 2026-09-11: the old 115-character line was 1520 px in a 770 px
// line at the default width 30 / text 1.1, so "Tab page", the pending count
// and KEYS SHARED WITH THE GAME were never visible, and the budget is
// headset-independent at 25.7 em). menu.cpp bridges the world in --
// elite_binds' slots, GetAsyncKeyState, the gate's evidence, the raster's
// ruler -- and menu_test asserts every rule without one.
//
// THE ALIASES. Elite's ten UI elements, the keys that walk the cockpit
// panels (UI_Up .. CyclePreviousPage, kMenuUiElements in table order), become
// aliases of the menu's OWN actions: the same dispatcher, never a new one,
// and never a Hotkey binding -- hotkey.cpp's registry holds sixteen and the
// d3d11 half already fills it with up to twelve pairs; twenty aliases would
// overflow it silently. menuAliasResolve applies these per slot, in this
// order, and records the reason for every slot it does not take:
//   R9 not a keyboard slot (a gamepad, the mouse)     recorded, never adopted
//   R8 a keyboard key this build cannot name           reported with Elite's name
//   R6 a chord                                         the poll is a bare vk
//   R4 Shift                                           the menu's own modifier
//   R3 the summon key, or one half of its chord        would close what it opens
//   R1 one of the menu's keys with the SAME meaning    the fixed key already does it
//   R2 one of the menu's keys with ANOTHER meaning     the documented meaning wins
//   R5 an EDVR hotkey                                  polled whether or not the menu is open
//   R7 a key an earlier slot already took              first in table order wins
//   else adopted. A modifier used as a plain key (a Ctrl-as-Back) is admitted.
// The invariant the tests pin: every adopted vk is unique, and none is a
// fixed key, Escape, the summon key or a registered hotkey. The typing
// keys are deliberately NOT fixed: a letter is free outside an edit and
// inert inside one, which is what the tracker below is for.
//
// WHEN THEY ACT: menuAliasMayAct -- only while the gate PROVABLY holds the
// game's keyboard (input_gate.h's inputGateHoldsGameKeyboard: the flag, a
// live door, and the game seen reaching it), never while a value is typed,
// never on a status page. Everywhere else an adopted key IS a game key, and
// one press of E on Monitor would cycle the ship's panel AND the menu's page.
// THE ONE EXCEPTION IS THE PAGE PAIR (menuAliasFollowsTab): the next and
// previous tab keys act wherever Tab acts, Monitor and Status included --
// there every key reaches the ship anyway, Tab among them, and a player
// parked on Monitor with Q/E in their fingers must be able to leave it
// (asked for after the first flight, 2026-09-11). Only typing holds them.
//
// THE TRACKER swallows until release. A key stepped with act=false is
// tracked and parked, so that when the state later allows it neither the
// edge nor the repeat train fires: W held for thrust when the menu opens, Q
// held across the Status page onto Performance, a letter held when a Number
// row opens for typing. Only a fresh press counts.
#pragma once

#include <cstddef>
#include <cstdint>

namespace edvr {

// The menu's actions, as the one dispatcher sees them. Aliases only ever
// emit the first ten; Home, End and Reset exist so the fixed keys go
// through the same switch rather than a second one.
enum MenuNav : uint8_t {
    kNavUp,
    kNavDown,
    kNavLeft,
    kNavRight,
    kNavSelect,
    kNavBack,
    kNavPageNext,
    kNavPagePrev,
    kNavReadOn,
    kNavReadBack,
    kNavHome,
    kNavEnd,
    kNavReset,
    kNavCount
};

// Hold-to-repeat: the first repeat, then the cadence. (Moved here from
// menu.cpp with the tracker, so the test can step it.)
constexpr uint64_t kKeyRepeatFirstMs = 400;
constexpr uint64_t kKeyRepeatMs = 83;
// A tracker's nextMs while it waits for a release: never reached by a clock.
constexpr uint64_t kKeyRepeatParked = UINT64_MAX;

struct KeyRepeat {
    int      vk = 0;
    bool     down = false;
    uint64_t nextMs = 0;
};

// Edge-and-repeat over a raw down state; returns how many presses the key
// delivered this tick (0 or 1). act=false TRACKS the key and SWALLOWS it:
// the tracker is parked until the key is released, so a key held across a
// state change never fires when the state later allows it -- neither the
// edge nor the repeat train. With act=true this is exactly the old
// pollKey for a key that was not held across a swallowed state.
int keyRepeatStep(KeyRepeat& k, bool down, uint64_t now, bool act);

// Seed a tracker from the raw key: a key that is down now is parked until
// released; a key that is up is fresh.
void keyRepeatPrime(KeyRepeat& k, bool down);

// The ten elements, in the order that is also the priority for duplicates
// (R7): Up, Down, Left, Right, Select, Back, PageNext, PagePrev, ReadOn,
// ReadBack. `repeats` says whether holding the key repeats the action:
// the panel-cycling keys are edge-only (BELIEVED: Elite's own tab key does
// not repeat; a held E cycling seven pages at 12 Hz is not what it does in
// the ship).
struct MenuUiElement {
    const char* element;
    MenuNav     nav;
    bool        repeats;
};
constexpr int kMenuUiElementCount = 10;
extern const MenuUiElement kMenuUiElements[kMenuUiElementCount];

// One element as elite_binds reported it, bridged by menu.cpp from its
// EliteKeySlots so this module never reads a file: up to two slots
// (Primary, Secondary, in file order), each with its EDVR binding string
// ("W", "UP", "SHIFT+E", "0xA2"; empty when the key has no name here), the
// raw name as written in the .binds, and the three facts the rules need.
constexpr int kMenuAliasMax = 20;   // 10 elements x 2 slots
struct MenuAliasInput {
    const char* element;
    MenuNav     nav;
    bool        repeats;
    int         slots;
    const char* binding[2];
    const char* eliteName[2];
    bool        keyboard[2];       // Device="Keyboard"
    bool        chorded[2];        // a keyboard <Modifier> sits on the slot
    bool        modifierMain[2];   // the main key is itself Shift/Ctrl/Alt
};

// The menu's own keys, as the rules see them: initKeys' twelve with their
// navs, Escape (menuTick's own edge), the summon chord, and every vk
// hotkey.cpp has registered (hotkeyRegisteredKeys).
struct MenuFixedKeys {
    int      vks[16];
    MenuNav  navs[16];
    int      count;
    int      escapeVk;
    int      summonVk;
    uint32_t summonMods;
    int      registered[16];
    int      registeredCount;
};

enum MenuAliasSkip : uint8_t {
    kSkipNotKeyboard,   // R9
    kSkipUnnamed,       // R8
    kSkipChord,         // R6
    kSkipShiftKey,      // R4
    kSkipSummonKey,     // R3
    kSkipSameAsFixed,   // R1: still feeds the legend's name for its nav
    kSkipFixedWins,     // R2
    kSkipEdvrHotkey,    // R5
    kSkipDuplicate,     // R7
};

struct MenuAlias {
    int     vk;
    MenuNav nav;
    bool    repeats;
    bool    modifierMain;    // a modifier key used as a key: named as such in the log
    char    name[12];        // the display name (menuKeyDisplayName)
    char    element[24];
    char    eliteName[32];   // Elite's own spelling, for the log
};

struct MenuAliasSkipped {
    char          element[24];
    char          name[32];      // the display name; Elite's name when this build has none
    MenuNav       nav;
    MenuAliasSkip reason;
    int           vk;            // 0 when the slot never resolved to one
    MenuNav       fixedNav;      // kSkipFixedWins: what the menu's key on this vk does
    bool          summonChord;   // kSkipSummonKey: a half of the chord, not the key itself
};

struct MenuAliasTable {
    MenuAlias        alias[kMenuAliasMax];
    int              count;
    MenuAliasSkipped skipped[kMenuAliasMax];
    int              skippedCount;
    // The legend's name per nav: the first adopted-or-same-as-fixed slot's
    // display name, else the fixed key's own (Up, Down, Left, Right, Enter,
    // Esc, Tab, Tab, PgDn, PgUp, Home, End, R).
    char             navName[kNavCount][12];
    bool             adopted;   // count > 0
};

// Apply R1-R9 to `n` elements in table order. Pure and total: the same
// inputs give a memcmp-equal table, which is what "read the same as before"
// relies on. `out` is zeroed first, padding included.
void menuAliasResolve(const MenuAliasInput* in, int n, const MenuFixedKeys& fixed,
                      MenuAliasTable* out);

// The one predicate: gate && !editing && !statusPage.
bool menuAliasMayAct(bool gateHoldsGameKeyboard, bool editing, bool statusPage);

// The page pair follows Tab: PageNext and PagePrev are the two navs whose
// aliases act on the shared pages too, and under menu.keyboard = shared,
// exactly as the fixed Tab does there.
bool menuAliasFollowsTab(MenuNav nav);

// The predicate for ONE alias: a page key needs only "not typing"; every
// other nav is menuAliasMayAct.
bool menuAliasMayActNav(MenuNav nav, bool gateHoldsGameKeyboard, bool editing, bool statusPage);

// A key's name as the panel prints it: letters and digits as themselves,
// Backspace, Del, Ins, PgUp, PgDn, Home, End, Tab, Enter, Space, Esc, the
// arrows, L-Shift .. R-Alt, F1..F24, Num 0..9, Num ., Num -; a punctuation
// key as the character it types; anything else as 0x%02X. Never longer than
// nine characters.
void menuKeyDisplayName(int vk, char* out, size_t n);

// For the log's outcome line: "pick W/S, change A/D, select Space, page Q/E
// (also Del), read on Z/C (also Ins), back L-Ctrl (Key_LeftControl, a
// modifier key used as a key; the menu only)". A group is named when any
// of its navs has an adopted or same-as-fixed slot; extra adopted keys for
// a group follow as "(also ...)". Empty when there is nothing to say.
void menuAliasSummary(const MenuAliasTable& t, char* out, size_t n);

// The log's "Skipped:" body: "Space (select; already the menu's key), End
// (page; the menu's last-row key keeps its meaning), ...". Keys this build
// cannot name (kSkipUnnamed) are left out -- the log lists those on their
// own "please report" line.
void menuAliasSkippedText(const MenuAliasTable& t, char* out, size_t n);

// The Status row's value: "W/S A/D Space Q/E Z/C L-Ctrl" -- one name (or
// pair) per group that has an adopted or same-as-fixed slot.
void menuAliasStatusValue(const MenuAliasTable& t, char* out, size_t n);

// The tooltip's action line for a row when aliases are live: "Space or A/D
// changes it." for a Toggle or Choice row, "Space types a value; A/D steps
// it." for a Number or Text row (`typed`). Names from navName.
void menuComposeTipAction(const char* const navName[kNavCount], bool typed, char* out,
                          size_t n);

// The footer's ruler: the single-line pixel width of `utf8` at the footer's
// em, supplied by the raster (menuPanelMeasureLine) or by a test. May be
// null, which means everything fits.
typedef int (*MenuMeasureFn)(const char* utf8, void* ctx);

struct MenuFooterInput {
    bool        editing;
    bool        statusPage;
    const char* pageName;
    bool        privateWanted;   // menu.keyboard = private
    bool        gatePrivate;     // inputGatePrivate() right now
    bool        aliasesLive;     // menuAliasMayAct && adopted: the legend follows this
    bool        adopted;
    const char* navName[kNavCount];   // from MenuAliasTable, or the fixed names
    int         pendingN;
    int         widthPx;
};

// Compose the footer: a legend line, and at most one status line after a
// '\n'. The legend names the live keys (fixed names when the aliases are
// not live -- except the page pair, which follows Tab and is named
// wherever it is adopted, the shared pages included), and drops items in a
// fixed order until it measures within widthPx -- change, pick, select;
// "page" and "close" are never dropped. The status line, by precedence:
// KEYS SHARED WITH THE GAME (private wanted but the gate is not), keys
// shared (<page>) on a status page, keys shared (menu.keyboard) with the
// adopted pick or change pair named as off, the pending-restart count, the
// reminder that the fixed keys still work; dropped when it does not fit.
// At most 159 bytes.
void menuComposeFooter(const MenuFooterInput& in, MenuMeasureFn measure, void* ctx, char* out,
                       size_t n);

}  // namespace edvr
