// The scrolling list of settings.
//
// A child window that draws itself: one row per setting, grouped by the ini's
// own sections, with the control on the right, the word it is set to under it,
// and one link back to the recommended value when it is not there already.
// Custom-drawn for the same reason the rest of the window is --
// nothing to install, and a hundred real child controls to build, move and
// theme is more machinery than one paint function.
//
// Editing writes straight into edvr.ini. There is no Apply button because there
// is nothing to apply to: EDVR re-reads that file about once a second, so the
// change is live by the time you have let go of the mouse.
#pragma once

#include <windows.h>

#include <string>

#include "settings.h"

namespace edvr::installer {

HWND createSettingsList(HWND parent, int id, UINT dpi);

// mirrorDir is where a change gets echoed outside the game folder (see
// mirror.h); empty is a valid value meaning "no mirror for this install",
// which a write simply skips.
void settingsListSetModel(HWND list, SettingsModel* model, const std::wstring& mirrorDir);
void settingsListSetFilter(HWND list, const std::wstring& needle);
void settingsListRescale(HWND list, UINT dpi);

// Set when a write fails, so the screen above can say so.
const std::string& settingsListLastError(HWND list);

}  // namespace edvr::installer
