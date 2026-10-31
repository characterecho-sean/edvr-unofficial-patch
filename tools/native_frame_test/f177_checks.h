// F177 checks (native on-foot VR), run177 below. Header only, run by native_frame_test.
//
// F177 (menu_aim_prompt.h): the first start with native on foot VR on asks once whether the player wants look to aim or mouse aim. The rig pins the page's words (the heading, the eleven
// lines, the three rows and the footer: line lengths under the line's 95 characters, the line count inside one raster, the two-space indents kept), the fit of the text to the card, what each
// choice writes (the choice and the hide, in one job; "Ask me next time" nothing), the trigger as a pure decision over every input, the log's lines, and, by source pins, where menu.cpp
// uses them and that the key is declared as a VR-only row and is not a flat key.
//
// F190 adds: the retry rule (waiting for the door never counts; a failure with the door ready does), the 2 s settle from the door's first ready tick, the first-menu-open rule over every input,
// and the shown line's four frame names (during the splash, after the first scene, after the switch to VR, at the first menu open).
#pragma once

#include "../../src/d3d11/menu_aim_prompt.h"
#include "../../src/d3d11/menu_panel.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

namespace f177test {

namespace ap = edvr::aimprompt;

template <class Check>
inline void run177(Check&& check) {
    char msg[512];

    // ---- 1. the page's words (F188: a welcome in front of the choice) -----------------------------------------------------------------------------------------------------------------------------
    static_assert(ap::kLineChars + 1 == sizeof(edvr::MenuLine::left), "the line holds what MenuLine.left holds");
    static_assert(ap::kPageLinesMax == edvr::kMenuMaxLines, "the page limit is the raster's");
    check(ap::kNoteCount == 13 && ap::kPageLines == 1 + 13 + 3 && ap::kPageLines == 17 && ap::kPageLines <= ap::kPageLinesMax,
          "F188: the page is the heading, thirteen lines (the last blank, between the words and the rows) and three rows (17), inside the 18 lines one raster holds");
    bool lengthsOk = true;
    size_t longest = 0;
    for (int i = 0; i < ap::kNoteCount; ++i) {
        const size_t n = std::strlen(ap::kNotes[i]);
        if (n > longest) longest = n;
        if (n >= 88) lengthsOk = false;
    }
    std::snprintf(msg, sizeof(msg), "F188: every line of the page is under 88 characters, so the card's text size does not shrink further (the longest is %u; the line holds 95)", static_cast<unsigned>(longest));
    check(lengthsOk && longest > 80 && longest <= static_cast<size_t>(ap::kLineChars), msg);
    check(std::strcmp(ap::kHeading, "Welcome to native VR on foot") == 0, "F188: the heading reads Welcome to native VR on foot");
    check(std::strcmp(ap::kNotes[0], "On foot you are your commander: your body, hands and weapon, all in VR around you.") == 0 &&
              std::strcmp(ap::kNotes[1], "First, choose how you aim. You can change it any time on the Native on foot page.") == 0 && std::strlen(ap::kNotes[2]) == 0,
          "F188: the welcome is the two lines given, then a blank line");
    check(std::strcmp(ap::kNotes[3], "Mouse/controller aim (the game's own):") == 0 && std::strlen(ap::kNotes[7]) == 0 && std::strcmp(ap::kNotes[8], "Look to aim:") == 0 && std::strlen(ap::kNotes[12]) == 0,
          "F177: the two section lines and the blank lines between and after them are the words given");
    check(std::strcmp(ap::kNotes[4], "  The mouse and right stick aim as in the flat game. Your head looks around on its own.") == 0 &&
              std::strcmp(ap::kNotes[5], "  In the sights your head aims; mouse and stick only turn you (ADS: aim with head).") == 0 &&
              std::strcmp(ap::kNotes[6], "  Precise and familiar. Where you look and where you point can differ.") == 0,
          "F177: the three lines under mouse/controller aim are the words given");
    check(std::strcmp(ap::kNotes[9], "  Your head aims. Where you look is where you face, walk and point, sights included.") == 0 &&
              std::strcmp(ap::kNotes[10], "  The mouse and stick no longer look up or down; they turn you in steps or smoothly.") == 0 &&
              std::strcmp(ap::kNotes[11], "  Aiming means moving your head. For a big turn, snap turn or turn your chair.") == 0,
          "F177: the three lines under look to aim are the words given");
    bool indentsOk = true;
    for (int i : {4, 5, 6, 9, 10, 11}) indentsOk = indentsOk && ap::kNotes[i][0] == ' ' && ap::kNotes[i][1] == ' ' && ap::kNotes[i][2] != ' ';
    for (int i : {0, 1, 3, 8}) indentsOk = indentsOk && ap::kNotes[i][0] != ' ';
    check(indentsOk, "F177: the indented lines keep exactly two leading spaces and the others none");
    check(std::strcmp(ap::kRows[ap::kMouseAim], "Use mouse/controller aim") == 0 && std::strcmp(ap::kRows[ap::kLookToAim], "Use look to aim") == 0 && std::strcmp(ap::kRows[ap::kLater], "Ask me next time") == 0 &&
              ap::kMouseAim == 0 && ap::kLookToAim == 1 && ap::kLater == 2 && ap::kChoiceCount == 3,
          "F177: the three rows are Use mouse/controller aim, Use look to aim, Ask me next time, in that order");
    check(std::strcmp(ap::kFooter, "Up/Down to choose, Enter or UI Select to confirm. Esc asks next time.") == 0 && std::strlen(ap::kFooter) < 160,
          "F177: the footer is the words given and fits the footer buffer (160)");
    // ---- 2. the text's fit to the card --------------------------------------------------------------------------------------------------------------------------------------------------------------
    check(ap::fitCapPx(1600, 40, 0.0) == 40 && ap::fitCapPx(0, 40, 40.0) == 40, "F177: with nothing measured the cap asked for stands");
    check(ap::fitCapPx(1600, 20, 40.0) == 20, "F177: a line that already fits changes nothing (the text is never grown)");
    const int fit = ap::fitCapPx(1600, 60, 41.0);
    std::snprintf(msg, sizeof(msg), "F177: a 41 em line in a 1600 px card shrinks a 60 px cap to %d px, where line and padding take the card (%.0f px of 1600)", fit,
                  fit * (41.0 * (8.0 / 7.0) * 1.04 + 1.6));
    check(fit > 10 && fit < 60 && fit * (41.0 * (8.0 / 7.0) * 1.04 + 1.6) <= 1600.0 && (fit + 1) * (41.0 * (8.0 / 7.0) * 1.04 + 1.6) > 1600.0, msg);
    check(ap::fitCapPx(480, 60, 60.0) == ap::kCapFloorPx, "F177: a card too narrow for any text size stops at the panel's floor");
    check(ap::fitCapPx(900, 30, 41.0) <= ap::fitCapPx(1600, 30, 41.0) && ap::fitCapPx(1600, 60, 50.0) <= ap::fitCapPx(1600, 60, 41.0), "F177: a narrower card or a longer line never gives a larger cap");

    // ---- 3. what each choice writes -----------------------------------------------------------------------------------------------------------------------------------------------------------------
    {
        const ap::Writes mouse = ap::writesFor(ap::kMouseAim);
        check(mouse.count == 2 && std::strcmp(mouse.pair[0].key, "fix.native_on_foot_look_to_aim") == 0 && std::strcmp(mouse.pair[0].value, "off") == 0 &&
                  std::strcmp(mouse.pair[1].key, "menu.aim_choice_prompt") == 0 && std::strcmp(mouse.pair[1].value, "off") == 0,
              "F177: Use mouse aim writes look to aim off and the page off, in one job");
        const ap::Writes lta = ap::writesFor(ap::kLookToAim);
        check(lta.count == 2 && std::strcmp(lta.pair[0].key, "fix.native_on_foot_look_to_aim") == 0 && std::strcmp(lta.pair[0].value, "on") == 0 &&
                  std::strcmp(lta.pair[1].key, "menu.aim_choice_prompt") == 0 && std::strcmp(lta.pair[1].value, "off") == 0,
              "F177: Use look to aim writes look to aim on and the page off, in one job");
        check(ap::writesFor(ap::kLater).count == 0 && ap::writesFor(-1).count == 0 && ap::writesFor(ap::kChoiceCount).count == 0 && ap::writesFor(99).count == 0,
              "F177: Ask me next time, and a row that does not exist, write nothing");
        check(ap::highlightFor(false) == ap::kMouseAim && ap::highlightFor(true) == ap::kLookToAim, "F177: the row lit on open is the player's current setting");
    }

    // ---- 4. the trigger (F188: no scene and no settling in it; the door is the first Submit; F190: the settle is stepFor's, below) ---------------------------------------------------------------------------------------------------
    {
        ap::Trigger ok;
        ok.keyOn = ok.vr = ok.nativeOnFoot = ok.focused = true;
        ok.menuOpen = ok.shownThisSession = false;
        check(ap::decide(ok).verdict == ap::Verdict::Show,
              "F188: key on, in the headset, native on foot on, menu closed, game focused: shows, with no first scene and no settling time asked for of the decision (the settle is the door's: stepFor)");
        ap::Trigger t = ok;
        t.shownThisSession = true;
        check(ap::decide(t).verdict == ap::Verdict::Wait, "F177: never twice in a session");
        t = ok;
        t.keyOn = false;
        ap::Decision d = ap::decide(t);
        check(d.verdict == ap::Verdict::NotShown && d.reason && std::strstr(d.reason, "menu.aim_choice_prompt") != nullptr, "F177: key off: not shown, and the reason names the key");
        t = ok;
        t.vr = false;
        d = ap::decide(t);
        check(d.verdict == ap::Verdict::NotShown && d.reason && std::strstr(d.reason, "flat") != nullptr, "F177: the flat profile: not shown, and the reason says flat");
        t = ok;
        t.nativeOnFoot = false;
        d = ap::decide(t);
        check(d.verdict == ap::Verdict::NotShown && d.reason && std::strstr(d.reason, "fix.native_on_foot") != nullptr, "F177: native on foot off: not shown, and the reason names the key");
        t = ok;
        t.menuOpen = true;
        check(ap::decide(t).verdict == ap::Verdict::Wait, "F177: with the EDVR menu already open it waits, and opens when it closes");
        t = ok;
        t.focused = false;
        check(ap::decide(t).verdict == ap::Verdict::Wait, "F177: with the game's window not focused it waits");
        // a permanent reason outranks a wait, so the log can say it at once; shown outranks everything
        t = ok;
        t.keyOn = false;
        t.menuOpen = true;
        check(ap::decide(t).verdict == ap::Verdict::NotShown, "F177: a reason it will not show is told at once, whatever else is still to come");
        t.shownThisSession = true;
        check(ap::decide(t).verdict == ap::Verdict::Wait, "F177: once shown, nothing else is said");
        // every combination of the six inputs: exactly one shows
        int showing = 0;
        for (unsigned mask = 0; mask < 64; ++mask) {
            ap::Trigger x;
            x.keyOn = (mask & 1) != 0;
            x.vr = (mask & 2) != 0;
            x.nativeOnFoot = (mask & 4) != 0;
            x.menuOpen = (mask & 8) != 0;
            x.shownThisSession = (mask & 16) != 0;
            x.focused = (mask & 32) != 0;
            if (ap::decide(x).verdict == ap::Verdict::Show) {
                ++showing;
                check(x.keyOn && x.vr && x.nativeOnFoot && !x.menuOpen && !x.shownThisSession && x.focused, "F177: it shows only when every condition holds");
            }
        }
        check(showing == 1, "F188: of the 64 input combinations exactly one shows");
        // the retry rule (F190): a try that does not open is made again every 250 ms; only a failed try made with the door ready counts, and 40 of them (10 s) end the tries
        check(ap::kRetryMs == 250 && ap::kRetryMax == 40 && ap::kRetryMax * ap::kRetryMs == 10000, "F190: the retry is every 250 ms and the limit is 40 failed tries made with the door ready (10 s)");
        int failed = 0;
        check(ap::afterAttempt(true, false, &failed) == ap::Retry::Opened && ap::afterAttempt(true, true, &failed) == ap::Retry::Opened && failed == 0,
              "F190: an open that works is Opened, with the door ready or not, and counts nothing");
        bool neverGaveUp = true;
        for (int i = 0; i < 100000; ++i) neverGaveUp = neverGaveUp && ap::afterAttempt(false, false, &failed) == ap::Retry::TryAgain;
        check(neverGaveUp && failed == 0,
              "F190: waiting for the door (at the splash, or while the VR runtime comes up after a switch to VR, however long the game has run) is tried again for as long as it takes: 100000 tries without the door, none counted");
        bool tryAgain = true;
        for (int i = 1; i < ap::kRetryMax; ++i) tryAgain = tryAgain && ap::afterAttempt(false, true, &failed) == ap::Retry::TryAgain;
        check(tryAgain && failed == ap::kRetryMax - 1 && ap::afterAttempt(false, true, &failed) == ap::Retry::GiveUp && failed == ap::kRetryMax,
              "F190: with the door ready 39 failed tries are tried again and the 40th gives up");
        int mixed = 0;
        for (int i = 0; i < 30; ++i) ap::afterAttempt(false, true, &mixed);
        for (int i = 0; i < 500; ++i) ap::afterAttempt(false, false, &mixed);
        check(mixed == 30 && ap::afterAttempt(true, true, &mixed) == ap::Retry::Opened, "F190: tries without the door add nothing to the count of the failed ones made with it");
        // The old rule counted failures after the game's first rendered scene whether or not the door was ready, which a session that began flat has long passed: the VR runtime coming up after a
        // switch used the ten seconds up. A session that waits 12 s for the door (48 tries) and then opens at once must not give up.
        int slow = 0;
        bool waited = true;
        for (int i = 0; i < 48; ++i) waited = waited && ap::afterAttempt(false, false, &slow) == ap::Retry::TryAgain;
        check(waited && slow == 0 && ap::afterAttempt(true, true, &slow) == ap::Retry::Opened, "F190: 12 s of waiting for the door (48 tries) and then an open that works does not give up");
    }
    // ---- 4b. the settle (F190): the page opens kSettleMs after the door is first ready in this stretch of the VR profile ---------------------------------------------------------------------------
    {
        check(ap::kSettleMs == 2000, "F190: the settle is 2000 ms");
        ap::Door door;
        check(ap::stepFor(&door, false, 100) == ap::Step::WaitDoor && !door.seen && ap::stepFor(&door, false, 90000) == ap::Step::WaitDoor && !door.seen,
              "F190: with the door not ready the tick waits for it, and the settle has not started however long that is");
        check(ap::stepFor(&door, true, 5000) == ap::Step::Settling && door.seen && door.sinceMs == 5000, "F190: the first tick with the door ready starts the settle and settles");
        check(ap::stepFor(&door, true, 5250) == ap::Step::Settling && ap::stepFor(&door, true, 6999) == ap::Step::Settling && door.sinceMs == 5000,
              "F190: the settle holds the page for 2 s from that tick (1999 ms is still settling) and does not restart");
        check(ap::stepFor(&door, true, 7000) == ap::Step::Open && ap::stepFor(&door, true, 7250) == ap::Step::Open, "F190: 2000 ms after the door was first ready the page tries to open, and goes on trying");
        check(ap::stepFor(&door, false, 8000) == ap::Step::WaitDoor && ap::stepFor(&door, true, 8250) == ap::Step::Open && door.sinceMs == 5000,
              "F190: a door that drops and comes back keeps the clock of its first ready tick (the settle is once per stretch, not once per frame)");
        door = ap::Door{};
        check(ap::stepFor(&door, true, 20000) == ap::Step::Settling && door.sinceMs == 20000 && ap::stepFor(&door, true, 21999) == ap::Step::Settling && ap::stepFor(&door, true, 22000) == ap::Step::Open,
              "F190: a profile switch clears the door (menuProfileSwitch): the next stretch of the VR profile settles for its own 2 s from its own first ready tick");
        // a door ready at once at 0 ms is not mistaken for a door never seen
        ap::Door early;
        check(ap::stepFor(&early, true, 0) == ap::Step::Settling && early.seen && ap::stepFor(&early, true, 1999) == ap::Step::Settling && ap::stepFor(&early, true, 2000) == ap::Step::Open,
              "F190: a door first ready at tick 0 settles for 2 s too");
    }
    // ---- 4c. the first menu open (F190): the player's own opening of the menu shows the page when the tick has not -----------------------------------------------------------------------------------
    {
        ap::Trigger ok;
        ok.keyOn = ok.vr = ok.nativeOnFoot = true;
        ok.focused = false;
        ok.menuOpen = ok.shownThisSession = false;
        check(ap::opensOnFirstMenu(ok), "F190: key on, in the headset, native on foot on, page not shown this session: the first menu open shows it");
        ap::Trigger t = ok;
        t.shownThisSession = true;
        check(!ap::opensOnFirstMenu(t), "F190: a page already shown this session (picked, Ask me next time or Esc all leave it shown) is not shown again by the menu key");
        t = ok;
        t.keyOn = false;
        check(!ap::opensOnFirstMenu(t), "F190: with menu.aim_choice_prompt off the menu opens as usual");
        t = ok;
        t.nativeOnFoot = false;
        check(!ap::opensOnFirstMenu(t), "F190: with fix.native_on_foot off the menu opens as usual");
        t = ok;
        t.vr = false;
        check(!ap::opensOnFirstMenu(t), "F190: in the flat profile the menu opens as usual");
        int firstOpen = 0;
        for (unsigned mask = 0; mask < 64; ++mask) {
            ap::Trigger x;
            x.keyOn = (mask & 1) != 0;
            x.vr = (mask & 2) != 0;
            x.nativeOnFoot = (mask & 4) != 0;
            x.menuOpen = (mask & 8) != 0;
            x.shownThisSession = (mask & 16) != 0;
            x.focused = (mask & 32) != 0;
            if (ap::opensOnFirstMenu(x)) {
                ++firstOpen;
                check(x.keyOn && x.vr && x.nativeOnFoot && !x.shownThisSession, "F190: the first menu open shows it only with the key on, in the headset, native on foot on and the page not yet shown");
            }
        }
        check(firstOpen == 4, "F190: of the 64 input combinations 4 open the page at the first menu open (the 4 that differ only in the two moments: menu open, game focused)");
    }
    // ---- 5. the log's lines ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------
    {
        char line[400];
        ap::formatNotShown(line, sizeof(line), "menu.aim_choice_prompt is off");
        check(std::strcmp(line, "aim choice prompt (F177): not shown: menu.aim_choice_prompt is off") == 0, "F177: the not-shown line reads as the log's contract");
        ap::formatNotShown(line, sizeof(line), nullptr);
        check(std::strcmp(line, "aim choice prompt (F177): not shown: ?") == 0, "F177: a missing reason prints a mark, not a crash");
        ap::formatWriteFailed(line, sizeof(line), "edvr.ini: access denied");
        check(std::strcmp(line, "aim choice prompt (F177): write failed: edvr.ini: access denied") == 0, "F177: the write-failed line carries the error");
        ap::formatSaved(line, sizeof(line), ap::writesFor(ap::kLookToAim), "edvr.ini");
        check(std::strcmp(line, "aim choice prompt (F177): saved fix.native_on_foot_look_to_aim = on and menu.aim_choice_prompt = off to edvr.ini") == 0,
              "F177: the saved line names both keys and their values, so a pick whose write never came back reads differently");
        ap::formatSaved(line, sizeof(line), ap::writesFor(ap::kLater), "edvr.ini");
        check(std::strcmp(line, "aim choice prompt (F177): saved nothing") == 0, "F177: nothing to save says so");
        check(std::strcmp(ap::kLogPickedMouse, "aim choice prompt (F177): picked mouse aim") == 0 &&
                  std::strcmp(ap::kLogPickedLookToAim, "aim choice prompt (F177): picked look to aim") == 0 && std::strcmp(ap::kLogLater, "aim choice prompt (F177): later") == 0,
              "F177: picked mouse aim, picked look to aim and later are the log's words");
        // F188: shown says on which frame and how long after the game started
        ap::formatShown(line, sizeof(line), ap::Frame::Splash, 5.84);
        check(std::strcmp(line, "aim choice prompt (F177): shown during the splash, 5.8 s after the game started") == 0, "F188: shown on a frame before the first scene says during the splash, and the seconds since the game started");
        ap::formatShown(line, sizeof(line), ap::Frame::AfterScene, 28.04);
        check(std::strcmp(line, "aim choice prompt (F177): shown after the first scene, 28.0 s after the game started") == 0, "F188: shown after the first scene says so");
        ap::formatShown(line, sizeof(line), ap::Frame::AfterSwitch, 61.26);
        check(std::strcmp(line, "aim choice prompt (F177): shown after the switch to VR, 61.3 s after the game started") == 0, "F190: shown after the player turned 3D on says after the switch to VR");
        ap::formatShown(line, sizeof(line), ap::Frame::FirstMenuOpen, 12.0);
        check(std::strcmp(line, "aim choice prompt (F177): shown at the first menu open, 12.0 s after the game started") == 0, "F190: shown by the player's first opening of the menu says at the first menu open");
        check(std::strcmp(ap::frameName(ap::Frame::Splash), "during the splash") == 0 && std::strcmp(ap::frameName(ap::Frame::AfterScene), "after the first scene") == 0 &&
                  std::strcmp(ap::frameName(ap::Frame::AfterSwitch), "after the switch to VR") == 0 && std::strcmp(ap::frameName(ap::Frame::FirstMenuOpen), "at the first menu open") == 0,
              "F190: the four frames are named during the splash, after the first scene, after the switch to VR and at the first menu open");
        // which frame a page opened on: the menu key outranks the switch, which outranks the scene
        bool framesOk = true;
        for (int mask = 0; mask < 8; ++mask) {
            const bool viaKey = (mask & 1) != 0, afterSwitch = (mask & 2) != 0, scenePast = (mask & 4) != 0;
            const ap::Frame want = viaKey ? ap::Frame::FirstMenuOpen : afterSwitch ? ap::Frame::AfterSwitch : scenePast ? ap::Frame::AfterScene : ap::Frame::Splash;
            framesOk = framesOk && ap::frameFor(viaKey, afterSwitch, scenePast) == want;
        }
        check(framesOk, "F190: the frame is the first menu open if the menu key opened it, else after the switch if the stretch began with one, else after the first scene or during the splash");
    }

    // ---- 6. the source: where the menu uses them, and the key's declaration -----------------------------------------------------------------------------------------------------------------------------
    {
        const auto slurp = [](const char* path) {
            std::ifstream f(path, std::ios::binary);
            std::stringstream ss;
            ss << f.rdbuf();
            return ss.str();
        };
        const std::string cpp = slurp("src/d3d11/menu.cpp");
        const std::string ini = slurp("edvr.ini");
        const std::string flat = slurp("src/common/flat_profile_keys.h");
        const auto count = [](const std::string& hay, const std::string& needle) {
            size_t n = 0, at = 0;
            while (!needle.empty() && (at = hay.find(needle, at)) != std::string::npos) {
                ++n;
                at += needle.size();
            }
            return n;
        };
        check(!cpp.empty() && !ini.empty() && !flat.empty(), "F177: menu.cpp, edvr.ini and src/common/flat_profile_keys.h are read (run from the repository root)");
        check(count(cpp, "aimPromptTick(now);") == 1 && count(cpp, "const aimprompt::Decision d = aimprompt::decide(t);") == 1 && count(cpp, "kSettle") == 0 && count(cpp, "aimPromptSceneMs") == 0 &&
                  count(cpp, "aimPromptFailedAfterScene") == 0 && count(cpp, "sceneArrived()") == 2,
              "F190: the VR tick asks the pure decision once a tick; the settle is the header's (stepFor), not the menu's; the first scene is read twice, for the shown line of the tick and of the first menu open, and no longer for the retry limit");
        check(count(cpp, "const bool doorReady = (glitchConsumerPresent() || nativeMenuAvailable()) && headPose(headNow);") == 1 &&
                  count(cpp, "if (aimprompt::stepFor(&s.aimPromptDoor, doorReady, now) != aimprompt::Step::Open) return;") == 1 && count(cpp, "const bool opened = openAimPrompt(now);") == 1 &&
                  count(cpp, "aimprompt::afterAttempt(opened, doorReady, &s.aimPromptFailedWithDoor)") == 1 &&
                  count(cpp, "aimPromptNoteShown(aimprompt::frameFor(false, s.aimPromptAfterSwitch, scenePast));") == 1 &&
                  count(cpp, "aimprompt::formatShown(line, sizeof(line), frame, secondsSinceProcessStart());") == 1,
              "F190: the page opens on the first tick 2 s after the door (the compositor consumer or the native menu, with a head pose) was first ready, asked before openMenu so a try before the first Submit says nothing; only a failed open with the door ready counts; the shown line names the frame");
        check(count(cpp, "if (!aimPromptOnFirstMenuOpen(now)) openMenu(now);") == 1 && count(cpp, "aimPromptOnFirstMenuOpen(now)") == 1 && count(cpp, "if (!aimprompt::opensOnFirstMenu(t)) return false;") == 1 &&
                  count(cpp, "aimPromptNoteShown(aimprompt::frameFor(true, s.aimPromptAfterSwitch, sceneArrived()));") == 1,
              "F190: the summon key of the VR tick (the flat panel's keeps openMenu) asks the pure first-open rule before the usual open, and the shown line says it was the first menu open");
        check(count(cpp, "s.aimPromptDoor = aimprompt::Door{};") == 1 && count(cpp, "s.aimPromptFailedWithDoor = 0;") == 1 && count(cpp, "s.aimPromptAfterSwitch = !runtimeVrProfile();") == 1,
              "F190: a profile switch clears the door, the count of failed tries and notes whether the next stretch of the VR profile began with a switch");
        check(count(cpp, "if (s.aimPromptUp) aimPromptClosed();") == 1 && count(cpp, "const aimprompt::Writes w = aimprompt::writesFor(choice);") == 1 &&
                  count(cpp, "job.destination = WriteDestination::Pairs;") == 1 && count(cpp, "for (int i = 0; i < w.count; ++i) job.pairs.emplace_back(w.pair[i].key, w.pair[i].value);") == 1,
              "F177: every close of the menu ends the page, and a pick is one job holding every pair of writesFor");
        check(count(cpp, "if (choice == aimprompt::kLater) {\n        closeMenu(\"aim choice: ask next time\");\n        return;\n    }") == 1,
              "F177: Ask me next time closes the menu and returns before any write is built");
        check(count(cpp, "moveRowAfter(p, \"menu\", \"aim_choice_prompt\", \"fix\", \"native_on_foot_look_to_aim\");") == 1 && count(cpp, "cfg.getBool(\"menu.aim_choice_prompt\", true)") == 1,
              "F177: the row follows Look to aim on the Native on foot page, and the key is read through the config accessor with the shipped default on");
        check(count(cpp, "if (w.job.destination == WriteDestination::Pairs) {") == 1 && count(cpp, "aimPromptWriteDone(w);") == 1,
              "F177: the writer's completions reach the page's own handler, which says saved or write failed");
        check(count(cpp, "if (n == 0 || s.aimPromptUp) return;") == 1 && count(cpp, "bool tooltipsOn() { return g_s.tooltipDelayS > 0.0f && !g_s.aimPromptWide; }") == 1 &&
                  count(cpp, "float cardWidthDeg() { return g_s.aimPromptWide ? aimprompt::kWidthDeg : g_s.widthDeg; }") == 1 && count(cpp, "cardWidthDeg()") >= 3,
              "F177: while the page is up Tab does not leave it, and its card and its missing tooltip strip are the geometry's too");
        check(count(ini, "\n[menu]\n") + count(ini, "\r\n[menu]\r\n") == 1 && count(ini, "aim_choice_prompt = on") == 1 &&
                  count(ini, "# ui: Aiming choice at start | choices on, off | recommended on | live | menu native_on_foot") == 1,
              "F177: edvr.ini declares menu.aim_choice_prompt = on once, as a live on/off row of the Native on foot page");
        const size_t menuAt = ini.find("\n[menu]"), keyAt = ini.find("aim_choice_prompt = on");
        const size_t nextSection = menuAt == std::string::npos ? std::string::npos : ini.find("\n[", menuAt + 1);
        check(menuAt != std::string::npos && keyAt != std::string::npos && keyAt > menuAt && keyAt < nextSection, "F177: the key sits inside the [menu] section");
        check(flat.find("aim_choice_prompt") == std::string::npos, "F177: the key is not a flat profile key (the flat panel has no such page)");
    }
}

}  // namespace f177test
