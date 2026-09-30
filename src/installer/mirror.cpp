#include "mirror.h"

#include <windows.h>
#include <shlobj.h>

#include <cstdio>
#include <cwctype>
#include <string>

#include "../common/iniedit.h"
#include "state.h"

namespace edvr::installer {
namespace {

const wchar_t* kIni = L"edvr.ini";
const wchar_t* kFlatIni = L"edvr-flat.ini";
const wchar_t* kBaseIni = L"edvr.ini.base";
const wchar_t* kStateIni = L"state.ini";
const wchar_t* kD3d11 = L"d3d11.dll";
const wchar_t* kOpenvr = L"openvr_api.dll";
const wchar_t* kBackupSub = L"backup";

std::wstring localAppDataFolder() {
    PWSTR path = nullptr;
    std::wstring base;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &path)) && path) {
        base = path;
        CoTaskMemFree(path);
    } else {
        wchar_t env[MAX_PATH]{};
        if (GetEnvironmentVariableW(L"LOCALAPPDATA", env, MAX_PATH)) base = env;
    }
    return base;
}

// "Frontier launcher" -> "frontier-launcher". Lowercase and hyphenated so it
// reads naturally as part of a folder name rather than shouting at whoever
// browses to it.
std::wstring slugOf(const std::wstring& s) {
    std::wstring out;
    for (wchar_t c : s) {
        if (iswalnum(c)) {
            out += static_cast<wchar_t>(towlower(c));
        } else if (!out.empty() && out.back() != L'-') {
            out += L'-';
        }
    }
    while (!out.empty() && out.back() == L'-') out.pop_back();
    return out;
}

bool ensureDirTree(const std::wstring& path) {
    if (path.empty() || dirExists(path)) return true;
    const size_t slash = path.find_last_of(L"\\/");
    if (slash != std::wstring::npos && slash > 2) {
        if (!ensureDirTree(path.substr(0, slash))) return false;
    }
    if (CreateDirectoryW(path.c_str(), nullptr)) return true;
    return GetLastError() == ERROR_ALREADY_EXISTS && dirExists(path);
}

// Every file this writes, either direction, can legitimately be one Windows
// marked read-only (a launcher's file verification does this to files it
// considers its own), and a replace simply fails against that.
void clearReadOnly(const std::wstring& path) {
    const DWORD attrs = GetFileAttributesW(path.c_str());
    if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_READONLY))
        SetFileAttributesW(path.c_str(), attrs & ~FILE_ATTRIBUTE_READONLY);
}

// The largest file copied: the d3d11.dll and openvr_api.dll of the backup pair
// are megabytes, and a file that is not is not one this mirror has any business
// holding.
const size_t kCopyLimit = 256u << 20;

// A file copied whole, and put in place with one replace: what was at `to` is
// either still there or entirely replaced. This was CopyFileW in place, and a
// failure part-way -- a full disk, a scanner taking the handle -- left the
// mirror's only copy of somebody's settings truncated.
bool copyOver(const std::wstring& from, const std::wstring& to) {
    clearReadOnly(to);
    std::string bytes;
    if (!readFileBytes(from, &bytes, kCopyLimit)) return false;
    return writeFileAtomic(to, bytes);
}

// One ini into the mirror as its newest generation (see iniedit.h). A
// `checkpoint` -- an installer run -- keeps the copy it replaces as .1, and .1
// as .2; anything else is a change made a moment ago and replaces the newest in
// place.
bool mirrorIni(const std::wstring& from, const std::wstring& mirrorDir, const wchar_t* name,
               bool checkpoint) {
    std::string bytes;
    if (!readFileBytes(from, &bytes) || bytes.empty()) return false;
    for (int g = 0; g < kMirrorGenerations; ++g) clearReadOnly(generationPath(mirrorDir, name, g));
    return writeGenerations(mirrorDir, name, bytes, checkpoint);
}

// UTC, formatted the same way state.cpp's utcNow() is -- one clock, one
// format, wherever a timestamp reaches the report.
std::string fileSavedUtc(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) return std::string();
    SYSTEMTIME utc{};
    if (!FileTimeToSystemTime(&data.ftLastWriteTime, &utc)) return std::string();
    char buf[32];
    sprintf_s(buf, "%04u-%02u-%02uT%02u:%02u:%02uZ", utc.wYear, utc.wMonth, utc.wDay, utc.wHour,
              utc.wMinute, utc.wSecond);
    return std::string(buf);
}

}  // namespace

std::wstring defaultMirrorRoot() {
    const std::wstring base = localAppDataFolder();
    return base.empty() ? std::wstring() : joinPath(base, L"EDVR");
}

std::wstring mirrorDirFor(const GameInstall& game, const std::wstring& root) {
    if (root.empty() || game.dir.empty()) return std::wstring();
    std::wstring name = leafOf(game.dir);
    const std::wstring slug = slugOf(game.source);
    if (!slug.empty()) name += L"-" + slug;
    return joinPath(root, name);
}

MirrorInfo readMirror(const std::wstring& mirrorDir) {
    MirrorInfo info;
    info.dir = mirrorDir;
    if (mirrorDir.empty()) return info;

    // The newest generation of each ini that is there and not empty (iniedit.h):
    // a crash between two writes, or a copy an older build left half-written,
    // must not hide the settings behind it.
    const std::wstring iniPath = newestGeneration(mirrorDir, kIni);
    const std::wstring flatIniPath = newestGeneration(mirrorDir, kFlatIni);
    info.hasIni = !iniPath.empty();
    info.hasFlatIni = !flatIniPath.empty();
    info.hasBaseIni = fileExists(joinPath(mirrorDir, kBaseIni));
    info.hasState = fileExists(joinPath(mirrorDir, kStateIni));
    // The mirror preserves ownership metadata, not the active runtime scope.
    // The selected artifact writes a fresh descriptor during repair.

    const std::wstring backupSub = joinPath(mirrorDir, kBackupSub);
    info.hasBackupPair =
        fileExists(joinPath(backupSub, kD3d11)) || fileExists(joinPath(backupSub, kOpenvr));

    // When the settings were mirrored: the later of the two files' copies (the
    // format sorts as text), so a flat-only mirror says when ITS file was saved.
    const std::string iniWhen = info.hasIni ? fileSavedUtc(iniPath) : std::string();
    const std::string flatWhen = info.hasFlatIni ? fileSavedUtc(flatIniPath) : std::string();
    info.savedUtc = flatWhen > iniWhen ? flatWhen : iniWhen;
    return info;
}

bool MirrorInfo::holdsSettings() const { return hasIni || hasFlatIni; }

bool MirrorInfo::holdsSettingsFor(const std::string& profile) const {
    return profile == "flat" ? (hasFlatIni || hasIni) : hasIni;
}

bool offerRestore(bool folderHasSettings, const MirrorInfo& mirror, const std::string& profile) {
    return !folderHasSettings && mirror.holdsSettingsFor(profile);
}

MirrorResult updateMirror(const std::wstring& gameDir, const std::wstring& backupDir,
                          const std::wstring& mirrorDir) {
    MirrorResult result;
    if (mirrorDir.empty() || !ensureDirTree(mirrorDir)) return result;
    result.ok = true;

    // An install or repair is the checkpoint the generations exist for: the
    // copy this replaces is kept. Without that, a game folder wiped by an update
    // and then installed fresh -- because the restore failed or was declined --
    // overwrote the only saved copy with the defaults it had just written.
    const std::wstring iniPath = joinPath(gameDir, kIni);
    if (fileExists(iniPath) && mirrorIni(iniPath, mirrorDir, kIni, true))
        result.saved.push_back("edvr.ini");

    // The flat profile's own settings file, mirrored whenever a flat install
    // left one; a VR-only folder simply has nothing to copy.
    const std::wstring flatIniPath = joinPath(gameDir, kFlatIni);
    if (fileExists(flatIniPath) && mirrorIni(flatIniPath, mirrorDir, kFlatIni, true))
        result.saved.push_back("edvr-flat.ini");

    const std::wstring basePath = baseIniPath(gameDir);
    if (fileExists(basePath) && copyOver(basePath, joinPath(mirrorDir, kBaseIni)))
        result.saved.push_back("edvr.ini.base");

    const std::wstring statePathSrc = statePath(gameDir);
    if (fileExists(statePathSrc) && copyOver(statePathSrc, joinPath(mirrorDir, kStateIni)))
        result.saved.push_back("state.ini");

    // Whatever THIS run just backed up -- not a scan for "the newest stamp",
    // which could just as easily be a settings-window backup that holds no
    // DLLs at all. A run that backed up neither file (nothing to replace)
    // leaves whatever pair a previous run mirrored in place, rather than
    // deleting a good recovery copy because today had nothing to add.
    if (!backupDir.empty()) {
        const std::wstring d3dSrc = joinPath(backupDir, kD3d11);
        const std::wstring ovrSrc = joinPath(backupDir, kOpenvr);
        const bool haveD3d = fileExists(d3dSrc);
        const bool haveOvr = fileExists(ovrSrc);
        if (haveD3d || haveOvr) {
            const std::wstring backupSub = joinPath(mirrorDir, kBackupSub);
            if (ensureDirTree(backupSub)) {
                bool copiedAny = false;
                if (haveD3d) copiedAny |= copyOver(d3dSrc, joinPath(backupSub, kD3d11));
                if (haveOvr) copiedAny |= copyOver(ovrSrc, joinPath(backupSub, kOpenvr));
                if (copiedAny)
                    result.saved.push_back("the last d3d11.dll/openvr_api.dll backup pair");
            }
        }
    }
    return result;
}

MirrorResult updateMirrorIni(const std::wstring& gameDir, const std::wstring& mirrorDir) {
    MirrorResult result;
    if (mirrorDir.empty() || !ensureDirTree(mirrorDir)) return result;

    // A setting changed a moment ago, not a checkpoint: the newest copy is
    // replaced in place and the older generations an install kept stay put. A
    // generation per toggle would push the copy worth keeping out in three.
    const std::wstring iniPath = joinPath(gameDir, kIni);
    if (fileExists(iniPath) && mirrorIni(iniPath, mirrorDir, kIni, false)) {
        result.ok = true;
        result.saved.push_back("edvr.ini");
    }

    const std::wstring flatIniPath = joinPath(gameDir, kFlatIni);
    if (fileExists(flatIniPath) && mirrorIni(flatIniPath, mirrorDir, kFlatIni, false)) {
        result.ok = true;
        result.saved.push_back("edvr-flat.ini");
    }
    return result;
}

bool restoreFromMirror(const std::wstring& gameDir, const MirrorInfo& info,
                       std::vector<std::string>* notes) {
    if (!info.holdsSettings()) return false;
    auto note = [&](const std::string& s) {
        if (notes) notes->push_back(s);
    };

    // Every settings file the mirror holds, each from the newest generation there
    // is: the one readMirror judged worth offering. Either is enough on its own
    // (a flat install mirrors edvr-flat.ini alone), and neither's failure is
    // hidden behind the other's success: the flat profile's file lost while the
    // shared edvr.ini came back is a restore that did not happen, and it says so.
    bool restoredAll = true;
    bool restoredAny = false;
    struct Settings {
        const wchar_t* leaf;
        bool           held;
        const char*    what;
    };
    const Settings settings[] = {
        {kIni, info.hasIni, "edvr.ini"},
        {kFlatIni, info.hasFlatIni, "edvr-flat.ini (the flat profile's own settings)"},
    };
    for (const Settings& one : settings) {
        if (!one.held) continue;
        const std::wstring source = newestGeneration(info.dir, one.leaf);
        if (source.empty() || !copyOver(source, joinPath(gameDir, one.leaf))) {
            note(std::string("Could not restore ") + one.what +
                 " from the copy kept outside the game folder.");
            restoredAll = false;
            continue;
        }
        restoredAny = true;
        note(std::string("Restored ") + one.what + " from the copy kept outside the game folder.");
    }
    // Nothing of the settings came back: the rest of the mirror is not worth
    // putting into a folder that could not take its settings.
    if (!restoredAny) return false;

    if (info.hasBaseIni || info.hasState) {
        if (ensureDirTree(stateDirPath(gameDir))) {
            bool copiedAny = false;
            if (info.hasBaseIni)
                copiedAny |= copyOver(joinPath(info.dir, kBaseIni), baseIniPath(gameDir));
            if (info.hasState)
                copiedAny |= copyOver(joinPath(info.dir, kStateIni), statePath(gameDir));
            if (copiedAny) {
                note("Restored the install record too, so this still merges as an update rather "
                     "than starting over.");
            }
        }
    }

    if (info.hasBackupPair) {
        const std::wstring restoreDir =
            joinPath(backupRootPath(gameDir), L"restored-" + timestampName());
        if (ensureDirTree(restoreDir)) {
            const std::wstring backupSub = joinPath(info.dir, kBackupSub);
            const std::wstring d3dSrc = joinPath(backupSub, kD3d11);
            const std::wstring ovrSrc = joinPath(backupSub, kOpenvr);
            bool any = false;
            if (fileExists(d3dSrc)) any |= copyOver(d3dSrc, joinPath(restoreDir, kD3d11));
            if (fileExists(ovrSrc)) any |= copyOver(ovrSrc, joinPath(restoreDir, kOpenvr));
            if (any) {
                note("Restored the last backed-up d3d11.dll/openvr_api.dll pair into edvr_backup\\" +
                     toUtf8(leafOf(restoreDir)) + "\\.");
            }
        }
    }
    return restoredAll;
}

}  // namespace edvr::installer
