// What the installer did last time, written next to the game.
//
// Without a record, every question the installer needs to answer on the second
// run is a guess: did WE rename that d3d11_edhm.dll, or did the user? Is this
// openvr_api_orig.dll the game's original or a leftover? Was the file now
// sitting in our place put there by another mod, or by an older EDVR?
//
// A record turns all of those into facts. It also carries the shipped edvr.ini
// of the installed version -- the base a three-way merge needs to tell "the
// user changed this" from "the default changed".
//
// It is written in the same ini dialect the game reads, for the same reason
// the logs are plain text: somebody diagnosing a broken install should be able
// to open it.
#pragma once

#include <string>

namespace edvr::installer {

struct InstallState {
    bool present = false;  // a record was found and parsed

    std::string  edvrVersion;   // the EDVR build installed
    std::string  installedUtc;  // when, ISO-8601 Z

    // Which Openvr folder was used, RELATIVE to the game folder (Openvr\win64
    // or Openvr). Relative because the reader trims a value at whitespace
    // followed by # or ; -- the game's own ini rule, and one an absolute path
    // through a folder called "Games #2" would fall foul of.
    std::wstring openvrDir;

    bool         d3d11Installed = false;
    std::string  d3d11Sha;       // what we put at d3d11.dll, to spot a clobber
    std::wstring chainTarget;    // the mod we renamed aside, e.g. d3d11_edhm.dll
    std::wstring chainMod;       // its name, e.g. EDHM

    bool         openvrInstalled = false;
    std::string  openvrSha;
    std::wstring openvrOrigName;  // normally openvr_api_orig.dll
    std::string  openvrOrigSha;   // the game's own runtime, as we found it

    bool         ngxInstalled = false;  // NVIDIA's DLSS runtime, the copy we placed
    std::string  ngxSha;

    std::string iniSha;       // the edvr.ini we wrote; a differing file is user edits
    bool        hasBaseIni = false;
};

std::wstring stateDirPath(const std::wstring& gameDir);
std::wstring statePath(const std::wstring& gameDir);
std::wstring baseIniPath(const std::wstring& gameDir);
std::wstring backupRootPath(const std::wstring& gameDir);

InstallState readState(const std::wstring& gameDir);
InstallState parseState(const std::string& text);
std::string  serializeState(const InstallState& state);

// UTC now, as 2026-08-27T14:03:11Z, and a backup folder name from the same
// clock reading.
std::string utcNow();
std::wstring timestampName();

}  // namespace edvr::installer
