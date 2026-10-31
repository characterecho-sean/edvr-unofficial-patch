// F177: the aiming choice at start. Header only, no game or D3D11 state, so a rig runs the very functions the menu uses.
//
// The community is divided on look to aim against mouse aim on foot, so the first start with native on foot VR on asks once. The VR menu opens itself on a page that explains the two and lets
// the player pick one; picking either writes the choice and hides the page for good (menu.aim_choice_prompt = off). "Ask me next time" and Esc close it and write nothing, so it asks again
// at the next start. The key is a row on the Native on foot page (Aiming choice at start) so the page can be turned back on.
//
// WHEN (F188, F190). Once per session, in the headset, with fix.native_on_foot on and menu.aim_choice_prompt on, kSettleMs after the menu panel can first be drawn in (the door) in this stretch of
// the VR profile: Elite's first submitted eye frame, which is the launch idents, the movie or the splash, well before its first rendered scene (frame_flag.h sceneArrived), or, in a session that
// began flat, the first eye frame after the player turned 3D on. The runtime's own frames before that (the headset's home, a loading skybox) have no door for the panel. The game's window has the
// keyboard and the EDVR menu is not already open. Never twice in a session, whatever happens to the page. An open that fails is tried again every kRetryMs; only a failed try made with the
// door ready counts, kRetryMax of them end the tries: waiting for the door (the splash, or the VR runtime coming up after a switch) never does.
// THE FIRST MENU OPEN (F190). The player who opens the EDVR menu (the summon key) in the headset before the page has shown this session, with the key and native on foot on, gets the page in
// place of the menu's usual one. A page already shown this session (picked, "Ask me next time" or Esc) is not shown again that way.
//
// WHAT IT WRITES. One write job with both pairs (a single atomic replace of the file), so a crash cannot leave the choice without the hide, or the hide without the choice.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace edvr {
namespace aimprompt {

constexpr const char* kPageName = "Aiming";
constexpr const char* kHeading = "Welcome to native VR on foot";

// The page's words, one line each (a blank string is a blank line). Indented lines keep their two leading spaces. A line holds at most kLineChars characters (MenuLine.left is 96 bytes).
// The last line is blank: it keeps the choices apart from the words above them.
constexpr const char* kNotes[] = {
    "On foot you are your commander: your body, hands and weapon, all in VR around you.",
    "First, choose how you aim. You can change it any time on the Native on foot page.",
    "",
    "Mouse/controller aim (the game's own):",
    "  The mouse and right stick aim as in the flat game. Your head looks around on its own.",
    "  In the sights your head aims; mouse and stick only turn you (ADS: aim with head).",
    "  Precise and familiar. Where you look and where you point can differ.",
    "",
    "Look to aim:",
    "  Your head aims. Where you look is where you face, walk and point, sights included.",
    "  The mouse and stick no longer look up or down; they turn you in steps or smoothly.",
    "  Aiming means moving your head. For a big turn, snap turn or turn your chair.",
    "",
};
constexpr int kNoteCount = static_cast<int>(sizeof(kNotes) / sizeof(kNotes[0]));

enum Choice : int { kMouseAim = 0, kLookToAim = 1, kLater = 2, kChoiceCount = 3 };
constexpr const char* kRows[kChoiceCount] = {"Use mouse/controller aim", "Use look to aim", "Ask me next time"};
constexpr const char* kFooter = "Up/Down to choose, Enter or UI Select to confirm. Esc asks next time.";

constexpr int kLineChars = 95;      // MenuLine.left holds 95 characters and the terminator
constexpr int kPageLinesMax = 18;   // kMenuMaxLines: what one raster holds
constexpr int kPageLines = 1 + kNoteCount + kChoiceCount;   // the heading, the notes, the three rows

// The card's width in degrees while the page is up (the player's width_degrees is 16..45): wide enough that the words need not be wrapped, with no tooltip strip beside it. The text is
// shrunk to fit it (fitCapPx), never past the floor the panel itself uses.
constexpr float kWidthDeg = 44.0f;
constexpr int   kCapFloorPx = 10;
// How often an open that is not possible yet (no compositor door, no head pose) or failed is tried again, and how many failed tries made with the door ready before the session gives up
// (40 x 250 ms = 10 s). A try made while the door is not ready (the splash, the VR runtime coming up after a live switch to VR) is not a failure and counts nothing.
constexpr uint64_t kRetryMs = 250;
constexpr int      kRetryMax = 40;
// F190: the page opens this long after the door is first ready in this stretch of the VR profile (the first eye frame at game load, the first one after a switch to VR).
constexpr uint64_t kSettleMs = 2000;

// The largest cap height (px) at which the widest note line, `widestEm` ems long in the note face (the note face's em is 8/7 of the cap, the card's padding 0.8 cap each side), fits a
// card cardPx wide, with 4 % over for the face's rounding at small sizes. Never above the cap asked for and never below the floor.
inline int fitCapPx(int cardPx, int capPx, double widestEm) noexcept {
    if (cardPx <= 0 || !(widestEm > 0.0)) return capPx;
    const double perCap = widestEm * (8.0 / 7.0) * 1.04 + 1.6;
    int fit = static_cast<int>(static_cast<double>(cardPx) / perCap);
    if (fit < kCapFloorPx) fit = kCapFloorPx;
    return fit < capPx ? fit : capPx;
}

// What each choice writes: look to aim, then the hide. Later writes nothing.
struct Pair {
    const char* key;
    const char* value;
};
struct Writes {
    int  count = 0;
    Pair pair[2] = {{nullptr, nullptr}, {nullptr, nullptr}};
};
inline Writes writesFor(int choice) noexcept {
    Writes w;
    if (choice != kMouseAim && choice != kLookToAim) return w;
    w.count = 2;
    w.pair[0] = {"fix.native_on_foot_look_to_aim", choice == kLookToAim ? "on" : "off"};
    w.pair[1] = {"menu.aim_choice_prompt", "off"};
    return w;
}

// The row that is lit when the page opens: the player's current setting.
inline int highlightFor(bool lookToAimOn) noexcept { return lookToAimOn ? kLookToAim : kMouseAim; }

// The trigger. Every input is a fact the menu already holds; the decision is pure.
struct Trigger {
    bool keyOn = false;             // menu.aim_choice_prompt
    bool vr = false;                // the VR profile (the flat profile has no such page)
    bool nativeOnFoot = false;      // fix.native_on_foot
    bool menuOpen = false;          // the EDVR menu is already up (or this page is)
    bool shownThisSession = false;  // the page has opened once already
    bool focused = false;           // the game's window has the keyboard
};
enum class Verdict : uint8_t { Show, Wait, NotShown };
struct Decision {
    Verdict     verdict = Verdict::Wait;
    const char* reason = nullptr;   // for NotShown: the words the log says once a session; for Wait: why it waits (not logged)
};
inline Decision decide(const Trigger& t) noexcept {
    if (t.shownThisSession) return {Verdict::Wait, "already shown this session"};
    if (!t.vr) return {Verdict::NotShown, "the flat profile has no such page"};
    if (!t.keyOn) return {Verdict::NotShown, "menu.aim_choice_prompt is off"};
    if (!t.nativeOnFoot) return {Verdict::NotShown, "fix.native_on_foot is off"};
    if (t.menuOpen) return {Verdict::Wait, "the menu is open"};
    if (!t.focused) return {Verdict::Wait, "the game's window does not have the keyboard"};
    return {Verdict::Show, nullptr};
}

// F190: the door, as the tick sees it. `seen` and `sinceMs` are the first tick in this stretch of the VR profile (cleared at a profile switch) on which the door was ready.
struct Door {
    bool     seen = false;
    uint64_t sinceMs = 0;
};
enum class Step : uint8_t { WaitDoor, Settling, Open };
// What the tick does this time round: wait for the door, wait out the settle, or try to open. The first tick with the door ready starts the settle.
inline Step stepFor(Door* door, bool doorReady, uint64_t nowMs) noexcept {
    if (!doorReady) return Step::WaitDoor;
    if (!door->seen) {
        door->seen = true;
        door->sinceMs = nowMs;
    }
    return nowMs - door->sinceMs >= kSettleMs ? Step::Open : Step::Settling;
}

// What the tick does with an open attempt. `failedWithDoor` counts the tries that failed with the door ready; a try without the door is waiting, never counted (F190: it used to be counted after
// the game's first rendered scene, which a session that began flat and turned 3D on had long passed, so the VR runtime coming up used up the ten seconds).
enum class Retry : uint8_t { Opened, TryAgain, GiveUp };
inline Retry afterAttempt(bool opened, bool doorReady, int* failedWithDoor) noexcept {
    if (opened) return Retry::Opened;
    if (doorReady && ++*failedWithDoor >= kRetryMax) return Retry::GiveUp;
    return Retry::TryAgain;
}

// F190: whether the player's first opening of the menu shows the page: every condition of decide() but the two that are about the moment (the menu is closed by definition, and the summon key is
// pressed with the game focused).
inline bool opensOnFirstMenu(const Trigger& t) noexcept {
    Trigger x = t;
    x.menuOpen = false;
    x.focused = true;
    return decide(x).verdict == Verdict::Show;
}

// Which frame the page opened on, for the log: at the first menu open, after the switch to VR, after the game's first rendered scene, or during Elite's splash (its first submitted frame until its
// first rendered scene).
enum class Frame : uint8_t { Splash, AfterScene, AfterSwitch, FirstMenuOpen };
inline Frame frameFor(bool viaMenuKey, bool afterSwitch, bool scenePast) noexcept {
    if (viaMenuKey) return Frame::FirstMenuOpen;
    if (afterSwitch) return Frame::AfterSwitch;
    return scenePast ? Frame::AfterScene : Frame::Splash;
}
inline const char* frameName(Frame f) noexcept {
    switch (f) {
        case Frame::AfterScene: return "after the first scene";
        case Frame::AfterSwitch: return "after the switch to VR";
        case Frame::FirstMenuOpen: return "at the first menu open";
        default: return "during the splash";
    }
}

// The log's lines (every one starts with kLogTag; the --health rule reads them).
constexpr const char* kLogTag = "aim choice prompt (F177): ";
constexpr const char* kLogPickedMouse = "aim choice prompt (F177): picked mouse aim";
constexpr const char* kLogPickedLookToAim = "aim choice prompt (F177): picked look to aim";
constexpr const char* kLogLater = "aim choice prompt (F177): later";
// Shown, and on which frame and how long after the game's process started.
inline int formatShown(char* buf, size_t n, Frame frame, double secondsSinceStart) noexcept {
    return std::snprintf(buf, n, "aim choice prompt (F177): shown %s, %.1f s after the game started", frameName(frame), secondsSinceStart);
}
inline int formatNotShown(char* buf, size_t n, const char* reason) noexcept {
    return std::snprintf(buf, n, "aim choice prompt (F177): not shown: %s", reason ? reason : "?");
}
inline int formatWriteFailed(char* buf, size_t n, const char* err) noexcept {
    return std::snprintf(buf, n, "aim choice prompt (F177): write failed: %s", err && *err ? err : "?");
}
// The write landed: the line that tells a pick that was saved from one whose write never came back.
inline int formatSaved(char* buf, size_t n, const Writes& w, const char* iniName) noexcept {
    if (w.count != 2) return std::snprintf(buf, n, "aim choice prompt (F177): saved nothing");
    return std::snprintf(buf, n, "aim choice prompt (F177): saved %s = %s and %s = %s to %s", w.pair[0].key, w.pair[0].value, w.pair[1].key, w.pair[1].value, iniName ? iniName : "?");
}

}  // namespace aimprompt
}  // namespace edvr
