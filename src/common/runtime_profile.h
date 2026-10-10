// Installation scope is read before feature configuration. INI reloads never
// widen it. The absent-descriptor path preserves existing manual VR installs.
#pragma once
#include <windows.h>
#include <cstring>
#include <string>

namespace edvr {
enum class RuntimeProfile { LegacyVr, Vr, Flat, Invalid };
inline RuntimeProfile g_runtimeProfile = RuntimeProfile::LegacyVr;

inline std::string profileTrim(const std::string& value) {
    const size_t first = value.find_first_not_of(" \t\r");
    if (first == std::string::npos) return {};
    return value.substr(first, value.find_last_not_of(" \t\r") - first + 1);
}

// Reject duplicates/unknown fields so two readers cannot disagree on scope.
inline RuntimeProfile parseRuntimeProfile(std::string text) {
    if (text.size() > 4096 || text.find('\0') != std::string::npos)
        return RuntimeProfile::Invalid;
    if (text.compare(0, 3, "\xef\xbb\xbf") == 0) text.erase(0, 3);
    bool section = false, schema = false, profile = false;
    RuntimeProfile result = RuntimeProfile::Invalid;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t end = text.find('\n', pos);
        if (end == std::string::npos) end = text.size();
        const std::string line = profileTrim(text.substr(pos, end - pos));
        pos = end + 1;
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;
        if (line == "[install]") {
            if (section) return RuntimeProfile::Invalid;
            section = true;
            continue;
        }
        if (!section) return RuntimeProfile::Invalid;
        const size_t eq = line.find('=');
        if (eq == std::string::npos) return RuntimeProfile::Invalid;
        const auto key = profileTrim(line.substr(0, eq));
        const auto value = profileTrim(line.substr(eq + 1));
        if (key == "schema" && !schema && value == "1") schema = true;
        else if (key == "profile" && !profile) {
            profile = true;
            if (value == "flat") result = RuntimeProfile::Flat;
            else if (value == "vr") result = RuntimeProfile::Vr;
            else return RuntimeProfile::Invalid;
        } else return RuntimeProfile::Invalid;
    }
    return section && schema && profile ? result : RuntimeProfile::Invalid;
}

inline RuntimeProfile readRuntimeProfile(const std::wstring& directory) {
    const std::wstring path = directory + L"\\edvr_profile.ini";
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND
            ? RuntimeProfile::LegacyVr : RuntimeProfile::Invalid;
    }
    LARGE_INTEGER size{};
    std::string text;
    DWORD got = 0;
    const bool sized = GetFileSizeEx(file, &size) && size.QuadPart > 0 && size.QuadPart <= 4096;
    if (sized) text.resize(static_cast<size_t>(size.QuadPart));
    const bool read = sized && ReadFile(file, &text[0], static_cast<DWORD>(text.size()), &got, nullptr)
        && got == text.size();
    CloseHandle(file);
    return read ? parseRuntimeProfile(text) : RuntimeProfile::Invalid;
}

inline void runtimeProfileInitialize(const std::wstring& moduleDir,
                                     const std::wstring& exeDir) {
    g_runtimeProfile = readRuntimeProfile(moduleDir);
    if (g_runtimeProfile == RuntimeProfile::LegacyVr && moduleDir != exeDir)
        g_runtimeProfile = readRuntimeProfile(exeDir);
}
inline bool runtimeFlatProfile() { return g_runtimeProfile == RuntimeProfile::Flat; }
inline bool runtimeVrProfile() {
    return g_runtimeProfile == RuntimeProfile::Vr || g_runtimeProfile == RuntimeProfile::LegacyVr;
}
inline bool runtimeFeaturesAllowed() { return g_runtimeProfile != RuntimeProfile::Invalid; }
inline const char* runtimeProfileName() {
    switch (g_runtimeProfile) {
    case RuntimeProfile::LegacyVr: return "legacy-vr";
    case RuntimeProfile::Vr: return "vr";
    case RuntimeProfile::Flat: return "flat";
    default: return "invalid";
    }
}

// Flat owns a separate mono adapter. Generic setting reads cannot activate the
// stereo pipeline or unrelated fixes; the mono adapter explicitly reads the
// desired mode through requestedTemporalMode().
//
// A key the flat panel shows must be listed here or its getter reads 0 and the
// row does nothing, with no line in the log. The panel's rows are the table in
// menu_flat_rows.h, and tools\flat_sharpen_test fails the build for any of
// them this function refuses.
inline bool runtimeProfileAllowsKey(const char* key) {
    if (runtimeVrProfile()) return true;
    if (!key) return false;
    if (std::strncmp(key, "log.", 4) == 0 || std::strcmp(key, "advanced.real_dll") == 0)
        return true;
    return runtimeFlatProfile() && (std::strcmp(key, "advanced.d3d11_fixes") == 0 ||
        std::strcmp(key, "advanced.input_gate") == 0 ||
        std::strcmp(key, "advanced.flat_camera_producer_probe") == 0 ||
        std::strcmp(key, "hotkey.menu") == 0 ||
        std::strcmp(key, "fix.temporal_aa_model") == 0 ||
        std::strcmp(key, "hotkey.dump_draws") == 0 ||
        std::strcmp(key, "fix.render_sharpness") == 0 ||
        // The interface quality (flat panel row; ui_panel_scale.cpp's flat factor and the flat UI layer). The VR layer reads it
        // too, but stays inert in flat: it is live only with fix.temporal_aa on through this gate, which refuses that key.
        std::strcmp(key, "fix.ui_quality") == 0 ||
        // The flat jitter cycle's length (flat_runtime.cpp, FlatLivePhase::phaseCount; temporal_math.h). Developer tier, no flat panel
        // row. Unlisted, getInt answers 0 here whatever the file says, which the reader takes for out of range and reads as 8.
        std::strcmp(key, "advanced.temporal_aa_jitter_phases") == 0 ||
        // The game's own vertex-buffer cache repair (vertex_resync_hook.cpp): installed at startup in BOTH profiles, because the stale cache is Frontier's, not VR's.
        // TEMPORARY, for the scanner-body A/B flight. Unlisted, getString answers "off" here whatever the file says, and the A/B would be a comparison of one thing.
        std::strcmp(key, "advanced.vertex_resync") == 0);
}
} // namespace edvr
