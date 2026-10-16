#pragma once
// A rig that calls Config::init must not inherit the machine's own Elite Settings.xml. Config::init decides the runtime profile from the 3D mode in
// %LOCALAPPDATA%\Frontier Developments\Elite Dangerous\Options\Graphics\Settings.xml (runtime_profile.h) and mirrors the settings under
// %LOCALAPPDATA%\EDVR, so a rig run by hand on a machine last flown flat took the flat profile (and wrote into the real profile's mirror).
// run_jobs.py gives its pool children a mode-4 fixture; this is the same for a rig run directly, and the way a rig pins either mode itself.
//
//   pinRigGraphicsMode(root, 4)   // VR; 0 is flat
//   ...Config::get().init(dir)...
//   restoreRigLocalAppData(saved)
#include <windows.h>

#include <string>

namespace edvr {
namespace rig {

// Makes <root>\Frontier Developments\Elite Dangerous\Options\Graphics\Settings.xml with the 3D mode and points this process's LOCALAPPDATA at root.
inline bool pinRigGraphicsMode(const std::wstring& root, int mode) {
    CreateDirectoryW(root.c_str(), nullptr);
    std::wstring path = root;
    for (const wchar_t* part : {L"Frontier Developments", L"Elite Dangerous", L"Options", L"Graphics"}) {
        path += L"\\";
        path += part;
        CreateDirectoryW(path.c_str(), nullptr);
    }
    const std::string xml = "<Settings>\r\n<StereoscopicMode>" + std::to_string(mode) + "</StereoscopicMode>\r\n</Settings>\r\n";
    HANDLE h = CreateFileW((path + L"\\Settings.xml").c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const BOOL ok = WriteFile(h, xml.data(), static_cast<DWORD>(xml.size()), &written, nullptr);
    CloseHandle(h);
    return ok && written == xml.size() && SetEnvironmentVariableW(L"LOCALAPPDATA", root.c_str()) != 0;
}

// A fixture tree under the temp directory, named for the process, for a rig with no scratch directory of its own. Removed by removeRigTree.
inline std::wstring rigFixtureRoot(const wchar_t* tag) {
    wchar_t temp[MAX_PATH]{};
    GetTempPathW(MAX_PATH, temp);
    return std::wstring(temp) + L"edvr_" + tag + L"_localappdata_" + std::to_wstring(GetCurrentProcessId());
}
inline void removeRigTree(const std::wstring& dir) {
    WIN32_FIND_DATAW fd{};
    HANDLE find = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (find != INVALID_HANDLE_VALUE) {
        do {
            const std::wstring name = fd.cFileName;
            if (name == L"." || name == L"..") continue;
            const std::wstring full = dir + L"\\" + name;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) removeRigTree(full);
            else DeleteFileW(full.c_str());
        } while (FindNextFileW(find, &fd));
        FindClose(find);
    }
    RemoveDirectoryW(dir.c_str());
}

// The LOCALAPPDATA this process started with, to put back (empty: it had none).
inline std::wstring currentRigLocalAppData() {
    wchar_t buffer[MAX_PATH]{};
    const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buffer, MAX_PATH);
    return n && n < MAX_PATH ? std::wstring(buffer) : std::wstring();
}
inline void restoreRigLocalAppData(const std::wstring& saved) {
    SetEnvironmentVariableW(L"LOCALAPPDATA", saved.empty() ? nullptr : saved.c_str());
}

}  // namespace rig
}  // namespace edvr
