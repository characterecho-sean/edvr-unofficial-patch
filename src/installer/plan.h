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
#include "../common/iniedit.h"
#include "probe.h"
#include "state.h"

namespace edvr::installer {

// What this installer carries. Sizes and hashes come from the embedded
// resources; the planner needs them to tell "already installed" from "an older
// build" without reading the payload itself.
struct PayloadInfo {
    std::string version;
    std::string descriptorText;
    std::string descriptorSha;
    bool        haveD3d11 = false;
    bool        nativeGraphicsValid = false;
    std::string d3d11Sha;
    bool        haveOpenvr = false;
    std::string openvrSha;
    bool        haveNgx = false;   // NVIDIA's DLSS runtime, nvngx_dlss.dll
    std::string ngxSha;
    bool        haveXess = false;
    std::string xessSha, xessLicenseSha, xessThirdPartySha;
    bool        nativePairValid = false;
    bool        haveOpenxrLoader = false;
    std::string openxrLoaderSha;
    bool        haveOpenxrLicense = false;
    std::string openxrLicenseSha;
    // The shipped default settings, one text per settings file (state.h: settingsLeafFor
    // names each). The VR edition carries edvr.ini; both editions carry edvr-flat.ini. The
    // scope the install was asked for decides which of these it writes.
    bool        haveIni = false;      // edvr.ini
    std::string iniText;
    bool        haveFlatIni = false;  // edvr-flat.ini
    std::string flatIniText;
};

// The folder as it is right now.
struct Survey {
    GameInstall game;

    // Elite Dangerous, and whether the copy running is this folder's. Two
    // separate facts because they lead to opposite places: one stops the run,
    // the other is only worth saying out loud. A rig with two installs plays
    // one while the other is patched, and a refusal that went by the
    // executable's name alone stopped the folder nobody was in.
    bool gameRunningHere = false;       // from this folder: nothing here can be written
    bool gameRunningElsewhere = false;  // from another install: this folder is free
    // The process list could not be read, so whether the game is running is not
    // known. Refused like gameRunningHere -- a check that failed is not a check
    // that passed -- but with its own words, since closing the game will not
    // fix it.
    bool gameRunStateUnknown = false;

    DllInfo              d3d11;       // <game>\d3d11.dll
    DllInfo              openxrLoader; // <game>\Openvr\win64\openxr_loader.dll
    DllInfo              nativeConfig; // edvr_openxr.ini
    // edvr_openxr.ini's text, read only when the file is present: a file is EDVR's by what it says as well as by its hash (planner).
    std::string          nativeConfigText;
    DllInfo              openxrLicense; // Khronos notice
    std::vector<DllInfo> otherD3d11;  // d3d11_*.dll beside it: chain targets, ours or theirs
    bool                 iniPresent = false;
    bool                 descriptorPresent = false;
    std::string          descriptorSha;
    // <game>\edvr.ini: the VR profile's settings, and -- only until edvr-flat.ini
    // exists -- what the flat runtime reads as well.
    std::string iniText;
    // <game>\edvr-flat.ini: the flat profile's own settings. Two files, so a flat
    // operation and a VR one never touch each other's tuning.
    bool        flatIniPresent = false;
    std::string flatIniText;
    // The shipped defaults each settings file was last merged against: edvr.ini.base
    // and edvr-flat.ini.base (state.h). Each is only as good as the install that
    // wrote it; the planner decides whether it describes the file (planInstall).
    std::string baseIniText;
    std::string flatBaseIniText;

    // NVIDIA's DLSS runtime beside the game, and whether this machine has a
    // card for it: an NVIDIA adapter by DXGI's vendor id. A machine with an
    // AMD integrated GPU beside an RTX card counts, and the rig this was
    // built on is one.
    DllInfo      ngx;            // <game>\nvngx_dlss.dll
    DllInfo      xess, xessLicense, xessThirdParty;
    bool         nvidiaAdapter = false;
    std::wstring nvidiaAdapterName;

    bool    haveOpenvrDir = false;
    DllInfo openvrCurrent;  // <openvr>\openvr_api.dll
    DllInfo openvrOrig;     // the renamed original, whatever it is called
    // Always openvr_api_orig.dll; advanced.real_openvr_dll, which used to let
    // a manual install choose another name, was retired with the forwarding
    // proxy it configured for.
    std::wstring openvrOrigName;

    // A genuine OpenVR runtime found in one of our own backup folders, newest
    // first. This is what makes "the original was lost" recoverable rather than
    // a trip through the launcher's file verification.
    std::vector<std::wstring> openvrOrigInBackups;

    // The tool's receipts for this folder (state.h readToolReceipts). They are provenance for the files they list: a file the tool placed
    // is EDVR's, by its hash, whether or not the installer's own record lists it.
    std::vector<ToolReceipt> toolReceipts;
    // The game's original runtime the tool kept beside EDVR's, when the tool's receipt and the file itself prove it (adoptToolOriginal in
    // plan.cpp). Its kind is Absent when nothing is adopted; it is also first in openvrOrigInBackups when it is.
    DllInfo toolOriginal;

    InstallState state;
    // Which Elite Dangerous the folder's executable is; the gate refuses
    // everything but OdysseyQualified and says why per case.
    EliteExeKind eliteKind = EliteExeKind::Unreadable;
    std::wstring eliteFileVersion;  // as the exe's version resource reports it
};

Survey surveyTarget(const GameInstall& game);

// The state record is optional; a canonical descriptor (schema 1 or 2, by its exact
// bytes) still identifies the installed edition after a tool install or a lost
// installer record. Its scope names the edition: both and vr are "vr", flat is "flat".
std::string installedProfile(const Survey& survey);

// The scope the folder's install covers: both, vr or flat, from the record (recordedScope) or, without a record, from a canonical
// descriptor. Empty when the folder has neither.
std::string installedScope(const Survey& survey);

// The scope a descriptor text declares when it is one of EDVR's own canonical texts (both, vr or flat), else empty. installedScope
// reads the same texts from the file on disk; the log bundler reads the file's text.
std::string canonicalDescriptorScope(const std::string& text);

// The scope an install runs when none was asked for: the folder's own, else both.
std::string defaultScope(const Survey& survey);

// The installer record names a different scope than the descriptor on disk: an install older than the files here.
bool installRecordStale(const Survey& survey);

// Whether the folder holds a settings file `profile` would read: its own, or, for
// flat, the shared edvr.ini that the flat runtime falls back to until it has one
// of its own. What "this folder has lost its settings" is asked in terms of.
bool hasSettingsFor(const Survey& survey, const std::string& profile);

struct Options {
    // Which halves to install is not a choice. Both files are the patch: the
    // transition flash fix and Explorer Cam live in openvr_api.dll, and an
    // install with only one of them is a support thread waiting to happen --
    // the log says a fix stood down, and the person reading it has no idea they
    // opted out of it. What this installer carries is what it installs.
    // What the install covers: "both" (VR runtime and both settings files), "vr" (runtime and edvr.ini) or "flat" (edvr-flat.ini,
    // no runtime). Empty means defaultScope: the scope the folder already has, else both.
    std::string scope;
    bool keepSettings = true;   // merge each settings file the scope installs rather than replace it
    bool repair = false;        // rewrite our files even when they look right
    bool removeSettings = false;  // uninstall: delete the installed edition's settings file too
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
    std::string  item;  // "d3d11" | "openvr" | "ngx" for WritePayload
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
    // The settings file this plan works on ("edvr.ini" or "edvr-flat.ini"), for
    // the report to name; empty when the plan touches none.
    std::string              settingsFile;
};

Plan planInstall(const Survey& survey, const Options& options, const PayloadInfo& payload);
// The payload says which OpenXR and XeSS files are EDVR's by their bytes. Without one (an empty PayloadInfo) only the record proves a
// file, which can only keep more files than the full proof would: callers that remove files pass the payload the installer carries.
Plan planUninstall(const Survey& survey, const Options& options, const PayloadInfo& payload = PayloadInfo());

// The one-paragraph summary shown before anything is done.
std::string planSummary(const Plan& plan);

}  // namespace edvr::installer
