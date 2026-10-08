// Capturing a hotkey in the settings menu, as pure logic (docs/settings-menu.md,
// "Hotkeys page").
//
// The menu owns the glue (it reads the keyboard, the pad and the joystick table
// into a snapshot every frame while a hotkey row is waiting, and acts on the
// answer); everything that DECIDES lives here, where a rig can drive it with
// synthetic snapshots:
//
//   * HotkeyCapture, the state machine: what the next input means. A keyboard
//     key with the Ctrl/Shift/Alt held at that moment, a pad button or trigger,
//     or a joystick button or hat. Esc cancels, Delete or Backspace clears,
//     and whatever was already down when the capture began is ignored until it
//     has been released -- the Enter that started the capture cannot become the
//     binding it was meant to open.
//   * hotkeyCheckBinding: the checks on a captured binding. A key the menu
//     itself navigates with is refused; an exact copy of another EDVR hotkey is
//     refused, naming which; a press that Elite's own bindings also act on is a
//     WARNING and not a refusal, because these keys are watched and never
//     captured, so the game sees the press as well.
//   * hotkeyPageRows: which generated rows the Hotkeys page shows, by tier.
#pragma once

#include <cstdint>

#include "../common/hotkey.h"
#include "elite_binds.h"
#include "joy_watch.h"
#include "menu_schema.h"

namespace edvr {

// ---- what a capture reads ---------------------------------------------------

// May this virtual key be the main key of a captured hotkey? A whitelist: the
// keys a person can name and press (letters, digits, the F row, the cursor and
// editing keys, the numpad, the punctuation row, the lock keys). The
// modifiers are not keys here -- they make the chord -- and neither are the
// Windows keys, the mouse buttons, the IME and media keys.
bool hotkeyCaptureKeyEligible(int vk);

struct CaptureSnapshot {
    uint8_t     keyDown[256] = {};   // by virtual key; only eligible keys are read
    uint32_t    mods = 0;            // kHotkeyCtrl | kHotkeyAlt | kHotkeyShift held now
    uint16_t    padButtons = 0;      // XInput wButtons, every connected pad ORed
    uint8_t     padTriggers = 0;     // bit 0 left trigger, bit 1 right, past the threshold
    JoySnapshot joy;
};

enum class CaptureKind : uint8_t {
    Waiting,       // nothing new yet
    Cancelled,     // a bare Esc
    Cleared,       // a bare Delete or Backspace, and the row may be cleared
    ClearRefused,  // ...and it may not (the menu's own key)
    Captured,      // `binding` is the answer
};

struct CaptureStep {
    CaptureKind   kind = CaptureKind::Waiting;
    HotkeyBinding binding;
};

class HotkeyCapture {
public:
    // Start waiting. Everything held in `now` is parked: it counts only after
    // it has been seen released.
    void begin(const CaptureSnapshot& now);
    void end() { m_active = false; }
    bool active() const { return m_active; }
    // One look. `canClear` is false for hotkey.menu. The step does NOT end the
    // capture; the caller does, on anything but Waiting.
    CaptureStep step(const CaptureSnapshot& now, bool canClear);

private:
    bool            m_active = false;
    CaptureSnapshot m_prev;
};

// ---- what a captured binding is checked against -----------------------------

// Another EDVR hotkey, by its dotted ini name, with the value it holds now. The
// caller lists every hotkey row but the one being captured, the developer ones
// included whether or not they are shown: a hidden hotkey fires all the same.
struct HotkeyOther {
    const char*   dotted;
    HotkeyBinding binding;
};

enum class BindVerdict : uint8_t {
    Ok,
    Duplicate,   // exactly another EDVR hotkey's binding: refused, `duplicateOf` says which
    Reserved,    // a key the menu navigates with: refused
};

// Where Elite's bindings can matter for the hotkey being set. The menu key and
// the diagnostic keys work wherever the player is; Explorer Cam's is pressed on
// foot, and a ship control that shares the key does nothing there.
enum class ClashScope : uint8_t { AnyContext, OnFoot };

struct BindCheck {
    BindVerdict verdict = BindVerdict::Ok;
    const char* duplicateOf = "";
    int         clashCount = 0;        // Elite bindings the same press also triggers
    char        clashList[160] = "";   // "HumanoidJump, HumanoidSprint (+2 more)"
};

// Would a press of `a` also be a press of `b`? Keyboard: the same key, unless
// each side holds a modifier the other lacks (SHIFT+F5 and CTRL+F5 are two
// different presses); pad and joystick: the same button, trigger or hat
// direction. Pure.
bool hotkeyClashes(const HotkeyBinding& a, const HotkeyBinding& b);

// Is this Elite element one the player's bindings use on foot (or everywhere)?
// By name: the on-foot controls are the Humanoid ones, plus the panel and
// camera elements that are live there. A ship or SRV control is not.
bool eliteElementIsOnFoot(const char* element);

// A key the menu's own navigation uses (Up, Down, Left, Right, Enter, Space, Tab,
// Page Up, Page Down, Home, End, R, Escape), bare or with Shift alone. With Ctrl
// or Alt held it is a different press and free.
bool hotkeyReservedByMenu(const HotkeyBinding& b);

BindCheck hotkeyCheckBinding(const HotkeyBinding& b, const HotkeyOther* others, int nOthers,
                             const EliteBindUse* uses, int nUses, ClashScope scope);

// ---- deciding what a capture step does to a row -----------------------------

// Explorer Cam's key IS its switch (the key is the only way in, and an empty one
// turns it off), so while a session is on it cannot be changed or cleared from the
// menu: the key being edited is the key that gets the player back out. True for
// that row only, and only while `explorerCamSession` is.
bool hotkeyRowLocked(const char* rowKey, bool explorerCamSession);
// "Leave Explorer Cam (F5) to change its key": the row's words while it is locked.
// `keyText` is the key as the ini holds it ("F5", "231D0200:Joy_12"); empty reads
// "(its key)".
size_t hotkeyLockedText(const char* keyText, char* out, size_t cap);

// Everything a decision needs to know about the row being captured.
struct HotkeyRowContext {
    const char*         rowKey = "";          // "menu", "explorer_cam", "toggle_exposure", ...
    const char*         currentText = "";     // the row's value in the ini now
    const HotkeyOther*  others = nullptr;     // every other hotkey row, hidden ones too
    int                 nOthers = 0;
    const EliteBindUse* uses = nullptr;       // Elite's bindings
    int                 nUses = 0;
    ClashScope          scope = ClashScope::AnyContext;
    bool                explorerCamSession = false;   // explorerCamSessionActive()
};

enum class CaptureOutcome : uint8_t {
    Waiting,           // nothing yet
    Cancelled,         // Esc: the row stays as it was
    Clear,             // write an empty value
    ClearAlready,      // ...but it is empty already
    ClearRefusedMenu,  // the menu key cannot be cleared from the menu
    Locked,            // Explorer Cam's key while a session is on: refused, change or clear
    Reserved,          // a key the menu navigates with: refused
    Duplicate,         // another EDVR hotkey has it: refused, `check.duplicateOf` says which
    Unchanged,         // it is what the row already holds
    Bind,              // write `binding`; `check.clashCount` Elite bindings share the press (a warning)
};

struct CaptureDecision {
    CaptureOutcome outcome = CaptureOutcome::Waiting;
    HotkeyBinding  binding;
    BindCheck      check;
};

// May a capture begin on this row at all? (Not while it is locked.)
bool hotkeyCaptureMayBegin(const HotkeyRowContext& ctx);

// The one place that turns a state machine step into what happens to the row. The
// order is the rule: a cancel is always harmless; a lock beats everything that
// would change the row; the menu key's clear is refused whatever else; the checks
// run on a binding in the order reserved, duplicate; and only then is an
// identical value "unchanged" and anything else a write.
CaptureDecision hotkeyDecide(const CaptureStep& step, const HotkeyRowContext& ctx);

// ---- the Hotkeys page -------------------------------------------------------

// The indices into `rows` of the rows the Hotkeys page shows, in table order:
// every Hotkey row of the Fix tier, and the Advanced tier's only with the
// developer switch on. Returns the count written (at most `max`).
int hotkeyPageRows(const MenuRowDef* rows, int n, bool developer, int* out, int max);

}  // namespace edvr
