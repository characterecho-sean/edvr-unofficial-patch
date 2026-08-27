#include "settings.h"

#include <windows.h>

#include "detect.h"
#include "iniedit.h"

namespace edvr::installer {
namespace {

#include "settings_schema.inc"  // generated: kSettings[]

bool writeWhole(const std::wstring& path, const std::string& text) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const BOOL ok = WriteFile(f, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
    FlushFileBuffers(f);
    CloseHandle(f);
    return ok && written == text.size();
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

std::wstring SettingRow::helper() const {
    if (!def || def->kind == SettingKind::Toggle) return std::wstring();
    std::wstring text = L"default " + fromUtf8(*def->shipped ? def->shipped : "(empty)");
    if (*def->lo && *def->hi) {
        text += L"  \x00b7  " + fromUtf8(def->lo) + L" to " + fromUtf8(def->hi);
    }
    return text;
}

const std::vector<SettingDef>& settingDefs() {
    static const std::vector<SettingDef> defs(
        kSettings, kSettings + sizeof(kSettings) / sizeof(kSettings[0]));
    return defs;
}

bool SettingsModel::load(const std::wstring& gameDir) {
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
        // at all -- what this build ships. Showing the shipped value is right:
        // it is what the game will use.
        const std::string dotted = std::string(def.section) + "." + def.key;
        row.value = iniValue(m_text, dotted, def.shipped);
        row.isRecommended = row.value == def.recommended;
        row.isShipped = row.value == def.shipped;
        m_rows.push_back(row);
    }
}

bool SettingsModel::set(size_t index, const std::string& value) {
    if (index >= m_rows.size()) return false;
    const SettingDef& def = *m_rows[index].def;
    const std::string dotted = std::string(def.section) + "." + def.key;

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

}  // namespace edvr::installer
