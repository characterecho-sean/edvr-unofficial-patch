// A copy of the player's edvr.ini that a game update cannot reach.
//
// Every file this installer manages -- edvr.ini, edvr.ini.base, state.ini, the
// backups in edvr_backup\ -- lives INSIDE the game folder, because that is the
// only folder guaranteed to still be there next time. On 2026-09-02 that
// guarantee failed: an Elite Dangerous update wiped a whole install folder --
// d3d11.dll, openvr_api_orig.dll, edvr.ini, edvr.ini.base, state.ini, every
// edvr_backup\ stamp -- and put back only the files IT owns. Fifty tuned
// settings and hours of field work survived purely because a chat transcript
// happened to have a copy.
//
// So a second copy is kept somewhere a game update has no reason to touch:
// %LOCALAPPDATA%\EDVR\, well outside the game's own folder tree. It is not a
// replacement for edvr_backup\ -- that is what an uninstall restores from, and
// it stays exactly as it was. This is a mirror of last resort, refreshed on
// every install and every settings change, whose only job is to still exist
// when the game folder does not.
#pragma once

#include <string>
#include <vector>

#include "detect.h"

namespace edvr::installer {

// %LOCALAPPDATA%\EDVR. Empty if even the environment variable fallback fails,
// which callers treat as "no mirror today" rather than an error -- a settings
// edit or an install must never fail because its safety net could not be
// found.
std::wstring defaultMirrorRoot();

// <root>\<leaf of game.dir>-<store>, e.g.
// ...\EDVR\elite-dangerous-odyssey-64-steam
//
// The leaf alone is not enough to name the mirror: Frontier's own Products\
// folder name is baked into the build and identical whether the game came
// from the Frontier launcher, Steam or Epic, and one machine having more than
// one of those installed is not a hypothetical -- it is what this feature was
// written on. The store name is what keeps two installs from overwriting one
// mirror.
std::wstring mirrorDirFor(const GameInstall& game,
                          const std::wstring& root = defaultMirrorRoot());

// What the mirror currently holds, read back for a restore offer.
struct MirrorInfo {
    std::wstring dir;
    bool         hasIni = false;
    bool         hasBaseIni = false;
    bool         hasState = false;
    bool         hasBackupPair = false;  // at least one of d3d11.dll / openvr_api.dll
    std::string  savedUtc;               // the mirrored edvr.ini's own last-write time
};

MirrorInfo readMirror(const std::wstring& mirrorDir);

// What updateMirror/updateMirrorIni actually copied, in words fit for the
// report: "edvr.ini", "edvr.ini.base", and so on. Empty when there was
// nothing to mirror yet (a folder with no edvr.ini at all).
struct MirrorResult {
    bool                     ok = false;
    std::vector<std::string> saved;
};

// Snapshots edvr.ini, edvr.ini.base and state.ini, plus -- when backupDir
// carries them -- the d3d11.dll and openvr_api.dll this run just backed up
// (the pair that lets a wiped folder recover the game's ORIGINAL runtime
// without a trip through the launcher's file verification). Called after
// every successful install and repair; backupDir is the plan's own backup
// folder for that run, so this never guesses which stamp was newest.
MirrorResult updateMirror(const std::wstring& gameDir, const std::wstring& backupDir,
                          const std::wstring& mirrorDir);

// Snapshots edvr.ini alone. Called after every settings-window change: the
// install record and the DLL backups do not move there, and re-copying them
// on every toggle would be needless disk I/O for files that did not change.
MirrorResult updateMirrorIni(const std::wstring& gameDir, const std::wstring& mirrorDir);

// Copies the mirror back into a game folder that has no edvr.ini of its own:
// edvr.ini to its root, edvr.ini.base and state.ini into edvr_install\ (so the
// next install still merges as an update instead of starting from scratch),
// and the backup pair into a fresh edvr_backup\restored-<stamp>\ -- exactly
// where the installer's own scan for a genuine original openvr_api.dll
// already looks. Returns false only if the one essential part, edvr.ini,
// could not be restored.
bool restoreFromMirror(const std::wstring& gameDir, const MirrorInfo& info,
                       std::vector<std::string>* notes);

}  // namespace edvr::installer
