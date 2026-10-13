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
#include <vector>

namespace edvr::installer {

struct InstallState {
    bool present = false;  // a record was found and parsed

    std::string  edvrVersion;   // the EDVR build installed
    std::string  installedUtc;  // when, ISO-8601 Z
    std::string  profile = "vr"; // the edition: "flat" for a flat scope, "vr" otherwise (kept for older readers)
    std::string  scope;          // both | vr | flat: what the install covers; read from profile when a record has none
    std::string  descriptorSha;
    std::string  components; // installed component inventory, comma separated

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
    std::string  xessSha, xessLicenseSha, xessThirdPartySha;

    std::string iniSha;       // the edvr.ini we wrote; a differing file is user edits
    bool        hasBaseIni = false;

    bool nativeInstalled = false;
    std::string nativeGraphicsSha;
    std::string nativeRuntimeSha;
    std::string openxrLoaderSha;
    std::string openxrLicenseSha;
    std::string nativeConfigSha;
    std::wstring nativeOriginalName;
    std::string nativeOriginalSha;
};

std::wstring stateDirPath(const std::wstring& gameDir);
std::wstring statePath(const std::wstring& gameDir);
// The shipped default of each settings file, kept as the base of its next merge: edvr.ini.base for edvr.ini, edvr-flat.ini.base for
// edvr-flat.ini. Each is written from its own payload file by the install that manages that file.
std::wstring baseIniPath(const std::wstring& gameDir);
std::wstring flatBaseIniPath(const std::wstring& gameDir);
std::wstring backupRootPath(const std::wstring& gameDir);

// The scope a record names: its scope when it has one; a record from before scopes reads its profile (vr -> both, flat -> flat).
// Empty when neither says anything a scope can be read from.
std::string recordedScope(const InstallState& state);

// The settings files a scope ("both", "vr" or "flat") installs: edvr.ini for vr and both, edvr-flat.ini for flat and both.
bool scopeHasVrIni(const std::string& scope);
bool scopeHasFlatIni(const std::string& scope);

// The settings file an edition owns: edvr.ini for "vr", edvr-flat.ini for "flat".
// The flat runtime reads edvr-flat.ini first and falls back to edvr.ini only
// while there is none (config.cpp), so a flat operation that touched edvr.ini
// would land on the VR profile's tuning, and one that touched only edvr.ini
// would change nothing the flat runtime reads. Anything else is the VR file: a
// record from before the profiles existed says nothing and means VR.
const wchar_t* settingsLeafFor(const std::string& profile);
std::wstring   settingsPathFor(const std::wstring& gameDir, const std::string& profile);

// The tool's receipts (tools\install_edvr.py) for this folder. The tool keeps its own record beside the game: each file it placed, what
// it replaced, and the hashes of both. The installer reads them for provenance only; its own record stays the authority for what it did.
struct ToolReceiptFile {
    std::string  key;           // runtime, loader, notice, config, xess, ... (a version 1 receipt: native, graphics)
    std::wstring target;        // the file the tool placed
    std::wstring backup;        // the copy of what it replaced; empty when nothing was there
    std::string  beforeSha;     // the hash of what it replaced
    std::string  installedSha;  // the hash of what it placed
};
struct ToolReceipt {
    std::wstring path;          // the receipt file that was read
    bool         native = false;  // kind edvr-native-openxr, else edvr-flat
    std::vector<ToolReceiptFile> files;
};

// The newest receipt of each kind in the game folder, picked as install_edvr.py picks its current receipts: the newest backup-named
// sibling of the receipt when there is one, else the receipt itself. A receipt is read only when its target is this folder and it
// its state is "installed". Anything that does not parse as one is absent.
std::vector<ToolReceipt> readToolReceipts(const std::wstring& gameDir);

InstallState readState(const std::wstring& gameDir);
InstallState parseState(const std::string& text);
std::string  serializeState(const InstallState& state);

// UTC now, as 2026-08-27T14:03:11Z, and a backup folder name from the same
// clock reading.
std::string utcNow();
std::wstring timestampName();

}  // namespace edvr::installer
