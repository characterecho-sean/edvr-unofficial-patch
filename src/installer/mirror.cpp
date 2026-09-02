#include "mirror.h"

#include <windows.h>
#include <shlobj.h>

#include <cstdio>
#include <cwctype>

#include "state.h"

namespace edvr::installer {
namespace {

const wchar_t* kIni = L"edvr.ini";
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

// Clears read-only on the destination first: every file this copies, either
// direction, can legitimately be one Windows marked read-only (a launcher's
// file verification does this to files it considers its own), and CopyFileW
// simply fails against that rather than replacing it.
bool copyOver(const std::wstring& from, const std::wstring& to) {
    const DWORD attrs = GetFileAttributesW(to.c_str());
    if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_READONLY))
        SetFileAttributesW(to.c_str(), attrs & ~FILE_ATTRIBUTE_READONLY);
    return CopyFileW(from.c_str(), to.c_str(), FALSE) != 0;
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

    const std::wstring iniPath = joinPath(mirrorDir, kIni);
    info.hasIni = fileExists(iniPath);
    info.hasBaseIni = fileExists(joinPath(mirrorDir, kBaseIni));
    info.hasState = fileExists(joinPath(mirrorDir, kStateIni));

    const std::wstring backupSub = joinPath(mirrorDir, kBackupSub);
    info.hasBackupPair =
        fileExists(joinPath(backupSub, kD3d11)) || fileExists(joinPath(backupSub, kOpenvr));

    if (info.hasIni) info.savedUtc = fileSavedUtc(iniPath);
    return info;
}

MirrorResult updateMirror(const std::wstring& gameDir, const std::wstring& backupDir,
                          const std::wstring& mirrorDir) {
    MirrorResult result;
    if (mirrorDir.empty() || !ensureDirTree(mirrorDir)) return result;
    result.ok = true;

    const std::wstring iniPath = joinPath(gameDir, kIni);
    if (fileExists(iniPath) && copyOver(iniPath, joinPath(mirrorDir, kIni)))
        result.saved.push_back("edvr.ini");

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

    const std::wstring iniPath = joinPath(gameDir, kIni);
    if (!fileExists(iniPath)) return result;

    result.ok = copyOver(iniPath, joinPath(mirrorDir, kIni));
    if (result.ok) result.saved.push_back("edvr.ini");
    return result;
}

bool restoreFromMirror(const std::wstring& gameDir, const MirrorInfo& info,
                       std::vector<std::string>* notes) {
    if (!info.hasIni) return false;
    auto note = [&](const std::string& s) {
        if (notes) notes->push_back(s);
    };

    if (!copyOver(joinPath(info.dir, kIni), joinPath(gameDir, kIni))) return false;
    note("Restored edvr.ini from the copy kept outside the game folder.");

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
    return true;
}

}  // namespace edvr::installer
