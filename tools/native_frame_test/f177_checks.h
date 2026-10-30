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

    // ---- 1. the page's words --------------------------------------------------------------------------------------------------------------------------------------------------------------------
    static_assert(ap::kLineChars + 1 == sizeof(edvr::MenuLine::left), "the line holds what MenuLine.left holds");
    static_assert(ap::kPageLinesMax == edvr::kMenuMaxLines, "the page limit is the raster's");
    check(ap::kNoteCount == 12 && ap::kPageLines == 1 + 12 + 3 && ap::kPageLines <= ap::kPageLinesMax,
          "F177: the page is the heading, twelve lines (the last blank, between the words and the rows) and three rows (16), inside the 18 lines one raster holds");
    bool lengthsOk = true;
    size_t longest = 0;
    for (int i = 0; i < ap::kNoteCount; ++i) {
        const size_t n = std::strlen(ap::kNotes[i]);
        if (n > longest) longest = n;
        if (n > static_cast<size_t>(ap::kLineChars)) lengthsOk = false;
    }
    std::snprintf(msg, sizeof(msg), "F177: every line of the page fits the line's 95 characters (the longest is %u)", static_cast<unsigned>(longest));
    check(lengthsOk && longest > 80, msg);
    check(std::strcmp(ap::kHeading, "Aiming on foot") == 0, "F177: the heading reads Aiming on foot");
    check(std::strcmp(ap::kNotes[0], "Two ways to aim on foot. You can change this any time: Native on foot page, Look to aim.") == 0 && std::strlen(ap::kNotes[1]) == 0 &&
              std::strcmp(ap::kNotes[2], "Mouse/controller aim (the game's own):") == 0 && std::strlen(ap::kNotes[6]) == 0 && std::strcmp(ap::kNotes[7], "Look to aim:") == 0 &&
              std::strlen(ap::kNotes[11]) == 0,
          "F177: the opening line, the three blank lines (the last before the rows) and the two section lines are the words given");
    check(std::strcmp(ap::kNotes[3], "  The mouse and right stick aim as in the flat game. Your head looks around on its own.") == 0 &&
              std::strcmp(ap::kNotes[4], "  In the sights your head aims; mouse and stick only turn you (ADS: aim with head).") == 0 &&
              std::strcmp(ap::kNotes[5], "  Precise and familiar. Where you look and where you point can differ.") == 0,
          "F177: the three lines under mouse/controller aim are the words given");
    check(std::strcmp(ap::kNotes[8], "  Your head aims. Where you look is where you face, walk and point, sights included.") == 0 &&
              std::strcmp(ap::kNotes[9], "  The mouse and stick no longer look up or down; they turn you in steps or smoothly.") == 0 &&
              std::strcmp(ap::kNotes[10], "  Aiming means moving your head. For a big turn, snap turn or turn your chair.") == 0,
          "F177: the three lines under look to aim are the words given");
    bool indentsOk = true;
    for (int i : {3, 4, 5, 8, 9, 10}) indentsOk = indentsOk && ap::kNotes[i][0] == ' ' && ap::kNotes[i][1] == ' ' && ap::kNotes[i][2] != ' ';
    for (int i : {0, 2, 7}) indentsOk = indentsOk && ap::kNotes[i][0] != ' ';
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

    // ---- 4. the trigger ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
    {
        ap::Trigger ok;
        ok.keyOn = ok.vr = ok.nativeOnFoot = ok.scenePast = ok.settled = ok.focused = true;
        ok.menuOpen = ok.shownThisSession = false;
        check(ap::decide(ok).verdict == ap::Verdict::Show, "F177: key on, in the headset, native on foot on, past the first scene and settled, menu closed, game focused: shows");
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
        t.scenePast = false;
        t.settled = false;
        check(ap::decide(t).verdict == ap::Verdict::Wait, "F177: before the first rendered scene (launch movie, startup loading) it waits");
        t = ok;
        t.settled = false;
        check(ap::decide(t).verdict == ap::Verdict::Wait, "F177: past the first scene but not settled it waits");
        t = ok;
        t.menuOpen = true;
        check(ap::decide(t).verdict == ap::Verdict::Wait, "F177: with the EDVR menu already open it waits, and opens when it closes");
        t = ok;
        t.focused = false;
        check(ap::decide(t).verdict == ap::Verdict::Wait, "F177: with the game's window not focused it waits");
        // a permanent reason outranks a wait, so the log can say it before the scene arrives; shown outranks everything
        t = ok;
        t.keyOn = false;
        t.scenePast = false;
        t.settled = false;
        t.menuOpen = true;
        check(ap::decide(t).verdict == ap::Verdict::NotShown, "F177: a reason it will not show is told at once, whatever else is still to come");
        t.shownThisSession = true;
        check(ap::decide(t).verdict == ap::Verdict::Wait, "F177: once shown, nothing else is said");
        // every single input off (the ones that wait or refuse), none of them shows
        int showing = 0;
        for (unsigned mask = 0; mask < 256; ++mask) {
            ap::Trigger x;
            x.keyOn = (mask & 1) != 0;
            x.vr = (mask & 2) != 0;
            x.nativeOnFoot = (mask & 4) != 0;
            x.scenePast = (mask & 8) != 0;
            x.settled = (mask & 16) != 0 && x.scenePast;
            x.menuOpen = (mask & 32) != 0;
            x.shownThisSession = (mask & 64) != 0;
            x.focused = (mask & 128) != 0;
            if (ap::decide(x).verdict == ap::Verdict::Show) {
                ++showing;
                check(x.keyOn && x.vr && x.nativeOnFoot && x.scenePast && x.settled && !x.menuOpen && !x.shownThisSession && x.focused, "F177: it shows only when every condition holds");
            }
        }
        check(showing == 1, "F177: of the 256 input combinations exactly one shows");
        check(ap::kSettleMs >= 1000 && ap::kRetryMs > 0 && ap::kRetryMax * ap::kRetryMs >= 5000, "F177: it settles for seconds, not frames, and keeps trying to open for seconds");
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
        check(std::strcmp(ap::kLogShown, "aim choice prompt (F177): shown") == 0 && std::strcmp(ap::kLogPickedMouse, "aim choice prompt (F177): picked mouse aim") == 0 &&
                  std::strcmp(ap::kLogPickedLookToAim, "aim choice prompt (F177): picked look to aim") == 0 && std::strcmp(ap::kLogLater, "aim choice prompt (F177): later") == 0,
              "F177: shown, picked mouse aim, picked look to aim and later are the log's words");
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
        check(count(cpp, "aimPromptTick(now);") == 1 && count(cpp, "const aimprompt::Decision d = aimprompt::decide(t);") == 1 && count(cpp, "if (!s.aimPromptSceneMs && sceneArrived()) s.aimPromptSceneMs = now;") == 1,
              "F177: the VR tick asks the pure decision once a tick, with the first rendered scene as its 'past loading'");
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
