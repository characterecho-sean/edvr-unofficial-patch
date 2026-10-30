// F177 checks (native on-foot VR), run177 below. Header only, run by native_frame_test.
//
// F177 (menu_aim_prompt.h): the first start with native on foot VR on asks once whether the player wants look to aim or mouse aim. The rig pins the page's words (the heading, the eleven
// lines, the three rows and the footer: line lengths under the line's 95 characters, the line count inside one raster, the two-space indents kept), the fit of the text to the card, what each
// choice writes (the choice and the hide, in one job; "Ask me next time" nothing), the trigger as a pure decision over every input, the log's lines, and, by source pins, where menu.cpp
// uses them and that the key is declared as a VR-only row and is not a flat key.
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

    // ---- 4. the trigger (F188: no scene and no settling in it; the door is the first Submit) ---------------------------------------------------------------------------------------------------
    {
        ap::Trigger ok;
        ok.keyOn = ok.vr = ok.nativeOnFoot = ok.focused = true;
        ok.menuOpen = ok.shownThisSession = false;
        check(ap::decide(ok).verdict == ap::Verdict::Show,
              "F188: key on, in the headset, native on foot on, menu closed, game focused: shows, with no first scene and no settling time asked for (the splash is the page's time)");
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
        // the retry rule: a try that does not open is made again every 250 ms; before the first scene without end, after it for 40 tries (10 s)
        check(ap::kRetryMs == 250 && ap::kRetryMax == 40 && ap::kRetryMax * ap::kRetryMs == 10000, "F188: the retry is every 250 ms and the limit after the first scene is 40 tries (10 s)");
        int failed = 0;
        check(ap::afterAttempt(true, false, &failed) == ap::Retry::Opened && ap::afterAttempt(true, true, &failed) == ap::Retry::Opened && failed == 0, "F188: an open that works is Opened, before the scene or after it, and counts nothing");
        bool neverGaveUp = true;
        for (int i = 0; i < 100000; ++i) neverGaveUp = neverGaveUp && ap::afterAttempt(false, false, &failed) == ap::Retry::TryAgain;
        check(neverGaveUp && failed == 0, "F188: before the first rendered scene a try that fails is tried again for as long as it takes (100000 failures, none counted)");
        bool tryAgain = true;
        for (int i = 1; i < ap::kRetryMax; ++i) tryAgain = tryAgain && ap::afterAttempt(false, true, &failed) == ap::Retry::TryAgain;
        check(tryAgain && failed == ap::kRetryMax - 1 && ap::afterAttempt(false, true, &failed) == ap::Retry::GiveUp && failed == ap::kRetryMax,
              "F188: after the first scene 39 failed tries are tried again and the 40th gives up");
        int mixed = 0;
        for (int i = 0; i < 30; ++i) ap::afterAttempt(false, true, &mixed);
        for (int i = 0; i < 500; ++i) ap::afterAttempt(false, false, &mixed);
        check(mixed == 30 && ap::afterAttempt(true, true, &mixed) == ap::Retry::Opened, "F188: failures before the scene add nothing to the count after it");
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
        ap::formatShown(line, sizeof(line), false, 5.84);
        check(std::strcmp(line, "aim choice prompt (F177): shown during the splash, 5.8 s after the game started") == 0, "F188: shown on a frame before the first scene says during the splash, and the seconds since the game started");
        ap::formatShown(line, sizeof(line), true, 28.04);
        check(std::strcmp(line, "aim choice prompt (F177): shown after the first scene, 28.0 s after the game started") == 0, "F188: shown after the first scene says so");
        check(std::strcmp(ap::frameName(false), "during the splash") == 0 && std::strcmp(ap::frameName(true), "after the first scene") == 0, "F188: the two frames are named during the splash and after the first scene");
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
        check(count(cpp, "aimPromptTick(now);") == 1 && count(cpp, "const aimprompt::Decision d = aimprompt::decide(t);") == 1 && count(cpp, "sceneArrived()") == 1 && count(cpp, "kSettle") == 0 &&
                  count(cpp, "aimPromptSceneMs") == 0,
              "F188: the VR tick asks the pure decision once a tick with no scene and no settling in it; the first scene is read once, for the retry limit and the line");
        check(count(cpp, "const bool doorReady = (glitchConsumerPresent() || nativeMenuAvailable()) && headPose(headNow);") == 1 && count(cpp, "const bool opened = doorReady && openAimPrompt(now);") == 1 &&
                  count(cpp, "aimprompt::afterAttempt(opened, scenePast, &s.aimPromptFailedAfterScene)") == 1 && count(cpp, "aimprompt::formatShown(line, sizeof(line), scenePast, secondsSinceProcessStart());") == 1,
              "F188: the page opens on the first frame the door (the compositor consumer or the native menu, with a head pose) can draw it in, asked before openMenu so a try before the first Submit says nothing; the shown line names the frame");
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
