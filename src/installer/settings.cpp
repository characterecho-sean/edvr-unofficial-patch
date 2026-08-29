#include "settings.h"

#include <windows.h>

#include "detect.h"
#include "iniedit.h"
#include "state.h"

namespace edvr::installer {
namespace {

#include "settings_schema.inc"  // generated: kSettings[]

// Written beside the file and moved into place.
//
// This one rewrites the WHOLE of somebody's edvr.ini on every toggle, and the
// game re-reads that file about once a second. Truncating it first meant a
// failed write left an empty or half-written settings file with no backup
// anywhere -- and a poll landing in the gap read a 0-byte ini and dropped every
// setting to its compiled default until the next reload.
bool ensureBackupDir(const std::wstring& path) {
    if (dirExists(path)) return true;
    const size_t slash = path.find_last_of(L"\\/");
    if (slash != std::wstring::npos) {
        const std::wstring parent = path.substr(0, slash);
        if (!dirExists(parent) && !CreateDirectoryW(parent.c_str(), nullptr) &&
            GetLastError() != ERROR_ALREADY_EXISTS) {
            return false;
        }
    }
    return CreateDirectoryW(path.c_str(), nullptr) != 0 ||
           (GetLastError() == ERROR_ALREADY_EXISTS && dirExists(path));
}

bool writeWhole(const std::wstring& path, const std::string& text) {
    const std::wstring temp = path + L".edvrnew";
    HANDLE f = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const BOOL ok = WriteFile(f, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
    FlushFileBuffers(f);
    CloseHandle(f);
    if (!ok || written != text.size()) {
        DeleteFileW(temp.c_str());
        return false;
    }
    if (!MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileW(temp.c_str());
        return false;
    }
    return true;
}

}  // namespace

std::vector<Choice> splitChoices(const char* packed) {
    std::vector<Choice> out;
    if (!packed || !*packed) return out;
    std::string current;
    auto flush = [&]() {
        if (current.empty()) return;
        Choice choice;
        const size_t equals = current.find('=');
        if (equals == std::string::npos) {
            choice.value = current;
            choice.label = current;
        } else {
            choice.value = current.substr(0, equals);
            choice.label = current.substr(equals + 1);
        }
        out.push_back(choice);
        current.clear();
    };
    for (const char* p = packed; *p; ++p) {
        if (*p == '|')
            flush();
        else
            current += *p;
    }
    flush();
    return out;
}

namespace {

// A number as somebody would write it: no trailing zeros, no ".0" on a whole
// one, and never scientific notation for the small values these settings use.
std::string tidyNumber(double value, int decimals) {
    char buffer[64];
    sprintf_s(buffer, "%.*f", decimals, value);
    std::string text = buffer;
    if (text.find('.') != std::string::npos) {
        while (!text.empty() && text.back() == '0') text.pop_back();
        if (!text.empty() && text.back() == '.') text.pop_back();
    }
    return text.empty() ? "0" : text;
}

bool asNumber(const std::string& text, double* out) {
    if (text.empty()) return false;
    char* end = nullptr;
    const double value = strtod(text.c_str(), &end);
    if (end == text.c_str()) return false;
    while (end && *end == ' ') ++end;
    if (end && *end != '\0' && *end != '%') return false;
    *out = value;
    return true;
}

// 0, 0.0 and 0.00 are one value, and a row that calls a file saying "0.70"
// different from a recommendation of "0.7" is wrong about the only thing it
// was asked to know.
bool sameValue(const SettingDef& def, const std::string& a, const std::string& b) {
    if (a == b) return true;
    if (def.kind != SettingKind::Number) return false;
    double left = 0, right = 0;
    if (!asNumber(a, &left) || !asNumber(b, &right)) return false;
    const double difference = left > right ? left - right : right - left;
    return difference < 1e-9;
}

// The file's text, in the terms the window uses.
std::wstring showValue(const SettingDef& def, const std::string& fileValue) {
    if (!def.percent) return fromUtf8(fileValue);
    double value = 0;
    if (!asNumber(fileValue, &value)) return fromUtf8(fileValue);
    return fromUtf8(tidyNumber(value * 100.0, 1)) + L"%";
}

}  // namespace

std::wstring SettingRow::shown() const {
    return def ? showValue(*def, value) : std::wstring();
}

std::wstring SettingRow::shownRecommended() const {
    return def ? showValue(*def, def->recommended) : std::wstring();
}

bool SettingRow::parseTyped(const std::wstring& typed, std::string* fileValue) const {
    if (!def) return false;
    const std::string text = toUtf8(typed);
    if (def->kind == SettingKind::Text) {
        *fileValue = text;
        return true;
    }
    double value = 0;
    if (!asNumber(text, &value)) return false;
    // "30", "30%" and "0.3" all mean the same thing on a percentage setting.
    // The last of those is what the file already holds, so a value that is
    // already a fraction is left alone rather than turned into 0.003.
    if (def->percent) {
        const bool typedAsFraction = text.find('%') == std::string::npos && value > 0.0 &&
                                     value <= 1.0 && text.find('.') != std::string::npos;
        if (!typedAsFraction) value /= 100.0;
    }
    *fileValue = tidyNumber(value, def->percent ? 4 : (def->precision > 0 ? 4 : 0));
    return true;
}

std::wstring SettingRow::helper() const {
    if (!def || def->kind == SettingKind::Toggle) return std::wstring();
    std::wstring text = L"default ";
    text += *def->shipped ? showValue(*def, def->shipped) : std::wstring(L"(empty)");
    if (*def->lo && *def->hi) {
        text += L"  \x00b7  " + showValue(*def, def->lo) + L" to " + showValue(*def, def->hi);
    }
    return text;
}

const std::vector<SettingDef>& settingDefs() {
    static const std::vector<SettingDef> defs(
        kSettings, kSettings + sizeof(kSettings) / sizeof(kSettings[0]));
    return defs;
}

bool SettingsModel::load(const std::wstring& gameDir) {
    m_gameDir = gameDir;
    m_backedUp = false;
    m_iniPath = joinPath(gameDir, L"edvr.ini");
    m_text = readTextFile(m_iniPath);
    m_loaded = true;
    m_error.clear();
    refreshRows();
    return true;
}

void SettingsModel::refreshRows() {
    m_rows.clear();
    for (const SettingDef& def : settingDefs()) {
        SettingRow row;
        row.def = &def;
        row.choices = splitChoices(def.choices);

        // What the file says, or -- for a setting the user's ini does not carry
        // at all -- what this build ships. An un-migrated file is read the way
        // the runtime reads it: through the moved-from map, ignoring an old
        // line that still carries its retired default (that is an un-updated
        // file, not a choice -- the field saw the window claim splash while
        // the runtime honoured a stale menu_backdrop = stock).
        const std::string dotted = std::string(def.section) + "." + def.key;
        const char* kAbsent = "\x01";
        row.value = iniValue(m_text, dotted, kAbsent);
        if (row.value == kAbsent) {
            row.value = def.shipped;
            for (const auto& mv : kMovedSettings) {
                if (!mv[0][0] || dotted != mv[1]) continue;
                const std::string oldValue = iniValue(m_text, mv[0], kAbsent);
                if (oldValue == kAbsent) continue;
                if (mv[2][0] && oldValue == mv[2]) continue;  // stale default
                row.value = oldValue;
                break;
            }
        }
        row.isRecommended = sameValue(def, row.value, def.recommended);
        row.isShipped = sameValue(def, row.value, def.shipped);
        m_rows.push_back(row);
    }
}

bool SettingsModel::set(size_t index, const std::string& value) {
    if (index >= m_rows.size()) return false;
    const SettingDef& def = *m_rows[index].def;
    const std::string dotted = std::string(def.section) + "." + def.key;

    // Re-read the file first: the install/update path rewrites edvr.ini --
    // migrating renamed keys as it goes -- and a model cached when this
    // window opened would write that pre-update text straight back over the
    // migration, one stale byte at a time. Measured in the field, 2026-08-28:
    // an update at 21:45:22, a settings save at 21:45:39, and the migrated
    // file was gone.
    m_text = readTextFile(m_iniPath);

    // The merge engine, used for one value: merging a document with itself is a
    // no-op (installer_test proves it), so the only change is the forced value
    // -- written into the line where the setting already lives, uncommented if
    // it was an expert default, with the file's layout and every comment
    // untouched. Exactly what a careful hand edit would do.
    std::string source = m_text;
    if (source.empty()) {
        m_error = "edvr.ini is not there yet -- install EDVR into this folder first.";
        return false;
    }
    // One copy of the file as it was when this window opened, before the first
    // change of the session. The install path backs edvr.ini up; changing a
    // setting did not, and a settings screen is exactly where somebody changes
    // several things and then wants the one they had.
    if (!m_backedUp) {
        const std::wstring backupDir =
            joinPath(backupRootPath(m_gameDir), L"settings-" + timestampName());
        if (ensureBackupDir(backupDir)) {
            CopyFileW(m_iniPath.c_str(), joinPath(backupDir, L"edvr.ini").c_str(), FALSE);
        }
        m_backedUp = true;   // tried once; a failure here must not block editing
    }

    MergeReport report;
    const std::string updated = mergeIni(source, source, &source, {{dotted, value}}, &report);

    if (!writeWhole(m_iniPath, updated)) {
        m_error = "Could not write edvr.ini. If the game folder is under Program Files, run the "
                  "installer as administrator.";
        return false;
    }
    m_text = updated;
    m_error.clear();
    refreshRows();
    return true;
}

const std::vector<ResolutionPreset>& vscreenPresets() {
    // 16:9 exactly, 1920x1080 up to the 5120x2880 the ini's own comment
    // names as the headroom step; the window offers these plus Custom.
    static const std::vector<ResolutionPreset> presets = {
        {"1920", "1080"}, {"2560", "1440"}, {"2880", "1620"},
        {"3200", "1800"}, {"3840", "2160"}, {"5120", "2880"},
    };
    return presets;
}

}  // namespace edvr::installer
