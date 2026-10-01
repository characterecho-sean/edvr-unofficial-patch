// Where Elite keeps the user's graphics choices, in one place.
//
// They live under the Windows profile, not the game folder:
//   %LOCALAPPDATA%\Frontier Developments\Elite Dangerous\Options\Graphics
// Several files sit there and their names shift with the game version
// (Settings.xml names the preset; Custom.<major>.<minor>.fxcfg is the Custom
// preset's values, one file per game version, the older ones left behind).
//
// Two binaries need the folder and resolve LOCALAPPDATA their own way: the
// installer's log bundler asks the shell (SHGetKnownFolderPath, with the
// environment variable as its fallback) and sweeps the whole folder into the
// zip; the d3d11 DLL, which links no shell library, reads the environment
// variable (as device_hook.cpp's eliteHmdMultiplier does) for the flat F8
// panel's settings warning (flat_elite_settings.h). What they share is the
// composition below, so the folder is spelled once. Header-only and free of
// any Windows API, so both can include it.
#pragma once

#include <string>

namespace edvr {

// The folder under a LocalAppData path. Empty in, empty out.
inline std::wstring eliteGraphicsFolderUnder(const std::wstring& localAppData) {
    if (localAppData.empty()) return std::wstring();
    std::wstring out = localAppData;
    if (out.back() != L'\\' && out.back() != L'/') out += L'\\';
    out += L"Frontier Developments\\Elite Dangerous\\Options\\Graphics";
    return out;
}

}  // namespace edvr
