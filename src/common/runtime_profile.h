// Installation scope is read before feature configuration. INI reloads never
// widen it. The absent-descriptor path preserves existing manual VR installs.
// The rules that decide the profile are in install_scope.h; this file reads the
// files and the command line and holds the one global the fixes read.
#pragma once
#include <windows.h>
#include <atomic>
#include <cstring>
#include <string>

#include "elite_graphics_folder.h"
#include "flat_profile_keys.h"
#include "install_scope.h"

namespace edvr {
inline std::atomic<RuntimeProfile> g_runtimeProfile{RuntimeProfile::LegacyVr};

enum class FileState { Absent, Unreadable, Read };

// Reads a whole file of at most maxBytes. Absent means the path names nothing; a file that is too large, cannot be opened or cannot be
// read is Unreadable. An empty file is Read with an empty text.
inline FileState readBoundedFile(const std::wstring& path, size_t maxBytes, std::string& text) {
    text.clear();
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND ? FileState::Absent : FileState::Unreadable;
    }
    LARGE_INTEGER size{};
    DWORD got = 0;
    bool ok = GetFileSizeEx(file, &size) && size.QuadPart >= 0 && size.QuadPart <= static_cast<LONGLONG>(maxBytes);
    if (ok) {
        text.resize(static_cast<size_t>(size.QuadPart));
        ok = text.empty() || (ReadFile(file, &text[0], static_cast<DWORD>(text.size()), &got, nullptr) && got == text.size());
    }
    CloseHandle(file);
    return ok ? FileState::Read : FileState::Unreadable;
}

inline ScopeRead readInstallDescriptor(const std::wstring& directory) {
    std::string text;
    switch (readBoundedFile(directory + L"\\edvr_profile.ini", 4096, text)) {
    case FileState::Absent: return absentDescriptor();
    case FileState::Unreadable: return ScopeRead{InstallScope::Invalid, 0, true};
    default: return parseInstallDescriptor(text);
    }
}

inline std::string runtimeProfileUtf8(const std::wstring& wide) {
    if (wide.empty()) return {};
    const int size = static_cast<int>(wide.size());
    const int need = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), size, nullptr, 0, nullptr, nullptr);
    if (need <= 0) return {};
    std::string out(static_cast<size_t>(need), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), size, &out[0], need, nullptr, nullptr);
    return out;
}

// The decision and the paths and command line it was made from. The log is not open when the decision runs, so the log line is
// written later, from these.
inline LaunchDecision g_launchDecision{};
inline std::string g_settingsPathUtf8;
inline std::wstring g_settingsPathWide;
inline std::string g_gameCommandLineUtf8;

inline void runtimeProfileInitialize(const std::wstring& moduleDir,
                                     const std::wstring& exeDir) {
    ScopeRead scope = readInstallDescriptor(moduleDir);
    if (!scope.present && moduleDir != exeDir) scope = readInstallDescriptor(exeDir);

    // Settings.xml sits under the Windows profile. Without LOCALAPPDATA the mode is unknown, as it is for a missing file.
    std::wstring localAppData;
    const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", nullptr, 0);
    if (length > 0) {
        localAppData.resize(length);
        const DWORD got = GetEnvironmentVariableW(L"LOCALAPPDATA", &localAppData[0], length);
        if (got > 0 && got < length) localAppData.resize(got);
        else localAppData.clear();
    }
    const std::wstring settingsPath = localAppData.empty() ? std::wstring()
        : eliteGraphicsFolderUnder(localAppData) + L"\\Settings.xml";
    g_settingsPathUtf8 = settingsPath.empty() ? std::string("(LOCALAPPDATA not set)") : runtimeProfileUtf8(settingsPath);
    g_settingsPathWide = settingsPath;
    std::string xml;
    const bool haveXml = !settingsPath.empty() && readBoundedFile(settingsPath, 1024 * 1024, xml) == FileState::Read;
    const ModeRead mode = readGameMode(haveXml ? xml.c_str() : nullptr);

    // The raw line is parsed here and never logged: it carries the account's tokens. The log gets the redacted form.
    const std::string commandLine = runtimeProfileUtf8(std::wstring(GetCommandLineW()));
    const VrSwitch cmd = parseVrSwitch(commandLine);
    g_gameCommandLineUtf8 = redactCommandLine(commandLine);
    // Both switches override Settings.xml in the game (flights 2026-10-12: /steam /vr over a file at 0 went into VR; /steam /novr over
    // a file at 4 stayed flat, no openvr_api.dll load).
    g_launchDecision = decideLaunch(scope, mode, cmd, /*applyVr=*/true, /*applyNoVr=*/true);
    g_runtimeProfile = g_launchDecision.active;
}

// Called only at the commit point, at the frame boundary (design section 3.3).
inline void runtimeProfileSet(RuntimeProfile p) { g_runtimeProfile.store(p, std::memory_order_relaxed); }

inline bool runtimeFlatProfile() {
    return g_runtimeProfile.load(std::memory_order_relaxed) == RuntimeProfile::Flat;
}
inline bool runtimeVrProfile() {
    const RuntimeProfile p = g_runtimeProfile.load(std::memory_order_relaxed);
    return p == RuntimeProfile::Vr || p == RuntimeProfile::LegacyVr;
}
// Passthrough forwards only, like Invalid, so no fix runs in it.
inline bool runtimeFeaturesAllowed() {
    const RuntimeProfile p = g_runtimeProfile.load(std::memory_order_relaxed);
    return p != RuntimeProfile::Invalid && p != RuntimeProfile::Passthrough;
}
inline const char* runtimeProfileName() {
    switch (g_runtimeProfile.load(std::memory_order_relaxed)) {
    case RuntimeProfile::LegacyVr: return "legacy-vr";
    case RuntimeProfile::Vr: return "vr";
    case RuntimeProfile::Flat: return "flat";
    case RuntimeProfile::Passthrough: return "passthrough";
    default: return "invalid";
    }
}
inline std::string runtimeProfileLogLine() {
    return formatProfileLine(g_launchDecision, runtimeProfileName(), g_settingsPathUtf8);
}
inline const std::string& runtimeGameCommandLine() { return g_gameCommandLineUtf8; }

// What the mode watch starts from: the Settings.xml path (empty when LOCALAPPDATA is not set), the install scope, and the launch file's
// read (its mode is Vr when the read is unknown, design Q2). The command line's override is not part of it.
inline const std::wstring& runtimeSettingsPathWide() { return g_settingsPathWide; }
inline InstallScope runtimeInstallScope() { return g_launchDecision.scope.scope; }
inline ModeRead runtimeLaunchRead() { return g_launchDecision.mode; }
// The class the launch runs: the file's, or the command line's when /vr overrode it.
inline GameMode runtimeLaunchEffective() { return g_launchDecision.effective; }
inline RuntimeProfile runtimeProfileValue() { return g_runtimeProfile.load(std::memory_order_relaxed); }
// The profile the launch decision chose (before any mid-session commit), for gates that must still know a VR launch after a drop.
inline RuntimeProfile runtimeLaunchProfile() { return g_launchDecision.active; }

// Scope gates for the hooks. An install hooks what its scope includes, and a hook acts per call on the active profile, so a switch
// mid-session finds its hooks in place. decideLaunch never yields LegacyVr, so LegacyVr marks the time before the decision: then the
// answer is the VR-only install that predates the unified one (VR yes, flat no).
inline bool runtimeLaunchDecided() { return g_launchDecision.active != RuntimeProfile::LegacyVr; }
inline bool runtimeScopeIncludesFlat() {
    if (!runtimeLaunchDecided()) return false;
    const InstallScope scope = runtimeInstallScope();
    return scope == InstallScope::Both || scope == InstallScope::Flat;
}
inline bool runtimeScopeIncludesVr() {
    if (!runtimeLaunchDecided()) return true;
    const InstallScope scope = runtimeInstallScope();
    return scope == InstallScope::Both || scope == InstallScope::Vr;
}

// Flat owns a separate mono adapter. Generic setting reads cannot activate the
// stereo pipeline or unrelated fixes; the mono adapter explicitly reads the
// desired mode through requestedTemporalMode().
//
// A key the flat panel shows must be listed here or its getter reads 0 and the
// row does nothing, with no line in the log. The panel's rows are the table in
// menu_flat_rows.h, and tools\flat_sharpen_test fails the build for any of
// them this function refuses.
// The list itself is flat_profile_keys.h's, shared with the installer's Flat view. The log.* and advanced.real_dll keys are read by
// every profile; the rest only by flat.
inline bool runtimeProfileAllowsKey(const char* key) {
    if (runtimeVrProfile()) return true;
    if (!key) return false;
    if (std::strncmp(key, "log.", 4) == 0 || std::strcmp(key, "advanced.real_dll") == 0)
        return true;
    return runtimeFlatProfile() && flatProfileReadsKey(key);
}
} // namespace edvr
