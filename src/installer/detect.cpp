#include "detect.h"

#include <windows.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cwctype>
#include <set>
#include <vector>

namespace edvr::installer {
namespace {

const wchar_t* kGameExe = L"EliteDangerous64.exe";

std::wstring lower(std::wstring s) {
    for (wchar_t& c : s) c = static_cast<wchar_t>(towlower(c));
    return s;
}

std::wstring env(const wchar_t* name) {
    wchar_t buf[1024]{};
    const DWORD n = GetEnvironmentVariableW(name, buf, 1024);
    if (n == 0 || n >= 1024) return std::wstring();
    return std::wstring(buf, n);
}

std::string readTextFileImpl(const std::wstring& path, size_t limit = 4u << 20) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return std::string();
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(f, &size) || size.QuadPart <= 0 ||
        static_cast<size_t>(size.QuadPart) > limit) {
        CloseHandle(f);
        return std::string();
    }
    std::string text(static_cast<size_t>(size.QuadPart), '\0');
    DWORD read = 0;
    const BOOL ok = ReadFile(f, &text[0], static_cast<DWORD>(text.size()), &read, nullptr);
    CloseHandle(f);
    if (!ok) return std::string();
    text.resize(read);
    return text;
}

std::wstring regString(HKEY root, const wchar_t* subkey, const wchar_t* value, DWORD extraFlags) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(root, subkey, 0, KEY_READ | extraFlags, &key) != ERROR_SUCCESS)
        return std::wstring();
    wchar_t buf[1024]{};
    DWORD bytes = sizeof(buf) - sizeof(wchar_t);
    DWORD type = 0;
    const LSTATUS st =
        RegQueryValueExW(key, value, nullptr, &type, reinterpret_cast<BYTE*>(buf), &bytes);
    RegCloseKey(key);
    if (st != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ)) return std::wstring();
    return std::wstring(buf);
}

// Every subdirectory of `dir`, or nothing if it cannot be listed.
std::vector<std::wstring> subdirs(const std::wstring& dir) {
    std::vector<std::wstring> out;
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW(joinPath(dir, L"*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        const std::wstring name = fd.cFileName;
        if (name == L"." || name == L"..") continue;
        out.push_back(name);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return out;
}

std::vector<std::wstring> filesMatching(const std::wstring& dir, const std::wstring& pattern) {
    std::vector<std::wstring> out;
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW(joinPath(dir, pattern).c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        out.push_back(fd.cFileName);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return out;
}

// A store told us where it put the game. Confirm it, and add every product
// folder underneath that really holds the executable.
//
// "Underneath" is one level of Products\ and no more. A recursive search of a
// Steam library would take minutes and could wander into somebody's backups.
void addFromRoot(const std::wstring& root, const std::wstring& source,
                 std::vector<GameInstall>* out) {
    if (root.empty()) return;

    auto push = [&](const std::wstring& dir, const std::wstring& product) {
        GameInstall gi;
        gi.dir = canonicalPath(dir);
        gi.source = source;
        gi.product = product;
        gi.odyssey = lower(product).find(L"odyssey") != std::wstring::npos;
        gi.openvrDir = findOpenvrDir(gi.dir);
        out->push_back(gi);
    };

    if (gameDirLooksRight(root)) {
        // The root IS a product folder: name it by its own leaf.
        const size_t slash = root.find_last_of(L"\\/");
        push(root, slash == std::wstring::npos ? root : root.substr(slash + 1));
        return;
    }

    // Store roots point at either the launcher folder or the game folder above
    // Products\. Both shapes appear in the wild, and Steam adds one more level
    // (the library, then "Elite Dangerous").
    const std::wstring candidates[] = {
        root,
        joinPath(root, L"Elite Dangerous"),
        joinPath(root, L"EDLaunch"),
    };
    for (const std::wstring& c : candidates) {
        const std::wstring products = joinPath(c, L"Products");
        if (!dirExists(products)) continue;
        for (const std::wstring& p : subdirs(products)) {
            const std::wstring dir = joinPath(products, p);
            if (gameDirLooksRight(dir)) push(dir, p);
        }
    }
}

// ---- Steam ---------------------------------------------------------------
//
// The library index, not a guessed path: people move Elite to whichever drive
// has room, and libraryfolders.vdf is where Steam records that.
void findSteam(std::vector<GameInstall>* out) {
    std::wstring steam = regString(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath", 0);
    if (steam.empty())
        steam = regString(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Valve\\Steam", L"InstallPath",
                          KEY_WOW64_32KEY);
    if (steam.empty()) return;
    for (wchar_t& c : steam) {
        if (c == L'/') c = L'\\';
    }

    std::vector<std::wstring> libraries{steam};
    const std::string vdf = readTextFileImpl(joinPath(steam, L"steamapps\\libraryfolders.vdf"));
    // Deliberately not a VDF parser: every entry of interest is a "path" key
    // whose value is a quoted, backslash-escaped Windows path, and that is all
    // this needs to read. A wrong or stale entry costs nothing -- it simply
    // fails the executable check below.
    size_t p = 0;
    while ((p = vdf.find("\"path\"", p)) != std::string::npos) {
        const size_t open = vdf.find('"', p + 6);
        if (open == std::string::npos) break;
        const size_t close = vdf.find('"', open + 1);
        if (close == std::string::npos) break;
        std::string raw = vdf.substr(open + 1, close - open - 1);
        std::string unescaped;
        for (size_t i = 0; i < raw.size(); ++i) {
            if (raw[i] == '\\' && i + 1 < raw.size() && raw[i + 1] == '\\') ++i;
            unescaped += raw[i];
        }
        libraries.push_back(fromUtf8(unescaped));
        p = close + 1;
    }

    for (const std::wstring& lib : libraries) {
        if (lib.empty()) continue;
        addFromRoot(joinPath(lib, L"steamapps\\common\\Elite Dangerous"), L"Steam", out);
    }
}

// ---- Epic ----------------------------------------------------------------
//
// Epic writes one .item manifest per installed game, as JSON. Again no parser:
// InstallLocation is the only field that matters and DisplayName is only used
// to tell Elite's manifest from the other twenty.
void findEpic(std::vector<GameInstall>* out) {
    const std::wstring manifests =
        joinPath(env(L"ProgramData"), L"Epic\\EpicGamesLauncher\\Data\\Manifests");
    if (!dirExists(manifests)) return;

    for (const std::wstring& name : filesMatching(manifests, L"*.item")) {
        const std::string json = readTextFileImpl(joinPath(manifests, name), 1u << 20);
        if (json.empty()) continue;

        auto field = [&](const char* key) -> std::string {
            const std::string needle = std::string("\"") + key + "\"";
            size_t k = json.find(needle);
            if (k == std::string::npos) return std::string();
            const size_t colon = json.find(':', k + needle.size());
            if (colon == std::string::npos) return std::string();
            const size_t open = json.find('"', colon);
            if (open == std::string::npos) return std::string();
            std::string value;
            for (size_t i = open + 1; i < json.size(); ++i) {
                if (json[i] == '\\' && i + 1 < json.size()) {
                    ++i;
                    value += json[i];
                    continue;
                }
                if (json[i] == '"') break;
                value += json[i];
            }
            return value;
        };

        const std::wstring display = lower(fromUtf8(field("DisplayName")));
        const std::wstring location = fromUtf8(field("InstallLocation"));
        if (location.empty()) continue;
        if (display.find(L"elite") == std::wstring::npos &&
            lower(location).find(L"elite") == std::wstring::npos) {
            continue;
        }
        addFromRoot(location, L"Epic", out);
    }
}

// ---- Frontier launcher ---------------------------------------------------
void findFrontier(std::vector<GameInstall>* out) {
    // Where the launcher itself keeps Products these days.
    addFromRoot(joinPath(env(L"LOCALAPPDATA"), L"Frontier_Developments"), L"Frontier launcher", out);

    // The uninstall registry, both views: whatever the launcher's installer
    // recorded is the authoritative answer for an install that moved.
    const wchar_t* uninstallRoots[] = {
        L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall",
        L"SOFTWARE\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\Uninstall",
    };
    for (const wchar_t* uroot : uninstallRoots) {
        HKEY key = nullptr;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, uroot, 0, KEY_READ, &key) != ERROR_SUCCESS) continue;
        for (DWORD i = 0;; ++i) {
            wchar_t sub[512]{};
            DWORD len = 512;
            if (RegEnumKeyExW(key, i, sub, &len, nullptr, nullptr, nullptr, nullptr) !=
                ERROR_SUCCESS) {
                break;
            }
            const std::wstring path = std::wstring(uroot) + L"\\" + sub;
            const std::wstring name = lower(regString(HKEY_LOCAL_MACHINE, path.c_str(),
                                                      L"DisplayName", 0));
            if (name.find(L"elite dangerous") == std::wstring::npos &&
                name.find(L"frontier") == std::wstring::npos) {
                continue;
            }
            addFromRoot(regString(HKEY_LOCAL_MACHINE, path.c_str(), L"InstallLocation", 0),
                        L"Frontier launcher", out);
        }
        RegCloseKey(key);
    }

    // And the places it has installed itself to by default, on every fixed
    // drive. Cheap: three directory probes per drive, no walking.
    std::vector<std::wstring> roots;
    const std::wstring progFiles86 = env(L"ProgramFiles(x86)");
    const std::wstring progFiles = env(L"ProgramFiles");
    if (!progFiles86.empty()) roots.push_back(joinPath(progFiles86, L"Frontier"));
    if (!progFiles.empty()) roots.push_back(joinPath(progFiles, L"Frontier"));

    const DWORD drives = GetLogicalDrives();
    for (int i = 0; i < 26; ++i) {
        if (!(drives & (1u << i))) continue;
        wchar_t root[4] = {static_cast<wchar_t>(L'A' + i), L':', L'\\', 0};
        if (GetDriveTypeW(root) != DRIVE_FIXED) continue;
        const std::wstring d(root);
        roots.push_back(d + L"Frontier");
        roots.push_back(d + L"Games\\Frontier");
        roots.push_back(d + L"Program Files (x86)\\Frontier");
    }
    for (const std::wstring& r : roots) addFromRoot(r, L"Frontier launcher", out);
}

}  // namespace

std::string toUtf8(const std::wstring& s) {
    if (s.empty()) return std::string();
    const int need = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr,
                                         0, nullptr, nullptr);
    if (need <= 0) return std::string();
    std::string out(static_cast<size_t>(need), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), &out[0], need, nullptr,
                        nullptr);
    return out;
}

std::wstring fromUtf8(const std::string& s) {
    if (s.empty()) return std::wstring();
    const int need =
        MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    if (need <= 0) return std::wstring();
    std::wstring out(static_cast<size_t>(need), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), &out[0], need);
    return out;
}

std::string readTextFile(const std::wstring& path, size_t limit) {
    return readTextFileImpl(path, limit);
}

std::wstring leafOf(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

std::wstring joinPath(const std::wstring& dir, const std::wstring& leaf) {
    if (dir.empty()) return leaf;
    if (leaf.empty()) return dir;
    std::wstring out = dir;
    if (out.back() != L'\\' && out.back() != L'/') out += L'\\';
    out += leaf;
    return out;
}

bool fileExists(const std::wstring& path) {
    const DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

bool dirExists(const std::wstring& path) {
    const DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

std::wstring canonicalPath(const std::wstring& path) {
    wchar_t buf[1024]{};
    const DWORD n = GetFullPathNameW(path.c_str(), 1024, buf, nullptr);
    std::wstring out = (n > 0 && n < 1024) ? std::wstring(buf, n) : path;
    while (out.size() > 3 && (out.back() == L'\\' || out.back() == L'/')) out.pop_back();
    return out;
}

bool gameDirLooksRight(const std::wstring& dir) {
    return !dir.empty() && fileExists(joinPath(dir, kGameExe));
}

std::wstring findOpenvrDir(const std::wstring& gameDir) {
    // Both layouts are in the field, and a previous install leaves the renamed
    // original as the only openvr file in the folder -- so the search goes by
    // FILE, not by folder name, and accepts either name.
    const std::wstring candidates[] = {
        joinPath(gameDir, L"Openvr\\win64"),
        joinPath(gameDir, L"Openvr"),
        gameDir,
    };
    for (const std::wstring& c : candidates) {
        if (!dirExists(c)) continue;
        if (fileExists(joinPath(c, L"openvr_api.dll")) ||
            fileExists(joinPath(c, L"openvr_api_orig.dll"))) {
            return c;
        }
    }
    return std::wstring();
}

bool describeDir(const std::wstring& dir, const std::wstring& source, GameInstall* out) {
    std::vector<GameInstall> found;
    addFromRoot(canonicalPath(dir), source, &found);
    if (found.empty()) return false;
    *out = found.front();
    return true;
}

bool gameIsRunning() {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    bool running = false;
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, kGameExe) == 0) {
                running = true;
                break;
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return running;
}

std::vector<GameInstall> findInstalls() {
    std::vector<GameInstall> found;
    findSteam(&found);
    findEpic(&found);
    findFrontier(&found);

    // One install can be reachable through more than one store record (an Epic
    // manifest and the uninstall registry can name the same folder). Keep the
    // first, which is the more specific source.
    std::vector<GameInstall> unique;
    std::set<std::wstring> seen;
    for (const GameInstall& gi : found) {
        if (!seen.insert(lower(gi.dir)).second) continue;
        unique.push_back(gi);
    }

    // Odyssey first: it is what EDVR is for, and on a machine with both
    // products the Horizons folder is the wrong answer offered by default.
    std::stable_sort(unique.begin(), unique.end(),
                     [](const GameInstall& a, const GameInstall& b) {
                         return a.odyssey && !b.odyssey;
                     });
    return unique;
}

}  // namespace edvr::installer
