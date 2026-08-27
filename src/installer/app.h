// The bits both faces of the installer share: the command line, the plain-text
// description of what is in a game folder, and the elevation hand-off.
//
// The window is the product; the command line exists because an installer that
// can only be driven by hand cannot be tested, cannot be scripted by whoever
// packages EDVR for a community, and cannot tell you what it WOULD do without
// doing it (--dry-run, which is how every case in tools/installer_test was
// first checked against a real folder).
#pragma once

#include <windows.h>

#include <string>

#include "plan.h"

namespace edvr::installer {

struct AppArgs {
    enum class Act { None, Install, Repair, Uninstall };

    Act          action = Act::None;
    std::wstring dir;                    // --dir, the game folder
    bool         keepSettings = true;    // --replace-settings
    bool         removeSettings = false; // --remove-settings (uninstall)
    bool         dryRun = false;         // --dry-run
    bool         autorun = false;        // set on the elevated relaunch: already confirmed
    bool         help = false;
    bool         badArg = false;
    std::wstring badArgText;
};

AppArgs parseArgs(int argc, wchar_t** argv);
std::string usageText();

Options optionsFor(const AppArgs& args, bool repair);

// What is in this folder right now, in the words the user needs: which halves
// are installed, which other mod is in the chain, whether the game's original
// runtime is safe.
std::string statusReport(const Survey& survey, const PayloadInfo& payload);

// Everything the plan will do, then everything the merge decided.
std::string planReport(const Plan& plan);

// Does this run need administrator rights for that folder?
bool needsElevationFor(const Survey& survey);

// Relaunch ourselves elevated, carrying the same arguments plus --autorun.
// Returns false if the user declined the UAC prompt.
bool relaunchElevated(const AppArgs& args);

bool isElevated();

int runConsole(const AppArgs& args);
int runGui(HINSTANCE instance, const AppArgs& args);

}  // namespace edvr::installer
