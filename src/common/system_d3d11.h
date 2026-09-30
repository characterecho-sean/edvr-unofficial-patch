// Windows' own d3d11, by full path.
//
// An import of D3D11CreateDevice binds to whichever d3d11.dll the loader finds
// first, and next to the game (or in build\, next to the test rigs) that is
// EDVR's proxy. Code that wants the genuine module -- the OpenXR runtime's
// separate device, and every rig that only needs a WARP device and must not
// start a second proxy that logs into build\edvr_logs and arms crash
// sentinels against the other rigs -- takes it from System32 through here
// and links without d3d11.lib.
//
// The module is looked up with GetModuleHandleExW before anything is loaded:
// 3Dmigoto/EDHM with load_library_redirect=2 hooks LoadLibraryExW and answers
// the System32 path with its own game-directory proxy. Only a process where
// nothing has mapped d3d11 yet falls back to a load, and the module's real
// path is checked either way, so a substitute is refused rather than used.
#pragma once

#include <windows.h>
#include <d3d11.h>
#include <tlhelp32.h>

#include <atomic>
#include <cstdio>
#include <string>
#include <vector>

namespace edvr {

// <System32>\d3d11.dll, or empty when the system directory cannot be read.
inline std::wstring systemD3D11Path() {
    wchar_t directory[MAX_PATH]{};
    const UINT length = GetSystemDirectoryW(directory, MAX_PATH);
    if (!length || length >= MAX_PATH) return std::wstring();
    return std::wstring(directory, length) + L"\\d3d11.dll";
}

struct SystemD3D11 {
    // One reference, the caller's to FreeLibrary; null when result failed.
    HMODULE module = nullptr;
    // How the module was obtained: "mapped" (already in the process), "loaded",
    // or "none" when the path itself was not available.
    const char* route = "none";
    // The path the module really has, once known -- the culprit on a mismatch.
    std::wstring path;
    // E_FAIL: no system directory; a Win32 HRESULT: the load failed;
    // E_ACCESSDENIED: the module at the path is not Windows' own.
    HRESULT result = E_FAIL;
};

// Takes one reference on System32's d3d11.dll and says how.
inline SystemD3D11 openSystemD3D11() {
    SystemD3D11 out;
    const std::wstring wanted = systemD3D11Path();
    if (wanted.empty()) return out;
    // Flags 0 (not UNCHANGED_REFCOUNT) adds a reference, balanced by FreeLibrary.
    if (GetModuleHandleExW(0, wanted.c_str(), &out.module) && out.module) {
        out.route = "mapped";
    } else {
        out.module = LoadLibraryExW(wanted.c_str(), nullptr,
                                    LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if (!out.module) {
            out.result = HRESULT_FROM_WIN32(GetLastError());
            return out;
        }
        out.route = "loaded";
    }
    wchar_t actual[MAX_PATH]{};
    const DWORD length = GetModuleFileNameW(out.module, actual, MAX_PATH);
    if (length && length < MAX_PATH) out.path.assign(actual, length);
    if (!length || length >= MAX_PATH || _wcsicmp(actual, wanted.c_str()) != 0) {
        FreeLibrary(out.module);
        out.module = nullptr;
        out.result = E_ACCESSDENIED;
        return out;
    }
    out.result = S_OK;
    return out;
}

// Records whether systemD3D11CreateDevice() holds a permanent reference to
// System32's d3d11.dll.
inline std::atomic<bool> g_systemD3D11Pinned{false};

inline bool isSystemD3D11Pinned() {
    return g_systemD3D11Pinned.load(std::memory_order_relaxed);
}

// D3D11CreateDevice from System32's d3d11.dll, which stays mapped for the life
// of the process. Null when the module cannot be had; a caller that gets null
// reports it the way it reports any other failed device.
inline PFN_D3D11_CREATE_DEVICE systemD3D11CreateDevice() {
    static const PFN_D3D11_CREATE_DEVICE create = [] {
        const SystemD3D11 system = openSystemD3D11();
        if (!system.module) return PFN_D3D11_CREATE_DEVICE(nullptr);
        g_systemD3D11Pinned.store(true, std::memory_order_relaxed);
        return reinterpret_cast<PFN_D3D11_CREATE_DEVICE>(GetProcAddress(system.module, "D3D11CreateDevice"));
    }();
    return create;
}

// The full path of every d3d11.dll mapped into this process, in load order:
// empty when none is. Two entries mean a second module of the name -- EDVR's
// proxy beside the exe -- is in the process as well as Windows' own.
inline std::vector<std::wstring> mappedD3D11Paths() {
    std::vector<std::wstring> paths;
    HANDLE snapshot = INVALID_HANDLE_VALUE;
    // A snapshot taken while another thread loads a module fails with
    // ERROR_BAD_LENGTH; asking again is the documented answer.
    for (int attempt = 0; attempt < 8 && snapshot == INVALID_HANDLE_VALUE; ++attempt) {
        snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, GetCurrentProcessId());
        if (snapshot == INVALID_HANDLE_VALUE && GetLastError() != ERROR_BAD_LENGTH) break;
    }
    if (snapshot == INVALID_HANDLE_VALUE) return paths;
    MODULEENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    for (BOOL more = Module32FirstW(snapshot, &entry); more; more = Module32NextW(snapshot, &entry)) {
        if (_wcsicmp(entry.szModule, L"d3d11.dll") == 0) paths.emplace_back(entry.szExePath);
    }
    CloseHandle(snapshot);
    return paths;
}

// For a rig that means to run on Windows' own d3d11 (every rig that only needs
// a WARP device): logs which d3d11.dll the process is running on, and says
// whether that is System32's and nothing else. A rig whose exe sits in build\
// and imports D3D11CreateDevice binds build\d3d11.dll instead and runs under
// EDVR's hooks unnoticed; a rig that checks this fails instead.
inline bool reportSystemD3D11Only(const char* rig) {
    const std::wstring system = systemD3D11Path();
    const std::vector<std::wstring> mapped = mappedD3D11Paths();
    bool only = !system.empty() && !mapped.empty();
    for (const std::wstring& path : mapped) {
        std::printf("[%s] d3d11.dll mapped from %ls\n", rig, path.c_str());
        if (_wcsicmp(path.c_str(), system.c_str()) != 0) only = false;
    }
    if (mapped.empty()) std::printf("[%s] no d3d11.dll is mapped\n", rig);
    return only;
}

// For a rig that loads EDVR's proxy on purpose, by path, to test it: call this
// before that load. True when no d3d11.dll is mapped yet -- the exe imports
// none, since an import is bound before main runs -- so the proxy the rig
// loads is the only one and arrives when the rig says.
inline bool reportNoD3D11Mapped(const char* rig) {
    const std::vector<std::wstring> mapped = mappedD3D11Paths();
    for (const std::wstring& path : mapped) std::printf("[%s] d3d11.dll mapped at startup from %ls\n", rig, path.c_str());
    if (mapped.empty()) std::printf("[%s] no d3d11.dll mapped before the rig loads one\n", rig);
    return mapped.empty();
}

}  // namespace edvr
