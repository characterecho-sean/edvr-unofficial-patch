// Deciding what to do, separately from doing it.
//
// Everything the installer will touch is worked out here, from a survey of the
// folder and the record of the last run, and returned as an ordered list of
// steps plus the prose that explains them. Nothing in this file writes
// anything.
//
// That split is not tidiness. The interesting cases -- EDHM already installed
// as d3d11.dll, a game update that put the stock openvr_api.dll back over ours,
// another mod's uninstaller deleting our proxy, a rename that lost the game's
// original runtime -- are exactly the ones that are hard to reproduce on a real
// machine and easy to get wrong. A planner that is a pure function of a Survey
// can have every one of them written down as a test (tools/installer_test), and
// the GUI can show the user the plan before a single file moves.
#pragma once

#include <string>
#include <vector>

#include "detect.h"
#include "iniedit.h"
#include "probe.h"
#include "state.h"

namespace edvr::installer {

// What this installer carries. Sizes and hashes come from the embedded
// resources; the planner needs them to tell "already installed" from "an older
// build" without reading the payload itself.
struct PayloadInfo {
    std::string version;
    bool        haveD3d11 = false;
    std::string d3d11Sha;
    bool        haveOpenvr = false;
    std::string openvrSha;
    std::string iniText;  // the shipped default edvr.ini
};

// The folder as it is right now.
struct Survey {
    GameInstall game;
    bool        gameRunning = false;

    DllInfo              d3d11;       // <game>\d3d11.dll
    std::vector<DllInfo> otherD3d11;  // d3d11_*.dll beside it: chain targets, ours or theirs
    bool                 iniPresent = false;
    std::string iniText;      // the user's edvr.ini
    std::string baseIniText;  // the shipped ini of the installed version, if kept

    bool    haveOpenvrDir = false;
    DllInfo openvrCurrent;  // <openvr>\openvr_api.dll
    DllInfo openvrOrig;     // the renamed original, whatever it is called
    // Normally openvr_api_orig.dll, but advanced.real_openvr_dll lets a manual
    // install choose any name, and a folder set up that way must not be read
    // as one where the original was lost.
    std::wstring openvrOrigName;

    // A genuine OpenVR runtime found in one of our own backup folders, newest
    // first. This is what makes "the original was lost" recoverable rather than
    // a trip through the launcher's file verification.
    std::vector<std::wstring> openvrOrigInBackups;

    InstallState state;
};

Survey surveyTarget(const GameInstall& game);

struct Options {
    // Which halves to install is not a choice. Both files are the patch: the
    // transition flash fix and Explorer Cam live in openvr_api.dll, and an
    // install with only one of them is a support thread waiting to happen --
    // the log says a fix stood down, and the person reading it has no idea they
    // opted out of it. What this installer carries is what it installs.
    bool keepSettings = true;   // merge the existing edvr.ini rather than replace it
    bool repair = false;        // rewrite our files even when they look right
    bool removeSettings = false;  // uninstall: delete edvr.ini too
    std::wstring backupStamp;   // folder name under edvr_backup\; caller supplies the clock
    std::string  nowUtc;        // stamped into the install record
};

enum class Action {
    MakeDir,
    Backup,        // copy `from` to `to`, leaving the original in place
    Rename,        // move `from` to `to`, replacing whatever is at `to`
    WritePayload,  // write embedded item `item` to `to`
    WriteText,     // write `text` to `to`
    Delete,        // remove `from`
};

struct Step {
    Action       action = Action::MakeDir;
    std::wstring from;
    std::wstring to;
    std::string  item;  // "d3d11" | "openvr" for WritePayload
    std::string  text;  // for WriteText
    std::string  why;   // shown to the user, one line

    // What `from` was when the plan was made.
    //
    // A plan is worked out, shown, and then sits in front of a confirmation
    // dialog for as long as somebody takes to read it -- during which another
    // installer can run, a game update can land, or a second copy of this
    // window can do the whole job. Executing a stale plan is how the game's
    // original runtime gets renamed on top of itself. Checked before anything
    // is touched; a mismatch refuses the whole run rather than doing half of
    // it.
    std::string  expectSha;
    // `from` must exist. Without this a source that has gone is skipped in
    // silence, and a run that restored nothing reports success.
    bool         required = false;
};

struct Plan {
    std::vector<Step>        steps;
    std::vector<std::string> notes;     // what will happen and why, in order
    std::vector<std::string> problems;  // things the user has to know about
    bool                     blocked = false;  // nothing will be done until these are fixed
    bool                     nothingToDo = false;
    MergeReport              merge;
    InstallState             nextState;
    std::wstring             backupDir;
};

Plan planInstall(const Survey& survey, const Options& options, const PayloadInfo& payload);
Plan planUninstall(const Survey& survey, const Options& options);

// The one-paragraph summary shown before anything is done.
std::string planSummary(const Plan& plan);

}  // namespace edvr::installer
