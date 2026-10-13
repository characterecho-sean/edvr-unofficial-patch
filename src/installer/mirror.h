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
//
// edvr.ini and edvr-flat.ini are kept as three generations (iniedit.h,
// writeGenerations): <name> the newest, <name>.1 and <name>.2 the two before it.
// An install or repair rotates them, so the settings that stood BEFORE it are
// still there if it wrote defaults over them; a setting changed in the settings
// window or the in-headset menu replaces the newest in place. `hasIni`,
// `hasFlatIni` and the restore go by the newest generation that exists and is
// not empty.
struct MirrorInfo {
    std::wstring dir;
    bool         hasIni = false;         // some generation of edvr.ini
    bool         hasFlatIni = false;     // some generation of edvr-flat.ini, the flat profile's own settings
    bool         hasBaseIni = false;     // edvr.ini.base
    bool         hasFlatBaseIni = false; // edvr-flat.ini.base
    bool         hasState = false;
    bool         hasBackupPair = false;  // at least one of d3d11.dll / openvr_api.dll
    std::string  savedUtc;               // when the newest settings copy was mirrored (edvr.ini's or edvr-flat.ini's, the later)
};

MirrorInfo readMirror(const std::wstring& mirrorDir);

// The settings files a restore into `gameDir` would bring back for `scope` ("both", "vr" or "flat"): each file the scope installs
// (scopeHasVrIni, scopeHasFlatIni) that the folder does not have and the mirror holds, edvr.ini first. Empty means there is nothing
// to offer. A file the folder already has is never in the list, so a restore cannot overwrite it.
std::vector<std::string> settingsToRestore(const std::wstring& gameDir, const MirrorInfo& mirror, const std::string& scope);

// "edvr.ini", or "edvr.ini and edvr-flat.ini", for the names settingsToRestore returns.
std::string settingsNamesOf(const std::vector<std::string>& names);

// What updateMirror/updateMirrorIni actually copied, in words fit for the
// report: "edvr.ini", "edvr.ini.base", and so on. Empty when there was
// nothing to mirror yet (a folder with no edvr.ini at all).
struct MirrorResult {
    bool                     ok = false;
    std::vector<std::string> saved;
};

// Snapshots edvr.ini, edvr-flat.ini, their two merge bases and state.ini, plus -- when backupDir
// carries them -- the d3d11.dll and openvr_api.dll this run just backed up
// (the pair that lets a wiped folder recover the game's ORIGINAL runtime
// without a trip through the launcher's file verification). Called after
// every successful install and repair; backupDir is the plan's own backup
// folder for that run, so this never guesses which stamp was newest.
//
// The checkpoint: an ini that differs from the mirror's newest copy is written
// as the new newest and the copy it replaces is kept as .1 (and .1 as .2, the
// oldest dropped), the new one landing on disk before anything older moves.
// An ini identical to the newest changes nothing, so a run that installed
// nothing does not age the history out. Every file is written through
// writeFileAtomic: a failure leaves what was there.
MirrorResult updateMirror(const std::wstring& gameDir, const std::wstring& backupDir,
                          const std::wstring& mirrorDir);

// Snapshots edvr.ini alone. Called after every settings-window change: the
// install record and the DLL backups do not move there, and re-copying them
// on every toggle would be needless disk I/O for files that did not change.
// Replaces the newest generation in place and leaves .1 and .2 alone: a
// generation per toggle would push the copy worth keeping out in three.
MirrorResult updateMirrorIni(const std::wstring& gameDir, const std::wstring& mirrorDir);

// Copies the mirror back into a game folder that has lost its settings: the
// newest copy of each settings file settingsToRestore names for `scope` -- never
// a file the folder already has -- to the folder's root; edvr.ini.base,
// edvr-flat.ini.base and state.ini into edvr_install\ where the folder lacks them
// (so the next install still merges as an update instead of starting from
// scratch; a present record or base is the current install's and is kept); and the backup pair into
// a fresh edvr_backup\restored-<stamp>\ -- exactly where the installer's own scan
// for a genuine original openvr_api.dll already looks.
//
// Returns false when there is nothing to restore (nothing was attempted), and
// when ANY settings file it set out to copy could not be copied back -- the
// others may have been, and `notes` says which is which, so a caller never
// reports a restore that put back the shared edvr.ini and lost the flat
// profile's file. A failure to restore the record or the backup pair is a note,
// not a failure: the settings are what somebody would miss.
bool restoreFromMirror(const std::wstring& gameDir, const MirrorInfo& info, const std::string& scope,
                       std::vector<std::string>* notes);

}  // namespace edvr::installer
